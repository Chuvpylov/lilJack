/*
 * hui_plot3d.h — 3D plot system for hui (implot3d-inspired)
 *
 * stb-style single header.
 * Define HUI_PLOT3D_IMPLEMENTATION in exactly one translation unit.
 * Requires hui.h (with HUI_IMPLEMENTATION) to be included first.
 * Optionally include hui_plot.h before this header to share hui_colormap.
 *
 * Usage:
 *   static hui_plot3d_cam cam = HUI_PLOT3D_CAM_DEFAULT;
 *   if (hui_plot3d_begin("Surface", hui_rect_make(x,y,w,h), &cam)) {
 *       hui_plot3d_surface("Z(x,y)", xs, ys, zs, rows, cols, &HUI_CMAP_VIRIDIS);
 *       hui_plot3d_line("Path", px, py, pz, n);
 *       hui_plot3d_end();
 *   }
 *
 * Camera interaction:
 *   LMB drag  — rotate (azimuth / elevation)
 *   Scroll    — zoom
 */

#ifndef HUI_PLOT3D_H
#define HUI_PLOT3D_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#include "hui_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Colormap (minimal, only if hui_plot.h not already included) ---- */

#ifndef HUI_PLOT_H

#ifndef HUI_CMAP_MAX_STOPS
#  define HUI_CMAP_MAX_STOPS 8
#endif

typedef struct {
    float     t[HUI_CMAP_MAX_STOPS];
    hui_color c[HUI_CMAP_MAX_STOPS];
    int       n;
} hui_colormap;

hui_color hui_colormap_at(const hui_colormap *cm, float t);
extern const hui_colormap HUI_CMAP_VIRIDIS;

#endif /* !HUI_PLOT_H */

/* ---- Camera ---- */

typedef struct {
    float elev;   /* elevation in degrees (pitch), -90..90 */
    float azim;   /* azimuth  in degrees (yaw),   0..360  */
    float zoom;   /* zoom factor, 0.1..10.0               */
} hui_plot3d_cam;

#define HUI_PLOT3D_CAM_DEFAULT { 30.0f, -60.0f, 1.0f }

/* ---- Marker types ---- */

typedef enum {
    HUI_PLOT3D_MARKER_CIRCLE   = 0,
    HUI_PLOT3D_MARKER_SQUARE   = 1,
    HUI_PLOT3D_MARKER_DIAMOND  = 2,
    HUI_PLOT3D_MARKER_CROSS    = 3,
    HUI_PLOT3D_MARKER_PLUS     = 4,
    HUI_PLOT3D_MARKER_ASTERISK = 5,
    HUI_PLOT3D_MARKER_UP       = 6,
    HUI_PLOT3D_MARKER_DOWN     = 7,
    HUI_PLOT3D_MARKER_LEFT     = 8,
    HUI_PLOT3D_MARKER_RIGHT    = 9,
} hui_plot3d_marker;

/* ---- API ---- */

/* Begin 3D plot frame. Returns false if rect is too small.
 * cam: persistent camera state, modified by mouse interaction.
 * LMB drag = rotate. Scroll = zoom. */
bool hui_plot3d_begin(const char *title, hui_rect r, hui_plot3d_cam *cam);
void hui_plot3d_end(void);

/* Series — call between begin/end. */
void hui_plot3d_line(const char *label,
                     const float *xs, const float *ys, const float *zs, int n);

void hui_plot3d_scatter(const char *label,
                        const float *xs, const float *ys, const float *zs, int n);

void hui_plot3d_surface(const char *label,
                        const float *xs, const float *ys, const float *zs,
                        int rows, int cols, const hui_colormap *cm);

/* Set marker style for the NEXT scatter call (resets after the call). */
void hui_plot3d_set_marker(hui_plot3d_marker m, int size_px);

/* ---- Implementation ---- */

#ifdef HUI_PLOT3D_IMPLEMENTATION

/* Max quads for painter's-algorithm surface sort (rows*cols ≤ 64×64). */
#ifndef HUI_P3D_MAX_QUADS
#  define HUI_P3D_MAX_QUADS 4096
#endif

