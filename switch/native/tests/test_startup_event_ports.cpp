/* Host behavior test for guest syscalls 134 (sys_event_port_create) and 136 (sys_event_port_connect_local) through the REAL
 * dispatcher (lv2_syscall in startup_execution.cpp) over real collaborators: coherent guest memory (atomic domain), the real
 * trace recorder and discovery budget, the REAL owned ps3_lv2_objects registry (real pthread guard + wait port), the real SPURS
 * composition (startup_spurs_compose + a real N12 initialize in one case) and real PPU service threads for the really blocked
 * receiver. The generated call sites are modelled by setting the argument registers exactly as the `sc` sequence does: a LABEL
 * fixture, not the game. This is HOST test evidence only: no native/Horizon/emulator execution, no secondary guest PPU, no
 * real compiled helper. Expected ids/bytes are derived by hand from the lv2 id contract (port base 0x0E000000, queue base
 * 0x8D000000, step 0x100, never reused) and from the pinned sys_event.cpp (name stored verbatim, no uniqueness, nonzero name is
 * the event source), never captured from the implementation. */
#define main inherited_spurs_main
#include "test_startup_spurs_initialize.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<int> ep_failures{0};
#define PCHECK(value) do { if (!(value)) { std::printf("EVENT_PORTS_FAIL line=%d expression=%s\n", __LINE__, #value); \
    std::fflush(stdout); ep_failures.fetch_add(1); } } while (0)

std::atomic<long long> watchdog_deadline_ms{0};
std::atomic<const char *> watchdog_phase{"idle"};
long long now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void arm(const char *phase, long long ms) { watchdog_phase.store(phase); watchdog_deadline_ms.store(now_ms() + ms); }
void disarm() { watchdog_deadline_ms.store(0); }
void watchdog_main() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const long long deadline = watchdog_deadline_ms.load();
        if (deadline && now_ms() > deadline) {
            std::printf("EVENT_PORTS_WATCHDOG_TIMEOUT phase=%s\n", watchdog_phase.load());
            std::fflush(stdout);
            _exit(124);
        }
    }
}

constexpr uint32_t PORT_BASE = 0x0E000000u, QUEUE_BASE = 0x8D000000u, STEP = 0x100u;
constexpr uint32_t OUT = STACK_BASE + 0x440, ATTRG = STACK_BASE + 0x400, OUT_PAGE1 = STACK_BASE + 0x1040, OUT_PAGE2 = STACK_BASE + 0x2040;
constexpr uint32_t SENT = 0xCAFEF00Du, SERVICE_KIND = 9, SERVICE_PRIORITY = 100, CREATOR_PRIORITY = 250, EXIT_PUBLISHED = 0x7E57u;
constexpr uint64_t NAME_D0A77 = 0xD0A77, NAME_FULL = UINT64_C(0xFEDCBA9876543210), NAME_HIGH = UINT64_C(0x8000000000000001);
constexpr uint64_t R6 = UINT64_C(0x6666666666666666), R7 = UINT64_C(0x7777777777777777), R8 = UINT64_C(0x8888888888888888);
constexpr uint64_t HIGH = UINT64_C(0xABCDEF0100000000);
uint64_t sx(uint32_t cell) { return uint64_t(int64_t(int32_t(cell))); }
void idle_trampoline(void *) {}

struct Obs {
    Runtime *rt = nullptr;
    pthread_t host{};
    ps3_ppu_service *service = nullptr;
    unsigned calls = 0;
    StartupStop stop = StartupStop::Returned;
    ppu_context terminal{};
    bool active_cleared = false, trampoline_cleared = false, batch_closed = false, same_thread = false;
};
void on_terminal(void *context, StartupControl *control) {
    Obs *o = static_cast<Obs *>(context);
    ++o->calls;
    o->stop = control->stop; o->terminal = control->terminal;
    o->active_cleared = active == nullptr;
    o->trampoline_cleared = g_trampoline_fn == nullptr;
    o->batch_closed = !o->rt->recorder.report().batch_active;
    o->same_thread = pthread_equal(pthread_self(), o->host) != 0;
    if (o->service) ps3_ppu_service_exit(o->service, EXIT_PUBLISHED);
}
bool regs_seeded(const ppu_context &c, const ppu_context &s) {
    for (int i : {1, 2, 3, 4, 5, 6, 7, 8, 11}) if (c.gpr[i] != s.gpr[i]) return false;
    return c.thread_id == s.thread_id && c.lr == s.lr;
}

