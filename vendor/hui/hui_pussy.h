/*
 * hui_pussy.h — PUSSY: Platform Unified Scheduler SYstem
 *
 * Minimal cooperative task scheduler. No threads, no OS dependencies.
 * Works on bare-metal (RP2350, AVR) and Linux/macOS/Windows.
 * Caller drives execution by calling hui_pussy_tick(now_us) each loop iteration.
 *
 * Features:
 *   - Fixed pool, no heap (default 32 tasks — override HUI_PUSSY_MAX_TASKS)
 *   - 5 priority levels: REALTIME → IDLE
 *   - Periodic tasks and one-shots
 *   - Drift-free scheduling (next = intended_time + period, not actual_time + period)
 *   - Automatic catch-up prevention (skips if > 1 period behind)
 *   - POSIX clock built-in; Windows and bare-metal also supported
 *   - Optional jitter stats (#define HUI_PUSSY_STATS before include)
 *
 * Usage — one-file style:
 *   #define HUI_PUSSY_IMPLEMENTATION
 *   #include "hui_pussy.h"
 *
 *   hui_task_id t = hui_task_add(my_fn, NULL, 16667, HUI_PRIO_NORMAL); // ~60Hz
 *   while (running) {
 *       hui_pussy_tick(hui_pussy_now_us());
 *       // ... rest of loop ...
 *   }
 *
 * Bare-metal clock override:
 *   #define HUI_PUSSY_CLOCK_FN my_timer_us   // uint64_t my_timer_us(void)
 *   #define HUI_PUSSY_IMPLEMENTATION
 *   #include "hui_pussy.h"
 */

#ifndef HUI_PUSSY_H
#define HUI_PUSSY_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Priority levels ---- */

typedef enum {
    HUI_PRIO_REALTIME = 0,   /* run first, no yield */
    HUI_PRIO_HIGH     = 1,
    HUI_PRIO_NORMAL   = 2,   /* default */
    HUI_PRIO_LOW      = 3,
    HUI_PRIO_IDLE     = 4,   /* run last, only when nothing else is due */
    HUI_PRIO__COUNT   = 5,
} hui_priority;

/* ---- Task ---- */

typedef void (*hui_task_fn)(void *arg);

#ifndef HUI_PUSSY_MAX_TASKS
#  define HUI_PUSSY_MAX_TASKS 32
#endif

typedef struct {
    hui_task_fn  fn;
    void        *arg;
    uint64_t     period_us;   /* 0 = one-shot (auto-removed after first run) */
    uint64_t     next_us;     /* absolute time of next execution */
    hui_priority priority;
    bool         active;
#ifdef HUI_PUSSY_STATS
    uint64_t     run_count;
    uint64_t     jitter_us_min;
    uint64_t     jitter_us_max;
    uint64_t     jitter_us_sum;
#endif
} hui_task;

typedef int hui_task_id;
#define HUI_TASK_INVALID (-1)

/* ---- API ---- */

/* Add a periodic task. period_us = 0 makes it a one-shot (runs once then removed).
 * Starts after one period from now. Returns task_id or HUI_TASK_INVALID if full. */
hui_task_id hui_task_add(hui_task_fn fn, void *arg,
                         uint64_t period_us, hui_priority prio);

/* Add a one-shot task to run at a specific absolute time. */
hui_task_id hui_task_once(hui_task_fn fn, void *arg,
                          uint64_t run_at_us, hui_priority prio);

/* Remove a task. Safe to call with HUI_TASK_INVALID. */
void hui_task_remove(hui_task_id id);

/* Reset next execution to now + period (use after long sleep/pause to avoid burst). */
void hui_task_reset(hui_task_id id, uint64_t now_us);

/* Get task pointer (NULL if invalid or inactive). */
hui_task *hui_task_get(hui_task_id id);

/* Run all tasks due at now_us, highest priority first.
 * Call every frame or loop iteration. Returns number of tasks executed. */
int hui_pussy_tick(uint64_t now_us);

/* Portable microsecond clock.
 * Selects POSIX / Windows / user-defined based on platform and defines. */
uint64_t hui_pussy_now_us(void);

#ifdef HUI_PUSSY_STATS
/* Print jitter statistics for all active tasks to stdout. */
void hui_pussy_print_stats(void);
#endif

/* ---- Implementation ---- */

#ifdef HUI_PUSSY_IMPLEMENTATION

static hui_task hui__pussy_pool[HUI_PUSSY_MAX_TASKS];

hui_task_id hui_task_add(hui_task_fn fn, void *arg,
                         uint64_t period_us, hui_priority prio) {
    uint64_t now = hui_pussy_now_us();
    for (int i = 0; i < HUI_PUSSY_MAX_TASKS; i++) {
        if (hui__pussy_pool[i].active) continue;
        hui_task *t = &hui__pussy_pool[i];
        memset(t, 0, sizeof(*t));
        t->fn        = fn;
        t->arg       = arg;
        t->period_us = period_us;
        t->next_us   = now + (period_us > 0 ? period_us : 0);
        t->priority  = prio;
        t->active    = true;
#ifdef HUI_PUSSY_STATS
        t->jitter_us_min = UINT64_MAX;
#endif
        return (hui_task_id)i;
    }
    return HUI_TASK_INVALID;
}

