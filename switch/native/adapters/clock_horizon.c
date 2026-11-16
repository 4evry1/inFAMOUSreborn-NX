#include "clock_horizon.h"
#include <switch.h>

static ps3_clock_status now_microseconds(void *context, uint64_t *output)
{
    const ps3_horizon_clock *clock = context;
    if (!clock || !clock->initialized || !output) return PS3_CLOCK_ARGUMENT;
    return ps3_clock_elapsed_microseconds(clock->origin_ticks,
        armGetSystemTick(), clock->frequency, output);
}

ps3_clock_status ps3_horizon_clock_init(ps3_horizon_clock *clock)
{
    if (!clock) return PS3_CLOCK_ARGUMENT;
    const uint64_t frequency = armGetSystemTickFreq();
    if (!frequency) return PS3_CLOCK_PLATFORM_FAILURE;
    *clock = (ps3_horizon_clock){armGetSystemTick(), frequency, 1};
    return PS3_CLOCK_OK;
}

ps3_clock ps3_horizon_clock_port(ps3_horizon_clock *clock)
{
    return (ps3_clock){clock, now_microseconds};
}
