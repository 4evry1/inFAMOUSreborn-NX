#include "clock.h"

ps3_clock_status ps3_clock_elapsed_microseconds(
    uint64_t origin, uint64_t ticks, uint64_t frequency, uint64_t *output)
{
    if (!output || !frequency) return PS3_CLOCK_ARGUMENT;
    if (ticks < origin) return PS3_CLOCK_RANGE;
    const __uint128_t scaled = (__uint128_t)(ticks - origin) * UINT64_C(1000000);
    const __uint128_t elapsed = scaled / frequency;
    if (elapsed > UINT64_MAX) return PS3_CLOCK_RANGE;
    *output = (uint64_t)elapsed;
    return PS3_CLOCK_OK;
}
