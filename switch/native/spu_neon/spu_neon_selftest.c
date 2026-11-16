/* spu_neon_selftest.c -- differential self-test of the fast SPU helper bodies against their scalar references.
 *
 *   int ps3_spu_neon_selftest(char *report, unsigned report_bytes);
 *
 * For every helper that has a NEON body (spu_helpers.h under SPU__NEON, spu_context.h LS accessors) -- and, on a
 * host build, every helper that has an SSE body -- this runs SPU_NEON_SELFTEST_ITER (default 32768) pseudo-random
 * inputs (fixed seed, edge values, every shift-count class, every shufb selector class) through the fast function
 * and through its scalar reference and compares all 128 bits. The scalar reference is spu_<op>_ref from
 * spu_helpers.h (the previous scalar body of the op); the LS accessors are compared against a byte-wise
 * big-endian reference written here.
 *
 * Report (NUL-terminated, fits report_bytes; failing ops are listed before passing ones if space runs out):
 *   NEON_SELFTEST_INFO backend=<neon|sse|scalar> float=<neon|ref> ls=<neon|sse|scalar> iterations=<n> seed=0x...
 *   NEON_SELFTEST op=<name> mismatches=<n> first_a=<fast result> first_b=<reference result> in=<inputs>
 *   NEON_SELFTEST_TOTAL ops=<n> mismatching_ops=<n>
 * Words are printed w0_w1_w2_w3 (w0 = _u32[0] = SPU word 0). Returns the number of mismatching ops (0 = PASS).
 *
 * Build for the host (SSE paths, proves the harness):
 *   cc -O2 -msse4.1 -DSPU_NEON_SELFTEST_MAIN -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL \
 *      -I source/ps3recomp/runtime/spu -I source/ps3recomp/include native/spu_neon/spu_neon_selftest.c -lm
 * Build for the host against the NEON bodies (portable emulation of <arm_neon.h>, see neon_shim/arm_neon.h):
 *   cc -O2 -U__SSE2__ -U__SSE__ -U__SSE3__ -U__SSSE3__ -U__SSE4_1__ -U__AVX__ -U__AVX2__ -D__ARM_NEON=1 \
 *      -DSPU_NEON_SHIM -I native/spu_neon/neon_shim ... (same as above)
 * On the console it is linked into the program like any other unit.
 */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>

#ifndef SPU_NEON_SELFTEST_ITER
#define SPU_NEON_SELFTEST_ITER 32768u
#endif
#define SELFTEST_SEED 0x5EED0F5C0DEC0DEDull

/* ---- symbols spu_context.h / spu_helpers.h reference that the program normally gets from spu_channels.c --------
 * Weak, so the real definitions win whenever they are linked in (the console program); alone (host build) these
 * keep the self-test self-contained. */
#if defined(__GNUC__)
__attribute__((weak)) int g_spu_ls_watch_n = 0;
__attribute__((weak)) void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }
__attribute__((weak)) unsigned* spu_ls_watch_list(int* out_n)
{ static unsigned none[4]; if (out_n) *out_n = 0; return none; }
#if !defined(PS3_NATIVE_SPU_EXECUTOR)
__attribute__((weak)) int g_spu_xfloat = 1;
__attribute__((weak)) uint32_t g_spu_xflim[8] = {0xFF7FFFFFu,0xFF7FFFFFu,0xFF7FFFFFu,0xFF7FFFFFu,
                                                  0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu,0x7F7FFFFFu};
#endif
#endif

