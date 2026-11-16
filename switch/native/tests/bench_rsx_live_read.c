/* Host micro-benchmark of the RSX consumer's guest-memory path (renderer CPU work, 2026-11-15).
 *
 * Question it answers: what does one FIFO word cost the consumer (rsx_live_pump) when it is read through the console's
 * real memory chain (atomic domain guard -> timebase audit wrapper -> demand window -> flat space mutex), against a direct
 * read of the flat guest window? The chain here is the REAL source/ps3recomp/runtime/memory/atomic_domain.c and
 * native/adapters/demand_window.c over a host copy of guest_space_flat_horizon.c's transfer() (one mutex, commit bitmap,
 * memmove), a barging guard with the same algorithm as barging_lock_horizon.h (futex instead of svcWaitForAddress), and
 * the same 71 owners / 2048 pins the console composition configures. Build flags match the console's: no debug line.
 *
 * Input: a method capture (methods.bin: u32 subchannel, method, data triples, a flip = 0xFFFFFFFF) is packed into NV40
 * packets (runs of consecutive increasing methods) in the IO mapping; the consumer is pumped over it to idle.
 *
 *   build: gcc -std=gnu11 -O2 -DPS3_ATOMIC_DEBUG_LINE=0 native/tests/bench_rsx_live_read.c native/domain/rsx_live_fifo.c \\
 *              native/adapters/demand_window.c source/ps3recomp/runtime/memory/atomic_domain.c -I native -lpthread -o bench
 *   run:   bench <methods.bin> [background threads] [repeats]
 *   Variants per background-thread count: "domain" = read32 through the chain per word and `get` after every packet (the old consumer);
 *   "direct" = the pointer port and `get` every GET_BATCH words (the new default). Both run the same rsx_live_pump.
 *
 * Output lines: BENCH <variant> bg=<n> words=<w> packets=<p> methods=<m> ns_total=<..> ns_per_word=<..> ... Host numbers are
 * x86; the console's A57 at 1.02 GHz is several times slower per operation, and its contention is worse than the few
 * background threads here, so these are floors.
 */
