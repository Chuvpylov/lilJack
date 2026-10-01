/*
 * hui_graph.h — Node graph editor for hui
 *
 * Pan+zoom canvas with nodes, bezier links, drag, multi-select.
 * stb-style single-header: declarations always visible;
 * implementation gated by #ifdef HUI_GRAPH_IMPLEMENTATION.
 *
 * Usage:
 *   #define HUI_GRAPH_IMPLEMENTATION
 *   #include "hui_graph.h"
 *
 * Requires hui.h to be included first (provides hui_math, hui_draw,
 * hui_cum, HUI_FONT_W/H, hui_fmt, hui_is_hovered, etc.)
 *
 * C99. No dynamic allocation. No heap.
 */

#ifndef HUI_GRAPH_H
#define HUI_GRAPH_H

#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits (override before include) ---- */

#ifndef HUI_GRAPH_MAX_NODES
#  define HUI_GRAPH_MAX_NODES  256
#endif
#ifndef HUI_GRAPH_MAX_LINKS
#  define HUI_GRAPH_MAX_LINKS  512
#endif
#ifndef HUI_GRAPH_MAX_SLOTS
#  define HUI_GRAPH_MAX_SLOTS  16
#endif

/* ---- Types ---- */

typedef struct {
    int      id;                                         /* unique node ID (user-assigned, > 0) */
    hui_v2   pos;                                        /* canvas position (NOT screen) */
    char     title[32];
    int      input_count;                                /* number of input slots (left side) */
    int      output_count;                               /* number of output slots (right side) */
    char     input_names[HUI_GRAPH_MAX_SLOTS][16];
    char     output_names[HUI_GRAPH_MAX_SLOTS][16];
    void    *user_data;                                  /* caller-owned, not touched by graph */
    bool     selected;                                   /* set by graph, readable by caller */
} hui_graph_node;

typedef struct {
    int       from_node, from_slot;  /* output slot */
    int       to_node,   to_slot;    /* input slot */
    hui_color color;                 /* CUM_ACCENT if zeroed */
} hui_graph_link;

typedef struct {
    hui_v2  pan;           /* canvas pan offset in pixels */
    float   zoom;          /* 0.25 – 4.0; 1.0 = 100% */
    bool    _initialized;
} hui_graph_view;

/* ---- Public API ---- */

/* Begin graph canvas. r = screen rect.
 * Returns true if r is large enough to draw. */
bool hui_graph_begin(hui_rect r, hui_graph_view *view);
void hui_graph_end(void);

/* Draw a node. Call between begin/end.
 * node->selected is updated by this call (click to select).
 * Returns true if the node body rect was clicked. */
bool hui_graph_node_draw(hui_graph_node *node);

/* Draw a link between two nodes.
 * from_node/to_node must have been drawn this frame already. */
void hui_graph_link_draw(const hui_graph_link *link);

/* Convenience: draw all links first, then all nodes. */
void hui_graph_draw_all(hui_graph_node *nodes, int n_nodes,
                        const hui_graph_link *links, int n_links);

/* Draw minimap in corner of current graph rect (call after graph_end). */
void hui_graph_minimap(hui_rect r, const hui_graph_node *nodes, int n_nodes,
                       const hui_graph_view *view);

/* Adjust view pan/zoom so all nodes fit on screen. */
void hui_graph_fit_screen(hui_graph_view *view,
                          const hui_graph_node *nodes, int n_nodes,
                          hui_rect canvas_r);

/* DAG auto-layout: topologically sort nodes by links, assign column positions.
 * Modifies node[i].pos in place. */
void hui_graph_auto_layout(hui_graph_node *nodes, int n_nodes,
                           const hui_graph_link *links, int n_links);

#ifdef __cplusplus
}
#endif

/* ================================================================
 * IMPLEMENTATION
 * ================================================================ */

