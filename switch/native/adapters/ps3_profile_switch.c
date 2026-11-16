/* The Horizon half of the sampling profiler (see ps3_profile_core.h). 2026-11-13.
 *
 * Method (the same as the inFAMOUS 2 sibling's hz_profile_run, which was proved on this console): a separate host thread wakes every 1/hz s, pauses the thread it wants
 * (svcSetThreadActivity Paused), reads its register context (svcGetThreadContext3: pc, lr, fp, sp), copies a small window of the paused thread's host stack (so the frame
 * chain can be read after it runs again), and resumes it. Nothing that can take a lock (malloc, stdio, the log) is called while a thread is paused. The lifted game
 * functions are ordinary C functions with a frame record each, so the chain of return addresses is the guest call stack.
 *
 * Which threads: the main guest thread (ps3_profile_start gets its handle) every tick, with the frame chain; every other thread libnx's threadCreate made (the link
 * wraps threadCreate: __wrap_threadCreate records handle and entry) one per 4th tick in turn, pc only. Cost at 1 kHz: one pause/resume pair per ms on the main thread
 * (about 1 % of it) and a quarter of that on the others.
 *
 * Output: <app dir>/profile-<seconds since start>.txt once a minute (counts of that minute), and at once when <app dir>/profile-now.txt appears or changes. Off with
 * <app dir>/profile-off.txt; the rate (100..2000) from <app dir>/profile-rate.txt. */
#ifdef __SWITCH__
#include <switch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "ps3_profile_core.h"
#include "ps3_profile_switch.h"

#ifndef PS3_PROFILE_DIR
#define PS3_PROFILE_DIR "sdmc:/switch/infamous-native-spu-stage31/"
#endif
enum { WINDOW_BYTES = 4096, DEFAULT_HZ = 1000, DUMP_SECONDS = 60, OTHER_EVERY = 4 };

extern Result __real_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid);

/* ---- the threads libnx's threadCreate started in this process */
static Handle g_handles[PS3_PROFILE_MAX_THREADS];
static uintptr_t g_entries[PS3_PROFILE_MAX_THREADS];
static uint8_t g_ready[PS3_PROFILE_MAX_THREADS];
static uint64_t g_thread_samples[PS3_PROFILE_MAX_THREADS];
static uint32_t g_reserved;
static Handle g_main_handle;
static Handle g_sampler_handle;
static uint32_t g_started;
static Thread g_sampler;

void ps3_profile_note_thread(uint32_t handle, uintptr_t entry) {
    const uint32_t slot = __atomic_fetch_add(&g_reserved, 1u, __ATOMIC_RELAXED);
    if (slot >= PS3_PROFILE_MAX_THREADS) return;
    g_handles[slot] = (Handle)handle; g_entries[slot] = entry;
    __atomic_store_n(&g_ready[slot], 1u, __ATOMIC_RELEASE);
}
Result __wrap_threadCreate(Thread *t, ThreadFunc entry, void *arg, void *stack_mem, size_t stack_sz, int prio, int cpuid) {
    const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
    if (R_SUCCEEDED(rc) && t) ps3_profile_note_thread((uint32_t)t->handle, (uintptr_t)entry);
    return rc;
}

/* ---- one sample */
static uint8_t g_window[WINDOW_BYTES];      /* the sampler thread's own copy of the target's stack top */
static uint64_t g_paused_ticks;            /* system ticks the sampled threads spent paused by this sampler since the last dump (the cost of the profiler to the guest) */
static uint64_t g_pause_count;
static inline uint64_t load64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
/* Returns 1 and fills `out` when the thread could be read. `chain`: also read the frame chain. Between the first SVC and the last, nothing that takes a lock runs. */
static int sample_thread(Handle handle, ps3_profile_sample *out, int chain) {
    ThreadContext ctx;
    size_t have = 0;
    const uint64_t paused_at = armGetSystemTick();
    if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused))) return 0;
    const int ok = R_SUCCEEDED(svcGetThreadContext3(&ctx, handle));
    if (ok && chain) {
        MemoryInfo info; u32 page_info;
        if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, ctx.sp)) && (info.perm & Perm_R) && ctx.sp >= info.addr && ctx.sp < info.addr + info.size) {
            const uint64_t available = info.addr + info.size - ctx.sp;
            have = available < WINDOW_BYTES ? (size_t)available : WINDOW_BYTES;
            memcpy(g_window, (const void *)ctx.sp, have);
        }
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);        /* always, whatever happened above */
    g_paused_ticks += armGetSystemTick() - paused_at; ++g_pause_count;
    if (!ok) return 0;
    memset(out, 0, sizeof *out);
    out->pc = ctx.pc.x; out->lr = ctx.lr;
    if (chain && have) {
        uint64_t at = ctx.fp;
        while (out->frames < PS3_PROFILE_FRAMES && !(at & 7u) && at >= ctx.sp && at + 16u <= ctx.sp + have) {
            const uint64_t previous = load64(g_window + (at - ctx.sp)), ret = load64(g_window + (at - ctx.sp) + 8u);
            if (!ret) break;
            out->ret[out->frames++] = ret;
            if (previous <= at) break;
            at = previous;
        }
    }
    return 1;
}

