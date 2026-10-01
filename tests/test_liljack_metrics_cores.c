/* the operator 2026-09-13: header plots become STACKED per-component columns — "height
 * of a column is load, colour is a core — same for GPU and memory: swap, RAM".
 * The sampler therefore publishes, next to the aggregate rings, one display
 * ring per logical core (busy/core_count), three MEM parts (used, cached, swap)
 * and two GPU parts (compute, memory), all closed by the SAME 25/50 ms bucket
 * rule as the aggregate: every bucket a sample closes carries the value, NaN
 * only on an outage or an unavailable source.
 *
 * Fake-/proc contract (LJ_METRICS_PROC=<dir>): stat holds the aggregate "cpu "
 * line plus "cpu0".."cpuN" lines; meminfo holds MemTotal, MemAvailable, Cached,
 * Buffers, SwapTotal, SwapFree. A missing meminfo key is 0 for THAT component,
 * never NaN for the whole plot. LJ_METRICS_NVML=/nonexistent forces the GPU
 * parts unavailable (NaN), which must leave CPU and MEM untouched. */
#define _GNU_SOURCE
#include "../liljack_app/c_metrics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static void sleep_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,NULL);}
static char stat_path[320];
/* Per-core busy over each 100-jiffy step: core0 100%, core1 50%, core2 0%, core3 25%.
 * Every poll gets a fresh step so every sample is a valid delta of the same shape. */
