/* Decision 33, phase B1a: the lifted libsre module as a native guest module.
 *
 * The unit holds no firmware-derived data: the image and the export table come from the per-build
 * generated unit (tools/lle/gen_lle_sre_glue.py) through `libsre_module_description`. It always
 * compiles: the FLAG decides whether ps3_hle_call routes into it (startup_execution.cpp), never
 * whether the module exists. That is what lets one test binary measure both routes over the same
 * image, and what keeps the flag-off build free of any route. */
#include "lle_sre.h"
#include "guest_space.h"
#include "atomic_domain.h"
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kPageAlign = 0x10000;
constexpr uint32_t kOpdSize = 8;
constexpr size_t kMaxCallbackDepth = 8;
constexpr uint32_t kInstructionAlign = 4;

bool g_admitted = false;
// The PPU thread now inside Sony's code. Thread-local because two PPU threads may be routed at once,
// and it is set by lle_sre_call_export, which is the only place that knows the cpu being entered.
PPU_THREAD_LOCAL ppu_context *g_routed_cpu = nullptr;
uint64_t g_callback_toc[kMaxCallbackDepth];
size_t g_callback_depth = 0;

uint64_t read_u32(const unsigned char *bytes) {
    return (uint64_t(bytes[0]) << 24) | (uint64_t(bytes[1]) << 16) |
        (uint64_t(bytes[2]) << 8) | uint64_t(bytes[3]);
}

const void *compiled_code_context() { return &g_admitted; }

ps3_compiled_code_status find_lifted(const void *context, uint32_t address,
    ps3_compiled_function *target)
{
    if (!context || context != compiled_code_context()) return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    if (!target) return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    if (address == 0 || (address & (kInstructionAlign - 1)) != 0)
        return PS3_COMPILED_CODE_INVALID_ADDRESS;
    if (!g_admitted) return PS3_COMPILED_CODE_NOT_ADMITTED;
    const LibsreModuleDescription &module = libsre_module_description;
    if (address < module.base || uint64_t(address) - module.base >= module.image_size)
        return PS3_COMPILED_CODE_NOT_FOUND;
    for (uint64_t index = 0; index < libsre_function_table_count; ++index) {
        if (libsre_function_table[index].addr == address) {
            *target = libsre_function_table[index].func;
            return PS3_COMPILED_CODE_OK;
        }
    }
    return PS3_COMPILED_CODE_NOT_FOUND;
}

bool overlaps(uint64_t left_address, uint64_t left_length, uint64_t right_address,
    uint64_t right_length)
{
    return left_address < right_address + right_length && right_address < left_address + left_length;
}

}  // namespace

bool lle_sre_image_mapping(const LleSreImageMapping *declared, size_t declared_count,
    LleSreImageMapping *mapping)
{
    if (!mapping) return false;
    const LibsreModuleDescription &module = libsre_module_description;
    const LleSreImageMapping candidate{module.base, module.mapped_extent, true};
    for (size_t index = 0; index < declared_count; ++index) {
        if (!declared[index].length) return false;
        if (overlaps(candidate.base, candidate.length, declared[index].base, declared[index].length))
            return false;
    }
    *mapping = candidate;
    return true;
}

LleSreMappingStatus lle_sre_mapping_check()
{
    const LibsreModuleDescription &module = libsre_module_description;
    if (!module.image || !module.exports || !module.export_count || !module.image_size ||
        !module.mapped_extent || (module.base & (kPageAlign - 1)) != 0 ||
        (module.mapped_extent & (kPageAlign - 1)) != 0 ||
        module.image_size > module.mapped_extent)
        return LleSreMappingStatus::ExtentTooSmall;
    if (uint64_t(module.base) + module.mapped_extent > UINT64_C(0x100000000))
        return LleSreMappingStatus::ExtentTooSmall;
    if (module.toc < module.base || uint64_t(module.toc) - module.base + 4 > module.mapped_extent)
        return LleSreMappingStatus::ExtentTooSmall;
    const uint64_t base = module.base;
    for (size_t index = 0; index < module.export_count; ++index) {
        const LibsreExportEntry &entry = module.exports[index];
        if (index && entry.nid <= module.exports[index - 1].nid) return LleSreMappingStatus::EmptyExports;
        if (entry.opd < base || uint64_t(entry.opd) - base + kOpdSize > module.image_size)
            return LleSreMappingStatus::ImageTooLarge;
        if ((entry.opd & 3) != 0) return LleSreMappingStatus::ImageTooLarge;
    }
    return LleSreMappingStatus::Mapped;
}

