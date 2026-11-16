#define _POSIX_C_SOURCE 200809L
/* Guest cellSpursAttachLv2EventQueue (static) domain API + lifecycle over REAL collaborators: coherent guest memory +
 * atomic domain, shared user-memory quota, lv2 objects, SPU group policy, PPU service threads on host threads, the
 * kernel-1 idle service executing on real atomic owners, and a real SPURS instance of 5 SPUs. The only doubles are the
 * lwmutex/lwcond registries (SyncDouble, as in test_spurs_instance.c) and the attach memory port, which is a thin
 * wrapper over the REAL atomic domain (ps3_atomic_read/write/compare_exchange64) with fault injection. Expected values
 * are hand-derived from the pinned reference (RPCS3 63996a47 cellSpurs.cpp:530-585: exception 0xD6C, spuPortBits 0xDA8),
 * never produced by the implementation. */
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
       PORT_EA = 0x007C0010, PAT = 0xA5, EXC = 0xD6C, BITS = 0xDA8, SDK = 0x250001 };
enum { LV2_ESRCH = 0x80010005u, LV2_EBUSY = 0x8001000Au };

typedef struct Fixture_ {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard g_mem, g_lv2, g_ppu, g_pol, g_dom;
    ps3_pthread_wait_port port;
    ps3_notify_port np;
    ps3_user_memory memory; ps3_user_memory_slot slots[2]; ps3_user_memory_charge_record charges[16];
    ps3_atomic_domain domain; ps3_atomic_owner_record records[16];
    ps3_lv2_objects lv2; ps3_ppu_threads ppu; ps3_spu_resource_policy policy;
    ps3_pthread_service_thread th[PS3_SPURS_MAX_INSTANCES][3];
    ps3_spurs_runtime rt;
    unsigned char *ls[PS3_SPURS_LS_SLOTS];
    unsigned mutexes, conds; uint32_t mutex_at[4], cond_at[4];
} Fixture;
static const unsigned char MUTEX_IMAGE[24] = {0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7};
static const unsigned char COND_IMAGE[8] = {0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7};
static uint32_t sync_mutex_create(void *c, uint32_t ea, uint32_t protocol, uint32_t recursive) {
    Fixture *f = c;
    CHECK(protocol == 2 && recursive == 0x20);
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
    CHECK(ps3_atomic_write(&f->domain, ea, COND_IMAGE, sizeof COND_IMAGE) == PS3_GUEST_MEMORY_OK);
    f->cond_at[f->conds++] = ea;
    return 0;
}
static uint32_t sync_cond_destroy(void *c, uint32_t ea) {
    Fixture *f = c;
    for (unsigned i = 0; i < f->conds; ++i) if (f->cond_at[i] == ea) { f->cond_at[i] = f->cond_at[--f->conds]; return 0; }
    return PS3_LV2_ESRCH;
}
static void setup(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(SPACE);
    CHECK(f->space);
    f->backing = elf_test_host_space_port(f->space);
    CHECK(f->backing.commit(f->backing.context, 0, SPACE) == PS3_GUEST_MEMORY_OK);
    CHECK(!ps3_pthread_guard_init(&f->g_mem) && !ps3_pthread_guard_init(&f->g_lv2) && !ps3_pthread_guard_init(&f->g_ppu) &&
          !ps3_pthread_guard_init(&f->g_pol) && !ps3_pthread_guard_init(&f->g_dom) && !ps3_pthread_wait_port_init(&f->port));
    f->np = ps3_pthread_wait_port_port(&f->port);
    ps3_user_memory_config mc = {WINDOW, WINDOW_END, QUOTA, f->backing, ps3_pthread_guard_port(&f->g_mem)};
    CHECK(ps3_user_memory_init(&f->memory, &mc, f->slots, 2) == PS3_USER_MEMORY_OK);
    CHECK(ps3_user_memory_attach_charges(&f->memory, f->charges, 16) == PS3_USER_MEMORY_OK);
    CHECK(ps3_atomic_domain_init(&f->domain, f->backing, ps3_pthread_guard_port(&f->g_dom), f->records, 16, NULL, 0) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_domain_attach_wait_port(&f->domain, f->np) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_lv2_objects_init(&f->lv2, ps3_pthread_guard_port(&f->g_lv2), f->np) == PS3_LV2_OK);
    CHECK(ps3_ppu_threads_init(&f->ppu, ps3_pthread_guard_port(&f->g_ppu), f->np, &f->memory) == PS3_PPU_OK);
    CHECK(ps3_spu_resource_policy_init(&f->policy, ps3_pthread_guard_port(&f->g_pol)) == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_configure(&f->policy, 6, 0).status == PS3_SPU_POLICY_OK);
    CHECK(ps3_spu_resource_policy_attach(&f->policy, &f->memory, &f->lv2, f->np));
    ps3_spurs_runtime_config rc;
    memset(&rc, 0, sizeof rc);
    rc.domain = &f->domain; rc.notify = f->np; rc.lv2 = &f->lv2; rc.policy = &f->policy; rc.ppu = &f->ppu;
    rc.sync = (ps3_spurs_sync_port){f, sync_mutex_create, sync_mutex_destroy, sync_cond_create, sync_cond_destroy};
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned r = 0; r < 3; ++r) rc.threads[i][r] = ps3_pthread_service_thread_port(&f->th[i][r]);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) { f->ls[i] = malloc(PS3_SPURS_K1_LS_BYTES); CHECK(f->ls[i]); memset(f->ls[i], PAT, PS3_SPURS_K1_LS_BYTES); rc.local_stores[i] = f->ls[i]; }
    rc.creator_priority = 250;
    CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) == 0);
    unsigned char fill[0x1200];
    memset(fill, PAT, sizeof fill);
    CHECK(ps3_atomic_write(&f->domain, B1 - 0x100, fill, sizeof fill) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain, PORT_EA - 16, fill, 32) == PS3_GUEST_MEMORY_OK);
}
static void destroy(Fixture *f) {
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) free(f->ls[i]);
    CHECK(ps3_user_memory_cleanup(&f->memory).status == PS3_USER_MEMORY_OK);
    ps3_pthread_wait_port_destroy(&f->port);
    ps3_pthread_guard_destroy(&f->g_mem); ps3_pthread_guard_destroy(&f->g_lv2); ps3_pthread_guard_destroy(&f->g_ppu);
    ps3_pthread_guard_destroy(&f->g_pol); ps3_pthread_guard_destroy(&f->g_dom);
    elf_test_host_space_destroy(f->space);
}
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void attribute(uint8_t *a, uint32_t nspus) {
    memset(a, 0, 512);
    p32(a, 2); p32(a + 4, SDK); p32(a + 8, nspus); p32(a + 12, 128); p32(a + 16, 249);
    for (unsigned i = 0; i < 15; ++i) a[0x15 + i] = (uint8_t)('a' + i);
}
static uint64_t get64(Fixture *f, uint32_t ea) {
    uint8_t b[8]; uint64_t v = 0;
    CHECK(ps3_atomic_read(&f->domain, ea, b, 8) == PS3_GUEST_MEMORY_OK);
    for (unsigned i = 0; i < 8; ++i) v = v << 8 | b[i];
    return v;
}
static void put64d(Fixture *f, uint32_t ea, uint64_t v) {
    uint8_t b[8];
    for (unsigned i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (56 - 8 * i));
    CHECK(ps3_atomic_write(&f->domain, ea, b, 8) == PS3_GUEST_MEMORY_OK);
}
static void put8d(Fixture *f, uint32_t ea, uint8_t v) { CHECK(ps3_atomic_write(&f->domain, ea, &v, 1) == PS3_GUEST_MEMORY_OK); }
static uint8_t get8d(Fixture *f, uint32_t ea) { uint8_t v; CHECK(ps3_atomic_read(&f->domain, ea, &v, 1) == PS3_GUEST_MEMORY_OK); return v; }
static Fixture *boot(void) {
    Fixture *f = calloc(1, sizeof *f);
    CHECK(f);
    setup(f);
    uint8_t a[512];
    attribute(a, 5);
    ps3_spurs_request r = {0xD00FF990u, B1, a};
    ps3_spurs_result res = ps3_spurs_runtime_initialize(&f->rt, &r);
    CHECK(res.outcome == PS3_SPURS_OUTCOME_OK && f->rt.instances[0].state == PS3_SPURS_INSTANCE_ACTIVE);
    put8d(f, PORT_EA, 1);
    return f;
}
static void finish(Fixture *f) { destroy(f); free(f); }
static ps3_spurs_instance *inst(Fixture *f) { return &f->rt.instances[0]; }
static ps3_spu_group_record *grec(Fixture *f) {
    for (unsigned i = 0; i < PS3_SPU_GROUP_CAP; ++i) if (f->policy.groups[i].active && f->policy.groups[i].id == inst(f)->group) return &f->policy.groups[i];
    CHECK(0);
    return NULL;
}
static uint32_t mkq(Fixture *f, uint32_t type) {
    uint32_t id = 0;
    const ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, type, 0};
    CHECK(ps3_lv2_queue_create(&f->lv2, &id, &qa, 0, 127) == PS3_LV2_OK);
    return id;
}

