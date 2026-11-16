#include "../adapters/discovery_budget_binding.h"
// Reuse the actual bridge, real host memory/domain and terminal-before-unwind
// guard/credit checks. This is host bridge evidence, not native stub execution.
#define main recorder_control_main
#include "test_startup_recorder.cpp"
#undef main


namespace {
static_assert(!std::is_convertible<HleTraceBatch *, BoundaryBudget>::value &&
    !std::is_convertible<bool, BoundaryBudget>::value,
    "Boundary mode must not accept a misplaced batch pointer or bool");
using infamous::discovery::Category;
using infamous::discovery::DiscoveryBudget;
using infamous::discovery::Limits;
using infamous::discovery::StopReason;
int budget_cases, budget_failures;
#define BCHECK(value) do { if (!(value)) { std::printf("STARTUP_BUDGET_FAIL case=%d line=%d\n", budget_cases, __LINE__); ++budget_failures; } } while (0)
constexpr uint64_t LONG_DEADLINE = UINT64_C(1000000000);
struct ClockFixture {
    uint64_t now_us = 0, later_us = 0;
    unsigned calls = 0, later_from = 0, fail_on = 0, cancel_on = 0;
    bool fail = false;
    std::atomic<bool> *cancel = nullptr;
    static ps3_clock_status sample(void *opaque, uint64_t *output) {
        auto &self = *static_cast<ClockFixture *>(opaque);
        ++self.calls;
        if (self.cancel && self.calls == self.cancel_on) self.cancel->store(true);
        if (self.fail || self.calls == self.fail_on) return PS3_CLOCK_ARGUMENT;
        *output = self.later_from && self.calls >= self.later_from ? self.later_us : self.now_us;
        return PS3_CLOCK_OK;
    }
};
struct BudgetFixture {
    Runtime runtime;
    ClockFixture clock;
    ps3_clock port{&clock, ClockFixture::sample};
    DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    explicit BudgetFixture(Limits limits = {4, 4, 4, LONG_DEADLINE}, bool valid_clock = true)
        : port{&clock, valid_clock ? ClockFixture::sample : nullptr}, policy(limits, 0), budget(policy, port) {
        BCHECK(runtime.recorder.configure(runtime.events, 4, PpuMemoryTraceRetention::Suffix));
    }
    ~BudgetFixture() {
        // Budget/binding storage still exists during reservation release.
        if (runtime.execution.ppu.admitted)
            BCHECK(native_startup_release_after_join(runtime.execution) == PS3_GUEST_MEMORY_OK);
    }
    bool admit() {
        return native_startup_admit_budgeted(runtime.execution, runtime.binding,
            runtime.services, runtime.recorder, budget);
    }
};
template<class Operation> bool call_bridge(BudgetFixture &fixture, Operation operation)
{
    Runtime &runtime = fixture.runtime;
    terminal_recorder = &runtime.recorder;
    terminal_guard = &runtime.guard;
    terminal_closed = terminal_unlocked = false;
    active = runtime.control.get();
    bool returned = true;
    try { operation(); active = nullptr; }
    catch (const Terminal &) {
        returned = false;
        BCHECK(terminal_closed && terminal_unlocked);
    }
    terminal_recorder = nullptr;
    terminal_guard = nullptr;
    return returned;
}
#ifdef PS3_NATIVE_STARTUP_MEMORY
bool call_id(BudgetFixture &f) {
    return call_bridge(f, [&] { ps3_hle_call(0x350d454e, &f.runtime.execution.ppu.cpu); });
}
#endif
void zero_effects(BudgetFixture &f, const ppu_context &before) {
    BCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
    BCHECK(f.runtime.probe.writes == 0 && f.runtime.recorder.report().published_count == 0);
    BCHECK(!f.runtime.recorder.report().batch_active);
}
#ifdef PS3_NATIVE_STARTUP_MEMORY
void combined_once() {
    ++budget_cases; BudgetFixture f({4, 2, 4, LONG_DEADLINE});
    BCHECK(f.admit()); BCHECK(f.clock.calls == 0);
    if (!call_id(f)) { BCHECK(false); return; }
    BCHECK(f.policy.snapshot().hle == 1 && f.clock.calls == 3);
    f.runtime.execution.ppu.cpu.gpr[11] = 147;
    f.runtime.execution.ppu.cpu.gpr[3] = UINT64_C(0xcafebabe11223344);
    if (!call_bridge(f, [&] { lv2_syscall(&f.runtime.execution.ppu.cpu); })) { BCHECK(false); return; }
    BCHECK(f.runtime.execution.ppu.cpu.gpr[3] == UINT64_C(79800000));
    BCHECK(f.policy.snapshot().hle == 2 && f.clock.calls == 4);
    BCHECK(f.runtime.recorder.report().published_count == 2 && f.runtime.probe.writes == 2);
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_bridge(f, [&] { ps3_hle_call(0xffffffff, &f.runtime.execution.ppu.cpu); }));
    BCHECK(f.runtime.control->stop == StartupStop::HleSyscallBudget);
    BCHECK(f.policy.snapshot().hle == 2 && f.policy.snapshot().generated == 0 && f.policy.snapshot().indirect == 0);
    BCHECK(f.policy.snapshot().stop == StopReason::HleExhausted && f.clock.calls == 5);
    BCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
    BCHECK(f.runtime.control->import_count == 1);
}
void entry_exhaustion() {
    ++budget_cases; BudgetFixture f({4, 0, 4, LONG_DEADLINE});
    BCHECK(f.admit()); const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == StartupStop::HleSyscallBudget);
    BCHECK(f.policy.snapshot().hle == 0 && f.policy.snapshot().stop == StopReason::HleExhausted);
    BCHECK(f.clock.calls == 1 && f.runtime.control->import_count == 0);
    zero_effects(f, before);
}
void unknown_entry(bool syscall) {
    ++budget_cases; BudgetFixture f;
    BCHECK(f.admit());
    f.runtime.execution.ppu.cpu.gpr[11] = UINT64_C(0x100000000) | 147;
    f.runtime.execution.ppu.cpu.gpr[3] = UINT64_C(0xcafebabe11223344);
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_bridge(f, [&] {
        if (syscall) lv2_syscall(&f.runtime.execution.ppu.cpu);
        else ps3_hle_call(0xffffffff, &f.runtime.execution.ppu.cpu);
    }));
    BCHECK(f.runtime.control->stop == (syscall ? StartupStop::UnsupportedSyscall : StartupStop::UnknownImport));
    BCHECK(f.policy.snapshot().hle == 1 && f.policy.snapshot().stop == StopReason::None && f.clock.calls == 1);
    if (syscall) BCHECK(f.runtime.control->syscall_number == 147);
    zero_effects(f, before);
}
enum class RejectedEntry { ForeignCpu, InvalidOwner, Cancelled, LegacyDeadline };
void rejected_entry(RejectedEntry kind) {
    ++budget_cases; BudgetFixture f;
    BCHECK(f.admit()); const ppu_context before = f.runtime.execution.ppu.cpu;
    ppu_context alien{};
    const ppu_context alien_before = alien;
    if (kind == RejectedEntry::InvalidOwner) f.runtime.services.memory = nullptr;
    if (kind == RejectedEntry::Cancelled) f.runtime.control->cancelled.store(true);
    if (kind == RejectedEntry::LegacyDeadline) { host_tick = 1; f.runtime.control->deadline_tick = 1; }
    BCHECK(!call_bridge(f, [&] { ps3_hle_call(0x350d454e,
        kind == RejectedEntry::ForeignCpu ? &alien : &f.runtime.execution.ppu.cpu); }));
    host_tick = 0;
    const StartupStop wanted = kind == RejectedEntry::Cancelled ? StartupStop::Cancelled :
        kind == RejectedEntry::LegacyDeadline ? StartupStop::Deadline : StartupStop::MissingServices;
    BCHECK(f.runtime.control->stop == wanted && f.clock.calls == 0);
    BCHECK(f.policy.snapshot().hle == 0 && f.policy.snapshot().stop == StopReason::None);
    BCHECK(!std::memcmp(&alien, &alien_before, sizeof(alien)));
    zero_effects(f, before);
}
void legacy_rejects(bool recorded) {
    ++budget_cases; BudgetFixture f;
    f.runtime.execution.ppu.budget = &f.budget;
    const bool admitted = recorded ? native_startup_admit_recorded(f.runtime.execution, f.runtime.binding,
        f.runtime.services, f.runtime.recorder) : native_startup_admit(f.runtime.execution, f.runtime.binding, f.runtime.services);
    BCHECK(!admitted && !f.runtime.execution.ppu.admitted && !f.runtime.control->cpu);
    BCHECK(f.runtime.execution.ppu.budget == &f.budget && f.clock.calls == 0);
    BCHECK(f.policy.snapshot().hle == 0 && f.policy.snapshot().stop == StopReason::None);
}
void rollback_and_fixed_deadline() {
    ++budget_cases; BudgetFixture f({4, 4, 4, 1000});
    f.runtime.services.memory = nullptr;
    BCHECK(!f.admit());
    BCHECK(!f.runtime.execution.ppu.budget && !f.runtime.execution.ppu.recorder && !f.runtime.control->cpu);
    BCHECK(f.policy.snapshot().start_ns == 0 && f.policy.snapshot().last_ns == 0 && f.clock.calls == 0);
    f.runtime.services.memory = &f.runtime.binding.coherent_memory;
    f.clock.now_us = 2;
    BCHECK(f.admit() && f.clock.calls == 0);
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == StartupStop::Deadline && f.policy.snapshot().stop == StopReason::Deadline);
    BCHECK(f.policy.snapshot().hle == 0 && f.policy.snapshot().start_ns == 0);
    zero_effects(f, before);
}
enum class InvalidSelection { UnconfiguredRecorder, ActiveRecorder, Trace, Recorder, Budget, Clock, UsedPolicy };
void invalid_selection(InvalidSelection kind) {
    ++budget_cases; BudgetFixture f({4, 4, 4, LONG_DEADLINE}, kind != InvalidSelection::Clock);
    PpuMemoryTraceRecorder unconfigured;
    if (kind == InvalidSelection::ActiveRecorder) BCHECK(f.runtime.recorder.begin(2));
    if (kind == InvalidSelection::Trace) f.runtime.execution.ppu.trace = &f.runtime.control->journal;
    if (kind == InvalidSelection::Recorder) f.runtime.execution.ppu.recorder = &f.runtime.recorder;
    if (kind == InvalidSelection::Budget) f.runtime.execution.ppu.budget = &f.budget;
    if (kind == InvalidSelection::UsedPolicy) BCHECK(f.budget.admit(Category::Hle).admitted);
    auto *const trace_before = f.runtime.execution.ppu.trace;
    auto *const recorder_before = f.runtime.execution.ppu.recorder;
    auto *const budget_before = f.runtime.execution.ppu.budget;
    const auto policy_before = f.policy.snapshot();
    const auto report_before = f.runtime.recorder.report();
    const auto calls_before = f.clock.calls;
    const ppu_context cpu_before = f.runtime.execution.ppu.cpu;
    BCHECK(!native_startup_admit_budgeted(f.runtime.execution, f.runtime.binding, f.runtime.services,
        kind == InvalidSelection::UnconfiguredRecorder ? unconfigured : f.runtime.recorder, f.budget));
    BCHECK(!f.runtime.execution.ppu.admitted && !f.runtime.control->cpu);
    BCHECK(f.runtime.execution.ppu.trace == trace_before && f.runtime.execution.ppu.recorder == recorder_before &&
        f.runtime.execution.ppu.budget == budget_before);
    const auto after = f.policy.snapshot();
    BCHECK(after.generated == policy_before.generated && after.hle == policy_before.hle && after.indirect == policy_before.indirect &&
        after.start_ns == policy_before.start_ns && after.last_ns == policy_before.last_ns && after.stop == policy_before.stop);
    BCHECK(f.clock.calls == calls_before);
    BCHECK(f.runtime.recorder.report().batch_active == report_before.batch_active &&
        f.runtime.recorder.report().remaining_publications == report_before.remaining_publications);
    BCHECK(!std::memcmp(&cpu_before, &f.runtime.execution.ppu.cpu, sizeof(cpu_before)));
    BCHECK(f.runtime.probe.writes == 0 && f.runtime.recorder.report().published_count == 0);
    // This is explicitly the caller's external credit, not adapter cleanup.
    if (kind == InvalidSelection::ActiveRecorder) f.runtime.recorder.finish();
}
void reservation_capacity_rollback() {
    ++budget_cases; BudgetFixture f;
    std::unique_ptr<PpuMemoryExecution> holders[4];
    for (auto &holder : holders) {
        holder = std::make_unique<PpuMemoryExecution>();
        BCHECK(ppu_memory_admit(*holder, f.runtime.binding, f.runtime.services,
            {f.runtime.control.get(), memory_cancelled, memory_stopped}) == PpuMemoryAdmission::Ready);
    }
    unsigned char owners_before[sizeof(f.runtime.owners)];
    std::memcpy(owners_before, f.runtime.owners, sizeof(owners_before));
    BCHECK(!f.admit());
    BCHECK(!f.runtime.execution.ppu.budget && !f.runtime.execution.ppu.recorder && !f.runtime.control->cpu);
    BCHECK(!std::memcmp(owners_before, f.runtime.owners, sizeof(owners_before)));
    BCHECK(f.clock.calls == 0 && f.policy.snapshot().hle == 0 && f.policy.snapshot().stop == StopReason::None);
    // No worker was launched for these synchronous host reservation holders.
    for (auto &holder : holders) if (holder->admitted)
        BCHECK(ppu_memory_release_after_join(*holder) == PS3_GUEST_MEMORY_OK);
}
enum class ClockFailure { Provider, Overflow, Regression };
void clock_failure(ClockFailure failure) {
    ++budget_cases; BudgetFixture f;
    BCHECK(f.admit());
    if (failure == ClockFailure::Regression) {
        f.clock.now_us = 5;
        if (!call_id(f)) { BCHECK(false); return; }
        f.clock.now_us = 4;
        f.runtime.execution.ppu.cpu.gpr[3] = 0x10200;
    } else if (failure == ClockFailure::Provider) f.clock.fail = true;
    else f.clock.now_us = UINT64_MAX / 1000 + 1;
    const ppu_context before = f.runtime.execution.ppu.cpu;
    const auto publications = f.runtime.recorder.report().published_count;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == StartupStop::BudgetClockFailure);
    BCHECK(f.policy.snapshot().stop == (failure == ClockFailure::Regression ? StopReason::ClockRegressed : StopReason::InvalidClock));
    BCHECK(f.runtime.recorder.report().published_count == publications);
    BCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
}
void latched_invalid_category() {
    ++budget_cases; BudgetFixture f;
    BCHECK(f.admit());
    const auto denied = f.budget.admit(static_cast<Category>(99));
    BCHECK(!denied.admitted && denied.reason == StopReason::InvalidCategory);
    const unsigned before_calls = f.clock.calls;
    f.clock.fail = true;
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == StartupStop::BudgetInvalidInput && f.clock.calls == before_calls);
    zero_effects(f, before);
}
void generated_mapping() {
    ++budget_cases; BudgetFixture f({0, 4, 4, LONG_DEADLINE});
    BCHECK(f.admit()); const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_bridge(f, [&] { (void)ppu_native_read32(&f.runtime.execution.ppu.cpu, 0x10000); }));
    BCHECK(f.runtime.control->stop == StartupStop::GeneratedBudget);
    BCHECK(f.policy.snapshot().generated == 0 && f.policy.snapshot().stop == StopReason::GeneratedExhausted);
    zero_effects(f, before);
}
void full_strict_preflight() {
    ++budget_cases; BudgetFixture f;
    BCHECK(f.runtime.recorder.configure(f.runtime.events, 1, PpuMemoryTraceRetention::Strict));
    BCHECK(f.runtime.recorder.begin(1));
    f.runtime.recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, false, 0x10000, 8, 7));
    f.runtime.recorder.finish();
    BCHECK(f.admit()); const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == StartupStop::TraceCapacity && f.policy.snapshot().hle == 1 && f.clock.calls == 1);
    BCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    BCHECK(f.runtime.probe.writes == 0 && !std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
}
void partial_poll_stop(bool invalid_clock) {
    ++budget_cases; BudgetFixture f({4, 4, 4, 1000000});
    BCHECK(f.admit()); const ppu_context before = f.runtime.execution.ppu.cpu;
    if (invalid_clock) f.clock.fail_on = 3;
    else { f.clock.later_from = 3; f.clock.later_us = 1000; }
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == (invalid_clock ? StartupStop::BudgetClockFailure : StartupStop::Deadline));
    BCHECK(f.policy.snapshot().hle == 1 && f.clock.calls == 3);
    BCHECK(f.runtime.bytes(0x10200) == 0x1234 && f.runtime.bytes(0x10128) == 0);
    BCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    BCHECK(f.runtime.recorder.report().remaining_publications == 0 && terminal_closed && terminal_unlocked);
    BCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
}
void cancel_inside_clock(bool exhausted) {
    ++budget_cases; BudgetFixture f({4, exhausted ? UINT64_C(0) : UINT64_C(4), 4, LONG_DEADLINE});
    BCHECK(f.admit());
    f.clock.cancel = &f.runtime.control->cancelled;
    f.clock.cancel_on = 1;
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_id(f));
    BCHECK(f.runtime.control->stop == (exhausted ? StartupStop::HleSyscallBudget : StartupStop::Cancelled));
    BCHECK(f.policy.snapshot().stop == (exhausted ? StopReason::HleExhausted : StopReason::None));
    BCHECK(f.policy.snapshot().hle == (exhausted ? 0 : 1) && f.clock.calls == 1);
    zero_effects(f, before);
}
#else
void feature_off(bool enabled, bool invalid_owner) {
    ++budget_cases; BudgetFixture f;
    BCHECK(enabled ? f.admit() : native_startup_admit(f.runtime.execution, f.runtime.binding, f.runtime.services));
    f.runtime.execution.ppu.cpu.gpr[11] = UINT64_C(0x100000000) | 147;
    f.runtime.execution.ppu.cpu.gpr[3] = UINT64_C(0xcafebabe11223344);
    if (invalid_owner) f.runtime.services.memory = nullptr;
    const ppu_context before = f.runtime.execution.ppu.cpu;
    BCHECK(!call_bridge(f, [&] { lv2_syscall(&f.runtime.execution.ppu.cpu); }));
    BCHECK(f.runtime.control->stop == (enabled && invalid_owner ? StartupStop::MissingServices : StartupStop::UnsupportedSyscall));
    BCHECK(f.runtime.control->syscall_number == 147);
    const uint64_t charged = enabled && !invalid_owner ? 1 : 0;
    BCHECK(f.policy.snapshot().hle == charged && f.clock.calls == charged);
    BCHECK(f.policy.snapshot().stop == StopReason::None);
    zero_effects(f, before);
}
#endif
}
int main() {
    const int controls = recorder_control_main();
    failures = 0;
#ifdef PS3_NATIVE_STARTUP_MEMORY
    combined_once(); entry_exhaustion(); unknown_entry(false); unknown_entry(true);
    rejected_entry(RejectedEntry::ForeignCpu); rejected_entry(RejectedEntry::InvalidOwner);
    rejected_entry(RejectedEntry::Cancelled); rejected_entry(RejectedEntry::LegacyDeadline);
    legacy_rejects(false); legacy_rejects(true); rollback_and_fixed_deadline();
    invalid_selection(InvalidSelection::UnconfiguredRecorder); invalid_selection(InvalidSelection::ActiveRecorder);
    invalid_selection(InvalidSelection::Trace); invalid_selection(InvalidSelection::Recorder);
    invalid_selection(InvalidSelection::Budget); invalid_selection(InvalidSelection::Clock); invalid_selection(InvalidSelection::UsedPolicy);
    reservation_capacity_rollback();
    clock_failure(ClockFailure::Provider); clock_failure(ClockFailure::Overflow); clock_failure(ClockFailure::Regression);
    latched_invalid_category(); generated_mapping(); full_strict_preflight();
    partial_poll_stop(false); partial_poll_stop(true); cancel_inside_clock(false); cancel_inside_clock(true);
#else
    feature_off(false, false); feature_off(false, true);
    feature_off(true, false); feature_off(true, true);
#endif
    std::printf("STARTUP_BUDGET_HOST cases=%d failures=%d recorder_controls=%d native_stub_proof=0 indirect_host_gate=uncovered\n",
        budget_cases, budget_failures + failures, controls);
    return controls || budget_failures || failures ? 1 : 0;
}
