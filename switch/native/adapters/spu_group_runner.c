/* Decision 33 / C1b: run one lv2 SPU thread group on the C1a interpreter.
 *
 * Each initialized thread of the group gets one interpreter owner, borrowing that thread's local store from the lv2
 * SPU domain and the composition's atomic domain, so its DMA and atomics observe exactly the memory the PPU does.
 * The owner runs on its own host thread through the composition's service-thread port.
 *
 * The worker is a loop of slices (ps3_spu_executor_run_interp). A slice ends in one of four ways:
 *   BUDGET   -- the slice ran out: poll cancellation, publish progress, slice again;
 *   PAUSED   -- a channel the interpreter cannot satisfy alone (an empty in-mailbox, a full out mailbox, a blocking
 *               RdEventStat). Sleep a bounded moment, poll cancellation, retry: the PPU (or a peer SPU) is what
 *               changes the condition, and the owner never spins without a cancellation point;
 *   STOPPED  -- an lv2 stop code: serve it, or record the thread's terminal;
 *   FAULT    -- a named terminal (an unsupported channel, a memory failure, a halt): record it and end the thread.
 *
 * Nothing is invented. Every exit is either a stop the SPU itself executed or a terminal this module records.
 */
#include "spu_group_runner.h"
#include "spu_run_port.h"
#ifdef PS3_SPU_LIFTED_BINK
#include "../spu_lifted/bink/bink_lifted.h"
#endif
#if (defined(PS3_SPU_LIFTED_JOBS) || defined(PS3_SPU_LIFTED_KERNEL)) && !defined(PS3_SPU_IMAGE_ACCOUNT)
#define PS3_SPU_IMAGE_ACCOUNT 1   /* the job layer needs the region map the accounting unit owns */
#endif
#ifdef PS3_SPU_IMAGE_ACCOUNT
#include "spu_image_account.h"
#endif
#ifdef PS3_SPU_LIFTED_JOBS
#include "../spu_lifted/jobs/jobmods_table.h"
#endif
#ifdef PS3_SPU_LIFTED_KERNEL
#include "../spu_lifted/kernel/spurs_kernel.h"
#include "../spu_lifted/kernel/wws_restore_stub.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* SPU-to-PPU user events. The wire format is the desktop reference's, not a guess:
 *   source/ps3recomp/runtime/spu/spu_channels.c:1501-1508 -- on SPU_WrOutIntrMbox the hook sees the raw channel word;
 *   source/ps3recomp/runtime/syscalls/lv2_register.c:1846-1886 (spu_deliver_user_event) -- it decodes
 *     code = value >> 24, port = code & 63, data0 = the pending SPU_WrOutMbox word, and pushes
 *       source 0xFFFFFFFF53505501, data1 spu id, data2 ((u64)port << 32) | (value & 0xFFFFFF), data3 data0,
 *     then writes CELL_OK (0) / CELL_ENOTCONN / CELL_EBUSY into the in-mailbox. */
static const uint64_t SPU_USER_EVENT_KEY = UINT64_C(0xFFFFFFFF53505501);
enum {
    SPU_EVENT_SPUP_SHIFT = 24,
    SPU_EVENT_SPUP_FIELD = 0xFF,
    SPU_EVENT_SPUP_MASK = 63,
    SPU_CELL_OK = 0,
    SPU_CELL_EBUSY = 0x8001000Au,
    SPU_CELL_ENOTCONN = 0x80010016u,
    SPU_STOP_GROUP_EXIT = 0x101,
    SPU_STOP_THREAD_EXIT = 0x102,
    SPU_STOP_RECEIVE_EVENT = 0x110
};


static void publish_state(ps3_spu_thread_run *run, unsigned state)
{
    /* Release: every field the owner might read is written before the state it belongs to. */
    __atomic_store_n(&run->state, state, __ATOMIC_RELEASE);
}

