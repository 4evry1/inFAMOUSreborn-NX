#define _GNU_SOURCE
#include "../adapters/demand_window.h"
#include "../adapters/heap_guest_space.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "DEMAND_WINDOW_FAIL line=%d\n", __LINE__); ++failures; } } while (0)

/* Counting guard: a real nonrecursive mutex that also tracks lock/unlock counts
 * and nesting depth so every case can assert the decorator leaves it balanced.
 * Every fixture gives the window and its backing two distinct guards: they lock
 * in domain -> window -> backing order, so sharing one nonrecursive mutex would
 * self-deadlock on the first on-demand granule commit. */
typedef struct counting_guard {
    pthread_mutex_t mutex;
    unsigned locks, unlocks;
    int depth;
} counting_guard;
static void counting_lock(void *context)
{
    counting_guard *guard = context;
    if (pthread_mutex_lock(&guard->mutex)) abort();
    ++guard->locks;
    ++guard->depth;
}
static void counting_unlock(void *context)
{
    counting_guard *guard = context;
    --guard->depth;
    ++guard->unlocks;
    if (pthread_mutex_unlock(&guard->mutex)) abort();
}
static ps3_instance_guard counting_port(counting_guard *guard)
{
    return (ps3_instance_guard){guard, counting_lock, counting_unlock};
}
static void counting_assert(const counting_guard *guard, int line)
{
    if (guard->depth) { fprintf(stderr, "DEMAND_WINDOW_UNBALANCED line=%d depth=%d\n", line, guard->depth); ++failures; }
    if (guard->locks != guard->unlocks) {
        fprintf(stderr, "DEMAND_WINDOW_UNBALANCED line=%d locks=%u unlocks=%u\n", line, guard->locks, guard->unlocks);
        ++failures;
    }
}
#define CHECK_GUARD(g) counting_assert(&(g), __LINE__)

/* malloc/free allocator over the real heap adapter. */
typedef struct allocator_fixture { size_t live; } allocator_fixture;
static void *fixture_allocate(void *context, size_t size)
{
    allocator_fixture *fixture = context;
    void *memory = malloc(size);
    if (memory) ++fixture->live;
    return memory;
}
static void fixture_deallocate(void *context, void *memory)
{
    allocator_fixture *fixture = context;
    if (memory) --fixture->live;
    free(memory);
}

