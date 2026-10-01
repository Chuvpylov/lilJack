/*
 * backends/hui_linux_fb.h — Linux /dev/fb0 framebuffer backend
 *
 * Software rasterizer (hui_headless.h) → mmap'd /dev/fb0.
 * Input via /dev/input/event* (evdev, non-blocking poll).
 *
 * Select with: #define HUI_BACKEND_LINUX_FB before #include "hui.h"
 * Links: -lm  (no extra deps — all via direct syscalls)
 *
 * Typical usage:
 *   int w, h;
 *   hui_linux_fb_query_size(&w, &h);   // detect /dev/fb0 resolution
 *   hui_init(w, h);
 *   while (running) {
 *       hui_begin_frame();
 *       ... draw ...
 *       hui_end_frame();               // calls hui_backend_flush internally
 *   }
 *   hui_linux_fb_close();
 *   hui_shutdown();
 *
 * Root (or video/input group) is typically required for /dev/fb0 and evdev.
 * For testing without hardware: modprobe vfb  →  /dev/fb1 (adjust HUI_FB_DEV).
 *
 * Format detection: reads fb_var_screeninfo; handles 32bpp and 16bpp (RGB565),
 * honours the line stride (fb_fix_screeninfo.line_length).
 *
 * Environment (runtime, all optional — the default is the real /dev/fb0):
 *   HUI_FB_DEV=/path        framebuffer device; when it is a regular file (or
 *                           does not exist yet) it is created / truncated and
 *                           used as a file-backed framebuffer: no ioctl, size
 *                           from HUI_FB_W x HUI_FB_H (default 320x320), format
 *                           HUI_FB_BPP (32 = xRGB8888, 16 = RGB565). This is
 *                           how CI runs the the handheld build without the board
 *                           (archhui: make -C cockpit/deck fbtest).
 *   HUI_FB_W, HUI_FB_H, HUI_FB_BPP   file-backed geometry
 *   HUI_FB_NO_EVDEV=1       do not open /dev/input/event* (headless CI)
 *   HUI_FB_EVDEV=/dev/input/eventN   open only this input device
 *
 * Keys: hui__evkey maps Linux KEY_* codes to HUI_KEY_* / ASCII; the PicoCalc
 * keypad (the handheld, picocalc_kbd.ko) reaches this table as ordinary KEY_*
 * codes, see extras/haku/HAKU.md "Keys on the device". Modifier keys fill
 * io->mods; the driver's MSC_SCAN carries the STM32's shifted ASCII, which
 * is used for text_typed so '[' ']' '!' '?' etc. arrive as typed.
 */

#ifndef HUI_LINUX_FB_H
#define HUI_LINUX_FB_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>

/* Override to use a different framebuffer device. */
#ifndef HUI_FB_DEV
#  define HUI_FB_DEV "/dev/fb0"
#endif

/* Maximum evdev devices to open. */
#ifndef HUI_FB_MAX_EVDEV
#  define HUI_FB_MAX_EVDEV 8
#endif

/* Suppress the default init/free/resize from headless (we provide our own). */
#define HUI_HEADLESS_NO_INIT
/* Suppress default flush (we provide ours). */
#define HUI_HEADLESS_NO_FLUSH
#include "hui_headless.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- FB device state ---- */

typedef struct {
    int      fd;
    uint8_t *map;
    size_t   map_size;
    int      w, h;
    int      bpp;
    int      stride;               /* bytes per line */
    int      r_off, g_off, b_off;  /* bit offsets from fb_var_screeninfo */
    bool     open;
    bool     file;                 /* file-backed framebuffer (HUI_FB_DEV is a regular file) */
} hui__fb_dev_t;

static hui__fb_dev_t hui__fb_dev;

/* ---- evdev input state ---- */

typedef struct {
    int     fds[HUI_FB_MAX_EVDEV];
    int     n;
    int     mx, my;
    uint8_t btn;
    uint8_t mods;      /* HUI_MOD_* held */
    int     scan;      /* last MSC_SCAN value (PicoCalc: the STM32's ASCII), -1 = none */
} hui__evdev_t;

