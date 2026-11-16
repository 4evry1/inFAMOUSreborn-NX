#include "spurs_policy_execution.h"
#include <string.h>

enum { WWS_MODULE_LSA=0xA00, WWS_MODULE_BYTES=0x2D80, WWS_ENTRY_LSA=0x2BF0, WWS_IMAGE_ID=1,
       SPURS_POLICY_STACK=0x3FFB0, SPURS_POLICY_ARGUMENT=0x100, SPURS_WORKLOADS=16 };
static const uint64_t WWS_MODULE_FINGERPRINT=UINT64_C(0x6FFFB30E41EE17BC);
static int valid_linked_image(const ps3_spu_linked_image *image) {
    if (!image || !image->functions || !image->count || image->count>SPU_LS_SIZE/4u) return 0;
    for (size_t i=0;i<image->count;++i) {
        const ps3_spu_lifted_function *f=&image->functions[i];
        if (!f->function || f->pc>=SPU_LS_SIZE || (f->pc&3u) || (i && f->pc<=image->functions[i-1].pc)) return 0;
    }
    return 1;
}
static uint64_t module_fingerprint(const unsigned char *bytes,size_t count) {
    uint64_t value=UINT64_C(1469598103934665603);
    for (size_t i=0;i<count;++i) value=(value^bytes[i])*UINT64_C(1099511628211);
    return value;
}
static ps3_spurs_policy_execution_result result(ps3_spurs_policy_execution_status status) {
    return (ps3_spurs_policy_execution_result){.status=status};
}
static int access_memory(void *context,uint64_t ea,size_t bytes,int write) {
    ps3_spurs_policy_execution_lane *lane=context;
    ps3_spu_executor_services *s=&lane->adapter->config.services;
    return s->access(s->context,ea,bytes,write);
}
static int checkpoint(void *context,unsigned phase) {
    ps3_spurs_policy_execution_lane *lane=context;
    if (lane->cancel_requested) return PS3_SPU_EXEC_CANCELLED;
    ps3_spu_executor_services *s=&lane->adapter->config.services;
    return s->checkpoint(s->context,phase);
}
static int reserve_trace(void *context,const ps3_spu_transfer *transfer) {
    ps3_spurs_policy_execution_lane *lane=context;
    ps3_spu_executor_services *s=&lane->adapter->config.services;
    return s->reserve_trace(s->context,transfer);
}
static void finish_trace(void *context,const ps3_spu_transfer *transfer,ps3_guest_memory_status status,int committed,const void *bytes) {
    ps3_spurs_policy_execution_lane *lane=context;
    ps3_spu_executor_services *s=&lane->adapter->config.services;
    s->finish_trace(s->context,transfer,status,committed,bytes);
}
static int select_workload(void *context,uint32_t spu,unsigned is_poll,uint64_t *packed) {
    ps3_spurs_policy_execution_lane *lane=context;
    if (lane->cancel_requested) return PS3_SPU_EXEC_CANCELLED;
    if (spu!=lane->request.spu_index || !lane->request.select) return PS3_SPU_EXEC_UNSUPPORTED;
    int status=lane->request.select(lane->request.selection_context,spu,is_poll,packed);
    return !status ? PS3_SPU_EXEC_OK : status==PS3_SPURS_POLICY_CANCELLED ? PS3_SPU_EXEC_CANCELLED :
        status==PS3_SPURS_POLICY_UNSUPPORTED ? PS3_SPU_EXEC_UNSUPPORTED : PS3_SPU_EXEC_MEMORY;
}
static ps3_spurs_policy_execution_result execution_result(ps3_spurs_policy_execution_lane *lane,int code) {
    ps3_spu_executor *e=&lane->executor;
    ps3_spurs_policy_execution_status status;
    switch (code) {
    case PS3_SPU_EXEC_SLICE: status=PS3_SPURS_POLICY_SLICE_CONTINUE; break;
    case PS3_SPU_EXEC_KERNEL_EXIT: status=PS3_SPURS_POLICY_KERNEL_EXIT; break;
    case PS3_SPU_EXEC_STOP: status=PS3_SPURS_POLICY_STOP; break;
    case PS3_SPU_EXEC_CANCELLED: status=PS3_SPURS_POLICY_CANCELLED; break;
    case PS3_SPU_EXEC_UNSUPPORTED: status=PS3_SPURS_POLICY_UNSUPPORTED; break;
    case PS3_SPU_EXEC_INVALID: status=PS3_SPURS_POLICY_INVALID; break;
    default: status=PS3_SPURS_POLICY_FAULT; break;
    }
    lane->last=(ps3_spurs_policy_execution_result){status,e->core.pc,e->terminal.channel,e->core.stop_code,e->hops,
        status==PS3_SPURS_POLICY_SLICE_CONTINUE && e->continuation && !e->core.host_depth};
    return lane->last;
}
static ps3_spurs_policy_execution_result begin_policy(void *context,const ps3_spurs_policy_execution_request *request) {
    ps3_spurs_policy_execution *adapter=context;
    if (!adapter || !adapter->initialized || !request || request->spu_index>=PS3_SPURS_POLICY_EXECUTION_LANES ||
        request->workload_id>=SPURS_WORKLOADS || !request->domain || request->owner.domain!=request->domain || !request->local_store ||
        request->local_store_size!=SPU_LS_SIZE || ((uintptr_t)request->local_store&15u) ||
        request->module_size!=WWS_MODULE_BYTES || request->module_address>UINT32_MAX-WWS_MODULE_BYTES || request->module_reloaded>1u ||
        !request->select || !request->instance_address || (request->instance_address&127u))
        return result(PS3_SPURS_POLICY_INVALID);
    ps3_spurs_policy_execution_lane *lane=&adapter->lanes[request->spu_index];
    if (lane->active) return result(PS3_SPURS_POLICY_BUSY);
    const int same_owner=lane->executor.initialized && lane->executor.memory.domain==request->domain &&
        lane->executor.memory.owner.slot==request->owner.slot && lane->executor.memory.owner.generation==request->owner.generation &&
        lane->executor.core.ls==request->local_store;
    const int reuse=same_owner && lane->retained;
    if (!request->module_reloaded && (!reuse || lane->request.module_address!=request->module_address ||
        lane->request.module_size!=request->module_size || lane->request.unique_id!=request->unique_id))
        return result(PS3_SPURS_POLICY_UNSUPPORTED);
    if (request->module_reloaded && module_fingerprint(request->local_store+WWS_MODULE_LSA,WWS_MODULE_BYTES)!=WWS_MODULE_FINGERPRINT)
        return result(PS3_SPURS_POLICY_UNSUPPORTED);
    for (uint32_t i=0;i<PS3_SPURS_POLICY_EXECUTION_LANES;++i) {
        const ps3_spurs_policy_execution_lane *other=&adapter->lanes[i];
        if (other!=lane && other->active && (other->request.local_store==request->local_store ||
            (other->request.domain==request->domain && other->request.owner.slot==request->owner.slot &&
             other->request.owner.generation==request->owner.generation))) return result(PS3_SPURS_POLICY_BUSY);
    }
    if (!reuse) {
        if (lane->executor.initialized && ps3_spu_executor_release(&lane->executor)) return result(PS3_SPURS_POLICY_BUSY);
        memset(lane,0,sizeof(*lane)); lane->adapter=adapter;
        ps3_spu_executor_services services={lane,access_memory,checkpoint,reserve_trace,finish_trace};
        ps3_guest_memory_status memory=ps3_spu_executor_init_borrowed(&lane->executor,request->domain,request->owner,request->local_store,
                                                                   request->local_store_size,request->spu_index,services);
        if (memory) return result(PS3_SPURS_POLICY_FAULT);
    } else {
        /* Dispatch only overwrites the ABI registers below. General registers,
         * channels, MFC slots and decrementer belong to the persistent SPU. */
        lane->entered=0; lane->cancel_requested=0;
    }
    lane->request=*request; lane->retained=0;
    ps3_spu_executor *e=&lane->executor;
    ps3_spu_executor_start_config start={adapter->config.image,WWS_IMAGE_ID,adapter->config.clock,
                                        PS3_SPU_THREAD_OPTION_NONE,UINT64_MAX};
    int status=reuse ? PS3_SPU_EXEC_OK : ps3_spu_executor_start(e,&start);
    if (status) { (void)ps3_spu_executor_release(e); return execution_result(lane,status); }
    e->policy=(ps3_spu_policy_callbacks){lane,select_workload,adapter->config.kernel_select_pc,adapter->config.kernel_exit_pc};
    e->core.pc=WWS_ENTRY_LSA; e->core.image_id=WWS_IMAGE_ID; e->core.module_img_a00=WWS_IMAGE_ID;
    e->core.policy_mode=1; e->core.status=SPU_STATUS_RUNNING;
    e->core.gpr[0]=spu_make_preferred_u32(adapter->config.kernel_exit_pc);
    e->core.gpr[1]=spu_make_preferred_u32(SPURS_POLICY_STACK);
    e->core.gpr[3]=spu_make_preferred_u32(SPURS_POLICY_ARGUMENT);
    e->core.gpr[4]=(u128){0};
    /* Registers store architectural word order in host-native words. A raw
     * host u64 assignment swaps the two words of the preferred doubleword. */
    e->core.gpr[4]._u32[0]=(uint32_t)(request->data>>32);
    e->core.gpr[4]._u32[1]=(uint32_t)request->data;
    e->core.gpr[5]=spu_make_preferred_u32(request->poll_status);
    lane->active=1; lane->last=result(PS3_SPURS_POLICY_READY); lane->last.pc=WWS_ENTRY_LSA;
    return lane->last;
}
static ps3_spurs_policy_execution_result step_policy(void *context,uint32_t index,uint64_t budget) {
    ps3_spurs_policy_execution *adapter=context;
    if (!adapter || !adapter->initialized || index>=PS3_SPURS_POLICY_EXECUTION_LANES || !budget) return result(PS3_SPURS_POLICY_INVALID);
    ps3_spurs_policy_execution_lane *lane=&adapter->lanes[index];
    if (!lane->active) return result(PS3_SPURS_POLICY_INVALID);
    if (lane->cancel_requested) { lane->last.status=PS3_SPURS_POLICY_CANCELLED; lane->last.resumable=0; return lane->last; }
    if (lane->entered && lane->last.status!=PS3_SPURS_POLICY_SLICE_CONTINUE) return lane->last;
    int code;
    if (!lane->entered) { lane->entered=1; code=ps3_spu_executor_enter_slice(&lane->executor,adapter->config.entry,budget); }
    else code=ps3_spu_executor_continue_slice(&lane->executor,budget);
    return execution_result(lane,code);
}
static ps3_spurs_policy_execution_result resume_policy(void *context,uint32_t index,uint64_t selection) {
    (void)context; (void)index; (void)selection;
    /* This adapter performs select synchronously while the generated stack is
     * live. It never emits a resumable KERNEL_SELECT event. */
    return result(PS3_SPURS_POLICY_INVALID);
}
static ps3_spurs_policy_execution_status cancel_policy(void *context,uint32_t index) {
    ps3_spurs_policy_execution *adapter=context;
    if (!adapter || !adapter->initialized || index>=PS3_SPURS_POLICY_EXECUTION_LANES || !adapter->lanes[index].active)
        return PS3_SPURS_POLICY_INVALID;
    ps3_spurs_policy_execution_lane *lane=&adapter->lanes[index];
    if (lane->executor.escape) return PS3_SPURS_POLICY_BUSY;
    lane->cancel_requested=1; lane->last.status=PS3_SPURS_POLICY_CANCELLED; lane->last.resumable=0;
    return PS3_SPURS_POLICY_CANCELLED;
}
static ps3_spurs_policy_execution_status release_policy(void *context,uint32_t index) {
    ps3_spurs_policy_execution *adapter=context;
    if (!adapter || !adapter->initialized || index>=PS3_SPURS_POLICY_EXECUTION_LANES || !adapter->lanes[index].active)
        return PS3_SPURS_POLICY_INVALID;
    ps3_spurs_policy_execution_lane *lane=&adapter->lanes[index];
    if (lane->executor.escape || lane->executor.trace_reserved) return PS3_SPURS_POLICY_BUSY;
    if (lane->last.status==PS3_SPURS_POLICY_SLICE_CONTINUE && !lane->cancel_requested) return PS3_SPURS_POLICY_BUSY;
    (void)ps3_spu_executor_take_continuation(&lane->executor);
    if (lane->last.status==PS3_SPURS_POLICY_KERNEL_EXIT && !lane->cancel_requested) {
        lane->active=0; lane->retained=1; return PS3_SPURS_POLICY_READY;
    }
    ps3_guest_memory_status status=ps3_spu_executor_release(&lane->executor);
    if (status) return PS3_SPURS_POLICY_FAULT;
    lane->active=0; return PS3_SPURS_POLICY_READY;
}
ps3_spurs_policy_execution_status ps3_spurs_policy_execution_init(ps3_spurs_policy_execution *adapter,const ps3_spurs_policy_execution_config *config) {
    if (!adapter || !config || !valid_linked_image(config->image) || !config->entry || !config->clock.now_microseconds ||
        !config->services.access || !config->services.checkpoint || !config->services.reserve_trace || !config->services.finish_trace ||
        !config->kernel_select_pc || !config->kernel_exit_pc || config->kernel_select_pc==config->kernel_exit_pc ||
        config->kernel_select_pc>=WWS_MODULE_LSA || config->kernel_exit_pc>=WWS_MODULE_LSA ||
        (config->kernel_select_pc&3u) || (config->kernel_exit_pc&3u) ||
        ps3_spu_linked_image_lookup(config->image,WWS_ENTRY_LSA)!=config->entry) return PS3_SPURS_POLICY_INVALID;
    if (adapter->initialized) return PS3_SPURS_POLICY_BUSY;
    adapter->config=*config; adapter->initialized=1; return PS3_SPURS_POLICY_READY;
}
ps3_spurs_policy_execution_port ps3_spurs_policy_execution_bind(ps3_spurs_policy_execution *adapter) {
    return (ps3_spurs_policy_execution_port){adapter,begin_policy,step_policy,resume_policy,cancel_policy,release_policy};
}
