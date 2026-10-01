/*
 * hui_debug.h — Debug overlays for hui
 *
 * stb-style single header.
 * Define HUI_DEBUG_IMPLEMENTATION in exactly one translation unit.
 * Requires hui.h (with HUI_IMPLEMENTATION) to be included first.
 *
 * Included tools:
 *   hui_draw_list_inspector(r)      — live draw-command breakdown by type
 *   hui_fps_overlay(r)              — FPS + frame-time overlay
 *   hui_debug_grid_toggle()         — toggle snap-grid overlay (key: 'G')
 *   hui_debug_grid_overlay(r, step, c) — draw grid if toggled on
 *   hui_hover_inspector()           — floating mouse-pos + scroll tooltip
 *   hui_layout_inspector_toggle()   — toggle layout rect overlay (key: 'L')
 *   hui_layout_inspector_active()   — query layout inspector state
 *   hui_layout_inspector_push()     — register a rect+ID for overlay
 *   hui_layout_inspector_flush()    — draw all registered rects as overlays
 *   hui_theme_cycle()               — cycle runtime theme (Ristretto/Dark/Light)
 *   hui_theme_picker(x, y, *open)   — floating theme swatch picker widget
 *   hui_show_demo(r, *open)         — comprehensive widget demo window
 */

#ifndef HUI_DEBUG_H
#define HUI_DEBUG_H

#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render a live draw-list breakdown panel inside rect r.
 * Shows total command count plus a bar-chart breakdown by hui_cmd_type.
 * Call after hui_end_frame() (or inside the same frame after your UI code). */
void hui_draw_list_inspector(hui_rect r);

/* Toggle the snap-grid overlay on/off.
 * Typically bound to a key: if (hui_key_pressed('g')) hui_debug_grid_toggle(); */
void hui_debug_grid_toggle(void);

/* Draw a grid over rect r with given cell step and color, but only when the
 * grid overlay is currently enabled (via hui_debug_grid_toggle). */
void hui_debug_grid_overlay(hui_rect r, int step, hui_color c);

/* Show a small floating tooltip near the cursor with:
 *   mouse x, y   scroll_dy   focused widget id
 * Call once per frame while you want the inspector active. */
void hui_hover_inspector(void);

/* ---- Layout Inspector ---- */

/* Toggle layout inspector on/off.
 * Typically: if (hui_key_pressed('l')) hui_layout_inspector_toggle(); */
void hui_layout_inspector_toggle(void);

/* Returns true when the layout inspector is active. */
bool hui_layout_inspector_active(void);

/* Register a rect+ID during the frame (call from widget code or user code).
 * id can be any integer; label is a short display name (copied, max 16 chars). */
void hui_layout_inspector_push(hui_rect r, int id, const char *label);

/* Draw all registered rects as overlays. Call once per frame after your UI code. */
void hui_layout_inspector_flush(void);

/* ---- Runtime Theme Switcher ---- */

/* Cycle to the next runtime theme (Ristretto -> Dark -> Light -> ...). */
void hui_theme_cycle(void);

/* Show a floating theme picker widget near point (x, y).
 * Returns true when closed. Pass *open to auto-dismiss. */
bool hui_theme_picker(int x, int y, bool *open);

/* ---- Demo Window ---- */

/* Draw a comprehensive widget demo window at rect r.
 * *open can be NULL (always shown) or a bool pointer (show X button to close).
 * Exercises all widgets so developers can verify their build. */
void hui_show_demo(hui_rect r, bool *open);

/* ---- Frame Dump & Recording ---- */

/* Dump current framebuffer as PPM to path. Call after hui_end_frame(). */
void hui_debug_dump_frame(const char *path);

/* Dump current framebuffer as PNG to path (requires HUI_IMAGE_USE_STB). */
int  hui_debug_dump_frame_png(const char *path);

/* Start recording: dump every frame to dir/frame_NNNN.ppm on hui_debug_frame_tick().
 * Creates dir if it doesn't exist. */
void hui_debug_record_start(const char *dir);
void hui_debug_record_stop(void);
bool hui_debug_recording(void);

/* Call once per frame after hui_end_frame() to write the next recorded frame.
 * No-op when not recording. Also auto-dumps if HUI_DEBUG_DUMP_DIR is defined. */
void hui_debug_frame_tick(void);