static int is_cancelled(const ps3_spu_group_runner *runner, const ps3_spu_thread_run *run)
{
    if (__atomic_load_n(&run->stop_requested, __ATOMIC_ACQUIRE)) return 1;
    const unsigned *flag = runner->services.cancel_flag;
    return flag && __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

enum { LATE_TRACE_LINES = 48 };
static char g_late_trace[LATE_TRACE_LINES][192];
static unsigned g_late_trace_count;
static void trace_line(const ps3_spu_group_runner *runner, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void trace_line(const ps3_spu_group_runner *runner, const char *format, ...)
{
    char line[192];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    /* Root diagnostic (2026-11-12): the first lines of every group after the two SPURS groups are kept and printed
     * with the thread results (the live trace is off in normal builds). */
    if (runner->group_id >= 0x9E000200u) {
        const unsigned index = __atomic_fetch_add(&g_late_trace_count, 1u, __ATOMIC_RELAXED);
        if (index < LATE_TRACE_LINES) memcpy(g_late_trace[index], line, sizeof line);
    }
    if (!runner->services.trace) return;
    runner->services.trace(runner->services.context, line);
}

uint64_t g_ps3_spu_run_ns[8], g_ps3_spu_run_instructions[8];   /* Decision 99, see run_slice */

/* Decision 135 (2026-11-16): plain SPU DMA copies directly from/to the flat guest window where the atomic domain proves it safe
 * (ps3_atomic_dma_read / _write). The resolver is the flat guest space's lock-free committed-range pointer (the one the renderer and the PPU's
 * lean accesses use); with no flat space it answers NULL and every DMA is guarded as before. -DPS3_SPU_LOCKFREE_DMA=0: not installed. */
#ifndef PS3_SPU_LOCKFREE_DMA
#define PS3_SPU_LOCKFREE_DMA 1
#endif
#if defined(__SWITCH__) && PS3_SPU_LOCKFREE_DMA
extern const void *ps3_flat_guest_fast_range(uint32_t address, uint64_t length);   /* guest_space_flat_horizon.h (not included: libnx types) */
static const void *direct_dma_range(void *context, uint32_t address, uint64_t length) { (void)context; return ps3_flat_guest_fast_range(address, length); }
#define SPU_DIRECT_DMA_INSTALL(domain) ps3_atomic_domain_set_direct((domain), direct_dma_range, NULL)
#else
#define SPU_DIRECT_DMA_INSTALL(domain) ((void)(domain))
#endif

/* Decision 134 (2026-11-16, after console run rv69s): where does a SPURS job thread's time go? Per SPU thread slot (thread id bits 8..10):
 *   memory operations (every DMA and atomic command, timed from the executor's reserve_trace to its finish_trace: the atomic domain's guard is taken and released
 *   in between, so this is lock wait + copy), by class: GET, PUT, GETLLAR, PUTLLC/PUTLLUC; count, total, longest;
 *   idle waits (the parked kernel's RdEventStat pause, outside the slice), count and total;
 *   the thread's own CPU time (Horizon thread tick count), against the wall time the slices took (g_ps3_spu_run_ns): wall - cpu inside slices = blocked or not scheduled.
 * Process-wide: how many of the five job threads (slots 0..4) are inside a slice at once (time-weighted), and the atomic domain guard's word sampled at every slice
 * start (held, announced waiters). Printed by ps3_spu_run_profile_report (STARTUP_SPU_PROFILE / _CONCURRENCY / _DOMAIN_LOCK). Relaxed atomics; each slot has one writer. */
enum { PROF_SLOTS = 8, PROF_CLASSES = 4, PROF_JOB_THREADS = 5, PROF_CPU_EVERY = 64 };
typedef struct {
    uint64_t mem_n[PROF_CLASSES], mem_ns[PROF_CLASSES], mem_max_ns[PROF_CLASSES], mem_bytes[PROF_CLASSES];
    uint64_t idle_n, idle_ns, cpu_ticks;
    uint64_t dma[PS3_ATOMIC_DMA_COUNTS];   /* Decision 135: direct / guarded bytes, redone runs, no-direct DMAs (ps3_atomic_dma_take_counts) */
} spu_prof_slot;
static spu_prof_slot g_prof[PROF_SLOTS];
static __thread unsigned t_prof_slot = PROF_SLOTS;          /* this worker's slot (PROF_SLOTS: not a worker) */
static __thread uint64_t t_prof_mem_begin_ns;
static __thread unsigned t_prof_mem_class, t_prof_slices;
static uint32_t g_conc_lock;
static unsigned g_conc_busy;
static uint64_t g_conc_last_ns, g_conc_ns[PROF_JOB_THREADS + 1];
static uint64_t g_lock_samples, g_lock_held, g_lock_waiters;
static uint64_t prof_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void prof_max(uint64_t *slot, uint64_t value) {
    uint64_t seen = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (value > seen && !__atomic_compare_exchange_n(slot, &seen, value, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}
/* One of the five job threads enters (+1) or leaves (-1) a slice: the time since the last change goes to the bucket of the old count. */
static void prof_concurrency(int delta, uint64_t now) {
    if (t_prof_slot >= PROF_JOB_THREADS) return;
    while (__atomic_exchange_n(&g_conc_lock, 1u, __ATOMIC_ACQUIRE)) { }
    if (g_conc_last_ns && g_conc_busy <= PROF_JOB_THREADS) g_conc_ns[g_conc_busy] += now - g_conc_last_ns;
    g_conc_last_ns = now;
    g_conc_busy = (unsigned)((int)g_conc_busy + delta);
    __atomic_store_n(&g_conc_lock, 0u, __ATOMIC_RELEASE);
}
static void prof_mem_begin(const ps3_spu_transfer *transfer) {
    if (t_prof_slot >= PROF_SLOTS) return;
    const uint32_t cmd = transfer->command;
    t_prof_mem_class = cmd == MFC_GETLLAR_CMD ? 2u : (cmd == MFC_PUTLLC_CMD || cmd == MFC_PUTLLUC_CMD) ? 3u : transfer->write ? 1u : 0u;
    t_prof_mem_begin_ns = prof_now_ns();
}
static void prof_mem_end(const ps3_spu_transfer *transfer) {
    if (t_prof_slot >= PROF_SLOTS || !t_prof_mem_begin_ns) return;
    const uint64_t spent = prof_now_ns() - t_prof_mem_begin_ns;
    spu_prof_slot *s = &g_prof[t_prof_slot];
    const unsigned c = t_prof_mem_class;
    __atomic_store_n(&s->mem_n[c], s->mem_n[c] + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->mem_ns[c], s->mem_ns[c] + spent, __ATOMIC_RELAXED);
    __atomic_store_n(&s->mem_bytes[c], s->mem_bytes[c] + transfer->bytes, __ATOMIC_RELAXED);
    prof_max(&s->mem_max_ns[c], spent);
    t_prof_mem_begin_ns = 0;
}
#if defined(__SWITCH__)
extern uint32_t svcGetInfo(uint64_t *out, uint32_t id0, uint32_t handle, uint64_t id1);
enum { PROF_INFO_THREAD_TICKS = 25u, PROF_INFO_THREAD_TICKS_OLD = 0xF0000002u, PROF_CURRENT_THREAD = 0xFFFF8000u };
static void prof_cpu_sample(void) {
    uint64_t ticks = 0;
    if (svcGetInfo(&ticks, PROF_INFO_THREAD_TICKS, PROF_CURRENT_THREAD, UINT64_MAX) && svcGetInfo(&ticks, PROF_INFO_THREAD_TICKS_OLD, PROF_CURRENT_THREAD, UINT64_MAX)) return;
    __atomic_store_n(&g_prof[t_prof_slot].cpu_ticks, ticks, __ATOMIC_RELAXED);
}
#else
static void prof_cpu_sample(void) { }
#endif
/* At every slice start: the domain guard's word (bit 0 held, bits 31..1 announced waiters, barging_lock_horizon.h). */
static void prof_lock_sample(const ps3_atomic_domain *domain) {
    if (!domain || !domain->guard.context) return;
    const uint32_t word = __atomic_load_n((const uint32_t *)domain->guard.context, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_lock_samples, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_lock_held, word & 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_lock_waiters, word >> 1, __ATOMIC_RELAXED);
}
void ps3_spu_run_profile_report(void (*print)(const char *line), unsigned seconds)
{
    if (!print) return;
    char line[900];
    for (unsigned k = 0; k < PROF_SLOTS; ++k) {
        const spu_prof_slot *s = &g_prof[k];
        const uint64_t wall = __atomic_load_n(&g_ps3_spu_run_ns[k], __ATOMIC_RELAXED);
        if (!wall && !s->idle_n) continue;
        size_t used = (size_t)snprintf(line, sizeof line, "STARTUP_SPU_PROFILE seconds=%u slot=%u slice_ms=%llu cpu_ms=%llu idle_waits=%llu idle_ms=%llu", seconds, k,
                                       (unsigned long long)(wall / 1000000u), (unsigned long long)(__atomic_load_n(&s->cpu_ticks, __ATOMIC_RELAXED) * 1000u / 19200000u),
                                       (unsigned long long)s->idle_n, (unsigned long long)(s->idle_ns / 1000000u));
        static const char *const names[PROF_CLASSES] = { "get", "put", "getllar", "putllc" };
        for (unsigned c = 0; c < PROF_CLASSES && used < sizeof line; ++c)
            used += (size_t)snprintf(line + used, sizeof line - used, " %s=%llu/%llums/%lluKB/max%lluus", names[c], (unsigned long long)s->mem_n[c],
                                     (unsigned long long)(s->mem_ns[c] / 1000000u), (unsigned long long)(s->mem_bytes[c] >> 10), (unsigned long long)(s->mem_max_ns[c] / 1000u));
        if (used < sizeof line)   /* Decision 135: how the plain DMA was copied */
            used += (size_t)snprintf(line + used, sizeof line - used, " dma_direct_get=%lluKB dma_direct_put=%lluKB dma_guarded_get=%lluKB dma_guarded_put=%lluKB redo_get=%llu redo_put=%llu no_direct=%llu",
                                     (unsigned long long)(s->dma[PS3_ATOMIC_DMA_DIRECT_GET] >> 10), (unsigned long long)(s->dma[PS3_ATOMIC_DMA_DIRECT_PUT] >> 10),
                                     (unsigned long long)(s->dma[PS3_ATOMIC_DMA_GUARDED_GET] >> 10), (unsigned long long)(s->dma[PS3_ATOMIC_DMA_GUARDED_PUT] >> 10),
                                     (unsigned long long)s->dma[PS3_ATOMIC_DMA_REDO_GET], (unsigned long long)s->dma[PS3_ATOMIC_DMA_REDO_PUT], (unsigned long long)s->dma[PS3_ATOMIC_DMA_NO_DIRECT]);
        if (used < sizeof line)   /* Decision 136: waits for the atomic domain's guard (every GETLLAR/PUTLLC and every guarded DMA run takes it) */
            used += (size_t)snprintf(line + used, sizeof line - used, " guard=%llu/%llums/max%lluus spun_out=%llu",
                                     (unsigned long long)s->dma[PS3_ATOMIC_GUARD_ACQUIRES], (unsigned long long)(s->dma[PS3_ATOMIC_GUARD_WAIT_NS] / 1000000u),
                                     (unsigned long long)(s->dma[PS3_ATOMIC_GUARD_WAIT_MAX_NS] / 1000u), (unsigned long long)s->dma[PS3_ATOMIC_GUARD_SPUN_OUT]);
        print(line);
    }
    while (__atomic_exchange_n(&g_conc_lock, 1u, __ATOMIC_ACQUIRE)) { }
    uint64_t conc[PROF_JOB_THREADS + 1];
    for (unsigned k = 0; k <= PROF_JOB_THREADS; ++k) conc[k] = g_conc_ns[k];
    __atomic_store_n(&g_conc_lock, 0u, __ATOMIC_RELEASE);
    snprintf(line, sizeof line, "STARTUP_SPU_CONCURRENCY seconds=%u in_slice_job_threads: 0=%llums 1=%llums 2=%llums 3=%llums 4=%llums 5=%llums", seconds,
             (unsigned long long)(conc[0] / 1000000u), (unsigned long long)(conc[1] / 1000000u), (unsigned long long)(conc[2] / 1000000u),
             (unsigned long long)(conc[3] / 1000000u), (unsigned long long)(conc[4] / 1000000u), (unsigned long long)(conc[5] / 1000000u));
    print(line);
    const uint64_t n = __atomic_load_n(&g_lock_samples, __ATOMIC_RELAXED);
    snprintf(line, sizeof line, "STARTUP_SPU_DOMAIN_LOCK seconds=%u samples=%llu held_permille=%llu mean_waiters_x100=%llu (sampled at every SPU slice start)", seconds,
             (unsigned long long)n, (unsigned long long)(n ? __atomic_load_n(&g_lock_held, __ATOMIC_RELAXED) * 1000u / n : 0u),
             (unsigned long long)(n ? __atomic_load_n(&g_lock_waiters, __ATOMIC_RELAXED) * 100u / n : 0u));
    print(line);
}
static void sleep_microseconds(uint32_t microseconds)
{
    struct timespec request;
    request.tv_sec = (time_t)(microseconds / 1000000u);
    request.tv_nsec = (long)((microseconds % 1000000u) * 1000u);
    nanosleep(&request, NULL);
}

/* Root (2026-11-15, Decision 116): an SPU waiting on SPU_RdEventStat (the SPURS kernel's idle wait for a lock-line
 * reservation loss, i.e. new work) slept the whole pause (200 us) before it looked again, so every job the PPU pushed
 * waited on average 100 us before an SPU saw it (rv57s: the five job threads ~42 % asleep while the game's main thread
 * waited ~21 % for their jobs). The atomic domain now calls loss_hook whenever a write latches a reservation loss; an
 * idle worker waits on g_spu_loss_word with the same 200 us ceiling and wakes at once. Nothing signals: same as before.
 * PS3_SPU_LOSS_WAKE=0 turns it off. */
#ifndef PS3_SPU_LOSS_WAKE
#define PS3_SPU_LOSS_WAKE 1
#endif
#if defined(__SWITCH__) && PS3_SPU_LOSS_WAKE
/* libnx's switch/types.h clashes with the SPU headers' u128, so the two calls are declared here as libnx declares them
 * (switch/kernel/svc.h: Result = u32; ArbitrationType_WaitIfEqual = 2, SignalType_Signal = 0). */
extern uint32_t svcWaitForAddress(void *address, uint32_t arb_type, int64_t value, int64_t timeout);
extern uint32_t svcSignalToAddress(void *address, uint32_t signal_type, int32_t value, int32_t count);
enum { SPU_LOSS_ARB_WAIT_IF_EQUAL = 2, SPU_LOSS_SIGNAL = 0 };
static uint32_t g_spu_loss_word, g_spu_loss_waiters;
uint64_t g_ps3_spu_loss_signals, g_ps3_spu_loss_waits, g_ps3_spu_loss_woken;   /* readable by the composition's report */
static void loss_hook(void)
{
    __atomic_fetch_add(&g_spu_loss_word, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_spu_loss_waiters, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_add(&g_ps3_spu_loss_signals, 1u, __ATOMIC_RELAXED);
        (void)svcSignalToAddress(&g_spu_loss_word, SPU_LOSS_SIGNAL, 0, -1);
    }
}
static void idle_wait_microseconds(uint32_t microseconds, uint32_t channel)
{
    if (channel != SPU_RdEventStat || !microseconds) { sleep_microseconds(microseconds); return; }
    const uint32_t seen = __atomic_load_n(&g_spu_loss_word, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_spu_loss_waiters, 1u, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&g_ps3_spu_loss_waits, 1u, __ATOMIC_RELAXED);
    const uint32_t rc = svcWaitForAddress(&g_spu_loss_word, SPU_LOSS_ARB_WAIT_IF_EQUAL, (int64_t)(int32_t)seen, (int64_t)microseconds * 1000);
    __atomic_fetch_sub(&g_spu_loss_waiters, 1u, __ATOMIC_SEQ_CST);
    if (rc == 0u || __atomic_load_n(&g_spu_loss_word, __ATOMIC_RELAXED) != seen) __atomic_fetch_add(&g_ps3_spu_loss_woken, 1u, __ATOMIC_RELAXED);
}
#define SPU_LOSS_HOOK_INSTALL() (ps3_atomic_reservation_loss_hook = loss_hook)
#else
static void idle_wait_microseconds(uint32_t microseconds, uint32_t channel) { (void)channel; sleep_microseconds(microseconds); }
#define SPU_LOSS_HOOK_INSTALL() ((void)0)
#endif

/* Decision 134: an idle wait (the paused thread's sleep outside its slice), timed for STARTUP_SPU_PROFILE. */
static void idle_wait_timed(uint32_t microseconds, uint32_t channel)
{
    const uint64_t t0 = t_prof_slot < PROF_SLOTS ? prof_now_ns() : 0u;
    idle_wait_microseconds(microseconds, channel);
    if (!t0) return;
    spu_prof_slot *s = &g_prof[t_prof_slot];
    __atomic_store_n(&s->idle_n, s->idle_n + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s->idle_ns, s->idle_ns + (prof_now_ns() - t0), __ATOMIC_RELAXED);
}

/* Round 2 (freeze diagnostics): the stall streak of a worker, for the monitor thread. A streak is one channel retried within 50 ms of the last retry. */
static uint64_t timespec_ns(const struct timespec *t) { return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec; }
/* A thread that found work after a spurious wake runs far more than the kernel's idle loop; this many instructions before it parks again counts as "found work". */
enum { SPURIOUS_WORK_INSTRUCTIONS = 2000 };
static void maybe_spurious_lr(ps3_spu_group_runner *runner, ps3_spu_thread_run *run, uint64_t now)
{
    ps3_spu_executor *o = &run->executor;
    spu_context *c = &o->core;
    const uint64_t instructions = __atomic_load_n(&run->instructions, __ATOMIC_RELAXED);
    if (run->spurious_outstanding) {          /* what the last spurious wake cost: the instructions run since it, up to this park */
        const uint64_t cost = instructions - run->spurious_mark;
        run->spurious_instr_total += cost;
        if (cost > SPURIOUS_WORK_INSTRUCTIONS) ++run->spurious_useful;
        run->spurious_outstanding = 0;
    }
    if (!(c->event_mask & PS3_SPU_EVENT_LOCK_LINE_LOST) || (c->event_status & c->event_mask)) return;
    const uint64_t bound = (uint64_t)runner->services.spurious_lr_us * 1000ull;
    if (now - __atomic_load_n(&run->wait_since_ns, __ATOMIC_RELAXED) < bound || now - run->spurious_last_ns < bound) return;
    (void)ps3_atomic_owner_clear_reservation(o->memory.domain, o->memory.owner);   /* as after a real loss: the next putllc fails */
    c->event_status |= PS3_SPU_EVENT_LOCK_LINE_LOST;                               /* the bit native_latch_reservation_loss sets */
    run->spurious_last_ns = now; run->spurious_mark = instructions; run->spurious_outstanding = 1;
    __atomic_fetch_add(&run->spurious_lr, 1u, __ATOMIC_RELAXED);
}
static void note_wait(ps3_spu_thread_run *run, uint32_t channel, uint64_t now)
{
    const uint64_t last = __atomic_load_n(&run->wait_last_ns, __ATOMIC_RELAXED);
    if (!last || now - last > 50000000ull || __atomic_load_n(&run->wait_channel, __ATOMIC_RELAXED) != channel) {
        __atomic_store_n(&run->wait_channel, channel, __ATOMIC_RELAXED);
        __atomic_store_n(&run->wait_since_ns, now, __ATOMIC_RELAXED);
        __atomic_store_n(&run->wait_reported, 0u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&run->wait_last_ns, now, __ATOMIC_RELAXED);
}

static uint32_t queue_for_spup(const ps3_lv2_spu_thread *thread, uint32_t spup)
{
    return thread->spup_queue[spup & SPU_EVENT_SPUP_MASK];
}

/* Turns one SPU_WrOutIntrMbox value into a queue event and returns the CELL result the SPU may read back. */
static uint32_t deliver_user_event(ps3_spu_group_runner *runner, ps3_spu_thread_run *run, uint32_t value)
{
    const uint32_t spup = (value >> SPU_EVENT_SPUP_SHIFT) & SPU_EVENT_SPUP_FIELD;
    const uint32_t queue = queue_for_spup(run->thread, spup);
    if (!queue) {
        trace_line(runner, "SPU_EVENT thread=0x%08X spup=%u queue=none", run->thread->id, spup);
        __atomic_fetch_add(&run->events_notconn, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&run->last_event_spup, spup, __ATOMIC_RELAXED);
        return SPU_CELL_ENOTCONN;
    }
    /* data0 is the plain mailbox word the SPU wrote just before this one. No word means no event: never invent one. */
    uint32_t data0 = 0;
    if (ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT, &data0) != PS3_SPU_EXEC_OK)
        return SPU_CELL_ENOTCONN;
    const uint32_t code = ps3_lv2_queue_send(runner->services.objects, queue, SPU_USER_EVENT_KEY, run->thread->id,
                                             ((uint64_t)spup << 32) | (value & 0xFFFFFFu), data0);
    __atomic_fetch_add(&run->events_sent, 1u, __ATOMIC_RELAXED);
    if (code != PS3_LV2_OK) __atomic_fetch_add(code == PS3_LV2_EBUSY ? &run->events_busy : &run->events_notconn, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&run->last_event_queue, queue, __ATOMIC_RELAXED); __atomic_store_n(&run->last_event_spup, spup, __ATOMIC_RELAXED);
    __atomic_store_n(&run->last_event_data0, data0 & 0xFFFFFFu, __ATOMIC_RELAXED);
    trace_line(runner, "SPU_EVENT thread=0x%08X spup=%u queue=0x%08X data0=0x%06X result=0x%08X",
               run->thread->id, spup, queue, data0 & 0xFFFFFFu, code);
    if (code == PS3_LV2_OK) return SPU_CELL_OK;
    if (code == PS3_LV2_EBUSY) return SPU_CELL_EBUSY;
    return SPU_CELL_ENOTCONN;
}

/* Takes a pending SPU_WrOutIntrMbox value and delivers it. The SPU does NOT block on that write (the interrupt mailbox
 * is one deep, so the first write succeeds), so this is polled at every slice boundary: the reference
 * (spu_channels.c:1505) delivers on the write itself, and a slice boundary is the interpreter's equivalent point.
 * Returns 1 when a value was taken and delivered (or refused with a reply written). */
static int serve_out_interrupt_mailbox(ps3_spu_group_runner *runner, ps3_spu_thread_run *run)
{
    uint32_t pending = 0;
    if (ps3_spu_executor_mbox_pending(&run->executor, PS3_SPU_MAILBOX_OUT_INTR, &pending) != PS3_SPU_EXEC_OK || !pending)
        return 0;
    uint32_t value = 0;
    if (ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT_INTR, &value) != PS3_SPU_EXEC_OK) return 0;
    /* Root (2026-11-12): the four lv2 forms of this word, by its top byte (RPCS3 SPUThread.cpp, SPU_WrOutIntrMbox of a
     * thread SPU): below 64 sys_spu_thread_send_event (one reply word); 64..127 sys_spu_thread_throw_event (NO reply);
     * 128 sys_event_flag_set_bit (the out-mailbox word is the flag id; one reply word); 192 its impatient form (no
     * reply). Every form consumes the out-mailbox word. Before this every form got a reply and an unconnected throw
     * left its out-mailbox word behind: inFAMOUS's movie decoder then blocked on its next out-mailbox write, and the
     * SPURS threads collected stale reply words. */
    const uint32_t code = value >> SPU_EVENT_SPUP_SHIFT;
    if (code < 64u) {
        const uint32_t result = deliver_user_event(runner, run, value);
        if (result == SPU_CELL_ENOTCONN) { uint32_t unused = 0; (void)ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT, &unused); }
        (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, result);
    } else if (code < 128u) {
        const uint32_t result = deliver_user_event(runner, run, value);
        if (result == SPU_CELL_ENOTCONN) { uint32_t unused = 0; (void)ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT, &unused); }
    } else if (code == 128u || code == 192u) {
        uint32_t flag = 0;
        uint32_t result = 0x80010002u;
        if (ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT, &flag) == PS3_SPU_EXEC_OK && (value & 0xFFFFFFu) < 64u)
            result = ps3_lv2_event_flag_set(runner->services.objects, flag, UINT64_C(1) << (value & 0xFFFFFFu));
        trace_line(runner, "SPU_FLAG thread=0x%08X flag=0x%08X bit=%u result=0x%08X", run->thread->id, flag, value & 0xFFFFFFu, result);
        if (code == 128u) (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, result);
    } else {
        trace_line(runner, "SPU_INTR_UNKNOWN thread=0x%08X value=0x%08X", run->thread->id, value);
        (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, 0x80010002u);
    }
    return 1;
}

