#define _POSIX_C_SOURCE 200809L
/* lv2_objects + ppu_threads services with real host threads, real quota. */
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "../domain/lv2_objects.h"
#include "../domain/ppu_threads.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { WINDOW = 0x00400000, WINDOW_END = 0x00800000, QUOTA = 0x00200000,
       CHARGE_RECORDS = 8 };   /* > PS3_PPU_SERVICE_CAP (4 baseline, 6 target): the table, not charge records, is what a full registry hits */
typedef struct {
    ElfTestHostSpace *space;
    ps3_pthread_guard memory_guard, lv2_guard, ppu_guard;
    ps3_pthread_wait_port port;
    ps3_user_memory memory;
    ps3_user_memory_slot slots[2];
    ps3_user_memory_charge_record charges[CHARGE_RECORDS];
    ps3_lv2_objects lv2;
    ps3_ppu_threads ppu;
    ps3_pthread_service_thread threads[3];
} Fixture;
static void setup(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(WINDOW_END);
    CHECK(f->space && !ps3_pthread_guard_init(&f->memory_guard) && !ps3_pthread_guard_init(&f->lv2_guard) &&
          !ps3_pthread_guard_init(&f->ppu_guard) && !ps3_pthread_wait_port_init(&f->port));
    ps3_user_memory_config c = {WINDOW, WINDOW_END, QUOTA, elf_test_host_space_port(f->space), ps3_pthread_guard_port(&f->memory_guard)};
    CHECK(ps3_user_memory_init(&f->memory, &c, f->slots, 2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f->memory, f->charges, CHARGE_RECORDS) == PS3_USER_MEMORY_OK);
    ps3_notify_port port = ps3_pthread_wait_port_port(&f->port);
    CHECK(ps3_lv2_objects_init(&f->lv2, ps3_pthread_guard_port(&f->lv2_guard), port) == PS3_LV2_OK);
    CHECK(ps3_ppu_threads_init(&f->ppu, ps3_pthread_guard_port(&f->ppu_guard), port, &f->memory) == PS3_PPU_OK);
    CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);   /* the real admitted startup main thread id (cpu.thread_id) */
}
static void teardown(Fixture *f) {
    CHECK(ps3_user_memory_cleanup(&f->memory).status == PS3_USER_MEMORY_OK);
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->memory_guard); ps3_pthread_guard_destroy(&f->lv2_guard); ps3_pthread_guard_destroy(&f->ppu_guard);
    elf_test_host_space_destroy(f->space);
}
static uint64_t used(Fixture *f) { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&f->memory, &i) == PS3_USER_MEMORY_OK); return i.used_bytes; }

static void lv2_validation(void) {
    Fixture f; setup(&f);
    ps3_lv2_semaphore_attr prio = {PS3_SYNC_PRIORITY}, bad = {3};
    uint32_t id = 0, first = 0;
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, NULL, 0, 0) == PS3_LV2_EFAULT);   /* EFAULT before EINVAL */
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, &prio, 0, 0) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, &prio, 2, 1) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, &prio, -1, 1) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, &bad, 0, 1) == PS3_LV2_EINVAL);
    CHECK(id == 0);
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &first, &prio, 0, 1) == PS3_LV2_OK && first == 0x96000000u);
    CHECK(ps3_lv2_semaphore_destroy(&f.lv2, first) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &id, &prio, 0, 1) == PS3_LV2_OK && id == 0x96000100u); /* never reused */
    CHECK(ps3_lv2_semaphore_destroy(&f.lv2, first) == PS3_LV2_ESRCH);
    ps3_lv2_objects_limit(&f.lv2, PS3_LV2_KIND_SEMAPHORE, 1);
    uint32_t untouched = 77;
    CHECK(ps3_lv2_semaphore_create(&f.lv2, &untouched, &prio, 0, 1) == PS3_LV2_EAGAIN && untouched == 77);
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q = 0, p = 0, p2 = 0;
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 0) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 128) == PS3_LV2_EINVAL);
    ps3_lv2_queue_attr qbad = {PS3_SYNC_PRIORITY, 3, 0};
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qbad, 0, 42) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 127) == PS3_LV2_OK && q == 0x8D000000u);
    CHECK(ps3_lv2_port_create(&f.lv2, &p, 2, 0) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && p == 0x0E000000u);
    CHECK(ps3_lv2_port_connect_local(&f.lv2, p + 1, q) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_port_send(&f.lv2, p, 1, 2, 3) == PS3_LV2_ENOTCONN);
    CHECK(ps3_lv2_port_connect_local(&f.lv2, p, q) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_connect_local(&f.lv2, p, q) == PS3_LV2_EISCONN);
    CHECK(ps3_lv2_port_create(&f.lv2, &p2, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK);
    /* Owned-or-FENCED delivery: a non-owned event changes nothing. */
    ps3_lv2_event owned = {0, 0, 1, 0};
    CHECK(ps3_lv2_queue_bind_service(&f.lv2, q, owned) == PS3_LV2_OK);
    ps3_lv2_queue before, after;
    CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &before) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&f.lv2, p, 1, 2, 3) == PS3_LV2_FENCED);
    CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &after) == PS3_LV2_OK);
    CHECK(memcmp(&before, &after, sizeof before) == 0);
    CHECK(ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_EISCONN);
    CHECK(ps3_lv2_port_disconnect(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_destroy(&f.lv2, p2) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK);
    CHECK(ps3_lv2_semaphore_destroy(&f.lv2, id) == PS3_LV2_OK);
    CHECK(!ps3_lv2_live_count(&f.lv2, 0) && !ps3_lv2_live_count(&f.lv2, 1) && !ps3_lv2_live_count(&f.lv2, 2));
    teardown(&f);
}

