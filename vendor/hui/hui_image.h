/*
 * hui_image.h — Image widget + spritesheet API for hui
 *
 * stb-style single-header. Gate implementation with:
 *   #define HUI_IMAGE_IMPLEMENTATION
 *   #include "hui_image.h"
 *
 * Optional stb_image integration:
 *   #define HUI_IMAGE_USE_STB   (before implementation block)
 *   Requires "../deps/stb_image.h" to be present.
 *
 * Without HUI_IMAGE_USE_STB: minimal P6 PPM loader only.
 *
 * Dependencies: hui_math.h, hui_cum.h (for CUM_BG3 outline color)
 * Do NOT include any backend header from here.
 */

#ifndef HUI_IMAGE_H
#define HUI_IMAGE_H

#include <stdint.h>
#include <alloca.h>
#include "hui_math.h"
#include "hui_cum.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types ------------------------------------------------------------ */

typedef enum {
    HUI_IMG_FMT_RGB  = 0,
    HUI_IMG_FMT_RGBA = 1,
    HUI_IMG_FMT_GRAY = 2,
} hui_img_fmt;

typedef enum {
    HUI_IMG_SCALE_NEAREST  = 0,
    HUI_IMG_SCALE_BILINEAR = 1,
} hui_img_scale;

typedef struct {
    const uint8_t *data;
    int w, h, stride; /* stride = bytes per row; 0 = auto (w * channels) */
    hui_img_fmt fmt;
} hui_buf_desc;

/* Texture registration — used by GPU backends (headless uses pixels directly) */
typedef uint32_t hui_tex_id;
#define HUI_TEX_INVALID 0

/* Icon spritesheet */
typedef struct {
    uint8_t  *pixels;                /* RGBA, heap-owned */
    int       sheet_w, sheet_h;
    int       cell_w,  cell_h;
    int       cols,    rows;
    hui_tex_id tex;                  /* GPU tex (0 = not uploaded) */
} hui_sheet;

/* ---- API declarations ------------------------------------------------- */

/* Load image from file → caller-owned RGBA buffer, or NULL on failure.
 * With HUI_IMAGE_USE_STB: delegates to stbi_load.
 * Without: minimal PPM (P6) loader only. */
uint8_t   *hui_image_load(const char *path, int *w, int *h);
void       hui_image_free(uint8_t *pixels);

/* Widget: display image inside rect r with given scale mode.
 * Draws a 1px CUM_BG3 outline around r. */
/* ⚠ LIFETIME: on the headless-derived backends (headless, X11, fb, canvas,
 * rp2350) an image blit is QUEUED and the pixel pointer is BORROWED until
 * hui_end_frame's flush — the caller keeps `rgba`/`bd->data` valid and
 * unchanged until then; an earlier free or overwrite draws garbage with no
 * diagnostic. Contract text lives where the pointer is taken:
 * backends/hui_headless.h, hui_image_rgba_affine.
 * Library-made scratch (hui_image_blit's BILINEAR resample, hui_image_view's
 * RGB/GRAY→RGBA conversion) is taken from the backend's per-frame arena
 * (hui_frame_scratch, lifetime == the blit queue), so those paths honour the
 * contract without copying the app's asset. Before 2026-09-12 they used
 * alloca()/malloc() and read freed memory at flush — tools/image_scratch_test.c
 * is the planted proof, fail-before / pass-after. */
void       hui_image_view(hui_rect r, const hui_buf_desc *bd, hui_img_scale scale);

/* Low-level RGBA blit — no buf_desc wrapper required. */
void       hui_image_blit(hui_rect r, const uint8_t *rgba, int iw, int ih,
                          hui_img_scale scale);

/* Multiplicative tint — returns heap-owned RGBA copy, caller must free. */
uint8_t   *hui_image_tint(const uint8_t *rgba, int w, int h, hui_color tint);

