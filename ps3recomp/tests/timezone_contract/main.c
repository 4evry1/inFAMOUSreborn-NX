#include "sys_timezone_values.h"

#include <stdint.h>
#include <stdio.h>

static int expect_values(int bias, int standard_bias, int daylight_bias,
                         int daylight, int32_t want_timezone,
                         int32_t want_summertime)
{
    int32_t timezone = 0;
    int32_t summertime = 0;
    ps3_timezone_values(bias, standard_bias, daylight_bias, daylight,
                        &timezone, &summertime);
    if (timezone == want_timezone && summertime == want_summertime)
        return 1;
    fprintf(stderr,
            "FAIL: bias=%d standard=%d daylight_bias=%d daylight=%d -> "
            "%d,%d (wanted %d,%d)\n",
            bias, standard_bias, daylight_bias, daylight,
            timezone, summertime, want_timezone, want_summertime);
    return 0;
}

int main(void)
{
    if (!expect_values(60, 0, -60, 0, -60, 0) ||
        !expect_values(60, 0, -60, 1, -60, 60) ||
        !expect_values(0, 0, -60, 1, 0, 60))
        return 1;
    puts("timezone_contract: PASS");
    return 0;
}