/* ---- a real guest-like receiver blocked in the borrowed queue ---- */
typedef struct { Fixture *f; uint32_t q; ps3_notify_cancel cancel; uint64_t blocked; pthread_t th; volatile unsigned done; uint32_t rc; } Recv;
static void *recv_main(void *p) {
    Recv *r = p;
    ps3_lv2_event e;
    r->rc = ps3_lv2_queue_receive(&r->f->lv2, r->q, &e, &r->cancel, &r->blocked);
    __atomic_store_n(&r->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void await_blocked(Fixture *f, uint32_t q, uint32_t want) {
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ps3_lv2_queue s;
        CHECK(ps3_lv2_queue_snapshot(&f->lv2, q, &s) == PS3_LV2_OK);
        if (s.blocked_receivers == want) return;
        clock_gettime(CLOCK_MONOTONIC, &t1); CHECK(t1.tv_sec - t0.tv_sec < 5);
        sched_yield();
    }
}
static void recv_start(Recv *r, Fixture *f, uint32_t q) {
    memset(r, 0, sizeof *r);
    r->f = f; r->q = q;
    CHECK(pthread_create(&r->th, NULL, recv_main, r) == 0);
    await_blocked(f, q, 1);
}
static void recv_stop(Recv *r) {
    ps3_notify_cancel_request(&r->cancel, &r->f->np);
    CHECK(pthread_join(r->th, NULL) == 0);
    CHECK(r->rc == PS3_LV2_CANCELLED);
}

/* ---- the attach memory port: a thin wrapper over the REAL atomic domain + a call log + injected faults ---- */
typedef struct { char kind; uint32_t ea; size_t len; uint64_t a, b; } Call;
typedef struct AMem_ {
    Fixture *f;
    Call log[64]; unsigned n;
    unsigned reads, writes, cas;
    unsigned fail_read_at, fail_write_at, fail_cas_at;     /* 1-based per kind: nonzero return, NOTHING performed */
    unsigned interfere_from, interfere_count;               /* a concurrent writer ORs another DA8 bit before these CAS calls run */
    unsigned commit_then_fail_at;                           /* the CAS commits, then the callback reports a failure */
    unsigned hook_write_at; void (*hook)(struct AMem_ *);
    unsigned recorded_at_first_write; uint64_t bits_at_first_write; uint32_t queue_at_first_write;
} AMem;
static void note(AMem *m, char k, uint32_t ea, size_t len, uint64_t a, uint64_t b) { CHECK(m->n < 64); m->log[m->n++] = (Call){k, ea, len, a, b}; }
static int am_read(void *c, uint32_t ea, void *b, size_t n) {
    AMem *m = c; unsigned i = ++m->reads;
    note(m, 'R', ea, n, 0, 0);
    if (m->fail_read_at == i) return -1;
    return ps3_atomic_read(&m->f->domain, ea, b, n) == PS3_GUEST_MEMORY_OK ? 0 : -2;
}
static int am_write(void *c, uint32_t ea, const void *b, size_t n) {
    AMem *m = c; unsigned i = ++m->writes;
    note(m, 'W', ea, n, n == 1 ? *(const uint8_t *)b : 0, 0);
    if (i == 1) {   /* the ledger must already hold the binding when the first guest write happens */
        ps3_spurs_instance *in = inst(m->f);
        m->bits_at_first_write = in->user_port_bits;
        m->queue_at_first_write = in->user_port_queue[1];
        m->recorded_at_first_write = 1;
    }
    if (m->hook_write_at == i && m->hook) m->hook(m);
    if (m->fail_write_at == i) return -1;
    return ps3_atomic_write(&m->f->domain, ea, b, n) == PS3_GUEST_MEMORY_OK ? 0 : -2;
}
static int am_cas(void *c, uint32_t ea, uint64_t exp, uint64_t des, ps3_atomic_compare_result *r) {
    AMem *m = c; unsigned i = ++m->cas;
    note(m, 'C', ea, 8, exp, des);
    if (m->fail_cas_at == i) return -1;
    if (m->interfere_count && i >= m->interfere_from && i < m->interfere_from + m->interfere_count)
        put64d(m->f, ea, get64(m->f, ea) | (UINT64_C(0x100) << i));
    if (ps3_atomic_compare_exchange64(&m->f->domain, ea, exp, des, r) != PS3_GUEST_MEMORY_OK) return -2;
    if (m->commit_then_fail_at == i && r->exchanged) return 7;
    return 0;
}
static void amem(AMem *m, Fixture *f) { memset(m, 0, sizeof *m); m->f = f; }
static ps3_spurs_attach_memory mport(AMem *m) { return (ps3_spurs_attach_memory){m, am_read, am_write, am_cas}; }
static ps3_spurs_attach_request req(uint32_t queue) { return (ps3_spurs_attach_request){B1, queue, PORT_EA, 0, SDK, 4}; }
static ps3_spurs_result attach(Fixture *f, AMem *m, ps3_spurs_attach_request rq, ps3_spurs_attach_info *info) {
    ps3_spurs_attach_memory mem = mport(m);
    return ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, &rq, &mem, info);
}
static void expect_call(const AMem *m, unsigned i, char kind, uint32_t ea, size_t len) { CHECK(i < m->n && m->log[i].kind == kind && m->log[i].ea == ea && m->log[i].len == len); }

/* ---- observable world, for exact "no effect" / "only this changed" comparisons ---- */
typedef struct {
    unsigned char img[0x1000], around[32];
    uint32_t pq[PS3_SPU_GROUP_THREADS][PS3_SPU_PORTS];
    uint32_t group_exception_queue, group_state, group_num;
    uint64_t bits, sem, que, prt, ppu, used;
    uint32_t rec[PS3_SPU_PORTS], state;
    unsigned owners, ls_used;
} World;
static void world(Fixture *f, World *w) {
    memset(w, 0, sizeof *w);
    CHECK(ps3_atomic_read(&f->domain, B1, w->img, 0x1000) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_read(&f->domain, PORT_EA - 16, w->around, 32) == PS3_GUEST_MEMORY_OK);
    ps3_spurs_instance *in = inst(f);
    ps3_spu_group_record g;
    CHECK(ps3_spu_group_snapshot(&f->policy, in->group, &g) == PS3_LV2_OK);
    for (unsigned n = 0; n < PS3_SPU_GROUP_THREADS; ++n) memcpy(w->pq[n], g.threads[n].port_queue, sizeof w->pq[n]);
    w->group_exception_queue = g.exception_queue; w->group_state = g.state; w->group_num = g.num;
    w->bits = in->user_port_bits; memcpy(w->rec, in->user_port_queue, sizeof w->rec); w->state = in->state;
    w->sem = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE); w->que = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE);
    w->prt = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_PORT); w->ppu = ps3_ppu_threads_live(&f->ppu);
    ps3_user_memory_info ui; CHECK(ps3_user_memory_query(&f->memory, &ui) == PS3_USER_MEMORY_OK); w->used = ui.used_bytes;
    w->owners = in->owners_acquired;
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) w->ls_used += f->rt.ls_used[i];
}
static void same(const World *a, const World *b) { CHECK(memcmp(a, b, sizeof *a) == 0); }
static void expect_quiet_kernel(Fixture *f) { CHECK(!ps3_spurs_runtime_poll_stop(&f->rt, NULL)); }

