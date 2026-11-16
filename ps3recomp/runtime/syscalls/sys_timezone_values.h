#ifndef PS3RECOMP_SYS_TIMEZONE_VALUES_H
#define PS3RECOMP_SYS_TIMEZONE_VALUES_H

#include <stdint.h>

static inline void ps3_timezone_values(int bias, int standard_bias,
                                       int daylight_bias, int daylight,
                                       int32_t* timezone,
                                       int32_t* summertime)
{
    if (timezone)
        *timezone = -bias;
    if (summertime)
        *summertime = -(daylight ? daylight_bias : standard_bias);
}

#endif