/* Fake backing: records commit/decommit calls and can fail the nth commit. */
#define FAKE_WINDOW_PAGES 256u /* one bit per 4 KiB page of the shared 1 MiB window */
#define FAKE_CALLS 512
typedef struct fake_backing {
    counting_guard guard;
    unsigned commit_calls, decommit_calls;
    uint32_t commit_offsets[FAKE_CALLS];
    uint32_t decommit_offsets[FAKE_CALLS];
    uint64_t commit_lengths[FAKE_CALLS];
    unsigned fail_commit_at;   /* 1-based commit call index that fails; 0 = never */
    ps3_guest_memory_status fail_status;
    /* One bit per 4 KiB page over the whole window (WINDOW_SIZE == 1 MiB), so the
     * bit index is in range for any offset the shared window constants produce. */
    uint8_t committed[FAKE_WINDOW_PAGES / 8u]; /* one bit per 4 KiB page of the window */
    uint32_t granule;
    uint32_t base;
} fake_backing;
static int fake_committed(const fake_backing *fake, uint32_t address)
{
    uint32_t index = (address - fake->base) / 4096u;
    return fake->committed[index / 8] & (1u << (index % 8));
}
static ps3_guest_memory_status fake_commit(void *context, uint32_t address, uint64_t length)
{
    fake_backing *fake = context;
    if (fake->guard.depth != 0) { fprintf(stderr, "FAKE_GUARD_NOT_EXCLUSIVE\n"); ++failures; }
    const unsigned call = ++fake->commit_calls;
    if (call <= FAKE_CALLS) {
        fake->commit_offsets[call - 1] = address - fake->base;
        fake->commit_lengths[call - 1] = length;
    }
    if (fake->fail_commit_at && call == fake->fail_commit_at)
        return fake->fail_status;
    for (uint64_t page = 0; page < length; page += 4096u) {
        uint32_t index = (address - fake->base + (uint32_t)page) / 4096u;
        fake->committed[index / 8] |= (uint8_t)(1u << (index % 8));
    }
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status fake_decommit(void *context, uint32_t address, uint64_t length)
{
    fake_backing *fake = context;
    if (fake->guard.depth != 0) { fprintf(stderr, "FAKE_GUARD_NOT_EXCLUSIVE\n"); ++failures; }
    const unsigned call = ++fake->decommit_calls;
    if (call <= FAKE_CALLS) fake->decommit_offsets[call - 1] = address - fake->base;
    for (uint64_t page = 0; page < length; page += 4096u) {
        uint32_t index = (address - fake->base + (uint32_t)page) / 4096u;
        fake->committed[index / 8] &= (uint8_t)~(1u << (index % 8));
    }
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status fake_unsupported(void *context, uint32_t address, void *buffer, size_t length)
{
    (void)context; (void)address; (void)buffer; (void)length;
    return PS3_GUEST_MEMORY_UNMAPPED;
}
static ps3_guest_memory_status fake_write(void *context, uint32_t address, const void *buffer, size_t length)
{
    (void)context; (void)address; (void)buffer; (void)length;
    return PS3_GUEST_MEMORY_UNMAPPED;
}
static ps3_guest_memory_status fake_zero(void *context, uint32_t address, uint64_t length)
{
    (void)context; (void)address; (void)length;
    /* Zero needs no data path, so the fake accepts it: a case can then assert
     * that the decorator refused or rolled back before forwarding. */
    return PS3_GUEST_MEMORY_OK;
}
static void fake_init(fake_backing *fake, uint32_t base, uint32_t granule)
{
    memset(fake, 0, sizeof(*fake));
    fake->guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    fake->fail_status = PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    fake->base = base;
    fake->granule = granule;
}
static ps3_guest_memory fake_port(fake_backing *fake)
{
    return (ps3_guest_memory){fake, fake_commit, fake_decommit, fake_unsupported,
        fake_write, fake_zero};
}
static void fake_destroy(fake_backing *fake) { pthread_mutex_destroy(&fake->guard.mutex); }

/* Shared window constants used by the functional cases. */
#define WINDOW_BASE 0x10000000u
#define WINDOW_SIZE 0x00100000u     /* 16 granules of 64 KiB */
#define WINDOW_GRANULE 0x00010000u
#define WINDOW_CAP 0x00100000ull    /* == WINDOW_SIZE: the cap never binds in these cases */

typedef struct heap_window {
    allocator_fixture allocator;
    counting_guard heap_guard;   /* the heap backing's own guard */
    counting_guard window_guard; /* the decorator's guard: never the same object */
    ps3_heap_guest_space *heap;
    ps3_demand_window window;
    ps3_guest_memory port;
} heap_window;
static int heap_window_open(heap_window *fixture, uint64_t cap_bytes, uint32_t size, uint32_t granule)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->heap_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    fixture->window_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_allocator allocator = {&fixture->allocator, fixture_allocate, fixture_deallocate};
    if (ps3_heap_guest_space_create(&fixture->heap, allocator, counting_port(&fixture->heap_guard), 131072) !=
        PS3_GUEST_MEMORY_OK) return 0;
    if (&fixture->heap_guard.mutex == &fixture->window_guard.mutex) return 0;
    const ps3_guest_memory_status status = ps3_demand_window_init(&fixture->window,
        ps3_heap_guest_space_port(fixture->heap), counting_port(&fixture->window_guard),
        WINDOW_BASE, size, granule, cap_bytes);
    if (status != PS3_GUEST_MEMORY_OK) return 0;
    fixture->port = ps3_demand_window_port(&fixture->window);
    return 1;
}
static void heap_window_close(heap_window *fixture)
{
    CHECK(ps3_heap_guest_space_destroy(&fixture->heap) == PS3_GUEST_MEMORY_OK);
    CHECK(fixture->allocator.live == 0);
    CHECK_GUARD(fixture->window_guard);
    CHECK_GUARD(fixture->heap_guard);
    pthread_mutex_destroy(&fixture->window_guard.mutex);
    pthread_mutex_destroy(&fixture->heap_guard.mutex);
}

static ps3_guest_memory all_null_but(ps3_guest_memory memory, int skip)
{
    if (skip == 0) memory.commit = NULL;
    if (skip == 1) memory.decommit = NULL;
    if (skip == 2) memory.read = NULL;
    if (skip == 3) memory.write = NULL;
    if (skip == 4) memory.zero = NULL;
    return memory;
}

/* Case 1: every init refusal leaves the storage unchanged; a second init is refused. */
static void init_refusals(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, WINDOW_CAP, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_demand_window storage = fixture.window;
    ps3_demand_window before;
    memcpy(&before, &storage, sizeof(storage));
    const ps3_guest_memory good = ps3_demand_window_port(&fixture.window);
    const ps3_guest_memory wrong = {(void *)0x1, fake_commit, fake_decommit, fake_unsupported,
        fake_write, fake_zero};
    const ps3_instance_guard guard = counting_port(&fixture.window_guard);
    const ps3_instance_guard no_lock = {guard.context, NULL, counting_unlock};
    const ps3_instance_guard no_unlock = {guard.context, counting_lock, NULL};

#define REFUSED(call) do { \
        ps3_demand_window local; \
        memcpy(&local, &before, sizeof(local)); \
        CHECK((call) == PS3_GUEST_MEMORY_INVALID_RANGE); \
        CHECK(!memcmp(&local, &before, sizeof(local))); \
        CHECK_GUARD(fixture.window_guard); \
    } while (0)
    REFUSED(ps3_demand_window_init(NULL, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    for (int skip = 0; skip < 5; ++skip)
        REFUSED(ps3_demand_window_init(&storage, all_null_but(good, skip), guard,
            WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, (ps3_guest_memory){0, good.commit, good.decommit,
        good.read, good.write, good.zero}, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, no_lock, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, no_unlock, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    /* granule not a power of two, below a page, zero */
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, 0x00030000u, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, 2048u, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, 0u, WINDOW_CAP));
    /* size == 0, size not a multiple of the granule, base unaligned */
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, 0u, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE + 4096u, WINDOW_GRANULE, WINDOW_CAP));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE + 4096u, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP));
    /* past the end of the guest address space */
    REFUSED(ps3_demand_window_init(&storage, good, guard, 0xFFFF0000u, 0x00020000u, WINDOW_GRANULE, 0x00020000u));
    /* more granule slots than the bitmap holds */
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE,
        (PS3_DEMAND_WINDOW_MAX_GRANULES + 1u) * WINDOW_GRANULE, WINDOW_GRANULE,
        (PS3_DEMAND_WINDOW_MAX_GRANULES + 1u) * (uint64_t)WINDOW_GRANULE));
    /* cap zero, cap not a multiple of the granule, cap larger than the window */
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, 0u));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_GRANULE + 4096u));
    REFUSED(ps3_demand_window_init(&storage, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_SIZE + WINDOW_GRANULE));
