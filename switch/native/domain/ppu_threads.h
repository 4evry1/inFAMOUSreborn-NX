#ifndef NATIVE_PPU_THREADS_H
#define NATIVE_PPU_THREADS_H
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/sync/guard.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#include "../../../ps3recomp/runtime/memory/user_memory.h"
#include "service_thread.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native library SERVICE threads (no guest code). IDs 0x01000000 + serial,
 * never reused [POL]. Each service charges its stack against the shared user
 * memory quota (_sys_ppu_thread_create pinned ENOMEM) and runs its body on an
 * injected host thread. Priority policy [POL/R-PRIO]: a service whose priority
 * is numerically lower (higher) than the creator's runs its prefix
 * synchronously: create returns only after the body really blocks (it set its
 * blocked word inside a real blocking primitive) or exits. join() waits for the
 * host thread, releases the stack charge and retires the record. */
enum { PS3_PPU_ACTIVITY_RUNNING = 0, PS3_PPU_ACTIVITY_BLOCKED = 1, PS3_PPU_ACTIVITY_EXITED = 2 };
/* PS3_NATIVE_LLE_PROFILE (build-wide, see adapters/startup_capacity_policy.h): 4 SPURS records + 8 child records. */
#ifdef PS3_NATIVE_LLE_PROFILE
enum { PS3_PPU_SERVICE_CAP = 68 };   /* 4 + 64 (root 2026-11-11: the game asked for a 25th thread) */
#else
enum { PS3_PPU_SERVICE_CAP = 7 };
#endif
enum { PS3_PPU_THREAD_BASE = 0x01000000u,   /* 2 SPURS helper + 2 handler + 3 child records; keep equal to adapters/startup_capacity_policy.h */ PS3_PPU_STACK_GRANULE = 4096,
       /* actual mapped regions of a guest-storage service are whole user-allocator granules */
       PS3_PPU_GUEST_REGION_GRANULE = 0x10000 };
enum { PS3_PPU_OK = 0, PS3_PPU_EAGAIN = 0x80010001u, PS3_PPU_EINVAL = 0x80010002u, PS3_PPU_ENOMEM = 0x80010004u,
       PS3_PPU_ESRCH = 0x80010005u, PS3_PPU_EBUSY = 0x8001000Au, PS3_PPU_PROVIDER_FAILURE = 0xFFFF0003u,
       PS3_PPU_UNSUPPORTED = 0xFFFF0004u,
       /* create failure AFTER the service started: *ref IS valid and charged; the caller must
        * stop the body and join before freeing anything the body borrows. */
       PS3_PPU_OWNED_FAILURE = 0xFFFF0005u };
/* Typed handle: ids are never reused, so a stale handle can never reach a recycled slot. */
typedef struct ps3_ppu_ref { uint32_t slot, id; } ps3_ppu_ref;
/* RESERVED and CLEANUP_PENDING are appended so FREE/RUNNING/EXITED keep their numeric values.
 * CLEANUP_PENDING: no host thread remains to join (joined, or never created); only the storage release
 * (quota uncharge, or the mapped allocation's free) failed and the record, ref and storage are retained
 * until a later join retries exactly that. */
typedef enum { PS3_PPU_SERVICE_FREE = 0, PS3_PPU_SERVICE_RUNNING, PS3_PPU_SERVICE_EXITED,
               PS3_PPU_SERVICE_RESERVED, PS3_PPU_SERVICE_CLEANUP_PENDING } ps3_ppu_service_life;
/* How a record owns its resources: quota-only (address-less charge token; the original create/reserve) or ONE real
 * user-memory allocation holding the stack region followed by the TLS region (reserve_guest). */
