#ifndef NATIVE_PPU_MEMORY_CONTEXT_H
#define NATIVE_PPU_MEMORY_CONTEXT_H
#include "discovery_budget_binding.h"
#include "ppu_recomp.h"
#include "execution_services.h"
#include "native_memory_abi.h"
#include "atomic_domain.h"
#include "ppu_memory_trace.h"
#include "ppu_inline_layout.h"
#include <atomic>
#include <cstddef>
#include <type_traits>

enum class PpuMemoryStop { Cancelled, InvalidRange, UnsupportedRegion, PermissionDenied, InvalidBinding, ProviderFailure, TraceCapacity, BudgetStop };
struct PpuMemoryFailure {
    PpuMemoryStop reason;
    ps3_guest_memory_status status;
    uint32_t address;
    size_t length;
    bool write;
};
struct PpuMemoryStopPort {
    void *context;
    bool (*cancelled)(void *);
    // Must not return. The adapter calls this only after every domain guard is released.
    void (*stop)(void *, PpuMemoryFailure);
};
struct PpuMemoryRamRange { uint32_t address; uint64_t length; bool writable = false; };

// Decision 33 (LLE libsre): lifted firmware code reaches the game through guest function pointers.
// Those nested calls carry the game's TOC, which the owner publishes here for the duration of one
// call and retracts immediately after. Members are unconditional so the layout never depends on
// PS3_NATIVE_LLE_SRE; nothing reads them in a build without it.
struct PpuMemoryGuestCallback {
    // Called on every foreign entry into game code. Returns false when the owner is not in a state
    // that has a TOC to lend; `toc` then must not be read.
    bool (*entered)(void *context, uint64_t *toc);
    void *context;
};

// Composition initializes the domain first and keeps these RAM declarations and all
// dependencies stable through release. Ranges must be disjoint, and every transfer
// must fit in one range. A mapped special region is still unsupported.
class PpuMemoryBinding {
public:
    PpuMemoryBinding(ps3_atomic_domain &domain,const PpuMemoryRamRange *ram,size_t count)
        : domain(&domain), coherent_memory(ps3_atomic_domain_memory(&domain)), ram(ram), ram_count(count) {}
    PpuMemoryBinding(const PpuMemoryBinding &) = delete;
    PpuMemoryBinding &operator=(const PpuMemoryBinding &) = delete;
    PpuMemoryBinding(PpuMemoryBinding &&) = delete;
    PpuMemoryBinding &operator=(PpuMemoryBinding &&) = delete;
    ps3_atomic_domain *const domain;
    const ps3_guest_memory coherent_memory;
    const PpuMemoryRamRange *const ram;
    const size_t ram_count;
};

// Only this owner is accepted by the new generated-memory variant. Legacy standalone
// CPUs and StartupExecution objects are not interchangeable with this type.
struct PpuMemoryExecution {
    ppu_context cpu{};
    const Ps3PpuExecutionServices *services = nullptr;
    PpuMemoryBinding *memory = nullptr;
    PpuMemoryStopPort boundary{};
    ps3_atomic_owner reservation{};
    bool admitted = false;
    PpuMemoryTraceJournal *trace = nullptr;
    // Select exactly one observer before admission. Recorder and storage are borrowed
    // through join/readback; no reconfiguration or reentry during admitted execution.
    PpuMemoryTraceRecorder *recorder = nullptr;
    DiscoveryBudgetBinding *budget = nullptr;
    // Select before admission, exactly like the observer above; null means "no foreign caller".
    PpuMemoryGuestCallback *guest_callback = nullptr;
    // ppu-speed-20261113 item 2: access counter of the inline loads of the generated game code (native/adapters/ppu_inline_memory.h).
    // LAST member: its byte offset is a constant of the generated objects (PPU_INLINE_TICK_OFFSET, asserted below).
    uint32_t inline_tick = 0;
    PpuMemoryExecution() = default;
    PpuMemoryExecution(const PpuMemoryExecution &) = delete;
    PpuMemoryExecution &operator=(const PpuMemoryExecution &) = delete;
    PpuMemoryExecution(PpuMemoryExecution &&) = delete;
    PpuMemoryExecution &operator=(PpuMemoryExecution &&) = delete;
};
static_assert(std::is_standard_layout<PpuMemoryExecution>::value &&
    offsetof(PpuMemoryExecution,cpu)==0,"Generated CPU must be pointer-interconvertible with its owner");
static_assert(sizeof(ppu_context)==1104 && offsetof(ppu_context,cr)==1024 &&
    offsetof(ppu_context,lr)==1032,"Original generated CPU ABI must remain unchanged");
