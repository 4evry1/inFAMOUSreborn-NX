#include "ppu_memory_context.h"
#include "discovery_budget_binding.h"
extern "C" {
#include "guest_space_host.h"
}
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace infamous::discovery;
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL line=%d expression=%s\n", __LINE__, #x); } } while (0)
struct Stopped { PpuMemoryFailure failure; };
struct Runtime {
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[4]{};
    ps3_atomic_pin_record pins[4]{};
    PpuMemoryRamRange ram{0x10000, 8192, true};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    PpuMemoryExecution execution;
    PpuMemoryTraceRecorder recorder;
    PpuMemoryTraceEvent events[8]{};
    bool cancel = false, external_credit = false;
    unsigned clock_calls = 0;
    std::uint64_t microseconds = 1;
    ps3_clock clock{this, sample};
    DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    static void lock(void *p) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(p)); }
    static void unlock(void *p) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p)); }
    static ps3_clock_status sample(void *p, std::uint64_t *out) {
        auto &self = *static_cast<Runtime *>(p); ++self.clock_calls; *out = self.microseconds; return PS3_CLOCK_OK;
    }
    static bool cancelled(void *p) { return static_cast<Runtime *>(p)->cancel; }
    [[noreturn]] static void stopped(void *p, PpuMemoryFailure failure) {
        auto &self = *static_cast<Runtime *>(p);
        CHECK(self.recorder.report().batch_active == self.external_credit);
        const int status = pthread_mutex_trylock(&self.guard);
        CHECK(status == 0); if (status == 0) pthread_mutex_unlock(&self.guard);
        throw Stopped{failure};
    }
    ps3_atomic_domain &initialize() {
        if (!space || ps3_atomic_domain_init(&domain, elf_test_host_space_port(space),
            {&guard, lock, unlock}, owners, 4, pins, 4) != PS3_GUEST_MEMORY_OK) std::abort();
        return domain;
    }
    explicit Runtime(std::uint64_t limit, bool selected = true)
        : binding(initialize(), &ram, 1), policy({limit, 8, 8, 1000000}, 1000), budget(policy, clock) {
        services.memory = &binding.coherent_memory;
        CHECK(ps3_atomic_commit(&domain, ram.address, ram.length) == PS3_GUEST_MEMORY_OK);
        CHECK(recorder.configure(events, 8, PpuMemoryTraceRetention::Suffix));
        execution.recorder = &recorder;
        if (selected) execution.budget = &budget;
    }
    bool admit() { return ppu_memory_admit(execution, binding, services, {this, cancelled, stopped}) == PpuMemoryAdmission::Ready; }
    ~Runtime() {
        if (execution.admitted) CHECK(ppu_memory_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        CHECK(pthread_mutex_destroy(&guard) == 0);
        elf_test_host_space_destroy(space);
    }
    std::uint32_t raw() {
        std::uint32_t bytes = 0;
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, 0x10000, &bytes, 4) == PS3_GUEST_MEMORY_OK);
        return __builtin_bswap32(bytes);
    }
    template<class F> PpuMemoryFailure stopped_by(F operation) {
        try { operation(); CHECK(false); }
        catch (const Stopped &stopped) { return stopped.failure; }
        return {PpuMemoryStop::InvalidBinding, PS3_GUEST_MEMORY_PLATFORM_FAILURE, 0, 0, false};
    }
};
static void generated_limits()
{
    Runtime r(2); CHECK(r.admit()); CHECK(r.clock_calls == 0);
    ppu_native_write32(&r.execution.cpu, 0x10000, 0x12345678);
    CHECK(ppu_native_read32(&r.execution.cpu, 0x10000) == 0x12345678);
    CHECK(r.clock_calls == 2 && r.policy.snapshot().generated == 2);
    const auto publications = r.recorder.report().published_count;
    const auto stopped = r.stopped_by([&] { ppu_native_write32(&r.execution.cpu, 0x10000, 0xdeadbeef); });
    CHECK(stopped.reason == PpuMemoryStop::BudgetStop && stopped.status == PS3_GUEST_MEMORY_OK);
    CHECK(r.raw() == 0x12345678 && r.recorder.report().published_count == publications);
    CHECK(r.policy.snapshot().generated == 2 && r.clock_calls == 3);
    CHECK(r.budget.reason() == StopReason::GeneratedExhausted);
}
static void precedence_and_noops()
{
    { Runtime r(0); CHECK(r.admit()); r.cancel = true;
      const auto stopped = r.stopped_by([&] { ppu_native_write32(&r.execution.cpu, 0x10000, 1); });
      CHECK(stopped.reason == PpuMemoryStop::Cancelled && r.clock_calls == 0 && r.raw() == 0);
      CHECK(r.budget.reason() == StopReason::None && r.policy.snapshot().generated == 0); }
    { Runtime r(0); CHECK(r.admit()); r.services.memory = nullptr;
      const auto stopped = r.stopped_by([&] { ppu_native_read32(&r.execution.cpu, 0x10000); });
      CHECK(stopped.reason == PpuMemoryStop::InvalidBinding && r.clock_calls == 0);
      CHECK(r.policy.snapshot().generated == 0); }
    { Runtime r(0); CHECK(r.admit());
      ppu_native_read_bytes(&r.execution.cpu, UINT64_MAX, nullptr, 0);
      ppu_native_write_bytes(&r.execution.cpu, UINT64_MAX, nullptr, 0);
      CHECK(r.clock_calls == 0 && r.policy.snapshot().generated == 0 && r.recorder.report().published_count == 0);
      const auto stopped = r.stopped_by([&] { ppu_native_read32(&r.execution.cpu, UINT64_MAX); });
      CHECK(stopped.reason == PpuMemoryStop::BudgetStop && r.clock_calls == 1);
      CHECK(r.recorder.report().published_count == 0); }
    { Runtime r(2); CHECK(r.admit());
      r.stopped_by([&] { ppu_native_read32(&r.execution.cpu, 0x14000); });
      CHECK(r.policy.snapshot().generated == 1 && r.clock_calls == 1 && r.recorder.report().published_count == 0); }
}
static void admission_and_external_credit()
{
    { Runtime r(2); CHECK(r.budget.admit(Category::Hle).admitted);
      unsigned char before[sizeof(r.owners)]; std::memcpy(before, r.owners, sizeof(r.owners));
      CHECK(!r.admit() && !r.execution.admitted); CHECK(r.clock_calls == 1);
      CHECK(!std::memcmp(before, r.owners, sizeof(r.owners)));
      CHECK(r.policy.snapshot().generated == 0); }
    { Runtime r(2); CHECK(r.admit());
      CHECK(r.recorder.begin(2)); r.external_credit = true;
      const auto stopped = r.stopped_by([&] { ppu_native_read32(&r.execution.cpu, 0x10000); });
      CHECK(stopped.reason == PpuMemoryStop::InvalidBinding && r.clock_calls == 0);
      CHECK(r.recorder.report().batch_active && r.recorder.report().remaining_publications == 2);
      r.recorder.finish(); r.external_credit = false; }
}
static void actual_provider_failure()
{
    Runtime r(2); CHECK(r.admit());
    CHECK(r.binding.coherent_memory.decommit(r.binding.coherent_memory.context, 0x10000, 4096) == PS3_GUEST_MEMORY_OK);
    const auto stopped = r.stopped_by([&] { ppu_native_write32(&r.execution.cpu, 0x10000, 33); });
    CHECK(stopped.reason == PpuMemoryStop::ProviderFailure && stopped.status == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(r.clock_calls == 1 && r.policy.snapshot().generated == 1);
    CHECK(r.recorder.report().published_count == 0 && !r.recorder.report().batch_active);
}
static void reservation_and_disabled()
{
    { Runtime r(1); CHECK(r.admit());
      CHECK(ppu_native_load_reserve32(&r.execution.cpu, 0x10000) == 0);
      unsigned char reservation_bytes[sizeof(r.owners)]; std::memcpy(reservation_bytes, r.owners, sizeof(r.owners));
      ppu_context cpu_before = r.execution.cpu;
      const auto count = r.recorder.report().published_count;
      const auto stopped = r.stopped_by([&] { ppu_native_store_conditional32(&r.execution.cpu, 0x10000, 42); });
      CHECK(stopped.reason == PpuMemoryStop::BudgetStop && r.raw() == 0);
      CHECK(!std::memcmp(reservation_bytes, r.owners, sizeof(r.owners)));
      CHECK(!std::memcmp(&cpu_before, &r.execution.cpu, sizeof(cpu_before)));
      CHECK(r.recorder.report().published_count == count && r.policy.snapshot().generated == 1); }
    { Runtime r(0, false); r.execution.recorder = nullptr; CHECK(r.admit());
      ppu_native_write32(&r.execution.cpu, 0x10000, 17);
      CHECK(ppu_native_load_reserve32(&r.execution.cpu, 0x10000) == 17);
      CHECK(ppu_native_store_conditional32(&r.execution.cpu, 0x10000, 18));
      CHECK(r.raw() == 18 && r.clock_calls == 0 && r.policy.snapshot().generated == 0); }
}
int main() {
    generated_limits(); precedence_and_noops(); admission_and_external_credit(); actual_provider_failure(); reservation_and_disabled();
    std::printf("PPU_MEMORY_BUDGET_%s checks=%u failures=%u real_domain_direct_helpers=true native_execution=false\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
