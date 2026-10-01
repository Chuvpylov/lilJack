/*
 * hui_draw.h — Draw command list for hui
 *
 * C99. No heap allocation in command structs.
 * Wire-compatible: cmds are raw POD bytes, no pointers.
 */

#ifndef HUI_DRAW_H
#define HUI_DRAW_H

#include <stdint.h>
#include <string.h>
#include "hui_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pool size defaults (override before include) ---- */

#ifndef HUI_MAX_CMDS
#  ifdef HUI_BAREMETAL
#    define HUI_MAX_CMDS   512
#    define HUI_STRPOOL    1024
#    define HUI_DATAPOOL   2048
#  else
#    define HUI_MAX_CMDS   4096
#    define HUI_STRPOOL    8192
#    define HUI_DATAPOOL   32768   /* data_id stored in int16_t → max index 32767 */
#  endif
#endif

/* ---- Command types ---- */

typedef enum {
    HUI_CMD_NOP        = 0,
    HUI_CMD_LINE       = 1,   /* x0,y0 → x1,y1 */
    HUI_CMD_BEZIER4    = 2,   /* cubic bezier p0..p3 */
    HUI_CMD_RECT       = 3,   /* x0,y0, x1=w, y1=h (outline) */
    HUI_CMD_RECT_FILL  = 4,   /* filled rect */
    HUI_CMD_CIRCLE     = 5,   /* x0=cx, y0=cy, x1=r (outline) */
    HUI_CMD_CIRCLE_FILL= 6,   /* filled circle */
    HUI_CMD_HEMI       = 7,   /* x0=cx, y0=cy, x1=r, x2=a0_deg, x3=a1_deg */
    HUI_CMD_TRIANGLE   = 8,   /* p0=(x0,y0), p1=(x1,y1), p2=(x2,y2) outline */
    HUI_CMD_TRIANGLE_FILL=9,  /* filled triangle */
    HUI_CMD_ARROW      = 10,  /* x0,y0 → x1,y1 with arrowhead */
    HUI_CMD_IMAGE      = 11,  /* x0,y0,x1=w,y1=h, x2=tex_id */
    HUI_CMD_TEXT       = 12,  /* x0,y0, x2=text_id (into strpool) */
    HUI_CMD_CLIP_PUSH  = 13,  /* set clip rect: x0,y0,x1=w,y1=h */
    HUI_CMD_CLIP_POP   = 14,
    HUI_CMD_PLOT_LINE  = 15,  /* x2=data_id, x3=count */
    HUI_CMD_PLOT_BAR   = 16,
    HUI_CMD_PLOT_HEAT  = 17,
    HUI_CMD_SHADER     = 18,  /* shader_id, target rect */
    HUI_CMD_COUNT
} hui_cmd_type;

/* ---- The 32-byte draw command ---- */

/** @brief Wire-compatible 32-byte draw command. No pointers — safe to memcpy, send over SPI/UDP, store in flash. */
typedef struct {
    uint8_t  type;           /* hui_cmd_type */
    uint8_t  flags;          /* blend mode, fill, AA hint (HUI_FLAG_*) */
    uint8_t  col_r, col_g, col_b, col_a; /* RGBA fill/stroke color */
    int16_t  x0, y0;         /* P0 — origin, center, or start point */
    int16_t  x1, y1;         /* P1 — end point, w/h for rects, radius for circles */
    int16_t  x2, y2;         /* P2 — bezier ctrl pt, tex_id (IMAGE), text_id (TEXT) */
    int16_t  x3, y3;         /* P3 — bezier ctrl pt or arc angle (HEMI) */
    uint8_t  thick;          /* line/stroke thickness in pixels (0 = 1px); TEXT: scale factor */
    uint8_t  rounding;       /* corner rounding radius in pixels */
    uint8_t  z_layer;        /* render order — higher value draws on top (HUI_LAYER_*) */
    uint8_t  _pad[7];        /* pad to 32 bytes */
} hui_cmd;