/* Spritesheet */
hui_sheet *hui_sheet_load(const char *path, int cell_w, int cell_h);
void       hui_sheet_free(hui_sheet *s);
void       hui_icon(hui_rect r, const hui_sheet *s, int cell_idx);

#ifdef __cplusplus
}
#endif

/* ======================================================================= */
/* IMPLEMENTATION                                                           */
/* ======================================================================= */

#ifdef HUI_IMAGE_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HUI_IMAGE_USE_STB
#  define STB_IMAGE_IMPLEMENTATION
#  include "../deps/stb_image.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Forward-declare the backend blit function.
 * Provided by whichever backend is compiled in (headless, x11, fb, …). */
extern void hui_image_rgba(hui_rect dst, const uint8_t *rgba, int iw, int ih);

/* Blit a raw 24-bit RGB (3 bytes/pixel) frame — converts to RGBA and enqueues.
 * rgba_tmp must be caller-owned scratch of at least iw*ih*4 bytes.
 * Use for live camera/video frames without touching internal framebuffer state. */
static inline void hui_blit_rgb24(hui_rect dst, const uint8_t *rgb,
                                  int iw, int ih, uint8_t *rgba_tmp) {
    int n = iw * ih;
    for (int i = 0; i < n; i++) {
        rgba_tmp[i*4+0] = rgb[i*3+0];
        rgba_tmp[i*4+1] = rgb[i*3+1];
        rgba_tmp[i*4+2] = rgb[i*3+2];
        rgba_tmp[i*4+3] = 255;
    }
    hui_image_rgba(dst, rgba_tmp, iw, ih);
}

/* Forward-declare hui_rect_outline for the 1px border. */
extern void hui_rect_outline(hui_rect r, hui_color c, uint8_t rounding);

/* ---- hui_image_load --------------------------------------------------- */

#ifdef HUI_IMAGE_USE_STB

uint8_t *hui_image_load(const char *path, int *w, int *h) {
    int ch;
    return stbi_load(path, w, h, &ch, 4);
}

#else /* minimal P6 PPM loader */

uint8_t *hui_image_load(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || magic[0] != 'P' || magic[1] != '6') {
        fclose(f); return NULL;
    }

    /* Skip comments */
    int c;
    while ((c = fgetc(f)) == '\n' || c == '\r' || c == ' ') {}
    ungetc(c, f);
    while ((c = fgetc(f)) == '#') { while ((c = fgetc(f)) != '\n' && c != EOF) {} }
    ungetc(c, f);

    int width = 0, height = 0, maxval = 0;
    if (fscanf(f, " %d %d %d", &width, &height, &maxval) != 3
        || width <= 0 || height <= 0 || maxval != 255) {
        fclose(f); return NULL;
    }
    /* consume single whitespace after header */
    fgetc(f);

    int npix = width * height;
    uint8_t *rgb = (uint8_t *)malloc((size_t)npix * 3);
    if (!rgb) { fclose(f); return NULL; }
    if ((int)fread(rgb, 3, (size_t)npix, f) != npix) {
        free(rgb); fclose(f); return NULL;
    }
    fclose(f);

    uint8_t *rgba = (uint8_t *)malloc((size_t)npix * 4);
    if (!rgba) { free(rgb); return NULL; }
    for (int i = 0; i < npix; i++) {
        rgba[i*4+0] = rgb[i*3+0];
        rgba[i*4+1] = rgb[i*3+1];
        rgba[i*4+2] = rgb[i*3+2];
        rgba[i*4+3] = 255;
    }
    free(rgb);
    *w = width;
    *h = height;
    return rgba;
}

#endif /* HUI_IMAGE_USE_STB */

/* ---- hui_image_free --------------------------------------------------- */

void hui_image_free(uint8_t *pixels) {
    free(pixels);
}

/* ---- hui_image_blit --------------------------------------------------- */

