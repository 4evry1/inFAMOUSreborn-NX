#include "startup_wws_composition.h"
#include "spurs_policy_execution.h"
#include <stdlib.h>

extern const ps3_spu_linked_image ps3_native_wws_linked_image;
void pm_wwsjob_spu_func_00002BF0(spu_context *);
struct ps3_startup_wws_composition {
    ps3_startup_wws_config services;
    ps3_spurs_policy_execution adapter;
};
_Static_assert((int)PS3_STARTUP_WWS_LANES==(int)PS3_SPURS_POLICY_EXECUTION_LANES,"joined lane count");
static int access_memory(void *context,uint64_t address,size_t bytes,int write) {
    (void)context; (void)write;
    return address<=UINT32_MAX && bytes<=PS3_GUEST_ADDRESS_BYTES-address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
static int checkpoint(void *context,unsigned phase) {
    ps3_startup_wws_composition *owner=context; (void)phase;
    return owner->services.is_cancelled(owner->services.cancellation_context) ? PS3_SPU_EXEC_CANCELLED : PS3_SPU_EXEC_OK;
}
static int reserve_trace(void *context,const ps3_spu_transfer *transfer) {
    (void)context; (void)transfer; return PS3_SPU_EXEC_OK;
}
static void finish_trace(void *context,const ps3_spu_transfer *transfer,ps3_guest_memory_status status,int committed,const void *bytes) {
    (void)context; (void)transfer; (void)status; (void)committed; (void)bytes;
}
ps3_spurs_policy_execution_status ps3_startup_wws_create(const ps3_startup_wws_config *config,ps3_startup_wws_composition **out) {
    if (!config || !out || !config->clock.now_microseconds || !config->is_cancelled) return PS3_SPURS_POLICY_INVALID;
    if (*out) return PS3_SPURS_POLICY_BUSY;
    ps3_startup_wws_composition *owner=calloc(1,sizeof(*owner));
    if (!owner) return PS3_SPURS_POLICY_FAULT;
    owner->services=*config;
    const ps3_spurs_policy_execution_config execution={&ps3_native_wws_linked_image,pm_wwsjob_spu_func_00002BF0,config->clock,
        {owner,access_memory,checkpoint,reserve_trace,finish_trace},0x290,0x808};
    ps3_spurs_policy_execution_status status=ps3_spurs_policy_execution_init(&owner->adapter,&execution);
    if (status!=PS3_SPURS_POLICY_READY) { free(owner); return status; }
    *out=owner; return PS3_SPURS_POLICY_READY;
}
ps3_spurs_policy_execution_port ps3_startup_wws_port(ps3_startup_wws_composition *owner) {
    return owner ? ps3_spurs_policy_execution_bind(&owner->adapter) : (ps3_spurs_policy_execution_port){0};
}
ps3_spurs_policy_execution_status ps3_startup_wws_read_joined(const ps3_startup_wws_composition *owner,uint32_t index,ps3_startup_wws_diagnostics *out) {
    if (!owner || !out || index>=PS3_STARTUP_WWS_LANES) return PS3_SPURS_POLICY_INVALID;
    const ps3_spurs_policy_execution_lane *lane=&owner->adapter.lanes[index];
    if (lane->executor.escape) return PS3_SPURS_POLICY_BUSY;
    *out=(ps3_startup_wws_diagnostics){.initialized=owner->adapter.initialized,.active=lane->active,.entered=lane->entered,.retained=lane->retained,
        .last=lane->last,.terminal_code=lane->executor.terminal.code,.provider_status=lane->executor.terminal.provider_status,
        .effect_committed=lane->executor.terminal.effect_committed,.request_ea=lane->executor.terminal.request.ea,
        .request_lsa=lane->executor.terminal.request.lsa,.request_bytes=lane->executor.terminal.request.bytes,
        .request_command=lane->executor.terminal.request.command};
    return PS3_SPURS_POLICY_READY;
}
ps3_spurs_policy_execution_status ps3_startup_wws_destroy_joined(ps3_startup_wws_composition **handle) {
    if (!handle || !*handle) return PS3_SPURS_POLICY_INVALID;
    ps3_startup_wws_composition *owner=*handle;
    for (uint32_t i=0;i<PS3_STARTUP_WWS_LANES;++i) {
        ps3_spurs_policy_execution_lane *lane=&owner->adapter.lanes[i];
        if (lane->active || lane->executor.escape || lane->executor.trace_reserved || lane->executor.continuation) return PS3_SPURS_POLICY_BUSY;
    }
    for (uint32_t i=0;i<PS3_STARTUP_WWS_LANES;++i) {
        ps3_spu_executor *executor=&owner->adapter.lanes[i].executor;
        if (executor->initialized && ps3_spu_executor_release(executor)!=PS3_GUEST_MEMORY_OK) return PS3_SPURS_POLICY_FAULT;
    }
    free(owner); *handle=NULL; return PS3_SPURS_POLICY_READY;
}
