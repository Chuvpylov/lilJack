/*
 * hui_ttf.h — TTF/OTF font loading and atlas baking for hui
 *
 * Uses deps/stb_truetype.h for rasterisation.
 * Blits glyphs directly into the headless ARGB framebuffer (hui__fb.pixels).
 * Falls back to a <text> SVG element on the SVG backend.
 *
 * USAGE:
 *   #define HUI_TTF_IMPLEMENTATION   // exactly one TU, after #include "hui.h"
 *   #include "hui_ttf.h"
 *
 *   // load once
 *   hui_ttf_font *f = hui_ttf_load("deps/fonts/CascadiaMono-Regular.ttf", 13.0f);
 *
 *   // inside hui_begin_frame / hui_end_frame
 *   hui_ttf_text(f, 10, 20, "HELLO", HUI_RGB(0xf0, 0xed, 0xe6));
 *   int w = hui_ttf_text_width(f, "HELLO");
 *
 *   // free when done
 *   hui_ttf_free(f);
 *
 * NOTES:
 *   - One font handle = one baked size.  Load separately for 11px, 13px, etc.
 *   - Atlas: 512×512 greyscale, ASCII 32-126 baked at load time.
 *   - Pixel coords: (x, y) is the left edge of the text baseline.
 *   - Framebuffer format: ARGB packed uint32_t  (0xAARRGGBB).
 *   - Requires hui.h + backend included first in the same TU.
 */

#ifndef HUI_TTF_H
#define HUI_TTF_H

#include "hui_math.h"   /* hui_color, HUI_RGB, HUI_RGBA */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Public types + constants                                             */
/* ------------------------------------------------------------------ */

#define HUI_TTF_ATLAS_W  512
#define HUI_TTF_ATLAS_H  512
#define HUI_TTF_FIRST     32   /* first baked codepoint (space) */
#define HUI_TTF_LAST     126   /* last  baked codepoint (~)      */
#define HUI_TTF_GLYPHS   (HUI_TTF_LAST - HUI_TTF_FIRST + 1)

typedef struct hui_ttf_font hui_ttf_font;

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/** Load a TTF/OTF file and bake an atlas at the given pixel height.
 *  Returns NULL on failure. */
hui_ttf_font *hui_ttf_load(const char *path, float size_px);

/** Free all resources.  Safe to call with NULL. */
void hui_ttf_free(hui_ttf_font *f);

/** Clip subsequently-drawn text to a LOGICAL rectangle (scaled to framebuffer
 *  pixels internally). Off by default. Keeps smooth-scrolled editor text inside
 *  its viewport. hui_ttf_clear_clip() restores the full range. */
void hui_ttf_set_clip(int x0, int y0, int x1, int y1);
void hui_ttf_clear_clip(void);

/** Attach (or detach with NULL) a hi-DPI companion font: the same face baked
 *  at a larger pixel height.  While attached, hui_ttf_text(f, …) scales the
 *  pen position by (hires->size_px / f->size_px) and rasterizes with the
 *  companion — measurement (hui_ttf_text_width) and layout stay on f, in
 *  logical pixels.  The companion is caller-owned. */
void hui_ttf_set_hires(hui_ttf_font *f, hui_ttf_font *hires);

/** Draw UTF-8 text at pixel position (x, y = baseline).
 *  On headless: blends into hui__fb.
 *  On SVG:      emits a <text> element. */
void hui_ttf_text(const hui_ttf_font *f, int x, int y,
                  const char *str, hui_color col);

/** Pixel width of str at this font size. */
int hui_ttf_text_width(const hui_ttf_font *f, const char *str);

/** Recommended line spacing in pixels (size + gap). */
int hui_ttf_line_height(const hui_ttf_font *f);

/** Pixels from top of line box to baseline (positive). */
int hui_ttf_ascent(const hui_ttf_font *f);

/** Chain a fallback font for codepoints missing from the primary font. */
void hui_ttf_set_fallback(hui_ttf_font *f, hui_ttf_font *fallback);

/** True if this font or one of its fallbacks has a glyph for codepoint. */
int hui_ttf_has_codepoint(const hui_ttf_font *f, uint32_t codepoint);

/* ------------------------------------------------------------------ */
/* Implementation                                                       */
/* ------------------------------------------------------------------ */

#ifdef HUI_TTF_IMPLEMENTATION

#if defined(HUI_BACKEND_HEADLESS) || defined(HUI_BACKEND_X11) || defined(HUI_BACKEND_CANVAS)
#include <dlfcn.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "deps/stb_truetype.h"

