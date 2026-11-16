#define _POSIX_C_SOURCE 200809L
/* Native cellSpursGetInfo (ps3_spurs_runtime_get_info) domain verifier, D1-D13 plus guest nSpus=255 bounds.
 * Real collaborators exactly as test_spurs_instance.c: coherent guest memory + atomic domain, shared user-memory quota, lv2
 * objects, SPU group policy, PPU SERVICE threads, the kernel-1 idle service. Two REAL initialized instances provide the ids.
 * The memory port is a logging double over the real atomic domain. Expected accesses and the expected 280 output bytes are
 * built here from LITERAL reference offsets (RPCS3 63996a47 cellSpurs.cpp:1970-2007, CellSpursInfo h:308-329); the F_ constants
 * of the domain are never used. Selector: PS3_SPURS_GETINFO_ONLY="lane,lane" (red-log selection). */
#include "atomic_domain.h"
#include "user_memory.h"
#include "guest_space_host.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "../domain/spurs_instance.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #x); abort(); } } while (0)
enum { SPACE = 0x00800000, WINDOW = 0x00400000, WINDOW_END = 0x00700000, QUOTA = 0x00300000, B1 = 0x007CF100, B2 = 0x007D0380,
       INFO = 0x007E0000, PAT = 0xA5, MARGIN = 0x40, UNOWNED = 0x007C0000, MAXLOG = 256 };

typedef struct Fixture_ {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard g_mem, g_lv2, g_ppu, g_pol, g_dom;
    ps3_pthread_wait_port port;
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
    ps3_notify_port np = ps3_pthread_wait_port_port(&f->port);
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
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned r = 0; r < 3; ++r) rc.threads[i][r] = ps3_pthread_service_thread_port(&f->th[i][r]);
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) { f->ls[i] = malloc(PS3_SPURS_K1_LS_BYTES); CHECK(f->ls[i]); memset(f->ls[i], PAT, PS3_SPURS_K1_LS_BYTES); rc.local_stores[i] = f->ls[i]; }
    rc.creator_priority = 250;
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) != 0 && !f->rt.initialized);   /* L5: no main id adopted -> refused */
    CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);               /* the admitted main thread (startup cpu.thread_id == 1) */
    CHECK(ps3_spurs_runtime_init(&f->rt, &rc) == 0);
    unsigned char fill[0x1200];
    memset(fill, PAT, sizeof fill);
    CHECK(ps3_atomic_write(&f->domain, B1 - 0x100, fill, sizeof fill) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_write(&f->domain, B2 - 0x100, fill, sizeof fill) == PS3_GUEST_MEMORY_OK);
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
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }
static void attribute(uint8_t *a, uint32_t rev, uint32_t nspus, uint32_t spu_prio, uint32_t ppu_prio, uint32_t prefix_size) {
    memset(a, 0, 512);
    p32(a, rev); p32(a + 4, 0x250001); p32(a + 8, nspus); p32(a + 12, spu_prio); p32(a + 16, ppu_prio);
    for (unsigned i = 0; i < 15; ++i) a[0x15 + i] = (uint8_t)('a' + i);
    p32(a + 0x24, prefix_size); p32(a + 0x28, 0);
}
static ps3_spurs_result init(Fixture *f, uint32_t b, const uint8_t *attr) {
    ps3_spurs_request r = {0xD00FF990u, b, attr};
    return ps3_spurs_runtime_initialize(&f->rt, &r);
}
/* ---- guest memory helpers (host side, coherent) ---- */
static void rd(Fixture *f, uint32_t a, void *b, size_t n) { CHECK(ps3_atomic_read(&f->domain, a, b, n) == PS3_GUEST_MEMORY_OK); }
static void wr(Fixture *f, uint32_t a, const void *b, size_t n) { CHECK(ps3_atomic_write(&f->domain, a, b, n) == PS3_GUEST_MEMORY_OK); }
static uint64_t rdv(Fixture *f, uint32_t a, unsigned n) { uint8_t b[8]; rd(f, a, b, n); uint64_t v = 0; for (unsigned i = 0; i < n; ++i) v = v << 8 | b[i]; return v; }
static void wrv(Fixture *f, uint32_t a, uint64_t v, unsigned n) { uint8_t b[8]; for (unsigned i = 0; i < n; ++i) b[i] = (uint8_t)(v >> (8 * (n - 1 - i))); wr(f, a, b, n); }
static void fill(Fixture *f, uint32_t a, size_t n) { uint8_t *p = malloc(n); CHECK(p); memset(p, PAT, n); wr(f, a, p, n); free(p); }

/* ---- the two real instances (literal ids derived from the real creation serials, cross-checked against the records) ---- */
typedef struct { uint32_t base, n, prio_spu, prio_ppu, prefix, group, helper, handler; } Inst;
static const Inst I0 = {B1, 5, 128, 249, 3, 0x04000100u, 0x01000000u, 0x01000001u};
static const Inst I1 = {B2, 1, 255, 249, 15, 0x04000200u, 0x01000002u, 0x01000003u};
static void two_instances(Fixture *f) {
    uint8_t a1[512], a2[512];
    attribute(a1, 2, I0.n, I0.prio_spu, I0.prio_ppu, I0.prefix);
    attribute(a2, 2, I1.n, I1.prio_spu, I1.prio_ppu, I1.prefix);
    ps3_spurs_result r = init(f, B1, a1);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.instance == 0);
    r = init(f, B2, a2);
    CHECK(r.outcome == PS3_SPURS_OUTCOME_OK && r.instance == 1);
    const Inst *want[2] = {&I0, &I1};
    for (unsigned i = 0; i < 2; ++i) {
        const ps3_spurs_instance *in = &f->rt.instances[i];
        CHECK(in->state == PS3_SPURS_INSTANCE_ACTIVE && in->spurs_ea == want[i]->base && in->nspus == want[i]->n);
        CHECK(in->group == want[i]->group && in->helper.id == want[i]->helper && in->handler.id == want[i]->handler);
        for (uint32_t k = 0; k < in->nspus; ++k) CHECK(in->spu_threads[k] == ((k << 24) | (want[i]->group & 0xFFFFFFu)));
    }
}