// Counts (and optionally observes) every wake on a registry's notify port; waits are forwarded unchanged.
struct WakeProbe {
    ps3_notify_port real{};
    std::atomic<unsigned> wakes{0};
    std::function<void()> hook;
    static ps3_notify_wait_result wait(void *c, const uint64_t *w, uint64_t e, const uint32_t *cancel, uint64_t d) {
        auto &s = *static_cast<WakeProbe *>(c); return s.real.wait(s.real.context, w, e, cancel, d);
    }
    static void wake(void *c) { auto &s = *static_cast<WakeProbe *>(c); ++s.wakes; if (s.hook) s.hook(); s.real.wake_all(s.real.context); }
    void attach(ps3_lv2_objects &l) { real = l.port; l.port = {this, wait, wake}; }
};
// Guard wrapper: after an unlock it evaluates `predicate` once (outside the guard) and, the first time it holds, runs `action`.
// Single-threaded rigs only. Used to inject a fault at an exact point AFTER a real domain effect.
struct GuardHook {
    ps3_instance_guard real{};
    std::function<bool()> predicate;
    std::function<void()> action;
    bool fired = false, busy = false;
    static void lock(void *c) { auto &h = *static_cast<GuardHook *>(c); h.real.lock(h.real.context); }
    static void unlock(void *c) {
        auto &h = *static_cast<GuardHook *>(c);
        h.real.unlock(h.real.context);
        if (h.predicate && !h.fired && !h.busy) { h.busy = true; if (h.predicate()) { h.fired = true; h.action(); } h.busy = false; }
    }
};
// A private real registry (no SPURS services, hence no other lock traffic) for hook-based and cleanup-order cases.
struct OwnLv2 {
    ps3_pthread_guard guard;
    ps3_pthread_wait_port wait_port;
    GuardHook hook;
    WakeProbe probe;
    ps3_lv2_objects lv2{};
    OwnLv2() {
        PCHECK(!ps3_pthread_guard_init(&guard) && !ps3_pthread_wait_port_init(&wait_port));
        hook.real = ps3_pthread_guard_port(&guard);
        PCHECK(ps3_lv2_objects_init(&lv2, {&hook, GuardHook::lock, GuardHook::unlock}, ps3_pthread_wait_port_port(&wait_port)) == PS3_LV2_OK);
        probe.attach(lv2);
    }
    ~OwnLv2() { ps3_pthread_wait_port_destroy(&wait_port); ps3_pthread_guard_destroy(&guard); }
};
ps3_lv2_port *raw_port(ps3_lv2_objects &l, uint32_t id) { for (auto &p : l.ports) if (p.active && p.id == id) return &p; return nullptr; }
ps3_lv2_queue *raw_queue(ps3_lv2_objects &l, uint32_t id) { for (auto &q : l.queues) if (q.active && q.id == id) return &q; return nullptr; }
uint32_t native_queue(ps3_lv2_objects &l, int32_t size = 8) {
    uint32_t q = 0; ps3_lv2_queue_attr qa{PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
    PCHECK(ps3_lv2_queue_create(&l, &q, &qa, 0, size) == PS3_LV2_OK);
    return q;
}
uint32_t native_port(ps3_lv2_objects &l, uint64_t name = 0) {
    uint32_t p = 0; PCHECK(ps3_lv2_port_create(&l, &p, PS3_EVENT_PORT_LOCAL, name) == PS3_LV2_OK);
    return p;
}

struct State {
    uint64_t published = 0; size_t ports = 0, queues = 0, led_ports = 0, led_queues = 0;
    uint32_t port_serial = 0, queue_serial = 0;
    std::vector<unsigned char> out;
    bool operator==(const State &o) const {
        return published == o.published && ports == o.ports && queues == o.queues && led_ports == o.led_ports && led_queues == o.led_queues &&
               port_serial == o.port_serial && queue_serial == o.queue_serial && out == o.out;
    }
};
struct Rig {
    std::unique_ptr<Runtime> rt;
    Obs obs;
    ppu_context seeded{};
    bool returned = false;
    uint64_t published0 = 0;
    explicit Rig(bool compose = true, bool admit = true) : rt(new Runtime(1, compose, admit)) {
        obs.rt = rt.get(); obs.host = pthread_self();
        rt->control->terminal_notification = {on_terminal, &obs};
        if (compose) home = &rt->comp->lv2;
    }
    StartupControl &ctl() { return *rt->control; }
    ppu_context &cpu() { return rt->execution.ppu.cpu; }
    ps3_lv2_objects *home = nullptr;   // the composed registry; lv2() falls back to it while a case has detached the injected pointer
    ps3_lv2_objects &lv2() { return *(rt->execution.event_queues ? rt->execution.event_queues : home); }
    State state() {
        State s;
        s.published = rt->recorder.report().published_count;
        s.ports = ps3_lv2_live_count(&lv2(), PS3_LV2_KIND_PORT); s.queues = ps3_lv2_live_count(&lv2(), PS3_LV2_KIND_QUEUE);
        s.led_ports = ctl().event_port_owned_count; s.led_queues = ctl().event_queue_owned_count;
        s.port_serial = lv2().serial[PS3_LV2_KIND_PORT]; s.queue_serial = lv2().serial[PS3_LV2_KIND_QUEUE];
        s.out = snapshot(*rt, OUT, 16);
        return s;
    }
    void call(uint64_t number, uint64_t r3, uint64_t r4, uint64_t r5, uint64_t r6 = R6) {
        auto &c = cpu();
        c.gpr[11] = number; c.gpr[3] = r3; c.gpr[4] = r4; c.gpr[5] = r5; c.gpr[6] = r6; c.gpr[7] = R7; c.gpr[8] = R8;
        seeded = c; obs.calls = 0; obs.stop = StartupStop::Returned;
        published0 = rt->recorder.report().published_count;
        ctl().stop = StartupStop::None; active = rt->control.get(); ctl().cpu = &c; g_trampoline_fn = idle_trampoline;
        try { lv2_syscall(&c); returned = true; active = nullptr; } catch (const Terminal &) { returned = false; }
        g_trampoline_fn = nullptr;
    }
    void expect_stop(StartupStop expected, int line) {
        const bool ok = !returned && obs.calls == 1 && obs.stop == expected && ctl().stop == expected && regs_seeded(obs.terminal, seeded) &&
            regs_seeded(cpu(), seeded) && obs.active_cleared && obs.trampoline_cleared && obs.batch_closed && obs.same_thread && active == nullptr;
        if (!ok) {
            std::printf("EVENT_PORTS_FAIL line=%d expected_stop=%d got_stop=%d returned=%d calls=%u\n", line, int(expected), int(obs.stop), int(returned), obs.calls);
            ep_failures.fetch_add(1);
        }
        PCHECK(!rt->recorder.report().batch_active);
    }
    void expect_return(uint64_t r3, int line) {
        const auto &c = cpu();
        const bool ok = returned && obs.calls == 0 && ctl().stop == StartupStop::None && active == nullptr && c.gpr[3] == r3 &&
            c.gpr[4] == seeded.gpr[4] && c.gpr[5] == seeded.gpr[5] && c.gpr[6] == seeded.gpr[6] && c.gpr[7] == R7 && c.gpr[8] == R8 &&
            c.gpr[11] == seeded.gpr[11] && c.gpr[1] == seeded.gpr[1] && c.gpr[2] == seeded.gpr[2] && c.thread_id == seeded.thread_id && c.lr == seeded.lr;
        if (!ok) {
            std::printf("EVENT_PORTS_FAIL line=%d expected_return r3=0x%llx returned=%d calls=%u stop=%d got_r3=0x%llx\n", line,
                (unsigned long long)r3, int(returned), obs.calls, int(ctl().stop), (unsigned long long)c.gpr[3]);
            ep_failures.fetch_add(1);
        }
        PCHECK(!rt->recorder.report().batch_active);
    }
    void expect_write_event(uint64_t index, uint32_t address, uint64_t value, int line) {
        PpuMemoryTraceSample s{};
        const bool ok = rt->recorder.read(size_t(index), s) && s.sequence == index && s.event.origin == PpuMemoryTraceOrigin::Hle &&
            s.event.operation == PpuMemoryTraceOperation::Write && s.event.address == address && s.event.length == 4 && s.event.value == value &&
            s.event.status == PS3_GUEST_MEMORY_OK && !s.event.stored && !s.event.byte_transfer;
        if (!ok) { std::printf("EVENT_PORTS_FAIL line=%d trace event %llu\n", line, (unsigned long long)index); ep_failures.fetch_add(1); }
    }
    uint32_t guest_queue(uint32_t out = OUT) {   // the observed sc128: PRIORITY, PPU, size 127
        const unsigned char attr[16]{0, 0, 0, 2, 0, 0, 0, 1, 0, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E};
        rt->put(ATTRG, attr, sizeof attr); rt->put32(out, 0);
        call(128, out, ATTRG, 0, 127);
        PCHECK(returned && cpu().gpr[3] == 0);
        return rt->get32(out);
    }
    uint32_t guest_port(uint64_t name, uint32_t out = OUT) {   // the observed sc134: LOCAL
        rt->put32(out, 0);
        call(134, out, 1, name);
        PCHECK(returned && cpu().gpr[3] == 0);
        return rt->get32(out);
    }
    void fill_trace() {
        auto &rec = rt->recorder; const size_t capacity = rt->events.size();
        PCHECK(rec.begin(capacity));
        for (size_t i = 0; i < capacity; ++i) rec.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, SENT, 8, 1));
        rec.finish();
    }
    void decommit(uint32_t address) { auto m = rt->mem(); PCHECK(m.decommit(m.context, address & ~0xFFFu, 4096) == PS3_GUEST_MEMORY_OK); }   // page granular
};
ps3_lv2_port port_snap(ps3_lv2_objects &l, uint32_t id, uint32_t expect_rc = PS3_LV2_OK) {
    ps3_lv2_port p{}; PCHECK(ps3_lv2_port_snapshot(&l, id, &p) == expect_rc); return p;
}
ps3_lv2_queue queue_snap(ps3_lv2_objects &l, uint32_t id) {
    ps3_lv2_queue q{}; PCHECK(ps3_lv2_queue_snapshot(&l, id, &q) == PS3_LV2_OK); return q;
}
ps3_lv2_event take(ps3_lv2_objects &l, uint32_t queue) {   // never blocks: a missing event is a test failure, not a hang
    ps3_lv2_event e{}; ps3_lv2_queue q{};
    if (ps3_lv2_queue_snapshot(&l, queue, &q) != PS3_LV2_OK || q.count == 0) { PCHECK(!"an event is queued"); return e; }
    PCHECK(ps3_lv2_queue_receive(&l, queue, &e, nullptr, nullptr) == PS3_LV2_OK); return e;
}