/* Serves one stall the SPU performed. A full interrupt mailbox is the only channel stall this runner can clear from
 * the SPU side, and clearing it is the same delivery. Everything else waits for the PPU or a peer SPU. */
static int serve_pause(ps3_spu_group_runner *runner, ps3_spu_thread_run *run, uint32_t channel)
{
    if (channel != SPU_WrOutIntrMbox) return 0;
    return serve_out_interrupt_mailbox(runner, run);
}

/* Services for lifted code (spu_lifted_hybrid.h). Lifted code runs many blocks per slice and cannot be resumed at an
 * arbitrary instruction, so the waits the interpreter turns into PAUSED happen in place: serve what the SPU asked for
 * (the out-interrupt mailbox is the only thing a wait can clear from this side), poll cancellation, sleep a bounded
 * moment. `context` is the thread's ps3_spu_worker_argument. */
static int lifted_wait(void *context, spu_context *ctx, uint32_t channel)
{
    ps3_spu_worker_argument *worker = context;
    (void)ctx;
    if (is_cancelled(worker->runner, worker->run)) return 1;
    { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); note_wait(worker->run, channel, timespec_ns(&now)); }
    __atomic_fetch_add(&worker->run->pauses, 1u, __ATOMIC_RELEASE);
    if (serve_out_interrupt_mailbox(worker->runner, worker->run)) return 0;
    idle_wait_timed(worker->runner->services.pause_sleep_microseconds ? worker->runner->services.pause_sleep_microseconds : 200, channel);
    return 0;
}
/* The reference delivers an interrupt-mailbox word on the write itself (spu_channels.c:1505); the interpreter path does
 * so at the next slice boundary, lifted code does so here. */
static void lifted_intr_written(void *context, spu_context *ctx)
{
    ps3_spu_worker_argument *worker = context;
    (void)ctx;
    (void)serve_out_interrupt_mailbox(worker->runner, worker->run);
}

static void end_thread(ps3_spu_group_runner *runner, ps3_spu_thread_run *run, ps3_spu_thread_state state)
{
    publish_state(run, (unsigned)state);
    if (state == PS3_SPU_THREAD_EXITED)
        (void)ps3_lv2_spu_thread_record_end(runner->services.groups, run->thread->id, 1, run->exit_status, 0);
    else if (state == PS3_SPU_THREAD_GROUP_EXIT)
        (void)ps3_lv2_spu_thread_record_end(runner->services.groups, run->thread->id, 0, 0, 1);
    else if (state != PS3_SPU_THREAD_RUNNING)
        (void)ps3_lv2_spu_thread_record_end(runner->services.groups, run->thread->id, 0, 0, 0);
}

