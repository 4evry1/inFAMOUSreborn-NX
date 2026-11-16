#include "time/clock.h"
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    int failures = 0;
#define CHECK(value) do { if (!(value)) { printf("CLOCK_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    uint64_t value = 0x12345678;
    CHECK(ps3_clock_elapsed_microseconds(0, 1, 0, &value) == PS3_CLOCK_ARGUMENT);
    CHECK(value == 0x12345678);
    CHECK(ps3_clock_elapsed_microseconds(0, 1, 1, NULL) == PS3_CLOCK_ARGUMENT);
    CHECK(ps3_clock_elapsed_microseconds(20, 19, 19200000, &value) == PS3_CLOCK_RANGE);
    CHECK(value == 0x12345678);
    CHECK(ps3_clock_elapsed_microseconds(0, UINT64_MAX, 1, &value) == PS3_CLOCK_RANGE);
    CHECK(value == 0x12345678);
    CHECK(ps3_clock_elapsed_microseconds(20, 20, 19200000, &value) == PS3_CLOCK_OK && value == 0);
    CHECK(ps3_clock_elapsed_microseconds(20, 39, 19200000, &value) == PS3_CLOCK_OK && value == 0);
    CHECK(ps3_clock_elapsed_microseconds(20, 40, 19200000, &value) == PS3_CLOCK_OK && value == 1);
    CHECK(ps3_clock_elapsed_microseconds(20, 19200020, 19200000, &value) == PS3_CLOCK_OK && value == 1000000);
    CHECK(ps3_clock_elapsed_microseconds(0, UINT64_MAX, 1000000, &value) == PS3_CLOCK_OK && value == UINT64_MAX);
    CHECK(ps3_clock_elapsed_microseconds(0, UINT64_MAX, UINT64_MAX, &value) == PS3_CLOCK_OK && value == 1000000);
    CHECK(ps3_clock_elapsed_microseconds(0, UINT64_C(4500000000), 1000000, &value) == PS3_CLOCK_OK && value == UINT64_C(4500000000));
    printf("CLOCK_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