LleSreMappingStatus lle_sre_mapping_admit()
{
    if (g_admitted) return LleSreMappingStatus::Mapped;
    const LleSreMappingStatus checked = lle_sre_mapping_check();
    if (checked != LleSreMappingStatus::Mapped) return checked;
    g_admitted = true;
    return LleSreMappingStatus::Mapped;
}

bool lle_sre_mapping_admitted() { return g_admitted; }

namespace { void opd_cache_reset(); }   // ppu-speed-20261113: the descriptor cache below, emptied whenever the image is (re)mapped
LleSreMappingStatus lle_sre_map_image(const ps3_guest_memory *memory)
{
    if (!memory || !memory->zero || !memory->write) return LleSreMappingStatus::ExtentTooSmall;
    if (!g_admitted) return LleSreMappingStatus::EmptyExports;
    const LibsreModuleDescription &module = libsre_module_description;
    if (memory->zero(memory->context, module.base, module.mapped_extent) != PS3_GUEST_MEMORY_OK)
        return LleSreMappingStatus::ExtentTooSmall;
    if (memory->write(memory->context, module.base, module.image, module.image_size) != PS3_GUEST_MEMORY_OK)
        return LleSreMappingStatus::ImageTooLarge;
    opd_cache_reset();
    return LleSreMappingStatus::Mapped;
}

uint32_t lle_sre_base() { return libsre_module_description.base; }
uint32_t lle_sre_toc() { return libsre_module_description.toc; }
uint32_t lle_sre_mapped_extent() { return libsre_module_description.mapped_extent; }

bool lle_sre_export_opd(uint32_t nid, uint32_t *opd)
{
    const LibsreModuleDescription &module = libsre_module_description;
    size_t first = 0, limit = module.export_count;
    while (first < limit) {
        const size_t middle = first + (limit - first) / 2;
        if (module.exports[middle].nid < nid) first = middle + 1;
        else if (module.exports[middle].nid > nid) limit = middle;
        else {
            if (opd) *opd = module.exports[middle].opd;
            return true;
        }
    }
    return false;
}

bool lle_sre_exports_nid(uint32_t nid) { return lle_sre_export_opd(nid, nullptr); }

namespace {
/* ppu-speed-20261113 (task 3): the descriptor (code, toc) of an exported function lies in the mapped libsre image and never changes after
 * lle_sre_map_image, yet every call of Sony's code read it through the atomic domain: one guard acquisition per libsre call (run rv56s: 3.4 % of the
 * guest main thread asleep waiting for that guard inside this very read). Cache it, once per descriptor, in a small lock-free table: a slot is
 * claimed 0 -> 1 with a compare-and-swap, filled, then published with state 2 (release); readers take only published slots. A full table or a
 * claimed-but-unpublished slot just means the old read. lle_sre_map_image resets it (single-threaded setup). -DPS3_LLE_NO_OPD_CACHE reads every time. */
struct OpdSlot { uint32_t state, opd, code, toc; };
constexpr unsigned kOpdSlots = 1024, kOpdProbes = 8;
OpdSlot g_opd_cache[kOpdSlots];
OpdSlot *opd_probe(uint32_t opd, unsigned n) { return &g_opd_cache[((opd >> 3) * 2654435761u >> 22 ^ n) & (kOpdSlots - 1)]; }
void opd_cache_reset() { for (OpdSlot &slot : g_opd_cache) __atomic_store_n(&slot.state, 0u, __ATOMIC_RELEASE); }
bool opd_cached(uint32_t opd, uint32_t *code, uint32_t *toc)
{
#ifndef PS3_LLE_NO_OPD_CACHE
    for (unsigned n = 0; n < kOpdProbes; ++n) {
        OpdSlot *slot = opd_probe(opd, n);
        const uint32_t state = __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE);
        if (state == 0) return false;
        if (state == 2 && slot->opd == opd) { *code = slot->code; *toc = slot->toc; return true; }
    }
#else
    (void)opd; (void)code; (void)toc;
#endif
    return false;
}
void opd_remember(uint32_t opd, uint32_t code, uint32_t toc)
{
#ifndef PS3_LLE_NO_OPD_CACHE
    for (unsigned n = 0; n < kOpdProbes; ++n) {
        OpdSlot *slot = opd_probe(opd, n);
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&slot->state, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            slot->opd = opd; slot->code = code; slot->toc = toc;
            __atomic_store_n(&slot->state, 2u, __ATOMIC_RELEASE);
            return;
        }
        if (expected == 2 && slot->opd == opd) return;
    }
#else
    (void)opd; (void)code; (void)toc;
#endif
}
}  // namespace

