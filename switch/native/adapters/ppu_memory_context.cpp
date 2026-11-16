#include <atomic>
#include "ppu_memory_context.h"
#include <cstdlib>
#include <cstring>

#ifdef PS3_NATIVE_FLAT_GUEST
#include "guest_space_flat_horizon.h"
#endif
bool g_ppu_fast_memory = true;
// Root (Decision 111): the lean path of generated-code memory access is off until the composition turns it on
// (after the first flip). Run rv45s had it on from the first instruction and never reached a picture: the start-up
// phases end guest code through the per-access cancel and budget checks that the lean path leaves out.
bool g_ppu_lean_memory = false;
std::atomic<uint64_t> g_ppu_fast_write_conflicts{0};
std::atomic<uint64_t> g_ppu_fast_write_conflicts_locked{0};
std::atomic<uint64_t> g_ppu_fast_write_sticky{0}, g_ppu_fast_write_late{0};   // Decision 87: guarded for an SPU-reserved line / caught by the second test
namespace {
inline bool ps3_atomic_write_conflicts_counted(const ps3_atomic_domain *domain, uint64_t address, uint64_t length)
{
    // Root (2026-11-13): the _fast test (atomic_domain.h gives the ordering argument): acquire loads of locked_ops and busy_map, plain loads of reserved_map. Only valid in the
    // fast_store / lean_store sequence: test, store, full fence, test again.
    if (!ps3_atomic_domain_write_conflicts_fast(domain, address, length)) return false;
    g_ppu_fast_write_conflicts.fetch_add(1, std::memory_order_relaxed);
    if (__atomic_load_n(&domain->locked_ops, __ATOMIC_RELAXED) != 0) g_ppu_fast_write_conflicts_locked.fetch_add(1, std::memory_order_relaxed);   // diagnostic split: an operation in flight, else a reserved granule
    return true;
}
}
uint32_t g_ppu_write_watch_base = 0, g_ppu_write_watch_size = 0, g_ppu_write_watch_count = 0, g_ppu_write_watch_dropped = 0;
PpuWriteWatchEntry g_ppu_write_watch[PPU_WRITE_WATCH_ENTRIES];
std::atomic_flag g_ppu_write_watch_lock = ATOMIC_FLAG_INIT;
namespace {
bool same_memory(const ps3_guest_memory *memory,const ps3_guest_memory &expected) {
    return memory && memory->context==expected.context && memory->commit==expected.commit &&
        memory->decommit==expected.decommit && memory->read==expected.read &&
        memory->write==expected.write && memory->zero==expected.zero;
}
bool valid_ranges(const PpuMemoryBinding &binding) {
    if(!binding.domain || !binding.ram || !binding.ram_count) return false;
    for(size_t i=0;i<binding.ram_count;++i) {
        const auto &range=binding.ram[i];
        if(!range.length || range.length>(uint64_t{1}<<32)-range.address) return false;
        for(size_t j=0;j<i;++j) {
            const auto &prior=binding.ram[j];
            if(uint64_t(range.address)<uint64_t(prior.address)+prior.length &&
                uint64_t(prior.address)<uint64_t(range.address)+range.length) return false;
        }
    }
    return true;
}
bool coherent(const PpuMemoryExecution &execution) {
    return execution.admitted && execution.memory && execution.services &&
        execution.reservation.domain==execution.memory->domain &&
        same_memory(execution.services->memory,execution.memory->coherent_memory) &&
        same_memory(&execution.memory->coherent_memory,ps3_atomic_domain_memory(execution.memory->domain));
}
bool observation_valid(const PpuMemoryExecution &execution)
{
    if (execution.trace && execution.recorder) return false;
    if (!execution.recorder) return true;
    const auto state = execution.recorder->report();
    return state.configured && !state.batch_active;
}
struct OwnedTraceBatch {
    PpuMemoryTraceRecorder *recorder = nullptr;
    OwnedTraceBatch() = default;
    OwnedTraceBatch(const OwnedTraceBatch &) = delete;
    OwnedTraceBatch &operator=(const OwnedTraceBatch &) = delete;
};
static_assert(std::is_trivially_destructible<OwnedTraceBatch>::value,
              "Trace credit cleanup must never depend on unwinding");
void finish_owned(OwnedTraceBatch &batch)
{
    if (batch.recorder) {
        batch.recorder->finish();
        batch.recorder = nullptr;
    }
}
[[noreturn]] void fail(PpuMemoryExecution &execution,PpuMemoryStop reason,
    ps3_guest_memory_status status,uint32_t address,size_t size,bool write) {
    if(execution.boundary.stop)
        execution.boundary.stop(execution.boundary.context,{reason,status,address,size,write});
    std::abort();
}
PpuMemoryExecution &owner(ppu_context *cpu) {
    if(!cpu) std::abort();
    return *reinterpret_cast<PpuMemoryExecution *>(cpu);
}
PpuMemoryExecution &enter(ppu_context *cpu,uint32_t address,size_t size,bool write) {
    auto &execution=owner(cpu);
    if(!coherent(execution) || !observation_valid(execution))
        fail(execution,PpuMemoryStop::InvalidBinding,PS3_GUEST_MEMORY_INVALID_RANGE,address,size,write);
    if(execution.boundary.cancelled && execution.boundary.cancelled(execution.boundary.context))
        fail(execution,PpuMemoryStop::Cancelled,PS3_GUEST_MEMORY_OK,address,size,write);
    if (execution.budget && !execution.budget->admit(infamous::discovery::Category::Generated).admitted)
        fail(execution,PpuMemoryStop::BudgetStop,PS3_GUEST_MEMORY_OK,address,size,write);
    return execution;
}
bool is_ram(const PpuMemoryExecution &execution,uint32_t address,size_t size,bool write) {
    for(size_t i=0;i<execution.memory->ram_count;++i) {
        const auto &range=execution.memory->ram[i];
        if(address>=range.address && uint64_t(address)-range.address+size<=range.length &&
            (!write || range.writable)) return true;
    }
    return false;
}
void check_status(PpuMemoryExecution &execution,ps3_guest_memory_status status,uint32_t address,size_t size,bool write) {
    if(status!=PS3_GUEST_MEMORY_OK)
        fail(execution,PpuMemoryStop::ProviderFailure,status,address,size,write);
}
void check_status(PpuMemoryExecution &execution, ps3_guest_memory_status status,
                  uint32_t address, size_t size, bool write, OwnedTraceBatch &batch)
{
    if (status != PS3_GUEST_MEMORY_OK) finish_owned(batch);
    check_status(execution, status, address, size, write);
}
void checked_range(PpuMemoryExecution &execution,uint32_t address,size_t size,bool write,bool attempted_sc=false) {
    PpuMemoryStop reason=PpuMemoryStop::UnsupportedRegion;
    bool invalid=size>(uint64_t{1}<<32)-address;
    if(!invalid && is_ram(execution,address,size,write)) return;
    if(!invalid && write && is_ram(execution,address,size,false)) reason=PpuMemoryStop::PermissionDenied;
    if(attempted_sc) check_status(execution,
        ps3_atomic_owner_clear_reservation(execution.memory->domain,execution.reservation),address,size,write);
    if(invalid) reason=PpuMemoryStop::InvalidRange;
    fail(execution,reason,PS3_GUEST_MEMORY_INVALID_RANGE,address,size,write);
}
void trace_capacity(PpuMemoryExecution &execution, uint32_t address, size_t size, bool write,
                    OwnedTraceBatch &batch, bool attempted_sc = false)
{
    if (execution.recorder) {
        if (execution.recorder->begin(1)) {
            batch.recorder = execution.recorder;
            return;
        }
    } else if (!execution.trace || ppu_memory_trace_available(*execution.trace)) return;
    if (attempted_sc) check_status(execution,
        ps3_atomic_owner_clear_reservation(execution.memory->domain, execution.reservation), address, size, write);
    fail(execution, PpuMemoryStop::TraceCapacity, PS3_GUEST_MEMORY_NO_MEMORY, address, size, write);
}
void trace_success(PpuMemoryExecution &execution, const PpuMemoryTraceEvent &event, OwnedTraceBatch &batch)
{
    if (batch.recorder) {
        batch.recorder->publish(event);
        finish_owned(batch);
        return;
    }
    if (execution.trace && !ppu_memory_trace_append(*execution.trace, event))
        fail(execution, PpuMemoryStop::TraceCapacity, PS3_GUEST_MEMORY_PLATFORM_FAILURE,
             event.address, event.length, event.operation != PpuMemoryTraceOperation::Read);
}
// Root diagnostic (2026-11-12): a write watch for generated code. When g_ppu_write_watch_size is not zero, every
// generated-code write that overlaps [base, base + size) is recorded with the writer's link register and stack
// pointer: one entry per (address, lr), last bytes and a count. Off by default; the composition root sets the
// range and prints the table. Service (HLE) writes do not pass here.
void watch_write(ppu_context *cpu, uint32_t address, const unsigned char *bytes, size_t size)
{
    while (g_ppu_write_watch_lock.test_and_set(std::memory_order_acquire)) {}
    const uint32_t lr = cpu ? static_cast<uint32_t>(cpu->lr) : 0;
    PpuWriteWatchEntry *entry = nullptr;
    for (uint32_t index = 0; index < g_ppu_write_watch_count; ++index)
        if (g_ppu_write_watch[index].address == address && g_ppu_write_watch[index].lr == lr) { entry = &g_ppu_write_watch[index]; break; }
    if (!entry && g_ppu_write_watch_count < PPU_WRITE_WATCH_ENTRIES) {
        entry = &g_ppu_write_watch[g_ppu_write_watch_count++];
        *entry = PpuWriteWatchEntry{};
        entry->address = address; entry->lr = lr;
    }
    if (entry) {
        entry->length = static_cast<uint32_t>(size);
        entry->r1 = cpu ? static_cast<uint32_t>(cpu->gpr[1]) : 0;
        std::memset(entry->bytes, 0, sizeof entry->bytes);
        std::memcpy(entry->bytes, bytes, size > 16 ? 16 : size);
        if (!entry->count) std::memcpy(entry->first_bytes, entry->bytes, 16);
        ++entry->count;
    } else ++g_ppu_write_watch_dropped;
    g_ppu_write_watch_lock.clear(std::memory_order_release);
}
inline void watch_check(ppu_context *cpu, uint32_t address, const unsigned char *bytes, size_t size)
{
    const uint32_t watch_size = g_ppu_write_watch_size;
    if (watch_size && address < g_ppu_write_watch_base + watch_size && address + size > g_ppu_write_watch_base)
        watch_write(cpu, address, bytes, size);
}
// Root (2026-11-12, Decision 60): the fast path of generated-code memory access, when the process has a flat
// guest space. A read is a direct load. A write is a direct store, then a full barrier, then the atomic domain's
// conflict test; on a conflict the store is repeated through the domain (ps3_atomic_domain_write_conflicts says
// why that is enough). The cancellation, budget and RAM-range checks stay; the per-access trace is not kept on
// this path. Off when g_ppu_fast_memory is false or the pages are not committed (then the old path reports it).
#ifdef PS3_NATIVE_FLAT_GUEST
inline unsigned char *fast_pointer(uint32_t address, size_t size)
{
    return g_ppu_fast_memory ? static_cast<unsigned char *>(ps3_flat_guest_fast_pointer(address, static_cast<uint32_t>(size))) : nullptr;
}
inline bool fast_store(PpuMemoryExecution &execution, uint32_t address, const unsigned char *bytes, size_t size)
{
    unsigned char *guest = fast_pointer(address, size);
    if (!guest) return false;
#if PS3_ATOMIC_TOUCHED_STORES
    if (!ps3_atomic_touched_hit_fast(address, size)) { std::memcpy(guest, bytes, size); return true; }   // Decision 120
#endif
    // Root (Decision 87): test BEFORE storing. A line that has ever held an SPU reservation, or that an atomic
    // operation is working on right now, gets one guarded store (which also drops the reservations) and no
    // unguarded one. Storing first and repeating later let an SPU store-conditional succeed on a stale line.
    if (ps3_atomic_sticky_hit_fast(address, size)) {
        g_ppu_fast_write_sticky.fetch_add(1, std::memory_order_relaxed);
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
        return true;
    }
    if (ps3_atomic_write_conflicts_counted(execution.memory->domain, address, size)) {
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
        return true;
    }
    std::memcpy(guest, bytes, size);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    // Second test: an operation that began between the first test and the store (it raises its count, with a
    // barrier, before it reads). Rare; the repeat keeps the old behaviour for that window.
    if (ps3_atomic_sticky_hit_fast(address, size) || ps3_atomic_write_conflicts_counted(execution.memory->domain, address, size)) {
        g_ppu_fast_write_late.fetch_add(1, std::memory_order_relaxed);
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
    }
    return true;
}
#else
inline unsigned char *fast_pointer(uint32_t, size_t) { return nullptr; }
inline bool fast_store(PpuMemoryExecution &, uint32_t, const unsigned char *, size_t) { return false; }
#endif
// Root (2026-11-13, Decision 110, hardware run rv43s): the game's main thread was 75-95 % in generated code and every
// load and store of that code came through read_scalar/write_scalar: the binding coherence test (about twenty
// compares), an indirect call for the cancel flag, the budget, a scan of the RAM ranges, a call for the flat pointer
// and a byte loop, per access. The lean path below serves an access to committed pages of the flat guest space with
// a bitmap test and one load (stores keep every step of fast_store: the reserved-line test, the conflict test, the
// barrier and the second test). What it leaves out for those accesses: the per-access cancel and budget checks (both
// are still made at every lv2 call and dispatch boundary), the per-access trace (already off on the fast path) and
// the RAM-range permission test (a store to a committed read-only range is no longer refused).
// -DPS3_PPU_LEAN_MEMORY=0 gives the old path back.
#ifndef PS3_PPU_LEAN_MEMORY
#define PS3_PPU_LEAN_MEMORY 1
#endif
#if defined(PS3_NATIVE_FLAT_GUEST) && PS3_PPU_LEAN_MEMORY
inline unsigned char *lean_pointer(uint32_t address, size_t size)
{
    static const unsigned char *bitmap;
    static uintptr_t base;
    if (__builtin_expect(!bitmap, 0)) {
        const unsigned char *found = ps3_flat_guest_committed_bitmap();
        const uintptr_t window = g_ps3_flat_guest_base;
        if (!found || !window) return nullptr;
        base = window;
        __atomic_store_n(&bitmap, found, __ATOMIC_RELEASE);
    }
    if (__builtin_expect(!g_ppu_fast_memory || !g_ppu_lean_memory, 0)) return nullptr;
    {   // every 256th access of a thread takes the full path, so a cancel still reaches a thread that makes no calls
        static thread_local uint32_t count;
        if (__builtin_expect((++count & 255u) == 0u, 0)) return nullptr;
    }
    const uint64_t end = uint64_t(address) + size;
    if (__builtin_expect(end > PS3_GUEST_ADDRESS_BYTES, 0)) return nullptr;
    const uint32_t first = address / PS3_GUEST_PAGE_BYTES, last = uint32_t((end - 1) / PS3_GUEST_PAGE_BYTES);
    if (__builtin_expect(!((bitmap[first >> 3] >> (first & 7u)) & 1u), 0)) return nullptr;
    if (__builtin_expect(last != first, 0) && !((bitmap[last >> 3] >> (last & 7u)) & 1u)) return nullptr;
    return reinterpret_cast<unsigned char *>(base + address);
}
// fast_store's steps on an already resolved pointer.
inline void lean_store(PpuMemoryExecution &execution, uint32_t address, unsigned char *guest, const unsigned char *bytes, size_t size)
{
#if PS3_ATOMIC_TOUCHED_STORES
    // Decision 120 (atomic_domain.h): a line no atomic operation has ever touched takes a plain store, no fence, no conflict test.
    if (__builtin_expect(!ps3_atomic_touched_hit_fast(address, size), 1)) { std::memcpy(guest, bytes, size); return; }
#endif
    if (ps3_atomic_sticky_hit_fast(address, size)) {
        g_ppu_fast_write_sticky.fetch_add(1, std::memory_order_relaxed);
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
        return;
    }
    if (ps3_atomic_write_conflicts_counted(execution.memory->domain, address, size)) {
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
        return;
    }
    std::memcpy(guest, bytes, size);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ps3_atomic_sticky_hit_fast(address, size) || ps3_atomic_write_conflicts_counted(execution.memory->domain, address, size)) {
        g_ppu_fast_write_late.fetch_add(1, std::memory_order_relaxed);
        check_status(execution, ps3_atomic_write(execution.memory->domain, address, bytes, size), address, size, true);
    }
}
#define PPU_LEAN 1
#else
#define PPU_LEAN 0
#endif
uint64_t read_scalar(ppu_context *cpu,uint64_t ea,size_t size) {
    uint32_t address=static_cast<uint32_t>(ea);
#if PPU_LEAN
    if (const unsigned char *lean = lean_pointer(address, size)) {
        switch (size) {
        case 1: return lean[0];
        case 2: { uint16_t v; std::memcpy(&v, lean, 2); return __builtin_bswap16(v); }
        case 4: { uint32_t v; std::memcpy(&v, lean, 4); return __builtin_bswap32(v); }
        case 8: { uint64_t v; std::memcpy(&v, lean, 8); return __builtin_bswap64(v); }
        default: break;
        }
    }
#endif
    auto &execution=enter(cpu,address,size,false);
    checked_range(execution,address,size,false);
    if (const unsigned char *guest = fast_pointer(address, size)) {
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) value = (value << 8) | guest[i];
        return value;
    }
    OwnedTraceBatch batch;
    trace_capacity(execution, address, size, false, batch);
    unsigned char bytes[8];
    check_status(execution,ps3_atomic_read(execution.memory->domain,address,bytes,size),address,size,false,batch);
    uint64_t value=0; for(size_t i=0;i<size;++i) value=(value<<8)|bytes[i];
    trace_success(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, false, address, size, value), batch);
    return value;
}
void write_scalar(ppu_context *cpu,uint64_t ea,uint64_t value,size_t size) {
    uint32_t address=static_cast<uint32_t>(ea);
#if PPU_LEAN
    if (unsigned char *lean = lean_pointer(address, size)) {
        PpuMemoryExecution &direct = *reinterpret_cast<PpuMemoryExecution *>(cpu);
        if (__builtin_expect(direct.memory != nullptr, 1)) {
            unsigned char lean_bytes[8];
            switch (size) {
            case 1: lean_bytes[0] = static_cast<unsigned char>(value); break;
            case 2: { const uint16_t v = __builtin_bswap16(static_cast<uint16_t>(value)); std::memcpy(lean_bytes, &v, 2); break; }
            case 4: { const uint32_t v = __builtin_bswap32(static_cast<uint32_t>(value)); std::memcpy(lean_bytes, &v, 4); break; }
            default: { const uint64_t v = __builtin_bswap64(value); std::memcpy(lean_bytes, &v, 8); break; }
            }
            if (size == 1 || size == 2 || size == 4 || size == 8) {
                watch_check(cpu, address, lean_bytes, size);
                lean_store(direct, address, lean, lean_bytes, size);
                return;
            }
        }
    }
#endif
    auto &execution=enter(cpu,address,size,true);
    checked_range(execution,address,size,true);
    unsigned char bytes[8];
    for(size_t i=0;i<size;++i) bytes[size-i-1]=static_cast<unsigned char>(value>>(8*i));
    watch_check(cpu, address, bytes, size);
    if (fast_store(execution, address, bytes, size)) return;
    OwnedTraceBatch batch;
    trace_capacity(execution, address, size, true, batch);
    check_status(execution,ps3_atomic_write(execution.memory->domain,address,bytes,size),address,size,true,batch);
    trace_success(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, true, address, size, value), batch);
}
}
bool ppu_memory_binding_valid(const PpuMemoryExecution &execution)
{
    return coherent(execution);
}
PpuMemoryAdmission ppu_memory_admit(PpuMemoryExecution &execution,PpuMemoryBinding &binding,
    const Ps3PpuExecutionServices &services,PpuMemoryStopPort boundary) {
    if(execution.admitted) return PpuMemoryAdmission::AlreadyAdmitted;
    if(!observation_valid(execution) ||
        (execution.budget && !execution.budget->valid_for_admission()) || !boundary.stop || !valid_ranges(binding) ||
        !same_memory(&binding.coherent_memory,ps3_atomic_domain_memory(binding.domain)) ||
        !same_memory(services.memory,binding.coherent_memory)) return PpuMemoryAdmission::InvalidBinding;
    ps3_atomic_owner reservation{};
    if(ps3_atomic_owner_acquire(binding.domain,&reservation)!=PS3_GUEST_MEMORY_OK) return PpuMemoryAdmission::OwnerUnavailable;
    execution.services=&services; execution.memory=&binding; execution.boundary=boundary;
    execution.reservation=reservation; execution.admitted=true;
    return PpuMemoryAdmission::Ready;
}
ps3_guest_memory_status ppu_memory_release_after_join(PpuMemoryExecution &execution) {
    if(!execution.admitted || !execution.memory) return PS3_GUEST_MEMORY_INVALID_RANGE;
    auto status=ps3_atomic_owner_release(execution.memory->domain,&execution.reservation);
    if(status==PS3_GUEST_MEMORY_OK) {
        execution.admitted=false; execution.services=nullptr; execution.memory=nullptr;
    }
    return status;
}
// ppu-speed-20261113 item 2: the state the inline loads of the generated game code read, and the check every 256th of them runs.
// Off (null) until ppu_inline_memory_set(true), which needs the flat window's commit bitmap and base (the lean path's own inputs),
// g_ppu_fast_memory and g_ppu_lean_memory, so the inline loads switch on exactly when the lean path does (Decision 111). The
// storage is written once, before the release store of the pointer; readers go through the pointer.
const ppu_inline_window *g_ppu_inline_window = nullptr;
namespace { [[maybe_unused]] ppu_inline_window g_ppu_inline_window_storage; }
extern "C" void ppu_inline_memory_set(bool on)
{
#if defined(PS3_NATIVE_FLAT_GUEST) && PS3_PPU_LEAN_MEMORY
    if (on && g_ppu_fast_memory && g_ppu_lean_memory) {
        const unsigned char *bitmap = ps3_flat_guest_committed_bitmap();
        const uintptr_t base = g_ps3_flat_guest_base;
        if (bitmap && base) {
            g_ppu_inline_window_storage.bitmap = bitmap;
            g_ppu_inline_window_storage.base = base;
            __atomic_store_n(&g_ppu_inline_window, &g_ppu_inline_window_storage, __ATOMIC_RELEASE);
            return;
        }
    }
#else
    (void)on;
#endif
    __atomic_store_n(&g_ppu_inline_window, static_cast<const ppu_inline_window *>(nullptr), __ATOMIC_RELEASE);
}
extern "C" uint8_t ppu_native_read8(ppu_context *cpu,uint64_t ea) { return read_scalar(cpu,ea,1); }
extern "C" uint16_t ppu_native_read16(ppu_context *cpu,uint64_t ea) { return read_scalar(cpu,ea,2); }
extern "C" uint32_t ppu_native_read32(ppu_context *cpu,uint64_t ea) { return read_scalar(cpu,ea,4); }
extern "C" uint64_t ppu_native_read64(ppu_context *cpu,uint64_t ea) { return read_scalar(cpu,ea,8); }
// ppu-speed-20261113 item 2: the two ways a load of the generated game code can be served faster than read_scalar (see
// ppu_inline_memory.h). Both count accesses in PpuMemoryExecution::inline_tick, per context, no thread-local storage; every 256th
// access runs the full path's checks (enter: binding coherence, observation, cancel, budget) before it is served.
namespace {
typedef uint16_t __attribute__((may_alias, aligned(1))) inline_u16;
typedef uint32_t __attribute__((may_alias, aligned(1))) inline_u32;
typedef uint64_t __attribute__((may_alias, aligned(1))) inline_u64;
inline uint32_t &inline_tick(ppu_context *cpu) { return reinterpret_cast<PpuMemoryExecution *>(cpu)->inline_tick; }
// the slow route of the inline sites: they have already counted this access. Due: run the checks. Then the original helper.
inline void inline_due(ppu_context *cpu, uint64_t ea, size_t length)
{
    if ((inline_tick(cpu) & 255u) == 0u) (void)enter(cpu, static_cast<uint32_t>(ea), length, false);
}
// the 256th access of a context: the full path's checks (cold, out of line, so the hit path of the leaf functions needs no stack frame)
__attribute__((noinline, cold)) void inline_checkpoint(ppu_context *cpu, uint32_t address, uint32_t size)
{
    (void)enter(cpu, address, size, false);
}
// the leaf route: true and the host address of [ea, ea + size) when the window is on, no page boundary is crossed and the page is
// committed; false: the caller falls back to the original helper (the 256th access runs its checks here, first).
inline bool inline_host(ppu_context *cpu, uint64_t ea, uint32_t size, uintptr_t &host)
{
    const ppu_inline_window *const window = __atomic_load_n(&g_ppu_inline_window, __ATOMIC_RELAXED);
    if (__builtin_expect(window == nullptr, 0)) return false;
    uint32_t &tick = inline_tick(cpu);
    const uint32_t count = tick + 1u;
    tick = count;
    const uint32_t address = static_cast<uint32_t>(ea);
    if (__builtin_expect((count & 255u) == 0u, 0)) inline_checkpoint(cpu, address, size);
    if (size > 1u && __builtin_expect((address & 4095u) > 4096u - size, 0)) return false;
    const uint32_t page = address >> 12;
    if (__builtin_expect(((window->bitmap[page >> 3] >> (page & 7u)) & 1u) == 0u, 0)) return false;
    host = window->base + address;
    return true;
}
}
extern "C" uint8_t ppu_native_read8_slow(ppu_context *cpu, uint64_t ea) { inline_due(cpu, ea, 1); return ppu_native_read8(cpu, ea); }
extern "C" uint16_t ppu_native_read16_slow(ppu_context *cpu, uint64_t ea) { inline_due(cpu, ea, 2); return ppu_native_read16(cpu, ea); }
extern "C" uint32_t ppu_native_read32_slow(ppu_context *cpu, uint64_t ea) { inline_due(cpu, ea, 4); return ppu_native_read32(cpu, ea); }
extern "C" uint64_t ppu_native_read64_slow(ppu_context *cpu, uint64_t ea) { inline_due(cpu, ea, 8); return ppu_native_read64(cpu, ea); }
extern "C" void ppu_native_read_bytes_slow(ppu_context *cpu, uint64_t ea, void *destination, size_t length) { inline_due(cpu, ea, length); ppu_native_read_bytes(cpu, ea, destination, length); }
extern "C" uint8_t ppu_native_read8_fast(ppu_context *cpu, uint64_t ea)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 1, host), 1)) return *reinterpret_cast<const volatile unsigned char *>(host);
    return ppu_native_read8(cpu, ea);
}
extern "C" uint16_t ppu_native_read16_fast(ppu_context *cpu, uint64_t ea)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 2, host), 1)) return __builtin_bswap16(*reinterpret_cast<const volatile inline_u16 *>(host));
    return ppu_native_read16(cpu, ea);
}
extern "C" uint32_t ppu_native_read32_fast(ppu_context *cpu, uint64_t ea)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 4, host), 1)) return __builtin_bswap32(*reinterpret_cast<const volatile inline_u32 *>(host));
    return ppu_native_read32(cpu, ea);
}
extern "C" uint64_t ppu_native_read64_fast(ppu_context *cpu, uint64_t ea)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 8, host), 1)) return __builtin_bswap64(*reinterpret_cast<const volatile inline_u64 *>(host));
    return ppu_native_read64(cpu, ea);
}
extern "C" void ppu_native_read_bytes16_fast(ppu_context *cpu, uint64_t ea, void *destination)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 16, host), 1)) {
        const uint64_t a = *reinterpret_cast<const volatile inline_u64 *>(host), b = *reinterpret_cast<const volatile inline_u64 *>(host + 8);
        static_cast<inline_u64 *>(destination)[0] = a; static_cast<inline_u64 *>(destination)[1] = b;
        return;
    }
    ppu_native_read_bytes(cpu, ea, destination, 16);
}
extern "C" void ppu_native_read_bytes4_fast(ppu_context *cpu, uint64_t ea, void *destination)
{
    uintptr_t host;
    if (__builtin_expect(inline_host(cpu, ea, 4, host), 1)) { *static_cast<inline_u32 *>(destination) = *reinterpret_cast<const volatile inline_u32 *>(host); return; }
    ppu_native_read_bytes(cpu, ea, destination, 4);
}
extern "C" void ppu_native_write8(ppu_context *cpu,uint64_t ea,uint8_t value) { write_scalar(cpu,ea,value,1); }
extern "C" void ppu_native_write16(ppu_context *cpu,uint64_t ea,uint16_t value) { write_scalar(cpu,ea,value,2); }
extern "C" void ppu_native_write32(ppu_context *cpu,uint64_t ea,uint32_t value) { write_scalar(cpu,ea,value,4); }
extern "C" void ppu_native_write64(ppu_context *cpu,uint64_t ea,uint64_t value) { write_scalar(cpu,ea,value,8); }
extern "C" void ppu_native_read_bytes(ppu_context *cpu,uint64_t ea,void *destination,size_t size) {
    if(!size) return;
    uint32_t address=static_cast<uint32_t>(ea);
#if PPU_LEAN
    if (size <= 16 && destination) if (const unsigned char *lean = lean_pointer(address, size)) { std::memcpy(destination, lean, size); return; }
#endif
    auto &execution=enter(cpu,address,size,false);
    if(size>16 || !destination) fail(execution,PpuMemoryStop::InvalidRange,PS3_GUEST_MEMORY_INVALID_RANGE,address,size,false);
    checked_range(execution,address,size,false);
    if (const unsigned char *guest = fast_pointer(address, size)) { std::memcpy(destination, guest, size); return; }
    OwnedTraceBatch batch;
    trace_capacity(execution, address, size, false, batch);
    unsigned char staged[16];
    check_status(execution,ps3_atomic_read(execution.memory->domain,address,staged,size),address,size,false,batch);
    PpuMemoryTraceEvent event{};
    event.operation = PpuMemoryTraceOperation::Read;
    event.address = address;
    event.length = size;
    event.byte_transfer = true;
    std::memcpy(event.bytes, staged, size);
    trace_success(execution, event, batch);
    std::memcpy(destination,staged,size);
}
extern "C" void ppu_native_write_bytes(ppu_context *cpu,uint64_t ea,const void *source,size_t size) {
    if(!size) return;
    uint32_t address=static_cast<uint32_t>(ea);
#if PPU_LEAN
    if (size <= 16 && source) if (unsigned char *lean = lean_pointer(address, size)) {
        PpuMemoryExecution &direct = *reinterpret_cast<PpuMemoryExecution *>(cpu);
        if (__builtin_expect(direct.memory != nullptr, 1)) {
            unsigned char lean_bytes[16];
            std::memcpy(lean_bytes, source, size);
            watch_check(cpu, address, lean_bytes, size);
            lean_store(direct, address, lean, lean_bytes, size);
            return;
        }
    }
#endif
    auto &execution=enter(cpu,address,size,true);
    if(size>16 || !source) fail(execution,PpuMemoryStop::InvalidRange,PS3_GUEST_MEMORY_INVALID_RANGE,address,size,true);
    checked_range(execution,address,size,true);
    unsigned char staged[16];
    std::memcpy(staged, source, size);
    watch_check(cpu, address, staged, size);
    if (fast_store(execution, address, staged, size)) return;
    OwnedTraceBatch batch;
    trace_capacity(execution, address, size, true, batch);
    check_status(execution,ps3_atomic_write(execution.memory->domain,address,staged,size),address,size,true,batch);
    PpuMemoryTraceEvent event{};
    event.operation = PpuMemoryTraceOperation::Write;
    event.address = address;
    event.length = size;
    event.byte_transfer = true;
    std::memcpy(event.bytes, staged, size);
    trace_success(execution, event, batch);
}
extern "C" uint32_t ppu_native_load_reserve32(ppu_context *cpu,uint64_t ea) {
    uint32_t address=static_cast<uint32_t>(ea),value;
    auto &execution=enter(cpu,address,4,false); checked_range(execution,address,4,false);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 4, false, batch);
    check_status(execution,ps3_atomic_load_reserve32(execution.memory->domain,execution.reservation,address,&value),address,4,false,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, false, address, 4, value);
    event.operation = PpuMemoryTraceOperation::LoadReserve;
    trace_success(execution, event, batch);
    return value;
}
extern "C" uint64_t ppu_native_load_reserve64(ppu_context *cpu,uint64_t ea) {
    uint32_t address=static_cast<uint32_t>(ea); uint64_t value;
    auto &execution=enter(cpu,address,8,false); checked_range(execution,address,8,false);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 8, false, batch);
    check_status(execution,ps3_atomic_load_reserve64(execution.memory->domain,execution.reservation,address,&value),address,8,false,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, false, address, 8, value);
    event.operation = PpuMemoryTraceOperation::LoadReserve;
    trace_success(execution, event, batch);
    return value;
}
// Decision 112: a conditional store that is refused before it is tried (the line was written since the lwarx) leaves no reservation behind.
extern "C" void ppu_native_clear_reservation(ppu_context *cpu) {
    auto &execution=owner(cpu);
    if(execution.admitted && execution.memory) (void)ps3_atomic_owner_clear_reservation(execution.memory->domain,execution.reservation);
}
extern "C" bool ppu_native_store_conditional32(ppu_context *cpu,uint64_t ea,uint32_t value) {
    uint32_t address=static_cast<uint32_t>(ea); int stored;
    auto &execution=enter(cpu,address,4,true); checked_range(execution,address,4,true,true);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 4, true, batch, true);
    check_status(execution,ps3_atomic_store_conditional32(execution.memory->domain,execution.reservation,address,value,&stored),address,4,true,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, true, address, 4, value);
    event.operation = PpuMemoryTraceOperation::StoreConditional;
    event.stored = stored != 0;
    trace_success(execution, event, batch);
    return stored!=0;
}
extern "C" bool ppu_native_store_conditional64(ppu_context *cpu,uint64_t ea,uint64_t value) {
    uint32_t address=static_cast<uint32_t>(ea); int stored;
    auto &execution=enter(cpu,address,8,true); checked_range(execution,address,8,true,true);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 8, true, batch, true);
    check_status(execution,ps3_atomic_store_conditional64(execution.memory->domain,execution.reservation,address,value,&stored),address,8,true,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, true, address, 8, value);
    event.operation = PpuMemoryTraceOperation::StoreConditional;
    event.stored = stored != 0;
    trace_success(execution, event, batch);
    return stored!=0;
}
extern "C" uint64_t ppu_native_atomic_add16(ppu_context *cpu,uint64_t ea,int32_t increment) {
    uint32_t address=static_cast<uint32_t>(ea); uint16_t value;
    auto &execution=enter(cpu,address,2,true); checked_range(execution,address,2,true);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 2, true, batch);
    check_status(execution,ps3_atomic_add16(execution.memory->domain,address,increment,&value),address,2,true,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, true, address, 2, value);
    event.operation = PpuMemoryTraceOperation::Add16;
    event.increment = increment;
    trace_success(execution, event, batch);
    return value;
}
extern "C" void ppu_native_zero128(ppu_context *cpu,uint64_t ea) {
    uint32_t address=static_cast<uint32_t>(ea)&~uint32_t{127};
    auto &execution=enter(cpu,address,128,true); checked_range(execution,address,128,true);
    OwnedTraceBatch batch;
    trace_capacity(execution, address, 128, true, batch);
    check_status(execution,ps3_atomic_zero(execution.memory->domain,address,128),address,128,true,batch);
    auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated, true, address, 128, 0);
    event.operation = PpuMemoryTraceOperation::Zero128;
    trace_success(execution, event, batch);
}
// Decision 33: the TOC a foreign caller (lifted firmware) must be given before it can jump into
// the game's own code. False when the composition selected no callback owner or none is active.
bool ppu_memory_guest_callback_toc(ppu_context *cpu, uint64_t *toc) {
    if (!cpu) return false;
    PpuMemoryExecution &execution = owner(cpu);
    PpuMemoryGuestCallback *callback = execution.guest_callback;
    uint64_t published = 0;
    if (!callback || !callback->entered || !callback->entered(callback->context, &published)) return false;
    if (toc) *toc = published;
    return true;
}
