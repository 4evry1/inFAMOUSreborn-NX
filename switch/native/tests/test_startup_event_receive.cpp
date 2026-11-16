/* Host behavior test for the generic borrowed guest sys_event_queue_receive (sc130) port, StartupExecution::event_receive.
 * Real collaborators: the actual startup_execution.cpp lv2_syscall dispatcher and stop()/terminal-notification path, the
 * actual composed ps3_lv2_objects queue registry (real ps3_lv2_queue_receive / port_send, real pthread wait port, real
 * cancel token), the actual PPU service registry with the real pthread service-thread adapter, real shared guest memory and
 * the real recorder/budget. The test-side receive callback (RealPort) is the policy the future native provider would
 * implement: pre/post-check a persistent cancel token around the real queue primitive. The guest is a LABEL FIXTURE: a host
 * worker seeds PPU registers and issues one real syscall 130. This is HOST test evidence, not game execution, not a secondary
 * guest thread and not the real compiled helper. Expected values are derived by hand from the contract, never captured.
 * The inherited SPURS fixture (Runtime, host threadExit shim that throws Terminal, tracked guards) is reused unchanged. */
#define main inherited_spurs_main
#include "test_startup_spurs_initialize.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<int> receive_failures{0};
#define RCHECK(value) do { if (!(value)) { std::printf("EVENT_RECEIVE_FAIL line=%d expression=%s\n", __LINE__, #value); \
    std::fflush(stdout); receive_failures.fetch_add(1); } } while (0)

// Hard watchdog: a lane that does not finish is a FAILURE (exit 124), never a pass or a skip.
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
            std::printf("EVENT_RECEIVE_WATCHDOG_TIMEOUT phase=%s\n", watchdog_phase.load());
            std::fflush(stdout);
            _exit(124);
        }
    }
}

constexpr uint32_t SERVICE_KIND = 9, SERVICE_PRIORITY = 100, CREATOR_PRIORITY = 250, EXIT_PUBLISHED = 0x7E57u, EXIT_COMPLETED = 0xC0DEu;
constexpr uint64_t SERVICE_STACK = 0x10000;
constexpr uint32_t DUMMY_MAPPED = STACK + 0x70, DUMMY_UNMAPPED = 0xDEAD0000u, SENTINEL = 0xCAFEF00Du;
constexpr uint64_t R6 = UINT64_C(0x6666666666666666), R7 = UINT64_C(0x7777777777777777), R8 = UINT64_C(0x8888888888888888);
constexpr uint64_t HIGH = UINT64_C(0xABCDEF0100000000);   // r3 high word is garbage: queue id is the LOW 32 bits only
constexpr uint64_t D1 = UINT64_C(0x1111222233334444), D2 = UINT64_C(0x55556666FFFF8888), D3 = UINT64_C(0x99AABBCCDDEEFF00);
void idle_trampoline(void *) {}

// Everything the observer saw on the exiting host thread (written only by that thread, read after join).
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

