#define _POSIX_C_SOURCE 200809L
/* Mapped guest storage for PPU service records (host test, NOT game execution): ONE real user-memory
 * allocation per guest service = [stack region][TLS region], explicit storage mode, unified release.
 * Real collaborators: user-memory allocator + host mapped space + quota, pthread guards/wait port/service
 * threads. The only doubles are counting/failing wrappers that forward to the real ports.
 * Usage: test_ppu_guest_storage            -> every case
 *        test_ppu_guest_storage <case>     -> one case (red/green demonstration); --list names them. */
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "ppu_threads.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { WINDOW = 0x00400000, WINDOW_END = 0x00800000, QUOTA = 0x00200000, SLOT_COUNT = 8, CHARGE_COUNT = 8, G64 = 0x10000,
       THREAD_COUNT = 6, BODY_EXIT = 7, SENTINEL_CODE = 99 };

/* ---- real memory port wrapped to count, and fail ONCE per armed failure without touching real state ---- */
typedef struct {
    ps3_guest_memory real;
    uint32_t fail_commit, fail_decommit;                 /* armed failure counts (atomic) */
    uint32_t commit_calls, decommit_calls, decommit_ok;  /* atomic */
    uint32_t last_commit_address, last_decommit_address; /* written under the allocator guard */
    uint64_t last_commit_size, last_decommit_size;
} MemorySpy;
static int take_failure(uint32_t *armed) {
    uint32_t n = __atomic_load_n(armed, __ATOMIC_SEQ_CST);
    while (n) if (__atomic_compare_exchange_n(armed, &n, n - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 1;
    return 0;
}
static ps3_guest_memory_status spy_commit(void *c, uint32_t a, uint64_t n) {
    MemorySpy *s = c;
    __atomic_add_fetch(&s->commit_calls, 1, __ATOMIC_SEQ_CST);
    s->last_commit_address = a; s->last_commit_size = n;
    if (take_failure(&s->fail_commit)) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return s->real.commit(s->real.context, a, n);
}
static ps3_guest_memory_status spy_decommit(void *c, uint32_t a, uint64_t n) {
    MemorySpy *s = c;
    __atomic_add_fetch(&s->decommit_calls, 1, __ATOMIC_SEQ_CST);
    s->last_decommit_address = a; s->last_decommit_size = n;
    if (take_failure(&s->fail_decommit)) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;   /* NOT freed: the range stays mapped */
    ps3_guest_memory_status st = s->real.decommit(s->real.context, a, n);
    if (st == PS3_GUEST_MEMORY_OK) __atomic_add_fetch(&s->decommit_ok, 1, __ATOMIC_SEQ_CST);
    return st;
}
static ps3_guest_memory_status spy_read(void *c, uint32_t a, void *p, size_t n) { MemorySpy *s = c; return s->real.read(s->real.context, a, p, n); }
static ps3_guest_memory_status spy_write(void *c, uint32_t a, const void *p, size_t n) { MemorySpy *s = c; return s->real.write(s->real.context, a, p, n); }
static ps3_guest_memory_status spy_zero(void *c, uint32_t a, uint64_t n) { MemorySpy *s = c; return s->real.zero(s->real.context, a, n); }

/* ---- counting wrapper over the real pthread service-thread adapter: one host join must ever succeed ---- */
typedef struct { ps3_pthread_service_thread real; uint32_t start_calls, join_calls, join_ok; } CountedThread;
static int counted_start(void *c, void (*entry)(void *), void *argument) {
    CountedThread *t = c;
    ++t->start_calls;
    return ps3_pthread_service_thread_port(&t->real).start(&t->real, entry, argument);
}
static int counted_join(void *c) {
    CountedThread *t = c;
    ++t->join_calls;
    const int rc = ps3_pthread_service_thread_port(&t->real).join(&t->real);
    if (!rc) ++t->join_ok;
    return rc;
}

typedef struct {
    ElfTestHostSpace *space;
    ps3_pthread_guard memory_guard, ppu_guard;
    ps3_pthread_wait_port port;
    MemorySpy spy;
    ps3_user_memory memory;
    ps3_user_memory_slot slots[SLOT_COUNT];
    ps3_user_memory_charge_record charges[CHARGE_COUNT];
    ps3_ppu_threads ppu;
    CountedThread threads[THREAD_COUNT];
    uint32_t window_end, quota;
} Fixture;
static void setup_with(Fixture *f, uint32_t window_end, uint32_t quota, int adopt_main) {
    memset(f, 0, sizeof *f);
    f->window_end = window_end; f->quota = quota;
    f->space = elf_test_host_space_create(WINDOW_END);
    CHECK(f->space && !ps3_pthread_guard_init(&f->memory_guard) && !ps3_pthread_guard_init(&f->ppu_guard) && !ps3_pthread_wait_port_init(&f->port));
    f->spy.real = elf_test_host_space_port(f->space);
    const ps3_guest_memory spied = {&f->spy, spy_commit, spy_decommit, spy_read, spy_write, spy_zero};
    ps3_user_memory_config c = {WINDOW, window_end, quota, spied, ps3_pthread_guard_port(&f->memory_guard)};
    CHECK(ps3_user_memory_init(&f->memory, &c, f->slots, SLOT_COUNT) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f->memory, f->charges, CHARGE_COUNT) == PS3_USER_MEMORY_OK);
    CHECK(ps3_ppu_threads_init(&f->ppu, ps3_pthread_guard_port(&f->ppu_guard), ps3_pthread_wait_port_port(&f->port), &f->memory) == PS3_PPU_OK);
    if (adopt_main) CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);
}
static void setup(Fixture *f) { setup_with(f, WINDOW_END, QUOTA, 1); }
static void teardown(Fixture *f) {
    CHECK(ps3_user_memory_cleanup(&f->memory).status == PS3_USER_MEMORY_OK);
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->memory_guard); ps3_pthread_guard_destroy(&f->ppu_guard);
    elf_test_host_space_destroy(f->space);
}
static ps3_service_thread_port tp(Fixture *f, int i) { ps3_service_thread_port p = {&f->threads[i], counted_start, counted_join}; return p; }
static ps3_user_memory_info info(Fixture *f) { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&f->memory, &i) == PS3_USER_MEMORY_OK); return i; }
static uint64_t used(Fixture *f) { return info(f).used_bytes; }
static size_t allocations(Fixture *f) { return info(f).allocation_count; }
static unsigned live_charges(Fixture *f) { unsigned n = 0; for (size_t i = 0; i < CHARGE_COUNT; ++i) n += f->charges[i].id != 0; return n; }
static uint64_t life_of(Fixture *f, ps3_ppu_ref r) { return __atomic_load_n(&f->ppu.services[r.slot].life, __ATOMIC_SEQ_CST); }
static void noop_body(ps3_ppu_service *self, void *argument) { (void)argument; ps3_ppu_service_exit(self, BODY_EXIT); }