/* Print draw list as human-readable text to f (one line per command). */
void hui_debug_dump_dl(FILE *f);

/* Write a diff image: changed pixels = red, unchanged = dimmed.
 * prev and cur are ARGB uint32_t buffers, w*h pixels. out must be w*h uint32_t. */
void hui_debug_frame_diff(const uint32_t *prev, const uint32_t *cur,
                          int w, int h, uint32_t *out);

#ifdef HUI_DEBUG_IMPLEMENTATION

#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#endif

/* ---- Grid overlay toggle ---- */

static bool hui__debug_grid_on = false;

void hui_debug_grid_toggle(void) { hui__debug_grid_on = !hui__debug_grid_on; }

void hui_debug_grid_overlay(hui_rect r, int step, hui_color c) {
    if (!hui__debug_grid_on) return;
    hui_set_layer(HUI_LAYER_OVERLAY);
    /* reuse hui_draw_grid from hui_widgets.h */
    extern void hui_draw_grid(hui_rect r, int step, hui_color c);
    hui_draw_grid(r, step, c);
    hui_reset_layer();
}

/* ---- Hover inspector ---- */

void hui_hover_inspector(void) {
    if (!hui_g) return;
    const hui_io_ctx *io = &hui_g->io;
    hui_set_layer(HUI_LAYER_OVERLAY);

    /* build info string */
    const char *line1 = hui_fmt("x:%d y:%d", io->mouse_x, io->mouse_y);
    const char *line2 = hui_fmt("scroll:%+.1f  btn:%d",
                                (double)io->scroll_dy, io->mouse_btn);
    const char *line3 = hui_fmt("focus:%d", hui_g->focus_id);

    int w = 110, h = HUI_FONT_H * 3 + 14;
    int tx = io->mouse_x + 14;
    int ty = io->mouse_y + 6;
    /* keep on screen */
    if (tx + w > hui_g->screen_w) tx = io->mouse_x - w - 4;
    if (ty + h > hui_g->screen_h) ty = io->mouse_y - h - 4;

    hui_rect box = hui_rect_make(tx, ty, w, h);
    hui_rect_fill(box, HUI_RGBA(10, 10, 15, 220), 3);
    hui_rect_outline(box, CUM_BG3, 3);
    hui_text(tx + 5, ty + 4,                      line1, CUM_YELL);
    hui_text(tx + 5, ty + 4 + HUI_FONT_H + 3,     line2, CUM_FG2);
    hui_text(tx + 5, ty + 4 + (HUI_FONT_H + 3)*2, line3, CUM_FG3);

    hui_reset_layer();
}

/* ---- Draw list inspector ---- */

