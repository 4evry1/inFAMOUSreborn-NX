#define _POSIX_C_SOURCE 200809L
/* M1: blocking wait-any on owner reservation loss (real atomic domain, real
 * pthread wait-port adapter, real coherent host guest backing). */
#include "atomic_domain.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { SPACE = 1u << 20, LINE_A = 0x10000, LINE_B = 0x10080, LINE_C = 0x20000 };

typedef struct {
    ElfTestHostSpace *space;
    ps3_pthread_guard guard;
    ps3_pthread_wait_port port;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[8];
} Fixture;

static void setup(Fixture *f, int attach) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(SPACE);
    CHECK(f->space);
    ps3_guest_memory backing = elf_test_host_space_port(f->space);
    CHECK(backing.commit(backing.context, 0, SPACE) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_pthread_guard_init(&f->guard) == 0);
    CHECK(ps3_pthread_wait_port_init(&f->port) == 0);
    CHECK(ps3_atomic_domain_init(&f->domain, backing, ps3_pthread_guard_port(&f->guard), f->owners, 8, NULL, 0) == PS3_GUEST_MEMORY_OK);
    if (attach) CHECK(ps3_atomic_domain_attach_wait_port(&f->domain, ps3_pthread_wait_port_port(&f->port)) == PS3_GUEST_MEMORY_OK);
}
static void teardown(Fixture *f) {
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->guard);
    elf_test_host_space_destroy(f->space);
}
static void reserve(Fixture *f, ps3_atomic_owner o, uint32_t line) {
    unsigned char bytes[128];
    CHECK(ps3_atomic_load_reserve128(&f->domain, o, line, bytes) == PS3_GUEST_MEMORY_OK);
}
static uint64_t ms_from_now(unsigned ms) { return ps3_pthread_wait_port_now_ns() + (uint64_t)ms * 1000000u; }

typedef struct {
    Fixture *f;
    ps3_atomic_owner owners[4];
    size_t count;
    uint64_t deadline;
    ps3_notify_cancel *cancel;
    ps3_atomic_wait_result result;
    uint64_t mask;
    atomic_int done;
} Waiter;
static void *waiter_main(void *p) {
    Waiter *w = p;
    w->result = ps3_atomic_owners_wait_reservation_loss(&w->f->domain, w->owners, w->count, w->deadline, w->cancel, &w->mask);
    atomic_store(&w->done, 1);
    return NULL;
}
static void wait_until_sleeping(Fixture *f, unsigned calls) {
    while (ps3_pthread_wait_port_sleepers(&f->port) < calls) sched_yield();
}