/* ---- deterministic pattern over real mapped guest bytes ---- */
static void make_pattern(uint8_t *out, size_t n, uint32_t seed) { for (size_t i = 0; i < n; ++i) out[i] = (uint8_t)(seed + i * 31u + (i >> 8)); }
static ps3_guest_memory_status put_pattern(Fixture *f, uint32_t address, size_t n, uint32_t seed) {
    uint8_t *buffer = malloc(n); CHECK(buffer);
    make_pattern(buffer, n, seed);
    const ps3_guest_memory_status st = f->spy.real.write(f->spy.real.context, address, buffer, n);
    free(buffer);
    return st;
}
static int pattern_intact(Fixture *f, uint32_t address, size_t n, uint32_t seed) {
    uint8_t *want = malloc(n), *got = malloc(n); CHECK(want && got);
    make_pattern(want, n, seed);
    const int ok = f->spy.real.read(f->spy.real.context, address, got, n) == PS3_GUEST_MEMORY_OK && !memcmp(want, got, n);
    free(want); free(got);
    return ok;
}
static int is_mapped(Fixture *f, uint32_t address) { uint8_t b; return f->spy.real.read(f->spy.real.context, address, &b, 1) == PS3_GUEST_MEMORY_OK; }
static int all_zero(Fixture *f, uint32_t address, size_t n) {
    uint8_t *got = malloc(n); CHECK(got);
    int ok = f->spy.real.read(f->spy.real.context, address, got, n) == PS3_GUEST_MEMORY_OK;
    for (size_t i = 0; ok && i < n; ++i) ok = got[i] == 0;
    free(got);
    return ok;
}

static ps3_ppu_ref reserve_g(Fixture *f, int thread, ps3_ppu_service_body body, void *argument, uint64_t stack, uint64_t tls) {
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_reserve_guest(&f->ppu, 1, 249, 250, stack, tls, tp(f, thread), body, argument, &r) == PS3_PPU_OK);
    return r;
}
/* Semantic layout equality over ALL seven fields. The struct has padding (u32 next to u64) and the production snapshot does not
 * promise padding bytes, so successful layouts are compared field-by-field, never with memcmp. (memcmp stays only for the
 * no-effects assertions, which DO promise the output bytes are untouched.) */
static int same_layout(const ps3_ppu_guest_storage *a, const ps3_ppu_guest_storage *b) {
    return a->allocation_address == b->allocation_address && a->allocation_bytes == b->allocation_bytes &&
           a->stack_address == b->stack_address && a->stack_logical_bytes == b->stack_logical_bytes &&
           a->stack_region_bytes == b->stack_region_bytes && a->tls_address == b->tls_address &&
           a->tls_capacity_bytes == b->tls_capacity_bytes;
}
static ps3_ppu_guest_storage storage_of(Fixture *f, ps3_ppu_ref r) {
    ps3_ppu_guest_storage g; memset(&g, 0xEE, sizeof g);
    CHECK(ps3_ppu_service_guest_storage(&f->ppu, r, &g) == PS3_PPU_OK);
    return g;
}
/* Layout invariants that must hold for ANY allocator-chosen address (no hardcoded addresses). */
static ps3_ppu_guest_storage expect_layout(Fixture *f, ps3_ppu_ref r, uint64_t logical, uint64_t stack_region, uint64_t tls_capacity) {
    const ps3_ppu_guest_storage g = storage_of(f, r);
    CHECK(g.stack_logical_bytes == logical && g.stack_region_bytes == stack_region && g.tls_capacity_bytes == tls_capacity);
    CHECK(g.allocation_bytes == stack_region + tls_capacity);
    CHECK(g.stack_logical_bytes <= g.stack_region_bytes);
    CHECK(g.stack_region_bytes % G64 == 0 && g.tls_capacity_bytes % G64 == 0 && g.tls_capacity_bytes > 0);
    CHECK(g.allocation_address % G64 == 0 && g.allocation_address >= WINDOW && g.allocation_address + g.allocation_bytes <= f->window_end);
    CHECK(g.stack_address == g.allocation_address);
    CHECK((uint64_t)g.tls_address == (uint64_t)g.allocation_address + g.stack_region_bytes);
    return g;
}

/* ================= 1. exact first request: stack 0x10000 + TLS 0x10000, one allocation, real memory ================= */
static void first_request_layout(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref r = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
    CHECK(r.id == 0x01000000u && life_of(&f, r) == PS3_PPU_SERVICE_RESERVED);
    const ps3_ppu_guest_storage g = expect_layout(&f, r, 0x10000, 0x10000, 0x10000);
    CHECK(g.allocation_bytes == 0x20000);
    CHECK(used(&f) == 0x20000);                   /* exactly ONE charge: a second token/allocation would show 0x40000 */
    CHECK(allocations(&f) == 1 && live_charges(&f) == 0);   /* no separate address-less charge token */
    CHECK(f.spy.commit_calls == 1 && f.spy.last_commit_address == g.allocation_address && f.spy.last_commit_size == 0x20000);
    CHECK(f.spy.decommit_calls == 0 && f.threads[0].start_calls == 0);   /* reserve starts nothing and frees nothing */
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1);
    CHECK(all_zero(&f, g.allocation_address, g.allocation_bytes));        /* really committed, zero filled */
    CHECK(put_pattern(&f, g.stack_address, g.stack_region_bytes, 0x11) == PS3_GUEST_MEMORY_OK);
    CHECK(put_pattern(&f, g.tls_address, g.tls_capacity_bytes, 0x77) == PS3_GUEST_MEMORY_OK);
    CHECK(pattern_intact(&f, g.stack_address, g.stack_region_bytes, 0x11));   /* TLS writes did not clobber the stack */
    CHECK(pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x77));
    uint8_t straddle[32], want[32], a[16], b[16];
    CHECK(f.spy.real.read(f.spy.real.context, g.tls_address - 16, straddle, 32) == PS3_GUEST_MEMORY_OK);   /* contiguous across the seam */
    CHECK(f.spy.real.read(f.spy.real.context, g.tls_address - 16, a, 16) == PS3_GUEST_MEMORY_OK && f.spy.real.read(f.spy.real.context, g.tls_address, b, 16) == PS3_GUEST_MEMORY_OK);
    memcpy(want, a, 16); memcpy(want + 16, b, 16);
    CHECK(!memcmp(straddle, want, 32));
    CHECK(!is_mapped(&f, g.allocation_address + (uint32_t)g.allocation_bytes) && !is_mapped(&f, g.allocation_address - 1));   /* exact extent only */
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_OK);
    CHECK(used(&f) == 0 && allocations(&f) == 0 && f.spy.decommit_calls == 1 && f.spy.decommit_ok == 1);
    CHECK(f.spy.last_decommit_address == g.allocation_address && f.spy.last_decommit_size == 0x20000);
    CHECK(!is_mapped(&f, g.stack_address) && !is_mapped(&f, g.tls_address) && f.threads[0].join_calls == 0);
    teardown(&f);
}