// Test-side callback backed by the REAL lv2 receive. Persistent cancel token is pre- and post-checked: the real primitive
// dequeues BEFORE it looks at cancel, so a consumed event under an observed cancel is discarded and never reaches the guest.
struct RealPort {
    ps3_lv2_objects *lv2 = nullptr;
    ps3_ppu_service *service = nullptr;
    StartupControl *control = nullptr;
    ps3_notify_cancel cancel{};
    void (*after_dequeue)(RealPort &) = nullptr;   // test-only race injection point (between dequeue and post-check)
    std::atomic<unsigned> calls{0};
    uint32_t last_queue = 0, last_rc = 0xFFFFFFFFu;
    uint64_t last_timeout = 0xFFFFFFFFFFFFFFFFull;
    ps3_lv2_event event{};
    pthread_t host{};
    bool active_ok = false, no_guards = false, same_thread = false, pre_cancel = false, post_cancel = false;
    StartupEventReceivePort port{this, &RealPort::receive};
    static StartupEventReceiveResult receive(void *context, uint32_t queue, uint64_t timeout) {
        RealPort &p = *static_cast<RealPort *>(context);
        ++p.calls;
        p.last_queue = queue; p.last_timeout = timeout;
        p.active_ok = active == p.control && active->cpu != nullptr;
        p.no_guards = thread_holds_no_tracked_guard();
        p.same_thread = pthread_equal(pthread_self(), p.host) != 0;
        StartupEventReceiveResult out{};
        if (timeout) { out.outcome = StartupEventReceiveOutcome::Unsupported; return out; }
        if (ps3_notify_cancel_is_requested(&p.cancel)) { p.pre_cancel = true; out.outcome = StartupEventReceiveOutcome::Cancelled; return out; }
        p.last_rc = ps3_lv2_queue_receive(p.lv2, queue, &p.event, &p.cancel, p.service ? &p.service->activity : nullptr);
        if (p.last_rc == PS3_LV2_CANCELLED) { out.outcome = StartupEventReceiveOutcome::Cancelled; return out; }
        if (p.last_rc != PS3_LV2_OK) { out.outcome = StartupEventReceiveOutcome::ProviderFailure; return out; }
        if (p.after_dequeue) p.after_dequeue(p);
        if (ps3_notify_cancel_is_requested(&p.cancel)) { p.post_cancel = true; out.outcome = StartupEventReceiveOutcome::Cancelled; return out; }
        out.outcome = StartupEventReceiveOutcome::Completed; out.result = 0;
        out.source = p.event.source; out.data1 = p.event.data1; out.data2 = p.event.data2; out.data3 = p.event.data3;
        return out;
    }
};
// Generic stub for dispatcher-only cases the real queue cannot produce (guest-error completion, mid-callback execution cancel).
struct StubPort {
    StartupEventReceiveResult reply{};
    void (*hook)(StubPort &) = nullptr;
    Runtime *rt = nullptr;
    unsigned calls = 0;
    uint32_t queue = 0;
    uint64_t timeout = 0;
    ppu_context at_call{};
    bool active_ok = false, batch_closed = false, no_guards = false;
    StartupEventReceivePort port{this, &StubPort::receive};
    static StartupEventReceiveResult receive(void *context, uint32_t queue, uint64_t timeout) {
        StubPort &p = *static_cast<StubPort *>(context);
        ++p.calls; p.queue = queue; p.timeout = timeout; p.at_call = *active->cpu;
        p.active_ok = active == p.rt->control.get();
        p.batch_closed = !p.rt->recorder.report().batch_active;
        p.no_guards = thread_holds_no_tracked_guard();
        if (p.hook) p.hook(p);
        return p.reply;
    }
};

void seed(ppu_context &cpu, uint64_t queue, uint64_t dummy, uint64_t timeout) {
    cpu.gpr[11] = 130; cpu.gpr[3] = queue; cpu.gpr[4] = dummy; cpu.gpr[5] = timeout; cpu.gpr[6] = R6; cpu.gpr[7] = R7; cpu.gpr[8] = R8;
}
bool regs_seeded(const ppu_context &c, const ppu_context &s) {
    for (int i : {1, 2, 3, 4, 5, 6, 7, 8, 11}) if (c.gpr[i] != s.gpr[i]) return false;
    return c.thread_id == s.thread_id && c.lr == s.lr;
}

