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
pthread_mutex_t *terminal_guard;
bool terminal_closed, terminal_unlocked;
uint64_t host_tick;
void guard_lock(void *p) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(p)); }
void guard_unlock(void *p) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(p)); }
struct MemoryProbe {
    ps3_guest_memory backing{};
    StartupControl *control = nullptr;
    bool cancel_output = false, fail_toc = false, cancel_read = false;
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
    void invoke() {
        terminal_recorder = &recorder; terminal_guard = &guard;
        terminal_closed = terminal_unlocked = false;
        active = control.get();
        try { ps3_hle_call(0x350d454e, &execution.ppu.cpu); active = nullptr; }
        catch (const Terminal &) { CHECK(terminal_closed); CHECK(terminal_unlocked); }
        terminal_recorder = nullptr; terminal_guard = nullptr;
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
    throw Terminal{}; // Observations above precede stack unwinding.
}

namespace {
void legacy() {
    ++cases; Runtime r;
    CHECK(native_startup_admit(r.execution, r.binding, r.services));
    r.invoke();
    CHECK(r.execution.ppu.cpu.gpr[3] == 0);
    CHECK(r.control->journal.count == 2);
    CHECK(r.bytes(0x10200) == 0x1234);
}
void suffix_pair(bool alias) {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix, 17));
    if (alias) r.execution.ppu.cpu.gpr[3] = 0x10128;
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    r.invoke();
    const auto report = r.recorder.report();
    CHECK(!report.batch_active && report.published_count == 2 && report.retained_count == 1);
    CHECK(report.overwritten_begin == 17 && report.overwritten_end == 18);
    PpuMemoryTraceSample sample{};
    CHECK(r.recorder.read(0, sample));
    CHECK(sample.sequence == 18 && sample.event.address == 0x10128);
    CHECK(r.control->journal.count == 0);
    CHECK(r.bytes(0x10128) == UINT64_C(0x123456789abcdef0));
    if (!alias) CHECK(r.bytes(0x10200) == 0x1234);
    CHECK(r.execution.ppu.cpu.gpr[3] == 0);
}
void strict_insufficient() {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Strict));
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    const ppu_context before = r.execution.ppu.cpu;
    r.invoke();
    CHECK(r.control->stop == StartupStop::TraceCapacity);
    CHECK(std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)) == 0);
    CHECK(r.bytes(0x10200) == 0 && r.bytes(0x10128) == 0);
    CHECK(r.recorder.report().published_count == 0);
}
void external_credit() {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix));
    CHECK(r.recorder.begin(2));
    CHECK(!native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(r.recorder.report().batch_active && r.recorder.report().remaining_publications == 2);
    CHECK(!r.execution.ppu.admitted && !r.execution.ppu.recorder && !r.control->cpu);
    r.recorder.finish();
}
void admission_selection() {
    ++cases; Runtime r;
    const ppu_context before = r.execution.ppu.cpu;
    CHECK(!native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(!r.execution.ppu.recorder && !r.execution.ppu.admitted && !r.control->cpu);
    CHECK(r.recorder.configure(r.events, 4, PpuMemoryTraceRetention::Suffix));
    r.execution.ppu.trace = &r.control->journal;
    CHECK(!native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(r.execution.ppu.trace == &r.control->journal && !r.execution.ppu.recorder);
    r.execution.ppu.trace = nullptr;
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(!native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(r.execution.ppu.recorder == &r.recorder && !r.execution.ppu.trace);
    CHECK(std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)) == 0);
}
void preflight_cancel() {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix));
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    const ppu_context before = r.execution.ppu.cpu;
    r.control->cancelled.store(true);
    r.invoke();
    CHECK(r.control->stop == StartupStop::Cancelled);
    CHECK(std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)) == 0);
    CHECK(r.bytes(0x10200) == 0 && r.bytes(0x10128) == 0);
    CHECK(r.recorder.report().published_count == 0 && !r.recorder.report().batch_active);
}
void partial_publication(bool fail_toc) {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix, 91));
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    const ppu_context before = r.execution.ppu.cpu;
    r.probe.fail_toc = fail_toc;
    r.probe.cancel_output = !fail_toc;
    r.invoke();
    CHECK(terminal_closed && terminal_unlocked);
    CHECK(r.control->stop == (fail_toc ? StartupStop::ProviderFailure : StartupStop::Cancelled));
    CHECK(!std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)));
    CHECK(r.bytes(0x10200) == 0x1234 && r.bytes(0x10128) == 0);
    CHECK(r.probe.writes == 1);
    const auto report = r.recorder.report();
    CHECK(!report.batch_active && report.remaining_publications == 0 && report.published_count == 1);
    PpuMemoryTraceSample sample{};
    CHECK(r.recorder.read(0, sample));
    CHECK(sample.sequence == 91 && sample.event.origin == PpuMemoryTraceOrigin::Hle);
    CHECK(sample.event.operation == PpuMemoryTraceOperation::Write);
    CHECK(sample.event.address == 0x10200 && sample.event.length == 8 && sample.event.value == 0x1234);
    CHECK(sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored && !sample.event.byte_transfer);
}
void cancellation_from_probe() {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix));
    CHECK(native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    const ppu_context before = r.execution.ppu.cpu;
    r.probe.cancel_read = true;
    r.invoke();
    CHECK(r.control->stop == StartupStop::Cancelled);
    CHECK(!std::memcmp(&before, &r.execution.ppu.cpu, sizeof(before)));
    CHECK(r.probe.writes == 0 && r.recorder.report().published_count == 0);
}
void lower_admission_failure() {
    ++cases; Runtime r;
    CHECK(r.recorder.configure(r.events, 1, PpuMemoryTraceRetention::Suffix));
    r.services.memory = nullptr;
    CHECK(!native_startup_admit_recorded(r.execution, r.binding, r.services, r.recorder));
    CHECK(!r.execution.ppu.recorder && !r.execution.ppu.trace && !r.execution.ppu.admitted && !r.control->cpu);
    CHECK(!r.recorder.report().batch_active && r.recorder.report().published_count == 0);
}
}
int main() {
    legacy(); suffix_pair(false); suffix_pair(true); strict_insufficient(); external_credit();
    admission_selection(); preflight_cancel();
    partial_publication(false); partial_publication(true); cancellation_from_probe(); lower_admission_failure();
    std::printf("STARTUP_RECORDER_HOST cases=%d failures=%d native_stub_proof=0\n", cases, failures);
    return failures ? 1 : 0;
}
