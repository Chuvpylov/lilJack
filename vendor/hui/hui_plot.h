/*
 * hui_plot.h — Plot system for hui (P2)
 *
 * stb-style single header.
 * Define HUI_PLOT_IMPLEMENTATION in exactly one translation unit.
 * Requires hui.h (with HUI_IMPLEMENTATION) to be included first.
 *
 * Usage:
 *   if (hui_plot_begin("Sensor", hui_rect_make(x, y, 400, 260))) {
 *       hui_plot_setup_axes("Time (s)", "Voltage");
 *       hui_plot_setup_limits(0, 10, -1.5, 1.5);
 *       hui_plot_line("CH1", xs, ys, N);
 *       hui_plot_end();
 *   }
 *
 * Auto-range (omit hui_plot_setup_limits):
 *   if (hui_plot_begin("Data", r)) {
 *       hui_plot_scatter("Points", xs, ys, N);
 *       hui_plot_end();
 *   }
 *
 * Implicit x (index 0,1,2,...):
 *   hui_plot_line_vals("Signal", ys, N);
 *
 * Realtime ring buffer:
 *   static hui_plot_ringbuf rb;
 *   static float buf[256];
 *   hui_plot_ringbuf_init(&rb, buf, 256);  // once
 *   hui_plot_ringbuf_push(&rb, sample);    // each tick
 *   // per frame:
 *   if (hui_plot_begin("Live", r)) {
 *       hui_plot_line_ring("Signal", &rb);
 *       hui_plot_end();
 *   }
 */

#ifndef HUI_PLOT_H
#define HUI_PLOT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifndef HUI_PLOT_MAX_SERIES
#  define HUI_PLOT_MAX_SERIES 12
#endif

/* Scratch buffer for ring-buffer → line rendering. */
#ifndef HUI_PLOT_RING_SCRATCH
#  define HUI_PLOT_RING_SCRATCH 4096
#endif

/* Maximum custom ticks per axis (hui_plot_setup_axis_ticks). */
#ifndef HUI_PLOT_MAX_CUSTOM_TICKS
#  define HUI_PLOT_MAX_CUSTOM_TICKS 32
#endif

/* Max bins per axis for hui_plot_histogram2d (default 64 → 64×64 = 4096 cells). */
#ifndef HUI_PLOT_HIST2D_MAX_BINS
#  define HUI_PLOT_HIST2D_MAX_BINS 64
#endif

/* Gap between subplot cells in pixels. */
#ifndef HUI_SUBPLOTS_GAP
#  define HUI_SUBPLOTS_GAP 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Ring buffer (y-only) ---- */

/** @brief Circular buffer for streaming scalar samples (Y only). */
typedef struct {
    float *data;    /**< @brief Caller-owned storage array of `cap` floats. */
    int    cap;     /**< @brief Total buffer capacity in samples. */
    int    head;    /**< @brief Index where the NEXT push will be written. */
    int    count;   /**< @brief Number of valid elements currently stored [0, cap]. */
} hui_plot_ringbuf;

/* ---- Ring buffer (XY pairs) ---- */

/** @brief Circular buffer for streaming XY point pairs. */
typedef struct {
    float *data;    /**< @brief Caller-owned storage of `cap * 2` floats, interleaved as [x0,y0, x1,y1, ...]. */
    int    cap;     /**< @brief Buffer capacity in XY pairs. */
    int    head;    /**< @brief Pair index where the NEXT push will be written. */
    int    count;   /**< @brief Number of valid pairs currently stored [0, cap]. */
} hui_plot_ringbuf_xy;

void  hui_plot_ringbuf_init(hui_plot_ringbuf *rb, float *buf, int cap);
void  hui_plot_ringbuf_push(hui_plot_ringbuf *rb, float v);
float hui_plot_ringbuf_get(const hui_plot_ringbuf *rb, int i); /* 0=oldest */

void hui_plot_ringbuf_xy_init(hui_plot_ringbuf_xy *rb, float *buf, int cap);
void hui_plot_ringbuf_xy_push(hui_plot_ringbuf_xy *rb, float x, float y);

/* ---- Pan/zoom state (user-owned, must persist across frames) ---- */

typedef struct {
    double xlim[2], ylim[2]; /* current view extents */
    bool   initialized;      /* false on first frame → auto-range captured */
    bool   _dragging;        /* internal: LMB pan drag in progress */
    bool   _box_sel;         /* internal: RMB range-select drag in progress */
    double _box_sx, _box_sy; /* internal: data coords where box-select started */
    /* RMB range selection — readable by caller after hui_plot_end() */
    bool   range_active;     /* true while RMB drag is in progress */
    bool   range_confirmed;  /* true for ONE frame when RMB released after drag */
    bool   rmb_clicked;      /* true for ONE frame when RMB released without drag */
    double range_x0, range_x1; /* selected x range in data coords (range_active or range_confirmed) */
} hui_plot_state;

/* Reset to auto-range (takes effect next frame). */
void hui_plot_state_reset(hui_plot_state *state);

/* ---- Begin / End ---- */

/** @brief Start a new plot frame. @param title text drawn in the title bar (NULL = no title bar). @param r screen rect for the entire plot including axes. @return false if r is too small to render — skip series calls and hui_plot_end(). */
bool hui_plot_begin(const char *title, hui_rect r);
/** @brief Finalise and rasterize all series queued since hui_plot_begin(). */
void hui_plot_end(void);

/** @brief Like hui_plot_begin() with interactive pan (LMB drag) and zoom (scroll wheel). @param title plot title. @param r screen rect. @param state persistent view state; call hui_plot_state_reset() to jump back to auto-range. @return false if r is too small. */
bool hui_plot_begin_ex(const char *title, hui_rect r, hui_plot_state *state);

/* ---- Setup (call between begin/end, before series calls) ---- */

/** @brief Set human-readable axis labels rendered below/left of the plot area. @param xlabel label for the horizontal axis (NULL = none). @param ylabel label for the vertical axis (NULL = none). */
void hui_plot_setup_axes(const char *xlabel, const char *ylabel);

/** @brief Pin the view to explicit axis limits; overrides auto-range for this frame. @param xmin left data boundary. @param xmax right data boundary. @param ymin bottom data boundary. @param ymax top data boundary. */
void hui_plot_setup_limits(double xmin, double xmax, double ymin, double ymax);

/* ---- Series ---- */

/** @brief Connected line series. @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param n number of points. */
void hui_plot_line(const char *label, const float *xs, const float *ys, int n);
/** @brief Line series with implicit x = 0, 1, 2, …. @param label legend entry. @param ys y-coordinate array. @param n number of samples. */
void hui_plot_line_vals(const char *label, const float *ys, int n);
/** @brief Scatter plot (discrete markers, no connecting line). @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param n number of points. */
void hui_plot_scatter(const char *label, const float *xs, const float *ys, int n);
/** @brief Vertical bar chart. @param label legend entry. @param xs bar centre positions. @param ys bar heights. @param n number of bars. @param bar_w bar width in data units. */
void hui_plot_bars(const char *label, const float *xs, const float *ys, int n, float bar_w);

/** @brief Frequency histogram of scalar values. @param label legend entry. @param vals input sample array. @param n number of samples. @param bins bin count (0 = auto sqrt(n)). @param rmin range minimum (rmin >= rmax = auto-range). @param rmax range maximum. */
void hui_plot_histogram(const char *label, const float *vals, int n,
                        int bins, float rmin, float rmax);

/* Line from ring buffer (oldest→newest, x=index). */
void hui_plot_line_ring(const char *label, hui_plot_ringbuf *rb);

/* Line from ring buffer of XY pairs (oldest→newest). */
void hui_plot_line_ring_xy(const char *label, hui_plot_ringbuf_xy *rb);

/* Plot line from struct array via stride.
 * xs/ys point to the first float in your struct; stride_bytes is sizeof(YourStruct).
 * Example: hui_plot_line_stride("v", &pts[0].t, &pts[0].v, N, sizeof(Pt)); */
void hui_plot_line_stride(const char *label,
                          const float *xs, const float *ys,
                          int n, int stride_bytes);

/** @brief Step / staircase plot — horizontal segment then vertical jump at each point. @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param n number of points. */
void hui_plot_stairs(const char *label, const float *xs, const float *ys, int n);

/** @brief Filled band between two Y series sharing the same X values. @param label legend entry. @param xs shared x-coordinate array. @param ys1 lower Y boundary. @param ys2 upper Y boundary. @param n number of points. */
void hui_plot_shaded(const char *label,
                     const float *xs, const float *ys1, const float *ys2, int n);

/** @brief Logic-analyser style binary lane — ys[i] treated as 0 or 1 and rendered as high/low steps. @param label lane name shown in legend. @param xs x-coordinate array (timestamps). @param ys signal array; non-zero = high. @param n number of points. */
void hui_plot_digital(const char *label, const float *xs, const float *ys, int n);

/** @brief Horizontal bar chart (category axis on Y). @param label legend entry. @param xs bar lengths (x-axis values). @param ys category centre positions on the y-axis. @param n number of bars. @param bar_h bar height in data units (0 = auto 0.8 of category spacing). */
void hui_plot_bars_h(const char *label, const float *xs, const float *ys, int n, float bar_h);

/** @brief Stem plot — vertical lines from the zero baseline to each point with a dot marker at the tip. @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param n number of points. */
void hui_plot_stems(const char *label, const float *xs, const float *ys, int n);

/** @brief Symmetric vertical error bars centred on each (x,y) point. @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param err half-width of each bar in data units (same scale as y). @param n number of points. */
void hui_plot_error_bars(const char *label,
                         const float *xs, const float *ys, const float *err, int n);

/* Horizontal error bars — lines from x-err to x+err with end caps. */
void hui_plot_error_bars_h(const char *label,
                            const float *xs, const float *ys, const float *err, int n);

/** @brief Bubble chart — scatter with variable-radius filled circles. @param label legend entry. @param xs x-coordinate array. @param ys y-coordinate array. @param sz per-point radius in data units. @param n number of points. @param sz_scale global multiplier applied to all sz values. */
void hui_plot_bubbles(const char *label,
                      const float *xs, const float *ys, const float *sz, int n,
                      float sz_scale);

/** @brief OHLC candlestick chart. @param label legend entry. @param dates x-axis positions (timestamps or index). @param opens open prices. @param closes close prices (close >= open = bullish/green, else bearish/red). @param lows low wicks. @param highs high wicks. @param n number of candles. */
void hui_plot_candlestick(const char *label,
                          const float *dates, const float *opens,
                          const float *closes, const float *lows,
                          const float *highs, int n);

/* Zero-copy line via getter: getter(idx, user_data, *x, *y). */
typedef void (*hui_plot_getter_fn)(int idx, void *user_data, float *x, float *y);
void hui_plot_line_fn(const char *label, hui_plot_getter_fn getter, void *user_data, int n);

/* ---- Annotations / tags (call between begin/end) ---- */

/** @brief Floating text label pinned to a data point. @param x data x coordinate. @param y data y coordinate. @param text annotation string. @param c text and dot color. */
void hui_plot_annotation(double x, double y, const char *text, hui_color c);

/** @brief Colored vertical marker line at a specific X value with a label on the x-axis. @param x data x position. @param text label string. @param c line and label color. */
void hui_plot_tag_x(double x, const char *text, hui_color c);

/** @brief Colored horizontal marker line at a specific Y value with a label on the y-axis. @param y data y position. @param text label string. @param c line and label color. */
void hui_plot_tag_y(double y, const char *text, hui_color c);

/* ---- Drag tools (call between begin/end; id must be unique per tool per frame) ---- */

/** @brief Draggable point handle rendered at data coordinates. @param id unique non-zero integer to distinguish concurrent handles. @param x data x position updated while dragging. @param y data y position updated while dragging. @param c handle color. @param radius hit-test radius in pixels (<=0 = default 5 px). @return true on every frame the point is being dragged. */
bool hui_plot_drag_point(int id, double *x, double *y, hui_color c, int radius);

/* Draggable vertical line at data x.   Returns true while dragging. */
bool hui_plot_drag_line_x(int id, double *x, hui_color c);

/* Draggable horizontal line at data y. Returns true while dragging. */
bool hui_plot_drag_line_y(int id, double *y, hui_color c);

/* Draggable rect spanning data (*x1,*y1)→(*x2,*y2).
 * Center handle = move. Four corner handles = resize.
 * NOTE: consumes 5 consecutive sub-IDs starting from id*5. Avoid overlapping.
 * Returns true while any handle is being dragged. */
bool hui_plot_drag_rect(int id, double *x1, double *y1, double *x2, double *y2, hui_color c);

/* ---- Coordinate queries (call between begin/end) ---- */

/* Is the mouse currently inside the plot area? */
bool hui_plot_is_hovered(void);

/* Mouse position in data coordinates. Only valid when hui_plot_is_hovered(). */
void hui_plot_mouse_pos(double *x, double *y);

/* Convert data coordinate to screen pixel. */
void hui_plot_to_pixels(double x, double y, int *px, int *py);

/* Convert screen pixel to data coordinate. */
void hui_plot_pixels_to(int px, int py, double *x, double *y);

/* ---- Colormap ---- */

/* A colormap is a small array of (position, color) stops.
 * hui_colormap_at() linearly interpolates for t in [0,1].
 * Built-in maps: HUI_CMAP_VIRIDIS, HUI_CMAP_PLASMA, HUI_CMAP_HOT. */

#ifndef HUI_CMAP_MAX_STOPS
#  define HUI_CMAP_MAX_STOPS 8
#endif

typedef struct {
    float     t[HUI_CMAP_MAX_STOPS];   /* positions [0,1], sorted ascending */
    hui_color c[HUI_CMAP_MAX_STOPS];   /* color at each stop */
    int       n;                        /* number of stops */
} hui_colormap;

/* Interpolate colormap at t in [0,1]. */
hui_color hui_colormap_at(const hui_colormap *cm, float t);

/* Built-in colormaps (defined in implementation). */
extern const hui_colormap HUI_CMAP_VIRIDIS;
extern const hui_colormap HUI_CMAP_PLASMA;
extern const hui_colormap HUI_CMAP_HOT;

/* ---- Axis format hints ---- */

typedef enum {
    HUI_AXIS_FMT_DEFAULT = 0,  /* %.4g */
    HUI_AXIS_FMT_TIME_S,       /* auto us/ms/s  (ASCII 'u' for micro) */
    HUI_AXIS_FMT_FREQ_HZ,      /* auto Hz/kHz/MHz */
    HUI_AXIS_FMT_DB,           /* %.0fdB */
    HUI_AXIS_FMT_DATE_UNIX,    /* Unix timestamp → "Jan '24" */
} hui_axis_fmt;