/* ---- deterministic generator (splitmix64) ------------------------------------------------------------------- */
static uint64_t g_rng;
static uint64_t rnd64(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint32_t rnd32(void) { return (uint32_t)(rnd64() >> 16); }

static const uint32_t k_w32[] = { 0u, 1u, 2u, 3u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 255u, 256u,
    0x7FFFu, 0x8000u, 0x8001u, 0xFFFFu, 0x10000u, 0x00010001u, 0x7FFF7FFFu, 0x80008000u, 0xFFFF0000u, 0x0000FFFFu,
    0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFC1u, 0xFFFFFFE0u, 0xFFFFFFE1u, 0xFFFFFFF0u,
    0x7F7F7F7Fu, 0x80808080u, 0x01010101u, 0xFEFEFEFEu, 0xAAAAAAAAu, 0x55555555u, 0x00FF00FFu, 0xFF00FF00u,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x00800000u, 0x007FFFFFu,
    0x80000001u, 0x3F800000u, 0xBF800000u, 0x00000001u };
#define K_W32 (sizeof k_w32 / sizeof k_w32[0])
static const uint8_t k_b8[] = { 0x00, 0x01, 0x02, 0x0F, 0x10, 0x1F, 0x20, 0x3F, 0x40, 0x7E, 0x7F, 0x80, 0x81, 0xBF, 0xC0, 0xDF,
                                0xE0, 0xFE, 0xFF, 0x55, 0xAA };

/* one operand, lane distribution picked per call */
static u128 gen_u128(void)
{
    u128 r;
    switch (rnd32() % 12) {
    case 0: case 1: r._u64[0] = rnd64(); r._u64[1] = rnd64(); break;                       /* fully random */
    case 2: case 3: for (int i = 0; i < 4; i++) r._u32[i] = k_w32[rnd32() % K_W32]; break;  /* word edge values */
    case 4: for (int i = 0; i < 16; i++) r._u8[i] = k_b8[rnd32() % sizeof k_b8]; break;     /* byte edge values */
    case 5: for (int i = 0; i < 8; i++) r._u16[i] = (uint16_t)k_w32[rnd32() % K_W32]; break; /* halfword edge values */
    case 6: for (int i = 0; i < 4; i++) r._u32[i] = rnd32() % 80u; break;                   /* small counts */
    case 7: for (int i = 0; i < 4; i++) r._u32[i] = 0u - (rnd32() % 80u); break;            /* negative small counts */
    case 8: for (int i = 0; i < 8; i++) r._u16[i] = (uint16_t)(rnd32() % 40u); break;       /* small halfword counts */
    case 9: for (int i = 0; i < 8; i++) r._u16[i] = (uint16_t)(0u - (rnd32() % 40u)); break;
    case 10: { uint32_t v = (rnd32() & 1) ? 0xFFFFFFFFu : 0u; for (int i = 0; i < 4; i++) r._u32[i] = (rnd32() & 3) ? v : rnd32(); break; }
    default: for (int i = 0; i < 4; i++) r._u32[i] = (rnd32() & 1) ? k_w32[rnd32() % K_W32] : rnd32(); break;
    }
    return r;
}
/* shufb control: every selector class (00x/01x, 10x, 110, 111) in every byte, plus insertion-style immediates */
static u128 gen_ctl(void)
{
    u128 r;
    if ((rnd32() & 7) == 0) return gen_u128();
    for (int i = 0; i < 16; i++) {
        uint32_t cls = rnd32() & 7;                                 /* top three bits of the selector */
        r._u8[i] = (uint8_t)((cls << 5) | (rnd32() & 0x1F));
    }
    return r;
}
/* immediate / shift-count operand */
static int32_t gen_imm(void)
{
    switch (rnd32() & 7) {
    case 0: case 1: return (int32_t)(rnd32() % 161u) - 80;
    case 2: return (int32_t)k_w32[rnd32() % K_W32];
    case 3: return (int32_t)(rnd32() % 1024u) - 512;
    default: return (int32_t)rnd32();
    }
}

/* ---- result bookkeeping --------------------------------------------------------------------------------------- */
typedef struct { const char *name; uint32_t bad; char fa[40], fb[40], in[176]; } sres;
static sres g_res[256];
static int g_nres;

static sres *begin_op(const char *name)
{
    sres *r = &g_res[g_nres++];
    memset(r, 0, sizeof *r); r->name = name; strcpy(r->fa, "-"); strcpy(r->fb, "-");
    return r;
}
static void hex128(char *o, u128 v) { sprintf(o, "%08x_%08x_%08x_%08x", v._u32[0], v._u32[1], v._u32[2], v._u32[3]); }
static void note(sres *r, u128 x, u128 y, u128 a, u128 b, u128 c, long imm)
{
    if (r->bad++) return;
    char ha[40], hb[40], hc[40];
    hex128(r->fa, x); hex128(r->fb, y); hex128(ha, a); hex128(hb, b); hex128(hc, c);
    snprintf(r->in, sizeof r->in, "a=%s b=%s c=%s imm=%ld", ha, hb, hc, imm);
}
#define NEQ(x, y) (memcmp(&(x), &(y), 16) != 0)
#define ITER SPU_NEON_SELFTEST_ITER

/* ---- per-signature drivers ---------------------------------------------------------------------------------- */
#define T_U1(N) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
    u128 a = gen_u128(), z = a; u128 x = spu_##N(a), y = spu_##N##_ref(a); if (NEQ(x, y)) note(r_, x, y, a, z, z, 0); } } while (0)
#define T_U2(N) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
    u128 a = gen_u128(), b = gen_u128(); u128 x = spu_##N(a, b), y = spu_##N##_ref(a, b); if (NEQ(x, y)) note(r_, x, y, a, b, b, 0); } } while (0)
#define T_U3(N) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
    u128 a = gen_u128(), b = gen_u128(), c = gen_u128(); u128 x = spu_##N(a, b, c), y = spu_##N##_ref(a, b, c); if (NEQ(x, y)) note(r_, x, y, a, b, c, 0); } } while (0)
#define T_UI(N) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
    u128 a = gen_u128(); int32_t m = gen_imm(); u128 x = spu_##N(a, m), y = spu_##N##_ref(a, m); if (NEQ(x, y)) note(r_, x, y, a, a, a, m); } } while (0)
#define T_US(N) T_UI(N)
/* ops whose scalar reference has another name / shape */
#define T_U2X(N, REF) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
    u128 a = gen_u128(), b = gen_u128(); u128 x = spu_##N(a, b), y = REF(a, b); if (NEQ(x, y)) note(r_, x, y, a, b, b, 0); } } while (0)

/* ---- local-store accessors against a byte-wise big-endian reference -------------------------------------------- */
static u128 ref_ls_read(const uint8_t *ls, uint32_t lsa)
{
    const uint8_t *p = ls + (lsa & SPU_LS_MASK & ~0xFu);
    u128 v;
    for (int i = 0; i < 4; i++)
        v._u32[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4 + 1] << 16) | ((uint32_t)p[i*4 + 2] << 8) | (uint32_t)p[i*4 + 3];
    return v;
}
static void test_ls(void)
{
    sres *rr = begin_op("ls_read128"), *rw = begin_op("ls_write128");
    spu_context *ctx = (spu_context *)calloc(1, sizeof *ctx);
    if (!ctx) { rr->bad = rw->bad = 1; strcpy(rr->fa, "calloc-failed"); strcpy(rw->fa, "calloc-failed"); return; }
    ctx->ls = ctx->ls_store;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 8) { uint64_t v = rnd64(); memcpy(ctx->ls + i, &v, 8); }
    for (uint32_t it = 0; it < ITER; it++) {
        uint32_t lsa;
        switch (rnd32() & 7) {                       /* aligned, unaligned, edges, beyond 256 KB (masked) */
        case 0: lsa = (rnd32() % (SPU_LS_SIZE / 16)) * 16u; break;
        case 1: lsa = rnd32() % SPU_LS_SIZE; break;
        case 2: lsa = SPU_LS_SIZE - 16u + (rnd32() & 15u); break;
        case 3: lsa = rnd32() & 15u; break;
        case 4: lsa = rnd32(); break;
        case 5: lsa = 0x1320u; break;
        default: lsa = (rnd32() % (SPU_LS_SIZE / 16)) * 16u + (rnd32() & 15u); break;
        }
        u128 x = spu_ls_read128(ctx, lsa), y = ref_ls_read(ctx->ls, lsa);
        if (NEQ(x, y)) { u128 z; memset(&z, 0, sizeof z); note(rr, x, y, z, z, z, (long)lsa); }

        u128 v = gen_u128();
        uint32_t base = lsa & SPU_LS_MASK & ~0xFu;
        uint32_t start = base >= 16 ? base - 16 : 0;
        if (start + 48 > SPU_LS_SIZE) start = SPU_LS_SIZE - 48;
        uint8_t want[48];
        memcpy(want, ctx->ls + start, 48);
        for (int w = 0; w < 4; w++) {                /* big-endian image of v at base */
            uint32_t q = v._u32[w]; uint8_t *d = want + (base - start) + w * 4;
            d[0] = (uint8_t)(q >> 24); d[1] = (uint8_t)(q >> 16); d[2] = (uint8_t)(q >> 8); d[3] = (uint8_t)q;
        }
        spu_ls_write128(ctx, lsa, v);
        if (memcmp(want, ctx->ls + start, 48)) {
            u128 gx, gy; memcpy(&gx, ctx->ls + base, 16); memcpy(&gy, want + (base - start), 16);
            note(rw, gx, gy, v, v, v, (long)lsa);
        }
    }
    free(ctx);
}