#ifdef HUI_GRAPH_IMPLEMENTATION

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Node geometry constants (canvas units) ---- */
#ifndef HUI__GRAPH_NODE_W
#  define HUI__GRAPH_NODE_W     140
#endif
#ifndef HUI__GRAPH_NODE_TITLE_H
#  define HUI__GRAPH_NODE_TITLE_H 22
#endif
#ifndef HUI__GRAPH_NODE_SLOT_H
#  define HUI__GRAPH_NODE_SLOT_H  22
#endif
#define HUI__GRAPH_NODE_PAD_BOT  12
#define HUI__GRAPH_SLOT_R         5

/* ---- Slot position cache ---- */
typedef struct {
    int node_id;
    int slot;
    int is_output;
    int sx, sy;  /* screen coords of slot circle center */
} hui__graph_slot_pos;

static hui__graph_slot_pos hui__graph_slots[HUI_GRAPH_MAX_NODES * HUI_GRAPH_MAX_SLOTS * 2];
static int hui__graph_slot_count = 0;

/* ---- Per-frame graph state ---- */
static bool       hui__graph_active  = false;
static hui_rect   hui__graph_rect;      /* screen rect of canvas */
static hui_graph_view *hui__graph_view = NULL;

/* Drag state */
static int   hui__graph_drag_id     = 0;   /* node id being dragged (0 = none) */
static hui_v2 hui__graph_drag_offset;      /* canvas-space offset from node origin to grab point */

/* Pan drag state */
static bool  hui__graph_pan_dragging = false;

/* ---- Coordinate transforms ---- */

static inline hui_v2 hui__graph_to_screen(hui_v2 canvas_pos) {
    hui_v2 s;
    s.x = canvas_pos.x * hui__graph_view->zoom + hui__graph_view->pan.x + (float)hui__graph_rect.x;
    s.y = canvas_pos.y * hui__graph_view->zoom + hui__graph_view->pan.y + (float)hui__graph_rect.y;
    return s;
}

static inline hui_v2 hui__graph_to_canvas(float sx, float sy) {
    hui_v2 c;
    c.x = (sx - hui__graph_view->pan.x - (float)hui__graph_rect.x) / hui__graph_view->zoom;
    c.y = (sy - hui__graph_view->pan.y - (float)hui__graph_rect.y) / hui__graph_view->zoom;
    return c;
}

/* Node height in canvas units */
static inline int hui__graph_node_h(const hui_graph_node *n) {
    int slots = n->input_count > n->output_count ? n->input_count : n->output_count;
    return HUI__GRAPH_NODE_TITLE_H + slots * HUI__GRAPH_NODE_SLOT_H + HUI__GRAPH_NODE_PAD_BOT;
}

/* Color zero check */
static inline bool hui__color_is_zero(hui_color c) {
    return c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0;
}

/* Dim a color by mixing toward BG1 */
static inline hui_color hui__color_dim(hui_color c, float t) {
    hui_color bg = CUM_BG1;
    return HUI_RGBA(
        (uint8_t)(c.r + (bg.r - c.r) * t),
        (uint8_t)(c.g + (bg.g - c.g) * t),
        (uint8_t)(c.b + (bg.b - c.b) * t),
        c.a
    );
}

/* ---- hui_graph_begin ---- */

