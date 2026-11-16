#include "../legacy/startup_execution_014.h"
#include "guest_space.h"
#include "elf_image.h"
extern "C" {
#include "../adapters/lwmutex_wait_horizon.h"
}
#include <switch.h>
#include <cstdio>
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
            else value = *bytes;
        }
        if (trace) accesses.push_back({address, value, width, false});
        return value;
    }
    void write(uint64_t address, uint64_t value, unsigned width, bool trace = true) {
        uint8_t *bytes = find(address, width);
        if (bytes) {
            if (width == 4) { uint32_t raw = __builtin_bswap32(uint32_t(value)); std::memcpy(bytes, &raw, 4); }
            else if (width == 8) { uint64_t raw = __builtin_bswap64(value); std::memcpy(bytes, &raw, 8); }
            else *bytes = uint8_t(value);
        }
        if (trace) accesses.push_back({address, value, width, true});
    }
};
struct TlsProvider {
    const ps3_guest_memory *memory;
    Ps3TlsArena arena;
    ps3_lwmutex_registry *registry = nullptr;
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
    return true;
}

void configure(StartupControl &control, const Oracle &oracle, uintptr_t base)
{
    control.base = base;
    control.deadline_tick = armGetSystemTick() + armNsToTicks(TIMEOUT_NS);
    for (const Region &region : oracle.regions) {
        if (control.span_count >= STARTUP_SPAN_CAPACITY) std::abort();
        control.spans[control.span_count++] = {region.address, region.bytes.size(), region.writable};
    }
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
void model_create_to_syscall(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    const uint32_t object = uint32_t(cpu.gpr[3]);
    const uint32_t words[] = {0xffffffff, 0, 0x12, 0, queue, 0};
    for (unsigned index = 0; index < 6; ++index) memory.write(object + index * 4, words[index], 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8);
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


extern "C" int native_test_startup_lwmutex(const ps3_guest_memory *backing, uintptr_t base, const char *path)
{
    int failures = 0;
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
    ps3_atomic_pin_record pins[8]{};
    const ps3_instance_guard guard{&domain_mutex, lock_domain, unlock_domain};
    CHECK(ps3_atomic_domain_init(&domain, *backing, guard, owners, 8, pins, 8) == PS3_GUEST_MEMORY_OK);
    ps3_guest_memory coherent = ps3_atomic_domain_memory(&domain);
    const ps3_guest_memory *memory = &coherent;
    ps3_horizon_lwmutex_wait wait{};
    CHECK(ps3_horizon_lwmutex_wait_init(&wait, 2000000) == PS3_WAIT_READY);
    ps3_lwmutex_registry registry{};
    ps3_lwmutex_object objects[8]{};
    ps3_lwmutex_waiter waiters[8]{};
    CHECK(ps3_lwmutex_init(&registry, &domain, ps3_horizon_lwmutex_wait_guard(&wait),
        ps3_horizon_lwmutex_wait_port(&wait), objects, 8, waiters, 8).status == PS3_LWMUTEX_OK);
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
    if (failures || !native_startup_bind_memory(base)) return 1;
    TlsProvider provider{memory, {TLS_BLOCK, TLS_BLOCK + 0x10000}};
    provider.registry = &registry;
    Ps3PpuExecutionServices services{&provider, initialize_tls, memory, create_mutex,
        lock_mutex, try_mutex, unlock_mutex, destroy_mutex};
    StartupControl control;
    configure(control, oracle, base);
    StartupExecution execution{};
    execution.services = &services;
    execution.control = &control;
    execution.guest_priority = plan.process_parameters.primary_priority;
    execution.has_guest_priority = true;
    execution.cpu.gpr[1] = STACK_TOP;
    execution.cpu.gpr[2] = toc;
    execution.cpu.gpr[3] = 1;
    execution.cpu.gpr[4] = ARGV;
    execution.cpu.gpr[5] = ENVP;
    execution.cpu.gpr[7] = 1;
    execution.cpu.gpr[8] = plan.tls.guest_address;
    execution.cpu.gpr[9] = plan.tls.file_size;
    execution.cpu.gpr[10] = plan.tls.memory_size;
    execution.cpu.gpr[12] = plan.process_parameters.malloc_page_size;
    execution.cpu.gpr[13] = 0x10f07000;
    execution.cpu.thread_id = 1;
    execution.cpu.cia = code;
    const ppu_context initial = execution.cpu;
    ppu_context expected = initial;
    model_to_tls(expected, oracle);
    const ppu_context expected_before_tls = expected;
    oracle.write(expected.gpr[1] + 0x28, expected.gpr[2], 8);
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
    CHECK(run(execution));
    CHECK(control.stop == StartupStop::UnsupportedSyscall && control.syscall_number == 352);
    CHECK(control.import_count == 2 && control.imports[0] == 0x744680a2 && control.imports[1] == 0x2f85c0ef);
    CHECK(control.tls_returns == 1 && control.mutex_returns == 1 && provider.calls == 1 && provider.mutex_calls == 1);
    CHECK(provider.active_scopes == 0 && registry.next_queue_id == first_queue + 1);
    CHECK(std::memcmp(&control.before_tls, &expected_before_tls, sizeof(expected)) == 0);
    CHECK(std::memcmp(&control.after_tls, &expected_after_tls, sizeof(expected)) == 0);
    if (std::memcmp(&execution.cpu, &expected, sizeof(expected))) {
        for (unsigned reg = 0; reg < 32; ++reg) if (execution.cpu.gpr[reg] != expected.gpr[reg])
            std::printf("STARTUP_MUTEX_REGISTER r%u actual=%llx expected=%llx\n", reg,
                (unsigned long long)execution.cpu.gpr[reg], (unsigned long long)expected.gpr[reg]);
        CHECK(false);
    }
    CHECK(oracle.valid && control.access_count == oracle.accesses.size());
    for (size_t index = 0; index < control.access_count && index < oracle.accesses.size(); ++index) {
        const StartupAccess &actual = control.accesses[index], &wanted = oracle.accesses[index];
        CHECK(actual.address == wanted.address && actual.value == wanted.value &&
              actual.bytes == wanted.bytes && actual.write == wanted.write);
        std::printf("STARTUP_MUTEX_ACCESS index=%zu address=%llx bytes=%u write=%d value=%llx\n", index,
            (unsigned long long)actual.address, actual.bytes, actual.write, (unsigned long long)actual.value);
    }
    CHECK(compare(memory, oracle));
    CHECK(ps3_atomic_decommit(&domain, 0x9b6000, 4096) == PS3_GUEST_MEMORY_BUSY);
    std::printf("STARTUP_CREATE_RETURN imports=%zu tls_returns=%zu mutex_returns=%zu syscall=%u info=0x%llx sp=0x%llx guest_priority=%d guest_stack=0x%x abi_scratch_bytes=4096 raw_single_worker_page_store=true syscall_effects=false\n",
        control.import_count, control.tls_returns, control.mutex_returns, control.syscall_number,
        (unsigned long long)execution.cpu.gpr[3], (unsigned long long)execution.cpu.gpr[1],
        execution.guest_priority, plan.process_parameters.primary_stack_size);
    const uint32_t test_object = ARGV + 0x100, test_attributes = ARGV + 0x200;
    auto attributes = [&](uint32_t protocol, uint32_t recursive) {
        uint32_t bytes[4] = {__builtin_bswap32(protocol), __builtin_bswap32(recursive), 0x12345678, 0x90abcdef};
        CHECK(memory->write(memory->context, test_attributes, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
    };
    auto call = [&](StartupEntry entry, uint64_t r4, uint64_t identity, uint32_t cell,
                    StartupStop wanted_stop = StartupStop::Returned, uint32_t address = 0) {
        StartupControl invocation;
        configure(invocation, oracle, base);
        invocation.entry = entry;
        StartupExecution owner{initial, &services, &invocation, execution.guest_priority, true};
        owner.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.cpu.gpr[3] = address ? address : test_object;
        owner.cpu.gpr[4] = r4;
        owner.cpu.thread_id = identity;
        const ppu_context before = owner.cpu;
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
        CHECK(std::memcmp(&owner.cpu, &wanted, sizeof(wanted)) == 0);
    };
    StartupControl missing_metadata;
    configure(missing_metadata, oracle, base);
    missing_metadata.entry = StartupEntry::MutexTryStub;
    StartupExecution missing_owner{initial, &services, &missing_metadata};
    missing_owner.cpu.gpr[3] = 0x9b6380;
    const ppu_context missing_before = missing_owner.cpu;
    const unsigned calls_before_missing = provider.mutex_calls;
    CHECK(run(missing_owner));
    CHECK(missing_metadata.stop == StartupStop::MissingServices && provider.mutex_calls == calls_before_missing);
    CHECK(std::memcmp(&missing_owner.cpu, &missing_before, sizeof(initial)) == 0);
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
    configure(cancelled, oracle, base);
    cancelled.entry = StartupEntry::MutexCreateStub;
    provider.cancel_after_create = &cancelled.cancelled;
    StartupExecution cancelled_owner{initial, &services, &cancelled, execution.guest_priority, true};
    cancelled_owner.cpu.gpr[1] = STACK_TOP + 0x200;
    cancelled_owner.cpu.gpr[3] = test_object;
    cancelled_owner.cpu.gpr[4] = test_attributes;
    const ppu_context before_cancel = cancelled_owner.cpu;
    CHECK(run(cancelled_owner));
    CHECK(cancelled.stop == StartupStop::Cancelled && cancelled.mutex_returns == 0 && provider.active_scopes == 0);
    CHECK(std::memcmp(&cancelled_owner.cpu, &before_cancel, sizeof(before_cancel)) == 0);
    CHECK(ps3_atomic_decommit(&domain, ARGV, 4096) == PS3_GUEST_MEMORY_BUSY);
    CHECK(ps3_lwmutex_destroy(&registry, test_object).status == PS3_LWMUTEX_OK);
    provider.cancel_after_create = nullptr;
    const auto cleaned = ps3_lwmutex_cleanup(&registry);
    if (cleaned.status != PS3_LWMUTEX_OK) {
        std::printf("STARTUP_MUTEX_FATAL cleanup=%d retained_resources=true\n", cleaned.status);
        std::fflush(stdout);
        svcExitProcess();
    }
    native_startup_unbind_memory(base);
    std::printf("STARTUP_MUTEX_CANCEL_PASS joined=true provider_scopes=%u cpu_unpublished=true retained_pin_reclaimed=true\n", provider.active_scopes);
    std::printf("STARTUP_LWMUTEX_%s failures=%d cpu_bytes=%zu accesses=%zu all_original_objects=true syscall352_effects=false game_boot=false\n",
        failures ? "FAIL" : "PASS", failures, sizeof(ppu_context), control.access_count);
    return failures != 0;
#undef CHECK
}