/* ---- the dump */
static uint64_t g_main_samples_total;
static void write_dump(uint64_t start_tick, unsigned hz, unsigned interval, const char *name) {
    static const char *const names[] = { "ps3_profile_start", "ps3_profile_note_thread", "__wrap_threadCreate" };
    uint64_t addresses[3]; ps3_profile_thread_info threads[PS3_PROFILE_MAX_THREADS + 1]; unsigned count = 0;
    addresses[0] = (uint64_t)(uintptr_t)&ps3_profile_start; addresses[1] = (uint64_t)(uintptr_t)&ps3_profile_note_thread; addresses[2] = (uint64_t)(uintptr_t)&__wrap_threadCreate;
    for (unsigned slot = 0; slot < PS3_PROFILE_MAX_THREADS && slot < g_reserved; ++slot) {
        if (!__atomic_load_n(&g_ready[slot], __ATOMIC_ACQUIRE)) continue;
        threads[count] = (ps3_profile_thread_info){ g_handles[slot], g_entries[slot], g_thread_samples[slot], slot, g_handles[slot] == g_main_handle };
        ++count;
    }
    char path[200];
    snprintf(path, sizeof path, "%s%s", PS3_PROFILE_DIR, name);
    FILE *f = fopen(path, "wb");
    const uint64_t seconds = (armGetSystemTick() - start_tick) / armGetSystemTickFreq();
    const int lines = ps3_profile_core_write(f, (unsigned)seconds, interval, hz, names, addresses, 3, threads, count, 3000);
    uint64_t main_samples = 0, other_samples = 0, with_chain = 0, dropped = 0;
    ps3_profile_core_totals(&main_samples, &other_samples, &with_chain, &dropped);
    if (f) fclose(f);
    const uint64_t interval_ticks = (uint64_t)(interval ? interval : 1u) * armGetSystemTickFreq();
    const uint64_t paused_us = g_paused_ticks * 1000000ull / armGetSystemTickFreq();
    printf("STARTUP_PROFILER seconds=%llu file=%s lines=%d main_samples=%llu other_samples=%llu with_chain=%llu dropped=%llu hz=%u pauses=%llu paused_us_each=%llu paused_permille_of_one_thread=%llu\n",
           (unsigned long long)seconds, name, lines, (unsigned long long)main_samples, (unsigned long long)other_samples, (unsigned long long)with_chain, (unsigned long long)dropped, hz,
           (unsigned long long)g_pause_count, (unsigned long long)(g_pause_count ? paused_us / g_pause_count : 0), (unsigned long long)(g_paused_ticks * 1000ull / interval_ticks));
    g_paused_ticks = 0; g_pause_count = 0;
}

