/* Host behavior test for the per-execution terminal notification port (StartupControl::terminal_notification).
 * Real collaborators: the actual startup_execution.cpp stop() path driven by the actual lv2_syscall dispatcher
 * (sc128 with the actual composed-or-removed event-queue registry), the actual PPU service registry
 * (ps3_ppu_service_create / _exit / _join with the real pthread service-thread adapter), real shared guest memory
 * and user-memory quota. The guest is a LABEL FIXTURE: a host worker seeds PPU registers and issues one real syscall
 * that fails deterministically. This is not the game, not a secondary guest thread and not the real helper.
 * The inherited SPURS fixture (Runtime, host threadExit shim that throws Terminal) is reused unchanged.
 * Expected values are derived by hand from the contract (seeded registers, stop reasons), never captured.
 * An actual early terminal stop bypasses the domain trampoline's post-body service_exit publication; to model that
 * faithfully the worker catches the host shim's Terminal and calls pthread_exit instead of returning through the
 * trampoline (which would repair the missing notification and hide the defect). */
#define main inherited_spurs_main
#include "test_startup_spurs_initialize.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<int> notify_failures{0};
#define TCHECK(value) do { if (!(value)) { std::printf("TERMINAL_NOTIFY_FAIL line=%d expression=%s\n", __LINE__, #value); \
    std::fflush(stdout); notify_failures.fetch_add(1); } } while (0)

// Hard watchdog: any blocking lane that does not finish is a FAILURE (exit 124), never a pass or a skip.
std::atomic<long long> watchdog_deadline_ms{0};
std::atomic<const char *> watchdog_phase{"idle"};
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void arm(const char *phase, long long ms) { watchdog_phase.store(phase); watchdog_deadline_ms.store(now_ms() + ms); }
void disarm() { watchdog_deadline_ms.store(0); }
void watchdog_main() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const long long deadline = watchdog_deadline_ms.load();
        if (deadline && now_ms() > deadline) {
            std::printf("TERMINAL_NOTIFY_WATCHDOG_TIMEOUT phase=%s\n", watchdog_phase.load());
            std::fflush(stdout);
            _exit(124);
        }
    }
}

constexpr uint32_t EXIT_PUBLISHED = 0x7E57u, EXIT_RESCUE = 0xDEADu, SERVICE_KIND = 9, SERVICE_PRIORITY = 100, CREATOR_PRIORITY = 250;
constexpr uint64_t SERVICE_STACK = 0x10000;
constexpr uint32_t GUEST_ATTR_OFFSET = 0x400, GUEST_OUT_OFFSET = 0x440, OUT_SENTINEL = 0xCAFEF00Du;
void idle_trampoline(void *) {}

// Everything the observer saw on the exiting host thread. Written only by that thread before the notification
// returns; read by the owner after the service wait resolves or after join.
struct Observation {
    unsigned calls = 0;
    StartupControl *control = nullptr;
    StartupStop stop = StartupStop::Returned;   // sentinel distinct from every expected reason
    ppu_context terminal{}, cpu{};
    bool active_cleared = false, trampoline_cleared = false, trace_closed = false, same_thread = false;
};
struct Worker {
    Runtime *rt = nullptr;
    ps3_ppu_service *service = nullptr;   // set by the service body; null in plain-thread lanes
    uint32_t out = 0, attr = 0, key = 0;
    pthread_t host{};
    Observation obs;
    std::atomic<bool> stopped{false};      // set by the worker after the terminal catch, before it exits
    bool reached_after_stop = false, caught = false, active_after = false, trampoline_after = false;
};
void on_terminal(void *context, StartupControl *control) {
    Worker *w = static_cast<Worker *>(context);
    Observation &o = w->obs;
    ++o.calls;
    o.control = control; o.stop = control->stop; o.terminal = control->terminal; o.cpu = *control->cpu;
    o.active_cleared = active == nullptr;
    o.trampoline_cleared = g_trampoline_fn == nullptr;
    o.trace_closed = w->rt->recorder.can_begin(1);
    o.same_thread = pthread_equal(pthread_self(), w->host) != 0;
    if (w->service) ps3_ppu_service_exit(w->service, EXIT_PUBLISHED);   // the future composition binding, nonblocking
}
bool same_regs(const ppu_context &a, const ppu_context &b) {
    for (size_t i = 0; i < sizeof a.gpr / sizeof a.gpr[0]; ++i) if (a.gpr[i] != b.gpr[i]) return false;
    return a.thread_id == b.thread_id && a.lr == b.lr;
}
// One real sc128 on the owned execution. key != 0: real input the domain does not own (UnsupportedSyscall, stop with
// the trace batch OPEN after begin_trace). event_queues == nullptr: MissingServices (no batch). Both stop for real.
void run_guest(Worker &w) {
    Runtime &r = *w.rt;
    auto &cpu = r.execution.ppu.cpu;
    w.host = pthread_self();
    cpu.gpr[11] = 128; cpu.gpr[3] = w.out; cpu.gpr[4] = w.attr; cpu.gpr[5] = w.key; cpu.gpr[6] = 127;
    r.control->stop = StartupStop::None;
    active = r.control.get(); r.control->cpu = &cpu; g_trampoline_fn = idle_trampoline;
    try { lv2_syscall(&cpu); w.reached_after_stop = true; active = nullptr; } catch (const Terminal &) { w.caught = true; }
    w.active_after = active == nullptr; w.trampoline_after = g_trampoline_fn == nullptr;
    w.stopped.store(true);
}
void service_body(ps3_ppu_service *self, void *argument) {
    Worker *w = static_cast<Worker *>(argument);
    w->service = self;
    run_guest(*w);
    pthread_exit(nullptr);   // never return through the domain trampoline: that is the real early-stop bypass
}

