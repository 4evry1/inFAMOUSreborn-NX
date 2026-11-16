/* Actual generated entry and captured raw module. The first transfer is
 * captured and refused before provider access; no guest queue is supplied. */
#define main ps3_existing_spu_executor_regressions
#include "test_spu_executor.c"
#undef main
#include "../adapters/spurs_policy_execution.h"
#include "spu_recomp.h"
extern const ps3_spu_linked_image ps3_native_wws_linked_image;
static unsigned actual_select_requests;
typedef struct { Fixture *fixture; unsigned calls; uint64_t ea; size_t bytes; int write; } RefusedTransfer;
static int stop_before_fixture_selection(void *context,uint32_t spu,unsigned is_poll,uint64_t *packed) {
    (void)context; (void)packed; assert(spu==0 && is_poll==1); ++actual_select_requests;
    return PS3_SPURS_POLICY_UNSUPPORTED;
}
static int refuse_first_transfer(void *context,uint64_t ea,size_t bytes,int write) {
    RefusedTransfer *transfer=context; assert(!transfer->calls);
    ++transfer->calls; transfer->ea=ea; transfer->bytes=bytes; transfer->write=write;
    return PS3_SPU_EXEC_ACCESS;
}
static int actual_checkpoint(void *context,unsigned phase) {
    return checkpoint(((RefusedTransfer *)context)->fixture,phase);
}
static int actual_reserve(void *context,const ps3_spu_transfer *transfer) {
    return reserve_trace(((RefusedTransfer *)context)->fixture,transfer);
}
static void actual_finish(void *context,const ps3_spu_transfer *transfer,ps3_guest_memory_status status,int committed,const void *bytes) {
    finish_trace(((RefusedTransfer *)context)->fixture,transfer,status,committed,bytes);
}
static ps3_clock_status fixture_clock(void *context,uint64_t *value) {
    *value=++*(uint64_t *)context; return PS3_CLOCK_OK;
}
static void store_be32(unsigned char *ls,size_t at,uint32_t value) {
    ls[at]=(unsigned char)(value>>24); ls[at+1]=(unsigned char)(value>>16);
    ls[at+2]=(unsigned char)(value>>8); ls[at+3]=(unsigned char)value;
}
static void controlled_exit_entry(spu_context *ctx) {
    ++ctx->gpr[9]._u32[0]; ctx->ls[0x1500]=0x99;
    spu_wrch(ctx,MFC_LSA,spu_make_preferred_u32(0x180));
    ctx->pc=0x808; spu_indirect_branch(ctx);
}
static void retained_dispatch_and_cancel(ps3_spurs_policy_execution_request request,ps3_spu_executor_services services) {
    ps3_spurs_policy_execution *adapter=calloc(1,sizeof(*adapter)); assert(adapter);
    uint64_t clock=0;
    const ps3_spu_lifted_function functions[]={{0x2BF0,controlled_exit_entry}};
    const ps3_spu_linked_image image={"controlled-retention",functions,1};
    const ps3_spurs_policy_execution_config config={&image,controlled_exit_entry,{&clock,fixture_clock},services,0x290,0x808};
    assert(ps3_spurs_policy_execution_init(adapter,&config)==PS3_SPURS_POLICY_READY);
    ps3_spurs_policy_execution_port port=ps3_spurs_policy_execution_bind(adapter);
    request.module_reloaded=0;
    assert(port.begin(port.context,&request).status==PS3_SPURS_POLICY_UNSUPPORTED);
    assert(!clock && !adapter->lanes[0].active);
    request.module_reloaded=1;
    assert(port.begin(port.context,&request).status==PS3_SPURS_POLICY_READY);
    ps3_spurs_policy_execution_request aliased=request; aliased.spu_index=1;
    assert(port.begin(port.context,&aliased).status==PS3_SPURS_POLICY_BUSY);
    assert(port.step(port.context,0,1).status==PS3_SPURS_POLICY_KERNEL_EXIT);
    assert(adapter->lanes[0].executor.core.gpr[9]._u32[0]==1);
    assert(port.release(port.context,0)==PS3_SPURS_POLICY_READY);
    request.module_reloaded=0;
    assert(port.begin(port.context,&request).status==PS3_SPURS_POLICY_READY);
    assert(clock==1 && adapter->lanes[0].executor.core.gpr[9]._u32[0]==1);
    assert(adapter->lanes[0].executor.core.mfc_lsa==0x180 && request.local_store[0x1500]==0x99);
    assert(port.step(port.context,0,1).status==PS3_SPURS_POLICY_KERNEL_EXIT);
    assert(adapter->lanes[0].executor.core.gpr[9]._u32[0]==2);
    assert(port.release(port.context,0)==PS3_SPURS_POLICY_READY);
    assert(port.begin(port.context,&request).status==PS3_SPURS_POLICY_READY);
    assert(port.cancel(port.context,0)==PS3_SPURS_POLICY_CANCELLED);
    assert(port.step(port.context,0,1).status==PS3_SPURS_POLICY_CANCELLED);
    assert(adapter->lanes[0].executor.core.gpr[9]._u32[0]==2);
    assert(port.release(port.context,0)==PS3_SPURS_POLICY_READY);
    assert(!adapter->lanes[0].executor.initialized);
    free(adapter);
    puts("POLICY_LIFECYCLE controlled_entry=1 retained_LS_register_channel_decrementer=1 cancel_no_replay=1 alias_refused=1");
}
static void entry_abi_words(const spu_context *ctx,uint32_t poll_status) {
    const unsigned registers[]={0,1,3,5};
    const uint32_t preferred[]={0x808,0x3FFB0,0x100,poll_status};
    for (unsigned i=0;i<4;++i) for (unsigned word=0;word<4;++word)
        assert(ctx->gpr[registers[i]]._u32[word]==(word?0:preferred[i]));
}
static void generated_tag_wait_helper(Fixture *fixture,ps3_spu_executor *executor,const unsigned char *module_ls) {
    uint64_t clock=0;
    const ps3_spu_executor_start_config config={&ps3_native_wws_linked_image,1,{&clock,fixture_clock},PS3_SPU_THREAD_OPTION_NONE,128};
    assert(ps3_spu_executor_start(executor,&config)==PS3_SPU_EXEC_OK);
    memcpy(executor->core.ls+0xA00,module_ls+0xA00,0x2D80);
    executor->policy=(ps3_spu_policy_callbacks){NULL,stop_before_fixture_selection,0x290,0x808};
    executor->core.policy_mode=1; executor->core.image_id=1; executor->core.pc=0x2330;
    executor->core.gpr[3]=spu_make_preferred_u32(1u<<3);
    executor->core.gpr[4]=spu_make_preferred_u32(0x808);
    int status=ps3_spu_executor_enter_slice(executor,pm_wwsjob_spu_func_00002330,128);
    printf("ACTUAL_WWS_TAG_WAIT helper=0x2330 count_block=0x2340 status=%d terminal=%d pc=0x%X channel=%u tag_mask=0x%X pending=%u tag_available=%u provider_reads=%u provider_writes=%u live_job=0\n",
        status,executor->terminal.code,executor->core.pc,executor->terminal.channel,executor->core.mfc_tag_mask,
        executor->binding.status.pending_tag_update,executor->binding.status.tag_available,fixture->reads,fixture->writes);
    fflush(stdout);
    assert(status==PS3_SPU_EXEC_KERNEL_EXIT);
    assert(executor->core.gpr[3]._u32[0]==UINT32_MAX && executor->core.mfc_tag_mask==(1u<<3));
    assert(!executor->binding.status.tag_available && !executor->binding.status.pending_tag_update);
    assert(!fixture->reads && !fixture->writes && !actual_select_requests);
}
int main(void) {
    const char *module=getenv("PS3_SPURS_WWS_MODULE"); assert(module);
    Fixture f={0}; ps3_spu_executor *owner=create(&f);
    unsigned char *ls=aligned_alloc(16,SPU_LS_SIZE); assert(ls); memset(ls,0,SPU_LS_SIZE);
    FILE *input=fopen(module,"rb"); assert(input);
    assert(fread(ls+0xA00,1,0x2D80,input)==0x2D80 && fgetc(input)==EOF); assert(!fclose(input));
    store_be32(ls,0x1C4,0x7CF100); store_be32(ls,0x1C8,0); store_be32(ls,0x1CC,31);
    store_be32(ls,0x1D4,0x69C880); store_be32(ls,0x1D8,0); store_be32(ls,0x1DC,0);
    store_be32(ls,0x1E0,0x808); store_be32(ls,0x1E4,0x290);
    ps3_spurs_policy_execution *adapter=calloc(1,sizeof(*adapter)); assert(adapter);
    uint64_t clock=0; RefusedTransfer refused={.fixture=&f};
    const ps3_spu_executor_services services={&refused,refuse_first_transfer,actual_checkpoint,actual_reserve,actual_finish};
    const ps3_spurs_policy_execution_config config={&ps3_native_wws_linked_image,pm_wwsjob_spu_func_00002BF0,
        {&clock,fixture_clock},services,0x290,0x808};
    assert(ps3_spurs_policy_execution_init(adapter,&config)==PS3_SPURS_POLICY_READY);
    ps3_spurs_policy_execution_port port=ps3_spurs_policy_execution_bind(adapter);
    ps3_spurs_policy_execution_request request={.domain=&f.domain,.owner=owner->memory.owner,.local_store=ls,
        .local_store_size=SPU_LS_SIZE,.instance_address=0x7CF100,.spu_index=0,.workload_id=0,
        .module_address=0x69C880,.module_size=0x2D80,.unique_id=0,.poll_status=0,.module_reloaded=1,
        .data=UINT64_C(0x404E9300),.selection_context=NULL,.select=stop_before_fixture_selection};
    retained_dispatch_and_cancel(request,owner->services);
    input=fopen(module,"rb"); assert(input); assert(fread(ls+0xA00,1,0x2D80,input)==0x2D80); assert(!fclose(input));
    ps3_spurs_policy_execution_result actual=port.begin(port.context,&request); assert(actual.status==PS3_SPURS_POLICY_READY);
    entry_abi_words(&adapter->lanes[0].executor.core,request.poll_status);
    const uint32_t incoming_high=adapter->lanes[0].executor.core.gpr[4]._u32[0];
    const uint32_t incoming_low=adapter->lanes[0].executor.core.gpr[4]._u32[1];
    for (unsigned slice=0;slice<8;++slice) {
        actual=port.step(port.context,0,128);
        if (actual.status!=PS3_SPURS_POLICY_SLICE_CONTINUE) break;
    }
    const ps3_spu_executor *executed=&adapter->lanes[0].executor;
    assert(adapter->lanes[0].entered && spu_ls_read32(&executed->core,0)!=0);
    assert(actual.status!=PS3_SPURS_POLICY_READY && actual.status!=PS3_SPURS_POLICY_INVALID);
    printf("ACTUAL_WWS_ENTRY entry=0x2BF0 image=1 status=%u pc=0x%08X channel=0x%X stop=0x%X hops=%llu event_mask=0x%X select_requests=%u fixture_selection_performed=0 live_game_job=0 provider=%u ea=0x%llX lsa=0x%X bytes=%u command=0x%X drains=%u\n",
        actual.status,actual.pc,actual.channel,actual.stop_code,(unsigned long long)actual.hops,executed->core.event_mask,actual_select_requests,
        executed->terminal.provider_status,(unsigned long long)executed->terminal.request.ea,executed->terminal.request.lsa,
        executed->terminal.request.bytes,executed->terminal.request.command,executed->boundary_drains);
    printf("ACTUAL_WWS_ARGUMENT data=0x%llX r4_word0=0x%08X r4_word1=0x%08X first_transfer_refused=1 calls=%u bytes=%zu guest_provider_reads=%u guest_provider_writes=%u queue_fixture_supplied=0\n",
        (unsigned long long)request.data,incoming_high,incoming_low,refused.calls,refused.bytes,f.reads,f.writes);
    fflush(stdout);
    assert(refused.calls==1 && refused.ea==request.data && refused.bytes==128 && !refused.write);
    assert(executed->terminal.request.command==MFC_GETLLAR_CMD && executed->terminal.request.lsa==0x80);
    assert(actual.pc==0x242C && actual.channel==MFC_Cmd && executed->terminal.code==PS3_SPU_EXEC_ACCESS);
    assert(!executed->terminal.effect_committed && !f.reads && !f.writes && !f.reserves && !actual_select_requests);
    assert(spu_ls_read32(&executed->core,0x13F0)==0x404E9300);
    if (actual.status==PS3_SPURS_POLICY_SLICE_CONTINUE) assert(port.cancel(port.context,0)==PS3_SPURS_POLICY_CANCELLED);
    assert(port.release(port.context,0)==PS3_SPURS_POLICY_READY);
    input=fopen(module,"rb"); assert(input); assert(fread(ls+0xA00,1,0x2D80,input)==0x2D80); assert(!fclose(input));
    request.data=UINT64_C(0x12345678404E9300); request.poll_status=3;
    assert(port.begin(port.context,&request).status==PS3_SPURS_POLICY_READY);
    const spu_context *abi=&adapter->lanes[0].executor.core;
    entry_abi_words(abi,request.poll_status);
    assert(abi->gpr[4]._u32[0]==0x12345678 && abi->gpr[4]._u32[1]==0x404E9300);
    assert(!abi->gpr[4]._u32[2] && !abi->gpr[4]._u32[3]);
    assert(port.cancel(port.context,0)==PS3_SPURS_POLICY_CANCELLED);
    assert(port.release(port.context,0)==PS3_SPURS_POLICY_READY);
    puts("POLICY_ARGUMENT_ABI high_nonzero=1 low_nonzero=1 r0_r1_r3_r5_unchanged=1 argument_not_executed=1");
    generated_tag_wait_helper(&f,owner,ls);
    free(adapter); free(ls); finish(&f,owner);
    return 0;
}