void hui_draw_list_inspector(hui_rect r) {
    if (!hui_g) return;

    /* Snapshot counts per type from the *previous* frame's draw list.
     * We read hui_g->dl which has already been flushed — the counts are
     * still valid until the next hui_begin_frame resets them. */
    const hui_dl *dl = &hui_g->dl;
    uint16_t total   = dl->count;

    /* Count per command type */
    int counts[HUI_CMD_COUNT] = {0};
    for (uint16_t i = 0; i < total; i++) {
        uint8_t t = dl->cmds[i].type;
        if (t < HUI_CMD_COUNT) counts[t]++;
    }

    /* Find max for bar scaling */
    int cmax = 1;
    for (int t = 1; t < HUI_CMD_COUNT; t++)
        if (counts[t] > cmax) cmax = counts[t];

    /* Panel background */
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_rect_fill(r, HUI_RGBA(10, 10, 15, 220), 3);
    hui_rect_outline(r, CUM_BG3, 3);

    /* Header */
    int x = r.x + 6, y = r.y + 5;
    hui_text(x, y, "Draw List Inspector", CUM_FG);
    y += HUI_FONT_H + 4;
    hui_line(r.x + 4, y, r.x + r.w - 4, y, CUM_BG3, 1);
    y += 4;
    hui_text(x, y, hui_fmt("total cmds: %u / %d", total, HUI_MAX_CMDS), CUM_FG2);
    y += HUI_FONT_H + 6;

    /* Per-type bars */
    static const char *names[HUI_CMD_COUNT] = {
        "NOP", "LINE", "BEZ4", "RECT", "RECTF", "CIRC", "CIRCF",
        "HEMI", "TRI", "TRIF", "ARROW", "IMG", "TEXT",
        "CLIP+", "CLIP-", "PLTL", "PLTB", "PLTH", "SHDR"
    };

    int bar_max_w = r.w - 60;
    int row_h     = HUI_FONT_H + 3;

    for (int t = 1; t < HUI_CMD_COUNT; t++) {
        if (y + row_h > r.y + r.h - 4) break;
        if (counts[t] == 0) continue;

        const char *nm = (t < HUI_CMD_COUNT && names[t]) ? names[t] : "?";
        hui_text(x, y, nm, CUM_FG3);

        int label_w = 36;
        int bar_x   = x + label_w;
        int bar_w   = bar_max_w * counts[t] / cmax;
        if (bar_w < 1) bar_w = 1;

        /* color by type group */
        hui_color bc = CUM_BLUE;
        if (t == HUI_CMD_TEXT)                          bc = CUM_GREEN;
        else if (t >= HUI_CMD_RECT && t <= HUI_CMD_TRIANGLE_FILL) bc = CUM_CYAN;
        else if (t >= HUI_CMD_PLOT_LINE)                bc = CUM_PURP;

        hui_rect_fill(hui_rect_make(bar_x, y + 1, bar_w, HUI_FONT_H - 2), bc, 1);
        hui_text(bar_x + bar_w + 3, y, hui_fmt("%d", counts[t]), CUM_FG2);
        y += row_h;
    }

    /* Memory usage */
    if (y + HUI_FONT_H + 6 < r.y + r.h - 4) {
        y += 4;
        hui_line(r.x + 4, y, r.x + r.w - 4, y, CUM_BG3, 1);
        y += 4;
        hui_text(x, y,
            hui_fmt("str: %u/%d  data: %u/%d",
                    dl->strpool_len, HUI_STRPOOL,
                    dl->datapool_len, HUI_DATAPOOL),
            CUM_FG3);
    }

    hui_reset_layer();
}

/* ---- Layout Inspector ---- */

typedef struct {
    hui_rect r;
    int      id;
    char     label[16];
} hui__linsp_entry;

static bool              hui__linsp_on    = false;
static hui__linsp_entry  hui__linsp_rects[256];
static int               hui__linsp_count = 0;

void hui_layout_inspector_toggle(void) {
    hui__linsp_on    = !hui__linsp_on;
    hui__linsp_count = 0;
}

bool hui_layout_inspector_active(void) { return hui__linsp_on; }

void hui_layout_inspector_push(hui_rect r, int id, const char *label) {
    if (!hui__linsp_on || hui__linsp_count >= 256) return;
    hui__linsp_entry *e = &hui__linsp_rects[hui__linsp_count++];
    e->r  = r;
    e->id = id;
    /* copy label, max 15 chars + NUL */
    int i = 0;
    if (label) {
        for (; label[i] && i < 15; i++) e->label[i] = label[i];
    }
    e->label[i] = '\0';
}

void hui_layout_inspector_flush(void) {
    if (!hui__linsp_on) return;
    hui_set_layer(HUI_LAYER_OVERLAY);
    for (int i = 0; i < hui__linsp_count; i++) {
        hui__linsp_entry *e = &hui__linsp_rects[i];
        hui_rect r = e->r;
        /* outline */
        hui_rect_outline(r, HUI_RGBA(CUM_ACCENT.r, CUM_ACCENT.g, CUM_ACCENT.b, 180), 1);
        /* fill if hovered */
        if (hui_is_hovered(r)) {
            hui_rect_fill(r, HUI_RGBA(CUM_ACCENT.r, CUM_ACCENT.g, CUM_ACCENT.b, 40), 0);
        }
        /* label + id near top-left */
        const char *txt = hui_fmt("%.12s#%d", e->label, e->id);
        hui_text(r.x + 2, r.y + 2, txt, HUI_RGBA(CUM_ACCENT.r, CUM_ACCENT.g, CUM_ACCENT.b, 220));
    }
    hui__linsp_count = 0;
    hui_reset_layer();
}

/* ---- Runtime Theme Switcher ---- */

static int hui__theme_idx = 0;

