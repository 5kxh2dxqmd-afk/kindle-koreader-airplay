#define _GNU_SOURCE
/*
 * airplay_wrapper.c — thin wrapper around UxPlay's raop lib.
 *
 * UxPlay handles: HTTP/RTSP server, FairPlay, pairing, AES-CTR decryption,
 *                 H.264 NAL extraction → delivers raw NALs via video_process cb.
 * We handle:      H.264 decode (ffmpeg), gray8 conversion, frame buffer,
 *                 mDNS (our own UDP multicast implementation).
 */

#include "airplay_mirror.h"
#include "raop.h"
#include "dnssd.h"
#include "logger.h"
#include "llhttp/llhttp.h"

/* Intercept llhttp_init (via -Wl,--wrap,llhttp_init) to enable lenient parsing.
 * AirPlay uses RTSP/1.0 request lines and non-standard header tokens that
 * strict HTTP/1.x parsing rejects with HPE_INVALID_HEADER_TOKEN. */
void __wrap_llhttp_init(llhttp_t *parser, llhttp_type_t type,
                        const llhttp_settings_t *settings)
{
    extern void __real_llhttp_init(llhttp_t *, llhttp_type_t,
                                   const llhttp_settings_t *);
    __real_llhttp_init(parser, type, settings);
    llhttp_set_lenient_headers(parser, 1);
    llhttp_set_lenient_version(parser, 1);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* Kindle EPDC v2 mxcfb structures (Paperwhite 3+, Oasis, Voyage).
 * 72-byte struct → MXCFB_SEND_UPDATE = 0x4048462E */
struct mxcfb_rect { uint32_t top, left, width, height; };
struct mxcfb_alt_buffer_data {
    uint32_t phys_addr, width, height;
    struct mxcfb_rect alt_update_region;
};
struct mxcfb_update_data {
    struct mxcfb_rect            update_region;
    uint32_t                     waveform_mode;
    uint32_t                     update_mode;
    uint32_t                     update_marker;
    int32_t                      temp;
    uint32_t                     flags;
    int32_t                      dither_mode;
    int32_t                      quant_bit;
    struct mxcfb_alt_buffer_data alt_buffer_data;
};
#define MXCFB_SEND_UPDATE   _IOW('F', 0x2E, struct mxcfb_update_data)
#define WAVEFORM_MODE_GC16  2
#define WAVEFORM_MODE_A2    6
#define UPDATE_MODE_PARTIAL 0
#define UPDATE_MODE_FULL    1
#define TEMP_USE_AUTO       0x1000

#include <sys/time.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* Decode only when a render is due — saves ~98% of ffmpeg CPU at 0.5 FPS */
#define DECODE_INTERVAL 1.8  /* seconds; slightly under POLL_INTERVAL_MS=2000 */
static double g_next_decode = 0.0;

static double mono_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec * 1e-6;
}

/* ─── mDNS (our own, no Bonjour on Kindle) ──────────────────────── */

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define MDNS_TTL  120

static volatile int   mdns_running = 0;
static int            mdns_started = 0;
static int            mdns_fd      = -1;
static pthread_t      mdns_tid;

static void b64_encode(const uint8_t *in, int inlen, char *out)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, j = 0;
    while (i < inlen) {
        uint32_t v = (uint32_t)in[i++] << 16;
        if (i < inlen) v |= (uint32_t)in[i++] << 8;
        if (i < inlen) v |= in[i++];
        out[j++] = t[(v >> 18) & 63];
        out[j++] = t[(v >> 12) & 63];
        out[j++] = (i > inlen + 0) ? '=' : t[(v >> 6) & 63];
        out[j++] = (i > inlen + 1) ? '=' : t[v & 63];
    }
    out[j] = '\0';
}

static in_addr_t get_wifi_addr(void)
{
    struct ifaddrs *ifa_list, *ifa;
    if (getifaddrs(&ifa_list) < 0) return INADDR_ANY;
    in_addr_t result = INADDR_ANY;
    for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (sin->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
        result = sin->sin_addr.s_addr;
        break;
    }
    freeifaddrs(ifa_list);
    return result;
}

