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
#define CHECK(value) do { if (!(value)) { std::printf("DIAGNOSTIC_MODULE_FAIL module_admission=assumed game_boot=false line=%d\n", __LINE__); ++failures; } } while (0)
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
#include MODULE_ADMISSION_STUB
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
constexpr uint16_t MODULE_CATALOG[] = {0x000e,0x000a,0x0017,0x0009,0x0000,0x0014,0x0011,0x0015,0x0033,0x0010,0x001c,0xf035,0x0016};
static_assert(sizeof(MODULE_CATALOG) / sizeof(*MODULE_CATALOG) == 13);
struct ModuleRuntime : Runtime {
    RegistrationGuard model_guard;
    ps3_diagnostic_module_state model{};
    ps3_diagnostic_module_slot slots[13]{};
    PpuMemoryTraceEvent observations[16]{};
    OperationalClock operational;
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ModuleRuntime(uint64_t hle_limit = 32, size_t capacity = 16)
        : policy({0, hle_limit, 0, UINT64_C(1000000000)}, 0), budget(policy, operational.port) {
        model_guard.control = control.get();
        CHECK(ps3_diagnostic_module_init(&model,
            {&model_guard, RegistrationGuard::lock, RegistrationGuard::unlock},
            MODULE_CATALOG, 13, slots, 13) == PS3_DIAGNOSTIC_MODULE_OK);
        execution.diagnostic_modules = &model;
        CHECK(recorder.configure(observations, capacity, PpuMemoryTraceRetention::Strict, 191));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        const unsigned char sentinel[] = {0x91,0x82,0x73,0x64,0x55,0x46,0x37,0x28};
        CHECK(ps3_atomic_write(&domain, 0x10128, sentinel, 8) == PS3_GUEST_MEMORY_OK);
        probe.writes = probe.reads = 0;
    }
    ~ModuleRuntime() { if (pthread_mutex_destroy(&model_guard.mutex)) std::abort(); }
    ps3_diagnostic_module_result read(uint16_t id = 0xe) {
        model_guard.cancel_after_unlock = false;
        ps3_diagnostic_module_result value{};
        CHECK(ps3_diagnostic_module_read(&model, id, &value) == PS3_DIAGNOSTIC_MODULE_OK);
        return value;
    }
    void call(uint64_t id, uint32_t nid = 0x32267a31) {
        execution.ppu.cpu.gpr[3] = id;
        execution.ppu.cpu.gpr[4] = UINT64_C(0xfedcba9876543210);
        const auto before = execution.ppu.cpu;
        const auto counts_before = policy.snapshot();
        const bool cancelled_before = control->cancelled.load();
        terminal_recorder = &recorder; terminal_guard = &guard;
        terminal_registration_guard = &model_guard.mutex;
        terminal_closed = terminal_unlocked = false;
        g_trampoline_fn = pending_trampoline;
        active = control.get();
        try {
            if (nid == 0x32267a31) func_0055C8A4(&execution.ppu.cpu);
            else ps3_hle_call(nid, &execution.ppu.cpu); // Labelled unknown-export bridge fixture.
            CHECK(g_trampoline_fn == pending_trampoline);
            active = nullptr;
        } catch (const Terminal &) {
            CHECK(terminal_closed && terminal_unlocked);
            CHECK(g_trampoline_fn == nullptr);
        }
        auto expected_cpu = before;
        if (control->stop == StartupStop::None) expected_cpu.gpr[3] = 0;
        CHECK(!std::memcmp(&expected_cpu, &execution.ppu.cpu, sizeof(expected_cpu)));
        CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
        const auto counts = policy.snapshot();
        uint64_t expected = counts_before.hle;
        if (!cancelled_before && expected < policy.limits().hle) ++expected;
        CHECK(counts.hle == expected && counts.generated == 0 && counts.indirect == 0);
        std::printf("DIAGNOSTIC_MODULE_HOST module_admission=assumed game_boot=false nid=0x%x id=0x%llx stop=%d model_calls=%zu returns=%zu hle=%llu publications=%llu actual_load_stub_body=%d\n",
            nid, static_cast<unsigned long long>(id), int(control->stop), control->diagnostic_module_calls,
            control->diagnostic_module_returns, static_cast<unsigned long long>(counts.hle),
            static_cast<unsigned long long>(recorder.report().published_count), int(nid == 0x32267a31));
        g_trampoline_fn = nullptr;
        terminal_recorder = nullptr; terminal_guard = terminal_registration_guard = nullptr;
    }
};
[[maybe_unused]] void toc_event(ModuleRuntime &r, size_t index) {
    PpuMemoryTraceSample value{}; CHECK(r.recorder.read(index, value));
    CHECK(value.sequence == 191 + index && value.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(value.event.operation == PpuMemoryTraceOperation::Write && value.event.address == 0x10128);
    CHECK(value.event.length == 8 && value.event.value == UINT64_C(0x123456789abcdef0));
    CHECK(!value.event.stored && !value.event.byte_transfer && value.event.status == PS3_GUEST_MEMORY_OK);
}
void catalog_assumption() {
    ++cases; ModuleRuntime r;
#ifdef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
    for (size_t index = 0; index < 13; ++index) {
        r.call(MODULE_CATALOG[index]);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->diagnostic_module_has_result && r.control->diagnostic_module_pending_trampoline_seen);
        const auto result = r.control->diagnostic_module_result;
        CHECK(result.status == PS3_DIAGNOSTIC_MODULE_OK && result.module_id == MODULE_CATALOG[index]);
        CHECK(result.previous_count == 0 && result.current_count == 1 && result.total_admissions == index + 1);
        CHECK(r.control->diagnostic_module_argument == MODULE_CATALOG[index]);
        CHECK(r.read(MODULE_CATALOG[index]).current_count == 1);
        toc_event(r, index);
    }
    CHECK(r.control->diagnostic_module_calls == 13 && r.control->diagnostic_module_returns == 13);
    CHECK(r.operational.calls == 39 && r.probe.reads == 13 && r.probe.writes == 13);
#else
    r.call(0xe);
    CHECK(r.control->stop == StartupStop::UnknownImport);
    CHECK(r.read().total_admissions == 0 && !r.recorder.report().published_count);
    CHECK(!r.control->diagnostic_module_calls && !r.control->diagnostic_module_returns);
    // With the flag off the new NID still stops as UnknownImport and the
    // appended counters stay zero.
    { ++cases; ModuleRuntime m; m.call(0, 0x63ff6ff9);
      CHECK(m.control->stop == StartupStop::UnknownImport);
      CHECK(!m.control->sysmodule_initialize_calls && !m.control->sysmodule_initialize_returns);
      CHECK(m.control->sysmodule_initialize_last_result == 0);
      CHECK(!m.recorder.report().published_count && !m.read().total_admissions); }
#endif
}
#ifdef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
void repeats_and_isolation() {
    ++cases; ModuleRuntime first, second;
    first.call(0); first.call(0); second.call(0);
    CHECK(first.read(0).current_count == 2 && first.read(0).total_admissions == 2);
    CHECK(second.read(0).current_count == 1 && second.read(0).total_admissions == 1);
    CHECK(first.control->diagnostic_module_result.previous_count == 1 &&
        first.control->diagnostic_module_result.current_count == 2);
}
void early_rejections() {
    for (uint64_t id : {UINT64_C(0x1000e), UINT64_C(0x10000000e)}) {
        ++cases; ModuleRuntime r; r.call(id);
        CHECK(r.control->stop == StartupStop::DiagnosticModuleAdmissionFailure);
        CHECK(r.control->diagnostic_module_argument == id && !r.control->diagnostic_module_has_result);
        CHECK(!r.control->diagnostic_module_calls && !r.read().total_admissions);
        CHECK(!r.probe.reads && !r.probe.writes && !r.recorder.report().published_count);
    }
    for (uint64_t stack : {UINT64_C(0x100010100), UINT64_C(0xffffffd4)}) {
        ++cases; ModuleRuntime r; r.execution.ppu.cpu.gpr[1] = stack; r.call(0xe);
        CHECK(r.control->stop == StartupStop::InvalidMemory && r.operational.calls == 1);
        CHECK(!r.probe.reads && !r.probe.writes && !r.recorder.report().published_count);
        CHECK(!r.read().total_admissions && !r.control->diagnostic_module_has_result);
    }
    { ++cases; ModuleRuntime r; r.execution.diagnostic_modules = nullptr; r.call(0xe);
      CHECK(r.control->stop == StartupStop::MissingServices && !r.read().total_admissions);
      CHECK(!r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ModuleRuntime r; ps3_diagnostic_module_state uninitialized{};
      r.execution.diagnostic_modules = &uninitialized; r.call(0xe);
      CHECK(r.control->stop == StartupStop::MissingServices && !r.read().total_admissions); }
    { ++cases; ModuleRuntime r; r.call(0xffff);
      CHECK(r.control->stop == StartupStop::DiagnosticModuleAdmissionFailure);
      CHECK(r.control->diagnostic_module_has_result && r.control->diagnostic_module_calls == 1);
      CHECK(r.control->diagnostic_module_result.status == PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE);
      CHECK(!r.read().total_admissions && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ModuleRuntime r; r.call(0xe, 0x5a59e258);
      CHECK(r.control->stop == StartupStop::UnknownImport && !r.read().total_admissions);
      CHECK(!r.control->diagnostic_module_calls && !r.recorder.report().published_count); }
    { ++cases; ModuleRuntime r; r.control->cancelled.store(true); r.call(0xe);
      CHECK(r.control->stop == StartupStop::Cancelled && r.operational.calls == 0);
      CHECK(!r.read().total_admissions && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ModuleRuntime r; r.probe.cancel_toc_probe = true; r.call(0xe);
      CHECK(r.control->stop == StartupStop::Cancelled && r.operational.calls == 1);
      CHECK(!r.read().total_admissions && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; ModuleRuntime r(32,1); r.call(0xe); r.probe.writes = 0; r.call(0xa);
      CHECK(r.control->stop == StartupStop::TraceCapacity && !r.control->diagnostic_module_has_result);
      CHECK(r.read(0xe).current_count == 1 && r.read(0xa).current_count == 0);
      CHECK(r.read().total_admissions == 1 && r.control->diagnostic_module_calls == 1);
      CHECK(!r.probe.writes && r.recorder.report().published_count == 1); }
    { ++cases; ModuleRuntime r(0); r.call(0xe);
      CHECK(r.control->stop == StartupStop::HleSyscallBudget);
      CHECK(!r.read().total_admissions && !r.control->diagnostic_module_calls);
      CHECK(!r.probe.writes && !r.recorder.report().published_count); }
}
void count_exhaustion() {
    ++cases; ModuleRuntime r;
    // Quiescent, consistent distant counter boundary, not a real load history.
    r.slots[0].admission_count = UINT32_MAX; r.model.total_admissions = UINT32_MAX;
    r.call(0xe);
    CHECK(r.control->stop == StartupStop::DiagnosticModuleAdmissionFailure);
    CHECK(r.control->diagnostic_module_has_result && r.control->diagnostic_module_calls == 1);
    CHECK(r.control->diagnostic_module_result.status == PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED);
    CHECK(r.read().current_count == UINT32_MAX && r.read().total_admissions == UINT32_MAX);
    CHECK(!r.probe.writes && !r.recorder.report().published_count);
}
void after_model(bool fail_toc) {
    ++cases; ModuleRuntime r;
    r.model_guard.cancel_after_unlock = !fail_toc; r.probe.fail_toc = fail_toc;
    r.call(0xe);
    CHECK(r.control->stop == (fail_toc ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    CHECK(r.control->diagnostic_module_calls == 1 && !r.control->diagnostic_module_returns);
    CHECK(r.control->diagnostic_module_has_result && r.control->diagnostic_module_result.status == PS3_DIAGNOSTIC_MODULE_OK);
    CHECK(r.control->diagnostic_module_result.current_count == 1 && r.read().total_admissions == 1);
    CHECK(!r.probe.writes && !r.recorder.report().published_count);
    CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
}
// cellSysmoduleInitialize (NID 0x63ff6ff9). Firmware contract: the libsysmodule export
// returns 0 unconditionally, sets no state and issues no
// syscall, so a second call also returns 0 (not 0x80012001).
void sysmodule_initialize_contract() {
    {
        ++cases; ModuleRuntime r;
        r.call(0, 0x63ff6ff9);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->sysmodule_initialize_calls == 1 && r.control->sysmodule_initialize_returns == 1);
        CHECK(r.control->sysmodule_initialize_last_result == 0);
        CHECK(!r.control->sysmodule_initialize_second_call);
        // The TOC was republished exactly as the other HLE handlers do.
        CHECK(r.recorder.report().published_count == 1);
        toc_event(r, 0);
        // No admission was recorded: Initialize does not touch the module table.
        CHECK(r.read().total_admissions == 0);
        CHECK(r.control->diagnostic_module_calls == 0 && r.control->diagnostic_module_returns == 0);
    }
    {
        ++cases; ModuleRuntime r;
        r.call(0, 0x63ff6ff9);
        r.call(0, 0x63ff6ff9);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->sysmodule_initialize_calls == 2 && r.control->sysmodule_initialize_returns == 2);
        // The firmware does not produce CELL_SYSMODULE_ERROR_DUPLICATED here.
        CHECK(r.control->sysmodule_initialize_last_result == 0);
        CHECK(r.control->sysmodule_initialize_second_call);
        CHECK(r.recorder.report().published_count == 2);
        CHECK(r.read().total_admissions == 0);
    }
    // Interaction with LoadModule: the firmware gate on the internal
    // initialized flag lives in LoadModuleInternal and is not set by
    // Initialize, so admission still works and still counts independently.
    {
        ++cases; ModuleRuntime r;
        r.call(0, 0x63ff6ff9);
        r.call(0xe);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->sysmodule_initialize_calls == 1);
        CHECK(r.control->diagnostic_module_calls == 1 && r.control->diagnostic_module_result.status == PS3_DIAGNOSTIC_MODULE_OK);
        CHECK(r.read(0xe).current_count == 1 && r.read().total_admissions == 1);
    }
    // Argument registers are not inspected: a wild r3 changes nothing.
    {
        ++cases; ModuleRuntime r;
        r.call(UINT64_C(0xffffffffffffffff), 0x63ff6ff9);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->sysmodule_initialize_last_result == 0);
        CHECK(r.control->sysmodule_initialize_calls == 1);
    }
    // Cancellation before the TOC publication keeps the call counted but not
    // returned, matching the other HLE handlers.
    {
        ++cases; ModuleRuntime r;
        r.control->cancelled.store(true);
        r.call(0, 0x63ff6ff9);
        CHECK(r.control->stop == StartupStop::Cancelled);
        CHECK(r.control->sysmodule_initialize_calls == 0 && r.control->sysmodule_initialize_returns == 0);
        CHECK(!r.probe.writes && !r.recorder.report().published_count);
    }
}
#endif
}
int main() {
    failures = cases = 0; host_tick = 0;
    catalog_assumption();
#ifdef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
    repeats_and_isolation(); early_rejections(); count_exhaustion(); after_model(false); after_model(true);
    sysmodule_initialize_contract();
#endif
    std::printf("Diagnostic module host module_admission=assumed game_boot=false cases=%d failures=%d actual_stub_body=true native_runtime=false\n", cases, failures);
    return failures ? 1 : 0;
}
