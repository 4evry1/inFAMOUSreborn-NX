#ifndef PS3_RUNTIME_LWMUTEX_H
#define PS3_RUNTIME_LWMUTEX_H
#include "../memory/atomic_domain.h"
#include "wait.h"
#include "publication.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PS3_LWMUTEX_FREE UINT32_C(0xffffffff)
#define PS3_LWMUTEX_DEAD UINT32_C(0xfffffffe)
#define PS3_LWMUTEX_RESERVED UINT32_C(0xfffffffd)
enum {
    PS3_LWMUTEX_FIFO = 1,
    PS3_LWMUTEX_PRIORITY = 2,
    PS3_LWMUTEX_RECURSIVE = 0x10,
    PS3_LWMUTEX_NONRECURSIVE = 0x20,
    PS3_LWMUTEX_BYTES = 24
};
typedef enum ps3_lwmutex_status {
    PS3_LWMUTEX_OK = 0,
    PS3_LWMUTEX_INVALID_ARGUMENT,
    PS3_LWMUTEX_UNSUPPORTED,
    PS3_LWMUTEX_ALREADY_EXISTS,
    PS3_LWMUTEX_NOT_FOUND,
    PS3_LWMUTEX_BUSY,
    PS3_LWMUTEX_NOT_OWNER,
    PS3_LWMUTEX_DEADLOCK,
    PS3_LWMUTEX_OVERFLOW,
    PS3_LWMUTEX_RESOURCE_EXHAUSTED,
    PS3_LWMUTEX_TIMED_OUT,
    PS3_LWMUTEX_CANCELLED,
    PS3_LWMUTEX_MEMORY_FAILURE,
    PS3_LWMUTEX_WAIT_FAILURE,
    PS3_LWMUTEX_INCONSISTENT,
    PS3_LWMUTEX_CLEANUP_REQUIRED
} ps3_lwmutex_status;
typedef struct ps3_lwmutex_result {
    ps3_lwmutex_status status;
    ps3_guest_memory_status memory_status;
    ps3_wait_status wait_status;
} ps3_lwmutex_result;
typedef struct ps3_lwmutex_thread {
    uint32_t id;
    int32_t priority;
} ps3_lwmutex_thread;
/* These records are caller storage, private to the registry after init.
 * state zero denotes unused; nonzero records may retain failed cleanup. */
struct ps3_lwmutex_registry;
struct ps3_lwmutex_condition_binding;
struct ps3_lwmutex_condition_wait;
/* ppu-speed-20261113 item 1: lock-free fast path for the uncontended lock/unlock of a guest lwmutex.
 * PS3_LWMUTEX_FAST=0 compiles it out (every operation then behaves exactly as before the change).
 * The fast path is also off at run time until the composition calls ps3_lwmutex_enable_fast_path. */
#ifndef PS3_LWMUTEX_FAST
#define PS3_LWMUTEX_FAST 1
#endif
typedef struct ps3_lwmutex_object {
    uint64_t generation;
    uint32_t address, attributes, queue_id;
    unsigned state, active_operations;
    ps3_atomic_pin pin;
    struct ps3_lwmutex_condition_binding *condition_bindings;
    /* Host pointer to the 24-byte control block while the object is fast-eligible, else NULL (see lwmutex.c). */
    unsigned char *host;
} ps3_lwmutex_object;
typedef struct ps3_lwmutex_waiter {
    size_t object_index;
    uint64_t object_generation, sequence, deadline;
    ps3_lwmutex_thread thread;
    ps3_wait_cancel cancel;
    unsigned state;
    struct ps3_lwmutex_condition_wait *condition_authority;
    struct ps3_lwmutex_condition_binding *condition_binding;
    uint64_t reservation_identity;
    uint32_t saved_recursion;
    unsigned condition_counted;
} ps3_lwmutex_waiter;
/* Host view of guest memory for the fast path (flat guest window). pointer returns a host pointer to
 * [address, address + bytes) when every page of it is committed and may be used with host atomics, else NULL.
 * The pages of a live lwmutex are pinned (ps3_atomic_pin), so a pointer taken at create stays valid until destroy. */
typedef struct ps3_lwmutex_fast_port {
    void *context;
    unsigned char *(*pointer)(void *context, uint32_t address, uint32_t bytes);
} ps3_lwmutex_fast_port;
enum { PS3_LWMUTEX_FAST_SLOTS = 256, PS3_LWMUTEX_FAST_PROBES = 8 };
/* One entry per fast-eligible live mutex: a lock-free index from guest address to its host pointer and the identity
 * (attributes, queue id) the guest control block must still show for the fast path to touch it. Written only under the
 * registry guard, read without it. key = address << 32 | queue_id; 0 empty, 1 removed. One cache line each, so two hot
 * mutexes never share a line. locks/unlocks and the refusal counters are racy diagnostics.
 * why[]: how often the fast path refused and the guarded path ran, by reason (PS3_LWMUTEX_WHY_*). */