/* ================= 2. rounding: logical 4 KiB, actual regions 64 KiB ================= */
static void rounding_policy(void) {
    static const struct { uint64_t stack, tls, logical, stack_region, tls_capacity; } t[] = {
        {0, 1, 0x1000, G64, G64},                 /* zero stack == one 4 KiB granule (quota-only policy) */
        {1, 1, 0x1000, G64, G64},
        {0x1000, G64, 0x1000, G64, G64},
        {0x10000, 0x10001, 0x10000, G64, 2 * G64},
        {0x10001, 0xFFFF, 0x11000, 2 * G64, G64},
        {0x1F000, 0x30000, 0x1F000, 2 * G64, 3 * G64},
        {0x20000, 0x20000, 0x20000, 2 * G64, 2 * G64},
    };
    Fixture f; setup(&f);
    for (size_t i = 0; i < sizeof t / sizeof *t; ++i) {
        ps3_ppu_ref r = reserve_g(&f, 0, noop_body, NULL, t[i].stack, t[i].tls);
        const ps3_ppu_guest_storage g = expect_layout(&f, r, t[i].logical, t[i].stack_region, t[i].tls_capacity);
        CHECK(used(&f) == g.allocation_bytes && allocations(&f) == 1 && live_charges(&f) == 0);
        CHECK(put_pattern(&f, g.tls_address + (uint32_t)g.tls_capacity_bytes - 1, 1, 0x5A) == PS3_GUEST_MEMORY_OK);   /* last TLS byte is real */
        CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_OK && used(&f) == 0 && allocations(&f) == 0);
    }
    teardown(&f);
}

/* ================= 3. invalid input / overflow / capacity: no effects at all ================= */
static void expect_pristine(Fixture *f) {
    CHECK(used(f) == 0 && allocations(f) == 0 && live_charges(f) == 0 && f->spy.commit_calls == 0 && f->spy.decommit_calls == 0);
    CHECK(ps3_ppu_threads_live(&f->ppu) == 0);
}
#define EXPECT_REJECT(code, ...) do { ps3_ppu_ref r_ = sentinel; \
    CHECK(ps3_ppu_service_reserve_guest(__VA_ARGS__, &r_) == (code)); CHECK(r_.slot == sentinel.slot && r_.id == sentinel.id); expect_pristine(&f); } while (0)
static void invalid_inputs_no_effects(void) {
    Fixture f; setup(&f);
    const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
    ps3_service_thread_port good = tp(&f, 0), nostart = good, nojoin = good;
    nostart.start = NULL; nojoin.join = NULL;
    ps3_ppu_threads cold; memset(&cold, 0, sizeof cold);
    EXPECT_REJECT(PS3_PPU_EINVAL, NULL, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &cold, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, 0x10000, good, NULL, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, 0x10000, nostart, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, 0x10000, nojoin, noop_body, NULL);
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL, NULL) == PS3_PPU_EINVAL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 3072, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_UNSUPPORTED, &f.ppu, 1, 250, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_UNSUPPORTED, &f.ppu, 1, 251, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 3072, 250, 0x10000, 0, good, noop_body, NULL);        /* EINVAL beats UNSUPPORTED */
    EXPECT_REJECT(PS3_PPU_UNSUPPORTED, &f.ppu, 1, 250, 250, 0x10000, 0, good, noop_body, NULL);    /* priority policy precedes TLS validation */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, 0, good, noop_body, NULL);         /* zero TLS capacity is refused, never defaulted */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0, 0, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, QUOTA + 1, 0, good, noop_body, NULL);       /* zero TLS beats allocation-time quota failure */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, UINT64_MAX, 0x10000, good, noop_body, NULL);              /* 4 KiB rounding overflow */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, UINT64_MAX - 0xFFF, 0x10000, good, noop_body, NULL);      /* fits 4 KiB, 64 KiB rounding overflows */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, UINT64_MAX, good, noop_body, NULL);              /* TLS 64 KiB rounding overflow */
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, 0x10000, UINT64_MAX - 0xFFFF + 1, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EINVAL, &f.ppu, 1, 249, 250, UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000), good, noop_body, NULL);   /* region sum overflow */
    EXPECT_REJECT(PS3_PPU_ENOMEM, &f.ppu, 1, 249, 250, UINT64_C(0x4000000000000000), UINT64_C(0x4000000000000000), good, noop_body, NULL);   /* huge but arithmetic-safe: real quota failure */
    EXPECT_REJECT(PS3_PPU_ENOMEM, &f.ppu, 1, 249, 250, QUOTA, G64, good, noop_body, NULL);         /* total just over quota */
    /* capacity/id are decided BEFORE any allocation: no commit even for a request that would exceed quota */
    ps3_ppu_threads_limit(&f.ppu, 0);
    EXPECT_REJECT(PS3_PPU_EAGAIN, &f.ppu, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL);
    EXPECT_REJECT(PS3_PPU_EAGAIN, &f.ppu, 1, 249, 250, QUOTA, G64, good, noop_body, NULL);
    ps3_ppu_threads_limit(&f.ppu, PS3_PPU_SERVICE_CAP);
    f.ppu.serial = 0xFFFFFEu;                                         /* id namespace exhausted */
    EXPECT_REJECT(PS3_PPU_EAGAIN, &f.ppu, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL);
    f.ppu.serial = 0;
    /* a full registry: every slot occupied by a quota-only reservation */
    ps3_ppu_ref held[PS3_PPU_SERVICE_CAP];
    for (size_t i = 0; i < PS3_PPU_SERVICE_CAP; ++i)
        CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x1000, good, noop_body, NULL, &held[i]) == PS3_PPU_OK);
    const uint64_t quota_held = used(&f);
    CHECK(quota_held == PS3_PPU_SERVICE_CAP * 0x1000u && live_charges(&f) == PS3_PPU_SERVICE_CAP && f.spy.commit_calls == 0);
    ps3_ppu_ref r = sentinel;
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, good, noop_body, NULL, &r) == PS3_PPU_EAGAIN);
    CHECK(r.slot == sentinel.slot && r.id == sentinel.id && f.spy.commit_calls == 0 && allocations(&f) == 0 && used(&f) == quota_held);
    for (size_t i = 0; i < PS3_PPU_SERVICE_CAP; ++i) CHECK(ps3_ppu_service_abandon(&f.ppu, held[i]) == PS3_PPU_OK);
    expect_pristine(&f);
    /* none of the rejections consumed an id: only the SERVICE_CAP quota-only reservations did */
    ps3_ppu_ref first = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
    CHECK(first.id == 0x01000000u + PS3_PPU_SERVICE_CAP);
    CHECK(ps3_ppu_service_abandon(&f.ppu, first) == PS3_PPU_OK);
    teardown(&f);
    /* main id not adopted: nothing allocated or consumed */
    Fixture g; setup_with(&g, WINDOW_END, QUOTA, 0);
    ps3_ppu_ref s = sentinel;
    CHECK(ps3_ppu_service_reserve_guest(&g.ppu, 1, 249, 250, 0x10000, 0x10000, tp(&g, 0), noop_body, NULL, &s) == PS3_PPU_EINVAL);
    CHECK(s.slot == sentinel.slot && s.id == sentinel.id && g.spy.commit_calls == 0 && allocations(&g) == 0 && g.ppu.serial == 0);
    teardown(&g);
}

