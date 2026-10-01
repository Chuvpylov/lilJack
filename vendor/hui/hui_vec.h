/* hui_vec.h — vector document model + renderer + .barva/SVG I/O.
 * Foundation of the Barva editor (see docs/superpowers/specs/
 * 2026-07-19-barva-vector-editor-design.md).
 *
 * Depends on hui_glyph.h for hui_contour/hui_seg and (impl) the cubic
 * flattener, so HUI_VEC_IMPLEMENTATION must live in the same TU as
 * HUI_GLYPH_IMPLEMENTATION (unity-build pattern, like kvitka.c).
 * Document space is y-down (screen/SVG orientation). */
#ifndef HUI_VEC_H
#define HUI_VEC_H
#include <stdint.h>
#include "hui_math.h"
#include "hui_glyph.h"

/* 2x3 affine: x' = a*x + c*y + e ; y' = b*x + d*y + f (SVG matrix order) */
typedef struct { float a,b,c,d,e,f; } hui_vec_xform;

typedef enum { HUI_VEC_PATH = 0, HUI_VEC_TEXT = 1, HUI_VEC_GROUP = 2, HUI_VEC_IMAGE = 3 } hui_vec_kind;

enum { HUI_STROKE_CENTER=0, HUI_STROKE_INSIDE=1, HUI_STROKE_OUTSIDE=2 };
enum { HUI_JOIN_MITER=0, HUI_JOIN_ROUND=1, HUI_JOIN_BEVEL=2 };
enum { HUI_CAP_BUTT=0, HUI_CAP_ROUND=1, HUI_CAP_SQUARE=2 };
enum { HUI_GRAD_LINEAR=0, HUI_GRAD_RADIAL=1 };
enum { HUI_MARKER_NONE=0, HUI_MARKER_GLYPH=1 };

typedef struct hui_vec_obj hui_vec_obj;
struct hui_vec_obj {
    hui_vec_kind kind;
    uint32_t id;              /* unique within the doc; 0 = unassigned */
    char name[48];
    int  visible, locked;     /* per-object; renderer skips !visible, hit skips locked */
    /* path payload */
    hui_contour *contours; int ncontour, ccap;
    /* group payload (kind==HUI_VEC_GROUP): child objects, bottom-up order */
    hui_vec_obj *objs; int nobj, ocap;
    /* text payload (kind==HUI_VEC_TEXT) */
    char  text[256];
    char  font_family[64];
    float font_size;
    /* image payload (kind==HUI_VEC_IMAGE): pixels live in the document's
     * asset table (shared, content-addressed); the object covers local
     * (0,0)..(img_w,img_h) — one local unit per image pixel — and xf
     * places/scales/rotates it like any other object */
    uint32_t img_asset;
    int      img_w, img_h;
    /* style */
    int       has_fill, has_stroke;
    hui_color fill, stroke;
    float     stroke_w;
    int       stroke_align, stroke_join, stroke_cap;   /* HUI_STROKE_ / HUI_JOIN_ / HUI_CAP_ */
    float     miter_limit;
    float     dash[8]; int ndash; float dash_offset;   /* doc units; ndash 0 = solid */
    float     opacity;        /* 0..1, multiplies both fill and stroke alpha */
    /* linear gradient fill (used when has_grad && has_fill): color runs from
     * grad_a at grad_p0 to grad_b at grad_p1, both points in local coords */
    int       has_grad;
    hui_color grad_a, grad_b;
    hui_v2    grad_p0, grad_p1;
    int       grad_kind;      /* HUI_GRAD_LINEAR / HUI_GRAD_RADIAL */
    int       grad_stroke;    /* paint the stroke with the object's gradient */
    /* glyph markers repeated along the path (HUI_MARKER_*) */
    int       marker_mode;
    char      marker_text[64];
    char      marker_font[64];
    float     marker_size, marker_spacing, marker_offset;
    /* outer glow (soft halo behind the object) */
    int       has_glow;
    hui_color glow_color;
    float     glow_radius, glow_intensity;
    int       blend_mode;     /* HUI_BLEND_* — compositing against the canvas */
    hui_vec_xform xf;
};

typedef struct {
    char name[48];
    int  visible, locked;
    hui_vec_obj *objs; int nobj, ocap;
} hui_vec_layer;

/* raster asset (RGBA8, row-major, w*h*4 bytes, doc-owned). id is a content
 * hash so the same pixels pasted twice share one entry. */
typedef struct { uint32_t id; uint8_t *rgba; int w, h; } hui_vec_asset;

typedef struct {
    int w, h;                 /* document size, px units */
    hui_color bg;
    hui_vec_layer *layers; int nlayer, lcap;
    uint32_t next_id;         /* monotonic id source */
    hui_vec_asset *assets; int nasset, acap;
} hui_vec_doc;

hui_vec_doc   *hui_vec_doc_new(int w, int h);
void           hui_vec_doc_free(hui_vec_doc *d);
hui_vec_layer *hui_vec_layer_add(hui_vec_doc *d, const char *name);
hui_vec_obj   *hui_vec_obj_add(hui_vec_layer *l, hui_vec_kind kind);
hui_contour   *hui_vec_obj_add_contour(hui_vec_obj *o);
hui_vec_obj *hui_vec_group_add(hui_vec_layer *l);
hui_vec_obj *hui_vec_group_child_add(hui_vec_obj *g, hui_vec_kind k);
hui_vec_obj *hui_vec_obj_add_id(hui_vec_doc *d, hui_vec_layer *l, hui_vec_kind k);
void         hui_vec_doc_assign_ids(hui_vec_doc *d);
hui_vec_obj *hui_vec_find_id(hui_vec_doc *d, uint32_t id, hui_vec_layer **out_layer);
/* low-level: append a zero-initialized obj to an obj array (layer/group/scratch);
 * grows via capacity-doubling. Returns NULL on OOM. */
hui_vec_obj *hui_vec_obj_push_into(hui_vec_obj **arr, int *n, int *cap, hui_vec_kind k);

hui_vec_xform hui_vec_xform_identity(void);
hui_v2        hui_vec_xform_apply(hui_vec_xform t, hui_v2 p);
hui_vec_xform hui_vec_xform_mul(hui_vec_xform A, hui_vec_xform B); /* A∘B */

/* ---- raster assets + image objects ----
 * hui_vec_asset_add copies the pixels (adopt takes ownership); both return
 * the asset id — identical pixels yield the same id, so nothing is stored
 * twice. hui_vec_image_add creates an image object sized to the asset at
 * (x,y) in the layer, with a fresh id. steal_assets moves every asset src
 * owns that dst lacks into dst (undo reloads use it: the text form of a
 * document does not carry pixels, the live doc does). gc drops assets no
 * object references. */
uint32_t hui_vec_asset_add  (hui_vec_doc *d, const uint8_t *rgba, int w, int h);
uint32_t hui_vec_asset_adopt(hui_vec_doc *d, uint8_t *rgba, int w, int h);
const hui_vec_asset *hui_vec_asset_find(const hui_vec_doc *d, uint32_t id);
hui_vec_obj *hui_vec_image_add(hui_vec_doc *d, hui_vec_layer *l, uint32_t asset, float x, float y);
void     hui_vec_doc_steal_assets(hui_vec_doc *dst, hui_vec_doc *src);
int      hui_vec_doc_gc_assets(hui_vec_doc *d);
/* App hooks for the on-disk form of assets. With both set, .barva files and
 * SVG exports carry PNG (base64); without them, .barva falls back to raw
 * base64 RGBA (bigger, but lossless and dependency-free) and SVG skips the
 * image with a comment. Both return malloc'd buffers. */
extern unsigned char *(*hui_vec_png_encode)(const uint8_t *rgba, int w, int h, int *len_out);
extern uint8_t       *(*hui_vec_png_decode)(const unsigned char *png, int len, int *w, int *h);

/* .barva native text format (versioned "barva 1"; unknown keywords skipped) */
int          hui_vec_save(const hui_vec_doc *d, const char *path); /* 0 = ok */
enum { HUI_VEC_SAVE_SKIP_ASSETS = 1 };   /* text only — no asset payloads (undo ring) */
int          hui_vec_save_ex(const hui_vec_doc *d, const char *path, int flags);
hui_vec_doc *hui_vec_load(const char *path);                       /* NULL = fail */

/* ---- clipboard text form of a set of objects ----
 * serialize writes the objects whose ids are listed (top-level entries or
 * nested ones — nested objects get their ancestors' transforms baked into
 * xf so they land where they were seen) in document order, preceded by any
 * image assets they reference:
 *     barva-clip 1
 *     asset ...            (optional)
 *     obj/group ... endobj
 * parse appends everything it finds to layer l of d (assets imported, all
 * ids re-stamped fresh so pasting into the source document is safe) and
 * returns the number of top-level objects added; their ids go to out_ids
 * (up to cap). Accepts whole .barva files too (layer lines are ignored). */
char *hui_vec_clip_serialize(const hui_vec_doc *d, const uint32_t *ids, int n);
int   hui_vec_clip_parse(hui_vec_doc *d, hui_vec_layer *l, const char *text,
                         uint32_t *out_ids, int cap);

/* Render doc into viewport: doc point p maps to screen
 * (vp.x + pan_x + p.x*zoom, vp.y + pan_y + p.y*zoom); clipped to vp;
 * hidden layers skipped; even-odd fill; opacity multiplies alpha. */
void hui_vec_render(const hui_vec_doc *d, hui_rect viewport,
                    float zoom, float pan_x, float pan_y);
/* Render a single layer's objects (no document background, visibility flag
 * ignored) — used for layer preview thumbnails. */
void hui_vec_render_layer(const hui_vec_doc *d, int layer, hui_rect viewport,
                          float zoom, float pan_x, float pan_y);
/* Render one object only — used for per-object preview cards. */
void hui_vec_render_object(const hui_vec_doc *d, int layer, int obj,
                           hui_rect viewport, float zoom, float pan_x, float pan_y);
/* Render a single object node (recursing into group children); the node's own
 * visibility flag is ignored so hidden objects still show in previews. */
void hui_vec_render_single(const hui_vec_doc *d, const hui_vec_obj *o,
                           hui_rect viewport, float zoom, float pan_x, float pan_y);
/* App hook resolving a text object's font family to a loaded glyph font.
 * NULL (default) or returning NULL: text objects are skipped. */
extern hui_glyph_font *(*hui_vec_font_lookup)(const char *family);

/* SVG export: <g> per visible layer, <path> per object; text objects baked
 * to outlines via hui_vec_font_lookup (skipped with a comment otherwise). */
int hui_vec_export_svg(const hui_vec_doc *d, const char *path); /* 0 = ok */

/* SVG import (subset — see the Barva spec): svg/g/path/rect/circle/ellipse/
 * line/polyline/polygon; fill/stroke/stroke-width/opacity/transform (attrs +
 * minimal style="");  top-level <g> -> layer, nested <g> -> group id.
 * Unsupported elements are counted into *skipped (if non-NULL), never fatal.
 * NULL on open failure or no <svg> root. */
hui_vec_doc *hui_vec_import_svg(const char *path, int *skipped);

#ifdef HUI_VEC_IMPLEMENTATION
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

hui_vec_doc *hui_vec_doc_new(int w, int h) {
    hui_vec_doc *d = (hui_vec_doc*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->w = w > 0 ? w : 800; d->h = h > 0 ? h : 600;
    d->bg = (hui_color){16,16,20,255};
    d->next_id = 1;
    return d;
}

static void hui__vec_obj_free_payload(hui_vec_obj *o) {
    for (int i = 0; i < o->ncontour; i++) free(o->contours[i].segs);
    free(o->contours);
    o->contours = NULL; o->ncontour = o->ccap = 0;
    for (int i = 0; i < o->nobj; i++) hui__vec_obj_free_payload(&o->objs[i]);
    free(o->objs);
    o->objs = NULL; o->nobj = o->ocap = 0;
}

void hui_vec_doc_free(hui_vec_doc *d) {
    if (!d) return;
    for (int li = 0; li < d->nlayer; li++) {
        hui_vec_layer *l = &d->layers[li];
        for (int oi = 0; oi < l->nobj; oi++) hui__vec_obj_free_payload(&l->objs[oi]);
        free(l->objs);
    }
    for (int i = 0; i < d->nasset; i++) free(d->assets[i].rgba);
    free(d->assets);
    free(d->layers); free(d);
}

/* ---- raster assets ---- */
unsigned char *(*hui_vec_png_encode)(const uint8_t *rgba, int w, int h, int *len_out) = NULL;
uint8_t       *(*hui_vec_png_decode)(const unsigned char *png, int len, int *w, int *h) = NULL;

static uint32_t hui__vec_asset_hash(const uint8_t *rgba, int w, int h) {
    uint32_t hsh = 2166136261u;
    #define HUI__FNV(b) (hsh = (hsh ^ (uint32_t)(b)) * 16777619u)
    HUI__FNV(w & 0xff); HUI__FNV((w >> 8) & 0xff); HUI__FNV(h & 0xff); HUI__FNV((h >> 8) & 0xff);
    size_t n = (size_t)w * (size_t)h * 4u;
    for (size_t i = 0; i < n; i++) HUI__FNV(rgba[i]);
    #undef HUI__FNV
    return hsh | 1u;   /* never 0 */
}

const hui_vec_asset *hui_vec_asset_find(const hui_vec_doc *d, uint32_t id) {
    if (!d || !id) return NULL;
    for (int i = 0; i < d->nasset; i++) if (d->assets[i].id == id) return &d->assets[i];
    return NULL;
}

static hui_vec_asset *hui__vec_asset_slot(hui_vec_doc *d) {
    if (d->nasset == d->acap) {
        int nc = d->acap ? d->acap * 2 : 4;
        hui_vec_asset *na = (hui_vec_asset*)realloc(d->assets, (size_t)nc * sizeof *na);
        if (!na) return NULL;
        d->assets = na; d->acap = nc;
    }
    hui_vec_asset *a = &d->assets[d->nasset++];
    memset(a, 0, sizeof *a);
    return a;
}

uint32_t hui_vec_asset_adopt(hui_vec_doc *d, uint8_t *rgba, int w, int h) {
    if (!d || !rgba || w <= 0 || h <= 0) { free(rgba); return 0; }
    uint32_t id = hui__vec_asset_hash(rgba, w, h);
    size_t n = (size_t)w * (size_t)h * 4u;
    for (;;) {
        const hui_vec_asset *e = hui_vec_asset_find(d, id);
        if (!e) break;
        if (e->w == w && e->h == h && memcmp(e->rgba, rgba, n) == 0) { free(rgba); return id; }
        id += 2;   /* hash collision with different pixels: probe */
    }
    hui_vec_asset *a = hui__vec_asset_slot(d);
    if (!a) { free(rgba); return 0; }
    a->id = id; a->rgba = rgba; a->w = w; a->h = h;
    return id;
}

uint32_t hui_vec_asset_add(hui_vec_doc *d, const uint8_t *rgba, int w, int h) {
    if (!d || !rgba || w <= 0 || h <= 0) return 0;
    size_t n = (size_t)w * (size_t)h * 4u;
    uint8_t *copy = (uint8_t*)malloc(n);
    if (!copy) return 0;
    memcpy(copy, rgba, n);
    return hui_vec_asset_adopt(d, copy, w, h);
}

hui_vec_obj *hui_vec_image_add(hui_vec_doc *d, hui_vec_layer *l, uint32_t asset, float x, float y) {
    const hui_vec_asset *a = hui_vec_asset_find(d, asset);
    if (!a || !l) return NULL;
    hui_vec_obj *o = hui_vec_obj_add(l, HUI_VEC_IMAGE);
    if (!o) return NULL;
    o->id = d->next_id++;
    o->has_fill = 0; o->has_stroke = 0;
    snprintf(o->name, sizeof o->name, "image");
    o->img_asset = asset; o->img_w = a->w; o->img_h = a->h;
    o->xf = (hui_vec_xform){1,0,0,1, x, y};
    return o;
}

void hui_vec_doc_steal_assets(hui_vec_doc *dst, hui_vec_doc *src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->nasset; i++) {
        hui_vec_asset *a = &src->assets[i];
        if (!a->rgba) continue;
        if (hui_vec_asset_find(dst, a->id)) continue;   /* dst already carries it */
        hui_vec_asset *slot = hui__vec_asset_slot(dst);
        if (!slot) break;
        *slot = *a;
        a->rgba = NULL;   /* ownership moved */
    }
}

static void hui__vec_mark_assets_rec(const hui_vec_obj *o, uint32_t *ids, int *n, int cap) {
    if (o->kind == HUI_VEC_IMAGE && o->img_asset) {
        int seen = 0;
        for (int i = 0; i < *n; i++) if (ids[i] == o->img_asset) { seen = 1; break; }
        if (!seen && *n < cap) ids[(*n)++] = o->img_asset;
    }
    for (int i = 0; i < o->nobj; i++) hui__vec_mark_assets_rec(&o->objs[i], ids, n, cap);
}

int hui_vec_doc_gc_assets(hui_vec_doc *d) {
    if (!d || d->nasset == 0) return 0;
    uint32_t *used = (uint32_t*)malloc((size_t)d->nasset * sizeof *used);
    if (!used) return 0;
    int nused = 0;
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++)
            hui__vec_mark_assets_rec(&d->layers[li].objs[oi], used, &nused, d->nasset);
    int w = 0, removed = 0;
    for (int i = 0; i < d->nasset; i++) {
        int keep = 0;
        for (int k = 0; k < nused; k++) if (used[k] == d->assets[i].id) { keep = 1; break; }
        if (keep) d->assets[w++] = d->assets[i];
        else { free(d->assets[i].rgba); removed++; }
    }
    d->nasset = w;
    free(used);
    return removed;
}

/* ---- base64 (asset payloads) ---- */
static const char hui__b64_tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *hui__b64_encode(const unsigned char *data, size_t n) {
    size_t out = ((n + 2) / 3) * 4;
    char *s = (char*)malloc(out + 1);
    if (!s) return NULL;
    size_t i = 0, j = 0;
    while (i + 2 < n) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8) | data[i+2];
        s[j++] = hui__b64_tab[(v >> 18) & 63]; s[j++] = hui__b64_tab[(v >> 12) & 63];
        s[j++] = hui__b64_tab[(v >> 6) & 63];  s[j++] = hui__b64_tab[v & 63];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < n) v |= (uint32_t)data[i+1] << 8;
        s[j++] = hui__b64_tab[(v >> 18) & 63]; s[j++] = hui__b64_tab[(v >> 12) & 63];
        s[j++] = (i + 1 < n) ? hui__b64_tab[(v >> 6) & 63] : '=';
        s[j++] = '=';
    }
    s[j] = 0;
    return s;
}
static unsigned char *hui__b64_decode(const char *s, size_t *len_out) {
    size_t n = strlen(s);
    unsigned char *out = (unsigned char*)malloc(n / 4 * 3 + 3);
    if (!out) return NULL;
    uint32_t v = 0; int bits = 0; size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i]; int d;
        if      (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=') break;
        else continue;   /* whitespace / junk */
        v = (v << 6) | (uint32_t)d; bits += 6;
        if (bits >= 8) { bits -= 8; out[j++] = (unsigned char)((v >> bits) & 0xff); }
    }
    if (len_out) *len_out = j;
    return out;
}

