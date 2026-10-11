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
/* Kindle WebKit struggles with huge / 8-bit BMPs; keep a modest 24-bit image. */
#define MAX_BMP_EDGE 1024
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

static void write_bmp_copy(const char *dir)
{
    if (!dir || !dir[0] || !have_frame || !bmp_frame || !bmp_length) return;
    char path[768];
    snprintf(path, sizeof(path), "%s/current.bmp", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    (void)fwrite(bmp_frame, 1, bmp_length, f);
    fclose(f);
}

/* Build a 24-bit BMP (BGR, 16-level dithered gray). Kindle's WebKit is unreliable with 8-bit paletted BMPs. */
static int refresh_frame(void)
{
    int src_w = 0, src_h = 0;
    if (airplay_mirror_get_frame(gray_frame, &src_w, &src_h) != 0 ||
        src_w <= 0 || src_h <= 0 ||
        (size_t)src_w > MAX_FRAME_BYTES / (size_t)src_h) {
        return have_frame;
    }

    int w = src_w, h = src_h;
    if (w > MAX_BMP_EDGE || h > MAX_BMP_EDGE) {
        if (w >= h) {
            h = (int)((long)h * MAX_BMP_EDGE / w);
            w = MAX_BMP_EDGE;
        } else {
            w = (int)((long)w * MAX_BMP_EDGE / h);
            h = MAX_BMP_EDGE;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }

    size_t row = ((size_t)w * 3u + 3u) & ~(size_t)3u;
    size_t size = 14u + 40u + row * (size_t)h;
    if (size > bmp_capacity) {
        uint8_t *p = realloc(bmp_frame, size);
        if (!p) return have_frame;
        bmp_frame = p;
        bmp_capacity = size;
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
    memset(bmp_frame, 0, 14u + 40u);
    bmp_frame[0] = 'B'; bmp_frame[1] = 'M';
    put_u32(bmp_frame + 2, (uint32_t)size);
    put_u32(bmp_frame + 10, 14u + 40u);
    put_u32(bmp_frame + 14, 40u);
    put_u32(bmp_frame + 18, (uint32_t)w);
    put_u32(bmp_frame + 22, (uint32_t)h);
    put_u16(bmp_frame + 26, 1);
    put_u16(bmp_frame + 28, 24);
    put_u32(bmp_frame + 34, (uint32_t)(row * (size_t)h));

    uint8_t *pixels = bmp_frame + 54u;
    size_t changed = 0;
    for (int y = 0; y < h; y++) {
        int src_y = (int)((long)y * src_h / h);
        if (src_y >= src_h) src_y = src_h - 1;
        const uint8_t *src_row = gray_frame + (size_t)src_y * (size_t)src_w;
        uint8_t *dst = pixels + (size_t)(h - y - 1) * row;
        uint8_t *prev_row = prev_gray + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int src_x = (int)((long)x * src_w / w);
            if (src_x >= src_w) src_x = src_w - 1;
            int v16 = (int)src_row[src_x] * 240 / 255;   /* 0..240, 1/16 level units */
            uint8_t g = (uint8_t)(((v16 + bayer4[y & 3][x & 3]) >> 4) * 17);
            if (same_dims) {
                int d = (int)g - (int)prev_row[x];
                if (d >= -EINK_HYSTERESIS && d <= EINK_HYSTERESIS) g = prev_row[x];
            }
            if (!same_dims || g != prev_row[x]) changed++;
            prev_row[x] = g;
            dst[x * 3]     = g;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = g;
        }
        if (row > (size_t)w * 3u)
            memset(dst + (size_t)w * 3u, 0, row - (size_t)w * 3u);
    }

    bmp_length = size;
    frame_width = w;
    frame_height = h;
    prev_w = w;
    prev_h = h;
    /* Identical picture: keep the same frame id and skip the file writes. */
    if (same_dims && changed == 0 && have_frame) return 1;
    frame_sequence++;
    have_frame = 1;
    /* file:// WAF can show this without crossing onto http://127.0.0.1 */
    write_bmp_copy(app_dir);
    write_bmp_copy(WAF_DIR);
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
        refresh_frame();
        connected_client = airplay_mirror_is_connected();
        if (!connected_client) {
            have_frame = 0;
            frame_width = 0;
            frame_height = 0;
            prev_w = 0;
            prev_h = 0;
        }
        char json[192];
        int len = snprintf(json, sizeof(json),
            "{\"connected\":%s,\"hasFrame\":%s,\"width\":%d,\"height\":%d,\"frameId\":%lu}",
            connected_client ? "true" : "false", have_frame ? "true" : "false",
            frame_width, frame_height, frame_sequence);
        response(fd, 200, "OK", "application/json", json, (size_t)len);
    } else if (strncmp(path, "/api/frame.bmp", 14) == 0) {
        refresh_frame();
        if (have_frame) response(fd, 200, "OK", "image/bmp", bmp_frame, bmp_length);
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
