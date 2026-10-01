/* backends/hui_canvas.h — Emscripten/WASM Canvas2D backend for hui
 *
 * Select with: #define HUI_BACKEND_CANVAS before #include "hui.h"
 * Build: emcc -O2 -s USE_SDL=0 myapp.c -o myapp.html
 *        (or with --shell-file for custom HTML)
 *
 * Features:
 *   - Software rasterizer (hui_headless) → putImageData on <canvas>
 *   - Callback-driven main loop via emscripten_set_main_loop_arg
 *   - Full browser event handling: mouse, keyboard, wheel, touch
 *   - DPR-aware canvas sizing
 *   - Async clipboard API bridge
 *   - Fullscreen + pointer lock support
 *   - PNG download via JS Blob / canvas.toDataURL
 *   - PUSSY clock via emscripten_get_now()
 *
 * Usage pattern:
 *   #define HUI_IMPLEMENTATION
 *   #define HUI_BACKEND_CANVAS
 *   #include "hui.h"
 *
 *   static void my_frame(void *user) {
 *       hui_begin_frame();
 *       // ... draw widgets ...
 *       hui_end_frame();
 *   }
 *   int main(void) {
 *       hui_init(800, 600);
 *       hui_set_frame_fn(my_frame, NULL);
 *       hui_run();
 *       return 0;
 *   }
 *
 * Note on pixel coordinates:
 *   hui_init(css_w, css_h) takes CSS pixel dimensions.
 *   Internally the framebuffer is scaled by DPR so everything is
 *   sharp on high-DPI displays.  Widget/input coordinates remain in
 *   CSS pixels (the DPR scaling is transparent to application code).
 *
 * PUSSY clock shim:
 *   Define HUI_PUSSY_CLOCK_FN before including hui_pussy.h:
 *     #define HUI_PUSSY_CLOCK_FN hui__canvas_now_us
 *     #include "hui_pussy.h"
 */

#ifndef HUI_CANVAS_H
#define HUI_CANVAS_H

/* ---- Emscripten includes / non-emcc stubs -------------------------------- */

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  include <emscripten/html5.h>
#else
/* Stubs for non-Emscripten builds (syntax checking with plain gcc/clang).
 * None of these actually run; they just silence unknown-identifier errors. */
#  define EMSCRIPTEN_KEEPALIVE
#  define EM_ASM(code, ...)        ((void)0)
#  define EM_ASM_INT(code, ...)    0
#  define EM_ASM_DOUBLE(code, ...) 0.0
typedef void (*em_callback_func)(void);
typedef void (*em_arg_callback_func)(void *);
static inline void emscripten_set_main_loop_arg(em_arg_callback_func f,
        void *a, int fps, int sim) { (void)f;(void)a;(void)fps;(void)sim; }
static inline double emscripten_get_now(void)                             { return 0.0; }
static inline void   emscripten_request_fullscreen(const char *t, int s)  { (void)t;(void)s; }
static inline void   emscripten_request_pointerlock(const char *t, int d) { (void)t;(void)d; }
static inline void   emscripten_cancel_main_loop(void)                    {}
#endif /* __EMSCRIPTEN__ */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Public API declarations --------------------------------------------- */

/* Typedef for async clipboard paste callback. */
typedef void (*hui_clipboard_cb)(const char *text, void *user);

/* hui_set_frame_fn / hui_run / hui_stop are declared in hui.h.
 * hui_run is implemented here (inside HUI_IMPLEMENTATION) for the canvas
 * backend because hui.h guards its own hui_run with #ifndef HUI_BACKEND_CANVAS.
 * The declaration is re-stated here for documentation purposes only. */
/* void hui_set_frame_fn(void (*fn)(void *user), void *user); -- from hui.h */
/* void hui_run(void);                                         -- implemented below */

/* ---- Canvas-specific extras ---------------------------------------------- */

/* Trigger browser fullscreen for the <canvas> element. */
void hui_canvas_fullscreen_request(void);

/* Acquire pointer lock on the <canvas> element (hides cursor, raw mouse). */
void hui_canvas_pointerlock_request(void);

/* Write text to the async Clipboard API (fire-and-forget). */
void hui_canvas_clipboard_set(const char *text);

/* Request paste text; cb is called asynchronously (may be a later frame).
 * text is NULL on permission denial or API unavailability. */
void hui_canvas_clipboard_paste(hui_clipboard_cb cb, void *user);

/* Browser "paste" event bridge (no permission prompt — the user pressed
 * Ctrl+V / Cmd+V in the page). Whatever the event carried is parked here:
 * text as UTF-8, images as their encoded bytes (PNG/JPEG/... as the browser
 * hands them over). take_* return a malloc'd copy and clear the slot; NULL
 * when nothing is pending. hui_canvas_clipboard_pending() lets a frame loop
 * notice an arrival without consuming it. */
char          *hui_canvas_clipboard_take_text(void);
unsigned char *hui_canvas_clipboard_take_image(int *len_out);
int            hui_canvas_clipboard_pending(void);

/* Trigger a browser "Save As" PNG download of the current canvas contents. */
void hui_canvas_download_png(const char *filename);

/* Returns the current device pixel ratio (set at init, may change on resize). */
float hui_canvas_dpr(void);

/* ---- Implementation ------------------------------------------------------- */

#ifdef HUI_IMPLEMENTATION

/* Include the headless rasterizer — suppress its own init/free/resize/flush
 * because we provide our own versions below. */
#ifndef HUI_HEADLESS_H
#  define HUI_HEADLESS_NO_INIT
#  define HUI_HEADLESS_NO_FLUSH
#  include "hui_headless.h"
#endif

/* ---- Canvas state -------------------------------------------------------- */

/* DPR and CSS-pixel dimensions — set at init, updated on resize. */
static float hui__canvas_dpr    = 1.0f;
static int   hui__canvas_css_w  = 0;
static int   hui__canvas_css_h  = 0;
/* Physical pixel dimensions (= CSS × DPR). */
static int   hui__canvas_phys_w = 0;
static int   hui__canvas_phys_h = 0;

/* Async clipboard callbacks. */
static hui_clipboard_cb hui__clip_cb   = NULL;
static void            *hui__clip_user = NULL;

/* Parked "paste" event payloads (see hui_canvas_clipboard_take_*). */
static char          *hui__clip_paste_text = NULL;
static unsigned char *hui__clip_paste_img  = NULL;
static int            hui__clip_paste_img_len = 0;

/* Note: hui__frame_fn and hui__frame_user are defined as statics in
 * hui.h's HUI_IMPLEMENTATION block.  They are accessible here because
 * this file is #include-d from within the same translation unit. */

/* ---- PUSSY clock shim ---------------------------------------------------- */

/* uint64_t microseconds from emscripten_get_now() (millisecond resolution).
 * Use as: #define HUI_PUSSY_CLOCK_FN hui__canvas_now_us
 *         #include "hui_pussy.h"          */
static uint64_t hui__canvas_now_us(void)
    __attribute__((unused));
static uint64_t hui__canvas_now_us(void) {
    return (uint64_t)(emscripten_get_now() * 1000.0);
}

