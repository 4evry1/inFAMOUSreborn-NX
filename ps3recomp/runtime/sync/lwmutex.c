#include "lwmutex.h"
#include <string.h>
#include <limits.h>

enum { OBJECT_LIVE = 1, OBJECT_UNPUBLISHED = 2, OBJECT_RETIRING = 3 };
enum { WAITER_LIVE = 1, WAITER_ABANDONED = 2, CONDITION_DORMANT = 3,
       CONDITION_ELIGIBLE = 4, CONDITION_POISONED = 5 };
typedef struct guest_state {
    uint32_t owner, waiters, attributes, recursion, queue_id, padding;
} guest_state;

#ifndef PS3_LWMUTEX_GUARD_TIMING
#define PS3_LWMUTEX_GUARD_TIMING 0   /* 1: time the registry-guard wait of every guarded lock, also with the fast path on */
#endif
/* Root (2026-11-12, Decision 86): where lock time goes. File-level so the registry layout does not change.
 * Written under the registry guard, read once at the end of a run (racy read, diagnostics only). */
ps3_lwmutex_debug_counters g_ps3_lwmutex_debug;
static void debug_contended(uint32_t address, uint32_t owner, uint32_t waiter, uint64_t ns)
{
    ps3_lwmutex_debug_counters *d = &g_ps3_lwmutex_debug;
    ++d->lock_contended; d->contended_ns += ns;
    if (ns > d->contended_max_ns) { d->contended_max_ns = ns; d->max_address = address; d->max_owner = owner; d->max_waiter = waiter; }
    size_t slot = PS3_LWMUTEX_DEBUG_TOP, least = 0;
    for (size_t index = 0; index < PS3_LWMUTEX_DEBUG_TOP; ++index) {
        if (d->top[index].count && d->top[index].address == address) { slot = index; break; }
        if (d->top[index].ns < d->top[least].ns) least = index;
    }
    if (slot == PS3_LWMUTEX_DEBUG_TOP) { slot = least; d->top[slot].address = address; d->top[slot].count = 0; d->top[slot].ns = 0; }
    ++d->top[slot].count; d->top[slot].ns += ns; d->top[slot].last_owner = owner; d->top[slot].last_waiter = waiter;
}
/* A sleeper on this registry's wait port is a mutex waiter (WAITER_LIVE) or a condition waiter that is taking its
 * mutex back (CONDITION_ELIGIBLE). With none, a wake-up call has nobody to wake. */
static int has_port_sleeper(ps3_lwmutex_registry *registry);

static ps3_lwmutex_result result(ps3_lwmutex_status status)
{
    return (ps3_lwmutex_result){status, PS3_GUEST_MEMORY_OK, PS3_WAIT_READY};
}

static ps3_lwmutex_result memory_failure(ps3_guest_memory_status status)
{
    return (ps3_lwmutex_result){PS3_LWMUTEX_MEMORY_FAILURE, status, PS3_WAIT_READY};
}

static ps3_lwmutex_result wait_failure(ps3_wait_status status)
{
    ps3_lwmutex_status core_status = PS3_LWMUTEX_WAIT_FAILURE;
    if (status == PS3_WAIT_TIMEOUT) core_status = PS3_LWMUTEX_TIMED_OUT;
    if (status == PS3_WAIT_CANCELLED) core_status = PS3_LWMUTEX_CANCELLED;
    return (ps3_lwmutex_result){core_status, PS3_GUEST_MEMORY_OK, status};
}

static int valid_registry(ps3_lwmutex_registry *registry)
{
    return registry && registry->initialized;
}

static int valid_thread(ps3_lwmutex_thread thread)
{
    return thread.id && thread.id < PS3_LWMUTEX_RESERVED;
}

static uint32_t decode32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static void encode32(unsigned char *bytes, uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index) {
        bytes[3 - index] = (unsigned char)value;
        value >>= 8;
    }
}

static size_t find_object(ps3_lwmutex_registry *registry, uint32_t address)
{
    /* Root (Decision 86): a hint table in front of the linear scan (1024 objects of 64 bytes on the console).
     * A hint is only a place to look first: it is checked against the object it names, so a stale one, or one
     * left by another registry, costs a scan and nothing else. Callers hold the registry guard. */
    static uint16_t hint[256];
    const size_t slot = (address >> 3) & 255u;
    const size_t guess = hint[slot];
    if (guess < registry->object_capacity && registry->objects[guess].state &&
        registry->objects[guess].address == address) return guess;
    for (size_t index = 0; index < registry->object_capacity; ++index) {
        if (registry->objects[index].state && registry->objects[index].address == address) {
            if (index <= UINT16_MAX) hint[slot] = (uint16_t)index;
            return index;
        }
    }
    return registry->object_capacity;
}

/* ---- ppu-speed-20261113 item 1: the lock-free fast path -------------------------------------------------------------------
 *
 * Why: the console log (rv49s) shows 4.58 M lock calls of which 12 k (0.27 %) ever waited for an owner; yet each lock+unlock
 * took the registry guard once, the atomic-domain guard four times and read the clock three times (128-bit divides), and
 * every thread of the game goes through those two process-wide guards. The 80 s that the whole run spent getting the registry
 * guard (STARTUP_LWMUTEX_TIME guard_ms) is that serialisation, not the work.
 *
 * What: for a fast-eligible mutex (see ps3_lwmutex_enable_fast_path) an uncontended lock is one host compare-and-swap of the
 * 64-bit (owner, waiters) pair at the start of the guest control block from (FREE, 0) to (thread, 0), and an uncontended
 * unlock one CAS from (thread, 0) to (FREE, 0) with recursion 0. Nothing else changes in guest memory, exactly as before.
 * The guest-visible layout, the owner word, the waiter count, the recursion count, the attributes and the queue are untouched.
 * The fast path refuses (and the guarded path below runs as it always did) whenever: the mutex has any waiter (waiters != 0,
 * so queue order, priority order and wake-ups stay in the guarded path), the thread already owns it or recursion != 0, the
 * guest control block does not show the identity (attributes, queue id, zero padding) the registry gave it at create (a
 * destroyed or reused address), the timeout is huge, cancellation is already requested, the 128-byte line has held an SPU
 * reservation (ps3_atomic_sticky_hit), or the entry is missing.
 *
 * Exclusion with the guarded paths: the guarded paths no longer take the pair word by a domain read-modify-write. For a
 * fast-eligible mutex they use the same host CAS (exchange_pair) and retry when it fails because the pair changed
 * (LWMUTEX_RACED). Only transitions that START from waiters == 0 can race with the fast path (a registered waiter makes every
 * fast CAS fail), so only four guarded transitions retry: initial acquire, waiter registration, the condition-wait
 * admission, and retire (destroy).
 *
 * Reservations: a store the domain did not see must still kill reservations on its line. After every successful host CAS the
 * path runs the Decision 87 test (sticky line or domain conflict, ps3_atomic_domain_write_conflicts); when it fires, one
 * guarded write of the (unchanged) attributes word, always in the pair word's 128-byte line (fast-eligible addresses have
 * address % 128 <= 112), drops the reservations and bumps the line sequences, as any domain write does. The exposure left is
 * the one lean_store already has (a store that the second test catches late).
 *
 * Counters: the old lock_calls/unlock_calls/notify_skipped are bumped only by the guarded paths; fast operations are counted
 * per entry (racy plain increments on the entry's own cache line) and read with ps3_lwmutex_fast_totals. */