static int build_mdns_response(uint8_t *out, int maxlen,
                               const char *device_name, int port,
                               in_addr_t local_ip, const char *pk_b64)
{
    uint8_t *p = out, *end = out + maxlen;
#define WU16(v) do { *p++ = ((v)>>8)&0xff; *p++ = (v)&0xff; } while(0)
#define WU32(v) do { WU16((v)>>16); WU16((v)&0xffff); } while(0)
#define WNAME(s) do { \
    const char *_s = (s); \
    while (*_s) { \
        const char *_d = strchr(_s, '.'); \
        int _l = _d ? (int)(_d-_s) : (int)strlen(_s); \
        *p++ = (uint8_t)_l; memcpy(p,_s,_l); p+=_l; \
        if (!_d) break; else _s=_d+1; } \
    *p++ = 0; } while(0)

    WU16(0x0000); WU16(0x8400);
    WU16(0); WU16(3); WU16(0); WU16(1);

    char full[256];
    snprintf(full, sizeof(full), "%s._airplay._tcp.local", device_name);

    /* PTR */
    WNAME("_airplay._tcp.local");
    WU16(12); WU16(0x8001); WU32(MDNS_TTL);
    uint8_t *rdp = p; p += 2; uint8_t *rds = p;
    WNAME(full); uint16_t rdl = (uint16_t)(p-rds);
    rdp[0]=rdl>>8; rdp[1]=rdl&0xff;

    /* SRV */
    WNAME(full);
    WU16(33); WU16(0x8001); WU32(MDNS_TTL);
    rdp = p; p += 2; rds = p;
    WU16(0); WU16(0); WU16(port); WNAME("kindle.local");
    rdl = (uint16_t)(p-rds); rdp[0]=rdl>>8; rdp[1]=rdl&0xff;

    /* TXT */
    WNAME(full);
    WU16(16); WU16(0x8001); WU32(MDNS_TTL);
    rdp = p; p += 2; rds = p;
    char pk_txt[200]; snprintf(pk_txt, sizeof(pk_txt), "pk=%s", pk_b64 ? pk_b64 : "");
    const char *txts[] = {
        "deviceid=AA:BB:CC:DD:EE:FF", "features=0x5A7FFFF7,0x1E",
        "flags=0x4", "model=AppleTV3,2", pk_txt,
        "srcvers=220.68", "statusFlags=68", "vv=2", NULL };
    for (int i = 0; txts[i]; i++) {
        uint8_t tl = (uint8_t)strlen(txts[i]); *p++ = tl; memcpy(p,txts[i],tl); p+=tl;
    }
    rdl = (uint16_t)(p-rds); rdp[0]=rdl>>8; rdp[1]=rdl&0xff;

    /* A record */
    WNAME("kindle.local");
    WU16(1); WU16(0x8001); WU32(MDNS_TTL); WU16(4);
    *p++ = (local_ip)&0xff; *p++ = (local_ip>>8)&0xff;
    *p++ = (local_ip>>16)&0xff; *p++ = (local_ip>>24)&0xff;

#undef WU16
#undef WU32
#undef WNAME
    return (p < end) ? (int)(p - out) : -1;
}

typedef struct { const char *name; int port; char pk_b64[64]; } mdns_args_t;

static void *mdns_thread_fn(void *arg)
{
    mdns_args_t *a = (mdns_args_t *)arg;
    uint8_t resp[2048];
    in_addr_t ip = get_wifi_addr();
    int rlen = build_mdns_response(resp, sizeof(resp), a->name, a->port, ip, a->pk_b64);
    if (rlen < 0) { free(a); return NULL; }

    struct sockaddr_in mcast = {
        .sin_family = AF_INET,
        .sin_port   = htons(MDNS_PORT),
        .sin_addr.s_addr = inet_addr(MDNS_ADDR)
    };
    for (int i = 0; i < 3 && mdns_running; i++) {
        sendto(mdns_fd, resp, rlen, 0, (struct sockaddr *)&mcast, sizeof(mcast));
        usleep(250000);
    }
    time_t last = time(NULL);
    uint8_t qbuf[2048];
    while (mdns_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(mdns_fd, &fds);
        struct timeval tv = {.tv_sec=1};
        if (select(mdns_fd+1,&fds,NULL,NULL,&tv) > 0 && FD_ISSET(mdns_fd,&fds)) {
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            int ql = recvfrom(mdns_fd, qbuf, sizeof(qbuf), 0,
                              (struct sockaddr *)&from, &fl);
            if (ql > 4 && !(qbuf[2]&0x80) && memmem(qbuf,ql,"_airplay",8))
                sendto(mdns_fd, resp, rlen, 0, (struct sockaddr *)&mcast, sizeof(mcast));
        }
        time_t now = time(NULL);
        if (now - last >= 5) {
            sendto(mdns_fd, resp, rlen, 0, (struct sockaddr *)&mcast, sizeof(mcast));
            last = now;
        }
    }
    free(a);
    return NULL;
}

