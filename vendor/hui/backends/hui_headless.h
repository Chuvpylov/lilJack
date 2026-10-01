/*
 * hui_headless.h — Software rasterizer backend for hui
 *
 * Renders to a uint32_t RGBA pixel buffer.
 * Supports PPM dump for visual verification.
 *
 * Algorithms:
 *   Line:     Bresenham (thick: parallel shift + fill)
 *   Circle:   Midpoint / Bresenham circle algorithm
 *   Triangle: Scanline span-fill with edge walking
 *   Bezier:   Recursive subdivision to depth 8
 *   Arc/Hemi: Midpoint circle, angle-filtered
 *   Text:     8x8 bitmap font blit
 *   Rect:     Horizontal span fills
 *
 * Select with: #define HUI_BACKEND_HEADLESS before #include "hui.h"
 */

#ifndef HUI_HEADLESS_H
#define HUI_HEADLESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../hui_math.h"
#include "../hui_draw.h"
#include "../hui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pixel buffer ---- */

typedef struct {
    uint32_t *pixels;    /* ARGB packed: 0xAARRGGBB */
    int       w, h;
    /* Active clip rect */
    int       clip_x0, clip_y0, clip_x1, clip_y1;
    /* Clip stack (max 16 levels) */
    struct { int x0,y0,x1,y1; } clip_stack[16];
    int clip_depth;
} hui_fb;

static hui_fb hui__fb;

/* ---- Rasterization scale (DPR) ----
 * Draw commands and TTF pen positions are authored in logical (CSS) pixels;
 * when hui__fb_scale > 1 the framebuffer is allocated at physical resolution
 * and every command's coordinates are scaled at rasterization time.  Layout,
 * hit-testing and all app-side geometry stay in logical pixels — the image is
 * identical, just rasterized crisper.  Backends that render 1:1 (X11,
 * headless PNG) leave this at 1.0 and are bit-identical to before. */
static float hui__fb_scale = 1.0f;

static inline int hui__ss(int v) {           /* scale a coordinate */
    return hui__fb_scale == 1.0f ? v
         : (int)((float)v * hui__fb_scale + (v < 0 ? -0.5f : 0.5f));
}
/* Scale a span edge-consistently: end = ss(x0+w) - ss(x0), so adjacent
 * rects/clips stay seam-free at fractional scales. */
static inline int hui__ss_span(int x0, int w) { return hui__ss(x0 + w) - hui__ss(x0); }
static inline int hui__ss_thick(uint8_t t) {   /* line thickness: never below 1 */
    int v = t ? t : 1;
    /* Keep hairlines at 1px at ANY DPR: rounding 1→2 at fractional DPR would
       push them off the anti-aliased 1px line path onto the thick (Bresenham
       quad) path, which staircases diagonals (the globe grid/boundaries). A
       1px AA hairline stays smooth; only genuinely thick lines scale. */
    if (v <= 1) return 1;
    if (hui__fb_scale == 1.0f) return v;
    int s = (int)((float)v * hui__fb_scale + 0.5f);
    return s > 0 ? s : 1;
}

/* ---- Init/shutdown ---- */

#ifndef HUI_HEADLESS_NO_INIT
static void hui_headless_init(int w, int h) {
    hui__fb.w = w;
    hui__fb.h = h;
    hui__fb.pixels = (uint32_t*)calloc((size_t)(w*h), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = w; hui__fb.clip_y1 = h;
    hui__fb.clip_depth = 0;
}

static void hui__scratch_free(void);          /* per-frame scratch arena, defined with the blit queue below */
static void hui_headless_free(void) {
    hui__scratch_free();
    free(hui__fb.pixels);
    hui__fb.pixels = NULL;
}

static void hui_headless_resize(int w, int h) {
    free(hui__fb.pixels);
    hui__fb.w = w; hui__fb.h = h;
    hui__fb.pixels = (uint32_t*)calloc((size_t)(w*h), sizeof(uint32_t));
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = w; hui__fb.clip_y1 = h;
    hui__fb.clip_depth = 0;
    /* Keep context screen dimensions in sync */
    if (hui_g) { hui_g->screen_w = (int16_t)w; hui_g->screen_h = (int16_t)h; }
}
#endif /* HUI_HEADLESS_NO_INIT */

/* ---- PPM dump ---- */

static void hui__save_argb_ppm(const char *path, const uint32_t *pixels, int w, int h)
    __attribute__((unused));
static void hui__save_argb_ppm(const char *path, const uint32_t *pixels, int w, int h) {
    if (!path || !pixels || w <= 0 || h <= 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t px = pixels[i];
        uint8_t r = (uint8_t)((px >> 16) & 0xFF);
        uint8_t g = (uint8_t)((px >>  8) & 0xFF);
        uint8_t b = (uint8_t)((px      ) & 0xFF);
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
}

static void hui_headless_save_ppm(const char *path)
    __attribute__((unused));
static void hui_headless_save_ppm(const char *path) {
    hui__save_argb_ppm(path, hui__fb.pixels, hui__fb.w, hui__fb.h);
}

/* ---- PNG dump (atomic write, no external deps) ---- */

/* CRC32 table — initialized once. */
static uint32_t hui__crc_table[256];
static int      hui__crc_ready = 0;
static void hui__crc_init(void) {
    if (hui__crc_ready) return;
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        hui__crc_table[n] = c;
    }
    hui__crc_ready = 1;
}
HUI_MAYBE_UNUSED static uint32_t hui__crc32(const uint8_t *d, int len, uint32_t crc) {
    crc ^= 0xFFFFFFFFu;
    for (int i = 0; i < len; i++)
        crc = hui__crc_table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* Write a 4-byte big-endian uint32. */
static void hui__png_u32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v };
    fwrite(b, 1, 4, f);
}

/* Write a PNG chunk: length + type + data + CRC. */
static void hui__png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len) {
    hui__png_u32(f, len);
    fwrite(type, 1, 4, f);
    if (len && data) fwrite(data, 1, (size_t)len, f);
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < 4; i++)
        c = hui__crc_table[(c ^ (uint8_t)type[i]) & 0xFF] ^ (c >> 8);
    if (len && data)
        for (uint32_t i = 0; i < len; i++)
            c = hui__crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    hui__png_u32(f, c ^ 0xFFFFFFFFu);
}

/* Save headless pixel buffer as an uncompressed PNG (stored DEFLATE, no zlib compression).
 * Atomically renames <path>.tmp → <path> so readers never see a partial file.
 * Returns 0 on success, -1 on error. */
static int hui__save_argb_png(const char *path, const uint32_t *pixels, int w, int h)
    __attribute__((unused));