#if PS3_LWMUTEX_FAST && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LWMUTEX_FAST_AVAILABLE 1
#else
#define LWMUTEX_FAST_AVAILABLE 0
#endif
#if LWMUTEX_FAST_AVAILABLE
#define LWMUTEX_RACED ((ps3_lwmutex_status)0x7001)   /* internal: the pair changed between the read and the compare-and-swap */
/* Racy diagnostic counters: a relaxed load and store (lost increments are fine, a data race is not). Same code as ++x. */
#define FAST_COUNT(counter) __atomic_store_n(&(counter), __atomic_load_n(&(counter), __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED)
enum { FAST_MAX_TIMEOUT_US = UINT64_C(1) << 40 };
static inline uint64_t host_load_be64(const unsigned char *p, int order)
{
    return __builtin_bswap64(__atomic_load_n((const uint64_t *)(const void *)p, order));
}
static inline uint32_t host_load_be32(const unsigned char *p, int order)
{
    return __builtin_bswap32(__atomic_load_n((const uint32_t *)(const void *)p, order));
}
static ps3_lwmutex_fast_entry *fast_lookup(ps3_lwmutex_registry *registry, uint32_t address, uint64_t *key_out)
{
    if (!address) return NULL;
    const size_t first = ((size_t)address >> 3) ^ ((size_t)address >> 11);
    for (unsigned probe = 0; probe < PS3_LWMUTEX_FAST_PROBES; ++probe) {
        ps3_lwmutex_fast_entry *entry = &registry->fast_table[(first + probe) & (PS3_LWMUTEX_FAST_SLOTS - 1)];
        const uint64_t key = __atomic_load_n(&entry->key, __ATOMIC_ACQUIRE);
        if ((key >> 32) == address) { *key_out = key; return entry; }
        if (!key) return NULL;
    }
    return NULL;
}
/* Under the registry guard. The entry for `address` (an old one is replaced) or the first free slot of its probe run. */
static ps3_lwmutex_fast_entry *fast_slot(ps3_lwmutex_registry *registry, uint32_t address)
{
    const size_t first = ((size_t)address >> 3) ^ ((size_t)address >> 11);
    ps3_lwmutex_fast_entry *free_slot = NULL;
    for (unsigned probe = 0; probe < PS3_LWMUTEX_FAST_PROBES; ++probe) {
        ps3_lwmutex_fast_entry *entry = &registry->fast_table[(first + probe) & (PS3_LWMUTEX_FAST_SLOTS - 1)];
        if ((entry->key >> 32) == address) return entry;
        if (entry->key <= 1 && !free_slot) free_slot = entry;
        if (!entry->key) break;
    }
    return free_slot;
}
static void fast_publish(ps3_lwmutex_registry *registry, const ps3_lwmutex_object *object)
{
    if (!object->host) return;
    ps3_lwmutex_fast_entry *entry = fast_slot(registry, object->address);
    if (!entry) return;                                   /* probe run full: the mutex stays on the guarded path */
    entry->host = object->host; entry->attributes = object->attributes;
    __atomic_store_n(&entry->key, (uint64_t)object->address << 32 | object->queue_id, __ATOMIC_RELEASE);
}
static void fast_unpublish(ps3_lwmutex_registry *registry, const ps3_lwmutex_object *object)
{
    if (!object->host) return;
    ps3_lwmutex_fast_entry *entry = fast_slot(registry, object->address);
    if (entry && (entry->key >> 32) == object->address) __atomic_store_n(&entry->key, 1, __ATOMIC_RELEASE);
}
/* After a successful host CAS or store (a full barrier): the reservation test of the lock-free writers. */
static ps3_lwmutex_result fast_settle(ps3_lwmutex_registry *registry, uint32_t address, uint32_t attributes)
{
    if (!ps3_atomic_sticky_hit(address, 12) && !ps3_atomic_domain_write_conflicts(registry->domain, address, 12))
        return result(PS3_LWMUTEX_OK);
    unsigned char bytes[4]; encode32(bytes, attributes);
    ps3_guest_memory_status status = ps3_atomic_write(registry->domain, (uint64_t)address + 8, bytes, sizeof(bytes));
    return status == PS3_GUEST_MEMORY_OK ? result(PS3_LWMUTEX_OK) : memory_failure(status);
}
/* try_only: also answer BUSY (held by another thread, nobody waiting) without the guard. Returns 1 when `out` is the answer. */
static int fast_lock_try(ps3_lwmutex_registry *registry, uint32_t address, uint32_t thread_id,
    ps3_wait_cancel cancel, int try_only, ps3_lwmutex_result *out)
{
    uint64_t key;
    ps3_lwmutex_fast_entry *entry = fast_lookup(registry, address, &key);
    if (!entry) return 0;
    unsigned char *host = entry->host;
    const uint32_t attributes = entry->attributes, queue_id = (uint32_t)key;
    if (!host) return 0;
    if (cancel.is_cancelled && cancel.is_cancelled(cancel.context)) { FAST_COUNT(entry->why[PS3_LWMUTEX_WHY_CANCEL]); return 0; }
    if (host_load_be64(host + 8, __ATOMIC_RELAXED) != ((uint64_t)attributes << 32) ||
        host_load_be64(host + 16, __ATOMIC_RELAXED) != ((uint64_t)queue_id << 32)) {
        FAST_COUNT(entry->why[PS3_LWMUTEX_WHY_IDENTITY]); return 0;
    }
    if (ps3_atomic_sticky_hit(address, 12)) { FAST_COUNT(entry->why[PS3_LWMUTEX_WHY_STICKY]); return 0; }
    uint64_t expected = __builtin_bswap64((uint64_t)PS3_LWMUTEX_FREE << 32);
    const uint64_t desired = __builtin_bswap64((uint64_t)thread_id << 32);
    if (__atomic_compare_exchange_n((uint64_t *)(void *)host, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        FAST_COUNT(entry->locks);
        *out = fast_settle(registry, address, attributes);
        return 1;
    }
    const uint64_t seen = __builtin_bswap64(expected);
    const uint32_t owner = (uint32_t)(seen >> 32), waiters = (uint32_t)seen;
    if (try_only && !waiters && owner && owner != thread_id && owner < PS3_LWMUTEX_RESERVED) {
        FAST_COUNT(entry->locks);   /* counted with the lock family: a try that answered without the guard */
        *out = result(PS3_LWMUTEX_BUSY);
        return 1;
    }
    FAST_COUNT(entry->why[waiters ? PS3_LWMUTEX_WHY_WAITERS : PS3_LWMUTEX_WHY_HELD]);
    return 0;
}
static int fast_unlock_try(ps3_lwmutex_registry *registry, uint32_t address, uint32_t thread_id, ps3_lwmutex_result *out)
{
    uint64_t key;
    ps3_lwmutex_fast_entry *entry = fast_lookup(registry, address, &key);
    if (!entry) return 0;
    unsigned char *host = entry->host;
    const uint32_t attributes = entry->attributes, queue_id = (uint32_t)key;
    if (!host) return 0;
    /* recursion must be 0 (its word is the low half of the second 64-bit word); anything else is the guarded path's */
    if (host_load_be64(host + 8, __ATOMIC_RELAXED) != ((uint64_t)attributes << 32) ||
        host_load_be64(host + 16, __ATOMIC_RELAXED) != ((uint64_t)queue_id << 32)) {
        FAST_COUNT(entry->why[PS3_LWMUTEX_WHY_IDENTITY]); return 0;
    }
    if (ps3_atomic_sticky_hit(address, 12)) { FAST_COUNT(entry->why[PS3_LWMUTEX_WHY_STICKY]); return 0; }
    uint64_t expected = __builtin_bswap64((uint64_t)thread_id << 32);
    const uint64_t desired = __builtin_bswap64((uint64_t)PS3_LWMUTEX_FREE << 32);
    if (!__atomic_compare_exchange_n((uint64_t *)(void *)host, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        const uint64_t seen = __builtin_bswap64(expected);
        FAST_COUNT(entry->why[(uint32_t)seen ? PS3_LWMUTEX_WHY_WAITERS : (uint32_t)(seen >> 32) != thread_id ? PS3_LWMUTEX_WHY_HELD : PS3_LWMUTEX_WHY_OTHER]);
        return 0;
    }
    FAST_COUNT(entry->unlocks);
    *out = fast_settle(registry, address, attributes);
    return 1;
}
#endif

static int belongs_to(ps3_lwmutex_registry *registry, size_t waiter, size_t object)
{
    ps3_lwmutex_waiter *record = &registry->waiters[waiter];
    return record->state && record->object_index == object &&
           record->object_generation == registry->objects[object].generation;
}

static ps3_lwmutex_result read_state(
    ps3_lwmutex_registry *registry, size_t object_index, guest_state *state)
{
    ps3_lwmutex_object *object = &registry->objects[object_index];
#if LWMUTEX_FAST_AVAILABLE
    if (object->host) {
        /* A fast-eligible mutex: the pair is read as ONE 64-bit load (the fast path changes it with a 64-bit CAS), the rest
         * is immutable (attributes, queue id, padding) or only ever written by the owner (recursion). */
        const unsigned char *host = object->host;
        const uint64_t pair = host_load_be64(host, __ATOMIC_ACQUIRE);
        *state = (guest_state){(uint32_t)(pair >> 32), (uint32_t)pair, host_load_be32(host + 8, __ATOMIC_RELAXED),
            host_load_be32(host + 12, __ATOMIC_RELAXED), host_load_be32(host + 16, __ATOMIC_RELAXED),
            host_load_be32(host + 20, __ATOMIC_RELAXED)};
    } else
#endif
    {
        unsigned char image[PS3_LWMUTEX_BYTES];
        ps3_guest_memory_status status = ps3_atomic_read(
            registry->domain, object->address, image, sizeof(image));
        if (status != PS3_GUEST_MEMORY_OK) return memory_failure(status);
        *state = (guest_state){decode32(image), decode32(image + 4), decode32(image + 8),
            decode32(image + 12), decode32(image + 16), decode32(image + 20)};
    }
    uint64_t expected_waiters = 0;
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        expected_waiters += belongs_to(registry, index, object_index) &&
            (registry->waiters[index].state < CONDITION_DORMANT || registry->waiters[index].condition_counted);
    }
    if (state->attributes != object->attributes || state->queue_id != object->queue_id ||
        state->padding || state->waiters != expected_waiters ||
        state->owner == PS3_LWMUTEX_DEAD || state->owner == PS3_LWMUTEX_RESERVED ||
        !state->owner || (state->owner == PS3_LWMUTEX_FREE && state->recursion) ||
        (!(state->attributes & PS3_LWMUTEX_RECURSIVE) && state->recursion)) {
        return result(PS3_LWMUTEX_INCONSISTENT);
    }
    return result(PS3_LWMUTEX_OK);
}

/* may_race: the pair can legitimately differ from `state` because a lock-free operation changed it (the transition starts from
 * waiters == 0); the failure is then LWMUTEX_RACED and the caller reads the state again. Otherwise a failed compare is what it
 * has always been: INCONSISTENT. */
static ps3_lwmutex_result exchange_pair(
    ps3_lwmutex_registry *registry, size_t object_index,
    guest_state state, uint32_t owner, uint32_t waiters, int may_race)
{
    uint64_t expected = (uint64_t)state.owner << 32 | state.waiters;
    uint64_t desired = (uint64_t)owner << 32 | waiters;
#if LWMUTEX_FAST_AVAILABLE
    ps3_lwmutex_object *object = &registry->objects[object_index];
    if (object->host && !ps3_atomic_sticky_hit(object->address, 12)) {
        uint64_t wire_expected = __builtin_bswap64(expected);
        if (!__atomic_compare_exchange_n((uint64_t *)(void *)object->host, &wire_expected, __builtin_bswap64(desired), 0,
                __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return result(may_race ? LWMUTEX_RACED : PS3_LWMUTEX_INCONSISTENT);
        return fast_settle(registry, object->address, object->attributes);
    }
#else
    (void)may_race;
#endif
    ps3_atomic_compare_result exchanged;
    ps3_guest_memory_status status = ps3_atomic_compare_exchange64(
        registry->domain, registry->objects[object_index].address, expected, desired, &exchanged);
    if (status != PS3_GUEST_MEMORY_OK) return memory_failure(status);
    return result(exchanged.exchanged ? PS3_LWMUTEX_OK : PS3_LWMUTEX_INCONSISTENT);
}

static ps3_lwmutex_result write_recursion(
    ps3_lwmutex_registry *registry, size_t object_index, uint32_t recursion)
{
#if LWMUTEX_FAST_AVAILABLE
    ps3_lwmutex_object *object = &registry->objects[object_index];
    if (object->host && !ps3_atomic_sticky_hit(object->address, 12)) {
        /* only the owner writes this word, so a plain atomic store is exclusive; then the reservation test */
        __atomic_store_n((uint32_t *)(void *)(object->host + 12), __builtin_bswap32(recursion), __ATOMIC_SEQ_CST);
        return fast_settle(registry, object->address, object->attributes);
    }
#endif
    unsigned char bytes[4];
    encode32(bytes, recursion);
    ps3_guest_memory_status status = ps3_atomic_write(registry->domain,
        registry->objects[object_index].address + 12, bytes, sizeof(bytes));
    return status == PS3_GUEST_MEMORY_OK ? result(PS3_LWMUTEX_OK) : memory_failure(status);
}

/* Registry guard prevents a waiter waking between pair publication and record
 * removal. A failed removal retains only registry-owned data, never its stack. */
static ps3_lwmutex_result remove_waiter(ps3_lwmutex_registry *registry, size_t index)
{
    size_t object_index = registry->waiters[index].object_index;
    guest_state state;
    ps3_lwmutex_result outcome = read_state(registry, object_index, &state);
    if (outcome.status == PS3_LWMUTEX_OK) {
        outcome = exchange_pair(registry, object_index, state, state.owner, state.waiters - 1, 0);   /* waiters >= 1: no fast CAS can change it */
    }
    if (outcome.status == PS3_LWMUTEX_OK) {
        registry->waiters[index] = (ps3_lwmutex_waiter){0};
        registry->wait.notify_all(registry->wait.context);
    } else {
        registry->waiters[index].state = WAITER_ABANDONED;
        registry->waiters[index].cancel = (ps3_wait_cancel){0};
        outcome.status = PS3_LWMUTEX_CLEANUP_REQUIRED;
    }
    return outcome;
}

static ps3_lwmutex_result reap_abandoned(ps3_lwmutex_registry *registry, size_t object_index)
{
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        if (belongs_to(registry, index, object_index) &&
            registry->waiters[index].state == WAITER_ABANDONED) {
            ps3_lwmutex_result outcome = remove_waiter(registry, index);
            if (outcome.status != PS3_LWMUTEX_OK) return outcome;
        }
    }
    return result(PS3_LWMUTEX_OK);
}

static int has_port_sleeper(ps3_lwmutex_registry *registry)
{
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        const unsigned state = registry->waiters[index].state;
        if (state == WAITER_LIVE || state == CONDITION_ELIGIBLE) return 1;
    }
    return 0;
}

static int cancelled(ps3_wait_cancel cancel)
{
    return cancel.is_cancelled && cancel.is_cancelled(cancel.context);
}

static size_t eligible_waiter(ps3_lwmutex_registry *registry, size_t object_index, uint64_t now)
{
    size_t chosen = registry->waiter_capacity;
    int priority = (registry->objects[object_index].attributes & 0xf) == PS3_LWMUTEX_PRIORITY;
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        ps3_lwmutex_waiter *candidate = &registry->waiters[index];
        if (!belongs_to(registry, index, object_index) || (candidate->state != WAITER_LIVE && candidate->state != CONDITION_ELIGIBLE) ||
            (candidate->deadline != UINT64_MAX && candidate->deadline <= now)) continue;
        if (chosen == registry->waiter_capacity ||
            (priority && candidate->thread.priority < registry->waiters[chosen].thread.priority) ||
            ((!priority || candidate->thread.priority == registry->waiters[chosen].thread.priority) &&
             candidate->sequence < registry->waiters[chosen].sequence)) chosen = index;
    }
    return chosen;
}

