#define _POSIX_C_SOURCE 200809L
/* Reserve/start/abandon lifecycle for the pure-C PPU service. Reuses the real Fixture of
 * test_lv2_services.c (real host space, user memory + charges, guards, LV2, wait port and pthread
 * service-thread adapter) and runs the ORIGINAL main unchanged first (compatibility), then the
 * new cases. Usage: test_ppu_reservation            -> original checks + every new case
 *                    test_ppu_reservation <case>     -> one new case (red/green demonstration) */
#define main lv2_services_original_main
#include "test_lv2_services.c"
#undef main
#include <errno.h>

enum { CODE_SENTINEL = 99 };
static void wait_flag(uint32_t *flag) {   /* real synchronisation on an atomic; the bound only converts a hang into a failure */
    for (unsigned long i = 0; i < 400000000UL; ++i) { if (__atomic_load_n(flag, __ATOMIC_SEQ_CST)) return; sched_yield(); }
    CHECK(!"flag wait timed out");
}
static void set_flag(uint32_t *flag) { __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST); }
static ps3_ppu_ref reserve_ok(Fixture *f, int thread_index, ps3_ppu_service_body body, void *argument, uint64_t stack) {
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_reserve(&f->ppu, 1, 249, 250, stack, tp(f, thread_index), body, argument, &r) == PS3_PPU_OK);
    return r;
}
static uint64_t life_of(Fixture *f, ps3_ppu_ref r) { return __atomic_load_n(&f->ppu.services[r.slot].life, __ATOMIC_SEQ_CST); }

/* ---- reserve never runs the body; exact quota; id/params published before the real thread starts ---- */
typedef struct { uint32_t published_id, param, runs, observed_id, observed_match; } Probe;
static void probe_body(ps3_ppu_service *self, void *argument) {
    Probe *p = argument;
    p->observed_id = self->id;
    p->observed_match = p->published_id == self->id && p->param == 0xC0FFEEu;
    __atomic_add_fetch(&p->runs, 1, __ATOMIC_SEQ_CST);
    ps3_ppu_service_exit(self, 11);
}
static void reserve_publish_then_start(void) {
    Fixture f; setup(&f);
    Probe p; memset(&p, 0, sizeof p);
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), probe_body, &p, &r) == PS3_PPU_OK);
    CHECK(r.id == 0x01000000u && r.slot < PS3_PPU_SERVICE_CAP);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_RESERVED && PS3_PPU_SERVICE_RESERVED == 3 &&
          PS3_PPU_SERVICE_FREE == 0 && PS3_PPU_SERVICE_RUNNING == 1 && PS3_PPU_SERVICE_EXITED == 2);
    CHECK(used(&f) == 0x4000 && ps3_ppu_threads_live(&f.ppu) == 1);   /* exactly one charge */
    CHECK(!f.threads[0].started && p.runs == 0);                      /* nothing ran */
    CHECK(!ps3_ppu_service_is_blocked(&f.ppu, r));
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_EINVAL && code == CODE_SENTINEL);   /* reserved-only join */
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000 && !f.threads[0].started);
    /* the caller completes the child's parameters and its id output BEFORE start */
    p.published_id = r.id; p.param = 0xC0FFEEu;
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_OK);
    CHECK(p.runs == 1 && p.observed_match && p.observed_id == r.id && f.threads[0].started);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_EXITED && used(&f) == 0x4000);   /* still one charge */
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_EINVAL && p.runs == 1);   /* no second start */
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_EINVAL && ps3_ppu_threads_live(&f.ppu) == 1);   /* EXITED is not released here */
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 11 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}

/* ---- a real LV2 receive wait satisfies start; no synthetic BLOCKED ---- */
static void start_real_lv2_wait(void) {
    Fixture f; setup(&f);
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q, p;
    CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 42) == PS3_LV2_OK);
    CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && ps3_lv2_port_connect_local(&f.lv2, p, q) == PS3_LV2_OK);
    HelperArgs args = {&f.lv2, q, {0}, {0, 0, 1, 0}, 0xFFFFFFFFu};
    CHECK(ps3_lv2_queue_bind_service(&f.lv2, q, args.owned) == PS3_LV2_OK);
    ps3_ppu_ref h = reserve_ok(&f, 0, helper_body, &args, 0x8000);
    ps3_lv2_queue snap;
    CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &snap) == PS3_LV2_OK && snap.receivers == 0);   /* reserved: no receiver exists */
    CHECK(!ps3_ppu_service_is_blocked(&f.ppu, h) && used(&f) == 0x8000);
    CHECK(ps3_ppu_service_start(&f.ppu, h) == PS3_PPU_OK);
    CHECK(ps3_ppu_service_is_blocked(&f.ppu, h));                    /* really admitted to the LV2 wait */
    CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &snap) == PS3_LV2_OK && snap.receivers == 1 && snap.blocked_receivers == 1);
    CHECK(life_of(&f, h) == PS3_PPU_SERVICE_RUNNING && used(&f) == 0x8000);
    CHECK(ps3_ppu_service_abandon(&f.ppu, h) == PS3_PPU_EINVAL);     /* RUNNING is never released by abandon */
    CHECK(ps3_lv2_port_send(&f.lv2, p, 0, 1, 0) == PS3_LV2_OK);
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, h, &code) == PS3_PPU_OK && code == 0 && args.last_rc == PS3_LV2_OK);
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    CHECK(ps3_lv2_port_disconnect(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK);
    teardown(&f);
}

