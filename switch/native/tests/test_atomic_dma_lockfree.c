/* Decision 135: plain SPU DMA without the domain guard (ps3_atomic_dma_read / ps3_atomic_dma_write, atomic_domain.h).
 *
 *   equivalence     single thread: random ranges over a mix of touched and untouched lines; the direct path must read exactly what the guarded read
 *                   reads and write exactly what the guarded write writes, account every byte once, and call the guarded-write hook for every byte
 *                   it writes directly.
 *   reservations    a DMA PUT to any byte of a line with a 128-byte reservation (or a 32-bit one) makes that reservation's store-conditional fail and
 *                   fires the loss hook; a PUT to another line does not; a GET never disturbs a reservation.
 *   transition      threads, fresh (never touched) lines each round: an atomic thread increments a counter with GETLLAR/PUTLLC and writes back the
 *                   rest of the line it read; a DMA writer PUTs an increasing value to the second half of the line and GETs it back; a DMA reader
 *                   GETs the line. Every GET must see a line that is not torn (all scribble words equal, the counter never goes back) and the
 *                   writer must never read back a value below the one it wrote (a store-conditional that succeeded over its PUT would restore an
 *                   older one). At the end: counter = increments, scribble = the last PUT. The first GETLLAR of each round happens while the DMA
 *                   threads already copy that line directly: the redo after the second test is what keeps it exact.
 * The guest memory is a static buffer whose every byte is accessed with relaxed atomics by the backing, so ThreadSanitizer builds check the
 * protocol (owner records, maps, counters) rather than the plain-data races it permits by design. */
#define _POSIX_C_SOURCE 200809L
#include "atomic_domain.h"
#include "../adapters/guard_pthread.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SPACE = 4u << 20, LINE = 128, ROUNDS_DEFAULT = 600, INCREMENTS = 16, PUTS = 16, READS = 24 };
static unsigned char g_space[SPACE] __attribute__((aligned(128)));
static unsigned g_failures;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++g_failures; } } while (0)

