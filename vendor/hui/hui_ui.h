/*
 * hui_ui.h — Ease-of-use convenience layer for hui
 *
 * Optional sugar on top of hui_widgets.h. Include after hui.h:
 *
 *   #define HUI_IMPLEMENTATION
 *   #include "hui.h"
 *   #include "hui_ui.h"   // optional — not needed for core API
 *
 * All macros auto-allocate their row via hui_panel_row() and the
 * matching HUI_ROW_* constant.  Must be called inside a
 * hui_panel_begin() / hui_panel_end() block.
 *
 * Row-height constants (HUI_ROW_BTN, HUI_ROW_LABEL, etc.) and
 * hui_panel_row_split() are declared in hui_widgets.h and available
 * without this header.
 */

#ifndef HUI_UI_H
#define HUI_UI_H

#include "hui_widgets.h"

/* ---- Single-line widget macros ---- */

/* Button — returns true on click */
#define hui_ui_button(label) \
    hui_button((label), hui_panel_row(HUI_ROW_BTN))

/* Toggle button — returns true when state changes */
#define hui_ui_toggle(label, val) \
    hui_toggle((label), hui_panel_row(HUI_ROW_BTN), (val))

/* Checkbox — returns true when state changes */
#define hui_ui_checkbox(label, val) \
    hui_checkbox((label), hui_panel_row(HUI_ROW_BTN), (val))

/* Float slider */
#define hui_ui_slider_f(label, val, mn, mx) \
    hui_slider_f((label), hui_panel_row(HUI_ROW_SLIDER), (val), (mn), (mx))

/* Integer slider */
#define hui_ui_slider_i(label, val, mn, mx) \
    hui_slider_i((label), hui_panel_row(HUI_ROW_SLIDER), (val), (mn), (mx))

/* Left-aligned label in default foreground color */
#define hui_ui_label(text) \
    hui_label((text), hui_panel_row(HUI_ROW_LABEL), HUI_ALIGN_LEFT, CUM_FG)

/* Left-aligned label with explicit color */
#define hui_ui_label2(text, col) \
    hui_label((text), hui_panel_row(HUI_ROW_LABEL), HUI_ALIGN_LEFT, (col))

/* Horizontal separator line */
#define hui_ui_sep() \
    hui_separator(hui_panel_row(HUI_ROW_SEP))

/* Small vertical gap */
#define hui_ui_gap() \
    hui_panel_spacing(HUI_ROW_SM_GAP)

/* Large vertical gap */
#define hui_ui_gap_lg() \
    hui_panel_spacing(HUI_ROW_LG_GAP)

/* Text input field */
#define hui_ui_input(st, buf, sz) \
    hui_text_input(hui_panel_row(HUI_ROW_INPUT), (st), (buf), (sz))

/* Progress bar */
#define hui_ui_progress(val, fg, bg) \
    hui_progress_bar(hui_panel_row(HUI_ROW_PROG), (val), (fg), (bg))

/* Collapsible section header with a small gap above it.
   Expands to a bool expression — use in an if() to wrap contents. */
#define hui_ui_section(title, open) \
    (hui_panel_spacing(HUI_ROW_SM_GAP), \
     hui_collapsible((title), hui_panel_row(HUI_ROW_BTN), (open)))

/* Tab bar — wraps hui_tab_bar; returns true if active tab changed */
#define hui_ui_tab(labels, n, active) \
    hui_tab_bar(hui_panel_row(HUI_ROW_TAB), (labels), (n), (active))

/* Drag-to-edit float */
#define hui_ui_drag_f(label, val, speed) \
    hui_drag_f((label), hui_panel_row(HUI_ROW_BTN), (val), (speed))

/* Layout helpers — thin wrappers so callers never touch hui_panel_* directly */
#define hui_ui_same_line()      hui_panel_same_line()
#define hui_ui_indent(px)       hui_panel_indent(px)
#define hui_ui_unindent(px)     hui_panel_unindent(px)

/* ---- Panel / window wrappers ---- */

/* Open a panel with the standard default rounding (4px).
   Must be paired with hui_ui_window_end(). */
#define hui_ui_window_begin(title, rect) \
    hui_panel_begin((title), (rect), 4)

#define hui_ui_window_end() \
    hui_panel_end()

/* ---- Compound helpers ---- */

/* Section header: small gap + accent-colored label in a LABEL row.
   Purely visual — no toggle. Use hui_ui_section() for collapsible. */
#define hui_ui_header(text) \
    do { \
        hui_panel_spacing(HUI_ROW_SM_GAP); \
        hui_label((text), hui_panel_row(HUI_ROW_LABEL), HUI_ALIGN_LEFT, CUM_ACCENT); \
    } while (0)

/* Read-only value row: "label  <value>" right-aligned in a LABEL row.
   hui_ui_value_f(label, val)  — prints %.3f
   hui_ui_value_i(label, val)  — prints %d   */
#define hui_ui_value_f(label, val) \
    do { \
        hui_rect _r = hui_panel_row(HUI_ROW_LABEL); \
        hui_label((label), _r, HUI_ALIGN_LEFT,  CUM_FG2); \
        hui_label(hui_fmt("%.3f", (double)(val)), _r, HUI_ALIGN_RIGHT, CUM_FG); \
    } while (0)

#define hui_ui_value_i(label, val) \
    do { \
        hui_rect _r = hui_panel_row(HUI_ROW_LABEL); \
        hui_label((label), _r, HUI_ALIGN_LEFT,  CUM_FG2); \
        hui_label(hui_fmt("%d", (int)(val)),     _r, HUI_ALIGN_RIGHT, CUM_FG); \
    } while (0)

/* Allocate the next panel row and split it into n equal columns with gap px
   between them. out must be hui_rect[n]. Combines hui_panel_row + hui_panel_row_split. */
#define hui_ui_row(h, n, gap, out) \
    hui_panel_row_split(hui_panel_row(h), (n), (gap), (out))

#endif /* HUI_UI_H */