static void sampler_main(void *argument) {
    (void)argument;
    unsigned hz = DEFAULT_HZ;
    {   /* profile-rate.txt: the main-thread rate in Hz */
        char path[200]; snprintf(path, sizeof path, "%sprofile-rate.txt", PS3_PROFILE_DIR);
        FILE *f = fopen(path, "rb"); unsigned value = 0;
        if (f) { if (fscanf(f, "%u", &value) == 1 && value >= 100u && value <= 2000u) hz = value; fclose(f); }
    }
    const uint64_t period_ticks = armGetSystemTickFreq() / hz;
    const uint64_t start_tick = armGetSystemTick();
    uint64_t next_tick = start_tick + period_ticks, interval_start = start_tick, tick_index = 0, last_check = start_tick;
    unsigned next_other = 0;
    time_t last_trigger = 0;
    char path[200]; snprintf(path, sizeof path, "%sprofile-now.txt", PS3_PROFILE_DIR);
    {   struct stat st; if (stat(path, &st) == 0) last_trigger = st.st_mtime; }     /* a trigger file left from an earlier run does not fire */
    ps3_profile_core_reset();
    printf("STARTUP_PROFILER started hz=%u main_handle=0x%x window=%u threads_so_far=%u\n", hz, (unsigned)g_main_handle, (unsigned)WINDOW_BYTES, (unsigned)g_reserved);
    for (;;) {
        const uint64_t now = armGetSystemTick();
        if (now < next_tick) svcSleepThread((s64)((next_tick - now) * 1000000000ull / armGetSystemTickFreq()));
        next_tick += period_ticks;
        if (armGetSystemTick() > next_tick + 50 * period_ticks) next_tick = armGetSystemTick() + period_ticks;      /* far behind (a stall): do not burst */
        ++tick_index;
        if (g_main_handle) {
            ps3_profile_sample sample;
            if (sample_thread(g_main_handle, &sample, 1)) { ps3_profile_core_add_main(&sample); ++g_main_samples_total; }
        }
        if (tick_index % OTHER_EVERY == 0) {
            const unsigned count = g_reserved < PS3_PROFILE_MAX_THREADS ? g_reserved : PS3_PROFILE_MAX_THREADS;
            for (unsigned tries = 0; tries < count; ++tries) {
                const unsigned slot = next_other++ % count;
                if (!__atomic_load_n(&g_ready[slot], __ATOMIC_ACQUIRE) || g_handles[slot] == g_main_handle || g_handles[slot] == g_sampler_handle) continue;
                ps3_profile_sample sample;
                if (sample_thread(g_handles[slot], &sample, 0)) { ps3_profile_core_add_other(slot, sample.pc); ++g_thread_samples[slot]; }
                break;
            }
        }
        const uint64_t after = armGetSystemTick();
        if (after - last_check >= armGetSystemTickFreq()) {          /* once a second: the interval, the trigger file */
            last_check = after;
            struct stat st; int trigger = 0;
            if (stat(path, &st) == 0 && st.st_mtime != last_trigger) { last_trigger = st.st_mtime; trigger = 1; }
            if (trigger || after - interval_start >= (uint64_t)DUMP_SECONDS * armGetSystemTickFreq()) {
                char name[64]; snprintf(name, sizeof name, "profile-%llu.txt", (unsigned long long)((after - start_tick) / armGetSystemTickFreq()));
                write_dump(start_tick, hz, (unsigned)((after - interval_start) / armGetSystemTickFreq()), name);
                ps3_profile_core_reset(); memset(g_thread_samples, 0, sizeof g_thread_samples);
                interval_start = armGetSystemTick(); next_tick = interval_start + period_ticks;
            }
        }
    }
}

void ps3_profile_start(uint32_t main_thread_handle) {
    if (__atomic_exchange_n(&g_started, 1u, __ATOMIC_ACQ_REL)) return;
    {   /* profile-off.txt: do not start */
        char path[200]; snprintf(path, sizeof path, "%sprofile-off.txt", PS3_PROFILE_DIR);
        struct stat st;
        if (stat(path, &st) == 0) { printf("STARTUP_PROFILER off (profile-off.txt)\n"); return; }
    }
    g_main_handle = (Handle)main_thread_handle;
    /* priority 0x2a (above the guest's 0x3B), any allowed core; the thread sleeps nearly all the time. It is made with the REAL threadCreate so it is not in its own table. */
    if (R_FAILED(__real_threadCreate(&g_sampler, sampler_main, NULL, NULL, 0x10000, 0x2a, -2))) { printf("STARTUP_PROFILER thread_create_failed\n"); return; }
    g_sampler_handle = g_sampler.handle;
    if (R_FAILED(threadStart(&g_sampler))) { printf("STARTUP_PROFILER thread_start_failed\n"); threadClose(&g_sampler); return; }
}
#endif