#define HUI__P3D_TITLE_H  18
#define HUI__P3D_NTICKS    5
#define HUI__P3D_GRID_N    5

/* ---- Context ---- */

typedef struct {
    hui_rect        r;           /* outer rect including title  */
    hui_rect        pr;          /* plot rect (inner)           */
    hui_plot3d_cam *cam;
    bool            active;
    /* axis bounds (updated by series calls) */
    float           xmin, xmax;
    float           ymin, ymax;
    float           zmin, zmax;
    bool            bounds_set;
    /* series palette index */
    int             series_idx;
    /* next scatter marker style */
    hui_plot3d_marker next_marker;
    int               next_marker_sz;
    /* interaction */
    bool            _dragging;
    float           _drag_az0, _drag_el0;
    int             _drag_sx,  _drag_sy;
} hui__p3d_ctx;

static hui__p3d_ctx hui__p3d;

/* Series palette — mirrors hui_plot.h palette order */
static const hui_color hui__p3d_palette[] = {
    /* CUM_* values inline so this file compiles standalone when hui_cum.h is included */
    /* These resolve at compile-time via the macros from hui_cum.h                    */
    /* (hui_cum.h is always pulled in via hui.h which precedes this file)             */
#ifdef CUM_BLUE
    CUM_BLUE, CUM_ORANG, CUM_GREEN, CUM_PINK,
    CUM_YELL, CUM_CYAN,  CUM_PURP,  CUM_ACCENT,
#else
    /* fallback hard-coded Ristretto palette if CUM macros unavailable */
    {90,158,210,255}, {232,133,90,255}, {141,196,92,255}, {232,85,110,255},
    {242,194,68,255}, {94,194,184,255}, {136,144,224,255}, {90,158,210,255},
#endif
};
#define HUI__P3D_PAL_N ((int)(sizeof(hui__p3d_palette)/sizeof(hui__p3d_palette[0])))

/* ---- Colormap (only when hui_plot.h not already providing it) ---- */

#ifndef HUI_PLOT_H

hui_color hui_colormap_at(const hui_colormap *cm, float t) {
    if (!cm || cm->n == 0) return HUI_RGBA(0,0,0,255);
    if (t <= cm->t[0])           return cm->c[0];
    if (t >= cm->t[cm->n - 1])  return cm->c[cm->n - 1];
    for (int i = 0; i < cm->n - 1; i++) {
        if (t <= cm->t[i + 1]) {
            float u = (t - cm->t[i]) / (cm->t[i + 1] - cm->t[i]);
            return hui_color_lerp(cm->c[i], cm->c[i + 1], u);
        }
    }
    return cm->c[cm->n - 1];
}

const hui_colormap HUI_CMAP_VIRIDIS = {
    { 0.0f, 0.143f, 0.286f, 0.429f, 0.571f, 0.714f, 0.857f, 1.0f },
    { {68,1,84,255},    {70,50,127,255},  {54,92,141,255},
      {39,127,142,255}, {31,161,135,255}, {74,193,109,255},
      {159,218,58,255}, {253,231,37,255} },
    8
};

#endif /* !HUI_PLOT_H */

/* ---- Bounds helpers ---- */

static void hui__p3d_accf(float v, float *mn, float *mx) {
    if (v < *mn) *mn = v;
    if (v > *mx) *mx = v;
}

static void hui__p3d_update_bounds(const float *xs, const float *ys,
                                   const float *zs, int n) {
    hui__p3d_ctx *c = &hui__p3d;
    if (!c->bounds_set) {
        c->xmin = c->xmax = xs[0];
        c->ymin = c->ymax = ys[0];
        c->zmin = c->zmax = zs[0];
        c->bounds_set = true;
    }
    for (int i = 0; i < n; i++) {
        hui__p3d_accf(xs[i], &c->xmin, &c->xmax);
        hui__p3d_accf(ys[i], &c->ymin, &c->ymax);
        hui__p3d_accf(zs[i], &c->zmin, &c->zmax);
    }
}

/* ---- 3D → 2D projection ---- */