// One real sc130 through the real dispatcher on THIS thread. A terminal stop is observed through the notification.
struct Direct {
    std::unique_ptr<Runtime> rt{new Runtime(1)};
    Obs obs;
    ppu_context seeded{};
    bool returned = false;
    uint64_t published0 = 0;
    Direct() { obs.rt = rt.get(); obs.host = pthread_self(); rt->control->terminal_notification = {on_terminal, &obs}; }
    void call(uint64_t queue, uint64_t dummy, uint64_t timeout) {
        auto &cpu = rt->execution.ppu.cpu;
        seed(cpu, queue, dummy, timeout);
        seeded = cpu;
        published0 = rt->recorder.report().published_count;
        rt->control->stop = StartupStop::None;
        active = rt->control.get(); rt->control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
        try { lv2_syscall(&cpu); returned = true; active = nullptr; } catch (const Terminal &) { returned = false; }
        g_trampoline_fn = nullptr;
    }
    void expect_stop(StartupStop expected, unsigned line) {
        if (returned || obs.calls != 1 || obs.stop != expected || rt->control->stop != expected || !regs_seeded(obs.terminal, seeded) ||
            !regs_seeded(rt->execution.ppu.cpu, seeded) || !obs.active_cleared || !obs.trampoline_cleared || !obs.batch_closed ||
            !obs.same_thread || active != nullptr) {
            std::printf("EVENT_RECEIVE_FAIL line=%u expected_stop=%d got_stop=%d returned=%d calls=%u\n", line, int(expected), int(obs.stop), int(returned), obs.calls);
            receive_failures.fetch_add(1);
        }
        RCHECK(rt->recorder.report().published_count == published0 && !rt->recorder.report().batch_active);   // register-only: no trace
    }
    void expect_returned(unsigned line) {
        if (!returned || obs.calls != 0 || rt->control->stop != StartupStop::None || active != nullptr) {
            std::printf("EVENT_RECEIVE_FAIL line=%u expected_return returned=%d calls=%u stop=%d\n", line, int(returned), obs.calls, int(rt->control->stop));
            receive_failures.fetch_add(1);
        }
        RCHECK(rt->recorder.report().published_count == published0 && !rt->recorder.report().batch_active);
        RCHECK(rt->execution.ppu.cpu.gpr[11] == 130 && rt->execution.ppu.cpu.gpr[8] == R8);
    }
};

