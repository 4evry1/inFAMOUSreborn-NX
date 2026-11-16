#include "../adapters/startup_execution.cpp"
extern "C" {
#include "guest_space_host.h"
}
#include <memory>
#include <pthread.h>
#include <thread>
#include <initializer_list>

namespace {
int failures;
#define CHECK(value) do { if (!(value)) { std::printf("SPU_INITIALIZE_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
struct Terminal {};
pthread_mutex_t *terminal_guard;
pthread_mutex_t *terminal_resource_guard;
void lock(void *p) { if (pthread_mutex_lock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
void unlock(void *p) { if (pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p))) std::abort(); }
struct ResourceGuard {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    StartupControl *control = nullptr;
    bool cancel_after_unlock = false;
    static void lock(void *p) { ::lock(&static_cast<ResourceGuard *>(p)->mutex); }
    static void unlock(void *p) {
        auto &self = *static_cast<ResourceGuard *>(p);
        ::unlock(&self.mutex);
        if (self.cancel_after_unlock) {
            self.cancel_after_unlock = false;
            self.control->cancelled.store(true, std::memory_order_release);
        }
    }
};
struct Runtime {
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[2]{};
    ps3_atomic_pin_record pins[2]{};
    PpuMemoryRamRange ram{0x10000, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    std::unique_ptr<StartupControl> control{new StartupControl};
    StartupExecution execution{};
    ResourceGuard resource_guard;
    ps3_spu_resource_policy resources{};
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[4]{};
    unsigned clock_calls = 0;
    bool fail_post_operation_clock = false;
    static ps3_clock_status sample(void *context, uint64_t *value) {
        auto &self = *static_cast<Runtime *>(context);
        ++self.clock_calls;
        if (self.fail_post_operation_clock && self.clock_calls == 2) return PS3_CLOCK_ARGUMENT;
        *value = 1; return PS3_CLOCK_OK;
    }
    ps3_clock clock{this, sample};
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    ps3_atomic_domain &initialize() {
        if (!space || ps3_atomic_domain_init(&domain, elf_test_host_space_port(space),
            {&guard, lock, unlock}, owners, 2, pins, 2) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    Runtime(uint64_t hle_limit = 64) : binding(initialize(), &ram, 1),
        policy({0, hle_limit, 0, UINT64_C(1000000000)}, 0), budget(policy, clock) {
        CHECK(ps3_atomic_commit(&domain, ram.address, ram.length) == PS3_GUEST_MEMORY_OK);
        services.memory = &binding.coherent_memory;
        execution.control = control.get();
        resource_guard.control = control.get();
        CHECK(ps3_spu_resource_policy_init(&resources,
            {&resource_guard, ResourceGuard::lock, ResourceGuard::unlock}) == PS3_SPU_POLICY_OK);
        execution.spu_resources = &resources;
        execution.ppu.cpu.gpr[1] = 0x10100;
        execution.ppu.cpu.gpr[2] = UINT64_C(0x123456789abcdef0);
        execution.ppu.cpu.lr = 0x193460;
        CHECK(recorder.configure(events, 4, PpuMemoryTraceRetention::Strict, 0));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
    }
    ~Runtime() {
        if (execution.ppu.admitted) CHECK(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(!pthread_mutex_destroy(&guard));
        CHECK(!pthread_mutex_destroy(&resource_guard.mutex));
        elf_test_host_space_destroy(space);
    }
    void call(uint64_t usable, uint64_t raw, uint64_t selector = 169, uint64_t result = 0) {
        auto &cpu = execution.ppu.cpu;
        cpu.gpr[3] = usable; cpu.gpr[4] = raw; cpu.gpr[11] = selector;
        control->stop = StartupStop::None;
        const auto before = cpu;
        const auto counts = policy.snapshot();
        const bool cancelled = control->cancelled.load();
        unsigned char memory_before[8192], memory_after[8192];
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, ram.address,
            memory_before, sizeof(memory_before)) == PS3_GUEST_MEMORY_OK);
        active = control.get(); control->cpu = &cpu; terminal_guard = &guard;
        terminal_resource_guard = &resource_guard.mutex;
        try { lv2_syscall(&cpu); active = nullptr; } catch (const Terminal &) {}
        auto expected = before;
        if (control->stop == StartupStop::None) expected.gpr[3] = result;
        CHECK(!std::memcmp(&expected, &cpu, sizeof(cpu)));
        CHECK(control->import_count == 0 && control->journal.count == 0);
        CHECK(recorder.report().published_count == 0 && !recorder.report().batch_active);
        CHECK(recorder.report().remaining_publications == 0);
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, ram.address,
            memory_after, sizeof(memory_after)) == PS3_GUEST_MEMORY_OK);
        CHECK(!std::memcmp(memory_before, memory_after, sizeof(memory_before)));
        CHECK(policy.snapshot().hle == counts.hle + (!cancelled && counts.hle < policy.limits().hle ? 1 : 0));
        CHECK(policy.snapshot().generated == 0 && policy.snapshot().indirect == 0);
    }
};
}
extern "C" uint64_t armGetSystemTick(void) { return 0; }
extern "C" [[noreturn]] void threadExit(void) {
    const int result = pthread_mutex_trylock(terminal_guard);
    CHECK(result == 0); if (!result) pthread_mutex_unlock(terminal_guard);
    const int resource_result = pthread_mutex_trylock(terminal_resource_guard);
    CHECK(resource_result == 0);
    if (!resource_result) pthread_mutex_unlock(terminal_resource_guard);
    throw Terminal{};
}
int main() {
    std::printf("SPU_INITIALIZE_LAYOUT scope=host_fixture cpu=%zu binding=%zu ppu_owner=%zu startup_owner=%zu control=%zu policy=%zu result=%zu owner_policy_offset=%zu\n",
        sizeof(ppu_context), sizeof(PpuMemoryBinding), sizeof(PpuMemoryExecution), sizeof(StartupExecution),
        sizeof(StartupControl), sizeof(ps3_spu_resource_policy), sizeof(ps3_spu_policy_result),
        offsetof(StartupExecution, spu_resources));
#ifdef PS3_NATIVE_STARTUP_SPU_INITIALIZE
    for (uint64_t usable : {UINT64_C(0), UINT64_C(6), UINT64_C(0xffffffff), UINT64_C(0x1234567800000006)}) {
        Runtime r;
        for (uint64_t raw : {UINT64_C(0), UINT64_C(5), UINT64_C(6), UINT64_C(0xffffffff), UINT64_C(0xabcdef1200000005)}) {
            const auto before = ps3_spu_resource_policy_snapshot(&r.resources).current;
            const bool valid = uint32_t(raw) <= 5;
            r.call(usable, raw, 169, valid ? 0 : UINT64_C(0xffffffff80010002));
            CHECK(r.control->stop == StartupStop::None && r.control->spu_initialize_has_result);
            const auto result = r.control->spu_initialize_result;
            CHECK(result.has_snapshot && result.previous.raw_limit == before.raw_limit);
            CHECK(result.status == (valid ? PS3_SPU_POLICY_OK : PS3_SPU_POLICY_INVALID_RAW));
            CHECK(result.current.raw_limit == (valid ? uint32_t(raw) : before.raw_limit));
            CHECK(result.current.managed_limit == 6 - result.current.raw_limit);
            CHECK(r.control->spu_initialize_usable == usable && r.control->spu_initialize_raw == raw);
        }
        CHECK(r.control->spu_initialize_calls == 5 && r.control->spu_initialize_returns == 5);
        r.call(0, 5); r.call(6, 5); r.call(6, 0);
        CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.managed_limit == 6);
    }
    { Runtime r; r.call(6, 0, UINT64_C(0x1000000a9));
#ifdef PS3_NATIVE_STARTUP_MEMORY
      CHECK(r.control->stop == StartupStop::MissingServices && r.control->spu_initialize_calls == 0); }
