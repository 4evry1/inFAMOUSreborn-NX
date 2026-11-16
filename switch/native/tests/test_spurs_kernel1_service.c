#define _POSIX_C_SOURCE 200809L
/* Kernel-1 idle service over a real atomic domain, real coherent guest memory,
 * real owners/LS buffers, real worker thread and real wait port. Expected bytes
 * are hand-derived from KERNEL-IDLE-CONTRACT s1/s2 below, not produced by the
 * implementation and not from a capture. */
#include "atomic_domain.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "../domain/spurs_kernel1_service.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { SPACE = 0x00800000, B = 0x007CF100, LS = PS3_SPURS_K1_LS_BYTES, LS_FILL = 0xA5 };

typedef struct {
    ElfTestHostSpace *space;
    ps3_guest_memory backing, counted;
    atomic_uint line_writes[2]; /* full-line backing writes to L0, L1 (actual memory effects) */
    ps3_pthread_guard guard;
    ps3_pthread_wait_port port;
    ps3_pthread_service_thread worker;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[12];
    ps3_atomic_owner owners[8];
    unsigned char *ls[8];
    ps3_spurs_k1_service service;
    unsigned char image[0x1000];
    uint32_t n;
} Fixture;

static void put32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7u + 3u); }
/* s1 table for flags=0, nSpus=n, plus a pattern in bytes the kernel never reads. */
static void build_instance(unsigned char *img, uint32_t n) {
    memset(img, 0, 0x1000);
    for (unsigned i = 0x100; i < 0x900; ++i) img[i] = pattern(i);
    for (unsigned i = 0x920; i < 0xD00; ++i) img[i] = pattern(i);
    for (unsigned i = 0xD20; i < 0x1000; ++i) img[i] = pattern(i);
    img[0x6C] = img[0x6D] = img[0x6E] = img[0x6F] = 0xFF;   /* wklFlag.flag */
    img[0x76] = (unsigned char)n;                          /* nSpus */
    img[0x77] = 0xFF;                                      /* wklFlagReceiver */
    img[0xB2] = img[0xB3] = 0xFF;                          /* wklEnabled 0x0000FFFF */
    memset(img + 0xC0, 0xFF, 8);                           /* sysSrvPreemptWklId */
    img[0xC9] = 0x10;                                      /* spuPort */
    static const unsigned char info[0x20] = {0,0,0,0,0,0,1,0, 0,0,0,0,0,0,0,0, 0,0,0x22,0, 0xFF,0,0,0, 0,0,0,0,0,0,0,0};
    memcpy(img + 0xD00, info, sizeof info);                /* wklInfoSysSrv */
}
static ps3_guest_memory_status counted_write(void *context, uint32_t a, const void *p, size_t n) {
    Fixture *f = context;
    if (n == 128 && (a == B || a == B + 0x80)) atomic_fetch_add(&f->line_writes[a == B ? 0 : 1], 1);
    return f->backing.write(f->backing.context, a, p, n);
}
static ps3_guest_memory_status counted_read(void *c, uint32_t a, void *p, size_t n) { Fixture *f = c; return f->backing.read(f->backing.context, a, p, n); }
static ps3_guest_memory_status counted_zero(void *c, uint32_t a, uint64_t n) { Fixture *f = c; return f->backing.zero(f->backing.context, a, n); }
static ps3_guest_memory_status counted_commit(void *c, uint32_t a, uint64_t n) { Fixture *f = c; return f->backing.commit(f->backing.context, a, n); }
static ps3_guest_memory_status counted_decommit(void *c, uint32_t a, uint64_t n) { Fixture *f = c; return f->backing.decommit(f->backing.context, a, n); }
static void setup(Fixture *f, uint32_t n, const unsigned char *img) {
    memset(f, 0, sizeof *f);
    f->n = n;
    f->space = elf_test_host_space_create(SPACE);
    CHECK(f->space);
    f->backing = elf_test_host_space_port(f->space);
    CHECK(f->backing.commit(f->backing.context, 0, SPACE) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_pthread_guard_init(&f->guard) == 0 && ps3_pthread_wait_port_init(&f->port) == 0);
    f->counted = (ps3_guest_memory){f, counted_commit, counted_decommit, counted_read, counted_write, counted_zero};
    CHECK(ps3_atomic_domain_init(&f->domain, f->counted, ps3_pthread_guard_port(&f->guard), f->records, 12, NULL, 0) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_domain_attach_wait_port(&f->domain, ps3_pthread_wait_port_port(&f->port)) == PS3_GUEST_MEMORY_OK);
    memcpy(f->image, img, 0x1000);
    CHECK(ps3_atomic_write(&f->domain, B, f->image, 0x1000) == PS3_GUEST_MEMORY_OK);
    unsigned char guard_bytes[0x100];
    for (unsigned i = 0; i < 0x100; ++i) guard_bytes[i] = pattern(i + 0x5000);
    CHECK(ps3_atomic_write(&f->domain, B - 0x100, guard_bytes, 0x100) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain, B + 0x1000, guard_bytes, 0x100) == PS3_GUEST_MEMORY_OK);
    for (uint32_t i = 0; i < n; ++i) {
        CHECK(ps3_atomic_owner_acquire(&f->domain, &f->owners[i]) == PS3_GUEST_MEMORY_OK);
        f->ls[i] = malloc(LS);
        CHECK(f->ls[i]);
        memset(f->ls[i], LS_FILL, LS);
    }
    atomic_store(&f->line_writes[0], 0);
    atomic_store(&f->line_writes[1], 0);
}
static ps3_spurs_k1_config config_of(Fixture *f, const uint8_t *order) {
    ps3_spurs_k1_config c = {&f->domain, ps3_pthread_wait_port_port(&f->port), ps3_pthread_service_thread_port(&f->worker),
                             B, f->n, f->owners, f->ls, order};
    return c;
}
static void teardown(Fixture *f) {
    ps3_spurs_k1_cancel_and_join(&f->service);
    for (uint32_t i = 0; i < f->n; ++i) {
        CHECK(ps3_atomic_owner_release(&f->domain, &f->owners[i]) == PS3_GUEST_MEMORY_OK);
        free(f->ls[i]);
    }
    unsigned char guard_bytes[0x100];
    CHECK(ps3_atomic_read(&f->domain, B - 0x100, guard_bytes, 0x100) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 0x100; ++i) CHECK(guard_bytes[i] == pattern(i + 0x5000));
    CHECK(ps3_atomic_read(&f->domain, B + 0x1000, guard_bytes, 0x100) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 0x100; ++i) CHECK(guard_bytes[i] == pattern(i + 0x5000));
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->guard);
    elf_test_host_space_destroy(f->space);
}
static void read_instance(Fixture *f, unsigned char *out) { CHECK(ps3_atomic_read(&f->domain, B, out, 0x1000) == PS3_GUEST_MEMORY_OK); }
static void expect_instance(Fixture *f, const unsigned char *expected) {
    unsigned char now[0x1000];
    read_instance(f, now);
    for (unsigned i = 0; i < 0x1000; ++i)
        if (now[i] != expected[i]) { fprintf(stderr, "instance +0x%03X = %02X expected %02X\n", i, now[i], expected[i]); abort(); }
}
static void quiescent_instance(const unsigned char *img, uint32_t n, unsigned char *out) {
    memcpy(out, img, 0x1000);
    unsigned char mask = (unsigned char)((1u << n) - 1u);
    out[0x73] = mask; out[0xC8] = mask; out[0xCC] = mask;
}
/* Expected LS of context `spu`, which ran at position `position` of `order`. */
static void expected_ls(const unsigned char *img, const unsigned char *final_img, uint32_t spu,
                        const uint8_t *order, uint32_t position, unsigned char *ls) {
    memset(ls, LS_FILL, LS);
    memcpy(ls + 0x80, img + 0x900, 0x80);
    memset(ls + 0x100, 0, 0x190);
    memcpy(ls + 0x100, final_img, 0x80);
    put32(ls + 0x1C4, B);
    put32(ls + 0x1C8, spu);
    put32(ls + 0x1CC, 31);
    put32(ls + 0x1D4, 0x100);
    put32(ls + 0x1D8, 0xFF);
    put32(ls + 0x1DC, 32);
    put32(ls + 0x1E0, 0x808);
    put32(ls + 0x1E4, 0x290);
    ls[0x1EA] = 1; ls[0x1EB] = 1;
    put32(ls + 0x1F0, 0xF0020000u);
    put32(ls + 0x200, 0x00020000u);
    put32(ls + 0x280, 0x423A3A02u); put32(ls + 0x284, 0x43F43A82u); put32(ls + 0x288, 0x43F26502u); put32(ls + 0x28C, 0x420EB382u);
    unsigned char mirror_mask = 0;
    for (uint32_t k = 0; k <= position; ++k) mirror_mask |= (unsigned char)(1u << order[k]);
    memcpy(ls + 0x2D80, img + 0x80, 0x80);
    ls[0x2D80 + 0x48] = mirror_mask; ls[0x2D80 + 0x4C] = mirror_mask;
    memcpy(ls + 0x3FFE0, img + 0xD00, 0x20);
}
static void expect_ls(Fixture *f, const unsigned char *final_img, const uint8_t *order) {
    unsigned char *want = malloc(LS);
    CHECK(want);
    for (uint32_t k = 0; k < f->n; ++k) {
        uint32_t spu = order[k];
        expected_ls(f->image, final_img, spu, order, k, want);
        for (uint32_t i = 0; i < LS; ++i)
            if (f->ls[spu][i] != want[i]) { fprintf(stderr, "spu %u LS 0x%05X = %02X expected %02X\n", spu, i, f->ls[spu][i], want[i]); abort(); }
    }
    free(want);
}
static const uint8_t identity[8] = {0, 1, 2, 3, 4, 5, 6, 7};

