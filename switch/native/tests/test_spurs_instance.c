#define _POSIX_C_SOURCE 200809L
/* S0-S16 transaction over real collaborators: coherent guest memory + atomic domain, shared user-memory
 * quota, lv2 objects, SPU group policy, PPU SERVICE threads on real host threads, the kernel-1 idle
 * service executing on real atomic owners. Expected bytes are hand-derived in expected() from
 * CONTRACT-CLOSURE W0-W36 + KERNEL-IDLE-CONTRACT (never produced by the implementation, never captured).
 * Only the lwmutex/lwcond registries are a boundary double (SyncDouble): the real registries are
 * exercised by the startup bridge. */
#include "atomic_domain.h"
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "../domain/spurs_instance.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { SPACE = 0x00800000, WINDOW = 0x00400000, WINDOW_END = 0x00700000, QUOTA = 0x00300000, B1 = 0x007CF100, B2 = 0x007D0380,
       PAT = 0xA5 };

typedef struct Fixture_ {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard g_mem, g_lv2, g_ppu, g_pol, g_dom;
    ps3_pthread_wait_port port;
    ps3_notify_port notify_underlying;
    uint64_t fail_helper_wait;
    const uint32_t *failure_cancel;
    ps3_user_memory memory; ps3_user_memory_slot slots[2]; ps3_user_memory_charge_record charges[16];
    ps3_atomic_domain domain; ps3_atomic_owner_record records[16];
    ps3_lv2_objects lv2; ps3_ppu_threads ppu; ps3_spu_resource_policy policy;
    ps3_pthread_service_thread th[PS3_SPURS_MAX_INSTANCES][3];
    ps3_spurs_runtime rt;
    unsigned char *ls[PS3_SPURS_LS_SLOTS];
    /* sync double + write sink */
    unsigned mutexes, conds; uint32_t mutex_at[4], cond_at[4];
    uint32_t fail_mutex, fail_cond;
    unsigned writes, fail_write_at, hook_at; void (*hook)(struct Fixture_ *);
    uint64_t hog;
    ps3_user_memory_charge_token hog_token;
} Fixture_;
typedef Fixture_ Fixture;
static const unsigned char MUTEX_IMAGE[24] = {0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7};
static const unsigned char COND_IMAGE[8] = {0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7};

static uint32_t sync_mutex_create(void *c, uint32_t ea, uint32_t protocol, uint32_t recursive) {
    Fixture *f = c;
    CHECK(protocol == 2 && recursive == 0x20);
    if (f->fail_mutex) return f->fail_mutex;
    CHECK(ps3_atomic_write(&f->domain, ea, MUTEX_IMAGE, sizeof MUTEX_IMAGE) == PS3_GUEST_MEMORY_OK);
    f->mutex_at[f->mutexes++] = ea;
    return 0;
}
static uint32_t sync_mutex_destroy(void *c, uint32_t ea) {
    Fixture *f = c;
    for (unsigned i = 0; i < f->mutexes; ++i) if (f->mutex_at[i] == ea) { f->mutex_at[i] = f->mutex_at[--f->mutexes]; return 0; }
    return PS3_LV2_ESRCH;
}
static uint32_t sync_cond_create(void *c, uint32_t ea, uint32_t mutex_ea) {
    Fixture *f = c;
    CHECK(mutex_ea + 0x18 == ea);
    if (f->fail_cond) return f->fail_cond;
    CHECK(ps3_atomic_write(&f->domain, ea, COND_IMAGE, sizeof COND_IMAGE) == PS3_GUEST_MEMORY_OK);
    f->cond_at[f->conds++] = ea;
    return 0;
}
static uint32_t sync_cond_destroy(void *c, uint32_t ea) {
    Fixture *f = c;
    for (unsigned i = 0; i < f->conds; ++i) if (f->cond_at[i] == ea) { f->cond_at[i] = f->cond_at[--f->conds]; return 0; }
    return PS3_LV2_ESRCH;
}
static int sink_write(void *c, uint32_t ea, const void *bytes, size_t n) {
    Fixture *f = c;
    unsigned index = f->writes++;
    if (f->fail_write_at && index + 1 == f->fail_write_at) return -1;
    int rc = ps3_atomic_write(&f->domain, ea, bytes, n) == PS3_GUEST_MEMORY_OK ? 0 : -1;
    if (f->hook && index + 1 == f->hook_at) f->hook(f);
    return rc;
}

static ps3_notify_wait_result fixture_wait(void *context,const uint64_t *word,uint64_t expected,
    const uint32_t *cancel,uint64_t deadline) {
    Fixture *f=context;
    ps3_notify_wait_result result=f->notify_underlying.wait(f->notify_underlying.context,word,expected,cancel,deadline);
    if(__atomic_load_n(&f->fail_helper_wait,__ATOMIC_ACQUIRE)&&cancel==f->failure_cancel&&
        cancel&&!__atomic_load_n(cancel,__ATOMIC_ACQUIRE))return PS3_NOTIFY_FAILURE;
    return result;
}
static void fixture_wake(void *context) {
    Fixture *f=context;f->notify_underlying.wake_all(f->notify_underlying.context);
}
static void setup(Fixture *f, int sink) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(SPACE);
    CHECK(f->space);
    f->backing = elf_test_host_space_port(f->space);
    CHECK(f->backing.commit(f->backing.context, 0, SPACE) == PS3_GUEST_MEMORY_OK);
    CHECK(!ps3_pthread_guard_init(&f->g_mem) && !ps3_pthread_guard_init(&f->g_lv2) && !ps3_pthread_guard_init(&f->g_ppu) &&
          !ps3_pthread_guard_init(&f->g_pol) && !ps3_pthread_guard_init(&f->g_dom) && !ps3_pthread_wait_port_init(&f->port));
    f->notify_underlying=ps3_pthread_wait_port_port(&f->port);
    ps3_notify_port np = {f,fixture_wait,fixture_wake};
    ps3_user_memory_config mc = {WINDOW, WINDOW_END, QUOTA, f->backing, ps3_pthread_guard_port(&f->g_mem)};
    CHECK(ps3_user_memory_init(&f->memory, &mc, f->slots, 2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f->memory, f->charges, 16) == PS3_USER_MEMORY_OK);
    CHECK(ps3_atomic_domain_init(&f->domain, f->backing, ps3_pthread_guard_port(&f->g_dom), f->records, 16, NULL, 0) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_domain_attach_wait_port(&f->domain, np) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_lv2_objects_init(&f->lv2, ps3_pthread_guard_port(&f->g_lv2), np) == PS3_LV2_OK);
    CHECK(ps3_ppu_threads_init(&f->ppu, ps3_pthread_guard_port(&f->g_ppu), np, &f->memory) == PS3_PPU_OK);
    CHECK(ps3_spu_resource_policy_init(&f->policy, ps3_pthread_guard_port(&f->g_pol)) == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_configure(&f->policy, 6, 0).status == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_attach(&f->policy, &f->memory, &f->lv2, np));
    ps3_spurs_runtime_config rc;
    memset(&rc, 0, sizeof rc);
    rc.domain = &f->domain; rc.notify = np; rc.lv2 = &f->lv2; rc.policy = &f->policy; rc.ppu = &f->ppu;
    rc.sync = (ps3_spurs_sync_port){f, sync_mutex_create, sync_mutex_destroy, sync_cond_create, sync_cond_destroy};
    if (sink) rc.writer = (ps3_spurs_write_port){f, sink_write};
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned r = 0; r < 3; ++r) rc.threads[i][r] = ps3_pthread_service_thread_port(&f->th[i][r]);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) { f->ls[i] = malloc(PS3_SPURS_K1_LS_BYTES); CHECK(f->ls[i]); memset(f->ls[i], PAT, PS3_SPURS_K1_LS_BYTES); rc.local_stores[i] = f->ls[i]; }
    rc.creator_priority = 250;
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) != 0 && !f->rt.initialized);   /* L5: no main id adopted -> refused, nothing mutated */
    CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);               /* the admitted main thread (startup cpu.thread_id == 1) */
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) == 0);
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) != 0);           /* one-shot */
    unsigned char fill[0x1200];
    memset(fill, PAT, sizeof fill);
    CHECK(ps3_atomic_write(&f->domain, B1 - 0x100, fill, sizeof fill) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain, B2 - 0x100, fill, sizeof fill) == PS3_GUEST_MEMORY_OK);
}
static uint64_t used(Fixture *f) { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&f->memory, &i) == PS3_USER_MEMORY_OK); return i.used_bytes; }
static uint64_t available(Fixture *f) { ps3_user_memory_info i; CHECK(ps3_user_memory_query(&f->memory, &i) == PS3_USER_MEMORY_OK); return i.available_bytes; }
static void destroy(Fixture *f) {
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) free(f->ls[i]);
    CHECK(ps3_user_memory_cleanup(&f->memory).status == PS3_USER_MEMORY_OK);
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->g_mem); ps3_pthread_guard_destroy(&f->g_lv2); ps3_pthread_guard_destroy(&f->g_ppu);
    ps3_pthread_guard_destroy(&f->g_pol); ps3_pthread_guard_destroy(&f->g_dom);
    elf_test_host_space_destroy(f->space);
}
/* Everything returned: lv2/ppu empty, quota back to the hog only, owners and LS slots free, sync registries empty. */
static void expect_clean(Fixture *f, uint64_t hog) {
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE) == 0 && ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 0 &&
          ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_PORT) == 0);
    CHECK(ps3_ppu_threads_live(&f->ppu) == 0 && used(f) == hog && f->mutexes == 0 && f->conds == 0);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) CHECK(!f->rt.ls_used[i]);
    ps3_atomic_owner all[16];
    for (unsigned i = 0; i < 16; ++i) CHECK(ps3_atomic_owner_acquire(&f->domain, &all[i]) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 16; ++i) CHECK(ps3_atomic_owner_release(&f->domain, &all[i]) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) CHECK(f->rt.instances[i].state == PS3_SPURS_INSTANCE_FREE);
}