/* ─── ffmpeg decode ─────────────────────────────────────────────── */

static AVCodecContext  *av_ctx   = NULL;
static AVFrame         *av_frame = NULL;
static struct SwsContext *sws    = NULL;

static int decoder_init(void)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return -1;
    av_ctx = avcodec_alloc_context3(codec);
    if (!av_ctx) return -1;
    av_ctx->thread_count = 1;          /* single-thread: Kindle is ARM single/dual core */
    if (avcodec_open2(av_ctx, codec, NULL) < 0) return -1;
    av_frame = av_frame_alloc();
    return av_frame ? 0 : -1;
}

static void decoder_free(void)
{
    if (sws)      { sws_freeContext(sws); sws = NULL; }
    if (av_frame) { av_frame_free(&av_frame); }
    if (av_ctx)   { avcodec_free_context(&av_ctx); }
}

/* ─── global state ──────────────────────────────────────────────── */

static struct {
    airplay_config_t cfg;
    raop_t          *raop;
    dnssd_t         *dnssd;

    pthread_mutex_t  frame_mutex;
    uint8_t         *frame_buf;
    int              frame_w, frame_h, frame_ready;
} g;

/* ─── Direct framebuffer state ──────────────────────────────────── */

static int    g_fb_fd     = -1;
static void  *g_fb_mmap   = NULL;
static int    g_fb_w      = 0;
static int    g_fb_h      = 0;
static int    g_fb_stride = 0;
static size_t g_fb_size   = 0;
static int    g_fb_bpp    = 1;

/* Last-rendered frame identity for dedup */
static uint32_t g_last_hash = 0;
static int      g_last_fw   = 0;
static int      g_last_fh   = 0;

/* Last-rendered framebuffer for change-detection */
static uint8_t *g_last_fb  = NULL;
static int      g_last_fb_w = 0;
static int      g_last_fb_h = 0;

/* Periodic forced refresh to clear ghosting on static/hardware-cursor screens */
#define GHOST_CLEAR_INTERVAL 30   /* seconds */
static time_t g_last_forced_refresh = 0;


static FILE *g_log;
static void dbg(const char *fmt, ...) {
    if (!g_log) g_log = fopen("/mnt/us/koreader/plugins/airplay.koplugin/airplay.log", "a");
    if (!g_log) return;
    va_list ap; va_start(ap,fmt); vfprintf(g_log,fmt,ap); va_end(ap);
    fputc('\n',g_log); fflush(g_log);
}

/* ─── video_process callback ───────────────────────────────────── */

static uint64_t g_pkt_count = 0;

