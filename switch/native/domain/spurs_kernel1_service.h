#ifndef NATIVE_SPURS_KERNEL1_SERVICE_H
#define NATIVE_SPURS_KERNEL1_SERVICE_H
#include "spurs_policy_execution_port.h"
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#include "service_thread.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native HLE of the SPURS kernel-1 startup -> system service -> initial idle
 * path (KERNEL-IDLE-CONTRACT s2, steps K1-K13) for 1..8 contexts on one owned
 * worker. Reference-intent reconstruction (I1/I3) of libsre, NOT a binary
 * oracle: any state outside the supported idle subset stops (fail closed) on
 * the snapshot BEFORE the conditional store that would leave idle. Value-
 * identical reservation ops (K6/K8/K9) still commit, as the reference intent's
 * void reservation_op does. Blocking at K13 is a real reservation-loss wait
 * (atomic_domain wait-any); cancellation actively wakes it.
 * Ownership: owners and LS buffers are borrowed from the SPU-thread records and
 * must outlive ps3_spurs_k1_cancel_and_join(); the domain and wait port must be
 * the same ones attached to each other. No callbacks: the worker publishes one
 * terminal record (seq-cst) and the bridge polls ps3_spurs_k1_poll_stop() at its
 * pre-effect boundary. Linearization: no further guest effect is issued by the
 * worker after it decides STOP, and the PPU issues none after it observes it; an
 * effect already executing concurrently is not retroactively stopped. */
enum { PS3_SPURS_K1_MAX_SPUS = 8, PS3_SPURS_K1_LS_BYTES = 0x40000 };
typedef enum {
    PS3_SPURS_K1_STOP_NONE = 0,
    PS3_SPURS_K1_STOP_ALREADY_INIT_HALT,
    PS3_SPURS_K1_STOP_MESSAGE_WAKE,
    PS3_SPURS_K1_STOP_DISPATCH_REQUIRED,
    PS3_SPURS_K1_STOP_EXIT_REQUESTED,
    PS3_SPURS_K1_STOP_TRACE_BUFFER_NONZERO,
    PS3_SPURS_K1_STOP_SERVICE_REQUEST,
    PS3_SPURS_K1_STOP_UNSUPPORTED_STATE,
    PS3_SPURS_K1_STOP_PROVIDER_FAILURE,
    PS3_SPURS_K1_STOP_WORKER_START_FAILED
} ps3_spurs_k1_stop_reason;
typedef enum {
    PS3_SPURS_K1_CTX_NOT_STARTED = 0,
    PS3_SPURS_K1_CTX_RUNNING,
    PS3_SPURS_K1_CTX_BLOCKED_LR,
    PS3_SPURS_K1_CTX_STOPPED,
    PS3_SPURS_K1_CTX_CANCELLED
} ps3_spurs_k1_context_state;
typedef enum {
    PS3_SPURS_K1_PHASE_CREATED = 0,
    PS3_SPURS_K1_PHASE_RUNNING,
    PS3_SPURS_K1_PHASE_QUIESCENT,
    PS3_SPURS_K1_PHASE_STOPPED,
    PS3_SPURS_K1_PHASE_CANCELLED
} ps3_spurs_k1_phase;
typedef struct ps3_spurs_k1_config {
    ps3_atomic_domain *domain;
    ps3_notify_port wait_port;
    ps3_service_thread_port worker;
    uint32_t instance;            /* B: 128-byte aligned CellSpurs EA */
    uint32_t spu_count;           /* nSpus, 1..8 */
    const ps3_atomic_owner *owners; /* spu_count, distinct, live */
    unsigned char *const *local_stores; /* spu_count x PS3_SPURS_K1_LS_BYTES */
    const uint8_t *start_order;   /* optional permutation of 0..spu_count-1 */
} ps3_spurs_k1_config;
typedef struct ps3_spurs_k1_context {
    ps3_atomic_owner owner;
    unsigned char *ls;
    uint32_t spu;
    uint32_t state;               /* ps3_spurs_k1_context_state, atomic */
    uint64_t idle_publications;   /* successful K12 commits, atomic */
    uint64_t wake_evaluations;    /* K13 re-evaluations after loss, atomic */
    uint64_t conditional_failures;/* PUTLLC retries, atomic */
    uint32_t steps_completed;     /* bitmask of K1..K13 executed, atomic */
    uint32_t regs[6];             /* HLE r0,r1,r3,r4,r5,pc after K2 */
    uint8_t workload_priority[16], workload_unique_id[16];
    unsigned workload_update_requested;
    uint8_t selected_contention[16], selected_pending[16];
    uint32_t selected_workload, selected_poll_status;
    unsigned selection_is_poll, policy_active;
} ps3_spurs_k1_context;
typedef struct ps3_spurs_k1_stop {
    uint32_t reason, spu, step;
    uint64_t snapshot_digest;
} ps3_spurs_k1_stop;
typedef struct ps3_spurs_k1_service {
    ps3_spurs_k1_config config;
    ps3_spurs_k1_context contexts[PS3_SPURS_K1_MAX_SPUS];
    uint8_t order[PS3_SPURS_K1_MAX_SPUS];
    ps3_notify_cancel cancel;
    uint64_t phase;               /* futex word, atomic */
    uint64_t settled;             /* futex word: bumped when all latches clear, atomic */
    ps3_spurs_k1_stop stop;       /* written once before phase=STOPPED */
    unsigned worker_started, worker_joined, initialized;
    ps3_spurs_policy_execution_port policy_execution;
} ps3_spurs_k1_service;