bool hui_graph_begin(hui_rect r, hui_graph_view *view) {
    if (r.w < 32 || r.h < 32) return false;

    /* Initialize view on first use */
    if (!view->_initialized) {
        view->pan.x       = 100.0f;
        view->pan.y       = 50.0f;
        view->zoom        = 1.0f;
        view->_initialized = true;
    }

    hui__graph_active      = true;
    hui__graph_rect        = r;
    hui__graph_view        = view;
    hui__graph_slot_count  = 0;

    /* --- Scroll to zoom --- */
    if (hui_is_hovered(r) && hui_g->io.scroll_dy != 0) {
        float factor = (hui_g->io.scroll_dy > 0) ? 1.1f : (1.0f / 1.1f);
        float old_zoom = view->zoom;
        float new_zoom = hui_clampf(old_zoom * factor, 0.25f, 4.0f);

        /* Zoom toward mouse position */
        float mx = (float)hui_g->io.mouse_x;
        float my = (float)hui_g->io.mouse_y;
        float ox = mx - (float)r.x;
        float oy = my - (float)r.y;

        view->pan.x = ox - (ox - view->pan.x) * (new_zoom / old_zoom);
        view->pan.y = oy - (oy - view->pan.y) * (new_zoom / old_zoom);
        view->zoom  = new_zoom;
    }

    /* --- LMB drag on empty canvas = pan --- */
    /* Pan dragging is handled in hui_graph_end after node processing.
     * We start tracking here if mouse down on canvas and no node drag active. */
    bool lmb_down = (hui_g->io.mouse_btn & 1) != 0;
    bool lmb_was  = (hui_g->io.mouse_btn_prev & 1) != 0;
    bool lmb_new  = lmb_down && !lmb_was;

    if (lmb_new && hui_is_hovered(r) && hui__graph_drag_id == 0) {
        hui__graph_pan_dragging = true;
    }
    if (!lmb_down) {
        hui__graph_pan_dragging = false;
        hui__graph_drag_id      = 0;
    }

    if (hui__graph_pan_dragging && hui__graph_drag_id == 0) {
        int dx = hui_g->io.mouse_x - hui_g->io.mouse_x_prev;
        int dy = hui_g->io.mouse_y - hui_g->io.mouse_y_prev;
        view->pan.x += (float)dx;
        view->pan.y += (float)dy;
    }

    /* Canvas background */
    hui_rect_fill(r, CUM_BG2, 0);

    /* Grid lines */
    {
        int grid_step = (int)(40.0f * view->zoom);
        if (grid_step < 8)  grid_step = 8;
        if (grid_step > 160) grid_step = 160;

        int ox = (int)view->pan.x % grid_step;
        int oy = (int)view->pan.y % grid_step;
        if (ox < 0) ox += grid_step;
        if (oy < 0) oy += grid_step;

        hui_color gc = CUM_BG3;
        gc.a = 180;
        for (int x = r.x + ox; x < r.x + r.w; x += grid_step)
            hui_line(x, r.y, x, r.y + r.h, gc, 1);
        for (int y = r.y + oy; y < r.y + r.h; y += grid_step)
            hui_line(r.x, y, r.x + r.w, y, gc, 1);
    }

    /* Clip to canvas */
    hui_clip_push(r);

    return true;
}

/* ---- hui_graph_end ---- */

void hui_graph_end(void) {
    if (!hui__graph_active) return;
    hui_clip_pop();
    hui__graph_active = false;

    /* Border around the whole canvas */
    hui_rect_outline(hui__graph_rect, CUM_BG3, 0);
}

/* ---- hui_graph_node_draw ---- */