static_assert(offsetof(PpuMemoryExecution,inline_tick)==PPU_INLINE_TICK_OFFSET,
    "The inline loads compiled into the generated objects address the access counter at this offset of every context");
static_assert(!std::is_copy_constructible<PpuMemoryExecution>::value &&
    !std::is_move_constructible<PpuMemoryExecution>::value &&
    !std::is_copy_constructible<PpuMemoryBinding>::value &&
    !std::is_move_constructible<PpuMemoryBinding>::value,"Live ownership cannot be copied or moved");
enum class PpuMemoryAdmission { Ready, AlreadyAdmitted, InvalidBinding, OwnerUnavailable };
bool ppu_memory_binding_valid(const PpuMemoryExecution &);
PpuMemoryAdmission ppu_memory_admit(PpuMemoryExecution &,PpuMemoryBinding &,
    const Ps3PpuExecutionServices &,PpuMemoryStopPort);
// Decision 33: publishes the TOC of the guest owner this CPU belongs to when a foreign caller
// (lifted firmware) is about to invoke game code through a guest function pointer. False when the
// composition selected no owner or none is currently entered; `toc` is untouched then.
bool ppu_memory_guest_callback_toc(ppu_context *, uint64_t *toc);
// Caller must join every worker before release, and retain all binding dependencies
// until this succeeds. There is deliberately no destructor that releases a live owner.
ps3_guest_memory_status ppu_memory_release_after_join(PpuMemoryExecution &);
// Root diagnostic (2026-11-12): optional write watch for generated-code writes (see ppu_memory_context.cpp).
// Off while g_ppu_write_watch_size is zero. Set base and size before any guest thread runs; read the table after
// they are joined.
// Root (2026-11-12, Decision 60): the lock-free path of generated-code memory access (builds with a flat guest
// space only). g_ppu_fast_memory turns it off at run time; g_ppu_fast_write_conflicts counts stores that had to be
// repeated through the atomic domain.
extern bool g_ppu_fast_memory;
extern bool g_ppu_lean_memory;   // Decision 111: set by the composition after the first flip
// ppu-speed-20261113 item 2: inline loads in the generated game code. ppu_inline_memory_set(true) (the composition calls it right
// after it sets g_ppu_lean_memory) publishes the flat window's commit bitmap and base to those loads; (false) withdraws them.
// ppu_native_readNN_slow / ppu_native_readNN_fast are the routes the generated objects take when built with native/adapters/
// ppu_inline_memory.h (PPU_INLINE_LOADS=1 / 2); see that header and ppu_memory_context.cpp. All C symbols: the objects reference them.
extern "C" {
extern const ppu_inline_window *g_ppu_inline_window;
void ppu_inline_memory_set(bool on);
uint8_t ppu_native_read8_slow(ppu_context *cpu, uint64_t ea);
uint16_t ppu_native_read16_slow(ppu_context *cpu, uint64_t ea);
uint32_t ppu_native_read32_slow(ppu_context *cpu, uint64_t ea);
uint64_t ppu_native_read64_slow(ppu_context *cpu, uint64_t ea);
void ppu_native_read_bytes_slow(ppu_context *cpu, uint64_t ea, void *destination, size_t length);
uint8_t ppu_native_read8_fast(ppu_context *cpu, uint64_t ea);
uint16_t ppu_native_read16_fast(ppu_context *cpu, uint64_t ea);
uint32_t ppu_native_read32_fast(ppu_context *cpu, uint64_t ea);
uint64_t ppu_native_read64_fast(ppu_context *cpu, uint64_t ea);
void ppu_native_read_bytes16_fast(ppu_context *cpu, uint64_t ea, void *destination);
void ppu_native_read_bytes4_fast(ppu_context *cpu, uint64_t ea, void *destination);
}
extern std::atomic<uint64_t> g_ppu_fast_write_conflicts;
extern std::atomic<uint64_t> g_ppu_fast_write_conflicts_locked;
extern std::atomic<uint64_t> g_ppu_fast_write_sticky, g_ppu_fast_write_late;
enum { PPU_WRITE_WATCH_ENTRIES = 96 };
struct PpuWriteWatchEntry {
    uint32_t address, length, lr, r1, count;
    unsigned char bytes[16];        // last bytes written (guest order)
    unsigned char first_bytes[16];  // bytes of the first write
};
extern uint32_t g_ppu_write_watch_base, g_ppu_write_watch_size, g_ppu_write_watch_count, g_ppu_write_watch_dropped;
extern PpuWriteWatchEntry g_ppu_write_watch[PPU_WRITE_WATCH_ENTRIES];
#endif
