/* Behavioural tests for ps3_tls_initialize_allocated: real host guest-memory adapter with real committed
 * bytes. The counting port forwards every normal access to the real adapter; the only injected behaviour
 * is one denied byte operation (read/write/zero) per case. Nothing here mocks a successful copy. */
#include "tls_initialize.h"
#include "guest_space_host.h"
#include "memory/user_memory.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); abort(); } } while (0)
enum { SPACE = 0x4000000, SRC = 0x00100000, SRC_BYTES = 0x4000, BLOCK = 0x02000000, REGION = 0x30000, PAD = 0x1000,
       FILL = 0xA5 };
typedef struct {
    ps3_guest_memory real;
    unsigned commits, decommits, reads, writes, zeros;
    unsigned fail_read, fail_write, fail_zero; /* 1-based call index to deny; 0 = never */
} Port;
typedef struct { ElfTestHostSpace *space; Port port; unsigned char src[SRC_BYTES]; } Env;
static unsigned tests;

static ps3_guest_memory_status p_commit(void *c, uint32_t a, uint64_t n) { Port *p = c; ++p->commits; return p->real.commit(p->real.context, a, n); }
static ps3_guest_memory_status p_decommit(void *c, uint32_t a, uint64_t n) { Port *p = c; ++p->decommits; return p->real.decommit(p->real.context, a, n); }
static ps3_guest_memory_status p_read(void *c, uint32_t a, void *o, size_t n) {
    Port *p = c;
    if (++p->reads == p->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return p->real.read(p->real.context, a, o, n);
}
static ps3_guest_memory_status p_write(void *c, uint32_t a, const void *i, size_t n) {
    Port *p = c;
    if (++p->writes == p->fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return p->real.write(p->real.context, a, i, n);
}
static ps3_guest_memory_status p_zero(void *c, uint32_t a, uint64_t n) {
    Port *p = c;
    if (++p->zeros == p->fail_zero) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return p->real.zero(p->real.context, a, n);
}
static ps3_guest_memory counting(Port *p) { ps3_guest_memory m = {p, p_commit, p_decommit, p_read, p_write, p_zero}; return m; }
static ps3_guest_memory null_lifecycle(Port *p) { ps3_guest_memory m = {p, NULL, NULL, p_read, p_write, p_zero}; return m; }
static void reset_counts(Port *p) { p->commits = p->decommits = p->reads = p->writes = p->zeros = 0; p->fail_read = p->fail_write = p->fail_zero = 0; }

static void real_commit(Env *e, uint32_t a, uint64_t n) { CHECK(e->port.real.commit(e->port.real.context, a, n) == PS3_GUEST_MEMORY_OK); }
static void real_fill(Env *e, uint32_t a, uint64_t n, int byte) {
    unsigned char *b = malloc((size_t)n); CHECK(b); memset(b, byte, (size_t)n);
    CHECK(e->port.real.write(e->port.real.context, a, b, (size_t)n) == PS3_GUEST_MEMORY_OK); free(b);
}
static void real_read(Env *e, uint32_t a, void *out, size_t n) { CHECK(e->port.real.read(e->port.real.context, a, out, n) == PS3_GUEST_MEMORY_OK); }
static void env_init(Env *e) {
    memset(e, 0, sizeof *e);
    e->space = elf_test_host_space_create(SPACE); CHECK(e->space);
    e->port.real = elf_test_host_space_port(e->space);
    for (size_t i = 0; i < SRC_BYTES; ++i) e->src[i] = (unsigned char)(i * 7 + 3);
    real_commit(e, SRC, SRC_BYTES);
    CHECK(e->port.real.write(e->port.real.context, SRC, e->src, SRC_BYTES) == PS3_GUEST_MEMORY_OK);
}
/* Caller-owned storage: [BLOCK-PAD, BLOCK+REGION) committed and filled with FILL. */
static void env_region(Env *e) { real_commit(e, BLOCK - PAD, PAD + REGION); real_fill(e, BLOCK - PAD, PAD + REGION, FILL); }
static void env_destroy(Env *e) { elf_test_host_space_destroy(e->space); }

static Ps3TlsReport run(const ps3_guest_memory *m, uint32_t block, uint64_t cap, Ps3TlsTemplate t, size_t scratch_size,
                        Ps3TlsImage *out) {
    unsigned char *scratch = malloc(scratch_size ? scratch_size : 1); CHECK(scratch);
    Ps3TlsReport r = ps3_tls_initialize_allocated(m, block, cap, &t, scratch, scratch_size, out);
    free(scratch);
    return r;
}
/* Validation failure: no guest byte is touched, no callback is called, output unchanged. */
static void expect_validation(Env *e, uint32_t block, uint64_t cap, Ps3TlsTemplate t, Ps3TlsStatus st) {
    unsigned char *before = malloc(PAD + REGION), *after = malloc(PAD + REGION), src[SRC_BYTES];
    CHECK(before && after);
    real_read(e, BLOCK - PAD, before, PAD + REGION);
    real_read(e, SRC, src, SRC_BYTES);
    Ps3TlsImage out, old; memset(&out, 0xa5, sizeof out); old = out;
    reset_counts(&e->port);
    ps3_guest_memory m = counting(&e->port);
    Ps3TlsReport r = run(&m, block, cap, t, 3, &out);
    CHECK(r.status == st && r.cause == st);
    CHECK(r.memory_status == PS3_GUEST_MEMORY_OK && r.cleanup_status == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(&out, &old, sizeof out));
    CHECK(!e->port.commits && !e->port.decommits && !e->port.reads && !e->port.writes && !e->port.zeros);
    real_read(e, BLOCK - PAD, after, PAD + REGION);
    CHECK(!memcmp(before, after, PAD + REGION));
    real_read(e, SRC, before, SRC_BYTES); CHECK(!memcmp(before, src, SRC_BYTES));
    free(before); free(after); ++tests;
}
static void expect_region_bytes(Env *e, uint32_t addr, const unsigned char *want, size_t n) {
    unsigned char *got = malloc(n); CHECK(got); real_read(e, addr, got, n); CHECK(!memcmp(got, want, n)); free(got);
}
static void expect_uniform(Env *e, uint32_t addr, size_t n, int byte) {
    unsigned char *got = malloc(n); CHECK(got); real_read(e, addr, got, n);
    for (size_t i = 0; i < n; ++i) CHECK(got[i] == byte);
    free(got);
}

static void test_basic(int null_cb) {
    Env e; env_init(&e); env_region(&e);
    reset_counts(&e.port);
    ps3_guest_memory m = null_cb ? null_lifecycle(&e.port) : counting(&e.port);
    Ps3TlsImage out = {1, 2, 3};
    Ps3TlsReport r = run(&m, BLOCK, 0x9000, (Ps3TlsTemplate){SRC + 0xe7c, 4, 0x200}, 2, &out);
    CHECK(r.status == PS3_TLS_OK && r.cause == PS3_TLS_OK && r.memory_status == PS3_GUEST_MEMORY_OK && r.cleanup_status == PS3_GUEST_MEMORY_OK);
    CHECK(out.block == BLOCK && out.thread_pointer == BLOCK + 0x7000 && out.allocation_size == 0x9000);
    CHECK(e.port.reads == 2 && e.port.writes == 2 && e.port.zeros == 1 && !e.port.commits && !e.port.decommits);
    expect_region_bytes(&e, BLOCK, e.src + 0xe7c, 4);
    expect_uniform(&e, BLOCK + 4, 0x200 - 4, 0);                   /* BSS tail zeroed over previous 0xA5 */
    expect_uniform(&e, BLOCK + 0x200, REGION - 0x200, FILL);        /* nothing beyond memory_size is written */
    expect_uniform(&e, BLOCK - PAD, PAD, FILL);
    env_destroy(&e); ++tests;
}
static void test_chunked(void) {
    static const size_t scratches[] = {1, 7, 4096, 10000, 20000};
    for (size_t k = 0; k < sizeof scratches / sizeof *scratches; ++k) {
        Env e; env_init(&e); env_region(&e); reset_counts(&e.port);
        ps3_guest_memory m = counting(&e.port);
        Ps3TlsImage out;
        Ps3TlsReport r = run(&m, BLOCK, 0x10000, (Ps3TlsTemplate){SRC + 5, 10000, 12000}, scratches[k], &out);
        unsigned chunks = (unsigned)((10000 + scratches[k] - 1) / scratches[k]);
        CHECK(r.status == PS3_TLS_OK && out.allocation_size == 0xB000);
        CHECK(e.port.reads == chunks && e.port.writes == chunks && e.port.zeros == 1 && !e.port.commits && !e.port.decommits);
        expect_region_bytes(&e, BLOCK, e.src + 5, 10000);
        expect_uniform(&e, BLOCK + 10000, 2000, 0);
        expect_uniform(&e, BLOCK + 12000, REGION - 12000, FILL);
        env_destroy(&e); ++tests;
    }
}
static void test_capacity(void) {
    Env e; env_init(&e); env_region(&e);
    Ps3TlsTemplate t = {SRC, 4, 0x200};
    expect_validation(&e, BLOCK, 0x8000, t, PS3_TLS_NO_SPACE);       /* one page too small */
    expect_validation(&e, BLOCK, 0, t, PS3_TLS_NO_SPACE);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, 4, 0x1001}, PS3_TLS_NO_SPACE); /* rounds to 0xA000 */
    expect_validation(&e, BLOCK, 0x7000, (Ps3TlsTemplate){0, 0, 0}, PS3_TLS_NO_SPACE);
    Ps3TlsImage out;
    reset_counts(&e.port); ps3_guest_memory m = null_lifecycle(&e.port);
    CHECK(run(&m, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, 4, 0x1000}, 8, &out).status == PS3_TLS_OK && out.allocation_size == 0x9000);
    CHECK(run(&m, BLOCK, 0x9000, t, 8, &out).status == PS3_TLS_OK && out.allocation_size == 0x9000); /* exact */
    /* Bigger capacity: allocation_size is the required extent, not the capacity; tail untouched. */
    real_fill(&e, BLOCK, REGION, FILL);
    CHECK(run(&m, BLOCK, 0x20000, t, 8, &out).status == PS3_TLS_OK && out.allocation_size == 0x9000 &&
          out.thread_pointer == BLOCK + 0x7000);
    expect_uniform(&e, BLOCK + 0x200, 0x20000 - 0x200, FILL);
    CHECK(run(&m, BLOCK, 0x8000, (Ps3TlsTemplate){0, 0, 0}, 1, &out).status == PS3_TLS_OK && out.allocation_size == 0x8000);
    CHECK(e.port.commits == 0 && e.port.decommits == 0);
    env_destroy(&e); ++tests;
}
static void test_end_and_overflow(void) {
    Env e; env_init(&e); env_region(&e);
    Ps3TlsTemplate none = {0, 0, 0}, t = {SRC, 4, 0x200};
    Ps3TlsImage out;
    reset_counts(&e.port); ps3_guest_memory m = counting(&e.port);
    /* Range ending exactly at 4 GiB is accepted; empty template performs no byte operation. */
    Ps3TlsReport r = run(&m, 0xFFFF8000u, 0x8000, none, 1, &out);
    CHECK(r.status == PS3_TLS_OK && out.block == 0xFFFF8000u && out.thread_pointer == 0xFFFFF000u && out.allocation_size == 0x8000);
    CHECK(!e.port.reads && !e.port.writes && !e.port.zeros && !e.port.commits && !e.port.decommits);
    r = run(&m, 0, UINT64_C(0x100000000), none, 1, &out);
    CHECK(r.status == PS3_TLS_OK && out.thread_pointer == 0x7000 && out.allocation_size == 0x8000);
    expect_validation(&e, 0xFFFF8000u, 0x9000, none, PS3_TLS_INVALID_RANGE);      /* one page past 4 GiB */
    expect_validation(&e, 0xFFFFF000u, 0x1000, none, PS3_TLS_NO_SPACE);           /* valid end, too small */
    expect_validation(&e, 0x1000, UINT64_MAX & ~UINT64_C(0xFFF), t, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, 0x1000, UINT64_C(0x100000000), none, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, 4, UINT64_MAX}, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, 4, UINT64_C(0x100000000) - 0x8000 + 1}, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){0xFFFFFFFEu, 4, 0x200}, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, UINT64_MAX, UINT64_MAX}, PS3_TLS_INVALID_RANGE);
    /* Largest representable extent passes layout; the real adapter then rejects the 4 GiB zero (no rollback). */
    reset_counts(&e.port);
    r = run(&m, 0, UINT64_C(0x100000000), (Ps3TlsTemplate){0, 0, UINT64_C(0x100000000) - 0x8000}, 1, &out);
    CHECK(r.status == PS3_TLS_MEMORY_FAILURE && r.memory_status == PS3_GUEST_MEMORY_INVALID_RANGE && e.port.zeros == 1);
    env_destroy(&e); ++tests;
}
static void test_alignment(void) {
    Env e; env_init(&e); env_region(&e);
    Ps3TlsTemplate t = {SRC, 4, 0x200};
    expect_validation(&e, BLOCK + 1, 0x9000, t, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK + 0x800, 0x9000, t, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9001, t, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK, 0x9800, t, PS3_TLS_INVALID_RANGE);
    expect_validation(&e, BLOCK + 1, 0x9fff, t, PS3_TLS_INVALID_RANGE);
    env_destroy(&e); ++tests;
}
static void expect_copy_ok(Env *e, uint32_t source, uint64_t cap) {
    unsigned char want[4]; real_read(e, source, want, 4);
    reset_counts(&e->port); ps3_guest_memory m = counting(&e->port);
    Ps3TlsImage out;
    CHECK(run(&m, BLOCK, cap, (Ps3TlsTemplate){source, 4, 0x200}, 4, &out).status == PS3_TLS_OK);
    expect_region_bytes(e, BLOCK, want, 4);
    unsigned char again[4]; real_read(e, source, again, 4);
    if (source < BLOCK || source >= BLOCK + 0x9000) CHECK(!memcmp(again, want, 4)); /* source never modified */
}
static void test_overlap(void) {
    Env e; env_init(&e); env_region(&e);
    /* Distinct per-byte contents around the boundaries so a wrong copy cannot hide. */
    for (uint32_t i = 0; i < 8; ++i) { unsigned char v = (unsigned char)(0x40 + i); CHECK(e.port.real.write(e.port.real.context, BLOCK - 4 + i, &v, 1) == 0); }
    for (uint32_t i = 0; i < 4; ++i) { unsigned char v = (unsigned char)(0x70 + i); CHECK(e.port.real.write(e.port.real.context, BLOCK + 0x9000 + i, &v, 1) == 0); }
    expect_validation(&e, BLOCK, 0x10000, (Ps3TlsTemplate){BLOCK, 4, 0x200}, PS3_TLS_OVERLAP);
    expect_validation(&e, BLOCK, 0x10000, (Ps3TlsTemplate){BLOCK + 0x8FFC, 4, 0x200}, PS3_TLS_OVERLAP);
    expect_validation(&e, BLOCK, 0x10000, (Ps3TlsTemplate){BLOCK - 2, 4, 0x200}, PS3_TLS_OVERLAP);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){BLOCK + 0x100, 1, 0x200}, PS3_TLS_OVERLAP);
    expect_copy_ok(&e, BLOCK - 4, 0x9000);          /* ends exactly at block */
    expect_copy_ok(&e, BLOCK + 0x9000, 0x10000);    /* begins exactly at required end, inside caller capacity */
    /* Empty copy never overlaps. */
    Ps3TlsImage out; reset_counts(&e.port); ps3_guest_memory m = counting(&e.port);
    CHECK(run(&m, BLOCK, 0x9000, (Ps3TlsTemplate){BLOCK, 0, 0x200}, 4, &out).status == PS3_TLS_OK && !e.port.reads);
    env_destroy(&e); ++tests;
}
static void test_invalid_inputs(void) {
    Env e; env_init(&e); env_region(&e);
    Ps3TlsTemplate t = {SRC, 4, 0x200};
    unsigned char scratch[4];
    Ps3TlsImage out, old; memset(&out, 0xa5, sizeof out); old = out;
    ps3_guest_memory full = counting(&e.port), no_read = full, no_write = full, no_zero = full;
    no_read.read = NULL; no_write.write = NULL; no_zero.zero = NULL;
    reset_counts(&e.port);
#define ARG(r) do { Ps3TlsReport rr = (r); CHECK(rr.status == PS3_TLS_ARGUMENT && rr.cause == PS3_TLS_ARGUMENT && !memcmp(&out, &old, sizeof out)); ++tests; } while (0)
    ARG(ps3_tls_initialize_allocated(NULL, BLOCK, 0x9000, &t, scratch, 4, &out));
    ARG(ps3_tls_initialize_allocated(&no_read, BLOCK, 0x9000, &t, scratch, 4, &out));
    ARG(ps3_tls_initialize_allocated(&no_write, BLOCK, 0x9000, &t, scratch, 4, &out));
    ARG(ps3_tls_initialize_allocated(&no_zero, BLOCK, 0x9000, &t, scratch, 4, &out));
    ARG(ps3_tls_initialize_allocated(&full, BLOCK, 0x9000, NULL, scratch, 4, &out));
    ARG(ps3_tls_initialize_allocated(&full, BLOCK, 0x9000, &t, NULL, 4, &out));
    ARG(ps3_tls_initialize_allocated(&full, BLOCK, 0x9000, &t, scratch, 0, &out));
    ARG(ps3_tls_initialize_allocated(&full, BLOCK, 0x9000, &t, scratch, 4, NULL));
    ARG(ps3_tls_initialize_allocated(NULL, 0, 0, NULL, NULL, 0, NULL));
#undef ARG
    CHECK(!e.port.reads && !e.port.writes && !e.port.zeros);
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){SRC, 5, 4}, PS3_TLS_INVALID_RANGE);   /* file > memory */
    expect_validation(&e, BLOCK, 0x9000, (Ps3TlsTemplate){0, 4, 0x200}, PS3_TLS_INVALID_RANGE); /* nonempty at zero */
    Ps3TlsImage ok; reset_counts(&e.port);
    CHECK(run(&full, BLOCK, 0x9000, (Ps3TlsTemplate){0, 0, 0}, 4, &ok).status == PS3_TLS_OK && !e.port.reads && !e.port.writes && !e.port.zeros);
    env_destroy(&e);
}
/* which: 0 read, 1 write, 2 zero. Template file 4, memory 0x200, scratch 2 => reads/writes 1..2, zero 1. */
static void test_failure(int which, unsigned nth) {
    Env e; env_init(&e); env_region(&e);
    unsigned char *before = malloc(PAD + REGION), *after = malloc(PAD + REGION), src[4];
    CHECK(before && after); real_read(&e, BLOCK - PAD, before, PAD + REGION); memcpy(src, e.src + 0xe7c, 4);
    reset_counts(&e.port);
    if (which == 0) e.port.fail_read = nth; else if (which == 1) e.port.fail_write = nth; else e.port.fail_zero = nth;
    ps3_guest_memory m = counting(&e.port);
    Ps3TlsTemplate t = {SRC + 0xe7c, 4, 0x200};
    Ps3TlsImage out, old; memset(&out, 0xa5, sizeof out); old = out;
    Ps3TlsReport r = run(&m, BLOCK, 0x9000, t, 2, &out);
    CHECK(r.status == PS3_TLS_MEMORY_FAILURE && r.cause == PS3_TLS_MEMORY_FAILURE);
    CHECK(r.memory_status == PS3_GUEST_MEMORY_PLATFORM_FAILURE && r.cleanup_status == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(&out, &old, sizeof out));
    CHECK(!e.port.commits && !e.port.decommits);                 /* no rollback / cleanup attempted */
    /* Model of the partial state: bytes of all previous full chunks are present; nothing was rolled back. */
    size_t copied = which == 2 ? 4 : (size_t)(nth - 1) * 2;
    memcpy(before + PAD, src, copied);
    if (which == 2) { /* zero was denied: BSS tail still holds FILL */ }
    real_read(&e, BLOCK - PAD, after, PAD + REGION);
    CHECK(!memcmp(before, after, PAD + REGION));
    /* Caller still owns committed storage: it is still mapped (reads do not report UNMAPPED). */
    unsigned char probe; CHECK(e.port.real.read(e.port.real.context, BLOCK, &probe, 1) == PS3_GUEST_MEMORY_OK);
    /* Explicit retry on the same allocation succeeds once the fault clears. */
    reset_counts(&e.port);
    r = run(&m, BLOCK, 0x9000, t, 2, &out);
    CHECK(r.status == PS3_TLS_OK && out.allocation_size == 0x9000 && !e.port.commits && !e.port.decommits);
    expect_region_bytes(&e, BLOCK, src, 4);
    expect_uniform(&e, BLOCK + 4, 0x200 - 4, 0);
    free(before); free(after); env_destroy(&e); ++tests;
}
static void test_real_memory_failures(void) {
    Env e; env_init(&e); env_region(&e);
    Ps3TlsImage out, old; memset(&out, 0xa5, sizeof out); old = out;
    ps3_guest_memory m = counting(&e.port);
    reset_counts(&e.port);
    /* Uncommitted source page: the real adapter reports UNMAPPED on read. */
    Ps3TlsReport r = run(&m, BLOCK, 0x9000, (Ps3TlsTemplate){0x00500000, 4, 0x200}, 2, &out);
    CHECK(r.status == PS3_TLS_MEMORY_FAILURE && r.memory_status == PS3_GUEST_MEMORY_UNMAPPED && r.cleanup_status == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(&out, &old, sizeof out) && !e.port.decommits && !e.port.commits);
    expect_uniform(&e, BLOCK, 0x1000, FILL);
    /* Caller handed storage that is not actually mapped: write fails UNMAPPED; this function does not commit it. */
    reset_counts(&e.port);
    r = run(&m, 0x02800000, 0x9000, (Ps3TlsTemplate){SRC, 4, 0x200}, 2, &out);
    CHECK(r.status == PS3_TLS_MEMORY_FAILURE && r.memory_status == PS3_GUEST_MEMORY_UNMAPPED && !e.port.commits);
    unsigned char probe; CHECK(e.port.real.read(e.port.real.context, 0x02800000, &probe, 1) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(!memcmp(&out, &old, sizeof out));
    env_destroy(&e); ++tests;
}
static void test_independent(void) {
    unsigned char *snap[2];
    for (int order = 0; order < 2; ++order) {
        Env e; env_init(&e); real_commit(&e, BLOCK, 0x30000); real_fill(&e, BLOCK, 0x30000, FILL);
        Ps3TlsImage a, b, c;
        reset_counts(&e.port); ps3_guest_memory m = counting(&e.port);
        Ps3TlsTemplate ta = {SRC + 0x10, 8, 0x100}, tb = {SRC + 0x200, 6, 0x80};
        if (order) CHECK(run(&m, BLOCK + 0x9000, 0x9000, tb, 3, &b).status == PS3_TLS_OK);
        CHECK(run(&m, BLOCK, 0x9000, ta, 5, &a).status == PS3_TLS_OK);
        if (!order) CHECK(run(&m, BLOCK + 0x9000, 0x9000, tb, 3, &b).status == PS3_TLS_OK);
        CHECK(run(&m, BLOCK + 0x12000, 0x8000, (Ps3TlsTemplate){0, 0, 0}, 1, &c).status == PS3_TLS_OK);
        CHECK(a.block == BLOCK && b.block == BLOCK + 0x9000 && c.block == BLOCK + 0x12000);
        CHECK(a.thread_pointer == BLOCK + 0x7000 && b.thread_pointer == BLOCK + 0x9000 + 0x7000 && c.allocation_size == 0x8000);
        expect_region_bytes(&e, a.block, e.src + 0x10, 8);
        expect_uniform(&e, a.block + 8, 0x100 - 8, 0);
        expect_uniform(&e, a.block + 0x100, 0x9000 - 0x100, FILL);   /* A's tail untouched by B */
        expect_region_bytes(&e, b.block, e.src + 0x200, 6);
        expect_uniform(&e, b.block + 6, 0x80 - 6, 0);
        expect_uniform(&e, b.block + 0x80, 0x9000 - 0x80, FILL);
        snap[order] = malloc(0x30000); CHECK(snap[order]); real_read(&e, BLOCK, snap[order], 0x30000);
        CHECK(!e.port.commits && !e.port.decommits);
        env_destroy(&e);
    }
    CHECK(!memcmp(snap[0], snap[1], 0x30000));                       /* call order is irrelevant */
    free(snap[0]); free(snap[1]); ++tests;
}
static void lock(void *p) { CHECK(!pthread_mutex_lock(p)); }
static void unlock(void *p) { CHECK(!pthread_mutex_unlock(p)); }
static void test_user_memory(void) {
    enum { UNIT = 65536, COUNT = 8 };
    Env e; env_init(&e);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_user_memory_slot slots[COUNT]; ps3_user_memory um;
    ps3_guest_memory shared = counting(&e.port);   /* one counter stream for the allocator and the initializer */
    ps3_user_memory_config cfg = {.window_begin = 0x01000000, .window_end = 0x03000000, .quota_bytes = (uint64_t)COUNT * UNIT,
                                  .memory = shared, .guard = {.context = &guard, .lock = lock, .unlock = unlock}};
    CHECK(ps3_user_memory_init(&um, &cfg, slots, COUNT) == PS3_USER_MEMORY_OK);
    uint32_t addr[COUNT]; Ps3TlsImage image[COUNT];
    reset_counts(&e.port);
    Ps3TlsTemplate t = {SRC + 0xe7c, 4, 0x200};
    ps3_user_memory_info info;
    for (unsigned i = 0; i < COUNT; ++i) {
        ps3_user_memory_result ar = ps3_user_memory_allocate(&um, UNIT, UNIT, &addr[i]);
        CHECK(ar.status == PS3_USER_MEMORY_OK && e.port.commits == i + 1);
        CHECK(ps3_user_memory_query(&um, &info) == PS3_USER_MEMORY_OK && info.used_bytes == (uint64_t)(i + 1) * UNIT);
        Ps3TlsReport r = run(&shared, addr[i], UNIT, t, 2, &image[i]);
        CHECK(r.status == PS3_TLS_OK && image[i].block == addr[i] && image[i].allocation_size == 0x9000 && image[i].thread_pointer == addr[i] + 0x7000);
        CHECK(e.port.commits == i + 1 && e.port.decommits == 0);       /* initializer did not commit again */
        CHECK(ps3_user_memory_query(&um, &info) == PS3_USER_MEMORY_OK && info.used_bytes == (uint64_t)(i + 1) * UNIT && info.allocation_count == i + 1);
        expect_region_bytes(&e, addr[i], e.src + 0xe7c, 4);
        expect_uniform(&e, addr[i] + 4, 0x200 - 4, 0);
    }
    uint32_t extra = 77;       /* quota exactly exhausted by the allocations: nothing was charged twice */
    CHECK(ps3_user_memory_allocate(&um, UNIT, UNIT, &extra).status == PS3_USER_MEMORY_QUOTA_EXCEEDED && extra == 77);
    CHECK(info.available_bytes == 0);
    for (unsigned i = 0; i < COUNT; ++i) {   /* release with the original allocation address, not the TLS extent */
        CHECK(ps3_user_memory_free(&um, addr[i]).status == PS3_USER_MEMORY_OK);
        CHECK(e.port.decommits == i + 1);
        unsigned char probe; CHECK(e.port.real.read(e.port.real.context, addr[i], &probe, 1) == PS3_GUEST_MEMORY_UNMAPPED);
        CHECK(ps3_user_memory_free(&um, addr[i]).status == PS3_USER_MEMORY_NOT_OWNED);
    }
    CHECK(ps3_user_memory_query(&um, &info) == PS3_USER_MEMORY_OK && info.used_bytes == 0 && info.allocation_count == 0);
    CHECK(e.port.commits == COUNT && e.port.decommits == COUNT);
    CHECK(ps3_user_memory_cleanup(&um).status == PS3_USER_MEMORY_OK);
    env_destroy(&e); ++tests;
}
int main(void) {
    test_basic(0); test_basic(1);
    test_chunked(); test_capacity(); test_end_and_overflow(); test_alignment(); test_overlap(); test_invalid_inputs();
    test_failure(0, 1); test_failure(0, 2); test_failure(1, 1); test_failure(1, 2); test_failure(2, 1);
    test_real_memory_failures(); test_independent(); test_user_memory();
    printf("TLS_ALLOCATED_PASS tests=%u\n", tests);
    return 0;
}