static void cb_video_process(void *cls, raop_ntp_t *ntp,
                             video_decode_struct *data)
{
    (void)cls; (void)ntp;
    if (!data || !data->data || data->data_len <= 0) return;

    g_pkt_count++;
    if (g_pkt_count == 1 || g_pkt_count % 300 == 0)
        dbg("video_process: packets_seen=%llu last_len=%d",
            (unsigned long long)g_pkt_count, data->data_len);

    /* Skip decode entirely if render window hasn't arrived yet.
     * Saves ~98% of ffmpeg CPU at 0.5 FPS vs 30 FPS stream. */
    double now = mono_sec();
    if (now < g_next_decode) return;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return;
    pkt->data = data->data;
    pkt->size = data->data_len;

    int ret = avcodec_send_packet(av_ctx, pkt);
    av_packet_free(&pkt);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        dbg("video_process: avcodec_send_packet failed ret=%d (%s)", ret, errbuf);
        return;
    }

    int got_frame = 0;
    while (avcodec_receive_frame(av_ctx, av_frame) == 0) {
        got_frame = 1;
        int w = av_frame->width, h = av_frame->height;
        if (w <= 0 || h <= 0) continue;

        if (!sws || av_ctx->width != w || av_ctx->height != h) {
            if (sws) sws_freeContext(sws);
            /* SWS_FAST_BILINEAR: faster than SWS_BILINEAR, fine for e-ink */
            sws = sws_getContext(w, h, av_frame->format,
                                 w, h, AV_PIX_FMT_GRAY8,
                                 SWS_FAST_BILINEAR, NULL, NULL, NULL);
        }
        if (!sws) continue;

        uint8_t *gray = malloc(w * h);
        if (!gray) continue;

        uint8_t *dst[4]   = { gray, NULL, NULL, NULL };
        int      dstst[4] = { w, 0, 0, 0 };
        sws_scale(sws, (const uint8_t *const *)av_frame->data,
                  av_frame->linesize, 0, h, dst, dstst);

        pthread_mutex_lock(&g.frame_mutex);
        free(g.frame_buf);
        g.frame_buf   = gray;
        g.frame_w     = w;
        g.frame_h     = h;
        g.frame_ready = 1;
        pthread_mutex_unlock(&g.frame_mutex);

        dbg("video_process: decoded frame %dx%d", w, h);

        /* Gate: next decode allowed after DECODE_INTERVAL seconds */
        g_next_decode = mono_sec() + DECODE_INTERVAL;
        break;  /* one frame per window is enough */
    }
    if (!got_frame)
        dbg("video_process: send_packet OK but no frame produced yet (len=%d)", data->data_len);
}

static void cb_raop_log(void *cls, int level, const char *msg)
{ (void)cls; dbg("raop[%d]: %s", level, msg); }

/* no-op stubs for required callbacks */
static void cb_audio_process(void *cls, raop_ntp_t *ntp, audio_decode_struct *d)
{ (void)cls;(void)ntp;(void)d; }
static void cb_conn_init(void *cls)   { (void)cls; dbg("raop: conn_init"); }
static void cb_conn_destroy(void *cls){ (void)cls; dbg("raop: conn_destroy"); }
static void cb_conn_reset(void *cls, int r){ (void)cls; dbg("raop: conn_reset %d",r); }
static void cb_conn_teardown(void *cls, bool *td96, bool *td110)
{ (void)cls; if(td96)*td96=true; if(td110)*td110=true; }
static void cb_audio_flush(void *cls) { (void)cls; }
static void cb_video_flush(void *cls) { (void)cls; }
static void cb_video_pause(void *cls) { (void)cls; }
static void cb_video_resume(void *cls){ (void)cls; }
static void cb_conn_feedback(void *cls){ (void)cls; }
static void cb_video_reset(void *cls, reset_type_t r){ (void)cls;(void)r; }
static double cb_audio_volume(void *cls){ (void)cls; return 0.0; }
static int cb_video_set_codec(void *cls, video_codec_t codec)
{ (void)cls; dbg("raop: video_set_codec codec=%d", (int)codec); return 0; }

/* ─── Public API ────────────────────────────────────────────────── */