/* ---- abandon / reuse / stale refs ---- */
static void abandon_reuse_stale(void) {
    Fixture f; setup(&f);
    CHECK(ps3_ppu_service_abandon(NULL, (ps3_ppu_ref){0, 1}) == PS3_PPU_EINVAL && ps3_ppu_service_start(NULL, (ps3_ppu_ref){0, 1}) == PS3_PPU_EINVAL);
    ps3_ppu_ref a = reserve_ok(&f, 0, noop_body, NULL, 0x4000);
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_OK);
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !f.threads[0].started);   /* no host join, nothing started */
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_ESRCH);      /* double abandon rejected */
    CHECK(ps3_ppu_service_start(&f.ppu, a) == PS3_PPU_ESRCH && ps3_ppu_service_join(&f.ppu, a, &code) == PS3_PPU_ESRCH && code == CODE_SENTINEL);
    ps3_ppu_ref b = reserve_ok(&f, 1, noop_body, NULL, 0x4000);
    CHECK(b.slot == a.slot && b.id == a.id + 1);                     /* slot reused, id never reused */
    CHECK(ps3_ppu_service_start(&f.ppu, a) == PS3_PPU_ESRCH && ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_ESRCH);   /* stale: cannot touch b */
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000 && life_of(&f, b) == PS3_PPU_SERVICE_RESERVED);
    CHECK(ps3_ppu_service_start(&f.ppu, (ps3_ppu_ref){b.slot, 0}) == PS3_PPU_ESRCH);
    CHECK(ps3_ppu_service_start(&f.ppu, (ps3_ppu_ref){PS3_PPU_SERVICE_CAP, b.id}) == PS3_PPU_ESRCH);
    CHECK(ps3_ppu_service_start(&f.ppu, b) == PS3_PPU_OK && f.threads[1].started && !f.threads[0].started);
    CHECK(ps3_ppu_service_join(&f.ppu, b, &code) == PS3_PPU_OK && code == 5 && used(&f) == 0);
    CHECK(ps3_ppu_service_abandon(&f.ppu, b) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* ---- explicit start failure keeps the reservation: retry or abandon ---- */
static void start_failure_retains(void) {
    Fixture f; setup(&f);
    Probe p; memset(&p, 0, sizeof p);
    ps3_ppu_ref r = reserve_ok(&f, 0, probe_body, &p, 0x4000);
    p.published_id = r.id; p.param = 0xC0FFEEu;
    f.threads[0].fail_next_start = 1;
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_EAGAIN);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_RESERVED && used(&f) == 0x4000 && ps3_ppu_threads_live(&f.ppu) == 1);
    CHECK(!f.threads[0].started && p.runs == 0);
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_OK && p.runs == 1 && p.observed_match);   /* same ref, retry works */
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 11 && used(&f) == 0);
    /* failure then explicit abandon releases the quota; the id stays consumed */
    ps3_ppu_ref r2 = reserve_ok(&f, 1, noop_body, NULL, 0x4000);
    f.threads[1].fail_next_start = 1;
    CHECK(r2.id == r.id + 1 && ps3_ppu_service_start(&f.ppu, r2) == PS3_PPU_EAGAIN);
    CHECK(ps3_ppu_service_abandon(&f.ppu, r2) == PS3_PPU_OK && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    CHECK(ps3_ppu_service_start(&f.ppu, r2) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* ---- reserve rejections: create's validation precedence, no charge, no id, *ref untouched ---- */
static void reserve_rejections(void) {
    Fixture f; setup(&f);
    const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
    ps3_ppu_ref r = sentinel;
    ps3_service_thread_port good = tp(&f, 0), nostart = good, nojoin = good;
    nostart.start = NULL; nojoin.join = NULL;
    CHECK(ps3_ppu_service_reserve(NULL, 1, 249, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, good, NULL, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, good, noop_body, NULL, NULL) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, nostart, noop_body, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, nojoin, noop_body, NULL, &r) == PS3_PPU_EINVAL);
    ps3_ppu_threads cold; memset(&cold, 0, sizeof cold);
    CHECK(ps3_ppu_service_reserve(&cold, 1, 249, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 3072, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EINVAL);   /* EINVAL beats UNSUPPORTED */
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 250, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_UNSUPPORTED);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 251, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_UNSUPPORTED);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, UINT64_MAX, good, noop_body, NULL, &r) == PS3_PPU_EINVAL);    /* rounding overflow */
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, QUOTA + 1, good, noop_body, NULL, &r) == PS3_PPU_ENOMEM);
    ps3_ppu_threads_limit(&f.ppu, 0);
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, QUOTA + 1, good, noop_body, NULL, &r) == PS3_PPU_ENOMEM);   /* quota before capacity */
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EAGAIN);
    ps3_ppu_threads_limit(&f.ppu, PS3_PPU_SERVICE_CAP);
    CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !f.threads[0].started);
    {   /* no adopted main id: nothing charged */
        ps3_ppu_threads fresh; memset(&fresh, 0, sizeof fresh);
        CHECK(ps3_ppu_threads_init(&fresh, ps3_pthread_guard_port(&f.ppu_guard), ps3_pthread_wait_port_port(&f.port), &f.memory) == PS3_PPU_OK);
        CHECK(ps3_ppu_service_reserve(&fresh, 1, 249, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EINVAL && used(&f) == 0 && r.id == sentinel.id);
    }
    /* none of the failures consumed an id */
    ps3_ppu_ref ok = reserve_ok(&f, 0, noop_body, NULL, 0x4000);
    CHECK(ok.id == 0x01000000u && ps3_ppu_service_abandon(&f.ppu, ok) == PS3_PPU_OK);
    /* a reservation counts against live capacity ... */
    ps3_ppu_threads_limit(&f.ppu, 1);
    ps3_ppu_ref a = reserve_ok(&f, 0, noop_body, NULL, 0x4000);
    CHECK(a.id == 0x01000001u);                                      /* abandoned id consumed forever */
    r = sentinel;
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, good, noop_body, NULL, &r) == PS3_PPU_EAGAIN && r.id == sentinel.id && used(&f) == 0x4000);
    CHECK(ps3_ppu_service_abandon(&f.ppu, a) == PS3_PPU_OK);
    ps3_ppu_ref b = reserve_ok(&f, 0, noop_body, NULL, 0x4000);
    CHECK(b.id == 0x01000002u && ps3_ppu_service_abandon(&f.ppu, b) == PS3_PPU_OK);
    ps3_ppu_threads_limit(&f.ppu, PS3_PPU_SERVICE_CAP);
    /* ... and against the whole table (CAP slots) ... */
    r = sentinel;
    ps3_ppu_ref many[PS3_PPU_SERVICE_CAP];
    for (unsigned i = 0; i < PS3_PPU_SERVICE_CAP; ++i) many[i] = reserve_ok(&f, 0, noop_body, NULL, 0x1000);
    CHECK(ps3_ppu_threads_live(&f.ppu) == PS3_PPU_SERVICE_CAP && used(&f) == 0x1000u * PS3_PPU_SERVICE_CAP);
    /* the fixture has more charge records and quota than services, so the (CAP+1)th reservation is refused by the TABLE (EAGAIN), with no charge */
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x1000, good, noop_body, NULL, &r) == PS3_PPU_EAGAIN && used(&f) == 0x1000u * PS3_PPU_SERVICE_CAP && r.id == sentinel.id);
    for (unsigned i = 0; i < PS3_PPU_SERVICE_CAP; ++i) CHECK(ps3_ppu_service_abandon(&f.ppu, many[i]) == PS3_PPU_OK);
    /* ... and against quota */
    ps3_ppu_ref big = reserve_ok(&f, 0, noop_body, NULL, QUOTA);
    CHECK(used(&f) == QUOTA && ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x1000, good, noop_body, NULL, &r) == PS3_PPU_ENOMEM);
    CHECK(ps3_ppu_service_abandon(&f.ppu, big) == PS3_PPU_OK && used(&f) == 0);
    ps3_ppu_ref small = reserve_ok(&f, 0, noop_body, NULL, 0x1000);
    CHECK(ps3_ppu_service_abandon(&f.ppu, small) == PS3_PPU_OK && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}