struct hui_ttf_font {
    stbtt_bakedchar glyphs[HUI_TTF_GLYPHS];
    uint8_t        *atlas;     /* HUI_TTF_ATLAS_W × HUI_TTF_ATLAS_H greyscale */
    unsigned char  *font_data; /* retained for dynamic Unicode glyph raster */
    stbtt_fontinfo  info;
    float           size_px;
    float           scale;
    int             ascent;    /* pixels above baseline (positive) */
    int             atlas_ok;
    int             pango_line_h;
    int             pango_ascent;
    char            pango_family[64];
    hui_ttf_font   *fallback;
    /* Optional hi-DPI companion: same face baked at size_px × hires_scale.
     * When set, hui_ttf_text scales the pen position and draws with the
     * companion instead — layout/measurement stay on the base font (logical
     * pixels).  stb_truetype advances accumulate in float, so the companion's
     * run width is exactly hires_scale × the base run width (no grid drift).
     * Owned by the caller (attach/detach); hui_ttf_free does not free it. */
    hui_ttf_font   *hires;
    float           hires_scale;
};

static uint32_t hui__ttf_utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    if (!p || !*p) return 0;

    uint32_t cp;
    unsigned char c = *p++;
    if (c < 0x80) {
        *pp = (const char *)p;
        return c;
    }

    if ((c & 0xE0) == 0xC0) {
        if ((p[0] & 0xC0) != 0x80) { *pp = (const char *)p; return 0xFFFD; }
        cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[0] & 0x3F);
        p += 1;
        if (cp < 0x80) cp = 0xFFFD;
    } else if ((c & 0xF0) == 0xE0) {
        if ((p[0] & 0xC0) != 0x80 || (p[1] & 0xC0) != 0x80) {
            *pp = (const char *)p;
            return 0xFFFD;
        }
        cp = ((uint32_t)(c & 0x0F) << 12) |
             ((uint32_t)(p[0] & 0x3F) << 6) |
             (uint32_t)(p[1] & 0x3F);
        p += 2;
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    } else if ((c & 0xF8) == 0xF0) {
        if ((p[0] & 0xC0) != 0x80 || (p[1] & 0xC0) != 0x80 ||
            (p[2] & 0xC0) != 0x80) {
            *pp = (const char *)p;
            return 0xFFFD;
        }
        cp = ((uint32_t)(c & 0x07) << 18) |
             ((uint32_t)(p[0] & 0x3F) << 12) |
             ((uint32_t)(p[1] & 0x3F) << 6) |
             (uint32_t)(p[2] & 0x3F);
        p += 3;
        if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
    } else {
        cp = 0xFFFD;
    }

    *pp = (const char *)p;
    return cp;
}

static int hui__ttf_zero_width_cp(uint32_t cp) {
    return cp == 0x200D ||
           (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF);
}

static const hui_ttf_font *hui__ttf_font_for_cp(const hui_ttf_font *f,
                                                uint32_t cp) {
    if (hui__ttf_zero_width_cp(cp)) return f;
    for (const hui_ttf_font *cur = f; cur; cur = cur->fallback) {
        if (cp >= HUI_TTF_FIRST && cp <= HUI_TTF_LAST)
            return cur;
        if (stbtt_FindGlyphIndex(&cur->info, (int)cp) > 0)
            return cur;
    }
    return NULL;
}

static int hui__ttf_needs_pango(const char *str) {
    if (!str) return 0;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if (*p >= 0x80)
            return 1;
    }
    return 0;
}

#if defined(HUI_BACKEND_HEADLESS) || defined(HUI_BACKEND_X11) || defined(HUI_BACKEND_CANVAS)

typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;
typedef struct _PangoLayout PangoLayout;
typedef struct _PangoFontDescription PangoFontDescription;

enum {
    HUI__CAIRO_FORMAT_ARGB32 = 0,
    HUI__PANGO_SCALE = 1024
};