/* Override tick label format on x or y axis (call between begin/end). */
void hui_plot_setup_x_fmt(hui_axis_fmt fmt);
void hui_plot_setup_y_fmt(hui_axis_fmt fmt);

/* Set custom tick positions and optional labels (call between begin/end).
 * is_x=true for x-axis, false for y-axis.
 * labels=NULL or labels[i]=NULL → auto-format the value.
 * Replaces auto-step ticks for that axis. */
void hui_plot_setup_axis_ticks(bool is_x, const double *vals,
                               const char * const *labels, int n);

/* Axis scale type. Call between begin/end. */
typedef enum {
    HUI_AXIS_SCALE_LINEAR = 0, /* default */
    HUI_AXIS_SCALE_LOG10,      /* log base 10 — limits must be > 0 */
} hui_axis_scale;

void hui_plot_setup_axis_scale(bool is_x, hui_axis_scale scale);

/* ---- Subplots ---- */

/* Subdivide rect r into a rows×cols grid. Draw title if non-NULL.
 * Use hui_subplots_cell(row, col) to get the rect for each cell,
 * then pass it to hui_plot_begin(). Call hui_subplots_end() when done. */
void     hui_subplots_begin(const char *title, int rows, int cols, hui_rect r);
hui_rect hui_subplots_cell(int row, int col);
void     hui_subplots_end(void);

/* ---- Waterfall / spectrogram buffer ---- */

/* Caller allocates data[cap * bins] floats (row-major). */
typedef struct {
    float *data;   /* caller-owned: cap × bins floats */
    int    bins;   /* FFT bins per frame */
    int    cap;    /* max frames */
    int    head;   /* index where NEXT push goes */
    int    count;  /* frames stored [0, cap] */
} hui_waterfall_buf;

void hui_waterfall_buf_init(hui_waterfall_buf *wb, float *data, int bins, int cap);
/* Push one FFT frame (bins floats). Oldest frame is discarded when full. */
void hui_waterfall_buf_push(hui_waterfall_buf *wb, const float *frame);

/* Standalone spectrogram/waterfall — does NOT use hui_plot_begin/end.
 * x-axis = frequency [0, sample_rate_hz/2], y-axis = time (newest at top).
 * vmin/vmax define the colormap input range (e.g. -120 to 0 for dBFS).
 * frame_period_s: seconds between frames (used for y-axis tick labels).
 * Pass frame_period_s <= 0 to suppress y-axis time labels. */
void hui_plot_waterfall(const char *title, hui_rect r,
                        const hui_waterfall_buf *wb,
                        const hui_colormap *cm,
                        float vmin, float vmax,
                        float sample_rate_hz, float frame_period_s);

/* ---- Heatmap series (inside hui_plot_begin/end) ---- */

/* data[row * cols + col] = value. Axes span [0,cols] × [0,rows] by default. */
void hui_plot_heatmap(const char *label, const float *data,
                      int rows, int cols,
                      float vmin, float vmax,
                      const hui_colormap *cm);

/* 2D histogram heatmap — bins n (xs[i], ys[i]) pairs into an xbins×ybins grid.
 * 0 bins → auto (sqrt(n)). xmin>=xmax or ymin>=ymax → auto-range per axis.
 * Colormap NULL → viridis. Cell value = count (normalized by max). */
void hui_plot_histogram2d(const char *label,
                          const float *xs, const float *ys, int n,
                          int xbins, int ybins,
                          float xmin, float xmax,
                          float ymin, float ymax,
                          const hui_colormap *cm);

/* ---- Audio convenience wrappers (inside hui_plot_begin/end) ---- */

/* FFT magnitude spectrum.
 * Sets x-axis to [0, sample_rate_hz/2] with Hz/kHz formatting.
 * mag[i] = magnitude for bin i. */
void hui_plot_spectrum(const char *label, const float *mag, int n_bins,
                       float sample_rate_hz);

/* PCM waveform.
 * Sets x-axis to [0, n/sample_rate_hz] with time formatting.
 * samples[i] = sample at time i/sample_rate_hz. */
void hui_plot_waveform(const char *label, const float *samples, int n,
                       float sample_rate_hz);

/* Standalone pie/donut chart — does NOT use hui_plot_begin/end.
 * vals[n] need not be normalized (function sums them).
 * labels[n] may be NULL to suppress text labels.
 * cm may be NULL to cycle through the default CUM palette.
 * inner_r in [0,1]: 0 = solid pie, >0 = donut hole (e.g. 0.4). */
void hui_plot_pie(const char *title, hui_rect r,
                  const float *vals, int n, const char **labels,
                  const hui_colormap *cm, float inner_r);

/* ---- Secondary Y axis ---- */

/* Axis selector for series calls. Y1 = primary left axis (default), Y2 = secondary right axis. */
typedef enum {
    HUI_PLOT_AXIS_Y1 = 0,  /* primary left Y axis (default) */
    HUI_PLOT_AXIS_Y2 = 1,  /* secondary right Y axis */
} hui_plot_yaxis;

/* Select which Y axis the NEXT series call uses. Resets to Y1 after each series call.
 * Call between hui_plot_begin/end, before the series function. */
void hui_plot_set_axis(hui_plot_yaxis axis);

/* Set explicit limits for the Y2 axis (optional; auto-range if not called). */
void hui_plot_setup_y2_limits(double ymin, double ymax);

/* ---- Implementation ---- */

#ifdef HUI_PLOT_IMPLEMENTATION

/* ---- Layout ---- */
#define HUI__PLT_TITLE_H    16
#define HUI__PLT_YLABEL_W   10   /* vertical ylabel char column */
#define HUI__PLT_YAXIS_W    46   /* y-tick numbers */
#define HUI__PLT_XAXIS_H    18   /* x-tick numbers + marks */
#define HUI__PLT_XLABEL_H   10   /* xlabel text */
#define HUI__PLT_Y2AXIS_W   46   /* y2-tick numbers on right side */
#define HUI__PLT_MARGIN_R    8
#define HUI__PLT_NTICKS      5

typedef enum {
    HUI__PLT_LINE,
    HUI__PLT_SCATTER,
    HUI__PLT_BARS,
    HUI__PLT_HIST,
    HUI__PLT_STAIRS,
    HUI__PLT_SHADED,
    HUI__PLT_DIGITAL,
    HUI__PLT_HEATMAP,
    HUI__PLT_BARS_H,
    HUI__PLT_STEMS,
    HUI__PLT_ERROR_BARS,
    HUI__PLT_ERROR_BARS_H,
    HUI__PLT_BUBBLES,
    HUI__PLT_CANDLE,
} hui__plt_type;

typedef struct {
    hui__plt_type  type;
    const float   *xs;     /* NULL for line_vals (implicit x = index) */
    const float   *ys;
    const float   *ys2;   /* SHADED: second y-series; CANDLE: closes; ERROR: err */
    const float   *ys3;   /* CANDLE: lows */
    const float   *ys4;   /* CANDLE: highs */
    int            n;
    float          param;  /* BARS: bar_w; HIST: bin count (cast int) */
    float          rmin, rmax; /* HIST range; HEATMAP: vmin/vmax */
    char           label[32];
    hui_color      color;
    const hui_colormap *cmap;  /* HEATMAP: colormap (NULL = VIRIDIS) */
    hui_plot_yaxis yaxis;      /* which Y axis this series uses */
} hui__plt_series;

#define HUI__PLT_MAX_ANNOTS 16
#define HUI__PLT_MAX_TAGS   16

typedef struct { double x, y; char text[32]; hui_color c; } hui__plt_annot;
typedef struct { double v; char text[16]; hui_color c; bool is_x; } hui__plt_tag;

typedef struct {
    hui_rect        outer_r;
    hui_rect        plot_r;
    double          xlim[2], ylim[2];
    bool            xlim_set, ylim_set;
    char            xlabel[32], ylabel[32];
    char            title[64];
    hui__plt_series series[HUI_PLOT_MAX_SERIES];
    int             series_n;
    bool            active;
    double          dxmin, dxmax, dymin, dymax;
    bool            has_data;
    hui__plt_annot  annots[HUI__PLT_MAX_ANNOTS];
    int             annots_n;
    hui__plt_tag    tags[HUI__PLT_MAX_TAGS];
    int             tags_n;
    hui_axis_fmt    xfmt, yfmt;
    hui_axis_scale  xscale, yscale;
    hui_plot_state *interact_state; /* non-NULL when begin_ex used */
    /* Custom ticks (0 = use auto nice-step) */
    int    xticks_n, yticks_n;
    double xtick_vals[HUI_PLOT_MAX_CUSTOM_TICKS];
    double ytick_vals[HUI_PLOT_MAX_CUSTOM_TICKS];
    char   xtick_labels[HUI_PLOT_MAX_CUSTOM_TICKS][16];
    char   ytick_labels[HUI_PLOT_MAX_CUSTOM_TICKS][16];
    /* Secondary Y axis */
    double          y2lim[2];
    bool            y2lim_set;
    double          dy2min, dy2max;  /* auto-range accumulator for y2 */
    bool            has_y2data;
    hui_plot_yaxis  next_yaxis;      /* reset to Y1 after each series */
    char            y2label[32];
} hui__plt_ctx;

static hui__plt_ctx hui__plt;

static const hui_color hui__plt_palette[] = {
    CUM_BLUE, CUM_ORANG, CUM_GREEN, CUM_PINK,
    CUM_YELL, CUM_CYAN,  CUM_PURP,  CUM_ACCENT,
};
#define HUI__PLT_PAL_N ((int)(sizeof(hui__plt_palette)/sizeof(hui__plt_palette[0])))

/* ---- Ring buffer ---- */

void hui_plot_ringbuf_init(hui_plot_ringbuf *rb, float *buf, int cap) {
    rb->data = buf; rb->cap = cap; rb->head = 0; rb->count = 0;
}

void hui_plot_ringbuf_push(hui_plot_ringbuf *rb, float v) {
    if (!rb || !rb->data || rb->cap <= 0) return;
    rb->data[rb->head] = v;
    rb->head = (rb->head + 1) % rb->cap;
    if (rb->count < rb->cap) rb->count++;
}

float hui_plot_ringbuf_get(const hui_plot_ringbuf *rb, int i) {
    if (!rb || !rb->data || rb->count == 0) return 0.0f;
    return rb->data[(rb->head - rb->count + i + rb->cap * 2) % rb->cap];
}

/* ---- Coordinate transforms ---- */

static int hui__px_x(double v) {
    hui__plt_ctx *p = &hui__plt;
    if (p->xscale == HUI_AXIS_SCALE_LOG10) {
        double lv  = (v > 0.0) ? log10(v) : -30.0;
        double lo  = (p->xlim[0] > 0.0) ? log10(p->xlim[0]) : -30.0;
        double hi  = (p->xlim[1] > 0.0) ? log10(p->xlim[1]) : -30.0;
        double r   = hi - lo;
        if (r == 0.0) return p->plot_r.x;
        return p->plot_r.x + (int)((lv - lo) / r * p->plot_r.w);
    }
    double r = p->xlim[1] - p->xlim[0];
    if (r == 0.0) return p->plot_r.x;
    return p->plot_r.x + (int)((v - p->xlim[0]) / r * p->plot_r.w);
}

static int hui__px_y(double v) {
    hui__plt_ctx *p = &hui__plt;
    if (p->yscale == HUI_AXIS_SCALE_LOG10) {
        double lv  = (v > 0.0) ? log10(v) : -30.0;
        double lo  = (p->ylim[0] > 0.0) ? log10(p->ylim[0]) : -30.0;
        double hi  = (p->ylim[1] > 0.0) ? log10(p->ylim[1]) : -30.0;
        double r   = hi - lo;
        if (r == 0.0) return p->plot_r.y + p->plot_r.h / 2;
        return p->plot_r.y + p->plot_r.h - 1
               - (int)((lv - lo) / r * (p->plot_r.h - 1));
    }
    double r = p->ylim[1] - p->ylim[0];
    if (r == 0.0) return p->plot_r.y + p->plot_r.h / 2;
    return p->plot_r.y + p->plot_r.h - 1
           - (int)((v - p->ylim[0]) / r * (p->plot_r.h - 1));
}

/* Secondary Y axis pixel transform (uses y2lim, always linear). */
static int hui__px_y2(double v) {
    hui__plt_ctx *p = &hui__plt;
    double r = p->y2lim[1] - p->y2lim[0];
    if (r == 0.0) return p->plot_r.y + p->plot_r.h / 2;
    return p->plot_r.y + p->plot_r.h - 1
           - (int)((v - p->y2lim[0]) / r * (p->plot_r.h - 1));
}

/* ---- Tick step ---- */

static double hui__nice_step(double range, int n) {
    if (range <= 0.0 || n <= 0) return 1.0;
    double e = floor(log10(range / (double)n));
    double f = (range / (double)n) / pow(10.0, e);
    double nice = (f < 1.5) ? 1.0 : (f < 3.0) ? 2.0 : (f < 7.0) ? 5.0 : 10.0;
    return nice * pow(10.0, e);
}

/* ---- Tick label formatting ---- */

static const char *hui__fmt_tick(double v, hui_axis_fmt fmt) {
    switch (fmt) {
        case HUI_AXIS_FMT_TIME_S:
            if (v != 0.0 && fabs(v) < 1e-3)
                return hui_fmt("%.1fus", v * 1e6);
            if (fabs(v) < 1.0)
                return hui_fmt("%.1fms", v * 1e3);
            return hui_fmt("%.3gs", v);
        case HUI_AXIS_FMT_FREQ_HZ:
            if (fabs(v) < 1e3)
                return hui_fmt("%.0fHz", v);
            if (fabs(v) < 1e6)
                return hui_fmt("%.2fkHz", v * 1e-3);
            return hui_fmt("%.1fMHz", v * 1e-6);
        case HUI_AXIS_FMT_DB:
            return hui_fmt("%.0fdB", v);
        case HUI_AXIS_FMT_DATE_UNIX: {
            time_t t = (time_t)(long long)v;
            struct tm *tm = gmtime(&t);
            if (!tm) return hui_fmt("%.4g", v);
            static char _dbuf[16];
            strftime(_dbuf, sizeof(_dbuf), "%b '%y", tm);
            return _dbuf;
        }
        default:
            return hui_fmt("%.4g", v);
    }
}

/* ---- Auto-range ---- */