/* ---- request builders ---- */
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }
static void attribute(uint8_t *a, uint32_t rev, uint32_t nspus, uint32_t spu_prio, uint32_t ppu_prio, uint32_t prefix_size, uint32_t flags) {
    memset(a, 0, 512);
    p32(a, rev); p32(a + 4, 0x250001); p32(a + 8, nspus); p32(a + 12, spu_prio); p32(a + 16, ppu_prio);
    for (unsigned i = 0; i < 15; ++i) a[0x15 + i] = (uint8_t)('a' + i);
    p32(a + 0x24, prefix_size); p32(a + 0x28, flags);
}
static ps3_spurs_result init(Fixture *f, uint32_t b, const uint8_t *attr) {
    ps3_spurs_request r = {0xD00FF990u, b, attr};
    return ps3_spurs_runtime_initialize(&f->rt, &r);
}
/* ---- independent expected instance image (W0-W36 + kernel idle publication) ---- */
typedef struct { uint32_t base, n, group, queue, port, helper, handler, first_sem; uint32_t spu_prio, ppu_prio, prefix; } Expect;
static void expected(const Expect *e, unsigned char *img) {
    memset(img, 0, 0x1000);
    unsigned char mask = (unsigned char)((1u << e->n) - 1u);
    memset(img + 0x6C, 0xFF, 4); img[0x73] = mask; img[0x76] = (unsigned char)e->n; img[0x77] = 0xFF;
    img[0xB2] = img[0xB3] = 0xFF; memset(img + 0xC0, 0xFF, 8); img[0xC8] = mask; img[0xC9] = 0x10; img[0xCC] = mask;
    for (unsigned i = 0; i < 16; ++i) p64(img + 0x120 + 0x80 * i, 0x96000000u + 0x100u * (e->first_sem + i));
    p64(img + 0x980, 0x96000000u + 0x100u * (e->first_sem + 16));
    p32(img + 0x988, 0xFFFFFFFFu); p32(img + 0x98C, 0xFFFFFFFFu);
    p64(img + 0xD00, 0x100); p32(img + 0xD10, 0x2200); img[0xD14] = 0xFF;
    p64(img + 0xD20, e->handler); p64(img + 0xD28, e->helper);
    p32(img + 0xD30, e->group);
    for (uint32_t n = 0; n < e->n; ++n) p32(img + 0xD34 + 4 * n, (n << 24) | (e->group & 0xFFFFFFu));
    p32(img + 0xD5C, e->queue); p32(img + 0xD60, e->port); p32(img + 0xD68, 1);
    p32(img + 0xD74, 0x818); p32(img + 0xD84, e->spu_prio); p32(img + 0xD88, e->ppu_prio);
    if (e->prefix) { for (unsigned i = 0; i < e->prefix; ++i) img[0xD8C + i] = (unsigned char)('a' + i); img[0xD9B] = (unsigned char)e->prefix; }
    p32(img + 0xDA0, 2); p32(img + 0xDA4, 0x250001);
    memcpy(img + 0xDB0, MUTEX_IMAGE, 24); memcpy(img + 0xDC8, COND_IMAGE, 8);
    p32(img + 0xF04, 0x10); p32(img + 0xF08, 3); p64(img + 0xF10, e->port);
}
static void expect_bytes(Fixture *f, uint32_t b, const unsigned char *want) {
    unsigned char now[0x1000], guard[0x100];
    CHECK(ps3_atomic_read(&f->domain, b, now, 0x1000) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 0x1000; ++i)
        if (now[i] != want[i]) { fprintf(stderr, "B=%X +0x%03X = %02X expected %02X\n", b, i, now[i], want[i]); abort(); }
    CHECK(ps3_atomic_read(&f->domain, b - 0x100, guard, 0x100) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 0x100; ++i) CHECK(guard[i] == PAT);
    CHECK(ps3_atomic_read(&f->domain, b + 0x1000, guard, 0x100) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 0x100; ++i) CHECK(guard[i] == PAT);
}
static void settle(Fixture *f, ps3_spurs_instance *in) {
    /* the OK result already proves quiescence; this checks the kernel service really holds every context blocked */
    CHECK(ps3_spurs_k1_phase_of(&in->kernel) == PS3_SPURS_K1_PHASE_QUIESCENT);
    for (uint32_t i = 0; i < in->nspus; ++i) {
        CHECK(ps3_spurs_k1_context_state_of(&in->kernel, i) == PS3_SPURS_K1_CTX_BLOCKED_LR);
        CHECK(ps3_spurs_k1_counter(&in->kernel, i, PS3_SPURS_K1_COUNT_STEPS) == 0x3FFEu);
        CHECK(ps3_spurs_k1_counter(&in->kernel, i, PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS) == 1);
    }
    (void)f;
}
static void inventory(Fixture *f, ps3_spurs_instance *in, const Expect *e) {
    CHECK(in->state == PS3_SPURS_INSTANCE_ACTIVE && in->semaphore_count == 17);
    for (unsigned i = 0; i < 17; ++i) { int32_t v = -1; CHECK(ps3_lv2_semaphore_get_value(&f->lv2, in->semaphores[i], &v) == PS3_LV2_OK && v == 0); }
    ps3_spu_group_record g;
    CHECK(ps3_spu_group_snapshot(&f->policy, e->group, &g) == PS3_LV2_OK);
    CHECK(g.state == PS3_SPU_GROUP_RUNNING && g.num == e->n && g.executor == &in->kernel && g.joiner == 1 && g.exception_queue == e->queue);
    for (uint32_t n = 0; n < e->n; ++n) CHECK(g.threads[n].active && g.threads[n].port_queue[16] == e->queue && g.threads[n].arg2 == e->base && g.threads[n].arg1 == ((uint64_t)n << 32));
    ps3_lv2_queue q;
    CHECK(ps3_lv2_queue_snapshot(&f->lv2, e->queue, &q) == PS3_LV2_OK && q.receivers == 1 && q.blocked_receivers == 1 && q.service_bound && q.count == 0);
    CHECK(ps3_ppu_service_is_blocked(&f->ppu, in->helper) && ps3_ppu_service_is_blocked(&f->ppu, in->handler));
    CHECK(in->helper.id == e->helper && in->handler.id == e->handler);
    CHECK(__atomic_load_n(&in->handler_status, __ATOMIC_SEQ_CST) == PS3_SPURS_HANDLER_BLOCKED_JOIN);
    /* the helper rejects any non-owned send (FENCED), unchanged */
    CHECK(ps3_lv2_port_send(&f->lv2, e->port, 9, 9, 9) == PS3_LV2_FENCED);
    for (uint32_t n = 0; n < e->n; ++n) CHECK(f->rt.config.local_stores[in->ls_slot[n]][0x1C4] == 0 && in->ls[n][0x1D8 + 3] == 0xFF);
}

