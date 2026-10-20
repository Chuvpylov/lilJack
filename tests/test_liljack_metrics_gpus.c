#define _GNU_SOURCE
#include <assert.h>
#include "../liljack_app/c_metrics.c"
static void near(float a, float b) { assert(fabsf(a-b)<0.0001f); }
int main(int argc, char **argv) {
    assert(argc==3);
    setenv("LJ_METRICS_NVML",argv[1],1);
    setenv("LJ_METRICS_INTERVAL_MS","0",1);
    setenv("LJ_METRICS_SMOOTH_MS","0",1);
    void *lib=dlopen(argv[1],RTLD_NOW); assert(lib);
    void (*config)(int,int,int)=(void (*)(int,int,int))dlsym(lib,"mock_config");
    int (*shutdowns)(void)=(int (*)(void))dlsym(lib,"mock_shutdowns");
    int (*handles)(void)=(int (*)(void))dlsym(lib,"mock_handles");
    assert(lj_metrics_init()==0 && lj_metrics_gpu_count()==2);
    assert(isnan(lj_metrics_gpu(0)->utilization));
    assert(!strcmp(lj_metrics_gpu(1)->uuid,"GPU-test-2"));
    assert(strstr(lj_metrics_gpu(1)->name,"V100"));
    lj_metrics_poll();
    near(lj_metrics_gpu(0)->utilization,.25f);
    near(lj_metrics_gpu(1)->utilization,.9f);
    near(lj_metrics_gpu(1)->memory_fraction,.75f);
    assert(lj_metrics_gpu(1)->memory_total==(32ULL<<30));
    assert(strstr(lj_metrics_gpu_label(1,1),"24.00/32.00 GiB"));
    lj_sample legacy; lj_metrics_current(&legacy); near(legacy.gpu,.25f);
    config(1,1,1); /* index reorder after init must not move histories/identity */
    lj_metrics_poll();
    assert(handles()==2); /* handles and metadata are cached, no rediscovery per poll */
    assert(isnan(lj_metrics_gpu(0)->utilization));
    near(lj_metrics_gpu(0)->memory_fraction,.25f);
    near(lj_metrics_gpu(1)->utilization,.9f);
    assert(isnan(lj_metrics_gpu(1)->memory_fraction));
    assert(strstr(lj_metrics_gpu_label(1,1),"unavailable"));
    const float *h=NULL;
    assert(lj_metrics_gpu_history(0,0,&h)==2); near(h[0],.25f); assert(isnan(h[1]));
    /* Deterministic display clock: no sleeps or real GPU timing. */
    memset(G.part_bucket,0,sizeof G.part_bucket); memset(G.bucket,0,sizeof G.bucket);
    G.display_count=0; display_sample(1000); display_sample(1050);
    assert(lj_metrics_gpu_display_history(1,0,&h)==1); near(h[0],.9f);
    assert(lj_metrics_gpu_display_history(1,1,&h)==1 && isnan(h[0]));
    config(0,0,0);
    for(int i=0;i<300;i++) lj_metrics_poll();
    assert(lj_metrics_gpu_history(1,0,&h)==256); near(h[0],.9f); near(h[255],.9f);
    assert(!lj_metrics_gpu(-1) && !lj_metrics_gpu(2));
    assert(!lj_metrics_gpu_history(0,2,&h) && !lj_metrics_gpu_history(0,0,NULL));
    lj_metrics_close(); assert(shutdowns()==1 && lj_metrics_gpu_count()==0);
    setenv("MOCK_COUNT","1",1); assert(!lj_metrics_init());
    assert(lj_metrics_gpu_count()==1); lj_metrics_poll(); lj_metrics_current(&legacy); near(legacy.gpu,.25f); lj_metrics_close();
    setenv("MOCK_COUNT","0",1); assert(!lj_metrics_init() && !lj_metrics_gpu_count());
    lj_metrics_poll(); lj_metrics_current(&legacy); assert(isnan(legacy.gpu)); lj_metrics_close();
    setenv("MOCK_COUNT","20",1); assert(!lj_metrics_init());
    assert(lj_metrics_gpu_count()==LJ_METRIC_GPUS_MAX); lj_metrics_close();
    unsetenv("MOCK_COUNT");
    setenv("MOCK_HANDLE_FAIL","1",1); assert(!lj_metrics_init() && lj_metrics_gpu_count()==1);
    assert(!strcmp(lj_metrics_gpu(0)->uuid,"GPU-test-2")); lj_metrics_close(); unsetenv("MOCK_HANDLE_FAIL");
    const char *failures[]={"MOCK_INIT_FAIL","MOCK_COUNT_FAIL","MOCK_UUID_FAIL"};
    for(int i=0;i<3;i++) { setenv(failures[i],"1",1); assert(!lj_metrics_init() && !lj_metrics_gpu_count()); lj_metrics_close(); unsetenv(failures[i]); }
    setenv("LJ_METRICS_NVML",argv[2],1); assert(!lj_metrics_init() && !lj_metrics_gpu_count()); lj_metrics_close();
    dlclose(lib);
    puts("metrics-gpus: identity, two cards, partial failures, cached handles, histories, wrap, single/zero/capped enumeration, NVML lifecycle PASS");
}
