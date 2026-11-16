#include "../legacy/startup_execution_014.h"
#include "guest_space.h"
#include "elf_image.h"
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <vector>

namespace {
constexpr uint32_t STACK_TOP = 0xd0100000;
constexpr uint32_t STACK_START = STACK_TOP - 0x10000;
constexpr size_t STACK_BYTES = 0x11000;
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
}

extern "C" int native_test_startup_resume(const ps3_guest_memory *memory, uintptr_t base, const char *path)
{
    int failures = 0;
#define CHECK(value) do { if (!(value)) { std::printf("STARTUP_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    FILE *file = std::fopen(path, "rb");
    if (!file) return 1;
    if (std::fseek(file, 0, SEEK_END)) { std::fclose(file); return 1; }
    long size = std::ftell(file);
    if (size <= 0) { std::fclose(file); return 1; }
    ElfImageReader reader{read_file, file, uint64_t(size)};
    ElfImagePlan plan{}; ElfImageSegment segments[ELF_IMAGE_MAX_SEGMENTS];
    ElfImageStatus planned = elf_image_plan(&reader, segments, ELF_IMAGE_MAX_SEGMENTS, &plan);
    std::fclose(file);
    if (planned != ELF_IMAGE_OK || plan.segment_count != 2 || !plan.has_tls || plan.entry_opd != 0x771b08 ||
        plan.tls.guest_address != 0x785e7c || plan.tls.file_size != 4 || plan.tls.memory_size != 0x200) return 1;
    Oracle oracle;
    for (size_t index = 0; index < plan.segment_count; ++index) {
        if (segments[index].memory_size > 16 * 1024 * 1024 || uint64_t(segments[index].guest_address) + segments[index].memory_size >= ARGV) return 1;
        if (!snapshot(memory, oracle, segments[index].guest_address, segments[index].memory_size, (segments[index].flags & 2) != 0)) return 1;
    }
    CHECK(memory->commit(memory->context, STACK_START, STACK_BYTES) == PS3_GUEST_MEMORY_OK);
    CHECK(memory->commit(memory->context, ARGV, 4096) == PS3_GUEST_MEMORY_OK);
    std::vector<uint8_t> stack(STACK_BYTES, 0xa5), argv(4096, 0);
    CHECK(memory->write(memory->context, STACK_START, stack.data(), stack.size()) == PS3_GUEST_MEMORY_OK);
    const char boot_path[] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    uint64_t pointer = __builtin_bswap64(uint64_t(ARGV + 0x20));
    std::memcpy(argv.data(), &pointer, 8); std::memcpy(argv.data() + 0x20, boot_path, sizeof(boot_path));
    CHECK(memory->write(memory->context, ARGV, argv.data(), argv.size()) == PS3_GUEST_MEMORY_OK);
    CHECK(snapshot(memory, oracle, STACK_START, STACK_BYTES, true));
    CHECK(snapshot(memory, oracle, ARGV, 4096, true));
    const uint32_t code = uint32_t(oracle.read(plan.entry_opd, 4, false));
    const uint32_t toc = uint32_t(oracle.read(plan.entry_opd + 4, 4, false));
    CHECK(code == 0x10230 && toc == 0x78bf78 && oracle.valid);
    if (failures || !native_startup_bind_memory(base)) return 1;
    TlsProvider provider{memory, {TLS_BLOCK, TLS_BLOCK + 0x10000}};
    Ps3PpuExecutionServices services{&provider, initialize_tls};
    StartupControl control; configure(control, oracle, base);
    StartupExecution execution{}; execution.services = &services; execution.control = &control;
    execution.cpu.gpr[1] = STACK_TOP; execution.cpu.gpr[2] = toc;
    execution.cpu.gpr[3] = 1; execution.cpu.gpr[4] = ARGV; execution.cpu.gpr[5] = ENVP;
    execution.cpu.gpr[7] = 1; execution.cpu.gpr[8] = plan.tls.guest_address;
    execution.cpu.gpr[9] = plan.tls.file_size; execution.cpu.gpr[10] = plan.tls.memory_size;
    execution.cpu.gpr[12] = 0x100000; execution.cpu.gpr[13] = 0x10f07000;
    execution.cpu.thread_id = 1; execution.cpu.cia = code;
    const ppu_context initial = execution.cpu;
    ppu_context expected = initial; model_to_tls(expected, oracle);
    const ppu_context expected_before_tls = expected;
    oracle.write(expected.gpr[1] + 0x28, expected.gpr[2], 8);
    expected.gpr[13] = TLS_BLOCK + 0x7000; expected.gpr[3] = 0;
    const ppu_context expected_after_tls = expected;
    Region tls_region{TLS_BLOCK, std::vector<uint8_t>(0x9000, 0), true};
    uint8_t *tls_source = oracle.find(plan.tls.guest_address, 4);
    if (tls_source) std::memcpy(tls_region.bytes.data(), tls_source, 4);
    oracle.regions.push_back(std::move(tls_region));
    model_after_tls_to_mutex(expected, oracle);
    CHECK(run(execution));
    CHECK(control.stop == StartupStop::UnknownImport);
    CHECK(control.import_count == 2 && control.imports[0] == 0x744680a2 && control.imports[1] == 0x2f85c0ef);
    CHECK(control.tls_returns == 1 && provider.calls == 1 && provider.active_scopes == 0);
    CHECK(oracle.valid && std::memcmp(&control.before_tls, &expected_before_tls, sizeof(expected)) == 0);
    CHECK(std::memcmp(&control.after_tls, &expected_after_tls, sizeof(expected)) == 0);
    if (std::memcmp(&execution.cpu, &expected, sizeof(expected))) {
        for (unsigned reg = 0; reg < 32; ++reg) if (execution.cpu.gpr[reg] != expected.gpr[reg])
            std::printf("STARTUP_REGISTER_MISMATCH r%u actual=%llx expected=%llx\n", reg, (unsigned long long)execution.cpu.gpr[reg], (unsigned long long)expected.gpr[reg]);
        CHECK(false);
    }
    CHECK(control.access_count == oracle.accesses.size());
    for (size_t index = 0; index < control.access_count && index < oracle.accesses.size(); ++index) {
        const StartupAccess &actual = control.accesses[index], &wanted = oracle.accesses[index];
        CHECK(actual.address == wanted.address && actual.value == wanted.value && actual.bytes == wanted.bytes && actual.write == wanted.write);
        std::printf("STARTUP_ACCESS index=%zu address=%llx bytes=%u write=%d value=%llx\n", index,
            (unsigned long long)actual.address, actual.bytes, actual.write, (unsigned long long)actual.value);
    }
    CHECK(compare(memory, oracle));
    std::printf("STARTUP_TLS_RETURN tls_calls=%u provider_active_scopes=%u imports=%zu next_nid=0x%x lr=0x%llx r3=0x%llx r4=0x%llx envp=0x%x unknown_handler_called=false\n",
        provider.calls, provider.active_scopes, control.import_count, control.import_count > 1 ? control.imports[1] : 0,
        (unsigned long long)execution.cpu.lr, (unsigned long long)execution.cpu.gpr[3], (unsigned long long)execution.cpu.gpr[4], ENVP);
    // Real cancellation checks at ordinary VM and HLE boundaries; no providers invoked.
    for (StartupEntry entry : {StartupEntry::Game, StartupEntry::TlsStub}) {
        StartupControl cancelled; configure(cancelled, oracle, base); cancelled.entry = entry;
        cancelled.cancelled.store(true, std::memory_order_release);
        StartupExecution negative{initial, &services, &cancelled};
        CHECK(run(negative)); CHECK(cancelled.stop == StartupStop::Cancelled);
        ppu_context cancelled_expected = initial;
        if (entry == StartupEntry::Game) cancelled_expected.gpr[2] = plan.entry_opd;
        CHECK(std::memcmp(&negative.cpu, &cancelled_expected, sizeof(initial)) == 0);
        CHECK(provider.calls == 1 && provider.active_scopes == 0 && compare(memory, oracle));
    }
    StartupControl expired; configure(expired, oracle, base); expired.deadline_tick = armGetSystemTick() - 1;
    StartupExecution deadline{initial, &services, &expired}; CHECK(run(deadline));
    CHECK(expired.stop == StartupStop::Deadline && compare(memory, oracle));
    StartupControl unsupported; configure(unsupported, oracle, base); unsupported.entry = StartupEntry::UnsupportedExternal;
    StartupExecution external{initial, &services, &unsupported}; CHECK(run(external));
    CHECK(unsupported.stop == StartupStop::UnsupportedExternal && unsupported.external_id != 0 && compare(memory, oracle));
    // Boundary guards reject missing services and corrupt counters before publication.
    const int guards_before = failures;
    Ps3PpuExecutionServices missing_callback{nullptr, nullptr};
    const Ps3PpuExecutionServices *missing_ports[2] = {nullptr, &missing_callback};
    for (const Ps3PpuExecutionServices *missing : missing_ports) {
        StartupControl missing_control; configure(missing_control, oracle, base); missing_control.entry = StartupEntry::TlsStub;
        StartupExecution missing_execution{initial, missing, &missing_control};
        CHECK(run(missing_execution)); CHECK(missing_control.stop == StartupStop::MissingServices);
        CHECK(std::memcmp(&missing_execution.cpu, &initial, sizeof(initial)) == 0 && provider.calls == 1 && compare(memory, oracle));
    }
    for (unsigned corrupt = 0; corrupt < 3; ++corrupt) {
        StartupControl invalid; configure(invalid, oracle, base);
        if (corrupt == 0) invalid.span_count = STARTUP_SPAN_CAPACITY + 1;
        if (corrupt == 1) invalid.access_count = STARTUP_TRACE_CAPACITY + 1;
        if (corrupt == 2) { invalid.entry = StartupEntry::TlsStub; invalid.import_count = STARTUP_IMPORT_CAPACITY + 1; }
        StartupExecution invalid_execution{initial, &services, &invalid};
        CHECK(run(invalid_execution));
        CHECK(invalid.stop == (corrupt == 0 ? StartupStop::InvalidMemory : StartupStop::TraceCapacity));
        ppu_context invalid_expected = initial;
        if (corrupt != 2) invalid_expected.gpr[2] = plan.entry_opd;
        CHECK(std::memcmp(&invalid_execution.cpu, &invalid_expected, sizeof(initial)) == 0 && provider.calls == 1 && compare(memory, oracle));
    }
    std::printf("STARTUP_BOUNDARY_GUARDS_%s missing_services=2 corrupt_counters=3\n", failures == guards_before ? "PASS" : "FAIL");
    // Cancellation is set AFTER a real commit, while provider RAII still owns its lock.
    const int cancellation_before = failures;
    bool cancelled_joined = false, reclaimed = false;
    const uint32_t cancelled_block = 0x0e400000;
    TlsProvider committing{memory, {cancelled_block, cancelled_block + 0x10000}};
    std::atomic<bool> committed_ready{false}, allow_return{false};
    committing.committed_ready = &committed_ready; committing.allow_return = &allow_return;
    Ps3PpuExecutionServices committing_services{&committing, initialize_tls};
    StartupControl in_provider; configure(in_provider, oracle, base); in_provider.entry = StartupEntry::TlsStub;
    StartupExecution interrupted{initial, &committing_services, &in_provider};
    interrupted.cpu.gpr[1] = STACK_TOP + 0x300;
    interrupted.cpu.gpr[4] = plan.tls.guest_address; interrupted.cpu.gpr[5] = 4; interrupted.cpu.gpr[6] = 0x200;
    const ppu_context interrupted_initial = interrupted.cpu;
    Thread interrupted_thread;
    const bool interrupted_started = start(interrupted, interrupted_thread);
    CHECK(interrupted_started);
    if (interrupted_started) {
        const uint64_t ready_deadline = armGetSystemTick() + armNsToTicks(TIMEOUT_NS);
        while (!committed_ready.load(std::memory_order_acquire) && armGetSystemTick() < ready_deadline) svcSleepThread(1000);
        CHECK(committed_ready.load(std::memory_order_acquire));
        uint8_t actual[512] = {}, wanted[4] = {};
        CHECK(memory->read(memory->context, cancelled_block, actual, sizeof(actual)) == PS3_GUEST_MEMORY_OK);
        CHECK(memory->read(memory->context, plan.tls.guest_address, wanted, 4) == PS3_GUEST_MEMORY_OK);
        CHECK(std::memcmp(actual, wanted, 4) == 0);
        for (unsigned byte = 4; byte < sizeof(actual); ++byte) CHECK(actual[byte] == 0);
        const bool premature_release = mutexTryLock(&committing.mutex);
        CHECK(!premature_release);
        if (premature_release) mutexUnlock(&committing.mutex);
        in_provider.cancelled.store(true, std::memory_order_release);
        allow_return.store(true, std::memory_order_release);
        join(interrupted_thread);
        cancelled_joined = true;
        CHECK(in_provider.stop == StartupStop::Cancelled && in_provider.tls_returns == 0 && in_provider.access_count == 0);
        CHECK(std::memcmp(&interrupted.cpu, &interrupted_initial, sizeof(initial)) == 0);
        CHECK(committing.calls == 1 && committing.active_scopes == 0 && committing.arena.next == cancelled_block + 0x9000);
        const bool lock_released = mutexTryLock(&committing.mutex);
        CHECK(lock_released);
        if (lock_released) mutexUnlock(&committing.mutex);
        CHECK(compare(memory, oracle));
        reclaimed = memory->decommit(memory->context, cancelled_block, 0x9000) == PS3_GUEST_MEMORY_OK;
        CHECK(reclaimed);
        std::memset(actual, 0x55, sizeof(actual));
        CHECK(memory->read(memory->context, cancelled_block, actual, 4) == PS3_GUEST_MEMORY_UNMAPPED);
        CHECK(actual[0] == 0x55 && actual[3] == 0x55);
    }
    std::printf("STARTUP_IN_PROVIDER_CANCEL_%s committed_ready=%d active_scopes=%u tls_returns=%zu joined=%d reclaimed=%d\n",
        failures == cancellation_before ? "PASS" : "FAIL", committed_ready.load(std::memory_order_acquire),
        committing.active_scopes, in_provider.tls_returns, cancelled_joined, reclaimed);
    // Two real TLS providers overlap in time; one waiting provider cannot block dispatch on the other worker.
    std::atomic<unsigned> ready{0};
    TlsProvider separate[2] = {{memory, {0x0e100000, 0x0e110000}},
                              {memory, {0x0e200000, 0x0e210000}}};
    Ps3PpuExecutionServices separate_services[2] = {{&separate[0], initialize_tls}, {&separate[1], initialize_tls}};
    StartupControl separate_controls[2];
    StartupExecution isolated[2]{};
    ppu_context isolated_expected[2];
    Thread independent_threads[2];
    bool independent_started[2] = {false, false};
    for (unsigned index = 0; index < 2; ++index) {
        separate[index].concurrent_ready = &ready;
        configure(separate_controls[index], oracle, base);
        separate_controls[index].entry = StartupEntry::TlsStub;
        isolated[index] = {initial, &separate_services[index], &separate_controls[index]};
        isolated[index].cpu.gpr[1] = STACK_TOP + 0x100 + index * 0x100;
        isolated[index].cpu.gpr[2] = toc + index;
        isolated[index].cpu.gpr[4] = plan.tls.guest_address;
        isolated[index].cpu.gpr[5] = 4; isolated[index].cpu.gpr[6] = 0x200;
        isolated[index].cpu.thread_id = index + 10;
        isolated_expected[index] = isolated[index].cpu;
        isolated_expected[index].gpr[3] = 0;
        isolated_expected[index].gpr[13] = separate[index].arena.next + 0x7000;
        independent_started[index] = start(isolated[index], independent_threads[index]);
        CHECK(independent_started[index]);
    }
    for (unsigned index = 0; index < 2; ++index) {
        if (!independent_started[index]) continue;
        join(independent_threads[index]);
        const uint32_t block = 0x0e100000 + index * 0x100000;
        CHECK(separate_controls[index].stop == StartupStop::Returned && separate[index].calls == 1 && separate[index].active_scopes == 0);
        CHECK(std::memcmp(&isolated[index].cpu, &isolated_expected[index], sizeof(initial)) == 0 && provider.calls == 1);
        uint8_t bytes[4096] = {}, source[4] = {};
        CHECK(memory->read(memory->context, plan.tls.guest_address, source, 4) == PS3_GUEST_MEMORY_OK);
        for (unsigned offset = 0; offset < 0x9000; offset += sizeof(bytes)) {
            CHECK(memory->read(memory->context, block + offset, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK);
            for (unsigned byte = 0; byte < sizeof(bytes); ++byte) CHECK(bytes[byte] == (offset + byte < 4 ? source[offset + byte] : 0));
        }
        const uint32_t toc_slot = uint32_t(isolated[index].cpu.gpr[1]) + 0x28;
        uint64_t actual_toc = 0;
        CHECK(memory->read(memory->context, toc_slot, &actual_toc, 8) == PS3_GUEST_MEMORY_OK);
        CHECK(__builtin_bswap64(actual_toc) == toc + index);
        CHECK(memory->write(memory->context, toc_slot, oracle.find(toc_slot, 8), 8) == PS3_GUEST_MEMORY_OK);
        CHECK(memory->decommit(memory->context, block, 0x9000) == PS3_GUEST_MEMORY_OK);
    }
    CHECK(ready.load(std::memory_order_acquire) == 2 && compare(memory, oracle));
    // A genuine service failure returns before worker termination, releasing provider RAII locks.
    TlsProvider exhausted{memory, {0x0e300000, 0x0e300000}};
    Ps3PpuExecutionServices failed_services{&exhausted, initialize_tls};
    StartupControl failed; configure(failed, oracle, base); failed.entry = StartupEntry::TlsStub;
    StartupExecution failed_execution{initial, &failed_services, &failed};
    failed_execution.cpu.gpr[4] = plan.tls.guest_address; failed_execution.cpu.gpr[5] = 4; failed_execution.cpu.gpr[6] = 0x200;
    CHECK(run(failed_execution)); CHECK(failed.stop == StartupStop::ProviderFailure && exhausted.calls == 1 && exhausted.active_scopes == 0);
    const bool acquired = mutexTryLock(&exhausted.mutex);
    CHECK(acquired);
    if (acquired) mutexUnlock(&exhausted.mutex);
    CHECK(compare(memory, oracle));
    native_startup_unbind_memory(base);
    std::printf("STARTUP_RESUME_%s failures=%d cpu_bytes=%zu accesses=%zu all_original_objects=true weakened_generated_symbols=0 cancellation_boundaries=2 independent_providers=2 joined_before_release=true game_boot=false\n",
        failures ? "FAIL" : "PASS", failures, sizeof(ppu_context), control.access_count);
    return failures != 0;
}