static void idle_lane(uint32_t n, const uint8_t *order) {
    Fixture *f = calloc(1, sizeof *f);
    unsigned char img[0x1000], fin[0x1000];
    build_instance(img, n);
    setup(f, n, img);
    ps3_spurs_k1_config c = config_of(f, order == identity ? NULL : order);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop = {0};
    CHECK(ps3_spurs_k1_start(&f->service, &stop) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_phase_of(&f->service) == PS3_SPURS_K1_PHASE_QUIESCENT);
    CHECK(!ps3_spurs_k1_poll_stop(&f->service, NULL));
    /* Actual conditional stores (no value-identical elision): per context
     * L1 K4,K5,K6,K8 and L0 K9,K12. */
    CHECK(atomic_load(&f->line_writes[1]) == 4 * n && atomic_load(&f->line_writes[0]) == 2 * n);
    /* Quiescence means every context holds its L0 reservation with a clear latch
     * (non-consuming probe: expired deadline). */
    uint64_t probe_mask = 0;
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f->domain, f->owners, n, 0, NULL, &probe_mask) == PS3_ATOMIC_WAIT_TIMEOUT);
    quiescent_instance(img, n, fin);
    expect_instance(f, fin);
    for (uint32_t i = 0; i < n; ++i) {
        CHECK(ps3_spurs_k1_context_state_of(&f->service, i) == PS3_SPURS_K1_CTX_BLOCKED_LR);
        CHECK(ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_STEPS) == 0x3FFEu);
        CHECK(ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS) == 1);
    }
    /* Each context holds an L0 reservation: an unrelated-line store wakes nobody;
     * a same-value store to B+0x00 wakes all, they re-block, bytes unchanged. */
    uint64_t settled = ps3_spurs_k1_settled_count(&f->service), evals = 0;
    for (uint32_t i = 0; i < n; ++i) evals += ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS);
    unsigned char same = 0;
    CHECK(ps3_atomic_write(&f->domain, B + 0x100, &img[0x100], 1) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain, B, &same, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_spurs_k1_wait_settled_after(&f->service, settled, ps3_pthread_wait_port_now_ns() + 5000000000u) == PS3_SPURS_K1_PHASE_QUIESCENT);
    uint64_t evals_after = 0;
    for (uint32_t i = 0; i < n; ++i) evals_after += ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS);
    CHECK(evals_after == evals + n);
    expect_instance(f, fin);
    /* No busy polling: nothing is evaluated while nobody writes. */
    struct timespec pause = {0, 50000000};
    nanosleep(&pause, NULL);
    uint64_t evals_idle = 0;
    for (uint32_t i = 0; i < n; ++i) evals_idle += ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS);
    CHECK(evals_idle == evals_after);
    /* Cancel during BLOCKED_LR: all cancelled, joined, owners released (teardown). */
    ps3_spurs_k1_cancel_and_join(&f->service);
    CHECK(ps3_spurs_k1_phase_of(&f->service) == PS3_SPURS_K1_PHASE_CANCELLED);
    for (uint32_t i = 0; i < n; ++i) CHECK(ps3_spurs_k1_context_state_of(&f->service, i) == PS3_SPURS_K1_CTX_CANCELLED);
    expect_ls(f, fin, order);
    expect_instance(f, fin);
    teardown(f);
    free(f);
}

