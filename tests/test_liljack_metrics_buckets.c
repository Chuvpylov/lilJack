/* the operator 2026-09-13: "GPU should be 100% red and it's zebra red/black". The display
 * ring has one bucket (50 ms default) per plot column; when a poll closes several buckets
 * at once only the first carried the value and the rest were NaN, drawn as
 * black columns. Samples at any cadence slower than the bucket must not leave holes. */
#define _GNU_SOURCE
#include "../liljack_app/c_metrics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static void sleep_ms(int ms){struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,NULL);}
static void write_file(const char *p,const char *s){FILE *f=fopen(p,"w");if(!f){perror(p);exit(2);}fputs(s,f);fclose(f);}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    char dir[256]="/tmp/lj-metrics-buckets.XXXXXX";if(!mkdtemp(dir))return 2;
    char p[320];snprintf(p,sizeof p,"%s/stat",dir);write_file(p,"cpu  0 0 0 100 0 0 0 0\n");
    snprintf(p,sizeof p,"%s/meminfo",dir);write_file(p,"MemTotal:       1000 kB\nMemAvailable:    250 kB\n");
    setenv("LJ_METRICS_PROC",dir,1);setenv("LJ_METRICS_NVML","/nonexistent/libnvidia-ml.so.1",1);
    setenv("LJ_METRICS_INTERVAL_MS","100",1);          /* a realistic poll cadence: 4 buckets per sample */
    if(lj_metrics_init()!=0){puts("metrics-buckets: init failed");return 2;}
    for(int i=0;i<12;i++){lj_metrics_poll();sleep_ms(100);}
    const float *h=NULL;int n=lj_metrics_display_history(LJ_METRIC_MEM,&h);
    int first=-1,last=-1;for(int i=0;i<n;i++)if(isfinite(h[i])){if(first<0)first=i;last=i;}
    int holes=0,span=first>=0?last-first+1:0;for(int i=first;i>=0&&i<=last;i++)if(!isfinite(h[i]))holes++;
    char pic[128];int k=0;for(int i=first;i>=0&&i<=last&&k<120;i++)pic[k++]=isfinite(h[i])?'#':'.';pic[k]=0;
    printf("metrics-buckets: buckets=%d sampled span=%d holes=%d (%s)\n",n,span,holes,pic);
    int ok=span>=20&&holes==0;printf("metrics-buckets: %s\n",ok?"PASS":"FAIL");lj_metrics_close();return ok?0:1;
}