/* ---- the run --------------------------------------------------------------------------------------------------- */
static void run_all(void)
{
    g_rng = SELFTEST_SEED; g_nres = 0;

    /* constructors */
    { sres *r_ = begin_op("splat_u32"); for (uint32_t i = 0; i < ITER; i++) { uint32_t v = (i & 1) ? rnd32() : k_w32[rnd32() % K_W32]; u128 x = spu_splat_u32(v), y = spu_splat_u32_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("splat_u16"); for (uint32_t i = 0; i < ITER; i++) { uint16_t v = (uint16_t)((i & 1) ? rnd32() : k_w32[rnd32() % K_W32]); u128 x = spu_splat_u16(v), y = spu_splat_u16_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("splat_u8");  for (uint32_t i = 0; i < ITER; i++) { uint8_t v = (uint8_t)rnd32(); u128 x = spu_splat_u8(v), y = spu_splat_u8_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("link");      for (uint32_t i = 0; i < ITER; i++) { uint32_t v = (i & 1) ? rnd32() : k_w32[rnd32() % K_W32]; u128 x = spu_link(v), y = spu_link_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("pref_u32");  for (uint32_t i = 0; i < ITER; i++) { uint32_t v = (i & 1) ? rnd32() : k_w32[rnd32() % K_W32]; u128 x = spu_pref_u32(v), y = spu_pref_u32_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("il");   for (uint32_t i = 0; i < ITER; i++) { int32_t v = gen_imm(); u128 x = spu_il(v), y = spu_il_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, v); } }
    { sres *r_ = begin_op("ila");  for (uint32_t i = 0; i < ITER; i++) { uint32_t v = rnd32(); u128 x = spu_ila(v), y = spu_ila_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("ilh");  for (uint32_t i = 0; i < ITER; i++) { uint16_t v = (uint16_t)rnd32(); u128 x = spu_ilh(v), y = spu_ilh_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("ilhu"); for (uint32_t i = 0; i < ITER; i++) { uint16_t v = (uint16_t)rnd32(); u128 x = spu_ilhu(v), y = spu_ilhu_ref(v), z = x; if (NEQ(x, y)) note(r_, x, y, z, z, z, (long)v); } }
    { sres *r_ = begin_op("iohl"); for (uint32_t i = 0; i < ITER; i++) { u128 a = gen_u128(); uint16_t v = (uint16_t)rnd32(); u128 x = spu_iohl(a, v), y = spu_iohl_ref(a, v); if (NEQ(x, y)) note(r_, x, y, a, a, a, (long)v); } }

    /* integer arithmetic */
    T_U2(a); T_U2(sf); T_U2(ah); T_U2(sfh); T_U2(cg); T_U2(bg);
    T_UI(ai); T_UI(ahi); T_UI(sfi); T_UI(sfhi);
    T_U3(sfx); T_U3(addx); T_U3(cgx); T_U3(bgx);
    /* multiplies */
    T_U2(mpy); T_U2(mpyu); T_U2(mpyh); T_U2(mpyhh); T_U2(mpyhhu); T_U2(mpys);
    T_U3(mpya); T_U3(mpyhha); T_U3(mpyhhau); T_UI(mpyi); T_UI(mpyui);
    /* logic / select */
    T_U2(and); T_U2(or); T_U2(xor); T_U2(nand); T_U2(nor); T_U2(andc); T_U2(orc); T_U2(eqv);
    T_UI(andi); T_UI(ori); T_UI(xori); T_UI(andhi); T_UI(orhi); T_UI(xorhi); T_UI(andbi); T_UI(orbi); T_UI(xorbi);
    T_U3(selb);
    /* counts, sign extension, byte ops, horizontals */
    T_U1(clz); T_U1(xsbh); T_U1(xshw); T_U1(xswd); T_U1(orx);
    { sres *r_ = begin_op("cntb"); for (uint32_t i = 0; i < ITER; i++) { u128 a = gen_u128(); u128 x = spu_cntb(a), y = spu_cntb_ref(a); if (NEQ(x, y)) note(r_, x, y, a, a, a, 0); } }
    T_U2(absdb); T_U2(avgb); T_U2(sumb);
    /* compares */
    T_U2(ceq); T_U2(ceqh); T_U2(ceqb); T_U2(cgt); T_U2(cgth); T_U2(cgtb); T_U2(clgt); T_U2(clgth); T_U2(clgtb);
    T_UI(ceqi); T_UI(cgti); T_UI(clgti); T_UI(ceqbi); T_UI(ceqhi); T_UI(clgtbi); T_UI(clgthi); T_UI(cgthi); T_UI(cgtbi);
    /* shifts / rotates: immediate and register counts (counts beyond the lane width, negative counts) */
    T_US(shli); T_US(shlhi); T_US(roti); T_US(rothi); T_US(rotmi); T_US(rotmai); T_US(rotmhi); T_US(rothmi); T_US(rotmahi);
    T_U2(shl); T_U2(shlh); T_U2(rot); T_U2(roth); T_U2(rotm); T_U2(rotma); T_U2(rothm); T_U2(rothma);
    /* quadword bit shifts */
    T_US(shlqbii); T_US(rotqbii); T_US(rotqmbii); T_U2(shlqbi); T_U2(rotqbi); T_U2(rotqmbi);
    /* quadword byte shifts / rotates (pshufb / tbl tables) */
    T_US(shlqbyi); T_US(rotqbyi); T_US(rotqmbyi);
    T_U2(shlqby); T_U2(rotqby); T_U2(rotqmby); T_U2(shlqbybi); T_U2(rotqbybi); T_U2(rotqmbybi);

    /* shufb: random controls of every selector class, then every selector byte value in every position */
    {
        sres *r_ = begin_op("shufb");
        for (uint32_t it = 0; it < ITER; it++) {
            u128 a = gen_u128(), b = gen_u128(), c = gen_ctl();
            u128 x = spu_shufb(a, b, c), y = spu_shufb_ref(a, b, c);
            if (NEQ(x, y)) note(r_, x, y, a, b, c, 0);
        }
        for (unsigned v = 0; v < 256; v++) {
            for (int pos = -1; pos < 16; pos++) {      /* pos -1: all sixteen bytes carry v */
                for (int rep = 0; rep < 4; rep++) {
                    u128 a = gen_u128(), b = gen_u128(), c = gen_ctl();
                    if (pos < 0) memset(&c, (int)v, 16); else c._u8[pos] = (uint8_t)v;
                    u128 x = spu_shufb(a, b, c), y = spu_shufb_ref(a, b, c);
                    if (NEQ(x, y)) note(r_, x, y, a, b, c, (long)v);
                }
            }
        }
    }
    /* the byte-count quadword ops over their whole count range (0..63 and the negatives) */
    {
        sres *r_ = begin_op("qword_byte_ops_all_counts");
        for (int n = -80; n < 80; n++) {
            for (int rep = 0; rep < 8; rep++) {
                u128 a = gen_u128(), b = gen_u128(); b._u32[0] = (uint32_t)n << ((rep & 1) ? 3 : 0);
                u128 x, y;
                x = spu_shlqby(a, b);    y = spu_shlqby_ref(a, b);    if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqby(a, b);    y = spu_rotqby_ref(a, b);    if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqmby(a, b);   y = spu_rotqmby_ref(a, b);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_shlqbybi(a, b);  y = spu_shlqbybi_ref(a, b);  if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqbybi(a, b);  y = spu_rotqbybi_ref(a, b);  if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqmbybi(a, b); y = spu_rotqmbybi_ref(a, b); if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_shlqbyi(a, n);   y = spu_shlqbyi_ref(a, n);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqbyi(a, n);   y = spu_rotqbyi_ref(a, n);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqmbyi(a, n);  y = spu_rotqmbyi_ref(a, n);  if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_shlqbii(a, n);   y = spu_shlqbii_ref(a, n);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqbii(a, n);   y = spu_rotqbii_ref(a, n);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqmbii(a, n);  y = spu_rotqmbii_ref(a, n);  if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_shlqbi(a, b);    y = spu_shlqbi_ref(a, b);    if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqbi(a, b);    y = spu_rotqbi_ref(a, b);    if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
                x = spu_rotqmbi(a, b);   y = spu_rotqmbi_ref(a, b);   if (NEQ(x, y)) note(r_, x, y, a, b, b, n);
            }
        }
    }

    /* insertion controls (tables): positions beyond 0..15, negative, and the a+i7 / a+b forms */
    {
        sres *r1 = begin_op("cbd_pos"), *r2 = begin_op("chd_pos"), *r3 = begin_op("cwd_pos"), *r4 = begin_op("cdd_pos");
        sres *r5 = begin_op("cbd"), *r6 = begin_op("chd"), *r7 = begin_op("cwd"), *r8 = begin_op("cdd");
        sres *r9 = begin_op("cbx"), *r10 = begin_op("chx"), *r11 = begin_op("cwx"), *r12 = begin_op("cdx");
        for (uint32_t it = 0; it < ITER; it++) {
            int pos = (it < 256) ? (int)it - 128 : (int)gen_imm();
            u128 a = gen_u128(), b = gen_u128(), z = a;
            int i7 = (int)(gen_imm() % 64);
            u128 x, y;
            x = spu_cbd_pos(pos); y = spu_cbd_pos_ref(pos); if (NEQ(x, y)) note(r1, x, y, z, z, z, pos);
            x = spu_chd_pos(pos); y = spu_chd_pos_ref(pos); if (NEQ(x, y)) note(r2, x, y, z, z, z, pos);
            x = spu_cwd_pos(pos); y = spu_cwd_pos_ref(pos); if (NEQ(x, y)) note(r3, x, y, z, z, z, pos);
            x = spu_cdd_pos(pos); y = spu_cdd_pos_ref(pos); if (NEQ(x, y)) note(r4, x, y, z, z, z, pos);
            x = spu_cbd(a, i7); y = spu_cbd_pos_ref((int)a._u32[0] + i7); if (NEQ(x, y)) note(r5, x, y, a, a, a, i7);
            x = spu_chd(a, i7); y = spu_chd_pos_ref((int)a._u32[0] + i7); if (NEQ(x, y)) note(r6, x, y, a, a, a, i7);
            x = spu_cwd(a, i7); y = spu_cwd_pos_ref((int)a._u32[0] + i7); if (NEQ(x, y)) note(r7, x, y, a, a, a, i7);
            x = spu_cdd(a, i7); y = spu_cdd_pos_ref((int)a._u32[0] + i7); if (NEQ(x, y)) note(r8, x, y, a, a, a, i7);
            x = spu_cbx(a, b); y = spu_cbd_pos_ref((int)(a._u32[0] + b._u32[0])); if (NEQ(x, y)) note(r9, x, y, a, b, b, 0);
            x = spu_chx(a, b); y = spu_chd_pos_ref((int)(a._u32[0] + b._u32[0])); if (NEQ(x, y)) note(r10, x, y, a, b, b, 0);
            x = spu_cwx(a, b); y = spu_cwd_pos_ref((int)(a._u32[0] + b._u32[0])); if (NEQ(x, y)) note(r11, x, y, a, b, b, 0);
            x = spu_cdx(a, b); y = spu_cdd_pos_ref((int)(a._u32[0] + b._u32[0])); if (NEQ(x, y)) note(r12, x, y, a, b, b, 0);
        }
    }

    /* single precision (bit-for-bit, including exponent-255 lanes, zeros, denormals) */
    T_U2(fa); T_U2(fs); T_U2(fm); T_U3(fma); T_U3(fms); T_U3(fnms);
    T_U2(fceq); T_U2(fcgt); T_U2(fcmeq); T_U2(fcmgt);
#if SPU__NEONF
    /* Decision 126: the RI8 conversions, i8 over its whole 0..255 range (int inputs both random and small) */
#define T_CONV(N) do { sres *r_ = begin_op(#N); for (uint32_t it_ = 0; it_ < ITER; it_++) { \
        u128 a = gen_u128(); if (it_ & 2u) for (int l_ = 0; l_ < 4; l_++) a._s32[l_] = (int32_t)(rnd32() % 65536u) - 32768; \
        const int m = (int)(rnd32() & 0xFFu); u128 x = spu_##N(a, m), y = spu_##N##_ref(a, m); if (NEQ(x, y)) note(r_, x, y, a, a, a, m); } } while (0)
    T_CONV(cflts); T_CONV(cfltu); T_CONV(csflt); T_CONV(cuflt);
#undef T_CONV
#endif

    /* local store */
    test_ls();
}

/* ---- report ---------------------------------------------------------------------------------------------------- */
typedef struct { char *buf; unsigned cap, len; } rep_t;
static int rep_fits(const rep_t *r, const char *line, unsigned reserve) { return r->len + (unsigned)strlen(line) + reserve < r->cap; }
static void rep_put(rep_t *r, const char *line) { size_t n = strlen(line); memcpy(r->buf + r->len, line, n); r->len += (unsigned)n; r->buf[r->len] = 0; }

int ps3_spu_neon_selftest(char *report, unsigned report_bytes)
{
    run_all();

    int bad_ops = 0;
    for (int i = 0; i < g_nres; i++) if (g_res[i].bad) bad_ops++;

    const char *backend =
#if SPU__NEON
        "neon";
#elif SPU__SIMD
        "sse";
#else
        "scalar";
#endif
    const char *fl =
#if SPU__NEONF
        "neon";
#elif defined(SPU__FSIMD) && SPU__FSIMD
        "sse";
#else
        "ref";
#endif
    const char *ls =
#if SPU__NEON
        "neon";
#elif SPU_LS_SIMD
        "sse";
#else
        "scalar";
#endif
    if (report && report_bytes) {
        rep_t rp = { report, report_bytes, 0 }; report[0] = 0;
        char line[400];
        snprintf(line, sizeof line, "NEON_SELFTEST_INFO backend=%s float=%s ls=%s iterations=%u seed=0x%016llx\n",
                 backend, fl, ls, (unsigned)ITER, (unsigned long long)SELFTEST_SEED);
        if (rep_fits(&rp, line, 160)) rep_put(&rp, line);
        int dropped = 0;
        for (int pass = 0; pass < 2; pass++) {              /* failing ops first, then passing ones while there is room */
            for (int i = 0; i < g_nres; i++) {
                const sres *s = &g_res[i];
                if ((pass == 0) != (s->bad != 0)) continue;
                if (s->bad) snprintf(line, sizeof line, "NEON_SELFTEST op=%s mismatches=%u first_a=%s first_b=%s %s\n", s->name, s->bad, s->fa, s->fb, s->in);
                else        snprintf(line, sizeof line, "NEON_SELFTEST op=%s mismatches=0 first_a=- first_b=-\n", s->name);
                if (rep_fits(&rp, line, 160)) rep_put(&rp, line); else dropped++;
            }
        }
        if (dropped) { snprintf(line, sizeof line, "NEON_SELFTEST_INFO dropped_lines=%d (report buffer too small)\n", dropped); if (rep_fits(&rp, line, 100)) rep_put(&rp, line); }
        snprintf(line, sizeof line, "NEON_SELFTEST_TOTAL ops=%d mismatching_ops=%d\n", g_nres, bad_ops);
        if (rp.len + strlen(line) + 1 > rp.cap) rp.len = rp.cap > strlen(line) + 1 ? rp.cap - (unsigned)strlen(line) - 1 : 0;   /* the total line always fits */
        rep_put(&rp, line);
    }
    return bad_ops;
}

#ifdef SPU_NEON_SELFTEST_MAIN
int main(void)
{
    static char rep[1 << 17];
    int bad = ps3_spu_neon_selftest(rep, sizeof rep);
    fputs(rep, stdout);
    return bad ? 1 : 0;
}
#endif