/* ---- DPR accessor -------------------------------------------------------- */

float hui_canvas_dpr(void) { return hui__canvas_dpr; }

/* ---- Forward declarations ------------------------------------------------ */

static void hui__canvas_setup_events(void);

/* ---- Browser keycode translation ---------------------------------------- */

/* Maps a KeyboardEvent.code string to a hui key constant or ASCII value.
 * Returns 0 for unknown / unhandled keys. */
static int hui__canvas_keycode(const char *code) {
    /* Special key table. */
    static const struct { const char *s; int k; } tab[] = {
        {"ArrowLeft",  HUI_KEY_LEFT},
        {"ArrowRight", HUI_KEY_RIGHT},
        {"ArrowUp",    HUI_KEY_UP},
        {"ArrowDown",  HUI_KEY_DOWN},
        {"Home",       HUI_KEY_HOME},
        {"End",        HUI_KEY_END},
        {"PageUp",     HUI_KEY_PGUP},
        {"PageDown",   HUI_KEY_PGDN},
        {"Backspace",  HUI_KEY_BACKSPACE},
        {"Enter",      HUI_KEY_RETURN},
        {"Escape",     HUI_KEY_ESCAPE},
        {"Delete",     HUI_KEY_DELETE},
        {"Tab",        HUI_KEY_TAB},
        {"Space",      ' '},
        {NULL, 0}
    };

    if (!code) return 0;

    /* "KeyA".."KeyZ" → 'a'..'z' */
    if (code[0] == 'K' && code[1] == 'e' && code[2] == 'y' &&
        code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0')
        return code[3] - 'A' + 'a';

    /* "Digit0".."Digit9" → '0'..'9' */
    if (code[0] == 'D' && code[1] == 'i' && code[2] == 'g' &&
        code[3] == 'i' && code[4] == 't' &&
        code[5] >= '0' && code[5] <= '9' && code[6] == '\0')
        return (int)(unsigned char)code[5];

    for (int i = 0; tab[i].s; i++)
        if (strcmp(tab[i].s, code) == 0) return tab[i].k;

    return 0;
}

/* ---- Exported C callbacks called from JS --------------------------------- */

/* JS calls these directly via Module._hui__canvas_*() after events fire.
 * All coordinates are already in physical (framebuffer) pixels unless
 * noted.  The JS event handlers below perform the DPR scaling. */

/* Mouse moved — CSS pixels. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_mouse_move(int x, int y) {
    if (!hui_g) return;
    hui_g->io.mouse_x = (int16_t)x;
    hui_g->io.mouse_y = (int16_t)y;
}

/* Click queue for canvas backend.
 *
 * Problem A (fast click): mousedown+mouseup both fire between frames.
 *   begin_frame sees btn=0, prev=0 → no 0→1 transition → click missed.
 *
 * Problem B (normal click): mousedown fires BEFORE begin_frame.
 *   begin_frame snapshots prev = btn = 1 (already pressed) → no 0→1
 *   transition → click missed.
 *
 * Fix: on any queued click, pre_frame temporarily clears the queued bits
 * from mouse_btn (saving the real state) so begin_frame snapshots prev=0.
 * post_begin restores the real state → widgets see prev=0, btn=1 → click.
 *
 * Phase-2 correctness for drag: we track which buttons are physically held
 * (btn_held) so that phase-2 cleanup only clears bits that were actually
 * released. Without this, dragging dies one frame after mousedown because
 * phase-2 cleared the LMB bit even while the button was still held. */
static uint8_t hui__canvas_click_queue = 0;
static uint8_t hui__canvas_click_phase = 0; /* 0=idle, 1=press, 2=release */
static uint8_t hui__canvas_saved_btn   = 0;
static uint8_t hui__canvas_btn_held    = 0; /* real physical button state */

/* Key-event queue: JS keydown fires asynchronously between C frames, so
 * begin_frame snapshots keys_prev=keys=1 and hui_key_pressed() always
 * returns false.  We latch each keydown into keys_queued[], then in
 * post_begin force keys_prev[k]=0 so widgets see a clean rising edge. */
static uint8_t hui__canvas_keys_queued[256];

/* Text-event queue: hui_begin_frame() clears text_typed_len=0, which erases
 * any chars that JS injected before the frame started.  We buffer them here
 * and copy into text_typed in post_begin, AFTER begin_frame has cleared it. */
static char hui__canvas_text_queue[32];
static int  hui__canvas_text_queue_len = 0;
/* Scroll deltas accumulate between frames; begin_frame clears io.scroll_d*, so
 * we queue here and restore in post_begin (same pattern as keys/text). */
static int  hui__canvas_scroll_dy_q = 0;
static int  hui__canvas_scroll_dx_q = 0;
/* Pixel-precise vertical scroll queue (touch pan / momentum + pixel-mode
 * trackpad wheels). Accumulated as float so sub-pixel deltas aren't lost,
 * drained into io.scroll_py as whole pixels with remainder carry. */
static float hui__canvas_scroll_py_q = 0.0f;

/* identical-frame skip state (see hui_backend_flush) — declared up here so
 * hui__canvas_resize can force real draws after the canvas is blanked. */
static uint64_t hui__canvas_prev_hash   = 0;
static int      hui__canvas_force_draws = 2;   /* init/resize must draw */
static uint8_t *hui__canvas_rgba        = NULL;
static size_t   hui__canvas_rgba_cap    = 0;

/* Drag-delta fix: JS updates mouse_x immediately (between frames), so
 * begin_frame snapshots prev=current → delta=0 every frame.
 * Solution: snapshot mouse_x at end-of-frame and restore it as prev
 * in post_begin, so delta = (new JS position) - (end-of-prev-frame position). */
static int16_t hui__canvas_eof_x = 0;
static int16_t hui__canvas_eof_y = 0;
/* Same problem as eof_x/y but for buttons: a mouseup that lands between frames
 * clears mouse_btn before begin_frame snapshots prev, so prev==cur==0 and the
 * down->up RELEASE edge (!lmb && lmb_prev) is invisible — drag-release handlers
 * (widget resize end, map pan end) can never fire in the browser. Snapshot the
 * button state widgets actually saw last frame and restore it as prev. */
static uint8_t hui__canvas_eof_btn = 0;

EMSCRIPTEN_KEEPALIVE void hui__canvas_mouse_btn(int btn, int down) {
    if (!hui_g) return;
    uint8_t mask = (uint8_t)(1u << btn);
    if (down) {
        hui_g->io.mouse_btn       |= mask;
        hui__canvas_click_queue   |= mask;
        hui__canvas_btn_held      |= mask;
    } else {
        hui_g->io.mouse_btn       &= (uint8_t)~mask;
        hui__canvas_btn_held      &= (uint8_t)~mask;
    }
}

/* Call at the start of each frame, BEFORE hui_begin_frame. */
static void hui__canvas_pre_frame(void) {
    if (!hui_g) return;
    if (hui__canvas_click_phase == 2) {
        /* Clear only bits that have been physically released (not still held).
         * Held bits must stay set so drag continues past phase-2. */
        uint8_t released = hui__canvas_click_queue & (uint8_t)~hui__canvas_btn_held;
        hui_g->io.mouse_btn    &= (uint8_t)~released;
        hui__canvas_click_queue = 0;
        hui__canvas_click_phase = 0;
    } else if (hui__canvas_click_queue && hui__canvas_click_phase == 0) {
        /* Temporarily clear queued bits so begin_frame snapshots prev=0. */
        hui__canvas_saved_btn  = hui_g->io.mouse_btn;
        hui_g->io.mouse_btn   &= (uint8_t)~hui__canvas_click_queue;
        hui__canvas_click_phase = 1;
    }
}

/* Call after hui_end_frame() — snapshot mouse position so next frame's
 * post_begin can supply the correct drag prev. */
static void hui__canvas_end_frame(void) {
    if (!hui_g) return;
    hui__canvas_eof_x   = hui_g->io.mouse_x;
    hui__canvas_eof_y   = hui_g->io.mouse_y;
    hui__canvas_eof_btn = hui_g->io.mouse_btn;   /* what widgets saw this frame */
}

/* Call AFTER hui_begin_frame (which snapshots prev). */
static void hui__canvas_post_begin(void) {
    if (!hui_g) return;
    if (hui__canvas_click_phase == 1) {
        /* Restore real state: widgets see prev=0, btn=1 → click. */
        hui_g->io.mouse_btn = hui__canvas_saved_btn | hui__canvas_click_queue;
        hui__canvas_click_phase = 2;
    }
    /* Restore drag-correct prev: JS events may have moved mouse between frames,
     * making begin_frame's prev=current (delta=0). Override with end-of-last-frame. */
    hui_g->io.mouse_x_prev = hui__canvas_eof_x;
    hui_g->io.mouse_y_prev = hui__canvas_eof_y;
    /* Restore the button-release edge: prev = what widgets saw last frame, so a
     * between-frames mouseup shows as prev=1,cur=0 for exactly one frame (see
     * eof_btn). This runs after the click-phase-1 restore above, so a synthesized
     * click still gets its down edge this frame and its up edge next frame. */
    hui_g->io.mouse_btn_prev = hui__canvas_eof_btn;

    /* Key-event queue: for each latched keydown, force keys_prev[k]=0 so
     * hui_key_pressed() sees a clean rising edge regardless of when JS fired. */
    for (int k = 0; k < 256; k++) {
        if (hui__canvas_keys_queued[k]) {
            hui_g->io.keys[k]      = 1;
            hui_g->io.keys_prev[k] = 0;
            hui__canvas_keys_queued[k] = 0;
        }
    }

    /* Text-event queue: drain into text_typed AFTER begin_frame cleared it. */
    for (int i = 0; i < hui__canvas_text_queue_len && hui_g->io.text_typed_len < 31; i++) {
        hui_g->io.text_typed[hui_g->io.text_typed_len++] = hui__canvas_text_queue[i];
    }
    hui__canvas_text_queue_len = 0;

    /* Scroll queue: restore accumulated wheel delta after begin_frame cleared it. */
    if (hui__canvas_scroll_dy_q) { hui_g->io.scroll_dy = (int16_t)hui__canvas_scroll_dy_q; hui__canvas_scroll_dy_q = 0; }
    if (hui__canvas_scroll_dx_q) { hui_g->io.scroll_dx = (int16_t)hui__canvas_scroll_dx_q; hui__canvas_scroll_dx_q = 0; }
    /* Pixel scroll queue: drain whole pixels, carry the sub-pixel remainder. */
    if (hui__canvas_scroll_py_q >= 1.0f || hui__canvas_scroll_py_q <= -1.0f) {
        int whole = (int)hui__canvas_scroll_py_q;
        if (whole >  4000) whole =  4000;
        if (whole < -4000) whole = -4000;
        hui_g->io.scroll_py = (int16_t)whole;
        hui__canvas_scroll_py_q -= (float)whole;
    }
}

/* Scroll wheel — positive = scroll up, negative = scroll down. Queued (a wheel
 * event fires between frames; begin_frame would clear io.scroll_dy first). */
EMSCRIPTEN_KEEPALIVE void hui__canvas_scroll(float dy) {
    if (!hui_g) return;
    hui__canvas_scroll_dy_q += (dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0));
}

