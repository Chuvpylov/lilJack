/*
 * hui.h — Immediate-mode UI + scheduler framework
 *
 * Single-header, stb-style. Define HUI_IMPLEMENTATION in exactly one
 * translation unit before including.
 *
 * Backend selection (define one before including):
 *   HUI_BACKEND_HEADLESS   — memory buffer + PPM dump  [WORKING]
 *   HUI_BACKEND_X11        — X11 window + software raster  [WORKING]
 *   HUI_BACKEND_SDL2       — SDL2 window + software raster  [WORKING]
 *   HUI_BACKEND_LINUX_FB   — /dev/fb0 mmap + evdev  [WORKING]
 *   HUI_BACKEND_SVG        — SVG string/file export  [WORKING]
 *   HUI_BACKEND_LINUX_DRM  — DRM/KMS  [STUB — falls back to headless]
 *   HUI_BACKEND_OPENGL     — GL 3.3  [STUB — falls back to headless]
 *   HUI_BACKEND_GLES2      — GLES2  [STUB — falls back to headless]
 *   HUI_BACKEND_RP2350     — RP2350 bare-metal PIO/SPI + ST7789/ILI9341  [WORKING]
 *   HUI_BACKEND_MATRIX8X8  — 8x8 WS2812/APA102 LED grid  [WORKING]
 *   HUI_BACKEND_CANVAS     — Emscripten/WASM Canvas2D  [WORKING]
 *
 * Example:
 *   #define HUI_IMPLEMENTATION
 *   #define HUI_BACKEND_HEADLESS
 *   #include "hui.h"
 */

#ifndef HUI_H
#define HUI_H

#define HUI_VERSION_MAJOR 0
#define HUI_VERSION_MINOR 5
#define HUI_VERSION_PATCH 1

/* Enable POSIX clock_gettime (must precede system headers) */
#if (defined(__linux__) || defined(__APPLE__)) && !defined(HUI_BAREMETAL)
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <time.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ---- Profile macros ---- */
/*
 * Define exactly one before #include "hui.h" to select a pre-configured
 * feature set. Unrecognised = HUI_PROFILE_WORKSTATION (all features).
 *
 * Each profile sets derivative opt-out flags used by widget headers:
 *   HUI_NO_FLOAT_WIDGETS — omit color pickers, gradient editor, etc.
 *   HUI_NO_DEBUG         — omit debug overlays (hui_debug.h no-ops)
 *   HUI_NO_RENDER        — no rasteriser; command encoding only
 *
 * Usage:
 *   #define HUI_PROFILE_MCU
 *   #include "hui.h"
 */
#if defined(HUI_PROFILE_MCU)
   /* Minimal embedded — strip heavy float widgets and debug tooling. */
#  ifndef HUI_NO_FLOAT_WIDGETS
#    define HUI_NO_FLOAT_WIDGETS
#  endif
#  ifndef HUI_NO_DEBUG
#    define HUI_NO_DEBUG
#  endif
#elif defined(HUI_PROFILE_KIOSK)
   /* Kiosk / touch-screen — no dev-tools; all widgets remain. */
#  ifndef HUI_NO_DEBUG
#    define HUI_NO_DEBUG
#  endif
#elif defined(HUI_PROFILE_PROTOCOL)
   /* Protocol node — command encoding only; no rendering, no widgets. */
#  ifndef HUI_NO_RENDER
#    define HUI_NO_RENDER
#  endif
#  ifndef HUI_NO_FLOAT_WIDGETS
#    define HUI_NO_FLOAT_WIDGETS
#  endif
#  ifndef HUI_NO_DEBUG
#    define HUI_NO_DEBUG
#  endif
#elif defined(HUI_PROFILE_BALL)
   /* Ball build launcher — draw + font + math + one backend only.
    * hui_ball.c has its own minimal button impl; no widget layer needed.
    * Strips: hui_widgets.h, debug overlays, float/gradient editors. */
#  ifndef HUI_NO_WIDGETS
#    define HUI_NO_WIDGETS
#  endif
#  ifndef HUI_NO_FLOAT_WIDGETS
#    define HUI_NO_FLOAT_WIDGETS
#  endif
#  ifndef HUI_NO_DEBUG
#    define HUI_NO_DEBUG
#  endif
/* HUI_PROFILE_WORKSTATION — default; all features, nothing disabled. */
#endif