/* One slice. Returns 1 when the thread reached a terminal (its state is already published). */
static int run_slice(ps3_spu_group_runner *runner, ps3_spu_thread_run *run)
{
    const uint64_t budget = runner->services.slice_instructions ? runner->services.slice_instructions : 200000;
    ps3_spu_interp_result result;
    /* A lifted thread returns the same result kinds: STOPPED at the instruction after a stop (lifted code or the
     * interpreter), PAUSED/BUDGET only from interpreted stretches, FAULT as a terminal. `budget` bounds interpreted
     * stretches only; lifted code runs until the next stop. */
    /* Root (2026-11-12, Decision 99): wall time spent inside SPU code, by thread id low byte / 0x100 (0..7), and the
     * instruction count beside it. Tells a busy SPU thread from one that waits. */
    struct timespec slice_begin, slice_end;
    clock_gettime(CLOCK_MONOTONIC, &slice_begin);
    __atomic_store_n(&run->slice_begin_ns, timespec_ns(&slice_begin), __ATOMIC_RELAXED);
    prof_lock_sample(runner->services.domain);
    prof_concurrency(+1, timespec_ns(&slice_begin));
    const int status = run->lifted
        ? ps3_spu_lifted_run_slice(&run->executor, run->lifted, &run->lifted_services, budget, &result, &run->lifted_stats)
        : run->dyn_on
        ? ps3_spu_lifted_run_slice_dyn(&run->executor, &run->dyn, &run->lifted_services, budget, &result, &run->lifted_stats)
        : ps3_spu_executor_run_interp(&run->executor, run->executor.core.pc, budget, &result);
    clock_gettime(CLOCK_MONOTONIC, &slice_end);
    __atomic_store_n(&run->slice_begin_ns, 0u, __ATOMIC_RELAXED);
    prof_concurrency(-1, timespec_ns(&slice_end));
    if (t_prof_slot < PROF_SLOTS && !(++t_prof_slices % PROF_CPU_EVERY)) {
        prof_cpu_sample();
        uint64_t dma[PS3_ATOMIC_DMA_COUNTS];
        ps3_atomic_dma_take_counts(dma);
        for (unsigned k = 0; k < PS3_ATOMIC_DMA_COUNTS; ++k)
            __atomic_store_n(&g_prof[t_prof_slot].dma[k], k == PS3_ATOMIC_GUARD_WAIT_MAX_NS ? (dma[k] > g_prof[t_prof_slot].dma[k] ? dma[k] : g_prof[t_prof_slot].dma[k])
                                                                                    : g_prof[t_prof_slot].dma[k] + dma[k], __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&run->slice_count, 1u, __ATOMIC_RELAXED);
    {
        const unsigned which = run->thread ? (unsigned)((run->thread->id >> 8) & 7u) : 7u;
        const uint64_t spent = (uint64_t)(slice_end.tv_sec - slice_begin.tv_sec) * 1000000000ull + (uint64_t)slice_end.tv_nsec - (uint64_t)slice_begin.tv_nsec;
        __atomic_fetch_add(&g_ps3_spu_run_ns[which], spent, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_ps3_spu_run_instructions[which], result.instructions, __ATOMIC_RELAXED);
#ifdef PS3_SPU_IMAGE_ACCOUNT
        /* Decision 104: one pass per slice, never per instruction. */
        if (run->dyn_on) {
            ps3_spu_acct_flush(&run->executor, spent, run->dyn.slice_lifted_ns, run->dyn.lifted_ns, run->dyn.lifted_enters);
            run->dyn.slice_lifted_ns = 0;
            ps3_spu_acct_flush_escapes(&run->dyn);   /* round 2: why and where lifted job code escaped (PS3_SPU_ESCAPE_DIAG; a few compares when nothing did) */
        } else if (run->lifted && run->acct_static_slot) {
            const uint64_t enters = run->lifted_stats.enters - run->acct_enters_seen;
            run->acct_enters_seen = run->lifted_stats.enters;
            ps3_spu_acct_flat(run->acct_static_slot, result.instructions, enters ? 0u : spent, enters ? spent : 0u, enters);
        } else if (run->acct_static_slot) {
            ps3_spu_acct_flat(run->acct_static_slot, result.instructions, spent, 0u, 0u);
        }
#endif
    }
    __atomic_store_n(&run->pc, result.pc, __ATOMIC_RELAXED);
    __atomic_fetch_add(&run->instructions, result.instructions, __ATOMIC_RELAXED);
    if (status != PS3_SPU_EXEC_OK) {
        run->terminal_code = (uint32_t)status;
        run->last_channel = 0;
        trace_line(runner, "SPU_STOP thread=0x%08X kind=refused code=%d pc=0x%05X", run->thread->id, status, run->pc);
        end_thread(runner, run, PS3_SPU_THREAD_FAULT);
        return 1;
    }
    switch (result.kind) {
    case PS3_SPU_INTERP_BUDGET:
        __atomic_store_n(&run->wait_last_ns, 0u, __ATOMIC_RELAXED);
        /* The SPU may have written an interrupt mailbox word during the slice and carried on (that write does not
         * block). Deliver it now, at the slice boundary. */
        (void)serve_out_interrupt_mailbox(runner, run);
        /* Several SPU workers can share one host core, and Horizon does not time-slice equal-priority threads: a
         * worker that never stalls would keep its core. A zero-length sleep is a yield to them. */
        sleep_microseconds(0);
        return 0;
    case PS3_SPU_INTERP_PAUSED:
        /* Root fix (C1b): published atomically; the owner reads these while the worker runs (TSan). */
        __atomic_store_n(&run->last_channel, result.channel, __ATOMIC_RELEASE);
        note_wait(run, result.channel, timespec_ns(&slice_end));
        if (runner->services.spurious_lr_us && result.channel == SPU_RdEventStat) maybe_spurious_lr(runner, run, timespec_ns(&slice_end));
        __atomic_fetch_add(&run->pauses, 1u, __ATOMIC_RELEASE);
        /* The slice can stop on a channel BEFORE it ever runs out of budget, so the pending interrupt-mailbox word the
         * SPU wrote earlier in the same slice is delivered here: a blocking RdInMbox waits on exactly that reply. */
        (void)serve_out_interrupt_mailbox(runner, run);
        if (__atomic_load_n(&run->pauses, __ATOMIC_RELAXED) == 1u || result.channel != run->traced_channel) {
            run->traced_channel = result.channel;   /* root: trace the first pause per channel, not every retry */
            trace_line(runner, "SPU_PAUSE thread=0x%08X pc=0x%05X channel=%u", run->thread->id, run->pc, result.channel);
        }
        if (serve_pause(runner, run, result.channel)) return 0;
        /* The PPU (or a peer SPU) must act. A bounded sleep keeps the owner's cancellation point reachable. */
        idle_wait_timed(runner->services.pause_sleep_microseconds ? runner->services.pause_sleep_microseconds : 200, result.channel);
        return 0;
    case PS3_SPU_INTERP_FAULT:
        run->terminal_code = result.code;
        run->last_channel = result.channel;
        trace_line(runner, "SPU_STOP thread=0x%08X kind=fault code=%u channel=%u pc=0x%05X stop=0x%X opcode=0x%08X",
                   run->thread->id, result.code, result.channel, result.pc, result.stop_code, result.opcode);
        end_thread(runner, run, PS3_SPU_THREAD_FAULT);
        return 1;
    case PS3_SPU_INTERP_STOPPED:
        __atomic_store_n(&run->wait_last_ns, 0u, __ATOMIC_RELAXED);
        break;
    }
    trace_line(runner, "SPU_STOP thread=0x%08X kind=stop code=0x%X pc=0x%05X", run->thread->id, result.code, run->pc);
    switch (result.code) {
    case SPU_STOP_THREAD_EXIT:
        run->exit_status = run->executor.core.gpr[3]._u32[0];   /* stop 0x102: r3's preferred word */
        trace_line(runner, "SPU_EXIT thread=0x%08X status=0x%X r3=0x%X r4=0x%X r5=0x%X r6=0x%X", run->thread->id,
                   run->exit_status, run->executor.core.gpr[3]._u32[0], run->executor.core.gpr[4]._u32[0],
                   run->executor.core.gpr[5]._u32[0], run->executor.core.gpr[6]._u32[0]);
        end_thread(runner, run, PS3_SPU_THREAD_EXITED);
        return 1;
    case SPU_STOP_GROUP_EXIT:
        end_thread(runner, run, PS3_SPU_THREAD_GROUP_EXIT);
        return 1;
    case SPU_STOP_RECEIVE_EVENT: {
        /* Root (2026-11-12): sys_spu_thread_receive_event. The SPU wrote the queue number to its out mailbox, then
         * stopped; lv2 takes one event from the queue bound by 193 and answers through the in mailbox with four words:
         * CELL_OK and the low words of data1..data3 (one word, CELL_EINVAL, when nothing is bound). The SPU waits here
         * until the PPU sends an event; the wait is a bounded poll so cancellation stays reachable. */
        uint32_t spuq = 0, queue = 0;
        const int popped = ps3_spu_executor_mbox_receive(&run->executor, PS3_SPU_MAILBOX_OUT, &spuq);
        trace_line(runner, "SPU_RECEIVE_BEGIN thread=0x%08X pc=0x%05X spuq=0x%08X pop=%d out=%u in=%u", run->thread->id,
                   run->executor.core.pc, spuq, popped, (unsigned)run->executor.core.ch_out_mbox.count,
                   (unsigned)run->executor.core.ch_in_mbox.count);
        if (ps3_lv2_spu_thread_bound_queue(runner->services.groups, run->thread->id, &queue) != PS3_LV2_SPU_OK || !queue) {
            trace_line(runner, "SPU_RECEIVE thread=0x%08X spuq=%u queue=none", run->thread->id, spuq);
            (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, 0x80010002u);
            return 0;
        }
        __atomic_store_n(&run->receive_wait, 1u, __ATOMIC_RELAXED);
        for (;;) {
            ps3_lv2_event event;
            uint32_t number = 0;
            const uint32_t code = ps3_lv2_queue_tryreceive(runner->services.objects, queue, &event, 1, &number);
            if (code != PS3_LV2_OK) {   /* the queue is gone: lv2 wakes the SPU with the error */
                trace_line(runner, "SPU_RECEIVE thread=0x%08X queue=0x%08X result=0x%08X", run->thread->id, queue, code);
                (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, code);
                return 0;
            }
            if (number) {
                (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, SPU_CELL_OK);
                (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, (uint32_t)event.data1);
                (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, (uint32_t)event.data2);
                (void)ps3_spu_executor_mbox_send(&run->executor, PS3_SPU_MAILBOX_IN, (uint32_t)event.data3);
                __atomic_fetch_add(&run->events_received, 1u, __ATOMIC_RELAXED);
                __atomic_store_n(&run->receive_wait, 0u, __ATOMIC_RELAXED);
                trace_line(runner, "SPU_RECEIVE_EVENT thread=0x%08X source=0x%llX data=0x%llX,0x%llX,0x%llX in=%u", run->thread->id,
                           (unsigned long long)event.source, (unsigned long long)event.data1, (unsigned long long)event.data2,
                           (unsigned long long)event.data3, (unsigned)run->executor.core.ch_in_mbox.count);
                return 0;
            }
            if (is_cancelled(runner, run)) {
                trace_line(runner, "SPU_STOP thread=0x%08X kind=cancelled pc=0x%05X (receive event)", run->thread->id, run->pc);
                end_thread(runner, run, PS3_SPU_THREAD_CANCELLED);
                return 1;
            }
            __atomic_fetch_add(&run->pauses, 1u, __ATOMIC_RELEASE);
            { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); note_wait(run, 0x1000u | SPU_STOP_RECEIVE_EVENT, timespec_ns(&now)); }   /* 0x1110: waiting in sys_spu_thread_receive_event */
            sleep_microseconds(100);
        }
    }
    default:
        run->terminal_code = PS3_SPU_EXEC_STOP;
        end_thread(runner, run, PS3_SPU_THREAD_FAULT);
        return 1;
    }
}

static void worker_main(void *argument)
{
    ps3_spu_worker_argument *worker = argument;
    ps3_spu_group_runner *runner = worker->runner;
    ps3_spu_thread_run *run = worker->run;
#if defined(__aarch64__) && !defined(PS3_SPU_FLOAT_NEAREST)
    /* Root (2026-11-13, Decision 107): an SPU does single-precision maths with truncation and flush-to-zero. The helpers
     * leave rounding to the thread's FPCR and nothing set it, so every SPU thread rounded to nearest (the inFAMOUS 2
     * port found the same fault: wrong SPU rounding made characters invisible). RMode = RZ (bits 23:22), FZ (bit 24),
     * once per SPU worker thread; the double-precision scope (spu_fp_begin_double) still switches and restores.
     * -DPS3_SPU_FLOAT_NEAREST gives the old behaviour back. */
    {
        uint64_t fpcr;
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        fpcr |= (3ull << 22) | (1ull << 24);
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    }
#endif
    ps3_atomic_set_writer_tag(((run->thread->id >> 8) & 0xFFu) + 1u);   /* Decision 123: guarded writes of this worker carry slot + 1 */
    t_prof_slot = (unsigned)((run->thread->id >> 8) & 7u);              /* Decision 134: this worker's STARTUP_SPU_PROFILE slot */
    publish_state(run, PS3_SPU_THREAD_RUNNING);
    trace_line(runner, "SPU_THREAD_START thread=0x%08X index=%u pc=0x%05X", run->thread->id, run->thread->spu_number, run->pc);
    for (;;) {
        if (is_cancelled(runner, run)) {
            trace_line(runner, "SPU_STOP thread=0x%08X kind=cancelled pc=0x%05X", run->thread->id, run->pc);
            end_thread(runner, run, PS3_SPU_THREAD_CANCELLED);
            return;
        }
        if (run_slice(runner, run)) return;
    }
}

static ps3_lv2_spu_group *find_group(ps3_lv2_spu_groups *groups, uint32_t group_id)
{
    for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi)
        if (groups->groups[gi].active && groups->groups[gi].id == group_id) return &groups->groups[gi];
    return NULL;
}

int ps3_spu_group_runner_init(ps3_spu_group_runner *runner, const ps3_spu_group_runner_services *services,
                              uint32_t group_id)
{
    if (!runner) return PS3_SPU_EXEC_INVALID;
    memset(runner, 0, sizeof *runner);
    if (!services || !services->threads || !services->thread_port_count ||
        !services->threads[0].start || !services->threads[0].join ||
        !services->domain || !services->groups || !services->objects ||
        !services->services.access || !services->services.checkpoint ||
        !services->services.reserve_trace || !services->services.finish_trace ||
        !services->clock.now_microseconds)
        return PS3_SPU_EXEC_INVALID;
    ps3_lv2_spu_group *group = find_group(services->groups, group_id);
    if (!group || !group->started) return PS3_SPU_EXEC_INVALID;
    runner->services = *services;
    runner->group = group;
    runner->group_id = group_id;
    runner->num = group->num;
    runner->started = 0;
    return PS3_SPU_EXEC_OK;
}

/* The SPU ABI argument: argN is a u64 in the preferred doubleword of rN -- the doubleword the SPU addresses as
 * byte 0 of the register. The register keeps architectural word order, so the high word is _u32[0] and the low word
 * is _u32[1]; that IS the doubleword-0 preferred slot. A caller that passes a plain pointer (which lv2 does for
 * CellSpurs) therefore lands in _u32[1] of the r3 doubleword, and the SPU's own `lqd`/word accesses see it. */
static void set_argument(u128 *register_value, uint64_t argument)
{
    *register_value = (u128){0};
    /* Root fix (C1b Eden): set the two words explicitly. `_u64[0] = argument` on a little-endian host put the LOW word
     * in _u32[0] (Eden: arg1=0x100000000 showed r3 words 00000000 00000001), so the kernel read the CellSpurs EA from
     * r4 as EAH and its first DMA was refused. Architectural order: _u32[0] = high word, _u32[1] = low word. */
    register_value->_u32[0] = (uint32_t)(argument >> 32);
    register_value->_u32[1] = (uint32_t)argument;
}

/* Gives the thread's borrowed resources back. The record stays readable (`initialized` keeps its meaning: this
 * thread took part in the run), so the result printer can still report what it did after the join. */
static void release_run(ps3_spu_group_runner *runner, ps3_spu_thread_run *run)
{
    if (!run->owner_acquired) return;
    (void)ps3_spu_executor_release(&run->executor);
    (void)ps3_atomic_owner_release(runner->services.domain, &run->owner);
    run->owner_acquired = 0;
}