bool hui_graph_node_draw(hui_graph_node *node) {
    if (!hui__graph_active || !node || node->id <= 0) return false;

    hui_graph_view *view = hui__graph_view;
    float z = view->zoom;

    int nw = (int)((float)HUI__GRAPH_NODE_W  * z);
    int nh = (int)((float)hui__graph_node_h(node) * z);
    int title_h = (int)((float)HUI__GRAPH_NODE_TITLE_H * z);

    hui_v2 sp = hui__graph_to_screen(node->pos);
    int sx = (int)sp.x;
    int sy = (int)sp.y;

    hui_rect node_r    = hui_rect_make(sx, sy, nw, nh);
    hui_rect header_r  = hui_rect_make(sx, sy, nw, title_h);

    bool hdr_hov  = hui_is_hovered(header_r);

    bool lmb_down = (hui_g->io.mouse_btn & 1) != 0;
    bool lmb_was  = (hui_g->io.mouse_btn_prev & 1) != 0;
    bool lmb_new  = lmb_down && !lmb_was;

    /* --- Dragging --- */
    if (hui__graph_drag_id == node->id) {
        if (lmb_down) {
            /* Update position */
            hui_v2 mouse_canvas = hui__graph_to_canvas(
                (float)hui_g->io.mouse_x, (float)hui_g->io.mouse_y);
            node->pos.x = mouse_canvas.x - hui__graph_drag_offset.x;
            node->pos.y = mouse_canvas.y - hui__graph_drag_offset.y;
            /* Recalculate screen pos after move */
            sp = hui__graph_to_screen(node->pos);
            sx = (int)sp.x;
            sy = (int)sp.y;
            node_r   = hui_rect_make(sx, sy, nw, nh);
            header_r = hui_rect_make(sx, sy, nw, title_h);
        } else {
            hui__graph_drag_id = 0;
        }
    } else if (lmb_new && hdr_hov && hui__graph_drag_id == 0) {
        /* Start drag */
        hui__graph_drag_id = node->id;
        hui__graph_pan_dragging = false;  /* cancel pan */
        hui_v2 mouse_canvas = hui__graph_to_canvas(
            (float)hui_g->io.mouse_x, (float)hui_g->io.mouse_y);
        hui__graph_drag_offset.x = mouse_canvas.x - node->pos.x;
        hui__graph_drag_offset.y = mouse_canvas.y - node->pos.y;
    }

    /* --- Selection --- */
    bool shift = hui_key_held('S') || /* shift not in key codes; use hack via left-shift */
                 hui_g->io.keys[0xA0] || hui_g->io.keys[0xA1]; /* L/R shift if backend fills */
    /* Fallback: check for capital letters as proxy — use a dedicated shift key
     * The shift check: many backends fill keys[16] for shift */
    shift = shift || hui_g->io.keys[16];

    if (lmb_new && hui_is_hovered(header_r)) {
        if (shift) {
            node->selected = !node->selected;
        } else {
            node->selected = true;
            /* Caller is responsible for deselecting others; we only set this node */
        }
    }

    /* Deselect on empty canvas click is handled by caller via hui_graph_draw_all or manually */

    /* --- Draw node body --- */
    uint8_t rnd = (uint8_t)(4.0f * z < 255.0f ? 4.0f * z : 255.0f);
    if (rnd > 8) rnd = 8;

    /* Body background */
    hui_rect_fill(node_r, CUM_BG1, rnd);

    /* Header background */
    hui_color hdr_col = node->selected ? hui__color_dim(CUM_ACCENT, 0.4f)
                      : hdr_hov        ? CUM_BG4
                      :                  CUM_BG3;
    hui_rect_fill(header_r, hdr_col, rnd);
    /* Re-fill bottom of header to square off bottom corners */
    int half_rnd = rnd / 2;
    hui_rect header_bot = hui_rect_make(sx, sy + title_h - half_rnd, nw, half_rnd);
    hui_rect_fill(header_bot, hdr_col, 0);

    /* Title text — centered */
    {
        int text_w = (int)(strlen(node->title) * HUI_FONT_W);
        int tx = sx + (nw - text_w) / 2;
        int ty = sy + (title_h - HUI_FONT_H) / 2;
        hui_text(tx, ty, node->title, CUM_FG);
    }

    /* --- Slots --- */
    int slot_h = (int)((float)HUI__GRAPH_NODE_SLOT_H * z);
    int slot_r = (int)((float)HUI__GRAPH_SLOT_R * z);
    if (slot_r < 3) slot_r = 3;

    int body_top = sy + title_h;

    /* Input slots (left side) */
    for (int i = 0; i < node->input_count && i < HUI_GRAPH_MAX_SLOTS; i++) {
        int cx = sx;
        int cy = body_top + slot_h / 2 + i * slot_h;

        hui_circle_fill(cx, cy, slot_r, CUM_BG4);
        hui_circle(cx, cy, slot_r, CUM_FG3);

        /* Label to the right of circle */
        int lx = cx + slot_r + 4;
        int ly = cy - HUI_FONT_H / 2;
        if (node->input_names[i][0])
            hui_text(lx, ly, node->input_names[i], CUM_FG2);

        /* Cache slot position */
        if (hui__graph_slot_count < HUI_GRAPH_MAX_NODES * HUI_GRAPH_MAX_SLOTS * 2) {
            hui__graph_slot_pos *sp_cache = &hui__graph_slots[hui__graph_slot_count++];
            sp_cache->node_id   = node->id;
            sp_cache->slot      = i;
            sp_cache->is_output = 0;
            sp_cache->sx        = cx;
            sp_cache->sy        = cy;
        }
    }

    /* Output slots (right side) */
    for (int i = 0; i < node->output_count && i < HUI_GRAPH_MAX_SLOTS; i++) {
        int cx = sx + nw;
        int cy = body_top + slot_h / 2 + i * slot_h;

        hui_circle_fill(cx, cy, slot_r, CUM_BG4);
        hui_circle(cx, cy, slot_r, CUM_FG3);

        /* Label to the left of circle */
        if (node->output_names[i][0]) {
            int lw = (int)(strlen(node->output_names[i]) * HUI_FONT_W);
            int lx = cx - slot_r - 4 - lw;
            int ly = cy - HUI_FONT_H / 2;
            hui_text(lx, ly, node->output_names[i], CUM_FG2);
        }

        /* Cache slot position */
        if (hui__graph_slot_count < HUI_GRAPH_MAX_NODES * HUI_GRAPH_MAX_SLOTS * 2) {
            hui__graph_slot_pos *sp_cache = &hui__graph_slots[hui__graph_slot_count++];
            sp_cache->node_id   = node->id;
            sp_cache->slot      = i;
            sp_cache->is_output = 1;
            sp_cache->sx        = cx;
            sp_cache->sy        = cy;
        }
    }

    /* Border */
    hui_color border_col = node->selected ? CUM_ACCENT : CUM_BG3;
    hui_rect_outline(node_r, border_col, rnd);

    /* Return true if body was clicked */
    bool body_r_rect = hui_is_clicked(node_r, 0);
    return body_r_rect;
}