/* asset line: "asset <id> <w> <h> <png|rgba> <base64>" */
static void hui__vec_write_asset(FILE *fp, const hui_vec_asset *a) {
    if (!a->rgba) return;
    char *b64 = NULL;
    const char *fmt = "rgba";
    if (hui_vec_png_encode) {
        int plen = 0;
        unsigned char *png = hui_vec_png_encode(a->rgba, a->w, a->h, &plen);
        if (png && plen > 0) { b64 = hui__b64_encode(png, (size_t)plen); fmt = "png"; }
        free(png);
    }
    if (!b64) b64 = hui__b64_encode(a->rgba, (size_t)a->w * (size_t)a->h * 4u);
    if (!b64) return;
    fprintf(fp, "asset %u %d %d %s %s\n", a->id, a->w, a->h, fmt, b64);
    free(b64);
}

/* parse one asset line into d (skips ids d already holds; unknown/undecodable
 * formats are dropped — the referencing object then draws as a placeholder) */
static void hui__vec_parse_asset(hui_vec_doc *d, const char *line) {
    unsigned id = 0; int w = 0, h = 0; char fmt[16] = ""; int pos = 0;
    if (sscanf(line, "asset %u %d %d %15s %n", &id, &w, &h, fmt, &pos) < 4) return;
    if (w <= 0 || h <= 0 || id == 0) return;
    if (hui_vec_asset_find(d, id)) return;
    size_t raw_len = 0;
    unsigned char *raw = hui__b64_decode(line + pos, &raw_len);
    if (!raw) return;
    uint8_t *rgba = NULL;
    if (strcmp(fmt, "rgba") == 0) {
        if (raw_len >= (size_t)w * (size_t)h * 4u) rgba = raw; else free(raw);
    } else if (strcmp(fmt, "png") == 0) {
        int dw = 0, dh = 0;
        if (hui_vec_png_decode && raw_len < 0x7FFFFFFFu)
            rgba = hui_vec_png_decode(raw, (int)raw_len, &dw, &dh);
        free(raw);
        if (rgba && (dw != w || dh != h)) { free(rgba); rgba = NULL; }
    } else free(raw);
    if (!rgba) return;
    /* keep the file's id (content hash of the same pixels) so object refs hold */
    hui_vec_asset *a = hui__vec_asset_slot(d);
    if (!a) { free(rgba); return; }
    a->id = id; a->rgba = rgba; a->w = w; a->h = h;
}

hui_vec_layer *hui_vec_layer_add(hui_vec_doc *d, const char *name) {
    if (d->nlayer == d->lcap) {
        int nc = d->lcap ? d->lcap * 2 : 4;
        hui_vec_layer *nl = (hui_vec_layer*)realloc(d->layers, (size_t)nc * sizeof *nl);
        if (!nl) return NULL;
        d->layers = nl; d->lcap = nc;
    }
    hui_vec_layer *l = &d->layers[d->nlayer++];
    memset(l, 0, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", name ? name : "layer");
    l->visible = 1;
    return l;
}

hui_vec_obj *hui_vec_obj_add(hui_vec_layer *l, hui_vec_kind kind) {
    if (l->nobj == l->ocap) {
        int nc = l->ocap ? l->ocap * 2 : 8;
        hui_vec_obj *no = (hui_vec_obj*)realloc(l->objs, (size_t)nc * sizeof *no);
        if (!no) return NULL;
        l->objs = no; l->ocap = nc;
    }
    hui_vec_obj *o = &l->objs[l->nobj++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->id = 0;               /* stamped by hui_vec_doc_assign_ids */
    o->visible = 1;
    o->has_fill = 1;
    o->fill = (hui_color){200,200,210,255};
    o->stroke = (hui_color){20,20,20,255};
    o->stroke_w = 2.0f;
    o->miter_limit = 4.0f;
    o->marker_size = 16.0f;
    o->glow_color = (hui_color){120,180,255,255}; o->glow_radius = 8.0f; o->glow_intensity = 0.8f;
    o->opacity = 1.0f;
    o->font_size = 24.0f;
    o->xf = hui_vec_xform_identity();
    return o;
}

hui_contour *hui_vec_obj_add_contour(hui_vec_obj *o) {
    if (o->ncontour == o->ccap) {
        int nc = o->ccap ? o->ccap * 2 : 4;
        hui_contour *ncs = (hui_contour*)realloc(o->contours, (size_t)nc * sizeof *ncs);
        if (!ncs) return NULL;
        o->contours = ncs; o->ccap = nc;
    }
    hui_contour *c = &o->contours[o->ncontour++];
    memset(c, 0, sizeof *c);
    return c;
}

/* shared: append a zero-initialized obj into an obj array (layer or group) */
static hui_vec_obj *hui__vec_obj_push(hui_vec_obj **arr, int *n, int *cap, hui_vec_kind k) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        hui_vec_obj *no = (hui_vec_obj*)realloc(*arr, (size_t)nc * sizeof *no);
        if (!no) return NULL;
        *arr = no; *cap = nc;
    }
    hui_vec_obj *o = &(*arr)[(*n)++];
    memset(o, 0, sizeof *o);
    o->kind = k; o->visible = 1; o->opacity = 1.0f;
    o->has_fill = (k != HUI_VEC_GROUP);
    o->fill = (hui_color){200,200,210,255};
    o->stroke = (hui_color){20,20,20,255};
    o->stroke_w = 2.0f; o->miter_limit = 4.0f; o->marker_size = 16.0f; o->glow_color=(hui_color){120,180,255,255}; o->glow_radius=8.0f; o->glow_intensity=0.8f; o->font_size = 24.0f;
    o->xf = hui_vec_xform_identity();
    return o;
}

hui_vec_obj *hui_vec_obj_push_into(hui_vec_obj **arr, int *n, int *cap, hui_vec_kind k) {
    return hui__vec_obj_push(arr, n, cap, k);
}

hui_vec_obj *hui_vec_group_add(hui_vec_layer *l) {
    hui_vec_obj *g = hui__vec_obj_push(&l->objs, &l->nobj, &l->ocap, HUI_VEC_GROUP);
    if (g) snprintf(g->name, sizeof g->name, "group");
    return g;
}

hui_vec_obj *hui_vec_group_child_add(hui_vec_obj *g, hui_vec_kind k) {
    if (!g || g->kind != HUI_VEC_GROUP) return NULL;
    return hui__vec_obj_push(&g->objs, &g->nobj, &g->ocap, k);
}

hui_vec_obj *hui_vec_obj_add_id(hui_vec_doc *d, hui_vec_layer *l, hui_vec_kind k) {
    hui_vec_obj *o = (k == HUI_VEC_GROUP) ? hui_vec_group_add(l)
                                          : hui_vec_obj_add(l, k);
    if (o) o->id = d->next_id++;
    return o;
}

static void hui__vec_assign_ids_rec(hui_vec_doc *d, hui_vec_obj *o) {
    if (o->id == 0) o->id = d->next_id++;
    else if (o->id >= d->next_id) d->next_id = o->id + 1;
    for (int i = 0; i < o->nobj; i++) hui__vec_assign_ids_rec(d, &o->objs[i]);
}

void hui_vec_doc_assign_ids(hui_vec_doc *d) {
    if (!d) return;
    if (d->next_id == 0) d->next_id = 1;
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++) {
            hui_vec_obj *o = &d->layers[li].objs[oi];
            if (o->id != 0 && o->id >= d->next_id) d->next_id = o->id + 1;
        }
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++)
            hui__vec_assign_ids_rec(d, &d->layers[li].objs[oi]);
}

static hui_vec_obj *hui__vec_find_id_rec(hui_vec_obj *o, uint32_t id) {
    if (o->id == id) return o;
    for (int i = 0; i < o->nobj; i++) {
        hui_vec_obj *r = hui__vec_find_id_rec(&o->objs[i], id);
        if (r) return r;
    }
    return NULL;
}

hui_vec_obj *hui_vec_find_id(hui_vec_doc *d, uint32_t id, hui_vec_layer **out_layer) {
    if (out_layer) *out_layer = NULL;
    if (!d || id == 0) return NULL;
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++) {
            hui_vec_obj *r = hui__vec_find_id_rec(&d->layers[li].objs[oi], id);
            if (r) { if (out_layer) *out_layer = &d->layers[li]; return r; }
        }
    return NULL;
}

hui_vec_xform hui_vec_xform_identity(void) { return (hui_vec_xform){1,0,0,1,0,0}; }

hui_v2 hui_vec_xform_apply(hui_vec_xform t, hui_v2 p) {
    return (hui_v2){ t.a*p.x + t.c*p.y + t.e, t.b*p.x + t.d*p.y + t.f };
}

hui_vec_xform hui_vec_xform_mul(hui_vec_xform A, hui_vec_xform B) {
    return (hui_vec_xform){
        A.a*B.a + A.c*B.b,        A.b*B.a + A.d*B.b,
        A.a*B.c + A.c*B.d,        A.b*B.c + A.d*B.d,
        A.a*B.e + A.c*B.f + A.e,  A.b*B.e + A.d*B.f + A.f };
}

static hui_vec_xform hui__vec_xform_inverse(hui_vec_xform t) {
    float det = t.a*t.d - t.b*t.c;
    if (fabsf(det) < 1e-9f) return hui_vec_xform_identity();
    float id = 1.0f/det;
    hui_vec_xform r;
    r.a =  t.d*id; r.b = -t.b*id; r.c = -t.c*id; r.d =  t.a*id;
    r.e = -(t.e*r.a + t.f*r.c);
    r.f = -(t.e*r.b + t.f*r.d);
    return r;
}

/* ---- renderer ---- */

hui_glyph_font *(*hui_vec_font_lookup)(const char *family) = 0;

/* Flatten one contour under (xf then zoom/pan) into screen-space points. */
#define HUI__VEC_FLAT_CAP 1024
static int hui__vec_flatten(const hui_contour *c, hui_vec_xform xf,
                            hui_rect vp, float zoom, float px, float py,
                            float *ox, float *oy) {
    float fx[HUI__VEC_FLAT_CAP], fy[HUI__VEC_FLAT_CAP]; int fn = 0;
    fx[fn] = c->start.x; fy[fn] = c->start.y; fn++;
    hui_v2 prev = c->start;
    for (int j = 0; j < c->nseg; j++) {
        if (fn < HUI__VEC_FLAT_CAP-1)
            hui__cubic_flat(prev, c->segs[j].c0, c->segs[j].c1, c->segs[j].a,
                            fx, fy, &fn, HUI__VEC_FLAT_CAP, 0);
        prev = c->segs[j].a;
    }
    for (int j = 0; j < fn; j++) {
        hui_v2 p = hui_vec_xform_apply(xf, (hui_v2){fx[j], fy[j]});
        ox[j] = (float)vp.x + px + p.x * zoom;
        oy[j] = (float)vp.y + py + p.y * zoom;
    }
    return fn;
}

/* Anti-aliased even-odd scanline fill of an edge list: per pixel row,
 * HUI_VEC_AA_SUB sub-scanlines accumulate exact horizontal span coverage,
 * emitted as run-length alpha-modulated 1px fills (same technique as
 * hui_glyph_draw). Clipped to vp. */
typedef struct { float x0,y0,x1,y1; } hui__vec_edge;
#ifndef HUI_VEC_AA_SUB
#  define HUI_VEC_AA_SUB 3
#endif
static void hui__vec_fill_edges(const hui__vec_edge *e, int ne, hui_rect vp, hui_color col) {
    if (ne == 0) return;
    float minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < ne; i++) {
        if (e[i].y0 < minY) minY = e[i].y0;  if (e[i].y1 < minY) minY = e[i].y1;
        if (e[i].y0 > maxY) maxY = e[i].y0;  if (e[i].y1 > maxY) maxY = e[i].y1;
    }
    int y0 = (int)floorf(minY), y1 = (int)ceilf(maxY) + 1;
    if (y0 < vp.y) y0 = vp.y;
    if (y1 > vp.y + vp.h) y1 = vp.y + vp.h;
    enum { COVW = 8192 };
    static float cov[COVW];
    int cx0 = vp.x < 0 ? 0 : vp.x;
    int cx1 = vp.x + vp.w; if (cx1 > COVW) cx1 = COVW;
    for (int y = y0; y < y1; y++) {
        int rxa = cx1, rxb = -1;
        for (int sub = 0; sub < HUI_VEC_AA_SUB; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / (float)HUI_VEC_AA_SUB;
            float xs[256]; int nx = 0;
            for (int i = 0; i < ne; i++) {
                float ey0 = e[i].y0, ey1 = e[i].y1, ex0 = e[i].x0, ex1 = e[i].x1;
                if ((sy >= ey0 && sy < ey1) || (sy >= ey1 && sy < ey0)) {
                    float t = (sy - ey0) / (ey1 - ey0);
                    if (nx < 256) xs[nx++] = ex0 + t * (ex1 - ex0);
                }
            }
            for (int i = 1; i < nx; i++) {          /* insertion sort */
                float v = xs[i]; int j = i - 1;
                while (j >= 0 && xs[j] > v) { xs[j+1] = xs[j]; j--; }
                xs[j+1] = v;
            }
            float w = 1.0f / (float)HUI_VEC_AA_SUB;
            for (int i = 0; i + 1 < nx; i += 2) {
                float fa = xs[i], fb = xs[i+1];
                if (fa < (float)cx0) fa = (float)cx0;
                if (fb > (float)cx1) fb = (float)cx1;
                if (fb <= fa) continue;
                int ia = (int)fa, ib = (int)fb;
                if (ia >= cx1) continue;
                if (ia == ib) {
                    cov[ia] += (fb - fa) * w;
                } else {
                    cov[ia] += ((float)(ia+1) - fa) * w;
                    for (int px = ia+1; px < ib && px < cx1; px++) cov[px] += w;
                    if (ib < cx1) cov[ib] += (fb - (float)ib) * w;
                }
                if (ia < rxa) rxa = ia;
                if (ib > rxb) rxb = ib < cx1 ? ib : cx1-1;
            }
        }
        int px = rxa;
        while (px <= rxb) {
            int a8 = (int)(cov[px] * 255.0f + 0.5f);
            if (a8 > 255) a8 = 255;
            a8 = (a8 + 8) & ~15;
            if (a8 > 255) a8 = 255;
            int run = px;
            while (run <= rxb) {
                int b8 = (int)(cov[run] * 255.0f + 0.5f);
                if (b8 > 255) b8 = 255;
                b8 = (b8 + 8) & ~15;
                if (b8 > 255) b8 = 255;
                if (b8 != a8) break;
                cov[run] = 0.0f;
                run++;
            }
            if (a8 > 0) {
                hui_color c2 = col;
                c2.a = (uint8_t)((int)col.a * a8 / 255);
                hui_rect_fill(hui_rect_make(px, y, run - px, 1), c2, 0);
            }
            px = run;
        }
    }
}

/* Nonzero-winding AA fill (for strokes): same coverage accumulator as
 * hui__vec_fill_edges but spans open where the signed winding number != 0, so
 * overlapping / self-intersecting loops fill solid with one blend per pixel. */
static void hui__vec_fill_edges_nz(const hui__vec_edge *e, int ne, hui_rect vp, hui_color col) {
    if (ne == 0) return;
    float minY=1e9f, maxY=-1e9f;
    for (int i=0;i<ne;i++){ if(e[i].y0<minY)minY=e[i].y0; if(e[i].y1<minY)minY=e[i].y1;
                            if(e[i].y0>maxY)maxY=e[i].y0; if(e[i].y1>maxY)maxY=e[i].y1; }
    int y0=(int)floorf(minY), y1=(int)ceilf(maxY)+1;
    if(y0<vp.y)y0=vp.y; if(y1>vp.y+vp.h)y1=vp.y+vp.h;
    enum { COVW = 8192 };
    static float cov[COVW];
    int cx0 = vp.x<0?0:vp.x, cx1 = vp.x+vp.w; if(cx1>COVW)cx1=COVW;
    for (int y=y0;y<y1;y++){
        int rxa=cx1, rxb=-1;
        for (int sub=0; sub<HUI_VEC_AA_SUB; sub++){
            float sy=(float)y + ((float)sub+0.5f)/(float)HUI_VEC_AA_SUB;
            float xs[256]; int dir[256]; int nx=0;
            for (int i=0;i<ne;i++){
                float ey0=e[i].y0,ey1=e[i].y1,ex0=e[i].x0,ex1=e[i].x1;
                if ((sy>=ey0&&sy<ey1)||(sy>=ey1&&sy<ey0)){
                    float t=(sy-ey0)/(ey1-ey0);
                    if(nx<256){ xs[nx]=ex0+t*(ex1-ex0); dir[nx]=(ey1>ey0)?1:-1; nx++; }
                }
            }
            for (int i=1;i<nx;i++){ float v=xs[i]; int dd=dir[i]; int j=i-1;
                while(j>=0&&xs[j]>v){ xs[j+1]=xs[j]; dir[j+1]=dir[j]; j--; } xs[j+1]=v; dir[j+1]=dd; }
            float w=1.0f/(float)HUI_VEC_AA_SUB;
            int wind=0; float spanx=0;
            for (int i=0;i<nx;i++){
                int prev=wind; wind+=dir[i];
                if (prev==0 && wind!=0) spanx=xs[i];
                else if (prev!=0 && wind==0){
                    float fa=spanx, fb=xs[i];
                    if(fa<(float)cx0)fa=(float)cx0; if(fb>(float)cx1)fb=(float)cx1;
                    if(fb>fa){
                        int ia=(int)fa, ib=(int)fb;
                        if(ia<cx1){
                            if(ia==ib) cov[ia]+=(fb-fa)*w;
                            else { cov[ia]+=((float)(ia+1)-fa)*w;
                                   for(int px=ia+1;px<ib&&px<cx1;px++)cov[px]+=w;
                                   if(ib<cx1)cov[ib]+=(fb-(float)ib)*w; }
                            if(ia<rxa)rxa=ia; if(ib>rxb)rxb=ib<cx1?ib:cx1-1;
                        }
                    }
                }
            }
        }
        int px=rxa;
        while(px<=rxb){
            int a8=(int)(cov[px]*255.0f+0.5f); if(a8>255)a8=255; a8=(a8+8)&~15; if(a8>255)a8=255;
            int run=px;
            while(run<=rxb){ int b8=(int)(cov[run]*255.0f+0.5f); if(b8>255)b8=255; b8=(b8+8)&~15; if(b8>255)b8=255;
                if(b8!=a8)break; cov[run]=0.0f; run++; }
            if(a8>0){ hui_color c2=col; c2.a=(uint8_t)((int)col.a*a8/255);
                      hui_rect_fill(hui_rect_make(px,y,run-px,1),c2,0); }
            px=run;
        }
    }
}

