/* backends/hui_matrix8x8.h — 8×8 WS2812/APA102 LED matrix backend for hui
 *
 * Select with: #define HUI_BACKEND_MATRIX8X8 before #include "hui.h"
 *
 * Configuration (define before including):
 *   HUI_MATRIX_TYPE_WS2812  (default) — single-wire NeoPixel protocol
 *   HUI_MATRIX_TYPE_APA102  — SPI Dotstar protocol
 *   HUI_MATRIX_BRIGHTNESS   — global brightness 0-255 (default 128)
 *   HUI_MATRIX_SERPENTINE   — 1 = alternating row direction (default 0)
 *
 * The internal render resolution is 8×8 pixels (virtual display).
 * Draw commands are issued at normal screen coordinates and downsampled.
 *
 * Platform responsibilities:
 *   WS2812 — implement: void hui_matrix_ws2812_write(const uint8_t *grb, int n)
 *     Must output n*3 bytes as WS2812 GRB at the correct timing (800 kHz).
 *     On RP2040/RP2350 this is typically one PIO state machine call.
 *     On AVR it is a timing-sensitive inline asm loop.
 *
 *   APA102 — implement: void hui_matrix_apa102_write(const uint8_t *grb, int n)
 *     Must send the APA102 frame:
 *       [0x00 0x00 0x00 0x00]              — start frame
 *       [0xFF r g b] × n  (NOTE: grb→rgb reorder done here)
 *       [0xFF × ceil(n/2) bytes]           — end frame
 *     over SPI (CPOL=0, CPHA=0, MSB first).
 */

#ifndef HUI_MATRIX8X8_H
#define HUI_MATRIX8X8_H

/* 8×8 matrix profile: AA makes no sense at this resolution; force off */
#ifndef HUI_NO_AA
#  define HUI_NO_AA
#endif

#include <stdint.h>
#include <string.h>

/* ---- Configuration ---- */

#define HUI_MATRIX_LEDS      64   /* 8×8 */
#define HUI_MATRIX_W          8
#define HUI_MATRIX_H          8

#ifndef HUI_MATRIX_BRIGHTNESS
#  define HUI_MATRIX_BRIGHTNESS 128
#endif
#ifndef HUI_MATRIX_SERPENTINE
#  define HUI_MATRIX_SERPENTINE 0
#endif

#define HUI_MATRIX_TYPE_WS2812 0
#define HUI_MATRIX_TYPE_APA102 1
#ifndef HUI_MATRIX_TYPE
#  define HUI_MATRIX_TYPE HUI_MATRIX_TYPE_WS2812
#endif

/* ---- Pull in headless rasterizer (init/free/resize suppressed) ---- */
#define HUI_HEADLESS_NO_INIT
#define HUI_HEADLESS_NO_FLUSH
#include "hui_headless.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- LED output buffer (GRB order — WS2812 native) ---- */
static uint8_t hui__matrix_buf[HUI_MATRIX_LEDS * 3];

/* ---- Platform output functions (user-provided, platform-specific) ---- */

/* WS2812: transmit n LEDs worth of GRB data (n*3 bytes).
 * The caller guarantees the buffer is GRB-ordered at 800 kHz bit timing.
 * On RP2040/RP2350: push bytes to a PIO state machine configured for WS2812.
 * On AVR: use a timing-exact ISR-disabled loop (see Adafruit NeoPixel). */
extern void hui_matrix_ws2812_write(const uint8_t *grb, int n);

/* APA102: transmit n LEDs in the full APA102 frame protocol.
 * The grb buffer is GRB-ordered; implementations must reorder to RGB
 * and prepend/append start and end frames over SPI.
 * See file header for the required frame layout. */
extern void hui_matrix_apa102_write(const uint8_t *grb, int n);

/* ---- IMU quaternion → LED grid index ---- */
/*
 * Convert IMU orientation quaternion to LED grid index.
 * Projects the device's "up" vector onto the 8×8 grid.
 *   col = clamp((vx + 1) * 4, 0, 7)
 *   row = clamp((vy + 1) * 4, 0, 7)
 * Returns grid_idx = row*8 + col  (0..63).
 *
 * Typical use: light a single LED to show tilt direction.
 *   int led = hui_imu_quat_to_led(imu_quat);
 *   hui__matrix_buf[led*3+0] = 0;    // G
 *   hui__matrix_buf[led*3+1] = 255;  // R
 *   hui__matrix_buf[led*3+2] = 0;    // B
 */
