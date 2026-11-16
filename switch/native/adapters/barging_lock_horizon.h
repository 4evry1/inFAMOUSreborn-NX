#ifndef PS3_BARGING_LOCK_HORIZON_H
#define PS3_BARGING_LOCK_HORIZON_H
/* Root (2026-11-12, Decision 88): a guard for short critical sections that does not hand ownership over.
 *
 * libnx mutexUnlock gives a contended mutex straight to the first waiter (svcArbitrateUnlock). Horizon does not
 * preempt a running thread for a woken one of the same priority, and the guest threads share cores at one
 * priority, so the new owner often cannot run for up to a 10 ms time slice while every other thread that needs
 * the guard queues behind it. Measured: 36 s spent getting the lwmutex registry guard in a 95 s run (55875 lock
 * calls, 3995 waits over 1 ms, longest 81 ms).
 *
 * Here whoever runs takes the lock. A waiter spins briefly (the holder is usually on another core), then yields
 * its core (the holder may share it), then sleeps 50 us at a time (the holder may have a lower priority, which a
 * yield never runs). The word is a libnx Mutex's storage (uint32_t), used as 0 = free, 1 = held; never mix with
 * mutexLock on the same word. */
#include <switch.h>
#include <stdint.h>
/* ppu-speed-20261113 (task 3): the guard now BLOCKS instead of sleeping in 50 us steps. Run rv56s: 21.6 % of the guest main thread's time
 * was svcSleepThread inside this lock (profile: the atomic-domain guard, callers lean_store -> ps3_atomic_write, lwarx, stwcx,
 * lle_sre_call_export's ps3_atomic_read, lv2 syscalls). A waiter that lost the race slept until its next 50 us tick even when the
 * holder let go a microsecond later, and the yield/sleep(0) rounds before it burn the core.
 *
 * Word: bit 0 = held, bits 31..1 = the number of waiters that have announced themselves (they are parked on the word or about to be). lock: one CAS
 * 0 -> 1; else a short spin (yield hints: the holder is usually on another core and brief) trying to take the word whenever bit 0 is clear;
 * then announce (word += 2) and wait on the address with svcWaitForAddress(WaitIfEqual, that value) (1 ms safety timeout, so a lost wake-up
 * costs at most that), withdraw (word -= 2) and try again. unlock: clear bit 0; only if waiters are announced, svcSignalToAddress wakes one. So
 * an uncontended lock/unlock is two atomic operations and no system call, and a contended unlock calls the kernel only while somebody waits.
 * Still a BARGING lock: unlock hands nothing over, the woken thread competes with whoever runs (Decision 88: a hand-over to a thread that
 * cannot run for a time slice convoys everyone). If the kernel refuses the address-arbiter calls (firmware before 6.0:
 * KernelError_NotImplemented, or any unexpected result) the lock falls back for the rest of the process to the old polling loop (spin 200,
 * sleep(0) x 16, then 50 us sleeps). -DPS3_BARGING_LOCK_POLL compiles only the old loop. The word is a libnx Mutex's storage (uint32_t); never
 * mix with mutexLock on the same word. */
#ifndef PS3_BARGING_LOCK_POLL
#define PS3_BARGING_LOCK_POLL 0
#endif
/* per translation unit (a refusal is found again, once, by each one that uses the lock) */
static volatile uint32_t g_ps3_barging_arbiter_failed;
static inline void ps3_barging_lock_poll(uint32_t *word)
{
    for (unsigned round = 0;; ++round) {
        uint32_t seen = __atomic_load_n(word, __ATOMIC_RELAXED);   /* bits above 0 are announced waiters of the blocking protocol: leave them */
        if (!(seen & 1u) && __atomic_compare_exchange_n(word, &seen, seen | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (round < 200u) { __asm__ volatile("yield"); continue; }
        if (round < 216u) { svcSleepThread(0); continue; }     /* yield, same core */
        svcSleepThread(50000);
    }
}
static inline void ps3_barging_lock(void *word_storage)
{
    uint32_t *word = (uint32_t *)word_storage;
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
#if PS3_BARGING_LOCK_POLL
    ps3_barging_lock_poll(word);
#else
    for (unsigned round = 0; round < 300u; ++round) {   /* about 1-2 us on a Cortex-A57: longer than a typical guard hold */
        uint32_t seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (!(seen & 1u) && __atomic_compare_exchange_n(word, &seen, seen | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        __asm__ volatile("yield");
    }
    while (!__atomic_load_n(&g_ps3_barging_arbiter_failed, __ATOMIC_RELAXED)) {
        uint32_t seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (!(seen & 1u)) {
            if (__atomic_compare_exchange_n(word, &seen, seen | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        (void)__atomic_fetch_add(word, 2u, __ATOMIC_ACQ_REL);                       /* announce: one more waiter */
        seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        Result rc = 0;
        if (seen & 1u) rc = svcWaitForAddress(word, ArbitrationType_WaitIfEqual, (int64_t)seen, 1000000);   /* the word changed meanwhile: InvalidState, loop */
        else if (__atomic_compare_exchange_n(word, &seen, (seen - 2u) | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;   /* it was released: take it and withdraw at once */
        (void)__atomic_fetch_sub(word, 2u, __ATOMIC_ACQ_REL);                       /* withdraw */
        if (R_SUCCEEDED(rc)) continue;
        const unsigned description = R_DESCRIPTION(rc);
        if (R_MODULE(rc) == Module_Kernel && (description == KernelError_TimedOut || description == KernelError_InvalidState)) continue;   /* timeout / word changed */
        __atomic_store_n(&g_ps3_barging_arbiter_failed, 1u, __ATOMIC_RELAXED);     /* no address arbiter here: poll from now on */
    }
    ps3_barging_lock_poll(word);
#endif
}
static inline void ps3_barging_unlock(void *word_storage)
{
    uint32_t *word = (uint32_t *)word_storage;
#if PS3_BARGING_LOCK_POLL
    __atomic_store_n(word, 0u, __ATOMIC_RELEASE);
#else
    if (__atomic_fetch_and(word, ~1u, __ATOMIC_RELEASE) > 1u) (void)svcSignalToAddress(word, SignalType_Signal, 0, 1);   /* waiters announced */
#endif
}
#endif
