/*
 * hui_widgets.h — Widget layer for hui
 *
 * Provides:
 *   hui_progress_bar()     — filled progress bar, auto-rounded
 *   hui_text_wrap()        — word-wrapped text, returns total height
 *   hui_panel_begin/end()  — bordered panel with optional title + layout cursor
 *   hui_panel_row()        — allocate next content row, advance cursor
 *   hui_panel_peek()       — peek at next row rect without advancing
 *   hui_panel_cursor_y()   — query current cursor Y
 *   hui_panel_set_cursor() — override cursor Y (for custom layouts)
 *
 * Auto-included by hui.h after hui_draw.h. Requires HUI_IMPLEMENTATION
 * to be defined in the same translation unit as hui.h.
 *
 * Usage:
 *   hui_panel_begin("My Panel", hui_rect_make(10,10,200,300), 4);
 *   hui_rect r = hui_panel_row(20);           // allocate 20px row
 *   hui_progress_bar(r, 0.75f, CUM_ACCENT, CUM_BG3);
 *   hui_text_wrap(r.x, r.y, r.w, "some long text...", CUM_FG);
 *   hui_panel_end();
 */

#ifndef HUI_WIDGETS_H
#define HUI_WIDGETS_H

#include <stdbool.h>
#include <math.h>
#include "hui_math.h"
#include "hui_cum.h"
#include "hui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Standard row heights — use with hui_panel_row() ---- */
/** @brief 22 px — fits buttons, toggles, tab bars, collapsibles. */
#ifndef HUI_ROW_BTN
#  define HUI_ROW_BTN     22
#endif
/** @brief 22 px — fits float/int slider track with text inside. */
#ifndef HUI_ROW_SLIDER
#  define HUI_ROW_SLIDER  22
#endif
/** @brief 14 px — single-line text label (one glyph row + padding). */
#ifndef HUI_ROW_LABEL
#  define HUI_ROW_LABEL   14
#endif
/** @brief 24 px — text input field with cursor; extra height for click target. */
#ifndef HUI_ROW_INPUT
#  define HUI_ROW_INPUT   24
#endif
/** @brief 22 px — tab bar header row (same as HUI_ROW_BTN). */
#ifndef HUI_ROW_TAB
#  define HUI_ROW_TAB     22
#endif
/** @brief 10 px — thin filled progress bar track. */
#ifndef HUI_ROW_PROG
#  define HUI_ROW_PROG    10
#endif
/** @brief 1 px — hairline separator; pass to hui_panel_row() then hui_separator(). */
#ifndef HUI_ROW_SEP
#  define HUI_ROW_SEP      1
#endif
/** @brief 4 px — small vertical breathing room; pass to hui_panel_spacing(). */
#ifndef HUI_ROW_SM_GAP
#  define HUI_ROW_SM_GAP   4
#endif
/** @brief 10 px — large section break; pass to hui_panel_spacing(). */
#ifndef HUI_ROW_LG_GAP
#  define HUI_ROW_LG_GAP  10
#endif

/* ---- Alignment ---- */
/** @brief Text / content alignment within a rect. */
typedef enum {
    /** @brief Flush to the left edge of the rect. */
    HUI_ALIGN_LEFT   = 0,
    /** @brief Horizontally centred inside the rect. */
    HUI_ALIGN_CENTER,
    /** @brief Flush to the right edge of the rect. */
    HUI_ALIGN_RIGHT
} hui_align;

/* ---- Widget visual state ---- */
typedef enum {
    HUI_STATE_NORMAL   = 0,
    HUI_STATE_HOVER,
    HUI_STATE_ACTIVE,
    HUI_STATE_FOCUS,
    HUI_STATE_SELECTED,
    HUI_STATE_DISABLED,
    HUI_STATE_ERROR,
} hui_widget_state;

/* ---- Declarations ---- */

/* Filled progress bar. val in [0,1]. Auto-rounds to r.h/2 radius. */
void hui_progress_bar(hui_rect r, float val, hui_color fill, hui_color track);

/* Word-wrap text into max_w pixels. Returns total height rendered. */
int  hui_text_wrap(int x, int y, int max_w, const char *text, hui_color c);

/** @brief Open a bordered panel with optional title bar and layout cursor. @param title header text, or NULL for no title bar. @param r screen rect for the whole panel. @param rounding corner radius in pixels (0 = square). */
void     hui_panel_begin(const char *title, hui_rect r, uint8_t rounding);
/** @brief Close the most recently opened panel and pop its clip rect. */
void     hui_panel_end(void);
/** @brief Allocate the next content row and advance the cursor. @param h row height in pixels. @return clipped row rect for placing widgets. */
hui_rect hui_panel_row(int h);
/** @brief Return the next row rect without moving the cursor. @param h row height in pixels. @return same rect hui_panel_row() would return. */
hui_rect hui_panel_peek(int h);
/** @brief Current Y position of the layout cursor (top of next row). */
int      hui_panel_cursor_y(void);
/** @brief Override the cursor Y, e.g. to place two widgets at the same height. @param y absolute screen Y to resume layout from. */
void     hui_panel_set_cursor(int y);

/** @brief Divide a row rect into n equal columns separated by gap pixels. @param r source row rect. @param n number of columns. @param gap pixel gap between columns. @param out array of at least n hui_rect values written in left-to-right order. */
static inline void hui_panel_row_split(hui_rect r, int n, int gap, hui_rect *out) {
    int total_gap = gap * (n - 1);
    int cell_w    = (r.w - total_gap) / n;
    for (int i = 0; i < n; i++)
        out[i] = hui_rect_make(r.x + i * (cell_w + gap), r.y, cell_w, r.h);
}

/* Widget state color (CUM-based) */
hui_color hui_state_color(hui_widget_state s);
/* Focus ring drawn just outside a rect */
void      hui_draw_focus_ring(hui_rect r, uint8_t rounding);

/** @brief Draw aligned text inside a rect. @param text string to draw. @param r bounding rect; text is clipped to it. @param align left/center/right alignment. @param c text color. */
void hui_label(const char *text, hui_rect r, hui_align align, hui_color c);

/** @brief Clickable button with label. @param label text shown inside the button. @param r screen rect. @return true on the frame the button is clicked (LMB down, was up last frame). */
bool hui_button(const char *label, hui_rect r);

/** @brief Draw a horizontal rule through the vertical centre of rect r. @param r row rect; typically from hui_panel_row(HUI_ROW_SEP). */
void hui_separator(hui_rect r);

/* Grid layout — equal-width columns with gap */
void     hui_grid_begin(hui_rect area, int cols, int gap);
hui_rect hui_grid_cell(int h);   /* allocate next cell, advance (wraps to new row) */
void     hui_grid_end(void);

/* Ghost / drag visual helpers */
void hui_ghost_rect(hui_rect r, hui_color c);           /* translucent + dashed border */
void hui_draw_grid(hui_rect r, int step, hui_color c);  /* background grid lines */
void hui_draw_snap_highlight(hui_rect r, hui_color c);  /* snap-target cell indicator */

/** @brief Square tick box with right-side label. @param label text shown beside the box. @param r row rect. @param val boolean toggled in place. @return true on the frame the value changes. */
bool hui_checkbox(const char *label, hui_rect r, bool *val);

/** @brief iOS-style pill toggle switch. @param label text shown beside the pill. @param r row rect. @param val boolean toggled in place. @return true on the frame the value changes. */
bool hui_toggle(const char *label, hui_rect r, bool *val);

/** @brief Horizontal float slider mapping mouse X to [mn, mx]. @param label text shown inside the track. @param r row rect. @param val pointer updated in place. @param mn minimum value. @param mx maximum value. @return true if the value changed this frame. */
bool hui_slider_f(const char *label, hui_rect r, float *val, float mn, float mx);
/** @brief Horizontal integer slider mapping mouse X to [mn, mx]. @param label text shown inside the track. @param r row rect. @param val pointer updated in place. @param mn minimum value. @param mx maximum value. @return true if the value changed this frame. */
bool hui_slider_i(const char *label, hui_rect r, int   *val, int   mn, int   mx);

/** @brief Drag-to-edit float — hold LMB and move horizontally to change value. @param label text shown in the box. @param r row rect. @param val pointer updated in place. @param speed change per pixel dragged. @return true while the value is changing. */
bool hui_drag_f(const char *label, hui_rect r, float *val, float speed);

/* Panel layout helpers */
void hui_panel_same_line(void);     /* place next row on same Y as previous */
/** @brief Insert a blank vertical gap without allocating a widget row. @param px gap height in pixels; use HUI_ROW_SM_GAP / HUI_ROW_LG_GAP. */
void hui_panel_spacing(int px);
void hui_panel_indent(int px);      /* increase left indent */
void hui_panel_unindent(int px);    /* decrease left indent (floors at 0) */

/* Text input state — caller owns one per text field */
typedef struct {
    int  cursor;   /* insertion point [0, strlen(buf)] */
    bool focused;  /* true when receiving keyboard input */
} hui_text_state;

/** @brief Single-line text field with cursor. @param r row rect. @param st caller-owned edit state (cursor pos + focus flag). @param buf null-terminated string buffer edited in place. @param bufsz total byte capacity of buf including the null terminator. @return true on any frame the buffer content changes. */
bool hui_text_input(hui_rect r, hui_text_state *st, char *buf, int bufsz);

/** @brief Clickable section header that toggles a collapsed/expanded state. @param title header text; an arrow indicates open/closed. @param r row rect for the header itself (not the content area). @param open caller-owned flag toggled on click. @return current value of *open after potential toggle. */
bool hui_collapsible(const char *title, hui_rect r, bool *open);

/** @brief Open a vertically scrollable viewport. @param r screen rect including the scrollbar track. @param content_h total virtual height of the content in pixels. @param scroll_y caller-owned scroll offset (init to 0); updated by wheel and thumb drag. @return clipped content rect (excludes scrollbar column); subtract *scroll_y from widget Y coords inside. */
hui_rect hui_scroll_begin(hui_rect r, int content_h, int *scroll_y);
/** @brief Close the scrollable region opened by hui_scroll_begin(). */
void     hui_scroll_end(void);

/** @brief Row of clickable tab buttons. @param r row rect for the tab bar. @param labels array of tab title strings. @param n number of tabs. @param active caller-owned selected index (0-based); updated on click. @return true on the frame the selected tab changes. */
bool hui_tab_bar(hui_rect r, const char **labels, int n, int *active);

/* Color and gradient widgets require float math — omitted under HUI_PROFILE_MCU.
 * Define HUI_NO_FLOAT_WIDGETS before including hui.h to suppress them. */
#ifndef HUI_NO_FLOAT_WIDGETS

/* Color picker — 2D HSV saturation/value square + hue bar on the right.
 * r is the total bounding rect (hue bar is right 12px; SV square fills the rest).
 * Returns true when color changes. */
bool hui_color_picker(hui_rect r, hui_color *color);

/* Color panel — SV square only (no hue bar). Useful inside a larger layout
 * that provides a separate hue control. Returns true when color changes. */
bool hui_color_panel(hui_rect r, hui_color *color);

#endif /* HUI_NO_FLOAT_WIDGETS */

/* Combo box — closed shows current item + arrow button; click opens dropdown list below.
 * *open is caller-owned open/close state. *active is selected index (0-based).
 * Returns true when selection changes. */
bool hui_combo_box(const char *label, hui_rect r, const char **items, int n,
                   int *active, bool *open);

/* Dropdown box — like combo_box but open state shows a text filter input at the top.
 * Type to filter items; select filtered match or Escape to close.
 * Returns true when selection changes. */
bool hui_dropdown_box(const char *label, hui_rect r, const char **items, int n,
                      int *active, bool *open);

/* Spinner — integer field with − / + buttons on left/right sides.
 * Click buttons or drag center to change value. Returns true on change. */
bool hui_spinner(const char *label, hui_rect r, int *val, int step, int mn, int mx);

/* Value box — shows an integer; click to enter edit mode, type to change.
 * *edit is caller-owned edit state (false = display, true = editing). */
bool hui_value_box(const char *label, hui_rect r, int *val, int mn, int mx, bool *edit);

/* List view — scrollable list of strings. *active is selected index (-1 = none).
 * *scroll_y is caller-owned scroll offset. Returns true when selection changes. */
bool hui_list_view(hui_rect r, const char **items, int n, int *active, int *scroll_y);

/* Zoom slider — dual-handle range slider. Drag center to pan, drag handles to resize.
 * lo/hi are the current sub-range within [total_min, total_max]. Returns true on change. */
bool hui_zoom_slider_f(hui_rect r, float *lo, float *hi, float total_min, float total_max);

/** @brief Draw a small floating label near the cursor. @param text tooltip string. Call only when the trigger rect is hovered, e.g. inside if(hui_is_hovered(r)). */
void hui_tooltip(const char *text);

/* Badge — small filled label at top-right corner of rect r */
void hui_badge(hui_rect r, const char *text, hui_color c);

/* FPS overlay — draws "XX.X fps" at top-right of rect r with a dark background pill.
 * Call once per frame anywhere between begin/end. */
void hui_fps_overlay(hui_rect r);

/* Validation pulse color — returns a color that flashes trigger_c when age_frames==0
 * and fades back to base_c over decay_frames. Use for brief visual feedback on change.
 * age_frames: frames since the event (increment each frame; reset to 0 on trigger).
 * Example: static int pulse = 0; if(changed) pulse = 0; else pulse++;
 *          hui_rect_fill(r, hui_pulse_color(CUM_BG2, CUM_OK, pulse, 12), 4); */
hui_color hui_pulse_color(hui_color base_c, hui_color trigger_c,
                          int age_frames, int decay_frames);

/* Generic popup overlay — dims background, draws popup rect, closes on outside click.
 * Returns true while open. Must be paired with hui_popup_end(). */
bool hui_popup_begin(hui_rect r, bool *open);
void hui_popup_end(void);

/* Menubar + dropdown menus.
 * Pattern: always call hui_menu_begin/end around items; hui_menu_item no-ops when closed.
 *
 *   hui_menubar_begin(r);
 *   hui_menu_begin("File");
 *     if (hui_menu_item("New",  "Ctrl+N")) { ... }
 *     if (hui_menu_item("Quit", "Alt+F4")) { running = false; }
 *   hui_menu_end();
 *   hui_menubar_end();
 */
/** @brief Start a horizontal menu bar occupying rect r. Must be paired with hui_menubar_end(). @param r screen rect for the bar background (typically full-width, ~22 px tall). */
void hui_menubar_begin(hui_rect r);
/** @brief Add a top-level menu button to the active menu bar. @param label menu title shown in the bar. @return true while the dropdown is open; use this to guard hui_menu_item() calls. */
bool hui_menu_begin(const char *label);
/** @brief Add a clickable item to the open dropdown menu. @param label item text. @param shortcut optional shortcut hint shown right-aligned, or NULL. @return true on the frame the item is clicked. */
bool hui_menu_item(const char *label, const char *shortcut);
void hui_menu_separator(void);
/** @brief Close the dropdown opened by hui_menu_begin(). */
void hui_menu_end(void);
/** @brief Finish and draw the menu bar opened by hui_menubar_begin(). */
void hui_menubar_end(void);

/* Context menu — right-click inside trigger_r opens a floating menu at cursor.
 * *open is caller-owned (init to false). Always call hui_context_menu_end().
 *
 *   hui_context_menu_begin(canvas_rect, &ctx_open);
 *     if (hui_menu_item("Paste", NULL)) { ... }
 *   hui_context_menu_end();
 */
bool hui_context_menu_begin(hui_rect trigger_r, bool *open);
void hui_context_menu_end(void);

#ifndef HUI_NO_FLOAT_WIDGETS
/* Gradient — a sorted list of color stops in [0, 1].
 * Use hui_gradient_init() to set up a simple two-stop gradient.
 * hui_gradient_at() linearly interpolates between stops. */