typedef struct { ps3_lv2_objects *lv2; uint32_t queue; ps3_notify_cancel cancel; ps3_lv2_event owned; uint32_t last_rc; } HelperArgs;
static void helper_body(ps3_ppu_service *self, void *argument) {
    HelperArgs *a = argument;
    for (;;) {
        ps3_lv2_event e;
        a->last_rc = ps3_lv2_queue_receive(a->lv2, a->queue, &e, &a->cancel, &self->activity);
        if (a->last_rc != PS3_LV2_OK) { ps3_ppu_service_exit(self, 2); return; }
        if (e.data1 == a->owned.data1 && e.data2 == a->owned.data2 && e.data3 == a->owned.data3) { ps3_ppu_service_exit(self, 0); return; }
    }
}
static void helper_lifecycle(int cancel_instead) {
    Fixture f; setup(&f);
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q, p;
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 42) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && ps3_lv2_port_connect_local(&f.lv2, p, q) == PS3_LV2_OK);
    HelperArgs args = {&f.lv2, q, {0}, {0, 0, 1, 0}, 0xFFFFFFFFu};
    CHECK(ps3_lv2_queue_bind_service(&f.lv2, q, args.owned) == PS3_LV2_OK);
    ps3_ppu_ref helper = {0};
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x8000, ps3_pthread_service_thread_port(&f.threads[0]), helper_body, &args, &helper) == PS3_PPU_OK);
    /* Prefix ran synchronously: the helper is really blocked receiving. */
    CHECK(helper.id == 0x01000000u && ps3_ppu_service_is_blocked(&f.ppu, helper));
    ps3_lv2_queue snap;
    CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &snap) == PS3_LV2_OK && snap.receivers == 1 && snap.blocked_receivers == 1);
    CHECK(used(&f) == 0x8000);
    CHECK(ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_EBUSY);
    CHECK(ps3_lv2_port_send(&f.lv2, p, 9, 9, 9) == PS3_LV2_FENCED && ps3_ppu_service_is_blocked(&f.ppu, helper));
    CHECK(ps3_lv2_queue_unbind_service(&f.lv2, q) == PS3_LV2_EBUSY);            /* I6: guarantee holds while the helper lives */
    CHECK(ps3_lv2_port_send(&f.lv2, p, 9, 9, 9) == PS3_LV2_FENCED);
    if (cancel_instead) { ps3_notify_port port = ps3_pthread_wait_port_port(&f.port); ps3_notify_cancel_request(&args.cancel, &port); }
    else CHECK(ps3_lv2_port_send(&f.lv2, p, 0, 1, 0) == PS3_LV2_OK);
    uint32_t code = 99;
    CHECK(ps3_ppu_service_join(&f.ppu, helper, &code) == PS3_PPU_OK);
    CHECK(code == (cancel_instead ? 2u : 0u) && args.last_rc == (cancel_instead ? PS3_LV2_CANCELLED : PS3_LV2_OK));
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    CHECK(ps3_ppu_service_join(&f.ppu, helper, &code) == PS3_PPU_ESRCH);
    CHECK(ps3_lv2_port_disconnect(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK);
    teardown(&f);
}
static void noop_body(ps3_ppu_service *self, void *argument) { (void)argument; ps3_ppu_service_exit(self, 5); }
static void create_failures(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref s = {0};
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 3072, 250, 0x4000, ps3_pthread_service_thread_port(&f.threads[0]), noop_body, NULL, &s) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, QUOTA + 1, ps3_pthread_service_thread_port(&f.threads[0]), noop_body, NULL, &s) == PS3_PPU_ENOMEM);
    f.threads[0].fail_next_start = 1;
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, ps3_pthread_service_thread_port(&f.threads[0]), noop_body, NULL, &s) == PS3_PPU_EAGAIN);
    ps3_ppu_threads_limit(&f.ppu, 0);
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, ps3_pthread_service_thread_port(&f.threads[0]), noop_body, NULL, &s) == PS3_PPU_EAGAIN);
    CHECK(s.id == 0 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    ps3_ppu_threads_limit(&f.ppu, 4);
    /* stack 0 rounds to 4096; stack 0x4001 rounds to 0x5000; exited service joins. */
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4001, ps3_pthread_service_thread_port(&f.threads[1]), noop_body, NULL, &s) == PS3_PPU_OK);
    CHECK(s.id == 0x01000001u && f.ppu.services[s.slot].stack_bytes == 0x5000);
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, s, &code) == PS3_PPU_OK && code == 5 && used(&f) == 0);
    teardown(&f);
}

