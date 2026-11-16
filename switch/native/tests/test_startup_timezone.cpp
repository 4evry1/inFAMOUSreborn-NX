/* Actual host bridge, mmap space and coherent domain. The selected fixed
 * virtual-console policy is not evidence of Switch timezone detection. */
#include <initializer_list>
#define main inherited_recorder_main
#include "test_startup_recorder.cpp"
#undef main

namespace {
int timezone_cases, timezone_checks, timezone_failures;
#define ZCHECK(value) do { ++timezone_checks; if (!(value)) { ++timezone_failures; std::printf("TIMEZONE_FAIL case=%d line=%d\n", timezone_cases, __LINE__); } } while (0)
struct TimezoneFixture {
    Runtime runtime;
    const StartupFixedTimezone selected;
    unsigned operational_calls = 0, fail_at = 0, advance_at = 0;
    ps3_clock operational{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget{policy, operational};
    static ps3_clock_status sample(void *opaque, uint64_t *output) {
        auto &self = *static_cast<TimezoneFixture *>(opaque);
        ++self.operational_calls;
        if (self.operational_calls == self.fail_at) return PS3_CLOCK_ARGUMENT;
        *output = self.advance_at && self.operational_calls >= self.advance_at ? 2 : 0;
        return PS3_CLOCK_OK;
    }
    explicit TimezoneFixture(int32_t base = -300, int32_t adjustment = 60,
        uint64_t hle_limit = 10, bool strict_one = false, uint64_t duration = UINT64_C(1000000000))
        : selected{base, adjustment}, policy({10, hle_limit, 10, duration}, 0) {
        ZCHECK(runtime.recorder.configure(runtime.events, strict_one ? 1 : 4,
            strict_one ? PpuMemoryTraceRetention::Strict : PpuMemoryTraceRetention::Suffix));
        runtime.execution.fixed_timezone = &selected;
        runtime.execution.ppu.cpu.gpr[11] = 144;
        runtime.execution.ppu.cpu.gpr[3] = 0x10200;
        runtime.execution.ppu.cpu.gpr[4] = 0x10210;
        const unsigned char sentinels[4]{0x11, 0x22, 0x33, 0x44};
        const unsigned char second[4]{0x55, 0x66, 0x77, 0x88};
        const auto &memory = runtime.binding.coherent_memory;
        ZCHECK(memory.write(memory.context, 0x10200, sentinels, 4) == PS3_GUEST_MEMORY_OK);
        ZCHECK(memory.write(memory.context, 0x10210, second, 4) == PS3_GUEST_MEMORY_OK);
        runtime.probe.writes = 0;
    }
    ~TimezoneFixture() {
        host_tick = 0;
        if (runtime.execution.ppu.admitted)
            ZCHECK(native_startup_release_after_join(runtime.execution) == PS3_GUEST_MEMORY_OK);
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
        catch (const Terminal &) { returned = false; ZCHECK(terminal_closed && terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
        return returned;
    }
    uint32_t value(uint32_t address) {
        unsigned char bytes[4]{};
        ZCHECK(runtime.binding.coherent_memory.read(runtime.binding.coherent_memory.context, address, bytes, 4) == PS3_GUEST_MEMORY_OK);
        return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
    }
    void event(size_t index, uint32_t address, uint32_t expected) {
        PpuMemoryTraceSample sample{};
        ZCHECK(runtime.recorder.read(index, sample));
        ZCHECK(sample.sequence == index && sample.event.origin == PpuMemoryTraceOrigin::Hle &&
            sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == address &&
            sample.event.length == 4 && sample.event.value == expected && sample.event.status == PS3_GUEST_MEMORY_OK &&
            !sample.event.stored && !sample.event.byte_transfer);
    }
    void unchanged(const ppu_context &before) {
        ZCHECK(!std::memcmp(&before, &runtime.execution.ppu.cpu, sizeof(before)));
        ZCHECK(runtime.probe.writes == 0 && runtime.recorder.report().published_count == 0 && !runtime.recorder.report().batch_active);
        ZCHECK(value(0x10200) == UINT32_C(0x11223344) && value(0x10210) == UINT32_C(0x55667788));
    }
};
#ifndef TIMEZONE_FEATURE_OFF
void success(int32_t base, int32_t adjustment) {
    ++timezone_cases; TimezoneFixture f(base, adjustment);
    ZCHECK(!f.runtime.services.allocate_memory && !f.runtime.services.free_memory && !f.runtime.services.query_memory &&
        !f.runtime.services.clock && !f.runtime.execution.calendar_clock);
    ZCHECK(f.admit()); auto expected = f.runtime.execution.ppu.cpu; expected.gpr[3] = 0;
    ZCHECK(f.call());
    ZCHECK(!std::memcmp(&expected, &f.runtime.execution.ppu.cpu, sizeof(expected)));
    ZCHECK(f.value(0x10200) == uint32_t(base) && f.value(0x10210) == uint32_t(adjustment));
    ZCHECK(f.runtime.bytes(0x10128) == 0 && f.runtime.probe.writes == 2);
    ZCHECK(f.runtime.control->timezone_calls == 1 && f.runtime.control->timezone_minutes == base &&
        f.runtime.control->summertime_minutes == adjustment);
    ZCHECK(f.operational_calls == 4 && f.policy.snapshot().hle == 1 && f.policy.snapshot().generated == 0 && f.policy.snapshot().indirect == 0);
    ZCHECK(f.runtime.control->calendar_calls == 0 && f.runtime.control->import_count == 0);
    f.event(0, 0x10200, uint32_t(base)); f.event(1, 0x10210, uint32_t(adjustment));
}
void overlap(bool identical) {
    ++timezone_cases; TimezoneFixture f(0x01020304, 0x11223344);
    f.runtime.execution.ppu.cpu.gpr[4] = identical ? 0x10200 : 0x10202;
    ZCHECK(f.admit() && f.call());
    f.event(0, 0x10200, 0x01020304); f.event(1, identical ? 0x10200 : 0x10202, 0x11223344);
    if (identical) ZCHECK(f.value(0x10200) == 0x11223344);
    else {
        const unsigned char expected[6]{1, 2, 0x11, 0x22, 0x33, 0x44};
        unsigned char actual[6]{};
        ZCHECK(f.runtime.binding.coherent_memory.read(f.runtime.binding.coherent_memory.context, 0x10200, actual, 6) == PS3_GUEST_MEMORY_OK);
        ZCHECK(!std::memcmp(actual, expected, 6));
    }
}
enum class Denial { Missing, NullFirst, NullSecond, HighFirst, HighSecond, Wrap, ReadOnly,
    UnmappedFirst, UnmappedSecond, Quota, TraceFull, CancelEntry, CancelProbe, CancelSecondProbe,
    ClockProbe, ClockSecondProbe, DeadlineProbe, DeadlineSecondProbe, Alias };
void denied(Denial kind) {
    ++timezone_cases; TimezoneFixture f(-300, 60, kind == Denial::Quota ? 0 : 10,
        kind == Denial::TraceFull, kind == Denial::DeadlineProbe || kind == Denial::DeadlineSecondProbe ? 1000 : UINT64_C(1000000000));
    auto &cpu = f.runtime.execution.ppu.cpu;
    if (kind == Denial::Missing) f.runtime.execution.fixed_timezone = nullptr;
    if (kind == Denial::NullFirst) cpu.gpr[3] = 0;
    if (kind == Denial::NullSecond) cpu.gpr[4] = 0;
    if (kind == Denial::HighFirst) cpu.gpr[3] |= UINT64_C(0x100000000);
    if (kind == Denial::HighSecond) cpu.gpr[4] |= UINT64_C(0x100000000);
    if (kind == Denial::Wrap) cpu.gpr[4] = UINT32_MAX - 1;
    if (kind == Denial::ReadOnly) f.runtime.ram.writable = false;
    if (kind == Denial::UnmappedFirst || kind == Denial::UnmappedSecond) {
        cpu.gpr[kind == Denial::UnmappedFirst ? 3 : 4] = 0x11000;
        const auto &memory = f.runtime.binding.coherent_memory;
        ZCHECK(memory.decommit(memory.context, 0x11000, 4096) == PS3_GUEST_MEMORY_OK);
    }
    if (kind == Denial::CancelEntry) f.runtime.control->cancelled.store(true);
    if (kind == Denial::CancelProbe) f.runtime.probe.cancel_read = true;
    if (kind == Denial::CancelSecondProbe) { cpu.gpr[3] = 0x10210; cpu.gpr[4] = 0x10200; f.runtime.probe.cancel_read = true; }
    if (kind == Denial::ClockProbe) f.fail_at = 2;
    if (kind == Denial::ClockSecondProbe) f.fail_at = 3;
    if (kind == Denial::DeadlineProbe) f.advance_at = 2;
    if (kind == Denial::DeadlineSecondProbe) f.advance_at = 3;
    if (kind == Denial::Alias) cpu.gpr[11] |= UINT64_C(0x100000000);
    ZCHECK(f.admit()); const auto before = cpu;
    ZCHECK(!f.call());
    StartupStop expected = StartupStop::InvalidMemory;
    if (kind == Denial::Missing) expected = StartupStop::MissingServices;
    if (kind == Denial::Quota) expected = StartupStop::HleSyscallBudget;
    if (kind == Denial::TraceFull) expected = StartupStop::TraceCapacity;
    if (kind == Denial::CancelEntry || kind == Denial::CancelProbe || kind == Denial::CancelSecondProbe) expected = StartupStop::Cancelled;
    if (kind == Denial::ClockProbe || kind == Denial::ClockSecondProbe) expected = StartupStop::BudgetClockFailure;
    if (kind == Denial::DeadlineProbe || kind == Denial::DeadlineSecondProbe) expected = StartupStop::Deadline;
    if (kind == Denial::Alias) expected = StartupStop::UnsupportedSyscall;
    ZCHECK(f.runtime.control->stop == expected && f.runtime.control->timezone_calls == 0);
    ZCHECK(f.policy.snapshot().hle == ((kind == Denial::CancelEntry || kind == Denial::Quota) ? 0u : 1u));
    if (kind == Denial::CancelProbe || kind == Denial::ClockProbe || kind == Denial::DeadlineProbe)
        ZCHECK(f.runtime.probe.reads == 1);
    if (kind == Denial::CancelSecondProbe || kind == Denial::ClockSecondProbe || kind == Denial::DeadlineSecondProbe)
        ZCHECK(f.runtime.probe.reads == 2);
    f.unchanged(before);
}
void partial(bool fail_second, bool fail_first = false) {
    ++timezone_cases; TimezoneFixture f;
    f.runtime.execution.ppu.cpu.gpr[4] = 0x10128;
    if (fail_first) { f.runtime.execution.ppu.cpu.gpr[3] = 0x10128; f.runtime.execution.ppu.cpu.gpr[4] = 0x10210; }
    f.runtime.probe.fail_toc = fail_second || fail_first;
    f.runtime.probe.cancel_output = !fail_second && !fail_first;
    ZCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    ZCHECK(!f.call());
    ZCHECK(f.runtime.control->stop == (fail_second || fail_first ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    ZCHECK(f.runtime.control->timezone_calls == 1 && f.runtime.control->timezone_minutes == -300 && f.runtime.control->summertime_minutes == 60);
    ZCHECK(f.runtime.probe.writes == (fail_first ? 0u : 1u) && f.runtime.recorder.report().published_count == (fail_first ? 0u : 1u));
    ZCHECK(!f.runtime.recorder.report().batch_active && !std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)) &&
        !std::memcmp(&before, &f.runtime.control->terminal, sizeof(before)));
    if (!fail_first) { ZCHECK(f.value(0x10200) == uint32_t(-300)); f.event(0, 0x10200, uint32_t(-300)); }
    else ZCHECK(f.value(0x10200) == UINT32_C(0x11223344));
    ZCHECK(f.runtime.bytes(0x10128) == 0);
}
void reservation(bool same_granule) {
    ++timezone_cases; TimezoneFixture f;
    ZCHECK(f.admit()); auto *cpu = &f.runtime.execution.ppu.cpu;
    const uint32_t address = same_granule ? 0x10240 : 0x10300;
    ZCHECK(ppu_native_load_reserve32(cpu, address) == 0);
    ZCHECK(f.call());
    ZCHECK(ppu_native_store_conditional32(cpu, address, 0x12345678) == !same_granule);
    ZCHECK(f.value(address) == (same_granule ? UINT32_C(0) : UINT32_C(0x12345678)));
    ZCHECK(f.policy.snapshot().generated == 2 && f.policy.snapshot().hle == 1);
}
void inter_write_budget(bool deadline) {
    ++timezone_cases; TimezoneFixture f(-300, 60, 10, false,
        deadline ? UINT64_C(1000) : UINT64_C(1000000000));
    if (deadline) f.advance_at = 4; else f.fail_at = 4;
    ZCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
    ZCHECK(!f.call());
    ZCHECK(f.runtime.control->stop == (deadline ? StartupStop::Deadline : StartupStop::BudgetClockFailure));
    ZCHECK(f.policy.snapshot().stop == (deadline ? infamous::discovery::StopReason::Deadline : infamous::discovery::StopReason::InvalidClock));
    ZCHECK(f.operational_calls == 4 && f.policy.snapshot().hle == 1 && f.runtime.control->timezone_calls == 1);
    ZCHECK(f.value(0x10200) == uint32_t(-300) && f.value(0x10210) == UINT32_C(0x55667788));
    ZCHECK(f.runtime.probe.writes == 1 && f.runtime.recorder.report().published_count == 1 &&
        !f.runtime.recorder.report().batch_active && !f.runtime.recorder.report().remaining_publications);
    ZCHECK(!std::memcmp(&before, &f.runtime.execution.ppu.cpu, sizeof(before)) &&
        !std::memcmp(&before, &f.runtime.control->terminal, sizeof(before)));
    f.event(0, 0x10200, uint32_t(-300));
}
#endif
}
int main() {
#ifdef TIMEZONE_FEATURE_OFF
    {
        ++timezone_cases; TimezoneFixture f;
        f.runtime.services.allocate_memory = [](void *, uint64_t, uint64_t, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.free_memory = [](void *, uint32_t) -> ps3_user_memory_service_result { std::abort(); };
        f.runtime.services.query_memory = [](void *, uint64_t) -> ps3_user_memory_service_result { std::abort(); };
        ZCHECK(f.admit()); const auto before = f.runtime.execution.ppu.cpu;
        ZCHECK(!f.call() && f.runtime.control->stop == StartupStop::UnsupportedSyscall);
        ZCHECK(f.runtime.control->timezone_calls == 0); f.unchanged(before);
    }
#else
    success(-300, 60); success(0, 0); success(INT32_MIN, INT32_MAX);
    overlap(false); overlap(true);
    for (Denial kind : {Denial::Missing, Denial::NullFirst, Denial::NullSecond, Denial::HighFirst,
        Denial::HighSecond, Denial::Wrap, Denial::ReadOnly, Denial::UnmappedFirst, Denial::UnmappedSecond,
        Denial::Quota, Denial::TraceFull, Denial::CancelEntry, Denial::CancelProbe, Denial::CancelSecondProbe,
        Denial::ClockProbe, Denial::ClockSecondProbe, Denial::DeadlineProbe, Denial::DeadlineSecondProbe, Denial::Alias}) denied(kind);
    partial(false); partial(true); partial(true, true);
    reservation(true); reservation(false);
    inter_write_budget(false); inter_write_budget(true);
#endif
    std::printf("Timezone host cases=%d checks=%d failures=%d inherited_failures=%d fixed_virtual_policy=true sdk_detection_proof=false\n", timezone_cases, timezone_checks, timezone_failures, failures);
    return timezone_failures || failures ? 1 : 0;
}
