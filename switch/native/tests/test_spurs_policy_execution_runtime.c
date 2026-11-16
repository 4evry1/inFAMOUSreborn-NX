/* Reuses the real coherent-memory/trace fixture and runs its existing regression
 * suite before the new borrowed-owner and continuation lifecycle cases. */
#define main ps3_existing_spu_executor_regressions
#include "test_spu_executor.c"
#undef main
#include "../adapters/spurs_policy_execution.h"

static void policy_slice_third(spu_context *ctx) {
    ++ctx->gpr[9]._u32[0];
    spu_wrch(ctx,SPU_WrDec,spu_make_preferred_u32(0));
}
static void policy_slice_second(spu_context *ctx) {
    ++ctx->gpr[9]._u32[0];
    ctx->pc=0x108; SPU_XFER(ctx,policy_slice_third);
}
static void policy_slice_first(spu_context *ctx) {
    ++ctx->gpr[9]._u32[0];
    ctx->pc=0x104; SPU_XFER(ctx,policy_slice_second);
}
static void borrowed_owner_and_safe_slice(void) {
    Fixture f={0}; ps3_spu_executor *original=create(&f);
    ps3_spu_executor *borrowed=calloc(1,sizeof(*borrowed)); assert(borrowed);
    unsigned char *ls=aligned_alloc(16,SPU_LS_SIZE); assert(ls); memset(ls,0,SPU_LS_SIZE);
    assert(ps3_spu_executor_init_borrowed(borrowed,original->memory.domain,original->memory.owner,ls,SPU_LS_SIZE,3,original->services)==PS3_GUEST_MEMORY_OK);
    assert(borrowed->memory.owner.slot==original->memory.owner.slot);
    static const ps3_spu_lifted_function functions[]={
        {0x100,policy_slice_first},{0x104,policy_slice_second},{0x108,policy_slice_third}
    };
    const ps3_spu_linked_image image={"policy-slices",functions,3};
    ScriptClock clock={{100},1,0,0,PS3_CLOCK_OK};
    ps3_spu_executor_start_config cfg={&image,1,{&clock,script_sample},PS3_SPU_THREAD_OPTION_NONE,100};
    assert(ps3_spu_executor_start(borrowed,&cfg)==PS3_SPU_EXEC_OK);
    borrowed->core.image_id=1; borrowed->core.pc=0x100;
    assert(ps3_spu_executor_enter_slice(borrowed,policy_slice_first,1)==PS3_SPU_EXEC_SLICE);
    assert(!borrowed->stopped && borrowed->continuation==policy_slice_third);
    assert(borrowed->core.gpr[9]._u32[0]==2);
    assert(ps3_spu_executor_continue_slice(borrowed,1)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(borrowed->core.gpr[9]._u32[0]==3); /* no effect replay */
    assert(borrowed->terminal.channel==SPU_WrDec);
    assert(ps3_spu_executor_release(borrowed)==PS3_GUEST_MEMORY_OK);
    /* Borrowed release must leave the original live coherent owner usable. */
    unsigned char byte=0;
    assert(ps3_atomic_owner_read(&f.domain,original->memory.owner,0x1000,&byte,1)==PS3_GUEST_MEMORY_OK);
    free(ls); free(borrowed); finish(&f,original);
}
static unsigned policy_selections;
static int choose_policy(void *context,uint32_t spu,unsigned is_poll,uint64_t *selected) {
    (void)context; assert(spu==0 && is_poll==1); ++policy_selections;
    *selected=UINT64_C(0x0000000700000003); return 0;
}
static void policy_nested_select(spu_context *ctx) {
    ctx->gpr[0]=spu_make_preferred_u32(0x124); ctx->host_depth++;
    ctx->pc=0x290; spu_indirect_branch(ctx); spu_drain_call(ctx,0x124);
    ctx->host_depth--;
    assert(ctx->gpr[3]._u64[0]==UINT64_C(0x0000000700000003));
    ctx->gpr[9]._u32[0]=0xA5; /* caller statements after the live C frame */
    ctx->pc=0x808; spu_indirect_branch(ctx);
}
static void nested_selection_preserves_call_stack(void) {
    Fixture f={0}; ps3_spu_executor *o=create(&f);
    const ps3_spu_lifted_function functions[]={{0x120,policy_nested_select}};
    const ps3_spu_linked_image image={"nested-select",functions,1};
    ScriptClock clock={{100},1,0,0,PS3_CLOCK_OK};
    ps3_spu_executor_start_config cfg={&image,1,{&clock,script_sample},PS3_SPU_THREAD_OPTION_NONE,100};
    assert(ps3_spu_executor_start(o,&cfg)==PS3_SPU_EXEC_OK);
    o->policy=(ps3_spu_policy_callbacks){NULL,choose_policy,0x290,0x808};
    o->core.image_id=1; o->core.pc=0x120; policy_selections=0;
    assert(ps3_spu_executor_enter_slice(o,policy_nested_select,1)==PS3_SPU_EXEC_KERNEL_EXIT);
    assert(policy_selections==1 && o->core.gpr[9]._u32[0]==0xA5);
    assert(!o->core.host_depth && !o->continuation);
    finish(&f,o);
}
static void adapter_rejects_invalid_image_before_lookup(void) {
    Fixture f={0}; ps3_spu_executor *o=create(&f);
    ps3_spurs_policy_execution *adapter=calloc(1,sizeof(*adapter)); assert(adapter);
    const ps3_spu_lifted_function functions[]={{0x2BF0,policy_nested_select}};
    const ps3_spu_linked_image image={"oversized",functions,SPU_LS_SIZE/4u+1u};
    ScriptClock clock={{100},1,0,0,PS3_CLOCK_OK};
    const ps3_spurs_policy_execution_config config={&image,policy_nested_select,{&clock,script_sample},o->services,0x290,0x808};
    assert(ps3_spurs_policy_execution_init(adapter,&config)==PS3_SPURS_POLICY_INVALID);
    assert(!clock.next && !adapter->initialized);
    free(adapter); finish(&f,o);
}
static void policy_halt_entry(spu_context *ctx) { ctx->status=SPU_STATUS_STOPPED_BY_HALT; }
static void architectural_halt_is_terminal_stop(void) {
    Fixture f={0}; ps3_spu_executor *o=create(&f);
    const ps3_spu_lifted_function functions[]={{0x100,policy_halt_entry}};
    const ps3_spu_linked_image image={"architectural-halt",functions,1};
    ScriptClock clock={{100},1,0,0,PS3_CLOCK_OK};
    const ps3_spu_executor_start_config cfg={&image,1,{&clock,script_sample},PS3_SPU_THREAD_OPTION_NONE,100};
    assert(ps3_spu_executor_start(o,&cfg)==PS3_SPU_EXEC_OK);
    o->core.pc=0x100; o->core.image_id=1; o->policy=(ps3_spu_policy_callbacks){NULL,choose_policy,0x290,0x808};
    assert(ps3_spu_executor_enter_slice(o,policy_halt_entry,1)==PS3_SPU_EXEC_STOP);
    assert(o->stopped && o->core.status==SPU_STATUS_STOPPED_BY_HALT && o->terminal.channel==PS3_SPU_EXEC_OP_STOP);
    finish(&f,o);
}
int main(void) {
    int status=ps3_existing_spu_executor_regressions();
    if (status) return status;
    borrowed_owner_and_safe_slice();
    nested_selection_preserves_call_stack();
    adapter_rejects_invalid_image_before_lookup();
    architectural_halt_is_terminal_stop();
    puts("SPURS policy borrowed owner and nonreplaying slices PASS");
    return 0;
}
