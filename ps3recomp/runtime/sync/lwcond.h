#ifndef PS3_RUNTIME_LWCOND_H
#define PS3_RUNTIME_LWCOND_H
#include "lwmutex.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_LWCOND_BYTES=8, PS3_LWCOND_NAME_BYTES=8, PS3_LWCOND_WAIT_PUBLICATION_MAX=3 };
typedef enum ps3_lwcond_status {
    PS3_LWCOND_OK=0, PS3_LWCOND_INVALID_ARGUMENT, PS3_LWCOND_UNSUPPORTED,
    PS3_LWCOND_ALREADY_EXISTS, PS3_LWCOND_NOT_FOUND, PS3_LWCOND_BUSY,
    PS3_LWCOND_NOT_OWNER, PS3_LWCOND_TARGET_NOT_FOUND, PS3_LWCOND_TARGET_NOT_WAITING,
    PS3_LWCOND_RESOURCE_EXHAUSTED, PS3_LWCOND_TIMED_OUT, PS3_LWCOND_CANCELLED,
    PS3_LWCOND_STOPPED, PS3_LWCOND_INCONSISTENT, PS3_LWCOND_MEMORY_FAILURE,
    PS3_LWCOND_WAIT_FAILURE, PS3_LWCOND_MUTEX_FAILURE, PS3_LWCOND_THREAD_FAILURE,
    PS3_LWCOND_CLEANUP_REQUIRED
} ps3_lwcond_status;
typedef enum ps3_lwcond_outcome {
    PS3_LWCOND_NOT_WAITED=0, PS3_LWCOND_QUEUED, PS3_LWCOND_SELECTED,
    PS3_LWCOND_EXPIRED, PS3_LWCOND_WAIT_CANCELLED, PS3_LWCOND_FAILED
} ps3_lwcond_outcome;
typedef enum ps3_lwcond_thread_status {
    PS3_LWCOND_THREAD_OK=0, PS3_LWCOND_THREAD_QUERY_FAILURE
} ps3_lwcond_thread_status;
typedef struct ps3_lwcond_thread_existence {
    void *context;
    ps3_lwcond_thread_status (*query)(void *, uint32_t, unsigned *);
} ps3_lwcond_thread_existence;
typedef struct ps3_lwcond_result {
    ps3_lwcond_status status;
    ps3_lwcond_outcome condition_outcome;
    ps3_guest_memory_status memory_status;
    ps3_wait_status wait_status;
    ps3_lwmutex_status mutex_status;
    unsigned mutex_reacquired, cleanup_retained, must_not_resume;
} ps3_lwcond_result;
typedef struct ps3_lwcond_report {
    ps3_sync_publication_report publications;
    size_t selected_count;
} ps3_lwcond_report;
/* Caller storage, private after init. Active bindings/tokens cannot move. */
typedef struct ps3_lwcond_object {
    uint64_t generation;
    uint32_t address, mutex_address, queue_id;
    unsigned char name[PS3_LWCOND_NAME_BYTES];
    ps3_atomic_pin pin;
    ps3_lwmutex_condition_binding binding;
    size_t active_waiters;
    unsigned state, has_pin;
} ps3_lwcond_object;
typedef struct ps3_lwcond_waiter {
    size_t object_index;
    uint64_t object_generation, sequence, deadline;
    ps3_lwmutex_thread thread;
    ps3_lwmutex_condition_wait reservation;
    ps3_lwcond_outcome outcome;
    unsigned state;
} ps3_lwcond_waiter;
typedef struct ps3_lwcond_registry {
    ps3_lwmutex_registry *mutexes;
    ps3_instance_guard guard;
    ps3_wait_port wait;
    ps3_lwcond_thread_existence threads;
    ps3_lwcond_object *objects;
    size_t object_capacity;
    ps3_lwcond_waiter *waiters;
    size_t waiter_capacity;
    uint64_t next_sequence;
    uint32_t next_queue_id;
    unsigned initialized;
} ps3_lwcond_registry;
/* Exactly one condition registry per mutex registry. Fresh/quiescent caller
 * storage and normal-returning, nonreentrant ports.
 * Canonical guard contexts represent distinct underlying primitives. Condition
 * guard and paired wait adapter differ from mutex/domain guards. One identity
 * cannot operate concurrently. All guest users use this HLE family. Storage,
 * callbacks and backing survive join AND successful cleanup. Only a wait owner
 * invokes its own cancel/stop callbacks; cancel tokens are never retained.
 * Reports are nonaliasing operation outputs, count starts at zero; reserve
 * credit before mutation: create/destroy/signal/to=1, all=waiter_capacity,
 * wait=3, cleanup=128 per batch. Reports list successful writes only; never
 * replay them. selected_count is the completed prefix, even on fatal failure.
 * Wait ownership flag is set only after successful full-depth restoration;
 * pre-release error leaves ownership unchanged, not newly attested. */
ps3_lwcond_result ps3_lwcond_init(ps3_lwcond_registry *registry, ps3_lwmutex_registry *mutexes,
    ps3_instance_guard guard, ps3_wait_port wait, ps3_lwcond_thread_existence threads,
    ps3_lwcond_object *objects, size_t object_capacity,
    ps3_lwcond_waiter *waiters, size_t waiter_capacity);
ps3_lwcond_result ps3_lwcond_create(ps3_lwcond_registry *registry, uint32_t condition_address,
    uint32_t mutex_address, const unsigned char name[PS3_LWCOND_NAME_BYTES], ps3_lwcond_report *report);
ps3_lwcond_result ps3_lwcond_destroy(ps3_lwcond_registry *registry,
    uint32_t condition_address, ps3_lwcond_report *report);
ps3_lwcond_result ps3_lwcond_wait(ps3_lwcond_registry *registry, uint32_t condition_address,
    ps3_lwmutex_thread thread, uint64_t timeout_us, ps3_wait_cancel cancel,
    ps3_wait_cancel instance_stop, ps3_lwcond_report *report);
ps3_lwcond_result ps3_lwcond_signal(ps3_lwcond_registry *registry,
    uint32_t condition_address, ps3_lwcond_report *report);
ps3_lwcond_result ps3_lwcond_signal_all(ps3_lwcond_registry *registry,
    uint32_t condition_address, ps3_lwcond_report *report);
ps3_lwcond_result ps3_lwcond_signal_to(ps3_lwcond_registry *registry,
    uint32_t condition_address, uint64_t target_thread_id, ps3_lwcond_report *report);
/* Quiescent only. BUSY while any live call restores; poisoned reservations are
 * repaired with actual publication reporting before objects/bindings retire.
 * A capacity-limited successful prefix is retained for a later fresh report. */
ps3_lwcond_result ps3_lwcond_cleanup(ps3_lwcond_registry *registry, ps3_lwcond_report *report);
#ifdef __cplusplus
}
#endif
#endif