HUI__SASSERT(sizeof(hui_cmd) == 32, "hui_cmd must be 32 bytes");

/* Flags for hui_cmd.flags */
#define HUI_FLAG_AA       (1 << 0)  /* anti-alias hint */
#define HUI_FLAG_FILL     (1 << 1)  /* fill (overloaded for some cmds) */
#define HUI_FLAG_BLEND    (1 << 2)  /* alpha blend */
/* blend/compositing modes, encoded in cmd flags bits 3-5 (0 = normal over) */
enum { HUI_BLEND_NORMAL=0, HUI_BLEND_MULTIPLY=1, HUI_BLEND_SCREEN=2,
       HUI_BLEND_ADD=3, HUI_BLEND_DARKEN=4, HUI_BLEND_LIGHTEN=5 };
/* queue-time current blend mode; hui_rect_fill stamps it into cmd flags so it
 * survives the deferred draw list. Set around a group of fills, reset to 0. */
extern uint8_t hui_blend_mode;

/* Layer values for hui_cmd.z_layer (higher = renders on top) */
/** @brief Layer for backgrounds and drop shadows. */
#define HUI_LAYER_BG      0    /* backgrounds, drop shadows */
/** @brief Default layer for normal content. */
#define HUI_LAYER_NORMAL  64   /* default content */
/** @brief Layer for popups and dropdown menus, renders above normal content. */
#define HUI_LAYER_POPUP   128  /* popups, dropdown menus */
/** @brief Topmost layer for tooltips and debug overlays. */
#define HUI_LAYER_OVERLAY 192  /* tooltips, debug overlays */

/* ---- Draw list ---- */

#ifdef HUI_BAREMETAL
/* Static allocation on baremetal */
/** @brief Draw list — static fixed-capacity variant for baremetal targets. Holds commands, interned strings, and plot float data. */
typedef struct {
    hui_cmd  cmds[HUI_MAX_CMDS];
    uint16_t count;
    char     strpool[HUI_STRPOOL];
    uint16_t strpool_len;
    float    datapool[HUI_DATAPOOL];
    uint32_t datapool_len;
} hui_dl;
#else
/* Dynamic on Linux/desktop */
/** @brief Draw list — heap-backed, auto-growing variant for Linux/desktop. Holds commands, interned strings, and plot float data. */
typedef struct {
    hui_cmd  *cmds;
    uint16_t  count;
    uint16_t  cap;
    char     *strpool;
    uint16_t  strpool_len;
    uint16_t  strpool_cap;
    float    *datapool;
    uint32_t  datapool_len;
    uint32_t  datapool_cap;
} hui_dl;
#endif

/* ---- Draw list management ---- */

#ifdef HUI_IMPLEMENTATION

static uint8_t hui__cur_layer = HUI_LAYER_NORMAL;

#ifndef HUI_BAREMETAL
#include <stdlib.h>

static void hui_dl_init(hui_dl *dl) {
    dl->cap          = HUI_MAX_CMDS;
    dl->cmds         = (hui_cmd *)malloc(sizeof(hui_cmd) * dl->cap);
    dl->count        = 0;
    dl->strpool_cap  = HUI_STRPOOL;
    dl->strpool      = (char *)malloc(dl->strpool_cap);
    dl->strpool_len  = 0;
    dl->datapool_cap = HUI_DATAPOOL;
    dl->datapool     = (float *)malloc(sizeof(float) * dl->datapool_cap);
    dl->datapool_len = 0;
}

static void hui_dl_free(hui_dl *dl) {
    free(dl->cmds);
    free(dl->strpool);
    free(dl->datapool);
    memset(dl, 0, sizeof(*dl));
}