// ---------------------------------------------------------------- domain: named ports (pinned: stored verbatim, no uniqueness)
void case_domain_names() {
    Rig r; auto &l = r.lv2();
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;
    PCHECK(ps3_lv2_port_create(&l, &a, PS3_EVENT_PORT_LOCAL, NAME_D0A77) == PS3_LV2_OK && a == PORT_BASE);
    PCHECK(ps3_lv2_port_create(&l, &b, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && b == PORT_BASE + STEP);
    PCHECK(ps3_lv2_port_create(&l, &c, PS3_EVENT_PORT_LOCAL, NAME_D0A77) == PS3_LV2_OK && c == PORT_BASE + 2 * STEP && c != a);   // duplicate name is valid
    PCHECK(ps3_lv2_port_create(&l, &d, PS3_EVENT_PORT_LOCAL, NAME_FULL) == PS3_LV2_OK && d == PORT_BASE + 3 * STEP);
    PCHECK(ps3_lv2_port_create(&l, &e, PS3_EVENT_PORT_LOCAL, NAME_HIGH) == PS3_LV2_OK && e == PORT_BASE + 4 * STEP);
    PCHECK(port_snap(l, a).name == NAME_D0A77 && port_snap(l, b).name == 0 && port_snap(l, c).name == NAME_D0A77);
    PCHECK(port_snap(l, d).name == NAME_FULL && port_snap(l, e).name == NAME_HIGH);   // all 64 bits survive
    PCHECK(port_snap(l, a).type == PS3_EVENT_PORT_LOCAL && port_snap(l, a).queue == 0 && port_snap(l, a).active);
    uint32_t keep = 0x1234;   // invalid and unsupported inputs leave the output and the id serial untouched
    PCHECK(ps3_lv2_port_create(&l, &keep, PS3_EVENT_PORT_IPC, NAME_D0A77) == PS3_LV2_UNSUPPORTED && keep == 0x1234);
    for (uint32_t bad : {0u, 2u, 4u, 0x80000001u}) PCHECK(ps3_lv2_port_create(&l, &keep, bad, 1) == PS3_LV2_EINVAL && keep == 0x1234);
    PCHECK(ps3_lv2_port_create(&l, nullptr, PS3_EVENT_PORT_LOCAL, 1) == PS3_LV2_EFAULT);
    PCHECK(l.serial[PS3_LV2_KIND_PORT] == 5);
    // Event sources: nonzero name verbatim; NO_NAME keeps (pid 1 << 32) | port id; data words are carried unchanged.
    const uint32_t q = native_queue(l, 8);
    for (uint32_t p : {a, b, c, d, e}) PCHECK(ps3_lv2_port_connect_local(&l, p, q) == PS3_LV2_OK);
    struct { uint32_t port; uint64_t source; } expected[] = {{a, NAME_D0A77}, {b, (UINT64_C(1) << 32) | b}, {c, NAME_D0A77}, {d, NAME_FULL}, {e, NAME_HIGH}};
    for (const auto &row : expected) {
        PCHECK(ps3_lv2_port_send(&l, row.port, 0x11, 0x22, 0x33) == PS3_LV2_OK);
        const ps3_lv2_event ev = take(l, q);
        PCHECK(ev.source == row.source && ev.data1 == 0x11 && ev.data2 == 0x22 && ev.data3 == 0x33);
    }
    // Filter: a service-bound queue still answers FENCED for a named port with non-owned data and changes nothing; owned data passes.
    PCHECK(ps3_lv2_queue_bind_service(&l, q, {0, 7, 8, 9}) == PS3_LV2_OK);
    const ps3_lv2_queue before = queue_snap(l, q);
    PCHECK(ps3_lv2_port_send(&l, a, 1, 2, 3) == PS3_LV2_FENCED);
    const ps3_lv2_queue after = queue_snap(l, q);
    PCHECK(!std::memcmp(&before, &after, sizeof before));
    PCHECK(ps3_lv2_port_send(&l, a, 7, 8, 9) == PS3_LV2_OK);
    const ps3_lv2_event owned = take(l, q);
    PCHECK(owned.source == NAME_D0A77 && owned.data1 == 7 && owned.data2 == 8 && owned.data3 == 9);
    PCHECK(ps3_lv2_queue_unbind_service(&l, q) == PS3_LV2_OK);
    // Overflow: a full queue answers EBUSY for a named port and keeps its single event.
    const uint32_t tiny = native_queue(l, 1);
    PCHECK(ps3_lv2_port_connect_local(&l, d, tiny) == PS3_LV2_EISCONN);   // d is already connected to q: the pinned answer
    PCHECK(ps3_lv2_port_disconnect(&l, d) == PS3_LV2_OK && ps3_lv2_port_connect_local(&l, d, tiny) == PS3_LV2_OK);
    PCHECK(ps3_lv2_port_send(&l, d, 1, 1, 1) == PS3_LV2_OK && ps3_lv2_port_send(&l, d, 2, 2, 2) == PS3_LV2_EBUSY);
    PCHECK(queue_snap(l, tiny).count == 1 && take(l, tiny).source == NAME_FULL);
    // Liveness/namespaces: ids are never reused and the three namespaces are disjoint.
    PCHECK(ps3_lv2_port_disconnect(&l, a) == PS3_LV2_OK && ps3_lv2_port_destroy(&l, a) == PS3_LV2_OK && ps3_lv2_port_destroy(&l, a) == PS3_LV2_ESRCH);
    PCHECK(ps3_lv2_port_send(&l, a, 1, 1, 1) == PS3_LV2_ESRCH);
    uint32_t next = 0; PCHECK(ps3_lv2_port_create(&l, &next, PS3_EVENT_PORT_LOCAL, NAME_D0A77) == PS3_LV2_OK && next == PORT_BASE + 5 * STEP);
    uint32_t sem = 0; ps3_lv2_semaphore_attr sa{PS3_SYNC_PRIORITY};
    PCHECK(ps3_lv2_semaphore_create(&l, &sem, &sa, 0, 1) == PS3_LV2_OK && sem == 0x96000000u && sem != q && sem != next);
    PCHECK(ps3_lv2_port_connect_local(&l, q, next) == PS3_LV2_ESRCH);   // a queue id is not a port id and vice versa
    // Capacity: composition-controlled exhaustion answers EAGAIN, leaves the output alone and consumes no serial.
    ps3_lv2_objects_limit(&l, PS3_LV2_KIND_PORT, uint32_t(ps3_lv2_live_count(&l, PS3_LV2_KIND_PORT)));
    uint32_t untouched = 0x77, serial = l.serial[PS3_LV2_KIND_PORT];
    PCHECK(ps3_lv2_port_create(&l, &untouched, PS3_EVENT_PORT_LOCAL, NAME_D0A77) == PS3_LV2_EAGAIN && untouched == 0x77 && l.serial[PS3_LV2_KIND_PORT] == serial);
}

// ---------------------------------------------------------------- sc134
void case_create_success(uint64_t type_reg, uint64_t name, uint32_t out) {
    Rig r; const State before = r.state(); const uint32_t expected = PORT_BASE + STEP * r.lv2().serial[PS3_LV2_KIND_PORT];
    r.rt->put32(out, 0xDEADBEEFu); r.rt->put32(OUT + 0x20, 0x01020304u);
    r.call(134, out, type_reg, name);
    r.expect_return(0, __LINE__);
    PCHECK(r.rt->get32(out) == expected && r.rt->get32(OUT + 0x20) == 0x01020304u);
    const ps3_lv2_port p = port_snap(r.lv2(), expected);
    PCHECK(p.id == expected && p.type == PS3_EVENT_PORT_LOCAL && p.name == name && p.queue == 0 && p.active);
    auto &c = r.ctl();
    PCHECK(c.event_port_owned_count == 1 && c.event_port_owned_ids[0] == expected && c.event_port_created_id == expected && c.event_port_retained_id == 0);
    PCHECK(c.event_port_create_calls == 1 && c.event_port_create_returns == 1 && c.event_port_last_name == name && c.event_port_last_type == 1);
    PCHECK(r.state().published == before.published + 1 && r.state().ports == before.ports + 1 && r.state().port_serial == before.port_serial + 1);
    PCHECK(r.state().queues == before.queues && r.state().led_queues == 0);
    r.expect_write_event(r.published0, out, expected, __LINE__);   // exactly one HLE BE u32 id store
    PCHECK(c.stop == StartupStop::None);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);   // normal shutdown destroys exactly the owned port
    PCHECK(c.event_port_owned_count == 0 && !raw_port(r.lv2(), expected));
}
void case_create_guest_error(uint64_t type_reg, uint32_t cell) {
    Rig r; const State before = r.state();
    r.call(134, OUT, type_reg, NAME_D0A77);
    r.expect_return(sx(cell), __LINE__);
    PCHECK(r.state() == before && r.ctl().event_port_owned_count == 0 && r.ctl().event_port_create_returns == 0);
}
void case_create_exhausted() {
    Rig r; ps3_lv2_objects_limit(&r.lv2(), PS3_LV2_KIND_PORT, 0);
    const State before = r.state();
    r.call(134, OUT, 1, NAME_D0A77);
    r.expect_return(sx(PS3_LV2_EAGAIN), __LINE__);
    PCHECK(r.state() == before);
    ps3_lv2_objects_limit(&r.lv2(), PS3_LV2_KIND_PORT, PS3_LV2_PORT_CAP);
    PCHECK(r.guest_port(NAME_D0A77) == PORT_BASE);   // the refused call consumed no serial
}
void case_create_stop(uint64_t out, uint64_t type, uint64_t name, const std::function<void(Rig &)> &pre, const std::function<void(Rig &)> &post,
                      StartupStop expected, int line) {
    Rig r; if (pre) pre(r);
    const State before = r.state();
    r.call(134, out, type, name);
    r.expect_stop(expected, line);
    PCHECK(r.state() == before);   // nothing allocated, nothing stored, nothing traced, no ledger entry, no serial
    PCHECK(r.ctl().event_port_owned_count == before.led_ports && r.ctl().event_port_created_id == 0);
    if (post) post(r);
}
void case_create_budget_exhaustion() {   // the HLE budget stops the call BEFORE any effect even when the call itself is a valid create
    unsigned admitted = 0;
    {   // phase 1: measure how many harmless (invalid-type, effect-free) syscalls this budget admits
        Rig r; bool stopped = false;
        for (; admitted < 300 && !stopped; ++admitted) { r.call(134, OUT, 2, NAME_D0A77); stopped = !r.returned; }
        PCHECK(stopped && admitted > 8);
        --admitted;   // the last iteration was the stopped call
    }
    Rig r;
    for (unsigned i = 0; i < admitted; ++i) { r.call(134, OUT, 2, NAME_D0A77); PCHECK(r.returned); }
    const State before = r.state(); const size_t calls0 = r.ctl().event_port_create_calls;
    r.call(134, OUT, 1, NAME_D0A77);   // valid input, but the budget is exhausted
    r.expect_stop(StartupStop::HleSyscallBudget, __LINE__);
    PCHECK(r.state() == before && r.ctl().event_port_owned_count == 0 && r.ctl().event_port_create_calls == calls0);
}
void case_create_post_effect_cancel() {   // cancel observed right after the real create: the commit stays owned, r3 is suppressed
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t expected = PORT_BASE;
    own.hook.predicate = [&] { return raw_port(own.lv2, expected) != nullptr; };
    own.hook.action = [&] { r.ctl().cancelled.store(true); };
    r.rt->put32(OUT, 0);
    r.call(134, OUT, 1, NAME_D0A77);
    r.expect_stop(StartupStop::Cancelled, __LINE__);
    PCHECK(own.hook.fired && raw_port(own.lv2, expected) && r.rt->get32(OUT) == expected);
    PCHECK(r.ctl().event_port_owned_count == 1 && r.ctl().event_port_owned_ids[0] == expected && r.ctl().event_port_created_id == expected);
    PCHECK(r.rt->recorder.report().published_count == 1);
    r.expect_write_event(0, OUT, expected, __LINE__);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK && !raw_port(own.lv2, expected));
}
void case_create_store_failure_rolls_back() {   // the id store faults AFTER the real create: rolled back, serial consumed, nothing leaks
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    own.hook.predicate = [&] { return raw_port(own.lv2, PORT_BASE) != nullptr; };
    own.hook.action = [&] { r.decommit(OUT_PAGE1); };
    r.call(134, OUT_PAGE1, 1, NAME_D0A77);
    r.expect_stop(StartupStop::ProviderFailure, __LINE__);
    PCHECK(own.hook.fired && !raw_port(own.lv2, PORT_BASE) && r.ctl().event_port_owned_count == 0 && r.ctl().event_port_retained_id == 0);
    PCHECK(r.rt->recorder.report().published_count == 0 && own.lv2.serial[PS3_LV2_KIND_PORT] == 1);
    uint32_t next = 0; PCHECK(ps3_lv2_port_create(&own.lv2, &next, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK && next == PORT_BASE + STEP);   // never reused
}
void case_create_rollback_refused_retains() {   // the rollback itself is refused: ownership is retained, recorded and later released
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t foreign_queue = native_queue(own.lv2);
    own.hook.predicate = [&] { return raw_port(own.lv2, PORT_BASE) != nullptr; };
    own.hook.action = [&] { r.decommit(OUT_PAGE1); PCHECK(ps3_lv2_port_connect_local(&own.lv2, PORT_BASE, foreign_queue) == PS3_LV2_OK); };
    r.call(134, OUT_PAGE1, 1, NAME_FULL);
    r.expect_stop(StartupStop::ProviderFailure, __LINE__);
    PCHECK(own.hook.fired && raw_port(own.lv2, PORT_BASE) && r.ctl().event_port_retained_id == PORT_BASE);
    PCHECK(r.ctl().event_port_owned_count == 1 && r.ctl().event_port_owned_ids[0] == PORT_BASE && r.rt->recorder.report().published_count == 0);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);   // EISCONN -> disconnect -> destroy
    PCHECK(!raw_port(own.lv2, PORT_BASE) && raw_queue(own.lv2, foreign_queue) && r.ctl().event_port_owned_count == 0);
}