/* ---- hui_graph_link_draw ---- */

static const hui__graph_slot_pos *hui__find_slot(int node_id, int slot, int is_output) {
    for (int i = 0; i < hui__graph_slot_count; i++) {
        const hui__graph_slot_pos *s = &hui__graph_slots[i];
        if (s->node_id == node_id && s->slot == slot && s->is_output == is_output)
            return s;
    }
    return NULL;
}

void hui_graph_link_draw(const hui_graph_link *link) {
    if (!hui__graph_active || !link) return;

    const hui__graph_slot_pos *from = hui__find_slot(link->from_node, link->from_slot, 1);
    const hui__graph_slot_pos *to   = hui__find_slot(link->to_node,   link->to_slot,   0);
    if (!from || !to) return;

    float z = hui__graph_view->zoom;
    float cx_off = 80.0f * z;

    hui_v2i p0 = { (int16_t)from->sx, (int16_t)from->sy };
    hui_v2i p1 = { (int16_t)(from->sx + (int)cx_off), (int16_t)from->sy };
    hui_v2i p2 = { (int16_t)(to->sx   - (int)cx_off), (int16_t)to->sy };
    hui_v2i p3 = { (int16_t)to->sx,   (int16_t)to->sy };

    hui_color col = hui__color_is_zero(link->color) ? CUM_ACCENT : link->color;
    hui_bezier4(p0, p1, p2, p3, col, 2);
}

/* ---- hui_graph_draw_all ---- */