static void copy_relaxed(unsigned char *to, const unsigned char *from, size_t n) {
    for (size_t i = 0; i < n; ++i) __atomic_store_n(to + i, __atomic_load_n(from + i, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
}
static int in_space(uint32_t a, uint64_t n) { return (uint64_t)a + n <= SPACE; }
static ps3_guest_memory_status b_read(void *c, uint32_t a, void *p, size_t n) { (void)c; if (!in_space(a, n)) return PS3_GUEST_MEMORY_UNMAPPED; copy_relaxed(p, g_space + a, n); return PS3_GUEST_MEMORY_OK; }
static ps3_guest_memory_status b_write(void *c, uint32_t a, const void *p, size_t n) { (void)c; if (!in_space(a, n)) return PS3_GUEST_MEMORY_UNMAPPED; copy_relaxed(g_space + a, p, n); return PS3_GUEST_MEMORY_OK; }
static ps3_guest_memory_status b_zero(void *c, uint32_t a, uint64_t n) {
    (void)c; if (!in_space(a, n)) return PS3_GUEST_MEMORY_UNMAPPED;
    for (uint64_t i = 0; i < n; ++i) __atomic_store_n(g_space + a + i, 0, __ATOMIC_RELAXED);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status b_commit(void *c, uint32_t a, uint64_t n) { (void)c; return in_space(a, n) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_NO_MEMORY; }
static const void *direct(void *c, uint32_t a, uint64_t n) { (void)c; return in_space(a, n) ? g_space + a : NULL; }

static ps3_pthread_guard g_guard;
static ps3_atomic_domain g_domain;
static ps3_atomic_owner_record g_owners[8];
static uint64_t g_hook_bytes, g_loss_hooks;
static void write_hook(uint64_t a, uint64_t n) { (void)a; __atomic_fetch_add(&g_hook_bytes, n, __ATOMIC_RELAXED); }
static void loss_hook(void) { __atomic_fetch_add(&g_loss_hooks, 1u, __ATOMIC_RELAXED); }
static uint64_t xorshift(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
static ps3_atomic_owner acquire(void) { ps3_atomic_owner o; CHECK(ps3_atomic_owner_acquire(&g_domain, &o) == PS3_GUEST_MEMORY_OK, "acquire"); return o; }
static int line_touched(uint64_t a) { const uint32_t i = ps3_atomic_sticky_index(a); return (int)((g_ps3_atomic_touched[i >> 6] >> (i & 63u)) & 1u); }

/* ------------------------------------------------------------------------------------------------ equivalence */
static void test_equivalence(uint64_t seed) {
    ps3_atomic_owner o = acquire();
    uint64_t s = seed | 1u;
    const uint32_t base = 0x100000u, size = 0x40000u;
    for (uint32_t a = base; a < base + size; a += 8) { uint64_t v = xorshift(&s); b_write(NULL, a, &v, 8); }
    for (unsigned k = 0; k < 160; ++k) {   /* a sprinkle of touched lines: compare-exchange on one word of each */
        const uint32_t line = base + (uint32_t)(xorshift(&s) % (size / LINE)) * LINE;
        ps3_atomic_compare_result r;
        (void)ps3_atomic_compare_exchange32(&g_domain, line, 0xDEADBEEFu, 0, &r);
    }
    static unsigned char want[0x4000], got[0x4000], data[0x4000];
    unsigned direct_runs = 0, guarded_runs = 0;
    for (unsigned trial = 0; trial < 4000; ++trial) {
        const uint32_t n = 1u + (uint32_t)(xorshift(&s) % 0x4000u), a = base + (uint32_t)(xorshift(&s) % (size - n));
        uint64_t counts[PS3_ATOMIC_DMA_COUNTS];
        ps3_atomic_dma_take_counts(counts);
        CHECK(ps3_atomic_owner_read(&g_domain, o, a, want, n) == PS3_GUEST_MEMORY_OK, "guarded read");
        CHECK(ps3_atomic_dma_read(&g_domain, o, a, got, n) == PS3_GUEST_MEMORY_OK, "dma read");
        CHECK(!memcmp(want, got, n), "GET 0x%X+%u differs from the guarded read", a, n);
        ps3_atomic_dma_take_counts(counts);
        CHECK(counts[PS3_ATOMIC_DMA_DIRECT_GET] + counts[PS3_ATOMIC_DMA_GUARDED_GET] == n, "GET bytes %llu+%llu != %u",
              (unsigned long long)counts[PS3_ATOMIC_DMA_DIRECT_GET], (unsigned long long)counts[PS3_ATOMIC_DMA_GUARDED_GET], n);
        direct_runs += counts[PS3_ATOMIC_DMA_DIRECT_GET] != 0; guarded_runs += counts[PS3_ATOMIC_DMA_GUARDED_GET] != 0;
        for (uint32_t i = 0; i < n; ++i) data[i] = (unsigned char)xorshift(&s);
        const uint64_t hooks_before = g_hook_bytes;
        CHECK(ps3_atomic_dma_write(&g_domain, o, a, data, n) == PS3_GUEST_MEMORY_OK, "dma write");
        CHECK(g_hook_bytes - hooks_before == n, "write hook saw %llu of %u bytes", (unsigned long long)(g_hook_bytes - hooks_before), n);
        CHECK(ps3_atomic_owner_read(&g_domain, o, a, got, n) == PS3_GUEST_MEMORY_OK && !memcmp(got, data, n), "PUT 0x%X+%u not stored", a, n);
        ps3_atomic_dma_take_counts(counts);
        CHECK(counts[PS3_ATOMIC_DMA_DIRECT_PUT] + counts[PS3_ATOMIC_DMA_GUARDED_PUT] == n, "PUT bytes");
        /* the guarded part is exactly the touched lines of the range */
        uint64_t touched_bytes = 0;
        for (uint64_t line = a & ~(uint64_t)(LINE - 1); line < (uint64_t)a + n; line += LINE)
            if (line_touched(line)) { const uint64_t lo = line > a ? line : a, hi = line + LINE < (uint64_t)a + n ? line + LINE : (uint64_t)a + n; touched_bytes += hi - lo; }
        CHECK(counts[PS3_ATOMIC_DMA_GUARDED_PUT] == touched_bytes, "guarded PUT bytes %llu, touched bytes %llu",
              (unsigned long long)counts[PS3_ATOMIC_DMA_GUARDED_PUT], (unsigned long long)touched_bytes);
    }
    /* out of range / stale owner: the guarded path's answers */
    CHECK(ps3_atomic_dma_read(&g_domain, o, SPACE - 16, got, 64) == PS3_GUEST_MEMORY_UNMAPPED, "unmapped GET");
    ps3_atomic_owner stale = o; stale.generation += 7;
    CHECK(ps3_atomic_dma_read(&g_domain, stale, base, got, 64) == PS3_GUEST_MEMORY_INVALID_RANGE, "stale owner GET");
    CHECK(ps3_atomic_dma_write(&g_domain, stale, base, got, 64) == PS3_GUEST_MEMORY_INVALID_RANGE, "stale owner PUT");
    CHECK(direct_runs > 1000 && guarded_runs > 100, "both paths exercised (direct %u guarded %u)", direct_runs, guarded_runs);
    printf("DMA_EQUIVALENCE trials=4000 with_direct=%u with_guarded=%u touched_lines=%llu\n", direct_runs, guarded_runs, (unsigned long long)g_ps3_atomic_touched_lines);
    CHECK(ps3_atomic_owner_release(&g_domain, &o) == PS3_GUEST_MEMORY_OK, "release");
}

/* ------------------------------------------------------------------------------------------------ reservations */
static void test_reservations(void) {
    ps3_atomic_owner a = acquire(), b = acquire();
    const uint32_t line = 0x200000u, other = 0x200400u;
    unsigned char buf[LINE], got[LINE];
    unsigned stored = 0;
    for (unsigned offset = 0; offset < LINE; offset += 16) {
        unsigned st = 0;
        CHECK(ps3_atomic_load_reserve128(&g_domain, a, line, buf) == PS3_GUEST_MEMORY_OK, "getllar");
        const uint64_t loss_before = g_loss_hooks;
        memset(got, (int)offset, 16);
        CHECK(ps3_atomic_dma_write(&g_domain, b, line + offset, got, 16) == PS3_GUEST_MEMORY_OK, "put");
        CHECK(g_loss_hooks == loss_before + 1u, "PUT at +%u: loss hook not fired", offset);
        int ok = 1;
        CHECK(ps3_atomic_store_conditional128(&g_domain, a, line, buf, &ok) == PS3_GUEST_MEMORY_OK && !ok, "PUT at +%u: putllc succeeded over it", offset);
        (void)st;
        /* a PUT to another line and a GET of this one leave the reservation alone */
        CHECK(ps3_atomic_load_reserve128(&g_domain, a, line, buf) == PS3_GUEST_MEMORY_OK, "getllar");
        CHECK(ps3_atomic_dma_write(&g_domain, b, other, got, 64) == PS3_GUEST_MEMORY_OK, "put other");
        CHECK(ps3_atomic_dma_read(&g_domain, b, line, got, LINE) == PS3_GUEST_MEMORY_OK, "get");
        buf[0] ^= 1u;
        CHECK(ps3_atomic_store_conditional128(&g_domain, a, line, buf, &ok) == PS3_GUEST_MEMORY_OK && ok, "putllc after unrelated PUT/GET failed");
        stored += ok;
    }
    {   /* a 32-bit reservation (PPU lwarx/stwcx) */
        uint32_t value = 0; int ok = 1;
        CHECK(ps3_atomic_load_reserve32(&g_domain, a, line + 0x200u, &value) == PS3_GUEST_MEMORY_OK, "lwarx");
        memset(got, 0x5A, 8);
        CHECK(ps3_atomic_dma_write(&g_domain, b, line + 0x208u, got, 8) == PS3_GUEST_MEMORY_OK, "put");
        CHECK(ps3_atomic_store_conditional32(&g_domain, a, line + 0x200u, value + 1u, &ok) == PS3_GUEST_MEMORY_OK && !ok, "stwcx succeeded over a PUT to its line");
    }
    printf("DMA_RESERVATIONS put_offsets=8 unrelated_ok=%u\n", stored);
    CHECK(ps3_atomic_owner_release(&g_domain, &a) == PS3_GUEST_MEMORY_OK && ps3_atomic_owner_release(&g_domain, &b) == PS3_GUEST_MEMORY_OK, "release");
}

/* ------------------------------------------------------------------------------------------------ transition stress */
typedef struct { pthread_barrier_t *start, *end; unsigned rounds; uint32_t *lines; ps3_atomic_owner owner; unsigned role; uint64_t redo, lost; } Worker;
static uint64_t word_at(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void *worker(void *arg) {
    Worker *w = arg;
    unsigned char line[LINE];
    for (unsigned r = 0; r < w->rounds; ++r) {
        const uint32_t at = w->lines[r];
        pthread_barrier_wait(w->start);
        if (w->role == 0) {                      /* atomic: INCREMENTS successful GETLLAR/PUTLLC on word 0, the rest of the line written back as read */
            for (unsigned k = 0; k < INCREMENTS;) {
                int ok = 0;
                CHECK(ps3_atomic_load_reserve128(&g_domain, w->owner, at, line) == PS3_GUEST_MEMORY_OK, "getllar");
                uint64_t counter = word_at(line) + 1u; memcpy(line, &counter, 8);
                CHECK(ps3_atomic_store_conditional128(&g_domain, w->owner, at, line, &ok) == PS3_GUEST_MEMORY_OK, "putllc");
                if (ok) ++k; else ++w->lost;
            }
        } else if (w->role == 1) {               /* DMA writer: the second half of the line, an increasing value, read back */
            for (uint64_t v = 1; v <= PUTS; ++v) {
                unsigned char half[64];
                for (unsigned i = 0; i < 64; i += 8) memcpy(half + i, &v, 8);
                CHECK(ps3_atomic_dma_write(&g_domain, w->owner, at + 64u, half, 64) == PS3_GUEST_MEMORY_OK, "put");
                CHECK(ps3_atomic_dma_read(&g_domain, w->owner, at, line, LINE) == PS3_GUEST_MEMORY_OK, "get");
                for (unsigned i = 64; i < LINE; i += 8) CHECK(word_at(line + i) == word_at(line + 64), "round %u: torn scribble", r);
                CHECK(word_at(line + 64) >= v, "round %u: wrote %llu, read back %llu (a store-conditional succeeded over the PUT)", r, (unsigned long long)v,
                      (unsigned long long)word_at(line + 64));
            }
        } else {                                 /* DMA reader */
            uint64_t last = 0;
            for (unsigned k = 0; k < READS; ++k) {
                CHECK(ps3_atomic_dma_read(&g_domain, w->owner, at, line, LINE) == PS3_GUEST_MEMORY_OK, "get");
                for (unsigned i = 64; i < LINE; i += 8) CHECK(word_at(line + i) == word_at(line + 64), "round %u: torn scribble", r);
                CHECK(word_at(line) >= last, "round %u: counter went back", r);
                last = word_at(line);
            }
        }
        uint64_t counts[PS3_ATOMIC_DMA_COUNTS];
        ps3_atomic_dma_take_counts(counts);
        w->redo += counts[PS3_ATOMIC_DMA_REDO_GET] + counts[PS3_ATOMIC_DMA_REDO_PUT];
        pthread_barrier_wait(w->end);
    }
    return NULL;
}
static void test_transition(unsigned rounds) {
    uint32_t *lines = calloc(rounds, sizeof *lines);
    unsigned found = 0;
    for (uint32_t at = 0x300000u; found < rounds && at + LINE <= SPACE; at += LINE)
        if (!line_touched(at)) lines[found++] = at;     /* fresh lines (no touched bit, not even by a hash collision) */
    CHECK(found == rounds, "fresh lines %u of %u", found, rounds);
    unsigned char zero[LINE] = { 0 };
    for (unsigned r = 0; r < found; ++r) b_write(NULL, lines[r], zero, LINE);
    pthread_barrier_t start, end;
    enum { WORKERS = 4 };                 /* atomic, writer, two readers */
    pthread_barrier_init(&start, NULL, WORKERS + 1); pthread_barrier_init(&end, NULL, WORKERS + 1);
    Worker w[WORKERS]; pthread_t t[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        w[i] = (Worker){ &start, &end, found, lines, acquire(), i < 2 ? i : 2u, 0, 0 };
        pthread_create(&t[i], NULL, worker, &w[i]);
    }
    uint64_t torn_free = 0;
    for (unsigned r = 0; r < found; ++r) {
        pthread_barrier_wait(&start);
        pthread_barrier_wait(&end);
        unsigned char line[LINE];
        b_read(NULL, lines[r], line, LINE);
        CHECK(word_at(line) == INCREMENTS, "round %u: counter %llu, %u increments", r, (unsigned long long)word_at(line), INCREMENTS);
        CHECK(word_at(line + 64) == PUTS && word_at(line + 120) == PUTS, "round %u: scribble %llu, last PUT %u", r, (unsigned long long)word_at(line + 64), PUTS);
        torn_free += word_at(line) == INCREMENTS;
    }
    uint64_t redo = 0, lost = 0;
    for (unsigned i = 0; i < WORKERS; ++i) { pthread_join(t[i], NULL); redo += w[i].redo; lost += w[i].lost; CHECK(ps3_atomic_owner_release(&g_domain, &w[i].owner) == PS3_GUEST_MEMORY_OK, "release"); }
    pthread_barrier_destroy(&start); pthread_barrier_destroy(&end);
    printf("DMA_TRANSITION rounds=%u exact=%llu redone_runs=%llu putllc_failures=%llu\n", found, (unsigned long long)torn_free, (unsigned long long)redo, (unsigned long long)lost);
    free(lines);
}

int main(int argc, char **argv) {
    const unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : ROUNDS_DEFAULT;
    if (ps3_pthread_guard_init(&g_guard)) return 2;
    const ps3_guest_memory memory = { NULL, b_commit, b_commit, b_read, b_write, b_zero };
    if (ps3_atomic_domain_init(&g_domain, memory, ps3_pthread_guard_port(&g_guard), g_owners, 8, NULL, 0) != PS3_GUEST_MEMORY_OK) return 2;
    ps3_atomic_domain_set_direct(&g_domain, direct, NULL);
    ps3_atomic_guarded_write_hook = write_hook;
    ps3_atomic_reservation_loss_hook = loss_hook;
    test_equivalence(0x1234567u);
    test_reservations();
    test_transition(rounds);
    printf("%s failures=%u\n", g_failures ? "ATOMIC_DMA_LOCKFREE_FAIL" : "ATOMIC_DMA_LOCKFREE_PASS", g_failures);
    ps3_pthread_guard_destroy(&g_guard);
    return g_failures ? 1 : 0;
}