static void ok_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a1[512], a2[512];
    attribute(a1, 2, 5, 128, 249, 0, 0);
    attribute(a2, 2, 1, 255, 249, 0, 0);
    CHECK(available(f) == QUOTA);
    ps3_spurs_result r = init(f, B1, a1);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.rc == 0 && r.step == 16 && r.instance == 0);
    CHECK(used(f) == 0x14C000 && available(f) == QUOTA - 0x14C000);          /* 5*256KiB + 0x8000 + 0x4000 */
    Expect e1 = {B1, 5, 0x04000100u, 0x8D000000u, 0x0E000000u, 0x01000000u, 0x01000001u, 0, 128, 249, 0};
    unsigned char want[0x1000];
    expected(&e1, want);
    expect_bytes(f, B1, want);
    inventory(f, &f->rt.instances[0], &e1);
    settle(f, &f->rt.instances[0]);
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE) == 17 && ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 1 &&
          ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_PORT) == 1 && ps3_ppu_threads_live(&f->ppu) == 2);
    /* second observed init (nSpus 1, prio 255) shares the quota: 5+1 = managed 6 */
    r = init(f, B2, a2);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.rc == 0 && r.instance == 1);
    CHECK(used(f) == 0x14C000 + 0x4C000 && available(f) == QUOTA - 0x14C000 - 0x4C000);
    Expect e2 = {B2, 1, 0x04000200u, 0x8D000100u, 0x0E000100u, 0x01000002u, 0x01000003u, 17, 255, 249, 0};
    expected(&e2, want);
    expect_bytes(f, B2, want);
    inventory(f, &f->rt.instances[1], &e2);
    settle(f, &f->rt.instances[1]);
    expected(&e1, want);
    expect_bytes(f, B1, want);                                               /* instance 1 untouched */
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE) == 34 && ps3_ppu_threads_live(&f->ppu) == 4);
    CHECK(f->mutexes == 2 && f->conds == 2);
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt, NULL));
    /* a third init has no instance record: explicit stop before any effect */
    r = init(f, 0x007CE000, a1);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INSTANCE_CAPACITY);
    /* BOTH workers still live: a wake leaving idle is published for the bridge's pre-effect poll */
    unsigned char msg = 0x01;
    CHECK(ps3_atomic_write(&f->domain, B2 + 0x72, &msg, 1) == PS3_GUEST_MEMORY_OK);
    ps3_spurs_stop_info si = {0};
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!ps3_spurs_runtime_poll_stop(&f->rt, &si)) { clock_gettime(CLOCK_MONOTONIC, &t1); CHECK(t1.tv_sec - t0.tv_sec < 5); sched_yield(); }
    CHECK(si.instance == 1 && si.reason == PS3_SPURS_K1_STOP_MESSAGE_WAKE && si.spu == 0);
    CHECK(si.category == PS3_SPURS_STOP_CATEGORY_KERNEL);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);                          /* idempotent */
    /* never-reused ids: a fresh instance after teardown continues the serials */
    r = init(f, B1, a1);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK);
    CHECK(f->rt.instances[0].group == 0x04000300u && f->rt.instances[0].queue == 0x8D000200u);
    destroy(f);
    free(f);
}
static void prefix_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a[512];
    attribute(a, 1, 5, 128, 249, 5, 0);   /* revision 1, prefix "abcde" */
    ps3_spurs_result r = init(f, B1, a);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK);
    Expect e = {B1, 5, 0x04000100u, 0x8D000000u, 0x0E000000u, 0x01000000u, 0x01000001u, 0, 128, 249, 5};
    unsigned char want[0x1000];
    expected(&e, want);
    p32(want + 0xDA0, 1);
    expect_bytes(f, B1, want);
    destroy(f);
    free(f);
}
static void validation_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a[512], untouched[0x1200];
    attribute(a, 2, 5, 128, 249, 0, 0);
    ps3_spurs_request q = {0, B1, a};
    ps3_spurs_result r = ps3_spurs_runtime_initialize(&f->rt, &q);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == 0x80410711u);
    q.attribute_ea = 0xD00FF994u; r = ps3_spurs_runtime_initialize(&f->rt, &q);
    CHECK(r.rc == 0x80410710u && r.outcome == PS3_SPURS_OUTCOME_FAILED);
    q.attribute_ea = 0xD00FF990u;
    attribute(a, 3, 5, 128, 249, 0, 0); r = init(f, B1, a); CHECK(r.rc == 0x80410702u);
    attribute(a, 2, 5, 128, 249, 16, 0); r = init(f, B1, a); CHECK(r.rc == 0x80410702u);
    attribute(a, 2, 5, 128, 249, 0, 0);
    r = init(f, 0, a); CHECK(r.rc == 0x80410711u);
    r = init(f, B1 + 0x40, a); CHECK(r.rc == 0x80410710u);
    r = init(f, 0xD0000000u, a); CHECK(r.rc == 0x80410709u && r.outcome == PS3_SPURS_OUTCOME_FAILED);
    r = init(f, 0x30000000u, a); CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_UNKNOWN_REGION);
    attribute(a, 2, 5, 128, 249, 0, 0x10000000u); r = init(f, B1, a);   /* printf flag: unimplemented -> stop */
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE);
    attribute(a, 2, 5, 128, 249, 0, 0); a[0x14] = 1; r = init(f, B1, a);  /* exitIfNoWork */
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE);
    /* all of the above: no effect at all */
    memset(untouched, PAT, sizeof untouched);
    unsigned char now[0x1200];
    CHECK(ps3_atomic_read(&f->domain, B1 - 0x100, now, sizeof now) == PS3_GUEST_MEMORY_OK && !memcmp(now, untouched, sizeof now));
    expect_clean(f, 0);
    CHECK(f->rt.instances[0].state == PS3_SPURS_INSTANCE_FREE);
    /* nSpus out of lv2 range: lv2 group_create EINVAL is guest-visible -> rollback + FAILED(rc) at S5 (review H1) */
    attribute(a, 2, 7, 128, 249, 0, 0); r = init(f, B1, a);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == PS3_LV2_EINVAL && r.step == 5);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}

/* One lane per failure path (capacity-limited real owners). */
typedef void (*Arm)(Fixture *);
static void arm_sem(Fixture *f) { ps3_lv2_objects_limit(&f->lv2, PS3_LV2_KIND_SEMAPHORE, 5); }
static void arm_sem_prv(Fixture *f) { ps3_lv2_objects_limit(&f->lv2, PS3_LV2_KIND_SEMAPHORE, 16); }
static void arm_quota(Fixture *f) { CHECK(ps3_user_memory_charge(&f->memory, QUOTA - 0x100000, &f->hog_token) == PS3_USER_MEMORY_OK); f->hog = QUOTA - 0x100000; }
static void arm_mutex(Fixture *f) { f->fail_mutex = PS3_LV2_EAGAIN; }
static void arm_cond(Fixture *f) { f->fail_cond = 0x80010004u; }
static void arm_queue(Fixture *f) { ps3_lv2_objects_limit(&f->lv2, PS3_LV2_KIND_QUEUE, 0); }
static void arm_port(Fixture *f) { ps3_lv2_objects_limit(&f->lv2, PS3_LV2_KIND_PORT, 0); }
static void arm_helper_start(Fixture *f) { f->th[0][PS3_SPURS_THREAD_HELPER].fail_next_start = 1; }
static void arm_ppu_limit(Fixture *f) { ps3_ppu_threads_limit(&f->ppu, 1); }
static void arm_helper_quota(Fixture *f) { CHECK(ps3_user_memory_charge(&f->memory, QUOTA - 0x140000 - 0x4000, &f->hog_token) == PS3_USER_MEMORY_OK); f->hog = QUOTA - 0x140000 - 0x4000; }
static void arm_handler_quota(Fixture *f) { CHECK(ps3_user_memory_charge(&f->memory, QUOTA - 0x140000 - 0x8000 - 0x2000, &f->hog_token) == PS3_USER_MEMORY_OK); f->hog = QUOTA - 0x140000 - 0x8000 - 0x2000; }
static void failure_lane(Arm arm, uint32_t rc, uint32_t step) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    arm(f);
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    ps3_spurs_result r = init(f, B1, a);
    if (r.outcome != PS3_SPURS_OUTCOME_FAILED || r.rc != rc || r.step != step) fprintf(stderr, "lane step %u: outcome %d rc %X step %u reason %u\n", step, r.outcome, r.rc, r.step, r.reason);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == rc && r.step == step);
    expect_clean(f, f->hog);
    unsigned char rev[4];                                                    /* reference: instance bytes are NOT restored */
    CHECK(ps3_atomic_read(&f->domain, B1 + 0xDA0, rev, 4) == PS3_GUEST_MEMORY_OK && rev[3] == 2);
    if (f->hog) CHECK(ps3_user_memory_uncharge(&f->memory, &f->hog_token) == PS3_USER_MEMORY_OK);
    destroy(f);
    free(f);
}
static void hook_preset_on_spu(struct Fixture_ *f) { unsigned char v = 0x01; CHECK(ps3_atomic_write(&f->domain, B1 + 0xC8, &v, 1) == PS3_GUEST_MEMORY_OK); }
static void kernel_stop_lane(int worker_start_failure) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 1);
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    if (worker_start_failure) f->th[0][PS3_SPURS_THREAD_KERNEL].fail_next_start = 1;
    else { f->hook = hook_preset_on_spu; f->hook_at = 5; }                 /* injected during S2: sysSrvOnSpu bit already set */
    ps3_spurs_result r = init(f, B1, a);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_KERNEL && r.step == 12);
    CHECK(r.detail == (worker_start_failure ? (uint32_t)PS3_SPURS_K1_STOP_WORKER_START_FAILED : (uint32_t)PS3_SPURS_K1_STOP_ALREADY_INIT_HALT));
    unsigned char ppu0[8];                                                   /* no ppu0, no r3 write: still the sentinel */
    CHECK(ps3_atomic_read(&f->domain, B1 + 0xD20, ppu0, 8) == PS3_GUEST_MEMORY_OK);
    const unsigned char sentinel[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(!memcmp(ppu0, sentinel, 8));
    CHECK(f->rt.instances[0].state == PS3_SPURS_INSTANCE_RETAINED);
    unsigned char eh[4];
    CHECK(ps3_atomic_read(&f->domain, B1 + 0xD68, eh, 4) == PS3_GUEST_MEMORY_OK && !eh[3]);   /* S13+ never ran */
    CHECK(used(f) == 0x14C000);                                              /* retained for diagnostics */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}