ps3_lwmutex_result ps3_lwmutex_init_with_conditions(
    ps3_lwmutex_registry *registry, ps3_atomic_domain *domain,
    ps3_instance_guard guard, ps3_wait_port wait,
    ps3_lwmutex_object *objects, size_t object_capacity,
    ps3_lwmutex_waiter *waiters, size_t waiter_capacity,
    size_t condition_binding_capacity)
{
    if (!registry || !domain || !domain->initialized || guard.context == domain->guard.context || !guard.lock || !guard.unlock ||
        !wait.now || !wait.wait || !wait.notify_all || !objects || !object_capacity ||
        !waiters || !waiter_capacity || object_capacity > SIZE_MAX / sizeof(*objects) ||
        waiter_capacity > UINT32_MAX || waiter_capacity > SIZE_MAX / sizeof(*waiters)) {
        return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    }
    memset(objects, 0, object_capacity * sizeof(*objects));
    memset(waiters, 0, waiter_capacity * sizeof(*waiters));
    *registry = (ps3_lwmutex_registry){.domain = domain, .guard = guard, .wait = wait,
        .objects = objects, .object_capacity = object_capacity, .waiters = waiters,
        .waiter_capacity = waiter_capacity, .condition_binding_capacity = condition_binding_capacity, .next_sequence = 1, .next_reservation = 1, .next_queue_id = 1, .initialized = 1};
    return result(PS3_LWMUTEX_OK);
}