/* ================= 4. allocator status -> precise native PPU errors ================= */
static void allocator_status_mapping(void) {
    const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
    uint32_t scratch = 0;
    /* (a) quota exhaustion -> ENOMEM; the allocator itself reports QUOTA_EXCEEDED for exactly this request */
    {
        Fixture f; setup(&f);
        uint32_t big;
        CHECK(ps3_user_memory_allocate(&f.memory, 0x1F0000, G64, &big).status == PS3_USER_MEMORY_OK);
        CHECK(ps3_user_memory_allocate(&f.memory, 0x20000, G64, &scratch).status == PS3_USER_MEMORY_QUOTA_EXCEEDED);
        ps3_ppu_ref r = sentinel;
        const uint32_t commits = f.spy.commit_calls;
        CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_ENOMEM);
        CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == 0x1F0000 && allocations(&f) == 1 && f.spy.commit_calls == commits && !ps3_ppu_threads_live(&f.ppu));
        CHECK(ps3_user_memory_free(&f.memory, big).status == PS3_USER_MEMORY_OK);
        ps3_ppu_ref ok = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);   /* the failure left nothing behind */
        CHECK(ok.id == 0x01000000u && used(&f) == 0x20000);
        CHECK(ps3_ppu_service_abandon(&f.ppu, ok) == PS3_PPU_OK);
        teardown(&f);
    }
    /* (b) allocator slot table full (CAPACITY_EXCEEDED) is a provider failure, not guest ENOMEM */
    {
        Fixture f; setup(&f);
        uint32_t ext[SLOT_COUNT];
        for (size_t i = 0; i < SLOT_COUNT; ++i) CHECK(ps3_user_memory_allocate(&f.memory, G64, G64, &ext[i]).status == PS3_USER_MEMORY_OK);
        CHECK(ps3_user_memory_allocate(&f.memory, 0x20000, G64, &scratch).status == PS3_USER_MEMORY_CAPACITY_EXCEEDED);
        ps3_ppu_ref r = sentinel;
        const uint32_t commits = f.spy.commit_calls;
        CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_PROVIDER_FAILURE);
        CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == SLOT_COUNT * G64 && f.spy.commit_calls == commits && !ps3_ppu_threads_live(&f.ppu));
        for (size_t i = 0; i < SLOT_COUNT; ++i) CHECK(ps3_user_memory_free(&f.memory, ext[i]).status == PS3_USER_MEMORY_OK);
        teardown(&f);
    }
    /* (c) fragmented window with quota available (NO_SPACE) is a provider failure */
    {
        Fixture f; setup_with(&f, WINDOW + SLOT_COUNT * G64, SLOT_COUNT * G64, 1);
        uint32_t ext[SLOT_COUNT];
        for (size_t i = 0; i < SLOT_COUNT; ++i) CHECK(ps3_user_memory_allocate(&f.memory, G64, G64, &ext[i]).status == PS3_USER_MEMORY_OK);
        for (size_t i = 1; i < SLOT_COUNT; i += 2) CHECK(ps3_user_memory_free(&f.memory, ext[i]).status == PS3_USER_MEMORY_OK);
        CHECK(ps3_user_memory_allocate(&f.memory, 0x20000, G64, &scratch).status == PS3_USER_MEMORY_NO_SPACE);
        ps3_ppu_ref r = sentinel;
        CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_PROVIDER_FAILURE);
        CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == SLOT_COUNT / 2 * G64 && !ps3_ppu_threads_live(&f.ppu));
        for (size_t i = 0; i < SLOT_COUNT; i += 2) CHECK(ps3_user_memory_free(&f.memory, ext[i]).status == PS3_USER_MEMORY_OK);
        teardown(&f);
    }
    /* (d) commit (host mapping) failure is a provider failure; nothing owned, id not consumed */
    {
        Fixture f; setup(&f);
        f.spy.fail_commit = 1;
        ps3_ppu_ref r = sentinel;
        CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, 0x10000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_PROVIDER_FAILURE);
        CHECK(r.slot == sentinel.slot && r.id == sentinel.id && f.spy.commit_calls == 1 && used(&f) == 0 && allocations(&f) == 0 && !ps3_ppu_threads_live(&f.ppu));
        CHECK(f.ppu.serial == 0);
        ps3_ppu_ref ok = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
        CHECK(ok.id == 0x01000000u && used(&f) == 0x20000 && f.spy.commit_calls == 2);
        CHECK(ps3_ppu_service_abandon(&f.ppu, ok) == PS3_PPU_OK);
        teardown(&f);
    }
}