/* ---- single-pass stroke outline builder ---- */
static void hui__so_edge(hui__vec_edge *o,int *no,int cap,float x0,float y0,float x1,float y1){
    if(*no<cap) o[(*no)++]=(hui__vec_edge){x0,y0,x1,y1};
}
static void hui__so_arc(hui__vec_edge *o,int *no,int cap,float cx,float cy,
                        float ax,float ay,float bx,float by,float r){
    float a0=atan2f(ay-cy,ax-cx), a1=atan2f(by-cy,bx-cx);
    float da=a1-a0; while(da> 3.14159265f)da-=6.28318531f; while(da<-3.14159265f)da+=6.28318531f;
    int steps=(int)(fabsf(da)*(r<2?2:r)/3.0f); if(steps<1)steps=1; if(steps>48)steps=48;
    float pxr=ax,pyr=ay;
    for(int s=1;s<=steps;s++){ float a=a0+da*((float)s/(float)steps);
        float nx=cx+cosf(a)*r, ny=cy+sinf(a)*r; hui__so_edge(o,no,cap,pxr,pyr,nx,ny); pxr=nx;pyr=ny; }
}
static int hui__vec_stroke_outline(const float *px,const float *py,int n,int closed,
                            float half_l,float half_r,int join,int cap,float miter_limit,
                            hui__vec_edge *out,int out_cap){
    if(n<2) return 0;
    if(miter_limit<1.0f) miter_limit=1.0f;
    float qx[HUI__VEC_FLAT_CAP], qy[HUI__VEC_FLAT_CAP]; int m=0;
    for(int i=0;i<n;i++){ if(m==0 || fabsf(px[i]-qx[m-1])>0.01f || fabsf(py[i]-qy[m-1])>0.01f){
        if(m<HUI__VEC_FLAT_CAP){ qx[m]=px[i]; qy[m]=py[i]; m++; } } }
    if(closed && m>2 && fabsf(qx[0]-qx[m-1])<0.01f && fabsf(qy[0]-qy[m-1])<0.01f) m--;
    if(m<2) return 0;
    int no=0;
    float lx[HUI__VEC_FLAT_CAP*3], ly[HUI__VEC_FLAT_CAP*3]; int nl=0;
    float rx[HUI__VEC_FLAT_CAP*3], ry[HUI__VEC_FLAT_CAP*3]; int nr=0;
    int seg = closed ? m : m-1;
    for(int i=0;i<seg;i++){
        int a=i, b=(i+1)%m;
        float dx=qx[b]-qx[a], dy=qy[b]-qy[a]; float len=sqrtf(dx*dx+dy*dy); if(len<1e-4f)continue;
        float ux=dx/len, uy=dy/len; float nx=-uy, ny=ux;
        if(nl<HUI__VEC_FLAT_CAP*3-2){ lx[nl]=qx[a]+nx*half_l; ly[nl]=qy[a]+ny*half_l; nl++;
                                       lx[nl]=qx[b]+nx*half_l; ly[nl]=qy[b]+ny*half_l; nl++; }
        if(nr<HUI__VEC_FLAT_CAP*3-2){ rx[nr]=qx[a]-nx*half_r; ry[nr]=qy[a]-ny*half_r; nr++;
                                       rx[nr]=qx[b]-nx*half_r; ry[nr]=qy[b]-ny*half_r; nr++; }
    }
    if(nl<2||nr<2) return 0;
    hui__so_edge(out,&no,out_cap, lx[0],ly[0], lx[1],ly[1]);
    for(int i=2;i+1<nl;i+=2){
        float jx=qx[(i/2)%m], jy=qy[(i/2)%m];
        if(join==HUI_JOIN_ROUND){
            hui__so_arc(out,&no,out_cap, jx,jy, lx[i-1],ly[i-1], lx[i],ly[i], half_l);
        } else if(join==HUI_JOIN_MITER){
            float ax=lx[i-1],ay=ly[i-1], bx=lx[i],by=ly[i];
            float mx=(ax+bx)*0.5f,my=(ay+by)*0.5f;
            float d=sqrtf((mx-jx)*(mx-jx)+(my-jy)*(my-jy));
            float ratio = d>1e-4f ? (half_l/d) : 0;
            if(ratio>0 && ratio<=miter_limit){
                float ex=jx+(mx-jx)/d*half_l*ratio, ey=jy+(my-jy)/d*half_l*ratio;
                hui__so_edge(out,&no,out_cap, ax,ay, ex,ey);
                hui__so_edge(out,&no,out_cap, ex,ey, bx,by);
            } else hui__so_edge(out,&no,out_cap, ax,ay, bx,by);
        } else {
            hui__so_edge(out,&no,out_cap, lx[i-1],ly[i-1], lx[i],ly[i]);
        }
        hui__so_edge(out,&no,out_cap, lx[i],ly[i], lx[i+1],ly[i+1]);
    }
    if(!closed){
        float lex=lx[nl-1],ley=ly[nl-1], rex=rx[nr-1],rey=ry[nr-1];
        float ex=qx[m-1],ey=qy[m-1];
        if(cap==HUI_CAP_ROUND) hui__so_arc(out,&no,out_cap, ex,ey, lex,ley, rex,rey, half_l);
        else if(cap==HUI_CAP_SQUARE){
            float dx=qx[m-1]-qx[m-2],dy=qy[m-1]-qy[m-2]; float l=sqrtf(dx*dx+dy*dy); if(l<1e-4f)l=1;
            float ux=dx/l*half_l, uy=dy/l*half_l;
            hui__so_edge(out,&no,out_cap, lex,ley, lex+ux,ley+uy);
            hui__so_edge(out,&no,out_cap, lex+ux,ley+uy, rex+ux,rey+uy);
            hui__so_edge(out,&no,out_cap, rex+ux,rey+uy, rex,rey);
        } else hui__so_edge(out,&no,out_cap, lex,ley, rex,rey);
    } else {
        hui__so_edge(out,&no,out_cap, lx[nl-1],ly[nl-1], lx[0],ly[0]);
    }
    hui__so_edge(out,&no,out_cap, rx[nr-1],ry[nr-1], rx[nr-2],ry[nr-2]);
    for(int i=nr-3;i>=1;i-=2){
        float jx=qx[((i+1)/2)%m], jy=qy[((i+1)/2)%m];
        if(join==HUI_JOIN_ROUND) hui__so_arc(out,&no,out_cap, jx,jy, rx[i+1],ry[i+1], rx[i],ry[i], half_r);
        else hui__so_edge(out,&no,out_cap, rx[i+1],ry[i+1], rx[i],ry[i]);
        hui__so_edge(out,&no,out_cap, rx[i],ry[i], rx[i-1],ry[i-1]);
    }
    if(!closed){
        float lsx=lx[0],lsy=ly[0], rsx=rx[0],rsy=ry[0];
        float sx=qx[0],sy=qy[0];
        if(cap==HUI_CAP_ROUND) hui__so_arc(out,&no,out_cap, sx,sy, rsx,rsy, lsx,lsy, half_l);
        else if(cap==HUI_CAP_SQUARE){
            float dx=qx[0]-qx[1],dy=qy[0]-qy[1]; float l=sqrtf(dx*dx+dy*dy); if(l<1e-4f)l=1;
            float ux=dx/l*half_l, uy=dy/l*half_l;
            hui__so_edge(out,&no,out_cap, rsx,rsy, rsx+ux,rsy+uy);
            hui__so_edge(out,&no,out_cap, rsx+ux,rsy+uy, lsx+ux,lsy+uy);
            hui__so_edge(out,&no,out_cap, lsx+ux,lsy+uy, lsx,lsy);
        } else hui__so_edge(out,&no,out_cap, rsx,rsy, lsx,lsy);
    } else {
        hui__so_edge(out,&no,out_cap, rx[0],ry[0], rx[nr-1],ry[nr-1]);
    }
    return no;
}

/* Split a flattened polyline into "on" dash runs by arc length. Writes each
 * run's points contiguously into ox/oy, with run_start[r]/run_len[r] describing
 * run r. Dash lengths are doc units scaled by `zoom`. Returns run count. */
static int hui__vec_dash_split(const float *px,const float *py,int n,
                        const float *dash,int ndash,float dash_offset,float zoom,
                        float *ox,float *oy,int *run_start,int *run_len,int max_runs,int max_pts){
    if(n<2||ndash<=0) return 0;
    float pat[16]; int np=0;
    for(int i=0;i<ndash&&np<16;i++){ float v=dash[i]*zoom; if(v<0)v=0; pat[np++]=v; }
    if(np%2==1 && np<16){ for(int i=0,cc=np;i<cc&&np<16;i++) pat[np++]=pat[i]; }
    float total=0; for(int i=0;i<np;i++) total+=pat[i]; if(total<1e-3f) return 0;
    float phase=dash_offset*zoom; phase=fmodf(phase,total); if(phase<0)phase+=total;
    int di=0; while(di<np && phase>=pat[di]){ phase-=pat[di]; di=(di+1)%np; if(di==0)break; }
    int on=(di%2==0);
    float rem=pat[di]-phase; if(rem<=0)rem=pat[di];
    int nr=0, np_out=0; int cur=-1;
    for(int s=0;s+1<n;s++){
        float ax=px[s],ay=py[s],bx=px[s+1],by=py[s+1];
        float dx=bx-ax,dy=by-ay; float seg=sqrtf(dx*dx+dy*dy); if(seg<1e-4f)continue;
        float ux=dx/seg,uy=dy/seg; float t=0;
        if(on && cur<0 && nr<max_runs && np_out<max_pts){
            cur=nr; run_start[nr]=np_out; ox[np_out]=ax; oy[np_out]=ay; np_out++; run_len[nr]=1;
        }
        while(t<seg){
            float step = rem<(seg-t)?rem:(seg-t);
            t+=step; rem-=step;
            float cx=ax+ux*t, cy=ay+uy*t;
            if(on && cur>=0 && np_out<max_pts){ ox[np_out]=cx; oy[np_out]=cy; np_out++; run_len[cur]++; }
            if(rem<=1e-4f){
                di=(di+1)%np; rem=pat[di]; on=!on;
                if(!on && cur>=0){ nr++; cur=-1; }
                else if(on && cur<0 && nr<max_runs && np_out<max_pts){ cur=nr; run_start[nr]=np_out;
                    ox[np_out]=cx; oy[np_out]=cy; np_out++; run_len[nr]=1; }
            }
        }
    }
    if(cur>=0 && run_len[cur]>=2) nr++;   /* drop a degenerate run opened at the very end */
    return nr;
}


/* Linear-gradient variant of the AA fill: same coverage accumulation, but
 * emitted in short chunks whose color is interpolated along the projection
 * of the pixel onto the (gx0,gy0)->(gx1,gy1) axis (screen space). */
/* kind: HUI_GRAD_LINEAR/RADIAL; nz: 0 even-odd spans, 1 nonzero-winding spans */
static void hui__vec_fill_edges_grad(const hui__vec_edge *e, int ne, hui_rect vp,
                                     hui_color ca, hui_color cb,
                                     float gx0, float gy0, float gx1, float gy1,
                                     int kind, int nz) {
    if (ne == 0) return;
    float ax = gx1 - gx0, ay = gy1 - gy0;
    float alen2 = ax*ax + ay*ay;
    if (alen2 < 1e-6f) { if(nz) hui__vec_fill_edges_nz(e,ne,vp,ca); else hui__vec_fill_edges(e, ne, vp, ca); return; }
    float rad = sqrtf(alen2);
    float minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < ne; i++) {
        if (e[i].y0 < minY) minY = e[i].y0;  if (e[i].y1 < minY) minY = e[i].y1;
        if (e[i].y0 > maxY) maxY = e[i].y0;  if (e[i].y1 > maxY) maxY = e[i].y1;
    }
    int y0 = (int)floorf(minY), y1 = (int)ceilf(maxY) + 1;
    if (y0 < vp.y) y0 = vp.y;
    if (y1 > vp.y + vp.h) y1 = vp.y + vp.h;
    enum { COVW = 8192 };
    static float cov[COVW];
    int cx0 = vp.x < 0 ? 0 : vp.x;
    int cx1 = vp.x + vp.w; if (cx1 > COVW) cx1 = COVW;
    for (int y = y0; y < y1; y++) {
        int rxa = cx1, rxb = -1;
        for (int sub = 0; sub < HUI_VEC_AA_SUB; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / (float)HUI_VEC_AA_SUB;
            float xs[256]; int dir[256]; int nx = 0;
            for (int i = 0; i < ne; i++) {
                float ey0 = e[i].y0, ey1 = e[i].y1, ex0 = e[i].x0, ex1 = e[i].x1;
                if ((sy >= ey0 && sy < ey1) || (sy >= ey1 && sy < ey0)) {
                    float t = (sy - ey0) / (ey1 - ey0);
                    if (nx < 256) { xs[nx]=ex0+t*(ex1-ex0); dir[nx]=(ey1>ey0)?1:-1; nx++; }
                }
            }
            for (int i = 1; i < nx; i++) {
                float v = xs[i]; int dd=dir[i]; int j = i - 1;
                while (j >= 0 && xs[j] > v) { xs[j+1] = xs[j]; dir[j+1]=dir[j]; j--; }
                xs[j+1] = v; dir[j+1]=dd;
            }
            float w = 1.0f / (float)HUI_VEC_AA_SUB;
            int wind=0; float spanx=0;
            for (int i = 0; i < nx; i++) {
                float fa, fb; int have=0;
                if (nz) {
                    int prev=wind; wind+=dir[i];
                    if (prev==0 && wind!=0){ spanx=xs[i]; continue; }
                    else if (prev!=0 && wind==0){ fa=spanx; fb=xs[i]; have=1; }
                    else continue;
                } else {
                    if (i+1>=nx || (i&1)) continue;   /* even-odd: pairs at even i */
                    fa=xs[i]; fb=xs[i+1]; have=1;
                }
                if(!have) continue;
                if (fa < (float)cx0) fa = (float)cx0;
                if (fb > (float)cx1) fb = (float)cx1;
                if (fb <= fa) continue;
                int ia = (int)fa, ib = (int)fb;
                if (ia >= cx1) continue;
                if (ia == ib) {
                    cov[ia] += (fb - fa) * w;
                } else {
                    cov[ia] += ((float)(ia+1) - fa) * w;
                    for (int px = ia+1; px < ib && px < cx1; px++) cov[px] += w;
                    if (ib < cx1) cov[ib] += (fb - (float)ib) * w;
                }
                if (ia < rxa) rxa = ia;
                if (ib > rxb) rxb = ib < cx1 ? ib : cx1-1;
            }
        }
        /* run-length emission keyed on (quantized coverage, quantized
         * gradient position): a vertical gradient costs one command per row,
         * a horizontal one ~TSTEPS commands per row — never per-pixel, so a
         * full-screen gradient cannot exhaust the draw list */
        enum { TSTEPS = 48 };
        float sy_mid = (float)y + 0.5f;
        #define HUI__GT(PX) (kind==HUI_GRAD_RADIAL \
            ? sqrtf(((PX)+0.5f-gx0)*((PX)+0.5f-gx0)+(sy_mid-gy0)*(sy_mid-gy0))/rad \
            : (((PX)+0.5f-gx0)*ax + (sy_mid-gy0)*ay)/alen2)
        int px = rxa;
        while (px <= rxb) {
            int a8 = (int)(cov[px] * 255.0f + 0.5f);
            if (a8 > 255) a8 = 255;
            a8 = (a8 + 8) & ~15;
            if (a8 > 255) a8 = 255;
            float t0 = HUI__GT((float)px);
            int qt = (int)((t0 < 0 ? 0 : t0 > 1 ? 1 : t0) * (float)(TSTEPS-1) + 0.5f);
            int run = px;
            while (run <= rxb) {
                int b8 = (int)(cov[run] * 255.0f + 0.5f);
                if (b8 > 255) b8 = 255;
                b8 = (b8 + 8) & ~15;
                if (b8 > 255) b8 = 255;
                float t1 = HUI__GT((float)run);
                int q1 = (int)((t1 < 0 ? 0 : t1 > 1 ? 1 : t1) * (float)(TSTEPS-1) + 0.5f);
                if (b8 != a8 || q1 != qt) break;
                cov[run] = 0.0f;
                run++;
            }
            if (a8 > 0) {
                float t = (float)qt / (float)(TSTEPS-1);
                hui_color cc = {
                    (uint8_t)((float)ca.r + t * ((float)cb.r - (float)ca.r)),
                    (uint8_t)((float)ca.g + t * ((float)cb.g - (float)ca.g)),
                    (uint8_t)((float)ca.b + t * ((float)cb.b - (float)ca.b)),
                    (uint8_t)(((float)ca.a + t * ((float)cb.a - (float)ca.a)) * (float)a8 / 255.0f) };
                hui_rect_fill(hui_rect_make(px, y, run - px, 1), cc, 0);
            }
            px = run;
        }
        #undef HUI__GT
    }
}

static hui_color hui__vec_alpha(hui_color c, float opacity) {
    float a = (float)c.a * (opacity < 0 ? 0 : opacity > 1 ? 1 : opacity);
    c.a = (uint8_t)(a + 0.5f);
    return c;
}

static void hui__vec_render_layer_objs(const hui_vec_doc *d, int li, hui_rect vp,
                                       float zoom, float pan_x, float pan_y);

/* document whose asset table image objects resolve against while rendering */
static const hui_vec_doc *hui__vec_cur_doc = NULL;

/* image object: inverse-affine blit of the asset through xf and the view.
 * Missing asset (file saved without a decoder, or gc'd) draws a placeholder
 * frame so the object stays visible and selectable. */
