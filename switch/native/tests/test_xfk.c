#include "spu_helpers.h"
#include "spu_xfk.h"
#include <float.h>
#include <math.h>
#include <stdio.h>

static u128 splat_float(float value)
{
    u128 result;
    for (int lane = 0; lane < 4; ++lane) result._f32[lane] = value;
    return result;
}

static int check_lanes(u128 result, float expected)
{
    for (int lane = 0; lane < 4; ++lane)
        if (result._f32[lane] != expected) return 1;
    return 0;
}

int native_test_xfk(void)
{
    int failures = 0;
    u128 a = splat_float(1.5f), b = splat_float(-2.0f), c = splat_float(0.25f);
    for (unsigned mask = 0; mask < 8; ++mask) {
        failures += check_lanes(spu_fa_k(a, b, mask), -0.5f);
        failures += check_lanes(spu_fs_k(a, b, mask), 3.5f);
        failures += check_lanes(spu_fm_k(a, b, mask), -3.0f);
        failures += check_lanes(spu_fma_k(a, b, c, mask), -2.75f);
        failures += check_lanes(spu_fms_k(a, b, c, mask), -3.25f);
        failures += check_lanes(spu_fnms_k(a, b, c, mask), 3.25f);
    }
    a = splat_float(0.0f); b = splat_float(INFINITY);
    failures += check_lanes(spu_fm_k(a, b, 2), 0.0f);
    failures += !isnan(spu_fm_k(a, b, 0)._f32[0]);
    failures += check_lanes(spu_fm_k(b, a, 1), 0.0f);
    failures += !isnan(spu_fm_k(b, a, 2)._f32[0]);
    c = splat_float(INFINITY);
    failures += check_lanes(spu_fma_k(a, a, c, 4), FLT_MAX);
    failures += !isinf(spu_fma_k(a, a, c, 0)._f32[0]);
    b = splat_float(NAN);
    failures += check_lanes(spu_fm_k(a, b, 2), 0.0f);
    failures += !isnan(spu_fm_k(a, b, 0)._f32[0]);
    a = splat_float(-0.0f); b = splat_float(2.0f);
    failures += spu_fm_k(a, b, 3)._u32[0] != 0x80000000u;
    a = splat_float(0x1.000002p0f); b = splat_float(0x1.fffffcp-1f);
    c = splat_float(-1.0f);
    failures += check_lanes(spu_fma_k(a, b, c, 7), -0x1p-46f);
    const float lane_results[] = {3.0f, 4.0f, 3.0f, 0.0f};
    for (int lane = 0; lane < 4; ++lane) {
        a._f32[lane] = (float)(lane + 1);
        b._f32[lane] = (float)(4 - lane);
        c._f32[lane] = (float)(-lane - 1);
    }
    u128 distinct_lanes = spu_fma_k(a, b, c, 7);
    for (int lane = 0; lane < 4; ++lane)
        failures += distinct_lanes._f32[lane] != lane_results[lane];
    printf("SPU masked float ops: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
