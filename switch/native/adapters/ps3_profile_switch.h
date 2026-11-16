#ifndef PS3_NATIVE_PROFILE_SWITCH_H
#define PS3_NATIVE_PROFILE_SWITCH_H
/* The Horizon sampling profiler (ps3_profile_switch.c; linked only with LLE_PROFILER=1, which also wraps libnx's threadCreate so every thread is known). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Starts the sampler thread once. `main_thread_handle`: the kernel handle of the thread to profile in detail (the guest main thread, Thread::handle). Does nothing when
 * <app dir>/profile-off.txt exists. */
void ps3_profile_start(uint32_t main_thread_handle);
/* Called by the threadCreate wrapper. */
void ps3_profile_note_thread(uint32_t handle, uintptr_t entry);
#ifdef __cplusplus
}
#endif
#endif
