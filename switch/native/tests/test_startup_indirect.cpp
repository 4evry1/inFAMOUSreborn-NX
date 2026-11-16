#ifndef PS3_NATIVE_STARTUP_SPU_INITIALIZE
#error Stage31 continuation requires genuine guarded SPU admission-policy initialization
#endif
#ifndef PS3_NATIVE_STARTUP_SYSUTIL_CALLBACKS
#error Stage30 continuation requires genuine SYSUTIL registration under assumed module admission
#endif
#ifndef PS3_NATIVE_DIAGNOSTIC_SYSMODULE_ADMISSION
#error Stage29 diagnostic continuation requires explicit assumed module admission
#endif
#ifndef PS3_NATIVE_STARTUP_EXITSPAWN
#error Stage28 discovery requires EXITSPAWN registration
#endif
#ifdef PS3_NATIVE_HEAP_THREAD_ID
// Root (2026-11-12): the run deadline of the LLE profile in seconds. 120 is the packaged default; a long Eden run
// (the whole hard-disk install) is built with -DSTARTUP_DEADLINE_SECONDS=<n>.
#ifndef STARTUP_DEADLINE_SECONDS
#define STARTUP_DEADLINE_SECONDS 120
#endif
#include <stdint.h>
extern bool g_ppu_lean_memory;   // ppu_memory_context.cpp (Decision 111); file scope, not the anonymous namespace
uint32_t g_startup_main_thread_handle = 0;   // Decision 116: the guest main thread, for the priority override at the first flip
extern "C" uint64_t g_ps3_spu_loss_signals, g_ps3_spu_loss_waits, g_ps3_spu_loss_woken;   // spu_group_runner.c (Decision 116)
extern "C" void ppu_inline_memory_set(bool on);   // ppu-speed-20261113 item 2: inline loads of the generated game code
extern "C" uint64_t g_ps3_spu_run_ns[8], g_ps3_spu_run_instructions[8];   // Decision 99 (spu_group_runner.c)
#ifdef PS3_SPU_IMAGE_ACCOUNT
#include <stdio.h>
extern "C" void ps3_spu_images_report(void (*print)(const char *line), unsigned seconds);   // Decision 104 (spu_image_account.c): STARTUP_SPU_IMAGES, outside the anonymous namespace
extern "C" void ps3_spu_images_print_line(const char *line) { printf("%s\n", line); }
#endif
#include "barging_lock_horizon.h"
#include "../adapters/startup_execution.h"
#include "../adapters/spu_run_port.h"
#include <stdint.h>
#include <stdio.h>
// Root (2026-11-13, freeze diagnostics A1): STARTUP_SPU_STATE / STARTUP_SPU_TERMINAL / STARTUP_SPU_BLOCKED lines (spu_group_runner.c), default on;
// -DPS3_NO_SPU_STATE removes the calls. Outside the anonymous namespace: a C object.
extern "C" void ps3_spu_state_print_line(const char *line) { printf("%s\n", line); }
// Diagnostics of the flat guest space (guest_space_flat_horizon.c), read by the heartbeat. Declared here, outside any
// namespace, because they are C objects.
extern "C" { extern volatile uint32_t g_ps3_flat_guest_stage; extern const volatile uint32_t *g_ps3_flat_guest_lock_word; }
#ifdef PS3_NATIVE_RSX_RENDER
// Root (2026-11-12, Decision 67): the live renderer. Only in an NRO built with LLE_RSX_RENDER=1.
#include "../adapters/rsx_render_bridge.h"
#endif
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
#include "../adapters/taskwait_horizon.h"
extern "C" void startup_taskwait_print_objects(unsigned seconds);
#endif
#include "../adapters/discovery_budget_binding.h"
#include "../adapters/calendar_clock_horizon.h"
#include "../adapters/pad_port_horizon.h"
#include "../diagnostics/startup_observation.h"
#include "timebase.h"
#if defined(PS3_NATIVE_STARTUP_DISCOVERY) && (!defined(PS3_NATIVE_STARTUP_BUDGET_TEST) || defined(PS3_NATIVE_STARTUP_RECORDER_TEST) || !defined(PS3_NATIVE_STARTUP_CURRENT_TIME) || !defined(PS3_NATIVE_STARTUP_TIMEZONE) || !defined(PS3_NATIVE_STARTUP_TIMEBASE_COUNTER))
#error "One-shot discovery requires the reviewed budget composition without recorder fixtures"
#endif
static_assert(STARTUP_TRACE_CAPACITY == 4096 && STARTUP_IMPORT_CAPACITY == 64,
              "Heap thread-ID scout must retain the existing 0.18 limits");
#if !defined(PS3_NATIVE_HEAP_BACKEND) || !defined(PS3_NATIVE_INDIRECT_SCOUT) || !defined(PS3_NATIVE_STARTUP_TIMEBASE) || !defined(PS3_NATIVE_THREAD_ID_QUERY)
#error "Heap thread-ID requires the heap backend and existing service scout"
#endif
#endif
#ifdef PS3_NATIVE_HEAP_STARTUP
#include "../adapters/startup_execution.h"
static_assert(STARTUP_TRACE_CAPACITY == 2048 && STARTUP_IMPORT_CAPACITY == 32,
              "Heap startup must retain the accepted 0.16 prefix limits");
#if defined(PS3_NATIVE_INDIRECT_SCOUT) || defined(PS3_NATIVE_STARTUP_TIMEBASE) || defined(PS3_NATIVE_THREAD_ID_QUERY)
#error "Heap startup requires the accepted non-scout prefix"
#endif
#endif
#include "../adapters/startup_execution.h"
#include "guest_space.h"
#include "../generated/boot_content_us.h"
#include "../adapters/system_language_horizon.h"
#include "../adapters/compiled_code_table.h"
#include "compiled_code_index.h"
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
#include "guest_timebase.h"
#endif
#include "../adapters/clock_horizon.h"
#ifdef PS3_NATIVE_LLE_SRE
#include "../adapters/lle_sre.h"
#endif
#ifdef PS3_NATIVE_SPU_RUN
#include "../adapters/spu_run_port.h"
#include "../adapters/service_thread_horizon.h"
#include <cstdio>
extern "C" void startup_live_trace_line(void *, const char *line);
// Root (C1b): the process-wide SPU host threads, cancel latch and result printer for the native SPU group runner.
static ps3_horizon_service_thread g_spu_run_threads[PS3_LV2_SPU_THREADS_MAX];
static ps3_service_thread_port g_spu_run_ports[PS3_LV2_SPU_THREADS_MAX];
static unsigned g_spu_run_cancel = 0;
static void spu_run_print(const char *line) { std::printf("%s\n", line); }
#endif
#ifdef PS3_NATIVE_LV2_SYNC
#include <ctime>
#include <time.h>
#endif
#include <unistd.h>
extern "C" uint32_t __nx_applet_exit_mode;
extern "C" int native_test_clock_horizon(void);
extern "C" int native_clock_conversion_tests(void);
int native_test_ppu_memory_trace(void);
#include "elf_image.h"
extern "C" {
#include "../adapters/lwmutex_wait_horizon.h"
#ifdef PS3_NATIVE_FAST_SYNC
#include "../adapters/guest_space_flat_horizon.h"   // ps3_flat_guest_fast_pointer for the lwmutex fast path
#ifdef PS3_NATIVE_PROFILER
#include "../adapters/ps3_profile_switch.h"
#endif
#endif
}
#ifdef PS3_NATIVE_GCM_INIT
#include "../adapters/demand_window.h"
#endif
#ifdef PS3_NATIVE_AUDIO_OUT
#ifndef PS3_NATIVE_LLE_SRE
#error "PS3_NATIVE_AUDIO_OUT plays the LLE build's audio service: it needs PS3_NATIVE_LLE_SRE"
#endif
#include "../adapters/startup_audio_out.h"   // Root (2026-11-15): the console's sound output (audout) behind the audio ticker's service
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
#include "../adapters/startup_spurs_composition.h"
#include "../adapters/startup_wws_composition.h"
#include "../adapters/startup_spurs_sync_report.h"
#include "../adapters/startup_child_composition.h"
#include "../adapters/wait_port_horizon.h"
#include "../adapters/service_thread_horizon.h"
#endif
#ifdef PS3_NATIVE_GAMEDATA
#include "../adapters/gamedata_storage_posix.h"    // POSIX storage behind the guest /dev_hdd0/game root
#endif
#include "../adapters/startup_capacity_policy.h"   // shared plain capacity profile: used by every composition mode
#include <switch.h>
#include <sys/stat.h>
// Root diagnostics (racy on purpose), defined in startup_child_composition.cpp and domain/ppu_threads.c.
extern volatile uint32_t g_startup_create_stage;
extern "C" { extern volatile uint32_t g_ps3_ppu_start_stage; extern uint32_t g_ps3_ppu_prefix_cancel;
  extern uint64_t g_ps3_ppu_prefix_released;
  extern uint64_t g_ps3_ppu_prefix_waits, g_ps3_ppu_prefix_slices, g_ps3_ppu_prefix_max_slices, g_ps3_ppu_prefix_over_1s, g_ps3_ppu_prefix_over_10s; }
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <vector>
#include <memory>
#include <algorithm>

// Root (Decision 86): where guest lock time went. Every 120 s and at the end (seconds = 0xFFFFFFFF).
static void startup_print_lwmutex_time(unsigned seconds)
{
            const ps3_lwmutex_debug_counters &d = g_ps3_lwmutex_debug;
            std::printf("STARTUP_LWMUTEX_TIME seconds=%u locks=%llu unlocks=%llu notify_skipped=%llu guard_ms=%llu guard_max_us=%llu guard_over_1ms=%llu contended=%llu contended_ms=%llu contended_max_us=%llu max_address=0x%x max_owner=0x%x max_waiter=0x%x\n", seconds,
                (unsigned long long)d.lock_calls, (unsigned long long)d.unlock_calls, (unsigned long long)d.notify_skipped,
                (unsigned long long)(d.guard_ns / 1000000u), (unsigned long long)(d.guard_max_ns / 1000u), (unsigned long long)d.guard_over_1ms,
                (unsigned long long)d.lock_contended, (unsigned long long)(d.contended_ns / 1000000u), (unsigned long long)(d.contended_max_ns / 1000u),
                unsigned(d.max_address), unsigned(d.max_owner), unsigned(d.max_waiter));
#ifdef PS3_NATIVE_FAST_SYNC
            {   // lock-free operations are not in the counters above; fallbacks are the ones that tried the fast path and went to the guarded one
                uint64_t fast_locks = 0, fast_unlocks = 0, fast_fallbacks = 0;
                ps3_lwmutex_fast_totals(nullptr, &fast_locks, &fast_unlocks, &fast_fallbacks);
                std::printf("STARTUP_LWMUTEX_FAST seconds=%u fast_locks=%llu fast_unlocks=%llu fast_fallbacks=%llu\n", seconds,
                    (unsigned long long)fast_locks, (unsigned long long)fast_unlocks, (unsigned long long)fast_fallbacks);
                uint64_t why[PS3_LWMUTEX_WHY_COUNT];
                ps3_lwmutex_fast_reasons(nullptr, why);
                std::printf("STARTUP_LWMUTEX_FAST_WHY seconds=%u cancel=%llu identity_or_recursion=%llu sticky_line=%llu held_or_not_owner=%llu waiters=%llu other=%llu\n", seconds,
                    (unsigned long long)why[PS3_LWMUTEX_WHY_CANCEL], (unsigned long long)why[PS3_LWMUTEX_WHY_IDENTITY], (unsigned long long)why[PS3_LWMUTEX_WHY_STICKY],
                    (unsigned long long)why[PS3_LWMUTEX_WHY_HELD], (unsigned long long)why[PS3_LWMUTEX_WHY_WAITERS], (unsigned long long)why[PS3_LWMUTEX_WHY_OTHER]);
            }
#endif
            std::printf("STARTUP_STORE_PATHS seconds=%u conflicts=%llu sticky_stores=%llu late_repeats=%llu sticky_lines=%llu\n", seconds, (unsigned long long)g_ppu_fast_write_conflicts.load(),
                (unsigned long long)g_ppu_fast_write_sticky.load(), (unsigned long long)g_ppu_fast_write_late.load(), (unsigned long long)g_ps3_atomic_sticky_lines);
            /* Decision 112 (rv50s): PPU reservations on the lines the SPUs share. noted = loads of such a line remembered, checked = conditional stores that had a note,
             * lost = stores refused because something wrote the line between the lwarx and the stwcx (each one is a lost wake-up the value check alone would have let through). */
            std::printf("STARTUP_PPU_RESERVATION seconds=%u on=%d noted=%llu checked=%llu lost=%llu\n", seconds, g_ps3_atomic_granule_reservations,
                (unsigned long long)g_ps3_atomic_reservation_noted, (unsigned long long)g_ps3_atomic_reservation_checked, (unsigned long long)g_ps3_atomic_reservation_lost);
            for (unsigned waiter = 0; waiter < unsigned(PS3_LWMUTEX_DEBUG_PENDING); ++waiter)
                if (d.pending[waiter].address)   // threads asleep on a guest mutex now, and who held it when they began
                    std::printf("STARTUP_LWMUTEX_PENDING seconds=%u mutex=0x%x owner=0x%x waiter=0x%x since_ms=%llu\n", seconds, unsigned(d.pending[waiter].address),
                        unsigned(d.pending[waiter].owner), unsigned(d.pending[waiter].waiter), (unsigned long long)(d.pending[waiter].since / 1000000u));
            for (unsigned index = 0; index < unsigned(PS3_LWMUTEX_DEBUG_TOP); ++index)
                if (d.top[index].count)
                    std::printf("STARTUP_LWMUTEX_TOP address=0x%x count=%llu ms=%llu last_owner=0x%x last_waiter=0x%x\n", unsigned(d.top[index].address),
                        (unsigned long long)d.top[index].count, (unsigned long long)(d.top[index].ns / 1000000u), unsigned(d.top[index].last_owner), unsigned(d.top[index].last_waiter));
}
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
#ifdef PS3_NATIVE_LLE_SRE
// Root (2026-11-11): fixed guest window of the audio service. Read indexes (8 x be64) at +0, port buffers at
// +0x10000 + port * 0x40000 (8 channels x 32 blocks x 256 samples x 4 bytes = 0x40000 is the largest port).
constexpr uint32_t AUDIO_WINDOW = 0x0e200000, AUDIO_WINDOW_SIZE = 0x210000;
constexpr uint32_t AUDIO_WINDOW_BUFFERS = AUDIO_WINDOW + 0x10000, AUDIO_WINDOW_STRIDE = 0x40000;
bool audio_window_overlaps(uint32_t base, uint64_t size)
{
    return uint64_t(AUDIO_WINDOW) < uint64_t(base) + size &&
           uint64_t(base) < uint64_t(AUDIO_WINDOW) + AUDIO_WINDOW_SIZE;
}
// The ticker: one host thread that advances the read index of every running port in real time (the game follows
// playback by reading that guest word). It only calls startup_audio_tick and sleeps 2 ms.
// Root (2026-11-12): a line every 10 s, flushed, so a run that never reaches its summary still shows how far it got.
std::atomic<uint32_t> g_startup_heartbeat_flips{0};
std::atomic<uint64_t> g_startup_ticker_ticks{0};
// For the heartbeat: where the tick thread is (1 audio, 2 timers, 3 RSX pump, 4 asleep) and the raw words of the
// locks a frozen run could be stuck on (a libnx Mutex word is its owner's thread handle, bit 30 = waiters).
std::atomic<uint32_t> g_startup_ticker_stage{0};
const Mutex *g_hb_domain_mutex = nullptr, *g_hb_window_mutex = nullptr;
const ps3_atomic_domain *g_hb_domain = nullptr;
uint32_t g_hb_ticker_handle = 0;
// Its own thread, never touching guest state: it keeps printing when the tick thread or the game is stuck, and the
// tick count in the line says which of the two it is.
extern "C" uint64_t ps3_atomic_domain_debug_enter_count(unsigned line);
extern "C" void *ps3_flat_guest_fast_pointer(uint32_t address, uint32_t length);
extern "C" void ps3_flat_guest_debug(uint64_t out[7]);
extern "C" void native_status_row(int row, const char *text);
extern "C" void native_log_flush(void) __attribute__((weak));
extern "C" int ps3_spu_neon_selftest(char *report, unsigned report_bytes) __attribute__((weak));   // native/spu_neon
extern "C" void rsx_render_bridge_times(uint64_t *method_ticks, uint64_t *flip_ticks) __attribute__((weak));   // Decision 93: the log is written in blocks
extern "C" void startup_rsx_print_stats(unsigned seconds) __attribute__((weak));   // startup_execution.cpp (LLE builds only): STARTUP_RSX_PUMP, the consumer's own time (renderer CPU work 2026-11-15)
struct StartupHeartbeat {
    std::atomic<bool> stop{false};
    Thread thread{};
    bool started = false;
    const std::atomic<uint64_t> *rsx_methods = nullptr;   // optional
    static void run(void *opaque)
    {
        auto &self = *static_cast<StartupHeartbeat *>(opaque);
        unsigned seconds = 0, slices = 0;
        while (!self.stop.load(std::memory_order_acquire)) {
            svcSleepThread(INT64_C(2000000));   // 2 ms: the activity profile samples at this rate
            startup_activity_sample();
            {   // Root (2026-11-12, user request): on-screen status. A spinner twice a second, so a frozen program is visible as a frozen spinner.
                static uint64_t status_start = armGetSystemTick(), status_last = 0; static unsigned status_spin = 0;
                const uint64_t status_now = armGetSystemTick();
                if (armTicksToNs(status_now - status_last) >= UINT64_C(500000000)) {   // twice a second: each update redraws the whole screen
                    status_last = status_now;
                    char status[160];
                    std::snprintf(status, sizeof status, "[%c] running %llu s of %u   game frames: %u", "|/-\\"[status_spin++ & 3u],
                        (unsigned long long)(armTicksToNs(status_now - status_start) / UINT64_C(1000000000)), unsigned(STARTUP_DEADLINE_SECONDS),
                        unsigned(g_startup_heartbeat_flips.load()));
                    native_status_row(4, status);
                    if ((status_spin & 3u) == 0u && native_log_flush) native_log_flush();   // every 2 s
                }
            }
#ifndef PS3_NO_SPU_STATE
            {   // Root (freeze diagnostics A1): once a second, print the SPU threads that ended, or stalled on one channel / sat inside one slice for over 5 s (once each).
                static uint64_t watch_start = armGetSystemTick(), watch_last = 0;
                const uint64_t watch_now = armGetSystemTick();
                if (armTicksToNs(watch_now - watch_last) >= UINT64_C(1000000000)) {
                    watch_last = watch_now;
                    if (ps3_spu_run_watch(ps3_spu_state_print_line, unsigned(armTicksToNs(watch_now - watch_start) / UINT64_C(1000000000))) && native_log_flush) native_log_flush();
                }
            }
#endif
            if (++slices < 5) continue;   // a 2 ms sleep takes longer than 2 ms: the 10 s mark is taken from the clock
            slices = 0;
            {
                static uint64_t last_tick = armGetSystemTick();
                if (armTicksToNs(armGetSystemTick() - last_tick) < UINT64_C(10000000000)) continue;
                last_tick += armNsToTicks(UINT64_C(10000000000));
            }
            if (false) continue;
            {   // Root (Decision 87): how idle each CPU core was in the last 10 s. Tells "waiting" from "out of CPU".
                static uint64_t idle_before[4], tick_before = 0;
                const uint64_t tick_now = armGetSystemTick();
                char line[160]; int used = std::snprintf(line, sizeof line, "STARTUP_CORE_IDLE seconds=%u", seconds + 10u);
                for (int core = 0; core < 4; ++core) {
                    uint64_t idle = 0;
                    const Result rc = svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, uint64_t(core));
                    if (R_FAILED(rc)) used += std::snprintf(line + used, sizeof line - size_t(used), " core%d=rc0x%x", core, unsigned(rc));
                    else if (tick_before && tick_now > tick_before)
                        used += std::snprintf(line + used, sizeof line - size_t(used), " core%d=%u%%", core, unsigned((idle - idle_before[core]) * 100u / (tick_now - tick_before)));
                    idle_before[core] = idle;
                }
                tick_before = tick_now;
                std::printf("%s\n", line);
            }
            if ((seconds + 10u) % 120u == 0u) { startup_activity_print_calls(seconds + 10u); startup_print_lwmutex_time(seconds + 10u); }
            if ((seconds + 10u) % 60u == 0u) {   // Root (Decision 96): per-thread sample profile (cumulative) and renderer time, each minute
                std::printf("STARTUP_ACTIVITY_AT seconds=%u\n", seconds + 10u);
                startup_activity_print();
                {   // SPU thread 0x9D000N00: milliseconds inside SPU code and millions of instructions (lifted code counts none)
                    char line[320]; int used = std::snprintf(line, sizeof line, "STARTUP_SPU_TIME seconds=%u", seconds + 10u);
                    for (unsigned which = 0; which < 8u; ++which)
                        used += std::snprintf(line + used, sizeof line - size_t(used), " t%u=%llums/%lluM", which,
                            (unsigned long long)(__atomic_load_n(&::g_ps3_spu_run_ns[which], __ATOMIC_RELAXED) / 1000000u),
                            (unsigned long long)(__atomic_load_n(&::g_ps3_spu_run_instructions[which], __ATOMIC_RELAXED) / 1000000u));
                    std::printf("%s\n", line);
                }
#ifndef PS3_NO_SPU_STATE
                ps3_spu_run_state_report(ps3_spu_state_print_line, seconds + 10u);   // freeze diagnostics A1: what is every SPU thread doing now
#endif
#ifdef PS3_SPU_IMAGE_ACCOUNT
                ps3_spu_images_report(ps3_spu_images_print_line, seconds + 10u);   // Decision 104: top 12 SPU images by time, once a minute
#endif
                if (rsx_render_bridge_times) {
                    uint64_t method_ticks = 0, flip_ticks = 0;
                    rsx_render_bridge_times(&method_ticks, &flip_ticks);
                    std::printf("STARTUP_RENDER_TIME seconds=%u methods_ms=%llu flips_ms=%llu\n", seconds + 10u,
                        (unsigned long long)(armTicksToNs(method_ticks) / 1000000u), (unsigned long long)(armTicksToNs(flip_ticks) / 1000000u));
                }
                if (startup_rsx_print_stats) startup_rsx_print_stats(seconds + 10u);
            }
            {   // Root (2026-11-12): <app dir>/peek.txt = "hexaddr:words" (one range); printed every 10 s from the flat guest window.
                static int peek_state = -1; static unsigned peek_address = 0, peek_words = 0;
                if (peek_state < 0) {
                    peek_state = 0;
                    if (FILE *file = std::fopen("sdmc:/switch/infamous-native-spu-stage31/peek.txt", "rb")) {
                        if (std::fscanf(file, "%x:%u", &peek_address, &peek_words) == 2 && peek_words && peek_words <= 96u) peek_state = 1;
                        std::fclose(file);
                    }
                    // Root (Decision 86): with no peek.txt, watch the loader's SPURS event flag (0x406c6a00) and
                    // job queue (0x406c6a80), once a minute: a loader thread waits on that flag when loading stops.
                    if (peek_state == 0) { peek_address = 0x406c6a00u; peek_words = 96u; peek_state = 2; }
                }
                if (peek_state == 1 || (peek_state == 2 && (seconds + 10u) % 60u == 0u)) {
                    std::printf("STARTUP_PEEK seconds=%u base=0x%08x:", seconds + 10u, peek_address);
                    for (unsigned word = 0; word < peek_words; ++word) {
                        const unsigned char *bytes = static_cast<const unsigned char *>(ps3_flat_guest_fast_pointer(peek_address + word * 4u, 4u));
                        if (bytes) std::printf(" %02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3]); else std::printf(" --------");
                    }
                    std::printf("\n");
                }
            }
            slices = 0; seconds += 10;
            // Root (Decision 111): the lean memory path of generated code starts once the game has shown its first
            // frame; the start-up phases before that keep the fully checked path they were built around.
            if (!::g_ppu_lean_memory && g_startup_heartbeat_flips.load() > 0u) {
                ::g_ppu_lean_memory = true;
                ppu_inline_memory_set(true);   // no effect unless the game units were built with native/adapters/ppu_inline_memory.h
                std::printf("STARTUP_PPU_LEAN_MEMORY on seconds=%u flips=%u\n", seconds, unsigned(g_startup_heartbeat_flips.load()));
                // Decision 116: LLE_MAIN_PRIORITY (decimal, 28..63; sibling-env.txt) replaces the main thread's priority.
                if (const char *text = std::getenv("LLE_MAIN_PRIORITY")) {
                    const int wanted = std::atoi(text);
                    if (::g_startup_main_thread_handle && wanted >= 28 && wanted <= 63) {
                        const Result rc = svcSetThreadPriority(::g_startup_main_thread_handle, u32(wanted));
                        std::printf("STARTUP_MAIN_THREAD_PRIORITY override=%d rc=0x%x\n", wanted, unsigned(rc));
                    }
                }
            }
            if (seconds % 60u == 0u)   // Decision 116: idle SPU waits on RdEventStat, how many a reservation loss cut short
                std::printf("STARTUP_SPU_LOSS_WAKE seconds=%u waits=%llu woken=%llu signals=%llu\n", seconds,
                    static_cast<unsigned long long>(__atomic_load_n(&g_ps3_spu_loss_waits, __ATOMIC_RELAXED)),
                    static_cast<unsigned long long>(__atomic_load_n(&g_ps3_spu_loss_woken, __ATOMIC_RELAXED)),
                    static_cast<unsigned long long>(__atomic_load_n(&g_ps3_spu_loss_signals, __ATOMIC_RELAXED)));
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
            if (seconds % 60u == 0u)   // Decision 120/121: main-thread SPU task waits cut short by a guarded write; lines an atomic ever touched
                std::printf("STARTUP_TASKWAIT_WAKE seconds=%u waits=%llu woken=%llu signals=%llu touched_lines=%llu\n", seconds,
                    static_cast<unsigned long long>(__atomic_load_n(&g_startup_taskwait_waits, __ATOMIC_RELAXED)),
                    static_cast<unsigned long long>(__atomic_load_n(&g_startup_taskwait_woken, __ATOMIC_RELAXED)),
                    static_cast<unsigned long long>(__atomic_load_n(&g_startup_taskwait_signals, __ATOMIC_RELAXED)),
                    static_cast<unsigned long long>(g_ps3_atomic_touched_lines));
            if (seconds % 60u == 0u) startup_taskwait_print_objects(seconds);
            if (seconds % 60u == 0u) {   // Decision 123: per writer tag (0 PPU/other, N = SPU slot N-1): writes into the watched window while a wait paused, and which ended the wait
                std::printf("STARTUP_TASKWAIT_WAKERS seconds=%u", seconds);
                for (unsigned tag = 0; tag < STARTUP_TASKWAIT_TAGS; ++tag)
                    std::printf(" t%u=%llu/%llu", tag, (unsigned long long)__atomic_load_n(&g_startup_taskwait_overlap_by_tag[tag], __ATOMIC_RELAXED),
                                (unsigned long long)__atomic_load_n(&g_startup_taskwait_end_by_tag[tag], __ATOMIC_RELAXED));
                std::printf("\n");
            }   // rv62s diagnostic: which guest objects the main thread waits on
#endif
            {
                static unsigned status_log_row = 7;
                char note[160];
                std::snprintf(note, sizeof note, "%4u s: %u game frames, %llu timer ticks", seconds, unsigned(g_startup_heartbeat_flips.load()),
                    static_cast<unsigned long long>(g_startup_ticker_ticks.load()));
                native_status_row(int(status_log_row), note);
                status_log_row = status_log_row >= 40u ? 7u : status_log_row + 1u;
            }
            std::printf("STARTUP_HEARTBEAT seconds=%u flips=%u ticks=%llu repeated_stores=%llu ticker_stage=%u ticker_handle=0x%x domain_lock=0x%x window_lock=0x%x domain_line=%u flat_stage=%u flat_lock=0x%x\n", seconds,
                g_startup_heartbeat_flips.load(), static_cast<unsigned long long>(g_startup_ticker_ticks.load()),
                static_cast<unsigned long long>(g_ppu_fast_write_conflicts.load()), g_startup_ticker_stage.load(), g_hb_ticker_handle,
                g_hb_domain_mutex ? unsigned(*reinterpret_cast<const volatile uint32_t *>(g_hb_domain_mutex)) : 0u,
                g_hb_window_mutex ? unsigned(*reinterpret_cast<const volatile uint32_t *>(g_hb_window_mutex)) : 0u,
                g_hb_domain ? unsigned(g_hb_domain->debug_line) : 0u, unsigned(g_ps3_flat_guest_stage),
                g_ps3_flat_guest_lock_word ? unsigned(*g_ps3_flat_guest_lock_word) : 0u);
            std::fflush(stdout);
        }
    }
};
StartupHeartbeat startup_heartbeat;
struct StartupAudioTicker {
    StartupAudioProcess *process = nullptr;
    StartupTimerProcess *timers = nullptr;      // lv2 timers (syscalls 70..76) expire on this thread too
    StartupRsxProcess *rsx = nullptr;           // the live RSX command-buffer consumer is pumped here too
    const ps3_guest_memory *memory = nullptr;
    const ps3_clock *clock = nullptr;
    std::atomic<bool> stop{false};
    Thread thread{};
    bool started = false;
    uint64_t clock_failures = 0;
    static void run(void *opaque)
    {
        auto &self = *static_cast<StartupAudioTicker *>(opaque);
        while (!self.stop.load(std::memory_order_acquire)) {
            uint64_t now_us = 0;
            if (self.clock->now_microseconds(self.clock->context, &now_us) == PS3_CLOCK_OK) {
                g_startup_ticker_stage.store(1, std::memory_order_relaxed);
                startup_audio_tick(*self.process, *self.memory, now_us);
                g_startup_ticker_stage.store(2, std::memory_order_relaxed);
                if (self.timers) startup_timer_fire(*self.timers, now_us);
                g_startup_ticker_stage.store(3, std::memory_order_relaxed);
                if (self.rsx && !self.rsx_own_thread) startup_rsx_pump(*self.rsx, *self.memory, 0x20000);
                g_startup_ticker_stage.store(4, std::memory_order_relaxed);
            } else ++self.clock_failures;
            g_startup_ticker_ticks.fetch_add(1, std::memory_order_relaxed);
            svcSleepThread(INT64_C(1000000));   // 1 ms: the resolution of guest lv2 timers
        }
#ifdef PS3_NATIVE_RSX_RENDER
        if (!self.rsx_own_thread) rsx_render_bridge_stop();   // on the thread that drove it
#endif
    }
    bool rsx_own_thread = false;   // Decision 117: set when StartupRsxPumpThread runs the RSX consumer instead
};
// Root (2026-11-15, Decision 117): the RSX command consumer (and with it the whole renderer's CPU side) ran on the 1 ms
// ticker above. In gameplay one pump took ~250 ms (rv57s and rv58s: STARTUP_HEARTBEAT ticks ~4/s, ticker_stage=3), so
// guest lv2 timers (the 5.3 ms audio timer among them) and the audio read index advanced ~4 times a second instead of
// ~190 and 1000. The consumer now has its own thread; the ticker keeps its beat. PS3_NATIVE_RSX_THREAD=0 restores it.
#ifndef PS3_NATIVE_RSX_THREAD
#define PS3_NATIVE_RSX_THREAD 1
#endif
std::atomic<uint64_t> g_startup_rsx_pumps{0};
struct StartupRsxPumpThread {
    StartupRsxProcess *rsx = nullptr;
    const ps3_guest_memory *memory = nullptr;
    std::atomic<bool> stop{false};
    Thread thread{};
    bool started = false;
    static void run(void *opaque)
    {
        auto &self = *static_cast<StartupRsxPumpThread *>(opaque);
        while (!self.stop.load(std::memory_order_acquire)) {
            startup_rsx_pump(*self.rsx, *self.memory, 0x20000);
            g_startup_rsx_pumps.fetch_add(1, std::memory_order_relaxed);
            svcSleepThread(INT64_C(500000));   // 0.5 ms between budgets (the ticker gave 1 ms)
        }
#ifdef PS3_NATIVE_RSX_RENDER
        rsx_render_bridge_stop();   // on the thread that drove it
#endif
    }
};
#endif
#ifdef PS3_NATIVE_GCM_INIT
// Fixed native GCM-init window, checked against every other committed window before use.
constexpr uint32_t GCM_INIT_WINDOW = 0x0e100000, GCM_INIT_WINDOW_SIZE = 0x20000;
constexpr uint32_t GCM_INIT_CONTEXT = GCM_INIT_WINDOW + 0x0000;
constexpr uint32_t GCM_INIT_OPD = GCM_INIT_WINDOW + 0x0100;
constexpr uint32_t GCM_INIT_CALLBACK_CODE = GCM_INIT_WINDOW + 0x0200;
constexpr uint32_t GCM_INIT_CALLBACK_TOC = 0x0078bf78;
constexpr uint32_t GCM_INIT_CONTROL = GCM_INIT_WINDOW + 0x1040;
constexpr uint32_t GCM_INIT_OFFSET_TABLE = GCM_INIT_WINDOW + 0x2000;
constexpr uint32_t GCM_INIT_IO_TABLE = GCM_INIT_WINDOW + 0x4000;
constexpr uint32_t GCM_INIT_REPORTS = GCM_INIT_WINDOW + 0x10000;
constexpr uint32_t GCM_INIT_REPORTS_SIZE = 0x10000;
// RSX local-memory window (commit-on-demand): the only thing that admits the local range.
constexpr uint32_t GCM_LOCAL_BASE = GCM_INIT_LOCAL_BASE, GCM_LOCAL_SIZE = GCM_INIT_LOCAL_SIZE;
constexpr uint32_t GCM_LOCAL_GRANULE = 0x10000;
constexpr uint32_t GCM_LOCAL_CAP = GCM_INIT_LOCAL_SIZE;   // Decision 90: all of video memory (was 0x08000000; rv21: exhaustion=cap at 0xad80000 refused an SPU job's PUT)
// Composition-owned (never on a thread stack); re-created by startup_gcm_init_attach.
gcm_init_service gcm_init_storage;
#ifdef PS3_NATIVE_STARTUP_TTY
// Composition-owned console sink for lv2 syscall 403 (sys_tty_write). One stable static object, borrowed by the primary
// execution before it starts; the same instance is what the after-join STARTUP_TTY lines read. Children are NOT wired to it:
// that would need startup_child_* changes, which the task forbids.
StartupTtyRing startup_tty_ring;
#endif
#ifdef PS3_NATIVE_STARTUP_FS
// Composition-owned file-system composition (decision 24). One stable static
// object graph for the whole process: the POSIX storage adapter, its injected port and the ~42 KiB fs_service. The mount table is
// configured once, BEFORE the parent starts, so the six `sys_fs` imports see a composed root for the whole run.
// The console root is the SD stage directory; the host tests point the same code path at a scratch directory under the build dir
// (STARTUP_FS_TEST_ROOT below overrides it, and nothing else in this file knows the difference).
constexpr const char *STARTUP_FS_ROOT_PRODUCTION = "sdmc:/switch/infamous-native-spu-stage31";
#if defined(STARTUP_FS_TEST_ROOT)
constexpr const char *STARTUP_FS_ROOT = STARTUP_FS_TEST_ROOT;
#else
constexpr const char *STARTUP_FS_ROOT = STARTUP_FS_ROOT_PRODUCTION;
#endif
fs_storage_posix startup_fs_storage;
StartupFsContext startup_fs_context;
Mutex startup_fs_mutex{};
#endif
// Composition-owned commit-on-demand decorator over the domain backing; its guard is
// distinct from the heap backing guard and from the atomic-domain guard (domain ->
// window -> backing lock order).
ps3_demand_window gcm_local_window_storage;
Mutex gcm_local_window_mutex{};
gcm_init_layout gcm_init_window_layout()
{
    return {GCM_INIT_CONTEXT, GCM_INIT_OPD, GCM_INIT_CALLBACK_CODE, GCM_INIT_CALLBACK_TOC,
            GCM_INIT_CONTROL, GCM_INIT_REPORTS, GCM_INIT_REPORTS_SIZE,
            GCM_INIT_OFFSET_TABLE, GCM_INIT_IO_TABLE};
}
#ifdef PS3_NATIVE_GCM_INIT
// Root (2026-11-11): RSX frame capture for the host replay tool. Every method the live consumer forwards is
// recorded from the start (state set in earlier frames is needed to replay a later one); at flip number
// RSX_CAPTURE_AT_FLIP the method log, the RSX-visible guest memory (the IO mapping and the committed part of
// local memory) and a small state file are written to <app dir>/capture/. One capture per run. Diagnostic only.
#ifndef RSX_CAPTURE_AT_FLIP_NUMBER
#define RSX_CAPTURE_AT_FLIP_NUMBER 60
#endif
constexpr uint32_t RSX_CAPTURE_AT_FLIP = RSX_CAPTURE_AT_FLIP_NUMBER;
constexpr size_t RSX_CAPTURE_MAX_RECORDS = 8u * 1024u * 1024u;   // 96 MiB of triples at most
struct StartupRsxCapture {
    // Root (2026-11-12, Decision 65): only the CURRENT frame's methods are kept as records. Everything earlier is
    // folded, at each flip, into a snapshot of the state it left: the last value of every NV4097 register, the
    // vertex program words and the vertex constants. Keeping every method since boot (the first version) grew to
    // tens of MiB in the scene phase and ended the process when the allocation failed (r38b, r39, r48).
    std::vector<uint32_t> records;      // this frame: subchannel, method, data
    enum : uint32_t { REGS = 0x4000, VP_INSTRUCTIONS = 512, CONSTANTS = 1024 };
    std::vector<uint32_t> reg = std::vector<uint32_t>(REGS), vp = std::vector<uint32_t>(VP_INSTRUCTIONS * 4u),
        constant = std::vector<uint32_t>(CONSTANTS * 4u);
    std::vector<uint8_t> reg_set = std::vector<uint8_t>(REGS), vp_set = std::vector<uint8_t>(VP_INSTRUCTIONS),
        constant_set = std::vector<uint8_t>(CONSTANTS);
    uint32_t vp_load = 0, constant_load = 0;
    static bool draws(uint32_t method)
    {
        return method == 0x1808u || method == 0x1814u || method == 0x1818u || method == 0x1824u || method == 0x1D94u;
    }
    void fold(uint32_t subchannel, uint32_t method, uint32_t data)
    {
        if (subchannel != 0 || method >= REGS * 4u || draws(method)) return;
        if (method == 0x1E9Cu) { vp_load = data; return; }            // SET_TRANSFORM_PROGRAM_LOAD
        if (method == 0x1EFCu) { constant_load = data; return; }      // SET_TRANSFORM_CONSTANT_LOAD
        if (method >= 0x0B80u && method < 0x0C00u) {                  // SET_TRANSFORM_PROGRAM: 4 words an instruction
            const uint32_t word = ((method - 0x0B80u) >> 2) & 3u;
            if (vp_load < VP_INSTRUCTIONS) { vp[vp_load * 4u + word] = data; vp_set[vp_load] = 1; }
            if (word == 3u) ++vp_load;
            return;
        }
        if (method >= 0x1F00u && method < 0x1F80u) {                  // SET_TRANSFORM_CONSTANT: load index + offset
            const uint32_t slot = (method - 0x1F00u) >> 2, index = constant_load + slot / 4u;
            if (index < CONSTANTS) { constant[index * 4u + slot % 4u] = data; constant_set[index] = 1; }
            return;
        }
        reg[method >> 2] = data; reg_set[method >> 2] = 1;
    }
    static bool put(std::FILE *file, uint32_t a, uint32_t b, uint32_t c)
    {
        const uint32_t triple[3] = {a, b, c};
        return std::fwrite(triple, sizeof triple, 1, file) == 1;
    }
    // The snapshot as one synthetic frame of ordinary methods, then a flip record (frame number 0).
    bool write_snapshot(std::FILE *file) const
    {
        bool ok = true;
        for (uint32_t index = 0; index < REGS; ++index)
            if (reg_set[index]) ok = put(file, 0, index * 4u, reg[index]) && ok;
        for (uint32_t index = 0; index < VP_INSTRUCTIONS; ++index)
            if (vp_set[index]) {
                ok = put(file, 0, 0x1E9Cu, index) && ok;
                for (uint32_t word = 0; word < 4; ++word) ok = put(file, 0, 0x0B80u + word * 4u, vp[index * 4u + word]) && ok;
            }
        for (uint32_t index = 0; index < CONSTANTS; ++index)
            if (constant_set[index]) {
                ok = put(file, 0, 0x1EFCu, index) && ok;
                for (uint32_t word = 0; word < 4; ++word) ok = put(file, 0, 0x1F00u + word * 4u, constant[index * 4u + word]) && ok;
            }
        // the registers were written last-value-first: restore the two load indices the game left
        ok = put(file, 0, 0x1E9Cu, vp_load) && ok;
        ok = put(file, 0, 0x1EFCu, constant_load) && ok;
        return put(file, 0xFFFFFFFFu, 0, 0) && ok;
    }
    bool render_tried = false;
    uint32_t flips = 0;
    uint32_t at_flip = RSX_CAPTURE_AT_FLIP;   // overridden by <app dir>/capture-at.txt (one decimal number)
    bool done = false, overflow = false;
    const ps3_guest_memory *raw_backing = nullptr;   // below the commit-on-demand window: a read never commits
    const gcm_init_service *gcm = nullptr;
    uint64_t dumped_bytes = 0, dumped_ranges = 0, skipped_granules = 0;
    int write_errors = 0;
    static void method(void *user, uint32_t subchannel, uint32_t method_number, uint32_t data)
    {
        auto &self = *static_cast<StartupRsxCapture *>(user);
#ifdef PS3_NATIVE_RSX_RENDER
        if (!self.render_tried && self.gcm) {
            self.render_tried = true;
            const gcm_as_config &cfg = self.gcm->as.cfg;
            const rsx_render_bridge_config render_config{STARTUP_FS_ROOT, cfg.io_ea, cfg.io_size, cfg.local_base, cfg.local_size};
            (void)rsx_render_bridge_start(&render_config);
        }
        rsx_render_bridge_method(subchannel, method_number, data);
#endif
        if (self.done) return;
        if (self.records.size() / 3 >= RSX_CAPTURE_MAX_RECORDS) { self.overflow = true; return; }
        self.records.push_back(subchannel); self.records.push_back(method_number); self.records.push_back(data);
    }
    void dump_range(std::FILE *file, uint32_t base, uint32_t size)
    {
        // 4 KiB steps: a larger read fails as a whole when one page in it is not committed, and the r33 capture
        // lost the game's glyph atlas that way (only a few of its pages had been written).
        static unsigned char granule[0x1000];
        for (uint64_t offset = 0; offset < size; offset += sizeof(granule)) {
            const uint32_t length = uint32_t(size - offset < sizeof(granule) ? size - offset : sizeof(granule));
            if (raw_backing->read(raw_backing->context, base + uint32_t(offset), granule, length) != PS3_GUEST_MEMORY_OK) {
                ++skipped_granules;   // not committed: nothing there
                continue;
            }
            bool all_zero = true;
            for (uint32_t index = 0; index < length; index += 8) if (granule[index] | granule[index + 4]) { all_zero = false; break; }
            if (all_zero) {
                for (uint32_t index = 0; index < length; ++index) if (granule[index]) { all_zero = false; break; }
                if (all_zero) { ++skipped_granules; continue; }
            }
            const uint32_t header[2] = {base + uint32_t(offset), length};
            if (std::fwrite(header, sizeof(header), 1, file) != 1 || std::fwrite(granule, 1, length, file) != length) ++write_errors;
            dumped_bytes += length; ++dumped_ranges;
        }
    }
    static void flip(void *user, uint32_t buffer_id)
    {
        auto &self = *static_cast<StartupRsxCapture *>(user);
        g_startup_heartbeat_flips.fetch_add(1, std::memory_order_relaxed);
#ifdef PS3_NATIVE_RSX_RENDER
        if (self.gcm) {
            for (unsigned index = 0; index < GCM_INIT_DISPLAY_BUFFER_COUNT; ++index) {
                const gcm_init_display_buffer &display = self.gcm->displays[index];
                if (display.set) rsx_render_bridge_display(index, display.offset, display.pitch, display.width, display.height);
            }
        }
        rsx_render_bridge_flip(buffer_id);
#endif
        if (self.done) return;
        ++self.flips;
        // Root (2026-11-12, Decision 101): without a capture-at.txt the capture is no longer frame 60 (a logo) but
        // the first frame with at least 150,000 recorded methods: the first real scene. That frame replays on the
        // laptop, where the renderer work for it (several colour targets, thousands of draws) can be done and seen.
        const bool big_frame = self.at_flip == RSX_CAPTURE_AT_FLIP && self.records.size() / 3 >= 150000u;
        const bool chosen_frame = self.at_flip != RSX_CAPTURE_AT_FLIP && self.flips == self.at_flip;
        if (!(big_frame || chosen_frame) || !self.raw_backing || !self.gcm) {
            for (size_t index = 0; index + 2 < self.records.size(); index += 3)
                self.fold(self.records[index], self.records[index + 1], self.records[index + 2]);
            self.records.clear();
            return;
        }
        self.records.push_back(0xFFFFFFFFu); self.records.push_back(self.flips); self.records.push_back(buffer_id);
        self.done = true;
        char path[320];
        std::snprintf(path, sizeof path, "%s/capture", STARTUP_FS_ROOT);
        ::mkdir(path, 0777);
        std::snprintf(path, sizeof path, "%s/capture/methods.bin", STARTUP_FS_ROOT);
        if (std::FILE *file = std::fopen(path, "wb")) {
            if (!self.write_snapshot(file)) ++self.write_errors;
            if (std::fwrite(self.records.data(), sizeof(uint32_t), self.records.size(), file) != self.records.size()) ++self.write_errors;
            std::fclose(file);
        } else ++self.write_errors;
        std::snprintf(path, sizeof path, "%s/capture/memory.bin", STARTUP_FS_ROOT);
        if (std::FILE *file = std::fopen(path, "wb")) {
            // Root (2026-11-12, Decision 102): a 4 MiB stream buffer. With the default one every 4 KiB granule was
            // its own SD write: console run rv33 had written 84 MiB of the scene capture in three minutes when the
            // user closed the program, so the capture was unusable.
            static char capture_buffer[4u * 1024u * 1024u];
            std::setvbuf(file, capture_buffer, _IOFBF, sizeof capture_buffer);
            native_status_row(6, "Saving one scene frame to the SD card for analysis (about a minute)...");
            const gcm_as_config &cfg = self.gcm->as.cfg;
            self.dump_range(file, cfg.io_ea, cfg.io_size);
            self.dump_range(file, cfg.local_base, cfg.local_size);
            std::fclose(file);
        } else ++self.write_errors;
        // Everything below the IO mapping (program image, heaps): same record format, for offline inspection of
        // game state. Large (hundreds of MiB). Diagnostic only.
        std::snprintf(path, sizeof path, "%s/capture/main.bin", STARTUP_FS_ROOT);
        if (big_frame) { /* a scene: main.bin would be several hundred MiB more; the replay does not need it */ }
        else if (std::FILE *file = std::fopen(path, "wb")) {
            const gcm_as_config &cfg = self.gcm->as.cfg;
            self.dump_range(file, 0x00010000u, cfg.io_ea - 0x00010000u);
            std::fclose(file);
        } else ++self.write_errors;
        std::snprintf(path, sizeof path, "%s/capture/state.txt", STARTUP_FS_ROOT);
        if (std::FILE *file = std::fopen(path, "w")) {
            const gcm_as_config &cfg = self.gcm->as.cfg;
            std::fprintf(file, "flip %u buffer %u\nio_ea 0x%08x io_size 0x%08x\nlocal_base 0x%08x local_size 0x%08x\nrecords %zu overflow %d\n",
                self.flips, buffer_id, cfg.io_ea, cfg.io_size, cfg.local_base, cfg.local_size, self.records.size() / 3, int(self.overflow));
            for (unsigned index = 0; index < GCM_INIT_DISPLAY_BUFFER_COUNT; ++index) {
                const gcm_init_display_buffer &display = self.gcm->displays[index];
                if (display.set) std::fprintf(file, "display %u offset 0x%08x pitch %u width %u height %u\n", index, display.offset, display.pitch, display.width, display.height);
            }
            std::fprintf(file, "complete 1\n");   // written last: a capture without this line was cut short
            std::fprintf(file, "memory records: u32 guest address, u32 length, bytes (host-endian header, raw guest bytes)\n");
            std::fprintf(file, "methods records: u32 subchannel, u32 method, u32 data (host-endian); a flip is 0xFFFFFFFF, flip index, buffer id\n");
            std::fclose(file);
        } else ++self.write_errors;
        std::vector<uint32_t>().swap(self.records);   // give the memory back
    }
};
StartupRsxCapture startup_rsx_capture;
ps3_guest_memory startup_rsx_capture_backing{};
#endif
bool gcm_init_window_overlaps(uint32_t base, uint64_t size)
{
    return uint64_t(GCM_INIT_WINDOW) < uint64_t(base) + size &&
           uint64_t(base) < uint64_t(GCM_INIT_WINDOW) + GCM_INIT_WINDOW_SIZE;
}
bool gcm_local_window_overlaps(uint32_t base, uint64_t size)
{
    return uint64_t(GCM_LOCAL_BASE) < uint64_t(base) + size &&
           uint64_t(base) < uint64_t(GCM_LOCAL_BASE) + GCM_LOCAL_SIZE;
}
#endif
constexpr uint64_t TIMEOUT_NS = UINT64_C(5000000000);
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
struct TimebaseMemoryAudit {
    ps3_guest_memory backing;
    std::atomic<uint64_t> calls{0};
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    std::atomic<uint64_t> writes{0};
    std::atomic<bool> *cancel_after_write = nullptr;
    uint32_t cancel_write_address = 0;
    unsigned cancellation_triggers = 0;
    bool fail_write_enabled = false;
    uint32_t fail_write_address = 0;
    unsigned failed_write_triggers = 0;
    std::atomic<bool> *cancel_after_read = nullptr;
    uint64_t *deadline_after_read = nullptr;
    uint32_t stop_read_address = 0;
    unsigned read_stop_triggers = 0;
#endif
};
ps3_guest_memory_status frequency_commit(void *context, uint32_t address, uint64_t length)
{
    auto &audit = *static_cast<TimebaseMemoryAudit *>(context);
    ++audit.calls;
    return audit.backing.commit(audit.backing.context, address, length);
}
ps3_guest_memory_status frequency_decommit(void *context, uint32_t address, uint64_t length)
{
    auto &audit = *static_cast<TimebaseMemoryAudit *>(context);
    ++audit.calls;
    return audit.backing.decommit(audit.backing.context, address, length);
}
ps3_guest_memory_status frequency_read(void *context, uint32_t address, void *output, size_t length)
{
    auto &audit = *static_cast<TimebaseMemoryAudit *>(context);
    ++audit.calls;
    const auto status = audit.backing.read(audit.backing.context, address, output, length);
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    if (status == PS3_GUEST_MEMORY_OK && address == audit.stop_read_address && length == 8 &&
        (audit.cancel_after_read || audit.deadline_after_read)) {
        ++audit.read_stop_triggers;
        if (audit.cancel_after_read) audit.cancel_after_read->store(true, std::memory_order_release);
        if (audit.deadline_after_read) *audit.deadline_after_read = 1;
    }
#endif
    return status;
}
ps3_guest_memory_status frequency_write(void *context, uint32_t address, const void *input, size_t length)
{
    auto &audit = *static_cast<TimebaseMemoryAudit *>(context);
    ++audit.calls;
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    ++audit.writes;
    if (audit.fail_write_enabled && address == audit.fail_write_address && length == 8) {
        ++audit.failed_write_triggers;
        return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    }
#endif
    const auto status = audit.backing.write(audit.backing.context, address, input, length);
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    if (status == PS3_GUEST_MEMORY_OK && audit.cancel_after_write &&
        address == audit.cancel_write_address && length == 8) {
        ++audit.cancellation_triggers;
        audit.cancel_after_write->store(true, std::memory_order_release);
    }
#endif
    return status;
}
ps3_guest_memory_status frequency_zero(void *context, uint32_t address, uint64_t length)
{
    auto &audit = *static_cast<TimebaseMemoryAudit *>(context);
    ++audit.calls;
    return audit.backing.zero(audit.backing.context, address, length);
}
#endif
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
            else if (width == 2) { uint16_t raw; std::memcpy(&raw, bytes, 2); value = __builtin_bswap16(raw); }
            else value = *bytes;
        }
        if (trace) {
            StartupAccess expected{};
            expected.origin = PpuMemoryTraceOrigin::Generated;
            expected.operation = PpuMemoryTraceOperation::Read;
            expected.address = uint32_t(address);
            expected.length = width;
            expected.value = value;
            accesses.push_back(expected);
        }
        return value;
    }
    void write(uint64_t address, uint64_t value, unsigned width, bool trace = true, PpuMemoryTraceOrigin origin = PpuMemoryTraceOrigin::Generated) {
        // Generated scalar writes narrow before the ABI.
        // vm_write8/16/32 take a narrow value argument before the raw bridge.
        value &= UINT64_MAX >> ((8 - width) * 8);
        uint8_t *bytes = find(address, width);
        if (bytes) {
            if (width == 4) { uint32_t raw = __builtin_bswap32(uint32_t(value)); std::memcpy(bytes, &raw, 4); }
            else if (width == 8) { uint64_t raw = __builtin_bswap64(value); std::memcpy(bytes, &raw, 8); }
            else if (width == 2) { uint16_t raw = __builtin_bswap16(uint16_t(value)); std::memcpy(bytes, &raw, 2); }
            else *bytes = uint8_t(value);
        }
        if (trace) {
            StartupAccess expected{};
            expected.origin = origin;
            expected.operation = PpuMemoryTraceOperation::Write;
            expected.address = uint32_t(address);
            expected.length = width;
            expected.value = value;
            accesses.push_back(expected);
        }
    }
};

int test_oracle_memory_encoding()
{
    Oracle fixture;
    fixture.regions.push_back({0x1000, std::vector<uint8_t>(16, 0), true});
    fixture.write(0x1000, UINT64_C(0x0123456789abcdef), 8, false);
    fixture.write(0x1008, 0x00a6, 2, false);
    fixture.write(0x100a, 0x5a, 1, false);
    const unsigned char expected[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                      0x00, 0xa6, 0x5a, 0, 0, 0, 0, 0};
    const bool passed = fixture.valid && !std::memcmp(fixture.regions[0].bytes.data(), expected, sizeof(expected));
    std::printf("INDIRECT_ORACLE_ENCODING_%s independent_expected_bytes=true widths=8,2,1\n", passed ? "PASS" : "FAIL");
    return !passed;
}

struct ClockProvider {
    ps3_clock real;
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
    unsigned calls = 0;
#endif
    Mutex mutex{};
    unsigned active_scopes = 0;
    std::atomic<bool> *cancel_after_sample = nullptr;
};
struct ClockScope {
    ClockProvider *provider;
    explicit ClockScope(ClockProvider *value) : provider(value) {
        mutexLock(&provider->mutex);
        ++provider->active_scopes;
    }
    ~ClockScope() {
        --provider->active_scopes;
        mutexUnlock(&provider->mutex);
    }
};
ps3_clock_status sample_real_clock(void *context, uint64_t *output)
{
    auto *provider = static_cast<ClockProvider *>(context);
    ClockScope scope(provider);
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
    ++provider->calls;
#endif
    ps3_clock_status status = provider->real.now_microseconds(provider->real.context, output);
    if (status == PS3_CLOCK_OK && provider->cancel_after_sample)
        provider->cancel_after_sample->store(true, std::memory_order_release);
    return status;
}
struct ClockArithmeticFixture { uint64_t origin, ticks, frequency; };
ps3_clock_status sample_arithmetic_fixture(void *context, uint64_t *output)
{
    const auto *fixture = static_cast<const ClockArithmeticFixture *>(context);
    return ps3_clock_elapsed_microseconds(fixture->origin, fixture->ticks, fixture->frequency, output);
}
struct TlsProvider {
    const ps3_guest_memory *memory;
    Ps3TlsArena arena;
    ps3_lwmutex_registry *registry = nullptr;
    ps3_user_memory_service *user_memory = nullptr;
    std::atomic<bool> *cancel_after_allocate = nullptr;
    unsigned allocation_calls = 0, free_calls = 0, query_calls = 0;
    uint64_t last_timeout = 0;
    unsigned mutex_calls = 0;
    std::atomic<bool> *cancel_after_create = nullptr;
    Mutex mutex{};
    unsigned calls = 0;
    unsigned active_scopes = 0;
    std::atomic<unsigned> *concurrent_ready = nullptr;
    std::atomic<bool> *committed_ready = nullptr;
    std::atomic<bool> *allow_return = nullptr;
#if defined(PS3_NATIVE_STARTUP_RECORDER_TEST) || defined(PS3_NATIVE_STARTUP_BUDGET_TEST)
    unsigned recorder_tls_core_calls = 0;
    std::atomic<bool> *recorder_cancel_after_tls = nullptr;
    Ps3TlsReport recorder_tls_report{};
    Ps3TlsImage recorder_tls_image{};
    Ps3TlsArena recorder_tls_before{}, recorder_tls_after{};
#endif
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
#if defined(PS3_NATIVE_STARTUP_RECORDER_TEST) || defined(PS3_NATIVE_STARTUP_BUDGET_TEST)
    provider->recorder_tls_before = provider->arena;
#endif
    Ps3TlsReport report = ps3_tls_initialize(provider->memory, &provider->arena, tls, scratch, sizeof(scratch), output);
#if defined(PS3_NATIVE_STARTUP_RECORDER_TEST) || defined(PS3_NATIVE_STARTUP_BUDGET_TEST)
    ++provider->recorder_tls_core_calls;
    provider->recorder_tls_report = report;
    provider->recorder_tls_after = provider->arena;
    provider->recorder_tls_image = *output;
    if (report.status == PS3_TLS_OK && provider->recorder_cancel_after_tls)
        provider->recorder_cancel_after_tls->store(true, std::memory_order_release);
#endif
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
    #ifndef PS3_NATIVE_GUEST_PRIORITY
#define PS3_NATIVE_GUEST_PRIORITY 0x2c
#endif
    // Root (2026-11-15, Decision 116): the guest main thread runs one step ABOVE the other guest threads (58 when they are
    // at 59). At 59 every wake from its usleep(5) polls and SPU task waits queued behind the 10 ms rotation of the other
    // priority-59 threads of its core; above them it gets its core back at once (the inFAMOUS 2 port measured the same).
    // PS3_NATIVE_MAIN_PRIORITY overrides; LLE_MAIN_PRIORITY in sibling-env.txt changes it at the first flip (heartbeat).
#ifndef PS3_NATIVE_MAIN_PRIORITY
#if PS3_NATIVE_GUEST_PRIORITY == 0x3B
#define PS3_NATIVE_MAIN_PRIORITY 0x3A
#else
#define PS3_NATIVE_MAIN_PRIORITY PS3_NATIVE_GUEST_PRIORITY
#endif
#endif
    // Decision 89: 2 MiB of host stack (was 256 KiB): nested indirect calls are no longer cut at depth 8.
    Result result = threadCreate(&worker, native_startup_worker, &execution, nullptr, 0x200000, PS3_NATIVE_MAIN_PRIORITY, -2);   // Decision 78/116
    if (R_FAILED(result)) result = threadCreate(&worker, native_startup_worker, &execution, nullptr, 0x200000, PS3_NATIVE_GUEST_PRIORITY, -2);
    if (R_FAILED(result)) result = threadCreate(&worker, native_startup_worker, &execution, nullptr, 0x200000, 0x2c, -2);
    if (R_SUCCEEDED(result)) {
        ::g_startup_main_thread_handle = worker.handle;
        s32 actual = -1;
        svcGetThreadPriority(&actual, worker.handle);
        std::printf("STARTUP_MAIN_THREAD_PRIORITY requested=%d actual=%d guest=%d\n", int(PS3_NATIVE_MAIN_PRIORITY), int(actual), int(PS3_NATIVE_GUEST_PRIORITY));
    }
    if (R_SUCCEEDED(result)) {   // Decision 89: the main guest thread may use cores 0..2 (see service_thread_horizon.c)
        uint64_t allowed = 7;
        if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) allowed = 7;
        const Result mask_result = svcSetThreadCoreMask(worker.handle, 0, u32(allowed & 7u));
        std::printf("STARTUP_MAIN_THREAD_CORES preferred=0 mask=0x%x rc=0x%x\n", unsigned(allowed & 7u), unsigned(mask_result));
    }
    if (R_FAILED(result)) return false;
    result = threadStart(&worker);
    if (R_FAILED(result)) {
        if (R_FAILED(threadClose(&worker))) std::abort();
        return false;
    }
#ifdef PS3_NATIVE_PROFILER
    ps3_profile_start(uint32_t(worker.handle));   // 2026-11-13: sampling profiler of the guest main thread, profile-<s>.txt once a minute (ps3_profile_switch.c)
#endif
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
    return native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK;
}

void configure(StartupControl &control, const Oracle &)
{
    control.deadline_tick = armGetSystemTick() + armNsToTicks(TIMEOUT_NS);
}
ps3_compiled_dispatch_stop_reason dispatch_stop_requested(void *context)
{
    auto &control = *static_cast<StartupControl *>(context);
    if (control.dispatch_stop_latched != PS3_COMPILED_DISPATCH_CONTINUE)
        return control.dispatch_stop_latched;
    if (control.cancelled.load(std::memory_order_acquire))
        control.dispatch_stop_latched = PS3_COMPILED_DISPATCH_CANCELLED;
    else if (control.deadline_tick && armGetSystemTick() >= control.deadline_tick)
        control.dispatch_stop_latched = PS3_COMPILED_DISPATCH_DEADLINE;
    return control.dispatch_stop_latched;
}
void prepare(StartupExecution &execution, const ppu_context &initial,
             const Ps3PpuExecutionServices &services, StartupControl &control,
             PpuMemoryBinding &binding, int32_t priority, bool has_priority = true)
{
    execution.ppu.cpu = initial;
    execution.dispatch_stop = {&control, dispatch_stop_requested};
    execution.control = &control;
    execution.guest_priority = priority;
    execution.has_guest_priority = has_priority;
    if (!native_startup_admit(execution, binding, services)) std::abort();
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
// Root (Decision 88): barging locks, not libnx mutexes (which hand a contended lock to a thread that may be
// unable to run for a whole time slice). Every guard built from these two covers a short critical section.
void lock_domain(void *context) { ps3_barging_lock(context); }
void unlock_domain(void *context) { ps3_barging_unlock(context); }
#if defined(PS3_NATIVE_LV2_SYNC) || defined(PS3_NATIVE_LV2_SPU)
/* Root fix (B2b review): one process-wide owner for the B2a/B2b lv2 services, wired on EVERY execution
 * that runs guest code. The worker wired them only on the post-discovery path, which the NRO never
 * reaches (run_startup_discovery is [[noreturn]]), and guarded the SPU registries with the host-only
 * ps3_pthread_guard, which does not exist in the Switch build. Guards are Horizon Mutexes, like the
 * other domains in this composition. */
static bool wire_lle_lv2_services(StartupExecution &execution)
{
#ifdef PS3_NATIVE_LV2_SYNC
    static ps3_notify_cancel process_cancel{0};
    static const StartupLv2SleepPort process_sleep{nullptr,
        [](void *, uint64_t microseconds, bool cancelled) -> bool {
            if (cancelled) return false;
#if defined(__SWITCH__) && !defined(STARTUP_COARSE_USLEEP)
            /* Decision 134 (console run rv69s): the job manager's wait loop (guest 0x41CF98) polls with sys_timer_usleep(1..5). 68,582 such
             * sleeps cost the main thread 150 s of 600 (25.5 % of its samples in svcSleepThread): 2.2 ms each, where the PS3 returns in
             * microseconds and a timed svcWaitForAddress on this console returns at its timeout (the taskwait pause: 884,758 waits of 250 us
             * = 37 % of 600 s, exactly). A short sleep is now a timed wait on a word nobody signals. -DSTARTUP_COARSE_USLEEP: the old sleep. */
            if (microseconds && microseconds <= 2000) {
                static uint32_t never_signalled;
                // Root review: at least 50 us, so a 1 us poll loop of the high-priority main thread cannot starve the SPU job thread
                // that shares its core (slot 2 prefers core 0) -- the very work it is polling for. Still ~40x shorter than the 2.2 ms.
                const uint64_t floored = microseconds < 50 ? 50 : microseconds;
                (void)svcWaitForAddress(&never_signalled, ArbitrationType_WaitIfEqual, 0, int64_t(floored) * 1000);
                return true;
            }
#endif
            const uint64_t capped = microseconds > 50000 ? 50000 : microseconds;
            timespec wait{time_t(capped / 1000000u), long(capped % 1000000u) * 1000};
            nanosleep(&wait, nullptr);
            return true;
        }};
    execution.lv2_sync_cancel = &process_cancel;
    execution.lv2_sleep = &process_sleep;
#endif
#ifdef PS3_NATIVE_LV2_SPU
    static ps3_lv2_spu_images process_spu_images{};
    static ps3_lv2_spu_groups process_spu_groups{};
    static Mutex spu_image_mutex{}, spu_group_mutex{};
    static bool initialized = false;
    if (!initialized) {
        static std::unique_ptr<unsigned char[]> process_local_stores(
            new unsigned char[size_t(PS3_LV2_SPU_GROUP_CAP) * PS3_LV2_SPU_THREADS_MAX * PS3_LV2_SPU_LS_BYTES]());
        mutexInit(&spu_image_mutex);
        mutexInit(&spu_group_mutex);
        if (ps3_lv2_spu_images_init(&process_spu_images,
                ps3_instance_guard{&spu_image_mutex, lock_domain, unlock_domain}) != PS3_LV2_SPU_OK) {
            std::puts("STARTUP_LV2_SPU_IMAGES_INIT_STOP");
            return false;
        }
        if (ps3_lv2_spu_groups_init(&process_spu_groups,
                ps3_instance_guard{&spu_group_mutex, lock_domain, unlock_domain},
                process_local_stores.get(), uint32_t(PS3_LV2_SPU_THREADS_MAX)) != PS3_LV2_SPU_OK) {
            std::puts("STARTUP_LV2_SPU_GROUPS_INIT_STOP");
            return false;
        }
        initialized = true;
    }
    execution.spu_images = &process_spu_images;
    execution.spu_groups = &process_spu_groups;
#endif
    return true;
}
#endif
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
ps3_user_memory_service_result allocate_memory(void *context, uint64_t size, uint64_t alignment, uint64_t output)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ProviderScope scope(provider);
    ++provider->allocation_calls;
    ps3_user_memory_service_result result = ps3_user_memory_allocate_and_publish_be32(
        provider->user_memory, size, alignment, output);
    if (result.status == PS3_USER_MEMORY_SERVICE_OK && provider->cancel_after_allocate)
        provider->cancel_after_allocate->store(true, std::memory_order_release);
    return result;
}
ps3_user_memory_service_result free_memory(void *context, uint32_t address)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->free_calls;
    return ps3_user_memory_free_owned(provider->user_memory, address);
}
ps3_user_memory_service_result query_memory(void *context, uint64_t output)
{
    auto *provider = static_cast<TlsProvider *>(context);
    ++provider->query_calls;
    return ps3_user_memory_query_and_publish_be32(provider->user_memory, output);
}

int test_memory_family(const ps3_guest_memory *memory, ps3_atomic_domain *domain,
                       ps3_user_memory *allocator, TlsProvider &provider,
                       const Ps3PpuExecutionServices &services, const ppu_context &initial,
                       Oracle &oracle, PpuMemoryBinding &binding, int32_t priority)
{
    int failures = 0;
#define VERIFY(condition) do { if (!(condition)) { std::printf("MEMORY_ABI_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    constexpr uint32_t output = ARGV + 0x300;
    auto call = [&](uint64_t number, uint64_t r3, uint64_t r4, uint64_t r5,
                    uint32_t cell, StartupStop stop = StartupStop::Returned) {
        auto control_storage = std::make_unique<StartupControl>();
        StartupControl &control = *control_storage;
        configure(control, oracle);
        control.entry = StartupEntry::MemorySyscall;
        StartupExecution owner{};
        prepare(owner, initial, services, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[3] = r3;
        owner.ppu.cpu.gpr[4] = r4;
        owner.ppu.cpu.gpr[5] = r5;
        owner.ppu.cpu.gpr[11] = number;
        owner.ppu.cpu.cr = 0x12345678;
        const ppu_context before = owner.ppu.cpu;
        const uint64_t toc_sentinel = UINT64_C(0x1122334455667788);
        VERIFY(memory->write(memory->context, uint32_t(owner.ppu.cpu.gpr[1]) + 0x28,
            &toc_sentinel, sizeof(toc_sentinel)) == PS3_GUEST_MEMORY_OK);
        VERIFY(run(owner));
        if (control.memory_result.retained_pin) {
            std::printf("STARTUP_MEMORY_FATAL retained_output_pin=true joined=true backing_retained=true service=%d pin_release=%d\n",
                control.memory_result.status, control.memory_result.pin_release_status);
            std::fflush(stdout);
            svcExitProcess();
        }
        VERIFY(control.stop == stop && provider.active_scopes == 0);
        ppu_context expected = before;
        if (stop == StartupStop::Returned) expected.gpr[3] = uint64_t(int64_t(int32_t(cell)));
        VERIFY(std::memcmp(&owner.ppu.cpu, &expected, sizeof(expected)) == 0);
        VERIFY(control.memory_returns == (stop == StartupStop::Returned ? 1 : 0));
        uint64_t actual_toc = 0;
        VERIFY(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28,
            &actual_toc, sizeof(actual_toc)) == PS3_GUEST_MEMORY_OK);
        VERIFY(actual_toc == toc_sentinel && control.journal.count == 0);
        return control.memory_result;
    };
    auto read_output = [&]() {
        uint32_t value = 0;
        VERIFY(memory->read(memory->context, output, &value, 4) == PS3_GUEST_MEMORY_OK);
        return __builtin_bswap32(value);
    };
    auto accounting = [&](uint64_t used) {
        ps3_user_memory_info info{};
        VERIFY(ps3_user_memory_query(allocator, &info) == PS3_USER_MEMORY_OK);
        VERIFY(info.quota_bytes == UINT64_C(213) * 1024 * 1024 && info.used_bytes == used);
        VERIFY(info.available_bytes == info.quota_bytes - used);
    };
    call(UINT64_C(0x100000160), output, 0, 0, 0, StartupStop::UnsupportedSyscall);
    call(352, output, 0, 0, 0);
    VERIFY(read_output() == 213 * 1024 * 1024);
    call(348, 0x100000, 0, UINT64_C(0xabc0000000000000) | output, 0);
    VERIFY(read_output() == 0x40000000);
    accounting(0x100000);
    unsigned char zeroes[64];
    std::memset(zeroes, 0xff, sizeof(zeroes));
    VERIFY(memory->read(memory->context, 0x40000000, zeroes, sizeof(zeroes)) == PS3_GUEST_MEMORY_OK);
    for (unsigned char value : zeroes) VERIFY(value == 0);
    call(352, output, 0, 0, 0);
    uint32_t available = 0;
    VERIFY(memory->read(memory->context, output + 4, &available, 4) == PS3_GUEST_MEMORY_OK);
    VERIFY(__builtin_bswap32(available) == 212 * 1024 * 1024);
    call(349, UINT64_C(0xabc0000040000000), 0, 0, 0);
    call(348, 0x10000, 0x200, output, 0);
    VERIFY(read_output() == 0x40000000);
    call(349, 0x40000000, 0, 0, 0);
    call(348, 0x100000, 0x400, output, 0);
    ps3_atomic_pin pin{};
    VERIFY(ps3_atomic_pin_acquire(domain, 0x40000000, 64, &pin) == PS3_GUEST_MEMORY_OK);
    auto retained = call(349, 0x40000000, 0, 0, 0, StartupStop::MemoryProviderFailure);
    VERIFY(retained.retained_allocation && retained.retained_address == 0x40000000);
    accounting(0x100000);
    VERIFY(ps3_atomic_pin_release(domain, &pin) == PS3_GUEST_MEMORY_OK);
    call(349, 0x40000000, 0, 0, 0);
    call(349, 0x40000000, 0, 0, 0x80010002);
    call(348, 0, 0x400, output, 0x80010010);
    call(348, UINT64_C(0x100000001), 0x400, output, 0x80010010);
    call(348, 0x100000, UINT64_C(0x100000400), output, 0x80010002);
    call(348, UINT64_C(0x100100000), 0x400, output, 0x80010004);
    const unsigned queries_before = provider.query_calls;
    call(352, 0x10000, 0, 0, 0x8001000d);
    call(352, 0xc0000000, 0, 0, 0x8001000d);
    call(352, 0xfffffffc, 0, 0, 0x8001000d);
    call(352, ARGV + 4092, 0, 0, 0x8001000d);
    VERIFY(provider.query_calls == queries_before);
    accounting(0);
    auto cancelled_storage = std::make_unique<StartupControl>();
    StartupControl &cancelled = *cancelled_storage;
    configure(cancelled, oracle);
    cancelled.entry = StartupEntry::MemorySyscall;
    StartupExecution owner{};
        prepare(owner, initial, services, cancelled, binding, priority);
    owner.ppu.cpu.gpr[3] = 0x100000;
    owner.ppu.cpu.gpr[4] = 0x400;
    owner.ppu.cpu.gpr[5] = output;
    owner.ppu.cpu.gpr[11] = 348;
    const ppu_context before = owner.ppu.cpu;
    provider.cancel_after_allocate = &cancelled.cancelled;
    VERIFY(run(owner));
    if (cancelled.memory_result.retained_pin || cancelled.memory_result.retained_allocation) {
        std::printf("STARTUP_MEMORY_FATAL cancellation_retained_resources=true joined=true backing_retained=true\n");
        std::fflush(stdout);
        svcExitProcess();
    }
    VERIFY(cancelled.stop == StartupStop::Cancelled && cancelled.memory_returns == 0);
    VERIFY(cancelled.memory_result.status == PS3_USER_MEMORY_SERVICE_OK && cancelled.memory_result.published);
    VERIFY(provider.active_scopes == 0 && std::memcmp(&owner.ppu.cpu, &before, sizeof(before)) == 0);
    VERIFY(read_output() == 0x40000000);
    accounting(0x100000);
    provider.cancel_after_allocate = nullptr;
    VERIFY(ps3_user_memory_free(allocator, 0x40000000).status == PS3_USER_MEMORY_OK);
    accounting(0);
    // Restore the complete scratch pages before the independent startup oracle.
    VERIFY(memory->write(memory->context, ARGV, oracle.find(ARGV, 4096), 4096) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory->write(memory->context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES), ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("MEMORY_ABI_%s full64_size_flags=true effective32_pointers=true syscall_no_toc_write=true cancel_committed_owned=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}

#ifdef PS3_NATIVE_THREAD_ID_QUERY
int test_thread_id_query(const ps3_guest_memory *backing,
    const Ps3PpuExecutionServices &services, const ppu_context &initial,
    Oracle &oracle, PpuMemoryBinding &binding, int32_t priority,
    TimebaseMemoryAudit &audit)
{
    int failures = 0;
    unsigned cases = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("THREAD_ID_STUB_FAIL case=%u line=%d\n", cases, __LINE__); ++failures; } } while (0)
    const ps3_guest_memory &memory = binding.coherent_memory;
    VERIFY(memory.commit(memory.context, ARGV + 4096, 4096) == PS3_GUEST_MEMORY_OK);
    std::vector<unsigned char> baseline(8192), stack_baseline(ABI_SCRATCH_BYTES);
    VERIFY(memory.read(memory.context, ARGV, baseline.data(), baseline.size()) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.read(memory.context, STACK_TOP, stack_baseline.data(), stack_baseline.size()) == PS3_GUEST_MEMORY_OK);
    const unsigned char id_one[8] = {0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
    const unsigned char id_two[8] = {0, 0, 0, 0, 0x55, 0x66, 0x77, 0x88};
    const unsigned char toc_bytes[8] = {0, 0, 0, 0, 0, 0x78, 0xbf, 0x78};
    auto call = [&](uint64_t id, uint32_t output, const unsigned char *literal_id,
                    StartupStop wanted, unsigned negative = 0, unsigned reservation = 0) {
        ++cases;
        VERIFY(memory.commit(memory.context, ARGV + 4096, 4096) == PS3_GUEST_MEMORY_OK);
        VERIFY(memory.write(memory.context, ARGV, baseline.data(), baseline.size()) == PS3_GUEST_MEMORY_OK);
        VERIFY(memory.write(memory.context, STACK_TOP, stack_baseline.data(), stack_baseline.size()) == PS3_GUEST_MEMORY_OK);
        std::vector<PpuMemoryRamRange> ranges(binding.ram, binding.ram + binding.ram_count);
        for (auto &range : ranges) {
            if (range.address == ARGV) {
                range.length = (negative == 3 || negative == 12) ? 4096 : 8192;
                if (negative == 2) range.writable = false;
            }
        }
        if (negative == 3 || negative == 12) ranges.push_back({ARGV + 4096, 4096, false});
        PpuMemoryBinding fixture_binding(*binding.domain, ranges.data(), ranges.size());
        Ps3PpuExecutionServices selected{};
        selected.memory = &fixture_binding.coherent_memory;
        selected.compiled_code = services.compiled_code;
        auto control = std::make_unique<StartupControl>();
        auto owner = std::make_unique<StartupExecution>();
        configure(*control, oracle);
        control->entry = StartupEntry::IndirectCall;
        prepare(*owner, initial, selected, *control, fixture_binding, priority, false);
        std::memset(&owner->ppu.cpu, 0xa5, sizeof(owner->ppu.cpu));
        owner->ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner->ppu.cpu.gpr[2] = 0x78bf78;
        owner->ppu.cpu.gpr[3] = UINT64_C(0xabcddcba00000000) | output;
        owner->ppu.cpu.ctr = 0x55da04;
        owner->ppu.cpu.thread_id = id;
        if (negative == 1 || negative == 13) VERIFY(memory.decommit(memory.context, ARGV + 4096, 4096) == PS3_GUEST_MEMORY_OK);
        if (negative == 4) control->journal.capacity = 1;
        if (negative == 5) control->journal.capacity = 0;
        if (negative == 6) control->cancelled.store(true);
        if (negative == 7) control->deadline_tick = 1;
        if (negative == 8) selected.memory = backing;
        if (negative == 9) owner->ppu.cpu.gpr[1] = UINT64_MAX - 0x10;
        if (negative == 11) owner->ppu.cpu.gpr[1] = UINT64_C(0x40000000) - 0x28;
        if (negative == 12) owner->ppu.cpu.gpr[1] = ARGV + 4096 - 0x28;
        if (negative == 13) owner->ppu.cpu.gpr[1] = ARGV + 4092 - 0x28;
        if (negative == 14) {
            control->journal.capacity = 2;
            control->journal.count = 1;
            control->accesses[0] = {};
            control->accesses[0].origin = PpuMemoryTraceOrigin::Hle;
            control->accesses[0].operation = PpuMemoryTraceOperation::Write;
            control->accesses[0].address = ARGV + 0x700;
            control->accesses[0].length = 8;
            control->accesses[0].value = 0;
            control->accesses[0].stored = false;
            control->accesses[0].byte_transfer = false;
            control->accesses[0].status = PS3_GUEST_MEMORY_OK;
        }
        if (negative == 10) {
            audit.cancel_after_write = &control->cancelled;
            audit.cancel_write_address = output;
            audit.cancellation_triggers = 0;
        }
        if (negative >= 16 && negative <= 19) {
            audit.stop_read_address = (negative == 16 || negative == 18) ? output : STACK_TOP + 0x128;
            audit.read_stop_triggers = 0;
            if (negative == 16 || negative == 17) audit.cancel_after_read = &control->cancelled;
            else audit.deadline_after_read = &control->deadline_tick;
        }
        if (negative == 15) {
            audit.fail_write_enabled = true;
            audit.fail_write_address = STACK_TOP + 0x128;
            audit.failed_write_triggers = 0;
        }
        auto observer_control = std::make_unique<StartupControl>();
        auto observer = std::make_unique<StartupExecution>();
        uint32_t reserved_address = output;
        if (reservation) {
            configure(*observer_control, oracle);
            prepare(*observer, initial, selected, *observer_control, fixture_binding, priority, false);
            reserved_address = reservation == 1 ? output : reservation == 3 ? output + 8 : ARGV + 0x1800;
            (void)ppu_native_load_reserve64(&observer->ppu.cpu, reserved_address);
        }
        const ppu_context before = owner->ppu.cpu;
        const uint64_t writes_before = audit.writes.load();
        const size_t events_before = control->journal.count;
        const auto first_event_before = control->accesses[0];
        VERIFY(run(*owner));
        audit.cancel_after_write = nullptr;
        audit.cancel_after_read = nullptr;
        audit.deadline_after_read = nullptr;
        audit.fail_write_enabled = false;
        const uint64_t target_writes = audit.writes.load() - writes_before;
        ppu_context expected = before;
        if (wanted == StartupStop::Returned) expected.gpr[3] = 0;
        VERIFY(control->stop == wanted);
        VERIFY(!std::memcmp(&owner->ppu.cpu, &expected, sizeof(expected)));
        const bool published = wanted == StartupStop::Returned || negative == 10 || negative == 15;
        VERIFY(target_writes == (wanted == StartupStop::Returned || negative == 15 ? 2 : published ? 1 : 0));
        VERIFY(control->journal.count == events_before + (wanted == StartupStop::Returned ? 2 : published ? 1 : 0));
        if (events_before) VERIFY(!std::memcmp(&control->accesses[0], &first_event_before, sizeof(first_event_before)));
        if (published) {
            const auto &event = control->accesses[0];
            VERIFY(event.origin == PpuMemoryTraceOrigin::Hle && event.operation == PpuMemoryTraceOperation::Write);
            VERIFY(event.address == output && event.length == 8 && event.value == id &&
                !event.stored && !event.byte_transfer && event.status == PS3_GUEST_MEMORY_OK);
        }
        if (wanted == StartupStop::Returned) {
            const auto &event = control->accesses[1];
            VERIFY(event.origin == PpuMemoryTraceOrigin::Hle && event.operation == PpuMemoryTraceOperation::Write);
            VERIFY(event.address == STACK_TOP + 0x128 && event.length == 8 && event.value == 0x78bf78 &&
                !event.stored && !event.byte_transfer && event.status == PS3_GUEST_MEMORY_OK);
            VERIFY(control->dispatch_count == 1 && control->dispatches[0].captured_ctr == 0x55da04);
            VERIFY(control->dispatches[0].core_returned && owner->dispatch.depth == 0);
        }
        if (negative == 10) VERIFY(audit.cancellation_triggers == 1);
        if (negative == 15) VERIFY(audit.failed_write_triggers == 1);
        if (negative >= 16 && negative <= 19) VERIFY(audit.read_stop_triggers == 1);
        std::vector<unsigned char> expected_memory = baseline;
        std::vector<unsigned char> expected_stack = stack_baseline;
        if (published && output >= ARGV && uint64_t(output) + 8 <= uint64_t(ARGV) + 8192)
            std::memcpy(expected_memory.data() + output - ARGV, literal_id, 8);
        if (published && output >= STACK_TOP && uint64_t(output) + 8 <= uint64_t(STACK_TOP) + ABI_SCRATCH_BYTES)
            std::memcpy(expected_stack.data() + output - STACK_TOP, literal_id, 8);
        if (wanted == StartupStop::Returned) std::memcpy(expected_stack.data() + 0x128, toc_bytes, 8);
        std::vector<unsigned char> actual_memory((negative == 1 || negative == 13) ? 4096 : 8192), actual_stack(ABI_SCRATCH_BYTES);
        VERIFY(memory.read(memory.context, ARGV, actual_memory.data(), actual_memory.size()) == PS3_GUEST_MEMORY_OK);
        VERIFY(!std::memcmp(actual_memory.data(), expected_memory.data(), actual_memory.size()));
        VERIFY(memory.read(memory.context, STACK_TOP, actual_stack.data(), actual_stack.size()) == PS3_GUEST_MEMORY_OK);
        VERIFY(actual_stack == expected_stack);
        if (reservation) {
            const bool stored = ppu_native_store_conditional64(&observer->ppu.cpu,
                reserved_address, UINT64_C(0x1020304050607080));
            VERIFY(stored == (reservation == 2));
            VERIFY(native_startup_release_after_join(*observer) == PS3_GUEST_MEMORY_OK);
        }
        std::printf("THREAD_ID_STUB_CASE index=%u id=0x%llx output=0x%x stop=%d wanted=%d writes=%llu reservation=%u original_stub=true\n",
            cases, (unsigned long long)id, output, int(control->stop), int(wanted),
            (unsigned long long)target_writes, reservation);
    };
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Returned, 0, 1);
    call(0x55667788, ARGV + 0x300, id_two, StartupStop::Returned, 0, 2);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Returned, 0, 3);
    call(0x11223344, ARGV + 4092, id_one, StartupStop::Returned);
    call(0x11223344, STACK_TOP + 0x128, id_one, StartupStop::Returned);
    call(0x11223344, STACK_TOP + 0x12c, id_one, StartupStop::Returned);
    for (uint64_t id : {UINT64_C(0), uint64_t(PS3_LWMUTEX_RESERVED), UINT64_C(0x100000001), UINT64_MAX})
        call(id, ARGV + 0x300, id_one, StartupStop::MissingServices);
    call(0x11223344, 0x40000000, id_one, StartupStop::InvalidMemory);
    call(0x11223344, 0xfffffffc, id_one, StartupStop::InvalidMemory);
    call(0x11223344, ARGV + 4092, id_one, StartupStop::InvalidMemory, 1);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::InvalidMemory, 2);
    call(0x11223344, ARGV + 4092, id_one, StartupStop::InvalidMemory, 3);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::TraceCapacity, 4);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::TraceCapacity, 5);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Cancelled, 6);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Deadline, 7);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::MissingServices, 8);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::InvalidMemory, 9);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Cancelled, 10);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::InvalidMemory, 11);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::InvalidMemory, 12);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::InvalidMemory, 13);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::TraceCapacity, 14);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::ProviderFailure, 15);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Cancelled, 16);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Cancelled, 17);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Deadline, 18);
    call(0x11223344, ARGV + 0x300, id_one, StartupStop::Deadline, 19);
    VERIFY(memory.commit(memory.context, ARGV + 4096, 4096) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.write(memory.context, ARGV, baseline.data(), baseline.size()) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.write(memory.context, STACK_TOP, stack_baseline.data(), stack_baseline.size()) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.decommit(memory.context, ARGV + 4096, 4096) == PS3_GUEST_MEMORY_OK);
    std::printf("THREAD_ID_STUB_%s cases=%u generated_stub=true address=0x55da04 cpu_bytes=%zu failures=%d\n",
        failures ? "FAIL" : "PASS", cases, sizeof(ppu_context), failures);
    return failures;
#undef VERIFY
}
#endif
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
int test_timebase_frequency(const ps3_guest_memory *backing,
    const Ps3PpuExecutionServices &services, const ppu_context &initial,
    Oracle &oracle, PpuMemoryBinding &binding, int32_t priority,
    TlsProvider &provider, ClockProvider &clock, TimebaseMemoryAudit &audit)
{
    int failures = 0;
    unsigned cases = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("TIMEBASE_BRIDGE_FAIL case=%u line=%d\n", cases, __LINE__); ++failures; } } while (0)
    VERIFY(PS3_GUEST_TIMEBASE_FREQUENCY_HZ == UINT64_C(79800000));
    auto call = [&](uint64_t number, uint64_t incoming, bool absent_capabilities,
                    StartupStop wanted, unsigned negative = 0) {
        ++cases;
        auto control = std::make_unique<StartupControl>();
        auto owner = std::make_unique<StartupExecution>();
        Ps3PpuExecutionServices selected = services;
        if (absent_capabilities) {
            selected.allocate_memory = nullptr;
            selected.free_memory = nullptr;
            selected.query_memory = nullptr;
            selected.clock = nullptr;
        }
        configure(*control, oracle);
        control->entry = negative == 5 ? StartupEntry::UnsupportedExternal : StartupEntry::MemorySyscall;
        prepare(*owner, initial, selected, *control, binding, priority);
        std::memset(&owner->ppu.cpu, 0xa5, sizeof(owner->ppu.cpu));
        owner->ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner->ppu.cpu.gpr[3] = incoming;
        owner->ppu.cpu.gpr[11] = number;
        for (unsigned reg = 4; reg <= 10; ++reg) owner->ppu.cpu.gpr[reg] = UINT64_C(0xfeedface12340000) + reg;
        owner->ppu.cpu.cr = 0x12345678;
        if (negative == 1) control->cancelled.store(true);
        if (negative == 2) control->deadline_tick = 1;
        if (negative == 3) selected.memory = backing;
        if (negative == 4) owner->ppu.services = nullptr;
        const ppu_context before = owner->ppu.cpu;
        const uint64_t memory_calls = audit.calls.load();
        const unsigned allocations = provider.allocation_calls;
        const unsigned frees = provider.free_calls, queries = provider.query_calls;
        const unsigned clocks = clock.calls;
        VERIFY(run(*owner));
        const uint64_t memory_calls_after_worker = audit.calls.load();
        ppu_context expected = before;
        if (wanted == StartupStop::Returned) expected.gpr[3] = UINT64_C(79800000);
        VERIFY(control->stop == wanted);
        VERIFY(!std::memcmp(&owner->ppu.cpu, &expected, sizeof(expected)));
        VERIFY(memory_calls_after_worker == memory_calls);
        VERIFY(provider.allocation_calls == allocations && provider.free_calls == frees && provider.query_calls == queries);
        VERIFY(clock.calls == clocks && control->clock_calls == 0 && control->memory_returns == 0);
        VERIFY(control->journal.count == 0 && control->import_count == 0);
        if (negative == 5) VERIFY(control->external_id == 18);
        VERIFY(compare(&binding.coherent_memory, oracle));
        std::printf("TIMEBASE_BRIDGE_CASE index=%u number=0x%llx input=0x%llx stop=%d wanted=%d full_cpu_bytes=%zu provider_delta=%llu absent_unrelated=%d\n",
            cases, (unsigned long long)number, (unsigned long long)incoming,
            int(control->stop), int(wanted), sizeof(ppu_context),
            (unsigned long long)(memory_calls_after_worker - memory_calls), absent_capabilities);
    };
    for (uint64_t incoming : {UINT64_C(0xffffabcd00000000), UINT64_C(0xffffabcd40000000),
            UINT64_C(0xffffabcd00010230), UINT64_C(0)})
        call(147, incoming, false, StartupStop::Returned);
    call(147, UINT64_C(0xdeadbeef40000000), true, StartupStop::Returned);
    call(147, UINT64_C(0xdeadbeef00010230), true, StartupStop::Cancelled, 1);
    call(147, UINT64_C(0xdeadbeef40000000), true, StartupStop::Deadline, 2);
    call(147, UINT64_C(0xdeadbeef40000000), false, StartupStop::MissingServices, 3);
    call(147, UINT64_C(0xdeadbeef40000000), false, StartupStop::MissingServices, 4);
    for (uint64_t number : {UINT64_C(146), UINT64_C(148), UINT64_C(0x100000093)})
        call(number, UINT64_C(0xdeadbeef40000000), false, StartupStop::UnsupportedSyscall);
    call(147, UINT64_C(0xdeadbeef40000000), false, StartupStop::UnsupportedExternal, 5);
    std::printf("TIMEBASE_BRIDGE_%s cases=%u frequency=%llu generated_stub=false native_bridge=true failures=%d\n",
        failures ? "FAIL" : "PASS", cases, (unsigned long long)PS3_GUEST_TIMEBASE_FREQUENCY_HZ, failures);
    return failures;
#undef VERIFY
}
#endif
int test_clock_abi(const ps3_guest_memory *memory, const Ps3PpuExecutionServices &services,
                   const ppu_context &initial, Oracle &oracle,
                   ClockProvider &provider, PpuMemoryBinding &binding, int32_t priority)
{
    int failures = 0;
#define VERIFY(condition) do { if (!(condition)) { std::printf("CLOCK_ABI_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    auto call = [&](const ps3_clock *clock, StartupStop wanted, bool cancel, uint64_t exact_value) {
        Ps3PpuExecutionServices selected = services;
        selected.clock = clock;
        auto control_storage = std::make_unique<StartupControl>();
        StartupControl &control = *control_storage;
        configure(control, oracle);
        control.entry = StartupEntry::ClockStub;
        StartupExecution owner{};
        prepare(owner, initial, selected, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[3] = UINT64_MAX;
        owner.ppu.cpu.gpr[4] = UINT64_C(0xdeadbeefcafef00d);
        const ppu_context before = owner.ppu.cpu;
        const uint64_t sentinel = UINT64_C(0x1122334455667788);
        VERIFY(memory->write(memory->context, uint32_t(before.gpr[1]) + 0x28,
            &sentinel, sizeof(sentinel)) == PS3_GUEST_MEMORY_OK);
        provider.cancel_after_sample = cancel ? &control.cancelled : nullptr;
        VERIFY(run(owner));
        provider.cancel_after_sample = nullptr;
        VERIFY(control.stop == wanted && provider.active_scopes == 0);
        ppu_context expected = before;
        uint64_t stored = 0;
        VERIFY(memory->read(memory->context, uint32_t(before.gpr[1]) + 0x28, &stored, 8) == PS3_GUEST_MEMORY_OK);
        if (wanted == StartupStop::Returned) {
            expected.gpr[3] = control.clock_sample;
            VERIFY(control.clock_calls == 1 && control.clock_returns == 1 && control.clock_status == PS3_CLOCK_OK);
            VERIFY(__builtin_bswap64(stored) == before.gpr[2]);
            if (exact_value) VERIFY(control.clock_sample == exact_value);
        } else {
            VERIFY(control.clock_returns == 0 && stored == sentinel);
            if (cancel) VERIFY(control.clock_calls == 1 && control.clock_status == PS3_CLOCK_OK);
        }
        VERIFY(std::memcmp(&owner.ppu.cpu, &expected, sizeof(expected)) == 0);
    };
    call(services.clock, StartupStop::Returned, false, 0);
    // Deterministic conversion/ABI fixture only; startup always uses real ticks.
    ClockArithmeticFixture wide{0, UINT64_C(0xfedcba9876543210), 1000000};
    ps3_clock wide_port{&wide, sample_arithmetic_fixture};
    call(&wide_port, StartupStop::Returned, false, wide.ticks);
    ps3_horizon_clock invalid{};
    ps3_clock invalid_port = ps3_horizon_clock_port(&invalid);
    call(&invalid_port, StartupStop::ClockProviderFailure, false, 0);
    call(nullptr, StartupStop::MissingServices, false, 0);
    call(services.clock, StartupStop::Cancelled, true, 0);
    VERIFY(memory->write(memory->context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES),
        ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("CLOCK_ABI_%s no_arguments=true full_u64=true wide_fixture_only=true actual_startup_clock=Horizon cancel_after_real_sample=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
int test_real_indirect(const Ps3PpuExecutionServices &services,
    PpuMemoryBinding &binding, const ppu_context &initial, Oracle &oracle,
    int32_t priority)
{
    int failures = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("INDIRECT_NATIVE_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    const uint32_t first = ARGV + 0x300, second = ARGV + 0x380;
    const uint32_t words[3] = {__builtin_bswap32(1u), __builtin_bswap32(2u), __builtin_bswap32(3u)};
    const ps3_guest_memory &memory = binding.coherent_memory;
    VERIFY(memory.write(memory.context, first, words, sizeof(words)) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.write(memory.context, second, words, sizeof(words)) == PS3_GUEST_MEMORY_OK);
    auto call = [&](uint64_t ctr, StartupStop expected_stop,
                    ps3_compiled_dispatch_status expected_status, bool cancel,
                    bool deadline, bool depth_full, bool terminal) {
        Ps3PpuExecutionServices selected = services;
        if (terminal) selected.clock = nullptr;
        auto control_storage = std::make_unique<StartupControl>();
        StartupControl &control = *control_storage;
        configure(control, oracle);
        control.entry = StartupEntry::IndirectCall;
        StartupExecution execution{};
        prepare(execution, initial, selected, control, binding, priority);
        execution.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        execution.ppu.cpu.gpr[3] = first;
        execution.ppu.cpu.gpr[4] = second;
        execution.ppu.cpu.cr = 0xabcdeff0;
        execution.ppu.cpu.ctr = ctr;
        const ppu_context before = execution.ppu.cpu;
        if (cancel) control.cancelled.store(true, std::memory_order_release);
        if (deadline) control.deadline_tick = armGetSystemTick() - 1;
        if (depth_full) execution.dispatch.depth = execution.dispatch.limit;
        const uint32_t depth_before = execution.dispatch.depth;
        Thread worker;
        if (!start(execution, worker)) std::abort();
        join(worker);
        VERIFY(control.stop == expected_stop && control.dispatch_count == 1);
        const auto &record = control.dispatches[0];
        VERIFY(record.captured_ctr == ctr && !std::memcmp(&record.before, &before, sizeof(before)));
        if (terminal) {
            VERIFY(!record.core_returned && execution.dispatch.depth == depth_before + 1);
            VERIFY(control.import_count == 1 && control.imports[0] == 0x8461e528);
        } else {
            VERIFY(record.core_returned && record.result.status == expected_status);
            VERIFY(execution.dispatch.depth == depth_before);
        }
        ppu_context wanted = before;
        if (expected_status == PS3_COMPILED_DISPATCH_OK && !terminal) {
            wanted.gpr[0] = 0;
            wanted.gpr[3] = 0;
            wanted.gpr[9] = 3;
            wanted.cr = (wanted.cr & ~0xfu) | 2;
            VERIFY(control.journal.count == 6);
        } else VERIFY(control.journal.count == 0);
        VERIFY(!std::memcmp(&execution.ppu.cpu, &wanted, sizeof(wanted)));
        VERIFY(native_startup_release_after_join(execution) == PS3_GUEST_MEMORY_OK);
        std::printf("INDIRECT_NATIVE_CASE ctr=0x%llx stop=%d core_returned=%d status=%d depth_before=%u depth_after=%u joined=true\n",
            (unsigned long long)ctr, int(control.stop), record.core_returned, record.core_returned ? int(record.result.status) : -1,
            depth_before, execution.dispatch.depth);
    };
    call(0x11e08, StartupStop::Returned, PS3_COMPILED_DISPATCH_OK, false, false, false, false);
    call(UINT64_C(0xdeadbeef00011e08), StartupStop::Returned, PS3_COMPILED_DISPATCH_OK, false, false, false, false);
    call(0, StartupStop::DispatchFailure, PS3_COMPILED_DISPATCH_INVALID_ADDRESS, false, false, false, false);
    call(0x11e09, StartupStop::DispatchFailure, PS3_COMPILED_DISPATCH_INVALID_ADDRESS, false, false, false, false);
    call(0xfffff000, StartupStop::DispatchFailure, PS3_COMPILED_DISPATCH_LOOKUP_FAILURE, false, false, false, false);
    call(0x11e08, StartupStop::Cancelled, PS3_COMPILED_DISPATCH_STOP_CANCELLED, true, false, false, false);
    call(0x11e08, StartupStop::Deadline, PS3_COMPILED_DISPATCH_STOP_DEADLINE, false, true, false, false);
    call(0x11e08, StartupStop::DispatchFailure, PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED, false, false, true, false);
    call(0x55da84, StartupStop::MissingServices, PS3_COMPILED_DISPATCH_OK, false, false, false, true);
    VERIFY(memory.write(memory.context, ARGV, oracle.find(ARGV, 4096), 4096) == PS3_GUEST_MEMORY_OK);
    std::printf("INDIRECT_NATIVE_%s failures=%d actual_leaf_and_import_targets=true no_host_target_fixture=true\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
int test_binding_identity(const ps3_guest_memory *backing,
    const Ps3PpuExecutionServices &services, TlsProvider &provider,
    PpuMemoryBinding &binding, const ppu_context &initial, Oracle &oracle,
    int32_t priority)
{
    int failures = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("CONTEXT_BINDING_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    Ps3PpuExecutionServices selected = services;
    auto control_storage = std::make_unique<StartupControl>();
    StartupControl &control = *control_storage;
    auto rejected_storage = std::make_unique<StartupControl>();
    StartupControl &rejected_control = *rejected_storage;
    configure(control, oracle);
    StartupExecution owner{};
    prepare(owner, initial, selected, control, binding, priority);
    const PpuMemoryStopPort original_boundary = owner.ppu.boundary;
    PpuMemoryTraceJournal *const original_trace = owner.ppu.trace;
    owner.control = &rejected_control;
    VERIFY(!native_startup_admit(owner, binding, selected));
    VERIFY(owner.ppu.trace == original_trace && !rejected_control.cpu);
    VERIFY(owner.ppu.boundary.context == original_boundary.context && owner.ppu.admitted);
    owner.control = &control;
    VERIFY(native_startup_release_after_join(owner) == PS3_GUEST_MEMORY_OK);
    selected.memory = backing;
    StartupExecution rejected{};
    rejected.control = &rejected_control;
    VERIFY(!native_startup_admit(rejected, binding, selected));
    VERIFY(!rejected.ppu.admitted && !rejected.ppu.trace && !rejected_control.cpu);
    // Same real configuration must admit before testing the observer compatibility guard.
    auto recorder_control_storage = std::make_unique<StartupControl>();
    StartupControl &recorder_control = *recorder_control_storage;
    configure(recorder_control, oracle);
    auto recorder_positive_storage = std::make_unique<StartupExecution>();
    StartupExecution &recorder_positive = *recorder_positive_storage;
    auto initialize_candidate = [&](StartupExecution &candidate, StartupControl &candidate_control) {
        candidate.ppu.cpu = initial;
        candidate.control = &candidate_control;
        candidate.guest_priority = priority;
        candidate.has_guest_priority = true;
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
        candidate.dispatch_stop = {&candidate_control, owner.dispatch_stop.stop_requested};
#endif
    };
    initialize_candidate(recorder_positive, recorder_control);
    const bool positive_admitted = native_startup_admit(recorder_positive, binding, services);
    VERIFY(positive_admitted && recorder_positive.ppu.admitted);
    if (positive_admitted) VERIFY(native_startup_release_after_join(recorder_positive) == PS3_GUEST_MEMORY_OK);
    auto recorder_rejected_control_storage = std::make_unique<StartupControl>();
    StartupControl &recorder_rejected_control = *recorder_rejected_control_storage;
    configure(recorder_rejected_control, oracle);
    auto recorder_rejected_storage = std::make_unique<StartupExecution>();
    StartupExecution &recorder_rejected = *recorder_rejected_storage;
    initialize_candidate(recorder_rejected, recorder_rejected_control);
    PpuMemoryTraceEvent external_events[2]{};
    PpuMemoryTraceRecorder external_recorder;
    VERIFY(external_recorder.configure(external_events, 2, PpuMemoryTraceRetention::Suffix));
    VERIFY(external_recorder.begin(2));
    external_recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Hle, true, 0x1234, 8, 0x1122));
    recorder_rejected.ppu.recorder = &external_recorder;
    const auto external_before = external_recorder.report();
    PpuMemoryTraceSample sample_before{}, sample_after{};
    VERIFY(external_recorder.read(0, sample_before));
    const unsigned tls_calls_before = provider.calls, free_calls_before = provider.free_calls;
    std::vector<unsigned char> slots_before(binding.domain->owner_count * sizeof(*binding.domain->owners));
    std::memcpy(slots_before.data(), binding.domain->owners, slots_before.size());
    const bool recorder_admitted = native_startup_admit(recorder_rejected, binding, services);
    VERIFY(!recorder_admitted);
    VERIFY(!recorder_rejected.ppu.admitted && !recorder_rejected.ppu.services && !recorder_rejected.ppu.memory);
    VERIFY(!recorder_rejected.ppu.trace && !recorder_rejected.ppu.boundary.stop && !recorder_rejected_control.cpu);
    VERIFY(!recorder_rejected.ppu.reservation.domain);
    VERIFY(std::memcmp(&recorder_rejected.ppu.cpu, &initial, sizeof(initial)) == 0);
    VERIFY(std::memcmp(slots_before.data(), binding.domain->owners, slots_before.size()) == 0);
    VERIFY(provider.calls == tls_calls_before && provider.free_calls == free_calls_before);
    VERIFY(external_recorder.report().batch_active == external_before.batch_active &&
           external_recorder.report().remaining_publications == external_before.remaining_publications &&
           external_recorder.report().published_count == external_before.published_count);
    VERIFY(external_recorder.read(0, sample_after) && sample_after.sequence == sample_before.sequence &&
           ppu_memory_event_equal(sample_after.event, sample_before.event));
    if (recorder_admitted) VERIFY(native_startup_release_after_join(recorder_rejected) == PS3_GUEST_MEMORY_OK);
    external_recorder.finish(); // Only the caller owns this external batch.
    for (StartupEntry entry : {StartupEntry::ClockStub, StartupEntry::MemorySyscall}) {
        selected = services;
        auto invocation_storage = std::make_unique<StartupControl>();
        StartupControl &invocation = *invocation_storage;
        configure(invocation, oracle);
        invocation.entry = entry;
        StartupExecution execution{};
        prepare(execution, initial, selected, invocation, binding, priority);
        execution.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        execution.ppu.cpu.gpr[3] = 0x40000000;
        execution.ppu.cpu.gpr[11] = 349;
        const ppu_context before = execution.ppu.cpu;
        uint64_t memory_before = 0, memory_after = 0;
        const ps3_guest_memory &memory = binding.coherent_memory;
        VERIFY(memory.read(memory.context, STACK_TOP + 0x128, &memory_before, 8) == PS3_GUEST_MEMORY_OK);
        const unsigned frees_before = provider.free_calls;
        selected.memory = backing; // Adversarial substitution AFTER admission.
        VERIFY(run(execution));
        VERIFY(invocation.stop == StartupStop::MissingServices);
        VERIFY(invocation.import_count == 0 && invocation.clock_calls == 0 && invocation.memory_returns == 0);
        VERIFY(invocation.journal.count == 0 && provider.free_calls == frees_before);
        VERIFY(!std::memcmp(&execution.ppu.cpu, &before, sizeof(before)));
        VERIFY(memory.read(memory.context, STACK_TOP + 0x128, &memory_after, 8) == PS3_GUEST_MEMORY_OK);
        VERIFY(memory_before == memory_after);
    }
    std::printf("CONTEXT_BINDING_%s drift_rejected_before_provider=true free_syscall_covered=true admission_failure_unchanged=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
int test_reservation_boundaries(const Ps3PpuExecutionServices &services,
    PpuMemoryBinding &binding, const ppu_context &initial, Oracle &oracle,
    int32_t priority)
{
    int failures = 0;
#define VERIFY(value) do { if (!(value)) { std::printf("CONTEXT_RESERVATION_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    constexpr uint32_t reserved = ARGV + 0x300;
    auto exercise = [&](StartupEntry entry, uint32_t reservation_address,
                        uint64_t target, bool cancel, bool should_store,
                        StartupStop expected_stop, bool fill_trace = false) {
        auto control_storage = std::make_unique<StartupControl>();
        StartupControl &control = *control_storage;
        configure(control, oracle);
        control.entry = entry;
        StartupExecution owner{};
        prepare(owner, initial, services, control, binding, priority);
        owner.ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner.ppu.cpu.gpr[4] = target;
        owner.ppu.cpu.gpr[5] = 17;
        const uint32_t observed = ppu_native_load_reserve32(&owner.ppu.cpu, reservation_address);
        const ppu_context before = owner.ppu.cpu;
        if (fill_trace) control.journal.count = control.journal.capacity;
        control.cancelled.store(cancel, std::memory_order_release);
        Thread worker;
        if (!start(owner, worker)) std::abort();
        join(worker);
        VERIFY(control.stop == expected_stop);
        if (expected_stop != StartupStop::Returned)
            VERIFY(!std::memcmp(&owner.ppu.cpu, &before, sizeof(before)));
        control.cancelled.store(false, std::memory_order_release);
        control.journal.count = 0;
        VERIFY(ppu_native_store_conditional32(&owner.ppu.cpu, reservation_address, observed) == should_store);
        VERIFY(native_startup_release_after_join(owner) == PS3_GUEST_MEMORY_OK);
    };
    // Actual clock HLE returns normally; its unrelated TOC write preserves LR.
    exercise(StartupEntry::ClockStub, reserved, 0, false, true, StartupStop::Returned);
    // The same actual HLE TOC write invalidates an overlapping reservation.
    exercise(StartupEntry::ClockStub, STACK_TOP + 0x128, 0, false, false, StartupStop::Returned);
    // Cancellation precedes the SC instruction, so it is not an attempted SC.
    exercise(StartupEntry::MemorySc32, reserved, reserved, true, true, StartupStop::Cancelled);
    // Rejected SC instructions consume their reservation before failing closed.
    exercise(StartupEntry::MemorySc32, reserved, 0x10000, false, false, StartupStop::InvalidMemory);
    exercise(StartupEntry::MemorySc32, reserved, 0xffffffff, false, false, StartupStop::InvalidMemory);
    exercise(StartupEntry::MemorySc32, reserved, reserved, false, false, StartupStop::TraceCapacity, true);
    const ps3_guest_memory &memory = binding.coherent_memory;
    VERIFY(memory.write(memory.context, ARGV, oracle.find(ARGV, 4096), 4096) == PS3_GUEST_MEMORY_OK);
    VERIFY(memory.write(memory.context, STACK_TOP, oracle.find(STACK_TOP, ABI_SCRATCH_BYTES),
        ABI_SCRATCH_BYTES) == PS3_GUEST_MEMORY_OK);
    std::printf("CONTEXT_RESERVATION_%s nonoverlap_hle_preserves=true overlapping_hle_invalidates=true cancellation_not_attempt=true rejected_sc_consumes=true failures=%d\n",
        failures ? "FAIL" : "PASS", failures);
    return failures;
#undef VERIFY
}
void model_create_to_syscall(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    const uint32_t object = uint32_t(cpu.gpr[3]);
    const uint32_t words[] = {0xffffffff, 0, 0x12, 0, queue, 0};
    for (unsigned index = 0; index < 6; ++index) memory.write(object + index * 4, words[index], 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
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



namespace {
void model_publish_mutex(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    const uint32_t address = uint32_t(cpu.gpr[3]);
    const uint32_t attributes = uint32_t(cpu.gpr[4]);
    const uint32_t combined = uint32_t(memory.read(attributes, 4, false) | memory.read(attributes + 4, 4, false));
    const uint32_t words[] = {0xffffffff, 0, combined, 0, queue, 0};
    for (unsigned index = 0; index < 6; ++index) memory.write(address + index * 4, words[index], 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
}
void model_record(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    cpu.gpr[11] = cpu.gpr[3];
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[3] += 0x10;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x6754, 4);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[9] = 0;
    cpu.gpr[0] = 0;
    cpu.gpr[3] = uint32_t(cpu.gpr[3]);
    memory.write(cpu.gpr[11], cpu.gpr[0], 4);
    memory.write(cpu.gpr[11] + 8, cpu.gpr[9], 8);
    cpu.lr = 0x452dd0;
    model_publish_mutex(cpu, memory, queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
}
void model_info_and_second_mutex(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    // 352 output policy is the configured quota, never a sampled host capacity.
    memory.write(cpu.gpr[3], 213 * 1024 * 1024, 4, false);
    memory.write(cpu.gpr[3] + 4, 213 * 1024 * 1024, 4, false);
    cpu.gpr[3] = 0;
    cpu.gpr[29] = memory.read(cpu.gpr[2] - 0x673c, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x78, 4);
    cpu.gpr[0] = (uint32_t(cpu.gpr[0]) >> 20) & 0xfff;
    cpu.gpr[29] = memory.read(cpu.gpr[29], 4);
    set_cr(cpu, uint32_t(cpu.gpr[0]), uint32_t(cpu.gpr[29]), false);
    if (cpu.cr & 8) { memory.valid = false; return; }
    cpu.gpr[0] = uint32_t(cpu.gpr[29]) << 20;
    uint64_t saved[5];
    for (unsigned index = 0; index < 5; ++index)
        saved[index] = memory.read(cpu.gpr[1] + 0xc8 + index * 8, 8, false);
    cpu.gpr[27] = saved[0];
    cpu.gpr[28] = saved[1];
    memory.write(cpu.gpr[30] + 0x24, cpu.gpr[0], 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x100, 8);
    cpu.gpr[29] = saved[2];
    cpu.gpr[30] = saved[3];
    cpu.lr = cpu.gpr[0];
    cpu.gpr[31] = saved[4];
    cpu.gpr[1] += 0xf0;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7ff8, 4);
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7ff4, 4);
    cpu.lr = 0x10438;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x6754, 4);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x6758, 4);
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x452e10;
    model_publish_mutex(cpu, memory, queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
}
}


namespace {
void model_stream_records(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    cpu.lr = 0x10440;
    const uint64_t saved[] = {cpu.gpr[28], cpu.gpr[29], cpu.gpr[30], cpu.gpr[31]};
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x70, cpu.gpr[28], 8);
    cpu.gpr[28] = memory.read(cpu.gpr[2] - 0x6804, 4);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[0] = cpu.gpr[28] + 0xd0;
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    cpu.gpr[31] = cpu.gpr[0];
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[30], 8);
    cpu.gpr[29] = cpu.gpr[0];
    cpu.gpr[30] = cpu.gpr[31] + 0x78;
    for (unsigned index = 0; index < 3; ++index) {
        cpu.gpr[3] = uint32_t(cpu.gpr[31]);
        cpu.gpr[31] += 0x28;
        cpu.lr = 0x44f03c;
        model_record(cpu, memory, queue++);
        set_cr(cpu, cpu.gpr[30], cpu.gpr[31], true);
    }
    cpu.gpr[6] = memory.read(cpu.gpr[2] - 0x6800, 4);
    cpu.gpr[31] = cpu.gpr[28] + 8;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x67f8, 4);
    cpu.gpr[5] = memory.read(cpu.gpr[2] - 0x67fc, 4);
    cpu.gpr[30] = cpu.gpr[31] + 0xc8;
    cpu.gpr[0] = memory.read(cpu.gpr[6] + 4, 2);
    cpu.gpr[11] = memory.read(cpu.gpr[4] + 4, 2);
    cpu.gpr[0] = uint64_t(int64_t(int16_t(cpu.gpr[0])));
    cpu.gpr[9] = memory.read(cpu.gpr[5] + 4, 2);
    cpu.gpr[11] = uint64_t(int64_t(int16_t(cpu.gpr[11])));
    cpu.gpr[9] = uint64_t(int64_t(int16_t(cpu.gpr[9])));
    cpu.gpr[10] = uint32_t(cpu.gpr[0]) << 5;
    cpu.gpr[0] = uint32_t(cpu.gpr[0]) << 3;
    cpu.gpr[8] = uint32_t(cpu.gpr[9]) << 5;
    cpu.gpr[7] = uint32_t(cpu.gpr[11]) << 5;
    cpu.gpr[9] = uint32_t(cpu.gpr[9]) << 3;
    cpu.gpr[11] = uint32_t(cpu.gpr[11]) << 3;
    cpu.gpr[0] += cpu.gpr[10];
    cpu.gpr[9] += cpu.gpr[8];
    cpu.gpr[0] += cpu.gpr[29];
    cpu.gpr[11] += cpu.gpr[7];
    cpu.gpr[9] += cpu.gpr[29];
    cpu.gpr[11] += cpu.gpr[29];
    memory.write(cpu.gpr[6] + 0x44, cpu.gpr[0], 4);
    cpu.gpr[0] = 0x100;
    memory.write(cpu.gpr[5] + 0x44, cpu.gpr[9], 4);
    memory.write(cpu.gpr[4] + 0x44, cpu.gpr[11], 4);
    memory.write(cpu.gpr[28] + 0x148, cpu.gpr[0], 4);
    for (unsigned index = 0; index < 5; ++index) {
        cpu.gpr[3] = uint32_t(cpu.gpr[31]);
        cpu.gpr[31] += 0x28;
        cpu.lr = 0x44f0c4;
        model_record(cpu, memory, queue++);
        set_cr(cpu, cpu.gpr[30], cpu.gpr[31], true);
    }
    cpu.gpr[0] = 5;
    cpu.gpr[29] = saved[1];
    cpu.gpr[30] = saved[2];
    memory.write(cpu.gpr[28], cpu.gpr[0], 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0xa0, 8);
    cpu.gpr[28] = saved[0];
    cpu.gpr[31] = saved[3];
    cpu.lr = cpu.gpr[0];
    cpu.gpr[1] += 0x90;
}
void model_descriptor_table(ppu_context &cpu, Oracle &memory)
{
    cpu.lr = 0x10448;
    cpu.gpr[8] = memory.read(cpu.gpr[2] - 0x675c, 4);
    cpu.gpr[9] = 0xfd;
    cpu.gpr[0] = 0;
    cpu.ctr = uint32_t(cpu.gpr[9]);
    cpu.gpr[11] = 2;
    cpu.gpr[9] = 1;
    cpu.gpr[10] = 0;
    cpu.gpr[5] = UINT64_MAX;
    cpu.gpr[6] = cpu.gpr[8] + 0x400;
    cpu.gpr[7] = 0;
    memory.write(cpu.gpr[8] + 4, cpu.gpr[9], 4);
    memory.write(cpu.gpr[8] + 8, cpu.gpr[11], 4);
    memory.write(cpu.gpr[8] + 0x408, cpu.gpr[0], 4);
    memory.write(cpu.gpr[8], cpu.gpr[0], 4);
    memory.write(cpu.gpr[8] + 0x400, cpu.gpr[0], 4);
    memory.write(cpu.gpr[8] + 0x404, cpu.gpr[0], 4);
    for (unsigned index = 0; index < 253; ++index) {
        cpu.gpr[9] = uint32_t(cpu.gpr[10] + cpu.gpr[8]);
        cpu.gpr[11] = uint32_t(cpu.gpr[10] + cpu.gpr[6]);
        cpu.gpr[10] += 4;
        memory.write(cpu.gpr[9] + 0xc, cpu.gpr[5], 4);
        memory.write(cpu.gpr[11] + 0xc, cpu.gpr[7], 4);
        cpu.ctr = uint32_t(cpu.ctr - 1);
    }
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7fec, 4);
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7ff0, 4);
    cpu.lr = 0x10460;
}
}


namespace {
void model_clock_to_indirect(ppu_context &cpu, Oracle &memory, uint64_t sample, uint32_t next_queue)
{
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = sample;
    cpu.gpr[2] = 0x78bf78;
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x7fe8, 4);
    memory.write(cpu.gpr[9], cpu.gpr[3], 8);
    cpu.lr = 0x10470;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x10210;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[2] - 0x7fc0, 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x7fc4, 4);
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x7fbc, 4);
    cpu.lr = 0x105f4;
    // Actual original462F20 and unit003's standalone462E60 definition.
    cpu.gpr[5] = 0;
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[6] = 0;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x462f38;
    const uint32_t high_cr = int64_t(cpu.gpr[3]) < 0 ? 8 : int64_t(cpu.gpr[3]) > 0 ? 4 : 2;
    cpu.cr = (cpu.cr & 0x0fffffffu) | (high_cr << 28);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    memory.write(cpu.gpr[1] + 0x70, cpu.gpr[28], 8);
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[31] = cpu.gpr[4];
    if (high_cr == 2) { memory.valid = false; return; }
    cpu.gpr[0] = memory.read(cpu.gpr[3], 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    cpu.gpr[0] = UINT64_MAX;
    memory.write(cpu.gpr[4] + 0xc, cpu.gpr[3], 4);
    memory.write(cpu.gpr[4], cpu.gpr[0], 4);
    cpu.gpr[0] = 0x1fe00000;
    memory.write(cpu.gpr[4] + 4, cpu.gpr[5], 4);
    memory.write(cpu.gpr[4] + 0x10, cpu.gpr[0], 4);
    memory.write(cpu.gpr[4] + 8, cpu.gpr[6], 4);
    cpu.lr = 0x462ed0;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x65d8, 4);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x65dc, 4);
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x462368;
    // Actual standalone463EB0, stopping before its real indirect call.
    memory.write(cpu.gpr[1] - 0x90, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x90;
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[29], 8);
    memory.write(cpu.gpr[1] + 0xa0, cpu.gpr[0], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[3], 4);
    cpu.gpr[29] = cpu.gpr[4];
    memory.write(cpu.gpr[1] + 0x88, cpu.gpr[31], 8);
    set_cr(cpu, cpu.gpr[0], 0, true);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[30], 8);
    cpu.gpr[31] = cpu.gpr[3];
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x65c0, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[30] + 0x70, 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.lr = 0x463f8c;
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x65c0, 4);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.gpr[0] = 2;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x65c4, 4);
    cpu.gpr[3] = cpu.gpr[9] + 0x58;
    memory.write(cpu.gpr[9] + 0x70, cpu.gpr[0], 4);
    cpu.lr = 0x463e8c;
    model_publish_mutex(cpu, memory, next_queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[0] = cpu.gpr[30] + 0x58;
    cpu.gpr[30] = uint32_t(cpu.gpr[0]);
    cpu.gpr[4] = 0;
    cpu.gpr[3] = cpu.gpr[30];
    cpu.lr = 0x463fa4;
    memory.write(cpu.gpr[3], cpu.thread_id, 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[31], 4);
    cpu.gpr[4] = uint32_t(cpu.gpr[29]);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8);
    cpu.gpr[0] = memory.read(cpu.gpr[4], 4);
    cpu.gpr[2] = memory.read(cpu.gpr[4] + 4, 4);
    cpu.ctr = uint32_t(cpu.gpr[0]);
}
void model_mutex_transition(ppu_context &cpu, Oracle &memory, bool lock)
{
    const uint32_t address = uint32_t(cpu.gpr[3]);
    const uint32_t wanted_owner = lock ? UINT32_MAX : uint32_t(cpu.thread_id);
    if (memory.read(address, 4, false) != wanted_owner) memory.valid = false;
    memory.write(address, lock ? uint32_t(cpu.thread_id) : UINT32_MAX, 4, false);
    memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
    cpu.gpr[3] = 0;
    cpu.gpr[2] = 0x78bf78;
}
void model_mutex_boolean_wrapper(ppu_context &cpu, Oracle &memory, bool lock)
{
    if (lock) cpu.gpr[4] = 0;
    const uint64_t caller_lr = cpu.lr;
    if (lock) cpu.gpr[0] = caller_lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    if (!lock) cpu.gpr[0] = caller_lr;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = lock ? 0x463dec : 0x463db0;
    model_mutex_transition(cpu, memory, lock);
    // The supported successful HLE return is zero. Arithmetic shift by31
    // yields zero and clears XER carry; no production PPC helper is used.
    cpu.xer &= ~UINT64_C(0x20000000);
    cpu.gpr[0] = 0;
    cpu.gpr[3] = 0;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.gpr[3] = 0;
    cpu.lr = cpu.gpr[0];
}
void model_first_indirect_return(ppu_context &cpu, Oracle &memory, uint32_t queue)
{
    cpu.gpr[3] = memory.read(cpu.gpr[2] - 0x65dc, 4);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    cpu.gpr[3] = uint32_t(cpu.gpr[3] + 8);
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x462398;
    cpu.gpr[4] = memory.read(cpu.gpr[2] - 0x65c4, 4);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x70, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x70;
    memory.write(cpu.gpr[1] + 0x80, cpu.gpr[0], 8);
    cpu.lr = 0x463e2c;
    model_publish_mutex(cpu, memory, queue);
    cpu.gpr[2] = 0x78bf78;
    set_cr(cpu, cpu.gpr[3], 0, true);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    // Return through463EB0, then its exact callers462350/462E60/462F20.
    cpu.gpr[2] = 0x78bf78;
    cpu.gpr[0] = 2;
    memory.write(cpu.gpr[31], cpu.gpr[0], 4);
    cpu.gpr[3] = cpu.gpr[30];
    cpu.lr = 0x463f24;
    model_mutex_transition(cpu, memory, false);
    cpu.gpr[29] = memory.read(cpu.gpr[1] + 0x78, 8, false);
    cpu.gpr[30] = memory.read(cpu.gpr[1] + 0x80, 8, false);
    cpu.xer &= ~UINT64_C(0x20000000);
    cpu.gpr[0] = 0;
    cpu.gpr[31] = memory.read(cpu.gpr[1] + 0x88, 8, false);
    cpu.gpr[9] = 0;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0xa0, 8);
    cpu.gpr[1] += 0x90;
    cpu.gpr[9] = 1;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[3] = 0;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.gpr[28] = memory.read(cpu.gpr[2] - 0x65dc, 4);
    cpu.gpr[29] = uint32_t(cpu.gpr[28] + 8);
    cpu.gpr[3] = cpu.gpr[29];
    cpu.lr = 0x462ee4;
    model_mutex_boolean_wrapper(cpu, memory, true);
    cpu.gpr[0] = memory.read(cpu.gpr[28] + 0x20, 4);
    cpu.gpr[3] = cpu.gpr[29];
    memory.write(cpu.gpr[31] + 0x14, cpu.gpr[0], 4);
    memory.write(cpu.gpr[28] + 0x20, cpu.gpr[31], 4);
    cpu.lr = 0x462efc;
    model_mutex_boolean_wrapper(cpu, memory, false);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0xa0, 8);
    cpu.gpr[28] = memory.read(cpu.gpr[1] + 0x70, 8, false);
    cpu.gpr[29] = memory.read(cpu.gpr[1] + 0x78, 8, false);
    cpu.lr = cpu.gpr[0];
    cpu.gpr[31] = memory.read(cpu.gpr[1] + 0x88, 8, false);
    cpu.gpr[1] += 0x90;
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    // Remaining105D0 path must not introduce an unmodelled second dispatch.
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x7fb8, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[9], 4);
    set_cr(cpu, cpu.gpr[0], 0, true);
    if (!(cpu.cr & 2)) {
        cpu.gpr[0] = memory.read(cpu.gpr[2] - 0x7fb4, 4);
        set_cr(cpu, cpu.gpr[0], 0, true);
        cpu.gpr[9] = uint32_t(cpu.gpr[0]);
        if (!(cpu.cr & 2)) { memory.valid = false; return; }
    }
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x80, 8);
    cpu.gpr[1] += 0x70;
    cpu.lr = cpu.gpr[0];
    cpu.lr = 0x10218;
}
void model_bf188(ppu_context &cpu, Oracle &memory)
{
    const uint64_t saved[] = {cpu.gpr[25], cpu.gpr[26], cpu.gpr[27], cpu.gpr[28], cpu.gpr[29], cpu.gpr[30]};
    memory.write(cpu.gpr[1] - 0x10, cpu.gpr[30], 8);
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x7e80, 4);
    cpu.gpr[6] = cpu.gpr[3] + 0xc4;
    cpu.gpr[11] = cpu.gpr[3];
    memory.write(cpu.gpr[1] - 0x38, cpu.gpr[25], 8);
    cpu.gpr[7] = cpu.gpr[3] + 4;
    cpu.gpr[8] = cpu.gpr[3] + 0xa4;
    cpu.gpr[10] = cpu.gpr[3] + 0xb4;
    cpu.gpr[25] = uint32_t(cpu.gpr[6]);
    memory.write(cpu.gpr[1] - 0x30, cpu.gpr[26], 8);
    cpu.gpr[9] = 0;
    cpu.gpr[26] = memory.read(cpu.gpr[30] - 0x7fe4, 4);
    cpu.gpr[0] = 0;
    memory.write(cpu.gpr[1] - 0x18, cpu.gpr[29], 8);
    cpu.gpr[29] = memory.read(cpu.gpr[30] - 0x7fe8, 4);
    cpu.gpr[5] = 0x35;
    memory.write(cpu.gpr[1] - 0x28, cpu.gpr[27], 8);
    memory.write(cpu.gpr[1] - 0x20, cpu.gpr[28], 8);
    memory.write(cpu.gpr[3], 0, 1);
    cpu.gpr[28] = cpu.gpr[26] + 0x24;
    cpu.gpr[27] = 5;
    cpu.gpr[9] = UINT64_MAX;
    for (uint32_t offset : {0x14u, 0x20u, 0x24u, 0x1cu, 0x18u}) memory.write(cpu.gpr[3] + offset, 0, 4);
    memory.write(cpu.gpr[11] + 0x28, cpu.gpr[29], 4);
    cpu.gpr[11] += 0x28;
    memory.write(cpu.gpr[11] + 4, cpu.gpr[28], 4);
    memory.write(cpu.gpr[11], cpu.gpr[26], 4);
    memory.write(cpu.gpr[11] + 0xc, 0, 4);
    memory.write(cpu.gpr[8] + 8, cpu.gpr[5], 4);
    memory.write(cpu.gpr[8] + 0xc, cpu.gpr[9], 4);
    memory.write(cpu.gpr[8] + 4, 0, 4);
    memory.write(cpu.gpr[10] + 8, cpu.gpr[27], 4);
    memory.write(cpu.gpr[10] + 0xc, 0, 4);
    memory.write(cpu.gpr[10] + 4, 0, 4);
    memory.write(cpu.gpr[6] + 8, cpu.gpr[27], 4);
    memory.write(cpu.gpr[25] + 4, 0, 4);
    memory.write(cpu.gpr[10], 0, 4);
    memory.write(cpu.gpr[6] + 0xc, 0, 4);
    memory.write(cpu.gpr[25], 0, 4);
    memory.write(cpu.gpr[11] + 8, 0, 4);
    memory.write(cpu.gpr[8], 0, 4);
    for (uint32_t offset : {0xcu, 0u, 4u, 8u}) memory.write(cpu.gpr[7] + offset, 0, 4);
    for (unsigned index = 0; index < 6; ++index) cpu.gpr[25 + index] = saved[index];
    memory.write(cpu.gpr[3] + 0xcc, cpu.gpr[5], 4);
    memory.write(cpu.gpr[3] + 0xbc, cpu.gpr[5], 4);
}
void model_bf9e0_drain(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[3] = 1;
    cpu.gpr[4] = 0x2710;
    const uint64_t saved29 = cpu.gpr[29], saved30 = cpu.gpr[30];
    set_cr(cpu, cpu.gpr[3], 1, true);
    memory.write(cpu.gpr[1] - 0x10, cpu.gpr[30], 8);
    memory.write(cpu.gpr[1] - 0x18, cpu.gpr[29], 8);
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x7e80, 4);
    cpu.gpr[0] = cpu.gpr[4] ^ UINT64_C(0xffff0000);
    set_cr(cpu, cpu.gpr[0], UINT64_MAX, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    set_cr(cpu, cpu.gpr[4], 0x2710, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.gpr[3] = memory.read(cpu.gpr[30] - 0x7fcc, 4);
    cpu.gpr[30] = saved30;
    cpu.gpr[3] += 0x1150;
    cpu.gpr[29] = saved29;
    model_bf188(cpu, memory);
}
void model_b03e8_drain(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[3] = 1;
    cpu.gpr[4] = 0x7d00;
    const uint64_t saved[] = {cpu.gpr[25], cpu.gpr[26], cpu.gpr[27], cpu.gpr[28], cpu.gpr[29], cpu.gpr[30]};
    set_cr(cpu, cpu.gpr[3], 1, true);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x140, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x140;
    memory.write(cpu.gpr[1] + 0x130, cpu.gpr[30], 8);
    for (unsigned index = 0; index < 5; ++index) memory.write(cpu.gpr[1] + 0x108 + index * 8, cpu.gpr[25 + index], 8);
    memory.write(cpu.gpr[1] + 0x150, cpu.gpr[0], 8);
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x7e9c, 4);
    cpu.gpr[0] = cpu.gpr[4] ^ UINT64_C(0xffff0000);
    set_cr(cpu, cpu.gpr[0], UINT64_MAX, true);
    if (cpu.cr & 2) { memory.valid = false; return; }
    set_cr(cpu, cpu.gpr[4], 0x7d00, true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.gpr[9] = memory.read(cpu.gpr[30] - 0x7ff0, 4);
    cpu.gpr[0] = 0;
    for (unsigned index = 0; index < 4; ++index) cpu.gpr[25 + index] = saved[index];
    memory.write(cpu.gpr[9] + 4, 0, 4);
    memory.write(cpu.gpr[9], 0, 4);
    cpu.gpr[0] = memory.read(cpu.gpr[1] + 0x150, 8);
    cpu.gpr[29] = saved[4];
    cpu.gpr[30] = saved[5];
    cpu.lr = cpu.gpr[0];
    cpu.gpr[1] += 0x140;
}
void model_4204b0(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[9] = 0x1fff;
    cpu.gpr[10] = cpu.gpr[3] + 0xc;
    cpu.gpr[0] = 0;
    cpu.gpr[11] = UINT64_MAX;
    memory.write(cpu.gpr[3] + 0x198, cpu.gpr[9], 4);
    cpu.gpr[9] = 1;
    cpu.gpr[10] = uint32_t(cpu.gpr[10]);
    memory.write(cpu.gpr[3] + 0x194, cpu.gpr[11], 4);
    memory.write(cpu.gpr[3] + 0x190, cpu.gpr[11], 4);
    for (uint32_t offset : {0u, 4u, 8u}) memory.write(cpu.gpr[3] + offset, 0, 4);
    for (unsigned index = 0; index < 6; ++index) {
        memory.write(cpu.gpr[3] + 0x19c + index, 0, 1);
        memory.write(cpu.gpr[3] + 0x1bc + index * 4, 0, 4);
    }
    for (uint32_t offset = 0xdd8; offset <= 0xe14; offset += 4) memory.write(cpu.gpr[3] + offset, 0, 4);
    cpu.gpr[11] = memory.read(cpu.gpr[2] - 0x78b8, 4);
    cpu.gpr[8] = memory.read(cpu.gpr[2] - 0x78bc, 4);
    memory.write(cpu.gpr[3] + 0x18c, cpu.gpr[9], 4);
    cpu.gpr[9] = 0xa6;
    memory.write(cpu.gpr[10] + 8, cpu.gpr[11], 4);
    memory.write(cpu.gpr[10], 0, 2);
    memory.write(cpu.gpr[10] + 2, cpu.gpr[9], 2);
    memory.write(cpu.gpr[10] + 4, cpu.gpr[8], 4);
}
void model_557838_until_147(ppu_context &cpu, Oracle &memory)
{
    cpu.gpr[4] = 0xffff;
    cpu.gpr[3] = 1;
    memory.write(cpu.gpr[1] - 0xa0, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0xa0;
    cpu.gpr[0] = cpu.lr;
    set_cr(cpu, cpu.gpr[3], 1, true);
    memory.write(cpu.gpr[1] + 0xb0, cpu.gpr[0], 8);
    cpu.gpr[0] = 0;
    memory.write(cpu.gpr[1] + 0x90, cpu.gpr[30], 8);
    for (unsigned index = 0; index < 3; ++index) memory.write(cpu.gpr[1] + 0x78 + index * 8, cpu.gpr[27 + index], 8);
    cpu.gpr[30] = memory.read(cpu.gpr[2] - 0x6128, 4);
    cpu.gpr[0] = 0xffff;
    set_cr(cpu, cpu.gpr[4], cpu.gpr[0], true);
    if (!(cpu.cr & 2)) { memory.valid = false; return; }
    cpu.gpr[3] = memory.read(cpu.gpr[30] - 0x8000, 4);
    cpu.gpr[28] = 0;
    cpu.lr = 0x5577c4;
    model_4204b0(cpu, memory);
    cpu.gpr[29] = memory.read(cpu.gpr[30] - 0x7ffc, 4);
    cpu.gpr[11] = 147;
    memory.write(cpu.gpr[29] + 0x40, cpu.gpr[28], 4);
    memory.write(cpu.gpr[29] + 0x28, cpu.gpr[28], 4);
}
void model_initializer_loop(ppu_context &cpu, Oracle &memory, std::vector<ppu_context> &entries)
{
    cpu.gpr[9] = memory.read(cpu.gpr[2] - 0x7920, 4);
    cpu.gpr[0] = cpu.lr;
    memory.write(cpu.gpr[1] - 0x80, cpu.gpr[1], 8);
    cpu.gpr[1] -= 0x80;
    memory.write(cpu.gpr[1] + 0x78, cpu.gpr[31], 8);
    cpu.gpr[31] = cpu.gpr[9] - 4;
    memory.write(cpu.gpr[1] + 0x90, cpu.gpr[0], 8);
    cpu.gpr[11] = uint32_t(cpu.gpr[31]);
    cpu.gpr[9] = memory.read(cpu.gpr[11], 4);
    set_cr(cpu, cpu.gpr[9], UINT64_MAX, true);
    constexpr uint32_t targets[] = {0xbf9e0, 0xb03e8, 0x557838};
    for (uint32_t target : targets) {
        if (cpu.cr & 2) { memory.valid = false; return; }
        cpu.gpr[11] = uint32_t(cpu.gpr[9]);
        cpu.gpr[31] -= 4;
        cpu.gpr[0] = memory.read(cpu.gpr[11], 4);
        memory.write(cpu.gpr[1] + 0x28, cpu.gpr[2], 8);
        cpu.ctr = uint32_t(cpu.gpr[0]);
        cpu.gpr[2] = memory.read(cpu.gpr[11] + 4, 4);
        if (cpu.ctr != target) { memory.valid = false; return; }
        entries.push_back(cpu);
        if (target == 0xbf9e0) model_bf9e0_drain(cpu, memory);
        else if (target == 0xb03e8) model_b03e8_drain(cpu, memory);
        else { model_557838_until_147(cpu, memory); return; }
        cpu.gpr[2] = 0x78bf78;
        cpu.gpr[9] = uint32_t(cpu.gpr[31]);
        cpu.gpr[9] = memory.read(cpu.gpr[9], 4);
        set_cr(cpu, cpu.gpr[9], UINT64_MAX, true);
    }
}

bool inspect_unlocked_resources_after_join(const ps3_guest_memory &memory,
    const ps3_lwmutex_object *objects, size_t object_count,
    const ps3_lwmutex_waiter *waiters, size_t waiter_count, size_t expected_count, bool joined)
{
    if (!joined) return false;
    for (size_t index = 0; index < waiter_count; ++index) if (waiters[index].state) return false;
    size_t count = 0;
    for (size_t index = 0; index < object_count; ++index) {
        const auto &object = objects[index];
        if (!object.state) continue;
        if (object.active_operations) return false;
        unsigned char bytes[24];
        if (memory.read(memory.context, object.address, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK) return false;
        uint32_t words[6]{};
        for (unsigned word = 0; word < 6; ++word)
            for (unsigned byte = 0; byte < 4; ++byte) words[word] = (words[word] << 8) | bytes[word * 4 + byte];
        if (words[0] != UINT32_MAX || words[1] || words[2] != object.attributes || words[4] != object.queue_id)
            return false;
        ++count;
        std::printf("INDIRECT_JOINED_RESOURCE address=0x%x owner=0xffffffff waiting=0 state=%u active_operations=0 queue=%u metadata_matches=true\n",
            object.address, object.state, object.queue_id);
    }
    return count == expected_count;
}
}

#ifdef PS3_NATIVE_STARTUP_RECORDER_TEST
enum class RecorderScenario {
    Normal, Alias, CancelAfterOutput, CancelAfterClock, CancelAfterCreate,
    FailToc, CancelPreflightRead, DeadlinePreflightRead, FullStrict,
    InvalidTlsTemplate, CancelAfterTls
};
static const char *recorder_scenario_name(RecorderScenario scenario)
{
    switch (scenario) {
    case RecorderScenario::Normal: return "normal";
    case RecorderScenario::Alias: return "output-toc-alias";
    case RecorderScenario::CancelAfterOutput: return "cancel-after-output";
    case RecorderScenario::CancelAfterClock: return "cancel-after-clock";
    case RecorderScenario::CancelAfterCreate: return "cancel-after-create";
    case RecorderScenario::FailToc: return "fail-toc-write";
    case RecorderScenario::CancelPreflightRead: return "cancel-preflight-read";
    case RecorderScenario::DeadlinePreflightRead: return "deadline-preflight-read";
    case RecorderScenario::FullStrict: return "full-strict";
    case RecorderScenario::InvalidTlsTemplate: return "invalid-tls-template";
    case RecorderScenario::CancelAfterTls: return "cancel-after-real-tls-core";
    }
    std::abort();
}
// Test-only route. Red deliberately bypasses the not-yet-existing admission
// API, using real legacy/lower admission then selecting the observer before
// worker start. Green must compile without this seam and call the real API.
static bool admit_recorder_fixture(StartupExecution &execution,
    PpuMemoryBinding &binding, const Ps3PpuExecutionServices &services,
    PpuMemoryTraceRecorder &recorder)
{
#ifdef PS3_NATIVE_STARTUP_RECORDER_RED
    if (execution.ppu.trace || execution.ppu.recorder || execution.ppu.admitted ||
        !execution.control || !recorder.report().configured || recorder.report().batch_active)
        return false;
    if (!native_startup_admit(execution, binding, services)) return false;
    execution.ppu.trace = nullptr;
    execution.ppu.recorder = &recorder;
    return true;
#else
    return native_startup_admit_recorded(execution, binding, services, recorder);
#endif
}

static int test_startup_recorded_services(const Ps3PpuExecutionServices &services,
    PpuMemoryBinding &binding, TlsProvider &provider, ClockProvider &clock,
    TimebaseMemoryAudit &audit, Oracle &oracle, const ppu_context &initial,
    int32_t priority)
{
    int failures = 0;
    unsigned cases = 0;
    const char *scenario_label = "setup";
#define RECORD_CHECK(value) do { if (!(value)) { std::printf("STARTUP_RECORDER_NATIVE_FAIL case=%u scenario=%s line=%d\n", cases, scenario_label, __LINE__); ++failures; } } while (0)
    const ps3_guest_memory &memory = binding.coherent_memory;
    const uint32_t output = ARGV + 0x100, object = ARGV + 0x200, attributes = ARGV + 0x240;
    const uint64_t toc_slot = STACK_TOP + 0x128;
    uint32_t attribute_bytes[4]{__builtin_bswap32(uint32_t(PS3_LWMUTEX_FIFO)),
        __builtin_bswap32(uint32_t(PS3_LWMUTEX_NONRECURSIVE)), 0, 0};
    RECORD_CHECK(memory.write(memory.context, attributes, attribute_bytes, sizeof(attribute_bytes)) == PS3_GUEST_MEMORY_OK);
    auto read64 = [&](uint32_t address) {
        uint64_t value = 0;
        RECORD_CHECK(memory.read(memory.context, address, &value, sizeof(value)) == PS3_GUEST_MEMORY_OK);
        return __builtin_bswap64(value);
    };
    {
        ++cases;
        scenario_label = "external-active-admission";
        auto control = std::make_unique<StartupControl>();
        auto owner = std::make_unique<StartupExecution>();
        PpuMemoryTraceEvent external_event{};
        PpuMemoryTraceRecorder external;
        RECORD_CHECK(external.configure(&external_event, 1, PpuMemoryTraceRetention::Suffix));
        RECORD_CHECK(external.begin(2));
        owner->control = control.get();
        owner->dispatch_stop = {control.get(), dispatch_stop_requested};
        configure(*control, oracle);
        control->entry = StartupEntry::IndirectCall;
        owner->guest_priority = priority;
        owner->has_guest_priority = true;
        owner->ppu.cpu = initial;
        owner->ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner->ppu.cpu.gpr[2] = 0x78bf78;
        owner->ppu.cpu.gpr[3] = output;
        owner->ppu.cpu.thread_id = 0x11223344;
        owner->ppu.cpu.ctr = 0x55da04;
        const unsigned provider_before = provider.calls + provider.mutex_calls + clock.calls;
        RECORD_CHECK(!admit_recorder_fixture(*owner, binding, services, external));
        RECORD_CHECK(!owner->ppu.admitted && !owner->ppu.recorder && !control->cpu);
        RECORD_CHECK(external.report().batch_active && external.report().remaining_publications == 2);
        RECORD_CHECK(provider.calls + provider.mutex_calls + clock.calls == provider_before);
        external.finish();
    }
    auto fixture = [&](StartupEntry entry, uint64_t target, PpuMemoryTraceRetention retention,
        size_t capacity, RecorderScenario scenario) {
        ++cases;
        scenario_label = recorder_scenario_name(scenario);
        auto control = std::make_unique<StartupControl>();
        auto owner = std::make_unique<StartupExecution>();
        auto events = std::make_unique<PpuMemoryTraceEvent[]>(capacity);
        PpuMemoryTraceRecorder recorder;
        RECORD_CHECK(recorder.configure(events.get(), capacity, retention, 41));
        if (scenario == RecorderScenario::FullStrict) {
            RECORD_CHECK(recorder.begin(1));
            recorder.publish(ppu_memory_scalar_event(PpuMemoryTraceOrigin::Generated,
                false, ARGV, 8, UINT64_C(0xdecafbad01234567)));
            recorder.finish();
        }
        configure(*control, oracle);
        control->entry = entry;
        owner->control = control.get();
        owner->dispatch_stop = {control.get(), dispatch_stop_requested};
        owner->guest_priority = priority;
        owner->has_guest_priority = true;
        owner->ppu.cpu = initial;
        owner->ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        owner->ppu.cpu.gpr[2] = 0x78bf78;
        owner->ppu.cpu.gpr[3] = output;
        owner->ppu.cpu.thread_id = 0x11223344;
        owner->ppu.cpu.ctr = target;
        if (entry == StartupEntry::TlsStub) {
            owner->ppu.cpu.gpr[4] = 0x785e7c;
            owner->ppu.cpu.gpr[5] = 4;
            owner->ppu.cpu.gpr[6] = 0x200;
            if (scenario == RecorderScenario::InvalidTlsTemplate) owner->ppu.cpu.gpr[5] = 0x201;
        }
        if (entry == StartupEntry::MutexCreateStub || entry == StartupEntry::MutexLockStub ||
            entry == StartupEntry::MutexTryStub || entry == StartupEntry::MutexUnlockStub ||
            entry == StartupEntry::MutexDestroyStub) {
            owner->ppu.cpu.gpr[3] = object;
            owner->ppu.cpu.gpr[4] = entry == StartupEntry::MutexCreateStub ? attributes : 0;
        }
        if (entry == StartupEntry::MemorySyscall) owner->ppu.cpu.gpr[11] = target;
        if (scenario == RecorderScenario::Alias) owner->ppu.cpu.gpr[3] = toc_slot;
        RECORD_CHECK(admit_recorder_fixture(*owner, binding, services, recorder));
        if (!owner->ppu.admitted) return;
        // Seed distinct per-case sentinels before snapshots/counters. Prior
        // fixtures cannot make a later missing write appear correct.
        const uint64_t output_sentinel = __builtin_bswap64(UINT64_C(0xa1b2c30000000000) | cases);
        const uint64_t toc_sentinel = __builtin_bswap64(UINT64_C(0xd4e5f60000000000) | cases);
        RECORD_CHECK(memory.write(memory.context, output, &output_sentinel, 8) == PS3_GUEST_MEMORY_OK);
        RECORD_CHECK(memory.write(memory.context, uint32_t(toc_slot), &toc_sentinel, 8) == PS3_GUEST_MEMORY_OK);
        const auto recorded_before = recorder.report();
        PpuMemoryTraceSample sample_before{};
        if (scenario == RecorderScenario::FullStrict) RECORD_CHECK(recorder.read(0, sample_before));
        const ppu_context before = owner->ppu.cpu;
        const uint64_t output_before = read64(output), toc_before = read64(uint32_t(toc_slot));
        const unsigned clock_before = clock.calls, tls_before = provider.calls;
        const Ps3TlsArena tls_arena_before = provider.arena;
        const unsigned tls_core_calls_before = provider.recorder_tls_core_calls;
        unsigned char template_bytes[4]{};
        if (entry == StartupEntry::TlsStub)
            RECORD_CHECK(memory.read(memory.context, 0x785e7c, template_bytes, sizeof(template_bytes)) == PS3_GUEST_MEMORY_OK);
        const unsigned query_before = provider.query_calls;
        const unsigned mutex_before = provider.mutex_calls;
        const uint64_t writes_before = audit.writes.load();
        const unsigned failed_writes_before = audit.failed_write_triggers;
        if (scenario == RecorderScenario::CancelAfterOutput) {
            audit.cancel_after_write = &control->cancelled;
            audit.cancel_write_address = output;
        }
        if (scenario == RecorderScenario::CancelAfterClock) clock.cancel_after_sample = &control->cancelled;
        if (scenario == RecorderScenario::CancelAfterCreate) provider.cancel_after_create = &control->cancelled;
        if (scenario == RecorderScenario::CancelAfterTls) provider.recorder_cancel_after_tls = &control->cancelled;
        if (scenario == RecorderScenario::FailToc) {
            audit.fail_write_enabled = true;
            audit.fail_write_address = uint32_t(toc_slot);
        }
        if (scenario == RecorderScenario::CancelPreflightRead) {
            audit.cancel_after_read = &control->cancelled;
            audit.stop_read_address = output;
        }
        if (scenario == RecorderScenario::DeadlinePreflightRead) {
            audit.deadline_after_read = &control->deadline_tick;
            audit.stop_read_address = uint32_t(toc_slot);
        }
        const bool joined_and_released = run(*owner);
        if (!joined_and_released) {
            // join() itself exits on unconfirmed join. Also retain everything
            // on start/release failure: never clear borrowed pointers or free
            // control/recorder/event storage after any false run result.
            std::printf("STARTUP_RECORDER_NATIVE_FATAL scenario=%s run_failed=true borrowed_state_retained=true\n", scenario_label);
            std::fflush(stdout);
            svcExitProcess();
        }
        audit.cancel_after_write = nullptr;
        audit.fail_write_enabled = false;
        audit.cancel_after_read = nullptr;
        audit.deadline_after_read = nullptr;
        clock.cancel_after_sample = nullptr;
        provider.cancel_after_create = nullptr;
        provider.recorder_cancel_after_tls = nullptr;
        RECORD_CHECK(joined_and_released);
        RECORD_CHECK(!recorder.report().batch_active);
        RECORD_CHECK(provider.active_scopes == 0 && clock.active_scopes == 0);
        RECORD_CHECK(control->journal.count == 0);
        const auto report = recorder.report();
        if (scenario == RecorderScenario::FullStrict) {
            RECORD_CHECK(control->stop == StartupStop::TraceCapacity);
            RECORD_CHECK(audit.writes.load() == writes_before);
            RECORD_CHECK(provider.calls == tls_before && provider.mutex_calls == mutex_before && clock.calls == clock_before);
            RECORD_CHECK(provider.recorder_tls_core_calls == tls_core_calls_before);
            RECORD_CHECK(provider.arena.next == tls_arena_before.next && provider.arena.end == tls_arena_before.end);
            RECORD_CHECK(!std::memcmp(&before, &owner->ppu.cpu, sizeof(before)));
            RECORD_CHECK(read64(output) == output_before && read64(uint32_t(toc_slot)) == toc_before);
            RECORD_CHECK(report.configured == recorded_before.configured && report.capacity == recorded_before.capacity);
            RECORD_CHECK(report.retained_count == recorded_before.retained_count && report.published_count == recorded_before.published_count);
            RECORD_CHECK(report.retained_begin == recorded_before.retained_begin && report.retained_end == recorded_before.retained_end);
            RECORD_CHECK(report.overwritten_begin == recorded_before.overwritten_begin && report.overwritten_end == recorded_before.overwritten_end);
            RECORD_CHECK(report.batch_active == recorded_before.batch_active && report.remaining_publications == recorded_before.remaining_publications);
            PpuMemoryTraceSample sample_after{};
            RECORD_CHECK(recorder.read(0, sample_after));
            RECORD_CHECK(sample_after.sequence == sample_before.sequence && ppu_memory_event_equal(sample_after.event, sample_before.event));
        } else if (retention == PpuMemoryTraceRetention::Strict && capacity == 1 && target == 0x55da04) {
            RECORD_CHECK(control->stop == StartupStop::TraceCapacity);
            RECORD_CHECK(report.published_count == 0);
            RECORD_CHECK(audit.writes.load() == writes_before);
            RECORD_CHECK(read64(output) == output_before && read64(uint32_t(toc_slot)) == toc_before);
            RECORD_CHECK(!std::memcmp(&before, &owner->ppu.cpu, sizeof(before)));
        } else if (scenario == RecorderScenario::CancelPreflightRead || scenario == RecorderScenario::DeadlinePreflightRead) {
            RECORD_CHECK(control->stop == (scenario == RecorderScenario::CancelPreflightRead ? StartupStop::Cancelled : StartupStop::Deadline));
            RECORD_CHECK(report.published_count == 0 && audit.writes.load() == writes_before);
            RECORD_CHECK(!std::memcmp(&before, &owner->ppu.cpu, sizeof(before)));
            RECORD_CHECK(read64(output) == output_before && read64(uint32_t(toc_slot)) == toc_before);
        } else if (scenario == RecorderScenario::InvalidTlsTemplate) {
            RECORD_CHECK(control->stop == StartupStop::ProviderFailure);
            RECORD_CHECK(provider.recorder_tls_report.status != PS3_TLS_OK);
            RECORD_CHECK(provider.recorder_tls_core_calls == tls_core_calls_before + 1);
            RECORD_CHECK(provider.recorder_tls_image.block == 0 && provider.recorder_tls_image.allocation_size == 0 &&
                provider.recorder_tls_image.thread_pointer == 0);
            RECORD_CHECK(report.published_count == 0);
            RECORD_CHECK(!std::memcmp(&before, &owner->ppu.cpu, sizeof(before)));
            RECORD_CHECK(provider.arena.next == tls_arena_before.next && provider.arena.end == tls_arena_before.end);
            RECORD_CHECK(read64(uint32_t(toc_slot)) == toc_before);
            unsigned char unmapped_probe = 0;
            RECORD_CHECK(memory.read(memory.context, TLS_BLOCK, &unmapped_probe, 1) == PS3_GUEST_MEMORY_UNMAPPED);
            std::printf("STARTUP_RECORDER_NATIVE_TLS_FAILURE observed_status=%d owned_extent_live=false\n",
                int(provider.recorder_tls_report.status));
        } else if (scenario == RecorderScenario::CancelAfterTls) {
            RECORD_CHECK(control->stop == StartupStop::Cancelled);
            RECORD_CHECK(report.published_count == 0);
            RECORD_CHECK(!std::memcmp(&before, &owner->ppu.cpu, sizeof(before)));
            RECORD_CHECK(read64(uint32_t(toc_slot)) == toc_before);
        } else if (scenario == RecorderScenario::CancelAfterOutput || scenario == RecorderScenario::FailToc) {
            RECORD_CHECK(control->stop == (scenario == RecorderScenario::CancelAfterOutput ? StartupStop::Cancelled : StartupStop::ProviderFailure));
            RECORD_CHECK(report.published_count == 1);
            RECORD_CHECK(read64(output) == UINT64_C(0x11223344));
            RECORD_CHECK(read64(uint32_t(toc_slot)) == toc_before);
            RECORD_CHECK(owner->ppu.cpu.gpr[3] == before.gpr[3]);
            PpuMemoryTraceSample output_event{};
            RECORD_CHECK(recorder.read(0, output_event));
            RECORD_CHECK(output_event.sequence == 41);
            RECORD_CHECK(output_event.event.origin == PpuMemoryTraceOrigin::Hle);
            RECORD_CHECK(output_event.event.operation == PpuMemoryTraceOperation::Write);
            RECORD_CHECK(output_event.event.address == output && output_event.event.length == 8);
            RECORD_CHECK(output_event.event.value == UINT64_C(0x11223344));
            RECORD_CHECK(output_event.event.status == PS3_GUEST_MEMORY_OK);
            RECORD_CHECK(!output_event.event.stored && !output_event.event.byte_transfer);
            if (scenario == RecorderScenario::FailToc) RECORD_CHECK(audit.failed_write_triggers == failed_writes_before + 1);
        } else if (scenario == RecorderScenario::CancelAfterClock || scenario == RecorderScenario::CancelAfterCreate) {
            RECORD_CHECK(control->stop == StartupStop::Cancelled);
            RECORD_CHECK(report.published_count == 0);
            RECORD_CHECK(owner->ppu.cpu.gpr[3] == before.gpr[3]);
            RECORD_CHECK(read64(uint32_t(toc_slot)) == toc_before);
        } else if (entry == StartupEntry::MemorySyscall) {
            RECORD_CHECK(control->stop == StartupStop::Returned);
            RECORD_CHECK(report.published_count == 0);
            if (target == 147) RECORD_CHECK(owner->ppu.cpu.gpr[3] == UINT64_C(79800000));
            else {
                RECORD_CHECK(owner->ppu.cpu.gpr[3] == 0);
                const unsigned char literal_memory_info[8] = {0x0d, 0x50, 0, 0, 0x0d, 0x50, 0, 0};
                unsigned char actual_memory_info[8]{};
                RECORD_CHECK(memory.read(memory.context, output, actual_memory_info, 8) == PS3_GUEST_MEMORY_OK);
                RECORD_CHECK(!std::memcmp(actual_memory_info, literal_memory_info, 8));
                RECORD_CHECK(provider.query_calls == query_before + 1);
            }
            ppu_context expected = before;
            expected.gpr[3] = target == 147 ? UINT64_C(79800000) : 0;
            RECORD_CHECK(!std::memcmp(&expected, &owner->ppu.cpu, sizeof(expected)));
            RECORD_CHECK(read64(uint32_t(toc_slot)) == toc_before);
        } else {
            RECORD_CHECK(control->stop == StartupStop::Returned);
            const uint64_t publications = target == 0x55da04 ? 2 : 1;
            RECORD_CHECK(report.published_count == publications && report.retained_count == 1);
            RECORD_CHECK(report.retained_begin == 41 + publications - 1 && report.retained_end == 41 + publications);
            PpuMemoryTraceSample last{};
            RECORD_CHECK(recorder.read(0, last));
            RECORD_CHECK(last.sequence == (target == 0x55da04 ? 42 : 41));
            RECORD_CHECK(last.event.origin == PpuMemoryTraceOrigin::Hle);
            RECORD_CHECK(last.event.operation == PpuMemoryTraceOperation::Write);
            RECORD_CHECK(last.event.address == uint32_t(toc_slot) && last.event.length == 8);
            RECORD_CHECK(last.event.value == UINT64_C(0x78bf78));
            RECORD_CHECK(last.event.status == PS3_GUEST_MEMORY_OK);
            RECORD_CHECK(!last.event.stored && !last.event.byte_transfer);
            RECORD_CHECK(read64(uint32_t(toc_slot)) == before.gpr[2]);
            if (target == 0x55da04 && scenario != RecorderScenario::Alias) RECORD_CHECK(read64(output) == UINT64_C(0x11223344));
            if (entry != StartupEntry::ClockStub && entry != StartupEntry::TlsStub)
                RECORD_CHECK(owner->ppu.cpu.gpr[3] == 0);
        }
        if (entry == StartupEntry::ClockStub && scenario != RecorderScenario::FullStrict)
            RECORD_CHECK(clock.calls == clock_before + 1);
        if (entry == StartupEntry::TlsStub && scenario != RecorderScenario::FullStrict) {
            RECORD_CHECK(provider.calls == tls_before + 1);
            if (scenario != RecorderScenario::CancelAfterTls && scenario != RecorderScenario::InvalidTlsTemplate)
                RECORD_CHECK(owner->ppu.cpu.gpr[13] == TLS_BLOCK + 0x7000);
        }
        // Retain uncertainty. r13 alone is not allocation ownership evidence.
        // The real core's captured report/image/cursor, one exclusive call,
        // joined worker and full template/BSS readback must all agree first.
        if (entry == StartupEntry::TlsStub && provider.calls != tls_before &&
            scenario == RecorderScenario::InvalidTlsTemplate) {
            // Do not infer a particular validation enum or transient memory
            // behavior. A cleanup failure or published ownership is uncertain.
            if (provider.recorder_tls_report.status == PS3_TLS_OK ||
                provider.recorder_tls_report.status == PS3_TLS_CLEANUP_FAILURE ||
                provider.arena.next != tls_arena_before.next || provider.arena.end != tls_arena_before.end) {
                std::puts("STARTUP_RECORDER_NATIVE_FATAL invalid_tls_cleanup_uncertain=true resources_retained=true");
                std::fflush(stdout);
                svcExitProcess();
            }
        } else if (entry == StartupEntry::TlsStub && provider.calls != tls_before) {
            const bool owned = joined_and_released && provider.active_scopes == 0 &&
                provider.calls == tls_before + 1 && provider.recorder_tls_core_calls == tls_core_calls_before + 1 &&
                provider.recorder_tls_report.status == PS3_TLS_OK &&
                provider.recorder_tls_image.block == TLS_BLOCK &&
                provider.recorder_tls_image.allocation_size == 0x9000 &&
                provider.recorder_tls_image.thread_pointer == TLS_BLOCK + 0x7000 &&
                tls_arena_before.next == TLS_BLOCK && tls_arena_before.end == TLS_BLOCK + 0x10000 &&
                provider.recorder_tls_before.next == tls_arena_before.next &&
                provider.recorder_tls_before.end == tls_arena_before.end &&
                provider.recorder_tls_after.next == TLS_BLOCK + 0x9000 &&
                provider.recorder_tls_after.end == TLS_BLOCK + 0x10000 &&
                provider.arena.next == provider.recorder_tls_after.next &&
                provider.arena.end == provider.recorder_tls_after.end;
            if (!owned) {
                std::puts("STARTUP_RECORDER_NATIVE_FATAL tls_ownership_uncertain=true resources_retained=true");
                std::fflush(stdout);
                svcExitProcess();
            }
            std::vector<unsigned char> tls_actual(0x9000), tls_expected(0x9000, 0);
            std::memcpy(tls_expected.data(), template_bytes, 4);
            if (memory.read(memory.context, TLS_BLOCK, tls_actual.data(), tls_actual.size()) != PS3_GUEST_MEMORY_OK ||
                tls_actual != tls_expected) {
                std::puts("STARTUP_RECORDER_NATIVE_FATAL tls_template_bss_mismatch=true resources_retained=true");
                std::fflush(stdout);
                svcExitProcess();
            }
            if (memory.decommit(memory.context, TLS_BLOCK, 0x9000) != PS3_GUEST_MEMORY_OK) {
                std::puts("STARTUP_RECORDER_NATIVE_FATAL tls_cleanup_failed=true resources_retained=true");
                std::fflush(stdout);
                svcExitProcess();
            }
            provider.arena = tls_arena_before;
        }
        if (scenario == RecorderScenario::FullStrict && entry == StartupEntry::MutexCreateStub &&
            provider.mutex_calls == mutex_before + 1 && control->stop == StartupStop::Returned &&
            owner->ppu.cpu.gpr[3] == 0) {
            if (destroy_mutex(services.context, object).status != PS3_LWMUTEX_OK) {
                std::puts("STARTUP_RECORDER_NATIVE_FATAL unexpected_create_cleanup_failed=true resources_retained=true");
                std::fflush(stdout);
                svcExitProcess();
            }
        }
        if (scenario != RecorderScenario::FullStrict && (entry == StartupEntry::MutexCreateStub || entry == StartupEntry::MutexLockStub ||
            entry == StartupEntry::MutexTryStub || entry == StartupEntry::MutexUnlockStub ||
            entry == StartupEntry::MutexDestroyStub)) RECORD_CHECK(provider.mutex_calls == mutex_before + 1);
        std::printf("STARTUP_RECORDER_NATIVE_CASE case=%u scenario=%s joined=%d stop=%d published=%llu retained=%zu credit_closed=%d\n",
            cases, scenario_label, joined_and_released, int(control->stop), (unsigned long long)report.published_count,
            report.retained_count, !report.batch_active);
    };
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Alias);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Strict, 1, RecorderScenario::Normal);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::CancelAfterOutput);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::FailToc);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::CancelPreflightRead);
    fixture(StartupEntry::IndirectCall, 0x55da04, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::DeadlinePreflightRead);
    fixture(StartupEntry::ClockStub, 0, PpuMemoryTraceRetention::Strict, 1, RecorderScenario::FullStrict);
    fixture(StartupEntry::TlsStub, 0, PpuMemoryTraceRetention::Strict, 1, RecorderScenario::FullStrict);
    fixture(StartupEntry::MutexCreateStub, 0, PpuMemoryTraceRetention::Strict, 1, RecorderScenario::FullStrict);
    fixture(StartupEntry::ClockStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::ClockStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::CancelAfterClock);
    fixture(StartupEntry::TlsStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::TlsStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::InvalidTlsTemplate);
    fixture(StartupEntry::TlsStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::CancelAfterTls);
    fixture(StartupEntry::MutexCreateStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexLockStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexUnlockStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexTryStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexUnlockStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexDestroyStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MutexCreateStub, 0, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::CancelAfterCreate);
    // Cancelled successful create retains the real object/pin; destroy only
    // after join using the same actual service, without inventing abandonment.
    RECORD_CHECK(destroy_mutex(services.context, object).status == PS3_LWMUTEX_OK);
    fixture(StartupEntry::MemorySyscall, 147, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    fixture(StartupEntry::MemorySyscall, 352, PpuMemoryTraceRetention::Suffix, 1, RecorderScenario::Normal);
    std::printf("STARTUP_RECORDER_NATIVE_%s cases=%u failures=%d actual_table_and_stubs=true scout=false\n",
        failures ? "FAIL" : "PASS", cases, failures);
    return failures;
#undef RECORD_CHECK
}
#endif

#if defined(PS3_NATIVE_STARTUP_RECORDER_TEST) && defined(PS3_NATIVE_STARTUP_BUDGET_TEST)
#error "Choose one private startup test suite"
#endif
#ifdef PS3_NATIVE_STARTUP_BUDGET_TEST
#if !defined(PS3_NATIVE_HEAP_THREAD_ID) || !defined(PS3_NATIVE_THREAD_ID_QUERY) || !defined(PS3_NATIVE_STARTUP_TIMEBASE) || !defined(PS3_NATIVE_INDIRECT_DISPATCH)
#error "Native budget cases require the reviewed frozen 0.20 composition"
#endif
struct NativeBudgetClock {
    ps3_clock real{};
    unsigned calls = 0, fail_at = 0;
    static ps3_clock_status sample(void *opaque, uint64_t *output) {
        auto &self = *static_cast<NativeBudgetClock *>(opaque);
        ++self.calls;
        if (self.calls == self.fail_at) return PS3_CLOCK_ARGUMENT;
        return self.real.now_microseconds(self.real.context, output);
    }
};
struct NativeBudgetExecution {
    std::unique_ptr<StartupControl> control{new StartupControl};
    std::unique_ptr<StartupExecution> owner{new StartupExecution};
    std::vector<StartupAccess> events = std::vector<StartupAccess>(512);
    PpuMemoryTraceRecorder recorder;
    NativeBudgetClock clock;
    ps3_clock operational{&clock, NativeBudgetClock::sample};
    std::unique_ptr<infamous::discovery::DiscoveryBudget> policy;
    std::unique_ptr<DiscoveryBudgetBinding> budget;
    std::vector<infamous::diagnostics::ImportRecord> imports =
        std::vector<infamous::diagnostics::ImportRecord>(64);
    std::vector<infamous::diagnostics::CompletedDispatch> completed =
        std::vector<infamous::diagnostics::CompletedDispatch>(32);
    std::vector<infamous::diagnostics::PendingDispatch> pending =
        std::vector<infamous::diagnostics::PendingDispatch>(8);
    infamous::diagnostics::StartupObservation observation;
};
[[noreturn]] static void native_budget_fatal(const char *reason)
{
    std::printf("NATIVE_BUDGET_FATAL reason=%s borrowed_storage_retained=true ledger_unread_if_unjoined=true\n", reason);
    std::fflush(stdout);
    svcExitProcess();
}
#ifdef PS3_NATIVE_GCM_INIT
static void configure_gcm_init(StartupExecution &owner)
{
    const gcm_init_layout layout = gcm_init_window_layout();
    const gcm_init_status status = startup_gcm_init_attach(owner, &gcm_init_storage, layout);
    if (status != GCM_INIT_OK) native_budget_fatal("gcm-init-attach");
    std::printf("STARTUP_GCM_INIT_CONFIGURATION window=0x%08x size=0x%x context=0x%08x opd=0x%08x callback_code=0x%08x control=0x%08x reports=0x%08x reports_size=0x%x offset_table=0x%08x io_table=0x%08x local_base=0x%08x local_size=0x%08x local_backed=on_demand local_granule=0x%x local_cap=0x%08x intr_thread_created=0 front_end=state_recording decoder_composed=0 game_picture=false\n",
        GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE, GCM_INIT_CONTEXT, GCM_INIT_OPD, GCM_INIT_CALLBACK_CODE,
        GCM_INIT_CONTROL, GCM_INIT_REPORTS, GCM_INIT_REPORTS_SIZE, GCM_INIT_OFFSET_TABLE, GCM_INIT_IO_TABLE,
        GCM_LOCAL_BASE, GCM_LOCAL_SIZE, GCM_LOCAL_GRANULE, GCM_LOCAL_CAP);
}
static void report_gcm_init(const StartupControl &control)
{
    const auto &gcm = control.gcm_init;
    const auto &result = gcm.result;
    std::printf("STARTUP_GCM_INIT_RESULT calls=%zu returns=%zu raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx raw_r6=0x%016llx failure=%u status=%s step=%s walker_status=%u methods=%u shadow_writes=%u noop_actions=%u acquires=%u references=%u put=0x%x get=0x%x ref=0x%x fe_status=%s fe_subchannel=%u fe_method=0x%x fe_data=0x%x fe_io_offset=0x%x committed=%d publications=%zu toc_written=%d has_result=%d\n",
        size_t(control.gcm_init_calls), size_t(control.gcm_init_returns),
        (unsigned long long)gcm.raw_r3, (unsigned long long)gcm.raw_r4,
        (unsigned long long)gcm.raw_r5, (unsigned long long)gcm.raw_r6,
        unsigned(gcm.failure), gcm_init_status_name(result.status), gcm_init_step_name(result.failed_step),
        unsigned(result.walker_status), unsigned(result.methods_accepted), unsigned(result.shadow_writes),
        unsigned(result.noop_actions), unsigned(result.acquires_satisfied), unsigned(result.references_published),
        unsigned(result.final_put), unsigned(result.final_get), unsigned(result.final_ref),
        gcm_fe_status_name(result.fe_status), unsigned(result.fe_subchannel), unsigned(result.fe_method),
        unsigned(result.fe_data), unsigned(result.fe_io_offset), int(result.committed),
        gcm.publications, int(gcm.toc_written), int(gcm.has_result));
}
static void report_gcm_query(const StartupControl &control, const gcm_init_service *service)
{
    const auto &query = control.gcm_query;
    const gcm_init_tile *tile = service ? gcm_init_service_tile(service, 0) : nullptr;
    const gcm_init_zcull *zcull = service ? gcm_init_service_zcull(service, 0) : nullptr;
    const gcm_init_display_buffer *display0 =
        service ? gcm_init_service_display_buffer(service, 0) : nullptr;
    const gcm_init_display_buffer *display1 =
        service ? gcm_init_service_display_buffer(service, 1) : nullptr;
    unsigned tiles_bound = 0;
    for (uint32_t i = 0; service && i < GCM_INIT_TILE_COUNT; i++) {
        const gcm_init_tile *t = gcm_init_service_tile(service, i);
        if (t && t->bound) tiles_bound |= 1u << i;
    }
    std::printf("STARTUP_GCM_QUERY_RESULT config_calls=%zu config_returns=%zu label_calls=%zu label_returns=%zu offset_table_calls=%zu offset_table_returns=%zu control_calls=%zu control_returns=%zu flip_mode_calls=%zu flip_mode_returns=%zu flip_mode=%u last_nid=0x%08x last_lr=0x%llx last_failure=%u last_status=%s last_result=0x%08x publications=%zu toc_written=%d has_result=%d tables_materialized=%d tile_info_calls=%zu tile_info_returns=%zu bind_tile_calls=%zu bind_tile_returns=%zu tile0=%u/0x%x/0x%x/0x%x/%u/%u/%u/%u/%u bind_zcull_calls=%zu bind_zcull_returns=%zu unbind_zcull_calls=%zu unbind_zcull_returns=%zu unbind_tile_calls=%zu unbind_tile_returns=%zu zcull0=0x%x/%u/%u/%u/%u/%u tiles_bound=0x%04x display_buffer_calls=%zu display_buffer_returns=%zu display0=0x%x/0x%x/%u/%u/%u display1=0x%x/0x%x/%u/%u/%u\n",
        size_t(control.gcm_config_calls), size_t(control.gcm_config_returns),
        size_t(control.gcm_label_calls), size_t(control.gcm_label_returns),
        size_t(control.gcm_offset_table_calls), size_t(control.gcm_offset_table_returns),
        size_t(control.gcm_control_calls), size_t(control.gcm_control_returns),
        size_t(control.gcm_flip_mode_calls), size_t(control.gcm_flip_mode_returns),
        service ? service->flip_mode : 0,
        query.nid, (unsigned long long)query.lr, unsigned(query.failure),
        gcm_init_status_name(query.status), query.result, query.publications,
        int(query.toc_written), int(query.has_result),
        service ? service->tables_materialized : 0,
        size_t(control.gcm_tile_info_calls), size_t(control.gcm_tile_info_returns),
        size_t(control.gcm_bind_tile_calls), size_t(control.gcm_bind_tile_returns),
        unsigned(tile ? tile->location : 0), unsigned(tile ? tile->offset : 0),
        unsigned(tile ? tile->size : 0), unsigned(tile ? tile->pitch : 0),
        unsigned(tile ? tile->comp : 0), unsigned(tile ? tile->base : 0),
        unsigned(tile ? tile->bank : 0), unsigned(tile ? tile->set : 0),
        unsigned(tile ? tile->bound : 0),
        size_t(control.gcm_bind_zcull_calls), size_t(control.gcm_bind_zcull_returns),
        size_t(control.gcm_unbind_zcull_calls), size_t(control.gcm_unbind_zcull_returns),
        size_t(control.gcm_unbind_tile_calls), size_t(control.gcm_unbind_tile_returns),
        unsigned(zcull ? zcull->offset : 0), unsigned(zcull ? zcull->width : 0),
        unsigned(zcull ? zcull->height : 0), unsigned(zcull ? zcull->z_format : 0),
        unsigned(zcull ? zcull->set : 0), unsigned(zcull ? zcull->bound : 0),
        tiles_bound,
        size_t(control.gcm_display_buffer_calls), size_t(control.gcm_display_buffer_returns),
        unsigned(display0 ? display0->offset : 0), unsigned(display0 ? display0->pitch : 0),
        unsigned(display0 ? display0->width : 0), unsigned(display0 ? display0->height : 0),
        unsigned(display0 ? display0->set : 0),
        unsigned(display1 ? display1->offset : 0), unsigned(display1 ? display1->pitch : 0),
        unsigned(display1 ? display1->width : 0), unsigned(display1 ? display1->height : 0),
        unsigned(display1 ? display1->set : 0));
}
static void report_gcm_local_memory()
{
    const ps3_demand_window_stats stats = ps3_demand_window_read_stats(&gcm_local_window_storage);
    const char *exhaustion = stats.exhaustion == PS3_DEMAND_WINDOW_CAP ? "cap"
        : stats.exhaustion == PS3_DEMAND_WINDOW_BACKING ? "backing" : "none";
    char journal[PS3_DEMAND_WINDOW_JOURNAL * 11 + 1]{};
    if (!stats.journal_count) {
        std::snprintf(journal, sizeof(journal), "none");
    } else {
        size_t offset = 0;
        for (uint32_t index = 0; index < stats.journal_count && index < PS3_DEMAND_WINDOW_JOURNAL; ++index) {
            if (offset >= sizeof(journal)) break;
            const int written = std::snprintf(journal + offset, sizeof(journal) - offset, "%s0x%x",
                index ? "," : "", stats.journal[index]);
            if (written < 0 || size_t(written) >= sizeof(journal) - offset) break;
            offset += size_t(written);
        }
    }
    std::printf("STARTUP_GCM_LOCAL_MEMORY_RESULT policy=commit_on_demand base=0x%08x size=0x%08x granule=0x%x cap=0x%08x granule_commits=%llu committed_bytes=0x%llx triggering_reads=%llu triggering_writes=%llu triggering_zeros=%llu lowest_offset=0x%x highest_end_offset=0x%x exhaustion=%s exhaustion_offset=0x%x exhaustion_status=%u refused_window_commits=%llu refused_partial=%llu journal=%s\n",
        GCM_LOCAL_BASE, GCM_LOCAL_SIZE, GCM_LOCAL_GRANULE, GCM_LOCAL_CAP,
        (unsigned long long)stats.granule_commits, (unsigned long long)stats.committed_bytes,
        (unsigned long long)stats.triggering_reads, (unsigned long long)stats.triggering_writes,
        (unsigned long long)stats.triggering_zeros,
        stats.lowest_offset, stats.highest_end_offset, exhaustion,
        stats.exhaustion_offset, stats.exhaustion_status,
        (unsigned long long)stats.refused_window_commits, (unsigned long long)stats.refused_partial,
        journal);
}
#endif
static int test_native_budget(const ps3_guest_memory *backing,
    const Ps3PpuExecutionServices &services, PpuMemoryBinding &binding,
    TlsProvider &provider, ClockProvider &clock_provider, const ps3_clock &real_clock,
    Oracle &oracle, const ppu_context &initial, int32_t priority,
    ps3_lwmutex_object *objects,
    ps3_lwmutex_waiter *waiters, ps3_atomic_owner_record *owners, size_t owner_count)
{
    using infamous::discovery::Limits;
    using infamous::discovery::StopReason;
    int failures = 0;
    unsigned workers = 0;
    const char *label = "admission-rollback";
#define BVERIFY(value) do { if (!(value)) { std::printf("NATIVE_BUDGET_FAIL case=%s line=%d\n", label, __LINE__); ++failures; } } while (0)
    const ps3_guest_memory &memory = binding.coherent_memory;
    auto make = [&](Limits limits, StartupEntry entry) {
        auto test = std::make_unique<NativeBudgetExecution>();
        test->clock.real = real_clock;
        uint64_t start_ns = 0;
        if (!discovery_budget_sample_start_ns(real_clock, start_ns) ||
            !test->recorder.configure(test->events.data(), test->events.size(), PpuMemoryTraceRetention::Suffix) ||
            !test->observation.configure(test->imports.data(), test->imports.size(),
                test->completed.data(), test->completed.size(), test->pending.data(), test->pending.size()))
            native_budget_fatal("configuration");
        test->policy = std::make_unique<infamous::discovery::DiscoveryBudget>(limits, start_ns);
        test->budget = std::make_unique<DiscoveryBudgetBinding>(*test->policy, test->operational);
        configure(*test->control, oracle);
        test->control->entry = entry;
        test->owner->control = test->control.get();
        test->owner->dispatch_stop = {test->control.get(), dispatch_stop_requested};
        test->owner->guest_priority = priority;
        test->owner->has_guest_priority = true;
        test->owner->ppu.cpu = initial;
        return test;
    };
    auto admit = [&](NativeBudgetExecution &test, const Ps3PpuExecutionServices &selected) {
        const bool admitted = native_startup_admit_discovery(*test.owner, binding, selected,
            test.recorder, *test.budget, test.observation);
        BVERIFY(test.clock.calls == 0);
        BVERIFY(test.control->cpu == (admitted ? &test.owner->ppu.cpu : nullptr));
        return admitted;
    };
    auto execute = [&](NativeBudgetExecution &test) {
        if (!run(*test.owner)) native_budget_fatal("run-failed");
        ++workers;
        // Every mutable observation below follows confirmed join/release.
        if (test.recorder.report().batch_active || test.recorder.report().remaining_publications)
            native_budget_fatal("open-credit-after-join");
        BVERIFY(test.control->journal.count == 0 && test.owner->ppu.trace == nullptr);
        BVERIFY(test.control->import_count == 0 && test.control->dispatch_count == 0);
        BVERIFY(test.owner->observation == &test.observation);
        BVERIFY(provider.active_scopes == 0 && clock_provider.active_scopes == 0);
        const auto observation = test.observation.report();
        BVERIFY(!observation.immediate_outstanding && observation.pending_capacity == 8);
        BVERIFY(observation.pending_count == test.owner->dispatch.depth);
        std::printf("NATIVE_OBSERVATION_CASE case=%s joined=true imports=%llu completed=%llu started=%llu pending=%zu legacy_imports=%zu legacy_dispatches=%zu\n",
            label, (unsigned long long)observation.imports.total,
            (unsigned long long)observation.completed.total,
            (unsigned long long)observation.dispatch_started, observation.pending_count,
            test.control->import_count, test.control->dispatch_count);
        const auto snapshot = test.policy->snapshot();
        std::printf("NATIVE_BUDGET_CASE case=%s joined=true stop=%d generated=%llu hle=%llu indirect=%llu reason=%d op_clock_calls=%u events=%llu depth=%u credit_closed=true\n",
            label, int(test.control->stop), (unsigned long long)snapshot.generated,
            (unsigned long long)snapshot.hle, (unsigned long long)snapshot.indirect, int(snapshot.stop),
            test.clock.calls, (unsigned long long)test.recorder.report().published_count, test.owner->dispatch.depth);
    };
    {
        auto test = make({64, 8, 8, TIMEOUT_NS}, StartupEntry::IndirectCall);
        Ps3PpuExecutionServices invalid = services;
        invalid.memory = backing;
        std::vector<unsigned char> before(sizeof(*owners) * owner_count);
        std::memcpy(before.data(), owners, before.size());
        if (admit(*test, invalid)) {
            BVERIFY(false);
            native_budget_fatal("unexpected-rollback-admission");
        }
        BVERIFY(!test->owner->ppu.budget && !test->owner->ppu.recorder &&
            !test->owner->observation && !test->control->cpu && test->observation.valid_for_admission());
        BVERIFY(!std::memcmp(before.data(), owners, before.size()));
        const auto snapshot = test->policy->snapshot();
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 0 && snapshot.indirect == 0 &&
            snapshot.stop == StopReason::None && snapshot.last_ns == snapshot.start_ns && test->clock.calls == 0);
    }
    // This is the sole Game invocation, before scratch fixtures alter inputs.
    label = "original-generated-to-hle";
    {
        BVERIFY(oracle.accesses.empty());
        ppu_context expected = initial;
        model_to_tls(expected, oracle);
        oracle.write(expected.gpr[1] + 0x28, expected.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
        expected.gpr[13] = TLS_BLOCK + 0x7000;
        expected.gpr[3] = 0;
        Region tls_region{TLS_BLOCK, std::vector<uint8_t>(0x9000, 0), true};
        const uint8_t *source = oracle.find(0x785e7c, 4);
        if (!source) native_budget_fatal("tls-model-source");
        std::memcpy(tls_region.bytes.data(), source, 4);
        oracle.regions.push_back(std::move(tls_region));
        model_after_tls_to_mutex(expected, oracle);
        uint64_t expected_generated = 0, expected_hle = 0;
        for (const auto &event : oracle.accesses) {
            if (event.origin == PpuMemoryTraceOrigin::Generated) ++expected_generated;
            else ++expected_hle;
        }
        if (!expected_generated || expected_hle != 1 || oracle.accesses.size() > 512)
            native_budget_fatal("early-model-bounds");
        const Ps3TlsArena arena_before = provider.arena;
        const unsigned tls_before = provider.calls, core_before = provider.recorder_tls_core_calls;
        const unsigned mutex_before = provider.mutex_calls;
        auto test = make({expected_generated, 1, 8, TIMEOUT_NS}, StartupEntry::Game);
        if (!admit(*test, services)) native_budget_fatal("game-admission");
        BVERIFY(test->clock.calls == 0);
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == StartupStop::HleSyscallBudget && snapshot.stop == StopReason::HleExhausted);
        BVERIFY(snapshot.generated == expected_generated && snapshot.hle == 1 && snapshot.indirect == 0);
        BVERIFY(test->clock.calls == expected_generated + 3);
        BVERIFY(test->control->tls_returns == 1 && test->control->mutex_returns == 0);
        BVERIFY(test->observation.report().imports.total == 1 && test->observation.report().dispatch_started == 0 &&
            test->observation.report().completed.total == 0 && test->observation.report().pending_count == 0);
        infamous::diagnostics::ImportRecord imported{};
        BVERIFY(test->observation.read_import(0, imported) && imported.sequence == 0 && imported.nid == 0x744680a2);
        BVERIFY(provider.calls == tls_before + 1 && provider.mutex_calls == mutex_before);
        BVERIFY(!std::memcmp(&expected, &test->owner->ppu.cpu, sizeof(expected)));
        BVERIFY(test->recorder.report().published_count == oracle.accesses.size());
        for (size_t index = 0; index < oracle.accesses.size(); ++index) {
            PpuMemoryTraceSample sample{};
            BVERIFY(test->recorder.read(index, sample));
            BVERIFY(sample.sequence == index && ppu_memory_event_equal(sample.event, oracle.accesses[index]));
        }
        const bool memory_matches = compare(&memory, oracle);
        BVERIFY(memory_matches);
        const bool owned_tls = memory_matches && provider.active_scopes == 0 &&
            provider.calls == tls_before + 1 && provider.recorder_tls_core_calls == core_before + 1 &&
            provider.recorder_tls_report.status == PS3_TLS_OK && provider.recorder_tls_image.block == TLS_BLOCK &&
            provider.recorder_tls_image.allocation_size == 0x9000 && provider.recorder_tls_image.thread_pointer == TLS_BLOCK + 0x7000 &&
            arena_before.next == TLS_BLOCK && arena_before.end == TLS_BLOCK + 0x10000 &&
            provider.recorder_tls_before.next == arena_before.next && provider.recorder_tls_before.end == arena_before.end &&
            provider.recorder_tls_after.next == TLS_BLOCK + 0x9000 && provider.recorder_tls_after.end == arena_before.end &&
            provider.arena.next == provider.recorder_tls_after.next && provider.arena.end == provider.recorder_tls_after.end;
        if (!owned_tls) native_budget_fatal("tls-ownership-uncertain");
        if (memory.decommit(memory.context, TLS_BLOCK, 0x9000) != PS3_GUEST_MEMORY_OK)
            native_budget_fatal("tls-decommit");
        provider.arena = arena_before;
        for (size_t index = 0; index < size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY); ++index) BVERIFY(!objects[index].state);
    }
    const uint32_t output = ARGV + 0x100, object = ARGV + 0x200, attributes = ARGV + 0x240;
    const uint32_t slot = STACK_TOP + 0x128;
    auto seed = [&] {
        const uint64_t output_bytes = __builtin_bswap64(UINT64_C(0x1122334455667788));
        const uint64_t toc_bytes = __builtin_bswap64(UINT64_C(0x8877665544332211));
        if (memory.write(memory.context, output, &output_bytes, 8) != PS3_GUEST_MEMORY_OK ||
            memory.write(memory.context, slot, &toc_bytes, 8) != PS3_GUEST_MEMORY_OK)
            native_budget_fatal("scratch-seed");
    };
    auto read64 = [&](uint32_t address) {
        uint64_t value = 0;
        if (memory.read(memory.context, address, &value, 8) != PS3_GUEST_MEMORY_OK)
            native_budget_fatal("scratch-read");
        return __builtin_bswap64(value);
    };
    auto stub = [&](Limits limits, StartupEntry entry) {
        auto test = make(limits, entry);
        test->owner->ppu.cpu.gpr[1] = STACK_TOP + 0x100;
        test->owner->ppu.cpu.gpr[2] = 0x78bf78;
        test->owner->ppu.cpu.gpr[3] = output;
        test->owner->ppu.cpu.thread_id = 0x11223344;
        test->owner->ppu.cpu.ctr = 0x55da04;
        seed();
        return test;
    };
    label = "actual-indirect-success";
    {
        auto test = stub({0, 8, 8, TIMEOUT_NS}, StartupEntry::IndirectCall);
        if (!admit(*test, services)) native_budget_fatal("stub-admission");
        const infamous::diagnostics::DispatchEntry entered{test->owner->ppu.cpu.ctr,
            test->owner->ppu.cpu.lr, test->owner->ppu.cpu.gpr[1], 0};
        ppu_context expected = test->owner->ppu.cpu;
        expected.gpr[2] = 0x78bf78;
        expected.gpr[3] = 0;
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == StartupStop::Returned && snapshot.stop == StopReason::None);
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 1 && snapshot.indirect == 1 && test->clock.calls == 4);
        const auto observed = test->observation.report();
        BVERIFY(observed.imports.total == 1 && observed.dispatch_started == 1 &&
            observed.completed.total == 1 && observed.pending_count == 0 && test->owner->dispatch.depth == 0);
        infamous::diagnostics::ImportRecord imported{};
        BVERIFY(test->observation.read_import(0, imported) && imported.sequence == 0 && imported.nid == 0x350d454e);
        infamous::diagnostics::CompletedDispatch completed{};
        BVERIFY(test->observation.read_completed(0, completed));
        BVERIFY(completed.sequence == 0 && completed.call_id == 0 && completed.parent_id == infamous::diagnostics::NO_PARENT_ID &&
            completed.entry.captured_ctr == entered.captured_ctr && completed.entry.lr == entered.lr && completed.entry.sp == entered.sp &&
            completed.entry.depth == 0 && completed.depth_after == 0 && completed.result.status == uint32_t(PS3_COMPILED_DISPATCH_OK) &&
            completed.result.lookup_attempted && completed.result.lookup_status == uint32_t(PS3_COMPILED_CODE_OK));
        BVERIFY(read64(output) == 0x11223344 && read64(slot) == 0x78bf78 && test->owner->ppu.cpu.gpr[3] == 0);
        BVERIFY(!std::memcmp(&expected, &test->owner->ppu.cpu, sizeof(expected)));
        BVERIFY(test->recorder.report().published_count == 2);
        for (size_t index = 0; index < 2; ++index) {
            PpuMemoryTraceSample sample{};
            BVERIFY(test->recorder.read(index, sample));
            BVERIFY(sample.sequence == index && sample.event.origin == PpuMemoryTraceOrigin::Hle &&
                sample.event.operation == PpuMemoryTraceOperation::Write &&
                sample.event.address == (index ? slot : output) && sample.event.length == 8 &&
                sample.event.value == (index ? UINT64_C(0x78bf78) : UINT64_C(0x11223344)) &&
                sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored && !sample.event.byte_transfer);
        }
        const unsigned char literal_id[8]{0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};
        const unsigned char literal_toc[8]{0, 0, 0, 0, 0, 0x78, 0xbf, 0x78};
        unsigned char actual_id[8]{}, actual_toc[8]{};
        BVERIFY(memory.read(memory.context, output, actual_id, 8) == PS3_GUEST_MEMORY_OK);
        BVERIFY(memory.read(memory.context, slot, actual_toc, 8) == PS3_GUEST_MEMORY_OK);
        BVERIFY(!std::memcmp(actual_id, literal_id, 8) && !std::memcmp(actual_toc, literal_toc, 8));
    }
    for (bool deadline : {false, true}) {
        label = deadline ? "actual-indirect-deadline" : "actual-indirect-exhausted";
        auto test = stub({8, 8, deadline ? UINT64_C(8) : UINT64_C(0), deadline ? UINT64_C(0) : TIMEOUT_NS}, StartupEntry::IndirectCall);
        if (!admit(*test, services)) native_budget_fatal("denied-admission");
        const ppu_context before = test->owner->ppu.cpu;
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == (deadline ? StartupStop::Deadline : StartupStop::IndirectBudget));
        BVERIFY(snapshot.stop == (deadline ? StopReason::Deadline : StopReason::IndirectExhausted));
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 0 && snapshot.indirect == 0 && test->clock.calls == 1);
        BVERIFY(test->control->dispatch_count == 0 && test->owner->dispatch.depth == 0 && test->recorder.report().published_count == 0);
        const auto observed = test->observation.report();
        BVERIFY(observed.imports.total == 0 && observed.dispatch_started == 0 && observed.completed.total == 0 && observed.pending_count == 0);
        BVERIFY(!std::memcmp(&before, &test->owner->ppu.cpu, sizeof(before)));
        BVERIFY(read64(output) == UINT64_C(0x1122334455667788) && read64(slot) == UINT64_C(0x8877665544332211));
    }
    for (bool alias : {false, true}) {
        label = alias ? "actual-syscall-alias" : "actual-syscall147";
        auto test = stub({8, 8, 8, TIMEOUT_NS}, StartupEntry::MemorySyscall);
        test->owner->ppu.cpu.gpr[11] = alias ? UINT64_C(0x100000000) | 147 : 147;
        test->owner->ppu.cpu.gpr[3] = UINT64_C(0xcafebabe11223344);
        if (!admit(*test, services)) native_budget_fatal("syscall-admission");
        ppu_context expected = test->owner->ppu.cpu;
        if (!alias) expected.gpr[3] = UINT64_C(79800000);
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == (alias ? StartupStop::UnsupportedSyscall : StartupStop::Returned));
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 1 && snapshot.indirect == 0 && snapshot.stop == StopReason::None);
        BVERIFY(test->clock.calls == 1 && test->control->syscall_number == 147 && test->recorder.report().published_count == 0);
        const auto observed = test->observation.report();
        BVERIFY(observed.imports.total == 0 && observed.dispatch_started == 0 && observed.completed.total == 0 && observed.pending_count == 0);
        BVERIFY(!std::memcmp(&expected, &test->owner->ppu.cpu, sizeof(expected)));
        BVERIFY(read64(output) == UINT64_C(0x1122334455667788) && read64(slot) == UINT64_C(0x8877665544332211));
    }
    label = "actual-mutex-polls";
    {
        uint32_t bytes[4]{__builtin_bswap32(uint32_t(PS3_LWMUTEX_FIFO)), __builtin_bswap32(uint32_t(PS3_LWMUTEX_NONRECURSIVE)), 0, 0};
        if (memory.zero(memory.context, object, PS3_LWMUTEX_BYTES) != PS3_GUEST_MEMORY_OK ||
            memory.write(memory.context, attributes, bytes, sizeof(bytes)) != PS3_GUEST_MEMORY_OK)
            native_budget_fatal("mutex-seed");
        auto test = stub({0, 8, 8, TIMEOUT_NS}, StartupEntry::MutexCreateStub);
        test->owner->ppu.cpu.gpr[3] = object;
        test->owner->ppu.cpu.gpr[4] = attributes;
        ppu_context expected = test->owner->ppu.cpu;
        expected.gpr[2] = 0x78bf78;
        expected.gpr[3] = 0;
        const unsigned before_calls = provider.mutex_calls;
        if (!admit(*test, services)) native_budget_fatal("mutex-admission");
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == StartupStop::Returned && test->owner->ppu.cpu.gpr[3] == 0);
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 1 && snapshot.indirect == 0 && test->clock.calls == 2);
        BVERIFY(provider.mutex_calls == before_calls + 1 && test->recorder.report().published_count == 1 && read64(slot) == 0x78bf78);
        const auto observed = test->observation.report();
        BVERIFY(observed.imports.total == 1 && observed.dispatch_started == 0 && observed.completed.total == 0 && observed.pending_count == 0);
        infamous::diagnostics::ImportRecord imported{};
        BVERIFY(test->observation.read_import(0, imported) && imported.sequence == 0 && imported.nid == 0x2f85c0ef);
        BVERIFY(!std::memcmp(&expected, &test->owner->ppu.cpu, sizeof(expected)) &&
            !std::memcmp(&expected, &test->control->terminal, sizeof(expected)));
        PpuMemoryTraceSample sample{};
        BVERIFY(test->recorder.read(0, sample));
        BVERIFY(sample.sequence == 0 && sample.event.origin == PpuMemoryTraceOrigin::Hle &&
            sample.event.operation == PpuMemoryTraceOperation::Write && sample.event.address == slot &&
            sample.event.length == 8 && sample.event.value == UINT64_C(0x78bf78) &&
            sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored && !sample.event.byte_transfer);
        if (!inspect_unlocked_resources_after_join(memory, objects, size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY), waiters, size_t(STARTUP_LWMUTEX_WAITER_CAPACITY), 1, true))
            native_budget_fatal("mutex-ownership");
        if (destroy_mutex(services.context, object).status != PS3_LWMUTEX_OK)
            native_budget_fatal("mutex-destroy");
    }
    label = "actual-indirect-partial-poll-stop";
    {
        auto test = stub({0, 8, 8, TIMEOUT_NS}, StartupEntry::IndirectCall);
        test->clock.fail_at = 4;
        if (!admit(*test, services)) native_budget_fatal("partial-admission");
        const infamous::diagnostics::DispatchEntry entered{test->owner->ppu.cpu.ctr,
            test->owner->ppu.cpu.lr, test->owner->ppu.cpu.gpr[1], 0};
        const ppu_context before = test->owner->ppu.cpu;
        execute(*test);
        const auto snapshot = test->policy->snapshot();
        BVERIFY(test->control->stop == StartupStop::BudgetClockFailure && snapshot.stop == StopReason::InvalidClock);
        BVERIFY(snapshot.generated == 0 && snapshot.hle == 1 && snapshot.indirect == 1 && test->clock.calls == 4);
        const auto observed = test->observation.report();
        BVERIFY(observed.imports.total == 1 && observed.dispatch_started == 1 && observed.completed.total == 0 &&
            observed.pending_count == 1 && test->owner->dispatch.depth == 1);
        infamous::diagnostics::ImportRecord imported{};
        BVERIFY(test->observation.read_import(0, imported) && imported.sequence == 0 && imported.nid == 0x350d454e);
        infamous::diagnostics::PendingDispatch pending{};
        BVERIFY(test->observation.read_pending(0, pending));
        BVERIFY(pending.occupied && pending.call_id == 0 && pending.parent_id == infamous::diagnostics::NO_PARENT_ID &&
            pending.entry.captured_ctr == entered.captured_ctr && pending.entry.lr == entered.lr && pending.entry.sp == entered.sp && pending.entry.depth == 0);
        BVERIFY(read64(output) == 0x11223344 && read64(slot) == UINT64_C(0x8877665544332211));
        BVERIFY(!std::memcmp(&before, &test->owner->ppu.cpu, sizeof(before)) && test->recorder.report().published_count == 1);
        BVERIFY(!std::memcmp(&before, &test->control->terminal, sizeof(before)));
        PpuMemoryTraceSample sample{};
        BVERIFY(test->recorder.read(0, sample));
        BVERIFY(sample.sequence == 0 && sample.event.origin == PpuMemoryTraceOrigin::Hle && sample.event.operation == PpuMemoryTraceOperation::Write &&
            sample.event.address == output && sample.event.length == 8 && sample.event.value == 0x11223344 &&
            sample.event.status == PS3_GUEST_MEMORY_OK && !sample.event.stored && !sample.event.byte_transfer);
    }
    BVERIFY(workers == 8);
    for (size_t index = 0; index < size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY); ++index) BVERIFY(!objects[index].state);
    std::printf("NATIVE_BUDGET_%s workers=%u failures=%d indirect_cases_use_actual_table=true indirect_cases=4 direct_syscall_bridge_cases=2 original_game_cases=1 mutex_stub_cases=1 sc_native_reservation_gate=uncovered scout=false\n",
        failures ? "FAIL" : "PASS", workers, failures);
    return failures;
#undef BVERIFY
}
#if defined(PS3_NATIVE_STARTUP_DISCOVERY) && !defined(PS3_NATIVE_STARTUP_LWCOND)
#error Stage27 discovery requires the complete lightweight-condition family
#endif
#if defined(PS3_NATIVE_STARTUP_SPURS_INITIALIZE) && !defined(PS3_NATIVE_STARTUP_DISCOVERY)
#error N12 SPURS composition exists only in the one-shot discovery composition
#endif
struct NativeSysutilCallbackComposition {
    Mutex mutex{};
    ps3_sysutil_callback_registry registry{};
};
struct NativeDiscChangeComposition {
    Mutex mutex{};
    ps3_disc_change_registry registry{};
};
struct NativeSpuResourceComposition {
    Mutex mutex{};
    ps3_spu_resource_policy policy{};
};
extern "C" __attribute__((used,section(".spu_composition_layout"))) const uint64_t spu_composition_layout[] = {
    sizeof(NativeSpuResourceComposition), alignof(NativeSpuResourceComposition),
    offsetof(NativeSpuResourceComposition,mutex), offsetof(NativeSpuResourceComposition,policy)
};
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
// N12: Horizon realisation of the SPURS composition ports. All of it lives in one stable heap object
// that the discovery owner never releases: it must outlive the confirmed joins of every SPURS service
// thread, which native_startup_release_after_join performs before anything else is touched.
struct NativeWwsExecutionContext {
    const ps3_notify_cancel *cancel=nullptr;
};
int native_wws_is_cancelled(void *context) {
    const auto &services=*static_cast<NativeWwsExecutionContext *>(context);
    return services.cancel && ps3_notify_cancel_is_requested(services.cancel);
}
struct NativeSpursComposition {
    static constexpr size_t helper_stack=0x20000, handler_stack=0x20000, kernel_stack=0x40000;
    Mutex lv2_mutex{}, ppu_mutex{};
    Mutex workload_mutex[PS3_SPURS_MAX_INSTANCES]{};
    NativeWwsExecutionContext workload_context[PS3_SPURS_MAX_INSTANCES]{};
    ps3_startup_wws_composition *workload_adapter[PS3_SPURS_MAX_INSTANCES]{};
    ps3_horizon_wait_port wait_port{};
    ps3_horizon_service_thread threads[PS3_SPURS_MAX_INSTANCES][3]{};
    std::unique_ptr<unsigned char[]> local_stores;
    StartupSpursComposition composition{};
    StartupSpursPorts ports{};
};
// N12 FIRST ACTUAL GUEST CHILD (sys_ppu_thread_create NID 0x24a1ea07). ONE stable heap object, allocated BEFORE the parent
// starts and never released (the process exits with it): dedicated Horizon service thread, per-child recorder/budget/clock,
// and the glue (adapters/startup_child_composition) that owns the per-child dispatch-stop latch and the immutable create port.
// Policy values are EXPLICIT; the entry always comes from the guest OPD through the real compiled lookup.
/* Decision 33 / C1b (2026-11-11): the discovery COUNT budgets. Without PS3_NATIVE_LLE_PROFILE they are the values
 * every earlier run used. Under the LLE profile the game really waits for its SPUs by polling libsre
 * (cellSpurs 0x4E153E3E in a loop at 0x41DB90: 9339 calls in a few milliseconds used up the 10000-call budget
 * before any SPU could run its workload). A count limit that ends a correctly running wait is raised; the
 * wall-clock deadline stays the bound on every run. */
#ifdef PS3_NATIVE_LLE_PROFILE
// Decision 90: the counted budgets are not a bound for the game build any more (rv21 ended on generated=4e9 after
// 1100 s at about 20 frames a second); the wall-clock deadline is.
constexpr uint64_t lle_discovery_generated = UINT64_C(4000000000000000), lle_discovery_hle = UINT64_C(1000000000000000),
    lle_discovery_indirect = UINT64_C(1000000000000000);
#else
constexpr uint64_t lle_discovery_generated = 2000000, lle_discovery_hle = 10000, lle_discovery_indirect = 10000;
#endif
struct NativeChildComposition {
    static constexpr size_t host_stack_bytes=0x100000;                 // 1 MiB, explicit (root saves -fstack-usage evidence)
    static constexpr uint32_t kind=4;                                  // ps3_ppu_threads kind is an opaque label; SPURS uses 1 and 2
    static constexpr uint32_t priority=249, creator_priority=250, guest_stack=0x10000;
    static constexpr uint64_t tls_capacity=0x10000;                    // one 0x20000 allocation = stack 0x10000 + TLS region 0x10000
    static constexpr uint64_t return_lr=UINT64_C(0xDEADC0DE);          // unmapped, never code: the prefix is expected to never return
    static constexpr uint32_t stack_entry_slack=0x70, dispatch_limit=4096;   // Decision 89 (was 8)
#ifdef PS3_NATIVE_LLE_PROFILE
    // LLE profile (2026-11-11): the game now runs real work past 10 s (disc archives, SPURS workloads); the run was
    // ended only by this deadline. 45 s is what the emulator harness can observe (it waits 90 s in total).
    static constexpr uint64_t budget_deadline_ns=UINT64_C(1000000000)*STARTUP_DEADLINE_SECONDS;
#else
    static constexpr uint64_t budget_deadline_ns=UINT64_C(10000000000);
#endif
    // THIRD SLOT (decision 10 A): the game's third sys_ppu_thread_create child (func_000F1FE8), which runs to its own
    // sys_ppu_thread_exit. Its real request is priority 2000 / creator 250 / stack 0x4000, so prepare() reserves through the
    // FENCED entry point (the slot's run_to_exit flag) and start() waits, bounded, until the child really exited. The create bridge
    // matches priority + creator + stack only; the port handler comes from the guest OPD, never from this policy.
    static constexpr uint32_t third_slot=2;
    static constexpr uint32_t third_priority=2000, third_creator_priority=250, third_guest_stack=0x4000;
    static constexpr unsigned third_max_exit_polls=5000;
    // Injected wait between exit polls: one host millisecond so the child's own host thread can reach its exit.
    static void exit_wait_poll(void *) { svcSleepThread(INT64_C(1000000)); }
    // THREE-CHILD PROFILE: the slot count is the shared startup capacity profile's STARTUP_CHILD_SLOT_CAPACITY (baseline 1, target 3).
    // Slots are fixed, stable (this object is heap-allocated, never copied/moved/freed) and fully independent: each owns its own
    // host thread, 256-event recorder, budget clock/operational clock, budget policy/binding. Limits and stacks are identical per slot.
    static constexpr unsigned slot_capacity=STARTUP_CHILD_SLOT_CAPACITY;
    static_assert(slot_capacity>=1 && slot_capacity<=STARTUP_CHILD_COMPOSITION_MAX_SLOTS,
        "child slot capacity must be within the composition's fixed slot storage");
    struct Slot {
        ps3_horizon_service_thread thread{};
        std::vector<PpuMemoryTraceEvent> events=std::vector<PpuMemoryTraceEvent>(256);
        PpuMemoryTraceRecorder recorder;
        NativeBudgetClock clock;                                       // OWN counters over the shared immutable real clock
        ps3_clock operational{&clock,NativeBudgetClock::sample};
        std::unique_ptr<infamous::discovery::DiscoveryBudget> policy;
        std::unique_ptr<DiscoveryBudgetBinding> budget;
    };
    Slot slots[slot_capacity];
    StartupChildComposition composition;
    bool resources_ready=false, configured=false;
};
#endif
struct NativeDiagnosticModuleComposition {
    static constexpr size_t catalog_count=13;
    Mutex mutex{};
    ps3_diagnostic_module_state state{};
    ps3_diagnostic_module_slot slots[catalog_count]{};
    const uint16_t catalog[catalog_count]{0x000e,0x000a,0x0017,0x0009,0x0000,0x0014,0x0011,
        0x0015,0x0033,0x0010,0x001c,0xf035,0x0016};
};
struct NativeProcessComposition {
    Mutex mutex{};
    ps3_exitspawn_registry registry{};
};
#ifdef PS3_NATIVE_STARTUP_LWCOND
struct NativeConditionComposition {
    // Condition objects/waiters/bindings and mutex waiters stay at 32 (named in the shared startup capacity profile);
    // only the lwmutex OBJECT table follows the profile (second-instance requirement: 31 ordinary + 2 SPURS mutexes).
    static constexpr size_t object_capacity=STARTUP_LWCOND_OBJECT_CAPACITY, waiter_capacity=STARTUP_LWCOND_WAITER_CAPACITY,
        binding_capacity=STARTUP_LWCOND_BINDING_CAPACITY;
    static constexpr size_t mutex_object_capacity=STARTUP_LWMUTEX_OBJECT_CAPACITY, mutex_waiter_capacity=STARTUP_LWMUTEX_WAITER_CAPACITY;
    ps3_lwmutex_registry mutexes{};
    ps3_lwmutex_object mutex_objects[mutex_object_capacity]{};
    ps3_lwmutex_waiter mutex_waiters[mutex_waiter_capacity]{};
    ps3_horizon_lwmutex_wait wait{};
    ps3_lwcond_registry registry{};
    ps3_lwcond_object objects[object_capacity]{};
    ps3_lwcond_waiter waiters[waiter_capacity]{};
    ps3_lwcond_report cleanup_report{};
    uint32_t admitted_main_id=0;
    bool main_admitted=false;
};
static ps3_lwcond_thread_status condition_thread_exists(void *context,uint32_t id,unsigned *exists) {
    const auto &conditions=*static_cast<const NativeConditionComposition *>(context);
    *exists=conditions.main_admitted && id==conditions.admitted_main_id ? 1u : 0u;
    return PS3_LWCOND_THREAD_OK;
}
#endif
#ifdef PS3_NATIVE_STARTUP_DISCOVERY
[[noreturn]] static void run_startup_discovery(const Ps3PpuExecutionServices &services,
    PpuMemoryBinding &binding, const ps3_clock &real_clock,
    const ppu_context &initial, int32_t priority, NativeConditionComposition &conditions,
    NativeProcessComposition &process, NativeDiagnosticModuleComposition &modules,
    NativeSysutilCallbackComposition &sysutil, NativeSpuResourceComposition &spu, const StartupProcessSdk &process_sdk
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    , ps3_user_memory &allocator, NativeSpursComposition &spurs, NativeChildComposition &child
#endif
    )
{
    using DiscoveryEntryWithProcessSdk=void (*)(const Ps3PpuExecutionServices &,
        PpuMemoryBinding &,const ps3_clock &,const ppu_context &,int32_t,NativeConditionComposition &,
        NativeProcessComposition &,NativeDiagnosticModuleComposition &,NativeSysutilCallbackComposition &,
        NativeSpuResourceComposition &,const StartupProcessSdk &
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
        ,ps3_user_memory &,NativeSpursComposition &,NativeChildComposition &
#endif
        );
    static_assert(std::is_same<decltype(&run_startup_discovery),DiscoveryEntryWithProcessSdk>::value,
        "The process SDK must reach discovery independently of SPURS feature selection");
#ifdef PS3_NATIVE_LLE_PROFILE
    constexpr uint64_t deadline_ns = UINT64_C(1000000000) * STARTUP_DEADLINE_SECONDS;   // see NativeChildComposition::budget_deadline_ns (root 2026-11-11: 45 s -> 120 s, the game is still loading at 45 s)
#else
    constexpr uint64_t deadline_ns = UINT64_C(10000000000);
#endif
#ifdef PS3_NATIVE_LLE_PROFILE
    constexpr int64_t join_ns = INT64_C(1000000000) * (STARTUP_DEADLINE_SECONDS + 2);   // deadline_ns (45 s under the LLE profile) + the same 2 s margin
#else
    constexpr int64_t join_ns = INT64_C(12000000000);
#endif
    auto state = std::make_unique<NativeBudgetExecution>();
    state->owner->process_sdk=process_sdk;
    // D2: ONE Horizon pad process for the whole composition. Static storage because the struct holds an atomic_flag, and
    // created before the primary starts so the game's first cellPadInit already sees the controller and every child
    // guest thread borrows the same pad_state (ports_.process_lv2->pad_process) and the same controller port.
    static pad_port_horizon startup_pad_port_horizon;
    static StartupPadProcess startup_pad_process;
    pad_port_horizon_init(&startup_pad_port_horizon);
    startup_pad_process.port = pad_port_horizon_port(&startup_pad_port_horizon);
    state->owner->pad_process = &startup_pad_process;
#ifdef PS3_NATIVE_LLE_SRE
    // Root (2026-11-11): ONE audio service for the process, borrowed by the primary and every child, plus its ticker.
    // The ticker reads the REAL clock directly (the operational clock counts its calls without a lock); both give the
    // same microsecond values, so PortStart's start time and the ticker agree.
    static StartupAudioProcess startup_audio_process;
    static StartupAudioTicker startup_audio_ticker;
    {
        const audio_port_layout audio_layout{AUDIO_WINDOW, AUDIO_WINDOW_BUFFERS, AUDIO_WINDOW_STRIDE};
        audio_service_init(&startup_audio_process.service, &audio_layout);
    }
    state->owner->audio_process = &startup_audio_process;
    static StartupTimerProcess startup_timer_process;
    lv2_timer_table_init(&startup_timer_process.table, 1);
    state->owner->timer_process = &startup_timer_process;
    startup_audio_ticker.timers = &startup_timer_process;
    static StartupMutexProcess startup_mutex_process;
    state->owner->mutex_process = &startup_mutex_process;
    static StartupDialogProcess startup_dialog_process;
    state->owner->dialog_process = &startup_dialog_process;
    static StartupRsxProcess startup_rsx_process;
#ifdef PS3_NATIVE_GCM_INIT
    startup_rsx_process.gcm = &gcm_init_storage;
    startup_rsx_process.gcm_ready = gcm_init_service_is_initialized;
    startup_rsx_process.hook_user = &startup_rsx_capture;
    startup_rsx_process.method_hook = StartupRsxCapture::method;
    startup_rsx_process.flip_hook = StartupRsxCapture::flip;
    startup_rsx_process.layout.control_address = GCM_INIT_CONTROL;
    startup_rsx_process.layout.label_address = GCM_INIT_REPORTS;
    startup_rsx_process.layout.label_size = GCM_INIT_REPORTS_SIZE;
    startup_rsx_process.layout.report_offset = 0x1400;
    state->owner->rsx_process = &startup_rsx_process;
    startup_audio_ticker.rsx = &startup_rsx_process;
#endif
    startup_audio_ticker.process = &startup_audio_process;
    startup_audio_ticker.memory = &binding.coherent_memory;
    startup_audio_ticker.clock = &real_clock;
    static StartupRsxPumpThread startup_rsx_thread;
#if PS3_NATIVE_RSX_THREAD
    if (startup_audio_ticker.rsx) {   // Decision 117: priority 58 (above the guest threads), prefers core 2, may use 0..2
        startup_rsx_thread.rsx = startup_audio_ticker.rsx;
        startup_rsx_thread.memory = &binding.coherent_memory;
        if (R_SUCCEEDED(threadCreate(&startup_rsx_thread.thread, StartupRsxPumpThread::run, &startup_rsx_thread, nullptr, 0x400000, 0x3a, 2))) {
            uint64_t allowed = 7;
            if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) allowed = 7;
            (void)svcSetThreadCoreMask(startup_rsx_thread.thread.handle, 2, u32(allowed & 7u));
            if (R_SUCCEEDED(threadStart(&startup_rsx_thread.thread))) { startup_rsx_thread.started = true; startup_audio_ticker.rsx_own_thread = true; }
            else threadClose(&startup_rsx_thread.thread);
        }
        std::printf("STARTUP_RSX_THREAD started=%d priority=0x3a core=2\n", int(startup_rsx_thread.started));
    }
#endif
    // Core 1 (an SPU core, never the PPU core 0), one priority step above the SPU workers so a 2 ms tick is not starved.
    if (R_SUCCEEDED(threadCreate(&startup_audio_ticker.thread, StartupAudioTicker::run, &startup_audio_ticker,
            nullptr,
#ifdef PS3_NATIVE_RSX_RENDER
            0x400000,   // the renderer runs on this thread: the driver's shader compiler needs a deep stack
#else
            0x10000,
#endif
            0x2b, 1))) {
        g_hb_ticker_handle = startup_audio_ticker.thread.handle;
        if (R_SUCCEEDED(threadStart(&startup_audio_ticker.thread))) startup_audio_ticker.started = true;
        else threadClose(&startup_audio_ticker.thread);
    }
#ifdef PS3_NATIVE_AUDIO_OUT
    // Root (2026-11-15): real sound. Its own thread (core 2, priority 0x2b) plays what the audio service's running port
    // "plays": it also ticks the service, so the guest's audio clock no longer waits for this ticker thread's renderer
    // frames. A failed audout start prints one STARTUP_AUDIO_OUT line, starts nothing and leaves guest memory alone.
    (void)startup_audio_out_start(startup_audio_process, binding.coherent_memory, real_clock, ps3_flat_guest_fast_pointer);
#endif
    std::printf("STARTUP_AUDIO_TICKER started=%d core=1 priority=0x2b period_us=1000 window=0x%08x size=0x%x\n",
        int(startup_audio_ticker.started), AUDIO_WINDOW, AUDIO_WINDOW_SIZE);
    if (R_SUCCEEDED(threadCreate(&startup_heartbeat.thread, StartupHeartbeat::run, &startup_heartbeat, nullptr, 0x8000, 0x2a, 2))) {
        if (R_SUCCEEDED(threadStart(&startup_heartbeat.thread))) startup_heartbeat.started = true;
        else threadClose(&startup_heartbeat.thread);
    }
#endif
    auto disc_change=std::make_unique<NativeDiscChangeComposition>();
    if (ps3_disc_change_init(&disc_change->registry,{&disc_change->mutex,lock_domain,unlock_domain},
            PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)!=PS3_DISC_CHANGE_OK)
        native_budget_fatal("discovery-disc-change-registry");
    state->owner->disc_change_callbacks=&disc_change->registry;
    std::printf("STARTUP_DISC_CHANGE_POLICY source=native_application_configuration policy=immutable_selected_package media_change_events=none registry=process_shared sony_dgam_transport=false callback_delivery=false\n");
    state->events.resize(1024);
    state->clock.real = real_clock;
    const ps3_calendar_clock calendar = calendar_clock_horizon_port();
    u64 calendar_before = 0, calendar_after = 0;
    const Result calendar_before_status = timeGetCurrentTime(TimeType_Default, &calendar_before);
    ps3_calendar_time calendar_proof{};
    const ps3_calendar_clock_status calendar_proof_status = ps3_calendar_clock_read(&calendar, &calendar_proof);
    const Result calendar_after_status = timeGetCurrentTime(TimeType_Default, &calendar_after);
    std::printf("STARTUP_CALENDAR_PROBE_INPUT before_status=0x%x sample_status=%d after_status=0x%x before=%llu seconds=%lld nanoseconds=%u after=%llu\n",
        unsigned(calendar_before_status), int(calendar_proof_status), unsigned(calendar_after_status),
        (unsigned long long)calendar_before, (long long)calendar_proof.seconds,
        unsigned(calendar_proof.nanoseconds), (unsigned long long)calendar_after);
    if (R_FAILED(calendar_before_status) || R_FAILED(calendar_after_status) ||
        calendar_proof_status != PS3_CALENDAR_CLOCK_OK || calendar_proof.nanoseconds >= UINT32_C(1000000000) ||
        calendar_before > uint64_t(INT64_MAX) || calendar_after > uint64_t(INT64_MAX) ||
        calendar_after < calendar_before || calendar_after - calendar_before > 1 || calendar_proof.seconds < 0)
        native_budget_fatal("discovery-calendar-provider");
    const uint64_t sampled_seconds = uint64_t(calendar_proof.seconds);
    if ((sampled_seconds < calendar_before && calendar_before - sampled_seconds > 1) ||
        (sampled_seconds > calendar_after && sampled_seconds - calendar_after > 1))
        native_budget_fatal("discovery-calendar-anchor-crosscheck");
    std::printf("STARTUP_CALENDAR_PROVIDER status=%d seconds=%lld nanoseconds=%u sdk_clock_realtime=true anchor_crosscheck=true before=%llu after=%llu tolerance_seconds=1\n",
        int(calendar_proof_status), (long long)calendar_proof.seconds, unsigned(calendar_proof.nanoseconds),
        (unsigned long long)calendar_before, (unsigned long long)calendar_after);
    state->owner->boot_content = infamous_boot_content_metadata();
    const auto system_language=system_language_horizon_read();
    state->owner->system_parameters={
        system_language.snapshot,SystemLanguageCompatibility::NativePortLegacyFamilies,
        SystemParameterProfile::NativePortConventions,SystemConfirmButton::Cross,
        SystemDateFormat::YearMonthDay,SystemTimeFormat::TwentyFourHour,0};
    state->owner->system_profile={SystemProfileSource::NativeApplicationProfile,"Player","Nintendo Switch"};
    state->owner->presentation_topology=PresentationTopology::SingleNativeTarget;
    state->owner->presentation_mode=NativePresentationMode::Fixed720p;
    const auto presentation=presentation_inventory(state->owner->presentation_topology);
    std::printf("STARTUP_PRESENTATION_TOPOLOGY source=native_port_configuration policy=single_native_target primary_count=%d secondary_count=%d horizon_monitor_query=false gpu_initialized_by_query=false game_presentation_proven=false\n",
        int(presentation.primary_count),int(presentation.secondary_count));
    const auto presentation_mode=presentation_mode_outcome(state->owner->presentation_mode);
    if (presentation_mode.status!=NativePresentationModeStatus::Configured || presentation_mode.width!=1280 ||
        presentation_mode.height!=720 || presentation_mode.aspect_numerator!=16 || presentation_mode.aspect_denominator!=9)
        native_budget_fatal("presentation-mode-policy");
    std::printf("STARTUP_PRESENTATION_MODE source=native_port_configuration policy=fixed_720p width=1280 height=720 aspect=16:9 logical_render_target=true monitor_attached=false gpu_initialized_by_query=false frame_presented=false\n");
    std::printf("STARTUP_APPLICATION_PROFILE source=native_application_configuration user_label=Player device_label=Nintendo_Switch horizon_account_queried=false device_settings_queried=false identity_authority=false\n");
    std::printf("STARTUP_SYSTEM_LANGUAGE status=%u language=%u stage=%u native_result=0x%08x language_code=0x%016llx horizon_language=%d initialize_succeeded=%d exit_called=%d source=horizon_set_service\n",
        unsigned(system_language.snapshot.status),unsigned(system_language.snapshot.language),unsigned(system_language.stage),
        unsigned(system_language.native_result),(unsigned long long)system_language.language_code,int(system_language.horizon_language),
        int(system_language.initialize_succeeded),int(system_language.exit_called));
    std::printf("STARTUP_SYSTEM_PROFILE source=native_port_conventions confirm=logical_cross date=iso time=24hour ps3_profile_parental_level=0 switch_parental_settings=not_queried legacy_language_policy=native_family_projection\n");
    std::printf("STARTUP_BOOT_CONTENT origin=%u title_id=%.10s parental_level=%u source=verified_selected_disc_package reserved_policy=zero\n",
        unsigned(state->owner->boot_content.origin), state->owner->boot_content.title_id,
        unsigned(state->owner->boot_content.parental_level));
    state->owner->conditions = &conditions.registry;
    state->owner->exitspawn_callbacks = &process.registry;
    state->owner->diagnostic_modules = &modules.state;
    state->owner->sysutil_callbacks = &sysutil.registry;
    state->owner->spu_resources = &spu.policy;
    state->owner->calendar_clock = &calendar;
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
    // Owned by this composition frame through the parent join; no process-global scheduling state.
    // ppu-speed-20261113 (task 3): the guest's own wait loops (ps3_taskwait_spin, 83 pauses per frame in rv56s) slept 1 ms per pause: 18 % of the main thread's
// time, and on average half a pause of latency after the awaited SPU job had finished. Default now 250 us; -DSTARTUP_TASKWAIT_SLEEP_NS=1000000 (lle.mk
// LLE_TASKWAIT_SLEEP_NS) gives the old value back.
#ifndef STARTUP_TASKWAIT_SLEEP_NS
#define STARTUP_TASKWAIT_SLEEP_NS 50000   // Decision 127 (rv65s): was 250000; hand-offs to the SPURS job threads are latency-bound
#endif
StartupTaskwaitHorizonConfig taskwait_config{int64_t(STARTUP_TASKWAIT_SLEEP_NS)};
    state->owner->taskwait = {&taskwait_config, startup_taskwait_horizon_yield_once};
    // Decision 121: every guarded write of guest memory may end a pause early (the pause then re-tests the guest predicate).
    if (startup_taskwait_horizon_wake_enabled()) ps3_atomic_guarded_write_hook = startup_taskwait_horizon_wake;
    std::printf("STARTUP_TASKWAIT_CONFIGURATION sleep_ns=%lld max_sleep_ns=%lld predicate=guest_coherent_reads manual_spurs_kick=false wake_on_guarded_write=%d touched_stores=%d\n",
        (long long)taskwait_config.sleep_ns, (long long)STARTUP_TASKWAIT_MAX_SLEEP_NS, int(startup_taskwait_horizon_wake_enabled()), int(PS3_ATOMIC_TOUCHED_STORES));
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {   // B9BC6207 effective PROCESS sdk: copied from the validated loader plan by the caller, immutable from here; never read from guest memory.
        uint32_t effective_sdk=0;
        const bool effective_ok=startup_spurs_attach_effective_sdk(process_sdk,&effective_sdk);
        std::printf("STARTUP_SPURS_ATTACH_CONFIGURATION game_boot=false source=loader_plan_process_parameters raw_sdk=0x%08x valid=%d effective_ok=%d effective_sdk=0x%08x max_cas_attempts=%u cellspurs_da4_used=false\n",
            unsigned(process_sdk.raw),int(process_sdk.valid),int(effective_ok),unsigned(effective_sdk),unsigned(STARTUP_SPURS_ATTACH_MAX_CAS_ATTEMPTS));
        if(!effective_ok) native_budget_fatal("spurs-attach-process-sdk");
    }
#endif
    const StartupFixedTimezone fixed_timezone{0, 0};
    state->owner->fixed_timezone = &fixed_timezone;
    std::printf("STARTUP_TIMEZONE_CONFIGURATION policy=explicit_virtual_ps3_fixed_utc timezone_minutes=%d summertime_minutes=%d total_minutes=0 source=composition horizon_default_zone_used=false\n",
        int(fixed_timezone.timezone_minutes), int(fixed_timezone.summertime_minutes));
    uint64_t start_ns = 0;
    if (!discovery_budget_sample_start_ns(real_clock, start_ns) ||
        !state->recorder.configure(state->events.data(), state->events.size(), PpuMemoryTraceRetention::Suffix) ||
        !state->observation.configure(state->imports.data(), state->imports.size(),
            state->completed.data(), state->completed.size(), state->pending.data(), state->pending.size()))
        native_budget_fatal("discovery-configuration");
    state->policy = std::make_unique<infamous::discovery::DiscoveryBudget>(
        infamous::discovery::Limits{lle_discovery_generated, lle_discovery_hle, lle_discovery_indirect, deadline_ns}, start_ns);
    std::printf("STARTUP_DISCOVERY_LIMITS generated=%llu hle=%llu indirect=%llu deadline_ns=%llu\n",
        (unsigned long long)lle_discovery_generated, (unsigned long long)lle_discovery_hle,
        (unsigned long long)lle_discovery_indirect, (unsigned long long)deadline_ns);
    state->budget = std::make_unique<DiscoveryBudgetBinding>(*state->policy, state->operational);
    state->control->entry = StartupEntry::Game;
    const uint64_t now_tick = armGetSystemTick(), duration_tick = armNsToTicks(deadline_ns);
    if (duration_tick > UINT64_MAX - now_tick) native_budget_fatal("discovery-deadline-overflow");
    state->control->deadline_tick = now_tick + duration_tick;
    state->owner->control = state->control.get();
    state->owner->dispatch_stop = {state->control.get(), dispatch_stop_requested};
    state->owner->guest_priority = priority;
    state->owner->has_guest_priority = true;
    state->owner->ppu.cpu = initial;
    if (!native_startup_admit_discovery(*state->owner, binding, services,
        state->recorder, *state->budget, state->observation))
        native_budget_fatal("discovery-admission");
#ifdef PS3_NATIVE_GCM_INIT
    configure_gcm_init(*state->owner);
#endif
    if(initial.thread_id!=1 || priority!=250) native_budget_fatal("condition-main-identity");
    conditions.admitted_main_id=initial.thread_id;
    conditions.main_admitted=true;
    std::printf("STARTUP_CONDITION_CONFIGURATION objects=%zu waiters=%zu bindings=%zu domain_pins=%u main_id=%u main_priority=%d separate_guard_wait=true cleanup_on_unknown=retained mutex_objects=%zu mutex_waiters=%zu\n",
        conditions.object_capacity,conditions.waiter_capacity,conditions.binding_capacity,unsigned(STARTUP_ATOMIC_PIN_CAPACITY),
        unsigned(conditions.admitted_main_id),int(priority),conditions.mutex_object_capacity,conditions.mutex_waiter_capacity);
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {   // N12: construct the real runtime graph and inject it (execution.spurs) only after every owner is valid.
        const StartupSpursComposeStatus compose_status=startup_spurs_compose(*state->owner,spurs.composition,
            *binding.domain,allocator,spu.policy,spurs.ports);
        std::printf("STARTUP_SPURS_COMPOSITION module_admission=assumed game_boot=false status=%s ok=%d execution_spurs_set=%d main_thread_id=%u main_id_reserved_in_service_namespace=%d creator_priority=%d instances=%u services_per_instance=3 local_store_slots=%u local_store_bytes=%u charge_records=%d shared_notify_port=true distinct_guards=lv2,ppu,policy,domain policy_configuration=guest_syscall_169_only os_scheduling_guarantee=false\n",
            startup_spurs_compose_status_name(compose_status),int(compose_status==StartupSpursComposeStatus::Ok),
            int(state->owner->spurs!=nullptr),unsigned(initial.thread_id),
            int(ps3_ppu_threads_main_id(&spurs.composition.ppu)==uint32_t(initial.thread_id)),int(priority),
            unsigned(PS3_SPURS_MAX_INSTANCES),unsigned(PS3_SPURS_LS_SLOTS),unsigned(PS3_SPURS_K1_LS_BYTES),int(PS3_STARTUP_SPURS_CHARGE_RECORDS));
        if(compose_status!=StartupSpursComposeStatus::Ok || state->owner->spurs!=&spurs.composition.runtime)
            native_budget_fatal("spurs-composition");
        ps3_instance_guard workload_guards[PS3_SPURS_MAX_INSTANCES]{};
        ps3_spurs_policy_execution_port workload_ports[PS3_SPURS_MAX_INSTANCES]{};
        for(unsigned instance=0;instance<PS3_SPURS_MAX_INSTANCES;++instance) {
            spurs.workload_context[instance].cancel=&spurs.composition.runtime.instances[instance].kernel.cancel;
            const ps3_startup_wws_config workload_config{real_clock,&spurs.workload_context[instance],native_wws_is_cancelled};
            const auto workload_status=ps3_startup_wws_create(&workload_config,&spurs.workload_adapter[instance]);
            if(workload_status!=PS3_SPURS_POLICY_READY) native_budget_fatal("wws-policy-execution-init");
            workload_guards[instance]={&spurs.workload_mutex[instance],lock_domain,unlock_domain};
            workload_ports[instance]=ps3_startup_wws_port(spurs.workload_adapter[instance]);
        }
        const int workload_services=ps3_spurs_runtime_set_workload_services(&spurs.composition.runtime,workload_guards,workload_ports);
        std::printf("STARTUP_WWS_COMPOSITION configured=%d instances=%u image_id=1 entry=0x2bf0 kernel_select=0x290 kernel_exit=0x808 real_clock=true coherent_domain=true transfer_trace=disabled execution_claim=false\n",
            int(workload_services==0),unsigned(PS3_SPURS_MAX_INSTANCES));
        if(workload_services!=0) native_budget_fatal("wws-runtime-services");
    }
#ifdef PS3_NATIVE_GAMEDATA
    // Game data (cellGameDataCheckCreate2): ONE stable object graph for the whole process. It is declared BEFORE the child
    // composition so slot 2's child (which runs the game-data worker) sees the port through the create bridge's request copy.
    // No struct member and no flag-conditional layout: the objects live here, the flag guards only this block.
    static gamedata_storage_posix gamedata_storage_storage;
    static gamedata_check gamedata_storage_state;
    static StartupGameDataPort gamedata_storage_port;
    gamedata_storage_posix &gamedata_storage=gamedata_storage_storage;
    gamedata_check &gamedata_state=gamedata_storage_state;
    StartupGameDataPort &gamedata_port=gamedata_storage_port;
    {
        if(gamedata_storage_posix_init(&gamedata_storage,"sdmc:/switch/infamous-native-spu-stage31/hdd0/game")!=0) {
            std::printf("STARTUP_GAMEDATA_STORAGE init=fail errno=%d\n",gamedata_storage.last_errno);
            std::fflush(stdout);
            native_budget_fatal("gamedata-storage");
        }
        std::printf("STARTUP_GAMEDATA_STORAGE init=ok root=%s\n",gamedata_storage.root);
        gamedata_port.storage=gamedata_storage_posix_port(&gamedata_storage);
        gamedata_port.state=&gamedata_state;
        state->owner->gamedata=&gamedata_port;
    }
#endif
#if defined(PS3_NATIVE_LV2_SYNC) || defined(PS3_NATIVE_LV2_SPU)
    if (!wire_lle_lv2_services(*state->owner)) native_budget_fatal("lle-lv2-services");
#endif
#ifdef PS3_NATIVE_STARTUP_TTY
    // Behaviour block (never a struct member): the process-wide console ring is set before the parent starts, so every
    // sys_tty_write the guest issues during this run appends to it.
    state->owner->tty=&startup_tty_ring;
#endif
#ifdef PS3_NATIVE_STARTUP_FS
    // File system (decision 24): the root is initialised through the POSIX adapter BEFORE the parent starts, then the five
    // mounts are installed against the one root token. A missing root is NOT a failure (fs_storage_posix_init creates nothing
    // and every lookup under a missing directory is ENOENT), so the console keeps booting without an SD card; the print makes
    // that visible instead of silent. A failing mount registration IS fatal: it would leave a guest path unresolvable, which
    // would turn a real purge into an ENOENT storm that looks like success.
    {
        const int root_status=fs_storage_posix_init(&startup_fs_storage,STARTUP_FS_ROOT);
        std::printf("STARTUP_FS_STORAGE init=%s root=%s\n",root_status==0?"ok":"fail",STARTUP_FS_ROOT);
        std::fflush(stdout);
        if(root_status!=0) native_budget_fatal("fs-storage");
        startup_fs_context = StartupFsContext{};
        startup_fs_context.storage=startup_fs_storage;
        startup_fs_context.guard = {&startup_fs_mutex, lock_domain, unlock_domain};   // children share this service
        const fs_storage_port fs_port=fs_storage_posix_port(&startup_fs_context.storage);
        fs_service_init(&startup_fs_context.service,&fs_port);
        for(uint32_t mount_index=0;mount_index<FS_MOUNT_CAPACITY;++mount_index) {
            const char *guest=fs_canonical_guest_mount(mount_index);
            if(!guest) continue;
            if(fs_mount_add(&startup_fs_context.service,guest,STARTUP_FS_ROOT)!=0) {
                std::printf("STARTUP_FS_MOUNT guest=%s status=fail\n",guest);
                std::fflush(stdout);
                native_budget_fatal("fs-mount");
            }
            std::printf("STARTUP_FS_MOUNT guest=%s host=%s status=ok\n",guest,
                startup_fs_context.service.mounts[mount_index].host);
        }
        std::fflush(stdout);
        startup_fs_context.active=true;
        state->owner->fs=&startup_fs_context;
        state->control->fs.active=true;
        state->control->fs.root_ready=true;
    }
#endif
    {   // Guest children: per-slot recorder/budget/clock/stop, then the immutable create port. All BEFORE the parent starts.
        static_assert(int(PS3_PPU_SERVICE_CAP)==int(STARTUP_PPU_SERVICE_CAPACITY),
            "domain PS3_PPU_SERVICE_CAP must equal the shared startup capacity profile");
        static_assert(int(STARTUP_SPURS_SERVICE_RECORD_CAPACITY)==2*int(PS3_SPURS_MAX_INSTANCES) &&
            int(STARTUP_PPU_SERVICE_CAPACITY)==2*int(PS3_SPURS_MAX_INSTANCES)+int(STARTUP_CHILD_SLOT_CAPACITY),
            "service capacity = (helper+handler) per SPURS instance + one record per child slot");
        static_assert(int(STARTUP_ATOMIC_OWNER_CAPACITY)==int(STARTUP_PRIMARY_OWNER_CAPACITY)+int(STARTUP_SPU_OWNER_CAPACITY)+int(STARTUP_CHILD_SLOT_CAPACITY),
            "atomic owner capacity is derived: primary + SPU + one per child slot");
        static_assert(std::extent<decltype(NativeChildComposition::slots)>::value==size_t(STARTUP_CHILD_SLOT_CAPACITY),
            "child slot array is bound to the shared startup capacity profile");
        std::printf("STARTUP_CAPACITY_PROFILE game_boot=false child_slots=%u service_cap=%d atomic_owners=%u primary_owners=%u spu_owners=%u child_owners=%u lwmutex_objects=%u lwmutex_waiters=%u lwcond_objects=%u lwcond_waiters=%u lwcond_bindings=%u atomic_pins=%u source=startup_capacity_policy\n",
            unsigned(STARTUP_CHILD_SLOT_CAPACITY),int(PS3_PPU_SERVICE_CAP),unsigned(STARTUP_ATOMIC_OWNER_CAPACITY),
            unsigned(STARTUP_PRIMARY_OWNER_CAPACITY),unsigned(STARTUP_SPU_OWNER_CAPACITY),unsigned(STARTUP_CHILD_SLOT_CAPACITY),
            unsigned(STARTUP_LWMUTEX_OBJECT_CAPACITY),unsigned(STARTUP_LWMUTEX_WAITER_CAPACITY),unsigned(STARTUP_LWCOND_OBJECT_CAPACITY),
            unsigned(STARTUP_LWCOND_WAITER_CAPACITY),unsigned(STARTUP_LWCOND_BINDING_CAPACITY),unsigned(STARTUP_ATOMIC_PIN_CAPACITY));
        uint64_t child_start_ns[NativeChildComposition::slot_capacity]{};
        StartupChildSlotBinding bindings[NativeChildComposition::slot_capacity]{};
        for(unsigned slot_index=0;slot_index<NativeChildComposition::slot_capacity;++slot_index) {
            auto &slot=child.slots[slot_index];
            slot.clock.real=real_clock;
            if(!discovery_budget_sample_start_ns(real_clock,child_start_ns[slot_index]) ||
                !slot.recorder.configure(slot.events.data(),slot.events.size(),PpuMemoryTraceRetention::Suffix))
                native_budget_fatal("child-resources");
            // Deadline measured from here (<= parent start + a few ms), i.e. never later than prepare + 10 s. A child BLOCKED in
            // the sc130 receive does not poll the wall budget; cancellation sets the control before waking it.
            slot.policy=std::make_unique<infamous::discovery::DiscoveryBudget>(
                infamous::discovery::Limits{lle_discovery_generated,lle_discovery_hle,lle_discovery_indirect,NativeChildComposition::budget_deadline_ns},child_start_ns[slot_index]);
            slot.budget=std::make_unique<DiscoveryBudgetBinding>(*slot.policy,slot.operational);
            bindings[slot_index].thread=ps3_horizon_service_thread_port(&slot.thread);
            bindings[slot_index].recorder=&slot.recorder;
            bindings[slot_index].budget=slot.budget.get();
            StartupChildCompositionPolicy &policy=bindings[slot_index].policy;
            // Known actual second request (native 0.24.11): priority 249, creator 250, stack 0x10000 -> the same policy per slot.
            policy.kind=NativeChildComposition::kind; policy.priority=NativeChildComposition::priority;
            policy.creator_priority=NativeChildComposition::creator_priority; policy.stack_size=NativeChildComposition::guest_stack;
            policy.tls_capacity=NativeChildComposition::tls_capacity; policy.return_lr=NativeChildComposition::return_lr;
            policy.stack_entry_slack=NativeChildComposition::stack_entry_slack;
            if(slot_index!=NativeChildComposition::third_slot) continue;   // slots 0 and 1 keep the policy above exactly
            policy.priority=NativeChildComposition::third_priority;
            policy.creator_priority=NativeChildComposition::third_creator_priority;
            policy.stack_size=NativeChildComposition::third_guest_stack;
            policy.run_to_exit=true;
            policy.exit_wait_context=nullptr; policy.exit_wait_poll=&NativeChildComposition::exit_wait_poll;
            policy.max_exit_polls=NativeChildComposition::third_max_exit_polls;
        }
        child.resources_ready=true;
        StartupChildPpuPorts ports{};                          // SHARED fields only; per-child thread/recorder/budget/dispatch_stop stay empty
        ports.threads=&spurs.composition.ppu;                  // the SAME shared registry the SPURS services use (main adopted)
        ports.queues=&spurs.composition.lv2;                   // the SAME shared LV2 registry
        ports.memory=&binding;                                 // the coherent binding
        ports.code=services.compiled_code;                     // the actual compiled lookup (never a fixture)
        ports.clock=&real_clock;
        ports.dispatch_limit=NativeChildComposition::dispatch_limit;
        // PROCESS-WIDE services (decision 14): the SAME guarded user-memory + lwmutex instance, and the SAME fixed timezone, the
        // primary execution uses. Both are function-scope locals here and outlive the child teardown below (which runs after the
        // parent join, before SPURS/queue/memory teardown). This is what lets the game-data worker's funcStat reach
        // sys_memory_allocate (sc348) and the timezone syscall (sc144) instead of stopping MissingServices.
        ports.process_services=&services;
        ports.fixed_timezone=&fixed_timezone;
        ports.process_lv2=state->owner.get();   // root (B2b): children borrow the primary's lv2 sync/SPU services
        state->owner->ppu_threads=&spurs.composition.ppu;   // sys_process_is_stack: child stacks live in user memory
        const StartupChildCompositionConfigure configured=child.composition.configure(ports,bindings,NativeChildComposition::slot_capacity);
        std::printf("STARTUP_CHILD_COMPOSITION game_boot=false configure=%d slots=%u kind=%u priority=%u creator_priority=%u stack=0x%x tls_capacity=0x%llx stack_entry_slack=0x%x return_lr=0x%llx host_stack=0x%zx dispatch_limit=%u service_cap=%d entry_source=guest_opd_via_compiled_lookup per_slot_events=256 per_slot_limits=generated2000000_hle10000_indirect10000_deadline_ns10000000000 shared_registries=ppu,lv2\n",
            int(configured),unsigned(NativeChildComposition::slot_capacity),NativeChildComposition::kind,NativeChildComposition::priority,
            NativeChildComposition::creator_priority,unsigned(NativeChildComposition::guest_stack),(unsigned long long)NativeChildComposition::tls_capacity,
            unsigned(NativeChildComposition::stack_entry_slack),(unsigned long long)NativeChildComposition::return_lr,
            NativeChildComposition::host_stack_bytes,unsigned(NativeChildComposition::dispatch_limit),int(PS3_PPU_SERVICE_CAP));
        for(unsigned slot_index=0;slot_index<NativeChildComposition::slot_capacity;++slot_index) {
            const StartupChildCompositionPolicy &policy=bindings[slot_index].policy;
            std::printf("STARTUP_CHILD_SLOT_POLICY slot_index=%u priority=%u creator_priority=%u stack=0x%x run_to_exit=%d max_exit_polls=%u\n",
                slot_index,policy.priority,policy.creator_priority,unsigned(policy.stack_size),int(policy.run_to_exit),policy.max_exit_polls);
        }
        if(configured!=StartupChildCompositionConfigure::Ok || !child.composition.create_port() ||
            child.composition.slot_count()!=NativeChildComposition::slot_capacity)
            native_budget_fatal("child-composition-configure");
        child.configured=true;
        // Occupancy BEFORE the parent starts (single-threaded here; ps3_ppu_threads_live reads each record lifetime atomically).
        std::printf("STARTUP_CHILD_OCCUPANCY timing=before_parent_start source=ps3_ppu_threads_live_atomic services_live=%zu service_cap=%d owner_capacity=%u owner_occupancy=not_sampled\n",
            ps3_ppu_threads_live(&spurs.composition.ppu),int(PS3_PPU_SERVICE_CAP),unsigned(STARTUP_ATOMIC_OWNER_CAPACITY));
        state->owner->ppu_create=child.composition.create_port();   // only after SPURS composition AND child configuration succeeded
#ifdef PS3_NATIVE_SPU_RUN
    {   // Root (C1b): sys_spu_thread_group_start runs the group on the native SPU interpreter through this port.
        ps3_spu_run_config spu_run{};
        spu_run.domain = binding.domain;
        spu_run.objects = state->owner->event_queues;
        startup_timer_process.objects = state->owner->event_queues;   // root: lv2 timers deliver into the same registry
        spu_run.groups = state->owner->spu_groups;
        spu_run.clock = real_clock;
        /* Root fix (C1b hardware, 2026-11-11): every host thread used priority 0x2C on the process's default core,
         * and Horizon does not time-slice equal-priority threads on one core. The game waits for its SPUs by
         * spinning on the PPU (cellSpurs 0x4E153E3E at 0x41DB90, no blocking call), so on the console the SPU workers
         * never ran and the wait never ended; the nxlink run only passed because each trace line blocks on a socket.
         * On a PS3 the SPUs are separate processors that a PPU spin cannot starve. The workers therefore go on the
         * two application cores (0..2) that this thread -- which creates the PPU threads on the default core -- is
         * not on, alternating. Same priority; a busy worker yields at every slice boundary (spu_group_runner.c). */
        const int ppu_core = int(svcGetCurrentProcessorNumber());
        uint64_t allowed_cores = 0;
        if (R_FAILED(svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) allowed_cores = 7;
        int spu_cores[2] = {1, 2};
        if (ppu_core == 1) spu_cores[0] = 0;
        if (ppu_core == 2) spu_cores[1] = 0;
        for (unsigned index = 0; index < PS3_LV2_SPU_THREADS_MAX; ++index) {
            ps3_horizon_service_thread_init(&g_spu_run_threads[index], 0x20000);
            /* Root (2026-11-12, Decision 80, after the first console picture): the runner's thread slots are handed
             * out in creation order: 0..4 the five SPURS kernel threads, 5 the sound system's SPU thread, 6 the movie
             * decoder (later groups after it). The decoder is the one that needs a whole core: on the console it got
             * about 19 million instructions a second while it shared cores 1 and 2 with the other six SPU threads and
             * two thirds of the guest PPU threads. Slots 6 and 7 now have core 2 to themselves (PPU threads are
             * spread over cores 0 and 1 only, service_thread_horizon.c); slots 0..5 share core 1. */
            g_spu_run_threads[index].cpu = index >= 6u ? spu_cores[1] : spu_cores[0];
            /* Root (2026-11-13, Decision 108, hardware run rv38s): slots 0..4 are the five SPURS job threads and all
             * five sat on ONE core (spu_cores[0]) while the game's main thread slept waiting for their jobs: each
             * showed 60-70 % "busy", most of it waiting for its turn. They now prefer cores 1, 2, 0, 1, 2 and may run
             * on any of cores 0..2 (service_thread_horizon.c sets the mask for a `wide` thread). The same for the
             * later groups (slot 6 on: the movie decoder). Slot 5 keeps its rule below. */
            if (index < 5u) { static const int prefer[5] = {1, 2, 0, 1, 2}; g_spu_run_threads[index].cpu = prefer[index]; }
            if (index != 5u) g_spu_run_threads[index].wide = 1;
            /* Core 3 is open only to some launches (a forwarder that runs the NRO as an application with four
             * cores; the homebrew menu gives three). When the process may use it, the sound system's SPU thread
             * (slot 5: 2 billion instructions in the first console run, with no sound output to feed) moves there. */
            if (index == 5u && (allowed_cores & 8u)) g_spu_run_threads[index].cpu = 3;
            /* Decision 137 (rv71s, SPU agent's measurement): cores 0..2 carried ~1,665 of 1,980 core-seconds (core 2 ~108 %: two job
             * threads + RSX + Bink) while core 3 held only the audio SPU (60 %). With core 3 allowed, the five job threads prefer cores
             * 1, 1, 0, 2, 3 and may use cores 0..3; the audio SPU (slot 5, core 3) rises to 0x3a so a job thread there never delays it.
             * -DSTARTUP_SPU_OLD_LAYOUT keeps the old layout. */
#ifndef STARTUP_SPU_OLD_LAYOUT
            if (allowed_cores & 8u) {
                if (index < 5u) { static const int prefer4[5] = {1, 1, 0, 2, 3}; g_spu_run_threads[index].cpu = prefer4[index]; g_spu_run_threads[index].wide = 2; }
                if (index == 5u) g_spu_run_threads[index].priority = 0x3a;
            }
#endif
            g_spu_run_ports[index] = ps3_horizon_service_thread_port(&g_spu_run_threads[index]);
        }
        if (ps3_spu_neon_selftest) {   // Root (Decision 101): NEON SPU helpers against their scalar references, on this CPU
            static char report[16384];
            const uint64_t began = armGetSystemTick();
            const int mismatching = ps3_spu_neon_selftest(report, sizeof report);
            std::printf("%s", report);
            std::printf("STARTUP_NEON_SELFTEST mismatching_ops=%d milliseconds=%llu\n", mismatching,
                (unsigned long long)(armTicksToNs(armGetSystemTick() - began) / 1000000u));
            char row[120];
            std::snprintf(row, sizeof row, mismatching ? "SPU SELF-TEST: %d operations WRONG (tell Claude)" : "SPU self-test: all operations correct", mismatching);
            native_status_row(5, row);
        }
        std::printf("STARTUP_SPU_RUN_CORES ppu_core=%d spu_cores=%d,%d priority=0x%x fallback=default_core_if_refused late_groups_own_core=1 allowed_core_mask=0x%llx sound_spu_core=%d\n",
            ppu_core, spu_cores[0], spu_cores[1], g_spu_run_threads[0].priority, static_cast<unsigned long long>(allowed_cores), g_spu_run_threads[5].cpu);
        // Decision 127 (rv65s): the main thread waits on the SPURS job threads (STARTUP_TASKWAIT_WAKERS t1..t5) while those threads are 56 %
        // idle: the hand-offs are latency-bound. An idle SPU (RdEventStat wait) re-polls after at most this many microseconds (was the
        // runner default 200). -DSTARTUP_SPU_PAUSE_US=200 gives the old value back.
#ifndef STARTUP_SPU_PAUSE_US
#define STARTUP_SPU_PAUSE_US 50
#endif
        spu_run.pause_sleep_microseconds = STARTUP_SPU_PAUSE_US;
        spu_run.threads = g_spu_run_ports;
        spu_run.thread_count = PS3_LV2_SPU_THREADS_MAX;
        spu_run.cancel_flag = &g_spu_run_cancel;
        for (size_t index = 0; index < binding.ram_count && index < PS3_SPU_RUN_MAX_RANGES; ++index) {
            spu_run.range_base[index] = binding.ram[index].address;
            spu_run.range_length[index] = uint32_t(binding.ram[index].length);
            ++spu_run.range_count;
        }
        spu_run.trace = startup_live_trace_line;
        const uint32_t configured = ps3_spu_run_configure(&spu_run);
        std::printf("STARTUP_SPU_RUN_CONFIGURE status=0x%08x threads=%u ranges=%u objects=%d groups=%d\n", configured,
            spu_run.thread_count, spu_run.range_count, int(spu_run.objects != nullptr), int(spu_run.groups != nullptr));
        std::fflush(stdout);
    }
#endif
    }
#endif
    std::puts("STARTUP_DISCOVERY_BEGIN module_admission=assumed game_boot=false original_game_entries=1 memory_suffix=1024 imports_suffix=64 completed_suffix=32 pending=8 generated_limit=2000000 hle_syscall_limit=10000 indirect_limit=10000 deadline_ns=10000000000 join_ns=12000000000 depth_limit=8 heap_budget_source=LAUNCH_MEMORY logical_backing_pages=131072 user_quota_bytes=223346688");
    std::fflush(stdout);
    Thread worker;
    if (!start(*state->owner, worker)) native_budget_fatal("discovery-start");
    Result waited = svcWaitSynchronizationSingle(worker.handle, join_ns);
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    if (R_FAILED(waited)) {
        // Deadline watchdog: ONLY prepublished atomics (primary control->cancelled + child dispatch-stop + create-skip latch). NEVER a child owner method
        // (cancel/status/reap) while the parent may still be inside prepare/start. One more finite grace wait, then the
        // existing fail-closed exit retaining everything.
        state->control->cancelled.store(true, std::memory_order_release);   // PRIMARY cancel: prepublished atomic, no other parent state is read
        __atomic_store_n(&g_ps3_ppu_prefix_cancel, 1u, __ATOMIC_SEQ_CST);   // root: a creator waiting for its new thread leaves that wait
#ifdef PS3_NATIVE_SPU_RUN
        __atomic_store_n(&g_spu_run_cancel, 1u, __ATOMIC_RELEASE);   // root (C1b): a PPU blocked in SPU join converges
#endif
        child.composition.request_stop();
        std::printf("STARTUP_CHILD_WATCHDOG parent_wait_result=0x%x primary_cancel_published=true child_latches_published=true owner_methods_called=false grace_ns=60000000000\n", waited);
        std::fflush(stdout);
        waited = svcWaitSynchronizationSingle(worker.handle, INT64_C(60000000000));   // root 2026-11-11: 5 s -> 60 s; the primary can be inside a long pure-compute loop (0xBBF68..), which only sees the cancel at its next import
    }
#endif
    if (R_FAILED(waited)) {
        // No mutable guest/CPU/policy/recorder/observer reads on this branch.
        std::printf("STARTUP_DISCOVERY_UNJOINED result=0x%x borrowed_graph_retained=true ledgers_unread=true\n", waited);
        std::fflush(stdout);
        {
            // Root (2026-11-11) DIAGNOSTIC, deliberately racy: the primary did not leave within the grace wait and the
            // process is about to exit, so its control block and registers are read WITHOUT synchronisation to name
            // where it is. The values may be torn; they are a hint, never a result. Also sent to the debug log
            // (Eden's eden_log.txt) because the guest log lost its last lines on this exit path.
            char line[384];
            const StartupControl &racy = *state->control;
            const ppu_context &racy_cpu = state->owner->ppu.cpu;
            std::snprintf(line, sizeof line,
                "STARTUP_UNJOINED_RACY syscall=%u lv2_sync=%u lv2_sync_arg=0x%x lv2_sync_blocked=0x%llx lv2_sync_calls=%zu "
                "lv2_sync_returns=%zu lv2_spu=0x%x cia=0x%llx lr=0x%llx r1=0x%llx r3=0x%llx r11=0x%llx ctr=0x%llx create_stage=%u ppu_start_stage=%u hle_calls=%zu\n",
                unsigned(racy.syscall_number), unsigned(racy.lv2_sync_number), unsigned(racy.lv2_sync_argument),
                (unsigned long long)racy.lv2_sync_blocked, racy.lv2_sync_calls, racy.lv2_sync_returns,
                unsigned(racy.lv2_spu_number), (unsigned long long)racy_cpu.cia, (unsigned long long)racy_cpu.lr,
                (unsigned long long)racy_cpu.gpr[1], (unsigned long long)racy_cpu.gpr[3],
                (unsigned long long)racy_cpu.gpr[11], (unsigned long long)racy_cpu.ctr,
                unsigned(g_startup_create_stage), unsigned(g_ps3_ppu_start_stage), size_t(racy.import_count));
            std::fputs(line, stdout);
            std::fflush(stdout);
            svcOutputDebugString(line, std::strlen(line));
            svcSleepThread(INT64_C(200000000));   // let the file system service take the flushed bytes before exit
        }
        svcExitProcess();
    }
    std::printf("STARTUP_TEARDOWN step=primary_wait_done result=0x%x\n", waited); std::fflush(stdout);
    if (R_FAILED(threadWaitForExit(&worker)) || R_FAILED(threadClose(&worker)))
        native_budget_fatal("discovery-join-finalization");
    std::printf("STARTUP_TEARDOWN step=primary_joined\n"); std::fflush(stdout);
#ifdef PS3_NATIVE_LLE_SRE
    // Root: the audio ticker borrows guest memory: stop and join it before any teardown. Bounded: a ticker that does
    // not leave within 3 s is reported and left (never closed), so a stuck tick cannot hide the whole report.
    if (startup_heartbeat.started) {
        startup_heartbeat.stop.store(true, std::memory_order_release);
        if (R_SUCCEEDED(svcWaitSynchronizationSingle(startup_heartbeat.thread.handle, INT64_C(2000000000))) &&
            R_SUCCEEDED(threadWaitForExit(&startup_heartbeat.thread)) && R_SUCCEEDED(threadClose(&startup_heartbeat.thread)))
            startup_heartbeat.started = false;
    }
#ifdef PS3_NATIVE_AUDIO_OUT
    startup_audio_out_stop();   // the sound thread reads the guest ring and borrows the service: join it first (bounded 2 s)
#endif
    if (startup_rsx_thread.started) {   // Decision 117: the RSX consumer's own thread, before the ticker (same order as before)
        startup_rsx_thread.stop.store(true, std::memory_order_release);
        const Result rsx_wait = svcWaitSynchronizationSingle(startup_rsx_thread.thread.handle, INT64_C(3000000000));
        if (R_SUCCEEDED(rsx_wait)) {
            if (R_FAILED(threadWaitForExit(&startup_rsx_thread.thread)) || R_FAILED(threadClose(&startup_rsx_thread.thread)))
                native_budget_fatal("rsx-thread-join");
            startup_rsx_thread.started = false;
        }
        std::printf("STARTUP_TEARDOWN step=rsx_thread_stopped wait_result=0x%x joined=%d\n", rsx_wait, int(!startup_rsx_thread.started));
    }
    if (startup_audio_ticker.started) {
        startup_audio_ticker.stop.store(true, std::memory_order_release);
        const Result ticker_wait = svcWaitSynchronizationSingle(startup_audio_ticker.thread.handle, INT64_C(3000000000));
        if (R_SUCCEEDED(ticker_wait)) {
            if (R_FAILED(threadWaitForExit(&startup_audio_ticker.thread)) || R_FAILED(threadClose(&startup_audio_ticker.thread)))
                native_budget_fatal("audio-ticker-join");
            startup_audio_ticker.started = false;
        }
        std::printf("STARTUP_TEARDOWN step=ticker_stopped wait_result=0x%x joined=%d\n", ticker_wait, int(!startup_audio_ticker.started));
        std::fflush(stdout);
    }
#endif
#ifdef PS3_NATIVE_SPU_RUN
    // Root (C1b): the SPU workers borrow guest memory and lv2 objects: cancel and join them before any teardown.
    __atomic_store_n(&g_spu_run_cancel, 1u, __ATOMIC_RELEASE);
    std::printf("STARTUP_TEARDOWN step=spu_shutdown_begin\n"); std::fflush(stdout);
    ps3_spu_run_shutdown();
    std::printf("STARTUP_TEARDOWN step=spu_shutdown_done\n"); std::fflush(stdout);
    ps3_spu_run_report(spu_run_print);
#ifdef PS3_SPU_IMAGE_ACCOUNT
    ps3_spu_images_report(spu_run_print, 0u);   // Decision 104: the whole run, once more at teardown (seconds=0)
#endif
    std::fflush(stdout);
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {   // PARENT JOINED: single-threaded owner. The child is cancelled and reaped BEFORE any SPURS/queue/memory teardown or report.
        struct ChildPoll { static void sleep(void *) { svcSleepThread(INT64_C(10000000)); } };
        StartupChildCompositionTeardownPolicy teardown_policy{};
        teardown_policy.poll=&ChildPoll::sleep; teardown_policy.max_reap_polls=200; teardown_policy.max_stage_retries=3;
        (void)child.composition.teardown(teardown_policy);   // slot 0's legacy report is re-read per slot below
        const auto &totals=child.composition.totals();
        const auto &refusal=totals.last_refusal;
        const char *refusal_reason=refusal.reason==StartupChildRefusalReason::NoSlot ? "no_slot" :
            refusal.reason==StartupChildRefusalReason::PolicyMismatch ? "policy_mismatch" : "none";
        const StartupControl &pc=*state->control;
        std::printf("STARTUP_CHILD_COMPOSITION_TOTALS slots=%u prepare_calls=%u start_calls=%u refused_no_slot=%u policy_mismatch=%u refused_stopped=%u refused_invalid=%u unmatched_start=%u\n",
            totals.slot_count,totals.prepare_calls,totals.start_calls,totals.refused_no_slot,totals.policy_mismatch,
            totals.refused_stopped,totals.refused_invalid,totals.unmatched_start);
        std::printf("STARTUP_CHILD_LAST_REFUSAL reason=%s free_slots=%u priority=%d creator_priority=%d stack=0x%08x output=0x%08x entry=0x%08x argument=0x%llx\n",
            refusal_reason,refusal.free_slots,int(refusal.priority),int(refusal.creator_priority),unsigned(refusal.stack_size),
            unsigned(refusal.output),unsigned(refusal.entry),(unsigned long long)refusal.argument);
        // lv2 syscall 44 (sys_ppu_thread_join) on the PRIMARY thread: every call, the one that returned, and the last result.
        std::printf("STARTUP_PPU_JOIN_CALLS calls=%zu returns=%zu has_result=%d last_thread=0x%llx last_status=%d last_exit_value=0x%llx\n",
            pc.ppu_join_calls,pc.ppu_join_returns,int(pc.ppu_join_has_result),(unsigned long long)pc.ppu_join_last_thread,
            int(pc.ppu_join_last_status),(unsigned long long)pc.ppu_join_last_exit_value);
        std::printf("STARTUP_CHILD_PPU_CREATE_CALLS calls=%zu prepared=%zu id_written=%zu started=%zu returns=%zu guest_errors=%zu terminal=%zu has_prepare=%d prepare=%d has_start=%d start=%d last_entry=0x%08x last_toc=0x%08x last_output=0x%08x last_thread_id=0x%x last_priority=%d last_stack=0x%x last_argument=0x%llx last_flags=0x%llx last_cell=0x%x\n",
            pc.ppu_create_calls,pc.ppu_create_prepared,pc.ppu_create_id_written,pc.ppu_create_started,pc.ppu_create_returns,
            pc.ppu_create_guest_errors,pc.ppu_create_terminal,int(pc.ppu_create_has_prepare),int(pc.ppu_create_prepare),
            int(pc.ppu_create_has_start),int(pc.ppu_create_start),unsigned(pc.ppu_create_last_entry),unsigned(pc.ppu_create_last_toc),
            unsigned(pc.ppu_create_last_output),unsigned(pc.ppu_create_last_thread_id),int(pc.ppu_create_last_priority),
            unsigned(pc.ppu_create_last_stack),(unsigned long long)pc.ppu_create_last_argument,(unsigned long long)pc.ppu_create_last_flags,
            unsigned(pc.ppu_create_last_cell));
        // PER-SLOT lines: every slot prints its own index, ids, before/after state and first cancel/reap sequence. Mutable child state is read
        // only here: the parent is joined and teardown() ran (cancel-all then bounded reap-all) on this single owner thread.
        unsigned cancel_seq_max=0, reap_seq_min=0, ids_distinct_slots=0, prepared_slots=0;
        bool ids_all_distinct=true;
        for(unsigned slot_index=0;slot_index<child.composition.slot_count();++slot_index) {
            const StartupChildCompositionReport &r=slot_index==0 ? child.composition.report() : child.composition.report(slot_index);   // report() = slot 0 legacy contract (refreshes the watchdog flag)
            const StartupChildPpuStatus &b=r.before, &a=r.after;
            bool distinct_id=r.prepared_id!=0;
            for(unsigned other=0;other<child.composition.slot_count();++other)
                if(other!=slot_index && r.prepared_id!=0 && child.composition.report(other).prepared_id==r.prepared_id) distinct_id=false;
            if(r.prepare_attempted) { ++prepared_slots; if(distinct_id) ++ids_distinct_slots; else ids_all_distinct=false; }
            if(r.cancel_sequence>cancel_seq_max) cancel_seq_max=r.cancel_sequence;
            if(r.first_reap_sequence!=0 && (reap_seq_min==0 || r.first_reap_sequence<reap_seq_min)) reap_seq_min=r.first_reap_sequence;
            std::printf("STARTUP_CHILD_PPU_RESULT slot_index=%u game_boot=false guest_execution_claim=false configured=%d prepare_calls=%u prepare_attempted=%d prepare_status=%d prepare_native_rc=0x%x mapped_prepare=%d prepared_id=0x%x start_calls=%u start_attempted=%d start_skipped_by_latch=%d start_status=%d start_native_rc=0x%x start_stop=%u mapped_start=%d observed_blocked=%d watchdog_stop=%d distinct_id=%d first_cancel_sequence=%u first_reap_sequence=%u\n",
                slot_index,int(r.configured),r.prepare_calls,int(r.prepare_attempted),int(r.prepare.status),unsigned(r.prepare.native_rc),int(r.mapped_prepare),
                unsigned(r.prepared_id),r.start_calls,int(r.start_attempted),int(r.start_skipped_by_cancel_latch),int(r.start.status),
                unsigned(r.start.native_rc),unsigned(r.start.stop),int(r.mapped_start),int(r.observed_blocked),int(r.stop_requested_by_watchdog),int(distinct_id),r.cancel_sequence,r.first_reap_sequence);
            std::printf("STARTUP_CHILD_PPU_BEFORE_CANCEL slot_index=%u phase=%d has_resources=%d service_owned=%d admitted=%d host_started=%d id=0x%x storage_allocation=0x%08x storage_bytes=0x%llx stack=0x%08x stack_bytes=0x%llx tls_region=0x%08x tls_capacity=0x%llx tls_block=0x%08x thread_pointer=0x%08x receive_calls=%u last_queue=0x%x last_pc=0x%llx last_native_rc=0x%x fault=%d terminal_calls=%u stop=%u consumed_events=%u cancel_requested=%d first_cancel_sequence=%u first_reap_sequence=%u\n",
                slot_index,int(b.phase),int(b.has_resources),int(b.service_owned),int(b.admitted),int(b.host_started),unsigned(b.ref.id),
                unsigned(b.storage.allocation_address),(unsigned long long)b.storage.allocation_bytes,unsigned(b.storage.stack_address),
                (unsigned long long)b.storage.stack_region_bytes,unsigned(b.storage.tls_address),(unsigned long long)b.storage.tls_capacity_bytes,
                unsigned(b.tls.block),unsigned(b.tls.thread_pointer),b.receive_calls,unsigned(b.last_queue),(unsigned long long)b.last_pc,
                unsigned(b.last_native_rc),int(b.fault),b.terminal_calls,unsigned(b.stop),b.consumed_events,int(b.cancel_requested),r.cancel_sequence,r.first_reap_sequence);
            std::printf("STARTUP_CHILD_PPU_TEARDOWN slot_index=%u teardown=%s latches_asserted=%d reap_status=%d reap_service_rc=0x%x reap_memory_status=%d reap_calls=%u still_running_polls=%u stage_retries=%u after_phase=%d after_has_resources=%d terminal_calls=%u stop=%u exit_code_is_startup_stop_not_guest_join_value=%u receive_calls=%u last_queue=0x%x last_pc=0x%llx fault=%d consumed_events=%u first_consumed_data1=0x%llx first_consumed_data2=0x%llx first_consumed_data3=0x%llx discarded_event=%d cancel_requested=%d first_cancel_sequence=%u first_reap_sequence=%u\n",
                slot_index,startup_child_teardown_name(r.teardown),int(r.latches_asserted_by_teardown),int(r.reap.status),unsigned(r.reap.service_rc),
                int(r.reap.memory_status),r.reap_calls,r.still_running_polls,r.stage_retries,int(a.phase),int(a.has_resources),a.terminal_calls,
                unsigned(a.stop),unsigned(a.exit_code),a.receive_calls,unsigned(a.last_queue),(unsigned long long)a.last_pc,int(a.fault),
                a.consumed_events,(unsigned long long)a.first_consumed.data1,(unsigned long long)a.first_consumed.data2,
                (unsigned long long)a.first_consumed.data3,int(a.discarded_event),int(a.cancel_requested),r.cancel_sequence,r.first_reap_sequence);
            // READ-ONLY child diagnostics (decision 14): valid now because teardown() reaped this slot (or it never reserved anything).
            // Unconditional: the terminal register set and counters say what the child finally did, whatever the feature selection.
            if(const StartupChildPpu *prepared=child.composition.child(slot_index)) {
                const StartupControl &k=prepared->control();
                const size_t n_imports=k.import_count>STARTUP_IMPORT_CAPACITY ? STARTUP_IMPORT_CAPACITY : k.import_count;
                char last_imports[9*8+1]={}; size_t used=0;
                for(size_t i=n_imports>8?n_imports-8:0;i<n_imports;++i)
                    used+=size_t(std::snprintf(last_imports+used,sizeof(last_imports)-used,"%s0x%08x",i==(n_imports>8?n_imports-8:0)?"":",",unsigned(k.imports[i])));
                std::printf("STARTUP_CHILD_TERMINAL slot_index=%u stop=%u cia=0x%llx lr=0x%llx r1=0x%llx r3=0x%llx r11=0x%llx syscall=%u imports=%zu last_imports=%s timezone_calls=%zu memory_status=%d\n",
                    slot_index,unsigned(k.stop),(unsigned long long)k.terminal.cia,(unsigned long long)k.terminal.lr,
                    (unsigned long long)k.terminal.gpr[1],(unsigned long long)k.terminal.gpr[3],(unsigned long long)k.terminal.gpr[11],
                    unsigned(k.syscall_number),k.import_count,last_imports[0]?last_imports:"none",k.timezone_calls,int(k.memory_result.status));
#ifdef PS3_NATIVE_GAMEDATA
                std::printf("STARTUP_CHILD_GAMEDATA slot_index=%u gamedata_check_calls=%zu gamedata_check_returns=%zu callback_invoked=%d dispatch_returned=%d dispatch_status=%d status=%d result=%d action=%d gamedata_size_calls=%zu gamedata_size_returns=%zu guest_thread_exit_calls=%zu has_guest_exit=%d guest_exit_value=0x%llx\n",
                    slot_index,k.gamedata_check_calls,k.gamedata_check_returns,int(k.gamedata_check.callback_invoked),
                    int(k.gamedata_check.dispatch_returned),int(k.gamedata_check.dispatch_status),int(k.gamedata_check.status),
                    int(k.gamedata_check.result),int(k.gamedata_check.action.action),k.gamedata_size_calls,k.gamedata_size_returns,
                    k.guest_thread_exit_calls,int(k.has_guest_exit),(unsigned long long)k.guest_exit_value);
#endif
            }
            // lv2 syscall 44 (sys_ppu_thread_join) routing evidence, one line per slot, unconditional.
            std::printf("STARTUP_CHILD_JOIN slot_index=%u join_calls=%u joined=%d\n",slot_index,r.join_calls,int(r.joined));
        }
#ifdef PS3_NATIVE_GAMEDATA
        {   // what cellGameDataCheckCreate2 saw and did, plus slot 2 (the game-data worker) exit evidence
            const StartupChildCompositionReport &gd=child.composition.report(NativeChildComposition::third_slot);
            const StartupChildPpuStatus &after=gd.after;
            std::printf("STARTUP_GAMEDATA_RESULT dir=%s is_new=%d size_kb=%d free_kb=%d calls_free=%u calls_stat=%u calls_read=%u calls_create=%u calls_write=%u last_status=%d last_errno=%d slot2_observed_exited=%d slot2_exit_polls=%u slot2_final_status=%d slot2_final_stop=%u\n",
                gamedata_state.dir_name,int(gamedata_state.is_new),int(gamedata_state.size_kb),int(gamedata_state.free_kb),
                gamedata_storage.calls_free,gamedata_storage.calls_stat,gamedata_storage.calls_read,gamedata_storage.calls_create,
                gamedata_storage.calls_write,int(gamedata_storage.last_status),gamedata_storage.last_errno,
                int(gd.observed_exited),gd.exit_polls,int(gd.final_start.status),unsigned(after.stop));
        }
#endif
        std::printf("STARTUP_CHILD_SLOT_AGGREGATE slots=%u prepared_slots=%u distinct_id_slots=%u all_ids_distinct=%d max_first_cancel_sequence=%u min_first_reap_sequence=%u all_cancels_before_first_reap=%d aggregate_released=%d timing=post_parent_join_post_teardown\n",
            child.composition.slot_count(),prepared_slots,ids_distinct_slots,int(ids_all_distinct),cancel_seq_max,reap_seq_min,
            int(reap_seq_min==0 || cancel_seq_max<reap_seq_min),int(child.composition.released()));
        // Service registry occupancy AFTER the parent join and the aggregate teardown, via atomic reads of each record lifetime.
        // Atomic-domain owner records are plain, domain-guarded storage: no safe occupancy snapshot exists, so only capacity is printed.
        std::printf("STARTUP_CHILD_OCCUPANCY timing=after_parent_join_after_child_teardown source=ps3_ppu_threads_live_atomic services_live=%zu service_cap=%d owner_capacity=%u owner_occupancy=not_sampled\n",
            ps3_ppu_threads_live(&spurs.composition.ppu),int(PS3_PPU_SERVICE_CAP),unsigned(STARTUP_ATOMIC_OWNER_CAPACITY));
        std::fflush(stdout);
        if(!child.composition.released()) {   // retained: no destructor, no free, no SPURS/queue/memory teardown
            std::puts("STARTUP_CHILD_PPU_TEARDOWN_FAILED child_not_released=true nothing_released=true borrowed_graph_retained=true");
            std::fflush(stdout);
            svcExitProcess();
        }
    }
#endif
#ifdef PS3_NATIVE_STARTUP_TASKWAIT
    {
        const auto &wait = state->owner->taskwait_observation;
        std::printf("STARTUP_TASKWAIT_JOINED begins=%zu spins=%zu ends=%zu pauses_attempted=%zu pauses_completed=%zu depth=%u max_depth=%u failure=%u unfinished_scope=%d guest_completion_claim=false\n",
            wait.begins, wait.spins, wait.ends, wait.pauses_attempted, wait.pauses_completed,
            unsigned(wait.depth), unsigned(wait.max_depth), unsigned(wait.failure), int(wait.depth != 0));
    }
#endif
#ifdef PS3_NATIVE_STARTUP_TTY
    {   // lv2 syscall 403 (sys_tty_write) console sink. Read only after the confirmed join; the ring is the composition's own
        // static, so `used` bounds every print below. The content is split at '\n' into at most 64 lines.
        const StartupControl &tty_control = *state->control;
        std::printf("STARTUP_TTY_SUMMARY calls=%zu returns=%zu bytes=%zu dropped=%zu last_channel=%u\n",
            tty_control.tty_write_calls, tty_control.tty_write_returns, tty_control.tty_bytes,
            startup_tty_ring.dropped, unsigned(tty_control.tty_last_channel));
        unsigned line_index = 0;
        size_t cursor = 0;
        while (cursor < startup_tty_ring.used && line_index < 64) {
            size_t end = cursor;
            while (end < startup_tty_ring.used && startup_tty_ring.bytes[end] != '\n') ++end;
            std::printf("STARTUP_TTY line=%u text=", line_index);
            for (size_t index = cursor; index < end; ++index) std::putchar(startup_tty_ring.bytes[index]);
            std::putchar('\n');
            ++line_index;
            cursor = end + 1;   // skips the '\n' (or lands past `used` when the last line has no terminator)
        }
        std::printf("STARTUP_TTY_LINES lines=%u truncated=%d capacity=%zu\n", line_index,
            int(cursor < startup_tty_ring.used), size_t(STARTUP_TTY_RING_BYTES));
    }
#endif
#ifdef PS3_NATIVE_STARTUP_PAD
    {   // cellPadInit / cellPadSetPortSetting. A non-zero pad result is NOT a
        // native stop: func_00154194 silently returns on it, so `requests` is the only evidence the game skipped its pad block.
        const StartupControl &pad_control = *state->control;
        std::printf("STARTUP_PAD_INIT calls=%zu returns=%zu requests=%zu last=0x%08x status=%u count=%u max_pads=%u attached=%u initialized=%d second_call=%d\n",
            pad_control.pad_init_calls, pad_control.pad_init_returns, pad_control.pad_init_requests,
            unsigned(uint32_t(pad_control.pad_init.result)), unsigned(pad_control.pad_init.status),
            unsigned(pad_control.pad_init.count), unsigned(pad_control.pad_init.state.max_pads),
            unsigned(pad_control.pad_init.state.attached_pads), int(pad_control.pad_init.initialized),
            int(pad_control.pad_init.second_call));
        std::printf("STARTUP_PAD_PORT_SETTING calls=%zu returns=%zu last=0x%08x status=%u port=%u setting=%u applied=%d field_a=%u field_b=%u record_offset=0x%x syscall=%u subop=%u replayed=%d\n",
            pad_control.pad_set_port_setting_calls, pad_control.pad_set_port_setting_returns,
            unsigned(uint32_t(pad_control.pad_set_port_setting.result)),
            unsigned(pad_control.pad_set_port_setting.status), unsigned(pad_control.pad_set_port_setting.port_no),
            unsigned(pad_control.pad_set_port_setting.setting), int(pad_control.pad_set_port_setting.applied),
            unsigned(pad_control.pad_set_port_setting.detail.field_a),
            unsigned(pad_control.pad_set_port_setting.detail.field_b),
            unsigned(pad_control.pad_set_port_setting.detail.record_offset),
            unsigned(pad_control.pad_set_port_setting.detail.syscall_number),
            unsigned(pad_control.pad_set_port_setting.detail.syscall_subop),
            int(pad_control.pad_set_port_setting.syscall_replayed));
        std::printf("STARTUP_PAD_PROCESS info2_calls=%llu data_calls=%llu data_changed=%llu act_calls=%llu host_reads=%llu max_pads=%u initialized=%u\n",
            (unsigned long long)startup_pad_process.info2_calls, (unsigned long long)startup_pad_process.data_calls,
            (unsigned long long)startup_pad_process.data_changed, (unsigned long long)startup_pad_process.act_calls,
            (unsigned long long)startup_pad_port_horizon.reads, unsigned(startup_pad_process.state.max_pads),
            unsigned(startup_pad_process.state.initialized));
#ifdef PS3_NATIVE_LLE_SRE
        {
            const audio_service &audio = startup_audio_process.service;
            uint64_t calls_out = 0, calls_port = 0;
            unsigned open_ports = 0, running_ports = 0;
            for (unsigned i = 0; i < AUDIO_IMPORT_COUNT; ++i) (i <= AUDIO_IMPORT_OUT_CONFIGURE ? calls_out : calls_port) += audio.calls[i];
            for (unsigned i = 0; i < AUDIO_PORT_COUNT; ++i) { open_ports += audio.ports[i].open != 0; running_ports += audio.ports[i].running != 0; }
            std::printf("STARTUP_AUDIO initialized=%u calls_out=%llu calls_port=%llu open_ports=%u running_ports=%u ticks=%llu index_stores=%llu store_failures=%llu clock_failures=%llu counter0=%llu channels0=%u blocks0=%u\n",
                unsigned(audio.initialized), (unsigned long long)calls_out, (unsigned long long)calls_port, open_ports, running_ports,
                (unsigned long long)startup_audio_process.ticks, (unsigned long long)startup_audio_process.index_stores,
                (unsigned long long)startup_audio_process.store_failures, (unsigned long long)startup_audio_ticker.clock_failures,
                (unsigned long long)audio.ports[0].counter, unsigned(audio.ports[0].channels), unsigned(audio.ports[0].blocks));
            unsigned live_timers = 0, running_timers = 0;
            for (unsigned i = 0; i < LV2_TIMER_CAPACITY; ++i) {
                live_timers += startup_timer_process.table.timers[i].in_use != 0;
                running_timers += startup_timer_process.table.timers[i].running != 0;
            }
            std::printf("STARTUP_LV2_TIMER objects=%d creates=%llu starts=%llu events=%llu fires=%llu send_failures=%llu last_send_code=0x%llx live=%u running=%u period0_us=%llu queue0=0x%x\n",
                int(startup_timer_process.objects != nullptr), (unsigned long long)startup_timer_process.table.creates,
                (unsigned long long)startup_timer_process.table.starts, (unsigned long long)startup_timer_process.table.events,
                (unsigned long long)startup_timer_process.fires, (unsigned long long)startup_timer_process.send_failures,
                (unsigned long long)startup_timer_process.last_send_code, live_timers, running_timers,
                (unsigned long long)startup_timer_process.table.timers[0].period_us, unsigned(startup_timer_process.table.timers[0].queue_id));
            std::printf("STARTUP_PPU_PREFIX_WAIT waits=%llu slices_50ms=%llu max_slices=%llu over_1s=%llu over_10s=%llu cancel=%u released_after_250ms=%llu\n",
                (unsigned long long)g_ps3_ppu_prefix_waits, (unsigned long long)g_ps3_ppu_prefix_slices,
                (unsigned long long)g_ps3_ppu_prefix_max_slices, (unsigned long long)g_ps3_ppu_prefix_over_1s,
                (unsigned long long)g_ps3_ppu_prefix_over_10s, unsigned(g_ps3_ppu_prefix_cancel), (unsigned long long)g_ps3_ppu_prefix_released);
            {
                const rsx_live_fifo &rsx = startup_rsx_process.fifo;
                std::printf("STARTUP_RSX_LIVE configured=%d status=%u pumps=%llu words=%llu packets=%llu methods=%llu jumps=%llu calls=%llu references=%llu last_reference=0x%x label_writes=%llu acquires=%llu acquire_waits=%llu device_semaphore_ops=%llu reports=%llu draws=%llu clears=%llu flips=%llu last_flip_buffer=%u get=0x%x put=0x%x fault=%u fault_reason=%u fault_offset=0x%x fault_word=0x%x\n",
                    int(startup_rsx_process.configured), unsigned(startup_rsx_process.last_status), (unsigned long long)startup_rsx_process.pumps,
                    (unsigned long long)rsx.words, (unsigned long long)rsx.packets, (unsigned long long)rsx.methods, (unsigned long long)rsx.jumps,
                    (unsigned long long)rsx.calls, (unsigned long long)rsx.references, unsigned(rsx.last_reference), (unsigned long long)rsx.label_writes,
                    (unsigned long long)rsx.acquires, (unsigned long long)rsx.acquire_waits, (unsigned long long)rsx.device_semaphore_ops,
                    (unsigned long long)rsx.reports, (unsigned long long)rsx.draws, (unsigned long long)rsx.clears, (unsigned long long)rsx.flips,
                    unsigned(rsx.last_flip_buffer), unsigned(rsx.get), unsigned(rsx.last_put), unsigned(rsx.fault), unsigned(rsx.fault_reason),
                    unsigned(rsx.fault_offset), unsigned(rsx.fault_word));
#ifdef PS3_NATIVE_RSX_RENDER
                {
                    const rsx_render_bridge_stats render = rsx_render_bridge_get_stats();
                    std::printf("STARTUP_RSX_RENDER state=%d present_ready=%d methods=%llu flips=%llu presented=%llu reports=%llu reads_refused=%llu reads_uncommitted=%llu\n",
                        render.state, render.present_ready, (unsigned long long)render.methods, (unsigned long long)render.flips,
                        (unsigned long long)render.presented, (unsigned long long)render.reports,
                        (unsigned long long)render.reads_refused, (unsigned long long)render.reads_uncommitted);
                }
#endif
                std::printf("STARTUP_FAST_MEMORY enabled=%d repeated_stores=%llu\n", int(g_ppu_fast_memory),
                    static_cast<unsigned long long>(g_ppu_fast_write_conflicts.load()));
                std::printf("STARTUP_WRITE_WATCH_SUMMARY base=0x%x size=0x%x entries=%u dropped=%u\n",
                    g_ppu_write_watch_base, g_ppu_write_watch_size, g_ppu_write_watch_count, g_ppu_write_watch_dropped);
                for (uint32_t watch_index = 0; watch_index < g_ppu_write_watch_count; ++watch_index) {
                    const PpuWriteWatchEntry &entry = g_ppu_write_watch[watch_index];
                    char last_hex[33], first_hex[33];
                    for (unsigned byte = 0; byte < 16; ++byte) {
                        std::snprintf(last_hex + byte * 2, 3, "%02x", entry.bytes[byte]);
                        std::snprintf(first_hex + byte * 2, 3, "%02x", entry.first_bytes[byte]);
                    }
                    std::printf("STARTUP_WRITE_WATCH address=0x%x length=%u lr=0x%x r1=0x%x count=%u first=%s last=%s\n",
                        entry.address, entry.length, entry.lr, entry.r1, entry.count, first_hex, last_hex);
                }
                std::printf("STARTUP_RSX_CAPTURE at_flip=%u flips=%u done=%d overflow=%d dumped_ranges=%llu dumped_bytes=%llu skipped_granules=%llu write_errors=%d\n",
                    unsigned(startup_rsx_capture.at_flip), unsigned(startup_rsx_capture.flips), int(startup_rsx_capture.done), int(startup_rsx_capture.overflow),
                    (unsigned long long)startup_rsx_capture.dumped_ranges, (unsigned long long)startup_rsx_capture.dumped_bytes,
                    (unsigned long long)startup_rsx_capture.skipped_granules, startup_rsx_capture.write_errors);
                std::printf("STARTUP_RSX_GCM flip_commands=%llu flip_immediate=%llu handler_sets=%llu address_queries=%llu debug_level=%u flip_handler=0x%x vblank_handler=0x%x user_handler=0x%x graphics_handler=0x%x queue_handler=0x%x subchannel_methods=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                    (unsigned long long)startup_rsx_process.flip_commands, (unsigned long long)startup_rsx_process.flip_immediate,
                    (unsigned long long)startup_rsx_process.handler_sets, (unsigned long long)startup_rsx_process.address_queries,
                    unsigned(startup_rsx_process.debug_level), unsigned(startup_rsx_process.handlers[0]), unsigned(startup_rsx_process.handlers[1]),
                    unsigned(startup_rsx_process.handlers[2]), unsigned(startup_rsx_process.handlers[3]), unsigned(startup_rsx_process.handlers[4]),
                    (unsigned long long)rsx.methods_by_subchannel[0], (unsigned long long)rsx.methods_by_subchannel[1], (unsigned long long)rsx.methods_by_subchannel[2],
                    (unsigned long long)rsx.methods_by_subchannel[3], (unsigned long long)rsx.methods_by_subchannel[4], (unsigned long long)rsx.methods_by_subchannel[5],
                    (unsigned long long)rsx.methods_by_subchannel[6], (unsigned long long)rsx.methods_by_subchannel[7]);
            }
            {
                // Root diagnostic: the primary's registers when it ended (it was last seen inside a float loop at 0xBBF68..).
                const ppu_context &pc = state->owner->ppu.cpu;
                std::printf("STARTUP_PRIMARY_REGS lr=0x%llx r1=0x%llx r3=0x%llx r8=0x%llx r9=0x%llx r11=0x%llx r28=0x%llx r29=0x%llx r30=0x%llx r31=0x%llx ctr=0x%llx f0=%g f1=%g f12=%g f13=%g f21=%g f23=%g f24=%g f25=%g f28=%g f29=%g f30=%g f31=%g\n",
                    (unsigned long long)pc.lr, (unsigned long long)pc.gpr[1], (unsigned long long)pc.gpr[3], (unsigned long long)pc.gpr[8],
                    (unsigned long long)pc.gpr[9], (unsigned long long)pc.gpr[11], (unsigned long long)pc.gpr[28], (unsigned long long)pc.gpr[29],
                    (unsigned long long)pc.gpr[30], (unsigned long long)pc.gpr[31], (unsigned long long)pc.ctr,
                    pc.fpr[0], pc.fpr[1], pc.fpr[12], pc.fpr[13], pc.fpr[21], pc.fpr[23], pc.fpr[24], pc.fpr[25], pc.fpr[28], pc.fpr[29], pc.fpr[30], pc.fpr[31]);
            }
            std::printf("STARTUP_MSG_DIALOG opens=%llu closes=%llu aborts=%llu callbacks=%llu check_calls=%llu busy=%llu open=%d pending=%d last_type=0x%x last_text=\"%s\"\n",
                (unsigned long long)startup_dialog_process.opens, (unsigned long long)startup_dialog_process.closes,
                (unsigned long long)startup_dialog_process.aborts, (unsigned long long)startup_dialog_process.callbacks,
                (unsigned long long)startup_dialog_process.check_calls, (unsigned long long)startup_dialog_process.busy,
                int(startup_dialog_process.open), int(startup_dialog_process.result_pending),
                unsigned(startup_dialog_process.last_type), startup_dialog_process.last_text);
            std::printf("STARTUP_BINK_HANDOFF calls=%llu releases=%llu relock_failures=%llu first_cond=0x%x first_lr=0x%x first_thread=0x%x mutexes=0x%x:0x%x,0x%x:0x%x,0x%x:0x%x\n",
                (unsigned long long)startup_mutex_process.bink_calls,
                (unsigned long long)startup_mutex_process.bink_handoffs, (unsigned long long)startup_mutex_process.bink_relock_failures,
                startup_mutex_process.bink_first_cond, startup_mutex_process.bink_first_lr, startup_mutex_process.bink_first_thread,
                startup_mutex_process.bink_first_ids[0], startup_mutex_process.bink_first_owners[0],
                startup_mutex_process.bink_first_ids[1], startup_mutex_process.bink_first_owners[1],
                startup_mutex_process.bink_first_ids[2], startup_mutex_process.bink_first_owners[2]);
            startup_print_missing_services();
            std::printf("STARTUP_LV2_MUTEX_STOP site=%u id=0x%x thread=0x%x\n", g_lv2_mutex_stop_site, g_lv2_mutex_stop_id, g_lv2_mutex_stop_thread);
            for (const auto &entry : startup_mutex_process.entries)
                if (entry.in_use) std::printf("STARTUP_LV2_MUTEX_ENTRY id=0x%x owner=0x%x count=%u recursive=%d\n", entry.id, entry.owner, entry.count, int(entry.recursive));
            std::printf("STARTUP_LV2_MUTEX creates=%llu destroys=%llu locks=%llu contended=%llu unlocks=%llu errors=%llu live=%u peak=%u capacity=%u\n",
                (unsigned long long)startup_mutex_process.creates, (unsigned long long)startup_mutex_process.destroys,
                (unsigned long long)startup_mutex_process.locks, (unsigned long long)startup_mutex_process.contended,
                (unsigned long long)startup_mutex_process.unlocks, (unsigned long long)startup_mutex_process.errors,
                unsigned(startup_mutex_process.live), unsigned(startup_mutex_process.peak), unsigned(StartupMutexProcess::capacity));
            std::printf("STARTUP_LV2_OBJECTS semaphores=%zu/%d queues=%zu/%d ports=%zu/%d event_flags=%zu/%d\n",
                state->owner->event_queues ? ps3_lv2_live_count(state->owner->event_queues, PS3_LV2_KIND_SEMAPHORE) : size_t(0), int(PS3_LV2_SEMAPHORE_CAP),
                state->owner->event_queues ? ps3_lv2_live_count(state->owner->event_queues, PS3_LV2_KIND_QUEUE) : size_t(0), int(PS3_LV2_QUEUE_CAP),
                state->owner->event_queues ? ps3_lv2_live_count(state->owner->event_queues, PS3_LV2_KIND_PORT) : size_t(0), int(PS3_LV2_PORT_CAP),
                state->owner->event_queues ? ps3_lv2_live_count(state->owner->event_queues, PS3_LV2_KIND_EVENT_FLAG) : size_t(0), int(PS3_LV2_EVENT_FLAG_CAP));
        }
#endif
        std::fflush(stdout);
    }
#endif
#ifdef PS3_NATIVE_STARTUP_FS
    {   // The six `sys_fs` imports (B2-S5). The first 32 calls are printed with
        // their guest path, the mount they resolved to and the code they returned -- that is the answer to "which mount does
        // the purge target" -- followed by one summary line. Read only after the confirmed join.
        const StartupFsObservation &fs_observation = state->control->fs;
        const bool storage_missing = !fs_observation.root_ready ||
            fs_observation.last_storage_status == FS_STORAGE_NOT_FOUND;
        for (size_t index = 0; index < fs_observation.captured; ++index) {
            const StartupFsCallRecord &record = fs_observation.capture[index];
            std::printf("STARTUP_FS_CALL n=%u sc=%u path=%s mount=%s rc=0x%08x\n",
                unsigned(index), unsigned(record.sc),
                record.sc == 806u || record.sc == 807u ? "-" : record.path,
                record.resolved ? fs_canonical_host_leaf(record.mount) : "unresolved",
                unsigned(uint32_t(record.rc)));
        }
        std::printf("STARTUP_FS_SUMMARY calls=%llu returns=%llu captured=%zu dropped=%zu root=%s root_ready=%d storage_missing=%d "
            "stat=%llu opendir=%llu readdir=%llu closedir=%llu rmdir=%llu unlink=%llu emfile=%llu ebadf=%llu denies_dir_ok=%llu denies_file_ok=%llu last_rc=0x%08x\n",
            (unsigned long long)fs_observation.calls, (unsigned long long)fs_observation.returns,
            fs_observation.captured, fs_observation.dropped, STARTUP_FS_ROOT,
            int(fs_observation.root_ready), int(storage_missing),
            (unsigned long long)fs_observation.counters.calls_stat,
            (unsigned long long)fs_observation.counters.calls_opendir,
            (unsigned long long)fs_observation.counters.calls_readdir,
            (unsigned long long)fs_observation.counters.calls_closedir,
            (unsigned long long)fs_observation.counters.calls_rmdir,
            (unsigned long long)fs_observation.counters.calls_unlink,
            (unsigned long long)fs_observation.counters.refusals_capacity,
            (unsigned long long)fs_observation.counters.refusals_stale_handle,
            (unsigned long long)fs_observation.counters.deletes_dir_ok,
            (unsigned long long)fs_observation.counters.deletes_file_ok,
            unsigned(uint32_t(fs_observation.last_rc)));
        std::printf("STARTUP_FS_FILE_SUMMARY open=%llu read=%llu write=%llu lseek=%llu fstat=%llu close=%llu bytes_read=%llu bytes_written=%llu open_files=%u\n",
            (unsigned long long)startup_fs_context.service.counters.calls_open,
            (unsigned long long)startup_fs_context.service.counters.calls_read,
            (unsigned long long)startup_fs_context.service.counters.calls_write,
            (unsigned long long)startup_fs_context.service.counters.calls_lseek,
            (unsigned long long)startup_fs_context.service.counters.calls_fstat,
            (unsigned long long)startup_fs_context.service.counters.calls_close,
            (unsigned long long)startup_fs_context.service.counters.bytes_read,
            (unsigned long long)startup_fs_context.service.counters.bytes_written,
            unsigned(fs_open_file_count(&startup_fs_context.service)));
        for (size_t index = 0; index < startup_fs_context.process_logged; ++index) {
            const StartupFsProcessCall &record = startup_fs_context.process_log[index];
            std::printf("STARTUP_FS_PROCESS_CALL n=%zu sc=%u form=%s thread=0x%x rc=0x%08x path=%s%s\n",
                index, unsigned(record.sc), record.import_form ? "import" : "syscall",
                unsigned(record.thread_id), unsigned(uint32_t(record.rc)), record.path,
                record.truncated ? " truncated=1" : "");
        }
        std::printf("STARTUP_FS_PROCESS_LOG logged=%zu dropped=%llu capacity=%zu\n",
            startup_fs_context.process_logged,
            (unsigned long long)startup_fs_context.process_dropped,
            size_t(STARTUP_FS_PROCESS_LOG_CAPACITY));
        std::fflush(stdout);
        // The scratch tree the host tests create under the build directory is the test's own to remove: the composition never
        // deletes it (decision 24 makes the GUEST's deletes real, not the harness's).
    }
#endif
    // Every mutable read below follows confirmed join. Keep reservation owners,
    // unknown resources and the live caller graph until process exit.
    const auto spu_snapshot = ps3_spu_resource_policy_snapshot(&spu.policy);
    if (spu_snapshot.status != PS3_SPU_POLICY_OK || !spu_snapshot.has_snapshot)
        native_budget_fatal("discovery-spu-policy-snapshot");
    const auto &spu_result = state->control->spu_initialize_result;
    std::printf("STARTUP_SPU_INITIALIZE_RESULT module_admission=assumed game_boot=false service_scope=admission_policy_only observation_scope=last_reached_handler calls=%zu returns=%zu last_handler_usable=0x%016llx last_handler_raw=0x%016llx last_handler_has_result=%d last_handler_status=%d last_handler_has_snapshot=%d previous_managed=%u previous_raw=%u result_managed=%u result_raw=%u retained_managed=%u retained_raw=%u snapshot_status=%d resource_inventory=empty spu_execution_available=false graph_retained=true joined=true\n",
        state->control->spu_initialize_calls, state->control->spu_initialize_returns,
        (unsigned long long)state->control->spu_initialize_usable,
        (unsigned long long)state->control->spu_initialize_raw,
        int(state->control->spu_initialize_has_result), int(spu_result.status), int(spu_result.has_snapshot),
        unsigned(spu_result.previous.managed_limit), unsigned(spu_result.previous.raw_limit),
        unsigned(spu_result.current.managed_limit), unsigned(spu_result.current.raw_limit),
        unsigned(spu_snapshot.current.managed_limit), unsigned(spu_snapshot.current.raw_limit),
        int(spu_snapshot.status));
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {   // N12 observation: real aa6269a8 calls/returns/writes/result, kernel fence + context state and quota.
        const auto &c=*state->control;
        const auto &r=c.spurs_initialize_result;
        ps3_user_memory_info quota{};
        const auto quota_status=ps3_user_memory_query(&allocator,&quota);
        std::printf("STARTUP_SPURS_INITIALIZE_RESULT module_admission=assumed game_boot=false scope=native_n12_reconstruction nid=0xaa6269a8 calls=%zu returns=%zu writes=%zu has_result=%d outcome=%d rc=0x%08x stop_reason=%u step=%u instance=%u detail=0x%08x kernel_spu=%u kernel_step=%u write_trace_failure=%d fence_latched=%d fence_category=%u fence_instance=%u fence_reason=%u fence_spu=%u fence_step=%u quota_status=%d quota_used=0x%llx quota_available=0x%llx\n",
            c.spurs_initialize_calls,c.spurs_initialize_returns,c.spurs_initialize_writes,int(c.spurs_initialize_has_result),
            int(r.outcome),unsigned(r.rc),unsigned(r.reason),unsigned(r.step),unsigned(r.instance),unsigned(r.detail),
            unsigned(r.kernel_spu),unsigned(r.kernel_step),int(c.spurs_write_trace_failure),int(c.spurs_fence_latched),unsigned(c.spurs_fence.category),
            unsigned(c.spurs_fence.instance),unsigned(c.spurs_fence.reason),unsigned(c.spurs_fence.spu),unsigned(c.spurs_fence.step),
            int(quota_status),(unsigned long long)quota.used_bytes,(unsigned long long)quota.available_bytes);
        {   // before/after statuses, prior (guest) allocation, SPURS delta charge and quota total, recorded separately
            const auto &q=c.spurs_quota;
            std::printf("STARTUP_SPURS_QUOTA_REPORT before_status=%d after_status=%d measured=%d charged=%d prior_used=0x%llx delta_charge=0x%llx total_used=0x%llx expected_delta=0x%llx charge_matches=%d final_query_status=%d final_used=0x%llx\n",
                q.before.status,q.after.status,int(q.measured),int(q.charged),(unsigned long long)q.prior_used,
                (unsigned long long)q.delta_charge,(unsigned long long)q.total_used,
                (unsigned long long)STARTUP_SPURS_FIRST_INSTANCE_CHARGE,int(startup_spurs_quota_charge_is(q,STARTUP_SPURS_FIRST_INSTANCE_CHARGE)),
                int(quota_status),(unsigned long long)quota.used_bytes);
        }
        {   // SPURS synchronization create diagnostics: DIAGNOSTIC observation of the lwmutex/lwcond create callbacks only (not a gameplay
            // claim). One summary line, then every retained bounded record in call order, then the last failed create if there was one.
            // Snapshots were taken per registry under that registry's own guard just before the real call and are not atomic across registries.
            char sync_line[1536];
            startup_spurs_sync_create_format_summary(sync_line,sizeof sync_line,c.spurs_sync_create_calls,c.spurs_sync_create_retained,
                c.spurs_sync_create_dropped,c.spurs_sync_create_overflow,c.spurs_sync_has_last_failed);
            std::puts(sync_line);
            for(size_t sync_index=0;sync_index<c.spurs_sync_create_retained && sync_index<STARTUP_SPURS_SYNC_CREATE_CAPACITY;++sync_index) {
                char sync_label[24];
                std::snprintf(sync_label,sizeof sync_label,"%zu",sync_index);
                startup_spurs_sync_create_format(sync_line,sizeof sync_line,sync_label,c.spurs_sync_create[sync_index]);
                std::puts(sync_line);
            }
            if(c.spurs_sync_has_last_failed) {
                startup_spurs_sync_create_format(sync_line,sizeof sync_line,"last_failed",c.spurs_sync_last_failed);
                std::puts(sync_line);
            }
        }
        for(size_t argument=0;argument<7;++argument)
            std::printf("SPURS_INITIALIZE_RAW_ARGUMENT r%zu=0x%016llx\n",argument+3,(unsigned long long)c.spurs_initialize_arguments[argument]);
        unsigned idle_contexts=0,active_instances=0;
        for(unsigned index=0;index<PS3_SPURS_MAX_INSTANCES;++index) {
            const auto &in=spurs.composition.runtime.instances[index];
            if(in.state==PS3_SPURS_INSTANCE_FREE) continue;
            if(in.state==PS3_SPURS_INSTANCE_ACTIVE) ++active_instances;
            std::printf("STARTUP_SPURS_INSTANCE index=%u state=%u spurs_ea=0x%08x nspus=%u group=0x%08x ppu_priority=%u handler_status=%llu kernel_initialized=%u kernel_phase=%d\n",
                index,unsigned(in.state),unsigned(in.spurs_ea),unsigned(in.nspus),unsigned(in.group),unsigned(in.ppu_priority),
                (unsigned long long)__atomic_load_n(&in.handler_status,__ATOMIC_SEQ_CST),unsigned(in.kernel_initialized),
                in.kernel_initialized?int(ps3_spurs_k1_phase_of(&in.kernel)):-1);
            if(!in.kernel_initialized) continue;
            for(uint32_t spu_index=0;spu_index<in.nspus && spu_index<PS3_SPURS_K1_MAX_SPUS;++spu_index) {
                const auto context=ps3_spurs_k1_context_state_of(&in.kernel,spu_index);
                const uint64_t idle=ps3_spurs_k1_counter(&in.kernel,spu_index,PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS);
                if(in.state==PS3_SPURS_INSTANCE_ACTIVE && context==PS3_SPURS_K1_CTX_BLOCKED_LR && idle>=1) ++idle_contexts;
                std::printf("STARTUP_SPURS_CONTEXT instance=%u spu=%u state=%d idle_publications=%llu wake_evaluations=%llu conditional_failures=%llu steps=%llu\n",
                    index,unsigned(spu_index),int(context),(unsigned long long)idle,
                    (unsigned long long)ps3_spurs_k1_counter(&in.kernel,spu_index,PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS),
                    (unsigned long long)ps3_spurs_k1_counter(&in.kernel,spu_index,PS3_SPURS_K1_COUNT_CONDITIONAL_FAILURES),
                    (unsigned long long)ps3_spurs_k1_counter(&in.kernel,spu_index,PS3_SPURS_K1_COUNT_STEPS));
            }
        }
        // Expected next boundary after exactly ONE successful aa6269a8 with five started idle contexts: sc128
        // (sys_event_queue_create) as a native UnsupportedSyscall. A log line only; not a game-boot claim.
        const bool one_successful=c.spurs_initialize_calls==1 && c.spurs_initialize_returns==1 && c.spurs_initialize_has_result &&
            r.outcome==PS3_SPURS_OUTCOME_OK && r.rc==0 && active_instances==1 && idle_contexts==5 &&
            startup_spurs_quota_charge_is(c.spurs_quota,STARTUP_SPURS_FIRST_INSTANCE_CHARGE);   // measured delta, not the quota total
        const bool expected_boundary=one_successful && c.stop==StartupStop::UnsupportedSyscall && c.syscall_number==128;
        std::printf("STARTUP_SPURS_N12_BOUNDARY expected_next_stop=unsupported_syscall_128 observed_stop=%d observed_syscall=%u one_successful_instance=%d active_instances=%u idle_contexts=%u expected_boundary_observed=%d acceptance_scope=probe_log_only game_boot=false\n",
            int(c.stop),unsigned(c.syscall_number),int(one_successful),active_instances,idle_contexts,int(expected_boundary));
        {   // B9BC6207 observation: bounded scalars only, no full state. Not a guest-OK or render claim: the result/stop below are what the native route observed.
            const auto &ar=c.spurs_attach_result;
            const auto &ai=c.spurs_attach_info;
            std::printf("STARTUP_SPURS_ATTACH_RESULT game_boot=false render_claim=false nid=0xb9bc6207 calls=%zu returns=%zu has_result=%d args_r3=0x%016llx r4=0x%016llx r5=0x%016llx r6=0x%016llx effective_sdk_present=%d sdk=0x%08x outcome=%d rc=0x%08x stop_reason=%u step=%u detail=0x%08x instance=%u reads=%u writes=%u cas_calls=%u cas_committed=%u spup=%u port_written=%u cas_attempts=%u prior_bits=0x%016llx prior_bit_set=%u commit_status=%d cause=%u callback_error=%d trace_failure=%d latched_stop=%d invalid_address=0x%08x observed_stop=%d observed_syscall=%u final_r3=0x%016llx\n",
                c.spurs_attach_calls,c.spurs_attach_returns,int(c.spurs_attach_has_result),(unsigned long long)c.spurs_attach_arguments[0],
                (unsigned long long)c.spurs_attach_arguments[1],(unsigned long long)c.spurs_attach_arguments[2],(unsigned long long)c.spurs_attach_arguments[3],
                int(c.spurs_attach_has_sdk),unsigned(c.spurs_attach_sdk),int(ar.outcome),unsigned(ar.rc),unsigned(ar.reason),unsigned(ar.step),unsigned(ar.detail),
                unsigned(ar.instance),c.spurs_attach_reads,c.spurs_attach_writes,c.spurs_attach_cas_calls,c.spurs_attach_cas_committed,unsigned(ai.spup),
                unsigned(ai.port_written),unsigned(ai.cas_attempts),(unsigned long long)ai.prior_port_bits,unsigned(ai.prior_bit_set),ai.commit_status,unsigned(ai.cause),
                int(c.spurs_attach_callback_error),int(c.spurs_attach_trace_failure),int(c.spurs_attach_stop_latched),unsigned(c.spurs_attach_invalid_address),
                int(c.stop),unsigned(c.syscall_number),(unsigned long long)state->owner->ppu.cpu.gpr[3]);
        }
        {   // 1F402F8F (cellSpursGetInfo) observation of the LAST call: bounded scalars only, printed after the join and before resource teardown, as an adjacent
            // report. reads/writes are the adapter's ACTUAL performed effects; domain_reads/writes are the domain's own count and must agree (mismatch is reported, not hidden).
            // meet/miss deadline words are reference-compatibility zeros in the guest output, NOT telemetry, so they are not printed as measurements. No game-picture/FPS claim.
            const auto &ir=c.spurs_info_result;
            const auto &ip=c.spurs_info_report;
            std::printf("STARTUP_SPURS_INFO_RESULT game_boot=false render_claim=false nid=0x1f402f8f calls=%zu returns=%zu has_result=%d args_r3=0x%016llx r4=0x%016llx outcome=%d rc=0x%08x stop_reason=%u step=%u detail=0x%08x instance=%u reads=%u writes=%u domain_reads=%u domain_writes=%u counts_agree=%d last_write_offset=0x%08x partial=%u flags=0x%08x nspus=%u prefix_size=%u trace_buffer_raw=0x%016llx trace_size=0x%016llx mirror_mismatch=0x%02x callback_error=%d trace_failure=%d latched_stop=%d invalid_address=0x%08x observed_stop=%d observed_syscall=%u final_r3=0x%016llx\n",
                c.spurs_info_calls,c.spurs_info_returns,int(c.spurs_info_has_result),(unsigned long long)c.spurs_info_arguments[0],(unsigned long long)c.spurs_info_arguments[1],
                int(ir.outcome),unsigned(ir.rc),unsigned(ir.reason),unsigned(ir.step),unsigned(ir.detail),unsigned(ir.instance),c.spurs_info_reads,c.spurs_info_writes,
                unsigned(ip.reads),unsigned(ip.writes),int(c.spurs_info_reads==ip.reads && c.spurs_info_writes==ip.writes),unsigned(ip.last_write_offset),unsigned(ip.partial),unsigned(ip.flags),
                unsigned(ip.nspus),unsigned(ip.prefix_size),(unsigned long long)ip.trace_buffer_raw,(unsigned long long)ip.trace_size,unsigned(ip.mirror_mismatch),
                int(c.spurs_info_callback_error),int(c.spurs_info_trace_failure),int(c.spurs_info_stop_latched),unsigned(c.spurs_info_invalid_address),
                int(c.stop),unsigned(c.syscall_number),(unsigned long long)state->owner->ppu.cpu.gpr[3]);
        }
    }
#endif
    const auto budget = state->policy->snapshot();
    const auto memory = state->recorder.report();
    const auto observation = state->observation.report();
    const auto &terminal = state->control->terminal;
    std::printf("STARTUP_SYSUTIL_CALLBACK_RESULT module_admission=assumed game_boot=false service_scope=registration_only observation_scope=last_reached_handler calls=%zu returns=%zu last_handler_slot=0x%016llx last_handler_descriptor=0x%016llx last_handler_userdata=0x%016llx last_handler_has_result=%d invalid_argument=%d status=%d result_slot=%u previous_descriptor=0x%08x previous_userdata=0x%08x current_descriptor=0x%08x current_userdata=0x%08x pending_trampoline_seen=%d graph_retained=true\n",
        state->control->sysutil_callback_calls,state->control->sysutil_callback_returns,
        (unsigned long long)state->control->sysutil_callback_slot,
        (unsigned long long)state->control->sysutil_callback_descriptor,
        (unsigned long long)state->control->sysutil_callback_userdata,
        int(state->control->sysutil_callback_has_result),int(state->control->sysutil_callback_invalid_argument),
        int(state->control->sysutil_callback_result.status),
        unsigned(state->control->sysutil_callback_result.slot),
        unsigned(state->control->sysutil_callback_result.previous.callback_descriptor_address),
        unsigned(state->control->sysutil_callback_result.previous.userdata_address),
        unsigned(state->control->sysutil_callback_result.current.callback_descriptor_address),
        unsigned(state->control->sysutil_callback_result.current.userdata_address),
        int(state->control->sysutil_callback_pending_trampoline_seen));
    for(uint32_t slot=0;slot<PS3_SYSUTIL_CALLBACK_SLOT_COUNT;++slot) {
        ps3_sysutil_callback_pair retained{};
        const auto status=ps3_sysutil_callback_read(&sysutil.registry,slot,&retained);
        if(status!=PS3_SYSUTIL_CALLBACK_OK) native_budget_fatal("sysutil-callback-snapshot");
        std::printf("STARTUP_SYSUTIL_CALLBACK_PAIR module_admission=assumed game_boot=false service_scope=registration_only slot=%u callback_descriptor=0x%08x userdata=0x%08x snapshot_status=%d\n",
            unsigned(slot),unsigned(retained.callback_descriptor_address),unsigned(retained.userdata_address),int(status));
    }
    std::printf("STARTUP_DIAGNOSTIC_MODULE_RESULT module_admission=assumed game_boot=false observation_scope=last_reached_handler calls=%zu returns=%zu last_handler_argument=0x%016llx last_handler_has_result=%d status=%d module_id=0x%04x previous_count=%u current_count=%u total_admissions=%llu pending_trampoline_seen=%d graph_retained=true\n",
        state->control->diagnostic_module_calls,state->control->diagnostic_module_returns,
        (unsigned long long)state->control->diagnostic_module_argument,int(state->control->diagnostic_module_has_result),
        int(state->control->diagnostic_module_result.status),unsigned(state->control->diagnostic_module_result.module_id),
        unsigned(state->control->diagnostic_module_result.previous_count),unsigned(state->control->diagnostic_module_result.current_count),
        (unsigned long long)state->control->diagnostic_module_result.total_admissions,
        int(state->control->diagnostic_module_pending_trampoline_seen));
    std::printf("STARTUP_SYSMODULE_INITIALIZE calls=%zu returns=%zu last_result=0x%08x\n",
        state->control->sysmodule_initialize_calls,state->control->sysmodule_initialize_returns,
        unsigned(state->control->sysmodule_initialize_last_result));
    for(size_t module=0;module<modules.catalog_count;++module) {
        ps3_diagnostic_module_result admitted{};
        const auto status=ps3_diagnostic_module_read(&modules.state,modules.catalog[module],&admitted);
        if(status!=PS3_DIAGNOSTIC_MODULE_OK) native_budget_fatal("diagnostic-module-snapshot");
        std::printf("STARTUP_DIAGNOSTIC_MODULE_SLOT module_admission=assumed game_boot=false module_id=0x%04x count=%u total_admissions=%llu\n",
            unsigned(admitted.module_id),unsigned(admitted.current_count),(unsigned long long)admitted.total_admissions);
    }
    ps3_exitspawn_snapshot retained_exitspawn{};
    const auto exitspawn_snapshot_status=ps3_exitspawn_read(&process.registry,&retained_exitspawn);
    if(exitspawn_snapshot_status!=PS3_EXITSPAWN_OK) native_budget_fatal("discovery-exitspawn-snapshot");
    std::printf("STARTUP_EXITSPAWN_RESULT calls=%zu returns=%zu pending_trampoline_seen=%d kind=%d argument=0x%016llx status=%d decision=%d last_atexitspawn=0x%08x last_at_Exitspawn=0x%08x retained_atexitspawn=0x%08x retained_at_Exitspawn=0x%08x snapshot_status=%d registration_policy=deferred_opaque_address graph_retained=true\n",
        state->control->exitspawn_calls,state->control->exitspawn_returns,
        int(state->control->exitspawn_pending_trampoline_seen),int(state->control->exitspawn_kind),(unsigned long long)state->control->exitspawn_argument,
        int(state->control->exitspawn_result.status),int(state->control->exitspawn_result.decision),
        unsigned(state->control->exitspawn_result.snapshot.atexitspawn),unsigned(state->control->exitspawn_result.snapshot.at_Exitspawn),
        unsigned(retained_exitspawn.atexitspawn),unsigned(retained_exitspawn.at_Exitspawn),int(exitspawn_snapshot_status));
    std::printf("STARTUP_CONDITION_RESULT calls=%zu returns=%zu nid=0x%08x address=0x%08x status=%d memory_status=%d wait_status=%d mutex_status=%d condition_outcome=%d mutex_reacquired=%u cleanup_retained=%u must_not_resume=%u publications=%zu selected=%zu graph_retained=true\n",
        state->control->condition_calls,state->control->condition_returns,
        unsigned(state->control->condition_nid),unsigned(state->control->condition_address),
        int(state->control->condition_result.status),int(state->control->condition_result.memory_status),
        int(state->control->condition_result.wait_status),int(state->control->condition_result.mutex_status),
        int(state->control->condition_result.condition_outcome),unsigned(state->control->condition_result.mutex_reacquired),
        unsigned(state->control->condition_result.cleanup_retained),unsigned(state->control->condition_result.must_not_resume),
        state->control->condition_publications,
        state->control->condition_selected);
    const bool dispatch_sum_valid = observation.pending_count <= 8 &&
        observation.completed.total <= UINT64_MAX - uint64_t(observation.pending_count);
    // This recorder starts at sequence zero. Its public overwritten end is
    // the retained begin; add only the public retained count with headroom.
    const uint64_t retained_begin = memory.overwritten_end;
    const bool retained_sum_valid = memory.retained_count <= 1024 &&
        uint64_t(memory.retained_count) <= UINT64_MAX - retained_begin;
    const uint64_t retained_end = retained_sum_valid
        ? retained_begin + uint64_t(memory.retained_count) : retained_begin;
    bool valid_report = !memory.batch_active && !memory.remaining_publications &&
        state->control->journal.count == 0 && state->owner->ppu.trace == nullptr &&
        state->control->import_count == 0 && state->control->dispatch_count == 0 &&
        !observation.immediate_outstanding && state->control->stop != StartupStop::None &&
        state->owner->ppu.recorder == &state->recorder && state->owner->ppu.budget == state->budget.get() &&
        state->owner->observation == &state->observation && observation.configured &&
        observation.imports.capacity == 64 && observation.completed.capacity == 32 && observation.pending_capacity == 8 &&
        observation.pending_count == state->owner->dispatch.depth && dispatch_sum_valid &&
        observation.dispatch_started == observation.completed.total + uint64_t(observation.pending_count) &&
        retained_sum_valid && retained_end == memory.published_count && memory.overwritten_begin == 0 &&
        state->events.size() == 1024;
    std::printf("STARTUP_DISCOVERY_JOINED module_admission=assumed game_boot=false stop=%d external_id=0x%x syscall_low32=%u r11=0x%016llx lr=0x%016llx sp=0x%016llx ctr=0x%016llx depth=%u generated=%llu hle_syscalls=%llu indirect=%llu budget_reason=%d operational_clock_calls=%u resource_cleanup=not_attempted reservation_release=not_attempted graph_retained=true\n",
        int(state->control->stop), state->control->external_id, state->control->syscall_number,
        (unsigned long long)terminal.gpr[11], (unsigned long long)terminal.lr,
        (unsigned long long)terminal.gpr[1], (unsigned long long)terminal.ctr,
        state->owner->dispatch.depth, (unsigned long long)budget.generated,
        (unsigned long long)budget.hle, (unsigned long long)budget.indirect,
        int(budget.stop), state->clock.calls);
    std::printf("STARTUP_DISCOVERY_CALENDAR calls=%zu status=%d seconds=%lld nanoseconds=%u\n",
        state->control->calendar_calls, int(state->control->calendar_status),
        (long long)state->control->calendar_sample.seconds, unsigned(state->control->calendar_sample.nanoseconds));
    std::printf("STARTUP_DISCOVERY_TIMEZONE calls=%zu timezone_minutes=%d summertime_minutes=%d configured_policy=explicit_virtual_ps3_fixed_utc\n",
        state->control->timezone_calls, int(state->control->timezone_minutes), int(state->control->summertime_minutes));
    std::printf("STARTUP_DISCOVERY_TIMEBASE attempts=%zu status=%d has_sample=%d last_microseconds=%llu last_ticks=%llu equal_samples=%llu frequency_hz=%llu granularity_microseconds=1 source=borrowed_horizon_elapsed_clock\n",
        state->control->timebase_attempts, int(state->control->timebase_status), state->control->timebase_has_sample,
        (unsigned long long)state->control->timebase_last_microseconds,
        (unsigned long long)state->control->timebase_last_ticks,
        (unsigned long long)state->control->timebase_equal_samples,
        (unsigned long long)PS3_TIMEBASE_FREQUENCY);
    const auto &service = state->control->memory_result;
    std::printf("STARTUP_DISCOVERY_SERVICE_SNAPSHOT memory_returns=%zu service_status=%d allocator_status=%d memory_status=%d allocator_memory_status=%d published=%d retained_allocation=%d retained_pin=%d retained_address=0x%x mutex_status=%d clock_status=%d invalid_address=0x%016llx generated_memory_status_retained_by_bridge=false\n",
        state->control->memory_returns, int(service.status), int(service.allocator.status),
        int(service.memory_status), int(service.allocator.memory_status), service.published,
        service.retained_allocation, service.retained_pin, service.retained_address,
        int(state->control->mutex_failure.status), int(state->control->clock_status),
        (unsigned long long)state->control->invalid_address);
#ifdef PS3_NATIVE_STARTUP_THREAD_PRIORITY
    std::printf("STARTUP_THREAD_PRIORITY_RESULT module_admission=assumed game_boot=false observation_scope=last_reached_handler syscall=48 calls=%zu returns=%zu requested_id=0x%016llx output_argument=0x%016llx has_priority=%d captured_priority=%d has_result=%d captured_result=0x%016llx memory_checked=%d write_attempted=%d published=%d memory_status=%d output_readback=not_attempted event_stream=DISCOVERY_ACCESS_records\n",
        state->control->thread_priority_calls, state->control->thread_priority_returns,
        (unsigned long long)state->control->thread_priority_id,
        (unsigned long long)state->control->thread_priority_output,
        int(state->control->thread_priority_has_value), int(state->control->thread_priority_value),
        int(state->control->thread_priority_has_result), (unsigned long long)state->control->thread_priority_result,
        int(state->control->thread_priority_memory_checked), int(state->control->thread_priority_write_attempted),
        int(state->control->thread_priority_published), int(state->control->thread_priority_memory_status));
#endif
#ifdef PS3_NATIVE_LV2_SYNC
    // Root (B2a review): name the last lv2 sync syscall and its object, so a deadline stop while a
    // guest wait is blocked says which syscall and which id it was blocked on.
    std::printf("STARTUP_LV2_SYNC_RESULT calls=%zu returns=%zu last_syscall=%u last_argument=0x%08x last_result=0x%08x blocked=0x%016llx\n",
        state->control->lv2_sync_calls, state->control->lv2_sync_returns,
        unsigned(state->control->lv2_sync_number), unsigned(state->control->lv2_sync_argument),
        unsigned(state->control->lv2_sync_result), (unsigned long long)state->control->lv2_sync_blocked);
#endif
#ifdef PS3_NATIVE_LV2_SPU
    // Decision 33 / B2b: the SPU images and thread groups, and -- when 173 was reached -- the exact
    // group, thread count and per-thread entry point phase C1 will run.
    {
        const StartupControl &c = *state->control;
        std::printf("STARTUP_LV2_SPU_RESULT calls=%zu returns=%zu last_syscall=0x%08llx last_argument=0x%08llx "
            "last_entry=0x%05x segments=%zu image_segments=%zu started=%d group=0x%08x num=%u priority=%u entries=",
            c.lv2_spu_calls, c.lv2_spu_returns, (unsigned long long)c.lv2_spu_number,
            (unsigned long long)c.lv2_spu_argument, unsigned(c.lv2_spu_last_entry),
            size_t(c.lv2_spu_segments_address), size_t(c.lv2_spu_image_segments),
            int(c.lv2_spu_has_start), unsigned(c.lv2_spu_last_group), unsigned(c.lv2_spu_last_num),
            unsigned(c.lv2_spu_last_priority));
        for (uint32_t index = 0; index < c.lv2_spu_last_num && index < 8; ++index)
            std::printf("%s0x%05x", index ? "," : "", unsigned(c.lv2_spu_entries[index]));
        std::printf("\n");
        {
            uint64_t flat[7]; ps3_flat_guest_debug(flat);
            std::printf("STARTUP_FLAT_GUEST committed_pages=%llu slabs=%llu slab_acquires=%llu slab_releases=%llu slab_failures=%llu last_kernel_result=0x%llx last_slab_step=%llu "
                "(steps: 1 memalign, 2 find code address, 3 svcMapProcessCodeMemory, 4 svcSetProcessMemoryPermission, 5 svcMapProcessMemory)\n",
                (unsigned long long)flat[0], (unsigned long long)flat[1], (unsigned long long)flat[2], (unsigned long long)flat[3],
                (unsigned long long)flat[4], (unsigned long long)flat[5], (unsigned long long)flat[6]);
        }
        startup_activity_print();
#ifdef PS3_NATIVE_LLE_PROFILE
        std::printf("STARTUP_SAVEDATA list_auto_load calls=%u returns=%u version=%u fixed_called=%u fixed_result=%d dir=\"%s\" stat_called=%u stat_result=%d result=0x%08x list_load2_calls=%u list_load2_verdict=%d list_load2_result=0x%08x\n",
            unsigned(g_savedata_auto_load.calls), unsigned(g_savedata_auto_load.returns), unsigned(g_savedata_auto_load.version),
            unsigned(g_savedata_auto_load.fixed_called), int(g_savedata_auto_load.fixed_result), g_savedata_auto_load.dir_name,
            unsigned(g_savedata_auto_load.stat_called), int(g_savedata_auto_load.stat_result), unsigned(g_savedata_auto_load.result), unsigned(g_savedata_auto_load.list_load_calls), int(g_savedata_auto_load.list_load_verdict), unsigned(g_savedata_auto_load.list_load_result));
#endif
        std::printf("STARTUP_MOVIE_POOL calls=%u zero_answers=%u waits=%u wait_timeouts=%u last_player=0x%08x\n", unsigned(g_movie_pool_calls.load()),
            unsigned(g_movie_pool_zero.load()), unsigned(g_movie_pool_waits.load()), unsigned(g_movie_pool_wait_timeouts.load()), unsigned(g_movie_pool_last_player.load()));
        startup_print_lwmutex_time(0xFFFFFFFFu);   // Root (Decision 86)
        {
            std::printf("STARTUP_DOMAIN_OPS conflicts=%llu conflicts_with_operation=%llu sticky_stores=%llu late_repeats=%llu sticky_lines=%llu", (unsigned long long)g_ppu_fast_write_conflicts.load(), (unsigned long long)g_ppu_fast_write_conflicts_locked.load(),
                (unsigned long long)g_ppu_fast_write_sticky.load(), (unsigned long long)g_ppu_fast_write_late.load(), (unsigned long long)g_ps3_atomic_sticky_lines);
            for (unsigned line = 0; line < 1024u; ++line)
                if (const uint64_t count = ps3_atomic_domain_debug_enter_count(line)) std::printf(" L%u=%llu", line, (unsigned long long)count);
            std::printf("\n");
        }
        startup_activity_print_calls(0xFFFFFFFFu);
        const uint32_t spu_calls = g_startup_spu_call_count.load();
        for (uint32_t index = 0; index < spu_calls && index < kStartupSpuCallLogCapacity; ++index) {
            const StartupSpuCallRecord &r = g_startup_spu_call_log[index];
            std::printf("STARTUP_LV2_SPU_CALL n=%u of=%u sc=%u a0=0x%08x a1=0x%08x a2=0x%08x result=0x%08x\n", unsigned(index),
                unsigned(spu_calls), unsigned(r.number), unsigned(r.a0), unsigned(r.a1), unsigned(r.a2), unsigned(r.result));
        }
    }
#endif
#ifdef PS3_NATIVE_LLE_SRE
    std::printf("STARTUP_LLE_SRE_RESULT routed=%zu last_nid=0x%08x libc_returns=%zu\n",
        size_t(state->control->lle_sre_routed), unsigned(state->control->lle_sre_last_nid),
        size_t(state->control->lle_sre_libc_returns));
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
    const auto &attribute_result = state->control->spurs_attribute;
    const bool attribute_return_completed = state->control->spurs_attribute_calls != 0 &&
        state->control->spurs_attribute_calls != SIZE_MAX &&
        state->control->spurs_attribute_calls == state->control->spurs_attribute_returns;
    std::printf("STARTUP_SPURS_ATTRIBUTE_RESULT module_admission=assumed game_boot=false scope=last_reached_handler nid=0x%08x calls=%zu returns=%zu completed_return_counter_evidence=%d has_result=%d result=0x%08x memory_checked=%d memory_status=%d write_attempted=%d last_write_is_toc=%d last_write_address=0x%08x last_write_length=%zu successful_writes=%zu publications=%zu attribute_bytes=%zu last_attribute_address=0x%08x last_attribute_length=%zu attribute_readback=not_attempted event_stream=DISCOVERY_ACCESS_records\n",
        unsigned(attribute_result.nid), state->control->spurs_attribute_calls, state->control->spurs_attribute_returns,
        int(attribute_return_completed), int(attribute_result.has_result), unsigned(attribute_result.result),
        int(attribute_result.memory_checked), int(attribute_result.memory_status), int(attribute_result.write_attempted),
        int(attribute_result.last_write_is_toc), unsigned(attribute_result.last_write_address), attribute_result.last_write_length,
        attribute_result.successful_writes, attribute_result.publications, attribute_result.attribute_bytes,
        unsigned(attribute_result.last_attribute_address), attribute_result.last_attribute_length);
    for (size_t argument = 0; argument < 7; ++argument)
        std::printf("SPURS_ATTRIBUTE_RAW_ARGUMENT r%zu=0x%016llx\n", argument + 3,
            (unsigned long long)attribute_result.arguments[argument]);
    if (attribute_result.last_attribute_length > sizeof(attribute_result.last_attribute_bytes)) valid_report = false;
    else for (size_t offset = 0; offset < attribute_result.last_attribute_length; offset += 32) {
        const size_t remaining = attribute_result.last_attribute_length - offset;
        const size_t length = remaining < 32 ? remaining : 32;
        std::printf("SPURS_ATTRIBUTE_PUBLISHED_BYTES scope=last_confirmed_attribute_write address=0x%08x offset=%zu length=%zu data=",
            unsigned(attribute_result.last_attribute_address), offset, length);
        for (size_t byte = 0; byte < length; ++byte) std::printf("%02x", attribute_result.last_attribute_bytes[offset + byte]);
        std::putchar('\n');
    }
    // Workload attribute initializer (efeb2679) and SetName (4a5eab63): separate observations/counters, bounded scalars and
    // the initializer's own 512-byte image only. Names and priority/pm pointers are numbers; no guest memory is touched here.
    const auto &workload = state->control->spurs_workload_attribute;
    std::printf("STARTUP_SPURS_WORKLOAD_ATTRIBUTE_RESULT scope=last_reached_handler nid=0xefeb2679 calls=%zu returns=%zu stack_slot=0x%08x stack_raw=0x%016llx max_contention=0x%08x priority_address=0x%08x priority=%02x%02x%02x%02x%02x%02x%02x%02x sp=0x%016llx stack_read=%d priority_read=%d image_write_attempted=%d image_written_bytes=%zu publications=%zu toc_written=%d has_result=%d result=0x%08x memory_status=%d attribute_readback=not_attempted event_stream=DISCOVERY_ACCESS_records\n",
        state->control->spurs_workload_attribute_calls, state->control->spurs_workload_attribute_returns,
        unsigned(workload.stack_slot), (unsigned long long)workload.stack_raw, unsigned(workload.max_contention), unsigned(workload.priority_address),
        workload.priority[0], workload.priority[1], workload.priority[2], workload.priority[3],
        workload.priority[4], workload.priority[5], workload.priority[6], workload.priority[7], (unsigned long long)workload.sp,
        int(workload.stack_read), int(workload.priority_read), int(workload.image_write_attempted), workload.image_written_bytes,
        workload.publications, int(workload.toc_written), int(workload.has_result), unsigned(workload.result), int(workload.memory_status));
    for (size_t argument = 0; argument < 8; ++argument)
        std::printf("SPURS_WORKLOAD_ATTRIBUTE_RAW_ARGUMENT r%zu=0x%016llx\n", argument + 3, (unsigned long long)workload.arguments[argument]);
    for (size_t offset = 0; offset < sizeof(workload.image) && workload.image_written_bytes == sizeof(workload.image); offset += 32) {
        std::printf("SPURS_WORKLOAD_ATTRIBUTE_IMAGE offset=%zu data=", offset);
        for (size_t byte = 0; byte < 32; ++byte) std::printf("%02x", workload.image[offset + byte]);
        std::putchar('\n');
    }
    const auto &named = state->control->spurs_workload_name;
    std::printf("STARTUP_SPURS_WORKLOAD_NAME_RESULT scope=last_reached_handler nid=0x4a5eab63 calls=%zu returns=%zu attribute=0x%08x name_class=0x%08x name_instance=0x%08x patch_address=0x%08x patch=%02x%02x%02x%02x%02x%02x%02x%02x raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx class_write_attempted=%d class_written=%d instance_write_attempted=%d instance_written=%d written_bytes=%zu publications=%zu toc_written=%d has_result=%d result=0x%08x memory_status=%d names_dereferenced=false attribute_readback=not_attempted\n",
        state->control->spurs_workload_name_calls, state->control->spurs_workload_name_returns,
        unsigned(named.attribute), unsigned(named.name_class), unsigned(named.name_instance), unsigned(named.patch_address),
        named.patch[0], named.patch[1], named.patch[2], named.patch[3], named.patch[4], named.patch[5], named.patch[6], named.patch[7],
        (unsigned long long)named.arguments[0], (unsigned long long)named.arguments[1], (unsigned long long)named.arguments[2],
        int(named.class_write_attempted), int(named.class_written), int(named.instance_write_attempted), int(named.instance_written),
        named.written_bytes, named.publications, int(named.toc_written), int(named.has_result), unsigned(named.result), int(named.memory_status));
#endif
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {
        const auto &registration=state->control->spurs_registration;
        const auto &domain=registration.domain_result;
        std::printf("STARTUP_SPURS_REGISTRATION_RESULT nid=0xc0158d8b calls=%zu returns=%zu raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx attribute=0x%08x revision=%u has_domain_result=%d native_stop=%u domain_has_sdk_result=%u domain_sdk_result=0x%08x wid=%u unique_id=%u effects=0x%x reads=%zu writes=%zu written_bytes=%zu observer_calls=%zu domain_publications=%zu hle_publications=%zu attribute_reads=%zu has_result=%d result=0x%08x toc_written=%d memory_status=%d trace_failure=%d latched_stop=%d attribute_readback=not_attempted\n",
            size_t(state->control->spurs_registration_calls),size_t(state->control->spurs_registration_returns),
            (unsigned long long)registration.arguments[0],(unsigned long long)registration.arguments[1],
            (unsigned long long)registration.arguments[2],unsigned(registration.attribute),unsigned(registration.revision),
            int(registration.has_domain_result),unsigned(domain.stop),unsigned(domain.has_sdk_result),unsigned(domain.sdk_result),
            unsigned(domain.wid),unsigned(domain.unique_id),unsigned(domain.effects),size_t(domain.reads),size_t(domain.writes),
            size_t(domain.written_bytes),size_t(domain.observer_calls),size_t(domain.publications),size_t(registration.publications),
            size_t(registration.attribute_reads),int(registration.has_result),unsigned(registration.result),int(registration.toc_written),
            int(registration.memory_status),int(registration.trace_failure),int(registration.latched_stop));
        std::printf("STARTUP_SPURS_REGISTRATION_REQUEST spurs=0x%08x wid_output=0x%08x policy_module=0x%08x size=0x%x data=0x%016llx priority=%02x%02x%02x%02x%02x%02x%02x%02x min=%u max=%u class=0x%08x instance=0x%08x hook=0x%08x hook_arg=0x%08x scope=captured_request_not_readback\n",
            unsigned(registration.request.spurs),unsigned(registration.request.wid_output),unsigned(registration.request.policy_module),
            unsigned(registration.request.size),(unsigned long long)registration.request.data,
            registration.request.priority[0],registration.request.priority[1],registration.request.priority[2],registration.request.priority[3],
            registration.request.priority[4],registration.request.priority[5],registration.request.priority[6],registration.request.priority[7],
            unsigned(registration.request.min_contention),unsigned(registration.request.max_contention),unsigned(registration.request.name_class),
            unsigned(registration.request.name_instance),unsigned(registration.request.hook),unsigned(registration.request.hook_argument));
    }
    {
        const auto &idle=state->control->spurs_idle;
        const auto &domain=idle.domain_result;
        std::printf("STARTUP_SPURS_IDLE_RESULT nid=0x182d9890 calls=%zu returns=%zu raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx spurs=0x%08x wid=%u count=%u has_domain_result=%d native_stop=%u domain_has_sdk_result=%u domain_sdk_result=0x%08x reads=%zu writes=%zu written_bytes=%zu write_attempted=%u idle_byte_written=%u observer_calls=%zu domain_publications=%zu hle_publications=%zu has_result=%d result=0x%08x toc_written=%d memory_status=%d trace_failure=%d latched_stop=%d readiness_write=false\n",
            size_t(state->control->spurs_idle_calls),size_t(state->control->spurs_idle_returns),
            (unsigned long long)idle.arguments[0],(unsigned long long)idle.arguments[1],(unsigned long long)idle.arguments[2],
            unsigned(idle.request.spurs),unsigned(idle.request.wid),unsigned(idle.request.count),int(idle.has_domain_result),
            unsigned(domain.stop),unsigned(domain.has_sdk_result),unsigned(domain.sdk_result),size_t(domain.reads),
            size_t(domain.writes),size_t(domain.written_bytes),unsigned(domain.idle_byte_write_attempted),unsigned(domain.idle_byte_written),
            size_t(domain.observer_calls),size_t(domain.publications),size_t(idle.publications),int(idle.has_result),
            unsigned(idle.result),int(idle.toc_written),int(idle.memory_status),int(idle.trace_failure),int(idle.latched_stop));
    }
    {
        const auto &exception=state->control->spurs_exception;
        const auto &domain=exception.domain_result;
        std::printf("STARTUP_SPURS_EXCEPTION_RESULT nid=0x%08x calls=%zu returns=%zu action=%u raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx raw_r6=0x%016llx spurs=0x%08x wid=%u handler=0x%08x argument=0x%08x has_domain_result=%d native_stop=%u domain_has_sdk_result=%u domain_sdk_result=0x%08x slot=0x%08x ",
            unsigned(exception.nid),size_t(state->control->spurs_exception_calls),size_t(state->control->spurs_exception_returns),unsigned(exception.request.action),
            (unsigned long long)exception.arguments[0],(unsigned long long)exception.arguments[1],
            (unsigned long long)exception.arguments[2],(unsigned long long)exception.arguments[3],
            unsigned(exception.request.spurs),unsigned(exception.request.wid),unsigned(exception.request.handler),unsigned(exception.request.argument),
            int(exception.has_domain_result),unsigned(domain.stop),unsigned(domain.has_sdk_result),unsigned(domain.sdk_result),unsigned(domain.slot));
        std::printf("reads=%zu writes=%zu written_bytes=%zu claim_attempts=%zu conditional_attempts=%zu conditional_failures=%zu release_fences=%zu claim_written=%u argument_written=%u handler_written=%u handler_cleared=%u observer_calls=%zu domain_publications=%zu hle_publications=%zu has_result=%d result=0x%08x toc_written=%d memory_status=%d trace_failure=%d latched_stop=%d callback_invoked=false callback_delivery_supported=false readiness_write=false\n",
            size_t(domain.reads),size_t(domain.writes),size_t(domain.written_bytes),size_t(domain.claim_attempts),
            size_t(domain.conditional_attempts),size_t(domain.conditional_failures),size_t(domain.release_fences),
            unsigned(domain.claim_written),unsigned(domain.argument_written),unsigned(domain.handler_written),unsigned(domain.handler_cleared),
            size_t(domain.observer_calls),size_t(domain.publications),size_t(exception.publications),int(exception.has_result),
            unsigned(exception.result),int(exception.toc_written),int(exception.memory_status),int(exception.trace_failure),int(exception.latched_stop));
    }
    {
        const auto &ready=state->control->spurs_ready;
        const auto &domain=ready.domain_result;
        std::printf("STARTUP_SPURS_READY_COUNT_RESULT calls=%zu returns=%zu raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx spurs=0x%08x wid=%u value=%u has_domain_result=%d native_stop=%u domain_has_sdk_result=%u domain_sdk_result=0x%08x ready_address=0x%08x reads=%zu writes=%zu written_bytes=%zu release_fences=%zu store_attempted=%u stored=%u observer_calls=%zu domain_publications=%zu hle_publications=%zu has_result=%d result=0x%08x toc_written=%d memory_status=%d trace_failure=%d latched_stop=%d manual_wake=false game_execution_claim=false\n",
            size_t(state->control->spurs_ready_calls),size_t(state->control->spurs_ready_returns),
            (unsigned long long)ready.arguments[0],(unsigned long long)ready.arguments[1],(unsigned long long)ready.arguments[2],
            unsigned(ready.request.spurs),unsigned(ready.request.wid),unsigned(ready.request.value),int(ready.has_domain_result),
            unsigned(domain.stop),unsigned(domain.has_sdk_result),unsigned(domain.sdk_result),unsigned(domain.ready_address),
            size_t(domain.reads),size_t(domain.writes),size_t(domain.written_bytes),size_t(domain.release_fences),
            unsigned(domain.store_attempted),unsigned(domain.stored),size_t(domain.observer_calls),size_t(domain.publications),
            size_t(ready.publications),int(ready.has_result),unsigned(ready.result),int(ready.toc_written),
            int(ready.memory_status),int(ready.trace_failure),int(ready.latched_stop));
    }
    {
        const auto &info=state->control->spurs_workload_info;
        const auto &domain=info.domain_result;
        std::printf("STARTUP_SPURS_WORKLOAD_INFO_RESULT calls=%zu returns=%zu raw_r3=0x%016llx raw_r4=0x%016llx raw_r5=0x%016llx spurs=0x%08x wid=%u info=0x%08x has_domain_result=%d native_stop=%u domain_has_sdk_result=%u domain_sdk_result=0x%08x reads=%zu writes=%zu written_bytes=%zu write_attempts=%zu observer_calls=%zu domain_publications=%zu hle_publications=%zu has_result=%d result=0x%08x toc_written=%d memory_status=%d trace_failure=%d latched_stop=%d\n",
            size_t(state->control->spurs_workload_info_calls),size_t(state->control->spurs_workload_info_returns),
            (unsigned long long)info.arguments[0],(unsigned long long)info.arguments[1],(unsigned long long)info.arguments[2],
            unsigned(info.request.spurs),unsigned(info.request.wid),unsigned(info.request.info),int(info.has_domain_result),
            unsigned(domain.stop),unsigned(domain.has_sdk_result),unsigned(domain.sdk_result),
            size_t(domain.reads),size_t(domain.writes),size_t(domain.written_bytes),size_t(domain.write_attempts),
            size_t(domain.observer_calls),size_t(domain.publications),size_t(info.publications),int(info.has_result),
            unsigned(info.result),int(info.toc_written),int(info.memory_status),int(info.trace_failure),int(info.latched_stop));
    }
    if(state->control->spurs_fence_latched && state->control->spurs_fence.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT) {
        const auto &fence=state->control->spurs_fence;
        std::printf("STARTUP_SPURS_HELPER_EVENT_FENCE instance=%u spurs=0x%08x queue=0x%08x source=0x%016llx data1=0x%016llx data2=0x%016llx data3=0x%016llx received_dequeued=true delivery_unsupported=true registration_state=not_sampled guest_exception_claim=false\n",
            unsigned(fence.instance),unsigned(fence.spurs_ea),unsigned(fence.queue),(unsigned long long)fence.event.source,
            (unsigned long long)fence.event.data1,(unsigned long long)fence.event.data2,(unsigned long long)fence.event.data3);
    }
    if(state->control->spurs_fence_latched && state->control->spurs_fence.category==PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE) {
        const auto &fence=state->control->spurs_fence;
        std::printf("STARTUP_SPURS_HELPER_RECEIVE_FAILURE_FENCE instance=%u spurs=0x%08x queue=0x%08x receive_status=0x%08x has_event=%u received_dequeued=false callback_invoked=false\n",
            unsigned(fence.instance),unsigned(fence.spurs_ea),unsigned(fence.queue),unsigned(fence.receive_status),unsigned(fence.has_event));
    }
    // SPURS taskset setup block, the two pure struct-init entries. Counters and
    // last-result fields are unconditional StartupControl members appended in startup_execution.h; the print lives in
    // this block because both dispatches are served inside PS3_NATIVE_STARTUP_SPURS_INITIALIZE.
    // g13 appends the block's creation entry:
    // create_calls counts every cellSpursCreateTasksetWithAttribute entry, create_last its r3 (0 on success,
    // the firmware's code on a refusal) and create_wid the workload id the native registration claimed.
    // g14 appends the next entry on the same path,
    // _cellSpursLFQueueInitialize: lfq_calls counts every entry, lfq_last its r3 (0 on success, the
    // firmware's code on a refusal), lfq_returns how many produced one, lfq_writes the guest bytes the
    // adapter wrote and lfq_shape 1 for the CellSpursLFQueue shape / 0 for the CellSyncLFQueue one.
    // g14b/b1 adds the next entry on the same path, cellSpursLFQueueAttachLv2EventQueue (NID 0x1656D49F,
    // g14b/b1): lfqa_calls counts every entry and lfqa_last its r3, which must
    // be 0 on the game's own arguments -- a non-zero r3 is a fatal guest assert at 0x00452290.
    std::printf("STARTUP_SPURS_TASKSET_INIT attr_calls=%zu attr_returns=%zu attr_last=0x%08x evflag_calls=%zu evflag_returns=%zu evflag_last=0x%08x name_calls=%zu name_last=0x%08x attach_calls=%zu attach_last=0x%08x create_calls=%zu create_last=0x%08x create_wid=%u lfq_calls=%zu lfq_last=0x%08x lfqa_calls=%zu lfqa_last=0x%08x task_calls=%zu task_last=0x%08x task_id=%u\n",
        size_t(state->control->taskset_attr_init_calls),size_t(state->control->taskset_attr_init_returns),
        unsigned(state->control->taskset_attr_init_last),size_t(state->control->event_flag_init_calls),
        size_t(state->control->event_flag_init_returns),unsigned(state->control->event_flag_init_last),
        size_t(state->control->taskset_name_calls),unsigned(state->control->taskset_name_last),
        size_t(state->control->event_flag_attach_calls),unsigned(state->control->event_flag_attach_last),
        size_t(state->control->taskset_create_calls),unsigned(state->control->taskset_create_last),
        unsigned(state->control->taskset_create_wid),
        size_t(state->control->lfq_calls),unsigned(state->control->lfq_last),
        size_t(state->control->lfqa_calls),unsigned(state->control->lfqa_last),
        size_t(state->control->task_calls),unsigned(state->control->task_last),
        unsigned(state->control->task_id));
#endif
    {
        const auto &boot = state->control->boot_disc;
        std::printf("STARTUP_BOOT_DISC_RESULT calls=%zu returns=%zu raw_output=0x%016llx output=0x%08x metadata_checked=%d origin=%u title_id=%.10s parental_level=%u failure=%u write_attempts=%zu writes=%zu written_bytes=%zu publications=%zu toc_written=%d has_result=%d result=0x%08x memory_status=%d\n",
            size_t(state->control->boot_disc_calls), size_t(state->control->boot_disc_returns),
            (unsigned long long)boot.raw_output, unsigned(boot.output), int(boot.metadata_checked),
            unsigned(boot.metadata.origin), boot.metadata.title_id, unsigned(boot.metadata.parental_level),
            unsigned(boot.failure), boot.output_write_attempts, boot.output_writes, boot.output_written_bytes,
            boot.publications, int(boot.toc_written), int(boot.has_result), unsigned(boot.result), int(boot.memory_status));
    }
    {
        const auto &parameter=state->control->system_parameter;
        std::printf("STARTUP_SYSTEM_PARAMETER_RESULT calls=%zu returns=%zu raw_id=0x%016llx raw_output=0x%016llx id=0x%x effective_id=0x%x output=0x%08x process_sdk_valid=%d process_sdk=0x%08x settings_checked=%d language_status=%u language=%u profile=%u legacy_projection=%d value=%u failure=%u output_write_attempts=%zu output_written=%d publications=%zu toc_written=%d has_result=%d result=0x%08x memory_status=%d\n",
            size_t(state->control->system_parameter_calls),size_t(state->control->system_parameter_returns),
            (unsigned long long)parameter.raw_id,(unsigned long long)parameter.raw_output,
            unsigned(parameter.id),unsigned(parameter.effective_id),unsigned(parameter.output),
            int(parameter.process_sdk.valid),unsigned(parameter.process_sdk.raw),int(parameter.settings_checked),
            unsigned(parameter.settings.language.status),unsigned(parameter.settings.language.language),unsigned(parameter.settings.profile),
            int(parameter.legacy_projection),unsigned(parameter.value),unsigned(parameter.failure),
            parameter.output_write_attempts,int(parameter.output_written),parameter.publications,int(parameter.toc_written),
            int(parameter.has_result),unsigned(parameter.result),int(parameter.memory_status));
    }
    {
        const auto &text=state->control->system_string;
        std::printf("STARTUP_SYSTEM_STRING_RESULT calls=%zu returns=%zu raw_id=0x%016llx raw_output=0x%016llx raw_size=0x%016llx id=0x%x output=0x%08x size=%u profile_checked=%d text_checked=%d source=%u profile_status=%u text_length=%zu failure=%u output_write_attempts=%zu output_written_bytes=%zu publications=%zu toc_written=%d has_result=%d result=0x%08x memory_status=%d\n",
            size_t(state->control->system_string_calls),size_t(state->control->system_string_returns),
            (unsigned long long)text.raw_id,(unsigned long long)text.raw_output,(unsigned long long)text.raw_size,
            unsigned(text.id),unsigned(text.output),unsigned(text.size),int(text.profile_checked),int(text.text_checked),unsigned(text.source),
            unsigned(text.profile_status),text.text_length,unsigned(text.failure),text.output_write_attempts,
            text.output_written_bytes,text.publications,int(text.toc_written),int(text.has_result),unsigned(text.result),int(text.memory_status));
    }
    {
        const auto &disc=state->control->disc_change;
        ps3_disc_change_pair current{};
        const auto snapshot_status=ps3_disc_change_read(&disc_change->registry,&current);
        if (snapshot_status!=PS3_DISC_CHANGE_OK) valid_report=false;
        std::printf("STARTUP_DISC_CHANGE_RESULT calls=%zu returns=%zu raw_eject=0x%016llx raw_insert=0x%016llx attempted=%d status=%u committed=%u previous_eject=0x%08x previous_insert=0x%08x registered_eject=0x%08x registered_insert=0x%08x snapshot_status=%u current_eject=0x%08x current_insert=0x%08x publications=%zu toc_written=%d has_result=%d memory_status=%d\n",
            size_t(state->control->disc_change_calls),size_t(state->control->disc_change_returns),
            (unsigned long long)disc.raw_eject,(unsigned long long)disc.raw_insert,int(disc.registration_attempted),
            unsigned(disc.registration.status),unsigned(disc.registration.committed),
            unsigned(disc.registration.previous.eject_descriptor),unsigned(disc.registration.previous.insert_descriptor),
            unsigned(disc.registration.current.eject_descriptor),unsigned(disc.registration.current.insert_descriptor),
            unsigned(snapshot_status),unsigned(current.eject_descriptor),unsigned(current.insert_descriptor),
            disc.publications,int(disc.toc_written),int(disc.has_result),int(disc.memory_status));
    }
    {
        const auto &video=state->control->video_output_count;
        std::printf("STARTUP_VIDEO_OUTPUT_COUNT_RESULT calls=%zu returns=%zu raw_video_out=0x%016llx video_out=%u topology=%u inventory_status=%u inventory_checked=%d count=%d failure=%u publications=%zu toc_written=%d has_result=%d memory_status=%d\n",
            size_t(state->control->video_output_count_calls),size_t(state->control->video_output_count_returns),
            (unsigned long long)video.raw_video_out,unsigned(video.video_out),unsigned(video.topology),
            unsigned(video.inventory_status),int(video.inventory_checked),int(video.count),unsigned(video.failure),video.publications,
            int(video.toc_written),int(video.has_result),int(video.memory_status));
    }
    {
        const auto &video=state->control->video_state;
        std::printf("STARTUP_VIDEO_STATE_RESULT calls=%zu returns=%zu caller=%u failure=%u lr=0x%llx output=0x%08x publications=%zu\n",
            size_t(state->control->video_state_calls),size_t(state->control->video_state_returns),
            unsigned(video.caller),unsigned(video.failure),(unsigned long long)video.lr,
            video.output,video.publications);
    }
    {
        const auto &video=state->control->video_configure;
        char record_hex[33];
        for(size_t i=0;i<16;++i) std::snprintf(record_hex+i*2,3,"%02x",unsigned(video.record[i]));
        record_hex[32]='\0';
        std::printf("STARTUP_VIDEO_CONFIGURE_RESULT calls=%zu returns=%zu failure=%u lr=0x%llx r3=0x%llx r4=0x%llx r5=0x%llx r6=0x%llx record=%s configured=%d publications=%zu toc_written=%d has_result=%d game_picture=false\n",
            size_t(state->control->video_configure_calls),size_t(state->control->video_configure_returns),
            unsigned(video.failure),(unsigned long long)video.lr,(unsigned long long)video.raw_r3,
            (unsigned long long)video.raw_r4,(unsigned long long)video.raw_r5,(unsigned long long)video.raw_r6,
            record_hex,int(state->control->video_logical_target_configured),video.publications,
            int(video.toc_written),int(video.has_result));
    }
#ifdef PS3_NATIVE_GCM_INIT
    report_gcm_init(*state->control);
    report_gcm_query(*state->control, &gcm_init_storage);
    report_gcm_local_memory();
#endif
    std::printf("STARTUP_DISCOVERY_MEMORY published=%llu retained=%zu retained_interval=[%llu,%llu) overwritten=[%llu,%llu) interval_valid=%d\n",
        (unsigned long long)memory.published_count, memory.retained_count,
        (unsigned long long)retained_begin, (unsigned long long)retained_end,
        (unsigned long long)memory.overwritten_begin, (unsigned long long)memory.overwritten_end, retained_sum_valid);
    for (size_t index = 0; index < memory.retained_count; ++index) {
        PpuMemoryTraceSample sample{};
        if (!state->recorder.read(index, sample)) { valid_report = false; break; }
        std::printf("DISCOVERY_ACCESS sequence=%llu origin=%d operation=%d address=0x%016llx length=%zu value=0x%016llx status=%d stored=%d byte_transfer=%d\n",
            (unsigned long long)sample.sequence, int(sample.event.origin), int(sample.event.operation),
            (unsigned long long)sample.event.address, size_t(sample.event.length), (unsigned long long)sample.event.value,
            int(sample.event.status), sample.event.stored, sample.event.byte_transfer);
        if(sample.event.byte_transfer) {
            if(sample.event.length==0 || sample.event.length>PPU_MEMORY_TRACE_BYTE_CAPACITY) { valid_report=false; break; }
            char hexadecimal[2*PPU_MEMORY_TRACE_BYTE_CAPACITY+1]{};
            constexpr char digits[]="0123456789abcdef";
            for(size_t byte=0;byte<sample.event.length;++byte) {
                hexadecimal[2*byte]=digits[sample.event.bytes[byte]>>4];
                hexadecimal[2*byte+1]=digits[sample.event.bytes[byte]&15];
            }
            printf("DISCOVERY_ACCESS_BYTES sequence=%llu data=%s\n",
                static_cast<unsigned long long>(sample.sequence),hexadecimal);
        }
    }
    std::printf("STARTUP_DISCOVERY_OBSERVATION imports_total=%llu imports_retained=[%llu,%llu) imports_overwritten=[0,%llu) completed_total=%llu completed_retained=[%llu,%llu) completed_overwritten=[0,%llu) started=%llu pending=%zu\n",
        (unsigned long long)observation.imports.total, (unsigned long long)observation.imports.retained_begin,
        (unsigned long long)observation.imports.retained_end, (unsigned long long)observation.imports.overwritten_end,
        (unsigned long long)observation.completed.total, (unsigned long long)observation.completed.retained_begin,
        (unsigned long long)observation.completed.retained_end, (unsigned long long)observation.completed.overwritten_end,
        (unsigned long long)observation.dispatch_started, observation.pending_count);
    for (uint64_t sequence = observation.imports.retained_begin; sequence < observation.imports.retained_end; ++sequence) {
        infamous::diagnostics::ImportRecord record{};
        if (!state->observation.read_import(sequence, record)) { valid_report = false; break; }
        std::printf("DISCOVERY_IMPORT sequence=%llu nid=0x%08x\n", (unsigned long long)record.sequence, record.nid);
    }
#ifdef PS3_NATIVE_STARTUP_SPURS_ATTRIBUTES
    if (state->control->stop == StartupStop::UnknownImport &&
        observation.imports.retained_end > observation.imports.retained_begin) {
        infamous::diagnostics::ImportRecord next_import{};
        if (!state->observation.read_import(observation.imports.retained_end - 1, next_import)) valid_report = false;
        else {
            std::printf("STARTUP_NEXT_UNKNOWN_IMPORT module_admission=assumed game_boot=false nid=0x%08x lr=0x%016llx service_executed=false\n",
                unsigned(next_import.nid), (unsigned long long)terminal.lr);
            for (unsigned argument = 3; argument <= 10; ++argument)
                std::printf("NEXT_IMPORT_RAW_ARGUMENT r%u=0x%016llx\n", argument, (unsigned long long)terminal.gpr[argument]);
            constexpr uint32_t spurs_initialize_with_attribute_nid = 0xaa6269a8;
            if (next_import.nid == spurs_initialize_with_attribute_nid) {
                constexpr size_t instance_probe_limit = 4096;
                const uint32_t instance = uint32_t(terminal.gpr[3]);
                uint64_t scope_remaining = 0;
                bool scope_writable = false;
                for (size_t range_index = 0; range_index < binding.ram_count; ++range_index) {
                    const auto &range = binding.ram[range_index];
                    if (instance >= range.address && uint64_t(instance) - range.address < range.length) {
                        const uint64_t remaining = range.length - (uint64_t(instance) - range.address);
                        if (remaining > scope_remaining) { scope_remaining = remaining; scope_writable = range.writable; }
                    }
                }
                const uint32_t captured_attribute = uint32_t(attribute_result.arguments[0]);
                const bool aliases_attribute = state->control->spurs_attribute_calls &&
                    uint64_t(instance) < uint64_t(captured_attribute) + PS3_SPURS_ATTRIBUTE_SIZE &&
                    uint64_t(captured_attribute) < uint64_t(instance) + instance_probe_limit;
                size_t checked = 0;
                const size_t limit = scope_remaining < instance_probe_limit ? size_t(scope_remaining) : instance_probe_limit;
                bool probe_attempted = false;
                ps3_guest_memory_status probe_status = PS3_GUEST_MEMORY_INVALID_RANGE;
                while (!aliases_attribute && checked < limit) {
                    unsigned char probe[64];
                    const size_t length = limit - checked < sizeof(probe) ? limit - checked : sizeof(probe);
                    probe_status = binding.coherent_memory.read(binding.coherent_memory.context,
                        instance + uint32_t(checked), probe, length);
                    probe_attempted = true;
                    if (probe_status != PS3_GUEST_MEMORY_OK) break;
                    checked += length;
                }
                std::printf("STARTUP_PENDING_SPURS_INSTANCE scope=postjoin_debug_read_only raw_argument=0x%016llx decoded_address=0x%08x permission_scope_remaining=%llu scope_writable=%d probe_limit=%zu probe_attempted=%d readable_prefix=%zu provider_status=%d suppressed_attribute_alias=%d resource_creation=false executor_available=false\n",
                    (unsigned long long)terminal.gpr[3], unsigned(instance), (unsigned long long)scope_remaining,
                    int(scope_writable), instance_probe_limit, int(probe_attempted), checked, int(probe_status), int(aliases_attribute));
            }
        }
    }
#endif
    for (uint64_t sequence = observation.completed.retained_begin; sequence < observation.completed.retained_end; ++sequence) {
        infamous::diagnostics::CompletedDispatch record{};
        if (!state->observation.read_completed(sequence, record)) { valid_report = false; break; }
        std::printf("DISCOVERY_COMPLETED sequence=%llu id=%llu parent=%llu ctr=0x%016llx lr=0x%016llx sp=0x%016llx depth=%u depth_after=%u status=%u lookup_attempted=%d lookup_status=%u\n",
            (unsigned long long)record.sequence, (unsigned long long)record.call_id, (unsigned long long)record.parent_id,
            (unsigned long long)record.entry.captured_ctr, (unsigned long long)record.entry.lr,
            (unsigned long long)record.entry.sp, record.entry.depth, record.depth_after, record.result.status,
            record.result.lookup_attempted, record.result.lookup_status);
    }
    if (observation.pending_count > observation.pending_capacity) valid_report = false;
    for (size_t depth = 0; depth < observation.pending_count && depth < observation.pending_capacity; ++depth) {
        infamous::diagnostics::PendingDispatch record{};
        if (!state->observation.read_pending(depth, record)) { valid_report = false; break; }
        if (!record.occupied) { valid_report = false; break; }
        std::printf("DISCOVERY_PENDING id=%llu parent=%llu ctr=0x%016llx lr=0x%016llx sp=0x%016llx depth=%u\n",
            (unsigned long long)record.call_id, (unsigned long long)record.parent_id,
            (unsigned long long)record.entry.captured_ctr, (unsigned long long)record.entry.lr,
            (unsigned long long)record.entry.sp, record.entry.depth);
    }
    for (unsigned index = 0; index < 32; ++index)
        std::printf("DISCOVERY_REGISTER r%u=0x%016llx\n", index, (unsigned long long)terminal.gpr[index]);
    const auto *cpu_bytes = reinterpret_cast<const unsigned char *>(&terminal);
    for (size_t offset = 0; offset < sizeof(terminal); offset += 32) {
        std::printf("DISCOVERY_CPU_BYTES offset=%zu data=", offset);
        for (size_t index = offset; index < sizeof(terminal) && index < offset + 32; ++index)
            std::printf("%02x", cpu_bytes[index]);
        std::putchar('\n');
    }
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    {   // Every SPURS service/worker is cancelled, woken and CONFIRMED joined before anything it borrows is
        // touched. ps3_spurs_runtime_shutdown returns the number of instances NOT torn down (0 == success);
        // any nonzero count makes the release BUSY and releases nothing: the graph is then retained.
        const ps3_guest_memory_status release_status=native_startup_release_after_join(*state->owner);
        std::printf("STARTUP_SPURS_SHUTDOWN shutdown_zero_means_success=true release_status=%d released=%d guest_memory_released=false graph_retained_until_process_exit=true\n",
            int(release_status),int(release_status==PS3_GUEST_MEMORY_OK));
        if(release_status!=PS3_GUEST_MEMORY_OK) {
            std::puts("STARTUP_SPURS_SHUTDOWN_FAILED services_not_joined=true nothing_released=true borrowed_graph_retained=true");
            std::fflush(stdout);
            svcExitProcess();
        }
        for(unsigned instance=0;instance<PS3_SPURS_MAX_INSTANCES;++instance) {
            const auto &capture=spurs.composition.runtime.helper_stops[instance];
            std::printf("STARTUP_SPURS_HELPER_STOP_JOINED instance=%u captured=%u category=%u timing=after_successful_runtime_shutdown callback_delivery_supported=false\n",instance,unsigned(capture.ready!=0),unsigned(capture.category));
            if(capture.ready && capture.category==PS3_SPURS_STOP_CATEGORY_HELPER_EVENT) std::printf("STARTUP_SPURS_HELPER_EVENT_PAYLOAD instance=%u spurs=0x%08x queue=0x%08x source=0x%016llx data1=0x%016llx data2=0x%016llx data3=0x%016llx received_dequeued=true delivery_unsupported=true registration_state=not_sampled guest_exception_claim=false\n",
                instance,unsigned(capture.spurs_ea),unsigned(capture.queue),(unsigned long long)capture.event.source,
                (unsigned long long)capture.event.data1,(unsigned long long)capture.event.data2,(unsigned long long)capture.event.data3);
            if(capture.ready && capture.category==PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE) std::printf("STARTUP_SPURS_HELPER_RECEIVE_FAILURE_PAYLOAD instance=%u spurs=0x%08x queue=0x%08x receive_status=0x%08x has_event=false received_dequeued=false callback_invoked=false\n",
                instance,unsigned(capture.spurs_ea),unsigned(capture.queue),unsigned(capture.receive_status));
        }
        // Adapter state is sampled only after every borrowing kernel/handler has joined.
        for(unsigned instance=0;instance<PS3_SPURS_MAX_INSTANCES;++instance)
            for(unsigned spu_index=0;spu_index<PS3_STARTUP_WWS_LANES;++spu_index) {
                ps3_startup_wws_diagnostics lane{};
                if(ps3_startup_wws_read_joined(spurs.workload_adapter[instance],spu_index,&lane)!=PS3_SPURS_POLICY_READY)
                    native_budget_fatal("wws-joined-diagnostics");
                std::printf("STARTUP_WWS_EXECUTION_JOINED instance=%u spu=%u adapter_initialized=%u active=%u entered=%u retained=%u status=%u pc=0x%08x channel=%u stop_code=%u hops=%llu resumable=%u terminal_code=%d provider_status=%d effect_committed=%u timing=after_successful_runtime_shutdown transfer_trace=disabled\n",
                    instance,spu_index,unsigned(lane.initialized),unsigned(lane.active),unsigned(lane.entered),
                    unsigned(lane.retained),unsigned(lane.last.status),unsigned(lane.last.pc),unsigned(lane.last.channel),
                    unsigned(lane.last.stop_code),(unsigned long long)lane.last.hops,unsigned(lane.last.resumable),
                    int(lane.terminal_code),int(lane.provider_status),unsigned(lane.effect_committed));
            }
    }
#endif
    // The completion record is terminal for the accepted runner: with N12 enabled it is published only AFTER every
    // SPURS service joined and shutdown succeeded (a shutdown failure above exits without ever publishing it).
    std::printf("STARTUP_DISCOVERY_REPORT_COMPLETE module_admission=assumed game_boot=false valid_report=%d joined=true graph_retained_until_process_exit=true correctness_or_gameboot_claim=false\n", valid_report);
    const AppletType exit_applet_type = appletGetAppletType();
    if (exit_applet_type != AppletType_Application && exit_applet_type != AppletType_SystemApplication &&
        exit_applet_type != AppletType_LibraryApplet) {
        std::fflush(stdout);
        svcExitProcess();
    }
    // Mode1 asks libnx to terminate the application process, preserving the
    // retained graph lifetime instead of returning to hbloader in this process.
    constexpr uint32_t sdk_applet_process_exit_mode = 1;
    __nx_applet_exit_mode = sdk_applet_process_exit_mode;
    std::fflush(stdout);
    ::_exit(0);
}
#endif
#endif

extern "C" int native_test_startup_indirect(const ps3_guest_memory *backing, const char *path)
{
    PpuCompiledCodeTable compiled_table;
    if (function_table_count != PPU_COMPILED_CODE_INDEX_COUNT ||
        compiled_table.admit(function_table, size_t(function_table_count),
            ppu_compiled_code_sorted_offsets, PPU_COMPILED_CODE_INDEX_COUNT) != PS3_COMPILED_CODE_OK) {
        std::puts("INDIRECT_TABLE_ADMISSION_FAIL");
        return 1;
    }
#ifdef PS3_NATIVE_LLE_SRE
    // Decision 33: the game's table first, then Sony's lifted libsre. Every indirect call the lifted
    // code makes through a guest function pointer resolves through this one port.
    const ps3_compiled_code_lookup compiled_lookup =
        lle_sre_chained_compiled_code_lookup(compiled_table.port());
#else
    const ps3_compiled_code_lookup compiled_lookup = compiled_table.port();
#endif
    std::printf("INDIRECT_TABLE_ADMISSION_PASS entries=%llu table_bytes=%zu index_bytes=%zu\n",
        (unsigned long long)function_table_count, size_t(function_table_count) * sizeof(func_entry),
        sizeof(uint32_t) * size_t(PPU_COMPILED_CODE_INDEX_COUNT));
#ifdef PS3_NATIVE_STARTUP_DISCOVERY
    int failures = 0;
    // Keep retained fixture helpers well-formed without executing them.
    (void)&test_oracle_memory_encoding;
    (void)&snapshot;
#else
    int failures = test_oracle_memory_encoding();
    failures += native_clock_conversion_tests() != 0;
    failures += native_test_ppu_memory_trace() != 0;
    failures += native_test_clock_horizon() != 0;
#endif
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
#ifdef PS3_NATIVE_GCM_INIT
    {
        // Fixed-window admission for the commit-on-demand local range: refuse rather than
        // move it if anything already owns part of [GCM_LOCAL_BASE, GCM_LOCAL_BASE + size).
        bool local_conflict = false;
        for (size_t index = 0; index < plan.segment_count; ++index)
            if (gcm_local_window_overlaps(segments[index].guest_address, segments[index].memory_size)) local_conflict = true;
        if (gcm_local_window_overlaps(STACK_START, STACK_BYTES) || gcm_local_window_overlaps(ARGV, 4096) ||
            gcm_local_window_overlaps(TLS_BLOCK, 0x10000) || gcm_local_window_overlaps(0x40000000, 0x10000000) ||
            gcm_local_window_overlaps(GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE))
            local_conflict = true;
        if (local_conflict) {
            std::printf("STARTUP_GCM_LOCAL_CONFIGURATION_CONFLICT base=0x%08x size=0x%x moved=false\n",
                GCM_LOCAL_BASE, GCM_LOCAL_SIZE);
            std::fflush(stdout);
            return 1;
        }
    }
    startup_rsx_capture_backing = *backing;   // root: the raw backing below the on-demand window, for the frame capture
    // Root diagnostic (2026-11-12): who writes the 2D projection matrix at 0x917910 (its last row is zero where
    // (0,0,0,1) is expected; evidence/rsx-capture-r34). Printed as STARTUP_WRITE_WATCH.
    g_ppu_write_watch_base = 0;   // off: the matrix question is answered (Decision 56)
    g_ppu_write_watch_size = 0;
    startup_rsx_capture.raw_backing = &startup_rsx_capture_backing;
    {
        char capture_path[320];
        std::snprintf(capture_path, sizeof capture_path, "%s/capture-at.txt", STARTUP_FS_ROOT);
        if (std::FILE *capture_file = std::fopen(capture_path, "r")) {
            unsigned wanted = 0;
            if (std::fscanf(capture_file, "%u", &wanted) == 1 && wanted) startup_rsx_capture.at_flip = wanted;
            std::fclose(capture_file);
        }
    }
    g_hb_window_mutex = &gcm_local_window_mutex;
    startup_rsx_capture.gcm = &gcm_init_storage;
    if (ps3_demand_window_init(&gcm_local_window_storage, *backing,
            ps3_instance_guard{&gcm_local_window_mutex, lock_domain, unlock_domain},
            GCM_LOCAL_BASE, GCM_LOCAL_SIZE, GCM_LOCAL_GRANULE, GCM_LOCAL_CAP) != PS3_GUEST_MEMORY_OK)
        native_budget_fatal("gcm-local-window-init");
#endif
    Mutex domain_mutex{};
    ps3_atomic_domain domain{};
    g_hb_domain_mutex = &domain_mutex;
    g_hb_domain = &domain;
    // Capacities come from the shared startup capacity profile (owners = primary + SPU + child slots; pins unchanged).
    ps3_atomic_owner_record owners[STARTUP_ATOMIC_OWNER_CAPACITY]{};
    ps3_atomic_pin_record pins[STARTUP_ATOMIC_PIN_CAPACITY]{};
    const ps3_instance_guard guard{&domain_mutex, lock_domain, unlock_domain};
#ifdef PS3_NATIVE_GCM_INIT
    // On-demand local memory decorates the same backing the domain would otherwise use.
    ps3_guest_memory domain_backing = ps3_demand_window_port(&gcm_local_window_storage);
#else
    ps3_guest_memory domain_backing = *backing;
#endif
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
    TimebaseMemoryAudit timebase_audit{domain_backing};
    const ps3_guest_memory audited_backing{&timebase_audit, frequency_commit,
        frequency_decommit, frequency_read, frequency_write, frequency_zero};
    CHECK(ps3_atomic_domain_init(&domain, audited_backing, guard, owners, STARTUP_ATOMIC_OWNER_CAPACITY, pins, STARTUP_ATOMIC_PIN_CAPACITY) == PS3_GUEST_MEMORY_OK);
#else
    CHECK(ps3_atomic_domain_init(&domain, domain_backing, guard, owners, STARTUP_ATOMIC_OWNER_CAPACITY, pins, STARTUP_ATOMIC_PIN_CAPACITY) == PS3_GUEST_MEMORY_OK);
#endif
    ps3_guest_memory coherent = ps3_atomic_domain_memory(&domain);
    const ps3_guest_memory *memory = &coherent;
    ps3_horizon_lwmutex_wait wait{};
    CHECK(ps3_horizon_lwmutex_wait_init(&wait, 2000000) == PS3_WAIT_READY);
    Mutex allocator_mutex{};
    ps3_user_memory allocator{};
    // Decision 91: 1024 slots (was 64). Hardware run rv22 stopped at the 65th live sys_memory_allocate block
    // (stop=12, allocator status CAPACITY_EXCEEDED) while the game was still loading.
    static ps3_user_memory_slot allocation_slots[1024]{};
    const ps3_user_memory_config allocation_config{
        UINT64_C(0x40000000), UINT64_C(0x50000000), UINT64_C(213) * 1024 * 1024,
        coherent, {&allocator_mutex, lock_domain, unlock_domain}};
    CHECK(ps3_user_memory_init(&allocator, &allocation_config, allocation_slots, 1024) == PS3_USER_MEMORY_OK);
    ps3_user_memory_service memory_service{&allocator, &domain};
    auto spu_storage=std::make_unique<NativeSpuResourceComposition>();
    auto &spu=*spu_storage;
    if(ps3_spu_resource_policy_init(&spu.policy,{&spu.mutex,lock_domain,unlock_domain})!=PS3_SPU_POLICY_OK)
        native_budget_fatal("spu-resource-policy-init");
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
    // N12 stable caller-owned storage, every allocation checked; composed (and injected into the
    // StartupExecution) only inside run_startup_discovery after admission supplies the real main id.
    std::unique_ptr<NativeSpursComposition> spurs_storage(new (std::nothrow) NativeSpursComposition);
    if(!spurs_storage) native_budget_fatal("spurs-composition-allocation");
    std::unique_ptr<NativeChildComposition> child_storage(new (std::nothrow) NativeChildComposition);
    if(!child_storage) native_budget_fatal("child-composition-allocation");
    // Every child slot's dedicated host thread (NOT a SPURS thread) is initialized here, BEFORE the parent can start. ps3_horizon_service_thread_init
    // only records the stack size (void, cannot fail); any later configure/resource failure is fail-closed before the parent starts.
    for(unsigned slot_index=0;slot_index<NativeChildComposition::slot_capacity;++slot_index)
        ps3_horizon_service_thread_init(&child_storage->slots[slot_index].thread,NativeChildComposition::host_stack_bytes);
    // Root (2026-11-15, Decision 119): rv59s STARTUP_LV2_TIMER: the 5.3 ms audio timer sent 223,140 events and only 36,765
    // reached the game (186,375 EBUSY: queue full), so the game's audio thread mixed ~1/6 of real time and every block
    // played as silence (STARTUP_AUDIO_OUT empty_pct=100). Like the inFAMOUS 2 port (its biggest audible win), guest threads
    // whose name says sound get host priority 0x3a, one step above the other guest threads. Every create is logged
    // (STARTUP_GUEST_THREAD) so the next run names the real mixer if it is not among them.
    g_startup_child_priority_hook = [](void *context, int32_t guest_priority, const char *name, uint32_t entry) {
        auto *thread = static_cast<ps3_horizon_service_thread *>(context);
        const char *const keys[] = {"snd", "Snd", "SND", "sound", "Sound", "audio", "Audio", "AUDIO", "mix", "Mix", "wwise", "Wwise", "AK::"};
        bool audio = false;
        for (const char *key : keys) if (name && std::strstr(name, key)) { audio = true; break; }
        if (thread && audio && thread->priority > 0x3a) thread->priority = 0x3a;
        std::printf("STARTUP_GUEST_THREAD name=\"%s\" guest_priority=%d entry=0x%08x host_priority=0x%x audio=%d\n",
            name ? name : "", int(guest_priority), unsigned(entry), thread ? unsigned(thread->priority) : 0u, int(audio));
    };
    {
        auto &s=*spurs_storage;
        s.local_stores.reset(new (std::nothrow) unsigned char[size_t(PS3_SPURS_LS_SLOTS)*PS3_SPURS_K1_LS_BYTES]());
        if(!s.local_stores) native_budget_fatal("spurs-local-store-allocation");
        ps3_horizon_wait_port_init(&s.wait_port);
        s.ports.notify=ps3_horizon_wait_port_port(&s.wait_port);
        s.ports.lv2_guard={&s.lv2_mutex,lock_domain,unlock_domain};
        s.ports.ppu_guard={&s.ppu_mutex,lock_domain,unlock_domain};
        for(unsigned instance=0;instance<PS3_SPURS_MAX_INSTANCES;++instance)
            for(unsigned role=0;role<3;++role) {
                ps3_horizon_service_thread_init(&s.threads[instance][role],
                    role==PS3_SPURS_THREAD_HELPER?s.helper_stack:role==PS3_SPURS_THREAD_HANDLER?s.handler_stack:s.kernel_stack);
                s.ports.threads[instance][role]=ps3_horizon_service_thread_port(&s.threads[instance][role]);
            }
        for(unsigned slot=0;slot<PS3_SPURS_LS_SLOTS;++slot)
            s.ports.local_stores[slot]=s.local_stores.get()+size_t(slot)*PS3_SPURS_K1_LS_BYTES;
    }
#endif
    auto sysutil_storage=std::make_unique<NativeSysutilCallbackComposition>();
    auto &sysutil=*sysutil_storage;
    CHECK(ps3_sysutil_callback_init(&sysutil.registry,{&sysutil.mutex,lock_domain,unlock_domain})==PS3_SYSUTIL_CALLBACK_OK);
    for(uint32_t slot=0;slot<PS3_SYSUTIL_CALLBACK_SLOT_COUNT;++slot) {
        ps3_sysutil_callback_pair initial_pair{};
        CHECK(ps3_sysutil_callback_read(&sysutil.registry,slot,&initial_pair)==PS3_SYSUTIL_CALLBACK_OK);
        CHECK(initial_pair.callback_descriptor_address==0 && initial_pair.userdata_address==0);
    }
    auto module_storage=std::make_unique<NativeDiagnosticModuleComposition>();
    auto &modules=*module_storage;
    const unsigned char expected_module_bytes[26]{0x00,0x0e,0x00,0x0a,0x00,0x17,0x00,0x09,
        0x00,0x00,0x00,0x14,0x00,0x11,0x00,0x15,0x00,0x33,0x00,0x10,0x00,0x1c,0xf0,0x35,0x00,0x16};
    unsigned char actual_module_bytes[sizeof(expected_module_bytes)]{};
    CHECK(memory->read(memory->context,0x5670b0,actual_module_bytes,sizeof(actual_module_bytes))==PS3_GUEST_MEMORY_OK);
    CHECK(!std::memcmp(actual_module_bytes,expected_module_bytes,sizeof(expected_module_bytes)));
    CHECK(ps3_diagnostic_module_init(&modules.state,{&modules.mutex,lock_domain,unlock_domain},
        modules.catalog,modules.catalog_count,modules.slots,modules.catalog_count)==PS3_DIAGNOSTIC_MODULE_OK);
    for(size_t module=0;module<modules.catalog_count;++module) {
        ps3_diagnostic_module_result initial_module{};
        CHECK(ps3_diagnostic_module_read(&modules.state,modules.catalog[module],&initial_module)==PS3_DIAGNOSTIC_MODULE_OK);
        CHECK(initial_module.current_count==0 && initial_module.total_admissions==0);
    }
    auto process_storage=std::make_unique<NativeProcessComposition>();
    auto &process=*process_storage;
    CHECK(ps3_exitspawn_init(&process.registry,{&process.mutex,lock_domain,unlock_domain})==PS3_EXITSPAWN_OK);
    ps3_exitspawn_snapshot initial_exitspawn{};
    CHECK(ps3_exitspawn_read(&process.registry,&initial_exitspawn)==PS3_EXITSPAWN_OK);
    CHECK(initial_exitspawn.atexitspawn==0 && initial_exitspawn.at_Exitspawn==0);
    auto condition_storage=std::make_unique<NativeConditionComposition>();
    auto &conditions=*condition_storage;
    auto &registry=conditions.mutexes;
    auto &objects=conditions.mutex_objects;
    auto &waiters=conditions.mutex_waiters;
    CHECK(ps3_lwmutex_init_with_conditions(&registry, &domain, ps3_horizon_lwmutex_wait_guard(&wait),
        ps3_horizon_lwmutex_wait_port(&wait), objects, NativeConditionComposition::mutex_object_capacity, waiters, NativeConditionComposition::mutex_waiter_capacity,
        NativeConditionComposition::binding_capacity).status == PS3_LWMUTEX_OK);
#ifdef PS3_NATIVE_FAST_SYNC
    // ppu-speed-20261113 item 1: lock-free lwmutex lock/unlock on the flat guest window (no-op without one: the pointer is then null,
    // no mutex is fast-eligible and the import arm keeps its domain reads/writes).
    {
        struct FastWindow { static unsigned char *pointer(void *, uint32_t address, uint32_t bytes) { return static_cast<unsigned char *>(ps3_flat_guest_fast_pointer(address, bytes)); } };
        CHECK(ps3_lwmutex_enable_fast_path(&registry, ps3_lwmutex_fast_port{nullptr, FastWindow::pointer}).status == PS3_LWMUTEX_OK);
        native_startup_enable_fast_sync(FastWindow::pointer, nullptr);
        std::puts("STARTUP_FAST_SYNC lwmutex lock-free path enabled (objects become eligible when the guest space is flat)");
    }
#endif
    CHECK(ps3_horizon_lwmutex_wait_init(&conditions.wait, 2000000) == PS3_WAIT_READY);
    CHECK(ps3_lwcond_init(&conditions.registry, &registry,
        ps3_horizon_lwmutex_wait_guard(&conditions.wait), ps3_horizon_lwmutex_wait_port(&conditions.wait),
        {&conditions,condition_thread_exists}, conditions.objects, conditions.object_capacity,
        conditions.waiters, conditions.waiter_capacity).status == PS3_LWCOND_OK);
    if (failures) return 1;
    Oracle oracle;
#ifndef PS3_NATIVE_STARTUP_DISCOVERY
    for (size_t index = 0; index < plan.segment_count; ++index) {
        if (segments[index].memory_size > 16 * 1024 * 1024 ||
            uint64_t(segments[index].guest_address) + segments[index].memory_size >= ARGV) return 1;
        if (!snapshot(memory, oracle, segments[index].guest_address, segments[index].memory_size,
                      (segments[index].flags & 2) != 0)) return 1;
    }
#endif
#ifdef PS3_NATIVE_GCM_INIT
    {
        // Fixed-window admission: refuse rather than move the window if anything already owns it.
        bool conflict = false;
        for (size_t index = 0; index < plan.segment_count; ++index)
            if (gcm_init_window_overlaps(segments[index].guest_address, segments[index].memory_size)) conflict = true;
        if (gcm_init_window_overlaps(STACK_START, STACK_BYTES) || gcm_init_window_overlaps(ARGV, 4096) ||
            gcm_init_window_overlaps(TLS_BLOCK, 0x10000) || gcm_init_window_overlaps(0x40000000, 0x10000000))
            conflict = true;
        if (conflict) {
            std::printf("STARTUP_GCM_INIT_CONFIGURATION_CONFLICT window=0x%08x size=0x%x moved=false\n",
                GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE);
            std::fflush(stdout);
            return 1;
        }
    }
#endif
#ifdef PS3_NATIVE_LLE_SRE
    {
        bool audio_conflict = false;
        for (size_t index = 0; index < plan.segment_count; ++index)
            if (audio_window_overlaps(segments[index].guest_address, segments[index].memory_size)) audio_conflict = true;
        if (audio_window_overlaps(STACK_START, STACK_BYTES) || audio_window_overlaps(ARGV, 4096) ||
            audio_window_overlaps(TLS_BLOCK, 0x10000) || audio_window_overlaps(0x40000000, 0x10000000))
            audio_conflict = true;
#ifdef PS3_NATIVE_GCM_INIT
        if (audio_window_overlaps(GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE) || audio_window_overlaps(GCM_LOCAL_BASE, GCM_LOCAL_SIZE))
            audio_conflict = true;
#endif
        if (audio_conflict) {
            std::printf("STARTUP_AUDIO_CONFIGURATION_CONFLICT window=0x%08x size=0x%x moved=false\n", AUDIO_WINDOW, AUDIO_WINDOW_SIZE);
            std::fflush(stdout);
            return 1;
        }
    }
#endif
    CHECK(memory->commit(memory->context, STACK_START, STACK_BYTES) == PS3_GUEST_MEMORY_OK);
    CHECK(memory->commit(memory->context, ARGV, 4096) == PS3_GUEST_MEMORY_OK);
#ifdef PS3_NATIVE_LLE_SRE
    CHECK(memory->commit(memory->context, AUDIO_WINDOW, AUDIO_WINDOW_SIZE) == PS3_GUEST_MEMORY_OK);
#endif
#ifdef PS3_NATIVE_GCM_INIT
    CHECK(memory->commit(memory->context, GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE) == PS3_GUEST_MEMORY_OK);
#endif
    std::vector<uint8_t> stack(STACK_BYTES, 0xa5), argv(4096, 0);
    CHECK(memory->write(memory->context, STACK_START, stack.data(), stack.size()) == PS3_GUEST_MEMORY_OK);
    const char boot_path[] = "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN";
    uint64_t pointer = __builtin_bswap64(uint64_t(ARGV + 0x20));
    std::memcpy(argv.data(), &pointer, 8);
    std::memcpy(argv.data() + 0x20, boot_path, sizeof(boot_path));
    CHECK(memory->write(memory->context, ARGV, argv.data(), argv.size()) == PS3_GUEST_MEMORY_OK);
#ifndef PS3_NATIVE_STARTUP_DISCOVERY
    CHECK(snapshot(memory, oracle, STACK_START, STACK_BYTES, true));
    CHECK(snapshot(memory, oracle, ARGV, 4096, true));
    const uint32_t code = uint32_t(oracle.read(plan.entry_opd, 4, false));
    const uint32_t toc = uint32_t(oracle.read(plan.entry_opd + 4, 4, false));
    CHECK(code == 0x10230 && toc == 0x78bf78 && oracle.valid);
#else
    uint32_t descriptor[2]{};
    CHECK(memory->read(memory->context, plan.entry_opd, descriptor, sizeof(descriptor)) == PS3_GUEST_MEMORY_OK);
    const uint32_t code = __builtin_bswap32(descriptor[0]);
    const uint32_t toc = __builtin_bswap32(descriptor[1]);
    CHECK(code == 0x10230 && toc == 0x78bf78);
#endif
    if (failures) return 1;
    std::vector<PpuMemoryRamRange> permissions;
#ifdef PS3_NATIVE_STARTUP_DISCOVERY
    for (size_t index = 0; index < plan.segment_count; ++index) {
        if (segments[index].memory_size > 16 * 1024 * 1024 ||
            uint64_t(segments[index].guest_address) + segments[index].memory_size >= ARGV) return 1;
        permissions.push_back({segments[index].guest_address, segments[index].memory_size, (segments[index].flags & 2) != 0});
    }
    permissions.push_back({STACK_START, STACK_BYTES, true});
    permissions.push_back({ARGV, 4096, true});
#else
    for (const Region &region : oracle.regions)
        permissions.push_back({region.address, region.bytes.size(), region.writable});
#endif
    permissions.push_back({TLS_BLOCK, 0x10000, true});
#ifdef PS3_NATIVE_LLE_SRE
    permissions.push_back({AUDIO_WINDOW, AUDIO_WINDOW_SIZE, true});
#endif
#ifdef PS3_NATIVE_GCM_INIT
    permissions.push_back({GCM_INIT_WINDOW, GCM_INIT_WINDOW_SIZE, true});
    permissions.push_back({GCM_LOCAL_BASE, GCM_LOCAL_SIZE, true});
#endif
    permissions.push_back({0x40000000, 0x10000000, true});
#ifdef PS3_NATIVE_LLE_SRE
    LleSreImageMapping lle_module_range{};
#endif
#ifdef PS3_NATIVE_LLE_SRE
    // Decision 33: the lifted libsre module needs its own guest window. It is offered against the
    // ranges just declared and refused (named stop below) if it would overlap any of them.
    {
        std::vector<LleSreImageMapping> declared;
        declared.reserve(permissions.size());
        for (const PpuMemoryRamRange &range : permissions)
            declared.push_back({range.address, range.length, range.writable});
        LleSreImageMapping module{};
        if (!lle_sre_image_mapping(declared.data(), declared.size(), &module)) {
            std::printf("STARTUP_LLE_SRE_MAPPING_CONFLICT base=0x%08x size=0x%x\n",
                lle_sre_base(), unsigned(lle_sre_mapped_extent()));
            std::fflush(stdout);
            return 1;
        }
        permissions.push_back({module.base, module.length, module.writable});
        lle_module_range = module;
    }
#endif
#ifdef PS3_NATIVE_LV2_SPU
    /* Decision 33 / B2b-2: the SPU image segment arrays live in a SYSTEM-owned guest window declared
     * as ordinary RAM, not in the title's user-memory quota (a console keeps them in kernel memory,
     * and the game allocator behind sc 348 only grants 64 KiB/1 MiB blocks). The window sits just past
     * the libsre extent 0x30000000+0x50000; a conflict is refused here, before any binding exists. */
    for (const PpuMemoryRamRange &declared : permissions)
        if (uint64_t(PS3_LV2_SPU_ARENA_BASE) < declared.address + declared.length &&
            uint64_t(declared.address) < uint64_t(PS3_LV2_SPU_ARENA_BASE) + PS3_LV2_SPU_ARENA_SIZE) {
            std::printf("STARTUP_LV2_SPU_ARENA_CONFLICT base=0x%08x size=0x%x overlaps=0x%08x+0x%llx\n",
                unsigned(PS3_LV2_SPU_ARENA_BASE), unsigned(PS3_LV2_SPU_ARENA_SIZE),
                unsigned(declared.address), (unsigned long long)declared.length);
            std::fflush(stdout);
            return 1;
        }
    permissions.push_back({PS3_LV2_SPU_ARENA_BASE, PS3_LV2_SPU_ARENA_SIZE, true});
#endif
    PpuMemoryBinding binding(domain, permissions.data(), permissions.size());
#ifdef PS3_NATIVE_LLE_SRE
    // Decision 33: map Sony's module once, right after the binding (root fix: the worker placed this after the [[noreturn]] run_startup_discovery, so the real path never mapped), BEFORE the guest entry and before any route can reach it.
    // The extent is committed as ordinary RAM, zeroed and filled with the image; each refusal is a
    // named stop, never a silent skip.
    {
        const LleSreMappingStatus admitted = lle_sre_mapping_admit();
        if (admitted != LleSreMappingStatus::Mapped) {
            std::printf("STARTUP_LLE_SRE_ADMIT_STOP status=%d\n", int(admitted));
            std::fflush(stdout);
            return 1;
        }
        const ps3_guest_memory_status committed =
            ps3_atomic_commit(&domain, lle_module_range.base, lle_module_range.length);
        if (committed != PS3_GUEST_MEMORY_OK) {
            std::printf("STARTUP_LLE_SRE_COMMIT_STOP base=0x%08x size=0x%x status=%d\n",
                lle_module_range.base, unsigned(lle_module_range.length), int(committed));
            std::fflush(stdout);
            return 1;
        }
        const LleSreMappingStatus mapped = lle_sre_map_image(&binding.coherent_memory);
        if (mapped != LleSreMappingStatus::Mapped) {
            std::printf("STARTUP_LLE_SRE_MAP_STOP base=0x%08x size=0x%x status=%d\n",
                lle_module_range.base, unsigned(lle_module_range.length), int(mapped));
            std::fflush(stdout);
            return 1;
        }
        std::printf("STARTUP_LLE_SRE_MAPPED base=0x%08x toc=0x%08x image=%u extent=0x%x exports=%zu\n",
            lle_sre_base(), lle_sre_toc(), unsigned(libsre_module_description.image_size),
            lle_sre_mapped_extent(), libsre_module_description.export_count);
        std::fflush(stdout);
    }
#endif
#ifdef PS3_NATIVE_LV2_SPU
    // The SPU segs arena is ordinary RAM: commit it once, beside the libsre module window.
    {
        const ps3_guest_memory_status committed = ps3_atomic_commit(&domain, PS3_LV2_SPU_ARENA_BASE,
            PS3_LV2_SPU_ARENA_SIZE);
        if (committed != PS3_GUEST_MEMORY_OK) {
            std::printf("STARTUP_LV2_SPU_ARENA_COMMIT_STOP base=0x%08x size=0x%x status=%d\n",
                unsigned(PS3_LV2_SPU_ARENA_BASE), unsigned(PS3_LV2_SPU_ARENA_SIZE), int(committed));
            std::fflush(stdout);
            return 1;
        }
        std::printf("STARTUP_LV2_SPU_ARENA base=0x%08x size=0x%x slots=%u\n",
            unsigned(PS3_LV2_SPU_ARENA_BASE), unsigned(PS3_LV2_SPU_ARENA_SIZE),
            0u);
        std::fflush(stdout);
    }
#endif
    TlsProvider provider{memory, {TLS_BLOCK, TLS_BLOCK + 0x10000}};
    provider.registry = &registry;
    provider.user_memory = &memory_service;
    ps3_horizon_clock horizon_clock{};
    CHECK(ps3_horizon_clock_init(&horizon_clock) == PS3_CLOCK_OK);
    ClockProvider clock_provider{};
    clock_provider.real = ps3_horizon_clock_port(&horizon_clock);
    ps3_clock shared_clock{&clock_provider, sample_real_clock};
    Ps3PpuExecutionServices services{&provider, initialize_tls, memory, create_mutex,
        lock_mutex, try_mutex, unlock_mutex, destroy_mutex, allocate_memory, free_memory, query_memory, &shared_clock, &compiled_lookup};
#ifdef PS3_NATIVE_STARTUP_RECORDER_TEST
    // Isolated test-only path: before any original startup owner admission.
    ppu_context recorder_initial{};
    recorder_initial.gpr[1] = STACK_TOP;
    recorder_initial.gpr[2] = toc;
    recorder_initial.thread_id = 1;
    const int recorder_failures = test_startup_recorded_services(services, binding,
        provider, clock_provider, timebase_audit, oracle, recorder_initial,
        plan.process_parameters.primary_priority);
    const auto recorder_mutex_cleanup = ps3_lwmutex_cleanup(&registry);
    const auto recorder_allocator_cleanup = ps3_user_memory_cleanup(&allocator);
    if (recorder_mutex_cleanup.status != PS3_LWMUTEX_OK ||
        recorder_allocator_cleanup.status != PS3_USER_MEMORY_OK) {
        std::puts("STARTUP_RECORDER_NATIVE_CLEANUP_FATAL resources_retained=true");
        std::fflush(stdout);
        svcExitProcess();
    }
    return recorder_failures != 0;
#endif
#ifdef PS3_NATIVE_STARTUP_BUDGET_TEST
    static_assert(std::extent<decltype(owners)>::value == size_t(STARTUP_ATOMIC_OWNER_CAPACITY) &&
        std::extent<typename std::remove_reference<decltype(objects)>::type>::value == size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY) &&
        std::extent<typename std::remove_reference<decltype(waiters)>::type>::value == size_t(STARTUP_LWMUTEX_WAITER_CAPACITY),
        "Native budget composition array extents are bound to the shared startup capacity profile");
    ppu_context budget_initial{};
    budget_initial.gpr[1] = STACK_TOP;
    budget_initial.gpr[2] = toc;
    budget_initial.gpr[3] = 1;
    budget_initial.gpr[4] = ARGV;
    budget_initial.gpr[5] = ENVP;
    budget_initial.gpr[7] = 1;
    budget_initial.gpr[8] = plan.tls.guest_address;
    budget_initial.gpr[9] = plan.tls.file_size;
    budget_initial.gpr[10] = plan.tls.memory_size;
    budget_initial.gpr[12] = plan.process_parameters.malloc_page_size;
    budget_initial.gpr[13] = 0x10f07000;
    budget_initial.thread_id = 1;
    budget_initial.cia = code;
    const ps3_clock budget_real_clock = ps3_horizon_clock_port(&horizon_clock);
#ifdef PS3_NATIVE_STARTUP_DISCOVERY
    run_startup_discovery(services, binding, budget_real_clock, budget_initial,
        plan.process_parameters.primary_priority, conditions, process, modules, sysutil, spu,
        StartupProcessSdk{uint32_t(plan.process_parameters.sdk_version), plan.has_process_parameters != 0}
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
        , allocator, *spurs_storage, *child_storage
#endif
        );
#endif
    const int budget_failures = test_native_budget(backing, services, binding, provider,
        clock_provider, budget_real_clock, oracle, budget_initial,
        plan.process_parameters.primary_priority, objects, waiters, owners, STARTUP_ATOMIC_OWNER_CAPACITY);
    const auto budget_mutex_cleanup = ps3_lwmutex_cleanup(&registry);
    const auto budget_allocator_cleanup = ps3_user_memory_cleanup(&allocator);
    if (budget_mutex_cleanup.status != PS3_LWMUTEX_OK || budget_allocator_cleanup.status != PS3_USER_MEMORY_OK)
        native_budget_fatal("root-cleanup");
    std::printf("NATIVE_BUDGET_CLEANUP known_resources_only=true normal_return=true failures=%d\n", budget_failures);
    return budget_failures != 0;
#endif
    auto control_storage = std::make_unique<StartupControl>();
    StartupControl &control = *control_storage;
    configure(control, oracle);
    StartupExecution execution{};
    execution.control = &control;
    execution.dispatch_stop = {&control, dispatch_stop_requested};
#if defined(PS3_NATIVE_LV2_SYNC) || defined(PS3_NATIVE_LV2_SPU)
    if (!wire_lle_lv2_services(execution)) return 1;
#endif
    CHECK(native_startup_admit(execution, binding, services));
#ifdef PS3_NATIVE_GCM_INIT
    configure_gcm_init(execution);
#endif
    execution.guest_priority = plan.process_parameters.primary_priority;
    execution.has_guest_priority = true;
    execution.ppu.cpu.gpr[1] = STACK_TOP;
    execution.ppu.cpu.gpr[2] = toc;
    execution.ppu.cpu.gpr[3] = 1;
    execution.ppu.cpu.gpr[4] = ARGV;
    execution.ppu.cpu.gpr[5] = ENVP;
    execution.ppu.cpu.gpr[7] = 1;
    execution.ppu.cpu.gpr[8] = plan.tls.guest_address;
    execution.ppu.cpu.gpr[9] = plan.tls.file_size;
    execution.ppu.cpu.gpr[10] = plan.tls.memory_size;
    execution.ppu.cpu.gpr[12] = plan.process_parameters.malloc_page_size;
    execution.ppu.cpu.gpr[13] = 0x10f07000;
    execution.ppu.cpu.thread_id = 1;
    execution.ppu.cpu.cia = code;
    const ppu_context initial = execution.ppu.cpu;
    failures += test_memory_family(memory, &domain, &allocator, provider, services,
        initial, oracle, binding, execution.guest_priority);
    failures += test_clock_abi(memory, services, initial, oracle,
        clock_provider, binding, execution.guest_priority);
    failures += test_real_indirect(services, binding, initial, oracle, execution.guest_priority);
    failures += test_binding_identity(backing, services, provider, binding, initial, oracle, execution.guest_priority);
    failures += test_reservation_boundaries(services, binding, initial, oracle, execution.guest_priority);
#ifdef PS3_NATIVE_STARTUP_TIMEBASE
    failures += test_timebase_frequency(backing, services, initial, oracle,
        binding, execution.guest_priority, provider, clock_provider, timebase_audit);
    if (failures) return 1;
#endif
#ifdef PS3_NATIVE_THREAD_ID_QUERY
    failures += test_thread_id_query(backing, services, initial, oracle,
        binding, execution.guest_priority, timebase_audit);
    if (failures) return 1;
#endif
    ppu_context expected = initial;
    model_to_tls(expected, oracle);
    const ppu_context expected_before_tls = expected;
    oracle.write(expected.gpr[1] + 0x28, expected.gpr[2], 8, true, PpuMemoryTraceOrigin::Hle);
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
    model_info_and_second_mutex(expected, oracle, first_queue + 1);
    model_stream_records(expected, oracle, first_queue + 2);
    model_descriptor_table(expected, oracle);
    const bool startup_joined = run(execution);
    CHECK(startup_joined);
    if (control.memory_result.retained_pin || control.memory_result.retained_allocation) {
        std::printf("STARTUP_MEMORY_FATAL retained_output_pin=%d retained_allocation=%d address=0x%x joined=true backing_retained=true\n",
            control.memory_result.retained_pin, control.memory_result.retained_allocation,
            control.memory_result.retained_address);
        std::fflush(stdout);
        svcExitProcess();
    }
#ifdef PS3_NATIVE_MEMORY_SCOUT
    std::printf("STARTUP_MEMORY_SCOUT stop=%d imports=%zu accesses=%zu syscall=%u memory_returns=%zu\n",
        int(control.stop), control.import_count, control.journal.count, control.syscall_number, control.memory_returns);
    std::printf("SCOUT_CLOCK calls=%zu returns=%zu status=%d sample=%llu\n", control.clock_calls, control.clock_returns, control.clock_status, (unsigned long long)control.clock_sample);
    for (size_t index = 0; index < control.import_count; ++index)
        std::printf("SCOUT_IMPORT index=%zu nid=0x%x\n", index, control.imports[index]);
    for (size_t index = 0; index < control.journal.count; ++index) {
        const StartupAccess &access = control.accesses[index];
        std::printf("SCOUT_ACCESS index=%zu address=%llx bytes=%zu write=%d value=%llx\n", index,
            (unsigned long long)access.address, access.length, (access.operation == PpuMemoryTraceOperation::Write), (unsigned long long)access.value);
    }
    for (unsigned reg = 0; reg < 32; ++reg)
        std::printf("SCOUT_REGISTER r%u value=%llx\n", reg, (unsigned long long)execution.ppu.cpu.gpr[reg]);
    std::printf("SCOUT_CPU cr=%x lr=%llx cia=%llx\n", execution.ppu.cpu.cr,
        (unsigned long long)execution.ppu.cpu.lr, (unsigned long long)execution.ppu.cpu.cia);
    const auto scout_cleanup = ps3_lwmutex_cleanup(&registry);
    if (scout_cleanup.status != PS3_LWMUTEX_OK || ps3_user_memory_cleanup(&allocator).status != PS3_USER_MEMORY_OK) {
        std::fflush(stdout);
        svcExitProcess();
    }
    std::printf("STARTUP_MEMORY_SCOUT_COMPLETE acceptance=false failures=%d\n", failures);
    return failures != 0;
#endif
#ifdef PS3_NATIVE_INDIRECT_SCOUT
    model_clock_to_indirect(expected, oracle, control.first_clock_sample, first_queue + 10);
    CHECK(control.dispatch_count >= 1);
    if (control.dispatch_count) {
        CHECK(!std::memcmp(&control.dispatches[0].before, &expected, sizeof(expected)));
        CHECK(control.dispatches[0].captured_ctr == 0x462380);
    }
    const size_t prior_events = oracle.accesses.size();
    expected.gpr[3] = oracle.read(expected.gpr[2] - 0x65dc, 4);
    expected.gpr[0] = expected.lr;
    oracle.write(expected.gpr[1] - 0x70, expected.gpr[1], 8);
    expected.gpr[1] -= 0x70;
    expected.gpr[3] = uint32_t(expected.gpr[3] + 8);
    oracle.write(expected.gpr[1] + 0x80, expected.gpr[0], 8);
    expected.lr = 0x462398;
    CHECK(oracle.accesses.size() == prior_events + 3 && control.journal.count >= oracle.accesses.size());
    for (size_t index = 0; index < oracle.accesses.size() && index < control.journal.count; ++index)
        CHECK(ppu_memory_event_equal(control.accesses[index], oracle.accesses[index]));
    std::printf("INDIRECT_SCOUT_PREFIX prior_events=%zu wrapper_events=3 full_entry_cpu_compared=true failures=%d\n",
        prior_events, failures);
    std::printf("INDIRECT_SCOUT_STOP reason=%d external=%u invalid_address=0x%llx imports=%zu accesses=%zu dispatches=%zu retained_depth=%u joined=true\n",
        int(control.stop), control.external_id, (unsigned long long)control.invalid_address,
        control.import_count, control.journal.count, control.dispatch_count, execution.dispatch.depth);
    for (size_t index = 0; index < control.dispatch_count; ++index) {
        const auto &record = control.dispatches[index];
        std::printf("INDIRECT_SCOUT_CALL index=%zu ctr=0x%llx before_sp=0x%llx before_lr=0x%llx core_returned=%d status=%d depth_before=%u depth_after=%u\n",
            index, (unsigned long long)record.captured_ctr, (unsigned long long)record.before.gpr[1],
            (unsigned long long)record.before.lr, record.core_returned,
            record.core_returned ? int(record.result.status) : -1, record.depth_before, record.depth_after);
    }
    for (size_t index = 0; index < control.import_count; ++index)
        std::printf("INDIRECT_SCOUT_IMPORT index=%zu nid=0x%x\n", index, control.imports[index]);
    for (size_t index = 0; index < control.journal.count; ++index) {
        const auto &event = control.accesses[index];
        std::printf("INDIRECT_SCOUT_ACCESS index=%zu origin=%s operation=%d address=0x%x length=%zu value=0x%llx stored=%d\n",
            index, event.origin == PpuMemoryTraceOrigin::Generated ? "generated" : "hle",
            int(event.operation), event.address, event.length, (unsigned long long)event.value, event.stored);
    }
    for (unsigned index = 0; index < 32; ++index)
        std::printf("INDIRECT_SCOUT_REGISTER r%u=0x%llx\n", index, (unsigned long long)execution.ppu.cpu.gpr[index]);
    std::printf("INDIRECT_SCOUT_CPU cr=0x%x lr=0x%llx ctr=0x%llx cia=0x%llx\n", execution.ppu.cpu.cr,
        (unsigned long long)execution.ppu.cpu.lr, (unsigned long long)execution.ppu.cpu.ctr,
        (unsigned long long)execution.ppu.cpu.cia);
    for (const Region &region : oracle.regions) {
        uint64_t checksum = UINT64_C(14695981039346656037);
        unsigned char bytes[4096];
        for (size_t offset = 0; offset < region.bytes.size();) {
            const size_t length = std::min(sizeof(bytes), region.bytes.size() - offset);
            CHECK(memory->read(memory->context, region.address + offset, bytes, length) == PS3_GUEST_MEMORY_OK);
            for (size_t index = 0; index < length; ++index) checksum = (checksum ^ bytes[index]) * UINT64_C(1099511628211);
            offset += length;
        }
        std::printf("INDIRECT_SCOUT_MEMORY address=0x%x length=%zu fnv64=0x%llx\n",
            region.address, region.bytes.size(), (unsigned long long)checksum);
    }
    std::printf("INDIRECT_SCOUT_COMPLETE acceptance=false all_workers_joined=true resources_retained=true failures=%d\n", failures);
    std::fflush(stdout);
    svcExitProcess();
#endif
    CHECK(startup_joined && !execution.ppu.admitted);
    CHECK(control.clock_calls == 1 && control.clock_returns == 1 && control.clock_status == PS3_CLOCK_OK);
    model_clock_to_indirect(expected, oracle, control.first_clock_sample, first_queue + 10);
    std::vector<ppu_context> dispatch_entries;
    dispatch_entries.push_back(expected);
    model_first_indirect_return(expected, oracle, first_queue + 11);
    model_initializer_loop(expected, oracle, dispatch_entries);
    CHECK(control.stop == StartupStop::UnsupportedSyscall && control.syscall_number == 147);
    CHECK(control.memory_returns == 1 && control.tls_returns == 1 && control.mutex_returns == 16);
    CHECK(control.clock_returns == 1 && provider.calls == 1 && provider.mutex_calls == 16);
    CHECK(provider.active_scopes == 0 && registry.next_queue_id == first_queue + 12);
    const uint32_t expected_imports[] = {0x744680a2,
        0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef,
        0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef, 0x2f85c0ef,
        0x8461e528, 0x2f85c0ef, 0x1573dc3f, 0x2f85c0ef, 0x1bc200f4, 0x1573dc3f, 0x1bc200f4};
    CHECK(control.import_count == sizeof(expected_imports) / sizeof(expected_imports[0]));
    for (size_t index = 0; index < control.import_count && index < 18; ++index)
        CHECK(control.imports[index] == expected_imports[index]);
    CHECK(control.dispatch_count == 4 && dispatch_entries.size() == 4 && execution.dispatch.depth == 0);
    for (size_t index = 0; index < control.dispatch_count && index < dispatch_entries.size(); ++index) {
        const auto &call = control.dispatches[index];
        CHECK(call.core_returned && call.result.status == PS3_COMPILED_DISPATCH_OK && call.result.lookup_attempted);
        CHECK(call.result.lookup_status == PS3_COMPILED_CODE_OK && call.depth_before == 0 && call.depth_after == 0);
        CHECK(call.captured_ctr == dispatch_entries[index].ctr);
        const bool entry_cpu_matched = !std::memcmp(&call.before, &dispatch_entries[index], sizeof(ppu_context));
        CHECK(entry_cpu_matched);
        std::printf("INDIRECT_ORACLE_CALL index=%zu target=0x%llx full_cpu_match=%s returned=true depth=0\n",
            index, (unsigned long long)call.captured_ctr, entry_cpu_matched ? "true" : "false");
    }
    CHECK(!std::memcmp(&control.before_tls, &expected_before_tls, sizeof(expected)));
    CHECK(!std::memcmp(&control.after_tls, &expected_after_tls, sizeof(expected)));
    if (std::memcmp(&execution.ppu.cpu, &expected, sizeof(expected))) {
        for (unsigned reg = 0; reg < 32; ++reg) if (execution.ppu.cpu.gpr[reg] != expected.gpr[reg])
            std::printf("INDIRECT_ORACLE_REGISTER r%u actual=0x%llx expected=0x%llx\n", reg,
                (unsigned long long)execution.ppu.cpu.gpr[reg], (unsigned long long)expected.gpr[reg]);
        std::printf("INDIRECT_ORACLE_SPECIAL actual_cr=0x%x expected_cr=0x%x actual_lr=0x%llx expected_lr=0x%llx\n",
            execution.ppu.cpu.cr, expected.cr, (unsigned long long)execution.ppu.cpu.lr, (unsigned long long)expected.lr);
        CHECK(false);
    }
    CHECK(!std::memcmp(&control.terminal, &expected, sizeof(expected)));
    CHECK(oracle.valid && control.journal.count == oracle.accesses.size());
    size_t generated = 0, hle = 0, expected_generated = 0, expected_hle = 0;
    for (const auto &event : oracle.accesses) {
        if (event.origin == PpuMemoryTraceOrigin::Generated) ++expected_generated;
        else ++expected_hle;
    }
    for (size_t index = 0; index < control.journal.count && index < oracle.accesses.size(); ++index) {
        const auto &actual = control.accesses[index];
        const auto &wanted = oracle.accesses[index];
        if (!ppu_memory_event_equal(actual, wanted)) {
            std::printf("INDIRECT_ORACLE_EVENT_FAIL index=%zu actual=0x%x/%zu/%llx/%d expected=0x%x/%zu/%llx/%d\n",
                index, actual.address, actual.length, (unsigned long long)actual.value, int(actual.operation),
                wanted.address, wanted.length, (unsigned long long)wanted.value, int(wanted.operation));
            CHECK(false);
        }
        if (actual.origin == PpuMemoryTraceOrigin::Generated) ++generated;
        else ++hle;
        std::printf("INDIRECT_ORACLE_ACCESS index=%zu origin=%s operation=%d address=0x%x length=%zu value=0x%llx\n",
            index, actual.origin == PpuMemoryTraceOrigin::Generated ? "generated" : "hle", int(actual.operation),
            actual.address, actual.length, (unsigned long long)actual.value);
    }
    CHECK(ppu_memory_trace_equal(control.accesses, control.journal.count,
        oracle.accesses.data(), oracle.accesses.size()));
    CHECK(generated == 827 && hle == 18 && expected_generated == 827 && expected_hle == 18);
    const bool memory_matched = compare(memory, oracle);
    CHECK(memory_matched);
#ifdef PS3_NATIVE_GCM_INIT
    report_gcm_init(control);
    report_gcm_query(control, &gcm_init_storage);
    report_gcm_local_memory();
#endif
    std::printf("INDIRECT_ORACLE_STOP syscall=%u lr=0x%llx sp=0x%llx cpu_bytes=%zu generated=%zu hle=%zu memory_comparison_attempted=true memory_match=%s handler_called=false failures=%d\n",
        control.syscall_number, (unsigned long long)execution.ppu.cpu.lr,
        (unsigned long long)execution.ppu.cpu.gpr[1], sizeof(ppu_context), generated, hle, memory_matched ? "true" : "false", failures);
    // Compare stopped state first. Cleanup rejects every held or uncertain
    // object; no new lock is automatically unlocked or abandoned.
    if (!inspect_unlocked_resources_after_join(*memory, objects, size_t(STARTUP_LWMUTEX_OBJECT_CAPACITY), waiters, size_t(STARTUP_LWMUTEX_WAITER_CAPACITY), 12, startup_joined)) {
        std::puts("INDIRECT_CLEANUP_FATAL joined=true unknown_or_held_resource=true backing_retained=true");
        std::fflush(stdout);
        svcExitProcess();
    }
    const auto cleaned = ps3_lwmutex_cleanup(&registry);
    const auto allocation_cleanup = ps3_user_memory_cleanup(&allocator);
    if (cleaned.status != PS3_LWMUTEX_OK || allocation_cleanup.status != PS3_USER_MEMORY_OK) {
        std::printf("INDIRECT_CLEANUP_FATAL mutex=%d allocator=%d retained_resources=true\n",
            cleaned.status, allocation_cleanup.status);
        std::fflush(stdout);
        svcExitProcess();
    }
    for (const auto &object : objects) CHECK(!object.state);
    std::printf("INDIRECT_DIAGNOSTIC_CLEANUP joined=true unlocked_only=true core_status=0 no_abandonment=true guest_progress=false\n");
    std::printf("STARTUP_INDIRECT_%s failures=%d actual_targets=4 full_context_and_memory=true original_trampolines_executed=true next_syscall=147 game_boot=false\n",
        failures ? "FAIL" : "PASS", failures);
    return failures != 0;
#undef CHECK
}