#undef REFUSED
    /* A partially initialised storage (initialized set) is refused too. */
    ps3_demand_window half;
    memset(&half, 0, sizeof(half));
    half.initialized = 1;
    ps3_demand_window half_before = half;
    CHECK(ps3_demand_window_init(&half, good, guard, WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP) ==
        PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(!memcmp(&half, &half_before, sizeof(half)));
    /* The wrong-context port is never used: only non-NULL is required. */
    CHECK(wrong.context != NULL);
    heap_window_close(&fixture);
}

/* Case 2: outside the window everything forwards to the heap and works. */
static void outside_window(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, WINDOW_CAP, WINDOW_SIZE, WINDOW_GRANULE));
    unsigned char input[64], output[64];
    memset(input, 0x5A, sizeof(input));
    memset(output, 0xA5, sizeof(output));
    /* Before the window. */
    CHECK(fixture.port.commit(fixture.port.context, 0x0F000000u, 0x10000u) == PS3_GUEST_MEMORY_OK);
    CHECK(fixture.port.write(fixture.port.context, 0x0F000010u, input, sizeof(input)) == PS3_GUEST_MEMORY_OK);
    CHECK(fixture.port.read(fixture.port.context, 0x0F000010u, output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(input, output, sizeof(input)));
    /* After the window. */
    CHECK(fixture.port.commit(fixture.port.context, 0x10200000u, 0x10000u) == PS3_GUEST_MEMORY_OK);
    CHECK(fixture.port.write(fixture.port.context, 0x10200010u, input, sizeof(input)) == PS3_GUEST_MEMORY_OK);
    memset(output, 0, sizeof(output));
    CHECK(fixture.port.read(fixture.port.context, 0x10200010u, output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(input, output, sizeof(input)));
    /* Forwarding failures (unmapped page) still come from the heap. */
    CHECK(fixture.port.read(fixture.port.context, 0x0E000000u, output, 16) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(fixture.port.decommit(fixture.port.context, 0x10200000u, 0x10000u) == PS3_GUEST_MEMORY_OK);
    CHECK(fixture.port.read(fixture.port.context, 0x10200000u, output, 16) == PS3_GUEST_MEMORY_UNMAPPED);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 0 && stats.refused_window_commits == 0 && stats.refused_partial == 0);
    heap_window_close(&fixture);
}

/* Cases 3, 4, 5, 6, 7: the on-demand commit paths. */
static void commit_paths(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, WINDOW_CAP, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_guest_memory memory = fixture.port;
    unsigned char input[64], output[64];

    /* Case 3: first write inside the window commits exactly one granule. */
    const uint32_t first_address = WINDOW_BASE + 0x00030000u + 0x100u;
    memset(input, 0xC3, sizeof(input));
    CHECK(memory.write(memory.context, first_address, input, sizeof(input)) == PS3_GUEST_MEMORY_OK);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 1);
    CHECK(stats.committed_bytes == WINDOW_GRANULE);
    CHECK(stats.journal_count == 1 && stats.journal[0] == 0x00030000u);
    CHECK(stats.triggering_writes == 1 && stats.triggering_reads == 0 && stats.triggering_zeros == 0);
    CHECK(stats.lowest_offset == 0x00030000u);
    CHECK(stats.highest_end_offset == 0x00040000u);
    CHECK(stats.exhaustion == PS3_DEMAND_WINDOW_NOT_EXHAUSTED);
    memset(output, 0, sizeof(output));
    CHECK(memory.read(memory.context, first_address, output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(input, output, sizeof(input)));
    /* The rest of the granule is zero-filled by the backing commit. */
    unsigned char granule_bytes[0x141];
    memset(granule_bytes, 0xEE, sizeof(granule_bytes));
    CHECK(memory.read(memory.context, WINDOW_BASE + 0x00030000u, granule_bytes, sizeof(granule_bytes)) ==
        PS3_GUEST_MEMORY_OK);
    CHECK(granule_bytes[0] == 0 && granule_bytes[0xFF] == 0);
    CHECK(granule_bytes[0x100] == 0xC3 && granule_bytes[0x13F] == 0xC3 && granule_bytes[0x140] == 0);
    CHECK(ps3_demand_window_read_stats(&fixture.window).granule_commits == 1);

    /* Case 4: a second access in the same granule commits nothing new. */
    CHECK(memory.write(memory.context, first_address + 4096u, input, 16) == PS3_GUEST_MEMORY_OK);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 1);
    CHECK(stats.triggering_writes == 1);
    CHECK(stats.journal_count == 1);

    /* Case 5: a read-first access commits and returns zeros. */
    memset(output, 0x77, sizeof(output));
    CHECK(memory.read(memory.context, WINDOW_BASE + 0x00050000u, output, 32) == PS3_GUEST_MEMORY_OK);
    for (size_t i = 0; i < 32; ++i) CHECK(output[i] == 0);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 2);
    CHECK(stats.triggering_reads == 1 && stats.triggering_writes == 1 && stats.triggering_zeros == 0);
    CHECK(stats.journal_count == 2 && stats.journal[1] == 0x00050000u);

    /* Case 6: a zero over three granules commits three. */
    CHECK(memory.zero(memory.context, WINDOW_BASE + 0x000A0000u, 3u * WINDOW_GRANULE) == PS3_GUEST_MEMORY_OK);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 5);
    CHECK(stats.committed_bytes == 5u * WINDOW_GRANULE);
    CHECK(stats.triggering_zeros == 1);
    CHECK(stats.journal_count == 5);
    CHECK(stats.journal[2] == 0x000A0000u && stats.journal[3] == 0x000B0000u && stats.journal[4] == 0x000C0000u);
    CHECK(stats.lowest_offset == 0x00030000u);
    CHECK(stats.highest_end_offset == 0x000D0000u);

    /* Case 7: a 16-byte write across a granule boundary commits two. */
    const uint32_t straddle = WINDOW_BASE + 0x00070000u + WINDOW_GRANULE - 8u;
    CHECK(memory.write(memory.context, straddle, input, 16) == PS3_GUEST_MEMORY_OK);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 7);
    CHECK(stats.committed_bytes == 7u * WINDOW_GRANULE);
    CHECK(stats.triggering_writes == 2);
    CHECK(stats.journal_count == 7 && stats.journal[5] == 0x00070000u && stats.journal[6] == 0x00080000u);
    memset(output, 0, sizeof(output));
    CHECK(memory.read(memory.context, straddle, output, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(input, output, 16));

    /* Case 13: the last byte of the window commits the final granule. */
    unsigned char byte = 0x9D;
    const uint32_t last = WINDOW_BASE + WINDOW_SIZE - 1u;
    CHECK(memory.write(memory.context, last, &byte, 1) == PS3_GUEST_MEMORY_OK);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 8);
    CHECK(stats.highest_end_offset == WINDOW_SIZE);
    CHECK(stats.journal[7] == WINDOW_SIZE - WINDOW_GRANULE);
    unsigned char read_back = 0;
    CHECK(memory.read(memory.context, last, &read_back, 1) == PS3_GUEST_MEMORY_OK && read_back == 0x9D);
    heap_window_close(&fixture);
}