/* ---------------------------------------------------------------------------------------------------------------- */
static void policy_exact_lane(void) {
    Fixture *f = calloc(1, sizeof *f);
    setup(f);
    uint32_t group = 0, tid = 0;
    CHECK(ps3_spu_group_create(&f->policy, &group, 3, 128, 0, 0) == PS3_LV2_OK);
    for (uint32_t n = 0; n < 3; ++n) CHECK(ps3_spu_thread_initialize(&f->policy, &tid, group, n, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1, 0, 0, 2, 0) == PS3_LV2_OK);
    uint32_t q = mkq(f, PS3_PPU_QUEUE), other = mkq(f, PS3_PPU_QUEUE), third = mkq(f, PS3_PPU_QUEUE);
    uint8_t spup = 0xFF;
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, group, q, UINT64_C(1) << 5, &spup) == PS3_LV2_OK && spup == 5);
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, group, third, UINT64_C(1) << 6, &spup) == PS3_LV2_OK && spup == 6);
    ps3_spu_group_record before, after;
    /* argument errors: nothing written */
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 64, q) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, 0) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, 0x04FFFF00u, 5, q) == PS3_LV2_ESRCH);
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(NULL, group, 5, q) == PS3_LV2_EINVAL);
    CHECK(ps3_spu_group_snapshot(&f->policy, group, &before) == PS3_LV2_OK);
    for (uint32_t n = 0; n < 3; ++n) CHECK(before.threads[n].port_queue[5] == q && before.threads[n].port_queue[6] == third);
    /* a foreign queue on ONE thread of the port: ESRCH and not one thread changes (no forward effects, no partial strip) */
    for (uint32_t bad = 0; bad < 3; ++bad) {
        ps3_spu_group_record *g = NULL;
        for (unsigned i = 0; i < PS3_SPU_GROUP_CAP; ++i) if (f->policy.groups[i].active && f->policy.groups[i].id == group) g = &f->policy.groups[i];
        CHECK(g);
        g->threads[bad].port_queue[5] = other;
        CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, q) == PS3_LV2_ESRCH);
        CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
        for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[5] == (n == bad ? other : q));
        g->threads[bad].port_queue[5] = 0;     /* an already-clear thread is a mismatch as well */
        CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, q) == PS3_LV2_ESRCH);
        CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
        for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[5] == (n == bad ? 0 : q));
        g->threads[bad].port_queue[5] = q;
    }
    /* the other queue's exact request on the same port does not strip q */
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, other) == PS3_LV2_ESRCH);
    /* full match clears every thread of that port only */
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, q) == PS3_LV2_OK);
    CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
    for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[5] == 0 && after.threads[n].port_queue[6] == third);
    CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 5, q) == PS3_LV2_ESRCH);   /* not idempotent: nothing bound */
    /* the pre-existing unconditional primitive keeps its semantics: it strips whatever is bound */
    CHECK(ps3_spu_group_disconnect_event_all_threads(&f->policy, group, 6) == PS3_LV2_OK);
    CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
    for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[6] == 0);
    /* F4: a DEAD binding that still stores `queue` on every thread (its lv2 queue was destroyed) is cleared and answers OK:
     * the match is on the stored id only; our own stale binding occupies nothing and must not wedge shutdown */
    {
        uint32_t dq = mkq(f, PS3_PPU_QUEUE), dq2 = mkq(f, PS3_PPU_QUEUE);
        CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, group, dq, UINT64_C(1) << 9, &spup) == PS3_LV2_OK && spup == 9);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, dq) == PS3_LV2_OK);
        CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 9, dq) == PS3_LV2_OK);
        CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
        for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[9] == 0);
        /* a dead binding storing a DIFFERENT id still refuses and writes nothing */
        CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, group, dq2, UINT64_C(1) << 10, &spup) == PS3_LV2_OK && spup == 10);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, dq2) == PS3_LV2_OK);
        CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 10, q) == PS3_LV2_ESRCH);
        CHECK(ps3_spu_group_snapshot(&f->policy, group, &after) == PS3_LV2_OK);
        for (uint32_t n = 0; n < 3; ++n) CHECK(after.threads[n].port_queue[10] == dq2);
        CHECK(ps3_spu_group_disconnect_event_all_threads_exact(&f->policy, group, 10, dq2) == PS3_LV2_OK);
    }
    CHECK(ps3_spu_group_destroy(&f->policy, group) == PS3_LV2_OK);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, other) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, third) == PS3_LV2_OK);
    destroy(f);
    free(f);
}