struct ServiceRun {
    std::unique_ptr<Runtime> rt{new Runtime(1)};
    Worker w;
    ps3_pthread_service_thread thread{};
    ps3_ppu_ref ref{};
    uint32_t create_rc = 0xFFFFFFFFu;
    uint64_t used0 = 0; size_t live0 = 0, queues0 = 0;
    StartupStop expected = StartupStop::None;
    void setup(bool missing_services, uint32_t key, uint32_t slot_offset, bool observe) {
        Runtime &r = *rt;
        TCHECK(r.execution.event_queues != nullptr);   // the composed registry is really there before it is removed
        if (missing_services) r.execution.event_queues = nullptr;
        expected = missing_services ? StartupStop::MissingServices : StartupStop::UnsupportedSyscall;
        w.rt = rt.get(); w.attr = STACK_BASE + GUEST_ATTR_OFFSET; w.out = STACK_BASE + slot_offset; w.key = key;
        const unsigned char attribute[16]{0, 0, 0, 2, 0, 0, 0, 1, 0, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E};   // PRIORITY, PPU
        r.put(w.attr, attribute, sizeof attribute); r.put32(w.out, OUT_SENTINEL);
        if (observe) r.control->terminal_notification = {on_terminal, &w};   // injected before the worker starts
        used0 = r.used(); live0 = ps3_ppu_threads_live(&r.comp->ppu);
        queues0 = ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE);
    }
    void create() {
        create_rc = ps3_ppu_service_create(&rt->comp->ppu, SERVICE_KIND, SERVICE_PRIORITY, CREATOR_PRIORITY, SERVICE_STACK,
            ps3_pthread_service_thread_port(&thread), service_body, &w, &ref);
    }
    // The terminal record, observed from inside the notification and again after join, against the seeded values.
    void check_record(const Observation &o, unsigned expected_calls) {
        Runtime &r = *rt;
        TCHECK(o.calls == expected_calls);
        TCHECK(o.control == r.control.get());
        TCHECK(o.stop == expected);
        TCHECK(o.terminal.gpr[11] == 128 && o.terminal.gpr[3] == w.out && o.terminal.gpr[4] == w.attr &&
               o.terminal.gpr[5] == w.key && o.terminal.gpr[6] == 127);   // no r3 result, no continuation
        TCHECK(o.terminal.gpr[1] == STACK && o.terminal.gpr[2] == TOC && o.terminal.thread_id == 1);
        TCHECK(same_regs(o.terminal, o.cpu));
        TCHECK(o.active_cleared && o.trampoline_cleared && o.trace_closed && o.same_thread);
    }
    void check_no_guest_effect() {
        Runtime &r = *rt;
        TCHECK(!w.reached_after_stop && w.caught);
        TCHECK(w.active_after && w.trampoline_after);
        TCHECK(r.control->stop == expected && same_regs(r.control->terminal, w.obs.terminal));   // record not mutated later
        TCHECK(r.get32(w.out) == OUT_SENTINEL);   // the failed syscall wrote nothing
        TCHECK(ps3_lv2_live_count(&r.comp->lv2, PS3_LV2_KIND_QUEUE) == queues0 && r.control->event_queue_owned_count == 0);
        TCHECK(r.control->event_queue_calls == (expected == StartupStop::UnsupportedSyscall ? 1u : 0u));
        TCHECK(r.control->event_queue_created_id == 0 && r.control->event_queue_retained_id == 0);
    }
    // Create has returned: the wait was resolved by the notification, the host thread is NOT yet joined.
    void check_resolved_then_join() {
        Runtime &r = *rt;
        TCHECK(create_rc == PS3_PPU_OK);
        ps3_ppu_service &s = r.comp->ppu.services[ref.slot];
        TCHECK(s.id == ref.id);
        TCHECK(__atomic_load_n(&s.life, __ATOMIC_SEQ_CST) == PS3_PPU_SERVICE_EXITED);
        TCHECK(__atomic_load_n(&s.activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_EXITED);
        TCHECK(!ps3_ppu_service_is_blocked(&r.comp->ppu, ref));
        TCHECK(s.exit_code == EXIT_PUBLISHED);   // published by the observer; the trampoline would have said 0
        check_record(w.obs, 1);
        TCHECK(r.used() == used0 + SERVICE_STACK);                          // stack charge owned until join
        TCHECK(ps3_ppu_threads_live(&r.comp->ppu) == live0 + 1);
        uint32_t code = 0;
        TCHECK(ps3_ppu_service_join(&r.comp->ppu, ref, &code) == PS3_PPU_OK && code == EXIT_PUBLISHED);
        TCHECK(r.used() == used0 && ps3_ppu_threads_live(&r.comp->ppu) == live0);   // released by join, not before
        check_no_guest_effect();
        TCHECK(w.obs.calls == 1);   // still exactly once after the thread is fully gone
    }
};

