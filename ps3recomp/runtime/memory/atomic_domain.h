#ifndef PS3RECOMP_ATOMIC_DOMAIN_H
#define PS3RECOMP_ATOMIC_DOMAIN_H
#include "guest_space.h"
#include "../sync/guard.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_RESERVATION_GRANULE_BYTES = 128 };
typedef struct {
    uint64_t generation;
    uint32_t address;
    unsigned active, reserved, width, reservation_lost;
} ps3_atomic_owner_record;
typedef struct {
    uint64_t generation, size;
    uint32_t address;
    unsigned active;
} ps3_atomic_pin_record;
/* Injected futex-style notification port (OS mechanics live only in adapters).
 * Distinct from sync/wait.h's guard-handoff condvar port: waiters here sleep on
 * a word OUTSIDE every guard, so owned services can block without holding the
 * shared domain guard.
 * wait: sleep while __atomic_load(word)==expected, the optional cancel flag is
 * clear and the MONOTONIC deadline (UINT64_MAX = none) has not passed; the
 * predicate is evaluated under the same exclusion wake_all acquires, so a change
 * published before wake_all is never lost. Spurious WOKEN returns are allowed;
 * FAILURE reports an unrecoverable provider error (never treated as success).
 * Non-reentrancy contract: wake_all and wait never call the domain, never take
 * the domain guard and run no consumer code. The domain calls wake_all with its
 * guard held (lock order: domain guard -> port exclusion only).
 * Lifetime: the port and domain must not be torn down until every waiter has
 * returned and been joined. */
typedef enum { PS3_NOTIFY_WOKEN, PS3_NOTIFY_TIMEOUT, PS3_NOTIFY_FAILURE } ps3_notify_wait_result;
typedef struct ps3_notify_port {
    void *context;
    ps3_notify_wait_result (*wait)(void *context, const uint64_t *word, uint64_t expected,
                                 const uint32_t *cancel, uint64_t deadline_ns);
    void (*wake_all)(void *context);
} ps3_notify_port;
/* Cooperative cancellation token; `requested` is accessed only atomically. */
typedef struct ps3_notify_cancel { uint32_t requested; } ps3_notify_cancel;
/* Root (2026-11-13, coordinator request after console run rv56s): the layout of the domain by who WRITES a field.
 *
 * The console profile put 25 % of the guest main thread in lean_store, the lock-free store path, with the stalls right after the acquire loads of locked_ops, reserved_map and
 * busy_map. Those loads read words that other cores keep invalidating: locked_ops, reserved_mask and debug_line were written by every guarded operation and sat in the same
 * 64-byte line as the first words of reserved_map, so every store of every other thread paid a cache-line transfer (false sharing) after any guarded operation anywhere.
 *
 * The domain is now four groups, each separated from the next by at least PS3_ATOMIC_GROUP_GAP (64) bytes, so that no two groups can share a cache line whatever the address the
 * object has (the struct need not be 64-aligned; the groups are separated by padding, not by alignment):
 *   1. cold: set by init, only read afterwards (backing, guard, owner and pin arrays, wait port). Never written while the domain is live.
 *   2. locked_ops: read by every lock-free store, written only by the guarded operations that are not tied to one address (owner acquire/release, commit, pins, waits).
 *   3. written: loss_epoch, reserved_mask, debug_line. Written by guarded operations; read only by waiters (loss_epoch) and diagnostics. No lock-free store reads them.
 *   4. reserved_map (128 bytes): read by every lock-free store, written only when the set of reserved granules changes.
 *   5. busy_map: read by every lock-free store (the word of the store's own granule bucket), written by every read-modify-write at its begin and end.
 *      (A 64-byte line holds 16 buckets; a store whose bucket shares a line with a busy one still pays for the transfer. Spreading the buckets over more lines is a
 *      possible next step, left out here because it trades the transfers for L1 capacity.)
 * What a lock-free store reads (locked_ops, busy_map[b], reserved_map[w], and the process-wide sticky bitmap) is therefore never in a
 * line that every guarded operation writes. */