void case_missing_services() {
    {   // No port at all: explicit change from the old generic UnsupportedSyscall to MissingServices.
        Direct d; RCHECK(d.rt->execution.event_receive == nullptr);
        d.call(HIGH | 7, DUMMY_MAPPED, 0);
        d.expect_stop(StartupStop::MissingServices, __LINE__);
        RCHECK(d.rt->control->event_receive_calls == 0 && d.rt->control->event_receive_returns == 0);
    }
    {   // A port whose receive callback is absent is the same failure.
        Direct d; StartupEventReceivePort empty{};
        d.rt->execution.event_receive = &empty;
        d.call(7, DUMMY_MAPPED, 0);
        d.expect_stop(StartupStop::MissingServices, __LINE__);
    }
}
void case_outcome_stops() {
    struct { StartupEventReceiveOutcome outcome; StartupStop stop; } table[] = {
        {StartupEventReceiveOutcome::Unsupported, StartupStop::UnsupportedSyscall},
        {StartupEventReceiveOutcome::ProviderFailure, StartupStop::ProviderFailure},
        {StartupEventReceiveOutcome::Cancelled, StartupStop::Cancelled}};
    for (const auto &row : table) {
        Direct d; StubPort s; s.rt = d.rt.get();
        s.reply.outcome = row.outcome; s.reply.result = 0; s.reply.source = 1; s.reply.data1 = 2;   // payload of a non-Completed outcome is never published
        d.rt->execution.event_receive = &s.port;
        d.call(HIGH | 0x55, DUMMY_MAPPED, 77);
        d.expect_stop(row.stop, __LINE__);
        RCHECK(s.calls == 1 && s.queue == 0x55 && s.timeout == 77 && s.active_ok && s.batch_closed && s.no_guards);
        RCHECK(s.at_call.gpr[4] == DUMMY_MAPPED && s.at_call.gpr[3] == (HIGH | 0x55));   // callback saw the untouched call state
        RCHECK(d.rt->control->event_receive_calls == 1 && d.rt->control->event_receive_returns == 0);
    }
}
void case_completed_stub() {
    {   // Completed nonzero: guest CELL error in r3 (sign-extended through int32), r4..r7 preserved (also the unreadable dummy).
        Direct d; StubPort s; s.rt = d.rt.get();
        s.reply.outcome = StartupEventReceiveOutcome::Completed; s.reply.result = 0x80010005u;
        s.reply.source = 0xAA; s.reply.data1 = 0xBB; s.reply.data2 = 0xCC; s.reply.data3 = 0xDD;
        d.rt->execution.event_receive = &s.port;
        d.call(3, DUMMY_UNMAPPED, 0);
        d.expect_returned(__LINE__);
        auto &c = d.rt->execution.ppu.cpu;
        RCHECK(c.gpr[3] == UINT64_C(0xFFFFFFFF80010005) && c.gpr[4] == DUMMY_UNMAPPED && c.gpr[6] == R6 && c.gpr[7] == R7);
        RCHECK(c.gpr[5] == 0);
        RCHECK(d.rt->control->event_receive_returns == 1);
    }
    {   // Completed rc 0: all four 64-bit outputs into r4..r7; r5 (timeout) is overwritten by data1, r8 untouched.
        Direct d; StubPort s; s.rt = d.rt.get();
        s.reply.outcome = StartupEventReceiveOutcome::Completed; s.reply.result = 0;
        s.reply.source = UINT64_C(0xFEDCBA9876543210); s.reply.data1 = D1; s.reply.data2 = D2; s.reply.data3 = D3;
        d.rt->execution.event_receive = &s.port;
        d.rt->put32(DUMMY_MAPPED, SENTINEL);
        d.call(3, DUMMY_MAPPED, 0);
        d.expect_returned(__LINE__);
        auto &c = d.rt->execution.ppu.cpu;
        RCHECK(c.gpr[3] == 0 && c.gpr[4] == s.reply.source && c.gpr[5] == D1 && c.gpr[6] == D2 && c.gpr[7] == D3 && c.gpr[8] == R8);
        RCHECK(d.rt->get32(DUMMY_MAPPED) == SENTINEL);   // the dummy buffer is never written
    }
    {   // Execution cancelled WHILE the callback ran: the result is published only after the boundary check, so nothing is.
        Direct d; StubPort s; s.rt = d.rt.get();
        s.reply.outcome = StartupEventReceiveOutcome::Completed; s.reply.result = 0; s.reply.source = 9; s.reply.data1 = D1;
        s.hook = [](StubPort &p) { p.rt->control->cancelled.store(true, std::memory_order_release); };
        d.rt->execution.event_receive = &s.port;
        d.call(3, DUMMY_MAPPED, 0);
        d.expect_stop(StartupStop::Cancelled, __LINE__);
    }
}
void case_real_direct() {
    {   // Real provider, invalid queue: ESRCH is a provider fault that terminates, never an error fed to unchecked guest code.
        Direct d; RealPort p; p.lv2 = &d.rt->comp->lv2; p.control = d.rt->control.get(); p.host = pthread_self();
        d.rt->execution.event_receive = &p.port;
        d.call(HIGH | 0x1234, DUMMY_MAPPED, 0);
        d.expect_stop(StartupStop::ProviderFailure, __LINE__);
        RCHECK(p.calls == 1 && p.last_rc == PS3_LV2_ESRCH && p.last_queue == 0x1234 && p.active_ok && p.no_guards && p.same_thread);
    }
    {   // Real provider policy refuses a finite timeout it cannot honour: UnsupportedSyscall.
        Direct d; RealPort p; p.lv2 = &d.rt->comp->lv2; p.control = d.rt->control.get(); p.host = pthread_self();
        d.rt->execution.event_receive = &p.port;
        d.call(5, DUMMY_MAPPED, 1000);
        d.expect_stop(StartupStop::UnsupportedSyscall, __LINE__);
        RCHECK(p.calls == 1 && p.last_timeout == 1000 && p.last_rc == 0xFFFFFFFFu);   // never reached the queue primitive
    }
}

