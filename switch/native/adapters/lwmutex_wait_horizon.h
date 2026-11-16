#ifndef PS3_LWMUTEX_WAIT_HORIZON_H
#define PS3_LWMUTEX_WAIT_HORIZON_H
#include "../../../ps3recomp/runtime/sync/wait.h"
#include <switch.h>

typedef struct ps3_horizon_lwmutex_wait {
    Mutex mutex;
    CondVar condition;
    uint64_t origin_ticks, frequency, poll_nanoseconds;
    unsigned initialized;
    /* Decision 88: `mutex` is a barging lock word now (barging_lock_horizon.h). Sleepers wait on `condition`
     * with `sleep_mutex`; `sequence` counts wake-ups so a wake-up between dropping the guard and sleeping is
     * not lost. */
    Mutex sleep_mutex;
    uint32_t sequence;
} ps3_horizon_lwmutex_wait;
/* Fresh/quiescent storage only. Context owns the registry mutex and condition.
 * Composition joins every user before releasing it. Unexpected kernel errors
 * terminate only the owned diagnostic process: SDK reacquisition is verified
 * for success/timeout, not arbitrary errors. No false held-guard error returns.
 * This diagnostic policy is not a general game runtime shutdown mechanism. */
ps3_wait_status ps3_horizon_lwmutex_wait_init(
    ps3_horizon_lwmutex_wait *wait, uint64_t poll_nanoseconds);
ps3_instance_guard ps3_horizon_lwmutex_wait_guard(ps3_horizon_lwmutex_wait *wait);
ps3_wait_port ps3_horizon_lwmutex_wait_port(ps3_horizon_lwmutex_wait *wait);
#endif