/* Swatch accent colors representing each theme (for picker display only) */
typedef struct { uint8_t r, g, b; const char *name; } hui__theme_desc;
static const hui__theme_desc hui__themes[3] = {
    {  90, 158, 210, "Ristretto" },  /* CUM_BLUE from Ristretto */
    {  70, 130, 200, "Dark"      },  /* CUM_BLUE from Dark */
    { 100, 160, 255, "Light"     },  /* Light — bright blue accent */
};

void hui_theme_cycle(void) {
    hui__theme_idx = (hui__theme_idx + 1) % 3;
}

bool hui_theme_picker(int x, int y, bool *open) {
    if (open && !(*open)) return true;

    hui_set_layer(HUI_LAYER_OVERLAY);

    const int pw = 180, ph = 120;
    hui_rect panel = hui_rect_make(x, y, pw, ph);

    /* Background + border */
    hui_rect_fill(panel, HUI_RGBA(18, 14, 14, 240), 4);
    hui_rect_outline(panel, CUM_BG3, 4);

    /* Title */
    int tx = x + 8, ty = y + 6;
    hui_text(tx, ty, "Theme", CUM_FG);

    /* Close [X] button */
    hui_rect close_r = hui_rect_make(x + pw - 18, y + 4, 14, 14);
    hui_color close_c = hui_is_hovered(close_r) ? CUM_ERR : CUM_FG3;
    hui_text(close_r.x, close_r.y, "X", close_c);
    if (hui_is_clicked(close_r, 0)) {
        if (open) *open = false;
        hui_reset_layer();
        return true;
    }

    /* Swatches */
    int sw_y = y + 26;
    int sw_h = 22;
    int sw_gap = 6;
    for (int i = 0; i < 3; i++) {
        const hui__theme_desc *td = &hui__themes[i];
        hui_rect swatch = hui_rect_make(x + 8, sw_y, 20, sw_h);
        hui_rect row    = hui_rect_make(x + 8, sw_y, pw - 16, sw_h);

        /* Highlight active theme row */
        if (i == hui__theme_idx) {
            hui_rect_fill(row, HUI_RGBA(td->r, td->g, td->b, 40), 3);
            hui_rect_outline(row, HUI_RGBA(td->r, td->g, td->b, 160), 3);
        } else if (hui_is_hovered(row)) {
            hui_rect_fill(row, HUI_RGBA(td->r, td->g, td->b, 20), 3);
        }

        /* Color swatch */
        hui_rect_fill(swatch, HUI_RGBA(td->r, td->g, td->b, 255), 3);

        /* Theme name label */
        hui_text(x + 34, sw_y + (sw_h - HUI_FONT_H) / 2, td->name,
                 (i == hui__theme_idx) ? HUI_RGBA(td->r, td->g, td->b, 255) : CUM_FG2);

        /* Click to select */
        if (hui_is_clicked(row, 0)) {
            hui__theme_idx = i;
        }

        sw_y += sw_h + sw_gap;
    }

    hui_reset_layer();
    return false;
}

/* ---- Demo Window ---- */

