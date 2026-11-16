#include "startup_execution_014.h"
#include <switch.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#undef vm_read8
#undef vm_read16
#undef vm_read32
#undef vm_read64
#undef vm_write8
#undef vm_write16
#undef vm_write32
#undef vm_write64

extern "C" {
uint8_t *vm_base = nullptr;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void *) = nullptr;
}
namespace {
// Raw owned memory, architectural state and cancellation data only; no services.
thread_local StartupControl *active = nullptr;
[[noreturn]] void stop(StartupStop reason)
{
    if (!active || !active->cpu) std::abort();
    active->stop = reason;
    active->terminal = *active->cpu;
    g_trampoline_fn = nullptr;
    active = nullptr;
    threadExit();
}
void check_boundary()
{
    if (!active) std::abort();
    if (active->cancelled.load(std::memory_order_acquire)) stop(StartupStop::Cancelled);
    if (active->deadline_tick && armGetSystemTick() >= active->deadline_tick) stop(StartupStop::Deadline);
}
uint8_t *checked_address(uint64_t address, unsigned bytes, bool write)
{
    check_boundary();
    bool covered = false;
    if (active->span_count > STARTUP_SPAN_CAPACITY) stop(StartupStop::InvalidMemory);
    if (address <= UINT32_MAX && bytes <= UINT64_C(0x100000000) - address) {
        for (size_t index = 0; index < active->span_count; ++index) {
            const StartupSpan &span = active->spans[index];
            if (address >= span.address && address - span.address <= span.length &&
                bytes <= span.length - (address - span.address) && (!write || span.writable)) covered = true;
        }
    }
    if (!covered) {
        active->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
    if (active->access_count >= STARTUP_TRACE_CAPACITY) stop(StartupStop::TraceCapacity);
    return reinterpret_cast<uint8_t *>(active->base + address);
}
uint64_t read_guest(uint64_t address, unsigned bytes)
{
    const uint8_t *source = checked_address(address, bytes, false);
    uint64_t value = 0;
    for (unsigned index = 0; index < bytes; ++index) value = (value << 8) | source[index];
    active->accesses[active->access_count++] = {address, value, bytes, false};
    return value;
}
void write_guest(uint64_t address, uint64_t value, unsigned bytes)
{
    uint8_t *destination = checked_address(address, bytes, true);
    for (unsigned index = 0; index < bytes; ++index) destination[index] = uint8_t(value >> ((bytes - index - 1) * 8));
    active->accesses[active->access_count++] = {address, value, bytes, true};
}
}
bool native_startup_bind_memory(uintptr_t base)
{
    if (!base || vm_base || base > UINTPTR_MAX - UINT32_MAX) return false;
    vm_base = reinterpret_cast<uint8_t *>(base);
    return true;
}
void native_startup_unbind_memory(uintptr_t base)
{
    // Caller has joined every worker before releasing this raw legacy data ABI.
    if (vm_base != reinterpret_cast<uint8_t *>(base)) std::abort();
    vm_base = nullptr;
}
extern "C" {
uint8_t vm_read8(uint64_t address) { return uint8_t(read_guest(address, 1)); }
uint16_t vm_read16(uint64_t address) { return uint16_t(read_guest(address, 2)); }
uint32_t vm_read32(uint64_t address) { return uint32_t(read_guest(address, 4)); }
uint64_t vm_read64(uint64_t address) { return read_guest(address, 8); }
void vm_write8(uint64_t address, uint8_t value) { write_guest(address, value, 1); }
void vm_write16(uint64_t address, uint16_t value) { write_guest(address, value, 2); }
void vm_write32(uint64_t address, uint32_t value) { write_guest(address, value, 4); }
void vm_write64(uint64_t address, uint64_t value) { write_guest(address, value, 8); }
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
static int mutex_cancelled(void *context)
{
    return static_cast<StartupControl *>(context)->cancelled.load(std::memory_order_acquire);
}
static bool is_mutex_import(uint32_t nid)
{
    return nid == 0x2f85c0ef || nid == 0x1573dc3f || nid == 0xaeb78725 ||
           nid == 0x1bc200f4 || nid == 0xc3476d0c;
}
static bool caller_pointer_failure(ps3_guest_memory_status status)
{
    return status == PS3_GUEST_MEMORY_INVALID_RANGE || status == PS3_GUEST_MEMORY_UNMAPPED;
}
static uint32_t mutex_cell_result(ps3_lwmutex_result result)
{
    switch (result.status) {
    case PS3_LWMUTEX_OK: return 0;
    case PS3_LWMUTEX_NOT_OWNER: return 0x80010009;
    case PS3_LWMUTEX_BUSY: return 0x8001000a;
    case PS3_LWMUTEX_DEADLOCK: return 0x80010008;
    case PS3_LWMUTEX_OVERFLOW: return 0x80010011;
    case PS3_LWMUTEX_TIMED_OUT: return 0x8001000b;
    default:
        active->mutex_failure = result;
        std::printf("STARTUP_MUTEX_CAPABILITY_STOP nid=0x%x status=%d memory=%d wait=%d\n",
            active->mutex_nid, result.status, result.memory_status, result.wait_status);
        stop(StartupStop::MutexProviderFailure);
    }
}
static bool owner_writable(const StartupControl *control, uint32_t address, uint64_t length)
{
    if (control->span_count > STARTUP_SPAN_CAPACITY || length > UINT64_C(0x100000000) - address) return false;
    for (size_t index = 0; index < control->span_count; ++index) {
        const StartupSpan &span = control->spans[index];
        if (span.writable && address >= span.address && address - span.address <= span.length &&
            length <= span.length - (address - span.address)) return true;
    }
    return false;
}
static uint32_t invoke_mutex(StartupExecution *execution, uint32_t nid)
{
    const Ps3PpuExecutionServices *services = execution->services;
    ppu_context *cpu = &execution->cpu;
    if (!services || !services->memory || !services->memory->read || !services->memory->write ||
        !services->create_mutex || !services->lock_mutex || !services->try_mutex ||
        !services->unlock_mutex || !services->destroy_mutex) stop(StartupStop::MissingServices);
    active->mutex_nid = nid;
    // Guest pointers use the established low32 ABI. Probe the entire control
    // object before any operation; no raw base pointer is a memory capability.
    const uint32_t address = uint32_t(cpu->gpr[3]);
    if (address % 8 || !owner_writable(execution->control, address, PS3_LWMUTEX_BYTES)) return 0x8001000d;
    unsigned char object[PS3_LWMUTEX_BYTES];
    ps3_guest_memory_status checked = services->memory->read(services->memory->context,
        address, object, sizeof(object));
    if (checked != PS3_GUEST_MEMORY_OK) {
        if (caller_pointer_failure(checked)) return 0x8001000d;
        active->mutex_failure = {PS3_LWMUTEX_MEMORY_FAILURE, checked, PS3_WAIT_READY};
        stop(StartupStop::MutexProviderFailure);
    }
    ps3_lwmutex_result result{};
    if (nid == 0x2f85c0ef) {
        unsigned char bytes[16];
        checked = services->memory->read(services->memory->context, uint32_t(cpu->gpr[4]), bytes, sizeof(bytes));
        if (checked != PS3_GUEST_MEMORY_OK) {
            if (caller_pointer_failure(checked)) return 0x8001000d;
            active->mutex_failure = {PS3_LWMUTEX_MEMORY_FAILURE, checked, PS3_WAIT_READY};
            stop(StartupStop::MutexProviderFailure);
        }
        Ps3PpuLwmutexAttributes attributes{};
        for (unsigned index = 0; index < 4; ++index) {
            attributes.protocol = (attributes.protocol << 8) | bytes[index];
            attributes.recursive = (attributes.recursive << 8) | bytes[index + 4];
        }
        std::memcpy(attributes.name, bytes + 8, sizeof(attributes.name));
        if ((attributes.protocol != PS3_LWMUTEX_FIFO && attributes.protocol != PS3_LWMUTEX_PRIORITY &&
             attributes.protocol != 4) || (attributes.recursive != PS3_LWMUTEX_RECURSIVE &&
             attributes.recursive != PS3_LWMUTEX_NONRECURSIVE)) return 0x80010002;
        result = services->create_mutex(services->context, address, &attributes);
    } else if (nid == 0xc3476d0c) {
        result = services->destroy_mutex(services->context, address);
    } else {
        if (!execution->has_guest_priority || !cpu->thread_id || cpu->thread_id >= PS3_LWMUTEX_RESERVED)
            stop(StartupStop::MissingServices);
        ps3_lwmutex_thread identity{uint32_t(cpu->thread_id), execution->guest_priority};
        if (nid == 0x1573dc3f) {
            ps3_wait_cancel cancel{execution->control, mutex_cancelled};
            result = services->lock_mutex(services->context, address, identity, cpu->gpr[4], cancel);
        } else if (nid == 0xaeb78725) {
            result = services->try_mutex(services->context, address, identity);
        } else {
            result = services->unlock_mutex(services->context, address, identity);
        }
    }
    if (nid == 0x1573dc3f && result.status == PS3_LWMUTEX_OK) {
        active->observed_lock_address = address;
        active->observed_lock_owner = uint32_t(cpu->thread_id);
        ++active->observed_lock_acquisitions;
    }
    // The provider has returned normally, so all of its scoped locks are gone.
    // Cancellation never revokes a successful create or its retained pin.
    check_boundary();
    return mutex_cell_result(result);
}
static void dispatch_mutex(StartupExecution *execution, uint32_t nid)
{
    ppu_context *cpu = &execution->cpu;
    const uint64_t toc = cpu->gpr[2], slot = cpu->gpr[1] + 0x28;
    if (slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory);
    checked_address(slot, 8, true);
    const uint32_t cell = invoke_mutex(execution, nid);
    check_boundary();
    const uint64_t stored_toc = __builtin_bswap64(toc);
    const ps3_guest_memory *memory = execution->services->memory;
    ps3_guest_memory_status written = memory->write(memory->context, uint32_t(slot), &stored_toc, sizeof(stored_toc));
    if (written != PS3_GUEST_MEMORY_OK) {
        active->mutex_failure = {PS3_LWMUTEX_MEMORY_FAILURE, written, PS3_WAIT_READY};
        stop(StartupStop::MutexProviderFailure);
    }
    active->accesses[active->access_count++] = {slot, toc, 8, true};
    cpu->gpr[2] = toc;
    cpu->gpr[3] = uint64_t(int64_t(int32_t(cell)));
    ++active->mutex_returns;
}
#endif
void ps3_hle_call(unsigned int nid, ppu_context *cpu)
{
    check_boundary();
    if (cpu != active->cpu) stop(StartupStop::MissingServices);
    auto *execution = reinterpret_cast<StartupExecution *>(cpu);
    if (execution->control != active) stop(StartupStop::MissingServices);
    if (active->import_count >= STARTUP_IMPORT_CAPACITY) stop(StartupStop::TraceCapacity);
    active->imports[active->import_count++] = nid;
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
    if (is_mutex_import(nid)) { dispatch_mutex(execution, nid); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_CLOCK
    if (nid == 0x8461e528) {
        if (!execution->services || !execution->services->clock ||
            !execution->services->clock->now_microseconds) stop(StartupStop::MissingServices);
        const uint64_t toc = cpu->gpr[2], slot = cpu->gpr[1] + 0x28;
        if (slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory);
        checked_address(slot, 8, true);
        uint64_t sample = 0;
        const ps3_clock *clock = execution->services->clock;
        const ps3_clock_status status = clock->now_microseconds(clock->context, &sample);
        active->clock_status = status;
        ++active->clock_calls;
        if (status == PS3_CLOCK_OK) active->clock_sample = sample;
        if (status != PS3_CLOCK_OK) stop(StartupStop::ClockProviderFailure);
        check_boundary();
        write_guest(slot, toc, 8);
        cpu->gpr[2] = toc;
        cpu->gpr[3] = sample;
        ++active->clock_returns;
        return;
    }
#endif
    if (nid != 0x744680a2) stop(StartupStop::UnknownImport);
    if (!execution->services || !execution->services->initialize_tls || active->tls_returns) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu->gpr[2];
    const uint64_t toc_slot = cpu->gpr[1] + 0x28;
    if (toc_slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory);
    checked_address(toc_slot, 8, true);
    active->before_tls = *cpu;
    Ps3TlsTemplate tls = {uint32_t(cpu->gpr[4]), cpu->gpr[5], cpu->gpr[6]};
    Ps3TlsImage image = {};
    // Provider resources unwind normally before any stop/cancellation path below.
    Ps3TlsReport report = execution->services->initialize_tls(execution->services->context, &tls, &image);
    if (report.status != PS3_TLS_OK) stop(StartupStop::ProviderFailure);
    check_boundary();
    if (!image.allocation_size || image.block % 4096 || image.allocation_size % 4096 ||
        image.allocation_size > UINT64_C(0x100000000) - image.block ||
        image.thread_pointer < image.block || image.thread_pointer - image.block >= image.allocation_size ||
        active->span_count >= STARTUP_SPAN_CAPACITY) stop(StartupStop::InvalidMemory);
    active->spans[active->span_count++] = {image.block, image.allocation_size, true};
    write_guest(toc_slot, toc, 8);
    cpu->gpr[2] = toc;
    cpu->gpr[13] = image.thread_pointer;
    cpu->gpr[3] = 0;
    ++active->tls_returns;
    active->after_tls = *cpu;
    // Genuine return through the ORIGINAL TLS import stub and startup call site.
}
[[noreturn]] void native_startup_unsupported(unsigned int operation)
{
    if (!active) std::abort();
    active->external_id = operation;
    stop(StartupStop::UnsupportedExternal);
}
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
void lv2_syscall(ppu_context *cpu)
{
    check_boundary();
    if (cpu != active->cpu) stop(StartupStop::MissingServices);
    active->syscall_number = uint32_t(cpu->gpr[11]);
#ifdef PS3_NATIVE_STARTUP_MEMORY
    auto *execution = reinterpret_cast<StartupExecution *>(cpu);
    if (execution->control != active || !execution->services) stop(StartupStop::MissingServices);
    const Ps3PpuExecutionServices *services = execution->services;
    if (!services->memory || !services->memory->read || !services->allocate_memory ||
        !services->free_memory || !services->query_memory) stop(StartupStop::MissingServices);
    const uint32_t number = active->syscall_number;
    if (cpu->gpr[11] != number) stop(StartupStop::UnsupportedSyscall);
    if (number != 348 && number != 349 && number != 352) stop(StartupStop::UnsupportedSyscall);
    const uint32_t output = uint32_t(number == 348 ? cpu->gpr[5] : cpu->gpr[3]);
    uint64_t alignment = 0;
    uint32_t cell = 0;
    bool invoke = true;
    if (number == 348) {
        const uint64_t flags = cpu->gpr[4];
        if (flags == 0 || flags == 0x400) alignment = PS3_USER_MEMORY_ALIGN_1M;
        else if (flags == 0x200) alignment = PS3_USER_MEMORY_ALIGN_64K;
        else { cell = 0x80010002; invoke = false; }
        if (invoke && (!cpu->gpr[3] || cpu->gpr[3] % alignment)) {
            cell = 0x80010010;
            invoke = false;
        }
    }
    if (invoke && number != 349) {
        const uint64_t bytes = number == 348 ? 4 : 8;
        if (!owner_writable(execution->control, output, bytes)) {
            cell = 0x8001000d;
            invoke = false;
        } else {
            unsigned char probe[8];
            const ps3_guest_memory_status checked = services->memory->read(
                services->memory->context, output, probe, size_t(bytes));
            if (checked != PS3_GUEST_MEMORY_OK) {
                active->memory_result = {};
                active->memory_result.status = PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE;
                active->memory_result.memory_status = checked;
                if (caller_pointer_failure(checked)) { cell = 0x8001000d; invoke = false; }
                else stop(StartupStop::MemoryProviderFailure);
            }
        }
    }
    if (invoke) {
        ps3_user_memory_service_result result{};
        if (number == 348) result = services->allocate_memory(services->context, cpu->gpr[3], alignment, output);
        else if (number == 349) result = services->free_memory(services->context, uint32_t(cpu->gpr[3]));
        else result = services->query_memory(services->context, output);
        // Save ownership evidence BEFORE cancellation or error handling. A
        // successful publication remains owned even if CPU return is suppressed.
        active->memory_result = result;
        check_boundary();
        if (result.status == PS3_USER_MEMORY_SERVICE_OK) cell = 0;
        else if (result.status == PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE &&
                 (result.allocator.status == PS3_USER_MEMORY_QUOTA_EXCEEDED ||
                  result.allocator.status == PS3_USER_MEMORY_NO_SPACE)) cell = 0x80010004;
        else if (result.status == PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE && number == 349 &&
                 result.allocator.status == PS3_USER_MEMORY_NOT_OWNED) cell = 0x80010002;
        else if (result.status == PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE && !result.published &&
                 !result.retained_allocation && !result.retained_pin && caller_pointer_failure(result.memory_status)) cell = 0x8001000d;
        else {
            std::printf("STARTUP_MEMORY_CAPABILITY_STOP syscall=%u service=%d allocator=%d memory=%d allocator_memory=%d retained_address=0x%x retained_pin=%d\n",
                number, result.status, result.allocator.status, result.memory_status, result.allocator.memory_status,
                result.retained_address, result.retained_pin);
            stop(StartupStop::MemoryProviderFailure);
        }
    }
    check_boundary();
    cpu->gpr[3] = uint64_t(int64_t(int32_t(cell)));
    ++active->memory_returns;
    // lv2 ABI: no HLE TOC-slot write, CR/r2/LR remain untouched.
    return;
#else
    stop(StartupStop::UnsupportedSyscall);
#endif
}
#endif
uint64_t ppu_timebase_now(void);
void native_startup_worker(void *argument)
{
    auto *execution = static_cast<StartupExecution *>(argument);
    active = execution->control;
    if (!active || active->base != reinterpret_cast<uintptr_t>(vm_base)) std::abort();
    active->cpu = &execution->cpu;
    g_trampoline_fn = nullptr;
    if (active->entry == StartupEntry::TlsStub) func_0055DA64(&execution->cpu);
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
    else if (active->entry == StartupEntry::MutexCreateStub) func_0055D9E4(&execution->cpu);
    else if (active->entry == StartupEntry::MutexLockStub) func_0055D924(&execution->cpu);
    else if (active->entry == StartupEntry::MutexTryStub) func_0055DAE4(&execution->cpu);
    else if (active->entry == StartupEntry::MutexUnlockStub) func_0055D944(&execution->cpu);
    else if (active->entry == StartupEntry::MutexDestroyStub) func_0055DB24(&execution->cpu);
#ifdef PS3_NATIVE_STARTUP_MEMORY
    else if (active->entry == StartupEntry::MemorySyscall) lv2_syscall(&execution->cpu);
#ifdef PS3_NATIVE_STARTUP_CLOCK
    else if (active->entry == StartupEntry::ClockStub) func_0055DA84(&execution->cpu);
#endif
#endif
#endif
    else if (active->entry == StartupEntry::UnsupportedExternal) (void)ppu_timebase_now();
    else func_00010230(&execution->cpu);
    stop(StartupStop::Returned);
}
}
