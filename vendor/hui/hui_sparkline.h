#ifndef HUI_SPARKLINE_H
#define HUI_SPARKLINE_H
#include <math.h>
#include <stdint.h>
/* hui_sparkline.h — a value history as a strip of bars, with THE RULE lilJack
 * learned the hard way (2026-09-12, header GPU/CPU/MEM graphs):
 *
 *   NAN DRAWS NOTHING. A sample that is unavailable is a GAP, not a zero — a
 *   flat bar at 0% reads as an idle machine, which is a lie. And when EVERY
 *   sample is NAN the picture is EMPTY: no frame, no baseline, no bytes — a
 *   plot of a missing source is not a picture, it is a placeholder pretending.
 *
 * Layout is PURE (no drawing, no allocation) so it is testable headless and so
 * cell/sixel presenters can lay out in one unit and paint in another; the
 * drawing wrapper is a thin loop over the same rects. Opt-in, additive:
 * nothing in hui_plot.h changes. hui_plot's line series already treat NAN as a
 * gap sentinel (hui_plot.h hui__draw_line); this header adds the all-NAN
 * "draw nothing" half, which a series inside a framed plot cannot express. */

/* Caller-owned display aggregation. Push each acquired sample exactly once.
 * Only completed time buckets are published; a finite mean never treats NAN
 * as zero. Return elapsed bucket count (0 means hold the previous display).
 * If >1, the first closed bucket is followed by count-1 unavailable gaps.
 * Length changes and backwards clocks reset the partial bucket. No allocation,
 * clock access, or drawing; the caller owns history and chooses its capacity. */
typedef struct {
    uint64_t bucket, length_ms;
    double sum;
    unsigned count;
    int started;
} hui_sparkline_bucket;
static inline uint64_t hui_sparkline_bucket_push(hui_sparkline_bucket *s,
        uint64_t now_ms, uint64_t length_ms, float value, float *closed){
    if(!s||!closed||!length_ms)return 0;
    uint64_t bucket=now_ms/length_ms,elapsed=0;
    if(!s->started||s->length_ms!=length_ms||bucket<s->bucket){
        *s=(hui_sparkline_bucket){.bucket=bucket,.length_ms=length_ms,.started=1};
    }else if(bucket>s->bucket){
        elapsed=bucket-s->bucket;
        *closed=s->count?(float)(s->sum/s->count):NAN;
        s->bucket=bucket;s->sum=0;s->count=0;
    }
    if(isfinite(value)){s->sum+=value;s->count++;}
    return elapsed;
}

typedef struct { int x, y, w, h; } hui_spark_bar;

/* Lay out one bar per FINITE sample of ys[0..n) inside the box (x,y,w,h).
 *   xs    — sample positions, or NULL for evenly spaced (index).
 *   xmin/xmax — the x range mapped onto [x, x+w-bar_w]; xmax<=xmin = auto from
 *           the finite xs (or 0..n-1 when xs is NULL).
 *   vmin/vmax — the value range mapped onto bar height [0, h-2]; vmax<=vmin =
 *           auto from the finite ys. Values are clamped into the range.
 *   bar_w — bar width in pixels (>=1).
 * Returns the number of bars written to out (<= cap). Returns 0, and writes
 * NOTHING to out, when n<1, the box cannot hold a bar, or NO sample is finite —
 * that 0 is the caller's signal to draw nothing at all. */
static inline int hui_sparkline_layout(int x, int y, int w, int h,
                                       const float *xs, const float *ys, int n,
                                       float xmin, float xmax,
                                       float vmin, float vmax,
                                       int bar_w, hui_spark_bar *out, int cap) {
    if (!ys || !out || n < 1 || cap < 1 || bar_w < 1 || w < bar_w || h < 3) return 0;
    int finite = 0;
    float axmin = 0, axmax = 0, avmin = 0, avmax = 0;
    for (int i = 0; i < n; i++) {
        float v = ys[i], px = xs ? xs[i] : (float)i;
        if (isnan(v) || isnan(px)) continue;
        if (!finite) { axmin = axmax = px; avmin = avmax = v; }
        else {
            if (px < axmin) axmin = px;
            if (px > axmax) axmax = px;
            if (v < avmin) avmin = v;
            if (v > avmax) avmax = v;
        }
        finite++;
    }
    if (!finite) return 0;                        /* all NAN: nothing, honestly */
    if (xmax <= xmin) { xmin = axmin; xmax = axmax; }
    if (vmax <= vmin) { vmin = avmin; vmax = avmax; }
    float xspan = xmax - xmin, vspan = vmax - vmin;
    int usable_w = w - bar_w, usable_h = h - 2, count = 0;
    for (int i = 0; i < n && count < cap; i++) {
        float v = ys[i], px = xs ? xs[i] : (float)i;
        if (isnan(v) || isnan(px)) continue;      /* a gap, not a zero */
        if (v < vmin) v = vmin;
        if (v > vmax) v = vmax;
        if (px < xmin) px = xmin;
        if (px > xmax) px = xmax;
        int bx = x + (xspan > 0 ? (int)((px - xmin) * (float)usable_w / xspan) : 0);
        int bh = vspan > 0 ? (int)((v - vmin) * (float)usable_h / vspan) : usable_h;
        out[count++] = (hui_spark_bar){ bx, y + h - 1 - bh, bar_w, bh };
    }
    return count;
}

#ifdef HUI_H
/* Draw the bars in colour c. Returns what the layout returned: 0 means nothing
 * was drawn and the caller should show its "—" (or nothing), never a frame.
 * Bounded scratch: at most 256 bars per call; longer histories are decimated
 * by the caller (a sparkline wider than 256 bars is a plot, use hui_plot). */
static inline int hui_sparkline_draw(hui_rect r, const float *xs, const float *ys, int n,
                                     float xmin, float xmax, float vmin, float vmax,
                                     int bar_w, hui_color c) {
    hui_spark_bar bars[256];
    int k = hui_sparkline_layout(r.x, r.y, r.w, r.h, xs, ys, n, xmin, xmax, vmin, vmax,
                                 bar_w, bars, 256);
    for (int i = 0; i < k; i++)
        if (bars[i].h > 0)
            hui_rect_fill(hui_rect_make((int16_t)bars[i].x, (int16_t)bars[i].y,
                                        (int16_t)bars[i].w, (int16_t)bars[i].h), c, 0);
    return k;
}
#endif
#endif /* HUI_SPARKLINE_H */