// A real PPU service thread whose body issues sc130 against the real queue and really blocks.
struct Lane {
    std::unique_ptr<Runtime> rt{new Runtime(1)};
    RealPort rp; Obs obs;
    uint32_t queue = 0, port_id = 0;
    uint64_t dummy = DUMMY_MAPPED;
    ppu_context seeded{}, after{};
    ps3_pthread_service_thread thread{};
    ps3_ppu_ref ref{};
    uint32_t create_rc = 0xFFFFFFFFu, join_code = 0xFFFFFFFFu;
    bool reached = false, caught = false;
    uint64_t published0 = 0;
    void setup(bool preload) {
        Runtime &r = *rt;
        ps3_lv2_queue_attr qa{PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
        RCHECK(ps3_lv2_queue_create(&r.comp->lv2, &queue, &qa, 0, 8) == PS3_LV2_OK);
        RCHECK(ps3_lv2_port_create(&r.comp->lv2, &port_id, PS3_EVENT_PORT_LOCAL, 0) == PS3_LV2_OK &&
               ps3_lv2_port_connect_local(&r.comp->lv2, port_id, queue) == PS3_LV2_OK);
        rp.lv2 = &r.comp->lv2; rp.control = r.control.get();
        r.execution.event_receive = &rp.port;
        obs.rt = &r;
        r.control->terminal_notification = {on_terminal, &obs};
        r.put32(DUMMY_MAPPED, SENTINEL);
        if (preload) RCHECK(send() == PS3_LV2_OK);
        published0 = r.recorder.report().published_count;
    }
    uint32_t send(uint64_t a = D1, uint64_t b = D2, uint64_t c = D3) { return ps3_lv2_port_send(&rt->comp->lv2, port_id, a, b, c); }
    void cancel() { ps3_notify_cancel_request(&rp.cancel, &rt->comp->lv2.port); }
    static void body(ps3_ppu_service *self, void *argument) {
        Lane &l = *static_cast<Lane *>(argument);
        Runtime &r = *l.rt;
        l.rp.service = self; l.obs.service = self; l.rp.host = l.obs.host = pthread_self();
        auto &cpu = r.execution.ppu.cpu;
        seed(cpu, HIGH | l.queue, l.dummy, 0);
        l.seeded = cpu;
        r.control->stop = StartupStop::None;
        active = r.control.get(); r.control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
        try { lv2_syscall(&cpu); l.reached = true; l.after = cpu; active = nullptr; } catch (const Terminal &) { l.caught = true; }
        if (l.reached) { ps3_ppu_service_exit(self, EXIT_COMPLETED); return; }
        pthread_exit(nullptr);   // an early terminal stop never returns through the domain trampoline
    }
    void start() {
        arm("lane-create", 10000);
        create_rc = ps3_ppu_service_create(&rt->comp->ppu, SERVICE_KIND, SERVICE_PRIORITY, CREATOR_PRIORITY, SERVICE_STACK,
            ps3_pthread_service_thread_port(&thread), body, this, &ref);
        disarm();
        RCHECK(create_rc == PS3_PPU_OK);
    }
    bool blocked() { return ps3_ppu_service_is_blocked(&rt->comp->ppu, ref); }
    void expect_really_blocked() {
        RCHECK(blocked());
        ps3_lv2_queue snap{};
        RCHECK(ps3_lv2_queue_snapshot(&rt->comp->lv2, queue, &snap) == PS3_LV2_OK && snap.receivers == 1 && snap.blocked_receivers == 1 && snap.count == 0);
        RCHECK(obs.calls == 0 && !reached && !caught && rp.calls == 1);
    }
    void join() {
        arm("lane-join", 10000);
        RCHECK(ps3_ppu_service_join(&rt->comp->ppu, ref, &join_code) == PS3_PPU_OK);
        disarm();
        ps3_lv2_queue snap{};
        RCHECK(ps3_lv2_queue_snapshot(&rt->comp->lv2, queue, &snap) == PS3_LV2_OK && snap.receivers == 0 && snap.blocked_receivers == 0);
    }
    void teardown() {
        RCHECK(ps3_lv2_port_disconnect(&rt->comp->lv2, port_id) == PS3_LV2_OK && ps3_lv2_port_destroy(&rt->comp->lv2, port_id) == PS3_LV2_OK &&
               ps3_lv2_queue_destroy(&rt->comp->lv2, queue) == PS3_LV2_OK);
    }
    void expect_resumed(uint64_t d1 = D1, uint64_t d2 = D2, uint64_t d3 = D3) {
        RCHECK(reached && !caught && obs.calls == 0 && join_code == EXIT_COMPLETED && rt->control->stop == StartupStop::None);
        RCHECK(after.gpr[3] == 0 && after.gpr[4] == rp.event.source && after.gpr[5] == d1 && after.gpr[6] == d2 && after.gpr[7] == d3);
        RCHECK(rp.event.data1 == d1 && rp.event.data2 == d2 && rp.event.data3 == d3);
        RCHECK(after.gpr[8] == R8 && after.gpr[11] == 130 && after.gpr[1] == STACK && after.gpr[2] == TOC);
        RCHECK(rt->get32(DUMMY_MAPPED) == SENTINEL);   // dummy buffer untouched
        RCHECK(rt->recorder.report().published_count == published0 && !rt->recorder.report().batch_active);   // register-only
        RCHECK(rp.calls == 1 && rp.last_queue == queue && rp.last_timeout == 0 && rp.active_ok && rp.no_guards && rp.same_thread);
        RCHECK(rt->control->event_receive_calls == 1 && rt->control->event_receive_returns == 1 &&
               rt->control->event_receive_queue == queue && rt->control->event_receive_timeout == 0);
    }
    void expect_terminated(StartupStop expected, int event_left) {   // event_left: 1 queued, 0 consumed, -1 either (race)
        RCHECK(!reached && caught && obs.calls == 1 && obs.stop == expected && rt->control->stop == expected && join_code == EXIT_PUBLISHED);
        RCHECK(regs_seeded(obs.terminal, seeded) && regs_seeded(rt->execution.ppu.cpu, seeded));   // no r3 result, no r4..r7, no continuation
        RCHECK(obs.active_cleared && obs.trampoline_cleared && obs.batch_closed && obs.same_thread);
        RCHECK(rt->get32(DUMMY_MAPPED) == SENTINEL);
        RCHECK(rt->recorder.report().published_count == published0 && !rt->recorder.report().batch_active);
        RCHECK(rp.calls == 1 && rp.no_guards && rp.same_thread && rt->control->event_receive_returns == 0);
        ps3_lv2_queue snap{};
        RCHECK(ps3_lv2_queue_snapshot(&rt->comp->lv2, queue, &snap) == PS3_LV2_OK && (event_left < 0 || (snap.count == 1) == (event_left == 1)));
    }
};

void case_blocked_wake(uint64_t dummy) {
    Lane l; l.dummy = dummy; l.setup(false); l.start();
    l.expect_really_blocked();
    RCHECK(l.send() == PS3_LV2_OK);   // the event wakes the really blocked pthread receiver
    l.join();
    l.expect_resumed();
    l.teardown();
}
void case_blocked_cancel() {
    Lane l; l.setup(false); l.start();
    l.expect_really_blocked();
    l.cancel();
    l.join();
    l.expect_terminated(StartupStop::Cancelled, 0);
    RCHECK(l.rp.last_rc == PS3_LV2_CANCELLED && !l.rp.pre_cancel && !l.rp.post_cancel);   // code after the syscall is unreachable
    l.teardown();
}
void case_preloaded_event() {
    Lane l; l.setup(true); l.start(); l.join();   // never blocks: the real queue already holds the event
    l.expect_resumed();
    l.teardown();
}
void case_cancel_after_dequeue() {   // persistent cancel observed AFTER the real dequeue: the consumed event is discarded
    Lane l; l.setup(true);
    l.rp.after_dequeue = [](RealPort &p) { ps3_notify_cancel_request(&p.cancel, &p.lv2->port); };
    l.start(); l.join();
    l.expect_terminated(StartupStop::Cancelled, 0);
    RCHECK(l.rp.post_cancel && l.rp.last_rc == PS3_LV2_OK);
    l.teardown();
}
void case_cancel_precheck() {   // cancel + queued event both present before the call: precheck wins, event is left untouched
    Lane l; l.setup(true); l.cancel();
    l.start(); l.join();
    l.expect_terminated(StartupStop::Cancelled, 1);
    RCHECK(l.rp.pre_cancel && l.rp.last_rc == 0xFFFFFFFFu);
    l.teardown();
}
// Explicit cross-thread race: event and cancel are issued from two threads released together. Either order is legal, but a
// guest continuation may exist only when the actual cancel was NOT observed by the callback's post-check.
void case_race(unsigned iterations) {
    unsigned resumed = 0, cancelled = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        Lane l; l.setup(false); l.start();
        l.expect_really_blocked();
        std::atomic<bool> go{false};
        std::thread sender([&] { while (!go.load()) {} RCHECK(l.send() == PS3_LV2_OK); });
        std::thread canceller([&] { while (!go.load()) {} if (i & 1) for (int y = 0; y < 3000; ++y) std::this_thread::yield(); l.cancel(); });
        go.store(true);
        sender.join(); canceller.join();
        l.join();
        RCHECK(ps3_notify_cancel_is_requested(&l.rp.cancel));
        if (l.reached) {
            ++resumed;
            RCHECK(!l.rp.pre_cancel && !l.rp.post_cancel && l.rp.last_rc == PS3_LV2_OK);
            l.expect_resumed();
        } else {
            ++cancelled;
            RCHECK(l.rp.pre_cancel || l.rp.post_cancel || l.rp.last_rc == PS3_LV2_CANCELLED);
            l.expect_terminated(StartupStop::Cancelled, -1);
        }
        l.teardown();
    }
    std::printf("race: iterations=%u resumed=%u cancelled=%u\n", iterations, resumed, cancelled);
    RCHECK(resumed + cancelled == iterations);
}
// Per-execution context: two independent executions block concurrently; each gets only its own event and context.
void case_two_executions() {
    Lane a, b; a.setup(false); b.setup(false);
    RCHECK(a.rt->control.get() != b.rt->control.get() && &a.rp != &b.rp);
    a.start(); b.start();
    a.expect_really_blocked(); b.expect_really_blocked();
    RCHECK(b.send(0xB1, 0xB2, 0xB3) == PS3_LV2_OK);
    b.join();
    RCHECK(a.blocked() && !a.reached);    // a was not woken by b's event
    RCHECK(a.send(0xA1, 0xA2, 0xA3) == PS3_LV2_OK);
    a.join();
    a.expect_resumed(0xA1, 0xA2, 0xA3);
    b.expect_resumed(0xB1, 0xB2, 0xB3);
    a.teardown(); b.teardown();
}
}  // namespace