/* ---- wait-port failure inside start still owns the live record ---- */
static void start_wait_failure_owned(void) {
    Fixture f; setup(&f);
    ps3_ppu_threads bad; memset(&bad, 0, sizeof bad);
    ps3_notify_port port = {NULL, failing_wait, failing_wake};
    CHECK(ps3_ppu_threads_init(&bad, ps3_pthread_guard_port(&f.ppu_guard), port, &f.memory) == PS3_PPU_OK);
    CHECK(ps3_ppu_threads_adopt_main(&bad, 1) == PS3_PPU_OK);
    uint32_t release = 0, code = CODE_SENTINEL;
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_reserve(&bad, 1, 249, 250, 0x4000, tp(&f, 0), spin_body, &release, &r) == PS3_PPU_OK);
    CHECK(!f.threads[0].started && ps3_ppu_threads_live(&bad) == 1 && used(&f) == 0x4000);
    CHECK(ps3_ppu_service_start(&bad, r) == PS3_PPU_OWNED_FAILURE);
    CHECK(f.threads[0].started && ps3_ppu_threads_live(&bad) == 1 && used(&f) == 0x4000);
    CHECK(ps3_ppu_service_start(&bad, r) == PS3_PPU_EINVAL);         /* in-progress flag cleared; not startable again */
    CHECK(ps3_ppu_service_abandon(&bad, r) == PS3_PPU_EINVAL && ps3_ppu_threads_live(&bad) == 1);
    __atomic_store_n(&release, 1, __ATOMIC_SEQ_CST);
    CHECK(ps3_ppu_service_join(&bad, r, &code) == PS3_PPU_OK && code == 7 && used(&f) == 0 && ps3_ppu_threads_live(&bad) == 0);
    teardown(&f);
}