/* Pre-populate slot cache without drawing, so links can be drawn under nodes */
static void hui__graph_cache_slots(hui_graph_node *nodes, int n_nodes) {
    float z = hui__graph_view->zoom;
    int slot_h = (int)((float)HUI__GRAPH_NODE_SLOT_H * z);

    for (int ni = 0; ni < n_nodes; ni++) {
        hui_graph_node *node = &nodes[ni];
        if (node->id <= 0) continue;

        hui_v2 sp = hui__graph_to_screen(node->pos);
        int sx = (int)sp.x;
        int sy = (int)sp.y;
        int title_h = (int)((float)HUI__GRAPH_NODE_TITLE_H * z);
        int nw      = (int)((float)HUI__GRAPH_NODE_W * z);
        int body_top = sy + title_h;

        for (int i = 0; i < node->input_count && i < HUI_GRAPH_MAX_SLOTS; i++) {
            if (hui__graph_slot_count >= HUI_GRAPH_MAX_NODES * HUI_GRAPH_MAX_SLOTS * 2) break;
            hui__graph_slot_pos *sc = &hui__graph_slots[hui__graph_slot_count++];
            sc->node_id   = node->id;
            sc->slot      = i;
            sc->is_output = 0;
            sc->sx        = sx;
            sc->sy        = body_top + slot_h / 2 + i * slot_h;
        }
        for (int i = 0; i < node->output_count && i < HUI_GRAPH_MAX_SLOTS; i++) {
            if (hui__graph_slot_count >= HUI_GRAPH_MAX_NODES * HUI_GRAPH_MAX_SLOTS * 2) break;
            hui__graph_slot_pos *sc = &hui__graph_slots[hui__graph_slot_count++];
            sc->node_id   = node->id;
            sc->slot      = i;
            sc->is_output = 1;
            sc->sx        = sx + nw;
            sc->sy        = body_top + slot_h / 2 + i * slot_h;
        }
    }
}

void hui_graph_draw_all(hui_graph_node *nodes, int n_nodes,
                        const hui_graph_link *links, int n_links)
{
    if (!hui__graph_active) return;

    bool lmb_new = (hui_g->io.mouse_btn & 1) && !(hui_g->io.mouse_btn_prev & 1);
    bool shift   = hui_g->io.keys[16];

    /* Pre-pass: populate slot cache so links can query positions */
    hui__graph_cache_slots(nodes, n_nodes);

    /* Links first (under nodes) */
    for (int i = 0; i < n_links; i++)
        hui_graph_link_draw(&links[i]);

    /* Reset slot cache so hui_graph_node_draw re-populates fresh screen coords
     * (node drag may have moved them since the pre-pass) */
    hui__graph_slot_count = 0;

    /* Then nodes */
    bool any_node_hovered = false;
    for (int i = 0; i < n_nodes; i++) {
        hui_graph_node *n = &nodes[i];
        float z = hui__graph_view->zoom;
        int nw = (int)((float)HUI__GRAPH_NODE_W * z);
        int nh = (int)((float)hui__graph_node_h(n) * z);
        hui_v2 sp = hui__graph_to_screen(n->pos);
        hui_rect nr = hui_rect_make((int)sp.x, (int)sp.y, nw, nh);
        if (hui_is_hovered(nr)) any_node_hovered = true;
        hui_graph_node_draw(n);
    }

    /* Click on empty canvas = deselect all (no shift) */
    if (lmb_new && !any_node_hovered && !shift &&
        hui_is_hovered(hui__graph_rect))
    {
        for (int i = 0; i < n_nodes; i++)
            nodes[i].selected = false;
    }
}

/* ---- hui_graph_fit_screen ---- */