#ifndef HUI_GRADIENT_MAX_STOPS
#  define HUI_GRADIENT_MAX_STOPS 16
#endif
typedef struct {
    float     pos[HUI_GRADIENT_MAX_STOPS];  /* stop positions [0,1], kept sorted */
    hui_color col[HUI_GRADIENT_MAX_STOPS];  /* stop colors */
    int       n;                            /* number of stops */
    int       sel;                          /* selected stop index (-1 = none) */
} hui_gradient;

void      hui_gradient_init(hui_gradient *g, hui_color a, hui_color b);
hui_color hui_gradient_at(const hui_gradient *g, float t);

/* Gradient editor — horizontal ramp with draggable stop handles.
 * Click on ramp to add a stop. RMB on a handle (or Delete key when selected) removes it.
 * r should be at least 24px tall; bottom 16px is the handle row. */
bool hui_gradient_edit(hui_rect r, hui_gradient *g);
#endif /* HUI_NO_FLOAT_WIDGETS */

/* List view (multi-select) — same as hui_list_view but with a per-item bool selected[].
 * Click toggles selection. *focus is the keyboard-cursor row (-1 = none).
 * Returns true when any selection changes. */
bool hui_list_view_ex(hui_rect r, const char **items, int n,
                      bool *selected, int *focus, int *scroll_y);

/* Drag handle — circular hit target at given screen position.
 * Each handle needs a unique non-zero integer id.
 * While dragging, adds mouse delta to x/y; if snap_grid > 0, snaps to grid.
 * Returns true while the value changed. */
bool hui_drag_handle(int id, int *x, int *y, int radius, int snap_grid);

/* Resize handle — bottom-right corner grip drawn inside body_r.
 * Adds mouse delta to w/h, clamped to [min_w, min_h].
 * Returns true while the size changed. */
bool hui_resize_handle(int id, int *w, int *h, hui_rect body_r, int min_w, int min_h);

/* Returns true while widget with given id is being dragged. */
bool hui_is_dragging(int id);

/* Bitmap view — scale-blit a raw RGB (3 bytes/px) buffer into rect r.
 * Available on headless-derived backends (HEADLESS, X11, SDL2).
 * Falls back to a placeholder on other backends. */
void hui_bitmap_view(hui_rect r, const uint8_t *rgb, int iw, int ih);

/* ---- Curve editor ---- */
/*
 * Piecewise curve over a [0,1] × [0,1] domain with draggable control points.
 * Supports linear interpolation and smooth Catmull-Rom spline.
 *
 * Usage:
 *   static hui_curve cv; // zero-initialise or call hui_curve_init()
 *   hui_curve_init(&cv);
 *   // per frame:
 *   hui_curve_edit(r, &cv, CUM_ACCENT);
 *   float y = hui_curve_eval(&cv, x);  // evaluate at any x in [0,1]
 */
#ifndef HUI_CURVE_MAX_POINTS
#  define HUI_CURVE_MAX_POINTS 32
#endif

typedef enum {
    HUI_CURVE_LINEAR = 0,  /* piecewise linear between control points */
    HUI_CURVE_SMOOTH,      /* Catmull-Rom spline through control points */
} hui_curve_type;

typedef struct {
    float          x[HUI_CURVE_MAX_POINTS];   /* control point x, sorted [0,1] */
    float          y[HUI_CURVE_MAX_POINTS];   /* control point y [0,1] */
    int            n;                          /* number of control points */
    int            sel;                        /* selected point index (-1 = none) */
    hui_curve_type type;
} hui_curve;

/* Initialise with two default endpoints: (0,0) and (1,1). */
void  hui_curve_init(hui_curve *c);

/* Evaluate curve at x ∈ [0,1]. Returns y clamped to [0,1]. */
float hui_curve_eval(const hui_curve *c, float x);

/* Interactive curve editor widget.
 *   - LMB click on empty area: insert point
 *   - LMB drag: move selected point
 *   - RMB on point / Delete key when selected: remove (min 2 points kept)
 *   - col: color for the curve line
 * Returns true when the curve changes this frame. */
bool  hui_curve_edit(hui_rect r, hui_curve *c, hui_color col);

/* ---- Sequencer ---- */
/*
 * Frame-based timeline widget with named tracks and draggable items.
 *
 * Usage:
 *   static hui_seq_state seq = { .frame_min=0, .frame_max=120, .current_frame=0,
 *                                ._drag_item=-1, ._drag_track=-1 };
 *   static hui_seq_track tracks[2] = {
 *       { "Camera", { {0,30,"Shot A",0}, {50,80,"Shot B",0} }, 2, CUM_BLUE },
 *       { "Audio",  { {0,120,"BG Music",0} },                  1, CUM_GREEN },
 *   };
 *   hui_sequencer(hui_rect_make(10,10,700,160), tracks, 2, &seq);
 */

/* A sequencer item: one segment on a track. */
typedef struct {
    int  start_frame;   /* start (inclusive) */
    int  end_frame;     /* end (exclusive) */
    char label[32];
    hui_color color;    /* fill color; 0 → use track default */
} hui_seq_item;

/* A sequencer track (one row). */
typedef struct {
    char         name[32];    /* track label shown on left */
    hui_seq_item items[32];   /* max 32 items per track */
    int          item_count;
    hui_color    color;       /* default item color */
    bool         expanded;    /* reserved for future multi-row */
} hui_seq_track;

/* Sequencer state (user-owned, must persist across frames). */
typedef struct {
    int  frame_min;     /* first visible frame */
    int  frame_max;     /* last visible frame (exclusive) */
    int  current_frame; /* playhead position */
    int  _drag_item;    /* internal: item being dragged (-1=none) */
    int  _drag_track;   /* internal: track of dragged item */
    int  _drag_offset;  /* internal: offset within item where drag started */
    bool _drag_right;   /* internal: dragging right edge (resize) vs body (move) */
} hui_seq_state;

/* Draw a sequencer widget.
 * r: screen rect. tracks/n_tracks: track array. state: persistent state.
 * Returns true if current_frame or any item changed this frame.
 * Initialise state->frame_min/max before first call.
 * Note: no vertical scroll — if n_tracks*28 > available height, tracks are clipped. */
bool hui_sequencer(hui_rect r, hui_seq_track *tracks, int n_tracks, hui_seq_state *state);

/* ---- Light rig ---- */
/*
 * Radial 2D canvas for 3D light positioning.
 * Each light is a point on a unit hemisphere seen from above.
 * Position encoded as azimuth (0°=north/forward, clockwise) and
 * elevation (0°=horizon, 90°=zenith).
 *
 * Projection to screen disk: u = sin(az)*cos(el), v = -cos(az)*cos(el)
 * → (0,0) = zenith (center), |(u,v)| = 1 = horizon (edge of disk).
 *
 * Usage:
 *   static hui_light lights[3] = {
 *       {45.0f, 60.0f, CUM_YELL, true},
 *       {200.0f, 30.0f, CUM_BLUE, true},
 *       {0.0f, 0.0f, HUI_RGB(255,255,255), false},
 *   };
 *   if (hui_light_rig(r, lights, 3)) { ... }
 */
#ifndef HUI_LIGHT_MAX
#  define HUI_LIGHT_MAX 8
#endif

typedef struct {
    float     az;    /* azimuth degrees [0, 360) — 0=forward/north, clockwise */
    float     el;    /* elevation degrees [0, 90] — 0=horizon, 90=zenith */
    hui_color col;   /* light color / tint */
    bool      on;    /* enabled flag (dims orb when false) */
} hui_light;

/* Radial hemisphere canvas. LMB drag to reposition lights.
 * Returns true when any light position changes. */
bool hui_light_rig(hui_rect r, hui_light *lights, int n);

/* ---- Implementation ---- */

#ifdef HUI_IMPLEMENTATION

/* ---- Named drag state ---- */
static int  hui__drag_id;
static bool hui__drag_active;

/* ---- Progress bar ---- */

void hui_progress_bar(hui_rect r, float val, hui_color fill, hui_color track) {
    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;
    uint8_t rad = (uint8_t)(r.h / 2);
    hui_rect_fill(r, track, rad);
    if (val > 0.0f) {
        int fw = (int)(r.w * val);
        int min_fw = rad * 2;   /* minimum fill width to keep rounded ends */
        if (fw < min_fw && r.w >= min_fw) fw = min_fw;
        hui_rect_fill(hui_rect_make(r.x, r.y, fw, r.h), fill, rad);
    }
}

/* ---- Word-wrapped text ---- */

int hui_text_wrap(int x, int y, int max_w, const char *text, hui_color c) {
    if (!text || max_w <= 0) return 0;
    const int cw = HUI_FONT_ADVANCE;  /* pixels per char (glyph width + 1px gap) */
    const int lh = HUI_FONT_H + 2;   /* pixels per line (glyph height + 2px gap) */
    int  cy = y;
    char lbuf[256];
    int  llen = 0, lpx = 0;
    const char *p = text;

    for (;;) {
        if (*p == '\n' || *p == '\0') {
            /* flush line buffer */
            if (llen > 0) {
                lbuf[llen] = '\0';
                hui_text(x, cy, lbuf, c);
                cy += lh;
                llen = 0; lpx = 0;
            }
            if (*p == '\0') break;
            p++;        /* skip '\n' */
            continue;
        }

        /* find next word */
        const char *ws = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        int wlen = (int)(p - ws);
        if (wlen == 0) { if (*p == ' ') p++; continue; }
        int wpx = wlen * cw;

        /* soft-wrap: start new line if word overflows (skip wrap if at line start) */
        int space_px = (llen > 0) ? cw : 0;
        if (llen > 0 && lpx + space_px + wpx > max_w) {
            lbuf[llen] = '\0';
            hui_text(x, cy, lbuf, c);
            cy += lh;
            llen = 0; lpx = 0;
            space_px = 0;
        }

        /* append space + word to line buffer */
        if (space_px && llen < 254) { lbuf[llen++] = ' '; lpx += cw; }
        int copy = wlen;
        if (llen + copy > 254) copy = 254 - llen;
        for (int i = 0; i < copy; i++) lbuf[llen++] = ws[i];
        lpx += wpx;

        if (*p == ' ') p++;
    }

    return cy - y;
}

/* ---- Panel ---- */

#define HUI__PANEL_PAD     4
#define HUI__PANEL_TITLE_H 16
#define HUI__PANEL_MAX     8

typedef struct {
    hui_rect rect;
    int      body_y;        /* Y where content starts (after title bar) */
    int      cursor_y;      /* next row Y */
    int      inner_x;       /* rect.x + PAD */
    int      inner_w;       /* rect.w - 2*PAD */
    int      indent;        /* extra left offset from inner_x */
    /* same_line tracking */
    int      last_end_x;    /* right edge of last allocated row */
    int      last_row_y;    /* Y of last allocated row */
    int      last_row_h;    /* height of last allocated row */
    bool     same_line;     /* if true, next row() shares Y with previous */
} hui__panel_ctx;

static hui__panel_ctx hui__panel_stk[HUI__PANEL_MAX];
static int            hui__panel_sp;
static int            hui__panel_overflow;

void hui_panel_begin(const char *title, hui_rect r, uint8_t rounding) {
    /* background + border */
    hui_rect_fill(r, CUM_BG2, rounding);
    hui_rect_outline(r, CUM_BG3, rounding);

    int body_y = r.y;
    if (title && *title) {
        /* title bar — drawn flat; outer rounded corners of panel bg peek through
         * at small rounding values (acceptable until per-corner rounding is added) */
        hui_rect_fill(hui_rect_make(r.x, r.y, r.w, HUI__PANEL_TITLE_H), CUM_BG0, 0);
        hui_text(r.x + HUI__PANEL_PAD,
                 r.y + (HUI__PANEL_TITLE_H - HUI_FONT_H) / 2,
                 title, CUM_FG);
        /* separator line */
        hui_line(r.x + 1, r.y + HUI__PANEL_TITLE_H,
                 r.x + r.w - 2, r.y + HUI__PANEL_TITLE_H,
                 CUM_BG3, 1);
        body_y = r.y + HUI__PANEL_TITLE_H;
    }

    if (hui__panel_sp < HUI__PANEL_MAX) {
        hui_clip_push(hui_rect_make(r.x, body_y, r.w, r.y + r.h - body_y));
        hui__panel_ctx *ctx = &hui__panel_stk[hui__panel_sp++];
        ctx->rect       = r;
        ctx->body_y     = body_y;
        ctx->cursor_y   = body_y + HUI__PANEL_PAD;
        ctx->inner_x    = r.x + HUI__PANEL_PAD;
        ctx->inner_w    = r.w - HUI__PANEL_PAD * 2;
        ctx->indent     = 0;
        ctx->last_end_x = ctx->inner_x;
        ctx->last_row_y = ctx->cursor_y;
        ctx->last_row_h = 0;
        ctx->same_line  = false;
    } else {
        /* overflow — draw visible but layout/clip skipped; matched in panel_end */
        hui__panel_overflow++;
    }
}

void hui_panel_end(void) {
    if (hui__panel_overflow > 0) { hui__panel_overflow--; return; }
    if (hui__panel_sp > 0) { hui__panel_sp--; hui_clip_pop(); }
}

hui_rect hui_panel_peek(int h) {
    if (hui__panel_sp == 0) return hui_rect_make(0, 0, 0, 0);
    hui__panel_ctx *ctx = &hui__panel_stk[hui__panel_sp - 1];
    return hui_rect_make(ctx->inner_x + ctx->indent, ctx->cursor_y,
                         ctx->inner_w - ctx->indent, h);
}

hui_rect hui_panel_row(int h) {
    if (hui__panel_sp == 0) return hui_rect_make(0, 0, 0, 0);
    hui__panel_ctx *ctx = &hui__panel_stk[hui__panel_sp - 1];
    int sl = ctx->same_line ? 1 : 0;
    ctx->same_line = false;
    int x, y, w;
    if (sl) {
        x = ctx->last_end_x + HUI__PANEL_PAD;
        y = ctx->last_row_y;
        w = (ctx->inner_x + ctx->inner_w) - x;
        if (w < 0) w = 0;
        if (h > ctx->last_row_h) h = ctx->last_row_h;
    } else {
        x = ctx->inner_x + ctx->indent;
        y = ctx->cursor_y;
        w = ctx->inner_w - ctx->indent;
        ctx->cursor_y += h + HUI__PANEL_PAD;
    }
    ctx->last_end_x = x + w;
    ctx->last_row_y = y;
    ctx->last_row_h = h;
    return hui_rect_make(x, y, w, h);
}

int hui_panel_cursor_y(void) {
    if (hui__panel_sp == 0) return 0;
    return hui__panel_stk[hui__panel_sp - 1].cursor_y;
}

void hui_panel_set_cursor(int y) {
    if (hui__panel_sp > 0)
        hui__panel_stk[hui__panel_sp - 1].cursor_y = y;
}

/* ---- Widget state color ---- */

hui_color hui_state_color(hui_widget_state s) {
    switch (s) {
    case HUI_STATE_HOVER:    return CUM_BG4;
    case HUI_STATE_ACTIVE:   return CUM_BG1;
    case HUI_STATE_FOCUS:    return CUM_ACCENT;
    case HUI_STATE_SELECTED: { hui_color c = CUM_ACCENT; c.a = 80; return c; }
    case HUI_STATE_DISABLED: return CUM_BG3;
    case HUI_STATE_ERROR:    return CUM_ERR;
    default:                 return CUM_BG3;
    }
}

void hui_draw_focus_ring(hui_rect r, uint8_t rounding) {
    hui_rect outer = hui_rect_make(r.x - 2, r.y - 2, r.w + 4, r.h + 4);
    hui_rect_outline(outer, CUM_ACCENT, rounding > 0 ? rounding + 2 : 0);
}

/* ---- Label ---- */

