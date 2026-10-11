#define _GNU_SOURCE
#include "airplay_mirror.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_PORT 7080
#define MAX_FRAME_BYTES (16u * 1024u * 1024u)
/* Kindle WebKit is unreliable with 8-bit BMPs, so emit 24-bit. The page tells us
 * the box it wants to fill (&w=&h= on /api/status); until then use 1024x1024. */
#define MAX_FIT_EDGE 1600
#define LEVEL_BLACK 24     /* source gray at or below this becomes black   */
#define LEVEL_WHITE 224    /* source gray at or above this becomes white   */
#define WAF_DIR "/var/local/mesquite/com.kindleairplay.app"

static volatile sig_atomic_t stopping;
static int connected_client;
static int have_frame;
static uint8_t *gray_frame;
static uint8_t *bmp_frame;
static size_t bmp_capacity;
static size_t bmp_length;
static int frame_width;
static int frame_height;
static unsigned long frame_sequence;
static int fit_w = 1024, fit_h = 1024;
static int invert_mode;   /* 0 = auto (invert dark UIs), 1 = always, 2 = never */
static int inverted;      /* current auto decision, with hysteresis */

/* Dirty-tile output: the frame is cut into TILE_COLS x TILE_ROWS tiles and only
 * tiles whose pixels changed are rewritten (small BMPs t<N>.bmp) and get a new
 * version number. The page then redraws just those regions, which is much
 * faster on e-ink and leaves unchanged areas alone (no ghosting there). */
#define TILE_COLS 4
#define TILE_ROWS 4
#define TILE_COUNT (TILE_COLS * TILE_ROWS)
static unsigned long tile_ver[TILE_COUNT];
static int tile_w, tile_h;
/* Previous source frame, so an unchanged picture costs one memcmp per poll. */
static uint8_t *src_prev;
static size_t src_prev_cap;
static int src_prev_w, src_prev_h, src_prev_fw, src_prev_fh, src_prev_inv;

/* E-ink friendly output: quantise to the panel's 16 gray levels with a fixed
 * 4x4 ordered dither, and keep the previous output pixel when the new value is
 * within one level (hysteresis). Video noise then stops changing pixels from
 * frame to frame, and an identical picture does not bump frame_sequence, so the
 * page does not reload (= re-flash) a static screen on every poll. */
#define EINK_HYSTERESIS 17
static uint8_t *prev_gray;
static size_t prev_capacity;
static int prev_w, prev_h;
static const uint8_t bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};
static char app_dir[512] = "/mnt/us/documents/illusion-airplay";

static void firewall_rule(const char *verb, const char *chain,
                          const char *protocol, const char *port_flag, int port)
{
    char command[160];
    snprintf(command, sizeof(command),
        "iptables %s %s -p %s %s %d -j ACCEPT 2>/dev/null",
        verb, chain, protocol, port_flag, port);
    (void)system(command);
}

static void firewall_open(void)
{
    firewall_rule("-I", "INPUT", "tcp", "--dport", 7000);
    firewall_rule("-I", "INPUT", "tcp", "--dport", 7100);
    firewall_rule("-I", "INPUT", "udp", "--dport", 5353);
    firewall_rule("-I", "OUTPUT", "tcp", "--sport", 7000);
    firewall_rule("-I", "OUTPUT", "tcp", "--sport", 7100);
    firewall_rule("-I", "OUTPUT", "udp", "--sport", 5353);
}

static void firewall_close(void)
{
    firewall_rule("-D", "INPUT", "tcp", "--dport", 7000);
    firewall_rule("-D", "INPUT", "tcp", "--dport", 7100);
    firewall_rule("-D", "INPUT", "udp", "--dport", 5353);
    firewall_rule("-D", "OUTPUT", "tcp", "--sport", 7000);
    firewall_rule("-D", "OUTPUT", "tcp", "--sport", 7100);
    firewall_rule("-D", "OUTPUT", "udp", "--sport", 5353);
}