enum { PS3_LWMUTEX_WHY_CANCEL = 0, PS3_LWMUTEX_WHY_IDENTITY, PS3_LWMUTEX_WHY_STICKY, PS3_LWMUTEX_WHY_HELD, PS3_LWMUTEX_WHY_WAITERS,
       PS3_LWMUTEX_WHY_OTHER, PS3_LWMUTEX_WHY_COUNT };
typedef struct __attribute__((aligned(64))) ps3_lwmutex_fast_entry {
    uint64_t key;
    unsigned char *host;
    uint32_t attributes, pad;
    uint64_t locks, unlocks;
    uint32_t why[PS3_LWMUTEX_WHY_COUNT];
} ps3_lwmutex_fast_entry;
typedef struct ps3_lwmutex_registry {
    ps3_atomic_domain *domain;
    ps3_instance_guard guard;
    ps3_wait_port wait;
    ps3_lwmutex_object *objects;
    size_t object_capacity;
    ps3_lwmutex_waiter *waiters;
    size_t waiter_capacity;
    size_t condition_binding_capacity, condition_binding_count;
    uint64_t next_sequence, next_reservation;
    uint32_t next_queue_id;
    unsigned initialized;
    ps3_lwmutex_fast_port fast;
    ps3_lwmutex_fast_entry fast_table[PS3_LWMUTEX_FAST_SLOTS];
} ps3_lwmutex_registry;
enum {
    PS3_LWMUTEX_CONDITION_DORMANT = 1,
    PS3_LWMUTEX_CONDITION_ELIGIBLE = 2,
    PS3_LWMUTEX_CONDITION_POISONED = 3
};
/* Active tokens are noncopyable/nonmovable, private to this service, and must
 * outlive retained cleanup. Registry membership validates their stable address. */
typedef struct ps3_lwmutex_condition_binding {
    ps3_lwmutex_registry *registry;
    struct ps3_lwmutex_condition_binding *next;
    size_t object_index;
    uint64_t object_generation;
    uint32_t address, attributes;
    size_t reservations;
    unsigned retained;
} ps3_lwmutex_condition_binding;
typedef struct ps3_lwmutex_condition_wait {
    ps3_lwmutex_registry *registry;
    size_t waiter_index;
    uint64_t object_generation, reservation_identity;
    unsigned state; /* Diagnostic mirror; registry record is authority. */
} ps3_lwmutex_condition_wait;
/* Fresh zero tokens; all ports use canonical contexts and distinct primitives.
 * Known registry/domain context aliases are rejected at init. No callback may
 * reenter this registry. Only an operation's owner invokes its cancel token.
 * Each release/promote/abort writes at most one full 24-byte image; reacquire
 * needs one free report slot (acquire OR abort); cleanup is a bounded batch.
 * No guest write occurs without report credit. Fatal abort still poisons host
 * admission state when credit is absent; counted cleanup is retained. */
ps3_lwmutex_result ps3_lwmutex_condition_bind(ps3_lwmutex_registry *, uint32_t,
    ps3_lwmutex_condition_binding *);
ps3_lwmutex_result ps3_lwmutex_condition_unbind(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_binding *);
ps3_lwmutex_result ps3_lwmutex_condition_release(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_binding *, ps3_lwmutex_thread,
    ps3_lwmutex_condition_wait *, ps3_sync_publication_report *);
ps3_lwmutex_result ps3_lwmutex_condition_promote(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_wait *, ps3_sync_publication_report *);
/* One guarded batch: distinct DORMANT authorities, complete report/sequence
 * headroom checked before mutation. Burns the complete admission interval;
 * provider failure commits only *promoted supplied-order prefix. Later tokens
 * remain dormant. Caller output/array/report must not alias live records. */
ps3_lwmutex_result ps3_lwmutex_condition_promote_many(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_wait *const *, size_t, size_t *, ps3_sync_publication_report *);
ps3_lwmutex_result ps3_lwmutex_condition_reacquire(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_wait *, ps3_wait_cancel, ps3_sync_publication_report *);
ps3_lwmutex_result ps3_lwmutex_condition_abort(ps3_lwmutex_registry *,
    ps3_lwmutex_condition_wait *, ps3_sync_publication_report *);
ps3_lwmutex_result ps3_lwmutex_condition_cleanup(ps3_lwmutex_registry *,
    ps3_sync_publication_report *);
/* Fresh/quiescent registry/storage only. Ports, records and guest window must
 * not alias. Registry guard is distinct from domain/backing guards. All guest
 * control-block users must use this HLE family; generated inline/kernel paths
 * are unsupported. One guest identity cannot run concurrent operations.
 * Priorities are immutable during a wait; dynamic priority changes unsupported.
 * Composition disables host cancellation/nonlocal unwinding for each operation;
 * cancellation uses only the cooperative token. It retains storage until all
 * operations join and cleanup succeeds. Host output buffers may not alias live
 * records or shared guest backing. No public call destroys provider lifetimes. */
/* Compatibility initializer disables condition associations (capacity zero).
 * New consumers explicitly budget bindings independently of mutex waiters.
 * Intrusive binding storage remains stable until unbound; membership walks
 * visit no more than condition_binding_capacity caller-owned records. */