// ---------------------------------------------------------------- sc136
void case_connect_success() {
    Rig r; const uint32_t q = r.guest_queue(), p = r.guest_port(NAME_D0A77, OUT + 0x10);
    PCHECK(q == QUEUE_BASE + STEP * (r.lv2().serial[PS3_LV2_KIND_QUEUE] - 1) && p == PORT_BASE + STEP * (r.lv2().serial[PS3_LV2_KIND_PORT] - 1));
    const State before = r.state(); const ps3_lv2_queue q0 = queue_snap(r.lv2(), q);
    r.call(136, HIGH | p, HIGH | q, 0);   // ids are the LOW 32 bits of r3/r4
    r.expect_return(0, __LINE__);
    PCHECK(port_snap(r.lv2(), p).queue == q && port_snap(r.lv2(), p).name == NAME_D0A77);
    const ps3_lv2_queue q1 = queue_snap(r.lv2(), q);   // a real connect only: no filter bound, nothing enqueued, no wake, no receiver
    PCHECK(!q1.service_bound && q1.count == 0 && q1.receivers == 0 && q1.blocked_receivers == 0 && q1.sequence == q0.sequence && q1.head == q0.head);
    PCHECK(r.state() == before);   // no trace, no ledger or serial change, no guest memory change
    auto &c = r.ctl();
    PCHECK(c.event_port_connect_calls == 1 && c.event_port_connect_returns == 1 && c.event_port_connect_port == p && c.event_port_connect_queue == q && c.event_port_connect_cell == 0);
    r.call(136, p, q, 0);   // second connect: the pinned EISCONN, first connection intact
    r.expect_return(sx(PS3_LV2_EISCONN), __LINE__);
    PCHECK(port_snap(r.lv2(), p).queue == q && c.event_port_connect_calls == 2 && c.event_port_connect_cell == PS3_LV2_EISCONN);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);   // EISCONN -> disconnect -> destroy, then the queue
    PCHECK(!raw_port(r.lv2(), p) && !raw_queue(r.lv2(), q) && c.event_port_owned_count == 0 && c.event_queue_owned_count == 0);
}
void case_connect_errors() {
    Rig r; const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
    const State before = r.state();
    struct { uint64_t port, queue; } rows[] = {{PORT_BASE + 0x900, q}, {p, QUEUE_BASE + 0x900}, {PORT_BASE + 0x900, QUEUE_BASE + 0x900}, {0, 0}, {p, 0}, {0, q}};
    for (const auto &row : rows) {
        r.call(136, row.port, row.queue, 0);
        r.expect_return(sx(PS3_LV2_ESRCH), __LINE__);
        PCHECK(port_snap(r.lv2(), p).queue == 0 && r.state() == before);
    }
    const uint32_t foreign_queue = native_queue(r.lv2());   // a missing port wins over a valid foreign queue: ESRCH, not a stop
    r.call(136, PORT_BASE + 0x900, foreign_queue, 0);
    r.expect_return(sx(PS3_LV2_ESRCH), __LINE__);
    PCHECK(r.ctl().event_port_connect_calls == 7 && r.ctl().event_port_connect_returns == 7);
}
void case_connect_future_id_race(bool missing_port) {
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
    const uint32_t future = missing_port
        ? PORT_BASE + STEP * own.lv2.serial[PS3_LV2_KIND_PORT]
        : QUEUE_BASE + STEP * own.lv2.serial[PS3_LV2_KIND_QUEUE];
    uint32_t foreign = 0;
    unsigned unlocks = 0;
    // Allocate the guessed id just after the registry has reported that it is absent.
    own.hook.predicate = [&] { return ++unlocks == (missing_port ? 1u : 2u); };
    own.hook.action = [&] { foreign = missing_port ? native_port(own.lv2) : native_queue(own.lv2); };
    r.call(136, missing_port ? future : p, missing_port ? q : future, 0);
    r.expect_return(sx(PS3_LV2_ESRCH), __LINE__);
    PCHECK(own.hook.fired && foreign == future);
    PCHECK(port_snap(own.lv2, p).queue == 0);
    if (missing_port) PCHECK(port_snap(own.lv2, foreign).queue == 0);
    else PCHECK(queue_snap(own.lv2, foreign).count == 0 && queue_snap(own.lv2, foreign).sequence == 0);
    PCHECK(r.ctl().event_port_owned_count == 1 && r.ctl().event_queue_owned_count == 1);
    PCHECK(r.ctl().event_port_connect_calls == 1 && r.ctl().event_port_connect_returns == 1);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
    if (missing_port) {
        PCHECK(raw_port(own.lv2, foreign) && port_snap(own.lv2, foreign).queue == 0);
        PCHECK(ps3_lv2_port_destroy(&own.lv2, foreign) == PS3_LV2_OK);
    } else {
        PCHECK(raw_queue(own.lv2, foreign));
        PCHECK(ps3_lv2_queue_destroy(&own.lv2, foreign) == PS3_LV2_OK);
    }
}
void case_connect_foreign(int which) {   // valid but not guest-owned: explicit Unsupported stop BEFORE any effect
    Rig r; const uint32_t own_q = r.guest_queue(), own_p = r.guest_port(NAME_D0A77, OUT + 0x10);
    const uint32_t fq = native_queue(r.lv2()), fp = native_port(r.lv2(), NAME_FULL);
    const State before = r.state();
    const uint32_t p = which == 0 ? own_p : fp, q = which == 1 ? own_q : fq;   // 0: own port+foreign queue, 1: foreign port+own queue, 2: both foreign
    r.call(136, p, q, 0);
    r.expect_stop(StartupStop::UnsupportedSyscall, __LINE__);
    PCHECK(r.state() == before && port_snap(r.lv2(), own_p).queue == 0 && port_snap(r.lv2(), fp).queue == 0 && queue_snap(r.lv2(), fq).sequence == 0);
    PCHECK(r.ctl().event_port_connect_returns == 0);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
    PCHECK(raw_port(r.lv2(), fp) && raw_queue(r.lv2(), fq) && !raw_port(r.lv2(), own_p) && !raw_queue(r.lv2(), own_q));   // foreign resources preserved
}
void case_connect_stop(const std::function<void(Rig &)> &pre, StartupStop expected, bool expect_connected, int line) {
    Rig r; const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
    const State before = r.state();
    if (pre) pre(r);
    r.call(136, p, q, 0);
    r.expect_stop(expected, line);
    PCHECK(r.state() == before);
    r.rt->execution.event_queues = &r.rt->comp->lv2; r.rt->comp->lv2.initialized = 1;
    PCHECK((port_snap(r.lv2(), p).queue == q) == expect_connected);
    r.ctl().cancelled.store(false);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
}
void case_connect_post_effect_cancel() {   // cancel right after the real connect: it stays connected, r3 suppressed
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
    own.hook.predicate = [&] { ps3_lv2_port *x = raw_port(own.lv2, p); return x && x->queue == q; };
    own.hook.action = [&] { r.ctl().cancelled.store(true); };
    const State before = r.state();
    r.call(136, p, q, 0);
    r.expect_stop(StartupStop::Cancelled, __LINE__);
    PCHECK(own.hook.fired && raw_port(own.lv2, p)->queue == q && r.state() == before && r.ctl().event_port_connect_returns == 0);
    r.ctl().cancelled.store(false);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK && !raw_port(own.lv2, p) && !raw_queue(own.lv2, q));
}