static void hui__plt_acc(float v, double *mn, double *mx) {
    if ((double)v < *mn) *mn = (double)v;
    if ((double)v > *mx) *mx = (double)v;
}

/* ---- Series draw ---- */

static void hui__draw_line(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    for (int i = 1; i < s->n; i++) {
        float fx0 = s->xs ? s->xs[i-1] : (float)(i-1);
        float fy0 = s->ys[i-1];
        float fx1 = s->xs ? s->xs[i]   : (float)i;
        float fy1 = s->ys[i];
        /* NaN = gap sentinel: skip segment if either endpoint is NaN */
        if (isnan(fx0) || isnan(fy0) || isnan(fx1) || isnan(fy1)) continue;
        hui_line(hui__px_x((double)fx0), y2 ? hui__px_y2((double)fy0) : hui__px_y((double)fy0),
                 hui__px_x((double)fx1), y2 ? hui__px_y2((double)fy1) : hui__px_y((double)fy1),
                 s->color, 1);
    }
    hui_clip_pop();
}

static void hui__draw_scatter(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    for (int i = 0; i < s->n; i++)
        hui_circle_fill(hui__px_x((double)s->xs[i]),
                        y2 ? hui__px_y2((double)s->ys[i]) : hui__px_y((double)s->ys[i]),
                        2, s->color);
    hui_clip_pop();
}

static void hui__draw_bars(hui__plt_series *s) {
    hui__plt_ctx *p = &hui__plt;
    hui_rect pr = p->plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    double bw2 = (double)s->param * 0.5;
    int y0 = y2 ? hui__px_y2(0.0) : hui__px_y(0.0);
    if (y0 < pr.y)        y0 = pr.y;
    if (y0 > pr.y + pr.h) y0 = pr.y + pr.h;
    for (int i = 0; i < s->n; i++) {
        int bx = hui__px_x((double)s->xs[i] - bw2);
        int br = hui__px_x((double)s->xs[i] + bw2);
        int by = y2 ? hui__px_y2((double)s->ys[i]) : hui__px_y((double)s->ys[i]);
        int bh = y0 - by;
        if (bh < 0) { by = y0; bh = -bh; }
        if (bh < 1) bh = 1;
        if (br > bx) hui_rect_fill(hui_rect_make(bx, by, br - bx, bh), s->color, 0);
    }
    hui_clip_pop();
}

static void hui__draw_bars_h(hui__plt_series *s) {
    hui__plt_ctx *p = &hui__plt;
    hui_rect pr = p->plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    double bh2 = (double)s->param * 0.5;
    int x0 = hui__px_x(0.0);
    if (x0 < pr.x)        x0 = pr.x;
    if (x0 > pr.x + pr.w) x0 = pr.x + pr.w;
    for (int i = 0; i < s->n; i++) {
        int by = y2 ? hui__px_y2((double)s->ys[i] + bh2) : hui__px_y((double)s->ys[i] + bh2);
        int bb = y2 ? hui__px_y2((double)s->ys[i] - bh2) : hui__px_y((double)s->ys[i] - bh2);
        int bx = hui__px_x((double)s->xs[i]);
        int bw = bx - x0;
        if (bw < 0) { bx = x0; bw = -bw; }
        if (bw < 1) bw = 1;
        if (bb > by) hui_rect_fill(hui_rect_make(x0, by, bw, bb - by), s->color, 0);
    }
    hui_clip_pop();
}

static void hui__draw_stems(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    int y0 = y2 ? hui__px_y2(0.0) : hui__px_y(0.0);
    for (int i = 0; i < s->n; i++) {
        int px = hui__px_x((double)s->xs[i]);
        int py = y2 ? hui__px_y2((double)s->ys[i]) : hui__px_y((double)s->ys[i]);
        hui_line(px, y0, px, py, s->color, 1);
        hui_circle_fill(px, py, 2, s->color);
    }
    hui_clip_pop();
}

static void hui__draw_error_bars(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    const int cap = 4; /* half-width of end-cap in pixels */
    for (int i = 0; i < s->n; i++) {
        int px  = hui__px_x((double)s->xs[i]);
        int py  = y2 ? hui__px_y2((double)s->ys[i])                        : hui__px_y((double)s->ys[i]);
        int pyt = y2 ? hui__px_y2((double)s->ys[i] + (double)s->ys2[i])   : hui__px_y((double)s->ys[i] + (double)s->ys2[i]);
        int pyb = y2 ? hui__px_y2((double)s->ys[i] - (double)s->ys2[i])   : hui__px_y((double)s->ys[i] - (double)s->ys2[i]);
        hui_line(px, pyb, px, pyt, s->color, 1);              /* vertical shaft */
        hui_line(px - cap, pyt, px + cap, pyt, s->color, 1);  /* top cap */
        hui_line(px - cap, pyb, px + cap, pyb, s->color, 1);  /* bottom cap */
        hui_circle_fill(px, py, 2, s->color);                  /* center dot */
    }
    hui_clip_pop();
}

static void hui__draw_error_bars_h(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    const int cap = 4;
    for (int i = 0; i < s->n; i++) {
        int py  = y2 ? hui__px_y2((double)s->ys[i]) : hui__px_y((double)s->ys[i]);
        int px  = hui__px_x((double)s->xs[i]);
        int pxr = hui__px_x((double)s->xs[i] + (double)s->ys2[i]);
        int pxl = hui__px_x((double)s->xs[i] - (double)s->ys2[i]);
        hui_line(pxl, py, pxr, py, s->color, 1);              /* horizontal shaft */
        hui_line(pxr, py - cap, pxr, py + cap, s->color, 1);  /* right cap */
        hui_line(pxl, py - cap, pxl, py + cap, s->color, 1);  /* left cap */
        hui_circle_fill(px, py, 2, s->color);
    }
    hui_clip_pop();
}

static void hui__draw_bubbles(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    /* Convert sz (data units) to pixels using x-axis scale */
    double xscale = (hui__plt.xlim[1] > hui__plt.xlim[0])
        ? (double)pr.w / (hui__plt.xlim[1] - hui__plt.xlim[0]) : 1.0;
    hui_color fill = { s->color.r, s->color.g, s->color.b, 80 };
    for (int i = 0; i < s->n; i++) {
        int px = hui__px_x((double)s->xs[i]);
        int py = y2 ? hui__px_y2((double)s->ys[i]) : hui__px_y((double)s->ys[i]);
        int r  = (int)((double)s->ys2[i] * (double)s->param * xscale);
        if (r < 1) r = 1;
        hui_circle_fill(px, py, r, fill);
        hui_circle(px, py, r, s->color);
    }
    hui_clip_pop();
}

static void hui__draw_candle(hui__plt_series *s) {
    /* xs=dates, ys=opens, ys2=closes, ys3=lows, ys4=highs */
    if (!s->xs || !s->ys || !s->ys2 || !s->ys3 || !s->ys4) return;
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);

    /* Auto body half-width: ~40% of average candle spacing in pixels */
    int half_w = 4;
    if (s->n >= 2) {
        double span = (double)s->xs[s->n-1] - (double)s->xs[0];
        double dx_avg = span / (double)(s->n - 1);
        int px_spacing = hui__px_x((double)s->xs[0] + dx_avg) - hui__px_x((double)s->xs[0]);
        half_w = px_spacing * 2 / 5;
        if (half_w < 1) half_w = 1;
        if (half_w > 12) half_w = 12;
    }

    hui_color up_c   = HUI_RGB(72, 200, 120);  /* close > open  — bullish */
    hui_color down_c = HUI_RGB(220, 80,  80);  /* close <= open — bearish */

    for (int i = 0; i < s->n; i++) {
        int px    = hui__px_x((double)s->xs[i]);
        int p_lo  = y2 ? hui__px_y2((double)s->ys3[i]) : hui__px_y((double)s->ys3[i]);
        int p_hi  = y2 ? hui__px_y2((double)s->ys4[i]) : hui__px_y((double)s->ys4[i]);
        int p_op  = y2 ? hui__px_y2((double)s->ys[i])  : hui__px_y((double)s->ys[i]);
        int p_cl  = y2 ? hui__px_y2((double)s->ys2[i]) : hui__px_y((double)s->ys2[i]);

        bool bull = s->ys2[i] >= s->ys[i];
        hui_color body_c = bull ? up_c : down_c;

        /* Wick: low to high */
        hui_line(px, p_lo, px, p_hi, body_c, 1);

        /* Body: open to close (ensure top < bottom for rect) */
        int body_top = p_cl < p_op ? p_cl : p_op;
        int body_bot = p_cl > p_op ? p_cl : p_op;
        int body_h   = body_bot - body_top;
        if (body_h < 1) body_h = 1;

        hui_rect br = hui_rect_make(px - half_w, body_top, half_w * 2, body_h);
        hui_rect_fill(br, body_c, 0);
        hui_rect_outline(br, HUI_RGBA(0, 0, 0, 80), 0);
    }
    hui_clip_pop();
}

static void hui__draw_hist(hui__plt_series *s) {
    int bins = (int)s->param;
    if (bins <= 0) bins = (int)sqrt((double)s->n);
    if (bins < 1)  bins = 1;
    if (bins > 256) bins = 256;

    float rmin = s->rmin, rmax = s->rmax;
    if (rmin >= rmax) {
        rmin = rmax = s->xs[0];
        for (int i = 1; i < s->n; i++) {
            if (s->xs[i] < rmin) rmin = s->xs[i];
            if (s->xs[i] > rmax) rmax = s->xs[i];
        }
        if (rmin >= rmax) rmax = rmin + 1.0f;
    }

    int counts[256] = {0};
    float bw = (rmax - rmin) / (float)bins;
    for (int i = 0; i < s->n; i++) {
        int b = (int)((s->xs[i] - rmin) / bw);
        if (b < 0) b = 0;
        if (b >= bins) b = bins - 1;
        counts[b]++;
    }
    int cmax = 1;
    for (int b = 0; b < bins; b++) if (counts[b] > cmax) cmax = counts[b];

    hui__plt_ctx *p = &hui__plt;
    if (!p->xlim_set) { p->xlim[0] = rmin; p->xlim[1] = rmax; }
    if (!p->ylim_set) { p->ylim[0] = 0.0;  p->ylim[1] = cmax * 1.15; }

    hui_rect pr = p->plot_r;
    hui_clip_push(pr);
    for (int b = 0; b < bins; b++) {
        int bx = hui__px_x((double)(rmin + (float)b * bw)) + 1;
        int br = hui__px_x((double)(rmin + (float)(b+1) * bw)) - 1;
        int by = hui__px_y((double)counts[b]);
        int y0 = hui__px_y(0.0);
        if (y0 > pr.y + pr.h) y0 = pr.y + pr.h;
        int bh = y0 - by;
        if (bh < 1) bh = 1;
        if (br > bx) hui_rect_fill(hui_rect_make(bx, by, br - bx, bh), s->color, 0);
    }
    hui_clip_pop();
}

static void hui__draw_stairs(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    for (int i = 0; i < s->n - 1; i++) {
        float fx0 = s->xs ? s->xs[i]   : (float)i;
        float fy0 = s->ys[i];
        float fx1 = s->xs ? s->xs[i+1] : (float)(i+1);
        float fy1 = s->ys[i+1];
        if (isnan(fx0)||isnan(fy0)||isnan(fx1)||isnan(fy1)) continue;
        int x0 = hui__px_x((double)fx0);
        int y0 = y2 ? hui__px_y2((double)fy0) : hui__px_y((double)fy0);
        int x1 = hui__px_x((double)fx1);
        int y1 = y2 ? hui__px_y2((double)fy1) : hui__px_y((double)fy1);
        /* horizontal segment at current y, then vertical to next y */
        hui_line(x0, y0, x1, y0, s->color, 1);
        hui_line(x1, y0, x1, y1, s->color, 1);
    }
    hui_clip_pop();
}

static void hui__draw_shaded(hui__plt_series *s) {
    if (!s->ys2) return;
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    bool y2 = (s->yaxis == HUI_PLOT_AXIS_Y2);
    for (int i = 0; i < s->n - 1; i++) {
        float fx0 = s->xs ? s->xs[i]   : (float)i;
        float fx1 = s->xs ? s->xs[i+1] : (float)(i+1);
        if (isnan(fx0)||isnan(fx1)) continue;
        /* quad as two triangles */
        int ax  = hui__px_x((double)fx0);
        int bx  = hui__px_x((double)fx1);
        int ay1 = y2 ? hui__px_y2((double)s->ys[i])    : hui__px_y((double)s->ys[i]);
        int by1 = y2 ? hui__px_y2((double)s->ys[i+1])  : hui__px_y((double)s->ys[i+1]);
        int ay2 = y2 ? hui__px_y2((double)s->ys2[i])   : hui__px_y((double)s->ys2[i]);
        int by2 = y2 ? hui__px_y2((double)s->ys2[i+1]) : hui__px_y((double)s->ys2[i+1]);
        hui_color fc = HUI_RGBA(s->color.r, s->color.g, s->color.b, 60);
        hui_triangle_fill((hui_v2i){(int16_t)ax,(int16_t)ay1},(hui_v2i){(int16_t)bx,(int16_t)by1},(hui_v2i){(int16_t)bx,(int16_t)by2}, fc);
        hui_triangle_fill((hui_v2i){(int16_t)ax,(int16_t)ay1},(hui_v2i){(int16_t)bx,(int16_t)by2},(hui_v2i){(int16_t)ax,(int16_t)ay2}, fc);
        /* outline the two bounding lines */
        hui_line(ax, ay1, bx, by1, s->color, 1);
        hui_line(ax, ay2, bx, by2, s->color, 1);
    }
    hui_clip_pop();
}

static void hui__draw_digital(hui__plt_series *s) {
    hui_rect pr = hui__plt.plot_r;
    hui_clip_push(pr);
    /* Map signal into a 1/3 height band near the bottom of the plot */
    int band_h = pr.h / 3;
    int base_y = pr.y + pr.h - band_h / 2;
    for (int i = 0; i < s->n - 1; i++) {
        float fx0 = s->xs ? s->xs[i]   : (float)i;
        float fx1 = s->xs ? s->xs[i+1] : (float)(i+1);
        float fv0 = s->ys[i], fv1 = s->ys[i+1];
        if (isnan(fx0)||isnan(fv0)||isnan(fx1)||isnan(fv1)) continue;
        int x0 = hui__px_x((double)fx0), x1 = hui__px_x((double)fx1);
        int y0 = base_y - (fv0 != 0.0f ? band_h : 0);
        int y1 = base_y - (fv1 != 0.0f ? band_h : 0);
        hui_line(x0, y0, x1 - 1, y0, s->color, 1);   /* horizontal */
        if (y0 != y1) hui_line(x1-1, y0, x1-1, y1, s->color, 1); /* edge */
    }
    hui_clip_pop();
}