static void hui_dl_grow_cmds(hui_dl *dl) {
    /* count/cap are uint16_t because the wire format stores a 16-bit command
     * count.  A straight doubling at 32768 used to wrap cap to zero, turning
     * realloc(..., 0) into a free and crashing on the next command. */
    uint16_t new_cap = dl->cap > (uint16_t)(UINT16_MAX / 2u)
                     ? UINT16_MAX : (uint16_t)(dl->cap * 2u);
    if (new_cap <= dl->cap) return;
    hui_cmd *grown = (hui_cmd *)realloc(dl->cmds, sizeof(hui_cmd) * new_cap);
    if (!grown) return;
    dl->cmds = grown;
    dl->cap = new_cap;
}
static void hui_dl_grow_str(hui_dl *dl) {
    dl->strpool_cap *= 2;
    dl->strpool = (char *)realloc(dl->strpool, dl->strpool_cap);
}
static void hui_dl_grow_data(hui_dl *dl) {
    dl->datapool_cap *= 2;
    dl->datapool = (float *)realloc(dl->datapool, sizeof(float)*dl->datapool_cap);
}

#else /* HUI_BAREMETAL */

static void hui_dl_init(hui_dl *dl) {
    memset(dl, 0, sizeof(*dl));
}
static void hui_dl_free(hui_dl *dl) { (void)dl; }
static void hui_dl_grow_cmds(hui_dl *dl)  { (void)dl; /* no-op, fixed */ }
static void hui_dl_grow_str(hui_dl *dl)   { (void)dl; }
static void hui_dl_grow_data(hui_dl *dl)  { (void)dl; }

#endif /* HUI_BAREMETAL */

static void hui_dl_reset(hui_dl *dl) {
    dl->count        = 0;
    dl->strpool_len  = 0;
    dl->datapool_len = 0;
}

/* Push a command onto the draw list — returns pointer to initialized cmd */
static hui_cmd *hui_dl_push(hui_dl *dl) {
#ifndef HUI_BAREMETAL
    if (dl->count >= dl->cap) hui_dl_grow_cmds(dl);
    if (dl->count >= dl->cap) { static hui_cmd hui__overflow_cmd; return &hui__overflow_cmd; }
#else
    if (dl->count >= HUI_MAX_CMDS) { static hui_cmd hui__overflow_cmd; return &hui__overflow_cmd; } /* drop into scratch */
#endif
    hui_cmd *c = &dl->cmds[dl->count++];
    memset(c, 0, sizeof(*c));
    c->z_layer = hui__cur_layer;
    return c;
}

/* Intern a string into strpool; returns text_id (offset) */
static int16_t hui_dl_str(hui_dl *dl, const char *s) {
    if (!s) return 0;
    int16_t id = (int16_t)dl->strpool_len;
    int i = 0;
    do {
#ifndef HUI_BAREMETAL
        if (dl->strpool_len >= dl->strpool_cap) hui_dl_grow_str(dl);
        dl->strpool[dl->strpool_len++] = s[i];
#else
        if (dl->strpool_len < HUI_STRPOOL - 1) {
            dl->strpool[dl->strpool_len++] = s[i];
        } else if (s[i] == '\0') {
            dl->strpool[HUI_STRPOOL - 1] = '\0';
            if (dl->strpool_len < HUI_STRPOOL) dl->strpool_len++;
        }
#endif
    } while (s[i++]);
    return id;
}

/* data_id is stored in int16_t cmd fields — pool size must fit */
HUI__SASSERT(HUI_DATAPOOL <= 32768, "HUI_DATAPOOL exceeds int16_t addressable range");

/* Append float array to datapool; returns data_id (index) */
static int16_t hui_dl_data(hui_dl *dl, const float *d, uint32_t n)
    __attribute__((unused));
static int16_t hui_dl_data(hui_dl *dl, const float *d, uint32_t n) {
    int16_t id = (int16_t)dl->datapool_len;
#ifndef HUI_BAREMETAL
    while (dl->datapool_len + n > dl->datapool_cap) hui_dl_grow_data(dl);
#else
    if (dl->datapool_len + n > HUI_DATAPOOL) return 0;
#endif
    memcpy(dl->datapool + dl->datapool_len, d, sizeof(float)*n);
    dl->datapool_len += n;
    return id;
}