/*
 * Orthographic projection:
 *   1. Normalise data coords to [-1,+1] per axis.
 *   2. Rotate: azimuth around Z, elevation around X (right-handed look-from-above).
 *   3. Map to screen with Y-flip.
 */
static hui_v2 hui__p3d_project(float x, float y, float z) {
    hui__p3d_ctx *c = &hui__p3d;

    /* Normalize to [-1,1] */
    float nx = (c->xmax > c->xmin) ? 2.0f*(x - c->xmin)/(c->xmax - c->xmin) - 1.0f : 0.0f;
    float ny = (c->ymax > c->ymin) ? 2.0f*(y - c->ymin)/(c->ymax - c->ymin) - 1.0f : 0.0f;
    float nz = (c->zmax > c->zmin) ? 2.0f*(z - c->zmin)/(c->zmax - c->zmin) - 1.0f : 0.0f;

    float az = c->cam->azim * (3.14159265f / 180.0f);
    float el = c->cam->elev * (3.14159265f / 180.0f);

    float cos_az = cosf(az), sin_az = sinf(az);
    float cos_el = cosf(el), sin_el = sinf(el);

    /* Azimuth rotation around Z axis */
    float rx = nx * cos_az - ny * sin_az;
    float ry = nx * sin_az + ny * cos_az;
    float rz = nz;

    /* Elevation rotation around X axis */
    float rx2 = rx;
    float ry2 = ry * cos_el - rz * sin_el;
    /* rz2 = ry * sin_el + rz * cos_el; (depth, not needed for ortho xy) */

    int   min_dim = hui__p3d.pr.w < hui__p3d.pr.h ? hui__p3d.pr.w : hui__p3d.pr.h;
    float scale   = (float)min_dim * 0.4f * c->cam->zoom;
    float cx_s    = (float)(c->pr.x + c->pr.w / 2);
    float cy_s    = (float)(c->pr.y + c->pr.h / 2);

    hui_v2 out;
    out.x = cx_s + rx2 * scale;
    out.y = cy_s - ry2 * scale;   /* Y-flip for screen space */
    return out;
}

/* Project with depth (z'' after full rotation) for painter's sort. */
static float hui__p3d_depth(float x, float y, float z) {
    hui__p3d_ctx *c = &hui__p3d;

    float nx = (c->xmax > c->xmin) ? 2.0f*(x - c->xmin)/(c->xmax - c->xmin) - 1.0f : 0.0f;
    float ny = (c->ymax > c->ymin) ? 2.0f*(y - c->ymin)/(c->ymax - c->ymin) - 1.0f : 0.0f;
    float nz = (c->zmax > c->zmin) ? 2.0f*(z - c->zmin)/(c->zmax - c->zmin) - 1.0f : 0.0f;

    float az = c->cam->azim * (3.14159265f / 180.0f);
    float el = c->cam->elev * (3.14159265f / 180.0f);

    float cos_az = cosf(az), sin_az = sinf(az);
    float cos_el = cosf(el), sin_el = sinf(el);
    (void)cos_el; /* depth only uses sin_el */

    float ry = nx * sin_az + ny * cos_az;

    /* z-depth = ry*sin(el) + nz*cos(el) */
    return ry * sin_el + nz * cos_el;
}

/* ---- Marker draw ---- */

