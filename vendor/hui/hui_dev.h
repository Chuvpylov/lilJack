/*
 * hui_dev.h — Developer stats overlay for baremetal HUI apps (PicoDeck/PicoCalc)
 *
 * Compact profiler panel: FPS, per-section timing, draw-list usage, custom lines.
 * No mouse or layer dependencies — works with the strip-renderer backend.
 *
 * Usage (stb-style single-header):
 *
 *   #define HUI_DEV_IMPLEMENTATION   // once, before including
 *   #include "hui_dev.h"
 *
 *   static hui_dev_stats_t g_dev = {0};
 *
 *   // In main loop, Core 0:
 *   uint32_t t0 = time_us_32();
 *   // ... build HUI commands ...
 *   g_dev.build_us = time_us_32() - t0;
 *   g_dev.flush_us = g_flush_us;        // volatile from Core 1
 *   g_dev.fps      = g_fps;
 *   hui_dev_fill_dl(&g_dev, dl);        // snapshot draw-list usage
 *   HUI_DEV_EXTRA(&g_dev, "audio: %dHz", rate);
 *
 *   // Somewhere in draw_frame():
 *   if (g_dev_visible)
 *       hui_dev_panel(SCR_W-202, 20, 200, &g_dev);
 *
 *   // Reset custom lines each frame:
 *   hui_dev_reset_lines(&g_dev);
 */

#ifndef HUI_DEV_H
#define HUI_DEV_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifndef HUI_DEV_MAX_LINES
#  define HUI_DEV_MAX_LINES 6
#endif
#ifndef HUI_DEV_LINE_LEN
#  define HUI_DEV_LINE_LEN  36
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Stats struct ---- */

typedef struct {
    uint32_t fps;
    uint32_t build_us;          /* Core 0: HUI command-list build time */
    uint32_t flush_us;          /* Core 1: strip-renderer flush time   */
    uint16_t dl_cmds;           /* commands in last frame              */
    uint16_t dl_cmds_max;       /* HUI_MAX_CMDS                        */
    uint16_t str_used;          /* strpool bytes used                  */
    uint16_t str_max;           /* HUI_STRPOOL                         */
    /* Custom status lines */
    char     lines[HUI_DEV_MAX_LINES][HUI_DEV_LINE_LEN];
    int      n_lines;
} hui_dev_stats_t;

/* ---- API ---- */

/* Snapshot draw-list usage into stats (call after building the frame). */
void hui_dev_fill_dl(hui_dev_stats_t *s, const hui_dl *dl);

/* Reset custom lines — call at the start of each frame. */
void hui_dev_reset_lines(hui_dev_stats_t *s);

/* Draw the developer panel at (x,y) with given width.
 * Returns height used so caller can stack other elements. */
int hui_dev_panel(int x, int y, int w, const hui_dev_stats_t *s);

/* Add a printf-style status line (up to HUI_DEV_MAX_LINES). */
#define HUI_DEV_EXTRA(s, fmt, ...) \
    do { if ((s)->n_lines < HUI_DEV_MAX_LINES) { \
        snprintf((s)->lines[(s)->n_lines++], HUI_DEV_LINE_LEN, fmt, ##__VA_ARGS__); \
    } } while(0)

/* ---- Implementation ---- */

#ifdef HUI_DEV_IMPLEMENTATION

void hui_dev_fill_dl(hui_dev_stats_t *s, const hui_dl *dl) {
    if (!dl) return;
    s->dl_cmds     = dl->count;
    s->dl_cmds_max = HUI_MAX_CMDS;
    s->str_used    = dl->strpool_len;
    s->str_max     = HUI_STRPOOL;
}

void hui_dev_reset_lines(hui_dev_stats_t *s) {
    s->n_lines = 0;
}

/* Helper: draw a small usage bar [x,y,w,6] */
static void hui__dev_bar(int x, int y, int w, float frac,
                          hui_color fill, hui_color track)
{
    int fw = (int)((float)w * frac);
    if (fw < 0) fw = 0;
    if (fw > w) fw = w;
    hui_rect_fill(hui_rect_make(x, y, w, 5), track, 0);
    if (fw > 0) hui_rect_fill(hui_rect_make(x, y, fw, 5), fill, 0);
}

int hui_dev_panel(int x, int y, int w, const hui_dev_stats_t *s)
{
    /* Row height: font is 12px rendered, use 13px leading */
    const int RH = 13;
    const int PAD = 5;

    /* Estimate height */
    int n_rows = 6 + s->n_lines; /* title + fps + flush + cmds + str + sep + custom */
    int h = PAD + n_rows * RH + PAD + 4;

    /* Background + border */
    hui_rect_fill(hui_rect_make(x, y, w, h), CUM_BG0, 0);
    hui_rect_outline(hui_rect_make(x, y, w, h), CUM_BG3, 0);

    int tx = x + PAD;
    int ty = y + PAD;

    /* Title */
    hui_text(tx, ty, "DEV PANEL", CUM_FG3);
    ty += RH;
    hui_line(x+2, ty, x+w-2, ty, CUM_BG3, 1);
    ty += 3;

    /* FPS + build time */
    hui_text(tx, ty,
        hui_fmt("FPS:%lu  bld:%lums",
                (unsigned long)s->fps,
                (unsigned long)(s->build_us / 1000)),
        CUM_FG);
    ty += RH;

    /* Flush time */
    hui_text(tx, ty,
        hui_fmt("flush:%lums  (%lu strips)",
                (unsigned long)(s->flush_us / 1000),
                (unsigned long)(s->flush_us > 0 ? 320 / 8 : 0)),
        s->flush_us > 40000 ? CUM_WARN : CUM_FG2);
    ty += RH;

    /* Draw-list usage bar */
    float dl_frac = s->dl_cmds_max > 0 ?
                    (float)s->dl_cmds / (float)s->dl_cmds_max : 0.f;
    hui_text(tx, ty,
        hui_fmt("cmds:%u/%u", s->dl_cmds, s->dl_cmds_max),
        dl_frac > 0.8f ? CUM_ERR : CUM_FG2);
    ty += RH - 5;
    hui__dev_bar(tx, ty, w - PAD*2, dl_frac,
                 dl_frac > 0.8f ? CUM_ERR : CUM_ACCENT, CUM_BG2);
    ty += 8;

    /* Strpool usage bar */
    float str_frac = s->str_max > 0 ?
                     (float)s->str_used / (float)s->str_max : 0.f;
    hui_text(tx, ty,
        hui_fmt("str:%u/%u", s->str_used, s->str_max),
        str_frac > 0.8f ? CUM_ERR : CUM_FG2);
    ty += RH - 5;
    hui__dev_bar(tx, ty, w - PAD*2, str_frac,
                 str_frac > 0.8f ? CUM_ERR : CUM_CYAN, CUM_BG2);
    ty += 9;

    /* Separator + custom lines */
    if (s->n_lines > 0) {
        hui_line(x+2, ty, x+w-2, ty, CUM_BG3, 1);
        ty += 3;
        for (int i = 0; i < s->n_lines; i++) {
            hui_text(tx, ty, s->lines[i], CUM_FG3);
            ty += RH;
        }
    }

    return ty - y + PAD;   /* actual height used */
}

#endif /* HUI_DEV_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif
#endif /* HUI_DEV_H */