static hui__evdev_t hui__evdev;

/* ---- Internal: open /dev/fb0 (or the file-backed framebuffer) ---- */

static int hui__fb_env_int(const char *name, int def) {
    const char *s = getenv(name);
    if (!s || !*s) return def;
    int v = atoi(s);
    return v > 0 ? v : def;
}

static bool hui__fb_open(void) {
    hui__fb_dev_t *d = &hui__fb_dev;
    if (d->open) return true;

    const char *path = getenv("HUI_FB_DEV");
    bool from_env = path && *path;
    if (!from_env) path = HUI_FB_DEV;

    d->fd = open(path, from_env ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
    if (d->fd < 0) {
        fprintf(stderr, "hui_linux_fb: cannot open %s: ", path);
        perror(NULL);
        return false;
    }

    struct stat st;
    bool is_file = fstat(d->fd, &st) == 0 && S_ISREG(st.st_mode);
    d->file = is_file;

    if (is_file) {
        /* file-backed framebuffer: geometry from the environment, xRGB8888 / RGB565 */
        d->w      = hui__fb_env_int("HUI_FB_W", 320);
        d->h      = hui__fb_env_int("HUI_FB_H", 320);
        d->bpp    = hui__fb_env_int("HUI_FB_BPP", 32);
        if (d->bpp != 16) d->bpp = 32;
        d->r_off  = 16; d->g_off = 8; d->b_off = 0;
        d->stride = d->w * (d->bpp / 8);
        d->map_size = (size_t)d->stride * (size_t)d->h;
        if (ftruncate(d->fd, (off_t)d->map_size) < 0) {
            perror("hui_linux_fb: ftruncate");
            close(d->fd); d->fd = -1;
            return false;
        }
    } else {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;
        if (ioctl(d->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
            perror("hui_linux_fb: FBIOGET_VSCREENINFO");
            close(d->fd); d->fd = -1;
            return false;
        }
        d->w     = (int)vinfo.xres;
        d->h     = (int)vinfo.yres;
        d->bpp   = (int)vinfo.bits_per_pixel;
        d->r_off = (int)vinfo.red.offset;
        d->g_off = (int)vinfo.green.offset;
        d->b_off = (int)vinfo.blue.offset;
        d->stride = d->w * (d->bpp / 8);
        if (ioctl(d->fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.line_length > 0)
            d->stride = (int)finfo.line_length;
        d->map_size = (size_t)d->stride * (size_t)d->h;
    }

    d->map = (uint8_t*)mmap(NULL, d->map_size,
                             PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    if (d->map == MAP_FAILED) {
        perror("hui_linux_fb: mmap");
        d->map = NULL;
        close(d->fd); d->fd = -1;
        return false;
    }

    d->open = true;
    return true;
}

/* ---- Internal: evdev open ---- */

static void hui__evdev_open(void) {
    hui__evdev_t *e = &hui__evdev;
    e->n = 0;
    e->btn = 0;
    e->mods = 0;
    e->scan = -1;
    if (hui__fb_dev.open) {
        e->mx = hui__fb_dev.w / 2;
        e->my = hui__fb_dev.h / 2;
    }
    const char *no = getenv("HUI_FB_NO_EVDEV");
    if (no && *no && *no != '0') return;
    const char *one = getenv("HUI_FB_EVDEV");
    if (one && *one) {
        int fd = open(one, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) e->fds[e->n++] = fd;
        else { fprintf(stderr, "hui_linux_fb: cannot open %s: ", one); perror(NULL); }
        return;
    }
    char path[48];
    for (int i = 0; i < HUI_FB_MAX_EVDEV; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) e->fds[e->n++] = fd;
    }
}

/* Map Linux evdev key codes (linux/input-event-codes.h) → HUI_KEY_* or
 * printable ASCII (unshifted). Modifiers are handled by hui__evmod. */
static int hui__evkey(int code) {
    switch (code) {
        case   1: return HUI_KEY_ESCAPE;      /* KEY_ESC        PicoCalc Esc  */
        case  14: return HUI_KEY_BACKSPACE;   /* KEY_BACKSPACE  Bsp           */
        case  15: return HUI_KEY_TAB;         /* KEY_TAB        Tab           */
        case  28: return HUI_KEY_RETURN;      /* KEY_ENTER      Enter         */
        case  96: return HUI_KEY_RETURN;      /* KEY_KPENTER                  */
        case 111: return HUI_KEY_DELETE;      /* KEY_DELETE     Del           */
        case 105: return HUI_KEY_LEFT;        /* KEY_LEFT       d-pad         */
        case 106: return HUI_KEY_RIGHT;       /* KEY_RIGHT                    */
        case 103: return HUI_KEY_UP;          /* KEY_UP                       */
        case 108: return HUI_KEY_DOWN;        /* KEY_DOWN                     */
        case 102: return HUI_KEY_HOME;        /* KEY_HOME       Fn+Tab        */
        case 107: return HUI_KEY_END;         /* KEY_END        Fn+Del        */
        case 104: return HUI_KEY_PGUP;        /* KEY_PAGEUP                   */
        case 109: return HUI_KEY_PGDN;        /* KEY_PAGEDOWN                 */
        case 110: return -1;                  /* KEY_INSERT (Fn+I) - no hui code */
        /* F-keys: PicoCalc Fn+1..0 -> F1..F10 (picocalc_kbd maps scancodes 0x81..0x90) */
        case  59: return HUI_KEY_F1;  case  60: return HUI_KEY_F2;  case  61: return HUI_KEY_F3;
        case  62: return HUI_KEY_F4;  case  63: return HUI_KEY_F5;  case  64: return HUI_KEY_F6;
        case  65: return HUI_KEY_F7;  case  66: return HUI_KEY_F8;  case  67: return HUI_KEY_F9;
        case  68: return HUI_KEY_F10; case  87: return HUI_KEY_F11; case  88: return HUI_KEY_F12;
        /* Digits */
        case  2: return '1'; case  3: return '2'; case  4: return '3';
        case  5: return '4'; case  6: return '5'; case  7: return '6';
        case  8: return '7'; case  9: return '8'; case 10: return '9';
        case 11: return '0';
        case 57: return ' ';
        /* Punctuation (unshifted; the shifted glyph arrives through MSC_SCAN on the PicoCalc) */
        case 12: return '-';  case 13: return '=';  case 26: return '[';  case 27: return ']';
        case 39: return ';';  case 40: return '\''; case 41: return '`';  case 43: return '\\';
        case 51: return ',';  case 52: return '.';  case 53: return '/';
        /* Letters (QWERTY scancode → ASCII lower) */
        case 16: return 'q'; case 17: return 'w'; case 18: return 'e';
        case 19: return 'r'; case 20: return 't'; case 21: return 'y';
        case 22: return 'u'; case 23: return 'i'; case 24: return 'o';
        case 25: return 'p'; case 30: return 'a'; case 31: return 's';
        case 32: return 'd'; case 33: return 'f'; case 34: return 'g';
        case 35: return 'h'; case 36: return 'j'; case 37: return 'k';
        case 38: return 'l'; case 44: return 'z'; case 45: return 'x';
        case 46: return 'c'; case 47: return 'v'; case 48: return 'b';
        case 49: return 'n'; case 50: return 'm';
        default: return -1;
    }
}

/* Modifier key codes → HUI_MOD_* bit (0 = not a modifier). */
static uint8_t hui__evmod(int code) {
    switch (code) {
        case 29: case 97:  return HUI_MOD_CTRL;   /* KEY_LEFTCTRL / KEY_RIGHTCTRL   PicoCalc Ctrl */
        case 42: case 54:  return HUI_MOD_SHIFT;  /* KEY_LEFTSHIFT / KEY_RIGHTSHIFT (right shift = mouse-mode toggle in picocalc_kbd) */
        case 56: case 100: return HUI_MOD_ALT;    /* KEY_LEFTALT / KEY_RIGHTALT     Alt / Sym */
        default: return 0;
    }
}

/* Poll all evdev fds and update hui_io_ctx. Non-blocking. */
static void hui__evdev_poll(hui_io_ctx *io) {
    hui__evdev_t *e = &hui__evdev;
    hui__fb_dev_t *d = &hui__fb_dev;

    struct pollfd pfds[HUI_FB_MAX_EVDEV];
    for (int i = 0; i < e->n; i++) {
        pfds[i].fd     = e->fds[i];
        pfds[i].events = POLLIN;
    }
    if (e->n <= 0 || poll(pfds, (nfds_t)e->n, 0) <= 0) goto apply;

    struct input_event ev;
    for (int i = 0; i < e->n; i++) {
        if (!(pfds[i].revents & POLLIN)) continue;
        while (read(e->fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) {
                    e->mx += ev.value;
                    if (d->open) {
                        if (e->mx < 0)    e->mx = 0;
                        if (e->mx >= d->w) e->mx = d->w - 1;
                    }
                } else if (ev.code == REL_Y) {
                    e->my += ev.value;
                    if (d->open) {
                        if (e->my < 0)    e->my = 0;
                        if (e->my >= d->h) e->my = d->h - 1;
                    }
                } else if (ev.code == REL_WHEEL) {
                    io->scroll_dy += (int16_t)ev.value;
                }
            } else if (ev.type == EV_MSC && ev.code == MSC_SCAN) {
                e->scan = (int)ev.value;      /* precedes the EV_KEY it belongs to */
            } else if (ev.type == EV_KEY) {
                /* Mouse buttons */
                if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT || ev.code == BTN_MIDDLE) {
                    int bit = (ev.code == BTN_LEFT) ? 0 :
                              (ev.code == BTN_RIGHT) ? 1 : 2;
                    if (ev.value) e->btn |=  (uint8_t)(1u << bit);
                    else          e->btn &= (uint8_t)~(uint8_t)(1u << bit);
                } else if (hui__evmod((int)ev.code)) {
                    uint8_t m = hui__evmod((int)ev.code);
                    if (ev.value) e->mods |= m; else e->mods &= (uint8_t)~m;
                } else {
                    int k = hui__evkey(ev.code);
                    if (k >= 0 && k < 256) {
                        io->keys[k] = (ev.value != 0);     /* value 2 = autorepeat: still held */
                        /* Typed text on key-down: prefer the driver's scancode when it is
                         * printable ASCII (PicoCalc: the STM32 already applied Shift, so
                         * '!' '?' '[' arrive as themselves); else the unshifted key. */
                        if (ev.value == 1 && io->text_typed_len < 31) {
                            int ch = (e->scan >= 32 && e->scan < 127) ? e->scan : k;
                            if (ch >= 32 && ch < 127) {
                                if ((e->mods & HUI_MOD_SHIFT) && ch == k && ch >= 'a' && ch <= 'z') ch -= 32;
                                io->text_typed[io->text_typed_len++] = (char)ch;
                                io->text_typed[io->text_typed_len]   = '\0';
                            }
                        }
                    }
                }
                e->scan = -1;
            }
        }
    }

apply:
    io->mouse_x   = (int16_t)e->mx;
    io->mouse_y   = (int16_t)e->my;
    io->mouse_btn = e->btn;
    io->mods      = e->mods;
}

/* ---- Blit headless ARGB → fb device ---- */

static void hui__fb_blit(void) {
    hui__fb_dev_t *d = &hui__fb_dev;
    if (!d->map || !hui__fb.pixels) return;
    int w = d->w < hui__fb.w ? d->w : hui__fb.w;
    int h = d->h < hui__fb.h ? d->h : hui__fb.h;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = hui__fb.pixels + (size_t)y * (size_t)hui__fb.w;
        uint8_t *line = d->map + (size_t)y * (size_t)d->stride;
        if (d->bpp == 32) {
            uint32_t *dst = (uint32_t*)line;
            for (int x = 0; x < w; x++) {
                uint32_t s = src[x];
                uint8_t r = (uint8_t)((s >> 16) & 0xFF);
                uint8_t g = (uint8_t)((s >>  8) & 0xFF);
                uint8_t b = (uint8_t)( s        & 0xFF);
                dst[x] = ((uint32_t)r << d->r_off) |
                         ((uint32_t)g << d->g_off) |
                         ((uint32_t)b << d->b_off);
            }
        } else if (d->bpp == 16) {
            uint16_t *dst = (uint16_t*)line;
            for (int x = 0; x < w; x++) {
                uint32_t s = src[x];
                uint8_t r = (uint8_t)((s >> 16) & 0xFF);
                uint8_t g = (uint8_t)((s >>  8) & 0xFF);
                uint8_t b = (uint8_t)( s        & 0xFF);
                dst[x] = (uint16_t)(((uint16_t)(r >> 3) << 11) |
                                    ((uint16_t)(g >> 2) <<  5) |
                                     (uint16_t)(b >> 3));
            }
        }
    }
}