typedef struct hui__ttf_pango_api {
    void *cairo_lib;
    void *pango_lib;
    void *pangocairo_lib;
    void *gobject_lib;

    cairo_surface_t *(*cairo_image_surface_create_for_data)(unsigned char *, int, int, int, int);
    cairo_t *(*cairo_create)(cairo_surface_t *);
    void (*cairo_destroy)(cairo_t *);
    void (*cairo_surface_destroy)(cairo_surface_t *);
    void (*cairo_surface_flush)(cairo_surface_t *);
    void (*cairo_set_source_rgba)(cairo_t *, double, double, double, double);
    void (*cairo_move_to)(cairo_t *, double, double);

    PangoLayout *(*pango_cairo_create_layout)(cairo_t *);
    void (*pango_cairo_show_layout)(cairo_t *, PangoLayout *);

    PangoFontDescription *(*pango_font_description_from_string)(const char *);
    void (*pango_font_description_set_absolute_size)(PangoFontDescription *, double);
    void (*pango_font_description_free)(PangoFontDescription *);
    void (*pango_layout_set_text)(PangoLayout *, const char *, int);
    void (*pango_layout_set_font_description)(PangoLayout *, const PangoFontDescription *);
    void (*pango_layout_set_single_paragraph_mode)(PangoLayout *, int);
    void (*pango_layout_set_width)(PangoLayout *, int);
    void (*pango_layout_get_pixel_size)(PangoLayout *, int *, int *);

    void (*g_object_unref)(void *);
} hui__ttf_pango_api;

static hui__ttf_pango_api hui__ttf_pango;
static int hui__ttf_pango_state = 0; /* 0 unknown, 1 ready, -1 unavailable */

static int hui__ttf_load_symbol(void *lib, void **dst, const char *name) {
    *dst = dlsym(lib, name);
    return *dst != NULL;
}

static int hui__ttf_pango_init(void) {
    if (hui__ttf_pango_state != 0)
        return hui__ttf_pango_state > 0;

    memset(&hui__ttf_pango, 0, sizeof(hui__ttf_pango));
    int flags = RTLD_LAZY | RTLD_LOCAL;
    hui__ttf_pango.cairo_lib      = dlopen("libcairo.so.2", flags);
    hui__ttf_pango.pango_lib      = dlopen("libpango-1.0.so.0", flags);
    hui__ttf_pango.pangocairo_lib = dlopen("libpangocairo-1.0.so.0", flags);
    hui__ttf_pango.gobject_lib    = dlopen("libgobject-2.0.so.0", flags);
    if (!hui__ttf_pango.cairo_lib || !hui__ttf_pango.pango_lib ||
        !hui__ttf_pango.pangocairo_lib || !hui__ttf_pango.gobject_lib) {
        hui__ttf_pango_state = -1;
        return 0;
    }

#define HUI__TTF_LOAD(lib, field, name) \
    do { \
        if (!hui__ttf_load_symbol((lib), (void **)&hui__ttf_pango.field, (name))) { \
            hui__ttf_pango_state = -1; \
            return 0; \
        } \
    } while (0)

    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_image_surface_create_for_data, "cairo_image_surface_create_for_data");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_create, "cairo_create");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_destroy, "cairo_destroy");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_surface_destroy, "cairo_surface_destroy");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_surface_flush, "cairo_surface_flush");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_set_source_rgba, "cairo_set_source_rgba");
    HUI__TTF_LOAD(hui__ttf_pango.cairo_lib, cairo_move_to, "cairo_move_to");

    HUI__TTF_LOAD(hui__ttf_pango.pangocairo_lib, pango_cairo_create_layout, "pango_cairo_create_layout");
    HUI__TTF_LOAD(hui__ttf_pango.pangocairo_lib, pango_cairo_show_layout, "pango_cairo_show_layout");

    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_font_description_from_string, "pango_font_description_from_string");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_font_description_set_absolute_size, "pango_font_description_set_absolute_size");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_font_description_free, "pango_font_description_free");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_layout_set_text, "pango_layout_set_text");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_layout_set_font_description, "pango_layout_set_font_description");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_layout_set_single_paragraph_mode, "pango_layout_set_single_paragraph_mode");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_layout_set_width, "pango_layout_set_width");
    HUI__TTF_LOAD(hui__ttf_pango.pango_lib, pango_layout_get_pixel_size, "pango_layout_get_pixel_size");

    HUI__TTF_LOAD(hui__ttf_pango.gobject_lib, g_object_unref, "g_object_unref");

#undef HUI__TTF_LOAD

    hui__ttf_pango_state = 1;
    return 1;
}