/* Case 8: transitions straddling a window edge are partial refusals. */
static void partial_refusals(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, WINDOW_CAP, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_guest_memory memory = fixture.port;
    unsigned char buffer[64];
    memset(buffer, 0x11, sizeof(buffer));
    /* Before the start: both the commit call and the transfer are refused; the
     * outside part would need a heap page this window does not own. */
    CHECK(memory.commit(memory.context, WINDOW_BASE - 8u, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.write(memory.context, WINDOW_BASE - 8u, buffer, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    /* Past the end. */
    CHECK(memory.zero(memory.context, WINDOW_BASE + WINDOW_SIZE - 8u, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.read(memory.context, WINDOW_BASE + WINDOW_SIZE - 8u, buffer, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    /* Entirely before: forwarded and unmapped, never counted as a partial. */
    CHECK(memory.read(memory.context, WINDOW_BASE - 0x1000u, buffer, 16) == PS3_GUEST_MEMORY_UNMAPPED);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    /* The window-start 16-byte transfer intersects (and is contained? no: it
     * starts 8 bytes before `base`) so it is a partial refusal, not a window
     * commit refusal. The explicit commit above is a window commit refusal. */
    CHECK(stats.refused_partial == 3);
    CHECK(stats.refused_window_commits == 1);
    CHECK(stats.granule_commits == 0);
    CHECK(stats.committed_bytes == 0);
    /* A transfer whose length runs past the end of the guest address space
     * intersects the window but cannot be classified: it is forwarded unchanged
     * and the backing rejects it (INVALID_RANGE), with refused_partial untouched. */
    const uint64_t past_end = PS3_GUEST_ADDRESS_BYTES - (WINDOW_BASE + 0x1000u) + 16u;
    CHECK(memory.write(memory.context, WINDOW_BASE + 0x1000u, buffer, (size_t)past_end) ==
        PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.read(memory.context, WINDOW_BASE + 0x1000u, buffer, (size_t)past_end) ==
        PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.zero(memory.context, WINDOW_BASE + 0x1000u, past_end) == PS3_GUEST_MEMORY_INVALID_RANGE);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.refused_partial == 3);
    CHECK(stats.refused_window_commits == 1);
    CHECK(stats.granule_commits == 0);
    CHECK(stats.committed_bytes == 0);
    /* No heap page was ever committed: a fresh read of an edge granule would
     * still report unmapped through the raw heap. */
    CHECK(ps3_heap_guest_space_stats(fixture.heap).committed_pages == 0);
    heap_window_close(&fixture);
}

/* Case 9: commit/decommit intersecting the window are refused; the heap keeps no page. */
static void commit_decommit_refusals(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, WINDOW_CAP, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_guest_memory memory = fixture.port;
    unsigned char buffer[16];
    CHECK(memory.commit(memory.context, WINDOW_BASE, WINDOW_GRANULE) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.commit(memory.context, WINDOW_BASE + 0x80000u, 0x10000u) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.commit(memory.context, WINDOW_BASE - 8u, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.decommit(memory.context, WINDOW_BASE, WINDOW_GRANULE) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.decommit(memory.context, WINDOW_BASE + WINDOW_SIZE - 8u, 16) == PS3_GUEST_MEMORY_INVALID_RANGE);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.refused_window_commits == 5);
    CHECK(stats.granule_commits == 0);
    CHECK(ps3_heap_guest_space_stats(fixture.heap).committed_pages == 0);
    /* Nothing was backed: a forwarded transfer inside the window still commits
     * on demand, but the page was not there before. */
    CHECK(memory.read(memory.context, WINDOW_BASE, buffer, sizeof(buffer)) == PS3_GUEST_MEMORY_OK);
    for (size_t i = 0; i < sizeof(buffer); ++i) CHECK(buffer[i] == 0);
    CHECK(ps3_heap_guest_space_stats(fixture.heap).committed_pages == 16);
    heap_window_close(&fixture);
}

/* Case 10: the cap. */
static void cap_exhaustion(void)
{
    heap_window fixture;
    CHECK(heap_window_open(&fixture, 2u * WINDOW_GRANULE, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_guest_memory memory = fixture.port;
    unsigned char buffer[64];
    memset(buffer, 0x42, sizeof(buffer));
    CHECK(memory.write(memory.context, WINDOW_BASE + 0x0 * WINDOW_GRANULE, buffer, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, WINDOW_BASE + 1 * WINDOW_GRANULE, buffer, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_demand_window_read_stats(&fixture.window).committed_bytes == 2u * WINDOW_GRANULE);
    /* The third granule is refused with a typed cap exhaustion. */
    CHECK(memory.write(memory.context, WINDOW_BASE + 2 * WINDOW_GRANULE, buffer, 16) == PS3_GUEST_MEMORY_NO_MEMORY);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.exhaustion == PS3_DEMAND_WINDOW_CAP);
    CHECK(stats.exhaustion_offset == 2u * WINDOW_GRANULE);
    CHECK(stats.exhaustion_status == PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(stats.granule_commits == 2 && stats.committed_bytes == 2u * WINDOW_GRANULE);
    CHECK(stats.journal_count == 2);
    /* Earlier data stays readable. */
    unsigned char output[16];
    memset(output, 0, sizeof(output));
    CHECK(memory.read(memory.context, WINDOW_BASE, output, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(buffer, output, 16));
    CHECK(memory.read(memory.context, WINDOW_BASE + WINDOW_GRANULE, output, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(buffer, output, 16));
    /* The refused granule stays unmapped in the heap. The heap backs whole
     * 64 KiB blocks, so each 64 KiB granule commit counts 16 pages. */
    CHECK(ps3_heap_guest_space_stats(fixture.heap).committed_pages == 2u * 16u);
    /* With no granule left, an access needing a new one is refused again and
     * committed_bytes is unchanged. */
    const uint64_t bytes_before = ps3_demand_window_read_stats(&fixture.window).committed_bytes;
    CHECK(memory.read(memory.context, WINDOW_BASE + 9 * WINDOW_GRANULE, output, 8) == PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(ps3_demand_window_read_stats(&fixture.window).committed_bytes == bytes_before);
    /* An access fully inside already committed granules still works. */
    CHECK(memory.read(memory.context, WINDOW_BASE + WINDOW_GRANULE, output, 8) == PS3_GUEST_MEMORY_OK);
    heap_window_close(&fixture);

    /* An access needing two new granules with only one slot left is refused and
     * leaves committed_bytes unchanged (case 10, second half). */
    heap_window second;
    CHECK(heap_window_open(&second, 3u * WINDOW_GRANULE, WINDOW_SIZE, WINDOW_GRANULE));
    ps3_guest_memory memory2 = second.port;
    CHECK(memory2.write(memory2.context, WINDOW_BASE, buffer, 8) == PS3_GUEST_MEMORY_OK);
    CHECK(memory2.write(memory2.context, WINDOW_BASE + WINDOW_GRANULE, buffer, 8) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_demand_window_read_stats(&second.window).committed_bytes == 2u * WINDOW_GRANULE);
    /* 3 granules needed, 2 already committed, only 1 slot free -> refused. */
    CHECK(memory2.zero(memory2.context, WINDOW_BASE + 2 * WINDOW_GRANULE, 2u * WINDOW_GRANULE) ==
        PS3_GUEST_MEMORY_NO_MEMORY);
    stats = ps3_demand_window_read_stats(&second.window);
    CHECK(stats.committed_bytes == 2u * WINDOW_GRANULE);
    CHECK(stats.granule_commits == 2);
    CHECK(stats.exhaustion == PS3_DEMAND_WINDOW_CAP && stats.exhaustion_offset == 2u * WINDOW_GRANULE);
    CHECK(ps3_heap_guest_space_stats(second.heap).committed_pages == 2u * 16u);
    heap_window_close(&second);
}

/* Case 11: a backing failure rolls back the granules this call committed. */
static void backing_failure(void)
{
    fake_backing fake;
    fake_init(&fake, WINDOW_BASE, WINDOW_GRANULE);
    const ps3_guest_memory backing = fake_port(&fake);
    counting_guard window_guard = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    ps3_demand_window window;
    memset(&window, 0, sizeof(window));
    CHECK(&window_guard.mutex != &fake.guard.mutex);
    CHECK(ps3_demand_window_init(&window, backing, counting_port(&window_guard),
        WINDOW_BASE, WINDOW_SIZE, WINDOW_GRANULE, WINDOW_CAP) == PS3_GUEST_MEMORY_OK);
    ps3_guest_memory memory = ps3_demand_window_port(&window);
    unsigned char buffer[16];
    memset(buffer, 0x00, sizeof(buffer));
    fake.fail_commit_at = 2;
    fake.fail_status = PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    /* The fake accepts zero transfers, so the retry below proves the rollback:
     * a successful forward would otherwise be the failure under test. */
    CHECK(memory.zero(memory.context, WINDOW_BASE + 4 * WINDOW_GRANULE, 2u * WINDOW_GRANULE) ==
        PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    CHECK(fake.commit_calls == 2);
    CHECK(fake.decommit_calls == 1);
    CHECK(fake.decommit_offsets[0] == 4 * WINDOW_GRANULE);
    CHECK(!fake_committed(&fake, WINDOW_BASE + 4 * WINDOW_GRANULE));
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&window);
    CHECK(stats.exhaustion == PS3_DEMAND_WINDOW_BACKING);
    CHECK(stats.exhaustion_offset == 5 * WINDOW_GRANULE);
    CHECK(stats.exhaustion_status == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    CHECK(stats.granule_commits == 0 && stats.committed_bytes == 0 && stats.journal_count == 0);
    CHECK(stats.lowest_offset == 0 && stats.highest_end_offset == 0);
    CHECK(stats.triggering_zeros == 0);
    /* A later access retries and succeeds. */
    const unsigned failed_commit_calls = fake.commit_calls;
    fake.fail_commit_at = 0;
    CHECK(memory.zero(memory.context, WINDOW_BASE + 4 * WINDOW_GRANULE, 2u * WINDOW_GRANULE) == PS3_GUEST_MEMORY_OK);
    stats = ps3_demand_window_read_stats(&window);
    CHECK(stats.granule_commits == 2 && stats.committed_bytes == 2u * WINDOW_GRANULE);
    CHECK(stats.triggering_zeros == 1);
    CHECK(stats.journal_count == 2 && stats.journal[0] == 4 * WINDOW_GRANULE);
    /* The exhaustion record is "first exhaustion only": unchanged. */
    CHECK(stats.exhaustion == PS3_DEMAND_WINDOW_BACKING);
    CHECK(stats.exhaustion_offset == 5 * WINDOW_GRANULE);
    CHECK(fake_committed(&fake, WINDOW_BASE + 4 * WINDOW_GRANULE));
    CHECK(fake_committed(&fake, WINDOW_BASE + 5 * WINDOW_GRANULE));
    /* The retry re-issues the same two granule commits. */
    CHECK(fake.commit_offsets[failed_commit_calls] == 4 * WINDOW_GRANULE);
    CHECK(fake.commit_offsets[failed_commit_calls + 1] == 5 * WINDOW_GRANULE);
    CHECK(fake.commit_lengths[failed_commit_calls] == WINDOW_GRANULE);
    CHECK(fake.commit_lengths[failed_commit_calls + 1] == WINDOW_GRANULE);
    CHECK_GUARD(window_guard);
    CHECK_GUARD(fake.guard);
    pthread_mutex_destroy(&window_guard.mutex);
    fake_destroy(&fake);
}

/* Case 12: the journal stops at 64 while granule_commits keeps counting. */
static void journal_bound(void)
{
    /* 200 granules of 4 KiB (the minimum granule) inside a 0xC8000 window. */
    const uint32_t granule = 4096u;
    const uint32_t granules = 200u;
    heap_window fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.heap_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    fixture.window_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_allocator allocator = {&fixture.allocator, fixture_allocate, fixture_deallocate};
    CHECK(ps3_heap_guest_space_create(&fixture.heap, allocator, counting_port(&fixture.heap_guard), 131072) ==
        PS3_GUEST_MEMORY_OK);
    CHECK(ps3_demand_window_init(&fixture.window, ps3_heap_guest_space_port(fixture.heap),
        counting_port(&fixture.window_guard), WINDOW_BASE, granules * granule, granule,
        (uint64_t)granules * granule) == PS3_GUEST_MEMORY_OK);
    ps3_guest_memory memory = ps3_demand_window_port(&fixture.window);
    CHECK(memory.zero(memory.context, WINDOW_BASE, (uint64_t)granules * granule) == PS3_GUEST_MEMORY_OK);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == granules);
    CHECK(stats.committed_bytes == (uint64_t)granules * granule);
    CHECK(stats.journal_count == PS3_DEMAND_WINDOW_JOURNAL);
    for (uint32_t index = 0; index < PS3_DEMAND_WINDOW_JOURNAL; ++index)
        CHECK(stats.journal[index] == index * granule);
    CHECK(stats.triggering_zeros == 1);
    CHECK(stats.highest_end_offset == granules * granule);
    CHECK(ps3_heap_guest_space_destroy(&fixture.heap) == PS3_GUEST_MEMORY_OK);
    CHECK_GUARD(fixture.window_guard);
    CHECK_GUARD(fixture.heap_guard);
    pthread_mutex_destroy(&fixture.window_guard.mutex);
    pthread_mutex_destroy(&fixture.heap_guard.mutex);
}

/* Case 14: the production constants over the real heap quota. */
static void production_constants(void)
{
    const uint32_t base = 0xC0000000u, size = 0x0F900000u, granule = 0x10000u;
    const uint64_t cap = 0x08000000ull;
    heap_window fixture;
    memset(&fixture, 0, sizeof(fixture));
    fixture.heap_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    fixture.window_guard = (counting_guard){.mutex = PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_allocator allocator = {&fixture.allocator, fixture_allocate, fixture_deallocate};
    CHECK(ps3_heap_guest_space_create(&fixture.heap, allocator, counting_port(&fixture.heap_guard), 131072) ==
        PS3_GUEST_MEMORY_OK);
    CHECK(ps3_demand_window_init(&fixture.window, ps3_heap_guest_space_port(fixture.heap),
        counting_port(&fixture.window_guard), base, size, granule, cap) == PS3_GUEST_MEMORY_OK);
    ps3_guest_memory memory = ps3_demand_window_port(&fixture.window);
    const uint32_t first_touch = 0xC2644000u;
    unsigned char word[4] = {0xDE, 0xAD, 0xBE, 0xEF}, read_back[4] = {0};
    CHECK(memory.write(memory.context, first_touch, word, sizeof(word)) == PS3_GUEST_MEMORY_OK);
    ps3_demand_window_stats stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 1);
    CHECK(stats.journal_count == 1);
    CHECK(stats.journal[0] == 0x02640000u);
    CHECK(stats.committed_bytes == granule);
    CHECK(memory.read(memory.context, first_touch, read_back, sizeof(read_back)) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(word, read_back, sizeof(word)));
    /* A write at the last aligned word of the window works. */
    const uint32_t last_word = 0xCF8FFFFCu;
    CHECK(memory.write(memory.context, last_word, word, sizeof(word)) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, last_word, read_back, sizeof(read_back)) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(word, read_back, sizeof(word)));
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.granule_commits == 2);
    CHECK(stats.journal[1] == size - granule);
    /* An 8-byte write there is a partial refusal. */
    unsigned char eight[8] = {0};
    CHECK(memory.write(memory.context, last_word, eight, sizeof(eight)) == PS3_GUEST_MEMORY_INVALID_RANGE);
    stats = ps3_demand_window_read_stats(&fixture.window);
    CHECK(stats.refused_partial == 1);
    CHECK(stats.granule_commits == 2);
    CHECK(stats.highest_end_offset == size);
    /* The window end is exactly the end of the gcm_init local window. */
    CHECK((uint64_t)base + size == 0xCF900000ull);
    CHECK(ps3_heap_guest_space_destroy(&fixture.heap) == PS3_GUEST_MEMORY_OK);
    CHECK_GUARD(fixture.window_guard);
    CHECK_GUARD(fixture.heap_guard);
    pthread_mutex_destroy(&fixture.window_guard.mutex);
    pthread_mutex_destroy(&fixture.heap_guard.mutex);
}

int main(void)
{
    init_refusals();
    outside_window();
    commit_paths();
    partial_refusals();
    commit_decommit_refusals();
    cap_exhaustion();
    backing_failure();
    journal_bound();
    production_constants();
    printf("DEMAND_WINDOW_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
