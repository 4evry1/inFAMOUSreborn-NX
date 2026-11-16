#include "../adapters/startup_execution.h"
#include "guest_space.h"
#include "../adapters/clock_horizon.h"
extern "C" int native_test_clock_horizon(void);
extern "C" int native_clock_conversion_tests(void);
int native_test_ppu_memory_trace(void);
#include "elf_image.h"
extern "C" {
#include "../adapters/lwmutex_wait_horizon.h"
}
#include <switch.h>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <vector>

namespace {
constexpr uint32_t STACK_TOP = 0xd0100000;
constexpr size_t MAIN_STACK_BYTES = 0x40000;
// Separate extra page above STACK_TOP is scratch for isolated import ABI cases;
// it is never counted as the main stack requested by the ELF process metadata.
constexpr size_t ABI_SCRATCH_BYTES = 4096;
constexpr uint32_t STACK_START = STACK_TOP - MAIN_STACK_BYTES;
constexpr size_t STACK_BYTES = MAIN_STACK_BYTES + ABI_SCRATCH_BYTES;
constexpr uint32_t ARGV = 0x00b00000;
constexpr uint32_t ENVP = ARGV + 0x10;
constexpr uint32_t TLS_BLOCK = 0x0e000000;
constexpr uint64_t TIMEOUT_NS = UINT64_C(5000000000);
struct Region { uint32_t address; std::vector<uint8_t> bytes; bool writable; };
struct Oracle {
    std::vector<Region> regions;
    std::vector<StartupAccess> accesses;
    bool valid = true;
    uint8_t *find(uint64_t address, size_t length) {
        for (Region &region : regions) {
            if (address >= region.address && address - region.address <= region.bytes.size() &&
                length <= region.bytes.size() - (address - region.address)) return region.bytes.data() + address - region.address;
        }
        valid = false;
        return nullptr;
    }
    uint64_t read(uint64_t address, unsigned width, bool trace = true) {
        uint8_t *bytes = find(address, width);
        uint64_t value = 0;
        if (bytes) {
            if (width == 4) { uint32_t raw; std::memcpy(&raw, bytes, 4); value = __builtin_bswap32(raw); }
            else if (width == 8) { uint64_t raw; std::memcpy(&raw, bytes, 8); value = __builtin_bswap64(raw); }
            else if (width == 2) { uint16_t raw; std::memcpy(&raw, bytes, 2); value = __builtin_bswap16(raw); }
            else value = *bytes;
        }
        if (trace) {
            StartupAccess expected{};
            expected.origin = PpuMemoryTraceOrigin::Generated;
            expected.operation = PpuMemoryTraceOperation::Read;
            expected.address = uint32_t(address);
            expected.length = width;
            expected.value = value;
            accesses.push_back(expected);
        }
        return value;
    }
    void write(uint64_t address, uint64_t value, unsigned width, bool trace = true, PpuMemoryTraceOrigin origin = PpuMemoryTraceOrigin::Generated) {
        // Generated scalar writes narrow before the ABI.
        // vm_write8/16/32 take a narrow value argument before the raw bridge.
        value &= UINT64_MAX >> ((8 - width) * 8);
        uint8_t *bytes = find(address, width);
        if (bytes) {
            if (width == 4) { uint32_t raw = __builtin_bswap32(uint32_t(value)); std::memcpy(bytes, &raw, 4); }
            else if (width == 8) { uint64_t raw = __builtin_bswap64(value); std::memcpy(bytes, &raw, 8); }
            else *bytes = uint8_t(value);
        }
        if (trace) {
            StartupAccess expected{};
            expected.origin = origin;
            expected.operation = PpuMemoryTraceOperation::Write;
            expected.address = uint32_t(address);
            expected.length = width;
            expected.value = value;
            accesses.push_back(expected);
        }
    }
};

struct ClockProvider {
    ps3_clock real;
    Mutex mutex{};
    unsigned active_scopes = 0;
    std::atomic<bool> *cancel_after_sample = nullptr;
};
struct ClockScope {
    ClockProvider *provider;
    explicit ClockScope(ClockProvider *value) : provider(value) {
        mutexLock(&provider->mutex);
        ++provider->active_scopes;
    }
    ~ClockScope() {
        --provider->active_scopes;
        mutexUnlock(&provider->mutex);
    }
};
ps3_clock_status sample_real_clock(void *context, uint64_t *output)
{
    auto *provider = static_cast<ClockProvider *>(context);
    ClockScope scope(provider);
    ps3_clock_status status = provider->real.now_microseconds(provider->real.context, output);
    if (status == PS3_CLOCK_OK && provider->cancel_after_sample)
        provider->cancel_after_sample->store(true, std::memory_order_release);
    return status;
}
struct ClockArithmeticFixture { uint64_t origin, ticks, frequency; };
ps3_clock_status sample_arithmetic_fixture(void *context, uint64_t *output)
{
    const auto *fixture = static_cast<const ClockArithmeticFixture *>(context);
    return ps3_clock_elapsed_microseconds(fixture->origin, fixture->ticks, fixture->frequency, output);
}
struct TlsProvider {
    const ps3_guest_memory *memory;
    Ps3TlsArena arena;
    ps3_lwmutex_registry *registry = nullptr;
    ps3_user_memory_service *user_memory = nullptr;
    std::atomic<bool> *cancel_after_allocate = nullptr;
    unsigned allocation_calls = 0, free_calls = 0, query_calls = 0;
    uint64_t last_timeout = 0;
    unsigned mutex_calls = 0;
    std::atomic<bool> *cancel_after_create = nullptr;
    Mutex mutex{};
    unsigned calls = 0;
    unsigned active_scopes = 0;
    std::atomic<unsigned> *concurrent_ready = nullptr;
    std::atomic<bool> *committed_ready = nullptr;
    std::atomic<bool> *allow_return = nullptr;
};
struct ProviderScope {
    TlsProvider *provider;
    explicit ProviderScope(TlsProvider *value) : provider(value) {
        mutexLock(&provider->mutex);
        ++provider->active_scopes;
    }
    ~ProviderScope() {
        --provider->active_scopes;
        mutexUnlock(&provider->mutex);
    }
};
Ps3TlsReport initialize_tls(void *context, const Ps3TlsTemplate *tls, Ps3TlsImage *output)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ProviderScope scope(provider);
    ++provider->calls;
    if (provider->concurrent_ready) {
        provider->concurrent_ready->fetch_add(1, std::memory_order_release);
        const uint64_t deadline = armGetSystemTick() + armNsToTicks(UINT64_C(1000000000));
        while (provider->concurrent_ready->load(std::memory_order_acquire) < 2) {
            if (armGetSystemTick() >= deadline) {
                return {PS3_TLS_MEMORY_FAILURE, PS3_TLS_MEMORY_FAILURE,
                        PS3_GUEST_MEMORY_PLATFORM_FAILURE, PS3_GUEST_MEMORY_OK};
            }
            svcSleepThread(1000);
        }
    }
    uint8_t scratch[4096];
    Ps3TlsReport report = ps3_tls_initialize(provider->memory, &provider->arena, tls, scratch, sizeof(scratch), output);
    if (report.status == PS3_TLS_OK && provider->committed_ready && provider->allow_return) {
        provider->committed_ready->store(true, std::memory_order_release);
        const uint64_t deadline = armGetSystemTick() + armNsToTicks(TIMEOUT_NS);
        while (!provider->allow_return->load(std::memory_order_acquire) && armGetSystemTick() < deadline) {
            svcSleepThread(1000);
        }
    }
    return report;
}
ElfReadStatus read_file(void *context, uint64_t offset, void *destination, size_t length)
{
    FILE *file = static_cast<FILE *>(context);
    if (offset > LONG_MAX || std::fseek(file, long(offset), SEEK_SET)) return ELF_READ_ERROR;
    return std::fread(destination, 1, length, file) == length ? ELF_READ_OK : ELF_READ_ERROR;
}
bool snapshot(const ps3_guest_memory *memory, Oracle &oracle, uint32_t address, size_t length, bool writable)
{
    Region region{address, std::vector<uint8_t>(length), writable};
    if (memory->read(memory->context, address, region.bytes.data(), length) != PS3_GUEST_MEMORY_OK) return false;
    oracle.regions.push_back(std::move(region));
    return true;
}
bool compare(const ps3_guest_memory *memory, const Oracle &oracle)
{
    uint8_t actual[4096];
    for (const Region &region : oracle.regions) {
        for (size_t offset = 0; offset < region.bytes.size();) {
            size_t count = region.bytes.size() - offset;
            if (count > sizeof(actual)) count = sizeof(actual);
            if (memory->read(memory->context, region.address + offset, actual, count) != PS3_GUEST_MEMORY_OK ||
                std::memcmp(actual, region.bytes.data() + offset, count)) return false;
            offset += count;
        }
    }
    return true;
}
bool start(StartupExecution &execution, Thread &worker)
{
    Result result = threadCreate(&worker, native_startup_worker, &execution, nullptr, 0x40000, 0x2c, -2);
    if (R_FAILED(result)) return false;
    result = threadStart(&worker);
    if (R_FAILED(result)) {
        if (R_FAILED(threadClose(&worker))) std::abort();
        return false;
    }
    return true;
}
void join(Thread &worker)
{
    Result result = svcWaitSynchronizationSingle(worker.handle, TIMEOUT_NS);
    if (R_FAILED(result)) {
        std::printf("STARTUP_FATAL worker_not_joined=true result=0x%x memory_not_reused=true process_exit_without_unwind=true\n", result);
        std::fflush(stdout);
        svcExitProcess();
    }
    if (R_FAILED(threadWaitForExit(&worker)) || R_FAILED(threadClose(&worker))) std::abort();
}
bool run(StartupExecution &execution)
{
    Thread worker;
    if (!start(execution, worker)) return false;
    join(worker);
    return native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK;
}