static void pending_loss_returns_without_sleep(void) {
    Fixture f; setup(&f, 1);
    ps3_atomic_owner o; CHECK(ps3_atomic_owner_acquire(&f.domain, &o) == PS3_GUEST_MEMORY_OK);
    reserve(&f, o, LINE_A);
    unsigned char v = 7;
    CHECK(ps3_atomic_write(&f.domain, LINE_A + 3, &v, 1) == PS3_GUEST_MEMORY_OK);
    uint64_t mask = 0;
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 1, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_LOST);
    CHECK(mask == 1 && ps3_pthread_wait_port_wait_calls(&f.port) == 0);
    /* wait does not consume: the caller does. */
    int lost = 0;
    CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == PS3_GUEST_MEMORY_OK && lost == 1);
    teardown(&f);
}
static void cross_thread_store_wakes(int same_value) {
    Fixture f; setup(&f, 1);
    Waiter w = {.f = &f, .count = 1, .deadline = UINT64_MAX};
    CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[0]) == PS3_GUEST_MEMORY_OK);
    reserve(&f, w.owners[0], LINE_A);
    pthread_t t; CHECK(pthread_create(&t, NULL, waiter_main, &w) == 0);
    wait_until_sleeping(&f, 1);
    CHECK(!atomic_load(&w.done));
    unsigned char v = 0;
    if (!same_value) v = 0x5A;
    CHECK(ps3_atomic_write(&f.domain, LINE_A + 0x7F, &v, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(w.result == PS3_ATOMIC_WAIT_LOST && w.mask == 1);
    teardown(&f);
}
static void nonoverlapping_store_does_not_wake(void) {
    Fixture f; setup(&f, 1);
    Waiter w = {.f = &f, .count = 1};
    CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[0]) == PS3_GUEST_MEMORY_OK);
    reserve(&f, w.owners[0], LINE_A);
    w.deadline = ms_from_now(150);
    pthread_t t; CHECK(pthread_create(&t, NULL, waiter_main, &w) == 0);
    wait_until_sleeping(&f, 1);
    unsigned char v = 1;
    CHECK(ps3_atomic_write(&f.domain, LINE_B, &v, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f.domain, LINE_A - 1, &v, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(w.result == PS3_ATOMIC_WAIT_TIMEOUT && w.mask == 0);
    int lost = 1;
    CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, w.owners[0], &lost) == PS3_GUEST_MEMORY_OK && lost == 0);
    teardown(&f);
}
static void cancel_wakes_blocked_wait(void) {
    Fixture f; setup(&f, 1);
    ps3_notify_cancel cancel = {0};
    Waiter w = {.f = &f, .count = 1, .deadline = UINT64_MAX, .cancel = &cancel};
    CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[0]) == PS3_GUEST_MEMORY_OK);
    reserve(&f, w.owners[0], LINE_A);
    pthread_t t; CHECK(pthread_create(&t, NULL, waiter_main, &w) == 0);
    wait_until_sleeping(&f, 1);
    ps3_notify_port port = ps3_pthread_wait_port_port(&f.port);
    ps3_notify_cancel_request(&cancel, &port);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(w.result == PS3_ATOMIC_WAIT_CANCELLED);
    /* An already-cancelled token never sleeps. */
    uint64_t mask = 9;
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, w.owners, 1, UINT64_MAX, &cancel, &mask) == PS3_ATOMIC_WAIT_CANCELLED);
    CHECK(mask == 0);
    teardown(&f);
}
static void invalid_lanes(void) {
    Fixture f; setup(&f, 0);
    ps3_atomic_owner o, p; uint64_t mask = 5;
    CHECK(ps3_atomic_owner_acquire(&f.domain, &o) == PS3_GUEST_MEMORY_OK);
    reserve(&f, o, LINE_A);
    /* No port attached: blocking is impossible, never spin. */
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 1, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID && mask == 5);
    CHECK(ps3_atomic_domain_attach_wait_port(&f.domain, ps3_pthread_wait_port_port(&f.port)) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_domain_attach_wait_port(&f.domain, ps3_pthread_wait_port_port(&f.port)) == PS3_GUEST_MEMORY_BUSY);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 0, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, NULL, 1, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 1, UINT64_MAX, NULL, NULL) == PS3_ATOMIC_WAIT_INVALID);
    /* Unreserved owner without pending loss can never be woken: INVALID. */
    CHECK(ps3_atomic_owner_acquire(&f.domain, &p) == PS3_GUEST_MEMORY_OK);
    ps3_atomic_owner both[2] = {o, p};
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, both, 2, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID);
    /* Stale generation handle. */
    ps3_atomic_owner stale = p;
    CHECK(ps3_atomic_owner_release(&f.domain, &p) == PS3_GUEST_MEMORY_OK);
    both[1] = stale;
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, both, 2, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID);
    CHECK(ps3_pthread_wait_port_wait_calls(&f.port) == 0);
    teardown(&f);
}
static void wait_any_mask_and_self_commit(void) {
    Fixture f; setup(&f, 1);
    ps3_atomic_owner o[3]; uint64_t mask = 0;
    for (int i = 0; i < 3; ++i) { CHECK(ps3_atomic_owner_acquire(&f.domain, &o[i]) == PS3_GUEST_MEMORY_OK); }
    reserve(&f, o[0], LINE_A); reserve(&f, o[1], LINE_C); reserve(&f, o[2], LINE_A);
    unsigned char v = 3;
    CHECK(ps3_atomic_write(&f.domain, LINE_C + 9, &v, 1) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, o, 3, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_LOST && mask == 2);
    int lost = 0;
    CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, o[1], &lost) == PS3_GUEST_MEMORY_OK && lost);
    reserve(&f, o[1], LINE_C);
    /* Owner 0 commits an unchanged line: self keeps no latch, peer 2 on the same line is latched. */
    unsigned char line[128]; int stored = 0;
    CHECK(ps3_atomic_load_reserve128(&f.domain, o[0], LINE_A, line) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_store_conditional128(&f.domain, o[0], LINE_A, line, &stored) == PS3_GUEST_MEMORY_OK && stored);
    ps3_atomic_owner rest[2] = {o[1], o[2]};
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, rest, 2, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_LOST && mask == 2);
    CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, o[0], &lost) == PS3_GUEST_MEMORY_OK && !lost);
    /* All latched -> full mask (fairness is the caller's rotation over the mask). */
    CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, o[2], &lost) == PS3_GUEST_MEMORY_OK && lost);
    reserve(&f, o[0], LINE_A); reserve(&f, o[2], LINE_A);
    CHECK(ps3_atomic_zero(&f.domain, LINE_A, 0x10080) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, o, 3, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_LOST && mask == 7);
    teardown(&f);
}