static void hui__vec_render_image(const hui_vec_obj *o, hui_vec_xform xf, hui_rect vp,
                                  float zoom, float pan_x, float pan_y, float op) {
    if (o->img_w <= 0 || o->img_h <= 0) return;
    /* local (image px) -> screen: M = view ∘ xf */
    float A = xf.a * zoom, B = xf.b * zoom, C = xf.c * zoom, D = xf.d * zoom;
    float E = (float)vp.x + pan_x + xf.e * zoom, F = (float)vp.y + pan_y + xf.f * zoom;
    /* screen bbox of the transformed image quad */
    float xs[4], ys[4];
    float cw = (float)o->img_w, ch = (float)o->img_h;
    xs[0]=E;            ys[0]=F;
    xs[1]=A*cw+E;       ys[1]=B*cw+F;
    xs[2]=C*ch+E;       ys[2]=D*ch+F;
    xs[3]=A*cw+C*ch+E;  ys[3]=B*cw+D*ch+F;
    float minx=xs[0],maxx=xs[0],miny=ys[0],maxy=ys[0];
    for (int i=1;i<4;i++){
        minx = xs[i]<minx ? xs[i] : minx;  maxx = xs[i]>maxx ? xs[i] : maxx;
        miny = ys[i]<miny ? ys[i] : miny;  maxy = ys[i]>maxy ? ys[i] : maxy;
    }
    /* clip the scan rect to the viewport (keeps it inside int16 at any zoom) */
    float fx0 = minx > (float)vp.x ? minx : (float)vp.x;
    float fy0 = miny > (float)vp.y ? miny : (float)vp.y;
    float fx1 = maxx < (float)(vp.x+vp.w) ? maxx : (float)(vp.x+vp.w);
    float fy1 = maxy < (float)(vp.y+vp.h) ? maxy : (float)(vp.y+vp.h);
    if (fx1 - fx0 < 1.0f || fy1 - fy0 < 1.0f) return;
    int dx0 = (int)floorf(fx0), dy0 = (int)floorf(fy0);
    int dx1 = (int)ceilf(fx1),  dy1 = (int)ceilf(fy1);
    const hui_vec_asset *a = hui_vec_asset_find(hui__vec_cur_doc, o->img_asset);
    if (a && a->rgba) {
        float det = A*D - B*C;
        if (fabsf(det) < 1e-9f) return;
        /* inverse of [A C E; B D F] scaled so local px == asset px */
        float sx = (float)a->w / cw, sy = (float)a->h / ch;
        float ia =  D/det, ic = -C/det, ie = (C*F - D*E)/det;
        float ib = -B/det, id =  A/det, iff = (B*E - A*F)/det;
        float inv[6] = { ia*sx, ic*sx, ie*sx,  ib*sy, id*sy, iff*sy };
        float ov = op < 0 ? 0 : op > 1 ? 1 : op;
        hui_image_rgba_affine(hui_rect_make(dx0, dy0, dx1-dx0, dy1-dy0),
                              a->rgba, a->w, a->h, inv, (uint8_t)(ov * 255.0f + 0.5f));
    } else {
        hui_color fc = hui__vec_alpha((hui_color){120,120,130,160}, op);
        for (int i = 0; i < 4; i++) {
            static const int nxt[4] = {1,3,0,2};
            hui_line((int)xs[i],(int)ys[i],(int)xs[nxt[i]],(int)ys[nxt[i]], fc, 1);
        }
        hui_line((int)xs[0],(int)ys[0],(int)xs[3],(int)ys[3], fc, 1);
    }
}

void hui_vec_render(const hui_vec_doc *d, hui_rect vp,
                    float zoom, float pan_x, float pan_y) {
    if (!d || zoom <= 0.0f) return;
    hui__vec_cur_doc = d;
    hui_clip_push(vp);
    /* document background */
    {
        int bx = vp.x + (int)pan_x, by = vp.y + (int)pan_y;
        hui_rect_fill(hui_rect_make(bx, by,
                      (int)((float)d->w * zoom), (int)((float)d->h * zoom)), d->bg, 0);
    }
    for (int li = 0; li < d->nlayer; li++) {
        if (d->layers[li].visible)
            hui__vec_render_layer_objs(d, li, vp, zoom, pan_x, pan_y);
    }
    hui_clip_pop();
}

void hui_vec_render_layer(const hui_vec_doc *d, int layer, hui_rect vp,
                          float zoom, float pan_x, float pan_y) {
    if (!d || layer < 0 || layer >= d->nlayer || zoom <= 0.0f) return;
    hui__vec_cur_doc = d;
    hui_clip_push(vp);
    hui__vec_render_layer_objs(d, layer, vp, zoom, pan_x, pan_y);
    hui_clip_pop();
}

/* outer glow: stacked outside-aligned stroke bands (wide+faint -> narrow+strong)
 * in the glow color, drawn behind the object to fake a soft halo */
static void hui__vec_render_glow(const hui_vec_obj *o, hui_vec_xform xf, hui_rect vp,
                                 float zoom, float pan_x, float pan_y, float op) {
    if (!o->has_glow || o->glow_radius<=0) return;
    float R = o->glow_radius * zoom; if (R<1) return;
    enum { GN = 6 };
    for (int ci=0; ci<o->ncontour; ci++){
        static float gx[HUI__VEC_FLAT_CAP], gy[HUI__VEC_FLAT_CAP];
        int fn = hui__vec_flatten(&o->contours[ci], xf, vp, zoom, pan_x, pan_y, gx, gy);
        if (fn<2) continue;
        int closed = (o->contours[ci].nseg>0 &&
            fabsf(o->contours[ci].segs[o->contours[ci].nseg-1].a.x-o->contours[ci].start.x)<0.5f &&
            fabsf(o->contours[ci].segs[o->contours[ci].nseg-1].a.y-o->contours[ci].start.y)<0.5f);
        /* interior side (so "outside" pushes the halo outward) */
        float area=0; for(int j=0;j<fn;j++){int k=(j+1)%fn; area+=gx[j]*gy[k]-gx[k]*gy[j];}
        int left_is_inside = (area>0);
        for (int k=0;k<GN;k++){
            float t=(float)(k+1)/(float)GN;              /* 1 = innermost/edge */
            float width = R*(1.0f-t) + R/(float)GN;      /* wide outer -> narrow inner */
            float hl,hr;
            if(left_is_inside){ hl=0; hr=width; } else { hl=width; hr=0; }
            hui_color gc = o->glow_color;
            float a = o->glow_intensity * op * t * 0.5f;   /* stronger near the edge */
            gc.a = (uint8_t)((float)gc.a * (a<0?0:a>1?1:a));
            if (gc.a==0) continue;
            static hui__vec_edge ge[4096];
            int gne = hui__vec_stroke_outline(gx,gy,fn,closed, hl,hr,
                        HUI_JOIN_ROUND, HUI_CAP_ROUND, 4.0f, ge, 4096);
            hui__vec_fill_edges_nz(ge, gne, vp, gc);
        }
    }
}

/* repeat marker_text glyphs along the object's path, oriented to the tangent */
static void hui__vec_render_markers(const hui_vec_obj *o, hui_vec_xform xf, hui_rect vp,
                                    float zoom, float pan_x, float pan_y, float op) {
    if (o->marker_mode != HUI_MARKER_GLYPH || !o->marker_text[0] || !hui_vec_font_lookup) return;
    hui_glyph_font *f = hui_vec_font_lookup(o->marker_font[0]?o->marker_font:"kvitka_mono");
    if (!f || f->units_per_em<=0) return;
    float ems = o->marker_size>0?o->marker_size:16.0f;
    float s = ems * zoom / (float)f->units_per_em;
    hui_color col = hui__vec_alpha(o->has_stroke?o->stroke:o->fill, op);
    float adv=0; { const char *p=o->marker_text; while(*p){ uint32_t cp=hui_utf8_next(&p);
        hui_glyph *g=hui_glyph_find(f,cp); if(g) adv+=(float)g->advance; } }
    float advpx = adv*s;
    float spacing = (o->marker_spacing>0? o->marker_spacing*zoom : (advpx>1?advpx:1));
    if (spacing<1) spacing=1;
    int placed=0;
    for (int ci=0; ci<o->ncontour && placed<4096; ci++){
        static float fx[HUI__VEC_FLAT_CAP], fy[HUI__VEC_FLAT_CAP];
        int fn = hui__vec_flatten(&o->contours[ci], xf, vp, zoom, pan_x, pan_y, fx, fy);
        if (fn<2) continue;
        float next = o->marker_offset*zoom; if(next<0)next=0;
        for (int seg=0; seg+1<fn && placed<4096; seg++){
            float ax=fx[seg],ay=fy[seg],bx=fx[seg+1],by=fy[seg+1];
            float dx=bx-ax,dy=by-ay; float L=sqrtf(dx*dx+dy*dy); if(L<1e-3f)continue;
            float tx=dx/L, ty=dy/L, pos=0;
            while (next <= L-pos+1e-4f && placed<4096){
                pos += next; next = spacing;
                float pxp=ax+tx*pos, pyp=ay+ty*pos;
                static hui__vec_edge me[2048]; int mne=0;
                float pen=-advpx*0.5f;
                const char *p=o->marker_text;
                while(*p){ uint32_t cp=hui_utf8_next(&p); hui_glyph *g=hui_glyph_find(f,cp);
                    if(!g) continue;
                    for(int gc=0; gc<g->ncontour; gc++){
                        const hui_contour *c=&g->contours[gc];
                        static float gx[HUI__VEC_FLAT_CAP], gy[HUI__VEC_FLAT_CAP]; int gn=0;
                        gx[gn]=c->start.x; gy[gn]=c->start.y; gn++;
                        hui_v2 prev=c->start;
                        for(int j=0;j<c->nseg;j++){ if(gn<HUI__VEC_FLAT_CAP-1)
                            hui__cubic_flat(prev,c->segs[j].c0,c->segs[j].c1,c->segs[j].a,gx,gy,&gn,HUI__VEC_FLAT_CAP,0);
                            prev=c->segs[j].a; }
                        for(int j=0;j<gn;j++){
                            float lx=pen+gx[j]*s, ly=-gy[j]*s;
                            float ex=pxp + lx*tx - ly*ty, ey=pyp + lx*ty + ly*tx;
                            int k=(j+1)%gn;
                            float lx2=pen+gx[k]*s, ly2=-gy[k]*s;
                            float ex2=pxp + lx2*tx - ly2*ty, ey2=pyp + lx2*ty + ly2*tx;
                            if(mne<2048) me[mne++]=(hui__vec_edge){ex,ey,ex2,ey2};
                        }
                    }
                    pen += (float)g->advance * s;
                }
                if(mne) hui__vec_fill_edges(me,mne,vp,col);
                placed++;
            }
            next -= (L-pos); if(next<0)next=0;
        }
    }
}

static void hui__vec_render_node(const hui_vec_obj *o, hui_vec_xform parent_xf,
                                 float parent_op, hui_rect vp,
                                 float zoom, float pan_x, float pan_y) {
    enum { HUI__VEC_MAXE = 4096 };
    static hui__vec_edge edges[HUI__VEC_MAXE];
    if (!o->visible) return;
    hui_vec_xform xf = hui_vec_xform_mul(parent_xf, o->xf);
    float op = parent_op * o->opacity;
    if (o->kind == HUI_VEC_GROUP) {
        for (int i = 0; i < o->nobj; i++)
            hui__vec_render_node(&o->objs[i], xf, op, vp, zoom, pan_x, pan_y);
        return;
    }
    if (o->kind == HUI_VEC_IMAGE) {
        hui__vec_render_image(o, xf, vp, zoom, pan_x, pan_y, op);
        return;
    }
    if (o->kind == HUI_VEC_TEXT) {
        if (!hui_vec_font_lookup) return;
        hui_glyph_font *f = hui_vec_font_lookup(o->font_family);
        if (!f) return;
        hui_v2 org = hui_vec_xform_apply(xf, (hui_v2){0,0});
        int tx = vp.x + (int)(pan_x + org.x * zoom);
        int ty = vp.y + (int)(pan_y + org.y * zoom);
        if (o->has_fill)
            hui_glyph_text(f, tx, ty, o->font_size * zoom, o->text,
                           hui__vec_alpha(o->fill, op), 0, 0);
        return;
    }
    if (o->blend_mode) hui_blend_mode = (uint8_t)o->blend_mode;   /* composite this object */
    /* outer glow behind everything else */
    hui__vec_render_glow(o, xf, vp, zoom, pan_x, pan_y, op);
    int ne = 0;
    float ox[HUI__VEC_FLAT_CAP], oy[HUI__VEC_FLAT_CAP];
    /* gather all contours of the object into one edge list (even-odd
     * across contours gives correct holes) */
    for (int ci = 0; ci < o->ncontour; ci++) {
        int fn = hui__vec_flatten(&o->contours[ci], xf, vp,
                                  zoom, pan_x, pan_y, ox, oy);
        for (int j = 0; j < fn; j++) {
            int k = (j + 1) % fn;
            if (ne < HUI__VEC_MAXE)
                edges[ne++] = (hui__vec_edge){ox[j],oy[j],ox[k],oy[k]};
        }
    }
    if (o->has_fill) {
        if (o->has_grad) {
            /* gradient axis: local points through the object transform into
             * screen space, colors carry object opacity */
            hui_v2 g0 = hui_vec_xform_apply(xf, o->grad_p0);
            hui_v2 g1 = hui_vec_xform_apply(xf, o->grad_p1);
            float sx0 = (float)vp.x + pan_x + g0.x * zoom;
            float sy0 = (float)vp.y + pan_y + g0.y * zoom;
            float sx1 = (float)vp.x + pan_x + g1.x * zoom;
            float sy1 = (float)vp.y + pan_y + g1.y * zoom;
            hui__vec_fill_edges_grad(edges, ne, vp,
                                     hui__vec_alpha(o->grad_a, op),
                                     hui__vec_alpha(o->grad_b, op),
                                     sx0, sy0, sx1, sy1, o->grad_kind, 0);
        } else {
            hui__vec_fill_edges(edges, ne, vp, hui__vec_alpha(o->fill, op));
        }
    }
    if (o->has_stroke) {
        float th = o->stroke_w * zoom;
        if (th < 0.75f) th = 0.75f;
        hui_color sc = hui__vec_alpha(o->stroke, op);
        /* gradient stroke: paint the outline with the object's gradient */
        int grads = o->grad_stroke;
        float gsx0=0,gsy0=0,gsx1=0,gsy1=0;
        if (grads) {
            hui_v2 g0=hui_vec_xform_apply(xf,o->grad_p0), g1=hui_vec_xform_apply(xf,o->grad_p1);
            gsx0=(float)vp.x+pan_x+g0.x*zoom; gsy0=(float)vp.y+pan_y+g0.y*zoom;
            gsx1=(float)vp.x+pan_x+g1.x*zoom; gsy1=(float)vp.y+pan_y+g1.y*zoom;
        }
        #define HUI__STROKE_FILL(E,NE) do { if(grads) \
            hui__vec_fill_edges_grad((E),(NE),vp, hui__vec_alpha(o->grad_a,op),hui__vec_alpha(o->grad_b,op), \
                gsx0,gsy0,gsx1,gsy1, o->grad_kind, 1); \
            else hui__vec_fill_edges_nz((E),(NE),vp,sc); } while(0)
        for (int ci = 0; ci < o->ncontour; ci++) {
            int fn = hui__vec_flatten(&o->contours[ci], xf, vp, zoom, pan_x, pan_y, ox, oy);
            if (fn < 2) continue;
            const hui_contour *c = &o->contours[ci];
            int closed = (c->nseg>0 &&
                fabsf(c->segs[c->nseg-1].a.x-c->start.x)<0.5f &&
                fabsf(c->segs[c->nseg-1].a.y-c->start.y)<0.5f);
            /* dashed strokes: split into on-runs, stroke each (center align v1) */
            if (o->ndash > 0) {
                static float dpx[4096], dpy[4096]; static int drs[256], drl[256];
                int nrun = hui__vec_dash_split(ox,oy,fn, o->dash,o->ndash,o->dash_offset,zoom,
                                               dpx,dpy, drs,drl, 256, 4096);
                for (int r=0;r<nrun;r++){
                    int st=drs[r], ln=drl[r]; if(ln<2) continue;
                    if (th<=1.4f){ for(int j=0;j+1<ln;j++) hui_line((int)dpx[st+j],(int)dpy[st+j],(int)dpx[st+j+1],(int)dpy[st+j+1],sc,1); continue; }
                    static hui__vec_edge de[2048];
                    int dne=hui__vec_stroke_outline(&dpx[st],&dpy[st],ln,0, th*0.5f,th*0.5f,
                              o->stroke_join,o->stroke_cap,o->miter_limit, de,2048);
                    HUI__STROKE_FILL(de,dne);
                }
                continue;
            }
            if (th <= 1.4f) {   /* thin: fast 1px polyline */
                int lim = closed ? fn : fn-1;
                for (int j=0;j<lim;j++){ int k=(j+1)%fn;
                    hui_line((int)ox[j],(int)oy[j],(int)ox[k],(int)oy[k], sc, 1); }
                continue;
            }
            /* alignment -> per-side offsets; interior side from signed area */
            float hl, hr;
            if (o->stroke_align==HUI_STROKE_CENTER){ hl=hr=th*0.5f; }
            else {
                float inside = th;
                int left_is_inside = 1;
                if (closed){ float area=0; for(int j=0;j<fn;j++){int k=(j+1)%fn; area+=ox[j]*oy[k]-ox[k]*oy[j];}
                    left_is_inside = (area>0); }
                if (o->stroke_align==HUI_STROKE_INSIDE){ hl=left_is_inside?inside:0; hr=left_is_inside?0:inside; }
                else { hl=left_is_inside?0:inside; hr=left_is_inside?inside:0; }
            }
            static hui__vec_edge se[4096];
            int sne = hui__vec_stroke_outline(ox,oy,fn,closed, hl,hr,
                        o->stroke_join,o->stroke_cap,o->miter_limit, se, 4096);
            HUI__STROKE_FILL(se, sne);
        }
        #undef HUI__STROKE_FILL
    }
    hui__vec_render_markers(o, xf, vp, zoom, pan_x, pan_y, op);
    hui_blend_mode = 0;
}

static void hui__vec_render_one(const hui_vec_obj *o, hui_rect vp,
                                float zoom, float pan_x, float pan_y) {
    hui__vec_render_node(o, hui_vec_xform_identity(), 1.0f, vp, zoom, pan_x, pan_y);
}

static void hui__vec_render_layer_objs(const hui_vec_doc *d, int li, hui_rect vp,
                                       float zoom, float pan_x, float pan_y) {
    const hui_vec_layer *l = &d->layers[li];
    for (int oi = 0; oi < l->nobj; oi++)
        hui__vec_render_one(&l->objs[oi], vp, zoom, pan_x, pan_y);
}