/* ---- independent expectation: guest image -> exact access list + output ---- */
typedef struct { uint32_t flags, spu_prio, ppu_prio, group, spus[8]; uint8_t nspus, pfx_size, pfx[16]; uint64_t ppu0, ppu1, tsize, traw; } G;
static void read_g(Fixture *f, uint32_t b, G *g) {
    memset(g, 0, sizeof *g);
    g->flags = (uint32_t)rdv(f, b + 0xD80, 4); g->nspus = (uint8_t)rdv(f, b + 0x76, 1);
    g->spu_prio = (uint32_t)rdv(f, b + 0xD84, 4); g->ppu_prio = (uint32_t)rdv(f, b + 0xD88, 4);
    g->group = (uint32_t)rdv(f, b + 0xD30, 4);
    for (unsigned k = 0; k < 8; ++k) g->spus[k] = (uint32_t)rdv(f, b + 0xD34 + 4 * k, 4);
    g->ppu0 = rdv(f, b + 0xD20, 8); g->ppu1 = rdv(f, b + 0xD28, 8);
    g->tsize = rdv(f, b + 0x948, 8); g->traw = rdv(f, b + 0x900, 8);
    g->pfx_size = (uint8_t)rdv(f, b + 0xD9B, 1);
    rd(f, b + 0xD8C, g->pfx, 15);
}
typedef struct { char op; uint32_t addr, len; uint64_t v; uint32_t step; } Ex;
enum { KIND_OK = 0, KIND_TRACE, KIND_PREFIX };
/* The normative access order (PLAN 3.2). Values: reads = the guest value, writes = the transformed BE value. */
static unsigned elist(const G *g, uint32_t B, uint32_t I, Ex *L, int *kind) {
    unsigned n = 0;
#define A(o, a, l, val, st) do { L[n].op = (o); L[n].addr = (a); L[n].len = (l); L[n].v = (val); L[n].step = (st); ++n; } while (0)
    *kind = KIND_OK;
    A('R', B + 0xD80, 4, g->flags, PS3_SPURS_INFO_STEP_FLAGS);
    A('R', B + 0x076, 1, g->nspus, PS3_SPURS_INFO_STEP_NSPUS);      A('W', I + 0x00, 4, g->nspus, PS3_SPURS_INFO_STEP_NSPUS);
    A('R', B + 0xD84, 4, g->spu_prio, PS3_SPURS_INFO_STEP_SPU_PRIORITY); A('W', I + 0x04, 4, g->spu_prio, PS3_SPURS_INFO_STEP_SPU_PRIORITY);
    A('R', B + 0xD88, 4, g->ppu_prio, PS3_SPURS_INFO_STEP_PPU_PRIORITY); A('W', I + 0x08, 4, g->ppu_prio, PS3_SPURS_INFO_STEP_PPU_PRIORITY);
    A('W', I + 0x0C, 1, (g->flags & 1) ? 1 : 0, PS3_SPURS_INFO_STEP_EXIT_FLAGS);
    A('W', I + 0x0D, 1, (g->flags & 4) ? 1 : 0, PS3_SPURS_INFO_STEP_EXIT_FLAGS);
    A('R', B + 0xD30, 4, g->group, PS3_SPURS_INFO_STEP_GROUP);      A('W', I + 0x24, 4, g->group, PS3_SPURS_INFO_STEP_GROUP);
    for (unsigned k = 0; k < 8; ++k) A('R', B + 0xD34 + 4 * k, 4, g->spus[k], PS3_SPURS_INFO_STEP_SPU_IDS);
    for (unsigned k = 0; k < 8; ++k) A('W', I + 0x28 + 4 * k, 4, g->spus[k], PS3_SPURS_INFO_STEP_SPU_IDS);
    A('R', B + 0xD20, 8, g->ppu0, PS3_SPURS_INFO_STEP_HANDLER0);    A('W', I + 0x48, 8, g->ppu0, PS3_SPURS_INFO_STEP_HANDLER0);
    A('R', B + 0xD28, 8, g->ppu1, PS3_SPURS_INFO_STEP_HANDLER1);    A('W', I + 0x50, 8, g->ppu1, PS3_SPURS_INFO_STEP_HANDLER1);
    A('R', B + 0x948, 8, g->tsize, PS3_SPURS_INFO_STEP_TRACE_SIZE); A('W', I + 0x18, 8, g->tsize, PS3_SPURS_INFO_STEP_TRACE_SIZE);
    A('R', B + 0x900, 8, g->traw, PS3_SPURS_INFO_STEP_TRACE_BUFFER);
    if (g->traw >> 32) { *kind = KIND_TRACE; return n; }
    A('W', I + 0x10, 4, (uint32_t)g->traw & ~3u, PS3_SPURS_INFO_STEP_TRACE_BUFFER);
    A('W', I + 0x20, 4, g->traw & 3u, PS3_SPURS_INFO_STEP_TRACE_BUFFER);
    A('R', B + 0xD9B, 1, g->pfx_size, PS3_SPURS_INFO_STEP_PREFIX);
    if (g->pfx_size > 15) { *kind = KIND_PREFIX; return n; }
    for (unsigned j = 0; j < g->pfx_size; ++j) A('R', B + 0xD8C + j, 1, g->pfx[j], PS3_SPURS_INFO_STEP_PREFIX);
    for (unsigned j = 0; j < g->pfx_size; ++j) A('W', I + 0x58 + j, 1, g->pfx[j], PS3_SPURS_INFO_STEP_PREFIX);
    A('W', I + 0x58 + g->pfx_size, 1, 0, PS3_SPURS_INFO_STEP_PREFIX);
    A('W', I + 0x68, 4, g->pfx_size, PS3_SPURS_INFO_STEP_PREFIX);
    A('W', I + 0x70, 4, 0, PS3_SPURS_INFO_STEP_COUNTERS);
    A('W', I + 0x6C, 4, 0, PS3_SPURS_INFO_STEP_COUNTERS);
#undef A
    return n;
}
static void apply(const Ex *L, unsigned upto, uint32_t I, uint8_t *buf /* 0x118, preset */) {
    for (unsigned i = 0; i < upto; ++i) {
        if (L[i].op != 'W') continue;
        const uint32_t o = L[i].addr - I;
        CHECK(o + L[i].len <= PS3_SPURS_INFO_BYTES);
        for (unsigned b = 0; b < L[i].len; ++b) buf[o + b] = (uint8_t)(L[i].v >> (8 * (L[i].len - 1 - b)));
    }
}