static void hui__ttf_set_pango_family(hui_ttf_font *f, const char *path) {
    if (!f) return;
    const char *name = path ? strrchr(path, '/') : NULL;
    name = name ? name + 1 : (path ? path : "");

    const char *family = "monospace";
    if (strstr(name, "FiraMono") || strstr(name, "Fira-Mono"))
        family = "Fira Mono";
    else if (strstr(name, "Cascadia"))
        family = "Cascadia Mono";
    else if (strstr(name, "DejaVuSansMono"))
        family = "DejaVu Sans Mono";
    else if (strstr(name, "NotoSansMono"))
        family = "Noto Sans Mono";
    else if (strstr(name, "NotoSansCJK"))
        family = "Noto Sans CJK";
    else if (strstr(name, "NotoSansArabic"))
        family = "Noto Sans Arabic";
    else if (strstr(name, "NotoSansHebrew"))
        family = "Noto Sans Hebrew";
    else if (strstr(name, "NotoSansSymbols2"))
        family = "Noto Sans Symbols2";
    else if (strstr(name, "NotoSansSymbols"))
        family = "Noto Sans Symbols";

    snprintf(f->pango_family, sizeof(f->pango_family), "%s", family);
}

static void hui__ttf_pango_apply_layout(PangoLayout *layout,
                                        const hui_ttf_font *f,
                                        const char *str) {
    PangoFontDescription *desc =
        hui__ttf_pango.pango_font_description_from_string(
            f->pango_family[0] ? f->pango_family : "monospace");
    if (desc) {
        hui__ttf_pango.pango_font_description_set_absolute_size(
            desc, (double)f->size_px * (double)HUI__PANGO_SCALE);
        hui__ttf_pango.pango_layout_set_font_description(layout, desc);
        hui__ttf_pango.pango_font_description_free(desc);
    }
    hui__ttf_pango.pango_layout_set_width(layout, -1);
    hui__ttf_pango.pango_layout_set_single_paragraph_mode(layout, 1);
    hui__ttf_pango.pango_layout_set_text(layout, str, -1);
}

static int hui__ttf_pango_measure_text(const hui_ttf_font *f,
                                       const char *str,
                                       int *out_w,
                                       int *out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!f || !str || !hui__ttf_pango_init()) return 0;

    unsigned char pixel[4] = {0, 0, 0, 0};
    cairo_surface_t *surface =
        hui__ttf_pango.cairo_image_surface_create_for_data(
            pixel, HUI__CAIRO_FORMAT_ARGB32, 1, 1, 4);
    if (!surface) return 0;

    cairo_t *cr = hui__ttf_pango.cairo_create(surface);
    if (!cr) {
        hui__ttf_pango.cairo_surface_destroy(surface);
        return 0;
    }

    PangoLayout *layout = hui__ttf_pango.pango_cairo_create_layout(cr);
    if (!layout) {
        hui__ttf_pango.cairo_destroy(cr);
        hui__ttf_pango.cairo_surface_destroy(surface);
        return 0;
    }

    hui__ttf_pango_apply_layout(layout, f, str);
    int w = 0, h = 0;
    hui__ttf_pango.pango_layout_get_pixel_size(layout, &w, &h);
    if (out_w) *out_w = w > 0 ? w : 0;
    if (out_h) *out_h = h > 0 ? h : 0;

    hui__ttf_pango.g_object_unref(layout);
    hui__ttf_pango.cairo_destroy(cr);
    hui__ttf_pango.cairo_surface_destroy(surface);
    return 1;
}

#else

static void hui__ttf_set_pango_family(hui_ttf_font *f, const char *path) {
    (void)f;
    (void)path;
}

static int hui__ttf_pango_measure_text(const hui_ttf_font *f,
                                       const char *str,
                                       int *out_w,
                                       int *out_h) {
    (void)f;
    (void)str;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return 0;
}

#endif

/* ---------- load --------------------------------------------------- */

hui_ttf_font *hui_ttf_load(const char *path, float size_px) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "hui_ttf: cannot open '%s'\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);  long flen = ftell(fp);  rewind(fp);
    unsigned char *buf = (unsigned char *)malloc((size_t)flen);
    if (!buf) { fclose(fp); return NULL; }
    if ((long)fread(buf, 1, (size_t)flen, fp) != flen) { free(buf); fclose(fp); return NULL; }
    fclose(fp);

    hui_ttf_font *f = (hui_ttf_font *)calloc(1, sizeof(*f));
    if (!f) { free(buf); return NULL; }
    int font_offset = stbtt_GetFontOffsetForIndex(buf, 0);
    if (font_offset < 0) font_offset = 0;
    if (!stbtt_InitFont(&f->info, buf, font_offset)) {
        free(f);
        free(buf);
        return NULL;
    }

    f->font_data = buf;
    f->size_px = size_px;
    hui__ttf_set_pango_family(f, path);
    f->scale = stbtt_ScaleForPixelHeight(&f->info, size_px);
    f->atlas = (uint8_t *)calloc(1, HUI_TTF_ATLAS_W * HUI_TTF_ATLAS_H);
    if (!f->atlas) { free(f->font_data); free(f); return NULL; }

    int ret = stbtt_BakeFontBitmap(
        buf, font_offset, size_px,
        f->atlas, HUI_TTF_ATLAS_W, HUI_TTF_ATLAS_H,
        HUI_TTF_FIRST, HUI_TTF_GLYPHS, f->glyphs);

    if (ret <= 0) {
        fprintf(stderr, "hui_ttf: ASCII atlas bake failed (ret=%d, size=%.1f) — using dynamic glyphs\n",
                ret, size_px);
        free(f->atlas);
        f->atlas = NULL;
        f->atlas_ok = 0;
    } else {
        f->atlas_ok = 1;
    }

    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (int)((float)asc * f->scale + 0.5f);
    if (f->ascent <= 0) f->ascent = (int)(size_px * 0.85f + 0.5f);

    int pw = 0, ph = 0;
    if (hui__ttf_pango_measure_text(f, "Mg", &pw, &ph) && ph > 0) {
        f->pango_line_h = ph + 2;
        f->pango_ascent = f->ascent;
        if (f->pango_ascent <= 0 || f->pango_ascent >= f->pango_line_h)
            f->pango_ascent = (int)((float)f->pango_line_h * 0.80f + 0.5f);
    }
    return f;
}