int main() {
    std::thread(watchdog_main).detach();
    struct Step { const char *name; void (*fn)(); };
    const Step steps[] = {
        {"missing_services", case_missing_services}, {"outcome_stops", case_outcome_stops}, {"completed_stub", case_completed_stub},
        {"real_direct", case_real_direct}, {"blocked_wake_mapped_dummy", [] { case_blocked_wake(DUMMY_MAPPED); }},
        {"blocked_wake_unmapped_dummy", [] { case_blocked_wake(DUMMY_UNMAPPED); }}, {"blocked_cancel", case_blocked_cancel},
        {"preloaded_event", case_preloaded_event}, {"cancel_after_dequeue", case_cancel_after_dequeue},
        {"cancel_precheck", case_cancel_precheck}, {"race", [] { case_race(8); }}, {"two_executions", case_two_executions}};
    for (const Step &s : steps) {
        const int before = receive_failures.load() + failures;
        arm(s.name, 60000);
        s.fn();
        disarm();
        std::printf("case %-28s %s\n", s.name, receive_failures.load() + failures == before ? "ok" : "FAILED");
    }
    const int total = receive_failures.load() + failures;
    std::printf("%s failures=%d (HOST test; not game execution)\n", total ? "STARTUP_EVENT_RECEIVE_FAIL" : "STARTUP_EVENT_RECEIVE_OK", total);
    return total ? 1 : 0;
}
