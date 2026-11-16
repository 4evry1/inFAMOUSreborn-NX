/* Host bridge/core evidence with labelled compiled callback fixtures. This
 * does not prove native generated depth-nine execution or a game prefix. */
#ifndef STARTUP_OBSERVATION_BRIDGE
#define STARTUP_OBSERVATION_BRIDGE "../adapters/startup_execution.cpp"
#endif
#include STARTUP_OBSERVATION_BRIDGE
#include "../diagnostics/startup_observation.h"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>

namespace {
using namespace infamous::diagnostics;
using infamous::discovery::DiscoveryBudget;
using infamous::discovery::Limits;
int checks, failures;
#define OCHECK(value) do { ++checks; if (!(value)) { ++failures; std::printf("STARTUP_OBSERVATION_FAIL line=%d\n", __LINE__); } } while (0)
struct Terminal {};
struct Fixture;
Fixture *terminal_fixture;
constexpr uint32_t TARGET = 0x1000;
enum class TargetMode { Return, NestedReturn, NestedReject, NestedInvalid };
enum class CoreStopScenario { Continue, CancelFirst, DeadlineSecond };
uint64_t host_tick;
void lock_guard(void *p) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(p)); }
void unlock_guard(void *p) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p)); }
struct Fixture {
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[4]{};
    ps3_atomic_pin_record pins[4]{};
    PpuMemoryRamRange ram{0x10000, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[4]{};
    ImportRecord imports[64]{};
    CompletedDispatch completed[32]{};
    PendingDispatch pending[8]{};
    StartupObservation observation;
    unsigned clock_calls = 0;
    ps3_clock clock{this, sample};
    DiscoveryBudget policy{{1000, 1000, 1000, UINT64_C(1000000000)}, 0};
    DiscoveryBudgetBinding budget{policy, clock};
    bool terminal_checked = false;
    size_t expected_terminal_pending = 0;
    unsigned lookup_calls = 0, target_calls = 0, stop_calls = 0;
    TargetMode mode = TargetMode::Return;
    CoreStopScenario core_stop = CoreStopScenario::Continue;
    ps3_compiled_dispatch_stop_reason stop_latched = PS3_COMPILED_DISPATCH_CONTINUE;
    ps3_compiled_code_lookup lookup{this, find};
    static ps3_compiled_code_status find(const void *p, uint32_t address, ps3_compiled_function *output) {
        auto &self = *const_cast<Fixture *>(static_cast<const Fixture *>(p));
        ++self.lookup_calls;
        if (address != TARGET) return PS3_COMPILED_CODE_NOT_FOUND;
        *output = target;
        return PS3_COMPILED_CODE_OK;
    }
    static ps3_compiled_dispatch_stop_reason stop_requested(void *p) {
        auto &self = *static_cast<Fixture *>(p);
        ++self.stop_calls;
        if (self.stop_latched != PS3_COMPILED_DISPATCH_CONTINUE) return self.stop_latched;
        if (self.core_stop == CoreStopScenario::CancelFirst && self.stop_calls == 1)
            self.stop_latched = PS3_COMPILED_DISPATCH_CANCELLED;
        if (self.core_stop == CoreStopScenario::DeadlineSecond && self.stop_calls == 2)
            self.stop_latched = PS3_COMPILED_DISPATCH_DEADLINE;
        return self.stop_latched;
    }
    static void target(ppu_context *cpu) {
        auto &self = *terminal_fixture;
        ++self.target_calls;
        ++cpu->gpr[8];
        if (self.mode == TargetMode::NestedReturn && self.execution.dispatch.depth < 3)
            ps3_indirect_call(cpu);
        if (self.mode == TargetMode::NestedReject || self.mode == TargetMode::NestedInvalid) {
            if (self.execution.dispatch.depth == 8 && self.mode == TargetMode::NestedInvalid)
                cpu->ctr = 3;
            ps3_indirect_call(cpu);
        }
    }
    static ps3_clock_status sample(void *p, uint64_t *output) {
        ++static_cast<Fixture *>(p)->clock_calls;
        *output = 0;
        return PS3_CLOCK_OK;
    }
    ps3_atomic_domain &initialize() {
        if (!space || ps3_atomic_domain_init(&domain, elf_test_host_space_port(space),
            {&guard, lock_guard, unlock_guard}, owners, 4, pins, 4) != PS3_GUEST_MEMORY_OK)
            std::abort();
        return domain;
    }
    explicit Fixture(Limits limits = {1000, 1000, 1000, UINT64_C(1000000000)})
        : binding(initialize(), &ram, 1), policy(limits, 0) {
        OCHECK(ps3_atomic_commit(&domain, ram.address, ram.length) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        services.compiled_code = &lookup;
        execution.control = control.get();
        execution.dispatch_stop = {this, stop_requested};
        execution.ppu.cpu.gpr[1] = 0x10100;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.gpr[3] = 0x10200;
        execution.ppu.cpu.thread_id = 0x1234;
        OCHECK(recorder.configure(events, 4, PpuMemoryTraceRetention::Suffix));
        OCHECK(observation.configure(imports, 64, completed, 32, pending, 8));
    }
    ~Fixture() {
        if (execution.ppu.admitted)
            OCHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        OCHECK(pthread_mutex_destroy(&guard) == 0);
        elf_test_host_space_destroy(space);
    }
    bool admit() {
        return native_startup_admit_discovery(execution, binding, services, recorder, budget, observation);
    }
    template<class Operation> bool call(Operation operation) {
        terminal_fixture = this;
        active = control.get();
        try { operation(); active = nullptr; terminal_fixture = nullptr; return true; }
        catch (const Terminal &) { terminal_fixture = nullptr; OCHECK(terminal_checked); return false; }
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return host_tick; }
extern "C" [[noreturn]] void threadExit(void) {
    if (!terminal_fixture) std::abort();
    auto &f = *terminal_fixture;
    OCHECK(!f.recorder.report().batch_active);
    const int locked = pthread_mutex_trylock(&f.guard);
    OCHECK(locked == 0);
    if (!locked) pthread_mutex_unlock(&f.guard);
    const auto report = f.observation.report();
    OCHECK(report.pending_count == f.expected_terminal_pending);
    OCHECK(!report.immediate_outstanding);
    if (f.expected_terminal_pending) {
        OCHECK(report.completed.total == 1 && report.dispatch_started == 9);
        CompletedDispatch completed{};
        OCHECK(f.observation.read_completed(0, completed));
        OCHECK(completed.call_id == 8 && completed.parent_id == 7 && completed.entry.depth == 8);
        OCHECK(completed.depth_after == 8 && !completed.result.lookup_attempted);
        OCHECK(completed.result.status == uint32_t(f.mode == TargetMode::NestedInvalid
            ? PS3_COMPILED_DISPATCH_INVALID_ADDRESS : PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED));
        for (size_t depth = 0; depth < 8; ++depth) {
            PendingDispatch pending{};
            OCHECK(f.observation.read_pending(depth, pending));
            OCHECK(pending.occupied && pending.call_id == depth && pending.entry.depth == depth);
            OCHECK(pending.parent_id == (depth ? depth - 1 : NO_PARENT_ID));
        }
    }
    f.terminal_checked = true;
    // Test escape only: throwing through compiled targets is unsupported by
    // the production core. Pending/guard evidence above precedes this escape.
    throw Terminal{};
}

namespace {
void imports_and_syscalls() {
    Fixture f;
    OCHECK(f.admit());
    for (unsigned i = 0; i < 65; ++i) {
        f.execution.ppu.cpu.gpr[3] = 0x10200;
        OCHECK(f.call([&] { ps3_hle_call(0x350d454e, &f.execution.ppu.cpu); }));
    }
    const auto report = f.observation.report();
    OCHECK(report.imports.total == 65 && report.imports.retained_begin == 1 && report.imports.retained_end == 65);
    OCHECK(f.control->import_count == 0 && f.control->journal.count == 0 && !f.execution.ppu.trace);
    ImportRecord record{};
    OCHECK(f.observation.read_import(1, record) && record.sequence == 1 && record.nid == 0x350d454e);
    OCHECK(f.observation.read_import(64, record) && record.sequence == 64 && record.nid == 0x350d454e);
    f.execution.ppu.cpu.gpr[11] = 147;
    OCHECK(f.call([&] { lv2_syscall(&f.execution.ppu.cpu); }));
    OCHECK(f.execution.ppu.cpu.gpr[3] == UINT64_C(79800000));
    OCHECK(f.observation.report().imports.total == 65 && f.policy.snapshot().hle == 66);
    OCHECK(!f.call([&] { ps3_hle_call(0xfedcba98, &f.execution.ppu.cpu); }));
    OCHECK(f.control->stop == StartupStop::UnknownImport);
    OCHECK(f.observation.read_import(65, record) && record.nid == 0xfedcba98);
    OCHECK(f.policy.snapshot().hle == 67);
}
void completed_suffix() {
    Fixture f;
    OCHECK(f.admit());
    auto &cpu = f.execution.ppu.cpu;
    cpu.ctr = UINT64_C(0xfedcba9800001000);
    cpu.lr = 0xabcdef;
    const auto before = cpu;
    for (unsigned i = 0; i < 33; ++i)
        OCHECK(f.call([&] { ps3_indirect_call(&cpu); }));
    const auto report = f.observation.report();
    OCHECK(report.dispatch_started == 33 && report.completed.total == 33);
    OCHECK(report.completed.retained_begin == 1 && report.completed.retained_end == 33);
    OCHECK(report.pending_count == 0 && !report.immediate_outstanding && f.control->dispatch_count == 0);
    OCHECK(f.lookup_calls == 33 && f.target_calls == 33 && f.stop_calls == 66 && f.policy.snapshot().indirect == 33);
    auto expected = before; expected.gpr[8] += 33;
    OCHECK(!std::memcmp(&cpu, &expected, sizeof(cpu)));
    const uint64_t retained_samples[] = {1, 32};
    for (uint64_t sequence : retained_samples) {
        CompletedDispatch record{};
        OCHECK(f.observation.read_completed(sequence, record));
        OCHECK(record.sequence == sequence && record.call_id == sequence && record.parent_id == NO_PARENT_ID);
        OCHECK(record.entry.captured_ctr == before.ctr && record.entry.lr == before.lr && record.entry.sp == before.gpr[1]);
        OCHECK(record.entry.depth == 0 && record.depth_after == 0 && record.result.lookup_attempted);
        OCHECK(record.result.status == uint32_t(PS3_COMPILED_DISPATCH_OK) && record.result.lookup_status == uint32_t(PS3_COMPILED_CODE_OK));
    }
}
void nested_returns() {
    Fixture f; f.mode = TargetMode::NestedReturn;
    OCHECK(f.admit()); f.execution.ppu.cpu.ctr = TARGET;
    OCHECK(f.call([&] { ps3_indirect_call(&f.execution.ppu.cpu); }));
    OCHECK(f.observation.report().pending_count == 0 && f.observation.report().completed.total == 3);
    OCHECK(f.stop_calls == 6 && f.lookup_calls == 3 && f.target_calls == 3);
    for (uint64_t sequence = 0; sequence < 3; ++sequence) {
        CompletedDispatch record{};
        OCHECK(f.observation.read_completed(sequence, record));
        const uint64_t id = 2 - sequence;
        OCHECK(record.sequence == sequence && record.call_id == id && record.entry.depth == id && record.depth_after == id);
        OCHECK(record.parent_id == (id ? id - 1 : NO_PARENT_ID));
    }
}
void ninth_entry(bool invalid_address) {
    Fixture f; f.mode = invalid_address ? TargetMode::NestedInvalid : TargetMode::NestedReject;
    f.expected_terminal_pending = 8;
    OCHECK(f.admit()); f.execution.ppu.cpu.ctr = TARGET;
    OCHECK(!f.call([&] { ps3_indirect_call(&f.execution.ppu.cpu); }));
    OCHECK(f.control->stop == StartupStop::DispatchFailure && f.execution.dispatch.depth == 8);
    OCHECK(f.lookup_calls == 8 && f.target_calls == 8 && f.stop_calls == 16 && f.policy.snapshot().indirect == 9);
    OCHECK(f.control->dispatch_count == 0);
}
void selector_rollback() {
    Fixture f;
    f.services.memory = nullptr;
    OCHECK(!f.admit());
    OCHECK(!f.execution.ppu.admitted && !f.execution.ppu.recorder && !f.execution.ppu.budget && !f.execution.observation && !f.control->cpu);
    OCHECK(f.observation.valid_for_admission() && f.budget.valid_for_admission() && f.clock_calls == 0);
    f.services.memory = &f.binding.coherent_memory;
    f.execution.dispatch.limit = 7;
    OCHECK(!f.admit() && f.observation.valid_for_admission());
    f.execution.dispatch.limit = 8;
    f.execution.observation = &f.observation;
    OCHECK(!native_startup_admit(f.execution, f.binding, f.services));
    OCHECK(!native_startup_admit_recorded(f.execution, f.binding, f.services, f.recorder));
    OCHECK(!native_startup_admit_budgeted(f.execution, f.binding, f.services, f.recorder, f.budget));
    OCHECK(!f.admit() && f.execution.observation == &f.observation && !f.control->cpu);
    f.execution.observation = nullptr;
    OCHECK(f.admit());
}
void rejected_entries() {
    {
        Fixture f; OCHECK(f.admit());
        const auto before = f.execution.ppu.cpu;
        f.control->cancelled.store(true);
        OCHECK(!f.call([&] { ps3_hle_call(0xfedcba98, &f.execution.ppu.cpu); }));
        OCHECK(f.control->stop == StartupStop::Cancelled && f.clock_calls == 0);
        OCHECK(f.observation.report().imports.total == 0 && f.policy.snapshot().hle == 0);
        OCHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
    }
    {
        Fixture f; OCHECK(f.admit());
        f.execution.ppu.cpu.ctr = 3;
        const auto before = f.execution.ppu.cpu;
        OCHECK(!f.call([&] { ps3_indirect_call(&f.execution.ppu.cpu); }));
        CompletedDispatch record{};
        OCHECK(f.observation.read_completed(0, record));
        OCHECK(record.result.status == uint32_t(PS3_COMPILED_DISPATCH_INVALID_ADDRESS) && !record.result.lookup_attempted);
        OCHECK(f.lookup_calls == 0 && f.target_calls == 0 && f.stop_calls == 0 && f.policy.snapshot().indirect == 1);
        OCHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
    }
    {
        Fixture f;
        OCHECK(f.observation.try_record_import(0xfedcba98));
        OCHECK(!f.admit() && !f.execution.ppu.admitted && !f.execution.observation);
        OCHECK(f.observation.report().imports.total == 1 && f.clock_calls == 0);
    }
    {
        Fixture f;
        PendingDispatch wrong_pending[7]{};
        ImportRecord imports[1]{}; CompletedDispatch completed[1]{};
        StartupObservation wrong;
        OCHECK(wrong.configure(imports, 1, completed, 1, wrong_pending, 7));
        OCHECK(!native_startup_admit_discovery(f.execution, f.binding, f.services, f.recorder, f.budget, wrong));
        OCHECK(!f.execution.ppu.admitted && !f.execution.observation && wrong.valid_for_admission());
    }
}
void quota_denied(bool indirect) {
    Fixture f(indirect ? Limits{1000, 1000, 0, UINT64_C(1000000000)}
                       : Limits{1000, 0, 1000, UINT64_C(1000000000)});
    OCHECK(f.admit());
    f.execution.ppu.cpu.ctr = TARGET;
    const auto before = f.execution.ppu.cpu;
    OCHECK(!f.call([&] {
        if (indirect) ps3_indirect_call(&f.execution.ppu.cpu);
        else ps3_hle_call(0x350d454e, &f.execution.ppu.cpu);
    }));
    OCHECK(f.control->stop == (indirect ? StartupStop::IndirectBudget : StartupStop::HleSyscallBudget));
    const auto budget = f.policy.snapshot();
    OCHECK(budget.hle == 0 && budget.indirect == 0 && budget.generated == 0);
    OCHECK(budget.stop == (indirect ? infamous::discovery::StopReason::IndirectExhausted
                                 : infamous::discovery::StopReason::HleExhausted));
    const auto report = f.observation.report();
    OCHECK(report.imports.total == 0 && report.dispatch_started == 0 && report.completed.total == 0);
    OCHECK(report.pending_count == 0 && !report.immediate_outstanding);
    OCHECK(f.clock_calls == 1 && f.lookup_calls == 0 && f.target_calls == 0 && f.stop_calls == 0);
    OCHECK(f.recorder.report().published_count == 0 && !f.recorder.report().batch_active);
    OCHECK(!std::memcmp(&before, &f.execution.ppu.cpu, sizeof(before)));
    uint64_t output = UINT64_MAX;
    OCHECK(f.binding.coherent_memory.read(f.binding.coherent_memory.context, 0x10200, &output, sizeof(output)) == PS3_GUEST_MEMORY_OK && output == 0);
}
void returned_error(CoreStopScenario scenario, bool missing_target) {
    Fixture f;
    f.core_stop = scenario;
    OCHECK(f.admit());
    auto &cpu = f.execution.ppu.cpu;
    cpu.ctr = UINT64_C(0x1234567800000000) | (missing_target ? TARGET + 4 : TARGET);
    cpu.lr = 0xabcdef;
    const auto before = cpu;
    OCHECK(!f.call([&] { ps3_indirect_call(&cpu); }));
    const bool cancelled = scenario == CoreStopScenario::CancelFirst;
    const bool deadline = scenario == CoreStopScenario::DeadlineSecond;
    const auto status = cancelled ? PS3_COMPILED_DISPATCH_STOP_CANCELLED :
        deadline ? PS3_COMPILED_DISPATCH_STOP_DEADLINE : PS3_COMPILED_DISPATCH_LOOKUP_FAILURE;
    OCHECK(f.control->stop == (cancelled ? StartupStop::Cancelled :
        deadline ? StartupStop::Deadline : StartupStop::DispatchFailure));
    const auto report = f.observation.report();
    OCHECK(report.imports.total == 0 && report.dispatch_started == 1 && report.completed.total == 1);
    OCHECK(report.completed.retained_begin == 0 && report.completed.retained_end == 1 && report.completed.overwritten_end == 0);
    OCHECK(report.pending_count == 0 && !report.immediate_outstanding && f.execution.dispatch.depth == 0);
    CompletedDispatch completed{};
    OCHECK(f.observation.read_completed(0, completed));
    OCHECK(completed.sequence == 0 && completed.call_id == 0 && completed.parent_id == NO_PARENT_ID);
    OCHECK(completed.entry.captured_ctr == before.ctr && completed.entry.lr == before.lr &&
        completed.entry.sp == before.gpr[1] && completed.entry.depth == 0 && completed.depth_after == 0);
    OCHECK(completed.result.status == uint32_t(status) && completed.result.lookup_attempted == !cancelled);
    if (!cancelled)
        OCHECK(completed.result.lookup_status == uint32_t(missing_target ? PS3_COMPILED_CODE_NOT_FOUND : PS3_COMPILED_CODE_OK));
    OCHECK(f.stop_calls == (deadline ? 2u : 1u) && f.lookup_calls == (cancelled ? 0u : 1u) && f.target_calls == 0);
    OCHECK(f.stop_latched == (cancelled ? PS3_COMPILED_DISPATCH_CANCELLED :
        deadline ? PS3_COMPILED_DISPATCH_DEADLINE : PS3_COMPILED_DISPATCH_CONTINUE));
    OCHECK(f.policy.snapshot().indirect == 1 && f.clock_calls == 1);
    OCHECK(f.recorder.report().published_count == 0 && !f.recorder.report().batch_active);
    OCHECK(f.control->dispatch_count == 0 && f.control->import_count == 0);
    OCHECK(!std::memcmp(&cpu, &before, sizeof(before)) && !std::memcmp(&f.control->terminal, &before, sizeof(before)));
}
}
#ifdef PS3_NATIVE_LLE_PROFILE
void fixed_record_saturation() {
    Fixture f;
    OCHECK(f.admit());
    f.execution.observation = nullptr;
    constexpr uint32_t served_nid = 0x350d454e;
    constexpr unsigned import_calls = STARTUP_IMPORT_CAPACITY + 6;
    constexpr unsigned dispatch_calls = STARTUP_DISPATCH_CAPACITY + 5;
    for (unsigned i = 0; i < import_calls; ++i) {
        f.execution.ppu.cpu.gpr[3] = 0x10200;
        OCHECK(f.call([&] { ps3_hle_call(served_nid, &f.execution.ppu.cpu); }));
        OCHECK(f.control->stop == StartupStop::None);
    }
    OCHECK(f.control->import_count == STARTUP_IMPORT_CAPACITY);
    OCHECK(f.control->import_overflow == import_calls - STARTUP_IMPORT_CAPACITY);
    OCHECK(f.control->imports[0] == served_nid && f.control->imports[STARTUP_IMPORT_CAPACITY - 1] == served_nid);
    auto &cpu = f.execution.ppu.cpu;
    cpu.ctr = TARGET;
    for (unsigned i = 0; i < dispatch_calls; ++i) {
        OCHECK(f.call([&] { ps3_indirect_call(&cpu); }));
        OCHECK(f.control->stop == StartupStop::None);
    }
    OCHECK(f.control->dispatch_count == STARTUP_DISPATCH_CAPACITY);
    OCHECK(f.control->dispatch_overflow == dispatch_calls - STARTUP_DISPATCH_CAPACITY);
    OCHECK(f.control->dispatches[0].captured_ctr == TARGET &&
        f.control->dispatches[STARTUP_DISPATCH_CAPACITY - 1].captured_ctr == TARGET);
    OCHECK(f.control->dispatches[0].core_returned &&
        f.control->dispatches[STARTUP_DISPATCH_CAPACITY - 1].core_returned);
    OCHECK(f.target_calls == dispatch_calls);
    std::printf("STARTUP_FIXED_RECORD_SATURATION imports=%zu import_overflow=%llu dispatches=%zu dispatch_overflow=%llu\n",
        f.control->import_count, (unsigned long long)f.control->import_overflow,
        f.control->dispatch_count, (unsigned long long)f.control->dispatch_overflow);
}
#endif
int main() {
#ifdef PS3_NATIVE_LLE_PROFILE
    fixed_record_saturation();
#endif
    imports_and_syscalls();
    completed_suffix(); nested_returns(); ninth_entry(false); ninth_entry(true); selector_rollback(); rejected_entries();
    quota_denied(false); quota_denied(true);
    returned_error(CoreStopScenario::Continue, true);
    returned_error(CoreStopScenario::CancelFirst, false);
    returned_error(CoreStopScenario::DeadlineSecond, false);
    std::printf("Startup observation host checks=%d failures=%d native_table_proof=false\n", checks, failures);
    return failures ? 1 : 0;
}