void hui_vec_render_object(const hui_vec_doc *d, int layer, int obj,
                           hui_rect vp, float zoom, float pan_x, float pan_y) {
    if (!d || layer < 0 || layer >= d->nlayer || zoom <= 0.0f) return;
    const hui_vec_layer *l = &d->layers[layer];
    if (obj < 0 || obj >= l->nobj) return;
    hui__vec_cur_doc = d;
    hui_clip_push(vp);
    hui__vec_render_one(&l->objs[obj], vp, zoom, pan_x, pan_y);
    hui_clip_pop();
}

void hui_vec_render_single(const hui_vec_doc *d, const hui_vec_obj *o,
                           hui_rect vp, float zoom, float pan_x, float pan_y) {
    if (!d || !o || zoom <= 0.0f) return;
    hui__vec_cur_doc = d;
    hui_clip_push(vp);
    /* preview always draws the node even if the object is toggled hidden;
       children still honor their own visibility inside the subtree */
    if (o->kind == HUI_VEC_GROUP) {
        for (int i = 0; i < o->nobj; i++)
            hui__vec_render_node(&o->objs[i], o->xf, o->opacity, vp, zoom, pan_x, pan_y);
    } else {
        hui_vec_obj tmp = *o; tmp.visible = 1;
        hui__vec_render_node(&tmp, hui_vec_xform_identity(), 1.0f, vp, zoom, pan_x, pan_y);
    }
    hui_clip_pop();
}

/* ---- .barva save/load ---- */

static void hui__vec_write_obj(FILE *fp, const hui_vec_obj *o) {
    if (o->kind == HUI_VEC_GROUP) {
        fprintf(fp, "group \"%s\" id %u opacity %.6g\n",
                o->name, o->id, (double)o->opacity);
        fprintf(fp, "xf %.6g %.6g %.6g %.6g %.6g %.6g\n",
                (double)o->xf.a,(double)o->xf.b,(double)o->xf.c,
                (double)o->xf.d,(double)o->xf.e,(double)o->xf.f);
        if (!o->visible) fprintf(fp, "hidden\n");
        if (o->locked)   fprintf(fp, "locked\n");
        for (int i = 0; i < o->nobj; i++) hui__vec_write_obj(fp, &o->objs[i]);
        fprintf(fp, "endgroup\n");
        return;
    }
    fprintf(fp, "obj %s \"%s\" id %u opacity %.6g\n",
            o->kind == HUI_VEC_TEXT ? "text" : o->kind == HUI_VEC_IMAGE ? "image" : "path",
            o->name, o->id, (double)o->opacity);
    fprintf(fp, "xf %.6g %.6g %.6g %.6g %.6g %.6g\n",
            (double)o->xf.a,(double)o->xf.b,(double)o->xf.c,
            (double)o->xf.d,(double)o->xf.e,(double)o->xf.f);
    if (!o->visible) fprintf(fp, "hidden\n");
    if (o->locked)   fprintf(fp, "locked\n");
    if (o->has_fill)
        fprintf(fp, "fill %u %u %u %u\n", o->fill.r,o->fill.g,o->fill.b,o->fill.a);
    if (o->has_stroke)
        fprintf(fp, "stroke %u %u %u %u %.6g\n",
                o->stroke.r,o->stroke.g,o->stroke.b,o->stroke.a,(double)o->stroke_w);
    if (o->has_stroke && (o->stroke_align||o->stroke_join||o->stroke_cap||o->miter_limit!=4.0f))
        fprintf(fp, "strokestyle %d %d %d %.6g\n",
                o->stroke_align,o->stroke_join,o->stroke_cap,(double)o->miter_limit);
    if (o->ndash>0){
        fprintf(fp, "dash %.6g %d", (double)o->dash_offset, o->ndash);
        for(int i=0;i<o->ndash;i++) fprintf(fp, " %.6g",(double)o->dash[i]);
        fprintf(fp, "\n");
    }
    if (o->has_grad || o->grad_stroke) {
        fprintf(fp, "grad %.6g %.6g %.6g %.6g %u %u %u %u %u %u %u %u\n",
                (double)o->grad_p0.x,(double)o->grad_p0.y,
                (double)o->grad_p1.x,(double)o->grad_p1.y,
                o->grad_a.r,o->grad_a.g,o->grad_a.b,o->grad_a.a,
                o->grad_b.r,o->grad_b.g,o->grad_b.b,o->grad_b.a);
        if (o->grad_kind)       fprintf(fp, "gradkind %d\n", o->grad_kind);
        if (o->grad_stroke)     fprintf(fp, "gradstroke\n");
        if (o->grad_stroke && !o->has_grad) fprintf(fp, "gradfilloff\n");
    }
    if (o->marker_mode)
        fprintf(fp, "marker %d %.6g %.6g %.6g \"%s\" \"%s\"\n",
                o->marker_mode, (double)o->marker_size, (double)o->marker_spacing,
                (double)o->marker_offset, o->marker_font, o->marker_text);
    if (o->has_glow)
        fprintf(fp, "glow %u %u %u %u %.6g %.6g\n",
                o->glow_color.r,o->glow_color.g,o->glow_color.b,o->glow_color.a,
                (double)o->glow_radius,(double)o->glow_intensity);
    if (o->blend_mode) fprintf(fp, "blend %d\n", o->blend_mode);
    if (o->kind == HUI_VEC_TEXT)
        fprintf(fp, "text \"%s\" \"%s\" %.6g\n", o->text, o->font_family, (double)o->font_size);
    if (o->kind == HUI_VEC_IMAGE)
        fprintf(fp, "image %u %d %d\n", o->img_asset, o->img_w, o->img_h);
    for (int ci = 0; ci < o->ncontour; ci++) {
        const hui_contour *c = &o->contours[ci];
        fprintf(fp, "c %.6g %.6g\n", (double)c->start.x, (double)c->start.y);
        for (int si = 0; si < c->nseg; si++) {
            const hui_seg *s = &c->segs[si];
            fprintf(fp, "s %.6g %.6g %.6g %.6g %.6g %.6g\n",
                    (double)s->c0.x,(double)s->c0.y,(double)s->c1.x,
                    (double)s->c1.y,(double)s->a.x,(double)s->a.y);
        }
    }
    fprintf(fp, "endobj\n");
}

int hui_vec_save_ex(const hui_vec_doc *d, const char *path, int flags) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "barva 2\n");
    fprintf(fp, "doc %d %d\n", d->w, d->h);
    fprintf(fp, "bg %u %u %u %u\n", d->bg.r, d->bg.g, d->bg.b, d->bg.a);
    if (!(flags & HUI_VEC_SAVE_SKIP_ASSETS) && d->nasset > 0) {
        /* only assets some object still references */
        uint32_t *used = (uint32_t*)malloc((size_t)d->nasset * sizeof *used);
        int nused = 0;
        if (used) {
            for (int li = 0; li < d->nlayer; li++)
                for (int oi = 0; oi < d->layers[li].nobj; oi++)
                    hui__vec_mark_assets_rec(&d->layers[li].objs[oi], used, &nused, d->nasset);
            for (int k = 0; k < nused; k++) {
                const hui_vec_asset *a = hui_vec_asset_find(d, used[k]);
                if (a) hui__vec_write_asset(fp, a);
            }
            free(used);
        }
    }
    for (int li = 0; li < d->nlayer; li++) {
        const hui_vec_layer *l = &d->layers[li];
        fprintf(fp, "layer \"%s\" visible %d locked %d\n", l->name, l->visible, l->locked);
        for (int oi = 0; oi < l->nobj; oi++) hui__vec_write_obj(fp, &l->objs[oi]);
    }
    fclose(fp);
    return 0;
}
int hui_vec_save(const hui_vec_doc *d, const char *path) { return hui_vec_save_ex(d, path, 0); }

/* --- v1 group migration: legacy files stored a flat group tag per object. We
   stash the tag in the object's id field (high bit marks it) during the load,
   then wrap same-tag members of each layer into a real group afterward. Using
   the value-carried id (not a pointer table) is realloc- and move-safe. --- */
#define HUI__V1_TAGBIT 0x80000000u
static void hui__vec_migrate_v1_groups(hui_vec_doc *d) {
    for (int li = 0; li < d->nlayer; li++) {
        hui_vec_layer *l = &d->layers[li];
        int has = 0;
        for (int i = 0; i < l->nobj; i++) if (l->objs[i].id & HUI__V1_TAGBIT) { has = 1; break; }
        if (!has) continue;
        hui_vec_obj *out = NULL; int outn = 0, outcap = 0;
        int done_tag[256]; int ndone = 0;
        for (int oi = 0; oi < l->nobj; oi++) {
            uint32_t raw = l->objs[oi].id;
            if (!(raw & HUI__V1_TAGBIT)) {                 /* ungrouped: keep flat */
                hui_vec_obj *slot = hui__vec_obj_push(&out, &outn, &outcap, l->objs[oi].kind);
                if (slot) *slot = l->objs[oi];
                continue;
            }
            int tag = (int)(raw & ~HUI__V1_TAGBIT);
            int already = 0; for (int k = 0; k < ndone; k++) if (done_tag[k] == tag) already = 1;
            if (already) continue;
            if (ndone < 256) done_tag[ndone++] = tag;
            hui_vec_obj *grp = hui__vec_obj_push(&out, &outn, &outcap, HUI_VEC_GROUP);
            if (!grp) continue;
            grp->kind = HUI_VEC_GROUP; grp->visible = 1; grp->opacity = 1.0f;
            grp->xf = hui_vec_xform_identity();
            snprintf(grp->name, sizeof grp->name, "group %d", tag);
            for (int oj = 0; oj < l->nobj; oj++) {
                uint32_t r2 = l->objs[oj].id;
                if ((r2 & HUI__V1_TAGBIT) && (int)(r2 & ~HUI__V1_TAGBIT) == tag) {
                    hui_vec_obj *ch = hui__vec_obj_push(&grp->objs, &grp->nobj, &grp->ocap, l->objs[oj].kind);
                    if (ch) { *ch = l->objs[oj]; ch->id = 0; }   /* clear tag; assign_ids stamps */
                }
            }
        }
        free(l->objs);
        l->objs = out; l->nobj = outn; l->ocap = outcap;
    }
}

typedef struct {
    hui_vec_doc *d;
    hui_vec_layer *cl;                 /* current layer */
    hui_vec_obj *gstack[32]; int gsp;  /* open groups (standalone heap) */
    hui_vec_obj *co;                   /* current leaf obj */
    hui_contour *cc;                   /* current contour */
    int version;
    int clip_mode;                     /* clipboard text: never switch/create layers */
} hui__vec_parser;

/* one line of .barva / clipboard text */
static void hui__vec_parse_line(hui__vec_parser *P, char *line) {
    enum { HV_MAXDEPTH = 32 };
    if (strncmp(line, "asset ", 6) == 0) { hui__vec_parse_asset(P->d, line); return; }
    if (P->clip_mode && strncmp(line, "layer ", 6) == 0) return;
    {
        unsigned r,g,b,a; float f0,f1,f2,f3,f4,f5; int i0,i1; unsigned uid; char nm[256];
        if (sscanf(line, "doc %d %d", &i0, &i1) == 2) { P->d->w=i0; P->d->h=i1; }
        else if (sscanf(line, "bg %u %u %u %u", &r,&g,&b,&a) == 4)
            P->d->bg = (hui_color){(uint8_t)r,(uint8_t)g,(uint8_t)b,(uint8_t)a};
        else if (sscanf(line, "layer \"%255[^\"]\" visible %d locked %d", nm,&i0,&i1) == 3) {
            P->cl = hui_vec_layer_add(P->d, nm);
            if (P->cl) { P->cl->visible = i0; P->cl->locked = i1; }
            P->co = NULL; P->cc = NULL; P->gsp = 0;
        }
        /* group open */
        else if ((nm[0]=0, uid=0, f0=1.0f,
                  sscanf(line, "group \"%255[^\"]\" id %u opacity %f", nm,&uid,&f0) >= 2 ||
                  sscanf(line, "group \"\" id %u opacity %f", &uid,&f0) >= 1)) {
            if (!P->cl) P->cl = hui_vec_layer_add(P->d, "layer");
            hui_vec_obj *ng = (hui_vec_obj*)calloc(1, sizeof *ng);
            if (ng) {
                ng->kind = HUI_VEC_GROUP; ng->visible = 1; ng->opacity = f0>0?f0:1.0f;
                ng->xf = hui_vec_xform_identity();
                snprintf(ng->name, sizeof ng->name, "%s", nm);
                ng->id = uid;
                if (P->gsp < HV_MAXDEPTH) P->gstack[P->gsp++] = ng; else free(ng);
            }
            P->co = NULL; P->cc = NULL;
        }
        else if (strncmp(line, "endgroup", 8) == 0) {
            if (P->gsp > 0) {
                hui_vec_obj *done = P->gstack[--P->gsp];
                hui_vec_obj *slot = (P->gsp > 0)
                    ? hui__vec_obj_push(&P->gstack[P->gsp-1]->objs, &P->gstack[P->gsp-1]->nobj, &P->gstack[P->gsp-1]->ocap, HUI_VEC_GROUP)
                    : (P->cl ? hui__vec_obj_push(&P->cl->objs, &P->cl->nobj, &P->cl->ocap, HUI_VEC_GROUP) : NULL);
                if (slot) *slot = *done;
                free(done);   /* frees the shell; child arrays were moved into slot */
            }
            P->co = NULL; P->cc = NULL;
        }
        /* object open: v2 (id) then v1 (group) forms */
        else if ((nm[0]=0, uid=0, i0=0, f0=1.0f,
                  sscanf(line, "obj path \"%255[^\"]\" id %u opacity %f", nm,&uid,&f0) == 3 ||
                  sscanf(line, "obj text \"%255[^\"]\" id %u opacity %f", nm,&uid,&f0) == 3 ||
                  sscanf(line, "obj image \"%255[^\"]\" id %u opacity %f", nm,&uid,&f0) == 3 ||
                  sscanf(line, "obj image \"\" id %u opacity %f", &uid,&f0) == 2 ||
                  sscanf(line, "obj path \"\" id %u opacity %f", &uid,&f0) == 2 ||
                  sscanf(line, "obj text \"\" id %u opacity %f", &uid,&f0) == 2 ||
                  sscanf(line, "obj path \"%255[^\"]\" group %d opacity %f", nm,&i0,&f0) == 3 ||
                  sscanf(line, "obj text \"%255[^\"]\" group %d opacity %f", nm,&i0,&f0) == 3 ||
                  sscanf(line, "obj path \"\" group %d opacity %f", &i0,&f0) == 2 ||
                  sscanf(line, "obj text \"\" group %d opacity %f", &i0,&f0) == 2)) {
            if (!P->cl) P->cl = hui_vec_layer_add(P->d, "layer");
            hui_vec_kind k = strncmp(line+4,"text",4)==0 ? HUI_VEC_TEXT
                           : strncmp(line+4,"image",5)==0 ? HUI_VEC_IMAGE : HUI_VEC_PATH;
            P->co = (P->gsp > 0) ? hui_vec_group_child_add(P->gstack[P->gsp-1], k)
                           : (P->cl ? hui_vec_obj_add(P->cl, k) : NULL);
            if (P->co) {
                snprintf(P->co->name, sizeof P->co->name, "%s", nm);
                P->co->id = uid; P->co->opacity = f0;
                P->co->has_fill = 0; P->co->has_stroke = 0;
                if (P->version == 1 && i0 != 0) P->co->id = HUI__V1_TAGBIT | (uint32_t)i0;
            }
            P->cc = NULL;
        }
        else if (P->co && sscanf(line, "xf %f %f %f %f %f %f", &f0,&f1,&f2,&f3,&f4,&f5) == 6)
            P->co->xf = (hui_vec_xform){f0,f1,f2,f3,f4,f5};
        else if (!P->co && P->gsp>0 && sscanf(line, "xf %f %f %f %f %f %f", &f0,&f1,&f2,&f3,&f4,&f5) == 6)
            P->gstack[P->gsp-1]->xf = (hui_vec_xform){f0,f1,f2,f3,f4,f5};
        else if (strncmp(line, "hidden", 6) == 0) { if (P->co) P->co->visible = 0; else if (P->gsp>0) P->gstack[P->gsp-1]->visible = 0; }
        else if (strncmp(line, "locked", 6) == 0) { if (P->co) P->co->locked = 1;  else if (P->gsp>0) P->gstack[P->gsp-1]->locked = 1; }
        else if (P->co && sscanf(line, "fill %u %u %u %u", &r,&g,&b,&a) == 4) {
            P->co->has_fill = 1;
            P->co->fill = (hui_color){(uint8_t)r,(uint8_t)g,(uint8_t)b,(uint8_t)a};
        }
        else if (P->co && sscanf(line, "stroke %u %u %u %u %f", &r,&g,&b,&a,&f0) == 5) {
            P->co->has_stroke = 1; P->co->stroke_w = f0;
            P->co->stroke = (hui_color){(uint8_t)r,(uint8_t)g,(uint8_t)b,(uint8_t)a};
        }
        else if (P->co && sscanf(line, "grad %f %f %f %f %u %u %u %u", &f0,&f1,&f2,&f3,&r,&g,&b,&a) == 8) {
            unsigned r2,g2,b2,a2;
            P->co->has_grad = 1;
            P->co->grad_p0 = (hui_v2){f0,f1}; P->co->grad_p1 = (hui_v2){f2,f3};
            P->co->grad_a = (hui_color){(uint8_t)r,(uint8_t)g,(uint8_t)b,(uint8_t)a};
            if (sscanf(line, "grad %*f %*f %*f %*f %*u %*u %*u %*u %u %u %u %u",
                       &r2,&g2,&b2,&a2) == 4)
                P->co->grad_b = (hui_color){(uint8_t)r2,(uint8_t)g2,(uint8_t)b2,(uint8_t)a2};
        }
        else if (P->co && sscanf(line,"blend %d",&i0)==1) P->co->blend_mode=i0;
        else if (P->co && strncmp(line,"glow ",5)==0) {
            unsigned gr,gg,gb,ga; float grad_=8,gi=0.8f;
            if (sscanf(line,"glow %u %u %u %u %f %f",&gr,&gg,&gb,&ga,&grad_,&gi)>=4){
                P->co->has_glow=1; P->co->glow_color=(hui_color){(uint8_t)gr,(uint8_t)gg,(uint8_t)gb,(uint8_t)ga};
                P->co->glow_radius=grad_; P->co->glow_intensity=gi;
            }
        }
        else if (P->co && strncmp(line,"marker ",7)==0) {
            int mm=0; float ms=16,msp=0,mo=0; char mf[64]="", mt[64]="";
            if (sscanf(line,"marker %d %f %f %f \"%63[^\"]\" \"%63[^\"]\"",&mm,&ms,&msp,&mo,mf,mt)>=4){
                P->co->marker_mode=mm; P->co->marker_size=ms; P->co->marker_spacing=msp; P->co->marker_offset=mo;
                snprintf(P->co->marker_font,sizeof P->co->marker_font,"%s",mf);
                snprintf(P->co->marker_text,sizeof P->co->marker_text,"%s",mt);
            }
        }
        else if (P->co && sscanf(line, "gradkind %d", &i0)==1) P->co->grad_kind=i0;
        else if (P->co && strncmp(line,"gradstroke",10)==0) P->co->grad_stroke=1;
        else if (P->co && strncmp(line,"gradfilloff",11)==0) P->co->has_grad=0;
        else if (P->co && P->co->kind == HUI_VEC_TEXT &&
                 sscanf(line, "text \"%255[^\"]\"", nm) == 1) {
            snprintf(P->co->text, sizeof P->co->text, "%s", nm);
            const char *q = strchr(line + 6 + strlen(nm), '"');   /* close of text */
            char fam[256]; float fs;
            if (q && sscanf(q + 1, " \"%255[^\"]\" %f", fam, &fs) == 2) {
                snprintf(P->co->font_family, sizeof P->co->font_family, "%s", fam);
                P->co->font_size = fs;
            }
        }
        else if (P->co && P->co->kind == HUI_VEC_IMAGE && sscanf(line, "image %u %d %d", &uid,&i0,&i1) == 3) {
            P->co->img_asset = uid; P->co->img_w = i0; P->co->img_h = i1;
        }
        else if (P->co && sscanf(line, "c %f %f", &f0,&f1) == 2) {
            P->cc = hui_vec_obj_add_contour(P->co);
            if (P->cc) P->cc->start = (hui_v2){f0,f1};
        }
        else if (P->cc && sscanf(line, "s %f %f %f %f %f %f", &f0,&f1,&f2,&f3,&f4,&f5) == 6)
            hui_glyph_add_seg(P->cc, (hui_v2){f0,f1}, (hui_v2){f2,f3}, (hui_v2){f4,f5});
        else if (P->co && strncmp(line,"strokestyle ",12)==0) {
            int a=0,j=0,cp=0; float ml=4;
            if (sscanf(line,"strokestyle %d %d %d %f",&a,&j,&cp,&ml)>=3){
                P->co->stroke_align=a; P->co->stroke_join=j; P->co->stroke_cap=cp; P->co->miter_limit=ml; }
        }
        else if (P->co && strncmp(line,"dash ",5)==0) {
            float off=0; int nd=0; const char *p=line+5;
            if (sscanf(p," %f %d",&off,&nd)==2){
                P->co->dash_offset=off; P->co->ndash=nd>8?8:(nd<0?0:nd);
                const char *q=p;
                for(int k=0;k<2;k++){ while(*q==' ')q++; while(*q&&*q!=' ')q++; }  /* skip offset+count */
                for(int i=0;i<P->co->ndash;i++){ while(*q==' ')q++; float v=0; if(sscanf(q,"%f",&v)==1) P->co->dash[i]=v;
                    while(*q&&*q!=' ')q++; }
            }
        }
        else if (strncmp(line, "endobj", 6) == 0) { P->co = NULL; P->cc = NULL; }
        /* anything else: unknown keyword — skipped for forward compat */
        }
}

