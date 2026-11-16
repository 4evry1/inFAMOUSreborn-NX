/* Host actual bridge/domain and actual derived caller. Clock callbacks are
 * port fixtures, not native SDK or original-startup runtime evidence. */
#include "native_timebase_abi.h"
#include <initializer_list>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define main inherited_recorder_main
#include "test_startup_recorder.cpp"
#undef main
#ifndef TIMEBASE_DIRECT_ONLY
#ifndef TIMEBASE_ACTUAL_LEAF
#error "Timebase tests require the provenance-pinned actual derived leaf"
#endif
#include TIMEBASE_ACTUAL_LEAF
#endif

namespace {
int counter_cases, counter_checks, counter_failures;
#define CCHECK(value) do { ++counter_checks; if (!(value)) { ++counter_failures; std::printf("TIMEBASE_FAIL case=%d line=%d\n", counter_cases, __LINE__); } } while (0)
constexpr uint64_t RESULT_SENTINEL = UINT64_C(0xcafef00ddeadbeef);
struct GuestCounterClock {
    uint64_t microseconds = 5;
    ps3_clock_status status = PS3_CLOCK_OK;
    unsigned calls = 0;
    StartupControl *control = nullptr;
    bool cancel = false, legacy_deadline = false;
    static ps3_clock_status sample(void *p, uint64_t *output) {
        auto &self = *static_cast<GuestCounterClock *>(p);
        ++self.calls;
        if (self.cancel) self.control->cancelled.store(true);
        if (self.legacy_deadline) { host_tick = 1; self.control->deadline_tick = 1; }
        if (self.status == PS3_CLOCK_OK) *output = self.microseconds;
        return self.status;
    }
};
struct CounterFixture {
    Runtime runtime;
    GuestCounterClock own_clock;
    GuestCounterClock *guest;
    ps3_clock guest_port;
    unsigned operational_calls = 0, fail_at = 0, advance_at = 0;
    ps3_clock operational{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget{policy, operational};
    static ps3_clock_status sample(void *p, uint64_t *output) {
        auto &self = *static_cast<CounterFixture *>(p); ++self.operational_calls;
        if (self.operational_calls == self.fail_at) return PS3_CLOCK_ARGUMENT;
        *output = self.advance_at && self.operational_calls >= self.advance_at ? 2 : 0;
        return PS3_CLOCK_OK;
    }
    explicit CounterFixture(uint64_t generated = 10, uint64_t duration = UINT64_C(1000000000),
        GuestCounterClock *shared = nullptr)
        : guest(shared ? shared : &own_clock), guest_port{guest, GuestCounterClock::sample},
          policy({generated, 10, 10, duration}, 0) {
        CCHECK(runtime.recorder.configure(runtime.events, 4, PpuMemoryTraceRetention::Suffix));
        own_clock.control = runtime.control.get();
        runtime.services.clock = &guest_port;
        auto &cpu = runtime.execution.ppu.cpu;
        for (unsigned index = 0; index < 32; ++index) cpu.gpr[index] = UINT64_C(0xfeed000000000000) + index;
        cpu.gpr[1] = 0x10100; cpu.lr = UINT64_C(0x1122334455667788); cpu.cr = 0x12345678;
    }
    ~CounterFixture() {
        host_tick = 0;
        if (runtime.execution.ppu.admitted)
            CCHECK(native_startup_release_after_join(runtime.execution) == PS3_GUEST_MEMORY_OK);
    }
    bool admit() { return native_startup_admit_budgeted(runtime.execution, runtime.binding,
        runtime.services, runtime.recorder, budget); }
    template<class Operation> bool call(Operation operation) {
        terminal_recorder = &runtime.recorder; terminal_guard = &runtime.guard;
        terminal_closed = terminal_unlocked = false; active = runtime.control.get();
        bool returned = true;
        try { operation(); active = nullptr; }
        catch (const Terminal &) { returned = false; CCHECK(terminal_closed && terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
        return returned;
    }
    void unchanged(const ppu_context &before) {
        CCHECK(!std::memcmp(&before, &runtime.execution.ppu.cpu, sizeof(before)));
        CCHECK(runtime.probe.writes == 0 && runtime.recorder.report().published_count == 0 &&
            !runtime.recorder.report().batch_active && runtime.control->import_count == 0);
    }
};
void success(uint64_t us, uint64_t expected) {
    ++counter_cases; CounterFixture f; f.guest->microseconds = us;
    CCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    uint64_t result = RESULT_SENTINEL;
    CCHECK(f.call([&] { result = ppu_native_timebase_now(&f.runtime.execution.ppu.cpu); }));
    CCHECK(result == expected && f.guest->calls == 1 && f.operational_calls == 2);
    CCHECK(f.runtime.control->timebase_attempts == 1 && f.runtime.control->timebase_status == PS3_CLOCK_OK &&
        f.runtime.control->timebase_has_sample && f.runtime.control->timebase_last_microseconds == us &&
        f.runtime.control->timebase_last_ticks == expected && f.runtime.control->timebase_equal_samples == 0);
    CCHECK(f.policy.snapshot().generated == 1 && f.policy.snapshot().hle == 0 && f.policy.snapshot().indirect == 0);
    f.unchanged(before);
}
void shared_clock() {
    ++counter_cases; GuestCounterClock shared;
    CounterFixture first(10, UINT64_C(1000000000), &shared), second(10, UINT64_C(1000000000), &shared);
    CCHECK(first.admit() && second.admit());
    uint64_t a = RESULT_SENTINEL, b = RESULT_SENTINEL, again = RESULT_SENTINEL;
    CCHECK(first.call([&] { a = ppu_native_timebase_now(&first.runtime.execution.ppu.cpu); }));
    CCHECK(second.call([&] { b = ppu_native_timebase_now(&second.runtime.execution.ppu.cpu); }));
    CCHECK(first.call([&] { again = ppu_native_timebase_now(&first.runtime.execution.ppu.cpu); }));
    CCHECK(a == 399 && b == a && again == a && shared.calls == 3);
    CCHECK(first.runtime.control->timebase_equal_samples == 1 && second.runtime.control->timebase_equal_samples == 0);
}
enum class Denial { NullCpu, ForeignCpu, InvalidOwner, ForeignControl, MissingClock, MissingCallback,
    ProviderFailure, CancelEntry, LegacyDeadline, Quota, ClockAdmission, DeadlineAdmission,
    CancelProvider, DeadlineProvider, ClockPoll, DeadlinePoll };
void denied(Denial kind) {
    ++counter_cases; CounterFixture f(kind == Denial::Quota ? 0 : 10,
        kind == Denial::DeadlineAdmission ? 0 : kind == Denial::DeadlinePoll ? 1000 : UINT64_C(1000000000));
    CCHECK(f.admit());
    ppu_context foreign{};
    auto control = std::make_unique<StartupControl>();
    if (kind == Denial::InvalidOwner) f.runtime.services.memory = nullptr;
    if (kind == Denial::ForeignControl) f.runtime.execution.control = control.get();
    if (kind == Denial::MissingClock) f.runtime.services.clock = nullptr;
    if (kind == Denial::MissingCallback) f.guest_port.now_microseconds = nullptr;
    if (kind == Denial::ProviderFailure) f.guest->status = PS3_CLOCK_ARGUMENT;
    if (kind == Denial::CancelEntry) f.runtime.control->cancelled.store(true);
    if (kind == Denial::LegacyDeadline) { host_tick = 1; f.runtime.control->deadline_tick = 1; }
    if (kind == Denial::ClockAdmission) f.fail_at = 1;
    if (kind == Denial::ClockPoll) f.fail_at = 2;
    if (kind == Denial::DeadlinePoll) f.advance_at = 2;
    if (kind == Denial::CancelProvider) f.guest->cancel = true;
    if (kind == Denial::DeadlineProvider) f.guest->legacy_deadline = true;
    const auto before = f.runtime.execution.ppu.cpu;
    uint64_t result = RESULT_SENTINEL;
    CCHECK(!f.call([&] { result = ppu_native_timebase_now(kind == Denial::NullCpu ? nullptr :
        kind == Denial::ForeignCpu ? &foreign : &f.runtime.execution.ppu.cpu); }));
    StartupStop expected = StartupStop::MissingServices;
    if (kind == Denial::ProviderFailure) expected = StartupStop::ClockProviderFailure;
    if (kind == Denial::CancelEntry || kind == Denial::CancelProvider) expected = StartupStop::Cancelled;
    if (kind == Denial::LegacyDeadline || kind == Denial::DeadlineAdmission || kind == Denial::DeadlineProvider || kind == Denial::DeadlinePoll) expected = StartupStop::Deadline;
    if (kind == Denial::Quota) expected = StartupStop::GeneratedBudget;
    if (kind == Denial::ClockAdmission || kind == Denial::ClockPoll) expected = StartupStop::BudgetClockFailure;
    CCHECK(f.runtime.control->stop == expected && result == RESULT_SENTINEL);
    const bool sampled = kind == Denial::ProviderFailure || kind == Denial::CancelProvider ||
        kind == Denial::DeadlineProvider || kind == Denial::ClockPoll || kind == Denial::DeadlinePoll;
    CCHECK(f.guest->calls == unsigned(sampled) && f.runtime.control->timebase_attempts == size_t(sampled));
    const bool charged = sampled || kind == Denial::MissingClock || kind == Denial::MissingCallback;
    CCHECK(f.policy.snapshot().generated == uint64_t(charged) && f.policy.snapshot().hle == 0 && f.policy.snapshot().indirect == 0);
    CCHECK(!std::memcmp(&before, &f.runtime.control->terminal, sizeof(before)));
    f.unchanged(before);
    f.runtime.execution.control = f.runtime.control.get();
}
#ifndef TIMEBASE_DIRECT_ONLY
void actual_caller(unsigned scenario) {
    ++counter_cases; CounterFixture f(scenario == 1 ? 1 : 10);
    if (scenario == 0) f.guest->status = PS3_CLOCK_ARGUMENT;
    if (scenario == 2) f.guest->cancel = true;
    CCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu;
    expected.gpr[0] = expected.lr;
    CCHECK(!f.call([&] { func_004427E8(&f.runtime.execution.ppu.cpu); }));
    CCHECK(f.runtime.control->stop == (scenario == 0 ? StartupStop::ClockProviderFailure :
        scenario == 1 ? StartupStop::GeneratedBudget : StartupStop::Cancelled));
    CCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)) &&
        !std::memcmp(&expected, &f.runtime.control->terminal, sizeof(expected)));
    CCHECK(f.runtime.bytes(uint32_t(expected.gpr[1] + 0x10)) == expected.lr && f.runtime.probe.writes == 1);
    CCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    PpuMemoryTraceSample sample{};
    CCHECK(f.runtime.recorder.read(0, sample));
    CCHECK(sample.sequence == 0 && sample.event.origin == PpuMemoryTraceOrigin::Generated &&
        sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == expected.gpr[1] + 0x10 &&
        sample.event.length == 8 && sample.event.value == expected.lr && !sample.event.stored && !sample.event.byte_transfer);
    CCHECK(f.policy.snapshot().generated == (scenario == 1 ? 1u : 2u) && f.policy.snapshot().hle == 0);
    CCHECK(f.guest->calls == (scenario == 1 ? 0u : 1u));
}
void actual_caller_zero_return() {
    ++counter_cases; CounterFixture f(2);
    f.guest->microseconds = 0;
    CCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu;
    // The real leaf consumes zero, updates its signed-low32 compare CR nibble,
    // and loops. The next helper admission exhausts the two-operation budget.
    expected.gpr[0] = 0;
    expected.cr = (expected.cr & ~UINT32_C(0xf)) | UINT32_C(2);
    CCHECK(!f.call([&] { func_004427E8(&f.runtime.execution.ppu.cpu); }));
    CCHECK(f.runtime.control->stop == StartupStop::GeneratedBudget &&
        f.policy.snapshot().stop == infamous::discovery::StopReason::GeneratedExhausted);
    CCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)) &&
        !std::memcmp(&expected, &f.runtime.control->terminal, sizeof(expected)));
    CCHECK(f.runtime.bytes(uint32_t(expected.gpr[1] + 0x10)) == expected.lr && f.runtime.probe.writes == 1);
    CCHECK(f.policy.snapshot().generated == 2 && f.policy.snapshot().hle == 0 && f.policy.snapshot().indirect == 0);
    CCHECK(f.guest->calls == 1 && f.runtime.control->timebase_attempts == 1 &&
        f.runtime.control->timebase_status == PS3_CLOCK_OK && f.runtime.control->timebase_has_sample &&
        f.runtime.control->timebase_last_microseconds == 0 && f.runtime.control->timebase_last_ticks == 0);
    CCHECK(f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    PpuMemoryTraceSample sample{};
    CCHECK(f.runtime.recorder.read(0, sample));
    CCHECK(sample.sequence == 0 && sample.event.origin == PpuMemoryTraceOrigin::Generated &&
        sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == expected.gpr[1] + 0x10 &&
        sample.event.length == 8 && sample.event.value == expected.lr && !sample.event.stored && !sample.event.byte_transfer);
}
void actual_caller_complete_return() {
    ++counter_cases; CounterFixture f;
    f.guest->microseconds = 5;
    f.runtime.execution.ppu.cpu.gpr[3] = 100;
    CCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu;
    expected.gpr[0] = expected.lr;
    expected.gpr[9] = UINT64_C(100) - UINT64_C(399);
    expected.cr = (expected.cr & ~UINT32_C(0xf)) | UINT32_C(4);
    CCHECK(f.call([&] { func_004427E8(&f.runtime.execution.ppu.cpu); }));
    CCHECK(f.runtime.control->stop == StartupStop::None);
    CCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    CCHECK(f.policy.snapshot().generated == 3 && f.policy.snapshot().hle == 0 && f.policy.snapshot().indirect == 0);
    CCHECK(f.guest->calls == 1 && f.runtime.control->timebase_attempts == 1 &&
        f.runtime.control->timebase_status == PS3_CLOCK_OK && f.runtime.control->timebase_last_microseconds == 5 &&
        f.runtime.control->timebase_last_ticks == 399);
    CCHECK(f.runtime.probe.writes == 1 && f.runtime.recorder.report().published_count == 2 && !f.runtime.recorder.report().batch_active);
    CCHECK(f.runtime.bytes(uint32_t(expected.gpr[1] + 0x10)) == expected.lr);
    for (size_t index = 0; index < 2; ++index) {
        PpuMemoryTraceSample sample{};
        CCHECK(f.runtime.recorder.read(index, sample));
        CCHECK(sample.sequence == index && sample.event.origin == PpuMemoryTraceOrigin::Generated &&
            sample.event.operation == (index ? PpuMemoryTraceOperation::Read : PpuMemoryTraceOperation::Write) &&
            sample.event.address == expected.gpr[1] + 0x10 && sample.event.length == 8 && sample.event.value == expected.lr &&
            sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored && !sample.event.byte_transfer);
    }
}
#endif
void no_worker_abort(bool control_without_cpu) {
    ++counter_cases;
    auto empty = std::make_unique<StartupControl>();
    const pid_t child = fork();
    CCHECK(child >= 0);
    if (child == 0) { active = control_without_cpu ? empty.get() : nullptr; ppu_native_timebase_now(nullptr); _exit(42); }
    int status = 0;
    if (child > 0) { CCHECK(waitpid(child, &status, 0) == child); CCHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT); }
}
}
int main() {
    success(0, 0); success(1, 79); success(4, 319); success(5, 399);
    success(UINT64_C(4294967301), UINT64_C(342738390619));
    success(UINT64_MAX, UINT64_C(14757395258967641213));
    shared_clock();
    for (Denial kind : {Denial::NullCpu, Denial::ForeignCpu, Denial::InvalidOwner, Denial::ForeignControl,
        Denial::MissingClock, Denial::MissingCallback, Denial::ProviderFailure, Denial::CancelEntry,
        Denial::LegacyDeadline, Denial::Quota, Denial::ClockAdmission, Denial::DeadlineAdmission,
        Denial::CancelProvider, Denial::DeadlineProvider, Denial::ClockPoll, Denial::DeadlinePoll}) denied(kind);
#ifndef TIMEBASE_DIRECT_ONLY
    actual_caller(0); actual_caller(1); actual_caller(2);
    actual_caller_zero_return();
    actual_caller_complete_return();
    constexpr const char *caller = "func_004427E8";
#else
    constexpr const char *caller = "pending_explicit_direct_only";
#endif
    no_worker_abort(false); no_worker_abort(true);
    std::printf("Timebase bridge host cases=%d checks=%d failures=%d inherited_failures=%d actual_derived_caller=%s sdk_proof=false\n", counter_cases, counter_checks, counter_failures, failures, caller);
    return counter_failures || failures ? 1 : 0;
}