static void on_signal(int signo)
{
    (void)signo;
    stopping = 1;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Write one tile of prev_gray as a 24-bit BMP, atomically (tmp + rename). */
static int write_tile(int index, int x0, int y0, int tw, int th, int stride)
{
    size_t row = ((size_t)tw * 3u + 3u) & ~(size_t)3u;
    size_t size = 54u + row * (size_t)th;
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    memset(buf, 0, 54u);
    buf[0] = 'B'; buf[1] = 'M';
    put_u32(buf + 2, (uint32_t)size);
    put_u32(buf + 10, 54u);
    put_u32(buf + 14, 40u);
    put_u32(buf + 18, (uint32_t)tw);
    put_u32(buf + 22, (uint32_t)th);
    put_u16(buf + 26, 1);
    put_u16(buf + 28, 24);
    put_u32(buf + 34, (uint32_t)(row * (size_t)th));
    for (int y = 0; y < th; y++) {
        const uint8_t *src = prev_gray + (size_t)(y0 + y) * (size_t)stride + (size_t)x0;
        uint8_t *dst = buf + 54u + (size_t)(th - y - 1) * row;
        for (int x = 0; x < tw; x++) {
            dst[x * 3]     = src[x];
            dst[x * 3 + 1] = src[x];
            dst[x * 3 + 2] = src[x];
        }
        if (row > (size_t)tw * 3u)
            memset(dst + (size_t)tw * 3u, 0, row - (size_t)tw * 3u);
    }
    char tmp[768], path[768];
    snprintf(tmp, sizeof(tmp), "%s/t%d.tmp", WAF_DIR, index);
    snprintf(path, sizeof(path), "%s/t%d.bmp", WAF_DIR, index);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(buf); return -1; }
    size_t n = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);
    if (n != size) { remove(tmp); return -1; }
    return rename(tmp, path);
}

/* Whole current frame as one BMP (only for /api/frame.bmp, i.e. debugging). */
static int build_full_bmp(void)
{
    if (!have_frame || !prev_gray || prev_w <= 0 || prev_h <= 0) return 0;
    int w = prev_w, h = prev_h;
    size_t row = ((size_t)w * 3u + 3u) & ~(size_t)3u;
    size_t size = 54u + row * (size_t)h;
    if (size > bmp_capacity) {
        uint8_t *p = realloc(bmp_frame, size);
        if (!p) return 0;
        bmp_frame = p;
        bmp_capacity = size;
    }
    memset(bmp_frame, 0, 54u);
    bmp_frame[0] = 'B'; bmp_frame[1] = 'M';
    put_u32(bmp_frame + 2, (uint32_t)size);
    put_u32(bmp_frame + 10, 54u);
    put_u32(bmp_frame + 14, 40u);
    put_u32(bmp_frame + 18, (uint32_t)w);
    put_u32(bmp_frame + 22, (uint32_t)h);
    put_u16(bmp_frame + 26, 1);
    put_u16(bmp_frame + 28, 24);
    put_u32(bmp_frame + 34, (uint32_t)(row * (size_t)h));
    for (int y = 0; y < h; y++) {
        const uint8_t *src = prev_gray + (size_t)y * (size_t)w;
        uint8_t *dst = bmp_frame + 54u + (size_t)(h - y - 1) * row;
        for (int x = 0; x < w; x++) {
            dst[x * 3]     = src[x];
            dst[x * 3 + 1] = src[x];
            dst[x * 3 + 2] = src[x];
        }
        if (row > (size_t)w * 3u)
            memset(dst + (size_t)w * 3u, 0, row - (size_t)w * 3u);
    }
    bmp_length = size;
    return 1;
}