/* close any groups left open by a malformed file / clip */
static void hui__vec_parse_finish(hui__vec_parser *P) {
    while (P->gsp > 0) {
        hui_vec_obj *done = P->gstack[--P->gsp];
        hui_vec_obj *slot = (P->gsp > 0)
            ? hui__vec_obj_push(&P->gstack[P->gsp-1]->objs, &P->gstack[P->gsp-1]->nobj, &P->gstack[P->gsp-1]->ocap, HUI_VEC_GROUP)
            : (P->cl ? hui__vec_obj_push(&P->cl->objs, &P->cl->nobj, &P->cl->ocap, HUI_VEC_GROUP) : NULL);
        if (slot) *slot = *done;
        free(done);
    }
}

/* read one line of any length (asset payloads run to megabytes); returns 0 at EOF */
static int hui__vec_getline(FILE *fp, char **buf, size_t *cap) {
    size_t len = 0;
    if (!*buf) { *cap = 1024; *buf = (char*)malloc(*cap); if (!*buf) return 0; }
    for (;;) {
        if (!fgets(*buf + len, (int)(*cap - len), fp)) return len > 0;
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len-1] == '\n') return 1;
        if (len + 1 >= *cap) {
            size_t nc = *cap * 2;
            char *nb = (char*)realloc(*buf, nc);
            if (!nb) return len > 0;
            *buf = nb; *cap = nc;
        } else return 1;   /* last line without newline */
    }
}

hui_vec_doc *hui_vec_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    char *line = NULL; size_t cap = 0;
    if (!hui__vec_getline(fp, &line, &cap) || strncmp(line, "barva ", 6) != 0) {
        free(line); fclose(fp); return NULL;
    }
    int version = atoi(line + 6);
    hui_vec_doc *d = hui_vec_doc_new(800, 600);
    if (!d) { free(line); fclose(fp); return NULL; }
    hui__vec_parser P; memset(&P, 0, sizeof P);
    P.d = d; P.version = version;
    while (hui__vec_getline(fp, &line, &cap)) hui__vec_parse_line(&P, line);
    hui__vec_parse_finish(&P);
    free(line);
    fclose(fp);
    if (version == 1) hui__vec_migrate_v1_groups(d);
    hui_vec_doc_assign_ids(d);
    return d;
}

/* ---- clipboard text form ---- */
static void hui__vec_clear_ids_rec(hui_vec_obj *o) {
    o->id = 0;
    for (int i = 0; i < o->nobj; i++) hui__vec_clear_ids_rec(&o->objs[i]);
}

static int hui__vec_id_in(const uint32_t *ids, int n, uint32_t id) {
    for (int i = 0; i < n; i++) if (ids[i] == id) return 1;
    return 0;
}

/* write selected objects in document order; a selected object nested in an
 * unselected group is written with its ancestors' transforms baked in */
static void hui__vec_clip_write_rec(FILE *fp, const hui_vec_obj *o, hui_vec_xform acc,
                                    const uint32_t *ids, int n) {
    if (hui__vec_id_in(ids, n, o->id)) {
        hui_vec_obj tmp = *o;                       /* shallow: payload pointers shared, read only */
        tmp.xf = hui_vec_xform_mul(acc, o->xf);
        hui__vec_write_obj(fp, &tmp);
        return;
    }
    if (o->kind == HUI_VEC_GROUP) {
        hui_vec_xform xf = hui_vec_xform_mul(acc, o->xf);
        for (int i = 0; i < o->nobj; i++) hui__vec_clip_write_rec(fp, &o->objs[i], xf, ids, n);
    }
}

char *hui_vec_clip_serialize(const hui_vec_doc *d, const uint32_t *ids, int n) {
    if (!d || !ids || n <= 0) return NULL;
    char *buf = NULL; size_t len = 0;
    FILE *fp = open_memstream(&buf, &len);
    if (!fp) return NULL;
    fprintf(fp, "barva-clip 1\n");
    /* assets referenced by the selection */
    uint32_t used[256]; int nused = 0;
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++) {
            const hui_vec_obj *o = &d->layers[li].objs[oi];
            /* mark from every selected node inside this entry */
            const hui_vec_obj *stack[512]; int sp = 0; stack[sp++] = o;
            while (sp > 0) {
                const hui_vec_obj *c = stack[--sp];
                if (hui__vec_id_in(ids, n, c->id)) hui__vec_mark_assets_rec(c, used, &nused, 256);
                else for (int k = 0; k < c->nobj && sp < 512; k++) stack[sp++] = &c->objs[k];
            }
        }
    for (int k = 0; k < nused; k++) {
        const hui_vec_asset *a = hui_vec_asset_find(d, used[k]);
        if (a) hui__vec_write_asset(fp, a);
    }
    for (int li = 0; li < d->nlayer; li++)
        for (int oi = 0; oi < d->layers[li].nobj; oi++)
            hui__vec_clip_write_rec(fp, &d->layers[li].objs[oi], hui_vec_xform_identity(), ids, n);
    fclose(fp);
    return buf;
}

int hui_vec_clip_parse(hui_vec_doc *d, hui_vec_layer *l, const char *text,
                       uint32_t *out_ids, int cap) {
    if (!d || !l || !text) return 0;
    hui__vec_parser P; memset(&P, 0, sizeof P);
    P.d = d; P.cl = l; P.version = 2; P.clip_mode = 1;
    int n0 = l->nobj;
    const char *p = text;
    char *line = NULL; size_t lcap = 0;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t L = nl ? (size_t)(nl - p) : strlen(p);
        if (L + 2 > lcap) {
            size_t nc = L + 2; if (nc < 1024) nc = 1024;
            char *nb = (char*)realloc(line, nc);
            if (!nb) break;
            line = nb; lcap = nc;
        }
        memcpy(line, p, L); line[L] = '\n'; line[L+1] = 0;
        p += L; if (*p == '\n') p++;
        if (strncmp(line, "barva", 5) == 0 && (line[5] == ' ' || line[5] == '-')) continue;   /* header */
        hui__vec_parse_line(&P, line);
    }
    hui__vec_parse_finish(&P);
    free(line);
    /* layer may have been reallocated by the pushes — index, don't hold pointers */
    int added = l->nobj - n0;
    for (int i = n0; i < l->nobj; i++) hui__vec_clear_ids_rec(&l->objs[i]);
    hui_vec_doc_assign_ids(d);
    if (out_ids) for (int i = n0; i < l->nobj && i - n0 < cap; i++) out_ids[i - n0] = l->objs[i].id;
    return added;
}

/* ---- SVG export ---- */

static int hui__vec_xf_is_identity(hui_vec_xform t) {
    return t.a==1 && t.b==0 && t.c==0 && t.d==1 && t.e==0 && t.f==0;
}

static void hui__vec_svg_path_d(FILE *fp, const hui_vec_obj *o) {
    fprintf(fp, "d=\"");
    for (int ci = 0; ci < o->ncontour; ci++) {
        const hui_contour *c = &o->contours[ci];
        fprintf(fp, "M%.6g %.6g", (double)c->start.x, (double)c->start.y);
        for (int si = 0; si < c->nseg; si++) {
            const hui_seg *s = &c->segs[si];
            fprintf(fp, "C%.6g %.6g %.6g %.6g %.6g %.6g",
                    (double)s->c0.x,(double)s->c0.y,(double)s->c1.x,
                    (double)s->c1.y,(double)s->a.x,(double)s->a.y);
        }
        fprintf(fp, "Z");
    }
    fprintf(fp, "\"");
}

static void hui__vec_svg_style(FILE *fp, const hui_vec_obj *o) {
    if (o->has_fill && o->has_grad)
        fprintf(fp, " fill=\"url(#g%p)\"", (const void*)o);
    else if (o->has_fill)
        fprintf(fp, " fill=\"#%02x%02x%02x\"", o->fill.r, o->fill.g, o->fill.b);
    else
        fprintf(fp, " fill=\"none\"");
    if (o->has_stroke && o->grad_stroke)
        fprintf(fp, " stroke=\"url(#gs%p)\" stroke-width=\"%.6g\"",
                (const void*)o, (double)o->stroke_w);
    else if (o->has_stroke)
        fprintf(fp, " stroke=\"#%02x%02x%02x\" stroke-width=\"%.6g\"",
                o->stroke.r, o->stroke.g, o->stroke.b, (double)o->stroke_w);
    if (o->has_stroke) {
        static const char *jn[]={"miter","round","bevel"}, *cp[]={"butt","round","square"};
        if (o->stroke_join) fprintf(fp, " stroke-linejoin=\"%s\"", jn[o->stroke_join%3]);
        if (o->stroke_cap)  fprintf(fp, " stroke-linecap=\"%s\"", cp[o->stroke_cap%3]);
        if (o->stroke_join==HUI_JOIN_MITER && o->miter_limit!=4.0f)
            fprintf(fp, " stroke-miterlimit=\"%.6g\"", (double)o->miter_limit);
        if (o->ndash>0){ fprintf(fp, " stroke-dasharray=\"");
            for(int i=0;i<o->ndash;i++) fprintf(fp, "%s%.6g", i?" ":"", (double)o->dash[i]);
            fprintf(fp, "\"");
            if(o->dash_offset!=0) fprintf(fp, " stroke-dashoffset=\"%.6g\"",(double)o->dash_offset); }
    }
    if (o->opacity < 1.0f)
        fprintf(fp, " opacity=\"%.6g\"", (double)o->opacity);
    if (!hui__vec_xf_is_identity(o->xf))
        fprintf(fp, " transform=\"matrix(%.6g %.6g %.6g %.6g %.6g %.6g)\"",
                (double)o->xf.a,(double)o->xf.b,(double)o->xf.c,
                (double)o->xf.d,(double)o->xf.e,(double)o->xf.f);
}

static int hui__vec_any_grad(const hui_vec_obj *o) {
    if (o->kind == HUI_VEC_GROUP) {
        for (int i = 0; i < o->nobj; i++) if (hui__vec_any_grad(&o->objs[i])) return 1;
        return 0;
    }
    return (o->has_grad && o->has_fill) || o->grad_stroke;
}
/* emit a linear or radial gradient def with id "<prefix><ptr>" */
static void hui__vec_emit_one_grad(FILE *fp, const hui_vec_obj *o, const char *prefix) {
    /* gradient endpoints carry only the object transform (not enclosing group
     * transforms) — acceptable for v1 export */
    hui_v2 g0 = hui_vec_xform_apply(o->xf, o->grad_p0);
    hui_v2 g1 = hui_vec_xform_apply(o->xf, o->grad_p1);
    if (o->grad_kind == HUI_GRAD_RADIAL) {
        double r = sqrt((double)((g1.x-g0.x)*(g1.x-g0.x)+(g1.y-g0.y)*(g1.y-g0.y)));
        fprintf(fp, "<radialGradient id=\"%s%p\" gradientUnits=\"userSpaceOnUse\" "
                    "cx=\"%.6g\" cy=\"%.6g\" r=\"%.6g\">"
                    "<stop offset=\"0\" stop-color=\"#%02x%02x%02x\"/>"
                    "<stop offset=\"1\" stop-color=\"#%02x%02x%02x\"/></radialGradient>\n",
                prefix,(const void*)o,(double)g0.x,(double)g0.y,r,
                o->grad_a.r,o->grad_a.g,o->grad_a.b, o->grad_b.r,o->grad_b.g,o->grad_b.b);
    } else {
        fprintf(fp, "<linearGradient id=\"%s%p\" gradientUnits=\"userSpaceOnUse\" "
                    "x1=\"%.6g\" y1=\"%.6g\" x2=\"%.6g\" y2=\"%.6g\">"
                    "<stop offset=\"0\" stop-color=\"#%02x%02x%02x\"/>"
                    "<stop offset=\"1\" stop-color=\"#%02x%02x%02x\"/></linearGradient>\n",
                prefix,(const void*)o,(double)g0.x,(double)g0.y,(double)g1.x,(double)g1.y,
                o->grad_a.r,o->grad_a.g,o->grad_a.b, o->grad_b.r,o->grad_b.g,o->grad_b.b);
    }
}
static void hui__vec_emit_grad_defs(FILE *fp, const hui_vec_obj *o) {
    if (o->kind == HUI_VEC_GROUP) {
        for (int i = 0; i < o->nobj; i++) hui__vec_emit_grad_defs(fp, &o->objs[i]);
        return;
    }
    if (o->has_grad && o->has_fill) hui__vec_emit_one_grad(fp, o, "g");
    if (o->grad_stroke)             hui__vec_emit_one_grad(fp, o, "gs");
}

static void hui__vec_svg_write_obj(FILE *fp, const hui_vec_obj *o) {
    if (o->kind == HUI_VEC_GROUP) {
        fprintf(fp, "<g");
        if (!hui__vec_xf_is_identity(o->xf))
            fprintf(fp, " transform=\"matrix(%.6g %.6g %.6g %.6g %.6g %.6g)\"",
                    (double)o->xf.a,(double)o->xf.b,(double)o->xf.c,
                    (double)o->xf.d,(double)o->xf.e,(double)o->xf.f);
        if (o->opacity < 1.0f) fprintf(fp, " opacity=\"%.6g\"", (double)o->opacity);
        fprintf(fp, ">\n");
        for (int i = 0; i < o->nobj; i++) hui__vec_svg_write_obj(fp, &o->objs[i]);
        fprintf(fp, "</g>\n");
        return;
    }
    if (o->kind == HUI_VEC_IMAGE) {
        const hui_vec_asset *a = hui_vec_asset_find(hui__vec_cur_doc, o->img_asset);
        unsigned char *png = NULL; int plen = 0;
        if (a && a->rgba && hui_vec_png_encode) png = hui_vec_png_encode(a->rgba, a->w, a->h, &plen);
        if (!png || plen <= 0) {
            free(png);
            fprintf(fp, "<!-- image \"%s\" %dx%d: %s -->\n", o->name, o->img_w, o->img_h,
                    a ? "no PNG encoder" : "asset missing");
            return;
        }
        char *b64 = hui__b64_encode(png, (size_t)plen);
        free(png);
        if (!b64) return;
        fprintf(fp, "<image x=\"0\" y=\"0\" width=\"%d\" height=\"%d\"", o->img_w, o->img_h);
        if (!hui__vec_xf_is_identity(o->xf))
            fprintf(fp, " transform=\"matrix(%.6g %.6g %.6g %.6g %.6g %.6g)\"",
                    (double)o->xf.a,(double)o->xf.b,(double)o->xf.c,
                    (double)o->xf.d,(double)o->xf.e,(double)o->xf.f);
        if (o->opacity < 1.0f) fprintf(fp, " opacity=\"%.6g\"", (double)o->opacity);
        fprintf(fp, " href=\"data:image/png;base64,%s\"/>\n", b64);
        free(b64);
        return;
    }
    if (o->kind == HUI_VEC_TEXT) {
        hui_glyph_font *f = hui_vec_font_lookup ? hui_vec_font_lookup(o->font_family) : 0;
        if (!f) { fprintf(fp, "<!-- text \"%s\": font unavailable -->\n", o->text); return; }
        float s = o->font_size / (float)f->units_per_em;
        hui_v2 org = hui_vec_xform_apply(o->xf, (hui_v2){0,0});
        fprintf(fp, "<path d=\"");
        float pen = 0;
        const char *p = o->text;
        while (*p) {
            uint32_t cp = hui_utf8_next(&p);
            hui_glyph *g = hui_glyph_find(f, cp);
            if (!g) continue;
            for (int ci = 0; ci < g->ncontour; ci++) {
                const hui_contour *c = &g->contours[ci];
                fprintf(fp, "M%.6g %.6g",
                        (double)(org.x + pen + c->start.x*s),
                        (double)(org.y - c->start.y*s));
                for (int si = 0; si < c->nseg; si++) {
                    const hui_seg *sg = &c->segs[si];
                    fprintf(fp, "C%.6g %.6g %.6g %.6g %.6g %.6g",
                        (double)(org.x+pen+sg->c0.x*s),(double)(org.y-sg->c0.y*s),
                        (double)(org.x+pen+sg->c1.x*s),(double)(org.y-sg->c1.y*s),
                        (double)(org.x+pen+sg->a.x*s), (double)(org.y-sg->a.y*s));
                }
                fprintf(fp, "Z");
            }
            pen += (float)g->advance * s;
        }
        fprintf(fp, "\"");
        hui__vec_svg_style(fp, o);
        fprintf(fp, "/>\n<!-- text source: \"%s\" font \"%s\" size %.6g -->\n",
                o->text, o->font_family, (double)o->font_size);
        return;
    }
    fprintf(fp, "<path ");
    hui__vec_svg_path_d(fp, o);
    hui__vec_svg_style(fp, o);
    fprintf(fp, "/>\n");
}