#endif /* HUI_IMPLEMENTATION */

/* ---- Current draw list pointer (set by hui_begin_frame) ---- */
/* This avoids needing hui_ctx to be defined in this header. */
extern hui_dl *hui_dl_g;
#define HUI_DL() hui_dl_g

/* ---- Draw API macros for color ---- */
#define HUI_C_SET(cmd, c) \
    do { (cmd)->col_r=(c).r; (cmd)->col_g=(c).g; \
         (cmd)->col_b=(c).b; (cmd)->col_a=(c).a; } while(0)

/* ---- Draw API ---- */

#ifdef HUI_IMPLEMENTATION

void hui_line(int x0, int y0, int x1, int y1, hui_color c, uint8_t thick) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type  = HUI_CMD_LINE;
    cmd->x0=(int16_t)x0; cmd->y0=(int16_t)y0;
    cmd->x1=(int16_t)x1; cmd->y1=(int16_t)y1;
    cmd->thick = thick;
    HUI_C_SET(cmd, c);
}

void hui_rect_outline(hui_rect r, hui_color c, uint8_t rounding) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type     = HUI_CMD_RECT;
    cmd->x0=(int16_t)r.x; cmd->y0=(int16_t)r.y;
    cmd->x1=(int16_t)r.w; cmd->y1=(int16_t)r.h;
    cmd->rounding = rounding;
    HUI_C_SET(cmd, c);
}

uint8_t hui_blend_mode = 0;

void hui_rect_fill(hui_rect r, hui_color c, uint8_t rounding) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type     = HUI_CMD_RECT_FILL;
    cmd->x0=(int16_t)r.x; cmd->y0=(int16_t)r.y;
    cmd->x1=(int16_t)r.w; cmd->y1=(int16_t)r.h;
    cmd->rounding = rounding;
    cmd->flags |= (uint8_t)((hui_blend_mode & 7u) << 3);   /* carry blend mode */
    HUI_C_SET(cmd, c);
}

/* Convenience: rect() is outline by default (plan API) */
void hui_rect_draw(hui_rect r, hui_color c, uint8_t rounding) {
    hui_rect_outline(r, c, rounding);
}

void hui_circle(int cx, int cy, int r, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_CIRCLE;
    cmd->x0=(int16_t)cx; cmd->y0=(int16_t)cy;
    cmd->x1=(int16_t)r;
    HUI_C_SET(cmd, c);
}

void hui_circle_fill(int cx, int cy, int r, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_CIRCLE_FILL;
    cmd->x0=(int16_t)cx; cmd->y0=(int16_t)cy;
    cmd->x1=(int16_t)r;
    HUI_C_SET(cmd, c);
}

void hui_triangle(hui_v2i a, hui_v2i b, hui_v2i cv, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_TRIANGLE;
    cmd->x0=a.x; cmd->y0=a.y;
    cmd->x1=b.x; cmd->y1=b.y;
    cmd->x2=cv.x; cmd->y2=cv.y;
    HUI_C_SET(cmd, c);
}

void hui_triangle_fill(hui_v2i a, hui_v2i b, hui_v2i cv, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_TRIANGLE_FILL;
    cmd->x0=a.x; cmd->y0=a.y;
    cmd->x1=b.x; cmd->y1=b.y;
    cmd->x2=cv.x; cmd->y2=cv.y;
    HUI_C_SET(cmd, c);
}

void hui_hemi(int cx, int cy, int r, int a0, int a1, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_HEMI;
    cmd->x0=(int16_t)cx; cmd->y0=(int16_t)cy;
    cmd->x1=(int16_t)r;
    cmd->x2=(int16_t)a0; cmd->x3=(int16_t)a1;
    HUI_C_SET(cmd, c);
}