enum { PS3_ATOMIC_GROUP_GAP = 64 };
typedef struct ps3_atomic_domain {
    /* 1. cold */
    ps3_guest_memory backing;
    ps3_instance_guard guard;
    ps3_atomic_owner_record *owners;
    size_t owner_count;
    ps3_atomic_pin_record *pins;
    size_t pin_count;
    unsigned initialized;
    ps3_notify_port wait_port;
    /* Decision 135: the host pointer of a fully committed guest range, or NULL (ps3_atomic_domain_set_direct). NULL: every DMA is guarded. */
    const void *(*direct)(void *context, uint32_t address, uint64_t length);
    void *direct_context;
    unsigned char gap_cold_[PS3_ATOMIC_GROUP_GAP];
    /* 2. Root (2026-11-12, Decision 60): what a lock-free writer must look at. locked_ops counts the guarded operations in progress that are not tied to one address (see
     * enter()); the read-modify-write operations (load-reserve, store-conditional, compare-exchange, add) are counted per granule bucket in busy_map (Decision 76). Only read and
     * written with seq-cst builtins. See ps3_atomic_domain_write_conflicts. */
    volatile uint32_t locked_ops;
    unsigned char gap_locked_[PS3_ATOMIC_GROUP_GAP];
    /* 3. written by guarded operations, never read by a lock-free store.
     * loss_epoch: bumped when a reservation loss is latched; the address the wait port sleeps on.
     * reserved_mask: one bit per held reservation, chosen by a hash of its granule; republished (only when it changed) before the guard is left. Nothing reads it but tests.
     * debug_line: diagnostic, the source line of the operation that last took the guard (atomic_domain.c). Only written when PS3_ATOMIC_DEBUG_LINE is 1 (off in the console
     * build, lle.mk LLE_ATOMIC_DEBUG_LINE); then it reads 0. */
    uint64_t loss_epoch;
    volatile uint64_t reserved_mask;
    volatile uint32_t debug_line;
    volatile uint32_t guard_inside;   /* Decision 136: 1 while a thread holds the guard (written by the holder only; read by threads about to take it) */
    unsigned char gap_written_[PS3_ATOMIC_GROUP_GAP];
    /* 4. Root (2026-11-12, Decision 75): the fine reservation map, PS3_RESERVED_MAP_BITS bits, one per held
     * reservation granule (same hash idea as reserved_mask, 16 times the bits). With about forty guest threads each
     * leaving a reservation behind, the 64-bit mask had most of its bits set and a large share of all lock-free
     * stores were repeated for nothing. The lock-free writer tests this map; reserved_mask is still maintained. */
    volatile uint64_t reserved_map[16];
    unsigned char gap_map_[PS3_ATOMIC_GROUP_GAP];
    /* 5. Root (2026-11-12, Decision 76): read-modify-write operations in flight, counted per granule bucket (the same
     * index as reserved_map). They used to raise the one global locked_ops; with the guard busy, each waited for it
     * with the count already raised, and every lock-free store anywhere was repeated meanwhile (92 % of all repeats
     * in build r77). A store now repeats for an operation only when it is on the store's own granule bucket.
     * locked_ops remains for the guarded operations that are not tied to one address (waits, commits, owners). */
    volatile uint32_t busy_map[1024];
    unsigned char gap_end_[PS3_ATOMIC_GROUP_GAP];   /* whatever follows the domain in an enclosing object does not share busy_map's last line */
} ps3_atomic_domain;
enum { PS3_RESERVED_MAP_BITS = 1024 };
static inline uint32_t ps3_atomic_granule_index(uint64_t address)
{
    const uint64_t line = address / PS3_RESERVATION_GRANULE_BYTES;
    return (uint32_t)((line ^ (line >> 10) ^ (line >> 20)) & (PS3_RESERVED_MAP_BITS - 1u));
}
/* The bit of reserved_mask that stands for the reservation granule containing `address`. */
static inline uint64_t ps3_atomic_granule_bit(uint64_t address)
{
    const uint64_t line = address / PS3_RESERVATION_GRANULE_BYTES;
    return UINT64_C(1) << ((line ^ (line >> 6) ^ (line >> 12)) & 63u);
}
/* For a writer that stores to the shared backing WITHOUT the guard (a composition with a flat guest base).
 *
 * The contract: store the bytes, then a full barrier, then call this. Zero means the store is complete: no
 * read-modify-write operation is in progress and no reservation covers a granule the store touched (a load-reserve raises
 * locked_ops, with a barrier, BEFORE it reads, so it either saw the new bytes or this call sees it). Non-zero
 * means the writer must repeat the store through ps3_atomic_write, which serializes it, drops the overlapping
 * reservations and wakes their waiters. A false positive (hash collision, an unrelated read-modify-write) only
 * costs that repeat. A reader needs nothing. `length` is at most one granule. */
