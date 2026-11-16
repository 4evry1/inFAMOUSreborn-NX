/* Host bridge fixture: this includes the real bridge to establish its worker
 * boundary directly. It does not execute a native generated import stub. */
#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>

namespace {
int failures, cases;
#define CHECK(value) do { if (!(value)) { std::printf("STARTUP_RECORDER_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
struct Terminal {};
PpuMemoryTraceRecorder *terminal_recorder;
pthread_mutex_t *terminal_guard, *terminal_registration_guard;
bool terminal_closed, terminal_unlocked;
uint64_t host_tick;
void guard_lock(void *p) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(p)); }
void guard_unlock(void *p) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p)); }
struct MemoryProbe {
    ps3_guest_memory backing{};
    StartupControl *control = nullptr;
    bool cancel_output = false, fail_toc = false, cancel_read = false;
    bool cancel_toc_probe = false;
    unsigned writes = 0, reads = 0;
    static ps3_guest_memory_status commit(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.commit(self.backing.context, address, length);
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.decommit(self.backing.context, address, length);
    }
    static ps3_guest_memory_status read(void *p, uint32_t address, void *bytes, size_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        ++self.reads;
        const auto status = self.backing.read(self.backing.context, address, bytes, length);
        if (self.cancel_toc_probe && address == 0x10128 && status == PS3_GUEST_MEMORY_OK)
            self.control->cancelled.store(true);
        if (self.cancel_read && address == 0x10200 && status == PS3_GUEST_MEMORY_OK)
            self.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status write(void *p, uint32_t address, const void *bytes, size_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        if (self.fail_toc && address == 0x10128) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        const auto status = self.backing.write(self.backing.context, address, bytes, length);
        if (status == PS3_GUEST_MEMORY_OK) {
            ++self.writes;
            if (self.cancel_output && address == 0x10200) self.control->cancelled.store(true);
        }
        return status;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t address, uint64_t length) {
        auto &self = *static_cast<MemoryProbe *>(p);
        return self.backing.zero(self.backing.context, address, length);
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};
struct Runtime {
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[4]{};
    ps3_atomic_pin_record pins[4]{};
    MemoryProbe probe{};
    PpuMemoryRamRange ram{0x10000, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[4]{};
    ps3_atomic_domain &initialize() {
        if (!space) std::abort();
        probe.backing = elf_test_host_space_port(space);
        if (ps3_atomic_domain_init(&domain, probe.port(),
            {&guard, guard_lock, guard_unlock}, owners, 4, pins, 4) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    Runtime() : binding(initialize(), &ram, 1) {
        CHECK(ps3_atomic_commit(&domain, ram.address, ram.length) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get();
        probe.control = control.get();
        execution.ppu.cpu.gpr[1] = 0x10100;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.gpr[3] = 0x10200;
        execution.ppu.cpu.thread_id = 0x1234;
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(pthread_mutex_destroy(&guard) == 0);
        elf_test_host_space_destroy(space);
    }
    uint64_t bytes(uint32_t address) {
        unsigned char value[8]{};
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, address, value, 8) == PS3_GUEST_MEMORY_OK);
        uint64_t result = 0; for (unsigned char byte : value) result = result * 256 + byte;
        return result;
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return host_tick; }
extern "C" [[noreturn]] void threadExit(void) {
    terminal_closed = !terminal_recorder || !terminal_recorder->report().batch_active;
    const int result = pthread_mutex_trylock(terminal_guard);
    terminal_unlocked = result == 0;
    if (!result) pthread_mutex_unlock(terminal_guard);
    CHECK(terminal_closed); CHECK(terminal_unlocked);
    if (terminal_registration_guard) {
        const int status = pthread_mutex_trylock(terminal_registration_guard);
        CHECK(status == 0);
        if (!status) pthread_mutex_unlock(terminal_registration_guard);
    }
    throw Terminal{}; // Observations above precede stack unwinding.
}

#include <ctime>
#include <initializer_list>
#include EXITSPAWN_STUB_A
#include EXITSPAWN_STUB_B
namespace {
struct RegistrationGuard {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    StartupControl *control = nullptr;
    bool cancel_after_unlock = false;
    static void lock(void *context) {
        if (pthread_mutex_lock(&static_cast<RegistrationGuard *>(context)->mutex)) std::abort();
    }
    static void unlock(void *context) {
        auto &self = *static_cast<RegistrationGuard *>(context);
        if (pthread_mutex_unlock(&self.mutex)) std::abort();
        if (self.cancel_after_unlock) {
            self.cancel_after_unlock = false;
            self.control->cancelled.store(true, std::memory_order_release);
        }
    }
};
struct OperationalClock {
    timespec epoch{};
    unsigned calls = 0;
    OperationalClock() { if (clock_gettime(CLOCK_MONOTONIC, &epoch)) std::abort(); }
    static ps3_clock_status sample(void *context, uint64_t *output) {
        auto &self = *static_cast<OperationalClock *>(context);
        timespec now{}; if (clock_gettime(CLOCK_MONOTONIC, &now)) std::abort();
        const int64_t seconds = int64_t(now.tv_sec) - self.epoch.tv_sec;
        const int64_t nanoseconds = seconds * INT64_C(1000000000) + now.tv_nsec - self.epoch.tv_nsec;
        if (nanoseconds < 0) std::abort();
        ++self.calls; *output = uint64_t(nanoseconds) / 1000;
        return PS3_CLOCK_OK;
    }
    ps3_clock port{this, sample};
};
void pending_trampoline(void *) { std::abort(); }
struct ExitRuntime : Runtime {
    RegistrationGuard registration_guard;
    ps3_exitspawn_registry registry{};
    PpuMemoryTraceEvent observations[16]{};
    OperationalClock operational;
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ExitRuntime(uint64_t hle_limit = 32, size_t capacity = 16)
        : policy({0, hle_limit, 0, UINT64_C(1000000000)}, 0), budget(policy, operational.port) {
        registration_guard.control = control.get();
        CHECK(ps3_exitspawn_init(&registry,
            {&registration_guard, RegistrationGuard::lock, RegistrationGuard::unlock}) == PS3_EXITSPAWN_OK);
        execution.exitspawn_callbacks = &registry;
        CHECK(recorder.configure(observations, capacity, PpuMemoryTraceRetention::Strict, 91));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        const unsigned char sentinel[] = {0x91,0x82,0x73,0x64,0x55,0x46,0x37,0x28};
        CHECK(ps3_atomic_write(&domain, 0x10128, sentinel, 8) == PS3_GUEST_MEMORY_OK);
        probe.writes = probe.reads = 0;
    }
    ~ExitRuntime() {
        if (pthread_mutex_destroy(&registration_guard.mutex)) std::abort();
    }
    ps3_exitspawn_snapshot snapshot() {
        registration_guard.cancel_after_unlock = false;
        ps3_exitspawn_snapshot value{};
        CHECK(ps3_exitspawn_read(&registry, &value) == PS3_EXITSPAWN_OK);
        return value;
    }
    void call(bool second, uint64_t descriptor, uint64_t ignored = UINT64_C(0xfedcba9876543210)) {
        execution.ppu.cpu.gpr[3] = descriptor; execution.ppu.cpu.gpr[4] = ignored;
        const auto before = execution.ppu.cpu;
        const auto counts_before = policy.snapshot();
        const bool cancelled_before = control->cancelled.load();
        terminal_recorder = &recorder; terminal_guard = &guard;
        terminal_registration_guard = &registration_guard.mutex;
        terminal_closed = terminal_unlocked = false;
        g_trampoline_fn = pending_trampoline;
        active = control.get();
        try {
            if (second) func_0055DAA4(&execution.ppu.cpu);
            else func_0055D9C4(&execution.ppu.cpu);
            CHECK(g_trampoline_fn == pending_trampoline);
            active = nullptr;
        } catch (const Terminal &) {
            CHECK(terminal_closed && terminal_unlocked);
            CHECK(g_trampoline_fn == nullptr);
        }
        CHECK(!std::memcmp(&before, &execution.ppu.cpu, sizeof(before)));
        CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
        const auto counts = policy.snapshot();
        uint64_t expected = counts_before.hle;
        if (!cancelled_before && expected < policy.limits().hle) ++expected;
        CHECK(counts.hle == expected && counts.generated == 0 && counts.indirect == 0);
        std::printf("EXITSPAWN_HOST nid=0x%x stop=%d calls=%zu returns=%zu hle=%llu publications=%llu\n",
            second ? 0x96328741u : 0x2c847572u, int(control->stop), control->exitspawn_calls,
            control->exitspawn_returns, static_cast<unsigned long long>(counts.hle),
            static_cast<unsigned long long>(recorder.report().published_count));
        g_trampoline_fn = nullptr;
        terminal_recorder = nullptr; terminal_guard = terminal_registration_guard = nullptr;
    }
};
[[maybe_unused]] void exact_toc(ExitRuntime &r, size_t index = 0) {
    PpuMemoryTraceSample value{}; CHECK(r.recorder.read(index, value));
    CHECK(value.sequence == 91 + index && value.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(value.event.operation == PpuMemoryTraceOperation::Write && value.event.address == 0x10128);
    CHECK(value.event.length == 8 && value.event.value == UINT64_C(0x123456789abcdef0));
    CHECK(!value.event.stored && !value.event.byte_transfer && value.event.status == PS3_GUEST_MEMORY_OK);
}
void independent_slots() {
    ++cases; ExitRuntime r;
    r.call(false, 0x771b18, 0);
#ifdef PS3_NATIVE_STARTUP_EXITSPAWN
    CHECK(r.control->stop == StartupStop::None);
    auto value = r.snapshot(); CHECK(value.atexitspawn == 0x771b18 && value.at_Exitspawn == 0);
    r.call(true, 0x771b30, UINT64_MAX);
    value = r.snapshot(); CHECK(value.atexitspawn == 0x771b18 && value.at_Exitspawn == 0x771b30);
    CHECK(r.control->exitspawn_calls == 2 && r.control->exitspawn_returns == 2);
    CHECK(r.control->exitspawn_pending_trampoline_seen);
    CHECK(r.control->exitspawn_result.status == PS3_EXITSPAWN_OK);
    CHECK(r.control->exitspawn_result.snapshot.atexitspawn == 0x771b18 &&
        r.control->exitspawn_result.snapshot.at_Exitspawn == 0x771b30);
    CHECK(r.recorder.report().published_count == 2 && r.operational.calls == 6);
    exact_toc(r); exact_toc(r, 1);
#else
    CHECK(r.control->stop == StartupStop::UnknownImport);
    const auto value = r.snapshot(); CHECK(!value.atexitspawn && !value.at_Exitspawn);
    CHECK(!r.control->exitspawn_calls && !r.recorder.report().published_count);
    r.call(true, 0x771b30);
    CHECK(r.control->stop == StartupStop::UnknownImport);
    const auto second_value = r.snapshot(); CHECK(!second_value.atexitspawn && !second_value.at_Exitspawn);
    CHECK(!r.control->exitspawn_calls && !r.recorder.report().published_count);
#endif
}
#ifdef PS3_NATIVE_STARTUP_EXITSPAWN
void null_and_first_wins() {
    for (bool second : {false, true}) {
        ++cases; ExitRuntime r;
        r.call(second, 0);
        CHECK(r.control->stop == StartupStop::None && r.control->exitspawn_result.decision == PS3_EXITSPAWN_NULL);
        auto value = r.snapshot(); CHECK(!value.atexitspawn && !value.at_Exitspawn);
        // These are intentionally unaligned/unmapped opaque identities.
        r.call(second, 3);
        CHECK(r.control->exitspawn_result.decision == PS3_EXITSPAWN_INSERTED);
        r.call(second, UINT32_MAX, 0x10400);
        CHECK(r.control->exitspawn_result.decision == PS3_EXITSPAWN_RETAINED);
        r.call(second, 0);
        CHECK(r.control->exitspawn_result.decision == PS3_EXITSPAWN_RETAINED);
        value = r.snapshot(); CHECK((second ? value.at_Exitspawn : value.atexitspawn) == 3);
        CHECK((second ? value.atexitspawn : value.at_Exitspawn) == 0);
        CHECK(r.probe.reads == 4); // Four TOC probes, no descriptor reads.
        CHECK(r.recorder.report().published_count == 4);
    }
    { ++cases; ExitRuntime r; r.call(false, UINT32_MAX);
      CHECK(r.snapshot().atexitspawn == UINT32_MAX && r.probe.reads == 1); }
}
void early_rejections() {
    for (uint64_t stack : {UINT64_C(0x100010100), UINT64_C(0xffffffd4)}) {
        ++cases; ExitRuntime r; r.execution.ppu.cpu.gpr[1] = stack;
        r.call(false, 3);
        CHECK(r.control->stop == StartupStop::InvalidMemory);
        CHECK(r.operational.calls == 1 && !r.probe.reads && !r.probe.writes);
        CHECK(!r.snapshot().atexitspawn && !r.control->exitspawn_calls);
        CHECK(!r.recorder.report().published_count);
        CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
    }
    for (bool second : {false, true}) {
        ++cases; ExitRuntime r; r.call(second, UINT64_C(0x100000003));
        CHECK(r.control->stop == StartupStop::InvalidMemory);
        CHECK(!r.snapshot().atexitspawn && !r.snapshot().at_Exitspawn);
        CHECK(!r.probe.writes && !r.recorder.report().published_count && !r.control->exitspawn_calls);
    }
    { ++cases; ExitRuntime r; r.execution.exitspawn_callbacks = nullptr;
      r.call(false, 3); CHECK(r.control->stop == StartupStop::MissingServices);
      CHECK(!r.snapshot().atexitspawn && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ExitRuntime r; ps3_exitspawn_registry uninitialized{};
      r.execution.exitspawn_callbacks = &uninitialized; r.call(true, 3);
      CHECK(r.control->stop == StartupStop::MissingServices && !r.probe.writes); }
    { ++cases; ExitRuntime r; r.control->cancelled.store(true); r.call(false, 3);
      CHECK(r.control->stop == StartupStop::Cancelled && r.operational.calls == 0);
      CHECK(!r.snapshot().atexitspawn && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ExitRuntime r; r.probe.cancel_toc_probe = true; r.call(false, 3);
      CHECK(r.control->stop == StartupStop::Cancelled && r.operational.calls == 1);
      CHECK(!r.snapshot().atexitspawn && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ExitRuntime r(32, 1); r.call(true, 0); r.probe.writes = 0;
      r.call(false, 3); CHECK(r.control->stop == StartupStop::TraceCapacity);
      const auto value = r.snapshot(); CHECK(!value.atexitspawn && !value.at_Exitspawn);
      CHECK(r.recorder.report().published_count == 1 && !r.probe.writes && r.control->exitspawn_calls == 1); }
    { ++cases; ExitRuntime r(1); r.call(false, 3); r.probe.writes = 0; r.call(true, 5);
      CHECK(r.control->stop == StartupStop::HleSyscallBudget);
      const auto value = r.snapshot(); CHECK(value.atexitspawn == 3 && !value.at_Exitspawn);
      CHECK(r.recorder.report().published_count == 1 && !r.probe.writes && r.control->exitspawn_calls == 1); }
}
void postregistration_stop(bool fail_toc) {
    ++cases; ExitRuntime r;
    r.registration_guard.cancel_after_unlock = !fail_toc; r.probe.fail_toc = fail_toc;
    r.call(false, 0x771b18);
    CHECK(r.control->stop == (fail_toc ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    CHECK(r.control->exitspawn_calls == 1 && !r.control->exitspawn_returns);
    CHECK(r.control->exitspawn_result.status == PS3_EXITSPAWN_OK &&
        r.control->exitspawn_result.decision == PS3_EXITSPAWN_INSERTED);
    CHECK(r.control->exitspawn_result.snapshot.atexitspawn == 0x771b18);
    CHECK(r.snapshot().atexitspawn == 0x771b18);
    CHECK(!r.probe.writes && !r.recorder.report().published_count);
    CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
}
void instance_isolation() {
    ++cases; ExitRuntime first, second;
    first.call(false, 3); second.call(false, 5);
    CHECK(first.snapshot().atexitspawn == 3 && second.snapshot().atexitspawn == 5);
}
#endif
}
int main() {
    failures = cases = 0; host_tick = 0;
    independent_slots();
#ifdef PS3_NATIVE_STARTUP_EXITSPAWN
    null_and_first_wins(); early_rejections(); postregistration_stop(false);
    postregistration_stop(true); instance_isolation();
#endif
    std::printf("Startup exitspawn host cases=%d failures=%d actual_stub_bodies=true native_runtime=false\n", cases, failures);
    return failures ? 1 : 0;
}
