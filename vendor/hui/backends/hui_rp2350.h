/* backends/hui_rp2350.h — RP2350 bare-metal SPI/PIO backend for hui
 *
 * Select with: #define HUI_BACKEND_RP2350 before #include "hui.h"
 *
 * Required SDK:
 *   pico_sdk_import.cmake with: pico_stdlib, hardware_spi, hardware_dma
 *
 * Configuration (define before including):
 *   HUI_RP2350_SPI      — SPI instance (default: spi0)
 *   HUI_RP2350_SCK      — SCK pin (default: 18)
 *   HUI_RP2350_MOSI     — MOSI pin (default: 19)
 *   HUI_RP2350_CS       — CS pin (default: 17)
 *   HUI_RP2350_DC       — D/C pin (default: 20)
 *   HUI_RP2350_RESET    — Reset pin (default: 21)
 *   HUI_RP2350_BAUDRATE — SPI baud rate (default: 62500000)
 *   HUI_RP2350_WIDTH    — Display width (default: 240)
 *   HUI_RP2350_HEIGHT   — Display height (default: 240)
 *   HUI_RP2350_DISPLAY  — HUI_DISPLAY_ST7789 (default) or HUI_DISPLAY_ILI9341
 */

#ifndef HUI_RP2350_H
#define HUI_RP2350_H

/* MCU profile: AA is too expensive on RP2350; force off regardless of HUI_AA */
#ifndef HUI_NO_AA
#  define HUI_NO_AA
#endif

#include <stdint.h>
#include <string.h>

/* ---- Configuration defaults ---- */

#ifndef HUI_RP2350_SPI
#  define HUI_RP2350_SPI   spi0
#endif
#ifndef HUI_RP2350_SCK
#  define HUI_RP2350_SCK   18
#endif
#ifndef HUI_RP2350_MOSI
#  define HUI_RP2350_MOSI  19
#endif
#ifndef HUI_RP2350_CS
#  define HUI_RP2350_CS    17
#endif
#ifndef HUI_RP2350_DC
#  define HUI_RP2350_DC    20
#endif
#ifndef HUI_RP2350_RESET
#  define HUI_RP2350_RESET 21
#endif
#ifndef HUI_RP2350_BAUDRATE
#  define HUI_RP2350_BAUDRATE 62500000
#endif
#ifndef HUI_RP2350_WIDTH
#  define HUI_RP2350_WIDTH  240
#endif
#ifndef HUI_RP2350_HEIGHT
#  define HUI_RP2350_HEIGHT 240
#endif

#define HUI_DISPLAY_ST7789  0
#define HUI_DISPLAY_ILI9341 1
#ifndef HUI_RP2350_DISPLAY
#  define HUI_RP2350_DISPLAY HUI_DISPLAY_ST7789
#endif

/* ---- SDK includes or host stubs ---- */

#if defined(PICO_SDK_VERSION_STRING) || defined(__rp2350)
#  include "pico/stdlib.h"
#  include "pico/multicore.h"
#  include "hardware/spi.h"
#  include "hardware/dma.h"
#  include "hardware/gpio.h"
#  define HUI_RP2350_HW 1
#else
/* Non-RP2350 build (e.g. unit tests on host) — stub out hardware calls */
#  define HUI_RP2350_HW 0
   typedef unsigned int spi_inst_t;
   typedef int dma_channel_config;
   static inline void spi_init(spi_inst_t *s, unsigned b)                      { (void)s;(void)b; }
   static inline void gpio_set_function(int p, int f)                          { (void)p;(void)f; }
   static inline void gpio_put(int p, int v)                                   { (void)p;(void)v; }
   static inline void gpio_init(int p)                                         { (void)p; }
   static inline void gpio_set_dir(int p, int d)                               { (void)p;(void)d; }
   static inline void spi_write_blocking(spi_inst_t *s, const uint8_t *b, size_t l) { (void)s;(void)b;(void)l; }
   static inline int  dma_claim_unused_channel(int p)                          { (void)p; return 0; }
   static inline void sleep_ms(int ms)                                         { (void)ms; }
   /* multicore stubs — single-threaded on host */
   static inline void     multicore_launch_core1(void (*fn)(void))             { (void)fn; }
   static inline void     multicore_fifo_push_blocking(uint32_t v)             { (void)v; }
   static inline uint32_t multicore_fifo_pop_blocking(void)                    { return 0; }
   /* SPI instance stub — Pico SDK exposes spi0/spi1 as extern spi_inst_t *.
    * Our stubs take spi_inst_t * so define spi0 as a pointer. */
   static spi_inst_t hui__rp_spi0_stub = 0;
#  define spi0 (&hui__rp_spi0_stub)
#  define GPIO_FUNC_SPI 1
#  define GPIO_OUT      1
#endif