/* ================= 5. quota-only API unchanged, coexisting with guest records ================= */
static void quota_only_unchanged_and_mixed(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref q = {0};
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &q) == PS3_PPU_OK);
    CHECK(used(&f) == 0x4000 && live_charges(&f) == 1 && allocations(&f) == 0 && f.spy.commit_calls == 0);
    ps3_ppu_guest_storage untouched; memset(&untouched, 0xAB, sizeof untouched);
    ps3_ppu_guest_storage probe = untouched;
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, q, &probe) == PS3_PPU_UNSUPPORTED && !memcmp(&probe, &untouched, sizeof probe));   /* owns no address */
    ps3_ppu_ref g = reserve_g(&f, 1, noop_body, NULL, 0x10000, 0x10000);
    CHECK(used(&f) == 0x4000 + 0x20000 && live_charges(&f) == 1 && allocations(&f) == 1);
    const ps3_ppu_guest_storage gs = storage_of(&f, g);
    CHECK(put_pattern(&f, gs.allocation_address, gs.allocation_bytes, 3) == PS3_GUEST_MEMORY_OK);
    /* each release frees only its own resource */
    CHECK(ps3_ppu_service_abandon(&f.ppu, q) == PS3_PPU_OK);
    CHECK(used(&f) == 0x20000 && live_charges(&f) == 0 && allocations(&f) == 1 && f.spy.decommit_calls == 0 && pattern_intact(&f, gs.allocation_address, gs.allocation_bytes, 3));
    /* a quota-only create (real host thread) joins while the guest allocation stays live and intact */
    ps3_ppu_ref c = {0};
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x8000, tp(&f, 2), noop_body, NULL, &c) == PS3_PPU_OK);
    CHECK(used(&f) == 0x20000 + 0x8000 && live_charges(&f) == 1);
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, c, &code) == PS3_PPU_OK && code == BODY_EXIT);
    CHECK(used(&f) == 0x20000 && live_charges(&f) == 0 && allocations(&f) == 1 && f.spy.decommit_calls == 0 && f.threads[2].join_ok == 1);
    CHECK(pattern_intact(&f, gs.allocation_address, gs.allocation_bytes, 3));
    /* quota-only precedence preserved: quota (ENOMEM) before capacity (EAGAIN) */
    ps3_ppu_ref r = {0xDEADu, 0xBEEFu};
    ps3_ppu_threads_limit(&f.ppu, 0);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, QUOTA + 1, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_ENOMEM);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EAGAIN);
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 1, 249, 250, QUOTA, G64, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EAGAIN);   /* guest: capacity first */
    CHECK(r.slot == 0xDEADu && r.id == 0xBEEFu);
    ps3_ppu_threads_limit(&f.ppu, PS3_PPU_SERVICE_CAP);
    CHECK(ps3_ppu_service_abandon(&f.ppu, g) == PS3_PPU_OK && used(&f) == 0 && allocations(&f) == 0 && f.spy.decommit_calls == 1);
    teardown(&f);
}

/* ================= 6. stale refs and snapshot contract ================= */
static void stale_refs_and_snapshot(void) {
    Fixture f; setup(&f);
    ps3_ppu_guest_storage out; memset(&out, 0xCD, sizeof out);
    ps3_ppu_guest_storage keep = out;
    ps3_ppu_ref a = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
    CHECK(ps3_ppu_service_guest_storage(NULL, a, &out) == PS3_PPU_EINVAL && !memcmp(&out, &keep, sizeof out));
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, a, NULL) == PS3_PPU_EINVAL);
    ps3_ppu_threads cold; memset(&cold, 0, sizeof cold);
    CHECK(ps3_ppu_service_guest_storage(&cold, a, &out) == PS3_PPU_EINVAL && !memcmp(&out, &keep, sizeof out));
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, (ps3_ppu_ref){a.slot, 0}, &out) == PS3_PPU_ESRCH);
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, (ps3_ppu_ref){a.slot, a.id + 1}, &out) == PS3_PPU_ESRCH);
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, (ps3_ppu_ref){PS3_PPU_SERVICE_CAP, a.id}, &out) == PS3_PPU_ESRCH);
    CHECK(!memcmp(&out, &keep, sizeof out));
    const ps3_ppu_guest_storage first = storage_of(&f, a);
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_OK);
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, a, &out) == PS3_PPU_ESRCH && !memcmp(&out, &keep, sizeof out));
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_ESRCH && ps3_ppu_service_start(&f.ppu, a) == PS3_PPU_ESRCH);
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, a, &code) == PS3_PPU_ESRCH && code == SENTINEL_CODE && f.spy.decommit_calls == 1);   /* stale join frees nothing */
    ps3_ppu_ref b = reserve_g(&f, 1, noop_body, NULL, 0x20000, 0x10000);   /* slot recycled, id never */
    CHECK(b.slot == a.slot && b.id == a.id + 1);
    const ps3_ppu_guest_storage second = expect_layout(&f, b, 0x20000, 0x20000, 0x10000);
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, a, &out) == PS3_PPU_ESRCH);   /* stale ref cannot read the recycled record */
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_ESRCH && life_of(&f, b) == PS3_PPU_SERVICE_RESERVED && used(&f) == second.allocation_bytes && allocations(&f) == 1);
    CHECK(ps3_ppu_service_abandon(&f.ppu, b) == PS3_PPU_OK);
    (void)first;
    teardown(&f);
}

/* ================= 7. abandon before start; failed abandon retains mapping and quota; start failure retains ================= */
static void abandon_before_start(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref r = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = storage_of(&f, r);
    CHECK(put_pattern(&f, g.allocation_address, g.allocation_bytes, 21) == PS3_GUEST_MEMORY_OK);
    /* start failure leaves the whole reservation, including the mapping, intact */
    f.threads[0].real.fail_next_start = 1;
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_EAGAIN);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_RESERVED && used(&f) == 0x20000 && allocations(&f) == 1 && f.spy.decommit_calls == 0);
    CHECK(pattern_intact(&f, g.allocation_address, g.allocation_bytes, 21));
    /* decommit fails WITHOUT freeing: still owned and still mapped */
    f.spy.fail_decommit = 1;
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_PROVIDER_FAILURE);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_RESERVED && f.spy.decommit_calls == 1 && f.spy.decommit_ok == 0);
    CHECK(used(&f) == 0x20000 && allocations(&f) == 1 && ps3_ppu_threads_live(&f.ppu) == 1);
    CHECK(pattern_intact(&f, g.allocation_address, g.allocation_bytes, 21));
    const ps3_ppu_guest_storage again = storage_of(&f, r);
    CHECK(same_layout(&again, &g));
    /* real retry frees exactly once */
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_OK);
    CHECK(f.spy.decommit_calls == 2 && f.spy.decommit_ok == 1 && f.spy.last_decommit_address == g.allocation_address && f.spy.last_decommit_size == g.allocation_bytes);
    CHECK(used(&f) == 0 && allocations(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !is_mapped(&f, g.allocation_address));
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_ESRCH && f.spy.decommit_calls == 2);
    CHECK(f.threads[0].join_calls == 0 && !f.threads[0].real.started);   /* no host thread ever existed */
    /* abandon of the retained-then-started record after a failed abandon: start still works */
    ps3_ppu_ref s = reserve_g(&f, 1, noop_body, NULL, 0x10000, 0x10000);
    f.spy.fail_decommit = 1;
    CHECK(ps3_ppu_service_abandon(&f.ppu, s) == PS3_PPU_PROVIDER_FAILURE);
    CHECK(ps3_ppu_service_start(&f.ppu, s) == PS3_PPU_OK && life_of(&f, s) == PS3_PPU_SERVICE_EXITED && used(&f) == 0x20000);
    CHECK(ps3_ppu_service_abandon(&f.ppu, s) == PS3_PPU_EINVAL && used(&f) == 0x20000);   /* EXITED is released only by join */
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, s, &code) == PS3_PPU_OK && code == BODY_EXIT && used(&f) == 0 && allocations(&f) == 0);
    teardown(&f);
}

