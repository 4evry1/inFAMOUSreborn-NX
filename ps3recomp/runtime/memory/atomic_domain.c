#include "atomic_domain.h"
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

/* The layout rule of atomic_domain.h: the groups of the domain are at least 64 bytes apart, so no two of them share a cache line at any address the object has. A field that
 * is moved into a neighbouring group, or a group that loses its gap, stops the build. */
#define GROUP_APART(gap, next) _Static_assert(offsetof(ps3_atomic_domain, next) >= offsetof(ps3_atomic_domain, gap) + PS3_ATOMIC_GROUP_GAP, #next " shares a cache line with the group before it")
GROUP_APART(gap_cold_, locked_ops);
GROUP_APART(gap_locked_, loss_epoch);
GROUP_APART(gap_locked_, reserved_mask);
GROUP_APART(gap_locked_, debug_line);
GROUP_APART(gap_written_, reserved_map);
GROUP_APART(gap_map_, busy_map);
_Static_assert(offsetof(ps3_atomic_domain, gap_written_) >= offsetof(ps3_atomic_domain, debug_line) + sizeof(uint32_t), "gap_written_ overlaps the written group");
_Static_assert(offsetof(ps3_atomic_domain, gap_locked_) >= offsetof(ps3_atomic_domain, locked_ops) + sizeof(uint32_t), "gap_locked_ overlaps locked_ops");

static int valid_domain(ps3_atomic_domain *d) { return d && d->initialized; }
static int valid_range(uint64_t a, uint64_t n) {
    return a <= UINT32_MAX && n <= PS3_GUEST_ADDRESS_BYTES - a;
}
static int scalar_range(uint64_t a, unsigned width) {
    return valid_range(a, width) && a % width == 0;
}
static int page_range(uint64_t a, uint64_t n) {
    return n && valid_range(a, n) && a % PS3_GUEST_PAGE_BYTES == 0 && n % PS3_GUEST_PAGE_BYTES == 0;
}
/* Root (2026-11-12, Decision 60): the operations that read guest bytes and later act on what they read
 * (load-reserve, store-conditional, compare-exchange, add) announce themselves in locked_ops for their whole
 * duration, BEFORE they take the guard or read anything. That is what a lock-free writer tests
 * (ps3_atomic_domain_write_conflicts). Plain reads, writes, zeroing, commits and pins do not: two plain stores to
 * the same bytes have no order to protect. The cleanup attribute lowers the count on every return path, after
 * leave() has republished reserved_mask. */
/* Root diagnostic (2026-11-12): how often each guarded operation runs, by the source line of its enter. Written
 * inside the guard.
 *
 * Root (2026-11-13, console run rv56s): both diagnostics below are WRITES to shared lines by every guarded operation (debug_line in the domain, the counter array under the
 * guard), which with the old layout invalidated the line every lock-free store reads. They are compiled out unless PS3_ATOMIC_DEBUG_LINE is 1 (lle.mk LLE_ATOMIC_DEBUG_LINE=1;
 * host builds that do not set it keep them, so the host tests and the hang diagnostics of the test programs are unchanged: PS3_ATOMIC_DEBUG_LINE defaults to 0 only when
 * __SWITCH__ is defined). With them off the field debug_line stays 0 and ps3_atomic_domain_debug_enter_count answers 0. */
