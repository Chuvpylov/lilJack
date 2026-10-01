/*
 * backends/hui_svg.h — SVG backend for hui  [WORKING]
 *
 * Translates hui_dl draw commands to an SVG string. No pixel buffer, no rasterizer.
 * Useful for: server-side chart/plot export, file-based vector screenshots, CI tests.
 *
 * Select with: #define HUI_BACKEND_SVG before #include "hui.h"
 *
 * Usage:
 *   hui_init(800, 600);
 *   hui_begin_frame();
 *   // ... draw widgets/plots ...
 *   hui_end_frame();              // calls hui_backend_flush → builds SVG in memory
 *   hui_svg_save("out.svg");      // write to file
 *   hui_shutdown();
 *
 * The SVG buffer is reset each frame. Get the current frame's SVG before the next
 * hui_end_frame() call or after it (same frame pointer is valid until next flush).
 */

#ifndef HUI_SVG_H
#define HUI_SVG_H

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include "../hui_draw.h"
#include "../hui_font.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HUI_SVG_BUF_SIZE
#  define HUI_SVG_BUF_SIZE (256 * 1024)   /* 256 KB — enough for a dense plot frame */
#endif

static char hui__svg_buf[HUI_SVG_BUF_SIZE];
static int  hui__svg_len;

/* Append formatted text to the SVG buffer (silently truncates on overflow). */
static void hui__svg_append(const char *fmt, ...) {
    int rem = HUI_SVG_BUF_SIZE - hui__svg_len - 1;
    if (rem <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(hui__svg_buf + hui__svg_len, (size_t)rem, fmt, ap);
    va_end(ap);
    if (n > 0) hui__svg_len += (n < rem) ? n : rem;
}

static void hui__svg_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                            int filled) {
    float opacity = (float)a / 255.0f;
    if (filled)
        hui__svg_append(" fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\"",
                        r, g, b, opacity);
    else
        hui__svg_append(" fill=\"none\" stroke=\"rgb(%u,%u,%u)\" stroke-opacity=\"%.3f\"",
                        r, g, b, opacity);
}

/* ---- Public API ---- */

/* Return pointer to current SVG string (null-terminated). Valid until next flush. */
static inline const char *hui_svg_get_string(void) { return hui__svg_buf; }

/* Write current SVG to file. Returns 0 on success, -1 on error. */
HUI_MAYBE_UNUSED static int hui_svg_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(hui__svg_buf, f);
    fclose(f);
    return 0;
}

/* ---- Backend lifecycle stubs (SVG needs no window/framebuffer) ---- */

HUI_MAYBE_UNUSED static void hui_headless_init(int w, int h)    { (void)w; (void)h; }
HUI_MAYBE_UNUSED static void hui_headless_free(void)            {}
HUI_MAYBE_UNUSED static void hui_headless_resize(int w, int h)  { (void)w; (void)h; }

/* ---- hui_backend_flush — the core translator ---- */