void hui_ttf_free(hui_ttf_font *f) {
    if (!f) return;
    hui_ttf_free(f->fallback);
    free(f->font_data);
    free(f->atlas);
    free(f);
}

int hui_ttf_line_height(const hui_ttf_font *f) {
    if (!f) return 0;
    if (f->pango_line_h > 0) return f->pango_line_h;
    return (int)(f->size_px * 1.25f + 0.5f);
}

int hui_ttf_ascent(const hui_ttf_font *f) {
    if (!f) return 0;
    if (f->pango_ascent > 0) return f->pango_ascent;
    return f->ascent;
}

void hui_ttf_set_fallback(hui_ttf_font *f, hui_ttf_font *fallback) {
    if (f) f->fallback = fallback;
}

void hui_ttf_set_hires(hui_ttf_font *f, hui_ttf_font *hires) {
    if (!f) return;
    if (hires && hires != f && hires->size_px > 0.0f && f->size_px > 0.0f) {
        f->hires       = hires;
        f->hires_scale = hires->size_px / f->size_px;
    } else {
        f->hires       = NULL;
        f->hires_scale = 0.0f;
    }
}

int hui_ttf_has_codepoint(const hui_ttf_font *f, uint32_t codepoint) {
    return hui__ttf_font_for_cp(f, codepoint) != NULL;
}

/* ---------- measure ----------------------------------------------- */

int hui_ttf_text_width(const hui_ttf_font *f, const char *str) {
    if (!f || !str) return 0;
    if (hui__ttf_needs_pango(str)) {
        int pango_w = 0, pango_h = 0;
        if (hui__ttf_pango_measure_text(f, str, &pango_w, &pango_h))
            return pango_w;
    }

    float x = 0.0f;
    const char *p = str;
    uint32_t prev = 0;
    const hui_ttf_font *prev_font = NULL;

    while (*p) {
        uint32_t cp = hui__ttf_utf8_next(&p);
        if (cp == 0 || cp == '\n' || cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        if (hui__ttf_zero_width_cp(cp)) continue;

        const hui_ttf_font *font = hui__ttf_font_for_cp(f, cp);
        if (!font) {
            x += f->size_px * 0.65f;
            prev = 0;
            prev_font = NULL;
            continue;
        }

        if (prev && prev_font == font)
            x += (float)stbtt_GetCodepointKernAdvance(&font->info, (int)prev, (int)cp) * font->scale;

        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font->info, (int)cp, &advance, &lsb);
        x += (float)advance * font->scale;
        prev = cp;
        prev_font = font;
    }
    return (int)(x + 0.5f);
}

/* ---------- draw — backend-specific ------------------------------- */

#if defined(HUI_BACKEND_SVG)

/* Forward-declare the SVG append helper defined in hui_svg.h */
static void hui__svg_append(const char *fmt, ...);

void hui_ttf_text(const hui_ttf_font *f, int x, int y,
                  const char *str, hui_color col)
{
    if (!f || !str || !*str) return;
    /* Emit SVG <text> — approximates glyph spacing well enough for charts. */
    float opacity = (float)col.a / 255.0f;
    hui__svg_append(
        "<text x=\"%d\" y=\"%d\" "
        "font-family=\"Cascadia Mono,Consolas,monospace\" "
        "font-size=\"%.1f\" "
        "fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\">",
        x, y, (double)f->size_px,
        (unsigned)col.r, (unsigned)col.g, (unsigned)col.b, (double)opacity);
    /* Escape XML special chars */
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '&':  hui__svg_append("&amp;");  break;
            case '<':  hui__svg_append("&lt;");   break;
            case '>':  hui__svg_append("&gt;");   break;
            default:   hui__svg_append("%c", *p); break;
        }
    }
    hui__svg_append("</text>\n");
}

