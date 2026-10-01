#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "c_metrics.h"
#include "hui_sparkline.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── NVML function-pointer surface ───────────────────────────────────────── */
typedef struct { unsigned int gpu; unsigned int memory; } nvml_util_t;
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvml_mem_t;
typedef void *nvml_dev_t;
typedef int (*fn_nvml_init)(void);
typedef int (*fn_nvml_handle)(unsigned int, nvml_dev_t *);
typedef int (*fn_nvml_util)(nvml_dev_t, nvml_util_t *);
typedef int (*fn_nvml_mem)(nvml_dev_t, nvml_mem_t *);
typedef int (*fn_nvml_temp)(nvml_dev_t, unsigned int, unsigned int *);

/* ── global state: all fixed-size, no heap, no realloc ───────────────────── */
static struct {
    int inited;
    int fd_stat;                /* /proc/stat, opened once */
    int fd_meminfo;             /* /proc/meminfo, opened once */
    char proc_dir[512];
    char stat_path[560];
    char meminfo_path[560];

    /* CPU delta */
    unsigned long long prev_idle, prev_total;
    int cpu_valid;              /* 0 until two /proc/stat reads exist */
    /* per-core deltas (the "cpuN" lines), same rule as the aggregate */
    unsigned long long prev_core_idle[LJ_METRIC_CORES_MAX], prev_core_total[LJ_METRIC_CORES_MAX];
    int core_valid[LJ_METRIC_CORES_MAX];
    int core_count;             /* cpuN lines seen on the last read, capped */

    /* stacked parts: raw ring + display ring + paced copy per part. Parts are
     * addressed by a fixed base per metric so one loop drives them all. */
#define PART_CPU 0
#define PART_MEM (LJ_METRIC_CORES_MAX)
#define PART_GPU (LJ_METRIC_CORES_MAX+3)
#define PART_COUNT (LJ_METRIC_CORES_MAX+3+2)
    float part_hist[PART_COUNT][LJ_METRIC_HISTORY];
    hui_sparkline_bucket part_bucket[PART_COUNT];
    float part_display[PART_COUNT][LJ_METRIC_HISTORY];
    float part_paced[PART_COUNT][LJ_METRIC_HISTORY];

    /* ring buffers */
    float hist[LJ_METRIC_COUNT][LJ_METRIC_HISTORY];
    hui_sparkline_bucket bucket[LJ_METRIC_COUNT];
    float display[LJ_METRIC_COUNT][LJ_METRIC_HISTORY];
    int display_count;
    uint64_t display_revision;
    uint64_t display_now_ms;      /* clock of the last display bucket push */
    float paced[LJ_METRIC_COUNT][LJ_METRIC_HISTORY];
    int paced_count;uint64_t paced_at;uint64_t paced_revision;int paced_started;int repaint_ms;
    int head;                   /* next write index */
    int count;                  /* valid samples (<= LJ_METRIC_HISTORY) */
    lj_sample cur;              /* the latest sample */

    /* rate limit */
    int interval_ms;
    int label_hold_ms;
    int display_ms;                              /* live plot bucket width */
    double last_poll_s;

    /* diagnostic */
    double last_cost_us;

    /* NVML */
    void *lib;
    fn_nvml_init nvml_init;
    fn_nvml_handle nvml_handle;
    fn_nvml_util nvml_util;
    fn_nvml_mem nvml_mem;
    fn_nvml_temp nvml_temp;
    int gpu_avail;
    nvml_dev_t dev;
    unsigned int gpu_temp_c;
    int gpu_has_temp;

    char read_buf[16384];
    float linear[LJ_METRIC_COUNT][LJ_METRIC_HISTORY];

    /* LOAD-plot smoothing: a second, separate array so the raw ring (hist /
     * linear) is never overwritten by the display aggregation. */
    float smooth[LJ_METRIC_COUNT][LJ_METRIC_HISTORY];
    int smooth_window;          /* samples averaged; 1 = smoothing off */
} G;