/* ---- serialization: start vs join/abandon/start while start owns the record ---- */
typedef struct { ps3_service_thread_port real; uint32_t entered, release; } Gate;
static int gate_start(void *c, void (*entry)(void *), void *argument) {
    Gate *g = c;
    set_flag(&g->entered);               /* start() is now inside thread.start, outside the PPU guard */
    wait_flag(&g->release);
    return g->real.start(g->real.context, entry, argument);
}
static int gate_join(void *c) { Gate *g = c; return g->real.join(g->real.context); }
typedef struct { Fixture *f; ps3_ppu_ref ref; uint32_t rc; } Caller;
static void *start_main(void *p) { Caller *c = p; c->rc = ps3_ppu_service_start(&c->f->ppu, c->ref); return NULL; }
static void *abandon_main(void *p) { Caller *c = p; c->rc = ps3_ppu_service_abandon(&c->f->ppu, c->ref); return NULL; }
static void *join_caller_main(void *p) { Caller *c = p; uint32_t code; c->rc = ps3_ppu_service_join(&c->f->ppu, c->ref, &code); return NULL; }
static void expect_busy_no_effects(Fixture *f, ps3_ppu_ref r, uint64_t charged) {
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f->ppu, r, &code) == PS3_PPU_EBUSY && code == CODE_SENTINEL);
    CHECK(ps3_ppu_service_abandon(&f->ppu, r) == PS3_PPU_EBUSY);
    CHECK(ps3_ppu_service_start(&f->ppu, r) == PS3_PPU_EBUSY);
    CHECK(!ps3_ppu_service_is_blocked(&f->ppu, r));
    CHECK(ps3_ppu_threads_live(&f->ppu) == 1 && used(f) == charged);
}
static void start_serializes_inside_thread_start(void) {
    Fixture f; setup(&f);
    Gate g; memset(&g, 0, sizeof g); g.real = tp(&f, 0);
    ps3_service_thread_port gp = {&g, gate_start, gate_join};
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_reserve(&f.ppu, 1, 249, 250, 0x4000, gp, noop_body, NULL, &r) == PS3_PPU_OK);
    Caller st = {&f, r, CODE_SENTINEL};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, start_main, &st) == 0);
    wait_flag(&g.entered);
    expect_busy_no_effects(&f, r, 0x4000);
    CHECK(!f.threads[0].started);                                    /* the real host thread has not been created yet */
    set_flag(&g.release);
    CHECK(pthread_join(t, NULL) == 0 && st.rc == PS3_PPU_OK && f.threads[0].started);
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 5 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}
typedef struct { uint32_t entered, release; } Hold;
static void hold_body(ps3_ppu_service *self, void *argument) {
    Hold *h = argument;
    set_flag(&h->entered);               /* running, activity still RUNNING: start() is parked in its prefix wait */
    wait_flag(&h->release);
    ps3_ppu_service_exit(self, 7);       /* exit publication is legal while start is still waiting */
}
static void start_serializes_inside_prefix_wait(void) {
    Fixture f; setup(&f);
    Hold h; memset(&h, 0, sizeof h);
    ps3_ppu_ref r = reserve_ok(&f, 0, hold_body, &h, 0x4000);
    Caller st = {&f, r, CODE_SENTINEL};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, start_main, &st) == 0);
    wait_flag(&h.entered);
    expect_busy_no_effects(&f, r, 0x4000);   /* (adapter .started is written by the starter thread after pthread_create: read only after joining it) */
    set_flag(&h.release);
    CHECK(pthread_join(t, NULL) == 0 && st.rc == PS3_PPU_OK && f.threads[0].started);
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 7 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}
typedef struct { uint32_t runs; } Count;
static void count_body(ps3_ppu_service *self, void *argument) {
    __atomic_add_fetch(&((Count *)argument)->runs, 1, __ATOMIC_SEQ_CST);
    ps3_ppu_service_exit(self, 3);
}
static int allowed(uint32_t rc) { return rc == PS3_PPU_OK || rc == PS3_PPU_EBUSY || rc == PS3_PPU_EINVAL || rc == PS3_PPU_ESRCH; }
static void concurrent_start_abandon_join(void) {
    for (int round = 0; round < 50; ++round) {
        Fixture f; setup(&f);
        Count c = {0};
        ps3_ppu_ref r = reserve_ok(&f, 0, count_body, &c, 0x4000);
        Caller s1 = {&f, r, CODE_SENTINEL}, s2 = s1, ab = s1, jn = s1;
        pthread_t t[4];
        CHECK(pthread_create(&t[0], NULL, start_main, &s1) == 0 && pthread_create(&t[1], NULL, start_main, &s2) == 0 &&
              pthread_create(&t[2], NULL, abandon_main, &ab) == 0 && pthread_create(&t[3], NULL, join_caller_main, &jn) == 0);
        for (int i = 0; i < 4; ++i) CHECK(pthread_join(t[i], NULL) == 0);
        CHECK(allowed(s1.rc) && allowed(s2.rc) && allowed(ab.rc) && allowed(jn.rc));
        const int started = (s1.rc == PS3_PPU_OK) + (s2.rc == PS3_PPU_OK), abandoned = ab.rc == PS3_PPU_OK, joined = jn.rc == PS3_PPU_OK;
        CHECK(started + abandoned == 1);                             /* exactly one of: started once, released unstarted */
        CHECK(c.runs == (uint32_t)started);                          /* never a double start */
        CHECK(!joined || started == 1);                              /* a reap needs a real start */
        uint32_t code = 0;
        if (started && !joined) CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 3);   /* reaped exactly once */
        CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_ESRCH);
        CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
        teardown(&f);
    }
}

/* ---- cleanup failure never silently discards an owned quota record ----
 * FAULT INJECTION (narrowly labelled): the service's saved token is overwritten with an invalid byte pattern while the
 * genuine allocator charge stays live, so the real uncharge rejects it; the exact saved token is restored for the retry.
 * This proves invalid-token failure handling, NOT that a normal uncharge spontaneously fails. */
static ps3_user_memory_charge_token poison_token(Fixture *f, ps3_ppu_ref r) {
    ps3_user_memory_charge_token saved = f->ppu.services[r.slot].stack;
    memset(&f->ppu.services[r.slot].stack, 0xFF, sizeof saved);
    return saved;
}
static void restore_token(Fixture *f, ps3_ppu_ref r, ps3_user_memory_charge_token saved) { f->ppu.services[r.slot].stack = saved; }
static void abandon_cleanup_failure_retains(void) {
    Fixture f; setup(&f);
    ps3_ppu_ref r = reserve_ok(&f, 0, noop_body, NULL, 0x4000);
    ps3_user_memory_charge_token saved = poison_token(&f, r);
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_PROVIDER_FAILURE);
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && life_of(&f, r) == PS3_PPU_SERVICE_RESERVED && used(&f) == 0x4000);   /* retained, still charged */
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_PROVIDER_FAILURE && ps3_ppu_threads_live(&f.ppu) == 1);   /* retry stays explicit */
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_EINVAL && code == CODE_SENTINEL);
    restore_token(&f, r, saved);
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_OK && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);   /* retry succeeds once */
    CHECK(ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* ---- join: a successful host join is remembered; a failed quota release retains CLEANUP_PENDING ---- */
typedef struct { ps3_service_thread_port real; unsigned calls, ok; } CountedJoin;
static int counted_join(void *c) { CountedJoin *j = c; int rc = j->real.join(j->real.context); ++j->calls; if (!rc) ++j->ok; return rc; }
static int counted_start(void *c, void (*entry)(void *), void *argument) { CountedJoin *j = c; return j->real.start(j->real.context, entry, argument); }
static void join_cleanup_pending(void) {
    Fixture f; setup(&f);
    CountedJoin cj; memset(&cj, 0, sizeof cj); cj.real = tp(&f, 0);
    ps3_service_thread_port port = {&cj, counted_start, counted_join};
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, port, noop_body, NULL, &r) == PS3_PPU_OK);
    ps3_user_memory_charge_token saved = poison_token(&f, r);
    uint32_t code = CODE_SENTINEL;
    f.threads[0].fail_next_join = 1;                                 /* attempt 1: the HOST join fails: old retry behaviour, not remembered */
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_PROVIDER_FAILURE && cj.calls == 1 && cj.ok == 0);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_EXITED && !f.ppu.services[r.slot].host_join_complete && ps3_ppu_threads_live(&f.ppu) == 1);
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_PROVIDER_FAILURE);   /* attempt 2: host join succeeds, quota release fails */
    CHECK(cj.calls == 2 && cj.ok == 1 && code == 5);
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_CLEANUP_PENDING && f.ppu.services[r.slot].host_join_complete);
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000);  /* retained, still charged */
    code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_PROVIDER_FAILURE && cj.calls == 2);   /* still failing: NO second host join */
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_EINVAL && ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_EINVAL && ps3_ppu_threads_live(&f.ppu) == 1);
    restore_token(&f, r, saved);
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 5);
    CHECK(cj.calls == 2 && cj.ok == 1);                              /* exactly one successful host join across the whole retry chain */
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_ESRCH);
    teardown(&f);
}