// ---------------------------------------------------------------- cleanup
void case_release_unavailable() {
    for (int mode = 0; mode < 3; ++mode) {
        OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
        const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
        r.call(136, p, q, 0); PCHECK(r.returned && r.cpu().gpr[3] == 0);
        StartupControl *control = r.rt->execution.control;
        if (mode == 0) r.rt->execution.event_queues = nullptr;
        if (mode == 1) own.lv2.initialized = 0;
        if (mode == 2) r.rt->execution.control = nullptr;
        const auto released = native_startup_release_after_join(r.rt->execution);
        r.rt->execution.event_queues = &own.lv2; own.lv2.initialized = 1; r.rt->execution.control = control;
        PCHECK(released == PS3_GUEST_MEMORY_BUSY && r.rt->execution.ppu.admitted);
        PCHECK(control->event_port_owned_count == 1 && control->event_port_owned_ids[0] == p && control->event_queue_owned_count == 1);
        PCHECK(raw_port(own.lv2, p) && raw_queue(own.lv2, q) && raw_port(own.lv2, p)->queue == q);   // nothing was touched
        PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK && !raw_port(own.lv2, p) && !raw_queue(own.lv2, q));
        PCHECK(control->event_port_owned_count == 0 && !r.rt->execution.ppu.admitted);
    }
}
void case_release_order_and_foreign() {   // ports are destroyed BEFORE the guest queues; foreign native resources are never touched
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t fq = native_queue(own.lv2), fp = native_port(own.lv2, NAME_FULL);
    PCHECK(ps3_lv2_port_connect_local(&own.lv2, fp, fq) == PS3_LV2_OK);
    const uint32_t q = r.guest_queue(), p = r.guest_port(NAME_D0A77, OUT + 0x10);
    r.call(136, p, q, 0); PCHECK(r.returned && r.cpu().gpr[3] == 0);
    unsigned violations = 0, queue_wakes = 0;
    own.probe.hook = [&] {   // runs under the registry guard: a queue destroy wakes after the queue went inactive
        if (!raw_queue(own.lv2, q)) { ++queue_wakes; if (raw_port(own.lv2, p)) ++violations; }
    };
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
    own.probe.hook = nullptr;
    PCHECK(queue_wakes >= 1 && violations == 0);
    PCHECK(!raw_port(own.lv2, p) && !raw_queue(own.lv2, q) && raw_port(own.lv2, fp) && raw_queue(own.lv2, fq) && raw_port(own.lv2, fp)->queue == fq);
    PCHECK(r.ctl().event_port_owned_count == 0 && r.ctl().event_queue_owned_count == 0 && !r.rt->execution.ppu.admitted);
}
void case_release_retry_remaining_only() {   // an error on a port keeps ITS id, leaves the queues untouched, and a retry releases only what remains
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10), p2 = r.guest_port(NAME_D0A77, OUT + 0x20);
    r.call(136, p, q, 0); PCHECK(r.returned && r.cpu().gpr[3] == 0);
    r.call(136, p2, q, 0); PCHECK(r.returned && r.cpu().gpr[3] == 0);
    own.hook.predicate = [&] { ps3_lv2_port *x = raw_port(own.lv2, p); return x && x->queue != 0; };   // first unlock after the destroy attempt on p
    own.hook.action = [&] { raw_port(own.lv2, p)->queue = 0; };                                          // p was disconnected behind our back: ENOTCONN
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_BUSY && own.hook.fired);
    PCHECK(r.ctl().event_port_owned_count == 1 && r.ctl().event_port_owned_ids[0] == p && raw_port(own.lv2, p) && !raw_port(own.lv2, p2));
    PCHECK(r.ctl().event_queue_owned_count == 1 && raw_queue(own.lv2, q) && r.rt->execution.ppu.admitted);   // queues untouched while a port remains
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
    PCHECK(!raw_port(own.lv2, p) && !raw_queue(own.lv2, q) && r.ctl().event_port_owned_count == 0 && !r.rt->execution.ppu.admitted);
}
void case_release_queue_busy_after_ports() {
    OwnLv2 own; Rig r(false); r.rt->execution.event_queues = &own.lv2;
    const uint32_t q = r.guest_queue(), p = r.guest_port(0, OUT + 0x10);
    r.call(136, p, q, 0); PCHECK(r.returned && r.cpu().gpr[3] == 0);
    raw_queue(own.lv2, q)->receivers = 1;   // a registered receiver: the queue cannot be destroyed yet
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_BUSY);
    PCHECK(!raw_port(own.lv2, p) && r.ctl().event_port_owned_count == 0 && raw_queue(own.lv2, q) && r.ctl().event_queue_owned_count == 1 && r.rt->execution.ppu.admitted);
    raw_queue(own.lv2, q)->receivers = 0;
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK && !raw_queue(own.lv2, q));
}