static void write_failure_lane(void) {
    unsigned total;
    {   /* count the PPU-side writes of a clean transaction through the sink */
        Fixture *f = calloc(1, sizeof *f);
        setup(f, 1);
        uint8_t a[512];
        attribute(a, 2, 5, 128, 249, 0, 0);
        CHECK(init(f, B1, a).outcome == PS3_SPURS_OUTCOME_OK);
        total = f->writes;
        CHECK(total > 50 && total < 100);
        destroy(f);
        free(f);
    }
    for (unsigned at = 1; at <= total; at += (at < 20 ? 1 : 4)) {   /* sink fails at S2..S15 writes: stop, never CELL_OK */
        Fixture *f = calloc(1, sizeof *f);
        setup(f, 1);
        f->fail_write_at = at;
        uint8_t a[512];
        attribute(a, 2, 5, 128, 249, 0, 0);
        ps3_spurs_result r = init(f, B1, a);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_WRITE_FAILED);
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        expect_clean(f, 0);
        destroy(f);
        free(f);
    }
}
static void join_failure_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    CHECK(init(f, B1, a).outcome == PS3_SPURS_OUTCOME_OK);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join = 1;
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1);                          /* a join failed: nothing is released */
    CHECK(used(f) == 0x14C000 && f->rt.instances[0].state != PS3_SPURS_INSTANCE_FREE && f->rt.instances[0].handler_live);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);                          /* retry completes */
    expect_clean(f, 0);
    f->th[0][PS3_SPURS_THREAD_KERNEL].fail_next_join = 1;
    CHECK(init(f, B1, a).outcome == PS3_SPURS_OUTCOME_OK);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1 && used(f) == 0x14C000 - 0x4000);   /* handler joined (stack returned); kernel join failed: owners/LS/group kept */
    CHECK(f->rt.ls_used[0] && f->rt.instances[0].owners_acquired == 5 && f->rt.instances[0].group_created);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}
/* H1: a later instance whose group_create is rejected (nSpus 7 > managed 6; check_valid is a max rule, not a sum, so 5+2 is legal) -> FAILED(rc), first untouched. */
static void second_instance_einval_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    CHECK(init(f, B1, a).outcome == PS3_SPURS_OUTCOME_OK);
    attribute(a, 2, 7, 128, 249, 0, 0);
    ps3_spurs_result r = init(f, B2, a);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == PS3_LV2_EINVAL && r.step == 5);
    CHECK(f->rt.instances[0].state == PS3_SPURS_INSTANCE_ACTIVE && f->rt.instances[1].state == PS3_SPURS_INSTANCE_FREE);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}
/* M1: S7 provider failures are STOPs (never a CELL rc in guest r3); resource rcs still roll back (failure_lane). */
static void sync_provider_stop_lane(int cond) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    if (cond) f->fail_cond = PS3_LV2_PROVIDER_FAILURE; else f->fail_mutex = PS3_LV2_PROVIDER_FAILURE;
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    ps3_spurs_result r = init(f, B1, a);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_PROVIDER_FAILURE && r.step == 7 && r.rc == 0);
    CHECK(f->rt.instances[0].state == PS3_SPURS_INSTANCE_RETAINED);
    f->fail_mutex = f->fail_cond = 0;
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}
/* M2: a failed owner release is remembered: slot stays used, shutdown keeps reporting stuck until the owner is really released. */
static void owner_release_failure_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f, 0);
    uint8_t a[512];
    attribute(a, 2, 5, 128, 249, 0, 0);
    CHECK(init(f, B1, a).outcome == PS3_SPURS_OUTCOME_OK);
    ps3_spurs_instance *in = &f->rt.instances[0];
    const unsigned slot2 = in->ls_slot[2];
    CHECK(ps3_atomic_owner_release(&f->domain, &in->owners[2]) == PS3_GUEST_MEMORY_OK);   /* makes the instance's own release fail */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1);
    CHECK(f->rt.ls_used[slot2] && in->owners_acquired == 3 && in->state != PS3_SPURS_INSTANCE_FREE);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1);                                       /* the failure is not forgotten on retry */
    CHECK(f->rt.ls_used[slot2] && in->owners_acquired == 3);
    CHECK(ps3_atomic_owner_acquire(&f->domain, &in->owners[2]) == PS3_GUEST_MEMORY_OK);   /* owner is live again: retry can complete */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    expect_clean(f, 0);
    destroy(f);
    free(f);
}