void hui_label(const char *text, hui_rect r, hui_align align, hui_color c) {
    if (!text) return;
    int tw = hui_text_width(text);
    int tx, ty = r.y + (r.h - HUI_FONT_PIXEL_H) / 2;
    switch (align) {
    default:
    case HUI_ALIGN_LEFT:   tx = r.x; break;
    case HUI_ALIGN_CENTER: tx = r.x + (r.w - tw) / 2; break;
    case HUI_ALIGN_RIGHT:  tx = r.x + r.w - tw; break;
    }
    hui_text(tx, ty, text, c);
}

/* ---- Button ---- */

bool hui_button(const char *label, hui_rect r) {
    bool hov     = hui_is_hovered(r);
    bool act     = hov && (hui_g->io.mouse_btn & 1u);
    bool clicked = hov && (hui_g->io.mouse_btn & 1u) && !(hui_g->io.mouse_btn_prev & 1u);

    uint8_t  rad    = (uint8_t)(r.h / 3);
    hui_color bg     = act ? CUM_BG1 : (hov ? CUM_BG4 : CUM_BG3);
    hui_color border = (hov || act) ? CUM_ACCENT : CUM_BG4;

    hui_rect_fill(r, bg, rad);
    hui_rect_outline(r, border, rad);

    int tw = label ? hui_text_width(label) : 0;
    if (label && tw > 0)
        hui_text(r.x + (r.w - tw) / 2, r.y + (r.h - HUI_FONT_PIXEL_H) / 2, label, CUM_FG);

    return clicked;
}

/* ---- Separator ---- */

void hui_separator(hui_rect r) {
    int mid = r.y + r.h / 2;
    hui_line(r.x, mid, r.x + r.w, mid, CUM_BG3, 1);
}

/* ---- Grid layout ---- */

#define HUI__GRID_MAX 4

typedef struct {
    hui_rect area;
    int      cols, gap, col_w;
    int      cur_col;
    int      row_y, row_h;
} hui__grid_ctx;

static hui__grid_ctx hui__grid_stk[HUI__GRID_MAX];
static int           hui__grid_sp;
static int           hui__grid_overflow;

void hui_grid_begin(hui_rect area, int cols, int gap) {
    if (cols <= 0) return;
    if (hui__grid_sp >= HUI__GRID_MAX) { hui__grid_overflow++; return; }
    hui__grid_ctx *g = &hui__grid_stk[hui__grid_sp++];
    g->area    = area;
    g->cols    = cols;
    g->gap     = gap;
    g->col_w   = (cols > 1) ? (area.w - gap * (cols - 1)) / cols : area.w;
    g->cur_col = 0;
    g->row_y   = area.y;
    g->row_h   = 0;
}

hui_rect hui_grid_cell(int h) {
    if (hui__grid_sp == 0) return hui_rect_make(0, 0, 0, 0);
    hui__grid_ctx *g = &hui__grid_stk[hui__grid_sp - 1];
    int x = g->area.x + g->cur_col * (g->col_w + g->gap);
    hui_rect r = hui_rect_make(x, g->row_y, g->col_w, h);
    if (h > g->row_h) g->row_h = h;
    if (++g->cur_col >= g->cols) {
        g->cur_col = 0;
        g->row_y  += g->row_h + g->gap;
        g->row_h   = 0;
    }
    return r;
}

void hui_grid_end(void) {
    if (hui__grid_overflow > 0) { hui__grid_overflow--; return; }
    if (hui__grid_sp > 0) hui__grid_sp--;
}

/* ---- Ghost / snap helpers ---- */

void hui_ghost_rect(hui_rect r, hui_color c) {
    /* translucent fill */
    hui_rect_fill(r, (hui_color){c.r, c.g, c.b, 40}, 2);
    /* dashed border */
    hui_color border = {c.r, c.g, c.b, 180};
    const int dash = 5, skip = 3;
    for (int x = r.x; x < r.x + r.w; x += dash + skip) {
        int x2 = x + dash; if (x2 > r.x + r.w) x2 = r.x + r.w;
        hui_line(x, r.y,           x2, r.y,           border, 1);
        hui_line(x, r.y + r.h - 1, x2, r.y + r.h - 1, border, 1);
    }
    for (int y = r.y; y < r.y + r.h; y += dash + skip) {
        int y2 = y + dash; if (y2 > r.y + r.h) y2 = r.y + r.h;
        hui_line(r.x,           y, r.x,           y2, border, 1);
        hui_line(r.x + r.w - 1, y, r.x + r.w - 1, y2, border, 1);
    }
}

void hui_draw_grid(hui_rect r, int step, hui_color c) {
    if (step <= 0) return;
    for (int x = r.x + step; x < r.x + r.w; x += step)
        hui_line(x, r.y, x, r.y + r.h, c, 1);
    for (int y = r.y + step; y < r.y + r.h; y += step)
        hui_line(r.x, y, r.x + r.w, y, c, 1);
}

void hui_draw_snap_highlight(hui_rect r, hui_color c) {
    hui_rect_fill(r, (hui_color){c.r, c.g, c.b, 30}, 2);
    hui_rect_outline(r, (hui_color){c.r, c.g, c.b, 220}, 2);
}


/* ---- Checkbox ---- */

bool hui_checkbox(const char *label, hui_rect r, bool *val) {
    bool hov     = hui_is_hovered(r);
    bool clicked = hui_is_clicked(r, 0);
    if (clicked && val) *val = !*val;

    int box_sz = r.h;
    hui_rect box = hui_rect_make(r.x, r.y, box_sz, box_sz);
    hui_rect_fill(box, hov ? CUM_BG4 : CUM_BG3, 2);
    hui_rect_outline(box, hov ? CUM_ACCENT : CUM_BG4, 2);

    if (val && *val) {
        int pad = box_sz / 4;
        int mx = box.x + box_sz / 2 - 1;
        int by = box.y + box_sz - pad - 1;
        hui_line(box.x + pad, box.y + box_sz / 2, mx, by, CUM_ACCENT, 2);
        hui_line(mx, by, box.x + box_sz - pad, box.y + pad, CUM_ACCENT, 2);
    }

    if (label) {
        hui_rect lr = hui_rect_make(r.x + box_sz + 4, r.y, r.w - box_sz - 4, r.h);
        hui_label(label, lr, HUI_ALIGN_LEFT, hov ? CUM_FG : CUM_FG2);
    }
    return clicked;
}

/* ---- Toggle switch ---- */

bool hui_toggle(const char *label, hui_rect r, bool *val) {
    bool hov     = hui_is_hovered(r);
    bool clicked = hui_is_clicked(r, 0);
    if (clicked && val) *val = !*val;

    bool on        = val && *val;
    int  track_h   = r.h;
    int  track_w   = r.h * 2;
    hui_rect track = hui_rect_make(r.x, r.y, track_w, track_h);
    uint8_t  rad   = (uint8_t)(track_h / 2);

    hui_color track_col = on ? CUM_OK : CUM_BG3;
    if (hov) { track_col.r = (uint8_t)hui_min((int)track_col.r + 20, 255); }
    hui_rect_fill(track, track_col, rad);

    int pad    = 2;
    int thumb_d = track_h - pad * 2;
    int thumb_x = on ? track.x + track_w - pad - thumb_d : track.x + pad;
    hui_rect_fill(hui_rect_make(thumb_x, track.y + pad, thumb_d, thumb_d),
                  CUM_FG, (uint8_t)(thumb_d / 2));

    if (label) {
        hui_rect lr = hui_rect_make(r.x + track_w + 4, r.y, r.w - track_w - 4, r.h);
        hui_label(label, lr, HUI_ALIGN_LEFT, hov ? CUM_FG : CUM_FG2);
    }
    return clicked;
}

/* ---- Sliders (shared draw helper) ---- */

static void hui__slider_draw(hui_rect r, float t, int active, int hov,
                             const char *label, const char *value) {
    /* Property sliders use a stable label/value baseline and a quiet track.
     * This remains readable at fractional font scales and makes columns line
     * up across a whole inspector instead of baking text into the fill. */
    int track_h = 8;
    int track_y = r.y + r.h - track_h - 3;
    hui_rect track = hui_rect_make(r.x, track_y, r.w, track_h);
    uint8_t rad = (uint8_t)(track_h / 2);
    int fw = (int)(track.w * t);

    if (label) hui_text(r.x, r.y + 2, label, hov ? CUM_FG : CUM_FG2);
    if (value) {
        int vw = hui_text_width(value);
        hui_text(r.x + r.w - vw, r.y + 2, value, CUM_FG3);
    }
    hui_rect_fill(track, CUM_BG3, rad);
    if (fw > 0)
        hui_rect_fill(hui_rect_make(track.x, track.y, fw, track.h),
                      active ? CUM_ACTIVE : CUM_ACCENT, rad);
    hui_rect_outline(track, hov ? CUM_FG3 : CUM_BG4, rad);

    /* slim thumb grip at the fill edge — narrow so it never hides the text
     * (the old full-height disc washed out the centred label at large fonts) */
    int tx = track.x + fw;
    if (tx < track.x + 3) tx = track.x + 3;
    if (tx > track.x + track.w - 3) tx = track.x + track.w - 3;
    /* Illuminated hardware-style position marker: restrained at rest, with a
     * bright LED core only while hovered/active. */
    hui_circle_fill(tx, track.y + track.h / 2, hov || active ? 6 : 5, CUM_BG0);
    hui_circle_fill(tx, track.y + track.h / 2, hov || active ? 4 : 3,
                    active ? CUM_ACTIVE : (hov ? CUM_ACCENT : CUM_FG2));
    if(active) hui_circle_fill(tx,track.y+track.h/2,1,CUM_FG);
}

bool hui_slider_f(const char *label, hui_rect r, float *val, float mn, float mx) {
    if (!val || mn >= mx) return false;
    bool hov  = hui_is_hovered(r);
    bool act  = hov && (hui_g->io.mouse_btn & 1u);
    bool changed = false;

    if (act) {
        float t = hui_clampf((float)(hui_g->io.mouse_x - r.x) / (float)r.w, 0.0f, 1.0f);
        float nv = mn + t * (mx - mn);
        if (nv != *val) { *val = nv; changed = true; }
    }

    float t = hui_clampf((*val - mn) / (mx - mn), 0.0f, 1.0f);
    const char *value = hui_fmt("%.2f", *val);
    hui__slider_draw(r, t, (int)act, (int)hov, label, value);
    return changed;
}

bool hui_slider_i(const char *label, hui_rect r, int *val, int mn, int mx) {
    if (!val || mn >= mx) return false;
    bool hov  = hui_is_hovered(r);
    bool act  = hov && (hui_g->io.mouse_btn & 1u);
    bool changed = false;

    if (act) {
        float t  = hui_clampf((float)(hui_g->io.mouse_x - r.x) / (float)r.w, 0.0f, 1.0f);
        int   nv = hui_clamp(mn + (int)(t * (float)(mx - mn) + 0.5f), mn, mx);
        if (nv != *val) { *val = nv; changed = true; }
    }

    float t = hui_clampf((float)(*val - mn) / (float)(mx - mn), 0.0f, 1.0f);
    const char *value = hui_fmt("%d", *val);
    hui__slider_draw(r, t, (int)act, (int)hov, label, value);
    return changed;
}

/* ---- Drag number ---- */

bool hui_drag_f(const char *label, hui_rect r, float *val, float speed) {
    if (!val) return false;
    bool hov  = hui_is_hovered(r);
    bool act  = hov && (hui_g->io.mouse_btn & 1u);
    bool changed = false;

    if (act) {
        int dx = (int)hui_g->io.mouse_x - (int)hui_g->io.mouse_x_prev;
        if (dx != 0) { *val += (float)dx * speed; changed = true; }
    }

    hui_color bg = act ? CUM_BG1 : (hov ? CUM_BG4 : CUM_BG3);
    hui_rect_fill(r, bg, 2);
    hui_rect_outline(r, hov ? CUM_ACCENT : CUM_BG4, 2);
    /* drag arrows hint */
    if (hov) {
        hui_text(r.x + 2, r.y + (r.h - HUI_FONT_PIXEL_H) / 2, "<", CUM_FG2);
        hui_text(r.x + r.w - HUI_FONT_ADVANCE - 2, r.y + (r.h - HUI_FONT_PIXEL_H) / 2, ">", CUM_FG2);
    }
    const char *disp = label ? hui_fmt("%s: %.3g", label, *val)
                              : hui_fmt("%.3g", *val);
    int tw = hui_text_width(disp);
    hui_text(r.x + (r.w - tw) / 2, r.y + (r.h - HUI_FONT_PIXEL_H) / 2, disp, CUM_FG);
    return changed;
}

/* ---- Panel layout helpers ---- */

void hui_panel_same_line(void) {
    if (hui__panel_sp > 0)
        hui__panel_stk[hui__panel_sp - 1].same_line = true;
}

void hui_panel_spacing(int px) {
    if (hui__panel_sp > 0)
        hui__panel_stk[hui__panel_sp - 1].cursor_y += px;
}

void hui_panel_indent(int px) {
    if (hui__panel_sp == 0) return;
    hui__panel_ctx *ctx = &hui__panel_stk[hui__panel_sp - 1];
    ctx->indent += px;
}

void hui_panel_unindent(int px) {
    if (hui__panel_sp == 0) return;
    hui__panel_ctx *ctx = &hui__panel_stk[hui__panel_sp - 1];
    ctx->indent -= px;
    if (ctx->indent < 0) ctx->indent = 0;
}


/* ---- Text input ---- */

bool hui_text_input(hui_rect r, hui_text_state *st, char *buf, int bufsz) {
    if (!st || !buf || bufsz <= 0) return false;
    bool changed = false;
    int  len     = (int)strlen(buf);

    /* Unique ID for this field — low bits of pointer, non-zero guaranteed */
    int fid = (int)((uintptr_t)st & 0x7fffffff) | 1;

    /* Sync focused bool with global focus: another field may have stolen it */
    if (st->focused && !hui_focus_has(fid)) st->focused = false;

    /* Click inside to focus, placing cursor at click position */
    if (hui_is_clicked(r, 0)) {
        st->focused = true;
        hui_focus_set(fid);
        int off = hui_g->io.mouse_x - r.x - 4;
        st->cursor = hui_clamp(off / (HUI_FONT_W + 1), 0, len);
    }
    /* Click outside to unfocus */
    if ((hui_g->io.mouse_btn & 1u) && !(hui_g->io.mouse_btn_prev & 1u) &&
        !hui_is_hovered(r)) {
        st->focused = false;
        hui_focus_clear(fid);
    }

    /* Clamp cursor */
    st->cursor = hui_clamp(st->cursor, 0, len);

    if (st->focused) {
        /* Printable chars from text_typed buffer (filled by backend via XLookupString) */
        for (int i = 0; i < hui_g->io.text_typed_len; i++) {
            unsigned char ch = (unsigned char)hui_g->io.text_typed[i];
            if (ch >= 32 && ch < 127 && len < bufsz - 1) {
                memmove(buf + st->cursor + 1, buf + st->cursor,
                        (size_t)(len - st->cursor + 1));
                buf[st->cursor++] = (char)ch;
                len++;
                changed = true;
            }
        }
        if (hui_key_pressed(HUI_KEY_BACKSPACE) && st->cursor > 0) {
            memmove(buf + st->cursor - 1, buf + st->cursor,
                    (size_t)(len - st->cursor + 1));
            st->cursor--;
            changed = true;
        }
        if (hui_key_pressed(HUI_KEY_DELETE) && st->cursor < len) {
            memmove(buf + st->cursor, buf + st->cursor + 1,
                    (size_t)(len - st->cursor));
            changed = true;
        }
        if (hui_key_pressed(HUI_KEY_LEFT)  && st->cursor > 0)   st->cursor--;
        if (hui_key_pressed(HUI_KEY_RIGHT) && st->cursor < len) st->cursor++;
        if (hui_key_pressed(HUI_KEY_HOME))  st->cursor = 0;
        if (hui_key_pressed(HUI_KEY_END))   st->cursor = len;
        if (hui_key_pressed(HUI_KEY_RETURN) || hui_key_pressed(HUI_KEY_ESCAPE)) {
            st->focused = false;
            hui_focus_clear(fid);
        }
    }

    /* Draw */
    bool      hov    = hui_is_hovered(r);
    hui_color bg     = st->focused ? CUM_BG1 : (hov ? CUM_BG4 : CUM_BG3);
    hui_color border = st->focused ? CUM_ACCENT : (hov ? CUM_BG4 : CUM_BG3);
    hui_rect_fill(r, bg, 2);
    hui_rect_outline(r, border, 2);

    int tx = r.x + 4;
    int ty = r.y + (r.h - HUI_FONT_PIXEL_H) / 2;
    hui_text(tx, ty, buf, CUM_FG);

    if (st->focused && (hui_g->frame / 30) % 2 == 0) {
        int cx = tx + st->cursor * (HUI_FONT_W + 1);
        hui_line(cx, ty - 1, cx, ty + HUI_FONT_PIXEL_H, CUM_ACCENT, 1);
    }
    if (st->focused) hui_draw_focus_ring(r, 2);

    return changed;
}

