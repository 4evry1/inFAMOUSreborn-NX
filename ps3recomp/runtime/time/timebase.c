#include "timebase.h"

uint64_t ps3_timebase_from_microseconds(uint64_t elapsed_microseconds)
{
    const __uint128_t ticks = (__uint128_t)elapsed_microseconds * PS3_TIMEBASE_FREQUENCY;
    return (uint64_t)(ticks / UINT64_C(1000000));
}
