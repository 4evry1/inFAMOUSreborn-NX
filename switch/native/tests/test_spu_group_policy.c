#define _POSIX_C_SOURCE 200809L
/* spu_resource_policy groups/threads/events with real quota and lv2 queues. */
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../domain/lv2_objects.h"
#include "../domain/spu_resource_policy.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { WINDOW = 0x00400000, WINDOW_END = 0x01400000, QUOTA = 0x00400000 };
typedef struct {
    ElfTestHostSpace *space;
    ps3_pthread_guard g1, g2, g3;
    ps3_pthread_wait_port port;
    ps3_user_memory memory;
    ps3_user_memory_slot slots[2];
    ps3_user_memory_charge_record charges[8];
    ps3_lv2_objects lv2;
    ps3_spu_resource_policy policy;
} Fixture;
static void setup(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(WINDOW_END);
    CHECK(f->space && !ps3_pthread_guard_init(&f->g1) && !ps3_pthread_guard_init(&f->g2) && !ps3_pthread_guard_init(&f->g3) && !ps3_pthread_wait_port_init(&f->port));
    ps3_user_memory_config c = {WINDOW, WINDOW_END, QUOTA, elf_test_host_space_port(f->space), ps3_pthread_guard_port(&f->g1)};
    CHECK(ps3_user_memory_init(&f->memory, &c, f->slots, 2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f->memory, f->charges, 8) == PS3_USER_MEMORY_OK);
    ps3_notify_port port = ps3_pthread_wait_port_port(&f->port);
    CHECK(ps3_lv2_objects_init(&f->lv2, ps3_pthread_guard_port(&f->g2), port) == PS3_LV2_OK);
    CHECK(ps3_spu_resource_policy_init(&f->policy, ps3_pthread_guard_port(&f->g3)) == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_configure(&f->policy, 6, 0).status == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_attach(&f->policy, &f->memory, &f->lv2, port));
}
static uint64_t used(Fixture *f) { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&f->memory, &i) == 0); return i.used_bytes; }
static void init_all(Fixture *f, uint32_t g, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t tid = 0;
        CHECK(ps3_spu_thread_initialize(&f->policy, &tid, g, i, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, (uint64_t)i << 32, 0x7CF100, 2, 16) == PS3_LV2_OK);
        CHECK(tid == ((i << 24) | (g & 0xFFFFFF)));
    }
}
/* L4: a destroyed event queue no longer occupies an SPU port / the EXCEPTION slot (pinned lv2_obj::check()).
 * Liveness is read through the already-attached lv2 (policy -> lv2 only; lv2 has no SPURS dependency). */
static void stale_queue_liveness(Fixture *f) {
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t d = 0, q1 = 0, q2 = 0;
    uint8_t spup = 0xEE;
    CHECK(ps3_spu_group_create(&f->policy, &d, 1, 100, 0, 16) == PS3_LV2_OK);
    init_all(f, d, 1);
    CHECK(ps3_lv2_queue_create(&f->lv2, &q1, &qa, 0, 42) == PS3_LV2_OK && ps3_lv2_queue_create(&f->lv2, &q2, &qa, 0, 42) == PS3_LV2_OK);
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, d, q1, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_OK && spup == 16);
    CHECK(ps3_spu_group_connect_event(&f->policy, d, q1, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_OK);
    /* live bindings keep both resources taken */
    CHECK(ps3_spu_group_connect_event(&f->policy, d, q2, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, d, q2, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_OK && spup == 17);
    CHECK(ps3_spu_group_disconnect_event_all_threads(&f->policy, d, 17) == PS3_LV2_OK);
    /* destroy the queue behind the policy's back: the stale references must be recognised as dead */
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q1) == PS3_LV2_OK);
    spup = 0xEE;
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, d, q2, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_OK && spup == 16);
    CHECK(ps3_spu_group_connect_event(&f->policy, d, q2, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_OK);
    ps3_spu_group_record snap;
    CHECK(ps3_spu_group_snapshot(&f->policy, d, &snap) == PS3_LV2_OK && snap.exception_queue == q2 && snap.threads[0].port_queue[16] == q2);
    /* and the live queue now holds them: a different live queue cannot take the EXCEPTION slot again */
    CHECK(ps3_spu_group_connect_event(&f->policy, d, q2, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_group_destroy(&f->policy, d) == PS3_LV2_OK);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q2) == PS3_LV2_OK);
}
typedef struct { Fixture *f; uint32_t g; uint64_t blocked; uint32_t rc; } Joiner;
static void *joiner(void *p) { Joiner *j = p; j->rc = ps3_spu_group_join(&j->f->policy, j->g, NULL, &j->blocked); return NULL; }

