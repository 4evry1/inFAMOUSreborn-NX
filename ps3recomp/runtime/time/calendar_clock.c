#include "calendar_clock.h"
#include <stddef.h>

ps3_calendar_clock_status ps3_calendar_clock_read(const ps3_calendar_clock *clock,
                                                 ps3_calendar_time *output)
{
    if (!clock || !output || !clock->sample) return PS3_CALENDAR_CLOCK_INVALID_ARGUMENT;
    ps3_calendar_time sample = {0};
    const ps3_calendar_clock_status status = clock->sample(clock->context, &sample);
    switch (status) {
    case PS3_CALENDAR_CLOCK_INVALID_ARGUMENT:
    case PS3_CALENDAR_CLOCK_PROVIDER_FAILURE:
    case PS3_CALENDAR_CLOCK_INVALID_SAMPLE:
        return status;
    case PS3_CALENDAR_CLOCK_OK:
        break;
    default:
        return PS3_CALENDAR_CLOCK_INVALID_SAMPLE;
    }
    if (sample.nanoseconds >= UINT32_C(1000000000)) return PS3_CALENDAR_CLOCK_INVALID_SAMPLE;
    *output = sample;
    return PS3_CALENDAR_CLOCK_OK;
}