/* ---- Collapsible section header ---- */

bool hui_collapsible(const char *title, hui_rect r, bool *open) {
    if (!open) return false;
    bool hov     = hui_is_hovered(r);
    bool clicked = hui_is_clicked(r, 0);
    if (clicked) *open = !*open;

    hui_rect_fill(r, hov ? CUM_BG4 : CUM_BG2, 2);

    /* Arrow indicator */
    int ax = r.x + 6;
    int ay = r.y + r.h / 2;
    if (*open) {
        /* Down-pointing triangle */
        hui_triangle_fill((hui_v2i){(int16_t)ax,        (int16_t)(ay - 3)},
                          (hui_v2i){(int16_t)(ax + 7),  (int16_t)(ay - 3)},
                          (hui_v2i){(int16_t)(ax + 3),  (int16_t)(ay + 4)}, CUM_FG2);
    } else {
        /* Right-pointing triangle */
        hui_triangle_fill((hui_v2i){(int16_t)ax,        (int16_t)(ay - 4)},
                          (hui_v2i){(int16_t)(ax + 7),  (int16_t)ay      },
                          (hui_v2i){(int16_t)ax,        (int16_t)(ay + 4)}, CUM_FG2);
    }

    if (title) {
        hui_rect lr = hui_rect_make(r.x + 18, r.y, r.w - 18, r.h);
        hui_label(title, lr, HUI_ALIGN_LEFT, hov ? CUM_FG : CUM_FG2);
    }
    return *open;
}

/* ---- Scroll region ---- */

#define HUI__SCROLL_BAR_W 8
#define HUI__SCROLL_MAX   4

typedef struct {
    hui_rect viewport;
} hui__scroll_ctx;

static hui__scroll_ctx hui__scroll_stk[HUI__SCROLL_MAX];
static int             hui__scroll_sp;
static int             hui__scroll_overflow;

hui_rect hui_scroll_begin(hui_rect r, int content_h, int *scroll_y) {
    if (!scroll_y) { hui_clip_push(r); return r; }

    int bar_w = (content_h > r.h) ? HUI__SCROLL_BAR_W : 0;
    int max_scroll = hui_max(content_h - r.h, 0);

    /* Scroll wheel */
    if (hui_is_hovered(r) && hui_g->io.scroll_dy != 0)
        *scroll_y -= hui_g->io.scroll_dy * 16;

    if (bar_w > 0) {
        hui_rect bar = hui_rect_make(r.x + r.w - bar_w, r.y, bar_w, r.h);

        float ratio   = hui_clampf((float)r.h / (float)content_h, 0.0f, 1.0f);
        int   thumb_h = hui_max((int)(r.h * ratio), 16);
        float t       = (max_scroll > 0) ? hui_clampf((float)*scroll_y / (float)max_scroll, 0.0f, 1.0f) : 0.0f;
        int   thumb_y = bar.y + (int)((float)(r.h - thumb_h) * t);
        hui_rect thumb = hui_rect_make(bar.x + 1, thumb_y, bar_w - 2, thumb_h);

        /* Drag thumb */
        if (hui_is_hovered(thumb) && (hui_g->io.mouse_btn & 1u) && max_scroll > 0) {
            int dy = (int)hui_g->io.mouse_y - (int)hui_g->io.mouse_y_prev;
            if (dy != 0)
                *scroll_y += (int)((float)dy * (float)max_scroll / (float)(r.h - thumb_h));
        }

        /* Click on track outside thumb to jump */
        if (hui_is_clicked(bar, 0) && !hui_is_hovered(thumb) && max_scroll > 0) {
            float ct = (float)(hui_g->io.mouse_y - bar.y) / (float)r.h;
            *scroll_y = (int)(ct * (float)max_scroll);
        }

        /* Clamp */
        *scroll_y = hui_clamp(*scroll_y, 0, max_scroll);

        hui_rect_fill(bar, CUM_BG1, 4);
        hui_color thumb_c = hui_is_hovered(thumb) ? CUM_BG4 : CUM_BG3;
        hui_rect_fill(thumb, thumb_c, 4);
    } else {
        *scroll_y = 0;
    }

    hui_rect content = hui_rect_make(r.x, r.y, r.w - bar_w, r.h);

    if (hui__scroll_sp < HUI__SCROLL_MAX) {
        hui_clip_push(content);
        hui__scroll_stk[hui__scroll_sp++].viewport = r;
    } else {
        hui__scroll_overflow++;
    }

    return content;
}

void hui_scroll_end(void) {
    if (hui__scroll_overflow > 0) { hui__scroll_overflow--; return; }
    if (hui__scroll_sp > 0) { hui__scroll_sp--; hui_clip_pop(); }
}

/* ---- Tab bar ---- */

bool hui_tab_bar(hui_rect r, const char **labels, int n, int *active) {
    if (!labels || n <= 0 || !active) return false;
    bool changed = false;
    int  tab_w   = r.w / n;

    for (int i = 0; i < n; i++) {
        hui_rect tr  = hui_rect_make(r.x + i * tab_w, r.y, tab_w, r.h);
        bool hov     = hui_is_hovered(tr);
        bool sel     = (*active == i);
        if (hui_is_clicked(tr, 0) && !sel) { *active = i; changed = true; }

        hui_color bg = sel ? CUM_BG3 : (hov ? CUM_BG4 : CUM_BG2);
        hui_rect_fill(tr, bg, sel ? 3 : 0);

        if (sel)
            hui_line(tr.x + 1, tr.y + tr.h - 2,
                     tr.x + tr.w - 2, tr.y + tr.h - 2, CUM_ACCENT, 2);

        if (labels[i]) {
            hui_rect lr = hui_rect_make(tr.x + 4, tr.y, tr.w - 8, tr.h);
            hui_label(labels[i], lr, HUI_ALIGN_CENTER,
                      sel ? CUM_FG : (hov ? CUM_FG2 : CUM_FG3));
        }
    }

    /* Bottom border */
    hui_line(r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, CUM_BG3, 1);

    return changed;
}


/* ---- Tooltip ---- */

void hui_tooltip(const char *text) {
    if (!text || !*text || !hui_g) return;
    int pad = 4;
    int tw  = hui_text_width(text);
    int bw  = tw + pad * 2;
    int bh  = HUI_FONT_PIXEL_H + pad * 2;
    int x   = (int)hui_g->io.mouse_x + 14;
    int y   = (int)hui_g->io.mouse_y + 14;
    if (x + bw > (int)hui_g->screen_w) x = (int)hui_g->io.mouse_x - bw - 4;
    if (y + bh > (int)hui_g->screen_h) y = (int)hui_g->io.mouse_y - bh - 4;
    hui_rect r = hui_rect_make(x, y, bw, bh);
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_rect_fill(r, CUM_BG0, 2);
    hui_rect_outline(r, CUM_BG4, 2);
    hui_text(x + pad, y + pad, text, CUM_FG);
    hui_reset_layer();
}

/* ---- Badge ---- */

void hui_badge(hui_rect r, const char *text, hui_color c) {
    if (!text || !*text) return;
    int pad = 3;
    int tw  = hui_text_width(text);
    int bw  = hui_max(tw + pad * 2, HUI_FONT_PIXEL_H + pad * 2); /* min size = circle */
    int bh  = HUI_FONT_PIXEL_H + pad * 2;
    int x   = r.x + r.w - bw;
    int y   = r.y - bh / 2;
    hui_rect br = hui_rect_make(x, y, bw, bh);
    hui_rect_fill(br, c, (uint8_t)(bh / 2));
    hui_text(br.x + (bw - tw) / 2, br.y + pad, text, CUM_FG);
}

void hui_fps_overlay(hui_rect r) {
    if (!hui_g) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f fps", (double)hui_g->fps);
    int tw = hui_text_width(buf);
    int pad = 4;
    int bw = tw + pad * 2;
    int bh = HUI_FONT_PIXEL_H + pad * 2;
    hui_rect pr = hui_rect_make(r.x + r.w - bw - 2, r.y + 2, bw, bh);
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_rect_fill(pr, (hui_color){0, 0, 0, 160}, (uint8_t)(bh / 2));
    hui_text(pr.x + pad, pr.y + pad, buf, CUM_FG2);
    hui_reset_layer();
}

hui_color hui_pulse_color(hui_color base_c, hui_color trigger_c,
                          int age_frames, int decay_frames) {
    if (decay_frames <= 0 || age_frames >= decay_frames) return base_c;
    float t = 1.0f - (float)age_frames / (float)decay_frames;
    hui_color c;
    c.r = (uint8_t)(base_c.r + (int)((trigger_c.r - base_c.r) * t));
    c.g = (uint8_t)(base_c.g + (int)((trigger_c.g - base_c.g) * t));
    c.b = (uint8_t)(base_c.b + (int)((trigger_c.b - base_c.b) * t));
    c.a = (uint8_t)(base_c.a + (int)((trigger_c.a - base_c.a) * t));
    return c;
}

/* ---- Popup ---- */

static bool hui__popup_open;

bool hui_popup_begin(hui_rect r, bool *open) {
    if (!open || !*open) return false;
    hui_set_layer(HUI_LAYER_POPUP);
    /* Dim the rest of the screen */
    hui_rect_fill(hui_rect_make(0, 0, hui_g->screen_w, hui_g->screen_h),
                  (hui_color){0, 0, 0, 80}, 0);
    hui_rect_fill(r, CUM_BG2, 4);
    hui_rect_outline(r, CUM_BG4, 4);
    /* Close on outside click */
    if ((hui_g->io.mouse_btn & 1u) && !(hui_g->io.mouse_btn_prev & 1u) &&
        !hui_rect_contains(r, hui_g->io.mouse_x, hui_g->io.mouse_y))
        *open = false;
    if (hui_key_pressed(HUI_KEY_ESCAPE))
        *open = false;
    hui_clip_push(r);
    hui__popup_open = true;
    return *open;
}

void hui_popup_end(void) {
    if (hui__popup_open) {
        hui_clip_pop();
        hui__popup_open = false;
        hui_reset_layer();
    }
}

/* ---- Menubar + dropdown menus ---- */

#define HUI__MENUBAR_MAX  8
#define HUI__MENU_ITEM_H  20
#define HUI__MENU_WIDTH   160

static int  hui__menu_open        = -1;
static int  hui__menubar_x;
static int  hui__menubar_y;
static int  hui__menubar_h;
static int  hui__menubar_idx;
static int  hui__cur_menu_idx;          /* index of menu currently in begin/end pair */
static bool hui__cur_menu_is_open;
static int  hui__menu_dd_x;            /* dropdown left edge X */
static int  hui__menu_item_y;          /* dropdown item cursor Y */
static int  hui__menu_item_count;      /* accumulator for current menu */
static int  hui__menu_counts[HUI__MENUBAR_MAX]; /* prev-frame counts for sizing */

void hui_menubar_begin(hui_rect r) {
    hui_rect_fill(r, CUM_BG1, 0);
    hui_line(r.x, r.y + r.h - 1, r.x + r.w - 1, r.y + r.h - 1, CUM_BG3, 1);
    hui__menubar_x   = r.x + 4;
    hui__menubar_y   = r.y;
    hui__menubar_h   = r.h;
    hui__menubar_idx = 0;
}

bool hui_menu_begin(const char *label) {
    int idx = hui__menubar_idx++;
    if (idx >= HUI__MENUBAR_MAX) idx = HUI__MENUBAR_MAX - 1;

    int tw  = label ? hui_text_width(label) : 0;
    int pad = 8;
    int w   = tw + pad * 2;
    hui_rect tr = hui_rect_make(hui__menubar_x, hui__menubar_y, w, hui__menubar_h);
    bool hov     = hui_is_hovered(tr);
    bool open    = (hui__menu_open == idx);

    if (hui_is_clicked(tr, 0))
        hui__menu_open = open ? -1 : idx;

    hui_rect_fill(tr, (hui__menu_open == idx || hov) ? CUM_BG3 : CUM_BG1, 0);
    if (label)
        hui_text(tr.x + pad, tr.y + (tr.h - HUI_FONT_PIXEL_H) / 2, label, CUM_FG);
    hui__menubar_x += w + 2;

    hui__cur_menu_idx    = idx;
    hui__cur_menu_is_open = (hui__menu_open == idx);
    hui__menu_item_count  = 0;

    if (hui__cur_menu_is_open) {
        int prev = (idx < HUI__MENUBAR_MAX) ? hui__menu_counts[idx] : 0;
        if (prev < 1) prev = 1;
        int dd_h = prev * HUI__MENU_ITEM_H + 8;
        hui_rect dd = hui_rect_make(tr.x, hui__menubar_y + hui__menubar_h,
                                    HUI__MENU_WIDTH, dd_h);
        hui_rect_fill(dd, CUM_BG1, 4);
        hui_rect_outline(dd, CUM_BG3, 4);
        hui__menu_dd_x   = dd.x;
        hui__menu_item_y = dd.y + 4;
    }
    return hui__cur_menu_is_open;
}

bool hui_menu_item(const char *label, const char *shortcut) {
    hui__menu_item_count++;   /* always count, even when closed */
    if (!hui__cur_menu_is_open) return false;

    hui_rect r = hui_rect_make(hui__menu_dd_x + 2, hui__menu_item_y,
                               HUI__MENU_WIDTH - 4, HUI__MENU_ITEM_H);
    bool hov     = hui_is_hovered(r);
    bool clicked = hui_is_clicked(r, 0);
    if (hov) hui_rect_fill(r, CUM_BG3, 2);

    if (label) {
        int lw = (HUI__MENU_WIDTH - 4) - (shortcut ? 64 : 12);
        hui_rect lr = hui_rect_make(r.x + 6, r.y, lw, r.h);
        hui_label(label, lr, HUI_ALIGN_LEFT, hov ? CUM_FG : CUM_FG2);
    }
    if (shortcut) {
        int sw = hui_text_width(shortcut);
        hui_text(r.x + r.w - sw - 6, r.y + (r.h - HUI_FONT_PIXEL_H) / 2, shortcut, CUM_FG3);
    }
    hui__menu_item_y += HUI__MENU_ITEM_H;

    if (clicked) hui__menu_open = -1;
    return clicked;
}

void hui_menu_separator(void) {
    if (!hui__cur_menu_is_open) return;
    int y = hui__menu_item_y + 4;
    hui_line(hui__menu_dd_x + 4, y, hui__menu_dd_x + HUI__MENU_WIDTH - 4, y, CUM_BG3, 1);
    hui__menu_item_y += 9;
}

