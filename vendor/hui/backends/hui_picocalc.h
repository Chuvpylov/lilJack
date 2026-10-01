/*
 * hui_picocalc.h — PicoCalc hardware backend for hui
 *
 * Strip framebuffer renderer: the screen is divided into horizontal strips
 * (320 × HUI_PC_STRIP_H pixels). All HUI commands are rasterised into each
 * strip in RAM, then the strip is sent to the ILI9488 as one contiguous
 * RAMWR transaction.  This gives exactly 40 SPI window resets per frame
 * instead of one per draw-call, eliminating the visible horizontal tearing.
 *
 * Memory: 320 × 8 × 3 = 7 680 bytes for the strip buffer.
 * IO: keyboard polled via i2ckbd I2C @ 0x1F.
 *
 * Select with: #define HUI_BACKEND_PICOCALC before #include "hui.h"
 */

#ifndef HUI_PICOCALC_H
#define HUI_PICOCALC_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../hui_math.h"
#include "../hui_draw.h"
#include "../hui_font.h"

/* HAL provided by bios project — paths resolved by CMake include dirs */
#include "hal/display/lcdspi.h"
#include "hal/keyboard/key_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Clip stack ---- */

#define HUI_PC_CLIP_DEPTH 8
#define HUI_PC_STRIP_H    8   /* rows per strip; 320×8×3 = 7 680 bytes */

typedef struct { int x0, y0, x1, y1; } hui_pc_clip_t;

static hui_pc_clip_t hui__pc_clip_stack[HUI_PC_CLIP_DEPTH];
static int           hui__pc_clip_depth = 0;
static hui_pc_clip_t hui__pc_clip       = {0, 0, 320, 320};

/* ---- Strip buffer ---- */

static uint8_t hui__strip_buf[320 * HUI_PC_STRIP_H * 3];

/* Font pointer from lcdspi.c (8×12 bitmap font) */
extern unsigned char *MainFont;

/* ---- Headless pixel-buffer stubs (no pixel buffer needed) ---- */

#ifndef HUI_HEADLESS_NO_INIT
static void hui_headless_init(int w, int h)   { (void)w; (void)h; }
static void hui_headless_free(void)            {}
static void hui_headless_resize(int w, int h) { (void)w; (void)h; }
#define HUI_HEADLESS_NO_INIT
#endif

/* ---- Strip rasterisation helpers ---- */

/* Write one pixel (absolute coords) into the current strip */
static inline void hui__pix(int x, int y, int sy0,
                             uint8_t r, uint8_t g, uint8_t b)
{
    int row = y - sy0;
    if ((unsigned)row >= (unsigned)HUI_PC_STRIP_H) return;
    if ((unsigned)x   >= 320u)                     return;
    int off = (row * 320 + x) * 3;
    hui__strip_buf[off]   = r;
    hui__strip_buf[off+1] = g;
    hui__strip_buf[off+2] = b;
}

/* Fill a horizontal span in the strip (absolute coords) */
static inline void hui__hspan(int x0, int x1, int y, int sy0,
                               uint8_t r, uint8_t g, uint8_t b)
{
    int row = y - sy0;
    if ((unsigned)row >= (unsigned)HUI_PC_STRIP_H) return;
    if (x0 < 0)   x0 = 0;
    if (x1 > 319) x1 = 319;
    if (x0 > x1)  return;
    int off = (row * 320 + x0) * 3;
    for (int x = x0; x <= x1; x++, off += 3) {
        hui__strip_buf[off]   = r;
        hui__strip_buf[off+1] = g;
        hui__strip_buf[off+2] = b;
    }
}

/* Rasterise a text string into the strip.
 * Font format: MainFont[0]=fw, [1]=fh, [2]=first_char, [3]=last_char,
 * then packed 1-bpp bitmaps (MSB = leftmost pixel, top row first). */
