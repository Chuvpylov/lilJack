/*
 * backends/hui_linux_drm.h — Linux DRM/KMS backend for hui
 *
 * Software rasterizer (hui_headless.h) → DRM dumb buffer → page-flip.
 * Double-buffered: two dumb buffers allocated at init; we rasterize into
 * the back buffer then set CRTC to display it (synchronous flip).
 *
 * Select with: #define HUI_BACKEND_LINUX_DRM before #include "hui.h"
 * Build:       add -ldrm to link flags (libdrm-dev required)
 *              apt-get install libdrm-dev
 *
 * Typical usage:
 *   int w, h;
 *   hui_drm_query_size(&w, &h);     // detect display resolution
 *   hui_init(w, h);
 *   while (running) {
 *       hui_begin_frame();
 *       ... draw ...
 *       hui_end_frame();             // calls hui_backend_flush internally
 *   }
 *   hui_drm_close();
 *   hui_shutdown();
 *
 * Requires CAP_SYS_ADMIN or membership in the 'video' group, OR the DRM
 * device must allow unprivileged access (common on most distros for seat
 * owners via logind/udev).
 *
 * If DRM initialisation fails the backend silently falls back to headless
 * PPM output so the build always produces something runnable.
 */

#ifndef HUI_LINUX_DRM_H
#define HUI_LINUX_DRM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* libdrm headers --------------------------------------------------------- */
#ifdef __has_include
#  if __has_include(<xf86drm.h>)
#    include <xf86drm.h>
#    include <xf86drmMode.h>
#  else
#    error "hui_linux_drm.h requires libdrm-dev: apt-get install libdrm-dev"
#  endif
#else
#  include <xf86drm.h>
#  include <xf86drmMode.h>
#endif

/* Fallback: some builds only have the kernel uapi headers. */
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifndef DRM_IOCTL_MODE_CREATE_DUMB
#  error "hui_linux_drm.h requires libdrm-dev: apt-get install libdrm-dev"
#endif

/* Suppress headless init/free/resize — we provide our own below.
 * Suppress headless flush       — we provide our own below. */
#define HUI_HEADLESS_NO_INIT
#define HUI_HEADLESS_NO_FLUSH
#include "hui_headless.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- tunables ---------------------------------------------------------- */

/* Override default device search path (call hui_drm_set_device() before
 * hui_init() to use a specific card). */
#ifndef HUI_DRM_DEV
#  define HUI_DRM_DEV NULL   /* NULL → auto-detect /dev/dri/card0..3 */
#endif

/* ---- DRM context ------------------------------------------------------- */

typedef struct {
    int fd;

    /* Selected connector / encoder / CRTC */
    uint32_t conn_id;
    uint32_t enc_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;

    /* Original CRTC state — restored on hui_drm_close() */
    drmModeCrtc *saved_crtc;

    /* Double-buffered dumb buffers: index 0 = front (displayed), 1 = back */
    uint32_t fb_id    [2];
    uint32_t buf_handle[2];
    uint8_t *buf_map  [2];
    uint32_t buf_stride[2];
    uint32_t buf_size [2];
    int      cur_front;   /* index of the currently displayed buffer (0 or 1) */
} hui__drm_ctx_t;

static hui__drm_ctx_t hui__drm;
static bool           hui__drm_ok   = false;
static char           hui__drm_dev_override[64]; /* empty = auto */

/* ---- Public API (declarations) ---------------------------------------- */

/* Returns true if DRM initialised successfully. */
static bool hui_drm_is_available(void) __attribute__((unused));

/* Override which /dev/dri/cardN to use. Must be called before hui_init(). */
static void hui_drm_set_device(const char *path) __attribute__((unused));

/* Detect display resolution without fully initialising.
 * On success writes w/h from the first connected mode; falls back to 1920×1080. */
static void hui_drm_query_size(int *out_w, int *out_h) __attribute__((unused));

/* Close DRM fds, unmap buffers, restore original CRTC. */
static void hui_drm_close(void) __attribute__((unused));

/* ---- Internal helpers -------------------------------------------------- */