LleSreRouteStatus lle_sre_call_export(uint32_t nid, ppu_context *cpu, const ps3_guest_memory *memory)
{
    uint32_t opd = 0;
    if (!lle_sre_export_opd(nid, &opd)) return LleSreRouteStatus::NotExported;
    if (!g_admitted || !cpu || !memory || !memory->read) return LleSreRouteStatus::Unavailable;
    uint32_t code = 0, toc = 0;
    if (!opd_cached(opd, &code, &toc)) {
        unsigned char descriptor[kOpdSize];
        if (memory->read(memory->context, opd, descriptor, sizeof(descriptor)) != PS3_GUEST_MEMORY_OK)
            return LleSreRouteStatus::Unavailable;
        code = uint32_t(read_u32(descriptor));
        toc = uint32_t(read_u32(descriptor + 4));
        opd_remember(opd, code, toc);
    }
    ps3_compiled_function target = nullptr;
    if (find_lifted(compiled_code_context(), code, &target) != PS3_COMPILED_CODE_OK)
        return LleSreRouteStatus::Unavailable;
    const uint64_t caller_toc = cpu->gpr[2];
    const uint64_t caller_stack = cpu->gpr[1];
    /* Restores the caller's r2/r1 and the frame bookkeeping on the NORMAL return path. A terminal
     * stop inside Sony's code does NOT unwind: stop() ends in threadExit(), which leaves this frame
     * without running this destructor, so on a stop r2 and the depth are whatever the terminal
     * record already captured -- deliberately, since that is the evidence of where it stopped. */
    struct FrameGuard {
        ppu_context *cpu;
        uint64_t toc;
        uint64_t stack;
        size_t depth;
        ~FrameGuard()
        {
            cpu->gpr[2] = toc;
            cpu->gpr[1] = stack;
            g_callback_depth = depth;
        }
    } guard{cpu, caller_toc, caller_stack, g_callback_depth};
    if (g_callback_depth < kMaxCallbackDepth)
        g_callback_toc[g_callback_depth++] = caller_toc;
    cpu->gpr[2] = toc;
    const ppu_context *const previous_cpu = g_routed_cpu;
    g_routed_cpu = cpu;
    target(cpu);
    g_routed_cpu = const_cast<ppu_context *>(previous_cpu);
    /* No r2 check here: Sony's own bodies restore r2 from their caller's frame slot themselves, and a
     * body that returns to its PLT stub legitimately still holds the stub's TOC. The caller's r2 is
     * the guard's job and is restored unconditionally; the dispatcher asserts it after the route. */
    return LleSreRouteStatus::Routed;
}

ps3_compiled_code_lookup lle_sre_compiled_code_lookup()
{
    return {compiled_code_context(), find_lifted};
}

namespace {
// One chained lookup over the game's table first, then the lifted module. Composition owns the
// PpuCompiledCodeTable; they are stored here because ps3_compiled_code_lookup carries only a context
// pointer. Single-threaded setup, then immutable for the life of the process.
ps3_compiled_code_lookup g_game_lookup{};
const void *const kChainContext = &g_game_lookup;

ps3_compiled_code_status find_chained(const void *context, uint32_t address,
    ps3_compiled_function *target)
{
    if (!context || context != kChainContext || !target) return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    // The game's answer is authoritative when it has one: libsre is a second table, never a shadow.
    if (g_game_lookup.find) {
        const ps3_compiled_code_status status = g_game_lookup.find(g_game_lookup.context, address, target);
        if (status != PS3_COMPILED_CODE_NOT_FOUND) return status;
    }
    return find_lifted(compiled_code_context(), address, target);
}
}  // namespace

ps3_compiled_code_lookup lle_sre_chained_compiled_code_lookup(ps3_compiled_code_lookup game)
{
    g_game_lookup = game;
    return {const_cast<void *>(kChainContext), find_chained};
}

bool lle_sre_callback_toc(uint64_t *toc)
{
    if (!g_callback_depth) return false;
    if (toc) *toc = g_callback_toc[g_callback_depth - 1];
    return true;
}


/* The lifted unit's accessors. Its own ppu_recomp.h declares these by EA and takes no CPU argument
 * (the PC runtime's global VM), while everything here reaches guest memory through the owner that
 * `cpu` points at. They are C symbols on purpose: the lifted unit is C++ and calls them from
 * `extern "C"`. Each forwards to the native accessor for the SAME cpu, so a lifted read is a native
 * read: same range check, same trace event, same stop on an unwritable or unmapped address. */
