#define SPU_DRAIN_IMPL 1
#include "spu_lifted_boundary.h"
/* Generated-code ABI storage referenced only by the native drain: a desktop irq-frame count. Native code never arms an
 * irq frame, so it stays 0; the drain only reads it. There is no thread-local continuation slot natively. */
SPU_THREAD_LOCAL int g_irq_live_n;
void (*ps3_lifted_drain_returned_hook)(spu_context *ctx,uint32_t return_pc);
static ps3_spu_executor *boundary_owner(spu_context *ctx) { return (ps3_spu_executor *)ctx; }  /* closed first-subobject contract */
/* Dynamic job mode (Decision 104): a transfer the lifted image cannot serve but the interpreter simply executes (hardware masks a branch
 * target to a word address, and LS 0 holds the interrupt vector) is handed to the interpreter at the exact architectural state. */
static void hybrid_escape(spu_context *ctx) {
    ps3_spu_executor *o=boundary_owner(ctx);
    if (!o->hybrid_escape) return;
    ctx->pc&=SPU_LS_MASK&~3u;
    o->hybrid_escape(ctx);
}
/* Stops before any effect of the refused operation; the pending continuation (if any) stays in the owner. */
_Noreturn static void stop(spu_context *ctx,uint32_t op,int code) {
    ps3_spu_execution_port *port=ps3_spu_execution_for(ctx);
    port->channel=op; port->effect_committed=0;
    ps3_spu_execution_fail(ctx,code);
}
_Noreturn static void unsupported(spu_context *ctx,uint32_t op) { stop(ctx,op,PS3_SPU_EXEC_UNSUPPORTED); }
/* The owner's single continuation slot (g_spu_trampoline_fn / SPU_XFER / SPU_RET / SPU_RET_REG natively). Generated
 * code runs only inside ps3_spu_executor_enter; a publish outside an entry scope is a composition defect and traps.
 * Publishing while work is already pending stops the owner instead of overwriting it (the pending work is kept). */
void (**ps3_spu_native_continuation_slot(spu_context *ctx))(spu_context *) {
    ps3_spu_executor *o=boundary_owner(ctx);
    if (!o->escape) __builtin_trap();
    if (o->continuation) unsupported(ctx,PS3_SPU_EXEC_OP_CONTINUATION);
    return &o->continuation;
}
/* Depth: like the desktop spu_depth_guard (default 2000), this REFUSES AT THE DRAIN, which runs only after the
 * callee returned. It does not bound host-stack descent through nested brsl before any drain is reached; the reached
 * prefix is bounded only by its actual finite code. */
enum { PS3_SPU_NATIVE_HOST_DEPTH_MAX=2000u };  /* desktop spu_depth_guard default (SPU_HOST_DEPTH_MAX unset): halts at >= */
static int interrupt_state_unsupported(const spu_context *ctx) {
    return ctx->irq_frame || g_irq_live_n ||                       /* desktop irq frames (never armed natively) */
           (ctx->int_enable && (ctx->event_status&ctx->event_mask)); /* native limit: no interrupt delivery */
}
/* A synchronous select preserves the generated brsl/bisl C frames. Returning
 * an event and longjmp here would destroy those frames and replay effects on
 * resume. Exit is different: the guest explicitly abandons this module stack. */
static int policy_kernel_call(spu_context *ctx) {
    ps3_spu_executor *o=boundary_owner(ctx);
    if (!o->policy.select) return 0;
    const uint32_t pc=ctx->pc&SPU_LS_MASK;
    if (pc==o->policy.exit_pc) {
        o->continuation=NULL; ctx->host_depth=0;
        o->binding.channel=PS3_SPU_EXEC_OP_INDIRECT_BRANCH; o->binding.effect_committed=0;
        ps3_spu_executor_pause(ctx,PS3_SPU_EXEC_KERNEL_EXIT);
    }
    if (pc!=o->policy.select_pc) return 0;
    uint64_t selected=0;
    const uint32_t return_pc=ctx->gpr[0]._u32[0]&SPU_LS_MASK;
    o->binding.channel=PS3_SPU_EXEC_OP_INDIRECT_BRANCH; o->binding.effect_committed=0;
    int status=o->binding.checkpoint(ctx,PS3_SPU_PRE_HOP);
    if (!status) status=o->policy.select(o->policy.context,ctx->spu_id,1,&selected);
    if (status) ps3_spu_execution_fail(ctx,status);
    ctx->gpr[3]=(u128){0}; ctx->gpr[3]._u64[0]=selected;
    ctx->pc=return_pc; o->continuation=NULL;
    return 1;
}
/* One owned hop (desktop spu_drain_call loop body / SPU_DRAIN, spu_drain.c:532-638, subset). Every refusal happens
 * BEFORE any effect and leaves fn pending in the owner:
 *   the desktop indirect marker (dispatcher / matched-indirect consume: not native) -> OP_INDIRECT_BRANCH;
 *   LS pc 0 (desktop spu_ls0_job_end: no fake job completion) or fn not THIS owner's bound image function at the
 *   pending pc (foreign, unlinked, stale) -> OP_CONTINUATION;
 *   irq frames / a pending enabled interrupt -> OP_DRAIN_CALL;
 *   hop budget exhausted (hops == hop_budget) -> BUDGET with OP_CONTINUATION.
 * Then PRE_HOP (a cancel there consumes no budget and keeps fn pending). Only then: take (clear the slot), ++hops,
 * drain_ret_pc = return pc (drain hops only, as the desktop sets it per hop), run fn. ctx->steps is desktop
 * bookkeeping (job-end/launch checks) and is not modelled. Budget/cancel bound the NUMBER OF HOPS only, never loops
 * or recursion inside one lifted function. */