typedef enum { PS3_PPU_STORAGE_QUOTA_ONLY = 0, PS3_PPU_STORAGE_GUEST_ALLOCATION } ps3_ppu_storage_mode;
struct ps3_ppu_service;
typedef void (*ps3_ppu_service_body)(struct ps3_ppu_service *self, void *argument);
typedef struct ps3_ppu_service {
    uint32_t id, priority, kind;
    uint64_t stack_bytes;
    ps3_user_memory_charge_token stack;   /* QUOTA_ONLY storage only; zero for a guest allocation (the allocation itself is the charge) */
    uint32_t storage_mode;                /* ps3_ppu_storage_mode */
    uint32_t allocation_address;          /* GUEST_ALLOCATION: base of the one allocation (stack region first) */
    uint64_t allocation_bytes, stack_region_bytes, tls_region_bytes;
    ps3_service_thread_port thread;
    ps3_ppu_service_body body;
    void *argument;
    uint64_t life;      /* ps3_ppu_service_life */
    /* futex word: 0 running, 1 inside a real blocking primitive (written by that
     * primitive under its own guard), 2 exited. Pass &activity as the blocked word. */
    uint64_t activity;
    uint32_t exit_code;
    struct ps3_ppu_threads *owner;
    unsigned joining;   /* guarded: exactly one joiner proceeds */
    unsigned starting;  /* guarded: start() owns the record from RESERVED->RUNNING through the prefix wait */
    unsigned host_join_complete; /* guarded: the host join succeeded (or no host thread exists); never join twice */
} ps3_ppu_service;
typedef struct ps3_ppu_threads {
    ps3_instance_guard guard;
    ps3_notify_port port;
    ps3_user_memory *memory;
    uint32_t serial, capacity_limit;
    uint32_t main_id;       /* ACTUAL admitted main PPU thread id (cpu.thread_id); 0 = not adopted */
    ps3_ppu_service services[PS3_PPU_SERVICE_CAP];
    unsigned initialized;
} ps3_ppu_threads;
/* Storage contract: zeroed, constructed once; a second init is refused (EINVAL). */
uint32_t ps3_ppu_threads_init(ps3_ppu_threads *, ps3_instance_guard, ps3_notify_port, ps3_user_memory *);
/* L5: the service id namespace is unified with the main thread. The composition adopts the REAL
 * admitted main id (cpu.thread_id, never a guessed value) once, before the first service exists; every
 * later service id skips it, so a service id can never equal the main id. Creation is refused (EINVAL)
 * until a main id is adopted. EINVAL: zero id, uninitialised registry, or a different id than the one
 * already adopted; EBUSY: a service id was already allocated (reservation would be retroactive).
 * Re-adopting the same id is an idempotent OK. main_id returns 0 when nothing is adopted. */
uint32_t ps3_ppu_threads_adopt_main(ps3_ppu_threads *, uint32_t main_id);
uint32_t ps3_ppu_threads_main_id(ps3_ppu_threads *);
void ps3_ppu_threads_limit(ps3_ppu_threads *, uint32_t live_capacity);
/* Native policy [POL/R-PRIO]: only a service of strictly higher priority (numerically lower)
 * than the creator is supported; it runs synchronously and create returns only after the body
 * really blocked or exited. Any other priority is refused (UNSUPPORTED) before charging or
 * starting anything. On OK, *ref is live. On EAGAIN/ENOMEM/EINVAL/UNSUPPORTED nothing is charged
 * or retained and *ref is untouched. On OWNED_FAILURE see above; it also covers a failed start whose
 * pre-start cleanup (quota release) failed: *ref is published for a CLEANUP_PENDING record with no host
 * thread, and the caller's usual stop+join only retries the quota release. create == reserve + start.
 * Failed-start cleanup is one internal guarded operation that inspects the exact record before releasing it: an
 * unstarted RESERVED record is released (original failure, *ref untouched); if that release fails it becomes
 * CLEANUP_PENDING (+ host join already complete) and OWNED_FAILURE publishes *ref; a record another actor has
 * meanwhile started (RUNNING/starting/EXITED) keeps its real state and OWNED_FAILURE publishes its live *ref for the
 * owner's cancel+join; a record already retired or recycled publishes no ref and returns EAGAIN. */
uint32_t ps3_ppu_service_create(ps3_ppu_threads *, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                uint64_t stack_size, ps3_service_thread_port thread, ps3_ppu_service_body body,
                                void *argument, ps3_ppu_ref *ref);