typedef struct { Fixture *f; atomic_int stop; unsigned stores; } Hammer;
static void *hammer_main(void *p) {
    Hammer *h = p;
    unsigned i = 0;
    while (!atomic_load(&h->stop)) {
        uint32_t offset = (i & 1) ? 0x80 + 0x60 + (i % 16) : (i % 0x40);
        unsigned char v;
        CHECK(ps3_atomic_read(&h->f->domain, B + offset, &v, 1) == PS3_GUEST_MEMORY_OK);
        CHECK(ps3_atomic_write(&h->f->domain, B + offset, &v, 1) == PS3_GUEST_MEMORY_OK); /* same value */
        ++i; ++h->stores;
        sched_yield();
    }
    return NULL;
}
static void contention_lane(uint32_t n, unsigned rounds) {
    uint64_t failures = 0;
    for (unsigned r = 0; r < rounds; ++r) {
        Fixture *f = calloc(1, sizeof *f);
        unsigned char img[0x1000], fin[0x1000];
        build_instance(img, n);
        setup(f, n, img);
        ps3_spurs_k1_config c = config_of(f, NULL);
        CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
        Hammer h = {.f = f};
        pthread_t t; CHECK(pthread_create(&t, NULL, hammer_main, &h) == 0);
        CHECK(ps3_spurs_k1_start(&f->service, NULL) == PS3_SPURS_K1_OK);
        atomic_store(&h.stop, 1);
        CHECK(pthread_join(t, NULL) == 0);
        uint64_t settled = ps3_spurs_k1_settled_count(&f->service);
        (void)settled;
        quiescent_instance(img, n, fin);
        expect_instance(f, fin);
        for (uint32_t i = 0; i < n; ++i) failures += ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_CONDITIONAL_FAILURES);
        ps3_spurs_k1_cancel_and_join(&f->service);
        for (uint32_t i = 0; i < n; ++i) CHECK(ps3_spurs_k1_counter(&f->service, i, PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS) == 1);
        teardown(f);
        free(f);
    }
    printf("contention lane n=%u rounds=%u conditional_failures=%llu\n", n, rounds, (unsigned long long)failures);
}