void configure(StartupControl &control, const Oracle &)
{
    control.deadline_tick = armGetSystemTick() + armNsToTicks(TIMEOUT_NS);
}
void prepare(StartupExecution &execution, const ppu_context &initial,
             const Ps3PpuExecutionServices &services, StartupControl &control,
             PpuMemoryBinding &binding, int32_t priority, bool has_priority = true)
{
    execution.ppu.cpu = initial;
    execution.control = &control;
    execution.guest_priority = priority;
    execution.has_guest_priority = has_priority;
    if (!native_startup_admit(execution, binding, services)) std::abort();
}
void set_cr(ppu_context &cpu, uint64_t left, uint64_t right, bool signed32)
{
    uint32_t value;
    if (signed32) value = int32_t(left) < int32_t(right) ? 8 : int32_t(left) > int32_t(right) ? 4 : 2;
    else value = left < right ? 8 : left > right ? 4 : 2;
    cpu.cr = (cpu.cr & ~0xfu) | value;
}
// Independent instruction-derived oracle; does not call the production bridge or guest functions.
void model_to_tls(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[2] = memory.read(0x771b0c, 4);
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8); cpu.gpr[1] -= 0x70;
    cpu.gpr[14] = 0; memory.write(cpu.gpr[1], 0, 8); cpu.lr = 0x10250;
    memory.write(cpu.gpr[1] - 0xa0, cpu.gpr[1], 8); cpu.gpr[1] -= 0xa0;
    cpu.gpr[0] = cpu.lr;
    for (unsigned reg : {26u, 28u, 30u, 31u, 27u, 29u}) {
        const uint32_t offset = 0x70 + (reg - 26) * 8;
        memory.write(cpu.gpr[1] + offset, cpu.gpr[reg], 8);
    }
    memory.write(cpu.gpr[1] + 0xb0, cpu.gpr[0], 8);
    cpu.gpr[31] = cpu.gpr[3]; cpu.gpr[28] = cpu.gpr[4]; cpu.gpr[26] = cpu.gpr[5];
    cpu.gpr[30] = cpu.gpr[6]; cpu.gpr[29] = cpu.gpr[12];
    cpu.gpr[11] = memory.read(cpu.gpr[2] - 0x7ffc, 4);
    cpu.gpr[4] = uint32_t(cpu.gpr[8]); cpu.gpr[6] = uint32_t(cpu.gpr[10]);
    cpu.gpr[3] = cpu.gpr[7]; cpu.gpr[5] = uint32_t(cpu.gpr[9]); cpu.gpr[27] = cpu.gpr[28];
    memory.write(cpu.gpr[11], cpu.gpr[29], 4); cpu.lr = 0x103a0;
}
void model_after_tls_to_mutex(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[2] = 0x78bf78;
    cpu.gpr[10] = cpu.gpr[28] + 4; cpu.gpr[8] = 0;
    for (unsigned bounded = 0; bounded < 4; ++bounded) {
        cpu.gpr[9] = uint32_t(cpu.gpr[10]); cpu.gpr[11] = uint32_t(cpu.gpr[28]);
        ++cpu.gpr[8]; cpu.gpr[10] += 8; set_cr(cpu, cpu.gpr[31], cpu.gpr[8], false);
        cpu.gpr[0] = memory.read(cpu.gpr[9], 4); cpu.gpr[28] += 4;
        memory.write(cpu.gpr[11], cpu.gpr[0], 4);
        if (cpu.cr & 8) break;
        if (bounded == 3) memory.valid = false;
    }
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x8000, 4);
    cpu.gpr[29] = cpu.gpr[26]; cpu.gpr[5] = cpu.gpr[26]; cpu.gpr[28] = cpu.gpr[31];
    cpu.gpr[10] = cpu.gpr[26] + 4; cpu.gpr[8] = 0;
    memory.write(cpu.gpr[9], cpu.gpr[31], 4); memory.write(cpu.gpr[9] + 4, cpu.gpr[27], 4);
    for (unsigned bounded = 0; bounded < 4; ++bounded) {
        cpu.gpr[9] = uint32_t(cpu.gpr[10]); cpu.gpr[11] = uint32_t(cpu.gpr[5]);
        ++cpu.gpr[8]; cpu.gpr[10] += 8; set_cr(cpu, cpu.gpr[30], cpu.gpr[8], false);
        cpu.gpr[0] = memory.read(cpu.gpr[9], 4); cpu.gpr[5] += 4;
        memory.write(cpu.gpr[11], cpu.gpr[0], 4);
        if (cpu.cr & 8) break;
        if (bounded == 3) memory.valid = false;
    }
    cpu.lr = 0x10420;
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x6734, 4); cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8); cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8); cpu.lr = 0x456434;
    memory.write(cpu.gpr[1] - 0xf0, cpu.gpr[1], 8); cpu.gpr[1] -= 0xf0;
    cpu.gpr[0] = cpu.lr; memory.write(cpu.gpr[1] + 0xe0, cpu.gpr[30], 8);
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x6750, 4);
    memory.write(cpu.gpr[1] + 0x100, cpu.gpr[0], 8); cpu.gpr[0] = cpu.gpr[3];
    memory.write(cpu.gpr[1] + 0xc8, cpu.gpr[27], 8); memory.write(cpu.gpr[1] + 0xd0, cpu.gpr[28], 8);
    cpu.gpr[3] = memory.read(cpu.gpr[30] + 0x20, 4);
    memory.write(cpu.gpr[1] + 0xd8, cpu.gpr[29], 8); set_cr(cpu, cpu.gpr[3], 0, true);
    memory.write(cpu.gpr[1] + 0xe8, cpu.gpr[31], 8);
    if (cpu.cr & 2) { cpu.gpr[3] = cpu.gpr[0]; memory.write(cpu.gpr[30] + 0x20, cpu.gpr[0], 4); }
    cpu.gpr[3] += 0x1b8; cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x6740, 4);
    cpu.gpr[3] = uint32_t(cpu.gpr[3]); cpu.lr = 0x4561dc;
}
void lock_domain(void *context) { mutexLock(static_cast<Mutex *>(context)); }
void unlock_domain(void *context) { mutexUnlock(static_cast<Mutex *>(context)); }
ps3_lwmutex_result create_mutex(void *context, uint32_t address, const Ps3PpuLwmutexAttributes *attributes)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ProviderScope scope(provider);
    ++provider->mutex_calls;
    ps3_lwmutex_result result = ps3_lwmutex_create(provider->registry, address,
        attributes->protocol, attributes->recursive);
    if (result.status == PS3_LWMUTEX_OK && provider->cancel_after_create)
        provider->cancel_after_create->store(true, std::memory_order_release);
    return result;
}
ps3_lwmutex_result lock_mutex(void *context, uint32_t address, ps3_lwmutex_thread thread,
                            uint64_t timeout, ps3_wait_cancel cancel)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->mutex_calls;
    provider->last_timeout = timeout;
    // No outer provider mutex while waiting: another guest can dispatch unlock.
    return ps3_lwmutex_lock(provider->registry, address, thread, timeout, cancel);
}
ps3_lwmutex_result try_mutex(void *context, uint32_t address, ps3_lwmutex_thread thread)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->mutex_calls;
    return ps3_lwmutex_try_lock(provider->registry, address, thread);
}
ps3_lwmutex_result unlock_mutex(void *context, uint32_t address, ps3_lwmutex_thread thread)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->mutex_calls;
    return ps3_lwmutex_unlock(provider->registry, address, thread);
}
ps3_lwmutex_result destroy_mutex(void *context, uint32_t address)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->mutex_calls;
    return ps3_lwmutex_destroy(provider->registry, address);
}
ps3_user_memory_service_result allocate_memory(void *context, uint64_t size, uint64_t alignment, uint64_t output)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ProviderScope scope(provider);
    ++provider->allocation_calls;
    ps3_user_memory_service_result result = ps3_user_memory_allocate_and_publish_be32(
        provider->user_memory, size, alignment, output);
    if (result.status == PS3_USER_MEMORY_SERVICE_OK && provider->cancel_after_allocate)
        provider->cancel_after_allocate->store(true, std::memory_order_release);
    return result;
}
ps3_user_memory_service_result free_memory(void *context, uint32_t address)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->free_calls;
    return ps3_user_memory_free_owned(provider->user_memory, address);
}
ps3_user_memory_service_result query_memory(void *context, uint64_t output)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->query_calls;
    return ps3_user_memory_query_and_publish_be32(provider->user_memory, output);
}