static void hui__draw_heatmap(hui__plt_series *s) {
    hui__plt_ctx *p = &hui__plt;
    hui_rect pr = p->plot_r;
    hui_clip_push(pr);

    int cols  = (int)s->param;   /* data columns (HEATMAP: param stores cols) */
    int rows  = s->n;             /* data rows */
    const float *data = s->xs;   /* data pointer stored in xs field */
    float vmin  = s->rmin, vmax = s->rmax;
    float vrange = vmax > vmin ? vmax - vmin : 1.0f;
    const hui_colormap *cm = s->cmap ? s->cmap : &HUI_CMAP_VIRIDIS;

    for (int row = 0; row < rows; row++) {
        int py0 = pr.y + row     * pr.h / rows;
        int py1 = pr.y + (row+1) * pr.h / rows;
        if (py1 <= py0) py1 = py0 + 1;
        for (int col = 0; col < cols; col++) {
            float v = data[row * cols + col];
            float t = (v - vmin) / vrange;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            hui_color c = hui_colormap_at(cm, t);
            int px0 = pr.x + col     * pr.w / cols;
            int px1 = pr.x + (col+1) * pr.w / cols;
            if (px1 <= px0) px1 = px0 + 1;
            hui_rect_fill(hui_rect_make(px0, py0, px1-px0, py1-py0), c, 0);
        }
    }
    hui_clip_pop();
}

static void hui__draw_annots(void) {
    hui__plt_ctx *p = &hui__plt;
    for (int i = 0; i < p->annots_n; i++) {
        hui__plt_annot *a = &p->annots[i];
        int px = hui__px_x(a->x), py = hui__px_y(a->y);
        hui_circle_fill(px, py, 3, a->c);
        hui_text(px + 5, py - HUI_FONT_H / 2, a->text, a->c);
    }
    for (int i = 0; i < p->tags_n; i++) {
        hui__plt_tag *t = &p->tags[i];
        hui_rect pr = p->plot_r;
        if (t->is_x) {
            int px = hui__px_x(t->v);
            hui_line(px, pr.y, px, pr.y + pr.h, t->c, 1);
            int lw = (int)strlen(t->text) * (HUI_FONT_W + 1);
            hui_rect_fill(hui_rect_make(px - lw/2 - 2, pr.y + pr.h + 2, lw + 4, HUI_FONT_H + 2), t->c, 1);
            hui_text(px - lw/2, pr.y + pr.h + 3, t->text, CUM_BG0);
        } else {
            int py = hui__px_y(t->v);
            hui_line(pr.x, py, pr.x + pr.w, py, t->c, 1);
            int lw = (int)strlen(t->text) * (HUI_FONT_W + 1);
            hui_rect_fill(hui_rect_make(pr.x - lw - 6, py - HUI_FONT_H/2 - 1, lw + 4, HUI_FONT_H + 2), t->c, 1);
            hui_text(pr.x - lw - 4, py - HUI_FONT_H/2, t->text, CUM_BG0);
        }
    }
}

/* ---- Axes + grid ---- */

static void hui__draw_axes(void) {
    hui__plt_ctx *p = &hui__plt;
    hui_rect pr = p->plot_r;

    /* Background */
    hui_set_layer(HUI_LAYER_BG);
    hui_rect_fill(pr, CUM_BG0, 0);
    hui_reset_layer();

    /* X grid + ticks */
    if (p->xticks_n > 0) {
        for (int ti = 0; ti < p->xticks_n; ti++) {
            double xv = p->xtick_vals[ti];
            int px = hui__px_x(xv);
            if (px < pr.x - 1 || px > pr.x + pr.w + 1) continue;
            hui_line(px, pr.y, px, pr.y + pr.h, CUM_BG3, 1);
            hui_line(px, pr.y + pr.h, px, pr.y + pr.h + 4, CUM_FG3, 1);
            const char *lbl = p->xtick_labels[ti][0]
                ? p->xtick_labels[ti] : hui__fmt_tick(xv, p->xfmt);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(px - lw / 2, pr.y + pr.h + 6, lbl, CUM_FG2);
        }
    } else if (p->xscale == HUI_AXIS_SCALE_LOG10) {
        /* Log10 ticks: one per decade + 2/5 sub-ticks */
        double lo = p->xlim[0] > 0.0 ? p->xlim[0] : 1e-10;
        double hi = p->xlim[1] > lo  ? p->xlim[1] : lo * 10.0;
        int exp0 = (int)floor(log10(lo));
        int exp1 = (int)ceil(log10(hi));
        static const double sub[] = {2.0, 5.0};
        for (int e = exp0; e <= exp1; e++) {
            double base = pow(10.0, (double)e);
            /* major tick */
            double xv = base;
            if (xv >= lo && xv <= hi) {
                int px = hui__px_x(xv);
                if (px >= pr.x - 1 && px <= pr.x + pr.w + 1) {
                    hui_line(px, pr.y, px, pr.y + pr.h, CUM_BG3, 1);
                    hui_line(px, pr.y + pr.h, px, pr.y + pr.h + 4, CUM_FG3, 1);
                    char lbuf[24]; snprintf(lbuf, sizeof lbuf, "1e%d", e);
                    int lw = (int)strlen(lbuf) * (HUI_FONT_W + 1);
                    hui_text(px - lw/2, pr.y + pr.h + 6, lbuf, CUM_FG2);
                }
            }
            /* sub-ticks */
            for (int si = 0; si < 2; si++) {
                xv = base * sub[si];
                if (xv < lo || xv > hi) continue;
                int px = hui__px_x(xv);
                if (px < pr.x - 1 || px > pr.x + pr.w + 1) continue;
                hui_line(px, pr.y, px, pr.y + pr.h, (hui_color){CUM_BG3.r,CUM_BG3.g,CUM_BG3.b,80}, 1);
                hui_line(px, pr.y + pr.h, px, pr.y + pr.h + 3, CUM_FG3, 1);
            }
        }
    } else {
        double xstep = hui__nice_step(p->xlim[1] - p->xlim[0], HUI__PLT_NTICKS);
        double xv = ceil(p->xlim[0] / xstep) * xstep;
        for (; xv <= p->xlim[1] + xstep * 1e-4; xv += xstep) {
            int px = hui__px_x(xv);
            if (px < pr.x - 1 || px > pr.x + pr.w + 1) continue;
            hui_line(px, pr.y, px, pr.y + pr.h, CUM_BG3, 1);
            hui_line(px, pr.y + pr.h, px, pr.y + pr.h + 4, CUM_FG3, 1);
            const char *lbl = hui__fmt_tick(xv, p->xfmt);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(px - lw / 2, pr.y + pr.h + 6, lbl, CUM_FG2);
        }
    }

    /* Y grid + ticks */
    if (p->yticks_n > 0) {
        for (int ti = 0; ti < p->yticks_n; ti++) {
            double yv = p->ytick_vals[ti];
            int py = hui__px_y(yv);
            if (py < pr.y - 1 || py > pr.y + pr.h + 1) continue;
            hui_line(pr.x, py, pr.x + pr.w, py, CUM_BG3, 1);
            hui_line(pr.x - 4, py, pr.x, py, CUM_FG3, 1);
            const char *lbl = p->ytick_labels[ti][0]
                ? p->ytick_labels[ti] : hui__fmt_tick(yv, p->yfmt);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(pr.x - lw - 6, py - HUI_FONT_H / 2, lbl, CUM_FG2);
        }
    } else if (p->yscale == HUI_AXIS_SCALE_LOG10) {
        double lo = p->ylim[0] > 0.0 ? p->ylim[0] : 1e-10;
        double hi = p->ylim[1] > lo  ? p->ylim[1] : lo * 10.0;
        int exp0 = (int)floor(log10(lo));
        int exp1 = (int)ceil(log10(hi));
        static const double sub[] = {2.0, 5.0};
        for (int e = exp0; e <= exp1; e++) {
            double base = pow(10.0, (double)e);
            double yv = base;
            if (yv >= lo && yv <= hi) {
                int py = hui__px_y(yv);
                if (py >= pr.y - 1 && py <= pr.y + pr.h + 1) {
                    hui_line(pr.x, py, pr.x + pr.w, py, CUM_BG3, 1);
                    hui_line(pr.x - 4, py, pr.x, py, CUM_FG3, 1);
                    char lbuf[24]; snprintf(lbuf, sizeof lbuf, "1e%d", e);
                    int lw = (int)strlen(lbuf) * (HUI_FONT_W + 1);
                    hui_text(pr.x - lw - 6, py - HUI_FONT_H/2, lbuf, CUM_FG2);
                }
            }
            for (int si = 0; si < 2; si++) {
                yv = base * sub[si];
                if (yv < lo || yv > hi) continue;
                int py = hui__px_y(yv);
                if (py < pr.y - 1 || py > pr.y + pr.h + 1) continue;
                hui_line(pr.x, py, pr.x + pr.w, py, (hui_color){CUM_BG3.r,CUM_BG3.g,CUM_BG3.b,80}, 1);
                hui_line(pr.x - 3, py, pr.x, py, CUM_FG3, 1);
            }
        }
    } else {
        double ystep = hui__nice_step(p->ylim[1] - p->ylim[0], HUI__PLT_NTICKS);
        double yv = ceil(p->ylim[0] / ystep) * ystep;
        for (; yv <= p->ylim[1] + ystep * 1e-4; yv += ystep) {
            int py = hui__px_y(yv);
            if (py < pr.y - 1 || py > pr.y + pr.h + 1) continue;
            hui_line(pr.x, py, pr.x + pr.w, py, CUM_BG3, 1);
            hui_line(pr.x - 4, py, pr.x, py, CUM_FG3, 1);
            const char *lbl = hui__fmt_tick(yv, p->yfmt);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(pr.x - lw - 6, py - HUI_FONT_H / 2, lbl, CUM_FG2);
        }
    }

    /* Y2 axis (right side) — only if Y2 data present */
    if (p->has_y2data) {
        int rx = pr.x + pr.w;  /* right edge of plot area */
        double y2step = hui__nice_step(p->y2lim[1] - p->y2lim[0], HUI__PLT_NTICKS);
        double yv2 = ceil(p->y2lim[0] / y2step) * y2step;
        for (; yv2 <= p->y2lim[1] + y2step * 1e-4; yv2 += y2step) {
            int py = hui__px_y2(yv2);
            if (py < pr.y - 1 || py > pr.y + pr.h + 1) continue;
            /* tick mark pointing into plot from right */
            hui_line(rx, py, rx + 4, py, CUM_FG3, 1);
            /* label to the right of the plot */
            const char *lbl = hui__fmt_tick(yv2, p->yfmt);
            hui_text(rx + 6, py - HUI_FONT_H / 2, lbl, CUM_FG2);
        }
        /* Y2 axis border line */
        hui_line(rx, pr.y, rx, pr.y + pr.h, CUM_FG3, 1);
        /* Y2 label — stacked chars on far right */
        if (p->y2label[0]) {
            int llen = (int)strlen(p->y2label);
            int lx   = p->outer_r.x + p->outer_r.w - 10;
            int ly   = pr.y + (pr.h - llen * (HUI_FONT_H + 2)) / 2;
            for (int i = 0; i < llen; i++) {
                char ch[2] = { p->y2label[i], '\0' };
                hui_text(lx, ly + i * (HUI_FONT_H + 2), ch, CUM_FG2);
            }
        }
    }

    /* Axis border lines */
    hui_line(pr.x,        pr.y,        pr.x,        pr.y + pr.h, CUM_FG3, 1);
    hui_line(pr.x,        pr.y + pr.h, pr.x + pr.w, pr.y + pr.h, CUM_FG3, 1);
    hui_line(pr.x + pr.w, pr.y,        pr.x + pr.w, pr.y + pr.h, CUM_BG3, 1);
    hui_line(pr.x,        pr.y,        pr.x + pr.w, pr.y,        CUM_BG3, 1);

    /* xlabel */
    if (p->xlabel[0]) {
        int lw = (int)strlen(p->xlabel) * (HUI_FONT_W + 1);
        hui_text(pr.x + (pr.w - lw) / 2,
                 pr.y + pr.h + HUI__PLT_XAXIS_H,
                 p->xlabel, CUM_FG2);
    }

    /* ylabel — vertical stacked chars */
    if (p->ylabel[0]) {
        int llen = (int)strlen(p->ylabel);
        int lx   = p->outer_r.x + 2;
        int ly   = pr.y + (pr.h - llen * (HUI_FONT_H + 2)) / 2;
        for (int i = 0; i < llen; i++) {
            char ch[2] = { p->ylabel[i], '\0' };
            hui_text(lx, ly + i * (HUI_FONT_H + 2), ch, CUM_FG2);
        }
    }

    /* outer panel border */
    hui_rect_outline(p->outer_r, CUM_BG3, 3);
}

/* ---- Legend ---- */

static void hui__draw_legend(void) {
    hui__plt_ctx *p = &hui__plt;
    if (p->series_n == 0) return;
    /* no labelled series = no legend (apps that draw their own pass "" as the label) */
    bool labelled = false;
    for (int i = 0; i < p->series_n; i++) if (p->series[i].label[0]) labelled = true;
    if (!labelled) return;

    int row_h = HUI_FONT_PIXEL_H + 5;   /* scaled font height, so rows do not overlap at HUI_FONT_SCALE > 1 */
    int lw    = 96;   /* grows to fit the widest label (scaled fonts / text hooks) */
    for (int i = 0; i < p->series_n; i++) { int w = 30 + hui_text_width(p->series[i].label); if (w > lw) lw = w; }
    int lh    = p->series_n * row_h + 6;
    int lx    = p->plot_r.x + p->plot_r.w - lw - 4;
    int ly    = p->plot_r.y + 4;

    hui_set_layer(HUI_LAYER_POPUP);
    hui_rect_fill(hui_rect_make(lx, ly, lw, lh), HUI_RGBA(10, 10, 10, 200), 2);
    hui_rect_outline(hui_rect_make(lx, ly, lw, lh), CUM_BG3, 2);

    for (int i = 0; i < p->series_n; i++) {
        hui__plt_series *s = &p->series[i];
        int ey = ly + 3 + i * row_h + row_h / 2;
        hui_line(lx + 4, ey, lx + 18, ey, s->color, 2);
        hui_text(lx + 22, ey - HUI_FONT_PIXEL_H / 2, s->label, CUM_FG);
    }
    hui_reset_layer();
}