/* Find a card fd that has at least one connector. */
static int hui__drm_open_device(void) {
    const char *override = hui__drm_dev_override[0] ? hui__drm_dev_override : NULL;
    if (override) {
        int fd = open(override, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "hui_drm: cannot open %s: %s\n", override, strerror(errno));
        }
        return fd;
    }
    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }
        int ok = (res->count_connectors > 0);
        drmModeFreeResources(res);
        if (ok) return fd;
        close(fd);
    }
    return -1;
}

/* Allocate and mmap one dumb buffer.  Returns false on error. */
static bool hui__drm_alloc_buf(int idx) {
    hui__drm_ctx_t *d = &hui__drm;
    uint32_t w = d->mode.hdisplay;
    uint32_t h = d->mode.vdisplay;

    /* Create dumb buffer */
    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width  = w;
    create.height = h;
    create.bpp    = 32;
    if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "hui_drm: DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n", strerror(errno));
        return false;
    }
    d->buf_handle[idx] = create.handle;
    d->buf_stride [idx] = create.pitch;
    d->buf_size   [idx] = (uint32_t)create.size;

    /* Register as a framebuffer (depth=24 bpp=32) */
    if (drmModeAddFB(d->fd, w, h, 24, 32,
                     create.pitch, create.handle,
                     &d->fb_id[idx]) != 0) {
        fprintf(stderr, "hui_drm: drmModeAddFB failed: %s\n", strerror(errno));
        return false;
    }

    /* Map dumb buffer into userspace */
    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "hui_drm: DRM_IOCTL_MODE_MAP_DUMB failed: %s\n", strerror(errno));
        return false;
    }
    void *ptr = mmap(NULL, (size_t)create.size,
                     PROT_READ | PROT_WRITE, MAP_SHARED,
                     d->fd, (off_t)map.offset);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "hui_drm: mmap dumb buffer failed: %s\n", strerror(errno));
        return false;
    }
    d->buf_map[idx] = (uint8_t *)ptr;
    memset(ptr, 0, (size_t)create.size);
    return true;
}

/* Destroy one dumb buffer (unmap + drmModeRmFB + DRM_IOCTL_MODE_DESTROY_DUMB). */
static void hui__drm_free_buf(int idx) {
    hui__drm_ctx_t *d = &hui__drm;
    if (d->buf_map[idx]) {
        munmap(d->buf_map[idx], (size_t)d->buf_size[idx]);
        d->buf_map[idx] = NULL;
    }
    if (d->fb_id[idx]) {
        drmModeRmFB(d->fd, d->fb_id[idx]);
        d->fb_id[idx] = 0;
    }
    if (d->buf_handle[idx]) {
        struct drm_mode_destroy_dumb destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = d->buf_handle[idx];
        ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        d->buf_handle[idx] = 0;
    }
}