/* Horizontal scroll — positive = scroll right, negative = scroll left. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_scroll_x(float dx) {
    if (!hui_g) return;
    hui__canvas_scroll_dx_q += (dx > 0.0f ? 1 : (dx < 0.0f ? -1 : 0));
}

/* Pixel-precise vertical scroll (+down/-up): touch pan/momentum and
 * pixel-mode (deltaMode 0) wheels — trackpads. Accumulates; post_begin
 * drains whole pixels into io.scroll_py with sub-pixel carry. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_scroll_px(float dy_px) {
    if (!hui_g) return;
    if (dy_px > 300.0f)  dy_px = 300.0f;    /* one event can't teleport the view */
    if (dy_px < -300.0f) dy_px = -300.0f;
    hui__canvas_scroll_py_q += dy_px;
}

/* Key down/up — code is the translated hui key constant (from
 * hui__canvas_keycode_str). */
EMSCRIPTEN_KEEPALIVE void hui__canvas_key(int code, int down) {
    if (!hui_g || (unsigned)code >= 256) return;
    hui_g->io.keys[code] = (bool)down;
    if (down) hui__canvas_keys_queued[code] = 1; /* latch for post_begin */
}

/* Printable text typed (single UTF-8 character or short string).
 * Called from the JS keydown handler for event.key.length === 1.
 * Appends to a queue so begin_frame()'s text_typed_len=0 clear doesn't
 * discard chars that fired before the frame started. post_begin() drains it. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_text(const char *s) {
    if (!s) return;
    int len = 0;
    while (s[len] && hui__canvas_text_queue_len < 31) {
        hui__canvas_text_queue[hui__canvas_text_queue_len++] = s[len++];
    }
}

/* Touch point update — raw slot bookkeeping only.
 * idx: slot 0-4; x,y: CSS pixels; active: 1=down 0=up; id: touch identifier.
 * NO mouse synthesis here: the JS gesture layer (see setup_events) classifies
 * tap vs pan and sends explicit clicks / pixel scrolls — a held finger must
 * never read as a held LMB (that made every touch drag a text selection). */
EMSCRIPTEN_KEEPALIVE void hui__canvas_touch(int idx, int x, int y,
                                             int active, unsigned id) {
    if (!hui_g || idx < 0 || idx >= 5) return;
    hui_g->io.touches[idx].x      = (int16_t)x;
    hui_g->io.touches[idx].y      = (int16_t)y;
    hui_g->io.touches[idx].id     = (uint32_t)id;
    hui_g->io.touches[idx].active = (bool)active;
}