int hui_vec_export_svg(const hui_vec_doc *d, const char *path) {
    hui__vec_cur_doc = d;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                "width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
            d->w, d->h, d->w, d->h);
    fprintf(fp, "<rect width=\"%d\" height=\"%d\" fill=\"#%02x%02x%02x\"/>\n",
            d->w, d->h, d->bg.r, d->bg.g, d->bg.b);
    /* gradient defs: one linearGradient per gradient-filled object, in doc
     * space (endpoints already carry the object transform) */
    {
        int any = 0;
        for (int li = 0; li < d->nlayer && !any; li++)
            for (int oi = 0; oi < d->layers[li].nobj; oi++)
                if (hui__vec_any_grad(&d->layers[li].objs[oi])) { any = 1; break; }
        if (any) {
            fprintf(fp, "<defs>\n");
            for (int li = 0; li < d->nlayer; li++)
                for (int oi = 0; oi < d->layers[li].nobj; oi++)
                    hui__vec_emit_grad_defs(fp, &d->layers[li].objs[oi]);
            fprintf(fp, "</defs>\n");
        }
    }
    for (int li = 0; li < d->nlayer; li++) {
        const hui_vec_layer *l = &d->layers[li];
        if (!l->visible) continue;
        fprintf(fp, "<g id=\"%s\">\n", l->name);
        for (int oi = 0; oi < l->nobj; oi++) hui__vec_svg_write_obj(fp, &l->objs[oi]);
        fprintf(fp, "</g>\n");
    }
    fprintf(fp, "</svg>\n");
    fclose(fp);
    return 0;
}

/* ---- SVG import ---- */

typedef struct {
    hui_vec_xform xf;
    int has_fill; hui_color fill;
    int has_stroke; hui_color stroke; float stroke_w;
    float opacity;
} hui__svg_style;

static int hui__svg_parse_color(const char *s, hui_color *out) {
    while (*s==' ') s++;
    static const struct { const char *n; hui_color c; } named[] = {
        {"black",{0,0,0,255}},{"white",{255,255,255,255}},{"red",{255,0,0,255}},
        {"green",{0,128,0,255}},{"blue",{0,0,255,255}},{"yellow",{255,255,0,255}},
        {"cyan",{0,255,255,255}},{"magenta",{255,0,255,255}}};
    if (*s == '#') {
        unsigned v = 0; int n = 0;
        for (s++; ((*s>='0'&&*s<='9')||(*s>='a'&&*s<='f')||(*s>='A'&&*s<='F')) && n < 6; s++, n++)
            v = v*16 + (unsigned)(*s<='9' ? *s-'0' : (*s|32)-'a'+10);
        if (n == 3) *out = (hui_color){(uint8_t)(((v>>8)&15)*17),
                                        (uint8_t)(((v>>4)&15)*17),
                                        (uint8_t)((v&15)*17), 255};
        else if (n == 6) *out = (hui_color){(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v,255};
        else return 0;
        return 1;
    }
    unsigned r,g,b;
    if (sscanf(s, "rgb(%u,%u,%u)", &r,&g,&b) == 3 ||
        sscanf(s, "rgb(%u, %u, %u)", &r,&g,&b) == 3) {
        *out = (hui_color){(uint8_t)r,(uint8_t)g,(uint8_t)b,255}; return 1;
    }
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (strncmp(s, named[i].n, strlen(named[i].n)) == 0) { *out = named[i].c; return 1; }
    return 0;   /* includes "none" and unknown */
}

static hui_vec_xform hui__svg_parse_transform(const char *s) {
    hui_vec_xform t = hui_vec_xform_identity();
    while (*s) {
        float v[6]; int n = 0;
        if (strncmp(s, "matrix(", 7) == 0) {
            n = sscanf(s+7, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f%*[ ,]%f%*[ ,]%f",
                       &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]);
            if (n == 6) t = hui_vec_xform_mul(t, (hui_vec_xform){v[0],v[1],v[2],v[3],v[4],v[5]});
        } else if (strncmp(s, "translate(", 10) == 0) {
            v[1] = 0;
            n = sscanf(s+10, "%f%*[ ,]%f", &v[0], &v[1]);
            if (n >= 1) t = hui_vec_xform_mul(t, (hui_vec_xform){1,0,0,1,v[0],v[1]});
        } else if (strncmp(s, "scale(", 6) == 0) {
            n = sscanf(s+6, "%f%*[ ,]%f", &v[0], &v[1]);
            if (n == 1) v[1] = v[0];
            if (n >= 1) t = hui_vec_xform_mul(t, (hui_vec_xform){v[0],0,0,v[1],0,0});
        } else if (strncmp(s, "rotate(", 7) == 0) {
            v[1] = v[2] = 0;
            n = sscanf(s+7, "%f%*[ ,]%f%*[ ,]%f", &v[0], &v[1], &v[2]);
            if (n >= 1) {
                float rad = v[0] * 3.14159265358979f / 180.0f;
                float ca = cosf(rad), sa = sinf(rad);
                hui_vec_xform R = {ca, sa, -sa, ca, 0, 0};
                if (n == 3) {   /* rotate about (cx,cy) */
                    t = hui_vec_xform_mul(t, (hui_vec_xform){1,0,0,1,v[1],v[2]});
                    t = hui_vec_xform_mul(t, R);
                    t = hui_vec_xform_mul(t, (hui_vec_xform){1,0,0,1,-v[1],-v[2]});
                } else t = hui_vec_xform_mul(t, R);
            }
        }
        const char *close = strchr(s, ')');
        if (!close) break;
        s = close + 1;
        while (*s == ' ' || *s == ',') s++;
    }
    return t;
}

/* Find attr="value" in one tag's source; value copied to out. */
static const char *hui__svg_attr(const char *tag, const char *name, char *out, int outsz) {
    size_t nl = strlen(name);
    for (const char *p = tag; (p = strstr(p, name)) != NULL; p += nl) {
        if (p > tag && (p[-1]==' '||p[-1]=='\t'||p[-1]=='\n')
            && p[nl] == '=' && (p[nl+1]=='"' || p[nl+1]=='\'')) {
            char q = p[nl+1];
            const char *v = p + nl + 2, *e = strchr(v, q);
            if (!e) return NULL;
            int len = (int)(e - v); if (len > outsz-1) len = outsz-1;
            memcpy(out, v, (size_t)len); out[len] = 0;
            return out;
        }
    }
    return NULL;
}

/* style="fill:#123;stroke:none" — find one declaration's value */
static const char *hui__svg_style_decl(const char *style, const char *key,
                                       char *out, int outsz) {
    size_t kl = strlen(key);
    for (const char *p = style; (p = strstr(p, key)) != NULL; p += kl) {
        const char *q = p + kl;
        while (*q==' ') q++;
        if (*q != ':') continue;
        q++;
        while (*q==' ') q++;
        int len = 0;
        while (q[len] && q[len] != ';' && len < outsz-1) len++;
        memcpy(out, q, (size_t)len); out[len] = 0;
        return out;
    }
    return NULL;
}

/* Apply fill/stroke/opacity/stroke-width/transform from a tag onto st. */
static void hui__svg_apply_style(const char *tag, hui__svg_style *st) {
    char v[256], sv[256];
    const char *fill = hui__svg_attr(tag, "fill", v, sizeof v);
    char stylebuf[512];
    const char *style = hui__svg_attr(tag, "style", stylebuf, sizeof stylebuf);
    if (!fill && style) fill = hui__svg_style_decl(stylebuf, "fill", v, sizeof v);
    if (fill) {
        hui_color c;
        if (strncmp(fill, "none", 4) == 0) st->has_fill = 0;
        else if (hui__svg_parse_color(fill, &c)) { st->has_fill = 1; st->fill = c; }
    }
    const char *stroke = hui__svg_attr(tag, "stroke", sv, sizeof sv);
    if (!stroke && style) stroke = hui__svg_style_decl(stylebuf, "stroke", sv, sizeof sv);
    if (stroke) {
        hui_color c;
        if (strncmp(stroke, "none", 4) == 0) st->has_stroke = 0;
        else if (hui__svg_parse_color(stroke, &c)) { st->has_stroke = 1; st->stroke = c; }
    }
    if (hui__svg_attr(tag, "stroke-width", v, sizeof v)) st->stroke_w = (float)atof(v);
    else if (style && hui__svg_style_decl(stylebuf, "stroke-width", v, sizeof v))
        st->stroke_w = (float)atof(v);
    if (hui__svg_attr(tag, "opacity", v, sizeof v))
        st->opacity *= (float)atof(v);
    if (hui__svg_attr(tag, "transform", v, sizeof v))
        st->xf = hui_vec_xform_mul(st->xf, hui__svg_parse_transform(v));
}

static void hui__svg_obj_style(hui_vec_obj *o, const hui__svg_style *st) {
    o->has_fill = st->has_fill; o->fill = st->fill;
    o->has_stroke = st->has_stroke; o->stroke = st->stroke; o->stroke_w = st->stroke_w;
    o->opacity = st->opacity;
    o->xf = st->xf;
    /* nested-<g> grouping becomes real group objects in Task 5 */
}

#define HUI__SVG_KAPPA 0.5522847498f
static void hui__svg_add_ellipse(hui_vec_obj *o, float cx, float cy, float rx, float ry) {
    hui_contour *c = hui_vec_obj_add_contour(o);
    if (!c) return;
    float kx = rx * HUI__SVG_KAPPA, ky = ry * HUI__SVG_KAPPA;
    c->start = (hui_v2){cx + rx, cy};
    hui_glyph_add_seg(c, (hui_v2){cx+rx, cy+ky}, (hui_v2){cx+kx, cy+ry}, (hui_v2){cx, cy+ry});
    hui_glyph_add_seg(c, (hui_v2){cx-kx, cy+ry}, (hui_v2){cx-rx, cy+ky}, (hui_v2){cx-rx, cy});
    hui_glyph_add_seg(c, (hui_v2){cx-rx, cy-ky}, (hui_v2){cx-kx, cy-ry}, (hui_v2){cx, cy-ry});
    hui_glyph_add_seg(c, (hui_v2){cx+kx, cy-ry}, (hui_v2){cx+rx, cy-ky}, (hui_v2){cx+rx, cy});
}

static void hui__svg_add_line_seg(hui_contour *c, hui_v2 to) {
    hui_v2 prev = c->nseg ? c->segs[c->nseg-1].a : c->start;
    hui_v2 c0 = { prev.x + (to.x-prev.x)/3.0f, prev.y + (to.y-prev.y)/3.0f };
    hui_v2 c1 = { prev.x + 2*(to.x-prev.x)/3.0f, prev.y + 2*(to.y-prev.y)/3.0f };
    hui_glyph_add_seg(c, c0, c1, to);
}

static const char *hui__svg_num(const char *s, float *out) {
    while (*s==' '||*s==','||*s=='\n'||*s=='\t') s++;
    char *end;
    float v = strtof(s, &end);
    if (end == s) return NULL;
    *out = v;
    return end;
}

static void hui__svg_arc_to_cubics(hui_contour *c, hui_v2 p0, float rx, float ry,
                                   float xrot_deg, int large, int sweep, hui_v2 p1) {
    /* SVG spec F.6.5: endpoint -> center parameterization */
    if (rx == 0 || ry == 0) { hui__svg_add_line_seg(c, p1); return; }
    if (rx < 0) rx = -rx;  if (ry < 0) ry = -ry;
    float phi = xrot_deg * 3.14159265358979f / 180.0f;
    float cphi = cosf(phi), sphi = sinf(phi);
    float dx2 = (p0.x - p1.x) * 0.5f, dy2 = (p0.y - p1.y) * 0.5f;
    float x1p =  cphi*dx2 + sphi*dy2;
    float y1p = -sphi*dx2 + cphi*dy2;
    float lam = (x1p*x1p)/(rx*rx) + (y1p*y1p)/(ry*ry);
    if (lam > 1) { float sl = sqrtf(lam); rx *= sl; ry *= sl; }
    float num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p;
    float den = rx*rx*y1p*y1p + ry*ry*x1p*x1p;
    float co = den > 0 ? sqrtf(num > 0 ? num/den : 0) : 0;
    if (large == sweep) co = -co;
    float cxp =  co * rx * y1p / ry;
    float cyp = -co * ry * x1p / rx;
    float cx = cphi*cxp - sphi*cyp + (p0.x + p1.x)*0.5f;
    float cy = sphi*cxp + cphi*cyp + (p0.y + p1.y)*0.5f;
    float th0 = atan2f((y1p - cyp)/ry, (x1p - cxp)/rx);
    float th1 = atan2f((-y1p - cyp)/ry, (-x1p - cxp)/rx);
    float dth = th1 - th0;
    if (!sweep && dth > 0) dth -= 2.0f*3.14159265358979f;
    if ( sweep && dth < 0) dth += 2.0f*3.14159265358979f;
    int nseg = (int)ceilf(fabsf(dth) / (3.14159265358979f * 0.5f));
    if (nseg < 1) nseg = 1;
    float step = dth / (float)nseg;
    float k = 4.0f/3.0f * tanf(step * 0.25f);
    for (int i = 0; i < nseg; i++) {
        float a0 = th0 + step * (float)i, a1 = a0 + step;
        hui_v2 e0 = { cx + rx*(cphi*cosf(a0)) - ry*(sphi*sinf(a0)),
                      cy + rx*(sphi*cosf(a0)) + ry*(cphi*sinf(a0)) };
        hui_v2 e1 = { cx + rx*(cphi*cosf(a1)) - ry*(sphi*sinf(a1)),
                      cy + rx*(sphi*cosf(a1)) + ry*(cphi*sinf(a1)) };
        hui_v2 d0 = { -rx*cphi*sinf(a0) - ry*sphi*cosf(a0),      /* derivatives */
                      -rx*sphi*sinf(a0) + ry*cphi*cosf(a0) };
        hui_v2 d1 = { -rx*cphi*sinf(a1) - ry*sphi*cosf(a1),
                      -rx*sphi*sinf(a1) + ry*cphi*cosf(a1) };
        hui_glyph_add_seg(c, (hui_v2){e0.x + k*d0.x, e0.y + k*d0.y},
                             (hui_v2){e1.x - k*d1.x, e1.y - k*d1.y}, e1);
    }
}

