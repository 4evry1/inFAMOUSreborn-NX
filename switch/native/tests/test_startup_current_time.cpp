/* Actual host bridge + mmap memory/coherent domain. Calendar callbacks below
 * are port fixtures; this is not native SDK or generated-call-site proof. */
#include "calendar_clock.h"
#include <initializer_list>
#define main inherited_recorder_main
#include "test_startup_recorder.cpp"
#undef main

namespace {
int current_cases, current_checks, current_failures;
#define TCHECK(value) do { ++current_checks; if (!(value)) { ++current_failures; std::printf("CURRENT_TIME_FAIL case=%d line=%d\n", current_cases, __LINE__); } } while (0)
struct CalendarFixture {
    ps3_calendar_time value{INT64_C(1700000001), 123456789};
    ps3_calendar_clock_status status = PS3_CALENDAR_CLOCK_OK;
    unsigned calls = 0;
    StartupControl *control = nullptr;
    bool cancel_after_sample = false, deadline_after_sample = false;
    static ps3_calendar_clock_status sample(void *p, ps3_calendar_time *output) {
        auto &self = *static_cast<CalendarFixture *>(p);
        ++self.calls;
        if (self.cancel_after_sample) self.control->cancelled.store(true);
        if (self.deadline_after_sample) { host_tick = 1; self.control->deadline_tick = 1; }
        if (self.status == PS3_CALENDAR_CLOCK_OK) *output = self.value;
        return self.status;
    }
};
struct CurrentFixture {
    Runtime runtime;
    CalendarFixture calendar;
    ps3_calendar_clock port{&calendar, CalendarFixture::sample};
    unsigned operational_calls = 0, operational_fail_at = 0, operational_advance_at = 0;
    ps3_clock operational{this, sample_operational};
    static ps3_clock_status sample_operational(void *p, uint64_t *output) {
        auto &self = *static_cast<CurrentFixture *>(p);
        ++self.operational_calls;
        if (self.operational_calls == self.operational_fail_at) return PS3_CLOCK_ARGUMENT;
        *output = self.operational_advance_at && self.operational_calls >= self.operational_advance_at ? 2 : 0;
        return PS3_CLOCK_OK;
    }
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget{policy, operational};
    explicit CurrentFixture(uint64_t hle_limit = 10, bool strict_one = false,
        uint64_t duration_ns = UINT64_C(1000000000))
        : policy({10, hle_limit, 10, duration_ns}, 0) {
        TCHECK(runtime.recorder.configure(runtime.events, strict_one ? 1 : 4,
            strict_one ? PpuMemoryTraceRetention::Strict : PpuMemoryTraceRetention::Suffix));
        calendar.control = runtime.control.get();
        runtime.execution.calendar_clock = &port;
        runtime.execution.ppu.cpu.gpr[11] = 145;
        runtime.execution.ppu.cpu.gpr[3] = 0x10200;
        runtime.execution.ppu.cpu.gpr[4] = 0x10210;
    }
    ~CurrentFixture() {
        host_tick = 0;
        if (runtime.execution.ppu.admitted)
            TCHECK(native_startup_release_after_join(runtime.execution) == PS3_GUEST_MEMORY_OK);
    }
    bool admit() {
        return native_startup_admit_budgeted(runtime.execution, runtime.binding,
            runtime.services, runtime.recorder, budget);
    }
    bool call() {
        terminal_recorder = &runtime.recorder; terminal_guard = &runtime.guard;
        terminal_closed = terminal_unlocked = false;
        active = runtime.control.get();
        bool returned = true;
        try { lv2_syscall(&runtime.execution.ppu.cpu); active = nullptr; }
        catch (const Terminal &) { returned = false; TCHECK(terminal_closed && terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
        return returned;
    }
    void event(size_t index, uint32_t address, uint64_t value) {
        PpuMemoryTraceSample sample{};
        TCHECK(runtime.recorder.read(index, sample));
        TCHECK(sample.sequence == index && sample.event.origin == PpuMemoryTraceOrigin::Hle &&
            sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == address &&
            sample.event.length == 8 && sample.event.value == value && sample.event.status == PS3_GUEST_MEMORY_OK &&
            !sample.event.stored && !sample.event.byte_transfer);
    }
    void unchanged(const ppu_context &before) {
        TCHECK(!std::memcmp(&before, &runtime.execution.ppu.cpu, sizeof(before)));
        TCHECK(runtime.probe.writes == 0 && runtime.recorder.report().published_count == 0);
        TCHECK(!runtime.recorder.report().batch_active);
    }
};
#ifndef CURRENT_TIME_FEATURE_OFF
void literal_success(int64_t seconds, uint32_t nanoseconds) {
    ++current_cases; CurrentFixture f;
    f.calendar.value = {seconds, nanoseconds};
    TCHECK(!f.runtime.services.allocate_memory && !f.runtime.services.free_memory && !f.runtime.services.query_memory);
    TCHECK(f.admit());
    auto expected = f.runtime.execution.ppu.cpu; expected.gpr[3] = 0;
    TCHECK(f.call());
    TCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    TCHECK(f.calendar.calls == 1 && f.runtime.control->calendar_calls == 1 &&
        f.runtime.control->calendar_status == PS3_CALENDAR_CLOCK_OK);
    TCHECK(f.runtime.control->calendar_sample.seconds == seconds && f.runtime.control->calendar_sample.nanoseconds == nanoseconds);
    TCHECK(f.runtime.bytes(0x10200) == uint64_t(seconds) && f.runtime.bytes(0x10210) == nanoseconds);
    unsigned char encoded[8]{};
    TCHECK(f.runtime.binding.coherent_memory.read(f.runtime.binding.coherent_memory.context, 0x10210, encoded, 8) == PS3_GUEST_MEMORY_OK);
    const unsigned char literal_nanos[8]{0, 0, 0, 0, static_cast<unsigned char>(nanoseconds >> 24),
        static_cast<unsigned char>(nanoseconds >> 16), static_cast<unsigned char>(nanoseconds >> 8), static_cast<unsigned char>(nanoseconds)};
    TCHECK(!std::memcmp(encoded, literal_nanos, 8));
    TCHECK(f.runtime.bytes(0x10128) == 0 && f.runtime.probe.writes == 2);
    TCHECK(f.policy.snapshot().hle == 1 && f.policy.snapshot().generated == 0 && f.policy.snapshot().indirect == 0);
    TCHECK(f.operational_calls == 5 && f.runtime.recorder.report().published_count == 2 && f.runtime.control->import_count == 0);
    f.event(0, 0x10200, uint64_t(seconds)); f.event(1, 0x10210, nanoseconds);
}
void overlap(bool identical) {
    ++current_cases; CurrentFixture f;
    f.calendar.value = {INT64_C(0x0102030405060708), 0x12345678};
    f.runtime.execution.ppu.cpu.gpr[4] = identical ? 0x10200 : 0x10204;
    TCHECK(f.admit() && f.call());
    f.event(0, 0x10200, UINT64_C(0x0102030405060708));
    f.event(1, identical ? 0x10200 : 0x10204, UINT64_C(0x12345678));
    if (identical) TCHECK(f.runtime.bytes(0x10200) == UINT64_C(0x12345678));
    else {
        const unsigned char literal[12]{1, 2, 3, 4, 0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
        unsigned char actual[12]{};
        TCHECK(f.runtime.binding.coherent_memory.read(f.runtime.binding.coherent_memory.context, 0x10200, actual, 12) == PS3_GUEST_MEMORY_OK);
        TCHECK(!std::memcmp(actual, literal, 12));
    }
}
enum class Rejection { NullSeconds, NullNanos, HighSeconds, HighNanos, OverflowSpan, Unmapped,
    ReadOnly, MissingPort, MissingCallback, ProviderFailure, InvalidSample, Cancelled,
    Quota, TraceFull, CancelRead, BudgetClockProbe, DeadlineProbe, CancelSample, DeadlineSample, SyscallAlias };
void reject(Rejection kind) {
    ++current_cases;
    CurrentFixture f(kind == Rejection::Quota ? 0 : 10, kind == Rejection::TraceFull,
        kind == Rejection::DeadlineProbe ? 1000 : UINT64_C(1000000000));
    if (kind == Rejection::NullSeconds) f.runtime.execution.ppu.cpu.gpr[3] = 0;
    if (kind == Rejection::NullNanos) f.runtime.execution.ppu.cpu.gpr[4] = 0;
    if (kind == Rejection::HighSeconds) f.runtime.execution.ppu.cpu.gpr[3] |= UINT64_C(0x100000000);
    if (kind == Rejection::HighNanos) f.runtime.execution.ppu.cpu.gpr[4] |= UINT64_C(0x100000000);
    if (kind == Rejection::OverflowSpan) f.runtime.execution.ppu.cpu.gpr[4] = UINT32_MAX - 3;
    if (kind == Rejection::Unmapped) {
        f.runtime.execution.ppu.cpu.gpr[4] = 0x11000;
        const auto &memory = f.runtime.binding.coherent_memory;
        TCHECK(memory.decommit(memory.context, 0x11000, 4096) == PS3_GUEST_MEMORY_OK);
    }
    if (kind == Rejection::ReadOnly) f.runtime.ram.writable = false;
    if (kind == Rejection::MissingPort) f.runtime.execution.calendar_clock = nullptr;
    if (kind == Rejection::MissingCallback) f.port.sample = nullptr;
    if (kind == Rejection::ProviderFailure) f.calendar.status = PS3_CALENDAR_CLOCK_PROVIDER_FAILURE;
    if (kind == Rejection::InvalidSample) f.calendar.value.nanoseconds = 1000000000;
    if (kind == Rejection::Cancelled) f.runtime.control->cancelled.store(true);
    if (kind == Rejection::CancelRead) f.runtime.probe.cancel_read = true;
    if (kind == Rejection::BudgetClockProbe) f.operational_fail_at = 2;
    if (kind == Rejection::DeadlineProbe) f.operational_advance_at = 2;
    if (kind == Rejection::CancelSample) f.calendar.cancel_after_sample = true;
    if (kind == Rejection::DeadlineSample) f.calendar.deadline_after_sample = true;
    if (kind == Rejection::SyscallAlias) f.runtime.execution.ppu.cpu.gpr[11] |= UINT64_C(0x100000000);
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    StartupStop expected = StartupStop::InvalidMemory;
    if (kind == Rejection::MissingPort || kind == Rejection::MissingCallback) expected = StartupStop::MissingServices;
    if (kind == Rejection::ProviderFailure || kind == Rejection::InvalidSample) expected = StartupStop::ClockProviderFailure;
    if (kind == Rejection::Cancelled || kind == Rejection::CancelRead || kind == Rejection::CancelSample) expected = StartupStop::Cancelled;
    if (kind == Rejection::Quota) expected = StartupStop::HleSyscallBudget;
    if (kind == Rejection::TraceFull) expected = StartupStop::TraceCapacity;
    if (kind == Rejection::DeadlineSample || kind == Rejection::DeadlineProbe) expected = StartupStop::Deadline;
    if (kind == Rejection::BudgetClockProbe) expected = StartupStop::BudgetClockFailure;
    if (kind == Rejection::SyscallAlias) expected = StartupStop::UnsupportedSyscall;
    TCHECK(f.runtime.control->stop == expected);
    const bool sampled = kind == Rejection::ProviderFailure || kind == Rejection::InvalidSample ||
        kind == Rejection::CancelSample || kind == Rejection::DeadlineSample;
    TCHECK(f.calendar.calls == unsigned(sampled) && f.runtime.control->calendar_calls == size_t(sampled));
    TCHECK(f.policy.snapshot().hle == ((kind == Rejection::Cancelled || kind == Rejection::Quota) ? 0u : 1u));
    if (kind == Rejection::ProviderFailure) TCHECK(f.runtime.control->calendar_status == PS3_CALENDAR_CLOCK_PROVIDER_FAILURE);
    if (kind == Rejection::InvalidSample) TCHECK(f.runtime.control->calendar_status == PS3_CALENDAR_CLOCK_INVALID_SAMPLE);
    if (kind == Rejection::CancelRead || kind == Rejection::BudgetClockProbe || kind == Rejection::DeadlineProbe) {
        TCHECK(f.runtime.probe.reads == 1);
        TCHECK(f.operational_calls == (kind == Rejection::CancelRead ? 1u : 2u));
    }
    if (kind == Rejection::BudgetClockProbe) TCHECK(f.policy.snapshot().stop == infamous::discovery::StopReason::InvalidClock);
    if (kind == Rejection::DeadlineProbe) TCHECK(f.policy.snapshot().stop == infamous::discovery::StopReason::Deadline);
    f.unchanged(before);
}
void partial(bool fail_second) {
    ++current_cases; CurrentFixture f;
    f.runtime.execution.ppu.cpu.gpr[4] = 0x10128;
    f.runtime.probe.fail_toc = fail_second;
    f.runtime.probe.cancel_output = !fail_second;
    TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    TCHECK(!f.call());
    TCHECK(f.runtime.control->stop == (fail_second ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    TCHECK(f.runtime.bytes(0x10200) == UINT64_C(1700000001) && f.runtime.bytes(0x10128) == 0);
    TCHECK(f.runtime.probe.writes == 1 && f.runtime.recorder.report().published_count == 1 && !f.runtime.recorder.report().batch_active);
    TCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)));
    TCHECK(f.runtime.control->calendar_status == PS3_CALENDAR_CLOCK_OK && f.runtime.control->calendar_calls == 1);
    f.event(0, 0x10200, UINT64_C(1700000001));
}
void reservation(bool same_granule) {
    ++current_cases; CurrentFixture f;
    TCHECK(f.admit());
    const uint32_t reserved = same_granule ? 0x10240 : 0x10300;
    auto *cpu = &f.runtime.execution.ppu.cpu;
    TCHECK(ppu_native_load_reserve32(cpu, reserved) == 0);
    TCHECK(f.call());
    const bool stored = ppu_native_store_conditional32(cpu, reserved, 0x12345678);
    TCHECK(stored == !same_granule);
    unsigned char bytes[4]{};
    TCHECK(f.runtime.binding.coherent_memory.read(f.runtime.binding.coherent_memory.context, reserved, bytes, 4) == PS3_GUEST_MEMORY_OK);
    const unsigned char success[4]{0x12, 0x34, 0x56, 0x78}, unchanged[4]{};
    TCHECK(!std::memcmp(bytes, same_granule ? unchanged : success, 4));
    TCHECK(f.policy.snapshot().generated == 2 && f.policy.snapshot().hle == 1);
    TCHECK(!f.runtime.recorder.report().batch_active);
}
#endif
}
int main() {
#ifdef CURRENT_TIME_FEATURE_OFF
    {
        ++current_cases; CurrentFixture f;
        // The legacy memory-family gate requires these capabilities before
        // rejecting145. Their invocation would be an unexpected test failure.
        f.runtime.services.allocate_memory = [](void *, uint64_t, uint64_t, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.free_memory = [](void *, uint32_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.query_memory = [](void *, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        TCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
        TCHECK(!f.call() && f.runtime.control->stop == StartupStop::UnsupportedSyscall);
        TCHECK(f.calendar.calls == 0 && f.policy.snapshot().hle == 1); f.unchanged(before);
    }
#else
    literal_success(INT64_C(1700000001), 123456789);
    literal_success(INT64_C(0x123456789abc), 999999999);
    literal_success(-1, 0);
    overlap(false); overlap(true);
    for (Rejection kind : {Rejection::NullSeconds, Rejection::NullNanos, Rejection::HighSeconds,
        Rejection::HighNanos, Rejection::OverflowSpan, Rejection::Unmapped, Rejection::ReadOnly,
        Rejection::MissingPort, Rejection::MissingCallback, Rejection::ProviderFailure,
        Rejection::InvalidSample, Rejection::Cancelled, Rejection::Quota, Rejection::TraceFull,
        Rejection::CancelRead, Rejection::BudgetClockProbe, Rejection::DeadlineProbe,
        Rejection::CancelSample, Rejection::DeadlineSample, Rejection::SyscallAlias}) reject(kind);
    partial(false); partial(true);
    reservation(true); reservation(false);
#endif
    std::printf("Current time host cases=%d checks=%d failures=%d inherited_failures=%d native_sdk_proof=false\n", current_cases, current_checks, current_failures, failures);
    return current_failures || failures ? 1 : 0;
}
