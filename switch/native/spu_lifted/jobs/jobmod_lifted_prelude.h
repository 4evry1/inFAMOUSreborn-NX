#ifndef PS3_JOBMOD_LIFTED_PRELUDE_H
#define PS3_JOBMOD_LIFTED_PRELUDE_H
/* Everything a generated job-module translation unit (generated/<name>_rc.c) needs from this repository's runtime
 * (Decision 104, 2026-11-13). The Windows port's lifted job modules (source/infamous-recomp/spu/jobmod_*) were generated for its own
 * runtime; gen_jobmods_lifted.py adapts the TEXT mechanically (it never edits instructions), and this header gives the macros
 * the text still uses their native meaning:
 *   - channel access, the stop/halt/escape entry points: the hybrid glue's (as native/spu_lifted/bink/bink_lifted_prelude.h);
 *   - SPU_XFER / SPU_FAST_XFER: a direct tail call into the next block when nothing is pending, else (an enabled interrupt with a
 *     selected event pending) the exact state goes to the interpreter, which delivers the interrupt: the lifted code never has
 *     to model interrupt delivery (ps3_lifted_irq_escape);
 *   - the Windows port's "ibf" indirect-branch fast path (spu_ibf.h): same lookup, the module's own entries only, same interrupt rule;
 *   - the float "xfk" variants: the interpreter's own operations (the range analysis that licensed skipping a clamp is not trusted:
 *     the lifted code must be bit-identical to the interpreter by construction);
 *   - the Windows runtime's diagnostics globals as constants.
 * Include path: source/ps3recomp/runtime/spu (+ memory, time, include), native/adapters, native/spu_lifted. Build with
 * -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL like every native TU. */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_helpers_vec.h"
#include "spu_execution_port.h"
#include "spu_dma.h"
#include "../../adapters/spu_executor.h"
#include "../spu_lifted_hybrid.h"
#include <stdio.h>

#define spu_rdch ps3_lifted_rdch
#define spu_wrch ps3_lifted_wrch
#define spu_rchcnt ps3_lifted_rchcnt
#ifdef PS3_LIFTED_TEST_RCHCNT_SPLAT
#  undef spu_pref_u32
#  define spu_pref_u32(v) spu_splat_u32(v)
#endif

#if defined(__has_attribute)
#  if __has_attribute(musttail)
#    define PS3_LIFTED_MUSTTAIL __attribute__((musttail))
#  endif
#endif
#ifndef PS3_LIFTED_MUSTTAIL
#  define PS3_LIFTED_MUSTTAIL
#endif

/* Test builds only (-DPS3_JOBMOD_TRACE, test_jobmods_lifted.c --trace): every lifted function reports its entry pc and the exact
 * architectural state (a function loads its registers from the context at entry). */
#ifdef PS3_JOBMOD_TRACE
void ps3_jobmod_trace_entry(spu_context *ctx, uint32_t pc);
#  define PS3_JOBMOD_TRACE_XFER(c) ((void)0)
#  define PS3_JOBMOD_TRACE_ENTRY(c, pc) ps3_jobmod_trace_entry((c), (pc))
#else
#  define PS3_JOBMOD_TRACE_XFER(c) ((void)0)
#  define PS3_JOBMOD_TRACE_ENTRY(c, pc) ((void)0)
#endif

/* Direct transfer to the next block. Every use in the generated text stores the constant target pc first (the generator checks), so
 * the interrupt escape needs nothing but the call. */
#define SPU_FAST_XFER(c, F) do {                                                                       \
        if (__builtin_expect(!((c)->int_enable && ((c)->event_status & (c)->event_mask)), 1)) {        \
            PS3_JOBMOD_TRACE_XFER(c);                                                                   \
            PS3_LIFTED_MUSTTAIL return F(c);                                                           \
        }                                                                                               \
        ps3_lifted_irq_escape(c);                                                                       \
    } while (0)
#undef SPU_XFER
#define SPU_XFER(c, F) SPU_FAST_XFER(c, F)