static void hui__p3d_draw_marker(int px, int py, hui_plot3d_marker m, int sz,
                                 hui_color c) {
    int h = sz / 2;
    if (h < 1) h = 1;

    switch (m) {
    case HUI_PLOT3D_MARKER_CIRCLE:
        hui_circle_fill(px, py, h, c);
        hui_circle(px, py, h, HUI_RGBA(0,0,0,180));
        break;
    case HUI_PLOT3D_MARKER_SQUARE: {
        hui_rect sr = hui_rect_make(px - h, py - h, h*2, h*2);
        hui_rect_fill(sr, c, 0);
        break;
    }
    case HUI_PLOT3D_MARKER_DIAMOND:
        hui_line(px,    py - h, px + h, py,    c, 1);
        hui_line(px + h, py,   px,    py + h, c, 1);
        hui_line(px,    py + h, px - h, py,   c, 1);
        hui_line(px - h, py,   px,    py - h, c, 1);
        break;
    case HUI_PLOT3D_MARKER_CROSS:
        hui_line(px - h, py - h, px + h, py + h, c, 1);
        hui_line(px + h, py - h, px - h, py + h, c, 1);
        break;
    case HUI_PLOT3D_MARKER_PLUS:
        hui_line(px - h, py, px + h, py, c, 1);
        hui_line(px, py - h, px, py + h, c, 1);
        break;
    case HUI_PLOT3D_MARKER_ASTERISK:
        hui_line(px - h, py,                         px + h, py,                         c, 1);
        hui_line((int)(px - h*0.866f), (int)(py - h*0.5f),
                 (int)(px + h*0.866f), (int)(py + h*0.5f), c, 1);
        hui_line((int)(px - h*0.866f), (int)(py + h*0.5f),
                 (int)(px + h*0.866f), (int)(py - h*0.5f), c, 1);
        break;
    case HUI_PLOT3D_MARKER_UP: {
        hui_v2i a = {(int16_t)px,       (int16_t)(py - h)};
        hui_v2i b = {(int16_t)(px + h), (int16_t)(py + h)};
        hui_v2i cv = {(int16_t)(px - h),(int16_t)(py + h)};
        hui_triangle_fill(a, b, cv, c);
        break;
    }
    case HUI_PLOT3D_MARKER_DOWN: {
        hui_v2i a = {(int16_t)px,       (int16_t)(py + h)};
        hui_v2i b = {(int16_t)(px + h), (int16_t)(py - h)};
        hui_v2i cv = {(int16_t)(px - h),(int16_t)(py - h)};
        hui_triangle_fill(a, b, cv, c);
        break;
    }
    case HUI_PLOT3D_MARKER_LEFT: {
        hui_v2i a = {(int16_t)(px - h), (int16_t)py};
        hui_v2i b = {(int16_t)(px + h), (int16_t)(py + h)};
        hui_v2i cv = {(int16_t)(px + h),(int16_t)(py - h)};
        hui_triangle_fill(a, b, cv, c);
        break;
    }
    case HUI_PLOT3D_MARKER_RIGHT: {
        hui_v2i a = {(int16_t)(px + h), (int16_t)py};
        hui_v2i b = {(int16_t)(px - h), (int16_t)(py - h)};
        hui_v2i cv = {(int16_t)(px - h),(int16_t)(py + h)};
        hui_triangle_fill(a, b, cv, c);
        break;
    }
    default:
        hui_circle_fill(px, py, h, c);
        break;
    }
}

/* ---- Surface quad sort ---- */

typedef struct {
    float depth;
    int   r, c;
} hui__p3d_quad;

static int hui__p3d_quad_cmp(const void *a, const void *b) {
    const hui__p3d_quad *qa = (const hui__p3d_quad *)a;
    const hui__p3d_quad *qb = (const hui__p3d_quad *)b;
    /* Far (large depth) drawn first */
    if (qa->depth < qb->depth) return  1;
    if (qa->depth > qb->depth) return -1;
    return 0;
}

static hui__p3d_quad hui__p3d_quad_buf[HUI_P3D_MAX_QUADS];

/* ---- Begin / End ---- */