/* Initialise DRM: find device → connector → CRTC → allocate buffers → setcrtc. */
static bool hui__drm_init(void) {
    hui__drm_ctx_t *d = &hui__drm;
    memset(d, 0, sizeof(*d));
    d->fd = -1;

    d->fd = hui__drm_open_device();
    if (d->fd < 0) {
        fprintf(stderr, "hui_drm: no usable DRM device found — falling back to headless\n");
        return false;
    }

    /* Enumerate connectors */
    drmModeRes *res = drmModeGetResources(d->fd);
    if (!res) {
        fprintf(stderr, "hui_drm: drmModeGetResources failed — falling back to headless\n");
        close(d->fd); d->fd = -1;
        return false;
    }

    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(d->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }

    if (!conn) {
        fprintf(stderr, "hui_drm: no connected connector found — falling back to headless\n");
        drmModeFreeResources(res);
        close(d->fd); d->fd = -1;
        return false;
    }

    d->conn_id = conn->connector_id;
    /* Use the preferred (first) mode */
    d->mode = conn->modes[0];

    /* Find encoder → CRTC */
    drmModeEncoder *enc = NULL;
    if (conn->encoder_id)
        enc = drmModeGetEncoder(d->fd, conn->encoder_id);

    if (enc && enc->crtc_id) {
        d->crtc_id = enc->crtc_id;
    } else {
        /* Walk available encoders + CRTCs */
        for (int ei = 0; ei < conn->count_encoders && !d->crtc_id; ei++) {
            drmModeEncoder *e = drmModeGetEncoder(d->fd, conn->encoders[ei]);
            if (!e) continue;
            for (int ci = 0; ci < res->count_crtcs && !d->crtc_id; ci++) {
                if (e->possible_crtcs & (1u << ci))
                    d->crtc_id = res->crtcs[ci];
            }
            drmModeFreeEncoder(e);
        }
    }
    if (enc) { d->enc_id = enc->encoder_id; drmModeFreeEncoder(enc); }

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    if (!d->crtc_id) {
        fprintf(stderr, "hui_drm: could not find a CRTC — falling back to headless\n");
        close(d->fd); d->fd = -1;
        return false;
    }

    /* Save original CRTC so we can restore on close */
    d->saved_crtc = drmModeGetCrtc(d->fd, d->crtc_id);

    /* Allocate both dumb buffers */
    if (!hui__drm_alloc_buf(0) || !hui__drm_alloc_buf(1)) {
        hui__drm_free_buf(0);
        hui__drm_free_buf(1);
        if (d->saved_crtc) { drmModeFreeCrtc(d->saved_crtc); d->saved_crtc = NULL; }
        close(d->fd); d->fd = -1;
        return false;
    }

    d->cur_front = 0;

    /* Activate display with buffer 0 */
    if (drmModeSetCrtc(d->fd, d->crtc_id, d->fb_id[0],
                       0, 0, &d->conn_id, 1, &d->mode) != 0) {
        fprintf(stderr, "hui_drm: drmModeSetCrtc failed: %s — falling back to headless\n",
                strerror(errno));
        hui__drm_free_buf(0);
        hui__drm_free_buf(1);
        if (d->saved_crtc) { drmModeFreeCrtc(d->saved_crtc); d->saved_crtc = NULL; }
        close(d->fd); d->fd = -1;
        return false;
    }

    return true;
}

/* Blit hui__fb.pixels (ARGB 0xAARRGGBB) into the back dumb buffer
 * converting to DRM XRGB8888 (B in lowest byte on little-endian). */
static void hui__drm_blit(void) {
    hui__drm_ctx_t *d = &hui__drm;
    int back = d->cur_front ^ 1;

    int src_w = hui__fb.w;
    int src_h = hui__fb.h;
    int dst_w = (int)d->mode.hdisplay;
    int dst_h = (int)d->mode.vdisplay;
    int copy_w = src_w < dst_w ? src_w : dst_w;
    int copy_h = src_h < dst_h ? src_h : dst_h;

    uint32_t *src     = hui__fb.pixels;
    uint8_t  *dst_base = d->buf_map[back];
    uint32_t  stride  = d->buf_stride[back];

    for (int y = 0; y < copy_h; y++) {
        uint32_t *drow = (uint32_t *)(dst_base + (size_t)y * stride);
        uint32_t *srow = src + y * src_w;
        for (int x = 0; x < copy_w; x++) {
            /* Source: 0xAARRGGBB (headless ARGB packed) */
            uint32_t px = srow[x];
            uint8_t r = (uint8_t)((px >> 16) & 0xFF);
            uint8_t g = (uint8_t)((px >>  8) & 0xFF);
            uint8_t b = (uint8_t)( px        & 0xFF);
            /* DRM XRGB8888: bits[31:24]=X, [23:16]=R, [15:8]=G, [7:0]=B */
            drow[x] = ((uint32_t)0xFF << 24) |
                      ((uint32_t)r    << 16) |
                      ((uint32_t)g    <<  8) |
                       (uint32_t)b;
        }
        /* Zero-fill any columns wider than the source */
        for (int x = copy_w; x < dst_w; x++)
            drow[x] = 0;
    }
    /* Zero-fill any rows taller than the source */
    for (int y = copy_h; y < dst_h; y++) {
        uint32_t *drow = (uint32_t *)(dst_base + (size_t)y * stride);
        memset(drow, 0, (size_t)dst_w * 4);
    }
}

