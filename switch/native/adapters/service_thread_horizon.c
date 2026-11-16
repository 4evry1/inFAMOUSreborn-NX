#include "service_thread_horizon.h"
#include <errno.h>
enum { HZ_IDLE = 0, HZ_RUNNING = 1, HZ_EXITED = 2 };
void ps3_horizon_service_thread_init(ps3_horizon_service_thread *t, size_t stack_bytes) {
    t->entry = NULL;
    t->argument = NULL;
    t->stack_bytes = stack_bytes ? stack_bytes : 0x10000;
    t->wide = 0;
    /* Root (2026-11-12, Decision 78): PS3_NATIVE_GUEST_PRIORITY (59 in the LLE build). Horizon runs the highest
     * priority thread of a core until it blocks, EXCEPT at priority 59 on cores 0..2, where the kernel rotates the
     * threads every 10 ms. At 0x2C a guest thread that spins in guest code kept its core for ever, and every other
     * guest thread of that core stopped in the middle of whatever it was doing (seen as threads "inside" a 1 ms
     * sleep or a mutex unlock for ten minutes: runs r83 and r86). */
#ifndef PS3_NATIVE_GUEST_PRIORITY
#define PS3_NATIVE_GUEST_PRIORITY 0x2C
#endif
    t->priority = PS3_NATIVE_GUEST_PRIORITY;
    t->cpu = -2;          /* default core */
    t->state = HZ_IDLE;
}
static void trampoline(void *p) {
    ps3_horizon_service_thread *t = p;
    t->entry(t->argument);
}
static int start_impl(void *context, void (*entry)(void *), void *argument) {
    ps3_horizon_service_thread *t = context;
    if (!t || !entry || t->state != HZ_IDLE || !t->stack_bytes) return EINVAL;
    t->entry = entry;
    t->argument = argument;
    int spread = 0;
#ifdef PS3_NATIVE_SPREAD_CORES
    /* Root (2026-11-12, Decision 70): a thread without a chosen core takes the next of cores 0..1 in turn (0..2 before Decision 80). Before,
     * every guest PPU thread ran on the process's default core, and the activity profile showed one of them in
     * guest code 98 % of the time there: on a console that thread has a hardware thread of its own, here it took
     * the core every other guest thread needed. A refused core falls back to the default one below. */
    if (t->cpu == -2) {
        static unsigned next_core;
        spread = 1;
        /* Decision 80: cores 0 and 1 only. Core 2 belongs to the SPU threads of the later groups (the movie
         * decoder), which set their core themselves. */
        /* Decision 87: cores 0..2 again. The movie decoder is translated code now (Decision 84) and no longer
         * needs a core to itself; during loading core 2 had nothing else to do. PS3_NATIVE_SPREAD_CORE_COUNT
         * overrides. */
#ifndef PS3_NATIVE_SPREAD_CORE_COUNT
#define PS3_NATIVE_SPREAD_CORE_COUNT 3u
#endif
        t->cpu = (int)(__atomic_fetch_add(&next_core, 1u, __ATOMIC_RELAXED) % PS3_NATIVE_SPREAD_CORE_COUNT);
    }
#endif
    if (R_FAILED(threadCreate(&t->thread, trampoline, t, NULL, t->stack_bytes, t->priority, t->cpu))) {
        /* An explicitly requested core the process may not use must not cost the thread: fall back to the default
         * core once and remember it (cpu = -2 afterwards shows the fallback to whoever reports the record). */
        if (t->cpu == -2 && t->priority == 0x2C) return EAGAIN;
        t->cpu = -2;
        if (R_FAILED(threadCreate(&t->thread, trampoline, t, NULL, t->stack_bytes, t->priority, t->cpu))) {
            if (t->priority == 0x2C) return EAGAIN;
            t->priority = 0x2C;   /* a refused priority must not cost the thread either */
            if (R_FAILED(threadCreate(&t->thread, trampoline, t, NULL, t->stack_bytes, t->priority, t->cpu))) return EAGAIN;
        }
    }
#ifdef PS3_NATIVE_SPREAD_CORES
    /* Root (2026-11-12, Decision 89): a spread thread may run on any of cores 0..2, with the core chosen above as
     * its preferred one. Pinned to one core each, hardware run rv20 had core 2 idle 85 % of the time during loading
     * while the main thread waited about 42 ms per contended lock for its turn on another core. Horizon moves a
     * runnable thread to an idle core its mask allows. Core 3 stays out: it has no time slicing at this priority.
     * A refusal changes nothing (the thread keeps its one core). */
    if ((spread || t->wide) && t->cpu >= 0 && t->cpu <= (t->wide == 2 ? 3 : 2)) {
        uint64_t allowed = 7;
        if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) allowed = 7;
        /* Decision 137: wide == 2 also allows core 3 when the process has it (SPURS job threads; core 3's other thread, the
         * audio SPU, runs at a higher priority there, so it always comes first). */
        (void)svcSetThreadCoreMask(t->thread.handle, t->cpu, (u32)(allowed & (t->wide == 2 ? 0xFu : 7u)));
    }
#else
    (void)spread;
#endif
    if (R_FAILED(threadStart(&t->thread))) {
        threadClose(&t->thread);   /* never ran: no thread exists, resources released */
        return EAGAIN;
    }
    t->state = HZ_RUNNING;
    return 0;
}
static int join_impl(void *context) {
    ps3_horizon_service_thread *t = context;
    if (!t || t->state == HZ_IDLE) return EINVAL;
    if (t->state == HZ_RUNNING) {
        if (R_FAILED(threadWaitForExit(&t->thread))) return EIO;   /* exit unconfirmed: still owned */
        t->state = HZ_EXITED;
    }
    if (R_FAILED(threadClose(&t->thread))) return EIO;             /* exited but handle/stack not released: retryable */
    t->state = HZ_IDLE;
    return 0;
}
ps3_service_thread_port ps3_horizon_service_thread_port(ps3_horizon_service_thread *t) {
    ps3_service_thread_port port = {t, start_impl, join_impl};
    return port;
}