static void hui__svg_parse_path(hui_vec_obj *o, const char *s) {
    hui_contour *c = NULL;
    hui_v2 cur = {0,0}, start = {0,0};
    hui_v2 prev_c1 = {0,0}, prev_q = {0,0};
    int have_prev_c = 0, have_prev_q = 0;
    char cmd = 0;
    while (*s) {
        while (*s==' '||*s==','||*s=='\n'||*s=='\t') s++;
        if (!*s) break;
        if ((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')) cmd = *s++;
        int rel = (cmd >= 'a');
        char C = rel ? (char)(cmd - 32) : cmd;
        float v[7]; const char *ns;
        if (C == 'Z') {
            if (c) cur = start;
            c = NULL;
            have_prev_c = have_prev_q = 0;
            continue;
        }
        if (C == 'M') {
            if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1]))) break;
            s = ns;
            cur.x = rel ? cur.x + v[0] : v[0];
            cur.y = rel ? cur.y + v[1] : v[1];
            start = cur;
            c = hui_vec_obj_add_contour(o);
            if (c) c->start = cur;
            cmd = rel ? 'l' : 'L';    /* implicit repeats of M are lineto */
            have_prev_c = have_prev_q = 0;
            continue;
        }
        if (!c) { c = hui_vec_obj_add_contour(o); if (c) c->start = cur; }
        if (!c) break;
        switch (C) {
        case 'L':
            if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1]))) return;
            s = ns;
            cur.x = rel ? cur.x + v[0] : v[0];
            cur.y = rel ? cur.y + v[1] : v[1];
            hui__svg_add_line_seg(c, cur);
            have_prev_c = have_prev_q = 0;
            break;
        case 'H':
            if (!(ns = hui__svg_num(s, &v[0]))) return;
            s = ns;
            cur.x = rel ? cur.x + v[0] : v[0];
            hui__svg_add_line_seg(c, cur);
            have_prev_c = have_prev_q = 0;
            break;
        case 'V':
            if (!(ns = hui__svg_num(s, &v[0]))) return;
            s = ns;
            cur.y = rel ? cur.y + v[0] : v[0];
            hui__svg_add_line_seg(c, cur);
            have_prev_c = have_prev_q = 0;
            break;
        case 'C': case 'S': {
            hui_v2 c0, c1, e;
            if (C == 'C') {
                if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1])) ||
                    !(ns = hui__svg_num(ns, &v[2])) || !(ns = hui__svg_num(ns, &v[3])) ||
                    !(ns = hui__svg_num(ns, &v[4])) || !(ns = hui__svg_num(ns, &v[5]))) return;
                s = ns;
                c0 = (hui_v2){ rel?cur.x+v[0]:v[0], rel?cur.y+v[1]:v[1] };
                c1 = (hui_v2){ rel?cur.x+v[2]:v[2], rel?cur.y+v[3]:v[3] };
                e  = (hui_v2){ rel?cur.x+v[4]:v[4], rel?cur.y+v[5]:v[5] };
            } else {
                if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1])) ||
                    !(ns = hui__svg_num(ns, &v[2])) || !(ns = hui__svg_num(ns, &v[3]))) return;
                s = ns;
                c0 = have_prev_c ? (hui_v2){2*cur.x - prev_c1.x, 2*cur.y - prev_c1.y} : cur;
                c1 = (hui_v2){ rel?cur.x+v[0]:v[0], rel?cur.y+v[1]:v[1] };
                e  = (hui_v2){ rel?cur.x+v[2]:v[2], rel?cur.y+v[3]:v[3] };
            }
            hui_glyph_add_seg(c, c0, c1, e);
            prev_c1 = c1; have_prev_c = 1; have_prev_q = 0;
            cur = e;
            break;
        }
        case 'Q': case 'T': {
            hui_v2 q, e;
            if (C == 'Q') {
                if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1])) ||
                    !(ns = hui__svg_num(ns, &v[2])) || !(ns = hui__svg_num(ns, &v[3]))) return;
                s = ns;
                q = (hui_v2){ rel?cur.x+v[0]:v[0], rel?cur.y+v[1]:v[1] };
                e = (hui_v2){ rel?cur.x+v[2]:v[2], rel?cur.y+v[3]:v[3] };
            } else {
                if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1]))) return;
                s = ns;
                q = have_prev_q ? (hui_v2){2*cur.x - prev_q.x, 2*cur.y - prev_q.y} : cur;
                e = (hui_v2){ rel?cur.x+v[0]:v[0], rel?cur.y+v[1]:v[1] };
            }
            hui_v2 c0 = { cur.x + 2.0f/3.0f*(q.x - cur.x), cur.y + 2.0f/3.0f*(q.y - cur.y) };
            hui_v2 c1 = { e.x   + 2.0f/3.0f*(q.x - e.x),   e.y   + 2.0f/3.0f*(q.y - e.y) };
            hui_glyph_add_seg(c, c0, c1, e);
            prev_q = q; have_prev_q = 1; have_prev_c = 0;
            cur = e;
            break;
        }
        case 'A': {
            if (!(ns = hui__svg_num(s, &v[0])) || !(ns = hui__svg_num(ns, &v[1])) ||
                !(ns = hui__svg_num(ns, &v[2])) || !(ns = hui__svg_num(ns, &v[3])) ||
                !(ns = hui__svg_num(ns, &v[4])) || !(ns = hui__svg_num(ns, &v[5])) ||
                !(ns = hui__svg_num(ns, &v[6]))) return;
            s = ns;
            hui_v2 e = { rel?cur.x+v[5]:v[5], rel?cur.y+v[6]:v[6] };
            hui__svg_arc_to_cubics(c, cur, v[0], v[1], v[2],
                                   v[3] != 0.0f, v[4] != 0.0f, e);
            cur = e;
            have_prev_c = have_prev_q = 0;
            break;
        }
        default: return;   /* unknown command: stop parsing this path */
        }
    }
}

hui_vec_doc *hui_vec_import_svg(const char *path, int *skipped) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 16*1024*1024) { fclose(fp); return NULL; }
    char *src = (char*)malloc((size_t)sz + 1);
    if (!src) { fclose(fp); return NULL; }
    if (fread(src, 1, (size_t)sz, fp) != (size_t)sz) { free(src); fclose(fp); return NULL; }
    src[sz] = 0; fclose(fp);

    if (!strstr(src, "<svg")) { free(src); return NULL; }
    hui_vec_doc *d = hui_vec_doc_new(800, 600);
    if (!d) { free(src); return NULL; }
    int nskip = 0;
    enum { HUI__SVG_DEPTH = 16 };
    hui__svg_style stack[HUI__SVG_DEPTH];
    stack[0] = (hui__svg_style){ hui_vec_xform_identity(),
                                 1, {0,0,0,255}, 0, {0,0,0,255}, 1.0f, 1.0f };
    int sp = 0;
    hui_vec_layer *cur_layer = NULL;
    hui_vec_obj *cur_group[HUI__SVG_DEPTH]; int cgsp = 0;   /* open group objects */
    int gdepth = 0;   /* <g> nesting depth; depth 1 = layer, deeper = group */

    /* gradient defs table for url(#id) fill/stroke resolution */
    enum { HV_GDEF_MAX = 64 };
    struct hv_gdef { char id[40]; int kind; hui_v2 p0,p1; hui_color a,b; int ns; };
    static struct hv_gdef gdefs[HV_GDEF_MAX];
    int ngdef = 0, cur_gdef = -1;

    for (char *p = src; (p = strchr(p, '<')) != NULL; ) {
        if (p[1] == '?' || p[1] == '!') {                    /* <?xml, <!--, <!DOCTYPE */
            char *e = strstr(p, p[1]=='!' && p[2]=='-' ? "-->" : ">");
            p = e ? e + (p[1]=='!' && p[2]=='-' ? 3 : 1) : p + 1;
            continue;
        }
        char *end = strchr(p, '>');
        if (!end) break;
        int closing = (p[1] == '/');
        int selfclose = (end[-1] == '/');
        char tagname[32];
        { int i = 0; const char *q = p + (closing ? 2 : 1);
          while (*q && *q!=' ' && *q!='>' && *q!='/' && *q!='\n' && i < 31) tagname[i++] = *q++;
          tagname[i] = 0; }
        char tagsrc[4096];
        { long len = end - p; if (len > 4095) len = 4095;
          memcpy(tagsrc, p, (size_t)len); tagsrc[len] = 0; }

        if (!closing && (strcmp(tagname,"linearGradient")==0 || strcmp(tagname,"radialGradient")==0)) {
            if (ngdef < HV_GDEF_MAX) {
                struct hv_gdef *gd = &gdefs[ngdef];
                memset(gd,0,sizeof *gd);
                char v[64];
                if (hui__svg_attr(tagsrc,"id",v,sizeof v)) snprintf(gd->id,sizeof gd->id,"%s",v);
                if (strcmp(tagname,"radialGradient")==0){
                    gd->kind=HUI_GRAD_RADIAL; float cx=0,cy=0,rr=1;
                    if(hui__svg_attr(tagsrc,"cx",v,sizeof v))cx=(float)atof(v);
                    if(hui__svg_attr(tagsrc,"cy",v,sizeof v))cy=(float)atof(v);
                    if(hui__svg_attr(tagsrc,"r",v,sizeof v))rr=(float)atof(v);
                    gd->p0=(hui_v2){cx,cy}; gd->p1=(hui_v2){cx+rr,cy};
                } else {
                    gd->kind=HUI_GRAD_LINEAR; float x1=0,y1=0,x2=0,y2=0;
                    if(hui__svg_attr(tagsrc,"x1",v,sizeof v))x1=(float)atof(v);
                    if(hui__svg_attr(tagsrc,"y1",v,sizeof v))y1=(float)atof(v);
                    if(hui__svg_attr(tagsrc,"x2",v,sizeof v))x2=(float)atof(v);
                    if(hui__svg_attr(tagsrc,"y2",v,sizeof v))y2=(float)atof(v);
                    gd->p0=(hui_v2){x1,y1}; gd->p1=(hui_v2){x2,y2};
                }
                cur_gdef = ngdef; ngdef++;
                if (selfclose) cur_gdef=-1;
            }
        }
        else if (closing && (strcmp(tagname,"linearGradient")==0 || strcmp(tagname,"radialGradient")==0)) {
            cur_gdef=-1;
        }
        else if (!closing && strcmp(tagname,"stop")==0 && cur_gdef>=0) {
            struct hv_gdef *gd=&gdefs[cur_gdef]; char v[64]; hui_color col;
            int got=0;
            if (hui__svg_attr(tagsrc,"stop-color",v,sizeof v) && hui__svg_parse_color(v,&col)) got=1;
            else if (hui__svg_attr(tagsrc,"style",v,sizeof v)) {
                char *sc=strstr(v,"stop-color:");
                if(sc && hui__svg_parse_color(sc+11,&col)) got=1;
            }
            if(got){ if(gd->ns==0) gd->a=col; else gd->b=col; gd->ns++; }
        }
        else if (!closing && strcmp(tagname, "svg") == 0) {
            char v[64];
            if (hui__svg_attr(tagsrc, "width", v, sizeof v))  d->w = atoi(v);
            if (hui__svg_attr(tagsrc, "height", v, sizeof v)) d->h = atoi(v);
        } else if (!closing && strcmp(tagname, "g") == 0) {
            gdepth++;
            if (sp < HUI__SVG_DEPTH-1) { stack[sp+1] = stack[sp]; sp++; }
            hui__svg_apply_style(tagsrc, &stack[sp]);
            char v[64];
            if (gdepth == 1) {
                cur_layer = hui_vec_layer_add(d,
                    hui__svg_attr(tagsrc, "id", v, sizeof v) ? v : "imported");
                cgsp = 0;
            } else if (cur_layer && cgsp < HUI__SVG_DEPTH) {
                /* nested <g> -> real group object; leaf shapes carry the full
                 * accumulated transform, so the group node's own xf is identity */
                hui_vec_obj *ng = (cgsp > 0)
                    ? hui_vec_group_child_add(cur_group[cgsp-1], HUI_VEC_GROUP)
                    : hui_vec_group_add(cur_layer);
                if (ng) {
                    ng->xf = hui_vec_xform_identity();
                    ng->opacity = 1.0f;
                    snprintf(ng->name, sizeof ng->name, "%s",
                             hui__svg_attr(tagsrc,"id",v,sizeof v) ? v : "group");
                    cur_group[cgsp++] = ng;
                }
            }
            if (selfclose) { gdepth--; if (sp > 0) sp--; if (cgsp>0 && gdepth>=1) cgsp--; }
        } else if (closing && strcmp(tagname, "g") == 0) {
            gdepth--; if (sp > 0) sp--;
            if (gdepth >= 1 && cgsp > 0) cgsp--;
            if (gdepth == 0) { cur_layer = NULL; cgsp = 0; }
        } else if (!closing && (
                   strcmp(tagname,"rect")==0 || strcmp(tagname,"circle")==0 ||
                   strcmp(tagname,"ellipse")==0 || strcmp(tagname,"line")==0 ||
                   strcmp(tagname,"polyline")==0 || strcmp(tagname,"polygon")==0 ||
                   strcmp(tagname,"path")==0)) {
            /* full-document rect outside any <g> = document background (what
             * our exporter writes) — absorb into d->bg, not an object */
            if (gdepth == 0 && strcmp(tagname, "rect") == 0) {
                char bw[64], bh[64], bf[64];
                if (hui__svg_attr(tagsrc,"width",bw,sizeof bw) &&
                    hui__svg_attr(tagsrc,"height",bh,sizeof bh) &&
                    atoi(bw) == d->w && atoi(bh) == d->h &&
                    !hui__svg_attr(tagsrc,"x",bf,sizeof bf) &&
                    !hui__svg_attr(tagsrc,"y",bf,sizeof bf)) {
                    hui_color bg;
                    if (hui__svg_attr(tagsrc,"fill",bf,sizeof bf) &&
                        hui__svg_parse_color(bf, &bg)) d->bg = bg;
                    p = end + 1;
                    continue;
                }
            }
            if (!cur_layer) cur_layer = hui_vec_layer_add(d, "imported");
            hui__svg_style st = stack[sp];
            hui__svg_apply_style(tagsrc, &st);
            hui_vec_obj *o = cgsp > 0
                ? hui_vec_group_child_add(cur_group[cgsp-1], HUI_VEC_PATH)
                : (cur_layer ? hui_vec_obj_add(cur_layer, HUI_VEC_PATH) : NULL);
            if (o) {
                hui__svg_obj_style(o, &st);
                snprintf(o->name, sizeof o->name, "%s", tagname);
                { char sv[64];
                  if (hui__svg_attr(tagsrc,"stroke-linejoin",sv,sizeof sv))
                      o->stroke_join = !strcmp(sv,"round")?HUI_JOIN_ROUND: !strcmp(sv,"bevel")?HUI_JOIN_BEVEL:HUI_JOIN_MITER;
                  if (hui__svg_attr(tagsrc,"stroke-linecap",sv,sizeof sv))
                      o->stroke_cap = !strcmp(sv,"round")?HUI_CAP_ROUND: !strcmp(sv,"square")?HUI_CAP_SQUARE:HUI_CAP_BUTT;
                  if (hui__svg_attr(tagsrc,"stroke-miterlimit",sv,sizeof sv)) o->miter_limit=(float)atof(sv);
                  char dv[256];
                  if (hui__svg_attr(tagsrc,"stroke-dasharray",dv,sizeof dv) && strcmp(dv,"none")){
                      int nd=0; const char *q=dv;
                      while(*q && nd<8){ while(*q==' '||*q==',')q++; if(!*q)break; o->dash[nd++]=(float)atof(q);
                          while(*q&&*q!=' '&&*q!=',')q++; }
                      o->ndash=nd; }
                  if (hui__svg_attr(tagsrc,"stroke-dashoffset",dv,sizeof dv)) o->dash_offset=(float)atof(dv);
                }
                /* resolve url(#id) fill/stroke gradient references; gradient
                 * endpoints are userspace (doc) — map into local via inv(o->xf) */
                { char fv[80];
                  hui_vec_xform inv = hui__vec_xform_inverse(o->xf);
                  if (hui__svg_attr(tagsrc,"fill",fv,sizeof fv) && !strncmp(fv,"url(#",5)){
                      char gid[40]; int k=0; const char *q=fv+5; while(*q&&*q!=')'&&k<39)gid[k++]=*q++; gid[k]=0;
                      for(int gi=0;gi<ngdef;gi++) if(!strcmp(gdefs[gi].id,gid)){
                          o->has_fill=1; o->has_grad=1; o->grad_kind=gdefs[gi].kind;
                          o->grad_p0=hui_vec_xform_apply(inv,gdefs[gi].p0);
                          o->grad_p1=hui_vec_xform_apply(inv,gdefs[gi].p1);
                          o->grad_a=gdefs[gi].a; o->grad_b=gdefs[gi].b; break; }
                  }
                  if (hui__svg_attr(tagsrc,"stroke",fv,sizeof fv) && !strncmp(fv,"url(#",5)){
                      char gid[40]; int k=0; const char *q=fv+5; while(*q&&*q!=')'&&k<39)gid[k++]=*q++; gid[k]=0;
                      for(int gi=0;gi<ngdef;gi++) if(!strcmp(gdefs[gi].id,gid)){
                          o->has_stroke=1; o->grad_stroke=1; o->grad_kind=gdefs[gi].kind;
                          o->grad_p0=hui_vec_xform_apply(inv,gdefs[gi].p0);
                          o->grad_p1=hui_vec_xform_apply(inv,gdefs[gi].p1);
                          o->grad_a=gdefs[gi].a; o->grad_b=gdefs[gi].b; break; }
                  }
                }
                static char v[8192];
                if (strcmp(tagname, "rect") == 0) {
                    float x=0,y=0,w=0,h=0;
                    if (hui__svg_attr(tagsrc,"x",v,sizeof v)) x=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"y",v,sizeof v)) y=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"width",v,sizeof v)) w=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"height",v,sizeof v)) h=(float)atof(v);
                    hui_contour *c = hui_vec_obj_add_contour(o);
                    if (c) {
                        c->start = (hui_v2){x,y};
                        hui__svg_add_line_seg(c, (hui_v2){x+w,y});
                        hui__svg_add_line_seg(c, (hui_v2){x+w,y+h});
                        hui__svg_add_line_seg(c, (hui_v2){x,y+h});
                        hui__svg_add_line_seg(c, (hui_v2){x,y});
                    }
                } else if (strcmp(tagname, "circle") == 0) {
                    float cx=0,cy=0,r=0;
                    if (hui__svg_attr(tagsrc,"cx",v,sizeof v)) cx=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"cy",v,sizeof v)) cy=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"r",v,sizeof v))  r=(float)atof(v);
                    hui__svg_add_ellipse(o, cx, cy, r, r);
                } else if (strcmp(tagname, "ellipse") == 0) {
                    float cx=0,cy=0,rx=0,ry=0;
                    if (hui__svg_attr(tagsrc,"cx",v,sizeof v)) cx=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"cy",v,sizeof v)) cy=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"rx",v,sizeof v)) rx=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"ry",v,sizeof v)) ry=(float)atof(v);
                    hui__svg_add_ellipse(o, cx, cy, rx, ry);
                } else if (strcmp(tagname, "line") == 0) {
                    float x1=0,y1=0,x2=0,y2=0;
                    if (hui__svg_attr(tagsrc,"x1",v,sizeof v)) x1=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"y1",v,sizeof v)) y1=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"x2",v,sizeof v)) x2=(float)atof(v);
                    if (hui__svg_attr(tagsrc,"y2",v,sizeof v)) y2=(float)atof(v);
                    hui_contour *c = hui_vec_obj_add_contour(o);
                    if (c) { c->start=(hui_v2){x1,y1}; hui__svg_add_line_seg(c,(hui_v2){x2,y2}); }
                } else if (strcmp(tagname,"polyline")==0 || strcmp(tagname,"polygon")==0) {
                    if (hui__svg_attr(tagsrc, "points", v, sizeof v)) {
                        hui_contour *c = hui_vec_obj_add_contour(o);
                        const char *q = v; float px2, py2; int first = 1;
                        while (c && sscanf(q, " %f%*[ ,]%f", &px2, &py2) == 2) {
                            if (first) { c->start = (hui_v2){px2,py2}; first = 0; }
                            else hui__svg_add_line_seg(c, (hui_v2){px2,py2});
                            while (*q==' '||*q==',') q++;
                            while (*q && *q!=' ' && *q!=',') q++;   /* skip x */
                            while (*q==' '||*q==',') q++;
                            while (*q && *q!=' ' && *q!=',') q++;   /* skip y */
                        }
                    }
                } else { /* path */
                    if (hui__svg_attr(tagsrc, "d", v, sizeof v))
                        hui__svg_parse_path(o, v);
                }
            }
        } else if (!closing && strcmp(tagname, "svg") != 0 && tagname[0]) {
            nskip++;                                  /* unsupported element */
        }
        p = end + 1;
    }
    free(src);
    if (skipped) *skipped = nskip;
    hui_vec_doc_assign_ids(d);
    return d;
}

#endif /* HUI_VEC_IMPLEMENTATION */
#endif /* HUI_VEC_H */
