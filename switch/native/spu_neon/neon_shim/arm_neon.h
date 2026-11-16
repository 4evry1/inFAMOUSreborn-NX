/* arm_neon.h -- PORTABLE HOST EMULATION of the AArch64 Advanced SIMD intrinsics the SPU helpers use.
 *
 * TEST ONLY. It lets native/spu_neon/spu_neon_selftest.c compile the NEON bodies of spu_helpers.h /
 * spu_context.h on an x86-64 host (-DSPU_NEON_SHIM -D__ARM_NEON=1 -I native/spu_neon/neon_shim, with the SSE macros
 * undefined) and run them against the scalar references. That checks the lane logic of every NEON body (which
 * intrinsic, which operand order, which shift count, which lane) -- it does NOT check the CPU: each emulated
 * intrinsic below implements the documented AArch64 semantics from the Arm Architecture Reference Manual
 * (e.g. USHL/SSHL take a signed per-lane count from the low byte, TBL returns 0 for an index outside the table,
 * FMLA/FMLS are fused), so a misreading here would hide a real bug. The console run of the same selftest is what
 * proves the real instructions. Never put this directory on an AArch64 include path.
 *
 * Only the intrinsics spu_helpers.h and spu_context.h use are provided. */
#ifndef SPU_NEON_SHIM_ARM_NEON_H
#define SPU_NEON_SHIM_ARM_NEON_H

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint8_t  uint8x16_t  __attribute__((vector_size(16)));
typedef int8_t   int8x16_t   __attribute__((vector_size(16)));
typedef uint16_t uint16x8_t  __attribute__((vector_size(16)));
typedef int16_t  int16x8_t   __attribute__((vector_size(16)));
typedef uint32_t uint32x4_t  __attribute__((vector_size(16)));
typedef int32_t  int32x4_t   __attribute__((vector_size(16)));
typedef float    float32x4_t __attribute__((vector_size(16)));
typedef uint16_t uint16x4_t  __attribute__((vector_size(8)));
typedef int16_t  int16x4_t   __attribute__((vector_size(8)));
typedef struct { uint8x16_t val[2]; } uint8x16x2_t;

#define SHIM_N(v) ((int)(sizeof(v) / sizeof((v)[0])))

/* ---- reinterpret ---- */
#define SHIM_RE(name, TO, FROM) static inline TO name(FROM v) { TO r; memcpy(&r, &v, sizeof r); return r; }
SHIM_RE(vreinterpretq_u8_u32,  uint8x16_t, uint32x4_t)
SHIM_RE(vreinterpretq_s8_u32,  int8x16_t,  uint32x4_t)
SHIM_RE(vreinterpretq_u16_u32, uint16x8_t, uint32x4_t)
SHIM_RE(vreinterpretq_s16_u32, int16x8_t,  uint32x4_t)
SHIM_RE(vreinterpretq_s32_u32, int32x4_t,  uint32x4_t)
SHIM_RE(vreinterpretq_f32_u32, float32x4_t, uint32x4_t)
SHIM_RE(vreinterpretq_u32_u8,  uint32x4_t, uint8x16_t)
SHIM_RE(vreinterpretq_u32_u16, uint32x4_t, uint16x8_t)
SHIM_RE(vreinterpretq_u32_s16, uint32x4_t, int16x8_t)
SHIM_RE(vreinterpretq_u32_s32, uint32x4_t, int32x4_t)
SHIM_RE(vreinterpretq_u32_f32, uint32x4_t, float32x4_t)
SHIM_RE(vreinterpretq_s16_u16, int16x8_t,  uint16x8_t)

/* ---- load / store ---- */
static inline uint8x16_t vld1q_u8(const uint8_t* p) { uint8x16_t r; memcpy(&r, p, 16); return r; }
static inline void vst1q_u8(uint8_t* p, uint8x16_t v) { memcpy(p, &v, 16); }

/* ---- duplicate ---- */
#define SHIM_DUPQ(name, T, E) static inline T name(E x) { T r; for (int i = 0; i < SHIM_N(r); i++) r[i] = x; return r; }
SHIM_DUPQ(vdupq_n_u8,  uint8x16_t, uint8_t)
SHIM_DUPQ(vdupq_n_s8,  int8x16_t,  int8_t)
SHIM_DUPQ(vdupq_n_u16, uint16x8_t, uint16_t)
SHIM_DUPQ(vdupq_n_s16, int16x8_t,  int16_t)
SHIM_DUPQ(vdupq_n_u32, uint32x4_t, uint32_t)
SHIM_DUPQ(vdupq_n_s32, int32x4_t,  int32_t)
SHIM_DUPQ(vdup_n_u16,  uint16x4_t, uint16_t)
SHIM_DUPQ(vdup_n_s16,  int16x4_t,  int16_t)