/* Sub-headers (declarations) */
#include "hui_math.h"
#include "hui_cum.h"
#include "hui_draw.h"
#include "hui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Style ---- */

typedef struct {
    hui_color bg;
    hui_color fg;
    hui_color accent;
    hui_color hover;
    hui_color pressed;
    int       font_scale;
} hui_style;

/* ---- IO Context ---- */
typedef struct {
    int16_t  mouse_x, mouse_y;          /* current mouse position (virtual coords) */
    uint8_t  mouse_btn;                  /* buttons down this frame (bit 0=LMB,1=RMB,2=MMB) */
    uint8_t  mouse_btn_prev;             /* buttons down last frame — for click detection */
    int16_t  mouse_x_prev, mouse_y_prev; /* mouse position last frame — for drag delta */
    int16_t  scroll_dy;                  /* scroll wheel delta this frame (+up/-down), coarse steps */
    int16_t  scroll_dx;                  /* horizontal scroll delta this frame (+right/-left) */
    int16_t  scroll_py;                  /* PIXEL-precise vertical scroll this frame (+down/-up):
                                            touch pan + momentum and pixel-mode trackpad wheels
                                            (canvas backend). 0 on backends/devices without it —
                                            apps fall back to scroll_dy steps. */
    bool     keys[256];                  /* key held state indexed by HUI_KEY_* / ASCII */
    bool     keys_prev[256];             /* key held last frame */
    char     text_typed[32];            /* chars typed this frame (printable + ctrl, backend-filled) */
    int      text_typed_len;            /* number of valid bytes in text_typed */
    uint8_t  mods;                      /* modifier keys held this frame (HUI_MOD_*) */
    float    dpr;   /* device pixel ratio (1.0 = normal, 2.0 = HiDPI/Retina) */
    /* Touch: up to 5 simultaneous points (index-stable within a gesture) */
    struct {
        int16_t  x, y;   /* screen-space position */
        uint32_t id;     /* platform touch identifier (stable during gesture) */
        bool     active; /* true = finger down */
    } touches[5];
} hui_io_ctx;

/* ---- Main context ---- */

typedef struct hui_ctx {
    hui_dl      dl;
    hui_io_ctx  io;
    hui_style   style;
    int16_t     screen_w;
    int16_t     screen_h;
    uint32_t    frame;
    uint64_t    time_us;
    uint64_t    time_us_prev;
    float       fps;
    int         focus_id;   /* ID of widget currently holding keyboard focus (0 = none) */
} hui_ctx;

/* Global context pointer */
extern hui_ctx *hui_g;

/* ---- IO helpers (static inline — always available, no HUI_IMPLEMENTATION needed) ---- */

/* Is the mouse currently inside rect r? */
static inline bool hui_is_hovered(hui_rect r) {
    if (!hui_g) return false;
    return hui_rect_contains(r, hui_g->io.mouse_x, hui_g->io.mouse_y);
}

/* Is mouse button btn (0=LMB,1=RMB,2=MMB) currently held and inside r? */
static inline bool hui_is_pressed(hui_rect r, int btn) {
    return hui_is_hovered(r) && (hui_g->io.mouse_btn & (1u << btn));
}

/* Did mouse button btn click inside r this frame (down now, was up last frame)? */
static inline bool hui_is_clicked(hui_rect r, int btn) {
    uint8_t mask = (uint8_t)(1u << btn);
    return hui_is_hovered(r) &&
           (hui_g->io.mouse_btn      & mask) &&
           !(hui_g->io.mouse_btn_prev & mask);
}

/* Was key k pressed this frame (down now, up last frame)? */
static inline bool hui_key_pressed(int k) {
    if (!hui_g || (unsigned)k >= 256) return false;
    return hui_g->io.keys[k] && !hui_g->io.keys_prev[k];
}

/* Is key k currently held? */
static inline bool hui_key_held(int k) {
    if (!hui_g || (unsigned)k >= 256) return false;
    return hui_g->io.keys[k];
}

