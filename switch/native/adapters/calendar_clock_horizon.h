#ifndef NATIVE_CALENDAR_CLOCK_HORIZON_H
#define NATIVE_CALENDAR_CLOCK_HORIZON_H
#include "calendar_clock.h"
#ifdef __cplusplus
extern "C" {
#endif
// Borrows the default libnx application time lifecycle through worker join.
// No initialization, re-anchoring, or SDK exit is performed by this adapter.
// The pinned SDK calendar anchor has whole-second epoch accuracy limitations.
ps3_calendar_clock calendar_clock_horizon_port(void);
#ifdef __cplusplus
}
#endif
#endif