/* Canvas resized (CSS pixel dimensions).
 * Framebuffer is re-allocated at physical resolution (css × DPR, capped) and
 * hui__fb_scale updated — browser zoom changes devicePixelRatio and lands
 * here, so zooming re-rasterizes at the new native resolution instead of
 * letting the browser rescale a stale bitmap.  Widget coordinates and mouse
 * input stay in CSS pixels. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_resize(int css_w, int css_h) {
    /* the canvas element is blanked by its own resize — force real draws
       until fresh content lands (the identical-frame skip must not hold the
       old hash against a now-blank canvas) */
    hui__canvas_force_draws = 2;
    hui__canvas_dpr = (float)EM_ASM_DOUBLE({ return window.devicePixelRatio || 1.0; });
    if (hui__canvas_dpr > 3.0f) hui__canvas_dpr = 3.0f;   /* integer-DPR phones render 1:1 — a 2.0 cap made DPR-3 screens upscale 1.5x with nearest-neighbor, ratcheting every hairline */
    if (hui__canvas_dpr < 0.25f) hui__canvas_dpr = 0.25f;
    hui__canvas_css_w  = css_w;
    hui__canvas_css_h  = css_h;
    hui__canvas_phys_w = (int)(css_w * hui__canvas_dpr + 0.5f);
    hui__canvas_phys_h = (int)(css_h * hui__canvas_dpr + 0.5f);

    /* Resize the pixel buffer to physical dimensions. */
    hui__fb_scale = hui__canvas_dpr;
    free(hui__fb.pixels);
    hui__fb.w = hui__canvas_phys_w; hui__fb.h = hui__canvas_phys_h;
    hui__fb.pixels = (uint32_t *)calloc(
        (size_t)(hui__canvas_phys_w * hui__canvas_phys_h), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = hui__canvas_phys_w; hui__fb.clip_y1 = hui__canvas_phys_h;
    hui__fb.clip_depth = 0;

    /* Visible canvas + FB canvas + ImageData all at physical pixels;
     * CSS style keeps the layout size. */
    {
        int _cw = css_w, _ch = css_h;
        int _pw = hui__canvas_phys_w, _ph = hui__canvas_phys_h;
        EM_ASM({
            var canvas = document.getElementById('canvas');
            if (!canvas) return;
            var cw = $0; var ch = $1; var pw = $2; var ph = $3;
            canvas.width        = pw;
            canvas.height       = ph;
            canvas.style.width  = cw + 'px';
            canvas.style.height = ch + 'px';
            var ctx = canvas.getContext('2d');
            ctx.imageSmoothingEnabled = false;
            Module._hui_canvas_2d_ctx = ctx;

            if (Module._hui_fb_canvas) {
                Module._hui_fb_canvas.width  = pw;
                Module._hui_fb_canvas.height = ph;
                var fctx = Module._hui_fb_canvas.getContext('2d');
                fctx.imageSmoothingEnabled = false;
                Module._hui_fb_ctx    = fctx;
                Module._hui_imagedata = fctx.createImageData(pw, ph);
            }
        }, _cw, _ch, _pw, _ph);
    }

    if (hui_g) {
        hui_g->screen_w = (int16_t)css_w;
        hui_g->screen_h = (int16_t)css_h;
        hui_g->io.dpr   = hui__canvas_dpr;   /* app watches this to re-bake fonts */
    }
}

/* JS calls this with the string e.code to get back the hui key constant.
 * Returning 0 means "not a key we care about". */
EMSCRIPTEN_KEEPALIVE int hui__canvas_keycode_str(const char *code) {
    return hui__canvas_keycode(code);
}

/* Called asynchronously by the JS clipboard promise resolver. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_clipboard_result(const char *text) {
    if (hui__clip_cb) {
        hui__clip_cb(text, hui__clip_user);
        hui__clip_cb   = NULL;
        hui__clip_user = NULL;
    }
}

/* Called from the JS "paste" listener with the event's text/plain payload. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_clipboard_paste_text(const char *text) {
    free(hui__clip_paste_text); hui__clip_paste_text = NULL;
    if (!text || !text[0]) return;
    size_t n = strlen(text);
    hui__clip_paste_text = (char*)malloc(n + 1);
    if (hui__clip_paste_text) memcpy(hui__clip_paste_text, text, n + 1);
}

/* Called from the JS "paste" listener with an image item's encoded bytes. */
EMSCRIPTEN_KEEPALIVE void hui__canvas_clipboard_paste_image(const unsigned char *data, int len) {
    free(hui__clip_paste_img); hui__clip_paste_img = NULL; hui__clip_paste_img_len = 0;
    if (!data || len <= 0) return;
    hui__clip_paste_img = (unsigned char*)malloc((size_t)len);
    if (!hui__clip_paste_img) return;
    memcpy(hui__clip_paste_img, data, (size_t)len);
    hui__clip_paste_img_len = len;
}

char *hui_canvas_clipboard_take_text(void) {
    char *t = hui__clip_paste_text; hui__clip_paste_text = NULL; return t;
}
unsigned char *hui_canvas_clipboard_take_image(int *len_out) {
    unsigned char *d = hui__clip_paste_img; int n = hui__clip_paste_img_len;
    hui__clip_paste_img = NULL; hui__clip_paste_img_len = 0;
    if (len_out) *len_out = d ? n : 0;
    return d;
}
int hui_canvas_clipboard_pending(void) {
    return (hui__clip_paste_text != NULL) || (hui__clip_paste_img != NULL);
}

/* ---- Canvas init --------------------------------------------------------- */

/* hui_headless_init: w/h are CSS pixel dimensions.
 * The framebuffer is allocated at PHYSICAL pixels (css × DPR, DPR capped at
 * 2.0 for software-rasterizer cost) and hui__fb_scale carries the ratio: the
 * rasterizer scales command coordinates, hui_ttf_text draws via each font's
 * hires companion.  App-side C coordinate spaces (layout, mouse, hit-testing)
 * stay in CSS pixels — same image, rasterized at native resolution, so
 * putImageData is 1:1 and the browser never rescales (crisp at any zoom). */