/* Explicit STOP port tests service lifetime/wiring only, never compiled WWS execution. */
static ps3_spurs_policy_execution_result workload_begin_stop(void *context,const ps3_spurs_policy_execution_request *request) {
    (void)context;(void)request;return (ps3_spurs_policy_execution_result){.status=PS3_SPURS_POLICY_STOP};
}
static ps3_spurs_policy_execution_result workload_step_stop(void *context,uint32_t spu,uint64_t hops) {
    (void)context;(void)spu;(void)hops;return (ps3_spurs_policy_execution_result){.status=PS3_SPURS_POLICY_STOP};
}
static ps3_spurs_policy_execution_status workload_release_ready(void *context,uint32_t spu) {
    (void)context;(void)spu;return PS3_SPURS_POLICY_READY;
}
static int reject_workload_write(void *context,const ps3_spurs_registration_event *event) {
    (void)context;return !event->committed;
}
static int accept_workload_event(void *context,const ps3_spurs_registration_event *event) {
    (void)context;(void)event;return 1;
}
static int deny_idle_scope(void *context,uint32_t address,size_t length,unsigned write) {
    unsigned *calls=context;++*calls;(void)address;(void)length;(void)write;return 0;
}
static int accept_idle_event(void *context,const ps3_spurs_idle_event *event) {
    (void)context;(void)event;return 1;
}
static void workload_services_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);ps3_pthread_guard locks[2];ps3_instance_guard guards[2];
    ps3_spurs_policy_execution_port ports[2];
    for(unsigned i=0;i<2;++i){
        CHECK(!ps3_pthread_guard_init(&locks[i]));guards[i]=ps3_pthread_guard_port(&locks[i]);
        ports[i]=(ps3_spurs_policy_execution_port){f,workload_begin_stop,workload_step_stop,workload_step_stop,workload_release_ready,workload_release_ready};
    }
    ps3_spurs_runtime empty={0};CHECK(ps3_spurs_runtime_set_workload_services(&empty,guards,ports)!=0);
    for(unsigned state=PS3_SPURS_INSTANCE_BUILDING;state<=PS3_SPURS_INSTANCE_RETAINED;++state){
        f->rt.instances[0].state=state;CHECK(ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports)!=0);
        CHECK(!f->rt.workload_services_configured);
    }
    f->rt.instances[0].state=PS3_SPURS_INSTANCE_FREE;
    ports[1].release=NULL;CHECK(ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports)!=0);
    ports[1].release=workload_release_ready;ps3_instance_guard saved=guards[0];guards[0]=f->domain.guard;
    CHECK(ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports)!=0);guards[0]=saved;
    CHECK(!ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports));
    CHECK(ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports)!=0);
    guards[0]=(ps3_instance_guard){0};ports[0]=(ps3_spurs_policy_execution_port){0};
    CHECK(f->rt.workload_guards[0].context==&locks[0]&&f->rt.workload_execution[0].context==f);
    unsigned char a[512];attribute(a,2,5,128,249,0,0);ps3_spurs_result initialized=init(f,B1,a);
    CHECK(initialized.outcome==PS3_SPURS_OUTCOME_OK);
    CHECK(f->rt.instances[0].kernel.policy_execution.context==f);
    CHECK(f->rt.instances[0].kernel.policy_execution.begin==workload_begin_stop);
    ps3_spurs_idle_request enabled_idle={B1,0,7};ps3_spurs_idle_result enabled_idle_result;
    unsigned scope_calls=0;
    ps3_spurs_runtime_request_idle_spu(&f->rt,&enabled_idle,(ps3_spurs_idle_memory_scope){&scope_calls,deny_idle_scope},NULL,NULL,NULL,accept_idle_event,&enabled_idle_result);
    CHECK(enabled_idle_result.stop==PS3_SPURS_IDLE_MEMORY&&enabled_idle_result.memory_status==PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(scope_calls==1&&!enabled_idle_result.reads&&!enabled_idle_result.writes);

    ps3_spurs_runtime_request_idle_spu(&f->rt,&enabled_idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,accept_idle_event,&enabled_idle_result);
    CHECK(enabled_idle_result.has_sdk_result&&enabled_idle_result.sdk_result==0x80410705u&&!enabled_idle_result.writes);
    enabled_idle.spurs=B1+0x1000;
    ps3_spurs_runtime_request_idle_spu(&f->rt,&enabled_idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,accept_idle_event,&enabled_idle_result);
    CHECK(enabled_idle_result.stop==PS3_SPURS_IDLE_UNOWNED_INSTANCE&&!enabled_idle_result.reads);

    ps3_spurs_idle_request idle={B1,0,7};ps3_spurs_idle_result idle_result;
    ps3_spurs_runtime_request_idle_spu(&f->rt,&idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,NULL,&idle_result);
    CHECK(idle_result.stop==PS3_SPURS_IDLE_INVALID_ARGUMENT&&!idle_result.writes);
    ps3_atomic_owner borrower;CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_registration_request request={.spurs=B1,.wid_output=0x10000,.policy_module=0x69c880,.size=0x2d80,
        .data=0x404e9300,.min_contention=1,.max_contention=5,.priority={2,2,2,2,2,0,0,0}};
    ps3_spurs_registration_result result;
    request.spurs=B1+0x1000;
    ps3_spurs_runtime_register_workload(&f->rt,&request,borrower,16,NULL,NULL,NULL,accept_workload_event,&result);
    CHECK(result.stop==PS3_SPURS_REGISTRATION_UNOWNED_INSTANCE&&!result.effects);
    request.spurs=B1;
    ps3_spurs_runtime_register_workload(&f->rt,&request,borrower,16,NULL,NULL,NULL,reject_workload_write,&result);
    CHECK(result.stop==PS3_SPURS_REGISTRATION_OBSERVER&&!result.has_sdk_result);
    CHECK(result.effects==PS3_SPURS_REGISTRATION_SLOT_CLAIMED);
    CHECK(f->rt.instances[0].state==PS3_SPURS_INSTANCE_ACTIVE&&f->rt.instances[0].owners_acquired==5);
    CHECK(f->rt.instances[0].ls[0]&&f->rt.instances[0].kernel_initialized);
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    CHECK(f->rt.workload_services_configured); /* contexts remain borrowed, one-shot services cannot be replaced */
    CHECK(ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports)!=0);
    destroy(f);for(unsigned i=0;i<2;++i)CHECK(!ps3_pthread_guard_destroy(&locks[i]));free(f);
}
static void no_workload_services_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);unsigned char a[512];attribute(a,2,5,128,249,0,0);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    ps3_spurs_idle_request idle={B1,0,7};ps3_spurs_idle_result idle_result;
    ps3_spurs_runtime_request_idle_spu(&f->rt,&idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,NULL,&idle_result);
    CHECK(idle_result.stop==PS3_SPURS_IDLE_INVALID_ARGUMENT&&!idle_result.writes);
    ps3_atomic_owner borrower;CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_registration_request request={.spurs=B1,.wid_output=0x10000,.policy_module=0x69c880,.min_contention=1};
    ps3_spurs_registration_result result;
    ps3_spurs_runtime_register_workload(&f->rt,&request,borrower,16,NULL,NULL,NULL,accept_workload_event,&result);
    CHECK(result.stop==PS3_SPURS_REGISTRATION_NO_EXECUTION_SERVICES&&!result.effects&&!result.has_sdk_result);
    ps3_spurs_runtime_request_idle_spu(&f->rt,&idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,accept_idle_event,&idle_result);
    CHECK(idle_result.stop==PS3_SPURS_IDLE_NO_EXECUTION_SERVICES&&!idle_result.reads&&!idle_result.writes);
    idle.spurs=0;
    ps3_spurs_runtime_request_idle_spu(&f->rt,&idle,(ps3_spurs_idle_memory_scope){0},NULL,NULL,NULL,accept_idle_event,&idle_result);
    CHECK(idle_result.has_sdk_result&&idle_result.sdk_result==0x80410711u&&!idle_result.reads);

    request.policy_module=0;
    ps3_spurs_runtime_register_workload(&f->rt,&request,borrower,16,NULL,NULL,NULL,accept_workload_event,&result);
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410811u&&!result.effects);
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);destroy(f);free(f);
}

/* Synthetic producer only: real queue guard/ring/sequence/wake and real helper
 * receive. Public sends remain fenced; no actual SPU exception is claimed. */
static void inject_helper_test_event(Fixture *f,uint32_t queue,ps3_lv2_event event) {
    ps3_lv2_objects *o=&f->lv2;
    o->guard.lock(o->guard.context);
    ps3_lv2_queue *q=NULL;
    for(unsigned i=0;i<PS3_LV2_QUEUE_CAP;++i)
        if(o->queues[i].active&&o->queues[i].id==queue)q=&o->queues[i];
    CHECK(q&&q->service_bound&&q->count<q->size);
    q->ring[(q->head+q->count)%PS3_EVENT_QUEUE_DEPTH_MAX]=event;
    ++q->count;
    __atomic_fetch_add(&q->sequence,1,__ATOMIC_SEQ_CST);
    o->guard.unlock(o->guard.context);
    o->port.wake_all(o->port.context);
}
static ps3_spurs_stop_info wait_runtime_stop(Fixture *f) {
    ps3_spurs_stop_info info={0};struct timespec begin,now;
    clock_gettime(CLOCK_MONOTONIC,&begin);
    while(!ps3_spurs_runtime_poll_stop(&f->rt,&info)) {
        clock_gettime(CLOCK_MONOTONIC,&now);CHECK(now.tv_sec-begin.tv_sec<5);sched_yield();
    }
    return info;
}
static void helper_event_fence_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    ps3_spurs_instance *in=&f->rt.instances[0];
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt,NULL));
    CHECK(ps3_lv2_port_send(&f->lv2,in->port,0x1234,0x5678,0x9abc)==PS3_LV2_FENCED);
    const ps3_lv2_event first={UINT64_C(0x1122334455667788),UINT64_C(0xaabbccddeeff0011),
        UINT64_C(0x2233445566778899),UINT64_C(0xfedcba9876543210)};
    inject_helper_test_event(f,in->queue,first);
    ps3_spurs_stop_info info=wait_runtime_stop(f);
    CHECK(info.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT&&info.reason==PS3_SPURS_STOP_UNEXPECTED_HELPER_EVENT&&info.has_event);
    CHECK(info.instance==0&&info.spurs_ea==B1&&info.queue==in->queue);
    CHECK(!memcmp(&info.event,&first,sizeof first));
    CHECK(info.spu==0&&info.step==0);
    const ps3_lv2_event second={9,8,7,6};
    inject_helper_test_event(f,in->queue,second);
    ps3_lv2_queue q;CHECK(ps3_lv2_queue_snapshot(&f->lv2,in->queue,&q)==PS3_LV2_OK&&q.count==1);
    CHECK(!memcmp(&q.ring[q.head],&second,sizeof second));
    ps3_spurs_stop_info repeat={0};CHECK(ps3_spurs_runtime_poll_stop(&f->rt,&repeat));
    CHECK(!memcmp(&repeat.event,&first,sizeof first));
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;
    CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(in->state!=PS3_SPURS_INSTANCE_FREE&&in->owners_acquired==5&&in->ls[0]);
    CHECK(__atomic_load_n(&f->rt.helper_stops[0].ready,__ATOMIC_ACQUIRE));
    CHECK(!memcmp(&f->rt.helper_stops[0].event,&first,sizeof first));
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));expect_clean(f,0);
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt,NULL));
    CHECK(!memcmp(&f->rt.helper_stops[0].event,&first,sizeof first));
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    CHECK(!__atomic_load_n(&f->rt.helper_stops[0].ready,__ATOMIC_ACQUIRE));
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt,NULL));
    /* Exact shutdown marker still exits without becoming an unexpected event. */
    CHECK(ps3_lv2_port_send(&f->lv2,f->rt.instances[0].port,0,1,0)==PS3_LV2_OK);
    uint32_t helper_exit=UINT32_MAX;
    CHECK(ps3_ppu_service_join(&f->ppu,f->rt.instances[0].helper,&helper_exit)==PS3_PPU_OK&&helper_exit==0);
    f->rt.instances[0].helper_live=0; /* This test confirmed the owned helper join. */
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));expect_clean(f,0);
    CHECK(!__atomic_load_n(&f->rt.helper_stops[0].ready,__ATOMIC_ACQUIRE));
    destroy(f);free(f);
}
static int accept_exception_event(void *context,const ps3_spurs_exception_event *event) {
    unsigned *calls=context;if(calls)++*calls;(void)event;return 1;
}
static void configure_exception_services(Fixture *f,ps3_pthread_guard locks[2]) {
    ps3_instance_guard guards[2];ps3_spurs_policy_execution_port ports[2];
    for(unsigned i=0;i<2;++i) {
        CHECK(!ps3_pthread_guard_init(&locks[i]));guards[i]=ps3_pthread_guard_port(&locks[i]);
        ports[i]=(ps3_spurs_policy_execution_port){f,workload_begin_stop,workload_step_stop,workload_step_stop,workload_release_ready,workload_release_ready};
    }
    CHECK(!ps3_spurs_runtime_set_workload_services(&f->rt,guards,ports));
}
static void exception_wrapper_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);unsigned observed=0;
    ps3_spurs_exception_request request={PS3_SPURS_EXCEPTION_SET,0,0,1,2};
    ps3_spurs_exception_result result;ps3_atomic_owner borrower;
    CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