static void stop_lane(uint32_t n, void (*mutate)(unsigned char *), int after_quiescence_write_offset,
                      unsigned char write_value, ps3_spurs_k1_status expected_status, uint32_t reason,
                      uint32_t spu, void (*expect)(unsigned char *img, unsigned char *want)) {
    Fixture *f = calloc(1, sizeof *f);
    unsigned char img[0x1000], want[0x1000];
    build_instance(img, n);
    if (mutate) mutate(img);
    setup(f, n, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop = {0};
    ps3_spurs_k1_status status = ps3_spurs_k1_start(&f->service, &stop);
    CHECK(status == expected_status);
    if (after_quiescence_write_offset >= 0) {
        CHECK(ps3_atomic_write(&f->domain, B + (uint32_t)after_quiescence_write_offset, &write_value, 1) == PS3_GUEST_MEMORY_OK);
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!ps3_spurs_k1_poll_stop(&f->service, &stop)) {   /* bounded: a missing stop must fail, not hang */
            clock_gettime(CLOCK_MONOTONIC, &t1);
            CHECK(t1.tv_sec - t0.tv_sec < 5);
            sched_yield();
        }
    } else {
        CHECK(ps3_spurs_k1_poll_stop(&f->service, &stop));
    }
    CHECK(stop.reason == reason && stop.spu == spu);
    CHECK(ps3_spurs_k1_context_state_of(&f->service, spu) == PS3_SPURS_K1_CTX_STOPPED);
    ps3_spurs_k1_cancel_and_join(&f->service);
    expect(img, want);
    if (after_quiescence_write_offset >= 0) want[after_quiescence_write_offset] = write_value;
    expect_instance(f, want);
    teardown(f);
    free(f);
}
static void unchanged(unsigned char *img, unsigned char *want) { memcpy(want, img, 0x1000); }
static void idle5(unsigned char *img, unsigned char *want) { quiescent_instance(img, 5, want); }
static void preset_on_spu(unsigned char *img) { img[0xC8] = 0x01; }
static void trace_nonzero(unsigned char *img) { img[0x907] = 0x40; }
static void kernel2(unsigned char *img) { img[0x74] = 0x40; }
static void exit_if_no_work(unsigned char *img) { img[0x74] = 0x80; }
static void preempt(unsigned char *img) { img[0xC2] = 0x03; }
static void trace_start_index(unsigned char *img) { img[0x90B] = 0x01; }
static void sysinfo_image(unsigned char *img) { img[0xD06] = 0x02; }
static void exit_expect(unsigned char *img, unsigned char *want) {
    /* K4/K5 committed, K12 published, then the idle pass sees allSpusIdle && EXIT_IF_NO_WORK. */
    memcpy(want, img, 0x1000); want[0x73] = 1; want[0xC8] = 1; want[0xCC] = 1;
}
static void k4_halt_expect(unsigned char *img, unsigned char *want) { memcpy(want, img, 0x1000); }
static void trace_expect(unsigned char *img, unsigned char *want) {
    memcpy(want, img, 0x1000); want[0xC8] = 1; want[0xCC] = 1; /* K4,K5 precede K5b */
}
static void preempt_expect(unsigned char *img, unsigned char *want) {
    /* spu0/1 reach idle; spu2 commits K4/K5 and stops at K6 before its commit. */
    memcpy(want, img, 0x1000); want[0x73] = 0x03; want[0xC8] = 0x07; want[0xCC] = 0x07;
}

