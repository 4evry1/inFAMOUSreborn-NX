/*
 * SPURS kernel-worker startup state.
 *
 * A host thread may start after its creator has stopped waiting for it.  The
 * creator abandons that worker before another one is assigned the same part,
 * and a late starter must observe ABANDONED and leave.  Keeping this small
 * state machine separate lets host tests exercise the race without starting
 * the full PS3 runtime.
 */
#ifndef PS3RECOMP_SPURS_KERNEL_STARTUP_H
#define PS3RECOMP_SPURS_KERNEL_STARTUP_H

#include <stdint.h>

enum {
    PS3_SPURS_KERNEL_PENDING = 0,
    PS3_SPURS_KERNEL_STARTED = 1,
    PS3_SPURS_KERNEL_LIVE = 2,
    PS3_SPURS_KERNEL_ABANDONED = 3
};

static inline int ps3_spurs_kernel_claim_start(volatile int32_t *state)
{
    int32_t expected = PS3_SPURS_KERNEL_PENDING;
    return __atomic_compare_exchange_n(state, &expected, PS3_SPURS_KERNEL_STARTED,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int ps3_spurs_kernel_abandon(volatile int32_t *state)
{
    int32_t expected = PS3_SPURS_KERNEL_PENDING;
    return __atomic_compare_exchange_n(state, &expected, PS3_SPURS_KERNEL_ABANDONED,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int ps3_spurs_kernel_is_started(volatile int32_t *state)
{
    const int32_t value = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    return value == PS3_SPURS_KERNEL_STARTED || value == PS3_SPURS_KERNEL_LIVE;
}

static inline int ps3_spurs_kernel_is_live(volatile int32_t *state)
{
    return __atomic_load_n(state, __ATOMIC_ACQUIRE) == PS3_SPURS_KERNEL_LIVE;
}

static inline void ps3_spurs_kernel_mark_live(volatile int32_t *state)
{
    __atomic_store_n(state, PS3_SPURS_KERNEL_LIVE, __ATOMIC_RELEASE);
}

#endif