static int refresh_frame(void)
{
    int src_w = 0, src_h = 0;
    if (airplay_mirror_get_frame(gray_frame, &src_w, &src_h) != 0 ||
        src_w <= 0 || src_h <= 0 ||
        (size_t)src_w > MAX_FRAME_BYTES / (size_t)src_h) {
        return have_frame;
    }
    size_t src_bytes = (size_t)src_w * (size_t)src_h;

    /* Same picture and same settings as last time: nothing to do. */
    if (have_frame && src_prev && src_prev_w == src_w && src_prev_h == src_h &&
        src_prev_fw == fit_w && src_prev_fh == fit_h && src_prev_inv == invert_mode &&
        memcmp(src_prev, gray_frame, src_bytes) == 0) {
        return 1;
    }

    /* Scale to fit inside fit_w x fit_h, keeping the aspect ratio (up or down). */
    int w = fit_w;
    int h = (int)((long)src_h * fit_w / src_w);
    if (h > fit_h) {
        h = fit_h;
        w = (int)((long)src_w * fit_h / src_h);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    /* Dark UIs read badly on e-ink (thin light strokes, heavy ghosting), so in
     * auto mode show them inverted: dark text on a light page. */
    int do_invert;
    if (invert_mode == 1) {
        do_invert = 1;
    } else if (invert_mode == 2) {
        do_invert = 0;
    } else {
        unsigned long sum = 0, cnt = 0;
        for (int sy = 0; sy < src_h; sy += 8)
            for (int sx = 0; sx < src_w; sx += 8) {
                sum += gray_frame[(size_t)sy * (size_t)src_w + (size_t)sx];
                cnt++;
            }
        int mean = cnt ? (int)(sum / cnt) : 255;
        if (inverted) { if (mean > 120) inverted = 0; }
        else if (mean < 80) inverted = 1;
        do_invert = inverted;
    }
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (i - LEVEL_BLACK) * 255 / (LEVEL_WHITE - LEVEL_BLACK);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        lut[i] = (uint8_t)(do_invert ? 255 - v : v);
    }

    int same_dims = (prev_gray && prev_w == w && prev_h == h);
    {
        size_t need = (size_t)w * (size_t)h;
        if (need > prev_capacity) {
            uint8_t *np = realloc(prev_gray, need);
            if (!np) return have_frame;
            prev_gray = np;
            prev_capacity = need;
            same_dims = 0;
        }
    }

    int tcw = (w + TILE_COLS - 1) / TILE_COLS;
    int tch = (h + TILE_ROWS - 1) / TILE_ROWS;
    size_t tchg[TILE_COUNT];
    memset(tchg, 0, sizeof(tchg));

    static int xi0[MAX_FIT_EDGE + 4], xi1[MAX_FIT_EDGE + 4], xfr[MAX_FIT_EDGE + 4], xtile[MAX_FIT_EDGE + 4];
    for (int x = 0; x < w; x++) {
        long long sx = ((long long)(2 * x + 1) * src_w * 256) / (2LL * w) - 128;
        if (sx < 0) sx = 0;
        int x0 = (int)(sx >> 8);
        if (x0 > src_w - 1) x0 = src_w - 1;
        xi0[x] = x0;
        xi1[x] = x0 + 1 < src_w ? x0 + 1 : x0;
        xfr[x] = (int)(sx & 255);
        xtile[x] = x / tcw;
    }
    for (int y = 0; y < h; y++) {
        long long sy = ((long long)(2 * y + 1) * src_h * 256) / (2LL * h) - 128;
        if (sy < 0) sy = 0;
        int y0 = (int)(sy >> 8);
        if (y0 > src_h - 1) y0 = src_h - 1;
        int y1 = y0 + 1 < src_h ? y0 + 1 : y0;
        int fy = (int)(sy & 255);
        const uint8_t *r0 = gray_frame + (size_t)y0 * (size_t)src_w;
        const uint8_t *r1 = gray_frame + (size_t)y1 * (size_t)src_w;
        uint8_t *prev_row = prev_gray + (size_t)y * (size_t)w;
        int trow = (y / tch) * TILE_COLS;
        for (int x = 0; x < w; x++) {
            int fx = xfr[x];
            int top = r0[xi0[x]] * (256 - fx) + r0[xi1[x]] * fx;
            int bot = r1[xi0[x]] * (256 - fx) + r1[xi1[x]] * fx;
            int sv = (top * (256 - fy) + bot * fy) >> 16;   /* 0..255 */
            int v16 = (int)lut[sv] * 240 / 255;             /* 0..240, 1/16 level units */
            uint8_t g = (uint8_t)(((v16 + bayer4[y & 3][x & 3]) >> 4) * 17);
            if (same_dims) {
                int d = (int)g - (int)prev_row[x];
                if (d >= -EINK_HYSTERESIS && d <= EINK_HYSTERESIS) g = prev_row[x];
            }
            if (!same_dims || g != prev_row[x]) tchg[trow + xtile[x]]++;
            prev_row[x] = g;
        }
    }

    /* Remember the source so an identical frame is skipped next time. */
    if (src_bytes > src_prev_cap) {
        uint8_t *np = realloc(src_prev, src_bytes);
        if (np) { src_prev = np; src_prev_cap = src_bytes; }
    }
    if (src_prev && src_bytes <= src_prev_cap) {
        memcpy(src_prev, gray_frame, src_bytes);
        src_prev_w = src_w; src_prev_h = src_h;
        src_prev_fw = fit_w; src_prev_fh = fit_h; src_prev_inv = invert_mode;
    } else {
        src_prev_w = 0;
    }

    frame_width = w;
    frame_height = h;
    prev_w = w;
    prev_h = h;
    tile_w = tcw;
    tile_h = tch;

    int any = 0;
    for (int t = 0; t < TILE_COUNT; t++) {
        if (!tchg[t]) continue;
        int x0 = (t % TILE_COLS) * tcw, y0 = (t / TILE_COLS) * tch;
        if (x0 >= w || y0 >= h) continue;
        int tw = tcw; if (x0 + tw > w) tw = w - x0;
        int th = tch; if (y0 + th > h) th = h - y0;
        if (write_tile(t, x0, y0, tw, th, w) == 0) {
            tile_ver[t]++;
            any = 1;
        } else {
            prev_w = 0;   /* couldn't write: force a full redraw on the next pass */
            src_prev_w = 0;
        }
    }
    if (same_dims && !any && have_frame) return 1;
    frame_sequence++;
    have_frame = 1;
    return 1;
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static void response(int fd, int code, const char *reason, const char *type,
                     const void *body, size_t len)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, reason, type, (unsigned long)len);
    if (n > 0 && (size_t)n < sizeof(header)) {
        send_all(fd, header, (size_t)n);
        if (body && len) send_all(fd, body, len);
    }
}