/* ---- Public API ---- */

bool hui_plot_begin(const char *title, hui_rect r) {
    if (r.w < 80 || r.h < 60) return false;
    hui__plt_ctx *p = &hui__plt;
    memset(p, 0, sizeof(*p));
    p->active      = true;
    p->outer_r     = r;
    p->dxmin       =  1e30;  p->dxmax  = -1e30;
    p->dymin       =  1e30;  p->dymax  = -1e30;
    p->dy2min      =  1e30;  p->dy2max = -1e30;
    p->next_yaxis  = HUI_PLOT_AXIS_Y1;
    p->y2lim_set   = false;
    p->has_y2data  = false;

    if (title) {
        strncpy(p->title, title, 63);
        p->title[63] = '\0';
    }

    int left  = HUI__PLT_YLABEL_W + HUI__PLT_YAXIS_W;
    int top   = HUI__PLT_TITLE_H;
    int bot   = HUI__PLT_XAXIS_H + HUI__PLT_XLABEL_H;
    int right = HUI__PLT_Y2AXIS_W;  /* always reserve right axis area */
    p->plot_r = hui_rect_make(r.x + left, r.y + top,
                               r.w - left - right,
                               r.h - top - bot);
    if (p->plot_r.w < 10 || p->plot_r.h < 10) { p->active = false; return false; }

    /* Draw outer background + title now */
    hui_set_layer(HUI_LAYER_BG);
    hui_rect_fill(r, CUM_BG1, 3);
    hui_reset_layer();

    int tw = (int)strlen(p->title) * (HUI_FONT_W + 1);
    hui_text(r.x + (r.w - tw) / 2,
             r.y + (HUI__PLT_TITLE_H - HUI_FONT_H) / 2,
             p->title, CUM_FG);

    return true;
}

void hui_plot_end(void) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active) return;

    /* Finalize auto-range */
    if (!p->xlim_set) {
        if (p->has_data && p->dxmax > p->dxmin) {
            double pad = (p->dxmax - p->dxmin) * 0.05;
            p->xlim[0] = p->dxmin - pad;
            p->xlim[1] = p->dxmax + pad;
        } else { p->xlim[0] = 0.0; p->xlim[1] = 1.0; }
    }
    if (!p->ylim_set) {
        if (p->has_data && p->dymax > p->dymin) {
            double pad = (p->dymax - p->dymin) * 0.08;
            p->ylim[0] = p->dymin - pad;
            p->ylim[1] = p->dymax + pad;
        } else { p->ylim[0] = 0.0; p->ylim[1] = 1.0; }
    }
    /* Clamp log-scale limits to strictly positive */
    if (p->xscale == HUI_AXIS_SCALE_LOG10 && p->xlim[0] <= 0.0) {
        p->xlim[0] = (p->xlim[1] > 0.0) ? p->xlim[1] * 1e-6 : 1e-10;
    }
    if (p->yscale == HUI_AXIS_SCALE_LOG10 && p->ylim[0] <= 0.0) {
        p->ylim[0] = (p->ylim[1] > 0.0) ? p->ylim[1] * 1e-6 : 1e-10;
    }
    /* Y2 auto-range */
    if (!p->y2lim_set) {
        if (p->has_y2data && p->dy2max > p->dy2min) {
            double pad = (p->dy2max - p->dy2min) * 0.08;
            p->y2lim[0] = p->dy2min - pad;
            p->y2lim[1] = p->dy2max + pad;
        } else { p->y2lim[0] = 0.0; p->y2lim[1] = 1.0; }
    }

    /* Render axes/grid */
    hui__draw_axes();

    /* Render series */
    for (int i = 0; i < p->series_n; i++) {
        hui__plt_series *s = &p->series[i];
        switch (s->type) {
            case HUI__PLT_LINE:    hui__draw_line(s);    break;
            case HUI__PLT_SCATTER: hui__draw_scatter(s); break;
            case HUI__PLT_BARS:    hui__draw_bars(s);    break;
            case HUI__PLT_HIST:    hui__draw_hist(s);    break;
            case HUI__PLT_STAIRS:  hui__draw_stairs(s);  break;
            case HUI__PLT_SHADED:  hui__draw_shaded(s);  break;
            case HUI__PLT_DIGITAL: hui__draw_digital(s); break;
            case HUI__PLT_HEATMAP: hui__draw_heatmap(s); break;
            case HUI__PLT_BARS_H:       hui__draw_bars_h(s);       break;
            case HUI__PLT_STEMS:        hui__draw_stems(s);        break;
            case HUI__PLT_ERROR_BARS:   hui__draw_error_bars(s);   break;
            case HUI__PLT_ERROR_BARS_H: hui__draw_error_bars_h(s); break;
            case HUI__PLT_BUBBLES:      hui__draw_bubbles(s);      break;
            case HUI__PLT_CANDLE:       hui__draw_candle(s);       break;
        }
    }

    /* Annotations + tags */
    hui__draw_annots();

    /* Legend */
    hui__draw_legend();

    /* Pan / zoom interaction (only when begin_ex used) */
    if (p->interact_state && hui_g) {
        hui_plot_state *st = p->interact_state;
        const hui_io_ctx *io = &hui_g->io;

        /* First frame: capture auto-resolved limits */
        if (!st->initialized) {
            st->xlim[0] = p->xlim[0]; st->xlim[1] = p->xlim[1];
            st->ylim[0] = p->ylim[0]; st->ylim[1] = p->ylim[1];
            st->initialized = true;
            st->_dragging   = false;
            st->_box_sel    = false;
        }

        bool hovered  = hui_is_hovered(p->plot_r);
        bool lmb_now  = (io->mouse_btn      & 1) != 0;
        bool lmb_prev = (io->mouse_btn_prev & 1) != 0;
        bool rmb_now  = (io->mouse_btn      & 2) != 0;
        bool rmb_prev = (io->mouse_btn_prev & 2) != 0;

        /* ---- Pan (LMB drag) ---- */
        if (hovered && lmb_now && !lmb_prev) st->_dragging = true;
        if (!lmb_now)                         st->_dragging = false;
        if (st->_dragging && lmb_now) {
            int dx = io->mouse_x - io->mouse_x_prev;
            int dy = io->mouse_y - io->mouse_y_prev;
            if (dx || dy) {
                double xr = st->xlim[1] - st->xlim[0];
                double yr = st->ylim[1] - st->ylim[0];
                st->xlim[0] -= (double)dx * xr / p->plot_r.w;
                st->xlim[1] -= (double)dx * xr / p->plot_r.w;
                st->ylim[0] += (double)dy * yr / p->plot_r.h;
                st->ylim[1] += (double)dy * yr / p->plot_r.h;
            }
        }

        /* ---- Zoom (scroll wheel, centered on mouse) ---- */
        if (hovered && io->scroll_dy != 0.0f) {
            double factor = (io->scroll_dy > 0) ? (1.0/1.15) : 1.15;
            double mx = st->xlim[0] + (double)(io->mouse_x - p->plot_r.x)
                        * (st->xlim[1] - st->xlim[0]) / p->plot_r.w;
            double my = st->ylim[1] - (double)(io->mouse_y - p->plot_r.y)
                        * (st->ylim[1] - st->ylim[0]) / p->plot_r.h;
            st->xlim[0] = mx + (st->xlim[0] - mx) * factor;
            st->xlim[1] = mx + (st->xlim[1] - mx) * factor;
            st->ylim[0] = my + (st->ylim[0] - my) * factor;
            st->ylim[1] = my + (st->ylim[1] - my) * factor;
            /* Clamp: prevent degenerate zoom-in (< 3 bars) causing visual chaos */
            if (st->xlim[1] - st->xlim[0] < 3.0) {
                double xmid = (st->xlim[0] + st->xlim[1]) * 0.5;
                st->xlim[0] = xmid - 1.5;
                st->xlim[1] = xmid + 1.5;
            }
        }

        /* ---- RMB range-select (drag = select bars, click = context menu) ---- */
        st->range_confirmed = false;
        st->rmb_clicked     = false;
        if (hovered && rmb_now && !rmb_prev) {
            st->_box_sel = true;
            st->_box_sx  = st->xlim[0] + (double)(io->mouse_x - p->plot_r.x)
                           * (st->xlim[1] - st->xlim[0]) / p->plot_r.w;
            st->_box_sy  = st->ylim[1] - (double)(io->mouse_y - p->plot_r.y)
                           * (st->ylim[1] - st->ylim[0]) / p->plot_r.h;
        }
        if (st->_box_sel && rmb_now) {
            double ex = st->xlim[0] + (double)(io->mouse_x - p->plot_r.x)
                        * (st->xlim[1] - st->xlim[0]) / p->plot_r.w;
            double rx0 = st->_box_sx < ex ? st->_box_sx : ex;
            double rx1 = st->_box_sx < ex ? ex : st->_box_sx;
            st->range_active = true;
            st->range_x0 = rx0; st->range_x1 = rx1;
            /* Draw selection highlight (x-only band, full chart height) */
            int ax = (int)((rx0 - st->xlim[0]) / (st->xlim[1]-st->xlim[0]) * p->plot_r.w) + p->plot_r.x;
            int bx = (int)((rx1 - st->xlim[0]) / (st->xlim[1]-st->xlim[0]) * p->plot_r.w) + p->plot_r.x;
            if (bx < ax) { int t = ax; ax = bx; bx = t; }
            hui_set_layer(HUI_LAYER_OVERLAY);
            hui_rect_fill(hui_rect_make(ax, p->plot_r.y, bx - ax + 1, p->plot_r.h),
                          (hui_color){CUM_ACCENT.r,CUM_ACCENT.g,CUM_ACCENT.b,35}, 0);
            hui_line(ax, p->plot_r.y, ax, p->plot_r.y + p->plot_r.h, CUM_ACCENT, 1);
            hui_line(bx, p->plot_r.y, bx, p->plot_r.y + p->plot_r.h, CUM_ACCENT, 1);
            hui_reset_layer();
        }
        if (st->_box_sel && !rmb_now && rmb_prev) {
            int dpx = io->mouse_x - (int)((st->_box_sx - st->xlim[0])
                      / (st->xlim[1]-st->xlim[0]) * p->plot_r.w + p->plot_r.x);
            if ((dpx < 0 ? -dpx : dpx) > 8) {
                st->range_confirmed = true; /* drag: confirmed selection */
            } else {
                st->rmb_clicked = true;     /* click: open context menu */
                st->range_active = false;
            }
            st->_box_sel = false;
        }
        if (!rmb_now && !st->_box_sel) {
            /* Keep range_active alive until user explicitly clears it */
        }

        p->interact_state = NULL;
    }

    p->active = false;
}

void hui_plot_state_reset(hui_plot_state *state) {
    if (state) { state->initialized = false; state->_dragging = false; state->_box_sel = false; }
}

bool hui_plot_begin_ex(const char *title, hui_rect r, hui_plot_state *state) {
    if (!hui_plot_begin(title, r)) return false;
    hui__plt_ctx *p = &hui__plt;
    p->interact_state = state;
    if (state && state->initialized) {
        p->xlim[0] = state->xlim[0]; p->xlim[1] = state->xlim[1]; p->xlim_set = true;
        p->ylim[0] = state->ylim[0]; p->ylim[1] = state->ylim[1]; p->ylim_set = true;
    }
    return true;
}

void hui_plot_setup_axes(const char *xlabel, const char *ylabel) {
    if (xlabel) { strncpy(hui__plt.xlabel, xlabel, 31); hui__plt.xlabel[31] = '\0'; }
    if (ylabel) { strncpy(hui__plt.ylabel, ylabel, 31); hui__plt.ylabel[31] = '\0'; }
}

void hui_plot_setup_limits(double xmin, double xmax, double ymin, double ymax) {
    hui__plt_ctx *p = &hui__plt;
    if (xmin < xmax) { p->xlim[0] = xmin; p->xlim[1] = xmax; p->xlim_set = true; }
    if (ymin < ymax) { p->ylim[0] = ymin; p->ylim[1] = ymax; p->ylim_set = true; }
}

void hui_plot_setup_x_fmt(hui_axis_fmt fmt) { hui__plt.xfmt = fmt; }
void hui_plot_setup_y_fmt(hui_axis_fmt fmt) { hui__plt.yfmt = fmt; }

void hui_plot_set_axis(hui_plot_yaxis axis) {
    hui__plt.next_yaxis = axis;
}

void hui_plot_setup_y2_limits(double ymin, double ymax) {
    if (ymin < ymax) {
        hui__plt.y2lim[0]  = ymin;
        hui__plt.y2lim[1]  = ymax;
        hui__plt.y2lim_set = true;
    }
}

void hui_plot_setup_axis_scale(bool is_x, hui_axis_scale scale) {
    if (is_x) {
        hui__plt.xscale = scale;
        /* Clamp lower limit > 0 for log scales */
        if (scale == HUI_AXIS_SCALE_LOG10 && hui__plt.xlim[0] <= 0.0)
            hui__plt.xlim[0] = 1e-10;
    } else {
        hui__plt.yscale = scale;
        if (scale == HUI_AXIS_SCALE_LOG10 && hui__plt.ylim[0] <= 0.0)
            hui__plt.ylim[0] = 1e-10;
    }
}

void hui_plot_setup_axis_ticks(bool is_x, const double *vals,
                               const char * const *labels, int n) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active || !vals || n <= 0) return;
    if (n > HUI_PLOT_MAX_CUSTOM_TICKS) n = HUI_PLOT_MAX_CUSTOM_TICKS;
    if (is_x) {
        p->xticks_n = n;
        for (int i = 0; i < n; i++) {
            p->xtick_vals[i] = vals[i];
            if (labels && labels[i]) {
                strncpy(p->xtick_labels[i], labels[i], 15);
                p->xtick_labels[i][15] = '\0';
            } else {
                p->xtick_labels[i][0] = '\0';
            }
        }
    } else {
        p->yticks_n = n;
        for (int i = 0; i < n; i++) {
            p->ytick_vals[i] = vals[i];
            if (labels && labels[i]) {
                strncpy(p->ytick_labels[i], labels[i], 15);
                p->ytick_labels[i][15] = '\0';
            } else {
                p->ytick_labels[i][0] = '\0';
            }
        }
    }
}

/* ---- Subplots ---- */

typedef struct {
    int      rows, cols;
    hui_rect outer;
    int      title_h;
    bool     active;
} hui__subplots_ctx;

static hui__subplots_ctx hui__subplots;