bool hui_plot3d_begin(const char *title, hui_rect r, hui_plot3d_cam *cam) {
    if (r.w < 40 || r.h < 40) return false;

    hui__p3d_ctx *c = &hui__p3d;
    c->r          = r;
    c->cam        = cam;
    c->active     = true;
    c->bounds_set = false;
    c->series_idx = 0;
    c->next_marker    = HUI_PLOT3D_MARKER_CIRCLE;
    c->next_marker_sz = 6;

    /* Reserve title bar */
    c->pr = r;
    if (title && title[0]) {
        c->pr.y += HUI__P3D_TITLE_H;
        c->pr.h -= HUI__P3D_TITLE_H;
        if (c->pr.h < 10) { c->active = false; return false; }
    }

    /* Default bounds (will be overwritten by first series call) */
    c->xmin = -1.0f; c->xmax = 1.0f;
    c->ymin = -1.0f; c->ymax = 1.0f;
    c->zmin = -1.0f; c->zmax = 1.0f;

    /* Background */
    hui_rect_fill(c->pr, CUM_BG1, 3);
    hui_rect_outline(c->pr, CUM_BG3, 3);

    /* Title */
    if (title && title[0]) {
        hui_rect title_r = r;
        title_r.h = HUI__P3D_TITLE_H;
        hui_rect_fill(title_r, CUM_BG0, 3);
        hui_text(title_r.x + 6,
                 title_r.y + (HUI__P3D_TITLE_H - 8) / 2,
                 title, CUM_FG);
    }

    /* ---- Mouse interaction ---- */
    {
        bool hov     = hui_is_hovered(c->pr);
        bool lmb     = (hui_g->io.mouse_btn      & 1) != 0;
        bool lmb_dn  = lmb && !(hui_g->io.mouse_btn_prev & 1);
        bool lmb_up  = !lmb && (hui_g->io.mouse_btn_prev & 1) != 0;

        if (lmb_up) c->_dragging = false;
        if (hov && lmb_dn) {
            c->_dragging  = true;
            c->_drag_az0  = cam->azim;
            c->_drag_el0  = cam->elev;
            c->_drag_sx   = hui_g->io.mouse_x;
            c->_drag_sy   = hui_g->io.mouse_y;
        }
        if (c->_dragging && lmb) {
            int dx = hui_g->io.mouse_x - c->_drag_sx;
            int dy = hui_g->io.mouse_y - c->_drag_sy;
            cam->azim = c->_drag_az0 + (float)dx * 0.5f;
            cam->elev = c->_drag_el0 - (float)dy * 0.5f;
            if (cam->elev >  89.0f) cam->elev =  89.0f;
            if (cam->elev < -89.0f) cam->elev = -89.0f;
        }
        if (hov && hui_g->io.scroll_dy) {
            cam->zoom *= (hui_g->io.scroll_dy > 0) ? 1.1f : (1.0f / 1.1f);
            if (cam->zoom < 0.1f)  cam->zoom = 0.1f;
            if (cam->zoom > 10.0f) cam->zoom = 10.0f;
        }
    }

    hui_clip_push(c->pr);
    return true;
}