/* ---- create stays the compatibility wrapper ---- */
typedef struct { Fixture *f; ps3_user_memory_charge_token saved; unsigned joins; int fail_cleanup; } Thief;
static int thief_start(void *context, void (*entry)(void *), void *argument) {   /* injected start adapter: fails, and (optionally) poisons only the service token */
    Thief *t = context; (void)entry; (void)argument;
    if (t->fail_cleanup) t->saved = poison_token(t->f, (ps3_ppu_ref){0, 0});
    return EAGAIN;
}
static int thief_join(void *context) { Thief *t = context; ++t->joins; return EINVAL; }
static void create_wrapper(void) {
    Fixture f; setup(&f);
    const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
    ps3_ppu_ref r = sentinel;
    /* ordinary start failure: abandoned, *ref untouched, id consumed */
    f.threads[0].fail_next_start = 1;
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EAGAIN);
    CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !f.threads[0].started);
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_OK && r.id == 0x01000001u);
    uint32_t code = 0;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 5 && used(&f) == 0);
    /* a real exit while create waits and a real LV2 block are covered by the unchanged original tests that run first */
    /* start failure whose quota release ALSO fails: OWNED_FAILURE publishes the ref of a CLEANUP_PENDING record, no host thread */
    Thief thief; memset(&thief, 0, sizeof thief); thief.f = &f; thief.fail_cleanup = 1;
    ps3_service_thread_port tport = {&thief, thief_start, thief_join};
    r = sentinel;
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tport, noop_body, NULL, &r) == PS3_PPU_OWNED_FAILURE);
    CHECK(r.id == 0x01000002u && r.slot == 0 && ps3_ppu_threads_live(&f.ppu) == 1 && life_of(&f, r) == PS3_PPU_SERVICE_CLEANUP_PENDING);
    CHECK(f.ppu.services[r.slot].host_join_complete && used(&f) == 0x4000 && !f.threads[0].started && !ps3_ppu_service_is_blocked(&f.ppu, r));
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_EINVAL && ps3_ppu_service_abandon(&f.ppu, r) == PS3_PPU_EINVAL);
    code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_PROVIDER_FAILURE && ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000);   /* poisoned still */
    CHECK(code == CODE_SENTINEL);                                    /* F2: no service ever EXITED, so no invented exit code */
    restore_token(&f, r, thief.saved);
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == CODE_SENTINEL);   /* the caller's usual stop+join frees it; exit_code untouched */
    CHECK(thief.joins == 0 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);   /* a join of a thread that never existed was never attempted */
    teardown(&f);
}

/* ---- F2: exit_code is written only when a real service EXITED was published ---- */
static void silent_body(ps3_ppu_service *self, void *argument) { (void)self; (void)argument; }   /* returns: the trampoline publishes exit(0) */
static void code_body(ps3_ppu_service *self, void *argument) { ps3_ppu_service_exit(self, *(uint32_t *)argument); }
static void exit_code_protocol(void) {
    Fixture f; setup(&f);
    uint32_t want = 0x1234u, code = CODE_SENTINEL;
    ps3_ppu_ref r = {0};
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 0), code_body, &want, &r) == PS3_PPU_OK);
    ps3_user_memory_charge_token saved = poison_token(&f, r);
    for (int attempt = 0; attempt < 3; ++attempt) {                  /* real EXITED code survives every cleanup retry */
        code = CODE_SENTINEL;
        CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_PROVIDER_FAILURE && code == 0x1234u);
        CHECK(life_of(&f, r) == PS3_PPU_SERVICE_CLEANUP_PENDING && used(&f) == 0x4000);
    }
    restore_token(&f, r, saved);
    code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 0x1234u && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    /* a real exit(0) published by the trampoline IS a real code: it overwrites the sentinel */
    code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 1), silent_body, NULL, &r) == PS3_PPU_OK);
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 0 && used(&f) == 0);
    /* a null exit_code pointer is still legal on every path */
    CHECK(ps3_ppu_service_create(&f.ppu, 1, 249, 250, 0x4000, tp(&f, 2), code_body, &want, &r) == PS3_PPU_OK);
    CHECK(ps3_ppu_service_join(&f.ppu, r, NULL) == PS3_PPU_OK && used(&f) == 0);
    teardown(&f);
}