static void release_wakes_and_retires_watched_handle(void) {
    Fixture f; setup(&f, 1);
    Waiter w = {.f = &f, .count = 2, .deadline = UINT64_MAX};
    CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[0]) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[1]) == PS3_GUEST_MEMORY_OK);
    reserve(&f, w.owners[0], LINE_A); reserve(&f, w.owners[1], LINE_C);
    ps3_atomic_owner releaser_copy = w.owners[1];      /* waiter array stays immutable */
    pthread_t t; CHECK(pthread_create(&t, NULL, waiter_main, &w) == 0);
    wait_until_sleeping(&f, 1);
    CHECK(ps3_atomic_owner_release(&f.domain, &releaser_copy) == PS3_GUEST_MEMORY_OK);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(w.result == PS3_ATOMIC_WAIT_INVALID);
    teardown(&f);
}
static ps3_notify_wait_result failing_wait(void *c, const uint64_t *w, uint64_t e, const uint32_t *k, uint64_t d) {
    (void)c; (void)w; (void)e; (void)k; (void)d; return PS3_NOTIFY_FAILURE;
}
static void failing_wake(void *c) { (void)c; }
static void port_failure_is_invalid(void) {
    Fixture f; setup(&f, 0);
    ps3_notify_port failing = {&f, failing_wait, failing_wake};
    CHECK(ps3_atomic_domain_attach_wait_port(&f.domain, failing) == PS3_GUEST_MEMORY_OK);
    ps3_atomic_owner o; uint64_t mask = 3;
    CHECK(ps3_atomic_owner_acquire(&f.domain, &o) == PS3_GUEST_MEMORY_OK);
    reserve(&f, o, LINE_A);
    CHECK(ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 1, UINT64_MAX, NULL, &mask) == PS3_ATOMIC_WAIT_INVALID && mask == 3);
    teardown(&f);
}
typedef struct { Fixture *f; ps3_notify_cancel *cancel; atomic_int go; } CancelRace;
static void *cancel_racer(void *p) {
    CancelRace *r = p;
    while (!atomic_load(&r->go)) sched_yield();
    ps3_notify_port port = ps3_pthread_wait_port_port(&r->f->port);
    ps3_notify_cancel_request(r->cancel, &port);
    return NULL;
}
static void *store_racer(void *p) {
    CancelRace *r = p;
    while (!atomic_load(&r->go)) sched_yield();
    unsigned char v = 1;
    CHECK(ps3_atomic_write(&r->f->domain, LINE_A, &v, 1) == PS3_GUEST_MEMORY_OK);
    return NULL;
}
/* Cancel and invalidation racing a sleeper: always returns LOST or CANCELLED, never hangs. */
static void cancel_store_races(unsigned rounds) {
    for (unsigned i = 0; i < rounds; ++i) {
        Fixture f; setup(&f, 1);
        ps3_notify_cancel cancel = {0};
        Waiter w = {.f = &f, .count = 1, .deadline = ms_from_now(5000), .cancel = &cancel};
        CHECK(ps3_atomic_owner_acquire(&f.domain, &w.owners[0]) == PS3_GUEST_MEMORY_OK);
        reserve(&f, w.owners[0], LINE_A);
        CancelRace r = {.f = &f, .cancel = &cancel};
        pthread_t a, b, t;
        CHECK(pthread_create(&t, NULL, waiter_main, &w) == 0);
        CHECK(pthread_create(&a, NULL, cancel_racer, &r) == 0);
        CHECK(pthread_create(&b, NULL, store_racer, &r) == 0);
        atomic_store(&r.go, 1);
        CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0 && pthread_join(t, NULL) == 0);
        CHECK(w.result == PS3_ATOMIC_WAIT_LOST || w.result == PS3_ATOMIC_WAIT_CANCELLED);
        teardown(&f);
    }
}