static void happy_lane(void) {
    Fixture *f = boot();
    ps3_spurs_instance *in = inst(f);
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    Recv rv;
    recv_start(&rv, f, q);
    /* a prior reservation on the DA8 line (must be lost) and one on an unrelated line (must survive) */
    ps3_atomic_owner on_bits, on_other;
    CHECK(ps3_atomic_owner_acquire(&f->domain, &on_bits) == PS3_GUEST_MEMORY_OK && ps3_atomic_owner_acquire(&f->domain, &on_other) == PS3_GUEST_MEMORY_OK);
    uint64_t v;
    CHECK(ps3_atomic_load_reserve64(&f->domain, on_bits, B1 + BITS, &v) == PS3_GUEST_MEMORY_OK && v == 0);
    CHECK(ps3_atomic_load_reserve64(&f->domain, on_other, B1 + 0x1100, &v) == PS3_GUEST_MEMORY_OK);
    World before, after, want;
    world(f, &before);
    ps3_lv2_queue q0;
    CHECK(ps3_lv2_queue_snapshot(&f->lv2, q, &q0) == PS3_LV2_OK && q0.count == 0 && q0.receivers == 1 && q0.blocked_receivers == 1 && !q0.service_bound);
    AMem m; amem(&m, f);
    ps3_spurs_attach_info info;
    ps3_spurs_result r = attach(f, &m, req(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.rc == 0 && r.reason == 0 && r.step == PS3_SPURS_ATTACH_STEP_DONE && r.instance == 0);
    CHECK(info.spup == 1 && info.port_written == 1 && info.cas_attempts == 1 && info.prior_port_bits == 0 && !info.prior_bit_set && info.commit_status == 0);
    /* exact memory-port traffic: exception, port byte, seed read (8 BE bytes), port byte write, ONE strong CAS */
    CHECK(m.n == 5);
    expect_call(&m, 0, 'R', B1 + EXC, 4); expect_call(&m, 1, 'R', PORT_EA, 1); expect_call(&m, 2, 'R', B1 + BITS, 8);
    expect_call(&m, 3, 'W', PORT_EA, 1); CHECK(m.log[3].a == 1);
    expect_call(&m, 4, 'C', B1 + BITS, 8); CHECK(m.log[4].a == 0 && m.log[4].b == 2);
    CHECK(m.recorded_at_first_write && m.bits_at_first_write == 2 && m.queue_at_first_write == q);   /* recorded BEFORE any guest write */
    world(f, &after);
    want = before;
    for (unsigned n = 0; n < 5; ++n) want.pq[n][1] = q;
    want.img[BITS + 7] = 0x02; want.bits = 2; want.rec[1] = q;
    same(&after, &want);
    CHECK(get8d(f, PORT_EA) == 1 && get64(f, B1 + BITS) == 2);
    for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[16] == in->queue && grec(f)->threads[n].port_queue[0] == 0);
    /* the internal binding, ports and the 8 raw bytes are exactly as hand-derived: 00 00 00 00 00 00 00 02 */
    uint8_t raw[8]; CHECK(ps3_atomic_read(&f->domain, B1 + BITS, raw, 8) == PS3_GUEST_MEMORY_OK);
    static const uint8_t be2[8] = {0, 0, 0, 0, 0, 0, 0, 2};
    CHECK(memcmp(raw, be2, 8) == 0);
    /* reservation on the DA8 line lost by the committing CAS; the unrelated reservation survives */
    int lost = 0;
    CHECK(ps3_atomic_owner_take_reservation_loss(&f->domain, on_bits, &lost) == PS3_GUEST_MEMORY_OK && lost == 1);
    CHECK(ps3_atomic_owner_take_reservation_loss(&f->domain, on_other, &lost) == PS3_GUEST_MEMORY_OK && lost == 0);
    /* the borrowed queue: untouched, empty, its receiver still blocked, nothing sent or woken */
    ps3_lv2_queue q1;
    CHECK(ps3_lv2_queue_snapshot(&f->lv2, q, &q1) == PS3_LV2_OK && q1.count == 0 && q1.receivers == 1 && q1.blocked_receivers == 1 && !q1.service_bound);
    CHECK(q1.sequence == q0.sequence && !rv.done);
    expect_quiet_kernel(f);
    CHECK(in->state == PS3_SPURS_INSTANCE_ACTIVE);
    /* a second attach of the same port is BUSY and changes nothing */
    AMem m2; amem(&m2, f);
    world(f, &before);
    r = attach(f, &m2, req(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == PS3_SPURS_RC_BUSY && m2.writes == 0 && m2.cas == 0);
    world(f, &after);
    same(&before, &after);
    CHECK(ps3_atomic_owner_release(&f->domain, &on_bits) == PS3_GUEST_MEMORY_OK && ps3_atomic_owner_release(&f->domain, &on_other) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    recv_stop(&rv);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    finish(f);
}

static void ports_and_prior_bits_lane(void) {
    /* concurrent/prior bits are preserved; a bit already set is an idempotent OR; ports 0 and 15 are valid */
    static const struct { uint8_t port; uint64_t prior; } cases[] = {
        {1, UINT64_C(0x8000000000000040)}, {1, UINT64_C(0x0000000000000002)}, {0, UINT64_C(0xFFFFFFFFFFFFFFFE)},
        {15, UINT64_C(0x00000001FFFF0000)}, {1, UINT64_C(0xFFFFFFFFFFFFFFFD)},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        Fixture *f = boot();
        const uint32_t q = mkq(f, PS3_PPU_QUEUE);
        put64d(f, B1 + BITS, cases[c].prior);
        put8d(f, PORT_EA, cases[c].port);
        AMem m; amem(&m, f);
        ps3_spurs_attach_info info;
        ps3_spurs_result r = attach(f, &m, req(q), &info);
        const uint64_t bit = UINT64_C(1) << cases[c].port;
        CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && info.spup == cases[c].port && get8d(f, PORT_EA) == cases[c].port);
        CHECK(get64(f, B1 + BITS) == (cases[c].prior | bit));
        CHECK(info.prior_port_bits == cases[c].prior && info.prior_bit_set == ((cases[c].prior & bit) != 0));
        CHECK(m.log[2].kind == 'R' && m.log[4].kind == 'C' && m.log[4].a == cases[c].prior && m.log[4].b == (cases[c].prior | bit));
        for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[cases[c].port] == q);
        CHECK(inst(f)->user_port_bits == bit && inst(f)->user_port_queue[cases[c].port] == q);
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
        finish(f);
    }
}

static void cas_retry_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    put64d(f, B1 + BITS, 0x20);
    /* a concurrent writer ORs bit 0x200 between the seed read and the first CAS: strong CAS fails, retry uses ITS observed value */
    AMem m; amem(&m, f); m.interfere_from = 1; m.interfere_count = 1;
    ps3_spurs_attach_info info;
    ps3_spurs_result r = attach(f, &m, req(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && info.cas_attempts == 2 && m.cas == 2 && m.reads == 3);   /* exactly ONE seed read */
    CHECK(m.log[4].kind == 'C' && m.log[4].a == 0x20 && m.log[4].b == 0x22);
    CHECK(m.log[5].kind == 'C' && m.log[5].a == 0x220 && m.log[5].b == 0x222);
    CHECK(get64(f, B1 + BITS) == 0x222 && info.prior_port_bits == 0x220 && !info.prior_bit_set);   /* concurrent bit preserved */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    finish(f);
    /* the attempt cap is explicit and exact: cap N with a writer that always wins -> N CAS calls, then exact rollback + STOP */
    for (uint32_t cap = 1; cap <= 3; ++cap) {
        f = boot();
        const uint32_t q2 = mkq(f, PS3_PPU_QUEUE);
        World before, after;
        world(f, &before);
        amem(&m, f); m.interfere_from = 1; m.interfere_count = 100;
        ps3_spurs_attach_request rq = req(q2); rq.max_cas_attempts = cap;
        r = attach(f, &m, rq, &info);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_ATTACH_FAILED && r.step == PS3_SPURS_ATTACH_STEP_BITS_CAS && r.detail == cap);
        CHECK(m.cas == cap && info.cas_attempts == cap && info.port_written == 1 && info.cause == PS3_SPURS_STOP_ATTACH_FAILED);
        world(f, &after);
        for (unsigned n = 0; n < 5; ++n) CHECK(after.pq[n][1] == 0);        /* exact detach confirmed ... */
        CHECK(after.bits == 0 && after.rec[1] == 0);                         /* ... so the record is cleared */
        CHECK(inst(f)->state == PS3_SPURS_INSTANCE_RETAINED);
        CHECK((get64(f, B1 + BITS) & 2) == 0);                               /* DA8 bit never set; only the concurrent bits */
        (void)before;
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, q2) == PS3_LV2_OK);
        finish(f);
    }
}