static void serve_static(int fd, const char *file, const char *type)
{
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", app_dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        response(fd, 404, "Not Found", "text/plain", "Not found", 9);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long length = ftell(f);
    if (length < 0 || length > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        response(fd, 500, "Internal Server Error", "text/plain", "Read error", 10);
        return;
    }
    char *data = malloc((size_t)length);
    if (!data) { fclose(f); return; }
    size_t n = fread(data, 1, (size_t)length, f);
    fclose(f);
    response(fd, 200, "OK", type, data, n);
    free(data);
}

static int query_int(const char *path, const char *key, int fallback)
{
    const char *p = strstr(path, key);
    return p ? atoi(p + strlen(key)) : fallback;
}

static void apply_query(const char *path)
{
    int qw = query_int(path, "&w=", 0), qh = query_int(path, "&h=", 0);
    if (qw >= 200 && qh >= 200) {
        fit_w = qw > MAX_FIT_EDGE ? MAX_FIT_EDGE : qw;
        fit_h = qh > MAX_FIT_EDGE ? MAX_FIT_EDGE : qh;
    }
    int qi = query_int(path, "&inv=", -1);
    if (qi >= 0 && qi <= 2) invert_mode = qi;
}

static void serve_request(int fd)
{
    char request[2048];
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) return;
    request[n] = '\0';
    char method[8] = {0}, path[256] = {0};
    if (sscanf(request, "%7s %255s", method, path) != 2 || strcmp(method, "GET") != 0) {
        response(fd, 405, "Method Not Allowed", "text/plain", "GET only", 8);
        return;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        serve_static(fd, "index.html", "text/html; charset=utf-8");
    } else if (strcmp(path, "/style.css") == 0) {
        serve_static(fd, "style.css", "text/css; charset=utf-8");
    } else if (strcmp(path, "/app.js") == 0) {
        serve_static(fd, "app.js", "application/javascript; charset=utf-8");
    } else if (strncmp(path, "/api/status", 11) == 0) {
        apply_query(path);
        refresh_frame();
        connected_client = airplay_mirror_is_connected();
        if (!connected_client) {
            have_frame = 0;
            frame_width = 0;
            frame_height = 0;
            prev_w = 0;
            prev_h = 0;
            tile_w = 0;
            tile_h = 0;
        }
        char json[640];
        int len = snprintf(json, sizeof(json),
            "{\"connected\":%s,\"hasFrame\":%s,\"width\":%d,\"height\":%d,\"frameId\":%lu,"
            "\"cols\":%d,\"rows\":%d,\"tw\":%d,\"th\":%d,\"tiles\":[",
            connected_client ? "true" : "false", have_frame ? "true" : "false",
            frame_width, frame_height, frame_sequence, TILE_COLS, TILE_ROWS, tile_w, tile_h);
        for (int t = 0; t < TILE_COUNT && len < (int)sizeof(json) - 32; t++)
            len += snprintf(json + len, sizeof(json) - (size_t)len, "%s%lu", t ? "," : "", tile_ver[t]);
        len += snprintf(json + len, sizeof(json) - (size_t)len, "]}");
        response(fd, 200, "OK", "application/json", json, (size_t)len);
    } else if (strncmp(path, "/api/frame.bmp", 14) == 0) {
        refresh_frame();
        if (build_full_bmp()) response(fd, 200, "OK", "image/bmp", bmp_frame, bmp_length);
        else response(fd, 503, "No Frame", "text/plain", "Waiting for video", 17);
    } else if (strncmp(path, "/api/stop", 9) == 0) {
        response(fd, 200, "OK", "application/json", "{\"stopping\":true}", 17);
        stopping = 1;
    } else {
        response(fd, 404, "Not Found", "text/plain", "Not found", 9);
    }
}

static int run_http_server(void)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return -1;
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WEB_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 8) < 0) {
        close(server);
        return -1;
    }
    while (!stopping) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (stopping) break;
            continue;
        }
        struct timeval timeout = { 3, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        serve_request(client);
        close(client);
    }
    close(server);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strlen(argv[1]) < sizeof(app_dir))
        snprintf(app_dir, sizeof(app_dir), "%s", argv[1]);
    gray_frame = malloc(MAX_FRAME_BYTES);
    if (!gray_frame) return 1;

    /* No SA_RESTART: let accept() return EINTR so SIGTERM actually stops us. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    airplay_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_name = "Kindle AirPlay";
    cfg.http_port = 7000;
    cfg.video_port = 7100;
    cfg.key_path = "/mnt/us/documents/illusion-airplay/airplay_key.pem";
    firewall_open();
    if (airplay_mirror_start(&cfg) != 0) {
        firewall_close();
        free(gray_frame);
        return 2;
    }
    int rc = run_http_server();
    airplay_mirror_stop();
    firewall_close();
    free(gray_frame);
    free(bmp_frame);
    return rc == 0 ? 0 : 3;
}