/* Root fix (B2b Eden crash): a PPU thread whose ENTRY is lifted libsre code (the SPURS handler thread,
 * entry 0x3000B504, created by sys_ppu_thread_create) runs Sony's code without ever passing through
 * lle_sre_call_export, so g_routed_cpu is null on that thread and every vm_* access aborted. Fall back
 * to the cpu the startup dispatcher owns on THIS thread (its thread_local `active` control). */
extern "C" ppu_context *startup_active_cpu(void);
extern "C" ppu_context *lle_sre_current_cpu(void) { return g_routed_cpu ? g_routed_cpu : startup_active_cpu(); }

namespace {
ppu_context *current_cpu()
{
    ppu_context *cpu = lle_sre_current_cpu();
    if (!cpu) std::abort();
    return cpu;
}
}  // namespace

extern "C" uint8_t vm_read8(uint64_t ea) { return ppu_native_read8(current_cpu(), ea); }
extern "C" uint16_t vm_read16(uint64_t ea) { return ppu_native_read16(current_cpu(), ea); }
/* Decision 112 (rv50s): a load of a line the SPUs also reserve notes the line's write sequence BEFORE it reads, so the stwcx below can tell that something wrote the
 * line since (a real reservation covers the whole 128-byte line; the value check alone does not). One relaxed load and a test for every other line. */
extern "C" uint32_t vm_read32(uint64_t ea)
{
    const uint32_t seq = ps3_atomic_reservation_snapshot(ea);
    const uint32_t value = ppu_native_read32(current_cpu(), ea);
    if (__builtin_expect(seq != PS3_ATOMIC_NO_SEQ, 0)) ps3_atomic_reservation_note(ea, seq);
    return value;
}
extern "C" uint64_t vm_read64(uint64_t ea)
{
    const uint32_t seq = ps3_atomic_reservation_snapshot(ea);
    const uint64_t value = ppu_native_read64(current_cpu(), ea);
    if (__builtin_expect(seq != PS3_ATOMIC_NO_SEQ, 0)) ps3_atomic_reservation_note(ea, seq);
    return value;
}
extern "C" void vm_write8(uint64_t ea, uint8_t value) { ppu_native_write8(current_cpu(), ea, value); }
extern "C" void vm_write16(uint64_t ea, uint16_t value) { ppu_native_write16(current_cpu(), ea, value); }
extern "C" void vm_write32(uint64_t ea, uint32_t value) { ppu_native_write32(current_cpu(), ea, value); }
extern "C" void vm_write64(uint64_t ea, uint64_t value) { ppu_native_write64(current_cpu(), ea, value); }
/* Root fix (C1b Eden): the lifted unit's ppu_recomp.h declares these as (ea, expected, value) -- store `value`
 * iff the guest word still equals `expected`, the value its inline lwarx/ldarx read. The lifted lwarx is a plain
 * vm_read plus ctx->reserve_*; it never takes a native reservation. The previous two-argument definitions here
 * therefore (a) received `expected` as the value to store and (b) asked the native domain to commit a reservation
 * that was never taken, so every lifted stwcx. failed and libsre's first real CAS loop (0x3000DE24, the SPURS
 * exception-handler latch) spun until the generated budget stopped the primary. Now: take the native reservation,
 * check the word still holds `expected`, then store conditionally -- the domain fails the store if any other PPU
 * or SPU committed to the word in between, which is the compare-and-swap the lifted code expects. */
/* Decision 112: after the reservation is taken (any later write to the line clears it, in the domain) the line must also be unwritten since the lwarx: ps3_atomic_reservation_intact(). */
extern "C" void ppu_native_clear_reservation(ppu_context *cpu);
extern "C" int ppu_stwcx32(uint64_t ea, uint32_t expected, uint32_t value)
{
    ppu_context *cpu = current_cpu();
    if (ppu_native_load_reserve32(cpu, ea) != expected) { (void)ps3_atomic_reservation_intact(ea); return 0; }
    if (!ps3_atomic_reservation_intact(ea)) { ppu_native_clear_reservation(cpu); return 0; }
    return ppu_native_store_conditional32(cpu, ea, value) ? 1 : 0;
}
extern "C" int ppu_stdcx64(uint64_t ea, uint64_t expected, uint64_t value)
{
    ppu_context *cpu = current_cpu();
    if (ppu_native_load_reserve64(cpu, ea) != expected) { (void)ps3_atomic_reservation_intact(ea); return 0; }
    if (!ps3_atomic_reservation_intact(ea)) { ppu_native_clear_reservation(cpu); return 0; }
    return ppu_native_store_conditional64(cpu, ea, value) ? 1 : 0;
}