/* ---- Pull in headless rasterizer (init/free/resize suppressed; we provide ours) ---- */
#define HUI_HEADLESS_NO_INIT
#define HUI_HEADLESS_NO_FLUSH
#include "hui_headless.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- RGBA source framebuffer (rasterizer writes here, core1 reads) ---- */
/* Declared file-scope so core1 can access it.  Core0 rasterises into this;
 * core1 converts it to RGB565 then signals "RGBA released" back to core0. */
static uint32_t hui__rp_rgba[HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT];

/* ---- RGB565 double-buffer ---- */
/* Core0 writes hui__rp_fb_write; core1 DMA-drains hui__rp_fb_read.
 * Pointers are swapped by core0 after posting to core1.
 *
 * RAM budget (240×240):
 *   RGBA:        240×240×4 = 225 KB
 *   RGB565 ×2:   240×240×2 = 112 KB each → 225 KB total
 *   Total:       ~450 KB of 520 KB SRAM — leaves ~70 KB for SDK + stacks.
 */
static uint16_t  hui__rp_fb_a[HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT];
static uint16_t  hui__rp_fb_b[HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT];
static uint16_t *hui__rp_fb_write = hui__rp_fb_a;   /* core0 fills this */
static uint16_t *hui__rp_fb_read  = hui__rp_fb_b;   /* core1 DMA-drains this */

static int hui__rp_dma_ch     = -1;
static int hui__rp_first_frame = 1; /* skip RGBA-released wait on frame 0 */

/* Suppress unused-variable warnings on host builds where HW-only paths are dead */
#if !HUI_RP2350_HW
static inline void hui__rp_unused_suppress(void) {
    (void)hui__rp_fb_read;
    (void)hui__rp_first_frame;
}
#endif

/* ---- Low-level SPI helpers ---- */

static inline void hui__rp_cs_low(void)  { gpio_put(HUI_RP2350_CS, 0); }
static inline void hui__rp_cs_high(void) { gpio_put(HUI_RP2350_CS, 1); }
static inline void hui__rp_dc_cmd(void)  { gpio_put(HUI_RP2350_DC, 0); }
static inline void hui__rp_dc_dat(void)  { gpio_put(HUI_RP2350_DC, 1); }

static void hui__rp_cmd(uint8_t cmd) {
    hui__rp_cs_low();
    hui__rp_dc_cmd();
    spi_write_blocking(HUI_RP2350_SPI, &cmd, 1);
    hui__rp_cs_high();
}

static void hui__rp_data(const uint8_t *buf, size_t len) {
    hui__rp_cs_low();
    hui__rp_dc_dat();
    spi_write_blocking(HUI_RP2350_SPI, buf, len);
    hui__rp_cs_high();
}

static void hui__rp_data_u8(uint8_t v) {
    hui__rp_data(&v, 1);
}

/* ---- Display init sequences ---- */

/* ST7789 minimal init for RGB565, portrait 240x240 */
static void hui__rp_st7789_init(void) {
    /* Hardware reset */
    gpio_put(HUI_RP2350_RESET, 0);
    sleep_ms(10);
    gpio_put(HUI_RP2350_RESET, 1);
    sleep_ms(120);

    hui__rp_cmd(0x01);   /* SWRESET — software reset */
    sleep_ms(150);
    hui__rp_cmd(0x11);   /* SLPOUT — sleep out */
    sleep_ms(255);

    /* COLMOD: 0x55 = 16-bit RGB565 */
    hui__rp_cmd(0x3A);
    hui__rp_data_u8(0x55);

    /* MADCTL: row/col order (landscape/portrait) */
    hui__rp_cmd(0x36);
    hui__rp_data_u8(0x00);

    /* CASET: column range 0..HUI_RP2350_WIDTH-1 */
    hui__rp_cmd(0x2A);
    {
        uint8_t d[4] = { 0, 0,
                         (uint8_t)((HUI_RP2350_WIDTH - 1) >> 8),
                         (uint8_t)((HUI_RP2350_WIDTH - 1) & 0xFF) };
        hui__rp_data(d, 4);
    }

    /* RASET: row range 0..HUI_RP2350_HEIGHT-1 */
    hui__rp_cmd(0x2B);
    {
        uint8_t d[4] = { 0, 0,
                         (uint8_t)((HUI_RP2350_HEIGHT - 1) >> 8),
                         (uint8_t)((HUI_RP2350_HEIGHT - 1) & 0xFF) };
        hui__rp_data(d, 4);
    }

    hui__rp_cmd(0x21);   /* INVON — inversion on (ST7789 quirk for most panels) */
    sleep_ms(10);
    hui__rp_cmd(0x29);   /* DISPON — display on */
    sleep_ms(10);
}

