#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "calendar_clock_horizon.h"
#include <stddef.h>
#include <time.h>

_Static_assert((time_t)-1 < (time_t)0 && sizeof(time_t) <= sizeof(int64_t),
               "Calendar adapter requires signed time_t representable in int64_t");
static ps3_calendar_clock_status calendar_clock_horizon_sample(void *context,
                                                               ps3_calendar_time *output)
{
    (void)context;
    if (!output) return PS3_CALENDAR_CLOCK_INVALID_ARGUMENT;
    struct timespec sample = {0};
    if (clock_gettime(CLOCK_REALTIME, &sample) != 0) return PS3_CALENDAR_CLOCK_PROVIDER_FAILURE;
    if (sample.tv_nsec < 0 || sample.tv_nsec >= 1000000000L) return PS3_CALENDAR_CLOCK_INVALID_SAMPLE;
    const ps3_calendar_time value = {(int64_t)sample.tv_sec, (uint32_t)sample.tv_nsec};
    *output = value;
    return PS3_CALENDAR_CLOCK_OK;
}
ps3_calendar_clock calendar_clock_horizon_port(void)
{
    return (ps3_calendar_clock){NULL, calendar_clock_horizon_sample};
}