/* ---- pre-effect lane: every case leaves the observable world bit-identical ---- */
static ps3_spurs_result pre(Fixture *f, AMem *m, ps3_spurs_attach_request rq, ps3_spurs_outcome outcome, uint32_t rc, uint32_t reason, uint32_t step) {
    World a, b;
    world(f, &a);
    amem(m, f);
    ps3_spurs_attach_info info;
    ps3_spurs_result r = attach(f, m, rq, &info);
    CHECK(r.outcome == outcome && r.rc == rc && r.reason == reason && r.step == step);
    CHECK(m->writes == 0 && m->cas == 0 && info.port_written == 0 && info.cas_attempts == 0);
    world(f, &b);
    same(&a, &b);
    CHECK(inst(f)->state == PS3_SPURS_INSTANCE_ACTIVE);
    expect_quiet_kernel(f);
    return r;
}
#define FAILED_RC(rcv) PS3_SPURS_OUTCOME_FAILED, (rcv), 0
static void validation_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    AMem m;
    ps3_spurs_result r;
    /* caller contract (host bug): STOP before ANY callback, even ahead of the guest-visible NULL check */
    {
        ps3_spurs_attach_request rq = req(q);
        ps3_spurs_attach_memory good, mem;
        amem(&m, f);
        good = mport(&m);
        World a, b; world(f, &a);
        r = ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, NULL, &good, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST && r.step == PS3_SPURS_ATTACH_STEP_REQUEST);
        r = ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, &rq, NULL, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST);
        for (unsigned miss = 0; miss < 3; ++miss) {
            mem = good;
            if (miss == 0) mem.read = NULL; else if (miss == 1) mem.write = NULL; else mem.compare_exchange64 = NULL;
            r = ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, &rq, &mem, NULL);
            CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST && r.step == PS3_SPURS_ATTACH_STEP_REQUEST);
        }
        rq.max_cas_attempts = 0;
        r = ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, &rq, &good, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST && r.step == PS3_SPURS_ATTACH_STEP_REQUEST);
        rq.spurs_ea = 0;   /* zero cap wins over the guest NULL check: it is a host contract error */
        r = ps3_spurs_runtime_attach_lv2_event_queue(&f->rt, &rq, &good, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST);
        CHECK(m.n == 0);
        ps3_spurs_runtime dead; memset(&dead, 0, sizeof dead);
        rq = req(q);
        r = ps3_spurs_runtime_attach_lv2_event_queue(&dead, &rq, &good, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST && m.n == 0);
        r = ps3_spurs_runtime_attach_lv2_event_queue(NULL, &rq, &good, NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_INVALID_REQUEST && m.n == 0);
        world(f, &b); same(&a, &b);
    }
    /* guest-visible reference errors, in reference order; NULL/ALIGN answer before any ownership lookup or memory access */
    ps3_spurs_attach_request rq = req(q);
    rq.port_ea = 0;               pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_NULL), PS3_SPURS_ATTACH_STEP_REQUEST); CHECK(m.n == 0);
    rq = req(q); rq.spurs_ea = 0; pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_NULL), PS3_SPURS_ATTACH_STEP_REQUEST); CHECK(m.n == 0);
    rq = req(q); rq.spurs_ea = B1 + 0x40; pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_ALIGN), PS3_SPURS_ATTACH_STEP_REQUEST); CHECK(m.n == 0);
    rq = req(q); rq.spurs_ea = B1 + 1; pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_ALIGN), PS3_SPURS_ATTACH_STEP_REQUEST); CHECK(m.n == 0);
    /* aligned but not an instance this runtime owns: explicit stop */
    rq = req(q); rq.spurs_ea = B2;
    pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_ATTACH_STEP_OWNER); CHECK(m.n == 0);
    /* exception != 0 -> STAT, after reading exactly the u32 at +0xD6C */
    put64d(f, B1 + 0xD68, UINT64_C(0x0000000100000001));     /* enableEH 1 (initialize image) and exception 1 */
    rq = req(q);
    pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_STAT), PS3_SPURS_ATTACH_STEP_EXCEPTION); CHECK(m.reads == 1 && m.log[0].ea == B1 + EXC && m.log[0].len == 4);
    put64d(f, B1 + 0xD68, UINT64_C(0x0000000100000000));     /* exception 0 again, enableEH preserved */
    /* g12c: the dynamic path is now SERVED, so the port byte is no longer read or validated:
     * it is output-only. The real dynamic lane below covers it; here we only prove the static contract is untouched by
     * asserting that the static port byte is still read exactly where it always was. */
    /* port byte rules */
    static const struct { uint8_t port; uint32_t sdk; int stop; uint32_t rc; } ports[] = {
        {0x40, SDK, 0, PS3_SPURS_RC_INVAL}, {0xFF, SDK, 0, PS3_SPURS_RC_INVAL}, {0x40, 0x17FFFF, 0, PS3_SPURS_RC_INVAL},
        {0x10, 0x180000, 0, PS3_SPURS_RC_PERM}, {0x3F, SDK, 0, PS3_SPURS_RC_PERM}, {0x10, 0x180001, 0, PS3_SPURS_RC_PERM},
        {0x10, 0x17FFFF, 1, 0}, {0x3F, 0x17FFFF, 1, 0}, {0x10, 0, 1, 0},
    };
    for (unsigned i = 0; i < sizeof ports / sizeof ports[0]; ++i) {
        put8d(f, PORT_EA, ports[i].port);
        rq = req(q); rq.sdk_version = ports[i].sdk;
        if (ports[i].stop) pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_PORT_RANGE);
        else pre(f, &m, rq, FAILED_RC(ports[i].rc), PS3_SPURS_ATTACH_STEP_PORT_RANGE);
        CHECK(m.reads == 2 && m.log[1].ea == PORT_EA && m.log[1].len == 1);
    }
    put8d(f, PORT_EA, 1);
    /* a memory-port read that was not performed: STOP before the connect, nothing changed */
    for (unsigned at = 1; at <= 3; ++at) {
        World a, b; world(f, &a);
        amem(&m, f); m.fail_read_at = at;
        ps3_spurs_attach_info info;
        r = attach(f, &m, req(q), &info);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_READ_FAILED && r.detail == (uint32_t)-1);
        CHECK(r.step == (at == 1 ? PS3_SPURS_ATTACH_STEP_EXCEPTION : at == 2 ? PS3_SPURS_ATTACH_STEP_PORT_READ : PS3_SPURS_ATTACH_STEP_BITS_READ));
        CHECK(m.writes == 0 && m.cas == 0 && m.reads == at);
        world(f, &b); same(&a, &b);
    }
    /* F3: an ABSENT queue (snapshot ESRCH) answers ESRCH at step QUEUE BEFORE the DA8 seed read and the mutating connect, so a
     * queue created concurrently under that id can never be bound past the type/service-bound preflight */
    const uint32_t dead = mkq(f, PS3_PPU_QUEUE);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, dead) == PS3_LV2_OK);
    static const uint32_t bad_queues[2] = {0, 0x8D00FF00u};
    for (unsigned i = 0; i < 3; ++i) {
        rq = req(i == 2 ? dead : bad_queues[i]);
        pre(f, &m, rq, FAILED_RC(LV2_ESRCH), PS3_SPURS_ATTACH_STEP_QUEUE);
        CHECK(m.reads == 2 && m.log[0].ea == B1 + EXC && m.log[1].ea == PORT_EA);   /* no DA8 seed read, hence no connect */
    }
    /* supported-invalid queues stop explicitly: non-PPU type, SPURS's own internal queue, a service-bound queue */
    const uint32_t spuq = mkq(f, PS3_SPU_QUEUE), svc = mkq(f, PS3_PPU_QUEUE);
    const ps3_lv2_event ev = {0, 0, 1, 0};
    CHECK(ps3_lv2_queue_bind_service(&f->lv2, svc, ev) == PS3_LV2_OK);
    const uint32_t unsupported[3] = {spuq, inst(f)->queue, svc};
    for (unsigned i = 0; i < 3; ++i) {
        rq = req(unsupported[i]);
        pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNSUPPORTED_LV2, PS3_SPURS_ATTACH_STEP_QUEUE);
        CHECK(m.reads == 2);
    }
    CHECK(ps3_lv2_queue_unbind_service(&f->lv2, svc) == PS3_LV2_OK);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, spuq) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, svc) == PS3_LV2_OK);
    finish(f);
}

/* ---- all-or-none busy, dead foreign bindings, foreign bindings, several ports ---- */
static void busy_and_foreign_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE), foreign = mkq(f, PS3_PPU_QUEUE);
    AMem m;
    /* ONE SPU already holds port 1 for a live foreign queue: BUSY (EISCONN), no thread changes, nothing recorded or written */
    grec(f)->threads[3].port_queue[1] = foreign;
    ps3_spurs_result r = pre(f, &m, req(q), FAILED_RC(PS3_SPURS_RC_BUSY), PS3_SPURS_ATTACH_STEP_CONNECT);
    (void)r;
    for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[1] == (n == 3 ? foreign : 0));
    /* L4: once that foreign queue is destroyed its binding is dead and no longer occupies the port */
    CHECK(ps3_lv2_queue_destroy(&f->lv2, foreign) == PS3_LV2_OK);
    amem(&m, f);
    CHECK(attach(f, &m, req(q), NULL).outcome == PS3_SPURS_OUTCOME_OK);
    for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[1] == q);
    CHECK(inst(f)->user_port_bits == 2 && inst(f)->user_port_queue[1] == q);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    finish(f);

    /* an independent binding on port 2 survives; two ports, two borrowed queues; port 2 stays BUSY */
    f = boot();
    const uint32_t qa = mkq(f, PS3_PPU_QUEUE), qb = mkq(f, PS3_PPU_QUEUE), qf = mkq(f, PS3_PPU_QUEUE);
    uint8_t spup = 0xFF;
    CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, inst(f)->group, qf, UINT64_C(1) << 2, &spup) == PS3_LV2_OK && spup == 2);
    amem(&m, f);
    CHECK(attach(f, &m, req(qa), NULL).outcome == PS3_SPURS_OUTCOME_OK);
    put8d(f, PORT_EA, 3);
    amem(&m, f);
    CHECK(attach(f, &m, req(qb), NULL).outcome == PS3_SPURS_OUTCOME_OK);
    put8d(f, PORT_EA, 2);
    r = pre(f, &m, req(qa), FAILED_RC(PS3_SPURS_RC_BUSY), PS3_SPURS_ATTACH_STEP_CONNECT);
    for (unsigned n = 0; n < 5; ++n)
        CHECK(grec(f)->threads[n].port_queue[1] == qa && grec(f)->threads[n].port_queue[2] == qf && grec(f)->threads[n].port_queue[3] == qb);
    CHECK(inst(f)->user_port_bits == 0xA && inst(f)->user_port_queue[1] == qa && inst(f)->user_port_queue[3] == qb && inst(f)->user_port_queue[2] == 0);
    CHECK(get64(f, B1 + BITS) == 0xA);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);   /* both user bindings are detached, the foreign one is not SPURS's to touch */
    CHECK(ps3_lv2_queue_destroy(&f->lv2, qa) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, qb) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, qf) == PS3_LV2_OK);
    finish(f);
}