/* ILI9341 minimal init for RGB565, 240x320 (or 240x240 with MADCTL crop) */
static void hui__rp_ili9341_init(void) {
    gpio_put(HUI_RP2350_RESET, 0);
    sleep_ms(10);
    gpio_put(HUI_RP2350_RESET, 1);
    sleep_ms(120);

    hui__rp_cmd(0x01);   /* SWRESET */
    sleep_ms(150);
    hui__rp_cmd(0x11);   /* SLPOUT */
    sleep_ms(150);

    /* PIXFMT: 0x55 = 16-bit */
    hui__rp_cmd(0x3A);
    hui__rp_data_u8(0x55);

    /* MADCTL */
    hui__rp_cmd(0x36);
    hui__rp_data_u8(0x48);

    hui__rp_cmd(0x29);   /* DISPON */
    sleep_ms(50);
}

/* ---- Set pixel write window ---- */

static void hui__rp_set_addr_window(int x0, int y0, int x1, int y1) {
    hui__rp_cmd(0x2A);   /* CASET */
    {
        uint8_t d[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                         (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
        hui__rp_data(d, 4);
    }
    hui__rp_cmd(0x2B);   /* RASET */
    {
        uint8_t d[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                         (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
        hui__rp_data(d, 4);
    }
    hui__rp_cmd(0x2C);   /* RAMWR — begin pixel data */
}

/* ---- IMU quaternion → 8×8 LED grid index ---- */
/* Rotates the world-up vector (0,1,0) by q, then projects (x,y) to the
 * 8×8 grid.  Returns index in [0, 63].
 * Also used by hui_matrix8x8.h; declared here for RP2350 + external matrix. */

#ifdef HUI_IMPLEMENTATION

int hui_imu_quat_to_led(hui_v4 q) {
    /* Rotate world-up (0, 1, 0) by quaternion q */
    hui_v3 up = hui_quat_rotate(q, (hui_v3){0.0f, 1.0f, 0.0f});
    /* Map [-1,1] x [-1,1] to [0,7] x [0,7] with bilinear clamping */
    int col = hui_clamp((int)((up.x + 1.0f) * 4.0f), 0, 7);
    int row = hui_clamp((int)((up.y + 1.0f) * 4.0f), 0, 7);
    return row * 8 + col;
}

/* ---- Init / free / resize ---- */

static void hui_headless_init(int w, int h) {
    /* Clamp to display dimensions; RP2350 has a fixed physical display. */
    (void)w; (void)h;
    hui__fb.w          = HUI_RP2350_WIDTH;
    hui__fb.h          = HUI_RP2350_HEIGHT;
    hui__fb.pixels     = hui__rp_rgba;   /* file-scope RGBA buffer */
    hui__fb.clip_x0    = 0;
    hui__fb.clip_y0    = 0;
    hui__fb.clip_x1    = HUI_RP2350_WIDTH;
    hui__fb.clip_y1    = HUI_RP2350_HEIGHT;
    hui__fb.clip_depth = 0;

    /* Configure GPIO */
    gpio_init(HUI_RP2350_CS);    gpio_set_dir(HUI_RP2350_CS,    GPIO_OUT);
    gpio_init(HUI_RP2350_DC);    gpio_set_dir(HUI_RP2350_DC,    GPIO_OUT);
    gpio_init(HUI_RP2350_RESET); gpio_set_dir(HUI_RP2350_RESET, GPIO_OUT);
    gpio_set_function(HUI_RP2350_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(HUI_RP2350_MOSI, GPIO_FUNC_SPI);

    /* Initialise SPI peripheral */
    spi_init(HUI_RP2350_SPI, HUI_RP2350_BAUDRATE);

    /* Claim a DMA channel for pixel transfers */
    hui__rp_dma_ch = dma_claim_unused_channel(1 /* required */);

    /* Send display init sequence */
#if HUI_RP2350_DISPLAY == HUI_DISPLAY_ILI9341
    hui__rp_ili9341_init();
#else
    hui__rp_st7789_init();
#endif

    memset(hui__rp_fb_a, 0, sizeof(hui__rp_fb_a));
    memset(hui__rp_fb_b, 0, sizeof(hui__rp_fb_b));

    /* Launch core1 — it owns conversion + DMA for all subsequent frames */
#if HUI_RP2350_HW
    multicore_launch_core1(hui__rp_core1_entry);
#endif
}

static void hui_headless_free(void) {
    /* Static buffers; nothing to free. */
    hui__fb.pixels = NULL;
}

static void hui_headless_resize(int w, int h) {
    /* Physical display is fixed size — ignore. */
    (void)w; (void)h;
}

/* ---- RGBA → RGB565 conversion (explicit src/dst) ---- */
/* Converts n pixels from ARGB8888 to big-endian RGB565 for SPI.
 * __bswap16 is a GCC built-in; falls back to manual swap on host. */
#ifndef __bswap16
#  define __bswap16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))
#endif

static void hui__rp_convert_fb(const uint32_t *rgba, uint16_t *rgb565, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t px  = rgba[i];
        uint16_t r5  = (uint16_t)((px >> 19) & 0x1F);
        uint16_t g6  = (uint16_t)((px >> 10) & 0x3F);
        uint16_t b5  = (uint16_t)((px >>  3) & 0x1F);
        rgb565[i]    = __bswap16((uint16_t)((r5 << 11) | (g6 << 5) | b5));
    }
}

/* ---- Core1 entry: convert + DMA (hardware only) ---- */
/* Protocol (via multicore FIFO, core0 → core1):
 *   push rgba_ptr   (uint32_t cast of const uint32_t *)
 *   push rgb565_ptr (uint32_t cast of uint16_t *)
 * After conversion core1 pushes 1 back → "RGBA buffer released, core0 may rasterise".
 * DMA then runs; CS raised on completion.  Loop forever. */
#if HUI_RP2350_HW
static void hui__rp_core1_entry(void) {
    int n = HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT;
    while (1) {
        const uint32_t *rgba   = (const uint32_t *)(uintptr_t)multicore_fifo_pop_blocking();
        uint16_t       *rgb565 = (uint16_t *)       (uintptr_t)multicore_fifo_pop_blocking();

        /* Convert RGBA → RGB565 */
        hui__rp_convert_fb(rgba, rgb565, n);

        /* Signal core0: RGBA buffer is no longer being read — safe to rasterise */
        multicore_fifo_push_blocking(1u);

        /* Set SPI window and DMA the RGB565 buffer to the display */
        hui__rp_set_addr_window(0, 0, HUI_RP2350_WIDTH - 1, HUI_RP2350_HEIGHT - 1);

        dma_channel_config cfg = dma_channel_get_default_config(hui__rp_dma_ch);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
        channel_config_set_dreq(&cfg, spi_get_dreq(HUI_RP2350_SPI, true));
        hui__rp_cs_low();
        hui__rp_dc_dat();
        dma_channel_configure(hui__rp_dma_ch, &cfg,
                              &spi_get_hw(HUI_RP2350_SPI)->dr,
                              rgb565,
                              (uint32_t)n,
                              true /* start */);
        dma_channel_wait_for_finish_blocking(hui__rp_dma_ch);
        hui__rp_cs_high();
    }
}
#endif /* HUI_RP2350_HW */

/* ---- Backend flush ---- */

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
#if HUI_RP2350_HW
    /* Double-buffer pipeline (hardware):
     *
     *  Frame N:  rasterise → post(rgba, rgb565_write) → swap → return
     *  Core1:                                convert → signal(rgba_released)
     *                                                → DMA → CS high → loop
     *
     * Core0 waits for "RGBA released" from core1 before rasterising frame N+1,
     * ensuring we never write into the RGBA buffer while core1 is still reading it.
     * DMA of frame N-1 may still be running while core0 rasterises frame N —
     * that is the pipeline overlap we're after. */

    /* Wait for core1 to finish reading RGBA from the previous frame */
    if (!hui__rp_first_frame)
        (void)multicore_fifo_pop_blocking();   /* receive "RGBA released" */
    hui__rp_first_frame = 0;

    /* Rasterise into the shared RGBA buffer */
    hui__rasterize(cmds, count, strpool, datapool);

    /* Hand (rgba, rgb565_write) to core1 */
    multicore_fifo_push_blocking((uint32_t)(uintptr_t)hui__fb.pixels);
    multicore_fifo_push_blocking((uint32_t)(uintptr_t)hui__rp_fb_write);

    /* Swap write/read pointers for next frame */
    uint16_t *tmp      = hui__rp_fb_write;
    hui__rp_fb_write   = hui__rp_fb_read;
    hui__rp_fb_read    = tmp;

    /* Return immediately — core1 handles conversion + DMA */

#else
    /* Host / non-HW stub: single-threaded, blocking (no core1, no real DMA) */
    hui__rasterize(cmds, count, strpool, datapool);
    hui__rp_convert_fb(hui__fb.pixels, hui__rp_fb_write,
                       HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT);
    hui__rp_set_addr_window(0, 0, HUI_RP2350_WIDTH - 1, HUI_RP2350_HEIGHT - 1);
    hui__rp_cs_low();
    hui__rp_dc_dat();
    spi_write_blocking(HUI_RP2350_SPI,
                       (const uint8_t *)hui__rp_fb_write,
                       (size_t)(HUI_RP2350_WIDTH * HUI_RP2350_HEIGHT * 2));
    hui__rp_cs_high();
#endif
}

#endif /* HUI_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_RP2350_H */