static int hui__save_argb_png(const char *path, const uint32_t *pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return -1;

    hui__crc_init();

    /* Build tmp path */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    /* PNG signature */
    static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig, 1, 8, f);

    /* IHDR */
    uint8_t ihdr[13];
    int W = w, H = h;
    ihdr[0]=(uint8_t)(W>>24); ihdr[1]=(uint8_t)(W>>16);
    ihdr[2]=(uint8_t)(W>>8);  ihdr[3]=(uint8_t)W;
    ihdr[4]=(uint8_t)(H>>24); ihdr[5]=(uint8_t)(H>>16);
    ihdr[6]=(uint8_t)(H>>8);  ihdr[7]=(uint8_t)H;
    ihdr[8]=8;   /* bit depth */
    ihdr[9]=2;   /* color type: RGB */
    ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    hui__png_chunk(f, "IHDR", ihdr, 13);

    /* IDAT — uncompressed zlib (stored blocks, BTYPE=00).
     * Each row: 1 filter byte (0x00) + W*3 RGB bytes.
     * Max DEFLATE stored block payload: 65535 bytes. */
    int row_bytes  = 1 + W * 3;         /* filter byte + RGB */
    int total_data = H * row_bytes;

    /* Adler-32 state */
    uint32_t adler_s1 = 1, adler_s2 = 0;
    #define HUI__ADLER(b) do { adler_s1=(adler_s1+(b))%65521; adler_s2=(adler_s2+adler_s1)%65521; } while(0)

    /* Accumulate IDAT payload in memory (zlib header + blocks + checksum) */
    int max_block  = 65535;
    int idat_cap   = 2 + total_data + (total_data / max_block + 1) * 5 + 4 + 16;
    uint8_t *idat  = (uint8_t*)malloc((size_t)idat_cap);
    if (!idat) { fclose(f); remove(tmp); return -1; }

    int ip = 0;
    /* zlib header: CMF=0x78 (deflate, window=32k), FLG=0x01 (check: 0x7801%31=0) */
    idat[ip++] = 0x78; idat[ip++] = 0x01;

    int remaining = total_data;
    int row = 0, col = 0;  /* current position in the unfiltered stream */
    /* We iterate by stored block */
    while (remaining > 0) {
        int blk = remaining < max_block ? remaining : max_block;
        int last = (blk == remaining) ? 1 : 0;
        /* DEFLATE block header: BFINAL, BTYPE=00, LEN, NLEN */
        idat[ip++] = (uint8_t)last;  /* BFINAL | BTYPE=00 */
        idat[ip++] = (uint8_t)(blk & 0xFF);
        idat[ip++] = (uint8_t)((blk >> 8) & 0xFF);
        idat[ip++] = (uint8_t)(~blk & 0xFF);
        idat[ip++] = (uint8_t)((~blk >> 8) & 0xFF);
        /* Write blk bytes of image data */
        for (int k = 0; k < blk; k++) {
            uint8_t byte;
            if (col == 0) {
                byte = 0; /* filter type None */
            } else {
                int c = col - 1;
                uint32_t px = pixels[row * W + c / 3];
                if      ((c % 3) == 0) byte = (uint8_t)((px >> 16) & 0xFF);
                else if ((c % 3) == 1) byte = (uint8_t)((px >>  8) & 0xFF);
                else                   byte = (uint8_t)( px        & 0xFF);
            }
            idat[ip++] = byte;
            HUI__ADLER(byte);
            col++;
            if (col == row_bytes) { col = 0; row++; }
        }
        remaining -= blk;
    }
    #undef HUI__ADLER

    /* Adler-32 checksum (big-endian) */
    uint32_t adler = (adler_s2 << 16) | adler_s1;
    idat[ip++] = (uint8_t)(adler >> 24);
    idat[ip++] = (uint8_t)(adler >> 16);
    idat[ip++] = (uint8_t)(adler >>  8);
    idat[ip++] = (uint8_t)(adler      );

    hui__png_chunk(f, "IDAT", idat, (uint32_t)ip);
    free(idat);

    /* IEND */
    hui__png_chunk(f, "IEND", NULL, 0);

    fclose(f);

    /* Atomic rename */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static int hui_headless_save_png(const char *path)
    __attribute__((unused));
static int hui_headless_save_png(const char *path) {
    return hui__save_argb_png(path, hui__fb.pixels, hui__fb.w, hui__fb.h);
}

/* Scale a source ARGB buffer into a destination buffer using integer-step
 * nearest-neighbor scaling with letterboxing. scale_step=0 picks the largest
 * integer scale that fits in the destination. */
static void hui__scale_argb_letterbox(uint32_t *dst, int dw, int dh,
                                      const uint32_t *src, int sw, int sh,
                                      int scale_step,
                                      int *out_scale,
                                      int *out_off_x,
                                      int *out_off_y)
    __attribute__((unused));
static void hui__scale_argb_letterbox(uint32_t *dst, int dw, int dh,
                                      const uint32_t *src, int sw, int sh,
                                      int scale_step,
                                      int *out_scale,
                                      int *out_off_x,
                                      int *out_off_y) {
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    int sc = scale_step;
    if (sc <= 0) {
        int sx = dw / sw;
        int sy = dh / sh;
        sc = sx < sy ? sx : sy;
        if (sc < 1) sc = 1;
    }
    int off_x = (dw - sw * sc) / 2;
    int off_y = (dh - sh * sc) / 2;

    if (out_scale) *out_scale = sc;
    if (out_off_x) *out_off_x = off_x;
    if (out_off_y) *out_off_y = off_y;

    if (sc == 1 && off_x == 0 && off_y == 0 && sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)dw * (size_t)dh * sizeof(uint32_t));
        return;
    }

    memset(dst, 0, (size_t)(dw * dh) * sizeof(uint32_t));

    int vis_x0 = off_x < 0 ? 0 : off_x;
    int vis_x1 = off_x + sw * sc;
    if (vis_x1 > dw) vis_x1 = dw;
    if (vis_x0 >= vis_x1) return;

    for (int sy2 = 0; sy2 < sh; sy2++) {
        int dy0 = off_y + sy2 * sc;
        int dy1 = dy0 + sc;
        if (dy1 <= 0) continue;
        if (dy0 >= dh) break;

        int base_y = dy0 < 0 ? 0 : dy0;
        uint32_t *row = dst + (size_t)base_y * (size_t)dw;
        for (int sx2 = 0; sx2 < sw; sx2++) {
            uint32_t px = src[sy2 * sw + sx2];
            int dx0 = off_x + sx2 * sc;
            int dx1 = dx0 + sc;
            if (dx1 <= 0) continue;
            if (dx0 >= dw) break;
            int x0 = dx0 < 0 ? 0 : dx0;
            int x1 = dx1 > dw ? dw : dx1;
            for (int x = x0; x < x1; x++)
                row[x] = px;
        }

        size_t row_bytes = (size_t)(vis_x1 - vis_x0) * sizeof(uint32_t);
        for (int dyr = base_y + 1; dyr < dy1 && dyr < dh; dyr++) {
            memmove(dst + (size_t)dyr * (size_t)dw + vis_x0,
                    row + vis_x0,
                    row_bytes);
        }
    }
}

/* ---- Image blit (headless-derived backends only) ---- */
/* Scale-blit raw RGB (3 bytes/px, row-major) into the framebuffer.
 * Nearest-neighbor, respects current clip rect.
 * Not available on GPU backends — use the draw command pipeline there. */
static void hui_image_rgb(hui_rect dst, const uint8_t *rgb, int iw, int ih)
    __attribute__((unused));