static void hui__text_strip(int cx, int cy, const char *s,
                             int sy0, int sy1,
                             uint8_t fr, uint8_t fg, uint8_t fb,
                             uint8_t br, uint8_t bg, uint8_t bb)
{
    int fw         = MainFont[0];   /* 8  */
    int fh         = MainFont[1];   /* 12 */
    int fc_start   = MainFont[2];   /* 0x20 */
    int fc_last    = MainFont[3];
    int bpc        = (fw * fh + 7) / 8;   /* bytes per char = 12 */

    for (; *s; cx += fw) {
        /* decode UTF-8; anything outside the font range renders as '?' —
         * indexing past fc_last would read garbage past the font table */
        uint32_t cp = hui_utf8_next(&s);
        unsigned char ch = (cp >= (unsigned)fc_start && cp <= (unsigned)fc_last)
                           ? (unsigned char)cp : '?';
        const uint8_t *bm = &MainFont[4 + (ch - fc_start) * bpc];

        for (int row = 0; row < fh; row++) {
            int ay = cy + row;
            if (ay < sy0 || ay >= sy1) continue;
            for (int col = 0; col < fw; col++) {
                int ax = cx + col;
                /* bit ordering matches draw_bitmap_spi in lcdspi.c */
                int bit_idx = row * fw + col;
                int bit_pos = (fh * fw - bit_idx - 1) % 8;
                int on = (bm[bit_idx / 8] >> bit_pos) & 1;
                if (on)
                    hui__pix(ax, ay, sy0, fr, fg, fb);
                else
                    hui__pix(ax, ay, sy0, br, bg, bb);
            }
        }
    }
}

/* ---- Command renderer ---- */