void hui_show_demo(hui_rect r, bool *open) {
    if (open && !(*open)) return;

    /* Static demo state */
    static bool  s_chk        = false;
    static bool  s_tog        = false;
    static float s_slid_f     = 0.4f;
    static int   s_slid_i     = 5;
    static float s_drag_f     = 1.0f;
    static char  s_buf[64]    = "Hello hui!";
    static hui_text_state s_txt_st = {0, false};
    static int   s_scroll_y   = 0;
    static bool  s_open_basic = true;
    static bool  s_open_slid  = true;
    static bool  s_open_text  = false;
    static bool  s_open_cont  = false;
    static bool  s_open_over  = false;
#ifdef HUI_PLOT_H
    static bool  s_open_plot  = false;
    /* Pre-computed sin wave */
    static float s_sin_x[32], s_sin_y[32];
    static bool  s_sin_init = false;
    if (!s_sin_init) {
        s_sin_init = true;
        for (int i = 0; i < 32; i++) {
            s_sin_x[i] = (float)i / 31.0f * 6.2832f;
            s_sin_y[i] = 0.5f + 0.45f * sinf(s_sin_x[i]);
        }
    }
#endif

    hui_panel_begin("hui Demo", r, 4);

    /* Optional close button in title area */
    if (open) {
        hui_rect close_r = hui_rect_make(r.x + r.w - 22, r.y + 4, 16, 16);
        if (hui_button("X", close_r)) *open = false;
    }

    /* ---- Section: Basics ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Basics", sec_r, &s_open_basic)) {
            hui_rect br = hui_panel_row(22);
            if (hui_button("Click me", br)) { /* no-op demo */ }

            hui_rect cr = hui_panel_row(20);
            hui_checkbox("Checkbox", cr, &s_chk);

            hui_rect tr = hui_panel_row(20);
            hui_toggle("Toggle", tr, &s_tog);

            hui_rect pr = hui_panel_row(16);
            hui_progress_bar(pr, s_slid_f, CUM_ACCENT, CUM_BG3);

            hui_rect sepr = hui_panel_row(8);
            hui_separator(sepr);

            hui_rect lr = hui_panel_row(HUI_FONT_H + 2);
            hui_label(s_chk ? "Checkbox: ON" : "Checkbox: OFF", lr, HUI_ALIGN_LEFT, CUM_FG2);
        }
    }

    /* ---- Section: Sliders ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Sliders", sec_r, &s_open_slid)) {
            hui_rect sf_r = hui_panel_row(22);
            hui_slider_f("Float", sf_r, &s_slid_f, 0.0f, 1.0f);

            hui_rect si_r = hui_panel_row(22);
            hui_slider_i("Int", si_r, &s_slid_i, 0, 20);

            hui_rect sd_r = hui_panel_row(22);
            hui_drag_f("Drag", sd_r, &s_drag_f, 0.01f);
        }
    }

    /* ---- Section: Text ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Text", sec_r, &s_open_text)) {
            hui_rect ti_r = hui_panel_row(22);
            hui_text_input(ti_r, &s_txt_st, s_buf, sizeof(s_buf));

            hui_rect tw_r = hui_panel_row(HUI_FONT_H * 2 + 4);
            hui_text_wrap(tw_r.x, tw_r.y, tw_r.w,
                          "Wrap: The quick brown fox jumps over the lazy dog.",
                          CUM_FG2);
        }
    }

    /* ---- Section: Containers ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Containers", sec_r, &s_open_cont)) {
            /* Scrollable list */
            hui_rect scroll_r = hui_panel_row(80);
            hui_rect content  = hui_scroll_begin(scroll_r, 22 * 6, &s_scroll_y);
            for (int i = 0; i < 6; i++) {
                hui_rect item = hui_rect_make(content.x, content.y + i * 22,
                                             content.w, 20);
                hui_label(hui_fmt("Item %d", i + 1), item, HUI_ALIGN_LEFT, CUM_FG2);
            }
            hui_scroll_end();
        }
    }

    /* ---- Section: Overlays ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Overlays", sec_r, &s_open_over)) {
            /* Tooltip demo */
            hui_rect tip_r = hui_panel_row(22);
            hui_button("Hover for tooltip", tip_r);
            if (hui_is_hovered(tip_r)) hui_tooltip("This is a tooltip!");

            /* Badge demo */
            hui_rect badge_r = hui_panel_row(22);
            hui_button("Badge demo", badge_r);
            hui_badge(badge_r, "NEW", CUM_OK);
        }
    }

#ifdef HUI_PLOT_H
    /* ---- Section: Plots ---- */
    {
        hui_rect sec_r = hui_panel_row(HUI_FONT_H + 4);
        if (hui_collapsible("Plots", sec_r, &s_open_plot)) {
            hui_rect plot_r = hui_panel_row(100);
            if (hui_plot_begin("sin(x)", plot_r)) {
                hui_plot_setup_limits(0, 6.2832f, 0.0f, 1.0f);
                hui_plot_line("sin", s_sin_x, s_sin_y, 32);
                hui_plot_end();
            }
        }
    }
#endif

    hui_panel_end();
}

/* ---- Frame Dump ---- */

void hui_debug_dump_frame(const char *path) {
    hui_headless_save_ppm(path);
}

int hui_debug_dump_frame_png(const char *path) {
    return hui_headless_save_png(path);
}

/* ---- Recording ---- */

static char  hui__rec_dir[256] = {0};
static int   hui__rec_frame    = 0;
static bool  hui__rec_active   = false;