void hui_plot3d_end(void) {
    hui__p3d_ctx *c = &hui__p3d;
    if (!c->active) return;

    /* Draw grid on XY floor (zmin plane) */
    {
        hui_color gc = CUM_BG3;
        float xstep  = (c->xmax - c->xmin) / (float)HUI__P3D_GRID_N;
        float ystep  = (c->ymax - c->ymin) / (float)HUI__P3D_GRID_N;
        for (int i = 0; i <= HUI__P3D_GRID_N; i++) {
            float xv = c->xmin + (float)i * xstep;
            float yv = c->ymin + (float)i * ystep;
            /* Lines parallel to Y */
            hui_v2 a = hui__p3d_project(xv, c->ymin, c->zmin);
            hui_v2 b = hui__p3d_project(xv, c->ymax, c->zmin);
            hui_line((int)a.x, (int)a.y, (int)b.x, (int)b.y, gc, 1);
            /* Lines parallel to X */
            a = hui__p3d_project(c->xmin, yv, c->zmin);
            b = hui__p3d_project(c->xmax, yv, c->zmin);
            hui_line((int)a.x, (int)a.y, (int)b.x, (int)b.y, gc, 1);
        }
    }

    /* Draw axis lines with arrowheads */
    {
        /* X axis — red */
        hui_v2 o  = hui__p3d_project(c->xmin, c->ymin, c->zmin);
        hui_v2 ax = hui__p3d_project(c->xmax, c->ymin, c->zmin);
        hui_v2 ay = hui__p3d_project(c->xmin, c->ymax, c->zmin);
        hui_v2 az = hui__p3d_project(c->xmin, c->ymin, c->zmax);

        hui_color cx_col = HUI_RGB(220, 80,  80);
        hui_color cy_col = HUI_RGB( 80, 200, 80);
        hui_color cz_col = HUI_RGB( 80, 130, 220);

        hui_arrow((int)o.x,  (int)o.y,  (int)ax.x, (int)ax.y, cx_col);
        hui_arrow((int)o.x,  (int)o.y,  (int)ay.x, (int)ay.y, cy_col);
        hui_arrow((int)o.x,  (int)o.y,  (int)az.x, (int)az.y, cz_col);

        /* Axis labels */
        hui_text((int)ax.x + 3, (int)ax.y - 4, "X", cx_col);
        hui_text((int)ay.x + 3, (int)ay.y - 4, "Y", cy_col);
        hui_text((int)az.x + 3, (int)az.y - 4, "Z", cz_col);
    }

    /* Tick labels — 5 ticks per axis along each axis edge */
    {
        int nt = HUI__P3D_NTICKS;
        for (int i = 0; i <= nt; i++) {
            float t = (float)i / (float)nt;

            /* X ticks */
            float xv = c->xmin + t * (c->xmax - c->xmin);
            hui_v2 p = hui__p3d_project(xv, c->ymin, c->zmin);
            hui_text((int)p.x - 8, (int)p.y + 4,
                     hui_fmt("%.3g", (double)xv), CUM_FG3);

            /* Y ticks */
            float yv = c->ymin + t * (c->ymax - c->ymin);
            p = hui__p3d_project(c->xmin, yv, c->zmin);
            hui_text((int)p.x - 24, (int)p.y + 2,
                     hui_fmt("%.3g", (double)yv), CUM_FG3);

            /* Z ticks */
            float zv = c->zmin + t * (c->zmax - c->zmin);
            p = hui__p3d_project(c->xmin, c->ymin, zv);
            hui_text((int)p.x - 28, (int)p.y + 2,
                     hui_fmt("%.3g", (double)zv), CUM_FG3);
        }
    }

    hui_clip_pop();
    c->active = false;
}

/* ---- Series ---- */

void hui_plot3d_line(const char *label,
                     const float *xs, const float *ys, const float *zs, int n) {
    (void)label;
    hui__p3d_ctx *c = &hui__p3d;
    if (!c->active || n < 2) return;

    hui__p3d_update_bounds(xs, ys, zs, n);
    hui_color col = hui__p3d_palette[c->series_idx % HUI__P3D_PAL_N];
    c->series_idx++;

    for (int i = 1; i < n; i++) {
        hui_v2 a = hui__p3d_project(xs[i-1], ys[i-1], zs[i-1]);
        hui_v2 b = hui__p3d_project(xs[i],   ys[i],   zs[i]);
        hui_line((int)a.x, (int)a.y, (int)b.x, (int)b.y, col, 2);
    }
}

void hui_plot3d_scatter(const char *label,
                        const float *xs, const float *ys, const float *zs, int n) {
    (void)label;
    hui__p3d_ctx *c = &hui__p3d;
    if (!c->active || n < 1) return;

    hui__p3d_update_bounds(xs, ys, zs, n);
    hui_color col = hui__p3d_palette[c->series_idx % HUI__P3D_PAL_N];
    c->series_idx++;

    hui_plot3d_marker marker  = c->next_marker;
    int               marksz  = c->next_marker_sz;
    /* Reset after use */
    c->next_marker    = HUI_PLOT3D_MARKER_CIRCLE;
    c->next_marker_sz = 6;

    for (int i = 0; i < n; i++) {
        hui_v2 p = hui__p3d_project(xs[i], ys[i], zs[i]);
        hui__p3d_draw_marker((int)p.x, (int)p.y, marker, marksz, col);
    }
}

