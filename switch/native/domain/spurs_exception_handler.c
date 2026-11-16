#include "spurs_exception_handler.h"
#include <stdatomic.h>
#include <string.h>

enum {
    POLICY_NULL=0x80410811u, POLICY_ALIGN=0x80410810u, POLICY_INVAL=0x80410802u,
    POLICY_SRCH=0x80410805u, POLICY_STAT=0x8041080Fu, POLICY_BUSY=0x8041080Au,
    FLAGS_OFFSET=0x70, ENABLED_OFFSET=0xB0, EXCEPTION_OFFSET=0xD6C,
    PRIMARY_HANDLERS=0x100, SECONDARY_HANDLERS=0x1200, HANDLER_STRIDE=0x80,
    ARGUMENT_OFFSET=8, PRIMARY_BYTES=0x1000, SECONDARY_BYTES=0x2000,
    SPURS2_FLAG=0x40000000u, PRIMARY_WORKLOADS=16, SECONDARY_WORKLOADS=32
};
typedef struct {
    const ps3_spurs_exception_config *config;
    ps3_spurs_exception_result *result;
} Operation;
static int stop(Operation *operation,ps3_spurs_exception_stop reason) {
    operation->result->stop=reason; return 0;
}
static int boundary(Operation *operation) {
    const ps3_spurs_exception_config *config=operation->config;
    return !config->may_continue || config->may_continue(config->poll_context)
        ? 1 : stop(operation,PS3_SPURS_EXCEPTION_CANCELLED);
}
static int memory_result(Operation *operation,ps3_guest_memory_status status) {
    if (status==PS3_GUEST_MEMORY_OK) return 1;
    operation->result->memory_status=status;
    return stop(operation,PS3_SPURS_EXCEPTION_MEMORY);
}
static int admit_memory(Operation *operation,uint64_t address,size_t bytes,unsigned write) {
    if (!boundary(operation)) return 0;
    const ps3_spurs_exception_config *config=operation->config;
    if (address>UINT32_MAX || bytes>PS3_GUEST_ADDRESS_BYTES-address || address<config->instance ||
        address-config->instance>config->owned_instance_bytes ||
        bytes>config->owned_instance_bytes-(address-config->instance))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    const ps3_spurs_exception_memory_scope *scope=&config->memory_scope;
    if (scope->permits && !scope->permits(scope->context,(uint32_t)address,bytes,write))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    return 1;
}
static int observe(Operation *operation,ps3_spurs_exception_operation kind,ps3_spurs_exception_stage stage,
                   uint32_t address,const unsigned char *bytes,size_t length) {
    const ps3_spurs_exception_event event={kind,stage,address,length,bytes};
    ++operation->result->observer_calls;
    if (!operation->config->observe(operation->config->observer_context,&event))
        return stop(operation,PS3_SPURS_EXCEPTION_OBSERVER);
    ++operation->result->publications; return 1;
}
static void encode64(unsigned char bytes[8],uint64_t value) {
    for (unsigned i=0;i<8;++i) bytes[7-i]=(unsigned char)(value>>(i*8));
}
static int read_value(Operation *operation,uint64_t address,size_t length,ps3_spurs_exception_stage stage,uint64_t *value) {
    unsigned char bytes[8];
    if (!admit_memory(operation,address,length,0) ||
        !memory_result(operation,ps3_atomic_owner_read(operation->config->domain,operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->reads;
    if (!observe(operation,PS3_SPURS_EXCEPTION_READ,stage,(uint32_t)address,bytes,length)) return 0;
    *value=0;
    for (size_t i=0;i<length;++i) *value=(*value<<8)|bytes[i];
    return 1;
}
static void release_fence(Operation *operation) {
    /* The coherent port completes each store before returning. Match the
     * authentic lwsync publication points without adding an acquire barrier
     * or serializing the complete registration operation. */
    atomic_thread_fence(memory_order_release);
    ++operation->result->release_fences;
}
static int write_value(Operation *operation,uint32_t address,uint64_t value,ps3_spurs_exception_stage stage,
                       unsigned *attempted,unsigned *confirmed) {
    unsigned char bytes[8]; encode64(bytes,value);
    if (!admit_memory(operation,address,sizeof(bytes),1)) return 0;
    *attempted=1;
    if (!memory_result(operation,ps3_atomic_owner_write(operation->config->domain,operation->config->owner,address,bytes,sizeof(bytes)))) return 0;
    *confirmed=1; ++operation->result->writes; operation->result->written_bytes+=sizeof(bytes);
    release_fence(operation);
    return observe(operation,PS3_SPURS_EXCEPTION_WRITE,stage,address,bytes,sizeof(bytes));
}
static void sdk_result(Operation *operation,uint32_t value) {
    if (boundary(operation)) { operation->result->has_sdk_result=1; operation->result->sdk_result=value; }
}
int ps3_spurs_exception_validate_address(const ps3_spurs_exception_request *request,uint32_t *result) {
    if (!request || !result || (request->action!=PS3_SPURS_EXCEPTION_SET && request->action!=PS3_SPURS_EXCEPTION_UNSET)) return 0;
    *result=(!request->spurs || (request->action==PS3_SPURS_EXCEPTION_SET && !request->handler))
        ? POLICY_NULL : (request->spurs&127u) ? POLICY_ALIGN : 0;
    return 1;
}
static int claim_handler(Operation *operation) {
    ps3_spurs_exception_result *result=operation->result;
    const ps3_spurs_exception_config *config=operation->config;
    for (;;) {
        if (result->claim_attempts>=config->max_claim_attempts) {
            if (boundary(operation)) stop(operation,PS3_SPURS_EXCEPTION_ATTEMPT_LIMIT);
            return 0;
        }
        if (!admit_memory(operation,result->slot,8,0)) return 0;
        uint64_t value=0; ++result->claim_attempts;
        if (!memory_result(operation,ps3_atomic_load_reserve64(config->domain,config->owner,result->slot,&value))) return 0;
        unsigned char observed[8]; encode64(observed,value); ++result->reads;
        if (!observe(operation,PS3_SPURS_EXCEPTION_READ,PS3_SPURS_EXCEPTION_LOAD_RESERVE,result->slot,observed,8)) return 0;
        if (value) { sdk_result(operation,POLICY_BUSY); return 0; }
        if (!admit_memory(operation,result->slot,8,1)) return 0;
        int stored=0; result->claim_write_attempted=1; ++result->conditional_attempts;
        if (!memory_result(operation,ps3_atomic_store_conditional64(config->domain,config->owner,result->slot,1,&stored))) return 0;
        if (!stored) { ++result->conditional_failures; continue; }
        result->claim_written=1; ++result->writes; result->written_bytes+=8;
        unsigned char sentinel[8]; encode64(sentinel,1);
        return observe(operation,PS3_SPURS_EXCEPTION_WRITE,PS3_SPURS_EXCEPTION_CLAIM,result->slot,sentinel,8);
    }
}
static void apply_owned(Operation *operation,const ps3_spurs_exception_request *request) {
    uint64_t flags,enabled,exception;
    const uint64_t base=request->spurs;
    if (!read_value(operation,base+FLAGS_OFFSET,8,PS3_SPURS_EXCEPTION_FLAGS,&flags)) return;
    const unsigned limit=(flags&SPURS2_FLAG) ? SECONDARY_WORKLOADS : PRIMARY_WORKLOADS;
    if (request->wid>=limit) { sdk_result(operation,POLICY_INVAL); return; }
    if (!read_value(operation,base+ENABLED_OFFSET,4,PS3_SPURS_EXCEPTION_ENABLED,&enabled)) return;
    if (!(enabled&(UINT32_C(0x80000000)>>request->wid))) { sdk_result(operation,POLICY_SRCH); return; }
    if (!read_value(operation,base+EXCEPTION_OFFSET,4,PS3_SPURS_EXCEPTION_STATE,&exception)) return;
    if (exception) { sdk_result(operation,POLICY_STAT); return; }
    const uint64_t slot=base+(request->wid<PRIMARY_WORKLOADS ? PRIMARY_HANDLERS+request->wid*HANDLER_STRIDE :
        SECONDARY_HANDLERS+(request->wid-PRIMARY_WORKLOADS)*HANDLER_STRIDE);
    if (slot>UINT32_MAX) { memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE); return; }
    ps3_spurs_exception_result *result=operation->result; result->slot=(uint32_t)slot;
    if (request->action==PS3_SPURS_EXCEPTION_UNSET) {
        if (!write_value(operation,result->slot,0,PS3_SPURS_EXCEPTION_CLEAR,&result->unset_write_attempted,&result->handler_cleared)) return;
    } else {
        if (!claim_handler(operation)) return;
        if (!write_value(operation,result->slot+ARGUMENT_OFFSET,request->argument,PS3_SPURS_EXCEPTION_ARGUMENT,
                         &result->argument_write_attempted,&result->argument_written)) return;
        if (!write_value(operation,result->slot,request->handler,PS3_SPURS_EXCEPTION_PUBLISH,
                         &result->handler_write_attempted,&result->handler_written)) return;
    }
    sdk_result(operation,0);
}
void ps3_spurs_exception_handler_apply(const ps3_spurs_exception_config *config,
    const ps3_spurs_exception_request *request,ps3_spurs_exception_result *result) {
    if (!result) return;
    ps3_spurs_exception_config captured_config;
    ps3_spurs_exception_request captured_request;
    if (config) captured_config=*config;
    if (request) captured_request=*request;
    memset(result,0,sizeof(*result)); Operation operation={&captured_config,result};
    if (!config || !request || !captured_config.domain || captured_config.owner.domain!=captured_config.domain ||
        !captured_config.observe || !captured_config.max_claim_attempts ||
        (captured_config.owned_instance_bytes!=PRIMARY_BYTES && captured_config.owned_instance_bytes!=SECONDARY_BYTES)) {
        stop(&operation,PS3_SPURS_EXCEPTION_INVALID_ARGUMENT); return;
    }
    uint32_t validation;
    if (!ps3_spurs_exception_validate_address(&captured_request,&validation)) {
        stop(&operation,PS3_SPURS_EXCEPTION_INVALID_ARGUMENT); return;
    }
    if (validation) { sdk_result(&operation,validation); return; }
    if (!captured_config.active_owned_instance || captured_request.spurs!=captured_config.instance) {
        stop(&operation,PS3_SPURS_EXCEPTION_UNOWNED_INSTANCE); return;
    }
    apply_owned(&operation,&captured_request);
}