/* ---- failure after the connect: exact rollback; a committed CAS is never undone ---- */
static void rollback_lane(void) {
    for (unsigned mode = 0; mode < 2; ++mode) {   /* 0: byte write not performed, 1: CAS not performed */
        Fixture *f = boot();
        const uint32_t q = mkq(f, PS3_PPU_QUEUE);
        put64d(f, B1 + BITS, 0x40);
        World before, after, want;
        world(f, &before);
        AMem m; amem(&m, f);
        if (mode == 0) m.fail_write_at = 1; else m.fail_cas_at = 1;
        ps3_spurs_attach_info info;
        ps3_spurs_result r = attach(f, &m, req(q), &info);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_WRITE_FAILED && r.detail == (uint32_t)-1);
        CHECK(r.step == (mode == 0 ? PS3_SPURS_ATTACH_STEP_PORT_WRITE : PS3_SPURS_ATTACH_STEP_BITS_CAS) && info.cause == PS3_SPURS_STOP_WRITE_FAILED);
        CHECK(info.port_written == mode && m.writes == 1 && m.cas == mode);
        world(f, &after);
        want = before; want.state = PS3_SPURS_INSTANCE_RETAINED;   /* binding detached exactly, record cleared, terminal stop */
        same(&after, &want);
        CHECK(get64(f, B1 + BITS) == 0x40);
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
        finish(f);
    }
    /* F2: the CAS commits and THEN its callback reports a failure (a trace recorder). The commit is never undone, but it is
     * reported as a STOP (never OK), with or without info: the instance is RETAINED, every binding/ledger/DA8/port byte stays */
    for (unsigned with_info = 0; with_info < 2; ++with_info) {
        Fixture *f = boot();
        const uint32_t q = mkq(f, PS3_PPU_QUEUE);
        World before, after;
        world(f, &before);
        AMem m; amem(&m, f); m.commit_then_fail_at = 1;
        ps3_spurs_attach_info info; memset(&info, 0xAB, sizeof info);
        ps3_spurs_result r = attach(f, &m, req(q), with_info ? &info : NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.rc == 0 && r.reason == PS3_SPURS_STOP_WRITE_FAILED &&
              r.step == PS3_SPURS_ATTACH_STEP_COMMITTED && r.detail == 7);
        CHECK(m.cas == 1 && m.writes == 1);   /* no detach, no rollback write, no retry */
        if (with_info) {
            CHECK(info.spup == 1 && info.port_written == 1 && info.cas_attempts == 1 && info.prior_port_bits == 0 && !info.prior_bit_set &&
                  info.commit_status == 7 && info.cause == 0);
        }
        CHECK(get64(f, B1 + BITS) == 2 && get8d(f, PORT_EA) == 1);   /* DA8 bit and port byte stay */
        ps3_spurs_instance *in = inst(f);
        CHECK(in->user_port_bits == 2 && in->user_port_queue[1] == q && in->state == PS3_SPURS_INSTANCE_RETAINED);   /* ledger kept, terminal */
        for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[1] == q);   /* ALL SPU bindings kept */
        world(f, &after);
        CHECK(after.sem == before.sem && after.que == before.que && after.prt == before.prt && after.ppu == before.ppu &&
              after.used == before.used && after.owners == before.owners && after.ls_used == before.ls_used);   /* residue/cleanup queue intact */
        {   /* a retained instance accepts no further attach */
            AMem m2; amem(&m2, f);
            r = attach(f, &m2, req(q), NULL);
            CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_ATTACH_FAILED && r.step == PS3_SPURS_ATTACH_STEP_OWNER && m2.n == 0);
        }
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);   /* shutdown detaches exactly; DA8 residue is not cleared */
        CHECK(in->state == PS3_SPURS_INSTANCE_FREE && in->user_port_bits == 0 && in->user_port_queue[1] == 0 && get64(f, B1 + BITS) == 2);
        CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 1);   /* the borrowed queue survives */
        CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
        finish(f);
    }
}

/* F5: test-only link wrap of the policy connect (verifier passes -Wl,--wrap=...): the REAL connect always runs; when armed the
 * returned port is then replaced, to model a sibling policy bug that answers OK with spup != the requested port. */
uint32_t __real_ps3_spu_group_connect_event_all_threads(ps3_spu_resource_policy *, uint32_t, uint32_t, uint64_t, uint8_t *);
static int g_fault_armed; static uint8_t g_fault_spup;
uint32_t __wrap_ps3_spu_group_connect_event_all_threads(ps3_spu_resource_policy *p, uint32_t group, uint32_t queue, uint64_t request, uint8_t *spup) {
    const uint32_t rc = __real_ps3_spu_group_connect_event_all_threads(p, group, queue, request, spup);
    if (rc == PS3_LV2_OK && g_fault_armed) *spup = g_fault_spup;
    return rc;
}

/* ---- F1: sdk_version is the EFFECTIVE PROCESS sdk (unsigned field, signed reference semantics; -1 is mapped by the adapter) ---- */
static void sdk_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    AMem m;
    ps3_spurs_attach_request rq;
    /* negative (> INT32_MAX) unmapped input is a host bug: STOP before ANY callback, ahead of every guest-visible check */
    static const uint32_t negative[] = {0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};
    for (unsigned i = 0; i < sizeof negative / sizeof negative[0]; ++i) {
        rq = req(q); rq.sdk_version = negative[i];
        pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_REQUEST);
        CHECK(m.n == 0);
        rq.spurs_ea = 0;   /* even ahead of the guest-visible NULL */
        pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_REQUEST);
        CHECK(m.n == 0);
    }
    /* the largest valid positive value compares as a new SDK */
    put8d(f, PORT_EA, 16);
    rq = req(q); rq.sdk_version = 0x7FFFFFFFu;
    pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_PERM), PS3_SPURS_ATTACH_STEP_PORT_RANGE);
    rq.sdk_version = 0x180000;   /* boundary: PERM */
    pre(f, &m, rq, FAILED_RC(PS3_SPURS_RC_PERM), PS3_SPURS_ATTACH_STEP_PORT_RANGE);
    rq.sdk_version = 0x17FFFF;   /* old SDK, port 16: legal in the reference but not modeled here -> STOP */
    pre(f, &m, rq, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_PORT_RANGE);
    /* old SDK, port 15: binds (OK) */
    put8d(f, PORT_EA, 15);
    amem(&m, f);
    ps3_spurs_attach_info info;
    rq = req(q); rq.sdk_version = 0x17FFFF;
    ps3_spurs_result r = attach(f, &m, rq, &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && info.spup == 15 && get8d(f, PORT_EA) == 15 && get64(f, B1 + BITS) == (UINT64_C(1) << 15));
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    finish(f);
}