int ps3_spu_group_runner_start(ps3_spu_group_runner *runner)
{
    if (!runner || !runner->group) return PS3_SPU_EXEC_INVALID;
    if (runner->started) return PS3_SPU_EXEC_OK;
    uint32_t prepared = 0;
    for (; prepared < runner->num && prepared < PS3_SPU_GROUP_RUNNER_MAX_THREADS; ++prepared) {
        ps3_lv2_spu_thread *thread = &runner->group->threads[prepared];
        if (!thread->initialized || !thread->ls) break;   /* an uninitialized slot is simply not run */
        ps3_spu_thread_run *run = &runner->runs[prepared];
        run->thread = thread;
        if (ps3_atomic_owner_acquire(runner->services.domain, &run->owner) != PS3_GUEST_MEMORY_OK) break;
        run->owner_acquired = 1;
        if (ps3_spu_executor_init_borrowed(&run->executor, runner->services.domain, run->owner, thread->ls,
                                           PS3_LV2_SPU_LS_BYTES, thread->spu_number,
                                           runner->services.services) != PS3_GUEST_MEMORY_OK) {
            release_run(runner, run);
            break;
        }
        /* A thread whose loaded local store is a known lifted program (content hash of its code segment) runs hybrid
         * lifted/interpreted; anything else is an interpreter owner, exactly as before. */
        run->lifted = ps3_spu_lifted_find(thread->ls, thread->entry_point & (SPU_LS_SIZE - 1u),
                                          runner->services.lifted_modules, runner->services.lifted_module_count);
#ifdef PS3_SPU_IMAGE_ACCOUNT
        uint32_t image_size = 0;
        const uint64_t image_key = runner->services.job_accounting ? ps3_spu_acct_image_key(thread->ls, &image_size) : 0u;
#endif
        if (run->lifted) {
            if (ps3_spu_lifted_start(&run->executor, run->lifted, runner->services.clock) != PS3_SPU_EXEC_OK) {
                release_run(runner, run);
                break;
            }
            trace_line(runner, "SPU_LIFTED thread=0x%08X module=%s", thread->id, run->lifted->name);
#ifdef PS3_SPU_IMAGE_ACCOUNT
            if (runner->services.job_accounting)
                run->acct_static_slot = ps3_spu_acct_slot(PS3_SPU_ACCT_STATIC, image_key, run->lifted->entry_pc, image_size, run->lifted->name);
#endif
        } else if (runner->services.job_accounting || runner->services.job_module_count) {
            /* Decision 104: the dynamic job layer: interpreter owner + region accounting + lifted job modules (when given). */
            if (ps3_spu_lifted_dyn_start(&run->executor, runner->services.clock) != PS3_SPU_EXEC_OK) {
                release_run(runner, run);
                break;
            }
            run->dyn_on = 1;
            run->dyn.modules = runner->services.job_modules; run->dyn.module_count = runner->services.job_module_count;
            run->dyn.window_lo = 0x4000u;
#ifdef PS3_SPU_IMAGE_ACCOUNT
            if (runner->services.job_accounting) {
                const uint64_t group = runner->group_id;
                char name[40]; snprintf(name, sizeof name, "image g%llx entry 0x%X", (unsigned long long)group, (unsigned)thread->entry_point);
                const unsigned image = ps3_spu_acct_slot(PS3_SPU_ACCT_IMAGE, image_key ^ (group * UINT64_C(0x9E3779B97F4A7C15)), thread->entry_point & (SPU_LS_SIZE - 1u), image_size, name);
                const unsigned policy = ps3_spu_acct_slot(PS3_SPU_ACCT_POLICY, group, 0xA00u, 0, "policy (not seen yet)");
                const unsigned job = ps3_spu_acct_slot(PS3_SPU_ACCT_JOB, 0, 0x4000u, 0, "job (not seen yet)");
                const unsigned other = ps3_spu_acct_slot(PS3_SPU_ACCT_OTHER, 0, 0, 0, "other (stubs, top of LS)");
                ps3_spu_acct_arm(&run->executor, image, policy, job, other);
                run->dyn.on_job_call = ps3_spu_acct_job_call;
                run->dyn.on_job_end = ps3_spu_acct_job_ran;          /* Decision 134: per-job run time */
                run->dyn.hook_context = (void *)(uintptr_t)runner->group_id;
            } else {
                ps3_spu_acct_arm(&run->executor, 0, 0, 0, 0);   /* the region map alone: the job call watch needs it */
            }
#else
            (void)0;
#endif
            trace_line(runner, "SPU_DYN thread=0x%08X modules=%u accounting=%u", thread->id, runner->services.job_module_count, runner->services.job_accounting);
        } else {
            ps3_spu_executor_set_interpreter(&run->executor);
            ps3_spu_executor_start_config start = { 0 };
            start.image = NULL;
            start.image_id = -1;
            start.clock = runner->services.clock;
            start.thread_option = PS3_SPU_THREAD_OPTION_NONE;
            start.hop_budget = 1;   /* an interpreter owner never takes a hop; any nonzero value is accepted */
            if (ps3_spu_executor_start(&run->executor, &start) != PS3_SPU_EXEC_OK) {
                release_run(runner, run);
                break;
            }
        }
        run->executor.core.pc = thread->entry_point & (SPU_LS_SIZE - 1u);   /* SPU_LS_MASK: the entry is an LS address */
        run->executor.core.status = SPU_STATUS_RUNNING;
        set_argument(&run->executor.core.gpr[3], thread->arg1);
        set_argument(&run->executor.core.gpr[4], thread->arg2);
        set_argument(&run->executor.core.gpr[5], thread->arg3);
        set_argument(&run->executor.core.gpr[6], thread->arg4);
        run->pc = run->executor.core.pc;
        {   /* root diagnostic: what the borrowed LS holds where the thread will start */
            const unsigned char *ls = thread->ls;
            const uint32_t at = thread->entry_point & (SPU_LS_SIZE - 1u) & ~3u;
            trace_line(runner, "SPU_LS thread=0x%08X ls=%p word0=%02X%02X%02X%02X entry_word=%02X%02X%02X%02X",
                       thread->id, (const void *)ls, ls[0], ls[1], ls[2], ls[3], ls[at], ls[at + 1], ls[at + 2], ls[at + 3]);
        }
        trace_line(runner, "SPU_ARG thread=0x%08X entry=0x%05X arg1=0x%llX r3ddw=0x%08X%08X", thread->id,
                   (unsigned)thread->entry_point, (unsigned long long)thread->arg1,
                   run->executor.core.gpr[3]._u32[0], run->executor.core.gpr[3]._u32[1]);
        run->initialized = 1;
    }
    if (!prepared) return PS3_SPU_EXEC_INVALID;   /* nothing to run: not a success, and nothing was started */
    runner->prepared = prepared;
    runner->started = 1;
    /* The owners exist; now give each one its host thread. A failed start is a terminal for that thread only. */
    for (uint32_t index = 0; index < prepared; ++index) {
        ps3_spu_thread_run *run = &runner->runs[index];
        runner->worker_arguments[index] = (ps3_spu_worker_argument){ runner, run };
        run->lifted_services = (ps3_spu_lifted_services){ .context = &runner->worker_arguments[index], .wait = lifted_wait,
                                                          .intr_written = lifted_intr_written };
        if (index >= runner->services.thread_port_count) {   /* no port for this thread: a named terminal */
            run->terminal_code = PS3_SPU_EXEC_INVALID;
            end_thread(runner, run, PS3_SPU_THREAD_FAULT);
            continue;
        }
        const ps3_service_thread_port port = runner->services.threads[index];
        if (port.start(port.context, worker_main, &runner->worker_arguments[index])) {
            trace_line(runner, "SPU_THREAD_START_FAILED thread=0x%08X index=%u", run->thread->id, index);
            run->terminal_code = PS3_SPU_EXEC_INVALID;
            end_thread(runner, run, PS3_SPU_THREAD_FAULT);
            continue;
        }
        run->host_started = 1;
        ++runner->threads_started;
    }
    return PS3_SPU_EXEC_OK;
}

const ps3_spu_thread_run *ps3_spu_group_runner_thread(const ps3_spu_group_runner *runner, uint32_t index)
{
    if (!runner || index >= runner->num || index >= PS3_SPU_GROUP_RUNNER_MAX_THREADS) return NULL;
    if (!runner->runs[index].initialized) return NULL;
    return &runner->runs[index];
}

int ps3_spu_group_runner_finished(const ps3_spu_group_runner *runner)
{
    if (!runner || !runner->started) return 0;
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        const unsigned state = __atomic_load_n(&runner->runs[index].state, __ATOMIC_ACQUIRE);
        if (state != PS3_SPU_THREAD_EXITED && state != PS3_SPU_THREAD_GROUP_EXIT &&
            state != PS3_SPU_THREAD_FAULT && state != PS3_SPU_THREAD_CANCELLED)
            return 0;
    }
    return 1;
}

int ps3_spu_group_runner_join(ps3_spu_group_runner *runner, uint32_t *cause, uint32_t *status)
{
    if (!runner || !runner->started) return PS3_SPU_EXEC_INVALID;
    if (runner->joined) {
        if (cause) *cause = runner->join_cause;
        if (status) *status = runner->join_status;
        return PS3_SPU_EXEC_OK;
    }
    /* Root fix (C1b): never block in a host join while a worker may still run. Wait, bounded and cancellable, for
     * every thread to reach a terminal; on cancellation ask the workers to stop, then wait for them to converge (each
     * polls its stop flag at every slice/pause boundary). Only then join the host threads, which by then return. */
    int cancelled = 0;
    for (;;) {
        if (ps3_spu_group_runner_finished(runner)) break;
        const unsigned *flag = runner->services.cancel_flag;
        if (flag && __atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
            cancelled = 1;
            for (uint32_t index = 0; index < runner->prepared; ++index)
                __atomic_store_n(&runner->runs[index].stop_requested, 1u, __ATOMIC_RELEASE);
        }
        sleep_microseconds(1000);
    }
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        if (index >= runner->services.thread_port_count) break;
        if (!runner->runs[index].host_started) continue;   /* no host thread to join */
        const ps3_service_thread_port port = runner->services.threads[index];
        if (port.join(port.context)) return PS3_SPU_EXEC_MEMORY;   /* the owner keeps the thread's storage */
    }
    /* The cause: GROUP_EXIT if any thread chose stop 0x101, else all-threads-exit. */
    uint32_t group_exit = 0, exited_status = 0;
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        const ps3_spu_thread_run *run = &runner->runs[index];
        const unsigned state = __atomic_load_n(&run->state, __ATOMIC_ACQUIRE);
        if (state == PS3_SPU_THREAD_GROUP_EXIT) group_exit = 1;
        if (state == PS3_SPU_THREAD_EXITED) exited_status = run->exit_status;
    }
    runner->join_cause = group_exit ? PS3_SPU_GROUP_JOIN_GROUP_EXIT : PS3_SPU_GROUP_JOIN_ALL_THREADS_EXIT;
    /* The group status is the last exiting thread's exit status, as lv2 reports it. */
    runner->join_status = group_exit ? 0 : exited_status;
    for (uint32_t index = 0; index < runner->prepared; ++index) release_run(runner, &runner->runs[index]);
    runner->joined = 1;
    if (cause) *cause = runner->join_cause;
    if (status) *status = runner->join_status;
    /* Root (C1b): a join the process cancellation ended is not a group exit: the caller names it, never CELL_OK. */
    return cancelled ? PS3_SPU_EXEC_CANCELLED : PS3_SPU_EXEC_OK;
}

int ps3_spu_group_runner_terminate(ps3_spu_group_runner *runner)
{
    if (!runner) return PS3_SPU_EXEC_INVALID;
    if (!runner->started || runner->joined) return PS3_SPU_EXEC_OK;
    for (uint32_t index = 0; index < runner->prepared; ++index)
        __atomic_store_n(&runner->runs[index].stop_requested, 1u, __ATOMIC_RELEASE);
    uint32_t cause = 0, status = 0;
    return ps3_spu_group_runner_join(runner, &cause, &status);
}

int ps3_spu_group_runner_thread_exit_status(const ps3_spu_group_runner *runner, uint32_t thread_id, uint32_t *status)
{
    if (!runner || !status) return PS3_SPU_EXEC_INVALID;
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        const ps3_spu_thread_run *run = &runner->runs[index];
        if (!run->thread || run->thread->id != thread_id) continue;
        if (__atomic_load_n(&run->state, __ATOMIC_ACQUIRE) != PS3_SPU_THREAD_EXITED)
            return PS3_SPU_EXEC_UNSUPPORTED;
        *status = run->exit_status;
        return PS3_SPU_EXEC_OK;
    }
    return PS3_SPU_EXEC_INVALID;
}