static void hui_headless_init(int w, int h) {
    hui__canvas_dpr = (float)EM_ASM_DOUBLE({
        return window.devicePixelRatio || 1.0;
    });
    if (hui__canvas_dpr > 3.0f) hui__canvas_dpr = 3.0f;   /* integer-DPR phones render 1:1 — a 2.0 cap made DPR-3 screens upscale 1.5x with nearest-neighbor, ratcheting every hairline */
    if (hui__canvas_dpr < 0.25f) hui__canvas_dpr = 0.25f;
    hui__canvas_css_w  = w;
    hui__canvas_css_h  = h;
    hui__canvas_phys_w = (int)(w * hui__canvas_dpr + 0.5f);
    hui__canvas_phys_h = (int)(h * hui__canvas_dpr + 0.5f);

    /* Pixel buffer at physical resolution; rasterizer scales coords. */
    hui__fb_scale = hui__canvas_dpr;
    hui__fb.w = hui__canvas_phys_w; hui__fb.h = hui__canvas_phys_h;
    hui__fb.pixels = (uint32_t *)calloc(
        (size_t)(hui__canvas_phys_w * hui__canvas_phys_h), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = hui__canvas_phys_w; hui__fb.clip_y1 = hui__canvas_phys_h;
    hui__fb.clip_depth = 0;

    {
    int _pw = hui__canvas_phys_w, _ph = hui__canvas_phys_h;
    EM_ASM({
        /* Visible canvas — physical pixel buffer, CSS-pixel styled. */
        var canvas = document.getElementById('canvas');
        if (!canvas) {
            canvas = document.createElement('canvas');
            canvas.id = 'canvas';
            document.body.appendChild(canvas);
            document.body.style.margin     = '0';
            document.body.style.overflow   = 'hidden';
            document.body.style.background = '#000';
        }
        var cw = $0; var ch = $1; var pw = $2; var ph = $3;
        canvas.width        = pw;
        canvas.height       = ph;
        canvas.style.width  = cw + 'px';
        canvas.style.height = ch + 'px';
        var ctx = canvas.getContext('2d');
        ctx.imageSmoothingEnabled = false;
        Module._hui_canvas_2d_ctx = ctx;

        /* Hidden offscreen FB canvas — physical resolution: putImageData at
         * 1:1, drawImage to the (equal-sized) visible canvas never scales. */
        var fb = document.createElement('canvas');
        fb.width  = pw;
        fb.height = ph;
        var fctx = fb.getContext('2d');
        fctx.imageSmoothingEnabled = false;
        Module._hui_fb_canvas  = fb;
        Module._hui_fb_ctx     = fctx;
        Module._hui_imagedata  = fctx.createImageData(pw, ph);
    }, w, h, _pw, _ph);
    }

    hui__canvas_setup_events();
}

/* ---- Canvas free --------------------------------------------------------- */

static void hui_headless_free(void) {
    free(hui__fb.pixels);
    hui__fb.pixels = NULL;
}

/* ---- Canvas resize ------------------------------------------------------- */

static void hui_headless_resize(int w, int h) {
    hui__canvas_resize(w, h);
}

/* ---- hui_backend_flush --------------------------------------------------- */

/* Called by hui_end_frame().
 * 1. Runs the headless software rasterizer (fills hui__fb.pixels in ARGB).
 * 2. Converts ARGB uint32 → RGBA bytes and copies into JS ImageData.
 * 3. Calls putImageData to update the visible canvas.
 * 4. Clears the pixel buffer for the next frame. */
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
#ifndef HUI_CANVAS_NO_FRAME_SKIP
    /* ---- identical-frame skip ----
     * The flush is by far the most expensive per-frame work in the browser:
     * a full-physical-resolution software rasterize, a per-pixel ARGB→RGBA
     * conversion, and a putImageData upload — every rAF tick, forever, even
     * on a completely static page. Hash the frame's full input (command
     * stream, string pool, float datapool, framebuffer dims, plus the app's
     * overlay queue via the weak g_overlay_hash_hook) and skip ALL of it
     * when the frame is pixel-identical to what the canvas already shows.
     * A spinning map or any animation changes the inputs, so motion is
     * never frozen. */
    {
        uint64_t h = 1469598103934665603ULL;
#define HUI__FNV(P, N) do { const uint8_t *_b = (const uint8_t *)(P); \
        for (size_t _i = 0; _i < (size_t)(N); _i++) { h ^= _b[_i]; h *= 1099511628211ULL; } \
    } while (0)
        HUI__FNV(cmds, (size_t)count * sizeof(hui_cmd));
        if (hui_g) {
            HUI__FNV(strpool,  hui_g->dl.strpool_len);
            HUI__FNV(datapool, (size_t)hui_g->dl.datapool_len * sizeof(float));
        }
#undef HUI__FNV
        h ^= ((uint64_t)(uint32_t)hui__fb.w << 32) | (uint32_t)hui__fb.h;
        {   extern __attribute__((weak)) uint64_t (*g_overlay_hash_hook)(void);
            if (&g_overlay_hash_hook && g_overlay_hash_hook)
                h ^= g_overlay_hash_hook();
        }
        if (h == hui__canvas_prev_hash && hui__canvas_force_draws <= 0) {
            /* pixel-identical to what the canvas already shows. The app's
               overlay queue was still filled this frame — run its finish
               hook so it resets exactly as a drawn frame would. */
            extern __attribute__((weak)) void (*g_overlay_finish_hook)(void);
            if (&g_overlay_finish_hook && g_overlay_finish_hook)
                g_overlay_finish_hook();
            return;
        }
        hui__canvas_prev_hash = h;
        if (hui__canvas_force_draws > 0) hui__canvas_force_draws--;
    }
#endif /* HUI_CANVAS_NO_FRAME_SKIP */
#ifdef HUI_CANVAS_TEXT_OVERLAY
    /* For vector-quality text: NOP all TEXT commands before rasterizing so
     * the bitmap font is skipped.  We re-render them with Canvas2D fillText
     * after putImageData.  Cast away const — safe, we restore below. */
    hui_cmd *mutable_cmds = (hui_cmd *)(uintptr_t)cmds;
    for (uint16_t _ti = 0; _ti < count; _ti++) {
        if (mutable_cmds[_ti].type == HUI_CMD_TEXT)
            mutable_cmds[_ti].type = HUI_CMD_NOP;
    }
#endif
    /* Rasterize into the uint32 ARGB pixel buffer. */
    hui__rasterize(cmds, count, strpool, datapool);

    /* Optional overlay pass — app defines g_overlay_hook to draw TTF text into
       the framebuffer after rasterization, before blit (parity with hui_x11). */
    { extern __attribute__((weak)) void (*g_overlay_hook)(void);
      if (g_overlay_hook) g_overlay_hook(); }

    int n_px = hui__fb.w * hui__fb.h;

    /* RGBA byte buffer for ImageData (RGBA order; hui__fb.pixels is
     * 0xAARRGGBB). Static + grown on demand — a per-frame malloc/free of a
     * multi-megabyte buffer was pure churn. */
    if ((size_t)n_px * 4 > hui__canvas_rgba_cap) {
        free(hui__canvas_rgba);
        hui__canvas_rgba = (uint8_t *)malloc((size_t)n_px * 4);
        hui__canvas_rgba_cap = hui__canvas_rgba ? (size_t)n_px * 4 : 0;
    }
    uint8_t *rgba = hui__canvas_rgba;
    if (rgba) {
        for (int i = 0; i < n_px; i++) {
            uint32_t px = hui__fb.pixels[i];
            rgba[i*4 + 0] = (uint8_t)((px >> 16) & 0xFF); /* R */
            rgba[i*4 + 1] = (uint8_t)((px >>  8) & 0xFF); /* G */
            rgba[i*4 + 2] = (uint8_t)( px        & 0xFF); /* B */
            rgba[i*4 + 3] = (uint8_t)((px >> 24) & 0xFF); /* A */
        }

        {
            int _ptr = (int)(uintptr_t)rgba;
            int _n   = n_px * 4;
            int _pw  = hui__canvas_phys_w;
            int _ph  = hui__canvas_phys_h;
            EM_ASM({
                var ptr = $0; var n = $1; var pw = $2; var ph = $3;
                /* 1. Fill CSS-pixel ImageData in the hidden FB canvas. */
                var id = Module._hui_imagedata;
                id.data.set(Module.HEAPU8.subarray(ptr, ptr + n));
                Module._hui_fb_ctx.putImageData(id, 0, 0);
                /* 2. Draw (nearest-neighbor scale) onto the physical-pixel visible canvas. */
                var ctx = Module._hui_canvas_2d_ctx;
                ctx.imageSmoothingEnabled = false;
                ctx.drawImage(Module._hui_fb_canvas, 0, 0, pw, ph);
                /* instrumentation: real blits this session (identical-frame
                 * skips don't land here) — read Module._hui_blits in devtools */
                Module._hui_blits = (Module._hui_blits || 0) + 1;
            }, _ptr, _n, _pw, _ph);
        }
    }

    /* Clear pixel buffer for next frame. */
    memset(hui__fb.pixels, 0, (size_t)n_px * sizeof(uint32_t));