/* ---- F5: a policy that answers OK with spup != the validated requested port: the ledger/rollback use the REQUESTED port only ---- */
static void spup_mismatch_lane(void) {
    static const uint8_t faults[] = {7, 0, 15, 63, 64, 0xFF};
    for (unsigned i = 0; i < sizeof faults; ++i) {
        Fixture *f = boot();
        const uint32_t q = mkq(f, PS3_PPU_QUEUE);
        World before, after, want;
        world(f, &before);
        AMem m; amem(&m, f);
        ps3_spurs_attach_info info;
        g_fault_spup = faults[i]; g_fault_armed = 1;   /* the real connect binds port 1 on every thread, then the returned port is replaced */
        ps3_spurs_result r = attach(f, &m, req(q), &info);
        g_fault_armed = 0;
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_PROVIDER_FAILURE && r.step == PS3_SPURS_ATTACH_STEP_CONNECT);
        CHECK(r.detail == faults[i] && info.cause == PS3_SPURS_STOP_PROVIDER_FAILURE && info.spup == 1);   /* detail names the bad spup */
        CHECK(m.writes == 0 && m.cas == 0 && info.port_written == 0 && info.cas_attempts == 0);
        world(f, &after);
        want = before; want.state = PS3_SPURS_INSTANCE_RETAINED;   /* exact rollback on the requested port: bindings gone, ledger empty */
        same(&after, &want);
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
        finish(f);
    }
}

static uint32_t g_foreign;
static void take_over_thread3(AMem *m) { grec(m->f)->threads[3].port_queue[1] = g_foreign; }
static void retained_rollback_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    g_foreign = mkq(f, PS3_PPU_QUEUE);
    ps3_spurs_instance *in = inst(f);
    /* the write fails AND another owner took one SPU's binding in between: the exact detach refuses (no forward effects) */
    AMem m; amem(&m, f); m.fail_write_at = 1; m.hook_write_at = 1; m.hook = take_over_thread3;
    ps3_spurs_attach_info info;
    ps3_spurs_result r = attach(f, &m, req(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_ROLLBACK_FAILED && r.step == PS3_SPURS_ATTACH_STEP_PORT_WRITE);
    CHECK(r.detail == LV2_ESRCH && info.cause == PS3_SPURS_STOP_WRITE_FAILED);
    for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[1] == (n == 3 ? g_foreign : q));   /* nothing stripped */
    CHECK(in->user_port_bits == 2 && in->user_port_queue[1] == q && in->state == PS3_SPURS_INSTANCE_RETAINED);   /* record retained */
    /* a retained instance accepts no further attach */
    {
        AMem m2; amem(&m2, f);
        r = attach(f, &m2, req(q), NULL);
        CHECK(r.outcome == PS3_SPURS_OUTCOME_STOP && r.reason == PS3_SPURS_STOP_ATTACH_FAILED && r.step == PS3_SPURS_ATTACH_STEP_OWNER && m2.n == 0);
    }
    /* shutdown: executors are joined and owners released, the failed user detach keeps the group alive and the record */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1);
    CHECK(!in->handler_live && !in->kernel_initialized && !in->helper_live && in->owners_acquired == 0 && in->unwinding_failed);
    CHECK(in->group_created == 1 && in->state != PS3_SPURS_INSTANCE_FREE && in->user_port_bits == 2 && in->user_port_queue[1] == q);
    ps3_spu_group_record g;
    CHECK(ps3_spu_group_snapshot(&f->policy, in->group, &g) == PS3_LV2_OK);
    for (unsigned n = 0; n < 5; ++n) CHECK(g.threads[n].port_queue[1] == (n == 3 ? g_foreign : q));
    CHECK(in->semaphore_count == 0 && !in->queue_created && !in->port_created && ps3_ppu_threads_live(&f->ppu) == 0);   /* the rest still unwound */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 1);   /* retry while the mismatch persists: same answer, still nothing stripped */
    CHECK(grec(f)->threads[3].port_queue[1] == g_foreign && grec(f)->threads[0].port_queue[1] == q);
    /* the foreign owner is gone: the retry detaches exactly, then destroys the group and returns the record */
    grec(f)->threads[3].port_queue[1] = q;
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(in->state == PS3_SPURS_INSTANCE_FREE && in->user_port_bits == 0 && in->user_port_queue[1] == 0 && in->group_created == 0);
    CHECK(ps3_spu_group_snapshot(&f->policy, 0x04000100u, &g) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 2);   /* both borrowed queues survive: SPURS never destroys them */
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f->lv2, g_foreign) == PS3_LV2_OK);
    finish(f);
}

/* g12c: the DYNAMIC port mode
 * The firmware's 0xAF2C with r6 != 0: the request mask is 0xFFFFFFFFFFFF0000 (ports 16..63), the
 * requested-port byte at *r31 is NEVER read, one connect syscall picks the lowest free port in the mask, and the keyed flag
 * (r7 = 1 from 0xB0D0) means the guest 0xDA8 word is not written at all. Expected ports are hand-derived from the
 * policy's scan (`for port in 0..63, first bit in req whose binding is free`), never from the implementation. */
static ps3_spurs_attach_request dreq(uint32_t queue) { return (ps3_spurs_attach_request){B1, queue, PORT_EA, 1, SDK, 4}; }
/* The port syscall 251 answers for the firmware's mask: the first bit of 0xFFFFFFFFFFFF0000 whose binding is free on every
 * thread of the group and whose stored queue is still live (the policy's own rule). Derived from the real records. */
static uint8_t dyn_want_port(Fixture *f) {
    const ps3_spu_group_record *g = grec(f);
    for (unsigned port = 16; port < PS3_SPU_PORTS; ++port) {
        int taken = 0;
        for (uint32_t n = 0; n < g->num; ++n) {
            ps3_lv2_queue probe;
            taken |= g->threads[n].port_queue[port] != 0 &&
                ps3_lv2_queue_snapshot(&f->lv2, g->threads[n].port_queue[port], &probe) == PS3_LV2_OK;
        }
        if (!taken) return (uint8_t)port;
    }
    return 0xFF;
}

static void dynamic_first_free_lane(void) {
    Fixture *f = boot();
    ps3_spurs_instance *in = inst(f);
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    /* The port byte holds a value the static mode would REFUSE (0xFF is > 63 -> INVAL, 0x00 would bind port 0): the
     * dynamic path must not look at it, so neither refusal may happen and *port must come back as the kernel's choice. */
    put8d(f, PORT_EA, 0xFF);
    /* The SPURS instance's OWN internal queue already occupies one port in 16..63 (it is connected with the same
     * ~0 << 16 mask at initialize), so the first FREE port in the firmware's mask is whatever the policy's scan answers.
     * `want` is derived the way syscall 251 answers, from the real records -- never from the implementation. */
    const uint8_t want = dyn_want_port(f);
    CHECK(want >= 16 && want < 64);
    AMem m; amem(&m, f);
    ps3_spurs_attach_info info;
    ps3_spurs_result r = attach(f, &m, dreq(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.rc == 0 && r.step == PS3_SPURS_ATTACH_STEP_DONE && r.instance == 0);
    CHECK(info.spup == want && info.port_written == 1 && info.cas_attempts == 0 && info.commit_status == 0);
    CHECK(info.prior_port_bits == 0 && !info.prior_bit_set && info.cause == 0);
    CHECK(get8d(f, PORT_EA) == want);
    for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[want] == q);
    /* The guest DA8 word is UNTOUCHED: the firmware's keyed path skips the atomic OR at 0xB074/0xB080. */
    CHECK(get64(f, B1 + BITS) == 0);
    /* The native ledger still records the binding, on the CHOSEN port, so shutdown detaches exactly. */
    CHECK(in->user_port_bits == (UINT64_C(1) << want) && in->user_port_queue[want] == q);
    /* Memory traffic, exactly: the 0xD6C exception read, then the port-byte WRITE. No port read, no DA8 seed read, no
     * CAS -- the three callbacks the static path makes and this one does not. */
    CHECK(m.n == 2 && m.reads == 1 && m.writes == 1 && m.cas == 0);
    CHECK(m.log[0].kind == 'R' && m.log[0].ea == B1 + EXC && m.log[0].len == 4);
    CHECK(m.log[1].kind == 'W' && m.log[1].ea == PORT_EA && m.log[1].len == 1 && m.log[1].a == want);
    CHECK(in->state == PS3_SPURS_INSTANCE_ACTIVE);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(in->state == PS3_SPURS_INSTANCE_FREE && in->user_port_bits == 0 && in->user_port_queue[want] == 0);
    ps3_spu_group_record g;
    CHECK(ps3_spu_group_snapshot(&f->policy, 0x04000100u, &g) == PS3_LV2_ESRCH);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    finish(f);
}