typedef enum { PS3_SPURS_K1_OK = 0, PS3_SPURS_K1_INVALID, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_CANCELLED } ps3_spurs_k1_status;
/* Install an execution adapter before start; its context must outlive join. */
ps3_spurs_k1_status ps3_spurs_k1_set_policy_execution(ps3_spurs_k1_service *, const ps3_spurs_policy_execution_port *);
ps3_spurs_k1_status ps3_spurs_k1_init(ps3_spurs_k1_service *, const ps3_spurs_k1_config *);
/* Storage contract: *s must be zero-filled or a previously initialised service. init
 * refuses (INVALID, nothing touched) while a started worker is not confirmed joined, snapshots
 * `config` (it may alias s->config) and validates every input before mutating *s.
 * group_start: start the worker and return OK once every context has executed K1..K12 and
 * blocked holding its L0 reservation after a final rescan found no pending loss latch AT THAT
 * TIME. This is initial-convergence readiness under the supported no-external-writer window,
 * not a globally latch-free instant: concurrent guest stores may latch a new loss during or
 * after publication, which the worker then re-evaluates. Otherwise the service stopped or
 * was cancelled. */
ps3_spurs_k1_status ps3_spurs_k1_start(ps3_spurs_k1_service *, ps3_spurs_k1_stop *);
/* Race-free pre-effect poll; nonzero when terminal STOP is published. */
int ps3_spurs_k1_poll_stop(const ps3_spurs_k1_service *, ps3_spurs_k1_stop *);
uint64_t ps3_spurs_k1_settled_count(const ps3_spurs_k1_service *);
/* Wait until the settled counter exceeds `after` or the service terminates. */
ps3_spurs_k1_phase ps3_spurs_k1_wait_settled_after(ps3_spurs_k1_service *, uint64_t after, uint64_t deadline_ns);
ps3_spurs_k1_phase ps3_spurs_k1_phase_of(const ps3_spurs_k1_service *);
ps3_spurs_k1_context_state ps3_spurs_k1_context_state_of(const ps3_spurs_k1_service *, uint32_t spu);
uint64_t ps3_spurs_k1_counter(const ps3_spurs_k1_service *, uint32_t spu, int which);
enum { PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS, PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS, PS3_SPURS_K1_COUNT_CONDITIONAL_FAILURES, PS3_SPURS_K1_COUNT_STEPS };
/* Idempotent teardown: cancel, wake, join; running contexts end CANCELLED.
 * Returns 0 only when no worker is outstanding. A nonzero return means the adapter failed to
 * join: worker_joined stays 0, nothing is released, and the call may be retried. Borrowed
 * owners/LS may be released/freed only after it returns 0. */
int ps3_spurs_k1_cancel_and_join(ps3_spurs_k1_service *);
#ifdef __cplusplus
}
#endif
#endif