void hui_graph_fit_screen(hui_graph_view *view,
                          const hui_graph_node *nodes, int n_nodes,
                          hui_rect canvas_r)
{
    if (n_nodes <= 0 || !view) return;

    float min_x =  1e9f, min_y =  1e9f;
    float max_x = -1e9f, max_y = -1e9f;

    for (int i = 0; i < n_nodes; i++) {
        float nx = nodes[i].pos.x;
        float ny = nodes[i].pos.y;
        float nw = (float)HUI__GRAPH_NODE_W;
        float nh = (float)hui__graph_node_h(&nodes[i]);
        if (nx       < min_x) min_x = nx;
        if (ny       < min_y) min_y = ny;
        if (nx + nw  > max_x) max_x = nx + nw;
        if (ny + nh  > max_y) max_y = ny + nh;
    }

    float content_w = max_x - min_x;
    float content_h = max_y - min_y;
    if (content_w < 1.0f) content_w = 1.0f;
    if (content_h < 1.0f) content_h = 1.0f;

    float margin = 0.10f;
    float avail_w = (float)canvas_r.w * (1.0f - margin * 2.0f);
    float avail_h = (float)canvas_r.h * (1.0f - margin * 2.0f);

    float zoom_x = avail_w / content_w;
    float zoom_y = avail_h / content_h;
    float zoom   = zoom_x < zoom_y ? zoom_x : zoom_y;
    zoom = hui_clampf(zoom, 0.25f, 4.0f);

    /* Center the content */
    float cx = (min_x + max_x) * 0.5f;
    float cy = (min_y + max_y) * 0.5f;

    view->zoom  = zoom;
    view->pan.x = (float)canvas_r.w * 0.5f - cx * zoom;
    view->pan.y = (float)canvas_r.h * 0.5f - cy * zoom;
    view->_initialized = true;   /* prevent hui_graph_begin from overriding */
}

/* ---- hui_graph_auto_layout ---- */

void hui_graph_auto_layout(hui_graph_node *nodes, int n_nodes,
                           const hui_graph_link *links, int n_links)
{
    if (n_nodes <= 0) return;

    /* Compute depth (column) for each node via BFS from roots */
    int depth[HUI_GRAPH_MAX_NODES];
    int row[HUI_GRAPH_MAX_NODES];     /* row within column */
    int col_count[HUI_GRAPH_MAX_NODES]; /* nodes per column */
    int id_to_idx[HUI_GRAPH_MAX_NODES]; /* node id → array index (id < HUI_GRAPH_MAX_NODES) */

    memset(depth,     -1, sizeof(int) * n_nodes);
    memset(col_count,  0, sizeof(int) * n_nodes);
    memset(id_to_idx, -1, sizeof(id_to_idx));

    /* Build id→index map */
    for (int i = 0; i < n_nodes; i++) {
        int id = nodes[i].id;
        if (id > 0 && id < HUI_GRAPH_MAX_NODES)
            id_to_idx[id] = i;
    }

    /* Find nodes that have no incoming links (roots) */
    bool has_input[HUI_GRAPH_MAX_NODES];
    memset(has_input, 0, sizeof(bool) * n_nodes);
    for (int i = 0; i < n_links; i++) {
        int id = links[i].to_node;
        if (id > 0 && id < HUI_GRAPH_MAX_NODES && id_to_idx[id] >= 0)
            has_input[id_to_idx[id]] = true;
    }

    /* BFS queue */
    int queue[HUI_GRAPH_MAX_NODES];
    int qhead = 0, qtail = 0;

    /* Seed roots */
    for (int i = 0; i < n_nodes; i++) {
        if (!has_input[i]) {
            depth[i] = 0;
            queue[qtail++] = i;
        }
    }

    /* BFS */
    while (qhead < qtail) {
        int idx = queue[qhead++];
        int cur_depth = depth[idx];
        int node_id = nodes[idx].id;

        for (int i = 0; i < n_links; i++) {
            if (links[i].from_node != node_id) continue;
            int tid = links[i].to_node;
            if (tid <= 0 || tid >= HUI_GRAPH_MAX_NODES) continue;
            int tidx = id_to_idx[tid];
            if (tidx < 0) continue;
            if (depth[tidx] < cur_depth + 1) {
                depth[tidx] = cur_depth + 1;
                queue[qtail++] = tidx;
            }
        }
    }

    /* Assign depth 0 to any still-unvisited nodes (isolated) */
    for (int i = 0; i < n_nodes; i++)
        if (depth[i] < 0) depth[i] = 0;

    /* Assign rows within each column */
    memset(col_count, 0, sizeof(int) * n_nodes);
    for (int i = 0; i < n_nodes; i++) {
        int d = depth[i];
        if (d < 0 || d >= HUI_GRAPH_MAX_NODES) d = 0;
        row[i] = col_count[d]++;
    }

    /* Set positions */
    for (int i = 0; i < n_nodes; i++) {
        nodes[i].pos.x = (float)(depth[i] * (HUI__GRAPH_NODE_W + 60));
        nodes[i].pos.y = (float)(row[i]   * (HUI__GRAPH_NODE_TITLE_H + HUI__GRAPH_NODE_SLOT_H * 4 + 30));
    }
}