void hui_menu_end(void) {
    int idx = hui__cur_menu_idx;
    if (idx >= 0 && idx < HUI__MENUBAR_MAX)
        hui__menu_counts[idx] = hui__menu_item_count;
}

void hui_menubar_end(void) {
    if (hui_key_pressed(HUI_KEY_ESCAPE)) hui__menu_open = -1;
    /* Close on click outside all menu labels */
    if ((hui_g->io.mouse_btn & 1u) && !(hui_g->io.mouse_btn_prev & 1u) &&
        hui__menu_open >= 0) {
        /* Check if click is inside the menubar strip — if not, close */
        hui_rect bar = hui_rect_make(0, hui__menubar_y,
                                     hui_g->screen_w, hui__menubar_h);
        if (!hui_rect_contains(bar, hui_g->io.mouse_x, hui_g->io.mouse_y)) {
            /* Also check dropdown area */
            int dd_h = hui__menu_counts[hui__menu_open] * HUI__MENU_ITEM_H + 8;
            hui_rect dd = hui_rect_make(hui__menu_dd_x,
                                        hui__menubar_y + hui__menubar_h,
                                        HUI__MENU_WIDTH, dd_h);
            if (!hui_rect_contains(dd, hui_g->io.mouse_x, hui_g->io.mouse_y))
                hui__menu_open = -1;
        }
    }
}

/* ---- Context menu ---- */

static bool     hui__ctx_open_ref;   /* internal copy when no caller *open */
static hui_rect hui__ctx_rect;
static int      hui__ctx_item_y;
static int      hui__ctx_item_count;
static int      hui__ctx_count_prev;
static bool     hui__ctx_is_open;

bool hui_context_menu_begin(hui_rect trigger_r, bool *open) {
    if (!open) open = &hui__ctx_open_ref;

    /* Right-click inside trigger opens the menu */
    if ((hui_g->io.mouse_btn & 2u) && !(hui_g->io.mouse_btn_prev & 2u) &&
        hui_rect_contains(trigger_r, hui_g->io.mouse_x, hui_g->io.mouse_y)) {
        *open = true;
        int h = hui_max(hui__ctx_count_prev * HUI__MENU_ITEM_H + 8, HUI__MENU_ITEM_H + 8);
        hui__ctx_rect = hui_rect_make(hui_g->io.mouse_x, hui_g->io.mouse_y,
                                      HUI__MENU_WIDTH, h);
        /* Keep on screen */
        if (hui__ctx_rect.x + HUI__MENU_WIDTH > hui_g->screen_w)
            hui__ctx_rect.x = (int16_t)(hui_g->screen_w - HUI__MENU_WIDTH);
        if (hui__ctx_rect.y + h > hui_g->screen_h)
            hui__ctx_rect.y = (int16_t)(hui_g->screen_h - h);
    }

    hui__ctx_item_count = 0;
    if (!*open) { hui__ctx_is_open = false; return false; }

    /* Close on outside LMB click or Escape */
    if ((hui_g->io.mouse_btn & 1u) && !(hui_g->io.mouse_btn_prev & 1u) &&
        !hui_rect_contains(hui__ctx_rect, hui_g->io.mouse_x, hui_g->io.mouse_y))
        *open = false;
    if (hui_key_pressed(HUI_KEY_ESCAPE))
        *open = false;
    if (!*open) { hui__ctx_is_open = false; return false; }

    /* Update height from prev count */
    int h = hui_max(hui__ctx_count_prev * HUI__MENU_ITEM_H + 8, HUI__MENU_ITEM_H + 8);
    hui__ctx_rect.h = (int16_t)h;

    hui_rect_fill(hui__ctx_rect, CUM_BG1, 4);
    hui_rect_outline(hui__ctx_rect, CUM_BG3, 4);
    hui__ctx_item_y    = hui__ctx_rect.y + 4;
    hui__ctx_is_open   = true;

    /* Redirect menu_item to context menu */
    hui__cur_menu_is_open = true;
    hui__menu_dd_x        = hui__ctx_rect.x;
    hui__menu_item_y      = hui__ctx_item_y;
    hui__menu_item_count  = 0;
    return true;
}

void hui_context_menu_end(void) {
    hui__ctx_count_prev  = hui__ctx_item_count + hui__menu_item_count;
    hui__ctx_item_count  = 0;
    hui__ctx_is_open     = false;
    hui__cur_menu_is_open = false;
}

/* ---- Named drag handle ---- */

bool hui_is_dragging(int id) {
    return hui__drag_active && hui__drag_id == id;
}

bool hui_drag_handle(int id, int *x, int *y, int radius, int snap_grid) {
    if (!x || !y) return false;
    hui_rect hit = hui_rect_make(*x - radius, *y - radius, radius*2, radius*2);
    bool hov  = hui_is_hovered(hit);
    bool lbtn = (hui_g->io.mouse_btn & 1u) != 0;

    if (hui_is_clicked(hit, 0)) { hui__drag_id = id; hui__drag_active = true; }
    if (!lbtn && hui__drag_id == id) hui__drag_active = false;

    bool active  = hui__drag_active && (hui__drag_id == id);
    bool changed = false;
    if (active) {
        int nx = *x + (hui_g->io.mouse_x - hui_g->io.mouse_x_prev);
        int ny = *y + (hui_g->io.mouse_y - hui_g->io.mouse_y_prev);
        if (snap_grid > 0) { nx = hui_snap_i(nx, snap_grid); ny = hui_snap_i(ny, snap_grid); }
        if (nx != *x || ny != *y) { *x = nx; *y = ny; changed = true; }
    }

    hui_color fill = active ? CUM_ACTIVE : (hov ? CUM_BG4 : CUM_BG3);
    hui_circle_fill(*x, *y, radius, fill);
    hui_circle(*x, *y, radius, active ? CUM_ACCENT : CUM_BG4);
    return changed;
}

/* ---- Resize handle ---- */

bool hui_resize_handle(int id, int *w, int *h, hui_rect body_r, int min_w, int min_h) {
    if (!w || !h) return false;
    int rx = body_r.x + body_r.w - 14;
    int ry = body_r.y + body_r.h - 14;
    hui_rect grip = hui_rect_make(rx, ry, 14, 14);
    bool hov  = hui_is_hovered(grip);
    bool lbtn = (hui_g->io.mouse_btn & 1u) != 0;

    if (hui_is_clicked(grip, 0)) { hui__drag_id = id; hui__drag_active = true; }
    if (!lbtn && hui__drag_id == id) hui__drag_active = false;

    bool active  = hui__drag_active && (hui__drag_id == id);
    bool changed = false;
    if (active) {
        int nw = *w + (hui_g->io.mouse_x - hui_g->io.mouse_x_prev);
        int nh = *h + (hui_g->io.mouse_y - hui_g->io.mouse_y_prev);
        if (nw < min_w) nw = min_w;
        if (nh < min_h) nh = min_h;
        if (nw != *w || nh != *h) { *w = nw; *h = nh; changed = true; }
    }

    hui_color c = active ? CUM_ACCENT : (hov ? CUM_FG2 : CUM_FG3);
    for (int i = 1; i <= 3; i++) {
        int o = i * 4;
        hui_line(rx + 14 - o, ry + 13, rx + 13, ry + 14 - o, c, 1);
    }
    return changed;
}

/* ---- Bitmap view ---- */

void hui_bitmap_view(hui_rect r, const uint8_t *rgb, int iw, int ih) {
    hui_rect_outline(r, CUM_BG4, 2);
#if defined(HUI_BACKEND_HEADLESS) || defined(HUI_BACKEND_X11) || \
    defined(HUI_BACKEND_SDL2)     || defined(HUI_BACKEND_LINUX_FB)
    if (rgb && iw > 0 && ih > 0) {
        hui_image_rgb(r, rgb, iw, ih);
        return;
    }
#else
    (void)rgb; (void)iw; (void)ih;
#endif
    hui_rect_fill(r, CUM_BG3, 2);
    hui_label("no image", r, HUI_ALIGN_CENTER, CUM_FG3);
}

/* ---- Color picker / gradient (guarded by HUI_NO_FLOAT_WIDGETS) ---- */
#ifndef HUI_NO_FLOAT_WIDGETS

/* RGB ↔ HSV helpers (h in [0,360), s/v in [0,1]) */
static void hui__rgb2hsv(uint8_t ri, uint8_t gi, uint8_t bi,
                          float *h, float *s, float *v) {
    float r = ri/255.f, g = gi/255.f, b = bi/255.f;
    float mx = r>g?(r>b?r:b):(g>b?g:b);
    float mn = r<g?(r<b?r:b):(g<b?g:b);
    float d  = mx - mn;
    *v = mx;
    *s = (mx > 0.0f) ? d / mx : 0.0f;
    if (d < 1e-6f) { *h = 0.0f; return; }
    if      (mx == r) *h = 60.f * (fmodf((g - b) / d, 6.f));
    else if (mx == g) *h = 60.f * ((b - r) / d + 2.f);
    else              *h = 60.f * ((r - g) / d + 4.f);
    if (*h < 0.f) *h += 360.f;
}
static hui_color hui__hsv2rgb(float h, float s, float v) {
    float c = v * s, x = c * (1.f - fabsf(fmodf(h/60.f, 2.f) - 1.f)), m = v - c;
    float r=0,g=0,b=0;
    int   seg = (int)(h/60.f) % 6;
    switch (seg) {
        case 0: r=c;g=x;b=0; break; case 1: r=x;g=c;b=0; break;
        case 2: r=0;g=c;b=x; break; case 3: r=0;g=x;b=c; break;
        case 4: r=x;g=0;b=c; break; case 5: r=c;g=0;b=x; break;
    }
    return (hui_color){(uint8_t)((r+m)*255),(uint8_t)((g+m)*255),(uint8_t)((b+m)*255),255};
}

bool hui_color_panel(hui_rect r, hui_color *color) {
    if (!color || !hui_g) return false;
    bool changed = false;
    float hue, sat, val;
    hui__rgb2hsv(color->r, color->g, color->b, &hue, &sat, &val);

    int cells = 16;
    for (int cy = 0; cy < cells; cy++) {
        for (int cx = 0; cx < cells; cx++) {
            float s2 = (float)cx / (float)(cells-1);
            float v2 = 1.0f - (float)cy / (float)(cells-1);
            hui_color c2 = hui__hsv2rgb(hue, s2, v2);
            int px = r.x + cx * r.w / cells;
            int py = r.y + cy * r.h / cells;
            hui_rect_fill(hui_rect_make(px, py, r.w/cells+1, r.h/cells+1), c2, 0);
        }
    }
    hui_rect_outline(r, CUM_BG4, 0);

    int cx = r.x + (int)(sat * (r.w - 1));
    int cy2 = r.y + (int)((1.0f - val) * (r.h - 1));
    hui_circle(cx, cy2, 4, (hui_color){255,255,255,200});

    if (hui_is_hovered(r) && (hui_g->io.mouse_btn & 1u)) {
        sat = hui_clampf((float)(hui_g->io.mouse_x - r.x) / (float)r.w, 0,1);
        val = 1.0f - hui_clampf((float)(hui_g->io.mouse_y - r.y) / (float)r.h, 0,1);
        *color = hui__hsv2rgb(hue, sat, val);
        color->a = 255;
        changed = true;
    }
    return changed;
}

bool hui_color_picker(hui_rect r, hui_color *color) {
    if (!color || !hui_g) return false;
    bool changed = false;

    const int hue_bar_w = 12;
    const int gap       = 4;
    hui_rect sv_r  = hui_rect_make(r.x, r.y, r.w - hue_bar_w - gap, r.h);
    hui_rect hue_r = hui_rect_make(r.x + r.w - hue_bar_w, r.y, hue_bar_w, r.h);

    float hue, sat, val;
    hui__rgb2hsv(color->r, color->g, color->b, &hue, &sat, &val);

    /* ---- SV square ---- */
    /* Draw sampled grid (8×8 is cheap enough) */
    int cells = 16;
    for (int cy = 0; cy < cells; cy++) {
        for (int cx = 0; cx < cells; cx++) {
            float s2 = (float)cx / (float)(cells-1);
            float v2 = 1.0f - (float)cy / (float)(cells-1);
            hui_color c2 = hui__hsv2rgb(hue, s2, v2);
            int px = sv_r.x + cx * sv_r.w / cells;
            int py = sv_r.y + cy * sv_r.h / cells;
            int pw = sv_r.w / cells + 1;
            int ph = sv_r.h / cells + 1;
            hui_rect_fill(hui_rect_make(px, py, pw, ph), c2, 0);
        }
    }
    hui_rect_outline(sv_r, CUM_BG4, 0);

    /* Crosshair */
    int cx = sv_r.x + (int)(sat * (sv_r.w - 1));
    int cy2 = sv_r.y + (int)((1.0f - val) * (sv_r.h - 1));
    hui_circle(cx, cy2, 4, (hui_color){255,255,255,200});

    /* Drag inside SV square */
    if (hui_is_hovered(sv_r) && (hui_g->io.mouse_btn & 1u)) {
        sat = hui_clampf((float)(hui_g->io.mouse_x - sv_r.x) / (float)sv_r.w, 0,1);
        val = 1.0f - hui_clampf((float)(hui_g->io.mouse_y - sv_r.y) / (float)sv_r.h, 0,1);
        *color = hui__hsv2rgb(hue, sat, val);
        color->a = 255;
        changed = true;
    }

    /* ---- Hue bar ---- */
    int hue_cells = 24;
    for (int hy = 0; hy < hue_cells; hy++) {
        float h2 = (float)hy / (float)hue_cells * 360.f;
        int py = hue_r.y + hy * hue_r.h / hue_cells;
        int ph = hue_r.h / hue_cells + 1;
        hui_rect_fill(hui_rect_make(hue_r.x, py, hue_r.w, ph), hui__hsv2rgb(h2,1,1), 0);
    }
    hui_rect_outline(hue_r, CUM_BG4, 0);

    /* Hue indicator line */
    int hy_px = hue_r.y + (int)(hue / 360.f * hue_r.h);
    hui_line(hue_r.x - 1, hy_px, hue_r.x + hue_r.w + 1, hy_px, (hui_color){255,255,255,220}, 1);

    /* Drag on hue bar */
    if (hui_is_hovered(hue_r) && (hui_g->io.mouse_btn & 1u)) {
        hue = hui_clampf((float)(hui_g->io.mouse_y - hue_r.y) / (float)hue_r.h, 0,1) * 360.f;
        *color = hui__hsv2rgb(hue, sat, val);
        color->a = 255;
        changed = true;
    }

    return changed;
}

/* ---- Gradient ---- */

void hui_gradient_init(hui_gradient *g, hui_color a, hui_color b) {
    g->n       = 2;
    g->sel     = -1;
    g->pos[0]  = 0.0f;  g->col[0] = a;
    g->pos[1]  = 1.0f;  g->col[1] = b;
}

hui_color hui_gradient_at(const hui_gradient *g, float t) {
    if (!g || g->n == 0) return HUI_RGB(0,0,0);
    if (t <= g->pos[0]) return g->col[0];
    if (t >= g->pos[g->n-1]) return g->col[g->n-1];
    for (int i = 0; i < g->n - 1; i++) {
        if (t <= g->pos[i+1]) {
            float u = (t - g->pos[i]) / (g->pos[i+1] - g->pos[i]);
            return hui_color_lerp(g->col[i], g->col[i+1], u);
        }
    }
    return g->col[g->n-1];
}

/* Insert a stop keeping pos[] sorted; returns new index or -1 if full. */
static int hui__grad_insert(hui_gradient *g, float pos, hui_color c) {
    if (g->n >= HUI_GRADIENT_MAX_STOPS) return -1;
    int idx = g->n;
    for (int i = 0; i < g->n; i++) {
        if (pos < g->pos[i]) { idx = i; break; }
    }
    for (int i = g->n; i > idx; i--) {
        g->pos[i] = g->pos[i-1];
        g->col[i] = g->col[i-1];
    }
    g->pos[idx] = pos;
    g->col[idx] = c;
    g->n++;
    return idx;
}