static inline int ps3_atomic_domain_write_conflicts(const ps3_atomic_domain *d, uint64_t address, uint64_t length)
{
    const uint32_t first = ps3_atomic_granule_index(address), last = ps3_atomic_granule_index(address + (length ? length - 1 : 0));
    if (__atomic_load_n(&d->locked_ops, __ATOMIC_SEQ_CST) != 0 || __atomic_load_n(&d->busy_map[first], __ATOMIC_SEQ_CST) != 0) return 1;
    if (last != first && __atomic_load_n(&d->busy_map[last], __ATOMIC_SEQ_CST) != 0) return 1;   /* the same bucket is not read twice */
    return (__atomic_load_n(&d->reserved_map[first >> 6], __ATOMIC_SEQ_CST) & (UINT64_C(1) << (first & 63u))) != 0 ||
           (last != first && (__atomic_load_n(&d->reserved_map[last >> 6], __ATOMIC_SEQ_CST) & (UINT64_C(1) << (last & 63u))) != 0);
}
/* Root (2026-11-12, Decision 87): lines that have ever held a 128-byte (SPU) reservation. A lock-free writer
 * must not store to such a line at all: it goes through ps3_atomic_write, which stores and drops the reservations
 * in one guarded step. Before, the writer stored first and dropped the reservation later, after waiting for the
 * guard; an SPU store-conditional that got the guard in between succeeded with a line read BEFORE that store
 * (review 2026-11-12: a lost wake-up in the game's SPURS event flag fits exactly this). The table is process-wide
 * (one domain per process on the console) and bits are never cleared: 65536 bits, hashed by line. */
/* The tables below are read by every lock-free store (sticky) or by every PPU load of a sticky line (line_seq, global_seq): they start on their own cache line and the
 * counters that are written on events are on lines of their own (atomic_domain.c), so a counter update never invalidates a line a store reads. */
#define PS3_ATOMIC_LINE_ALIGNED __attribute__((aligned(64)))
extern volatile uint64_t g_ps3_atomic_sticky[1024] PS3_ATOMIC_LINE_ALIGNED;
extern volatile uint64_t g_ps3_atomic_sticky_lines;   /* how many bits are set (diagnostic) */
static inline uint32_t ps3_atomic_sticky_index(uint64_t address)
{
    const uint64_t line = address / PS3_RESERVATION_GRANULE_BYTES;
    return (uint32_t)((line ^ (line >> 16)) & 65535u);
}
static inline int ps3_atomic_sticky_hit(uint64_t address, uint64_t length)
{
    const uint32_t first = ps3_atomic_sticky_index(address), last = ps3_atomic_sticky_index(address + (length ? length - 1 : 0));
    return ((__atomic_load_n(&g_ps3_atomic_sticky[first >> 6], __ATOMIC_SEQ_CST) >> (first & 63u)) & 1u) != 0 ||
           (last != first && ((__atomic_load_n(&g_ps3_atomic_sticky[last >> 6], __ATOMIC_SEQ_CST) >> (last & 63u)) & 1u) != 0);
}
/* Root (2026-11-13, coordinator request after console run rv56s): the same two tests for the lock-free STORE paths (fast_store and lean_store in ppu_memory_context.cpp), with
 * loads only as strong as the protocol needs. Use them only in the sequence
 *      test 1 (before the store):   sticky_hit_fast || write_conflicts_fast  -> true: store through ps3_atomic_write instead
 *      store; __atomic_thread_fence(__ATOMIC_SEQ_CST);
 *      test 2 (after the fence):    the same pair                            -> true: repeat the store through ps3_atomic_write
 * The callers that have no explicit fence before their test but a seq-cst read-modify-write (lwmutex fast_settle) keep ps3_atomic_sticky_hit and
 * ps3_atomic_domain_write_conflicts above, whose loads are all seq-cst: after a seq-cst RMW a plain load may still be satisfied before the RMW's store half.
 *
 * ORDERING ARGUMENT. Two things have to hold, and neither needs a seq-cst load.
 *  (a) Dekker between a lock-free writer W and a guarded operation G (the Decision 60/87 protocol):
 *          W: store X;  FENCE;  read the indicators (sticky bit, locked_ops, busy_map, reserved_map)
 *          G: raise an indicator with a seq-cst read-modify-write (g_ps3_atomic_sticky fetch_or in ps3_atomic_load_reserve128, busy_map in rmw_begin, locked_ops in enter);
 *             FENCE (all three issue atomic_thread_fence(seq_cst) right after); only then read X (the backing read).
 *      Both FENCEs are seq-cst fences, so they are totally ordered. If G's fence is first, everything G raised before it is visible to the loads W makes after its own fence; if
 *      W's fence is first, the store of X is visible to G's read after G's fence. Either W sees the indicator and repeats the store through the guard, or G's read sees the store.
 *      The loads that come after W's fence need no ordering strength of their own for this: plain loads do, so the sticky bitmap and reserved_map words are read with plain loads
 *      (LDR, not LDAR).
 *  (b) G publishes a reservation BEFORE it lowers its busy count (leave_counted stores reserved_map, then rmw_end lowers busy_map with a seq-cst RMW, which releases). W must not
 *      read busy_map == 0 (G has finished) and then a stale reserved_map with no bit. So W reads locked_ops and busy_map with ACQUIRE loads first, and reserved_map after
 *      them: an acquire load keeps every later load of W behind it, so reading the lowered count implies reading the published bit. (On AArch64 acquire and seq-cst loads are the
 *      same instruction, LDAR. What is saved is the LDAR of the sticky bitmap and reserved_map words, and the second load of a bucket or word when the first and last granule of the
 *      store are the same, which is nearly always. There are now at most two LDAR per test, both on lines no guarded operation writes except an operation on the same bucket.)
 * Test 1 has no fence in front of it and needs none: it is not part of (a). It makes a store whose indicators are ALREADY visible go through the guard (Decision 87: a line that
 * ever held a 128-byte reservation is never stored to without the guard). A bit that is set at the same moment is the case test 2 exists for, and sticky bits are never cleared, so
 * a plain load sees any bit set before the call returns from memory within a store-buffer drain; it cannot make a store unsafe that the two-test sequence made safe. The line sequence
 * numbers of the granule-reservation fix (Decision 112) are raised inside the domain's guarded writes (invalidate), so every store that test 1 or test 2 sends to ps3_atomic_write
 * still bumps them; nothing here touches that path. */