/* ---- hui_graph_minimap ---- */

void hui_graph_minimap(hui_rect r, const hui_graph_node *nodes, int n_nodes,
                       const hui_graph_view *view)
{
    if (r.w < 16 || r.h < 16) return;

    /* Background */
    hui_color bg = CUM_BG2;
    bg.a = 220;
    hui_rect_fill(r, bg, 4);
    hui_rect_outline(r, CUM_BG3, 4);

    if (n_nodes <= 0 || !view) return;

    /* Compute bounding box of all nodes */
    float min_x =  1e9f, min_y =  1e9f;
    float max_x = -1e9f, max_y = -1e9f;
    for (int i = 0; i < n_nodes; i++) {
        float nx = nodes[i].pos.x;
        float ny = nodes[i].pos.y;
        float nw = (float)HUI__GRAPH_NODE_W;
        float nh_f = (float)hui__graph_node_h(&nodes[i]);
        if (nx      < min_x) min_x = nx;
        if (ny      < min_y) min_y = ny;
        if (nx + nw > max_x) max_x = nx + nw;
        if (ny + nh_f > max_y) max_y = ny + nh_f;
    }

    float span_x = max_x - min_x;
    float span_y = max_y - min_y;
    if (span_x < 1.0f) span_x = 1.0f;
    if (span_y < 1.0f) span_y = 1.0f;

    int pad = 4;
    int mw = r.w - pad * 2;
    int mh = r.h - pad * 2;

    float scale_x = (float)mw / span_x;
    float scale_y = (float)mh / span_y;
    float scale   = scale_x < scale_y ? scale_x : scale_y;

    /* Draw nodes as tiny rects */
    for (int i = 0; i < n_nodes; i++) {
        float nx = (nodes[i].pos.x - min_x) * scale;
        float ny = (nodes[i].pos.y - min_y) * scale;
        float nw = (float)HUI__GRAPH_NODE_W * scale;
        float nh_f = (float)hui__graph_node_h(&nodes[i]) * scale;
        if (nw < 2.0f) nw = 2.0f;
        if (nh_f < 2.0f) nh_f = 2.0f;

        hui_rect nr = hui_rect_make(
            r.x + pad + (int)nx,
            r.y + pad + (int)ny,
            (int)nw,
            (int)nh_f
        );
        hui_color nc = nodes[i].selected ? CUM_ACCENT : CUM_BG4;
        hui_rect_fill(nr, nc, 0);
    }

    /* Draw viewport rect */
    /* The canvas rect would be needed here — approximate using hui__graph_rect if available */
    /* Viewport in canvas space: top-left = (-pan.x/zoom, -pan.y/zoom),
     * size = (canvas_w/zoom, canvas_h/zoom).
     * We don't have canvas_w here, so use a nominal 800x600. */
    {
        float nominal_w = 800.0f;
        float nominal_h = 600.0f;
        float vp_x = (-view->pan.x / view->zoom - min_x) * scale;
        float vp_y = (-view->pan.y / view->zoom - min_y) * scale;
        float vp_w = (nominal_w / view->zoom) * scale;
        float vp_h = (nominal_h / view->zoom) * scale;

        hui_rect vr = hui_rect_make(
            r.x + pad + (int)vp_x,
            r.y + pad + (int)vp_y,
            (int)vp_w,
            (int)vp_h
        );
        hui_color vc = CUM_FG3;
        vc.a = 120;
        hui_rect_outline(vr, vc, 0);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* HUI_GRAPH_IMPLEMENTATION */

#endif /* HUI_GRAPH_H */