const char *ps3_spu_thread_state_name(unsigned state)
{
    switch ((ps3_spu_thread_state)state) {
    case PS3_SPU_THREAD_IDLE: return "idle";
    case PS3_SPU_THREAD_RUNNING: return "running";
    case PS3_SPU_THREAD_EXITED: return "exited";
    case PS3_SPU_THREAD_GROUP_EXIT: return "group_exit";
    case PS3_SPU_THREAD_FAULT: return "fault";
    case PS3_SPU_THREAD_CANCELLED: return "cancelled";
    }
    return "unknown";
}

/* ================================================================== the plain-typed port (spu_run_port.h), root C1b */
/* One process-wide pool. start/join/terminate run on the PPU threads that issue the syscalls; configure runs once at
 * composition, before any guest code. A record is claimed for one group by its first start and kept until join. */
enum { SPU_RUN_POOL = PS3_LV2_SPU_GROUP_CAP };
static ps3_spu_run_config g_run_config;
static unsigned g_run_configured;
static ps3_spu_group_runner g_runners[SPU_RUN_POOL];
static unsigned g_runner_used[SPU_RUN_POOL];
static unsigned g_run_ports_taken;   /* host thread ports handed to runners so far (see ps3_spu_run_start) */

static int run_access(void *context, uint64_t address, size_t bytes, int write)
{
    (void)context;
    for (unsigned index = 0; index < g_run_config.range_count; ++index) {
        const uint64_t base = g_run_config.range_base[index], length = g_run_config.range_length[index];
        /* Root (2026-11-11): a range ends at its last byte, but the PS3 maps whole 64 KiB pages, so a transfer may
         * run past a segment's last byte into the rest of its page (the game's SPU job code reads 0x800 bytes at
         * 0x718180 and the text segment ends at 0x718748). The declared length is rounded up to the page. */
        const uint64_t paged = ((base + length + 0xFFFFu) & ~UINT64_C(0xFFFF)) - base;
        if (address >= base && bytes <= paged && address - base <= paged - bytes) return PS3_SPU_EXEC_OK;
    }
    if (g_run_config.trace) {   /* root diagnostic: name the refused DMA */
        char line[160];
        snprintf(line, sizeof line, "SPU_DMA_REFUSED ea=0x%llX bytes=0x%zX write=%d", (unsigned long long)address, bytes, write);
        g_run_config.trace(g_run_config.trace_context, line);
    }
#ifdef __SWITCH__
    {   /* Round 2 (freeze diagnostics): a refused DMA ends the thread silently (state=fault); say so on the console, the first 16 times. */
        static unsigned refused;
        if (__atomic_fetch_add(&refused, 1u, __ATOMIC_RELAXED) < 16u)
            printf("STARTUP_SPU_DMA_REFUSED ea=0x%llX bytes=0x%zX write=%d (the thread ends with state=fault; see STARTUP_SPU_TERMINAL)\n", (unsigned long long)address, bytes, write);
    }
#endif
    return PS3_SPU_EXEC_ACCESS;   /* outside every declared range: a named fault, never a fabricated read */
}
static int run_checkpoint(void *context, unsigned phase)
{
    (void)context; (void)phase;
    return g_run_config.cancel_flag && __atomic_load_n(g_run_config.cancel_flag, __ATOMIC_ACQUIRE)
        ? PS3_SPU_EXEC_CANCELLED : PS3_SPU_EXEC_OK;
}
/* SPU DMA is not recorded in the PPU trace journal (that journal is the PPU's discovery record). */
/* Decision 134: they time every memory operation for STARTUP_SPU_PROFILE (the guard of the atomic domain is taken and released between the two). */
static int run_reserve_trace(void *context, const ps3_spu_transfer *transfer) { (void)context; prof_mem_begin(transfer); return PS3_SPU_EXEC_OK; }
static void run_finish_trace(void *context, const ps3_spu_transfer *transfer, ps3_guest_memory_status status,
                             int committed, const void *bytes)
{ (void)context; (void)status; (void)committed; (void)bytes; prof_mem_end(transfer); }

