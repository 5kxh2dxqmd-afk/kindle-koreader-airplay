#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame callback: raw grayscale pixels (1 byte/pixel), row-major */
typedef void (*airplay_frame_cb)(const uint8_t *gray8, int width, int height, void *userdata);

typedef struct airplay_config {
    const char *device_name;   /* e.g. "Kindle AirPlay" */
    int         http_port;     /* AirPlay control port, default 7000 */
    int         video_port;    /* H.264 stream port, default 7100 */
    airplay_frame_cb on_frame;
    void        *userdata;
} airplay_config_t;

/* Returns 0 on success */
int  airplay_mirror_start(const airplay_config_t *cfg);
void airplay_mirror_stop(void);

/* Get latest decoded frame (non-blocking, returns 0 if new frame available) */
int  airplay_mirror_get_frame(uint8_t *out_gray8, int *out_w, int *out_h);

/* Scale current frame into fb (gray8, row stride = fb_stride bytes).
 * Nearest-neighbor scale to fit fb_w x fb_h with letterbox/pillarbox.
 * Returns 0 if new frame written, -1 if no frame ready. */
int  airplay_mirror_render_to_fb(uint8_t *fb, int fb_w, int fb_h, int fb_stride);

/* Write current frame directly to /dev/fb0 and trigger mxcfb eink refresh.
 * Bypasses KOReader screen/blitbuffer entirely.
 * Returns 0 on success, -1 if no frame ready or fb unavailable. */
int  airplay_mirror_render_direct(void);

/* Clear /dev/fb0 to white and trigger an eink refresh before KOReader regains
 * ownership of the display. Returns 0 on success, -1 if fb unavailable. */
int  airplay_mirror_clear_direct(void);

/* mDNS: advertise on 224.0.0.251:5353 — call in a thread or before event loop */
int  airplay_mdns_start(const char *device_name, int port);
void airplay_mdns_stop(void);

#ifdef __cplusplus
}
#endif