int test_memory_family(const ps3_guest_memory *memory, ps3_atomic_domain *domain,
                       ps3_user_memory *allocator, TlsProvider &provider,
                       const Ps3PpuExecutionServices &services, const ppu_context &initial,
                       Oracle &oracle, PpuMemoryBinding &binding, int32_t priority)
{
    int failures = 0;
#define VERIFY(condition) do { if (!(condition)) { std::printf("MEMORY_ABI_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    constexpr uint32_t output = ARGV + 0x300;
    auto call = [&](uint64_t number, uint64_t r3, uint64_t r4, uint64_t r5,
                    uint32_t cell, StartupStop stop = StartupStop::Returned) {
        StartupControl control;
        configure(control, oracle);
        control.entry = StartupEntry::MemorySyscall;
        StartupExecution owner{};
        prepare(owner, initial, services, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[3] = r3;
        owner.ppu.cpu.gpr[4] = r4;
        owner.ppu.cpu.gpr[5] = r5;
        owner.ppu.cpu.gpr[11] = number;
        owner.ppu.cpu.cr = 0x12345678;
        const ppu_context before = owner.ppu.cpu;
        const uint64_t toc_sentinel = UINT64_C(0x1122334455667788);
        VERIFY(memory->write(memory->context, uint32_t(owner.ppu.cpu.gpr[1]) + 0x28,
            &toc_sentinel, sizeof(toc_sentinel)) == PS3_GUEST_MEMORY_OK);
        VERIFY(run(owner));
        if (control.memory_result.retained_pin) {
            std::printf("STARTUP_MEMORY_FATAL retained_output_pin=true joined=true backing_retained=true service=%d pin_release=%d\n",
                control.memory_result.status, control.memory_result.pin_release_status);
            std::fflush(stdout);
            svcExitProcess();
        }
        VERIFY(control.stop == stop && provider.active_scopes == 0);
        ppu_context expected = before;
        if (stop == StartupStop::Returned) expected.gpr[3] = uint64_t(int64_t(int32_t(cell)));
        VERIFY(std::memcmp(&owner.ppu.cpu, &expected, sizeof(expected)) == 0);
        VERIFY(control.memory_returns == (stop == StartupStop::Returned ? 1 : 0));
        uint64_t actual_toc = 0;
        VERIFY(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28,
            &actual_toc, sizeof(actual_toc)) == PS3_GUEST_MEMORY_OK);
        VERIFY(actual_toc == toc_sentinel && control.journal.count == 0);
        return control.memory_result;
    };
    auto read_output = [&]() {
        uint32_t value = 0;
        VERIFY(memory->read(memory->context, output, &value, 4) == PS3_GUEST_MEMORY_OK);
        return __builtin_bswap32(value);
    };
    auto accounting = [&](uint64_t used) {
        ps3_user_memory_info info{};
        VERIFY(ps3_user_memory_query(allocator, &info) == PS3_USER_MEMORY_OK);
        VERIFY(info.quota_bytes == UINT64_C(213) * 1024 * 1024 && info.used_bytes == used);
        VERIFY(info.available_bytes == info.quota_bytes - used);
    };
    call(UINT64_C(0x100000160), output, 0, 0, 0, StartupStop::UnsupportedSyscall);
    call(352, output, 0, 0, 0);
    VERIFY(read_output() == 213 * 1024 * 1024);
    call(348, 0x100000, 0, UINT64_C(0xabc0000000000000) | output, 0);
    VERIFY(read_output() == 0x40000000);
    accounting(0x100000);
    unsigned char zeroes[64];
    std::memset(zeroes, 0xff, sizeof(zeroes));
    VERIFY(memory->read(memory->context, 0x40000000, zeroes, sizeof(zeroes)) == PS3_GUEST_MEMORY_OK);
    for (unsigned char value : zeroes) VERIFY(value == 0);
    call(352, output, 0, 0, 0);
    uint32_t available = 0;
    VERIFY(memory->read(memory->context, output + 4, &available, 4) == PS3_GUEST_MEMORY_OK);
    VERIFY(__builtin_bswap32(available) == 212 * 1024 * 1024);
    call(349, UINT64_C(0xabc0000040000000), 0, 0, 0);
    call(348, 0x10000, 0x200, output, 0);
    VERIFY(read_output() == 0x40000000);
    call(349, 0x40000000, 0, 0, 0);
    call(348, 0x100000, 0x400, output, 0);
    ps3_atomic_pin pin{};
    VERIFY(ps3_atomic_pin_acquire(domain, 0x40000000, 64, &pin) == PS3_GUEST_MEMORY_OK);
    auto retained = call(349, 0x40000000, 0, 0, 0, StartupStop::MemoryProviderFailure);
    VERIFY(retained.retained_allocation && retained.retained_address == 0x40000000);
    accounting(0x100000);
    VERIFY(ps3_atomic_pin_release(domain, &pin) == PS3_GUEST_MEMORY_OK);
    call(349, 0x40000000, 0, 0, 0);
    call(349, 0x40000000, 0, 0, 0x80010002);
    call(348, 0, 0x400, output, 0x80010010);
    call(348, UINT64_C(0x100000001), 0x400, output, 0x80010010);
    call(348, 0x100000, UINT64_C(0x100000400), output, 0x80010002);
    call(348, UINT64_C(0x100100000), 0x400, output, 0x80010004);
    const unsigned queries_before = provider.query_calls;
    call(352, 0x10000, 0, 0, 0x8001000d);
    call(352, 0xc0000000, 0, 0, 0x8001000d);
    call(352, 0xfffffffc, 0, 0, 0x8001000d);
    call(352, ARGV + 4092, 0, 0, 0x8001000d);
    VERIFY(provider.query_calls == queries_before);
    accounting(0);
    StartupControl cancelled;
    configure(cancelled, oracle);
    cancelled.entry = StartupEntry::MemorySyscall;
    StartupExecution owner{};
        prepare(owner, initial, services, cancelled, binding, priority);
    owner.ppu.cpu.gpr[3] = 0x100000;
    owner.ppu.cpu.gpr[4] = 0x400;
    owner.ppu.cpu.gpr[5] = output;
    owner.ppu.cpu.gpr[11] = 348;
    const ppu_context before = owner.ppu.cpu;
    provider.cancel_after_allocate = &cancelled.cancelled;
    VERIFY(run(owner));
    if (cancelled.memory_result.retained_pin || cancelled.memory_result.retained_allocation) {
        std::printf("STARTUP_MEMORY_FATAL cancellation_retained_resources=true joined=true backing_retained=true\n");
        std::fflush(stdout);
        svcExitProcess();
    }
    VERIFY(cancelled.stop == StartupStop::Cancelled && cancelled.memory_returns == 0);
    VERIFY(cancelled.memory_result.status == PS3_USER_MEMORY_SERVICE_OK && cancelled.memory_result.published);
    VERIFY(provider.active_scopes == 0 && std::memcmp(&owner.ppu.cpu, &before, sizeof(before)) == 0);
    VERIFY(read_output() == 0x40000000);
    accounting(0x100000);
    provider.cancel_after_allocate = nullptr;
    VERIFY(ps3_user_memory_free(allocator, 0x40000000).status == PS3_USER_MEMORY_OK);
    accounting(0);
    // Restore the complete scratch pages before the independent startup oracle.
    VERIFY(memory->write(memory->context, ARGV, oracle.find(ARGV, 4096), 4096) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory->write(memory->context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES), ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("MEMORY_ABI_%s full64_size_flags=true effective32_pointers=true syscall_no_toc_write=true cancel_committed_owned=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}

int test_clock_abi(const ps3_guest_memory *memory, const Ps3PpuExecutionServices &services,
                   const ppu_context &initial, Oracle &oracle,
                   ClockProvider &provider, PpuMemoryBinding &binding, int32_t priority)
{
    int failures = 0;
#define VERIFY(condition) do { if (!(condition)) { std::printf("CLOCK_ABI_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    auto call = [&](const ps3_clock *clock, StartupStop wanted, bool cancel, uint64_t exact_value) {
        Ps3PpuExecutionServices selected = services;
        selected.clock = clock;
        StartupControl control;
        configure(control, oracle);
        control.entry = StartupEntry::ClockStub;
        StartupExecution owner{};
        prepare(owner, initial, selected, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[3] = UINT64_MAX;
        owner.ppu.cpu.gpr[4] = UINT64_C(0xdeadbeefcafef00d);
        const ppu_context before = owner.ppu.cpu;
        const uint64_t sentinel = UINT64_C(0x1122334455667788);
        VERIFY(memory->write(memory->context, uint32_t(before.gpr[1]) + 0x28,
            &sentinel, sizeof(sentinel)) == PS3_GUEST_MEMORY_OK);
        provider.cancel_after_sample = cancel ? &control.cancelled : nullptr;
        VERIFY(run(owner));
        provider.cancel_after_sample = nullptr;
        VERIFY(control.stop == wanted && provider.active_scopes == 0);
        ppu_context expected = before;
        uint64_t stored = 0;
        VERIFY(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28, &stored, 8) == PS3_GUEST_MEMORY_OK);
        if (wanted == StartupStop::Returned) {
            expected.gpr[3] = control.clock_sample;
            VERIFY(control.clock_calls == 1 && control.clock_returns == 1 && control.clock_status == PS3_CLOCK_OK);
            VERIFY(__builtin_bswap64(stored) == before.gpr[2]);
            if (exact_value) VERIFY(control.clock_sample == exact_value);
        } else {
            VERIFY(control.clock_returns == 0 && stored == sentinel);
            if (cancel) VERIFY(control.clock_calls == 1 && control.clock_status == PS3_CLOCK_OK);
        }
        VERIFY(std::memcmp(&owner.ppu.cpu, &expected, sizeof(expected)) == 0);
    };
    call(services.clock, StartupStop::Returned, false, 0);
    // Deterministic conversion/ABI fixture only; startup always uses real ticks.
    ClockArithmeticFixture wide{0, UINT64_C(0xfedcba9876543210), 1000000};
    ps3_clock wide_port{&wide, sample_arithmetic_fixture};
    call(&wide_port, StartupStop::Returned, false, wide.ticks);
    ps3_horizon_clock invalid{};
    ps3_clock invalid_port = ps3_horizon_clock_port(&invalid);
    call(&invalid_port, StartupStop::ClockProviderFailure, false, 0);
    call(nullptr, StartupStop::MissingServices, false, 0);
    call(services.clock, StartupStop::Cancelled, true, 0);
    VERIFY(memory->write(memory->context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES),
        ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("CLOCK_ABI_%s no_arguments=true full_u64=true wide_fixture_only=true actual_startup_clock=Horizon cancel_after_real_sample=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
int test_binding_identity(const ps3_guest_memory *backing,
    const Ps3PpuExecutionServices &services, TlsProvider &provider,
    PpuMemoryBinding &binding, const ppu_context &initial, Oracle &oracle,
    int32_t priority)
{
    int failures = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("CONTEXT_BINDING_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    Ps3PpuExecutionServices selected = services;
    StartupControl control, rejected_control;
    configure(control, oracle);
    StartupExecution owner{};
    prepare(owner, initial, selected, control, binding, priority);
    const PpuMemoryStopPort original_boundary = owner.ppu.boundary;
    PpuMemoryTraceJournal *const original_trace = owner.ppu.trace;
    owner.control = &rejected_control;
    VERIFY(!native_startup_admit(owner, binding, selected));
    VERIFY(owner.ppu.trace == original_trace && !rejected_control.cpu);
    VERIFY(owner.ppu.boundary.context == original_boundary.context && owner.ppu.admitted);
    owner.control = &control;
    VERIFY(native_startup_release_after_join(owner) == PS3_GUEST_MEMORY_OK);
    selected.memory = backing;
    StartupExecution rejected{};
    rejected.control = &rejected_control;
    VERIFY(!native_startup_admit(rejected, binding, selected));
    VERIFY(!rejected.ppu.admitted && !rejected.ppu.trace && !rejected_control.cpu);
    // Same real configuration must admit before testing the observer compatibility guard.
    auto recorder_control_storage = std::make_unique<StartupControl>();
    StartupControl &recorder_control = *recorder_control_storage;
    configure(recorder_control, oracle);
    auto recorder_positive_storage = std::make_unique<StartupExecution>();
    StartupExecution &recorder_positive = *recorder_positive_storage;
    auto initialize_candidate = [&](StartupExecution &candidate, StartupControl &candidate_control) {
        candidate.ppu.cpu = initial;
        candidate.control = &candidate_control;
        candidate.guest_priority = priority;
        candidate.has_guest_priority = true;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
        candidate.dispatch_stop = {&candidate_control, owner.dispatch_stop.stop_requested};
#endif
    };
    initialize_candidate(recorder_positive, recorder_control);
    const bool positive_admitted = native_startup_admit(recorder_positive, binding, services);
    VERIFY(positive_admitted && recorder_positive.ppu.admitted);
    if (positive_admitted) VERIFY(native_startup_release_after_join(recorder_positive) == PS3_GUEST_MEMORY_OK);
    auto recorder_rejected_control_storage = std::make_unique<StartupControl>();
    StartupControl &recorder_rejected_control = *recorder_rejected_control_storage;
    configure(recorder_rejected_control, oracle);
    auto recorder_rejected_storage = std::make_unique<StartupExecution>();
    StartupExecution &recorder_rejected = *recorder_rejected_storage;
    initialize_candidate(recorder_rejected, recorder_rejected_control);
    PpuMemoryTraceEvent external_events[2]{};
    PpuMemoryTraceRecorder external_recorder;
    VERIFY(external_recorder.configure(external_events, 2, PpuMemoryTraceRetention::Suffix));
    VERIFY(external_recorder.begin(2));
    external_recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0x1234, 8, 0x1122));
    recorder_rejected.ppu.recorder = &external_recorder;
    const auto external_before = external_recorder.report();
    PpuMemoryTraceSample sample_before{}, sample_after{};
    VERIFY(external_recorder.read(0, sample_before));
    const unsigned tls_calls_before = provider.calls, free_calls_before = provider.free_calls;
    std::vector<unsigned char> slots_before(binding.domain->owner_count * sizeof(*binding.domain->owners));
    std::memcpy(slots_before.data(), binding.domain->owners, slots_before.size());
    const bool recorder_admitted = native_startup_admit(recorder_rejected, binding, services);
    VERIFY(!recorder_admitted);
    VERIFY(!recorder_rejected.ppu.admitted && !recorder_rejected.ppu.services && !recorder_rejected.ppu.memory);
    VERIFY(!recorder_rejected.ppu.trace && !recorder_rejected.ppu.boundary.stop && !recorder_rejected_control.cpu);
    VERIFY(!recorder_rejected.ppu.reservation.domain);
    VERIFY(std::memcmp(&recorder_rejected.ppu.cpu, &initial, sizeof(initial)) == 0);
    VERIFY(std::memcmp(slots_before.data(), binding.domain->owners, slots_before.size()) == 0);
    VERIFY(provider.calls == tls_calls_before && provider.free_calls == free_calls_before);
    VERIFY(external_recorder.report().batch_active == external_before.batch_active &&
           external_recorder.report().remaining_publications == external_before.remaining_publications &&
           external_recorder.report().published_count == external_before.published_count);
    VERIFY(external_recorder.read(0, sample_after) && sample_after.sequence == sample_before.sequence &&
           ppu_memory_event_equal(sample_after.event, sample_before.event));
    if (recorder_admitted) VERIFY(native_startup_release_after_join(recorder_rejected) == PS3_GUEST_MEMORY_OK);
    external_recorder.finish(); // Only the caller owns this external batch.
    for (StartupEntry entry : {StartupEntry::ClockStub, StartupEntry::MemorySyscall}) {
        selected = services;
        StartupControl invocation;
        configure(invocation, oracle);
        invocation.entry = entry;
        StartupExecution execution{};
        prepare(execution, initial, selected, invocation, binding, priority);
        execution.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        execution.ppu.cpu.gpr[3] = 0x40000000;
        execution.ppu.cpu.gpr[11] = 349;
        const ppu_context before = execution.ppu.cpu;
        uint64_t memory_before = 0, memory_after = 0;
        const ps3_guest_memory &memory = binding.coherent_memory;
        VERIFY(memory.read(memory.context, STACK_TOP + 0x128, &memory_before, 8) == PS3_GUEST_MEMORY_OK);
        const unsigned frees_before = provider.free_calls;
        selected.memory = backing; // Adversarial substitution AFTER admission.
        VERIFY(run(execution));
        VERIFY(invocation.stop == StartupStop::MissingServices);
        VERIFY(invocation.import_count == 0 && invocation.clock_calls == 0 && invocation.memory_returns == 0);
        VERIFY(invocation.journal.count == 0 && provider.free_calls == frees_before);
        VERIFY(!std::memcmp(&execution.ppu.cpu, &before, sizeof(before)));
        VERIFY(memory.read(memory.context, STACK_TOP + 0x128, &memory_after, 8) == PS3_GUEST_MEMORY_OK);
        VERIFY(memory_before == memory_after);
    }
    std::printf("CONTEXT_BINDING_%s drift_rejected_before_provider=true free_syscall_covered=true admission_failure_unchanged=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
int test_reservation_boundaries(const Ps3PpuExecutionServices &services,
    PpuMemoryBinding &binding, const ppu_context &initial, Oracle &oracle,
    int32_t priority)
{
    int failures = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("CONTEXT_RESERVATION_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    constexpr uint32_t reserved = ARGV + 0x300;
    auto exercise = [&](StartupEntry entry, uint32_t reservation_address,
                        uint64_t target, bool cancel, bool should_store,
                        StartupStop expected_stop, bool fill_trace = false) {
        StartupControl control;
        configure(control, oracle);
        control.entry = entry;
        StartupExecution owner{};
        prepare(owner, initial, services, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[4] = target;
        owner.ppu.cpu.gpr[5] = 17;
        const uint32_t observed = ppu_native_load_reserve32(&owner.ppu.cpu, reservation_address);
        const ppu_context before = owner.ppu.cpu;
        if (fill_trace) control.journal.count = control.journal.capacity;
        control.cancelled.store(cancel, std::memory_order_release);
        Thread worker;
        if (!start(owner, worker)) std::abort();
        join(worker);
        VERIFY(control.stop == expected_stop);
        if (expected_stop != StartupStop::Returned)
            VERIFY(!std::memcmp(&owner.ppu.cpu, &before, sizeof(before)));
        control.cancelled.store(false, std::memory_order_release);
        control.journal.count = 0;
        VERIFY(ppu_native_store_conditional32(&owner.ppu.cpu, reservation_address, observed) == should_store);
        VERIFY(native_startup_release_after_join(owner) == PS3_GUEST_MEMORY_OK);
    };
    // Actual clock HLE returns normally; its unrelated TOC write preserves LR.
    exercise(StartupEntry::ClockStub, reserved, 0, false, true, StartupStop::Returned);
    // The same actual HLE TOC write invalidates an overlapping reservation.
    exercise(StartupEntry::ClockStub, STACK_TOP + 0x128, 0, false, false, StartupStop::Returned);
    // Cancellation precedes the SC instruction, so it is not an attempted SC.
    exercise(StartupEntry::MemorySc32, reserved, reserved, true, true, StartupStop::Cancelled);
    // Rejected SC instructions consume their reservation before failing closed.
    exercise(StartupEntry::MemorySc32, reserved, 0x10000, false, false, StartupStop::InvalidMemory);
    exercise(StartupEntry::MemorySc32, reserved, 0xffffffff, false, false, StartupStop::InvalidMemory);
    exercise(StartupEntry::MemorySc32, reserved, reserved, false, false, StartupStop::TraceCapacity, true);
    const ps3_guest_memory &memory = binding.coherent_memory;
    VERIFY(memory.write(memory.context, ARGV, oracle.find(ARGV, 4096), 4096) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.write(memory.context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES),
        ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("CONTEXT_RESERVATION_%s nonoverlap_hle_preserves=true overlapping_hle_invalidates=true cancellation_not_attempt=true rejected_sc_consumes=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
void model_create_to_syscall(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    const uint32_t object = uint32_t(cpu.gpr[3]);
    const uint32_t words[] = {0xffffffff, 0, 0x12, 0, queue, 0};
    for (unsigned index = 0; index < 6; ++index) memory.write(object + index * 4, words[index], 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[31] = cpu.gpr[3];
    cpu.gpr[10] = memory.read(cpu.gpr[2] - 0x674c, 4);
    cpu.gpr[9] = memory.read(cpu.gpr[10] + 4, 4);
    memory.write(cpu.gpr[10], cpu.gpr[9], 4);
    cpu.gpr[3] = cpu.gpr[1] + 0x74;
    cpu.gpr[11] = 0x160;
}

}



namespace {
void model_publish_mutex(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    const uint32_t address = uint32_t(cpu.gpr[3]);
    const uint32_t attributes = uint32_t(cpu.gpr[4]);
    const uint32_t combined = uint32_t(memory.read(attributes, 4, false) | memory.read(attributes + 4, 4, false));
    const uint32_t words[] = {0xffffffff, 0, combined, 0, queue, 0};
    for (unsigned index = 0; index < 6; ++index) memory.write(address + index * 4, words[index], 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
}
void model_record(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    cpu.gpr[11] = cpu.gpr[3];
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[3] += 0x10;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x6754, 4);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[9] = 0;
    cpu.gpr[0] = 0;
    cpu.gpr[3] = uint32_t(cpu.gpr[3]);
    memory.write(cpu.gpr[11], cpu.gpr[0], 4);
    memory.write(cpu.gpr[11] + 8, cpu.gpr[9], 8);
    cpu.lr = 0x452dd0;
    model_publish_mutex(cpu, memory, queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
}
void model_info_and_second_mutex(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    // 352 output policy is the configured quota, never a sampled host capacity.
    memory.write(cpu.gpr[3], 213 * 1024 * 1024, 4, false);
    memory.write(cpu.gpr[3] + 4, 213 * 1024 * 1024, 4, false);
    cpu.gpr[3] = 0;
    cpu.gpr[29] = memory.read(cpu.gpr[2] - 0x673c, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x78, 4);
    cpu.gpr[0] = (uint32_t(cpu.gpr[0]) >> 20) & 0xfff;
    cpu.gpr[29] = memory.read(cpu.gpr[29], 4);
    set_cr(cpu, uint32_t(cpu.gpr[0]), uint32_t(cpu.gpr[29]), false);
    if (cpu.cr & 8) { memory.valid = false; return; }
    cpu.gpr[0] = uint32_t(cpu.gpr[29]) << 20;
    uint64_t saved[5];
    for (unsigned index = 0; index < 5; ++index)
        saved[index] = memory.read(cpu.gpr[1] + 0xc8 + index * 8, 8, false);
    cpu.gpr[27] = saved[0];
    cpu.gpr[28] = saved[1];
    memory.write(cpu.gpr[30] + 0x24, cpu.gpr[0], 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x100, 8);
    cpu.gpr[29] = saved[2];
    cpu.gpr[30] = saved[3];
    cpu.lr = cpu.gpr[0];
    cpu.gpr[31] = saved[4];
    cpu.gpr[1] += 0xf0;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7ff8, 4);
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7ff4, 4);
    cpu.lr = 0x10438;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x6754, 4);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x6758, 4);
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x452e10;
    model_publish_mutex(cpu, memory, queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
}
}


namespace {
void model_stream_records(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    cpu.lr = 0x10440;
    const uint64_t saved[] = {cpu.gpr[28], cpu.gpr[29], cpu.gpr[30], cpu.gpr[31]};
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x70, cpu.gpr[28], 8);
    cpu.gpr[28] = memory.read(cpu.gpr[2] - 0x6804, 4);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[0] = cpu.gpr[28] + 0xd0;
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    cpu.gpr[31] = cpu.gpr[0];
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[30], 8);
    cpu.gpr[29] = cpu.gpr[0];
    cpu.gpr[30] = cpu.gpr[31] + 0x78;
    for (unsigned index = 0; index < 3; ++index) {
        cpu.gpr[3] = uint32_t(cpu.gpr[31]);
        cpu.gpr[31] += 0x28;
        cpu.lr = 0x44f03c;
        model_record(cpu, memory, queue++);
        set_cr(cpu, cpu.gpr[30], cpu.gpr[31], true);
    }
    cpu.gpr[6] = memory.read(cpu.gpr[2] - 0x6800, 4);
    cpu.gpr[31] = cpu.gpr[28] + 8;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x67f8, 4);
    cpu.gpr[5] = memory.read(cpu.gpr[2] - 0x67fc, 4);
    cpu.gpr[30] = cpu.gpr[31] + 0xc8;
    cpu.gpr[0] = memory.read(cpu.gpr[6] + 4, 2);
    cpu.gpr[11] = memory.read(cpu.gpr[4] + 4, 2);
    cpu.gpr[0] = uint64_t(int64_t(int16_t(cpu.gpr[0])));
    cpu.gpr[9] = memory.read(cpu.gpr[5] + 4, 2);
    cpu.gpr[11] = uint64_t(int64_t(int16_t(cpu.gpr[11])));
    cpu.gpr[9] = uint64_t(int64_t(int16_t(cpu.gpr[9])));
    cpu.gpr[10] = uint32_t(cpu.gpr[0]) << 5;
    cpu.gpr[0] = uint32_t(cpu.gpr[0]) << 3;
    cpu.gpr[8] = uint32_t(cpu.gpr[9]) << 5;
    cpu.gpr[7] = uint32_t(cpu.gpr[11]) << 5;
    cpu.gpr[9] = uint32_t(cpu.gpr[9]) << 3;
    cpu.gpr[11] = uint32_t(cpu.gpr[11]) << 3;
    cpu.gpr[0] += cpu.gpr[10];
    cpu.gpr[9] += cpu.gpr[8];
    cpu.gpr[0] += cpu.gpr[29];
    cpu.gpr[11] += cpu.gpr[7];
    cpu.gpr[9] += cpu.gpr[29];
    cpu.gpr[11] += cpu.gpr[29];
    memory.write(cpu.gpr[6] + 0x44, cpu.gpr[0], 4);
    cpu.gpr[0] = 0x100;
    memory.write(cpu.gpr[5] + 0x44, cpu.gpr[9], 4);
    memory.write(cpu.gpr[4] + 0x44, cpu.gpr[11], 4);
    memory.write(cpu.gpr[28] + 0x148, cpu.gpr[0], 4);
    for (unsigned index = 0; index < 5; ++index) {
        cpu.gpr[3] = uint32_t(cpu.gpr[31]);
        cpu.gpr[31] += 0x28;
        cpu.lr = 0x44f0c4;
        model_record(cpu, memory, queue++);
        set_cr(cpu, cpu.gpr[30], cpu.gpr[31], true);
    }
    cpu.gpr[0] = 5;
    cpu.gpr[29] = saved[1];
    cpu.gpr[30] = saved[2];
    memory.write(cpu.gpr[28], cpu.gpr[0], 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0xa0, 8);
    cpu.gpr[28] = saved[0];
    cpu.gpr[31] = saved[3];
    cpu.lr = cpu.gpr[0];
    cpu.gpr[1] += 0x90;
}
void model_descriptor_table(ppu_context &cpu, Oracle &memory)
{
    cpu.lr = 0x10448;
    cpu.gpr[8] = memory.read(cpu.gpr[2] - 0x675c, 4);
    cpu.gpr[9] = 0xfd;
    cpu.gpr[0] = 0;
    cpu.ctr = uint32_t(cpu.gpr[9]);
    cpu.gpr[11] = 2;
    cpu.gpr[9] = 1;
    cpu.gpr[10] = 0;
    cpu.gpr[5] = UINT64_MAX;
    cpu.gpr[6] = cpu.gpr[8] + 0x400;
    cpu.gpr[7] = 0;
    memory.write(cpu.gpr[8] + 4, cpu.gpr[9], 4);
    memory.write(cpu.gpr[8] + 8, cpu.gpr[11], 4);
    memory.write(cpu.gpr[8] + 0x408, cpu.gpr[0], 4);
    memory.write(cpu.gpr[8], cpu.gpr[0], 4);
    memory.write(cpu.gpr[8] + 0x400, cpu.gpr[0], 4);
    memory.write(cpu.gpr[8] + 0x404, cpu.gpr[0], 4);
    for (unsigned index = 0; index < 253; ++index) {
        cpu.gpr[9] = uint32_t(cpu.gpr[10] + cpu.gpr[8]);
        cpu.gpr[11] = uint32_t(cpu.gpr[10] + cpu.gpr[6]);
        cpu.gpr[10] += 4;
        memory.write(cpu.gpr[9] + 0xc, cpu.gpr[5], 4);
        memory.write(cpu.gpr[11] + 0xc, cpu.gpr[7], 4);
        cpu.ctr = uint32_t(cpu.ctr - 1);
    }
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7fec, 4);
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7ff0, 4);
    cpu.lr = 0x10460;
}
}


namespace {
void model_clock_to_indirect(ppu_context &cpu, Oracle &memory, uint64_t sample, uint32_t next_queue)
{
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = sample;
    cpu.gpr[2] = 0x78bf78;
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x7fe8, 4);
    memory.write(cpu.gpr[9], cpu.gpr[3], 8);
    cpu.lr = 0x10470;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x10210;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[2] - 0x7fc0, 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7fc4, 4);
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7fbc, 4);
    cpu.lr = 0x105f4;
    // Actual original462F20 and unit003's standalone462E60 definition.
    cpu.gpr[5] = 0;
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[6] = 0;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x462f38;
    const uint32_t high_cr = int64_t(cpu.gpr[3]) < 0 ? 8 : int64_t(cpu.gpr[3]) > 0 ? 4 : 2;
    cpu.cr = (cpu.cr & 0x0fffffffu) | (high_cr << 28);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    memory.write(cpu.gpr[1] + 0x70, cpu.gpr[28], 8);
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[31] = cpu.gpr[4];
    if (high_cr == 2) { memory.valid = false; return; }
    cpu.gpr[0] = memory.read(cpu.gpr[3], 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    cpu.gpr[0] = UINT64_MAX;
    memory.write(cpu.gpr[4] + 0xc, cpu.gpr[3], 4);
    memory.write(cpu.gpr[4], cpu.gpr[0], 4);
    cpu.gpr[0] = 0x1fe00000;
    memory.write(cpu.gpr[4] + 4, cpu.gpr[5], 4);
    memory.write(cpu.gpr[4] + 0x10, cpu.gpr[0], 4);
    memory.write(cpu.gpr[4] + 8, cpu.gpr[6], 4);
    cpu.lr = 0x462ed0;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x65d8, 4);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x65dc, 4);
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x462368;
    // Actual standalone463EB0, stopping before its real indirect call.
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[3], 4);
    cpu.gpr[29] = cpu.gpr[4];
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    set_cr(cpu, cpu.gpr[0], 0, true);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[30], 8);
    cpu.gpr[31] = cpu.gpr[3];
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x65c0, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[30] + 0x70, 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.lr = 0x463f8c;
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x65c0, 4);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[0] = 2;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x65c4, 4);
    cpu.gpr[3] = cpu.gpr[9] + 0x58;
    memory.write(cpu.gpr[9] + 0x70, cpu.gpr[0], 4);
    cpu.lr = 0x463e8c;
    model_publish_mutex(cpu, memory, next_queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[0] = cpu.gpr[30] + 0x58;
    cpu.gpr[30] = uint32_t(cpu.gpr[0]);
    cpu.gpr[4] = 0;
    cpu.gpr[3] = cpu.gpr[30];
    cpu.lr = 0x463fa4;
    memory.write(cpu.gpr[3], cpu.thread_id, 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[31], 4);
    cpu.gpr[4] = uint32_t(cpu.gpr[29]);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[4], 4);
    cpu.gpr[2] = memory.read(cpu.gpr[4] + 4, 4);
    cpu.ctr = uint32_t(cpu.gpr[0]);
}
bool abandon_observed_lock_after_join(const ps3_guest_memory *memory, const Ps3PpuExecutionServices &services,
    ps3_lwmutex_waiter *waiters, size_t waiter_count, const StartupControl &control,
    const StartupExecution &execution, bool joined)
{
    if (!joined || control.observed_lock_acquisitions != 1 || control.observed_lock_address != 0x9b64c8 ||
        control.observed_lock_owner != execution.ppu.cpu.thread_id) return false;
    for (size_t index = 0; index < waiter_count; ++index) if (waiters[index].state) return false;
    unsigned char bytes[24];
    if (memory->read(memory->context, control.observed_lock_address, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK) return false;
    uint32_t owner = 0, waiting = 0;
    for (unsigned index = 0; index < 4; ++index) {
        owner = (owner << 8) | bytes[index];
        waiting = (waiting << 8) | bytes[index + 4];
    }
    if (owner != control.observed_lock_owner || waiting) return false;
    ps3_lwmutex_thread identity{control.observed_lock_owner, execution.guest_priority};
    const ps3_lwmutex_result result = services.unlock_mutex(services.context, control.observed_lock_address, identity);
    std::printf("DIAGNOSTIC_ABANDON_LOCK joined=true waiters=0 address=0x%x owner=%u status=%d guest_progress=false\n",
        control.observed_lock_address, owner, result.status);
    return result.status == PS3_LWMUTEX_OK;
}
}