#elif defined(HUI_BACKEND_HEADLESS) || defined(HUI_BACKEND_X11) || defined(HUI_BACKEND_CANVAS)

/* hui__fb is defined (static) in backends/hui_headless.h, included in same TU.
 * X11 backend includes hui_headless.h (with HUI_HEADLESS_NO_FLUSH) so hui__fb
 * is available there too. */
/* Optional text clip in LOGICAL coords (off by default). Scaled to framebuffer
 * pixels here (× hui__fb_scale) so callers pass logical rects and don't need to
 * know the DPI scale. Keeps smooth-scrolled editor text inside its viewport. */
static int hui__ttf_clip_x0 = -(1<<30), hui__ttf_clip_y0 = -(1<<30);
static int hui__ttf_clip_x1 =  (1<<30), hui__ttf_clip_y1 =  (1<<30);
static int hui__ttf_clip_on = 0;
void hui_ttf_set_clip(int x0, int y0, int x1, int y1) {
    hui__ttf_clip_x0 = x0; hui__ttf_clip_y0 = y0;
    hui__ttf_clip_x1 = x1; hui__ttf_clip_y1 = y1;
    hui__ttf_clip_on = 1;
}
void hui_ttf_clear_clip(void) { hui__ttf_clip_on = 0; }

static inline int hui__ttf_pt_clipped(int fx, int fy) {
    if (!hui__ttf_clip_on) return 0;
    float s = hui__fb_scale;   /* logical clip → physical fb pixels */
    return fx < (int)(hui__ttf_clip_x0 * s) || fx >= (int)(hui__ttf_clip_x1 * s) ||
           fy < (int)(hui__ttf_clip_y0 * s) || fy >= (int)(hui__ttf_clip_y1 * s);
}

static void hui__ttf_blend_pixel(int fx, int fy, hui_color col, uint32_t alpha) {
    if (fx < 0 || fy < 0 || fx >= hui__fb.w || fy >= hui__fb.h) return;
    if (hui__ttf_pt_clipped(fx, fy)) return;
    alpha = alpha * col.a / 255u;
    if (alpha == 0) return;

    uint32_t inv = 255u - alpha;
    uint32_t dst = hui__fb.pixels[fy * hui__fb.w + fx];
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db =  dst        & 0xFF;

    uint32_t or_ = (alpha * col.r + inv * dr) / 255u;
    uint32_t og  = (alpha * col.g + inv * dg) / 255u;
    uint32_t ob  = (alpha * col.b + inv * db) / 255u;

    hui__fb.pixels[fy * hui__fb.w + fx] =
        0xFF000000u | (or_ << 16) | (og << 8) | ob;
}

static void hui__ttf_blend_argb_pixel(int fx, int fy, uint32_t src) {
    if (fx < 0 || fy < 0 || fx >= hui__fb.w || fy >= hui__fb.h) return;
    if (hui__ttf_pt_clipped(fx, fy)) return;
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 0) return;

    /* Cairo ARGB32 pixels are premultiplied in native-endian 0xAARRGGBB. */
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >>  8) & 0xFFu;
    uint32_t sb =  src        & 0xFFu;
    uint32_t inv = 255u - sa;

    uint32_t dst = hui__fb.pixels[fy * hui__fb.w + fx];
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >>  8) & 0xFFu;
    uint32_t db =  dst        & 0xFFu;

    uint32_t or_ = sr + (dr * inv) / 255u;
    uint32_t og  = sg + (dg * inv) / 255u;
    uint32_t ob  = sb + (db * inv) / 255u;
    if (or_ > 255u) or_ = 255u;
    if (og  > 255u) og  = 255u;
    if (ob  > 255u) ob  = 255u;

    hui__fb.pixels[fy * hui__fb.w + fx] =
        0xFF000000u | (or_ << 16) | (og << 8) | ob;
}