/* ---- logging memory double over the real atomic domain ---- */
typedef struct { char op; uint32_t addr, len; uint64_t v; int rc; } Acc;
typedef struct Mem_ {
    Fixture *f;
    Acc log[MAXLOG]; unsigned n;
    unsigned fail_at; int fail_rc;               /* 1-based over every read/write callback */
    unsigned hook_at; void (*hook)(struct Mem_ *); void *hook_arg;   /* runs after that callback */
    unsigned calls_after_fail, failed;
    unsigned pf_calls, pf_when; ps3_spurs_span pf_r[8], pf_w[8]; size_t pf_rn, pf_wn; uint32_t pf_max; int pf_rc;
} Mem;
static int m_pf(void *c, const ps3_spurs_span *r, size_t rn, const ps3_spurs_span *w, size_t wn, uint32_t mx) {
    Mem *m = c;
    ++m->pf_calls; m->pf_when = m->n; m->pf_rn = rn; m->pf_wn = wn; m->pf_max = mx;
    CHECK(rn <= 8 && wn <= 8);
    memcpy(m->pf_r, r, rn * sizeof *r); memcpy(m->pf_w, w, wn * sizeof *w);
    return m->pf_rc;
}
static int m_access(Mem *m, char op, uint32_t a, void *buf, size_t len) {
    CHECK(len == 1 || len == 4 || len == 8);
    CHECK(m->n < MAXLOG);
    if (m->failed) ++m->calls_after_fail;
    Acc *e = &m->log[m->n]; e->op = op; e->addr = a; e->len = (uint32_t)len; e->v = 0; e->rc = 0;
    const unsigned idx = ++m->n;
    if (op == 'W') for (size_t i = 0; i < len; ++i) e->v = e->v << 8 | ((const uint8_t *)buf)[i];   /* the offered bytes, performed or not */
    if (m->fail_at && idx == m->fail_at) { m->failed = 1; e->rc = m->fail_rc; return m->fail_rc; }
    int rc;
    if (op == 'R') {
        rc = ps3_atomic_read(&m->f->domain, a, buf, len) == PS3_GUEST_MEMORY_OK ? 0 : -1;
        if (!rc) for (size_t i = 0; i < len; ++i) e->v = e->v << 8 | ((uint8_t *)buf)[i];
    } else {
        rc = ps3_atomic_write(&m->f->domain, a, buf, len) == PS3_GUEST_MEMORY_OK ? 0 : -1;
    }
    e->rc = rc;
    if (rc) m->failed = 1;
    if (m->hook && (m->hook_at == 0 || idx == m->hook_at)) m->hook(m);   /* hook_at 0 = after every callback */
    return rc;
}
static int m_rd(void *c, uint32_t a, void *b, size_t n) { return m_access(c, 'R', a, b, n); }
static int m_wr(void *c, uint32_t a, const void *b, size_t n) { return m_access(c, 'W', a, (void *)b, n); }
static ps3_spurs_result call(Fixture *f, Mem *m, uint32_t b, uint32_t i, ps3_spurs_info_report *rep) {
    ps3_spurs_info_request rq = {b, i};
    ps3_spurs_info_memory mm = {m, m_pf, m_rd, m_wr};
    m->f = f;
    return ps3_spurs_runtime_get_info(&f->rt, &rq, &mm, rep);
}
static void chk_res(ps3_spurs_result r, ps3_spurs_outcome o, uint32_t rc, uint32_t reason, uint32_t step, uint32_t detail, uint32_t inst) {
    if (r.outcome != o || r.rc != rc || r.reason != reason || r.step != step || r.detail != detail || r.instance != inst) {
        fprintf(stderr, "result outcome=%d rc=%08X reason=%u step=%u detail=%08X instance=%u; want %d %08X %u %u %08X %u\n", r.outcome, r.rc,
                r.reason, r.step, r.detail, r.instance, o, rc, reason, step, detail, inst);
        abort();
    }
    CHECK(r.kernel_spu == 0 && r.kernel_step == 0);
}

/* ---- state snapshots: a call must never mutate instance, resources, events or guest instance memory ---- */
typedef struct { uint32_t state, spurs_ea, nspus, prefix_size, ppu_priority, group, spu_threads[8], queue, port, spup, helper, handler, sem[17], semc;
                 int32_t spu_priority; unsigned flags[10]; uint64_t handler_status, user_port_bits; uint32_t gstate, gnum, gexc; const void *gexec; unsigned joiner; } ISnap;
typedef struct { ISnap in[2]; uint8_t img[2][0x1000]; uint64_t lv2_sem, lv2_q, lv2_p, ppu_live; } Snap;
static void snap_take(Fixture *f, Snap *s) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < 2; ++i) {
        const ps3_spurs_instance *in = &f->rt.instances[i];
        ISnap *d = &s->in[i];
        d->state = in->state; d->spurs_ea = in->spurs_ea; d->nspus = in->nspus; d->prefix_size = in->prefix_size; d->ppu_priority = in->ppu_priority;
        d->group = in->group; memcpy(d->spu_threads, in->spu_threads, sizeof d->spu_threads); d->queue = in->queue; d->port = in->port; d->spup = in->spup;
        d->helper = in->helper.id; d->handler = in->handler.id; memcpy(d->sem, in->semaphores, sizeof d->sem); d->semc = in->semaphore_count;
        d->spu_priority = in->spu_priority; d->handler_status = __atomic_load_n(&in->handler_status, __ATOMIC_SEQ_CST); d->user_port_bits = in->user_port_bits;
        d->flags[0] = in->group_created; d->flags[1] = in->threads_initialized; d->flags[2] = in->owners_acquired; d->flags[3] = in->lwmutex_created;
        d->flags[4] = in->lwcond_created; d->flags[5] = in->queue_created; d->flags[6] = in->attached; d->flags[7] = in->port_created;
        d->flags[8] = in->port_connected; d->flags[9] = in->queue_bound;
        ps3_spu_group_record g;
        CHECK(ps3_spu_group_snapshot(&f->policy, in->group, &g) == PS3_LV2_OK);
        d->gstate = (uint32_t)g.state; d->gnum = g.num; d->gexc = g.exception_queue; d->gexec = g.executor; d->joiner = (unsigned)g.joiner;
        rd(f, i ? B2 : B1, s->img[i], 0x1000);
    }
    s->lv2_sem = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_SEMAPHORE); s->lv2_q = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_QUEUE);
    s->lv2_p = ps3_lv2_live_count(&f->lv2, PS3_LV2_KIND_PORT); s->ppu_live = ps3_ppu_threads_live(&f->ppu);
}
static void snap_same(Fixture *f, const Snap *want) {
    Snap now;
    snap_take(f, &now);
    CHECK(memcmp(&now, want, sizeof now) == 0);
}

