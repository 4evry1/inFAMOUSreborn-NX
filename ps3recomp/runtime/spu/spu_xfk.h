/* v200 SPU_XFK: float ops whose operands are known to come straight from another float op of the same basic block
 * (tools: spu_xfk.py) skip the xfloat operand clamp for those operands. A float-op result is finite unless it
 * overflowed, so the clamp is a no-op for it in every case except an overflow to +-Inf (never seen in the job code;
 * on the SPU the result would saturate). m = bit i set -> clamp operand i (as before). -DSPU_XFK_EXACT clamps all. */
#ifndef SPU_XFK_H
#define SPU_XFK_H
#include "spu_helpers.h"

#if defined(PS3_SPU_SCALAR_XFK) || (!defined(__SSE2__) && !defined(_M_X64))
static inline u128 spu_xfk_operand(u128 value, unsigned mask, unsigned index)
{
#if defined(SPU_XFK_EXACT)
    (void)mask; (void)index;
    return spu_xfv(value);
#else
    return (mask & (1u << index)) ? spu_xfv(value) : value;
#endif
}
static inline u128 spu_fa_k(u128 a, u128 b, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    for (int i = 0; i < 4; ++i) result._f32[i] = a._f32[i] + b._f32[i];
    return result;
}
static inline u128 spu_fs_k(u128 a, u128 b, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    for (int i = 0; i < 4; ++i) result._f32[i] = a._f32[i] - b._f32[i];
    return result;
}
static inline u128 spu_fm_k(u128 a, u128 b, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    for (int i = 0; i < 4; ++i) result._f32[i] = a._f32[i] * b._f32[i];
    return result;
}
static inline u128 spu_fma_k(u128 a, u128 b, u128 c, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    c = spu_xfk_operand(c, mask, 2);
    for (int i = 0; i < 4; ++i) result._f32[i] = fmaf(a._f32[i], b._f32[i], c._f32[i]);
    return result;
}
static inline u128 spu_fms_k(u128 a, u128 b, u128 c, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    c = spu_xfk_operand(c, mask, 2);
    for (int i = 0; i < 4; ++i) result._f32[i] = fmaf(a._f32[i], b._f32[i], -c._f32[i]);
    return result;
}
static inline u128 spu_fnms_k(u128 a, u128 b, u128 c, unsigned mask)
{
    u128 result;
    a = spu_xfk_operand(a, mask, 0); b = spu_xfk_operand(b, mask, 1);
    c = spu_xfk_operand(c, mask, 2);
    for (int i = 0; i < 4; ++i) result._f32[i] = fmaf(-a._f32[i], b._f32[i], c._f32[i]);
    return result;
}
#else
#if defined(SPU_XFK_EXACT)
#  define SPU__XFKC(m, i, v) spu__xfq(v)
#else
#  define SPU__XFKC(m, i, v) (((m) >> (i)) & 1 ? spu__xfq(v) : (v))
#endif
#define spu_fa_k(a, b, m)      spu__fst(_mm_add_ps(SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b))))
#define spu_fs_k(a, b, m)      spu__fst(_mm_sub_ps(SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b))))
#define spu_fm_k(a, b, m)      spu__fst(_mm_mul_ps(SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b))))
#define spu_fma_k(a, b, c, m)  spu__fst(_mm_fmadd_ps (SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b)), SPU__XFKC(m, 2, spu__fld(c))))
#define spu_fms_k(a, b, c, m)  spu__fst(_mm_fmsub_ps (SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b)), SPU__XFKC(m, 2, spu__fld(c))))
#define spu_fnms_k(a, b, c, m) spu__fst(_mm_fnmadd_ps(SPU__XFKC(m, 0, spu__fld(a)), SPU__XFKC(m, 1, spu__fld(b)), SPU__XFKC(m, 2, spu__fld(c))))
#endif
#endif