#ifdef HUI_CANVAS_TEXT_OVERLAY
    /* Restore TEXT commands and render each one with Canvas2D fillText
     * (Courier New vector font — matches a dashboard-grade quality). */
    {
/* Rendered font size for Canvas2D text overlay.  Defaults to the C-layout
 * font size (HUI_FONT_H * HUI_FONT_SCALE).  Override via compiler flag,
 * e.g. -DHUI_CANVAS_FONT_PX=14, to render larger text without changing the
 * C-side layout grid.  A y-nudge re-centres the larger glyph in the row. */
#ifndef HUI_CANVAS_FONT_PX
#  define HUI_CANVAS_FONT_PX (HUI_FONT_H * HUI_FONT_SCALE)
#endif
        /* Font size in CSS pixels; rendered on the physical-pixel visible canvas
         * via DPR-scaled transform so text coordinates stay in CSS pixel space.
         * _y_nudge shifts text up to re-centre it when font > C-layout height. */
        double _fs_css  = (double)(HUI_CANVAS_FONT_PX);
        double _y_nudge = (_fs_css - (double)(HUI_FONT_H * HUI_FONT_SCALE)) * 0.5;
        double _dpr     = (double)hui__canvas_dpr;
        for (uint16_t _ti = 0; _ti < count; _ti++) {
            if (cmds[_ti].type != HUI_CMD_NOP) continue;
            /* Restore TEXT commands (the only NOPs we injected). */
            mutable_cmds[_ti].type = HUI_CMD_TEXT;
        }
        /* Set DPR transform once before the text pass. */
        EM_ASM({ Module._hui_canvas_2d_ctx.setTransform($0, 0, 0, $0, 0, 0); }, _dpr);
        for (uint16_t _ti = 0; _ti < count; _ti++) {
            if (cmds[_ti].type != HUI_CMD_TEXT) continue;
            const char *_txt = strpool + (int)cmds[_ti].x2;
            if (!_txt || !_txt[0]) continue;
            double _x   = (double)cmds[_ti].x0;
            double _y   = (double)cmds[_ti].y0 - _y_nudge;
            double _r   = (double)cmds[_ti].col_r / 255.0;
            double _g   = (double)cmds[_ti].col_g / 255.0;
            double _b   = (double)cmds[_ti].col_b / 255.0;
            double _a   = (double)cmds[_ti].col_a / 255.0;
            int    _ptr = (int)(uintptr_t)_txt;
            EM_ASM({
                var ctx = Module._hui_canvas_2d_ctx;
                var fs  = $7;
                var r = Math.round($3 * 255);
                var g = Math.round($4 * 255);
                var b = Math.round($5 * 255);
                var a = $6;
                var col = 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
                ctx.globalAlpha              = 1.0;
                ctx.globalCompositeOperation = 'source-over';
                ctx.font                     = fs + 'px \'Courier New\', monospace';
                ctx.textBaseline             = 'top';
                ctx.fillStyle                = col;
                ctx.fillText(UTF8ToString($8), $0, $1);
            }, _x, _y, 0.0, _r, _g, _b, _a, _fs_css, _ptr);
        }
        /* Reset transform to identity after text pass. */
        EM_ASM({ Module._hui_canvas_2d_ctx.setTransform(1, 0, 0, 1, 0, 0); });
    }
#endif /* HUI_CANVAS_TEXT_OVERLAY */
}

/* ---- JS event setup ------------------------------------------------------ */

/* Registers all browser event listeners.  Uses Module._hui__canvas_*
 * to call back into the EMSCRIPTEN_KEEPALIVE C functions above.
 * This must be called after the canvas element exists (end of hui_headless_init). */