typedef struct { Fixture *f; uint32_t g; uint64_t blocked; uint32_t rc; ps3_notify_cancel cancel; } CJoiner;
static void *cjoiner(void *p) { CJoiner *j = p; j->rc = ps3_spu_group_join(&j->f->policy, j->g, &j->cancel, &j->blocked); return NULL; }
static void wait_blocked(CJoiner *j) { while (!__atomic_load_n(&j->blocked, __ATOMIC_SEQ_CST)) sched_yield(); }
/* B1/I3/I5/L1/L2 on the shared fixture (groups a,b,c live; a is STOPPED). */
static void policy_review(Fixture *f, uint32_t q, uint32_t q2) {
    (void)q2;
    uint32_t x = 0xAAAA, g1, g2, tid;
    ps3_notify_port port = ps3_pthread_wait_port_port(&f->port);
    /* I3: lv2 numbering; RUN/SYSTEM_MODULE legal but unowned -> UNSUPPORTED; others EINVAL */
    CHECK(ps3_spu_group_connect_event(&f->policy, 0x04000200u, q, 1) == PS3_LV2_UNSUPPORTED);
    CHECK(ps3_spu_group_connect_event(&f->policy, 0x04000200u, q, 4) == PS3_LV2_UNSUPPORTED);
    CHECK(ps3_spu_group_connect_event(&f->policy, 0x04000200u, q, 7) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_connect_event(&f->policy, 0x04000200u, q, 2) == PS3_LV2_OK);
    /* I5: legal lv2 group type is UNSUPPORTED (after the EINVAL range checks), nothing charged */
    uint64_t before = used(f);
    CHECK(ps3_spu_group_create(&f->policy, &x, 1, 128, 0x20, 16) == PS3_LV2_UNSUPPORTED && x == 0xAAAA && used(f) == before);
    CHECK(ps3_spu_group_create(&f->policy, &x, 7, 128, 0x20, 16) == PS3_LV2_EINVAL);
    /* L2: a shared guard between policy and lv2 is refused (nested lock) */
    ps3_spu_resource_policy shared;
    memset(&shared, 0, sizeof shared);
    CHECK(ps3_spu_resource_policy_init(&shared, ps3_pthread_guard_port(&f->g2)) == PS3_SPU_POLICY_OK);
    CHECK(!ps3_spu_resource_policy_attach(&shared, &f->memory, &f->lv2, port));
    /* I7 (policy): a second init of live state is refused and changes nothing */
    CHECK(ps3_spu_resource_policy_init(&f->policy, ps3_pthread_guard_port(&f->g3)) == PS3_SPU_POLICY_INVALID_STATE);
    CHECK(ps3_spu_resource_policy_snapshot(&f->policy).current.managed_limit == 6);
    /* B1: join checks, parked joiner pins the record, destroy refused, cancel clears the joiner */
    CHECK(ps3_spu_group_create(&f->policy, &g1, 2, 128, 0, 16) == PS3_LV2_OK);
    CHECK(ps3_spu_group_join(&f->policy, g1, NULL, NULL) == PS3_SPU_ESTAT);        /* NOT_INITIALIZED */
    CHECK(ps3_spu_group_join(&f->policy, g1 + 0x1000, NULL, NULL) == PS3_LV2_ESRCH);
    for (uint32_t i = 0; i < 2; ++i) CHECK(ps3_spu_thread_initialize(&f->policy, &tid, g1, i, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 16) == PS3_LV2_OK);
    CJoiner j1 = {f, g1, 0, 0xFFFF, {0}};
    pthread_t t1; CHECK(pthread_create(&t1, NULL, cjoiner, &j1) == 0);
    wait_blocked(&j1);
    CHECK(ps3_spu_group_join(&f->policy, g1, NULL, NULL) == PS3_LV2_EBUSY);        /* second concurrent joiner */
    CHECK(ps3_spu_group_destroy(&f->policy, g1) == PS3_LV2_EBUSY);                 /* parked joiner pins the record */
    ps3_notify_cancel_request(&j1.cancel, &port);
    CHECK(pthread_join(t1, NULL) == 0 && j1.rc == PS3_LV2_CANCELLED && j1.blocked == 0);
    /* record never recycled under a parked joiner: after cancel destroy works, a new group's stop never completes a dead join */
    CHECK(ps3_spu_group_destroy(&f->policy, g1) == PS3_LV2_OK);
    CHECK(ps3_spu_group_create(&f->policy, &g2, 2, 128, 0, 16) == PS3_LV2_OK && g2 != g1);
    for (uint32_t i = 0; i < 2; ++i) CHECK(ps3_spu_thread_initialize(&f->policy, &tid, g2, i, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 16) == PS3_LV2_OK);
    int token;
    CHECK(ps3_spu_group_mark_running(&f->policy, g2, &token) == PS3_LV2_OK && ps3_spu_group_mark_stopped(&f->policy, g2, &token) == PS3_LV2_OK);
    CHECK(ps3_spu_group_release_executor(&f->policy, g2, &token) == PS3_LV2_OK && ps3_spu_group_destroy(&f->policy, g2) == PS3_LV2_OK);
    /* L1: pinned check_busy -- live 5-wide group (a) blocks shrinking the managed limit to 4 */
    ps3_spu_policy_result r = ps3_spu_resource_policy_configure(&f->policy, 6, 2);
    CHECK(r.status == PS3_SPU_POLICY_BUSY && ps3_spu_resource_policy_snapshot(&f->policy).current.managed_limit == 6);
}
int main(void) {
    Fixture f; setup(&f);
    uint32_t a, b, c, x = 0xAAAA;
    CHECK(ps3_spu_group_create(&f.policy, &x, 7, 128, 0, 16) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_create(&f.policy, &x, 5, 15, 0, 16) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_create(&f.policy, &x, 5, 256, 0, 16) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_create(&f.policy, &x, 5, 128, 0, 0x81) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_create(&f.policy, &x, 0, 128, 0, 16) == PS3_LV2_EINVAL && x == 0xAAAA);
    /* container charge before limits: an over-quota 6-SPU group is ENOMEM even though admissible */
    ps3_user_memory_charge_token hog;
    CHECK(ps3_user_memory_charge(&f.memory, QUOTA - 0x40000, &hog) == 0);
    CHECK(ps3_spu_group_create(&f.policy, &x, 6, 16, 0, 16) == PS3_SPU_ENOMEM && x == 0xAAAA);
    CHECK(ps3_user_memory_uncharge(&f.memory, &hog) == 0);
    /* max rule: (5,128), (1,255), (6,16) coexist; charges 0x140000+0x40000+0x180000 = 0x300000 */
    CHECK(ps3_spu_group_create(&f.policy, &a, 5, 128, 0, 16) == PS3_LV2_OK && a == 0x04000100u);
    CHECK(ps3_spu_group_create(&f.policy, &b, 1, 255, 0, 16) == PS3_LV2_OK && b == 0x04000200u);
    CHECK(ps3_spu_group_create(&f.policy, &c, 6, 16, 0, 16) == PS3_LV2_OK);
    CHECK(used(&f) == 0x300000);
    /* reconfigure to 2 raw -> managed 4: a 5-wide group is now inadmissible (charge rolled back) */
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q, q2; uint8_t spup = 0xEE;
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 42) == PS3_LV2_OK && ps3_lv2_queue_create(&f.lv2, &q2, &qa, 0, 42) == PS3_LV2_OK);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q, 0, &spup) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q, ~UINT64_C(0xFFFF), &spup) == PS3_SPU_ESTAT && spup == 0xEE);
    init_all(&f, a, 4);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q, ~UINT64_C(0xFFFF), &spup) == PS3_SPU_ESTAT);
    uint32_t tid;
    CHECK(ps3_spu_thread_initialize(&f.policy, &tid, a, 3, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 16) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_thread_initialize(&f.policy, &tid, a, 4, 0, 0, 0, 2, 16) == PS3_LV2_EINVAL); /* guest image refused */
    CHECK(ps3_spu_thread_initialize(&f.policy, &tid, a + 0x1000, 4, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 16) == PS3_LV2_ESRCH);
    CHECK(ps3_spu_thread_initialize(&f.policy, &tid, a, 4, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 4ull << 32, 0x7CF100, 2, 16) == PS3_LV2_OK);
    CHECK(ps3_spu_thread_initialize(&f.policy, &tid, a, 5, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 16) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q + 0x100000, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_ESRCH);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_OK && spup == 16);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q2, ~UINT64_C(0xFFFF), &spup) == PS3_LV2_OK && spup == 17);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q2, 1u << 1, &spup) == PS3_LV2_OK && spup == 1);
    CHECK(ps3_spu_group_connect_event_all_threads(&f.policy, a, q2, 1u << 1, &spup) == PS3_LV2_EISCONN);
    ps3_spu_group_record snap;
    CHECK(ps3_spu_group_snapshot(&f.policy, a, &snap) == PS3_LV2_OK && snap.state == PS3_SPU_GROUP_INITIALIZED);
    for (int n = 0; n < 5; ++n) CHECK(snap.threads[n].port_queue[16] == q && snap.threads[n].port_queue[17] == q2);
    /* EXCEPTION: EBUSY checked before the missing-queue ESRCH */
    CHECK(ps3_spu_group_connect_event(&f.policy, a, q, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_OK);
    CHECK(ps3_spu_group_connect_event(&f.policy, a, 0xDEAD, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_group_connect_event(&f.policy, b, 0xDEAD, PS3_SPU_EVENT_EXCEPTION) == PS3_LV2_ESRCH);
    CHECK(ps3_spu_group_connect_event(&f.policy, b, q, 7) == PS3_LV2_EINVAL);
    /* RUNNING + real join wake by stop; destroy refused while running */
    int token_a, token_b;
    CHECK(ps3_spu_group_mark_running(&f.policy, b, &token_b) == PS3_SPU_ESTAT);
    CHECK(ps3_spu_group_mark_running(&f.policy, a, NULL) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_mark_running(&f.policy, a, &token_a) == PS3_LV2_OK);
    CHECK(ps3_spu_group_destroy(&f.policy, a) == PS3_LV2_EBUSY);
    Joiner j = {&f, a, 0, 0xFFFF};
    pthread_t t; CHECK(pthread_create(&t, NULL, joiner, &j) == 0);
    while (!__atomic_load_n(&j.blocked, __ATOMIC_SEQ_CST)) sched_yield();
    CHECK(ps3_spu_group_destroy(&f.policy, a) == PS3_LV2_EBUSY);
    CHECK(ps3_spu_group_mark_stopped(&f.policy, a, &token_b) == PS3_LV2_EINVAL);   /* wrong executor token */
    CHECK(ps3_spu_group_mark_stopped(&f.policy, a, &token_a) == PS3_LV2_OK);
    CHECK(pthread_join(t, NULL) == 0 && j.rc == PS3_LV2_OK && j.blocked == 0);
    /* I4: destroy refuses while the executor binding exists; the charge is still held */
    uint64_t held = used(&f);
    CHECK(ps3_spu_group_destroy(&f.policy, a) == PS3_LV2_EBUSY && used(&f) == held);
    CHECK(ps3_spu_group_release_executor(&f.policy, a, &token_b) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_release_executor(&f.policy, a, &token_a) == PS3_LV2_OK);
    policy_review(&f, q, q2);
    stale_queue_liveness(&f);
    /* destroy releases each charge; quota back to zero */
    CHECK(ps3_spu_group_destroy(&f.policy, a) == PS3_LV2_OK && ps3_spu_group_destroy(&f.policy, b) == PS3_LV2_OK && ps3_spu_group_destroy(&f.policy, c) == PS3_LV2_OK);
    CHECK(used(&f) == 0 && ps3_spu_group_destroy(&f.policy, a) == PS3_LV2_ESRCH);
    CHECK(ps3_spu_resource_policy_configure(&f.policy, 6, 2).status == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_group_create(&f.policy, &x, 5, 128, 0, 16) == PS3_LV2_EINVAL && used(&f) == 0);
    CHECK(ps3_spu_group_create(&f.policy, &x, 4, 128, 0, 16) == PS3_LV2_OK && ps3_spu_group_destroy(&f.policy, x) == PS3_LV2_OK);
    CHECK(ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f.lv2, q2) == PS3_LV2_OK);
    CHECK(ps3_user_memory_cleanup(&f.memory).status == PS3_USER_MEMORY_OK);
    ps3_pthread_wait_port_destroy(&f.port);
    ps3_pthread_guard_destroy(&f.g1); ps3_pthread_guard_destroy(&f.g2); ps3_pthread_guard_destroy(&f.g3);
    elf_test_host_space_destroy(f.space);
    puts("test_spu_group_policy: OK");
    return 0;
}