static int hui__ttf_pango_draw_text(const hui_ttf_font *f, int x, int y,
                                    const char *str, hui_color col) {
    if (!f || !str || !*str || !hui__fb.pixels || !hui__ttf_pango_init())
        return 0;

    int text_w = 0, text_h = 0;
    if (!hui__ttf_pango_measure_text(f, str, &text_w, &text_h))
        return 0;
    if (text_w <= 0 || text_h <= 0)
        return 1;

    int pad = 4;
    int surf_w = text_w + pad * 2;
    int surf_h = text_h + pad * 2;
    if (surf_w <= 0 || surf_h <= 0)
        return 1;

    int stride = surf_w * 4;
    unsigned char *pixels = (unsigned char *)calloc((size_t)stride, (size_t)surf_h);
    if (!pixels) return 0;

    cairo_surface_t *surface =
        hui__ttf_pango.cairo_image_surface_create_for_data(
            pixels, HUI__CAIRO_FORMAT_ARGB32, surf_w, surf_h, stride);
    if (!surface) {
        free(pixels);
        return 0;
    }

    cairo_t *cr = hui__ttf_pango.cairo_create(surface);
    if (!cr) {
        hui__ttf_pango.cairo_surface_destroy(surface);
        free(pixels);
        return 0;
    }

    PangoLayout *layout = hui__ttf_pango.pango_cairo_create_layout(cr);
    if (!layout) {
        hui__ttf_pango.cairo_destroy(cr);
        hui__ttf_pango.cairo_surface_destroy(surface);
        free(pixels);
        return 0;
    }

    hui__ttf_pango_apply_layout(layout, f, str);
    hui__ttf_pango.cairo_set_source_rgba(
        cr,
        (double)col.r / 255.0,
        (double)col.g / 255.0,
        (double)col.b / 255.0,
        (double)col.a / 255.0);
    hui__ttf_pango.cairo_move_to(cr, (double)pad, (double)pad);
    hui__ttf_pango.pango_cairo_show_layout(cr, layout);
    hui__ttf_pango.cairo_surface_flush(surface);

    int dst_x0 = x - pad;
    int dst_y0 = y - hui_ttf_ascent(f) - pad;
    uint32_t *src = (uint32_t *)pixels;
    for (int py = 0; py < surf_h; py++) {
        int dy = dst_y0 + py;
        if (dy < 0 || dy >= hui__fb.h) continue;
        for (int px = 0; px < surf_w; px++) {
            int dx = dst_x0 + px;
            if (dx < 0 || dx >= hui__fb.w) continue;
            hui__ttf_blend_argb_pixel(dx, dy, src[py * surf_w + px]);
        }
    }

    hui__ttf_pango.g_object_unref(layout);
    hui__ttf_pango.cairo_destroy(cr);
    hui__ttf_pango.cairo_surface_destroy(surface);
    free(pixels);
    return 1;
}

static void hui__ttf_missing_glyph(int x, int y, const hui_ttf_font *f,
                                   hui_color col) {
    int w = f ? (int)(f->size_px * 0.55f + 0.5f) : 8;
    int h = f ? (int)(f->size_px * 0.85f + 0.5f) : 10;
    if (w < 5) w = 5;
    if (h < 7) h = 7;
    int top = y - h;
    for (int xx = 0; xx < w; xx++) {
        hui__ttf_blend_pixel(x + xx, top, col, 160);
        hui__ttf_blend_pixel(x + xx, top + h - 1, col, 160);
    }
    for (int yy = 0; yy < h; yy++) {
        hui__ttf_blend_pixel(x, top + yy, col, 160);
        hui__ttf_blend_pixel(x + w - 1, top + yy, col, 160);
    }
}

static void hui__ttf_draw_baked(const hui_ttf_font *f, uint32_t cp,
                                float *cx, float *cy, hui_color col) {
    stbtt_aligned_quad q;
    stbtt_GetBakedQuad(f->glyphs, HUI_TTF_ATLAS_W, HUI_TTF_ATLAS_H,
                       (int)cp - HUI_TTF_FIRST, cx, cy, &q, 1);

    int px0 = (int)(q.x0);
    int py0 = (int)(q.y0);
    int px1 = (int)(q.x1 + 0.5f);
    int py1 = (int)(q.y1 + 0.5f);

    int ax0 = (int)(q.s0 * HUI_TTF_ATLAS_W);
    int ay0 = (int)(q.t0 * HUI_TTF_ATLAS_H);
    int ax1 = (int)(q.s1 * HUI_TTF_ATLAS_W + 0.5f);
    int ay1 = (int)(q.t1 * HUI_TTF_ATLAS_H + 0.5f);

    int gw = px1 - px0;  if (gw <= 0) return;
    int gh = py1 - py0;  if (gh <= 0) return;
    int agw = ax1 - ax0; if (agw <= 0) agw = 1;
    int agh = ay1 - ay0; if (agh <= 0) agh = 1;

    for (int gy = 0; gy < gh; gy++) {
        int fy = py0 + gy;
        if (fy < 0 || fy >= hui__fb.h) continue;
        int arow = ay0 + gy * agh / gh;
        if (arow >= HUI_TTF_ATLAS_H) arow = HUI_TTF_ATLAS_H - 1;

        for (int gx = 0; gx < gw; gx++) {
            int fx = px0 + gx;
            if (fx < 0 || fx >= hui__fb.w) continue;
            int acol = ax0 + gx * agw / gw;
            if (acol >= HUI_TTF_ATLAS_W) acol = HUI_TTF_ATLAS_W - 1;

            uint32_t alpha = f->atlas[arow * HUI_TTF_ATLAS_W + acol];
            hui__ttf_blend_pixel(fx, fy, col, alpha);
        }
    }
}