/* Flip to the back buffer (synchronous: drmModeSetCrtc). */
static void hui__drm_flip(void) {
    hui__drm_ctx_t *d = &hui__drm;
    int back = d->cur_front ^ 1;
    drmModeSetCrtc(d->fd, d->crtc_id, d->fb_id[back],
                   0, 0, &d->conn_id, 1, &d->mode);
    d->cur_front = back;
}

/* ---- headless_init / free / resize (required by hui.h) ---------------- */
/* These replace HUI_HEADLESS_NO_INIT stubs. */

static void hui_headless_init(int w, int h) {
    hui__drm_ok = hui__drm_init();

    /* If DRM is up, use the actual display resolution as the pixel buffer size
     * so the rasterizer fills the full screen. */
    int fw = (hui__drm_ok && hui__drm.mode.hdisplay) ? (int)hui__drm.mode.hdisplay : w;
    int fh = (hui__drm_ok && hui__drm.mode.vdisplay) ? (int)hui__drm.mode.vdisplay : h;

    hui__fb.w       = fw;
    hui__fb.h       = fh;
    hui__fb.pixels  = (uint32_t *)calloc((size_t)(fw * fh), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = fw; hui__fb.clip_y1 = fh;
    hui__fb.clip_depth = 0;
}

static void hui_headless_free(void) {
    free(hui__fb.pixels);
    hui__fb.pixels = NULL;
}

static void hui_headless_resize(int w, int h) {
    /* DRM resolution is fixed by hardware; ignore resize requests when live. */
    if (hui__drm_ok) { (void)w; (void)h; return; }
    free(hui__fb.pixels);
    hui__fb.w = w; hui__fb.h = h;
    hui__fb.pixels  = (uint32_t *)calloc((size_t)(w * h), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = w; hui__fb.clip_y1 = h;
    hui__fb.clip_depth = 0;
}

/* ---- hui_backend_flush ------------------------------------------------- */

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    /* Clear software pixel buffer */
    if (hui__fb.pixels)
        memset(hui__fb.pixels, 0, (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));

    /* Rasterize into hui__fb.pixels */
    hui__rasterize(cmds, count, strpool, datapool);

    if (hui__drm_ok) {
        /* Blit to back dumb buffer, then page-flip */
        hui__drm_blit();
        hui__drm_flip();
    } else {
        /* Fallback: write a PPM for debugging / CI verification */
        hui_headless_save_ppm("build/drm_frame.ppm");
    }
}

/* ---- Public API (implementations) ------------------------------------- */

static bool hui_drm_is_available(void) { return hui__drm_ok; }

static void hui_drm_set_device(const char *path) {
    if (!path) { hui__drm_dev_override[0] = '\0'; return; }
    snprintf(hui__drm_dev_override, sizeof(hui__drm_dev_override), "%s", path);
}

static void hui_drm_query_size(int *out_w, int *out_h) {
    /* Open device temporarily just to read the mode, if not already open. */
    if (hui__drm_ok) {
        *out_w = (int)hui__drm.mode.hdisplay;
        *out_h = (int)hui__drm.mode.vdisplay;
        return;
    }
    /* Probe without full init */
    int fd = hui__drm_open_device();
    if (fd < 0) { *out_w = 1920; *out_h = 1080; return; }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { close(fd); *out_w = 1920; *out_h = 1080; return; }

    *out_w = 1920; *out_h = 1080; /* default if no connected connector found */
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            *out_w = c->modes[0].hdisplay;
            *out_h = c->modes[0].vdisplay;
            drmModeFreeConnector(c);
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    close(fd);
}

static void hui_drm_close(void) {
    hui__drm_ctx_t *d = &hui__drm;
    if (!hui__drm_ok) return;

    /* Restore original CRTC */
    if (d->saved_crtc) {
        drmModeSetCrtc(d->fd, d->saved_crtc->crtc_id,
                       d->saved_crtc->buffer_id,
                       d->saved_crtc->x, d->saved_crtc->y,
                       &d->conn_id, 1, &d->saved_crtc->mode);
        drmModeFreeCrtc(d->saved_crtc);
        d->saved_crtc = NULL;
    }

    hui__drm_free_buf(0);
    hui__drm_free_buf(1);

    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    hui__drm_ok = false;
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_LINUX_DRM_H */