#define EXCEPTION_CALL(scope) ps3_spurs_runtime_exception_handler(&f->rt,&request,(scope),borrower,16,NULL,NULL,&observed,accept_exception_event,&result)
    EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410811u&&!observed);
    request.spurs=B1+1;request.handler=0;EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410811u&&!observed);
    request.handler=1;EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410810u&&!observed);
    request.spurs=B1;EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_EXCEPTION_UNOWNED_INSTANCE&&!result.has_sdk_result&&!observed);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_EXCEPTION_NO_EXECUTION_SERVICES&&!result.reads&&!result.writes);
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    ps3_pthread_guard locks[2];configure_exception_services(f,locks);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    unsigned scope_calls=0;
    EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){&scope_calls,deny_idle_scope}));
    CHECK(result.stop==PS3_SPURS_EXCEPTION_MEMORY&&scope_calls==1&&!result.reads&&!result.writes);
    uint8_t mask[4];p32(mask,0x8000ffffu);CHECK(ps3_atomic_write(&f->domain,B1+0xb0,mask,4)==PS3_GUEST_MEMORY_OK);
    request.handler=0x665544;request.argument=0x112233;
    EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.has_sdk_result&&!result.sdk_result&&result.claim_written&&result.argument_written&&result.handler_written);
    uint8_t actual[16],expected_slot[16];p64(expected_slot,request.handler);p64(expected_slot+8,request.argument);
    CHECK(ps3_atomic_read(&f->domain,B1+0x100,actual,16)==PS3_GUEST_MEMORY_OK&&!memcmp(actual,expected_slot,16));
    request.action=PS3_SPURS_EXCEPTION_UNSET;request.handler=0;
    EXCEPTION_CALL(((ps3_spurs_exception_memory_scope){0}));
    CHECK(result.has_sdk_result&&!result.sdk_result&&result.handler_cleared);
    p64(expected_slot,0);
    CHECK(ps3_atomic_read(&f->domain,B1+0x100,actual,16)==PS3_GUEST_MEMORY_OK&&!memcmp(actual,expected_slot,16));
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    destroy(f);for(unsigned i=0;i<2;++i)CHECK(!ps3_pthread_guard_destroy(&locks[i]));free(f);
#undef EXCEPTION_CALL
}
typedef struct { Fixture *fixture;ps3_spurs_exception_stage stage;unsigned injected; } ExceptionInterleave;
static int exception_interleave_event(void *context,const ps3_spurs_exception_event *event) {
    ExceptionInterleave *interleave=context;
    if(event->operation==PS3_SPURS_EXCEPTION_WRITE&&event->stage==interleave->stage) {
        CHECK(!interleave->injected);++interleave->injected;
        inject_helper_test_event(interleave->fixture,interleave->fixture->rt.instances[0].queue,(ps3_lv2_event){0x11,0x22,0x33,0x44});
        ps3_spurs_stop_info stop=wait_runtime_stop(interleave->fixture);
        CHECK(stop.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT);
    }
    return 1;
}
static void exception_event_interleave_lane(ps3_spurs_exception_stage stage) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);ps3_pthread_guard locks[2];configure_exception_services(f,locks);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    uint8_t mask[4];p32(mask,0x8000ffffu);CHECK(ps3_atomic_write(&f->domain,B1+0xb0,mask,4)==PS3_GUEST_MEMORY_OK);
    ps3_atomic_owner borrower;CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_exception_request request={PS3_SPURS_EXCEPTION_SET,B1,0,0x556677,0x112233};ps3_spurs_exception_result result;
    ExceptionInterleave interleave={f,stage,0};
    ps3_spurs_runtime_exception_handler(&f->rt,&request,(ps3_spurs_exception_memory_scope){0},borrower,16,
        NULL,NULL,&interleave,exception_interleave_event,&result);
    CHECK(interleave.injected==1&&result.stop==PS3_SPURS_EXCEPTION_CANCELLED&&!result.has_sdk_result);
    CHECK(result.claim_written&&result.argument_written==(stage!=PS3_SPURS_EXCEPTION_CLAIM));
    CHECK(result.handler_written==(stage==PS3_SPURS_EXCEPTION_PUBLISH));
    uint8_t actual[16],expected_slot[16];p64(expected_slot,stage==PS3_SPURS_EXCEPTION_PUBLISH?request.handler:1);
    p64(expected_slot+8,stage==PS3_SPURS_EXCEPTION_CLAIM?0:request.argument);
    CHECK(ps3_atomic_read(&f->domain,B1+0x100,actual,16)==PS3_GUEST_MEMORY_OK&&!memcmp(actual,expected_slot,16));
    CHECK(f->rt.instances[0].state==PS3_SPURS_INSTANCE_ACTIVE&&f->rt.instances[0].owners_acquired==5);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(f->rt.instances[0].owners_acquired==5&&f->rt.instances[0].ls[0]);
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    CHECK(ps3_atomic_read(&f->domain,B1+0x100,actual,16)==PS3_GUEST_MEMORY_OK&&!memcmp(actual,expected_slot,16));
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    expect_clean(f,0);
    destroy(f);for(unsigned i=0;i<2;++i)CHECK(!ps3_pthread_guard_destroy(&locks[i]));free(f);
}
static void helper_spoofed_stop_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    ps3_spurs_instance *in=&f->rt.instances[0];uint32_t other_port;
    CHECK(ps3_lv2_port_create(&f->lv2,&other_port,PS3_EVENT_PORT_LOCAL,0)==PS3_LV2_OK);
    CHECK(other_port!=in->port&&ps3_lv2_port_connect_local(&f->lv2,other_port,in->queue)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&f->lv2,other_port,0,1,0)==PS3_LV2_OK);
    ps3_spurs_stop_info stop=wait_runtime_stop(f);
    CHECK(stop.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT&&stop.reason==PS3_SPURS_STOP_UNEXPECTED_HELPER_EVENT&&stop.has_event);
    CHECK(stop.event.source==((UINT64_C(1)<<32)|other_port)&&stop.event.data1==0&&stop.event.data2==1&&stop.event.data3==0);
    CHECK(stop.spurs_ea==B1&&stop.queue==in->queue);
    CHECK(ps3_lv2_port_disconnect(&f->lv2,other_port)==PS3_LV2_OK&&ps3_lv2_port_destroy(&f->lv2,other_port)==PS3_LV2_OK);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(in->owners_acquired==5&&in->ls[0]&&f->rt.helper_stops[0].event.source==stop.event.source);
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));expect_clean(f,0);destroy(f);free(f);
}
static void port_source_query_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);uint64_t source=UINT64_C(0xfedcba9876543210);
    ps3_lv2_objects empty={0};
    CHECK(ps3_lv2_port_source(NULL,0,&source)==PS3_LV2_EINVAL);
    CHECK(ps3_lv2_port_source(&empty,0,&source)==PS3_LV2_EINVAL);
    CHECK(ps3_lv2_port_source(&f->lv2,0,NULL)==PS3_LV2_EFAULT);
    CHECK(ps3_lv2_port_source(&f->lv2,0,&source)==PS3_LV2_ESRCH&&source==UINT64_C(0xfedcba9876543210));
    uint32_t queue,ports[2];ps3_lv2_queue_attr qa={PS3_SYNC_FIFO,PS3_PPU_QUEUE,0};
    CHECK(ps3_lv2_queue_create(&f->lv2,&queue,&qa,0,2)==PS3_LV2_OK);
    const uint64_t names[2]={0,UINT64_C(0x8877665544332211)};
    for(unsigned i=0;i<2;++i) {
        CHECK(ps3_lv2_port_create(&f->lv2,&ports[i],PS3_EVENT_PORT_LOCAL,names[i])==PS3_LV2_OK);
        CHECK(ps3_lv2_port_source(&f->lv2,ports[i],&source)==PS3_LV2_OK);
        CHECK(source==(i?names[i]:((UINT64_C(1)<<32)|ports[i])));
        CHECK(ps3_lv2_port_connect_local(&f->lv2,ports[i],queue)==PS3_LV2_OK);
        CHECK(ps3_lv2_port_send(&f->lv2,ports[i],1,2,3)==PS3_LV2_OK);
        ps3_lv2_event event;CHECK(ps3_lv2_queue_receive(&f->lv2,queue,&event,NULL,NULL)==PS3_LV2_OK);
        CHECK(event.source==source&&event.data1==1&&event.data2==2&&event.data3==3);
        CHECK(ps3_lv2_port_disconnect(&f->lv2,ports[i])==PS3_LV2_OK&&ps3_lv2_port_destroy(&f->lv2,ports[i])==PS3_LV2_OK);
        CHECK(ps3_lv2_port_source(&f->lv2,ports[i],&source)==PS3_LV2_ESRCH);
    }
    CHECK(ps3_lv2_queue_destroy(&f->lv2,queue)==PS3_LV2_OK);destroy(f);free(f);
}
static void helper_receive_failure_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    ps3_spurs_instance *in=&f->rt.instances[0];
    f->failure_cancel=&in->helper_cancel.requested;
    f->lv2.guard.lock(f->lv2.guard.context);ps3_lv2_queue *queue=NULL;
    for(unsigned i=0;i<PS3_LV2_QUEUE_CAP;++i)
        if(f->lv2.queues[i].active&&f->lv2.queues[i].id==in->queue)queue=&f->lv2.queues[i];
    CHECK(queue&&queue->blocked_receivers==1&&queue->count==0);
    __atomic_store_n(&f->fail_helper_wait,1,__ATOMIC_RELEASE);
    __atomic_fetch_add(&queue->sequence,1,__ATOMIC_SEQ_CST);
    f->lv2.guard.unlock(f->lv2.guard.context);fixture_wake(f);
    ps3_spurs_stop_info stop=wait_runtime_stop(f);
    CHECK(stop.category==PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE&&stop.reason==PS3_SPURS_STOP_HELPER_RECEIVE_FAILURE);
    CHECK(stop.receive_status==PS3_LV2_PROVIDER_FAILURE&&!stop.has_event&&stop.spurs_ea==B1&&stop.queue==in->queue);
    ps3_lv2_queue snapshot;CHECK(ps3_lv2_queue_snapshot(&f->lv2,in->queue,&snapshot)==PS3_LV2_OK&&snapshot.count==0);
    CHECK(f->rt.helper_stops[0].category==PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE);
    CHECK(f->rt.helper_stops[0].receive_status==PS3_LV2_PROVIDER_FAILURE);
    __atomic_store_n(&f->fail_helper_wait,0,__ATOMIC_RELEASE);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(in->owners_acquired==5&&in->ls[0]);CHECK(!ps3_spurs_runtime_shutdown(&f->rt));expect_clean(f,0);
    CHECK(f->rt.helper_stops[0].receive_status==PS3_LV2_PROVIDER_FAILURE);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    CHECK(!__atomic_load_n(&f->rt.helper_stops[0].ready,__ATOMIC_ACQUIRE));
    ps3_notify_cancel_request(&f->rt.instances[0].helper_cancel,&f->rt.config.notify);
    uint32_t helper_exit=UINT32_MAX;
    CHECK(ps3_ppu_service_join(&f->ppu,f->rt.instances[0].helper,&helper_exit)==PS3_PPU_OK&&helper_exit==2);
    f->rt.instances[0].helper_live=0;
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt,NULL)&&!__atomic_load_n(&f->rt.helper_stops[0].ready,__ATOMIC_ACQUIRE));
    destroy(f);free(f);
}
typedef struct { unsigned cancel,observed; } ReadyObservation;
static int ready_may_continue(void *context) { return !((ReadyObservation *)context)->cancel; }
static int observe_ready_count(void *context,const ps3_spurs_ready_count_event *event) {
    ReadyObservation *observation=context;++observation->observed;(void)event;return 1;
}
static int cancel_after_ready_store(void *context,const ps3_spurs_ready_count_event *event) {
    ReadyObservation *observation=context;++observation->observed;
    if(event->operation==PS3_SPURS_READY_COUNT_WRITE)observation->cancel=1;
    return 1;
}
static void ready_count_runtime_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);ps3_atomic_owner borrower,witness;
    CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owner_acquire(&f->domain,&witness)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_ready_count_request request={0,0,1};ps3_spurs_ready_count_result result;
    ReadyObservation observation={0};