extern "C" int native_test_startup_context(const ps3_guest_memory *backing, const char *path)
{
    int failures = native_clock_conversion_tests() != 0;
    failures += native_test_ppu_memory_trace() != 0;
    failures += native_test_clock_horizon() != 0;
#define CHECK(value) do { if (!(value)) { std::printf("STARTUP_MUTEX_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    FILE *file = std::fopen(path, "rb");
    if (!file) return 1;
    if (std::fseek(file, 0, SEEK_END)) { std::fclose(file); return 1; }
    long size = std::ftell(file);
    if (size <= 0) { std::fclose(file); return 1; }
    ElfImageReader reader{read_file, file, uint64_t(size)};
    ElfImagePlan plan{};
    ElfImageSegment segments[ELF_IMAGE_MAX_SEGMENTS];
    const auto planned = elf_image_plan(&reader, segments, ELF_IMAGE_MAX_SEGMENTS, &plan);
    std::fclose(file);
    if (planned != ELF_IMAGE_OK || plan.segment_count != 2 || !plan.has_tls ||
        !plan.has_process_parameters || plan.process_parameters.primary_priority != 250 ||
        plan.process_parameters.primary_stack_size != 0x40000 || plan.entry_opd != 0x771b08 ||
        plan.tls.guest_address != 0x785e7c || plan.tls.file_size != 4 || plan.tls.memory_size != 0x200) return 1;
    Mutex domain_mutex{};
    ps3_atomic_domain domain{};
    ps3_atomic_owner_record owners[8]{};
    ps3_atomic_pin_record pins[64]{};
    const ps3_instance_guard guard{&domain_mutex, lock_domain, unlock_domain};
    CHECK(ps3_atomic_domain_init(&domain, *backing, guard, owners, 8, pins, 64) == PS3_GUEST_MEMORY_OK);
    ps3_guest_memory coherent = ps3_atomic_domain_memory(&domain);
    const ps3_guest_memory *memory = &coherent;
    ps3_horizon_lwmutex_wait wait{};
    CHECK(ps3_horizon_lwmutex_wait_init(&wait, 2000000) == PS3_WAIT_READY);
    Mutex allocator_mutex{};
    ps3_user_memory allocator{};
    ps3_user_memory_slot allocation_slots[64]{};
    const ps3_user_memory_config allocation_config{
        UINT64_C(0x40000000), UINT64_C(0x50000000), UINT64_C(213) * 1024 * 1024,
        coherent, {&allocator_mutex, lock_domain, unlock_domain}};
    CHECK(ps3_user_memory_init(&allocator, &allocation_config, allocation_slots, 64) == PS3_USER_MEMORY_OK);
    ps3_user_memory_service memory_service{&allocator, &domain};
    ps3_lwmutex_registry registry{};
    ps3_lwmutex_object objects[32]{};
    ps3_lwmutex_waiter waiters[32]{};
    CHECK(ps3_lwmutex_init(&registry, &domain, ps3_horizon_lwmutex_wait_guard(&wait),
        ps3_horizon_lwmutex_wait_port(&wait), objects, 32, waiters, 32).status == PS3_LWMUTEX_OK);
    if (failures) return 1;
    Oracle oracle;
    for (size_t index = 0; index < plan.segment_count; ++index) {
        if (segments[index].memory_size > 16 * 1024 * 1024 ||
            uint64_t(segments[index].guest_address) + segments[index].memory_size >= ARGV) return 1;
        if (!snapshot(memory, oracle, segments[index].guest_address, segments[index].memory_size,
                      (segments[index].flags & 2) != 0)) return 1;
    }
    CHECK(memory->commit(memory->context, STACK_START, STACK_BYTES) == PS3_GUEST_MEMORY_OK);
    CHECK(memory->commit(memory->context, ARGV, 4096) == PS3_GUEST_MEMORY_OK);
    std::vector<uint8_t> stack(STACK_BYTES, 0xa5), argv(4096, 0);
    CHECK(memory->write(memory->context, STACK_START, stack.data(), stack.size()) == PS3_GUEST_MEMORY_OK);
    const char boot_path[] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    uint64_t pointer = __builtin_bswap64(uint64_t(ARGV + 0x20));
    std::memcpy(argv.data(), &pointer, 8);
    std::memcpy(argv.data() + 0x20, boot_path, sizeof(boot_path));
    CHECK(memory->write(memory->context, ARGV, argv.data(), argv.size()) == PS3_GUEST_MEMORY_OK);
    CHECK(snapshot(memory, oracle, STACK_START, STACK_BYTES, true));
    CHECK(snapshot(memory, oracle, ARGV, 4096, true));
    const uint32_t code = uint32_t(oracle.read(plan.entry_opd, 4, false));
    const uint32_t toc = uint32_t(oracle.read(plan.entry_opd + 4, 4, false));
    CHECK(code == 0x10230 && toc == 0x78bf78 && oracle.valid);
    if (failures) return 1;
    std::vector<PpuMemoryRamRange> permissions;
    for (const Region &region : oracle.regions)
        permissions.push_back({region.address, region.bytes.size(), region.writable});
    permissions.push_back({TLS_BLOCK, 0x10000, true});
    permissions.push_back({0x40000000, 0x10000000, true});
    PpuMemoryBinding binding(domain, permissions.data(), permissions.size());
    TlsProvider provider{memory, {TLS_BLOCK, TLS_BLOCK + 0x10000}};
    provider.registry = &registry;
    provider.user_memory = &memory_service;
    ps3_horizon_clock horizon_clock{};
    CHECK(ps3_horizon_clock_init(&horizon_clock) == PS3_CLOCK_OK);
    ClockProvider clock_provider{ps3_horizon_clock_port(&horizon_clock)};
    ps3_clock shared_clock{&clock_provider, sample_real_clock};
    Ps3PpuExecutionServices services{&provider, initialize_tls, memory, create_mutex,
        lock_mutex, try_mutex, unlock_mutex, destroy_mutex, allocate_memory, free_memory, query_memory, &shared_clock};
    StartupControl control;
    configure(control, oracle);
    StartupExecution execution{};
    execution.control = &control;
    CHECK(native_startup_admit(execution, binding, services));
    execution.guest_priority = plan.process_parameters.primary_priority;
    execution.has_guest_priority = true;
    execution.ppu.cpu.gpr[1] = STACK_TOP;
    execution.ppu.cpu.gpr[2] = toc;
    execution.ppu.cpu.gpr[3] = 1;
    execution.ppu.cpu.gpr[4] = ARGV;
    execution.ppu.cpu.gpr[5] = ENVP;
    execution.ppu.cpu.gpr[7] = 1;
    execution.ppu.cpu.gpr[8] = plan.tls.guest_address;
    execution.ppu.cpu.gpr[9] = plan.tls.file_size;
    execution.ppu.cpu.gpr[10] = plan.tls.memory_size;
    execution.ppu.cpu.gpr[12] = plan.process_parameters.malloc_page_size;
    execution.ppu.cpu.gpr[13] = 0x10f07000;
    execution.ppu.cpu.thread_id = 1;
    execution.ppu.cpu.cia = code;
    const ppu_context initial = execution.ppu.cpu;
    failures += test_memory_family(memory, &domain, &allocator, provider, services,
        initial, oracle, binding, execution.guest_priority);
    failures += test_clock_abi(memory, services, initial, oracle,
        clock_provider, binding, execution.guest_priority);
    failures += test_binding_identity(backing, services, provider, binding, initial, oracle, execution.guest_priority);
    failures += test_reservation_boundaries(services, binding, initial, oracle, execution.guest_priority);
    ppu_context expected = initial;
    model_to_tls(expected, oracle);
    const ppu_context expected_before_tls = expected;
    oracle.write(expected.gpr[1] + 0x28, expected.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    expected.gpr[13] = TLS_BLOCK + 0x7000;
    expected.gpr[3] = 0;
    const ppu_context expected_after_tls = expected;
    Region tls_region{TLS_BLOCK, std::vector<uint8_t>(0x9000, 0), true};
    uint8_t *tls_source = oracle.find(plan.tls.guest_address, 4);
    if (tls_source) std::memcpy(tls_region.bytes.data(), tls_source, 4);
    oracle.regions.push_back(std::move(tls_region));
    model_after_tls_to_mutex(expected, oracle);
    CHECK(expected.gpr[3] == 0x9b6380 && expected.gpr[4] == 0x59f8f8);
    CHECK(oracle.read(0x59f8f8, 4, false) == 2 && oracle.read(0x59f8fc, 4, false) == 0x10);
    const uint32_t first_queue = registry.next_queue_id;
    model_create_to_syscall(expected, oracle, first_queue);
    model_info_and_second_mutex(expected, oracle, first_queue + 1);
    model_stream_records(expected, oracle, first_queue + 2);
    model_descriptor_table(expected, oracle);
    const bool startup_joined = run(execution);
    CHECK(startup_joined);
    if (control.memory_result.retained_pin || control.memory_result.retained_allocation) {
        std::printf("STARTUP_MEMORY_FATAL retained_output_pin=%d retained_allocation=%d address=0x%x joined=true backing_retained=true\n",
            control.memory_result.retained_pin, control.memory_result.retained_allocation,
            control.memory_result.retained_address);
        std::fflush(stdout);
        svcExitProcess();
    }
#ifdef PS3_NATIVE_MEMORY_SCOUT
    std::printf("STARTUP_MEMORY_SCOUT stop=%d imports=%zu accesses=%zu syscall=%u memory_returns=%zu\n",
        int(control.stop), control.import_count, control.journal.count, control.syscall_number, control.memory_returns);
    std::printf("SCOUT_CLOCK calls=%zu returns=%zu status=%d sample=%llu\n", control.clock_calls, control.clock_returns, control.clock_status, (unsigned long long)control.clock_sample);
    for (size_t index = 0; index < control.import_count; ++index)
        std::printf("SCOUT_IMPORT index=%zu nid=0x%x\n", index, control.imports[index]);
    for (size_t index = 0; index < control.journal.count; ++index) {
        const StartupAccess &access = control.accesses[index];
        std::printf("SCOUT_ACCESS index=%zu address=%llx bytes=%zu write=%d value=%llx\n", index,
            (unsigned long long)access.address, access.length, (access.operation == PpuMemoryTraceOperation::Write), (unsigned long long)access.value);
    }
    for (unsigned reg = 0; reg < 32; ++reg)
        std::printf("SCOUT_REGISTER r%u value=%llx\n", reg, (unsigned long long)execution.ppu.cpu.gpr[reg]);
    std::printf("SCOUT_CPU cr=%x lr=%llx cia=%llx\n", execution.ppu.cpu.cr,
        (unsigned long long)execution.ppu.cpu.lr, (unsigned long long)execution.ppu.cpu.cia);
    const auto scout_cleanup = ps3_lwmutex_cleanup(&registry);
    if (scout_cleanup.status != PS3_LWMUTEX_OK || ps3_user_memory_cleanup(&allocator).status != PS3_USER_MEMORY_OK) {
        std::fflush(stdout);
        svcExitProcess();
    }
    std::printf("STARTUP_MEMORY_SCOUT_COMPLETE acceptance=false failures=%d\n", failures);
    return failures != 0;
#endif
    CHECK(control.clock_calls == 1 && control.clock_returns == 1 && control.clock_status == PS3_CLOCK_OK);
    model_clock_to_indirect(expected, oracle, control.clock_sample, first_queue + 10);
    CHECK(control.stop == StartupStop::UnsupportedExternal && control.external_id == 19);
    CHECK(control.syscall_number == 352 && control.memory_returns == 1);
    CHECK(control.import_count == 14 && control.imports[0] == 0x744680a2 && control.imports[11] == 0x8461e528 && control.imports[13] == 0x1573dc3f);
    CHECK(control.tls_returns == 1 && control.mutex_returns == 12 && provider.calls == 1 && provider.mutex_calls == 12);
    CHECK(provider.active_scopes == 0 && registry.next_queue_id == first_queue + 11);
    CHECK(std::memcmp(&control.before_tls, &expected_before_tls, sizeof(expected)) == 0);
    CHECK(std::memcmp(&control.after_tls, &expected_after_tls, sizeof(expected)) == 0);
    if (std::memcmp(&execution.ppu.cpu, &expected, sizeof(expected))) {
        for (unsigned reg = 0; reg < 32; ++reg) if (execution.ppu.cpu.gpr[reg] != expected.gpr[reg])
            std::printf("STARTUP_MUTEX_REGISTER r%u actual=%llx expected=%llx\n", reg,
                (unsigned long long)execution.ppu.cpu.gpr[reg], (unsigned long long)expected.gpr[reg]);
        CHECK(false);
    }
    CHECK(oracle.valid && control.journal.count == oracle.accesses.size());
    size_t actual_generated = 0, actual_hle = 0, expected_generated = 0, expected_hle = 0;
    for (const auto &event : oracle.accesses) {
        if (event.origin == PpuMemoryTraceOrigin::Generated) ++expected_generated;
        else ++expected_hle;
    }
    for (size_t index = 0; index < control.journal.count; ++index) {
        if (control.accesses[index].origin == PpuMemoryTraceOrigin::Generated) ++actual_generated;
        else ++actual_hle;
    }
    // Independently counted from the reviewed instruction model: generated
    // accesses plus TLS, eleven create, clock and lock HLE TOC publications.
    constexpr size_t EXPECTED_GENERATED_EVENTS = 678;
    constexpr size_t EXPECTED_HLE_EVENTS = 1 + 11 + 1 + 1;
    CHECK(expected_generated == EXPECTED_GENERATED_EVENTS && expected_hle == EXPECTED_HLE_EVENTS);
    CHECK(actual_generated == EXPECTED_GENERATED_EVENTS && actual_hle == EXPECTED_HLE_EVENTS);
    for (size_t index = 0; index < control.journal.count && index < oracle.accesses.size(); ++index) {
        const StartupAccess &actual = control.accesses[index], &wanted = oracle.accesses[index];
        CHECK(ppu_memory_event_equal(actual, wanted));
        std::printf("STARTUP_CONTEXT_ACCESS index=%zu origin=%s address=%llx bytes=%zu write=%d value=%llx\n", index,
            actual.origin == PpuMemoryTraceOrigin::Generated ? "generated" : "hle",
            (unsigned long long)actual.address, actual.length, (actual.operation == PpuMemoryTraceOperation::Write), (unsigned long long)actual.value);
    }
    CHECK(compare(memory, oracle));
    CHECK(ppu_memory_trace_equal(control.accesses, control.journal.count,
        oracle.accesses.data(), oracle.accesses.size()));
    std::printf("STARTUP_CONTEXT_JOURNAL generated=%zu hle=%zu expected_generated=%zu expected_hle=%zu joint_order_compared=true\n",
        actual_generated, actual_hle, expected_generated, expected_hle);
    CHECK(ps3_atomic_decommit(&domain, 0x9b6000, 4096) == PS3_GUEST_MEMORY_BUSY);
    std::printf("STARTUP_CONTEXT_RETURN imports=%zu tls_returns=%zu mutex_returns=%zu syscall=%u next_r3=0x%llx sp=0x%llx guest_priority=%d guest_stack=0x%x abi_scratch_bytes=4096 context_coherent_generated_writes=true clock_returned=true clock_us=%llu unsupported_indirect_pc=0x462380 indirect_handler_called=false syscall_effects=true\n",
        control.import_count, control.tls_returns, control.mutex_returns, control.syscall_number,
        (unsigned long long)execution.ppu.cpu.gpr[3], (unsigned long long)execution.ppu.cpu.gpr[1],
        execution.guest_priority, plan.process_parameters.primary_stack_size, (unsigned long long)control.clock_sample);
    if (!abandon_observed_lock_after_join(memory, services, waiters, 32, control, execution, startup_joined)) {
        std::printf("STARTUP_CONTEXT_FATAL abandon_failed=true backing_retained=true\n");
        std::fflush(stdout);
        svcExitProcess();
    }
    const uint32_t test_object = ARGV + 0x100, test_attributes = ARGV + 0x200;
    auto attributes = [&](uint32_t protocol, uint32_t recursive) {
        uint32_t bytes[4] = {__builtin_bswap32(protocol), __builtin_bswap32(recursive), 0x12345678, 0x90abcdef};
        CHECK(memory->write(memory->context, test_attributes, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
    };
    auto call = [&](StartupEntry entry, uint64_t r4, uint64_t identity, uint32_t cell,
                    StartupStop wanted_stop = StartupStop::Returned, uint32_t address = 0) {
        StartupControl invocation;
        configure(invocation, oracle);
        invocation.entry = entry;
        StartupExecution owner{};
        prepare(owner, initial, services, invocation, binding, execution.guest_priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[3] = address ? address : test_object;
        owner.ppu.cpu.gpr[4] = r4;
        owner.ppu.cpu.thread_id = identity;
        const ppu_context before = owner.ppu.cpu;
        const uint64_t sentinel_toc = UINT64_C(0x1122334455667788);
        CHECK(memory->write(memory->context, uint32_t(before.gpr[1]) + 0x28,
            &sentinel_toc, sizeof(sentinel_toc)) == PS3_GUEST_MEMORY_OK);
        CHECK(run(owner));
        CHECK(invocation.stop == wanted_stop && provider.active_scopes == 0);
        ppu_context wanted = before;
        if (wanted_stop == StartupStop::Returned) {
            wanted.gpr[3] = uint64_t(int64_t(int32_t(cell)));
            uint64_t stored = 0;
            CHECK(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28, &stored, 8) == PS3_GUEST_MEMORY_OK);
            CHECK(__builtin_bswap64(stored) == toc && invocation.mutex_returns == 1);
        } else {
            CHECK(invocation.mutex_returns == 0);
            uint64_t stored = 0;
            CHECK(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28,
                &stored, sizeof(stored)) == PS3_GUEST_MEMORY_OK);
            CHECK(stored == sentinel_toc);
        }
        CHECK(std::memcmp(&owner.ppu.cpu, &wanted, sizeof(wanted)) == 0);
    };
    StartupControl missing_metadata;
    configure(missing_metadata, oracle);
    missing_metadata.entry = StartupEntry::MutexTryStub;
    StartupExecution missing_owner{};
    prepare(missing_owner, initial, services, missing_metadata, binding, 0, false);
    missing_owner.ppu.cpu.gpr[3] = 0x9b6380;
    const ppu_context missing_before = missing_owner.ppu.cpu;
    const unsigned calls_before_missing = provider.mutex_calls;
    CHECK(run(missing_owner));
    CHECK(missing_metadata.stop == StartupStop::MissingServices && provider.mutex_calls == calls_before_missing);
    CHECK(std::memcmp(&missing_owner.ppu.cpu, &missing_before, sizeof(initial)) == 0);
    attributes(1, 0x20);
    unsigned char readonly_before[PS3_LWMUTEX_BYTES], readonly_after[PS3_LWMUTEX_BYTES];
    CHECK(memory->read(memory->context, 0x10000, readonly_before, sizeof(readonly_before)) == PS3_GUEST_MEMORY_OK);
    const int failures_before_readonly = failures;
    const unsigned calls_before_readonly = provider.mutex_calls;
    const uint32_t queue_before_readonly = registry.next_queue_id;
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0x8001000d, StartupStop::Returned, 0x10000);
    CHECK(memory->read(memory->context, 0x10000, readonly_after, sizeof(readonly_after)) == PS3_GUEST_MEMORY_OK);
    CHECK(std::memcmp(readonly_before, readonly_after, sizeof(readonly_before)) == 0);
    CHECK(provider.mutex_calls == calls_before_readonly && registry.next_queue_id == queue_before_readonly);
    std::printf("STARTUP_MUTEX_READONLY_%s bytes_unchanged=true provider_not_called=true registry_unchanged=true\n",
        failures == failures_before_readonly ? "PASS" : "FAIL");
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0);
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0, StartupStop::MutexProviderFailure);
    call(StartupEntry::MutexTryStub, 0, PS3_LWMUTEX_RESERVED, 0, StartupStop::MissingServices);
    call(StartupEntry::MutexTryStub, 0, UINT64_C(0x100000001), 0, StartupStop::MissingServices);
    call(StartupEntry::MutexTryStub, 0, 1, 0);
    call(StartupEntry::MutexTryStub, 0, 2, 0x8001000a);
    call(StartupEntry::MutexUnlockStub, 0, 2, 0x80010009);
    const uint64_t wide_timeout = UINT64_C(0x100000005);
    call(StartupEntry::MutexLockStub, wide_timeout, 1, 0x80010008);
    CHECK(provider.last_timeout == wide_timeout);
    call(StartupEntry::MutexLockStub, 1000, 2, 0x8001000b);
    call(StartupEntry::MutexLockStub, UINT64_MAX, 2, 0, StartupStop::MutexProviderFailure);
    CHECK(provider.last_timeout == UINT64_MAX);
    call(StartupEntry::MutexDestroyStub, 0, 1, 0x8001000a);
    call(StartupEntry::MutexUnlockStub, 0, 1, 0);
    call(StartupEntry::MutexDestroyStub, 0, 1, 0);
    call(StartupEntry::MutexTryStub, 0, 1, 0, StartupStop::MutexProviderFailure);
    attributes(1, 0);
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0x80010002);
    call(StartupEntry::MutexCreateStub, 0xc0000000, 1, 0x8001000d);
    call(StartupEntry::MutexTryStub, 0, 1, 0x8001000d, StartupStop::Returned, 0xc0000000);
    attributes(3, 0x20);
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0x80010002);
    attributes(4, 0x20);
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0, StartupStop::MutexProviderFailure);
    attributes(1, 0x10);
    call(StartupEntry::MutexCreateStub, test_attributes, 1, 0);
    call(StartupEntry::MutexTryStub, 0, 1, 0);
    const uint32_t maximum_recursion = __builtin_bswap32(UINT32_MAX);
    CHECK(memory->write(memory->context, test_object + 12, &maximum_recursion, 4) == PS3_GUEST_MEMORY_OK);
    call(StartupEntry::MutexTryStub, 0, 1, 0x80010011);
    const uint32_t zero = 0;
    CHECK(memory->write(memory->context, test_object + 12, &zero, 4) == PS3_GUEST_MEMORY_OK);
    call(StartupEntry::MutexUnlockStub, 0, 1, 0);
    call(StartupEntry::MutexDestroyStub, 0, 1, 0);
    std::printf("STARTUP_MUTEX_ABI_%s known_errors=7 full_context=true timeout64=0x%llx residuals_fail_stop=true\n", failures ? "FAIL" : "PASS", (unsigned long long)wide_timeout);
    attributes(1, 0x20);
    StartupControl cancelled;
    configure(cancelled, oracle);
    cancelled.entry = StartupEntry::MutexCreateStub;
    provider.cancel_after_create = &cancelled.cancelled;
    StartupExecution cancelled_owner{};
        prepare(cancelled_owner, initial, services, cancelled, binding, execution.guest_priority);
    cancelled_owner.ppu.cpu.gpr[1] = STACK_TOP + 0x200;
    cancelled_owner.ppu.cpu.gpr[3] = test_object;
    cancelled_owner.ppu.cpu.gpr[4] = test_attributes;
    const ppu_context before_cancel = cancelled_owner.ppu.cpu;
    CHECK(run(cancelled_owner));
    CHECK(cancelled.stop == StartupStop::Cancelled && cancelled.mutex_returns == 0 && provider.active_scopes == 0);
    CHECK(std::memcmp(&cancelled_owner.ppu.cpu, &before_cancel, sizeof(before_cancel)) == 0);
    CHECK(ps3_atomic_decommit(&domain, ARGV, 4096) == PS3_GUEST_MEMORY_BUSY);
    CHECK(ps3_lwmutex_destroy(&registry, test_object).status == PS3_LWMUTEX_OK);
    provider.cancel_after_create = nullptr;
    const auto cleaned = ps3_lwmutex_cleanup(&registry);
    const auto allocation_cleanup = ps3_user_memory_cleanup(&allocator);
    if (cleaned.status != PS3_LWMUTEX_OK || allocation_cleanup.status != PS3_USER_MEMORY_OK) {
        std::printf("STARTUP_MUTEX_FATAL cleanup=%d retained_resources=true\n", cleaned.status);
        std::fflush(stdout);
        svcExitProcess();
    }
    std::printf("STARTUP_MUTEX_CANCEL_PASS joined=true provider_scopes=%u cpu_unpublished=true retained_pin_reclaimed=true\n", provider.active_scopes);
    std::printf("STARTUP_CONTEXT_%s failures=%d cpu_bytes=%zu accesses=%zu all_checked_context_objects=true syscall352_effects=true game_boot=false\n",
        failures ? "FAIL" : "PASS", failures, sizeof(ppu_context), control.journal.count);
    return failures != 0;
#undef CHECK
}
