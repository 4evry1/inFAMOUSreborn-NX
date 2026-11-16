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
#define CHECK(value) do { if (!(value)) { std::printf("SYSUTIL_CALLBACK_FAIL module_admission=assumed game_boot=false line=%d\n", __LINE__); ++failures; } } while (0)
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
#include SYSUTIL_CALLBACK_STUB
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
struct SysutilRuntime : Runtime {
    RegistrationGuard registration_guard;
    ps3_sysutil_callback_registry registry{};
    PpuMemoryTraceEvent observations[16]{};
    OperationalClock operational;
    infamous::discovery::DiscoveryBudget policy;
    DiscoveryBudgetBinding budget;
    SysutilRuntime(uint64_t hle_limit = 32, size_t capacity = 16)
        : policy({0, hle_limit, 0, UINT64_C(1000000000)}, 0), budget(policy, operational.port) {
        registration_guard.control = control.get();
        CHECK(ps3_sysutil_callback_init(&registry,
            {&registration_guard, RegistrationGuard::lock, RegistrationGuard::unlock}) == PS3_SYSUTIL_CALLBACK_OK);
        execution.sysutil_callbacks = &registry;
        CHECK(recorder.configure(observations, capacity, PpuMemoryTraceRetention::Strict, 291));
        CHECK(native_startup_admit_budgeted(execution, binding, services, recorder, budget));
        const unsigned char sentinel[] = {0x91,0x82,0x73,0x64,0x55,0x46,0x37,0x28};
        CHECK(ps3_atomic_write(&domain, 0x10128, sentinel, 8) == PS3_GUEST_MEMORY_OK);
        probe.writes = probe.reads = 0;
    }
    ~SysutilRuntime() { if (pthread_mutex_destroy(&registration_guard.mutex)) std::abort(); }
    ps3_sysutil_callback_pair read(uint32_t slot = 0) {
        registration_guard.cancel_after_unlock = false;
        ps3_sysutil_callback_pair value{};
        CHECK(ps3_sysutil_callback_read(&registry, slot, &value) == PS3_SYSUTIL_CALLBACK_OK);
        return value;
    }
    void call(uint64_t slot, uint64_t callback = 3, uint64_t userdata = 5,
        uint64_t ordinary_return = 0, uint32_t nid = 0x9d98afa0) {
        execution.ppu.cpu.gpr[3] = slot; execution.ppu.cpu.gpr[4] = callback; execution.ppu.cpu.gpr[5] = userdata;
        const auto before = execution.ppu.cpu;
        const auto counts_before = policy.snapshot(); const bool cancelled_before = control->cancelled.load();
        terminal_recorder = &recorder; terminal_guard = &guard; terminal_registration_guard = &registration_guard.mutex;
        terminal_closed = terminal_unlocked = false; g_trampoline_fn = pending_trampoline; active = control.get();
        try {
            if (nid == 0x9d98afa0) func_0055D344(&execution.ppu.cpu);
            else ps3_hle_call(nid, &execution.ppu.cpu); // Labelled unsupported-export fixture.
            CHECK(g_trampoline_fn == pending_trampoline); active = nullptr;
        } catch (const Terminal &) {
            CHECK(terminal_closed && terminal_unlocked); CHECK(g_trampoline_fn == nullptr);
        }
        auto expected = before;
        if (control->stop == StartupStop::None) expected.gpr[3] = ordinary_return;
        CHECK(!std::memcmp(&expected, &execution.ppu.cpu, sizeof(expected)));
        CHECK(!recorder.report().batch_active && recorder.report().remaining_publications == 0);
        const auto counts = policy.snapshot(); uint64_t hle = counts_before.hle;
        if (!cancelled_before && hle < policy.limits().hle) ++hle;
        CHECK(counts.hle == hle && counts.generated == 0 && counts.indirect == 0);
        std::printf("SYSUTIL_CALLBACK_HOST module_admission=assumed game_boot=false observation_scope=last_reached_handler nid=0x%x stop=%d model_calls=%zu returns=%zu bad_argument=%d hle=%llu publications=%llu actual_register_stub_body=%d\n",
            nid, int(control->stop), control->sysutil_callback_calls, control->sysutil_callback_returns,
            int(control->sysutil_callback_invalid_argument), static_cast<unsigned long long>(counts.hle),
            static_cast<unsigned long long>(recorder.report().published_count), int(nid == 0x9d98afa0));
        g_trampoline_fn = nullptr; terminal_recorder = nullptr; terminal_guard = terminal_registration_guard = nullptr;
    }
};
[[maybe_unused]] void toc_event(SysutilRuntime &r, size_t index = 0) {
    PpuMemoryTraceSample value{}; CHECK(r.recorder.read(index, value));
    CHECK(value.sequence == 291 + index && value.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(value.event.operation == PpuMemoryTraceOperation::Write && value.event.address == 0x10128);
    CHECK(value.event.length == 8 && value.event.value == UINT64_C(0x123456789abcdef0));
    CHECK(!value.event.stored && !value.event.byte_transfer && value.event.status == PS3_GUEST_MEMORY_OK);
}
void all_slots() {
    ++cases; SysutilRuntime r;
#ifdef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
    for (uint32_t slot = 0; slot < 4; ++slot) {
        r.call(slot, 3 + slot * 2, UINT32_MAX - slot);
        CHECK(r.control->stop == StartupStop::None && r.control->sysutil_callback_has_result);
        const auto pair = r.read(slot), result = r.control->sysutil_callback_result.current;
        CHECK(pair.callback_descriptor_address == 3 + slot * 2 && pair.userdata_address == UINT32_MAX - slot);
        CHECK(result.callback_descriptor_address == pair.callback_descriptor_address && result.userdata_address == pair.userdata_address);
        CHECK(r.control->sysutil_callback_result.previous.callback_descriptor_address == 0 &&
            r.control->sysutil_callback_result.previous.userdata_address == 0);
        CHECK(r.control->sysutil_callback_pending_trampoline_seen); toc_event(r, slot);
    }
    CHECK(r.control->sysutil_callback_calls == 4 && r.control->sysutil_callback_returns == 4);
    CHECK(r.operational.calls == 12 && r.probe.reads == 4 && r.probe.writes == 4);
#else
    r.call(0, 0x7765a0, 0);
    CHECK(r.control->stop == StartupStop::UnknownImport && !r.control->sysutil_callback_calls);
    CHECK(!r.read().callback_descriptor_address && !r.read().userdata_address && !r.recorder.report().published_count);
#endif
}
#ifdef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
void replacement_null_and_isolation() {
    ++cases; SysutilRuntime first, second;
    first.call(0, 3, 5); first.call(0, 7, 9);
    CHECK(first.control->sysutil_callback_result.previous.callback_descriptor_address == 3 &&
        first.control->sysutil_callback_result.previous.userdata_address == 5);
    first.call(0, 0, 11); second.call(0, 13, 15);
    CHECK(first.read().callback_descriptor_address == 0 && first.read().userdata_address == 11);
    CHECK(second.read().callback_descriptor_address == 13 && second.read().userdata_address == 15);
}
void canonical_high_pointers() {
    ++cases; SysutilRuntime r;
    r.call(0, UINT64_C(0xffffffff80000003), UINT64_C(0xffffffff87654321));
    CHECK(r.read(0).callback_descriptor_address == UINT32_C(0x80000003) && r.read(0).userdata_address == UINT32_C(0x87654321));
    CHECK(r.control->sysutil_callback_descriptor == UINT64_C(0xffffffff80000003) &&
        r.control->sysutil_callback_userdata == UINT64_C(0xffffffff87654321));
    r.call(1, UINT64_MAX, UINT64_C(0xffffffff80000005));
    CHECK(r.read(1).callback_descriptor_address == UINT32_MAX && r.read(1).userdata_address == UINT32_C(0x80000005));
    r.call(2, 0, UINT64_C(0xffffffff80000007));
    CHECK(r.read(2).callback_descriptor_address == 0 && r.read(2).userdata_address == UINT32_C(0x80000007));
    CHECK(r.control->stop == StartupStop::None && r.probe.reads == 3);
}
void invalid_canonical_slots() {
    for (uint64_t slot : {UINT64_C(4), UINT64_C(0xffffffff), UINT64_MAX, UINT64_C(0xffffffff80000000)}) {
        ++cases; SysutilRuntime r; r.call(0, 3, 5); r.probe.writes = 0;
        r.call(slot, 7, 9, UINT64_C(0xffffffff8002b102));
        CHECK(r.control->stop == StartupStop::None);
        CHECK(r.control->sysutil_callback_has_result && r.control->sysutil_callback_result.status == PS3_SYSUTIL_CALLBACK_INVALID_SLOT);
        CHECK(r.control->sysutil_callback_slot == slot && r.control->sysutil_callback_result.slot == uint32_t(slot));
        CHECK(r.read().callback_descriptor_address == 3 && r.read().userdata_address == 5);
        CHECK(r.control->sysutil_callback_result.current.callback_descriptor_address == 0 &&
            r.control->sysutil_callback_result.previous.userdata_address == 0);
        CHECK(r.probe.writes == 1 && r.recorder.report().published_count == 2); toc_event(r, 1);
    }
}
void representation_precedence() {
    struct Input { uint64_t slot, callback, userdata; SysutilCallbackArgument bad; };
    const Input values[] = {
        {UINT64_C(0x100000000), UINT64_C(0x100000003), UINT64_C(0x100000005), SysutilCallbackArgument::Slot},
        {UINT64_C(0xffffffff00000000),3,5,SysutilCallbackArgument::Slot},
        {0,UINT64_C(0x100000003),UINT64_C(0x100000005),SysutilCallbackArgument::Descriptor},
        {0,3,UINT64_C(0x100000005),SysutilCallbackArgument::Userdata},
        {4,UINT64_C(0x100000003),5,SysutilCallbackArgument::Descriptor},
        {UINT64_MAX,3,UINT64_C(0xffffffff00000005),SysutilCallbackArgument::Userdata},
        {0,UINT64_C(0xffffffff00000003),5,SysutilCallbackArgument::Descriptor}};
    for (const auto &value : values) {
        ++cases; SysutilRuntime r; r.execution.ppu.cpu.gpr[1] = UINT64_C(0x100010100);
        r.call(value.slot, value.callback, value.userdata);
        CHECK(r.control->stop == StartupStop::SysutilCallbackFailure && r.control->sysutil_callback_invalid_argument == value.bad);
        CHECK(r.control->sysutil_callback_slot == value.slot && r.control->sysutil_callback_descriptor == value.callback &&
            r.control->sysutil_callback_userdata == value.userdata);
        CHECK(!r.control->sysutil_callback_has_result && !r.control->sysutil_callback_calls);
        CHECK(!r.read().callback_descriptor_address && !r.probe.reads && !r.probe.writes && !r.recorder.report().published_count);
    }
    { ++cases; SysutilRuntime r; r.execution.sysutil_callbacks = nullptr;
      r.call(UINT64_C(0x100000000), UINT64_C(0x100000003), UINT64_C(0x100000005));
      CHECK(r.control->stop == StartupStop::MissingServices && r.control->sysutil_callback_invalid_argument == SysutilCallbackArgument::None); }
}
void early_rejections() {
    for (uint64_t stack : {UINT64_C(0x20000), UINT64_C(0x100010100), UINT64_C(0xffffffd4)}) {
        ++cases; SysutilRuntime r; r.execution.ppu.cpu.gpr[1] = stack; r.call(0);
        CHECK(r.control->stop == StartupStop::InvalidMemory && r.operational.calls == 1);
        CHECK(r.control->sysutil_callback_invalid_argument == SysutilCallbackArgument::None);
        CHECK(!r.control->sysutil_callback_has_result && !r.control->sysutil_callback_calls);
        CHECK(!r.read().callback_descriptor_address && !r.probe.reads && !r.probe.writes && !r.recorder.report().published_count);
    }
    { ++cases; SysutilRuntime r; ps3_sysutil_callback_registry uninitialized{};
      r.execution.sysutil_callbacks = &uninitialized; r.call(0);
      CHECK(r.control->stop == StartupStop::MissingServices && !r.control->sysutil_callback_calls); }
    { ++cases; SysutilRuntime r; r.control->cancelled.store(true); r.execution.sysutil_callbacks = nullptr;
      r.call(UINT64_C(0x100000000)); CHECK(r.control->stop == StartupStop::Cancelled && r.operational.calls == 0);
      CHECK(!r.control->sysutil_callback_calls && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; SysutilRuntime r; r.probe.cancel_toc_probe = true; r.call(4);
      CHECK(r.control->stop == StartupStop::Cancelled && !r.control->sysutil_callback_has_result && r.operational.calls == 1);
      CHECK(!r.control->sysutil_callback_calls && !r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; SysutilRuntime r(32,1); r.call(0,3,5); r.probe.writes = 0; r.call(1,7,9);
      CHECK(r.control->stop == StartupStop::TraceCapacity && !r.control->sysutil_callback_has_result);
      CHECK(r.read(0).callback_descriptor_address == 3 && !r.read(1).callback_descriptor_address);
      CHECK(r.control->sysutil_callback_calls == 1 && !r.probe.writes && r.recorder.report().published_count == 1); }
    { ++cases; SysutilRuntime r(0); r.call(0);
      CHECK(r.control->stop == StartupStop::HleSyscallBudget && !r.control->sysutil_callback_calls);
      CHECK(!r.probe.writes && !r.recorder.report().published_count); }
    { ++cases; SysutilRuntime r; r.call(0,3,5,0,0x9d98afa1);
      CHECK(r.control->stop == StartupStop::UnknownImport && !r.control->sysutil_callback_calls);
      CHECK(!r.read().callback_descriptor_address && !r.recorder.report().published_count); }
}
void postregistration_stop(bool fail_toc) {
    ++cases; SysutilRuntime r; const uint32_t slot = fail_toc ? 3 : 2;
    r.registration_guard.cancel_after_unlock = !fail_toc; r.probe.fail_toc = fail_toc;
    r.call(slot, 3, 5);
    CHECK(r.control->stop == (fail_toc ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    CHECK(r.control->sysutil_callback_calls == 1 && !r.control->sysutil_callback_returns && r.control->sysutil_callback_has_result);
    CHECK(r.control->sysutil_callback_result.status == PS3_SYSUTIL_CALLBACK_OK);
    CHECK(r.read(slot).callback_descriptor_address == 3 && r.read(slot).userdata_address == 5);
    CHECK(!r.probe.writes && !r.recorder.report().published_count);
    CHECK(r.bytes(0x10128) == UINT64_C(0x9182736455463728));
}
#endif
}
int main() {
    failures = cases = 0; host_tick = 0; all_slots();
#ifdef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
    replacement_null_and_isolation(); canonical_high_pointers(); invalid_canonical_slots();
    representation_precedence(); early_rejections(); postregistration_stop(false); postregistration_stop(true);
#endif
    std::printf("Sysutil callback host cases=%d failures=%d actual_stub_body=true registration_only=true native_runtime=false module_admission=assumed game_boot=false\n", cases, failures);
    return failures ? 1 : 0;
}