/* Reserve/start lifecycle (create == reserve + start, kept as the compatibility wrapper).
 * reserve takes create's arguments and performs create's validation, quota, capacity and id
 * checks in the same precedence, charges exactly once, publishes a RESERVED record and *ref, and
 * starts nothing: the body never runs. The id is consumed forever, even if the reservation is
 * abandoned; a reservation counts against live capacity and quota. On any failure nothing is
 * charged or retained and *ref is untouched. The caller may publish the id and finish the child's
 * parameters before start.
 * start(ref): RESERVED -> RUNNING with the retained thread port/body/argument, then waits for the
 * same real activity != RUNNING condition as create (no synthetic BLOCKED). OK: the body is
 * blocked in a real primitive or exited. EAGAIN: the adapter failed to start; the record stays
 * RESERVED (quota and ref intact) for abandon or retry. OWNED_FAILURE: wait-port failure; the
 * service is live and charged, the caller must stop the body and join. ESRCH: stale ref. EINVAL:
 * not RESERVED. EBUSY: another start is using the record (start/abandon/join are serialized
 * through a guarded in-progress flag spanning thread.start and the prefix wait; no effects).
 * abandon(ref): releases an UNSTARTED RESERVED record and its quota; no host join. ESRCH: stale or
 * already abandoned; EINVAL: RUNNING/EXITED (never released here, use join); EBUSY: start in
 * progress. If the quota release fails the record stays RESERVED (still owned) and
 * PROVIDER_FAILURE is returned; retry is allowed. join of a RESERVED record is EINVAL. */
uint32_t ps3_ppu_service_reserve(ps3_ppu_threads *, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                 uint64_t stack_size, ps3_service_thread_port thread, ps3_ppu_service_body body,
                                 void *argument, ps3_ppu_ref *ref);
uint32_t ps3_ppu_service_start(ps3_ppu_threads *, ps3_ppu_ref);
/* start without the creator's prefix wait: RESERVED -> RUNNING, the host thread is started, and the call returns as
 * soon as thread.start succeeded. The body may still be running, blocked or already exited. Same checks and codes as
 * ps3_ppu_service_start (ESRCH, EBUSY, EINVAL, EAGAIN with the record left RESERVED), never OWNED_FAILURE. The
 * in-progress flag is cleared before returning, so join/abandon are not EBUSY afterwards. */
uint32_t ps3_ppu_service_start_background(ps3_ppu_threads *, ps3_ppu_ref);
uint32_t ps3_ppu_service_abandon(ps3_ppu_threads *, ps3_ppu_ref);
/* Body helper: mark the service exited (called by the body before returning). */
void ps3_ppu_service_exit(ps3_ppu_service *, uint32_t code);
/* ESRCH for a stale/retired handle; EBUSY for a concurrent second joiner; PROVIDER_FAILURE when
 * the adapter failed to join (record, charge and handle stay live; retry is allowed) or when the quota
 * release failed after a successful host join: the record becomes CLEANUP_PENDING (ref and charge
 * retained) and a later join retries only the release, never the host join.
 * *exit_code (optional) is written only when the service really EXITED (activity EXITED published by
 * ps3_ppu_service_exit, including the trampoline's exit 0 after a body returns): the real code is preserved across
 * cleanup retries. Joining a failed-create CLEANUP_PENDING record (no host thread ever existed) leaves *exit_code
 * untouched. This is a value/protocol guarantee, not a guest-visible PPU join. */