void hui_bezier4(hui_v2i p0, hui_v2i p1, hui_v2i p2, hui_v2i p3,
                 hui_color c, uint8_t thick) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_BEZIER4;
    cmd->x0=p0.x; cmd->y0=p0.y;
    cmd->x1=p1.x; cmd->y1=p1.y;
    cmd->x2=p2.x; cmd->y2=p2.y;
    cmd->x3=p3.x; cmd->y3=p3.y;
    cmd->thick = thick;
    HUI_C_SET(cmd, c);
}

void hui_arrow(int x0, int y0, int x1, int y1, hui_color c) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_ARROW;
    cmd->x0=(int16_t)x0; cmd->y0=(int16_t)y0;
    cmd->x1=(int16_t)x1; cmd->y1=(int16_t)y1;
    HUI_C_SET(cmd, c);
}

/* Optional UI-text override. If non-NULL, hui_text/hui_text_scaled call this
 * at frame-build time instead of pushing a HUI_CMD_TEXT. Lets an app render UI
 * text with a vector font (via hui_glyph_draw, which emits rect_fill spans).
 * Default NULL → unchanged 8x8 bitmap path. */
void (*hui_text_hook)(int x, int y, const char *s, hui_color c, int scale) = 0;

void hui_text(int x, int y, const char *s, hui_color c) {
    if (hui_text_hook) { hui_text_hook(x, y, s, c, 1); return; }
    hui_dl *dl = HUI_DL();
    int16_t tid = hui_dl_str(dl, s);
    hui_cmd *cmd = hui_dl_push(dl);
    cmd->type = HUI_CMD_TEXT;
    cmd->x0=(int16_t)x; cmd->y0=(int16_t)y;
    cmd->x2=tid; cmd->thick=1;
    HUI_C_SET(cmd, c);
}

void hui_text_scaled(int x, int y, const char *s, hui_color c, uint8_t scale) {
    if (hui_text_hook) { hui_text_hook(x, y, s, c, scale > 0 ? scale : 1); return; }
    hui_dl *dl = HUI_DL();
    int16_t tid = hui_dl_str(dl, s);
    hui_cmd *cmd = hui_dl_push(dl);
    cmd->type = HUI_CMD_TEXT;
    cmd->x0=(int16_t)x; cmd->y0=(int16_t)y;
    cmd->x2=tid; cmd->thick=(scale>0?scale:1);
    HUI_C_SET(cmd, c);
}

void hui_image(hui_rect dst, uint16_t tex_id, hui_color tint) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_IMAGE;
    cmd->x0=(int16_t)dst.x; cmd->y0=(int16_t)dst.y;
    cmd->x1=(int16_t)dst.w; cmd->y1=(int16_t)dst.h;
    cmd->x2=(int16_t)tex_id;
    HUI_C_SET(cmd, tint);
}

void hui_clip_push(hui_rect r) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_CLIP_PUSH;
    cmd->x0=(int16_t)r.x; cmd->y0=(int16_t)r.y;
    cmd->x1=(int16_t)r.w; cmd->y1=(int16_t)r.h;
}

void hui_clip_pop(void) {
    hui_cmd *cmd = hui_dl_push(HUI_DL());
    cmd->type = HUI_CMD_CLIP_POP;
}

void hui_set_layer(uint8_t z)  { hui__cur_layer = z; }
void hui_reset_layer(void)     { hui__cur_layer = HUI_LAYER_NORMAL; }
uint8_t hui_get_layer(void)    { return hui__cur_layer; }

#else /* declarations */