static void hui__canvas_setup_events(void) {
    EM_ASM({
        var canvas = document.getElementById('canvas');

        /* ---- Mouse ---- */
        /* mousemove + mouseup on document so drags that leave the canvas
         * boundary still track correctly — critical for LMB pan. */
        var _btnDown = 0;
        document.addEventListener('mousemove', function(e) {
            if (!Module._hui__canvas_mouse_move) return;
            var r = canvas.getBoundingClientRect();
            Module._hui__canvas_mouse_move(
                Math.round(e.clientX - r.left),
                Math.round(e.clientY - r.top));
            if (_btnDown) e.preventDefault();
        });

        /* Button events also carry coordinates — sync the pointer position
         * before reporting the button. A synthetic click (automation, some
         * touch/trackpad taps, first click after load) arrives with NO
         * preceding mousemove, so without this the press hit-tests at the
         * STALE pointer position — one click behind. */
        function _syncPos(e) {
            if (!Module._hui__canvas_mouse_move) return;
            var r = canvas.getBoundingClientRect();
            Module._hui__canvas_mouse_move(
                Math.round(e.clientX - r.left),
                Math.round(e.clientY - r.top));
        }

        canvas.addEventListener('mousedown', function(e) {
            canvas.focus();
            _syncPos(e);
            /* Remap browser button indices: 0=LMB,2=RMB,1=MMB → 0,1,2 */
            var btn = (e.button === 2) ? 1 : (e.button === 1) ? 2 : 0;
            _btnDown |= (1 << btn);
            Module._hui__canvas_mouse_btn(btn, 1);
            e.preventDefault();
        });

        document.addEventListener('mouseup', function(e) {
            if (!Module._hui__canvas_mouse_btn) return;
            _syncPos(e);
            var btn = (e.button === 2) ? 1 : (e.button === 1) ? 2 : 0;
            _btnDown &= ~(1 << btn);
            Module._hui__canvas_mouse_btn(btn, 0);
        });

        /* Suppress right-click browser context menu on canvas. */
        canvas.addEventListener('contextmenu', function(e) {
            e.preventDefault();
        });

        /* ---- Wheel ---- */
        canvas.addEventListener('wheel', function(e) {
            /* a wheel event carries a position — sync it like button events
             * do (X11 wheel implies pointer position too): scrolling right
             * after load / a click elsewhere must target what's under the
             * wheel, not a stale pointer parked on some earlier widget */
            _syncPos(e);
            if (Math.abs(e.deltaY) >= Math.abs(e.deltaX)) {
                if (e.deltaMode === 0 && Module._hui__canvas_scroll_px) {
                    /* Pixel-mode devices (trackpads, precision wheels):
                     * forward the raw pixel delta — smooth 1:1 scrolling
                     * instead of quantizing every event to a full step. */
                    Module._hui__canvas_scroll_px(e.deltaY);
                } else {
                    /* Line/page-mode wheels keep the coarse step path.
                     * Invert so positive = scroll up (hui convention). */
                    var scale = (e.deltaMode === 1) ? 1.0 : 3.0;
                    Module._hui__canvas_scroll(-e.deltaY * scale);
                }
            } else {
                var xscale = (e.deltaMode === 0) ? 1.0/100.0 : (e.deltaMode === 1) ? 1.0 : 3.0;
                /* Second/horizontal wheel: positive dx → scroll right. */
                Module._hui__canvas_scroll_x(e.deltaX * xscale);
            }
            e.preventDefault();
        }, {passive: false});

        /* ---- Keyboard ---- */
        /* Registered on document so the canvas does not need focus for keys. */
        document.addEventListener('keydown', function(e) {
            /* Translate KeyboardEvent.code → hui key constant. */
            var len = Module.lengthBytesUTF8(e.code) + 1;
            var ptr = Module._malloc(len);
            Module.stringToUTF8(e.code, ptr, len);
            var code = Module._hui__canvas_keycode_str(ptr);
            Module._free(ptr);
            if (code) {
                Module._hui__canvas_key(code, 1);
            }
            /* Printable text: single visible character via event.key. */
            if (e.key.length === 1) {
                var blen = Module.lengthBytesUTF8(e.key) + 1;
                var bptr = Module._malloc(blen);
                Module.stringToUTF8(e.key, bptr, blen);
                Module._hui__canvas_text(bptr);
                Module._free(bptr);
            }
            /* Prevent Tab from moving browser focus away from canvas. */
            if (e.code === 'Tab') e.preventDefault();
        });

        /* ---- Paste (Ctrl+V / Cmd+V) ---- */
        /* The paste event carries the clipboard without a permission prompt.
         * Images are parked as their encoded bytes; text as UTF-8. The C side
         * picks them up on a later frame (see hui_canvas_clipboard_take_*). */
        document.addEventListener('paste', function(e) {
            var cd = e.clipboardData; if (!cd) return;
            var handled = false;
            var items = cd.items || [];
            for (var i = 0; i < items.length; i++) {
                var it = items[i];
                if (it.kind === 'file' && it.type.indexOf('image/') === 0) {
                    var blob = it.getAsFile(); if (!blob) continue;
                    handled = true;
                    blob.arrayBuffer().then(function(buf) {
                        var bytes = new Uint8Array(buf);
                        var ptr = Module._malloc(bytes.length);
                        Module.HEAPU8.set(bytes, ptr);
                        Module._hui__canvas_clipboard_paste_image(ptr, bytes.length);
                        Module._free(ptr);
                    });
                    break;
                }
            }
            if (!handled) {
                var text = cd.getData('text/plain');
                if (text && text.length) {
                    var len = Module.lengthBytesUTF8(text) + 1;
                    var ptr = Module._malloc(len);
                    Module.stringToUTF8(text, ptr, len);
                    Module._hui__canvas_clipboard_paste_text(ptr);
                    Module._free(ptr);
                    handled = true;
                }
            }
            if (handled) e.preventDefault();
        });

        document.addEventListener('keyup', function(e) {
            var len = Module.lengthBytesUTF8(e.code) + 1;
            var ptr = Module._malloc(len);
            Module.stringToUTF8(e.code, ptr, len);
            var code = Module._hui__canvas_keycode_str(ptr);
            Module._free(ptr);
            if (code) {
                Module._hui__canvas_key(code, 0);
            }
        });

        /* ---- Touch: gesture layer ----
         * A finger is NOT a mouse. Raw synthesis (old behavior: primary
         * touch = LMB held) made every drag a text-selection sweep and made
         * scrolling impossible. Instead, classify:
         *   TAP  (no move past slop, short) → one clean synthesized click
         *         at the touch point (the click queue makes fast taps and
         *         double-taps register); the app's own single-click=preview /
         *         double-click=open behaviors just work.
         *   PAN  (moved past slop)          → pixel scroll that follows the
         *         finger 1:1 (scroll_px channel), with a decaying momentum
         *         fling on release. Never presses the button → touch can
         *         never arm text selection or drag widgets accidentally.
         * The second finger is ignored for now (pinch zoom = v2); slots are
         * still forwarded to io.touches for future gestures. */
        /* NOTE: EM_ASM is a C macro — commas inside BRACES split macro args
         * (only parens protect), so object literals are parenthesized and
         * multi-var declarations are split one per statement. */
        /* sel: long-press (no move past slop before the timer) enters
         * SELECTION mode — the press point becomes a held LMB and further
         * finger movement drag-selects through the app's normal mouse
         * path; lift releases. The standard mobile select gesture. */
        var _tch = ({ active: false, panning: false, sel: false,
                      id: -1, x0: 0, y0: 0, lx: 0, ly: 0, t0: 0, lt: 0,
                      vy: 0, momRaf: 0, lpTimer: 0 });
        function _tchStopMomentum() {
            if (_tch.momRaf) { cancelAnimationFrame(_tch.momRaf); _tch.momRaf = 0; }
        }
        function _tchPos(t) {
            var r = canvas.getBoundingClientRect();
            return ({ x: Math.round(t.clientX - r.left),
                      y: Math.round(t.clientY - r.top) });
        }
        canvas.addEventListener('touchstart', function(e) {
            _tchStopMomentum();
            var t = e.changedTouches[0];
            if (!_tch.active && t) {
                var p = _tchPos(t);
                _tch.active = true; _tch.panning = false; _tch.sel = false;
                _tch.id = t.identifier;
                _tch.x0 = _tch.lx = p.x; _tch.y0 = _tch.ly = p.y;
                _tch.t0 = _tch.lt = e.timeStamp;
                _tch.vy = 0;
                /* park the pointer at the touch point so a tap hit-tests
                 * where the finger is, and hover state follows the tap */
                Module._hui__canvas_mouse_move(p.x, p.y);
                Module._hui__canvas_touch(0, p.x, p.y, 1, t.identifier);
                /* long-press (finger stays inside the slop) → selection */
                if (_tch.lpTimer) clearTimeout(_tch.lpTimer);
                _tch.lpTimer = setTimeout(function () {
                    _tch.lpTimer = 0;
                    if (_tch.active && !_tch.panning && !_tch.sel) {
                        _tch.sel = true;
                        Module._hui__canvas_mouse_move(_tch.lx, _tch.ly);
                        Module._hui__canvas_mouse_btn(0, 1);   /* held press */
                    }
                }, 380);
            }
            e.preventDefault();
        }, {passive: false});

        canvas.addEventListener('touchmove', function(e) {
            if (!_tch.active) return;
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i];
                if (t.identifier !== _tch.id) continue;
                var p = _tchPos(t);
                if (_tch.sel) {
                    /* selection mode: the finger IS a held-button mouse —
                     * plain moves extend the selection via the normal path */
                    Module._hui__canvas_mouse_move(p.x, p.y);
                    _tch.lx = p.x; _tch.ly = p.y; _tch.lt = e.timeStamp;
                    Module._hui__canvas_touch(0, p.x, p.y, 1, t.identifier);
                    e.preventDefault();
                    return;
                }
                var dx = p.x - _tch.x0;
                var dy0 = p.y - _tch.y0;
                if (!_tch.panning && (dx*dx + dy0*dy0) > 64) { /* 8px slop */
                    _tch.panning = true;
                    if (_tch.lpTimer) { clearTimeout(_tch.lpTimer); _tch.lpTimer = 0; }
                }
                if (_tch.panning) {
                    var dy = p.y - _tch.ly;
                    /* finger down = content follows = view scrolls up:
                     * scroll_px is +down, so send the negated finger delta */
                    Module._hui__canvas_scroll_px(-dy);
                    var dt = Math.max(1, e.timeStamp - _tch.lt);
                    _tch.vy = 0.8 * _tch.vy + 0.2 * (dy / dt);
                }
                _tch.lx = p.x; _tch.ly = p.y; _tch.lt = e.timeStamp;
                Module._hui__canvas_touch(0, p.x, p.y, 1, t.identifier);
            }
            e.preventDefault();
        }, {passive: false});

        function _tchEnd(e, cancelled) {
            if (!_tch.active) return;
            for (var i = 0; i < e.changedTouches.length; i++) {
                var t = e.changedTouches[i];
                if (t.identifier !== _tch.id) continue;
                _tch.active = false;
                if (_tch.lpTimer) { clearTimeout(_tch.lpTimer); _tch.lpTimer = 0; }
                Module._hui__canvas_touch(0, 0, 0, 0, 0);
                if (_tch.sel) {
                    /* lift ends the selection drag — release the held press
                     * (also on cancel: a held phantom button would eat every
                     * later tap) */
                    _tch.sel = false;
                    Module._hui__canvas_mouse_btn(0, 0);
                    return;
                }
                if (cancelled) return;
                if (!_tch.panning) {
                    if (e.timeStamp - _tch.t0 < 600) {
                        /* TAP → clean click at the touch point */
                        Module._hui__canvas_mouse_move(_tch.lx, _tch.ly);
                        Module._hui__canvas_mouse_btn(0, 1);
                        Module._hui__canvas_mouse_btn(0, 0);
                    }
                    return;
                }
                /* PAN release → momentum fling while velocity lasts */
                var v = _tch.vy;                  /* px/ms, finger direction */
                if (Math.abs(v) < 0.25) return;
                if (v >  3.5) v =  3.5;
                if (v < -3.5) v = -3.5;
                var last = performance.now();
                function _mom(now) {
                    var dt = Math.min(50, now - last); last = now;
                    Module._hui__canvas_scroll_px(-v * dt);
                    v *= Math.pow(0.994, dt);     /* exponential decay */
                    if (Math.abs(v) > 0.04)
                        _tch.momRaf = requestAnimationFrame(_mom);
                    else _tch.momRaf = 0;
                }
                _tch.momRaf = requestAnimationFrame(_mom);
            }
        }
        canvas.addEventListener('touchend',    function(e) { _tchEnd(e, false); });
        canvas.addEventListener('touchcancel', function(e) { _tchEnd(e, true);  });

        /* ---- Window resize ---- */
        window.addEventListener('resize', function() {
            Module._hui__canvas_resize(
                Math.round(window.innerWidth),
                Math.round(window.innerHeight));
        });

        /* Make canvas focusable for keyboard events that target it directly. */
        canvas.setAttribute('tabindex', '0');

    });
}