ps3_lwmutex_result ps3_lwmutex_init(
    ps3_lwmutex_registry *registry, ps3_atomic_domain *domain,
    ps3_instance_guard guard, ps3_wait_port wait,
    ps3_lwmutex_object *objects, size_t object_capacity,
    ps3_lwmutex_waiter *waiters, size_t waiter_capacity)
{
    return ps3_lwmutex_init_with_conditions(registry,domain,guard,wait,
        objects,object_capacity,waiters,waiter_capacity,0);
}

static const ps3_lwmutex_registry *g_fast_registry;   /* the one registry of a process that enabled the fast path (diagnostics) */
ps3_lwmutex_result ps3_lwmutex_enable_fast_path(ps3_lwmutex_registry *registry, ps3_lwmutex_fast_port port)
{
#if LWMUTEX_FAST_AVAILABLE
    if (!valid_registry(registry) || !port.pointer) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    registry->fast = port;
    g_fast_registry = registry;
    registry->guard.unlock(registry->guard.context);
    return result(PS3_LWMUTEX_OK);
#else
    (void)registry; (void)port;
    return result(PS3_LWMUTEX_UNSUPPORTED);
#endif
}

void ps3_lwmutex_fast_totals(const ps3_lwmutex_registry *registry, uint64_t *locks, uint64_t *unlocks, uint64_t *fallbacks)
{
    uint64_t l = 0, u = 0, f = 0;
    if (!registry) registry = g_fast_registry;
    if (registry) for (size_t index = 0; index < PS3_LWMUTEX_FAST_SLOTS; ++index) {
        l += __atomic_load_n(&registry->fast_table[index].locks, __ATOMIC_RELAXED); u += __atomic_load_n(&registry->fast_table[index].unlocks, __ATOMIC_RELAXED);
        for (unsigned why = 0; why < PS3_LWMUTEX_WHY_COUNT; ++why) f += __atomic_load_n(&registry->fast_table[index].why[why], __ATOMIC_RELAXED);
    }
    if (locks) *locks = l;
    if (unlocks) *unlocks = u;
    if (fallbacks) *fallbacks = f;
}

void ps3_lwmutex_fast_reasons(const ps3_lwmutex_registry *registry, uint64_t reasons[PS3_LWMUTEX_WHY_COUNT])
{
    for (unsigned why = 0; why < PS3_LWMUTEX_WHY_COUNT; ++why) reasons[why] = 0;
    if (!registry) registry = g_fast_registry;
    if (registry) for (size_t index = 0; index < PS3_LWMUTEX_FAST_SLOTS; ++index)
        for (unsigned why = 0; why < PS3_LWMUTEX_WHY_COUNT; ++why) reasons[why] += __atomic_load_n(&registry->fast_table[index].why[why], __ATOMIC_RELAXED);
}

static ps3_lwmutex_result release_object(ps3_lwmutex_registry *registry, size_t index)
{
    ps3_guest_memory_status status = ps3_atomic_pin_release(registry->domain, &registry->objects[index].pin);
    if (status != PS3_GUEST_MEMORY_OK) {
        ps3_lwmutex_result outcome = memory_failure(status);
        outcome.status = PS3_LWMUTEX_CLEANUP_REQUIRED;
        return outcome;
    }
    uint64_t generation = registry->objects[index].generation;
    registry->objects[index] = (ps3_lwmutex_object){.generation = generation};
    return result(PS3_LWMUTEX_OK);
}

