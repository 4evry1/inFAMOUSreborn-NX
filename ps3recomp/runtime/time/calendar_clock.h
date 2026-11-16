#ifndef PS3RECOMP_CALENDAR_CLOCK_H
#define PS3RECOMP_CALENDAR_CLOCK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_calendar_time {
    int64_t seconds;
    uint32_t nanoseconds;
} ps3_calendar_time;
typedef enum ps3_calendar_clock_status {
    PS3_CALENDAR_CLOCK_OK = 0,
    PS3_CALENDAR_CLOCK_INVALID_ARGUMENT,
    PS3_CALENDAR_CLOCK_PROVIDER_FAILURE,
    PS3_CALENDAR_CLOCK_INVALID_SAMPLE
} ps3_calendar_clock_status;
typedef struct ps3_calendar_clock {
    void *context;
    ps3_calendar_clock_status (*sample)(void *context, ps3_calendar_time *output);
} ps3_calendar_clock;

// Calendar seconds are POSIX UTC; precision comes from the injected provider.
// Any int64 seconds are valid. Failure preserves the entire caller pair.
ps3_calendar_clock_status ps3_calendar_clock_read(const ps3_calendar_clock *clock,
                                                 ps3_calendar_time *output);
#ifdef __cplusplus
}
#endif
#endif