/* ================= 8. real pthread service owns/uses its storage; join (not exit) releases it ================= */
typedef struct {
    Fixture *f; ps3_ppu_ref ref; uint32_t seed, block; uint64_t release; uint32_t ok, visible_id;
    ps3_ppu_guest_storage seen;
} StorageBody;
static void storage_body(ps3_ppu_service *self, void *argument) {
    StorageBody *b = argument;
    ps3_ppu_guest_storage g;
    if (ps3_ppu_service_guest_storage(&b->f->ppu, b->ref, &g) != PS3_PPU_OK) { ps3_ppu_service_exit(self, 90); return; }
    b->seen = g; b->visible_id = self->id;
    b->ok = put_pattern(b->f, g.stack_address, g.stack_region_bytes, b->seed) == PS3_GUEST_MEMORY_OK &&
            put_pattern(b->f, g.tls_address, g.tls_capacity_bytes, b->seed + 1) == PS3_GUEST_MEMORY_OK &&
            pattern_intact(b->f, g.stack_address, g.stack_region_bytes, b->seed) && pattern_intact(b->f, g.tls_address, g.tls_capacity_bytes, b->seed + 1);
    if (b->block) {   /* publish BLOCKED, then really sleep on the wait port until released */
        __atomic_store_n(&self->activity, PS3_PPU_ACTIVITY_BLOCKED, __ATOMIC_SEQ_CST);
        self->owner->port.wake_all(self->owner->port.context);
        while (__atomic_load_n(&b->release, __ATOMIC_SEQ_CST) == 0)
            if (self->owner->port.wait(self->owner->port.context, &b->release, 0, NULL, UINT64_MAX) == PS3_NOTIFY_FAILURE) break;
    }
    ps3_ppu_service_exit(self, BODY_EXIT);
}
static void service_start_join(void) {
    Fixture f; setup(&f);
    StorageBody body = {&f, {0, 0}, 0x30, 0, 0, 0, 0, {0}};
    body.ref = reserve_g(&f, 0, storage_body, &body, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = expect_layout(&f, body.ref, 0x10000, 0x10000, 0x10000);
    CHECK(ps3_ppu_service_start(&f.ppu, body.ref) == PS3_PPU_OK);
    CHECK(body.ok && body.visible_id == body.ref.id && same_layout(&body.seen, &g));   /* the thread saw the SAME storage */
    CHECK(life_of(&f, body.ref) == PS3_PPU_SERVICE_EXITED);
    /* exited but not joined: storage and quota must still be owned (release too early would unmap here) */
    CHECK(f.spy.decommit_calls == 0 && used(&f) == 0x20000 && allocations(&f) == 1 && live_charges(&f) == 0);
    CHECK(pattern_intact(&f, g.stack_address, g.stack_region_bytes, 0x30) && pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x31));
    CHECK(ps3_ppu_service_abandon(&f.ppu, body.ref) == PS3_PPU_EINVAL && f.spy.decommit_calls == 0);
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_OK && code == BODY_EXIT);
    CHECK(f.threads[0].start_calls == 1 && f.threads[0].join_calls == 1 && f.threads[0].join_ok == 1 && !f.threads[0].real.started);
    CHECK(f.spy.decommit_calls == 1 && f.spy.decommit_ok == 1 && f.spy.last_decommit_address == g.allocation_address && f.spy.last_decommit_size == 0x20000);
    CHECK(used(&f) == 0 && allocations(&f) == 0 && !is_mapped(&f, g.stack_address) && !is_mapped(&f, g.tls_address));
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, body.ref, &(ps3_ppu_guest_storage){0}) == PS3_PPU_ESRCH);
    teardown(&f);
}
/* a body that really blocks keeps its storage while RUNNING; the owner wakes it, then joins */
static void service_blocked_then_join(void) {
    Fixture f; setup(&f);
    StorageBody body = {&f, {0, 0}, 0x40, 1, 0, 0, 0, {0}};
    body.ref = reserve_g(&f, 0, storage_body, &body, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = storage_of(&f, body.ref);
    CHECK(ps3_ppu_service_start(&f.ppu, body.ref) == PS3_PPU_OK);
    CHECK(ps3_ppu_service_is_blocked(&f.ppu, body.ref) && life_of(&f, body.ref) == PS3_PPU_SERVICE_RUNNING && body.ok);
    CHECK(f.spy.decommit_calls == 0 && used(&f) == 0x20000 && allocations(&f) == 1);
    CHECK(ps3_ppu_service_abandon(&f.ppu, body.ref) == PS3_PPU_EINVAL && f.spy.decommit_calls == 0);
    CHECK(pattern_intact(&f, g.stack_address, g.stack_region_bytes, 0x40) && pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x41));
    const ps3_ppu_guest_storage live = storage_of(&f, body.ref);   /* snapshot is valid while the service runs */
    CHECK(same_layout(&live, &g));
    __atomic_store_n(&body.release, 1, __ATOMIC_SEQ_CST);
    ps3_pthread_wait_port_port(&f.port).wake_all(&f.port);
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_OK && code == BODY_EXIT);
    CHECK(f.threads[0].join_ok == 1 && f.spy.decommit_ok == 1 && used(&f) == 0 && allocations(&f) == 0);
    teardown(&f);
}

/* ================= 9. host join failure retains the allocation and the ref ================= */
static void join_failure_retains(void) {
    Fixture f; setup(&f);
    StorageBody body = {&f, {0, 0}, 0x50, 0, 0, 0, 0, {0}};
    body.ref = reserve_g(&f, 0, storage_body, &body, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = storage_of(&f, body.ref);
    CHECK(ps3_ppu_service_start(&f.ppu, body.ref) == PS3_PPU_OK);
    f.threads[0].real.fail_next_join = 1;
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_PROVIDER_FAILURE && code == SENTINEL_CODE);
    CHECK(f.threads[0].join_calls == 1 && f.threads[0].join_ok == 0 && f.threads[0].real.started);
    CHECK(life_of(&f, body.ref) == PS3_PPU_SERVICE_EXITED && f.spy.decommit_calls == 0);   /* nothing freed after a failed host join */
    CHECK(used(&f) == 0x20000 && allocations(&f) == 1 && pattern_intact(&f, g.stack_address, g.stack_region_bytes, 0x50) && pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x51));
    const ps3_ppu_guest_storage again = storage_of(&f, body.ref);
    CHECK(same_layout(&again, &g));
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_OK && code == BODY_EXIT);
    CHECK(f.threads[0].join_calls == 2 && f.threads[0].join_ok == 1 && !f.threads[0].real.started);
    CHECK(f.spy.decommit_calls == 1 && f.spy.decommit_ok == 1 && used(&f) == 0 && allocations(&f) == 0);
    teardown(&f);
}