hui_task_id hui_task_once(hui_task_fn fn, void *arg,
                          uint64_t run_at_us, hui_priority prio) {
    for (int i = 0; i < HUI_PUSSY_MAX_TASKS; i++) {
        if (hui__pussy_pool[i].active) continue;
        hui_task *t = &hui__pussy_pool[i];
        memset(t, 0, sizeof(*t));
        t->fn        = fn;
        t->arg       = arg;
        t->period_us = 0;
        t->next_us   = run_at_us;
        t->priority  = prio;
        t->active    = true;
#ifdef HUI_PUSSY_STATS
        t->jitter_us_min = UINT64_MAX;
#endif
        return (hui_task_id)i;
    }
    return HUI_TASK_INVALID;
}

void hui_task_remove(hui_task_id id) {
    if (id >= 0 && id < HUI_PUSSY_MAX_TASKS)
        hui__pussy_pool[id].active = false;
}

void hui_task_reset(hui_task_id id, uint64_t now_us) {
    if (id < 0 || id >= HUI_PUSSY_MAX_TASKS) return;
    hui_task *t = &hui__pussy_pool[id];
    if (t->active)
        t->next_us = now_us + t->period_us;
}

hui_task *hui_task_get(hui_task_id id) {
    if (id >= 0 && id < HUI_PUSSY_MAX_TASKS && hui__pussy_pool[id].active)
        return &hui__pussy_pool[id];
    return NULL;
}

int hui_pussy_tick(uint64_t now_us) {
    int ran = 0;
    /* Execute in priority order: REALTIME first, IDLE last */
    for (int prio = 0; prio < HUI_PRIO__COUNT; prio++) {
        for (int i = 0; i < HUI_PUSSY_MAX_TASKS; i++) {
            hui_task *t = &hui__pussy_pool[i];
            if (!t->active || (int)t->priority != prio || t->next_us > now_us)
                continue;
#ifdef HUI_PUSSY_STATS
            {
                uint64_t jitter = now_us - t->next_us;
                if (jitter < t->jitter_us_min) t->jitter_us_min = jitter;
                if (jitter > t->jitter_us_max) t->jitter_us_max = jitter;
                t->jitter_us_sum += jitter;
                t->run_count++;
            }
#endif
            t->fn(t->arg);
            ran++;

            if (t->period_us == 0) {
                t->active = false;  /* one-shot */
            } else {
                /* Drift-free: advance from intended time, not actual time */
                t->next_us += t->period_us;
                /* Catch-up prevention: if > 1 period behind, skip to now+period */
                if (t->next_us + t->period_us < now_us)
                    t->next_us = now_us + t->period_us;
            }
        }
    }
    return ran;
}

/* ---- Clock ---- */

#if defined(HUI_PUSSY_CLOCK_FN)
uint64_t hui_pussy_now_us(void) { return HUI_PUSSY_CLOCK_FN(); }
#elif defined(__linux__) || defined(__APPLE__) || defined(_POSIX_C_SOURCE)
#include <time.h>
uint64_t hui_pussy_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
}
#elif defined(_WIN32)
#include <windows.h>
uint64_t hui_pussy_now_us(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1000000LL / freq.QuadPart);
}
#else
/* Bare-metal stub — define HUI_PUSSY_CLOCK_FN to your own uint64_t fn(void) */
uint64_t hui_pussy_now_us(void) { return 0; }
#endif

#ifdef HUI_PUSSY_STATS
#include <stdio.h>
void hui_pussy_print_stats(void) {
    printf("%-4s %-8s %-10s %-12s %-12s %-12s\n",
           "ID", "PRIO", "RUNS", "JITTER_MIN", "JITTER_MAX", "JITTER_AVG");
    for (int i = 0; i < HUI_PUSSY_MAX_TASKS; i++) {
        hui_task *t = &hui__pussy_pool[i];
        if (!t->active) continue;
#ifdef HUI_PUSSY_STATS
        if (t->run_count == 0) continue;
        uint64_t avg = t->jitter_us_sum / t->run_count;
        uint64_t mn  = (t->jitter_us_min == UINT64_MAX) ? 0 : t->jitter_us_min;
        printf("%-4d %-8d %-10llu %-12llu %-12llu %-12llu\n",
               i, (int)t->priority,
               (unsigned long long)t->run_count,
               (unsigned long long)mn,
               (unsigned long long)t->jitter_us_max,
               (unsigned long long)avg);
#endif
    }
}
#endif /* HUI_PUSSY_STATS */

#endif /* HUI_PUSSY_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* HUI_PUSSY_H */