/* -DPS3_ATOMIC_FAST_STORE_TESTS=0 (lle.mk LLE_ATOMIC_FAST_STORE_TESTS=0) makes every load below seq-cst again, as ps3_atomic_sticky_hit and ps3_atomic_domain_write_conflicts. */
#ifndef PS3_ATOMIC_FAST_STORE_TESTS
#define PS3_ATOMIC_FAST_STORE_TESTS 1
#endif
#if PS3_ATOMIC_FAST_STORE_TESTS
#define PS3_ATOMIC_PLAIN_ORDER __ATOMIC_RELAXED
#define PS3_ATOMIC_ACQUIRE_ORDER __ATOMIC_ACQUIRE
#else
#define PS3_ATOMIC_PLAIN_ORDER __ATOMIC_SEQ_CST
#define PS3_ATOMIC_ACQUIRE_ORDER __ATOMIC_SEQ_CST
#endif
static inline int ps3_atomic_sticky_hit_fast(uint64_t address, uint64_t length)
{
    const uint32_t first = ps3_atomic_sticky_index(address), last = ps3_atomic_sticky_index(address + (length ? length - 1 : 0));
    return ((__atomic_load_n(&g_ps3_atomic_sticky[first >> 6], PS3_ATOMIC_PLAIN_ORDER) >> (first & 63u)) & 1u) != 0 ||
           (last != first && ((__atomic_load_n(&g_ps3_atomic_sticky[last >> 6], PS3_ATOMIC_PLAIN_ORDER) >> (last & 63u)) & 1u) != 0);
}
/* Root (2026-11-15, Decision 120, console run rv61s): lines that have EVER been the target of a read-modify-write operation of the
 * domain (load-reserve 32/64/128, conditional stores, compare-exchange, add16, the unconditional line store). Same hash and size as
 * the sticky table; bits are never cleared. The lock-free store paths (fast_store, lean_store) send a store to a line with no bit
 * straight to memory, with no fence and no conflict test: the game's main thread spent 26 % of its time in those tests and the full
 * barrier (rv61s profile), almost all of it on lines (stack, buffers, object fields) no atomic operation ever looks at.
 * What this gives up: a guarded operation marks the line (seq-cst RMW, then its own fence) before it reads, but the plain store does not
 * fence after its test, so a store that runs exactly during the FIRST guarded operation ever made on its line may be overwritten by that
 * operation when both write the same bytes. From the second operation on the line on, every store to it follows the full Decision 60/87
 * protocol. -DPS3_ATOMIC_TOUCHED_STORES=0 (lle.mk LLE_ATOMIC_TOUCHED_STORES=0) restores the full protocol for every store. */