/* ---- Public API ---- */

/* Query /dev/fb0 resolution without fully initialising.
 * Call before hui_init() to get the correct w/h:
 *   int w, h;
 *   hui_linux_fb_query_size(&w, &h);
 *   hui_init(w, h);                   <- headless pixel buffer sized correctly */
static void hui_linux_fb_query_size(int *out_w, int *out_h) {
    if (hui__fb_open()) {
        *out_w = hui__fb_dev.w;
        *out_h = hui__fb_dev.h;
    } else {
        *out_w = 800;
        *out_h = 600;
    }
}

/* Close /dev/fb0 + evdev fds. Call after hui_shutdown(). */
static void hui_linux_fb_close(void) {
    hui__fb_dev_t *d = &hui__fb_dev;
    hui__evdev_t  *e = &hui__evdev;
    for (int i = 0; i < e->n; i++) close(e->fds[i]);
    e->n = 0;
    if (d->map)  { munmap(d->map, d->map_size); d->map = NULL; }
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    d->open = false;
}

/* ---- hui_headless_init / free / resize (required by hui.h) ---- */

static void hui_headless_init(int w, int h) {
    /* Open fb device if not already done by hui_linux_fb_query_size(). */
    if (!hui__fb_dev.open) hui__fb_open();
    /* Use actual fb dimensions; ignore requested w/h when fb is available. */
    int fw = hui__fb_dev.open ? hui__fb_dev.w : w;
    int fh = hui__fb_dev.open ? hui__fb_dev.h : h;
    hui__fb.w       = fw;
    hui__fb.h       = fh;
    hui__fb.pixels  = (uint32_t*)calloc((size_t)(fw * fh), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = fw; hui__fb.clip_y1 = fh;
    hui__fb.clip_depth = 0;
    hui__evdev_open();
}

static void hui_headless_free(void) {
    free(hui__fb.pixels);
    hui__fb.pixels = NULL;
}

static void hui_headless_resize(int w, int h) {
    /* FB resolution is fixed by hardware; ignore resize requests. */
    (void)w; (void)h;
}

/* ---- hui_backend_flush ---- */

/* Called by hui_begin_frame() (after snapshotting prev IO) to pull evdev events.
 * This ensures widgets see same-frame input with zero lag. */
static void hui_fb_pump_io(void) {
    if (hui_g) hui__evdev_poll(&hui_g->io);
}

#ifdef HUI_IMPLEMENTATION   /* multi-TU: defined ONCE (hui/tools/multi_tu_test, 2026-09-12) */
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    /* Clear pixel buffer */
    if (hui__fb.pixels)
        memset(hui__fb.pixels, 0,
               (size_t)(hui__fb.w * hui__fb.h) * sizeof(uint32_t));

    /* Software rasterize */
    hui__rasterize(cmds, count, strpool, datapool);

    /* Blit to /dev/fb0 — presentation only, no IO here */
    hui__fb_blit();
}
#endif /* HUI_IMPLEMENTATION — one definition per program; the declaration in hui.h stays visible to every TU */

#ifdef __cplusplus
}
#endif

#endif /* HUI_LINUX_FB_H */