void hui_subplots_begin(const char *title, int rows, int cols, hui_rect r) {
    hui__subplots_ctx *sp = &hui__subplots;
    sp->rows    = rows > 0 ? rows : 1;
    sp->cols    = cols > 0 ? cols : 1;
    sp->outer   = r;
    sp->title_h = (title && title[0]) ? HUI__PLT_TITLE_H : 0;
    sp->active  = true;
    hui_set_layer(HUI_LAYER_BG);
    hui_rect_fill(r, CUM_BG1, 3);
    hui_reset_layer();
    if (title && title[0]) {
        int tw = (int)strlen(title) * (HUI_FONT_W + 1);
        hui_text(r.x + (r.w - tw) / 2,
                 r.y + (HUI__PLT_TITLE_H - HUI_FONT_H) / 2,
                 title, CUM_FG);
    }
}

hui_rect hui_subplots_cell(int row, int col) {
    hui__subplots_ctx *sp = &hui__subplots;
    if (!sp->active) return hui_rect_make(0, 0, 0, 0);
    int gap    = HUI_SUBPLOTS_GAP;
    int in_x   = sp->outer.x + gap;
    int in_y   = sp->outer.y + sp->title_h + gap;
    int in_w   = sp->outer.w - gap * 2;
    int in_h   = sp->outer.h - sp->title_h - gap * 2;
    int cell_w = (in_w - gap * (sp->cols - 1)) / sp->cols;
    int cell_h = (in_h - gap * (sp->rows - 1)) / sp->rows;
    return hui_rect_make(in_x + col * (cell_w + gap),
                         in_y + row * (cell_h + gap),
                         cell_w, cell_h);
}

void hui_subplots_end(void) {
    hui__subplots_ctx *sp = &hui__subplots;
    if (!sp->active) return;
    hui_rect_outline(sp->outer, CUM_BG3, 3);
    sp->active = false;
}

/* ---- Series registration ---- */

static hui__plt_series *hui__push(const char *label, hui__plt_type type,
                                   const float *xs, const float *ys, int n) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active || p->series_n >= HUI_PLOT_MAX_SERIES) return NULL;
    hui__plt_series *s = &p->series[p->series_n];
    memset(s, 0, sizeof(*s));
    s->type  = type;
    s->xs    = xs;
    s->ys    = ys;
    s->n     = n;
    s->color = hui__plt_palette[p->series_n % HUI__PLT_PAL_N];
    s->yaxis = p->next_yaxis;
    p->next_yaxis = HUI_PLOT_AXIS_Y1;  /* reset to default after each series */
    if (label) { strncpy(s->label, label, 31); s->label[31] = '\0'; }
    p->series_n++;
    return s;
}

