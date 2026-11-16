/* Host stand-in for native/adapters/startup_execution.h, only for test_startup_audio_out_pump.cpp. It defines the real
 * header's include guard first, so `#include "startup_execution.h"` inside startup_audio_out.h becomes a no-op, and
 * supplies exactly what the pump uses: StartupAudioProcess (service + lock), the real ps3_clock and ps3_guest_memory
 * types, and the declaration of startup_audio_tick (the test defines it). */
#ifndef NATIVE_STARTUP_EXECUTION_H
#define NATIVE_STARTUP_EXECUTION_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "clock.h"          // the real ps3_clock (source/ps3recomp/runtime/time)
#include "guest_space.h"    // the real ps3_guest_memory (source/ps3recomp/runtime/memory)
#include "audio_service.h"  // the real audio service (native/domain)
struct StartupAudioProcess {
    audio_service service{};
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint64_t ticks = 0, index_stores = 0, store_failures = 0;
};
void startup_audio_tick(StartupAudioProcess &process, const ps3_guest_memory &memory, uint64_t now_us);
#endif