static void hop(spu_context *ctx,ps3_spu_lifted_entry fn,int in_drain,uint32_t return_pc) {
    ps3_spu_executor *o=boundary_owner(ctx);
    if (o->slice_mode && !in_drain && o->hops>=o->slice_end) {
        o->binding.channel=PS3_SPU_EXEC_OP_CONTINUATION; o->binding.effect_committed=0;
        ps3_spu_executor_pause(ctx,PS3_SPU_EXEC_SLICE);
    }
    int resolved=0;   /* Decision 125: fn was just looked up at this exact pc, so the validation lookup below would repeat it */
    if (fn==spu_indirect_branch) {
        if (!o->policy.select) unsupported(ctx,PS3_SPU_EXEC_OP_INDIRECT_BRANCH);
        if (policy_kernel_call(ctx)) {
            if (in_drain && !((ctx->pc^return_pc)&SPU_LS_MASK)) return;
        }
        fn=ps3_spu_linked_image_lookup(o->start.image,ctx->pc&SPU_LS_MASK);
        if (!fn) { hybrid_escape(ctx); unsupported(ctx,PS3_SPU_EXEC_OP_INDIRECT_BRANCH); }
        o->continuation=fn;
        resolved=1;
    }
    const uint32_t pc=ctx->pc&SPU_LS_MASK;
    if (!pc || (!resolved && ps3_spu_linked_image_lookup(o->start.image,pc)!=fn)) { if (!pc) hybrid_escape(ctx); unsupported(ctx,PS3_SPU_EXEC_OP_CONTINUATION); }
    if (interrupt_state_unsupported(ctx)) {
        if (o->irq_escape) o->irq_escape(ctx);   /* dynamic job mode: the interpreter delivers it from this exact state */
        unsupported(ctx,PS3_SPU_EXEC_OP_DRAIN_CALL);
    }
    if (o->hops>=o->start.hop_budget) stop(ctx,PS3_SPU_EXEC_OP_CONTINUATION,PS3_SPU_EXEC_BUDGET);
    ps3_spu_execution_port *port=ps3_spu_execution_for(ctx);
    port->channel=PS3_SPU_EXEC_OP_CONTINUATION; port->effect_committed=0;
    const int status=port->checkpoint(ctx,PS3_SPU_PRE_HOP);
    if (status) ps3_spu_execution_fail(ctx,status);
    o->continuation=NULL; ++o->hops;
    if (in_drain) ctx->drain_ret_pc=return_pc&SPU_LS_MASK;
    fn(ctx);
}
/* Bracket drain after a lifted call (desktop spu_drain_call, subset): run this owner's validated continuations one
 * hop at a time until none is pending, then require the matched return (pc == return pc: drain_ret_pc = 0, the
 * accepted matched POP). The desktop resume/lookup/restart on a mismatch is not native: OP_DRAIN_CALL. */