ps3_lwmutex_result ps3_lwmutex_init_with_conditions(
    ps3_lwmutex_registry *registry, ps3_atomic_domain *domain,
    ps3_instance_guard guard, ps3_wait_port wait,
    ps3_lwmutex_object *objects, size_t object_capacity,
    ps3_lwmutex_waiter *waiters, size_t waiter_capacity,
    size_t condition_binding_capacity);
ps3_lwmutex_result ps3_lwmutex_init(
    ps3_lwmutex_registry *registry, ps3_atomic_domain *domain,
    ps3_instance_guard guard, ps3_wait_port wait,
    ps3_lwmutex_object *objects, size_t object_capacity,
    ps3_lwmutex_waiter *waiters, size_t waiter_capacity);
/* Turn the lock-free fast path on for mutexes created from now on (UNSUPPORTED when compiled with PS3_LWMUTEX_FAST=0 or
 * on a big-endian host). Call right after init, before any worker runs. With it on, EVERY change of a fast-eligible
 * mutex's owner/waiter pair is a host compare-and-swap (the fast path and the guarded path alike), so
 * the registry guard no longer excludes the fast path; the guarded paths retry when the pair changed under them.
 * A mutex whose 128-byte line already held an SPU reservation at create time is never fast-eligible. */
ps3_lwmutex_result ps3_lwmutex_enable_fast_path(ps3_lwmutex_registry *registry, ps3_lwmutex_fast_port port);
/* Racy totals of the fast path (diagnostics): operations served without the guard, and the ones that fell back.
 * registry NULL: the registry that enabled the fast path last (a process has one). */
void ps3_lwmutex_fast_totals(const ps3_lwmutex_registry *registry, uint64_t *locks, uint64_t *unlocks, uint64_t *fallbacks);
/* The fallbacks of ps3_lwmutex_fast_totals by reason, PS3_LWMUTEX_WHY_COUNT entries: cancel requested; control block does not show the registered
 * identity or recursion != 0 (a recursive mutex held by the caller shows up here on unlock); the line is sticky (SPU reservation); lock: held by
 * another thread (or by the caller: relock) / unlock: not the owner; waiters != 0 (the guarded path hands over and wakes); anything else. */
void ps3_lwmutex_fast_reasons(const ps3_lwmutex_registry *registry, uint64_t reasons[PS3_LWMUTEX_WHY_COUNT]);
ps3_lwmutex_result ps3_lwmutex_create(
    ps3_lwmutex_registry *registry, uint32_t address,
    uint32_t protocol, uint32_t recursive);
ps3_lwmutex_result ps3_lwmutex_lock(
    ps3_lwmutex_registry *registry, uint32_t address,
    ps3_lwmutex_thread thread, uint64_t timeout_us, ps3_wait_cancel cancel);
ps3_lwmutex_result ps3_lwmutex_try_lock(
    ps3_lwmutex_registry *registry, uint32_t address, ps3_lwmutex_thread thread);
ps3_lwmutex_result ps3_lwmutex_unlock(
    ps3_lwmutex_registry *registry, uint32_t address, ps3_lwmutex_thread thread);
ps3_lwmutex_result ps3_lwmutex_destroy(
    ps3_lwmutex_registry *registry, uint32_t address);
/* Quiescent best effort; held/live-waiting objects return BUSY. Successful
 * retirements stay retired. Abandoned waiter removal and failed pin release
 * retain records for retry. Never destroys supplied ports or backing. */
ps3_lwmutex_result ps3_lwmutex_cleanup(ps3_lwmutex_registry *registry);
/* Root (2026-11-12, Decision 86): lock-time diagnostics, one process-wide set. Times are the wait port's
 * nanoseconds. guard_* is time spent getting the registry guard; contended_* is time from having the guard to
 * owning a mutex that somebody else held. top[] keeps the mutexes with the most contended time. */
enum { PS3_LWMUTEX_DEBUG_TOP = 6, PS3_LWMUTEX_DEBUG_PENDING = 32 };
struct ps3_lwmutex_debug_pending { uint32_t address, owner, waiter; uint64_t since; };
typedef struct ps3_lwmutex_debug_counters {
    uint64_t lock_calls, unlock_calls, notify_skipped;
    uint64_t guard_ns, guard_max_ns, guard_over_1ms;
    uint64_t lock_contended, contended_ns, contended_max_ns;
    uint32_t max_address, max_owner, max_waiter;
    struct { uint32_t address, last_owner, last_waiter; uint64_t count, ns; } top[PS3_LWMUTEX_DEBUG_TOP];
    struct ps3_lwmutex_debug_pending pending[PS3_LWMUTEX_DEBUG_PENDING];   /* address 0 = free; owner as seen when the wait began */
} ps3_lwmutex_debug_counters;
extern ps3_lwmutex_debug_counters g_ps3_lwmutex_debug;

#ifdef __cplusplus
}
#endif
#endif