static const char *const SERIES_NAME[LJ_METRIC_COUNT] =
    { "CPU", "MEM", "GPU", "GPU MEM" };

static double monotonic(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* pread from offset 0; fall back to rewind+read for proc files that refuse it. */
static ssize_t proc_read(int fd, char *buf, size_t cap) {
    ssize_t n = pread(fd, buf, cap - 1, 0);
    if (n < 0) {
        if (lseek(fd, 0, SEEK_SET) == (off_t)-1) return -1;
        n = read(fd, buf, cap - 1);
    }
    if (n < 0) return -1;
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

/* ── /proc/stat ──────────────────────────────────────────────────────────── */
/* The fields after a "cpu"/"cpuN" prefix: total = sum of all, idle = idle+iowait. */
static int parse_cpu_fields(const char *p0, unsigned long long *total, unsigned long long *idle) {
    unsigned long long f[8] = {0};
    char *p = (char *)p0;
    int n = 0;
    while (n < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') break;
        f[n] = strtoull(p, &p, 10);
        n++;
    }
    if (n < 4) return -1;                        /* need user nice system idle */
    unsigned long long t = 0;
    for (int i = 0; i < n; i++) t += f[i];
    *total = t;
    *idle = f[3] + (n > 4 ? f[4] : 0);           /* idle + iowait */
    return 0;
}
/* Parse the aggregate "cpu " line. */
static int parse_cpu(const char *buf, unsigned long long *total, unsigned long long *idle) {
    const char *line = buf;
    while (*line) {
        if (!strncmp(line, "cpu ", 4)) break;
        const char *nl = strchr(line, '\n');
        if (!nl) return -1;
        line = nl + 1;
    }
    if (!*line) return -1;
    return parse_cpu_fields(line + 3, total, idle);
}
/* Parse every "cpuN" line into total[]/idle[] by N; returns the count seen
 * (capped at LJ_METRIC_CORES_MAX). A fixture with only "cpu " gives 0 cores,
 * which the plot treats as "no stack": the aggregate bar as before. */
static int parse_cores(const char *buf, unsigned long long *total, unsigned long long *idle) {
    int count = 0;
    const char *line = buf;
    while (*line) {
        if (!strncmp(line, "cpu", 3) && line[3] >= '0' && line[3] <= '9') {
            char *end = NULL;
            unsigned long n = strtoul(line + 3, &end, 10);
            if (end && (*end == ' ' || *end == '\t') && n < LJ_METRIC_CORES_MAX &&
                parse_cpu_fields(end, &total[n], &idle[n]) == 0 && (int)n + 1 > count)
                count = (int)n + 1;
        } else if (strncmp(line, "cpu", 3)) break;   /* cpu lines lead /proc/stat; stop at intr */
        const char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    return count;
}
/* Trailing mean of the last `w` raw samples of a part ring, NaN skipped (the
 * same smoothing the aggregate gets in lj_metrics_history_smoothed, but only
 * the newest value is needed to feed a display bucket, so no copy is kept). */
static float part_latest_smoothed(int part) {
    int w = G.smooth_window > 1 ? G.smooth_window : 1;
    if (w > G.count) w = G.count;
    float sum = 0.0f; int cnt = 0;
    for (int j = 0; j < w; j++) {
        float v = G.part_hist[part][(G.head - 1 - j + LJ_METRIC_HISTORY * 2) % LJ_METRIC_HISTORY];
        if (v != v) continue;
        sum += v; cnt++;
    }
    return cnt ? sum / (float)cnt : (float)NAN;
}

/* ── /proc/meminfo ───────────────────────────────────────────────────────── */
static int find_u64(const char *buf, const char *key, unsigned long long *out) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p < '0' || *p > '9') return -1;
    char *end = NULL;
    *out = strtoull(p, &end, 10);
    return (end && end != p) ? 0 : -1;
}

/* ── NVML ────────────────────────────────────────────────────────────────── */
static void nvml_close(void) {
    if (G.lib) { dlclose(G.lib); G.lib = NULL; }
    G.gpu_avail = 0;
    G.dev = NULL;
    G.gpu_has_temp = 0;
}

static void nvml_open(void) {
    const char *path = getenv("LJ_METRICS_NVML");
    if (!path || !*path) path = "libnvidia-ml.so.1";
    nvml_close();
    G.lib = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
    if (!G.lib) return;                          /* absent -> unavailable, never 0.0 */
    G.nvml_init   = (fn_nvml_init)  dlsym(G.lib, "nvmlInit_v2");
    G.nvml_handle = (fn_nvml_handle)dlsym(G.lib, "nvmlDeviceGetHandleByIndex_v2");
    G.nvml_util   = (fn_nvml_util)  dlsym(G.lib, "nvmlDeviceGetUtilizationRates");
    G.nvml_mem    = (fn_nvml_mem)   dlsym(G.lib, "nvmlDeviceGetMemoryInfo");
    G.nvml_temp   = (fn_nvml_temp)  dlsym(G.lib, "nvmlDeviceGetTemperature");
    if (!G.nvml_init || !G.nvml_handle || !G.nvml_util || !G.nvml_mem) {
        nvml_close();
        return;
    }
    if (G.nvml_init() != 0) { nvml_close(); return; }
    G.gpu_avail = 1;
}

static void nvml_sample(float *gpu, float *gpu_mem) {
    *gpu = *gpu_mem = (float)NAN;
    if (!G.gpu_avail) return;
    if (G.nvml_handle(0, &G.dev) != 0) { nvml_close(); return; }
    nvml_util_t u;
    nvml_mem_t m;
    if (G.nvml_util(G.dev, &u) == 0) *gpu = (float)u.gpu / 100.0f;
    if (G.nvml_mem(G.dev, &m) == 0 && m.total > 0)
        *gpu_mem = (float)m.used / (float)m.total;
    if (G.nvml_temp) {
        unsigned int t = 0;
        if (G.nvml_temp(G.dev, 0, &t) == 0) { G.gpu_temp_c = t; G.gpu_has_temp = 1; }
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

int lj_metrics_init(void) {
    memset(&G, 0, sizeof G);
    const char *proc = getenv("LJ_METRICS_PROC");
    if (!proc || !*proc) proc = "/proc";
    snprintf(G.proc_dir, sizeof G.proc_dir, "%s", proc);
    snprintf(G.stat_path, sizeof G.stat_path, "%s/stat", G.proc_dir);
    snprintf(G.meminfo_path, sizeof G.meminfo_path, "%s/meminfo", G.proc_dir);

    G.display_ms = LJ_METRICS_DISPLAY_MS;          /* one plot column; the operator wants it configurable (50 or 100 ms) */
    { const char *dms = getenv("LJ_METRICS_DISPLAY_MS"); if (dms && *dms) { int v = atoi(dms); if (v >= 25 && v <= 1000) G.display_ms = v; } }
    G.label_hold_ms = LJ_METRICS_LABEL_MS;
    G.interval_ms = LJ_METRICS_REF_INTERVAL_MS;    /* 25 ms polls: a 50 ms column averages two samples (mean poll ~0.4 ms) */
    const char *ims = getenv("LJ_METRICS_INTERVAL_MS");
    if (ims && *ims) {
        int v = atoi(ims);
        if (v >= 0) G.interval_ms = v;
    }

    /* Trailing moving-average window for the LOAD plot, in SAMPLES. Derived from
     * the poll cadence so "a few seconds" stays a few seconds whatever the
     * interval; the ref falls back to 20ms when the rate limit is disabled, so
     * the value is still deterministic for tests. 0 or 1 disables smoothing. */
    {
        long ms = LJ_METRICS_SMOOTH_MS;
        const char *sms = getenv("LJ_METRICS_SMOOTH_MS");
        if (sms && *sms) ms = atol(sms);
        if (ms <= 1) {
            G.smooth_window = 1;
        } else {
            int ref = G.interval_ms > 0 ? G.interval_ms : LJ_METRICS_REF_INTERVAL_MS;
            long w = (ms + ref / 2) / ref;      /* round to nearest sample */
            if (w < 1) w = 1;
            if (w > LJ_METRIC_HISTORY) w = LJ_METRIC_HISTORY;
            G.smooth_window = (int)w;
        }
    }

    G.fd_stat = open(G.stat_path, O_RDONLY | O_CLOEXEC);
    G.fd_meminfo = open(G.meminfo_path, O_RDONLY | O_CLOEXEC);
    if (G.fd_stat < 0 || G.fd_meminfo < 0) {
        if (G.fd_stat >= 0) { close(G.fd_stat); G.fd_stat = -1; }
        if (G.fd_meminfo >= 0) { close(G.fd_meminfo); G.fd_meminfo = -1; }
        return -1;                               /* /proc unreadable — the only fatal error */
    }
    nvml_open();                                 /* best effort; unavailable is not an error */
    G.repaint_ms = LJ_METRICS_DISPLAY_REPAINT_MS;
    const char *rms = getenv("LJ_METRICS_DISPLAY_REPAINT_MS");
    if (rms && *rms) { int v = atoi(rms); if (v >= G.display_ms) G.repaint_ms = v; }
    G.inited = 1;
    return 0;
}

/* Called once per acquired sample, after the raw ring is updated. */
/* Push one value into one display ring; returns the buckets it closed. Shared
 * by the aggregate rings and the stacked part rings so both obey ONE rule. */
static uint64_t display_push(float *ring,hui_sparkline_bucket *bucket,uint64_t now_ms,float value){
    float closed=NAN;
    uint64_t count=hui_sparkline_bucket_push(bucket,now_ms,
            (uint64_t)(G.display_ms>0?G.display_ms:LJ_METRICS_DISPLAY_MS),value,&closed);
    if(!count)return 0;
    int add=count>LJ_METRIC_HISTORY?LJ_METRIC_HISTORY:(int)count;
    int keep=G.display_count;
    if(keep>LJ_METRIC_HISTORY-add)keep=LJ_METRIC_HISTORY-add;
    memmove(ring,ring+G.display_count-keep,(size_t)keep*sizeof(float));
    /* Every bucket this sample closed carries its value. It used to be
     * bucket 0 only, the other count-1 NaN: with a 100 ms poll and 25 ms
     * buckets three of four columns were black — the operator's "zebra red/black"
     * at 100% GPU. NaN is reserved for a real outage: a gap longer than
     * LJ_METRICS_DISPLAY_OUTAGE_MS, and for a sample that itself has no
     * value (an unavailable source stays a gap, never a number). */
    int outage=count>(uint64_t)(LJ_METRICS_DISPLAY_OUTAGE_MS/(G.display_ms>0?G.display_ms:LJ_METRICS_DISPLAY_MS));
    for(int i=0;i<add;i++)ring[keep+i]=(outage&&i<add-1)?NAN:closed;
    return count;
}
static void display_sample(uint64_t now_ms){
    uint64_t elapsed=0;G.display_now_ms=now_ms;
    for(int m=0;m<LJ_METRIC_COUNT;m++){
        const float *hist=NULL;
        int n=lj_metrics_history_smoothed(m,&hist);
        uint64_t count=display_push(G.display[m],&G.bucket[m],now_ms,n?hist[n-1]:NAN);
        if(count)elapsed=count;
    }
    /* The part rings share the clock and the bucket width, so they close the
     * same buckets as the aggregate and G.display_count stays one number. */
    for(int p=0;p<PART_COUNT;p++)
        display_push(G.part_display[p],&G.part_bucket[p],now_ms,G.count?part_latest_smoothed(p):(float)NAN);
    if(elapsed){
        G.display_count=elapsed>=(uint64_t)(LJ_METRIC_HISTORY-G.display_count)?
            LJ_METRIC_HISTORY:G.display_count+(int)elapsed;
        G.display_revision++;
    }
}
/* Paced view: the display ring is copied at most every repaint_ms, so the
 * header plot steps by several 25ms columns at once instead of crawling. */
static void paced_refresh(void){
    int rp=G.repaint_ms>0?G.repaint_ms:LJ_METRICS_DISPLAY_REPAINT_MS;
    uint64_t now=G.display_now_ms;
    if(G.paced_started&&now>=G.paced_at&&now-G.paced_at<(uint64_t)rp)return;
    if(G.paced_started&&G.paced_count==G.display_count&&!memcmp(G.paced,G.display,sizeof G.paced)
       &&!memcmp(G.part_paced,G.part_display,sizeof G.part_paced))return;
    memcpy(G.paced,G.display,sizeof G.paced);G.paced_count=G.display_count;
    memcpy(G.part_paced,G.part_display,sizeof G.part_paced);   /* parts step with the aggregate: one snapshot */
    G.paced_at=now;G.paced_started=1;G.paced_revision++;
}
int lj_metrics_paced_history(int which,const float **out){
    if(!out||which<0||which>=LJ_METRIC_COUNT)return 0;
    paced_refresh();*out=G.paced[which];return G.paced_count;
}
int lj_metrics_core_count(void){return G.core_count;}
int lj_metrics_part_count(int which){
    switch(which){case LJ_METRIC_CPU:return G.core_count;case LJ_METRIC_MEM:return 3;case LJ_METRIC_GPU:return 2;default:return 0;}
}
int lj_metrics_paced_part_history(int which,int part,const float **out){
    if(!out||part<0||part>=lj_metrics_part_count(which))return 0;
    int base=which==LJ_METRIC_CPU?PART_CPU:which==LJ_METRIC_MEM?PART_MEM:PART_GPU;
    paced_refresh();*out=G.part_paced[base+part];return G.paced_count;
}
int lj_metrics_paced_core_history(int core,const float **out){return lj_metrics_paced_part_history(LJ_METRIC_CPU,core,out);}
uint64_t lj_metrics_paced_revision(void){paced_refresh();return G.paced_revision;}
int lj_metrics_repaint_ms(void){return G.repaint_ms>0?G.repaint_ms:LJ_METRICS_DISPLAY_REPAINT_MS;}
int lj_metrics_display_ms(void){return G.display_ms>0?G.display_ms:LJ_METRICS_DISPLAY_MS;}
int lj_metrics_set_interval_ms(int ms){if(ms<25)ms=25;if(ms>1000)ms=1000;return G.interval_ms=ms;}
int lj_metrics_set_label_hold_ms(int ms){if(ms<100)ms=100;if(ms>5000)ms=5000;return G.label_hold_ms=ms;}
int lj_metrics_interval_ms(void){return G.interval_ms;}
int lj_metrics_label_hold_ms(void){return G.label_hold_ms>0?G.label_hold_ms:LJ_METRICS_LABEL_MS;}
int lj_metrics_set_display_ms(int ms){if(ms<25)ms=25;if(ms>1000)ms=1000;G.display_ms=ms;if(G.repaint_ms<ms)G.repaint_ms=ms;return G.display_ms;}
int lj_metrics_set_repaint_ms(int ms){int lo=lj_metrics_display_ms();if(ms<lo)ms=lo;if(ms>2000)ms=2000;G.repaint_ms=ms;return G.repaint_ms;}
int lj_metrics_display_history(int which,const float **out){
    if(!out||which<0||which>=LJ_METRIC_COUNT)return 0;
    *out=G.display[which];return G.display_count;
}
uint64_t lj_metrics_display_revision(void){return G.display_revision;}
/* The value text is HELD for LJ_METRICS_LABEL_MS (the operator 2026-09-12: "numbers
 * flicker" once buckets went to 25ms). The graph keeps its 25ms columns; only
 * the number is sampled at the slower cadence, and it is fixed-width so the
 * text never changes width between ticks. */
const char *lj_metrics_display_label(int which){
    static char labels[LJ_METRIC_COUNT][32];
    static uint64_t held_at[LJ_METRIC_COUNT];
    static int held_valid[LJ_METRIC_COUNT];
    if(which<0||which>=LJ_METRIC_COUNT)return "";
    uint64_t now=G.display_now_ms;
    if(held_valid[which]&&now>=held_at[which]&&now-held_at[which]<(uint64_t)lj_metrics_label_hold_ms())return labels[which];
    float v=G.display_count?G.display[which][G.display_count-1]:NAN;
    if(isfinite(v)){snprintf(labels[which],32,"%s %3.0f%%",SERIES_NAME[which],v*100);held_at[which]=now;held_valid[which]=1;}
    else {snprintf(labels[which],32,"%s   —",SERIES_NAME[which]);held_valid[which]=0;}   /* never hold "unavailable": show the first real value at once */
    return labels[which];
}
uint64_t lj_metrics_label_ms(void){return (uint64_t)lj_metrics_label_hold_ms();}

void lj_metrics_poll(void) {
    if (!G.inited) return;
    double now = monotonic();
    if (G.interval_ms > 0 && (now - G.last_poll_s) * 1000.0 < G.interval_ms)
        return;                                  /* rate limited: no /proc touch */
    G.last_poll_s = now;
    double t0 = now;

    lj_sample s;
    s.cpu = (float)NAN;
    s.mem = (float)NAN;
    s.gpu = (float)NAN;
    s.gpu_mem = (float)NAN;
    float parts[PART_COUNT];
    for (int p = 0; p < PART_COUNT; p++) parts[p] = (float)NAN;

    /* CPU: a delta between two reads; the first read has no previous. */
    if (proc_read(G.fd_stat, G.read_buf, sizeof G.read_buf) > 0) {
        unsigned long long total = 0, idle = 0;
        /* Per core, the same delta rule; each core's share is busy/core_count
         * so the stacked column adds up to the aggregate line exactly. */
        {
            unsigned long long ct[LJ_METRIC_CORES_MAX] = {0}, ci[LJ_METRIC_CORES_MAX] = {0};
            int n = parse_cores(G.read_buf, ct, ci);
            if (n != G.core_count) memset(G.core_valid, 0, sizeof G.core_valid);   /* hotplug: restart the deltas */
            G.core_count = n;
            for (int c = 0; c < n; c++) {
                if (ct[c] == 0) continue;
                if (G.core_valid[c] && ct[c] > G.prev_core_total[c] && ci[c] >= G.prev_core_idle[c]) {
                    float busy = 1.0f - (float)(ci[c] - G.prev_core_idle[c]) / (float)(ct[c] - G.prev_core_total[c]);
                    if (busy < 0.0f) busy = 0.0f;
                    if (busy > 1.0f) busy = 1.0f;
                    parts[PART_CPU + c] = busy / (float)n;
                }
                G.prev_core_total[c] = ct[c];
                G.prev_core_idle[c] = ci[c];
                G.core_valid[c] = 1;
            }
        }
        if (parse_cpu(G.read_buf, &total, &idle) == 0 && total > 0) {
            if (G.cpu_valid && total > G.prev_total && idle >= G.prev_idle) {
                unsigned long long dt = total - G.prev_total;
                unsigned long long di = idle - G.prev_idle;
                float busy = 1.0f - (float)di / (float)dt;
                if (busy < 0.0f) busy = 0.0f;
                if (busy > 1.0f) busy = 1.0f;
                s.cpu = busy;
            }
            G.prev_total = total;
            G.prev_idle = idle;
            G.cpu_valid = 1;
        }
    }

    /* MEM: single read, valid immediately. */
    if (proc_read(G.fd_meminfo, G.read_buf, sizeof G.read_buf) > 0) {
        unsigned long long total = 0, avail = 0;
        if (find_u64(G.read_buf, "MemTotal", &total) == 0 && total > 0) {
            if (find_u64(G.read_buf, "MemAvailable", &avail) != 0)
                find_u64(G.read_buf, "MemFree", &avail);
            if (avail <= total) s.mem = 1.0f - (float)avail / (float)total;
            /* Stack parts: a missing key is 0 for THAT component, never NaN
             * for the whole plot (a kernel without swap still has RAM). */
            unsigned long long cached = 0, buffers = 0, st = 0, sf = 0;
            find_u64(G.read_buf, "\nCached", &cached);     /* line-anchored: "SwapCached" also contains the key */
            find_u64(G.read_buf, "Buffers", &buffers);
            find_u64(G.read_buf, "SwapTotal", &st);
            find_u64(G.read_buf, "SwapFree", &sf);
            float fc = (float)(cached + buffers) / (float)total;
            if (fc > 1.0f) fc = 1.0f;
            /* used = today's MEM value minus the cached share, so used+cached
             * IS the MEM value; clamped at 0 because MemAvailable can dip
             * below Cached+Buffers (shmem is counted cached but not reclaimable). */
            float fu = isfinite(s.mem) ? s.mem - fc : 0.0f;
            if (fu < 0.0f) fu = 0.0f;
            parts[PART_MEM + 0] = fu;
            parts[PART_MEM + 1] = fc;
            parts[PART_MEM + 2] = (st > sf && st > 0) ? (float)(st - sf) / (float)total : 0.0f;
        }
    }

    nvml_sample(&s.gpu, &s.gpu_mem);
    parts[PART_GPU + 0] = s.gpu;         /* NaN when NVML is unavailable: the stack draws nothing */
    parts[PART_GPU + 1] = s.gpu_mem;

    for (int p = 0; p < PART_COUNT; p++) G.part_hist[p][G.head] = parts[p];
    G.hist[LJ_METRIC_CPU][G.head] = s.cpu;
    G.hist[LJ_METRIC_MEM][G.head] = s.mem;
    G.hist[LJ_METRIC_GPU][G.head] = s.gpu;
    G.hist[LJ_METRIC_GPU_MEM][G.head] = s.gpu_mem;
    G.head = (G.head + 1) % LJ_METRIC_HISTORY;
    if (G.count < LJ_METRIC_HISTORY) G.count++;
    G.cur = s;
    display_sample((uint64_t)(now*1000.0));

    G.last_cost_us = (monotonic() - t0) * 1e6;
}

int lj_metrics_history(int which, const float **out) {
    if (!out || which < 0 || which >= LJ_METRIC_COUNT) return 0;
    if (G.count == 0) { *out = G.linear[which]; return 0; }
    int start = (G.head - G.count + LJ_METRIC_HISTORY * 2) % LJ_METRIC_HISTORY;
    for (int i = 0; i < G.count; i++)
        G.linear[which][i] = G.hist[which][(start + i) % LJ_METRIC_HISTORY];
    *out = G.linear[which];
    return G.count;
}

/* Trailing moving average of a series, for the LOAD plot. The raw ring is left
 * intact: lj_metrics_history still returns per-sample values. NAN samples are SKIPPED, never averaged in as 0.0 — an
 * unavailable source stays unavailable, and a window with no valid sample is
 * itself NAN. The first samples use the partial window available so far. */
int lj_metrics_history_smoothed(int which, const float **out) {
    if (!out || which < 0 || which >= LJ_METRIC_COUNT) return 0;
    int n = lj_metrics_history(which, out);          /* fills G.linear[which] */
    int w = G.smooth_window;
    if (n <= 0 || w <= 1) return n;                  /* smoothing off: raw view */
    if (w > n) w = n;
    const float *raw = *out;
    for (int i = 0; i < n; i++) {
        int lo = i - w + 1;
        if (lo < 0) lo = 0;
        float sum = 0.0f;
        int cnt = 0;
        for (int j = lo; j <= i; j++) {
            float v = raw[j];
            if (v != v) continue;                    /* NAN excluded, never 0 */
            sum += v;
            cnt++;
        }
        G.smooth[which][i] = cnt ? sum / (float)cnt : (float)NAN;
    }
    *out = G.smooth[which];
    return n;
}

int lj_metrics_smooth_window(void) {
    return G.smooth_window;
}

void lj_metrics_current(lj_sample *out) {
    if (!out) return;
    /* ⚠ THE MODULE'S OWN RULE, WHICH IT BROKE WHEN UNINITIALISED. This header
     * states it plainly: "A field that is UNAVAILABLE is NAN, never 0.0: a
     * missing source rendered as a real number has bitten this repo repeatedly
     * (the GPU reads as an idle card, the CPU reads as 0% before the first
     * delta)." G is zero-initialised static storage, so before lj_metrics_init()
     * every field read as 0.0 — a perfectly idle machine. Measured 2026-09-10:
     * lj_metrics_init() is called ONLY from main(), so any embedder that drives
     * render() directly (the visual-inspection harness does exactly that) got
     * cpu=0.000000 and had no way to know it was meaningless. */
    if (!G.inited) {
        out->cpu = out->mem = out->gpu = out->gpu_mem = NAN;
        return;
    }
    *out = G.cur;
}

const char *lj_metrics_label(int which) {
    /* ⚠ ONE STATIC BUFFER MADE TWO LABELS THE SAME STRING. Every call returned
     * the same storage, so holding two at once — printf("%s | %s", label(CPU),
     * label(MEM)) — printed "CPU 10% | CPU 10%": the second call overwrote the
     * first before either was read. Measured 2026-09-10 while wiring the header
     * ribbon, which draws CPU and MEM side by side and is exactly the shape
     * that trips on it.
     *
     * A small ring gives each of the last LJ_METRIC_COUNT calls its own
     * storage, which is what every caller in this codebase actually needs (one
     * label per series, all live at once). The lifetime is now DOCUMENTED in
     * the header rather than left to be discovered: valid until the caller's
     * LJ_METRIC_COUNT-th subsequent call. */
    static char ring[LJ_METRIC_COUNT][96];
    static int slot = 0;
    char *buf = ring[slot];
    slot = (slot + 1) % LJ_METRIC_COUNT;
    buf[0] = '\0';
    if (which < 0 || which >= LJ_METRIC_COUNT) return buf;
    const char *name = SERIES_NAME[which];
    if (G.count == 0) { snprintf(buf, 96, "%s —", name); return buf; }
    switch (which) {
        case LJ_METRIC_CPU:
            if (!G.cpu_valid || isnan(G.cur.cpu))
                snprintf(buf, 96, "CPU —");
            else
                snprintf(buf, 96, "CPU %.0f%%", G.cur.cpu * 100.0f);
            break;
        case LJ_METRIC_MEM:
            snprintf(buf, 96, "MEM %.0f%%", isnan(G.cur.mem) ? 0.0f : G.cur.mem * 100.0f);
            break;
        case LJ_METRIC_GPU:
            if (isnan(G.cur.gpu)) snprintf(buf, 96, "GPU unavailable");
            else if (G.gpu_has_temp)
                snprintf(buf, 96, "GPU %.0f%% · %u°C", G.cur.gpu * 100.0f, G.gpu_temp_c);
            else snprintf(buf, 96, "GPU %.0f%%", G.cur.gpu * 100.0f);
            break;
        case LJ_METRIC_GPU_MEM:
            if (isnan(G.cur.gpu_mem)) snprintf(buf, 96, "GPU MEM —");
            else snprintf(buf, 96, "GPU MEM %.0f%%", G.cur.gpu_mem * 100.0f);
            break;
    }
    return buf;
}

double lj_metrics_poll_us(void) {
    return G.last_cost_us;
}

void lj_metrics_close(void) {
    if (G.fd_stat >= 0) { close(G.fd_stat); G.fd_stat = -1; }
    if (G.fd_meminfo >= 0) { close(G.fd_meminfo); G.fd_meminfo = -1; }
    nvml_close();
    G.inited = 0;
}
