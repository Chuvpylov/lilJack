#define _GNU_SOURCE
#include "../liljack_app/c_metrics.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static char dir[256];
static char stat_path[320];
static char meminfo_path[320];

static void write_file(const char *path, const char *fmt, ...) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    CHECK(fclose(f) == 0);
}

static void sleep_ms(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

int main(void) {
    /* ⚠ BEFORE ANY init(): the module's own contract says an unavailable field
     * is NAN and NEVER 0.0, because "a missing source rendered as a real number"
     * reads as an idle machine. G is zero-initialised static storage, so an
     * uninitialised sampler used to report cpu=0.000000 — perfectly idle, and
     * indistinguishable from a real reading. lj_metrics_init() is called only
     * from main(), so any embedder driving render() directly (the visual
     * inspection harness does) hit exactly this. */
    {
        lj_sample pre;
        lj_metrics_current(&pre);
        CHECK(pre.cpu != pre.cpu);        /* NAN, not 0.0 */
        CHECK(pre.mem != pre.mem);
        CHECK(pre.gpu != pre.gpu);
        CHECK(pre.gpu_mem != pre.gpu_mem);
    }

    snprintf(dir, sizeof dir, "/tmp/lj-metrics-test.XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(stat_path, sizeof stat_path, "%s/stat", dir);
    snprintf(meminfo_path, sizeof meminfo_path, "%s/meminfo", dir);

    /* ── init refuses an unreadable /proc ─────────────────────────────── */
    setenv("LJ_METRICS_PROC", "/nonexistent/lj-metrics-proc", 1);
    CHECK(lj_metrics_init() < 0);

    /* ── fixture: two stat reads give a clean 50% CPU delta ───────────── */
    /* idle/total v1 = 100/100, v2 = 150/200 -> delta idle 50 / delta total 100 */
    write_file(stat_path, "cpu  0 0 0 100 0 0 0 0\n");
    write_file(meminfo_path, "MemTotal:       1000 kB\nMemAvailable:    250 kB\n");

    setenv("LJ_METRICS_PROC", dir, 1);
    setenv("LJ_METRICS_NVML", "/nonexistent/libnvidia-ml.so.1", 1); /* force unavailable */
    unsetenv("LJ_METRICS_INTERVAL_MS");

    CHECK(lj_metrics_init() == 0);
    CHECK(lj_metrics_poll_us() == 0);                 /* nothing sampled yet */

    lj_metrics_poll();
    lj_sample s;
    lj_metrics_current(&s);
    /* First CPU read has no previous: not-yet-valid is NAN, never 0%. */
    CHECK(isnan(s.cpu));
    CHECK(fabsf(s.mem - 0.75f) < 0.001f);             /* 1 - 250/1000 */
    CHECK(isnan(s.gpu) && isnan(s.gpu_mem));          /* NVML absent -> unavailable, not 0.0 */
    CHECK(!strcmp(lj_metrics_label(LJ_METRIC_CPU), "CPU —"));
    CHECK(strstr(lj_metrics_label(LJ_METRIC_GPU), "unavailable") != NULL);

    const float *buf;
    CHECK(lj_metrics_history(LJ_METRIC_CPU, &buf) == 1);

    /* ── the rate limit: an immediate second poll does not touch /proc ── */
    lj_metrics_poll();
    CHECK(lj_metrics_history(LJ_METRIC_CPU, &buf) == 1); /* still one sample */
    lj_metrics_poll();
    CHECK(lj_metrics_history(LJ_METRIC_CPU, &buf) == 1);

    /* ── second real read: the delta is 50% ───────────────────────────── */
    write_file(stat_path, "cpu  50 0 0 150 0 0 0 0\n");
    sleep_ms(30);                                    /* past the 25ms rate limit */
    lj_metrics_poll();
    CHECK(lj_metrics_history(LJ_METRIC_CPU, &buf) == 2);
    lj_metrics_current(&s);
    CHECK(fabsf(s.cpu - 0.5f) < 0.001f);
    CHECK(!strcmp(lj_metrics_label(LJ_METRIC_CPU), "CPU 50%"));
    CHECK(lj_metrics_poll_us() > 0);

    /* ── wraparound: 300 samples cap at 256, newest last ──────────────── */
    lj_metrics_close();
    setenv("LJ_METRICS_INTERVAL_MS", "0", 1);        /* never rate-limited, for the ring test */
    CHECK(lj_metrics_init() == 0);
    for (int i = 0; i < 300; i++) {
        write_file(meminfo_path,
                   "MemTotal:       1000 kB\nMemAvailable:    %d kB\n", 1000 - i);
        lj_metrics_poll();
    }
    int n = lj_metrics_history(LJ_METRIC_MEM, &buf);
    CHECK(n == LJ_METRIC_HISTORY);                   /* capped at 256, not 300 */
    CHECK(fabsf(buf[0] - 0.044f) < 0.001f);          /* poll #44 -> 44/1000 */
    CHECK(fabsf(buf[255] - 0.299f) < 0.001f);        /* poll #299 -> 299/1000 */
    lj_metrics_close();

    printf("C metrics: /proc fixture, CPU delta 50%%, first-read NAN, NVML-absent "
           "unavailable, rate limit, 256-sample ring wraparound, LOAD-plot "
           "smoothing window (raw ring preserved, NAN not averaged as 0) passed");
    /* ⚠ TWO LABELS HELD AT ONCE MUST BE TWO DIFFERENT STRINGS. A single static
     * buffer made every call return the same storage, so the second overwrote
     * the first before either was read: printf("%s | %s", label(CPU),
     * label(MEM)) printed "CPU 10% | CPU 10%". Found while wiring the header
     * ribbon, which draws CPU and MEM side by side — exactly the shape that
     * trips on it. Every caller in this codebase wants one label per series,
     * all live at once. */
    {
        const char *c = lj_metrics_label(LJ_METRIC_CPU);
        const char *m = lj_metrics_label(LJ_METRIC_MEM);
        CHECK(c != m);                          /* distinct storage */
        CHECK(strncmp(c, "CPU", 3) == 0);
        CHECK(strncmp(m, "MEM", 3) == 0);       /* not a second "CPU …" */
        const char *g = lj_metrics_label(LJ_METRIC_GPU);
        CHECK(strncmp(c, "CPU", 3) == 0);       /* the first is still intact */
        CHECK(strncmp(g, "GPU", 3) == 0);
    }

    /* ── the LOAD-plot smoothing window ─────────────────────────────────
     * the operator called the per-sample LOAD sparkline seizure-inducing. The plot now
     * reads a trailing moving average; the RAW ring stays available so the
     * header ribbon keeps its per-sample readout. The window is derived from
     * LJ_METRICS_SMOOTH_MS / the poll cadence, and it is asserted here. */
    {
        lj_metrics_close();
        setenv("LJ_METRICS_PROC", dir, 1);
        setenv("LJ_METRICS_INTERVAL_MS", "0", 1);      /* sample as fast as called */
        setenv("LJ_METRICS_SMOOTH_MS", "100", 1);      /* 100ms / 25ms ref = 4 samples */
        write_file(stat_path, "cpu  0 0 0 100 0 0 0 0\n");
        write_file(meminfo_path, "MemTotal:       1000 kB\nMemAvailable:    900 kB\n");
        CHECK(lj_metrics_init() == 0);
        CHECK(lj_metrics_smooth_window() == 4);        /* the window IS asserted */

        /* mem = 1 - avail/1000 = 0.1, 0.2, 0.3, 0.4, 0.5 */
        for (int i = 1; i <= 5; i++) {
            write_file(meminfo_path,
                       "MemTotal:       1000 kB\nMemAvailable:    %d kB\n", 1000 - i * 100);
            lj_metrics_poll();
        }
        const float *raw = NULL, *sm = NULL;
        CHECK(lj_metrics_history(LJ_METRIC_MEM, &raw) == 5);
        CHECK(lj_metrics_history_smoothed(LJ_METRIC_MEM, &sm) == 5);
        CHECK(sm != raw);                              /* a distinct, smoothed view */
        CHECK(fabsf(raw[0] - 0.1f) < 0.001f);
        CHECK(fabsf(raw[4] - 0.5f) < 0.001f);          /* raw is UNCHANGED */
        CHECK(fabsf(sm[0] - 0.10f) < 0.001f);          /* partial window at the start */
        CHECK(fabsf(sm[1] - 0.15f) < 0.001f);
        CHECK(fabsf(sm[2] - 0.20f) < 0.001f);
        CHECK(fabsf(sm[3] - 0.25f) < 0.001f);
        CHECK(fabsf(sm[4] - 0.35f) < 0.001f);          /* (0.2+0.3+0.4+0.5)/4 */

        /* NAN is never averaged in as 0: an unavailable GPU stays unavailable. */
        const float *g2 = NULL;
        CHECK(lj_metrics_history_smoothed(LJ_METRIC_GPU, &g2) == 5);
        for (int i = 0; i < 5; i++) CHECK(isnan(g2[i]));

        /* window <= 1 disables smoothing and returns the raw ring. */
        lj_metrics_close();
        setenv("LJ_METRICS_SMOOTH_MS", "0", 1);
        CHECK(lj_metrics_init() == 0);
        CHECK(lj_metrics_smooth_window() == 1);
        const float *r2 = NULL, *s2 = NULL;
        lj_metrics_history(LJ_METRIC_MEM, &r2);
        lj_metrics_history_smoothed(LJ_METRIC_MEM, &s2);
        CHECK(s2 == r2);
        lj_metrics_close();
    }

    printf(" · per-poll cost %.2f µs\n", lj_metrics_poll_us());
    return 0;
}