#ifndef PS3_ATOMIC_DEBUG_LINE
#ifdef __SWITCH__
#define PS3_ATOMIC_DEBUG_LINE 0
#else
#define PS3_ATOMIC_DEBUG_LINE 1
#endif
#endif
#if PS3_ATOMIC_DEBUG_LINE
static uint64_t g_enter_counts[1024];
#define DEBUG_LINE(d, value) ((d)->debug_line = (uint32_t)(value))
#define DEBUG_ENTER(d) ((d)->debug_line = __LINE__, ++g_enter_counts[__LINE__ & 1023])
#else
#define DEBUG_LINE(d, value) ((void)(d))
#define DEBUG_ENTER(d) ((void)(d))
#endif
volatile uint64_t g_ps3_atomic_sticky[1024] PS3_ATOMIC_LINE_ALIGNED;
volatile uint64_t g_ps3_atomic_sticky_lines;
/* Decision 120: see atomic_domain.h. Marked before the operation reads (callers then raise their busy count and fence). */
volatile uint64_t g_ps3_atomic_touched[1024] PS3_ATOMIC_LINE_ALIGNED;
volatile uint64_t g_ps3_atomic_touched_lines;
static void mark_touched_line(uint64_t address)
{
    const uint32_t index = ps3_atomic_sticky_index(address);
    const uint64_t bit = UINT64_C(1) << (index & 63u);
    if (__atomic_load_n(&g_ps3_atomic_touched[index >> 6], __ATOMIC_RELAXED) & bit) return;
    if (!(__atomic_fetch_or(&g_ps3_atomic_touched[index >> 6], bit, __ATOMIC_SEQ_CST) & bit))
        __atomic_fetch_add(&g_ps3_atomic_touched_lines, 1u, __ATOMIC_RELAXED);
}
static void mark_touched(uint64_t a, unsigned width)
{
    mark_touched_line(a);
    const uint64_t end = a + (width ? width - 1u : 0u);
    if (ps3_atomic_sticky_index(end) != ps3_atomic_sticky_index(a)) mark_touched_line(end);
}
/* Decision 112: see atomic_domain.h (line sequence numbers of the sticky lines, the PPU reservation notes). */
#ifndef PS3_ATOMIC_GRANULE_RESERVATIONS
#define PS3_ATOMIC_GRANULE_RESERVATIONS 1
#endif
volatile uint32_t g_ps3_atomic_line_seq[65536] PS3_ATOMIC_LINE_ALIGNED;
volatile uint32_t g_ps3_atomic_global_seq;
int g_ps3_atomic_granule_reservations = PS3_ATOMIC_GRANULE_RESERVATIONS;
volatile uint64_t g_ps3_atomic_reservation_checked, g_ps3_atomic_reservation_lost, g_ps3_atomic_reservation_noted;
typedef struct { uint64_t address; uint32_t seq, used; } reservation_note;
enum { RESERVATION_NOTES = 8 };   /* between a lwarx and its stwcx the libsre loops read a few more words of the same line */
static _Thread_local reservation_note g_notes[RESERVATION_NOTES];
static _Thread_local unsigned g_note_next;
void ps3_atomic_reservation_note(uint64_t address, uint32_t seq)
{
    if (!g_ps3_atomic_granule_reservations) return;
    for (unsigned i = 0; i < RESERVATION_NOTES; ++i)
        if (g_notes[i].used && g_notes[i].address == address) {
            if (seq == PS3_ATOMIC_NO_SEQ) g_notes[i].used = 0; else g_notes[i].seq = seq;
            return;
        }
    if (seq == PS3_ATOMIC_NO_SEQ) return;
    g_notes[g_note_next++ % RESERVATION_NOTES] = (reservation_note){ address, seq, 1u };
    __atomic_fetch_add(&g_ps3_atomic_reservation_noted, 1u, __ATOMIC_RELAXED);
}
int ps3_atomic_reservation_intact(uint64_t address)
{
    if (!g_ps3_atomic_granule_reservations) return 1;
    for (unsigned i = 0; i < RESERVATION_NOTES; ++i)
        if (g_notes[i].used && g_notes[i].address == address) {
            const uint32_t before = g_notes[i].seq;
            g_notes[i].used = 0;
            __atomic_fetch_add(&g_ps3_atomic_reservation_checked, 1u, __ATOMIC_RELAXED);
            const uint32_t now = ps3_atomic_reservation_snapshot(address);
            if (now != PS3_ATOMIC_NO_SEQ && now == before) return 1;
            __atomic_fetch_add(&g_ps3_atomic_reservation_lost, 1u, __ATOMIC_RELAXED);
            return 0;
        }
    return 1;
}
/* A write to [a, a + n) made under the guard: raise the sequence of every sticky line it touches (a range of more than 64 lines raises the global one). */
static void bump_line_sequences(uint64_t a, uint64_t n)
{
    if (!g_ps3_atomic_granule_reservations) return;
    const uint64_t first = a / PS3_RESERVATION_GRANULE_BYTES, last = (a + n - 1u) / PS3_RESERVATION_GRANULE_BYTES;
    if (last - first >= 64u) { __atomic_fetch_add(&g_ps3_atomic_global_seq, 1u, __ATOMIC_SEQ_CST); return; }
    for (uint64_t line = first; line <= last; ++line) {
        const uint32_t index = ps3_atomic_sticky_index(line * PS3_RESERVATION_GRANULE_BYTES);
        if (!((__atomic_load_n(&g_ps3_atomic_sticky[index >> 6], __ATOMIC_RELAXED) >> (index & 63u)) & 1u)) continue;
        const uint32_t before = __atomic_fetch_add(&g_ps3_atomic_line_seq[index], 1u, __ATOMIC_SEQ_CST);
        /* A write by the host thread that holds the note (its own store to the line, the reservation of the CPU that stores is not
         * cancelled by it) moves that thread's notes of this line along, unless somebody else wrote since the note was taken. */
        const uint32_t expected = before + __atomic_load_n(&g_ps3_atomic_global_seq, __ATOMIC_ACQUIRE);
        for (unsigned i = 0; i < RESERVATION_NOTES; ++i)
            if (g_notes[i].used && g_notes[i].seq == expected && ps3_atomic_sticky_index(g_notes[i].address) == index) g_notes[i].seq = expected + 1u;
    }
}
uint64_t ps3_atomic_domain_debug_enter_count(unsigned line)
{
#if PS3_ATOMIC_DEBUG_LINE
    return line < 1024u ? g_enter_counts[line] : 0u;
#else
    (void)line;
    return 0u;
#endif
}
typedef struct { ps3_atomic_domain *d; uint32_t first, last; } rmw_token;
static rmw_token rmw_begin(ps3_atomic_domain *d, uint64_t a, unsigned width) {
    rmw_token token = {d, ps3_atomic_granule_index(a), ps3_atomic_granule_index(a + (width ? width - 1u : 0u))};
    if (d) {
        mark_touched(a, width);   /* Decision 120: before the busy count and its fence, so a store that missed the bit is caught by its own tests */
        __atomic_fetch_add(&d->busy_map[token.first], 1u, __ATOMIC_SEQ_CST);
        if (token.last != token.first) __atomic_fetch_add(&d->busy_map[token.last], 1u, __ATOMIC_SEQ_CST);
        atomic_thread_fence(memory_order_seq_cst);
    }
    return token;
}
static void rmw_end(rmw_token *token) {
    if (!token->d) return;
    if (token->last != token->first) __atomic_fetch_sub(&token->d->busy_map[token->last], 1u, __ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&token->d->busy_map[token->first], 1u, __ATOMIC_SEQ_CST);
}
#define RMW_SCOPE_AT(d, a, width) rmw_token rmw_scope_ __attribute__((cleanup(rmw_end), unused)) = rmw_begin((d), (a), (width))
/* Root (2026-11-12): PS3_ATOMIC_NARROW_CONFLICTS (off) limits locked_ops to the read-modify-write operations
 * (RMW_SCOPE). Build r41 ran with that rule and stalled at start twice at the same point, where r40 (every
 * guarded operation counts) never did; the hole is not found yet, so the wide rule is the default again. */
/* Decision 136 (console run rv71s): the guard is held for about a microsecond, but the job threads waited ~40 us per GETLLAR/PUTLLC for it (the sound
 * SPU, alone on its core, 4.7 us). The console's guard (barging_lock_horizon.h) spins ~1-2 us, then sleeps on the word; an unlock wakes one sleeper,
 * which then has to wait for its core: the five job threads share three cores with the main and RSX threads, and Horizon does not time-slice
 * threads of equal priority, so a woken waiter runs only when the thread on its core blocks. Here a thread first waits while the guard looks held
 * (guard_inside, set by the holder), up to PS3_ATOMIC_GUARD_SPINS polls with a processor yield hint and a scheduler yield every 256 polls (so a holder
 * waiting for this very core gets it), and only then calls the guard's lock, which normally succeeds at once. Exclusion is still the guard's alone;
 * guard_inside only decides how long to poll before asking. -DPS3_ATOMIC_GUARD_SPINS=0: the old behaviour. */