static void hui__ttf_draw_dynamic(const hui_ttf_font *font, uint32_t cp,
                                  float *cx, float cy, hui_color col) {
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, (int)cp,
                                font->scale, font->scale,
                                &x0, &y0, &x1, &y1);
    int gw = x1 - x0;
    int gh = y1 - y0;

    if (gw > 0 && gh > 0) {
        unsigned char *bitmap = (unsigned char *)malloc((size_t)gw * (size_t)gh);
        if (bitmap) {
            stbtt_MakeCodepointBitmap(&font->info, bitmap, gw, gh, gw,
                                      font->scale, font->scale, (int)cp);
            int px0 = (int)(*cx + 0.5f) + x0;
            int py0 = (int)(cy + 0.5f) + y0;
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    uint32_t alpha = bitmap[gy * gw + gx];
                    hui__ttf_blend_pixel(px0 + gx, py0 + gy, col, alpha);
                }
            }
            free(bitmap);
        }
    }

    if (!hui__ttf_zero_width_cp(cp)) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&font->info, (int)cp, &advance, &lsb);
        *cx += (float)advance * font->scale;
    }
}

void hui_ttf_text(const hui_ttf_font *f, int x, int y,
                  const char *str, hui_color col)
{
    if (!f || !str || !hui__fb.pixels) return;
    if (f->hires) {
        /* Hi-DPI: rasterize with the companion at the scaled pen position.
         * The companion has hires == NULL, so this recurses exactly once. */
        float s = f->hires_scale > 0.0f ? f->hires_scale : 1.0f;
        hui_ttf_text(f->hires,
                     (int)((float)x * s + (x < 0 ? -0.5f : 0.5f)),
                     (int)((float)y * s + (y < 0 ? -0.5f : 0.5f)),
                     str, col);
        return;
    }
    if (hui__ttf_needs_pango(str) && hui__ttf_pango_draw_text(f, x, y, str, col))
        return;

    float cx = (float)x;
    float cy = (float)y;
    const char *p = str;
    uint32_t prev = 0;
    const hui_ttf_font *prev_font = NULL;

    while (*p) {
        uint32_t cp = hui__ttf_utf8_next(&p);
        if (cp == 0 || cp == '\n' || cp == '\r') continue;
        if (cp == '\t') cp = ' ';
        if (cp >= 0xFE00 && cp <= 0xFE0F) continue;

        const hui_ttf_font *font = hui__ttf_font_for_cp(f, cp);
        if (!font) {
            hui__ttf_missing_glyph((int)(cx + 0.5f), (int)(cy + 0.5f), f, col);
            cx += f->size_px * 0.65f;
            prev = 0;
            prev_font = NULL;
            continue;
        }

        if (prev && prev_font == font)
            cx += (float)stbtt_GetCodepointKernAdvance(&font->info, (int)prev, (int)cp) * font->scale;

        if (cp >= HUI_TTF_FIRST && cp <= HUI_TTF_LAST &&
            font->atlas_ok && font->atlas) {
            float local_y = cy;
            hui__ttf_draw_baked(font, cp, &cx, &local_y, col);
        } else {
            hui__ttf_draw_dynamic(font, cp, &cx, cy, col);
        }

        prev = cp;
        prev_font = font;
    }
}

#else
/* Other backends: no-op (X11, SDL2, etc. don't expose a raw pixel buffer here) */
void hui_ttf_text(const hui_ttf_font *f, int x, int y,
                  const char *str, hui_color col)
{ (void)f; (void)x; (void)y; (void)str; (void)col; }
#endif /* backend */

#endif /* HUI_TTF_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_TTF_H */