void hui_image_blit(hui_rect r, const uint8_t *rgba, int iw, int ih,
                    hui_img_scale scale) {
    if (!rgba || iw <= 0 || ih <= 0 || r.w <= 0 || r.h <= 0) return;

    if (scale == HUI_IMG_SCALE_NEAREST) {
        hui_image_rgba(r, rgba, iw, ih);
        return;
    }

    /* Bilinear: pre-generate a scaled RGBA buffer sized (r.w x r.h).
     * ⚠ From the backend's PER-FRAME SCRATCH ARENA, never alloca/malloc: the
     * blit below only QUEUES a borrow of `buf` and executes at flush, so the
     * buffer must live until then (2026-09-12, tools/image_scratch_test.c). */
    int npix = r.w * r.h;
    int bufsz = npix * 4;
    uint8_t *buf = (uint8_t *)hui_frame_scratch((size_t)bufsz);
    if (!buf) { hui_image_rgba(r, rgba, iw, ih); return; } /* fallback: nearest */

    float sx = (float)(iw - 1) / (r.w  > 1 ? r.w  - 1 : 1);
    float sy = (float)(ih - 1) / (r.h > 1 ? r.h - 1 : 1);

    for (int dy = 0; dy < r.h; dy++) {
        float fy = dy * sy;
        int y0 = (int)fy; if (y0 >= ih - 1) y0 = ih - 2 < 0 ? 0 : ih - 2;
        int y1 = y0 + 1;  if (y1 >= ih)     y1 = ih - 1;
        float ty = fy - (float)y0;

        for (int dx = 0; dx < r.w; dx++) {
            float fx = dx * sx;
            int x0 = (int)fx; if (x0 >= iw - 1) x0 = iw - 2 < 0 ? 0 : iw - 2;
            int x1 = x0 + 1;  if (x1 >= iw)     x1 = iw - 1;
            float tx = fx - (float)x0;

            const uint8_t *p00 = rgba + (y0 * iw + x0) * 4;
            const uint8_t *p10 = rgba + (y0 * iw + x1) * 4;
            const uint8_t *p01 = rgba + (y1 * iw + x0) * 4;
            const uint8_t *p11 = rgba + (y1 * iw + x1) * 4;

            uint8_t *out = buf + (dy * r.w + dx) * 4;
            for (int ch = 0; ch < 4; ch++) {
                float v = (1.0f - ty) * ((1.0f - tx) * p00[ch] + tx * p10[ch])
                        +          ty * ((1.0f - tx) * p01[ch] + tx * p11[ch]);
                out[ch] = (uint8_t)(v + 0.5f);
            }
        }
    }

    hui_image_rgba(r, buf, r.w, r.h);   /* arena-owned until the flush; no free */
}

/* ---- hui_image_view --------------------------------------------------- */

void hui_image_view(hui_rect r, const hui_buf_desc *bd, hui_img_scale scale) {
    if (!bd || !bd->data || bd->w <= 0 || bd->h <= 0) return;

    int npix = bd->w * bd->h;
    int bufsz = npix * 4;
    /* Converted pixels come from the backend's per-frame scratch arena: the
     * blit queues a borrow until flush (see hui_image_blit). */

    if (bd->fmt == HUI_IMG_FMT_RGBA) {
        /* Use data directly — no conversion needed; the app's buffer is borrowed */
        hui_image_blit(r, bd->data, bd->w, bd->h, scale);
    } else if (bd->fmt == HUI_IMG_FMT_RGB) {
        uint8_t *rgba = (uint8_t *)hui_frame_scratch((size_t)bufsz);
        if (!rgba) return;
        int src_stride = bd->stride > 0 ? bd->stride : bd->w * 3;
        for (int y = 0; y < bd->h; y++) {
            const uint8_t *row = bd->data + y * src_stride;
            uint8_t *dst = rgba + y * bd->w * 4;
            for (int x = 0; x < bd->w; x++) {
                dst[x*4+0] = row[x*3+0];
                dst[x*4+1] = row[x*3+1];
                dst[x*4+2] = row[x*3+2];
                dst[x*4+3] = 255;
            }
        }
        hui_image_blit(r, rgba, bd->w, bd->h, scale);
    } else { /* HUI_IMG_FMT_GRAY */
        uint8_t *rgba = (uint8_t *)hui_frame_scratch((size_t)bufsz);
        if (!rgba) return;
        int src_stride = bd->stride > 0 ? bd->stride : bd->w;
        for (int y = 0; y < bd->h; y++) {
            const uint8_t *row = bd->data + y * src_stride;
            uint8_t *dst = rgba + y * bd->w * 4;
            for (int x = 0; x < bd->w; x++) {
                uint8_t g = row[x];
                dst[x*4+0] = g;
                dst[x*4+1] = g;
                dst[x*4+2] = g;
                dst[x*4+3] = 255;
            }
        }
        hui_image_blit(r, rgba, bd->w, bd->h, scale);
    }

    /* 1px outline */
    hui_color outline = CUM_BG3;
    hui_rect_outline(r, outline, 0);
}