/* ---- Frame loop ---------------------------------------------------------- */

/* hui_set_frame_fn is implemented in hui.h (HUI_IMPLEMENTATION) and stores
 * the callback in hui__frame_fn / hui__frame_user statics.  We reference
 * those statics here because this file is #include-d from within hui.h
 * in the same translation unit. */

void hui_run(void) {
    /* hui__frame_fn and hui__frame_user are the statics from hui.h.
     * emscripten_set_main_loop_arg expects em_arg_callback_func which
     * is typedef void (*)(void*) — identical to hui__frame_fn's type. */
#ifdef __EMSCRIPTEN__
    if (hui__frame_fn)
        emscripten_set_main_loop_arg(
            (em_arg_callback_func)hui__frame_fn, hui__frame_user, 0, 1);
#else
    /* Non-Emscripten fallback (syntax check / desktop builds):
     * loop until hui_stop() sets hui__running to false. */
    hui__running = true;
    while (hui__running && hui__frame_fn)
        hui__frame_fn(hui__frame_user);
#endif
}

/* ---- Clipboard ----------------------------------------------------------- */

void hui_canvas_clipboard_set(const char *text) {
    if (!text) return;
    EM_ASM({
        var s = UTF8ToString($0);
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(s);
        }
    }, text);
}

void hui_canvas_clipboard_paste(hui_clipboard_cb cb, void *user) {
    hui__clip_cb   = cb;
    hui__clip_user = user;
    EM_ASM({
        if (navigator.clipboard && navigator.clipboard.readText) {
            navigator.clipboard.readText().then(function(text) {
                var len = lengthBytesUTF8(text) + 1;
                var buf = Module._malloc(len);
                stringToUTF8(text, buf, len);
                Module._hui__canvas_clipboard_result(buf);
                Module._free(buf);
            }).catch(function() {
                /* Permission denied or API error — call back with NULL. */
                Module._hui__canvas_clipboard_result(0);
            });
        } else {
            Module._hui__canvas_clipboard_result(0);
        }
    });
}

/* ---- PNG download -------------------------------------------------------- */

void hui_canvas_download_png(const char *filename) {
    int _fptr = filename ? (int)(uintptr_t)filename : 0;
    EM_ASM({
        var canvas = document.getElementById('canvas');
        if (!canvas) return;
        var name = ($0 !== 0) ? UTF8ToString($0) : 'hui_export.png';
        var link = document.createElement('a');
        link.download = name;
        link.href     = canvas.toDataURL('image/png');
        link.click();
    }, _fptr);
}

/* ---- Fullscreen + pointer lock ------------------------------------------ */

void hui_canvas_fullscreen_request(void) {
    EM_ASM({
        var canvas = document.getElementById('canvas');
        if (!canvas) return;
        if      (canvas.requestFullscreen)       canvas.requestFullscreen();
        else if (canvas.webkitRequestFullscreen) canvas.webkitRequestFullscreen();
        else if (canvas.mozRequestFullScreen)    canvas.mozRequestFullScreen();
    });
}

void hui_canvas_pointerlock_request(void) {
    EM_ASM({
        var canvas = document.getElementById('canvas');
        if (!canvas) return;
        if (canvas.requestPointerLock) {
            canvas.requestPointerLock();
        }
    });
}

#endif /* HUI_IMPLEMENTATION */

#endif /* HUI_CANVAS_H */