static void signal_preserved(void) {
    unsigned char img[0x1000], fin[0x1000];
    build_instance(img, 5);
    img[0x70] = 0x80; img[0x71] = 0x01;   /* wklSignal1: must not be cleared for wid 32 */
    img[0x78] = 0x80; img[0x79] = 0x00;   /* wklSignal2 */
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_start(&f->service, NULL) == PS3_SPURS_K1_OK);
    quiescent_instance(img, 5, fin);
    expect_instance(f, fin);
    teardown(f);
    free(f);
}
static void message_before_select(void) {
    unsigned char img[0x1000];
    build_instance(img, 5);
    img[0x72] = 0x01;
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop = {0};
    CHECK(ps3_spurs_k1_start(&f->service, &stop) == PS3_SPURS_K1_STOPPED);
    CHECK(stop.reason == PS3_SPURS_K1_STOP_MESSAGE_WAKE && stop.spu == 0 && stop.step == 9);
    CHECK(atomic_load(&f->line_writes[0]) == 0 && atomic_load(&f->line_writes[1]) == 4); /* K4,K5,K6,K8 */
    unsigned char want[0x1000];
    memcpy(want, img, 0x1000); want[0xC8] = 1; want[0xCC] = 1;
    expect_instance(f, want);
    teardown(f);
    free(f);
}