/** @brief Antialiased line from (x0,y0) to (x1,y1). @param thick pixel width; 0 or 1 = hairline. */
void hui_line(int x0, int y0, int x1, int y1, hui_color c, uint8_t thick);
/** @brief Outlined (stroked) rectangle. @param rounding corner radius in pixels; 0 = sharp corners. */
void hui_rect_outline(hui_rect r, hui_color c, uint8_t rounding);
/** @brief Solid-filled rectangle. @param rounding corner radius in pixels; 0 = sharp corners. */
void hui_rect_fill(hui_rect r, hui_color c, uint8_t rounding);
/** @brief Alias for hui_rect_outline — outlined rectangle. */
void hui_rect_draw(hui_rect r, hui_color c, uint8_t rounding);
/** @brief Outlined circle. @param cx,cy center; @param r radius in pixels. */
void hui_circle(int cx, int cy, int r, hui_color c);
/** @brief Solid-filled circle. @param cx,cy center; @param r radius in pixels. */
void hui_circle_fill(int cx, int cy, int r, hui_color c);
/** @brief Outlined triangle with vertices a, b, cv (integer pixel coords). */
void hui_triangle(hui_v2i a, hui_v2i b, hui_v2i cv, hui_color c);
/** @brief Solid-filled triangle with vertices a, b, cv (integer pixel coords). */
void hui_triangle_fill(hui_v2i a, hui_v2i b, hui_v2i cv, hui_color c);
/** @brief Arc/semicircle outline. @param a0,a1 start/end angles in degrees (0=right, CCW). */
void hui_hemi(int cx, int cy, int r, int a0, int a1, hui_color c);
/** @brief Cubic bezier curve through four control points p0..p3. @param thick stroke width in pixels. */
void hui_bezier4(hui_v2i p0, hui_v2i p1, hui_v2i p2, hui_v2i p3,
                 hui_color c, uint8_t thick);
/** @brief Line from (x0,y0) to (x1,y1) with a filled arrowhead at the destination end. */
void hui_arrow(int x0, int y0, int x1, int y1, hui_color c);
/** @brief Draw 8x8 bitmap text at (x,y) at 1× scale. */
void hui_text(int x, int y, const char *s, hui_color c);
/** @brief Draw 8x8 bitmap text at integer pixel scale. @param scale 1=8px, 2=16px, 4=32px, etc. */
void hui_text_scaled(int x, int y, const char *s, hui_color c, uint8_t scale);
/** @brief Optional UI-text override hook. NULL = default 8x8 bitmap text. */
extern void (*hui_text_hook)(int x, int y, const char *s, hui_color c, int scale);
/* hui_text_ex — alias for hui_text_scaled (scale: 1=normal, 2=2x, 4=4x) */
#define hui_text_ex hui_text_scaled
/** @brief Blit a registered texture into dst rect, modulated by tint color. @param tex_id index from hui_image_load(). */
void hui_image(hui_rect dst, uint16_t tex_id, hui_color tint);
/** @brief Push a clip rectangle onto the clip stack; subsequent draws are scissored to r. */
void hui_clip_push(hui_rect r);
/** @brief Pop the most recently pushed clip rectangle, restoring the previous clip region. */
void hui_clip_pop(void);
void hui_set_layer(uint8_t z);   /* set current render layer (HUI_LAYER_*) */
void hui_reset_layer(void);      /* reset to HUI_LAYER_NORMAL */
uint8_t hui_get_layer(void);     /* current render layer — the ONLY way a backend compiled in a
                                    non-implementation TU may read it (hui__cur_layer is static
                                    under HUI_IMPLEMENTATION; a client's font.c TU hit this, 2026-09-12) */

#endif /* HUI_IMPLEMENTATION */

/* ---- Dashed line (unit-vector stepping, no new cmd type) ---- */
/* Emits multiple hui_line segments. dash and gap are in pixels. */
static inline void hui_draw_line_dashed(int x0, int y0, int x1, int y1,
                                        uint8_t thick, float dash, float gap,
                                        hui_color c) {
    float dx = (float)(x1-x0), dy = (float)(y1-y0);
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;
    float ux = dx/len, uy = dy/len;
    float step = dash + gap;
    for (float t = 0.0f; t < len; t += step) {
        float t1 = t + dash < len ? t + dash : len;
        hui_line((int)((float)x0 + ux*t),  (int)((float)y0 + uy*t),
                 (int)((float)x0 + ux*t1), (int)((float)y0 + uy*t1), c, thick);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_DRAW_H */