/* ---- lane access ---- */
static inline uint32x4_t vsetq_lane_u32(uint32_t x, uint32x4_t v, int lane) { v[lane] = x; return v; }
static inline uint32_t vgetq_lane_u32(uint32x4_t v, int lane) { return v[lane]; }

/* ---- element-wise arithmetic and logic (modular) ---- */
#define SHIM_BIN(name, T, EXPR) static inline T name(T a, T b) { T r; for (int i = 0; i < SHIM_N(r); i++) r[i] = (__typeof__(r[0]))(EXPR); return r; }
SHIM_BIN(vaddq_u16, uint16x8_t, a[i] + b[i])
SHIM_BIN(vaddq_u32, uint32x4_t, a[i] + b[i])
SHIM_BIN(vsubq_u16, uint16x8_t, a[i] - b[i])
SHIM_BIN(vsubq_u32, uint32x4_t, a[i] - b[i])
SHIM_BIN(vsubq_s16, int16x8_t,  (uint16_t)a[i] - (uint16_t)b[i])
SHIM_BIN(vsubq_s32, int32x4_t,  (uint32_t)a[i] - (uint32_t)b[i])
SHIM_BIN(vmulq_u32, uint32x4_t, a[i] * b[i])
SHIM_BIN(vandq_u8,  uint8x16_t, a[i] & b[i])
SHIM_BIN(vandq_u16, uint16x8_t, a[i] & b[i])
SHIM_BIN(vandq_u32, uint32x4_t, a[i] & b[i])
SHIM_BIN(vorrq_u8,  uint8x16_t, a[i] | b[i])
SHIM_BIN(vorrq_u16, uint16x8_t, a[i] | b[i])
SHIM_BIN(vorrq_u32, uint32x4_t, a[i] | b[i])
SHIM_BIN(veorq_u8,  uint8x16_t, a[i] ^ b[i])
SHIM_BIN(veorq_u16, uint16x8_t, a[i] ^ b[i])
SHIM_BIN(veorq_u32, uint32x4_t, a[i] ^ b[i])
SHIM_BIN(vbicq_u32, uint32x4_t, a[i] & ~b[i])
SHIM_BIN(vornq_u32, uint32x4_t, a[i] | ~b[i])
SHIM_BIN(vabdq_u8,  uint8x16_t, a[i] > b[i] ? a[i] - b[i] : b[i] - a[i])
SHIM_BIN(vrhaddq_u8, uint8x16_t, ((unsigned)a[i] + (unsigned)b[i] + 1u) >> 1)
SHIM_BIN(vminq_u32, uint32x4_t, a[i] < b[i] ? a[i] : b[i])
SHIM_BIN(vminq_s32, int32x4_t,  a[i] < b[i] ? a[i] : b[i])
static inline uint32x4_t vmvnq_u32(uint32x4_t a) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = ~a[i]; return r; }
static inline int16x8_t vnegq_s16(int16x8_t a) { int16x8_t r; for (int i = 0; i < 8; i++) r[i] = (int16_t)(0u - (uint16_t)a[i]); return r; }
static inline int32x4_t vnegq_s32(int32x4_t a) { int32x4_t r; for (int i = 0; i < 4; i++) r[i] = (int32_t)(0u - (uint32_t)a[i]); return r; }
static inline uint32x4_t vbslq_u32(uint32x4_t m, uint32x4_t a, uint32x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = (m[i] & a[i]) | (~m[i] & b[i]); return r; }

/* ---- compares: all ones / all zeros per lane ---- */
#define SHIM_CMP(name, T, R, OP) static inline R name(T a, T b) { R r; for (int i = 0; i < SHIM_N(r); i++) r[i] = (a[i] OP b[i]) ? (__typeof__(r[0]))~0 : 0; return r; }
SHIM_CMP(vceqq_u8,  uint8x16_t, uint8x16_t, ==)
SHIM_CMP(vceqq_u16, uint16x8_t, uint16x8_t, ==)
SHIM_CMP(vceqq_u32, uint32x4_t, uint32x4_t, ==)
SHIM_CMP(vcgtq_s8,  int8x16_t,  uint8x16_t, >)
SHIM_CMP(vcgtq_s16, int16x8_t,  uint16x8_t, >)
SHIM_CMP(vcgtq_s32, int32x4_t,  uint32x4_t, >)
SHIM_CMP(vcgtq_u8,  uint8x16_t, uint8x16_t, >)
SHIM_CMP(vcgtq_u16, uint16x8_t, uint16x8_t, >)
SHIM_CMP(vcgtq_u32, uint32x4_t, uint32x4_t, >)
SHIM_CMP(vcgeq_u32, uint32x4_t, uint32x4_t, >=)
SHIM_CMP(vcltq_u32, uint32x4_t, uint32x4_t, <)
SHIM_CMP(vceqq_f32, float32x4_t, uint32x4_t, ==)
SHIM_CMP(vcgtq_f32, float32x4_t, uint32x4_t, >)