/* spu_ibf.h (v177) replaced by its native meaning. A target that is one of this module's entries is entered directly; anything else
 * (and any pending interrupt) takes the line that follows the macro in the text: publish spu_indirect_branch, which the boundary
 * resolves against the bound image (an unknown pc is an escape row). */
typedef void (*spu_ibf_fn)(spu_context *);
#define g_spu_tlc_slow 0
#define g_spu_stack_reset_n 0
#define SPU_IBF_COND(ctx) (!((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask)))
#ifndef PS3_JOBMOD_WINDOW_LO
#define PS3_JOBMOD_WINDOW_LO 0x4000u   /* a task image that starts below the job window (jobmod_422c6e0242f5_e3090, LS 0x3000) defines its own start before including this */
#endif
#define SPU_IBF_RESOLVE(ctx, t, cache, find) (((t) >= PS3_JOBMOD_WINDOW_LO && (t) < 0x3E000u) ? find((t), &(cache)) : (spu_ibf_fn)0)
#define SPU_IBF_SITE(ctx, n, selfpc, selffn, cache, find)                                                   \
    { const uint32_t _ibt = (ctx)->gpr[n]._u32[0] & SPU_LS_MASK;                                            \
      if (SPU_IBF_COND(ctx)) {                                                                              \
          if (_ibt == (selfpc)) { (ctx)->pc = _ibt; PS3_JOBMOD_TRACE_XFER(ctx); goto spu_ibf_top; }          \
          { spu_ibf_fn _ibf = SPU_IBF_RESOLVE(ctx, _ibt, cache, find);                                      \
            if (_ibf) { (ctx)->pc = _ibt; PS3_JOBMOD_TRACE_XFER(ctx); PS3_LIFTED_MUSTTAIL return (_ibf)(ctx); } }  \
      } }
#define SPU_IBF_RETSELF(ctx, selfpc, selffn)                                                                \
    { if ((((ctx)->gpr[0]._u32[0] & SPU_LS_MASK) == (selfpc)) && SPU_IBF_COND(ctx)) {                        \
          (ctx)->pc = (selfpc); PS3_JOBMOD_TRACE_XFER(ctx); goto spu_ibf_top; } }
#define SPU_IBF_CALL(ctx, cache, find)                                                                     \
    { const uint32_t _ibt = (ctx)->pc & SPU_LS_MASK; spu_ibf_fn _ibf = 0;                                   \
      if (SPU_IBF_COND(ctx)) _ibf = SPU_IBF_RESOLVE(ctx, _ibt, cache, find);                                \
      if (_ibf) { (ctx)->pc = _ibt; PS3_JOBMOD_TRACE_XFER(ctx); _ibf(ctx); } else spu_indirect_branch(ctx); }

/* Native replacement hooks (round 2, native_inflate.h): a lifted function the generator was told about starts with `if (HOOK(ctx)) { SPU_RET(ctx); }`. The hook
 * either did the function's whole job (and set the result) or changed nothing. Without the switch the macro is the constant 0 and the branch is gone. */
#ifdef PS3_SPU_NATIVE_INFLATE
#  include "native_inflate.h"
#  define PS3_NATIVE_INFLATE_HOOK(c) ps3_native_inflate_edge_raw(c)
#else
#  define PS3_NATIVE_INFLATE_HOOK(c) 0
#endif

/* float "xfk" variants -> the interpreter's operations (the mask argument only licensed skipping a clamp). */
#define spu_fa_k(a, b, m)      spu_fa((a), (b))
#define spu_fs_k(a, b, m)      spu_fs((a), (b))
#define spu_fm_k(a, b, m)      spu_fm((a), (b))
#define spu_fma_k(a, b, c, m)  spu_fma((a), (b), (c))
#define spu_fms_k(a, b, c, m)  spu_fms((a), (b), (c))
#define spu_fnms_k(a, b, c, m) spu_fnms((a), (b), (c))

#endif