// ---------------------------------------------------------------- fences
void case_unsupported_neighbors() {   // no guest send / destroy / disconnect is enabled and a high-bit number is not 134/136
    for (uint64_t number : {UINT64_C(133), UINT64_C(135), UINT64_C(137), UINT64_C(138), UINT64_C(129), UINT64_C(0x100000086), UINT64_C(0x100000088)}) {
        OwnLv2 own; Rig r(false, false);
        r.rt->services.allocate_memory = [](void *, uint64_t, uint64_t, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        r.rt->services.free_memory = [](void *, uint32_t) -> ps3_user_memory_service_result { std::abort(); };
        r.rt->services.query_memory = [](void *, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        PCHECK(native_startup_admit_budgeted(r.rt->execution, *r.rt->binding, r.rt->services, r.rt->recorder, r.rt->budget));
        r.rt->execution.event_queues = &own.lv2;
        const uint32_t q = native_queue(own.lv2), p = native_port(own.lv2, NAME_D0A77);
        PCHECK(ps3_lv2_port_connect_local(&own.lv2, p, q) == PS3_LV2_OK);
        const ps3_lv2_queue q0 = queue_snap(own.lv2, q);
        r.call(number, p, 1, 2, 3);
        r.expect_stop(StartupStop::UnsupportedSyscall, __LINE__);
        const ps3_lv2_queue q1 = queue_snap(own.lv2, q);
        PCHECK(!std::memcmp(&q0, &q1, sizeof q0) && raw_port(own.lv2, p) && raw_port(own.lv2, p)->queue == q && own.probe.wakes == 0);
    }
}

// ---------------------------------------------------------------- real composition + really blocked receiver
void case_spurs_flow() {
    Rig r;
    r.rt->spu_initialize(6, 0); r.rt->attribute(5, 128, 249);
    r.rt->call_initialize(INSTANCE1);
    PCHECK(r.ctl().stop == StartupStop::None && r.cpu().gpr[3] == 0);
    auto &l = r.lv2();
    const size_t spurs_ports = ps3_lv2_live_count(&l, PS3_LV2_KIND_PORT), spurs_queues = ps3_lv2_live_count(&l, PS3_LV2_KIND_QUEUE);
    PCHECK(spurs_ports >= 1 && spurs_queues >= 1);
    const uint32_t port_serial = l.serial[PS3_LV2_KIND_PORT], queue_serial = l.serial[PS3_LV2_KIND_QUEUE];
    uint32_t spurs_queue = 0;
    for (auto &x : l.queues) if (x.active) spurs_queue = x.id;
    const uint32_t q = r.guest_queue(), p = r.guest_port(NAME_D0A77, OUT + 0x10), p2 = r.guest_port(0, OUT + 0x20);
    PCHECK(q == QUEUE_BASE + STEP * queue_serial && p == PORT_BASE + STEP * port_serial && p2 == p + STEP);   // same namespaces as SPURS, next serial
    r.call(136, p, q, 0); r.expect_return(0, __LINE__);
    PCHECK(port_snap(l, p).queue == q && ps3_lv2_live_count(&l, PS3_LV2_KIND_PORT) == spurs_ports + 2);
    r.call(136, p2, spurs_queue, 0);   // a SPURS-owned queue is foreign
    r.expect_stop(StartupStop::UnsupportedSyscall, __LINE__);
    PCHECK(port_snap(l, p2).queue == 0 && raw_queue(l, spurs_queue) && r.ctl().event_port_owned_count == 2);
    PCHECK(native_startup_release_after_join(r.rt->execution) == PS3_GUEST_MEMORY_OK);
    PCHECK(r.ctl().event_port_owned_count == 0 && r.ctl().event_queue_owned_count == 0 && !r.rt->execution.ppu.admitted);
}

constexpr uint64_t SERVICE_STACK = 0x10000;
struct BlockPort {
    ps3_lv2_objects *lv2 = nullptr;
    ps3_ppu_service *service = nullptr;
    ps3_notify_cancel cancel{};
    std::atomic<unsigned> calls{0};
    ps3_lv2_event event{};
    uint32_t last_rc = 0xFFFFFFFFu;
    StartupEventReceivePort port{this, &BlockPort::receive};
    static StartupEventReceiveResult receive(void *context, uint32_t queue, uint64_t timeout) {
        BlockPort &p = *static_cast<BlockPort *>(context);
        ++p.calls;
        StartupEventReceiveResult out{};
        if (timeout) { out.outcome = StartupEventReceiveOutcome::Unsupported; return out; }
        p.last_rc = ps3_lv2_queue_receive(p.lv2, queue, &p.event, &p.cancel, p.service ? &p.service->activity : nullptr);
        if (p.last_rc == PS3_LV2_CANCELLED) { out.outcome = StartupEventReceiveOutcome::Cancelled; return out; }
        if (p.last_rc != PS3_LV2_OK) { out.outcome = StartupEventReceiveOutcome::ProviderFailure; return out; }
        out.outcome = StartupEventReceiveOutcome::Completed; out.result = 0;
        out.source = p.event.source; out.data1 = p.event.data1; out.data2 = p.event.data2; out.data3 = p.event.data3;
        return out;
    }
};
// A real PPU service thread whose body issues the real sc130 against a queue owned by ANOTHER execution's guest ledger.
struct Blocker {
    std::unique_ptr<Runtime> rt{new Runtime(1)};
    BlockPort bp; Obs obs; WakeProbe probe;
    uint32_t queue = 0;
    ps3_pthread_service_thread thread{};
    ps3_ppu_ref ref{};
    bool reached = false, caught = false;
    ppu_context seeded{};
    Blocker() { probe.attach(rt->comp->lv2); }
    ps3_lv2_objects &lv2() { return rt->comp->lv2; }
    static void body(ps3_ppu_service *self, void *argument) {
        Blocker &b = *static_cast<Blocker *>(argument);
        b.bp.service = self; b.obs.service = self; b.obs.host = pthread_self();
        auto &cpu = b.rt->execution.ppu.cpu;
        cpu.gpr[11] = 130; cpu.gpr[3] = HIGH | b.queue; cpu.gpr[4] = STACK + 0x70; cpu.gpr[5] = 0; cpu.gpr[6] = R6; cpu.gpr[7] = R7; cpu.gpr[8] = R8;
        b.seeded = cpu;
        b.rt->control->stop = StartupStop::None;
        active = b.rt->control.get(); b.rt->control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
        try { lv2_syscall(&cpu); b.reached = true; active = nullptr; } catch (const Terminal &) { b.caught = true; }
        if (b.reached) { ps3_ppu_service_exit(self, 0xC0DEu); return; }
        pthread_exit(nullptr);
    }
    void start(uint32_t q) {
        queue = q; bp.lv2 = &lv2(); obs.rt = rt.get();
        rt->execution.event_receive = &bp.port;
        rt->control->terminal_notification = {on_terminal, &obs};
        arm("blocker-create", 10000);
        PCHECK(ps3_ppu_service_create(&rt->comp->ppu, SERVICE_KIND, SERVICE_PRIORITY, CREATOR_PRIORITY, SERVICE_STACK,
            ps3_pthread_service_thread_port(&thread), body, this, &ref) == PS3_PPU_OK);
        disarm();
    }
    bool blocked() { return ps3_ppu_service_is_blocked(&rt->comp->ppu, ref) != 0; }
    void expect_really_blocked(int line) {
        ps3_lv2_queue snap{};
        const bool ok = blocked() && ps3_lv2_queue_snapshot(&lv2(), queue, &snap) == PS3_LV2_OK && snap.receivers == 1 && snap.blocked_receivers == 1 &&
            snap.count == 0 && obs.calls == 0 && !reached && !caught && bp.calls == 1;
        if (!ok) { std::printf("EVENT_PORTS_FAIL line=%d receiver is not really blocked\n", line); ep_failures.fetch_add(1); }
    }
    void cancel_and_join() {
        ps3_notify_cancel_request(&bp.cancel, &lv2().port);
        uint32_t code = 0xFFFFFFFFu;
        arm("blocker-join", 10000);
        PCHECK(ps3_ppu_service_join(&rt->comp->ppu, ref, &code) == PS3_PPU_OK && code == EXIT_PUBLISHED);
        disarm();
        PCHECK(!reached && caught && obs.calls == 1 && obs.stop == StartupStop::Cancelled && bp.last_rc == PS3_LV2_CANCELLED);
    }
};
void case_blocked_receiver_stays_blocked() {
    Blocker a;                                       // owns the registry and the really blocked receiver
    Rig b(false); b.rt->execution.event_queues = &a.lv2();   // the creator execution (declared after: released before the registry dies)
    const uint32_t q = b.guest_queue();              // sc128 into the shared registry: owned by b's ledger
    a.start(q);
    a.expect_really_blocked(__LINE__);
    const uint32_t p = b.guest_port(NAME_D0A77, OUT + 0x10);
    const ps3_lv2_queue q0 = queue_snap(a.lv2(), q);
    const unsigned wakes0 = a.probe.wakes;
    b.call(136, p, q, 0);                            // real connect: no filter bind, no enqueue, no wake
    b.expect_return(0, __LINE__);
    PCHECK(port_snap(a.lv2(), p).queue == q);
    const ps3_lv2_queue q1 = queue_snap(a.lv2(), q);
    PCHECK(q1.sequence == q0.sequence && q1.count == 0 && !q1.service_bound && q1.receivers == 1 && q1.blocked_receivers == 1 && a.probe.wakes == wakes0);
    a.expect_really_blocked(__LINE__);               // STILL blocked through the connect
    // Cleanup while the receiver lives: the port is released first, the queue is BUSY and retained, the receiver is undisturbed.
    PCHECK(native_startup_release_after_join(b.rt->execution) == PS3_GUEST_MEMORY_BUSY);
    PCHECK(!raw_port(a.lv2(), p) && b.ctl().event_port_owned_count == 0 && b.ctl().event_queue_owned_count == 1 && b.rt->execution.ppu.admitted);
    a.expect_really_blocked(__LINE__);
    // Cancellation and join first; only then can the retained queue be released.
    a.cancel_and_join();
    PCHECK(queue_snap(a.lv2(), q).receivers == 0 && queue_snap(a.lv2(), q).blocked_receivers == 0);
    PCHECK(native_startup_release_after_join(b.rt->execution) == PS3_GUEST_MEMORY_OK && !raw_queue(a.lv2(), q) && b.ctl().event_queue_owned_count == 0);
}
}  // namespace

int main() {
    std::thread(watchdog_main).detach();
    struct Step { const char *name; std::function<void()> fn; };
    const auto stop_cases = [] {
        const uint64_t out = OUT;
        case_create_stop(0, 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);                                  // null output
        case_create_stop(out | UINT64_C(0x100000000), 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);       // not canonical
        case_create_stop(UINT64_C(0xFFFFFFFFFFFF0000), 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);
        case_create_stop(out + 1, 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);                            // unaligned
        case_create_stop(out + 2, 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);
        case_create_stop(UINT32_MAX - 3, 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);                     // end of address space
        case_create_stop(0x20000, 1, 0, nullptr, nullptr, StartupStop::InvalidMemory, __LINE__);                            // outside RAM
        case_create_stop(OUT_PAGE2, 1, 0, [](Rig &r) { r.decommit(OUT_PAGE2); }, nullptr, StartupStop::InvalidMemory, __LINE__);   // unmapped
        case_create_stop(out, 1, 0, [](Rig &r) { r.rt->ranges[1].writable = false; }, nullptr, StartupStop::InvalidMemory, __LINE__);   // read-only
        case_create_stop(out, 3, NAME_D0A77, nullptr, nullptr, StartupStop::UnsupportedSyscall, __LINE__);                  // IPC port: legal lv2, not owned
        case_create_stop(out, UINT64_C(0x100000003), 0, nullptr, nullptr, StartupStop::UnsupportedSyscall, __LINE__);
        case_create_stop(out, 1, NAME_D0A77, [](Rig &r) { r.rt->execution.event_queues = nullptr; }, [](Rig &r) { r.rt->execution.event_queues = &r.rt->comp->lv2; }, StartupStop::MissingServices, __LINE__);
        case_create_stop(out, 1, NAME_D0A77, [](Rig &r) { r.lv2().initialized = 0; }, [](Rig &r) { r.lv2().initialized = 1; }, StartupStop::MissingServices, __LINE__);
        case_create_stop(out, 1, NAME_D0A77, [](Rig &r) { r.ctl().cancelled.store(true); }, nullptr, StartupStop::Cancelled, __LINE__);
        case_create_stop(out, 1, NAME_D0A77, [](Rig &r) { r.fill_trace(); }, nullptr, StartupStop::TraceCapacity, __LINE__);
        case_create_stop(out, 1, NAME_D0A77, [](Rig &r) { r.ctl().event_port_owned_count = PS3_LV2_PORT_CAP; },
            [](Rig &r) { r.ctl().event_port_owned_count = 0; }, StartupStop::ProviderFailure, __LINE__);                    // ledger full: checked BEFORE the allocation
    };
    const Step steps[] = {
        {"domain_names", case_domain_names},
        {"create_observed_call", [] { case_create_success(1, NAME_D0A77, OUT); }},
        {"create_no_name", [] { case_create_success(1, 0, OUT); }},
        {"create_name_64bit", [] { case_create_success(1, NAME_FULL, OUT); case_create_success(1, NAME_HIGH, OUT + 8); }},
        {"create_type_high_bits", [] { case_create_success(UINT64_C(0xABCDEF0100000001), 5, OUT + 4); }},
        {"create_guest_errors", [] {
            for (uint64_t type : {UINT64_C(0), UINT64_C(2), UINT64_C(4), UINT64_C(0x80000001), UINT64_C(0xFFFFFFFF), UINT64_C(0x100000002)})
                case_create_guest_error(type, PS3_LV2_EINVAL); }},
        {"create_exhausted", case_create_exhausted},
        {"create_stops", stop_cases},
        {"create_budget", case_create_budget_exhaustion},
        {"create_post_effect_cancel", case_create_post_effect_cancel},
        {"create_store_failure", case_create_store_failure_rolls_back},
        {"create_rollback_refused", case_create_rollback_refused_retains},
        {"connect_success", case_connect_success},
        {"connect_errors", case_connect_errors},
        {"connect_future_id_race", [] { case_connect_future_id_race(false); case_connect_future_id_race(true); }},
        {"connect_foreign", [] { for (int which = 0; which < 3; ++which) case_connect_foreign(which); }},
        {"connect_stops", [] {
            case_connect_stop([](Rig &r) { r.ctl().cancelled.store(true); }, StartupStop::Cancelled, false, __LINE__);
            case_connect_stop([](Rig &r) { r.rt->execution.event_queues = nullptr; }, StartupStop::MissingServices, false, __LINE__);
            case_connect_stop([](Rig &r) { r.lv2().initialized = 0; r.rt->execution.event_queues = &r.lv2(); }, StartupStop::MissingServices, false, __LINE__); }},
        {"connect_post_effect_cancel", case_connect_post_effect_cancel},
        {"release_unavailable", case_release_unavailable},
        {"release_order_foreign", case_release_order_and_foreign},
        {"release_retry", case_release_retry_remaining_only},
        {"release_queue_busy", case_release_queue_busy_after_ports},
        {"unsupported_neighbors", case_unsupported_neighbors},
        {"spurs_flow", case_spurs_flow},
        {"blocked_receiver", case_blocked_receiver_stays_blocked}};
    for (const Step &s : steps) {
        const int before = ep_failures.load() + failures;
        arm(s.name, 60000);
        s.fn();
        disarm();
        std::printf("case %-28s %s\n", s.name, ep_failures.load() + failures == before ? "ok" : "FAILED");
    }
    const int total = ep_failures.load() + failures;
    std::printf("%s failures=%d (HOST test; not game execution)\n", total ? "STARTUP_EVENT_PORTS_FAIL" : "STARTUP_EVENT_PORTS_OK", total);
    return total ? 1 : 0;
}