/* ================= 10. decommit failure on join: CLEANUP_PENDING keeps ownership; real retry; ONE host join ================= */
static void decommit_failure_retry(void) {
    Fixture f; setup(&f);
    StorageBody body = {&f, {0, 0}, 0x60, 0, 0, 0, 0, {0}};
    body.ref = reserve_g(&f, 0, storage_body, &body, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = storage_of(&f, body.ref);
    CHECK(ps3_ppu_service_start(&f.ppu, body.ref) == PS3_PPU_OK);
    f.spy.fail_decommit = 2;   /* two wrapper failures that do NOT free, then the real decommit */
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_PROVIDER_FAILURE && code == BODY_EXIT);
    CHECK(life_of(&f, body.ref) == PS3_PPU_SERVICE_CLEANUP_PENDING);
    CHECK(f.threads[0].join_calls == 1 && f.threads[0].join_ok == 1 && !f.threads[0].real.started);
    CHECK(f.spy.decommit_calls == 1 && f.spy.decommit_ok == 0);
    CHECK(used(&f) == 0x20000 && allocations(&f) == 1);   /* quota retained with the mapping */
    CHECK(pattern_intact(&f, g.stack_address, g.stack_region_bytes, 0x60) && pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x61));
    const ps3_ppu_guest_storage pending = storage_of(&f, body.ref);   /* still owned: snapshot valid */
    CHECK(same_layout(&pending, &g));
    CHECK(ps3_ppu_service_abandon(&f.ppu, body.ref) == PS3_PPU_EINVAL && ps3_ppu_service_start(&f.ppu, body.ref) == PS3_PPU_EINVAL);
    CHECK(f.spy.decommit_calls == 1 && used(&f) == 0x20000);
    code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_PROVIDER_FAILURE && code == BODY_EXIT);   /* real exit code kept across retries */
    CHECK(f.threads[0].join_calls == 1 && f.spy.decommit_calls == 2 && f.spy.decommit_ok == 0 && used(&f) == 0x20000 && allocations(&f) == 1);
    CHECK(life_of(&f, body.ref) == PS3_PPU_SERVICE_CLEANUP_PENDING && pattern_intact(&f, g.tls_address, g.tls_capacity_bytes, 0x61));
    code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_OK && code == BODY_EXIT);
    CHECK(f.threads[0].join_calls == 1 && f.threads[0].join_ok == 1);   /* exactly one host join, ever */
    CHECK(f.spy.decommit_calls == 3 && f.spy.decommit_ok == 1 && f.spy.last_decommit_address == g.allocation_address && f.spy.last_decommit_size == g.allocation_bytes);
    CHECK(used(&f) == 0 && allocations(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !is_mapped(&f, g.stack_address) && !is_mapped(&f, g.tls_address));
    CHECK(ps3_ppu_service_join(&f.ppu, body.ref, &code) == PS3_PPU_ESRCH && f.threads[0].join_calls == 1 && f.spy.decommit_calls == 3);
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, body.ref, &(ps3_ppu_guest_storage){0}) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* ================= 11. several guest services: independent, exact accounting, freeing one never touches another ================= */
static void two_services_independent(void) {
    Fixture f; setup(&f);
    StorageBody a = {&f, {0, 0}, 0x10, 0, 0, 0, 0, {0}}, b = {&f, {0, 0}, 0x20, 0, 0, 0, 0, {0}};
    a.ref = reserve_g(&f, 0, storage_body, &a, 0x10000, 0x10000);
    b.ref = reserve_g(&f, 1, storage_body, &b, 0x20000, 0x20000);
    const ps3_ppu_guest_storage ga = expect_layout(&f, a.ref, 0x10000, 0x10000, 0x10000), gb = expect_layout(&f, b.ref, 0x20000, 0x20000, 0x20000);
    CHECK(used(&f) == 0x20000 + 0x40000 && allocations(&f) == 2 && live_charges(&f) == 0);
    CHECK(ga.allocation_address + ga.allocation_bytes <= gb.allocation_address || gb.allocation_address + gb.allocation_bytes <= ga.allocation_address);
    CHECK(ps3_ppu_service_start(&f.ppu, a.ref) == PS3_PPU_OK && ps3_ppu_service_start(&f.ppu, b.ref) == PS3_PPU_OK && a.ok && b.ok);
    uint32_t code = SENTINEL_CODE;
    CHECK(ps3_ppu_service_join(&f.ppu, a.ref, &code) == PS3_PPU_OK);
    CHECK(f.spy.decommit_calls == 1 && f.spy.last_decommit_address == ga.allocation_address && f.spy.last_decommit_size == ga.allocation_bytes);
    CHECK(used(&f) == 0x40000 && allocations(&f) == 1 && !is_mapped(&f, ga.stack_address));
    CHECK(pattern_intact(&f, gb.stack_address, gb.stack_region_bytes, 0x20) && pattern_intact(&f, gb.tls_address, gb.tls_capacity_bytes, 0x21));
    ps3_ppu_ref c = reserve_g(&f, 2, noop_body, NULL, 0x10000, 0x10000);   /* the freed hole is reusable without overlapping b */
    const ps3_ppu_guest_storage gc = expect_layout(&f, c, 0x10000, 0x10000, 0x10000);
    CHECK(gc.allocation_address + gc.allocation_bytes <= gb.allocation_address || gb.allocation_address + gb.allocation_bytes <= gc.allocation_address);
    CHECK(used(&f) == 0x60000 && allocations(&f) == 2);
    CHECK(ps3_ppu_service_abandon(&f.ppu, c) == PS3_PPU_OK && ps3_ppu_service_join(&f.ppu, b.ref, &code) == PS3_PPU_OK);
    CHECK(used(&f) == 0 && allocations(&f) == 0 && f.threads[1].join_ok == 1 && f.threads[0].join_ok == 1 && f.threads[2].join_calls == 0);
    teardown(&f);
}

