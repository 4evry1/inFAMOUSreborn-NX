#include "timebase.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

/* 79.8 ticks/us = 399/5: quotient/remainder avoids wide multiplication. */
static uint64_t oracle(uint64_t us)
{
    return (us / 5) * UINT64_C(399) + ((us % 5) * UINT64_C(399)) / 5;
}
static void check(uint64_t us)
{
    uint64_t result = ps3_timebase_from_microseconds(us);
    assert(result == oracle(us));
    assert(result == ps3_timebase_from_microseconds(us));
}
int main(void)
{
    static const uint64_t expected[] = {0, 79, 159, 239, 319, 399, 478, 558, 638, 718, 798};
    _Static_assert(PS3_TIMEBASE_FREQUENCY == UINT64_C(79800000), "PS3 frequency");
    for (uint64_t us = 0; us < sizeof expected / sizeof *expected; ++us)
        assert(ps3_timebase_from_microseconds(us) == expected[us]);
    assert(ps3_timebase_from_microseconds(UINT64_C(1000000)) == PS3_TIMEBASE_FREQUENCY);
    /* Last input below the first 64-bit counter wrap, then first above it. */
    const uint64_t boundary = UINT64_C(231162206437463052);
    assert(ps3_timebase_from_microseconds(boundary) == UINT64_C(18446744073709551549));
    assert(ps3_timebase_from_microseconds(boundary + 1) == UINT64_C(13));
    for (uint64_t offset = 0; offset < 20; ++offset) {
        check(boundary - 10 + offset);
        check(UINT64_MAX - offset);
    }
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0; i < 100000; ++i) {
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        check(state);
    }
    puts("TIMEBASE_CONTRACT_PASS");
    return 0;
}
