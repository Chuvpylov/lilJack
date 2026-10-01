/* Deterministic acquisition clock: the real c_metrics display integration. */
#include "../liljack_app/c_metrics.c"
#include <assert.h>
int main(int argc,char **argv){
    /* This test exercises the bucket mechanism at 25 ms; the DEFAULT is 50 ms since 2026-09-13
     * (the operator: "1 px over 50 ms"), so pin the runtime knob here. */
    setenv("LJ_METRICS_DISPLAY_MS","25",1);

    if(argc>1&&!strcmp(argv[1],"--live")){
        setenv("LJ_METRICS_NVML","/nonexistent/liljack-test-nvml",1);
        unsetenv("LJ_METRICS_INTERVAL_MS");
        assert(lj_metrics_init()==0&&G.interval_ms==25);
        double start=monotonic(),last=0,total_cost=0;int count=0;
        while(monotonic()-start<2.05){
            int before=G.count;lj_metrics_poll();
            if(G.count!=before){
                double at=G.last_poll_s;
                if(last)assert((at-last)*1000>=24.99);
                printf("real_sample_ms=%.3f delta_ms=%.3f revision=%llu label=%s cost_us=%.1f\n",
                    (at-start)*1000,last?(at-last)*1000:0,
                    (unsigned long long)lj_metrics_display_revision(),lj_metrics_display_label(LJ_METRIC_CPU),G.last_cost_us);
                last=at;total_cost+=G.last_cost_us;count++;
            }
            struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        }
        assert(count>=64&&count<=84);
        printf("Real sampler: %d samples in2.05s, configured25ms, mean poll %.1fus PASS\n",count,total_cost/count);
        lj_metrics_close();return 0;
    }

    /* One 25ms sample per 25ms display bucket: every step closes a bucket whose
     * value is the smoothed (window 3) value of the previous sample. */
    memset(&G,0,sizeof(G));G.display_ms=25;G.smooth_window=3;
    uint64_t revision=0;
    for(int t=0;t<=2000;t+=25){
        float value=(float)((t/25)%10)/10.f;
        for(int m=0;m<LJ_METRIC_COUNT;m++)G.hist[m][G.head]=m==LJ_METRIC_GPU?NAN:value;
        G.head=(G.head+1)%LJ_METRIC_HISTORY;if(G.count<LJ_METRIC_HISTORY)G.count++;
        display_sample((uint64_t)t);
        const float *history=NULL;int n=lj_metrics_display_history(LJ_METRIC_CPU,&history);
        assert(n==t/25);
        if(t){
            assert(lj_metrics_display_revision()==revision+1);
            /* Independent mean: the closed bucket holds ONE sample (t-25), itself a trailing mean of 3 raw samples. */
            int st=t-25;float sum=0;int count=0;
            for(int rt=st-50;rt<=st;rt+=25)if(rt>=0){sum+=(float)((rt/25)%10)/10.f;count++;}
            float expected=sum/count;assert(fabsf(expected-history[n-1])<.00001f);
            revision=lj_metrics_display_revision();
            assert(strstr(lj_metrics_display_label(LJ_METRIC_CPU),"%"));
            const float *gpu=NULL;assert(lj_metrics_display_history(LJ_METRIC_GPU,&gpu)==n&&isnan(gpu[n-1]));
        }
        printf("sample_ms=%d display_revision=%llu label=%s\n",t,(unsigned long long)lj_metrics_display_revision(),lj_metrics_display_label(LJ_METRIC_CPU));
    }
    assert(revision==80);
    /* Value label is HELD for LJ_METRICS_LABEL_MS and fixed width: over the 2s
     * run the text may change at most 2000/LABEL_MS (+1 for the first) times
     * and every label has the same length. (the operator: numbers flickered at 25ms.) */
    {
        memset(&G,0,sizeof(G));G.display_ms=25;G.smooth_window=1;
        char last[32]="";int changes=0;size_t width=0;
        for(int t=0;t<=2000;t+=25){
            float value=(float)((t/25)%97)/100.f;
            for(int m=0;m<LJ_METRIC_COUNT;m++)G.hist[m][G.head]=value;
            G.head=(G.head+1)%LJ_METRIC_HISTORY;if(G.count<LJ_METRIC_HISTORY)G.count++;
            display_sample((uint64_t)t);
            const char *l=lj_metrics_display_label(LJ_METRIC_CPU);
            if(strcmp(l,last)){changes++;snprintf(last,32,"%s",l);}
            if(strstr(l,"%")){if(!width)width=strlen(l);assert(strlen(l)==width);}   /* value labels never change width; "—" is a different state */
            if(t>=25)assert(strstr(l,"%"));   /* the first real bucket shows at once, never held behind "—" */
        }
        assert(changes<=2000/(int)lj_metrics_label_ms()+1);
        printf("Metrics label: %d text changes in 2s at %llums hold, constant width %zu PASS\n",changes,(unsigned long long)lj_metrics_label_ms(),width);
    }
    /* Paced view: with the default 100ms repaint the paced snapshot refreshes
     * every 4th bucket and each refresh adds exactly 4 columns. */
    {
        memset(&G,0,sizeof(G));G.display_ms=25;G.smooth_window=1;G.repaint_ms=LJ_METRICS_DISPLAY_REPAINT_MS;
        uint64_t rev=0;int prev_n=0,refreshes=0;
        for(int t=0;t<=2000;t+=25){
            for(int m=0;m<LJ_METRIC_COUNT;m++)G.hist[m][G.head]=(float)((t/25)%97)/100.f;
            G.head=(G.head+1)%LJ_METRIC_HISTORY;if(G.count<LJ_METRIC_HISTORY)G.count++;
            display_sample((uint64_t)t);
            const float *ph=NULL;int pn=lj_metrics_paced_history(LJ_METRIC_CPU,&ph);
            uint64_t r=lj_metrics_paced_revision();
            if(r!=rev){refreshes++;if(t>=100)assert(pn-prev_n==LJ_METRICS_DISPLAY_REPAINT_MS/G.display_ms);assert(t%LJ_METRICS_DISPLAY_REPAINT_MS==0);rev=r;prev_n=pn;}
            else assert(pn==prev_n);
        }
        assert(refreshes==2000/LJ_METRICS_DISPLAY_REPAINT_MS+1);
        printf("Metrics paced: %d refreshes in 2s at %dms, %d columns per step PASS\n",refreshes,LJ_METRICS_DISPLAY_REPAINT_MS,LJ_METRICS_DISPLAY_REPAINT_MS/G.display_ms);
    }
    puts("Metrics display: 81 samples/25ms, 80 completed 25ms means, one pixel column each; NAN PASS");
}