#else
      CHECK(r.control->stop == StartupStop::UnsupportedSyscall && r.control->spu_initialize_calls == 0); }
#endif
    { Runtime r; r.execution.spu_resources = nullptr; r.call(6, 0);
      CHECK(r.control->stop == StartupStop::MissingServices && !r.control->spu_initialize_has_result); }
    { Runtime r; r.resources.guard.lock = nullptr; r.call(6, 0);
      CHECK(r.control->stop == StartupStop::SpuPolicyFailure && r.control->spu_initialize_has_result);
      CHECK(r.control->spu_initialize_result.status == PS3_SPU_POLICY_INVALID_STATE); }
    { Runtime r; r.resources.limits.managed_limit = 3; r.call(6, 0);
      CHECK(r.control->stop == StartupStop::SpuPolicyFailure);
      CHECK(r.resources.limits.managed_limit == 3 && r.resources.limits.raw_limit == 0); }
    { Runtime r; r.control->cancelled.store(true); r.call(6, 5);
      CHECK(r.control->stop == StartupStop::Cancelled && r.control->spu_initialize_calls == 0);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 0); }
    { Runtime r; r.resource_guard.cancel_after_unlock = true; r.call(6, 5);
      CHECK(r.control->stop == StartupStop::Cancelled && r.control->spu_initialize_has_result);
      CHECK(r.control->spu_initialize_returns == 0 && r.control->spu_initialize_result.current.raw_limit == 5);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 5); }
    { Runtime r(0); r.call(6, 5);
      CHECK(r.control->stop == StartupStop::HleSyscallBudget && r.control->spu_initialize_calls == 0);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 0); }
    { Runtime r; r.fail_post_operation_clock = true; r.call(6, 5);
      CHECK(r.control->stop == StartupStop::BudgetClockFailure && r.control->spu_initialize_has_result);
      CHECK(r.control->spu_initialize_returns == 0 && r.control->spu_initialize_result.current.raw_limit == 5);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 5); }
    { Runtime r, independent;
      r.call(6, 5); independent.call(0, 0);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 5);
      CHECK(ps3_spu_resource_policy_snapshot(&independent.resources).current.raw_limit == 0);
      std::atomic<unsigned> violations{0};
      auto configure = [&](uint32_t raw) {
          for (unsigned i = 0; i < 2000; ++i) {
              const auto result = ps3_spu_resource_policy_configure(&r.resources, UINT32_MAX, raw);
              if (result.status != PS3_SPU_POLICY_OK || !result.has_snapshot ||
                  result.current.raw_limit != raw || result.current.managed_limit != 6 - raw ||
                  result.previous.managed_limit != 6 - result.previous.raw_limit) ++violations;
          }
      };
      std::thread first(configure, 0), second(configure, 5); first.join(); second.join();
      CHECK(violations.load() == 0); }
#else
    { Runtime r; r.call(6, 5); CHECK(r.control->stop == StartupStop::UnsupportedSyscall);
      CHECK(r.control->spu_initialize_calls == 0 && !r.control->spu_initialize_has_result);
      CHECK(ps3_spu_resource_policy_snapshot(&r.resources).current.raw_limit == 0); }
#endif
    std::printf("SPU_INITIALIZE_HOST failures=%d native_runtime=false module_admission=assumed game_boot=false\n", failures);
    return failures != 0;
}
