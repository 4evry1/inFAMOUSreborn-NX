#include "startup_execution.h"
#include "discovery_budget_binding.h"
#include "native_timebase_abi.h"
#ifdef PS3_NATIVE_STARTUP_TIMEBASE_COUNTER
#include "timebase.h"
#endif
#include "../diagnostics/startup_observation.h"
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
#include "startup_event_queue.h"
#include "startup_event_ports.h"
/* The same block's creation entry, cellSpursCreateTasksetWithAttribute (G9-3): the three argument
 * gates, the 6400-byte CellSpursTaskset image and the registration projection. Included here rather
 * than in the header because its only user is this translation unit's dispatch, and the feature-off
 * build must not carry even the builder symbols. See
 */
#include "../domain/spurs_taskset_create.h"
/* The next entry on the same path, _cellSpursLFQueueInitialize (G14): the argument gates, the owner
 * tag, the versioned CellSyncLFQueue line and the two auxiliary runs. Included here for the same
 * reason -- its only user is this translation unit's dispatch, and the feature-off build must not
 * carry even the builder symbols. */
#include "../domain/spurs_lfqueue.h"
/* The next entry on the same path,
 * cellSpursLFQueueAttachLv2EventQueue: the two entry gates, the owner resolution, the shape gate,
 * the iteration count and the two-width publication. Included here for the same reason as its
 * predecessor -- its only user is this translation unit's dispatch, and the feature-off build must
 * not carry even the builder symbols. */
#include "../domain/spurs_lfqueue_attach.h"
/* G15 (decision 25): the process-SDK branch table and the `0x17778` arm inFAMOUS actually takes, plus
 * the taskset's `+0x60` CellSpurs EA that arm's connect needs. See
 * Appended to the same block so the feature-off
 * build carries none of it either. */
#include "../domain/spurs_lfqueue_attach_sdk25.h"
/* G17: cellSpursCreateTask (NID 0xBEB600AC), the entry the game reaches next
 * (LR 0x00424744). Same block, so the
 * feature-off build carries none of its builders either. */
#include "../domain/spurs_task_create.h"
#include "../domain/spurs_ready_count.h"
#endif
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
#include "guest_timebase.h"
#endif
#ifdef PS3_NATIVE_LLE_SRE
#include "lle_sre.h"
#include "startup_absent_imports.h"
#endif
#ifdef PS3_NATIVE_LLE_PROFILE
#include "savedata_param_sfo.h"
#endif
#ifdef PS3_NATIVE_SPU_RUN
#include "spu_run_port.h"
#endif
#include <algorithm>
#include <switch.h>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <cstdio>
#include <memory>
#include <time.h>
#include <cstdio>

// Root (2026-11-12, Decision 64): the short spin locks of this file must not spin for ever. Horizon schedules
// strictly by priority and does not time-slice: a thread that spins on a lock whose holder has a lower priority on
// the same core starves the holder, and the lock is never released (seen as a frozen tick thread behind the atomic
// domain guard). After a few tries the waiter sleeps, which lets any thread run.
#if defined(__SWITCH__)
extern "C" void svcSleepThread(int64_t nano);
static inline void startup_spin_relax() { svcSleepThread(INT64_C(50000)); }
#else
#include <thread>
static inline void startup_spin_relax() { std::this_thread::yield(); }
#endif
static inline void startup_spin_acquire(std::atomic_flag &flag)
{
    unsigned tries = 0;
    while (flag.test_and_set(std::memory_order_acquire))
        if (++tries >= 32u) { startup_spin_relax(); tries = 0; }
}
extern "C" {
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void *) = nullptr;
}
/* Declared in startup_execution.h (StartupGcmGuestBridge); definition stays private to this
 * translation unit. Must be at namespace scope for the header pointer to name this type. */
struct HleTraceBatch {
    PpuMemoryTraceRecorder *recorder = nullptr;
    HleTraceBatch() = default;
    HleTraceBatch(const HleTraceBatch &) = delete;
    HleTraceBatch &operator=(const HleTraceBatch &) = delete;
};
namespace {
// Raw owned memory, architectural state and cancellation data only; no services.
thread_local StartupControl *active = nullptr;
extern "C++" {
namespace activity_profile {
enum : uint32_t { kThreads = 64, kBuckets = 96 };
struct Slot { std::atomic<uint64_t> now{0}; uint32_t guest_thread = 0; uint32_t entries = 0, last_lr = 0, lr_usleep = 0, lr_receive = 0; uint64_t last_arg0 = 0, usleep_arg = 0, usleep_max = 0; uint64_t call_key[64]{}; uint32_t call_count[64]{}, call_lr[64]{}; uint32_t call_arg[64]{}; uint32_t call_ret[64]{}, call_ret_changes[64]{}; uint64_t value[kBuckets]{}; uint64_t count[kBuckets]{}; uint64_t other = 0, samples = 0; };
Slot g_slots[kThreads];
std::atomic<uint32_t> g_slot_count{0};
thread_local Slot *t_slot = nullptr;
Slot *slot()
{
    if (!t_slot) {
        const uint32_t index = g_slot_count.fetch_add(1, std::memory_order_relaxed);
        static Slot overflow;
        t_slot = index < kThreads ? &g_slots[index] : &overflow;
    }
    return t_slot;
}
struct Scope {   // a stop that unwinds past this leaves the word stale: that thread has ended
    Slot *const s; const uint64_t previous; const uint64_t *result = nullptr; uint32_t entry = 64;
    Scope(uint64_t kind, uint64_t value, uint32_t lr, uint64_t arg0 = 0, const uint64_t *result_register = nullptr) : s(slot()), previous(s->now.load(std::memory_order_relaxed)), result(result_register) {
        s->last_arg0 = arg0;
        if (kind == 1 && value == 141) { s->usleep_arg = arg0; if (arg0 > s->usleep_max) s->usleep_max = arg0; }
        s->now.store(kind << 32 | value, std::memory_order_relaxed);
        ++s->entries; s->last_lr = lr;   // racy diagnostics, read once at the end
        {   // per-thread call counts by (kind, value); written by the owner thread only
            const uint64_t key = kind << 32 | value;
            uint32_t at = uint32_t((key * UINT64_C(0x9E3779B97F4A7C15)) >> 58);
            for (uint32_t probe = 0; probe < 64u; ++probe, at = (at + 1u) & 63u) {
                if (s->call_count[at] == 0u) s->call_key[at] = key;
                if (s->call_key[at] == key) { ++s->call_count[at]; s->call_lr[at] = lr; s->call_arg[at] = uint32_t(arg0); entry = at; break; }
            }
        }
        if (kind == 1 && value == 141) s->lr_usleep = lr;
        if (kind == 1 && value == 130) s->lr_receive = lr;
    }
    ~Scope() {
        s->now.store(previous, std::memory_order_relaxed);
        if (result && entry < 64u) {   // the call's r3 on return, and how often it differed from the time before
            const uint32_t value = uint32_t(*result);
            if (value != s->call_ret[entry]) ++s->call_ret_changes[entry];
            s->call_ret[entry] = value;
        }
    }
};
}
[[maybe_unused]] void activity_sample_impl()
{
    using namespace activity_profile;
    const uint32_t threads = std::min<uint32_t>(g_slot_count.load(std::memory_order_relaxed), kThreads);
    for (uint32_t index = 0; index < threads; ++index) {
        Slot &s = g_slots[index];
        const uint64_t now = s.now.load(std::memory_order_relaxed);
        ++s.samples;
        uint32_t bucket = 0;
        while (bucket < kBuckets && s.count[bucket] && s.value[bucket] != now) ++bucket;
        if (bucket == kBuckets) { ++s.other; continue; }
        s.value[bucket] = now; ++s.count[bucket];
    }
}
[[maybe_unused]] void activity_print_calls_impl(unsigned seconds)
{
    using namespace activity_profile;
    const uint32_t threads = std::min<uint32_t>(g_slot_count.load(std::memory_order_relaxed), kThreads);
    for (uint32_t index = 0; index < threads; ++index) {
        const Slot &s = g_slots[index];
        if (seconds != 0xFFFFFFFFu && index != 0 && s.entries < 20000u) continue;   // the main thread and the busy ones; every thread at the end
        std::printf("STARTUP_ACTIVITY_CALLS seconds=%u slot=%u entries=%u:", seconds, unsigned(index), unsigned(s.entries));
        for (uint32_t at = 0; at < 64u; ++at)
            if (s.call_count[at])
                std::printf(" %s%llx=%u@%x(%x)>%x/%u", (s.call_key[at] >> 32) == 1 ? "sc" : "nid", (unsigned long long)(s.call_key[at] & 0xFFFFFFFFu),
                    unsigned(s.call_count[at]), unsigned(s.call_lr[at]), unsigned(s.call_arg[at]), unsigned(s.call_ret[at]), unsigned(s.call_ret_changes[at]));
        std::printf("\n");
    }
}
[[maybe_unused]] void activity_print_impl()
{
    using namespace activity_profile;
    const uint32_t threads = std::min<uint32_t>(g_slot_count.load(std::memory_order_relaxed), kThreads);
    for (uint32_t index = 0; index < threads; ++index) {
        const Slot &s = g_slots[index];
        if (!s.samples) continue;
        std::printf("STARTUP_ACTIVITY slot=%u samples=%llu other=%llu entries=%u last_lr=0x%x lr_usleep=0x%x lr_receive=0x%x:", unsigned(index),
            (unsigned long long)s.samples, (unsigned long long)s.other, unsigned(s.entries), unsigned(s.last_lr), unsigned(s.lr_usleep), unsigned(s.lr_receive));
        std::printf(" [now=%llx last_arg0=0x%llx usleep_arg=%llu usleep_max=%llu]", (unsigned long long)s.now.load(), (unsigned long long)s.last_arg0,
            (unsigned long long)s.usleep_arg, (unsigned long long)s.usleep_max);
        for (uint32_t bucket = 0; bucket < kBuckets && s.count[bucket]; ++bucket)
            if (s.count[bucket] * 200u >= s.samples)   // 0.5 % and above
                std::printf(" %s%llx=%llu", (s.value[bucket] >> 32) == 1 ? "sc" : (s.value[bucket] >> 32) == 2 ? "nid" : "guest",
                    (unsigned long long)(s.value[bucket] & 0xFFFFFFFFu), (unsigned long long)s.count[bucket]);
        std::printf("\n");
    }
}
}
}  // namespace
/* Root (B2b): the cpu this thread's startup dispatcher owns, for lifted code entered directly as a
 * PPU thread entry (lle_sre.cpp current_cpu fallback). Null when no dispatch is active here. */
extern "C" ppu_context *startup_active_cpu(void) { return active ? active->cpu : nullptr; }
#ifdef PS3_NATIVE_FAST_SYNC
/* ppu-speed-20261113 item 1: with a host view of guest memory (the flat window), the lwmutex import arm does not go through the
 * atomic-domain guard for its three memory touches (the TOC-slot probe, the control-block probe, the TOC-slot store): a probe is a
 * committed-pages test, the store the lean-store protocol of ppu_memory_context.cpp (sticky test, store, barrier, second test; a
 * hit repeats the store through the domain). Set once by the composition before any guest thread runs; null = every arm as before. */
namespace {
struct FastSyncState { unsigned char *(*pointer)(void *context, uint32_t address, uint32_t bytes); void *context; };
FastSyncState g_fast_sync{nullptr, nullptr};
}
extern "C++" void native_startup_enable_fast_sync(unsigned char *(*pointer)(void *, uint32_t, uint32_t), void *context)
{
    g_fast_sync.context = context;
    __atomic_store_n(&g_fast_sync.pointer, pointer, __ATOMIC_RELEASE);
}
#endif
namespace {
static_assert(std::is_trivially_destructible<HleTraceBatch>::value,
    "Terminal credit closure must not depend on unwinding");
void finish_owned(HleTraceBatch *batch)
{
    if (batch && batch->recorder) {
        batch->recorder->finish();
        batch->recorder = nullptr;
    }
}
#ifdef PS3_NATIVE_LIVE_TRACE
/* Root diagnostic (B2a Eden hang): bounded live trace to the Horizon debug log (Eden's eden_log.txt),
 * which survives a hang that never reaches the end-of-run guest log. NRO overlay build only. */
static std::atomic<uint32_t> g_live_trace_count{0};
#if defined(__SWITCH__) && defined(PS3_NATIVE_NXLINK)
extern "C" void native_nxlink_line(const char *text, size_t length);
#endif
/* Root (C1b, 2026-11-11): the game polls one libsre export thousands of times (cellSpurs 0x4E153E3E from 0x41DBB0)
 * and filled the whole line budget, hiding every later SPU and stop line. While this thread repeats the SAME import
 * NID, only its import lines ("LT hle...", "LT lle-ret...") are folded: the first 8 calls and every 4096th print. */
static thread_local bool g_live_trace_fold = false;
static void live_trace(const char *format, ...)
{
    if (g_live_trace_fold && (!std::strncmp(format, "LT hle", 6) || !std::strncmp(format, "LT lle-ret", 10))) return;
    if (g_live_trace_count.fetch_add(1, std::memory_order_relaxed) >= 6000) return;
    char line[192];
    va_list args;
    va_start(args, format);
    int length = std::vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (length <= 0) return;
    if (size_t(length) > sizeof(line) - 2) length = int(sizeof(line) - 2);
    line[length++] = 0x0a;
#ifdef __SWITCH__
    svcOutputDebugString(line, size_t(length));
#ifdef PS3_NATIVE_NXLINK
    native_nxlink_line(line, size_t(length));   // the live trace on the real console (switch_main.c)
#endif
#else
    std::fwrite(line, 1, size_t(length), stderr);
#endif
}
#define LIVE_TRACE(...) live_trace(__VA_ARGS__)
#else
#define LIVE_TRACE(...) ((void)0)
#endif
/* Root (C1b): the SPU runner's diagnostic sink. Bounded like LIVE_TRACE; a no-op without the trace flag. */
extern "C" void startup_live_trace_line(void *, const char *line)
{
#ifdef PS3_NATIVE_LIVE_TRACE
    live_trace("LT %s", line);
#else
    (void)line;
#endif
}
static uint32_t g_missing_services_thread = 0, g_missing_services_r11 = 0, g_missing_services_lr = 0;
static int64_t g_missing_services_ra_delta = 0;
extern "C" void lv2_syscall(ppu_context *);
extern "C" void startup_print_missing_services(void)
{
    std::printf("STARTUP_MISSING_SERVICES thread=0x%x ra_minus_lv2_syscall=%lld r11=0x%x lr=0x%x\n", g_missing_services_thread,
        (long long)g_missing_services_ra_delta, g_missing_services_r11, g_missing_services_lr);
}
[[noreturn]] void stop(StartupStop reason, HleTraceBatch *batch = nullptr)
{
    LIVE_TRACE("LT stop reason=%d sc=%u", int(reason), active ? unsigned(active->syscall_number) : 0u);
#ifdef PS3_NATIVE_LIVE_TRACE
    /* Root diagnostic (2026-11-11): which of the many TraceCapacity sites stopped a game worker thread. The return
     * address is printed relative to lv2_syscall so it can be resolved against the ELF despite the random load base. */
    if (reason == StartupStop::TraceCapacity)
        LIVE_TRACE("LT trace-capacity ra_minus_lv2_syscall=%lld thread=0x%x batch_arg=%d batch_recorder=%d",
            (long long)((intptr_t)__builtin_return_address(0) - (intptr_t)(void *)&lv2_syscall),
            active && active->cpu ? unsigned(active->cpu->thread_id) : 0u, int(batch != nullptr),
            int(batch && batch->recorder));
#endif
    if (!active || !active->cpu) std::abort();
    /* Root diagnostic (2026-11-12): which of the many MissingServices sites ended a thread. First one wins. The
     * return address is kept relative to lv2_syscall so it resolves against the ELF despite the random load base. */
    if (reason == StartupStop::MissingServices && !g_missing_services_thread) {
        g_missing_services_thread = uint32_t(active->cpu->thread_id);
        g_missing_services_ra_delta = int64_t((intptr_t)__builtin_return_address(0) - (intptr_t)(void *)&lv2_syscall);
        g_missing_services_r11 = uint32_t(active->cpu->gpr[11]);
        g_missing_services_lr = uint32_t(active->cpu->lr);
    }

    finish_owned(batch);
    active->stop = reason;
    active->terminal = *active->cpu;
    StartupControl *const control = active;
    g_trampoline_fn = nullptr;
    active = nullptr;
    // Terminal record is complete and dispatch state is cleared: publish exactly once, then always exit.
    if (control->terminal_notification.fn)
        control->terminal_notification.fn(control->terminal_notification.context, control);
    threadExit();
}
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
bool spurs_fence_pending(StartupControl &control);
StartupStop spurs_fence_stop(const StartupControl &control)
{
    switch (control.spurs_fence.category) {
    case PS3_SPURS_STOP_CATEGORY_KERNEL: return StartupStop::SpursKernelFence;
    case PS3_SPURS_STOP_CATEGORY_HELPER_EVENT:
    case PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE: return StartupStop::SpursHelperFence;
    }
    return StartupStop::ProviderFailure;
}
#endif
enum class BoundaryBudget { LegacyOnly, Poll };
StartupStop budget_stop_reason(infamous::discovery::StopReason reason)
{
    using infamous::discovery::StopReason;
    switch (reason) {
    case StopReason::Cancelled: return StartupStop::Cancelled;
    case StopReason::Deadline: return StartupStop::Deadline;
    case StopReason::GeneratedExhausted: return StartupStop::GeneratedBudget;
    case StopReason::HleExhausted: return StartupStop::HleSyscallBudget;
    case StopReason::IndirectExhausted: return StartupStop::IndirectBudget;
    case StopReason::InvalidClock:
    case StopReason::ClockRegressed: return StartupStop::BudgetClockFailure;
    case StopReason::None:
    case StopReason::InvalidCategory: return StartupStop::BudgetInvalidInput;
    }
    return StartupStop::BudgetInvalidInput;
}
void check_boundary(BoundaryBudget mode, HleTraceBatch *batch = nullptr)
{
    if (!active) std::abort();
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    if (spurs_fence_pending(*active)) stop(spurs_fence_stop(*active), batch);
#endif
    if (active->cancelled.load(std::memory_order_acquire)) stop(StartupStop::Cancelled, batch);
    if (active->deadline_tick && armGetSystemTick() >= active->deadline_tick) stop(StartupStop::Deadline, batch);
    if (mode == BoundaryBudget::Poll) {
        // These sites have already validated the incoming CPU/owner. Never
        // apply admission freshness checks once its counters have advanced.
        if (!active->cpu) std::abort();
        auto &owner = *reinterpret_cast<PpuMemoryExecution *>(active->cpu);
        if (owner.budget) {
            const auto reason = owner.budget->poll();
            if (reason != infamous::discovery::StopReason::None)
                stop(budget_stop_reason(reason), batch);
        }
    }
}
void admit_startup_budget(StartupExecution &execution, infamous::discovery::Category category)
{
    if (!execution.ppu.budget) return;
    const auto decision = execution.ppu.budget->admit(category);
    if (!decision.admitted) stop(budget_stop_reason(decision.reason));
}
StartupExecution &execution_owner(ppu_context *cpu)
{
    auto *memory_owner = reinterpret_cast<PpuMemoryExecution *>(cpu);
    if (!ppu_memory_binding_valid(*memory_owner)) stop(StartupStop::MissingServices);
    return *reinterpret_cast<StartupExecution *>(memory_owner);
}
bool writable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (range.writable && address >= range.address &&
            uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
bool trace_can_begin(const StartupExecution &execution, size_t count)
{
    if (execution.ppu.recorder) return execution.ppu.recorder->can_begin(count);
    const auto &journal = execution.control->journal;
    return ppu_memory_trace_available(journal) && count <= journal.capacity - journal.count;
}
void begin_trace(StartupExecution &execution, size_t count, HleTraceBatch &batch)
{
    if (!execution.ppu.recorder) return;
    if (batch.recorder) std::abort();
    if (!execution.ppu.recorder->begin(count)) stop(StartupStop::TraceCapacity);
    batch.recorder = execution.ppu.recorder;
}
void publish_trace(StartupExecution &execution, const PpuMemoryTraceEvent &event,
    HleTraceBatch &batch)
{
    if (batch.recorder) {
        batch.recorder->publish(event);
        return;
    }
    if (execution.ppu.recorder) std::abort();
    if (!ppu_memory_trace_append(execution.control->journal, event))
        stop(StartupStop::TraceCapacity, &batch);
}
#ifdef PS3_NATIVE_LLE_SRE
/* Decision 33: the recorder admits exactly ONE open batch at a time, and Sony's lifted bodies reach
 * ps3_hle_call (their own import stubs) and lv2_syscall from inside a routed call. A route that
 * entered with the caller's batch still open would therefore make every nested entry refuse with
 * TraceCapacity -- a bookkeeping refusal, not a firmware gap. The route closes the caller's batch on
 * entry and gives the nested entries a free recorder, then restores exactly what it closed. */
class SuspendedTraceBatch {
public:
    /* True when this route had to close the recorder to make the nested entries possible. */
    static bool close(PpuMemoryTraceRecorder *recorder)
    {
        if (!recorder || !recorder->report().batch_active) return false;
        recorder->finish();
        return true;
    }
    /* The publication budget the caller still had open; the route hands it back on the way out. */
    static size_t remaining(const PpuMemoryTraceRecorder *recorder)
    {
        return recorder ? recorder->report().remaining_publications : 0;
    }
    /* Reopens the caller's publication budget. */
    static void reopen(PpuMemoryTraceRecorder *recorder, size_t publications)
    {
        if (recorder && !recorder->report().batch_active) recorder->begin(publications ? publications : 1);
    }
};
#endif

#ifdef PS3_NATIVE_FAST_SYNC
inline unsigned char *fast_sync_pointer(uint32_t address, uint32_t bytes)
{
    auto *const pointer = __atomic_load_n(&g_fast_sync.pointer, __ATOMIC_ACQUIRE);
    return pointer ? pointer(g_fast_sync.context, address, bytes) : nullptr;
}
inline bool fast_sync_readable(uint32_t address, uint32_t bytes) { return fast_sync_pointer(address, bytes) != nullptr; }
/* The 8-byte TOC store of an import arm (a thread's own stack slot) without the domain guard: sticky line -> not here; else store,
 * full barrier, second test, and a hit repeats the store through the domain (the lean_store sequence). true = stored. */
inline bool fast_sync_store(StartupExecution &execution, uint32_t slot, uint64_t encoded, HleTraceBatch &batch)
{
    unsigned char *const host = fast_sync_pointer(slot, sizeof(encoded));
    if (!host || ps3_atomic_sticky_hit(slot, sizeof(encoded))) return false;
    std::memcpy(host, &encoded, sizeof(encoded));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ps3_atomic_domain *const domain = execution.ppu.memory->domain;
    if (ps3_atomic_sticky_hit(slot, sizeof(encoded)) || ps3_atomic_domain_write_conflicts(domain, slot, sizeof(encoded))) {
        if (ps3_atomic_write(domain, slot, &encoded, sizeof(encoded)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
    }
    return true;
}
#endif
void preflight_toc(StartupExecution &execution, uint64_t slot)
{
    if (slot > UINT32_MAX || !writable(execution, uint32_t(slot), 8))
        stop(StartupStop::InvalidMemory);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
#ifdef PS3_NATIVE_FAST_SYNC
    if (fast_sync_readable(uint32_t(slot), sizeof(uint64_t))) return;   // committed: the probe read would succeed
#endif
    uint64_t probe;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, uint32_t(slot), &probe, sizeof(probe)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory);
}
void publish_toc(StartupExecution &execution, uint64_t slot, uint64_t toc,
    HleTraceBatch &batch, bool *committed = nullptr)
{
    if (committed) check_boundary(BoundaryBudget::Poll, &batch);
    const uint64_t encoded = __builtin_bswap64(toc);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
#ifdef PS3_NATIVE_FAST_SYNC
    if (!fast_sync_store(execution, uint32_t(slot), encoded, batch))
#endif
    if (memory.write(memory.context, uint32_t(slot), &encoded, sizeof(encoded)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    // The store is committed even if trace publication fails below.
    if (committed) *committed = true;
    if (committed) check_boundary(BoundaryBudget::Poll, &batch);
    publish_trace(execution,
        ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, uint32_t(slot), 8, toc), batch);
}
constexpr uint32_t boot_disc_info_nid = 0xb72bc4e6;
constexpr uint32_t boot_disc_parameter_error = 0x8002bd03;
constexpr uint32_t boot_disc_not_disc_error = 0x8002bd02;
constexpr size_t boot_disc_payload_size = 32;

void write_boot_disc_scalar(StartupExecution &execution, uint64_t address, size_t width,
    uint64_t value, uint64_t toc_slot, bool is_toc = false)
{
    auto &observation = execution.control->boot_disc;
    check_boundary(BoundaryBudget::Poll);
    if (address > UINT32_MAX || !writable(execution, uint32_t(address), width) ||
        (!is_toc && address < toc_slot + 8 && toc_slot < address + width)) {
        execution.control->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    unsigned char bytes[8]{};
    for (size_t index = 0; index < width; ++index)
        bytes[index] = static_cast<unsigned char>(value >> (8 * (width - 1 - index)));
    if (!is_toc) ++observation.output_write_attempts;
    const auto &memory = execution.ppu.memory->coherent_memory;
    observation.memory_status = memory.write(memory.context, uint32_t(address), bytes, width);
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    if (is_toc) observation.toc_written = true;
    else {
        ++observation.output_writes;
        observation.output_written_bytes += width;
    }
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(address), width, value), batch);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
}

bool valid_boot_title(const BootContentMetadata &metadata)
{
    if (metadata.title_id[9] != '\0') return false;
    for (size_t index = 0; index < 9; ++index) {
        const unsigned char character = metadata.title_id[index];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9'))) return false;
    }
    return true;
}

void dispatch_boot_disc_info(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.boot_disc;
    observation = {};
    ++control.boot_disc_calls;
    observation.raw_output = cpu.gpr[3];
    observation.output = uint32_t(cpu.gpr[3]);
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2], toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution, toc_slot);
    uint32_t result = boot_disc_parameter_error;
    if (observation.output) {
        // The original wrapper clears four bytes before querying the boot source.
        write_boot_disc_scalar(execution, observation.output, 4, 0, toc_slot);
        observation.metadata = execution.boot_content;
        observation.metadata_checked = true;
        switch (observation.metadata.origin) {
        case BootContentOrigin::Unavailable:
            observation.failure = StartupBootDiscFailure::MissingMetadata;
            stop(StartupStop::MissingServices);
        case BootContentOrigin::InstalledPackage:
            result = boot_disc_not_disc_error;
            break;
        case BootContentOrigin::DiscPackage: {
            if (!valid_boot_title(observation.metadata)) {
                observation.failure = StartupBootDiscFailure::InvalidMetadata;
                stop(StartupStop::BootDiscStop);
            }
            // Reserved zeros are our native provider policy, not an inferred Sony payload.
            unsigned char payload[boot_disc_payload_size]{};
            std::memcpy(payload, observation.metadata.title_id, sizeof observation.metadata.title_id);
            for (size_t index = 0; index < 4; ++index)
                payload[12 + index] = static_cast<unsigned char>(observation.metadata.parental_level >> (24 - 8 * index));
            for (size_t index = 0; index < 28; ++index)
                write_boot_disc_scalar(execution, uint64_t(observation.output) + index, 1, payload[index], toc_slot);
            // Preserve the original wrapper's final four stores, including partial-failure order.
            constexpr size_t final_offsets[] = {31, 28, 29, 30};
            for (const size_t index : final_offsets)
                write_boot_disc_scalar(execution, uint64_t(observation.output) + index, 1, payload[index], toc_slot);
            result = 0;
            break;
        }
        default:
            observation.failure = StartupBootDiscFailure::InvalidMetadata;
            stop(StartupStop::BootDiscStop);
        }
    }
    write_boot_disc_scalar(execution, toc_slot, 8, toc, toc_slot, true);
    observation.result = result;
    observation.has_result = true;
    cpu.gpr[3] = uint64_t(int64_t(int32_t(result)));
    ++control.boot_disc_returns;
}
constexpr uint32_t system_parameter_nid=0x40e895d3;
constexpr uint32_t system_parameter_value_error=0x8002b102;
constexpr uint32_t system_parameter_language=0x111, system_parameter_confirm=0x112;
constexpr uint32_t system_parameter_date=0x114, system_parameter_time=0x115, system_parameter_parental=0x121;

uint32_t effective_system_parameter(uint32_t id, const StartupProcessSdk &sdk)
{
    if (id != system_parameter_language || !sdk.valid) return id;
    if (sdk.raw <= 0x30ffff) return 0x2101;
    if (sdk.raw <= 0x3fffff) return 0x2102;
    if (sdk.raw <= 0x42ffff) return 0x2103;
    return id;
}

bool recognized_system_parameter(uint32_t id)
{
    // Literal recognition branches in the original libsysutil helper; recognition is not implementation.
    return id==0x111 || id==0x112 || (id>=0x114 && id<=0x117) ||
        (id>=0x121 && id<=0x125) || id==0x141 || (id>=0x151 && id<=0x157) ||
        (id>=0x1001 && id<=0x1007) || id==0x1013 || (id>=0x1021 && id<=0x1023) ||
        (id>=0x1030 && id<=0x103b) || (id>=0x2101 && id<=0x2103);
}

[[noreturn]] void stop_system_parameter(StartupExecution &execution, StartupSystemParameterFailure failure)
{
    execution.control->system_parameter.failure=failure;
    stop(StartupStop::SystemParameterStop);
}

uint32_t resolve_system_parameter(StartupExecution &execution)
{
    auto &observation=execution.control->system_parameter;
    if (observation.id!=system_parameter_language && observation.id!=system_parameter_confirm &&
        observation.id!=system_parameter_date && observation.id!=system_parameter_time &&
        observation.id!=system_parameter_parental)
        stop_system_parameter(execution,StartupSystemParameterFailure::UnimplementedParameter);
    observation.settings=execution.system_parameters;
    observation.settings_checked=true;
    const auto &settings=observation.settings;
    if (observation.id==system_parameter_language) {
        if (settings.language.status==SystemLanguageStatus::Unavailable)
            stop_system_parameter(execution,StartupSystemParameterFailure::MissingLanguage);
        if (settings.language.status!=SystemLanguageStatus::Available || uint32_t(settings.language.language)>19)
            stop_system_parameter(execution,StartupSystemParameterFailure::UnsupportedLanguage);
        const uint32_t language=uint32_t(settings.language.language);
        if (observation.effective_id==system_parameter_language) return language;
        if (settings.language_compatibility==SystemLanguageCompatibility::Unconfigured)
            stop_system_parameter(execution,StartupSystemParameterFailure::MissingProfile);
        if (settings.language_compatibility!=SystemLanguageCompatibility::NativePortLegacyFamilies)
            stop_system_parameter(execution,StartupSystemParameterFailure::InvalidProfile);
        if (language<=uint32_t(SystemLanguage::Polish)) return language;
        // Explicit native-profile family projection; Sony's provider mapping is not reconstructed.
        if (settings.language.language==SystemLanguage::EnglishGB) {
            observation.legacy_projection=true;
            return uint32_t(SystemLanguage::EnglishUS);
        }
        if (settings.language.language==SystemLanguage::PortugueseBR) {
            observation.legacy_projection=true;
            return uint32_t(SystemLanguage::PortuguesePT);
        }
        stop_system_parameter(execution,StartupSystemParameterFailure::UnsupportedLanguage);
    }
    if (settings.profile==SystemParameterProfile::Unavailable)
        stop_system_parameter(execution,StartupSystemParameterFailure::MissingProfile);
    if (settings.profile!=SystemParameterProfile::NativePortConventions)
        stop_system_parameter(execution,StartupSystemParameterFailure::InvalidProfile);
    uint32_t value=0, maximum=0;
    switch (observation.id) {
    case system_parameter_confirm: value=uint32_t(settings.confirm_button);maximum=1;break;
    case system_parameter_date: value=uint32_t(settings.date_format);maximum=2;break;
    case system_parameter_time: value=uint32_t(settings.time_format);maximum=1;break;
    case system_parameter_parental: value=settings.parental_level;maximum=11;break;
    default: std::abort();
    }
    if (value>maximum) stop_system_parameter(execution,StartupSystemParameterFailure::InvalidProfile);
    return value;
}

void write_system_parameter_scalar(StartupExecution &execution, uint64_t address, size_t width,
    uint64_t value, uint64_t toc_slot, bool is_toc)
{
    auto &observation=execution.control->system_parameter;
    check_boundary(BoundaryBudget::Poll);
    if (address>UINT32_MAX || !writable(execution,uint32_t(address),width) ||
        (!is_toc && address<toc_slot+8 && toc_slot<address+width)) {
        execution.control->invalid_address=address;
        stop(StartupStop::InvalidMemory);
    }
    if (!trace_can_begin(execution,1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    unsigned char bytes[8]{};
    for (size_t index=0;index<width;++index)
        bytes[index]=static_cast<unsigned char>(value>>(8*(width-1-index)));
    if (!is_toc) ++observation.output_write_attempts;
    const auto &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,uint32_t(address),bytes,width);
    if (observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    if (is_toc) observation.toc_written=true;
    else observation.output_written=true;
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,uint32_t(address),width,value),batch);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
}

void dispatch_system_parameter(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.system_parameter;
    observation={};
    ++control.system_parameter_calls;
    observation.raw_id=cpu.gpr[3];observation.id=uint32_t(cpu.gpr[3]);
    observation.raw_output=cpu.gpr[4];observation.output=uint32_t(cpu.gpr[4]);
    observation.process_sdk=execution.process_sdk;
    observation.effective_id=effective_system_parameter(observation.id,observation.process_sdk);
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution,toc_slot);
    uint32_t result=system_parameter_value_error;
    const bool wrapper_rejects_id=observation.id==0x124 ||
        (observation.id==0x122 && observation.process_sdk.valid && observation.process_sdk.raw>0x23ffff);
    if (!wrapper_rejects_id && observation.output && recognized_system_parameter(observation.effective_id)) {
        observation.value=resolve_system_parameter(execution);
        write_system_parameter_scalar(execution,observation.output,4,observation.value,toc_slot,false);
        result=0;
    }
    write_system_parameter_scalar(execution,toc_slot,8,toc,toc_slot,true);
    observation.result=result;
    observation.has_result=true;
    cpu.gpr[3]=uint64_t(int64_t(int32_t(result)));
    ++control.system_parameter_returns;
}
constexpr uint32_t system_string_nid=0x938013a0;
constexpr uint32_t system_string_value_error=0x8002b102, system_string_size_error=0x8002b103;
constexpr uint32_t system_string_user_name=0x131, system_string_device_label=0x113;

[[noreturn]] void stop_system_string(StartupExecution &execution, StartupSystemStringFailure failure)
{
    execution.control->system_string.failure=failure;
    stop(StartupStop::SystemStringStop);
}

void require_system_profile_source(StartupExecution &execution)
{
    auto &observation=execution.control->system_string;
    observation.profile_checked=true;
    observation.source=execution.system_profile.source;
    if (observation.source==SystemProfileSource::NativeApplicationProfile) return;
    observation.profile_status=observation.source==SystemProfileSource::Unconfigured?
        SystemProfileStatus::Unconfigured:SystemProfileStatus::InvalidSource;
    stop_system_string(execution,StartupSystemStringFailure::InvalidProfile);
}

void write_system_string_scalar(StartupExecution &execution, uint64_t address, size_t width,
    uint64_t value, uint64_t toc_slot, bool is_toc)
{
    auto &observation=execution.control->system_string;
    check_boundary(BoundaryBudget::Poll);
    if (address>UINT32_MAX || !writable(execution,uint32_t(address),width) ||
        (!is_toc && address<toc_slot+8 && toc_slot<address+width)) {
        execution.control->invalid_address=address;
        stop(StartupStop::InvalidMemory);
    }
    if (!trace_can_begin(execution,1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    unsigned char bytes[8]{};
    for (size_t index=0;index<width;++index)
        bytes[index]=static_cast<unsigned char>(value>>(8*(width-1-index)));
    if (!is_toc) ++observation.output_write_attempts;
    const auto &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,uint32_t(address),bytes,width);
    if (observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    if (is_toc) observation.toc_written=true;
    else observation.output_written_bytes+=width;
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,uint32_t(address),width,value),batch);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
}

uint32_t copy_system_profile_string(StartupExecution &execution, uint64_t toc_slot)
{
    auto &observation=execution.control->system_string;
    require_system_profile_source(execution);
    SystemProfileField field;
    size_t required_size=0;
    const char *text=nullptr;
    switch (observation.id) {
    case system_string_user_name:
        field=SystemProfileField::CurrentUserDisplayName;
        text=execution.system_profile.current_user_display_name;
        required_size=sizeof execution.system_profile.current_user_display_name;
        break;
    case system_string_device_label:
        field=SystemProfileField::DeviceDisplayLabel;
        text=execution.system_profile.device_display_label;
        required_size=sizeof execution.system_profile.device_display_label;
        break;
    case 0x1008: case 0x1011: case 0x1012: case 0x1024:
        stop_system_string(execution,StartupSystemStringFailure::UnimplementedParameter);
    default:
        return system_string_value_error;
    }
    // The original wrapper checks the exact size before retrieving the selected field.
    if (observation.size!=required_size) return system_string_size_error;
    const auto validation=validate_system_profile_text(execution.system_profile,field);
    observation.text_checked=true;
    observation.profile_status=validation.status;
    if (validation.status!=SystemProfileStatus::Valid)
        stop_system_string(execution,StartupSystemStringFailure::InvalidProfile);
    observation.text_length=validation.length;
    // Native serialization order; the original strncpy promises padding, not this store order.
    for (size_t index=0;index<required_size-1;++index) {
        const uint8_t value=index<validation.length?static_cast<unsigned char>(text[index]):0;
        write_system_string_scalar(execution,uint64_t(observation.output)+index,1,value,toc_slot,false);
    }
    write_system_string_scalar(execution,uint64_t(observation.output)+required_size-1,1,0,toc_slot,false);
    return 0;
}

void dispatch_system_string(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.system_string;
    observation={};
    ++control.system_string_calls;
    observation.raw_id=cpu.gpr[3];observation.id=uint32_t(cpu.gpr[3]);
    observation.raw_output=cpu.gpr[4];observation.output=uint32_t(cpu.gpr[4]);
    observation.raw_size=cpu.gpr[5];observation.size=uint32_t(cpu.gpr[5]);
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution,toc_slot);
    const uint32_t result=observation.output?copy_system_profile_string(execution,toc_slot):system_string_value_error;
    write_system_string_scalar(execution,toc_slot,8,toc,toc_slot,true);
    observation.result=result;
    observation.has_result=true;
    cpu.gpr[3]=uint64_t(int64_t(int32_t(result)));
    ++control.system_string_returns;
}

constexpr uint32_t disc_change_registration_nid=0xdfdd302e;
void dispatch_disc_change_registration(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.disc_change;
    observation={};
    ++control.disc_change_calls;
    observation.raw_eject=cpu.gpr[3];observation.raw_insert=cpu.gpr[4];
    if (!execution.ppu.admitted || !execution.ppu.memory || !execution.disc_change_callbacks)
        stop(StartupStop::MissingServices);
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution,toc_slot);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    observation.registration_attempted=true;
    observation.registration=ps3_disc_change_register(execution.disc_change_callbacks,
        {uint32_t(cpu.gpr[3]),uint32_t(cpu.gpr[4])});
    // Registration is retained process state, independent of later TOC/SDK publication.
    check_boundary(BoundaryBudget::Poll,&batch);
    if (observation.registration.status!=PS3_DISC_CHANGE_OK || !observation.registration.committed)
        stop(StartupStop::DiscChangeStop,&batch);
    unsigned char bytes[8];
    for (size_t index=0;index<sizeof bytes;++index)
        bytes[index]=static_cast<unsigned char>(toc>>(8*(sizeof bytes-1-index)));
    const auto &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,uint32_t(toc_slot),bytes,sizeof bytes);
    if (observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    observation.toc_written=true;
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,uint32_t(toc_slot),8,toc),batch);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[3]=0;
    ++control.disc_change_returns;
}

constexpr uint32_t video_output_count_nid=0x75bbb672;
void dispatch_video_output_count(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_output_count;
    observation={};
    ++control.video_output_count_calls;
    observation.raw_video_out=cpu.gpr[3];
    observation.video_out=uint32_t(cpu.gpr[3]);
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    observation.topology=execution.presentation_topology;
    const auto inventory=presentation_inventory(observation.topology);
    observation.inventory_checked=true;
    observation.inventory_status=inventory.status;
    if (inventory.status!=PresentationInventoryStatus::Configured) {
        observation.failure=inventory.status==PresentationInventoryStatus::Unconfigured
            ?StartupVideoOutputCountFailure::UnconfiguredTopology:StartupVideoOutputCountFailure::InvalidTopology;
        stop(StartupStop::VideoOutputCountStop);
    }
    if (observation.video_out>1) {
        observation.failure=StartupVideoOutputCountFailure::UnsupportedSelector;
        stop(StartupStop::VideoOutputCountStop);
    }
    observation.count=observation.video_out==0?inventory.primary_count:inventory.secondary_count;
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution,toc_slot);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    unsigned char bytes[8];
    for (size_t index=0;index<sizeof bytes;++index)
        bytes[index]=static_cast<unsigned char>(toc>>(8*(sizeof bytes-1-index)));
    const auto &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,uint32_t(toc_slot),bytes,sizeof bytes);
    if (observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    observation.toc_written=true;
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,uint32_t(toc_slot),8,toc),batch);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[3]=uint64_t(int64_t(observation.count));
    ++control.video_output_count_returns;
}


constexpr uint32_t video_device_info_nid=0x1e930eef;
constexpr uint32_t video_availability_nid=0xa322db75;
constexpr uint32_t video_resolution_nid=0xe558748d;
constexpr uint32_t video_device_info_lr=0x00115980;
constexpr uint32_t video_resolution_null_error=0x8002b222;
constexpr size_t video_device_info_bytes=0x1c8;
// DeviceInfo layout consumed by the authenticated caller: count, resolutionId, aspect.
constexpr uint32_t video_device_info_count_offset=0x04;
constexpr uint32_t video_device_info_resolution_id_offset=0x20;
constexpr uint32_t video_device_info_aspect_offset=0x23;
struct VideoResolutionRow { uint8_t id; uint16_t width, height; };
// evidence/codex-video-resolution-contract-r1/RESOLUTION-TABLE.json (23 rows, immutable).
constexpr VideoResolutionRow video_resolution_table[]={
    {1,1920,1080},{2,1280,720},{4,720,480},{5,720,576},{10,1600,1080},{11,1440,1080},
    {12,1280,1080},{13,960,1080},{100,1360,768},{129,1280,1470},{130,1920,1080},
    {131,1920,2205},{139,640,1470},{138,800,1470},{137,960,1470},{136,1024,1470},
    {145,1280,1470},{146,1920,1080},{155,640,1470},{154,800,1470},{153,960,1470},
    {152,1024,1470},{161,1920,1080},
};
const VideoResolutionRow *find_video_resolution(uint32_t id)
{
    for (const auto &row:video_resolution_table) if (row.id==id) return &row;
    return nullptr;
}
void publish_video_scalar(StartupExecution &execution,uint32_t address,size_t width,uint64_t value,
    StartupVideoDeviceInfoObservation &observation,HleTraceBatch &batch)
{
    unsigned char bytes[8]{};
    for (size_t index=0;index<width;++index)
        bytes[index]=static_cast<unsigned char>(value>>(8*(width-1-index)));
    check_boundary(BoundaryBudget::Poll,&batch);
    const ps3_guest_memory &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,address,bytes,width);
    if(observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    // Count the committed store before trace publication can fail.
    observation.bytes_written+=width;
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,address,width,value),batch);
    ++observation.publications;
}
void dispatch_video_device_info(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_device_info;
    observation={};
    ++control.video_device_info_calls;
    observation.raw_video_out=cpu.gpr[3];observation.video_out=uint32_t(cpu.gpr[3]);
    observation.raw_index=cpu.gpr[4];observation.index=uint32_t(cpu.gpr[4]);
    observation.raw_output=cpu.gpr[5];observation.output=uint32_t(cpu.gpr[5]);
    observation.topology=execution.presentation_topology;
    observation.mode=execution.presentation_mode;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    observation.bridge_checked=true;
    const auto mode=presentation_mode_outcome(observation.mode);
    const auto inventory=presentation_inventory(observation.topology);
    if (inventory.status!=PresentationInventoryStatus::Configured) {
        observation.failure=StartupVideoBridgeFailure::UnsupportedTopology;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    if (mode.status!=NativePresentationModeStatus::Configured) {
        observation.failure=StartupVideoBridgeFailure::UnsupportedMode;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    if (cpu.lr!=video_device_info_lr) {
        observation.failure=StartupVideoBridgeFailure::WrongLr;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    if (observation.video_out!=0) {
        observation.failure=StartupVideoBridgeFailure::WrongSelector;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    if (observation.index!=0) {
        observation.failure=StartupVideoBridgeFailure::WrongIndex;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    const uint64_t sp=cpu.gpr[1];
    if (sp>UINT32_MAX || sp+0xD8>UINT32_MAX) stop(StartupStop::InvalidMemory);
    if (observation.output!=uint32_t(sp+0xD8)) {
        observation.failure=StartupVideoBridgeFailure::WrongOutput;
        stop(StartupStop::VideoDeviceInfoStop);
    }
    if (!writable(execution,observation.output,video_device_info_bytes)) {
        observation.failure=StartupVideoBridgeFailure::InvalidExtent;
        stop(StartupStop::InvalidMemory);
    }
    const uint64_t toc=cpu.gpr[2],toc_slot=sp+0x28;
    if (toc_slot<sp) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    preflight_toc(execution,toc_slot);
    if (!trace_can_begin(execution,4)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,4,batch);
    // Declared native bridge policy: three ordered one-byte stores at the authenticated
    // DeviceInfo layout offsets. No Sony memcpy order is claimed and every other byte is
    // left untouched.
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_video_scalar(execution,observation.output+video_device_info_count_offset,1,1,observation,batch);
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_video_scalar(execution,observation.output+video_device_info_resolution_id_offset,1,2,observation,batch);
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_video_scalar(execution,observation.output+video_device_info_aspect_offset,1,2,observation,batch);
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[2]=toc;
    cpu.gpr[3]=0;
    ++control.video_device_info_returns;
}
void dispatch_video_availability(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_availability;
    observation={};
    ++control.video_availability_calls;
    observation.raw_selector=cpu.gpr[3];observation.selector=uint32_t(cpu.gpr[3]);
    observation.raw_resolution_id=cpu.gpr[4];observation.resolution_id=uint32_t(cpu.gpr[4]);
    observation.raw_aspect=cpu.gpr[5];observation.aspect=uint32_t(cpu.gpr[5]);
    observation.raw_option=cpu.gpr[6];observation.option=uint32_t(cpu.gpr[6]);
    observation.topology=execution.presentation_topology;
    observation.mode=execution.presentation_mode;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const auto mode=presentation_mode_outcome(observation.mode);
    const auto inventory=presentation_inventory(observation.topology);
    if (inventory.status!=PresentationInventoryStatus::Configured) {
        observation.failure=StartupVideoBridgeFailure::UnsupportedTopology;
        stop(StartupStop::VideoAvailabilityStop);
    }
    if (mode.status!=NativePresentationModeStatus::Configured) {
        observation.failure=StartupVideoBridgeFailure::UnsupportedMode;
        stop(StartupStop::VideoAvailabilityStop);
    }
    if (observation.selector!=0) {
        observation.failure=StartupVideoBridgeFailure::WrongSelector;
        stop(StartupStop::VideoAvailabilityStop);
    }
    observation.available=(observation.resolution_id==2 && (observation.aspect==0 || observation.aspect==2))?1:0;
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    preflight_toc(execution,toc_slot);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[2]=toc;
    cpu.gpr[3]=uint64_t(int64_t(observation.available));
    ++control.video_availability_returns;
}
void publish_resolution_error(StartupExecution &execution,uint64_t toc,uint64_t toc_slot)
{
    auto &control=*execution.control;
    auto &observation=control.video_resolution;
    check_boundary(BoundaryBudget::Poll);
    if (!trace_can_begin(execution,1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    execution.ppu.cpu.gpr[2]=toc;
    execution.ppu.cpu.gpr[3]=uint64_t(int64_t(int32_t(observation.result)));
    ++control.video_resolution_returns;
}
void publish_resolution_scalar(StartupExecution &execution,uint32_t address,uint16_t value,bool &committed,
    size_t &writes,size_t &publications,ps3_guest_memory_status &status,HleTraceBatch &batch)
{
    const unsigned char bytes[2]{uint8_t(value>>8),uint8_t(value)};
    check_boundary(BoundaryBudget::Poll,&batch);
    const ps3_guest_memory &memory=execution.ppu.memory->coherent_memory;
    status=memory.write(memory.context,address,bytes,2);
    if(status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    // Record the committed store before trace publication can fail.
    committed=true;
    ++writes;
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,address,2,value),batch);
    ++publications;
}
void dispatch_video_resolution(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_resolution;
    observation={};
    ++control.video_resolution_calls;
    observation.raw_resolution_id=cpu.gpr[3];observation.resolution_id=uint32_t(cpu.gpr[3]);
    observation.raw_output=cpu.gpr[4];observation.output=uint32_t(cpu.gpr[4]);
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const uint64_t toc=cpu.gpr[2],toc_slot=cpu.gpr[1]+0x28;
    if (toc_slot<cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    preflight_toc(execution,toc_slot);
    // Full-64 NULL is the first output decision; no output access.
    if (cpu.gpr[4]==0) {
        observation.failure=StartupVideoBridgeFailure::NullOutput;
        observation.result=video_resolution_null_error;
        publish_resolution_error(execution,toc,toc_slot);
        return;
    }
    // An unknown low32 id is the original SDK error, before output-pointer validity/access.
    const VideoResolutionRow *row=find_video_resolution(observation.resolution_id);
    if (!row) {
        observation.failure=StartupVideoBridgeFailure::UnknownResolution;
        observation.result=video_resolution_null_error;
        publish_resolution_error(execution,toc,toc_slot);
        return;
    }
    observation.matched=true;observation.width=row->width;observation.height=row->height;
    observation.result=0;
    if (!trace_can_begin(execution,3)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,3,batch);
    const uint64_t output=cpu.gpr[4];
    // HEIGHT at full-64 output+2, admitted on its own before any store.
    if (output>UINT64_MAX-2) {
        observation.failure=StartupVideoBridgeFailure::InvalidPointer;
        stop(StartupStop::InvalidMemory,&batch);
    }
    const uint64_t height_address=output+2;
    if (height_address>UINT32_MAX || !writable(execution,uint32_t(height_address),2)) {
        observation.failure=StartupVideoBridgeFailure::InvalidPointer;
        stop(StartupStop::InvalidMemory,&batch);
    }
    publish_resolution_scalar(execution,uint32_t(height_address),uint16_t(row->height),
        observation.height_written,observation.writes,observation.publications,observation.memory_status,batch);
    // WIDTH at full-64 output, admitted only after the height store succeeded.
    if (output>UINT32_MAX || !writable(execution,uint32_t(output),2)) {
        observation.failure=StartupVideoBridgeFailure::InvalidPointer;
        stop(StartupStop::InvalidMemory,&batch);
    }
    publish_resolution_scalar(execution,uint32_t(output),uint16_t(row->width),
        observation.width_written,observation.writes,observation.publications,observation.memory_status,batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[2]=toc;cpu.gpr[3]=0;
    ++control.video_resolution_returns;
}
constexpr uint32_t video_state_nid=0x887572d5;
// Both admitted call sites of the two bound callers. func_00115870 (caller 1)
// reads only +8/+0x0B; func_003AA898 (caller 2) reads +8/+9/+0x0B. The fallback
// no-device site at LR 115D94 and every other caller stay native STOP.
constexpr uint32_t video_state_lr_first=0x00115a7c,video_state_lr_second=0x00115ae4;
constexpr uint32_t video_state_new_lr_first=0x003aa9c0,video_state_new_lr_second=0x003aab58;
constexpr uint32_t video_state_caller_func_00115870=1,video_state_caller_func_003AA898=2;
constexpr uint32_t video_state_output_offset_func_00115870=0xc8;
constexpr uint32_t video_state_output_offset_func_003AA898=0x7c;
constexpr size_t video_state_bytes=16;
constexpr uint32_t video_state_resolution_id_offset=0x08,video_state_scan_mode_offset=0x09,
    video_state_aspect_offset=0x0b;
constexpr uint8_t video_state_resolution_id=2;   // resolutionId 720p
constexpr uint8_t video_state_scan_mode_progressive=1;   // CELL_VIDEO_OUT_SCAN_MODE_PROGRESSIVE
constexpr uint8_t video_state_aspect=2;          // aspect 16:9
void publish_state_byte(StartupExecution &execution,uint32_t address,uint8_t value,
    StartupVideoStateObservation &observation,HleTraceBatch &batch)
{
    const unsigned char bytes[1]{value};
    check_boundary(BoundaryBudget::Poll,&batch);
    const ps3_guest_memory &memory=execution.ppu.memory->coherent_memory;
    observation.memory_status=memory.write(memory.context,address,bytes,1);
    if(observation.memory_status!=PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure,&batch);
    // Count the committed store before trace publication can fail.
    ++observation.bytes_written;
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_trace(execution,ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,true,address,1,value),batch);
    ++observation.publications;
}
void dispatch_video_state(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_state;
    observation={};
    ++control.video_state_calls;
    observation.raw_selector=cpu.gpr[3];observation.selector=uint32_t(cpu.gpr[3]);
    observation.raw_index=cpu.gpr[4];observation.index=uint32_t(cpu.gpr[4]);
    observation.raw_output=cpu.gpr[5];observation.output=uint32_t(cpu.gpr[5]);
    observation.lr=cpu.lr;
    observation.topology=execution.presentation_topology;
    observation.mode=execution.presentation_mode;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    observation.bridge_checked=true;
    const auto mode=presentation_mode_outcome(observation.mode);
    const auto inventory=presentation_inventory(observation.topology);
    if (inventory.status!=PresentationInventoryStatus::Configured) {
        observation.failure=StartupVideoStateFailure::UnsupportedTopology;
        stop(StartupStop::VideoStateStop);
    }
    if (mode.status!=NativePresentationModeStatus::Configured) {
        observation.failure=StartupVideoStateFailure::UnsupportedMode;
        stop(StartupStop::VideoStateStop);
    }
    uint32_t caller=0;
    if (cpu.lr==video_state_lr_first || cpu.lr==video_state_lr_second)
        caller=video_state_caller_func_00115870;
    else if (cpu.lr==video_state_new_lr_first || cpu.lr==video_state_new_lr_second)
        caller=video_state_caller_func_003AA898;
    if (caller==0) {
        observation.failure=StartupVideoStateFailure::WrongLr;
        stop(StartupStop::VideoStateStop);
    }
    observation.caller=caller;
    if (observation.selector!=0) {
        observation.failure=StartupVideoStateFailure::WrongSelector;
        stop(StartupStop::VideoStateStop);
    }
    if (observation.index!=0) {
        observation.failure=StartupVideoStateFailure::WrongIndex;
        stop(StartupStop::VideoStateStop);
    }
    const uint64_t sp=cpu.gpr[1];
    const uint32_t output_offset=caller==video_state_caller_func_00115870
        ? video_state_output_offset_func_00115870 : video_state_output_offset_func_003AA898;
    if (sp>UINT32_MAX || sp+output_offset>UINT32_MAX) stop(StartupStop::InvalidMemory);
    // Full-64 output must be exactly the caller's own bound record at SP+offset.
    if (cpu.gpr[5]!=sp+output_offset) {
        observation.failure=StartupVideoStateFailure::WrongOutput;
        stop(StartupStop::VideoStateStop);
    }
    if (!writable(execution,observation.output,video_state_bytes)) {
        observation.failure=StartupVideoStateFailure::InvalidExtent;
        stop(StartupStop::InvalidMemory);
    }
    const uint64_t toc=cpu.gpr[2],toc_slot=sp+0x28;
    if (toc_slot<sp) stop(StartupStop::InvalidMemory);
    const size_t state_trace_events=caller==video_state_caller_func_00115870 ? 3 : 4;
    check_boundary(BoundaryBudget::Poll);
    preflight_toc(execution,toc_slot);
    if (!trace_can_begin(execution,state_trace_events)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,state_trace_events,batch);
    // Declared native consumed-view policy: ordered one-byte stores at the
    // authenticated displayMode offsets. No SDK halfword-copy order, no physical
    // enabled/colorSpace/refresh claim and no caller-output clear are made; every
    // other byte of the bound record is left untouched.
    publish_state_byte(execution,observation.output+video_state_resolution_id_offset,
        video_state_resolution_id,observation,batch);
    check_boundary(BoundaryBudget::Poll,&batch);
    if (caller==video_state_caller_func_003AA898) {
        publish_state_byte(execution,observation.output+video_state_scan_mode_offset,
            video_state_scan_mode_progressive,observation,batch);
        check_boundary(BoundaryBudget::Poll,&batch);
    }
    publish_state_byte(execution,observation.output+video_state_aspect_offset,
        video_state_aspect,observation,batch);
    check_boundary(BoundaryBudget::Poll,&batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    cpu.gpr[2]=toc;
    cpu.gpr[3]=0;
    ++control.video_state_returns;
}
constexpr uint32_t video_configure_nid=0x0bae8772;
constexpr uint32_t video_configure_lr=0x003aab44;
constexpr size_t video_configure_record_bytes=16;
constexpr uint32_t video_configure_pitch=5120;
// A guest read is admitted when the whole span lies in any bound RAM range (writable or not).
bool video_guest_readable(const StartupExecution &execution,uint32_t address,uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
// cellVideoOutConfigure, consumed view bound to func_003AA898 at LR 0x003AAB44. The
// game configures its single logical 720p target: videoOut 0, a 16-byte displayMode
// record {resolutionId 2, format 0, aspect AUTO/16:9, reserved 0, pitch 0x1400},
// option NULL and waitForEvent 0. Anything else is a native STOP; success records the
// logical target and returns 0 with no guest store beyond the TOC.
void dispatch_video_configure(StartupExecution &execution)
{
    auto &control=*execution.control;
    auto &cpu=execution.ppu.cpu;
    auto &observation=control.video_configure;
    observation={};
    ++control.video_configure_calls;
    observation.lr=cpu.lr;
    observation.raw_r3=cpu.gpr[3];
    observation.raw_r4=cpu.gpr[4];
    observation.raw_r5=cpu.gpr[5];
    observation.raw_r6=cpu.gpr[6];
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const auto mode=presentation_mode_outcome(execution.presentation_mode);
    const auto inventory=presentation_inventory(execution.presentation_topology);
    if (inventory.status!=PresentationInventoryStatus::Configured) {
        observation.failure=StartupVideoConfigureFailure::UnsupportedTopology;
        stop(StartupStop::VideoConfigureStop);
    }
    if (mode.status!=NativePresentationModeStatus::Configured) {
        observation.failure=StartupVideoConfigureFailure::UnsupportedMode;
        stop(StartupStop::VideoConfigureStop);
    }
    if (cpu.lr!=video_configure_lr) {
        observation.failure=StartupVideoConfigureFailure::WrongLr;
        stop(StartupStop::VideoConfigureStop);
    }
    if (uint32_t(observation.raw_r3)!=0) {
        observation.failure=StartupVideoConfigureFailure::WrongVideoOut;
        stop(StartupStop::VideoConfigureStop);
    }
    if (observation.raw_r4>>32) {
        observation.failure=StartupVideoConfigureFailure::HighBits;
        stop(StartupStop::VideoConfigureStop);
    }
    const uint32_t record_ea=uint32_t(observation.raw_r4);
    if (!video_guest_readable(execution,record_ea,video_configure_record_bytes)) {
        observation.failure=StartupVideoConfigureFailure::InvalidExtent;
        stop(StartupStop::InvalidMemory);
    }
    const ps3_guest_memory &memory=execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context,record_ea,observation.record,video_configure_record_bytes)!=
        PS3_GUEST_MEMORY_OK) {
        observation.failure=StartupVideoConfigureFailure::InvalidExtent;
        stop(StartupStop::InvalidMemory);
    }
    const uint8_t *record=observation.record;
    bool configuration_ok=record[0]==2 && record[1]==0 &&
        (record[2]==0 || record[2]==2);
    for (size_t index=3;index<12;++index) configuration_ok=configuration_ok && record[index]==0;
    const uint32_t pitch=(uint32_t(record[12])<<24)|(uint32_t(record[13])<<16)|
        (uint32_t(record[14])<<8)|uint32_t(record[15]);
    configuration_ok=configuration_ok && pitch==video_configure_pitch;
    if (!configuration_ok) {
        observation.failure=StartupVideoConfigureFailure::UnsupportedConfiguration;
        stop(StartupStop::VideoConfigureStop);
    }
    if (observation.raw_r5!=0 || uint32_t(observation.raw_r6)!=0) {
        observation.failure=StartupVideoConfigureFailure::UnsupportedOption;
        stop(StartupStop::VideoConfigureStop);
    }
    const uint64_t sp=cpu.gpr[1],toc=cpu.gpr[2],toc_slot=sp+0x28;
    if (toc_slot<sp) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    preflight_toc(execution,toc_slot);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution,1,batch);
    publish_toc(execution,toc_slot,toc,batch,&observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result=true;
    control.video_logical_target_configured=true;
    cpu.gpr[2]=toc;
    cpu.gpr[3]=0;
    ++control.video_configure_returns;
}
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
// Pre-effect poll of the terminal STOP records published by owned kernel workers (no callback, no
// cross-thread longjmp): the PPU thread observes it here and stops itself before its next guest effect.
bool spurs_fence_pending(StartupControl &control)
{
    if (control.spurs_fence_latched) return true;
    if (!control.cpu) return false;
    auto &execution = *reinterpret_cast<StartupExecution *>(control.cpu);
    if (!execution.spurs || !ps3_spurs_runtime_poll_stop(execution.spurs, &control.spurs_fence)) return false;
    control.spurs_fence_latched = true;
    return true;
}
#endif
bool memory_cancelled(void *context)
{
    auto &control = *static_cast<StartupControl *>(context);
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    if (spurs_fence_pending(control)) return true;
#endif
    return control.cancelled.load(std::memory_order_acquire) ||
        (control.deadline_tick && armGetSystemTick() >= control.deadline_tick);
}
[[noreturn]] void memory_stopped(void *context, PpuMemoryFailure failure)
{
    auto &control = *static_cast<StartupControl *>(context);
    control.invalid_address = failure.address;
    if (&control != active) std::abort();
    if (failure.reason == PpuMemoryStop::Cancelled) {
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
        if (control.spurs_fence_latched) stop(spurs_fence_stop(control));
#endif
        if (control.cancelled.load(std::memory_order_acquire)) stop(StartupStop::Cancelled);
        stop(StartupStop::Deadline);
    }
    if (failure.reason == PpuMemoryStop::TraceCapacity) stop(StartupStop::TraceCapacity);
    if (failure.reason == PpuMemoryStop::BudgetStop) {
        if (!control.cpu) stop(StartupStop::BudgetInvalidInput);
        auto &owner = *reinterpret_cast<PpuMemoryExecution *>(control.cpu);
        if (!owner.budget) stop(StartupStop::BudgetInvalidInput);
        stop(budget_stop_reason(owner.budget->reason()));
    }
    stop(StartupStop::InvalidMemory);
}
}
bool native_startup_admit(StartupExecution &execution, PpuMemoryBinding &binding,
                          const Ps3PpuExecutionServices &services)
{
    if (execution.observation || execution.ppu.budget || execution.ppu.recorder || execution.ppu.admitted || !execution.control) return false;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (!services.compiled_code || !services.compiled_code->find ||
        !execution.dispatch_stop.stop_requested || !execution.dispatch.limit || execution.dispatch.depth)
        return false;
#endif
    const auto admitted = ppu_memory_admit(execution.ppu, binding, services,
        {execution.control, memory_cancelled, memory_stopped});
    if (admitted != PpuMemoryAdmission::Ready) return false;
    execution.control->cpu = &execution.ppu.cpu;
    execution.ppu.trace = &execution.control->journal;
    return true;
}
bool native_startup_admit_recorded(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder)
{
    if (execution.observation || execution.ppu.budget || execution.ppu.trace || execution.ppu.recorder || execution.ppu.admitted || !execution.control)
        return false;
    const auto state = recorder.report();
    if (!state.configured || state.batch_active) return false;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (!services.compiled_code || !services.compiled_code->find ||
        !execution.dispatch_stop.stop_requested || !execution.dispatch.limit || execution.dispatch.depth)
        return false;
#endif
    execution.ppu.recorder = &recorder;
    const auto admitted = ppu_memory_admit(execution.ppu, binding, services,
        {execution.control, memory_cancelled, memory_stopped});
    if (admitted != PpuMemoryAdmission::Ready) {
        execution.ppu.recorder = nullptr;
        return false;
    }
    execution.control->cpu = &execution.ppu.cpu;
    return true;
}
bool native_startup_admit_budgeted(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder, DiscoveryBudgetBinding &budget)
{
    if (execution.observation || execution.ppu.trace || execution.ppu.recorder || execution.ppu.budget ||
        execution.ppu.admitted || !execution.control) return false;
    const auto state = recorder.report();
    if (!state.configured || state.batch_active || !budget.valid_for_admission()) return false;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (!services.compiled_code || !services.compiled_code->find ||
        !execution.dispatch_stop.stop_requested || !execution.dispatch.limit || execution.dispatch.depth)
        return false;
#endif
    execution.ppu.recorder = &recorder;
    execution.ppu.budget = &budget;
    const auto admitted = ppu_memory_admit(execution.ppu, binding, services,
        {execution.control, memory_cancelled, memory_stopped});
    if (admitted != PpuMemoryAdmission::Ready) {
        execution.ppu.recorder = nullptr;
        execution.ppu.budget = nullptr;
        return false;
    }
    execution.control->cpu = &execution.ppu.cpu;
    return true;
}
bool native_startup_admit_discovery(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder, DiscoveryBudgetBinding &budget,
    infamous::diagnostics::StartupObservation &observation)
{
#ifndef PS3_NATIVE_INDIRECT_DISPATCH
    // Discovery requires the complete explicit dispatch capability. Other
    // admission routes retain their original feature-off behavior.
    (void)execution; (void)binding; (void)services;
    (void)recorder; (void)budget; (void)observation;
    return false;
#else
    if (execution.observation || !observation.valid_for_admission()) return false;
    if (execution.dispatch.limit < 8 || observation.report().pending_capacity != 8)
        return false;
    if (!native_startup_admit_budgeted(execution, binding, services, recorder, budget))
        return false;
    // Selection is published only after lower admission has completed. Neither
    // path observes storage concurrently with this exclusive composition step.
    execution.observation = &observation;
    return true;
#endif
}
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
// Releases the guest-created (sc128) queues recorded in the control ledger. The registry still owns SPURS queues.
// A queue the domain refuses to destroy (receiver registered) stays recorded and the release reports BUSY.
static bool release_guest_event_queues(StartupExecution &execution)
{
    StartupControl *control = execution.control;
    if (!control) return false;
    if (!control->event_queue_owned_count) return true;
    if (control->event_queue_owned_count > PS3_LV2_QUEUE_CAP ||
        !execution.event_queues || !execution.event_queues->initialized) return false;
    size_t kept = 0;
    for (size_t i = 0; i < control->event_queue_owned_count; ++i) {
        const uint32_t id = control->event_queue_owned_ids[i];
        const uint32_t rc = ps3_lv2_queue_destroy(execution.event_queues, id);
        if (rc != PS3_LV2_OK && rc != PS3_LV2_ESRCH) control->event_queue_owned_ids[kept++] = id;
    }
    control->event_queue_owned_count = kept;
    return kept == 0;
}
// Releases ONLY the guest-created (sc134) ports recorded in the control ledger; every other port (SPURS, native) is untouched.
// Destroy first; a connected port (EISCONN) is disconnected and then destroyed. An id the domain no longer knows (ESRCH) is
// dropped; ANY other error keeps exactly that id recorded for a later retry (the remaining ids are still released).
static bool release_guest_event_ports(StartupExecution &execution)
{
    StartupControl *control = execution.control;
    if (!control) return false;
    if (!control->event_port_owned_count) return true;
    if (control->event_port_owned_count > PS3_LV2_PORT_CAP ||
        !execution.event_queues || !execution.event_queues->initialized) return false;
    size_t kept = 0;
    for (size_t i = 0; i < control->event_port_owned_count; ++i) {
        const uint32_t id = control->event_port_owned_ids[i];
        uint32_t rc = ps3_lv2_port_destroy(execution.event_queues, id);
        if (rc == PS3_LV2_EISCONN) {
            rc = ps3_lv2_port_disconnect(execution.event_queues, id);
            if (rc == PS3_LV2_OK) rc = ps3_lv2_port_destroy(execution.event_queues, id);
        }
        if (rc != PS3_LV2_OK && rc != PS3_LV2_ESRCH) control->event_port_owned_ids[kept++] = id;
    }
    control->event_port_owned_count = kept;
    return kept == 0;
}
#endif
ps3_guest_memory_status native_startup_release_after_join(StartupExecution &execution)
{
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // Services/workers borrow guest memory, the domain and the quota: confirmed joins first. Then the guest-created ports BEFORE
    // the guest-created queues they may be connected to (a retained port keeps every queue untouched), then admission.
    if (execution.spurs && ps3_spurs_runtime_shutdown(execution.spurs) != 0) return PS3_GUEST_MEMORY_BUSY;
    if (!release_guest_event_ports(execution)) return PS3_GUEST_MEMORY_BUSY;
    if (!release_guest_event_queues(execution)) return PS3_GUEST_MEMORY_BUSY;
#endif
    return ppu_memory_release_after_join(execution.ppu);
}
extern "C" {
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
static uint32_t mutex_cell_result(ps3_lwmutex_result result, HleTraceBatch &batch)
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
        stop(StartupStop::MutexProviderFailure, &batch);
    }
}
static uint32_t invoke_mutex(StartupExecution *execution, uint32_t nid, HleTraceBatch &batch)
{
    const Ps3PpuExecutionServices *services = execution->ppu.services;
    ppu_context *cpu = &execution->ppu.cpu;
    if (!services || !services->memory || !services->memory->read || !services->memory->write ||
        !services->create_mutex || !services->lock_mutex || !services->try_mutex ||
        !services->unlock_mutex || !services->destroy_mutex) stop(StartupStop::MissingServices, &batch);
    active->mutex_nid = nid;
    // Guest pointers use the established low32 ABI. Probe the entire control
    // object before any operation; no raw base pointer is a memory capability.
    const uint32_t address = uint32_t(cpu->gpr[3]);
    if (address % 8 || !writable(*execution, address, PS3_LWMUTEX_BYTES)) return 0x8001000d;
    unsigned char object[PS3_LWMUTEX_BYTES];
    ps3_guest_memory_status checked = PS3_GUEST_MEMORY_OK;
#ifdef PS3_NATIVE_FAST_SYNC
    // Committed pages: the probe read below would succeed and its bytes are not used; skip the domain guard and the copy.
    if (!fast_sync_readable(address, PS3_LWMUTEX_BYTES))
#endif
    checked = services->memory->read(services->memory->context,
        address, object, sizeof(object));
    if (checked != PS3_GUEST_MEMORY_OK) {
        if (caller_pointer_failure(checked)) return 0x8001000d;
        active->mutex_failure = {PS3_LWMUTEX_MEMORY_FAILURE, checked, PS3_WAIT_READY};
        stop(StartupStop::MutexProviderFailure, &batch);
    }
    ps3_lwmutex_result result{};
    if (nid == 0x2f85c0ef) {
        unsigned char bytes[16];
        checked = services->memory->read(services->memory->context, uint32_t(cpu->gpr[4]), bytes, sizeof(bytes));
        if (checked != PS3_GUEST_MEMORY_OK) {
            if (caller_pointer_failure(checked)) return 0x8001000d;
            active->mutex_failure = {PS3_LWMUTEX_MEMORY_FAILURE, checked, PS3_WAIT_READY};
            stop(StartupStop::MutexProviderFailure, &batch);
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
            stop(StartupStop::MissingServices, &batch);
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
    check_boundary(BoundaryBudget::Poll, &batch);
    return mutex_cell_result(result, batch);
}
static void dispatch_mutex(StartupExecution *execution, uint32_t nid)
{
    HleTraceBatch batch;
    ppu_context *cpu = &execution->ppu.cpu;
    const uint64_t toc = cpu->gpr[2], slot = cpu->gpr[1] + 0x28;
    if (slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(*execution, slot);
    begin_trace(*execution, 1, batch);
    const uint32_t cell = invoke_mutex(execution, nid, batch);
    check_boundary(BoundaryBudget::LegacyOnly, &batch);
    publish_toc(*execution, slot, toc, batch);
    finish_owned(&batch);
    cpu->gpr[2] = toc;
    cpu->gpr[3] = uint64_t(int64_t(int32_t(cell)));
    ++active->mutex_returns;
}
#endif
#ifdef PS3_NATIVE_THREAD_ID_QUERY
static void dispatch_thread_id(StartupExecution &execution, ppu_context &cpu)
{
    HleTraceBatch batch;
    const uint64_t id = cpu.thread_id;
    if (!id || id >= uint64_t(PS3_LWMUTEX_RESERVED)) stop(StartupStop::MissingServices, &batch);
    const uint32_t output = uint32_t(cpu.gpr[3]);
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1] || !writable(execution, output, 8)) stop(StartupStop::InvalidMemory, &batch);
    if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity, &batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    uint64_t probe;
    if (memory.read(memory.context, output, &probe, sizeof(probe)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 2, batch);
    const uint64_t encoded = __builtin_bswap64(id);
    if (memory.write(memory.context, output, &encoded, sizeof(encoded)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution,
        ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, output, 8, id), batch);
    // A completed output remains visible if cancellation suppresses the return.
    check_boundary(BoundaryBudget::Poll, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
}
#endif
#ifdef PS3_NATIVE_STARTUP_LWCOND
static bool is_condition_import(uint32_t nid)
{
    return nid == 0xda0eb71a || nid == 0x1c9a942c || nid == 0xef87a695 ||
        nid == 0xe9a1bd84 || nid == 0x52aadadf || nid == 0x2a6d9d51;
}
struct ConditionStopContext {
    StartupExecution *execution;
    StartupStop reason = StartupStop::None;
};
static int condition_stop_requested(void *context)
{
    auto &request = *static_cast<ConditionStopContext *>(context);
    if (request.reason != StartupStop::None) return 1;
    auto &execution = *request.execution;
    auto &control = *execution.control;
    if (control.cancelled.load(std::memory_order_acquire)) request.reason = StartupStop::Cancelled;
    else if (control.deadline_tick && armGetSystemTick() >= control.deadline_tick)
        request.reason = StartupStop::Deadline;
    else if (execution.ppu.budget) {
        const auto reason = execution.ppu.budget->poll();
        if (reason != infamous::discovery::StopReason::None)
            request.reason = budget_stop_reason(reason);
    }
    return request.reason != StartupStop::None;
}
static uint32_t condition_cell_result(const ps3_lwcond_result &result,
    bool wait, HleTraceBatch &batch)
{
    if (result.cleanup_retained || result.must_not_resume)
        stop(StartupStop::ConditionProviderFailure, &batch);
    switch (result.status) {
    case PS3_LWCOND_OK:
        if (wait && !result.mutex_reacquired) break;
        return 0;
    case PS3_LWCOND_NOT_FOUND:
    case PS3_LWCOND_TARGET_NOT_FOUND: return 0x80010005;
    case PS3_LWCOND_NOT_OWNER:
    case PS3_LWCOND_TARGET_NOT_WAITING: return 0x80010009;
    case PS3_LWCOND_BUSY: return 0x8001000a;
    case PS3_LWCOND_RESOURCE_EXHAUSTED: return 0x80010001;
    case PS3_LWCOND_TIMED_OUT:
        if (wait && result.mutex_reacquired) return 0x8001000b;
        break;
    default: break;
    }
    stop(StartupStop::ConditionProviderFailure, &batch);
}
static bool condition_pointer(uint64_t address, size_t length)
{
    return address && address <= UINT32_MAX && length <= UINT64_C(0x100000000) - address;
}
static bool condition_overlaps_toc(uint32_t address, size_t length, uint64_t slot)
{
    return uint64_t(address) < slot + 8 && slot < uint64_t(address) + length;
}
static void dispatch_condition(StartupExecution &execution, uint32_t nid)
{
    HleTraceBatch batch;
    auto &cpu = execution.ppu.cpu;
    auto *registry = execution.conditions;
    if (!registry || !registry->initialized || !registry->mutexes ||
        !registry->mutexes->initialized || registry->mutexes->domain != execution.ppu.memory->domain)
        stop(StartupStop::MissingServices, &batch);
    const uint64_t condition_address = cpu.gpr[3];
    if (!condition_pointer(condition_address, 8) || condition_address % 4 ||
        !writable(execution, uint32_t(condition_address), 8))
        stop(StartupStop::InvalidMemory, &batch);
    const bool wait = nid == 0x2a6d9d51;
    size_t publication_credit = wait ? 3 : (nid == 0xe9a1bd84 ? registry->waiter_capacity : 1);
    if (publication_credit > PS3_SYNC_PUBLICATION_CAPACITY || publication_credit == SIZE_MAX)
        stop(StartupStop::MissingServices, &batch);
    const size_t credit = publication_credit + 1; // The import TOC return is separate.
    if (!trace_can_begin(execution, credit)) stop(StartupStop::TraceCapacity, &batch);
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1] || slot > UINT32_MAX) stop(StartupStop::InvalidMemory, &batch);
    if (condition_overlaps_toc(uint32_t(condition_address), 8, slot))
        stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    const auto &memory = execution.ppu.memory->coherent_memory;
    unsigned char probe[PS3_LWMUTEX_BYTES];
    if (memory.read(memory.context, uint32_t(condition_address), probe, 8) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    unsigned char name[PS3_LWCOND_NAME_BYTES]{};
    if (nid == 0xda0eb71a) {
        if (!condition_pointer(cpu.gpr[4], PS3_LWMUTEX_BYTES) || cpu.gpr[4] % 8 ||
            !writable(execution, uint32_t(cpu.gpr[4]), PS3_LWMUTEX_BYTES) ||
            !condition_pointer(cpu.gpr[5], sizeof(name)) ||
            condition_overlaps_toc(uint32_t(cpu.gpr[4]), PS3_LWMUTEX_BYTES, slot))
            stop(StartupStop::InvalidMemory, &batch);
        if (memory.read(memory.context, uint32_t(cpu.gpr[4]), probe, sizeof(probe)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::InvalidMemory, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        if (memory.read(memory.context, uint32_t(cpu.gpr[5]), name, sizeof(name)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::InvalidMemory, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
    }
    ps3_lwmutex_thread thread{};
    if (wait) {
        if (!execution.has_guest_priority || !cpu.thread_id || cpu.thread_id >= PS3_LWMUTEX_RESERVED)
            stop(StartupStop::MissingServices, &batch);
        thread = {uint32_t(cpu.thread_id), execution.guest_priority};
    }
    begin_trace(execution, credit, batch);
    auto &control = *execution.control;
    control.condition_nid = nid;
    control.condition_address = uint32_t(condition_address);
    if (control.condition_calls != SIZE_MAX) ++control.condition_calls;
    ps3_lwcond_report report{};
    ps3_lwcond_result result{};
    ConditionStopContext request{&execution};
    if (nid == 0xda0eb71a)
        result = ps3_lwcond_create(registry, uint32_t(condition_address), uint32_t(cpu.gpr[4]), name, &report);
    else if (nid == 0x1c9a942c)
        result = ps3_lwcond_destroy(registry, uint32_t(condition_address), &report);
    else if (nid == 0xef87a695)
        result = ps3_lwcond_signal(registry, uint32_t(condition_address), &report);
    else if (nid == 0xe9a1bd84)
        result = ps3_lwcond_signal_all(registry, uint32_t(condition_address), &report);
    else if (nid == 0x52aadadf)
        result = ps3_lwcond_signal_to(registry, uint32_t(condition_address), cpu.gpr[4], &report);
    else {
        /* C1b: a child PPU that sleeps here must release its creator's prefix wait, exactly like the queue
         * receive and sys_spu_thread_group_join do (see StartupExecution::lv2_blocked_word). The condition core
         * has no blocked-word hook, so the word is published around the whole wait. */
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE   // the lv2 registry and the blocked word exist only in this composition
        uint64_t *const wait_word = execution.event_queues ? execution.lv2_blocked_word : nullptr;
        if (wait_word) {
            __atomic_store_n(wait_word, 1, __ATOMIC_SEQ_CST);
            execution.event_queues->port.wake_all(execution.event_queues->port.context);
        }
#else
        uint64_t *const wait_word = nullptr;
#endif
        result = ps3_lwcond_wait(registry, uint32_t(condition_address), thread, cpu.gpr[4],
            {}, {&request, condition_stop_requested}, &report);
        if (wait_word) __atomic_store_n(wait_word, 0, __ATOMIC_SEQ_CST);
    }
    control.condition_result = result;
    control.condition_publications = report.publications.count;
    control.condition_selected = report.selected_count;
    if (report.publications.count > publication_credit) stop(StartupStop::ConditionProviderFailure, &batch);
    bool toc_collision = false;
    for (size_t index = 0; index < report.publications.count; ++index) {
        const auto &publication = report.publications.writes[index];
        if (!publication.size || publication.size > PS3_SYNC_PUBLICATION_BYTES ||
            !condition_pointer(publication.address, publication.size) ||
            !writable(execution, publication.address, publication.size))
            stop(StartupStop::ConditionProviderFailure, &batch);
        publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
            publication.address, publication.bytes, publication.size), batch);
        toc_collision = toc_collision || condition_overlaps_toc(publication.address, publication.size, slot);
    }
    if (toc_collision) stop(StartupStop::InvalidMemory, &batch);
    // The core has returned normally with all guards released. Its completed
    // writes remain evidence even when restoration or the ABI return fails.
    if (request.reason != StartupStop::None) stop(request.reason, &batch);
    if (nid == 0xe9a1bd84 && result.status != PS3_LWCOND_OK && report.selected_count)
        stop(StartupStop::ConditionProviderFailure, &batch);
    const uint32_t cell = condition_cell_result(result, wait, batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = uint64_t(int64_t(int32_t(cell)));
    if (control.condition_returns != SIZE_MAX) ++control.condition_returns;
}
#endif
#ifdef PS3_NATIVE_STARTUP_EXITSPAWN
static void dispatch_exitspawn(StartupExecution &execution, uint32_t nid)
{
    HleTraceBatch batch;
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.exitspawn_pending_trampoline_seen =
        control.exitspawn_pending_trampoline_seen || g_trampoline_fn != nullptr;
    auto *registry = execution.exitspawn_callbacks;
    if (!registry || !registry->initialized) stop(StartupStop::MissingServices, &batch);
    // Registration retains an opaque identity; no descriptor dereference,
    // alignment restriction, callback invocation or r4 interpretation occurs.
    if (cpu.gpr[3] > UINT32_MAX) stop(StartupStop::InvalidMemory, &batch);
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    control.exitspawn_kind = nid == 0x2c847572 ?
        PS3_EXITSPAWN_ATEXITSPAWN : PS3_EXITSPAWN_AT_EXITSPAWN;
    control.exitspawn_argument = cpu.gpr[3];
    if (control.exitspawn_calls != SIZE_MAX) ++control.exitspawn_calls;
    control.exitspawn_result = ps3_exitspawn_register(registry,
        control.exitspawn_kind, uint32_t(cpu.gpr[3]));
    if (control.exitspawn_result.status != PS3_EXITSPAWN_OK)
        stop(StartupStop::ExitspawnProviderFailure, &batch);
    // Successful registration survives cancellation or a failed TOC spill.
    // The registration guard is released before any terminal transfer.
    check_boundary(BoundaryBudget::Poll, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    if (control.exitspawn_returns != SIZE_MAX) ++control.exitspawn_returns;
    control.exitspawn_pending_trampoline_seen =
        control.exitspawn_pending_trampoline_seen || g_trampoline_fn != nullptr;
    // These imports are void: leave the entire input CPU unchanged.
}
#endif
#ifdef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
// cellSysmoduleInitialize (NID 0x63ff6ff9). Firmware behaviour: the libsysmodule export
// is a short stub that stores 0 into r3 and returns. It
// reads no TOC slot, sets no module state, takes no mutex and issues no
// syscall, so a repeated call returns 0 as well -- the firmware does not
// return CELL_SYSMODULE_ERROR_DUPLICATED (0x80012001) from this export.
// cellSysmoduleLoadModule's gate on the internal initialized flag lives in
// cellSysmoduleLoadModuleInternal and is not reachable from here.
static void dispatch_sysmodule_initialize(StartupExecution &execution)
{
    HleTraceBatch batch;
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    // No argument is inspected or narrowed: the firmware never reads r3.
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    control.sysmodule_initialize_second_call = control.sysmodule_initialize_calls != 0;
    if (control.sysmodule_initialize_calls != SIZE_MAX) ++control.sysmodule_initialize_calls;
    control.sysmodule_initialize_last_result = 0;
    check_boundary(BoundaryBudget::Poll, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0; // Firmware: r3 is set to 0.
    if (control.sysmodule_initialize_returns != SIZE_MAX) ++control.sysmodule_initialize_returns;
}
static void dispatch_diagnostic_module_admission(StartupExecution &execution)
{
    HleTraceBatch batch;
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.diagnostic_module_argument = cpu.gpr[3];
    control.diagnostic_module_has_result = false;
    control.diagnostic_module_pending_trampoline_seen =
        control.diagnostic_module_pending_trampoline_seen || g_trampoline_fn != nullptr;
    auto *model = execution.diagnostic_modules;
    if (!model || !model->initialized) stop(StartupStop::MissingServices, &batch);
    if (cpu.gpr[3] > UINT16_MAX) stop(StartupStop::DiagnosticModuleAdmissionFailure, &batch);
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1] || slot > UINT32_MAX) stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    if (control.diagnostic_module_calls != SIZE_MAX) ++control.diagnostic_module_calls;
    control.diagnostic_module_result = ps3_diagnostic_module_admit(model, uint16_t(cpu.gpr[3]));
    control.diagnostic_module_has_result = true;
    if (control.diagnostic_module_result.status != PS3_DIAGNOSTIC_MODULE_OK)
        stop(StartupStop::DiagnosticModuleAdmissionFailure, &batch);
    // This opt-in model records an assumption, not module/provider readiness.
    // Its increment survives a later cancellation or failed real TOC spill.
    check_boundary(BoundaryBudget::Poll, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0; // Explicit diagnostic assumption only; not real loader success.
    if (control.diagnostic_module_returns != SIZE_MAX) ++control.diagnostic_module_returns;
    control.diagnostic_module_pending_trampoline_seen =
        control.diagnostic_module_pending_trampoline_seen || g_trampoline_fn != nullptr;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
static bool sysutil_callback_canonical32(uint64_t value)
{
    const uint64_t high = value >> 32;
    return high == 0 || (high == UINT32_MAX && (uint32_t(value) & UINT32_C(0x80000000)));
}
static void dispatch_sysutil_callback_registration(StartupExecution &execution)
{
    HleTraceBatch batch;
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.sysutil_callback_slot = cpu.gpr[3];
    control.sysutil_callback_descriptor = cpu.gpr[4];
    control.sysutil_callback_userdata = cpu.gpr[5];
    control.sysutil_callback_has_result = false;
    control.sysutil_callback_invalid_argument = SysutilCallbackArgument::None;
    control.sysutil_callback_pending_trampoline_seen =
        control.sysutil_callback_pending_trampoline_seen || g_trampoline_fn != nullptr;
    auto *registry = execution.sysutil_callbacks;
    if (!registry || !registry->initialized) stop(StartupStop::MissingServices, &batch);
    if (!sysutil_callback_canonical32(cpu.gpr[3])) {
        control.sysutil_callback_invalid_argument = SysutilCallbackArgument::Slot;
        stop(StartupStop::SysutilCallbackFailure, &batch);
    }
    if (!sysutil_callback_canonical32(cpu.gpr[4])) {
        control.sysutil_callback_invalid_argument = SysutilCallbackArgument::Descriptor;
        stop(StartupStop::SysutilCallbackFailure, &batch);
    }
    if (!sysutil_callback_canonical32(cpu.gpr[5])) {
        control.sysutil_callback_invalid_argument = SysutilCallbackArgument::Userdata;
        stop(StartupStop::SysutilCallbackFailure, &batch);
    }
    const uint64_t toc = cpu.gpr[2], slot = cpu.gpr[1] + 0x28;
    if (slot < cpu.gpr[1] || slot > UINT32_MAX) stop(StartupStop::InvalidMemory, &batch);
    preflight_toc(execution, slot);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    if (control.sysutil_callback_calls != SIZE_MAX) ++control.sysutil_callback_calls;
    control.sysutil_callback_result = ps3_sysutil_callback_register(registry, uint32_t(cpu.gpr[3]),
        {uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5])});
    control.sysutil_callback_has_result = true;
    // Registration is host state, not a guest publication or an invocation.
    // The whole pair and its actual result survive a later terminal boundary.
    check_boundary(BoundaryBudget::Poll, &batch);
    uint64_t return_value = 0;
    if (control.sysutil_callback_result.status == PS3_SYSUTIL_CALLBACK_INVALID_SLOT)
        return_value = UINT64_C(0xffffffff8002b102); // Verified primary error_code marshalling.
    else if (control.sysutil_callback_result.status != PS3_SYSUTIL_CALLBACK_OK)
        stop(StartupStop::SysutilCallbackFailure, &batch);
    publish_toc(execution, slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[3] = return_value;
    if (control.sysutil_callback_returns != SIZE_MAX) ++control.sysutil_callback_returns;
    control.sysutil_callback_pending_trampoline_seen =
        control.sysutil_callback_pending_trampoline_seen || g_trampoline_fn != nullptr;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
enum : uint32_t {
    spurs_attribute_initialize_nid = 0x95180230,
    spurs_attribute_prefix_nid = 0x07529113,
    spurs_attribute_group_type_nid = 0xa839a4d9,
    spurs_attribute_printf_nid = 0x1051d134,
    spurs_attribute_container_nid = 0x82275c1c
};
static bool is_spurs_attribute_import(uint32_t nid)
{
    return nid == spurs_attribute_initialize_nid || nid == spurs_attribute_prefix_nid ||
        nid == spurs_attribute_group_type_nid || nid == spurs_attribute_printf_nid ||
        nid == spurs_attribute_container_nid;
}
[[noreturn]] static void spurs_attribute_invalid_memory(uint64_t address)
{
    active->invalid_address = address;
    stop(StartupStop::InvalidMemory);
}
static uint32_t spurs_attribute_field(uint32_t attribute, size_t offset, size_t length)
{
    const uint64_t address = uint64_t(attribute) + offset;
    if (address > UINT32_MAX || length > UINT64_C(0x100000000) - address)
        spurs_attribute_invalid_memory(address);
    return uint32_t(address);
}
static bool spurs_attribute_overlaps(uint32_t first, size_t first_size, uint32_t second, size_t second_size)
{
    return first_size && second_size && uint64_t(first) < uint64_t(second) + second_size &&
        uint64_t(second) < uint64_t(first) + first_size;
}
static void spurs_attribute_read(StartupExecution &execution, uint32_t address, void *output, size_t length)
{
    bool readable = false;
    if (length <= UINT64_C(0x100000000) - address) {
        const auto &binding = *execution.ppu.memory;
        for (size_t i = 0; i < binding.ram_count; ++i) {
            const auto &range = binding.ram[i];
            if (address >= range.address && uint64_t(address) - range.address <= range.length &&
                length <= range.length - (uint64_t(address) - range.address)) readable = true;
        }
    }
    if (!readable) spurs_attribute_invalid_memory(address);
    auto &observation = execution.control->spurs_attribute;
    const auto &memory = execution.ppu.memory->coherent_memory;
    observation.memory_status = memory.read(memory.context, address, output, length);
    observation.memory_checked = true;
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
}
struct SpursAttributeWrite {
    uint32_t address;
    const void *bytes;
    size_t length;
    bool scalar;
    uint64_t value;
};
static size_t spurs_attribute_publications(const SpursAttributeWrite &write)
{
    static_assert(PPU_MEMORY_TRACE_BYTE_CAPACITY > 0, "Byte events require storage");
    return write.scalar ? 1 : (write.length + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
}
static void spurs_attribute_write(StartupExecution &execution, const SpursAttributeWrite &write,
    bool attribute, bool final_attribute_write, HleTraceBatch &batch)
{
    auto &observation = execution.control->spurs_attribute;
    observation.last_write_address = write.address;
    observation.last_write_length = write.length;
    observation.last_write_is_toc = !attribute;
    observation.write_attempted = true;
    const auto &memory = execution.ppu.memory->coherent_memory;
    observation.memory_status = memory.write(memory.context, write.address, write.bytes, write.length);
    observation.memory_checked = true;
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    ++observation.successful_writes;
    if (attribute) {
        observation.attribute_bytes += write.length;
        observation.last_attribute_address = write.address;
        observation.last_attribute_length = write.length;
        std::memcpy(observation.last_attribute_bytes, write.bytes, write.length);
    }
    if (final_attribute_write) observation.has_result = true;
    if (write.scalar) {
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            write.address, write.length, write.value), batch);
        ++observation.publications;
    } else {
        for (size_t offset = 0; offset < write.length;) {
            PpuMemoryTraceEvent event{};
            event.origin = PpuMemoryTraceOrigin::Hle;
            event.operation = PpuMemoryTraceOperation::Write;
            event.address = write.address + uint32_t(offset);
            event.length = write.length - offset;
            if (event.length > PPU_MEMORY_TRACE_BYTE_CAPACITY) event.length = PPU_MEMORY_TRACE_BYTE_CAPACITY;
            event.byte_transfer = true;
            std::memcpy(event.bytes, static_cast<const unsigned char *>(write.bytes) + offset, event.length);
            publish_trace(execution, event, batch);
            ++observation.publications;
            offset += event.length;
        }
    }
    // Coherent provider guards have returned. Record the complete confirmed
    // write before cancellation can suppress later fields, TOC or r3.
    check_boundary(BoundaryBudget::Poll, &batch);
}
static void dispatch_spurs_attribute(StartupExecution &execution, uint32_t nid)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_attribute = {};
    auto &observation = control.spurs_attribute;
    observation.nid = nid;
    for (size_t i = 0; i < 7; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    if (control.spurs_attribute_calls != SIZE_MAX) ++control.spurs_attribute_calls;
    const uint32_t attribute = uint32_t(cpu.gpr[3]);
    const uint32_t prefix = uint32_t(cpu.gpr[4]), prefix_size = uint32_t(cpu.gpr[5]);
    const int32_t group_type = int32_t(uint32_t(cpu.gpr[4]));
    ps3_spurs_attribute_status status = nid == spurs_attribute_prefix_nid
        ? ps3_spurs_attribute_validate_prefix(attribute, prefix, prefix_size)
        : nid == spurs_attribute_group_type_nid
            ? ps3_spurs_attribute_validate_group_type(attribute, group_type)
            : ps3_spurs_attribute_validate(attribute);
    const uint64_t toc = cpu.gpr[2], toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1] || toc_slot > UINT32_MAX - 7)
        spurs_attribute_invalid_memory(toc_slot);
    unsigned char image[PS3_SPURS_ATTRIBUTE_SIZE]{}, prefix_bytes[PS3_SPURS_ATTRIBUTE_PREFIX_CAPACITY]{};
    uint32_t encoded_first = 0, encoded_second = 0;
    SpursAttributeWrite writes[2]{};
    size_t write_count = 0;
    if (status == PS3_SPURS_ATTRIBUTE_OK) {
        if (nid == spurs_attribute_initialize_nid) {
            const ps3_spurs_attribute_initialization arguments{uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5]),
                uint32_t(cpu.gpr[6]), int32_t(uint32_t(cpu.gpr[7])), int32_t(uint32_t(cpu.gpr[8])), uint32_t(cpu.gpr[9])};
            status = ps3_spurs_attribute_initialize(attribute, &arguments, image);
            spurs_attribute_field(attribute, 0, sizeof(image));
            writes[write_count++] = {attribute, image, sizeof(image), false, 0};
        } else if (nid == spurs_attribute_prefix_nid) {
            const uint32_t destination = spurs_attribute_field(attribute, PS3_SPURS_ATTRIBUTE_PREFIX, prefix_size);
            if (prefix_size) {
                spurs_attribute_field(prefix, 0, prefix_size);
                if (spurs_attribute_overlaps(prefix, prefix_size, destination, prefix_size) ||
                    spurs_attribute_overlaps(prefix, prefix_size, uint32_t(toc_slot), 8))
                    spurs_attribute_invalid_memory(prefix);
                writes[write_count++] = {destination, prefix_bytes, prefix_size, false, 0};
            }
            encoded_first = __builtin_bswap32(prefix_size);
            writes[write_count++] = {spurs_attribute_field(attribute, PS3_SPURS_ATTRIBUTE_PREFIX_SIZE, 4),
                &encoded_first, 4, true, prefix_size};
        } else {
            const uint32_t flags_address = spurs_attribute_field(attribute, PS3_SPURS_ATTRIBUTE_FLAGS, 4);
            writes[write_count++] = {flags_address, &encoded_first, 4, true, 0};
            if (nid == spurs_attribute_container_nid) {
                encoded_first = __builtin_bswap32(uint32_t(cpu.gpr[4]));
                writes[0] = {spurs_attribute_field(attribute, PS3_SPURS_ATTRIBUTE_CONTAINER, 4),
                    &encoded_first, 4, true, uint32_t(cpu.gpr[4])};
                writes[write_count++] = {flags_address, &encoded_second, 4, true, 0};
            }
        }
        for (size_t i = 0; i < write_count; ++i)
            if (spurs_attribute_overlaps(writes[i].address, writes[i].length, uint32_t(toc_slot), 8))
                spurs_attribute_invalid_memory(writes[i].address);
        if (nid == spurs_attribute_prefix_nid && prefix_size)
            spurs_attribute_read(execution, prefix, prefix_bytes, prefix_size);
        if (nid == spurs_attribute_group_type_nid || nid == spurs_attribute_printf_nid || nid == spurs_attribute_container_nid) {
            uint32_t encoded_flags = 0, updated = 0;
            spurs_attribute_read(execution, spurs_attribute_field(attribute, PS3_SPURS_ATTRIBUTE_FLAGS, 4), &encoded_flags, 4);
            const uint32_t flags = __builtin_bswap32(encoded_flags);
            if (nid == spurs_attribute_group_type_nid) status = ps3_spurs_attribute_group_flags(group_type, flags, &updated);
            else if (nid == spurs_attribute_container_nid) status = ps3_spurs_attribute_container_flags(flags, &updated);
            else updated = ps3_spurs_attribute_printf_flags(flags);
            if (nid == spurs_attribute_container_nid) encoded_second = __builtin_bswap32(updated);
            else encoded_first = __builtin_bswap32(updated);
            writes[write_count - 1].value = updated;
        }
    }
    if (status != PS3_SPURS_ATTRIBUTE_OK && status != PS3_SPURS_ATTRIBUTE_NULL &&
        status != PS3_SPURS_ATTRIBUTE_ALIGN && status != PS3_SPURS_ATTRIBUTE_INVAL && status != PS3_SPURS_ATTRIBUTE_STAT)
        stop(StartupStop::ProviderFailure);
    observation.result = uint32_t(status);
    if (status != PS3_SPURS_ATTRIBUTE_OK) { write_count = 0; observation.has_result = true; }
    size_t publications = 1;
    for (size_t i = 0; i < write_count; ++i) publications += spurs_attribute_publications(writes[i]);
    if (!trace_can_begin(execution, publications)) stop(StartupStop::TraceCapacity);
    unsigned char probe[PS3_SPURS_ATTRIBUTE_SIZE];
    for (size_t i = 0; i <= write_count; ++i) {
        const uint32_t address = i == write_count ? uint32_t(toc_slot) : writes[i].address;
        const size_t length = i == write_count ? 8 : writes[i].length;
        if (!writable(execution, address, length)) spurs_attribute_invalid_memory(address);
        spurs_attribute_read(execution, address, probe, length);
    }
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, publications, batch);
    for (size_t i = 0; i < write_count; ++i)
        spurs_attribute_write(execution, writes[i], true, i + 1 == write_count, batch);
    const uint64_t encoded_toc = __builtin_bswap64(toc);
    spurs_attribute_write(execution, {uint32_t(toc_slot), &encoded_toc, 8, true, toc}, false, false, batch);
    finish_owned(&batch);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = uint64_t(int64_t(int32_t(observation.result)));
    if (control.spurs_attribute_returns != SIZE_MAX) ++control.spurs_attribute_returns;
}
// ---- _cellSpursWorkloadAttributeInitialize (0xefeb2679) / cellSpursWorkloadAttributeSetName (0x4a5eab63) ----
// Narrow workload helpers: independent of INITIALIZE, execution.spurs and every spurs_attribute*/info/attach counter.
// Every effect flag is set at the actual successful operation BEFORE its publication; nothing is rolled back.
enum : uint32_t {
    spurs_workload_attribute_initialize_nid = 0xefeb2679,
    spurs_workload_attribute_set_name_nid = 0x4a5eab63
};
static bool spurs_workload_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory || length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
static uint64_t spurs_workload_be64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[i];
    return value;
}
// One traced 8-byte coherent read. `credits` is the publication count this call still needs in total (read + cheapest
// completion). Pre-effect refusals (unreadable, credits, cancel/deadline/fence/budget) perform nothing. `observed_bytes`,
// `observed_raw`, `observed_low32` and `performed` are updated at the actual successful read, before the publication.
static void spurs_workload_read(StartupExecution &execution, uint32_t address, size_t credits,
    unsigned char observed_bytes[8], uint64_t &observed_raw, uint32_t *observed_low32, bool &performed,
    size_t &publications, ps3_guest_memory_status &memory_status)
{
    if (!spurs_workload_readable(execution, address, 8)) spurs_attribute_invalid_memory(address);
    if (!trace_can_begin(execution, credits)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    unsigned char bytes[8]{};
    const auto &memory = execution.ppu.memory->coherent_memory;
    memory_status = memory.read(memory.context, address, bytes, sizeof(bytes));
    if (memory_status != PS3_GUEST_MEMORY_OK) {
        active->invalid_address = address;
        stop(StartupStop::InvalidMemory, &batch);
    }
    std::memcpy(observed_bytes, bytes, sizeof(bytes));
    observed_raw = spurs_workload_be64(bytes);
    if (observed_low32) *observed_low32 = uint32_t(observed_raw);
    performed = true;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, false, address, 8, observed_raw), batch);
    ++publications;
    finish_owned(&batch);
}
// TOC write: toc_written is set right after the coherent write returned OK, before the publication can fail.
static void spurs_workload_toc(StartupExecution &execution, uint32_t slot, uint64_t toc, HleTraceBatch &batch,
    bool &toc_written, size_t &publications, ps3_guest_memory_status &memory_status)
{
    const uint64_t encoded = __builtin_bswap64(toc);
    const auto &memory = execution.ppu.memory->coherent_memory;
    memory_status = memory.write(memory.context, slot, &encoded, sizeof(encoded));
    if (memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    toc_written = true;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, slot, 8, toc), batch);
    ++publications;
}
// SDK policy error return: one TOC credit, no attribute access. The return becomes observable only after the TOC publication.
extern "C++" {
template <typename Observation>
static void spurs_workload_sdk_error(StartupExecution &execution, Observation &observation, uint32_t toc_slot,
    uint64_t toc, uint32_t status, size_t &returns)
{
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    spurs_workload_toc(execution, toc_slot, toc, batch, observation.toc_written, observation.publications,
        observation.memory_status);
    finish_owned(&batch);
    observation.result = status;
    observation.has_result = true;
    execution.ppu.cpu.gpr[2] = toc;
    execution.ppu.cpu.gpr[3] = uint64_t(int64_t(int32_t(status)));
    if (returns != SIZE_MAX) ++returns;
}
}
static uint32_t spurs_workload_toc_slot(StartupExecution &execution)
{
    const uint64_t sp = execution.ppu.cpu.gpr[1], slot = sp + 0x28;
    if (slot < sp || slot > UINT32_MAX - 7 || !writable(execution, uint32_t(slot), 8))
        spurs_attribute_invalid_memory(slot);
    return uint32_t(slot);
}
static void dispatch_spurs_workload_attribute(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_workload_attribute = {};
    auto &observation = control.spurs_workload_attribute;
    for (size_t i = 0; i < 8; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    observation.sp = cpu.gpr[1];
    if (control.spurs_workload_attribute_calls != SIZE_MAX) ++control.spurs_workload_attribute_calls;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    // Binder argument decoding precedes the SDK body: raw r3..r10 narrow to low 32 bits except the full 64-bit data.
    const uint32_t attribute = uint32_t(cpu.gpr[3]), policy_module = uint32_t(cpu.gpr[6]);
    const uint32_t priority_pointer = uint32_t(cpu.gpr[9]);
    ps3_spurs_workload_attribute_request request{};
    request.attribute = attribute;
    request.revision = uint32_t(cpu.gpr[4]);
    request.sdk_version = uint32_t(cpu.gpr[5]);
    request.policy_module = policy_module;
    request.size = uint32_t(cpu.gpr[7]);
    request.data = cpu.gpr[8];
    request.min_contention = uint32_t(cpu.gpr[10]);
    // Ninth argument: BE64 at (SP+0x70)&~3, low 32 bits. Native policy: SP must be canonical and SP+0x70 must not wrap.
    if (observation.sp > UINT32_MAX - 0x70) spurs_attribute_invalid_memory(observation.sp + 0x70);
    observation.stack_slot = uint32_t(observation.sp + 0x70) & ~3u;
    unsigned char stack_bytes[8];
    spurs_workload_read(execution, observation.stack_slot, 2, stack_bytes, observation.stack_raw,
        &observation.max_contention, observation.stack_read, observation.publications, observation.memory_status);
    request.max_contention = observation.max_contention;
    ps3_spurs_workload_attribute_status status =
        ps3_spurs_workload_attribute_validate_pointers(attribute, policy_module);
    if (status == PS3_SPURS_WORKLOAD_ATTRIBUTE_OK) {
        // The policy module is never dereferenced. The priority snapshot is taken after pointer checks, before min == 0.
        observation.priority_address = priority_pointer;
        uint64_t priority_raw = 0;
        spurs_workload_read(execution, priority_pointer, 2, observation.priority, priority_raw, nullptr,
            observation.priority_read, observation.publications, observation.memory_status);
        std::memcpy(request.priority, observation.priority, sizeof(request.priority));
        status = ps3_spurs_workload_attribute_validate_contention(request.min_contention, request.priority);
    }
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK && status != PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL &&
        status != PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN && status != PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL)
        stop(StartupStop::ProviderFailure);
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, uint32_t(status), control.spurs_workload_attribute_returns);
        return;
    }
    // Success output preflight: nothing is written before every native policy below has passed.
    spurs_attribute_field(attribute, 0, PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE);
    if (!writable(execution, attribute, PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE) ||
        spurs_attribute_overlaps(attribute, PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE, toc_slot, 8))
        spurs_attribute_invalid_memory(attribute);
    unsigned char image[PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE];
    if (ps3_spurs_workload_attribute_build(&request, image) != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK)
        stop(StartupStop::ProviderFailure);
    constexpr size_t image_events = PS3_SPURS_WORKLOAD_ATTRIBUTE_SIZE / 8;
    if (!trace_can_begin(execution, image_events + 1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, image_events + 1, batch);
    const auto &memory = execution.ppu.memory->coherent_memory;
    observation.image_write_attempted = true;
    observation.memory_status = memory.write(memory.context, attribute, image, sizeof(image));
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    observation.image_written_bytes = sizeof(image);
    std::memcpy(observation.image, image, sizeof(image));
    for (size_t i = 0; i < image_events; ++i) {
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            attribute + uint32_t(i * 8), 8, spurs_workload_be64(image + i * 8)), batch);
        ++observation.publications;
    }
    check_boundary(BoundaryBudget::Poll, &batch);
    spurs_workload_toc(execution, toc_slot, toc, batch, observation.toc_written, observation.publications,
        observation.memory_status);
    finish_owned(&batch);
    observation.result = 0;
    observation.has_result = true;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    if (control.spurs_workload_attribute_returns != SIZE_MAX) ++control.spurs_workload_attribute_returns;
}
// Two 4-byte coherent stores (class then instance) with one TOC write; names are opaque numbers and never dereferenced.
static void spurs_workload_name_store(StartupExecution &execution, StartupSpursWorkloadNameObservation &observation,
    uint32_t address, const unsigned char *bytes, uint32_t value, bool instance, HleTraceBatch &batch)
{
    (instance ? observation.instance_write_attempted : observation.class_write_attempted) = true;
    const auto &memory = execution.ppu.memory->coherent_memory;
    observation.memory_status = memory.write(memory.context, address, bytes, 4);
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    (instance ? observation.instance_written : observation.class_written) = true;
    observation.written_bytes += 4;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, address, 4, value), batch);
    ++observation.publications;
    check_boundary(BoundaryBudget::Poll, &batch);
}
static void dispatch_spurs_workload_name(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_workload_name = {};
    auto &observation = control.spurs_workload_name;
    for (size_t i = 0; i < 3; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    observation.attribute = uint32_t(cpu.gpr[3]);
    observation.name_class = uint32_t(cpu.gpr[4]);
    observation.name_instance = uint32_t(cpu.gpr[5]);
    if (control.spurs_workload_name_calls != SIZE_MAX) ++control.spurs_workload_name_calls;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    unsigned char patch[PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE]{};
    const auto status = ps3_spurs_workload_attribute_name_patch(observation.attribute, observation.name_class,
        observation.name_instance, patch);
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK && status != PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL &&
        status != PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN) stop(StartupStop::ProviderFailure);
    if (status != PS3_SPURS_WORKLOAD_ATTRIBUTE_OK) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, uint32_t(status), control.spurs_workload_name_returns);
        return;
    }
    // Only the 8 bytes at attribute+0x28 are touched: no read of the attribute, the names or their strings.
    const uint32_t destination = spurs_attribute_field(observation.attribute,
        PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_CLASS_OFFSET, PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE);
    if (!writable(execution, destination, PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE) ||
        spurs_attribute_overlaps(destination, PS3_SPURS_WORKLOAD_ATTRIBUTE_NAME_PATCH_SIZE, toc_slot, 8))
        spurs_attribute_invalid_memory(destination);
    if (!trace_can_begin(execution, 3)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    observation.patch_address = destination;
    std::memcpy(observation.patch, patch, sizeof(patch));
    HleTraceBatch batch;
    begin_trace(execution, 3, batch);
    spurs_workload_name_store(execution, observation, destination, patch, observation.name_class, false, batch);
    spurs_workload_name_store(execution, observation, destination + 4, patch + 4, observation.name_instance, true, batch);
    spurs_workload_toc(execution, toc_slot, toc, batch, observation.toc_written, observation.publications,
        observation.memory_status);
    finish_owned(&batch);
    observation.result = 0;
    observation.has_result = true;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    if (control.spurs_workload_name_returns != SIZE_MAX) ++control.spurs_workload_name_returns;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
enum : uint32_t { spurs_initialize_nid = 0xaa6269a8 };
// Guest-write sink for the transaction: coherent domain write + HLE trace events. Never stops the
// thread (the transaction still owns live services); failure is reported and the bridge stops afterwards.
struct SpursWriter { StartupExecution *execution; bool failed; };
static int spurs_instance_write(void *context, uint32_t address, const void *bytes, size_t length)
{
    auto *writer = static_cast<SpursWriter *>(context);
    auto &execution = *writer->execution;
    auto &control = *execution.control;
    auto fail = [&](bool trace) { writer->failed = true; control.spurs_write_trace_failure |= trace; return -1; };
    const bool scalar = length == 1 || length == 2 || length == 4 || length == 8;
    const size_t events = scalar ? 1 : (length + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
    if (!writable(execution, address, length)) return fail(false);
    if (!trace_can_begin(execution, events)) return fail(true);
    const bool recorded = execution.ppu.recorder != nullptr;
    if (recorded && !execution.ppu.recorder->begin(events)) return fail(true);
    const bool written = ps3_atomic_write(execution.ppu.memory->domain, address, bytes, length) == PS3_GUEST_MEMORY_OK;
    bool published = written;
    const auto publish = [&](const PpuMemoryTraceEvent &event) {
        if (!published) return;
        if (recorded) execution.ppu.recorder->publish(event);
        else published = ppu_memory_trace_append(execution.control->journal, event);
    };
    if (written && scalar) {
        uint64_t value = 0;
        for (size_t i = 0; i < length; ++i) value = value << 8 | static_cast<const unsigned char *>(bytes)[i];
        publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, address, length, value));
    } else if (written) {
        for (size_t offset = 0; offset < length;) {
            PpuMemoryTraceEvent event{};
            event.origin = PpuMemoryTraceOrigin::Hle;
            event.operation = PpuMemoryTraceOperation::Write;
            event.address = address + uint32_t(offset);
            event.length = length - offset;
            if (event.length > PPU_MEMORY_TRACE_BYTE_CAPACITY) event.length = PPU_MEMORY_TRACE_BYTE_CAPACITY;
            event.byte_transfer = true;
            std::memcpy(event.bytes, static_cast<const unsigned char *>(bytes) + offset, event.length);
            publish(event);
            offset += event.length;
        }
    }
    if (recorded) execution.ppu.recorder->finish();
    if (!written || !published) return fail(written);
    ++control.spurs_initialize_writes;
    return 0;
}
// Sync registries adapter: the real lwmutex/lwcond registries own the guest images (sync/lwmutex.c create 24 B image,
// destroy CAS of the owner/waiters pair; sync/lwcond.c create/destroy 8 B publication, reported in
// ps3_lwcond_report::publications). They write the atomic domain directly, so the adapters give them the same trace
// contract SpursWriter gives the transaction's own writes, on the PPU thread only: (1) preflight one trace event
// BEFORE the registry may mutate guest memory (no stop here, the transaction owns live services: a refusal is an
// untouched guest plus the terminal-trace flag the dispatcher turns into StartupStop::TraceCapacity), (2) after the
// registry returned, publish exactly the bytes it wrote (lwcond: its own report; lwmutex: read back from the domain
// the span it wrote; nothing for a call that wrote nothing), (3) finish. While no transaction writer is installed
// (shutdown/teardown after the PPU thread joined) the guest is not running and the trace is closed: nothing is
// published. The SPU-agent kernel-1 worker never publishes here: the journal has one CPU-owner producer.
static SpursWriter *spurs_transaction_writer(StartupExecution &execution)
{
    if (!execution.spurs) return nullptr;
    const auto &port = execution.spurs->config.writer;
    return port.write == spurs_instance_write ? static_cast<SpursWriter *>(port.context) : nullptr;
}
static void spurs_sync_trace_failure(StartupExecution &execution, SpursWriter &writer)
{
    writer.failed = true;
    execution.control->spurs_write_trace_failure = true;
}
static bool spurs_sync_reserve(StartupExecution &execution, SpursWriter &writer)
{
    if (!trace_can_begin(execution, 1) || (execution.ppu.recorder && !execution.ppu.recorder->begin(1))) {
        spurs_sync_trace_failure(execution, writer);
        return false;
    }
    return true;
}
static void spurs_sync_publish(StartupExecution &execution, SpursWriter &writer, uint32_t address,
    const unsigned char *bytes, size_t length)
{
    if (!length || length > PPU_MEMORY_TRACE_BYTE_CAPACITY) { spurs_sync_trace_failure(execution, writer); return; }
    const auto event = ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true, address, bytes, length);
    if (execution.ppu.recorder) execution.ppu.recorder->publish(event);
    else if (!ppu_memory_trace_append(execution.control->journal, event)) spurs_sync_trace_failure(execution, writer);
}
static void spurs_sync_finish(StartupExecution &execution)
{
    if (execution.ppu.recorder) execution.ppu.recorder->finish();
}
static void spurs_sync_publish_report(StartupExecution &execution, SpursWriter &writer, const ps3_lwcond_report &report)
{
    for (size_t i = 0; i < report.publications.count; ++i) {
        const auto &write = report.publications.writes[i];
        if (i >= 1) spurs_sync_trace_failure(execution, writer);   // one event was credited; create/destroy publish one
        else spurs_sync_publish(execution, writer, write.address, write.bytes, write.size);
    }
}
static uint32_t spurs_sync_result(ps3_lwmutex_status status)
{
    return status == PS3_LWMUTEX_OK ? 0u : status == PS3_LWMUTEX_RESOURCE_EXHAUSTED ? uint32_t(PS3_LV2_EAGAIN) : uint32_t(PS3_LV2_PROVIDER_FAILURE);
}
// DIAGNOSTIC observation of the create callbacks (contract: StartupSpursSyncCreateRecord in the header). Each registry is read
// while holding ONLY its own guard (lock, copy scalars, unlock), never nested, never waiting/allocating/publishing under it, and
// before the real create runs. The counting mirrors the registries' own slot-selection rule (state != 0 active; inactive and
// generation != UINT64_MAX reusable) for diagnostics only; nothing here feeds admission, a status or a return code.
static StartupSpursSyncRegistrySnapshot spurs_sync_snapshot_mutexes(ps3_lwmutex_registry *registry, uint32_t address, bool overlap)
{
    StartupSpursSyncRegistrySnapshot snapshot;
    if (!registry || !registry->initialized || !registry->objects || !registry->guard.lock || !registry->guard.unlock) return snapshot;
    registry->guard.lock(registry->guard.context);
    snapshot.taken = true;
    snapshot.object_capacity = registry->object_capacity;
    snapshot.overlap_evaluated = overlap;
    for (size_t index = 0; index < registry->object_capacity; ++index) {
        const ps3_lwmutex_object &object = registry->objects[index];
        if (object.state) ++snapshot.active_objects;
        else if (object.generation == UINT64_MAX) ++snapshot.exhausted_generation_objects;
        else ++snapshot.reusable_objects;
        if (overlap && object.state && uint64_t(address) < uint64_t(object.address) + PS3_LWMUTEX_BYTES &&
            uint64_t(object.address) < uint64_t(address) + PS3_LWMUTEX_BYTES) ++snapshot.overlapping_objects;
    }
    snapshot.next_queue_id = registry->next_queue_id;
    snapshot.queue_id_available = registry->next_queue_id && registry->next_queue_id < PS3_LWMUTEX_RESERVED;
    snapshot.condition_binding_count = registry->condition_binding_count;
    snapshot.condition_binding_capacity = registry->condition_binding_capacity;
    registry->guard.unlock(registry->guard.context);
    return snapshot;
}
static StartupSpursSyncRegistrySnapshot spurs_sync_snapshot_conditions(ps3_lwcond_registry *registry, uint32_t address, bool overlap)
{
    StartupSpursSyncRegistrySnapshot snapshot;
    constexpr uint64_t guest_condition_image_bytes = 8;   // two BE u32 guest words
    if (!registry || !registry->initialized || !registry->objects || !registry->guard.lock || !registry->guard.unlock) return snapshot;
    registry->guard.lock(registry->guard.context);
    snapshot.taken = true;
    snapshot.object_capacity = registry->object_capacity;
    snapshot.overlap_evaluated = overlap;
    for (size_t index = 0; index < registry->object_capacity; ++index) {
        const ps3_lwcond_object &object = registry->objects[index];
        if (object.state) ++snapshot.active_objects;
        else if (object.generation == UINT64_MAX) ++snapshot.exhausted_generation_objects;
        else ++snapshot.reusable_objects;
        if (overlap && object.state && uint64_t(address) < uint64_t(object.address) + guest_condition_image_bytes && uint64_t(object.address) < uint64_t(address) + guest_condition_image_bytes)
            ++snapshot.overlapping_objects;
    }
    snapshot.next_queue_id = registry->next_queue_id;
    snapshot.queue_id_available = registry->next_queue_id && registry->next_queue_id < PS3_LWMUTEX_RESERVED;
    registry->guard.unlock(registry->guard.context);
    return snapshot;
}
// Single PPU writer. Every entry is counted; the first CAPACITY are retained; every failure (nonzero rc) is also kept as the
// last failure. Never allocates, never stops, never reports to the caller.
static void spurs_sync_record(StartupControl &control, StartupSpursSyncCreateRecord record, uint32_t mapped_rc)
{
    record.mapped_rc = mapped_rc;
    if (control.spurs_sync_create_calls != SIZE_MAX) ++control.spurs_sync_create_calls;
    record.sequence = control.spurs_sync_create_calls > UINT32_MAX ? UINT32_MAX : uint32_t(control.spurs_sync_create_calls);
    if (control.spurs_sync_create_retained < STARTUP_SPURS_SYNC_CREATE_CAPACITY) control.spurs_sync_create[control.spurs_sync_create_retained++] = record;
    else {
        control.spurs_sync_create_overflow = true;
        if (control.spurs_sync_create_dropped != SIZE_MAX) ++control.spurs_sync_create_dropped;
    }
    if (mapped_rc != 0) {
        control.spurs_sync_last_failed = record;
        control.spurs_sync_has_last_failed = true;
    }
}
static uint32_t spurs_lwmutex_create(void *context, uint32_t address, uint32_t protocol, uint32_t recursive)
{
    auto &execution = *static_cast<StartupExecution *>(context);
    SpursWriter *writer = spurs_transaction_writer(execution);
    StartupSpursSyncCreateRecord record;
    record.operation = StartupSpursSyncOperation::LwmutexCreate;
    record.address = address; record.protocol = protocol; record.recursive = recursive;
    if (writer && !spurs_sync_reserve(execution, *writer)) {
        spurs_sync_record(*execution.control, record, uint32_t(PS3_LV2_PROVIDER_FAILURE));   // refused before the registry was called
        return uint32_t(PS3_LV2_PROVIDER_FAILURE);
    }
    auto *mutexes = execution.conditions->mutexes;
    record.mutexes = spurs_sync_snapshot_mutexes(mutexes, address, true);   // separate, sequential, each under its own guard only
    record.conditions = spurs_sync_snapshot_conditions(execution.conditions, address, false);
    const auto result = ps3_lwmutex_create(mutexes, address, protocol, recursive);
    record.real_call_performed = true;
    record.native_status = int(result.status);
    record.memory_status = int(result.memory_status);
    const uint32_t rc = spurs_sync_result(result.status);
    spurs_sync_record(*execution.control, record, rc);
    if (writer) {
        if (result.status == PS3_LWMUTEX_OK) {   // the registry wrote the whole PS3_LWMUTEX_BYTES image
            unsigned char image[PS3_LWMUTEX_BYTES];
            if (ps3_atomic_read(mutexes->domain, address, image, sizeof image) == PS3_GUEST_MEMORY_OK)
                spurs_sync_publish(execution, *writer, address, image, sizeof image);
            else spurs_sync_trace_failure(execution, *writer);
        }
        spurs_sync_finish(execution);
    }
    return rc;
}
static uint32_t spurs_lwmutex_destroy(void *context, uint32_t address)
{
    auto &execution = *static_cast<StartupExecution *>(context);
    SpursWriter *writer = spurs_transaction_writer(execution);
    auto *mutexes = execution.conditions->mutexes;
    unsigned char pair_before[8]{}, pair_after[8]{};   // owner/waiters: the only span destroy writes
    if (writer) {
        if (!spurs_sync_reserve(execution, *writer)) return uint32_t(PS3_LV2_PROVIDER_FAILURE);
        if (ps3_atomic_read(mutexes->domain, address, pair_before, sizeof pair_before) != PS3_GUEST_MEMORY_OK) {
            spurs_sync_finish(execution);
            spurs_sync_trace_failure(execution, *writer);
            return uint32_t(PS3_LV2_PROVIDER_FAILURE);
        }
    }
    const auto result = ps3_lwmutex_destroy(mutexes, address);
    if (writer) {
        // Publish only a span that really changed (a destroy of an unpublished object releases the pin, writes nothing).
        if (result.status == PS3_LWMUTEX_OK) {
            if (ps3_atomic_read(mutexes->domain, address, pair_after, sizeof pair_after) != PS3_GUEST_MEMORY_OK)
                spurs_sync_trace_failure(execution, *writer);
            else if (std::memcmp(pair_before, pair_after, sizeof pair_after))
                spurs_sync_publish(execution, *writer, address, pair_after, sizeof pair_after);
        }
        spurs_sync_finish(execution);
    }
    return spurs_sync_result(result.status);
}
static uint32_t spurs_lwcond_create(void *context, uint32_t address, uint32_t mutex_address)
{
    auto &execution = *static_cast<StartupExecution *>(context);
    static const unsigned char name[PS3_LWCOND_NAME_BYTES] = {'_', 's', 'p', 'u', 'P', 'r', 'v', 0};
    SpursWriter *writer = spurs_transaction_writer(execution);
    StartupSpursSyncCreateRecord record;
    record.operation = StartupSpursSyncOperation::LwcondCreate;
    record.address = address; record.mutex_address = mutex_address;
    if (writer && !spurs_sync_reserve(execution, *writer)) {
        spurs_sync_record(*execution.control, record, uint32_t(PS3_LV2_PROVIDER_FAILURE));   // refused before the registry was called
        return uint32_t(PS3_LV2_PROVIDER_FAILURE);
    }
    record.mutexes = spurs_sync_snapshot_mutexes(execution.conditions->mutexes, address, false);   // separate, sequential, each under its own guard only
    record.conditions = spurs_sync_snapshot_conditions(execution.conditions, address, true);
    ps3_lwcond_report report{};
    const auto result = ps3_lwcond_create(execution.conditions, address, mutex_address, name, &report);
    record.real_call_performed = true;
    record.native_status = int(result.status);
    record.memory_status = int(result.memory_status);
    record.nested_mutex_status = int(result.mutex_status);
    record.publication_count = report.publications.count;
    const uint32_t rc = result.status == PS3_LWCOND_OK ? 0u : result.status == PS3_LWCOND_RESOURCE_EXHAUSTED ? uint32_t(PS3_LV2_EAGAIN) : uint32_t(PS3_LV2_PROVIDER_FAILURE);
    spurs_sync_record(*execution.control, record, rc);
    if (writer) {   // the report lists exactly the guest bytes the registry wrote, whatever the final status
        spurs_sync_publish_report(execution, *writer, report);
        spurs_sync_finish(execution);
    }
    return rc;
}
static uint32_t spurs_lwcond_destroy(void *context, uint32_t address)
{
    auto &execution = *static_cast<StartupExecution *>(context);
    SpursWriter *writer = spurs_transaction_writer(execution);
    if (writer && !spurs_sync_reserve(execution, *writer)) return uint32_t(PS3_LV2_PROVIDER_FAILURE);
    ps3_lwcond_report report{};
    const auto result = ps3_lwcond_destroy(execution.conditions, address, &report);
    if (writer) {
        spurs_sync_publish_report(execution, *writer, report);
        spurs_sync_finish(execution);
    }
    return result.status == PS3_LWCOND_OK ? 0u : uint32_t(PS3_LV2_PROVIDER_FAILURE);
}
// The composition builds ps3_spurs_runtime_config with these adapters (see spurs_bridge_sync_port/writer).
extern "C" ps3_spurs_sync_port native_startup_spurs_sync_port(StartupExecution *execution)
{
    return {execution, spurs_lwmutex_create, spurs_lwmutex_destroy, spurs_lwcond_create, spurs_lwcond_destroy};
}
extern "C++" {   // the surrounding region is extern "C"; these match the C++ declarations in the header
StartupQuotaSample startup_quota_sample(ps3_user_memory *memory)
{
    StartupQuotaSample sample;
    if (!memory) return sample;
    ps3_user_memory_info info{};
    sample.status = int(ps3_user_memory_query(memory, &info));
    if (sample.status == PS3_USER_MEMORY_OK) {
        sample.quota = info.quota_bytes; sample.used = info.used_bytes;
        sample.available = info.available_bytes; sample.allocations = info.allocation_count;
    }
    return sample;
}
static bool quota_sample_conserved(const StartupQuotaSample &s)
{
    return s.status == PS3_USER_MEMORY_OK && s.used <= s.quota && s.available == s.quota - s.used;
}
StartupSpursQuotaReport startup_spurs_quota_report(const StartupQuotaSample &before, const StartupQuotaSample &after)
{
    StartupSpursQuotaReport report;
    report.before = before;
    report.after = after;
    report.measured = quota_sample_conserved(before) && quota_sample_conserved(after) && before.quota == after.quota;
    if (!report.measured) return report;   // never derive a charge from an unmeasured or inconsistent pair
    report.prior_used = before.used;
    report.total_used = after.used;
    // The quota can only grow across initialize (SPURS never frees guest allocations); a drop is not a charge.
    report.charged = after.used >= before.used && before.available - after.available == after.used - before.used;
    if (report.charged) report.delta_charge = after.used - before.used;
    return report;
}
bool startup_spurs_quota_charge_is(const StartupSpursQuotaReport &report, uint64_t expected_delta)
{
    return report.measured && report.charged && report.delta_charge == expected_delta &&
           report.total_used == report.prior_used + report.delta_charge;
}
bool startup_spurs_attach_effective_sdk(const StartupProcessSdk &sdk, uint32_t *effective)
{
    if (!sdk.valid || !effective) return false;
    const int32_t value = int32_t(sdk.raw);   // the reference get_sdk_version() is an s32: -1 means "unset", other negatives are unmodelled
    if (value == -1) { *effective = STARTUP_SPURS_ATTACH_MAPPED_UNSET_SDK; return true; }
    if (value < 0) return false;
    *effective = uint32_t(value);
    return true;
}
}   // extern "C++"
static void dispatch_spurs_initialize(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    if (control.spurs_initialize_calls != SIZE_MAX) ++control.spurs_initialize_calls;
    for (size_t i = 0; i < 7; ++i) control.spurs_initialize_arguments[i] = cpu.gpr[3 + i];
    control.spurs_initialize_has_result = false;
    if (!execution.spurs || !execution.conditions || !execution.conditions->mutexes) stop(StartupStop::MissingServices);
    const uint32_t spurs = uint32_t(cpu.gpr[3]), attribute = uint32_t(cpu.gpr[4]);
    const uint64_t toc = cpu.gpr[2], toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1] || toc_slot > UINT32_MAX - 7) spurs_attribute_invalid_memory(toc_slot);
    if (!writable(execution, uint32_t(toc_slot), 8)) spurs_attribute_invalid_memory(toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    unsigned char image[PS3_SPURS_ATTRIBUTE_BYTES]{};
    const unsigned char *attribute_bytes = nullptr;
    if (attribute && attribute % 8 == 0) {   // S0.1/S0.2 answer first for NULL/misaligned; bytes only otherwise
        spurs_attribute_read(execution, attribute, image, sizeof image);
        attribute_bytes = image;
    }
    check_boundary(BoundaryBudget::Poll);
    SpursWriter writer{&execution, false};
    const ps3_spurs_request request{attribute, spurs, attribute_bytes};
    execution.spurs->config.writer = {&writer, spurs_instance_write};
    // Read-only quota observation of the SAME allocator the instance charges join; nullptr -> UNOBSERVED (no report).
    control.spurs_quota = {};
    const StartupQuotaSample quota_before = startup_quota_sample(execution.spurs_quota_memory);
    const ps3_spurs_result result = ps3_spurs_runtime_initialize(execution.spurs, &request);
    const StartupQuotaSample quota_after = startup_quota_sample(execution.spurs_quota_memory);
    execution.spurs->config.writer = {nullptr, nullptr};
    control.spurs_quota = startup_spurs_quota_report(quota_before, quota_after);
    control.spurs_initialize_result = result;
    control.spurs_initialize_has_result = true;
    // Real effects (and live services) are committed. Every stop from here leaves them owned for the
    // joined shutdown; nothing below calls back into the PPU thread from another thread.
    // A trace refusal (sink or sync adapter) is terminal whatever the outcome: effects stay owned for shutdown.
    if (writer.failed && control.spurs_write_trace_failure) stop(StartupStop::TraceCapacity);
    if (result.outcome == PS3_SPURS_OUTCOME_STOP) {
        if (result.reason == PS3_SPURS_STOP_KERNEL) {
            control.spurs_fence = {};
            control.spurs_fence.instance = result.instance;
            control.spurs_fence.reason = result.detail;
            control.spurs_fence.spu = result.kernel_spu;
            control.spurs_fence.step = result.kernel_step;
            control.spurs_fence_latched = true;
            stop(StartupStop::SpursKernelFence);
        }
        stop(StartupStop::SpursInitializeStop);
    }
    check_boundary(BoundaryBudget::Poll);   // cancel/deadline/kernel fence BEFORE the TOC write and r3
    const uint64_t encoded_toc = __builtin_bswap64(toc);
    if (spurs_instance_write(&writer, uint32_t(toc_slot), &encoded_toc, 8) != 0) stop(StartupStop::TraceCapacity);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = uint64_t(int64_t(int32_t(result.rc)));
    if (control.spurs_initialize_returns != SIZE_MAX) ++control.spurs_initialize_returns;
}
// cellSpursAttachLv2EventQueue (NID 0xb9bc6207) through ps3_spurs_runtime_attach_lv2_event_queue. The domain owns validation,
// the all-SPU binding, the ownership ledger, rollback and shutdown detach; this adapter owns only (1) the explicit process SDK,
// (2) the attach memory port composed over the SAME coherent atomic domain the other HLE writes use, (3) trace/cancel/budget
// preflight before EVERY performed guest effect and (4) the result/TOC publication. The callbacks run inside the domain call,
// possibly with the domain mid-transaction, so they NEVER stop()/longjmp/terminate: a refusal returns a bounded failure
// (nothing performed), latches the reason, and the dispatcher stops only after the domain has rolled back or retained.
// The callback context lives for the call only; the domain never retains it. No guard or memory pin is held across a callback.
enum : uint32_t { spurs_attach_nid = 0xb9bc6207 };
struct SpursAttachMemory {
    StartupExecution *execution;
    StartupStop latched = StartupStop::None;   // first refusal: cancel/deadline/budget/fence/invalid memory
    bool callback_error = false, trace_failure = false;
    uint32_t invalid_address = 0;
};
static bool spurs_attach_poll(SpursAttachMemory &ctx)   // non-stopping twin of check_boundary(Poll)
{
    if (ctx.latched != StartupStop::None) return false;
    StartupExecution &execution = *ctx.execution;
    StartupControl &control = *execution.control;
    if (spurs_fence_pending(control)) ctx.latched = spurs_fence_stop(control);
    else if (control.cancelled.load(std::memory_order_acquire)) ctx.latched = StartupStop::Cancelled;
    else if (control.deadline_tick && armGetSystemTick() >= control.deadline_tick) ctx.latched = StartupStop::Deadline;
    else if (execution.ppu.budget) {
        const auto reason = execution.ppu.budget->poll();
        if (reason != infamous::discovery::StopReason::None) ctx.latched = budget_stop_reason(reason);
    }
    return ctx.latched == StartupStop::None;
}
static bool spurs_attach_scalar(size_t length) { return length == 1 || length == 2 || length == 4 || length == 8; }
static bool spurs_attach_readable(const StartupExecution &execution, uint32_t address, size_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory || !length || length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
static int spurs_attach_refuse(SpursAttachMemory &ctx) { ctx.callback_error = true; return -1; }
static int spurs_attach_refuse_memory(SpursAttachMemory &ctx, uint32_t address)
{
    if (ctx.latched == StartupStop::None) { ctx.latched = StartupStop::InvalidMemory; ctx.invalid_address = address; }
    return spurs_attach_refuse(ctx);
}
// One credited publication per memory effect, taken BEFORE the effect; a refusal here is pre-effect (nothing performed).
static bool spurs_attach_reserve(SpursAttachMemory &ctx)
{
    StartupExecution &execution = *ctx.execution;
    if (!trace_can_begin(execution, 1) || (execution.ppu.recorder && !execution.ppu.recorder->begin(1))) {
        ctx.trace_failure = true;
        return false;
    }
    return true;
}
static void spurs_attach_publish(SpursAttachMemory &ctx, bool write, uint32_t address, size_t length, uint64_t value)
{
    StartupExecution &execution = *ctx.execution;
    const auto event = ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, write, address, length, value);
    if (execution.ppu.recorder) execution.ppu.recorder->publish(event);
    else if (!ppu_memory_trace_append(execution.control->journal, event)) ctx.trace_failure = true;
}
static void spurs_attach_close(SpursAttachMemory &ctx)
{
    if (ctx.execution->ppu.recorder) ctx.execution->ppu.recorder->finish();
}
static uint64_t spurs_attach_be(const void *bytes, size_t length)
{
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) value = value << 8 | static_cast<const unsigned char *>(bytes)[i];
    return value;
}
static int spurs_attach_read(void *context, uint32_t address, void *bytes, size_t length)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    if (!spurs_attach_scalar(length) || !spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    if (!spurs_attach_readable(execution, address, length)) return spurs_attach_refuse_memory(ctx, address);
    if (!spurs_attach_reserve(ctx)) return spurs_attach_refuse(ctx);
    const bool read = ps3_atomic_read(execution.ppu.memory->domain, address, bytes, length) == PS3_GUEST_MEMORY_OK;
    if (read) {
        ++execution.control->spurs_attach_reads;
        spurs_attach_publish(ctx, false, address, length, spurs_attach_be(bytes, length));
    }
    spurs_attach_close(ctx);
    return read ? 0 : spurs_attach_refuse_memory(ctx, address);
}
static int spurs_attach_write(void *context, uint32_t address, const void *bytes, size_t length)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    if (!spurs_attach_scalar(length) || !spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    if (!writable(execution, address, length)) return spurs_attach_refuse_memory(ctx, address);
    if (!spurs_attach_reserve(ctx)) return spurs_attach_refuse(ctx);
    const bool written = ps3_atomic_write(execution.ppu.memory->domain, address, bytes, length) == PS3_GUEST_MEMORY_OK;
    if (written) {
        ++execution.control->spurs_attach_writes;
        // The byte IS written: a journal refusal is latched (the dispatcher stops) but must not claim "nothing performed".
        spurs_attach_publish(ctx, true, address, length, spurs_attach_be(bytes, length));
    }
    spurs_attach_close(ctx);
    return written ? 0 : spurs_attach_refuse_memory(ctx, address);
}
static int spurs_attach_compare_exchange(void *context, uint32_t address, uint64_t expected, uint64_t desired,
    ps3_atomic_compare_result *result)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    *result = {};
    if (!spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    if (!writable(execution, address, 8)) return spurs_attach_refuse_memory(ctx, address);
    if (!spurs_attach_reserve(ctx)) return spurs_attach_refuse(ctx);
    ps3_atomic_compare_result observed{};
    const bool ok = ps3_atomic_compare_exchange64(execution.ppu.memory->domain, address, expected, desired, &observed) == PS3_GUEST_MEMORY_OK;
    ++execution.control->spurs_attach_cas_calls;
    if (ok) {
        *result = observed;   // the actual observed/exchanged are reported even when the post-commit trace fails
        if (observed.exchanged) {
            ++execution.control->spurs_attach_cas_committed;
            spurs_attach_publish(ctx, true, address, 8, desired);
        } else {
            // Failed CAS (contention): the domain retries from this observed value, so the load it actually performed is traced with the credit already held.
            spurs_attach_publish(ctx, false, address, 8, observed.observed);
        }
    }
    spurs_attach_close(ctx);
    if (!ok) return spurs_attach_refuse_memory(ctx, address);
    // exchanged == 1 with a nonzero return is the domain's "committed, then failed" signal: it keeps bindings and stops.
    return observed.exchanged && ctx.trace_failure ? spurs_attach_refuse(ctx) : 0;
}
enum class SpursAttachDisposition { PublishOk, PublishFailed, Stop };
struct SpursAttachOutcome { SpursAttachDisposition disposition; StartupStop stop; };
// Pure mapping of one finished domain call. Only a clean OK (committed callback returned 0) may ever reach the guest as 0.
static SpursAttachOutcome spurs_attach_classify(const ps3_spurs_result &result, const ps3_spurs_attach_info &info,
    const SpursAttachMemory &ctx)
{
    if (ctx.trace_failure) return {SpursAttachDisposition::Stop, StartupStop::TraceCapacity};
    if (ctx.latched != StartupStop::None) return {SpursAttachDisposition::Stop, ctx.latched};
    if (result.outcome == PS3_SPURS_OUTCOME_STOP)
        return {SpursAttachDisposition::Stop, result.reason == PS3_SPURS_STOP_KERNEL ? StartupStop::SpursKernelFence : StartupStop::SpursAttachStop};
    if (result.outcome == PS3_SPURS_OUTCOME_OK)
        return info.commit_status != 0 ? SpursAttachOutcome{SpursAttachDisposition::Stop, StartupStop::SpursAttachStop}
                                       : SpursAttachOutcome{SpursAttachDisposition::PublishOk, StartupStop::None};
    if (result.outcome == PS3_SPURS_OUTCOME_FAILED) return {SpursAttachDisposition::PublishFailed, StartupStop::None};
    return {SpursAttachDisposition::Stop, StartupStop::ProviderFailure};
}
static void dispatch_spurs_attach(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    if (control.spurs_attach_calls != SIZE_MAX) ++control.spurs_attach_calls;
    for (size_t i = 0; i < 6; ++i) control.spurs_attach_arguments[i] = cpu.gpr[3 + i];
    control.spurs_attach_has_result = false;
    // Every check below is BEFORE any effect. The SDK comes only from the immutable process configuration.
    if (!execution.spurs || !execution.ppu.memory || !execution.process_sdk.valid) stop(StartupStop::MissingServices);
    uint32_t sdk = 0;
    if (!startup_spurs_attach_effective_sdk(execution.process_sdk, &sdk)) stop(StartupStop::SpursAttachStop);
    control.spurs_attach_sdk = sdk;
    control.spurs_attach_has_sdk = true;
    const uint64_t toc = cpu.gpr[2], toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1] || toc_slot > UINT32_MAX - 7) spurs_attribute_invalid_memory(toc_slot);
    if (!writable(execution, uint32_t(toc_slot), 8)) spurs_attribute_invalid_memory(toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_attach_memory memory{&context, spurs_attach_read, spurs_attach_write, spurs_attach_compare_exchange};
    // r3 CellSpurs*, r4 queue (u32), r5 port (vm::ptr<u8>), r6 isDynamic (s32): each is the low 32 bits of its register.
    const ps3_spurs_attach_request request{uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5]),
        uint32_t(cpu.gpr[6]), sdk, STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS};
    ps3_spurs_attach_info info{};
    const ps3_spurs_result result = ps3_spurs_runtime_attach_lv2_event_queue(execution.spurs, &request, &memory, &info);
    control.spurs_attach_result = result;
    control.spurs_attach_info = info;
    control.spurs_attach_has_result = true;
    control.spurs_attach_callback_error = context.callback_error || context.trace_failure;
    control.spurs_attach_trace_failure = context.trace_failure;
    control.spurs_attach_invalid_address = context.invalid_address;
    control.spurs_attach_stop_latched = context.latched;
    // Effects (and the retained ledger) stay owned for the joined shutdown whatever follows.
    const SpursAttachOutcome outcome = spurs_attach_classify(result, info, context);
    if (outcome.disposition == SpursAttachDisposition::Stop) {
        if (outcome.stop == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        if (outcome.stop == StartupStop::SpursKernelFence && !control.spurs_fence_latched) {
            control.spurs_fence = {};
            control.spurs_fence.instance = result.instance;
            control.spurs_fence.reason = result.detail;
            control.spurs_fence.spu = result.kernel_spu;
            control.spurs_fence.step = result.kernel_step;
            control.spurs_fence_latched = true;
        }
        stop(outcome.stop);
    }
    check_boundary(BoundaryBudget::Poll);   // cancel/deadline/budget/fence AFTER the domain call, BEFORE the TOC write and r3
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    publish_toc(execution, toc_slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = outcome.disposition == SpursAttachDisposition::PublishOk ? 0 : uint64_t(int64_t(int32_t(result.rc)));
    if (control.spurs_attach_returns != SIZE_MAX) ++control.spurs_attach_returns;
}
enum : uint32_t { spurs_add_workload_nid = 0xc0158d8b };
constexpr unsigned STARTUP_SPURS_REGISTRATION_MAX_ATTEMPTS = 64;
static uint64_t spurs_registration_read(StartupExecution &execution, uint32_t attribute,
    uint32_t offset, size_t length)
{
    auto &observation = execution.control->spurs_registration;
    const uint32_t address = spurs_attribute_field(attribute, offset, length);
    if (!spurs_attach_readable(execution, address, length)) spurs_attribute_invalid_memory(address);
    if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    unsigned char bytes[8]{};
    observation.memory_status = ps3_atomic_read(execution.ppu.memory->domain, address, bytes, length);
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) {
        active->invalid_address = address;
        stop(StartupStop::InvalidMemory, &batch);
    }
    ++observation.attribute_reads;
    const uint64_t value = spurs_attach_be(bytes, length);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, false, address, length, value), batch);
    ++observation.publications;
    finish_owned(&batch);
    return value;
}
static int spurs_registration_poll(void *opaque)
{
    return spurs_attach_poll(*static_cast<SpursAttachMemory *>(opaque)) ? 1 : 0;
}
// Domain effects are already committed when observed. Never unwind across its registration guard;
// a publication refusal is latched and the dispatcher stops after the domain has released the guard.
static int spurs_registration_observe(void *opaque, const ps3_spurs_registration_event *event)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &observation = context.execution->control->spurs_registration;
    if (!event || !event->bytes || !event->length) return spurs_attach_refuse(context), 0;
    if (event->operation == PS3_SPURS_REGISTRATION_CONDITIONAL_LINE && !event->committed) return 1;
    const bool cas = event->operation == PS3_SPURS_REGISTRATION_COMPARE_EXCHANGE;
    const bool write = cas ? event->committed != 0 : event->operation != PS3_SPURS_REGISTRATION_READ;
    for (size_t offset = 0; offset < event->length; offset += 8) {
        const size_t length = event->length - offset < 8 ? event->length - offset : 8;
        if (!spurs_attach_reserve(context)) return 0;
        const uint64_t value = cas ? (event->committed ? event->desired : event->observed)
                                   : spurs_attach_be(event->bytes + offset, length);
        spurs_attach_publish(context, write, event->address + uint32_t(offset), length, value);
        spurs_attach_close(context);
        if (context.trace_failure) return 0;
        ++observation.publications;
    }
    return 1;
}
static void dispatch_spurs_registration(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_registration = {};
    auto &observation = control.spurs_registration;
    if (control.spurs_registration_calls != SIZE_MAX) ++control.spurs_registration_calls;
    for (size_t i = 0; i < 3; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    observation.attribute = uint32_t(cpu.gpr[5]);
    uint32_t sdk = 0;
    if (!observation.attribute) sdk = uint32_t(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_NULL);
    else if (observation.attribute & 7u) sdk = uint32_t(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_ALIGN);
    else {
        observation.revision = uint32_t(spurs_registration_read(execution, observation.attribute, 0, 4));
        if (observation.revision != 1) sdk = uint32_t(PS3_SPURS_WORKLOAD_ATTRIBUTE_POLICY_INVAL);
    }
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_registration_returns);
        return;
    }
    auto &request = observation.request;
    request.spurs = uint32_t(cpu.gpr[3]);
    request.wid_output = uint32_t(cpu.gpr[4]);
    request.policy_module = uint32_t(spurs_registration_read(execution, observation.attribute, 8, 4));
    request.size = uint32_t(spurs_registration_read(execution, observation.attribute, 12, 4));
    request.data = spurs_registration_read(execution, observation.attribute, 16, 8);
    const uint64_t priority = spurs_registration_read(execution, observation.attribute, 24, 8);
    for (size_t i = 0; i < 8; ++i) request.priority[i] = static_cast<unsigned char>(priority >> (56 - i * 8));
    request.min_contention = uint32_t(spurs_registration_read(execution, observation.attribute, 32, 4));
    request.max_contention = uint32_t(spurs_registration_read(execution, observation.attribute, 36, 4));
    request.name_class = uint32_t(spurs_registration_read(execution, observation.attribute, 40, 4));
    request.name_instance = uint32_t(spurs_registration_read(execution, observation.attribute, 44, 4));
    request.hook = uint32_t(spurs_registration_read(execution, observation.attribute, 48, 4));
    request.hook_argument = uint32_t(spurs_registration_read(execution, observation.attribute, 52, 4));
    if (!ps3_spurs_workload_registration_validate_request(&request, &sdk)) stop(StartupStop::ProviderFailure);
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_registration_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    if (!writable(execution, request.spurs, PS3_SPURS_REGISTRATION_PRIMARY_BYTES)) spurs_attribute_invalid_memory(request.spurs);
    if (!writable(execution, request.wid_output, 4)) spurs_attribute_invalid_memory(request.wid_output);
    if (spurs_attribute_overlaps(request.wid_output, 4, toc_slot, 8) ||
        spurs_attribute_overlaps(request.spurs, PS3_SPURS_REGISTRATION_PRIMARY_BYTES, toc_slot, 8))
        spurs_attribute_invalid_memory(toc_slot);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    ps3_spurs_runtime_register_workload(execution.spurs, &request, execution.ppu.reservation,
        STARTUP_SPURS_REGISTRATION_MAX_ATTEMPTS, &context, spurs_registration_poll,
        &context, spurs_registration_observe, &observation.domain_result);
    observation.has_domain_result = true;
    observation.trace_failure = context.trace_failure;
    observation.latched_stop = context.latched;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) stop(context.latched);
    if (context.callback_error || observation.domain_result.stop != PS3_SPURS_REGISTRATION_RUNNING ||
        !observation.domain_result.has_sdk_result) stop(StartupStop::SpursWorkloadStop);
    spurs_workload_sdk_error(execution, observation, toc_slot, toc,
        observation.domain_result.sdk_result, control.spurs_registration_returns);
}
enum : uint32_t { spurs_request_idle_nid = 0x182d9890 };
static int spurs_operation_permits(void *opaque, uint32_t address, size_t length, unsigned write)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &execution = *context.execution;
    const bool allowed = write ? writable(execution, address, length)
                               : spurs_attach_readable(execution, address, length);
    const uint32_t toc_slot = uint32_t(execution.ppu.cpu.gpr[1] + 0x28);
    if (!allowed || (write && spurs_attribute_overlaps(address, length, toc_slot, 8))) {
        spurs_attach_refuse_memory(context, address);
        return 0;
    }
    return 1;
}
static int spurs_idle_observe(void *opaque, const ps3_spurs_idle_event *event)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &observation = context.execution->control->spurs_idle;
    if (!event || !event->bytes || !spurs_attach_scalar(event->length) ||
        (event->operation != PS3_SPURS_IDLE_READ && event->operation != PS3_SPURS_IDLE_WRITE)) {
        spurs_attach_refuse(context);
        return 0;
    }
    if (!spurs_attach_reserve(context)) return 0;
    spurs_attach_publish(context, event->operation == PS3_SPURS_IDLE_WRITE, event->address,
        event->length, spurs_attach_be(event->bytes, event->length));
    spurs_attach_close(context);
    if (context.trace_failure) return 0;
    ++observation.publications;
    return 1;
}
static void dispatch_spurs_idle(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_idle = {};
    auto &observation = control.spurs_idle;
    if (control.spurs_idle_calls != SIZE_MAX) ++control.spurs_idle_calls;
    for (size_t i = 0; i < 3; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    observation.request = {uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5])};
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    uint32_t sdk = 0;
    if (!ps3_spurs_idle_request_validate_address(&observation.request, &sdk)) stop(StartupStop::ProviderFailure);
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_idle_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_idle_memory_scope memory_scope{&context, spurs_operation_permits};
    ps3_spurs_runtime_request_idle_spu(execution.spurs, &observation.request, memory_scope, &context,
        spurs_registration_poll, &context, spurs_idle_observe, &observation.domain_result);
    observation.has_domain_result = true;
    observation.trace_failure = context.trace_failure;
    observation.latched_stop = context.latched;
    observation.memory_status = observation.domain_result.memory_status;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) {
        if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(context.latched);
    }
    if (context.callback_error || observation.domain_result.stop != PS3_SPURS_IDLE_RUNNING ||
        !observation.domain_result.has_sdk_result) stop(StartupStop::SpursIdleStop);
    spurs_workload_sdk_error(execution, observation, toc_slot, toc,
        observation.domain_result.sdk_result, control.spurs_idle_returns);
}

/* ---- SPURS taskset setup block: the two pure struct-init entries ----------------------------
 *
 * _cellSpursTasksetAttributeInitialize (0x16394A4E) and
 * _cellSpursEventFlagInitialize (0x5EF96465) neither make a syscall nor start
 * SPU work: each validates its arguments and writes one big-endian struct into guest memory.
 *
 * The byte image and the result code come from the platform-free domain unit
 * (native/domain/spurs_taskset_init.c); this adapter owns only the guest ABI (which register holds
 * which argument), the readable/writable preflight through the existing helpers, the traced HLE
 * writes, the TOC publication, r3 and the counters.
 *
 * Order, matching the firmware and the same defensive order the other SPURS dispatches use:
 *   1. re-entrancy + TOC slot preflight (writable, 8 bytes, not overlapping the TOC itself);
 *   2. every guest READ the entry performs (the eight priority bytes; taskset+0x74 when the
 *      event-flag call has a non-NULL taskset) -- a bad address stops InvalidMemory, never a
 *      fabricated value;
 *   3. the domain's validation, which yields the firmware's own code and no image on refusal;
 *   4. every guest WRITE the entry performs (512 or 128 bytes), traced as one HLE publication per
 *      scalar field and one per byte-capacity chunk for the zero-filled remainder;
 *   5. r3 = the domain's code (0 on success, sign-extended on refusal) and r2 = the caller's TOC.
 * A refusal still writes NO guest byte, so the game sees exactly what the firmware's early return
 * would have left behind. */
enum : uint32_t {
    spurs_taskset_attribute_initialize_nid = 0x16394a4e,
    spurs_event_flag_initialize_nid = 0x5ef96465
};

/* One traced guest write of `length` big-endian bytes at `address`, charged to the trace budget
 * before it happens. `image` is the domain's buffer at `offset`. */
static void spurs_taskset_write_image(StartupExecution &execution, uint32_t address,
    const unsigned char *image, size_t offset, size_t length, size_t &written)
{
    if (!writable(execution, address, length)) {
        execution.control->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
    const size_t publications = length == 1 || length == 2 || length == 4 || length == 8 ? 1
        : (length + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
    if (!trace_can_begin(execution, publications)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, publications, batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, address, image + offset, length) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    if (length == 1 || length == 2 || length == 4 || length == 8) {
        uint64_t value = 0;
        for (size_t index = 0; index < length; ++index)
            value = value << 8 | image[offset + index];
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            address, length, value), batch);
    } else {
        for (size_t done = 0; done < length;) {
            const size_t chunk = length - done > PPU_MEMORY_TRACE_BYTE_CAPACITY
                ? PPU_MEMORY_TRACE_BYTE_CAPACITY : length - done;
            publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
                address + uint32_t(done), image + offset + done, chunk), batch);
            done += chunk;
        }
    }
    finish_owned(&batch);
    written += length;
}

/* Reads `length` guest bytes at `address`, or stops InvalidMemory with the faulting address. */
static void spurs_taskset_read_guest(StartupExecution &execution, uint32_t address,
    unsigned char *bytes, size_t length)
{
    if (length > UINT64_C(0x100000000) - address || !spurs_attach_readable(execution, address, length)) {
        execution.control->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) value = value << 8 | bytes[index];
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, false, address,
        length, value), batch);
    finish_owned(&batch);
}

/* Publishes the TOC slot, sets r2 and r3, and counts the call. Shared by both entries. */
static void spurs_taskset_return(StartupExecution &execution, uint64_t toc, uint64_t toc_slot,
    uint32_t code, size_t &returns)
{
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    publish_toc(execution, toc_slot, toc, batch);
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    execution.ppu.cpu.gpr[2] = toc;
    execution.ppu.cpu.gpr[3] = ps3_spurs_taskset_init_sign_extend(code);
    if (returns != SIZE_MAX) ++returns;
}

/* _cellSpursTasksetAttributeInitialize (NID 0x16394A4E). Arguments per CONTRACT §A.1:
 *   r3 attribute, r4 revision, r5 sdk_version, r6 args (u64), r7 priority EA, r8 max_contention.
 * The continuation at 0x14480 reads eight bytes at r7 and memcpys them to attribute+0x10, so the
 * adapter reads those eight bytes and hands the domain the packed value. */
static void dispatch_spurs_taskset_attribute_initialize(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.taskset_attr_init_calls != SIZE_MAX) ++control.taskset_attr_init_calls;
    control.taskset_attr_init_last = 0xFFFFFFFFu;
    control.taskset_attr_init_writes = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    spurs_taskset_attribute_request request{};
    request.attribute = uint32_t(cpu.gpr[3]);
    request.revision = uint32_t(cpu.gpr[4]);
    request.sdk_version = uint32_t(cpu.gpr[5]);
    request.args = cpu.gpr[6];
    request.priority_value = 0;
    request.max_contention = uint32_t(cpu.gpr[8]);
    /* The firmware reads the eight priority bytes only after the NULL and alignment tests
     * (0x14494 follows 0x14490), so a refused pointer must not fault here first. */
    const bool reads_priority = request.attribute != 0u &&
        (request.attribute & (PS3_SPURS_TASKSET_ATTRIBUTE_ALIGN - 1u)) == 0u;
    if (reads_priority) {
        unsigned char priority[PS3_SPURS_TASKSET_PRIORITY_BYTES]{};
        spurs_taskset_read_guest(execution, uint32_t(cpu.gpr[7]), priority, sizeof(priority));
        for (size_t index = 0; index < sizeof(priority); ++index)
            request.priority_value = request.priority_value << 8 | priority[index];
    }

    unsigned char image[PS3_SPURS_TASKSET_ATTRIBUTE_BYTES]{};
    uint32_t code = 0;
    const spurs_taskset_init_status status = ps3_spurs_taskset_attribute_init(&request, image,
        sizeof(image), &code);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT) stop(StartupStop::ProviderFailure);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_OK) {
        const uint32_t base = request.attribute;
        /* The firmware's write order: memset 512, then the six fields. The traced
         * publications walk the same spans so a trace reader sees the same addresses. */
        spurs_taskset_write_image(execution, base, image, 0, PS3_SPURS_TASKSET_ATTRIBUTE_BYTES,
            control.taskset_attr_init_writes);
    }
    control.taskset_attr_init_last = code;
    spurs_taskset_return(execution, toc, toc_slot, code, control.taskset_attr_init_returns);
}

/* _cellSpursEventFlagInitialize (NID 0x5EF96465). Arguments per CONTRACT §B.1:
 *   r3 spurs, r4 taskset, r5 eventFlag, r6 flagClearMode, r7 flagDirection.
 * The firmware reads taskset+0x74 (the workload id) only when taskset != 0, and the game
 * passes taskset = 0, so the read is skipped on the path inFAMOUS takes. */
static void dispatch_spurs_event_flag_initialize(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.event_flag_init_calls != SIZE_MAX) ++control.event_flag_init_calls;
    control.event_flag_init_last = 0xFFFFFFFFu;
    control.event_flag_init_writes = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    spurs_event_flag_request request{};
    request.spurs = uint32_t(cpu.gpr[3]);
    request.taskset = uint32_t(cpu.gpr[4]);
    request.event_flag = uint32_t(cpu.gpr[5]);
    request.clear_mode = uint32_t(cpu.gpr[6]);
    request.direction = uint32_t(cpu.gpr[7]);
    request.taskset_wid = 0;
    /* 0x1584C reads taskset+0x74 after the NULL tests, after the three
     * alignment tests and only on the non-NULL branch, so a call
     * refused for any of those reasons must not fault on this read first. */
    const uint32_t align_mask = PS3_SPURS_EVENT_FLAG_ALIGN - 1u;
    const bool reads_wid = request.taskset != 0u && request.event_flag != 0u &&
        (request.spurs & align_mask) == 0u && (request.taskset & align_mask) == 0u;
    if (reads_wid) {
        unsigned char wid[4]{};
        spurs_taskset_read_guest(execution, request.taskset + PS3_SPURS_TASKSET_WID_OFFSET, wid,
            sizeof(wid));
        request.taskset_wid = uint32_t(wid[0]) << 24 | uint32_t(wid[1]) << 16 |
            uint32_t(wid[2]) << 8 | uint32_t(wid[3]);
    }

    unsigned char image[PS3_SPURS_EVENT_FLAG_BYTES]{};
    uint32_t code = 0;
    const spurs_taskset_init_status status = ps3_spurs_event_flag_init(&request, image,
        sizeof(image), &code);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT) stop(StartupStop::ProviderFailure);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_OK)
        spurs_taskset_write_image(execution, request.event_flag, image, 0,
            PS3_SPURS_EVENT_FLAG_BYTES, control.event_flag_init_writes);
    control.event_flag_init_last = code;
    spurs_taskset_return(execution, toc, toc_slot, code, control.event_flag_init_returns);
}

/* ---- the next two entries of the same taskset setup block -----------------------------------
 *
 *   taskset attribute name setter NID 0x652B70E2
 *   event-flag attach to an lv2 queue NID 0x87630976
 *
 * Contract: the name setter (g11) and the attach (g12, which corrects g11's
 * "failure-path only" reading: the game code 0x000F2E54 test is `EQ` on r3 == 0, so this import
 * runs on the SUCCESS path of _cellSpursEventFlagInitialize).
 *
 * The name setter is the import 0.24.44 stopped on, and it runs on the SUCCESS path of
 * _cellSpursTasksetAttributeInitialize (game code 0x000F2D74 branches to loc_000F2F80 when r3 == 0).
 * It is a leaf: it tests its two arguments, then stores the name POINTER at attribute+0x1C
 * It reads no guest byte at all -- it does not dereference the name
 * so the adapter must not read the string either.
 *
 * The attach is reached right after the event-flag init succeeds (r3 = 0x5880, direction 1,
 * isIwl 1; g12 §0). Its four argument gates are served exactly, with the firmware's own codes.
 * The passage past them is the libsre 0xB244 helper, whose first act is lv2 128
 * (sys_event_queue_create) with an attribute packet copied from libsre's private static
 * `*(r2-0x7BF0)` and `r6 = 1` in the syscall's `type` slot -- not the native `size` (ring depth).
 * Nothing the native lv2 registry can read reproduces that packet, and the firmware's own success
 * return is additionally the defect 0x80410910 (r31 is never cleared; g12 §F.2), so the entry
 * stops MissingServices past the gates instead of creating a queue whose identity the native
 * layer cannot justify (g12 §E.1, §F.1). That is the honest stop the contract names, not an
 * approximation. */
enum : uint32_t {
    spurs_taskset_attribute_set_name_nid = 0x652b70e2,
    spurs_event_flag_attach_nid = 0x87630976
};

/* 0x652B70E2 (NID 0x652B70E2). Arguments per CONTRACT §A.1:
 *   r3 attribute, r4 name pointer.
 * No guest read; one 4-byte traced write at attribute+0x1C on success. */
static void dispatch_spurs_taskset_attribute_set_name(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.taskset_name_calls != SIZE_MAX) ++control.taskset_name_calls;
    control.taskset_name_last = 0xFFFFFFFFu;
    control.taskset_name_writes = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    spurs_taskset_attribute_name_request request{};
    request.attribute = uint32_t(cpu.gpr[3]);
    request.name = uint32_t(cpu.gpr[4]);

    uint8_t image[PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES]{};
    uint32_t code = 0;
    const spurs_taskset_init_status status = ps3_spurs_taskset_attribute_set_name(&request, image,
        sizeof(image), &code);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT) stop(StartupStop::ProviderFailure);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_OK)
        /* the one write, exactly 4 bytes. */
        spurs_taskset_write_image(execution, request.attribute + PS3_SPURS_TSATTR_NAME, image, 0,
            PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES, control.taskset_name_writes);
    control.taskset_name_last = code;
    spurs_taskset_return(execution, toc, toc_slot, code, control.taskset_name_returns);
}

/* 0x87630976 (NID 0x87630976). Arguments per g12 CONTRACT §A: r3 is the only register read.
 * Gates 3 and 4 read eventFlag+0x0E and eventFlag+0x0C, each only after the
 * preceding gate passes, so a refused pointer must not fault here first.
 *
 * Past the gates the firmware runs `0x158C4`'s continuation:
 *   1. the CellSpurs EA on the isIwl branch inFAMOUS takes;
 *   2. create `_spuEvF` (protocol 1, type 1, key 0, depth 1) through the native registry;
 *   3. 0xB0D0 -> 0xAF2C -- connect it through the SPURS attach domain in DYNAMIC port mode (mask 0xFFFFFFFFFFFF0000,
 *      the kernel choosing the lowest free port in 16..63);
 *   4. 0x159E0 / 0x159EC -- F+0x7C (queue id, 4 bytes) then F+0x0C (port, 1 byte);
 *   5. r3 = 0.
 * On failure the firmware's own unwind applies: if the connect failed, `0xB30C` destroys the queue it created
 * (the destroy syscall) and the code is collapsed by `0x159A4..0x159BC` and returned; neither guest write happens, because both
 * sit strictly after a successful connect. The CellSpurs EA is not the runtime's instance -> the attach stops
 * (SpursAttachStop), which is decision 20's honest stop for an owner this runtime does not have. */
static void spurs_event_flag_attach_continue(StartupExecution &execution,
    const spurs_event_flag_attach_request &request, uint64_t toc, uint32_t toc_slot);
static void dispatch_spurs_event_flag_attach(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.event_flag_attach_calls != SIZE_MAX) ++control.event_flag_attach_calls;
    control.event_flag_attach_last = 0xFFFFFFFFu;
    control.event_flag_attach_queues_created = 0;
    control.event_flag_attach_queues_destroyed = 0;
    control.event_flag_attach_port = 0xFF;
    control.event_flag_attach_writes = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    spurs_event_flag_attach_request request{};
    request.event_flag = uint32_t(cpu.gpr[3]);
    request.direction = 0;
    request.spu_port = 0;
    request.is_iwl = 0;
    request.addr = 0;
    /* 0x158CC/0x158F0 then 0x1591C/0x1592C: the NULL and alignment gates read nothing. */
    const bool field_gates_reachable = request.event_flag != 0u &&
        (request.event_flag & (PS3_SPURS_EVENT_FLAG_ATTACH_ALIGN - 1u)) == 0u;
    if (field_gates_reachable) {
        unsigned char field[1]{};
        spurs_taskset_read_guest(execution, request.event_flag + PS3_SPURS_EVFLAG_DIRECTION, field,
            sizeof(field));
        request.direction = field[0];
        spurs_taskset_read_guest(execution, request.event_flag + PS3_SPURS_EVFLAG_SPU_PORT, field,
            sizeof(field));
        request.spu_port = field[0];
        spurs_taskset_read_guest(execution, request.event_flag + PS3_SPURS_EVFLAG_IS_IWL, field,
            sizeof(field));
        request.is_iwl = field[0];
    }

    uint32_t code = 0;
    const spurs_taskset_init_status status = ps3_spurs_event_flag_attach(&request, &code);
    if (status == PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT) stop(StartupStop::ProviderFailure);
    if (status != PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE) {
        /* A refusal is the firmware's own early return: no guest write, r3 = the code. */
        control.event_flag_attach_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.event_flag_attach_returns);
        return;
    }
    spurs_event_flag_attach_continue(execution, request, toc, toc_slot);
}
/* The continuation: create, connect, write, return. Split out only to keep the gate logic above readable; it runs on the
 * same service set and stops with the same vocabulary. */
static void spurs_event_flag_attach_continue(StartupExecution &execution,
    const spurs_event_flag_attach_request &request, uint64_t toc, uint32_t toc_slot)
{
    StartupControl &control = *execution.control;
    /* 1. the owner word, read only on the isIwl branch (the one inFAMOUS takes). */
    const uint32_t addr_ea = request.event_flag + PS3_SPURS_EVFLAG_TASKSET_PTR;
    unsigned char addr_bytes[4]{};
    spurs_taskset_read_guest(execution, addr_ea, addr_bytes, sizeof(addr_bytes));
    const uint32_t spurs_ea = uint32_t(addr_bytes[0]) << 24 | uint32_t(addr_bytes[1]) << 16 |
        uint32_t(addr_bytes[2]) << 8 | uint32_t(addr_bytes[3]);
    if (!execution.spurs || !execution.spurs->config.lv2) stop(StartupStop::MissingServices);
    ps3_lv2_objects *lv2 = execution.spurs->config.lv2;

    /* 2. The create call. The attribute is the packet g12b decoded; ps3_lv2_queue_create takes exactly its fields. */
    const ps3_lv2_queue_attr attr{PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_PROTOCOL,
        PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_TYPE, PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_NAME};
    uint32_t queue = 0;
    const uint32_t created = ps3_lv2_queue_create(lv2, &queue, &attr,
        PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_IPC_KEY, PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_DEPTH);
    control.event_flag_attach_queue = queue;
    if (created != PS3_LV2_OK) {
        /* 0xB2C4: the create's own return is passed on (0xB244 returns extsw(r31)); nothing was created. */
        const uint32_t collapsed = ps3_spurs_event_flag_attach_collapse(created);
        control.event_flag_attach_last = collapsed;
        spurs_taskset_return(execution, toc, toc_slot, collapsed, control.event_flag_attach_returns);
        return;
    }
    control.event_flag_attach_queues_created++;

    /* 3. 0xB2F0 -> 0xB0D0 -> 0xAF2C in DYNAMIC mode, through the same domain the static attach uses. */
    uint32_t sdk = 0;
    if (!startup_spurs_attach_effective_sdk(execution.process_sdk, &sdk)) {
        /* The attach's own host contract would refuse the request; the firmware has no such input. Destroy the queue
         * (0xB30C) and stop, rather than publish a code the firmware could not have produced. */
        (void)ps3_lv2_queue_destroy(lv2, queue);
        stop(StartupStop::SpursAttachStop);
    }
    const uint32_t chosen_ea = request.event_flag + PS3_SPURS_EVFLAG_SPU_PORT;
    SpursAttachMemory context{&execution};
    const ps3_spurs_attach_memory memory{&context, spurs_attach_read, spurs_attach_write,
        spurs_attach_compare_exchange};
    /* r4 is the created queue, r5 is the OUT byte (F+0x0C itself, exactly as 0x15988 passes &frame[0x70] up the
     * helper chain), r6 = 1 is the dynamic selector (0xB2B4). */
    const ps3_spurs_attach_request attach_request{spurs_ea, queue, chosen_ea, 1u, sdk,
        STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS};
    ps3_spurs_attach_info info{};
    const ps3_spurs_result result = ps3_spurs_runtime_attach_lv2_event_queue(execution.spurs,
        &attach_request, &memory, &info);
    control.spurs_attach_result = result;
    control.spurs_attach_info = info;
    control.spurs_attach_has_result = true;
    control.spurs_attach_callback_error = context.callback_error || context.trace_failure;
    control.spurs_attach_trace_failure = context.trace_failure;
    control.spurs_attach_invalid_address = context.invalid_address;
    control.spurs_attach_stop_latched = context.latched;
    if (result.outcome != PS3_SPURS_OUTCOME_OK) {
        /* The firmware destroys the queue it created when the worker failed. Its code is then
         * collapsed by 0x159A4..0x159BC (a STOP is a native fault, not a firmware code, so it is reported as one). */
        (void)ps3_lv2_queue_destroy(lv2, queue);
        control.event_flag_attach_queues_destroyed++;
        if (result.outcome == PS3_SPURS_OUTCOME_FAILED) {
            const uint32_t collapsed = ps3_spurs_event_flag_attach_collapse(result.rc);
            control.event_flag_attach_last = collapsed;
            spurs_taskset_return(execution, toc, toc_slot, collapsed, control.event_flag_attach_returns);
            return;
        }
        if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(context.latched != StartupStop::None ? context.latched :
            (result.reason == PS3_SPURS_STOP_KERNEL ? StartupStop::SpursKernelFence : StartupStop::SpursAttachStop));
    }
    /* 4. The two writes of 0x159E0 / 0x159EC, in that order. F+0x0C was already produced by the connect itself: 0x15988
     * passes &frame[0x70] all the way down to 0xAF2C's port store, which is the SAME byte the firmware
     * then copies into the struct at 0x159EC. Passing F+0x0C as the attach's port_ea makes that one write happen once. So the
     * adapter adds only F+0x7C (the created queue id, 4 bytes). The firmware's order is preserved: the port byte exists
     * before the queue-id store. */
    unsigned char id_image[4] = {(unsigned char)(queue >> 24), (unsigned char)(queue >> 16),
        (unsigned char)(queue >> 8), (unsigned char)queue};
    spurs_taskset_write_image(execution, request.event_flag + PS3_SPURS_EVFLAG_QUEUE_ID, id_image, 0,
        sizeof(id_image), control.event_flag_attach_writes);
    /* 5. success is 0 (0x1599C sets r31 = 0). */
    control.event_flag_attach_last = 0u;
    control.event_flag_attach_port = info.spup;
    spurs_taskset_return(execution, toc, toc_slot, 0u, control.event_flag_attach_returns);
}

/* cellSpursCreateTasksetWithAttribute (NID 0xC10931CB)
 *
 * The entry validates three
 * things and then runs the 0x14D4C continuation, whose worker writes the 6400-byte
 * CellSpursTaskset image and registers the taskset as a SPURS workload through 0xA000 ->
 * 0x9F0C -> 0x97E4, the module's internal AddWorkload. The firmware's own internal call set is
 * evidenced through; what this adapter adds is the guest ABI, the traced guest writes, the wid
 * it feeds back into +0x74, the native registration through the runtime's existing
 * ps3_spurs_runtime_register_workload path, and the counters.
 *
 * Order, matching the firmware and the same defensive order the other SPURS dispatches use:
 *   1. TOC slot preflight;
 *   2. every guest READ the entry/worker performs, in firmware order: attribute+0x00 (revision,
 *      only after the NULL and alignment gates), then -- once the worker is reached --
 *      attribute+0x08, +0x10 (eight bytes), +0x18, +0x1C, +0x20, +0x27. Nothing is fabricated;
 *   3. the entry's gates, then the worker's own argument gates;
 *   4. the 6400-byte image (one traced HLE write of the memset span plus the five scalar fields,
 *      in the firmware's order);
 *   5. the workload registration through the native path. The wid is written to +0x74 and
 *      wkl_flag_wait_task=0x80 to +0x72 only on the two arms 0x14BAC/0x14BBC publish on (a 0 or
 *      0x80410910 registration result); every other code skips both stores AND the event-flag
 *      setter, exactly as the firmware's own branch does;
 *   6. the event-flag setter on the taskset (0xDC5C), which reads taskset+0x70 and stores one
 *      byte at taskset + 0x100 + wid*128 for wid <= 15 -- the same word
 *      `cellSpursEventFlagSet(taskset->eventFlagSet, ...)` operates on;
 *   7. r3 = the worker's code sign-extended, r2 = the caller's TOC.
 *
 * No SPU thread, group or queue is started, signalled or woken: the registration publishes the
 * workload metadata (and raises neither the ready count nor the workload signal), and the
 * policy-module image at 0x00023680 is never executed. Verified against 0x97E4/0xA000/0x14A58
 * in CONTRACT §B and §E.
 *
 * The attestation words (0x14C04/0x14C1C/0x14C58/0x14CA8, reading *(r2-0x7C44) and
 * *(r2-0x7C38) and passing the decoded pointers 0x00030FA8 / 0x0003176C) belong to the
 * module's own ASSERT machinery. The native runtime does not emit SD-formatted asserts, so
 * neither the reads nor the calls are reproduced: they have no guest-visible effect, and a
 * fabricated trace of them would claim a diagnostic path the guest never sees. */
/* The attribute's own field offsets, named here so each guest read below is traceable to the
 * CellSpursTasksetAttribute layout g10 CONTRACT §A.4 fixed (the same numbers as
 * PS3_SPURS_TSATTR_* in spurs_taskset_init.h, restated because this dispatch reads them in the
 * worker's order rather than the initialiser's). */
enum : size_t {
    offsetof_field_revision = 0x00u,
    offsetof_field_args = 0x08u,
    offsetof_field_priority = 0x10u,
    offsetof_field_max_contention = 0x18u,
    offsetof_field_name = 0x1Cu,          /* cellSpursTasksetAttributeSetName's store (g11) */
    offsetof_field_taskset_size = 0x20u,
    offsetof_field_enable_clear_ls = 0x27u
};

enum : uint32_t { spurs_create_taskset_with_attribute_nid = 0xc10931cb };

/* Reads `length` guest bytes into `bytes` and decodes them big-endian. Stops InvalidMemory with
 * the faulting address when the span is not readable. */
static uint64_t spurs_taskset_read_be(StartupExecution &execution, uint32_t address, size_t length)
{
    unsigned char bytes[8]{};
    spurs_taskset_read_guest(execution, address, bytes, length);
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) value = value << 8 | bytes[index];
    return value;
}

/* The severity of the arm a registration failure lands in: 0x14BB8 detects 0x80410910 and 0x14BC4
 * detects any other non-zero code, and 0x14C74 covers the rest. Only 0 and 0x80410910 publish.
 *
 * The three registration values come from the firmware's own instructions, corrected here from g13:
 *   pm = 0x00023680 -> workload attribute +0x08 (0xA124)
 *   size = 0x00001E40 -> workload attribute +0x0C (0xA128)
 *   data = the TASKSET EA -> workload attribute +0x10 (0xA12C)
 * g13 registered attribute->args as `data`; r26 inside 0xA000 is that function's OWN callee-saved
 * copy of ITS r7, not the caller's r26. */
static void dispatch_spurs_create_taskset_with_attribute(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.taskset_create_calls != SIZE_MAX) ++control.taskset_create_calls;
    control.taskset_create_last = 0xFFFFFFFFu;
    control.taskset_create_wid = 0xFFFFFFFFu;
    control.taskset_create_writes = 0;
    control.taskset_create_registered = 0;
    control.taskset_create_has_registration = false;
    control.taskset_create_registration_sdk = 0;
    control.taskset_create_registration_stop = 0;
    control.taskset_create_registration_effects = 0;
    control.taskset_create_registration_wid = 0xFFFFFFFFu;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    /* r3 spurs, r4 taskset, r5 attr (0x14D5C/0x14D6C: each is the low 32 bits). */
    spurs_taskset_create_request request{};
    request.spurs = uint32_t(cpu.gpr[3]);
    request.taskset = uint32_t(cpu.gpr[4]);
    request.attr = uint32_t(cpu.gpr[5]);
    request.attribute = {};

    uint32_t code = 0;
    /* The attribute revision read sits after the NULL and alignment gates,
     * so a refused pointer must not fault on this read first. */
    if (request.attr != 0u && (request.attr & 7u) == 0u) {
        request.attribute.revision = uint32_t(spurs_taskset_read_be(execution,
            spurs_attribute_field(request.attr, offsetof_field_revision, 4u), 4u));
    }
    if (ps3_spurs_taskset_create_validate(&request, &code) != PS3_SPURS_TASKSET_CREATE_STATUS_OK) {
        control.taskset_create_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.taskset_create_returns);
        return;
    }

    /* The worker is reached with base taskset and pointer spurs.
     * Its own gates come before its memset: r4 == 0 and the 0x7F alignment tests. */
    if (ps3_spurs_taskset_create_validate_arguments(&request, &code) !=
        PS3_SPURS_TASKSET_CREATE_STATUS_OK) {
        control.taskset_create_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.taskset_create_returns);
        return;
    }

    /* The worker's remaining arguments, read from the attribute in this order:
     *   args (+0x08), max_contention (+0x18), name (+0x1C, arg 8),
     *   taskset_size (+0x20)
     *   and the enable_clear_ls byte (+0x27). */
    request.attribute.args = spurs_taskset_read_be(execution,
        spurs_attribute_field(request.attr, offsetof_field_args, 8u), 8u);
    request.attribute.max_contention = uint32_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.attr, offsetof_field_max_contention, 4u), 4u));
    {
        /* +0x10: the eight priority bytes, read in one span exactly as 0xA0DC..0xA0F4 does
         * (`lbz` at priority+i for i = 0..7). */
        unsigned char priority[8]{};
        spurs_taskset_read_guest(execution,
            spurs_attribute_field(request.attr, offsetof_field_priority, 8u), priority,
            sizeof(priority));
        for (size_t index = 0; index < sizeof(priority); ++index)
            request.attribute.priority[index] = priority[index];
    }
    request.attribute.name = uint32_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.attr, offsetof_field_name, 4u), 4u));
    request.attribute.taskset_size = uint32_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.attr, offsetof_field_taskset_size, 4u), 4u));
    request.attribute.enable_clear_ls_byte = (unsigned char)spurs_taskset_read_be(execution,
        spurs_attribute_field(request.attr, offsetof_field_enable_clear_ls, 1u), 1u);

    /* The 6400-byte CellSpursTaskset image (0x14B50 memset, then 0x14B90/0x14B94/0x14B98/0x14B9C).
     * The wid at +0x74 is written only after the registration produced one. The buffer is a fixed
     * automatic array, not heap: this dispatch allocates nothing. */
    unsigned char image[PS3_SPURS_TASKSET_BYTES]{};
    unsigned char flag_wait[1]{0};
    if (ps3_spurs_taskset_create_image(&request, image, sizeof(image), flag_wait, &code) !=
        PS3_SPURS_TASKSET_CREATE_STATUS_OK)
        stop(StartupStop::ProviderFailure);
    spurs_taskset_write_image(execution, request.taskset, image, 0, sizeof(image),
        control.taskset_create_writes);

    /* the registration projection of the SAME attribute. `data` is the taskset
     * EA (request.taskset, zero-extended at 0x14B38), which is also what the worker stores at
     * taskset+0x68 -- not attribute->args. */
    spurs_taskset_registration registration{};
    if (ps3_spurs_taskset_create_registration(&request, 0u, &registration, &code) !=
        PS3_SPURS_TASKSET_CREATE_STATUS_OK) {
        control.taskset_create_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.taskset_create_returns);
        return;
    }
    /* 0xA000 -> 0x9F0C -> 0x97E4 through the native workload-registration path, which claims a
     * wid slot, writes the workload info, the ready/contention bits and the two publication
     * messages, exactly as 0x97E4 does. */
    ps3_spurs_registration_request native{};
    native.spurs = registration.spurs;
    native.wid_output = request.taskset + PS3_SPURS_CSTS_WID;
    native.policy_module = PS3_SPURS_TASKSET_PM_EA;   /* 0x14B5C -> 0xA124 */
    native.size = PS3_SPURS_TASKSET_PM_SIZE;          /* 0x14B64 -> 0xA128 */
    native.data = registration.data;                  /* 0x14B88 -> 0xA12C (the taskset EA) */
    for (size_t index = 0; index < 8u; ++index) native.priority[index] = registration.priority[index];
    native.min_contention = registration.min_contention;
    native.max_contention = registration.max_contention;
    native.name_class = registration.name_class;
    native.name_instance = registration.name_instance;
    native.hook = registration.hook;
    native.hook_argument = registration.hook_argument;
    control.taskset_create_has_registration = true;
    uint32_t sdk = 0;
    if (!ps3_spurs_workload_registration_validate_request(&native, &sdk))
        stop(StartupStop::ProviderFailure);
    if (sdk) {
        const uint32_t collapsed = ps3_spurs_taskset_create_registration_code(sdk);
        control.taskset_create_last = collapsed;
        spurs_taskset_return(execution, toc, toc_slot, collapsed, control.taskset_create_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    if (!writable(execution, native.spurs, PS3_SPURS_REGISTRATION_PRIMARY_BYTES))
        spurs_attribute_invalid_memory(native.spurs);
    if (!writable(execution, native.wid_output, 4)) spurs_attribute_invalid_memory(native.wid_output);
    if (spurs_attribute_overlaps(native.wid_output, 4, toc_slot, 8) ||
        spurs_attribute_overlaps(native.spurs, PS3_SPURS_REGISTRATION_PRIMARY_BYTES, toc_slot, 8))
        spurs_attribute_invalid_memory(toc_slot);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    ps3_spurs_registration_result result{};
    ps3_spurs_runtime_register_workload(execution.spurs, &native, execution.ppu.reservation,
        STARTUP_SPURS_REGISTRATION_MAX_ATTEMPTS, &context, spurs_registration_poll,
        &context, spurs_registration_observe, &result);
    control.taskset_create_registration_sdk = result.sdk_result;
    control.taskset_create_registration_stop = unsigned(result.stop);
    control.taskset_create_registration_effects = result.effects;
    control.taskset_create_registration_wid = result.wid;
    control.taskset_create_trace_failure = context.trace_failure;
    control.taskset_create_latched_stop = context.latched;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) stop(context.latched);
    if (context.callback_error || result.stop != PS3_SPURS_REGISTRATION_RUNNING ||
        !result.has_sdk_result)
        stop(StartupStop::SpursWorkloadStop);
    /* the worker folds the module's policy code into the task namespace. */
    const uint32_t worker_code = ps3_spurs_taskset_create_registration_code(result.sdk_result);
    control.taskset_create_wid = result.wid;
    if (ps3_spurs_taskset_create_publishes_workload(worker_code)) {
        /* The worker stores wkl_flag_wait_task (0x80) at taskset+0x72 and the wid at taskset+0x74, then calls 0xDC5C.
         * 0xDC5C reads taskset+0x70 (now 0 or 1) and stores 1 as an 8-byte big-endian doubleword
         * at `taskset + 0x100 + wid*128` for wid <= 15; a wid above 15 lands at
         * `taskset + 0xB00 + (wid&15)*128`, which the reference calls wklFlag2. Both forms are
         * inside the 6400-byte object this call wrote (wid <= 15 -> offset <= 0x880, wid > 15 ->
         * offset <= 0x87F), so the store is this entry's own effect and is performed here.
         *
         * Reachability on inFAMOUS's path:
         * the first branch test
         * leaves the code <= 0 on this arm and the second leaves it <= 0 as well, so
         * the store runs exactly when the folded code is 0 or negative; the game's path is 0.
         * The firmware does it through a load-reserve/store-conditional pair; natively
         * the slot is inside the image written above, so the reservation always commits.
         * `taskset_create_registered` counts the calls that reached these three stores. */
        unsigned char wid_image[4]{};
        unsigned char flag_value[8]{};
        if (ps3_spurs_taskset_create_wid_image(result.wid, wid_image, sizeof(wid_image)) !=
            PS3_SPURS_TASKSET_CREATE_STATUS_OK ||
            ps3_spurs_taskset_create_flag_value_image(flag_value, sizeof(flag_value)) !=
            PS3_SPURS_TASKSET_CREATE_STATUS_OK)
            stop(StartupStop::ProviderFailure);
        uint32_t flag_address = 0;
        if (ps3_spurs_taskset_create_flag_address(request.taskset, result.wid, &flag_address) != 0)
            stop(StartupStop::ProviderFailure);
        spurs_taskset_write_image(execution, request.taskset + PS3_SPURS_CSTS_WID, wid_image, 0,
            4u, control.taskset_create_writes);
        spurs_taskset_write_image(execution, request.taskset + PS3_SPURS_CSTS_WKL_FLAG_WAIT_TASK,
            flag_wait, 0, 1u, control.taskset_create_writes);
        /* 0x14BE8 -> 0xDD30: one traced 8-byte big-endian HLE write of 1. */
        spurs_taskset_write_image(execution, flag_address, flag_value, 0, 8u,
            control.taskset_create_writes);
        control.taskset_create_registered++;
    }
    control.taskset_create_last = worker_code;
    spurs_taskset_return(execution, toc, toc_slot, worker_code, control.taskset_create_returns);
}
enum : uint32_t {
    /* _cellSpursLFQueueInitialize: the entry the
     * game reaches after its taskset block, NID hash-verified against the name. */
    spurs_lfqueue_initialize_nid = 0x011ee38b,
    /* the field offsets of the two owner objects the entry reads its version from */
    offsetof_lfq_taskset_version = 0x24u,
    offsetof_lfq_spurs_version = 0x74u
};

/* _cellSpursLFQueueInitialize (NID 0x011EE38B)
 *
 * The entry validates, dispatches on
 * the low nibble of its first argument, reads the owner's word, and then either rebuilds the
 * caller's 128-byte line (plus two 30-byte runs of zeros) or validates the line that is already
 * there. It makes no syscall and starts no SPU work on inFAMOUS's path.
 *
 * Order, matching the firmware:
 *   1. TOC slot preflight (the same helper every SPURS dispatch uses);
 *   2. the argument gates, on the REGISTER only: 0x20E8..0x21C8 are register tests, which the firmware
 *      evaluates strictly BEFORE any guest access, so a refused call reads nothing;
 *   3. G14c: the ENTRY's arm selector and, on the spurs arm only, its version
 *      gate 0x171A4..0x171B4 on `spurs+0x74`, which answers 0x80410902 when the word exceeds 0x1F and
 *      touches no other guest byte. That gate is the ENTRY's first access; reading the queue line
 *      before it was the G14b regression this ordering fixes;
 *   4. the word the TASKSET arm reads, `taskset+0x24` -- the dispatcher loads it after its
 *      gates, and this arm is not gated at all;
 *   5. 0x220C..0x2268: the STATE probe on `queue+0x2C`, the dispatcher's FIRST guest access;
 *      (0x22E4..0x2338) and writes nothing;
 *   7. otherwise the two auxiliary zero runs (0x1FCC..0x2014), inside the caller's object, outside
 *      the line; then the 128-byte line (0x1FA0..0x2098), in place;
 *   8. the state word the builder leaves at `queue+0x08`, as its own traced 2-byte write;
 *   9. the object is marked initialized;
 *  10. r3 = 0 or the validation refusal, sign-extended; r2 = the caller's TOC.
 *
 * No probe is reached on this path: 0x24F4 / 0x30B8 / 0x2708 / 0x3A18 are
 * only entered from entry 2 and 0x174F8, both of which are other exports. */
static void dispatch_spurs_lfqueue_initialize(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.lfq_calls != SIZE_MAX) ++control.lfq_calls;
    control.lfq_last = 0xFFFFFFFFu;
    control.lfq_writes = 0;
    control.lfq_shape = 0xFFu;
    control.lfq_owner = 0xFFFFFFFFu;
    control.lfq_version = 0xFFFFFFFFu;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    /* r3 owner (tagged), r4 queue, r5 buffer, r6 elem size, r7 depth, r8 direction: the six
     * registers the game sets at 0x004248B0..0x004248D0.
     *
     * G15b: the owner word is the LOW 32 bits. The firmware's nibble test and its version load
     * address the CellSpurs as a 32-bit EA; the caller's r3 also carries whatever
     * the PPU had above bit 31 (on inFAMOUS's own path r31 is zero-extended, so nothing changes
     * there). Reading this EA back out of the 64-bit r3 put those high bytes into the address, and
     * whether they were zero depended on how the CellSpurs create happens to sign-extend its exit
     * code -- which is not a value the entry may depend on. The `code` values stay 32-bit too, so
     * the only place a 64-bit value is formed is `spurs_taskset_return`'s deliberate `extsw`. */
    spurs_lfq_request request{};
    request.owner = uint32_t(cpu.gpr[3]);
    request.queue = uint32_t(cpu.gpr[4]);
    request.buffer = uint32_t(cpu.gpr[5]);
    request.elem_size = uint32_t(cpu.gpr[6]);
    request.depth = uint32_t(cpu.gpr[7]);
    request.direction = uint32_t(cpu.gpr[8]);
    control.lfq_owner = request.owner;

    /* The ENTRY's arm selector tests the low nibble of r3.
     *
     * G14c: on the SPURS arm (nibble 0) the ENTRY's first guest access is the version load
     * and its gate, so nothing else may be read or decided before it. `ps3_spurs_lfq_validate` also
     * gates nothing on this arm, because 0x209C is reached here through the version check's in-range branch with the caller's
     * own (unremapped) registers -- see the split documented on that function. */
    if (ps3_spurs_lfq_arm_of(&request) == PS3_SPURS_LFQ_ARM_SPURS) {
        uint32_t code = 0;
        const uint32_t spurs_version = uint32_t(
            spurs_taskset_read_be(execution, request.owner + offsetof_lfq_spurs_version, 4u));
        control.lfq_version = spurs_version;
        if (ps3_spurs_lfq_version_gate(&request, spurs_version, &code) !=
            PS3_SPURS_LFQ_STATUS_OK) {
            control.lfq_last = code;
            spurs_taskset_return(execution, toc, toc_slot, code, control.lfq_returns);
            return;
        }
        /* The version check's in-range branch goes to 0x1716C, i.e. the dispatcher with the CELLER'S own registers. Nothing
         * on this project's path calls the entry that way (inFAMOUS's owner is always tagged), so the
         * remaining work is the taskset arm's; it is not modelled on top of a register map the firmware
         * does not form here. */
        stop(StartupStop::MissingServices);
    }
    uint32_t code = 0;
    if (ps3_spurs_lfq_validate(&request, &code) != PS3_SPURS_LFQ_STATUS_OK) {
        control.lfq_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.lfq_returns);
        return;
    }
    /* the taskset arm's word. `r9` is the base the entry formed at
     * i.e. the QUEUE (the line and the taskset are the same object). This arm is not
     * version-gated at all (ps3_spurs_lfq_version_gate encodes that), so the load happens exactly where
     * the dispatcher does it. */
    {
        const uint32_t taskset_version = uint32_t(
            spurs_taskset_read_be(execution, request.queue + offsetof_lfq_taskset_version, 4u));
        control.lfq_version = taskset_version;
        if (ps3_spurs_lfq_version_gate(&request, taskset_version, &code) !=
            PS3_SPURS_LFQ_STATUS_OK) {
            control.lfq_last = code;
            spurs_taskset_return(execution, toc, toc_slot, code, control.lfq_returns);
            return;
        }
    }
    /* The shape decision reads the word whichever arm supplied it. On the spurs arm every admissible
     * value selects the CellSyncLFQueue shape: the version check goes to 0x1716C (the dispatcher), never
     * to the builder's 0x3A18 arm, which is what ps3_spurs_lfq_uses_spurs_shape encodes. */
    const uint32_t version = control.lfq_version;

    /* The caller's current line, read before anything is written: the validation arm below compares
     * exactly these bytes, and the image is applied in place because the real object is not zero on
     * entry. */
    unsigned char before[PS3_SPURS_LFQ_BYTES]{};
    spurs_taskset_read_guest(execution, request.queue, before, sizeof(before));

    /* An object that already holds 2 is
     * only validated -- 0x22E4..0x2338 compares the line against the arguments and writes nothing. */
    if (ps3_spurs_lfq_state_of(uint32_t(ps3_spurs_lfq_field(before, sizeof(before),
            PS3_SPURS_LFQ_STATE, 4u))) == PS3_SPURS_LFQ_STATE_MATCH) {
        const uint32_t result = ps3_spurs_lfq_match(&request, before, sizeof(before));
        control.lfq_last = result;
        spurs_taskset_return(execution, toc, toc_slot, result, control.lfq_returns);
        return;
    }

    /* 0x1FCC..0x2014: two runs of 15 zero `sth` at stride 2, at queue+0x30 and queue+0x50. */
    {
        unsigned char aux[PS3_SPURS_LFQ_AUX_RUN_BYTES]{};
        for (unsigned run = 0; run < 2u; ++run) {
            uint32_t address = 0;
            if (ps3_spurs_lfq_aux_image(run, aux, sizeof(aux)) != PS3_SPURS_LFQ_STATUS_OK ||
                ps3_spurs_lfq_aux_address(request.queue, run, &address) != 0)
                stop(StartupStop::ProviderFailure);
            spurs_taskset_write_image(execution, address, aux, 0, sizeof(aux),
                control.lfq_writes);
        }
    }

    /* 0x1FA0..0x2098: the 128-byte line, written over the object's own bytes. */
    unsigned char image[PS3_SPURS_LFQ_BYTES]{};
    for (size_t index = 0; index < sizeof(image); ++index) image[index] = before[index];
    if (ps3_spurs_lfq_image_in_place(&request, version, image, sizeof(image)) !=
        PS3_SPURS_LFQ_STATUS_OK)
        stop(StartupStop::ProviderFailure);
    spurs_taskset_write_image(execution, request.queue, image, 0, sizeof(image),
        control.lfq_writes);

    /* the builder's own reservation word, zero. */
    {
        unsigned char state[2]{};
        if (ps3_spurs_lfq_state_image(state, sizeof(state)) != PS3_SPURS_LFQ_STATUS_OK)
            stop(StartupStop::ProviderFailure);
        spurs_taskset_write_image(execution, request.queue + PS3_SPURS_LFQ_H8, state, 0,
            sizeof(state), control.lfq_writes);
    }

    /* the object is now initialized. This is the
     * store that makes a second call on the same object take 0x22E4. */
    {
        unsigned char mark[4]{};
        if (ps3_spurs_lfq_put_be(mark, sizeof(mark), 0, 4u, PS3_SPURS_LFQ_STATE_INITIALIZED) != 0)
            stop(StartupStop::ProviderFailure);
        spurs_taskset_write_image(execution, request.queue + PS3_SPURS_LFQ_STATE, mark, 0,
            sizeof(mark), control.lfq_writes);
    }

    /* r31 still holds 0, so the entry returns 0. */
    const uint32_t result_code = ps3_spurs_lfq_result(&request, version, before, sizeof(before));
    control.lfq_shape = unsigned(ps3_spurs_lfq_uses_spurs_shape(&request, version));
    control.lfq_last = result_code;
    spurs_taskset_return(execution, toc, toc_slot, result_code, control.lfq_returns);
}
/* cellSpursLFQueueAttachLv2EventQueue (NID 0x1656D49F)
 *
 * Contract: the SDK-25 arm (the arm inFAMOUS takes) on top of
 * the earlier arms (the arms it does not), with the owner resolution
 * re-decoded by decision 27 (evidence/root-gcm-next-boundary-prep-r1/DECISIONS.md). The entry is the
 * next one func_00424774 reaches (LR 0x0042483C, straight after _cellSpursLFQueueInitialize returns).
 *
 * WHICH ARM RUNS. The entry selects its behaviour from the EFFECTIVE PROCESS SDK VERSION:
 *
 *   r3 = the process SDK
 *   sdk > 0x17FFFF -> the middle arm
 *   sdk > 0x18FFFF -> the SDK-25 arm
 *
 * inFAMOUS's process SDK is 0x00250001 (its own logs print effective_sdk=0x00250001), so both
 * comparisons hold and the entry runs 0x17778 onwards. b1's decode covered the 0x17694..0x17774 loop,
 * which only the `0x17FFFF < sdk <= 0x18FFFF` arm reaches. That was the one premise i1's BLOCKER was
 * built on.
 *
 * WHAT 0x17778 DOES. It reads the line's `+0x24` word, refuses with 0x80410909 when it is zero, and then
 * rejoins the SHARED loop at 0x17674/0x176F8 through the same two shape gates.
 *
 * THE OWNER IS RESOLVED ON EVERY ARM (decision 27). The load of the line's `+0x74` word and the whole
 * owner resolution run BEFORE the SDK compare, so the SDK-25 arm does not skip them:
 * `r28` -- a CellSpurs EA, untagged -- is computed once and handed unchanged to 0xB244/0xAF2C. The tag
 * meaning is the opposite of the earlier labels: an UNTAGGED owner is a taskset (init's 0x171A4 bounds
 * its own `+0x74` wid, which only a taskset has) and a `|1` one is a CellSpurs, which is what the game
 * passes. G15's `+0x60` reading was wrong: the predecessor zeroes `queue+0x50..0x6E` (0x1FD8/0x1FE4).
 *
 * THE GAME'S LINE ANSWERS 0. `_cellSpursLFQueueInitialize` stored the DIRECTION 3 at `+0x24` (0x1FBC)
 * and its CellSpursLFQueue tail stored 0xFF at `+0x20` and `+0x21` (0x2084/0x2078), so:
 *   the direction word is non-zero, so the zero-direction exit does NOT return 0x80410909;
 *   it is 3, so the SPURS shape gate runs and both bytes are 0xFF;
 *   the loop index runs to 2 -> two iterations, each a queue create + dynamic connect + the SPU status syscalls (134/136);
 *   the version is 3 -> the SPURS publication, r31 = 0, so r3 = 0.
 *
 * Order, matching the firmware:
 *   1. TOC slot preflight;
 *   2. the two entry gates, on the REGISTER only: 0x17508 (`line & 0x7F`) then 0x17578 (`line == 0`);
 *   3. the owner resolution, on EVERY SDK arm and BEFORE the SDK compare at 0x175B8
 *      (0x17588 `+0x74`, then at most one `+0x64` read of an untagged owner); then the shape selector's
 *      `+0x24` and `+0x20`/`+0x21` only for the shape that tests them;
 *   4. ps3_spurs_lfqa_plan_build_for_sdk() -- the branch table, every gate, and the action list, with NO
 *      effect of any kind;
 *   5. on REFUSED: publish the code and return. Every refusal precedes the firmware's first store;
 *   6. on OK: each iteration's actions through the native lv2 registry and the SPURS attach service in
 *      dynamic-port mode (0xAF2C's `r6 = 1` arm: mask 0xFFFFFFFFFFFF0000, ports 16..63, keyed);
 *   7. the publication through ps3_spurs_lfqa_publication(), each span as its own traced write;
 *   8. r3 = ps3_spurs_lfqa_result(folded code), r2 = the caller's TOC.
 */
enum : uint32_t {
    /* cellSpursLFQueueAttachLv2EventQueue. */
    spurs_lfqueue_attach_nid = 0x1656d49f,
    /* The line words this entry reads, all inside the 128-byte object the predecessor built. */
    offsetof_lfqa_owner = 0x74u,       /* EVERY arm, before the SDK compare      */
    offsetof_lfqa_version = 0x24u,
    offsetof_lfqa_queue = 0x20u,
    offsetof_lfqa_port2 = 0x21u
};

static void dispatch_spurs_lfqueue_attach(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.lfqa_calls != SIZE_MAX) ++control.lfqa_calls;
    control.lfqa_last = 0xFFFFFFFFu;
    control.lfqa_owner = 0;
    control.lfqa_group = 0;
    control.lfqa_version = 0;
    control.lfqa_shape = 0xFFu;
    control.lfqa_iterations = 0;
    control.lfqa_queues_created = 0;
    control.lfqa_queues_destroyed = 0;
    control.lfqa_port = 0xFFu;
    control.lfqa_writes = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    spurs_lfqa_request request{};
    request.line = uint32_t(cpu.gpr[3]);
    /* Gates 1 and 2 read the register only,
     * so a refused call touches no guest byte. */
    {
        uint32_t gate_code = 0;
        if (ps3_spurs_lfqa_validate(&request, &gate_code) != PS3_SPURS_LFQA_STATUS_OK) {
            control.lfqa_last = gate_code;
            spurs_taskset_return(execution, toc, toc_slot, gate_code, control.lfqa_returns);
            return;
        }
    }

    /* The SDK decides which arm of the entry this call is on: 0x175C8/0x17668's two `cmplw`/`bgt`
     * pairs. It comes only from the immutable process configuration, exactly as the two neighbouring
     * SPURS entries take it. An unmapped (negative) SDK is the honest stop, never a guess. */
    uint32_t sdk = 0;
    if (!startup_spurs_attach_effective_sdk(execution.process_sdk, &sdk)) stop(StartupStop::SpursAttachStop);
    request.sdk_version = sdk;
    const spurs_lfqa_sdk_branch branch = ps3_spurs_lfqa_sdk_branch_of(sdk);
    (void)branch;   /* the table itself is applied by ps3_spurs_lfqa_plan_build_for_sdk() below */

    /* the owner word. Every SDK arm reads it, and the whole
     * 0x17590..0x175B4 resolution runs BEFORE the SDK compare at 0x175B8 -- decision 27. The word is the
     * low 32 bits of the u64 the init stored at `+0x70`, and `_cellSpursLFQueueInitialize` stores the
     * caller's r3 there tagged, so inFAMOUS's is its CellSpurs EA with bit 0 set. */
    {
        unsigned char word[4]{};
        spurs_taskset_read_guest(execution, request.line + offsetof_lfqa_owner, word, sizeof(word));
        request.owner = uint32_t(word[0]) << 24 | uint32_t(word[1]) << 16 |
            uint32_t(word[2]) << 8 | uint32_t(word[3]);
    }
    /* Only a nibble-0, non-zero owner is dereferenced, and the
     * word it yields is a CellSpurs EA -- the init's tag meaning (the init's version load bounds the
     * TASKSET's own `wid`, so an untagged owner is a taskset and a `|1` one is a CellSpurs). */
    uint32_t tag = 0;
    if ((request.owner & 0xFu) == 0u && request.owner != 0u) {
        unsigned char tag_bytes[4]{};
        spurs_taskset_read_guest(execution, request.owner + PS3_SPURS_LFQA_OWNER_TAG, tag_bytes,
            sizeof(tag_bytes));
        tag = uint32_t(tag_bytes[0]) << 24 | uint32_t(tag_bytes[1]) << 16 |
            uint32_t(tag_bytes[2]) << 8 | uint32_t(tag_bytes[3]);
    }
    control.lfqa_owner = request.owner;

    /* the shape selector / direction word. */
    {
        unsigned char word[4]{};
        spurs_taskset_read_guest(execution, request.line + offsetof_lfqa_version, word, sizeof(word));
        request.version = uint32_t(word[0]) << 24 | uint32_t(word[1]) << 16 |
            uint32_t(word[2]) << 8 | uint32_t(word[3]);
        control.lfqa_version = request.version;
    }

    /* the shape gates read `+0x20` (a byte on the SPURS shape, a word on the
     * CellSync one) and, on the SPURS shape only, `+0x21` second. */
    uint32_t port2 = 0;
    {
        unsigned char bytes[2]{};
        if (ps3_spurs_lfqa_is_spurs_shape(request.version)) {
            spurs_taskset_read_guest(execution, request.line + offsetof_lfqa_queue, bytes, 1u);
            request.recorded_queue = bytes[0];
            spurs_taskset_read_guest(execution, request.line + offsetof_lfqa_port2, bytes + 1, 1u);
            port2 = bytes[1];
        } else {
            unsigned char word[4]{};
            spurs_taskset_read_guest(execution, request.line + offsetof_lfqa_queue, word,
                sizeof(word));
            request.recorded_queue = uint32_t(word[0]) << 24 | uint32_t(word[1]) << 16 |
                uint32_t(word[2]) << 8 | uint32_t(word[3]);
        }
    }

    spurs_lfqa_plan plan{};
    ps3_spurs_lfqa_plan_build_for_sdk(&request, tag, port2, &plan);
    control.lfqa_shape = unsigned(plan.spurs_shape);
    control.lfqa_iterations = plan.iterations;
    control.lfqa_group = plan.group;
    if (plan.status != PS3_SPURS_LFQA_STATUS_OK) {
        /* Every refusal above sits before the firmware's first store, so no guest byte is written. The
         * shape and trip count ARE reported when the gate that refused ran after the shape selector
         * (0x1763C and 0x1767C do), so a capture can tell a shape refusal from an argument one. */
        if (plan.r3 == PS3_SPURS_LFQA_PORT) {
            control.lfqa_shape = unsigned(ps3_spurs_lfqa_is_spurs_shape(request.version));
            control.lfqa_iterations = ps3_spurs_lfqa_iterations(request.version);
        } else {
            control.lfqa_shape = 0xFFu;
            control.lfqa_iterations = 0;
        }
        control.lfqa_last = plan.r3;
        spurs_taskset_return(execution, toc, toc_slot, plan.r3, control.lfqa_returns);
        return;
    }

    /* The instance 0xAF2C connects to: `r28`, the owner resolved exactly as 0x17590..0x175B4 resolve it, on
     * EVERY arm (decision 27) -- a tagged owner IS the CellSpurs EA with its low nibble cleared, an
     * untagged one named it through its own `+0x64`. Both builders carry it in `plan.group`, and it is
     * already refused with 0x80410902 above when the resolution produced nothing. It is an UNTAGGED
     * CellSpurs EA, which is why the native lookup below is by `spurs_ea` and never by the plan's group id. */
    const uint32_t spurs_ea = plan.group;
    if (spurs_ea == 0u || !execution.spurs) stop(StartupStop::ProviderFailure);
    uint32_t group = 0;
    for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index) {
        const ps3_spurs_instance &instance = execution.spurs->instances[index];
        if (instance.state != PS3_SPURS_INSTANCE_FREE && instance.spurs_ea == spurs_ea) {
            /* the group id the guest object itself carries. It is what the
             * connect targets, so it is reported from the native record the attach service will read,
             * and a mismatch with the guest's own field is the honest KernelFence the service models. */
            group = instance.group;
            break;
        }
    }
    if (group == 0u) stop(StartupStop::ProviderFailure);
    control.lfqa_group = group;

    if (!execution.spurs->config.lv2) stop(StartupStop::MissingServices);
    ps3_lv2_objects *const lv2 = execution.spurs->config.lv2;

    /* The kernel's chosen port lands in the CALLER's frame, exactly as 0xB044 hands `&frame[0x70]` down:
     * i.e. the firmware's own stack slot, which in the guest is writable RAM. */
    const uint64_t port_slot = cpu.gpr[1] + 0x70u;
    if (port_slot < cpu.gpr[1] || port_slot > UINT32_MAX || !writable(execution, uint32_t(port_slot), 1u))
        stop(StartupStop::InvalidMemory);
    const uint32_t port_ea = uint32_t(port_slot);
    uint32_t last_queue = 0;
    uint32_t last_port = PS3_SPURS_LFQA_SPU_PORT;
    for (uint32_t step = 0; step < plan.actions_count; ++step) {
        const spurs_lfqa_action &action = plan.actions[step];
        check_boundary(BoundaryBudget::Poll);
        if (action.kind == PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH) {
            /* the packet ps3_spurs_lfqa_queue_image() reproduces (protocol 1, type 1,
             * name 0), ipc_key 0, depth 1. */
            const ps3_lv2_queue_attr attr{PS3_SPURS_LFQA_QUEUE_PROTOCOL, PS3_SPURS_LFQA_QUEUE_TYPE,
                uint64_t(PS3_SPURS_LFQA_QUEUE_NAME)};
            uint32_t created = 0;
            const uint32_t rc = ps3_lv2_queue_create(lv2, &created, &attr,
                PS3_SPURS_LFQA_QUEUE_IPC_KEY, PS3_SPURS_LFQA_QUEUE_DEPTH);
            if (rc != PS3_LV2_OK) {
                /* 0xB244 returns the create's own code; 0x176CC folds it when it is a 0x8041xxxx one. */
                control.lfqa_last = ps3_spurs_lfqa_result(rc);
                spurs_taskset_return(execution, toc, toc_slot, control.lfqa_last,
                    control.lfqa_returns);
                return;
            }
            control.lfqa_queues_created++;
            last_queue = created;
        } else {
            last_queue = action.queue;
        }
        /* 0xB0D0 -> 0xAF2C in DYNAMIC port mode (mask 0xFFFFFFFFFFFF0000, ports 16..63, keyed), through
         * the same reviewed domain that serves cellSpursAttachLv2EventQueue and the event-flag attach. */
        SpursAttachMemory context{&execution};
        const ps3_spurs_attach_memory memory{&context, spurs_attach_read, spurs_attach_write,
            spurs_attach_compare_exchange};
        const ps3_spurs_attach_request attach_request{spurs_ea, last_queue, port_ea, 1u, sdk,
            STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS};
        ps3_spurs_attach_info info{};
        const ps3_spurs_result result = ps3_spurs_runtime_attach_lv2_event_queue(execution.spurs,
            &attach_request, &memory, &info);
        control.spurs_attach_result = result;
        control.spurs_attach_info = info;
        control.spurs_attach_has_result = true;
        control.spurs_attach_callback_error = context.callback_error || context.trace_failure;
        control.spurs_attach_trace_failure = context.trace_failure;
        control.spurs_attach_invalid_address = context.invalid_address;
        control.spurs_attach_stop_latched = context.latched;
        if (result.outcome != PS3_SPURS_OUTCOME_OK) {
            /* the firmware destroys the queue it created when the worker failed. */
            if (action.kind == PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH) {
                (void)ps3_lv2_queue_destroy(lv2, last_queue);
                control.lfqa_queues_destroyed++;
            }
            if (result.outcome == PS3_SPURS_OUTCOME_FAILED) {
                control.lfqa_last = ps3_spurs_lfqa_result(result.rc);
                spurs_taskset_return(execution, toc, toc_slot, control.lfqa_last,
                    control.lfqa_returns);
                return;
            }
            if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
            stop(context.latched != StartupStop::None ? context.latched :
                (result.reason == PS3_SPURS_STOP_KERNEL ? StartupStop::SpursKernelFence :
                 StartupStop::SpursAttachStop));
        }
        last_port = info.spup;
        control.lfqa_port = info.spup;
    }

    /* the publication, over the caller's own line bytes. The SPURS shape writes
     * `+0x78` as a 64-bit word and `+0x20` as a 32-bit one; the CellSync shape writes seven fields at
     * their own widths. The adapter models one iteration's worth of frame values, which is what the
     * firmware's frame holds when both iterations succeed on the same group. */
    unsigned char line[PS3_SPURS_LFQA_BYTES]{};
    spurs_taskset_read_guest(execution, request.line, line, sizeof(line));
    const uint64_t id64 = uint64_t(last_queue) << 32 | last_queue;
    if (ps3_spurs_lfqa_publication(plan.spurs_shape, id64, last_queue, last_queue, last_port,
            last_port, 0xFFFFFFFFu, 0xFFFFu, 0xFFFFu, line, sizeof(line)) != 0)
        stop(StartupStop::ProviderFailure);
    /* The span the firmware writes is not contiguous: `+0x78` and `+0x20`. They are written as the two
     * scalar stores the widths call for, each with its own traced credit. */
    {
        unsigned char result_word[8]{};
        for (size_t index = 0; index < sizeof(result_word); ++index)
            result_word[index] = line[PS3_SPURS_LFQA_RESULT + index];
        const size_t width = plan.spurs_shape ? 8u : 4u;
        spurs_taskset_write_image(execution, request.line + PS3_SPURS_LFQA_RESULT, result_word, 0,
            width, control.lfqa_writes);
    }
    {
        unsigned char queue_word[4]{};
        for (size_t index = 0; index < sizeof(queue_word); ++index)
            queue_word[index] = line[PS3_SPURS_LFQA_QUEUE + index];
        const size_t width = plan.spurs_shape ? 4u : 1u;
        spurs_taskset_write_image(execution, request.line + PS3_SPURS_LFQA_QUEUE, queue_word, 0,
            width, control.lfqa_writes);
    }
    if (!plan.spurs_shape) {
        for (size_t field = 0; field < 5u; ++field) {
            static const uint32_t offset[5] = {PS3_SPURS_LFQA_PORT2, PS3_SPURS_LFQA_W28,
                PS3_SPURS_LFQA_W30, PS3_SPURS_LFQA_W50, PS3_SPURS_LFQA_W7C};
            static const size_t width[5] = {1u, 4u, 2u, 2u, 4u};
            unsigned char bytes[4]{};
            for (size_t index = 0; index < width[field]; ++index)
                bytes[index] = line[offset[field] + index];
            spurs_taskset_write_image(execution, request.line + offset[field], bytes, 0,
                width[field], control.lfqa_writes);
        }
    }

    control.lfqa_last = ps3_spurs_lfqa_result(0u);
    spurs_taskset_return(execution, toc, toc_slot, control.lfqa_last, control.lfqa_returns);
}
enum : uint32_t { spurs_ready_count_store_nid = 0xf843818d, spurs_get_workload_info_nid = 0x4e153e3e };
static int spurs_ready_observe(void *opaque, const ps3_spurs_ready_count_event *event)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &observation = context.execution->control->spurs_ready;
    if (!event || !event->bytes || !spurs_attach_scalar(event->length) ||
        (event->operation != PS3_SPURS_READY_COUNT_READ && event->operation != PS3_SPURS_READY_COUNT_WRITE)) {
        spurs_attach_refuse(context);
        return 0;
    }
    if (!spurs_attach_reserve(context)) return 0;
    spurs_attach_publish(context, event->operation == PS3_SPURS_READY_COUNT_WRITE, event->address,
        event->length, spurs_attach_be(event->bytes, event->length));
    spurs_attach_close(context);
    if (context.trace_failure) return 0;
    ++observation.publications;
    return 1;
}
static void dispatch_spurs_ready(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_ready = {};
    auto &observation = control.spurs_ready;
    if (control.spurs_ready_calls != SIZE_MAX) ++control.spurs_ready_calls;
    for (size_t i = 0; i < 3; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    observation.request = {uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5])};
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    uint32_t sdk = 0;
    if (!ps3_spurs_ready_count_validate_address(&observation.request, &sdk)) stop(StartupStop::ProviderFailure);
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_ready_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_ready_count_memory_scope memory_scope{&context, spurs_operation_permits};
    ps3_spurs_runtime_ready_count_store(execution.spurs, &observation.request, memory_scope, execution.ppu.reservation, &context,
        spurs_registration_poll, &context, spurs_ready_observe, &observation.domain_result);
    observation.has_domain_result = true;
    observation.trace_failure = context.trace_failure;
    observation.latched_stop = context.latched;
    observation.memory_status = observation.domain_result.memory_status;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) {
        if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(context.latched);
    }
    if (spurs_fence_pending(control)) {
        observation.latched_stop = spurs_fence_stop(control);
        stop(observation.latched_stop);
    }
    if (context.callback_error || observation.domain_result.stop != PS3_SPURS_READY_COUNT_RUNNING ||
        !observation.domain_result.has_sdk_result) stop(StartupStop::SpursReadyStop);
    spurs_workload_sdk_error(execution, observation, toc_slot, toc,
        observation.domain_result.sdk_result, control.spurs_ready_returns);
}

/* cellSpursCreateTask (NID 0xBEB600AC)
 *
 * The entry validates, calls 0x11ACC (ELF/context/size gates, slot sweep over taskset+0x30, six
 * record stores into the entry's own frame slot), then 0x10C6C (the ready bit at taskset+0x20,
 * 0xA750's ready-count raise, 0x85D0's signal byte), and only then writes the caller's *taskId.
 *
 * Order, matching the firmware's own guest accesses:
 *   1. TOC slot preflight and the two register gates (0x12540 taskset == 0, 0x12568 taskset & 0x7F);
 *      a refused register gate reads no guest byte;
 *   2. 0x11ACC's gates in ITS order, each reading only what the dump reads where it reads it:
 *      0x11B68 elf == 0, 0x11B88 elf & 0xF, 0x11BE0 context & 0xF, 0x11BF4 size, then -- and only
 *      now -- the ELF header (0x11C00 ident, 0x11D90 machine, 0x11D9C entry, 0x11DA8 phnum,
 *      0x11DAC phoff, 0x11DD4 phentsize) and each program header inside the walk;
 *   3. the lsPattern probe and the top-bits gate, both over the 16
 *      guest bytes behind r8 -- the POINTER, never the pointer's value;
 *   4. the slot sweep's taskset+0x30 words and the ready set's +0x20 words;
 *   5. the argument block behind r9;
 *   6. the plan: REFUSED publishes the code and writes nothing;
 *   7. on OK each action as its own traced HLE write, in firmware order;
 *   8. the signal through the native ready-count path (0xA750 mask, 0x85D0 byte);
 *   9. ONLY after the signal returned 0: the caller's *taskId is stored.
 *
 * The id 0x11ACC produces goes to ITS frame slot (passed in as a stack address and
 * written there), not to the caller's pointer. So a refused signal leaves the
 * caller's *taskId untouched, exactly as the firmware does.
 *
 * No SPU thread, group, port or event is started, signalled or woken. The syscall that reads the process
 * SDK is the only syscall on the path and comes from execution.process_sdk; 0x9124 / 0x1309C /
 * 0x12F6C are log helpers with no guest-memory effect. */
enum : uint32_t {
    spurs_task_create_nid = 0xbeb600ac,
    /* The ELF header fields 0x11ACC reads. */
    offsetof_task_elf_ident = 0x00u,
    offsetof_task_elf_machine = 0x12u,
    offsetof_task_elf_entry = 0x18u,
    offsetof_task_elf_ph_offset = 0x1Cu,
    offsetof_task_elf_ph_entry_size = 0x2Au,
    offsetof_task_elf_ph_num = 0x2Cu,
    offsetof_task_phdr_type = 0x00u,
    offsetof_task_phdr_file_size = 0x08u,
    /* The two taskset set words the call reads: the allocation set (+0x30) and the ready set
     * (+0x20). Both are four words of 32 bits. */
    offsetof_task_set_allocated = 0x30u,
    offsetof_task_set_ready = 0x20u,
    /* The taskset's own fields 0x10C6C reads to address the SPURS instance. */
    offsetof_task_spurs = 0x64u,
    offsetof_task_wid = 0x74u
};

static void dispatch_spurs_task_create(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    if (control.task_calls != SIZE_MAX) ++control.task_calls;
    control.task_last = 0xFFFFFFFFu;
    control.task_id = 0xFFFFFFFFu;
    control.task_writes = 0;
    control.task_ready_raised = 0;
    control.task_signal_written = 0;
    control.task_ready_address = 0;
    control.task_mask_bit = 0;
    control.task_slot = 0xFFu;
    control.task_id_written = 0;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);

    /* r3 taskset, r4 taskId ptr, r5 elf, r6 context, r7 size, r8 lsPattern ptr, r9 argument ptr
     * r8 and r9 are POINTERS the firmware dereferences; the
     * entry zero-extends r5..r9, so the low 32 bits are the EAs. */
    spurs_task_create_request request{};
    request.taskset = uint32_t(cpu.gpr[3]);
    request.task_id_ptr = uint32_t(cpu.gpr[4]);
    request.elf_address = uint32_t(cpu.gpr[5]);
    request.context = uint64_t(uint32_t(cpu.gpr[6]));
    request.size = uint32_t(cpu.gpr[7]);
    request.ls_pattern_ptr = uint32_t(cpu.gpr[8]);
    request.argument_ptr = uint32_t(cpu.gpr[9]);

    /* register-only gates, so a refused call touches no guest byte. */
    {
        uint32_t gate_code = 0;
        if (ps3_spurs_task_create_validate_taskset(&request, &gate_code) != PS3_SPURS_TKC_STATUS_OK) {
            control.task_last = gate_code;
            spurs_taskset_return(execution, toc, toc_slot, gate_code, control.task_returns);
            return;
        }
    }

    /* 0x11ACC's own gate order. Each early refusal reads nothing: the ELF is opened only after
     * 0x11B68 / 0x11B88 / 0x11BE0 / 0x11BF4 have all passed, which is the dump's order. */
    {
        const uint32_t elf_gate = request.elf_address;
        const bool elf_gate_ok = elf_gate != 0u && (elf_gate & PS3_SPURS_TKC_TASK_ALIGN) == 0u;
        const bool context_gate_ok = (request.context & PS3_SPURS_TKC_TASK_ALIGN) == 0u;
        const bool size_gate_ok = !(request.context != 0u && request.size <= PS3_SPURS_TKC_MIN_SIZE);
        if (!elf_gate_ok || !context_gate_ok || !size_gate_ok) {
            /* Answer from the same ordered gates the domain applies, without any guest read. */
            uint32_t code = 0;
            (void)ps3_spurs_task_create_validate(&request, &code);
            control.task_last = code;
            spurs_taskset_return(execution, toc, toc_slot, code, control.task_returns);
            return;
        }
    }

    /* in the dump's order: the 8-byte ident compare, then e_machine, e_entry,
     * e_phnum/e_phoff. e_phentsize is read at 0x11DD4, INSIDE the walk, so it is read there. */
    request.elf.ident = spurs_taskset_read_be(execution,
        spurs_attribute_field(request.elf_address, offsetof_task_elf_ident, 8u), 8u);
    if (request.elf.ident != PS3_SPURS_TKC_ELF_IDENT) {
        control.task_last = PS3_SPURS_TKC_ELF_BAD;
        spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_ELF_BAD, control.task_returns);
        return;
    }
    request.elf.machine = uint16_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.elf_address, offsetof_task_elf_machine, 2u), 2u));
    if (request.elf.machine != PS3_SPURS_TKC_EM_SPU) {
        control.task_last = PS3_SPURS_TKC_ELF_BAD;
        spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_ELF_BAD, control.task_returns);
        return;
    }
    request.elf.entry = uint32_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.elf_address, offsetof_task_elf_entry, 4u), 4u));
    if (request.elf.entry <= PS3_SPURS_TKC_ENTRY_MAX) {   /* root g17e */
        control.task_last = PS3_SPURS_TKC_ELF_BAD;
        spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_ELF_BAD, control.task_returns);
        return;
    }
    request.elf.ph_num = uint16_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.elf_address, offsetof_task_elf_ph_num, 2u), 2u));
    request.elf.ph_offset = uint32_t(spurs_taskset_read_be(execution,
        spurs_attribute_field(request.elf_address, offsetof_task_elf_ph_offset, 4u), 4u));
    /* walk one header at a time, bounded only by e_phnum, exactly as the firmware
     * does. A PT_LOAD with p_filesz <= 0x2FFF refuses. */
    if (request.elf.ph_num != 0u) {
        uint32_t header = request.elf_address + request.elf.ph_offset;
        for (unsigned index = 0; index < request.elf.ph_num; ++index) {
            const uint32_t type = uint32_t(spurs_taskset_read_be(execution,
                spurs_attribute_field(header, offsetof_task_phdr_type, 4u), 4u));
            if (type == PS3_SPURS_TKC_PT_LOAD) {
                const uint32_t file_size = uint32_t(spurs_taskset_read_be(execution,
                    spurs_attribute_field(header, offsetof_task_phdr_file_size, 4u), 4u));
                if (file_size <= PS3_SPURS_TKC_SEGMENT_MAX) {
                    control.task_last = PS3_SPURS_TKC_ELF_BAD;
                    spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_ELF_BAD,
                        control.task_returns);
                    return;
                }
            }
            /* e_phentsize, read here inside the walk. */
            request.elf.ph_entry_size = uint16_t(spurs_taskset_read_be(execution,
                spurs_attribute_field(request.elf_address, offsetof_task_elf_ph_entry_size, 2u), 2u));
            header += request.elf.ph_entry_size;
        }
    }
    /* `ph_num` is walked above; the domain re-checks the same predicates on the count only. */
    request.elf.segments = nullptr;
    request.elf.segment_count = 0u;
    request.elf.ph_num = 0u;

    /* Only a NON-ZERO lsPattern pointer runs the
     * probe, and only on a non-zero context (a zero context takes the shortcut ->
     * 0x11E10 and never reads it). 0x11C38..0x11D68 read the 16 bytes behind r25, 0x11D74 reads
     * its first word. */
    if (request.context != 0u && request.ls_pattern_ptr != 0u) {
        for (size_t index = 0; index < PS3_SPURS_TKC_BLOCK_BYTES; ++index) {
            request.ls_pattern_block[index] = (unsigned char)spurs_taskset_read_be(execution,
                request.ls_pattern_ptr + uint32_t(index), 1u);
        }
        if (ps3_spurs_task_ls_pattern_count(request.ls_pattern_block) >
                ps3_spurs_task_alignment_budget(request.size) ||
            ((uint32_t)ps3_spurs_task_load_be(request.ls_pattern_block, 4u) &
             PS3_SPURS_TKC_LS_PATTERN_MASK) != 0u) {
            control.task_last = PS3_SPURS_TKC_INVAL;
            spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_INVAL, control.task_returns);
            return;
        }
    }

    /* The sweep's allocation words and the ready set, both four words of 32 bits. */
    for (size_t index = 0; index < PS3_SPURS_TKC_SET_WORDS; ++index) {
        request.allocated_words[index] = uint32_t(spurs_taskset_read_be(execution,
            request.taskset + offsetof_task_set_allocated + uint32_t(index) * 4u, 4u));
        request.ready_words[index] = uint32_t(spurs_taskset_read_be(execution,
            request.taskset + offsetof_task_set_ready + uint32_t(index) * 4u, 4u));
    }

    /* the 16 bytes behind r9. */
    if (request.argument_ptr != 0u) {
        for (size_t index = 0; index < PS3_SPURS_TKC_BLOCK_BYTES; ++index) {
            request.argument_block[index] = (unsigned char)spurs_taskset_read_be(execution,
                request.argument_ptr + uint32_t(index), 1u);
        }
    }

    spurs_task_create_plan plan{};
    if (ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_REFUSED) {
        control.task_last = plan.result_code;
        spurs_taskset_return(execution, toc, toc_slot, plan.result_code, control.task_returns);
        return;
    }
    control.task_slot = unsigned(plan.slot);

    /* Every action in firmware order: the six record fields and the allocation bit, then 0x10C6C's
     * ready bit. Each is one traced HLE write of its own width. */
    for (size_t index = 0; index < plan.write_count; ++index) {
        const spurs_task_write &write = plan.writes[index];
        if (!writable(execution, write.address, write.length)) {
            execution.control->invalid_address = write.address;
            stop(StartupStop::InvalidMemory);
        }
        spurs_taskset_write_image(execution, write.address, write.bytes, 0, write.length,
            control.task_writes);
    }

    /* the signal addresses come from the taskset's own fields, which this call did
     * not write (g13 wrote them). */
    const uint32_t taskset_spurs = uint32_t(spurs_taskset_read_be(execution,
        request.taskset + offsetof_task_spurs, 4u));
    const uint32_t workload_id = uint32_t(spurs_taskset_read_be(execution,
        request.taskset + offsetof_task_wid, 4u));
    spurs_task_signal signal{};
    if (ps3_spurs_task_signal_build(request.taskset, taskset_spurs, workload_id, &signal) !=
        PS3_SPURS_TKC_STATUS_OK) {
        const uint32_t code = taskset_spurs == 0u ? PS3_SPURS_TKC_NULL : PS3_SPURS_TKC_ALIGN;
        control.task_last = code;
        spurs_taskset_return(execution, toc, toc_slot, code, control.task_returns);
        return;
    }
    control.task_ready_address = signal.ready_count_address;
    control.task_mask_bit = signal.ready_mask_bit;

    if (!execution.spurs) stop(StartupStop::MissingServices);
    if (!writable(execution, signal.ready_count_address, 4)) spurs_attribute_invalid_memory(signal.ready_count_address);
    if (!writable(execution, signal.signal_address, 1)) spurs_attribute_invalid_memory(signal.signal_address);
    check_boundary(BoundaryBudget::Poll);
    {
        SpursAttachMemory context{&execution};
        const ps3_spurs_ready_count_memory_scope memory_scope{&context, spurs_operation_permits};
        ps3_spurs_ready_mask_request mask_request{};
        mask_request.spurs = taskset_spurs;
        mask_request.wid = workload_id;
        mask_request.mask_bit = signal.ready_mask_bit;
        ps3_spurs_ready_mask_result mask_result{};
        /* The owned SPURS1 instance the raise targets. ps3_spurs_runtime_ready_count_store owns
         * exactly this resolution for its own request; it is repeated here against the mask form so
         * that spurs_instance.{c,h} -- which are not this task's files -- stay untouched. */
        const ps3_spurs_instance *owned_instance = nullptr;
        for (unsigned index = 0; index < PS3_SPURS_MAX_INSTANCES; ++index)
            if (execution.spurs->instances[index].state == PS3_SPURS_INSTANCE_ACTIVE &&
                execution.spurs->instances[index].spurs_ea == mask_request.spurs)
                owned_instance = &execution.spurs->instances[index];
        if (!owned_instance || !execution.spurs->workload_services_configured ||
            !owned_instance->kernel_initialized)
            stop(StartupStop::SpursTaskCreateStop);
        {
            const ps3_spurs_ready_count_config mask_config{
                execution.spurs->config.domain, execution.ppu.reservation, mask_request.spurs,
                PS3_SPURS_EXTENT, 1u, &context, spurs_registration_poll, &context,
                spurs_ready_observe, memory_scope};
            ps3_spurs_ready_mask_apply(&mask_config, &mask_request, &mask_result);
        }
        control.task_ready_raised = mask_result.mask_written;
        control.task_signal_written = mask_result.signal_written;
        if (context.trace_failure) stop(StartupStop::TraceCapacity);
        if (context.latched != StartupStop::None) stop(context.latched);
        if (context.callback_error || mask_result.stop != PS3_SPURS_READY_COUNT_RUNNING ||
            !mask_result.has_sdk_result) stop(StartupStop::SpursTaskCreateStop);
        if (mask_result.sdk_result != 0u) {
            /* 0x8041080F becomes 0x8041090F; any other code passes through and
             * 0x12520 returns it before the `*taskId` store. */
            const uint32_t code = mask_result.sdk_result == PS3_SPURS_TKC_POLICY_STAT
                ? PS3_SPURS_TKC_STAT : mask_result.sdk_result;
            control.task_last = code;
            spurs_taskset_return(execution, toc, toc_slot, code, control.task_returns);
            return;
        }
    }

    /* the id reaches the CALLER's pointer
     * here, and nowhere earlier. A refused signal above returned before this line. */
    if (request.task_id_ptr != 0u) {
        unsigned char id_image[4]{};
        for (size_t index = 0; index < sizeof(id_image); ++index)
            id_image[index] = (unsigned char)(plan.slot >> (8u * (3u - index)));
        spurs_taskset_write_image(execution, request.task_id_ptr, id_image, 0, sizeof(id_image),
            control.task_writes);
        control.task_id_written = 1;
    }
    control.task_id = plan.slot;
    control.task_last = PS3_SPURS_TKC_OK;
    spurs_taskset_return(execution, toc, toc_slot, PS3_SPURS_TKC_OK, control.task_returns);
}

static int spurs_workload_info_observe(void *opaque, const ps3_spurs_workload_info_event *event)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &observation = context.execution->control->spurs_workload_info;
    if (!event || !event->bytes || !spurs_attach_scalar(event->length) ||
        (event->operation != PS3_SPURS_WORKLOAD_INFO_READ && event->operation != PS3_SPURS_WORKLOAD_INFO_WRITE)) {
        spurs_attach_refuse(context);
        return 0;
    }
    if (!spurs_attach_reserve(context)) return 0;
    spurs_attach_publish(context, event->operation == PS3_SPURS_WORKLOAD_INFO_WRITE, event->address,
        event->length, spurs_attach_be(event->bytes, event->length));
    spurs_attach_close(context);
    if (context.trace_failure) return 0;
    ++observation.publications;
    return 1;
}
static void dispatch_spurs_workload_info(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_workload_info = {};
    auto &observation = control.spurs_workload_info;
    if (control.spurs_workload_info_calls != SIZE_MAX) ++control.spurs_workload_info_calls;
    for (size_t i = 0; i < 3; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    observation.request = {uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5])};
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    uint32_t sdk = 0;
    if (!ps3_spurs_workload_info_validate_address(&observation.request, &sdk)) stop(StartupStop::ProviderFailure);
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_workload_info_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_workload_info_memory_scope memory_scope{&context, spurs_operation_permits};
    ps3_spurs_runtime_get_workload_info(execution.spurs, &observation.request, memory_scope, execution.ppu.reservation, &context,
        spurs_registration_poll, &context, spurs_workload_info_observe, &observation.domain_result);
    observation.has_domain_result = true;
    observation.trace_failure = context.trace_failure;
    observation.latched_stop = context.latched;
    observation.memory_status = observation.domain_result.memory_status;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) {
        if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(context.latched);
    }
    if (spurs_fence_pending(control)) {
        observation.latched_stop = spurs_fence_stop(control);
        stop(observation.latched_stop);
    }
    if (context.callback_error || observation.domain_result.stop != PS3_SPURS_WORKLOAD_INFO_RUNNING ||
        !observation.domain_result.has_sdk_result) stop(StartupStop::SpursWorkloadInfoStop);
    spurs_workload_sdk_error(execution, observation, toc_slot, toc,
        observation.domain_result.sdk_result, control.spurs_workload_info_returns);
}
enum : uint32_t { spurs_set_exception_nid = 0xd2e23fa9, spurs_unset_exception_nid = 0x4c75deb8 };
static constexpr size_t spurs_exception_claim_attempt_limit = 64;
static int spurs_exception_observe(void *opaque, const ps3_spurs_exception_event *event)
{
    auto &context = *static_cast<SpursAttachMemory *>(opaque);
    auto &observation = context.execution->control->spurs_exception;
    if (!event || !event->bytes || !spurs_attach_scalar(event->length) ||
        (event->operation != PS3_SPURS_EXCEPTION_READ && event->operation != PS3_SPURS_EXCEPTION_WRITE)) {
        spurs_attach_refuse(context);
        return 0;
    }
    if (!spurs_attach_reserve(context)) return 0;
    spurs_attach_publish(context, event->operation == PS3_SPURS_EXCEPTION_WRITE, event->address,
        event->length, spurs_attach_be(event->bytes, event->length));
    spurs_attach_close(context);
    if (context.trace_failure) return 0;
    ++observation.publications;
    return 1;
}
static void dispatch_spurs_exception(StartupExecution &execution, uint32_t nid)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    control.spurs_exception = {};
    auto &observation = control.spurs_exception;
    observation.nid = nid;
    if (control.spurs_exception_calls != SIZE_MAX) ++control.spurs_exception_calls;
    for (size_t i = 0; i < 4; ++i) observation.arguments[i] = cpu.gpr[3 + i];
    const bool is_set = nid == spurs_set_exception_nid;
    observation.request = {is_set ? PS3_SPURS_EXCEPTION_SET : PS3_SPURS_EXCEPTION_UNSET,
        uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), is_set ? uint32_t(cpu.gpr[5]) : 0,
        is_set ? uint32_t(cpu.gpr[6]) : 0};
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2];
    const uint32_t toc_slot = spurs_workload_toc_slot(execution);
    uint32_t sdk = 0;
    if (!ps3_spurs_exception_validate_address(&observation.request, &sdk)) stop(StartupStop::ProviderFailure);
    if (sdk) {
        spurs_workload_sdk_error(execution, observation, toc_slot, toc, sdk, control.spurs_exception_returns);
        return;
    }
    if (!execution.spurs) stop(StartupStop::MissingServices);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_exception_memory_scope memory_scope{&context, spurs_operation_permits};
    ps3_spurs_runtime_exception_handler(execution.spurs, &observation.request, memory_scope,
        execution.ppu.reservation, spurs_exception_claim_attempt_limit, &context, spurs_registration_poll,
        &context, spurs_exception_observe, &observation.domain_result);
    observation.has_domain_result = true;
    observation.trace_failure = context.trace_failure;
    observation.latched_stop = context.latched;
    observation.memory_status = observation.domain_result.memory_status;
    if (context.trace_failure) stop(StartupStop::TraceCapacity);
    if (context.latched != StartupStop::None) {
        if (context.latched == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(context.latched);
    }
    if (spurs_fence_pending(control)) {
        observation.latched_stop = spurs_fence_stop(control);
        stop(observation.latched_stop);
    }
    if (context.callback_error || observation.domain_result.stop != PS3_SPURS_EXCEPTION_RUNNING ||
        !observation.domain_result.has_sdk_result) stop(StartupStop::SpursExceptionStop);
    spurs_workload_sdk_error(execution, observation, toc_slot, toc,
        observation.domain_result.sdk_result, control.spurs_exception_returns);
}

// cellSpursGetInfo (NID 0x1f402f8f) through ps3_spurs_runtime_get_info. The domain owns the check order, the field order/layout and the
// report; this adapter owns the NID/ABI decode (r3/r4 are the low 32 bits), the TOC slot/trace-credit preflight, the coherent memory port
// with one credited Hle publication per ACTUAL access, cancel/deadline/budget/fence polling before every access and the r3/TOC result.
// Callbacks never stop()/longjmp: a refusal returns nonzero (nothing performed), latches the reason, and the dispatcher stops after the
// domain returned. Output bytes already written are never restored (the reference has no rollback). Counters live ONLY in spurs_info_*.
enum : uint32_t { spurs_get_info_nid = 0x1f402f8f };
// Pure mapping of one finished domain call (precedence: trace failure > latched refusal > domain STOP > FAILED > OK). Only a clean OK
// may reach the guest as 0; a performed write whose publication failed is latched as TraceCapacity, never as "nothing performed".
static SpursAttachOutcome spurs_info_classify(const ps3_spurs_result &result, const SpursAttachMemory &ctx)
{
    if (ctx.trace_failure) return {SpursAttachDisposition::Stop, StartupStop::TraceCapacity};
    if (ctx.latched != StartupStop::None) return {SpursAttachDisposition::Stop, ctx.latched};
    if (result.outcome == PS3_SPURS_OUTCOME_STOP) return {SpursAttachDisposition::Stop, StartupStop::SpursInfoStop};
    if (result.outcome == PS3_SPURS_OUTCOME_OK) return {SpursAttachDisposition::PublishOk, StartupStop::None};
    if (result.outcome == PS3_SPURS_OUTCOME_FAILED) return {SpursAttachDisposition::PublishFailed, StartupStop::None};
    return {SpursAttachDisposition::Stop, StartupStop::ProviderFailure};
}
// Validation preflight (called once by the domain, after ownership/range/overlap and before any access): poll, every span readable /
// writable, and credit for the worst case (71 accesses) PLUS the TOC publication. Order: boundary > memory > trace credit. Nothing performed.
static int spurs_info_preflight(void *context, const ps3_spurs_span *reads, size_t read_count, const ps3_spurs_span *writes, size_t write_count,
    uint32_t max_accesses)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    if (!reads || !writes || max_accesses > PS3_SPURS_INFO_MAX_ACCESSES || !spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    for (size_t i = 0; i < read_count; ++i)
        if (!spurs_attach_readable(execution, reads[i].address, reads[i].length)) return spurs_attach_refuse_memory(ctx, reads[i].address);
    for (size_t i = 0; i < write_count; ++i)
        if (!writable(execution, writes[i].address, writes[i].length)) return spurs_attach_refuse_memory(ctx, writes[i].address);
    if (!trace_can_begin(execution, size_t(max_accesses) + 1)) { ctx.trace_failure = true; return spurs_attach_refuse(ctx); }
    return 0;
}
// A publication that failed AFTER a performed access leaves trace_failure set: the next callback refuses (nothing performed) so the
// domain stops, while the access that already happened stays counted and the dispatcher reports TraceCapacity.
static bool spurs_info_scalar(size_t length) { return length == 1 || length == 4 || length == 8; }
static int spurs_info_read(void *context, uint32_t address, void *bytes, size_t length)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    if (ctx.trace_failure || !spurs_info_scalar(length) || !spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    if (!spurs_attach_readable(execution, address, length)) return spurs_attach_refuse_memory(ctx, address);
    if (!spurs_attach_reserve(ctx)) return spurs_attach_refuse(ctx);
    const bool read = ps3_atomic_read(execution.ppu.memory->domain, address, bytes, length) == PS3_GUEST_MEMORY_OK;
    if (read) {
        ++execution.control->spurs_info_reads;
        spurs_attach_publish(ctx, false, address, length, spurs_attach_be(bytes, length));
    }
    spurs_attach_close(ctx);
    return read ? 0 : spurs_attach_refuse_memory(ctx, address);
}
static int spurs_info_write(void *context, uint32_t address, const void *bytes, size_t length)
{
    auto &ctx = *static_cast<SpursAttachMemory *>(context);
    StartupExecution &execution = *ctx.execution;
    if (ctx.trace_failure || !spurs_info_scalar(length) || !spurs_attach_poll(ctx)) return spurs_attach_refuse(ctx);
    if (!writable(execution, address, length)) return spurs_attach_refuse_memory(ctx, address);
    if (!spurs_attach_reserve(ctx)) return spurs_attach_refuse(ctx);
    const bool written = ps3_atomic_write(execution.ppu.memory->domain, address, bytes, length) == PS3_GUEST_MEMORY_OK;
    if (written) {
        ++execution.control->spurs_info_writes;
        // The byte IS written: a journal refusal is latched (the dispatcher stops, the next callback refuses) but this call returns 0.
        spurs_attach_publish(ctx, true, address, length, spurs_attach_be(bytes, length));
    }
    spurs_attach_close(ctx);
    return written ? 0 : spurs_attach_refuse_memory(ctx, address);
}
static void dispatch_spurs_get_info(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    if (control.spurs_info_calls != SIZE_MAX) ++control.spurs_info_calls;
    for (size_t i = 0; i < 2; ++i) control.spurs_info_arguments[i] = cpu.gpr[3 + i];
    control.spurs_info_has_result = false;   // the remaining spurs_info_* diagnostics describe the LAST call
    control.spurs_info_callback_error = control.spurs_info_trace_failure = false;
    control.spurs_info_reads = control.spurs_info_writes = control.spurs_info_invalid_address = 0;
    control.spurs_info_stop_latched = StartupStop::None;
    control.spurs_info_result = {};
    control.spurs_info_report = {};
    // Every check below is BEFORE any effect. The SDK is not consulted (the query takes no SDK).
    if (!execution.spurs || !execution.ppu.memory) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu.gpr[2], toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1] || toc_slot > UINT32_MAX - 7) spurs_attribute_invalid_memory(toc_slot);
    if (!writable(execution, uint32_t(toc_slot), 8)) spurs_attribute_invalid_memory(toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    SpursAttachMemory context{&execution};
    const ps3_spurs_info_memory memory{&context, spurs_info_preflight, spurs_info_read, spurs_info_write};
    // r3 CellSpurs*, r4 CellSpursInfo*: each is the low 32 bits of its register.
    const ps3_spurs_info_request request{uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4])};
    ps3_spurs_info_report report{};
    const ps3_spurs_result result = ps3_spurs_runtime_get_info(execution.spurs, &request, &memory, &report);
    control.spurs_info_result = result;
    control.spurs_info_report = report;
    control.spurs_info_has_result = true;
    control.spurs_info_callback_error = context.callback_error || context.trace_failure;
    control.spurs_info_trace_failure = context.trace_failure;
    control.spurs_info_invalid_address = context.invalid_address;
    control.spurs_info_stop_latched = context.latched;
    // Output bytes already written stay (no rollback); they are described by report.writes/partial and the actual counters.
    const SpursAttachOutcome outcome = spurs_info_classify(result, context);
    if (outcome.disposition == SpursAttachDisposition::Stop) {
        if (outcome.stop == StartupStop::InvalidMemory) active->invalid_address = context.invalid_address;
        stop(outcome.stop);
    }
    check_boundary(BoundaryBudget::Poll);   // cancel/deadline/budget/fence AFTER the domain call, BEFORE the TOC write and r3
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    publish_toc(execution, toc_slot, toc, batch);
    finish_owned(&batch);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = outcome.disposition == SpursAttachDisposition::PublishOk ? 0 : uint64_t(int64_t(int32_t(result.rc)));
    if (control.spurs_info_returns != SIZE_MAX) ++control.spurs_info_returns;
}
// sys_ppu_thread_create (NID 0x24a1ea07) through the generic borrowed StartupPpuCreatePort (contract in the header). The bridge only
// decodes, validates and sequences; it allocates nothing itself and never rolls back, joins or frees the provider-owned child.
// Every check that can fail on caller data runs BEFORE the provider is touched; the output trace reservation is taken before
// prepare() and closed before start(), so no reservation, pin or guard is held while start() waits for the child.
constexpr uint32_t ppu_thread_create_nid = 0x24a1ea07;
static bool ppu_create_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory || length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
static void dispatch_ppu_thread_create(StartupExecution &execution, ppu_context &cpu)
{
    StartupControl &control = *execution.control;
    HleTraceBatch batch;
    if (control.ppu_create_calls != SIZE_MAX) ++control.ppu_create_calls;
    const StartupPpuCreatePort *const port = execution.ppu_create;
    const Ps3PpuExecutionServices *const services = execution.ppu.services;
    if (!port || !port->prepare || !port->start || !services || !services->compiled_code || !services->compiled_code->find ||
        !control.initialized_tls_template_valid || !execution.has_guest_priority ||
        execution.guest_priority < 0 || execution.guest_priority > 3071) stop(StartupStop::MissingServices, &batch);
    StartupPpuCreatePrepareResult (*const prepare)(void *, const StartupPpuCreateRequest *, const Ps3TlsTemplate *) = port->prepare;
    StartupPpuCreateStartStatus (*const start)(void *, uint32_t) = port->start;
    void *const context = port->context;
    const Ps3TlsTemplate tls = control.initialized_tls_template;   // the provider receives a copy, never the live snapshot
    // Capture every argument before any register or guest byte changes.
    const uint64_t toc = cpu.gpr[2], output = cpu.gpr[3], opd = cpu.gpr[4], name = cpu.gpr[9];
    const uint64_t toc_slot = cpu.gpr[1] + 0x28;
    StartupPpuCreateRequest request;
    request.system_parameters = execution.system_parameters;
    request.system_profile = execution.system_profile;
    request.presentation_topology = execution.presentation_topology;
    request.presentation_mode = execution.presentation_mode;
    request.disc_change_callbacks = execution.disc_change_callbacks;
#ifdef PS3_NATIVE_GAMEDATA
    request.gamedata = execution.gamedata;
#endif
    request.process_sdk = execution.process_sdk;
    request.boot_content = execution.boot_content;
    request.argument = cpu.gpr[5];
    request.priority = int32_t(cpu.gpr[6]);
    request.stack_size = uint32_t(cpu.gpr[7]);
    request.flags = cpu.gpr[8];
    request.creator_thread_id = cpu.thread_id;
    request.creator_priority = execution.guest_priority;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    // Output u64: canonical, nonnull, 4-aligned, writable and readable now (the write happens only after prepare).
    // Root fix (C1b r2, hardware + Eden 2026-11-11): the rule was "8-aligned", an assumption no reference pinned. The game
    // passes 0x404FEE4C (r3 at the stop, from 0x4432B4: the id field of its own worker record) -- a u64 at a 4-aligned
    // address, which a PPU stores to without any fault. The id is written through the byte-wise memory port, so no
    // host alignment is involved either. 4 is what the evidence shows; anything odd still stops.
    if (!output || output > UINT32_MAX || output % 4 || !writable(execution, uint32_t(output), 8)) stop(StartupStop::InvalidMemory, &batch);
    // Saved-TOC slot (r1 + 0x28). Overlap with the output would let the TOC write destroy the id the child observes.
    if (toc_slot < cpu.gpr[1] || toc_slot > UINT32_MAX || !writable(execution, uint32_t(toc_slot), 8) ||
        (output < toc_slot + 8 && toc_slot < output + 8)) stop(StartupStop::InvalidMemory, &batch);
    uint64_t probe = 0;
    if (memory.read(memory.context, uint32_t(output), &probe, sizeof probe) != PS3_GUEST_MEMORY_OK ||
        memory.read(memory.context, uint32_t(toc_slot), &probe, sizeof probe) != PS3_GUEST_MEMORY_OK) stop(StartupStop::InvalidMemory, &batch);
    // Only the joinable form is supported; every other value would need an unpinned guest error, so it is a stop.
    // Root (2026-11-12, Decision 82, console runs rv8/rv9): flags 0 is the plain non-joinable thread. The game creates
    // one from 0x10BF4C (priority 1001, stack 0x4000) right after its install scan, and the stop below ended both
    // console runs there at second 410. It runs like a joinable thread that nobody joins; its slot is released at
    // teardown instead of at its exit. Interrupt threads (flag 2) still stop.
    if (request.flags == 0) request.flags = 1;
    if (request.flags != 1) stop(StartupStop::UnsupportedSyscall, &batch);
    if (request.priority < 0 || request.priority > 3071 || !request.stack_size) stop(StartupStop::UnsupportedSyscall, &batch);
    // OPD{BEu32 entry, BEu32 TOC} through the coherent tracked memory.
    if (!opd || opd > UINT32_MAX || opd % 4 || !ppu_create_readable(execution, uint32_t(opd), 8)) stop(StartupStop::InvalidMemory, &batch);
    unsigned char descriptor[8]{};
    if (memory.read(memory.context, uint32_t(opd), descriptor, sizeof descriptor) != PS3_GUEST_MEMORY_OK) stop(StartupStop::InvalidMemory, &batch);
    request.opd_address = uint32_t(opd);
    request.entry = uint32_t(descriptor[0]) << 24 | uint32_t(descriptor[1]) << 16 | uint32_t(descriptor[2]) << 8 | descriptor[3];
    request.toc = uint32_t(descriptor[4]) << 24 | uint32_t(descriptor[5]) << 16 | uint32_t(descriptor[6]) << 8 | descriptor[7];
    request.output = uint32_t(output);
    // The compiled lookup must know the entry BEFORE the provider allocates anything for it.
    ps3_compiled_function target = nullptr;
    if (!request.entry || request.entry % 4 ||
        services->compiled_code->find(services->compiled_code->context, request.entry, &target) != PS3_COMPILED_CODE_OK || !target)
        stop(StartupStop::DispatchFailure, &batch);
    // Name: no pinned reference allows a null name, so null is unsupported; otherwise a NUL must appear within the scan bound.
    if (!name) stop(StartupStop::UnsupportedSyscall, &batch);
    if (name > UINT32_MAX) stop(StartupStop::InvalidMemory, &batch);
    request.name_address = uint32_t(name);
    bool terminated = false;
    for (uint32_t index = 0; index < STARTUP_PPU_CREATE_NAME_SCAN && !terminated; ++index) {
        unsigned char character = 0;
        if (!ppu_create_readable(execution, uint32_t(name) + index, 1) || uint64_t(name) + index > UINT32_MAX ||
            memory.read(memory.context, uint32_t(name) + index, &character, 1) != PS3_GUEST_MEMORY_OK) stop(StartupStop::InvalidMemory, &batch);
        request.name[index] = char(character);
        if (!character) { request.name_length = index; terminated = true; }
    }
    if (!terminated) stop(StartupStop::UnsupportedSyscall, &batch);
    // The id and saved-TOC records are reserved BEFORE the provider can allocate.
    if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity, &batch);
    control.ppu_create_last_entry = request.entry; control.ppu_create_last_toc = request.toc;
    control.ppu_create_last_output = request.output; control.ppu_create_last_priority = request.priority;
    control.ppu_create_last_stack = request.stack_size; control.ppu_create_last_argument = request.argument;
    control.ppu_create_last_flags = request.flags;
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 2, batch);
    const StartupPpuCreatePrepareResult prepared = prepare(context, &request, &tls);
    control.ppu_create_prepare = prepared.status;
    control.ppu_create_has_prepare = true;
    switch (prepared.status) {
    case StartupPpuCreatePrepareStatus::Prepared:
        if (!prepared.thread_id) stop(StartupStop::ProviderFailure, &batch);   // no id may be invented or written
        break;
    case StartupPpuCreatePrepareStatus::GuestError:
        if (!prepared.cell) stop(StartupStop::ProviderFailure, &batch);       // a "completed error" of 0 would be a fake success
        control.ppu_create_last_cell = prepared.cell;
        if (control.ppu_create_guest_errors != SIZE_MAX) ++control.ppu_create_guest_errors;
        check_boundary(BoundaryBudget::Poll, &batch);
        publish_toc(execution, toc_slot, toc, batch);   // the stub's TOC save still happens; no id, no child, no start
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[2] = toc;
        cpu.gpr[3] = uint64_t(int64_t(int32_t(prepared.cell)));
        if (control.ppu_create_returns != SIZE_MAX) ++control.ppu_create_returns;
        return;
    case StartupPpuCreatePrepareStatus::Unsupported: stop(StartupStop::UnsupportedSyscall, &batch);
    case StartupPpuCreatePrepareStatus::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    default: stop(StartupStop::ProviderFailure, &batch);   // never trust an out-of-range status
    }
    // The provider now owns a prepared child. Every stop below leaves it owned for the composition root's teardown.
    const uint32_t thread_id = prepared.thread_id;
    control.ppu_create_last_thread_id = thread_id;
    if (control.ppu_create_prepared != SIZE_MAX) ++control.ppu_create_prepared;
    check_boundary(BoundaryBudget::Poll, &batch);   // cancel BEFORE the id is published
    const uint64_t encoded_id = __builtin_bswap64(uint64_t(thread_id));
    if (memory.write(memory.context, uint32_t(output), &encoded_id, sizeof encoded_id) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    if (control.ppu_create_id_written != SIZE_MAX) ++control.ppu_create_id_written;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, uint32_t(output), 8, thread_id), batch);
    publish_toc(execution, toc_slot, toc, batch);
    finish_owned(&batch);                           // nothing may stay reserved while start() waits for the child
    check_boundary(BoundaryBudget::Poll);           // cancel BEFORE the child is started
    const StartupPpuCreateStartStatus started = start(context, thread_id);   // may block; no reservation, pin or guard is held
    control.ppu_create_start = started;
    control.ppu_create_has_start = true;
    switch (started) {
    case StartupPpuCreateStartStatus::Started:
        if (control.ppu_create_started != SIZE_MAX) ++control.ppu_create_started;
        break;
    case StartupPpuCreateStartStatus::Terminal:   // diagnostic stop: the game helper cannot return, so no success is delivered
        if (control.ppu_create_terminal != SIZE_MAX) ++control.ppu_create_terminal;
        stop(StartupStop::ProviderFailure, &batch);
    case StartupPpuCreateStartStatus::Unsupported: stop(StartupStop::UnsupportedSyscall, &batch);
    case StartupPpuCreateStartStatus::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    default: stop(StartupStop::ProviderFailure, &batch);
    }
    check_boundary(BoundaryBudget::Poll);           // the started child stays; cancellation only suppresses r3
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    if (control.ppu_create_returns != SIZE_MAX) ++control.ppu_create_returns;
}
// lv2 syscall 44 (sys_ppu_thread_join) through the same borrowed StartupPpuCreatePort (contract in the header). The guest call is
// r3 = the u64 thread id the create bridge wrote, r4 = a pointer to the u64 exit-value output. Only a Joined result is a guest
// result: NotFound/NotExited/AlreadyJoined are terminal UnsupportedSyscall stops (a real PS3 would block or answer ESRCH/EINVAL,
// and this diagnostic runtime invents no errno yet), ProviderFailure stops ProviderFailure. Nothing is cancelled, joined or reaped
// by the bridge; the provider keeps ownership and the composition root stays the only release path.
static void dispatch_ppu_thread_join(StartupExecution &execution, ppu_context &cpu)
{
    StartupControl &control = *execution.control;
    HleTraceBatch batch;
    if (control.ppu_join_calls != SIZE_MAX) ++control.ppu_join_calls;
    const uint32_t thread_id = uint32_t(cpu.gpr[3]);   // the low word names the id; the raw u64 is checked below
    control.ppu_join_last_thread = cpu.gpr[3];         // the raw guest argument, so the diagnostic line shows what was passed
    const StartupPpuCreatePort *const port = execution.ppu_create;
    if (!port || !port->join) stop(StartupStop::MissingServices, &batch);
    StartupPpuJoinResult (*const join)(void *, uint32_t) = port->join;
    void *const context = port->context;
    // The id must fit the provider's 32-bit id space; a zero or over-wide id gets no invented guest errno.
    if (!cpu.gpr[3] || cpu.gpr[3] > UINT32_MAX) stop(StartupStop::UnsupportedSyscall, &batch);
    // Output u64: canonical, nonnull and writable now (the write happens only after the provider joined).
    const uint64_t output = cpu.gpr[4];
    if (!output || output > UINT32_MAX || !writable(execution, uint32_t(output), 8)) stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const StartupPpuJoinResult joined = join(context, thread_id);   // may block on the child; no reservation or pin is held
    control.ppu_join_last_status = joined.status;
    control.ppu_join_has_result = true;
    check_boundary(BoundaryBudget::Poll, &batch);
    switch (joined.status) {
    case StartupPpuJoinStatus::Joined: break;
    case StartupPpuJoinStatus::NotFound:
    case StartupPpuJoinStatus::NotExited:
    case StartupPpuJoinStatus::AlreadyJoined: stop(StartupStop::UnsupportedSyscall, &batch);
    case StartupPpuJoinStatus::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    default: stop(StartupStop::ProviderFailure, &batch);   // never trust an out-of-range status
    }
    // The exit value is the caller's only output; reserve its record before writing it.
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
    begin_trace(execution, 1, batch);
    const uint64_t encoded = __builtin_bswap64(joined.exit_value);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, uint32_t(output), &encoded, sizeof encoded) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    control.ppu_join_last_exit_value = joined.exit_value;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, uint32_t(output), 8,
        uint32_t(joined.exit_value)), batch);
    finish_owned(&batch);
    // The store is committed. A poll here suppresses only the guest register return.
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = 0;
    if (control.ppu_join_returns != SIZE_MAX) ++control.ppu_join_returns;
}
#endif
#ifdef PS3_NATIVE_GCM_INIT
constexpr uint32_t gcm_init_body_nid = 0x15bae46b;
constexpr uint32_t gcm_init_lr = 0x00446b18;

/* A guest read is admitted when the whole span lies in any bound RAM range (writable or not). */
static bool gcm_guest_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}

/* Guest-write sink of the GCM-init service: coherent write plus HLE trace events. Lengths 1/2/4/8
 * publish one scalar event; any other length publishes 24-byte chunks (spurs_instance_write rule).
 * The bridge is active only while one dispatch owns the batch; the service storage lives in the
 * composition, so a refusal here never stops the thread. */
static int gcm_bridge_write(void *user, uint32_t address, const void *bytes, size_t length)
{
    auto *bridge = static_cast<StartupGcmGuestBridge *>(user);
    if (!bridge || !bridge->active || !bridge->execution || !bridge->batch) return -1;
    StartupExecution &execution = *bridge->execution;
    if (!writable(execution, address, length)) return -1;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK) return -1;
    const unsigned char *source = static_cast<const unsigned char *>(bytes);
    const bool scalar = length == 1 || length == 2 || length == 4 || length == 8;
    if (scalar) {
        uint64_t value = 0;
        for (size_t index = 0; index < length; ++index) value = value << 8 | source[index];
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, address,
            length, value), *bridge->batch);
        ++bridge->events;
        return 0;
    }
    for (size_t offset = 0; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > PPU_MEMORY_TRACE_BYTE_CAPACITY) chunk = PPU_MEMORY_TRACE_BYTE_CAPACITY;
        publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
            address + uint32_t(offset), source + offset, chunk), *bridge->batch);
        ++bridge->events;
        offset += chunk;
    }
    return 0;
}

static int gcm_bridge_read(void *user, uint32_t address, void *bytes, size_t length)
{
    auto *bridge = static_cast<StartupGcmGuestBridge *>(user);
    if (!bridge || !bridge->execution) return -1;
    StartupExecution &execution = *bridge->execution;
    if (!gcm_guest_readable(execution, address, length)) return -1;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    return memory.read(memory.context, address, bytes, length) == PS3_GUEST_MEMORY_OK ? 0 : -1;
}

/* Exact publication count of a successful run: the generated image in its contiguous runs (one port
 * write per run, chunked at 24 bytes), context 16 -> 1 chunk, OPD 8 -> 1 scalar, 3 report words,
 * 5 control words (put, get, put, reference, get), the context EA out and the TOC return. */
static size_t gcm_init_trace_events()
{
    size_t events = 0;
    uint32_t index = 0;
    while (index < gcm_init_image_word_count) {
        const uint32_t start = gcm_init_image_words[index].io_offset;
        size_t used = 0;
        while (index < gcm_init_image_word_count &&
               gcm_init_image_words[index].io_offset == start + uint32_t(used) &&
               used + 4u <= GCM_INIT_IMAGE_MAX_RUN_BYTES) {
            used += 4u;
            ++index;
        }
        events += (used == 1 || used == 2 || used == 4 || used == 8)
            ? 1 : (used + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
    }
    events += 1;   // context descriptor, 16 bytes in one chunk
    events += 1;   // callback OPD, 8 bytes scalar
    events += 3;   // report words
    events += 5;   // control put, get, put, published reference, get
    events += 1;   // context EA at the game's out pointer
    events += 1;   // TOC return
    return events;
}

static void dispatch_gcm_init(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_init;
    observation = {};
    ++control.gcm_init_calls;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.raw_r5 = cpu.gpr[5];
    observation.raw_r6 = cpu.gpr[6];
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmInitFailure::MissingService;
        stop(StartupStop::GcmInitStop);
    }
    if (cpu.lr != gcm_init_lr) {
        observation.failure = StartupGcmInitFailure::WrongLr;
        stop(StartupStop::GcmInitStop);
    }
    // The export entry sets r7=1 itself; only r3..r6 are the documented request, full 64-bit admitted.
    if (observation.raw_r3 >> 32 || observation.raw_r4 >> 32 ||
        observation.raw_r5 >> 32 || observation.raw_r6 >> 32) {
        observation.failure = StartupGcmInitFailure::HighBits;
        stop(StartupStop::GcmInitStop);
    }
    observation.request.ctx_out_ea = uint32_t(observation.raw_r3);
    observation.request.cmd_size = uint32_t(observation.raw_r4);
    observation.request.io_size = uint32_t(observation.raw_r5);
    observation.request.io_ea = uint32_t(observation.raw_r6);

    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (toc_slot < sp) {
        observation.failure = StartupGcmInitFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    const gcm_init_layout &layout = execution.gcm_init->layout;
    // All-or-nothing writability preflight before any store.
    if (!writable(execution, observation.request.ctx_out_ea, 4) ||
        !writable(execution, observation.request.io_ea, GCM_INIT_IMAGE_PUT) ||
        !writable(execution, layout.context_ea, 16) ||
        !writable(execution, layout.callback_opd_ea, 8) ||
        !writable(execution, layout.control_ea, 12) ||
        !writable(execution, layout.reports_ea, layout.reports_size) ||
        toc_slot > UINT32_MAX || !writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmInitFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    check_boundary(BoundaryBudget::Poll);
    const size_t events = gcm_init_trace_events();
    if (!trace_can_begin(execution, events)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, events, batch);

    StartupGcmGuestBridge &bridge = execution.gcm_init_bridge;
    bridge.execution = &execution;
    bridge.batch = &batch;
    bridge.events = 0;
    bridge.active = true;
    gcm_init_result result{};
    const gcm_init_status status = gcm_init_service_run(execution.gcm_init, &observation.request, &result);
    bridge.active = false;
    bridge.batch = nullptr;
    observation.result = result;
    observation.publications = bridge.events;
    if (status != GCM_INIT_OK) {
        observation.failure = (status == GCM_INIT_UNSUPPORTED_REQUEST ||
            status == GCM_INIT_ALREADY_INITIALIZED || status == GCM_INIT_ALREADY_FAILED)
            ? StartupGcmInitFailure::ServiceRefused : StartupGcmInitFailure::ServiceFailed;
        stop(StartupStop::GcmInitStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;   // the TOC return event
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_init_returns;
}

constexpr uint32_t gcm_get_configuration_nid = 0xe315a0b2;
constexpr uint32_t gcm_get_label_address_nid = 0xf80196c1;
constexpr uint32_t gcm_get_offset_table_nid = 0x2922aed0;
constexpr uint32_t gcm_get_control_register_nid = 0xa547adde;

static size_t gcm_query_chunk_events(size_t bytes)
{
    return bytes == 0 ? 0 : (bytes + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
}

/* The four post-init queries share one observation, one TOC slot (SP+0x28) and the
 * same guest port as the init route. No LR is restricted: each query is a pure read
 * of service state, and the observed LR is recorded for later caller analysis. */
static void dispatch_gcm_query(StartupExecution &execution, uint32_t nid)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    switch (nid) {
    case gcm_get_configuration_nid: ++control.gcm_config_calls; break;
    case gcm_get_label_address_nid: ++control.gcm_label_calls; break;
    case gcm_get_offset_table_nid: ++control.gcm_offset_table_calls; break;
    case gcm_get_control_register_nid: ++control.gcm_control_calls; break;
    default: stop(StartupStop::MissingServices);
    }
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }

    if (nid == gcm_get_configuration_nid) {
        if (observation.raw_r3 >> 32) {
            observation.failure = StartupGcmQueryFailure::HighBits;
            stop(StartupStop::GcmQueryStop);
        }
        const uint32_t out = uint32_t(observation.raw_r3);
        if (!writable(execution, out, 24) || !writable(execution, uint32_t(toc_slot), 8)) {
            observation.failure = StartupGcmQueryFailure::InvalidMemory;
            stop(StartupStop::InvalidMemory);
        }
        preflight_toc(execution, toc_slot);
        if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity);
        HleTraceBatch batch;
        begin_trace(execution, 2, batch);
        StartupGcmGuestBridge &bridge = execution.gcm_init_bridge;
        bridge.execution = &execution;
        bridge.batch = &batch;
        bridge.events = 0;
        bridge.active = true;
        gcm_init_config config{};
        observation.status = gcm_init_service_configuration(execution.gcm_init, &config);
        if (observation.status != GCM_INIT_OK) {
            bridge.active = false;
            bridge.batch = nullptr;
            observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
                ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
            stop(StartupStop::GcmQueryStop, &batch);
        }
        uint8_t bytes[24];
        gcm_init_config_encode(&config, bytes);
        const int written = gcm_bridge_write(&bridge, out, bytes, sizeof bytes);
        observation.publications = bridge.events;
        bridge.active = false;
        bridge.batch = nullptr;
        if (written != 0) {
            observation.failure = StartupGcmQueryFailure::ServiceFailed;
            stop(StartupStop::GcmQueryStop, &batch);
        }
        publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
        ++observation.publications;   // the TOC return event
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        observation.has_result = true;
        observation.result = 0;
        cpu.gpr[2] = toc;
        cpu.gpr[3] = 0;
        ++control.gcm_config_returns;
        return;
    }

    if (nid == gcm_get_offset_table_nid) {
        if (observation.raw_r3 >> 32) {
            observation.failure = StartupGcmQueryFailure::HighBits;
            stop(StartupStop::GcmQueryStop);
        }
        const uint32_t out = uint32_t(observation.raw_r3);
        const gcm_init_layout &layout = execution.gcm_init->layout;
        const bool materialised = execution.gcm_init->tables_materialized != 0;
        if (!writable(execution, out, 8) ||
            (!materialised &&
             (!writable(execution, layout.offset_table_ea, GCM_INIT_EA_TABLE_BYTES) ||
              !writable(execution, layout.io_table_ea, GCM_INIT_IO_TABLE_BYTES))) ||
            !writable(execution, uint32_t(toc_slot), 8)) {
            observation.failure = StartupGcmQueryFailure::InvalidMemory;
            stop(StartupStop::InvalidMemory);
        }
        preflight_toc(execution, toc_slot);
        size_t events = 2;   // the 8-byte output scalar and the TOC return
        if (!materialised)
            events += gcm_query_chunk_events(GCM_INIT_EA_TABLE_BYTES) +
                      gcm_query_chunk_events(GCM_INIT_IO_TABLE_BYTES);
        if (!trace_can_begin(execution, events)) stop(StartupStop::TraceCapacity);
        HleTraceBatch batch;
        begin_trace(execution, events, batch);
        StartupGcmGuestBridge &bridge = execution.gcm_init_bridge;
        bridge.execution = &execution;
        bridge.batch = &batch;
        bridge.events = 0;
        bridge.active = true;
        uint32_t ea_table_ea = 0, io_table_ea = 0;
        observation.status = gcm_init_service_offset_tables(execution.gcm_init,
            &ea_table_ea, &io_table_ea);
        if (observation.status != GCM_INIT_OK) {
            observation.publications = bridge.events;
            bridge.active = false;
            bridge.batch = nullptr;
            observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
                ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
            stop(StartupStop::GcmQueryStop, &batch);
        }
        const uint8_t bytes[8]{
            uint8_t(ea_table_ea >> 24), uint8_t(ea_table_ea >> 16),
            uint8_t(ea_table_ea >> 8), uint8_t(ea_table_ea),
            uint8_t(io_table_ea >> 24), uint8_t(io_table_ea >> 16),
            uint8_t(io_table_ea >> 8), uint8_t(io_table_ea)};
        const int written = gcm_bridge_write(&bridge, out, bytes, sizeof bytes);
        observation.publications = bridge.events;
        bridge.active = false;
        bridge.batch = nullptr;
        if (written != 0) {
            observation.failure = StartupGcmQueryFailure::ServiceFailed;
            stop(StartupStop::GcmQueryStop, &batch);
        }
        publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
        ++observation.publications;   // the TOC return event
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        observation.has_result = true;
        observation.result = out;
        cpu.gpr[2] = toc;   // firmware leaves r3 as the caller's output pointer
        ++control.gcm_offset_table_returns;
        return;
    }

    if (nid == gcm_get_label_address_nid) {
        if (!writable(execution, uint32_t(toc_slot), 8)) {
            observation.failure = StartupGcmQueryFailure::InvalidMemory;
            stop(StartupStop::InvalidMemory);
        }
        preflight_toc(execution, toc_slot);
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
        HleTraceBatch batch;
        begin_trace(execution, 1, batch);
        uint32_t ea = 0;
        observation.status = gcm_init_service_label_address(execution.gcm_init,
            uint32_t(observation.raw_r3), &ea);
        if (observation.status != GCM_INIT_OK) {
            observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
                ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
            stop(StartupStop::GcmQueryStop, &batch);
        }
        publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
        ++observation.publications;
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        observation.has_result = true;
        observation.result = ea;
        cpu.gpr[2] = toc;
        cpu.gpr[3] = ea;
        ++control.gcm_label_returns;
        return;
    }

    // GetControlRegister: r3 is not an output pointer.
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    uint32_t ea = 0;
    observation.status = gcm_init_service_control_register(execution.gcm_init, &ea);
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = ea;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = ea;
    ++control.gcm_control_returns;
}

constexpr uint32_t gcm_set_flip_mode_nid = 0x4ae8d215;

/* cellGcmSetFlipMode: record HSYNC/VSYNC in the GCM service. The mode is the low
 * 32 bits of r3; only the TOC is written and r3 returns 0. */
static void dispatch_gcm_set_flip_mode(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = gcm_set_flip_mode_nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    ++control.gcm_flip_mode_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    observation.status = gcm_init_service_set_flip_mode(execution.gcm_init,
        uint32_t(observation.raw_r3));
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_flip_mode_returns;
}

constexpr uint32_t gcm_set_tile_info_nid = 0xbd100dbc;
constexpr uint32_t gcm_bind_tile_nid = 0x4524cccd;

/* cellGcmSetTileInfo: index, location, offset, size, pitch, comp, base and bank are the
 * low 32 bits of r3..r10 (u32/u8 in the firmware signature; the full raw values are
 * recorded). Only the TOC is written and r3 returns 0. Any domain refusal (local tiles
 * only, alignment, pitch, bank) stops with GcmQueryStop. */
static void dispatch_gcm_set_tile_info(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = gcm_set_tile_info_nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.raw_r5 = cpu.gpr[5];
    observation.raw_r6 = cpu.gpr[6];
    observation.raw_r7 = cpu.gpr[7];
    observation.raw_r8 = cpu.gpr[8];
    observation.raw_r9 = cpu.gpr[9];
    observation.raw_r10 = cpu.gpr[10];
    ++control.gcm_tile_info_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    observation.status = gcm_init_service_set_tile_info(execution.gcm_init,
        uint32_t(observation.raw_r3), uint32_t(observation.raw_r4),
        uint32_t(observation.raw_r5), uint32_t(observation.raw_r6),
        uint32_t(observation.raw_r7), uint32_t(observation.raw_r8),
        uint32_t(observation.raw_r9), uint32_t(observation.raw_r10));
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_tile_info_returns;
}

/* cellGcmBindTile: the tile index is the low 32 bits of r3. Only the TOC is written and
 * r3 returns 0; an index with no set tile (or out of range) stops with GcmQueryStop. */
static void dispatch_gcm_bind_tile(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = gcm_bind_tile_nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    ++control.gcm_bind_tile_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    observation.status = gcm_init_service_bind_tile(execution.gcm_init,
        uint32_t(observation.raw_r3));
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_bind_tile_returns;
}

constexpr uint32_t gcm_bind_zcull_nid = 0x9dc04436;
constexpr uint32_t gcm_unbind_zcull_nid = 0xa75640e8;
constexpr uint32_t gcm_unbind_tile_nid = 0xd9b7653e;

/* cellGcmBindZcull: parameters 1..8 are the low 32 bits of r3..r10; parameters 9..12 are the
 * low 32 bits of the 8-byte parameter-save slots at SP+0x70, SP+0x78, SP+0x80 and SP+0x88.
 * The whole 0x20-byte save area must be readable before anything is stored. Only the TOC is
 * written and r3 returns 0; any domain refusal stops with GcmQueryStop. */
static void dispatch_gcm_bind_zcull(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = gcm_bind_zcull_nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.raw_r5 = cpu.gpr[5];
    observation.raw_r6 = cpu.gpr[6];
    observation.raw_r7 = cpu.gpr[7];
    observation.raw_r8 = cpu.gpr[8];
    observation.raw_r9 = cpu.gpr[9];
    observation.raw_r10 = cpu.gpr[10];
    ++control.gcm_bind_zcull_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX ||
        sp > UINT64_C(0x100000000) - 0x90) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    const uint32_t save_ea = uint32_t(sp) + 0x70u;
    if (!gcm_guest_readable(execution, save_ea, 0x20u) || !writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    uint8_t save[0x20];
    if (memory.read(memory.context, save_ea, save, sizeof save) != PS3_GUEST_MEMORY_OK) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    // Big-endian 8-byte slots: the argument is the low 32 bits at offset 4 of each slot.
    auto low_word = [&save](size_t slot) {
        const uint8_t *p = save + slot * 8u + 4u;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    observation.raw_stack9 = low_word(0);
    observation.raw_stack10 = low_word(1);
    observation.raw_stack11 = low_word(2);
    observation.raw_stack12 = low_word(3);
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    gcm_init_zcull args{};
    args.offset = uint32_t(observation.raw_r4);
    args.width = uint32_t(observation.raw_r5);
    args.height = uint32_t(observation.raw_r6);
    args.cull_start = uint32_t(observation.raw_r7);
    args.z_format = uint32_t(observation.raw_r8);
    args.aa_format = uint32_t(observation.raw_r9);
    args.zcull_dir = uint32_t(observation.raw_r10);
    args.zcull_format = observation.raw_stack9;
    args.s_func = observation.raw_stack10;
    args.s_ref = observation.raw_stack11;
    args.s_mask = observation.raw_stack12;
    observation.status = gcm_init_service_bind_zcull(execution.gcm_init,
        uint32_t(observation.raw_r3), &args);
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_bind_zcull_returns;
}

static void dispatch_gcm_unbind(StartupExecution &execution, uint32_t nid)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    const bool zcull = nid == gcm_unbind_zcull_nid;
    if (zcull) ++control.gcm_unbind_zcull_calls;
    else ++control.gcm_unbind_tile_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    observation.status = zcull
        ? gcm_init_service_unbind_zcull(execution.gcm_init, uint32_t(observation.raw_r3))
        : gcm_init_service_unbind_tile(execution.gcm_init, uint32_t(observation.raw_r3));
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    if (zcull) ++control.gcm_unbind_zcull_returns;
    else ++control.gcm_unbind_tile_returns;
}

constexpr uint32_t gcm_set_display_buffer_nid = 0xa53d12ae;

/* cellGcmSetDisplayBuffer: parameters 1..5 are the low 32 bits of r3..r7 (display buffer id,
 * offset, pitch, width, height). Only the TOC is written and r3 returns 0; any domain refusal
 * stops with GcmQueryStop and stores nothing. */
static void dispatch_gcm_set_display_buffer(StartupExecution &execution)
{
    auto &control = *execution.control;
    auto &cpu = execution.ppu.cpu;
    auto &observation = control.gcm_query;
    observation = {};
    observation.nid = gcm_set_display_buffer_nid;
    observation.lr = cpu.lr;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.raw_r5 = cpu.gpr[5];
    observation.raw_r6 = cpu.gpr[6];
    observation.raw_r7 = cpu.gpr[7];
    ++control.gcm_display_buffer_calls;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    if (execution.gcm_init == nullptr) {
        observation.failure = StartupGcmQueryFailure::MissingService;
        stop(StartupStop::GcmQueryStop);
    }
    const uint64_t sp = cpu.gpr[1], toc_slot = sp + 0x28, toc = cpu.gpr[2];
    if (sp > UINT32_MAX || toc_slot < sp || toc_slot > UINT32_MAX) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    if (!writable(execution, uint32_t(toc_slot), 8)) {
        observation.failure = StartupGcmQueryFailure::InvalidMemory;
        stop(StartupStop::InvalidMemory);
    }
    preflight_toc(execution, toc_slot);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    observation.status = gcm_init_service_set_display_buffer(execution.gcm_init,
        uint32_t(observation.raw_r3), uint32_t(observation.raw_r4),
        uint32_t(observation.raw_r5), uint32_t(observation.raw_r6),
        uint32_t(observation.raw_r7));
    if (observation.status != GCM_INIT_OK) {
        observation.failure = observation.status == GCM_INIT_NOT_INITIALIZED
            ? StartupGcmQueryFailure::NotInitialized : StartupGcmQueryFailure::ServiceFailed;
        stop(StartupStop::GcmQueryStop, &batch);
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    ++observation.publications;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    observation.has_result = true;
    observation.result = 0;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    ++control.gcm_display_buffer_returns;
}

extern "C++" {   // the surrounding region is extern "C"; this matches the C++ declaration in the header
gcm_init_status startup_gcm_init_attach(StartupExecution &execution, gcm_init_service *storage,
                                        const gcm_init_layout &layout)
{
    if (!storage) return GCM_INIT_INVALID_ARGUMENT;
    StartupGcmGuestBridge &bridge = execution.gcm_init_bridge;
    bridge.execution = &execution;
    bridge.batch = nullptr;
    bridge.events = 0;
    bridge.active = false;
    gcm_guest_ports ports{};
    ports.user = &bridge;
    ports.write = gcm_bridge_write;
    ports.read = gcm_bridge_read;
    const gcm_init_status status = gcm_init_service_create(storage, &layout, &ports);
    execution.gcm_init = status == GCM_INIT_OK ? storage : nullptr;
    return status;
}
}
#endif
#ifdef PS3_NATIVE_GAMEDATA
constexpr uint32_t gamedata_check_create2_nid = 0xc9645c41;
constexpr uint32_t gamedata_get_size_kb_nid = 0xa9b0c1d9;
constexpr uint32_t ppu_thread_exit_nid = 0xaff080a4;

static bool gamedata_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}

static uint32_t gamedata_be32(const unsigned char *bytes)
{
    return uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
           uint32_t(bytes[2]) << 8 | bytes[3];
}

static size_t gamedata_trace_count(size_t length)
{
    if (length == 1 || length == 2 || length == 4 || length == 8) return 1;
    return (length + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1) / PPU_MEMORY_TRACE_BYTE_CAPACITY;
}

static void gamedata_publish_write(StartupExecution &execution, uint32_t address,
    const unsigned char *bytes, size_t length, HleTraceBatch &batch,
    StartupGameDataObservation &observation)
{
    const bool scalar = length == 1 || length == 2 || length == 4 || length == 8;
    if (scalar) {
        uint64_t value = 0;
        for (size_t index = 0; index < length; ++index) value = value << 8 | bytes[index];
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            address, length, value), batch);
        ++observation.publications;
        return;
    }
    for (size_t offset = 0; offset < length;) {
        size_t chunk = length - offset;
        if (chunk > PPU_MEMORY_TRACE_BYTE_CAPACITY) chunk = PPU_MEMORY_TRACE_BYTE_CAPACITY;
        publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
            address + uint32_t(offset), bytes + offset, chunk), batch);
        ++observation.publications;
        offset += chunk;
    }
}

static void gamedata_write(StartupExecution &execution, uint32_t address,
    const unsigned char *bytes, size_t length, HleTraceBatch &batch,
    StartupGameDataObservation &observation)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    observation.memory_status = memory.write(memory.context, address, bytes, length);
    if (observation.memory_status != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
    gamedata_publish_write(execution, address, bytes, length, batch, observation);
}

static bool gamedata_read(const StartupExecution &execution, uint32_t address,
    unsigned char *bytes, size_t length)
{
    if (!gamedata_readable(execution, address, length)) return false;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    return memory.read(memory.context, address, bytes, length) == PS3_GUEST_MEMORY_OK;
}

/* cellGameDataCheckCreate2 (NID 0xC9645C41). The firmware frame, argument quirks and callback
 * sequencing are decoded from the firmware. */
static void dispatch_gamedata_check_create2(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    StartupGameDataObservation &observation = control.gamedata_check;
    observation = {};
    if (control.gamedata_check_calls != SIZE_MAX) ++control.gamedata_check_calls;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.raw_r5 = cpu.gpr[5];
    observation.raw_r6 = cpu.gpr[6];
    observation.raw_r7 = cpu.gpr[7];
    observation.version = uint32_t(cpu.gpr[3]);
    observation.dir_name = uint32_t(cpu.gpr[4]);
    observation.err_dialog = uint32_t(cpu.gpr[5]);
    observation.func_stat = uint32_t(cpu.gpr[6]);
    observation.container = uint32_t(cpu.gpr[7]);
    const uint64_t toc = cpu.gpr[2];
    const uint64_t toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution, toc_slot);
    const StartupGameDataPort *const port = execution.gamedata;
    const Ps3PpuExecutionServices *const services = execution.ppu.services;
    if (!port || !port->state || !services || !services->compiled_code || !services->compiled_code->find)
        stop(StartupStop::MissingServices);
    if (!execution.process_sdk.valid) stop(StartupStop::MissingServices);
    observation.argument_checked = true;

    // Every normal return publishes the saved TOC exactly once. Argument quirks return the value
    // already in r3 and never enter the callback.
    const auto return_with = [&](uint64_t r3) {
        HleTraceBatch batch;
        begin_trace(execution, 1, batch);
        publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[2] = toc;
        cpu.gpr[3] = r3;
        observation.has_result = true;
        if (control.gamedata_check_returns != SIZE_MAX) ++control.gamedata_check_returns;
    };

    if (observation.version != 0) {
        observation.result = int32_t(observation.version);
        return_with(observation.raw_r3);
        return;
    }
    if (!observation.dir_name) {
        observation.result = 0;
        return_with(0);
        return;
    }
    if (!observation.func_stat) {
        observation.result = 0;
        return_with(0);
        return;
    }

    unsigned char name[GAMEDATA_DIRNAME_MAX + 1u]{};
    size_t readable = 0;
    for (size_t index = 0; index < sizeof(name); ++index) {
        if (uint64_t(observation.dir_name) + index > UINT32_MAX ||
            !gamedata_read(execution, observation.dir_name + uint32_t(index), &name[index], 1))
            stop(StartupStop::InvalidMemory);
        readable = index + 1;
        if (!name[index]) break;
    }
    const int32_t validated = gamedata_validate_dir_name(name, readable, execution.process_sdk.raw);
    if (validated != 0) {
        observation.result = validated;
        return_with(uint64_t(int64_t(validated)));
        return;
    }

    if (observation.func_stat > UINT32_MAX || observation.func_stat % 4 ||
        !gamedata_readable(execution, observation.func_stat, 8)) stop(StartupStop::InvalidMemory);
    unsigned char opd[8]{};
    if (!gamedata_read(execution, observation.func_stat, opd, sizeof(opd)))
        stop(StartupStop::InvalidMemory);
    const uint32_t entry = gamedata_be32(opd);
    const uint32_t callback_toc = gamedata_be32(opd + 4);
    observation.opd = observation.func_stat;
    observation.entry = entry;
    observation.toc = callback_toc;
    if (!entry || entry % 4) stop(StartupStop::DispatchFailure);
    ps3_compiled_function target = nullptr;
    if (services->compiled_code->find(services->compiled_code->context, entry, &target) !=
            PS3_COMPILED_CODE_OK || !target) stop(StartupStop::DispatchFailure);

    const uint64_t sp = cpu.gpr[1];
    if (sp < GAMEDATA_FIRMWARE_FRAME) stop(StartupStop::InvalidMemory);
    const uint32_t frame = uint32_t(sp - GAMEDATA_FIRMWARE_FRAME);
    if (frame % 16 || !writable(execution, frame, GAMEDATA_FIRMWARE_FRAME))
        stop(StartupStop::InvalidMemory);
    observation.frame = frame;
    observation.cb_result = frame + GAMEDATA_FRAME_CBRESULT;
    observation.stat_get = frame + GAMEDATA_FRAME_STATGET;
    observation.stat_set = frame + GAMEDATA_FRAME_STATSET;

    unsigned char statget[GAMEDATA_STATGET_BYTES]{};
    observation.status = gamedata_build_statget(&port->storage,
        reinterpret_cast<const char *>(name), statget, port->state);
    if (observation.status != GAMEDATA_OK)
        stop(observation.status == GAMEDATA_BROKEN_PARAM
            ? StartupStop::UnsupportedSyscall : StartupStop::ProviderFailure);

    const size_t events = 1 + gamedata_trace_count(GAMEDATA_CBRESULT_BYTES) +
        gamedata_trace_count(GAMEDATA_STATSET_BYTES) + gamedata_trace_count(GAMEDATA_STATGET_BYTES);
    if (!trace_can_begin(execution, events)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, events, batch);
    unsigned char backchain[8];
    for (size_t index = 0; index < sizeof(backchain); ++index)
        backchain[index] = static_cast<unsigned char>(sp >> (56 - 8 * index));
    gamedata_write(execution, frame, backchain, sizeof(backchain), batch, observation);
    const unsigned char zero_cbresult[GAMEDATA_CBRESULT_BYTES]{};
    gamedata_write(execution, observation.cb_result, zero_cbresult, sizeof(zero_cbresult),
        batch, observation);
    const unsigned char zero_statset[GAMEDATA_STATSET_BYTES]{};
    gamedata_write(execution, observation.stat_set, zero_statset, sizeof(zero_statset),
        batch, observation);
    gamedata_write(execution, observation.stat_get, statget, sizeof(statget), batch, observation);
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);

    const uint64_t saved_r1 = cpu.gpr[1], saved_lr = cpu.lr, saved_ctr = cpu.ctr;
    cpu.gpr[1] = frame;
    cpu.gpr[2] = callback_toc;
    cpu.gpr[3] = observation.cb_result;
    cpu.gpr[4] = observation.stat_get;
    cpu.gpr[5] = observation.stat_set;
    cpu.ctr = entry;
    observation.callback_invoked = true;
    ps3_compiled_dispatch_result dispatched{};
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (!execution.dispatch_stop.stop_requested) stop(StartupStop::MissingServices, &batch);
    if (execution.observation) {
        using namespace infamous::diagnostics;
        const DispatchEntry dispatch_entry{cpu.ctr, cpu.lr, cpu.gpr[1], execution.dispatch.depth};
        const auto storage = execution.dispatch.depth >= execution.dispatch.limit
            ? DispatchStorage::ImmediateNonInvoking : DispatchStorage::Pending;
        DispatchTicket ticket;
        if (!execution.observation->try_begin_dispatch(dispatch_entry, storage, ticket))
            stop(StartupStop::ObservationCapacity);
        dispatched = ps3_compiled_dispatch(&execution.dispatch,
            *services->compiled_code, execution.dispatch_stop, &cpu, dispatch_entry.captured_ctr);
        if (storage == DispatchStorage::ImmediateNonInvoking &&
            (dispatched.status == PS3_COMPILED_DISPATCH_OK || dispatched.lookup_attempted))
            std::abort();
        execution.observation->finish_dispatch(ticket, execution.dispatch.depth,
            {uint32_t(dispatched.status), dispatched.lookup_attempted, uint32_t(dispatched.lookup_status)});
    } else {
        if (control.dispatch_count >= STARTUP_DISPATCH_CAPACITY) stop(StartupStop::TraceCapacity);
        StartupDispatchRecord &record = control.dispatches[control.dispatch_count++];
        record.captured_ctr = cpu.ctr;
        record.depth_before = execution.dispatch.depth;
        record.before = cpu;
        dispatched = ps3_compiled_dispatch(&execution.dispatch, *services->compiled_code,
            execution.dispatch_stop, &cpu, record.captured_ctr);
        record.result = dispatched;
        record.depth_after = execution.dispatch.depth;
        record.core_returned = true;
    }
#else
    (void)target;
    stop(StartupStop::MissingServices);
#endif
    observation.dispatch_status = dispatched.status;
    observation.dispatch_returned = true;
    if (dispatched.status == PS3_COMPILED_DISPATCH_STOP_CANCELLED) stop(StartupStop::Cancelled);
    if (dispatched.status == PS3_COMPILED_DISPATCH_STOP_DEADLINE) stop(StartupStop::Deadline);
    if (dispatched.status != PS3_COMPILED_DISPATCH_OK) stop(StartupStop::DispatchFailure);

    // DRAIN the nested callback's pending tail call BEFORE any register is restored or any record is read back. Generated code
    // runs a callee's pending tail call at its caller; here the caller is native code, so without this a tail-calling funcStat
    // would write its CBResult only after CheckCreate2 had already returned. Same loop as the generated DRAIN_TRAMPOLINE.
    while (g_trampoline_fn) { void (*tf)(void *) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&cpu); }

    unsigned char result_bytes[4]{};
    if (!gamedata_read(execution, observation.cb_result, result_bytes, sizeof(result_bytes)))
        stop(StartupStop::InvalidMemory);
    const int32_t result = int32_t(gamedata_be32(result_bytes));
    unsigned char err_bytes[4]{};
    if (!gamedata_read(execution, observation.cb_result + 4, err_bytes, sizeof(err_bytes)))
        stop(StartupStop::InvalidMemory);
    observation.err_need_size_kb = int32_t(gamedata_be32(err_bytes));
    unsigned char set_bytes[4]{};
    if (!gamedata_read(execution, observation.stat_set, set_bytes, sizeof(set_bytes)))
        stop(StartupStop::InvalidMemory);
    const uint32_t set_param = gamedata_be32(set_bytes);
    observation.result = result;
    observation.set_param = set_param;
    unsigned char param[GAMEDATA_PARAM_BYTES]{};
    const uint8_t *set_param_bytes = nullptr;
    if (set_param) {
        if (!gamedata_read(execution, set_param, param, sizeof(param)))
            stop(StartupStop::InvalidMemory);
        set_param_bytes = param;
    }
    observation.status = gamedata_apply_result(&port->storage, port->state, result,
        set_param_bytes, &observation.action);
    if (observation.status != GAMEDATA_OK) stop(StartupStop::ProviderFailure);
    observation.check = *port->state;

    cpu.gpr[1] = saved_r1;
    cpu.gpr[2] = toc;
    cpu.lr = saved_lr;
    cpu.ctr = saved_ctr;
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch toc_batch;
    begin_trace(execution, 1, toc_batch);
    publish_toc(execution, toc_slot, toc, toc_batch, &observation.toc_written);
    finish_owned(&toc_batch);
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = uint64_t(int64_t(gamedata_guest_return()));
    observation.has_result = true;
    if (control.gamedata_check_returns != SIZE_MAX) ++control.gamedata_check_returns;
}

/* cellGameDataGetSizeKB (NID 0xA9B0C1D9): a cached size, never a directory walk (report §2). */
static void dispatch_gamedata_get_size_kb(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    StartupGameDataSizeObservation &observation = control.gamedata_size;
    observation = {};
    if (control.gamedata_size_calls != SIZE_MAX) ++control.gamedata_size_calls;
    observation.raw_size = cpu.gpr[3];
    observation.size = uint32_t(cpu.gpr[3]);
    const uint64_t toc = cpu.gpr[2];
    const uint64_t toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution, toc_slot);
    if (observation.size != 0 &&
        (observation.raw_size > UINT32_MAX || !writable(execution, observation.size, 4)))
        stop(StartupStop::InvalidMemory);
    const StartupGameDataPort *const port = execution.gamedata;
    const int32_t value = port && port->state ? port->state->size_kb : 0;
    observation.value = value;
    const size_t events = observation.size ? 2 : 1;
    if (!trace_can_begin(execution, events)) stop(StartupStop::TraceCapacity);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, events, batch);
    if (observation.size) {
        const uint32_t encoded = __builtin_bswap32(uint32_t(value));
        const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
        observation.memory_status = memory.write(memory.context, observation.size, &encoded,
            sizeof(encoded));
        if (observation.memory_status != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        observation.written = true;
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            observation.size, 4, uint32_t(value)), batch);
        ++observation.publications;
    }
    publish_toc(execution, toc_slot, toc, batch, &observation.toc_written);
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = 0;
    observation.has_result = true;
    if (control.gamedata_size_returns != SIZE_MAX) ++control.gamedata_size_returns;
}

/* sys_ppu_thread_exit (NID 0xAFF080A4). Only a child PPU thread may end this way. */
static void dispatch_ppu_thread_exit(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    if (control.guest_thread_exit_calls != SIZE_MAX) ++control.guest_thread_exit_calls;
    if (!execution.child_thread_exit_allowed) stop(StartupStop::UnsupportedSyscall);
    control.guest_exit_value = execution.ppu.cpu.gpr[3];
    control.has_guest_exit = true;
    stop(StartupStop::GuestThreadExit);
}
#endif
#ifdef PS3_NATIVE_STARTUP_PAD
/* cellPadInit (NID 0x1CF98800) and cellPadSetPortSetting (NID 0x578E3C98): the two `sys_io` entries
 * of the pad batch. Neither writes a guest byte -- the whole pad module state
 * object is firmware-private -- so there is no memory port, no trace event and
 * no writable() check here; the domain produces a byte image instead.
 *
 * `control.pad_init.state` is the instance-owned module flag: cellPadInit answers 0x80121103 when it
 * is already set, and the setter reads the same instance. A per-call copy would hide the second init.
 *
 * THE FAILURE SIGNAL. func_00154194's only failure exit is a silent return: nothing in
 * the guest prints, and nothing in the guest reaches func_00452290. So a non-zero result is NOT a
 * native stop -- it is the firmware's answer, the guest's choice to skip -- and the adapter records
 * it in `pad_init_requests` (a non-zero result) and `control.pad_init.result`, which the composition
 * prints. PAD_ASSUMED_PAD_COUNT is the adapter's decision about a host with no controller source
 * (INTEGRATION.md §8): one standard pad, which changes `attached_pads` and nothing else. */
/* cellPadGetInfo2 (NID 0xA703A51D), cellPadGetData (NID 0x8B72CDA1) and cellPadSetActDirect (NID
 * 0xF65544EE) complete the pad module: the three entries a per-frame poll loop issues. Each one writes
 * only the block the guest named, and each one is SILENT -- no capture record, no trace event, no log
 * line -- because it runs every frame. Their statistics are the per-process counters in
 * StartupPadProcess (info2_calls, data_calls, data_changed, act_calls), never a per-call record. */
constexpr uint32_t cell_pad_init_nid = 0x1cf98800;
constexpr uint32_t cell_pad_set_port_setting_nid = 0x578e3c98;
constexpr uint32_t cell_pad_get_info2_nid = 0xa703a51d;
constexpr uint32_t cell_pad_get_data_nid = 0x8b72cda1;
constexpr uint32_t cell_pad_set_act_direct_nid = 0xf65544ee;
constexpr uint32_t PAD_ASSUMED_PAD_COUNT = 1u;

/* Where a pad arm reads and mutates its module state. `pad_process` (the process-wide object a later
 * composition sets) wins when present; otherwise the execution's own control keeps the state, which is
 * exactly the pre-D1 behaviour for every arm. A port with no function pointers is handed on as NULL so
 * "no controller source" keeps one representation in the service. */
struct PadArmTarget {
    pad_state &state;
    const pad_port *port;
};

static bool pad_port_is_wired(const pad_port &port)
{
    return port.pad_count || port.capability || port.read_state;
}

static PadArmTarget pad_arm_target(StartupExecution &execution)
{
    StartupPadProcess *const process = execution.pad_process;
    if (!process) return PadArmTarget{execution.control->pad_init.state, nullptr};
    return PadArmTarget{process->state, pad_port_is_wired(process->port) ? &process->port : nullptr};
}

/* The pad module state is not atomic: one arm at a time, across every guest thread, while the call
 * runs. Never held across a stop (pad_sync_stop is called after releasing). */
class PadProcessGuard {
public:
    PadProcessGuard() = default;
    explicit PadProcessGuard(StartupPadProcess *process) : process_(process)
    {
        if (process_) startup_spin_acquire(process_->lock);
    }
    ~PadProcessGuard()
    {
        if (process_) process_->lock.clear(std::memory_order_release);
    }
    PadProcessGuard(const PadProcessGuard &) = delete;
    PadProcessGuard &operator=(const PadProcessGuard &) = delete;
    /* Moving hands the release responsibility over, so a caller can leave the region that owns the
     * lock before a `stop()` that does not return. */
    PadProcessGuard(PadProcessGuard &&other) noexcept : process_(other.process_) { other.process_ = nullptr; }
    PadProcessGuard &operator=(PadProcessGuard &&) = delete;

private:
    StartupPadProcess *process_ = nullptr;
};

static void dispatch_pad_init(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    StartupPadInitObservation &observation = control.pad_init;
    /* The observation is reset PER CALL except `state`, which is instance-owned across calls. */
    const pad_state retained = observation.state;
    observation = StartupPadInitObservation{};
    observation.state = retained;
    if (control.pad_init_calls != SIZE_MAX) ++control.pad_init_calls;
    observation.raw_r3 = cpu.gpr[3];
    observation.count = uint32_t(cpu.gpr[3]);
    observation.argument_checked = pad_init_argument_valid(observation.count) != 0;
    check_boundary(BoundaryBudget::Poll);
    const PadArmTarget target = pad_arm_target(execution);
    {
        PadProcessGuard guard(execution.pad_process);
        observation.status = pad_init(&target.state, target.port, observation.count,
            PAD_ASSUMED_PAD_COUNT, nullptr, 0, &observation.result);
        /* The shared state is what the guest observes; copy it out so every existing print and test of
         * `observation.state` keeps saying the truth about the process's pad module. */
        if (execution.pad_process) observation.state = target.state;
    }
    observation.initialized = observation.state.initialized != 0;
    /* The firmware's already-initialized short circuit (0x248C..0x24A0) answers BEFORE the range check,
     * so a valid argument still reaches it: the flag is the result itself, not "the argument was bad". */
    observation.second_call = observation.result == PAD_ERROR_ALREADY_INITIALIZED;
    if (observation.result != 0 && control.pad_init_requests != SIZE_MAX) ++control.pad_init_requests;
    cpu.gpr[3] = uint64_t(int64_t(observation.result));
    if (control.pad_init_returns != SIZE_MAX) ++control.pad_init_returns;
    check_boundary(BoundaryBudget::Poll);
}

static void dispatch_pad_set_port_setting(StartupExecution &execution)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    StartupPadPortSettingObservation &observation = control.pad_set_port_setting;
    observation = StartupPadPortSettingObservation{};
    if (control.pad_set_port_setting_calls != SIZE_MAX) ++control.pad_set_port_setting_calls;
    observation.raw_r3 = cpu.gpr[3];
    observation.raw_r4 = cpu.gpr[4];
    observation.port_no = uint32_t(cpu.gpr[3]);
    observation.setting = uint32_t(cpu.gpr[4]);
    check_boundary(BoundaryBudget::Poll);
    const PadArmTarget target = pad_arm_target(execution);
    {
        PadProcessGuard guard(execution.pad_process);
        observation.status = pad_set_port_setting(&target.state, target.port,
            observation.port_no, observation.setting, &observation.result, nullptr, 0,
            &observation.detail);
        if (execution.pad_process) control.pad_init.state = target.state;
    }
    observation.applied = observation.detail.applied != 0;
    /* sc 503 is recorded, not issued: the record gate is armed by the port-open path (0xB8C4), which
     * this adapter does not model yet, and the guest's own objects are firmware-private. */
    observation.syscall_replayed = false;
    cpu.gpr[3] = uint64_t(int64_t(observation.result));
    if (control.pad_set_port_setting_returns != SIZE_MAX) ++control.pad_set_port_setting_returns;
    check_boundary(BoundaryBudget::Poll);
}

/* cellPadGetInfo2(r3 = CellPadInfo2*): the service builds the 124-byte image, the adapter writes it,
 * because only the adapter may touch guest memory. A NULL or unreadable output pointer is the
 * firmware's own INVALID_ARGUMENT before the service is called (a refused write would be a stop). */
static void dispatch_pad_get_info2(StartupExecution &execution)
{
    ppu_context &cpu = execution.ppu.cpu;
    const uint64_t info_out = cpu.gpr[3];
    check_boundary(BoundaryBudget::Poll);
    const PadArmTarget target = pad_arm_target(execution);
    uint8_t image[PAD_INFO2_BYTES]{};
    int32_t result = PAD_ERROR_INVALID_ARGUMENT;
    bool wrote = false;
    {
        PadProcessGuard guard(execution.pad_process);
        if (execution.pad_process && execution.pad_process->info2_calls != UINT64_MAX)
            ++execution.pad_process->info2_calls;
        if (info_out == 0 || info_out > UINT32_MAX ||
            !writable(execution, uint32_t(info_out), PAD_INFO2_BYTES)) {
            result = PAD_ERROR_INVALID_ARGUMENT;
        } else if (pad_get_info2(&target.state, target.port, image, sizeof(image), &result) !=
            PAD_SERVICE_OK) {
            result = PAD_ERROR_INVALID_ARGUMENT;
        } else {
            wrote = result == 0;
        }
        if (wrote) {
            PadProcessGuard released = std::move(guard);
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            if (memory.write(memory.context, uint32_t(info_out), image, sizeof(image)) !=
                PS3_GUEST_MEMORY_OK) stop(StartupStop::MissingServices);
        }
    }
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = uint64_t(int64_t(result));
}

/* cellPadGetData(r3 = port, r4 = CellPadData*): the 132-byte image, and nothing is written unless the
 * service answered 0 (an unconnected port is NO_DEVICE with a stale guest buffer). */
static void dispatch_pad_get_data(StartupExecution &execution)
{
    ppu_context &cpu = execution.ppu.cpu;
    const uint32_t port_no = uint32_t(cpu.gpr[3]);
    const uint64_t data_out = cpu.gpr[4];
    check_boundary(BoundaryBudget::Poll);
    const PadArmTarget target = pad_arm_target(execution);
    uint8_t image[PAD_DATA_BYTES]{};
    int32_t result = PAD_ERROR_INVALID_ARGUMENT;
    bool wrote = false;
    {
        PadProcessGuard guard(execution.pad_process);
        if (execution.pad_process && execution.pad_process->data_calls != UINT64_MAX)
            ++execution.pad_process->data_calls;
        if (data_out == 0 || data_out > UINT32_MAX ||
            !writable(execution, uint32_t(data_out), PAD_DATA_BYTES)) {
            result = PAD_ERROR_INVALID_ARGUMENT;
        } else if (pad_get_data(&target.state, target.port, port_no, image, sizeof(image), &result) !=
            PAD_SERVICE_OK) {
            result = PAD_ERROR_INVALID_ARGUMENT;
        } else {
            wrote = result == 0;
            /* len 0 is the service's "nothing changed on this port" answer, which the guest drains on.
             * The image is big-endian: CellPadData.len is the last byte of its first word. */
            if (wrote && image[3] != 0u && execution.pad_process &&
                execution.pad_process->data_changed != UINT64_MAX) {
                ++execution.pad_process->data_changed;
            }
        }
        if (wrote) {
            PadProcessGuard released = std::move(guard);
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            if (memory.write(memory.context, uint32_t(data_out), image, sizeof(image)) !=
                PS3_GUEST_MEMORY_OK) stop(StartupStop::MissingServices);
        }
    }
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = uint64_t(int64_t(result));
}

/* cellPadSetActDirect(r3 = port, r4 = param). The guest's param pointer is only reported as present:
 * the service drives no actuator yet, so no guest byte is read from it. */
static void dispatch_pad_set_act_direct(StartupExecution &execution)
{
    ppu_context &cpu = execution.ppu.cpu;
    const uint32_t port_no = uint32_t(cpu.gpr[3]);
    const uint64_t param = cpu.gpr[4];
    check_boundary(BoundaryBudget::Poll);
    const PadArmTarget target = pad_arm_target(execution);
    int32_t result = PAD_ERROR_INVALID_ARGUMENT;
    {
        PadProcessGuard guard(execution.pad_process);
        if (execution.pad_process && execution.pad_process->act_calls != UINT64_MAX)
            ++execution.pad_process->act_calls;
        if (pad_set_act_direct(&target.state, target.port, port_no, param != 0, &result) !=
            PAD_SERVICE_OK) {
            result = PAD_ERROR_INVALID_ARGUMENT;
        }
    }
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = uint64_t(int64_t(result));
}
#endif
#ifdef PS3_NATIVE_STARTUP_FS
/* The six `sys_fs` imports under decision 24:
 *   cellFsOpendir 0x3F61245C (805)  cellFsReaddir  0x5C74903D (806)  cellFsClosedir 0xFF42DCC3 (807)
 *   cellFsStat    0x7DE6DCED (808)  cellFsRmdir    0x2796FDF3 (813)  cellFsUnlink   0x7F4677A8 (814)
 * One entry point serves all six (the gamedata pattern): the register capture, the guest-path read
 * and the B2-S5 capture then live in exactly one place.
 *
 * Every guest path is recorded for the FIRST 32 calls -- escaped, NUL-terminated, at most 255
 * characters, together with the mount it resolved to and the code it returned -- because "which
 * mount does the purge target" is the open B2-S5 question, and a capture inside the HLE is the only
 * place it can be answered without guessing. The counters are never truncated.
 *
 * Read/write discipline: the EFAULT guards run BEFORE any resolution (the firmware's own wrappers do,
 * CONTRACT §1a/§1b), so a NULL output pointer produces no storage call; the record is written only on
 * FS_OK, and the delete verbs never return EROFS (decision 24). */
constexpr uint32_t fs_opendir_nid = 0x3f61245c;
constexpr uint32_t fs_readdir_nid = 0x5c74903d;
constexpr uint32_t fs_closedir_nid = 0xff42dcc3;
constexpr uint32_t fs_stat_nid = 0x7de6dced;
constexpr uint32_t fs_rmdir_nid = 0x2796fdf3;
constexpr uint32_t fs_unlink_nid = 0x7f4677a8;
constexpr uint32_t fs_open_nid = 0x718bf5f8;
constexpr uint32_t fs_read_nid = 0x4d5ff8e2;
constexpr uint32_t fs_write_nid = 0xecdcf2ab;
constexpr uint32_t fs_close_nid = 0x2cb51f0d;
constexpr uint32_t fs_fstat_nid = 0xef3efa34;
constexpr uint32_t fs_lseek_nid = 0xa397d042;
constexpr uint32_t fs_mkdir_nid = 0xba901fe6;
constexpr uint32_t fs_rename_nid = 0xf12eecc8;
constexpr uint32_t fs_utime_nid = 0xbef554a4;
constexpr uint32_t fs_fsync_nid = 0x967a162b;

/* The lv2 syscall number behind each import, which is what a capture line names. */
static uint32_t fs_syscall_of(uint32_t nid)
{
    switch (nid) {
    case fs_opendir_nid: return 805u;
    case fs_readdir_nid: return 806u;
    case fs_closedir_nid: return 807u;
    case fs_stat_nid: return 808u;
    case fs_rmdir_nid: return 813u;
    case fs_unlink_nid: return 814u;
    case fs_open_nid: return 801u;
    case fs_read_nid: return 802u;
    case fs_write_nid: return 803u;
    case fs_close_nid: return 804u;
    case fs_fstat_nid: return 809u;
    case fs_lseek_nid: return 818u;
    case fs_mkdir_nid: return 811u;
    case fs_rename_nid: return 812u;
    case fs_utime_nid: return 815u;
    case fs_fsync_nid: return 820u;
    }
    return 0u;
}

/* Copies `length` guest bytes into `out` when the whole span is inside one RAM range. */
static bool fs_read_guest_string(const StartupExecution &execution, uint32_t address,
    unsigned char *out, size_t length)
{
    if (length == 0u || !execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    for (size_t index = 0; index < execution.ppu.memory->ram_count; ++index) {
        const PpuMemoryRamRange &range = execution.ppu.memory->ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) {
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            return memory.read(memory.context, address, out, length) == PS3_GUEST_MEMORY_OK;
        }
    }
    return false;
}

static void fs_escape_path(const char *guest_path, size_t length, char *out, size_t capacity,
    bool &truncated)
{
    size_t used = 0;
    truncated = false;
    if (!guest_path || capacity == 0) return;
    for (size_t index = 0; index < length && guest_path[index] != '\0'; ++index) {
        const unsigned char byte = static_cast<unsigned char>(guest_path[index]);
        if (byte >= 0x20u && byte < 0x7Fu) {
            if (used + 1u >= capacity) { truncated = true; break; }
            out[used++] = char(byte);
        } else {
            if (used + 4u >= capacity) { truncated = true; break; }
            static const char digits[] = "0123456789abcdef";
            out[used++] = '\\';
            out[used++] = 'x';
            out[used++] = digits[(byte >> 4) & 0xFu];
            out[used++] = digits[byte & 0xFu];
        }
    }
    out[used] = '\0';
}

/* Records one call for the B2-S5 capture. `guest_path` may be NULL (readdir/closedir carry none).
 * Non-printable and non-ASCII bytes are escaped as \xNN so a capture line stays one line. */
static void fs_capture_call(StartupControl &control, uint32_t sc, const char *guest_path,
    size_t length, uint32_t mount, bool resolved, int32_t rc)
{
    StartupFsObservation &observation = control.fs;
    if (observation.captured < STARTUP_FS_CAPTURE_CAPACITY) {
        StartupFsCallRecord &record = observation.capture[observation.captured++];
        record = StartupFsCallRecord{};
        record.sc = sc;
        record.mount = mount;
        record.resolved = resolved;
        record.rc = rc;
        fs_escape_path(guest_path, length, record.path, STARTUP_FS_CAPTURE_PATH,
            record.truncated);
    } else if (observation.dropped != SIZE_MAX) {
        ++observation.dropped;
    }
}

class FsServiceGuard {
public:
    explicit FsServiceGuard(const ps3_instance_guard &guard) : guard_(guard)
    {
        if (guard_.lock && guard_.unlock) {
            guard_.lock(guard_.context);
            held_ = true;
        }
    }
    ~FsServiceGuard()
    {
        if (held_) guard_.unlock(guard_.context);
    }
    FsServiceGuard(const FsServiceGuard &) = delete;
    FsServiceGuard &operator=(const FsServiceGuard &) = delete;

private:
    const ps3_instance_guard &guard_;
    bool held_ = false;
};

static void fs_process_log_call(StartupExecution &execution, uint32_t sc, const char *guest_path,
    size_t length, bool import_form, int32_t rc)
{
    StartupFsContext &context = *execution.fs;
    FsServiceGuard held(context.guard);
    if (context.process_logged < STARTUP_FS_PROCESS_LOG_CAPACITY) {
        StartupFsProcessCall &record = context.process_log[context.process_logged++];
        record = StartupFsProcessCall{};
        record.sc = sc;
        record.thread_id = uint32_t(execution.ppu.cpu.thread_id);
        record.rc = rc;
        record.import_form = import_form;
        fs_escape_path(guest_path, length, record.path, STARTUP_FS_PROCESS_LOG_PATH,
            record.truncated);
    } else if (context.process_dropped != UINT64_MAX) {
        ++context.process_dropped;
    }
}

/* Resolves a guest path string and returns the guest code. `resolved` reports whether the path
 * named a configured mount (the capture's `mount` field). */
static int32_t fs_resolve_guest_path(fs_service &service, const ps3_instance_guard &guard,
    const char *path, size_t length, uint32_t &mount, bool &resolved)
{
    fs_resolved_path route{};
    int32_t code = FS_OK;
    {
        FsServiceGuard held(guard);
        code = fs_resolve_path(&service, path, length, &route);
    }
    if (code == FS_OK) {
        mount = route.mount_index;
        resolved = true;
    }
    return code;
}

static void dispatch_fs(StartupExecution &execution, uint32_t nid)
{
    StartupControl &control = *execution.control;
    ppu_context &cpu = execution.ppu.cpu;
    StartupFsObservation &observation = control.fs;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    if (!execution.fs || !execution.fs->active) stop(StartupStop::MissingServices);
    fs_service &service = execution.fs->service;
    const ps3_instance_guard &guard = execution.fs->guard;
    const uint32_t sc = fs_syscall_of(nid);
    const uint64_t toc = cpu.gpr[2];
    const uint64_t toc_slot = cpu.gpr[1] + 0x28;
    if (toc_slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(execution, toc_slot);
    check_boundary(BoundaryBudget::Poll);

    observation.calls = observation.calls == UINT64_MAX ? UINT64_MAX : observation.calls + 1;
    uint32_t mount = 0xFFFFFFFFu;
    bool resolved = false;
    int32_t code = 0;
    char path[FS_PATH_MAX]{};
    size_t length = 0;
    /* The path-bearing entries read r3 as an EA32 (the guest truncates it, CALLS.md §3a). A pointer
     * that is not wholly readable is EFAULT before any resolution. */
    const bool path_entry = nid == fs_stat_nid || nid == fs_opendir_nid || nid == fs_rmdir_nid ||
        nid == fs_unlink_nid;
    if (path_entry) {
        const uint64_t raw = cpu.gpr[3];
        unsigned char buffer[FS_PATH_MAX]{};
        if (raw > UINT32_MAX || !fs_read_guest_string(execution, uint32_t(raw), buffer,
                sizeof(buffer))) {
            code = FS_ERROR_EFAULT;
        } else {
            length = 0;
            while (length < sizeof(buffer) && buffer[length] != '\0') ++length;
            if (length == sizeof(buffer)) {
                code = FS_ERROR_ENAMETOOLONG;
            } else {
                for (size_t index = 0; index <= length; ++index) path[index] = char(buffer[index]);
                code = fs_resolve_guest_path(service, guard, path, length, mount, resolved);
            }
        }
    }

    /* The output-range preflight, BEFORE any resolution: the firmware's public wrappers guard the
     * output pointer first, so a NULL output is EFAULT whatever the path
     * says. cellFsStat's record, cellFsOpendir's handle word and cellFsReaddir's dirent + nread all
     * follow the same rule; the deletes and cellFsClosedir have no output at all. */
    const uint64_t stat_out = cpu.gpr[4];
    const uint64_t dirent_out = cpu.gpr[4];
    const uint64_t nread_out = cpu.gpr[5];
    if (nid == fs_stat_nid &&
        (stat_out == 0 || stat_out > UINT32_MAX ||
         !writable(execution, uint32_t(stat_out), FS_CELLFS_STAT_BYTES)))
        code = FS_ERROR_EFAULT;
    else if (nid == fs_opendir_nid &&
        (stat_out == 0 || stat_out > UINT32_MAX || !writable(execution, uint32_t(stat_out), 4)))
        code = FS_ERROR_EFAULT;
    else if (nid == fs_readdir_nid &&
        (dirent_out == 0 || dirent_out > UINT32_MAX ||
         !writable(execution, uint32_t(dirent_out), FS_CELLFS_DIRENT_BYTES) ||
         nread_out == 0 || nread_out > UINT32_MAX ||
         !writable(execution, uint32_t(nread_out), FS_CELLFS_NREAD_BYTES)))
        code = FS_ERROR_EFAULT;

    /* The operation itself, only when the guards above let it through. */
    uint8_t stat_record[FS_CELLFS_STAT_BYTES]{};
    uint8_t dirent_record[FS_CELLFS_DIRENT_BYTES]{};
    uint8_t nread_record[FS_CELLFS_NREAD_BYTES]{};
    uint32_t handle = 0;
    bool wrote_nread = false, wrote_dirent = false, wrote_stat = false, wrote_handle = false;
    if (code == FS_OK) {
        check_boundary(BoundaryBudget::Poll);
        FsServiceGuard held(guard);
        if (nid == fs_stat_nid) {
            code = fs_stat(&service, path, length, stat_record);
            wrote_stat = code == FS_OK;
        } else if (nid == fs_opendir_nid) {
            code = fs_opendir(&service, path, length, &handle);
            wrote_handle = code == FS_OK;
        } else if (nid == fs_readdir_nid) {
            code = fs_readdir(&service, uint32_t(cpu.gpr[3]), dirent_record, nread_record);
            if (code == FS_OK) {
                wrote_nread = true;
                wrote_dirent = nread_record[FS_CELLFS_NREAD_BYTES - 1u] != 0u;
            }
        } else if (nid == fs_closedir_nid) {
            code = fs_closedir(&service, uint32_t(cpu.gpr[3]));
        } else if (nid == fs_rmdir_nid) {
            code = fs_rmdir(&service, path, length);
        } else if (nid == fs_unlink_nid) {
            code = fs_unlink(&service, path, length);
        }
    }

    /* Publish the outputs. cellFsReaddir MUST write nread on every successful call, including the
     * end of the directory: a stale nread makes the walker loop for ever (CALLS.md §3a). */
    /* The credit this batch needs: the stat record is published in PPU_MEMORY_TRACE_BYTE_CAPACITY-sized
     * chunks (gamedata_publish_write's rule), so it must be counted the same way, plus the handle word,
     * the nread word, the dirent and the TOC slot. */
    const size_t stat_events = wrote_stat
        ? (FS_CELLFS_STAT_BYTES + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1u) / PPU_MEMORY_TRACE_BYTE_CAPACITY
        : 0u;
    /* Root (2026-11-11): the dirent is the real 258-byte record now, so it is published in chunks like the
     * stat record (one event could hold only PPU_MEMORY_TRACE_BYTE_CAPACITY bytes and the larger record aborted). */
    const size_t dirent_events = wrote_dirent
        ? (FS_CELLFS_DIRENT_BYTES + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1u) / PPU_MEMORY_TRACE_BYTE_CAPACITY
        : 0u;
    const size_t events = stat_events + (wrote_handle ? 1u : 0u) + (wrote_nread ? 1u : 0u) +
        dirent_events + 1u;
    if (!trace_can_begin(execution, events)) stop(StartupStop::TraceCapacity);
    HleTraceBatch batch;
    begin_trace(execution, events, batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (wrote_stat) {
        if (memory.write(memory.context, uint32_t(stat_out), stat_record, sizeof(stat_record)) !=
            PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
        for (size_t offset = 0; offset < sizeof(stat_record);) {
            size_t chunk = sizeof(stat_record) - offset;
            if (chunk > PPU_MEMORY_TRACE_BYTE_CAPACITY) chunk = PPU_MEMORY_TRACE_BYTE_CAPACITY;
            publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
                uint32_t(stat_out) + uint32_t(offset), stat_record + offset, chunk), batch);
            offset += chunk;
        }
    }
    if (wrote_handle) {
        const unsigned char encoded[4]{uint8_t(handle >> 24), uint8_t(handle >> 16),
            uint8_t(handle >> 8), uint8_t(handle)};
        if (memory.write(memory.context, uint32_t(stat_out), encoded, sizeof(encoded)) !=
            PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            uint32_t(stat_out), 4, handle), batch);
    }
    if (wrote_nread) {
        if (memory.write(memory.context, uint32_t(nread_out), nread_record, sizeof(nread_record)) !=
            PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
        publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
            uint32_t(nread_out), 8, uint64_t(nread_record[FS_CELLFS_NREAD_BYTES - 1u])), batch);
    }
    if (wrote_dirent) {
        if (memory.write(memory.context, uint32_t(dirent_out), dirent_record,
                sizeof(dirent_record)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        for (size_t offset = 0; offset < sizeof(dirent_record);) {
            size_t chunk = sizeof(dirent_record) - offset;
            if (chunk > PPU_MEMORY_TRACE_BYTE_CAPACITY) chunk = PPU_MEMORY_TRACE_BYTE_CAPACITY;
            publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
                uint32_t(dirent_out) + uint32_t(offset), dirent_record + offset, chunk), batch);
            offset += chunk;
        }
    }
    publish_toc(execution, toc_slot, toc, batch, nullptr);
    finish_owned(&batch);

    /* The B2-S5 capture: the guest path, the mount it resolved to, the code and the counters. */
    if (path_entry)
        fs_capture_call(control, sc, path, length, mount, resolved, code);
    else
        fs_capture_call(control, sc, nullptr, 0, 0xFFFFFFFFu, false, code);
    if (path_entry) fs_process_log_call(execution, sc, path, length, true, code);
    {
        FsServiceGuard held(guard);
        observation.counters = service.counters;
        observation.last_storage_status = service.last_storage_status;
    }
    observation.last_rc = code;
    observation.returns = observation.returns == UINT64_MAX ? UINT64_MAX : observation.returns + 1;

    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[2] = toc;
    cpu.gpr[3] = uint64_t(int64_t(code));
}

/* The raw file syscalls the game's own libc issues (801 open, 802 read, 803 write, 804 close,
 * 809 fstat, 818 lseek) on the same service and guard as the cellFs imports above. A raw syscall
 * has no TOC slot and never touches r2; every pointer is an EA32 and the answer is the
 * sign-extended guest code in r3. */
constexpr uint32_t fs_syscall_open = 801u;
constexpr uint32_t fs_syscall_read = 802u;
constexpr uint32_t fs_syscall_write = 803u;
constexpr uint32_t fs_syscall_close = 804u;
constexpr uint32_t fs_syscall_fstat = 809u;
constexpr uint32_t fs_syscall_lseek = 818u;
/* D1: the four `cellFs*` entries the game's own libc issues as raw syscalls, alongside open/read/close. */
constexpr uint32_t fs_syscall_mkdir = 811u;
constexpr uint32_t fs_syscall_rename = 812u;
constexpr uint32_t fs_syscall_utime = 815u;
constexpr uint32_t fs_syscall_fsync = 820u;
/* `CellFsUtimbuf` (lv2 815, r4): two big-endian s64 fields. */
constexpr uint64_t fs_utimbuf_bytes = 16u;
struct FsUtimbufWire {
    uint64_t actime;
    uint64_t modtime;
};
constexpr uint32_t fs_syscall_copy_window = 4096u;
constexpr uint64_t fs_syscall_max_bytes = 0x7FFFFFFFu;

/* The console's source rule: admitted when the whole span lies in any bound RAM range, writable
 * or not (a file payload may be an ELF .rodata string). Kept here rather than depending on TTY. */
static bool fs_guest_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}

/* Reads one guest path argument (r3 or r4) exactly as the open arm's r3 does, applying the same EFAULT
 * (not wholly readable) and ENAMETOOLONG (no terminator inside FS_PATH_MAX) rules, so every path-bearing
 * call answers the same way for the same bad pointer. */
static int32_t fs_read_call_path(const StartupExecution &execution, uint64_t address, char *out,
    size_t &length)
{
    unsigned char buffer[FS_PATH_MAX]{};
    length = 0;
    if (address > UINT32_MAX || !fs_read_guest_string(execution, uint32_t(address), buffer,
            sizeof(buffer))) return FS_ERROR_EFAULT;
    while (length < sizeof(buffer) && buffer[length] != '\0') ++length;
    if (length == sizeof(buffer)) return FS_ERROR_ENAMETOOLONG;
    for (size_t index = 0; index <= length; ++index) out[index] = char(buffer[index]);
    return FS_OK;
}

static void fs_finish_syscall(StartupExecution &execution, int32_t code)
{
    StartupFsObservation &observation = execution.control->fs;
    fs_service &service = execution.fs->service;
    {
        FsServiceGuard held(execution.fs->guard);
        observation.counters = service.counters;
        observation.last_storage_status = service.last_storage_status;
    }
    observation.last_rc = code;
    observation.returns = observation.returns == UINT64_MAX ? UINT64_MAX : observation.returns + 1;
}

static void dispatch_fs_syscall(StartupExecution &execution, ppu_context &cpu, uint32_t number,
    bool import_form)
{
    StartupControl &control = *execution.control;
    StartupFsObservation &observation = control.fs;
    if (!execution.ppu.memory || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    if (!execution.fs || !execution.fs->active) stop(StartupStop::MissingServices);
    fs_service &service = execution.fs->service;
    const ps3_instance_guard &guard = execution.fs->guard;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    uint64_t toc = 0, toc_slot = 0;
    if (import_form) {
        toc = cpu.gpr[2];
        toc_slot = cpu.gpr[1] + 0x28;
        if (toc_slot < cpu.gpr[1]) stop(StartupStop::InvalidMemory);
        preflight_toc(execution, toc_slot);
    }
    observation.calls = observation.calls == UINT64_MAX ? UINT64_MAX : observation.calls + 1;
    check_boundary(BoundaryBudget::Poll);
    int32_t code = FS_OK;

    if (number == fs_syscall_open) {
        uint32_t mount = 0xFFFFFFFFu;
        bool resolved = false;
        char path[FS_PATH_MAX]{};
        size_t length = 0;
        unsigned char buffer[FS_PATH_MAX]{};
        if (cpu.gpr[3] > UINT32_MAX ||
            !fs_read_guest_string(execution, uint32_t(cpu.gpr[3]), buffer, sizeof(buffer))) {
            code = FS_ERROR_EFAULT;
        } else {
            while (length < sizeof(buffer) && buffer[length] != '\0') ++length;
            if (length == sizeof(buffer)) {
                code = FS_ERROR_ENAMETOOLONG;
            } else {
                for (size_t index = 0; index <= length; ++index) path[index] = char(buffer[index]);
                code = fs_resolve_guest_path(service, guard, path, length, mount, resolved);
            }
        }
        const uint64_t fd_out = cpu.gpr[5];
        if (fd_out == 0 || fd_out > UINT32_MAX ||
            !writable(execution, uint32_t(fd_out), 4)) code = FS_ERROR_EFAULT;
        uint32_t fd = 0;
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            if (!trace_can_begin(execution, import_form ? 2u : 1u)) stop(StartupStop::TraceCapacity);
            {
                FsServiceGuard held(guard);
                code = fs_open(&service, path, length, uint32_t(cpu.gpr[4]), &fd);
            }
            if (code == FS_OK) {
                const unsigned char encoded[4]{uint8_t(fd >> 24), uint8_t(fd >> 16),
                    uint8_t(fd >> 8), uint8_t(fd)};
                HleTraceBatch batch;
                begin_trace(execution, 1, batch);
                if (memory.write(memory.context, uint32_t(fd_out), encoded, sizeof(encoded)) !=
                    PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
                publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
                    uint32_t(fd_out), 4, fd), batch);
                finish_owned(&batch);
            }
        }
        fs_capture_call(control, fs_syscall_open, path, length, mount, resolved, code);
        fs_process_log_call(execution, fs_syscall_open, path, length, import_form, code);
    } else if (number == fs_syscall_read) {
        const uint32_t fd = uint32_t(cpu.gpr[3]);
        const uint64_t buffer = cpu.gpr[4], nbytes = cpu.gpr[5], nread_out = cpu.gpr[6];
        uint64_t total = 0;
        if (!(import_form && nread_out == 0) &&
            (nread_out == 0 || nread_out > UINT32_MAX ||
             !writable(execution, uint32_t(nread_out), 8))) {
            code = FS_ERROR_EFAULT;
        } else if (nbytes > fs_syscall_max_bytes) {
            code = FS_ERROR_EINVAL;
        } else if (nbytes > 0 && (buffer == 0 || buffer > UINT32_MAX ||
                !writable(execution, uint32_t(buffer), nbytes))) {
            code = FS_ERROR_EFAULT;
        }
        if (code == FS_OK && !trace_can_begin(execution,
                (nread_out != 0 ? 2u : 1u) + (import_form ? 1u : 0u)))
            stop(StartupStop::TraceCapacity);
        if (code == FS_OK) {
            unsigned char window[fs_syscall_copy_window];
            for (uint64_t offset = 0; offset < nbytes;) {
                size_t take = size_t(nbytes - offset);
                if (take > sizeof(window)) take = sizeof(window);
                check_boundary(BoundaryBudget::Poll);
                uint64_t got = 0;
                {
                    FsServiceGuard held(guard);
                    code = fs_read(&service, fd, window, uint32_t(take), &got);
                }
                if (code != FS_OK) break;
                if (got > take) stop(StartupStop::ProviderFailure);
                if (got > 0 && memory.write(memory.context,
                        uint32_t(uint64_t(buffer) + offset), window, size_t(got)) !=
                        PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure);
                total += got;
                if (got < take) break;
                offset += got;
            }
        }
        if (code == FS_OK) {
            LIVE_TRACE("LT fs-read fd=%u nbytes=%llu total=%llu", fd, (unsigned long long)nbytes, (unsigned long long)total);
            const size_t payload = size_t(total < PPU_MEMORY_TRACE_BYTE_CAPACITY
                ? total : PPU_MEMORY_TRACE_BYTE_CAPACITY);
            const bool write_nread = nread_out != 0;
            const size_t events = (write_nread ? 1u : 0u) + (payload ? 1u : 0u);
            if (events) {
                const uint64_t encoded_total = __builtin_bswap64(total);
                HleTraceBatch batch;
                begin_trace(execution, events, batch);
                if (write_nread) {
                    if (memory.write(memory.context, uint32_t(nread_out), &encoded_total,
                            sizeof(encoded_total)) != PS3_GUEST_MEMORY_OK)
                        stop(StartupStop::ProviderFailure, &batch);
                    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle,
                        true, uint32_t(nread_out), 8, total), batch);
                }
                if (payload) {
                    unsigned char readback[PPU_MEMORY_TRACE_BYTE_CAPACITY]{};
                    if (memory.read(memory.context, uint32_t(buffer), readback, payload) !=
                        PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
                    publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle,
                        true, uint32_t(buffer), readback, payload), batch);
                }
                finish_owned(&batch);
            }
        }
    } else if (number == fs_syscall_write) {
        const uint32_t fd = uint32_t(cpu.gpr[3]);
        const uint64_t buffer = cpu.gpr[4], nbytes = cpu.gpr[5], nwrite_out = cpu.gpr[6];
        uint64_t total = 0;
        if (!(import_form && nwrite_out == 0) &&
            (nwrite_out == 0 || nwrite_out > UINT32_MAX ||
             !writable(execution, uint32_t(nwrite_out), 8))) {
            code = FS_ERROR_EFAULT;
        } else if (nbytes > fs_syscall_max_bytes) {
            code = FS_ERROR_EINVAL;
        } else if (nbytes > 0 && (buffer == 0 || buffer > UINT32_MAX ||
                !fs_guest_readable(execution, uint32_t(buffer), nbytes))) {
            code = FS_ERROR_EFAULT;
        }
        if (code == FS_OK && !trace_can_begin(execution,
                (nwrite_out != 0 ? 1u : 0u) + (import_form ? 1u : 0u)))
            stop(StartupStop::TraceCapacity);
        if (code == FS_OK) {
            unsigned char window[fs_syscall_copy_window];
            for (uint64_t offset = 0; offset < nbytes;) {
                size_t take = size_t(nbytes - offset);
                if (take > sizeof(window)) take = sizeof(window);
                check_boundary(BoundaryBudget::Poll);
                if (memory.read(memory.context, uint32_t(uint64_t(buffer) + offset), window, take) !=
                    PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure);
                uint64_t written = 0;
                {
                    FsServiceGuard held(guard);
                    code = fs_write(&service, fd, window, uint32_t(take), &written);
                }
                if (code != FS_OK) break;
                if (written > take) stop(StartupStop::ProviderFailure);
                total += written;
                if (written < take) break;
                offset += written;
            }
        }
        if (code == FS_OK && nwrite_out != 0) {
            const uint64_t encoded_total = __builtin_bswap64(total);
            HleTraceBatch batch;
            begin_trace(execution, 1, batch);
            if (memory.write(memory.context, uint32_t(nwrite_out), &encoded_total,
                    sizeof(encoded_total)) != PS3_GUEST_MEMORY_OK)
                stop(StartupStop::ProviderFailure, &batch);
            publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
                uint32_t(nwrite_out), 8, total), batch);
            finish_owned(&batch);
        }
    } else if (number == fs_syscall_close) {
        {
            FsServiceGuard held(guard);
            code = fs_close(&service, uint32_t(cpu.gpr[3]));
        }
    } else if (number == fs_syscall_fstat) {
        const uint64_t stat_out = cpu.gpr[4];
        if (stat_out == 0 || stat_out > UINT32_MAX ||
            !writable(execution, uint32_t(stat_out), FS_CELLFS_STAT_BYTES)) {
            code = FS_ERROR_EFAULT;
        }
        uint8_t record[FS_CELLFS_STAT_BYTES]{};
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            const size_t events = (FS_CELLFS_STAT_BYTES + PPU_MEMORY_TRACE_BYTE_CAPACITY - 1u) /
                PPU_MEMORY_TRACE_BYTE_CAPACITY;
            if (!trace_can_begin(execution, import_form ? events + 1u : events))
                stop(StartupStop::TraceCapacity);
            {
                FsServiceGuard held(guard);
                code = fs_fstat(&service, uint32_t(cpu.gpr[3]), record);
                LIVE_TRACE("LT fs-fstat fd=%u rc=0x%x mode=%02x%02x%02x%02x size=%02x%02x%02x%02x%02x%02x%02x%02x", unsigned(cpu.gpr[3]), unsigned(code),
                    record[0], record[1], record[2], record[3], record[0x24], record[0x25], record[0x26], record[0x27],
                    record[0x28], record[0x29], record[0x2a], record[0x2b]);
            }
            if (code == FS_OK) {
                HleTraceBatch batch;
                begin_trace(execution, events, batch);
                if (memory.write(memory.context, uint32_t(stat_out), record, sizeof(record)) !=
                    PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure, &batch);
                for (size_t offset = 0; offset < sizeof(record);) {
                    size_t chunk = sizeof(record) - offset;
                    if (chunk > PPU_MEMORY_TRACE_BYTE_CAPACITY) chunk = PPU_MEMORY_TRACE_BYTE_CAPACITY;
                    publish_trace(execution, ppu_memory_bytes_event(PpuMemoryTraceOrigin::Hle, true,
                        uint32_t(stat_out) + uint32_t(offset), record + offset, chunk), batch);
                    offset += chunk;
                }
                finish_owned(&batch);
            }
        }
    } else if (number == fs_syscall_mkdir) {
        char path[FS_PATH_MAX]{};
        size_t length = 0;
        code = fs_read_call_path(execution, cpu.gpr[3], path, length);
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            FsServiceGuard held(guard);
            code = fs_mkdir(&service, path, length, uint32_t(cpu.gpr[4]));
        }
        fs_process_log_call(execution, fs_syscall_mkdir, path, length, import_form, code);
    } else if (number == fs_syscall_rename) {
        char path[FS_PATH_MAX]{};
        char to_path[FS_PATH_MAX]{};
        size_t length = 0, to_length = 0;
        code = fs_read_call_path(execution, cpu.gpr[3], path, length);
        if (code == FS_OK) code = fs_read_call_path(execution, cpu.gpr[4], to_path, to_length);
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            FsServiceGuard held(guard);
            code = fs_rename(&service, path, length, to_path, to_length);
        }
        fs_process_log_call(execution, fs_syscall_rename, path, length, import_form, code);
    } else if (number == fs_syscall_utime) {
        char path[FS_PATH_MAX]{};
        size_t length = 0;
        const uint64_t times_in = cpu.gpr[4];
        code = fs_read_call_path(execution, cpu.gpr[3], path, length);
        FsUtimbufWire times{};
        int64_t access_time = 0, modification_time = 0;
        if (code == FS_OK) {
            if (times_in == 0 || times_in > UINT32_MAX ||
                !fs_guest_readable(execution, uint32_t(times_in), fs_utimbuf_bytes)) {
                code = FS_ERROR_EFAULT;
            } else if (memory.read(memory.context, uint32_t(times_in), &times, sizeof(times)) !=
                PS3_GUEST_MEMORY_OK) {
                code = FS_ERROR_EFAULT;
            } else {
                access_time = int64_t(__builtin_bswap64(times.actime));
                modification_time = int64_t(__builtin_bswap64(times.modtime));
            }
        }
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            FsServiceGuard held(guard);
            code = fs_utime(&service, path, length, access_time, modification_time);
        }
        fs_process_log_call(execution, fs_syscall_utime, path, length, import_form, code);
    } else if (number == fs_syscall_fsync) {
        {
            FsServiceGuard held(guard);
            code = fs_fsync(&service, uint32_t(cpu.gpr[3]));
        }
    } else if (number == fs_syscall_lseek) {
        const uint64_t pos_out = cpu.gpr[6];
        if (pos_out == 0 || pos_out > UINT32_MAX ||
            !writable(execution, uint32_t(pos_out), 8)) {
            code = FS_ERROR_EFAULT;
        }
        uint64_t position = 0;
        if (code == FS_OK) {
            check_boundary(BoundaryBudget::Poll);
            if (!trace_can_begin(execution, import_form ? 2u : 1u))
                stop(StartupStop::TraceCapacity);
            {
                FsServiceGuard held(guard);
                code = fs_lseek(&service, uint32_t(cpu.gpr[3]), int64_t(cpu.gpr[4]),
                    uint32_t(cpu.gpr[5]), &position);
                LIVE_TRACE("LT fs-lseek fd=%u offset=%lld whence=%u rc=0x%x pos=%llu", unsigned(cpu.gpr[3]), (long long)cpu.gpr[4],
                    unsigned(cpu.gpr[5]), unsigned(code), (unsigned long long)position);
            }
            if (code == FS_OK) {
                const uint64_t encoded_position = __builtin_bswap64(position);
                HleTraceBatch batch;
                begin_trace(execution, 1, batch);
                if (memory.write(memory.context, uint32_t(pos_out), &encoded_position,
                        sizeof(encoded_position)) != PS3_GUEST_MEMORY_OK)
                    stop(StartupStop::ProviderFailure, &batch);
                publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
                    uint32_t(pos_out), 8, position), batch);
                finish_owned(&batch);
            }
        }
    }

    if (import_form) {
        HleTraceBatch batch;
        begin_trace(execution, 1, batch);
        publish_toc(execution, toc_slot, toc, batch, nullptr);
        finish_owned(&batch);
    }
    fs_finish_syscall(execution, code);
    check_boundary(BoundaryBudget::Poll);
    if (import_form) cpu.gpr[2] = toc;
    cpu.gpr[3] = uint64_t(int64_t(code));
}
#endif
#ifdef PS3_NATIVE_LLE_SRE
/* Decision 33 (LLE libsre): the nine libc imports Sony's own libsre.prx takes from sysPrxForUser.
 * Each one is the C function it is named after, on guest memory, returning exactly what the
 * library's C prototype returns: the destination pointer for the copy/fill forms (even for a zero
 * length), the compared value for memcmp/strncmp, the length for strlen. A read or write the guest
 * memory provider refuses is an InvalidMemory stop, never a fabricated value. */
enum : uint32_t {
    lle_sys_memset_nid = 0x68b9b011,
    lle_sys_memcpy_nid = 0x6bf66ea7,
    lle_sys_memcmp_nid = 0xfb5db080,
    lle_sys_strlen_nid = 0x2d36462b,
    lle_sys_strcpy_nid = 0x99c88692,
    lle_sys_strncpy_nid = 0xd3039d4d,
    lle_sys_strcat_nid = 0x052d29a6,
    lle_sys_strncat_nid = 0x996f7cf8,
    lle_sys_strncmp_nid = 0x04e83d2c,
    /* C1b: sys_prx_get_module_id_by_name(name, flags, pOpt). libsre (0x3000D30C) asks for an optional companion
     * module and compares the answer with CELL_PRX_ERROR_UNKNOWN_MODULE (0x8001112E) to carry on without it. This
     * runtime loads no PRX by name (libsre itself is mapped by the composition, not by the PRX loader), so
     * "unknown module" is the true answer for every name. */
    lle_sys_prx_get_module_id_by_name_nid = 0xe0998dbf
};
constexpr uint32_t lle_cell_prx_error_unknown_module = 0x8001112Eu;
// A bound rather than an unbounded walk: a guest string that never terminates at the end of its own
// region would otherwise loop until the address wraps.
constexpr size_t lle_string_limit = 0x10000000;

static bool is_lle_libc_import(uint32_t nid)
{
    switch (nid) {
    case lle_sys_memset_nid: case lle_sys_memcpy_nid: case lle_sys_memcmp_nid:
    case lle_sys_strlen_nid: case lle_sys_strcpy_nid: case lle_sys_strncpy_nid:
    case lle_sys_strcat_nid: case lle_sys_strncat_nid: case lle_sys_strncmp_nid:
    case lle_sys_prx_get_module_id_by_name_nid:
        return true;
    default:
        return false;
    }
}
static void lle_libc_read(StartupExecution &execution, uint32_t address, unsigned char *bytes, size_t length)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (!length) return;
    if (memory.read(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK) {
        active->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
}
static void lle_libc_write(StartupExecution &execution, uint32_t address, const unsigned char *bytes, size_t length)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (!length) return;
    if (memory.write(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK) {
        active->invalid_address = address;
        stop(StartupStop::InvalidMemory);
    }
}
static unsigned char lle_libc_byte(StartupExecution &execution, uint32_t address)
{
    unsigned char value = 0;
    lle_libc_read(execution, address, &value, 1);
    return value;
}
static void lle_libc_copy(StartupExecution &execution, uint32_t destination, uint32_t source, size_t length)
{
    unsigned char chunk[256];
    for (size_t done = 0; done < length;) {
        const size_t step = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        lle_libc_read(execution, uint32_t(source + done), chunk, step);
        lle_libc_write(execution, uint32_t(destination + done), chunk, step);
        done += step;
    }
}
static uint64_t lle_libc_strncmp_value(StartupExecution &execution, uint32_t left, uint32_t right, size_t limit)
{
    for (size_t index = 0; index < limit; ++index) {
        const unsigned char a = lle_libc_byte(execution, uint32_t(left + index));
        const unsigned char b = lle_libc_byte(execution, uint32_t(right + index));
        if (a != b) return uint64_t(int64_t(int32_t(int(a) - int(b))));
        if (!a) return 0;
    }
    return 0;
}
/* Byte at a time on purpose. A whole-chunk read runs past the terminator, and a string that ends
 * one byte before an unmapped page would then fault on bytes it never needed -- a spurious
 * InvalidMemory where the real strlen returns a length. */
static uint64_t lle_libc_strlen_value(StartupExecution &execution, uint32_t address)
{
    for (size_t done = 0; done < lle_string_limit; ++done)
        if (!lle_libc_byte(execution, uint32_t(address + done))) return done;
    active->invalid_address = address;
    stop(StartupStop::InvalidMemory);
}
static void lle_libc_memset(StartupExecution &execution, uint32_t destination, unsigned char value, size_t length)
{
    unsigned char chunk[256];
    for (size_t index = 0; index < sizeof(chunk); ++index) chunk[index] = value;
    for (size_t done = 0; done < length;) {
        const size_t step = length - done < sizeof(chunk) ? length - done : sizeof(chunk);
        lle_libc_write(execution, uint32_t(destination + done), chunk, step);
        done += step;
    }
}
static uint64_t lle_libc_memcmp_value(StartupExecution &execution, uint32_t left, uint32_t right, size_t length)
{
    unsigned char a[64], b[64];
    for (size_t done = 0; done < length;) {
        const size_t step = length - done < sizeof(a) ? length - done : sizeof(a);
        lle_libc_read(execution, uint32_t(left + done), a, step);
        lle_libc_read(execution, uint32_t(right + done), b, step);
        for (size_t index = 0; index < step; ++index)
            if (a[index] != b[index]) return uint64_t(int64_t(int32_t(int(a[index]) - int(b[index]))));
        done += step;
    }
    return 0;
}
static uint64_t lle_libc_strcpy_value(StartupExecution &execution, uint32_t destination, uint32_t source)
{
    for (size_t index = 0;; ++index) {
        const unsigned char value = lle_libc_byte(execution, uint32_t(source + index));
        lle_libc_write(execution, uint32_t(destination + index), &value, 1);
        if (!value) return destination;
        if (index + 1 >= lle_string_limit) { active->invalid_address = source; stop(StartupStop::InvalidMemory); }
    }
}
static uint64_t lle_libc_strncpy_value(StartupExecution &execution, uint32_t destination, uint32_t source, size_t limit)
{
    size_t index = 0;
    for (; index < limit; ++index) {
        const unsigned char value = lle_libc_byte(execution, uint32_t(source + index));
        lle_libc_write(execution, uint32_t(destination + index), &value, 1);
        if (!value) {
            for (++index; index < limit; ++index) {
                const unsigned char pad = 0;
                lle_libc_write(execution, uint32_t(destination + index), &pad, 1);
            }
            break;
        }
    }
    return destination;
}
static uint64_t lle_libc_strcat_value(StartupExecution &execution, uint32_t destination, uint32_t source)
{
    const uint64_t length = lle_libc_strlen_value(execution, destination);
    lle_libc_strcpy_value(execution, uint32_t(destination + length), source);
    return destination;
}
static uint64_t lle_libc_strncat_value(StartupExecution &execution, uint32_t destination, uint32_t source, size_t limit)
{
    size_t end = size_t(lle_libc_strlen_value(execution, destination));
    for (size_t index = 0; index < limit; ++index) {
        const unsigned char value = lle_libc_byte(execution, uint32_t(source + index));
        lle_libc_write(execution, uint32_t(destination + end + index), &value, 1);
        if (!value) return destination;
        if (end + index + 1 >= lle_string_limit) { active->invalid_address = source; stop(StartupStop::InvalidMemory); }
    }
    const unsigned char terminator = 0;
    lle_libc_write(execution, uint32_t(destination + end + limit), &terminator, 1);
    return destination;
}
static void dispatch_lle_libc(StartupExecution &execution, uint32_t nid)
{
    ppu_context &cpu = execution.ppu.cpu;
    const uint32_t first = uint32_t(cpu.gpr[3]);
    const uint32_t second = uint32_t(cpu.gpr[4]);
    const uint64_t third = cpu.gpr[5];
    /* The libc services perform up to lle_string_limit guest accesses, far more than one publication.
     * They must NOT hold an open recorder batch while they do it: Sony's body can re-enter this
     * dispatcher, and a second open batch is refused as TraceCapacity. Each access opens and closes
     * its own batch through the memory port, so nothing needs to be open here. */
    HleTraceBatch batch;
    uint64_t result = first;
    switch (nid) {
    case lle_sys_memset_nid: lle_libc_memset(execution, first, uint8_t(second), size_t(third)); break;
    case lle_sys_memcpy_nid: lle_libc_copy(execution, first, second, size_t(third)); break;
    case lle_sys_memcmp_nid: result = lle_libc_memcmp_value(execution, first, second, size_t(third)); break;
    case lle_sys_strlen_nid: result = lle_libc_strlen_value(execution, first); break;
    case lle_sys_strcpy_nid: result = lle_libc_strcpy_value(execution, first, second); break;
    case lle_sys_strncpy_nid: result = lle_libc_strncpy_value(execution, first, second, size_t(third)); break;
    case lle_sys_strcat_nid: result = lle_libc_strcat_value(execution, first, second); break;
    case lle_sys_strncat_nid: result = lle_libc_strncat_value(execution, first, second, size_t(third)); break;
    case lle_sys_strncmp_nid: result = lle_libc_strncmp_value(execution, first, second, size_t(third)); break;
    case lle_sys_prx_get_module_id_by_name_nid: {
        // The name must be a real readable guest string (an unreadable one is an InvalidMemory stop, as for strlen).
        char name[32] = {};
        const size_t length = size_t(lle_libc_strlen_value(execution, first));
        for (size_t index = 0; index < length && index + 1 < sizeof name; ++index)
            name[index] = char(lle_libc_byte(execution, uint32_t(first + index)));
        LIVE_TRACE("LT prx-get-module-id-by-name name=%s flags=0x%x", name, second);
        (void)name;
        result = uint64_t(int64_t(int32_t(lle_cell_prx_error_unknown_module)));
        break;
    }
    default: stop(StartupStop::UnknownImport, &batch);
    }
    check_boundary(BoundaryBudget::Poll, &batch);
    cpu.gpr[3] = result;
    ++active->lle_sre_libc_returns;
}
/* Decision 33 (LLE): sysPrxForUser's sys_process_is_stack and the sceNpTrophy family. Neither touches
 * guest memory nor the TOC; each sets r3 and returns. */
enum : uint32_t {
    lle_sys_process_is_stack_nid = 0x4f7172c9,
    sce_np_trophy_init_nid = 0x39567781,
    sce_np_trophy_term_nid = 0xa7fabf4d,
    sce_np_trophy_create_context_nid = 0xe3bf9a28,
    sce_np_trophy_destroy_context_nid = 0x3741ecc7,
    sce_np_trophy_create_handle_nid = 0x1c25470d,
    sce_np_trophy_destroy_handle_nid = 0x623cd2dc,
    sce_np_trophy_register_context_nid = 0x1197b52c,
    sce_np_trophy_get_required_disk_space_nid = 0x370136fe,
    sce_np_trophy_get_game_info_nid = 0x49d18217,
    sce_np_trophy_get_trophy_info_nid = 0xfce6d30a,
    sce_np_trophy_unlock_trophy_nid = 0x8ceedd21,
    sce_np_trophy_get_trophy_unlock_state_nid = 0xb3ac3478,
    sce_np_trophy_get_game_progress_nid = 0x079f0e87
};
/* The two error values are the SDK's SCE_NP_TROPHY_ERROR_* codes as the coordinator recalls them from the
 * public RPCS3 headers (not extracted from firmware here); the game ignores these return values at the
 * four call sites seen so far. */
constexpr uint32_t sce_np_trophy_error_not_initialized = 0x80022902u;
constexpr uint32_t sce_np_trophy_error_not_supported = 0x80022903u;
constexpr uint32_t ps3_user_stack_begin = 0xD0000000u;
constexpr uint32_t ps3_user_stack_end = 0xE0000000u;

static bool is_lle_process_import(uint32_t nid)
{
    switch (nid) {
    case lle_sys_process_is_stack_nid:
    case sce_np_trophy_init_nid: case sce_np_trophy_term_nid:
    case sce_np_trophy_create_context_nid: case sce_np_trophy_destroy_context_nid:
    case sce_np_trophy_create_handle_nid: case sce_np_trophy_destroy_handle_nid:
    case sce_np_trophy_register_context_nid: case sce_np_trophy_get_required_disk_space_nid:
    case sce_np_trophy_get_game_info_nid: case sce_np_trophy_get_trophy_info_nid:
    case sce_np_trophy_unlock_trophy_nid: case sce_np_trophy_get_trophy_unlock_state_nid:
    case sce_np_trophy_get_game_progress_nid:
        return true;
    default:
        return false;
    }
}
static void dispatch_lle_process_import(StartupExecution &execution, uint32_t nid)
{
    ppu_context &cpu = execution.ppu.cpu;
    uint64_t result = 0;
    switch (nid) {
    case lle_sys_process_is_stack_nid: {
        const uint32_t address = uint32_t(cpu.gpr[3]);
        /* On a PS3 every thread stack is in 0xD0000000..0xDFFFFFFF; here the primary's stack is there
         * but child stacks are user-memory allocations, so the registry is asked too. */
        result = address >= ps3_user_stack_begin && address < ps3_user_stack_end;
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
        if (!result && execution.ppu_threads)
            result = ps3_ppu_threads_is_guest_stack(execution.ppu_threads, address);
#endif
        break;
    }
    /* The port is offline and has no trophy service. The library is present but can never be
     * initialised, so init is refused as unsupported and every other call as not initialised.
     * No output pointer is written and no callback is invoked. */
    case sce_np_trophy_init_nid:
        result = uint64_t(int64_t(int32_t(sce_np_trophy_error_not_supported)));
        break;
    case sce_np_trophy_term_nid: case sce_np_trophy_create_context_nid:
    case sce_np_trophy_destroy_context_nid: case sce_np_trophy_create_handle_nid:
    case sce_np_trophy_destroy_handle_nid: case sce_np_trophy_register_context_nid:
    case sce_np_trophy_get_required_disk_space_nid: case sce_np_trophy_get_game_info_nid:
    case sce_np_trophy_get_trophy_info_nid: case sce_np_trophy_unlock_trophy_nid:
    case sce_np_trophy_get_trophy_unlock_state_nid: case sce_np_trophy_get_game_progress_nid:
        result = uint64_t(int64_t(int32_t(sce_np_trophy_error_not_initialized)));
        break;
    default:
        stop(StartupStop::UnknownImport);
    }
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = result;
}
/* Decision 34: the imports whose answer is fixed because the device or service is absent -- USB
 * keyboard, USB mouse, network control, sys_net, NP and the sysutil callback poll. The answers live
 * in startup_absent_imports.h; this arm only bridges the guest memory and the return register. */
static bool absent_import_zero_memory(void *context, uint32_t address, uint32_t size)
{
    const ps3_guest_memory &memory = *static_cast<const ps3_guest_memory *>(context);
    return memory.zero(memory.context, address, size) == PS3_GUEST_MEMORY_OK;
}
static bool absent_import_store_u32(void *context, uint32_t address, uint32_t value)
{
    const ps3_guest_memory &memory = *static_cast<const ps3_guest_memory *>(context);
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value >> 24), static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
    return memory.write(memory.context, address, bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK;
}
static bool dispatch_absent_import(StartupExecution &execution, uint32_t nid)
{
    auto &cpu = execution.ppu.cpu;
    if (!execution.ppu.admitted || !execution.ppu.memory) return false;
    const ps3_guest_memory *memory = &execution.ppu.memory->coherent_memory;
    const StartupAbsentImportMemory port{const_cast<ps3_guest_memory *>(memory),
        absent_import_zero_memory, absent_import_store_u32};
    const StartupAbsentImportResult result =
        startup_absent_import(nid, uint32_t(cpu.gpr[3]), uint32_t(cpu.gpr[4]), port);
    if (result.status == StartupAbsentImportStatus::NotHandled) return false;
    if (result.status == StartupAbsentImportStatus::BadPointer) stop(StartupStop::MissingServices);
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = uint64_t(int64_t(int32_t(result.value)));
    return true;
}
#endif

#ifdef PS3_NATIVE_LLE_SRE
/* ---- live RSX consumer and its GCM imports --------------------------------------------------------- */
/* Root (2026-11-11). NIDs checked by hash. See StartupRsxProcess and native/domain/rsx_live_fifo.h. */
enum : uint32_t {
    gcm_set_flip_command_nid = 0x21397818u, gcm_set_flip_command_wait_label_nid = 0xd8f88e1au,
    gcm_set_flip_immediate_nid = 0xacee8542u, gcm_set_flip_handler_nid = 0xa41ef7e8u,
    gcm_set_vblank_handler_nid = 0xa91b0402u, gcm_set_user_handler_nid = 0x06edea9eu,
    gcm_set_graphics_handler_nid = 0xd01b570du, gcm_set_queue_handler_nid = 0x0a862772u,
    gcm_set_debug_output_level_nid = 0x51c9d62bu, gcm_sort_remap_nid = 0x25b40ab4u,
    gcm_address_to_offset_nid = 0x21ac3697u
};
// RSX_PUMP_TEST_REGION_BEGIN (native/tests/verify_rsx_pump.sh compiles this text, up to the END marker, in a host harness)
/* Root (2026-11-15, renderer CPU work): switches of the consumer, set by the renderer bridge from <app dir>/sibling-env.txt
 * (rsx_render_bridge_sibling.c, which reads them at its first method) and read at every chunk below, so each can be flipped in
 * that file without a rebuild. The defaults are the new paths; the old behaviour is 0 / 0 / 0.
 *   PS3RECOMP_RSX_FIFO_DIRECT  1 = the FIFO words are read from the flat guest window (rsx_live_ports::pointer), 0 = one
 *                              atomic-domain read32 per word (the old path)
 *   PS3RECOMP_RSX_GET_BATCH    words between control-register `get` updates inside one pump (0 = after every packet)
 *   PS3RECOMP_RSX_PUMP_CHUNK   words consumed per hold of the process lock (0 = the whole budget under one hold, as before) */
volatile int g_ps3_rsx_fifo_direct = 1;
volatile uint32_t g_ps3_rsx_get_batch = 256u;
volatile uint32_t g_ps3_rsx_pump_chunk = 4096u;
/* guest_space_flat_horizon.c: lock-free pointer to [address, address + length) when every page of it is committed. Weak: a host
 * test build of this unit does not link that file, and the consumer then keeps reading through the port. */
extern "C" const void *ps3_flat_guest_fast_range(uint32_t address, uint64_t length) __attribute__((weak));
/* Telemetry of the pump (printed by startup_rsx_print_stats): wall nanoseconds in the pump and waiting for the lock. */
static std::atomic<uint64_t> g_rsx_pump_ns{0}, g_rsx_lock_wait_ns{0}, g_rsx_chunks{0};
static StartupRsxProcess *volatile g_rsx_stats_process = nullptr;
static inline uint64_t rsx_pump_now_ns()
{
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick());
#else
    struct timespec spec{};
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return uint64_t(spec.tv_sec) * UINT64_C(1000000000) + uint64_t(spec.tv_nsec);
#endif
}
namespace rsx_live_bridge {
class Guard {
public:
    explicit Guard(StartupRsxProcess &process) : process_(process)
    {
        process_.lock_waiters.fetch_add(1u, std::memory_order_relaxed);   // visible to the pump, which yields between chunks (startup_rsx_pump)
        startup_spin_acquire(process_.lock);
        process_.lock_waiters.fetch_sub(1u, std::memory_order_relaxed);
    }
    ~Guard() { process_.lock.clear(std::memory_order_release); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
private:
    StartupRsxProcess &process_;
};
static int read32(void *user, uint32_t address, uint32_t *value)
{
    const ps3_guest_memory &memory = *static_cast<const ps3_guest_memory *>(user);
    unsigned char bytes[4];
    if (memory.read(memory.context, address, bytes, 4) != PS3_GUEST_MEMORY_OK) return 1;
    *value = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
    return 0;
}
static int write32(void *user, uint32_t address, uint32_t value)
{
    const ps3_guest_memory &memory = *static_cast<const ps3_guest_memory *>(user);
    const unsigned char bytes[4] = {static_cast<unsigned char>(value >> 24), static_cast<unsigned char>(value >> 16),
        static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value)};
    return memory.write(memory.context, address, bytes, 4) == PS3_GUEST_MEMORY_OK ? 0 : 1;
}
/* One context for the four port functions: guest memory plus the process (for its optional observers). */
struct PortContext { const ps3_guest_memory *memory; StartupRsxProcess *process; };
static int port_read32(void *user, uint32_t address, uint32_t *value)
{
    return read32(const_cast<ps3_guest_memory *>(static_cast<PortContext *>(user)->memory), address, value);
}
static int port_write32(void *user, uint32_t address, uint32_t value)
{
    return write32(const_cast<ps3_guest_memory *>(static_cast<PortContext *>(user)->memory), address, value);
}
static void port_method(void *user, uint32_t subchannel, uint32_t method, uint32_t data)
{
    StartupRsxProcess *process = static_cast<PortContext *>(user)->process;
    if (process->method_hook) process->method_hook(process->hook_user, subchannel, method, data);
}
static void port_flip(void *user, uint32_t buffer_id)
{
    StartupRsxProcess *process = static_cast<PortContext *>(user)->process;
    if (process->flip_hook) process->flip_hook(process->hook_user, buffer_id);
}
/* Root (2026-11-15): the FIFO words in place. Only the flat guest space can answer (its commit bitmap is read without a lock);
 * everything else, and every range with an uncommitted page, is NULL and goes through port_read32 as before. */
static const uint8_t *port_pointer(void *user, uint32_t address, uint32_t bytes)
{
    (void)user;
    return ps3_flat_guest_fast_range ? static_cast<const uint8_t *>(ps3_flat_guest_fast_range(address, bytes)) : nullptr;
}
static rsx_live_ports ports(PortContext &context)
{
    rsx_live_ports result{};
    result.user = &context;
    result.read32 = port_read32;
    result.write32 = port_write32;
    result.method = port_method;
    result.flip = port_flip;
    result.pointer = g_ps3_rsx_fifo_direct && ps3_flat_guest_fast_range ? port_pointer : nullptr;
    return result;
}
/* The caller holds the lock. The consumer is created on the first step after cellGcmInit: before that the
 * control register is zero and the init image has not been walked. */
static bool ensure_configured(StartupRsxProcess &process)
{
    if (process.configured) return true;
    if (!process.gcm || !process.gcm_ready || !process.gcm_ready(process.gcm)) return false;
    rsx_live_layout layout = process.layout;
    layout.io_address = process.gcm->as.cfg.io_ea;
    layout.io_size = process.gcm->as.cfg.io_size;
    rsx_live_init(&process.fifo, &layout);
    process.configured = true;
    return true;
}
}
/* Root (2026-11-15): the process lock is held for one chunk of words at a time (PS3RECOMP_RSX_PUMP_CHUNK; 0 = the whole
 * budget under one hold, as before), not for the whole pump. Run rv58s: a pump took ~230 ms with the lock held and the game's
 * main thread spent 5.5 % of its time (cellGcmSetFlipCommand, 27688 samples) waiting for that lock to take a flip counter.
 * Between chunks the lock is free for the guest's GCM imports; the consumer is still driven by one thread only. */
extern "C++" void startup_rsx_pump(StartupRsxProcess &process, const ps3_guest_memory &memory, uint32_t word_budget)
{
    const uint32_t chunk_setting = g_ps3_rsx_pump_chunk;
    const uint32_t chunk = chunk_setting ? chunk_setting : word_budget;
    uint32_t remaining = word_budget;
    const uint64_t pump_start = rsx_pump_now_ns();
    uint64_t waited = 0, chunks = 0;
    bool more = true;
    g_rsx_stats_process = &process;
    while (more) {
        more = false;
        {
            const uint64_t before_lock = rsx_pump_now_ns();
            rsx_live_bridge::Guard guard(process);
            waited += rsx_pump_now_ns() - before_lock;
            if (!rsx_live_bridge::ensure_configured(process)) break;
            rsx_live_bridge::PortContext context{&memory, &process};
            const rsx_live_ports ports = rsx_live_bridge::ports(context);
            const uint32_t take = remaining < chunk ? remaining : chunk;
            const uint64_t words_before = process.fifo.words;
            if (chunks == 0 && process.pumps != UINT64_MAX) ++process.pumps;   // process.pumps counts calls, not chunks
            process.fifo.get_batch_words = g_ps3_rsx_get_batch;
            process.last_status = uint32_t(rsx_live_pump(&process.fifo, &ports, take));
            ++chunks;
            const uint64_t used = process.fifo.words - words_before;
            remaining = used >= remaining ? 0u : remaining - uint32_t(used);
            more = process.last_status == uint32_t(RSX_LIVE_BUDGET) && remaining != 0u;
        }
        /* the lock is free here: a guest import that queued for it while the chunk ran gets one 50 us sleep's time to take it
         * (the spin lock is not fair, and this thread would otherwise take it again within nanoseconds) */
        if (more && process.lock_waiters.load(std::memory_order_relaxed) != 0u) startup_spin_relax();
    }
    g_rsx_pump_ns.fetch_add(rsx_pump_now_ns() - pump_start, std::memory_order_relaxed);
    g_rsx_lock_wait_ns.fetch_add(waited, std::memory_order_relaxed);
    g_rsx_chunks.fetch_add(chunks, std::memory_order_relaxed);
}
/* One line of the pump's own numbers, for the composition's once-a-minute renderer report (next to STARTUP_RENDER_TIME): pump_ms is
 * wall time inside startup_rsx_pump, so pump_ms - methods_ms - flips_ms is what this consumer and its hooks cost. Counters are read
 * without the lock (a torn line is possible and harmless). */
extern "C" void startup_rsx_print_stats(unsigned seconds)
{
    const StartupRsxProcess *process = g_rsx_stats_process;
    if (!process) return;
    const rsx_live_fifo &fifo = process->fifo;
    std::printf("STARTUP_RSX_PUMP seconds=%u pumps=%llu chunks=%llu pump_ms=%llu lock_wait_ms=%llu words=%llu packets=%llu methods=%llu flips=%llu "
                "direct_words=%llu port_words=%llu get_writes=%llu direct=%d get_batch=%u chunk=%u\n", seconds,
        (unsigned long long)process->pumps, (unsigned long long)g_rsx_chunks.load(std::memory_order_relaxed),
        (unsigned long long)(g_rsx_pump_ns.load(std::memory_order_relaxed) / 1000000u),
        (unsigned long long)(g_rsx_lock_wait_ns.load(std::memory_order_relaxed) / 1000000u),
        (unsigned long long)fifo.words, (unsigned long long)fifo.packets, (unsigned long long)fifo.methods, (unsigned long long)fifo.flips,
        (unsigned long long)fifo.direct_words, (unsigned long long)fifo.port_words, (unsigned long long)fifo.get_writes,
        int(g_ps3_rsx_fifo_direct), unsigned(g_ps3_rsx_get_batch), unsigned(g_ps3_rsx_pump_chunk));
}
// RSX_PUMP_TEST_REGION_END
static bool dispatch_rsx_import(StartupExecution &execution, uint32_t nid)
{
    switch (nid) {
    case gcm_set_flip_command_nid: case gcm_set_flip_command_wait_label_nid: case gcm_set_flip_immediate_nid:
    case gcm_set_flip_handler_nid: case gcm_set_vblank_handler_nid: case gcm_set_user_handler_nid:
    case gcm_set_graphics_handler_nid: case gcm_set_queue_handler_nid: case gcm_set_debug_output_level_nid:
    case gcm_sort_remap_nid: case gcm_address_to_offset_nid: break;
    default: return false;
    }
    if (!execution.rsx_process) return false;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    StartupRsxProcess &process = *execution.rsx_process;
    ppu_context &cpu = execution.ppu.cpu;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    check_boundary(BoundaryBudget::Poll);
    uint64_t result = 0;
    const auto handler = [&](unsigned index) {
        /* The guest passes an OPD address (0 clears). Recorded only: no handler is ever called yet. */
        rsx_live_bridge::Guard guard(process);
        process.handlers[index] = uint32_t(cpu.gpr[3]);
        if (process.handler_sets != UINT64_MAX) ++process.handler_sets;
    };
    switch (nid) {
    case gcm_set_flip_command_nid: case gcm_set_flip_command_wait_label_nid: {
        /* r3 = CellGcmContextData* { be32 begin, end, current, callback }, r4 = display buffer id; the wait-label
         * form adds r5 = label index, r6 = label value. The game has already reserved room (0x4C bytes) and
         * called the context callback itself when there was none, so the words are written at `current`. */
        const uint32_t context = uint32_t(cpu.gpr[3]);
        uint32_t end = 0, current = 0;
        if (!context || rsx_live_bridge::read32(const_cast<ps3_guest_memory *>(&memory), context + 4, &end) ||
            rsx_live_bridge::read32(const_cast<ps3_guest_memory *>(&memory), context + 8, &current))
            stop(StartupStop::InvalidMemory);
        uint32_t words[6]; unsigned count = 0;
        if (nid == gcm_set_flip_command_wait_label_nid) {
            words[count++] = (1u << 18) | RSX_LIVE_NV406E_SEMAPHORE_OFFSET; words[count++] = uint32_t(cpu.gpr[5]) * 16u;
            words[count++] = (1u << 18) | RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE; words[count++] = uint32_t(cpu.gpr[6]);
        }
        words[count++] = RSX_LIVE_FLIP_HEADER; words[count++] = uint32_t(cpu.gpr[4]);
        if (current > end || end - current < count * 4u) stop(StartupStop::MissingServices);   // no room: the callback path is not served
        for (unsigned index = 0; index < count; ++index)
            if (rsx_live_bridge::write32(const_cast<ps3_guest_memory *>(&memory), current + index * 4u, words[index]))
                stop(StartupStop::InvalidMemory);
        if (rsx_live_bridge::write32(const_cast<ps3_guest_memory *>(&memory), context + 8, current + count * 4u))
            stop(StartupStop::InvalidMemory);
        { rsx_live_bridge::Guard guard(process); if (process.flip_commands != UINT64_MAX) ++process.flip_commands; }
        break;
    }
    case gcm_set_flip_immediate_nid: {
        rsx_live_bridge::Guard guard(process);
        if (rsx_live_bridge::ensure_configured(process)) {
            rsx_live_bridge::PortContext context{&memory, &process};
            const rsx_live_ports ports = rsx_live_bridge::ports(context);
            rsx_live_flip_now(&process.fifo, &ports, uint32_t(cpu.gpr[3]));
        }
        if (process.flip_immediate != UINT64_MAX) ++process.flip_immediate;
        break;
    }
    case gcm_set_flip_handler_nid: handler(0); break;
    case gcm_set_vblank_handler_nid: handler(1); break;
    case gcm_set_user_handler_nid: handler(2); break;
    case gcm_set_graphics_handler_nid: handler(3); break;
    case gcm_set_queue_handler_nid: handler(4); break;
    case gcm_set_debug_output_level_nid: {
        rsx_live_bridge::Guard guard(process);
        process.debug_level = uint32_t(cpu.gpr[3]);
        break;
    }
    case gcm_sort_remap_nid: break;
    default: {   // cellGcmAddressToOffset(r3 = address, r4 = u32 *offset)
        const uint32_t address = uint32_t(cpu.gpr[3]), output = uint32_t(cpu.gpr[4]);
        uint32_t offset = 0;
        bool found = false;
        {
            rsx_live_bridge::Guard guard(process);
            if (process.address_queries != UINT64_MAX) ++process.address_queries;
            if (process.gcm && process.gcm_ready && process.gcm_ready(process.gcm)) {
                const gcm_as_config &cfg = process.gcm->as.cfg;
                if (address >= cfg.io_ea && address - cfg.io_ea < cfg.io_size) { offset = address - cfg.io_ea; found = true; }
                else if (address >= cfg.local_base && address - cfg.local_base < cfg.local_size) { offset = address - cfg.local_base; found = true; }
            }
        }
        if (!found) { result = uint64_t(int64_t(int32_t(0x802100FFu))); break; }   // CELL_GCM_ERROR_FAILURE
        if (!output || rsx_live_bridge::write32(const_cast<ps3_guest_memory *>(&memory), output, offset))
            stop(StartupStop::InvalidMemory);
        break;
    }
    }
    cpu.gpr[3] = result;
    return true;
}
static bool dispatch_dialog_import(StartupExecution &execution, uint32_t nid);   // defined with the lv2 arms below
/* Root (2026-11-11): the 13 audio imports of native/domain/audio_service.h. NIDs checked by hash. */
/* All of it lives under PS3_NATIVE_LLE_SRE so only LLE builds link native/domain/audio_service.c. */
enum : uint32_t {
    audio_out_sound_availability_nid = 0xc01b4e7cu, audio_out_number_of_device_nid = 0xe5e2b09du,
    audio_out_device_info_nid = 0x7663e368u, audio_out_state_nid = 0xf4e3caa0u,
    audio_out_configure_nid = 0x4692ab35u, audio_init_nid = 0x0b168f92u, audio_quit_nid = 0xca5ac370u,
    audio_port_open_nid = 0xcd7bc431u, audio_port_start_nid = 0x89be28f2u,
    audio_port_stop_nid = 0x5b1e2c73u, audio_port_close_nid = 0x4129fe2du,
    audio_get_port_config_nid = 0x74a66af0u, audio_get_port_block_tag_nid = 0xe4046afeu
};
class AudioProcessGuard {
public:
    explicit AudioProcessGuard(StartupAudioProcess &process) : process_(process)
    {
        startup_spin_acquire(process_.lock);
    }
    ~AudioProcessGuard() { process_.lock.clear(std::memory_order_release); }
    AudioProcessGuard(const AudioProcessGuard &) = delete;
    AudioProcessGuard &operator=(const AudioProcessGuard &) = delete;
private:
    StartupAudioProcess &process_;
};
static void audio_store_be64(unsigned char *bytes, uint64_t value)
{
    for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value >> (56 - 8 * i));
}
/* The caller holds the lock. */
static void audio_tick_locked(StartupAudioProcess &process, const ps3_guest_memory &memory, uint64_t now_us)
{
    uint64_t indexes[AUDIO_PORT_COUNT]{};
    const uint32_t changed = audio_tick(&process.service, now_us, indexes, AUDIO_PORT_COUNT);
    if (process.ticks != UINT64_MAX) ++process.ticks;
    for (uint32_t port = 0; port < AUDIO_PORT_COUNT; ++port) {
        if (!(changed & (1u << port))) continue;
        unsigned char bytes[8];
        audio_store_be64(bytes, indexes[port]);
        if (memory.write && memory.write(memory.context, process.service.layout.index_base + 8u * port,
                bytes, sizeof(bytes)) == PS3_GUEST_MEMORY_OK) {
            if (process.index_stores != UINT64_MAX) ++process.index_stores;
        } else if (process.store_failures != UINT64_MAX) {
            ++process.store_failures;
        }
    }
}
/* This file region has C linkage (ps3_hle_call); the header declares this one as C++. */
extern "C++" void startup_audio_tick(StartupAudioProcess &process, const ps3_guest_memory &memory, uint64_t now_us)
{
    AudioProcessGuard guard(process);
    audio_tick_locked(process, memory, now_us);
}
static bool is_audio_import(uint32_t nid)
{
    switch (nid) {
    case audio_out_sound_availability_nid: case audio_out_number_of_device_nid:
    case audio_out_device_info_nid: case audio_out_state_nid: case audio_out_configure_nid:
    case audio_init_nid: case audio_quit_nid: case audio_port_open_nid: case audio_port_start_nid:
    case audio_port_stop_nid: case audio_port_close_nid: case audio_get_port_config_nid:
    case audio_get_port_block_tag_nid: return true;
    default: return false;
    }
}
/* Decision 132: the first calls of each audio import with their arguments, result and what was written back, printed
 * and flushed at once (a console run ends with HOME, so no summary at exit appears). PortOpen/Start/Stop/Close are
 * rare and all logged up to AUDIO_IMPORT_LOG_LIFECYCLE; the polled GetPortConfig and GetPortBlockTag only their first
 * few. */
enum : uint32_t { AUDIO_IMPORT_LOG_LIFECYCLE = 64u, AUDIO_IMPORT_LOG_CONFIG = 16u, AUDIO_IMPORT_LOG_BLOCK_TAG = 8u };
static uint32_t audio_load_be32(const unsigned char *bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}
static uint64_t audio_load_be64(const unsigned char *bytes)
{
    return (uint64_t(audio_load_be32(bytes)) << 32) | audio_load_be32(bytes + 4);
}
static void log_audio_import(uint32_t nid, const ppu_context &cpu, int32_t result, const unsigned char *image,
                             const unsigned char *param)
{
    struct Entry { uint32_t nid; const char *name; uint32_t limit; };
    static const Entry entries[] = {
        {audio_out_sound_availability_nid, "OutGetSoundAvailability", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_out_number_of_device_nid, "OutGetNumberOfDevice", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_out_device_info_nid, "OutGetDeviceInfo", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_out_state_nid, "OutGetState", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_out_configure_nid, "OutConfigure", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_init_nid, "Init", AUDIO_IMPORT_LOG_LIFECYCLE}, {audio_quit_nid, "Quit", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_port_open_nid, "PortOpen", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_port_start_nid, "PortStart", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_port_stop_nid, "PortStop", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_port_close_nid, "PortClose", AUDIO_IMPORT_LOG_LIFECYCLE},
        {audio_get_port_config_nid, "GetPortConfig", AUDIO_IMPORT_LOG_CONFIG},
        {audio_get_port_block_tag_nid, "GetPortBlockTag", AUDIO_IMPORT_LOG_BLOCK_TAG},
    };
    static std::atomic<uint32_t> seen[sizeof(entries) / sizeof(entries[0])];
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (entries[i].nid != nid) continue;
        const uint32_t n = seen[i].fetch_add(1, std::memory_order_relaxed);
        if (n >= entries[i].limit) return;
        char extra[192] = "";
        if (result == 0 && nid == audio_get_port_config_nid)
            std::snprintf(extra, sizeof extra, " read_index_addr=0x%08x status=0x%x channels=%llu blocks=%llu port_size=0x%x port_addr=0x%08x",
                unsigned(audio_load_be32(image)), unsigned(audio_load_be32(image + 4)),
                static_cast<unsigned long long>(audio_load_be64(image + 8)), static_cast<unsigned long long>(audio_load_be64(image + 16)),
                unsigned(audio_load_be32(image + 24)), unsigned(audio_load_be32(image + 28)));
        else if (nid == audio_port_open_nid && param)
            std::snprintf(extra, sizeof extra, " param_channels=%llu param_blocks=%llu attr=0x%llx level=0x%08x port=%u",
                static_cast<unsigned long long>(audio_load_be64(param)), static_cast<unsigned long long>(audio_load_be64(param + 8)),
                static_cast<unsigned long long>(audio_load_be64(param + 16)), unsigned(audio_load_be32(param + 24)),
                unsigned(audio_load_be32(image)));
        else if (result == 0 && nid == audio_get_port_block_tag_nid)
            std::snprintf(extra, sizeof extra, " tag=%llu", static_cast<unsigned long long>(audio_load_be64(image)));
        std::printf("STARTUP_AUDIO_IMPORT name=%s n=%u a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx lr=0x%llx result=0x%08x%s\n",
            entries[i].name, unsigned(n), static_cast<unsigned long long>(cpu.gpr[3]), static_cast<unsigned long long>(cpu.gpr[4]),
            static_cast<unsigned long long>(cpu.gpr[5]), static_cast<unsigned long long>(cpu.gpr[6]),
            static_cast<unsigned long long>(cpu.lr), unsigned(result), extra);
        std::fflush(stdout);
        return;
    }
}
/* Returns false when the NID is not an audio import or the process has no audio service (the caller then
 * falls through to UnknownImport, as before). Out images are built under the lock and written to guest
 * memory after it is released, only when the service answered 0. A non-NULL out pointer the guest
 * memory refuses is a stop, not a guest error. */
static bool dispatch_audio_import(StartupExecution &execution, uint32_t nid)
{
    if (!is_audio_import(nid) || !execution.audio_process) return false;
    if (!execution.ppu.admitted || !execution.ppu.memory || !execution.ppu.services ||
        !execution.ppu.services->clock || !execution.ppu.services->clock->now_microseconds)
        stop(StartupStop::MissingServices);
    StartupAudioProcess &process = *execution.audio_process;
    ppu_context &cpu = execution.ppu.cpu;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    check_boundary(BoundaryBudget::Poll);
    uint64_t now_us = 0;
    const ps3_clock *clock = execution.ppu.services->clock;
    if (clock->now_microseconds(clock->context, &now_us) != PS3_CLOCK_OK) stop(StartupStop::MissingServices);
    const uint32_t a0 = uint32_t(cpu.gpr[3]), a1 = uint32_t(cpu.gpr[4]), a2 = uint32_t(cpu.gpr[5]);
    unsigned char image[AUDIO_OUT_DEVICE_INFO_BYTES]{};
    uint32_t out_address = 0, out_bytes = 0;
    int32_t result = 0;
    /* PortOpen reads its parameter block before the lock: 28 bytes, be64 nChannel, be64 nBlock, be64 attr,
     * be32 level. */
    unsigned char param[28]{};
    bool param_ok = false;
    if (nid == audio_port_open_nid && a0 != 0 && a1 != 0) {
        if (memory.read(memory.context, a0, param, sizeof(param)) != PS3_GUEST_MEMORY_OK ||
            !writable(execution, a1, 4)) stop(StartupStop::MissingServices);
        param_ok = true;
    }
    const auto be64_at = [&](unsigned offset) {
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | param[offset + unsigned(i)];
        return value;
    };
    {
        AudioProcessGuard guard(process);
        audio_tick_locked(process, memory, now_us);
        audio_service *service = &process.service;
        switch (nid) {
        case audio_out_sound_availability_nid:
            result = audio_out_get_sound_availability(service, a0, a1, a2, uint32_t(cpu.gpr[6])); break;
        case audio_out_number_of_device_nid:
            result = audio_out_get_number_of_device(service, a0); break;
        case audio_out_device_info_nid:
            result = audio_out_get_device_info(service, a0, a1, a2 ? image : nullptr);
            out_address = a2; out_bytes = AUDIO_OUT_DEVICE_INFO_BYTES; break;
        case audio_out_state_nid:
            result = audio_out_get_state(service, a0, a1, a2 ? image : nullptr);
            out_address = a2; out_bytes = AUDIO_OUT_STATE_BYTES; break;
        case audio_out_configure_nid:
            result = audio_out_configure(service, a0, a1 != 0); break;
        case audio_init_nid: result = audio_init(service); break;
        case audio_quit_nid: result = audio_quit(service); break;
        case audio_port_open_nid: {
            uint32_t port = 0;
            if (!param_ok) { result = AUDIO_ERROR_PARAM; break; }
            const uint64_t channels = be64_at(0), blocks = be64_at(8);
            result = audio_port_open(service, channels > UINT32_MAX ? UINT32_MAX : uint32_t(channels),
                blocks > UINT32_MAX ? UINT32_MAX : uint32_t(blocks), be64_at(16),
                (uint32_t(param[24]) << 24) | (uint32_t(param[25]) << 16) | (uint32_t(param[26]) << 8) | param[27],
                &port);
            image[0] = static_cast<unsigned char>(port >> 24); image[1] = static_cast<unsigned char>(port >> 16);
            image[2] = static_cast<unsigned char>(port >> 8); image[3] = static_cast<unsigned char>(port);
            out_address = a1; out_bytes = 4; break;
        }
        case audio_port_start_nid: result = audio_port_start(service, a0, now_us); break;
        case audio_port_stop_nid: result = audio_port_stop(service, a0); break;
        case audio_port_close_nid: result = audio_port_close(service, a0); break;
        case audio_get_port_config_nid:
            result = audio_get_port_config(service, a0, a1 ? image : nullptr);
            out_address = a1; out_bytes = AUDIO_PORT_CONFIG_BYTES; break;
        case audio_get_port_block_tag_nid: {
            uint64_t tag = 0;
            result = audio_get_port_block_tag(service, a0, cpu.gpr[4], a2 ? &tag : nullptr);
            audio_store_be64(image, tag);
            out_address = a2; out_bytes = 8; break;
        }
        default: break;
        }
    }
    if (result == 0 && out_bytes != 0 && out_address != 0 &&
        memory.write(memory.context, out_address, image, out_bytes) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::MissingServices);
    log_audio_import(nid, cpu, result, image, param_ok ? param : nullptr);
    cpu.gpr[3] = uint64_t(int64_t(result));
    return true;
}
#endif

#ifdef PS3_NATIVE_LV2_SPU
/* Decision 33 / B2b: Sony's sysPrxForUser SPU-image imports, and the arms that serve them (defined
 * with the other SPU arms below). */
enum : uint32_t { spu_image_import_nid = 0xEBE5F72Fu, spu_image_close_nid = 0xE0DA8EFDu };
static void dispatch_spu_image_import(StartupExecution &execution, ppu_context &cpu);
static void dispatch_spu_image_close(StartupExecution &execution, ppu_context &cpu);
#endif
#ifdef PS3_NATIVE_LLE_PROFILE
/* cellSaveDataListAutoLoad (NID 0x21425307), Decision 83; defined after the lv2 helpers it uses. */
/* Decision 129: the load list, list-save and auto-save imports, defined with it. */
enum : uint32_t {
    savedata_list_auto_load_nid = 0x21425307u, savedata_list_load2_nid = 0x1DFBFDD6u, savedata_list_save2_nid = 0x2DE0D663u,
    savedata_auto_save2_nid = 0x8B7ED64Bu,
};
static void dispatch_savedata_list_auto_load(StartupExecution &execution, ppu_context &cpu);
static void dispatch_savedata_list_load2(StartupExecution &execution, ppu_context &cpu);
static void dispatch_savedata_list_save2(StartupExecution &execution, ppu_context &cpu);
static void dispatch_savedata_auto_save2(StartupExecution &execution, ppu_context &cpu);
#endif
#ifdef PS3_NATIVE_LIVE_TRACE
static void ps3_hle_call_body(unsigned int nid, ppu_context *cpu);
void ps3_hle_call(unsigned int nid, ppu_context *cpu)
{
    static thread_local unsigned int last_nid = 0;
    static thread_local uint32_t repeats = 0;
    repeats = nid == last_nid ? repeats + 1 : 0;
    last_nid = nid;
    g_live_trace_fold = repeats >= 8 && (repeats & 0xFFFu) != 0;
    ps3_hle_call_body(nid, cpu);   // a stop inside leaves the fold set; it only ever hides import lines
    LIVE_TRACE("LT hle-ret nid=0x%08x r3=0x%llx repeats=%u", nid, (unsigned long long)cpu->gpr[3], repeats);
    g_live_trace_fold = false;
}
static void ps3_hle_call_body(unsigned int nid, ppu_context *cpu)
#else
void ps3_hle_call(unsigned int nid, ppu_context *cpu)
#endif
{
    LIVE_TRACE("LT hle nid=0x%08x lr=0x%llx", nid, (unsigned long long)cpu->lr);
    activity_profile::Scope activity_scope(2, nid, uint32_t(cpu->lr), cpu->gpr[3], &cpu->gpr[3]);
    check_boundary(BoundaryBudget::LegacyOnly);
    if (cpu != active->cpu) stop(StartupStop::MissingServices);
    auto *execution = &execution_owner(cpu);
    if (execution->control != active) stop(StartupStop::MissingServices);
    admit_startup_budget(*execution, infamous::discovery::Category::Hle);
    if (execution->observation) {
        if (!execution->observation->try_record_import(nid))
            stop(StartupStop::ObservationCapacity);
    } else {
#ifdef PS3_NATIVE_LLE_PROFILE
        /* LLE profile: the fixed import record is a diagnostic, not a budget. A child thread (no observation object)
         * made its 65th import and was stopped mid-work. Keep the first STARTUP_IMPORT_CAPACITY entries, count the rest. */
        if (active->import_count < STARTUP_IMPORT_CAPACITY) active->imports[active->import_count++] = nid;
        else if (active->import_overflow != UINT64_MAX) ++active->import_overflow;
#else
        if (active->import_count >= STARTUP_IMPORT_CAPACITY) stop(StartupStop::TraceCapacity);
        active->imports[active->import_count++] = nid;
#endif
    }
#if defined(PS3_NATIVE_FAST_SYNC) && defined(PS3_NATIVE_STARTUP_LWMUTEX)
    /* ppu-speed-20261113 item 1: sys_lwmutex_lock / sys_lwmutex_unlock are 29 % of the main thread's time in the title scene, so
     * they go straight to their arm, before the arm chains below that cannot match them. Only when libsre does not export them
     * (it never does: they are sysPrxForUser imports of libsre itself), asked once. */
    if ((nid == 0x1573dc3f || nid == 0x1bc200f4) && __atomic_load_n(&g_fast_sync.pointer, __ATOMIC_ACQUIRE)) {
#ifdef PS3_NATIVE_LLE_SRE
        static const bool hoistable = !lle_sre_exports_nid(0x1573dc3f) && !lle_sre_exports_nid(0x1bc200f4);
#else
        static const bool hoistable = true;
#endif
        if (hoistable) { dispatch_mutex(execution, nid); return; }
    }
#endif
#ifdef PS3_NATIVE_LLE_SRE
    /* Decision 33: before EVERY hand-written arm, and for the same reason the flag exists. If Sony's
     * library exports the NID, Sony's own code runs it: no call may reach a cellSpurs/cellSync HLE
     * dispatch while the real library is mapped, so one library is never served from two places. A
     * NID libsre does not export falls through to the HLE arms exactly as before. */
    if (lle_sre_exports_nid(nid)) {
        const uint64_t caller_toc = cpu->gpr[2];
        // Counted BEFORE the entry: the lifted body may itself stop (an unserved syscall, an unserved
        // import), and the record of "Sony's code ran this NID" must survive that terminal stop.
        active->lle_sre_last_nid = nid;
        ++active->lle_sre_routed;
        const size_t budget = SuspendedTraceBatch::remaining(execution->ppu.recorder);
        const bool suspended = SuspendedTraceBatch::close(execution->ppu.recorder);
        const LleSreRouteStatus route = lle_sre_call_export(nid, cpu,
            &execution->ppu.memory->coherent_memory);
        LIVE_TRACE("LT lle-ret nid=0x%08x r3=0x%llx", nid, (unsigned long long)cpu->gpr[3]);
        /* The caller's own publications already happened before this route; reopening a batch here
         * would leave it open with no owner and make the NEXT entry refuse. The recorder is left
         * closed and the caller's frame is over: the route owns the whole caller for this NID. */
        ++active->lle_sre_toc_restores;
        if (cpu->gpr[2] != caller_toc) {
            ++active->lle_sre_toc_failures;
            stop(StartupStop::MissingServices);
        }
        if (route != LleSreRouteStatus::Routed) stop(StartupStop::MissingServices);
        return;
    }
#endif
#ifdef PS3_NATIVE_LLE_SRE
    if (is_lle_libc_import(nid)) { dispatch_lle_libc(*execution, nid); return; }
    if (is_lle_process_import(nid)) { dispatch_lle_process_import(*execution, nid); return; }
    if (dispatch_dialog_import(*execution, nid)) return;   // before the absent table: it owns cellSysutilCheckCallback when a dialog service exists
    if (dispatch_absent_import(*execution, nid)) return;
    if (dispatch_audio_import(*execution, nid)) return;
    if (dispatch_rsx_import(*execution, nid)) return;
#endif
#ifdef PS3_NATIVE_LV2_SPU
    /* Decision 33 / B2b: the SPU image imports Sony's libsre makes through sysPrxForUser. */
    if (nid == spu_image_import_nid) { dispatch_spu_image_import(*execution, *cpu); return; }
    if (nid == spu_image_close_nid) { dispatch_spu_image_close(*execution, *cpu); return; }
#endif
    if (nid == boot_disc_info_nid) { dispatch_boot_disc_info(*execution); return; }
    if (nid == system_parameter_nid) { dispatch_system_parameter(*execution); return; }
    if (nid == system_string_nid) { dispatch_system_string(*execution); return; }
    if (nid == disc_change_registration_nid) { dispatch_disc_change_registration(*execution); return; }
    if (nid == video_output_count_nid) { dispatch_video_output_count(*execution); return; }
    if (nid == video_device_info_nid) { dispatch_video_device_info(*execution); return; }
    if (nid == video_availability_nid) { dispatch_video_availability(*execution); return; }
    if (nid == video_resolution_nid) { dispatch_video_resolution(*execution); return; }
    if (nid == video_state_nid) { dispatch_video_state(*execution); return; }
    if (nid == video_configure_nid) { dispatch_video_configure(*execution); return; }
#ifdef PS3_NATIVE_GAMEDATA
    if (nid == gamedata_check_create2_nid) { dispatch_gamedata_check_create2(*execution); return; }
    if (nid == gamedata_get_size_kb_nid) { dispatch_gamedata_get_size_kb(*execution); return; }
    if (nid == ppu_thread_exit_nid) { dispatch_ppu_thread_exit(*execution); return; }
#endif
#ifdef PS3_NATIVE_LLE_PROFILE
    if (nid == savedata_list_auto_load_nid) { dispatch_savedata_list_auto_load(*execution, *cpu); return; }
    if (nid == savedata_list_load2_nid) { dispatch_savedata_list_load2(*execution, *cpu); return; }
    if (nid == savedata_list_save2_nid) { dispatch_savedata_list_save2(*execution, *cpu); return; }
    if (nid == savedata_auto_save2_nid) { dispatch_savedata_auto_save2(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_PAD
    if (nid == cell_pad_init_nid) { dispatch_pad_init(*execution); return; }
    if (nid == cell_pad_set_port_setting_nid) { dispatch_pad_set_port_setting(*execution); return; }
    if (nid == cell_pad_get_info2_nid) { dispatch_pad_get_info2(*execution); return; }
    if (nid == cell_pad_get_data_nid) { dispatch_pad_get_data(*execution); return; }
    if (nid == cell_pad_set_act_direct_nid) { dispatch_pad_set_act_direct(*execution); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_FS
    if (nid == fs_stat_nid || nid == fs_opendir_nid || nid == fs_readdir_nid ||
        nid == fs_closedir_nid || nid == fs_rmdir_nid || nid == fs_unlink_nid) {
        dispatch_fs(*execution, nid);
        return;
    }
    if (nid == fs_open_nid || nid == fs_read_nid || nid == fs_write_nid ||
        nid == fs_close_nid || nid == fs_fstat_nid || nid == fs_lseek_nid ||
        nid == fs_mkdir_nid || nid == fs_rename_nid || nid == fs_utime_nid ||
        nid == fs_fsync_nid) {
        dispatch_fs_syscall(*execution, *cpu, fs_syscall_of(nid), true);
        return;
    }
#endif
#ifdef PS3_NATIVE_GCM_INIT
    if (nid == gcm_init_body_nid) { dispatch_gcm_init(*execution); return; }
    if (nid == gcm_get_configuration_nid) { dispatch_gcm_query(*execution, nid); return; }
    if (nid == gcm_get_label_address_nid) { dispatch_gcm_query(*execution, nid); return; }
    if (nid == gcm_get_offset_table_nid) { dispatch_gcm_query(*execution, nid); return; }
    if (nid == gcm_get_control_register_nid) { dispatch_gcm_query(*execution, nid); return; }
    if (nid == gcm_set_flip_mode_nid) { dispatch_gcm_set_flip_mode(*execution); return; }
    if (nid == gcm_set_tile_info_nid) { dispatch_gcm_set_tile_info(*execution); return; }
    if (nid == gcm_bind_tile_nid) { dispatch_gcm_bind_tile(*execution); return; }
    if (nid == gcm_bind_zcull_nid) { dispatch_gcm_bind_zcull(*execution); return; }
    if (nid == gcm_unbind_zcull_nid) { dispatch_gcm_unbind(*execution, nid); return; }
    if (nid == gcm_unbind_tile_nid) { dispatch_gcm_unbind(*execution, nid); return; }
    if (nid == gcm_set_display_buffer_nid) { dispatch_gcm_set_display_buffer(*execution); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
    if (is_spurs_attribute_import(nid)) { dispatch_spurs_attribute(*execution, nid); return; }
    if (nid == spurs_workload_attribute_initialize_nid) { dispatch_spurs_workload_attribute(*execution); return; }
    if (nid == spurs_workload_attribute_set_name_nid) { dispatch_spurs_workload_name(*execution); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    if (nid == spurs_initialize_nid) { dispatch_spurs_initialize(*execution); return; }
    if (nid == spurs_attach_nid) { dispatch_spurs_attach(*execution); return; }
    if (nid == spurs_get_info_nid) { dispatch_spurs_get_info(*execution); return; }
    if (nid == spurs_add_workload_nid) { dispatch_spurs_registration(*execution); return; }
    if (nid == spurs_request_idle_nid) { dispatch_spurs_idle(*execution); return; }
    // SPURS taskset setup block, first two entries: both pure struct init.
    if (nid == spurs_taskset_attribute_initialize_nid) { dispatch_spurs_taskset_attribute_initialize(*execution); return; }
    if (nid == spurs_event_flag_initialize_nid) { dispatch_spurs_event_flag_initialize(*execution); return; }
    // The next two entries of the same block.
    if (nid == spurs_taskset_attribute_set_name_nid) { dispatch_spurs_taskset_attribute_set_name(*execution); return; }
    if (nid == spurs_event_flag_attach_nid) { dispatch_spurs_event_flag_attach(*execution); return; }
    // The block's creation entry: validates, writes the
    // 6400-byte CellSpursTaskset image and registers the taskset as a workload through the native path.
    if (nid == spurs_create_taskset_with_attribute_nid) { dispatch_spurs_create_taskset_with_attribute(*execution); return; }
    // The next entry on the same path: validates, writes the
    // 128-byte CellSyncLFQueue line and the two auxiliary runs, and publishes the state word.
    if (nid == spurs_lfqueue_initialize_nid) { dispatch_spurs_lfqueue_initialize(*execution); return; }
    // The next entry on the same path: the line's
    // owner resolution, the shape gate and one or two create+dynamic-connect iterations, then the
    // publication. Its result must be 0 -- a non-zero r3 sends the caller to a
    // print-then-trap fatal assert.
    if (nid == spurs_lfqueue_attach_nid) { dispatch_spurs_lfqueue_attach(*execution); return; }
    // cellSpursCreateTask: validates the SPU ELF,
    // claims a taskset slot, writes the per-task record and raises the workload's ready count.
    if (nid == spurs_task_create_nid) { dispatch_spurs_task_create(*execution); return; }
    if (nid == spurs_ready_count_store_nid) { dispatch_spurs_ready(*execution); return; }
    if (nid == spurs_get_workload_info_nid) { dispatch_spurs_workload_info(*execution); return; }
    if (nid == spurs_set_exception_nid || nid == spurs_unset_exception_nid) { dispatch_spurs_exception(*execution, nid); return; }
    if (nid == ppu_thread_create_nid) { dispatch_ppu_thread_create(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
    if (nid == 0x9d98afa0) { dispatch_sysutil_callback_registration(*execution); return; }
#endif
#ifdef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
    if (nid == 0x32267a31) { dispatch_diagnostic_module_admission(*execution); return; }
    if (nid == 0x63ff6ff9) { dispatch_sysmodule_initialize(*execution); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_EXITSPAWN
    if (nid == 0x2c847572 || nid == 0x96328741) { dispatch_exitspawn(*execution, nid); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_LWCOND
    if (is_condition_import(nid)) { dispatch_condition(*execution, nid); return; }
#endif
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    if (nid == 0x350d454e) { dispatch_thread_id(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
    if (is_mutex_import(nid)) { dispatch_mutex(execution, nid); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_CLOCK
    if (nid == 0x8461e528) {
        if (!execution->ppu.services || !execution->ppu.services->clock ||
            !execution->ppu.services->clock->now_microseconds) stop(StartupStop::MissingServices);
        const uint64_t toc = cpu->gpr[2], slot = cpu->gpr[1] + 0x28;
        if (slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory);
        preflight_toc(*execution, slot);
        HleTraceBatch batch;
        begin_trace(*execution, 1, batch);
        uint64_t sample = 0;
        const ps3_clock *clock = execution->ppu.services->clock;
        const ps3_clock_status status = clock->now_microseconds(clock->context, &sample);
        active->clock_status = status;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
        if (!active->clock_calls && status == PS3_CLOCK_OK) active->first_clock_sample = sample;
#endif
        ++active->clock_calls;
        if (status == PS3_CLOCK_OK) active->clock_sample = sample;
        if (status != PS3_CLOCK_OK) stop(StartupStop::ClockProviderFailure, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        publish_toc(*execution, slot, toc, batch);
        finish_owned(&batch);
        cpu->gpr[2] = toc;
        cpu->gpr[3] = sample;
        ++active->clock_returns;
        return;
    }
#endif
    if (nid != 0x744680a2) stop(StartupStop::UnknownImport);
    if (!execution->ppu.services || !execution->ppu.services->initialize_tls || active->tls_returns) stop(StartupStop::MissingServices);
    const uint64_t toc = cpu->gpr[2];
    const uint64_t toc_slot = cpu->gpr[1] + 0x28;
    if (toc_slot < cpu->gpr[1]) stop(StartupStop::InvalidMemory);
    preflight_toc(*execution, toc_slot);
    HleTraceBatch batch;
    begin_trace(*execution, 1, batch);
    active->before_tls = *cpu;
    Ps3TlsTemplate tls = {uint32_t(cpu->gpr[4]), cpu->gpr[5], cpu->gpr[6]};
    Ps3TlsImage image = {};
    // Provider resources unwind normally before any stop/cancellation path below.
    Ps3TlsReport report = execution->ppu.services->initialize_tls(execution->ppu.services->context, &tls, &image);
    if (report.status != PS3_TLS_OK) stop(StartupStop::ProviderFailure, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    if (!image.allocation_size || image.block % 4096 || image.allocation_size % 4096 ||
        image.allocation_size > UINT64_C(0x100000000) - image.block ||
        image.thread_pointer < image.block || image.thread_pointer - image.block >= image.allocation_size ||
        !writable(*execution, image.block, image.allocation_size)) stop(StartupStop::InvalidMemory, &batch);
    ps3_atomic_pin initialized_tls{};
    ps3_atomic_domain *domain = execution->ppu.memory->domain;
    if (ps3_atomic_pin_acquire(domain, image.block, image.allocation_size, &initialized_tls) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    if (ps3_atomic_pin_release(domain, &initialized_tls) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_toc(*execution, toc_slot, toc, batch);
    finish_owned(&batch);
    cpu->gpr[2] = toc;
    cpu->gpr[13] = image.thread_pointer;
    cpu->gpr[3] = 0;
    active->initialized_tls_template = tls;
    active->initialized_tls_template_valid = true;
    ++active->tls_returns;
    active->after_tls = *cpu;
    // Genuine return through the ORIGINAL TLS import stub and startup call site.
}
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
void ps3_indirect_call(ppu_context *cpu)
{
    if (!active || cpu != active->cpu) std::abort();
    auto &execution = execution_owner(cpu);
    if (execution.control != active || !execution.ppu.services->compiled_code ||
        !execution.ppu.services->compiled_code->find || !execution.dispatch_stop.stop_requested)
        stop(StartupStop::MissingServices);
    if (execution.ppu.budget) {
        check_boundary(BoundaryBudget::LegacyOnly);
        admit_startup_budget(execution, infamous::discovery::Category::Indirect);
    }
    if (execution.observation && execution.dispatch.depth < 8u) {   // Decision 89: deeper calls go unobserved (below)
        using namespace infamous::diagnostics;
        const DispatchEntry entry{cpu->ctr, cpu->lr, cpu->gpr[1], execution.dispatch.depth};
        const auto storage = execution.dispatch.depth >= execution.dispatch.limit
            ? DispatchStorage::ImmediateNonInvoking : DispatchStorage::Pending;
        DispatchTicket ticket;
        if (!execution.observation->try_begin_dispatch(entry, storage, ticket))
            stop(StartupStop::ObservationCapacity);
        const auto result = ps3_compiled_dispatch(&execution.dispatch,
            *execution.ppu.services->compiled_code, execution.dispatch_stop, cpu, entry.captured_ctr);
        if (storage == DispatchStorage::ImmediateNonInvoking &&
            (result.status == PS3_COMPILED_DISPATCH_OK || result.lookup_attempted))
            std::abort();
        execution.observation->finish_dispatch(ticket, execution.dispatch.depth,
            {uint32_t(result.status), result.lookup_attempted, uint32_t(result.lookup_status)});
        // Preserve the actual core's validation/error precedence. No clock or
        // cancellation callback intervenes between its return and publication.
        if (result.status == PS3_COMPILED_DISPATCH_STOP_CANCELLED) stop(StartupStop::Cancelled);
        if (result.status == PS3_COMPILED_DISPATCH_STOP_DEADLINE) stop(StartupStop::Deadline);
        if (result.status != PS3_COMPILED_DISPATCH_OK) stop(StartupStop::DispatchFailure);
        return;
    }
#ifdef PS3_NATIVE_LLE_PROFILE
    /* LLE profile: the fixed dispatch record is a diagnostic, not a budget. A child thread (no observation object)
     * that fills it keeps dispatching; only the record saturates and the excess is counted. */
    const bool has_record_room = active->dispatch_count < STARTUP_DISPATCH_CAPACITY;
    if (!has_record_room && active->dispatch_overflow != UINT64_MAX) ++active->dispatch_overflow;
#else
    if (active->dispatch_count >= STARTUP_DISPATCH_CAPACITY) stop(StartupStop::TraceCapacity);
    const bool has_record_room = true;
#endif
    StartupDispatchRecord *record = has_record_room
        ? &active->dispatches[active->dispatch_count++] : nullptr;
    if (record) {
        record->captured_ctr = cpu->ctr;
        record->depth_before = execution.dispatch.depth;
        record->before = *cpu;
    }
    // The lookup and stop ports return before guest entry. This frame has only
    // trivial borrowed state; a terminal target leaves depth as joined evidence.
    const auto result = ps3_compiled_dispatch(&execution.dispatch,
        *execution.ppu.services->compiled_code, execution.dispatch_stop, cpu,
        record ? record->captured_ctr : cpu->ctr);
    if (record) {
        record->result = result;
        record->depth_after = execution.dispatch.depth;
        record->core_returned = true;
    }
    if (result.status == PS3_COMPILED_DISPATCH_STOP_CANCELLED) stop(StartupStop::Cancelled);
    if (result.status == PS3_COMPILED_DISPATCH_STOP_DEADLINE) stop(StartupStop::Deadline);
    if (result.status != PS3_COMPILED_DISPATCH_OK) stop(StartupStop::DispatchFailure);
}
#endif
[[noreturn]] void native_startup_unsupported(unsigned int operation)
{
    if (!active) std::abort();
    active->external_id = operation;
    stop(StartupStop::UnsupportedExternal);
}
#ifdef PS3_NATIVE_STARTUP_TIMEBASE_COUNTER
uint64_t ppu_native_timebase_now(ppu_context *cpu)
{
    check_boundary(BoundaryBudget::LegacyOnly);
    // Check the worker-bound identity before recovering the larger owner.
    if (!cpu || cpu != active->cpu) stop(StartupStop::MissingServices);
    auto &execution = execution_owner(cpu);
    if (execution.control != active || !execution.ppu.admitted || !execution.ppu.services)
        stop(StartupStop::MissingServices);
    admit_startup_budget(execution, infamous::discovery::Category::Generated);
    const ps3_clock *clock = execution.ppu.services->clock;
    if (!clock || !clock->now_microseconds) stop(StartupStop::MissingServices);
    uint64_t microseconds = 0;
    if (active->timebase_attempts != SIZE_MAX) ++active->timebase_attempts;
    const auto status = clock->now_microseconds(clock->context, &microseconds);
    active->timebase_status = status;
    if (status != PS3_CLOCK_OK) stop(StartupStop::ClockProviderFailure);
    const uint64_t ticks = ps3_timebase_from_microseconds(microseconds);
    if (active->timebase_has_sample && active->timebase_last_microseconds == microseconds &&
        active->timebase_equal_samples != UINT64_MAX) ++active->timebase_equal_samples;
    active->timebase_has_sample = true;
    active->timebase_last_microseconds = microseconds;
    active->timebase_last_ticks = ticks;
    // Diagnostics retain a successful sample even if its return is suppressed.
    check_boundary(BoundaryBudget::Poll);
    return ticks;
}
#endif
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
static void dispatch_thread_priority(StartupExecution &execution, ppu_context &cpu)
{
    constexpr uint64_t cell_esrch_sign_extended = UINT64_C(0xffffffff80010005);
    auto &control = *execution.control;
    control.thread_priority_id = cpu.gpr[3];
    control.thread_priority_output = cpu.gpr[4];
    control.thread_priority_result = 0;
    control.thread_priority_value = 0;
    control.thread_priority_memory_status = PS3_GUEST_MEMORY_INVALID_RANGE;
    control.thread_priority_has_value = control.thread_priority_has_result = false;
    control.thread_priority_memory_checked = control.thread_priority_write_attempted = false;
    control.thread_priority_published = false;
    if (control.thread_priority_calls != SIZE_MAX) ++control.thread_priority_calls;
    if (!cpu.thread_id || cpu.thread_id >= PS3_LWMUTEX_RESERVED)
        stop(StartupStop::MissingServices);
    const uint32_t requested_id = uint32_t(cpu.gpr[3]);
    if (requested_id != cpu.thread_id) {
        // Startup admits only its primary guest thread. Creation or priority
        // mutation must extend this authority before those routes can be enabled.
        control.thread_priority_result = cell_esrch_sign_extended;
        control.thread_priority_has_result = true;
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[3] = cell_esrch_sign_extended;
        if (control.thread_priority_returns != SIZE_MAX) ++control.thread_priority_returns;
        return;
    }
    if (!execution.has_guest_priority) stop(StartupStop::MissingServices);
    const int32_t priority = execution.guest_priority;
    control.thread_priority_value = priority;
    control.thread_priority_has_value = true;
    const uint32_t output = uint32_t(cpu.gpr[4]);
    if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory);
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity);
    const auto &memory = execution.ppu.memory->coherent_memory;
    uint32_t probe = 0;
    control.thread_priority_memory_status = memory.read(memory.context, output, &probe, 4);
    control.thread_priority_memory_checked = true;
    if (control.thread_priority_memory_status != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    HleTraceBatch batch;
    begin_trace(execution, 1, batch);
    const uint32_t encoded = __builtin_bswap32(uint32_t(priority));
    control.thread_priority_write_attempted = true;
    control.thread_priority_memory_status = memory.write(memory.context, output, &encoded, 4);
    control.thread_priority_published = control.thread_priority_memory_status == PS3_GUEST_MEMORY_OK;
    if (!control.thread_priority_published) stop(StartupStop::ProviderFailure, &batch);
    control.thread_priority_result = 0;
    control.thread_priority_has_result = true;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        output, 4, uint32_t(priority)), batch);
    // The coherent port has released its guard. Retain the actual write and
    // publication even when this poll suppresses the guest register return.
    check_boundary(BoundaryBudget::Poll, &batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0;
    if (control.thread_priority_returns != SIZE_MAX) ++control.thread_priority_returns;
}
#endif
#ifdef PS3_NATIVE_STARTUP_TIMEZONE
static void dispatch_timezone(StartupExecution &execution, ppu_context &cpu)
{
    HleTraceBatch batch;
    if (!execution.fixed_timezone) stop(StartupStop::MissingServices, &batch);
    const uint64_t timezone_address = cpu.gpr[3], summertime_address = cpu.gpr[4];
    if (!timezone_address || !summertime_address || timezone_address > UINT32_MAX || summertime_address > UINT32_MAX ||
        !writable(execution, uint32_t(timezone_address), 4) || !writable(execution, uint32_t(summertime_address), 4))
        stop(StartupStop::InvalidMemory, &batch);
    if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity, &batch);
    const auto &memory = execution.ppu.memory->coherent_memory;
    uint32_t probe = 0;
    if (memory.read(memory.context, uint32_t(timezone_address), &probe, 4) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    if (memory.read(memory.context, uint32_t(summertime_address), &probe, 4) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const int32_t timezone = execution.fixed_timezone->timezone_minutes;
    const int32_t summertime = execution.fixed_timezone->summertime_minutes;
    ++active->timezone_calls;
    active->timezone_minutes = timezone;
    active->summertime_minutes = summertime;
    begin_trace(execution, 2, batch);
    const uint32_t encoded_timezone = __builtin_bswap32(uint32_t(timezone));
    if (memory.write(memory.context, uint32_t(timezone_address), &encoded_timezone, 4) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(timezone_address), 4, uint32_t(timezone)), batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const uint32_t encoded_summertime = __builtin_bswap32(uint32_t(summertime));
    if (memory.write(memory.context, uint32_t(summertime_address), &encoded_summertime, 4) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(summertime_address), 4, uint32_t(summertime)), batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0;
}
#endif
#ifdef PS3_NATIVE_STARTUP_TTY
// lv2 syscall 403 sys_tty_write: a native console sink. The game's libc write() (func_00452868) passes r3 = channel (fd 1 -> 0,
// fd 2 -> 1), r4 = buf, r5 = len and r6 = a u32 written-count pointer. Contract (INFERRED: documented lv2 behaviour, the same one
// RPCS3 implements; no kernel image was available to extract from): ch < 16, the buffer readable for len bytes, the count
// writable; the bytes are copied into the composition-owned ring and *pwritelen = len, then r3 = 0. Anything else stops; no
// errno is invented. Escaping keeps the sink printable and bounds it: bytes outside 0x20..0x7E except '\n' become \xNN, so a
// guest byte can never write past STARTUP_TTY_RING_BYTES.
// A guest read is admitted when the whole span lies in any bound RAM range, writable or not: the console
// source is often an ELF .rodata string, and only the 4-byte written count has to be writable.
static bool tty_readable(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory ||
        length > UINT64_C(0x100000000) - address) return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (address >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}

static void dispatch_tty_write(StartupExecution &execution, ppu_context &cpu)
{
    HleTraceBatch batch;
    StartupControl &control = *active;
    StartupTtyRing *const ring = execution.tty;
    if (control.tty_write_calls != SIZE_MAX) ++control.tty_write_calls;
    control.tty_last_channel = 0;
    control.tty_last_len = 0;
    if (!ring) stop(StartupStop::MissingServices, &batch);
    const uint32_t channel = uint32_t(cpu.gpr[3]);
    const uint32_t length = uint32_t(cpu.gpr[5]);
    const uint64_t buffer = cpu.gpr[4], written_address = cpu.gpr[6];
    control.tty_last_channel = channel;
    control.tty_last_len = length;
    if (channel >= 16) stop(StartupStop::UnsupportedSyscall, &batch);   // the lv2 TTY channel range
    if (!written_address || written_address > UINT32_MAX || !writable(execution, uint32_t(written_address), 4))
        stop(StartupStop::InvalidMemory, &batch);
    if (length > 0) {
        if (!buffer || buffer > UINT32_MAX || !tty_readable(execution, uint32_t(buffer), length))
            stop(StartupStop::InvalidMemory, &batch);
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        const auto &memory = execution.ppu.memory->coherent_memory;
        unsigned char probe = 0;
        if (memory.read(memory.context, uint32_t(buffer), &probe, 1) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::InvalidMemory, &batch);
    } else {
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
    }
    // Copy out of guest memory in bounded windows so no host buffer scales with the guest length.
    const auto &memory = execution.ppu.memory->coherent_memory;
    unsigned char window[64];
    for (uint32_t offset = 0; offset < length;) {
        size_t take = size_t(length - offset);
        if (take > sizeof(window)) take = sizeof(window);
        check_boundary(BoundaryBudget::Poll, &batch);
        if (memory.read(memory.context, uint32_t(buffer) + offset, window, take) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::InvalidMemory, &batch);
        startup_spin_acquire(ring->lock);
        for (size_t index = 0; index < take; ++index) {
            const unsigned char byte = window[index];
            const bool plain = byte == '\n' || (byte >= 0x20 && byte <= 0x7E);
            const size_t cost = plain ? 1 : 4;
            if (ring->used + cost > STARTUP_TTY_RING_BYTES) {
                ring->dropped += cost;   // escaped cost, so `used + dropped` bounds every guest byte that arrived
                continue;
            }
            if (plain) {
                ring->bytes[ring->used++] = char(byte);
            } else {
                static const char digits[] = "0123456789ABCDEF";
                ring->bytes[ring->used++] = '\\';
                ring->bytes[ring->used++] = 'x';
                ring->bytes[ring->used++] = digits[(byte >> 4) & 0xF];
                ring->bytes[ring->used++] = digits[byte & 0xF];
            }
        }
        ring->lock.clear(std::memory_order_release);
        offset += uint32_t(take);
    }
    if (control.tty_bytes + length < control.tty_bytes) control.tty_bytes = SIZE_MAX;
    else control.tty_bytes += length;
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    const uint32_t encoded = __builtin_bswap32(length);
    if (memory.write(memory.context, uint32_t(written_address), &encoded, 4) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(written_address), 4, length), batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0;
    if (control.tty_write_returns != SIZE_MAX) ++control.tty_write_returns;
}
#endif
#ifdef PS3_NATIVE_STARTUP_CURRENT_TIME
static void dispatch_current_time(StartupExecution &execution, ppu_context &cpu)
{
    HleTraceBatch batch;
    if (!execution.calendar_clock || !execution.calendar_clock->sample)
        stop(StartupStop::MissingServices, &batch);
    const uint64_t seconds_address = cpu.gpr[3], nanos_address = cpu.gpr[4];
    // This limited native slice rejects unverified desktop pointer aliases.
    if (!seconds_address || !nanos_address || seconds_address > UINT32_MAX || nanos_address > UINT32_MAX ||
        !writable(execution, uint32_t(seconds_address), 8) || !writable(execution, uint32_t(nanos_address), 8))
        stop(StartupStop::InvalidMemory, &batch);
    if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity, &batch);
    const auto &memory = execution.ppu.memory->coherent_memory;
    uint64_t probe = 0;
    if (memory.read(memory.context, uint32_t(seconds_address), &probe, 8) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    if (memory.read(memory.context, uint32_t(nanos_address), &probe, 8) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    ps3_calendar_time sample{};
    const auto status = ps3_calendar_clock_read(execution.calendar_clock, &sample);
    ++active->calendar_calls;
    active->calendar_status = status;
    if (status == PS3_CALENDAR_CLOCK_OK) active->calendar_sample = sample;
    if (status != PS3_CALENDAR_CLOCK_OK) stop(StartupStop::ClockProviderFailure, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 2, batch);
    const uint64_t seconds = uint64_t(sample.seconds);
    const uint64_t encoded_seconds = __builtin_bswap64(seconds);
    if (memory.write(memory.context, uint32_t(seconds_address), &encoded_seconds, 8) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(seconds_address), 8, seconds), batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const uint64_t encoded_nanos = __builtin_bswap64(uint64_t(sample.nanoseconds));
    if (memory.write(memory.context, uint32_t(nanos_address), &encoded_nanos, 8) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        uint32_t(nanos_address), 8, sample.nanoseconds), batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
/* Root (2026-11-11, hardware run r8): the per-thread ownership ledgers only grew, so a thread that created
 * and destroyed queues or ports (sc129 / sc135) ran out of ledger room while the registry had free slots.
 * Before a create checks for room, ids the registry no longer knows are dropped. */
static void compact_event_ledgers(StartupExecution &execution)
{
    ps3_lv2_objects *const objects = execution.event_queues;
    if (!objects) return;
    size_t kept = 0;
    for (size_t i = 0; i < active->event_queue_owned_count && i < PS3_LV2_QUEUE_CAP; ++i) {
        ps3_lv2_queue snapshot{};
        if (ps3_lv2_queue_snapshot(objects, active->event_queue_owned_ids[i], &snapshot) != PS3_LV2_ESRCH)
            active->event_queue_owned_ids[kept++] = active->event_queue_owned_ids[i];
    }
    active->event_queue_owned_count = kept;
    kept = 0;
    for (size_t i = 0; i < active->event_port_owned_count && i < PS3_LV2_PORT_CAP; ++i) {
        ps3_lv2_port snapshot{};
        if (ps3_lv2_port_snapshot(objects, active->event_port_owned_ids[i], &snapshot) != PS3_LV2_ESRCH)
            active->event_port_owned_ids[kept++] = active->event_port_owned_ids[i];
    }
    active->event_port_owned_count = kept;
}
// sc128: bridge sequencing only; ABI decoding, domain call and rollback live in startup_event_queue.h.
// Pre-effect: pointer validation, trace reservation, cancellation/deadline poll. Effect: the owned queue.
static void dispatch_event_queue_create(StartupExecution &execution, ppu_context &cpu)
{
    using namespace startup_event_queue;
    HleTraceBatch batch;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    Request request;
    switch (prepare(execution.event_queues, memory,
        [&execution](uint32_t address, uint64_t length) { return writable(execution, address, length); },
        cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], request)) {
    case Prepare::Ready: break;
    case Prepare::NoService: stop(StartupStop::MissingServices, &batch);
    case Prepare::BadPointer: stop(StartupStop::InvalidMemory, &batch);
    case Prepare::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    }
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
    // Invariant (registry capacity bounds live queues and nothing here destroys): the ownership ledger has room.
    if (active->event_queue_owned_count >= PS3_LV2_QUEUE_CAP) compact_event_ledgers(execution);
    if (active->event_queue_owned_count >= PS3_LV2_QUEUE_CAP) stop(StartupStop::ProviderFailure, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);   // output record reserved BEFORE the queue is allocated
    if (active->event_queue_calls != SIZE_MAX) ++active->event_queue_calls;
    const Outcome outcome = commit(execution.event_queues, memory, request);
    switch (outcome.status) {
    case Commit::Created:
        active->event_queue_created_id = outcome.queue_id;
        active->event_queue_owned_ids[active->event_queue_owned_count++] = outcome.queue_id;
        break;
    case Commit::GuestError:   // domain CELL return; nothing was allocated or stored
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[3] = uint64_t(int64_t(int32_t(outcome.cell)));
        return;
    case Commit::Unsupported: stop(StartupStop::UnsupportedSyscall, &batch);
    case Commit::RetainedOwnership:
        active->event_queue_retained_id = outcome.queue_id;   // still owned by the registry; terminal stop below
        active->event_queue_owned_ids[active->event_queue_owned_count++] = outcome.queue_id;   // shutdown retries release
        stop(StartupStop::ProviderFailure, &batch);
    case Commit::StoreFailed:
    case Commit::DomainFailure: stop(StartupStop::ProviderFailure, &batch);
    }
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true,
        request.output, 4, outcome.queue_id), batch);
    // The queue, its id and the stored output are a real commit: a stop here keeps them owned and only
    // suppresses r3 (same contract as the memory and SPU-initialize syscalls).
    check_boundary(BoundaryBudget::Poll, &batch);
    finish_owned(&batch);
    cpu.gpr[3] = 0;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
// sc134 sys_event_port_create: bridge sequencing only; ABI decoding, the domain call and the rollback live in startup_event_ports.h.
// Pre-effect: pointer validation, trace reservation check, ledger room, cancellation/deadline/budget poll. Effect: the owned port.
// Ownership is recorded IMMEDIATELY after the create (before the id store and before any later poll), so no later cancel, store
// failure or stop can orphan it; a failed store is rolled back, and a refused rollback keeps the record.
static void dispatch_event_port_create(StartupExecution &execution, ppu_context &cpu)
{
    using namespace startup_event_ports;
    HleTraceBatch batch;
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    CreateRequest request;
    switch (prepare_create(execution.event_queues, memory,
        [&execution](uint32_t address, uint64_t length) { return writable(execution, address, length); },
        cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], request)) {
    case CreatePrepare::Ready: break;
    case CreatePrepare::NoService: stop(StartupStop::MissingServices, &batch);
    case CreatePrepare::BadPointer: stop(StartupStop::InvalidMemory, &batch);
    case CreatePrepare::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    }
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
    // The ledger must have room BEFORE anything is allocated; the registry capacity bounds live ports, so a full ledger is an invariant stop.
    if (active->event_port_owned_count >= PS3_LV2_PORT_CAP) compact_event_ledgers(execution);
    if (active->event_port_owned_count >= PS3_LV2_PORT_CAP) stop(StartupStop::ProviderFailure, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);   // output record reserved BEFORE the port is allocated
    if (active->event_port_create_calls != SIZE_MAX) ++active->event_port_create_calls;
    active->event_port_last_type = request.type;
    active->event_port_last_name = request.name;
    const CreateOutcome created = create(execution.event_queues, request);
    switch (created.status) {
    case Create::Created: break;
    case Create::GuestError:   // domain CELL return (EINVAL type / EAGAIN capacity); nothing was allocated or stored
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[3] = uint64_t(int64_t(int32_t(created.cell)));
        return;
    case Create::Unsupported: stop(StartupStop::UnsupportedSyscall, &batch);
    case Create::DomainFailure: stop(StartupStop::ProviderFailure, &batch);
    }
    active->event_port_owned_ids[active->event_port_owned_count++] = created.port_id;   // ownership BEFORE the id store or any poll
    switch (publish(execution.event_queues, memory, request, created.port_id)) {
    case Publish::Stored: break;
    case Publish::StoreFailed:   // rolled back through the domain: the object is gone, so is its record (the id is never reused)
        --active->event_port_owned_count;
        stop(StartupStop::ProviderFailure, &batch);
    case Publish::RetainedOwnership:
        active->event_port_retained_id = created.port_id;   // still owned by the registry; the ledger keeps it for the shutdown release
        stop(StartupStop::ProviderFailure, &batch);
    }
    active->event_port_created_id = created.port_id;
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, request.output, 4, created.port_id), batch);
    // The port, its id and the stored output are a real commit: a stop here keeps them owned and only suppresses r3.
    check_boundary(BoundaryBudget::Poll, &batch);
    finish_owned(&batch);
    if (active->event_port_create_returns != SIZE_MAX) ++active->event_port_create_returns;
    cpu.gpr[3] = 0;
}
// sc136 sys_event_port_connect_local. Register-only: no guest memory, no trace record. Only a real connect between a port and a
// queue this guest created; a valid foreign resource is an explicit Unsupported stop before any effect. No filter is bound, no
// event is enqueued and nothing is woken (a blocked receiver on the queue stays blocked).
static void dispatch_event_port_connect(StartupExecution &execution, ppu_context &cpu)
{
    using namespace startup_event_ports;
    HleTraceBatch batch;
    ps3_lv2_objects *const lv2 = execution.event_queues;
    if (!lv2 || !lv2->initialized) stop(StartupStop::MissingServices, &batch);
    const uint32_t port = uint32_t(cpu.gpr[3]), queue = uint32_t(cpu.gpr[4]);   // high words are not part of the ids
    if (active->event_port_connect_calls != SIZE_MAX) ++active->event_port_connect_calls;
    active->event_port_connect_port = port;
    active->event_port_connect_queue = queue;
    check_boundary(BoundaryBudget::Poll, &batch);
    const Verdict verdict = classify(lv2, port, queue, active->event_port_owned_ids, active->event_port_owned_count,
                                     active->event_queue_owned_ids, active->event_queue_owned_count);
    if (verdict == Verdict::Foreign) stop(StartupStop::UnsupportedSyscall, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const ConnectOutcome outcome = verdict == Verdict::Absent
        ? ConnectOutcome{Connect::GuestError, PS3_LV2_ESRCH} : connect(lv2, port, queue);
    active->event_port_connect_cell = outcome.status == Connect::Connected ? 0 : outcome.cell;
    // The registry guard is released. Preserve the real effect before any terminal poll: a stop here suppresses r3 only.
    check_boundary(BoundaryBudget::Poll, &batch);
    switch (outcome.status) {
    case Connect::Connected: cpu.gpr[3] = 0; break;
    case Connect::GuestError: cpu.gpr[3] = uint64_t(int64_t(int32_t(outcome.cell))); break;
    case Connect::DomainFailure: stop(StartupStop::ProviderFailure, &batch);
    }
    if (active->event_port_connect_returns != SIZE_MAX) ++active->event_port_connect_returns;
}
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
// sys_event_queue_receive (sc130) through the generic borrowed StartupEventReceivePort. Register-only: no guest memory is read or
// written (r4 is a dummy buffer pointer that is never validated), so no trace record is ever reserved. The callback may genuinely
// block; no registry lock or memory pin is held here. The result is published only after the post-callback boundary check, so an
// execution cancelled/over budget while blocked never gets a guest continuation. Every failure is a terminal stop through stop().
static void dispatch_event_receive(StartupExecution &execution, ppu_context &cpu)
{
    HleTraceBatch batch;   // nothing is begun here; every stop still closes any pending owned batch before the notification
    const StartupEventReceivePort *const port = execution.event_receive;
    if (!port || !port->receive) stop(StartupStop::MissingServices, &batch);
    StartupEventReceiveResult (*const receive)(void *, uint32_t, uint64_t) = port->receive;
    void *const context = port->context;
    const uint32_t queue = uint32_t(cpu.gpr[3]);   // r3 high word is not part of the id
    const uint64_t timeout = cpu.gpr[5];           // r5 timeout in microseconds; r4 is deliberately ignored
    if (active->event_receive_calls != SIZE_MAX) ++active->event_receive_calls;
    active->event_receive_queue = queue;
    active->event_receive_timeout = timeout;
    check_boundary(BoundaryBudget::Poll, &batch);
    const StartupEventReceiveResult result = receive(context, queue, timeout);   // copied by value; may block
    check_boundary(BoundaryBudget::Poll, &batch);
    switch (result.outcome) {
    case StartupEventReceiveOutcome::Completed: break;
    case StartupEventReceiveOutcome::Cancelled: stop(StartupStop::Cancelled, &batch);
    case StartupEventReceiveOutcome::Unsupported: stop(StartupStop::UnsupportedSyscall, &batch);
    case StartupEventReceiveOutcome::ProviderFailure: stop(StartupStop::ProviderFailure, &batch);
    default: stop(StartupStop::ProviderFailure, &batch);   // never trust an out-of-range outcome
    }
    if (result.result == 0) {   // only success publishes the event; a completed guest error preserves r4..r7
        cpu.gpr[4] = result.source; cpu.gpr[5] = result.data1; cpu.gpr[6] = result.data2; cpu.gpr[7] = result.data3;
    }
    cpu.gpr[3] = uint64_t(int64_t(int32_t(result.result)));
    if (active->event_receive_returns != SIZE_MAX) ++active->event_receive_returns;
}
#endif
#ifdef PS3_NATIVE_LV2_SYNC
/* Decision 33 / B2a: the lv2 sync and object syscalls Sony's own libsre makes that are not SPU-thread
 * group calls. Every arm below is a thin ABI shell: it decodes r3.. from the guest, validates the
 * guest pointers against the binding's declared RAM, calls the shared ps3_lv2_objects domain, writes
 * the output big-endian through the coherent memory, and sets r3 sign-extended. No arm writes the
 * TOC slot (an lv2 syscall has no r2 contract) and no arm fabricates a success.
 *
 * Guest pointers are validated with writable()/readable() BEFORE the domain is called, so an
 * EFAULT output writes nothing at all. */
namespace lv2_sync {

constexpr uint32_t lv2_native_process_id = 0x01000500u;   // root (B2b): the sys_process_getpid (1) answer

enum : uint32_t {
    sc_process_spu_lock_line = 14, sc_process_get_sdk_version = 25, sc_timer_usleep = 141,
    sc_semaphore_create = 90, sc_semaphore_destroy = 91, sc_semaphore_wait = 92,
    sc_semaphore_trywait = 93, sc_semaphore_post = 94,
    sc_event_flag_create = 82, sc_event_flag_destroy = 83, sc_event_flag_wait = 85,
    sc_event_flag_trywait = 86, sc_event_flag_set = 87, sc_event_flag_clear = 118,
    sc_event_queue_destroy = 129, sc_event_queue_tryreceive = 131,
    sc_event_port_destroy = 135, sc_event_port_disconnect = 137, sc_event_port_send = 138,
    sc_memory_get_page_attribute = 351
};
/* lv2 sync flag/type constants the guest passes in its attribute structures. */
enum : uint32_t { sync_waiter_single = PS3_SYNC_WAITER_SINGLE, sync_waiter_multiple = PS3_SYNC_WAITER_MULTIPLE };
/* SPU lock-line flags (sys_process_is_spu_lock_line_reservation_address). */
enum : uint64_t { spu_lock_line_raw = 0x1, spu_lock_line_spu_thread = 0x2,
    spu_lock_line_known = 0x1 | 0x2 };
/* sys_page_attr_t: u64 attribute, u64 access_right, u32 page_size, u32 pad. */
enum : uint64_t { page_attribute_ppu = 0x40000, page_access_ppu_thread = 0x8, page_access_ppu = 0xF,
    page_size_small = 0x10000, page_size_large = 0x100000, page_size_4k = 0x1000 };
/* The stack region: guest r1 lives at 0xD00F0000..0xD0110000, and any address there is PPU-thread
 * stack, which the console reports with PPU_THR access rights and a 4 KiB page. */
constexpr uint32_t stack_region_high_nibble = 0xD;

constexpr uint64_t cell_ok = 0;
constexpr uint64_t sign_extend(uint32_t code) { return uint64_t(int64_t(int32_t(code))); }

bool readable_range(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory || length > UINT64_C(0x100000000) - address)
        return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (uint64_t(address) >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}

/* One traced big-endian guest write, charged to the trace budget before it happens. */
void publish_bytes(StartupExecution &execution, uint32_t address, const void *bytes, size_t length,
    HleTraceBatch &batch)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    uint64_t value = 0;
    const unsigned char *raw = static_cast<const unsigned char *>(bytes);
    for (size_t index = 0; index < length && index < 8; ++index) value = value << 8 | raw[index];
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, address,
        length, value), batch);
}
uint32_t spu_read_guest_u32(const StartupExecution &execution, uint32_t address)
{
    unsigned char raw[4]{};
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, address, raw, sizeof(raw)) != PS3_GUEST_MEMORY_OK) return 0;
    return (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) | (uint32_t(raw[2]) << 8) | raw[3];
}
void publish_u32(StartupExecution &execution, uint32_t address, uint32_t value, HleTraceBatch &batch)
{
    const uint32_t encoded = __builtin_bswap32(value);
    publish_bytes(execution, address, &encoded, sizeof(encoded), batch);
}
void publish_u64(StartupExecution &execution, uint32_t address, uint64_t value, HleTraceBatch &batch)
{
    const uint64_t encoded = __builtin_bswap64(value);
    publish_bytes(execution, address, &encoded, sizeof(encoded), batch);
}

/* Read `length` bytes from the guest into `out`. Every caller validated the range first. */
bool read_bytes(StartupExecution &execution, uint32_t address, void *out, size_t length)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    return memory.read(memory.context, address, out, length) == PS3_GUEST_MEMORY_OK;
}
uint32_t read_u32(StartupExecution &execution, uint32_t address)
{
    unsigned char raw[4]{};
    if (!read_bytes(execution, address, raw, sizeof(raw))) return 0;
    return (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) | (uint32_t(raw[2]) << 8) | uint32_t(raw[3]);
}
uint64_t read_u64(StartupExecution &execution, uint32_t address)
{
    unsigned char raw[8]{};
    if (!read_bytes(execution, address, raw, sizeof(raw))) return 0;
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value = value << 8 | raw[index];
    return value;
}

}  // namespace lv2_sync

/* sys_process_is_spu_lock_line_reservation_address (14).
 *
 * Caller evidence (lifted libsre, ppu_recomp_000.cpp:9182-9194 inside libsre_func_300075DC, reached
 * from cellSpursInitialize at lr 0x30008570):
 *     ctx->gpr[4] = 2;                     // SPU_THR
 *     ctx->gpr[3] = ctx->gpr[18];          // the CellSpurs object in game main memory
 *     ctx->gpr[11] = 0xE; lv2_syscall(ctx);
 *     cr12 <- (r3 < 0); on cr12 set, the caller re-enters the failure path (loc_3000766C) that
 *     returns CELL_SPURS_CORE_ERROR_*; on success it calls _sys_memset(r3, 0, 0x2000).
 * So r3 is an ADDRESS the caller then writes through, and r4 is the flag WORD (0x2 == SPU_THR), not
 * an address to validate. The console answers CELL_OK for main-memory addresses whose line can be
 * reserved, and THIS CALLER THEN USES r3 AS THE ADDRESS it hands to _sys_memset -- the success r3 is
 * the address, not merely 0 (the `cr` test is `r3 < 0`, so any non-negative r3 reads as success, and
 * `ctx->gpr[3] = ctx->gpr[18]` right after the syscall is what makes that concrete). We return the
 * address unchanged on success, so the caller's own memset target stays correct; the error paths
 * return the sign-extended negative code, which the same `r3 < 0` test takes. An address is
 * reserrable exactly when it lies in the binding's declared RAM, and the flags must be a subset of
 * RAW|SPU_THR. Nothing is fabricated. */
static void dispatch_spu_lock_line(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_sync;
    const uint32_t address = uint32_t(cpu.gpr[3]);
    const uint64_t flags = cpu.gpr[4];
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = sc_process_spu_lock_line;
    active->lv2_sync_argument = address;
    uint32_t code = PS3_LV2_OK;
    if (flags & ~spu_lock_line_known) code = PS3_LV2_EINVAL;
    else if (!readable_range(execution, address, 1)) code = PS3_LV2_EINVAL;
    /* Root fix (B2a Eden trace): the caller's test is EQUALITY, not sign. ppu_recomp_000.cpp:9190 sets cr3
     * from a compare of r3 with 0 and :9193 branches to the CELL_SPURS_CORE_ERROR_STAT (0x80410709) path when cr3.EQ
     * is clear; the memset target is r18, saved before the call (:9183/:9197). The worker returned the
     * address in r3, so every real init failed with STAT. lv2 returns CELL_OK (0). The comment above
     * describing an `r3 < 0` test and an address-valued success is wrong; kept for the record. */
    cpu.gpr[3] = code == PS3_LV2_OK ? cell_ok : sign_extend(code);
    ++active->lv2_sync_returns;
}

/* sys_process_get_sdk_version (25): pid, u32* version. pid 0 means "this process". */
static void dispatch_get_sdk_version(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_sync;
    const uint32_t pid = uint32_t(cpu.gpr[3]);
    const uint32_t output = uint32_t(cpu.gpr[4]);
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = sc_process_get_sdk_version;
    HleTraceBatch batch;
    if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
    // root (B2b Eden): libsre passes the sys_process_getpid answer straight back here.
    if (pid != 0 && pid != 1 && pid != lv2_native_process_id) {   // only this process is ours
        cpu.gpr[3] = sign_extend(PS3_LV2_ESRCH);
        return;
    }
    if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 1, batch);
    const uint32_t version = execution.process_sdk.valid ? execution.process_sdk.raw : 0;
    active->lv2_sync_argument = version;
    publish_u32(execution, output, version, batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    finish_owned(&batch);
    cpu.gpr[3] = cell_ok;
    ++active->lv2_sync_returns;
}

/* sys_memory_get_page_attribute (351): addr, sys_page_attr_t* (u64 attribute, u64 access_right,
 * u32 page_size, u32 pad).
 *
 * Caller evidence (lifted libsre, ppu_recomp_000.cpp:17903-17910 in libsre_func_3000EDB4):
 *     ctx->gpr[4] = ctx->gpr[1] + 0x70;    // the sys_page_attr_t lives on the caller's stack
 *     ctx->gpr[3] = ctx->gpr[29];          // an address taken from a descriptor the caller holds
 *     ctx->gpr[11] = 0x15F; lv2_syscall(ctx);
 *     cr12 <- (r3 < 0); on success the caller reads the FIRST word of the structure and keeps going.
 * So the console must fill the structure the caller points at. We report what the console reports:
 * the stack region is PPU-thread with a 4 KiB page; any other mapped RAM is PPU with a large page;
 * an unmapped address is EINVAL and the output is left untouched. */
static void dispatch_get_page_attribute(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_sync;
    const uint32_t address = uint32_t(cpu.gpr[3]);
    const uint32_t output = uint32_t(cpu.gpr[4]);
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = sc_memory_get_page_attribute;
    active->lv2_sync_argument = address;
    HleTraceBatch batch;
    if (!output || !writable(execution, output, 24)) stop(StartupStop::InvalidMemory, &batch);
    if (!readable_range(execution, address, 1)) {
        cpu.gpr[3] = sign_extend(PS3_LV2_EINVAL);
        ++active->lv2_sync_returns;
        return;
    }
    const bool is_stack = (address >> 28) == stack_region_high_nibble;
    const uint64_t access_right = is_stack ? page_access_ppu_thread : page_access_ppu;
    const uint32_t page_size = is_stack ? page_size_4k : page_size_large;
    if (!trace_can_begin(execution, 3)) stop(StartupStop::TraceCapacity, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 3, batch);
    active->lv2_sync_argument = page_attribute_ppu;
    publish_u64(execution, output, page_attribute_ppu, batch);
    publish_u64(execution, output + 8, access_right, batch);
    publish_u32(execution, output + 16, page_size, batch);
    publish_u32(execution, output + 20, 0, batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    finish_owned(&batch);
    cpu.gpr[3] = cell_ok;
    ++active->lv2_sync_returns;
}

/* sys_timer_usleep (141): u64 microseconds. A real sleep through the injected sleep port; without one
 * this is MissingServices, never a silent no-op. */
#ifdef __SWITCH__
extern "C" void *ps3_flat_guest_fast_pointer(uint32_t address, uint32_t length);
/* Root (2026-11-13, Decision 108): hardware runs rv37s and rv38s froze with the game's main thread in the wait loop of
 * func_002CFFA0 (usleep(1) with LR 0x2D0014, about 190,000 polls a second): it polls func_0041CF98(handle+4, 3, index)
 * until the WWS job manager's work list 3 reports that job index done, and that never happened. After about a second
 * of uninterrupted polling this prints the handle and the job manager's per-list progress words, then again every
 * ~2 million polls. Read-only. */
static void job_wait_diagnostic(const ppu_context &cpu)
{
    static thread_local uint32_t streak = 0;
    if (uint32_t(cpu.lr) != 0x002D0014u || cpu.gpr[3] != 1u) { streak = 0; return; }
    ++streak;
    if (streak != 200000u && (streak % 2000000u) != 0u) return;
    auto word = [](uint32_t address) -> uint32_t {
        const unsigned char *b = static_cast<const unsigned char *>(ps3_flat_guest_fast_pointer(address, 4u));
        return b ? (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3] : 0xDEADDEADu;
    };
    const uint32_t handle = uint32_t(cpu.gpr[31]);
    const uint32_t manager = word(handle + 4u);
    const uint32_t base = word(manager);
    char line[900]; int used = std::snprintf(line, sizeof line,
        "STARTUP_JOB_WAIT polls=%u handle=0x%08x [%08x %08x %08x %08x] manager=0x%08x base=0x%08x lists:", unsigned(streak), unsigned(handle),
        unsigned(word(handle)), unsigned(word(handle + 4u)), unsigned(word(handle + 8u)), unsigned(word(handle + 12u)), unsigned(manager), unsigned(base));
    for (uint32_t offset = 0; offset < 0xC0u && used < int(sizeof line) - 12; offset += 4u)
        used += std::snprintf(line + used, sizeof line - size_t(used), "%s%08x", (offset % 16u) == 0u ? " | " : " ", unsigned(word(base + offset)));
    std::printf("%s\n", line);
}
#endif

static void dispatch_timer_usleep(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_sync;
    const uint64_t microseconds = cpu.gpr[3];
#ifdef __SWITCH__
    job_wait_diagnostic(cpu);
#endif
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = sc_timer_usleep;
    active->lv2_sync_argument = microseconds;
    HleTraceBatch batch;
    if (!execution.lv2_sleep || !execution.lv2_sleep->sleep_microseconds)
        stop(StartupStop::MissingServices, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    /* Root (2026-11-11, Eden r16..r21): a sleeping thread has given the processor away, exactly like a thread in an
     * lv2 wait, but it did not say so: a creator waiting for its new higher-priority thread to block (the prefix wait
     * of ps3_ppu_service_start) never saw a thread whose loop only sleeps. The sleep now publishes BLOCKED on the
     * same word the lv2 waits use and wakes the creator; a zero-length sleep (a yield) does not. */
    uint64_t *const sleep_word = microseconds != 0 ? execution.lv2_blocked_word : nullptr;
    if (sleep_word) {
        __atomic_store_n(sleep_word, 1, __ATOMIC_SEQ_CST);
        if (execution.event_queues && execution.event_queues->port.wake_all)
            execution.event_queues->port.wake_all(execution.event_queues->port.context);
    }
    const bool ok = execution.lv2_sleep->sleep_microseconds(execution.lv2_sleep->context, microseconds,
        active->cancelled.load(std::memory_order_acquire));
    if (sleep_word) __atomic_store_n(sleep_word, 0, __ATOMIC_SEQ_CST);
    check_boundary(BoundaryBudget::Poll, &batch);
    if (!ok) stop(StartupStop::Cancelled, &batch);
    cpu.gpr[3] = cell_ok;
    ++active->lv2_sync_returns;
}

/* Root fix (B2a Eden review): a guest wait with timeout 0 (forever) blocked on lv2_sync_cancel, which
 * nothing sets when the discovery deadline passes, so Eden hung 90 s with no terminal record. The
 * wait now runs in bounded slices; between slices check_boundary turns deadline/cancel/fence into a
 * named stop while lv2_sync_number/lv2_sync_argument still name the blocked syscall and object. A
 * slice timeout is not a guest timeout: only the guest's own elapsed timeout returns ETIMEDOUT
 * (0x8001000B), never the domain's internal PS3_LV2_TIMED_OUT. Elapsed time is counted in whole
 * slices (it may overshoot by under one slice), which needs no host tick source. */
constexpr uint64_t lv2_wait_slice_us = 10000;
/* C1b: the word a blocking lv2 wait publishes into. A child PPU supplies its service activity word (so its
 * creator's prefix wait ends); everything else keeps the per-control scalar. */
static uint64_t *lv2_blocked_word(StartupExecution &execution, StartupControl *active)
{
    return execution.lv2_blocked_word ? execution.lv2_blocked_word : &active->lv2_sync_blocked;
}
constexpr uint32_t lv2_cell_etimedout = 0x8001000Bu;
extern "C++" {   // this region sits inside an extern "C" block; a template needs C++ linkage
template <typename WaitOnce>
static uint32_t lv2_sliced_wait(uint64_t timeout_us, HleTraceBatch &batch, WaitOnce wait_once)
{
    uint64_t remaining = timeout_us;
    for (;;) {
        const uint64_t slice = timeout_us && remaining < lv2_wait_slice_us ? remaining : lv2_wait_slice_us;
        const uint32_t code = wait_once(slice);
        if (code != PS3_LV2_TIMED_OUT) return code;
        if (timeout_us) {
            remaining -= slice;
            if (!remaining) return lv2_cell_etimedout;
        }
        check_boundary(BoundaryBudget::Poll, &batch);
    }
}
}  // extern "C++"

/* ---- Windows-port movie hooks in the lifted game image ---------------------------------------------- */
/* Root (2026-11-11). The lifted image is the Windows port's patched lift. Around the Bink player's setup
 * (func_00185050) it calls two helpers, because that port's main thread stored the movie texture pool at
 * player+0x4C later than the player thread read it:
 *   infamous_movie_pool_wait(player): wait, bounded, for the pool pointer before the movie lock is taken;
 *   infamous_movie_pool(player):      the pool pointer, or 0 = "not ready" (the player then gives the movie up).
 * The same order problem can exist here (host threads start slower than PS3 threads), so both keep their
 * meaning, on guest memory through this thread's own execution. They were fail-stop stubs before. */
extern "C++" { std::atomic<uint32_t> g_movie_pool_calls{0}, g_movie_pool_zero{0}, g_movie_pool_waits{0}, g_movie_pool_wait_timeouts{0}, g_movie_pool_last_player{0}; }
static uint32_t movie_pool_read(uint32_t player)
{
    if (!active || !active->cpu) return 0;
    StartupExecution &execution = execution_owner(active->cpu);
    if (!lv2_sync::readable_range(execution, player + 0x4Cu, 4)) return 0;
    return lv2_sync::read_u32(execution, player + 0x4Cu);
}
extern "C++" uint32_t infamous_movie_pool(uint32_t player)
{
    const uint32_t pool = movie_pool_read(player);
    g_movie_pool_calls.fetch_add(1, std::memory_order_relaxed);
    g_movie_pool_last_player.store(player, std::memory_order_relaxed);
    if (!pool) g_movie_pool_zero.fetch_add(1, std::memory_order_relaxed);
    return pool;
}
extern "C++" void infamous_movie_pool_wait(uint32_t player)
{
    if (!active || !active->cpu) return;
    StartupExecution &execution = execution_owner(active->cpu);
    if (!execution.lv2_sleep || !execution.lv2_sleep->sleep_microseconds) return;
    g_movie_pool_waits.fetch_add(1, std::memory_order_relaxed);
    /* Root (2026-11-12): the bound was about 2 s. Under an emulator the main thread can need longer than that to
     * store the pool, and a player that sees 0 gives the movie up for good. 60 s now; the timeouts are counted. */
    for (unsigned attempt = 0; attempt < 60000; ++attempt) {
        if (movie_pool_read(player) != 0) return;
        if (active->cancelled.load(std::memory_order_acquire)) return;
        if (!execution.lv2_sleep->sleep_microseconds(execution.lv2_sleep->context, 1000,
                active->cancelled.load(std::memory_order_acquire))) return;
    }    g_movie_pool_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
}

#ifdef PS3_NATIVE_LLE_PROFILE
/* ---- cellSaveData: the load list, auto-load, list-save and auto-save (Decisions 83 and 129) -------------- */
/* Root (2026-11-12, Decision 83). inFAMOUS starts a thread right after its install scan whose first call is
 *     cellSaveDataListAutoLoad(version, errDialog, setList, setBuf, funcFixed, funcStat, funcFile, container, userdata)
 * and the game waits for that thread. Without the call the thread ended (unknown import) and the console showed a
 * black screen for good. What a console does when NO save directory exists, and what is still done here:
 *   1. funcFixed(cbResult, listGet, fixedSet) with an empty list (dirNum = dirListNum = 0, dirList = setBuf->buf).
 *      The game names the directory it wants in fixedSet->dirName and puts its verdict in cbResult->result.
 *      A negative verdict ends the call with CELL_SAVEDATA_ERROR_CBRESULT.
 *   2. The named directory does not exist. funcStat(cbResult, statGet, statSet) is called with isNewData = 1, an
 *      all-zero directory record and the free space; a negative verdict (games answer ERR_NODATA here) ends the call
 *      with CELL_SAVEDATA_ERROR_CBRESULT.
 *   3. If the game accepts new data on a LOAD there is still nothing to read: CELL_SAVEDATA_ERROR_NODATA.
 *
 * Root (2026-11-16, Decision 129): real save data. The two SAVE imports, cellSaveDataListSave2 (0x2DE0D663) and
 * cellSaveDataAutoSave2 (0x8B7ED64B), were unknown imports: the thread that saved ended, the save never finished and
 * the game's dimmed "saving" screen never cleared. All four imports now run the console's callback protocol against
 * real files:
 *   list    funcList / funcFixed get the existing save directories under /dev_hdd0/home/<user id>/savedata whose
 *           names match setList->dirNamePrefix ('|'-separated) and carry a PARAM.SFO, sorted as setList asks;
 *   pick    with no system dialog the default the dialog would offer is taken (listSet->focusPosition: NEWDATA,
 *           DIRNAME, LISTHEAD, LISTTAIL, LATEST, OLDEST), else the first fixed entry, else (save) the new data;
 *   stat    funcStat gets isNewData, the directory times, the PARAM.SFO params and the file list (in setBuf->buf);
 *           on a save, statSet->setParam is the new PARAM.SFO and reCreateMode YES empties the directory first;
 *   file    funcFile runs until it answers OK_LAST: READ / WRITE / WRITE_NOTRUNC / DELETE on SECUREFILE,
 *           NORMALFILE, ICON0, ICON1, PIC1 and SND0, each reported back through fileGet->excSize;
 *   commit  a save writes PARAM.SFO last, so a directory whose save failed part way never lists as a save.
 * Every file goes through the process's own fs_service (the /dev_hdd0 mount and its confinement), never a host path.
 * SECUREFILE contents are stored as plain files: no PFD and no encryption; PARAM.SFO records which files are secure.
 * Each step prints one STARTUP_SAVEDATA_CALL line at once (console runs end with HOME, so a summary never prints).
 * The structures are the console's big-endian layouts, built in a frame below the caller's stack pointer, exactly
 * as dispatch_gamedata_check_create2 does for its one callback. */
extern "C++" { StartupSaveDataRecord g_savedata_auto_load; }
namespace savedata_abi {
enum : uint32_t {
    frame_bytes = 0x900, cb_result = 0x80, list_get = 0xA0, fixed_set = 0xF0, list_set = 0x100, file_get = 0x120,
    file_set = 0x170, stat_set = 0x1A0, stat_get = 0x200,
    cb_result_bytes = 0x14, cb_result_userdata = 0x10, list_get_bytes = 0x4C, fixed_set_bytes = 0x0C,
    list_set_bytes = 0x18, file_get_bytes = 0x44, file_set_bytes = 0x30, stat_get_bytes = 0x6A8, stat_set_bytes = 0x0C,
    error_cbresult = 0x8002B401u, error_access = 0x8002B402u, error_param = 0x8002B404u, error_failure = 0x8002B407u,
    error_nodata = 0x8002B40Bu, ret_cancel = 1,
};
static_assert(list_get + list_get_bytes <= fixed_set && fixed_set + fixed_set_bytes <= list_set &&
    list_set + list_set_bytes <= file_get && file_get + file_get_bytes <= file_set && file_set + file_set_bytes <= stat_set &&
    stat_set + stat_set_bytes <= stat_get && stat_get + stat_get_bytes <= frame_bytes, "savedata frame records overlap");
/* Calls the guest function behind `opd` with three arguments on this thread and returns when it has returned. */
static void call_guest(StartupExecution &execution, ppu_context &cpu, uint32_t opd_address, uint32_t frame,
    uint32_t a3, uint32_t a4, uint32_t a5)
{
    const auto *services = execution.ppu.services;
    if (!services || !services->compiled_code || !services->compiled_code->find) stop(StartupStop::MissingServices);
    if (!opd_address || opd_address % 4 || !lv2_sync::readable_range(execution, opd_address, 8)) stop(StartupStop::InvalidMemory);
    const uint32_t entry = lv2_sync::read_u32(execution, opd_address), toc = lv2_sync::read_u32(execution, opd_address + 4);
    ps3_compiled_function target = nullptr;
    if (!entry || entry % 4 || services->compiled_code->find(services->compiled_code->context, entry, &target) != PS3_COMPILED_CODE_OK ||
        !target) stop(StartupStop::DispatchFailure);
    const uint64_t saved_r1 = cpu.gpr[1], saved_r2 = cpu.gpr[2], saved_lr = cpu.lr, saved_ctr = cpu.ctr;
    cpu.gpr[1] = frame; cpu.gpr[2] = toc; cpu.gpr[3] = a3; cpu.gpr[4] = a4; cpu.gpr[5] = a5; cpu.ctr = entry;
    ps3_compiled_dispatch_result dispatched{};
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (!execution.dispatch_stop.stop_requested) stop(StartupStop::MissingServices);
    if (execution.observation) {
        using namespace infamous::diagnostics;
        const DispatchEntry dispatch_entry{cpu.ctr, cpu.lr, cpu.gpr[1], execution.dispatch.depth};
        const auto storage = execution.dispatch.depth >= execution.dispatch.limit
            ? DispatchStorage::ImmediateNonInvoking : DispatchStorage::Pending;
        DispatchTicket ticket;
        if (!execution.observation->try_begin_dispatch(dispatch_entry, storage, ticket)) stop(StartupStop::ObservationCapacity);
        dispatched = ps3_compiled_dispatch(&execution.dispatch, *services->compiled_code, execution.dispatch_stop, &cpu,
            dispatch_entry.captured_ctr);
        execution.observation->finish_dispatch(ticket, execution.dispatch.depth,
            {uint32_t(dispatched.status), dispatched.lookup_attempted, uint32_t(dispatched.lookup_status)});
    } else {
        dispatched = ps3_compiled_dispatch(&execution.dispatch, *services->compiled_code, execution.dispatch_stop, &cpu, entry);
    }
#else
    stop(StartupStop::MissingServices);
#endif
    if (dispatched.status == PS3_COMPILED_DISPATCH_STOP_CANCELLED) stop(StartupStop::Cancelled);
    if (dispatched.status == PS3_COMPILED_DISPATCH_STOP_DEADLINE) stop(StartupStop::Deadline);
    if (dispatched.status != PS3_COMPILED_DISPATCH_OK) stop(StartupStop::DispatchFailure);
    /* A tail-calling callback finishes here, before anything it wrote is read back (see the game-data check). */
    while (g_trampoline_fn) { void (*tail)(void *) = g_trampoline_fn; g_trampoline_fn = nullptr; tail(&cpu); }
    cpu.gpr[1] = saved_r1; cpu.gpr[2] = saved_r2; cpu.lr = saved_lr; cpu.ctr = saved_ctr;
}
static void write_zero(StartupExecution &execution, uint32_t address, uint32_t bytes)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (!memory.zero || memory.zero(memory.context, address, bytes) != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure);
}
static void write_bytes(StartupExecution &execution, uint32_t address, const void *bytes, size_t length)
{
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (length && memory.write(memory.context, address, bytes, length) != PS3_GUEST_MEMORY_OK) stop(StartupStop::ProviderFailure);
}
static void write_u32(StartupExecution &execution, uint32_t address, uint32_t value)
{
    const unsigned char raw[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8), (unsigned char)value};
    write_bytes(execution, address, raw, sizeof raw);
}
}  // namespace savedata_abi

/* The file is inside an extern "C" block here; this namespace uses templates and lambdas. */
extern "C++" {
namespace savedata_native {
using namespace savedata_abi;
namespace sfo = savedata_sfo;

constexpr char save_root[] = "/dev_hdd0/home/00000001/savedata";
constexpr const char *save_root_parents[] = {"/dev_hdd0/home", "/dev_hdd0/home/00000001", save_root};
constexpr char param_sfo_name[] = "PARAM.SFO";
constexpr char trim_suffix[] = ".TRIM";
constexpr size_t path_bytes = 160;

enum : uint32_t {
    cbresult_ok_next = 0, cbresult_ok_last = 1,
    fileop_read = 0, fileop_write = 1, fileop_delete = 2, fileop_write_notrunc = 3,
    recreate_mask = 0xFFFFu, recreate_yes = 2, recreate_yes_reset_owner = 3,
    focus_dirname = 0, focus_listhead = 1, focus_listtail = 2, focus_latest = 3, focus_oldest = 4, focus_newdata = 5,
    sort_subtitle = 1, order_ascent = 1,
    set_buf_bytes = 0x28, set_buf_dir_list_max = 0x00, set_buf_file_list_max = 0x04, set_buf_size = 0x20, set_buf_buffer = 0x24,
    set_list_bytes = 0x0C, set_list_sort_type = 0x00, set_list_sort_order = 0x04, set_list_prefix = 0x08, prefix_bytes = 256,
    list_get_dir_num = 0x00, list_get_dir_list_num = 0x04, list_get_dir_list = 0x08,
    list_set_focus = 0x00, list_set_focus_name = 0x04, list_set_fixed_num = 0x08, list_set_fixed = 0x0C, list_set_new_data = 0x10,
    new_data_bytes = 0x0C, new_data_dir_name = 0x04, fixed_set_dir_name = 0x00,
    dir_list_entry_bytes = 0x30, dir_list_param = 0x20,
    stat_get_free_kb = 0x00, stat_get_is_new = 0x04, stat_get_dir_atime = 0x08, stat_get_dir_mtime = 0x10,
    stat_get_dir_ctime = 0x18, stat_get_dir_name = 0x20, stat_get_param = 0x40, stat_get_size_kb = 0x654,
    stat_get_sys_size_kb = 0x658, stat_get_file_num = 0x65C, stat_get_file_list_num = 0x660, stat_get_file_list = 0x664,
    system_param_bytes = 0x610, system_param_sub_title = 0x80, system_param_detail = 0x100, system_param_attribute = 0x500,
    system_param_list_param = 0x508,
    stat_set_param = 0x00, stat_set_recreate = 0x04,
    file_stat_bytes = 0x38, file_stat_size = 0x08, file_stat_atime = 0x10, file_stat_mtime = 0x18, file_stat_ctime = 0x20,
    file_stat_name = 0x28,
    file_get_exc_size = 0x00,
    file_set_operation = 0x00, file_set_type = 0x08, file_set_secure_id = 0x0C, file_set_name = 0x1C, file_set_offset = 0x20,
    file_set_size = 0x24, file_set_buf_size = 0x28, file_set_buf = 0x2C,
    cellfs_stat_mode = 0x00, cellfs_stat_atime = 0x0C, cellfs_stat_mtime = 0x14, cellfs_stat_ctime = 0x1C, cellfs_stat_size = 0x24,
    cellfs_mode_type_mask = 0170000, cellfs_mode_directory = 0040000,
    hdd_free_kb = 0x00100000u, system_files_kb = 35u, kilobyte = 1024u,
    dir_capacity = 256, file_capacity = 128, io_chunk_bytes = 0x10000, sfo_capacity = 0x4000, fixed_list_scan_max = 2048,
    file_loop_limit = 0x10000,
};

struct DirEntry {
    char name[sfo::dir_name_bytes];
    char list_param[sfo::list_param_bytes];
    char sub_title[sfo::sub_title_bytes];
    int64_t mtime;
};
struct FileEntry {
    char name[sfo::file_name_bytes];
    uint32_t type;
    uint64_t size;
    int64_t atime, mtime, ctime;
};
/* Per-thread working storage, about 140 KiB: too large for a guest thread's host stack, and a stop() inside a guest
 * callback never returns, so nothing here may own a resource that must be released. Allocated once per thread. */
struct Scratch {
    DirEntry dirs[dir_capacity];
    uint32_t dir_count;
    FileEntry files[file_capacity];
    uint32_t file_count;
    sfo::Params params;
    uint8_t sfo[sfo_capacity];
    uint8_t chunk[io_chunk_bytes];
    uint8_t record[std::max<uint32_t>(system_param_bytes, stat_get_bytes)];
};
static Scratch &thread_scratch()
{
    static thread_local Scratch *scratch = nullptr;
    if (!scratch) scratch = static_cast<Scratch *>(std::calloc(1, sizeof(Scratch)));
    if (!scratch) stop(StartupStop::ProviderFailure);
    return *scratch;
}

/* One save-data call: which import, which directory, and the guest pieces its steps need. */
struct Call {
    const char *fn;
    bool is_save;
    uint32_t set_buf, func_stat, func_file, frame;
    char dir[sfo::dir_name_bytes];
    char dir_path[path_bytes];
};

static void put_be32(uint8_t *at, uint32_t value)
{
    at[0] = uint8_t(value >> 24); at[1] = uint8_t(value >> 16); at[2] = uint8_t(value >> 8); at[3] = uint8_t(value);
}
static void put_be64(uint8_t *at, uint64_t value) { put_be32(at, uint32_t(value >> 32)); put_be32(at + 4, uint32_t(value)); }
static uint32_t get_be32(const uint8_t *at) { return uint32_t(at[0]) << 24 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 8 | at[3]; }
static uint64_t get_be64(const uint8_t *at) { return uint64_t(get_be32(at)) << 32 | get_be32(at + 4); }

static void log_call(const Call &call, const char *step, const char *op, const char *file, uint64_t size, int32_t cb_result,
    uint32_t rc)
{
    std::printf("STARTUP_SAVEDATA_CALL fn=%s dir=%s step=%s op=%s file=%s size=%llu cb_result=%d rc=0x%08x\n", call.fn,
        call.dir[0] ? call.dir : "-", step, op, file && file[0] ? file : "-", (unsigned long long)size, int(cb_result), unsigned(rc));
    std::fflush(stdout);
}

/* A NUL-terminated guest string of at most `capacity - 1` bytes. False (and `out` empty) when unreadable or longer. */
static bool read_guest_string(StartupExecution &execution, uint32_t address, char *out, size_t capacity)
{
    std::memset(out, 0, capacity);
    if (!address) return false;
    for (size_t index = 0; index < capacity; ++index) {
        unsigned char byte = 0;
        const uint64_t at = uint64_t(address) + index;
        if (at > UINT32_MAX || !lv2_sync::readable_range(execution, uint32_t(at), 1) ||
            !lv2_sync::read_bytes(execution, uint32_t(at), &byte, 1)) break;
        out[index] = char(byte);
        if (!byte) return true;
    }
    std::memset(out, 0, capacity);
    return false;
}

static bool build_path(char *out, const char *directory, const char *file)
{
    const int used = file ? std::snprintf(out, path_bytes, "%s/%s/%s", save_root, directory, file)
                          : std::snprintf(out, path_bytes, "%s/%s", save_root, directory);
    return used > 0 && size_t(used) < path_bytes;
}

/* ---- the save store: guest paths through the process's fs_service ---------------------------------------- */
#ifdef PS3_NATIVE_STARTUP_FS
static bool storage_available(const StartupExecution &execution) { return execution.fs && execution.fs->active; }
static int32_t storage_stat(StartupExecution &execution, const char *path, bool &is_directory, uint64_t &size, int64_t times[3])
{
    uint8_t record[FS_CELLFS_STAT_BYTES]{};
    int32_t code;
    { FsServiceGuard held(execution.fs->guard); code = fs_stat(&execution.fs->service, path, std::strlen(path), record); }
    if (code != FS_OK) return code;
    is_directory = (get_be32(record + cellfs_stat_mode) & cellfs_mode_type_mask) == cellfs_mode_directory;
    times[0] = int64_t(get_be64(record + cellfs_stat_atime));
    times[1] = int64_t(get_be64(record + cellfs_stat_mtime));
    times[2] = int64_t(get_be64(record + cellfs_stat_ctime));
    size = get_be64(record + cellfs_stat_size);
    return FS_OK;
}
/* Calls visit(name, is_directory) for every entry of `path`. No guest code runs inside. */
template <typename Visit>
static int32_t storage_for_each(StartupExecution &execution, const char *path, Visit visit)
{
    fs_service &service = execution.fs->service;
    uint32_t handle = 0;
    int32_t code;
    { FsServiceGuard held(execution.fs->guard); code = fs_opendir(&service, path, std::strlen(path), &handle); }
    if (code != FS_OK) return code;
    for (;;) {
        uint8_t dirent[FS_CELLFS_DIRENT_BYTES]{}, nread[FS_CELLFS_NREAD_BYTES]{};
        { FsServiceGuard held(execution.fs->guard); code = fs_readdir(&service, handle, dirent, nread); }
        if (code != FS_OK || !nread[FS_CELLFS_NREAD_BYTES - 1]) break;
        char name[FS_DIRENT_NAME_MAX + 1]{};
        std::memcpy(name, dirent + 2, dirent[1]);
        visit(static_cast<const char *>(name), dirent[0] == FS_CELLFS_DTYPE_DIR);
    }
    { FsServiceGuard held(execution.fs->guard); fs_closedir(&service, handle); }
    return code;
}
static int32_t storage_open(StartupExecution &execution, const char *path, uint32_t flags, uint32_t &fd)
{
    FsServiceGuard held(execution.fs->guard);
    return fs_open(&execution.fs->service, path, std::strlen(path), flags, &fd);
}
static void storage_close(StartupExecution &execution, uint32_t fd)
{
    FsServiceGuard held(execution.fs->guard);
    fs_close(&execution.fs->service, fd);
}
static int32_t storage_seek(StartupExecution &execution, uint32_t fd, uint64_t offset)
{
    uint64_t position = 0;
    FsServiceGuard held(execution.fs->guard);
    return fs_lseek(&execution.fs->service, fd, int64_t(offset), FS_SEEK_SET, &position);
}
static int32_t storage_read(StartupExecution &execution, uint32_t fd, uint8_t *buffer, uint32_t length, uint32_t &got)
{
    uint64_t read = 0;
    int32_t code;
    { FsServiceGuard held(execution.fs->guard); code = fs_read(&execution.fs->service, fd, buffer, length, &read); }
    got = uint32_t(read);
    return code;
}
static int32_t storage_write(StartupExecution &execution, uint32_t fd, const uint8_t *buffer, uint32_t length)
{
    uint64_t written = 0;
    int32_t code;
    { FsServiceGuard held(execution.fs->guard); code = fs_write(&execution.fs->service, fd, buffer, length, &written); }
    return code == FS_OK && written != length ? FS_ERROR_EIO : code;
}
static int32_t storage_unlink(StartupExecution &execution, const char *path)
{
    FsServiceGuard held(execution.fs->guard);
    return fs_unlink(&execution.fs->service, path, std::strlen(path));
}
static int32_t storage_rename(StartupExecution &execution, const char *from, const char *to)
{
    FsServiceGuard held(execution.fs->guard);
    return fs_rename(&execution.fs->service, from, std::strlen(from), to, std::strlen(to));
}
static int32_t storage_make_directory(StartupExecution &execution, const char *path)
{
    int32_t code;
    { FsServiceGuard held(execution.fs->guard); code = fs_mkdir(&execution.fs->service, path, std::strlen(path), 0700); }
    return code == FS_ERROR_EEXIST ? FS_OK : code;
}
#else
static bool storage_available(const StartupExecution &) { return false; }
static int32_t storage_stat(StartupExecution &, const char *, bool &, uint64_t &, int64_t *) { return FS_ERROR_ENOTSUP; }
template <typename Visit> static int32_t storage_for_each(StartupExecution &, const char *, Visit) { return FS_ERROR_ENOTSUP; }
static int32_t storage_open(StartupExecution &, const char *, uint32_t, uint32_t &) { return FS_ERROR_ENOTSUP; }
static void storage_close(StartupExecution &, uint32_t) {}
static int32_t storage_seek(StartupExecution &, uint32_t, uint64_t) { return FS_ERROR_ENOTSUP; }
static int32_t storage_read(StartupExecution &, uint32_t, uint8_t *, uint32_t, uint32_t &) { return FS_ERROR_ENOTSUP; }
static int32_t storage_write(StartupExecution &, uint32_t, const uint8_t *, uint32_t) { return FS_ERROR_ENOTSUP; }
static int32_t storage_unlink(StartupExecution &, const char *) { return FS_ERROR_ENOTSUP; }
static int32_t storage_rename(StartupExecution &, const char *, const char *) { return FS_ERROR_ENOTSUP; }
static int32_t storage_make_directory(StartupExecution &, const char *) { return FS_ERROR_ENOTSUP; }
#endif

/* Reads a whole small file (PARAM.SFO) into `buffer`. */
static int32_t storage_read_whole(StartupExecution &execution, const char *path, uint8_t *buffer, uint32_t capacity, uint32_t &size)
{
    uint32_t fd = 0;
    size = 0;
    int32_t code = storage_open(execution, path, FS_O_RDONLY, fd);
    if (code != FS_OK) return code;
    code = storage_read(execution, fd, buffer, capacity, size);
    storage_close(execution, fd);
    return code;
}
static int32_t storage_write_whole(StartupExecution &execution, const char *path, const uint8_t *buffer, uint32_t size)
{
    uint32_t fd = 0;
    int32_t code = storage_open(execution, path, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, fd);
    if (code != FS_OK) return code;
    code = storage_write(execution, fd, buffer, size);
    storage_close(execution, fd);
    return code;
}

/* A directory is a save only when its PARAM.SFO reads back. */
static bool load_params(StartupExecution &execution, const char *dir_path, Scratch &scratch, sfo::Params &params)
{
    char path[path_bytes];
    uint32_t size = 0;
    if (std::snprintf(path, sizeof path, "%s/%s", dir_path, param_sfo_name) >= int(sizeof path)) return false;
    if (storage_read_whole(execution, path, scratch.sfo, sizeof scratch.sfo, size) != FS_OK) return false;
    return sfo::decode(scratch.sfo, size, params);
}
static int32_t store_params(StartupExecution &execution, const char *dir_path, Scratch &scratch)
{
    char path[path_bytes];
    if (std::snprintf(path, sizeof path, "%s/%s", dir_path, param_sfo_name) >= int(sizeof path)) return FS_ERROR_ENAMETOOLONG;
    const size_t size = sfo::encode(scratch.params, scratch.sfo, sizeof scratch.sfo);
    if (!size) return FS_ERROR_EIO;
    return storage_write_whole(execution, path, scratch.sfo, uint32_t(size));
}

/* Every save directory whose name matches `prefixes`, with its list parameter, subtitle and time, sorted. */
static void list_save_dirs(StartupExecution &execution, const char *prefixes, uint32_t sort_type, uint32_t sort_order,
    Scratch &scratch)
{
    scratch.dir_count = 0;
    if (!storage_available(execution)) return;
    storage_for_each(execution, save_root, [&](const char *name, bool is_directory) {
        if (!is_directory || scratch.dir_count >= dir_capacity || !sfo::dir_name_valid(name) || !sfo::prefix_matches(prefixes, name))
            return;
        DirEntry &entry = scratch.dirs[scratch.dir_count++];
        std::memset(&entry, 0, sizeof entry);
        sfo::copy_bounded(entry.name, sizeof entry.name, name, sizeof entry.name);
    });
    uint32_t kept = 0;
    for (uint32_t index = 0; index < scratch.dir_count; ++index) {
        DirEntry entry = scratch.dirs[index];
        char dir_path[path_bytes];
        bool is_directory = false;
        uint64_t size = 0;
        int64_t times[3]{};
        if (!build_path(dir_path, entry.name, nullptr) || !load_params(execution, dir_path, scratch, scratch.params)) continue;
        if (storage_stat(execution, dir_path, is_directory, size, times) == FS_OK) entry.mtime = times[1];
        sfo::copy_bounded(entry.list_param, sizeof entry.list_param, scratch.params.list_param, sizeof scratch.params.list_param);
        sfo::copy_bounded(entry.sub_title, sizeof entry.sub_title, scratch.params.sub_title, sizeof scratch.params.sub_title);
        scratch.dirs[kept++] = entry;
    }
    scratch.dir_count = kept;
    const bool ascending = sort_order == order_ascent;
    std::sort(scratch.dirs, scratch.dirs + scratch.dir_count, [&](const DirEntry &a, const DirEntry &b) {
        const int order = sort_type == sort_subtitle ? std::strcmp(a.sub_title, b.sub_title)
                                                     : (a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : std::strcmp(a.name, b.name));
        return ascending ? order < 0 : order > 0;
    });
}

/* The regular files of one save directory (PARAM.SFO and PARAM.PFD excluded), typed from its PARAM.SFO. */
static void list_save_files(StartupExecution &execution, const char *dir_path, Scratch &scratch)
{
    scratch.file_count = 0;
    storage_for_each(execution, dir_path, [&](const char *name, bool is_directory) {
        if (is_directory || scratch.file_count >= file_capacity || !sfo::file_name_valid(name)) return;
        FileEntry &entry = scratch.files[scratch.file_count++];
        std::memset(&entry, 0, sizeof entry);
        sfo::copy_bounded(entry.name, sizeof entry.name, name, sizeof entry.name);
    });
    for (uint32_t index = 0; index < scratch.file_count; ++index) {
        FileEntry &entry = scratch.files[index];
        char path[path_bytes];
        bool is_directory = false;
        int64_t times[3]{};
        entry.type = sfo::file_type_of(scratch.params, entry.name);
        if (std::snprintf(path, sizeof path, "%s/%s", dir_path, entry.name) < int(sizeof path) &&
            storage_stat(execution, path, is_directory, entry.size, times) == FS_OK) {
            entry.atime = times[0]; entry.mtime = times[1]; entry.ctime = times[2];
        }
    }
}

/* Writes the CellSaveDataDirList entries into setBuf->buf, bounded by dirListMax and bufSize. Returns the count. */
static uint32_t write_dir_list(StartupExecution &execution, uint32_t set_buf, const Scratch &scratch)
{
    const uint32_t buffer = lv2_sync::read_u32(execution, set_buf + set_buf_buffer);
    uint32_t listed = scratch.dir_count;
    listed = std::min(listed, lv2_sync::read_u32(execution, set_buf + set_buf_dir_list_max));
    listed = std::min(listed, lv2_sync::read_u32(execution, set_buf + set_buf_size) / uint32_t(dir_list_entry_bytes));
    if (!buffer || !listed || !writable(execution, buffer, uint64_t(listed) * dir_list_entry_bytes)) return 0;
    for (uint32_t index = 0; index < listed; ++index) {
        uint8_t entry[dir_list_entry_bytes]{};
        std::memcpy(entry, scratch.dirs[index].name, sizeof scratch.dirs[index].name);
        std::memcpy(entry + dir_list_param, scratch.dirs[index].list_param, sizeof scratch.dirs[index].list_param);
        write_bytes(execution, buffer + index * dir_list_entry_bytes, entry, sizeof entry);
    }
    return listed;
}
static void write_list_get(StartupExecution &execution, uint32_t frame, uint32_t set_buf, const Scratch &scratch, uint32_t listed)
{
    write_zero(execution, frame + list_get, list_get_bytes);
    write_u32(execution, frame + list_get + list_get_dir_num, scratch.dir_count);
    write_u32(execution, frame + list_get + list_get_dir_list_num, listed);
    write_u32(execution, frame + list_get + list_get_dir_list, lv2_sync::read_u32(execution, set_buf + set_buf_buffer));
}

/* Reads setList: the prefixes (empty = all) and the sort order. A missing setList lists everything. */
static void read_set_list(StartupExecution &execution, uint32_t set_list, char prefixes[prefix_bytes], uint32_t &sort_type,
    uint32_t &sort_order)
{
    std::memset(prefixes, 0, prefix_bytes);
    sort_type = sort_order = 0;
    if (!set_list || !lv2_sync::readable_range(execution, set_list, set_list_bytes)) return;
    sort_type = lv2_sync::read_u32(execution, set_list + set_list_sort_type);
    sort_order = lv2_sync::read_u32(execution, set_list + set_list_sort_order);
    read_guest_string(execution, lv2_sync::read_u32(execution, set_list + set_list_prefix), prefixes, prefix_bytes);
}

/* Builds the frame below the caller's stack pointer; the CBResult's userdata stays there for every callback. */
static uint32_t open_frame(StartupExecution &execution, ppu_context &cpu, uint32_t userdata)
{
    const uint64_t sp = cpu.gpr[1];
    if (sp < frame_bytes || sp > UINT32_MAX) stop(StartupStop::InvalidMemory);
    const uint32_t frame = (uint32_t(sp) - frame_bytes) & ~15u;
    if (!writable(execution, frame, frame_bytes)) stop(StartupStop::InvalidMemory);
    check_boundary(BoundaryBudget::Poll);
    write_zero(execution, frame, frame_bytes);
    write_u32(execution, frame, uint32_t(sp >> 32)); write_u32(execution, frame + 4, uint32_t(sp));   /* back chain */
    write_u32(execution, frame + cb_result + cb_result_userdata, userdata);
    return frame;
}
/* Runs one callback(cbResult, get, set) with a fresh verdict (OK_NEXT) and returns the game's verdict. The userdata
 * word is left as the previous callback left it. */
static int32_t run_callback(StartupExecution &execution, ppu_context &cpu, uint32_t frame, uint32_t opd, uint32_t get, uint32_t set)
{
    write_zero(execution, frame + cb_result, cb_result_userdata);
    call_guest(execution, cpu, opd, frame, frame + cb_result, frame + get, frame + set);
    check_boundary(BoundaryBudget::Poll);
    return int32_t(lv2_sync::read_u32(execution, frame + cb_result));
}

/* The directory the system dialog would have opened on, from what funcList put in listSet. No dialog exists here, so
 * the default choice is taken: focusPosition first, then the first fixed entry, then (a save) the new-data entry. */
static bool select_from_list_set(StartupExecution &execution, uint32_t frame, bool is_save, const Scratch &scratch,
    char selected[sfo::dir_name_bytes])
{
    const uint32_t set = frame + list_set;
    const uint32_t focus = lv2_sync::read_u32(execution, set + list_set_focus);
    const uint32_t fixed = lv2_sync::read_u32(execution, set + list_set_fixed);
    const uint32_t new_data = lv2_sync::read_u32(execution, set + list_set_new_data);
    uint32_t fixed_count = std::min(lv2_sync::read_u32(execution, set + list_set_fixed_num), uint32_t(fixed_list_scan_max));
    if (!fixed || !lv2_sync::readable_range(execution, fixed, uint64_t(fixed_count) * dir_list_entry_bytes)) fixed_count = 0;
    char new_name[sfo::dir_name_bytes]{}, focus_name[sfo::dir_name_bytes]{};
    char first[sfo::dir_name_bytes]{}, last[sfo::dir_name_bytes]{}, latest[sfo::dir_name_bytes]{}, oldest[sfo::dir_name_bytes]{};
    const bool has_new = is_save && new_data && lv2_sync::readable_range(execution, new_data, new_data_bytes) &&
        read_guest_string(execution, lv2_sync::read_u32(execution, new_data + new_data_dir_name), new_name, sizeof new_name) &&
        sfo::dir_name_valid(new_name);
    const bool has_focus_name = read_guest_string(execution, lv2_sync::read_u32(execution, set + list_set_focus_name), focus_name,
        sizeof focus_name) && sfo::dir_name_valid(focus_name);
    int64_t latest_time = INT64_MIN, oldest_time = INT64_MAX;
    bool focus_listed = false;
    for (uint32_t index = 0; index < fixed_count; ++index) {
        char name[sfo::dir_name_bytes]{};
        lv2_sync::read_bytes(execution, fixed + index * dir_list_entry_bytes, name, sizeof name - 1);
        if (!sfo::dir_name_valid(name)) continue;
        if (!first[0]) std::memcpy(first, name, sizeof name);
        std::memcpy(last, name, sizeof name);
        if (has_focus_name && !std::strcmp(name, focus_name)) focus_listed = true;
        for (uint32_t at = 0; at < scratch.dir_count; ++at) {
            if (std::strcmp(scratch.dirs[at].name, name)) continue;
            if (scratch.dirs[at].mtime > latest_time) { latest_time = scratch.dirs[at].mtime; std::memcpy(latest, name, sizeof name); }
            if (scratch.dirs[at].mtime < oldest_time) { oldest_time = scratch.dirs[at].mtime; std::memcpy(oldest, name, sizeof name); }
        }
    }
    const char *choice = nullptr;
    if (focus == focus_newdata && has_new) choice = new_name;
    else if (focus == focus_dirname && has_focus_name && (focus_listed || (has_new && !std::strcmp(focus_name, new_name)))) choice = focus_name;
    else if (focus == focus_listhead && first[0]) choice = first;
    else if (focus == focus_listtail && last[0]) choice = last;
    else if (focus == focus_latest && latest[0]) choice = latest;
    else if (focus == focus_oldest && oldest[0]) choice = oldest;
    else if (first[0]) choice = first;
    else if (has_new) choice = new_name;
    if (!choice) return false;
    sfo::copy_bounded(selected, sfo::dir_name_bytes, choice, sfo::dir_name_bytes);
    return true;
}

/* ---- funcFile: one operation ------------------------------------------------------------------------------ */
/* Copies a file range into guest memory. `done` is the byte count read (less than `length` at end of file). */
static int32_t copy_file_to_guest(StartupExecution &execution, Scratch &scratch, const char *path, uint32_t offset,
    uint32_t guest, uint32_t length, uint32_t &done)
{
    uint32_t fd = 0;
    done = 0;
    int32_t code = storage_open(execution, path, FS_O_RDONLY, fd);
    if (code != FS_OK) return code;
    code = storage_seek(execution, fd, offset);
    while (code == FS_OK && done < length) {
        const uint32_t want = std::min(length - done, uint32_t(io_chunk_bytes));
        uint32_t got = 0;
        code = storage_read(execution, fd, scratch.chunk, want, got);
        if (code != FS_OK || !got) break;
        const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
        if (memory.write(memory.context, guest + done, scratch.chunk, got) != PS3_GUEST_MEMORY_OK) {
            storage_close(execution, fd);
            stop(StartupStop::ProviderFailure);
        }
        done += got;
        if (got < want) break;
    }
    storage_close(execution, fd);
    return code;
}
/* Copies the first `end` bytes of `path` into a fresh file and puts it in place of `path` (the truncate a WRITE at a
 * non-zero offset needs; fs_service has no ftruncate). */
static int32_t trim_file(StartupExecution &execution, Scratch &scratch, const char *path, uint64_t end)
{
    char temporary[path_bytes];
    if (std::snprintf(temporary, sizeof temporary, "%s%s", path, trim_suffix) >= int(sizeof temporary)) return FS_ERROR_ENAMETOOLONG;
    uint32_t source = 0, target = 0;
    int32_t code = storage_open(execution, path, FS_O_RDONLY, source);
    if (code != FS_OK) return code;
    code = storage_open(execution, temporary, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC, target);
    if (code != FS_OK) { storage_close(execution, source); return code; }
    for (uint64_t copied = 0; code == FS_OK && copied < end;) {
        uint32_t got = 0;
        code = storage_read(execution, source, scratch.chunk, uint32_t(std::min<uint64_t>(end - copied, io_chunk_bytes)), got);
        if (code != FS_OK || !got) break;
        code = storage_write(execution, target, scratch.chunk, got);
        copied += got;
    }
    storage_close(execution, source);
    storage_close(execution, target);
    if (code == FS_OK) code = storage_unlink(execution, path);
    if (code == FS_OK) code = storage_rename(execution, temporary, path);
    if (code != FS_OK) storage_unlink(execution, temporary);
    return code;
}
/* Writes guest memory into a file at `offset`. WRITE ends the file at offset + length; WRITE_NOTRUNC keeps the rest. */
static int32_t copy_guest_to_file(StartupExecution &execution, Scratch &scratch, const char *path, bool truncate,
    uint32_t offset, uint32_t guest, uint32_t length, uint32_t &done)
{
    uint32_t fd = 0;
    done = 0;
    int32_t code = storage_open(execution, path, FS_O_WRONLY | FS_O_CREAT | (truncate && !offset ? FS_O_TRUNC : 0u), fd);
    if (code != FS_OK) return code;
    code = storage_seek(execution, fd, offset);
    while (code == FS_OK && done < length) {
        const uint32_t chunk = std::min(length - done, uint32_t(io_chunk_bytes));
        if (!lv2_sync::read_bytes(execution, guest + done, scratch.chunk, chunk)) {
            storage_close(execution, fd);
            stop(StartupStop::ProviderFailure);
        }
        code = storage_write(execution, fd, scratch.chunk, chunk);
        if (code == FS_OK) done += chunk;
    }
    storage_close(execution, fd);
    if (code != FS_OK || !truncate || !offset) return code;
    bool is_directory = false;
    uint64_t size = 0;
    int64_t times[3]{};
    if (storage_stat(execution, path, is_directory, size, times) == FS_OK && size > uint64_t(offset) + length)
        code = trim_file(execution, scratch, path, uint64_t(offset) + length);
    return code;
}

static const char *fileop_name(uint32_t operation)
{
    switch (operation) {
    case fileop_read: return "read";
    case fileop_write: return "write";
    case fileop_delete: return "delete";
    case fileop_write_notrunc: return "write_notrunc";
    default: return "unknown";
    }
}

/* Performs the operation funcFile put in fileSet. Returns 0 or the CELL_SAVEDATA_ERROR_* that ends the call. */
static uint32_t perform_file_operation(StartupExecution &execution, Call &call, Scratch &scratch, uint32_t &exc_size,
    int32_t cb, uint32_t &file_ops)
{
    const uint32_t set = call.frame + file_set;
    const uint32_t operation = lv2_sync::read_u32(execution, set + file_set_operation);
    const uint32_t type = lv2_sync::read_u32(execution, set + file_set_type);
    const uint32_t offset = lv2_sync::read_u32(execution, set + file_set_offset);
    const uint32_t size = lv2_sync::read_u32(execution, set + file_set_size);
    const uint32_t buffer_size = lv2_sync::read_u32(execution, set + file_set_buf_size);
    const uint32_t buffer = lv2_sync::read_u32(execution, set + file_set_buf);
    const uint32_t length = std::min(size, buffer_size);
    char name[sfo::file_name_bytes]{};
    const char *fixed_name = sfo::content_file_name(type);
    exc_size = 0;
    auto fail = [&](uint32_t rc) { log_call(call, "file", fileop_name(operation), name, length, cb, rc); return rc; };
    if (fixed_name) sfo::copy_bounded(name, sizeof name, fixed_name, sizeof name);
    else if (type > sfo::file_type_normal) return fail(error_param);
    else if (!read_guest_string(execution, lv2_sync::read_u32(execution, set + file_set_name), name, sizeof name) ||
             !sfo::file_name_valid(name)) return fail(error_param);
    char path[path_bytes];
    if (std::snprintf(path, sizeof path, "%s/%s", call.dir_path, name) >= int(sizeof path)) return fail(error_param);
    /* inFAMOUS writes ICON0 with a NULL buffer and size 0: an empty request, not an error (cellSaveData.c, 2026-10-06). */
    if (operation != fileop_delete && length && !buffer) return fail(error_param);
    int32_t code = FS_OK;
    uint32_t done = 0;
    switch (operation) {
    case fileop_read:
        if (length && !writable(execution, buffer, length)) return fail(error_param);
        code = copy_file_to_guest(execution, scratch, path, offset, buffer, length, done);
        break;
    case fileop_write:
    case fileop_write_notrunc: {
        if (length && !lv2_sync::readable_range(execution, buffer, length)) return fail(error_param);
        code = copy_guest_to_file(execution, scratch, path, operation == fileop_write, offset, buffer, length, done);
        if (code != FS_OK) break;
        if (type == sfo::file_type_secure) {
            uint8_t id[sfo::secure_id_bytes]{};
            lv2_sync::read_bytes(execution, set + file_set_secure_id, id, sizeof id);
            if (!sfo::secure_set(scratch.params, name, id)) return fail(error_failure);
        } else if (type == sfo::file_type_normal) {
            sfo::secure_remove(scratch.params, name);
        }
        break;
    }
    case fileop_delete:
        code = storage_unlink(execution, path);
        if (code == FS_ERROR_ENOENT) code = FS_OK;
        sfo::secure_remove(scratch.params, name);
        break;
    default:
        return fail(error_param);
    }
    if (code != FS_OK) return fail(error_access);
    exc_size = done;
    ++file_ops;
    log_call(call, "file", fileop_name(operation), name, done, cb, 0);
    return 0;
}

/* funcFile until the game answers OK_LAST (or an error). */
static uint32_t run_file_loop(StartupExecution &execution, ppu_context &cpu, Call &call, Scratch &scratch)
{
    if (!call.func_file) { log_call(call, "file", "-", "-", 0, 0, error_param); return error_param; }
    uint32_t exc_size = 0, file_ops = 0;
    for (uint32_t round = 0; round < file_loop_limit; ++round) {
        write_zero(execution, call.frame + file_get, file_get_bytes);
        write_zero(execution, call.frame + file_set, file_set_bytes);
        write_u32(execution, call.frame + file_get + file_get_exc_size, exc_size);
        const int32_t cb = run_callback(execution, cpu, call.frame, call.func_file, file_get, file_set);
        if (cb < 0) { log_call(call, "file", "-", "-", file_ops, cb, error_cbresult); return error_cbresult; }
        if (cb != cbresult_ok_next) { log_call(call, "file", "last", "-", file_ops, cb, 0); return 0; }
        const uint32_t rc = perform_file_operation(execution, call, scratch, exc_size, cb, file_ops);
        if (rc) return rc;
    }
    log_call(call, "file", "limit", "-", file_ops, 0, error_failure);
    return error_failure;
}

/* Fills statGet in scratch.record and the file list in setBuf->buf. */
static void write_stat_get(StartupExecution &execution, Call &call, Scratch &scratch, bool is_new, bool has_params,
    const int64_t times[3])
{
    uint8_t *get = scratch.record;
    std::memset(get, 0, stat_get_bytes);
    put_be32(get + stat_get_free_kb, hdd_free_kb);
    put_be32(get + stat_get_is_new, is_new ? 1u : 0u);
    /* A load of a directory that does not exist keeps Decision 83's all-zero record. */
    if (call.is_save || !is_new) std::memcpy(get + stat_get_dir_name, call.dir, sizeof call.dir);
    uint32_t listed = 0;
    if (!is_new) {
        put_be64(get + stat_get_dir_atime, uint64_t(times[0]));
        put_be64(get + stat_get_dir_mtime, uint64_t(times[1]));
        put_be64(get + stat_get_dir_ctime, uint64_t(times[2]));
        uint64_t size_kb = system_files_kb;
        for (uint32_t index = 0; index < scratch.file_count; ++index) size_kb += (scratch.files[index].size + kilobyte - 1) / kilobyte;
        put_be32(get + stat_get_size_kb, uint32_t(std::min<uint64_t>(size_kb, INT32_MAX)));
        put_be32(get + stat_get_sys_size_kb, system_files_kb);
        put_be32(get + stat_get_file_num, scratch.file_count);
        const uint32_t buffer = lv2_sync::read_u32(execution, call.set_buf + set_buf_buffer);
        listed = std::min(scratch.file_count, lv2_sync::read_u32(execution, call.set_buf + set_buf_file_list_max));
        listed = std::min(listed, lv2_sync::read_u32(execution, call.set_buf + set_buf_size) / uint32_t(file_stat_bytes));
        if (!buffer || !writable(execution, buffer, uint64_t(listed) * file_stat_bytes)) listed = 0;
        for (uint32_t index = 0; index < listed; ++index) {
            const FileEntry &file = scratch.files[index];
            uint8_t entry[file_stat_bytes]{};
            put_be32(entry, file.type);
            put_be64(entry + file_stat_size, file.size);
            put_be64(entry + file_stat_atime, uint64_t(file.atime));
            put_be64(entry + file_stat_mtime, uint64_t(file.mtime));
            put_be64(entry + file_stat_ctime, uint64_t(file.ctime));
            std::memcpy(entry + file_stat_name, file.name, sizeof file.name);
            write_bytes(execution, buffer + index * file_stat_bytes, entry, sizeof entry);
        }
        put_be32(get + stat_get_file_list_num, listed);
        put_be32(get + stat_get_file_list, listed ? buffer : 0u);
    }
    if (has_params) {
        uint8_t *param = get + stat_get_param;
        std::memcpy(param, scratch.params.title, sizeof scratch.params.title);
        std::memcpy(param + system_param_sub_title, scratch.params.sub_title, sizeof scratch.params.sub_title);
        std::memcpy(param + system_param_detail, scratch.params.detail, sizeof scratch.params.detail);
        put_be32(param + system_param_attribute, scratch.params.attribute);
        std::memcpy(param + system_param_list_param, scratch.params.list_param, sizeof scratch.params.list_param);
    }
    write_bytes(execution, call.frame + stat_get, get, stat_get_bytes);
}

/* statSet->setParam on a save: the new title, subtitle, detail, attribute and list parameter. */
static bool apply_set_param(StartupExecution &execution, uint32_t set_param, Scratch &scratch)
{
    if (!lv2_sync::readable_range(execution, set_param, system_param_bytes) ||
        !lv2_sync::read_bytes(execution, set_param, scratch.record, system_param_bytes)) return false;
    const char *raw = reinterpret_cast<const char *>(scratch.record);
    sfo::copy_bounded(scratch.params.title, sizeof scratch.params.title, raw, sfo::title_bytes);
    sfo::copy_bounded(scratch.params.sub_title, sizeof scratch.params.sub_title, raw + system_param_sub_title, sfo::sub_title_bytes);
    sfo::copy_bounded(scratch.params.detail, sizeof scratch.params.detail, raw + system_param_detail, sfo::detail_bytes);
    scratch.params.attribute = get_be32(scratch.record + system_param_attribute);
    sfo::copy_bounded(scratch.params.list_param, sizeof scratch.params.list_param, raw + system_param_list_param, sfo::list_param_bytes);
    return true;
}

static int32_t create_save_directory(StartupExecution &execution, const char *dir_path)
{
    for (const char *parent : save_root_parents) {
        const int32_t code = storage_make_directory(execution, parent);
        if (code != FS_OK) return code;
    }
    return storage_make_directory(execution, dir_path);
}
static void empty_save_directory(StartupExecution &execution, Scratch &scratch, const char *dir_path)
{
    for (uint32_t index = 0; index < scratch.file_count; ++index) {
        char path[path_bytes];
        if (std::snprintf(path, sizeof path, "%s/%s", dir_path, scratch.files[index].name) < int(sizeof path))
            storage_unlink(execution, path);
    }
    scratch.file_count = 0;
    scratch.params.secure_count = 0;
}

/* funcStat, then (unless the game ends there) the funcFile loop, then a save's PARAM.SFO. `record` is the Decision 83
 * load record, or null on a save. Returns the import's result code. */
static uint32_t run_stat_and_files(StartupExecution &execution, ppu_context &cpu, Call &call, StartupSaveDataRecord *record)
{
    Scratch &scratch = thread_scratch();
    const bool valid_name = sfo::dir_name_valid(call.dir) && build_path(call.dir_path, call.dir, nullptr);
    if (call.is_save && !valid_name) { log_call(call, "stat", "-", "-", 0, 0, error_param); return error_param; }
    if (call.is_save && !storage_available(execution)) { log_call(call, "stat", "-", "-", 0, 0, error_access); return error_access; }
    bool exists = false, has_params = false, is_directory = false;
    uint64_t size = 0;
    int64_t times[3]{};
    std::memset(&scratch.params, 0, sizeof scratch.params);
    scratch.file_count = 0;
    if (valid_name && storage_available(execution)) {
        exists = storage_stat(execution, call.dir_path, is_directory, size, times) == FS_OK && is_directory;
        has_params = exists && load_params(execution, call.dir_path, scratch, scratch.params);
        if (!has_params) std::memset(&scratch.params, 0, sizeof scratch.params);
    }
    const bool is_new = !has_params;
    if (!is_new) list_save_files(execution, call.dir_path, scratch);
    write_stat_get(execution, call, scratch, is_new, has_params, times);
    write_zero(execution, call.frame + stat_set, stat_set_bytes);
    const int32_t cb = run_callback(execution, cpu, call.frame, call.func_stat, stat_get, stat_set);
    if (record) { record->stat_called = 1; record->stat_result = cb; }
    const char *stat_op = is_new ? "new" : "existing";
    if (cb < 0) { log_call(call, "stat", stat_op, "-", scratch.file_count, cb, error_cbresult); return error_cbresult; }
    if (!call.is_save) {
        if (is_new) {
            const uint32_t rc = cb == cbresult_ok_last ? 0u : uint32_t(error_nodata);
            log_call(call, "stat", stat_op, "-", 0, cb, rc);
            return rc;
        }
        log_call(call, "stat", stat_op, "-", scratch.file_count, cb, 0);
        return cb == cbresult_ok_next ? run_file_loop(execution, cpu, call, scratch) : 0u;
    }
    const uint32_t set_param = lv2_sync::read_u32(execution, call.frame + stat_set + stat_set_param);
    const uint32_t recreate = lv2_sync::read_u32(execution, call.frame + stat_set + stat_set_recreate) & recreate_mask;
    if (set_param ? !apply_set_param(execution, set_param, scratch) : is_new) {
        log_call(call, "stat", stat_op, "setParam", 0, cb, error_param);
        return error_param;
    }
    sfo::copy_bounded(scratch.params.directory, sizeof scratch.params.directory, call.dir, sizeof call.dir);
    if (create_save_directory(execution, call.dir_path) != FS_OK) {
        log_call(call, "stat", stat_op, "mkdir", 0, cb, error_access);
        return error_access;
    }
    if (!is_new && (recreate == recreate_yes || recreate == recreate_yes_reset_owner)) empty_save_directory(execution, scratch, call.dir_path);
    log_call(call, "stat", stat_op, "-", recreate, cb, 0);
    const uint32_t rc = cb == cbresult_ok_next ? run_file_loop(execution, cpu, call, scratch) : 0u;
    if (rc) return rc;
    if (store_params(execution, call.dir_path, scratch) != FS_OK) {
        log_call(call, "commit", "write", param_sfo_name, 0, cb, error_access);
        return error_access;
    }
    log_call(call, "commit", "write", param_sfo_name, scratch.params.secure_count, cb, 0);
    return 0;
}

static Call make_call(const char *fn, bool is_save, uint32_t set_buf, uint32_t func_stat, uint32_t func_file)
{
    Call call{};
    call.fn = fn;
    call.is_save = is_save;
    call.set_buf = set_buf;
    call.func_stat = func_stat;
    call.func_file = func_file;
    return call;
}
static void finish_call(ppu_context &cpu, const Call &call, uint32_t code)
{
    log_call(call, "done", "-", "-", 0, 0, code);
    cpu.gpr[3] = uint64_t(int64_t(int32_t(code)));
}

/* cellSaveDataListLoad2 / cellSaveDataListSave2(version, setList, setBuf, funcList, funcStat, funcFile, container,
 * userdata): funcList gets the list, the default entry is picked, then stat and files. */
static void dispatch_list(StartupExecution &execution, ppu_context &cpu, bool is_save)
{
    StartupSaveDataRecord &record = g_savedata_auto_load;
    if (is_save) ++record.list_save_calls; else ++record.list_load_calls;
    const uint32_t set_list = uint32_t(cpu.gpr[4]), set_buf = uint32_t(cpu.gpr[5]), func_list = uint32_t(cpu.gpr[6]);
    const uint32_t func_stat = uint32_t(cpu.gpr[7]), func_file = uint32_t(cpu.gpr[8]), userdata = uint32_t(cpu.gpr[10]);
    Call call = make_call(is_save ? "cellSaveDataListSave2" : "cellSaveDataListLoad2", is_save, set_buf, func_stat, func_file);
    auto finish = [&](uint32_t code) {
        if (is_save) record.save_result = code; else record.list_load_result = code;
        finish_call(cpu, call, code);
    };
    if (!set_buf || !func_list || !lv2_sync::readable_range(execution, set_buf, set_buf_bytes) ||
        (is_save && (!func_stat || !func_file))) { finish(error_param); return; }
    Scratch &scratch = thread_scratch();
    char prefixes[prefix_bytes];
    uint32_t sort_type = 0, sort_order = 0;
    read_set_list(execution, set_list, prefixes, sort_type, sort_order);
    call.frame = open_frame(execution, cpu, userdata);
    list_save_dirs(execution, prefixes, sort_type, sort_order, scratch);
    const uint32_t listed = write_dir_list(execution, set_buf, scratch);
    write_list_get(execution, call.frame, set_buf, scratch, listed);
    const int32_t verdict = run_callback(execution, cpu, call.frame, func_list, list_get, list_set);
    if (!is_save) record.list_load_verdict = verdict;
    /* Decision 83: negative -> CBRESULT, OK_LAST -> CELL_OK, anything else goes on to the pick, and nothing to pick
     * is the player backing out (CELL_SAVEDATA_RET_CANCEL). */
    const uint32_t list_rc = verdict < 0 ? uint32_t(error_cbresult) : 0u;
    log_call(call, "list", prefixes[0] ? prefixes : "-", "-", scratch.dir_count, verdict, list_rc);
    if (verdict < 0) { finish(error_cbresult); return; }
    if (verdict == cbresult_ok_last) { finish(0); return; }
    if (!select_from_list_set(execution, call.frame, is_save, scratch, call.dir)) { finish(ret_cancel); return; }
    if (!func_stat) { finish(error_param); return; }
    finish(run_stat_and_files(execution, cpu, call, is_save ? nullptr : &record));
}
}  // namespace savedata_native
}  // extern "C++"

/* cellSaveDataListAutoLoad(version, errDialog, setList, setBuf, funcFixed, funcStat, funcFile, container, userdata),
 * NID 0x21425307. */
static void dispatch_savedata_list_auto_load(StartupExecution &execution, ppu_context &cpu)
{
    using namespace savedata_native;
    StartupSaveDataRecord &record = g_savedata_auto_load;
    ++record.calls;
    const uint32_t err_dialog = uint32_t(cpu.gpr[4]), set_list = uint32_t(cpu.gpr[5]), set_buf = uint32_t(cpu.gpr[6]);
    const uint32_t func_fixed = uint32_t(cpu.gpr[7]), func_stat = uint32_t(cpu.gpr[8]), func_file = uint32_t(cpu.gpr[9]);
    record.version = uint32_t(cpu.gpr[3]); record.func_fixed = func_fixed; record.func_stat = func_stat;
    Call call = make_call("cellSaveDataListAutoLoad", false, set_buf, func_stat, func_file);
    auto finish = [&](uint32_t code) { record.result = code; ++record.returns; finish_call(cpu, call, code); };
    if (err_dialog > 2 || !set_buf || !func_fixed || !func_stat || !lv2_sync::readable_range(execution, set_buf, set_buf_bytes)) {
        finish(error_param);
        return;
    }
    /* The ninth argument (userdata) is the first stack parameter: the doubleword at r1 + 0x70. */
    const uint64_t sp = cpu.gpr[1];
    const uint32_t userdata = lv2_sync::readable_range(execution, uint32_t(sp) + 0x70, 8) ? uint32_t(lv2_sync::read_u64(execution, uint32_t(sp) + 0x70)) : 0;
    Scratch &scratch = thread_scratch();
    char prefixes[prefix_bytes];
    uint32_t sort_type = 0, sort_order = 0;
    read_set_list(execution, set_list, prefixes, sort_type, sort_order);
    call.frame = open_frame(execution, cpu, userdata);
    /* 1. funcFixed with the existing saves (an empty list on a first start). */
    list_save_dirs(execution, prefixes, sort_type, sort_order, scratch);
    const uint32_t listed = write_dir_list(execution, set_buf, scratch);
    write_list_get(execution, call.frame, set_buf, scratch, listed);
    record.fixed_result = run_callback(execution, cpu, call.frame, func_fixed, list_get, fixed_set);
    record.fixed_called = 1;
    const uint32_t dir_name = lv2_sync::read_u32(execution, call.frame + fixed_set + fixed_set_dir_name);
    for (unsigned index = 0; index + 1 < sizeof record.dir_name && dir_name && lv2_sync::readable_range(execution, dir_name + index, 1); ++index) {
        unsigned char byte = 0;
        const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
        if (memory.read(memory.context, dir_name + index, &byte, 1) != PS3_GUEST_MEMORY_OK || !byte) break;
        record.dir_name[index] = char(byte >= 0x20 && byte < 0x7F ? byte : '?');
    }
    read_guest_string(execution, dir_name, call.dir, sizeof call.dir);
    log_call(call, "fixed", prefixes[0] ? prefixes : "-", "-", scratch.dir_count, record.fixed_result,
        record.fixed_result < 0 ? uint32_t(error_cbresult) : 0u);
    if (record.fixed_result < 0) { finish(error_cbresult); return; }
    /* Console run rv11: the game answers the empty list with OK_LAST (1), "finish normally, no more callbacks".
     * Going on to funcStat and returning NODATA made it show "There was an error while loading." and fall back to
     * the manual load list. OK_LAST ends the call with CELL_OK. */
    if (record.fixed_result == cbresult_ok_last) { finish(0); return; }
    /* 2. funcStat on the directory the game named, then its files when it exists. */
    finish(run_stat_and_files(execution, cpu, call, &record));
}
/* cellSaveDataListLoad2(version, setList, setBuf, funcList, funcStat, funcFile, container, userdata), NID 0x1DFBFDD6:
 * the load list the player picks from. With no save data the list is empty: funcList gets it, and its verdict
 * decides: negative -> CELL_SAVEDATA_ERROR_CBRESULT, OK_LAST -> CELL_OK, OK_NEXT -> nothing can be picked, which a
 * console reports as the player backing out (CELL_SAVEDATA_RET_CANCEL, 1). With saves the default entry is loaded. */
static void dispatch_savedata_list_load2(StartupExecution &execution, ppu_context &cpu)
{
    savedata_native::dispatch_list(execution, cpu, false);
}
/* cellSaveDataListSave2(version, setList, setBuf, funcList, funcStat, funcFile, container, userdata), NID 0x2DE0D663. */
static void dispatch_savedata_list_save2(StartupExecution &execution, ppu_context &cpu)
{
    savedata_native::dispatch_list(execution, cpu, true);
}
/* cellSaveDataAutoSave2(version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata), NID 0x8B7ED64B:
 * no list and no dialog, straight to funcStat on the named directory. */
static void dispatch_savedata_auto_save2(StartupExecution &execution, ppu_context &cpu)
{
    using namespace savedata_native;
    StartupSaveDataRecord &record = g_savedata_auto_load;
    ++record.auto_save_calls;
    const uint32_t dir_name = uint32_t(cpu.gpr[4]), err_dialog = uint32_t(cpu.gpr[5]), set_buf = uint32_t(cpu.gpr[6]);
    const uint32_t func_stat = uint32_t(cpu.gpr[7]), func_file = uint32_t(cpu.gpr[8]), userdata = uint32_t(cpu.gpr[10]);
    Call call = make_call("cellSaveDataAutoSave2", true, set_buf, func_stat, func_file);
    auto finish = [&](uint32_t code) { record.save_result = code; finish_call(cpu, call, code); };
    const bool named = read_guest_string(execution, dir_name, call.dir, sizeof call.dir) && sfo::dir_name_valid(call.dir);
    if (!named || err_dialog > 2 || !set_buf || !func_stat || !func_file ||
        !lv2_sync::readable_range(execution, set_buf, set_buf_bytes)) { finish(error_param); return; }
    call.frame = open_frame(execution, cpu, userdata);
    finish(run_stat_and_files(execution, cpu, call, nullptr));
}
#endif

/* ---- lv2 timers (70..76) and two thread calls (43, 47) ------------------------------------------- */
/* Root (2026-11-11). The table is process-wide (execution.timer_process); expiry is delivered by the
 * composition's tick thread through startup_timer_fire. Times are the microsecond clock the guest's
 * timebase is derived from (execution.ppu.services->clock). */
class TimerProcessGuard {
public:
    explicit TimerProcessGuard(StartupTimerProcess &process) : process_(process)
    {
        startup_spin_acquire(process_.lock);
    }
    ~TimerProcessGuard() { process_.lock.clear(std::memory_order_release); }
    TimerProcessGuard(const TimerProcessGuard &) = delete;
    TimerProcessGuard &operator=(const TimerProcessGuard &) = delete;
private:
    StartupTimerProcess &process_;
};
extern "C++" void startup_timer_fire(StartupTimerProcess &process, uint64_t now_us)
{
    lv2_timer_event events[LV2_TIMER_CAPACITY];
    size_t count = 0;
    {
        TimerProcessGuard guard(process);
        count = lv2_timer_collect_due(&process.table, now_us, events, LV2_TIMER_CAPACITY);
    }
    /* Sent outside the timer lock: the registry has its own guard and wakes the receiver. A full or
     * destroyed queue loses that event, as on lv2. */
    for (size_t index = 0; index < count; ++index) {
        const uint32_t code = process.objects
            ? ps3_lv2_queue_send(process.objects, events[index].queue_id, events[index].source,
                  events[index].data1, events[index].data2, 0)
            : uint32_t(PS3_LV2_ESRCH);
        if (code == PS3_LV2_OK) { if (process.fires != UINT64_MAX) ++process.fires; }
        else { if (process.send_failures != UINT64_MAX) ++process.send_failures; process.last_send_code = code; }
    }
}
static void dispatch_timer(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_sync;
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    if (!execution.timer_process) stop(StartupStop::UnsupportedSyscall, &batch);
    StartupTimerProcess &process = *execution.timer_process;
    const uint32_t first = uint32_t(cpu.gpr[3]);
    active->lv2_sync_argument = first;
    check_boundary(BoundaryBudget::Poll, &batch);
    uint32_t code = LV2_TIMER_EINVAL;
    switch (number) {
    case 70: {   // sys_timer_create(u32 *timer_id)
        if (!first || !writable(execution, first, 4)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t id = 0;
        { TimerProcessGuard guard(process); code = lv2_timer_create(&process.table, &id); }
        if (code == LV2_TIMER_OK) {
            const unsigned char bytes[4] = {static_cast<unsigned char>(id >> 24), static_cast<unsigned char>(id >> 16),
                static_cast<unsigned char>(id >> 8), static_cast<unsigned char>(id)};
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            if (memory.write(memory.context, first, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK)
                stop(StartupStop::InvalidMemory, &batch);
            active->lv2_sync_result = id;
        }
        break;
    }
    case 71: { TimerProcessGuard guard(process); code = lv2_timer_destroy(&process.table, first); break; }
    case 72: {   // sys_timer_get_information(id, sys_timer_information_t *): be64 next, be64 period, be32 state, be32 pad
        const uint32_t output = uint32_t(cpu.gpr[4]);
        if (!output || !writable(execution, output, 24)) stop(StartupStop::InvalidMemory, &batch);
        lv2_timer_information info{};
        { TimerProcessGuard guard(process); code = lv2_timer_get_information(&process.table, first, &info); }
        if (code == LV2_TIMER_OK) {
            unsigned char bytes[24]{};
            for (int i = 0; i < 8; ++i) {
                bytes[i] = static_cast<unsigned char>(info.next_us >> (56 - 8 * i));
                bytes[8 + i] = static_cast<unsigned char>(info.period_us >> (56 - 8 * i));
            }
            bytes[19] = static_cast<unsigned char>(info.state);
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            if (memory.write(memory.context, output, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK)
                stop(StartupStop::InvalidMemory, &batch);
        }
        break;
    }
    case 73: {   // _sys_timer_start(id, u64 base_time, u64 period)
        if (!execution.ppu.services || !execution.ppu.services->clock ||
            !execution.ppu.services->clock->now_microseconds) stop(StartupStop::MissingServices, &batch);
        uint64_t now_us = 0;
        const ps3_clock *clock = execution.ppu.services->clock;
        if (clock->now_microseconds(clock->context, &now_us) != PS3_CLOCK_OK) stop(StartupStop::MissingServices, &batch);
        TimerProcessGuard guard(process);
        code = lv2_timer_start(&process.table, first, cpu.gpr[4], cpu.gpr[5], now_us);
        break;
    }
    case 74: { TimerProcessGuard guard(process); code = lv2_timer_stop(&process.table, first); break; }
    case 75: {   // sys_timer_connect_event_queue(id, queue_id, u64 name, u64 data1, u64 data2)
        const uint32_t queue = uint32_t(cpu.gpr[4]);
        ps3_lv2_queue snapshot{};
        if (!process.objects) stop(StartupStop::MissingServices, &batch);
        if (ps3_lv2_queue_snapshot(process.objects, queue, &snapshot) != PS3_LV2_OK) { code = LV2_TIMER_ESRCH; break; }
        TimerProcessGuard guard(process);
        code = lv2_timer_connect(&process.table, first, queue, cpu.gpr[5], cpu.gpr[6], cpu.gpr[7]);
        break;
    }
    case 76: { TimerProcessGuard guard(process); code = lv2_timer_disconnect(&process.table, first); break; }
    default: stop(StartupStop::UnsupportedSyscall, &batch);
    }
    cpu.gpr[3] = sign_extend(code);
    ++active->lv2_sync_returns;
}
/* sys_ppu_thread_yield (43): give the host core away once. sys_ppu_thread_set_priority (47): accepted and
 * not applied (guest priorities are not mapped to host priorities yet); the thread id and value are only
 * recorded in the lv2_sync ledger. */
static void dispatch_thread_yield_or_priority(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    active->lv2_sync_argument = cpu.gpr[3];
    check_boundary(BoundaryBudget::Poll, &batch);
    if (number == 43) {
        if (!execution.lv2_sleep || !execution.lv2_sleep->sleep_microseconds) stop(StartupStop::MissingServices, &batch);
        const bool ok = execution.lv2_sleep->sleep_microseconds(execution.lv2_sleep->context, 0,
            active->cancelled.load(std::memory_order_acquire));
        check_boundary(BoundaryBudget::Poll, &batch);
        if (!ok) stop(StartupStop::Cancelled, &batch);
    } else {
        const int32_t priority = int32_t(uint32_t(cpu.gpr[4]));
        if (priority < 0 || priority > 3071) { cpu.gpr[3] = lv2_sync::sign_extend(0x80010002u); ++active->lv2_sync_returns; return; }
    }
    cpu.gpr[3] = lv2_sync::cell_ok;
    ++active->lv2_sync_returns;
}

/* ---- lv2 mutexes (100..104) ---------------------------------------------------------------------- */
/* Root (2026-11-11). sys_mutex_attribute_t: be32 protocol, be32 recursive (0x10 = recursive, 0x20 = not),
 * be32 pshared, be32 adaptive, be64 ipc_key, be32 flags, be32 pad, char name[8]. Values and error codes
 * are root's recollection of RPCS3's sys_mutex.cpp, not extracted from firmware. Priority inheritance
 * (protocol 3) is treated as priority order. The blocking wait is the registry semaphore's wait, so it is
 * sliced, cancellable and visible to the creator's prefix wait like every other lv2 wait. */
namespace lv2_mutex_abi {
constexpr uint32_t edeadlk = 0x80010008u, eperm = 0x80010009u, ebusy = 0x8001000Au, esrch = 0x80010005u,
    eagain = 0x80010001u, einval = 0x80010002u, recursive_flag = 0x10u;
class Guard {
public:
    explicit Guard(StartupMutexProcess &process) : process_(process)
    {
        startup_spin_acquire(process_.lock);
    }
    ~Guard() { process_.lock.clear(std::memory_order_release); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
private:
    StartupMutexProcess &process_;
};
static StartupMutexProcess::Entry *find(StartupMutexProcess &process, uint32_t id)
{
    for (auto &entry : process.entries) if (entry.in_use && entry.id == id) return &entry;
    return nullptr;
}
}
/* Root diagnostic (2026-11-12): which MissingServices check of dispatch_lv2_mutex ended a thread (0 = none). */
uint32_t g_lv2_mutex_stop_site = 0, g_lv2_mutex_stop_id = 0, g_lv2_mutex_stop_thread = 0;
static void dispatch_lv2_mutex(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_sync;
    using namespace lv2_mutex_abi;
    ps3_lv2_objects *const objects = execution.event_queues;
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    if (!execution.mutex_process) stop(StartupStop::UnsupportedSyscall, &batch);
    if (!objects) { g_lv2_mutex_stop_site = 1; stop(StartupStop::MissingServices, &batch); }
    StartupMutexProcess &process = *execution.mutex_process;
    const uint32_t first = uint32_t(cpu.gpr[3]);
    const uint32_t thread = uint32_t(cpu.thread_id);
    active->lv2_sync_argument = first;
    if (!thread) { g_lv2_mutex_stop_site = 2; g_lv2_mutex_stop_id = first; stop(StartupStop::MissingServices, &batch); }
    uint32_t code = einval;
    const auto finish = [&](uint32_t value) {
        if (value != PS3_LV2_OK) { Guard guard(process); if (process.errors != UINT64_MAX) ++process.errors; }
        cpu.gpr[3] = sign_extend(value);
        ++active->lv2_sync_returns;
    };
    if (number == 100) {   // sys_mutex_create(u32 *mutex_id, sys_mutex_attribute_t *attr)
        const uint32_t attr = uint32_t(cpu.gpr[4]);
        if (!first || !writable(execution, first, 4)) stop(StartupStop::InvalidMemory, &batch);
        if (!attr || !readable_range(execution, attr, 24)) stop(StartupStop::InvalidMemory, &batch);
        const uint32_t protocol = read_u32(execution, attr);
        const uint32_t recursive = read_u32(execution, attr + 4);
        if (read_u64(execution, attr + 16)) stop(StartupStop::UnsupportedSyscall, &batch);   // no IPC namespace natively
        if (protocol < 1 || protocol > 3) { finish(einval); return; }
        check_boundary(BoundaryBudget::Poll, &batch);
        ps3_lv2_semaphore_attr domain_attr{};
        domain_attr.protocol = protocol == 1 ? uint32_t(PS3_SYNC_FIFO) : uint32_t(PS3_SYNC_PRIORITY);
        uint32_t id = 0;
        code = ps3_lv2_semaphore_create(objects, &id, &domain_attr, 1, 1);
        if (code != PS3_LV2_OK) { finish(code); return; }
        bool recorded = false;
        {
            Guard guard(process);
            for (auto &entry : process.entries) {
                if (entry.in_use) continue;
                entry = StartupMutexProcess::Entry{};
                entry.id = id; entry.recursive = recursive == recursive_flag; entry.in_use = true;
                recorded = true;
                if (process.creates != UINT64_MAX) ++process.creates;
                if (++process.live > process.peak) process.peak = process.live;
                break;
            }
        }
        if (!recorded) { ps3_lv2_semaphore_destroy(objects, id); finish(eagain); return; }
        const unsigned char bytes[4] = {static_cast<unsigned char>(id >> 24), static_cast<unsigned char>(id >> 16),
            static_cast<unsigned char>(id >> 8), static_cast<unsigned char>(id)};
        const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
        if (memory.write(memory.context, first, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::InvalidMemory, &batch);
        active->lv2_sync_result = id;
        finish(PS3_LV2_OK);
        return;
    }
    if (number == 101) {   // sys_mutex_destroy(id)
        {
            Guard guard(process);
            StartupMutexProcess::Entry *entry = find(process, first);
            if (!entry) code = esrch;
            else if (entry->owner) code = ebusy;
            else {
                entry->in_use = false;
                if (process.live) --process.live;
                if (process.destroys != UINT64_MAX) ++process.destroys;
                code = PS3_LV2_OK;
            }
        }
        if (code == PS3_LV2_OK) ps3_lv2_semaphore_destroy(objects, first);
        finish(code);
        return;
    }
    if (number == 104) {   // sys_mutex_unlock(id)
        bool post = false;
        {
            Guard guard(process);
            StartupMutexProcess::Entry *entry = find(process, first);
            if (!entry) code = esrch;
            else if (entry->owner != thread) code = eperm;
            else {
                code = PS3_LV2_OK;
                if (process.unlocks != UINT64_MAX) ++process.unlocks;
                if (--entry->count == 0) { entry->owner = 0; post = true; }
            }
        }
        if (post) ps3_lv2_semaphore_post(objects, first, 1);
        finish(code);
        return;
    }
    if (number != 102 && number != 103) stop(StartupStop::UnsupportedSyscall, &batch);
    /* 102 sys_mutex_lock(id, u64 timeout_us) and 103 sys_mutex_trylock(id). */
    {
        Guard guard(process);
        StartupMutexProcess::Entry *entry = find(process, first);
        if (!entry) { code = esrch; }
        else if (entry->owner == thread) {
            if (entry->recursive) { ++entry->count; code = PS3_LV2_OK; if (process.locks != UINT64_MAX) ++process.locks; }
            else code = edeadlk;
        } else code = 0xFFFFFFFFu;   // must take the semaphore
    }
    if (code != 0xFFFFFFFFu) { finish(code); return; }
    if (number == 103) {
        code = ps3_lv2_semaphore_trywait(objects, first);
    } else {
        code = ps3_lv2_semaphore_trywait(objects, first);
        if (code == PS3_LV2_EBUSY) {
            { Guard guard(process); if (process.contended != UINT64_MAX) ++process.contended; }
            const uint64_t timeout_us = cpu.gpr[4];
            if (!execution.lv2_sync_cancel) { g_lv2_mutex_stop_site = 3; g_lv2_mutex_stop_id = first; g_lv2_mutex_stop_thread = thread; stop(StartupStop::MissingServices, &batch); }
            check_boundary(BoundaryBudget::Poll, &batch);
            code = lv2_sliced_wait(timeout_us, batch, [&](uint64_t slice) {
                return ps3_lv2_semaphore_wait(objects, first, slice, execution.lv2_sync_cancel,
                    lv2_blocked_word(execution, active));
            });
            active->lv2_sync_blocked = 0;
            check_boundary(BoundaryBudget::Poll, &batch);
            if (code == PS3_LV2_CANCELLED) stop(StartupStop::Cancelled, &batch);
            if (code == PS3_LV2_PROVIDER_FAILURE) stop(StartupStop::ProviderFailure, &batch);
        }
    }
    if (code == PS3_LV2_OK) {
        Guard guard(process);
        StartupMutexProcess::Entry *entry = find(process, first);
        if (entry) { entry->owner = thread; entry->count = 1; if (process.locks != UINT64_MAX) ++process.locks; }
        else code = esrch;   // destroyed while this thread waited
    }
    finish(code);
}

/* ---- the Bink reader's mutex handoff (Windows-port hook in the lifted image) ------------------------- */
/* Root (2026-11-12, Decision 62). The lifted image calls these two around the Bink movie reader's
 * sys_lwcond_wait at 0x187994 (condition 0x008EB1F8). The reader holds an lv2 mutex (the Bink I/O mutex) while it
 * waits there for frame data, and the Bink I/O thread needs that same mutex to deliver the data: the Windows port
 * found it had to give the mutex up for the length of that one wait (source/infamous-recomp/stubs.cpp,
 * infamous_bink_wait_unlock). The native port needs the same: with the two helpers answering "nothing released"
 * (Decision 46) a movie run showed two mutexes created, two locks, NO unlock, and the game presenting black
 * frames at the movie's rate for ever (Eden r43).
 *   unlock: only at that call site and condition; releases every lv2 mutex this thread owns (normally one) and
 *           remembers them; returns 1 when something was released.
 *   relock: takes them back (up to about 1 s each, cancellable). A mutex that cannot be retaken is left unowned;
 *           the game's own unlock then gets EPERM, which is what the Windows port accepts too.
 * Call-site and condition addresses are the Windows port's, checked against the lifted source (line 428859). */
namespace bink_handoff {
struct Released { uint32_t id, count; };
thread_local Released released[4];
thread_local unsigned released_count;
}
extern "C" int infamous_bink_wait_unlock(ppu_context *ctx, uint32_t cond)
{
    using namespace lv2_mutex_abi;
    if (!ctx || !active || !active->cpu) return 0;
    StartupExecution &execution = execution_owner(active->cpu);
    if (!execution.mutex_process || !execution.event_queues) return 0;
    StartupMutexProcess &process = *execution.mutex_process;
    const uint32_t thread = uint32_t(ctx->thread_id);
    {
        Guard guard(process);
        if (process.bink_calls != UINT64_MAX) ++process.bink_calls;
        if (!process.bink_first_cond) {   // diagnostic snapshot of the first call
            process.bink_first_cond = cond; process.bink_first_lr = uint32_t(ctx->lr); process.bink_first_thread = thread;
            unsigned slot = 0;
            for (auto &entry : process.entries)
                if (entry.in_use && slot < 4) { process.bink_first_ids[slot] = entry.id; process.bink_first_owners[slot++] = entry.owner; }
        }
    }
    if (cond != 0x008EB1F8u || uint32_t(ctx->lr) != 0x00187994u) return 0;
    if (!thread) return 0;
    bink_handoff::released_count = 0;
    {
        Guard guard(process);
        for (auto &entry : process.entries) {
            if (!entry.in_use || entry.owner != thread || bink_handoff::released_count >= 4) continue;
            bink_handoff::released[bink_handoff::released_count++] = {entry.id, entry.count};
            entry.owner = 0; entry.count = 0;
            if (process.unlocks != UINT64_MAX) ++process.unlocks;
            if (process.bink_handoffs != UINT64_MAX) ++process.bink_handoffs;
        }
    }
    for (unsigned index = 0; index < bink_handoff::released_count; ++index)
        ps3_lv2_semaphore_post(execution.event_queues, bink_handoff::released[index].id, 1);
    return bink_handoff::released_count != 0;
}
extern "C" void infamous_bink_wait_relock(ppu_context *ctx, int was_released)
{
    using namespace lv2_mutex_abi;
    if (!was_released || !ctx || !active || !active->cpu) return;
    StartupExecution &execution = execution_owner(active->cpu);
    if (!execution.mutex_process || !execution.event_queues) return;
    StartupMutexProcess &process = *execution.mutex_process;
    const uint32_t thread = uint32_t(ctx->thread_id);
    for (unsigned index = 0; index < bink_handoff::released_count; ++index) {
        const bink_handoff::Released item = bink_handoff::released[index];
        uint32_t code = PS3_LV2_EBUSY;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            code = ps3_lv2_semaphore_trywait(execution.event_queues, item.id);
            if (code != PS3_LV2_EBUSY) break;
            if (active->cancelled.load(std::memory_order_acquire)) break;
            if (!execution.lv2_sleep || !execution.lv2_sleep->sleep_microseconds ||
                !execution.lv2_sleep->sleep_microseconds(execution.lv2_sleep->context, 1000,
                    active->cancelled.load(std::memory_order_acquire))) break;
        }
        Guard guard(process);
        if (code == PS3_LV2_OK) {
            if (StartupMutexProcess::Entry *entry = find(process, item.id)) {
                entry->owner = thread; entry->count = item.count ? item.count : 1;
                if (process.locks != UINT64_MAX) ++process.locks;
            }
        } else if (process.bink_relock_failures != UINT64_MAX) ++process.bink_relock_failures;
    }
    bink_handoff::released_count = 0;
}

/* ---- message dialog and the sysutil callback poll -------------------------------------------------- */
/* Root (2026-11-11). NIDs checked by hash: cellMsgDialogOpen2 0x7603d3db, cellMsgDialogClose 0x20543730,
 * cellMsgDialogAbort 0x62b0f803, cellSysutilCheckCallback 0x189a74da. Type bits and button values are root's
 * recollection of the SDK header / RPCS3 (button type in bits 4..6: 1 = YES/NO, 2 = OK; results YES = 1,
 * OK = 1, NONE = -1), not extracted from firmware. */
namespace dialog_bridge {
class Guard {
public:
    explicit Guard(StartupDialogProcess &process) : process_(process)
    {
        startup_spin_acquire(process_.lock);
    }
    ~Guard() { process_.lock.clear(std::memory_order_release); }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
private:
    StartupDialogProcess &process_;
};
}
/* Calls a guest function through its OPD on the calling guest thread, with up to three arguments, and returns
 * when it returns. A new 0x80-byte frame is opened below the caller's stack pointer (back chain written), the
 * TOC and every register the call changes are restored afterwards. Used for callbacks the firmware would run
 * from inside an import. */
static void call_guest_opd(StartupExecution &execution, uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2)
{
    ppu_context &cpu = execution.ppu.cpu;
    if (!opd || !lv2_sync::readable_range(execution, opd, 8)) stop(StartupStop::InvalidMemory);
    const uint32_t entry = lv2_sync::read_u32(execution, opd), toc = lv2_sync::read_u32(execution, opd + 4);
    const ppu_context saved = cpu;
    const uint64_t frame = (cpu.gpr[1] - 0x80u) & ~UINT64_C(15);
    if (frame > UINT32_MAX || !writable(execution, uint32_t(frame), 0x80)) stop(StartupStop::InvalidMemory);
    unsigned char backchain[8];
    for (int i = 0; i < 8; ++i) backchain[i] = static_cast<unsigned char>(saved.gpr[1] >> (56 - 8 * i));
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, uint32_t(frame), backchain, sizeof(backchain)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::InvalidMemory);
    cpu.gpr[1] = frame;
    cpu.gpr[2] = toc;
    cpu.gpr[3] = a0; cpu.gpr[4] = a1; cpu.gpr[5] = a2;
    cpu.ctr = entry;
    ps3_indirect_call(&cpu);
    while (g_trampoline_fn) {
        void (*const pending)(void *) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        pending(&cpu);
    }
    /* Everything the callee may have changed that the import's caller relies on is put back; the import's own
     * result register is set by the arm afterwards. */
    cpu.gpr[1] = saved.gpr[1]; cpu.gpr[2] = saved.gpr[2]; cpu.lr = saved.lr; cpu.ctr = saved.ctr;
    for (int index = 13; index < 32; ++index) cpu.gpr[index] = saved.gpr[index];
}
static bool dispatch_dialog_import(StartupExecution &execution, uint32_t nid)
{
    enum : uint32_t { open2 = 0x7603d3dbu, close_nid = 0x20543730u, abort_nid = 0x62b0f803u, check = 0x189a74dau };
    if (nid != open2 && nid != close_nid && nid != abort_nid && nid != check) return false;
    if (!execution.dialog_process) return false;
    if (!execution.ppu.admitted || !execution.ppu.memory) stop(StartupStop::MissingServices);
    StartupDialogProcess &process = *execution.dialog_process;
    ppu_context &cpu = execution.ppu.cpu;
    check_boundary(BoundaryBudget::Poll);
    uint64_t result = 0;
    if (nid == open2) {
        const uint32_t type = uint32_t(cpu.gpr[3]), text = uint32_t(cpu.gpr[4]);
        char buffer[160]{};
        if (text) {
            const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
            for (size_t index = 0; index + 1 < sizeof(buffer); ++index) {
                unsigned char byte = 0;
                if (memory.read(memory.context, text + uint32_t(index), &byte, 1) != PS3_GUEST_MEMORY_OK || !byte) break;
                buffer[index] = (byte >= 0x20 && byte < 0x7F) ? char(byte) : '.';
            }
        }
        dialog_bridge::Guard guard(process);
        if (process.open) {
            if (process.busy != UINT64_MAX) ++process.busy;
            result = uint64_t(int64_t(int32_t(0x8002B105u)));   // a dialog is already open (CELL_SYSUTIL_ERROR_BUSY)
        } else {
            process.open = true;
            process.type = process.last_type = type;
            process.callback = uint32_t(cpu.gpr[5]);
            process.userdata = uint32_t(cpu.gpr[6]);
            std::memcpy(process.last_text, buffer, sizeof(process.last_text));
            if (process.opens != UINT64_MAX) ++process.opens;
            const uint32_t buttons = (type >> 4) & 7u;
            if (buttons == 1u || buttons == 2u) {   // YES/NO or OK: answered at once with YES / OK
                process.open = false;
                process.result_pending = true;
                process.button = 1;
            }
        }
    } else if (nid == close_nid || nid == abort_nid) {
        dialog_bridge::Guard guard(process);
        if (!process.open) {
            result = uint64_t(int64_t(int32_t(0x8002B302u)));   // no dialog is open (CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED)
        } else {
            process.open = false;
            if (nid == close_nid) {
                process.result_pending = true;
                process.button = -1;   // CELL_MSGDIALOG_BUTTON_NONE
                if (process.closes != UINT64_MAX) ++process.closes;
            } else if (process.aborts != UINT64_MAX) ++process.aborts;
        }
    } else {
        /* cellSysutilCheckCallback: deliver one pending dialog result on this thread. */
        uint32_t callback = 0, userdata = 0;
        int32_t button = 0;
        bool deliver = false;
        {
            dialog_bridge::Guard guard(process);
            if (process.check_calls != UINT64_MAX) ++process.check_calls;
            if (process.result_pending) {
                process.result_pending = false;
                callback = process.callback; userdata = process.userdata; button = process.button;
                deliver = callback != 0;
                if (deliver && process.callbacks != UINT64_MAX) ++process.callbacks;
            }
        }
        if (deliver) call_guest_opd(execution, callback, uint64_t(int64_t(button)), userdata, 0);
    }
    cpu.gpr[3] = result;
    return true;
}

/* ---- semaphores (90..94) --------------------------------------------------------------------- */
/* sys_semaphore_attr: u32 protocol, u32 pshared, u64 ipc_key, s32 flags, u32 pad, char name[8]. */
static void dispatch_semaphore(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_sync;
    ps3_lv2_objects *const objects = execution.event_queues;
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    if (!objects) stop(StartupStop::MissingServices, &batch);
    const uint32_t first = uint32_t(cpu.gpr[3]);
    active->lv2_sync_argument = first;
    if (number == sc_semaphore_create) {
        const uint32_t attr = uint32_t(cpu.gpr[4]);
        const int32_t initial = int32_t(uint32_t(cpu.gpr[5]));
        const int32_t max = int32_t(uint32_t(cpu.gpr[6]));
        if (!first || !writable(execution, first, 4)) stop(StartupStop::InvalidMemory, &batch);
        if (!attr || !readable_range(execution, attr, 24)) stop(StartupStop::InvalidMemory, &batch);
        ps3_lv2_semaphore_attr domain_attr{};
        domain_attr.protocol = read_u32(execution, attr);
        const uint64_t ipc_key = read_u64(execution, attr + 8);
        if (ipc_key) stop(StartupStop::UnsupportedSyscall, &batch);   // no IPC namespace natively
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        begin_trace(execution, 1, batch);
        uint32_t id = 0;
        const uint32_t code = ps3_lv2_semaphore_create(objects, &id, &domain_attr, initial, max);
        if (code != PS3_LV2_OK) {   // nothing was allocated and nothing is stored
            finish_owned(&batch);
            check_boundary(BoundaryBudget::Poll);
            cpu.gpr[3] = sign_extend(code);
            ++active->lv2_sync_returns;
            return;
        }
        publish_u32(execution, first, id, batch);
        finish_owned(&batch);
        active->lv2_sync_result = id;
        cpu.gpr[3] = cell_ok;
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_semaphore_destroy) {
        const uint32_t code = ps3_lv2_semaphore_destroy(objects, first);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_semaphore_trywait) {
        const uint32_t code = ps3_lv2_semaphore_trywait(objects, first);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_semaphore_post) {
        const int32_t count = int32_t(uint32_t(cpu.gpr[4]));
        const uint32_t code = ps3_lv2_semaphore_post(objects, first, count);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    /* sc_semaphore_wait: id, u64 timeout_us (0 = forever). A sleep holds no batch; the domain releases
     * the object guard while it sleeps, so any thread's post reaches it. */
    const uint64_t timeout_us = cpu.gpr[4];
    if (!execution.lv2_sync_cancel) stop(StartupStop::MissingServices, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const uint32_t code = lv2_sliced_wait(timeout_us, batch, [&](uint64_t slice) {
        return ps3_lv2_semaphore_wait(objects, first, slice, execution.lv2_sync_cancel,
            lv2_blocked_word(execution, active));
    });
    active->lv2_sync_blocked = 0;
    check_boundary(BoundaryBudget::Poll, &batch);
    if (code == PS3_LV2_CANCELLED) stop(StartupStop::Cancelled, &batch);
    if (code == PS3_LV2_PROVIDER_FAILURE) stop(StartupStop::ProviderFailure, &batch);
    cpu.gpr[3] = sign_extend(code);
    ++active->lv2_sync_returns;
}

/* ---- event flags (82, 83, 85, 86, 87, 118) ---------------------------------------------------- */
/* sys_event_flag_attr: u32 protocol, u32 pshared, u64 ipc_key, s32 flags, s32 type, char name[8]. */
static void dispatch_event_flag(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_sync;
    ps3_lv2_objects *const objects = execution.event_queues;
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    if (!objects) stop(StartupStop::MissingServices, &batch);
    const uint32_t first = uint32_t(cpu.gpr[3]);
    active->lv2_sync_argument = first;
    if (number == sc_event_flag_create) {
        const uint32_t attr = uint32_t(cpu.gpr[4]);
        const uint64_t bits = cpu.gpr[5];
        if (!first || !writable(execution, first, 4)) stop(StartupStop::InvalidMemory, &batch);
        if (!attr || !readable_range(execution, attr, 24)) stop(StartupStop::InvalidMemory, &batch);
        const uint32_t protocol = read_u32(execution, attr);
        const uint64_t ipc_key = read_u64(execution, attr + 8);
        const uint32_t type = read_u32(execution, attr + 16);
        if (ipc_key) stop(StartupStop::UnsupportedSyscall, &batch);
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        begin_trace(execution, 1, batch);
        uint32_t id = 0;
        const uint32_t code = ps3_lv2_event_flag_create(objects, &id, protocol, type, bits);
        if (code != PS3_LV2_OK) {
            finish_owned(&batch);
            check_boundary(BoundaryBudget::Poll);
            cpu.gpr[3] = sign_extend(code);
            ++active->lv2_sync_returns;
            return;
        }
        publish_u32(execution, first, id, batch);
        finish_owned(&batch);
        active->lv2_sync_result = id;
        cpu.gpr[3] = cell_ok;
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_event_flag_destroy) {
        const uint32_t code = ps3_lv2_event_flag_destroy(objects, first);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_event_flag_set) {
        const uint32_t code = ps3_lv2_event_flag_set(objects, first, cpu.gpr[4]);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    if (number == sc_event_flag_clear) {
        const uint32_t code = ps3_lv2_event_flag_clear(objects, first, cpu.gpr[4]);
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    const uint64_t bitptn = cpu.gpr[4];
    const uint32_t mode = uint32_t(cpu.gpr[5]);
    const uint32_t output = uint32_t(cpu.gpr[6]);
    if (!output || !writable(execution, output, 8)) stop(StartupStop::InvalidMemory, &batch);
    if (number == sc_event_flag_trywait) {
        uint64_t result = 0;
        const uint32_t code = ps3_lv2_event_flag_trywait(objects, first, bitptn, mode, &result);
        if (code == PS3_LV2_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            check_boundary(BoundaryBudget::Poll, &batch);
            begin_trace(execution, 1, batch);
            publish_u64(execution, output, result, batch);
            finish_owned(&batch);
        }
        cpu.gpr[3] = sign_extend(code);
        ++active->lv2_sync_returns;
        return;
    }
    /* sc_event_flag_wait: id, u64 bitptn, u32 mode, u64* result, u64 timeout. */
    const uint64_t timeout_us = cpu.gpr[7];
    if (!execution.lv2_sync_cancel) stop(StartupStop::MissingServices, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    uint64_t result = 0;
    const uint32_t code = lv2_sliced_wait(timeout_us, batch, [&](uint64_t slice) {
        return ps3_lv2_event_flag_wait(objects, first, bitptn, mode, &result, slice,
            execution.lv2_sync_cancel, lv2_blocked_word(execution, active));
    });
    active->lv2_sync_blocked = 0;
    check_boundary(BoundaryBudget::Poll, &batch);
    if (code == PS3_LV2_CANCELLED) stop(StartupStop::Cancelled, &batch);
    if (code == PS3_LV2_PROVIDER_FAILURE) stop(StartupStop::ProviderFailure, &batch);
    if (code == PS3_LV2_OK) {   // only success writes the matched bits
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        begin_trace(execution, 1, batch);
        publish_u64(execution, output, result, batch);
        finish_owned(&batch);
    }
    cpu.gpr[3] = sign_extend(code);
    ++active->lv2_sync_returns;
}

/* ---- queues and ports (129, 131, 135, 137, 138) ---------------------------------------------- */
/* sys_event_t is four u64 words. */
static void dispatch_event_object(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_sync;
    ps3_lv2_objects *const objects = execution.event_queues;
    HleTraceBatch batch;
    if (active->lv2_sync_calls != SIZE_MAX) ++active->lv2_sync_calls;
    active->lv2_sync_number = number;
    if (!objects) stop(StartupStop::MissingServices, &batch);
    const uint32_t first = uint32_t(cpu.gpr[3]);
    active->lv2_sync_argument = first;
    switch (number) {
    case sc_event_queue_destroy: {
        const uint32_t mode = uint32_t(cpu.gpr[4]);
        if (mode > 1) stop(StartupStop::UnsupportedSyscall, &batch);   // only force 0/1 are defined
        const uint32_t code = ps3_lv2_queue_destroy_force(objects, first, mode == 1);
        cpu.gpr[3] = sign_extend(code);
        break;
    }
    case sc_event_queue_tryreceive: {
        const uint32_t array = uint32_t(cpu.gpr[4]);
        const uint32_t size = uint32_t(cpu.gpr[5]);
        const uint32_t output = uint32_t(cpu.gpr[6]);
        if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
        if (array && size != 0) {
            const uint64_t bytes = uint64_t(size) * 32;
            if (bytes > 0x1000 || !writable(execution, array, bytes)) stop(StartupStop::InvalidMemory, &batch);
        }
        ps3_lv2_event events[32]{};
        if (size > 32) stop(StartupStop::UnsupportedSyscall, &batch);
        uint32_t number_out = 0;
        const uint32_t code = ps3_lv2_queue_tryreceive(objects, first, events, size, &number_out);
        if (code == PS3_LV2_OK && number_out) {
            if (!trace_can_begin(execution, 1 + number_out)) stop(StartupStop::TraceCapacity, &batch);
            check_boundary(BoundaryBudget::Poll, &batch);
            begin_trace(execution, 1 + number_out, batch);
            for (uint32_t index = 0; index < number_out; ++index) {
                const uint32_t base = array + index * 32;
                publish_u64(execution, base, events[index].source, batch);
                publish_u64(execution, base + 8, events[index].data1, batch);
                publish_u64(execution, base + 16, events[index].data2, batch);
                publish_u64(execution, base + 24, events[index].data3, batch);
            }
            publish_u32(execution, output, number_out, batch);
            finish_owned(&batch);
        } else if (code == PS3_LV2_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            check_boundary(BoundaryBudget::Poll, &batch);
            begin_trace(execution, 1, batch);
            publish_u32(execution, output, 0, batch);
            finish_owned(&batch);
        }
        cpu.gpr[3] = sign_extend(code);
        break;
    }
    case sc_event_port_destroy: {
        const uint32_t code = ps3_lv2_port_destroy(objects, first);
        cpu.gpr[3] = sign_extend(code);
        break;
    }
    case sc_event_port_disconnect: {
        const uint32_t code = ps3_lv2_port_disconnect(objects, first);
        cpu.gpr[3] = sign_extend(code);
        break;
    }
    default: {   // sc_event_port_send: id, u64 d1, u64 d2, u64 d3
        const uint32_t code = ps3_lv2_port_send(objects, first, cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
        cpu.gpr[3] = sign_extend(code);
        break;
    }
    }
    ++active->lv2_sync_returns;
}
#endif
#ifdef PS3_NATIVE_LV2_SPU

/* Decision 33 / B2b: SPU images and SPU thread groups as native lv2 objects.
 *
 * Nothing below runs an SPU. sys_spu_thread_group_start validates, records what phase C1 will run and
 * stops at StartupStop::SpuThreadGroupStart; the local stores it names are the composition's own
 * buffers and outlive the stop.
 *
 * Every return value was checked against the lifted caller's branch: `if (!((ctx->cr >> N) & 2))
 * goto fail` requires r3 == 0 EXACTLY on success, so success is always CELL_OK and never a
 * non-negative non-zero value. */
namespace lv2_spu_abi {

enum : uint32_t {
    sc_thread_get_exit_status = 165, sc_thread_set_argument = 166,
    sc_group_create = 170, sc_group_destroy = 171, sc_thread_initialize = 172, sc_group_start = 173,
    sc_group_terminate = 177, sc_group_join = 178, sc_group_set_priority = 179, sc_group_get_priority = 180,
    sc_thread_write_ls = 181, sc_thread_read_ls = 182, sc_group_connect_event = 185,
    sc_group_disconnect_event = 186, sc_thread_connect_event = 191, sc_thread_disconnect_event = 192,
    sc_thread_bind_queue = 193, sc_thread_unbind_queue = 194, sc_group_connect_all = 251
};
enum { event_run = 1, event_exception = 2, event_system = 4 };
constexpr uint64_t spu_cell_ok = 0;
constexpr uint64_t spu_sign_extend(uint32_t code) { return uint64_t(int64_t(int32_t(code))); }
/* Decision 33 / B2b-2: NO internal code may reach the guest. Every value at or above 0xFFFF0000 is an
 * outcome of THIS layer (provider failure, cancellation, a fenced queue, a legal-but-unowned input),
 * not a CELL error, and the console would never produce it. Each maps to a named stop; only real lv2
 * error numbers are returned in r3. */
bool spu_is_internal(uint32_t code) { return code >= 0xFFFF0000u; }
[[noreturn]] void spu_internal_stop(uint32_t code, HleTraceBatch &batch)
{
    switch (code) {
    case PS3_LV2_CANCELLED: stop(StartupStop::Cancelled, &batch);
    case PS3_LV2_TIMED_OUT: stop(StartupStop::Deadline, &batch);
    case PS3_LV2_UNSUPPORTED: stop(StartupStop::UnsupportedSyscall, &batch);
    case PS3_LV2_FENCED: stop(StartupStop::ProviderFailure, &batch);
    case PS3_LV2_SPU_UNSUPPORTED: stop(StartupStop::UnsupportedSyscall, &batch);
    case PS3_LV2_SPU_PROVIDER_FAILURE: stop(StartupStop::ProviderFailure, &batch);
    default: stop(StartupStop::ProviderFailure, &batch);
    }
}
/* The single return path: an internal code stops, a guest code goes to r3 sign-extended. */
uint64_t spu_guest_result(uint32_t code, HleTraceBatch &batch)
{
    if (spu_is_internal(code)) spu_internal_stop(code, batch);
    return code == PS3_LV2_SPU_OK ? spu_cell_ok : spu_sign_extend(code);
}
/* Guest layout of sys_spu_image_t: { u32 type; u32 entry_point; u32 segs; s32 nsegs }. */
enum { image_type = 0x00, image_entry = 0x04, image_segs = 0x08, image_nsegs = 0x0C, image_bytes = 0x10 };
/* The composition's SPU registry and its local-store pool, reached through StartupExecution. */
bool ready(const StartupExecution &execution)
{
    return execution.spu_images && execution.spu_groups;
}
bool spu_readable_range(const StartupExecution &execution, uint32_t address, uint64_t length)
{
    if (!execution.ppu.admitted || !execution.ppu.memory || length > UINT64_C(0x100000000) - address)
        return false;
    const PpuMemoryBinding &binding = *execution.ppu.memory;
    for (size_t index = 0; index < binding.ram_count; ++index) {
        const PpuMemoryRamRange &range = binding.ram[index];
        if (uint64_t(address) >= range.address && uint64_t(address) - range.address <= range.length &&
            length <= range.length - (uint64_t(address) - range.address)) return true;
    }
    return false;
}
void spu_publish_u32(StartupExecution &execution, uint32_t address, uint32_t value, HleTraceBatch &batch)
{
    const uint32_t encoded = __builtin_bswap32(value);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.write(memory.context, address, &encoded, sizeof(encoded)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, address, 4, value), batch);
}
}  // namespace lv2_spu_abi

/* _sys_spu_image_import (sysPrxForUser 0xEBE5F72F): img*, u32 src, u32 type.
 *
 * Caller branch (ppu_recomp_000.cpp:18603-18609 in libsre_func_3000F0F4):
 *     ctx->gpr[11] = (int64_t)(int32_t)(0xEBE5F72F); ps3_hle_call(...);    // the import stub
 *     cr12 <- (r3 < 0); if (!((cr12) & 2)) goto loc_3000Fxxx               // ANY negative r3 fails
 * So a non-zero non-negative r3 would be read as SUCCESS while carrying no information. Success is
 * therefore CELL_OK exactly. */
static void dispatch_spu_image_import(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_spu_abi;
    const uint32_t image_ea = uint32_t(cpu.gpr[3]);
    const uint32_t source_ea = uint32_t(cpu.gpr[4]);
    HleTraceBatch batch;
    if (active->lv2_spu_calls != SIZE_MAX) ++active->lv2_spu_calls;
    active->lv2_spu_number = 0xEBE5F72Fu;
    active->lv2_spu_argument = source_ea;
    if (!ready(execution)) stop(StartupStop::MissingServices, &batch);
    if (!image_ea || !writable(execution, image_ea, image_bytes)) stop(StartupStop::InvalidMemory, &batch);
    /* Read the SPU ELF through the guest memory port, a page at a time, without a whole-image copy. */
    unsigned char header[0x40]{};
    if (!lv2_spu_abi::spu_readable_range(execution, source_ea, sizeof(header))) stop(StartupStop::InvalidMemory, &batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, source_ea, header, sizeof(header)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    if (!(header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F') ||
        ((header[0x12] << 8) | header[0x13]) != 0x17)
        stop(StartupStop::UnsupportedExternal, &batch);   // not an SPU ELF: nothing to import
    /* Decision 33 / B2b-2: the segs array lives in the SYSTEM arena, not in the title's user-memory
     * quota. On a console it is kernel-owned memory, and the game allocator behind sc 348 only grants
     * 64 KiB / 1 MiB blocks whose size is a multiple of the alignment, so it could never serve a
     * 768-byte request. The arena is a fixed window with fixed slots (lv2_spu.c). */
    const uint32_t segs_bytes = PS3_LV2_SPU_IMAGE_SEGMENTS_MAX * PS3_LV2_SPU_SEGMENT_BYTES;
    if (!trace_can_begin(execution, 4)) stop(StartupStop::TraceCapacity, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    begin_trace(execution, 4, batch);
    uint32_t segs_ea = 0;
    const uint32_t reserved = ps3_lv2_spu_arena_allocate(execution.spu_images, segs_bytes, &segs_ea);
    if (reserved == PS3_LV2_SPU_ENOMEM) {   // the guest's own ENOMEM, and nothing is written
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[3] = lv2_spu_abi::spu_sign_extend(PS3_LV2_SPU_ENOMEM);
        return;
    }
    if (reserved != PS3_LV2_SPU_OK) {
        finish_owned(&batch);
        cpu.gpr[3] = lv2_spu_abi::spu_guest_result(reserved, batch);
        return;
    }
    active->lv2_spu_segments_address = segs_ea;
    /* Parse the ELF into a host buffer, then publish the guest copy big-endian. */
    static constexpr size_t kElfCapacity = 0x48000;   // root 2026-11-11: was 0x10000; a game SPU program can fill the 256 KiB local store
    std::unique_ptr<unsigned char[]> elf_storage(new unsigned char[kElfCapacity]);
    unsigned char *const elf_bytes_ptr = elf_storage.get();
    size_t elf_length = 0;
    {
        /* The ELF the caller passes is small (A1 measured 2100 and 2116 bytes); read its header, then
         * the program-header table, and only then the segment bytes it actually needs. */
        const uint32_t phoff = (uint32_t(header[0x1C]) << 24) | (uint32_t(header[0x1D]) << 16) |
            (uint32_t(header[0x1E]) << 8) | uint32_t(header[0x1F]);
        const uint16_t phentsize = uint16_t((header[0x2A] << 8) | header[0x2B]);
        const uint16_t phnum = uint16_t((header[0x2C] << 8) | header[0x2D]);
        const uint32_t table_bytes = uint32_t(phentsize ? phentsize : 0x20) * phnum;
        if (phoff > kElfCapacity || table_bytes > kElfCapacity - phoff) stop(StartupStop::UnsupportedExternal, &batch);
        size_t needed = phoff + table_bytes;
        if (memory.read(memory.context, source_ea, elf_bytes_ptr, 0x40) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        if (table_bytes && memory.read(memory.context, source_ea + phoff, elf_bytes_ptr + phoff, table_bytes) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        for (uint16_t index = 0; index < phnum; ++index) {
            const unsigned char *ph = elf_bytes_ptr + phoff + uint32_t(index) * (phentsize ? phentsize : 0x20);
            if (((uint32_t(ph[0]) << 24) | (uint32_t(ph[1]) << 16) | (uint32_t(ph[2]) << 8) | ph[3]) != 1) continue;
            const uint32_t offset = (uint32_t(ph[4]) << 24) | (uint32_t(ph[5]) << 16) | (uint32_t(ph[6]) << 8) | ph[7];
            const uint32_t filesz = (uint32_t(ph[0x10]) << 24) | (uint32_t(ph[0x11]) << 16) |
                (uint32_t(ph[0x12]) << 8) | ph[0x13];
            if (uint64_t(offset) + filesz > needed) needed = uint64_t(offset) + filesz;
        }
        if (needed > kElfCapacity) stop(StartupStop::UnsupportedExternal, &batch);
        if (needed > phoff + table_bytes &&
            memory.read(memory.context, source_ea, elf_bytes_ptr, size_t(needed)) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        elf_length = needed;
    }
    unsigned char segments[size_t(PS3_LV2_SPU_IMAGE_SEGMENTS_MAX) * PS3_LV2_SPU_SEGMENT_BYTES]{};
    ps3_lv2_spu_image image{};
    uint32_t written = 0;
    const uint32_t parsed = ps3_lv2_spu_image_parse(execution.spu_images, elf_bytes_ptr, elf_length, &image,
        segments, sizeof(segments), &written);
    if (parsed != PS3_LV2_SPU_OK) {
        finish_owned(&batch);
        check_boundary(BoundaryBudget::Poll);
        cpu.gpr[3] = lv2_spu_abi::spu_guest_result(parsed, batch);
        return;
    }
    /* Rewrite each COPY record's source as an EA the guest can read, and publish the whole array. */
    for (uint32_t index = 0; index < written; ++index) {
        unsigned char *record = segments + size_t(index) * PS3_LV2_SPU_SEGMENT_BYTES;
        if (((uint32_t(record[0]) << 24) | (uint32_t(record[1]) << 16) | (uint32_t(record[2]) << 8) | record[3]) == 1) {
            const uint32_t offset = (uint32_t(record[0x10]) << 24) | (uint32_t(record[0x11]) << 16) |
                (uint32_t(record[0x12]) << 8) | record[0x13];
            const uint32_t ea = source_ea + offset;
            record[0x10] = record[0x14] = (unsigned char)(ea >> 24);
            record[0x11] = record[0x15] = (unsigned char)(ea >> 16);
            record[0x12] = record[0x16] = (unsigned char)(ea >> 8);
            record[0x13] = record[0x17] = (unsigned char)ea;
        }
    }
    if (written && memory.write(memory.context, segs_ea, segments,
            size_t(written) * PS3_LV2_SPU_SEGMENT_BYTES) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    image.segs_ea = written ? segs_ea : 0;
    image.source_ea = source_ea;
    const uint32_t registered = ps3_lv2_spu_image_register(execution.spu_images, &image);
    if (registered != PS3_LV2_SPU_OK) {
        finish_owned(&batch);
        cpu.gpr[3] = lv2_spu_abi::spu_guest_result(registered, batch);
        return;
    }
    lv2_spu_abi::spu_publish_u32(execution, image_ea + image_type, image.type, batch);
    lv2_spu_abi::spu_publish_u32(execution, image_ea + image_entry, image.entry_point, batch);
    lv2_spu_abi::spu_publish_u32(execution, image_ea + image_segs, image.segs_ea, batch);
    lv2_spu_abi::spu_publish_u32(execution, image_ea + image_nsegs, image.nsegs, batch);
    active->lv2_spu_last_entry = image.entry_point;
    active->lv2_spu_image_segments = written;
    finish_owned(&batch);
    check_boundary(BoundaryBudget::Poll);
    cpu.gpr[3] = lv2_spu_abi::spu_cell_ok;
}

/* _sys_spu_image_close: free the guest segs array the import allocated. */
static void dispatch_spu_image_close(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_spu_abi;
    const uint32_t image_ea = uint32_t(cpu.gpr[3]);
    HleTraceBatch batch;
    if (active->lv2_spu_calls != SIZE_MAX) ++active->lv2_spu_calls;
    active->lv2_spu_number = 0xE0DA8EFDu;
    active->lv2_spu_argument = image_ea;
    if (!ready(execution)) stop(StartupStop::MissingServices, &batch);
    if (!image_ea || !lv2_spu_abi::spu_readable_range(execution, image_ea, image_bytes)) stop(StartupStop::InvalidMemory, &batch);
    ps3_lv2_spu_image image{};
    unsigned char raw[4]{};
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    if (memory.read(memory.context, image_ea + image_segs, raw, sizeof(raw)) != PS3_GUEST_MEMORY_OK)
        stop(StartupStop::ProviderFailure, &batch);
    const uint32_t segs_ea = (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) |
        (uint32_t(raw[2]) << 8) | raw[3];
    const uint32_t found = ps3_lv2_spu_image_find(execution.spu_images, segs_ea, &image);
    if (found != PS3_LV2_SPU_OK) {
        cpu.gpr[3] = lv2_spu_abi::spu_sign_extend(found);   // ESRCH: not ours to close
        return;
    }
    const uint32_t closed = ps3_lv2_spu_image_close(execution.spu_images, image.segs_ea);
    if (closed != PS3_LV2_SPU_OK) {
        cpu.gpr[3] = lv2_spu_abi::spu_guest_result(closed, batch);
        return;
    }
    /* The segs array came from the SYSTEM arena, so its slot is what close releases. */
    check_boundary(BoundaryBudget::Poll, &batch);
    const uint32_t released = ps3_lv2_spu_arena_free(execution.spu_images, image.segs_ea);
    check_boundary(BoundaryBudget::Poll, &batch);
    cpu.gpr[3] = lv2_spu_abi::spu_guest_result(released, batch);
    ++active->lv2_spu_returns;
}
#endif

#ifdef PS3_NATIVE_LV2_SPU
/* The SPU thread-group syscalls, on the same StartupExecution-held registries as the imports above.
 * Each arm validates guest pointers first (an EFAULT output writes nothing), publishes trace, and
 * answers with a sign-extended lv2 code; only 173 stops, because starting the group is where phase C1
 * begins. */
static uint32_t spu_read_u32(StartupExecution &execution, uint32_t address)
{
    unsigned char raw[4]{};
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    (void)memory.read(memory.context, address, raw, sizeof(raw));
    return (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) | (uint32_t(raw[2]) << 8) | raw[3];
}
static uint64_t spu_read_u64(StartupExecution &execution, uint32_t address)
{
    unsigned char raw[8]{};
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    (void)memory.read(memory.context, address, raw, sizeof(raw));
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value = value << 8 | raw[index];
    return value;
}
/* Root (2026-11-12): process-wide log of the SPU syscalls (any thread), printed as STARTUP_LV2_SPU_CALL.
 * Diagnostic only; a slot is claimed with one atomic add, entries past the capacity are dropped. */
extern "C++" {
void startup_activity_sample() { activity_sample_impl(); }
void startup_activity_print() { activity_print_impl(); }
void startup_activity_print_calls(unsigned seconds) { activity_print_calls_impl(seconds); }
StartupSpuCallRecord g_startup_spu_call_log[kStartupSpuCallLogCapacity];
std::atomic<uint32_t> g_startup_spu_call_count{0};
}
static void spu_call_log(uint32_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t result)
{
    const uint32_t index = g_startup_spu_call_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= kStartupSpuCallLogCapacity) return;
    g_startup_spu_call_log[index] = StartupSpuCallRecord{number, uint32_t(a0), uint32_t(a1), uint32_t(a2), uint32_t(result)};
}
/* sys_spu_thread_group_start (173): the phase-C1 boundary.
 *
 * Caller branch (lifted libsre, the SPURS kernel bring-up): every cellSpurs/cellSync status check is
 * `if (!((ctx->cr >> N) & 2)) goto fail`, i.e. r3 == 0 is the ONLY success. We therefore never return
 * a non-zero non-negative value: either the guest gets CELL_OK and its SPUs are "started", or it gets
 * a sign-extended CELL error, or the execution stops at this boundary for C1. */
static void dispatch_spu_group_start(StartupExecution &execution, ppu_context &cpu)
{
    using namespace lv2_spu_abi;
    const uint32_t group = uint32_t(cpu.gpr[3]);
    if (active->lv2_spu_calls != SIZE_MAX) ++active->lv2_spu_calls;
    active->lv2_spu_number = lv2_spu_abi::sc_group_start;
    active->lv2_spu_argument = group;
    HleTraceBatch batch;
    if (!lv2_spu_abi::ready(execution)) stop(StartupStop::MissingServices, &batch);
    check_boundary(BoundaryBudget::Poll, &batch);
    const uint32_t code = ps3_lv2_spu_group_start(execution.spu_groups, group);
    if (code != PS3_LV2_SPU_OK) {   // ESRCH / ESTAT / EBUSY: the guest's own error, never a fake start
        cpu.gpr[3] = lv2_spu_abi::spu_guest_result(code, batch);
        spu_call_log(173, group, 0, 0, code);
        ++active->lv2_spu_returns;
        return;
    }
    ps3_lv2_spu_start_snapshot snapshot{};
    if (ps3_lv2_spu_group_snapshot(execution.spu_groups, &snapshot) != PS3_LV2_SPU_OK)
        stop(StartupStop::ProviderFailure, &batch);
    active->lv2_spu_start = snapshot;
    active->lv2_spu_has_start = true;
    active->lv2_spu_last_group = snapshot.group;
    active->lv2_spu_last_num = snapshot.num;
    active->lv2_spu_last_priority = snapshot.priority;
    for (uint32_t index = 0; index < snapshot.num && index < PS3_LV2_SPU_THREADS_MAX; ++index) {
        active->lv2_spu_thread_ids[index] = snapshot.thread_id[index];
        active->lv2_spu_entries[index] = snapshot.entry[index];
        active->lv2_spu_args1[index] = snapshot.arg1[index];
        active->lv2_spu_ls_hashes[index] = snapshot.ls_hash[index];
    }
#ifdef PS3_NATIVE_SPU_RUN
    /* Decision 33 / C1b (root): run every initialized thread on the native SPU interpreter, then return to the PPU.
     * An internal runner code (no record, nothing preparable) is a named stop, never a CELL code in r3. */
    const uint32_t run = ps3_spu_run_start(group);
    LIVE_TRACE("LT spu-run-start group=0x%08x code=0x%08x", group, run);
    active->lv2_spu_run_start_code = run;
    if (run >= 0xFFFF0000u) stop(StartupStop::ProviderFailure, &batch);
    cpu.gpr[3] = run ? spu_sign_extend(run) : spu_cell_ok;
    ++active->lv2_spu_returns;
    spu_call_log(173, group, 0, 0, run);
#else
    /* The local stores are the composition's buffers and outlive this stop, so C1 can execute them. */
    stop(StartupStop::SpuThreadGroupStart, &batch);
#endif
}

/* The remaining SPU syscalls: decode, call the domain, publish any output, sign-extend r3. */
static void dispatch_spu_object(StartupExecution &execution, ppu_context &cpu, uint32_t number)
{
    using namespace lv2_spu_abi;
    HleTraceBatch batch;
    if (active->lv2_spu_calls != SIZE_MAX) ++active->lv2_spu_calls;
    active->lv2_spu_number = number;
    const uint32_t first = uint32_t(cpu.gpr[3]);
    active->lv2_spu_argument = first;
    if (!ready(execution)) stop(StartupStop::MissingServices, &batch);
    const ps3_guest_memory &memory = execution.ppu.memory->coherent_memory;
    uint32_t code = PS3_LV2_SPU_OK;
    const uint64_t log_a1 = cpu.gpr[4], log_a2 = cpu.gpr[5];
    switch (number) {
    case sc_group_create: {
        const uint32_t output = uint32_t(cpu.gpr[3]);
        const uint32_t num = uint32_t(cpu.gpr[4]);
        const uint32_t priority = uint32_t(cpu.gpr[5]);
        const uint32_t attr = uint32_t(cpu.gpr[6]);
        if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t type = 0;
        if (attr) {   // sys_spu_thread_group_attribute: u32 nsize; char name[128]; u32 type; u32 ct
            if (!lv2_spu_abi::spu_readable_range(execution, attr, 8)) stop(StartupStop::InvalidMemory, &batch);
            type = spu_read_u32(execution, attr + 0);   // nsize first; the type is read lazily below
        }
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        begin_trace(execution, 1, batch);
        uint32_t id = 0;
        code = ps3_lv2_spu_group_create(execution.spu_groups, &id, num, priority, type);
        if (code == PS3_LV2_SPU_OK) { lv2_spu_abi::spu_publish_u32(execution, output, id, batch); active->lv2_spu_last_group = id; }
        finish_owned(&batch);
        break;
    }
    case sc_group_destroy:
        code = ps3_lv2_spu_group_destroy(execution.spu_groups, first);
        break;
    case sc_thread_initialize: {
        const uint32_t thread_out = uint32_t(cpu.gpr[3]);
        const uint32_t group = uint32_t(cpu.gpr[4]);
        const uint32_t spu_number = uint32_t(cpu.gpr[5]);
        const uint32_t image_ea = uint32_t(cpu.gpr[6]);
        const uint32_t attr = uint32_t(cpu.gpr[7]);
        const uint32_t arg_ea = uint32_t(cpu.gpr[8]);
        if (!thread_out || !writable(execution, thread_out, 4)) stop(StartupStop::InvalidMemory, &batch);
        if (!image_ea || !lv2_spu_abi::spu_readable_range(execution, image_ea, image_bytes)) stop(StartupStop::InvalidMemory, &batch);
        if (attr && !lv2_spu_abi::spu_readable_range(execution, attr, 8)) stop(StartupStop::InvalidMemory, &batch);
        ps3_lv2_spu_image image{};
        const uint32_t segs_ea = spu_read_u32(execution, image_ea + image_segs);
        const uint32_t found = ps3_lv2_spu_image_find(execution.spu_images, segs_ea, &image);
        if (found != PS3_LV2_SPU_OK) { code = found; break; }
        std::unique_ptr<unsigned char[]> segments(new unsigned char[size_t(image.nsegs) * PS3_LV2_SPU_SEGMENT_BYTES]);
        if (image.nsegs && memory.read(memory.context, segs_ea, segments.get(),
                size_t(image.nsegs) * PS3_LV2_SPU_SEGMENT_BYTES) != PS3_GUEST_MEMORY_OK)
            stop(StartupStop::ProviderFailure, &batch);
        /* The COPY records carry EAs, so the "source" the domain copies from is guest memory itself.
         * Resolve each record's EA into a contiguous scratch buffer the domain can index. */
        uint64_t args[4] = {0, 0, 0, 0};
        if (arg_ea) {
            if (!lv2_spu_abi::spu_readable_range(execution, arg_ea, 32)) stop(StartupStop::InvalidMemory, &batch);
            for (unsigned index = 0; index < 4; ++index) args[index] = spu_read_u64(execution, arg_ea + index * 8);
        }
        if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        begin_trace(execution, 1, batch);
        uint32_t thread = 0;
        code = ps3_lv2_spu_thread_initialize_gather(execution.spu_groups, &thread, group, spu_number, &image,
            segments.get(), image.nsegs, &memory, args);
        if (code == PS3_LV2_SPU_OK) lv2_spu_abi::spu_publish_u32(execution, thread_out, thread, batch);
        finish_owned(&batch);
        break;
    }
    case sc_thread_read_ls: {
        const uint32_t thread = uint32_t(cpu.gpr[3]);
        const uint32_t ls_address = uint32_t(cpu.gpr[4]);
        const uint32_t output = uint32_t(cpu.gpr[5]);
        const uint32_t type = uint32_t(cpu.gpr[6]);
        if (!output || !writable(execution, output, 8)) stop(StartupStop::InvalidMemory, &batch);
        uint64_t value = 0;
        code = ps3_lv2_spu_thread_read_ls(execution.spu_groups, thread, ls_address, &value, type);
        if (code == PS3_LV2_SPU_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            begin_trace(execution, 1, batch);
            const uint64_t encoded = __builtin_bswap64(value);
            if (memory.write(memory.context, output, &encoded, sizeof(encoded)) != PS3_GUEST_MEMORY_OK)
                stop(StartupStop::ProviderFailure, &batch);
            publish_trace(execution, ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, output, 8, value), batch);
            finish_owned(&batch);
        }
        break;
    }
    case sc_thread_write_ls: {
        const uint32_t thread = uint32_t(cpu.gpr[3]);
        const uint32_t ls_address = uint32_t(cpu.gpr[4]);
        const uint32_t value_ea = uint32_t(cpu.gpr[5]);
        const uint32_t type = uint32_t(cpu.gpr[6]);
        if (!value_ea || !lv2_spu_abi::spu_readable_range(execution, value_ea, 8)) stop(StartupStop::InvalidMemory, &batch);
        code = ps3_lv2_spu_thread_write_ls(execution.spu_groups, thread, ls_address,
            spu_read_u64(execution, value_ea), type);
        break;
    }
    case sc_group_set_priority:
        code = ps3_lv2_spu_group_set_priority(execution.spu_groups, first, uint32_t(cpu.gpr[4]));
        break;
    case sc_group_get_priority: {
        const uint32_t output = uint32_t(cpu.gpr[3]);
        if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t priority = 0;
        code = ps3_lv2_spu_group_get_priority(execution.spu_groups, first, &priority);
        if (code == PS3_LV2_SPU_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            begin_trace(execution, 1, batch);
            lv2_spu_abi::spu_publish_u32(execution, output, priority, batch);
            finish_owned(&batch);
        }
        break;
    }
#ifdef PS3_NATIVE_SPU_RUN
    /* Decision 33 / C1b (root): the running group's real outcomes. 178 is sys_spu_thread_group_join(id, u32 *cause,
     * u32 *status); 165 is sys_spu_thread_get_exit_status(id, s32 *status) -- the output is r4 (the B2b arm read r3,
     * which is the thread id). Runner internal codes are named stops. */
    case sc_group_terminate: {
        code = ps3_spu_run_terminate(first);
        if (code >= 0xFFFF0000u) stop(StartupStop::ProviderFailure, &batch);
        if (code == PS3_LV2_SPU_OK) (void)ps3_lv2_spu_group_terminate(execution.spu_groups, first);
        break;
    }
    case sc_group_join: {
        const uint32_t cause_ea = uint32_t(cpu.gpr[4]), status_ea = uint32_t(cpu.gpr[5]);
        if ((cause_ea && !writable(execution, cause_ea, 4)) || (status_ea && !writable(execution, status_ea, 4)))
            stop(StartupStop::InvalidMemory, &batch);
        uint32_t cause = 0, status = 0;
        LIVE_TRACE("LT spu-run-join group=0x%08x", first);
        /* Root cause of the C1b primary ProviderFailure: libsre's start/join thread sleeps HERE, and this wait
         * published nothing, so its creator stayed inside sys_ppu_thread_create until the discovery deadline. A
         * child publishes BLOCKED exactly like the queue receive does (word = its service activity, wake = the
         * shared notify port) and RUNNING again before any stop or return. */
        uint64_t *const join_word = execution.event_queues ? execution.lv2_blocked_word : nullptr;
        if (join_word) {
            __atomic_store_n(join_word, 1, __ATOMIC_SEQ_CST);
            execution.event_queues->port.wake_all(execution.event_queues->port.context);
        }
        code = ps3_spu_run_join(first, &cause, &status);
        if (join_word) __atomic_store_n(join_word, 0, __ATOMIC_SEQ_CST);
        LIVE_TRACE("LT spu-run-joined group=0x%08x code=0x%08x cause=0x%x status=0x%x", first, code, cause, status);
        if (code == PS3_SPU_RUN_INTERNAL_CANCELLED) stop(StartupStop::Cancelled, &batch);
        if (code >= 0xFFFF0000u) stop(StartupStop::ProviderFailure, &batch);
        check_boundary(BoundaryBudget::Poll, &batch);
        if (code == PS3_LV2_SPU_OK) {
            if (!trace_can_begin(execution, 2)) stop(StartupStop::TraceCapacity, &batch);
            begin_trace(execution, 2, batch);
            if (cause_ea) lv2_spu_abi::spu_publish_u32(execution, cause_ea, cause, batch);
            if (status_ea) lv2_spu_abi::spu_publish_u32(execution, status_ea, status, batch);
            finish_owned(&batch);
        }
        break;
    }
    case sc_thread_get_exit_status: {
        const uint32_t output = uint32_t(cpu.gpr[4]);
        if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t status = 0;
        code = ps3_spu_run_exit_status(first, &status);
        if (code == PS3_LV2_SPU_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            begin_trace(execution, 1, batch);
            lv2_spu_abi::spu_publish_u32(execution, output, status, batch);
            finish_owned(&batch);
        }
        break;
    }
#else
    case sc_group_terminate:
        code = ps3_lv2_spu_group_terminate(execution.spu_groups, first);
        break;
    case sc_group_join:
        code = ps3_lv2_spu_group_join(execution.spu_groups, first);
        break;
    case sc_thread_get_exit_status: {
        const uint32_t output = uint32_t(cpu.gpr[3]);
        if (!output || !writable(execution, output, 4)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t status = 0;
        code = ps3_lv2_spu_thread_get_exit_status(execution.spu_groups, first, &status);
        break;
    }
#endif
    case sc_thread_set_argument: {
        const uint32_t arg_ea = uint32_t(cpu.gpr[4]);
        if (!arg_ea || !lv2_spu_abi::spu_readable_range(execution, arg_ea, 32)) stop(StartupStop::InvalidMemory, &batch);
        uint64_t args[4];
        for (unsigned index = 0; index < 4; ++index) args[index] = spu_read_u64(execution, arg_ea + index * 8);
        code = ps3_lv2_spu_thread_set_argument(execution.spu_groups, first, args);
        break;
    }
    case sc_group_connect_event:
        code = ps3_lv2_spu_group_connect_event(execution.spu_groups, first, uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[5]));
        break;
    case sc_group_disconnect_event:
        code = ps3_lv2_spu_group_disconnect_event(execution.spu_groups, first);
        break;
    case sc_thread_connect_event:
        /* Root (2026-11-12): sys_spu_thread_connect_event(id, eq, et, spup) -- r5 is the event type (1 = user), r6 the
         * port. The arm passed r5 as the port and stored it in the group-event table, so no SPU user port was ever
         * connected by this call (the movie decoder's replies went nowhere). */
        if (uint32_t(cpu.gpr[5]) != 1u) { code = PS3_LV2_SPU_EINVAL; break; }
        code = ps3_lv2_spu_thread_user_port(execution.spu_groups, first, uint32_t(cpu.gpr[4]), uint32_t(cpu.gpr[6]));
        break;
    case sc_thread_disconnect_event:   // (id, et, spup)
        if (uint32_t(cpu.gpr[4]) != 1u) { code = PS3_LV2_SPU_EINVAL; break; }
        code = ps3_lv2_spu_thread_user_port(execution.spu_groups, first, 0, uint32_t(cpu.gpr[5]));
        break;
    case sc_group_connect_all: {
        /* Root fix (B2b Eden): sys_spu_thread_group_connect_event_all_threads(u32 id, u32 eq, u64 req,
         * u8 *spup) -- r3 group, r4 queue, r5 request mask, r6 spup (ONE byte). The lifted caller
         * (ppu_recomp_000.cpp:13329-13332) sets exactly these. The worker read the output from r3. */
        const uint32_t output = uint32_t(cpu.gpr[6]);
        if (!output || !writable(execution, output, 1)) stop(StartupStop::InvalidMemory, &batch);
        uint32_t spu_number = 0;
        code = ps3_lv2_spu_group_connect_event_all_threads(execution.spu_groups, first, uint32_t(cpu.gpr[4]),
            cpu.gpr[5], &spu_number);
        if (code == PS3_LV2_SPU_OK) {
            if (!trace_can_begin(execution, 1)) stop(StartupStop::TraceCapacity, &batch);
            begin_trace(execution, 1, batch);
            const unsigned char spup = static_cast<unsigned char>(spu_number);
            lv2_sync::publish_bytes(execution, output, &spup, 1, batch);
            finish_owned(&batch);
        }
        break;
    }
    /* Root (2026-11-12): sys_spu_thread_bind_queue(id, spuq, spuq_num) / unbind(id, spuq_num). The queue the SPU
     * thread's stop 0x110 (sys_spu_thread_receive_event) reads; the runner serves that stop. One queue per thread:
     * spuq_num is not kept (inFAMOUS's movie decoder binds exactly one). */
    case sc_thread_bind_queue:
        code = ps3_lv2_spu_thread_bind_queue(execution.spu_groups, first, uint32_t(cpu.gpr[4]));
        break;
    case sc_thread_unbind_queue:
        code = ps3_lv2_spu_thread_unbind_queue(execution.spu_groups, first);
        break;
    default:
        stop(StartupStop::UnsupportedSyscall, &batch);
    }
    spu_call_log(number, first, log_a1, log_a2, code);
    cpu.gpr[3] = lv2_spu_abi::spu_guest_result(code, batch);
    ++active->lv2_spu_returns;
}
#endif

#ifdef PS3_NATIVE_LIVE_TRACE
static void lv2_syscall_body(ppu_context *cpu);
void lv2_syscall(ppu_context *cpu)
{
    const unsigned long long number = cpu->gpr[11];
    lv2_syscall_body(cpu);
    LIVE_TRACE("LT sc-ret=%llu r3=0x%llx", number, (unsigned long long)cpu->gpr[3]);
}
static void lv2_syscall_body(ppu_context *cpu)
#else
void lv2_syscall(ppu_context *cpu)
#endif
{
    LIVE_TRACE("LT sc=%llu r3=0x%llx r4=0x%llx lr=0x%llx", (unsigned long long)cpu->gpr[11], (unsigned long long)cpu->gpr[3], (unsigned long long)cpu->gpr[4], (unsigned long long)cpu->lr);
    activity_profile::Scope activity_scope(1, uint32_t(cpu->gpr[11]), uint32_t(cpu->lr), cpu->gpr[3], &cpu->gpr[3]);
    check_boundary(BoundaryBudget::LegacyOnly);
    if (cpu != active->cpu) stop(StartupStop::MissingServices);
    active->syscall_number = uint32_t(cpu->gpr[11]);
#if defined(PS3_NATIVE_STARTUP_MEMORY) || defined(PS3_NATIVE_STARTUP_CURRENT_TIME) || defined(PS3_NATIVE_STARTUP_SPU_INITIALIZE) || defined(PS3_NATIVE_STARTUP_THREAD_PRIORITY) || defined(PS3_NATIVE_STARTUP_TTY) || defined(PS3_NATIVE_STARTUP_FS) || defined(PS3_NATIVE_LV2_SYNC) || defined(PS3_NATIVE_LV2_SPU)
    auto *execution = &execution_owner(cpu);
    if (execution->control != active || !execution->ppu.services) stop(StartupStop::MissingServices);
    admit_startup_budget(*execution, infamous::discovery::Category::Hle);
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
    constexpr uint64_t thread_priority_syscall_number = 48;
    if (cpu->gpr[11] == thread_priority_syscall_number) {
        dispatch_thread_priority(*execution, *cpu);
        return;
    }
#endif
#ifdef PS3_NATIVE_STARTUP_SPU_INITIALIZE
    constexpr uint64_t spu_initialize_syscall_number = 169;
    constexpr uint64_t cell_einval_sign_extended = UINT64_C(0xffffffff80010002);
    if (cpu->gpr[11] == spu_initialize_syscall_number) {
        active->spu_initialize_usable = cpu->gpr[3];
        active->spu_initialize_raw = cpu->gpr[4];
        active->spu_initialize_has_result = false;
        if (!execution->spu_resources) stop(StartupStop::MissingServices);
        if (active->spu_initialize_calls != SIZE_MAX) ++active->spu_initialize_calls;
        active->spu_initialize_result = ps3_spu_resource_policy_configure(execution->spu_resources,
            uint32_t(cpu->gpr[3]), uint32_t(cpu->gpr[4]));
        active->spu_initialize_has_result = true;
        // The authority has released its guard. Preserve the real commit before
        // any terminal poll; cancellation suppresses r3 but cannot undo it.
        check_boundary(BoundaryBudget::Poll);
        const auto status = active->spu_initialize_result.status;
        if (status != PS3_SPU_POLICY_OK && status != PS3_SPU_POLICY_INVALID_RAW)
            stop(StartupStop::SpuPolicyFailure);
        cpu->gpr[3] = status == PS3_SPU_POLICY_OK ? 0 : cell_einval_sign_extended;
        if (active->spu_initialize_returns != SIZE_MAX) ++active->spu_initialize_returns;
        return;
    }
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    if (cpu->gpr[11] == 128) { dispatch_event_queue_create(*execution, *cpu); return; }
    if (cpu->gpr[11] == 130) { dispatch_event_receive(*execution, *cpu); return; }
    if (cpu->gpr[11] == 134) { dispatch_event_port_create(*execution, *cpu); return; }
    if (cpu->gpr[11] == 136) { dispatch_event_port_connect(*execution, *cpu); return; }
    if (cpu->gpr[11] == 44) { dispatch_ppu_thread_join(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_LV2_SYNC
    /* Decision 33 / B2a. Placed with the other lv2 arms so a native syscall is answered identically
     * whether the caller is the game or Sony's lifted libsre. The 128/130 arms above win first for
     * those two numbers: they are the composition's own queue service. */
    switch (uint32_t(cpu->gpr[11])) {
    /* Root (B2b Eden trace): sys_process_getpid (1), no arguments. libsre (libsre_func_30010A08,
     * ppu_recomp_000.cpp:19906-19937) caches the pid with -2 meaning "not yet fetched", then passes it
     * to sys_process_get_sdk_version (25). One native process; the conventional first user pid. */
    case 1:
        ++active->lv2_sync_calls;
        active->lv2_sync_number = 1;
        cpu->gpr[3] = lv2_sync::lv2_native_process_id;
        ++active->lv2_sync_returns;
        return;
    case lv2_sync::sc_process_spu_lock_line: dispatch_spu_lock_line(*execution, *cpu); return;
    case lv2_sync::sc_process_get_sdk_version: dispatch_get_sdk_version(*execution, *cpu); return;
    case lv2_sync::sc_memory_get_page_attribute: dispatch_get_page_attribute(*execution, *cpu); return;
    case lv2_sync::sc_timer_usleep: dispatch_timer_usleep(*execution, *cpu); return;
    case 70: case 71: case 72: case 73: case 74: case 75: case 76:
        dispatch_timer(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    case 100: case 101: case 102: case 103: case 104:
        dispatch_lv2_mutex(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    case 43: case 47:
        dispatch_thread_yield_or_priority(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    case lv2_sync::sc_semaphore_create:
    case lv2_sync::sc_semaphore_destroy:
    case lv2_sync::sc_semaphore_wait:
    case lv2_sync::sc_semaphore_trywait:
    case lv2_sync::sc_semaphore_post:
        dispatch_semaphore(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    case lv2_sync::sc_event_flag_create:
    case lv2_sync::sc_event_flag_destroy:
    case lv2_sync::sc_event_flag_wait:
    case lv2_sync::sc_event_flag_trywait:
    case lv2_sync::sc_event_flag_set:
    case lv2_sync::sc_event_flag_clear:
        dispatch_event_flag(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    case lv2_sync::sc_event_queue_destroy:
    case lv2_sync::sc_event_queue_tryreceive:
    case lv2_sync::sc_event_port_destroy:
    case lv2_sync::sc_event_port_disconnect:
    case lv2_sync::sc_event_port_send:
        dispatch_event_object(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    default: break;
    }
#endif
#ifdef PS3_NATIVE_LV2_SPU
    /* Decision 33 / B2b: SPU thread-group syscalls. 173 stops (the phase-C1 boundary); every other
     * one answers the guest. 193/194 stay UnsupportedSyscall: legal lv2, not owned yet. */
    switch (uint32_t(cpu->gpr[11])) {
    case 173: dispatch_spu_group_start(*execution, *cpu); return;
    case 165: case 166: case 170: case 171: case 172: case 177: case 178:
    case 179: case 180: case 181: case 182: case 185: case 186: case 191: case 192:
    case 193: case 194: case 251:
        dispatch_spu_object(*execution, *cpu, uint32_t(cpu->gpr[11])); return;
    default: break;
    }
#endif
#ifdef PS3_NATIVE_STARTUP_TIMEZONE
    if (cpu->gpr[11] == 144) { dispatch_timezone(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_TTY
    if (cpu->gpr[11] == 403) { dispatch_tty_write(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_FS
    if (cpu->gpr[11] == 801 || cpu->gpr[11] == 802 || cpu->gpr[11] == 803 || cpu->gpr[11] == 804 ||
        cpu->gpr[11] == 809 || cpu->gpr[11] == 818 || cpu->gpr[11] == 811 || cpu->gpr[11] == 812 ||
        cpu->gpr[11] == 815 || cpu->gpr[11] == 820) {
        dispatch_fs_syscall(*execution, *cpu, uint32_t(cpu->gpr[11]), false);
        return;
    }
#endif
#ifdef PS3_NATIVE_STARTUP_CURRENT_TIME
    if (cpu->gpr[11] == 145) { dispatch_current_time(*execution, *cpu); return; }
#endif
#ifdef PS3_NATIVE_STARTUP_MEMORY
    const Ps3PpuExecutionServices *services = execution->ppu.services;
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
    if (cpu->gpr[11] != active->syscall_number) stop(StartupStop::UnsupportedSyscall);
    if (active->syscall_number == 147) {
        check_boundary(BoundaryBudget::LegacyOnly);
        cpu->gpr[3] = PS3_GUEST_TIMEBASE_FREQUENCY_HZ;
        return;
    }
#endif
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
        if (!writable(*execution, output, bytes)) {
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
        check_boundary(BoundaryBudget::Poll);
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
    check_boundary(BoundaryBudget::Poll);
    cpu->gpr[3] = uint64_t(int64_t(int32_t(cell)));
    ++active->memory_returns;
    // lv2 ABI: no HLE TOC-slot write, CR/r2/LR remain untouched.
    return;
#else
    stop(StartupStop::UnsupportedSyscall);
#endif
#else
    // The active CPU still belongs to its admitted owner. A disabled budget
    // retains this feature-off route's original UnsupportedSyscall behavior,
    // even if a caller has invalidated the service binding since admission.
    auto &owner = *reinterpret_cast<PpuMemoryExecution *>(cpu);
    if (owner.budget) {
        if (!ppu_memory_binding_valid(owner)) stop(StartupStop::MissingServices);
        auto &execution = *reinterpret_cast<StartupExecution *>(&owner);
        if (execution.control != active) stop(StartupStop::MissingServices);
        admit_startup_budget(execution, infamous::discovery::Category::Hle);
    }
    stop(StartupStop::UnsupportedSyscall);
#endif
}
#endif
uint64_t ppu_timebase_now(void);
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
namespace {
StartupExecution &taskwait_execution()
{
    if (!active || !active->cpu) std::abort();
    check_boundary(BoundaryBudget::Poll);
    auto &execution = execution_owner(active->cpu);
    if (execution.control != active || !execution.ppu.admitted) stop(StartupStop::MissingServices);
    return execution;
}
void taskwait_increment(size_t &counter)
{
    if (counter != SIZE_MAX) ++counter;
}
}
// Decision 123: provided by taskwait_horizon.cpp in the console composition; absent (null) in host verifiers.
extern "C" void startup_taskwait_horizon_watch(uint32_t object) __attribute__((weak));
extern "C" void startup_taskwait_horizon_end(void) __attribute__((weak));
namespace {
// Root diagnostic (2026-11-15, rv62s): which guest objects the waits are on. The generated callers (func_0041FBA8 and the like) hold the
// object in r30 when they call ps3_taskwait_begin. One table per process, filled by the waiting threads, printed by the heartbeat.
struct TaskwaitObject { uint32_t object, lr; uint64_t begins, spins; };
constexpr unsigned TASKWAIT_OBJECTS = 16;
TaskwaitObject g_taskwait_objects[TASKWAIT_OBJECTS];
std::atomic_flag g_taskwait_objects_lock = ATOMIC_FLAG_INIT;
thread_local int g_taskwait_current = -1;
void taskwait_note_begin(const ppu_context &cpu)
{
    const uint32_t object = uint32_t(cpu.gpr[30]), lr = uint32_t(cpu.lr);
    while (g_taskwait_objects_lock.test_and_set(std::memory_order_acquire)) {}
    int slot = -1;
    for (unsigned i = 0; i < TASKWAIT_OBJECTS; ++i)
        if (g_taskwait_objects[i].begins && g_taskwait_objects[i].object == object && g_taskwait_objects[i].lr == lr) { slot = int(i); break; }
    if (slot < 0) for (unsigned i = 0; i < TASKWAIT_OBJECTS; ++i) if (!g_taskwait_objects[i].begins) { slot = int(i); g_taskwait_objects[i] = {object, lr, 0, 0}; break; }
    if (slot >= 0) ++g_taskwait_objects[slot].begins;
    g_taskwait_objects_lock.clear(std::memory_order_release);
    g_taskwait_current = slot;
    if (startup_taskwait_horizon_watch) startup_taskwait_horizon_watch(object);
}
}
extern "C" void startup_taskwait_print_objects(unsigned seconds)
{
    for (unsigned i = 0; i < TASKWAIT_OBJECTS; ++i) {
        const TaskwaitObject &o = g_taskwait_objects[i];
        if (o.begins) std::printf("STARTUP_TASKWAIT_OBJECT seconds=%u object=0x%08x lr=0x%08x begins=%llu spins=%llu\n", seconds, unsigned(o.object), unsigned(o.lr),
                                  (unsigned long long)o.begins, (unsigned long long)__atomic_load_n(&o.spins, __ATOMIC_RELAXED));
    }
}
extern "C" void ps3_taskwait_begin(void)
{
    auto &begin_execution = taskwait_execution();
    taskwait_note_begin(begin_execution.ppu.cpu);
    auto &observation = begin_execution.taskwait_observation;
    if (observation.depth == UINT32_MAX) {
        observation.failure = StartupTaskwaitFailure::DepthOverflow;
        stop(StartupStop::TaskwaitStop);
    }
    taskwait_increment(observation.begins);
    ++observation.depth;
    if (observation.max_depth < observation.depth) observation.max_depth = observation.depth;
}
extern "C" void ps3_taskwait_spin(void)
{
    auto &execution = taskwait_execution();
    auto &observation = execution.taskwait_observation;
    taskwait_increment(observation.spins);
    if (g_taskwait_current >= 0) __atomic_fetch_add(&g_taskwait_objects[g_taskwait_current].spins, 1u, __ATOMIC_RELAXED);
    if (!execution.taskwait.yield_once) {
        observation.failure = StartupTaskwaitFailure::MissingProvider;
        stop(StartupStop::TaskwaitStop);
    }
    taskwait_increment(observation.pauses_attempted);
    const bool returned = execution.taskwait.yield_once(execution.taskwait.context);
    if (returned) taskwait_increment(observation.pauses_completed);
    else observation.failure = StartupTaskwaitFailure::ProviderFailure;
    // A completed pause is retained even when cancellation/deadline became visible during it.
    check_boundary(BoundaryBudget::Poll);
    if (!returned) stop(StartupStop::TaskwaitStop);
}
extern "C" void ps3_taskwait_end(void)
{
    auto &observation = taskwait_execution().taskwait_observation;
    if (!observation.depth) {
        observation.failure = StartupTaskwaitFailure::UnbalancedEnd;
        stop(StartupStop::TaskwaitStop);
    }
    taskwait_increment(observation.ends);
    --observation.depth;
    if (startup_taskwait_horizon_end) startup_taskwait_horizon_end();
}
#endif
void native_startup_worker(void *argument)
{
    auto *execution = static_cast<StartupExecution *>(argument);
    active = execution->control;
    if (!active || !execution->ppu.admitted) std::abort();
    active->cpu = &execution->ppu.cpu;
    g_trampoline_fn = nullptr;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
    if (active->entry == StartupEntry::IndirectCall) {
        ps3_indirect_call(&execution->ppu.cpu);
        /* A thread entry may be a tail-call thunk (game func_004C9A88 -> func_00316720): the lifted code leaves the
         * real body in g_trampoline_fn and returns, exactly as it does at every generated call site, where
         * DRAIN_TRAMPOLINE runs it. Without this the thread "returned" before its body ran. */
        while (g_trampoline_fn) {
            void (*const pending)(void *) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            pending(&execution->ppu.cpu);
        }
    }
    else
#endif
    if (active->entry == StartupEntry::MemorySc32)
        execution->ppu.cpu.gpr[3] = ppu_native_store_conditional32(&execution->ppu.cpu,
            execution->ppu.cpu.gpr[4], uint32_t(execution->ppu.cpu.gpr[5]));
    else if (active->entry == StartupEntry::TlsStub) func_0055DA64(&execution->ppu.cpu);
#ifdef PS3_NATIVE_STARTUP_LWMUTEX
    else if (active->entry == StartupEntry::MutexCreateStub) func_0055D9E4(&execution->ppu.cpu);
    else if (active->entry == StartupEntry::MutexLockStub) func_0055D924(&execution->ppu.cpu);
    else if (active->entry == StartupEntry::MutexTryStub) func_0055DAE4(&execution->ppu.cpu);
    else if (active->entry == StartupEntry::MutexUnlockStub) func_0055D944(&execution->ppu.cpu);
    else if (active->entry == StartupEntry::MutexDestroyStub) func_0055DB24(&execution->ppu.cpu);
#ifdef PS3_NATIVE_STARTUP_MEMORY
    else if (active->entry == StartupEntry::MemorySyscall) lv2_syscall(&execution->ppu.cpu);
#ifdef PS3_NATIVE_STARTUP_CLOCK
    else if (active->entry == StartupEntry::ClockStub) func_0055DA84(&execution->ppu.cpu);
#endif
#endif
#endif
    else if (active->entry == StartupEntry::UnsupportedExternal) (void)ppu_timebase_now();
    else func_00010230(&execution->ppu.cpu);
    stop(StartupStop::Returned);
}
}