#ifdef HUI_IMPLEMENTATION   /* multi-TU: defined ONCE (hui/tools/multi_tu_test, 2026-09-12) */
void hui_backend_flush(const hui_cmd *cmds, uint16_t count,
                       const char *strpool, const float *datapool) {
    (void)datapool;
    int W = hui_g ? hui_g->screen_w : 800;
    int H = hui_g ? hui_g->screen_h : 600;

    hui__svg_len = 0;
    hui__svg_append(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\""
        " width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n",
        W, H, W, H);

    /* Pre-pass: emit <defs> with <clipPath> elements.
     * MUST iterate in the same layer order as the render loop below so that
     * clip IDs assigned here match the clip_next counter used during rendering. */
    {
        static const uint8_t clip_layer_order[] = {
            HUI_LAYER_BG, HUI_LAYER_NORMAL, HUI_LAYER_POPUP, HUI_LAYER_OVERLAY
        };
        int  clip_id  = 0;
        bool has_defs = false;
        for (int li = 0; li < 4; li++) {
            uint8_t lyr = clip_layer_order[li];
            for (uint16_t ci = 0; ci < count; ci++) {
                if (cmds[ci].z_layer != lyr) continue;
                if (cmds[ci].type != HUI_CMD_CLIP_PUSH) continue;
                if (!has_defs) { hui__svg_append("<defs>\n"); has_defs = true; }
                hui__svg_append(
                    "  <clipPath id=\"hclip%d\">"
                    "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"/>"
                    "</clipPath>\n",
                    clip_id++,
                    cmds[ci].x0, cmds[ci].y0, cmds[ci].x1, cmds[ci].y1);
            }
        }
        if (has_defs) hui__svg_append("</defs>\n");
    }

    hui__svg_append("<rect width=\"%d\" height=\"%d\" fill=\"#1a1a1a\"/>\n", W, H);

    /* Group commands by layer — render BG first, then NORMAL, POPUP, OVERLAY */
    int clip_next = 0;  /* matches pre-pass clip IDs */
    static const uint8_t layer_order[] = {
        HUI_LAYER_BG, HUI_LAYER_NORMAL, HUI_LAYER_POPUP, HUI_LAYER_OVERLAY
    };
    for (int li = 0; li < 4; li++) {
        uint8_t layer = layer_order[li];
        bool any = false;
        for (uint16_t ci = 0; ci < count; ci++) {
            if (cmds[ci].z_layer != layer) continue;
            if (!any) {
                hui__svg_append("<g>\n");
                any = true;
            }
            const hui_cmd *c = &cmds[ci];
            uint8_t r = c->col_r, g = c->col_g, b = c->col_b, a = c->col_a;
            switch (c->type) {

            case HUI_CMD_RECT_FILL:
                hui__svg_append("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"",
                                c->x0, c->y0, c->x1, c->y1);
                if (c->rounding > 0)
                    hui__svg_append(" rx=\"%d\"", c->rounding);
                hui__svg_color(r, g, b, a, 1);
                hui__svg_append("/>\n");
                break;

            case HUI_CMD_RECT:
                hui__svg_append("<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\"",
                                c->x0, c->y0, c->x1, c->y1);
                if (c->rounding > 0)
                    hui__svg_append(" rx=\"%d\"", c->rounding);
                {
                    float op = (float)a / 255.0f;
                    int thick = c->thick > 0 ? c->thick : 1;
                    hui__svg_append(" fill=\"none\" stroke=\"rgb(%u,%u,%u)\""
                                   " stroke-opacity=\"%.3f\" stroke-width=\"%d\"",
                                   r, g, b, op, thick);
                }
                hui__svg_append("/>\n");
                break;

            case HUI_CMD_LINE: {
                float op = (float)a / 255.0f;
                int thick = c->thick > 0 ? c->thick : 1;
                hui__svg_append(
                    "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\""
                    " stroke=\"rgb(%u,%u,%u)\" stroke-opacity=\"%.3f\""
                    " stroke-width=\"%d\"/>\n",
                    c->x0, c->y0, c->x1, c->y1, r, g, b, op, thick);
                break;
            }

            case HUI_CMD_CIRCLE:
                hui__svg_append("<circle cx=\"%d\" cy=\"%d\" r=\"%d\"",
                                c->x0, c->y0, c->x1);
                {
                    float op = (float)a / 255.0f;
                    hui__svg_append(" fill=\"none\" stroke=\"rgb(%u,%u,%u)\""
                                   " stroke-opacity=\"%.3f\"", r, g, b, op);
                }
                hui__svg_append("/>\n");
                break;

            case HUI_CMD_CIRCLE_FILL:
                hui__svg_append("<circle cx=\"%d\" cy=\"%d\" r=\"%d\"",
                                c->x0, c->y0, c->x1);
                hui__svg_color(r, g, b, a, 1);
                hui__svg_append("/>\n");
                break;

            case HUI_CMD_TRIANGLE:
            case HUI_CMD_TRIANGLE_FILL: {
                int filled = (c->type == HUI_CMD_TRIANGLE_FILL);
                hui__svg_append("<polygon points=\"%d,%d %d,%d %d,%d\"",
                                c->x0, c->y0, c->x1, c->y1, c->x2, c->y2);
                if (filled) {
                    hui__svg_color(r, g, b, a, 1);
                } else {
                    float op = (float)a / 255.0f;
                    hui__svg_append(" fill=\"none\" stroke=\"rgb(%u,%u,%u)\""
                                   " stroke-opacity=\"%.3f\"", r, g, b, op);
                }
                hui__svg_append("/>\n");
                break;
            }

            case HUI_CMD_BEZIER4: {
                float op = (float)a / 255.0f;
                int thick = c->thick > 0 ? c->thick : 1;
                hui__svg_append(
                    "<path d=\"M %d %d C %d %d %d %d %d %d\""
                    " fill=\"none\" stroke=\"rgb(%u,%u,%u)\""
                    " stroke-opacity=\"%.3f\" stroke-width=\"%d\"/>\n",
                    c->x0, c->y0, c->x1, c->y1, c->x2, c->y2, c->x3, c->y3,
                    r, g, b, op, thick);
                break;
            }

            case HUI_CMD_TEXT:
                if (strpool && c->x2 >= 0) {
                    const char *s = strpool + c->x2;
                    float op = (float)a / 255.0f;
                    /* SVG text is baseline-anchored; shift down by font height */
                    int ty = c->y0 + HUI_FONT_H;
                    hui__svg_append(
                        "<text x=\"%d\" y=\"%d\""
                        " font-family=\"monospace\" font-size=\"%d\""
                        " fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\">",
                        c->x0, ty, HUI_FONT_H, r, g, b, op);
                    /* Escape XML special chars */
                    for (const char *p = s; *p; p++) {
                        switch (*p) {
                        case '<':  hui__svg_append("&lt;");  break;
                        case '>':  hui__svg_append("&gt;");  break;
                        case '&':  hui__svg_append("&amp;"); break;
                        case '"':  hui__svg_append("&quot;");break;
                        default:   hui__svg_append("%c", *p);break;
                        }
                    }
                    hui__svg_append("</text>\n");
                }
                break;

            case HUI_CMD_ARROW: {
                /* Arrow: line + small filled triangle head */
                float op = (float)a / 255.0f;
                hui__svg_append(
                    "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\""
                    " stroke=\"rgb(%u,%u,%u)\" stroke-opacity=\"%.3f\""
                    " stroke-width=\"1\"/>\n",
                    c->x0, c->y0, c->x1, c->y1, r, g, b, op);
                /* Arrowhead approx: tiny circle at endpoint */
                hui__svg_append(
                    "<circle cx=\"%d\" cy=\"%d\" r=\"3\""
                    " fill=\"rgb(%u,%u,%u)\" fill-opacity=\"%.3f\"/>\n",
                    c->x1, c->y1, r, g, b, op);
                break;
            }

            case HUI_CMD_CLIP_PUSH:
                hui__svg_append("<g clip-path=\"url(#hclip%d)\">\n", clip_next++);
                break;

            case HUI_CMD_CLIP_POP:
                hui__svg_append("</g>\n");
                break;

            case HUI_CMD_HEMI: {
                /* Arc from a0 to a1 degrees around (cx, cy) with radius r */
                float op   = (float)a / 255.0f;
                float cx   = (float)c->x0, cy = (float)c->y0;
                float rad  = (float)c->x1;
                float a0r  = (float)c->x2 * (3.14159265f / 180.0f);
                float a1r  = (float)c->x3 * (3.14159265f / 180.0f);
                float sx   = cx + rad * cosf(a0r), sy = cy + rad * sinf(a0r);
                float ex   = cx + rad * cosf(a1r), ey = cy + rad * sinf(a1r);
                int da     = c->x3 - c->x2;
                int large  = (da < 0 ? -da : da) > 180 ? 1 : 0;
                hui__svg_append(
                    "<path d=\"M %.2f %.2f A %.2f %.2f 0 %d 1 %.2f %.2f\""
                    " fill=\"none\" stroke=\"rgb(%u,%u,%u)\""
                    " stroke-opacity=\"%.3f\"/>\n",
                    sx, sy, rad, rad, large, ex, ey, r, g, b, op);
                break;
            }

            /* Image, plot cmds, shader — silently skip in SVG */
            case HUI_CMD_IMAGE:
            case HUI_CMD_PLOT_LINE:
            case HUI_CMD_PLOT_BAR:
            case HUI_CMD_PLOT_HEAT:
            case HUI_CMD_SHADER:
            case HUI_CMD_NOP:
            default:
                break;
            }
        }
        if (any) hui__svg_append("</g>\n");
    }

    hui__svg_append("</svg>\n");
}
#endif /* HUI_IMPLEMENTATION — one definition per program; the declaration in hui.h stays visible to every TU */

#ifdef __cplusplus
}
#endif

#endif /* HUI_SVG_H */