void hui_plot_line(const char *label, const float *xs, const float *ys, int n) {
    if (!xs || !ys || n < 2) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_LINE, xs, ys, n);
    hui__plt_ctx *p = &hui__plt;
    if (s && s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_line_vals(const char *label, const float *ys, int n) {
    if (!ys || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_LINE, NULL, ys, n);
    hui__plt_ctx *p = &hui__plt;
    hui__plt_acc(0.0f,         &p->dxmin, &p->dxmax);
    hui__plt_acc((float)(n-1), &p->dxmin, &p->dxmax);
    if (s && s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        p->has_data = true;
    }
}

void hui_plot_scatter(const char *label, const float *xs, const float *ys, int n) {
    if (!xs || !ys || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_SCATTER, xs, ys, n);
    hui__plt_ctx *p = &hui__plt;
    if (s && s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_bars(const char *label, const float *xs, const float *ys, int n, float bar_w) {
    if (!xs || !ys || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_BARS, xs, ys, n);
    if (!s) return;
    s->param = bar_w > 0.0f ? bar_w : 0.8f;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        hui__plt_acc(0.0f, &p->dy2min, &p->dy2max);
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        hui__plt_acc(0.0f, &p->dymin, &p->dymax);  /* bars start from y=0 */
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_bars_h(const char *label, const float *xs, const float *ys, int n, float bar_h) {
    if (!xs || !ys || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_BARS_H, xs, ys, n);
    if (!s) return;
    s->param = bar_h > 0.0f ? bar_h : 0.8f;
    hui__plt_ctx *p = &hui__plt;
    hui__plt_acc(0.0f, &p->dxmin, &p->dxmax);  /* bars start from x=0 */
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_stems(const char *label, const float *xs, const float *ys, int n) {
    if (!xs || !ys || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_STEMS, xs, ys, n);
    if (!s) return;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        hui__plt_acc(0.0f, &p->dy2min, &p->dy2max);
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        hui__plt_acc(0.0f, &p->dymin, &p->dymax);  /* y=0 always in range */
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_error_bars(const char *label,
                         const float *xs, const float *ys, const float *err, int n) {
    if (!xs || !ys || !err || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_ERROR_BARS, xs, ys, n);
    if (!s) return;
    s->ys2 = err;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i] - err[i], &p->dy2min, &p->dy2max);
            hui__plt_acc(ys[i] + err[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i] - err[i], &p->dymin, &p->dymax);
            hui__plt_acc(ys[i] + err[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_error_bars_h(const char *label,
                            const float *xs, const float *ys, const float *err, int n) {
    if (!xs || !ys || !err || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_ERROR_BARS_H, xs, ys, n);
    if (!s) return;
    s->ys2 = err;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i] - err[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(xs[i] + err[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i] - err[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(xs[i] + err[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_bubbles(const char *label,
                      const float *xs, const float *ys, const float *sz, int n,
                      float sz_scale) {
    if (!xs || !ys || !sz || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_BUBBLES, xs, ys, n);
    if (!s) return;
    s->ys2  = sz;
    s->param = sz_scale > 0.0f ? sz_scale : 1.0f;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_candlestick(const char *label,
                          const float *dates, const float *opens,
                          const float *closes, const float *lows,
                          const float *highs, int n) {
    if (!dates || !opens || !closes || !lows || !highs || n < 1) return;
    /* xs=dates, ys=opens, ys2=closes, ys3=lows, ys4=highs */
    hui__plt_series *s = hui__push(label, HUI__PLT_CANDLE, dates, opens, n);
    if (!s) return;
    s->ys2 = closes;
    s->ys3 = lows;
    s->ys4 = highs;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(dates[i], &p->dxmin, &p->dxmax);
            hui__plt_acc(lows[i],  &p->dy2min, &p->dy2max);
            hui__plt_acc(highs[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            hui__plt_acc(dates[i],  &p->dxmin, &p->dxmax);
            hui__plt_acc(lows[i],   &p->dymin, &p->dymax);
            hui__plt_acc(highs[i],  &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_histogram(const char *label, const float *vals, int n,
                        int bins, float rmin, float rmax) {
    if (!vals || n < 1) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_HIST, vals, NULL, n);
    if (!s) return;
    s->param = (float)(bins > 0 ? bins : (int)sqrt((double)n));
    s->rmin  = rmin;
    s->rmax  = rmax;
    /* auto-range will be computed in hui__draw_hist */
}

/* ---- Ring buffer (y-only) ---- */

static float hui__plt_ring_scratch[HUI_PLOT_RING_SCRATCH];

void hui_plot_line_ring(const char *label, hui_plot_ringbuf *rb) {
    if (!rb || rb->count == 0) return;
    int n = rb->count < HUI_PLOT_RING_SCRATCH ? rb->count : HUI_PLOT_RING_SCRATCH;
    for (int i = 0; i < n; i++)
        hui__plt_ring_scratch[i] = hui_plot_ringbuf_get(rb, i);
    hui_plot_line_vals(label, hui__plt_ring_scratch, n);
}

/* ---- Ring buffer (XY) ---- */

void hui_plot_ringbuf_xy_init(hui_plot_ringbuf_xy *rb, float *buf, int cap) {
    rb->data = buf; rb->cap = cap; rb->head = 0; rb->count = 0;
}

void hui_plot_ringbuf_xy_push(hui_plot_ringbuf_xy *rb, float x, float y) {
    if (!rb || !rb->data || rb->cap <= 0) return;
    rb->data[rb->head * 2    ] = x;
    rb->data[rb->head * 2 + 1] = y;
    rb->head = (rb->head + 1) % rb->cap;
    if (rb->count < rb->cap) rb->count++;
}

/* Returns XY at logical index i (0=oldest). Result via out[0]=x, out[1]=y. */
static void hui__ringxy_get(const hui_plot_ringbuf_xy *rb, int i, float *ox, float *oy) {
    int idx = (rb->head - rb->count + i + rb->cap * 2) % rb->cap;
    *ox = rb->data[idx * 2    ];
    *oy = rb->data[idx * 2 + 1];
}

/* Scratch for XY ring: interleaved xs/ys */
static float hui__plt_ring_xs[HUI_PLOT_RING_SCRATCH];
static float hui__plt_ring_ys[HUI_PLOT_RING_SCRATCH];

void hui_plot_line_ring_xy(const char *label, hui_plot_ringbuf_xy *rb) {
    if (!rb || rb->count == 0) return;
    int n = rb->count < HUI_PLOT_RING_SCRATCH ? rb->count : HUI_PLOT_RING_SCRATCH;
    for (int i = 0; i < n; i++)
        hui__ringxy_get(rb, i, &hui__plt_ring_xs[i], &hui__plt_ring_ys[i]);
    hui_plot_line(label, hui__plt_ring_xs, hui__plt_ring_ys, n);
}

/* ---- New series types ---- */

void hui_plot_line_fn(const char *label, hui_plot_getter_fn getter, void *user_data, int n) {
    if (!getter || n < 2) return;
    int m = n < HUI_PLOT_RING_SCRATCH ? n : HUI_PLOT_RING_SCRATCH;
    for (int i = 0; i < m; i++)
        getter(i, user_data, &hui__plt_ring_xs[i], &hui__plt_ring_ys[i]);
    hui_plot_line(label, hui__plt_ring_xs, hui__plt_ring_ys, m);
}

void hui_plot_stairs(const char *label, const float *xs, const float *ys, int n) {
    if (!ys || n < 2) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_STAIRS, xs, ys, n);
    if (!s) return;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            if (xs) hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            else    hui__plt_acc((float)i, &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            if (xs) hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            else    hui__plt_acc((float)i, &p->dxmin, &p->dxmax);
            hui__plt_acc(ys[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_shaded(const char *label,
                     const float *xs, const float *ys1, const float *ys2, int n) {
    if (!ys1 || !ys2 || n < 2) return;
    hui__plt_series *s = hui__push(label, HUI__PLT_SHADED, xs, ys1, n);
    if (!s) return;
    s->ys2 = ys2;
    hui__plt_ctx *p = &hui__plt;
    if (s->yaxis == HUI_PLOT_AXIS_Y2) {
        for (int i = 0; i < n; i++) {
            if (xs) hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            else    hui__plt_acc((float)i, &p->dxmin, &p->dxmax);
            hui__plt_acc(ys1[i], &p->dy2min, &p->dy2max);
            hui__plt_acc(ys2[i], &p->dy2min, &p->dy2max);
        }
        p->has_y2data = true;
    } else {
        for (int i = 0; i < n; i++) {
            if (xs) hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
            else    hui__plt_acc((float)i, &p->dxmin, &p->dxmax);
            hui__plt_acc(ys1[i], &p->dymin, &p->dymax);
            hui__plt_acc(ys2[i], &p->dymin, &p->dymax);
        }
        p->has_data = true;
    }
}

void hui_plot_digital(const char *label, const float *xs, const float *ys, int n) {
    if (!ys || n < 2) return;
    hui__push(label, HUI__PLT_DIGITAL, xs, ys, n);
    hui__plt_ctx *p = &hui__plt;
    if (xs) for (int i = 0; i < n; i++) hui__plt_acc(xs[i], &p->dxmin, &p->dxmax);
    else { hui__plt_acc(0.0f, &p->dxmin, &p->dxmax); hui__plt_acc((float)(n-1), &p->dxmin, &p->dxmax); }
    p->has_data = true;  /* digital always Y1 (fixed band rendering) */
}

void hui_plot_heatmap(const char *label, const float *data,
                      int rows, int cols,
                      float vmin, float vmax,
                      const hui_colormap *cm) {
    if (!data || rows < 1 || cols < 1) return;
    /* Store data pointer in xs, use n=rows, param=cols, rmin/rmax=vmin/vmax */
    hui__plt_series *s = hui__push(label, HUI__PLT_HEATMAP, data, NULL, rows);
    if (!s) return;
    s->param = (float)cols;
    s->rmin  = vmin;
    s->rmax  = vmax;
    s->cmap  = cm;
    hui__plt_ctx *p = &hui__plt;
    /* Expand data bounds so auto-range covers the full heatmap area */
    hui__plt_acc(0.0f,        &p->dxmin, &p->dxmax);
    hui__plt_acc((float)cols, &p->dxmin, &p->dxmax);
    hui__plt_acc(0.0f,        &p->dymin, &p->dymax);
    hui__plt_acc((float)rows, &p->dymin, &p->dymax);
    p->has_data = true;
}

/* ---- 2D Histogram ---- */

static float hui__hist2d_buf[HUI_PLOT_HIST2D_MAX_BINS * HUI_PLOT_HIST2D_MAX_BINS];

void hui_plot_histogram2d(const char *label,
                          const float *xs, const float *ys, int n,
                          int xbins, int ybins,
                          float xmin, float xmax,
                          float ymin, float ymax,
                          const hui_colormap *cm) {
    if (!xs || !ys || n < 1) return;

    /* auto bin count */
    int auto_bins = (int)sqrt((double)n);
    if (auto_bins < 2) auto_bins = 2;
    if (xbins <= 0) xbins = auto_bins;
    if (ybins <= 0) ybins = auto_bins;
    if (xbins > HUI_PLOT_HIST2D_MAX_BINS) xbins = HUI_PLOT_HIST2D_MAX_BINS;
    if (ybins > HUI_PLOT_HIST2D_MAX_BINS) ybins = HUI_PLOT_HIST2D_MAX_BINS;

    /* auto data range */
    if (xmin >= xmax) {
        xmin = xmax = xs[0];
        for (int i = 1; i < n; i++) {
            if (xs[i] < xmin) xmin = xs[i];
            if (xs[i] > xmax) xmax = xs[i];
        }
        if (xmin >= xmax) xmax = xmin + 1.0f;
    }
    if (ymin >= ymax) {
        ymin = ymax = ys[0];
        for (int i = 1; i < n; i++) {
            if (ys[i] < ymin) ymin = ys[i];
            if (ys[i] > ymax) ymax = ys[i];
        }
        if (ymin >= ymax) ymax = ymin + 1.0f;
    }

    /* zero the grid */
    int cells = xbins * ybins;
    for (int i = 0; i < cells; i++) hui__hist2d_buf[i] = 0.0f;

    /* bin */
    float xbw = (xmax - xmin) / (float)xbins;
    float ybw = (ymax - ymin) / (float)ybins;
    for (int i = 0; i < n; i++) {
        int bx = (int)((xs[i] - xmin) / xbw);
        int by = (int)((ys[i] - ymin) / ybw);
        if (bx < 0) { bx = 0; } else if (bx >= xbins) { bx = xbins - 1; }
        if (by < 0) { by = 0; } else if (by >= ybins) { by = ybins - 1; }
        /* row = ybins-1-by so y=ymax is top of heatmap */
        hui__hist2d_buf[(ybins - 1 - by) * xbins + bx] += 1.0f;
    }

    /* find max for normalization */
    float cmax = 1.0f;
    for (int i = 0; i < cells; i++)
        if (hui__hist2d_buf[i] > cmax) cmax = hui__hist2d_buf[i];

    /* normalize to [0,1] */
    for (int i = 0; i < cells; i++) hui__hist2d_buf[i] /= cmax;

    /* set axis limits to data range before heatmap (which sets 0..cols, 0..rows) */
    hui__plt_ctx *p = &hui__plt;
    if (!p->xlim_set) { p->xlim[0] = xmin; p->xlim[1] = xmax; p->xlim_set = true; }
    if (!p->ylim_set) { p->ylim[0] = ymin; p->ylim[1] = ymax; p->ylim_set = true; }

    hui_plot_heatmap(label, hui__hist2d_buf, ybins, xbins, 0.0f, 1.0f, cm);

    /* fix up heatmap's axis expansion back to data coords */
    p->dxmin = xmin; p->dxmax = xmax;
    p->dymin = ymin; p->dymax = ymax;
}

/* ---- Annotations / tags ---- */

void hui_plot_annotation(double x, double y, const char *text, hui_color c) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active || p->annots_n >= HUI__PLT_MAX_ANNOTS) return;
    hui__plt_annot *a = &p->annots[p->annots_n++];
    a->x = x; a->y = y; a->c = c;
    strncpy(a->text, text ? text : "", 31); a->text[31] = '\0';
}

void hui_plot_tag_x(double x, const char *text, hui_color c) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active || p->tags_n >= HUI__PLT_MAX_TAGS) return;
    hui__plt_tag *t = &p->tags[p->tags_n++];
    t->v = x; t->is_x = true; t->c = c;
    strncpy(t->text, text ? text : "", 15); t->text[15] = '\0';
}

void hui_plot_tag_y(double y, const char *text, hui_color c) {
    hui__plt_ctx *p = &hui__plt;
    if (!p->active || p->tags_n >= HUI__PLT_MAX_TAGS) return;
    hui__plt_tag *t = &p->tags[p->tags_n++];
    t->v = y; t->is_x = false; t->c = c;
    strncpy(t->text, text ? text : "", 15); t->text[15] = '\0';
}

/* ---- Coordinate queries ---- */

bool hui_plot_is_hovered(void) {
    if (!hui__plt.active || !hui_g) return false;
    return hui_rect_contains(hui__plt.plot_r, hui_g->io.mouse_x, hui_g->io.mouse_y);
}

void hui_plot_mouse_pos(double *x, double *y) {
    hui__plt_ctx *p = &hui__plt;
    if (!hui_g) { if(x)*x=0; if(y)*y=0; return; }
    double rx = p->xlim[1] - p->xlim[0];
    double ry = p->ylim[1] - p->ylim[0];
    if (x) *x = (rx == 0.0) ? p->xlim[0] :
        p->xlim[0] + (hui_g->io.mouse_x - p->plot_r.x) / (double)p->plot_r.w * rx;
    if (y) *y = (ry == 0.0) ? p->ylim[0] :
        p->ylim[1] - (hui_g->io.mouse_y - p->plot_r.y) / (double)p->plot_r.h * ry;
}

void hui_plot_to_pixels(double x, double y, int *px, int *py) {
    if (px) *px = hui__px_x(x);
    if (py) *py = hui__px_y(y);
}

void hui_plot_pixels_to(int px, int py, double *x, double *y) {
    hui__plt_ctx *p = &hui__plt;
    double rx = p->xlim[1] - p->xlim[0];
    double ry = p->ylim[1] - p->ylim[0];
    if (x) *x = (rx == 0.0) ? p->xlim[0] :
        p->xlim[0] + (px - p->plot_r.x) / (double)p->plot_r.w * rx;
    if (y) *y = (ry == 0.0) ? p->ylim[0] :
        p->ylim[1] - (py - p->plot_r.y) / (double)p->plot_r.h * ry;
}

/* ---- Drag tool internals ---- */

static int  hui__plt_drag_id     = 0;
static bool hui__plt_drag_active = false;

/* pixel delta → data-x delta */
static double hui__ddx(int dpx) {
    hui__plt_ctx *p = &hui__plt;
    if (p->plot_r.w == 0) return 0.0;
    return (double)dpx / p->plot_r.w * (p->xlim[1] - p->xlim[0]);
}
/* pixel delta → data-y delta (screen-Y flipped) */
static double hui__ddy(int dpy) {
    hui__plt_ctx *p = &hui__plt;
    if (p->plot_r.h <= 1) return 0.0;
    return -(double)dpy / (p->plot_r.h - 1) * (p->ylim[1] - p->ylim[0]);
}

/* Try to start/maintain/stop drag on sub_id with given hit rect.
 * Returns true while this sub_id is the active drag. */
static bool hui__plt_drag_try(int sub_id, hui_rect hit_r) {
    if (!hui_g) return false;
    bool lmb     = (hui_g->io.mouse_btn      & 1) != 0;
    bool lmb_was = (hui_g->io.mouse_btn_prev & 1) != 0;
    /* start drag only if no other drag is active */
    if (!hui__plt_drag_active && lmb && !lmb_was && hui_is_hovered(hit_r)) {
        hui__plt_drag_id     = sub_id;
        hui__plt_drag_active = true;
    }
    /* release */
    if (!lmb && hui__plt_drag_id == sub_id)
        hui__plt_drag_active = false;
    return hui__plt_drag_active && (hui__plt_drag_id == sub_id);
}

bool hui_plot_drag_point(int id, double *x, double *y, hui_color c, int radius) {
    if (!hui_g || !x || !y || !hui__plt.active) return false;
    int r   = (radius > 0) ? radius : 5;
    int tol = r + 4;
    int px  = hui__px_x(*x), py = hui__px_y(*y);
    hui_rect hit = hui_rect_make(px - tol, py - tol, tol * 2, tol * 2);

    bool active = hui__plt_drag_try(id, hit);
    if (active) {
        *x += hui__ddx(hui_g->io.mouse_x - hui_g->io.mouse_x_prev);
        *y += hui__ddy(hui_g->io.mouse_y - hui_g->io.mouse_y_prev);
        px = hui__px_x(*x); py = hui__px_y(*y);
    }

    bool hov = hui_is_hovered(hit);
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_color fill = active ? CUM_ACTIVE : (hov ? CUM_ACCENT : c);
    hui_circle_fill(px, py, r, fill);
    hui_circle(px, py, r, CUM_FG);
    hui_reset_layer();
    return active;
}

bool hui_plot_drag_line_x(int id, double *x, hui_color c) {
    if (!hui_g || !x || !hui__plt.active) return false;
    hui_rect pr = hui__plt.plot_r;
    int px = hui__px_x(*x);
    hui_rect hit = hui_rect_make(px - 4, pr.y, 8, pr.h);

    bool active = hui__plt_drag_try(id, hit);
    if (active)
        *x += hui__ddx(hui_g->io.mouse_x - hui_g->io.mouse_x_prev);

    bool hov = hui_is_hovered(hit);
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_color lc = active ? CUM_ACTIVE : (hov ? CUM_ACCENT : c);
    hui_line(px, pr.y, px, pr.y + pr.h, lc, active ? 2 : 1);
    /* drag handle at vertical midpoint */
    int mid_y = pr.y + pr.h / 2;
    hui_circle_fill(px, mid_y, 4, lc);
    hui_circle(px, mid_y, 4, CUM_FG);
    hui_reset_layer();
    return active;
}

bool hui_plot_drag_line_y(int id, double *y, hui_color c) {
    if (!hui_g || !y || !hui__plt.active) return false;
    hui_rect pr = hui__plt.plot_r;
    int py = hui__px_y(*y);
    hui_rect hit = hui_rect_make(pr.x, py - 4, pr.w, 8);

    bool active = hui__plt_drag_try(id, hit);
    if (active)
        *y += hui__ddy(hui_g->io.mouse_y - hui_g->io.mouse_y_prev);

    bool hov = hui_is_hovered(hit);
    hui_set_layer(HUI_LAYER_OVERLAY);
    hui_color lc = active ? CUM_ACTIVE : (hov ? CUM_ACCENT : c);
    hui_line(pr.x, py, pr.x + pr.w, py, lc, active ? 2 : 1);
    /* drag handle at horizontal midpoint */
    int mid_x = pr.x + pr.w / 2;
    hui_circle_fill(mid_x, py, 4, lc);
    hui_circle(mid_x, py, 4, CUM_FG);
    hui_reset_layer();
    return active;
}

bool hui_plot_drag_rect(int id, double *x1, double *y1, double *x2, double *y2, hui_color c) {
    if (!hui_g || !x1 || !y1 || !x2 || !y2 || !hui__plt.active) return false;

    /* normalize so x1<x2, y1<y2 */
    if (*x1 > *x2) { double t = *x1; *x1 = *x2; *x2 = t; }
    if (*y1 > *y2) { double t = *y1; *y1 = *y2; *y2 = t; }

    /* screen coords: y2 maps to top (smaller screen-y), y1 to bottom */
    int px1 = hui__px_x(*x1), py_top = hui__px_y(*y2);
    int px2 = hui__px_x(*x2), py_bot = hui__px_y(*y1);
    int rw = px2 - px1, rh = py_bot - py_top;

    /* sub-ids: id*5+{0=center, 1=TL, 2=TR, 3=BR, 4=BL} */
    int cr = 5; /* corner handle radius */
    int cx = (px1 + px2) / 2, cy = (py_top + py_bot) / 2;

    hui_rect hit_c  = hui_rect_make(cx - cr - 2, cy - cr - 2, (cr+2)*2, (cr+2)*2);
    hui_rect hit_tl = hui_rect_make(px1 - cr, py_top - cr, cr*2, cr*2);
    hui_rect hit_tr = hui_rect_make(px2 - cr, py_top - cr, cr*2, cr*2);
    hui_rect hit_br = hui_rect_make(px2 - cr, py_bot - cr, cr*2, cr*2);
    hui_rect hit_bl = hui_rect_make(px1 - cr, py_bot - cr, cr*2, cr*2);

    bool a_c  = hui__plt_drag_try(id*5+0, hit_c);
    bool a_tl = hui__plt_drag_try(id*5+1, hit_tl);
    bool a_tr = hui__plt_drag_try(id*5+2, hit_tr);
    bool a_br = hui__plt_drag_try(id*5+3, hit_br);
    bool a_bl = hui__plt_drag_try(id*5+4, hit_bl);

    double ddx = hui__ddx(hui_g->io.mouse_x - hui_g->io.mouse_x_prev);
    double ddy = hui__ddy(hui_g->io.mouse_y - hui_g->io.mouse_y_prev);

    if (a_c)  { *x1 += ddx; *x2 += ddx; *y1 += ddy; *y2 += ddy; }
    if (a_tl) { *x1 += ddx; *y2 += ddy; }  /* TL: left+top */
    if (a_tr) { *x2 += ddx; *y2 += ddy; }  /* TR: right+top */
    if (a_br) { *x2 += ddx; *y1 += ddy; }  /* BR: right+bottom */
    if (a_bl) { *x1 += ddx; *y1 += ddy; }  /* BL: left+bottom */

    bool any = a_c || a_tl || a_tr || a_br || a_bl;

    /* recompute pixels after drag */
    if (any) {
        if (*x1 > *x2) { double t = *x1; *x1 = *x2; *x2 = t; }
        if (*y1 > *y2) { double t = *y1; *y1 = *y2; *y2 = t; }
        px1    = hui__px_x(*x1); py_top = hui__px_y(*y2);
        px2    = hui__px_x(*x2); py_bot = hui__px_y(*y1);
        rw = px2 - px1; rh = py_bot - py_top;
        cx = (px1 + px2) / 2;   cy = (py_top + py_bot) / 2;
    }

    hui_set_layer(HUI_LAYER_OVERLAY);
    if (rw > 0 && rh > 0) {
        hui_color fc = HUI_RGBA(c.r, c.g, c.b, 45);
        hui_rect_fill(hui_rect_make(px1, py_top, rw, rh), fc, 0);
    }
    hui_rect_outline(hui_rect_make(px1, py_top, rw, rh), any ? CUM_ACTIVE : c, 0);

    /* center (move) handle */
    hui_color cc = a_c ? CUM_ACTIVE : (hui_is_hovered(hit_c) ? CUM_ACCENT : c);
    hui_circle_fill(cx, cy, cr, cc);
    hui_circle(cx, cy, cr, CUM_FG);

    /* corner handles */
    int corner_px[4] = { px1, px2, px2, px1 };
    int corner_py[4] = { py_top, py_top, py_bot, py_bot };
    bool corner_a[4] = { a_tl, a_tr, a_br, a_bl };
    hui_rect corner_hit[4] = { hit_tl, hit_tr, hit_br, hit_bl };
    for (int i = 0; i < 4; i++) {
        hui_color ch = corner_a[i] ? CUM_ACTIVE
                     : (hui_is_hovered(corner_hit[i]) ? CUM_ACCENT : c);
        hui_circle_fill(corner_px[i], corner_py[i], cr, ch);
        hui_circle(corner_px[i], corner_py[i], cr, CUM_FG);
    }
    hui_reset_layer();
    return any;
}

/* ---- Colormap ---- */

hui_color hui_colormap_at(const hui_colormap *cm, float t) {
    if (!cm || cm->n == 0) return HUI_RGBA(0,0,0,255);
    if (t <= cm->t[0])      return cm->c[0];
    if (t >= cm->t[cm->n-1]) return cm->c[cm->n-1];
    for (int i = 0; i < cm->n - 1; i++) {
        if (t <= cm->t[i+1]) {
            float u = (t - cm->t[i]) / (cm->t[i+1] - cm->t[i]);
            return hui_color_lerp(cm->c[i], cm->c[i+1], u);
        }
    }
    return cm->c[cm->n-1];
}

const hui_colormap HUI_CMAP_VIRIDIS = {
    { 0.0f, 0.143f, 0.286f, 0.429f, 0.571f, 0.714f, 0.857f, 1.0f },
    { {68,1,84,255}, {70,50,127,255}, {54,92,141,255},
      {39,127,142,255}, {31,161,135,255}, {74,193,109,255},
      {159,218,58,255}, {253,231,37,255} },
    8
};

const hui_colormap HUI_CMAP_PLASMA = {
    { 0.0f, 0.143f, 0.286f, 0.429f, 0.571f, 0.714f, 0.857f, 1.0f },
    { {13,8,135,255}, {84,2,163,255}, {139,10,165,255},
      {185,50,137,255}, {219,92,104,255}, {244,136,73,255},
      {254,188,43,255}, {240,249,33,255} },
    8
};

const hui_colormap HUI_CMAP_HOT = {
    { 0.0f, 0.33f, 0.66f, 1.0f },
    { {0,0,0,255}, {255,0,0,255}, {255,165,0,255}, {255,255,255,255} },
    4
};

/* ---- Stride ---- */

void hui_plot_line_stride(const char *label,
                          const float *xs, const float *ys,
                          int n, int stride_bytes) {
    if (!xs || !ys || n < 2 || stride_bytes <= 0) return;
    /* Copy into scratch buffers — avoids keeping pointers to internal array layout */
    int m = n < HUI_PLOT_RING_SCRATCH ? n : HUI_PLOT_RING_SCRATCH;
    for (int i = 0; i < m; i++) {
        hui__plt_ring_xs[i] = *(const float *)((const char *)xs + i * stride_bytes);
        hui__plt_ring_ys[i] = *(const float *)((const char *)ys + i * stride_bytes);
    }
    hui_plot_line(label, hui__plt_ring_xs, hui__plt_ring_ys, m);
}

/* ---- Waterfall buffer ---- */

void hui_waterfall_buf_init(hui_waterfall_buf *wb, float *data, int bins, int cap) {
    wb->data  = data;
    wb->bins  = bins;
    wb->cap   = cap;
    wb->head  = 0;
    wb->count = 0;
}

void hui_waterfall_buf_push(hui_waterfall_buf *wb, const float *frame) {
    if (!wb || !wb->data || wb->bins <= 0 || wb->cap <= 0 || !frame) return;
    memcpy(wb->data + wb->head * wb->bins, frame, sizeof(float) * (size_t)wb->bins);
    wb->head = (wb->head + 1) % wb->cap;
    if (wb->count < wb->cap) wb->count++;
}

/* ---- Audio convenience wrappers ---- */

void hui_plot_spectrum(const char *label, const float *mag, int n_bins,
                       float sample_rate_hz) {
    if (!mag || n_bins < 2 || sample_rate_hz <= 0.0f) return;
    int m = n_bins < HUI_PLOT_RING_SCRATCH ? n_bins : HUI_PLOT_RING_SCRATCH;
    float nyquist = sample_rate_hz * 0.5f;
    for (int i = 0; i < m; i++)
        hui__plt_ring_xs[i] = (float)i * nyquist / (float)(m - 1);
    /* Set only x limits (ymin==ymax → condition fails → y auto-ranges) */
    hui_plot_setup_limits(0.0, (double)nyquist, 0.0, 0.0);
    hui_plot_setup_x_fmt(HUI_AXIS_FMT_FREQ_HZ);
    hui_plot_line(label, hui__plt_ring_xs, mag, m);
}

void hui_plot_waveform(const char *label, const float *samples, int n,
                       float sample_rate_hz) {
    if (!samples || n < 2 || sample_rate_hz <= 0.0f) return;
    int m = n < HUI_PLOT_RING_SCRATCH ? n : HUI_PLOT_RING_SCRATCH;
    float dt = 1.0f / sample_rate_hz;
    for (int i = 0; i < m; i++)
        hui__plt_ring_xs[i] = (float)i * dt;
    /* Set only x limits */
    hui_plot_setup_limits(0.0, (double)m * dt, 0.0, 0.0);
    hui_plot_setup_x_fmt(HUI_AXIS_FMT_TIME_S);
    hui_plot_line(label, hui__plt_ring_xs, samples, m);
}

/* ---- Standalone waterfall / spectrogram ---- */

void hui_plot_waterfall(const char *title, hui_rect r,
                        const hui_waterfall_buf *wb,
                        const hui_colormap *cm,
                        float vmin, float vmax,
                        float sample_rate_hz, float frame_period_s) {
    if (!wb || wb->count == 0 || r.w < 80 || r.h < 60) return;

    /* Outer background + border */
    hui_set_layer(HUI_LAYER_BG);
    hui_rect_fill(r, CUM_BG1, 3);
    hui_reset_layer();
    if (title && title[0]) {
        int tw = (int)strlen(title) * (HUI_FONT_W + 1);
        hui_text(r.x + (r.w - tw) / 2,
                 r.y + (HUI__PLT_TITLE_H - HUI_FONT_H) / 2,
                 title, CUM_FG);
    }

    /* Plot area */
    hui_rect pr = hui_rect_make(r.x + HUI__PLT_YAXIS_W,
                                r.y + HUI__PLT_TITLE_H,
                                r.w - HUI__PLT_YAXIS_W - HUI__PLT_MARGIN_R,
                                r.h - HUI__PLT_TITLE_H - HUI__PLT_XAXIS_H - HUI__PLT_XLABEL_H);
    if (pr.w < 10 || pr.h < 10) return;

    hui_set_layer(HUI_LAYER_BG);
    hui_rect_fill(pr, CUM_BG0, 0);
    hui_reset_layer();

    /* Render heatmap cells — newest frame at top (row 0 = newest) */
    int count = wb->count;
    int bins  = wb->bins;
    float vrange = vmax > vmin ? vmax - vmin : 1.0f;
    const hui_colormap *pcm = cm ? cm : &HUI_CMAP_VIRIDIS;

    hui_clip_push(pr);
    for (int row = 0; row < count; row++) {
        /* map display row → oldest-first logical index */
        int li = count - 1 - row;   /* li=0 = newest; li=count-1 = oldest */
        int fi = (wb->head - wb->count + li + wb->cap * 2) % wb->cap;
        const float *frame = wb->data + fi * bins;

        int py0 = pr.y + row     * pr.h / count;
        int py1 = pr.y + (row+1) * pr.h / count;
        if (py1 <= py0) py1 = py0 + 1;

        for (int col = 0; col < bins; col++) {
            float v = frame[col];
            float t = (v - vmin) / vrange;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            hui_color c = hui_colormap_at(pcm, t);
            int px0 = pr.x + col     * pr.w / bins;
            int px1 = pr.x + (col+1) * pr.w / bins;
            if (px1 <= px0) px1 = px0 + 1;
            hui_rect_fill(hui_rect_make(px0, py0, px1-px0, py1-py0), c, 0);
        }
    }
    hui_clip_pop();

    /* X-axis ticks (frequency) */
    if (sample_rate_hz > 0.0f) {
        double xmax = (double)(sample_rate_hz * 0.5f);
        double xstep = hui__nice_step(xmax, HUI__PLT_NTICKS);
        for (double xv = 0.0; xv <= xmax + xstep * 1e-4; xv += xstep) {
            int px = pr.x + (int)(xv / xmax * pr.w);
            if (px < pr.x || px > pr.x + pr.w) continue;
            hui_line(px, pr.y, px, pr.y + pr.h, CUM_BG3, 1);
            hui_line(px, pr.y + pr.h, px, pr.y + pr.h + 4, CUM_FG3, 1);
            const char *lbl = hui__fmt_tick(xv, HUI_AXIS_FMT_FREQ_HZ);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(px - lw / 2, pr.y + pr.h + 6, lbl, CUM_FG2);
        }
        /* X label */
        {
            const char *xl = "Frequency";
            int lw = (int)strlen(xl) * (HUI_FONT_W + 1);
            hui_text(pr.x + (pr.w - lw) / 2,
                     pr.y + pr.h + HUI__PLT_XAXIS_H, xl, CUM_FG2);
        }
    }

    /* Y-axis ticks (time — 0 = newest/top, increases downward) */
    if (frame_period_s > 0.0f && count > 0) {
        double total_time = (double)count * frame_period_s;
        double ystep = hui__nice_step(total_time, HUI__PLT_NTICKS);
        for (double yv = 0.0; yv <= total_time + ystep * 1e-4; yv += ystep) {
            int py = pr.y + (int)(yv / total_time * pr.h);
            if (py < pr.y || py > pr.y + pr.h) continue;
            hui_line(pr.x, py, pr.x + pr.w, py, CUM_BG3, 1);
            hui_line(pr.x - 4, py, pr.x, py, CUM_FG3, 1);
            const char *lbl = hui__fmt_tick(yv, HUI_AXIS_FMT_TIME_S);
            int lw = (int)strlen(lbl) * (HUI_FONT_W + 1);
            hui_text(pr.x - lw - 6, py - HUI_FONT_H / 2, lbl, CUM_FG2);
        }
    }

    /* Axis border lines */
    hui_line(pr.x,        pr.y,        pr.x,        pr.y + pr.h, CUM_FG3, 1);
    hui_line(pr.x,        pr.y + pr.h, pr.x + pr.w, pr.y + pr.h, CUM_FG3, 1);
    hui_line(pr.x + pr.w, pr.y,        pr.x + pr.w, pr.y + pr.h, CUM_BG3, 1);
    hui_line(pr.x,        pr.y,        pr.x + pr.w, pr.y,        CUM_BG3, 1);
    hui_rect_outline(r, CUM_BG3, 3);
}

/* ---- Pie / Donut chart ---- */

void hui_plot_pie(const char *title, hui_rect r,
                  const float *vals, int n, const char **labels,
                  const hui_colormap *cm, float inner_r) {
    if (!vals || n <= 0 || !hui_g) return;

    /* Title */
    if (title && title[0]) {
        hui_text(r.x + r.w/2 - (int)(strlen(title) * (HUI_FONT_W+1)) / 2,
                 r.y, title, CUM_FG);
        r.y += HUI__PLT_TITLE_H;
        r.h -= HUI__PLT_TITLE_H;
    }

    /* Sum values */
    float total = 0.0f;
    for (int i = 0; i < n; i++) if (vals[i] > 0.0f) total += vals[i];
    if (total <= 0.0f) return;

    /* Center and outer radius */
    float cx = r.x + r.w * 0.5f;
    float cy = r.y + r.h * 0.5f;
    float radius = (float)((r.w < r.h ? r.w : r.h) / 2) - 2.0f;
    float ir = inner_r > 0.0f ? inner_r * radius : 0.0f;
    if (inner_r < 0.0f) inner_r = 0.0f;
    if (inner_r >= 1.0f) inner_r = 0.95f;

    /* Fan-triangulate each slice */
    float angle = -HUI_PI * 0.5f; /* start at top */
    const int steps_per_circle = 64;
    for (int i = 0; i < n; i++) {
        if (vals[i] <= 0.0f) continue;
        float sweep = (vals[i] / total) * HUI_TAU;
        float a_end = angle + sweep;

        /* Pick color */
        hui_color slice_col;
        if (cm) {
            slice_col = hui_colormap_at(cm, (float)i / (float)(n > 1 ? n-1 : 1));
        } else {
            slice_col = hui__plt_palette[i % HUI__PLT_PAL_N];
        }
        /* Slightly dimmed fill, brighter for hover */
        hui_color fill_col = slice_col;
        float a_mid = angle + sweep * 0.5f;

        /* Number of triangle fan steps proportional to sweep */
        int steps = (int)(sweep / HUI_TAU * (float)steps_per_circle);
        if (steps < 2) steps = 2;

        for (int s = 0; s < steps; s++) {
            float t0 = angle + sweep * (float)s       / (float)steps;
            float t1 = angle + sweep * (float)(s + 1) / (float)steps;
            float c0s = cosf(t0), s0s = sinf(t0);
            float c1s = cosf(t1), s1s = sinf(t1);

            if (ir > 0.0f) {
                /* Donut: draw quad as two triangles */
                hui_v2i pi0 = {(int16_t)(cx + ir   * c0s), (int16_t)(cy + ir   * s0s)};
                hui_v2i po0 = {(int16_t)(cx + radius*c0s), (int16_t)(cy + radius*s0s)};
                hui_v2i pi1 = {(int16_t)(cx + ir   * c1s), (int16_t)(cy + ir   * s1s)};
                hui_v2i po1 = {(int16_t)(cx + radius*c1s), (int16_t)(cy + radius*s1s)};
                hui_triangle_fill(pi0, po0, po1, fill_col);
                hui_triangle_fill(pi0, po1, pi1, fill_col);
            } else {
                /* Solid: center fan */
                hui_v2i center = {(int16_t)cx, (int16_t)cy};
                hui_v2i p0     = {(int16_t)(cx + radius*c0s), (int16_t)(cy + radius*s0s)};
                hui_v2i p1     = {(int16_t)(cx + radius*c1s), (int16_t)(cy + radius*s1s)};
                hui_triangle_fill(center, p0, p1, fill_col);
            }
        }

        /* Slice border line (rim) */
        float c_start = cosf(angle), s_start = sinf(angle);
        float c_end   = cosf(a_end),  s_end   = sinf(a_end);
        hui_line((int)(cx + (ir>0?ir:0)*c_start), (int)(cy + (ir>0?ir:0)*s_start),
                 (int)(cx + radius*c_start), (int)(cy + radius*s_start),
                 CUM_BG0, 1);
        (void)c_end; (void)s_end;

        /* Label at slice midpoint */
        if (labels && labels[i] && labels[i][0]) {
            float lx = cx + (radius * 0.65f + ir * 0.35f) * cosf(a_mid);
            float ly = cy + (radius * 0.65f + ir * 0.35f) * sinf(a_mid);
            int lw = (int)(strlen(labels[i]) * (HUI_FONT_W + 1));
            hui_text((int)lx - lw/2, (int)ly - HUI_FONT_H/2, labels[i], CUM_BG0);
        }

        angle = a_end;
    }

    /* Outer ring */
    hui_circle(r.x + r.w/2, r.y + r.h/2, (int)radius, CUM_BG3);
    if (ir > 0.0f)
        hui_circle_fill(r.x + r.w/2, r.y + r.h/2, (int)ir, CUM_BG2);
}

#endif /* HUI_PLOT_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_PLOT_H */