static void hui_image_rgb(hui_rect dst, const uint8_t *rgb, int iw, int ih) {
    if (!rgb || iw <= 0 || ih <= 0 || dst.w <= 0 || dst.h <= 0) return;
    float sx = (float)iw / dst.w;
    float sy = (float)ih / dst.h;
    for (int dy = 0; dy < dst.h; dy++) {
        int fy = dst.y + dy;
        if (fy < 0 || fy >= hui__fb.h) continue;
        if (fy < hui__fb.clip_y0 || fy >= hui__fb.clip_y1) continue;
        int src_y = (int)((float)dy * sy); if (src_y >= ih) src_y = ih - 1;
        for (int dx = 0; dx < dst.w; dx++) {
            int fx = dst.x + dx;
            if (fx < 0 || fx >= hui__fb.w) continue;
            if (fx < hui__fb.clip_x0 || fx >= hui__fb.clip_x1) continue;
            int src_x = (int)((float)dx * sx); if (src_x >= iw) src_x = iw - 1;
            const uint8_t *p = rgb + (src_y * iw + src_x) * 3;
            hui__fb.pixels[fy * hui__fb.w + fx] =
                0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
}

/* ---- Deferred RGBA blit queue ---- */
/* hui_image_rgba*() enqueue a blit. Blits are executed INSIDE the lowest
 * draw-list layer pass, in command order: a blit issued between command k-1
 * and command k is composited right before command k, honouring the clip
 * rect active at that point. Later normal-layer draws therefore cover the
 * image (outlines, nodes, glyph fills drawn after it stay visible), while
 * popup/overlay layers always sit above every image.
 * Every blit is an inverse-affine sampling: for each destination pixel in
 * dst (logical px, clipped) the map inv[] gives the source pixel; pixels
 * mapping outside the image are skipped, which is what makes rotated and
 * sheared images work with the same loop as plain scaled ones.
 * Queue is reset by hui__blit_queue_clear() called from hui__rasterize(). */
typedef struct {
    hui_rect dst; const uint8_t *rgba; int iw, ih;
    float inv[6];               /* src = inv[0]*x + inv[1]*y + inv[2], inv[3]*x + inv[4]*y + inv[5]
                                   (x,y = logical dst pixel centre) */
    uint8_t alpha_mul;          /* 255 = opaque as stored; scales every source alpha */
    uint32_t seq;               /* draw-list command index at enqueue time */
    uint8_t layer;              /* hui__cur_layer at enqueue time: blit runs in that layer's pass */
    uint8_t done;
} hui__blit_t;
#ifndef HUI__BLIT_MAX
#define HUI__BLIT_MAX 512
#endif
static hui__blit_t hui__blit_queue[HUI__BLIT_MAX];
static int         hui__blit_count = 0;
static int         hui__blit_next  = 0;   /* next queue entry to execute */

/* ---- Per-frame scratch arena (library-owned pixels that a queued blit may
 * borrow) ----
 * The blit queue BORROWS pixel pointers until flush (see hui_image_rgba_affine).
 * That is the right contract for app-owned image assets and the wrong one for
 * scratch the LIBRARY makes on the way — hui_image_blit's bilinear resample,
 * hui_image_view's RGB/GRAY→RGBA conversion — which used to live in alloca()/
 * malloc() and was dead by flush (heap-use-after-free at hui__blit_exec,
 * 2026-09-12, tools/image_scratch_test.c). Such scratch is taken from here
 * instead. Lifetime is EXACTLY the blit queue's: reset in
 * hui__blit_queue_clear() after the blits ran, freed in hui_headless_free().
 * Pointers handed out never move within a frame (a new chunk is added, nothing
 * is realloc'd); on reset the chunks coalesce to one sized at the high-water
 * mark, so steady state is one buffer and zero allocations per frame. */
#ifndef HUI__SCRATCH_CHUNKS
#define HUI__SCRATCH_CHUNKS 8
#endif
typedef struct { uint8_t *base; size_t cap, used; } hui__scratch_chunk;
static hui__scratch_chunk hui__scratch[HUI__SCRATCH_CHUNKS];
static int    hui__scratch_n = 0;
static size_t hui__scratch_high = 0;          /* bytes used in the biggest frame so far */

static void *hui_frame_scratch(size_t n) __attribute__((unused));
static void *hui_frame_scratch(size_t n) {
    if (n == 0) return NULL;
    n = (n + 15u) & ~(size_t)15u;
    if (hui__scratch_n > 0) {
        hui__scratch_chunk *c = &hui__scratch[hui__scratch_n - 1];
        if (c->cap - c->used >= n) { void *p = c->base + c->used; c->used += n; return p; }
    }
    if (hui__scratch_n >= HUI__SCRATCH_CHUNKS) return NULL;
    size_t want = n;
    if (want < hui__scratch_high) want = hui__scratch_high;
    if (hui__scratch_n > 0 && want < hui__scratch[hui__scratch_n - 1].cap * 2)
        want = hui__scratch[hui__scratch_n - 1].cap * 2;
    uint8_t *b = (uint8_t *)malloc(want);
    if (!b) return NULL;
    hui__scratch[hui__scratch_n++] = (hui__scratch_chunk){ b, want, n };
    return b;
}
static void hui__scratch_reset(void) {
    size_t used = 0;
    for (int i = 0; i < hui__scratch_n; i++) used += hui__scratch[i].used;
    if (used > hui__scratch_high) hui__scratch_high = used;
    if (hui__scratch_n > 1) {                     /* coalesce to one chunk at high-water */
        for (int i = 0; i < hui__scratch_n; i++) free(hui__scratch[i].base);
        hui__scratch_n = 0;
        uint8_t *b = (uint8_t *)malloc(hui__scratch_high);
        if (b) hui__scratch[hui__scratch_n++] = (hui__scratch_chunk){ b, hui__scratch_high, 0 };
    } else if (hui__scratch_n == 1) {
        hui__scratch[0].used = 0;
    }
}
static void hui__scratch_free(void) {
    for (int i = 0; i < hui__scratch_n; i++) free(hui__scratch[i].base);
    hui__scratch_n = 0; hui__scratch_high = 0;
}

static void hui__blit_queue_clear(void) { hui__blit_count = 0; hui__blit_next = 0; hui__scratch_reset(); }

static void hui__blit_exec(const hui__blit_t *b) {
    hui_rect dst = b->dst;
    const uint8_t *rgba = b->rgba; int iw = b->iw, ih = b->ih;
    if (!rgba || iw <= 0 || ih <= 0 || dst.w <= 0 || dst.h <= 0) return;
    int px0, py0, px1, py1;   /* physical dst edges */
    float inv_scale = 1.0f;
    if (hui__fb_scale != 1.0f) {
        /* dst rect is in logical pixels — scale edges to physical. */
        px0 = hui__ss(dst.x); py0 = hui__ss(dst.y);
        px1 = hui__ss(dst.x + dst.w); py1 = hui__ss(dst.y + dst.h);
        inv_scale = 1.0f / hui__fb_scale;
    } else {
        px0 = dst.x; py0 = dst.y; px1 = dst.x + dst.w; py1 = dst.y + dst.h;
    }
    if (px1 <= px0 || py1 <= py0) return;
    /* clip: framebuffer bounds ∩ current clip rect (physical coords) */
    int cx0 = hui_max(0, hui__fb.clip_x0), cy0 = hui_max(0, hui__fb.clip_y0);
    int cx1 = hui_min(hui__fb.w, hui__fb.clip_x1), cy1 = hui_min(hui__fb.h, hui__fb.clip_y1);
    int fx0 = hui_max(px0, cx0), fy0 = hui_max(py0, cy0);
    int fx1 = hui_min(px1, cx1), fy1 = hui_min(py1, cy1);
    if (fx0 >= fx1 || fy0 >= fy1) return;
    const float *m = b->inv;
    uint32_t am = b->alpha_mul;
    for (int fy = fy0; fy < fy1; fy++) {
        float ly = ((float)fy + 0.5f) * inv_scale;   /* logical pixel centre */
        uint32_t *row = hui__fb.pixels + (size_t)fy * (size_t)hui__fb.w;
        /* per-row incremental mapping: src moves by (m0,m3)*inv_scale per pixel */
        float lx = ((float)fx0 + 0.5f) * inv_scale;
        float sxf = m[0]*lx + m[1]*ly + m[2];
        float syf = m[3]*lx + m[4]*ly + m[5];
        float dsx = m[0]*inv_scale, dsy = m[3]*inv_scale;
        for (int fx = fx0; fx < fx1; fx++, sxf += dsx, syf += dsy) {
            if (sxf < 0.0f || syf < 0.0f) continue;
            int src_x = (int)sxf, src_y = (int)syf;
            if (src_x >= iw || src_y >= ih) continue;
            const uint8_t *p = rgba + ((size_t)src_y * (size_t)iw + (size_t)src_x) * 4;
            uint32_t a = ((uint32_t)p[3] * am + 127u) / 255u;
            if (a == 0) continue;
            if (a >= 255) {
                row[fx] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            } else {
                uint32_t dst2 = row[fx];
                uint32_t dr = (dst2 >> 16) & 0xFF;
                uint32_t dg = (dst2 >>  8) & 0xFF;
                uint32_t db = (dst2      ) & 0xFF;
                uint32_t ia = 255u - a;
                row[fx] = 0xFF000000u |
                    (((p[0]*a + dr*ia) / 255u) << 16) |
                    (((p[1]*a + dg*ia) / 255u) <<  8) |
                     ((p[2]*a + db*ia) / 255u);
            }
        }
    }
}

/* Execute every queued blit whose seq <= upto (in order). */
static void hui__blit_flush_upto(uint32_t upto) {
    while (hui__blit_next < hui__blit_count &&
           hui__blit_queue[hui__blit_next].seq <= upto) {
        hui__blit_exec(&hui__blit_queue[hui__blit_next]);
        hui__blit_next++;
    }
}
/* Layer-aware variant: a per-pass cursor walks the queue in seq order and
 * executes only the blits enqueued in layer z, so an image/text strip queued
 * while drawing the NORMAL layer lands between the NORMAL commands around it
 * even when BG/POPUP layers are present in the same frame. */
static int hui__blit_cursor = 0;
static void hui__blit_flush_layer(uint32_t upto, uint8_t z) {
    while (hui__blit_cursor < hui__blit_count &&
           hui__blit_queue[hui__blit_cursor].seq <= upto) {
        hui__blit_t *b = &hui__blit_queue[hui__blit_cursor];
        if (!b->done && b->layer == z) { hui__blit_exec(b); b->done = 1; }
        hui__blit_cursor++;
    }
}

/* Enqueue an affine blit: dst is the (logical-px) bounding rect to scan;
 * inv maps a logical dst pixel centre to image pixel coordinates; pixels
 * mapping outside [0,iw)x[0,ih) are left untouched. alpha_mul (0..255)
 * scales source alpha (object opacity). Executed in command order inside
 * the lowest layer, under the clip rect active at that point.
 *
 * ⚠ LIFETIME CONTRACT — `rgba` is BORROWED, not copied. The queue stores the
 * pointer (b.rgba below) and reads the pixels only when the blit executes at
 * flush (hui_end_frame → hui_backend_flush → hui__blit_exec). The caller must
 * keep the buffer valid and UNCHANGED until that flush returns. A buffer freed
 * or overwritten earlier — a stack-local, a per-frame scratch, a decoder ring
 * that advanced — is undefined behaviour with no diagnostic: the blit reads
 * whatever is there and the picture is silently wrong (a plot drawn as its
 * baseline was how this was found, 2026-09-12). This applies to every entry
 * point that reaches here: hui_image_rgba, hui_image_rgba_region, and the
 * hui_image.h view/blit helpers built on them. Library-made scratch (bilinear
 * resample, RGB/GRAY→RGBA conversion) comes from hui_frame_scratch(), which
 * lives exactly as long as this queue — that is how those helpers honour the
 * contract without copying app assets (fixed 2026-09-12, image_scratch_test).
 *
 * Why borrow and not copy: these are image OBJECTS — decoded assets that live
 * for many frames — and copying megapixels per blit would be the wrong default.
 * The opposite disposition is correct for a per-frame terminal presenter whose
 * sources are scratch (the host app's c_ansi lj_ansi_image copies, ruling of
 * 2026-09-12); same contract, written where the pointer is taken, in both. */
static void hui_image_rgba_affine(hui_rect dst, const uint8_t *rgba, int iw, int ih,
                                  const float inv[6], uint8_t alpha_mul)
    __attribute__((unused));
static void hui_image_rgba_affine(hui_rect dst, const uint8_t *rgba, int iw, int ih,
                                  const float inv[6], uint8_t alpha_mul) {
    if (!rgba || iw <= 0 || ih <= 0 || dst.w <= 0 || dst.h <= 0 || !inv) return;
    if (hui__blit_count >= HUI__BLIT_MAX) return;
    hui__blit_t b;
    b.dst = dst; b.rgba = rgba; b.iw = iw; b.ih = ih;
    for (int i = 0; i < 6; i++) b.inv[i] = inv[i];
    b.alpha_mul = alpha_mul;
    b.seq = hui_g ? (uint32_t)hui_g->dl.count : 0u;
    b.layer = hui_get_layer(); b.done = 0;
    hui__blit_queue[hui__blit_count++] = b;
}

/* Enqueue an RGBA blit of the source region [sx0,sx1)x[sy0,sy1) (image
 * pixels, may be fractional) scaled onto dst. Lets a caller draw only the
 * visible slice of a large or heavily zoomed image, keeping dst inside
 * int16 range. */
static void hui_image_rgba_region(hui_rect dst, const uint8_t *rgba, int iw, int ih,
                                  float sx0, float sy0, float sx1, float sy1)
    __attribute__((unused));
static void hui_image_rgba_region(hui_rect dst, const uint8_t *rgba, int iw, int ih,
                                  float sx0, float sy0, float sx1, float sy1) {
    if (!rgba || iw <= 0 || ih <= 0 || dst.w <= 0 || dst.h <= 0) return;
    float kx = (sx1 - sx0) / (float)dst.w, ky = (sy1 - sy0) / (float)dst.h;
    if (kx <= 0.0f || ky <= 0.0f) return;
    float inv[6] = { kx, 0.0f, sx0 - (float)dst.x * kx,
                     0.0f, ky, sy0 - (float)dst.y * ky };
    hui_image_rgba_affine(dst, rgba, iw, ih, inv, 255);
}

/* Enqueue an RGBA blit — whole image scaled onto dst */
static void hui_image_rgba(hui_rect dst, const uint8_t *rgba, int iw, int ih)
    __attribute__((unused));
static void hui_image_rgba(hui_rect dst, const uint8_t *rgba, int iw, int ih) {
    hui_image_rgba_region(dst, rgba, iw, ih, 0.0f, 0.0f, (float)iw, (float)ih);
}

/* ===================================================================
 * Antialiasing — Xiaolin Wu algorithms (opt-in)
 *
 * Enable:  #define HUI_AA  before #include "hui.h"
 * Disable: #define HUI_NO_AA  (forced off; MCU/Matrix profiles set this)
 *
 * When active, hui__line_1px and hui__circle_outline switch to Wu
 * algorithms. All other primitives (filled shapes, thick lines,
 * bezier, text) are unaffected — AA is a line/stroke quality gate.
 * =================================================================== */
#if defined(HUI_AA) && !defined(HUI_NO_AA)
#  define HUI__AA_ON 1
#else
#  define HUI__AA_ON 0
#endif

/* ---- Pixel helpers ---- */

static inline int hui__in_clip(int x, int y) {
    return x >= hui__fb.clip_x0 && x < hui__fb.clip_x1 &&
           y >= hui__fb.clip_y0 && y < hui__fb.clip_y1;
}

/* current blend mode during a RECT_FILL flush (HUI_BLEND_*); 0 = normal over */
static int hui__cur_blend = 0;
static inline uint32_t hui__blend_ch(int mode, uint32_t s, uint32_t d) {
    switch (mode) {
        case HUI_BLEND_MULTIPLY: return (s*d)/255u;
        case HUI_BLEND_SCREEN:   return 255u - ((255u-s)*(255u-d))/255u;
        case HUI_BLEND_ADD:      return (s+d>255u)?255u:(s+d);
        case HUI_BLEND_DARKEN:   return s<d?s:d;
        case HUI_BLEND_LIGHTEN:  return s>d?s:d;
        default:                 return s;
    }
}

static inline void hui__put(int x, int y, hui_color c) {
    if (!hui__in_clip(x, y)) return;
    if (c.a == 0) return;
    if (hui__cur_blend) {
        /* apply the blend function per channel, then alpha-composite over dst */
        uint32_t dst = hui__fb.pixels[y * hui__fb.w + x];
        uint32_t dr=(dst>>16)&0xFF, dg=(dst>>8)&0xFF, db=dst&0xFF;
        uint32_t br=hui__blend_ch(hui__cur_blend,c.r,dr);
        uint32_t bg=hui__blend_ch(hui__cur_blend,c.g,dg);
        uint32_t bb=hui__blend_ch(hui__cur_blend,c.b,db);
        uint32_t a=c.a, ia=255-a;
        uint32_t nr=(br*a+dr*ia)/255, ng=(bg*a+dg*ia)/255, nb=(bb*a+db*ia)/255;
        hui__fb.pixels[y*hui__fb.w+x] = 0xFF000000u | (nr<<16) | (ng<<8) | nb;
        return;
    }
    if (c.a == 255) {
        hui__fb.pixels[y * hui__fb.w + x] =
            (0xFF000000u) | ((uint32_t)c.r << 16) |
            ((uint32_t)c.g << 8) | c.b;
    } else {
        /* Alpha blend over existing pixel */
        uint32_t dst = hui__fb.pixels[y * hui__fb.w + x];
        uint32_t dr = (dst >> 16) & 0xFF;
        uint32_t dg = (dst >>  8) & 0xFF;
        uint32_t db = (dst      ) & 0xFF;
        uint32_t a = c.a;
        uint32_t ia = 255 - a;
        uint32_t nr = (c.r * a + dr * ia) / 255;
        uint32_t ng = (c.g * a + dg * ia) / 255;
        uint32_t nb = (c.b * a + db * ia) / 255;
        hui__fb.pixels[y * hui__fb.w + x] =
            0xFF000000u | (nr << 16) | (ng << 8) | nb;
    }
}

static inline void hui__hline(int x0, int x1, int y, hui_color c) {
    if (x0 > x1) { int t=x0; x0=x1; x1=t; }
    int cx0 = hui__fb.clip_x0, cx1 = hui__fb.clip_x1;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (y < hui__fb.clip_y0 || y >= hui__fb.clip_y1) return;
    uint32_t col = 0xFF000000u | ((uint32_t)c.r<<16) |
                   ((uint32_t)c.g<<8) | c.b;
    if (c.a == 255 && !hui__cur_blend) {
        for (int x = x0; x < x1; x++)
            hui__fb.pixels[y * hui__fb.w + x] = col;
    } else {
        for (int x = x0; x < x1; x++)
            hui__put(x, y, c);
    }
}

/* ---- Clip stack ---- */

static void hui__clip_push(int x0, int y0, int x1, int y1) {
    if (hui__fb.clip_depth < 16) {
        hui__fb.clip_stack[hui__fb.clip_depth].x0 = hui__fb.clip_x0;
        hui__fb.clip_stack[hui__fb.clip_depth].y0 = hui__fb.clip_y0;
        hui__fb.clip_stack[hui__fb.clip_depth].x1 = hui__fb.clip_x1;
        hui__fb.clip_stack[hui__fb.clip_depth].y1 = hui__fb.clip_y1;
        hui__fb.clip_depth++;
    }
    /* Intersect with current */
    hui__fb.clip_x0 = hui_max(hui__fb.clip_x0, x0);
    hui__fb.clip_y0 = hui_max(hui__fb.clip_y0, y0);
    hui__fb.clip_x1 = hui_min(hui__fb.clip_x1, x1);
    hui__fb.clip_y1 = hui_min(hui__fb.clip_y1, y1);
}

static void hui__clip_pop(void) {
    if (hui__fb.clip_depth > 0) {
        hui__fb.clip_depth--;
        hui__fb.clip_x0 = hui__fb.clip_stack[hui__fb.clip_depth].x0;
        hui__fb.clip_y0 = hui__fb.clip_stack[hui__fb.clip_depth].y0;
        hui__fb.clip_x1 = hui__fb.clip_stack[hui__fb.clip_depth].x1;
        hui__fb.clip_y1 = hui__fb.clip_stack[hui__fb.clip_depth].y1;
    }
}

/* ---- Circle segment count (pixel-error formula from raylib) ---- */
/* Returns minimum segments so circle never deviates >0.5px from ideal. */
static inline int hui__circle_segs(float r) {
    if (r < 4.0f) return 6;
    return (int)ceilf(HUI_TAU / acosf(2.0f * powf(1.0f - 0.5f/r, 2.0f) - 1.0f));
}

/* ---- Wu AA helpers (compiled only when HUI__AA_ON) ---- */

#if HUI__AA_ON

/* Write pixel with fractional coverage multiplied into alpha.
 * cov=1.0 → full coverage; cov=0.0 → invisible. */
static inline void hui__put_cov(int x, int y, hui_color c, float cov) {
    if (cov <= 0.0f) return;
    if (cov >= 1.0f) { hui__put(x, y, c); return; }
    hui_color ca = {c.r, c.g, c.b, (uint8_t)((float)c.a * cov + 0.5f)};
    hui__put(x, y, ca);
}

/* Xiaolin Wu antialiased 1px line.
 * Handles all slopes; endpoints are included. */
static void hui__line_wu(int ix0, int iy0, int ix1, int iy1, hui_color c) {
    float x0 = (float)ix0, y0 = (float)iy0;
    float x1 = (float)ix1, y1 = (float)iy1;

    int steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep)  { float t; t=x0;x0=y0;y0=t; t=x1;x1=y1;y1=t; }
    if (x0 > x1){ float t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }

    float dx = x1 - x0;
    float dy = y1 - y0;
    float grad = (dx < 1e-6f) ? 1.0f : dy / dx;

    /* First endpoint */
    float xend = floorf(x0 + 0.5f);
    float yend = y0 + grad * (xend - x0);
    float xgap = 1.0f - (x0 + 0.5f - floorf(x0 + 0.5f));
    int px0 = (int)xend, py0 = (int)floorf(yend);
    float frac = yend - floorf(yend);
    if (steep) {
        hui__put_cov(py0,   px0, c, (1.0f-frac)*xgap);
        hui__put_cov(py0+1, px0, c, frac        *xgap);
    } else {
        hui__put_cov(px0, py0,   c, (1.0f-frac)*xgap);
        hui__put_cov(px0, py0+1, c, frac        *xgap);
    }
    float intery = yend + grad;

    /* Second endpoint */
    xend = floorf(x1 + 0.5f);
    yend = y1 + grad * (xend - x1);
    xgap = (x1 + 0.5f) - floorf(x1 + 0.5f);
    int px1 = (int)xend, py1 = (int)floorf(yend);
    frac = yend - floorf(yend);
    if (steep) {
        hui__put_cov(py1,   px1, c, (1.0f-frac)*xgap);
        hui__put_cov(py1+1, px1, c, frac        *xgap);
    } else {
        hui__put_cov(px1, py1,   c, (1.0f-frac)*xgap);
        hui__put_cov(px1, py1+1, c, frac        *xgap);
    }

    /* Main loop */
    for (int x = px0 + 1; x < px1; x++) {
        frac = intery - floorf(intery);
        int yi = (int)floorf(intery);
        if (steep) {
            hui__put_cov(yi,   x, c, 1.0f - frac);
            hui__put_cov(yi+1, x, c, frac);
        } else {
            hui__put_cov(x, yi,   c, 1.0f - frac);
            hui__put_cov(x, yi+1, c, frac);
        }
        intery += grad;
    }
}

/* Xiaolin Wu antialiased circle outline.
 * Plots two coverage-weighted pixels per octant step (8-way symmetric). */
static void hui__circle_wu(int cx, int cy, int r, hui_color c) {
    if (r <= 0) { hui__put(cx, cy, c); return; }
    float rf = (float)r;
    int limit = (int)(rf * 0.70710678f + 0.5f); /* r/sqrt(2) */
    for (int x = 0; x <= limit; x++) {
        float y  = sqrtf(rf*rf - (float)(x*x));
        int   yi = (int)floorf(y);
        float f  = y - (float)yi;   /* fractional part → outer coverage */
        float g  = 1.0f - f;        /* inner coverage  */
        /* 8-way symmetry, two stacked pixels per quadrant arm */
        hui__put_cov(cx+x, cy+yi,   c, g); hui__put_cov(cx+x, cy+yi+1, c, f);
        hui__put_cov(cx-x, cy+yi,   c, g); hui__put_cov(cx-x, cy+yi+1, c, f);
        hui__put_cov(cx+x, cy-yi,   c, g); hui__put_cov(cx+x, cy-yi-1, c, f);
        hui__put_cov(cx-x, cy-yi,   c, g); hui__put_cov(cx-x, cy-yi-1, c, f);
        hui__put_cov(cx+yi,   cy+x, c, g); hui__put_cov(cx+yi+1, cy+x, c, f);
        hui__put_cov(cx+yi,   cy-x, c, g); hui__put_cov(cx+yi+1, cy-x, c, f);
        hui__put_cov(cx-yi,   cy+x, c, g); hui__put_cov(cx-yi-1, cy+x, c, f);
        hui__put_cov(cx-yi,   cy-x, c, g); hui__put_cov(cx-yi-1, cy-x, c, f);
    }
}

#endif /* HUI__AA_ON */

/* ---- Bresenham line ---- */

static void hui__line_1px(int x0, int y0, int x1, int y1, hui_color c) {
#if HUI__AA_ON
    hui__line_wu(x0, y0, x1, y1, c);
    return;
#endif
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy;
    for(;;) {
        hui__put(x0, y0, c);
        if (x0==x1 && y0==y1) break;
        int e2 = 2*err;
        if (e2 >= dy) { if(x0==x1) break; err+=dy; x0+=sx; }
        if (e2 <= dx) { if(y0==y1) break; err+=dx; y0+=sy; }
    }
}

static void hui__circle_fill(int cx, int cy, int r, hui_color c); /* forward decl */
static void hui__tri_fill(int x0,int y0, int x1,int y1, int x2,int y2, hui_color c); /* forward decl */

static void hui__line(int x0, int y0, int x1, int y1, hui_color c, int thick) {
    if (thick <= 1) {
        hui__line_1px(x0,y0,x1,y1,c);
        return;
    }
    float dx = (float)(x1-x0), dy = (float)(y1-y0);
    float len = sqrtf(dx*dx+dy*dy);
    if (len < 0.001f) {
        hui__circle_fill(x0, y0, thick/2, c);
        return;
    }
    /* Perpendicular-quad thick line (no atan2 — one sqrtf for normalization) */
    float nx = -dy/len * ((float)thick * 0.5f);
    float ny =  dx/len * ((float)thick * 0.5f);
    int qx0 = (int)((float)x0+nx), qy0 = (int)((float)y0+ny);
    int qx1 = (int)((float)x1+nx), qy1 = (int)((float)y1+ny);
    int qx2 = (int)((float)x1-nx), qy2 = (int)((float)y1-ny);
    int qx3 = (int)((float)x0-nx), qy3 = (int)((float)y0-ny);
    hui__tri_fill(qx0,qy0, qx1,qy1, qx2,qy2, c);
    hui__tri_fill(qx0,qy0, qx2,qy2, qx3,qy3, c);
    /* Round endcaps */
    hui__circle_fill(x0, y0, thick/2, c);
    hui__circle_fill(x1, y1, thick/2, c);
}

/* ---- Midpoint circle ---- */

static void hui__circle_points(int cx, int cy, int x, int y, hui_color c) {
    hui__put(cx+x, cy+y, c); hui__put(cx-x, cy+y, c);
    hui__put(cx+x, cy-y, c); hui__put(cx-x, cy-y, c);
    hui__put(cx+y, cy+x, c); hui__put(cx-y, cy+x, c);
    hui__put(cx+y, cy-x, c); hui__put(cx-y, cy-x, c);
}

static void hui__circle_outline(int cx, int cy, int r, hui_color c) {
#if HUI__AA_ON
    hui__circle_wu(cx, cy, r, c);
    return;
#endif
    int x=0, y=r, p=1-r;
    while (x<=y) {
        hui__circle_points(cx,cy,x,y,c);
        if (p<0) { p+=2*x+3; x++; }
        else     { p+=2*(x-y)+5; x++; y--; }
    }
}

static void hui__circle_fill(int cx, int cy, int r, hui_color c) {
    int x=0, y=r, p=1-r;
    while (x<=y) {
        hui__hline(cx-y, cx+y+1, cy+x, c);
        hui__hline(cx-y, cx+y+1, cy-x, c);
        hui__hline(cx-x, cx+x+1, cy+y, c);
        hui__hline(cx-x, cx+x+1, cy-y, c);
        if (p<0) { p+=2*x+3; x++; }
        else     { p+=2*(x-y)+5; x++; y--; }
    }
}

/* ---- Arc/Hemi (angle-filtered circle) ---- */

static void hui__arc(int cx, int cy, int r, int a0deg, int a1deg, hui_color c) {
    /* Normalize angles */
    while (a0deg < 0)   a0deg += 360;
    while (a1deg < a0deg) a1deg += 360;
    /* Rasterize full circle, output only points in [a0,a1] range */
    int x=0, y=r, p=1-r;
    while (x<=y) {
        /* Check 8 symmetric points */
        int pts[8][2] = {
            {cx+x,cy+y},{cx-x,cy+y},{cx+x,cy-y},{cx-x,cy-y},
            {cx+y,cy+x},{cx-y,cy+x},{cx+y,cy-x},{cx-y,cy-x}
        };
        for (int i=0; i<8; i++) {
            int px = pts[i][0]-cx, py = pts[i][1]-cy;
            float ang = atan2f((float)-py, (float)px) * HUI_RAD2DEG;
            if (ang < 0) ang += 360.0f;
            float a = ang;
            /* Normalize into [a0, a1] check */
            int in = 0;
            if (a1deg - a0deg >= 360) {
                in = 1;
            } else {
                float fa0 = (float)a0deg;
                float fa1 = (float)a1deg;
                if (fa1 <= 360.0f) {
                    in = (a >= fa0 && a <= fa1);
                } else {
                    /* Wraps around 360 */
                    in = (a >= fa0) || (a <= fa1 - 360.0f);
                }
            }
            if (in) hui__put(pts[i][0], pts[i][1], c);
        }
        if (p<0) { p+=2*x+3; x++; }
        else     { p+=2*(x-y)+5; x++; y--; }
    }
}

/* ---- Rect ---- */

static void hui__rect_outline(int x, int y, int w, int h, hui_color c, int round) {
    if (round == 0) {
        hui__line_1px(x,    y,    x+w-1, y,     c);
        hui__line_1px(x+w-1,y,    x+w-1, y+h-1, c);
        hui__line_1px(x+w-1,y+h-1,x,    y+h-1, c);
        hui__line_1px(x,    y+h-1,x,    y,      c);
    } else {
        int r = round;
        /* Straight segments — extend 1px past arc endpoint to close junction gaps */
        hui__line_1px(x+r-1, y,     x+w-r,   y,       c);
        hui__line_1px(x+r-1, y+h-1, x+w-r,   y+h-1,   c);
        hui__line_1px(x,     y+r-1, x,       y+h-r,   c);
        hui__line_1px(x+w-1, y+r-1, x+w-1,   y+h-r,   c);
        /* Corners — quarter circles (angles use atan2(-py,px) convention: 0=right, 90=up-screen) */
        hui__arc(x+r,     y+r,     r,  90, 180, c);  /* top-left */
        hui__arc(x+w-r-1, y+r,     r,   0,  90, c);  /* top-right */
        hui__arc(x+w-r-1, y+h-r-1, r, 270, 360, c);  /* bottom-right */
        hui__arc(x+r,     y+h-r-1, r, 180, 270, c);  /* bottom-left */
    }
}

static void hui__rect_fill(int x, int y, int w, int h, hui_color c) {
    for (int row = y; row < y+h; row++)
        hui__hline(x, x+w, row, c);
}

static void hui__rect_fill_r(int x, int y, int w, int h, int r, hui_color c) {
    if (r <= 0) { hui__rect_fill(x, y, w, h, c); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;
    for (int row = y; row < y + h; row++) {
        int dy = 0;
        if (row < y + r)       dy = r - (row - y) - 1;
        else if (row >= y+h-r) dy = r - (y+h - row) - 1;
        int inset = (dy > 0) ? (int)((float)r - sqrtf((float)(r*r - dy*dy))) : 0;
        hui__hline(x + inset, x + w - inset, row, c);
    }
}

/* ---- Span-fill triangle ---- */

static void hui__tri_fill(int x0,int y0, int x1,int y1, int x2,int y2, hui_color c) {
    /* Sort vertices by y */
    if (y0 > y1) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
    if (y0 > y2) { int t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; }
    if (y1 > y2) { int t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }

    int total_h = y2 - y0;
    if (total_h == 0) return;

    for (int y = y0; y <= y2; y++) {
        int second_half = (y > y1 || y1 == y0);
        int seg_h = second_half ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0) seg_h = 1;

        float alpha = (float)(y - y0) / (float)total_h;
        float beta  = second_half
                    ? (float)(y - y1) / (float)seg_h
                    : (float)(y - y0) / (float)seg_h;

        int ax = x0 + (int)((float)(x2 - x0) * alpha);
        int bx = second_half
               ? x1 + (int)((float)(x2 - x1) * beta)
               : x0 + (int)((float)(x1 - x0) * beta);

        if (ax > bx) { int t=ax; ax=bx; bx=t; }
        hui__hline(ax, bx+1, y, c);
    }
}

static void hui__tri_outline(int x0,int y0, int x1,int y1, int x2,int y2, hui_color c) {
    hui__line_1px(x0,y0,x1,y1,c);
    hui__line_1px(x1,y1,x2,y2,c);
    hui__line_1px(x2,y2,x0,y0,c);
}

/* ---- Cubic bezier — recursive subdivision ---- */

static void hui__bezier4_rec(float x0,float y0, float x1,float y1,
                              float x2,float y2, float x3,float y3,
                              hui_color c, int depth) {
    if (depth > 8) {
        hui__line_1px((int)x0,(int)y0,(int)x3,(int)y3,c);
        return;
    }
    /* Flatness check: is the curve close enough to a line? */
    float dx = x3-x0, dy = y3-y0;
    float d1 = fabsf((x1-x3)*dy - (y1-y3)*dx);
    float d2 = fabsf((x2-x3)*dy - (y2-y3)*dx);
    float len2 = dx*dx + dy*dy;
    if ((d1+d2)*(d1+d2) <= 0.5f * len2) {
        hui__line_1px((int)x0,(int)y0,(int)x3,(int)y3,c);
        return;
    }
    /* De Casteljau subdivision */
    float mx01x = (x0+x1)*0.5f, mx01y = (y0+y1)*0.5f;
    float mx12x = (x1+x2)*0.5f, mx12y = (y1+y2)*0.5f;
    float mx23x = (x2+x3)*0.5f, mx23y = (y2+y3)*0.5f;
    float mx012x = (mx01x+mx12x)*0.5f, mx012y = (mx01y+mx12y)*0.5f;
    float mx123x = (mx12x+mx23x)*0.5f, mx123y = (mx12y+mx23y)*0.5f;
    float midx = (mx012x+mx123x)*0.5f, midy = (mx012y+mx123y)*0.5f;
    hui__bezier4_rec(x0,y0, mx01x,mx01y, mx012x,mx012y, midx,midy, c, depth+1);
    hui__bezier4_rec(midx,midy, mx123x,mx123y, mx23x,mx23y, x3,y3, c, depth+1);
}

/* ---- Arrow ---- */

static void hui__arrow(int x0,int y0, int x1,int y1, hui_color c) {
    hui__line_1px(x0,y0,x1,y1,c);
    /* Arrowhead: two short lines back from tip */
    float dx = (float)(x0-x1), dy = (float)(y0-y1);
    float len = sqrtf(dx*dx+dy*dy);
    if (len < 1.0f) return;
    dx /= len; dy /= len;
    float hs = 10.0f; /* head size */
    float spread = 0.4f;
    int ax0 = x1 + (int)((dx + dy*spread)*hs);
    int ay0 = y1 + (int)((dy - dx*spread)*hs);
    int ax1 = x1 + (int)((dx - dy*spread)*hs);
    int ay1 = y1 + (int)((dy + dx*spread)*hs);
    hui__line_1px(x1,y1,ax0,ay0,c);
    hui__line_1px(x1,y1,ax1,ay1,c);
}

/* ---- Text rendering ---- */

static void hui__text_s(int x, int y, const char *s, hui_color c, int scale) {
    if (!s || scale < 1) return;
    int cx = x;
    while (*s) {
        uint32_t cp = hui_utf8_next(&s);
        if (cp == '\n') { cx = x; y += (HUI_FONT_H + 1) * scale; continue; }
        /* one '?' per non-ASCII codepoint keeps advance == hui_text_width */
        const uint8_t *g = hui_font_glyph(cp < 0x80 ? (unsigned char)cp : '?');
        for (int row = 0; row < HUI_FONT_H; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < HUI_FONT_W; col++) {
                if (bits & (1 << col)) {  /* font data is LSB-first */
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            hui__put(cx + col*scale + dx, y + row*scale + dy, c);
                }
            }
        }
        cx += (HUI_FONT_W + 1) * scale;
    }
}