/* ---- F1: adversarial ref race at the failed-start transition (OUT OF CONTRACT for current callers: the ref never
 * escapes create before it returns today; this protects the later guest-id -> ref resolution) ----
 * A real guard wrapper releases the actual mutex and then parks the creator (create's own thread) right after start's
 * failure transition made the record an unstarted RESERVED reservation again. Another actor then uses the predictable
 * ref (sequential ids) through the real public API before the creator resumes its cleanup. */
typedef struct { ps3_instance_guard real; uint32_t armed_at, unlocks, paused, resume; } PauseGuard;
static _Thread_local int pause_creator;
static void pg_lock(void *c) { PauseGuard *g = c; g->real.lock(g->real.context); }
static void pg_unlock(void *c) {
    PauseGuard *g = c;
    g->real.unlock(g->real.context);                                 /* the REAL mutex is free before the creator parks */
    if (pause_creator && ++g->unlocks == g->armed_at) { set_flag(&g->paused); wait_flag(&g->resume); }
}
typedef struct { ps3_service_thread_port real; unsigned starts, joins, joins_ok; } FirstFail;   /* first start fails, later starts are REAL */
static int ff_start(void *c, void (*entry)(void *), void *argument) {
    FirstFail *x = c;
    if (++x->starts == 1) return EAGAIN;
    return x->real.start(x->real.context, entry, argument);
}
static int ff_join(void *c) { FirstFail *x = c; int rc = x->real.join(x->real.context); ++x->joins; if (!rc) ++x->joins_ok; return rc; }
typedef struct { Fixture *f; ps3_service_thread_port port; ps3_ppu_service_body body; void *argument; uint64_t stack; ps3_ppu_ref out; uint32_t rc; } Creator;
static void *creator_main(void *p) {
    Creator *c = p;
    pause_creator = 1;                                               /* only this thread is parked by the wrapper */
    c->rc = ps3_ppu_service_create(&c->f->ppu, 1, 249, 250, c->stack, c->port, c->body, c->argument, &c->out);
    return NULL;
}
static void install_pause_guard(Fixture *f, PauseGuard *g) {         /* fresh registry on the same real guard, memory and wait port */
    memset(g, 0, sizeof *g);
    g->real = ps3_pthread_guard_port(&f->ppu_guard); g->armed_at = 3;   /* unlock 1 = reserve, 2 = start(RUNNING), 3 = start's failure transition */
    memset(&f->ppu, 0, sizeof f->ppu);
    ps3_instance_guard wrapped = {g, pg_lock, pg_unlock};
    CHECK(ps3_ppu_threads_init(&f->ppu, wrapped, ps3_pthread_wait_port_port(&f->port), &f->memory) == PS3_PPU_OK);
    CHECK(ps3_ppu_threads_adopt_main(&f->ppu, 1) == PS3_PPU_OK);
}
static ps3_ppu_ref park_creator(Fixture *f, PauseGuard *g, Creator *c, pthread_t *t) {
    CHECK(pthread_create(t, NULL, creator_main, c) == 0);
    wait_flag(&g->paused);
    const ps3_ppu_ref predicted = {0, PS3_PPU_THREAD_BASE};          /* first service id in slot 0: no secret needed */
    CHECK(life_of(f, predicted) == PS3_PPU_SERVICE_RESERVED && !f->ppu.services[0].starting);   /* the exact failed-start transition */
    CHECK(f->ppu.services[0].id == predicted.id && ps3_ppu_threads_live(&f->ppu) == 1);
    return predicted;
}
static void live_ref_race(int exited_variant) {
    Fixture f; setup(&f);
    PauseGuard g; install_pause_guard(&f, &g);
    FirstFail ff; memset(&ff, 0, sizeof ff); ff.real = tp(&f, 0);
    ps3_service_thread_port port = {&ff, ff_start, ff_join};
    ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    uint32_t q = 0, p = 0;
    HelperArgs args = {&f.lv2, 0, {0}, {0, 0, 1, 0}, 0xFFFFFFFFu};
    if (!exited_variant) {
        CHECK(ps3_lv2_queue_create(&f.lv2, &q, &qa, 0, 42) == PS3_LV2_OK);
        CHECK(ps3_lv2_port_create(&f.lv2, &p, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && ps3_lv2_port_connect_local(&f.lv2, p, q) == PS3_LV2_OK);
        args.queue = q;
        CHECK(ps3_lv2_queue_bind_service(&f.lv2, q, args.owned) == PS3_LV2_OK);
    }
    Creator c = {&f, port, exited_variant ? noop_body : helper_body, exited_variant ? NULL : (void *)&args, 0x8000,
                 {0xDEADu, 0xBEEFu}, 0};
    pthread_t t;
    const ps3_ppu_ref ref = park_creator(&f, &g, &c, &t);
    /* the other actor starts the REAL pthread service of the still-live reservation */
    CHECK(ps3_ppu_service_start(&f.ppu, ref) == PS3_PPU_OK && ff.starts == 2);
    if (!exited_variant) {
        CHECK(ps3_ppu_service_is_blocked(&f.ppu, ref));              /* really inside the LV2 receive wait */
        ps3_lv2_queue snap;
        CHECK(ps3_lv2_queue_snapshot(&f.lv2, q, &snap) == PS3_LV2_OK && snap.receivers == 1 && snap.blocked_receivers == 1);
    } else CHECK(life_of(&f, ref) == PS3_PPU_SERVICE_EXITED);
    set_flag(&g.resume);
    CHECK(pthread_join(t, NULL) == 0);
    /* bad flag/status is asserted BEFORE anything is joined, so a regression aborts without a use-after-free */
    CHECK(c.rc == PS3_PPU_OWNED_FAILURE && c.out.slot == ref.slot && c.out.id == ref.id);   /* live ref published for the owner's cancel+join */
    CHECK(life_of(&f, ref) == (exited_variant ? PS3_PPU_SERVICE_EXITED : PS3_PPU_SERVICE_RUNNING));   /* real state retained */
    CHECK(!f.ppu.services[ref.slot].host_join_complete);             /* a live host thread is never marked joined */
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x8000 && ff.joins == 0);
    if (!exited_variant) {
        CHECK(ps3_ppu_service_is_blocked(&f.ppu, ref));
        CHECK(ps3_lv2_port_send(&f.lv2, p, 0, 1, 0) == PS3_LV2_OK);  /* the owner's cancel */
    }
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, ref, &code) == PS3_PPU_OK && code == (exited_variant ? 5u : 0u));
    CHECK(ff.joins == 1 && ff.joins_ok == 1 && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);   /* host thread joined exactly once */
    CHECK(ps3_ppu_service_join(&f.ppu, ref, &code) == PS3_PPU_ESRCH && ff.joins == 1);
    if (!exited_variant)
        CHECK(ps3_lv2_port_disconnect(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_port_destroy(&f.lv2, p) == PS3_LV2_OK && ps3_lv2_queue_destroy(&f.lv2, q) == PS3_LV2_OK);
    teardown(&f);
}
static void live_ref_race_real_wait(void) { live_ref_race(0); }
static void live_ref_race_exited(void) { live_ref_race(1); }
static void retired_ref_race(void) {
    for (int recycle = 0; recycle < 2; ++recycle) {
        Fixture f; setup(&f);
        PauseGuard g; install_pause_guard(&f, &g);
        FirstFail ff; memset(&ff, 0, sizeof ff); ff.real = tp(&f, 0);
        ps3_service_thread_port port = {&ff, ff_start, ff_join};
        const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
        Creator c = {&f, port, noop_body, NULL, 0x4000, sentinel, 0};
        pthread_t t;
        const ps3_ppu_ref ref = park_creator(&f, &g, &c, &t);
        CHECK(ps3_ppu_service_abandon(&f.ppu, ref) == PS3_PPU_OK && used(&f) == 0);   /* the other actor retires the reservation ... */
        ps3_ppu_ref again = {0};
        if (recycle) {                                               /* ... and (optionally) recycles its slot under a new id */
            again = reserve_ok(&f, 1, noop_body, NULL, 0x2000);
            CHECK(again.slot == ref.slot && again.id == ref.id + 1);
        }
        set_flag(&g.resume);
        CHECK(pthread_join(t, NULL) == 0);
        CHECK(c.rc == PS3_PPU_EAGAIN);                               /* the original start failure class */
        CHECK(c.out.slot == sentinel.slot && c.out.id == sentinel.id);   /* a retired/recycled ref is NEVER published */
        CHECK(ff.starts == 1 && ff.joins == 0 && !f.threads[0].started && !f.threads[1].started);
        if (recycle) {                                               /* the recycled reservation is untouched */
            CHECK(life_of(&f, again) == PS3_PPU_SERVICE_RESERVED && used(&f) == 0x2000 && ps3_ppu_threads_live(&f.ppu) == 1);
            CHECK(ps3_ppu_service_abandon(&f.ppu, again) == PS3_PPU_OK);
        }
        CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
        teardown(&f);
    }
}