void hui_debug_record_start(const char *dir) {
    if (!dir) return;
    snprintf(hui__rec_dir, sizeof(hui__rec_dir), "%s", dir);
    hui__rec_frame  = 0;
    hui__rec_active = true;
    /* Create dir (POSIX mkdir, ignore EEXIST) */
#if defined(__unix__) || defined(__APPLE__)
    mkdir(hui__rec_dir, 0755);
#endif
}

void hui_debug_record_stop(void)  { hui__rec_active = false; }
bool hui_debug_recording(void)    { return hui__rec_active;  }

void hui_debug_frame_tick(void) {
    char path[512];
#ifdef HUI_DEBUG_DUMP_DIR
    {
        static int hui__auto_n = 0;
        snprintf(path, sizeof(path), HUI_DEBUG_DUMP_DIR "/frame_%04d.ppm", hui__auto_n++);
        hui_headless_save_ppm(path);
    }
#endif
    if (!hui__rec_active) return;
    snprintf(path, sizeof(path), "%s/frame_%04d.ppm", hui__rec_dir, hui__rec_frame++);
    hui_headless_save_ppm(path);
}

/* ---- Draw List Dump ---- */

void hui_debug_dump_dl(FILE *f) {
    if (!hui_dl_g || !f) return;
    const hui_dl *dl = hui_dl_g;
    fprintf(f, "draw_list: %d cmds\n", dl->count);
    for (int i = 0; i < dl->count; i++) {
        const hui_cmd *c = &dl->cmds[i];
        const char *tname;
        switch (c->type) {
            case HUI_CMD_NOP:           tname = "NOP";        break;
            case HUI_CMD_LINE:          tname = "LINE";       break;
            case HUI_CMD_BEZIER4:       tname = "BEZIER4";    break;
            case HUI_CMD_RECT:          tname = "RECT";       break;
            case HUI_CMD_RECT_FILL:     tname = "RECT_FILL";  break;
            case HUI_CMD_CIRCLE:        tname = "CIRCLE";     break;
            case HUI_CMD_CIRCLE_FILL:   tname = "CIRC_FILL";  break;
            case HUI_CMD_HEMI:          tname = "HEMI";       break;
            case HUI_CMD_TRIANGLE:      tname = "TRI";        break;
            case HUI_CMD_TRIANGLE_FILL: tname = "TRI_FILL";   break;
            case HUI_CMD_ARROW:         tname = "ARROW";      break;
            case HUI_CMD_IMAGE:         tname = "IMAGE";      break;
            case HUI_CMD_TEXT:          tname = "TEXT";       break;
            case HUI_CMD_CLIP_PUSH:     tname = "CLIP_PUSH";  break;
            case HUI_CMD_CLIP_POP:      tname = "CLIP_POP";   break;
            case HUI_CMD_PLOT_LINE:     tname = "PLOT_LINE";  break;
            case HUI_CMD_PLOT_BAR:      tname = "PLOT_BAR";   break;
            case HUI_CMD_PLOT_HEAT:     tname = "PLOT_HEAT";  break;
            case HUI_CMD_SHADER:        tname = "SHADER";     break;
            default:                    tname = "UNK";        break;
        }
        fprintf(f, "[%4d] z=%d %-11s #%02x%02x%02x%02x  (%d,%d)->(%d,%d)",
                i, c->z_layer, tname,
                c->col_r, c->col_g, c->col_b, c->col_a,
                c->x0, c->y0, c->x1, c->y1);
        if (c->type == HUI_CMD_TEXT && dl->strpool)
            fprintf(f, "  \"%s\"", dl->strpool + c->x2);
        fprintf(f, "\n");
    }
}

/* ---- Frame Diff ---- */

void hui_debug_frame_diff(const uint32_t *prev, const uint32_t *cur,
                          int w, int h, uint32_t *out) {
    int n = w * h;
    for (int i = 0; i < n; i++) {
        if (prev[i] != cur[i]) {
            out[i] = 0xFFFF2020u;   /* red — changed pixel */
        } else {
            uint32_t p = cur[i];
            uint8_t r = (uint8_t)(((p >> 16) & 0xFF) >> 1);
            uint8_t g = (uint8_t)(((p >>  8) & 0xFF) >> 1);
            uint8_t b = (uint8_t)(( p        & 0xFF) >> 1);
            out[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

#endif /* HUI_DEBUG_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_DEBUG_H */