ps3_lwmutex_result ps3_lwmutex_create(
    ps3_lwmutex_registry *registry, uint32_t address,
    uint32_t protocol, uint32_t recursive)
{
    if (!valid_registry(registry) || address % 8 ||
        (uint64_t)address + PS3_LWMUTEX_BYTES > PS3_GUEST_ADDRESS_BYTES ||
        (recursive != PS3_LWMUTEX_RECURSIVE && recursive != PS3_LWMUTEX_NONRECURSIVE)) {
        return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    }
    if (protocol != PS3_LWMUTEX_FIFO && protocol != PS3_LWMUTEX_PRIORITY) return result(PS3_LWMUTEX_UNSUPPORTED);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    size_t empty = registry->object_capacity;
    for (size_t index = 0; index < registry->object_capacity; ++index) {
        ps3_lwmutex_object *object = &registry->objects[index];
        if (object->state && (uint64_t)address < (uint64_t)object->address + PS3_LWMUTEX_BYTES &&
            (uint64_t)object->address < (uint64_t)address + PS3_LWMUTEX_BYTES) {
            outcome = result(object->state == OBJECT_LIVE ?
                PS3_LWMUTEX_ALREADY_EXISTS : PS3_LWMUTEX_CLEANUP_REQUIRED);
            goto done;
        }
        if (!object->state && object->generation != UINT64_MAX && empty == registry->object_capacity) empty = index;
    }
    if (empty == registry->object_capacity || !registry->next_queue_id ||
        registry->next_queue_id >= PS3_LWMUTEX_RESERVED) goto done;
    ps3_atomic_pin pin;
    ps3_guest_memory_status status = ps3_atomic_pin_acquire(registry->domain, address, PS3_LWMUTEX_BYTES, &pin);
    if (status != PS3_GUEST_MEMORY_OK) {
        outcome = memory_failure(status);
        goto done;
    }
    ps3_lwmutex_object *object = &registry->objects[empty];
    *object = (ps3_lwmutex_object){.generation = object->generation + 1, .address = address,
        .attributes = protocol | recursive, .queue_id = registry->next_queue_id++,
        .state = OBJECT_UNPUBLISHED, .pin = pin};
    unsigned char image[PS3_LWMUTEX_BYTES] = {0};
    encode32(image, PS3_LWMUTEX_FREE);
    encode32(image + 8, object->attributes);
    encode32(image + 16, object->queue_id);
    status = ps3_atomic_write(registry->domain, address, image, sizeof(image));
    if (status != PS3_GUEST_MEMORY_OK) {
        outcome = release_object(registry, empty);
        if (outcome.status == PS3_LWMUTEX_OK) outcome = memory_failure(status);
        goto done;
    }
    object->state = OBJECT_LIVE;
#if LWMUTEX_FAST_AVAILABLE
    /* Fast-eligible: the registry has a fast port, the pair word and the attributes word (the invalidation write) lie in one
     * 128-byte line, and that line never held an SPU reservation. From here on the pair changes by host CAS only. */
    if (registry->fast.pointer && address && (address & 127u) <= 112u && !ps3_atomic_sticky_hit(address, 12)) {
        object->host = registry->fast.pointer(registry->fast.context, address, PS3_LWMUTEX_BYTES);
        fast_publish(registry, object);
    }
#endif
    outcome = result(PS3_LWMUTEX_OK);
done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static ps3_lwmutex_result initial_acquire(
    ps3_lwmutex_registry *registry, size_t object_index, ps3_lwmutex_thread thread,
    uint64_t deadline, ps3_wait_cancel cancel, int check_clock)
{
    for (;;) {   /* repeats only when a lock-free operation changed the pair between the read and the compare-and-swap */
        ps3_lwmutex_result outcome = reap_abandoned(registry, object_index);
        if (outcome.status != PS3_LWMUTEX_OK) return outcome;
        guest_state state;
        outcome = read_state(registry, object_index, &state);
        if (outcome.status != PS3_LWMUTEX_OK) return outcome;
        /* With the fast path on, the clock is read only when there is a deadline to compare with (a lock without timeout never
         * needed it but its failure): one 128-bit divide less on every guarded lock. */
#if LWMUTEX_FAST_AVAILABLE
        if (check_clock && (deadline != UINT64_MAX || !registry->fast.pointer)) {
#else
        if (check_clock) {
#endif
            uint64_t now;
            ps3_wait_status status = registry->wait.now(registry->wait.context, &now);
            if (status != PS3_WAIT_READY) return wait_failure(status);
            if (cancelled(cancel)) return wait_failure(PS3_WAIT_CANCELLED);
            if (deadline != UINT64_MAX && now >= deadline) return wait_failure(PS3_WAIT_TIMEOUT);
        }
#if LWMUTEX_FAST_AVAILABLE
        else if (check_clock && cancelled(cancel)) return wait_failure(PS3_WAIT_CANCELLED);
#endif
        if (state.owner == thread.id) {
            if (!(state.attributes & PS3_LWMUTEX_RECURSIVE)) return result(PS3_LWMUTEX_DEADLOCK);
            if (state.recursion == UINT32_MAX) return result(PS3_LWMUTEX_OVERFLOW);
            return write_recursion(registry, object_index, state.recursion + 1);
        }
        if (state.owner != PS3_LWMUTEX_FREE || state.waiters) return result(PS3_LWMUTEX_BUSY);
        outcome = exchange_pair(registry, object_index, state, thread.id, 0, 1);
#if LWMUTEX_FAST_AVAILABLE
        if (outcome.status == LWMUTEX_RACED) continue;
#endif
        return outcome;
    }
}

ps3_lwmutex_result ps3_lwmutex_try_lock(
    ps3_lwmutex_registry *registry, uint32_t address, ps3_lwmutex_thread thread)
{
    if (!valid_registry(registry) || !valid_thread(thread)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
#if LWMUTEX_FAST_AVAILABLE
    if (registry->fast.pointer) {
        ps3_lwmutex_result fast;
        if (fast_lock_try(registry, address, thread.id, (ps3_wait_cancel){0}, 1, &fast)) return fast;
    }
#endif
    registry->guard.lock(registry->guard.context);
    size_t object_index = find_object(registry, address);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_NOT_FOUND);
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        if (registry->waiters[index].state && registry->waiters[index].thread.id == thread.id) {
            registry->guard.unlock(registry->guard.context);
            return result(PS3_LWMUTEX_BUSY);
        }
    }
    if (object_index < registry->object_capacity) {
        outcome = registry->objects[object_index].state == OBJECT_LIVE ?
            initial_acquire(registry, object_index, thread, UINT64_MAX, (ps3_wait_cancel){0}, 0) : result(PS3_LWMUTEX_CLEANUP_REQUIRED);
    }
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static ps3_lwmutex_result calculate_deadline(
    ps3_lwmutex_registry *registry, uint64_t timeout_us, uint64_t *deadline)
{
    if (!timeout_us) {
        *deadline = UINT64_MAX;
        return result(PS3_LWMUTEX_OK);
    }
    uint64_t now;
    ps3_wait_status status = registry->wait.now(registry->wait.context, &now);
    if (status != PS3_WAIT_READY) return wait_failure(status);
    if (now == UINT64_MAX || timeout_us > (UINT64_MAX - 1 - now) / 1000) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    *deadline = now + timeout_us * 1000;
    return result(PS3_LWMUTEX_OK);
}

ps3_lwmutex_result ps3_lwmutex_lock(
    ps3_lwmutex_registry *registry, uint32_t address,
    ps3_lwmutex_thread thread, uint64_t timeout_us, ps3_wait_cancel cancel)
{
    if (!valid_registry(registry) || !valid_thread(thread)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
#if LWMUTEX_FAST_AVAILABLE
    if (registry->fast.pointer && timeout_us <= FAST_MAX_TIMEOUT_US) {
        ps3_lwmutex_result fast;
        if (fast_lock_try(registry, address, thread.id, cancel, 0, &fast)) return fast;
    }
#endif
    /* The guard-wait timing (two clock reads, each a 128-bit divide on the console) is a diagnostic; with the fast path on it is
     * off unless PS3_LWMUTEX_GUARD_TIMING=1, and the contended time is then measured from the moment the thread registers. */
    const int timing = PS3_LWMUTEX_GUARD_TIMING || !registry->fast.pointer;
    uint64_t debug_before = 0, debug_inside = 0;
    if (timing) (void)registry->wait.now(registry->wait.context, &debug_before);
    registry->guard.lock(registry->guard.context);
    if (timing) (void)registry->wait.now(registry->wait.context, &debug_inside);
    ++g_ps3_lwmutex_debug.lock_calls;
    if (timing && debug_inside > debug_before) {
        const uint64_t waited = debug_inside - debug_before;
        g_ps3_lwmutex_debug.guard_ns += waited;
        if (waited > g_ps3_lwmutex_debug.guard_max_ns) g_ps3_lwmutex_debug.guard_max_ns = waited;
        if (waited > 1000000u) ++g_ps3_lwmutex_debug.guard_over_1ms;
    }
    size_t object_index = find_object(registry, address);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_NOT_FOUND);
    if (object_index == registry->object_capacity) goto done;
    ps3_lwmutex_object *object = &registry->objects[object_index];
    if (object->state != OBJECT_LIVE) {
        outcome = result(PS3_LWMUTEX_CLEANUP_REQUIRED);
        goto done;
    }
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        if (registry->waiters[index].state && registry->waiters[index].thread.id == thread.id) {
            outcome = result(PS3_LWMUTEX_BUSY);
            goto done;
        }
    }
    uint64_t deadline = UINT64_MAX;
    outcome = calculate_deadline(registry, timeout_us, &deadline);
    if (outcome.status != PS3_LWMUTEX_OK) goto done;
    if (cancelled(cancel)) {
        outcome = wait_failure(PS3_WAIT_CANCELLED);
        goto done;
    }
    outcome = initial_acquire(registry, object_index, thread, deadline, cancel, 1);
    if (outcome.status != PS3_LWMUTEX_BUSY) goto done;
    size_t waiter_index = registry->waiter_capacity;
    for (size_t index = 0; index < registry->waiter_capacity; ++index) {
        if (!registry->waiters[index].state) { waiter_index = index; break; }
    }
    if (waiter_index == registry->waiter_capacity || registry->next_sequence == UINT64_MAX) {
        outcome = result(PS3_LWMUTEX_RESOURCE_EXHAUSTED);
        goto done;
    }
    guest_state state;
    for (;;) {   /* again only when a lock-free operation changed the pair under us: the owner may have just let go, so decide anew */
        outcome = read_state(registry, object_index, &state);
        if (outcome.status != PS3_LWMUTEX_OK) goto done;
        outcome = exchange_pair(registry, object_index, state, state.owner, state.waiters + 1, 1);
#if LWMUTEX_FAST_AVAILABLE
        if (outcome.status == LWMUTEX_RACED) {
            outcome = initial_acquire(registry, object_index, thread, deadline, cancel, 1);
            if (outcome.status != PS3_LWMUTEX_BUSY) goto done;
            continue;
        }
#endif
        break;
    }
    if (outcome.status != PS3_LWMUTEX_OK) goto done;
    if (!debug_inside) (void)registry->wait.now(registry->wait.context, &debug_inside);
    registry->waiters[waiter_index] = (ps3_lwmutex_waiter){.object_index = object_index,
        .object_generation = object->generation, .sequence = registry->next_sequence++,
        .deadline = deadline, .thread = thread, .cancel = cancel, .state = WAITER_LIVE};
    ++object->active_operations;
    const uint32_t debug_owner = state.owner;
    if (waiter_index < PS3_LWMUTEX_DEBUG_PENDING)   /* who waits for what right now; cleared on every way out */
        g_ps3_lwmutex_debug.pending[waiter_index] = (struct ps3_lwmutex_debug_pending){address, debug_owner, thread.id, debug_inside};
    for (;;) {
        uint64_t now;
        ps3_wait_status status = registry->wait.now(registry->wait.context, &now);
        if (status != PS3_WAIT_READY) { outcome = wait_failure(status); break; }
        if (cancelled(cancel)) { outcome = wait_failure(PS3_WAIT_CANCELLED); break; }
        if (deadline != UINT64_MAX && now >= deadline) { outcome = wait_failure(PS3_WAIT_TIMEOUT); break; }
        outcome = reap_abandoned(registry, object_index);
        if (outcome.status != PS3_LWMUTEX_OK) break;
        outcome = read_state(registry, object_index, &state);
        if (outcome.status != PS3_LWMUTEX_OK) break;
        if (state.owner == PS3_LWMUTEX_FREE && eligible_waiter(registry, object_index, now) == waiter_index) {
            /* Eligibility/deadline observation under registry guard is the
             * acquisition decision; CAS is its successful publication point. */
            status = registry->wait.now(registry->wait.context, &now);
            if (status != PS3_WAIT_READY) { outcome = wait_failure(status); break; }
            if (cancelled(cancel)) { outcome = wait_failure(PS3_WAIT_CANCELLED); break; }
            if (deadline != UINT64_MAX && now >= deadline) { outcome = wait_failure(PS3_WAIT_TIMEOUT); break; }
            outcome = exchange_pair(registry, object_index, state, thread.id, state.waiters - 1, 0);   /* waiters >= 1 */
            if (outcome.status != PS3_LWMUTEX_OK) break;
            registry->waiters[waiter_index] = (ps3_lwmutex_waiter){0};
            --object->active_operations;
            debug_contended(address, debug_owner, thread.id, now > debug_inside ? now - debug_inside : 0);
            if (waiter_index < PS3_LWMUTEX_DEBUG_PENDING) g_ps3_lwmutex_debug.pending[waiter_index].address = 0;
            goto done;
        }
        status = registry->wait.wait(registry->wait.context, &registry->guard, deadline, cancel);
        /* Timed adapters may wake early at their polling deadline. Only the
         * registry clock/deadline observation decides actual timeout expiry. */
        if (status == PS3_WAIT_TIMEOUT) continue;
        if (status != PS3_WAIT_READY) { outcome = wait_failure(status); break; }
    }
    if (waiter_index < PS3_LWMUTEX_DEBUG_PENDING) g_ps3_lwmutex_debug.pending[waiter_index].address = 0;
    ps3_lwmutex_result removed = remove_waiter(registry, waiter_index);
    --object->active_operations;
    if (removed.status != PS3_LWMUTEX_OK) {
        removed.wait_status = outcome.wait_status;
        outcome = removed;
    }
done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_unlock(
    ps3_lwmutex_registry *registry, uint32_t address, ps3_lwmutex_thread thread)
{
    if (!valid_registry(registry) || !valid_thread(thread)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
#if LWMUTEX_FAST_AVAILABLE
    if (registry->fast.pointer) {
        ps3_lwmutex_result fast;
        if (fast_unlock_try(registry, address, thread.id, &fast)) return fast;
    }
#endif
    registry->guard.lock(registry->guard.context);
    size_t object_index = find_object(registry, address);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_NOT_FOUND);
    if (object_index == registry->object_capacity) goto done;
    if (registry->objects[object_index].state != OBJECT_LIVE) {
        outcome = result(PS3_LWMUTEX_CLEANUP_REQUIRED);
        goto done;
    }
    outcome = reap_abandoned(registry, object_index);
    if (outcome.status != PS3_LWMUTEX_OK) goto done;
    guest_state state;
    outcome = read_state(registry, object_index, &state);
    if (outcome.status != PS3_LWMUTEX_OK) goto done;
    if (state.owner != thread.id) { outcome = result(PS3_LWMUTEX_NOT_OWNER); goto done; }
    if (state.recursion) outcome = write_recursion(registry, object_index, state.recursion - 1);
    else {
        outcome = exchange_pair(registry, object_index, state, PS3_LWMUTEX_FREE, state.waiters, 0);   /* the owner holds it: nothing else changes the pair */
        /* Root (Decision 86): wake only when somebody sleeps on this port. Every unlock used to make a kernel
         * call that woke nobody. A sleeper that registers after this point holds the guard first and reads the
         * state this unlock has already published, and the port polls every 2 ms besides. */
        if (outcome.status == PS3_LWMUTEX_OK) {
            ++g_ps3_lwmutex_debug.unlock_calls;
            if (has_port_sleeper(registry)) registry->wait.notify_all(registry->wait.context);
            else ++g_ps3_lwmutex_debug.notify_skipped;
        }
    }
done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static ps3_lwmutex_result retire_object_inner(ps3_lwmutex_registry *registry, size_t object_index)
{
    ps3_lwmutex_object *object = &registry->objects[object_index];
    if (object->state != OBJECT_LIVE) return release_object(registry, object_index);
    if (object->active_operations || object->condition_bindings) return result(PS3_LWMUTEX_BUSY);
    ps3_lwmutex_result outcome = reap_abandoned(registry, object_index);
    if (outcome.status != PS3_LWMUTEX_OK) return outcome;
    guest_state state;
    for (;;) {   /* again only when a lock-free lock took the mutex between the read and the compare-and-swap: then it is BUSY */
        outcome = read_state(registry, object_index, &state);
        if (outcome.status != PS3_LWMUTEX_OK) return outcome;
        if (state.owner != PS3_LWMUTEX_FREE || state.waiters) return result(PS3_LWMUTEX_BUSY);
        outcome = exchange_pair(registry, object_index, state, PS3_LWMUTEX_DEAD, 0, 1);
#if LWMUTEX_FAST_AVAILABLE
        if (outcome.status == LWMUTEX_RACED) continue;
#endif
        break;
    }
    if (outcome.status != PS3_LWMUTEX_OK) return outcome;
    object->state = OBJECT_RETIRING;
    return release_object(registry, object_index);
}

static ps3_lwmutex_result retire_object(ps3_lwmutex_registry *registry, size_t object_index)
{
#if LWMUTEX_FAST_AVAILABLE
    /* Hide the fast entry first so no new lock-free operation starts on a mutex that is being destroyed; one that already
     * started either wins the compare-and-swap (the mutex is then held: BUSY and the entry comes back) or finds DEAD. */
    ps3_lwmutex_object *object = &registry->objects[object_index];
    const int hidden = object->state == OBJECT_LIVE && object->host;
    if (hidden) fast_unpublish(registry, object);
    ps3_lwmutex_result outcome = retire_object_inner(registry, object_index);
    if (hidden && outcome.status != PS3_LWMUTEX_OK && registry->objects[object_index].state == OBJECT_LIVE) fast_publish(registry, &registry->objects[object_index]);
    return outcome;
#else
    return retire_object_inner(registry, object_index);
#endif
}

ps3_lwmutex_result ps3_lwmutex_destroy(ps3_lwmutex_registry *registry, uint32_t address)
{
    if (!valid_registry(registry)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    size_t object_index = find_object(registry, address);
    ps3_lwmutex_result outcome = object_index == registry->object_capacity ?
        result(PS3_LWMUTEX_NOT_FOUND) : retire_object(registry, object_index);
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_cleanup(ps3_lwmutex_registry *registry)
{
    if (!valid_registry(registry)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_OK);
    for (size_t index = 0; index < registry->object_capacity; ++index) {
        if (!registry->objects[index].state) continue;
        ps3_lwmutex_result retired = retire_object(registry, index);
        if (outcome.status == PS3_LWMUTEX_OK && retired.status != PS3_LWMUTEX_OK) outcome = retired;
    }
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static int report_has_space(const ps3_sync_publication_report *report)
{
    return report && report->count < PS3_SYNC_PUBLICATION_CAPACITY;
}

/* before: the state the caller read; state: the state to publish (the report gets its 24-byte image, as ever). With the fast
 * path the pair is changed by compare-and-swap from `before` (not by writing the whole image over a word a lock-free
 * operation may be changing), and the recursion word, which only the owner writes, by a separate store: BEFORE the pair when
 * the mutex is being released (a reader must never see a free mutex with recursion), after it otherwise.
 * may_race: see exchange_pair. */
static ps3_lwmutex_result publish_condition_state(ps3_lwmutex_registry *registry,
    size_t object, guest_state before, guest_state state, ps3_sync_publication_report *report, int may_race)
{
    unsigned char image[PS3_LWMUTEX_BYTES];
    encode32(image, state.owner); encode32(image + 4, state.waiters);
    encode32(image + 8, state.attributes); encode32(image + 12, state.recursion);
    encode32(image + 16, state.queue_id); encode32(image + 20, state.padding);
#if LWMUTEX_FAST_AVAILABLE
    if (registry->objects[object].host && !ps3_atomic_sticky_hit(registry->objects[object].address, 12)) {
        const int releasing = state.owner == PS3_LWMUTEX_FREE && before.owner != PS3_LWMUTEX_FREE;
        ps3_lwmutex_result outcome = result(PS3_LWMUTEX_OK);
        if (releasing && state.recursion != before.recursion) outcome = write_recursion(registry, object, state.recursion);
        if (outcome.status == PS3_LWMUTEX_OK && (state.owner != before.owner || state.waiters != before.waiters))
            outcome = exchange_pair(registry, object, before, state.owner, state.waiters, may_race);
        if (outcome.status == PS3_LWMUTEX_OK && !releasing && state.recursion != before.recursion)
            outcome = write_recursion(registry, object, state.recursion);
        if (outcome.status != PS3_LWMUTEX_OK) return outcome;
    } else
#else
    (void)before; (void)may_race;
#endif
    {
        ps3_guest_memory_status status = ps3_atomic_write(registry->domain,
            registry->objects[object].address, image, sizeof(image));
        if (status != PS3_GUEST_MEMORY_OK) return memory_failure(status);
    }
    ps3_sync_publication *entry = &report->writes[report->count++];
    entry->address = registry->objects[object].address;
    entry->size = sizeof(image);
    memcpy(entry->bytes, image, sizeof(image));
    return result(PS3_LWMUTEX_OK);
}

static int valid_binding(ps3_lwmutex_registry *registry,
    const ps3_lwmutex_condition_binding *binding)
{
    if (!binding || binding->registry != registry || !binding->retained ||
        binding->object_index >= registry->object_capacity) return 0;
    ps3_lwmutex_object *object = &registry->objects[binding->object_index];
    if (object->state != OBJECT_LIVE || object->generation != binding->object_generation ||
        object->address != binding->address || object->attributes != binding->attributes) return 0;
    ps3_lwmutex_condition_binding *entry=object->condition_bindings;
    for (size_t visited=0; entry && visited<registry->condition_binding_capacity; ++visited) {
        if (entry == binding) return 1;
        entry=entry->next;
    }
    return 0;
}

static ps3_lwmutex_waiter *condition_record(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token)
{
    if (!token || token->registry != registry ||
        token->waiter_index >= registry->waiter_capacity) return NULL;
    ps3_lwmutex_waiter *record = &registry->waiters[token->waiter_index];
    if (record->state < CONDITION_DORMANT || record->condition_authority != token ||
        record->object_generation != token->object_generation ||
        record->reservation_identity != token->reservation_identity ||
        !valid_binding(registry, record->condition_binding)) return NULL;
    return record;
}

ps3_lwmutex_result ps3_lwmutex_condition_bind(ps3_lwmutex_registry *registry,
    uint32_t address, ps3_lwmutex_condition_binding *binding)
{
    if (!valid_registry(registry) || !binding || binding->retained || binding->registry)
        return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    size_t object = find_object(registry, address);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_NOT_FOUND);
    if (registry->condition_binding_count>=registry->condition_binding_capacity) {
        outcome=result(PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    } else if (object < registry->object_capacity) {
        if (registry->objects[object].state != OBJECT_LIVE) outcome = result(PS3_LWMUTEX_CLEANUP_REQUIRED);
        else {
            guest_state state;
            outcome = read_state(registry, object, &state);
            if (outcome.status == PS3_LWMUTEX_OK) {
                *binding = (ps3_lwmutex_condition_binding){.registry=registry,
                    .next=registry->objects[object].condition_bindings, .object_index=object,
                    .object_generation=registry->objects[object].generation, .address=address,
                    .attributes=state.attributes, .retained=1};
                registry->objects[object].condition_bindings = binding;
                ++registry->condition_binding_count;
            }
        }
    }
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_unbind(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_binding *binding)
{
    if (!valid_registry(registry)) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_INVALID_ARGUMENT);
    if (valid_binding(registry, binding)) {
        outcome = result(PS3_LWMUTEX_BUSY);
        if (!binding->reservations) {
            ps3_lwmutex_condition_binding **entry = &registry->objects[binding->object_index].condition_bindings;
            for (size_t visited=0; *entry != binding && visited<registry->condition_binding_capacity; ++visited)
                entry = &(*entry)->next;
            *entry = binding->next;
            --registry->condition_binding_count;
            *binding = (ps3_lwmutex_condition_binding){0};
            outcome = result(PS3_LWMUTEX_OK);
        }
    }
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_release(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_binding *binding, ps3_lwmutex_thread thread,
    ps3_lwmutex_condition_wait *token, ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !valid_thread(thread) || !token || token->state || token->registry || !report)
        return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome = result(PS3_LWMUTEX_INVALID_ARGUMENT);
    if (!valid_binding(registry, binding)) goto release_done;
    if (!report_has_space(report)) { outcome=result(PS3_LWMUTEX_RESOURCE_EXHAUSTED); goto release_done; }
    size_t slot = registry->waiter_capacity;
    for (size_t index=0; index<registry->waiter_capacity; ++index) {
        if (registry->waiters[index].state && registry->waiters[index].thread.id == thread.id) {
            outcome=result(PS3_LWMUTEX_BUSY); goto release_done;
        }
        if (!registry->waiters[index].state && slot==registry->waiter_capacity) slot=index;
    }
    if (slot==registry->waiter_capacity || registry->next_reservation==UINT64_MAX ||
        binding->reservations==SIZE_MAX || registry->objects[binding->object_index].active_operations==UINT_MAX) {
        outcome=result(PS3_LWMUTEX_RESOURCE_EXHAUSTED); goto release_done;
    }
    guest_state state;
    outcome=read_state(registry,binding->object_index,&state);
    if (outcome.status!=PS3_LWMUTEX_OK) goto release_done;
    if (state.owner!=thread.id) { outcome=result(PS3_LWMUTEX_NOT_OWNER); goto release_done; }
    uint32_t recursion=state.recursion;
    const guest_state before=state;
    state.owner=PS3_LWMUTEX_FREE; state.recursion=0;
    outcome=publish_condition_state(registry,binding->object_index,before,state,report,0);   /* the caller owns it: nothing races */
    if (outcome.status!=PS3_LWMUTEX_OK) goto release_done;
    uint64_t identity=registry->next_reservation++;
    registry->waiters[slot]=(ps3_lwmutex_waiter){.object_index=binding->object_index,
        .object_generation=binding->object_generation,.deadline=UINT64_MAX,.thread=thread,
        .state=CONDITION_DORMANT,.condition_authority=token,.condition_binding=binding,
        .reservation_identity=identity,.saved_recursion=recursion};
    *token=(ps3_lwmutex_condition_wait){.registry=registry,.waiter_index=slot,
        .object_generation=binding->object_generation,.reservation_identity=identity,
        .state=PS3_LWMUTEX_CONDITION_DORMANT};
    ++binding->reservations; ++registry->objects[binding->object_index].active_operations;
    registry->wait.notify_all(registry->wait.context);
release_done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static ps3_lwmutex_result publish_condition_admission(ps3_lwmutex_registry *registry,
    ps3_lwmutex_waiter *record, uint64_t sequence, ps3_sync_publication_report *report)
{
    guest_state state, before;
    ps3_lwmutex_result outcome;
    for (;;) {   /* again only when a lock-free lock or unlock changed the pair (waiters == 0) between the read and the compare-and-swap */
        outcome=read_state(registry,record->object_index,&state);
        if (outcome.status!=PS3_LWMUTEX_OK) return outcome;
        if (state.waiters==UINT32_MAX) return result(PS3_LWMUTEX_INCONSISTENT);
        before=state;
        ++state.waiters;
        outcome=publish_condition_state(registry,record->object_index,before,state,report,1);
#if LWMUTEX_FAST_AVAILABLE
        if (outcome.status==LWMUTEX_RACED) continue;
#endif
        break;
    }
    if (outcome.status==PS3_LWMUTEX_OK) {
        record->sequence=sequence;
        record->state=CONDITION_ELIGIBLE;
        record->condition_counted=1;
        record->condition_authority->state=PS3_LWMUTEX_CONDITION_ELIGIBLE;
    }
    return outcome;
}

static ps3_lwmutex_result promote_condition(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token, ps3_sync_publication_report *report)
{
    ps3_lwmutex_waiter *record=condition_record(registry,token);
    if (!record) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    if (record->state==CONDITION_ELIGIBLE) return result(PS3_LWMUTEX_OK);
    if (record->state!=CONDITION_DORMANT) return result(PS3_LWMUTEX_CLEANUP_REQUIRED);
    if (!report_has_space(report) || registry->next_sequence==UINT64_MAX)
        return result(PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    ps3_lwmutex_result outcome=publish_condition_admission(registry,record,registry->next_sequence,report);
    if (outcome.status==PS3_LWMUTEX_OK) ++registry->next_sequence;
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_promote(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token, ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !report) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome=promote_condition(registry,token,report);
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static ps3_lwmutex_result abort_condition(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token, ps3_sync_publication_report *report)
{
    ps3_lwmutex_waiter *record=condition_record(registry,token);
    if (!record) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    if (record->state==CONDITION_POISONED) return result(PS3_LWMUTEX_CLEANUP_REQUIRED);
    ps3_lwmutex_result outcome=result(PS3_LWMUTEX_OK);
    if (record->condition_counted && report_has_space(report)) {
        guest_state state;
        outcome=read_state(registry,record->object_index,&state);
        if (outcome.status==PS3_LWMUTEX_OK) {
            const guest_state before=state;
            --state.waiters;
            outcome=publish_condition_state(registry,record->object_index,before,state,report,0);   /* counted: waiters >= 1 */
            if (outcome.status==PS3_LWMUTEX_OK) record->condition_counted=0;
        }
    }
    record->state=CONDITION_POISONED;
    record->cancel=(ps3_wait_cancel){0};
    token->state=PS3_LWMUTEX_CONDITION_POISONED;
    --registry->objects[record->object_index].active_operations;
    registry->wait.notify_all(registry->wait.context);
    outcome.status=PS3_LWMUTEX_CLEANUP_REQUIRED;
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_abort(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token, ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !report) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome=abort_condition(registry,token,report);
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

static void finish_condition(ps3_lwmutex_registry *registry, size_t slot)
{
    ps3_lwmutex_waiter *record=&registry->waiters[slot];
    --record->condition_binding->reservations;
    *record->condition_authority=(ps3_lwmutex_condition_wait){0};
    *record=(ps3_lwmutex_waiter){0};
}

ps3_lwmutex_result ps3_lwmutex_condition_reacquire(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *token, ps3_wait_cancel stop,
    ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !report) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_waiter *record=condition_record(registry,token);
    ps3_lwmutex_result outcome=result(PS3_LWMUTEX_INVALID_ARGUMENT);
    if (!record) goto reacquire_done;
    if (!report_has_space(report)) { outcome=abort_condition(registry,token,report); goto reacquire_done; }
    if (record->state!=CONDITION_ELIGIBLE) {
        outcome=result(record->state==CONDITION_POISONED ? PS3_LWMUTEX_CLEANUP_REQUIRED : PS3_LWMUTEX_INVALID_ARGUMENT);
        goto reacquire_done;
    }
    for (;;) {
        uint64_t now;
        if (cancelled(stop)) { outcome=wait_failure(PS3_WAIT_CANCELLED); break; }
        ps3_wait_status status=registry->wait.now(registry->wait.context,&now);
        if (status!=PS3_WAIT_READY) { outcome=wait_failure(status); break; }
        guest_state state;
        outcome=read_state(registry,record->object_index,&state);
        if (outcome.status!=PS3_LWMUTEX_OK) break;
        if (state.owner==PS3_LWMUTEX_FREE && eligible_waiter(registry,record->object_index,now)==token->waiter_index) {
            if (cancelled(stop)) { outcome=wait_failure(PS3_WAIT_CANCELLED); break; }
            const guest_state before=state;
            state.owner=record->thread.id; state.recursion=record->saved_recursion; --state.waiters;
            outcome=publish_condition_state(registry,record->object_index,before,state,report,0);   /* counted: waiters >= 1 */
            if (outcome.status!=PS3_LWMUTEX_OK) break;
            --registry->objects[record->object_index].active_operations;
            finish_condition(registry,token->waiter_index);
            goto reacquire_done;
        }
        status=registry->wait.wait(registry->wait.context,&registry->guard,UINT64_MAX,stop);
        if (status==PS3_WAIT_TIMEOUT || status==PS3_WAIT_READY) continue;
        outcome=wait_failure(status); break;
    }
    {
        ps3_lwmutex_result aborted=abort_condition(registry,token,report);
        if (aborted.memory_status==PS3_GUEST_MEMORY_OK) aborted.memory_status=outcome.memory_status;
        aborted.wait_status=outcome.wait_status;
        outcome=aborted;
    }
reacquire_done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_cleanup(ps3_lwmutex_registry *registry,
    ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !report) return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome=result(PS3_LWMUTEX_OK);
    for (size_t slot=0; slot<registry->waiter_capacity; ++slot) {
        ps3_lwmutex_waiter *record=&registry->waiters[slot];
        if (record->state!=CONDITION_POISONED) continue;
        if (record->condition_counted) {
            if (!report_has_space(report)) { outcome=result(PS3_LWMUTEX_RESOURCE_EXHAUSTED); break; }
            guest_state state;
            outcome=read_state(registry,record->object_index,&state);
            if (outcome.status!=PS3_LWMUTEX_OK) break;
            const guest_state before=state;
            --state.waiters;
            outcome=publish_condition_state(registry,record->object_index,before,state,report,0);
            if (outcome.status!=PS3_LWMUTEX_OK) { outcome.status=PS3_LWMUTEX_CLEANUP_REQUIRED; break; }
        }
        finish_condition(registry,slot);
    }
    registry->guard.unlock(registry->guard.context);
    return outcome;
}

ps3_lwmutex_result ps3_lwmutex_condition_promote_many(ps3_lwmutex_registry *registry,
    ps3_lwmutex_condition_wait *const *tokens, size_t count, size_t *promoted,
    ps3_sync_publication_report *report)
{
    if (!valid_registry(registry) || !promoted || !report || (count && !tokens))
        return result(PS3_LWMUTEX_INVALID_ARGUMENT);
    *promoted=0;
    registry->guard.lock(registry->guard.context);
    ps3_lwmutex_result outcome=result(PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    if (count>registry->waiter_capacity || report->count>PS3_SYNC_PUBLICATION_CAPACITY ||
        count>PS3_SYNC_PUBLICATION_CAPACITY-report->count ||
        count>UINT64_MAX-registry->next_sequence) goto batch_done;
    for (size_t index=0;index<count;++index) {
        ps3_lwmutex_waiter *record=condition_record(registry,tokens[index]);
        if (!record || record->state!=CONDITION_DORMANT) {
            outcome=result(PS3_LWMUTEX_INVALID_ARGUMENT); goto batch_done;
        }
        for (size_t previous=0;previous<index;++previous) {
            if (tokens[previous]==tokens[index]) {
                outcome=result(PS3_LWMUTEX_INVALID_ARGUMENT); goto batch_done;
            }
        }
    }
    uint64_t first_sequence=registry->next_sequence;
    /* Burn the full interval, including an unpublished suffix on failure.
     * Ordinary mutex admission cannot interleave while this guard is held. */
    registry->next_sequence+=count;
    outcome=result(PS3_LWMUTEX_OK);
    for (size_t index=0;index<count;++index) {
        ps3_lwmutex_waiter *record=condition_record(registry,tokens[index]);
        outcome=publish_condition_admission(registry,record,first_sequence+index,report);
        if (outcome.status!=PS3_LWMUTEX_OK) break;
        ++*promoted;
    }
batch_done:
    registry->guard.unlock(registry->guard.context);
    return outcome;
}