int airplay_mirror_start(const airplay_config_t *cfg)
{
    if (!cfg) return -1;
    dbg("airplay_mirror_start: enter built=" __DATE__ " " __TIME__);
    memset(&g, 0, sizeof(g));
    g.cfg = *cfg;
    pthread_mutex_init(&g.frame_mutex, NULL);

    dbg("step: decoder_init");
    if (decoder_init() < 0) { dbg("decoder_init failed"); return -1; }
    dbg("step: decoder_init OK");

    /* hardware address (use fixed for now) */
    static const unsigned char hw[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    const char *name = cfg->device_name ? cfg->device_name : "KindleAirPlay";
    int err = 0;
    dbg("step: dnssd_init name=%s", name);
    g.dnssd = dnssd_init(name, (int)strlen(name), (const char *)hw, 6, 0, &err);
    if (!g.dnssd) { dbg("dnssd_init failed err=%d",err); return -1; }
    dbg("step: dnssd_init OK");

    raop_callbacks_t cbs = {0};
    cbs.cls                = NULL;
    cbs.audio_process      = cb_audio_process;
    cbs.video_process      = cb_video_process;
    cbs.conn_init          = cb_conn_init;
    cbs.conn_destroy       = cb_conn_destroy;
    cbs.conn_reset         = cb_conn_reset;
    cbs.conn_teardown      = cb_conn_teardown;
    cbs.audio_flush        = cb_audio_flush;
    cbs.video_flush        = cb_video_flush;
    cbs.video_pause        = cb_video_pause;
    cbs.video_resume       = cb_video_resume;
    cbs.conn_feedback      = cb_conn_feedback;
    cbs.video_reset        = cb_video_reset;
    cbs.audio_set_client_volume = cb_audio_volume;
    cbs.video_set_codec         = cb_video_set_codec;

    dbg("step: raop_init");
    g.raop = raop_init(&cbs);
    if (!g.raop) { dbg("raop_init failed"); return -1; }
    dbg("step: raop_init OK");

    raop_set_log_callback(g.raop, cb_raop_log, NULL);
    raop_set_log_level(g.raop, LOGGER_DEBUG);

    /* raop_init2: nohold=0 (single client), device_id, keyfile persists Ed25519 key */
    char device_id[18] = "AA:BB:CC:DD:EE:FF";
    dbg("step: raop_init2");
    if (raop_init2(g.raop, 0, device_id,
                   "/mnt/us/koreader/plugins/airplay.koplugin/airplay_key.pem") < 0) {
        dbg("raop_init2 failed"); raop_destroy(g.raop); g.raop=NULL; return -1;
    }
    dbg("step: raop_init2 OK");

    /* tell raop about display for GET /info response */
    raop_set_plist(g.raop, "width",  640);
    raop_set_plist(g.raop, "height", 480);
    /* macOS refuses to send H264 if maxFPS<threshold or refreshRate too low.
     * We advertise 30fps and let the Lua poll loop drop frames at 1fps. */
    raop_set_plist(g.raop, "maxFPS", 30);
    raop_set_plist(g.raop, "refreshRate", 30);
    raop_set_plist(g.raop, "clientFPSdata", 1); /* log 0x05 plist content */

    unsigned short port = (unsigned short)(cfg->http_port > 0 ? cfg->http_port : 7000);
    raop_set_port(g.raop, port);
    /* pin mirror TCP data port so iptables rule for 7100 covers it */
    unsigned short tcp[2] = {
        (unsigned short)(cfg->video_port > 0 ? cfg->video_port : 7100),
        0
    };
    raop_set_tcp_ports(g.raop, tcp);
    dbg("step: raop_set_dnssd");
    raop_set_dnssd(g.raop, g.dnssd);  /* also sets pk into dnssd */
    dbg("step: raop_set_dnssd OK");

    unsigned short actual = port;
    dbg("step: raop_start_httpd port=%d", (int)port);
    if (raop_start_httpd(g.raop, &actual) < 0) {
        dbg("raop_start_httpd failed"); raop_destroy(g.raop); g.raop=NULL; return -1;
    }
    dbg("raop httpd started on port %d", (int)actual);

    /* mDNS: get pk from dnssd (raop_set_dnssd populated it) */
    char pk_b64[128] = "";
    /* raop stores pk_str internally; dnssd_stub has it after raop_set_dnssd */
    /* build pk_b64 from dnssd (dnssd_get_airplay_txt contains pk= entry) */
    {
        int tlen = 0;
        const char *txt = dnssd_get_airplay_txt(g.dnssd, &tlen);
        const unsigned char *p = (const unsigned char *)txt;
        while (p < (const unsigned char *)txt + tlen) {
            uint8_t l = *p++;
            if (l > 3 && memcmp(p, "pk=", 3) == 0) {
                int vl = l - 3;
                if (vl >= (int)sizeof(pk_b64)) vl = sizeof(pk_b64)-1;
                memcpy(pk_b64, p+3, vl); pk_b64[vl] = '\0';
                break;
            }
            p += l;
        }
    }

    /* mDNS socket */
    mdns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mdns_fd >= 0) {
        int on = 1;
        setsockopt(mdns_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        uint8_t ttl = MDNS_TTL;
        setsockopt(mdns_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        struct sockaddr_in bind_addr = {
            .sin_family = AF_INET, .sin_port = htons(MDNS_PORT),
            .sin_addr.s_addr = INADDR_ANY
        };
        bind(mdns_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
        struct ip_mreq mreq = {
            .imr_multiaddr.s_addr = inet_addr(MDNS_ADDR),
            .imr_interface.s_addr = INADDR_ANY
        };
        setsockopt(mdns_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        mdns_args_t *a = malloc(sizeof(*a));
        a->name = name; a->port = (int)actual;
        memcpy(a->pk_b64, pk_b64, sizeof(a->pk_b64));
        mdns_running = 1;
        mdns_started = 1;
        pthread_create(&mdns_tid, NULL, mdns_thread_fn, a);
    }

    dbg("airplay_mirror_start OK port=%d", (int)actual);
    return 0;
}

int airplay_mirror_render_to_fb(uint8_t *fb, int fb_w, int fb_h, int fb_stride)
{
    int ret = -1;
    pthread_mutex_lock(&g.frame_mutex);
    if (!g.frame_ready || !g.frame_buf) {
        pthread_mutex_unlock(&g.frame_mutex);
        return -1;
    }

    int sw = g.frame_w, sh = g.frame_h;
    uint8_t *src = g.frame_buf;

    /* Nearest-neighbor scale, letterbox/pillarbox centre */
    float scale = (float)fb_w / sw;
    if ((float)fb_h / sh < scale) scale = (float)fb_h / sh;
    int dw = (int)(sw * scale);
    int dh = (int)(sh * scale);
    int ox = (fb_w - dw) / 2;
    int oy = (fb_h - dh) / 2;

    /* Fill white */
    for (int y = 0; y < fb_h; y++)
        memset(fb + y * fb_stride, 0xFF, fb_stride);

    /* Scale region */
    for (int y = 0; y < dh; y++) {
        int sy = (int)((y * sh) / dh);
        uint8_t *dst_row = fb + (oy + y) * fb_stride + ox;
        uint8_t *src_row = src + sy * sw;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((x * sw) / dw);
            dst_row[x] = src_row[sx];
        }
    }

    g.frame_ready = 0;
    ret = 0;
    pthread_mutex_unlock(&g.frame_mutex);
    return ret;
}

static int fb_init(void)
{
    if (g_fb_fd >= 0) return 0;

    g_fb_fd = open("/dev/fb0", O_RDWR);
    if (g_fb_fd < 0) { dbg("fb_init: open failed: %s", strerror(errno)); return -1; }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    int vrc, frc;
    do { vrc = ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo); } while (vrc < 0 && errno == EINTR);
    do { frc = ioctl(g_fb_fd, FBIOGET_FSCREENINFO, &finfo); } while (frc < 0 && errno == EINTR);
    if (vrc < 0 || frc < 0) {
        dbg("fb_init: ioctl failed: %s", strerror(errno));
        close(g_fb_fd); g_fb_fd = -1; return -1;
    }

    g_fb_w      = (int)vinfo.xres;
    g_fb_h      = (int)vinfo.yres;
    g_fb_bpp    = (int)((vinfo.bits_per_pixel + 7) / 8);
    g_fb_stride = (int)finfo.line_length;
    g_fb_size   = (size_t)g_fb_stride * (size_t)g_fb_h;

    g_fb_mmap = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
    if (g_fb_mmap == MAP_FAILED) {
        dbg("fb_init: mmap failed: %s", strerror(errno));
        close(g_fb_fd); g_fb_fd = -1; g_fb_mmap = NULL; return -1;
    }

    /* Allocate last-rendered fb copy for change detection */
    if (g_last_fb) free(g_last_fb);
    g_last_fb = malloc(g_fb_size);
    if (!g_last_fb) {
        dbg("fb_init: last_fb malloc failed");
    } else {
        memset(g_last_fb, 0xFF, g_fb_size);  /* white = "nothing rendered yet" */
    }
    g_last_fb_w = g_fb_w;
    g_last_fb_h = g_fb_h;
    dbg("fb_init: %dx%d stride=%d bpp=%d", g_fb_w, g_fb_h, g_fb_stride, g_fb_bpp);
    return 0;
}

static void fb_cleanup(void)
{
    if (g_fb_mmap && g_fb_mmap != MAP_FAILED) { munmap(g_fb_mmap, g_fb_size); g_fb_mmap = NULL; }
    if (g_fb_fd >= 0) { close(g_fb_fd); g_fb_fd = -1; }
    g_fb_w = g_fb_h = g_fb_stride = g_fb_bpp = 0; g_fb_size = 0;
    g_last_hash = 0; g_last_fw = 0; g_last_fh = 0;
    if (g_last_fb) { free(g_last_fb); g_last_fb = NULL; }
    g_last_fb_w = g_last_fb_h = 0;
    g_last_forced_refresh = 0;
    g_next_decode = 0.0;  /* decode immediately on next start */
}

int airplay_mirror_render_direct(void)
{
    if (fb_init() < 0) return -1;

    pthread_mutex_lock(&g.frame_mutex);
    if (!g.frame_ready || !g.frame_buf) {
        pthread_mutex_unlock(&g.frame_mutex); return -1;
    }

    int sw = g.frame_w, sh = g.frame_h;
    uint8_t *src = g.frame_buf;

    /* FNV-1a content hash (every 16th byte) — skip ioctl if frame unchanged */
    uint32_t hash = 2166136261u;
    int npx = sw * sh;
    for (int i = 0; i < npx; i += 16)
        hash = (hash ^ src[i]) * 16777619u;

    if (hash == g_last_hash && sw == g_last_fw && sh == g_last_fh) {
        g.frame_ready = 0;
        pthread_mutex_unlock(&g.frame_mutex);
        dbg("render_direct: skip (hash unchanged) %dx%d hash=0x%08x", sw, sh, hash);
        return 1;  /* decoded but content unchanged — skip refresh */
    }
    g_last_hash = hash;
    g_last_fw   = sw;
    g_last_fh   = sh;

    uint8_t *fb = (uint8_t *)g_fb_mmap;

    /* Auto-rotate 90° CW when source is landscape and fb is portrait.
     * CW formula: Output(rx,ry) = Input(x=ry, y=sh-1-rx)
     * User tilts Kindle 90° CCW (left-side up) to view correctly. */
    int rotate = (sw > sh) && (g_fb_w < g_fb_h);
    int eff_w  = rotate ? sh : sw;   /* logical width after rotation */
    int eff_h  = rotate ? sw : sh;   /* logical height after rotation */

    float scale = (float)g_fb_w / eff_w;
    if ((float)g_fb_h / eff_h < scale) scale = (float)g_fb_h / eff_h;
    int dw = (int)(eff_w * scale);
    int dh = (int)(eff_h * scale);
    int ox = (g_fb_w - dw) / 2;
    int oy = (g_fb_h - dh) / 2;

    dbg("render_direct: geometry sw=%d sh=%d rotate=%d eff_w=%d eff_h=%d dw=%d dh=%d ox=%d oy=%d fb=%dx%d",
        sw, sh, rotate, eff_w, eff_h, dw, dh, ox, oy, g_fb_w, g_fb_h);

    /* Clear fb to white */
    for (int y = 0; y < g_fb_h; y++)
        memset(fb + (size_t)y * g_fb_stride, 0xFF, (size_t)g_fb_stride);

    /* Scale (+ optional rotate) into fb */
    for (int y = 0; y < dh; y++) {
        int ry = y * eff_h / dh;
        uint8_t *dst_row = fb + (size_t)(oy + y) * g_fb_stride + (size_t)ox * g_fb_bpp;
        for (int x = 0; x < dw; x++) {
            int rx = x * eff_w / dw;
            int src_x, src_y;
            if (rotate) {
                src_x = ry;          /* CW: input x = rotated y */
                src_y = sh - 1 - rx; /* CW: input y = sh-1 - rotated x */
            } else {
                src_x = rx;
                src_y = ry;
            }
            uint8_t val = src[src_y * sw + src_x];
            for (int b = 0; b < g_fb_bpp; b++)
                dst_row[x * g_fb_bpp + b] = val;
        }
    }

    g.frame_ready = 0;
    pthread_mutex_unlock(&g.frame_mutex);

    /* Skip ioctl if content identical to last render.
     * Exclude top letterbox + Mac status bar rows from comparison so clock/wifi
     * ticks don't trigger a full refresh.  Status bar ≈ top 4% of content height. */
    int have_baseline = (g_last_fb && g_last_fb_w == g_fb_w && g_last_fb_h == g_fb_h);
    if (have_baseline) {
        int skip_rows = oy + dh / 25;  /* letterbox top + ~4% of content */
        int changed = 0;
        for (int y = skip_rows; y < g_fb_h && !changed; y++) {
            const uint8_t *row_fb   = fb        + (size_t)y * g_fb_stride;
            const uint8_t *row_last = g_last_fb + (size_t)y * g_fb_stride;
            if (memcmp(row_fb, row_last, (size_t)(g_fb_w * g_fb_bpp)) != 0)
                changed = 1;
        }
        if (!changed) {
            time_t now = time(NULL);
            if ((now - g_last_forced_refresh) < GHOST_CLEAR_INTERVAL) {
                memcpy(g_last_fb, fb, g_fb_size);
                dbg("render_direct: skip (rows unchanged, cooldown)");
                return 1;  /* truly static — skip */
            }
            /* Fall through: content unchanged but ghost-clear interval elapsed.
             * Re-flash the screen to clear accumulated eink ghosting. */
            dbg("render_direct: ghost-clear refresh (static content)");
            g_last_forced_refresh = now;
        }
    }

    /* Full GC16 refresh — covers entire screen so white borders don't ghost */
    struct mxcfb_update_data upd;
    memset(&upd, 0, sizeof(upd));
    upd.update_region.top    = 0;
    upd.update_region.left   = 0;
    upd.update_region.width  = (uint32_t)g_fb_w;
    upd.update_region.height = (uint32_t)g_fb_h;
    upd.waveform_mode        = WAVEFORM_MODE_GC16;
    upd.update_mode          = UPDATE_MODE_FULL;
    upd.temp                 = TEMP_USE_AUTO;

    int upd_rc;
    do {
        upd_rc = ioctl(g_fb_fd, MXCFB_SEND_UPDATE, &upd);
    } while (upd_rc < 0 && errno == EINTR);
    if (upd_rc < 0)
        dbg("render_direct: ioctl failed errno=%d (%s)", errno, strerror(errno));
    else
        dbg("render_direct: ioctl OK, refresh sent (%dx%d full GC16)", g_fb_w, g_fb_h);

    g_last_forced_refresh = time(NULL);  /* reset ghost-clear clock on real refresh */

    /* Copy current fb into g_last_fb for next comparison */
    if (g_last_fb && g_last_fb_w == g_fb_w && g_last_fb_h == g_fb_h) {
        memcpy(g_last_fb, fb, g_fb_size);
    }

    return 0;
}

void airplay_mirror_stop(void)
{
    mdns_running = 0;
    if (mdns_fd >= 0) { close(mdns_fd); mdns_fd = -1; }
    if (mdns_started) { pthread_join(mdns_tid, NULL); mdns_started = 0; }

    if (g.raop) {
        raop_stop_httpd(g.raop);
        raop_destroy(g.raop);
        g.raop = NULL;
    }
    if (g.dnssd) { dnssd_destroy(g.dnssd); g.dnssd = NULL; }

    decoder_free();

    pthread_mutex_lock(&g.frame_mutex);
    free(g.frame_buf); g.frame_buf = NULL; g.frame_ready = 0;
    pthread_mutex_unlock(&g.frame_mutex);
    pthread_mutex_destroy(&g.frame_mutex);

    fb_cleanup();

    if (g_log) { fclose(g_log); g_log = NULL; }
}

int airplay_mirror_get_frame(uint8_t *out_gray8, int *out_w, int *out_h)
{
    int ret = -1;
    pthread_mutex_lock(&g.frame_mutex);
    if (g.frame_ready && g.frame_buf) {
        memcpy(out_gray8, g.frame_buf, g.frame_w * g.frame_h);
        *out_w = g.frame_w; *out_h = g.frame_h;
        g.frame_ready = 0; ret = 0;
    }
    pthread_mutex_unlock(&g.frame_mutex);
    return ret;
}

int airplay_mdns_start(const char *device_name, int port)
{
    (void)device_name; (void)port;
    return 0; /* mDNS started inside airplay_mirror_start */
}

void airplay_mdns_stop(void)
{
    mdns_running = 0;
}