#ifndef PS3_ATOMIC_TOUCHED_STORES
#define PS3_ATOMIC_TOUCHED_STORES 1
#endif
extern volatile uint64_t g_ps3_atomic_touched[1024] PS3_ATOMIC_LINE_ALIGNED;
extern volatile uint64_t g_ps3_atomic_touched_lines;   /* how many bits are set (diagnostic) */
static inline int ps3_atomic_touched_hit_fast(uint64_t address, uint64_t length)
{
    const uint32_t first = ps3_atomic_sticky_index(address), last = ps3_atomic_sticky_index(address + (length ? length - 1 : 0));
    return ((__atomic_load_n(&g_ps3_atomic_touched[first >> 6], __ATOMIC_RELAXED) >> (first & 63u)) & 1u) != 0 ||
           (last != first && ((__atomic_load_n(&g_ps3_atomic_touched[last >> 6], __ATOMIC_RELAXED) >> (last & 63u)) & 1u) != 0);
}
static inline int ps3_atomic_domain_write_conflicts_fast(const ps3_atomic_domain *d, uint64_t address, uint64_t length)
{
    const uint32_t first = ps3_atomic_granule_index(address), last = ps3_atomic_granule_index(address + (length ? length - 1 : 0));
    if (__atomic_load_n(&d->locked_ops, PS3_ATOMIC_ACQUIRE_ORDER) != 0 || __atomic_load_n(&d->busy_map[first], PS3_ATOMIC_ACQUIRE_ORDER) != 0) return 1;
    if (last != first && __atomic_load_n(&d->busy_map[last], PS3_ATOMIC_ACQUIRE_ORDER) != 0) return 1;
    return (__atomic_load_n(&d->reserved_map[first >> 6], PS3_ATOMIC_PLAIN_ORDER) & (UINT64_C(1) << (first & 63u))) != 0 ||
           (last != first && (__atomic_load_n(&d->reserved_map[last >> 6], PS3_ATOMIC_PLAIN_ORDER) & (UINT64_C(1) << (last & 63u))) != 0);
}
/* Root (2026-11-13, Decision 112, hardware run rv50s): a PPU reservation that sees what the hardware's does. The generated and
 * lifted PPU code takes lwarx/ldarx as a plain load plus the loaded value, and ppu_stwcx32/ppu_stdcx64 take the domain reservation
 * only at the store itself: the store succeeds whenever the word still has the loaded value. On hardware the reservation covers the
 * whole 128-byte line from the load on: an SPU putllc (or any store) to ANY word of the line between lwarx and stwcx makes the stwcx
 * fail. libsre's LFQueue / taskset signal code reads the waiting words of the line between lwarx and stwcx; with the value check
 * alone it acts on a stale 'nobody is waiting' and the SPU task sleeps for ever (rv47s and rv50s: one request pushed, never popped).
 *
 * The fix is for the lines SPUs touch only (the sticky lines above, 12 in a whole run): every write the domain makes to such a line
 * (ps3_atomic_write and the like, which is also where a PPU store to a sticky line is forced) raises a sequence number of the line.
 * A PPU load of a sticky line notes the sequence BEFORE it reads (ps3_atomic_reservation_snapshot / _note, per host thread, the last
 * four distinct addresses); the stwcx asks ps3_atomic_reservation_intact(): no note (the line was not sticky when it loaded) or an
 * unchanged sequence: go on as before; a changed sequence: the store fails, exactly as the hardware's, and the guest loop retries.
 * Hash collisions of the line index and the global bump (a huge range was written) only fail a store that could have succeeded.
 * g_ps3_atomic_granule_reservations = 0 (or -DPS3_ATOMIC_GRANULE_RESERVATIONS=0) gives the old value-only behaviour. */
extern volatile uint32_t g_ps3_atomic_line_seq[65536] PS3_ATOMIC_LINE_ALIGNED;
extern volatile uint32_t g_ps3_atomic_global_seq;
extern int g_ps3_atomic_granule_reservations;
extern volatile uint64_t g_ps3_atomic_reservation_checked, g_ps3_atomic_reservation_lost, g_ps3_atomic_reservation_noted;
#define PS3_ATOMIC_NO_SEQ 0xFFFFFFFFu   /* (not an enum: ISO C before C23 limits enumerators to int) */
/* The sequence of the line holding `address` when it is sticky, else PS3_ATOMIC_NO_SEQ. Call it before the load it describes. */
static inline uint32_t ps3_atomic_reservation_snapshot(uint64_t address)
{
    const uint32_t index = ps3_atomic_sticky_index(address);
    if (!((__atomic_load_n(&g_ps3_atomic_sticky[index >> 6], __ATOMIC_RELAXED) >> (index & 63u)) & 1u)) return PS3_ATOMIC_NO_SEQ;
    const uint32_t seq = __atomic_load_n(&g_ps3_atomic_line_seq[index], __ATOMIC_ACQUIRE) + __atomic_load_n(&g_ps3_atomic_global_seq, __ATOMIC_ACQUIRE);
    return seq == PS3_ATOMIC_NO_SEQ ? 0u : seq;
}
/* Remember the snapshot of the load of `address` (`seq` as returned above; PS3_ATOMIC_NO_SEQ forgets the address). Per host thread. */
void ps3_atomic_reservation_note(uint64_t address, uint32_t seq);
/* Root (2026-11-15, Decision 116): called (guard held) whenever a write latched a reservation loss of some owner. NULL (the
 * default) does nothing. The hook must not reenter the domain; the SPU runner uses it to wake its idle workers at once
 * instead of at the end of their 200 us pause sleep (native/adapters/spu_group_runner.c). */