/* ---- Focus system ---- */
/* Set keyboard focus to widget id. Pass 0 to clear focus. */
static inline void hui_focus_set(int id)   { if (hui_g) hui_g->focus_id = id; }
/* Release focus only if this widget currently holds it. */
static inline void hui_focus_clear(int id) { if (hui_g && hui_g->focus_id == id) hui_g->focus_id = 0; }
/* Does widget id currently have keyboard focus? */
static inline bool hui_focus_has(int id)   { return hui_g && hui_g->focus_id == id && id != 0; }
/* Return current focus ID (0 = none). */
static inline int  hui_focus_get(void)     { return hui_g ? hui_g->focus_id : 0; }

/* Current frames-per-second (exponentially smoothed, updated by hui_begin_frame). */
static inline float hui_fps(void) { return hui_g ? hui_g->fps : 0.0f; }

/* ---- Key codes for hui_g->io.keys[] ---- */
/* Printable ASCII (32-126) index directly. Special keys use codes > 127. */
#define HUI_KEY_BACKSPACE  0x08
#define HUI_KEY_TAB        0x09
#define HUI_KEY_RETURN     0x0D
#define HUI_KEY_ESCAPE     0x1B
#define HUI_KEY_DELETE     0x7F
#define HUI_KEY_LEFT       0x80
#define HUI_KEY_RIGHT      0x81
#define HUI_KEY_UP         0x82
#define HUI_KEY_DOWN       0x83
#define HUI_KEY_HOME       0x84
#define HUI_KEY_END        0x85
#define HUI_KEY_PGUP       0x86
#define HUI_KEY_PGDN       0x87
#define HUI_KEY_F1         0x88
#define HUI_KEY_F2         0x89
#define HUI_KEY_F3         0x8A
#define HUI_KEY_F4         0x8B
#define HUI_KEY_F5         0x8C
#define HUI_KEY_F6         0x8D
#define HUI_KEY_F7         0x8E
#define HUI_KEY_F8         0x8F
#define HUI_KEY_F9         0x90
#define HUI_KEY_F10        0x91
#define HUI_KEY_F11        0x92
#define HUI_KEY_F12        0x93

/* ---- Modifier bitmasks (passed as mods argument to panel input callbacks) ---- */
#define HUI_MOD_SHIFT 1
#define HUI_MOD_CTRL  2
#define HUI_MOD_ALT   4

/* ---- Backend forward declarations ---- */
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool);

#if defined(HUI_BACKEND_X11)
static void hui_x11_update_io(void);
#endif
#if defined(HUI_BACKEND_SDL2)
static void hui_sdl_pump_io(void);
#endif
#if defined(HUI_BACKEND_LINUX_FB)
static void hui_fb_pump_io(void);
#endif

/* Declared only when hui_headless.h or a backend that provides stubs is included */
#if defined(HUI_BACKEND_HEADLESS)  || defined(HUI_BACKEND_X11)      || \
    defined(HUI_BACKEND_SDL2)      || defined(HUI_BACKEND_OPENGL)    || \
    defined(HUI_BACKEND_GLES2)     || defined(HUI_BACKEND_LINUX_FB)  || \
    defined(HUI_BACKEND_LINUX_DRM) || defined(HUI_BACKEND_SVG)       || \
    defined(HUI_BACKEND_CANVAS)
static void hui_headless_init(int w, int h);
static void hui_headless_free(void);
static void hui_headless_resize(int w, int h);
#endif

/* ---- Frame API ---- */
void hui_init(int w, int h);
void hui_resize(int w, int h);     /* resize framebuffer + update screen dims */
void hui_begin_frame(void);
void hui_end_frame(void);
void hui_shutdown(void);

/* ---- Cursor shape ---- */
typedef enum { HUI_CURSOR_DEFAULT = 0, HUI_CURSOR_NS_RESIZE,
               HUI_CURSOR_EW_RESIZE } hui_cursor_t;
void hui_set_cursor(hui_cursor_t c);

/* ---- Utility ---- */
/* printf into a rotating 8-slot scratch buffer (256 bytes/slot).
 * Result is valid until the same slot is reused after 8 more calls.
 * Kills the snprintf+static-char boilerplate pattern. */
#ifdef __GNUC__
const char *hui_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
const char *hui_fmt(const char *fmt, ...);
#endif