void hui_plot3d_surface(const char *label,
                        const float *xs, const float *ys, const float *zs,
                        int rows, int cols, const hui_colormap *cm) {
    (void)label;
    hui__p3d_ctx *c = &hui__p3d;
    if (!c->active || rows < 2 || cols < 2) return;

    /* Update bounds from full grid */
    hui__p3d_update_bounds(xs, ys, zs, rows * cols);

    const hui_colormap *pcm = cm ? cm : &HUI_CMAP_VIRIDIS;
    float zrange = c->zmax - c->zmin;

    /* Build quad list with depth for painter's sort */
    int nq = 0;
    int max_r = rows - 1;
    int max_c = cols - 1;
    for (int r = 0; r < max_r && nq < HUI_P3D_MAX_QUADS; r++) {
        for (int cv = 0; cv < max_c && nq < HUI_P3D_MAX_QUADS; cv++) {
            int i00 = r       * cols + cv;
            int i10 = (r + 1) * cols + cv;
            int i01 = r       * cols + (cv + 1);
            int i11 = (r + 1) * cols + (cv + 1);
            /* Quad centre */
            float cx = (xs[i00] + xs[i10] + xs[i01] + xs[i11]) * 0.25f;
            float cy = (ys[i00] + ys[i10] + ys[i01] + ys[i11]) * 0.25f;
            float cz = (zs[i00] + zs[i10] + zs[i01] + zs[i11]) * 0.25f;
            hui__p3d_quad_buf[nq].depth = hui__p3d_depth(cx, cy, cz);
            hui__p3d_quad_buf[nq].r     = r;
            hui__p3d_quad_buf[nq].c     = cv;
            nq++;
        }
    }

    qsort(hui__p3d_quad_buf, (size_t)nq, sizeof(hui__p3d_quad), hui__p3d_quad_cmp);

    for (int qi = 0; qi < nq; qi++) {
        int r  = hui__p3d_quad_buf[qi].r;
        int cv = hui__p3d_quad_buf[qi].c;

        int i00 = r       * cols + cv;
        int i10 = (r + 1) * cols + cv;
        int i01 = r       * cols + (cv + 1);
        int i11 = (r + 1) * cols + (cv + 1);

        hui_v2 p00 = hui__p3d_project(xs[i00], ys[i00], zs[i00]);
        hui_v2 p10 = hui__p3d_project(xs[i10], ys[i10], zs[i10]);
        hui_v2 p01 = hui__p3d_project(xs[i01], ys[i01], zs[i01]);
        hui_v2 p11 = hui__p3d_project(xs[i11], ys[i11], zs[i11]);

        /* Colour from average Z of quad */
        float cz  = (zs[i00] + zs[i10] + zs[i01] + zs[i11]) * 0.25f;
        float t   = (zrange > 0.0f) ? (cz - c->zmin) / zrange : 0.5f;
        hui_color col = hui_colormap_at(pcm, t);

        /* Two triangles per quad */
        hui_v2i a, b, d;

        a.x = (int16_t)p00.x; a.y = (int16_t)p00.y;
        b.x = (int16_t)p10.x; b.y = (int16_t)p10.y;
        d.x = (int16_t)p01.x; d.y = (int16_t)p01.y;
        hui_triangle_fill(a, b, d, col);

        a.x = (int16_t)p10.x; a.y = (int16_t)p10.y;
        b.x = (int16_t)p11.x; b.y = (int16_t)p11.y;
        d.x = (int16_t)p01.x; d.y = (int16_t)p01.y;
        hui_triangle_fill(a, b, d, col);

        /* Edge outline (subdued) */
        hui_color edge = HUI_RGBA(col.r/3, col.g/3, col.b/3, 120);
        hui_line((int)p00.x, (int)p00.y, (int)p10.x, (int)p10.y, edge, 1);
        hui_line((int)p10.x, (int)p10.y, (int)p11.x, (int)p11.y, edge, 1);
        hui_line((int)p11.x, (int)p11.y, (int)p01.x, (int)p01.y, edge, 1);
        hui_line((int)p01.x, (int)p01.y, (int)p00.x, (int)p00.y, edge, 1);
    }

    c->series_idx++;
}

/* ---- Marker setter ---- */

void hui_plot3d_set_marker(hui_plot3d_marker m, int size_px) {
    hui__p3d.next_marker    = m;
    hui__p3d.next_marker_sz = (size_px > 0) ? size_px : 6;
}

#endif /* HUI_PLOT3D_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_PLOT3D_H */
