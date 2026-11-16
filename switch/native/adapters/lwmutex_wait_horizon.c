#include "lwmutex_wait_horizon.h"
#include "barging_lock_horizon.h"
#include <stdio.h>

enum { NANOSECONDS_PER_SECOND = 1000000000, CONDVAR_TIMEOUT_RESULT = 0xea01 };

static void lock_registry(void *context)
{
    ps3_barging_lock(context);
}

static void unlock_registry(void *context)
{
    ps3_barging_unlock(context);
}

static int is_cancelled(ps3_wait_cancel cancel)
{
    return cancel.is_cancelled && cancel.is_cancelled(cancel.context);
}

static ps3_wait_status monotonic_now(void *context, uint64_t *nanoseconds)
{
    ps3_horizon_lwmutex_wait *wait = context;
    if (!wait || !wait->initialized || !wait->frequency || !nanoseconds) return PS3_WAIT_FAILURE;
    uint64_t ticks = armGetSystemTick();
    if (ticks < wait->origin_ticks) return PS3_WAIT_FAILURE;
    __uint128_t elapsed = (__uint128_t)(ticks - wait->origin_ticks) * NANOSECONDS_PER_SECOND;
    elapsed /= wait->frequency;
    if (elapsed >= UINT64_MAX) return PS3_WAIT_FAILURE;
    *nanoseconds = (uint64_t)elapsed;
    return PS3_WAIT_READY;
}

static _Noreturn void stop_owned_diagnostic(Result kernel_result)
{
    printf("LWMUTEX_WAIT fatal_kernel_result=0x%x guard_state_unknown=true\n", kernel_result);
    fflush(stdout);
    svcExitProcess();
    __builtin_trap();
}

static ps3_wait_status wait_once(
    void *context, const ps3_instance_guard *guard,
    uint64_t deadline, ps3_wait_cancel cancel)
{
    ps3_horizon_lwmutex_wait *wait = context;
    if (!wait || !wait->initialized || !guard || guard->context != &wait->mutex ||
        guard->lock != lock_registry || guard->unlock != unlock_registry) return PS3_WAIT_FAILURE;
    if (is_cancelled(cancel)) return PS3_WAIT_CANCELLED;
    uint64_t now;
    if (monotonic_now(wait, &now) != PS3_WAIT_READY) return PS3_WAIT_FAILURE;
    if (deadline != UINT64_MAX && now >= deadline) return PS3_WAIT_TIMEOUT;
    uint64_t duration = wait->poll_nanoseconds;
    if (deadline != UINT64_MAX && deadline - now < duration) duration = deadline - now;
    /* Decision 88: the guard is a barging lock, so the sleep has its own mutex. The wake-up count is read with
     * the guard held; a notifier raises it before it takes sleep_mutex, so either this thread sees the new
     * count and does not sleep, or it is already waiting when the notifier wakes the condition. */
    const uint32_t sequence = __atomic_load_n(&wait->sequence, __ATOMIC_SEQ_CST);
    ps3_barging_unlock(&wait->mutex);
    Result status = 0;
    mutexLock(&wait->sleep_mutex);
    if (__atomic_load_n(&wait->sequence, __ATOMIC_SEQ_CST) == sequence)
        status = condvarWaitTimeout(&wait->condition, &wait->sleep_mutex, duration);
    /* Exact installed SDK header documents 0xea01; its archive object calls
     * mutexLock on that result. Other kernel errors have no proven postcondition. */
    if (status && R_VALUE(status) != CONDVAR_TIMEOUT_RESULT) stop_owned_diagnostic(status);
    mutexUnlock(&wait->sleep_mutex);
    ps3_barging_lock(&wait->mutex);
    if (is_cancelled(cancel)) return PS3_WAIT_CANCELLED;
    if (monotonic_now(wait, &now) != PS3_WAIT_READY) return PS3_WAIT_FAILURE;
    if (deadline != UINT64_MAX && now >= deadline) return PS3_WAIT_TIMEOUT;
    return PS3_WAIT_READY;
}

static void notify_all(void *context)
{
    ps3_horizon_lwmutex_wait *wait = context;
    /* Installed SDK uses the void svcSignalProcessWideKey and returns zero. */
    __atomic_fetch_add(&wait->sequence, 1u, __ATOMIC_SEQ_CST);
    mutexLock(&wait->sleep_mutex);
    condvarWakeAll(&wait->condition);
    mutexUnlock(&wait->sleep_mutex);
}

ps3_wait_status ps3_horizon_lwmutex_wait_init(
    ps3_horizon_lwmutex_wait *wait, uint64_t poll_nanoseconds)
{
    if (!wait || !poll_nanoseconds || poll_nanoseconds > INT64_MAX) return PS3_WAIT_FAILURE;
    uint64_t frequency = armGetSystemTickFreq();
    if (!frequency) return PS3_WAIT_FAILURE;
    *wait = (ps3_horizon_lwmutex_wait){.origin_ticks = armGetSystemTick(),
        .frequency = frequency, .poll_nanoseconds = poll_nanoseconds, .initialized = 1};
    mutexInit(&wait->mutex);
    mutexInit(&wait->sleep_mutex);
    condvarInit(&wait->condition);
    return PS3_WAIT_READY;
}

ps3_instance_guard ps3_horizon_lwmutex_wait_guard(ps3_horizon_lwmutex_wait *wait)
{
    return (ps3_instance_guard){wait ? &wait->mutex : NULL, lock_registry, unlock_registry};
}

ps3_wait_port ps3_horizon_lwmutex_wait_port(ps3_horizon_lwmutex_wait *wait)
{
    return (ps3_wait_port){wait, monotonic_now, wait_once, notify_all};
}