/* ---- Callback-driven main loop ---- */
/*
 * Set a per-frame callback to use with hui_run().
 * On desktop: hui_run() calls fn(user) in a tight loop until fn calls hui_stop().
 * On Emscripten: hui_run() delegates to emscripten_set_main_loop_arg — fn is called
 *   by the browser's animation scheduler (no blocking loop).
 *
 * Traditional while-loop apps do NOT need hui_run() — it is purely opt-in.
 */
void hui_set_frame_fn(void (*fn)(void *user), void *user);
void hui_run(void);
void hui_stop(void);  /* signal hui_run() loop to exit (desktop only) */

/* ---- Implementation ---- */

#ifdef HUI_IMPLEMENTATION

/* Define the draw list global (declared extern in hui_draw.h) */
hui_dl *hui_dl_g = NULL;

static hui_ctx hui__ctx_storage;
hui_ctx *hui_g = &hui__ctx_storage;

static hui_style hui__default_style(void) {
    hui_style s;
    s.bg       = CUM_BG2;
    s.fg       = CUM_FG;
    s.accent   = CUM_ACCENT;
    s.hover    = CUM_BG4;
    s.pressed  = CUM_BG1;
    s.font_scale = 1;
    return s;
}

void hui_init(int w, int h) {
    memset(hui_g, 0, sizeof(*hui_g));
    hui_g->screen_w = (int16_t)w;
    hui_g->screen_h = (int16_t)h;
    hui_g->style    = hui__default_style();
    hui_dl_init(&hui_g->dl);
    hui_dl_g = &hui_g->dl;

#if defined(HUI_BACKEND_HEADLESS)  || defined(HUI_BACKEND_X11)      || \
    defined(HUI_BACKEND_SDL2)      || defined(HUI_BACKEND_OPENGL)    || \
    defined(HUI_BACKEND_GLES2)     || defined(HUI_BACKEND_LINUX_FB)  || \
    defined(HUI_BACKEND_LINUX_DRM) || defined(HUI_BACKEND_CANVAS)
    hui_headless_init(w, h);
#endif
}

void hui_begin_frame(void) {
    /* 1. Snapshot prev state for delta detection */
    hui_g->io.mouse_btn_prev = hui_g->io.mouse_btn;
    hui_g->io.mouse_x_prev   = hui_g->io.mouse_x;
    hui_g->io.mouse_y_prev   = hui_g->io.mouse_y;
    memcpy(hui_g->io.keys_prev, hui_g->io.keys, sizeof(hui_g->io.keys));
    hui_g->io.scroll_dy      = 0;
    hui_g->io.scroll_dx      = 0;
    hui_g->io.scroll_py      = 0;
    hui_g->io.text_typed_len = 0;
    /* 2. Pull fresh input AFTER snapshotting prev */
#if defined(HUI_BACKEND_X11)
    hui_x11_update_io();
#endif
#if defined(HUI_BACKEND_SDL2)
    hui_sdl_pump_io();
#endif
#if defined(HUI_BACKEND_LINUX_FB)
    hui_fb_pump_io();
#endif
    /* 3. Update frame timer and fps */
#if (defined(__linux__) || defined(__APPLE__)) && !defined(HUI_BAREMETAL)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        hui_g->time_us_prev = hui_g->time_us;
        hui_g->time_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
        if (hui_g->frame > 1 && hui_g->time_us > hui_g->time_us_prev) {
            float dt = (float)(hui_g->time_us - hui_g->time_us_prev) * 1e-6f;
            float raw = 1.0f / dt;
            hui_g->fps = hui_g->fps > 0.0f ? hui_g->fps * 0.9f + raw * 0.1f : raw;
        }
    }
#endif
    hui_dl_reset(&hui_g->dl);
    hui_dl_g = &hui_g->dl;
    hui_g->frame++;
}

void hui_end_frame(void) {
    hui_dl *dl = &hui_g->dl;
    hui_backend_flush(dl->cmds, dl->count,
                      dl->strpool, dl->datapool);
}

void hui_shutdown(void) {
    hui_dl_free(&hui_g->dl);
#if defined(HUI_BACKEND_HEADLESS)  || defined(HUI_BACKEND_X11)      || \
    defined(HUI_BACKEND_SDL2)      || defined(HUI_BACKEND_OPENGL)    || \
    defined(HUI_BACKEND_GLES2)     || defined(HUI_BACKEND_LINUX_FB)  || \
    defined(HUI_BACKEND_LINUX_DRM) || defined(HUI_BACKEND_CANVAS)
    hui_headless_free();
#endif
}

