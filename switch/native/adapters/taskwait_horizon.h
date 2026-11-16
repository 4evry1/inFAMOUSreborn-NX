#ifndef NATIVE_TASKWAIT_HORIZON_H
#define NATIVE_TASKWAIT_HORIZON_H
#include <cstdint>
// A single scheduling pause is bounded independently of the guest predicate.
constexpr int64_t STARTUP_TASKWAIT_MAX_SLEEP_NS = INT64_C(10000000);
struct StartupTaskwaitHorizonConfig { int64_t sleep_ns; };
bool startup_taskwait_horizon_yield_once(void *context) noexcept;
// Decision 121/123: install as ps3_atomic_guarded_write_hook; wakes a pause early when guest memory near the watched object is written
// under the domain guard.
extern "C" void startup_taskwait_horizon_wake(uint64_t address, uint64_t length);
// Decision 123: the object the guest is about to wait on (ps3_taskwait_begin) and the end of that wait (ps3_taskwait_end).
extern "C" void startup_taskwait_horizon_watch(uint32_t object);
extern "C" void startup_taskwait_horizon_end(void);
bool startup_taskwait_horizon_wake_enabled() noexcept;
constexpr unsigned STARTUP_TASKWAIT_TAGS = 10;   // writer tag 0 (PPU/other), SPU slots 0..7 as 1..8, 9 = anything higher
extern uint64_t g_startup_taskwait_waits, g_startup_taskwait_woken, g_startup_taskwait_signals;
extern uint64_t g_startup_taskwait_overlap_by_tag[STARTUP_TASKWAIT_TAGS], g_startup_taskwait_end_by_tag[STARTUP_TASKWAIT_TAGS];
#endif