typedef struct { Fixture *f; atomic_uint stage; unsigned iterations; atomic_uint failures; } Race;
static void *race_storer(void *p) {
    Race *r = p;
    unsigned seed = 12345;
    for (unsigned i = 0; i < r->iterations; ++i) {
        while (atomic_load(&r->stage) != 2 * i + 1) sched_yield();
        seed = seed * 1103515245u + 12345u;
        for (unsigned spin = (seed >> 16) % 64; spin; --spin) sched_yield();
        unsigned char v = (unsigned char)i;
        if (ps3_atomic_write(&r->f->domain, LINE_A + (seed % 128), &v, 1) != PS3_GUEST_MEMORY_OK) atomic_fetch_add(&r->failures, 1);
        atomic_store(&r->stage, 2 * i + 2);
    }
    return NULL;
}
static void randomized_races_never_lose_wakeup(unsigned iterations) {
    Fixture f; setup(&f, 1);
    Race r = {.f = &f, .iterations = iterations};
    ps3_atomic_owner o; CHECK(ps3_atomic_owner_acquire(&f.domain, &o) == PS3_GUEST_MEMORY_OK);
    pthread_t t; CHECK(pthread_create(&t, NULL, race_storer, &r) == 0);
    for (unsigned i = 0; i < iterations; ++i) {
        reserve(&f, o, LINE_A);
        atomic_store(&r.stage, 2 * i + 1);
        uint64_t mask = 0;
        ps3_atomic_wait_result result = ps3_atomic_owners_wait_reservation_loss(&f.domain, &o, 1, ms_from_now(5000), NULL, &mask);
        if (result != PS3_ATOMIC_WAIT_LOST || mask != 1) { fprintf(stderr, "iteration %u result %d\n", i, (int)result); abort(); }
        int lost = 0;
        CHECK(ps3_atomic_owner_take_reservation_loss(&f.domain, o, &lost) == PS3_GUEST_MEMORY_OK && lost);
        while (atomic_load(&r.stage) != 2 * i + 2) sched_yield();
    }
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(atomic_load(&r.failures) == 0);
    teardown(&f);
}

int main(int argc, char **argv) {
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 10000;
    pending_loss_returns_without_sleep();
    cross_thread_store_wakes(0);
    cross_thread_store_wakes(1);
    nonoverlapping_store_does_not_wake();
    cancel_wakes_blocked_wait();
    invalid_lanes();
    wait_any_mask_and_self_commit();
    release_wakes_and_retires_watched_handle();
    port_failure_is_invalid();
    cancel_store_races(200);
    randomized_races_never_lose_wakeup(iterations);
    printf("test_atomic_wait: OK (%u races)\n", iterations);
    return 0;
}