#define READY_CALL(scope) ps3_spurs_runtime_ready_count_store(&f->rt,&request,(scope),borrower,NULL,NULL,&observation,observe_ready_count,&result)
    READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410811u&&!observation.observed);
    request.spurs=B1+1;READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410810u&&!observation.observed);
    request.spurs=B1;READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE&&!result.has_sdk_result);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_READY_COUNT_NO_EXECUTION_SERVICES&&!result.reads&&!result.writes);
    CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    ps3_pthread_guard locks[2];configure_exception_services(f,locks);
    CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    unsigned scope_calls=0;
    READY_CALL(((ps3_spurs_ready_count_memory_scope){&scope_calls,deny_idle_scope}));
    CHECK(result.stop==PS3_SPURS_READY_COUNT_MEMORY&&scope_calls==1&&!result.reads&&!result.writes);
    request.value=256;READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410802u&&!result.writes);
    request.value=255;
    /* Guest SDK-valid metadata only; local selector eligibility remains zero.
     * These setup stores touch L1, leaving every actual idle L0 reservation held. */
    uint8_t mask[4];p32(mask,0x8000ffffu);uint8_t runnable=2;
    CHECK(ps3_atomic_write(&f->domain,B1+0xb0,mask,4)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain,B1+0x80,&runnable,1)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_instance *in=&f->rt.instances[0];
    CHECK(ps3_spurs_k1_context_state_of(&in->kernel,0)==PS3_SPURS_K1_CTX_BLOCKED_LR);
    uint64_t wake_before=ps3_spurs_k1_counter(&in->kernel,0,PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS);
    uint64_t settled_before=ps3_spurs_k1_settled_count(&in->kernel);
    uint8_t line[128];CHECK(ps3_atomic_load_reserve128(&f->domain,witness,B1,line)==PS3_GUEST_MEMORY_OK&&line[0]==0);
    READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.has_sdk_result&&!result.sdk_result&&result.stored&&result.writes==1&&result.written_bytes==1);
    CHECK(result.ready_address==B1);
    int lost=0;CHECK(ps3_atomic_owner_take_reservation_loss(&f->domain,witness,&lost)==PS3_GUEST_MEMORY_OK&&lost);
    CHECK(ps3_atomic_load_reserve128(&f->domain,witness,B1,line)==PS3_GUEST_MEMORY_OK&&line[0]==255);
    for(unsigned i=1;i<16;++i)CHECK(line[i]==0);
    CHECK(ps3_spurs_k1_wait_settled_after(&in->kernel,settled_before,ps3_pthread_wait_port_now_ns()+UINT64_C(5000000000))==PS3_SPURS_K1_PHASE_QUIESCENT);
    CHECK(ps3_spurs_k1_counter(&in->kernel,0,PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS)>wake_before);
    CHECK(!ps3_spurs_runtime_poll_stop(&f->rt,NULL)); /* Wake proof, not dispatch/job completion. */
    request.value=7;observation=(ReadyObservation){0};
    ps3_spurs_runtime_ready_count_store(&f->rt,&request,(ps3_spurs_ready_count_memory_scope){0},borrower,
        &observation,ready_may_continue,&observation,cancel_after_ready_store,&result);
    CHECK(result.stop==PS3_SPURS_READY_COUNT_CANCELLED&&!result.has_sdk_result&&result.stored&&result.written_bytes==1);
    CHECK(ps3_atomic_read(&f->domain,B1,line,1)==PS3_GUEST_MEMORY_OK&&line[0]==7);
    uint32_t other_port;CHECK(ps3_lv2_port_create(&f->lv2,&other_port,PS3_EVENT_PORT_LOCAL,0)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_connect_local(&f->lv2,other_port,in->queue)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&f->lv2,other_port,0,1,0)==PS3_LV2_OK);
    CHECK(wait_runtime_stop(f).category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT);
    request.value=9;observation=(ReadyObservation){0};READY_CALL(((ps3_spurs_ready_count_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_READY_COUNT_CANCELLED&&!result.has_sdk_result&&!result.reads&&!result.writes&&!observation.observed);
    CHECK(ps3_lv2_port_disconnect(&f->lv2,other_port)==PS3_LV2_OK&&ps3_lv2_port_destroy(&f->lv2,other_port)==PS3_LV2_OK);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(in->owners_acquired==5&&in->ls[0]);CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    CHECK(ps3_atomic_read(&f->domain,B1,line,1)==PS3_GUEST_MEMORY_OK&&line[0]==7);
    CHECK(ps3_atomic_owner_release(&f->domain,&witness)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);expect_clean(f,0);
    destroy(f);for(unsigned i=0;i<2;++i)CHECK(!ps3_pthread_guard_destroy(&locks[i]));free(f);
#undef READY_CALL
}
typedef struct { unsigned observed,writes,cancel,deny_write; } WorkloadInfoObservation;
static int workload_info_continue(void *context) { return !((WorkloadInfoObservation *)context)->cancel; }
static int observe_workload_info(void *context,const ps3_spurs_workload_info_event *event) {
    WorkloadInfoObservation *o=context;++o->observed;
    if(event->operation==PS3_SPURS_WORKLOAD_INFO_WRITE)++o->writes;
    return 1;
}
static int cancel_after_info_byte(void *context,const ps3_spurs_workload_info_event *event) {
    WorkloadInfoObservation *o=context;observe_workload_info(context,event);
    if(o->writes==5)o->cancel=1;
    return 1;
}
static int workload_info_scope(void *context,uint32_t address,size_t length,unsigned write) {
    WorkloadInfoObservation *o=context;(void)address;(void)length;return !write||!o->deny_write;
}
static void workload_info_runtime_lane(void) {
    Fixture *f=calloc(1,sizeof *f);CHECK(f);setup(f,0);
    enum { INFO_OUTPUT=0x007e0000 };
    ps3_atomic_owner borrower={0};ps3_spurs_workload_info_request request={B1+1,0,0};
    ps3_spurs_workload_info_result result;WorkloadInfoObservation observation={0};
#define INFO_CALL(scope) ps3_spurs_runtime_get_workload_info(&f->rt,&request,(scope),borrower,NULL,NULL,&observation,observe_workload_info,&result)
    /* Pure errors do not acquire/use an owner or require an ACTIVE instance. */
    INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410811u&&!result.reads&&!result.writes);
    request.info=INFO_OUTPUT;INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410810u&&!observation.observed);
    request.spurs=B1;INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE&&!result.has_sdk_result);
    CHECK(ps3_atomic_owner_acquire(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);
    uint8_t a[512];attribute(a,2,5,128,249,0,0);CHECK(init(f,B1,a).outcome==PS3_SPURS_OUTCOME_OK);
    CHECK(!f->rt.workload_services_configured);
    INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.has_sdk_result&&result.sdk_result==0x80410805u&&result.reads==2&&!result.writes);
    /* Explicit guest-memory fixture, no registration/execution shortcut or policy service.
     * Priorities/state stay ineligible; query must still report enabled raw metadata. */
    uint8_t mask[4];p32(mask,0x8000ffffu);
    CHECK(ps3_atomic_write(&f->domain,B1+0xb0,mask,4)==PS3_GUEST_MEMORY_OK);
    const uint8_t metadata[20]={0,0,0,0,0,0x69,0xc8,0x80,0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0,0,0x2d,0x80};
    CHECK(ps3_atomic_write(&f->domain,B1+0xb00,metadata,sizeof metadata)==PS3_GUEST_MEMORY_OK);
    uint8_t zeros[8]={0};CHECK(ps3_atomic_write(&f->domain,B1+0xb18,zeros,8)==PS3_GUEST_MEMORY_OK);
    uint8_t fill[48];memset(fill,PAT,sizeof fill);
    CHECK(ps3_atomic_write(&f->domain,INFO_OUTPUT,fill,sizeof fill)==PS3_GUEST_MEMORY_OK);
    observation=(WorkloadInfoObservation){0};INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.has_sdk_result&&!result.sdk_result&&result.reads==17&&result.writes==21&&result.written_bytes==46);
    CHECK(!f->rt.workload_services_configured&&observation.observed==38&&observation.writes==21);
    uint8_t image[48];CHECK(ps3_atomic_read(&f->domain,INFO_OUTPUT,image,sizeof image)==PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(image,metadata+8,8)&&image[0x26]==PAT&&image[0x27]==PAT);
    CHECK(image[0x10]==0&&image[0x11]==0x69&&image[0x12]==0xc8&&image[0x13]==0x80);
    observation=(WorkloadInfoObservation){.deny_write=1};
    INFO_CALL(((ps3_spurs_workload_info_memory_scope){&observation,workload_info_scope}));
    CHECK(result.stop==PS3_SPURS_WORKLOAD_INFO_MEMORY&&!result.has_sdk_result&&result.reads==3&&!result.writes&&!result.write_attempts);
    CHECK(ps3_atomic_write(&f->domain,INFO_OUTPUT,fill,sizeof fill)==PS3_GUEST_MEMORY_OK);
    observation=(WorkloadInfoObservation){0};
    ps3_spurs_runtime_get_workload_info(&f->rt,&request,(ps3_spurs_workload_info_memory_scope){0},borrower,
        &observation,workload_info_continue,&observation,cancel_after_info_byte,&result);
    CHECK(result.stop==PS3_SPURS_WORKLOAD_INFO_CANCELLED&&!result.has_sdk_result&&result.writes==5&&result.written_bytes==5);
    CHECK(ps3_atomic_read(&f->domain,INFO_OUTPUT,image,sizeof image)==PS3_GUEST_MEMORY_OK);
    CHECK(image[0x0a]==PAT&&image[0x0b]==0&&image[0x0f]==0&&image[0x00]==PAT);
    /* Legal primary-record alias: priority stores overwrite data before its later
     * read, then data publication overwrites the later policy-pointer source. */
    request.info=B1+0xb00;observation=(WorkloadInfoObservation){0};INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.has_sdk_result&&!result.sdk_result&&result.written_bytes==46);
    CHECK(ps3_atomic_read(&f->domain,request.info,image,sizeof image)==PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(image,zeros,8)&&!memcmp(image+0x10,zeros,4));
    uint32_t other_port;ps3_spurs_instance *in=&f->rt.instances[0];
    CHECK(ps3_lv2_port_create(&f->lv2,&other_port,PS3_EVENT_PORT_LOCAL,0)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_connect_local(&f->lv2,other_port,in->queue)==PS3_LV2_OK);
    CHECK(ps3_lv2_port_send(&f->lv2,other_port,0,1,0)==PS3_LV2_OK);
    CHECK(wait_runtime_stop(f).category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT);
    observation=(WorkloadInfoObservation){0};request.info=INFO_OUTPUT;INFO_CALL(((ps3_spurs_workload_info_memory_scope){0}));
    CHECK(result.stop==PS3_SPURS_WORKLOAD_INFO_CANCELLED&&!result.has_sdk_result&&!result.reads&&!result.writes&&!observation.observed);
    CHECK(ps3_lv2_port_disconnect(&f->lv2,other_port)==PS3_LV2_OK&&ps3_lv2_port_destroy(&f->lv2,other_port)==PS3_LV2_OK);
    f->th[0][PS3_SPURS_THREAD_HANDLER].fail_next_join=1;CHECK(ps3_spurs_runtime_shutdown(&f->rt)==1);
    CHECK(in->owners_acquired==5&&in->ls[0]);CHECK(!ps3_spurs_runtime_shutdown(&f->rt));
    CHECK(ps3_atomic_owner_release(&f->domain,&borrower)==PS3_GUEST_MEMORY_OK);expect_clean(f,0);destroy(f);free(f);