/* ---- resource review (root B2/I1/I2/I5/I6/I7) ---- */
static ps3_service_thread_port tp(Fixture *f, int i) { return ps3_pthread_service_thread_port(&f->threads[i]); }
static void unsupported_and_reinit(void) {
    Fixture f; setup(&f);
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q = 0x1234, p = 0x1234;
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 7, 42) == PS3_LV2_UNSUPPORTED && q == 0x1234);   /* legal IPC key: stop, not EINVAL */
    CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_IPC, 0) == PS3_LV2_UNSUPPORTED && p == 0x1234);
    CHECK(ps3_lv2_port_create(&f.lv2, &p, 2, 0) == PS3_LV2_EINVAL);
    CHECK(!ps3_lv2_live_count(&f.lv2, PS3_LV2_KIND_QUEUE) && !ps3_lv2_live_count(&f.lv2, PS3_LV2_KIND_PORT));
    /* A named LOCAL port is a real owned object now (pinned sys_event_port_create stores `name` verbatim, no uniqueness):
     * it is no longer a stop. The full 64-bit name round-trips and a nonzero name is the event source. */
    CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_LOCAL, 0x1000) == PS3_LV2_OK && p == 0x0E000000u);
    ps3_lv2_port snap;
    CHECK(ps3_lv2_port_snapshot(&f.lv2, p, &snap) == PS3_LV2_OK && snap.name == 0x1000 && snap.type == PS3_EVENT_PORT_LOCAL && snap.queue == 0 && snap.active);
    CHECK(ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_port_snapshot(&f.lv2, p, &snap) == PS3_LV2_ESRCH);
    /* I7: a live object is never reset by a second init */
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 42) == PS3_LV2_OK);
    CHECK(ps3_lv2_objects_init(&f.lv2, ps3_pthread_guard_port(&f.lv2_guard), ps3_pthread_wait_port_port(&f.port)) == PS3_LV2_EINVAL);
    CHECK(ps3_lv2_live_count(&f.lv2, PS3_LV2_KIND_QUEUE) == 1);
    CHECK(ps3_ppu_threads_init(&f.ppu, ps3_pthread_guard_port(&f.ppu_guard), ps3_pthread_wait_port_port(&f.port), &f.memory) == PS3_PPU_EINVAL);
    CHECK(ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK);
    teardown(&f);
}
static void ppu_refs(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref s = {0}, a = {0}, b = {0};
    /* I2: equal/lower priority is refused before anything is charged or started */
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 250, 250, 0x4000, tp(&f, 0), noop_body, NULL, &s) == PS3_PPU_UNSUPPORTED);
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 251, 250, 0x4000, tp(&f, 0), noop_body, NULL, &s) == PS3_PPU_UNSUPPORTED);
    CHECK(s.id == 0 && used(&f) == 0 && !f.threads[0].started && ps3_ppu_threads_live(&f.ppu) == 0);
    /* B2: a stale handle can never reach a recycled slot */
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &a) == PS3_PPU_OK);
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, a, &code) == PS3_PPU_OK && used(&f) == 0);
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 1), noop_body, NULL, &b) == PS3_PPU_OK);
    CHECK(b.slot == a.slot && b.id != a.id);                       /* same slot, new id */
    CHECK(ps3_ppu_service_join(&f.ppu, a, &code) == PS3_PPU_ESRCH); /* stale: does not touch the new service */
    CHECK(ps3_ppu_service_is_blocked(&f.ppu, a) == 0);
    CHECK(f.threads[1].started || f.ppu.services[b.slot].life != PS3_PPU_SERVICE_FREE);
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1);
    /* adapter join failure: handle, record and charge stay live; retry joins for real */
    f.threads[1].fail_next_join = 1;
    CHECK(ps3_ppu_service_join(&f.ppu, b, &code) == PS3_PPU_PROVIDER_FAILURE);
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000);
    CHECK(ps3_ppu_service_join(&f.ppu, b, &code) == PS3_PPU_OK && code == 5 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    CHECK(ps3_ppu_service_join(&f.ppu, b, &code) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* L5: the service id namespace is unified with the ACTUAL admitted main thread id. */
static void ppu_main_identity(void) {
    ps3_ppu_ref r = {0};
    uint32_t code = 0;
    {   /* creation is refused until the main id is adopted (no guessed namespace separation) */
        Fixture f; setup(&f);
        ps3_ppu_threads fresh; memset(&fresh, 0, sizeof fresh);
        ps3_notify_port port = ps3_pthread_wait_port_port(&f.port);
        CHECK(ps3_ppu_threads_init(&fresh, ps3_pthread_guard_port(&f.ppu_guard), port, &f.memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_main_id(&fresh) == 0);
        CHECK(ps3_ppu_service_create(&fresh, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EINVAL && r.id == 0 && used(&f) == 0 && !f.threads[0].started);
        CHECK(ps3_ppu_threads_adopt_main(&fresh, 0) == PS3_PPU_EINVAL && ps3_ppu_threads_adopt_main(NULL, 5) == PS3_PPU_EINVAL);
        CHECK(ps3_ppu_threads_adopt_main(&fresh, 7) == PS3_PPU_OK && ps3_ppu_threads_main_id(&fresh) == 7);
        CHECK(ps3_ppu_threads_adopt_main(&fresh, 7) == PS3_PPU_OK);            /* same id again: idempotent */
        CHECK(ps3_ppu_threads_adopt_main(&fresh, 8) == PS3_PPU_EINVAL && ps3_ppu_threads_main_id(&fresh) == 7);
        teardown(&f);
    }
    {   /* adoption after a service exists is refused (its id was allocated without the reservation) */
        Fixture f; memset(&f, 0, sizeof f);
        f.space = elf_test_host_space_create(WINDOW_END);
        CHECK(f.space && !ps3_pthread_guard_init(&f.memory_guard) && !ps3_pthread_guard_init(&f.lv2_guard) &&
              !ps3_pthread_guard_init(&f.ppu_guard) && !ps3_pthread_wait_port_init(&f.port));
        ps3_user_memory_config c = {WINDOW, WINDOW_END, QUOTA, elf_test_host_space_port(f.space), ps3_pthread_guard_port(&f.memory_guard)};
        CHECK(ps3_user_memory_init(&f.memory, &c, f.slots, 2) == PS3_USER_MEMORY_OK);
        CHECK(ps3_user_memory_attach_charges(&f.memory, f.charges, CHARGE_RECORDS) == PS3_USER_MEMORY_OK);
        CHECK(ps3_ppu_threads_init(&f.ppu, ps3_pthread_guard_port(&f.ppu_guard), ps3_pthread_wait_port_port(&f.port), &f.memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_adopt_main(&f.ppu, 1) == PS3_PPU_OK);
        CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_OK && r.id == 0x01000000u);
        CHECK(ps3_ppu_threads_adopt_main(&f.ppu, 0x01000000u) == PS3_PPU_EINVAL && ps3_ppu_threads_main_id(&f.ppu) == 1);
        CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK);
        teardown(&f);
    }
    /* collision cases: the main id sits at the first, a middle and a recycled-slot position of the service namespace */
    static const uint32_t mains[] = {0x01000000u, 0x01000001u, 0x01000003u};
    for (unsigned m = 0; m < 3; ++m) {
        Fixture f; setup(&f);
        ps3_ppu_threads t; memset(&t, 0, sizeof t);
        CHECK(ps3_ppu_threads_init(&t, ps3_pthread_guard_port(&f.ppu_guard), ps3_pthread_wait_port_port(&f.port), &f.memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_threads_adopt_main(&t, mains[m]) == PS3_PPU_OK);
        uint32_t expected = 0x01000000u;
        for (unsigned i = 0; i < 8; ++i) {
            ps3_ppu_ref s = {0};
            CHECK(ps3_ppu_service_create(&t, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &s) == PS3_PPU_OK);
            if (expected == mains[m]) ++expected;
            CHECK(s.id == expected && s.id != mains[m]);
            ++expected;
            CHECK(ps3_ppu_service_join(&t, s, &code) == PS3_PPU_OK);
        }
        teardown(&f);
    }
}
typedef struct { Fixture *f; ps3_ppu_ref ref; uint32_t rc; } Joiner;
static void *join_main(void *p) { Joiner *j = p; uint32_t code; j->rc = ps3_ppu_service_join(&j->f->ppu, j->ref, &code); return NULL; }
static void ppu_double_join(void) {
    for (int round = 0; round < 50; ++round) {
        Fixture f; setup(&f);
        ps3_ppu_ref r = {0};
        CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_OK);
        Joiner j1 = {&f, r, 99}, j2 = {&f, r, 99};
        pthread_t t1, t2;
        CHECK(pthread_create(&t1, NULL, join_main, &j1) == 0 && pthread_create(&t2, NULL, join_main, &j2) == 0);
        CHECK(pthread_join(t1, NULL) == 0 && pthread_join(t2, NULL) == 0);
        int ok = (j1.rc == PS3_PPU_OK) + (j2.rc == PS3_PPU_OK);
        int refused = (j1.rc == PS3_PPU_ESRCH || j1.rc == PS3_PPU_EBUSY) + (j2.rc == PS3_PPU_ESRCH || j2.rc == PS3_PPU_EBUSY);
        CHECK(ok == 1 && refused == 1 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
        teardown(&f);
    }
}
/* I1: a wait-port FAILURE during the synchronous prefix leaves an OWNED running service. */
static ps3_notify_wait_result failing_wait(void *c, const uint64_t *w, uint64_t e, const uint32_t *k, uint64_t d) { (void)c; (void)w; (void)e; (void)k; (void)d; return PS3_NOTIFY_FAILURE; }
static void failing_wake(void *c) { (void)c; }
static void spin_body(ps3_ppu_service *self, void *argument) {
    uint32_t *release = argument;
    while (!__atomic_load_n(release, __ATOMIC_SEQ_CST)) sched_yield();
    ps3_ppu_service_exit(self, 7);
}
static void owned_failure(void) {
    Fixture f; setup(&f);
    ps3_ppu_threads bad;
    memset(&bad, 0, sizeof bad);
    ps3_notify_port port = {NULL, failing_wait, failing_wake};
    CHECK(ps3_ppu_threads_init(&bad, ps3_pthread_guard_port(&f.ppu_guard), port, &f.memory) == PS3_PPU_OK);
    CHECK(ps3_ppu_threads_adopt_main(&bad, 1) == PS3_PPU_OK);
    uint32_t release = 0, code = 0;
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_create(&bad, 1, 249, 250, 0x4000, tp(&f, 0), spin_body, &release, &r) == PS3_PPU_OWNED_FAILURE);
    CHECK(r.id == 0x01000000u && ps3_ppu_threads_live(&bad) == 1 && used(&f) == 0x4000 && f.threads[0].started);
    __atomic_store_n(&release, 1, __ATOMIC_SEQ_CST);                 /* caller stops the body, then joins */
    CHECK(ps3_ppu_service_join(&bad, r, &code) == PS3_PPU_OK && code == 7 && used(&f) == 0 && ps3_ppu_threads_live(&bad) == 0);
    teardown(&f);
}
int main(void) {
    lv2_validation();
    helper_lifecycle(0);
    helper_lifecycle(1);
    create_failures();
    unsupported_and_reinit();
    ppu_refs();
    ppu_double_join();
    owned_failure();
    ppu_main_identity();
    puts("test_lv2_services: OK");
    return 0;
}