extern void (*ps3_atomic_reservation_loss_hook)(void);
/* Root (2026-11-15, Decision 121, console run rv61s): called (guard held) after EVERY guarded write of guest memory (ps3_atomic_write,
 * conditional and unconditional line stores, compare-exchange, add, zero, decommit), whether or not it latched a loss. NULL (the
 * default) does nothing. Same contract as the loss hook. The composition uses it to wake the game's main thread from its SPU task
 * wait (native/adapters/taskwait_horizon.cpp) as soon as an SPU stores a completion, instead of at the end of a fixed pause.
 * Decision 123: the hook gets the written range [address, address + length). */
extern void (*ps3_atomic_guarded_write_hook)(uint64_t address, uint64_t length);
/* Decision 123: a small tag per host thread (0 = untagged; the SPU runner tags its workers with slot + 1), read by the write hook to say
 * which thread a guarded write came from. */
void ps3_atomic_set_writer_tag(unsigned tag);
unsigned ps3_atomic_writer_tag(void);
/* The conditional store to `address` asks: was the line written since the load that noted it? 0 = yes, the store must fail. The note
 * is consumed either way. 1 when there is no note for the address. */
int ps3_atomic_reservation_intact(uint64_t address);
typedef struct {
    const ps3_atomic_domain *domain;
    size_t slot;
    uint64_t generation;
} ps3_atomic_owner;
typedef struct {
    const ps3_atomic_domain *domain;
    size_t slot;
    uint64_t generation;
} ps3_atomic_pin;
typedef struct {
    uint64_t observed;
    int exchanged;
} ps3_atomic_compare_result;

/* Composition owns the fresh zero-initialized domain and bounded record arrays.
 * Initialize once, then retain all storage/backing/guard until every user is
 * joined and all owners/pins released. Records are private after initialization.
 * Never copy or reinitialize a live domain. Each owner represents one serialized
 * execution agent; do not run independent instruction streams through one owner.
 * Handles are valid only for the original domain object's lifetime. Pointer
 * identity cannot distinguish a destroyed domain recreated at the same address.
 * All accesses to shared backing, including mapping retirement, must use this
 * domain. Providers must obey guest_space.h and cannot reenter this domain or
 * contain host cancellation points. Raw backing access is outside the contract.
 * Operations are serialized/seq-cst initially, not a weak-memory timing model.
 * Stale/wrong-domain handles return INVALID_RANGE; exhausted slots/generations
 * return NO_MEMORY. No generation wraps. Outputs stay unchanged on error except
 * SC always consumes a valid owner's reservation, including on malformed input
 * or provider failure. LL failure preserves the owner's previous reservation.
 * A lost/mismatched reservation returns OK/stored=false after address/alignment
 * validation, without probing backing mappings. Guest exception mapping is a
 * separate CPU/ABI adapter decision, not inferred from that conditional failure.
 * Pins protect mapping lifetime, not bytes. Pin validation probes each covered
 * backing page once under the guard; zero-length pins are invalid.
 * Backing models ordinary guest RAM; MMIO/side-effecting providers are unsupported.
 * Host buffers must be disjoint from domain records and shared guest backing.
 * Cooperative cancellation occurs outside these operations; owner/pin handles
 * remain explicitly owned and must be released by caller cleanup after joining. */
ps3_guest_memory_status ps3_atomic_domain_init(ps3_atomic_domain *, ps3_guest_memory,
                                              ps3_instance_guard,
                                              ps3_atomic_owner_record *, size_t,
                                              ps3_atomic_pin_record *, size_t);
ps3_guest_memory ps3_atomic_domain_memory(ps3_atomic_domain *);
ps3_guest_memory_status ps3_atomic_owner_acquire(ps3_atomic_domain *, ps3_atomic_owner *);
ps3_guest_memory_status ps3_atomic_owner_release(ps3_atomic_domain *, ps3_atomic_owner *);
/* Consume this owner's reservation without accessing backing memory or retiring
 * the owner. Used when an attempted instruction is rejected by an outer adapter. */
