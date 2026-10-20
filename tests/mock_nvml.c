/* Minimal ABI-compatible NVML fixture: no NVIDIA SDK or hardware required. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail_util, fail_mem, reverse, shutdowns, handles;
void mock_config(int u, int m, int r) { fail_util=u; fail_mem=m; reverse=r; }
int mock_shutdowns(void) { return shutdowns; }
int mock_handles(void) { return handles; }
int nvmlInit_v2(void) { return getenv("MOCK_INIT_FAIL") ? 1 : 0; }
int nvmlShutdown(void) { shutdowns++; return 0; }
#ifndef MOCK_MISSING_COUNT
int nvmlDeviceGetCount_v2(unsigned int *n) {
    if (getenv("MOCK_COUNT_FAIL")) return 1;
    *n=getenv("MOCK_COUNT") ? (unsigned int)atoi(getenv("MOCK_COUNT")) : 2; return 0;
}
#endif
int nvmlDeviceGetHandleByIndex_v2(unsigned int i, void **d) {
    handles++;
    if (getenv("MOCK_HANDLE_FAIL") && i==0) return 1;
    *d=(void *)(uintptr_t)(reverse ? 2-i : i+1); return 0;
}
int nvmlDeviceGetUUID(void *d, char *b, unsigned int n) {
    if (getenv("MOCK_UUID_FAIL")) return 1;
    snprintf(b,n,"GPU-test-%u",(unsigned int)(uintptr_t)d); return 0;
}
int nvmlDeviceGetName(void *d, char *b, unsigned int n) {
    snprintf(b,n,"%s",(uintptr_t)d==1 ? "RTX 3060" : "Tesla V100"); return 0;
}
int nvmlDeviceGetUtilizationRates(void *d, void *out) {
    struct { unsigned int gpu, memory; } *u=out;
    if (fail_util && (uintptr_t)d==1) return 1;
    u->gpu=(uintptr_t)d==1?25:90; u->memory=0; return 0;
}
int nvmlDeviceGetMemoryInfo(void *d, void *out) {
    struct { unsigned long long total, free, used; } *m=out;
    if (fail_mem && (uintptr_t)d==2) return 1;
    m->total=((uintptr_t)d==1?8ULL:32ULL)<<30;
    m->used=((uintptr_t)d==1?2ULL:24ULL)<<30;
    m->free=m->total-m->used; return 0;
}
/* Temperature intentionally absent: optional symbols must stay optional. */