HUI_MAYBE_UNUSED static void hui__text(int x, int y, const char *s, hui_color c) {
    hui__text_s(x, y, s, c, 1);
}

/* ---- Core rasterizer (shared by headless + x11 + sdl2 backends) ---- */

/* Render all commands with the given z_layer value */
static void hui__rasterize_layer(const hui_cmd *cmds, uint16_t count,
                                  const char *strpool, const float *datapool,
                                  uint8_t z, int inline_blits) {
    (void)datapool;
    /* Reset clip to full screen at the start of each layer pass */
    hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
    hui__fb.clip_x1 = hui__fb.w; hui__fb.clip_y1 = hui__fb.h;
    hui__fb.clip_depth = 0;
    hui__blit_cursor = 0; (void)inline_blits;

    for (uint16_t i = 0; i < count; i++) {
        const hui_cmd *cmd = &cmds[i];
        /* blits queued in this layer before command i land here, under the current clip */
        hui__blit_flush_layer(i, z);
        if (cmd->z_layer != z) continue;
        hui_color c = {cmd->col_r, cmd->col_g, cmd->col_b, cmd->col_a};

        switch ((hui_cmd_type)cmd->type) {
        case HUI_CMD_NOP:
            break;

        case HUI_CMD_LINE:
            hui__line(hui__ss(cmd->x0), hui__ss(cmd->y0),
                      hui__ss(cmd->x1), hui__ss(cmd->y1), c, hui__ss_thick(cmd->thick));
            break;

        case HUI_CMD_BEZIER4:
            hui__bezier4_rec((float)cmd->x0*hui__fb_scale,(float)cmd->y0*hui__fb_scale,
                             (float)cmd->x1*hui__fb_scale,(float)cmd->y1*hui__fb_scale,
                             (float)cmd->x2*hui__fb_scale,(float)cmd->y2*hui__fb_scale,
                             (float)cmd->x3*hui__fb_scale,(float)cmd->y3*hui__fb_scale, c, 0);
            break;

        case HUI_CMD_RECT:
            hui__rect_outline(hui__ss(cmd->x0), hui__ss(cmd->y0),
                              hui__ss_span(cmd->x0, cmd->x1),
                              hui__ss_span(cmd->y0, cmd->y1), c,
                              hui__ss(cmd->rounding));
            break;

        case HUI_CMD_RECT_FILL:
            hui__cur_blend = (cmd->flags >> 3) & 7;
            hui__rect_fill_r(hui__ss(cmd->x0), hui__ss(cmd->y0),
                             hui__ss_span(cmd->x0, cmd->x1),
                             hui__ss_span(cmd->y0, cmd->y1),
                             hui__ss(cmd->rounding), c);
            hui__cur_blend = 0;
            break;

        case HUI_CMD_CIRCLE:
            hui__circle_outline(hui__ss(cmd->x0), hui__ss(cmd->y0), hui__ss(cmd->x1), c);
            break;

        case HUI_CMD_CIRCLE_FILL:
            hui__circle_fill(hui__ss(cmd->x0), hui__ss(cmd->y0), hui__ss(cmd->x1), c);
            break;

        case HUI_CMD_HEMI:
            /* x2/x3 are angles in degrees — never scaled */
            hui__arc(hui__ss(cmd->x0), hui__ss(cmd->y0), hui__ss(cmd->x1),
                     cmd->x2, cmd->x3, c);
            break;

        case HUI_CMD_TRIANGLE:
            hui__tri_outline(hui__ss(cmd->x0),hui__ss(cmd->y0),
                             hui__ss(cmd->x1),hui__ss(cmd->y1),
                             hui__ss(cmd->x2),hui__ss(cmd->y2), c);
            break;

        case HUI_CMD_TRIANGLE_FILL:
            hui__tri_fill(hui__ss(cmd->x0),hui__ss(cmd->y0),
                          hui__ss(cmd->x1),hui__ss(cmd->y1),
                          hui__ss(cmd->x2),hui__ss(cmd->y2), c);
            break;

        case HUI_CMD_ARROW:
            hui__arrow(hui__ss(cmd->x0), hui__ss(cmd->y0),
                       hui__ss(cmd->x1), hui__ss(cmd->y1), c);
            break;

        case HUI_CMD_IMAGE: {
            /* Headless: draw a tinted placeholder rect */
            hui__rect_fill(hui__ss(cmd->x0), hui__ss(cmd->y0),
                           hui__ss_span(cmd->x0, cmd->x1),
                           hui__ss_span(cmd->y0, cmd->y1), c);
            break;
        }

        case HUI_CMD_TEXT:
#ifndef HUI_HEADLESS_SKIP_TEXT
            if (strpool) {
                int scale = cmd->thick > 0 ? cmd->thick : 1;
                if (hui__fb_scale != 1.0f) {
                    int s = (int)((float)scale * hui__fb_scale + 0.5f);
                    hui__text_s(hui__ss(cmd->x0), hui__ss(cmd->y0),
                                strpool + cmd->x2, c, s > 0 ? s : 1);
                } else {
                    hui__text_s(cmd->x0, cmd->y0, strpool + cmd->x2, c, scale);
                }
            }
#endif
            break;

        case HUI_CMD_CLIP_PUSH:
            hui__clip_push(hui__ss(cmd->x0), hui__ss(cmd->y0),
                           hui__ss(cmd->x0+cmd->x1), hui__ss(cmd->y0+cmd->y1));
            break;

        case HUI_CMD_CLIP_POP:
            hui__clip_pop();
            break;

        /* Plot commands — not yet implemented in headless */
        case HUI_CMD_PLOT_LINE:
        case HUI_CMD_PLOT_BAR:
        case HUI_CMD_PLOT_HEAT:
        case HUI_CMD_SHADER:
            break;

        default:
            break;
        }
    }
}