typedef struct { const char *name; void (*fn)(void); } Case;
/* ---- fenced run-to-exit reservation: the SAME reserve_guest body, only the priority rule relaxed ---- */
static uint32_t fenced_rc(Fixture *f, uint32_t priority, uint32_t creator, ps3_ppu_ref *r) {
    return ps3_ppu_service_reserve_guest_fenced(&f->ppu, 4, priority, creator, 0x4000, 0x10000, tp(f, 0), noop_body, NULL, r);
}
static void fenced_reservation(void) {
    Fixture f; setup(&f);
    const ps3_ppu_ref sentinel = {0xDEADu, 0xBEEFu};
    ps3_ppu_ref r = sentinel;
    /* the game's third child {priority 2000, creator 250, stack 0x4000}: refused unfenced, admitted fenced */
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 4, 2000, 250, 0x4000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_UNSUPPORTED);
    CHECK(r.slot == sentinel.slot && r.id == sentinel.id && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && !f.threads[0].started);
    CHECK(fenced_rc(&f, 2000, 250, &r) == PS3_PPU_OK);
    CHECK(r.id == 0x01000000u && ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x20000);   /* identical storage accounting */
    ps3_ppu_guest_storage g;
    CHECK(ps3_ppu_service_guest_storage(&f.ppu, r, &g) == PS3_PPU_OK);
    CHECK(g.stack_logical_bytes == 0x4000 && g.stack_region_bytes == 0x10000 && g.tls_capacity_bytes == 0x10000 && g.allocation_bytes == 0x20000);
    CHECK(ps3_ppu_service_start(&f.ppu, r) == PS3_PPU_OK && f.threads[0].started);   /* the prefix wait still fences the child */
    CHECK(life_of(&f, r) == PS3_PPU_SERVICE_EXITED);
    { uint32_t code = 0; CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK); }
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    /* priority > 3071 stays EINVAL on BOTH entry points, and no id is consumed */
    r = sentinel;
    CHECK(fenced_rc(&f, 3072, 250, &r) == PS3_PPU_EINVAL && r.slot == sentinel.slot && r.id == sentinel.id);
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 4, 3072, 250, 0x4000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EINVAL);
    /* the rest of the validation is shared: EINVAL/UNSUPPORTED precedence and effects are unchanged */
    CHECK(ps3_ppu_service_reserve_guest_fenced(NULL, 4, 2000, 250, 0x4000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve_guest_fenced(&f.ppu, 4, 2000, 250, 0x4000, 0x10000, tp(&f, 0), NULL, NULL, &r) == PS3_PPU_EINVAL);
    CHECK(ps3_ppu_service_reserve_guest_fenced(&f.ppu, 4, 2000, 250, 0x4000, 0, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_EINVAL);   /* tls_capacity 0 */
    {   ps3_service_thread_port nostart = tp(&f, 0); nostart.start = NULL;
        CHECK(ps3_ppu_service_reserve_guest_fenced(&f.ppu, 4, 2000, 250, 0x4000, 0x10000, nostart, noop_body, NULL, &r) == PS3_PPU_EINVAL); }
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0 && r.id == sentinel.id);
    /* the equal-priority case is admitted fenced too, and a run-to-exit child above its creator is not */
    ps3_ppu_ref same = {0};
    CHECK(fenced_rc(&f, 250, 250, &same) == PS3_PPU_OK && ps3_ppu_service_abandon(&f.ppu, same) == PS3_PPU_OK);
    ps3_ppu_ref high = {0};
    CHECK(fenced_rc(&f, 249, 250, &high) == PS3_PPU_OK && ps3_ppu_service_abandon(&f.ppu, high) == PS3_PPU_OK);
    /* the unfenced entry point still refuses equal/lower priority for the same request */
    CHECK(ps3_ppu_service_reserve_guest(&f.ppu, 4, 250, 250, 0x4000, 0x10000, tp(&f, 0), noop_body, NULL, &r) == PS3_PPU_UNSUPPORTED);
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}
/* ---- background start returns while the body is still running and publishes no synthetic activity ---- */
typedef struct { uint32_t entered, release, exit_code; } BackgroundProbe;
static void background_body(ps3_ppu_service *self, void *argument) {
    BackgroundProbe *p = argument;
    set_flag(&p->entered);
    wait_flag(&p->release);
    ps3_ppu_service_exit(self, p->exit_code);
}
static void start_background(void) {
    Fixture f; setup(&f);
    BackgroundProbe p; memset(&p, 0, sizeof p); p.exit_code = 0x2A;
    ps3_ppu_ref r = reserve_ok(&f, 0, background_body, &p, 0x4000);
    CHECK(ps3_ppu_service_start_background(&f.ppu, r) == PS3_PPU_OK);
    wait_flag(&p.entered);                                            /* the body is really running and has not exited */
    CHECK(f.threads[0].started && life_of(&f, r) == PS3_PPU_SERVICE_RUNNING);
    CHECK(!ps3_ppu_service_is_blocked(&f.ppu, r));                   /* no synthetic BLOCKED/EXITED was published */
    CHECK(ps3_ppu_threads_live(&f.ppu) == 1 && used(&f) == 0x4000);
    CHECK(ps3_ppu_service_start_background(&f.ppu, r) == PS3_PPU_EINVAL);   /* already started */
    CHECK(ps3_ppu_service_start_background(&f.ppu, (ps3_ppu_ref){r.slot, 0}) == PS3_PPU_ESRCH);
    CHECK(ps3_ppu_service_start_background(&f.ppu, (ps3_ppu_ref){PS3_PPU_SERVICE_CAP, r.id}) == PS3_PPU_ESRCH);
    set_flag(&p.release);
    uint32_t code = CODE_SENTINEL;
    CHECK(ps3_ppu_service_join(&f.ppu, r, &code) == PS3_PPU_OK && code == 0x2A);
    CHECK(used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    CHECK(ps3_ppu_service_start_background(&f.ppu, r) == PS3_PPU_ESRCH);    /* retired */
    /* a failed host start leaves the record RESERVED and abandonable */
    ps3_ppu_ref failed = reserve_ok(&f, 1, noop_body, NULL, 0x4000);
    f.threads[1].fail_next_start = 1;
    CHECK(ps3_ppu_service_start_background(&f.ppu, failed) == PS3_PPU_EAGAIN);
    CHECK(life_of(&f, failed) == PS3_PPU_SERVICE_RESERVED && used(&f) == 0x4000 && ps3_ppu_threads_live(&f.ppu) == 1 && !f.threads[1].started);
    CHECK(ps3_ppu_service_abandon(&f.ppu, failed) == PS3_PPU_OK && used(&f) == 0 && ps3_ppu_threads_live(&f.ppu) == 0);
    teardown(&f);
}
static const Case cases[] = {
    {"reserve_publish_then_start", reserve_publish_then_start}, {"start_real_lv2_wait", start_real_lv2_wait},
    {"abandon_reuse_stale", abandon_reuse_stale}, {"start_failure_retains", start_failure_retains},
    {"reserve_rejections", reserve_rejections}, {"start_wait_failure_owned", start_wait_failure_owned},
    {"start_serializes_inside_thread_start", start_serializes_inside_thread_start},
    {"start_serializes_inside_prefix_wait", start_serializes_inside_prefix_wait},
    {"concurrent_start_abandon_join", concurrent_start_abandon_join},
    {"abandon_cleanup_failure_retains", abandon_cleanup_failure_retains}, {"join_cleanup_pending", join_cleanup_pending},
    {"create_wrapper", create_wrapper},
    {"exit_code_protocol", exit_code_protocol}, {"live_ref_race_real_wait", live_ref_race_real_wait},
    {"live_ref_race_exited", live_ref_race_exited}, {"retired_ref_race", retired_ref_race},
    {"fenced_reservation", fenced_reservation}, {"start_background", start_background},
};
int main(int argc, char **argv) {
    const size_t n = sizeof cases / sizeof cases[0];
    if (argc > 1) {
        for (size_t i = 0; i < n; ++i) if (!strcmp(argv[1], cases[i].name)) { cases[i].fn(); printf("case %s: OK\n", cases[i].name); return 0; }
        fprintf(stderr, "unknown case %s\n", argv[1]);
        return 2;
    }
    CHECK(lv2_services_original_main() == 0);   /* the original, unmodified checks */
    for (size_t i = 0; i < n; ++i) { cases[i].fn(); printf("case %s: OK\n", cases[i].name); }
    puts("test_ppu_reservation: OK");
    return 0;
}