/* ---- hui_image_tint --------------------------------------------------- */

uint8_t *hui_image_tint(const uint8_t *rgba, int w, int h, hui_color tint) {
    if (!rgba || w <= 0 || h <= 0) return NULL;
    int npix = w * h;
    uint8_t *out = (uint8_t *)malloc((size_t)npix * 4);
    if (!out) return NULL;
    for (int i = 0; i < npix; i++) {
        out[i*4+0] = (uint8_t)((rgba[i*4+0] * tint.r) / 255);
        out[i*4+1] = (uint8_t)((rgba[i*4+1] * tint.g) / 255);
        out[i*4+2] = (uint8_t)((rgba[i*4+2] * tint.b) / 255);
        out[i*4+3] = (uint8_t)((rgba[i*4+3] * tint.a) / 255);
    }
    return out;
}

/* ---- Spritesheet ------------------------------------------------------ */

hui_sheet *hui_sheet_load(const char *path, int cell_w, int cell_h) {
    int sw, sh;
    uint8_t *pixels = hui_image_load(path, &sw, &sh);
    if (!pixels) return NULL;

    hui_sheet *s = (hui_sheet *)malloc(sizeof(hui_sheet));
    if (!s) { free(pixels); return NULL; }

    s->pixels   = pixels;
    s->sheet_w  = sw;
    s->sheet_h  = sh;
    s->cell_w   = cell_w;
    s->cell_h   = cell_h;
    s->cols     = (cell_w > 0) ? sw / cell_w : 0;
    s->rows     = (cell_h > 0) ? sh / cell_h : 0;
    s->tex      = HUI_TEX_INVALID;
    return s;
}

void hui_sheet_free(hui_sheet *s) {
    if (!s) return;
    free(s->pixels);
    free(s);
}

void hui_icon(hui_rect r, const hui_sheet *s, int cell_idx) {
    if (!s || !s->pixels || s->cols <= 0 || s->rows <= 0) return;

    int total = s->cols * s->rows;
    if (cell_idx < 0)       cell_idx = 0;
    if (cell_idx >= total)  cell_idx = total - 1;

    int col = cell_idx % s->cols;
    int row = cell_idx / s->cols;
    int sx  = col * s->cell_w;
    int sy  = row * s->cell_h;

    int cellsz = s->cell_w * s->cell_h * 4;
    uint8_t *buf = (uint8_t *)malloc((size_t)cellsz);
    if (!buf) return;

    for (int y = 0; y < s->cell_h; y++) {
        const uint8_t *src = s->pixels + ((sy + y) * s->sheet_w + sx) * 4;
        uint8_t       *dst = buf + y * s->cell_w * 4;
        memcpy(dst, src, (size_t)s->cell_w * 4);
    }

    hui_image_rgba(r, buf, s->cell_w, s->cell_h);
    free(buf);
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_IMAGE_IMPLEMENTATION */

#endif /* HUI_IMAGE_H */