void hui_resize(int w, int h) {
    hui_g->screen_w = (int16_t)w;
    hui_g->screen_h = (int16_t)h;
#if defined(HUI_BACKEND_HEADLESS)  || defined(HUI_BACKEND_X11)      || \
    defined(HUI_BACKEND_SDL2)      || defined(HUI_BACKEND_OPENGL)    || \
    defined(HUI_BACKEND_GLES2)     || defined(HUI_BACKEND_LINUX_FB)  || \
    defined(HUI_BACKEND_LINUX_DRM) || defined(HUI_BACKEND_CANVAS)
    hui_headless_resize(w, h);
#endif
}

/* Rotating 8-slot printf scratch buffer */
static char hui__fmt_pool[8][256];
static int  hui__fmt_idx;

const char *hui_fmt(const char *fmt, ...) {
    char   *buf = hui__fmt_pool[hui__fmt_idx++ & 7];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);
    return buf;
}

/* ---- Callback-driven main loop — implementation ---- */
static void  (*hui__frame_fn)(void*)  = NULL;
static void   *hui__frame_user        = NULL;
static bool    hui__running           = false;

void hui_set_frame_fn(void (*fn)(void *user), void *user) {
    hui__frame_fn   = fn;
    hui__frame_user = user;
}

void hui_stop(void) { hui__running = false; }

#ifndef HUI_BACKEND_CANVAS  /* canvas backend provides its own hui_run */
void hui_run(void) {
    if (!hui__frame_fn) return;
    hui__running = true;
    while (hui__running) hui__frame_fn(hui__frame_user);
}
#endif

#endif /* HUI_IMPLEMENTATION */

/* ---- Pull in selected backend ---- */

#if defined(HUI_BACKEND_SVG)
#  include "backends/hui_svg.h"
#elif defined(HUI_BACKEND_HEADLESS)
#  include "backends/hui_headless.h"
#elif defined(HUI_BACKEND_X11)
#  include "backends/hui_x11.h"
#elif defined(HUI_BACKEND_SDL2)
#  include "backends/hui_sdl2.h"
#elif defined(HUI_BACKEND_LINUX_FB)
#  include "backends/hui_linux_fb.h"
#elif defined(HUI_BACKEND_LINUX_DRM)
#  include "backends/hui_linux_drm.h"
#elif defined(HUI_BACKEND_OPENGL)
#  include "backends/hui_opengl.h"
#elif defined(HUI_BACKEND_GLES2)
#  include "backends/hui_gles2.h"
#elif defined(HUI_BACKEND_PICOCALC)
#  include "backends/hui_picocalc.h"
#elif defined(HUI_BACKEND_RP2350)
#  include "backends/hui_rp2350.h"
#elif defined(HUI_BACKEND_MATRIX8X8)
#  include "backends/hui_matrix8x8.h"
#elif defined(HUI_BACKEND_CANVAS)
#  include "backends/hui_canvas.h"
#endif

/* Portable cursor shape — dispatches to whichever backend is compiled in.
   X11 already provides hui_win_set_cursor (XCreateFontCursor cache);
   116 = XC_sb_v_double_arrow (vertical resize), 108 = XC_sb_h_double_arrow
   (horizontal resize), 68 = XC_left_ptr (default). */
#ifdef HUI_IMPLEMENTATION
void hui_set_cursor(hui_cursor_t c) {
#if defined(HUI_BACKEND_X11)
    hui_win_set_cursor(c == HUI_CURSOR_NS_RESIZE ? 116 :
                       c == HUI_CURSOR_EW_RESIZE ? 108 : 68);
#elif defined(HUI_BACKEND_CANVAS)
    EM_ASM({
        var cv = (typeof Module !== 'undefined' && Module.canvas)
                 ? Module.canvas : document.getElementById('canvas');
        if (cv) cv.style.cursor = $0 === 1 ? 'ns-resize'
                                : $0 === 2 ? 'ew-resize' : 'default';
    }, (int)c);
#else
    (void)c;
#endif
}
#endif

/* Widget layer — included last so all draw primitives are available */
#ifndef HUI_NO_WIDGETS
#include "hui_widgets.h"
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HUI_H */
