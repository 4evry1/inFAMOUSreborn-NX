#include "spu_helpers.h"
#include <float.h>
#include <fenv.h>
#include <stdio.h>

static int check_double_result(const char *name, u128 actual, double expected)
{
    if (spu__dget(actual, 0) == expected && spu__dget(actual, 1) == expected)
        return 0;
    printf("FAIL %s: %.17g %.17g expected %.17g\n", name,
           spu__dget(actual, 0), spu__dget(actual, 1), expected);
    return 1;
}

int native_test_double_mode(void)
{
    const int original_rounding = fegetround();
    if (fesetround(FE_TOWARDZERO) != 0)
        return 1;
    u128 one = spu_zero(), increment = spu_zero(), tiny = spu_zero();
    for (int lane = 0; lane < 2; ++lane) {
        spu__dset(&one, lane, 1.0);
        spu__dset(&increment, lane, 0x1.8p-53);
        spu__dset(&tiny, lane, DBL_MIN);
    }
    int failures = check_double_result("round to nearest", spu_dfa(one, increment),
                                      0x1.0000000000001p0);
    failures += fegetround() != FE_TOWARDZERO;
    u128 half = spu_zero();
    for (int lane = 0; lane < 2; ++lane)
        spu__dset(&half, lane, 0.5);
    failures += check_double_result("preserve subnormal", spu_dfm(tiny, half),
                                    0x0.8p-1022);
    failures += fegetround() != FE_TOWARDZERO;
    failures += fesetround(original_rounding) != 0;
    printf("SPU double mode: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