static void hui__grad_remove(hui_gradient *g, int idx) {
    if (g->n <= 2 || idx < 0 || idx >= g->n) return;
    for (int i = idx; i < g->n - 1; i++) {
        g->pos[i] = g->pos[i+1];
        g->col[i] = g->col[i+1];
    }
    g->n--;
    if (g->sel >= g->n) g->sel = g->n - 1;
}

bool hui_gradient_edit(hui_rect r, hui_gradient *g) {
    if (!g || !hui_g) return false;
    bool changed = false;

    const int handle_h = 16;
    const int ramp_h   = r.h - handle_h;
    if (ramp_h < 4) return false;

    hui_rect ramp_r   = hui_rect_make(r.x, r.y,        r.w, ramp_h);
    hui_rect handle_r = hui_rect_make(r.x, r.y+ramp_h, r.w, handle_h);

    /* ---- Draw ramp (64 cells) ---- */
    const int cells = 64;
    for (int i = 0; i < cells; i++) {
        float t0 = (float)i     / (float)cells;
        float t1 = (float)(i+1) / (float)cells;
        hui_color mid = hui_gradient_at(g, (t0+t1)*0.5f);
        int px = ramp_r.x + i * ramp_r.w / cells;
        int pw = ramp_r.x + (i+1) * ramp_r.w / cells - px + 1;
        hui_rect_fill(hui_rect_make(px, ramp_r.y, pw, ramp_r.h), mid, 0);
    }
    hui_rect_outline(ramp_r, CUM_BG4, 0);

    /* Click on ramp → add stop */
    if (hui_is_clicked(ramp_r, 0)) {
        float t = hui_clampf((float)(hui_g->io.mouse_x - ramp_r.x) / (float)ramp_r.w, 0,1);
        hui_color nc = hui_gradient_at(g, t);
        int idx = hui__grad_insert(g, t, nc);
        if (idx >= 0) { g->sel = idx; changed = true; }
    }

    /* ---- Draw + interact with handles ---- */
    static int hui__grad_drag_idx = -1;

    for (int i = 0; i < g->n; i++) {
        int hx = ramp_r.x + (int)(g->pos[i] * (ramp_r.w - 1));
        int hy = handle_r.y + handle_h / 2;

        /* Triangle pointing up: tip at (hx, handle_r.y+1) */
        hui_v2i tip  = {(int16_t)hx,       (int16_t)(handle_r.y + 2)};
        hui_v2i bl   = {(int16_t)(hx - 5), (int16_t)(handle_r.y + handle_h - 2)};
        hui_v2i br   = {(int16_t)(hx + 5), (int16_t)(handle_r.y + handle_h - 2)};
        bool is_sel  = (g->sel == i);
        hui_triangle_fill(tip, bl, br, is_sel ? CUM_ACCENT : g->col[i]);
        hui_triangle(tip, bl, br, is_sel ? CUM_BG0 : CUM_BG4);

        /* Hit circle for interaction */
        hui_rect hit = hui_rect_make(hx - 6, handle_r.y, 12, handle_h);

        /* Start drag */
        if (hui_is_clicked(hit, 0)) {
            g->sel = i;
            hui__grad_drag_idx = i;
        }
        /* RMB → delete */
        if (hui_is_clicked(hit, 1) && g->n > 2) {
            hui__grad_remove(g, i);
            changed = true;
            if (hui__grad_drag_idx == i) hui__grad_drag_idx = -1;
            i--;  /* re-examine shifted index */
            continue;
        }
        (void)hy;
    }

    /* Drag selected handle */
    if (hui__grad_drag_idx >= 0 && hui__grad_drag_idx < g->n &&
        (hui_g->io.mouse_btn & 1u)) {
        int idx = hui__grad_drag_idx;
        /* Constrain between neighbours */
        float lo = (idx > 0)        ? g->pos[idx-1] + 0.005f : 0.0f;
        float hi = (idx < g->n - 1) ? g->pos[idx+1] - 0.005f : 1.0f;
        float nt = hui_clampf((float)(hui_g->io.mouse_x - ramp_r.x) / (float)ramp_r.w, lo, hi);
        if (nt != g->pos[idx]) {
            g->pos[idx] = nt;
            changed = true;
        }
    } else {
        hui__grad_drag_idx = -1;
    }

    /* Delete key removes selected stop */
    if (g->sel >= 0 && hui_key_pressed(HUI_KEY_DELETE) && g->n > 2) {
        hui__grad_remove(g, g->sel);
        g->sel = -1;
        changed = true;
    }

    return changed;
}

#endif /* HUI_NO_FLOAT_WIDGETS */

/* ---- List view (multi-select) ---- */

#define HUI__LIST_ROW_H 18

bool hui_list_view_ex(hui_rect r, const char **items, int n,
                      bool *selected, int *focus, int *scroll_y) {
    if (!items || n <= 0 || !selected || !focus || !scroll_y) return false;
    bool changed = false;

    int content_h = n * HUI__LIST_ROW_H;
    hui_rect content = hui_scroll_begin(r, content_h, scroll_y);

    for (int i = 0; i < n; i++) {
        int row_y = content.y + i * HUI__LIST_ROW_H - *scroll_y;
        if (row_y + HUI__LIST_ROW_H < r.y || row_y > r.y + r.h) continue;

        hui_rect row  = hui_rect_make(content.x, row_y, content.w, HUI__LIST_ROW_H);
        bool hov      = hui_is_hovered(row);
        bool is_focus = (*focus == i);

        hui_color bg = selected[i] ? CUM_ACCENT
                     : is_focus    ? CUM_BG4
                     : hov         ? CUM_BG3
                                   : CUM_BG1;
        hui_rect_fill(row, bg, 0);

        hui_color fg = selected[i] ? CUM_BG0 : CUM_FG;
        if (items[i])
            hui_label(items[i], hui_rect_make(row.x+4, row.y, row.w-8, row.h),
                      HUI_ALIGN_LEFT, fg);

        if (hui_is_clicked(row, 0)) {
            selected[i] = !selected[i];
            *focus = i;
            changed = true;
        }
    }

    hui_scroll_end();
    hui_rect_outline(r, CUM_BG4, 0);
    return changed;
}

/* ---- Combo box ---- */

#define HUI__COMBO_ITEM_H 18
#define HUI__COMBO_MAX_VISIBLE 8

bool hui_combo_box(const char *label, hui_rect r, const char **items, int n,
                   int *active, bool *open) {
    if (!items || n <= 0 || !active || !open) return false;
    bool changed = false;
    const int arrow_w = r.h;

    /* Closed button */
    hui_rect btn  = r;
    hui_rect arrow_r = hui_rect_make(r.x + r.w - arrow_w, r.y, arrow_w, r.h);
    hui_rect text_r  = hui_rect_make(r.x + 4, r.y, r.w - arrow_w - 8, r.h);

    bool hov = hui_is_hovered(btn);
    hui_rect_fill(btn, hov ? CUM_BG3 : CUM_BG2, 3);
    hui_rect_outline(btn, *open ? CUM_ACCENT : CUM_BG4, 3);

    /* current item text */
    const char *cur = (*active >= 0 && *active < n) ? items[*active] : (label ? label : "");
    if (cur) hui_label(cur, text_r, HUI_ALIGN_LEFT, CUM_FG);

    /* arrow ▼ as two lines */
    int ax = arrow_r.x + arrow_r.w/2, ay = arrow_r.y + arrow_r.h/2 - 1;
    hui_line(ax - 4, ay - 2, ax,     ay + 2, CUM_FG2, 1);
    hui_line(ax,     ay + 2, ax + 4, ay - 2, CUM_FG2, 1);

    /* toggle open on click */
    if (hui_is_clicked(btn, 0)) *open = !*open;

    /* ---- Dropdown (rendered in POPUP layer) ---- */
    if (*open) {
        int vis    = n < HUI__COMBO_MAX_VISIBLE ? n : HUI__COMBO_MAX_VISIBLE;
        int drop_h = vis * HUI__COMBO_ITEM_H;
        /* try below first, flip above if off-screen */
        int drop_y = r.y + r.h;
        if (hui_g && drop_y + drop_h > hui_g->screen_h)
            drop_y = r.y - drop_h;
        hui_rect drop = hui_rect_make(r.x, drop_y, r.w, drop_h);

        hui_set_layer(HUI_LAYER_POPUP);
        hui_rect_fill(drop, CUM_BG1, 3);
        hui_rect_outline(drop, CUM_BG4, 3);
        hui_clip_push(drop);

        for (int i = 0; i < vis; i++) {
            hui_rect row = hui_rect_make(drop.x, drop.y + i * HUI__COMBO_ITEM_H,
                                         drop.w, HUI__COMBO_ITEM_H);
            bool sel = (*active == i);
            bool rhov = hui_is_hovered(row);
            hui_rect_fill(row, sel ? CUM_ACCENT : (rhov ? CUM_BG3 : CUM_BG1), 0);
            if (items[i])
                hui_label(items[i], hui_rect_make(row.x+4,row.y,row.w-8,row.h),
                           HUI_ALIGN_LEFT, sel ? CUM_BG0 : CUM_FG);
            if (hui_is_clicked(row, 0)) {
                *active = i;
                *open   = false;
                changed = true;
            }
        }

        hui_clip_pop();

        /* close on outside click or Escape */
        if (hui_g) {
            bool outside_click = (hui_g->io.mouse_btn & 1u) &&
                                 !(hui_g->io.mouse_btn_prev & 1u) &&
                                 !hui_rect_contains(drop, hui_g->io.mouse_x, hui_g->io.mouse_y) &&
                                 !hui_rect_contains(r, hui_g->io.mouse_x, hui_g->io.mouse_y);
            if (outside_click || hui_key_pressed(HUI_KEY_ESCAPE)) *open = false;
        }
        hui_reset_layer();
    }
    return changed;
}

/* ---- Dropdown box ---- */

/* Internal static state: one filter buffer shared across all dropdown_boxes
 * (only one is typically open at a time). */
static char hui__dd_filter[64];
static int  hui__dd_filter_len;
static hui_text_state hui__dd_ts;
static uint32_t hui__dd_open_frame; /* frame when *open last went true */

static int hui__str_icontains(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0') return 1;
    for (const char *h = haystack; *h; h++) {
        const char *hp = h, *np = needle;
        while (*hp && *np && ((*hp | 0x20) == (*np | 0x20))) { hp++; np++; }
        if (!*np) return 1;
    }
    return 0;
}

bool hui_dropdown_box(const char *label, hui_rect r, const char **items, int n,
                      int *active, bool *open) {
    if (!items || n <= 0 || !active || !open) return false;
    bool changed = false;
    const int arrow_w = r.h;

    /* Closed state — identical button to combo_box */
    if (!*open) {
        hui_rect arrow_r = hui_rect_make(r.x + r.w - arrow_w, r.y, arrow_w, r.h);
        bool hov = hui_is_hovered(r);
        hui_rect_fill(r, hov ? CUM_BG3 : CUM_BG2, 3);
        hui_rect_outline(r, CUM_BG4, 3);
        const char *cur = (*active >= 0 && *active < n) ? items[*active] : (label ? label : "");
        if (cur) hui_label(cur, hui_rect_make(r.x+4, r.y, r.w-arrow_w-8, r.h),
                           HUI_ALIGN_LEFT, CUM_FG);
        int ax = arrow_r.x + arrow_r.w/2, ay = arrow_r.y + arrow_r.h/2 - 1;
        hui_line(ax-4, ay-2, ax,   ay+2, CUM_FG2, 1);
        hui_line(ax,   ay+2, ax+4, ay-2, CUM_FG2, 1);
        if (hui_is_clicked(r, 0)) {
            *open = true;
            hui__dd_filter[0] = '\0';
            hui__dd_filter_len = 0;
            hui__dd_ts.cursor = 0;
            hui__dd_ts.focused = true;
            hui__dd_open_frame = hui_g ? hui_g->frame : 0;
        }
        return false;
    }

    /* Open state: text filter input + filtered dropdown in POPUP layer */

    /* Filter input bar at same position as the closed button */
    hui_rect input_r = hui_rect_make(r.x, r.y, r.w - arrow_w, r.h);
    hui_rect arrow_r2 = hui_rect_make(r.x + r.w - arrow_w, r.y, arrow_w, r.h);

    hui_rect_fill(r, CUM_BG1, 3);
    hui_rect_outline(r, CUM_ACCENT, 3);

    /* Text cursor + typed input */
    if (hui__dd_ts.focused && hui_g) {
        for (int i = 0; i < hui_g->io.text_typed_len && hui__dd_filter_len < 63; i++) {
            hui__dd_filter[hui__dd_filter_len++] = hui_g->io.text_typed[i];
            hui__dd_filter[hui__dd_filter_len] = '\0';
        }
        if (hui_key_pressed(HUI_KEY_BACKSPACE) && hui__dd_filter_len > 0)
            hui__dd_filter[--hui__dd_filter_len] = '\0';
    }

    /* Draw filter text with blinking cursor */
    hui_label(hui__dd_filter[0] ? hui__dd_filter : (label ? label : ""),
              hui_rect_make(input_r.x+4, input_r.y, input_r.w-8, input_r.h),
              HUI_ALIGN_LEFT,
              hui__dd_filter[0] ? CUM_FG : CUM_FG3);

    /* ▲ arrow (flipped since open) */
    int ax = arrow_r2.x + arrow_r2.w/2, ay = arrow_r2.y + arrow_r2.h/2 + 1;
    hui_rect_fill(arrow_r2, CUM_BG3, 3);
    hui_line(ax-4, ay+2, ax,   ay-2, CUM_FG2, 1);
    hui_line(ax,   ay-2, ax+4, ay+2, CUM_FG2, 1);
    if (hui_is_clicked(arrow_r2, 0)) { *open = false; return changed; }

    /* Build filtered list */
    int vis_max = HUI__COMBO_MAX_VISIBLE;
    int drop_h  = vis_max * HUI__COMBO_ITEM_H;
    int drop_y  = r.y + r.h;
    if (hui_g && drop_y + drop_h > hui_g->screen_h) drop_y = r.y - drop_h;
    hui_rect drop = hui_rect_make(r.x, drop_y, r.w, drop_h);

    hui_set_layer(HUI_LAYER_POPUP);
    hui_rect_fill(drop, CUM_BG1, 3);
    hui_rect_outline(drop, CUM_BG4, 3);
    hui_clip_push(drop);

    int row_y = drop.y;
    for (int i = 0; i < n; i++) {
        if (!hui__str_icontains(items[i] ? items[i] : "", hui__dd_filter)) continue;
        if (row_y + HUI__COMBO_ITEM_H > drop.y + drop.h) break;
        hui_rect row = hui_rect_make(drop.x, row_y, drop.w, HUI__COMBO_ITEM_H);
        bool sel  = (*active == i);
        bool rhov = hui_is_hovered(row);
        hui_rect_fill(row, sel ? CUM_ACCENT : (rhov ? CUM_BG3 : CUM_BG1), 0);
        if (items[i])
            hui_label(items[i], hui_rect_make(row.x+4,row.y,row.w-8,row.h),
                      HUI_ALIGN_LEFT, sel ? CUM_BG0 : CUM_FG);
        if (hui_is_clicked(row, 0)) {
            *active = i; *open = false; changed = true;
        }
        row_y += HUI__COMBO_ITEM_H;
    }

    hui_clip_pop();

    if (hui_g) {
        bool outside = (hui_g->io.mouse_btn & 1u) &&
                       !(hui_g->io.mouse_btn_prev & 1u) &&
                       !hui_rect_contains(drop, hui_g->io.mouse_x, hui_g->io.mouse_y) &&
                       !hui_rect_contains(r,    hui_g->io.mouse_x, hui_g->io.mouse_y);
        if (outside || hui_key_pressed(HUI_KEY_ESCAPE)) *open = false;
    }
    hui_reset_layer();
    return changed;
}

