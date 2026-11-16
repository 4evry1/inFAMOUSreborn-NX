#ifndef PS3_NATIVE_SPU_LIFTED_BOUNDARY_H
#define PS3_NATIVE_SPU_LIFTED_BOUNDARY_H
#include "spu_executor.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native boundary for the generated lifted-code runtime ABI (spu_drain_call, spu_img_restore, spu_indirect_branch,
 * spu_stop, and the owned continuation slot behind g_spu_trampoline_fn / SPU_XFER / SPU_RET / SPU_RET_REG).
 * Owned continuations run one validated hop at a time (this image's function at pc, not LS 0, hop budget, PRE_HOP
 * checkpoint) inside a call's drain until the call's matched return, or at top level after the entry returns.
 * The policy adapter opts into exact bound-image indirect lookup and pinned
 * kernel calls. Kernel SELECT invokes its injected callback synchronously so
 * generated C call frames survive; EXIT abandons those frames explicitly.
 * Slices yield only at top-level pending continuations, never inside a drain.
 * Anything outside that admitted subset records its op id in the caller-owned port and stops the owner with no effect, leaving any pending
 * continuation in the owner (ps3_spu_executor_take_continuation). */
/* Top-level runner called by ps3_spu_executor_enter after the entry function returns; never returns. */
_Noreturn void ps3_spu_lifted_boundary_run_continuations(spu_context *);
/* Decision 134: called (when set) every time a lifted call's drain ends at its matched return, with that return address. Set once by the hybrid glue. */
extern void (*ps3_lifted_drain_returned_hook)(spu_context *ctx, uint32_t return_pc);
#ifdef __cplusplus
}
#endif
#endif
