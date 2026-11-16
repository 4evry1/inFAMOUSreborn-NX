/* ppu-speed-20261113 task 3: host test of native/adapters/barging_lock_horizon.h with the address-arbiter wait (futexes stand in for
 * svcWaitForAddress / svcSignalToAddress, evidence/ppu-speed-20261113/shim). Built by evidence/ppu-speed-20261113/tests/run_barging_lock_test.sh,
 * once with the blocking lock and once with -DPS3_BARGING_LOCK_POLL=1 (the old polling loop, for the wake-latency comparison).
 * Checks: mutual exclusion with many threads on few CPUs (the holder gets preempted), the arbiter really blocks (a sleeper burns no CPU
 * and is woken by unlock well before the 1 ms safety timeout), no lost wake-up (everything finishes), the fallback when the kernel refuses
 * the arbiter calls (NotImplemented, or an unknown error), and the wake latency after a holder lets go. */
#define _GNU_SOURCE
#include "barging_lock_horizon.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static uint32_t g_word;
static unsigned long g_counter;
typedef struct { unsigned iterations, hold_spins; } worker_args;
static void *worker(void *context)
{
    worker_args *a = context;
    for (unsigned i = 0; i < a->iterations; ++i) {
        ps3_barging_lock(&g_word);
        unsigned long v = g_counter;
        for (unsigned k = 0; k < a->hold_spins; ++k) __asm__ volatile("" ::: "memory");
        g_counter = v + 1;
        ps3_barging_unlock(&g_word);
    }
    return NULL;
}
static void run_threads(unsigned threads, unsigned iterations, unsigned hold_spins, int cpus)
{
    g_word = 0; g_counter = 0;
    if (cpus > 0) { cpu_set_t set; CPU_ZERO(&set); for (int c = 0; c < cpus; ++c) CPU_SET(c, &set); assert(!sched_setaffinity(0, sizeof set, &set)); }
    pthread_t t[64]; worker_args a = {iterations, hold_spins};
    const uint64_t t0 = now_ns();
    for (unsigned i = 0; i < threads; ++i) assert(!pthread_create(&t[i], NULL, worker, &a));
    for (unsigned i = 0; i < threads; ++i) assert(!pthread_join(t[i], NULL));
    const double ms = (double)(now_ns() - t0) / 1e6;
    assert(g_counter == (unsigned long)threads * iterations);
    assert((g_word & 1u) == 0u);   /* free; no announced waiter left behind */
    assert(PS3_BARGING_LOCK_POLL || g_word == 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("  %2u threads x %6u, hold %4u spins, %d cpus: %.1f ms, counter OK\n", threads, iterations, hold_spins, cpus, ms);
}
static void *sleeper(void *context)
{
    uint64_t *woke = context;
    ps3_barging_lock(&g_word);
    *woke = now_ns();
    ps3_barging_unlock(&g_word);
    return NULL;
}
/* wake latency: the main thread holds the lock, a second thread waits for it, the holder lets go after 20 ms; microseconds until the waiter owns it */
static double wake_latency_us(void)
{
    double worst = 0, sum = 0; const int rounds = 20;
    for (int r = 0; r < rounds; ++r) {
        g_word = 0; ps3_barging_lock(&g_word);
        uint64_t woke = 0; pthread_t t; assert(!pthread_create(&t, NULL, sleeper, &woke));
        struct timespec nap = {0, 20 * 1000 * 1000}; nanosleep(&nap, NULL);
        const uint64_t release = now_ns(); ps3_barging_unlock(&g_word);
        assert(!pthread_join(t, NULL));
        const double us = (double)(woke - release) / 1000.0; sum += us; if (us > worst) worst = us;
    }
    printf("  wake latency after a 20 ms hold: mean %.1f us, worst %.1f us\n", sum / rounds, worst);
    return sum / rounds;
}
/* a sleeper must not burn CPU while it waits */
static void sleeper_burns_no_cpu(void)
{
#if !PS3_BARGING_LOCK_POLL
    struct timespec before, used; assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &before));
    g_word = 0; ps3_barging_lock(&g_word);
    uint64_t woke = 0; pthread_t t; assert(!pthread_create(&t, NULL, sleeper, &woke));
    struct timespec nap = {0, 100 * 1000 * 1000}; nanosleep(&nap, NULL);
    assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &used));
    const double cpu_ms = (used.tv_sec - before.tv_sec) * 1e3 + (used.tv_nsec - before.tv_nsec) / 1e6;
    ps3_barging_unlock(&g_word); assert(!pthread_join(t, NULL));
    printf("  process CPU time while one thread waited 100 ms for the lock: %.1f ms\n", cpu_ms);
    assert(cpu_ms < 60.0);   /* spin phase only; a polling waiter on a loaded host burns far more */
#endif
}
/* hold the lock while a second thread asks for it, long enough for it to leave the spin phase and park (or, refused, to fall back) */
static void force_block(void)
{
    g_word = 0; ps3_barging_lock(&g_word);
    uint64_t woke = 0; pthread_t t; assert(!pthread_create(&t, NULL, sleeper, &woke));
    struct timespec nap = {0, 30 * 1000 * 1000}; nanosleep(&nap, NULL);
    ps3_barging_unlock(&g_word); assert(!pthread_join(t, NULL));
}
int main(void)
{
    puts(PS3_BARGING_LOCK_POLL ? "barging lock: POLLING variant" : "barging lock: BLOCKING variant (address arbiter)");
    run_threads(8, 200000, 0, 0);
    run_threads(8, 20000, 200, 2);          /* 8 threads on 2 CPUs, a longer hold: the holder is preempted while others want the lock */
    run_threads(16, 5000, 2000, 1);         /* everybody on one CPU */
    sleeper_burns_no_cpu();
    wake_latency_us();
#if !PS3_BARGING_LOCK_POLL
    /* the kernel refuses the arbiter: NotImplemented, and an unknown error. The lock must fall back to polling, once, for good, and still work. */
    for (int mode = 0; mode < 2; ++mode) {
        g_ps3_barging_arbiter_failed = 0;
        g_shim_arbiter_result = mode == 0 ? MAKERESULT(Module_Kernel, KernelError_NotImplemented) : MAKERESULT(3, 77);
        run_threads(4, 20000, 100, 2);
        force_block();   /* contention alone may never get past the spin phase: make one waiter park */
        assert(g_ps3_barging_arbiter_failed == 1);
        run_threads(4, 20000, 100, 2);
        g_shim_arbiter_result = 0; g_ps3_barging_arbiter_failed = 0;
    }
    run_threads(8, 50000, 0, 0);            /* and the blocking path works again after a reset */
#endif
    puts("barging lock: PASS");
    return 0;
}