/* ---- Spinner ---- */

bool hui_spinner(const char *label, hui_rect r, int *val, int step, int mn, int mx) {
    if (!val) return false;
    const int btn_w = r.h;  /* square buttons */
    hui_rect btn_l  = hui_rect_make(r.x,              r.y, btn_w, r.h);
    hui_rect btn_r  = hui_rect_make(r.x + r.w - btn_w, r.y, btn_w, r.h);
    hui_rect center = hui_rect_make(r.x + btn_w, r.y, r.w - btn_w*2, r.h);

    bool changed = false;

    /* − button */
    hui_color bl = hui_is_hovered(btn_l) ? CUM_BG4 : CUM_BG3;
    hui_rect_fill(btn_l, bl, 3);
    hui_label("-", btn_l, HUI_ALIGN_CENTER, CUM_FG);
    if (hui_is_clicked(btn_l, 0) && *val > mn) { *val -= step; changed = true; }

    /* center: drag or display */
    hui_rect_fill(center, CUM_BG1, 0);
    char buf[32]; snprintf(buf, sizeof buf, "%d", *val);
    if (label && *label) {
        char lbuf[48]; snprintf(lbuf, sizeof lbuf, "%s: %d", label, *val);
        hui_label(lbuf, center, HUI_ALIGN_CENTER, CUM_FG);
    } else {
        hui_label(buf, center, HUI_ALIGN_CENTER, CUM_FG);
    }
    /* drag center to change */
    if (hui_is_hovered(center) && (hui_g->io.mouse_btn & 1u)) {
        int dy = (int)hui_g->io.mouse_y_prev - (int)hui_g->io.mouse_y;
        if (dy != 0) { *val += dy * step; changed = true; }
    }

    /* + button */
    hui_color br = hui_is_hovered(btn_r) ? CUM_BG4 : CUM_BG3;
    hui_rect_fill(btn_r, br, 3);
    hui_label("+", btn_r, HUI_ALIGN_CENTER, CUM_FG);
    if (hui_is_clicked(btn_r, 0) && *val < mx) { *val += step; changed = true; }

    if (changed) *val = *val < mn ? mn : (*val > mx ? mx : *val);
    return changed;
}

/* ---- Value box ---- */

bool hui_value_box(const char *label, hui_rect r, int *val, int mn, int mx, bool *edit) {
    if (!val || !edit) return false;
    bool changed = false;

    hui_color bg = *edit ? CUM_BG1 : (hui_is_hovered(r) ? CUM_BG3 : CUM_BG2);
    hui_rect_fill(r, bg, 3);
    hui_rect_outline(r, *edit ? CUM_ACCENT : CUM_BG4, 3);

    if (hui_is_clicked(r, 0)) { *edit = true; hui_focus_set(0); }

    char buf[32];
    if (label && *label)
        snprintf(buf, sizeof buf, "%s: %d", label, *val);
    else
        snprintf(buf, sizeof buf, "%d", *val);

    if (*edit) {
        /* accept digit keys and backspace */
        if (hui_g) {
            for (int k = '0'; k <= '9'; k++) {
                if (hui_key_pressed(k)) {
                    int digit = k - '0';
                    *val = *val * 10 + digit;
                    changed = true;
                }
            }
            if (hui_key_pressed(HUI_KEY_BACKSPACE)) { *val /= 10; changed = true; }
            if (hui_key_pressed(HUI_KEY_RETURN) || hui_key_pressed(HUI_KEY_ESCAPE)) {
                *edit = false;
            }
            /* click outside = commit */
            if ((hui_g->io.mouse_btn & 1u) && !hui_is_hovered(r)) *edit = false;
        }
        snprintf(buf, sizeof buf, "%d|", *val);
    }

    hui_label(buf, r, HUI_ALIGN_CENTER, *edit ? CUM_FG : CUM_FG2);
    if (changed) *val = *val < mn ? mn : (*val > mx ? mx : *val);
    return changed;
}

/* ---- List view ---- */

#define HUI__LIST_ROW_H 18
#define HUI__LIST_PAD    4

bool hui_list_view(hui_rect r, const char **items, int n, int *active, int *scroll_y) {
    if (!items || n < 0 || !active || !scroll_y) return false;

    int content_h = n * (HUI__LIST_ROW_H + 1);
    hui_rect cr = hui_scroll_begin(r, content_h, scroll_y);
    hui_clip_push(cr);

    bool changed = false;
    for (int i = 0; i < n; i++) {
        int ry = cr.y + i * (HUI__LIST_ROW_H + 1) - *scroll_y;
        hui_rect row = hui_rect_make(cr.x, ry, cr.w, HUI__LIST_ROW_H);
        if (ry + HUI__LIST_ROW_H < r.y || ry > r.y + r.h) continue; /* cull */

        bool sel = (*active == i);
        bool hov = hui_is_hovered(row);
        hui_color bg = sel ? CUM_ACCENT : (hov ? CUM_BG3 : CUM_BG2);
        hui_rect_fill(row, bg, 0);

        if (items[i]) {
            hui_rect lr = hui_rect_make(row.x + HUI__LIST_PAD, row.y,
                                        row.w - HUI__LIST_PAD*2, row.h);
            hui_label(items[i], lr, HUI_ALIGN_LEFT, sel ? CUM_BG0 : CUM_FG);
        }

        if (hui_is_clicked(row, 0) && !sel) { *active = i; changed = true; }
    }

    hui_clip_pop();
    hui_scroll_end();
    return changed;
}

/* ---- Zoom slider ---- */

bool hui_zoom_slider_f(hui_rect r, float *lo, float *hi,
                       float total_min, float total_max) {
    if (!lo || !hi) return false;
    bool changed = false;
    float range  = total_max - total_min;
    if (range <= 0.0f) return false;

    /* clamp to valid range */
    if (*lo < total_min) *lo = total_min;
    if (*hi > total_max) *hi = total_max;
    if (*lo > *hi)       *lo = *hi;

    /* compute pixel positions */
    int track_x  = r.x;
    int track_w  = r.w;
    int lo_px    = track_x + (int)((*lo - total_min) / range * (float)track_w);
    int hi_px    = track_x + (int)((*hi - total_min) / range * (float)track_w);
    const int handle_w = 6;

    /* track background */
    hui_rect_fill(r, CUM_BG1, (uint8_t)(r.h/2));
    /* filled range */
    hui_rect_fill(hui_rect_make(lo_px, r.y, hi_px - lo_px, r.h),
                  CUM_ACCENT, (uint8_t)(r.h/2));

    /* left handle */
    hui_rect lh = hui_rect_make(lo_px - handle_w/2, r.y, handle_w, r.h);
    hui_rect_fill(lh, hui_is_hovered(lh) ? CUM_BG4 : CUM_BG3, 2);
    if (hui_is_hovered(lh) && (hui_g->io.mouse_btn & 1u)) {
        int dx = (int)hui_g->io.mouse_x - (int)hui_g->io.mouse_x_prev;
        if (dx) { *lo += (float)dx / (float)track_w * range; changed = true; }
    }

    /* right handle */
    hui_rect rh = hui_rect_make(hi_px - handle_w/2, r.y, handle_w, r.h);
    hui_rect_fill(rh, hui_is_hovered(rh) ? CUM_BG4 : CUM_BG3, 2);
    if (hui_is_hovered(rh) && (hui_g->io.mouse_btn & 1u)) {
        int dx = (int)hui_g->io.mouse_x - (int)hui_g->io.mouse_x_prev;
        if (dx) { *hi += (float)dx / (float)track_w * range; changed = true; }
    }

    /* center drag (pan both handles) */
    hui_rect center = hui_rect_make(lo_px + handle_w/2, r.y,
                                    hi_px - lo_px - handle_w, r.h);
    if (center.w > 0 && hui_is_hovered(center) && (hui_g->io.mouse_btn & 1u)) {
        int dx = (int)hui_g->io.mouse_x - (int)hui_g->io.mouse_x_prev;
        if (dx) {
            float delta = (float)dx / (float)track_w * range;
            *lo += delta; *hi += delta;
            changed = true;
        }
    }

    /* clamp + prevent lo > hi */
    if (*lo < total_min) *lo = total_min;
    if (*hi > total_max) *hi = total_max;
    if (*lo > *hi - 1e-6f) { *lo = *hi - 1e-6f; }
    return changed;
}

/* ---- Curve editor ---- */

void hui_curve_init(hui_curve *c) {
    if (!c) return;
    c->n    = 2;
    c->sel  = -1;
    c->type = HUI_CURVE_LINEAR;
    c->x[0] = 0.0f; c->y[0] = 0.0f;
    c->x[1] = 1.0f; c->y[1] = 1.0f;
}

float hui_curve_eval(const hui_curve *c, float x) {
    if (!c || c->n == 0) return 0.0f;
    if (c->n == 1)        return c->y[0];
    if (x <= c->x[0])     return c->y[0];
    if (x >= c->x[c->n-1]) return c->y[c->n-1];

    /* find segment i such that x[i] <= x < x[i+1] */
    int i = 0;
    while (i < c->n - 2 && c->x[i+1] <= x) i++;
    float t = (x - c->x[i]) / (c->x[i+1] - c->x[i]);

    if (c->type == HUI_CURVE_LINEAR)
        return c->y[i] + t * (c->y[i+1] - c->y[i]);

    /* Catmull-Rom spline */
    float p0 = (i > 0)       ? c->y[i-1]   : c->y[i];
    float p1 = c->y[i];
    float p2 = c->y[i+1];
    float p3 = (i+2 < c->n)  ? c->y[i+2]   : c->y[i+1];
    float t2 = t * t, t3 = t2 * t;
    float v = 0.5f * ((2.0f*p1)
              + (-p0 + p2) * t
              + (2.0f*p0 - 5.0f*p1 + 4.0f*p2 - p3) * t2
              + (-p0 + 3.0f*p1 - 3.0f*p2 + p3) * t3);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Insert a point keeping x[] sorted. Returns index of new point, or -1 if full/duplicate. */
static int hui__curve_insert(hui_curve *c, float x, float y) {
    if (c->n >= HUI_CURVE_MAX_POINTS) return -1;
    /* Reject if x is within epsilon of an existing point (prevents div-by-zero in eval) */
    for (int i = 0; i < c->n; i++) {
        if (c->x[i] >= x - 0.004f && c->x[i] <= x + 0.004f) return -1;
    }
    int ins = c->n;
    for (int i = 0; i < c->n; i++) {
        if (c->x[i] > x) { ins = i; break; }
    }
    for (int i = c->n; i > ins; i--) {
        c->x[i] = c->x[i-1]; c->y[i] = c->y[i-1];
    }
    c->x[ins] = x; c->y[ins] = y;
    c->n++;
    return ins;
}

/* Remove point at index k. */
static void hui__curve_remove(hui_curve *c, int k) {
    if (c->n <= 2 || k < 0 || k >= c->n) return;
    for (int i = k; i < c->n - 1; i++) {
        c->x[i] = c->x[i+1]; c->y[i] = c->y[i+1];
    }
    c->n--;
    if (c->sel >= c->n) c->sel = c->n - 1;
}

static int hui__curve_drag_idx = -1;

bool hui_curve_edit(hui_rect r, hui_curve *c, hui_color col) {
    if (!c || !hui_g) return false;
    bool changed = false;
    const int PH = 5;  /* point hit radius */

    /* Coordinate helpers: curve [0,1] ↔ screen pixels */
#define HUI__CV_PX(cx) (r.x + (int)((cx) * (float)(r.w)))
#define HUI__CV_PY(cy) (r.y + r.h - 1 - (int)((cy) * (float)(r.h)))
#define HUI__CV_CX(px) (((float)((px) - r.x)) / (float)(r.w))
#define HUI__CV_CY(py) (1.0f - ((float)((py) - r.y)) / (float)(r.h))

    hui_color bg    = CUM_BG1;
    hui_color grid  = HUI_RGBA(80, 80, 80, 80);
    hui_color pt_c  = CUM_BG4;
    hui_color pt_sel= CUM_ACCENT;

    /* Background */
    hui_rect_fill(r, bg, 3);

    /* Grid: 4×4 subdivisions */
    for (int gi = 1; gi < 4; gi++) {
        int gx = r.x + r.w * gi / 4;
        int gy = r.y + r.h * gi / 4;
        hui_line(gx, r.y, gx, r.y + r.h, grid, 1);
        hui_line(r.x, gy, r.x + r.w, gy, grid, 1);
    }

    /* Curve: draw as piecewise line segments (64 steps) */
    {
        const int SEG = 64;
        int px_prev = 0, py_prev = 0;
        for (int si = 0; si <= SEG; si++) {
            float t = (float)si / (float)SEG;
            float y_val = hui_curve_eval(c, t);
            int px = HUI__CV_PX(t);
            int py = HUI__CV_PY(y_val);
            if (si > 0)
                hui_line(px_prev, py_prev, px, py, col, 1);
            px_prev = px; py_prev = py;
        }
    }

    /* Control points */
    bool lmb     = (hui_g->io.mouse_btn & 1u) != 0;
    bool lmb_was = (hui_g->io.mouse_btn_prev & 1u) != 0;
    bool lmb_dn  = lmb && !lmb_was;
    bool lmb_up  = !lmb && lmb_was;
    bool rmb_dn  = (hui_g->io.mouse_btn & 2u) && !(hui_g->io.mouse_btn_prev & 2u);
    int mx = hui_g->io.mouse_x, my = hui_g->io.mouse_y;
    bool inside = hui_is_hovered(r);

    /* Release drag */
    if (lmb_up) hui__curve_drag_idx = -1;

    /* Drag active point */
    if (hui__curve_drag_idx >= 0 && lmb && inside) {
        int k = hui__curve_drag_idx;
        float nx = HUI__CV_CX(mx);
        float ny = HUI__CV_CY(my);
        if (nx < 0.0f) { nx = 0.0f; } else if (nx > 1.0f) { nx = 1.0f; }
        if (ny < 0.0f) { ny = 0.0f; } else if (ny > 1.0f) { ny = 1.0f; }
        /* clamp x between neighbours (keep sorted) */
        float lo = (k > 0)       ? c->x[k-1] + 0.005f : 0.0f;
        float hi = (k < c->n-1)  ? c->x[k+1] - 0.005f : 1.0f;
        if (nx < lo) { nx = lo; } else if (nx > hi) { nx = hi; }
        /* Don't allow dragging endpoints off the edges */
        if (k == 0)      { nx = 0.0f; }
        if (k == c->n-1) { nx = 1.0f; }
        if (c->x[k] != nx || c->y[k] != ny) {
            c->x[k] = nx; c->y[k] = ny;
            changed = true;
        }
    }

    /* Delete selected on key */
    if (c->sel >= 0 && hui_key_pressed(HUI_KEY_DELETE)) {
        hui__curve_remove(c, c->sel);
        c->sel = -1;
        hui__curve_drag_idx = -1;
        changed = true;
    }

    for (int i = 0; i < c->n; i++) {
        int px = HUI__CV_PX(c->x[i]);
        int py = HUI__CV_PY(c->y[i]);
        bool hov = inside && (mx-px)*(mx-px) + (my-py)*(my-py) <= PH*PH;

        hui_circle_fill(px, py, PH, hov ? pt_sel : (c->sel == i ? pt_sel : pt_c));
        hui_circle(px, py, PH, col);

        if (hov) {
            /* LMB: select + start drag */
            if (lmb_dn) {
                c->sel = i;
                hui__curve_drag_idx = i;
            }
            /* RMB: remove */
            if (rmb_dn) {
                hui__curve_remove(c, i);
                changed = true;
            }
            hui_tooltip(hui_fmt("(%.2f, %.2f)", c->x[i], c->y[i]));
        }
    }

    /* LMB on empty area → insert */
    if (lmb_dn && inside && hui__curve_drag_idx < 0) {
        float nx = HUI__CV_CX(mx);
        float ny = HUI__CV_CY(my);
        if (nx < 0.0f) { nx = 0.0f; } else if (nx > 1.0f) { nx = 1.0f; }
        if (ny < 0.0f) { ny = 0.0f; } else if (ny > 1.0f) { ny = 1.0f; }
        int ni = hui__curve_insert(c, nx, ny);
        if (ni >= 0) {
            c->sel = ni;
            hui__curve_drag_idx = ni;
            changed = true;
        }
    }

    /* Border */
    hui_rect_outline(r, col, 2);

#undef HUI__CV_PX
#undef HUI__CV_PY
#undef HUI__CV_CX
#undef HUI__CV_CY

    return changed;
}

/* ---- Light rig ---- */

static int hui__light_drag = -1;

bool hui_light_rig(hui_rect r, hui_light *lights, int n) {
    if (!lights || n <= 0 || !hui_g) return false;
    bool changed = false;

    /* Largest square centered in r */
    int sz  = r.w < r.h ? r.w : r.h;
    int cx  = r.x + r.w / 2;
    int cy  = r.y + r.h / 2;
    int rad = sz / 2 - 4;
    if (rad <= 0) return false;

    hui_color bg_c   = CUM_BG1;
    hui_color ring_c = HUI_RGBA(80, 80, 80, 120);

    /* Background disc */
    hui_circle_fill(cx, cy, rad, bg_c);
    hui_circle(cx, cy, rad, CUM_BG3);

    /* Elevation rings at ~33% and ~67% disk radius */
    hui_circle(cx, cy, rad / 3,     ring_c);
    hui_circle(cx, cy, rad * 2 / 3, ring_c);

    /* Cross-hair */
    hui_line(cx - rad, cy, cx + rad, cy, ring_c, 1);
    hui_line(cx, cy - rad, cx, cy + rad, ring_c, 1);

    /* "N" indicator at top */
    hui_text(cx - HUI_FONT_W / 2, cy - rad - HUI_FONT_H - 2, "N", CUM_FG2);

    /* Mouse state */
    int mx  = hui_g->io.mouse_x;
    int my  = hui_g->io.mouse_y;
    bool lmb    = (hui_g->io.mouse_btn      & 1u) != 0;
    bool lmb_dn = lmb && !(hui_g->io.mouse_btn_prev & 1u);
    bool lmb_up = !lmb && (hui_g->io.mouse_btn_prev & 1u);
    if (lmb_up) hui__light_drag = -1;

    /* Update dragged light */
    if (hui__light_drag >= 0 && hui__light_drag < n && lmb) {
        float u = (float)(mx - cx) / (float)rad;
        float v = (float)(my - cy) / (float)rad;
        float d = sqrtf(u * u + v * v);
        if (d > 1.0f) { u /= d; v /= d; d = 1.0f; }
        float az_r = atan2f(u, -v);
        float az   = az_r * (180.0f / 3.14159265f);
        if (az < 0.0f) az += 360.0f;
        float el   = acosf(d) * (180.0f / 3.14159265f);
        lights[hui__light_drag].az = az;
        lights[hui__light_drag].el = el;
        changed = true;
    }

    /* Draw orbs */
    const int ORB_R = 7;
    for (int i = 0; i < n; i++) {
        float az_r = lights[i].az * (3.14159265f / 180.0f);
        float el_r = lights[i].el * (3.14159265f / 180.0f);
        float u    = sinf(az_r) * cosf(el_r);
        float v    = -cosf(az_r) * cosf(el_r);
        int   px   = cx + (int)(u * (float)rad);
        int   py   = cy + (int)(v * (float)rad);

        hui_color fill = lights[i].on
            ? lights[i].col
            : HUI_RGBA(80, 80, 80, 180);
        hui_circle_fill(px, py, ORB_R, fill);
        hui_circle(px, py, ORB_R, hui__light_drag == i ? CUM_ACCENT : CUM_FG);

        /* Index label inside orb */
        if (ORB_R >= 6)
            hui_text(px - HUI_FONT_W / 2, py - HUI_FONT_H / 2,
                     hui_fmt("%d", i), CUM_BG0);

        bool hov = (mx-px)*(mx-px) + (my-py)*(my-py) <= ORB_R*ORB_R;
        if (hov) {
            if (lmb_dn) hui__light_drag = i;
            hui_tooltip(hui_fmt("L%d  az:%.0f°  el:%.0f°  %s",
                                i, lights[i].az, lights[i].el,
                                lights[i].on ? "on" : "off"));
        }
    }

    return changed;
}

/* ---- Sequencer ---- */

/* Helper: compute a "nice" tick step for a frame range. */
static int hui__seq_tick_step(int range) {
    /* Pick a step such that ~5-15 minor ticks are visible. */
    static const int steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000,
                                 2000, 5000, 10000, 0};
    int target = range / 10;
    if (target < 1) target = 1;
    for (int i = 0; steps[i] != 0; i++) {
        if (steps[i] >= target) return steps[i];
    }
    return 1000;
}