uint32_t ps3_spu_run_configure(const ps3_spu_run_config *config)
{
    if (!config || !config->domain || !config->objects || !config->groups || !config->clock.now_microseconds ||
        !config->threads || !config->thread_count || !config->range_count || config->range_count > PS3_SPU_RUN_MAX_RANGES)
        return PS3_SPU_RUN_INTERNAL_INVALID;
    g_run_config = *config;
    g_run_configured = 1;
    SPU_LOSS_HOOK_INSTALL();   /* Decision 116 */
    SPU_DIRECT_DMA_INSTALL(config->domain);   /* Decision 135 */
    ps3_atomic_guard_clock = prof_now_ns;      /* Decision 136: guard waits of the SPU workers (threads with a writer tag) */
    return 0;
}
static ps3_spu_group_runner *runner_for(uint32_t group)
{
    for (unsigned index = 0; index < SPU_RUN_POOL; ++index)
        if (g_runner_used[index] && g_runners[index].group_id == group) return &g_runners[index];
    return NULL;
}
#ifdef PS3_SPU_LIFTED_KERNEL
/* The modules of the dynamic layer: the SPURS kernel, then every compiled-in job and policy module. Built once (the first group start), read by every runner after. */
enum { DYNAMIC_MODULES_MAX = 64 };
static const ps3_spu_lifted_module *g_dynamic_modules[DYNAMIC_MODULES_MAX];
static unsigned g_dynamic_module_count, g_dynamic_modules_state;   /* state: 0 empty, 1 building, 2 ready */
static const ps3_spu_lifted_module *const *dynamic_module_set(unsigned *count)
{
    for (;;) {
        const unsigned state = __atomic_load_n(&g_dynamic_modules_state, __ATOMIC_ACQUIRE);
        if (state == 2u) break;
        unsigned expected = 0;
        if (state == 0u && __atomic_compare_exchange_n(&g_dynamic_modules_state, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            unsigned n = 0;
            g_dynamic_modules[n++] = &ps3_spurs_kernel_module;
            g_dynamic_modules[n++] = &ps3_wws_restore_stub_module;      /* Decision 134: laid over the kernel, checks its own bytes */
#ifdef PS3_SPU_LIFTED_JOBS
            for (unsigned i = 0; i < ps3_jobmods_count && n < DYNAMIC_MODULES_MAX; ++i) g_dynamic_modules[n++] = ps3_jobmods_table[i];
#endif
            g_dynamic_module_count = n;
            __atomic_store_n(&g_dynamic_modules_state, 2u, __ATOMIC_RELEASE);
            break;
        }
        sleep_microseconds(50);
    }
    *count = g_dynamic_module_count;
    return g_dynamic_modules;
}
#endif

uint32_t ps3_spu_run_start(uint32_t group)
{
    if (!g_run_configured) return PS3_SPU_RUN_INTERNAL_INVALID;
    if (runner_for(group)) return 0x8001000Au;   /* already running: lv2 EBUSY */
    unsigned slot = 0;
    while (slot < SPU_RUN_POOL && g_runner_used[slot]) ++slot;
    if (slot == SPU_RUN_POOL) return PS3_SPU_RUN_INTERNAL_NO_RUNNER;
    ps3_spu_group_runner_services services;
    memset(&services, 0, sizeof services);
    services.context = g_run_config.trace_context;
    /* Root fix (C1b Eden, 2026-11-11): every runner indexed the port pool from 0, so the game's SECOND SPURS
     * instance (group 0x9E000100, one SPU) asked port 0 -- already running the first group's thread 0 -- to start
     * again: SPU_THREAD_START_FAILED, the group "ended" at once and libsre's join thread returned. Each runner now
     * gets the ports after the ones earlier runners took. Ports are never returned (runner records are not either);
     * an exhausted pool is PS3_SPU_RUN_INTERNAL_PREPARE through the runner's own validation. */
    if (g_run_ports_taken >= g_run_config.thread_count) return PS3_SPU_RUN_INTERNAL_PREPARE;
    services.threads = g_run_config.threads + g_run_ports_taken;
    services.thread_port_count = g_run_config.thread_count - g_run_ports_taken;
    services.domain = g_run_config.domain;
    services.services = (ps3_spu_executor_services){ NULL, run_access, run_checkpoint, run_reserve_trace, run_finish_trace };
    services.objects = g_run_config.objects;
    services.groups = g_run_config.groups;
    services.clock = g_run_config.clock;
    services.slice_instructions = g_run_config.slice_instructions;
    services.pause_sleep_microseconds = g_run_config.pause_sleep_microseconds;
    services.cancel_flag = g_run_config.cancel_flag;
    services.trace = g_run_config.trace;
#ifdef PS3_SPU_LIFTED_BINK
    /* Compile-time opt-in: the Bink movie decoder runs lifted (native/spu_lifted/bink). */
    static const ps3_spu_lifted_module *const k_lifted_modules[] = { &ps3_bink_lifted_module };
    services.lifted_modules = k_lifted_modules;
    services.lifted_module_count = 1;
#endif
#ifdef PS3_SPU_LIFTED_KERNEL
    /* Decision 133: the lifted SPURS kernel (native/spu_lifted/kernel, generated into the build directory) first, then the lifted job modules. */
    services.job_modules = dynamic_module_set(&services.job_module_count);
#elif defined(PS3_SPU_LIFTED_JOBS)
    /* Decision 104: lifted job modules (native/spu_lifted/jobs), on by default; the set is what lle.mk compiled in (LLE_SPU_JOBMODS). */
    services.job_modules = ps3_jobmods_table;
    services.job_module_count = ps3_jobmods_count;
#endif
#ifdef PS3_SPU_IMAGE_ACCOUNT
    services.job_accounting = 1;   /* per-image accounting (STARTUP_SPU_IMAGES); LLE_SPU_ACCOUNT=0 compiles it out */
#ifdef PS3_SPU_SPURIOUS_LR_US
    services.spurious_lr_us = PS3_SPU_SPURIOUS_LR_US;   /* round 2 safety net, see spu_group_runner.h */
#endif
#endif
    if (ps3_spu_group_runner_init(&g_runners[slot], &services, group) != PS3_SPU_EXEC_OK) return PS3_SPU_RUN_INTERNAL_PREPARE;
    if (ps3_spu_group_runner_start(&g_runners[slot]) != PS3_SPU_EXEC_OK) return PS3_SPU_RUN_INTERNAL_PREPARE;
    g_run_ports_taken += g_runners[slot].prepared < services.thread_port_count
        ? g_runners[slot].prepared : services.thread_port_count;
    g_runner_used[slot] = 1;
    return 0;
}
uint32_t ps3_spu_run_join(uint32_t group, uint32_t *cause, uint32_t *status)
{
    ps3_spu_group_runner *runner = runner_for(group);
    if (!runner) return 0x80010005u;   /* ESRCH: no started group */
    const int code = ps3_spu_group_runner_join(runner, cause, status);
    if (code == PS3_SPU_EXEC_OK) return 0u;
    return code == PS3_SPU_EXEC_CANCELLED ? PS3_SPU_RUN_INTERNAL_CANCELLED : PS3_SPU_RUN_INTERNAL_JOIN;
}
uint32_t ps3_spu_run_exit_status(uint32_t thread, uint32_t *status)
{
    for (unsigned index = 0; index < SPU_RUN_POOL; ++index) {
        if (!g_runner_used[index]) continue;
        const int code = ps3_spu_group_runner_thread_exit_status(&g_runners[index], thread, status);
        if (code == PS3_SPU_EXEC_OK) return 0;
        if (code == PS3_SPU_EXEC_UNSUPPORTED) return 0x8001000Fu;   /* ESTAT: not exited yet */
    }
    return 0x80010005u;   /* ESRCH */
}
uint32_t ps3_spu_run_terminate(uint32_t group)
{
    ps3_spu_group_runner *runner = runner_for(group);
    if (!runner) return 0x80010005u;
    return ps3_spu_group_runner_terminate(runner) == PS3_SPU_EXEC_OK ? 0u : PS3_SPU_RUN_INTERNAL_JOIN;
}
void ps3_spu_run_shutdown(void)
{
    for (unsigned index = 0; index < SPU_RUN_POOL; ++index)
        if (g_runner_used[index]) (void)ps3_spu_group_runner_terminate(&g_runners[index]);
}
void ps3_spu_run_report(void (*print)(const char *line))
{
    if (!print) return;
    char line[900];
    for (unsigned slot = 0; slot < SPU_RUN_POOL; ++slot) {
        if (!g_runner_used[slot]) continue;
        const ps3_spu_group_runner *runner = &g_runners[slot];
        for (uint32_t index = 0; index < runner->prepared; ++index) {
            const ps3_spu_thread_run *run = &runner->runs[index];
            snprintf(line, sizeof line,
                     "STARTUP_SPU_THREAD_RESULT group=0x%08X index=%u thread=0x%08X state=%s pc=0x%05X terminal=0x%X "
                     "channel=%u insns=%llu pauses=%u events_sent=%u exit_status=0x%X events_received=%u",
                     runner->group_id, index, run->thread ? run->thread->id : 0u,
                     ps3_spu_thread_state_name(__atomic_load_n(&run->state, __ATOMIC_ACQUIRE)),
                     __atomic_load_n(&run->pc, __ATOMIC_RELAXED), run->terminal_code,
                     __atomic_load_n(&run->last_channel, __ATOMIC_RELAXED),
                     (unsigned long long)__atomic_load_n(&run->instructions, __ATOMIC_RELAXED),
                     __atomic_load_n(&run->pauses, __ATOMIC_RELAXED),
                     __atomic_load_n(&run->events_sent, __ATOMIC_RELAXED), run->exit_status,
                     __atomic_load_n(&run->events_received, __ATOMIC_RELAXED));
            print(line);
            if (run->lifted) {
                snprintf(line, sizeof line, "STARTUP_SPU_LIFTED thread=0x%08X module=%s enters=%llu escapes=%llu stops=%llu waits=%llu interp_instructions=%llu",
                         run->thread ? run->thread->id : 0u, run->lifted->name, (unsigned long long)run->lifted_stats.enters,
                         (unsigned long long)run->lifted_stats.escapes, (unsigned long long)run->lifted_stats.stops,
                         (unsigned long long)run->lifted_stats.waits, (unsigned long long)run->lifted_stats.interp_instructions);
                print(line);
            }
            if (run->dyn_on && (run->lifted_stats.job_calls || run->lifted_stats.enters)) {
                snprintf(line, sizeof line, "STARTUP_SPU_LIFTED_JOBS thread=0x%08X modules=%u job_calls=%llu armed=%llu unknown=%llu hash_mismatch=%llu enters=%llu escapes=%llu irq_escapes=%llu stops=%llu dma_disarms=%llu lifted_ms=%llu hash_ms=%llu interp_instructions=%llu esc_stub=%llu esc_norow=%llu esc_out=%llu esc_hybrid=%llu pm_tries=%llu pm_armed=%llu pm_mismatch=%llu pm_job_hooks=%llu pm_composites=%llu pm_pause_escapes=%llu",
                         run->thread ? run->thread->id : 0u, run->dyn.module_count, (unsigned long long)run->lifted_stats.job_calls,
                         (unsigned long long)run->lifted_stats.job_armed, (unsigned long long)run->lifted_stats.job_unknown,
                         (unsigned long long)run->lifted_stats.job_mismatch, (unsigned long long)run->lifted_stats.enters,
                         (unsigned long long)run->lifted_stats.escapes, (unsigned long long)run->lifted_stats.irq_escapes,
                         (unsigned long long)run->lifted_stats.stops, (unsigned long long)run->lifted_stats.dma_disarms,
                         (unsigned long long)(run->lifted_stats.lifted_ns / 1000000u), (unsigned long long)(run->lifted_stats.hash_ns / 1000000u),
                         (unsigned long long)run->lifted_stats.interp_instructions, (unsigned long long)run->lifted_stats.esc_stub,
                         (unsigned long long)run->lifted_stats.esc_norow, (unsigned long long)run->lifted_stats.esc_out, (unsigned long long)run->lifted_stats.esc_hybrid,
                         (unsigned long long)run->lifted_stats.pm_tries, (unsigned long long)run->lifted_stats.pm_armed, (unsigned long long)run->lifted_stats.pm_mismatch,
                         (unsigned long long)run->lifted_stats.pm_job_hooks, (unsigned long long)run->lifted_stats.pm_composites, (unsigned long long)run->lifted_stats.pm_pause_escapes);
                print(line);
            }
            if (run->dyn_on && (run->lifted_stats.kernel_armed || run->lifted_stats.kernel_mismatch)) {   /* Decision 133: the lifted SPURS kernel of this thread */
                snprintf(line, sizeof line, "STARTUP_SPU_LIFTED_KERNEL thread=0x%08X armed=%llu mismatch=%llu resident=%u pm_calls=%llu pm_armed=%llu exits=%llu disarms=%llu",
                         run->thread ? run->thread->id : 0u, (unsigned long long)run->lifted_stats.kernel_armed, (unsigned long long)run->lifted_stats.kernel_mismatch,
                         run->dyn.resident_kernel != NULL, (unsigned long long)run->lifted_stats.kernel_pm_calls, (unsigned long long)run->lifted_stats.kernel_pm_armed,
                         (unsigned long long)run->lifted_stats.kernel_exits, (unsigned long long)run->lifted_stats.kernel_disarms);
                print(line);
            }
            if (run->thread && run->thread->ls) {   /* root diagnostic (2026-11-12): where a parked thread really is */
                const uint32_t at = run->executor.core.pc & 0x3FFFCu, from = at >= 16u ? at - 16u : 0u;
                char *cursor = line;
                cursor += snprintf(cursor, sizeof line, "STARTUP_SPU_THREAD_WHERE thread=0x%08X core_pc=0x%05X receive_wait=%u "
                                   "out=%u in=%u intr=%u words_from=0x%05X:", run->thread->id, at, run->receive_wait,
                                   (unsigned)run->executor.core.ch_out_mbox.count, (unsigned)run->executor.core.ch_in_mbox.count,
                                   (unsigned)run->executor.core.ch_out_intr_mbox.count, from);
                for (uint32_t word = from; word < from + 48u && word + 4u <= 0x40000u; word += 4u) {
                    const unsigned char *b = run->thread->ls + word;
                    cursor += snprintf(cursor, (size_t)(line + sizeof line - cursor), " %02X%02X%02X%02X", b[0], b[1], b[2], b[3]);
                }
                print(line);
            }
            if (__atomic_load_n(&run->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_FAULT) {
                /* Root (2026-11-11): what the faulting thread was asking the MFC for. */
                const ps3_spu_executor_terminal *terminal = &run->executor.terminal;
                snprintf(line, sizeof line,
                         "STARTUP_SPU_THREAD_FAULT thread=0x%08X code=%d provider=%d channel=%u mfc_command=0x%X ea=0x%llX "
                         "lsa=0x%X bytes=0x%X write=%u committed=%u reg_lsa=0x%X reg_eal=0x%X reg_size=0x%X reg_tag=%u",
                         run->thread ? run->thread->id : 0u, terminal->code, (int)terminal->provider_status,
                         terminal->channel, terminal->request.command, (unsigned long long)terminal->request.ea,
                         terminal->request.lsa, terminal->request.bytes, terminal->request.write,
                         terminal->effect_committed, run->executor.core.mfc_lsa, run->executor.core.mfc_eal,
                         run->executor.core.mfc_size, (unsigned)run->executor.core.mfc_tag);
                print(line);
            }
        }
    }
    for (unsigned index = 0; index < g_late_trace_count && index < LATE_TRACE_LINES; ++index) {
        char out[224];
        snprintf(out, sizeof out, "STARTUP_SPU_LATE_TRACE %u %.190s", index, g_late_trace[index]);
        print(out);
    }
}

/* ============================================================================================ freeze diagnostics (round 2, A1)
 * Console run rv39s froze on the title screen: the game's main thread waited for a SPURS job index that four SPU threads had started
 * and never finished, while STARTUP_SPU_TIME showed those threads standing still. These two functions say, per SPU thread, what the
 * worker is doing: running, stalled on a channel (which, for how long), inside one slice for seconds (a host-level block or a lifted
 * spin), or ended (fault / exit / cancelled). They read the live executor without stopping it (racy on purpose, diagnostics only). */
/* The monitor reads a running worker's executor without stopping it (racy by design: these lines say what the worker is doing, and a torn value only
 * mangles a log field). The reading functions are therefore not instrumented by ThreadSanitizer; every cross-thread field a decision depends on is atomic. */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define PS3_RACY_DIAGNOSTIC __attribute__((no_sanitize("thread")))
#else
#define PS3_RACY_DIAGNOSTIC
#endif
static uint64_t state_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return timespec_ns(&t);
}

PS3_RACY_DIAGNOSTIC static void state_hex(char *line, size_t size, size_t *used, const char *label, const uint8_t *ls, uint32_t address, uint32_t bytes)
{
    if (*used >= size) return;
    *used += (size_t)snprintf(line + *used, size - *used, " %s=", label);
    for (uint32_t i = 0; i < bytes && *used < size; ++i)
        *used += (size_t)snprintf(line + *used, size - *used, "%02X", ls ? ls[address + i] : 0u);
}

/* One thread's state line (tag = the line family). `extra` (may be "") is appended. */
PS3_RACY_DIAGNOSTIC static void format_state(char *line, size_t size, const char *tag, unsigned seconds, const ps3_spu_group_runner *runner,
                         uint32_t index, const ps3_spu_thread_run *run, const char *extra, uint64_t now)
{
    const unsigned state = __atomic_load_n(&run->state, __ATOMIC_ACQUIRE);
    const spu_context *c = &run->executor.core;
    const uint64_t slice_begin = __atomic_load_n(&run->slice_begin_ns, __ATOMIC_RELAXED);
    const uint64_t wait_since = __atomic_load_n(&run->wait_since_ns, __ATOMIC_RELAXED);
    const uint64_t wait_last = __atomic_load_n(&run->wait_last_ns, __ATOMIC_RELAXED);
    const int blocked = wait_last && wait_since && now >= wait_last && now - wait_last < 50000000ull;
    const uint8_t *ls = run->thread ? run->thread->ls : NULL;
    /* the reservation the atomic domain holds for this thread (the real one: the spu_context resv_* fields are not used by the native executor) and how many
     * reservation-loss events the domain has latched in all (round 2, run rv47s: does a parked thread hold a reservation at all, and on which line?) */
    const ps3_atomic_domain *domain = run->executor.memory.domain;
    unsigned rsv = 0, rsv_lost = 0; uint32_t rsv_ea = 0; unsigned long long loss_epoch = 0;
    if (domain) {
        loss_epoch = (unsigned long long)domain->loss_epoch;
        if (run->executor.memory.owner.slot < domain->owner_count) {
            const ps3_atomic_owner_record *record = &domain->owners[run->executor.memory.owner.slot];
            rsv = record->reserved != 0; rsv_lost = record->reservation_lost != 0; rsv_ea = (uint32_t)record->address;
        }
    }
    size_t used = (size_t)snprintf(line, size,
        "%s seconds=%u group=0x%08X idx=%u thread=0x%08X state=%s pc=0x%05X core_pc=0x%05X lr=0x%08X sp=0x%05X "
        "blocked=%s ch=%u since_ms=%llu in_slice_ms=%llu slices=%llu pauses=%u ev=0x%X/0x%X ack=0x%X int=%u dec=0x%08X reload=0x%08X "
        "mfc_q=%u tag_mask=0x%X tag_stat=0x%X resv=%d/0x%X list_stall=0x%X/0x%X mbox_in=%u out=%u intr=%u sig=%u/%u rcv_wait=%u rsv=%u/0x%08X rsv_lost=%u loss_epoch=%llu spurious_lr=%llu useful=%llu instr_per=%llu ev_sent=%u ev_busy=%u ev_notconn=%u last_ev=q0x%08X/spup%u/d0x%06X%s",
        tag, seconds, runner->group_id, index, run->thread ? run->thread->id : 0u, ps3_spu_thread_state_name(state),
        __atomic_load_n(&run->pc, __ATOMIC_RELAXED), c->pc & 0x3FFFCu, c->gpr[0]._u32[0], c->gpr[1]._u32[0] & 0x3FFFFu,
        blocked ? "yes" : "no", blocked ? __atomic_load_n(&run->wait_channel, __ATOMIC_RELAXED) : __atomic_load_n(&run->last_channel, __ATOMIC_RELAXED),
        (unsigned long long)(blocked && now > wait_since ? (now - wait_since) / 1000000u : 0u),
        (unsigned long long)(slice_begin && now > slice_begin ? (now - slice_begin) / 1000000u : 0u),
        (unsigned long long)__atomic_load_n(&run->slice_count, __ATOMIC_RELAXED), __atomic_load_n(&run->pauses, __ATOMIC_RELAXED),
        c->event_status, c->event_mask, c->event_ack_mask, (unsigned)c->int_enable, c->decrementer, run->executor.decrementer_reload,
        (unsigned)run->executor.engine.queue_count, c->mfc_tag_mask, c->mfc_tag_status, c->resv_valid, c->resv_ea, (unsigned)c->list_stall_mask, (unsigned)c->list_stall_stat,
        (unsigned)c->ch_in_mbox.count, (unsigned)c->ch_out_mbox.count, (unsigned)c->ch_out_intr_mbox.count,
        (unsigned)c->ch_sig_notify[0].count, (unsigned)c->ch_sig_notify[1].count, __atomic_load_n(&run->receive_wait, __ATOMIC_RELAXED),
        rsv, rsv_ea, rsv_lost, loss_epoch, (unsigned long long)run->spurious_lr, (unsigned long long)run->spurious_useful, (unsigned long long)(run->spurious_lr ? run->spurious_instr_total / run->spurious_lr : 0u),
        __atomic_load_n(&run->events_sent, __ATOMIC_RELAXED), __atomic_load_n(&run->events_busy, __ATOMIC_RELAXED), __atomic_load_n(&run->events_notconn, __ATOMIC_RELAXED),
        __atomic_load_n(&run->last_event_queue, __ATOMIC_RELAXED), __atomic_load_n(&run->last_event_spup, __ATOMIC_RELAXED), __atomic_load_n(&run->last_event_data0, __ATOMIC_RELAXED), extra ? extra : "");
    if (used < size) state_hex(line, size, &used, "win4000", ls, 0x4000u, 16u);       /* the job in the window (its SpuGUID) */
    if (used < size) state_hex(line, size, &used, "pm12A0", ls, 0x12A0u, 16u);        /* pm_wwsjob's state word and its neighbours */
}

/* Decision 112 (run rv50s: a request sat in the loader's SPURS queue and no SPU thread ever took it): what the group's SPURS object and tasksets hold, and the lv2 queues the group's
 * threads send user events to, once per report (a minute). SPURS object = the line the threads last reserved; its workload table (+0xB00, 0x20 bytes per workload: image EA, argument
 * = taskset EA, size, priorities) names the tasksets. All reads are plain guest reads under the domain guard; nothing is changed. */
PS3_RACY_DIAGNOSTIC static void dump_guest_lines(const ps3_spu_group_runner *runner, ps3_atomic_domain *domain, void (*print)(const char *line), unsigned seconds,
                                                 const char *what, int wid, uint32_t base, uint32_t from, uint32_t bytes)
{
    char line[400];
    for (uint32_t offset = from; offset < from + bytes; offset += 64u) {
        unsigned char buffer[64];
        if (ps3_atomic_read(domain, (uint64_t)base + offset, buffer, sizeof buffer) != PS3_GUEST_MEMORY_OK) return;
        size_t used = (size_t)snprintf(line, sizeof line, "STARTUP_SPURS_DUMP seconds=%u group=0x%08X what=%s wid=%d base=0x%08X +0x%03X=", seconds, runner->group_id, what, wid, base, offset);
        for (unsigned i = 0; i < sizeof buffer && used + 3u < sizeof line; ++i) used += (size_t)snprintf(line + used, sizeof line - used, "%02X", buffer[i]);
        print(line);
    }
}
PS3_RACY_DIAGNOSTIC static void report_group_shared(const ps3_spu_group_runner *runner, void (*print)(const char *line), unsigned seconds)
{
    ps3_atomic_domain *domain = NULL; uint32_t spurs = 0;
    for (uint32_t index = 0; index < runner->prepared && !spurs; ++index) {
        const ps3_spu_thread_run *run = &runner->runs[index];
        if (!run->thread || !run->executor.memory.domain) continue;
        domain = (ps3_atomic_domain *)run->executor.memory.domain;
        if (run->executor.memory.owner.slot < domain->owner_count) {
            const uint32_t ea = (uint32_t)domain->owners[run->executor.memory.owner.slot].address;
            if (ea >= 0x10000u && !(ea & 127u)) spurs = ea;
        }
    }
    if (domain && spurs) {
        dump_guest_lines(runner, domain, print, seconds, "spurs", -1, spurs, 0x000u, 0x100u);
        dump_guest_lines(runner, domain, print, seconds, "spurs_workloads", -1, spurs, 0xB00u, 0x80u);
        dump_guest_lines(runner, domain, print, seconds, "spurs_signal", -1, spurs, 0xD40u, 0x40u);
        for (unsigned wid = 0; wid < 4u; ++wid) {
            unsigned char entry[16];
            if (ps3_atomic_read(domain, (uint64_t)spurs + 0xB00u + 0x20u * wid, entry, sizeof entry) != PS3_GUEST_MEMORY_OK) break;
            const uint64_t argument = ((uint64_t)entry[8] << 56) | ((uint64_t)entry[9] << 48) | ((uint64_t)entry[10] << 40) | ((uint64_t)entry[11] << 32) |
                                      ((uint64_t)entry[12] << 24) | ((uint64_t)entry[13] << 16) | ((uint64_t)entry[14] << 8) | entry[15];
            if (argument >= 0x10000u && argument < 0x100000000ull && !(argument & 127u))
                dump_guest_lines(runner, domain, print, seconds, "taskset", (int)wid, (uint32_t)argument, 0x000u, 0x180u);
        }
    }
    uint32_t reported[8]; unsigned reported_count = 0;
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        const ps3_spu_thread_run *run = &runner->runs[index];
        if (!run->thread || !runner->services.objects) continue;
        for (uint32_t spup = 0; spup < 64u; ++spup) {
            const uint32_t queue = run->thread->spup_queue[spup];
            if (!queue) continue;
            unsigned seen = 0;
            for (unsigned i = 0; i < reported_count; ++i) seen |= reported[i] == queue;
            if (seen || reported_count >= 8u) continue;
            reported[reported_count++] = queue;
            ps3_lv2_queue snapshot;   /* about 4.2 KB on the monitor thread's stack */
            if (ps3_lv2_queue_snapshot(runner->services.objects, queue, &snapshot) != PS3_LV2_OK) continue;
            char line[300];
            snprintf(line, sizeof line, "STARTUP_SPU_QUEUE seconds=%u group=0x%08X thread_idx=%u spup=%u queue=0x%08X depth=%u queued=%u receivers=%u blocked_receivers=%u events_ever=%llu",
                     seconds, runner->group_id, index, spup, queue, snapshot.size, snapshot.count, snapshot.receivers, snapshot.blocked_receivers, (unsigned long long)snapshot.sequence);
            print(line);
        }
    }
}