uint32_t ps3_ppu_service_join(ps3_ppu_threads *, ps3_ppu_ref, uint32_t *exit_code);
/* ---- Mapped guest storage (host/domain layer only: no CPU, register or platform type; this is NOT guest execution) ----
 * reserve_guest == reserve (same arguments, same RESERVED lifecycle: start/abandon/join are shared) except storage:
 * ONE ps3_user_memory_allocate of [stack region][TLS region] instead of a quota charge token. The allocation is the quota
 * charge (never both). Sizes: logical stack uses the existing 4 KiB policy (0 -> one 4 KiB granule); the actual stack region
 * is the logical stack rounded up to 64 KiB, the TLS region is tls_capacity rounded up to 64 KiB, both with checked 64-bit
 * arithmetic. tls_capacity == 0 is EINVAL (never defaulted).
 * Validation order: pointers/ports/body/ref/registry (EINVAL), priority > 3071 (EINVAL), priority >= creator (UNSUPPORTED),
 * stack rounding overflow / zero TLS / TLS rounding overflow / region-sum overflow (EINVAL), main id not adopted (EINVAL),
 * free slot + live capacity + id serial (EAGAIN, decided BEFORE allocating, so a committed allocation always has a record to
 * own it; note quota-only reserve charges first), then the allocator: QUOTA_EXCEEDED -> ENOMEM; every other allocator status
 * (CAPACITY_EXCEEDED, NO_SPACE, commit/MEMORY_FAILURE, ...) -> PROVIDER_FAILURE. No guest errno is invented. On any failure
 * nothing is allocated or retained, no id is consumed and *ref is untouched. The registry guard is held across the allocation
 * (lock order: registry guard, then allocator guard; memory callbacks must not reenter this registry).
 * Release is unified: join/abandon free the allocation (quota-only records uncharge instead). A free (decommit) failure keeps
 * the allocation, quota and ref: abandon stays RESERVED + PROVIDER_FAILURE; join becomes CLEANUP_PENDING + PROVIDER_FAILURE
 * with the completed host join remembered, so a retry only retries the free and never joins the host thread again.
 * guest_storage(ref): snapshot of the layout of a live GUEST_ALLOCATION record (RESERVED/RUNNING/EXITED/CLEANUP_PENDING).
 * EINVAL: NULL/uninitialised registry or NULL out; ESRCH: stale/unknown ref; UNSUPPORTED: quota-only record (owns no
 * address). *out is written only on OK. tls_capacity_bytes is the ALLOCATED capacity (the 64 KiB-rounded region extent);
 * stack_logical_bytes is the 4 KiB-rounded request. The allocator's address is reported, never assumed. The layout makes no
 * guard-page claim. The first known request (stack 0x10000, TLS 0x10000) is allocation_bytes 0x20000. */
typedef struct ps3_ppu_guest_storage {
    uint32_t allocation_address; uint64_t allocation_bytes;
    uint32_t stack_address; uint64_t stack_logical_bytes, stack_region_bytes;
    uint32_t tls_address; uint64_t tls_capacity_bytes;
} ps3_ppu_guest_storage;
uint32_t ps3_ppu_service_reserve_guest(ps3_ppu_threads *, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                       uint64_t stack_size, uint64_t tls_capacity, ps3_service_thread_port thread,
                                       ps3_ppu_service_body body, void *argument, ps3_ppu_ref *ref);
/* reserve_guest for a FENCED RUN-TO-EXIT child: identical to ps3_ppu_service_reserve_guest (same arguments, order, effects and
 * lifecycle) except that priority >= creator_priority is accepted (priority > 3071 is still EINVAL). Fence: start() already waits
 * until the body leaves RUNNING; the owner must treat a body that BLOCKS instead of exiting as a failure (it then cancels/joins),
 * so a lower-priority child never runs concurrently with its creator. Only the run-to-exit child owner uses this entry point. */
uint32_t ps3_ppu_service_reserve_guest_fenced(ps3_ppu_threads *, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                              uint64_t stack_size, uint64_t tls_capacity, ps3_service_thread_port thread,
                                              ps3_ppu_service_body body, void *argument, ps3_ppu_ref *ref);
uint32_t ps3_ppu_service_guest_storage(ps3_ppu_threads *, ps3_ppu_ref, ps3_ppu_guest_storage *out);
int ps3_ppu_service_is_blocked(ps3_ppu_threads *, ps3_ppu_ref);
size_t ps3_ppu_threads_live(ps3_ppu_threads *);
/* 1 when `address` lies inside the guest STACK region of a live (not FREE) service that owns a guest allocation
 * (reserve_guest / reserve_guest_fenced): [allocation_address, allocation_address + stack_region_bytes). 0 otherwise,
 * and 0 for a NULL or uninitialised registry. Taken under the registry guard. */
int ps3_ppu_threads_is_guest_stack(ps3_ppu_threads *, uint32_t address);
#ifdef __cplusplus
}
#endif
#endif