// 1. An early terminal stop on a real service thread resolves the creator's wait through the notification.
void lane_service(bool missing_services, const char *phase) {
    ServiceRun run;
    run.setup(missing_services, missing_services ? 0u : 0x5A5A1234u, GUEST_OUT_OFFSET, true);
    arm(phase, 15000);
    run.create();
    disarm();
    run.check_resolved_then_join();
}
// 2. No observer: the stop still records and exits (host thread exit) and nothing is notified; an absent fn with a
//    non-null context is still absent.
void lane_no_observer() {
    ServiceRun run;
    run.setup(true, 0, GUEST_OUT_OFFSET, false);
    run.rt->control->terminal_notification.context = &run.w;
    TCHECK(run.rt->control->terminal_notification.fn == nullptr);
    arm("no-observer", 15000);
    std::thread worker([&run] { run_guest(run.w); });
    worker.join();
    disarm();
    TCHECK(run.w.obs.calls == 0 && run.w.obs.control == nullptr);
    TCHECK(run.w.stopped.load() && run.w.caught && !run.w.reached_after_stop && run.w.active_after && run.w.trampoline_after);
    TCHECK(run.rt->control->stop == StartupStop::MissingServices && run.rt->control->terminal.gpr[11] == 128 &&
           run.rt->control->terminal.gpr[3] == run.w.out && run.rt->control->terminal.thread_id == 1);
}
// 3. Control for the hazard itself: WITHOUT a notification the real create really keeps waiting after the real early
//    stop (it is not a pass for the port). The test rescues the wait only after proving it was stuck.
void lane_hazard_control() {
    ServiceRun run;
    run.setup(true, 0, GUEST_OUT_OFFSET, false);
    std::atomic<bool> done{false};
    arm("hazard-control", 20000);
    std::thread creator([&run, &done] { run.create(); done.store(true); });
    while (!run.w.stopped.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    TCHECK(!done.load());   // the worker terminally stopped and exited; create is still blocked
    ps3_ppu_service *stuck = nullptr; unsigned running = 0;
    for (auto &s : run.rt->comp->ppu.services)
        if (__atomic_load_n(&s.life, __ATOMIC_SEQ_CST) == PS3_PPU_SERVICE_RUNNING &&
            __atomic_load_n(&s.activity, __ATOMIC_SEQ_CST) == PS3_PPU_ACTIVITY_RUNNING) { stuck = &s; ++running; }
    TCHECK(running == 1);
    if (stuck) ps3_ppu_service_exit(stuck, EXIT_RESCUE);   // what the missing notification should have done
    creator.join();
    disarm();
    TCHECK(run.create_rc == PS3_PPU_OK);
    uint32_t code = 0;
    TCHECK(ps3_ppu_service_join(&run.rt->comp->ppu, run.ref, &code) == PS3_PPU_OK && code == EXIT_RESCUE);
    TCHECK(run.w.obs.calls == 0 && run.w.caught && !run.w.reached_after_stop);
}
// 4. Two independently owned executions/services/runtimes at once: each worker is notified once, about its own
//    control, with its own seeded registers and its own stop reason.
void lane_two_contexts() {
    ServiceRun a, b;
    a.setup(false, 0xA1A1A1A1u, GUEST_OUT_OFFSET, true);
    b.setup(true, 0, GUEST_OUT_OFFSET + 0x40, true);
    TCHECK(a.rt->control.get() != b.rt->control.get() && &a.w != &b.w && a.expected != b.expected);
    arm("two-contexts", 20000);
    std::thread ca([&a] { a.create(); }), cb([&b] { b.create(); });
    ca.join(); cb.join();
    disarm();
    a.check_resolved_then_join();
    b.check_resolved_then_join();
    TCHECK(a.w.obs.control == a.rt->control.get() && b.w.obs.control == b.rt->control.get());
    TCHECK(a.w.obs.terminal.gpr[3] != b.w.obs.terminal.gpr[3] && a.w.obs.stop != b.w.obs.stop);
}
}
// Deterministic predicate check (never a failing lane): the exit predicate is false only while THIS thread owns a tracked guard.
void lane_local_ownership_predicate() {
    ps3_pthread_guard a, b;
    TCHECK(!ps3_pthread_guard_init(&a) && !ps3_pthread_guard_init(&b));
    const ps3_instance_guard pa = tracked_guard_port(&a), pb = tracked_guard_port(&b);
    TCHECK(thread_holds_no_tracked_guard());
    pa.lock(pa.context); pb.lock(pb.context);
    TCHECK(!thread_holds_no_tracked_guard());
    pa.unlock(pa.context);                       // non-LIFO release
    TCHECK(!thread_holds_no_tracked_guard());
    pb.unlock(pb.context);
    TCHECK(thread_holds_no_tracked_guard());
    TCHECK(!ps3_pthread_guard_destroy(&a) && !ps3_pthread_guard_destroy(&b));
}
void lane_other_thread_owns_guard() {
    ServiceRun run;
    run.setup(true, 0, GUEST_OUT_OFFSET, false);
    std::mutex gate;
    std::condition_variable changed;
    bool held = false, release = false;
    arm("exit-while-another-thread-holds-guard", 20000);
    std::thread holder([&] {
        const auto guard = run.rt->comp->ppu.guard;
        guard.lock(guard.context);
        std::unique_lock<std::mutex> lock(gate);
        held = true;
        changed.notify_one();
        changed.wait(lock, [&] { return release; });
        lock.unlock();
        guard.unlock(guard.context);
    });
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return held; });
    }
    std::thread worker([&] { run_guest(run.w); });
    worker.join();
    {
        std::lock_guard<std::mutex> lock(gate);
        release = true;
    }
    changed.notify_one();
    holder.join();
    disarm();
    TCHECK(run.w.caught && !run.w.reached_after_stop);
    TCHECK(run.rt->control->stop == StartupStop::MissingServices);
}

int main() {
    std::thread(watchdog_main).detach();
    lane_local_ownership_predicate();
    lane_other_thread_owns_guard();
    lane_service(false, "service-unsupported-open-batch");
    lane_service(true, "service-missing-services");
    lane_no_observer();
    lane_hazard_control();
    lane_two_contexts();
    const int total = notify_failures.load() + failures;
    std::printf("TERMINAL_NOTIFY_HOST failures=%d native_runtime=false game_boot=false real_guest_thread=false\n", total);
    return total != 0;
}