ps3_guest_memory_status ps3_atomic_owner_clear_reservation(ps3_atomic_domain *, ps3_atomic_owner);
/* Atomically read and clear pending reservation loss. Explicit reservation
 * consumption does not itself signal loss; later LL does not erase pending loss. */
ps3_guest_memory_status ps3_atomic_owner_take_reservation_loss(ps3_atomic_domain *, ps3_atomic_owner, int *);
/* Attach the blocking port once, before any concurrent use; BUSY if already set.
 * Every newly latched reservation loss then bumps loss_epoch and wakes waiters. */
ps3_guest_memory_status ps3_atomic_domain_attach_wait_port(ps3_atomic_domain *, ps3_notify_port);
typedef enum {
    PS3_ATOMIC_WAIT_LOST, PS3_ATOMIC_WAIT_TIMEOUT, PS3_ATOMIC_WAIT_CANCELLED, PS3_ATOMIC_WAIT_INVALID
} ps3_atomic_wait_result;
/* Wait-any for reservation loss over 1..64 owners of this domain (the handle
 * array must stay immutable during the wait; owner release wakes and retires a
 * watched handle -> INVALID). Returns LOST
 * with *mask = every owner whose loss latch is pending (latches are NOT consumed;
 * use take_reservation_loss). A pending loss returns without sleeping. Cancel is
 * checked first (mask 0). INVALID (mask untouched) for bad arguments, stale or
 * foreign handles, no attached port, an owner that is neither latched nor
 * holding a reservation (it could never be woken), or a port FAILURE. Every exit
 * (including TIMEOUT) re-validates all handles first. Never polls. */
ps3_atomic_wait_result ps3_atomic_owners_wait_reservation_loss(ps3_atomic_domain *, const ps3_atomic_owner *,
                                                               size_t, uint64_t deadline_ns,
                                                               ps3_notify_cancel *, uint64_t *mask);
/* Publish cancellation, then wake every sleeper of the port that waits on it. */
void ps3_notify_cancel_request(ps3_notify_cancel *, const ps3_notify_port *);
int ps3_notify_cancel_is_requested(const ps3_notify_cancel *);
ps3_guest_memory_status ps3_atomic_pin_acquire(ps3_atomic_domain *, uint64_t, uint64_t, ps3_atomic_pin *);
ps3_guest_memory_status ps3_atomic_pin_release(ps3_atomic_domain *, ps3_atomic_pin *);
ps3_guest_memory_status ps3_atomic_read(ps3_atomic_domain *, uint64_t, void *, size_t);
ps3_guest_memory_status ps3_atomic_write(ps3_atomic_domain *, uint64_t, const void *, size_t);
/* Owned DMA checks the original domain/slot/generation before any provider call.
 * Handle checks do not authorize concurrent destruction; join before release. */
ps3_guest_memory_status ps3_atomic_owner_read(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, void *, size_t);
ps3_guest_memory_status ps3_atomic_owner_write(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, const void *, size_t);
/* Decision 135 (2026-11-16, console run rv69s: the five SPURS job threads queued about a third of their time on the domain guard, which every
 * SPU DMA took for its whole copy): plain SPU DMA (GET = ps3_atomic_dma_read, PUT = ps3_atomic_dma_write) without the guard where that is
 * provably the same as the guarded copy. Same results and status as ps3_atomic_owner_read / _write, which they fall back to.
 *
 * The rule, per 128-byte line of the range (maximal runs of lines of one kind are handled together):
 *   - a line that has EVER been the target of a read-modify-write of the domain (the touched table, Decision 120; it holds every sticky line,
 *     every line a reservation was ever taken on and every line a store-conditional, compare-exchange or add ever wrote) is copied under the
 *     guard, exactly as before (ps3_atomic_owner_read / _write: reservation loss latching, line sequences, loss hook, guarded-write hook);
 *   - any other line is copied directly (the host pointer from `direct`), then a full fence, then the touched bits of the run are read again:
 *     a run that became touched meanwhile is copied again under the guard (GET: read again; PUT: written again, which invalidates and bumps
 *     like any guarded write). A PUT that stays direct calls ps3_atomic_guarded_write_hook (the taskwait wake) as a guarded write would.
 * Why that is exact: every read-modify-write marks the touched bit of its line with a seq-cst RMW and issues a full fence BEFORE it reads or
 * writes the line (rmw_begin). With the copy's own fence between the copy and the second test, the two fences are totally ordered: either the
 * second test sees the bit (and the run is redone under the guard), or the operation's accesses come after the copy (it reads the PUT's bytes;
 * the GET did not see its writes). A line no read-modify-write has touched holds no reservation, no line sequence and no busy count, so a
 * direct PUT has nothing to invalidate; a direct GET can only race with plain stores, which the PPU's own lock-free loads and stores (lean_store,
 * Decision 110/120) already do without the guard. Bits are never cleared, so a run that was untouched at both tests was untouched throughout.
 * Off: -DPS3_ATOMIC_LOCKFREE_DMA=0 (lle.mk LLE_SPU_LOCKFREE_DMA=0), or no `direct` installed: both calls are ps3_atomic_owner_read / _write. */
