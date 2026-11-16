#include "../adapters/ppu_memory_context.h"
extern "C" {
#include "guest_space_host.h"
}
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>

void func_F0000000(ppu_context *);
void func_F0000001(ppu_context *);

namespace {
int failures, cases;
#define CHECK(value) do { if (!(value)) { std::printf("GENERATED_RECORDER_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
struct Stopped { PpuMemoryFailure failure; };
struct StopState {
    bool cancel = false, closed_at_stop = false, guard_returned = false;
    pthread_mutex_t *mutex = nullptr;
    PpuMemoryTraceRecorder *recorder = nullptr;
};
bool cancelled(void *context) { return static_cast<StopState *>(context)->cancel; }
[[noreturn]] void stop(void *context, PpuMemoryFailure failure)
{
    auto &state = *static_cast<StopState *>(context);
    state.closed_at_stop = !state.recorder || !state.recorder->report().batch_active;
    const int locked = pthread_mutex_trylock(state.mutex);
    state.guard_returned = locked == 0;
    if (locked == 0) pthread_mutex_unlock(state.mutex);
    throw Stopped{failure}; // Batch closure is observed BEFORE C++ unwinding.
}
void lock(void *context) { pthread_mutex_lock(static_cast<pthread_mutex_t *>(context)); }
void unlock(void *context) { pthread_mutex_unlock(static_cast<pthread_mutex_t *>(context)); }

struct Probe {
    ps3_guest_memory backing{};
    PpuMemoryTraceRecorder *recorder = nullptr;
    StopState *stop_state = nullptr;
    unsigned operations = 0;
    bool saw_active = false, cancel_after_write = false;
    void observe() { ++operations; saw_active = saw_active || (recorder && recorder->report().batch_active); }
    static ps3_guest_memory_status commit(void *opaque, uint32_t address, uint64_t size) {
        auto &p = *static_cast<Probe *>(opaque); return p.backing.commit(p.backing.context, address, size);
    }
    static ps3_guest_memory_status decommit(void *opaque, uint32_t address, uint64_t size) {
        auto &p = *static_cast<Probe *>(opaque); return p.backing.decommit(p.backing.context, address, size);
    }
    static ps3_guest_memory_status read(void *opaque, uint32_t address, void *data, size_t size) {
        auto &p = *static_cast<Probe *>(opaque); p.observe(); return p.backing.read(p.backing.context, address, data, size);
    }
    static ps3_guest_memory_status write(void *opaque, uint32_t address, const void *data, size_t size) {
        auto &p = *static_cast<Probe *>(opaque); p.observe();
        const auto result = p.backing.write(p.backing.context, address, data, size);
        if (p.cancel_after_write && result == PS3_GUEST_MEMORY_OK) p.stop_state->cancel = true;
        return result;
    }
    static ps3_guest_memory_status zero(void *opaque, uint32_t address, uint64_t size) {
        auto &p = *static_cast<Probe *>(opaque); p.observe(); return p.backing.zero(p.backing.context, address, size);
    }
    ps3_guest_memory port() { return {this, commit, decommit, read, write, zero}; }
};
struct Runtime {
    static constexpr uint32_t base = 0x10000, readonly = 0x20000;
    ElfTestHostSpace *space = elf_test_host_space_create(0x30000);
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[4]{};
    ps3_atomic_pin_record pins[2]{};
    Probe probe{};
    StopState boundary{};
    PpuMemoryRamRange ranges[2]{{base, 8192, true}, {readonly, 4096, false}};
    PpuMemoryBinding binding;
    Ps3PpuExecutionServices services{};
    ps3_atomic_domain &initialize() {
        if (!space) std::abort();
        probe.backing = elf_test_host_space_port(space);
        boundary.mutex = &mutex; probe.stop_state = &boundary;
        if (ps3_atomic_domain_init(&domain, probe.port(), {&mutex, lock, unlock}, owners, 4, pins, 2) != PS3_GUEST_MEMORY_OK)
            std::abort();
        return domain;
    }
    Runtime() : binding(initialize(), ranges, 2) {
        services.memory = &binding.coherent_memory;
        CHECK(ps3_atomic_commit(&domain, base, 8192) == PS3_GUEST_MEMORY_OK);
        CHECK(ps3_atomic_commit(&domain, readonly, 4096) == PS3_GUEST_MEMORY_OK);
    }
    ~Runtime() { CHECK(pthread_mutex_destroy(&mutex) == 0); elf_test_host_space_destroy(space); }
    PpuMemoryAdmission admit(PpuMemoryExecution &owner) {
        boundary.recorder = owner.recorder; probe.recorder = owner.recorder;
        return ppu_memory_admit(owner, binding, services, {&boundary, cancelled, stop});
    }
    void release(PpuMemoryExecution &owner) {
        if (owner.admitted) CHECK(ppu_memory_release_after_join(owner) == PS3_GUEST_MEMORY_OK);
    }
    void put32(uint32_t address, uint32_t value) {
        unsigned char bytes[4]{static_cast<unsigned char>(value >> 24), static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
        CHECK(binding.coherent_memory.write(binding.coherent_memory.context, address, bytes, 4) == PS3_GUEST_MEMORY_OK);
    }
    uint32_t get32(uint32_t address) {
        unsigned char bytes[4]{};
        CHECK(binding.coherent_memory.read(binding.coherent_memory.context, address, bytes, 4) == PS3_GUEST_MEMORY_OK);
        return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
    }
};
template<class Operation> void expect_stop(Operation operation, PpuMemoryStop wanted)
{
    bool caught = false;
    try { operation(); } catch (const Stopped &stopped) { caught = true; CHECK(stopped.failure.reason == wanted); }
    CHECK(caught);
}
void expect_event(PpuMemoryTraceRecorder &recorder, size_t index, uint64_t sequence,
                  PpuMemoryTraceOperation operation, uint64_t value, bool stored = false)
{
    PpuMemoryTraceSample sample{};
    CHECK(recorder.read(index, sample));
    CHECK(sample.sequence == sequence && sample.event.origin == PpuMemoryTraceOrigin::Generated);
    CHECK(sample.event.operation == operation && sample.event.value == value && sample.event.stored == stored);
    CHECK(sample.event.status == PS3_GUEST_MEMORY_OK);
}

void null_and_legacy_controls()
{
    for (bool legacy : {false, true}) {
        ++cases; Runtime runtime; PpuMemoryExecution owner;
        PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceJournal journal{events, 1, 0};
        if (legacy) owner.trace = &journal;
        CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
        ppu_native_write32(&owner.cpu, Runtime::base, 0x12345678);
        CHECK(runtime.get32(Runtime::base) == 0x12345678);
        if (legacy) {
            CHECK(journal.count == 1 && events[0].value == 0x12345678);
            expect_stop([&] { ppu_native_write32(&owner.cpu, Runtime::readonly, 1); }, PpuMemoryStop::PermissionDenied);
            expect_stop([&] { ppu_native_write32(&owner.cpu, Runtime::base, 1); }, PpuMemoryStop::TraceCapacity);
            CHECK(runtime.get32(Runtime::base) == 0x12345678);
        } else CHECK(ppu_native_read32(&owner.cpu, Runtime::base) == 0x12345678);
        runtime.release(owner);
    }
}
void all_operation_families()
{
    ++cases; Runtime runtime; PpuMemoryExecution owner;
    PpuMemoryTraceEvent events[3]{}; PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events, 3, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
    CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
    ppu_native_write8(&owner.cpu, Runtime::base, 0x81);
    ppu_native_write16(&owner.cpu, Runtime::base + 2, 0x2233);
    ppu_native_write32(&owner.cpu, Runtime::base + 4, 0x44556677);
    ppu_native_write64(&owner.cpu, Runtime::base + 8, UINT64_C(0x8899aabbccddeeff));
    CHECK(ppu_native_read8(&owner.cpu, Runtime::base) == 0x81);
    CHECK(ppu_native_read16(&owner.cpu, Runtime::base + 2) == 0x2233);
    CHECK(ppu_native_read32(&owner.cpu, Runtime::base + 4) == 0x44556677);
    CHECK(ppu_native_read64(&owner.cpu, Runtime::base + 8) == UINT64_C(0x8899aabbccddeeff));
    unsigned char input[16], output[16]{};
    for (unsigned index = 0; index < 16; ++index) input[index] = static_cast<unsigned char>(index + 1);
    ppu_native_write_bytes(&owner.cpu, Runtime::base + 32, input, 16);
    ppu_native_read_bytes(&owner.cpu, Runtime::base + 32, output, 16);
    CHECK(std::memcmp(input, output, 16) == 0);
    CHECK(ppu_native_load_reserve32(&owner.cpu, Runtime::base + 4) == 0x44556677);
    CHECK(ppu_native_store_conditional32(&owner.cpu, Runtime::base + 4, 0x55667788));
    CHECK(ppu_native_load_reserve64(&owner.cpu, Runtime::base + 8) == UINT64_C(0x8899aabbccddeeff));
    CHECK(ppu_native_store_conditional64(&owner.cpu, Runtime::base + 8, UINT64_C(0x0102030405060708)));
    CHECK(ppu_native_atomic_add16(&owner.cpu, Runtime::base + 2, 3) == 0x2236);
    ppu_native_zero128(&owner.cpu, Runtime::base + 7);
    ppu_native_read_bytes(nullptr, 0, nullptr, 0); ppu_native_write_bytes(nullptr, 0, nullptr, 0);
    CHECK(recorder.report().published_count == 16 && recorder.report().retained_count == 3);
    CHECK(!recorder.report().batch_active && runtime.probe.saw_active);
    expect_event(recorder, 0, 13, PpuMemoryTraceOperation::StoreConditional, UINT64_C(0x0102030405060708), true);
    expect_event(recorder, 1, 14, PpuMemoryTraceOperation::Add16, 0x2236);
    expect_event(recorder, 2, 15, PpuMemoryTraceOperation::Zero128, 0);
    CHECK(runtime.get32(Runtime::base) == 0);
    runtime.release(owner);
}
void strict_precedence()
{
    ++cases; Runtime runtime; PpuMemoryExecution owner;
    PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Strict));
    CHECK(recorder.begin(1)); recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 1, 4, 7)); recorder.finish();
    owner.recorder = &recorder; CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
    runtime.put32(Runtime::base, 0xabcdef01); const ppu_context before = owner.cpu;
    const unsigned calls = runtime.probe.operations;
    expect_stop([&] { ppu_native_write32(&owner.cpu, Runtime::readonly, 2); }, PpuMemoryStop::PermissionDenied);
    expect_stop([&] { ppu_native_read32(&owner.cpu, UINT32_MAX - 1); }, PpuMemoryStop::InvalidRange);
    expect_stop([&] { ppu_native_write32(&owner.cpu, Runtime::base, 2); }, PpuMemoryStop::TraceCapacity);
    CHECK(runtime.probe.operations == calls && std::memcmp(&owner.cpu, &before, sizeof(before)) == 0);
    CHECK(runtime.get32(Runtime::base) == 0xabcdef01 && recorder.report().published_count == 1);
    runtime.release(owner);
}
void rejected_admission()
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        ++cases; Runtime runtime; PpuMemoryExecution owner;
        PpuMemoryTraceEvent events[2]{}, legacy_events[1]{}; PpuMemoryTraceRecorder recorder;
        PpuMemoryTraceJournal legacy{legacy_events, 1, 0};
        if (scenario != 0) CHECK(recorder.configure(events, 2, PpuMemoryTraceRetention::Suffix));
        if (scenario == 2) { CHECK(recorder.begin(2)); recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 1, 4, 9)); }
        owner.recorder = &recorder; if (scenario == 1) owner.trace = &legacy;
        unsigned char records[sizeof(runtime.owners)], reservation[sizeof(owner.reservation)];
        std::memcpy(records, runtime.owners, sizeof(records)); std::memcpy(reservation, &owner.reservation, sizeof(reservation));
        const auto before = recorder.report();
        const auto result = runtime.admit(owner);
        CHECK(result == PpuMemoryAdmission::InvalidBinding);
        CHECK(!owner.admitted && !owner.services && !owner.memory && !owner.boundary.stop);
        CHECK(std::memcmp(records, runtime.owners, sizeof(records)) == 0);
        CHECK(std::memcmp(reservation, &owner.reservation, sizeof(reservation)) == 0);
        CHECK(recorder.report().batch_active == before.batch_active && recorder.report().remaining_publications == before.remaining_publications);
        CHECK(recorder.report().published_count == before.published_count && legacy.count == 0);
        runtime.release(owner); recorder.finish();
    }
}
void rejected_entry()
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        ++cases; Runtime runtime; PpuMemoryExecution owner;
        PpuMemoryTraceEvent events[2]{}, legacy_events[1]{}; PpuMemoryTraceRecorder recorder, unconfigured;
        PpuMemoryTraceJournal legacy{legacy_events, 1, 0};
        CHECK(recorder.configure(events, 2, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
        CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
        if (scenario == 0) { CHECK(recorder.begin(2)); recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 1, 4, 10)); }
        if (scenario == 1) owner.trace = &legacy;
        if (scenario == 2) owner.recorder = &unconfigured;
        runtime.boundary.recorder = owner.recorder;
        const unsigned calls = runtime.probe.operations; const auto before = recorder.report();
        expect_stop([&] { ppu_native_write32(&owner.cpu, Runtime::base, 123); }, PpuMemoryStop::InvalidBinding);
        CHECK(runtime.probe.operations == calls && legacy.count == 0);
        CHECK(recorder.report().batch_active == before.batch_active && recorder.report().remaining_publications == before.remaining_publications);
        CHECK(recorder.report().published_count == before.published_count);
        runtime.release(owner); recorder.finish();
    }
}
void real_provider_failures()
{
    ++cases; Runtime runtime; PpuMemoryExecution owner;
    PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
    CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
    CHECK(ps3_atomic_decommit(&runtime.domain, Runtime::base + 4096, 4096) == PS3_GUEST_MEMORY_OK);
    unsigned char destination[16], before[16]; std::memset(destination, 0x5a, sizeof(destination)); std::memcpy(before, destination, sizeof(before));
    for (int operation = 0; operation < 3; ++operation) {
        runtime.probe.saw_active = false; runtime.boundary.closed_at_stop = false;
        expect_stop([&] {
            if (operation == 0) (void)ppu_native_read32(&owner.cpu, Runtime::base + 4096);
            if (operation == 1) ppu_native_read_bytes(&owner.cpu, Runtime::base + 4096 - 8, destination, 16);
            if (operation == 2) ppu_native_write32(&owner.cpu, Runtime::base + 4096, 77);
        }, PpuMemoryStop::ProviderFailure);
        CHECK(runtime.probe.saw_active && runtime.boundary.closed_at_stop && runtime.boundary.guard_returned);
        CHECK(!recorder.report().batch_active && recorder.report().published_count == 0);
        CHECK(std::memcmp(destination, before, 16) == 0);
        CHECK(recorder.begin(1)); recorder.finish();
    }
    ppu_native_write32(&owner.cpu, Runtime::base, 78);
    CHECK(!recorder.report().batch_active && recorder.report().published_count == 1);
    expect_event(recorder, 0, 0, PpuMemoryTraceOperation::Write, 78);
    runtime.release(owner);
}
void reservation_contracts()
{
    { ++cases; Runtime runtime; PpuMemoryExecution owner;
      PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
      CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
      CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready); runtime.put32(Runtime::base, 1);
      CHECK(ppu_native_load_reserve32(&owner.cpu, Runtime::base) == 1);
      runtime.put32(Runtime::base, 1); // Same-value real domain write still invalidates LL/SC.
      CHECK(!ppu_native_store_conditional32(&owner.cpu, Runtime::base, 2));
      expect_event(recorder, 0, 1, PpuMemoryTraceOperation::StoreConditional, 2, false);
      CHECK(!recorder.report().batch_active); runtime.release(owner); }
    for (bool cancel_before : {false, true}) {
        ++cases; Runtime runtime; PpuMemoryExecution owner;
        PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
        CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Strict)); owner.recorder = &recorder;
        CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready); runtime.put32(Runtime::base, 1);
        CHECK(ppu_native_load_reserve32(&owner.cpu, Runtime::base) == 1);
        runtime.boundary.cancel = cancel_before;
        expect_stop([&] { (void)ppu_native_store_conditional32(&owner.cpu, Runtime::base, 2); },
            cancel_before ? PpuMemoryStop::Cancelled : PpuMemoryStop::TraceCapacity);
        runtime.boundary.cancel = false;
        int stored = -1;
        CHECK(ps3_atomic_store_conditional32(&runtime.domain, owner.reservation, Runtime::base, 3, &stored) == PS3_GUEST_MEMORY_OK);
        CHECK(stored == (cancel_before ? 1 : 0));
        CHECK(recorder.report().published_count == 1 && !recorder.report().batch_active); runtime.release(owner);
    }
}
void no_new_post_write_cancellation()
{
    ++cases; Runtime runtime; PpuMemoryExecution owner;
    PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
    CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready); runtime.probe.cancel_after_write = true;
    ppu_native_write32(&owner.cpu, Runtime::base, 88);
    CHECK(runtime.boundary.cancel && recorder.report().published_count == 1 && !recorder.report().batch_active);
    expect_stop([&] { (void)ppu_native_read32(&owner.cpu, Runtime::base); }, PpuMemoryStop::Cancelled);
    CHECK(recorder.report().published_count == 1); runtime.release(owner);
}
void actual_derived_scalar()
{
    ++cases; Runtime runtime; PpuMemoryExecution owner;
    PpuMemoryTraceEvent events[1]{}; PpuMemoryTraceRecorder recorder;
    CHECK(recorder.configure(events, 1, PpuMemoryTraceRetention::Suffix)); owner.recorder = &recorder;
    CHECK(runtime.admit(owner) == PpuMemoryAdmission::Ready);
    owner.cpu.gpr[3] = 0x12345678; owner.cpu.gpr[4] = UINT64_C(0xabcdef0100010000);
    func_F0000001(&owner.cpu); owner.cpu.gpr[3] = 0; func_F0000000(&owner.cpu);
    CHECK(owner.cpu.gpr[3] == 0x12345678 && runtime.get32(Runtime::base) == 0x12345678);
    CHECK(recorder.report().published_count == 2 && !recorder.report().batch_active);
    expect_event(recorder, 0, 1, PpuMemoryTraceOperation::Read, 0x12345678); runtime.release(owner);
}
}
int main()
{
    null_and_legacy_controls(); all_operation_families(); strict_precedence(); rejected_admission(); rejected_entry();
    real_provider_failures(); reservation_contracts(); no_new_post_write_cancellation(); actual_derived_scalar();
    std::printf("GENERATED_RECORDER_%s cases=%d failures=%d real_domain=true actual_derived_scalar=true\n",
        failures ? "FAIL" : "PASS", cases, failures);
    return failures != 0;
}