/* The one generic verifier for performed runs. `g` = the EFFECTIVE guest values the reference order must observe. */
typedef struct { uint32_t B, I, margin, inst; uint32_t mirror; int skip_img; } Run;
static ps3_spurs_result expect_run(Fixture *f, Mem *m, const Run *run, const G *g, unsigned *out_n) {
    const uint32_t B = run->B, I = run->I;
    Ex L[MAXLOG]; int kind;
    const unsigned N = elist(g, B, I, L, &kind);
    fill(f, I - run->margin, run->margin * 2 + PS3_SPURS_INFO_BYTES);
    Snap before; snap_take(f, &before);
    ps3_spurs_info_report rep; memset(&rep, 0xEE, sizeof rep);
    ps3_spurs_result r = call(f, m, B, I, &rep);
    unsigned performed = N; ps3_spurs_outcome oc = PS3_SPURS_OUTCOME_OK; uint32_t reason = 0, step = PS3_SPURS_INFO_STEP_DONE, detail = 0;
    if (m->fail_at && m->fail_at <= N) {
        const Ex *x = &L[m->fail_at - 1];
        performed = m->fail_at - 1; oc = PS3_SPURS_OUTCOME_STOP; step = x->step; detail = (uint32_t)m->fail_rc;
        reason = x->op == 'R' ? PS3_SPURS_STOP_READ_FAILED : PS3_SPURS_STOP_WRITE_FAILED;
    } else if (kind == KIND_TRACE) {
        oc = PS3_SPURS_OUTCOME_STOP; reason = PS3_SPURS_STOP_GUEST_STATE; step = PS3_SPURS_INFO_STEP_TRACE_BUFFER; detail = (uint32_t)g->traw;
    } else if (kind == KIND_PREFIX) {
        oc = PS3_SPURS_OUTCOME_STOP; reason = PS3_SPURS_STOP_GUEST_STATE; step = PS3_SPURS_INFO_STEP_PREFIX; detail = g->pfx_size;
    }
    chk_res(r, oc, 0, reason, step, detail, run->inst);
    /* exact callback log: the performed prefix, plus the single failing callback; nothing afterwards */
    const unsigned expect_calls = (m->fail_at && m->fail_at <= N) ? m->fail_at : N;
    CHECK(m->n == expect_calls && m->calls_after_fail == 0);
    CHECK(m->pf_calls == 1 && m->pf_when == 0);
    for (unsigned i = 0; i < m->n; ++i)
        if (m->log[i].op != L[i].op || m->log[i].addr != L[i].addr || m->log[i].len != L[i].len || (m->log[i].v != L[i].v && !(m->log[i].op == 'R' && m->log[i].rc))) {   /* a failed read observed nothing */
            fprintf(stderr, "access %u: %c %08X/%u=%llX; want %c %08X/%u=%llX\n", i + 1, m->log[i].op, m->log[i].addr, m->log[i].len,
                    (unsigned long long)m->log[i].v, L[i].op, L[i].addr, L[i].len, (unsigned long long)L[i].v);
            abort();
        }
    unsigned rds = 0, wrs = 0; uint32_t last = UINT32_MAX;
    for (unsigned i = 0; i < performed; ++i) { if (L[i].op == 'R') ++rds; else { ++wrs; last = L[i].addr - I; } }
    CHECK(rep.reads == rds && rep.writes == wrs && rep.last_write_offset == last);
    CHECK(rep.partial == (wrs > 0 && oc != PS3_SPURS_OUTCOME_OK));
    /* observed values in the report */
    if (oc == PS3_SPURS_OUTCOME_OK || kind != KIND_OK) {
        CHECK(rep.flags == g->flags && rep.nspus == g->nspus && rep.trace_size == g->tsize && rep.trace_buffer_raw == g->traw);
        if (oc == PS3_SPURS_OUTCOME_OK || kind == KIND_PREFIX) CHECK(rep.prefix_size == g->pfx_size);
        else CHECK(rep.prefix_size == 0);
        CHECK(rep.mirror_mismatch == run->mirror);
    } else CHECK(rep.mirror_mismatch == 0);
    /* exact output bytes: sentinel everywhere except the performed writes; guards untouched */
    uint8_t want[PS3_SPURS_INFO_BYTES];
    memset(want, PAT, sizeof want);
    apply(L, performed, I, want);
    uint8_t got[PS3_SPURS_INFO_BYTES + 2 * MARGIN];
    rd(f, I - run->margin, got, run->margin * 2 + PS3_SPURS_INFO_BYTES);
    for (unsigned i = 0; i < run->margin; ++i) CHECK(got[i] == PAT && got[run->margin + PS3_SPURS_INFO_BYTES + i] == PAT);
    for (unsigned i = 0; i < PS3_SPURS_INFO_BYTES; ++i)
        if (got[run->margin + i] != want[i]) { fprintf(stderr, "info+0x%03X = %02X want %02X\n", i, got[run->margin + i], want[i]); abort(); }
    if (run->skip_img) rd(f, B, before.img[run->inst], 0x1000);   /* the lane's hook changed the guest SPURS image on purpose */
    snap_same(f, &before);   /* instance/lv2/ppu/guest SPURS image unchanged (guest mutations are applied by the lane BEFORE this call) */
    if (out_n) *out_n = performed;
    return r;
}
static Run run_for(uint32_t B, uint32_t inst, uint32_t mirror) { Run r = {B, INFO, MARGIN, inst, mirror, 0}; return r; }
static Mem *mem_new(void) { Mem *m = calloc(1, sizeof *m); CHECK(m); return m; }
static Fixture *fx(void) { Fixture *f = calloc(1, sizeof *f); CHECK(f); setup(f); two_instances(f); return f; }
static void fx_done(Fixture *f) { destroy(f); free(f); }

/* ---- literal hand-built 280-byte images (never produced by the implementation) ---- */
static void hand_image(unsigned which, uint8_t *o) {
    memset(o, PAT, PS3_SPURS_INFO_BYTES);
    if (which == 0) {   /* instance 0: nSpus 5, prio 128/249, group 0x04000100, prefix "abc" */
        p32(o + 0x00, 5); p32(o + 0x04, 128); p32(o + 0x08, 249); o[0x0C] = 0; o[0x0D] = 0;
        p32(o + 0x10, 0); p64(o + 0x18, 0); p32(o + 0x20, 0); p32(o + 0x24, 0x04000100u);
        p32(o + 0x28, 0x00000100u); p32(o + 0x2C, 0x01000100u); p32(o + 0x30, 0x02000100u); p32(o + 0x34, 0x03000100u); p32(o + 0x38, 0x04000100u);
        p32(o + 0x3C, 0); p32(o + 0x40, 0); p32(o + 0x44, 0);
        p64(o + 0x48, 0x01000001u); p64(o + 0x50, 0x01000000u);
        o[0x58] = 'a'; o[0x59] = 'b'; o[0x5A] = 'c'; o[0x5B] = 0; p32(o + 0x68, 3); p32(o + 0x6C, 0); p32(o + 0x70, 0);
    } else {            /* instance 1: nSpus 1, prio 255/249, group 0x04000200, prefix "abcdefghijklmno" */
        p32(o + 0x00, 1); p32(o + 0x04, 255); p32(o + 0x08, 249); o[0x0C] = 0; o[0x0D] = 0;
        p32(o + 0x10, 0); p64(o + 0x18, 0); p32(o + 0x20, 0); p32(o + 0x24, 0x04000200u);
        p32(o + 0x28, 0x00000200u);
        for (unsigned k = 1; k < 8; ++k) p32(o + 0x28 + 4 * k, 0);
        p64(o + 0x48, 0x01000003u); p64(o + 0x50, 0x01000002u);
        for (unsigned j = 0; j < 15; ++j) o[0x58 + j] = (uint8_t)('a' + j);
        o[0x67] = 0; p32(o + 0x68, 15); p32(o + 0x6C, 0); p32(o + 0x70, 0);
    }
}