void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool)
{
    (void)datapool;

    /* Render the frame in horizontal strips.
     * Each strip = one define_region_spi + one spi_write_fast.
     * 40 strips × (CASET+RASET+RAMWR overhead) instead of 200+ per frame. */

    for (int sy0 = 0; sy0 < 320; sy0 += HUI_PC_STRIP_H) {
        int sy1 = sy0 + HUI_PC_STRIP_H;
        if (sy1 > 320) sy1 = 320;
        int sh = sy1 - sy0;

        /* Clear strip to black */
        memset(hui__strip_buf, 0, (size_t)(320 * sh * 3));

        /* Reset clip to full screen for this strip pass */
        hui__pc_clip.x0 = 0;  hui__pc_clip.y0 = 0;
        hui__pc_clip.x1 = 320; hui__pc_clip.y1 = 320;
        hui__pc_clip_depth = 0;

        /* Strip is memset(0) at the start of each pass — text bg is always
         * transparent (black / strip clear colour).  Callers that want an
         * explicit bg colour should draw a RECT_FILL behind the text. */

        for (uint16_t i = 0; i < count; i++) {
            const hui_cmd *c = &cmds[i];

            /* Quick Y-range cull — skip rendering commands whose bbox cannot
             * touch this strip [sy0, sy1).  CLIP_PUSH/POP are always processed
             * to keep the clip stack consistent across strips. */
            if (c->type != HUI_CMD_CLIP_PUSH && c->type != HUI_CMD_CLIP_POP) {
                int cmd_y0, cmd_y1;
                switch (c->type) {
                case HUI_CMD_RECT_FILL:
                case HUI_CMD_RECT:
                    cmd_y0 = c->y0;
                    cmd_y1 = c->y0 + c->y1 - 1;
                    break;
                case HUI_CMD_LINE:
                    cmd_y0 = c->y0 < c->y1 ? c->y0 : c->y1;
                    cmd_y1 = c->y0 > c->y1 ? c->y0 : c->y1;
                    break;
                case HUI_CMD_TEXT:
                    cmd_y0 = c->y0;
                    cmd_y1 = c->y0 + (int)MainFont[1] - 1;
                    break;
                case HUI_CMD_CIRCLE:
                case HUI_CMD_CIRCLE_FILL:
                    cmd_y0 = c->y0 - c->x1;
                    cmd_y1 = c->y0 + c->x1;
                    break;
                default:
                    cmd_y0 = 0; cmd_y1 = 319;
                    break;
                }
                if (cmd_y1 < sy0 || cmd_y0 >= sy1) continue;
            }

            uint8_t r = c->col_r, g = c->col_g, b = c->col_b;

            switch (c->type) {

            case HUI_CMD_RECT_FILL: {
                /* Note: c->rounding is silently ignored in strip renderer.
                 * Callers that need rounded fill should approximate with a
                 * circle + rect combination until arc support is added. */
                int x0 = c->x0, y0 = c->y0;
                int x1 = x0 + c->x1 - 1, y1 = y0 + c->y1 - 1;
                /* Clip to hui clip rect */
                if (x0 < hui__pc_clip.x0) x0 = hui__pc_clip.x0;
                if (y0 < hui__pc_clip.y0) y0 = hui__pc_clip.y0;
                if (x1 >= hui__pc_clip.x1) x1 = hui__pc_clip.x1 - 1;
                if (y1 >= hui__pc_clip.y1) y1 = hui__pc_clip.y1 - 1;
                /* Intersect with strip */
                int ry0 = y0 > sy0 ? y0 : sy0;
                int ry1 = y1 < sy1-1 ? y1 : sy1-1;
                if (x0 <= x1 && ry0 <= ry1)
                    for (int y = ry0; y <= ry1; y++)
                        hui__hspan(x0, x1, y, sy0, r, g, b);
                break;
            }

            case HUI_CMD_RECT: {
                int x0 = c->x0, y0 = c->y0;
                int x1 = x0 + c->x1 - 1, y1 = y0 + c->y1 - 1;
                /* Top edge */
                if (y0 >= sy0 && y0 < sy1)
                    hui__hspan(x0, x1, y0, sy0, r, g, b);
                /* Bottom edge */
                if (y1 >= sy0 && y1 < sy1)
                    hui__hspan(x0, x1, y1, sy0, r, g, b);
                /* Left + right edges (interior rows) */
                int ey0 = (y0+1) > sy0 ? (y0+1) : sy0;
                int ey1 = (y1-1) < (sy1-1) ? (y1-1) : (sy1-1);
                for (int y = ey0; y <= ey1; y++) {
                    hui__pix(x0, y, sy0, r, g, b);
                    hui__pix(x1, y, sy0, r, g, b);
                }
                break;
            }

            case HUI_CMD_LINE: {
                int lx0=c->x0, ly0=c->y0, lx1=c->x1, ly1=c->y1;
                /* Liang-Barsky clip to hui clip rect */
                {
                    float dx=(float)(lx1-lx0), dy=(float)(ly1-ly0);
                    float t0=0.f, t1=1.f;
                    float p[4]={-dx,dx,-dy,dy};
                    float q[4]={(float)(lx0-hui__pc_clip.x0),
                                (float)(hui__pc_clip.x1-1-lx0),
                                (float)(ly0-hui__pc_clip.y0),
                                (float)(hui__pc_clip.y1-1-ly0)};
                    int vis=1;
                    for(int k=0;k<4;k++){
                        if(p[k]==0.f){if(q[k]<0.f){vis=0;break;}}
                        else{float rv=q[k]/p[k];
                             if(p[k]<0.f){if(rv>t0)t0=rv;}else{if(rv<t1)t1=rv;}}
                    }
                    if(!vis || t0>t1) break;
                    if(t1<1.f){lx1=lx0+(int)(t1*dx);ly1=ly0+(int)(t1*dy);}
                    if(t0>0.f){lx0=lx0+(int)(t0*dx);ly0=ly0+(int)(t0*dy);}
                }
                /* Horizontal line fast path */
                if (ly0 == ly1) {
                    if (ly0 >= sy0 && ly0 < sy1) {
                        if (lx0 > lx1) { int t=lx0; lx0=lx1; lx1=t; }
                        hui__hspan(lx0, lx1, ly0, sy0, r, g, b);
                    }
                /* Vertical line fast path */
                } else if (lx0 == lx1) {
                    if (ly0 > ly1) { int t=ly0; ly0=ly1; ly1=t; }
                    int ry0 = ly0 > sy0 ? ly0 : sy0;
                    int ry1 = ly1 < sy1-1 ? ly1 : sy1-1;
                    for (int y = ry0; y <= ry1; y++)
                        hui__pix(lx0, y, sy0, r, g, b);
                /* General diagonal — Bresenham, skip off-strip pixels */
                } else {
                    int dx = abs(lx1-lx0), sx = lx0<lx1 ? 1 : -1;
                    int dy = abs(ly1-ly0), sy_s = ly0<ly1 ? 1 : -1;
                    int err = (dx>dy ? dx : -dy)/2;
                    int x=lx0, y=ly0;
                    while (1) {
                        if (y >= sy0 && y < sy1)
                            hui__pix(x, y, sy0, r, g, b);
                        if (x==lx1 && y==ly1) break;
                        int e2=err;
                        if (e2>-dx){err-=dy; x+=sx;}
                        if (e2< dy){err+=dx; y+=sy_s;}
                    }
                }
                break;
            }

            case HUI_CMD_TEXT: {
                if (c->x2 >= 0) {
                    const char *s_str = strpool + c->x2;
                    hui__text_strip(c->x0, c->y0, s_str, sy0, sy1,
                                    r, g, b,
                                    0, 0, 0);
                }
                break;
            }

            case HUI_CMD_CIRCLE_FILL: {
                /* Bresenham midpoint circle — filled, strip-clipped. */
                int cx = c->x0, cy = c->y0, rad = c->x1;
                /* Quick reject: circle entirely outside strip or clip */
                if (cy + rad < sy0 || cy - rad >= sy1) break;
                if (cy + rad < hui__pc_clip.y0 || cy - rad >= hui__pc_clip.y1) break;
                /* Iterate each row in the strip that intersects the circle */
                int ry0 = cy - rad > sy0 ? cy - rad : sy0;
                int ry1 = cy + rad < sy1-1 ? cy + rad : sy1-1;
                for (int y = ry0; y <= ry1; y++) {
                    int dy = y - cy;
                    /* chord half-width at this row */
                    int dx = (int)sqrtf((float)(rad*rad - dy*dy));
                    int x0 = cx - dx, x1 = cx + dx;
                    /* clip to hui clip rect */
                    if (x0 < hui__pc_clip.x0) x0 = hui__pc_clip.x0;
                    if (x1 >= hui__pc_clip.x1) x1 = hui__pc_clip.x1 - 1;
                    if (x0 <= x1)
                        hui__hspan(x0, x1, y, sy0, r, g, b);
                }
                break;
            }

            case HUI_CMD_CIRCLE: {
                /* Bresenham midpoint circle — outline only, strip-clipped. */
                int cx = c->x0, cy = c->y0, rad = c->x1;
                if (cy + rad < sy0 || cy - rad >= sy1) break;
                /* Walk Bresenham octants; emit only pixels in this strip. */
                int bx = rad, by = 0, err = 0;
                while (bx >= by) {
                    hui__pix(cx+bx, cy+by, sy0, r, g, b);
                    hui__pix(cx-bx, cy+by, sy0, r, g, b);
                    hui__pix(cx+bx, cy-by, sy0, r, g, b);
                    hui__pix(cx-bx, cy-by, sy0, r, g, b);
                    hui__pix(cx+by, cy+bx, sy0, r, g, b);
                    hui__pix(cx-by, cy+bx, sy0, r, g, b);
                    hui__pix(cx+by, cy-bx, sy0, r, g, b);
                    hui__pix(cx-by, cy-bx, sy0, r, g, b);
                    if (err <= 0) { by++; err += 2*by + 1; }
                    else          { bx--; err -= 2*bx + 1; }
                }
                break;
            }

            case HUI_CMD_CLIP_PUSH: {
                if (hui__pc_clip_depth < HUI_PC_CLIP_DEPTH)
                    hui__pc_clip_stack[hui__pc_clip_depth++] = hui__pc_clip;
                hui__pc_clip.x0 = c->x0;
                hui__pc_clip.y0 = c->y0;
                hui__pc_clip.x1 = c->x0 + c->x1;
                hui__pc_clip.y1 = c->y0 + c->y1;
                break;
            }

            case HUI_CMD_CLIP_POP: {
                if (hui__pc_clip_depth > 0)
                    hui__pc_clip = hui__pc_clip_stack[--hui__pc_clip_depth];
                break;
            }

            default:
                break;
            }
        }

        /* Flush strip to LCD — one contiguous RAMWR per strip */
        define_region_spi(0, sy0, 319, sy1 - 1, 1);
        spi_write_fast(Pico_LCD_SPI_MOD, hui__strip_buf, (size_t)(320 * sh * 3));
        spi_finish(Pico_LCD_SPI_MOD);
        lcd_spi_raise_cs();
    }
}

#ifdef __cplusplus
}
#endif
#endif /* HUI_PICOCALC_H */