ps3_guest_memory_status ps3_atomic_dma_read(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, void *, size_t);
ps3_guest_memory_status ps3_atomic_dma_write(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, const void *, size_t);
/* Install the direct resolver (before any thread uses the domain). `fn` returns the host address of guest `address` when [address,
 * address + length) is committed backing memory the domain's own backing reads and writes, else NULL (that DMA takes the guarded path). */
void ps3_atomic_domain_set_direct(ps3_atomic_domain *, const void *(*fn)(void *context, uint32_t address, uint64_t length), void *context);
/* Per host thread, since the last call (then zeroed): bytes copied direct / under the guard by GET and PUT, runs redone after the second
 * test (GET, PUT), DMAs with no direct pointer. */
/* Decision 136 (run rv71s: the SPURS job threads paid ~40 us per GETLLAR/PUTLLC, the sound SPU alone on its core 4.7 us): the guard waits of this host
 * thread, when ps3_atomic_guard_clock is set and the thread has a writer tag (the SPU workers): acquisitions, total and longest wait (ns), and
 * how many acquisitions spun out (the guard still looked held after the spin) and went to the guard's own sleeping wait. */
enum { PS3_ATOMIC_DMA_DIRECT_GET, PS3_ATOMIC_DMA_DIRECT_PUT, PS3_ATOMIC_DMA_GUARDED_GET, PS3_ATOMIC_DMA_GUARDED_PUT,
       PS3_ATOMIC_DMA_REDO_GET, PS3_ATOMIC_DMA_REDO_PUT, PS3_ATOMIC_DMA_NO_DIRECT,
       PS3_ATOMIC_GUARD_ACQUIRES, PS3_ATOMIC_GUARD_WAIT_NS, PS3_ATOMIC_GUARD_WAIT_MAX_NS, PS3_ATOMIC_GUARD_SPUN_OUT, PS3_ATOMIC_DMA_COUNTS };
/* Monotonic nanoseconds for the guard-wait counters (set once by the composition; NULL: not measured). */
extern uint64_t (*ps3_atomic_guard_clock)(void);
void ps3_atomic_dma_take_counts(uint64_t out[PS3_ATOMIC_DMA_COUNTS]);
ps3_guest_memory_status ps3_atomic_zero(ps3_atomic_domain *, uint64_t, uint64_t);
ps3_guest_memory_status ps3_atomic_commit(ps3_atomic_domain *, uint64_t, uint64_t);
ps3_guest_memory_status ps3_atomic_decommit(ps3_atomic_domain *, uint64_t, uint64_t);
ps3_guest_memory_status ps3_atomic_load_reserve32(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, uint32_t *);
ps3_guest_memory_status ps3_atomic_load_reserve64(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, uint64_t *);
ps3_guest_memory_status ps3_atomic_store_conditional32(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, uint32_t, int *);
ps3_guest_memory_status ps3_atomic_store_conditional64(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, uint64_t, int *);
/* Serialized line bytes, not a host scalar. Addresses are 128-byte aligned.
 * LL uses one backing read and preserves its previous reservation on error.
 * SC always consumes its valid owner's reservation, as scalar SC does.
 * Unconditional success invalidates overlapping reservations (including self),
 * preserving nonoverlapping reservations; failure leaves reservations unchanged.
 * This is a generic coherent line write, not queued MFC/channel/event policy. */
ps3_guest_memory_status ps3_atomic_load_reserve128(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, void *);
ps3_guest_memory_status ps3_atomic_store_conditional128(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, const void *, int *);
ps3_guest_memory_status ps3_atomic_store_unconditional128(ps3_atomic_domain *, ps3_atomic_owner, uint64_t, const void *);
ps3_guest_memory_status ps3_atomic_add16(ps3_atomic_domain *, uint64_t, int32_t, uint16_t *);
ps3_guest_memory_status ps3_atomic_compare_exchange32(ps3_atomic_domain *, uint64_t, uint32_t, uint32_t, ps3_atomic_compare_result *);
ps3_guest_memory_status ps3_atomic_compare_exchange64(ps3_atomic_domain *, uint64_t, uint64_t, uint64_t, ps3_atomic_compare_result *);
#ifdef __cplusplus
}
#endif
#endif