#ifndef PS3_ATOMIC_GUARD_SPINS
#define PS3_ATOMIC_GUARD_SPINS 4096
#endif
int sched_yield(void);
uint64_t (*ps3_atomic_guard_clock)(void) = 0;
static _Thread_local unsigned g_writer_tag;
static _Thread_local uint64_t g_guard_counts[4];   /* acquisitions, wait ns, longest wait ns, spun out (folded by ps3_atomic_dma_take_counts) */
static inline void cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#endif
}
static void guard_lock(ps3_atomic_domain *d) {
    uint64_t (*const clock)(void) = g_writer_tag ? ps3_atomic_guard_clock : 0;
    const uint64_t t0 = clock ? clock() : 0;
    unsigned spins = 0;
    while (spins < PS3_ATOMIC_GUARD_SPINS && __atomic_load_n(&d->guard_inside, __ATOMIC_RELAXED)) {
        cpu_relax();
        if ((++spins & 255u) == 0u) (void)sched_yield();
    }
    d->guard.lock(d->guard.context);
    __atomic_store_n(&d->guard_inside, 1u, __ATOMIC_RELAXED);
    if (clock) {
        const uint64_t waited = clock() - t0;
        ++g_guard_counts[0]; g_guard_counts[1] += waited;
        if (waited > g_guard_counts[2]) g_guard_counts[2] = waited;
        if (spins >= PS3_ATOMIC_GUARD_SPINS && PS3_ATOMIC_GUARD_SPINS) ++g_guard_counts[3];
    }
}
static void guard_unlock(ps3_atomic_domain *d) {
    __atomic_store_n(&d->guard_inside, 0u, __ATOMIC_RELEASE);
    d->guard.unlock(d->guard.context);
}
static void enter(ps3_atomic_domain *d) {
#ifndef PS3_ATOMIC_NARROW_CONFLICTS
    __atomic_fetch_add(&d->locked_ops, 1u, __ATOMIC_SEQ_CST);
#endif
    guard_lock(d);
    atomic_thread_fence(memory_order_seq_cst);
}
static ps3_guest_memory_status leave_counted(ps3_atomic_domain *d, ps3_guest_memory_status status, int counted) {
    /* Every change of a reservation happens inside the guard, so recomputing here keeps reserved_mask exact; it is
     * published before locked_ops drops, so the two never both read zero while a reservation is live. */
    uint64_t mask = 0;
    uint64_t map[16] = {0};
    for (size_t i = 0; i < d->owner_count; ++i)
        if (d->owners[i].active && d->owners[i].reserved) {
            /* a reservation is one scalar or one 128-byte line: it lies in one granule or straddles two */
            const uint64_t end = (uint64_t)d->owners[i].address + (d->owners[i].width ? d->owners[i].width - 1u : 0u);
            const uint32_t first = ps3_atomic_granule_index(d->owners[i].address), last = ps3_atomic_granule_index(end);
            mask |= ps3_atomic_granule_bit(d->owners[i].address);
            mask |= ps3_atomic_granule_bit(end);
            map[first >> 6] |= UINT64_C(1) << (first & 63u);
            map[last >> 6] |= UINT64_C(1) << (last & 63u);
        }
    /* Bits are set first and cleared last, word by word: a writer may see a bit too many, never one too few. */
    for (size_t i = 0; i < 16; ++i) {
        const uint64_t old = __atomic_load_n(&d->reserved_map[i], __ATOMIC_RELAXED);
        if ((old | map[i]) != old) __atomic_store_n(&d->reserved_map[i], old | map[i], __ATOMIC_SEQ_CST);
    }
    for (size_t i = 0; i < 16; ++i)
        if (__atomic_load_n(&d->reserved_map[i], __ATOMIC_RELAXED) != map[i]) __atomic_store_n(&d->reserved_map[i], map[i], __ATOMIC_SEQ_CST);
    /* Root (2026-11-13): written only when it changed. This store ran in every guarded operation, in a line that the lock-free stores read; nothing reads reserved_mask on
     * the store path (the maps do), and a store of the value it already holds publishes nothing. The fence below is unchanged. */
    if (__atomic_load_n(&d->reserved_mask, __ATOMIC_RELAXED) != mask) __atomic_store_n(&d->reserved_mask, mask, __ATOMIC_SEQ_CST);
    atomic_thread_fence(memory_order_seq_cst);
    guard_unlock(d);
#ifndef PS3_ATOMIC_NARROW_CONFLICTS
    if (counted) __atomic_fetch_sub(&d->locked_ops, 1u, __ATOMIC_SEQ_CST);
#else
    (void)counted;
#endif
    return status;
}
static ps3_guest_memory_status leave(ps3_atomic_domain *d, ps3_guest_memory_status status) { return leave_counted(d, status, 1); }
/* Also plain since build r75: ps3_atomic_zero, ps3_atomic_owner_read, ps3_atomic_owner_write and
 * ps3_atomic_owner_take_reservation_loss (the SPU interpreters polled that one 15 000 times a second; it only
 * reads and clears a flag of its own owner record). */
/* Root (2026-11-12, Decision 74): the plain read and the plain write do not count in locked_ops (unless
 * PS3_ATOMIC_PLAIN_OPS_COUNT is defined). A lock-free writer that finds locked_ops raised repeats its store through
 * ps3_atomic_write; when that repeat raised locked_ops itself, every other thread's stores during it were pushed to
 * the same path, and theirs pushed the next ones: about 150 000 guarded writes a second in the game's idle scene,
 * with all its threads on their own cores. A plain store has no order to protect against another plain store, and
 * it still runs inside the guard, so it stays ordered against every operation that does count (reservations,
 * conditional stores, waits, commits). */
static void enter_plain(ps3_atomic_domain *d) {
#if !defined(PS3_ATOMIC_NARROW_CONFLICTS) && defined(PS3_ATOMIC_PLAIN_OPS_COUNT)
    __atomic_fetch_add(&d->locked_ops, 1u, __ATOMIC_SEQ_CST);
#endif
    guard_lock(d);
    atomic_thread_fence(memory_order_seq_cst);
}
static ps3_guest_memory_status leave_plain(ps3_atomic_domain *d, ps3_guest_memory_status status) {
    /* Root (r79): never touches locked_ops. Builds r73..r78 raised it here for the length of leave() and lowered it
     * again; that short raise, 135 000 times a second, was itself 96 % of the remaining store repeats. */
#if !defined(PS3_ATOMIC_NARROW_CONFLICTS) && defined(PS3_ATOMIC_PLAIN_OPS_COUNT)
    return leave_counted(d, status, 1);
#else
    return leave_counted(d, status, 0);
#endif
}
static ps3_atomic_owner_record *find_owner(ps3_atomic_domain *d, ps3_atomic_owner owner) {
    if (owner.domain != d || owner.slot >= d->owner_count) return NULL;
    ps3_atomic_owner_record *record = &d->owners[owner.slot];
    return record->active && record->generation == owner.generation ? record : NULL;
}
/* Called with the guard held. The port contract forbids reentering the domain
 * or running consumer code (lock order: domain guard -> port exclusion). */