#undef INFO_CALL
}
int main(void) {
    workload_info_runtime_lane();
    ready_count_runtime_lane();
    helper_receive_failure_lane();
    port_source_query_lane();
    helper_spoofed_stop_lane();
    exception_wrapper_lane();
    exception_event_interleave_lane(PS3_SPURS_EXCEPTION_CLAIM);
    exception_event_interleave_lane(PS3_SPURS_EXCEPTION_ARGUMENT);
    exception_event_interleave_lane(PS3_SPURS_EXCEPTION_PUBLISH);
    helper_event_fence_lane();
    workload_services_lane();
    no_workload_services_lane();
    ok_lane();
    prefix_lane();
    validation_lane();
    failure_lane(arm_sem, PS3_LV2_EAGAIN, 3);
    failure_lane(arm_sem_prv, PS3_LV2_EAGAIN, 3);
    failure_lane(arm_quota, 0x80010004u, 5);
    failure_lane(arm_mutex, PS3_LV2_EAGAIN, 7);
    failure_lane(arm_cond, 0x80010004u, 7);
    failure_lane(arm_queue, PS3_LV2_EAGAIN, 9);
    failure_lane(arm_port, 0x80410701u, 10);
    failure_lane(arm_helper_start, 0x8041070Fu, 11);
    failure_lane(arm_helper_quota, 0x8041070Fu, 11);
    failure_lane(arm_ppu_limit, 0x8041070Fu, 12);
    failure_lane(arm_handler_quota, 0x8041070Fu, 12);
    kernel_stop_lane(1);
    kernel_stop_lane(0);
    write_failure_lane();
    join_failure_lane();
    second_instance_einval_lane();
    sync_provider_stop_lane(0);
    sync_provider_stop_lane(1);
    owner_release_failure_lane();
    puts("test_spurs_instance: OK");
    return 0;
}