static const int BUSY[4]={100,50,0,25};
static void write_stat(int k){
    FILE *f=fopen(stat_path,"w");if(!f){perror(stat_path);exit(2);}
    unsigned long long tb=0,ti=0;for(int c=0;c<4;c++){tb+=(unsigned long long)BUSY[c]*k;ti+=(unsigned long long)(100-BUSY[c])*k;}
    fprintf(f,"cpu  %llu 0 0 %llu 0 0 0 0\n",tb,ti);
    for(int c=0;c<4;c++)fprintf(f,"cpu%d %llu 0 0 %llu 0 0 0 0\n",c,(unsigned long long)BUSY[c]*k,(unsigned long long)(100-BUSY[c])*k);
    fputs("intr 0\nctxt 0\n",f);fclose(f);
}
static float last(const float *h,int n){for(int i=n-1;i>=0;i--)if(isfinite(h[i]))return h[i];return NAN;}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    char dir[256]="/tmp/lj-metrics-cores.XXXXXX";if(!mkdtemp(dir))return 2;
    snprintf(stat_path,sizeof stat_path,"%s/stat",dir);write_stat(1);
    char p[320];snprintf(p,sizeof p,"%s/meminfo",dir);
    {FILE *f=fopen(p,"w");if(!f)return 2;
     fputs("MemTotal:       1000 kB\nMemFree:         100 kB\nMemAvailable:    250 kB\nBuffers:          50 kB\nCached:          100 kB\n"
           "SwapTotal:       400 kB\nSwapFree:        300 kB\n",f);fclose(f);}
    setenv("LJ_METRICS_PROC",dir,1);setenv("LJ_METRICS_NVML","/nonexistent/libnvidia-ml.so.1",1);
    unsetenv("LJ_METRICS_INTERVAL_MS");unsetenv("LJ_METRICS_DISPLAY_MS");
    CHECK(lj_metrics_part_count(LJ_METRIC_MEM)==3&&lj_metrics_part_count(LJ_METRIC_GPU)==2);
    CHECK(lj_metrics_init()==0);
    for(int k=2;k<=28;k++){write_stat(k);lj_metrics_poll();sleep_ms(25);}   /* ~675 ms: a dozen 50 ms buckets */
    CHECK(lj_metrics_core_count()==4);
    CHECK(lj_metrics_part_count(LJ_METRIC_CPU)==4);
    const float *h=NULL;int n=lj_metrics_paced_history(LJ_METRIC_CPU,&h);CHECK(n>=8);
    float cpu=last(h,n);CHECK(fabsf(cpu-.4375f)<.01f);                 /* (100+50+0+25)/400 */
    float sum=0;
    for(int c=0;c<4;c++){
        const float *ch=NULL;int cn=lj_metrics_paced_core_history(c,&ch);CHECK(cn==n);
        float v=last(ch,cn);float want=(float)BUSY[c]/100.f/4.f;
        int holes=0;for(int i=0;i<cn;i++)if(!isfinite(ch[i]))holes++;   /* every closed bucket carries a value */
        printf("metrics-cores: core%d paced=%.4f want=%.4f buckets=%d holes=%d\n",c,v,want,cn,holes);
        CHECK(fabsf(v-want)<.01f);CHECK(holes<=1);sum+=v;
        const float *same=NULL;CHECK(lj_metrics_paced_part_history(LJ_METRIC_CPU,c,&same)==cn&&same==ch);
    }
    CHECK(fabsf(sum-cpu)<.01f);                                          /* the stack is exactly the machine load */
    CHECK(lj_metrics_paced_core_history(4,&h)==0&&lj_metrics_paced_core_history(-1,&h)==0);
    /* MEM: used = (1 - 250/1000) - cached = .75 - .15 = .60; cached = (100+50)/1000; swap = (400-300)/1000. */
    const float *mem=NULL;int mn=lj_metrics_paced_history(LJ_METRIC_MEM,&mem);CHECK(mn==n);
    const float *used=NULL,*cached=NULL,*swap=NULL;
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_MEM,0,&used)==n);
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_MEM,1,&cached)==n);
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_MEM,2,&swap)==n);
    printf("metrics-cores: mem=%.3f used=%.3f cached=%.3f swap=%.3f\n",last(mem,mn),last(used,n),last(cached,n),last(swap,n));
    CHECK(fabsf(last(used,n)-.60f)<.001f&&fabsf(last(cached,n)-.15f)<.001f&&fabsf(last(swap,n)-.10f)<.001f);
    CHECK(fabsf(last(used,n)+last(cached,n)-last(mem,mn))<.001f);        /* first two combined == today's MEM */
    /* GPU: NVML absent -> both parts unavailable (NaN, never 0), while CPU/MEM above stayed finite. */
    const float *gc=NULL,*gm=NULL;
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_GPU,0,&gc)==n&&lj_metrics_paced_part_history(LJ_METRIC_GPU,1,&gm)==n);
    for(int i=0;i<n;i++)CHECK(!isfinite(gc[i])&&!isfinite(gm[i]));
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_GPU,2,&gc)==0&&lj_metrics_paced_part_history(LJ_METRIC_GPU_MEM,0,&gc)==0);
    lj_metrics_close();
    /* Missing meminfo keys: that component is 0, the plot is not NaN. */
    {FILE *f=fopen(p,"w");if(!f)return 2;fputs("MemTotal:       1000 kB\nMemAvailable:    250 kB\n",f);fclose(f);}
    CHECK(lj_metrics_init()==0);
    for(int k=30;k<=36;k++){write_stat(k);lj_metrics_poll();sleep_ms(25);}
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_MEM,0,&used)>0);
    CHECK(lj_metrics_paced_part_history(LJ_METRIC_MEM,1,&cached)>0&&lj_metrics_paced_part_history(LJ_METRIC_MEM,2,&swap)>0);
    n=lj_metrics_paced_history(LJ_METRIC_MEM,&mem);
    printf("metrics-cores: no-keys used=%.3f cached=%.3f swap=%.3f\n",last(used,n),last(cached,n),last(swap,n));
    CHECK(fabsf(last(used,n)-.75f)<.001f&&last(cached,n)==0.f&&last(swap,n)==0.f);
    lj_metrics_close();
    puts("metrics-cores: 4 cores 100/50/0/25 stacked to the machine load; MEM used/cached/swap; GPU parts unavailable stay NaN PASS");
    return 0;
}