/* ---- shifts by immediate ---- */
static inline uint32x4_t vshlq_n_u32(uint32x4_t a, int n) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] << n; return r; }
static inline int16x8_t  vshlq_n_s16(int16x8_t a, int n)  { int16x8_t r;  for (int i = 0; i < 8; i++) r[i] = (int16_t)((uint32_t)(int32_t)a[i] << n); return r; }
static inline int32x4_t  vshlq_n_s32(int32x4_t a, int n)  { int32x4_t r;  for (int i = 0; i < 4; i++) r[i] = (int32_t)((uint32_t)a[i] << n); return r; }
static inline uint8x16_t vshrq_n_u8(uint8x16_t a, int n)  { uint8x16_t r; for (int i = 0; i < 16; i++) r[i] = (uint8_t)(a[i] >> n); return r; }
static inline uint32x4_t vshrq_n_u32(uint32x4_t a, int n) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] >> n; return r; }
static inline int16x8_t  vshrq_n_s16(int16x8_t a, int n)  { int16x8_t r;  for (int i = 0; i < 8; i++) r[i] = (int16_t)(a[i] >> n); return r; }
static inline int32x4_t  vshrq_n_s32(int32x4_t a, int n)  { int32x4_t r;  for (int i = 0; i < 4; i++) r[i] = a[i] >> n; return r; }
/* narrowing shift right: (lane >> n) truncated to the half width */
static inline int16x4_t  vshrn_n_s32(int32x4_t a, int n)  { int16x4_t r;  for (int i = 0; i < 4; i++) r[i] = (int16_t)(a[i] >> n); return r; }
static inline uint16x4_t vshrn_n_u32(uint32x4_t a, int n) { uint16x4_t r; for (int i = 0; i < 4; i++) r[i] = (uint16_t)(a[i] >> n); return r; }

/* ---- shifts by register: USHL / SSHL. The count is the SIGNED low byte of the count lane; positive = left,
 *      negative = right; a count whose magnitude is >= the lane width gives 0 (sign fill for SSHL right). ---- */
static inline uint16x8_t vshlq_u16(uint16x8_t a, int16x8_t s) {
    uint16x8_t r; for (int i = 0; i < 8; i++) { int c = (int8_t)s[i];
        r[i] = c >= 0 ? (c >= 16 ? 0 : (uint16_t)((uint32_t)a[i] << c)) : (-c >= 16 ? 0 : (uint16_t)(a[i] >> -c)); } return r; }
static inline uint32x4_t vshlq_u32(uint32x4_t a, int32x4_t s) {
    uint32x4_t r; for (int i = 0; i < 4; i++) { int c = (int8_t)s[i];
#ifdef SHIM_INJECT_BUG_SHL   /* negative control for the harness (run_host_selftest.sh never defines it) */
        c = c >= 0 ? (c & 31) : -((-c) & 31);
#endif
        r[i] = c >= 0 ? (c >= 32 ? 0 : a[i] << c) : (-c >= 32 ? 0 : a[i] >> -c); } return r; }
static inline int16x8_t vshlq_s16(int16x8_t a, int16x8_t s) {
    int16x8_t r; for (int i = 0; i < 8; i++) { int c = (int8_t)s[i];
        r[i] = c >= 0 ? (c >= 16 ? 0 : (int16_t)((uint32_t)(int32_t)a[i] << c)) : (-c >= 16 ? (a[i] < 0 ? -1 : 0) : (int16_t)(a[i] >> -c)); } return r; }
static inline int32x4_t vshlq_s32(int32x4_t a, int32x4_t s) {
    int32x4_t r; for (int i = 0; i < 4; i++) { int c = (int8_t)s[i];
        r[i] = c >= 0 ? (c >= 32 ? 0 : (int32_t)((uint32_t)a[i] << c)) : (-c >= 32 ? (a[i] < 0 ? -1 : 0) : a[i] >> -c); } return r; }

