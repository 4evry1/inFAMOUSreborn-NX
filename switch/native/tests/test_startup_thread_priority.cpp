#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>
#include <initializer_list>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("THREAD_PRIORITY_FAIL line=%d expression=%s\n", __LINE__, #value); ++failures; } } while (0)
struct Terminal {};
pthread_mutex_t *terminal_guard;
void lock(void *p) { if (pthread_mutex_lock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
void unlock(void *p) { if (pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
// Translate the high guest window into the bounded host test backing. All
// coherence, reservations, permissions and admission remain production code.
struct GuestWindow {
    static constexpr uint32_t base = 0xd00ff000;
    ElfTestHostSpace *space = elf_test_host_space_create(0x3000);
    ps3_guest_memory backing = elf_test_host_space_port(space);
    StartupControl *control = nullptr;
    size_t reads = 0, writes = 0;
    bool cancel_read = false, cancel_write = false, fail_read = false, fail_write = false;
    static bool contains(uint32_t a, uint64_t n) {
        return a >= base && uint64_t(a) - base <= 0x3000 && n <= 0x3000 - (uint64_t(a) - base);
    }
    static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.commit(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.decommit(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    static ps3_guest_memory_status read(void *p, uint32_t a, void *b, size_t n) {
        auto &s = *static_cast<GuestWindow *>(p); ++s.reads;
        if (s.fail_read) return PS3_GUEST_MEMORY_UNMAPPED;
        const auto status = contains(a, n) ? s.backing.read(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (s.cancel_read) s.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status write(void *p, uint32_t a, const void *b, size_t n) {
        auto &s = *static_cast<GuestWindow *>(p); ++s.writes;
        if (s.fail_write) return PS3_GUEST_MEMORY_UNMAPPED;
        const auto status = contains(a, n) ? s.backing.write(s.backing.context, a - base, b, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
        if (s.cancel_write) s.control->cancelled.store(true);
        return status;
    }
    static ps3_guest_memory_status zero(void *p, uint32_t a, uint64_t n) {
        auto &s = *static_cast<GuestWindow *>(p);
        return contains(a, n) ? s.backing.zero(s.backing.context, a - base, n) : PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
    ~GuestWindow() { elf_test_host_space_destroy(space); }
};
struct Runtime {
    GuestWindow window;
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[2]{};
    ps3_atomic_pin_record pins[2]{};
    PpuMemoryRamRange ram{GuestWindow::base, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[8]{};
    unsigned clock_calls = 0, fail_clock_at = 0;
    static ps3_clock_status sample(void *p, uint64_t *value) {
        auto &s = *static_cast<Runtime *>(p);
        if (++s.clock_calls == s.fail_clock_at) return PS3_CLOCK_ARGUMENT;
        *value = 1; return PS3_CLOCK_OK;
    }
    ps3_clock clock{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ps3_atomic_domain &initialize() {
        if (!window.space || ps3_atomic_domain_init(&domain, window.port(), {&guard, lock, unlock}, owners, 2, pins, 2) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    Runtime(uint64_t hle_limit = 64, int32_t guest_priority = 250, bool is_writable = true,
        uint64_t permitted_length = 8192, bool has_guest_priority = true, uint32_t thread_id = 1)
        : ram{GuestWindow::base, permitted_length, is_writable}, binding(initialize(), &ram, 1),
        policy({64, hle_limit, 64, UINT64_C(1000000000)}, 0), budget(policy, clock) {
        CHECK(ps3_atomic_commit(&domain, ram.address, 8192) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get(); window.control = control.get();
        execution.guest_priority = guest_priority; execution.has_guest_priority = has_guest_priority;
        execution.ppu.cpu.thread_id = thread_id;
        execution.ppu.cpu.gpr[1] = 0xd00ff910;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.lr = 0x2cfe70;
        CHECK(recorder.configure(events, 8, PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        unsigned char bytes[8192]; std::memset(bytes, 0xa5, sizeof(bytes));
        CHECK(binding.coherent_memory.write(binding.coherent_memory.context, ram.address, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(!pthread_mutex_destroy(&guard));
    }
    void call(uint64_t id = 1, uint64_t output = 0xd00ff980, uint64_t selector = 48, uint64_t result = 0) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = id; cpu.gpr[4] = output; cpu.gpr[11] = selector;
        const auto before = cpu;
        unsigned char memory_before[8192], memory_after[8192];
        CHECK(window.backing.read(window.backing.context, 0, memory_before, sizeof(memory_before)) == PS3_GUEST_MEMORY_OK);
        const auto budget_before = policy.snapshot();
        const bool was_cancelled = control->cancelled.load();
        const auto events_before = recorder.report().published_count;
        control->stop = StartupStop::None;
        active = control.get(); control->cpu = &cpu; terminal_guard = &guard;
        try { lv2_syscall(&cpu); active = nullptr; } catch (const Terminal &) {}
        auto expected = before;
        if (control->stop == StartupStop::None) expected.gpr[3] = result;
        CHECK(!std::memcmp(&expected, &cpu, sizeof(cpu)));
        CHECK(control->import_count == 0 && control->journal.count == 0);
        CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
        CHECK(window.backing.read(window.backing.context, 0, memory_after, sizeof(memory_after)) == PS3_GUEST_MEMORY_OK);
        bool published = false;
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
        published = control->thread_priority_published;
#endif
        for (size_t i = 0; i < sizeof(memory_before); ++i) {
            const uint64_t guest = uint64_t(ram.address) + i;
            if (published && guest >= uint32_t(output) && guest < uint64_t(uint32_t(output)) + 4) continue;
            CHECK(memory_before[i] == memory_after[i]);
        }
        CHECK(recorder.report().published_count == events_before + (published ? 1 : 0));
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
        if (published) {
            const auto &event = events[events_before];
            CHECK(event.origin == PpuMemoryTraceOrigin::Hle && event.operation == PpuMemoryTraceOperation::Write);
            CHECK(event.address == uint32_t(output) && event.length == 4 && event.value == uint32_t(execution.guest_priority));
            CHECK(event.status == PS3_GUEST_MEMORY_OK && !event.byte_transfer);
        }
#endif
        CHECK(policy.snapshot().hle == budget_before.hle + (!was_cancelled && budget_before.hle < policy.limits().hle ? 1 : 0));
        CHECK(policy.snapshot().generated == budget_before.generated && policy.snapshot().indirect == budget_before.indirect);
    }
    void actual_request() {
        CHECK(!control->deadline_tick && !control->cancelled.load());
        call();
        unsigned char bytes[8]{};
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, 0xd00ff97e, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
        const unsigned char expected[]{0xa5, 0xa5, 0, 0, 0, 250, 0xa5, 0xa5};
        CHECK(control->stop == StartupStop::None);
        CHECK(!std::memcmp(bytes, expected, sizeof(bytes)));
        CHECK(recorder.report().published_count == 1);
#else
        CHECK(control->stop == StartupStop::UnsupportedSyscall);
        for (unsigned char byte : bytes) CHECK(byte == 0xa5);
        CHECK(recorder.report().published_count == 0);
#endif
        CHECK(policy.snapshot().hle == 1 && policy.snapshot().generated == 0 && policy.snapshot().indirect == 0);
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) {
    const int result = pthread_mutex_trylock(terminal_guard);
    CHECK(result == 0); if (!result) pthread_mutex_unlock(terminal_guard);
    throw Terminal{};
}
int main() {
    std::printf("THREAD_PRIORITY_LAYOUT cpu=%zu binding=%zu ppu_owner=%zu startup_owner=%zu control=%zu\n", sizeof(ppu_context), sizeof(PpuMemoryBinding), sizeof(PpuMemoryExecution), sizeof(StartupExecution), sizeof(StartupControl));
    { Runtime r; r.actual_request(); }
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
    // Owner-unit data dependence; actual ELF/main-composition acceptance is250.
    for (int32_t priority : {0, 3071}) {
        Runtime r(64, priority); r.call();
        uint32_t encoded = 0;
        CHECK(r.window.backing.read(r.window.backing.context, 0x980, &encoded, 4) == PS3_GUEST_MEMORY_OK);
        CHECK(__builtin_bswap32(encoded) == uint32_t(priority));
        CHECK(r.control->thread_priority_value == priority && r.control->thread_priority_has_value);
        CHECK(r.control->thread_priority_has_result && r.control->thread_priority_result == 0);
        CHECK(r.control->thread_priority_calls == 1 && r.control->thread_priority_returns == 1);
    }
    for (uint64_t id : {UINT64_C(0), UINT64_C(2), UINT64_C(0xffffffff)}) {
        Runtime r; const auto reads = r.window.reads, writes = r.window.writes;
        r.call(id, 0, 48, UINT64_C(0xffffffff80010005));
        CHECK(r.control->stop == StartupStop::None && r.window.reads == reads && r.window.writes == writes);
        CHECK(!r.control->thread_priority_has_value && !r.control->thread_priority_memory_checked);
        CHECK(r.control->thread_priority_has_result && r.control->thread_priority_result == UINT64_C(0xffffffff80010005));
    }
    { Runtime r; r.call(UINT64_C(0x1234567800000001), UINT64_C(0xabcdef12d00ff980));
      CHECK(r.control->stop == StartupStop::None);
      CHECK(r.control->thread_priority_id == UINT64_C(0x1234567800000001));
      CHECK(r.control->thread_priority_output == UINT64_C(0xabcdef12d00ff980)); }
    { Runtime r; r.call(1, 0xd00ff980, UINT64_C(0x100000030));
#ifdef PS3_NATIVE_STARTUP_MEMORY
      CHECK(r.control->stop == StartupStop::MissingServices);
#else
      CHECK(r.control->stop == StartupStop::UnsupportedSyscall);
#endif
      CHECK(r.control->thread_priority_calls == 0); }
    { Runtime r(64, 250, true, 8192, false); r.call();
      CHECK(r.control->stop == StartupStop::MissingServices && !r.control->thread_priority_has_value); }
    for (uint32_t id : {uint32_t(0), uint32_t(PS3_LWMUTEX_RESERVED)}) {
        Runtime r(64, 250, true, 8192, true, id); r.call();
        CHECK(r.control->stop == StartupStop::MissingServices);
    }
    for (uint64_t address : {UINT64_C(0), UINT64_C(0xd00feffc), UINT64_C(0xd0100ffe), UINT64_C(0xfffffffe)}) {
        Runtime r; const auto writes = r.window.writes; r.call(1, address);
        CHECK(r.control->stop == StartupStop::InvalidMemory && r.window.writes == writes);
        CHECK(!r.control->thread_priority_has_result && !r.control->thread_priority_write_attempted);
    }
    for (uint64_t address : {UINT64_C(0xd00ff981), UINT64_C(0xd00ffffe), UINT64_C(0xd00ff938)}) {
        Runtime r; r.call(1, address);
        CHECK(r.control->stop == StartupStop::None && r.control->thread_priority_published);
    }
    { Runtime r(64, 250, false); r.call(); CHECK(r.control->stop == StartupStop::InvalidMemory); }
    { Runtime r(64, 250, true, 12288); r.call(1, 0xd0101000);
      CHECK(r.control->stop == StartupStop::InvalidMemory && r.control->thread_priority_memory_checked);
      CHECK(r.control->thread_priority_memory_status == PS3_GUEST_MEMORY_UNMAPPED && !r.control->thread_priority_write_attempted); }
    { Runtime r; r.window.fail_read = true; r.call();
      CHECK(r.control->stop == StartupStop::InvalidMemory);
      CHECK(r.control->thread_priority_memory_checked && r.control->thread_priority_memory_status == PS3_GUEST_MEMORY_UNMAPPED);
      CHECK(!r.control->thread_priority_write_attempted); }
    { Runtime r; r.window.fail_write = true; r.call();
      CHECK(r.control->stop == StartupStop::ProviderFailure && r.control->thread_priority_write_attempted);
      CHECK(r.control->thread_priority_memory_status == PS3_GUEST_MEMORY_UNMAPPED);
      CHECK(!r.control->thread_priority_published && !r.control->thread_priority_has_result); }
    { Runtime r; r.control->cancelled.store(true); r.call();
      CHECK(r.control->stop == StartupStop::Cancelled && r.control->thread_priority_calls == 0); }
    { Runtime r; r.window.cancel_read = true; r.call();
      CHECK(r.control->stop == StartupStop::Cancelled && !r.control->thread_priority_write_attempted); }
    { Runtime r; r.window.cancel_write = true; r.call();
      CHECK(r.control->stop == StartupStop::Cancelled && r.control->thread_priority_published);
      CHECK(r.control->thread_priority_has_result && r.control->thread_priority_returns == 0); }
    { Runtime r; r.fail_clock_at = 3; r.call();
      CHECK(r.control->stop == StartupStop::BudgetClockFailure && r.control->thread_priority_published);
      CHECK(r.control->thread_priority_returns == 0); }
    { Runtime r; r.fail_clock_at = 2; r.call(2, 0);
      CHECK(r.control->stop == StartupStop::BudgetClockFailure && r.control->thread_priority_has_result);
      CHECK(r.control->thread_priority_result == UINT64_C(0xffffffff80010005) && r.control->thread_priority_returns == 0); }
    { Runtime r(0); r.call(); CHECK(r.control->stop == StartupStop::HleSyscallBudget && r.control->thread_priority_calls == 0); }
    { Runtime r; CHECK(r.recorder.begin(8));
      for (unsigned i = 0; i < 8; ++i) r.recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0, 4, i));
      r.recorder.finish(); r.call();
      CHECK(r.control->stop == StartupStop::TraceCapacity && !r.control->thread_priority_write_attempted); }
    { Runtime r; uint32_t value = 0; int stored = -1;
      CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, 0xd00ff980, &value) == PS3_GUEST_MEMORY_OK);
      r.call();
      CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, 0xd00ff980, 123, &stored) == PS3_GUEST_MEMORY_OK);
      CHECK(stored == 0); }
    // An unaligned four-byte output straddles a coherent reservation boundary.
    // Independently reserve each touched side; both must be invalidated.
    for (uint32_t reserved : {uint32_t(0xd00ff9fc), uint32_t(0xd00ffa00)}) {
        Runtime r; uint32_t value = 0; int stored = -1;
        CHECK(ps3_atomic_load_reserve32(&r.domain, r.execution.ppu.reservation, reserved, &value) == PS3_GUEST_MEMORY_OK);
        r.call(1, 0xd00ff9fe);
        CHECK(r.control->stop == StartupStop::None);
        CHECK(ps3_atomic_store_conditional32(&r.domain, r.execution.ppu.reservation, reserved, 123, &stored) == PS3_GUEST_MEMORY_OK);
        CHECK(stored == 0);
    }
    { Runtime first, second(64, 3071); first.call(); second.call();
      CHECK(first.control->thread_priority_value == 250 && second.control->thread_priority_value == 3071);
      first.call(2, 0, 48, UINT64_C(0xffffffff80010005));
      CHECK(!first.control->thread_priority_published && !first.control->thread_priority_has_value);
      CHECK(first.control->thread_priority_calls == 2 && first.control->thread_priority_returns == 2); }
#endif
    std::printf("THREAD_PRIORITY_HOST failures=%d native_runtime=false game_boot=false\n", failures);
    return failures != 0;
}