#define _GNU_SOURCE
#include "../domain/rsx_live_fifo.h"
#include "../adapters/demand_window.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { IO_EA = 0x40700000u, IO_SIZE = 0x08500000u, LOCAL_BASE = 0xC0000000u, LOCAL_SIZE = 0x0F900000u,
       CONTROL = 0x0E101040u, LABELS = 0x0E110000u, LABEL_SIZE = 0x10000u, OWNERS = 71, PINS = 2048 };

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---- barging guard: barging_lock_horizon.h with futex in place of the address arbiter ---- */
static void bl_lock(void *ctx)
{
    uint32_t *word = ctx, expected = 0;
    if (__atomic_compare_exchange_n(word, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    for (unsigned round = 0; round < 300u; ++round) {
        uint32_t seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (!(seen & 1u) && __atomic_compare_exchange_n(word, &seen, seen | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        __builtin_ia32_pause();
    }
    for (;;) {
        uint32_t seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (!(seen & 1u)) {
            if (__atomic_compare_exchange_n(word, &seen, seen | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        (void)__atomic_fetch_add(word, 2u, __ATOMIC_ACQ_REL);
        seen = __atomic_load_n(word, __ATOMIC_RELAXED);
        if (seen & 1u) {
            struct timespec timeout = {0, 1000000};
            (void)syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, seen, &timeout, NULL, 0);
        } else if (__atomic_compare_exchange_n(word, &seen, (seen - 2u) | 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        (void)__atomic_fetch_sub(word, 2u, __ATOMIC_ACQ_REL);
    }
}
static void bl_unlock(void *ctx)
{
    uint32_t *word = ctx;
    if (__atomic_fetch_and(word, ~1u, __ATOMIC_RELEASE) > 1u) (void)syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

/* ---- host flat space: guest_space_flat_horizon.c's port (one mutex, commit bitmap, memmove) ---- */
typedef struct { uint8_t *base; unsigned char *committed; pthread_mutex_t mutex; } hflat;
static hflat g_flat;
static int is_committed(const hflat *s, uint64_t page) { return (s->committed[page / 8] >> (page % 8)) & 1; }
static int fully_committed(const hflat *s, uint32_t address, uint64_t length)
{
    if (!length) return 1;
    for (uint64_t page = address / 4096u, last = ((uint64_t)address + length - 1) / 4096u; page <= last; ++page)
        if (!is_committed(s, page)) return 0;
    return 1;
}
static ps3_guest_memory_status hf_commit(void *c, uint32_t a, uint64_t n)
{
    hflat *s = c; pthread_mutex_lock(&s->mutex);
    for (uint64_t p = a / 4096u; p < ((uint64_t)a + n) / 4096u; ++p) s->committed[p / 8] |= (unsigned char)(1u << (p % 8));
    pthread_mutex_unlock(&s->mutex); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status hf_decommit(void *c, uint32_t a, uint64_t n) { (void)c; (void)a; (void)n; return PS3_GUEST_MEMORY_OK; }
static ps3_guest_memory_status hf_transfer(void *c, uint32_t a, void *buf, uint64_t n, int op)
{
    hflat *s = c;
    pthread_mutex_lock(&s->mutex);
    if (!fully_committed(s, a, n)) { pthread_mutex_unlock(&s->mutex); return PS3_GUEST_MEMORY_UNMAPPED; }
    if (op == 0) memmove(buf, s->base + a, n); else if (op == 1) memmove(s->base + a, buf, n); else memset(s->base + a, 0, n);
    pthread_mutex_unlock(&s->mutex); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status hf_read(void *c, uint32_t a, void *b, size_t n) { return hf_transfer(c, a, b, n, 0); }
static ps3_guest_memory_status hf_write(void *c, uint32_t a, const void *b, size_t n) { return hf_transfer(c, a, (void *)b, n, 1); }
static ps3_guest_memory_status hf_zero(void *c, uint32_t a, uint64_t n) { return hf_transfer(c, a, NULL, n, 2); }

/* ---- the timebase audit wrapper of test_startup_indirect.cpp (one atomic add per call) ---- */
typedef struct { ps3_guest_memory backing; _Atomic uint64_t calls; } audit_t;
static audit_t g_audit;
static ps3_guest_memory_status au_commit(void *c, uint32_t a, uint64_t n) { audit_t *x = c; ++x->calls; return x->backing.commit(x->backing.context, a, n); }
static ps3_guest_memory_status au_decommit(void *c, uint32_t a, uint64_t n) { audit_t *x = c; ++x->calls; return x->backing.decommit(x->backing.context, a, n); }
static ps3_guest_memory_status au_read(void *c, uint32_t a, void *b, size_t n) { audit_t *x = c; ++x->calls; return x->backing.read(x->backing.context, a, b, n); }
static ps3_guest_memory_status au_write(void *c, uint32_t a, const void *b, size_t n) { audit_t *x = c; ++x->calls; return x->backing.write(x->backing.context, a, b, n); }
static ps3_guest_memory_status au_zero(void *c, uint32_t a, uint64_t n) { audit_t *x = c; ++x->calls; return x->backing.zero(x->backing.context, a, n); }

#ifndef GET_BATCH
#define GET_BATCH 256u   /* words between control-register `get` updates in the batched variant */
#endif
static uint32_t g_domain_word, g_window_word;
static ps3_demand_window g_window;
static ps3_atomic_domain g_domain;
static ps3_atomic_owner_record g_owners[OWNERS];
static ps3_atomic_pin_record g_pins[PINS];
static ps3_guest_memory g_coherent;

static void build_chain(void)
{
    g_flat.base = mmap(NULL, UINT64_C(0x100000000), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_flat.base == MAP_FAILED) { perror("mmap"); exit(2); }
    g_flat.committed = calloc(1, 0x100000000ull / 4096 / 8);
    pthread_mutex_init(&g_flat.mutex, NULL);
    hf_commit(&g_flat, IO_EA, IO_SIZE);
    hf_commit(&g_flat, 0x0E100000u, 0x20000u);       /* GCM init window: control register and labels */
    hf_commit(&g_flat, 0x00010000u, 0x01000000u);    /* main image */
    const ps3_guest_memory flat = {&g_flat, hf_commit, hf_decommit, hf_read, hf_write, hf_zero};
    if (ps3_demand_window_init(&g_window, flat, (ps3_instance_guard){&g_window_word, bl_lock, bl_unlock},
                               LOCAL_BASE, LOCAL_SIZE, 0x10000u, LOCAL_SIZE) != PS3_GUEST_MEMORY_OK) { puts("window init"); exit(2); }
    const ps3_guest_memory window = ps3_demand_window_port(&g_window);
    g_audit.backing = window;
    const ps3_guest_memory audited = {&g_audit, au_commit, au_decommit, au_read, au_write, au_zero};
    if (ps3_atomic_domain_init(&g_domain, audited, (ps3_instance_guard){&g_domain_word, bl_lock, bl_unlock},
                               g_owners, OWNERS, g_pins, PINS) != PS3_GUEST_MEMORY_OK) { puts("domain init"); exit(2); }
    g_coherent = ps3_atomic_domain_memory(&g_domain);
}

/* ---- the bridge's ports, as in startup_execution.cpp (rsx_live_bridge::read32 / write32) ---- */
static unsigned long g_method_calls;
static int port_read32(void *user, uint32_t address, uint32_t *value)
{
    (void)user; unsigned char b[4];
    if (g_coherent.read(g_coherent.context, address, b, 4) != PS3_GUEST_MEMORY_OK) return 1;
    *value = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return 0;
}
static int port_write32(void *user, uint32_t address, uint32_t value)
{
    (void)user;
    const unsigned char b[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8), (unsigned char)value};
    return g_coherent.write(g_coherent.context, address, b, 4) == PS3_GUEST_MEMORY_OK ? 0 : 1;
}
static void port_method(void *user, uint32_t sub, uint32_t method, uint32_t data) { (void)user; (void)sub; (void)method; (void)data; ++g_method_calls; }
static void port_flip(void *user, uint32_t id) { (void)user; (void)id; }
#ifdef RSX_LIVE_HAS_DIRECT
static const uint8_t *port_pointer(void *user, uint32_t address, uint32_t bytes)
{
    (void)user;
    if ((uint64_t)address + bytes > 0x100000000ull) return NULL;
    for (uint64_t page = address / 4096u, last = ((uint64_t)address + bytes - 1) / 4096u; page <= last; ++page)
        if (!is_committed(&g_flat, page)) return NULL;
    return g_flat.base + address;
}
#endif

/* ---- background load: threads doing the guarded reads and writes the guest and SPU threads do ---- */
static atomic_int g_bg_stop;
static void *bg_thread(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg * 2654435761u + 1u, i = 0;
    unsigned char b[16];
    while (!atomic_load(&g_bg_stop)) {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t address = 0x00010000u + ((seed >> 8) & 0x00FFFFC0u);
        if ((i++ & 3u) == 3u) (void)g_coherent.write(g_coherent.context, address, b, 8);
        else (void)g_coherent.read(g_coherent.context, address, b, 16);
        for (volatile int spin = 0; spin < 40; ++spin) { }
    }
    return NULL;
}

/* ---- the FIFO ---- */
static uint32_t put_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return v; }
static uint32_t load_fifo(const char *path, uint32_t *packets, uint32_t *methods)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *rec = malloc((size_t)size);
    if (fread(rec, 1, (size_t)size, f) != (size_t)size) { puts("short read"); exit(2); }
    fclose(f);
    const size_t n = (size_t)size / 12u;
    uint32_t offset = 0, count = 0, np = 0;
    uint8_t *io = g_flat.base + IO_EA;
    for (size_t i = 0; i < n;) {
        if (rec[i * 3] == 0xFFFFFFFFu) { ++i; continue; }
        size_t j = i + 1;
        while (j < n && rec[j * 3] == rec[i * 3] && rec[j * 3] != 0xFFFFFFFFu && rec[j * 3 + 1] == rec[(j - 1) * 3 + 1] + 4u && j - i < 2000u) ++j;
        const uint32_t words = (uint32_t)(j - i);
        put_be(io + offset, (words << 18) | (rec[i * 3] << 13) | (rec[i * 3 + 1] & 0x1FFCu)); offset += 4;
        for (size_t k = i; k < j; ++k) { put_be(io + offset, rec[k * 3 + 2]); offset += 4; }
        count += words; ++np; i = j;
        if (offset > IO_SIZE - 0x10000u) break;
    }
    free(rec);
    *packets = np; *methods = count;
    return offset;
}

static int run_variant(const char *name, int direct, int bg_threads, uint32_t fifo_bytes, uint32_t packets, uint32_t methods, int repeats)
{
    pthread_t th[16];
    rsx_live_ports ports = {NULL, port_read32, port_write32, port_method, port_flip};
    rsx_live_layout layout = {CONTROL, IO_EA, IO_SIZE, LABELS, LABEL_SIZE, 0x1400};
    (void)direct;
#ifdef RSX_LIVE_HAS_DIRECT
    if (direct) ports.pointer = port_pointer;
#else
    if (direct) return 0;
#endif
    atomic_store(&g_bg_stop, 0);
    for (int i = 0; i < bg_threads; ++i) pthread_create(&th[i], NULL, bg_thread, (void *)(uintptr_t)(i + 1));
    uint64_t best = UINT64_MAX; uint64_t words = 0;
    for (int r = 0; r < repeats; ++r) {
        rsx_live_fifo fifo; rsx_live_init(&fifo, &layout);
#ifdef RSX_LIVE_HAS_DIRECT
        fifo.get_batch_words = direct ? GET_BATCH : 0;
#endif
        port_write32(NULL, CONTROL, fifo_bytes); port_write32(NULL, CONTROL + 4, 0); port_write32(NULL, CONTROL + 8, 0);
        g_method_calls = 0;
        const uint64_t t0 = now_ns();
        rsx_live_status st;
        do { st = rsx_live_pump(&fifo, &ports, 0x20000u); } while (st == RSX_LIVE_BUDGET);
        const uint64_t dt = now_ns() - t0;
        if (st != RSX_LIVE_IDLE || fifo.fault) { printf("BENCH %s FAILED status=%d fault=%u reason=%u\n", name, (int)st, fifo.fault, fifo.fault_reason); break; }
        words = fifo.words;
        if (dt < best) best = dt;
    }
    atomic_store(&g_bg_stop, 1);
    for (int i = 0; i < bg_threads; ++i) pthread_join(th[i], NULL);
    printf("BENCH %-8s bg=%d fifo_bytes=%u words=%llu packets=%u methods=%u ns_total=%llu ns_per_word=%.1f ns_per_packet=%.1f ns_per_method=%.1f (best of %d)\n",
           name, bg_threads, fifo_bytes, (unsigned long long)words, packets, methods, (unsigned long long)best,
           (double)best / (double)words, (double)best / packets, (double)best / methods, repeats);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: bench_rsx_live_read <methods.bin> [bg threads] [repeats]"); return 2; }
    const int bg_max = argc > 2 ? atoi(argv[2]) : 3, repeats = argc > 3 ? atoi(argv[3]) : 5;
    build_chain();
    uint32_t packets = 0, methods = 0;
    const uint32_t bytes = load_fifo(argv[1], &packets, &methods);
    printf("BENCH_INPUT fifo_bytes=%u words=%u packets=%u methods=%u\n", bytes, bytes / 4u, packets, methods);
    for (int bg = 0; bg <= bg_max; bg += (bg_max > 3 ? 2 : 1)) {
        run_variant("domain", 0, bg, bytes, packets, methods, repeats);
        run_variant("direct", 1, bg, bytes, packets, methods, repeats);
    }
    return 0;
}
