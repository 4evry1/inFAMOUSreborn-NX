/* Native SPU single-precision float policy contract (review finding F1).
 *
 * Natively (-DPS3_NATIVE_SPU_EXECUTOR) the approximate-xfloat clamp is a compile-time policy: exponent-255 operands
 * are clamped to +-FLT_MAX (sign kept) before every float op, everything else passes bit-for-bit. There is no
 * process-global switch (g_spu_xfloat / g_spu_xflim): this TU is linked WITHOUT any definition of them, so a native
 * helper that still reads one fails to link.
 *
 * verify_spu_float_policy.sh builds it as three host variants:
 *   portable: SSE feature macros undefined (-U__SSE2__ ...), so spu_helpers.h compiles the scalar !SPU__FSIMD
 *             branch, which is the branch an AArch64 build compiles (__SSE2__ is not defined there). The code still
 *             runs on the x86 host: this is HOST behaviour of the portable branch, not AArch64 execution.
 *   sse2:     SPU__FSIMD with the SSE2 mask/blend spu__xfq;  sse41: SPU__FSIMD with compile-time-limit spu__xfq.
 * Argument "exhaustive": every 2^32 bit pattern through the existing scalar helper spu_xf, the native spu_xfv (and
 * spu__xfq where compiled) vs spec(), so the native lane form is bit-identical to spu_xf everywhere.
 * Denormal ARITHMETIC lanes run in the host default FP environment (no FTZ/DAZ) and say nothing about the SPU mode. */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PS3_NATIVE_SPU_EXECUTOR
#error "native float policy contract: build with -DPS3_NATIVE_SPU_EXECUTOR"
#endif
#if SPU__FSIMD && defined(__SSE4_1__)
#define BRANCH "sse41"
#elif SPU__FSIMD
#define BRANCH "sse2"
#else
#define BRANCH "portable"
#endif