/* K1 (root): construction on live storage is refused and nothing is disturbed. */
static void reinit_refused(void) {
    unsigned char img[0x1000], fin[0x1000];
    build_instance(img, 5);
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_start(&f->service, NULL) == PS3_SPURS_K1_OK);
    quiescent_instance(img, 5, fin);
    ps3_spurs_k1_config alias = f->service.config;          /* borrowed alias of live storage */
    CHECK(ps3_spurs_k1_init(&f->service, &alias) == PS3_SPURS_K1_INVALID);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_INVALID);
    CHECK(f->service.worker_started && !f->service.worker_joined && f->service.initialized);
    CHECK(ps3_spurs_k1_phase_of(&f->service) == PS3_SPURS_K1_PHASE_QUIESCENT);
    for (uint32_t i = 0; i < 5; ++i) CHECK(ps3_spurs_k1_context_state_of(&f->service, i) == PS3_SPURS_K1_CTX_BLOCKED_LR);
    /* the worker is still the same live one: a same-value store is still re-evaluated by all five */
    uint64_t settled = ps3_spurs_k1_settled_count(&f->service);
    unsigned char same = 0;
    CHECK(ps3_atomic_write(&f->domain, B, &same, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_spurs_k1_wait_settled_after(&f->service, settled, ps3_pthread_wait_port_now_ns() + 5000000000u) == PS3_SPURS_K1_PHASE_QUIESCENT);
    expect_instance(f, fin);
    /* after confirmed join the storage may be rebuilt (never while live) */
    ps3_spurs_k1_cancel_and_join(&f->service);
    CHECK(f->service.worker_joined);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    teardown(f);
    free(f);
}
/* K2 (root): a failed join is never recorded as joined; resources stay owned. */
static void join_failure_not_recorded(void) {
    unsigned char img[0x1000];
    build_instance(img, 5);
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    CHECK(ps3_spurs_k1_start(&f->service, NULL) == PS3_SPURS_K1_OK);
    f->worker.fail_next_join = 1;
    ps3_spurs_k1_cancel_and_join(&f->service);
    CHECK(!f->service.worker_joined);                       /* adapter failed: lifetime not ended */
    CHECK(f->worker.started);                               /* thread really still exists */
    ps3_spurs_k1_cancel_and_join(&f->service);              /* retry joins for real */
    CHECK(f->service.worker_joined && !f->worker.started);
    teardown(f);
    free(f);
}
static void worker_start_failure(void) {
    unsigned char img[0x1000];
    build_instance(img, 5);
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    f->worker.fail_next_start = 1;
    ps3_spurs_k1_config c = config_of(f, NULL);
    CHECK(ps3_spurs_k1_init(&f->service, &c) == PS3_SPURS_K1_OK);
    ps3_spurs_k1_stop stop = {0};
    CHECK(ps3_spurs_k1_start(&f->service, &stop) == PS3_SPURS_K1_STOPPED && stop.reason == PS3_SPURS_K1_STOP_WORKER_START_FAILED);
    CHECK(ps3_spurs_k1_start(&f->service, &stop) == PS3_SPURS_K1_INVALID);
    expect_instance(f, img);
    for (uint32_t i = 0; i < 5; ++i) CHECK(ps3_spurs_k1_context_state_of(&f->service, i) == PS3_SPURS_K1_CTX_NOT_STARTED);
    teardown(f);
    free(f);
}
static void invalid_configs(void) {
    unsigned char img[0x1000];
    build_instance(img, 5);
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 5, img);
    ps3_spurs_k1_config c = config_of(f, NULL);
    ps3_spurs_k1_config bad = c; bad.instance = B + 0x40;
    CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    bad = c; bad.spu_count = 0; CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    bad = c; bad.spu_count = 9; CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    ps3_atomic_owner dup[5] = {f->owners[0], f->owners[1], f->owners[1], f->owners[3], f->owners[4]};
    bad = c; bad.owners = dup; CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    const uint8_t bad_order[5] = {0, 1, 1, 3, 4};
    bad = c; bad.start_order = bad_order; CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    ps3_pthread_wait_port other; CHECK(ps3_pthread_wait_port_init(&other) == 0);
    bad = c; bad.wait_port = ps3_pthread_wait_port_port(&other); CHECK(ps3_spurs_k1_init(&f->service, &bad) == PS3_SPURS_K1_INVALID);
    ps3_pthread_wait_port_destroy(&other);
    expect_instance(f, img);
    teardown(f);
    free(f);
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 20;
    static const uint8_t permuted[5] = {3, 0, 4, 1, 2};
    idle_lane(5, identity);
    idle_lane(5, permuted);
    idle_lane(1, identity);
    contention_lane(5, rounds);
    contention_lane(1, rounds);
    signal_preserved();
    /* Message wake after CELL_OK-equivalent quiescence: fence before PUTLLC. */
    stop_lane(5, NULL, 0x72, 0x01, PS3_SPURS_K1_OK, PS3_SPURS_K1_STOP_MESSAGE_WAKE, 0, idle5);
    stop_lane(5, NULL, 0x72, 0x08, PS3_SPURS_K1_OK, PS3_SPURS_K1_STOP_MESSAGE_WAKE, 3, idle5);
    stop_lane(5, preset_on_spu, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_ALREADY_INIT_HALT, 0, k4_halt_expect);
    stop_lane(5, trace_nonzero, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_TRACE_BUFFER_NONZERO, 0, trace_expect);
    stop_lane(5, trace_start_index, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 0, trace_expect);
    stop_lane(5, kernel2, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 0, unchanged);
    stop_lane(5, sysinfo_image, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 0, unchanged);
    stop_lane(5, preempt, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 2, preempt_expect);
    stop_lane(1, exit_if_no_work, -1, 0, PS3_SPURS_K1_STOPPED, PS3_SPURS_K1_STOP_EXIT_REQUESTED, 0, exit_expect);
    /* K4 (root): a guest rewrite of nSpus or an unsupported kernel mode after quiescence stops
     * on the fresh L0 snapshot BEFORE any store (bytes = quiescent image + the guest's own write). */
    stop_lane(5, NULL, 0x76, 0x04, PS3_SPURS_K1_OK, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 0, idle5);
    stop_lane(5, NULL, 0x74, 0x40, PS3_SPURS_K1_OK, PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 0, idle5);
    reinit_refused();
    join_failure_not_recorded();
    message_before_select();
    worker_start_failure();
    invalid_configs();
    puts("test_spurs_kernel1_service: OK");
    return 0;
}
