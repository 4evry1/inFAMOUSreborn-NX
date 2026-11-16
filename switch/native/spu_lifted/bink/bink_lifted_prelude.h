#ifndef PS3_BINK_LIFTED_PRELUDE_H
#define PS3_BINK_LIFTED_PRELUDE_H
/* Everything the adapted lifted translation unit (generated/bink_lifted_rc.c) needs from this repository's runtime.
 * It replaces the lifter's own preamble: the include of spu_recomp.h, the TLS g_spu_fast_tail and the clang-only
 * SPU_FAST_XFER. Include path: source/ps3recomp/runtime/spu (+ memory, time, include), native/adapters, native/spu_lifted.
 * Build with -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL, as every native TU. */
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_helpers_vec.h"
#include "spu_execution_port.h"
#include "spu_dma.h"
#include "../../adapters/spu_executor.h"
#include "../spu_lifted_hybrid.h"
#include <stdio.h>

/* Runtime-ABI functions the lifted text calls that the native boundary (spu_lifted_boundary.c) provides unchanged:
 * spu_indirect_branch, spu_drain_call, spu_img_restore (declared by spu_context.h). */

/* Channel access goes through the glue, which waits in place for mailbox conditions the interpreter would PAUSE on. */
#define spu_rdch ps3_lifted_rdch
#define spu_wrch ps3_lifted_wrch
#define spu_rchcnt ps3_lifted_rchcnt

/* Test-only: the native interpreter splats a channel count over all four lanes (spu_interp_native.c SPU_rchcnt); the
 * lifter's own `spu_pref_u32(spu_rchcnt(..))` zeroes lanes 1..3, which is what the hardware/RPCS3 does (spu_helpers.h
 * spu_pref_u32). A bit-exact register differential needs both sides to agree, so the test build can emulate the splat. */
#ifdef PS3_LIFTED_TEST_RCHCNT_SPLAT
#  undef spu_pref_u32
#  define spu_pref_u32(v) spu_splat_u32(v)
#endif

/* Direct block-to-block transfer (the lifter's spu_fasttail.py form). The Windows build takes it only when a TLS flag
 * is set; natively it is always on, except while an enabled interrupt has a selected event pending, where the
 * transfer is published as an owned continuation (the boundary then refuses it: interrupt delivery is not modelled for
 * lifted code). GCC >= 15 / clang: musttail makes the transfer a jump, so a loop never grows the C stack and a
 * compiler that cannot honour it fails the build. Older compilers fall back to a plain tail call that -O2 turns into a
 * jump (sibling-call optimisation); that must be checked on the object code (verify_bink_lifted.sh does so on x86-64). */
#if defined(__has_attribute)
#  if __has_attribute(musttail)
#    define PS3_LIFTED_MUSTTAIL __attribute__((musttail))
#  endif
#endif
#ifndef PS3_LIFTED_MUSTTAIL
#  define PS3_LIFTED_MUSTTAIL
#endif
#define SPU_FAST_XFER(c, F) do {                                                                       \
        if (__builtin_expect(!((c)->int_enable && ((c)->event_status & (c)->event_mask)), 1)) {        \
            PS3_LIFTED_MUSTTAIL return F(c);                                                           \
        }                                                                                               \
        g_spu_trampoline_fn = F; return;                                                                \
    } while (0)

#endif