static unsigned g_checks;
static uint32_t spec(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u ? (u & 0x80000000u) | 0x7F7FFFFFu : u; }
static u128 lanes(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0]=a; r._u32[1]=b; r._u32[2]=c; r._u32[3]=d; return r; }
static u128 splat(uint32_t a) { return lanes(a, a, a, a); }
static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void expect(const char *what, u128 got, u128 want) {
    ++g_checks;
    if (memcmp(&got, &want, 16)) {
        fprintf(stderr, "FLOAT_POLICY_FAIL branch=%s %s got=%08x %08x %08x %08x want=%08x %08x %08x %08x\n", BRANCH, what,
                got._u32[0], got._u32[1], got._u32[2], got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
        exit(1);
    }
}

enum { PINF=0x7F800000u, NINF=0xFF800000u, QNAN=0x7FC00000u, NQNAN=0xFFC00000u, SNAN=0x7F800001u, ALL1=0xFFFFFFFFu,
       PMAXNAN=0x7FFFFFFFu, FMAX=0x7F7FFFFFu, NFMAX=0xFF7FFFFFu, ONE=0x3F800000u, NONE=0xBF800000u, DMIN=0x00000001u,
       NDMAX=0x807FFFFFu, PZERO=0, NZERO=0x80000000u, NMIN=0x00800000u };

/* Clamp itself, lane by lane (lanes are independent: mixed classes in one register). */
static void clamp_lanes(void) {
    static const uint32_t cases[] = { PINF, NINF, QNAN, NQNAN, SNAN, ALL1, PMAXNAN, 0x7FA00000u, 0xFFB00001u,
                                      FMAX, NFMAX, ONE, NONE, NMIN, DMIN, NDMAX, 0x007FFFFFu, PZERO, NZERO, 0x7F000000u };
    const unsigned n = sizeof cases / sizeof cases[0];
    for (unsigned i = 0; i < n; ++i) {
        ++g_checks;
        if (bits(spu_xf(f32(cases[i]))) != spec(cases[i])) { fprintf(stderr, "FLOAT_POLICY_FAIL spu_xf %08x\n", cases[i]); exit(1); }
        for (unsigned rot = 0; rot < 4; ++rot) {
            const uint32_t a = cases[(i + rot) % n], b = cases[(i + rot + 1) % n], c = cases[(i + rot + 2) % n], d = cases[(i + rot + 3) % n];
            expect("spu_xfv", spu_xfv(lanes(a, b, c, d)), lanes(spec(a), spec(b), spec(c), spec(d)));
        }
    }
}

/* Every op that clamps its operands, on the branch this variant compiled. Each case differs from raw IEEE. */
static void op_semantics(void) {
    expect("fm 0*Inf=+0", spu_fm(splat(PZERO), lanes(PINF, QNAN, SNAN, ALL1)), lanes(PZERO, PZERO, PZERO, NZERO));
    expect("fm -0*Inf=-0", spu_fm(splat(NZERO), lanes(PINF, NINF, QNAN, NQNAN)), lanes(NZERO, PZERO, NZERO, PZERO));
    expect("fm 1*Inf=FMAX", spu_fm(splat(ONE), lanes(PINF, NINF, QNAN, NQNAN)), lanes(FMAX, NFMAX, FMAX, NFMAX));
    expect("fa Inf+-Inf=+0", spu_fa(lanes(PINF, QNAN, NINF, ONE), lanes(NINF, NQNAN, PINF, NONE)), splat(PZERO));
    expect("fs NaN-FMAX=0", spu_fs(lanes(QNAN, PINF, NINF, NQNAN), lanes(FMAX, FMAX, NFMAX, NFMAX)), splat(PZERO));
    expect("fma 0*Inf+1=1", spu_fma(splat(PZERO), lanes(PINF, QNAN, NINF, NQNAN), splat(ONE)), splat(ONE));
    expect("fms 1*1-Inf", spu_fms(splat(ONE), splat(ONE), lanes(PINF, NINF, QNAN, PZERO)), lanes(bits(1.0f - f32(FMAX)), bits(1.0f + f32(FMAX)), bits(1.0f - f32(FMAX)), ONE));
    expect("fnms Inf-0*Inf", spu_fnms(splat(PZERO), splat(PINF), lanes(PINF, NINF, ONE, NZERO)), lanes(FMAX, NFMAX, ONE, NZERO));
    expect("fceq NaN==NaN", spu_fceq(lanes(QNAN, PINF, NINF, ONE), lanes(QNAN, FMAX, NQNAN, ONE)), lanes(ALL1, ALL1, ALL1, ALL1));
    expect("fcgt Inf>FMAX false", spu_fcgt(lanes(PINF, QNAN, FMAX, NINF), lanes(FMAX, FMAX, PINF, NFMAX)), lanes(0, 0, 0, 0));
    expect("fcgt FMAX>-Inf", spu_fcgt(lanes(PINF, ONE, PZERO, NZERO), lanes(NINF, NQNAN, NZERO, PZERO)), lanes(ALL1, ALL1, 0, 0));
    expect("fcmeq |-Inf|==|NaN|", spu_fcmeq(lanes(NINF, NQNAN, PINF, ONE), lanes(QNAN, FMAX, NFMAX, NONE)), splat(ALL1));
    expect("fcmgt |Inf|>|FMAX| false", spu_fcmgt(lanes(PINF, NQNAN, ONE, NINF), lanes(NFMAX, FMAX, NINF, ONE)), lanes(0, 0, 0, ALL1));
    /* Conversions: a clamped operand saturates instead of converting NaN (undefined in C). */
    expect("cflts sat", spu_cflts(lanes(PINF, QNAN, NINF, NQNAN), 173), lanes(0x7FFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x80000000u));
    expect("cfltu sat", spu_cfltu(lanes(PINF, QNAN, NINF, NQNAN), 173), lanes(0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0));
    /* Finite, denormal and signed-zero operands pass the clamp unchanged (host default FP env: no FTZ/DAZ). */
    expect("fa finite/denormal/zero", spu_fa(lanes(ONE, DMIN, NDMAX, NZERO), lanes(PZERO, NZERO, NZERO, NZERO)), lanes(ONE, DMIN, NDMAX, NZERO));
    expect("fm finite/denormal/zero", spu_fm(lanes(FMAX, DMIN, NZERO, NFMAX), lanes(ONE, ONE, ONE, NONE)), lanes(FMAX, DMIN, NZERO, FMAX));
}

static int exhaustive(void) {
    for (uint64_t u = 0; u <= 0xFFFFFFFFull; u += 4) {
        const uint32_t a = (uint32_t)u;
        const u128 in = lanes(a, a + 1, a + 2, a + 3), want = lanes(spec(a), spec(a + 1), spec(a + 2), spec(a + 3));
        for (unsigned k = 0; k < 4; ++k)  /* the existing scalar helper spu_xf defines the semantics */
            if (bits(spu_xf(f32(a + k))) != want._u32[k]) { fprintf(stderr, "FLOAT_POLICY_FAIL exhaustive spu_xf at %08x\n", a + k); return 1; }
        u128 got = spu_xfv(in);
        if (memcmp(&got, &want, 16)) { fprintf(stderr, "FLOAT_POLICY_FAIL exhaustive spu_xfv at %08x\n", a); return 1; }
#if SPU__FSIMD
        got = spu__fst(spu__xfq(spu__fld(in)));
        if (memcmp(&got, &want, 16)) { fprintf(stderr, "FLOAT_POLICY_FAIL exhaustive spu__xfq at %08x\n", a); return 1; }
#endif
    }
    printf("FLOAT_POLICY_EXHAUSTIVE branch=%s patterns=4294967296 spu_xf=spec spu_xfv=spec%s\n", BRANCH, SPU__FSIMD ? " spu__xfq=spec" : "");
    return 0;
}

int main(int argc, char **argv) {
    clamp_lanes(); op_semantics();
    printf("FLOAT_POLICY_PASS branch=%s checks=%u clamp=compile_time_native global_switch=absent\n", BRANCH, g_checks);
    if (argc > 1 && !strcmp(argv[1], "exhaustive")) return exhaustive();
    return 0;
}
