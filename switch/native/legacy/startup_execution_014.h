#ifndef NATIVE_LEGACY_STARTUP_EXECUTION_014_H
#define NATIVE_LEGACY_STARTUP_EXECUTION_014_H
#ifdef PS3_NATIVE_CONTEXT_MEMORY
#error "Legacy startup owner cannot be used by the context-memory variant"
#endif
#include "ppu_recomp.h"
#include "execution_services.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

enum class StartupStop { None, Returned, UnknownImport, UnsupportedExternal,
    InvalidMemory, Cancelled, Deadline, TraceCapacity, MissingServices, ProviderFailure, UnsupportedSyscall, MutexProviderFailure, MemoryProviderFailure, ClockProviderFailure };
enum class StartupEntry { Game, TlsStub, UnsupportedExternal, MutexCreateStub, MutexLockStub, MutexTryStub, MutexUnlockStub, MutexDestroyStub, MemorySyscall, ClockStub };
enum { STARTUP_SPAN_CAPACITY = 12, STARTUP_TRACE_CAPACITY = 2048, STARTUP_IMPORT_CAPACITY = 32 };
struct StartupSpan { uint32_t address; uint64_t length; bool writable; };
struct StartupAccess { uint64_t address, value; unsigned bytes; bool write; };
struct StartupControl {
    std::atomic<bool> cancelled{false};
    uint64_t deadline_tick = 0;
    uintptr_t base = 0;
    StartupSpan spans[STARTUP_SPAN_CAPACITY]{};
    size_t span_count = 0;
    StartupAccess accesses[STARTUP_TRACE_CAPACITY]{};
    size_t access_count = 0;
    uint32_t imports[STARTUP_IMPORT_CAPACITY]{};
    size_t import_count = 0;
    size_t tls_returns = 0;
    size_t mutex_returns = 0;
    uint32_t syscall_number = 0;
    size_t memory_returns = 0;
    uint64_t clock_sample = 0;
    ps3_clock_status clock_status = PS3_CLOCK_ARGUMENT;
    size_t clock_calls = 0, clock_returns = 0;
    ps3_user_memory_service_result memory_result{};
    uint32_t mutex_nid = 0;
    uint32_t observed_lock_address = 0, observed_lock_owner = 0;
    size_t observed_lock_acquisitions = 0;
    ps3_lwmutex_result mutex_failure{};
    StartupStop stop = StartupStop::None;
    uint32_t external_id = 0;
    uint64_t invalid_address = 0;
    ppu_context before_tls{}, after_tls{}, terminal{};
    ppu_context *cpu = nullptr;
    StartupEntry entry = StartupEntry::Game;
};
/* NEW VARIANT ABI PRECONDITION: every generated entry, thread and indirect
 * path uses the CPU subobject of this owner. Never pass legacy standalone
 * ppu_context objects to this variant's HLE bridge. Selected original source
 * is inspected at build time; generated CPU layout remains unchanged.
 * This VM bridge is scoped to the verified PRE-ATOMIC prefix. Atomic/vector
 * external operations fail-stop. Later generated direct vm_base writers and
 * LL/SC require an explicit coherence adaptation; raw spans do not provide it. */
struct StartupExecution {
    ppu_context cpu;
    const Ps3PpuExecutionServices *services;
    StartupControl *control;
    int32_t guest_priority = 0;
    bool has_guest_priority = false;
};
static_assert(std::is_standard_layout<ppu_context>::value, "CPU must have standard layout");
static_assert(std::is_standard_layout<StartupExecution>::value && offsetof(StartupExecution, cpu) == 0,
              "CPU must be pointer-interconvertible with its explicit execution owner");
static_assert(sizeof(ppu_context) == 1104 && offsetof(ppu_context, cr) == 1024 &&
              offsetof(ppu_context, lr) == 1032, "Original generated CPU ABI must remain unchanged");
static_assert(std::atomic<bool>::is_always_lock_free, "Cancellation must not hide a service lock");

bool native_startup_bind_memory(uintptr_t base);
void native_startup_unbind_memory(uintptr_t base);
extern "C" void native_startup_worker(void *execution);
#endif