bool hui_sequencer(hui_rect r, hui_seq_track *tracks, int n_tracks, hui_seq_state *state) {
    if (!tracks || n_tracks <= 0 || !state) return false;

    bool changed = false;

    /* Layout constants */
    const int LEFT_W   = 120;  /* left panel width (track names) */
    const int HEADER_H = 24;   /* ruler height */
    const int TRACK_H  = 28;   /* height per track row */
    const int RESIZE_Z = 4;    /* resize handle grab width (px) */

    /* Sub-rects */
    hui_rect left_r   = hui_rect_make(r.x, r.y + HEADER_H, LEFT_W, r.h - HEADER_H);
    hui_rect header_r = hui_rect_make(r.x + LEFT_W, r.y, r.w - LEFT_W, HEADER_H);
    hui_rect tl_r     = hui_rect_make(r.x + LEFT_W, r.y + HEADER_H,
                                      r.w - LEFT_W, r.h - HEADER_H);

    int tl_x = tl_r.x;
    int tl_w = tl_r.w;
    int frame_range = state->frame_max - state->frame_min;
    if (frame_range <= 0) frame_range = 1;
    float px_per_frame = (float)tl_w / (float)frame_range;

    /* Helpers: frame <-> pixel */
#define HUI_SEQ_FRAME_TO_PX(f) \
    (tl_x + (int)((float)((f) - state->frame_min) * px_per_frame))
#define HUI_SEQ_PX_TO_FRAME(px) \
    (state->frame_min + (int)((float)((px) - tl_x) / px_per_frame))

    /* Mouse state */
    int  mx     = hui_g->io.mouse_x;
    int  my     = hui_g->io.mouse_y;
    bool lmb    = (hui_g->io.mouse_btn      & 1u) != 0;
    bool lmb_dn = lmb && !(hui_g->io.mouse_btn_prev & 1u);
    bool lmb_up = !lmb && (hui_g->io.mouse_btn_prev & 1u);

    /* Release drag */
    if (lmb_up) {
        state->_drag_item  = -1;
        state->_drag_track = -1;
    }

    /* ---- Overall background ---- */
    hui_rect_fill(r, CUM_BG0, 0);

    /* ---- Left panel ---- */
    hui_rect_fill(left_r, CUM_BG2, 0);
    /* 1px border on right edge */
    hui_line(left_r.x + left_r.w - 1, left_r.y,
             left_r.x + left_r.w - 1, left_r.y + left_r.h, CUM_BG3, 1);

    for (int t = 0; t < n_tracks; t++) {
        int ty = left_r.y + t * TRACK_H;
        if (ty + TRACK_H > left_r.y + left_r.h) break; /* clip */
        hui_rect row = hui_rect_make(left_r.x, ty, LEFT_W - 1, TRACK_H);
        /* Left panel uses solid BG2; alternating is handled in the timeline area */
        hui_rect_fill(row, CUM_BG2, 0);
        /* Bottom separator */
        hui_line(row.x, ty + TRACK_H - 1, row.x + row.w, ty + TRACK_H - 1, CUM_BG3, 1);
        /* Track name, vertically centered */
        int ty_text = ty + (TRACK_H - HUI_FONT_H) / 2;
        hui_clip_push(row);
        hui_text(row.x + 4, ty_text, tracks[t].name, CUM_FG);
        hui_clip_pop();
    }

    /* ---- Frame ruler ---- */
    hui_rect_fill(header_r, CUM_BG1, 0);

    int tick_minor = hui__seq_tick_step(frame_range);
    int tick_major = tick_minor * 10;

    /* First frame aligned to tick */
    int first_tick = (state->frame_min / tick_minor) * tick_minor;
    if (first_tick < state->frame_min) first_tick += tick_minor;

    hui_clip_push(header_r);
    for (int f = first_tick; f < state->frame_max; f += tick_minor) {
        int px = HUI_SEQ_FRAME_TO_PX(f);
        if (px < header_r.x || px > header_r.x + header_r.w) continue;
        bool major = (f % tick_major == 0);
        int  tick_h = major ? (HEADER_H / 2) : (HEADER_H / 4);
        hui_line(px, header_r.y + HEADER_H - tick_h,
                 px, header_r.y + HEADER_H, CUM_BG3, 1);
        if (major) {
            hui_text(px + 2, header_r.y + 2, hui_fmt("%d", f), CUM_FG2);
        }
    }

    /* Playhead triangle on ruler */
    {
        int phx = HUI_SEQ_FRAME_TO_PX(state->current_frame);
        /* Filled triangle: tip at bottom of header, base 5px wide at top */
        int tx = phx;
        int ty_tip = header_r.y + HEADER_H - 1;
        int ty_top = header_r.y + 2;
        hui_line(tx,     ty_top, tx - 4, ty_top,    CUM_ACCENT, 1);
        hui_line(tx,     ty_top, tx + 4, ty_top,    CUM_ACCENT, 1);
        hui_line(tx - 4, ty_top, tx,     ty_tip,    CUM_ACCENT, 1);
        hui_line(tx + 4, ty_top, tx,     ty_tip,    CUM_ACCENT, 1);
        /* Fill triangle body with two inner lines */
        for (int dy = 1; dy < ty_tip - ty_top; dy++) {
            int half = (dy * 4) / (ty_tip - ty_top);
            hui_line(tx - half, ty_top + dy, tx + half, ty_top + dy, CUM_ACCENT, 1);
        }
    }

    /* Ruler click/drag → scrub current_frame */
    if (hui_is_hovered(header_r) && lmb) {
        int new_frame = HUI_SEQ_PX_TO_FRAME(mx);
        if (new_frame < state->frame_min) new_frame = state->frame_min;
        if (new_frame >= state->frame_max) new_frame = state->frame_max - 1;
        if (new_frame != state->current_frame) {
            state->current_frame = new_frame;
            changed = true;
        }
    }
    hui_clip_pop();

    /* ---- Timeline tracks ---- */
    hui_clip_push(tl_r);

    for (int t = 0; t < n_tracks; t++) {
        int ty = tl_r.y + t * TRACK_H;
        if (ty + TRACK_H > tl_r.y + tl_r.h) break;

        /* Alternating row background */
        hui_color row_bg = (t & 1) ? CUM_BG0 : CUM_BG1;
        hui_rect_fill(hui_rect_make(tl_r.x, ty, tl_r.w, TRACK_H), row_bg, 0);
        /* Bottom separator */
        hui_line(tl_r.x, ty + TRACK_H - 1,
                 tl_r.x + tl_r.w, ty + TRACK_H - 1, CUM_BG3, 1);

        /* Draw items */
        for (int i = 0; i < tracks[t].item_count && i < 32; i++) {
            hui_seq_item *item = &tracks[t].items[i];

            int x0 = HUI_SEQ_FRAME_TO_PX(item->start_frame);
            int x1 = HUI_SEQ_FRAME_TO_PX(item->end_frame);
            if (x1 <= tl_r.x || x0 >= tl_r.x + tl_r.w) continue;
            if (x0 < tl_r.x) x0 = tl_r.x;
            if (x1 > tl_r.x + tl_r.w) x1 = tl_r.x + tl_r.w;

            int iw = x1 - x0;
            if (iw < 1) iw = 1;

            bool has_item_color = (item->color.r | item->color.g | item->color.b | item->color.a) != 0;
            hui_color fill = has_item_color ? item->color : tracks[t].color;
            /* Lighter outline: brighten each channel by 40 */
            uint8_t fr = fill.r;
            uint8_t fg = fill.g;
            uint8_t fb = fill.b;
            uint8_t fa = fill.a;
#define HUI__BRIGHT(v, d) (uint8_t)(((int)(v) + (d)) > 255 ? 255 : ((int)(v) + (d)))
            hui_color outline = HUI_RGBA(HUI__BRIGHT(fr,40), HUI__BRIGHT(fg,40),
                                         HUI__BRIGHT(fb,40), fa);
#undef HUI__BRIGHT

            hui_rect item_r = hui_rect_make(x0, ty + 2, iw, TRACK_H - 4);
            hui_rect_fill(item_r, fill, 2);
            hui_rect_outline(item_r, outline, 2);

            /* Clip label to item rect */
            if (iw > 6) {
                hui_clip_push(hui_rect_make(x0 + 2, ty + 2, iw - 4, TRACK_H - 4));
                hui_text(x0 + 3, ty + (TRACK_H - HUI_FONT_H) / 2, item->label, CUM_FG);
                hui_clip_pop();
            }

            /* Interaction: body move / right-edge resize */
            hui_rect body_r = item_r;

            /* Detect ongoing drag for this item */
            bool this_drag = (state->_drag_item  == i &&
                              state->_drag_track == t);

            if (this_drag && lmb) {
                /* Continue drag */
                int cur_frame = HUI_SEQ_PX_TO_FRAME(mx);
                if (state->_drag_right) {
                    /* Resize: move end_frame */
                    int new_end = cur_frame + 1;
                    if (new_end <= item->start_frame + 1)
                        new_end = item->start_frame + 1;
                    if (new_end > state->frame_max) new_end = state->frame_max;
                    if (new_end != item->end_frame) {
                        item->end_frame = new_end;
                        changed = true;
                    }
                } else {
                    /* Move: translate whole item */
                    int len = item->end_frame - item->start_frame;
                    int new_start = cur_frame - state->_drag_offset;
                    if (new_start < state->frame_min) new_start = state->frame_min;
                    if (new_start + len > state->frame_max)
                        new_start = state->frame_max - len;
                    if (new_start != item->start_frame) {
                        item->start_frame = new_start;
                        item->end_frame   = new_start + len;
                        changed = true;
                    }
                }
            } else if (!this_drag && lmb_dn &&
                       mx >= body_r.x && mx < body_r.x + body_r.w &&
                       my >= body_r.y && my < body_r.y + body_r.h) {
                /* Start drag */
                state->_drag_track = t;
                state->_drag_item  = i;
                bool right_edge = (mx >= body_r.x + body_r.w - RESIZE_Z);
                state->_drag_right  = right_edge;
                state->_drag_offset = HUI_SEQ_PX_TO_FRAME(mx) - item->start_frame;
            }
        }
    }

    /* Playhead vertical line across full timeline height */
    {
        int phx = HUI_SEQ_FRAME_TO_PX(state->current_frame);
        if (phx >= tl_r.x && phx <= tl_r.x + tl_r.w) {
            hui_line(phx, tl_r.y, phx, tl_r.y + tl_r.h, CUM_ACCENT, 1);
        }
    }

    hui_clip_pop();

    /* Outer border */
    hui_rect_outline(r, CUM_BG3, 0);

#undef HUI_SEQ_FRAME_TO_PX
#undef HUI_SEQ_PX_TO_FRAME

    return changed;
}

#endif /* HUI_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_WIDGETS_H */
