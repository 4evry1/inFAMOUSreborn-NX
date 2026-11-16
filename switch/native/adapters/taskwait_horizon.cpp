#include "taskwait_horizon.h"
#include <switch.h>
// Root (2026-11-15, Decision 121, console run rv61s): the game's main thread spent 29 % of its time in this pause, waiting for SPU
// jobs, while the five SPURS job threads were ~60 % idle: each wait lasted the whole pause (250 us) after the job had finished. The
// pause now waits on g_taskwait_wake_word with the same ceiling; a guarded write of guest memory (the SPU kernel's completion store
// is one) bumps the word and, when a thread is waiting, wakes it at once. A wake only makes the guest re-test its own predicate, so an
// early or spurious wake changes nothing but timing. -DSTARTUP_TASKWAIT_WAKE=0 gives the plain sleep back.
// Decision 122 (rv62s): one signal per pause, not one per guarded write.
// Decision 123 (rv63s): rv63s still sent 13 k signals a second (the main thread re-polled after every SPU write anywhere), ~5.5 % of every
// SPU thread. A write now wakes the pause only when it overlaps the watched window around the object the guest waits on
// (startup_taskwait_horizon_watch, from ps3_taskwait_begin: the object's 128-byte line and the three after it). Any other completion
// path still ends the pause at its 250 us ceiling. The hook also counts, per writer tag (SPU slot + 1, 0 = PPU/other), the writes that
// overlapped the window while a pause waited, and attributes the END of each wait to the last such writer.
#ifndef STARTUP_TASKWAIT_WAKE
#if defined(__SWITCH__)
#define STARTUP_TASKWAIT_WAKE 1
#else
#define STARTUP_TASKWAIT_WAKE 0   // host stand-in: its switch.h has svcSleepThread only
#endif
#endif
#ifndef STARTUP_TASKWAIT_WATCH_BYTES
#define STARTUP_TASKWAIT_WATCH_BYTES 512u
#endif
extern "C" unsigned ps3_atomic_writer_tag(void) __attribute__((weak));
namespace {
uint32_t g_taskwait_wake_word, g_taskwait_wake_waiters, g_taskwait_wake_pending;
uint32_t g_watch_lo, g_watch_hi;            // [lo, hi) guest window; hi == 0: no watch (every write wakes)
uint32_t g_last_overlap_tag;
}
uint64_t g_startup_taskwait_waits, g_startup_taskwait_woken, g_startup_taskwait_signals;
uint64_t g_startup_taskwait_overlap_by_tag[STARTUP_TASKWAIT_TAGS], g_startup_taskwait_end_by_tag[STARTUP_TASKWAIT_TAGS];
extern "C" void startup_taskwait_horizon_watch(uint32_t object)
{
    const uint32_t lo = object & ~127u;
    __atomic_store_n(&g_watch_lo, lo, __ATOMIC_RELAXED);
    __atomic_store_n(&g_watch_hi, lo + STARTUP_TASKWAIT_WATCH_BYTES, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_overlap_tag, 0u, __ATOMIC_RELAXED);
}
extern "C" void startup_taskwait_horizon_end(void)
{
    unsigned tag = __atomic_load_n(&g_last_overlap_tag, __ATOMIC_RELAXED);
    if (tag >= STARTUP_TASKWAIT_TAGS) tag = STARTUP_TASKWAIT_TAGS - 1u;
    __atomic_fetch_add(&g_startup_taskwait_end_by_tag[tag], 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_watch_hi, 0u, __ATOMIC_RELAXED);
}
extern "C" void startup_taskwait_horizon_wake(uint64_t address, uint64_t length)
{
    if (!__atomic_load_n(&g_taskwait_wake_waiters, __ATOMIC_SEQ_CST)) return;
    const uint32_t hi = __atomic_load_n(&g_watch_hi, __ATOMIC_RELAXED);
    if (hi) {
        const uint32_t lo = __atomic_load_n(&g_watch_lo, __ATOMIC_RELAXED);
        if (!(address < hi && address + length > lo)) return;
        unsigned tag = ps3_atomic_writer_tag ? ps3_atomic_writer_tag() : 0u;
        if (tag >= STARTUP_TASKWAIT_TAGS) tag = STARTUP_TASKWAIT_TAGS - 1u;
        __atomic_fetch_add(&g_startup_taskwait_overlap_by_tag[tag], 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_overlap_tag, tag, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&g_taskwait_wake_word, 1u, __ATOMIC_SEQ_CST);
#if STARTUP_TASKWAIT_WAKE
    if (!__atomic_load_n(&g_taskwait_wake_pending, __ATOMIC_RELAXED) &&
        !__atomic_exchange_n(&g_taskwait_wake_pending, 1u, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_add(&g_startup_taskwait_signals, 1u, __ATOMIC_RELAXED);
        (void)svcSignalToAddress(&g_taskwait_wake_word, SignalType_Signal, 0, -1);
    }
#endif
}
bool startup_taskwait_horizon_wake_enabled() noexcept { return STARTUP_TASKWAIT_WAKE != 0; }
bool startup_taskwait_horizon_yield_once(void *context) noexcept
{
    const auto *config = static_cast<const StartupTaskwaitHorizonConfig *>(context);
    if (!config || config->sleep_ns <= 0 || config->sleep_ns > STARTUP_TASKWAIT_MAX_SLEEP_NS)
        return false;
#if STARTUP_TASKWAIT_WAKE
    __atomic_store_n(&g_taskwait_wake_pending, 0u, __ATOMIC_SEQ_CST);
    const uint32_t seen = __atomic_load_n(&g_taskwait_wake_word, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_taskwait_wake_waiters, 1u, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_startup_taskwait_waits, 1u, __ATOMIC_RELAXED);
    const Result rc = svcWaitForAddress(&g_taskwait_wake_word, ArbitrationType_WaitIfEqual, int32_t(seen), config->sleep_ns);
    __atomic_fetch_sub(&g_taskwait_wake_waiters, 1u, __ATOMIC_SEQ_CST);
    if (R_SUCCEEDED(rc) || __atomic_load_n(&g_taskwait_wake_word, __ATOMIC_RELAXED) != seen)
        __atomic_fetch_add(&g_startup_taskwait_woken, 1u, __ATOMIC_RELAXED);
#else
    svcSleepThread(config->sleep_ns);
#endif
    return true;
}
