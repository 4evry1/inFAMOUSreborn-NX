#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#include "ppu_threads.h"
#include <string.h>

static void lock(ps3_ppu_threads *t) { t->guard.lock(t->guard.context); }
static void unlock(ps3_ppu_threads *t) { t->guard.unlock(t->guard.context); }
uint32_t ps3_ppu_threads_init(ps3_ppu_threads *t, ps3_instance_guard guard, ps3_notify_port port, ps3_user_memory *memory) {
    if (!t || !guard.lock || !guard.unlock || !port.wait || !port.wake_all || !memory) return PS3_PPU_EINVAL;
    if (t->initialized) return PS3_PPU_EINVAL;   /* one-shot on zeroed storage */
    memset(t, 0, sizeof *t);
    t->guard = guard; t->port = port; t->memory = memory; t->capacity_limit = PS3_PPU_SERVICE_CAP;
    t->initialized = 1;
    return PS3_PPU_OK;
}
uint32_t ps3_ppu_threads_adopt_main(ps3_ppu_threads *t, uint32_t main_id) {
    if (!t || !t->initialized || !main_id) return PS3_PPU_EINVAL;
    lock(t);
    uint32_t rc = PS3_PPU_OK;
    if (t->main_id) rc = t->main_id == main_id ? PS3_PPU_OK : PS3_PPU_EINVAL;
    else if (t->serial) rc = PS3_PPU_EINVAL;   /* services already hold ids allocated without the reservation */
    else t->main_id = main_id;
    unlock(t);
    return rc;
}
uint32_t ps3_ppu_threads_main_id(ps3_ppu_threads *t) {
    if (!t || !t->initialized) return 0;
    lock(t);
    const uint32_t id = t->main_id;
    unlock(t);
    return id;
}
void ps3_ppu_threads_limit(ps3_ppu_threads *t, uint32_t live_capacity) {
    lock(t);
    t->capacity_limit = live_capacity < PS3_PPU_SERVICE_CAP ? live_capacity : PS3_PPU_SERVICE_CAP;
    unlock(t);
}
size_t ps3_ppu_threads_live(ps3_ppu_threads *t) {
    lock(t);
    size_t n = 0;
    for (size_t i = 0; i < PS3_PPU_SERVICE_CAP; ++i) n += __atomic_load_n(&t->services[i].life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_FREE;
    unlock(t);
    return n;
}
static void trampoline(void *argument) {
    ps3_ppu_service *s = argument;
    s->body(s, s->argument);
    if (__atomic_load_n(&s->life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_EXITED) ps3_ppu_service_exit(s, 0);
}
void ps3_ppu_service_exit(ps3_ppu_service *s, uint32_t code) {
    s->exit_code = code;
    __atomic_store_n(&s->life, PS3_PPU_SERVICE_EXITED, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->activity, PS3_PPU_ACTIVITY_EXITED, __ATOMIC_SEQ_CST);
    s->owner->port.wake_all(s->owner->port.context);
}
static ps3_ppu_service *resolve(ps3_ppu_threads *t, ps3_ppu_ref ref) {   /* guarded */
    if (ref.slot >= PS3_PPU_SERVICE_CAP) return NULL;
    ps3_ppu_service *s = &t->services[ref.slot];
    return s->id == ref.id && ref.id && __atomic_load_n(&s->life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_FREE ? s : NULL;
}
int ps3_ppu_threads_is_guest_stack(ps3_ppu_threads *t, uint32_t address) {
    if (!t || !t->initialized) return 0;
    lock(t);
    int inside = 0;
    for (size_t i = 0; i < PS3_PPU_SERVICE_CAP && !inside; ++i) {
        const ps3_ppu_service *s = &t->services[i];
        if (__atomic_load_n(&s->life, __ATOMIC_SEQ_CST) == PS3_PPU_SERVICE_FREE) continue;
        if (s->storage_mode != PS3_PPU_STORAGE_GUEST_ALLOCATION) continue;
        inside = (uint64_t)address - s->allocation_address < s->stack_region_bytes;
    }
    unlock(t);
    return inside;
}
int ps3_ppu_service_is_blocked(ps3_ppu_threads *t, ps3_ppu_ref ref) {
    lock(t);
    ps3_ppu_service *s = resolve(t, ref);
    int blocked = s && __atomic_load_n(&s->activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_BLOCKED;
    unlock(t);
    return blocked;
}

_Static_assert(PS3_PPU_GUEST_REGION_GRANULE == PS3_USER_MEMORY_ALIGN_64K, "regions are whole user-allocator granules");
static int round_up_checked(uint64_t value, uint64_t granule, uint64_t *rounded) {
    if (value > UINT64_MAX - (granule - 1)) return 0;
    *rounded = (value + granule - 1) / granule * granule;
    return 1;
}
/* Logical stack policy shared by both reserve routes: 4 KiB rounding, zero is one granule, overflow is refused. */
static int logical_stack_bytes(uint64_t stack_size, uint64_t *logical) {
    if (!stack_size) { *logical = PS3_PPU_STACK_GRANULE; return 1; }
    return round_up_checked(stack_size, PS3_PPU_STACK_GRANULE, logical);
}
static uint32_t validate_reserve_arguments(ps3_ppu_threads *t, uint32_t priority, uint32_t creator_priority,
                                           ps3_service_thread_port thread, ps3_ppu_service_body body, ps3_ppu_ref *ref,
                                           int allow_lower_priority) {
    if (!t || !t->initialized || !body || !ref || !thread.start || !thread.join) return PS3_PPU_EINVAL;
    if (priority > 3071) return PS3_PPU_EINVAL;
    /* equal/lower priority would run unfenced; a fenced run-to-exit child is serialised by start()'s prefix wait instead */
    if (!allow_lower_priority && priority >= creator_priority) return PS3_PPU_UNSUPPORTED;
    return PS3_PPU_OK;
}
/* guarded; no effects: a free slot that live capacity and the id namespace still admit, else NULL */
static ps3_ppu_service *available_slot(ps3_ppu_threads *t) {
    ps3_ppu_service *slot = NULL;
    size_t live = 0;
    for (size_t i = 0; i < PS3_PPU_SERVICE_CAP; ++i) {
        if (__atomic_load_n(&t->services[i].life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_FREE) ++live;
        else if (!slot) slot = &t->services[i];
    }
    return !slot || live >= t->capacity_limit || t->serial >= 0xFFFFFEu ? NULL : slot;
}
/* guarded: consume the next id and fill the storage-independent part of the record; the caller sets the storage
 * fields, then publishes. */
static void initialise_record(ps3_ppu_threads *t, ps3_ppu_service *s, uint32_t kind, uint32_t priority, uint64_t stack_logical,
                              ps3_service_thread_port thread, ps3_ppu_service_body body, void *argument) {
    memset(s, 0, sizeof *s);
    s->id = PS3_PPU_THREAD_BASE + t->serial++;
    if (s->id == t->main_id) s->id = PS3_PPU_THREAD_BASE + t->serial++;   /* L5: never the main thread's id */
    s->priority = priority; s->kind = kind; s->stack_bytes = stack_logical;
    s->thread = thread; s->body = body; s->argument = argument; s->owner = t;
}
static void publish_reserved(ps3_ppu_service *s, ps3_ppu_ref *ref) {   /* guarded */
    __atomic_store_n(&s->life, PS3_PPU_SERVICE_RESERVED, __ATOMIC_SEQ_CST);
    ref->slot = (uint32_t)(s - s->owner->services); ref->id = s->id;
}
/* guarded. The ONE release of a record's storage: uncharge for quota-only, a real free of the mapped allocation otherwise.
 * Any failure leaves the storage owned (unchanged). */
static uint32_t release_storage(ps3_ppu_threads *t, ps3_ppu_service *s) {
    const int released = s->storage_mode == PS3_PPU_STORAGE_GUEST_ALLOCATION
        ? ps3_user_memory_free(t->memory, s->allocation_address).status == PS3_USER_MEMORY_OK
        : ps3_user_memory_uncharge(t->memory, &s->stack) == PS3_USER_MEMORY_OK;
    return released ? PS3_PPU_OK : PS3_PPU_PROVIDER_FAILURE;
}
uint32_t ps3_ppu_service_reserve(ps3_ppu_threads *t, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                 uint64_t stack_size, ps3_service_thread_port thread, ps3_ppu_service_body body,
                                 void *argument, ps3_ppu_ref *ref) {
    const uint32_t invalid = validate_reserve_arguments(t, priority, creator_priority, thread, body, ref, 0);
    if (invalid != PS3_PPU_OK) return invalid;
    uint64_t stack;
    if (!logical_stack_bytes(stack_size, &stack)) return PS3_PPU_EINVAL;
    lock(t);
    if (!t->main_id) { unlock(t); return PS3_PPU_EINVAL; }   /* L5: main id not adopted: nothing charged or started */
    ps3_user_memory_charge_token charge;
    if (ps3_user_memory_charge(t->memory, stack, &charge) != PS3_USER_MEMORY_OK) { unlock(t); return PS3_PPU_ENOMEM; }
    ps3_ppu_service *s = available_slot(t);
    if (!s) {
        ps3_user_memory_uncharge(t->memory, &charge);
        unlock(t);
        return PS3_PPU_EAGAIN;
    }
    initialise_record(t, s, kind, priority, stack, thread, body, argument);
    s->storage_mode = PS3_PPU_STORAGE_QUOTA_ONLY; s->stack = charge;
    publish_reserved(s, ref);
    unlock(t);
    return PS3_PPU_OK;
}
/* The ONE reserve_guest body; `allow_lower_priority` only relaxes the priority rule (fenced run-to-exit reservation). */
static uint32_t reserve_guest(ps3_ppu_threads *t, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                              uint64_t stack_size, uint64_t tls_capacity, ps3_service_thread_port thread,
                              ps3_ppu_service_body body, void *argument, ps3_ppu_ref *ref, int allow_lower_priority) {
    const uint32_t invalid = validate_reserve_arguments(t, priority, creator_priority, thread, body, ref, allow_lower_priority);
    if (invalid != PS3_PPU_OK) return invalid;
    uint64_t stack_logical, stack_region, tls_region, total;
    if (!logical_stack_bytes(stack_size, &stack_logical) || !tls_capacity ||
        !round_up_checked(stack_logical, PS3_PPU_GUEST_REGION_GRANULE, &stack_region) ||
        !round_up_checked(tls_capacity, PS3_PPU_GUEST_REGION_GRANULE, &tls_region) || stack_region > UINT64_MAX - tls_region)
        return PS3_PPU_EINVAL;
    total = stack_region + tls_region;
    lock(t);
    if (!t->main_id) { unlock(t); return PS3_PPU_EINVAL; }
    ps3_ppu_service *s = available_slot(t);   /* before allocating: a committed allocation must always have a record to own it */
    if (!s) { unlock(t); return PS3_PPU_EAGAIN; }
    uint32_t address = 0;
    const ps3_user_memory_status allocated = ps3_user_memory_allocate(t->memory, total, PS3_USER_MEMORY_ALIGN_64K, &address).status;
    if (allocated != PS3_USER_MEMORY_OK) {
        unlock(t);
        return allocated == PS3_USER_MEMORY_QUOTA_EXCEEDED ? PS3_PPU_ENOMEM : PS3_PPU_PROVIDER_FAILURE;
    }
    initialise_record(t, s, kind, priority, stack_logical, thread, body, argument);
    s->storage_mode = PS3_PPU_STORAGE_GUEST_ALLOCATION;
    s->allocation_address = address; s->allocation_bytes = total;
    s->stack_region_bytes = stack_region; s->tls_region_bytes = tls_region;
    publish_reserved(s, ref);
    unlock(t);
    return PS3_PPU_OK;
}
uint32_t ps3_ppu_service_reserve_guest(ps3_ppu_threads *t, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                       uint64_t stack_size, uint64_t tls_capacity, ps3_service_thread_port thread,
                                       ps3_ppu_service_body body, void *argument, ps3_ppu_ref *ref) {
    return reserve_guest(t, kind, priority, creator_priority, stack_size, tls_capacity, thread, body, argument, ref, 0);
}
uint32_t ps3_ppu_service_reserve_guest_fenced(ps3_ppu_threads *t, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                              uint64_t stack_size, uint64_t tls_capacity, ps3_service_thread_port thread,
                                              ps3_ppu_service_body body, void *argument, ps3_ppu_ref *ref) {
    return reserve_guest(t, kind, priority, creator_priority, stack_size, tls_capacity, thread, body, argument, ref, 1);
}
uint32_t ps3_ppu_service_guest_storage(ps3_ppu_threads *t, ps3_ppu_ref ref, ps3_ppu_guest_storage *out) {
    if (!t || !t->initialized || !out) return PS3_PPU_EINVAL;
    lock(t);
    const ps3_ppu_service *s = resolve(t, ref);
    uint32_t rc = PS3_PPU_OK;
    if (!s) rc = PS3_PPU_ESRCH;
    else if (s->storage_mode != PS3_PPU_STORAGE_GUEST_ALLOCATION) rc = PS3_PPU_UNSUPPORTED;
    else *out = (ps3_ppu_guest_storage){   /* the allocator keeps the whole allocation inside the 32-bit window, so the TLS start fits */
        .allocation_address = s->allocation_address, .allocation_bytes = s->allocation_bytes,
        .stack_address = s->allocation_address, .stack_logical_bytes = s->stack_bytes, .stack_region_bytes = s->stack_region_bytes,
        .tls_address = (uint32_t)(s->allocation_address + s->stack_region_bytes), .tls_capacity_bytes = s->tls_region_bytes };
    unlock(t);
    return rc;
}
/* Root diagnostic (racy on purpose): where the most recent start_service call is. Read only by the unjoined report. */
volatile uint32_t g_ps3_ppu_start_stage;
/* Set by the owner (watchdog / teardown) to end every creator prefix wait. Never cleared. */
uint32_t g_ps3_ppu_prefix_cancel;
/* Root diagnostic (racy sums, read after the run): how long creators waited in the prefix wait. */
uint64_t g_ps3_ppu_prefix_released;
uint64_t g_ps3_ppu_prefix_waits, g_ps3_ppu_prefix_slices, g_ps3_ppu_prefix_max_slices, g_ps3_ppu_prefix_over_1s, g_ps3_ppu_prefix_over_10s;
static uint32_t start_service(ps3_ppu_threads *t, ps3_ppu_ref ref, int wait_for_prefix) {
    __atomic_store_n(&g_ps3_ppu_start_stage, 1u, __ATOMIC_RELAXED);
    if (!t || !t->initialized) return PS3_PPU_EINVAL;
    lock(t);
    ps3_ppu_service *s = resolve(t, ref);
    if (!s) { unlock(t); return PS3_PPU_ESRCH; }
    if (s->starting) { unlock(t); return PS3_PPU_EBUSY; }
    if (__atomic_load_n(&s->life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_RESERVED) { unlock(t); return PS3_PPU_EINVAL; }
    s->starting = 1;   /* the record is owned by this start until the prefix wait ends: join/abandon/start see EBUSY */
    const ps3_service_thread_port thread = s->thread;
    __atomic_store_n(&s->life, PS3_PPU_SERVICE_RUNNING, __ATOMIC_SEQ_CST);   /* before start: the body may exit at once */
    unlock(t);
    __atomic_store_n(&g_ps3_ppu_start_stage, 2u, __ATOMIC_RELAXED);
    if (thread.start(thread.context, trampoline, s) != 0) {
        lock(t);
        __atomic_store_n(&s->life, PS3_PPU_SERVICE_RESERVED, __ATOMIC_SEQ_CST);   /* nothing ran: the reservation stays owned */
        s->starting = 0;
        unlock(t);
        return PS3_PPU_EAGAIN;
    }
    uint32_t rc = PS3_PPU_OK;
    __atomic_store_n(&g_ps3_ppu_start_stage, 3u, __ATOMIC_RELAXED);
#ifdef PS3_NATIVE_LLE_PROFILE
    /* Root (2026-11-11, Eden r16..r22): the creator waits here until its new HIGHER-priority thread first blocks or
     * exits (the composition starts equal and lower priority threads in the background). That is the PS3's own
     * order: such a thread runs before the creator continues. Two bounded variants (20 ms, 2 s, 10 s) were tried and
     * all changed the game's start-up order, so the wait stays unbounded, as on the console. What was wrong is that
     * it could not be CANCELLED: a thread that works for longer than the diagnostic run kept the creator here past
     * the deadline and the run ended unjoined, without its report. The wait is now sliced (50 ms) and ends when the
     * owner publishes g_ps3_ppu_prefix_cancel; the creator then reaches its own cancellation point. */
    uint64_t prefix_slices = 0;
    if (wait_for_prefix) __atomic_fetch_add(&g_ps3_ppu_prefix_waits, 1, __ATOMIC_RELAXED);
    while (wait_for_prefix && __atomic_load_n(&s->activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_RUNNING &&
           !__atomic_load_n(&g_ps3_ppu_prefix_cancel, __ATOMIC_SEQ_CST)) {
        /* Measured (Eden r24, three runs): 19 of 20 prefix waits end inside the first 50 ms slice; the one that does
         * not is a thread that SPINS on a flag its creator sets only after the create call returns
         * (func at 0x187980: `while (!*(obj+4)) ;` against a store of 1 at obj+4 after sys_ppu_thread_create at 0x187a94).
         * A PS3 runs two hardware threads, so there the creator simply continues next to it. Five slices (250 ms)
         * without a block or an exit and the creator goes on; the new thread keeps running. Earlier bounded tries
         * (r20..r22) looked broken only because of an unrelated, intermittent start-up stall (see DECISIONS). */
        if (prefix_slices >= 5) { __atomic_fetch_add(&g_ps3_ppu_prefix_released, 1, __ATOMIC_RELAXED); break; }
        ++prefix_slices;
        if (prefix_slices == 20) __atomic_fetch_add(&g_ps3_ppu_prefix_over_1s, 1, __ATOMIC_RELAXED);
        if (prefix_slices == 200) __atomic_fetch_add(&g_ps3_ppu_prefix_over_10s, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_ps3_ppu_prefix_slices, 1, __ATOMIC_RELAXED);
        if (prefix_slices > __atomic_load_n(&g_ps3_ppu_prefix_max_slices, __ATOMIC_RELAXED))
            __atomic_store_n(&g_ps3_ppu_prefix_max_slices, prefix_slices, __ATOMIC_RELAXED);
        uint64_t slice_deadline = UINT64_MAX;
#ifdef __SWITCH__
        /* The console wait port measures deadlines with its own tick clock (wait_port_horizon.c); a deadline taken
         * from clock_gettime did not time out there (Eden r17/r18). */
        extern uint64_t ps3_horizon_wait_port_now_ns(void);
        slice_deadline = ps3_horizon_wait_port_now_ns() + UINT64_C(50000000);
#else
        struct timespec ts;
        if (!clock_gettime(CLOCK_MONOTONIC, &ts))
            slice_deadline = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec + UINT64_C(50000000);
#endif
        if (t->port.wait(t->port.context, &s->activity, PS3_PPU_ACTIVITY_RUNNING, &g_ps3_ppu_prefix_cancel,
                         slice_deadline) == PS3_NOTIFY_FAILURE) {
            rc = PS3_PPU_OWNED_FAILURE;   /* running and charged; the caller must stop and join */
            break;
        }
    }
#else
    while (wait_for_prefix && __atomic_load_n(&s->activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_RUNNING)
        if (t->port.wait(t->port.context, &s->activity, PS3_PPU_ACTIVITY_RUNNING, NULL, UINT64_MAX) == PS3_NOTIFY_FAILURE) {
            rc = PS3_PPU_OWNED_FAILURE;   /* running and charged; the caller must stop and join */
            break;
        }
#endif
    __atomic_store_n(&g_ps3_ppu_start_stage, 4u, __ATOMIC_RELAXED);
    lock(t);
    s->starting = 0;
    unlock(t);
    return rc;
}
uint32_t ps3_ppu_service_start(ps3_ppu_threads *t, ps3_ppu_ref ref) {
    return start_service(t, ref, 1);
}
uint32_t ps3_ppu_service_start_background(ps3_ppu_threads *t, ps3_ppu_ref ref) {
    return start_service(t, ref, 0);
}
uint32_t ps3_ppu_service_abandon(ps3_ppu_threads *t, ps3_ppu_ref ref) {
    if (!t || !t->initialized) return PS3_PPU_EINVAL;
    lock(t);
    ps3_ppu_service *s = resolve(t, ref);
    uint32_t rc = PS3_PPU_OK;
    if (!s) rc = PS3_PPU_ESRCH;
    else if (s->starting) rc = PS3_PPU_EBUSY;
    else if (__atomic_load_n(&s->life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_RESERVED) rc = PS3_PPU_EINVAL;
    else if (release_storage(t, s) != PS3_PPU_OK) rc = PS3_PPU_PROVIDER_FAILURE;   /* still owned: stays RESERVED */
    else __atomic_store_n(&s->life, PS3_PPU_SERVICE_FREE, __ATOMIC_SEQ_CST);
    unlock(t);
    return rc;
}
/* INTERNAL (not exported): the ONE guarded cleanup of a create whose start failed. Resolve the exact ref and inspect
 * its real state in the same critical section as any release, so a reservation another actor started or retired in the
 * window after start's failure transition is never released, converted or published on stale assumptions. */
static uint32_t cleanup_failed_start(ps3_ppu_threads *t, ps3_ppu_ref made, uint32_t start_rc, ps3_ppu_ref *out) {
    lock(t);
    ps3_ppu_service *s = resolve(t, made);
    uint32_t rc;
    if (!s) rc = PS3_PPU_EAGAIN;                                     /* retired or recycled: nothing owned, no stale ref published */
    else if (s->starting || __atomic_load_n(&s->life, __ATOMIC_SEQ_CST) != PS3_PPU_SERVICE_RESERVED) {
        *out = made; rc = PS3_PPU_OWNED_FAILURE;                     /* another actor started the live record: keep its real state for the owner's cancel+join */
    } else if (release_storage(t, s) == PS3_PPU_OK) {
        __atomic_store_n(&s->life, PS3_PPU_SERVICE_FREE, __ATOMIC_SEQ_CST);
        rc = start_rc;                                               /* released: the original failure, *out untouched */
    } else {
        /* unstarted RESERVED and the storage release failed: still owned, and no host thread exists to join */
        __atomic_store_n(&s->life, PS3_PPU_SERVICE_CLEANUP_PENDING, __ATOMIC_SEQ_CST);
        s->host_join_complete = 1;
        *out = made; rc = PS3_PPU_OWNED_FAILURE;
    }
    unlock(t);
    return rc;
}
uint32_t ps3_ppu_service_create(ps3_ppu_threads *t, uint32_t kind, uint32_t priority, uint32_t creator_priority,
                                uint64_t stack_size, ps3_service_thread_port thread, ps3_ppu_service_body body,
                                void *argument, ps3_ppu_ref *ref) {
    ps3_ppu_ref made;
    uint32_t rc = ps3_ppu_service_reserve(t, kind, priority, creator_priority, stack_size, thread, body, argument, &made);
    if (rc != PS3_PPU_OK) return rc;
    rc = ps3_ppu_service_start(t, made);
    if (rc == PS3_PPU_OK || rc == PS3_PPU_OWNED_FAILURE) { *ref = made; return rc; }   /* owned from here */
    return cleanup_failed_start(t, made, rc, ref);
}
uint32_t ps3_ppu_service_join(ps3_ppu_threads *t, ps3_ppu_ref ref, uint32_t *exit_code) {
    if (!t || !t->initialized) return PS3_PPU_EINVAL;
    lock(t);
    ps3_ppu_service *s = resolve(t, ref);
    if (!s) { unlock(t); return PS3_PPU_ESRCH; }
    if (s->starting || s->joining) { unlock(t); return PS3_PPU_EBUSY; }
    if (__atomic_load_n(&s->life, __ATOMIC_SEQ_CST) == PS3_PPU_SERVICE_RESERVED) { unlock(t); return PS3_PPU_EINVAL; }   /* nothing to join */
    s->joining = 1;
    const int host_join_needed = !s->host_join_complete;
    unlock(t);
    int joined = host_join_needed ? s->thread.join(s->thread.context) : 0;   /* outside the guard: body may still need it */
    lock(t);
    if (joined != 0) { s->joining = 0; unlock(t); return PS3_PPU_PROVIDER_FAILURE; }
    s->host_join_complete = 1;   /* remembered independently of the storage release */
    /* only a REAL published exit (activity EXITED, set by ps3_ppu_service_exit) has a code; a failed-create cleanup
     * record never ran, so the caller's optional *exit_code stays untouched */
    if (exit_code && __atomic_load_n(&s->activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_EXITED) *exit_code = s->exit_code;
    if (release_storage(t, s) != PS3_PPU_OK) {
        __atomic_store_n(&s->life, PS3_PPU_SERVICE_CLEANUP_PENDING, __ATOMIC_SEQ_CST);   /* still owned: retained, retry releases only */
        s->joining = 0;
        unlock(t);
        return PS3_PPU_PROVIDER_FAILURE;
    }
    __atomic_store_n(&s->life, PS3_PPU_SERVICE_FREE, __ATOMIC_SEQ_CST);
    s->joining = 0;
    unlock(t);
    return PS3_PPU_OK;
}