/* ---- widening / narrowing multiply ---- */
static inline int16x4_t  vmovn_s32(int32x4_t a)  { int16x4_t r;  for (int i = 0; i < 4; i++) r[i] = (int16_t)a[i];  return r; }
static inline uint16x4_t vmovn_u32(uint32x4_t a) { uint16x4_t r; for (int i = 0; i < 4; i++) r[i] = (uint16_t)a[i]; return r; }
static inline int32x4_t  vmull_s16(int16x4_t a, int16x4_t b)   { int32x4_t r;  for (int i = 0; i < 4; i++) r[i] = (int32_t)a[i] * (int32_t)b[i]; return r; }
static inline uint32x4_t vmull_u16(uint16x4_t a, uint16x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = (uint32_t)a[i] * (uint32_t)b[i]; return r; }
static inline int32x4_t  vmlal_s16(int32x4_t acc, int16x4_t a, int16x4_t b)   { int32x4_t r;  for (int i = 0; i < 4; i++) r[i] = (int32_t)((uint32_t)acc[i] + (uint32_t)((int32_t)a[i] * (int32_t)b[i])); return r; }
static inline uint32x4_t vmlal_u16(uint32x4_t acc, uint16x4_t a, uint16x4_t b) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = acc[i] + (uint32_t)a[i] * (uint32_t)b[i]; return r; }

/* ---- counts, pairwise ---- */
static inline uint32x4_t vclzq_u32(uint32x4_t a) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] ? (uint32_t)__builtin_clz(a[i]) : 32u; return r; }
static inline uint8x16_t vcntq_u8(uint8x16_t a)  { uint8x16_t r; for (int i = 0; i < 16; i++) r[i] = (uint8_t)__builtin_popcount(a[i]); return r; }
static inline uint16x8_t vpaddlq_u8(uint8x16_t a)  { uint16x8_t r; for (int i = 0; i < 8; i++) r[i] = (uint16_t)(a[2*i] + a[2*i+1]); return r; }
static inline uint32x4_t vpaddlq_u16(uint16x8_t a) { uint32x4_t r; for (int i = 0; i < 4; i++) r[i] = (uint32_t)a[2*i] + (uint32_t)a[2*i+1]; return r; }

/* ---- permutes ---- */
static inline uint8x16_t vrev32q_u8(uint8x16_t a) { uint8x16_t r; for (int i = 0; i < 16; i++) r[i] = a[(i & ~3) | (3 - (i & 3))]; return r; }
static inline uint32x4_t vtrn2q_u32(uint32x4_t a, uint32x4_t b) { uint32x4_t r = { a[1], b[1], a[3], b[3] }; return r; }
static inline uint32x4_t vextq_u32(uint32x4_t a, uint32x4_t b, int n) {
    uint32_t cat[8]; memcpy(cat, &a, 16); memcpy(cat + 4, &b, 16);
    uint32x4_t r; memcpy(&r, cat + (n & 3), 16); return r;
}
/* TBL: index outside the table gives 0 */
static inline uint8x16_t vqtbl1q_u8(uint8x16_t t, uint8x16_t idx) { uint8x16_t r; for (int i = 0; i < 16; i++) r[i] = idx[i] < 16 ? t[idx[i]] : 0; return r; }
#ifdef SHIM_INJECT_BUG_TBL   /* negative control: out-of-table indices wrap instead of returning 0 */
static inline uint8x16_t vqtbl2q_u8(uint8x16x2_t t, uint8x16_t idx) { uint8x16_t r; for (int i = 0; i < 16; i++) { unsigned k = idx[i] & 31u; r[i] = k < 16 ? t.val[0][k] : t.val[1][k - 16]; } return r; }
#else
static inline uint8x16_t vqtbl2q_u8(uint8x16x2_t t, uint8x16_t idx) { uint8x16_t r; for (int i = 0; i < 16; i++) r[i] = idx[i] < 16 ? t.val[0][idx[i]] : idx[i] < 32 ? t.val[1][idx[i] - 16] : 0; return r; }
#endif

/* ---- single precision (follows the host FP environment; fused multiply-add is exact fmaf) ---- */
static inline float32x4_t vaddq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; return r; }
static inline float32x4_t vsubq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] - b[i]; return r; }
static inline float32x4_t vmulq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; return r; }
static inline float32x4_t vnegq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = -a[i]; return r; }
static inline float32x4_t vabsq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = fabsf(a[i]); return r; }
/* vfmaq_f32(a, b, c) = a + b*c, vfmsq_f32(a, b, c) = a - b*c, one rounding each */
static inline float32x4_t vfmaq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = fmaf(b[i], c[i], a[i]); return r; }
static inline float32x4_t vfmsq_f32(float32x4_t a, float32x4_t b, float32x4_t c) { float32x4_t r; for (int i = 0; i < 4; i++) r[i] = fmaf(-b[i], c[i], a[i]); return r; }

#endif /* SPU_NEON_SHIM_ARM_NEON_H */