/* ===== lanes ===== */
static void lane_d1(void) {   /* instance 0 OK, disjoint I, exact bytes AND exact access log AND report */
    Fixture *f = fx(); Mem *m = mem_new();
    G g; read_g(f, B1, &g);
    CHECK(g.nspus == 5 && g.spu_prio == 128 && g.ppu_prio == 249 && g.flags == 0 && g.group == 0x04000100u && g.pfx_size == 3);
    CHECK(g.spus[0] == 0x100 && g.spus[4] == 0x04000100u && g.spus[5] == 0 && g.spus[7] == 0);
    CHECK(g.ppu0 == 0x01000001u && g.ppu1 == 0x01000000u && g.tsize == 0 && g.traw == 0);
    Run run = run_for(B1, 0, 0);
    unsigned n;
    ps3_spurs_result r = expect_run(f, m, &run, &g, &n);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(n == 41 + 2 * 3 && m->n == 47);
    uint8_t hand[PS3_SPURS_INFO_BYTES], got[PS3_SPURS_INFO_BYTES];
    hand_image(0, hand); rd(f, INFO, got, sizeof got);
    CHECK(memcmp(hand, got, sizeof got) == 0);
    /* sentinel holes and name tail really are untouched, ids k>=nSpus really replaced */
    CHECK(got[0x0E] == PAT && got[0x0F] == PAT && got[0x14] == PAT && got[0x17] == PAT && got[0x5C] == PAT && got[0x67] == PAT && got[0x74] == PAT && got[0x117] == PAT);
    CHECK(rdv(f, INFO + 0x3C, 4) == 0 && rdv(f, INFO + 0x44, 4) == 0);
    /* report: counts, last write = miss counter, mirror clean */
    ps3_spurs_info_report rep; Mem *m2 = mem_new(); fill(f, INFO, PS3_SPURS_INFO_BYTES);
    r = call(f, m2, B1, INFO, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(rep.reads == 21 && rep.writes == 26 && rep.last_write_offset == 0x6C && rep.partial == 0 && rep.flags == 0 && rep.nspus == 5 &&
          rep.prefix_size == 3 && rep.trace_buffer_raw == 0 && rep.trace_size == 0 && rep.mirror_mismatch == 0);
    /* report is optional */
    Mem *m3 = mem_new(); fill(f, INFO, PS3_SPURS_INFO_BYTES);
    r = call(f, m3, B1, INFO, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(m3->n == 47);
    free(m); free(m2); free(m3); fx_done(f);
}
static void lane_d2(void) {   /* instance 1: its own distinct group/ids/handlers */
    Fixture *f = fx(); Mem *m = mem_new();
    G g; read_g(f, B2, &g);
    CHECK(g.nspus == 1 && g.spu_prio == 255 && g.group == 0x04000200u && g.pfx_size == 15 && g.ppu0 == 0x01000003u && g.ppu1 == 0x01000002u);
    Run run = run_for(B2, 1, 0);
    unsigned n;
    ps3_spurs_result r = expect_run(f, m, &run, &g, &n);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 1);
    CHECK(n == 41 + 2 * 15 && n == 71);
    uint8_t hand[PS3_SPURS_INFO_BYTES], got[PS3_SPURS_INFO_BYTES];
    hand_image(1, hand); rd(f, INFO, got, sizeof got);
    CHECK(memcmp(hand, got, sizeof got) == 0 && got[0x67] == 0);
    uint8_t hand0[PS3_SPURS_INFO_BYTES];
    hand_image(0, hand0);
    CHECK(memcmp(hand, hand0, sizeof hand) != 0);
    CHECK(f->rt.instances[0].group != f->rt.instances[1].group && f->rt.instances[0].handler.id != f->rt.instances[1].handler.id);
    free(m); fx_done(f);
}
static void mut_a(Mem *m) {   /* after the 1st callback (flags read): flip flags and an unread field; after the 3rd: change nSpus */
    Fixture *f = m->f; const uint32_t B = (uint32_t)(uintptr_t)m->hook_arg;
    if (m->n == 1) { wrv(f, B + 0xD80, 0x5, 4); wrv(f, B + 0xD88, 777, 4); }
    else if (m->n == 3) wrv(f, B + 0x76, 2, 1);
}
static void lane_d3a(void) {  /* guest is authoritative: every field mutated before the call */
    Fixture *f = fx(); Mem *m = mem_new();
    wrv(f, B1 + 0x76, 7, 1); wrv(f, B1 + 0xD84, 0x11111111u, 4); wrv(f, B1 + 0xD88, 0x22222222u, 4); wrv(f, B1 + 0xD80, 0xF5, 4);
    wrv(f, B1 + 0xD30, 0x04000199u, 4); wrv(f, B1 + 0xD34 + 28, 0xDEAD0007u, 4); wrv(f, B1 + 0xD20, 0x1122334455667788ull, 8);
    wrv(f, B1 + 0xD28, 0x99AABBCCDDEEFF00ull, 8); wrv(f, B1 + 0x948, 0x1122334455667788ull, 8); wrv(f, B1 + 0x900, 0xDEADBEEFull, 8);
    wr(f, B1 + 0xD8C, "ABCDE", 5); wrv(f, B1 + 0xD9B, 5, 1);
    G g; read_g(f, B1, &g);
    CHECK(g.nspus == 7 && g.traw == 0xDEADBEEFull && g.pfx_size == 5);
    Run run = run_for(B1, 0, 0xFF);   /* all eight mirror bits */
    ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(rdv(f, INFO + 0x0C, 1) == 1 && rdv(f, INFO + 0x0D, 1) == 1);          /* exit = 1, spurs2 = 1 (0xF5 has bits 1 and 4) */
    CHECK(rdv(f, INFO + 0x10, 4) == 0xDEADBEECu && rdv(f, INFO + 0x20, 4) == 3);
    CHECK(rdv(f, INFO + 0x00, 4) == 7 && rdv(f, INFO + 0x58, 5) == 0x4142434445ull && rdv(f, INFO + 0x68, 4) == 5);
    CHECK(rdv(f, INFO + 0x18, 8) == 0x1122334455667788ull && rdv(f, INFO + 0x48, 8) == 0x1122334455667788ull);
    free(m); fx_done(f);
}
static void lane_d3_bits(void) {   /* each mirror bit independently, output follows the guest, restored in between */
    Fixture *f = fx();
    struct { uint32_t off; unsigned n; uint64_t v; } mut[8] = {
        {0x76, 1, 4}, {0xD84, 4, 129}, {0xD88, 4, 250}, {0xD30, 4, 0x04000101u}, {0xD34 + 8, 4, 0x55},
        {0xD20, 8, 0x100000001ull}, {0xD28, 8, 0x100000000ull}, {0xD9B, 1, 4}};
    for (unsigned bit = 0; bit < 8; ++bit) {
        const uint64_t keep = rdv(f, B1 + mut[bit].off, mut[bit].n);
        wrv(f, B1 + mut[bit].off, mut[bit].v, mut[bit].n);
        Mem *m = mem_new();
        G g; read_g(f, B1, &g);
        Run run = run_for(B1, 0, 1u << bit);
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
        free(m);
        wrv(f, B1 + mut[bit].off, keep, mut[bit].n);
    }
    fx_done(f);
}
static void lane_d3b(void) {  /* mutation mid-call: flags captured once; unread fields follow the guest; fields already copied are never re-read */
    Fixture *f = fx(); Mem *m = mem_new();
    m->hook = mut_a; m->hook_at = 0; m->hook_arg = (void *)(uintptr_t)B1;
    G g; read_g(f, B1, &g);            /* effective: flags 0 (captured at access 1), nSpus 5 (copied before the 3rd-callback hook), ppu priority 777 */
    g.ppu_prio = 777;
    Run run = run_for(B1, 0, 4); run.skip_img = 1;       /* only the ppu priority differs from the native record (249); the later nSpus=2 change is never observed */
    ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(rdv(f, INFO + 0x0C, 1) == 0 && rdv(f, INFO + 0x0D, 1) == 0 && rdv(f, INFO + 0x00, 4) == 5 && rdv(f, INFO + 0x08, 4) == 777);
    CHECK(rdv(f, B1 + 0xD80, 4) == 5 && rdv(f, B1 + 0x76, 1) == 2);   /* the guest changes really happened */
    free(m); fx_done(f);
}
static void lane_d4(void) {   /* NULL before ALIGN before ownership, zero callbacks */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    const uint32_t pairs[5][2] = {{0, INFO}, {B1, 0}, {0, 0}, {B1 + 8, 0}, {UNOWNED, 0}};
    for (unsigned i = 0; i < 5; ++i) {
        Mem *m = mem_new(); ps3_spurs_info_report rep; memset(&rep, 0xEE, sizeof rep);
        ps3_spurs_result r = call(f, m, pairs[i][0], pairs[i][1], &rep);
        chk_res(r, PS3_SPURS_OUTCOME_FAILED, 0x80410711u, 0, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
        CHECK(m->n == 0 && m->pf_calls == 0 && rep.reads == 0 && rep.writes == 0 && rep.partial == 0 && rep.last_write_offset == UINT32_MAX);
        free(m);
    }
    {   /* D4b: misaligned with a non-NULL info */
        const uint32_t al[3] = {B1 + 8, UNOWNED + 8, 1};
        for (unsigned i = 0; i < 3; ++i) {
            Mem *m = mem_new();
            ps3_spurs_result r = call(f, m, al[i], INFO, NULL);
            chk_res(r, PS3_SPURS_OUTCOME_FAILED, 0x80410710u, 0, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
            CHECK(m->n == 0 && m->pf_calls == 0);
            free(m);
        }
    }
    snap_same(f, &s);
    fx_done(f);
}
static void lane_d5(void) {   /* no info alignment check: odd / +2 / +4 */
    Fixture *f = fx();
    const uint32_t offs[3] = {1, 2, 4};
    for (unsigned i = 0; i < 3; ++i) {
        Mem *m = mem_new(); G g; read_g(f, B1, &g);
        Run run = run_for(B1, 0, 0); run.I = INFO + offs[i];
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
        uint8_t hand[PS3_SPURS_INFO_BYTES], got[PS3_SPURS_INFO_BYTES];
        hand_image(0, hand); rd(f, run.I, got, sizeof got);
        CHECK(memcmp(hand, got, sizeof got) == 0);
        free(m);
    }
    fx_done(f);
}
static void lane_d6(void) {   /* unowned 128-aligned B, retained/building: distinct STOPs, zero callbacks */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    Mem *m = mem_new(); ps3_spurs_info_report rep; memset(&rep, 0xEE, sizeof rep);
    ps3_spurs_result r = call(f, m, UNOWNED, INFO, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_INFO_STEP_OWNER, UNOWNED, 0);
    CHECK(m->n == 0 && m->pf_calls == 0 && rep.reads == 0 && rep.writes == 0 && rep.partial == 0);
    r = call(f, m, B1 + 0x1000, INFO, NULL);   /* inside no instance: also unknown */
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_INFO_STEP_OWNER, B1 + 0x1000, 0);
    free(m);
    const uint32_t states[2] = {PS3_SPURS_INSTANCE_RETAINED, PS3_SPURS_INSTANCE_BUILDING};
    for (unsigned i = 0; i < 2; ++i) {
        f->rt.instances[1].state = states[i];
        Mem *m2 = mem_new();
        fill(f, INFO, PS3_SPURS_INFO_BYTES);
        r = call(f, m2, B2, INFO, &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INSTANCE_NOT_ACTIVE, PS3_SPURS_INFO_STEP_OWNER, states[i], 1);
        CHECK(m2->n == 0 && m2->pf_calls == 0 && rep.writes == 0 && rep.partial == 0);
        uint8_t got[PS3_SPURS_INFO_BYTES];
        rd(f, INFO, got, sizeof got);
        for (unsigned k = 0; k < sizeof got; ++k) CHECK(got[k] == PAT);
        free(m2);
        f->rt.instances[1].state = PS3_SPURS_INSTANCE_ACTIVE;
    }
    snap_same(f, &s);
    fx_done(f);
}
static void lane_d7(void) {   /* prefix 0 / 15 / 16 / 255 */
    Fixture *f = fx();
    struct { unsigned size; } cases[4] = {{0}, {15}, {16}, {255}};
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned j = 0; j < 15; ++j) wrv(f, B1 + 0xD8C + j, 'A' + j, 1);
        wrv(f, B1 + 0xD9B, cases[c].size, 1);
        Mem *m = mem_new(); G g; read_g(f, B1, &g);
        Run run = run_for(B1, 0, 0x80);   /* prefix size differs from the native 3 in every case */
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        uint8_t got[PS3_SPURS_INFO_BYTES];
        rd(f, INFO, got, sizeof got);
        if (cases[c].size == 0) {
            chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
            CHECK(got[0x58] == 0 && got[0x59] == PAT && got[0x67] == PAT && rdv(f, INFO + 0x68, 4) == 0);
        } else if (cases[c].size == 15) {
            chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
            CHECK(got[0x58] == 'A' && got[0x66] == 'O' && got[0x67] == 0 && rdv(f, INFO + 0x68, 4) == 15);
        } else {
            chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_GUEST_STATE, PS3_SPURS_INFO_STEP_PREFIX, cases[c].size, 0);
            for (unsigned k = 0x58; k <= 0x73; ++k) CHECK(got[k] == PAT);       /* name, length, counters untouched */
            CHECK(rdv(f, INFO + 0x20, 4) == 0 && rdv(f, INFO + 0x24, 4) == 0x04000100u);   /* earlier writes stay */
        }
        free(m);
    }
    fx_done(f);
}
static void lane_d8(void) {   /* trace: BE64 size, pointer/mode transform, high32 policy */
    Fixture *f = fx();
    const struct { uint64_t raw; int stop; } t[6] = {{0xDEADBEEFull, 0}, {0x123456ull, 0}, {0xFFFFFFFFull, 0}, {0x100000000ull, 1}, {0xFFFFFFFFFFFFFFFFull, 1}, {0x80000000ull, 0}};
    for (unsigned c = 0; c < 6; ++c) {
        wrv(f, B1 + 0x948, 0x1122334455667788ull, 8); wrv(f, B1 + 0x900, t[c].raw, 8); wrv(f, B1 + 0xD9B, 5, 1);
        Mem *m = mem_new(); G g; read_g(f, B1, &g);
        Run run = run_for(B1, 0, t[c].stop ? 0 : 0x80);   /* prefix size 5 vs native 3 is only observed when the call gets that far */
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        CHECK(rdv(f, INFO + 0x18, 8) == 0x1122334455667788ull);
        if (t[c].stop) {
            chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_GUEST_STATE, PS3_SPURS_INFO_STEP_TRACE_BUFFER, (uint32_t)t[c].raw, 0);
            CHECK(rdv(f, INFO + 0x10, 4) == 0xA5A5A5A5u && rdv(f, INFO + 0x20, 4) == 0xA5A5A5A5u);
        } else {
            chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
            CHECK(rdv(f, INFO + 0x10, 4) == ((uint32_t)t[c].raw & ~3u) && rdv(f, INFO + 0x20, 4) == (t[c].raw & 3));
        }
        free(m);
    }
    fx_done(f);
}
static void lane_d9_pre(void) {   /* preflight refusal: zero accesses, output untouched, instance ACTIVE */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    Mem *m = mem_new(); m->pf_rc = 9;
    fill(f, INFO, PS3_SPURS_INFO_BYTES);
    ps3_spurs_info_report rep; memset(&rep, 0xEE, sizeof rep);
    ps3_spurs_result r = call(f, m, B1, INFO, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_PREFLIGHT_REFUSED, PS3_SPURS_INFO_STEP_PREFLIGHT, 9, 0);
    CHECK(m->pf_calls == 1 && m->n == 0 && rep.reads == 0 && rep.writes == 0 && rep.partial == 0 && rep.last_write_offset == UINT32_MAX);
    uint8_t got[PS3_SPURS_INFO_BYTES];
    rd(f, INFO, got, sizeof got);
    for (unsigned k = 0; k < sizeof got; ++k) CHECK(got[k] == PAT);
    snap_same(f, &s);
    free(m); fx_done(f);
}
static void lane_d9_each(unsigned inst) {   /* a failure at EVERY access k of the normative sequence (read or write alike) */
    Fixture *f = fx();
    const uint32_t B = inst ? B2 : B1;
    G g; read_g(f, B, &g);
    Ex L[MAXLOG]; int kind;
    const unsigned N = elist(&g, B, INFO, L, &kind);
    CHECK(kind == KIND_OK && N == 41 + 2u * g.pfx_size);
    for (unsigned k = 1; k <= N; ++k) {
        Mem *m = mem_new(); m->fail_at = k; m->fail_rc = 13;
        Run run = run_for(B, inst, 0);
        unsigned performed;
        ps3_spurs_result r = expect_run(f, m, &run, &g, &performed);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, L[k - 1].op == 'R' ? PS3_SPURS_STOP_READ_FAILED : PS3_SPURS_STOP_WRITE_FAILED, L[k - 1].step, 13, inst);
        CHECK(performed == k - 1 && m->n == k && f->rt.instances[inst].state == PS3_SPURS_INSTANCE_ACTIVE);
        free(m);
    }
    /* bad memory (unmapped output): the first write fails after exactly two reads; nothing written */
    {
        Mem *m = mem_new(); ps3_spurs_info_report rep;
        ps3_spurs_result r = call(f, m, B, 0x00900000u, &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_INFO_STEP_NSPUS, (uint32_t)-1, inst);
        CHECK(rep.reads == 2 && rep.writes == 0 && rep.partial == 0 && rep.last_write_offset == UINT32_MAX && m->n == 3 && m->calls_after_fail == 0);
        free(m);
        /* highest legal info address passes the range check; the unmapped output then fails the same way */
        m = mem_new();
        r = call(f, m, B, 0xFFFFFEE8u, &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_INFO_STEP_NSPUS, (uint32_t)-1, inst);
        free(m);
    }
    fx_done(f);
}
static void lane_d9_reads(void) { lane_d9_each(0); }
static void lane_d9_writes(void) { lane_d9_each(1); }
static void lane_d10(void) {   /* whole-instance overlap STOP before effects; adjacent placements allowed */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    const uint32_t bad[6] = {B1 + 0x800, B1 - 0x100, B1 + 0xFE8, B1 - 0x117, B1 + 0xFFF, B1};
    for (unsigned i = 0; i < 6; ++i) {
        Mem *m = mem_new(); ps3_spurs_info_report rep; memset(&rep, 0xEE, sizeof rep);
        ps3_spurs_result r = call(f, m, B1, bad[i], &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_OUTPUT_OVERLAP, PS3_SPURS_INFO_STEP_OVERLAP, bad[i], 0);
        CHECK(m->n == 0 && m->pf_calls == 0 && rep.reads == 0 && rep.writes == 0 && rep.partial == 0);
        free(m);
    }
    snap_same(f, &s);
    const uint32_t good[2] = {B1 - 0x118, B1 + 0x1000};
    for (unsigned i = 0; i < 2; ++i) {
        Mem *m = mem_new(); G g; read_g(f, B1, &g);
        Run run = run_for(B1, 0, 0); run.I = good[i]; run.margin = 0;
        fill(f, good[i], PS3_SPURS_INFO_BYTES);
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
        free(m);
    }
    fx_done(f);
}
static void lane_d11(void) {   /* host contract: STOP INVALID_REQUEST before everything else, zero callbacks */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    ps3_spurs_runtime *zero = calloc(1, sizeof *zero);
    CHECK(zero);
    Mem *m = mem_new(); m->f = f;
    ps3_spurs_info_request rq = {B1, INFO}, rq_null = {0, 0};
    ps3_spurs_info_memory ok = {m, m_pf, m_rd, m_wr};
    ps3_spurs_info_memory bad[4] = {{m, NULL, m_rd, m_wr}, {m, m_pf, NULL, m_wr}, {m, m_pf, m_rd, NULL}, {NULL, NULL, NULL, NULL}};
    ps3_spurs_info_report rep;
    for (unsigned i = 0; i < 4; ++i) {
        memset(&rep, 0xEE, sizeof rep);
        ps3_spurs_result r = ps3_spurs_runtime_get_info(&f->rt, &rq, &bad[i], &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
        CHECK(rep.reads == 0 && rep.writes == 0 && rep.last_write_offset == UINT32_MAX && rep.partial == 0);
        r = ps3_spurs_runtime_get_info(&f->rt, &rq_null, &bad[i], NULL);   /* host contract outranks NULL */
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
    }
    ps3_spurs_result r = ps3_spurs_runtime_get_info(&f->rt, NULL, &ok, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
    r = ps3_spurs_runtime_get_info(&f->rt, &rq, NULL, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
    r = ps3_spurs_runtime_get_info(NULL, &rq, &ok, &rep);
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
    r = ps3_spurs_runtime_get_info(zero, &rq, &ok, &rep);   /* uninitialized runtime */
    chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0, 0);
    CHECK(m->n == 0 && m->pf_calls == 0);
    snap_same(f, &s);
    free(zero); free(m); fx_done(f);
}
static void lane_d12(void) {   /* info range wrap */
    Fixture *f = fx(); Snap s; snap_take(f, &s);
    const uint32_t bad[3] = {0xFFFFFF00u, 0xFFFFFEE9u, 0xFFFFFFFFu};
    for (unsigned i = 0; i < 3; ++i) {
        Mem *m = mem_new(); ps3_spurs_info_report rep;
        ps3_spurs_result r = call(f, m, B1, bad[i], &rep);
        chk_res(r, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_PREFLIGHT_REFUSED, PS3_SPURS_INFO_STEP_RANGE, 0, 0);
        CHECK(m->n == 0 && m->pf_calls == 0 && rep.writes == 0 && rep.partial == 0);
        free(m);
    }
    snap_same(f, &s);
    fx_done(f);
}
static void lane_d13(void) {   /* the exact preflight spans, once, before any access */
    Fixture *f = fx();
    for (unsigned inst = 0; inst < 2; ++inst) {
        const uint32_t B = inst ? B2 : B1;
        Mem *m = mem_new(); G g; read_g(f, B, &g);
        Run run = run_for(B, inst, 0);
        ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
        chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, inst);
        CHECK(m->pf_calls == 1 && m->pf_when == 0 && m->pf_rn == 4 && m->pf_wn == 3 && m->pf_max == 71);
        CHECK(m->pf_r[0].address == B + 0x076 && m->pf_r[0].length == 1);
        CHECK(m->pf_r[1].address == B + 0x900 && m->pf_r[1].length == 8);
        CHECK(m->pf_r[2].address == B + 0x948 && m->pf_r[2].length == 8);
        CHECK(m->pf_r[3].address == B + 0xD20 && m->pf_r[3].length == 0x7C);
        CHECK(m->pf_w[0].address == INFO && m->pf_w[0].length == 0x0E);
        CHECK(m->pf_w[1].address == INFO + 0x10 && m->pf_w[1].length == 4);
        CHECK(m->pf_w[2].address == INFO + 0x18 && m->pf_w[2].length == 0x5C);
        /* every performed access lies inside a declared span */
        for (unsigned i = 0; i < m->n; ++i) {
            int in = 0;
            if (m->log[i].op == 'R') { for (unsigned s = 0; s < 4; ++s) in |= m->log[i].addr >= m->pf_r[s].address && m->log[i].addr + m->log[i].len <= m->pf_r[s].address + m->pf_r[s].length; }
            else { for (unsigned s = 0; s < 3; ++s) in |= m->log[i].addr >= m->pf_w[s].address && m->log[i].addr + m->log[i].len <= m->pf_w[s].address + m->pf_w[s].length; }
            CHECK(in);
        }
        free(m);
    }
    fx_done(f);
}
static void lane_nspus255(void) {   /* guest nSpus never indexes native arrays: 255 -> BE32 255, all 8 guest ids copied, mirror bound by native nspus */
    Fixture *f = fx();
    wrv(f, B1 + 0x76, 255, 1);
    Mem *m = mem_new(); G g; read_g(f, B1, &g);
    CHECK(g.nspus == 255);
    Run run = run_for(B1, 0, 1);     /* bit0 only: ids still equal the native ids for k<5 and 0 beyond */
    ps3_spurs_result r = expect_run(f, m, &run, &g, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(rdv(f, INFO + 0x00, 4) == 255 && rdv(f, INFO + 0x28, 4) == 0x100 && rdv(f, INFO + 0x38, 4) == 0x04000100u && rdv(f, INFO + 0x44, 4) == 0);
    free(m);
    /* guest ids beyond native nspus (k = 5..7) nonzero: copied, reported as a mirror disagreement (bit0 + bit4) */
    wrv(f, B1 + 0xD34 + 20, 0xCAFE0005u, 4); wrv(f, B1 + 0xD34 + 28, 0xCAFE0007u, 4);
    m = mem_new(); read_g(f, B1, &g);
    run = run_for(B1, 0, 0x11);
    r = expect_run(f, m, &run, &g, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 0);
    CHECK(rdv(f, INFO + 0x3C, 4) == 0xCAFE0005u && rdv(f, INFO + 0x44, 4) == 0xCAFE0007u);
    free(m);
    /* a guest id mismatch INSIDE the native range (k = 2) with the guest nSpus 255 and a smaller native instance 1 */
    wrv(f, B2 + 0x76, 255, 1); wrv(f, B2 + 0xD34 + 4, 0xBEEF0001u, 4);
    m = mem_new(); read_g(f, B2, &g);
    run = run_for(B2, 1, 0x11);   /* native nspus 1: guest id k=1 (nonzero) vs native 0 -> bit4; nSpus differs -> bit0 */
    r = expect_run(f, m, &run, &g, NULL);
    chk_res(r, PS3_SPURS_OUTCOME_OK, 0, 0, PS3_SPURS_INFO_STEP_DONE, 0, 1);
    free(m);
    fx_done(f);
}

typedef struct { const char *name; void (*fn)(void); } Lane;
static const Lane LANES[] = {
    {"d1", lane_d1}, {"d2", lane_d2}, {"d3a", lane_d3a}, {"d3_bits", lane_d3_bits}, {"d3b", lane_d3b}, {"d4", lane_d4}, {"d5", lane_d5},
    {"d6", lane_d6}, {"d7", lane_d7}, {"d8", lane_d8}, {"d9_pre", lane_d9_pre}, {"d9_inst0", lane_d9_reads}, {"d9_inst1", lane_d9_writes},
    {"d10", lane_d10}, {"d11", lane_d11}, {"d12", lane_d12}, {"d13", lane_d13}, {"nspus255", lane_nspus255}
};
static int selected(const char *only, const char *name) {
    if (!only || !*only) return 1;
    const size_t n = strlen(name);
    for (const char *p = only; *p;) {
        const char *e = strchr(p, ',');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}
int main(void) {
    const char *only = getenv("PS3_SPURS_GETINFO_ONLY");
    const int isolate = getenv("PS3_SPURS_GETINFO_ISOLATE") != NULL;   /* red/diagnostic mode: every lane in its own child, all lanes reported */
    unsigned ran = 0, failed = 0;
    for (unsigned i = 0; i < sizeof LANES / sizeof *LANES; ++i) {
        if (!selected(only, LANES[i].name)) continue;
        if (isolate) {
            fflush(NULL);
            const pid_t pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) { LANES[i].fn(); _exit(0); }
            int st = 0;
            CHECK(waitpid(pid, &st, 0) == pid);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) printf("lane %s: OK\n", LANES[i].name);
            else { printf("lane %s: FAIL (%s %d)\n", LANES[i].name, WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st)); ++failed; }
        } else {
            LANES[i].fn();
            printf("lane %s: OK\n", LANES[i].name);
        }
        ++ran;
    }
    CHECK(ran > 0);
    if (failed) { printf("test_spurs_get_info: %u of %u lanes FAILED\n", failed, ran); return 1; }
    puts("test_spurs_get_info: OK");
    return 0;
}
