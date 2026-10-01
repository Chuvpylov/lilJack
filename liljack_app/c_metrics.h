#ifndef LJ_METRICS_H
#define LJ_METRICS_H
#include <stdint.h>
/* c_metrics.h — in-process CPU/MEM/GPU sampler, ~25ms cadence, /proc + NVML.
 *
 * Owner: deepseek (2026-09-10, claude msg liljack-tui-v3). the operator wants live
 * CPU/MEM/GPU tiles with load plots at "20ms max refresh, 60fps, without too
 * much CPU loading and interceptions in cpu exec".
 *
 * ⚠ THE ARCHITECTURE DECISION, AND IT IS THE WHOLE POINT: this does NOT go
 * through the Python backend. One `metrics` action at 50 Hz would run
 * backend.snapshot() fifty times a second — tmux subprocesses and sqlite —
 * which is exactly the "interception in cpu exec" the operator is telling us to
 * avoid. Everything here samples in-process, in C, straight from /proc.
 *
 * Values are fractions 0..1. A field that is UNAVAILABLE is NAN, never 0.0: a
 * missing source rendered as a real number has bitten this repo repeatedly
 * (the GPU reads as an idle card, the CPU reads as 0% before the first delta).
 *
 * NO ALLOCATION AND NO SUBPROCESS IN THE SAMPLE PATH. /proc/stat and
 * /proc/meminfo are opened ONCE at init and pread(+rewind) each poll; NVML is
 * dlopen'd once at init. The ring buffers are fixed-size arrays.
 *
 * Diagnostic overrides (for the planted-fixture tests; read once at init):
 *   LJ_METRICS_PROC        directory in place of /proc (default "/proc")
 *   LJ_METRICS_NVML        path to libnvidia-ml.so.1 (default "libnvidia-ml.so.1")
 *   LJ_METRICS_INTERVAL_MS poll rate-limit period in ms (default 20; 0 = never limit)
 */

/* 0..1 each; NAN = unavailable (for CPU also: not-yet-valid before two reads). */
typedef struct {
    float cpu;      /* total CPU busy fraction since the previous sample */
    float mem;      /* memory used fraction (1 - MemAvailable/MemTotal) */
    float gpu;      /* GPU utilisation 0..1, NAN when NVML is unavailable */
    float gpu_mem;  /* GPU memory used fraction, NAN when NVML is unavailable */
} lj_sample;

enum {
    LJ_METRIC_CPU = 0,
    LJ_METRIC_MEM,
    LJ_METRIC_GPU,
    LJ_METRIC_GPU_MEM,
    LJ_METRIC_COUNT,
    LJ_METRIC_HISTORY = 256
};

/* Default LOAD-plot smoothing window, in milliseconds. The sampler rate-limits
 * to ~25ms (LJ_METRICS_REF_INTERVAL_MS), so this is 4 samples at the default
 * cadence; it is capped at LJ_METRIC_HISTORY. Override with LJ_METRICS_SMOOTH_MS
 * (0 or 1 disables). the operator 2026-09-12: the header graph is one pixel column per
 * 25ms bucket, so the window is kept short or the columns would all agree. */
#define LJ_METRICS_SMOOTH_MS 100
#define LJ_METRICS_REF_INTERVAL_MS 25   /* smoothing reference only; the default POLL interval follows the display bucket */

/* Display-only completed means; acquisition stays at its existing cadence.
 * Each history element is one 25ms bucket of smoothed acquired samples: the
 * header graph paints ONE PIXEL COLUMN per bucket (the operator 2026-09-12), so an
 * 80px spark is 2s of history and LJ_METRIC_HISTORY (256) covers 6.4s. */
#define LJ_METRICS_DISPLAY_MS 50      /* DEFAULT plot column width in ms (the operator 2026-09-13: "1 px over 50 ms, or 100 ms, configurable");
                                       * runtime value: lj_metrics_display_ms(), env LJ_METRICS_DISPLAY_MS (25..1000) */
#define LJ_METRICS_DISPLAY_OUTAGE_MS 1000   /* buckets beyond this gap are NaN (a real sampler outage) */
/* The header/LOAD value text is held this long between refreshes so the number
 * does not flicker at the 25ms column rate; fixed-width "%3.0f%%". */
#define LJ_METRICS_LABEL_MS 500
/* Header graph REPAINT pacing (the operator 2026-09-12: 1px/25ms columns read too
 * fast). Resolution stays 25ms per pixel; the visible plot advances by
 * REPAINT/25 columns every REPAINT ms (default 100ms = 4 columns at 10Hz).
 * Env LJ_METRICS_DISPLAY_REPAINT_MS overrides (25 = every bucket, 50 = 2). */
#define LJ_METRICS_DISPLAY_REPAINT_MS 100
int lj_metrics_paced_history(int which,const float **out);   /* snapshot refreshed every REPAINT ms */