/* ================= 12. real concurrency: reserve/abandon from several host threads (TSan variant) ================= */
enum { WORKERS = 4, ROUNDS = 40 };
typedef struct { Fixture *f; int index; uint32_t ok, eagain, bad; } Worker;
static void *worker_main(void *p) {
    Worker *w = p;
    for (int i = 0; i < ROUNDS; ++i) {
        ps3_ppu_ref r = {0};
        const uint32_t rc = ps3_ppu_service_reserve_guest(&w->f->ppu, 1, 249, 250, 0x10000, 0x10000, tp(w->f, w->index), noop_body, NULL, &r);
        if (rc == PS3_PPU_EAGAIN) { ++w->eagain; sched_yield(); continue; }
        if (rc != PS3_PPU_OK) { ++w->bad; continue; }
        ps3_ppu_guest_storage g;
        const uint32_t seed = (uint32_t)(w->index * 64 + 1);
        if (ps3_ppu_service_guest_storage(&w->f->ppu, r, &g) != PS3_PPU_OK || put_pattern(w->f, g.stack_address, 256, seed) != PS3_GUEST_MEMORY_OK ||
            put_pattern(w->f, g.tls_address, 256, seed + 1) != PS3_GUEST_MEMORY_OK) ++w->bad;
        sched_yield();   /* hold the reservation while others race */
        if (!pattern_intact(w->f, g.stack_address, 256, seed) || !pattern_intact(w->f, g.tls_address, 256, seed + 1)) ++w->bad;   /* nobody else owns this range */
        if (ps3_ppu_service_abandon(&w->f->ppu, r) != PS3_PPU_OK) ++w->bad;
        ++w->ok;
    }
    return NULL;
}
static void concurrent_reserve_abandon(void) {
    Fixture f; setup(&f);
    ps3_ppu_threads_limit(&f.ppu, 2);   /* fewer slots than workers: capacity rejections must never reach the allocator */
    pthread_t th[WORKERS]; Worker w[WORKERS];
    for (int i = 0; i < WORKERS; ++i) { w[i] = (Worker){&f, i, 0, 0, 0}; CHECK(!pthread_create(&th[i], NULL, worker_main, &w[i])); }
    uint32_t ok = 0, eagain = 0, bad = 0;
    for (int i = 0; i < WORKERS; ++i) { CHECK(!pthread_join(th[i], NULL)); ok += w[i].ok; eagain += w[i].eagain; bad += w[i].bad; }
    CHECK(!bad && ok + eagain == WORKERS * ROUNDS && ok > 0);
    CHECK(f.spy.commit_calls == ok && f.spy.decommit_calls == ok && f.spy.decommit_ok == ok);   /* no allocate-then-fail, no leak */
    CHECK(used(&f) == 0 && allocations(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && live_charges(&f) == 0);
    teardown(&f);
}

/* ================= 13. the comparator ignores padding but sees every field ================= */
static void layout_comparison_ignores_padding(void) {
    ps3_ppu_guest_storage a, b;
    memset(&a, 0x11, sizeof a); memset(&b, 0xEE, sizeof b);   /* different padding bytes, then identical semantic fields */
    a.allocation_address = b.allocation_address = 0x00410000u; a.allocation_bytes = b.allocation_bytes = 0x20000;
    a.stack_address = b.stack_address = 0x00410000u; a.stack_logical_bytes = b.stack_logical_bytes = 0x10000; a.stack_region_bytes = b.stack_region_bytes = 0x10000;
    a.tls_address = b.tls_address = 0x00420000u; a.tls_capacity_bytes = b.tls_capacity_bytes = 0x10000;
    CHECK(sizeof a > 3 * sizeof(uint32_t) + 4 * sizeof(uint64_t) && memcmp(&a, &b, sizeof a) != 0);   /* padding exists, raw bytes differ */
    CHECK(same_layout(&a, &b) && same_layout(&b, &a));
    ps3_ppu_guest_storage c;
    c = b; c.allocation_address ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.allocation_bytes ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.stack_address ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.stack_logical_bytes ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.stack_region_bytes ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.tls_address ^= G64; CHECK(!same_layout(&a, &c));
    c = b; c.tls_capacity_bytes ^= G64; CHECK(!same_layout(&a, &c));
}

/* ================= 14. guest stack membership: [allocation_address, +stack_region_bytes) ================= */
static void guest_stack_region_membership(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref r = reserve_g(&f, 0, noop_body, NULL, 0x10000, 0x10000);
    const ps3_ppu_guest_storage g = expect_layout(&f, r, 0x10000, 0x10000, 0x10000);
    const uint32_t stack_begin = g.allocation_address;
    const uint32_t stack_end = (uint32_t)(g.allocation_address + g.stack_region_bytes);
    CHECK(g.tls_address == stack_end);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_begin) == 1);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_end - 1) == 1);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_begin - 1) == 0);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_end) == 0);
    CHECK(ps3_ppu_threads_is_guest_stack(NULL, stack_begin) == 0);
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_OK);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_begin) == 0);
    CHECK(ps3_ppu_threads_is_guest_stack(&f.ppu, stack_end - 1) == 0);
    teardown(&f);
}

typedef struct { const char *name; void (*run)(void); } Case;
static const Case cases[] = {
    {"first_request_layout", first_request_layout}, {"rounding_policy", rounding_policy},
    {"invalid_inputs_no_effects", invalid_inputs_no_effects}, {"allocator_status_mapping", allocator_status_mapping},
    {"quota_only_unchanged_and_mixed", quota_only_unchanged_and_mixed}, {"stale_refs_and_snapshot", stale_refs_and_snapshot},
    {"abandon_before_start", abandon_before_start}, {"service_start_join", service_start_join},
    {"service_blocked_then_join", service_blocked_then_join}, {"join_failure_retains", join_failure_retains},
    {"decommit_failure_retry", decommit_failure_retry}, {"two_services_independent", two_services_independent},
    {"concurrent_reserve_abandon", concurrent_reserve_abandon},
    {"layout_comparison_ignores_padding", layout_comparison_ignores_padding},
    {"guest_stack_region_membership", guest_stack_region_membership},
};
int main(int argc, char **argv) {
    const size_t n = sizeof cases / sizeof *cases;
    if (argc > 1 && !strcmp(argv[1], "--list")) { for (size_t i = 0; i < n; ++i) puts(cases[i].name); return 0; }
    int ran = 0;
    for (size_t i = 0; i < n; ++i) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        cases[i].run(); printf("PASS %s\n", cases[i].name); fflush(stdout); ++ran;
    }
    if (!ran) { fprintf(stderr, "unknown case\n"); return 2; }
    return 0;
}