static void dynamic_skips_occupied_lane(void) {
    /* Occupy a growing prefix of 16.. and prove the kernel's scan resumes exactly at the first free bit. The lv2 registry
     * holds only 8 queues, so at most 5 real ones can be spent here. */
    static const uint8_t occupied[] = {0, 1, 2, 5};
    uint8_t base = 0;   /* the answer with nothing extra occupied (the instance's own internal queue holds the lowest one) */
    for (unsigned c = 0; c < sizeof occupied / sizeof occupied[0]; ++c) {
        Fixture *f = boot();
        const uint32_t q = mkq(f, PS3_PPU_QUEUE);
        const uint8_t first = dyn_want_port(f);
        /* A live foreign queue on each of the `occupied[c]` lowest free ports of the mask. */
        for (uint8_t k = 0; k < occupied[c]; ++k) {
            const uint32_t foreign = mkq(f, PS3_PPU_QUEUE);
            uint8_t spup = 0xFF;
            CHECK(ps3_spu_group_connect_event_all_threads(&f->policy, inst(f)->group, foreign,
                UINT64_C(1) << (first + k), &spup) == PS3_LV2_OK && spup == first + k);
        }
        AMem m; amem(&m, f);
        ps3_spurs_attach_info info;
        const uint8_t want = dyn_want_port(f);
        const ps3_spurs_result r = attach(f, &m, dreq(q), &info);
        if (c == 0) base = want;
        /* Occupying `occupied[c]` more low ports of the mask shifts the scan up by exactly that many ports. */
        CHECK(want == (uint8_t)(base + occupied[c]));
        CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && info.spup == want && get8d(f, PORT_EA) == want);
        for (unsigned n = 0; n < 5; ++n) CHECK(grec(f)->threads[n].port_queue[want] == q);
        CHECK(get64(f, B1 + BITS) == 0);   /* still no guest DA8 write */
        CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
        finish(f);
    }
}

static void dynamic_all_full_lane(void) {
    /* Every port 16..63 occupied by a live foreign queue: sc 251 answers EISCONN, which 0xAF2C maps to 0x8041070A
     * (0xB050../0xB0B0). Nothing is bound, nothing is written, nothing is recorded. The lv2 registry holds only 8 queues,
     * so the occupancy is written into the policy's per-thread port table directly (the same white-box technique the
     * borrowed-busy cases above use) against ONE real live foreign queue, which keeps every binding "live" under the
     * policy's L4 rule. */
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE), foreign = mkq(f, PS3_PPU_QUEUE);
    ps3_spu_group_record *g = grec(f);
    for (unsigned k = 0; k < 48; ++k)   /* 16..63 inclusive */
        for (uint32_t n = 0; n < g->num; ++n) g->threads[n].port_queue[16 + k] = foreign;
    World before, after;
    world(f, &before);
    AMem m; amem(&m, f);
    ps3_spurs_attach_info info;
    const ps3_spurs_result r = attach(f, &m, dreq(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_FAILED && r.rc == PS3_SPURS_RC_BUSY && r.step == PS3_SPURS_ATTACH_STEP_CONNECT);
    CHECK(r.rc == 0x8041070Au);   /* the firmware's collapsed EISCONN code */
    CHECK(m.reads == 1 && m.writes == 0 && m.cas == 0 && info.port_written == 0 && info.cas_attempts == 0);
    world(f, &after);
    same(&before, &after);
    CHECK(inst(f)->state == PS3_SPURS_INSTANCE_ACTIVE);
    CHECK(get64(f, B1 + BITS) == 0);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, foreign) == PS3_LV2_OK);
    finish(f);
}

static void dynamic_static_unchanged_lane(void) {
    /* The static mode is byte-identical: same memory traffic, same port from the requested byte, and the DA8 OR still
     * happens. This is the direct contrast with dynamic_first_free_lane's two-callback shape. */
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    put8d(f, PORT_EA, 3);
    AMem m; amem(&m, f);
    ps3_spurs_attach_info info;
    const ps3_spurs_result r = attach(f, &m, req(q), &info);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && info.spup == 3 && get8d(f, PORT_EA) == 3);
    CHECK(m.reads == 3 && m.writes == 1 && m.cas == 1);   /* exception, port byte, DA8 seed / port write / DA8 CAS */
    CHECK(get64(f, B1 + BITS) == (UINT64_C(1) << 3));
    CHECK(inst(f)->user_port_bits == (UINT64_C(1) << 3) && inst(f)->user_port_queue[3] == q);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    finish(f);
}

/* ---- shutdown never owns, drains, wakes or destroys the borrowed queue ---- */
static void shutdown_lane(void) {
    Fixture *f = boot();
    const uint32_t q = mkq(f, PS3_PPU_QUEUE);
    Recv rv;
    recv_start(&rv, f, q);
    AMem m; amem(&m, f);
    CHECK(attach(f, &m, req(q), NULL).outcome == PS3_SPURS_OUTCOME_OK);
    ps3_spurs_instance *in = inst(f);
    ps3_lv2_queue q0, q1;
    CHECK(ps3_lv2_queue_snapshot(&f->lv2, q, &q0) == PS3_LV2_OK);
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);
    CHECK(in->state == PS3_SPURS_INSTANCE_FREE && in->user_port_bits == 0 && in->user_port_queue[1] == 0 && !in->group_created);
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE) == 0 && ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_PORT) == 0 &&
          ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 1 && ps3_ppu_threads_live(&f->ppu) == 0);   /* only the borrowed queue is left */
    CHECK(ps3_lv2_queue_snapshot(&f->lv2, q, &q1) == PS3_LV2_OK && q1.count == 0 && q1.receivers == 1 && q1.blocked_receivers == 1 &&
          !q1.service_bound && q1.sequence == q0.sequence && !rv.done);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == LV2_EBUSY);   /* its blocked owner-side receiver keeps it alive: unchanged lv2 rule */
    CHECK(ps3_spurs_runtime_shutdown(&f->rt) == 0);          /* idempotent */
    recv_stop(&rv);
    CHECK(ps3_lv2_queue_destroy(&f->lv2, q) == PS3_LV2_OK);
    CHECK(ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE) == 0);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) CHECK(!f->rt.ls_used[i]);
    finish(f);
}

/* PS3_SPURS_ATTACH_ONLY="name,name" runs only the named lanes (red-log selection); unset runs all. */
#define LANE(name) do { if (!only || strstr(only, #name)) name(); } while (0)
int main(void) {
    const char *only = getenv("PS3_SPURS_ATTACH_ONLY");
    LANE(policy_exact_lane);
    LANE(happy_lane);
    LANE(ports_and_prior_bits_lane);
    LANE(cas_retry_lane);
    LANE(validation_lane);
    LANE(sdk_lane);
    LANE(busy_and_foreign_lane);
    LANE(rollback_lane);
    LANE(retained_rollback_lane);
    LANE(spup_mismatch_lane);
    LANE(dynamic_first_free_lane);
    LANE(dynamic_skips_occupied_lane);
    LANE(dynamic_all_full_lane);
    LANE(dynamic_static_unchanged_lane);
    LANE(shutdown_lane);
    puts("test_spurs_user_attach: OK");
    return 0;
}