/* Multi-pass z-sorted rasterizer: renders lower z_layer values first */
#if defined(__GNUC__)
extern __attribute__((weak)) void (*g_overlay_layer_hook)(uint8_t layer);
extern __attribute__((weak)) void (*g_overlay_finish_hook)(void);
#endif

static void hui__layer_add(uint8_t *layers, int *n_layers, uint8_t z) {
    for (int j = 0; j < *n_layers; j++)
        if (layers[j] == z) return;
    if (*n_layers < 256)
        layers[(*n_layers)++] = z;
}

static void hui__rasterize(const hui_cmd *cmds, uint16_t count,
                            const char *strpool, const float *datapool) {
    /* Collect unique z_layer values (typically ≤4) */
    uint8_t layers[256];
    int     n_layers = 0;
    for (uint16_t i = 0; i < count; i++) {
        hui__layer_add(layers, &n_layers, cmds[i].z_layer);
    }
#if defined(__GNUC__)
    if (&g_overlay_layer_hook && g_overlay_layer_hook) {
        hui__layer_add(layers, &n_layers, HUI_LAYER_NORMAL);
        hui__layer_add(layers, &n_layers, HUI_LAYER_POPUP);
        hui__layer_add(layers, &n_layers, HUI_LAYER_OVERLAY);
    }
#endif
    /* Insertion-sort the layer values */
    for (int i = 1; i < n_layers; i++) {
        uint8_t key = layers[i]; int j = i - 1;
        while (j >= 0 && layers[j] > key) { layers[j+1] = layers[j]; j--; }
        layers[j+1] = key;
    }
    /* Render layers; RGBA blits are composited in command order inside the
     * first (lowest) layer pass, so they sit under later normal-layer draws
     * but below every overlay/popup layer. Any blit queued after the last
     * command flushes at the end of that pass. */
    for (int li = 0; li < n_layers; li++) {
        hui__rasterize_layer(cmds, count, strpool, datapool, layers[li], li == 0);
        hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
        hui__fb.clip_x1 = hui__fb.w; hui__fb.clip_y1 = hui__fb.h;
        hui__blit_flush_layer(0xFFFFFFFFu, layers[li]);   /* blits queued after the last cmd of this layer */
#if defined(__GNUC__)
        if (&g_overlay_layer_hook && g_overlay_layer_hook)
            g_overlay_layer_hook(layers[li]);
#endif
    }
    /* Blits whose layer had no draw-list commands (or no layers at all): flush on top */
    {
        hui__fb.clip_x0 = 0; hui__fb.clip_y0 = 0;
        hui__fb.clip_x1 = hui__fb.w; hui__fb.clip_y1 = hui__fb.h;
        for (int bi = 0; bi < hui__blit_count; bi++)
            if (!hui__blit_queue[bi].done) { hui__blit_exec(&hui__blit_queue[bi]); hui__blit_queue[bi].done = 1; }
    }
#if defined(__GNUC__)
    if (&g_overlay_finish_hook && g_overlay_finish_hook)
        g_overlay_finish_hook();
#endif
    hui__blit_queue_clear(); /* reset for next frame */
}

/* ---- hui_backend_flush for pure headless (PPM) use ---- */
/* If another backend (X11, SDL2) includes this header, it defines its
 * own hui_backend_flush and skips this one via HUI_HEADLESS_NO_FLUSH. */
#ifndef HUI_HEADLESS_NO_FLUSH
#ifdef HUI_IMPLEMENTATION   /* multi-TU: defined ONCE (hui/tools/multi_tu_test, 2026-09-12) */
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    hui__rasterize(cmds, count, strpool, datapool);
}
#endif /* HUI_IMPLEMENTATION — one definition per program; the declaration in hui.h stays visible to every TU */
#endif

#ifdef __cplusplus
}
#endif

#endif /* HUI_HEADLESS_H */