PS3_RACY_DIAGNOSTIC void ps3_spu_group_runner_state_report(const ps3_spu_group_runner *runner, void (*print)(const char *line), unsigned seconds)
{
    if (!runner || !print) return;
    char line[1024];
    const uint64_t now = state_now_ns();
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        const ps3_spu_thread_run *run = &runner->runs[index];
        if (!run->thread || !run->thread->ls) continue;
        format_state(line, sizeof line, "STARTUP_SPU_STATE", seconds, runner, index, run, "", now);
        print(line);
    }
    report_group_shared(runner, print, seconds);
}

/* Prints, once each: a thread that reached a terminal (STARTUP_SPU_TERMINAL), a thread stalled on one channel for more than `blocked_ns`
 * (STARTUP_SPU_BLOCKED kind=channel), a thread inside one slice for more than `blocked_ns` (STARTUP_SPU_BLOCKED kind=in_slice). Returns the lines printed. */
PS3_RACY_DIAGNOSTIC unsigned ps3_spu_group_runner_watch(ps3_spu_group_runner *runner, void (*print)(const char *line), unsigned seconds, uint64_t blocked_ns)
{
    if (!runner || !print) return 0;
    char line[1024], extra[160];
    unsigned printed = 0;
    const uint64_t now = state_now_ns();
    for (uint32_t index = 0; index < runner->prepared; ++index) {
        ps3_spu_thread_run *run = &runner->runs[index];
        if (!run->thread || !run->thread->ls) continue;
        const unsigned state = __atomic_load_n(&run->state, __ATOMIC_ACQUIRE);
        if (state != PS3_SPU_THREAD_IDLE && state != PS3_SPU_THREAD_RUNNING && !__atomic_exchange_n(&run->terminal_reported, 1u, __ATOMIC_RELAXED)) {
            const ps3_spu_executor_terminal *terminal = &run->executor.terminal;
            snprintf(extra, sizeof extra, " terminal_code=0x%X exec_code=%d provider=%d channel=%u mfc_cmd=0x%X ea=0x%llX lsa=0x%X bytes=0x%X write=%u",
                     run->terminal_code, terminal->code, (int)terminal->provider_status, terminal->channel, terminal->request.command,
                     (unsigned long long)terminal->request.ea, terminal->request.lsa, terminal->request.bytes, (unsigned)terminal->request.write);
            format_state(line, sizeof line, "STARTUP_SPU_TERMINAL", seconds, runner, index, run, extra, now);
            print(line); ++printed;
        }
        const uint64_t wait_since = __atomic_load_n(&run->wait_since_ns, __ATOMIC_RELAXED);
        const uint64_t wait_last = __atomic_load_n(&run->wait_last_ns, __ATOMIC_RELAXED);
        if (state == PS3_SPU_THREAD_RUNNING && wait_last && wait_since && now > wait_since && now - wait_since > blocked_ns &&
            now >= wait_last && now - wait_last < 50000000ull && !__atomic_exchange_n(&run->wait_reported, 1u, __ATOMIC_RELAXED)) {
            snprintf(extra, sizeof extra, " kind=channel channel=%u for_ms=%llu", __atomic_load_n(&run->wait_channel, __ATOMIC_RELAXED),
                     (unsigned long long)((now - wait_since) / 1000000u));
            format_state(line, sizeof line, "STARTUP_SPU_BLOCKED", seconds, runner, index, run, extra, now);
            print(line); ++printed;
        }
        const uint64_t slice_begin = __atomic_load_n(&run->slice_begin_ns, __ATOMIC_RELAXED);
        if (state == PS3_SPU_THREAD_RUNNING && slice_begin && now > slice_begin && now - slice_begin > blocked_ns && run->slice_reported_ns != slice_begin) {
            run->slice_reported_ns = slice_begin;
            snprintf(extra, sizeof extra, " kind=in_slice for_ms=%llu binding_channel=%u dyn=%u lifted=%d", (unsigned long long)((now - slice_begin) / 1000000u),
                     (unsigned)run->executor.binding.channel, run->dyn_on, run->lifted ? 1 : 0);
            format_state(line, sizeof line, "STARTUP_SPU_BLOCKED", seconds, runner, index, run, extra, now);
            print(line); ++printed;
        }
    }
    return printed;
}

/* The composition's runners (about once a minute / about once a second; spu_run_port.h). */
void ps3_spu_run_state_report(void (*print)(const char *line), unsigned seconds)
{
    for (unsigned slot = 0; slot < SPU_RUN_POOL; ++slot)
        if (g_runner_used[slot]) ps3_spu_group_runner_state_report(&g_runners[slot], print, seconds);
}

unsigned ps3_spu_run_watch(void (*print)(const char *line), unsigned seconds)
{
    unsigned printed = 0;
    for (unsigned slot = 0; slot < SPU_RUN_POOL; ++slot)
        if (g_runner_used[slot]) printed += ps3_spu_group_runner_watch(&g_runners[slot], print, seconds, 5000000000ull);
    return printed;
}