int hui_imu_quat_to_led(hui_v4 q);

#ifdef HUI_IMPLEMENTATION

int hui_imu_quat_to_led(hui_v4 q) {
    /* Rotate world-up (0, 1, 0) by quaternion q */
    hui_v3 up  = hui_quat_rotate(q, (hui_v3){0.0f, 1.0f, 0.0f});
    int    col = hui_clamp((int)((up.x + 1.0f) * 4.0f), 0, 7);
    int    row = hui_clamp((int)((up.y + 1.0f) * 4.0f), 0, 7);
    return row * 8 + col;
}

/* ---- Init / free / resize ---- */

/* Static RGBA pixel buffer for the 8×8 virtual framebuffer. */
static uint32_t hui__matrix_rgba[HUI_MATRIX_W * HUI_MATRIX_H];

static void hui_headless_init(int w, int h) {
    /* Virtual display is always 8×8; ignore caller's dimensions. */
    (void)w; (void)h;
    hui__fb.w       = HUI_MATRIX_W;
    hui__fb.h       = HUI_MATRIX_H;
    hui__fb.pixels  = hui__matrix_rgba;
    hui__fb.clip_x0 = 0;
    hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = HUI_MATRIX_W;
    hui__fb.clip_y1 = HUI_MATRIX_H;
    hui__fb.clip_depth = 0;
    memset(hui__matrix_rgba, 0, sizeof(hui__matrix_rgba));
    memset(hui__matrix_buf,  0, sizeof(hui__matrix_buf));
}

static void hui_headless_free(void) {
    /* Static buffers; nothing to free. */
    hui__fb.pixels = NULL;
}

static void hui_headless_resize(int w, int h) {
    /* Physical display is fixed 8×8 — ignore. */
    (void)w; (void)h;
}

/* ---- Serpentine (boustrophedon) index mapping ---- */
/* Standard (HUI_MATRIX_SERPENTINE=0): LED 0 is top-left, goes left→right,
 * wraps at each row end.
 * Serpentine (HUI_MATRIX_SERPENTINE=1): even rows go left→right, odd rows
 * go right→left — common in physical zig-zag wiring. */
static inline int hui__matrix_led_idx(int col, int row) {
#if HUI_MATRIX_SERPENTINE
    if (row & 1) {
        /* Odd row: right-to-left */
        return row * HUI_MATRIX_W + (HUI_MATRIX_W - 1 - col);
    }
#endif
    return row * HUI_MATRIX_W + col;
}

/* ---- Backend flush ---- */

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    int   i, row, col;
    const int scale = HUI_MATRIX_BRIGHTNESS;

    /* 1. Rasterize draw list into 8×8 RGBA framebuffer */
    hui__rasterize(cmds, count, strpool, datapool);

    /* 2. Convert each pixel: RGBA → GRB with brightness scaling,
     *    respecting serpentine mapping. */
    for (row = 0; row < HUI_MATRIX_H; row++) {
        for (col = 0; col < HUI_MATRIX_W; col++) {
            int        src = row * HUI_MATRIX_W + col;
            int        dst = hui__matrix_led_idx(col, row);
            uint32_t   px  = hui__fb.pixels[src];
            uint8_t    r   = (uint8_t)((px >> 16) & 0xFF);
            uint8_t    g   = (uint8_t)((px >>  8) & 0xFF);
            uint8_t    b   = (uint8_t)( px        & 0xFF);
            /* GRB order (WS2812 native), brightness-scaled */
            hui__matrix_buf[dst * 3 + 0] = (uint8_t)((g * scale) / 255);
            hui__matrix_buf[dst * 3 + 1] = (uint8_t)((r * scale) / 255);
            hui__matrix_buf[dst * 3 + 2] = (uint8_t)((b * scale) / 255);
        }
    }
    (void)i;  /* suppress unused-variable warning if loop var folded away */

    /* 3. Transmit to hardware */
#if HUI_MATRIX_TYPE == HUI_MATRIX_TYPE_APA102
    hui_matrix_apa102_write(hui__matrix_buf, HUI_MATRIX_LEDS);
#else
    hui_matrix_ws2812_write(hui__matrix_buf, HUI_MATRIX_LEDS);
#endif
}

#endif /* HUI_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_MATRIX8X8_H */