static void wake_waiters(ps3_atomic_domain *d) {
    if (!d->wait_port.wake_all) return;
    __atomic_fetch_add(&d->loss_epoch, 1, __ATOMIC_SEQ_CST);
    DEBUG_LINE(d, 300000 + __LINE__);   /* diagnostic: about to wake the waiters */
    d->wait_port.wake_all(d->wait_port.context);
    DEBUG_LINE(d, 400000 + __LINE__);   /* diagnostic: waiters woken */
}
void (*ps3_atomic_reservation_loss_hook)(void) = 0;   /* Decision 116, see atomic_domain.h */
void (*ps3_atomic_guarded_write_hook)(uint64_t, uint64_t) = 0;      /* Decision 121/123, see atomic_domain.h */
void ps3_atomic_set_writer_tag(unsigned tag) { g_writer_tag = tag; }
unsigned ps3_atomic_writer_tag(void) { return g_writer_tag; }
static void invalidate(ps3_atomic_domain *d, uint64_t a, uint64_t n) {
    if (!n) return;
    bump_line_sequences(a, n);
    int latched = 0;
    for (size_t i = 0; i < d->owner_count; ++i) {
        ps3_atomic_owner_record *r = &d->owners[i];
        uint64_t line = (uint64_t)r->address & ~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES - 1);
        if (r->active && r->reserved && a < line + PS3_RESERVATION_GRANULE_BYTES && line < a + n) {
            r->reserved = 0;
            r->reservation_lost = 1;
            latched = 1;
        }
    }
    if (latched) {
        wake_waiters(d);
        void (*const hook)(void) = ps3_atomic_reservation_loss_hook;
        if (hook) hook();
    }
    void (*const write_hook)(uint64_t, uint64_t) = ps3_atomic_guarded_write_hook;
    if (write_hook) write_hook(a, n);
}
static uint64_t decode(const unsigned char *bytes, unsigned n) {
    uint64_t value = 0;
    for (unsigned i = 0; i < n; ++i) value = (value << 8) | bytes[i];
    return value;
}
static void encode(unsigned char *bytes, unsigned n, uint64_t value) {
    while (n) {
        --n;
        bytes[n] = (unsigned char)value;
        value >>= 8;
    }
}
ps3_guest_memory_status ps3_atomic_domain_init(ps3_atomic_domain *d, ps3_guest_memory memory,
                                              ps3_instance_guard guard,
                                              ps3_atomic_owner_record *owners, size_t owner_count,
                                              ps3_atomic_pin_record *pins, size_t pin_count) {
    if (!d || !memory.commit || !memory.decommit || !memory.read || !memory.write || !memory.zero ||
        !guard.lock || !guard.unlock || (owner_count && !owners) || (pin_count && !pins) ||
        owner_count > SIZE_MAX / sizeof *owners || pin_count > SIZE_MAX / sizeof *pins || memory.context == d)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (d->initialized) return PS3_GUEST_MEMORY_BUSY;
    if (owner_count) memset(owners, 0, owner_count * sizeof *owners);
    if (pin_count) memset(pins, 0, pin_count * sizeof *pins);
    d->backing = memory;
    d->guard = guard;
    d->owners = owners;
    d->owner_count = owner_count;
    d->pins = pins;
    d->pin_count = pin_count;
    d->wait_port = (ps3_notify_port){0};
    d->direct = NULL; d->direct_context = NULL;
    d->loss_epoch = 0;
    d->initialized = 1;
    return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory_status ps3_atomic_owner_acquire(ps3_atomic_domain *d, ps3_atomic_owner *owner) {
    if (!valid_domain(d) || !owner) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    for (size_t i = 0; i < d->owner_count; ++i) {
        ps3_atomic_owner_record *r = &d->owners[i];
        if (!r->active && r->generation != UINT64_MAX) {
            ++r->generation;
            r->active = 1;
            r->reserved = 0;
            r->reservation_lost = 0;
            *owner = (ps3_atomic_owner){d, i, r->generation};
            return leave(d, PS3_GUEST_MEMORY_OK);
        }
    }
    return leave(d, PS3_GUEST_MEMORY_NO_MEMORY);
}
ps3_guest_memory_status ps3_atomic_owner_release(ps3_atomic_domain *d, ps3_atomic_owner *owner) {
    if (!valid_domain(d) || !owner) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, *owner);
    if (!r) return leave(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    r->reserved = 0;
    r->reservation_lost = 0;
    r->active = 0;
    *owner = (ps3_atomic_owner){0};
    wake_waiters(d); /* a waiter watching this handle must observe retirement */
    return leave(d, PS3_GUEST_MEMORY_OK);
}
ps3_guest_memory_status ps3_atomic_owner_clear_reservation(ps3_atomic_domain *d, ps3_atomic_owner owner) {
    if (!valid_domain(d)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *record = find_owner(d, owner);
    if (!record) return leave(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    record->reserved = 0;
    return leave(d, PS3_GUEST_MEMORY_OK);
}
ps3_guest_memory_status ps3_atomic_owner_take_reservation_loss(ps3_atomic_domain *d, ps3_atomic_owner owner, int *lost) {
    if (!valid_domain(d) || !lost) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *record = find_owner(d, owner);
    if (!record) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    *lost = record->reservation_lost != 0;
    record->reservation_lost = 0;
    return leave_plain(d, PS3_GUEST_MEMORY_OK);
}
ps3_guest_memory_status ps3_atomic_domain_attach_wait_port(ps3_atomic_domain *d, ps3_notify_port port) {
    if (!valid_domain(d) || !port.wait || !port.wake_all) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    if (d->wait_port.wait) return leave(d, PS3_GUEST_MEMORY_BUSY);
    d->wait_port = port;
    return leave(d, PS3_GUEST_MEMORY_OK);
}
int ps3_notify_cancel_is_requested(const ps3_notify_cancel *cancel) {
    return cancel && __atomic_load_n(&cancel->requested, __ATOMIC_SEQ_CST) != 0;
}
void ps3_notify_cancel_request(ps3_notify_cancel *cancel, const ps3_notify_port *port) {
    if (!cancel) return;
    __atomic_store_n(&cancel->requested, 1u, __ATOMIC_SEQ_CST);
    if (port && port->wake_all) port->wake_all(port->context);
}
/* Under the guard: INVALID unless every handle is live and latched or reserved. */
static int scan_owners(ps3_atomic_domain *d, const ps3_atomic_owner *owners, size_t count, uint64_t *pending) {
    *pending = 0;
    for (size_t i = 0; i < count; ++i) {
        ps3_atomic_owner_record *r = find_owner(d, owners[i]);
        if (!r || (!r->reservation_lost && !r->reserved)) return 0;
        if (r->reservation_lost) *pending |= UINT64_C(1) << i;
    }
    return 1;
}
ps3_atomic_wait_result ps3_atomic_owners_wait_reservation_loss(ps3_atomic_domain *d, const ps3_atomic_owner *owners,
                                                               size_t count, uint64_t deadline_ns,
                                                               ps3_notify_cancel *cancel, uint64_t *mask) {
    if (!valid_domain(d) || !owners || !mask || !count || count > 64) return PS3_ATOMIC_WAIT_INVALID;
    for (;;) {
        uint64_t pending = 0;
        enter(d); DEBUG_ENTER(d);
        ps3_notify_port port = d->wait_port;
        int valid = scan_owners(d, owners, count, &pending);
        uint64_t epoch = __atomic_load_n(&d->loss_epoch, __ATOMIC_SEQ_CST);
        leave(d, PS3_GUEST_MEMORY_OK);
        if (!valid || !port.wait) return PS3_ATOMIC_WAIT_INVALID;
        if (ps3_notify_cancel_is_requested(cancel)) { *mask = 0; return PS3_ATOMIC_WAIT_CANCELLED; }
        if (pending) { *mask = pending; return PS3_ATOMIC_WAIT_LOST; }
        ps3_notify_wait_result waited =
            port.wait(port.context, &d->loss_epoch, epoch, cancel ? &cancel->requested : NULL, deadline_ns);
        if (waited == PS3_NOTIFY_FAILURE) return PS3_ATOMIC_WAIT_INVALID;
        if (waited == PS3_NOTIFY_TIMEOUT) {
            enter(d); DEBUG_ENTER(d);
            valid = scan_owners(d, owners, count, &pending);
            leave(d, PS3_GUEST_MEMORY_OK);
            if (!valid) return PS3_ATOMIC_WAIT_INVALID;
            if (ps3_notify_cancel_is_requested(cancel)) { *mask = 0; return PS3_ATOMIC_WAIT_CANCELLED; }
            if (pending) { *mask = pending; return PS3_ATOMIC_WAIT_LOST; }
            *mask = 0;
            return PS3_ATOMIC_WAIT_TIMEOUT;
        }
    }
}
ps3_guest_memory_status ps3_atomic_read(ps3_atomic_domain *d, uint64_t a, void *buffer, size_t n) {
    if (!valid_domain(d) || !valid_range(a, n) || (n && !buffer)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_guest_memory_status status = n ? d->backing.read(d->backing.context, (uint32_t)a, buffer, n) : PS3_GUEST_MEMORY_OK;
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_write(ps3_atomic_domain *d, uint64_t a, const void *buffer, size_t n) {
    if (!valid_domain(d) || !valid_range(a, n) || (n && !buffer)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_guest_memory_status status = n ? d->backing.write(d->backing.context, (uint32_t)a, buffer, n) : PS3_GUEST_MEMORY_OK;
    DEBUG_LINE(d, 100000 + __LINE__);   /* diagnostic: the backing write returned */
    if (status == PS3_GUEST_MEMORY_OK) invalidate(d, a, n);
    DEBUG_LINE(d, 200000 + __LINE__);   /* diagnostic: invalidation done */
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_owner_read(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                             uint64_t a, void *buffer, size_t n) {
    if (!valid_domain(d) || !valid_range(a, n) || (n && !buffer)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    if (!find_owner(d, owner)) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    ps3_guest_memory_status status = n ? d->backing.read(d->backing.context, (uint32_t)a, buffer, n) : PS3_GUEST_MEMORY_OK;
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_owner_write(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                              uint64_t a, const void *buffer, size_t n) {
    if (!valid_domain(d) || !valid_range(a, n) || (n && !buffer)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    if (!find_owner(d, owner)) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    ps3_guest_memory_status status = n ? d->backing.write(d->backing.context, (uint32_t)a, buffer, n) : PS3_GUEST_MEMORY_OK;
    if (status == PS3_GUEST_MEMORY_OK) invalidate(d, a, n);
    return leave_plain(d, status);
}
/* ---- Decision 135: plain SPU DMA without the guard (see atomic_domain.h, ps3_atomic_dma_read) ---- */
#ifndef PS3_ATOMIC_LOCKFREE_DMA
#define PS3_ATOMIC_LOCKFREE_DMA 1
#endif
static _Thread_local uint64_t g_dma_counts[PS3_ATOMIC_GUARD_ACQUIRES];
void ps3_atomic_dma_take_counts(uint64_t out[PS3_ATOMIC_DMA_COUNTS]) {
    for (unsigned i = 0; i < PS3_ATOMIC_GUARD_ACQUIRES; ++i) { out[i] = g_dma_counts[i]; g_dma_counts[i] = 0; }
    for (unsigned i = 0; i < 4u; ++i) { out[PS3_ATOMIC_GUARD_ACQUIRES + i] = g_guard_counts[i]; g_guard_counts[i] = 0; }
}
void ps3_atomic_domain_set_direct(ps3_atomic_domain *d, const void *(*fn)(void *, uint32_t, uint64_t), void *context) {
    if (!d) return;
    d->direct_context = context;
    __atomic_store_n(&d->direct, fn, __ATOMIC_RELEASE);
}
#if !PS3_ATOMIC_LOCKFREE_DMA
#define LINE_STRIPE_LOCK(a) ((void)(a))
#define LINE_STRIPE_UNLOCK(a) ((void)(a))
#endif
#if PS3_ATOMIC_LOCKFREE_DMA
/* Line atomicity of SPU DMA (the MFC moves whole 128-byte lines): every SPU DMA copy, direct or guarded, copies one line at a time holding that
 * line's stripe, so two DMAs of the same line never interleave (they did not under the guard either). A stripe is held only for the copy of at
 * most 128 bytes. Lock order: the domain guard, then a stripe; a direct copy takes no guard. */
enum { DMA_STRIPES = 1024, DMA_STRIPE_SPINS = 2048 };
typedef struct { uint32_t word; unsigned char pad[60]; } dma_stripe;
static dma_stripe g_dma_stripes[DMA_STRIPES] __attribute__((aligned(64)));
int sched_yield(void);
static dma_stripe *dma_stripe_of(uint64_t line_address) {
    const uint64_t line = line_address / PS3_RESERVATION_GRANULE_BYTES;
    return &g_dma_stripes[(line ^ (line >> 10)) & (DMA_STRIPES - 1u)];
}
static void dma_stripe_lock(dma_stripe *s) {
    for (unsigned spins = 0;; ++spins) {
        uint32_t expected = 0;
        if (!__atomic_load_n(&s->word, __ATOMIC_RELAXED) &&
            __atomic_compare_exchange_n(&s->word, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        if (spins >= DMA_STRIPE_SPINS) { (void)sched_yield(); spins = 0; }
    }
}
static void dma_stripe_unlock(dma_stripe *s) { __atomic_store_n(&s->word, 0u, __ATOMIC_RELEASE); }
/* The 128-byte line operations (GETLLAR, PUTLLC, PUTLLUC) hold the line's stripe around their backing access too (inside the guard, as the
 * guarded DMA copies do): a direct DMA copy that began before the line's first read-modify-write never interleaves with them. */
#define LINE_STRIPE_LOCK(a) dma_stripe_lock(dma_stripe_of(a))
#define LINE_STRIPE_UNLOCK(a) dma_stripe_unlock(dma_stripe_of(a))
static int dma_line_touched(uint64_t address) {
    const uint32_t index = ps3_atomic_sticky_index(address);
    return (int)((__atomic_load_n(&g_ps3_atomic_touched[index >> 6], __ATOMIC_RELAXED) >> (index & 63u)) & 1u);
}
static int dma_run_touched(uint64_t a, uint64_t n) {
    for (uint64_t line = a & ~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES - 1u); line < a + n; line += PS3_RESERVATION_GRANULE_BYTES)
        if (dma_line_touched(line)) return 1;
    return 0;
}
/* The end of the run that starts at `start`: the following lines while their touched bit is `touched` (runs end on line boundaries). */
static uint64_t dma_run_end(uint64_t start, uint64_t end, int touched) {
    uint64_t next = (start & ~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES - 1u)) + PS3_RESERVATION_GRANULE_BYTES;
    while (next < end && dma_line_touched(next) == touched) next += PS3_RESERVATION_GRANULE_BYTES;
    return next < end ? next : end;
}
/* The owner handle is live (its own SPU thread holds it; owners change only under the guard). */
static int dma_owner_live(ps3_atomic_domain *d, ps3_atomic_owner owner) {
    if (owner.domain != d || owner.slot >= d->owner_count) return 0;
    const ps3_atomic_owner_record *r = &d->owners[owner.slot];
    return __atomic_load_n(&r->active, __ATOMIC_ACQUIRE) && __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE) == owner.generation;
}
#if defined(__SANITIZE_THREAD__)
#define PS3_DMA_RELAXED_COPY 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PS3_DMA_RELAXED_COPY 1
#endif
#endif
/* Guest bytes other threads may store to at the same time. Under ThreadSanitizer every byte is a relaxed atomic (the host tests' backing does the same),
 * so the sanitizer checks the protocol and not the plain-data races it permits by design (the same ones the PPU's lock-free accesses have). */
static void dma_copy(void *to, const void *from, uint64_t n) {
#ifdef PS3_DMA_RELAXED_COPY
    for (uint64_t i = 0; i < n; ++i) __atomic_store_n((unsigned char *)to + i, __atomic_load_n((const unsigned char *)from + i, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
#else
    memcpy(to, from, (size_t)n);
#endif
}
/* Direct copy of guest [a, a + n) (host `guest` = its address) to or from `buffer`, one line at a time under the line's stripe. */
static void dma_copy_lines(unsigned char *guest, unsigned char *buffer, uint64_t a, uint64_t n, int to_guest) {
    for (uint64_t at = a; at < a + n;) {
        const uint64_t line_end = (at & ~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES - 1u)) + PS3_RESERVATION_GRANULE_BYTES;
        const uint64_t end = line_end < a + n ? line_end : a + n;
        dma_stripe *s = dma_stripe_of(at);
        dma_stripe_lock(s);
        if (to_guest) dma_copy(guest + (at - a), buffer + (at - a), end - at); else dma_copy(buffer + (at - a), guest + (at - a), end - at);
        dma_stripe_unlock(s);
        at = end;
    }
}
/* The guarded copy of a run (ps3_atomic_owner_read / _write, with the line stripes): same owner check, backing call per line, and for a write
 * the same invalidate() (reservation loss, line sequences, loss hook, guarded-write hook) once for the run. */
static ps3_guest_memory_status dma_guarded(ps3_atomic_domain *d, ps3_atomic_owner owner, uint64_t a, unsigned char *buffer, uint64_t n, int write) {
    enter_plain(d); DEBUG_ENTER(d);
    if (!find_owner(d, owner)) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    ps3_guest_memory_status status = PS3_GUEST_MEMORY_OK;
    for (uint64_t at = a; at < a + n && status == PS3_GUEST_MEMORY_OK;) {
        const uint64_t line_end = (at & ~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES - 1u)) + PS3_RESERVATION_GRANULE_BYTES;
        const uint64_t end = line_end < a + n ? line_end : a + n;
        dma_stripe *s = dma_stripe_of(at);
        dma_stripe_lock(s);
        status = write ? d->backing.write(d->backing.context, (uint32_t)at, buffer + (at - a), (size_t)(end - at))
                       : d->backing.read(d->backing.context, (uint32_t)at, buffer + (at - a), (size_t)(end - at));
        dma_stripe_unlock(s);
        at = end;
    }
    if (write && status == PS3_GUEST_MEMORY_OK) invalidate(d, a, n);
    return leave_plain(d, status);
}
#endif
ps3_guest_memory_status ps3_atomic_dma_read(ps3_atomic_domain *d, ps3_atomic_owner owner, uint64_t a, void *buffer, size_t n) {
#if PS3_ATOMIC_LOCKFREE_DMA
    if (valid_domain(d) && valid_range(a, n) && n && buffer) {
        const void *(*const direct)(void *, uint32_t, uint64_t) = __atomic_load_n(&d->direct, __ATOMIC_ACQUIRE);
        const unsigned char *guest = direct && dma_owner_live(d, owner) ? direct(d->direct_context, (uint32_t)a, n) : NULL;
        if (!guest) { if (direct) ++g_dma_counts[PS3_ATOMIC_DMA_NO_DIRECT]; return ps3_atomic_owner_read(d, owner, a, buffer, n); }
        unsigned char *out = buffer;
        for (uint64_t start = a; start < a + n;) {
            const int touched = dma_line_touched(start);
            const uint64_t end = dma_run_end(start, a + n, touched), len = end - start;
            int guarded = touched;
            if (!guarded) {
                dma_copy_lines((unsigned char *)guest + (start - a), out + (start - a), start, len, 0);
                atomic_thread_fence(memory_order_seq_cst);
                if (dma_run_touched(start, len)) { guarded = 1; ++g_dma_counts[PS3_ATOMIC_DMA_REDO_GET]; }
                else g_dma_counts[PS3_ATOMIC_DMA_DIRECT_GET] += len;
            }
            if (guarded) {
                const ps3_guest_memory_status status = dma_guarded(d, owner, start, out + (start - a), len, 0);
                if (status != PS3_GUEST_MEMORY_OK) return status;
                g_dma_counts[PS3_ATOMIC_DMA_GUARDED_GET] += len;
            }
            start = end;
        }
        return PS3_GUEST_MEMORY_OK;
    }
#endif
    return ps3_atomic_owner_read(d, owner, a, buffer, n);
}
ps3_guest_memory_status ps3_atomic_dma_write(ps3_atomic_domain *d, ps3_atomic_owner owner, uint64_t a, const void *buffer, size_t n) {
#if PS3_ATOMIC_LOCKFREE_DMA
    if (valid_domain(d) && valid_range(a, n) && n && buffer) {
        const void *(*const direct)(void *, uint32_t, uint64_t) = __atomic_load_n(&d->direct, __ATOMIC_ACQUIRE);
        unsigned char *guest = direct && dma_owner_live(d, owner) ? (unsigned char *)direct(d->direct_context, (uint32_t)a, n) : NULL;
        if (!guest) { if (direct) ++g_dma_counts[PS3_ATOMIC_DMA_NO_DIRECT]; return ps3_atomic_owner_write(d, owner, a, buffer, n); }
        const unsigned char *in = buffer;
        for (uint64_t start = a; start < a + n;) {
            const int touched = dma_line_touched(start);
            const uint64_t end = dma_run_end(start, a + n, touched), len = end - start;
            int guarded = touched;
            if (!guarded) {
                dma_copy_lines(guest + (start - a), (unsigned char *)in + (start - a), start, len, 1);
                atomic_thread_fence(memory_order_seq_cst);
                if (dma_run_touched(start, len)) { guarded = 1; ++g_dma_counts[PS3_ATOMIC_DMA_REDO_PUT]; }
                else {
                    g_dma_counts[PS3_ATOMIC_DMA_DIRECT_PUT] += len;
                    void (*const write_hook)(uint64_t, uint64_t) = ps3_atomic_guarded_write_hook;   /* as invalidate() calls it for a guarded write */
                    if (write_hook) write_hook(start, len);
                }
            }
            if (guarded) {
                const ps3_guest_memory_status status = dma_guarded(d, owner, start, (unsigned char *)in + (start - a), len, 1);
                if (status != PS3_GUEST_MEMORY_OK) return status;
                g_dma_counts[PS3_ATOMIC_DMA_GUARDED_PUT] += len;
            }
            start = end;
        }
        return PS3_GUEST_MEMORY_OK;
    }
#endif
    return ps3_atomic_owner_write(d, owner, a, buffer, n);
}
ps3_guest_memory_status ps3_atomic_zero(ps3_atomic_domain *d, uint64_t a, uint64_t n) {
    if (!valid_domain(d) || !valid_range(a, n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_guest_memory_status status = n ? d->backing.zero(d->backing.context, (uint32_t)a, n) : PS3_GUEST_MEMORY_OK;
    if (status == PS3_GUEST_MEMORY_OK) invalidate(d, a, n);
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_commit(ps3_atomic_domain *d, uint64_t a, uint64_t n) {
    if (!valid_domain(d) || !page_range(a, n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    return leave(d, d->backing.commit(d->backing.context, (uint32_t)a, n));
}
ps3_guest_memory_status ps3_atomic_decommit(ps3_atomic_domain *d, uint64_t a, uint64_t n) {
    if (!valid_domain(d) || !page_range(a, n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    for (size_t i = 0; i < d->pin_count; ++i) {
        ps3_atomic_pin_record *p = &d->pins[i];
        if (p->active && a < (uint64_t)p->address + p->size && p->address < a + n)
            return leave(d, PS3_GUEST_MEMORY_BUSY);
    }
    ps3_guest_memory_status status = d->backing.decommit(d->backing.context, (uint32_t)a, n);
    if (status == PS3_GUEST_MEMORY_OK) invalidate(d, a, n);
    return leave(d, status);
}
ps3_guest_memory_status ps3_atomic_pin_acquire(ps3_atomic_domain *d, uint64_t a, uint64_t n, ps3_atomic_pin *pin) {
    if (!valid_domain(d) || !pin || !n || !valid_range(a, n)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter(d); DEBUG_ENTER(d);
    size_t slot = d->pin_count;
    for (size_t i = 0; i < d->pin_count; ++i) {
        if (!d->pins[i].active && d->pins[i].generation != UINT64_MAX) {
            slot = i;
            break;
        }
    }
    if (slot == d->pin_count) return leave(d, PS3_GUEST_MEMORY_NO_MEMORY);
    uint64_t address = a;
    while (address < a + n) {
        unsigned char byte;
        ps3_guest_memory_status status = d->backing.read(d->backing.context, (uint32_t)address, &byte, 1);
        if (status != PS3_GUEST_MEMORY_OK) return leave(d, status);
        address = (address & ~(PS3_GUEST_PAGE_BYTES - 1)) + PS3_GUEST_PAGE_BYTES;
    }
    ps3_atomic_pin_record *p = &d->pins[slot];
    ++p->generation;
    p->address = (uint32_t)a;
    p->size = n;
    p->active = 1;
    *pin = (ps3_atomic_pin){d, slot, p->generation};
    return leave(d, PS3_GUEST_MEMORY_OK);
}
ps3_guest_memory_status ps3_atomic_pin_release(ps3_atomic_domain *d, ps3_atomic_pin *pin) {
    if (!valid_domain(d) || !pin) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    if (pin->domain != d || pin->slot >= d->pin_count) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    ps3_atomic_pin_record *p = &d->pins[pin->slot];
    if (!p->active || p->generation != pin->generation) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    p->active = 0;
    *pin = (ps3_atomic_pin){0};
    return leave_plain(d, PS3_GUEST_MEMORY_OK);
}
static ps3_guest_memory_status load_reserve(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                           uint64_t a, unsigned width, uint64_t *value) {
    RMW_SCOPE_AT(d, a, width);
    if (!valid_domain(d) || !value || !scalar_range(a, width)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, owner);
    if (!r) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    unsigned char bytes[8];
    ps3_guest_memory_status status = d->backing.read(d->backing.context, (uint32_t)a, bytes, width);
    if (status == PS3_GUEST_MEMORY_OK) {
        r->address = (uint32_t)a;
        r->width = width;
        r->reserved = 1;
        *value = decode(bytes, width);
    }
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_load_reserve32(ps3_atomic_domain *d, ps3_atomic_owner owner, uint64_t a, uint32_t *value) {
    if (!value) return PS3_GUEST_MEMORY_INVALID_RANGE;
    uint64_t wide;
    ps3_guest_memory_status status = load_reserve(d, owner, a, 4, &wide);
    if (status == PS3_GUEST_MEMORY_OK) *value = (uint32_t)wide;
    return status;
}
ps3_guest_memory_status ps3_atomic_load_reserve64(ps3_atomic_domain *d, ps3_atomic_owner owner, uint64_t a, uint64_t *value) {
    return load_reserve(d, owner, a, 8, value);
}
static ps3_guest_memory_status store_conditional(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                uint64_t a, unsigned width, uint64_t value, int *stored) {
    RMW_SCOPE_AT(d, a, width);
    if (!valid_domain(d)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, owner);
    if (!r) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    int matches = r->reserved && r->address == a && r->width == width;
    r->reserved = 0;
    if (!stored || !scalar_range(a, width)) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    if (!matches) {
        *stored = 0;
        return leave_plain(d, PS3_GUEST_MEMORY_OK);
    }
    unsigned char bytes[8];
    encode(bytes, width, value);
    ps3_guest_memory_status status = d->backing.write(d->backing.context, (uint32_t)a, bytes, width);
    if (status == PS3_GUEST_MEMORY_OK) {
        invalidate(d, a, width);
        *stored = 1;
    }
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_store_conditional32(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                     uint64_t a, uint32_t value, int *stored) {
    return store_conditional(d, owner, a, 4, value, stored);
}
ps3_guest_memory_status ps3_atomic_store_conditional64(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                     uint64_t a, uint64_t value, int *stored) {
    return store_conditional(d, owner, a, 8, value, stored);
}
ps3_guest_memory_status ps3_atomic_load_reserve128(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                  uint64_t a, void *buffer) {
    RMW_SCOPE_AT(d, a, PS3_RESERVATION_GRANULE_BYTES);
    if (!valid_domain(d) || !buffer || !scalar_range(a, PS3_RESERVATION_GRANULE_BYTES))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    {   /* Decision 87: mark the line before reading it (the busy count above is already raised, with a barrier,
         * so a lock-free writer that stored without seeing this bit is still caught by its second test). */
        const uint32_t sticky = ps3_atomic_sticky_index(a);
        const uint64_t bit = UINT64_C(1) << (sticky & 63u);
        if (!(__atomic_load_n(&g_ps3_atomic_sticky[sticky >> 6], __ATOMIC_RELAXED) & bit)) {
            if (!(__atomic_fetch_or(&g_ps3_atomic_sticky[sticky >> 6], bit, __ATOMIC_SEQ_CST) & bit))
                __atomic_fetch_add(&g_ps3_atomic_sticky_lines, 1u, __ATOMIC_RELAXED);
            atomic_thread_fence(memory_order_seq_cst);
        }
    }
    enter_plain(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, owner);
    if (!r) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    unsigned char bytes[PS3_RESERVATION_GRANULE_BYTES];
    LINE_STRIPE_LOCK(a);
    ps3_guest_memory_status status = d->backing.read(d->backing.context, (uint32_t)a, bytes, sizeof bytes);
    LINE_STRIPE_UNLOCK(a);
    if (status == PS3_GUEST_MEMORY_OK) {
        memcpy(buffer, bytes, sizeof bytes);
        r->address = (uint32_t)a;
        r->width = sizeof bytes;
        r->reserved = 1;
    }
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_store_conditional128(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                       uint64_t a, const void *buffer, int *stored) {
    RMW_SCOPE_AT(d, a, PS3_RESERVATION_GRANULE_BYTES);
    if (!valid_domain(d)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, owner);
    if (!r) return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    int matches = r->reserved && r->address == a && r->width == PS3_RESERVATION_GRANULE_BYTES;
    r->reserved = 0;
    if (!buffer || !stored || !scalar_range(a, PS3_RESERVATION_GRANULE_BYTES))
        return leave_plain(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    if (!matches) {
        *stored = 0;
        return leave_plain(d, PS3_GUEST_MEMORY_OK);
    }
    unsigned char bytes[PS3_RESERVATION_GRANULE_BYTES];
    memcpy(bytes, buffer, sizeof bytes);
    LINE_STRIPE_LOCK(a);
    ps3_guest_memory_status status = d->backing.write(d->backing.context, (uint32_t)a, bytes, sizeof bytes);
    LINE_STRIPE_UNLOCK(a);
    if (status == PS3_GUEST_MEMORY_OK) {
        invalidate(d, a, PS3_RESERVATION_GRANULE_BYTES);
        *stored = 1;
    }
    return leave_plain(d, status);
}
ps3_guest_memory_status ps3_atomic_store_unconditional128(ps3_atomic_domain *d, ps3_atomic_owner owner,
                                                         uint64_t a, const void *buffer) {
    if (!valid_domain(d) || !buffer || !scalar_range(a, PS3_RESERVATION_GRANULE_BYTES))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    mark_touched(a, PS3_RESERVATION_GRANULE_BYTES);   /* Decision 120 (enter() fences after raising locked_ops) */
    enter(d); DEBUG_ENTER(d);
    ps3_atomic_owner_record *r = find_owner(d, owner);
    if (!r) return leave(d, PS3_GUEST_MEMORY_INVALID_RANGE);
    unsigned char bytes[PS3_RESERVATION_GRANULE_BYTES];
    memcpy(bytes, buffer, sizeof bytes);
    LINE_STRIPE_LOCK(a);
    ps3_guest_memory_status status = d->backing.write(d->backing.context, (uint32_t)a, bytes, sizeof bytes);
    LINE_STRIPE_UNLOCK(a);
    if (status == PS3_GUEST_MEMORY_OK) {
        invalidate(d, a, PS3_RESERVATION_GRANULE_BYTES);
    }
    return leave(d, status);
}
ps3_guest_memory_status ps3_atomic_add16(ps3_atomic_domain *d, uint64_t a, int32_t delta, uint16_t *result) {
    RMW_SCOPE_AT(d, a, 2);
    if (!valid_domain(d) || !result || !scalar_range(a, 2)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    unsigned char bytes[2];
    ps3_guest_memory_status status = d->backing.read(d->backing.context, (uint32_t)a, bytes, 2);
    if (status != PS3_GUEST_MEMORY_OK) return leave_plain(d, status);
    uint16_t value = (uint16_t)((uint32_t)decode(bytes, 2) + (uint32_t)delta);
    encode(bytes, 2, value);
    status = d->backing.write(d->backing.context, (uint32_t)a, bytes, 2);
    if (status == PS3_GUEST_MEMORY_OK) {
        invalidate(d, a, 2);
        *result = value;
    }
    return leave_plain(d, status);
}
static ps3_guest_memory_status compare_exchange(ps3_atomic_domain *d, uint64_t a, unsigned width,
                                               uint64_t expected, uint64_t desired, ps3_atomic_compare_result *result) {
    RMW_SCOPE_AT(d, a, width);
    if (!valid_domain(d) || !result || !scalar_range(a, width)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    enter_plain(d); DEBUG_ENTER(d);
    unsigned char bytes[8];
    ps3_guest_memory_status status = d->backing.read(d->backing.context, (uint32_t)a, bytes, width);
    if (status != PS3_GUEST_MEMORY_OK) return leave_plain(d, status);
    ps3_atomic_compare_result staged = {decode(bytes, width), 0};
    if (staged.observed == expected) {
        encode(bytes, width, desired);
        status = d->backing.write(d->backing.context, (uint32_t)a, bytes, width);
        if (status != PS3_GUEST_MEMORY_OK) return leave_plain(d, status);
        invalidate(d, a, width);
        staged.exchanged = 1;
    }
    *result = staged;
    return leave_plain(d, PS3_GUEST_MEMORY_OK);
}
ps3_guest_memory_status ps3_atomic_compare_exchange32(ps3_atomic_domain *d, uint64_t a, uint32_t expected,
                                                    uint32_t desired, ps3_atomic_compare_result *result) {
    return compare_exchange(d, a, 4, expected, desired, result);
}
ps3_guest_memory_status ps3_atomic_compare_exchange64(ps3_atomic_domain *d, uint64_t a, uint64_t expected,
                                                    uint64_t desired, ps3_atomic_compare_result *result) {
    return compare_exchange(d, a, 8, expected, desired, result);
}
static ps3_guest_memory_status port_commit(void *d, uint32_t a, uint64_t n) { return ps3_atomic_commit(d, a, n); }
static ps3_guest_memory_status port_decommit(void *d, uint32_t a, uint64_t n) { return ps3_atomic_decommit(d, a, n); }
static ps3_guest_memory_status port_read(void *d, uint32_t a, void *p, size_t n) { return ps3_atomic_read(d, a, p, n); }
static ps3_guest_memory_status port_write(void *d, uint32_t a, const void *p, size_t n) { return ps3_atomic_write(d, a, p, n); }
static ps3_guest_memory_status port_zero(void *d, uint32_t a, uint64_t n) { return ps3_atomic_zero(d, a, n); }
ps3_guest_memory ps3_atomic_domain_memory(ps3_atomic_domain *d) {
    ps3_guest_memory port = {d, port_commit, port_decommit, port_read, port_write, port_zero};
    return port;
}
