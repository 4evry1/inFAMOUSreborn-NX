/* Host stand-in for libnx's <switch.h>, only for test_startup_audio_out_pump.cpp: the one call the pump makes. */
#ifndef AUDIO_OUT_HOST_SWITCH_H
#define AUDIO_OUT_HOST_SWITCH_H
#include <stdint.h>
static inline void svcSleepThread(int64_t nanoseconds) { (void)nanoseconds; }
#endif