/* ── stacked components (the operator 2026-09-13: "height of a column is load, colour
 * is a core — same for GPU and memory: swap, RAM") ─────────────────────────
 * Next to each aggregate ring the sampler keeps PART rings, closed by the very
 * same display-bucket rule (every bucket a sample closes carries the value;
 * NaN only on an outage or an unavailable source) and paced together with the
 * aggregate snapshot, so a stacked column always adds up to the aggregate:
 *   CPU  part c (0..core_count-1) = that core's busy fraction / core_count,
 *        from the "cpuN" lines of /proc/stat (same delta rule as "cpu "), so
 *        the stack is exactly the machine load. Cores are capped at 64.
 *   MEM  0 = used   (1 - MemAvailable/MemTotal) - cached, clamped at 0
 *        1 = cached (Cached + Buffers) / MemTotal
 *        2 = swap   (SwapTotal - SwapFree) / MemTotal
 *        used + cached == the aggregate MEM value; swap sits on top.
 *   GPU  0 = compute utilisation (nvml utilization.gpu), 1 = memory used.
 * L1/L2 cache activity is NOT a part: it is not observable without perf
 * counters, which this in-process /proc sampler deliberately does not touch.
 * Fake-/proc test contract (LJ_METRICS_PROC): stat must carry "cpu " plus
 * "cpu0".."cpuN"; meminfo MemTotal, MemAvailable, Cached, Buffers, SwapTotal,
 * SwapFree — a missing key is 0 for THAT component, never NaN for the plot. */
#define LJ_METRIC_CORES_MAX 64
int lj_metrics_core_count(void);                                  /* cpuN lines seen, 0 before the first poll */
int lj_metrics_part_count(int which);                             /* CPU: core count; MEM 3; GPU 2; else 0 */
int lj_metrics_paced_core_history(int core,const float **out);    /* == paced_part_history(LJ_METRIC_CPU,core) */
int lj_metrics_paced_part_history(int which,int part,const float **out);   /* same count/pacing as lj_metrics_paced_history */
uint64_t lj_metrics_paced_revision(void);                       /* bumps when the snapshot refreshes */
int lj_metrics_repaint_ms(void);
uint64_t lj_metrics_label_ms(void);
int lj_metrics_display_history(int which,const float **out);
int lj_metrics_display_ms(void);        /* the live bucket width (ms) — one plot column */
/* Live setters (theme editor): clamped to 25..1000 / display..2000; return the stored value.
 * The display ring keeps its columns; new buckets close at the new width. */
int lj_metrics_set_display_ms(int ms);
int lj_metrics_set_interval_ms(int ms);
int lj_metrics_set_label_hold_ms(int ms);
int lj_metrics_interval_ms(void);
int lj_metrics_label_hold_ms(void);
int lj_metrics_set_repaint_ms(int ms);
const char *lj_metrics_display_label(int which);
uint64_t lj_metrics_display_revision(void);

int  lj_metrics_init(void);      /* 0 ok; negative only if /proc is unreadable */
void lj_metrics_poll(void);      /* cheap, idempotent, rate-limited internally */
int  lj_metrics_history(int which, const float **out); /* raw ring, newest last */

/* Smoothed view of a series for the LOAD plot, newest last, same count as
 * lj_metrics_history. A trailing moving average over lj_metrics_smooth_window()
 * samples kills the per-poll jitter that made the operator call the plot
 * seizure-inducing, while lj_metrics_history keeps the RAW ring available for
 * callers that need per-sample truth. Header and LOAD use the display API.
 *
 * ⚠ NAN IS EXCLUDED FROM THE MEAN, NEVER COUNTED AS 0. A window with no valid
 * sample stays NAN, so an unavailable source renders as unavailable rather than
 * as a flat idle line — the same rule the sampler itself follows. */
int  lj_metrics_history_smoothed(int which, const float **out);
/* The configured smoothing window in samples; 1 means smoothing is off and
 * lj_metrics_history_smoothed returns the raw ring. Derived at init from
 * LJ_METRICS_SMOOTH_MS (default below) and the poll cadence. */
int  lj_metrics_smooth_window(void);
/* "CPU 34%" / "GPU unavailable". ⚠ The returned pointer is valid until this
 * caller's LJ_METRIC_COUNT-th SUBSEQUENT call: the labels rotate through a
 * small ring so every series can be held at once, which is what the LOAD tile
 * and the header ribbon both need. A single shared buffer made two labels the
 * same string — printf("%s | %s", label(CPU), label(MEM)) printed the CPU one
 * twice. Do not keep a label across many calls; format or draw it. */
const char *lj_metrics_label(int which);
void lj_metrics_close(void);
void lj_metrics_current(lj_sample *out);               /* the latest sample */
double lj_metrics_poll_us(void);                       /* last sample cost in µs, 0 before first */

#endif /* LJ_METRICS_H */
