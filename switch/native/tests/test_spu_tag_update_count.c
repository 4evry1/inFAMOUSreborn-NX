/* Real channel engine/owner fixture. Synthetic tag masks describe channel
 * state only; no game queue, selected job, or fabricated DMA completion. */
#define main ps3_existing_spu_executor_regressions
#include "test_spu_executor.c"
#undef main

static void count_is_nonmutating_capacity(ps3_spu_executor *owner) {
    const spu_context before=owner->core;
    const ps3_spu_status_slots slots=owner->binding.status;
    const mfc_engine engine=owner->engine;
    uint32_t count=0xDEADBEEF;
    assert(ps3_spu_executor_count_channel(owner,MFC_WrTagUpdate,&count)==PS3_SPU_EXEC_OK && count==1);
    assert(!memcmp(&before,&owner->core,sizeof(before)));
    assert(!memcmp(&slots,&owner->binding.status,sizeof(slots)));
    assert(!memcmp(&engine,&owner->engine,sizeof(engine)) && !owner->binding.effect_committed);
}
static void empty_and_unread_slots(void) {
    Fixture fixture; ps3_spu_executor *owner=create(&fixture);
    count_is_nonmutating_capacity(owner);
    uint32_t count=99;
    assert(ps3_spu_executor_count_channel(owner,MFC_RdTagStat,&count)==PS3_SPU_EXEC_OK && !count);
    assert(write_channel(owner,MFC_WrTagMask,1u<<3)==PS3_SPU_EXEC_OK);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_IMMEDIATE)==PS3_SPU_EXEC_OK);
    assert(owner->binding.status.tag_available && owner->core.mfc_tag_status==(1u<<3));
    count_is_nonmutating_capacity(owner); count_is_nonmutating_capacity(owner);
    u128 value;
    assert(ps3_spu_executor_read_channel(owner,MFC_RdTagStat,&value)==PS3_SPU_EXEC_OK && value._u32[0]==(1u<<3));
    count_is_nonmutating_capacity(owner);
    assert(!owner->binding.status.tag_available && !fixture.reads && !fixture.writes);
    finish(&fixture,owner);
}
static void pending_update_and_real_completion(void) {
    for (unsigned mode=PS3_SPU_TAG_UPDATE_ANY;mode<=PS3_SPU_TAG_UPDATE_ALL;++mode) {
        Fixture fixture; ps3_spu_executor *owner=create(&fixture);
        assert(write_channel(owner,MFC_WrTagMask,1u<<3)==0);
        assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_IMMEDIATE)==0);
        owner->engine.tag_completed=0;
        assert(write_channel(owner,MFC_WrTagUpdate,mode)==0);
        assert(owner->binding.status.pending_tag_update==mode && owner->binding.status.tag_available);
        count_is_nonmutating_capacity(owner);
        u128 old;
        assert(ps3_spu_executor_read_channel(owner,MFC_RdTagStat,&old)==0 && old._u32[0]==(1u<<3));
        assert(owner->binding.status.pending_tag_update==mode && !owner->binding.status.tag_available);
        count_is_nonmutating_capacity(owner);
        parameters(owner,0x100,0x1000,16,3);
        assert(write_channel(owner,MFC_Cmd,MFC_GET_CMD)==0);
        assert(fixture.reads==1 && fixture.traces==1 && owner->binding.status.tag_available);
        assert(owner->binding.status.pending_tag_update==PS3_SPU_TAG_UPDATE_IMMEDIATE && owner->core.mfc_tag_status==(1u<<3));
        count_is_nonmutating_capacity(owner);
        finish(&fixture,owner);
    }
}
static void mask_change_and_no_count_reevaluation(void) {
    Fixture fixture; ps3_spu_executor *owner=create(&fixture);
    owner->engine.tag_completed=0;
    assert(write_channel(owner,MFC_WrTagMask,(1u<<2)|(1u<<3))==0);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_ALL)==0);
    count_is_nonmutating_capacity(owner);
    parameters(owner,0x100,0x1000,16,2); assert(write_channel(owner,MFC_Cmd,MFC_GET_CMD)==0);
    assert(!owner->binding.status.tag_available && owner->binding.status.pending_tag_update==PS3_SPU_TAG_UPDATE_ALL);
    count_is_nonmutating_capacity(owner);
    assert(write_channel(owner,MFC_WrTagMask,1u<<2)==0);
    assert(owner->binding.status.tag_available && !owner->binding.status.pending_tag_update && owner->core.mfc_tag_status==(1u<<2));
    u128 value; assert(ps3_spu_executor_read_channel(owner,MFC_RdTagStat,&value)==0);
    assert(write_channel(owner,MFC_WrTagMask,1u<<3)==0);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_ANY)==0);
    assert(owner->binding.status.pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && !owner->binding.status.tag_available);
    /* A count must not act as a completion/update callback, even if engine
     * completion changes before its normal explicit reevaluation point. */
    owner->engine.tag_completed|=1u<<3;
    count_is_nonmutating_capacity(owner);
    assert(owner->binding.status.pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && !owner->binding.status.tag_available);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_IMMEDIATE)==0);
    assert(owner->binding.status.tag_available && !owner->binding.status.pending_tag_update);
    count_is_nonmutating_capacity(owner);
    finish(&fixture,owner);
}
static void zero_mask_and_unmodeled_read_work(void) {
    Fixture fixture; ps3_spu_executor *owner=create(&fixture);
    assert(write_channel(owner,MFC_WrTagMask,0)==0);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_ANY)==0);
    assert(owner->binding.status.pending_tag_update==PS3_SPU_TAG_UPDATE_ANY && !owner->binding.status.tag_available);
    count_is_nonmutating_capacity(owner);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_ALL)==0);
    assert(!owner->binding.status.pending_tag_update && owner->binding.status.tag_available && !owner->core.mfc_tag_status);
    count_is_nonmutating_capacity(owner);
    owner->engine.queue_count=1; owner->core.list_stall_mask=1u<<3;
    count_is_nonmutating_capacity(owner);
    owner->engine.queue_count=0; owner->core.list_stall_mask=0;
    finish(&fixture,owner);
}
static void count_cancel_and_other_channels(void) {
    Fixture fixture; ps3_spu_executor *owner=create(&fixture);
    owner->engine.tag_completed=0; assert(write_channel(owner,MFC_WrTagMask,1u<<3)==0);
    assert(write_channel(owner,MFC_WrTagUpdate,PS3_SPU_TAG_UPDATE_ALL)==0);
    const ps3_spu_status_slots slots=owner->binding.status;
    const spu_context context=owner->core;
    fixture.cancel_phase=PS3_SPU_PRE_CHANNEL; uint32_t count=0x12345678;
    assert(ps3_spu_executor_count_channel(owner,MFC_WrTagUpdate,&count)==PS3_SPU_EXEC_CANCELLED);
    assert(count==0x12345678 && owner->terminal.channel==MFC_WrTagUpdate && !owner->terminal.effect_committed);
    assert(!memcmp(&slots,&owner->binding.status,sizeof(slots)) && !memcmp(&context,&owner->core,sizeof(context)));
    assert(!fixture.reads && !fixture.writes); finish(&fixture,owner);
    owner=create(&fixture); count=0x12345678;
    assert(ps3_spu_executor_count_channel(owner,MFC_Cmd,&count)==PS3_SPU_EXEC_UNSUPPORTED);
    assert(count==0x12345678 && !owner->terminal.effect_committed && !fixture.checkpoints);
    finish(&fixture,owner);
}
int main(void) {
    empty_and_unread_slots(); pending_update_and_real_completion(); mask_change_and_no_count_reevaluation();
    zero_mask_and_unmodeled_read_work(); count_cancel_and_other_channels();
    puts("SPU_TAG_UPDATE_COUNT_PASS reference_capacity=1 pending_updates_unchanged=1 real_dma_completion=1 nonmutating_count=1 cancellation=1");
    return 0;
}