void spu_drain_call(spu_context *ctx,uint32_t return_pc) {
    ps3_spu_executor *o=boundary_owner(ctx);
    ++o->boundary_drains;
    for (;;) {
        if (o->policy.select && (ctx->status&(SPU_STATUS_STOPPED_BY_STOP|SPU_STATUS_STOPPED_BY_HALT)))
            stop(ctx,PS3_SPU_EXEC_OP_STOP,PS3_SPU_EXEC_STOP);
        if (o->irq_escape && ctx->host_depth && interrupt_state_unsupported(ctx))
            o->irq_escape(ctx);                                  /* dynamic job mode (the callee has returned: the state is exact) */
        if (o->hybrid_escape && ctx->host_depth>=PS3_SPU_NATIVE_HOST_DEPTH_MAX) hybrid_escape(ctx);   /* dynamic job mode: a very deep recursion goes on in the interpreter */
        if (!ctx->host_depth ||                                  /* bracket increments before the callee: 0 = lost frame */
            ctx->host_depth>=PS3_SPU_NATIVE_HOST_DEPTH_MAX ||      /* desktop depth guard refuses here (after the callee) */
            interrupt_state_unsupported(ctx))
            unsupported(ctx,PS3_SPU_EXEC_OP_DRAIN_CALL);
        const ps3_spu_lifted_entry fn=o->continuation;
        if (!fn) {
            if (((ctx->pc^return_pc)&SPU_LS_MASK) && policy_kernel_call(ctx)) continue;
            if ((ctx->pc^return_pc)&SPU_LS_MASK) { hybrid_escape(ctx); unsupported(ctx,PS3_SPU_EXEC_OP_DRAIN_CALL); }  /* callee did not come back */
            ctx->drain_ret_pc=0;
            if (ps3_lifted_drain_returned_hook) ps3_lifted_drain_returned_hook(ctx,return_pc);   /* Decision 134: a lifted call came back (job run time) */
            return;
        }
        hop(ctx,fn,1,return_pc);
    }
}
/* Restores compare against the image id BOUND at owner start, never the mutable core value. */
void spu_img_restore(spu_context *ctx,int32_t saved_image) {
    ps3_spu_executor *o=boundary_owner(ctx);
    ++o->boundary_restores;
    const int target=ctx->module_img_a00 ? ctx->module_img_a00 : saved_image;
    if (target!=o->start.image_id) unsupported(ctx,PS3_SPU_EXEC_OP_IMG_RESTORE);
    ctx->image_id=target;
}
void spu_indirect_branch(spu_context *ctx) {
    ps3_spu_executor *o=boundary_owner(ctx);
    if (!o->policy.select) unsupported(ctx,PS3_SPU_EXEC_OP_INDIRECT_BRANCH);
    if (policy_kernel_call(ctx)) return;
    ps3_spu_lifted_entry fn=ps3_spu_linked_image_lookup(o->start.image,ctx->pc&SPU_LS_MASK);
    if (!fn) { hybrid_escape(ctx); unsupported(ctx,PS3_SPU_EXEC_OP_INDIRECT_BRANCH); }
    o->continuation=fn;
}
void spu_stop(spu_context *ctx) {
    if (boundary_owner(ctx)->policy.select) stop(ctx,PS3_SPU_EXEC_OP_STOP,PS3_SPU_EXEC_STOP);
    unsupported(ctx,PS3_SPU_EXEC_OP_STOP);
}
/* Top level (desktop SPU_DRAIN after the entry, subset): validated hops while work is pending at host_depth 0; a
 * normal return with nothing pending is terminal OP_ENTRY_RETURNED (never success, never a fake job completion). */
_Noreturn void ps3_spu_lifted_boundary_run_continuations(spu_context *ctx) {
    ps3_spu_executor *o=boundary_owner(ctx);
    for (;;) {
        if (o->policy.select && (ctx->status&(SPU_STATUS_STOPPED_BY_STOP|SPU_STATUS_STOPPED_BY_HALT)))
            stop(ctx,PS3_SPU_EXEC_OP_STOP,PS3_SPU_EXEC_STOP);
        const ps3_spu_lifted_entry fn=o->continuation;
        if (!fn) unsupported(ctx,PS3_SPU_EXEC_OP_ENTRY_RETURNED);
        if (ctx->host_depth) unsupported(ctx,PS3_SPU_EXEC_OP_CONTINUATION);  /* top-level work needs no live frame */
        hop(ctx,fn,0,0);
    }
}
/* Desktop-only registries and diagnostics referenced by the generated image TUs.
 * The native composition removes the registry constructor and never calls the
 * register function; reaching any of these is a composition defect, so they trap
 * instead of pretending to register or watch. Linking: the registry functions are referenced only by the removed
 * registry constructor, so the gc link drops them (Makefile.spu-entry and verify_spu_entry_checkpoint.sh assert they are
 * absent). spu_ls_watch_slow IS linked: the bound linked-image table keeps all 187 generated functions alive and
 * their inline LS-watch gate (spu_context.h spu_ls_watch_hit2) references it; it is reached only if
 * g_spu_ls_watch_n != 0, which native code never sets, and then traps. No float-policy storage exists natively:
 * the xfloat clamp is compile-time policy (spu_helpers.h). */
int g_spu_ls_watch_n;  /* 0: no LS watch configured (desktop env gate absent natively) */
void spu_ls_watch_slow(uint32_t lsa,int is_write,const uint8_t *p,uint32_t pc,uint32_t lr) { (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; __builtin_trap(); }
void spu_register_function(uint32_t addr,void (*fn)(spu_context *)) { (void)addr; (void)fn; __builtin_trap(); }
void spu_rc_add_pairs(const void *const *pairs,unsigned n) { (void)pairs; (void)n; __builtin_trap(); }
