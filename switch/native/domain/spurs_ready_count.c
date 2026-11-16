#include "spurs_ready_count.h"
#include <stdatomic.h>
#include <string.h>

enum {
    POLICY_NULL=0x80410811u,POLICY_ALIGN=0x80410810u,POLICY_INVAL=0x80410802u,
    POLICY_SRCH=0x80410805u,POLICY_STAT=0x8041080Fu,
    FLAGS_OFFSET=0x74,ENABLED_OFFSET=0xB0,EXCEPTION_OFFSET=0xD6C,
    PRIMARY_STATE_OFFSET=0x80,SECONDARY_STATE_OFFSET=0xD0,
    PRIMARY_WORKLOADS=16,SECONDARY_WORKLOADS=32,SPURS2_FLAG=0x40,
    RUNNABLE_STATE=2,PRIMARY_BYTES=0x1000,SECONDARY_BYTES=0x2000
};
typedef struct {
    const ps3_spurs_ready_count_config *config;
    ps3_spurs_ready_count_result *result;
} Operation;
static int stop(Operation *operation,ps3_spurs_ready_count_stop reason) {
    operation->result->stop=reason; return 0;
}
static int boundary(Operation *operation) {
    const ps3_spurs_ready_count_config *config=operation->config;
    return !config->may_continue || config->may_continue(config->poll_context)
        ? 1 : stop(operation,PS3_SPURS_READY_COUNT_CANCELLED);
}
static int memory_result(Operation *operation,ps3_guest_memory_status status) {
    if (status==PS3_GUEST_MEMORY_OK) return 1;
    operation->result->memory_status=status;
    return stop(operation,PS3_SPURS_READY_COUNT_MEMORY);
}
static int admit_memory(Operation *operation,uint64_t address,size_t length,unsigned write) {
    if (!boundary(operation)) return 0;
    const ps3_spurs_ready_count_config *config=operation->config;
    if (address>UINT32_MAX || length>PS3_GUEST_ADDRESS_BYTES-address || address<config->instance ||
        address-config->instance>config->owned_instance_bytes ||
        length>config->owned_instance_bytes-(address-config->instance))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    const ps3_spurs_ready_count_memory_scope *scope=&config->memory_scope;
    if (scope->permits && !scope->permits(scope->context,(uint32_t)address,length,write))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    return 1;
}
static int observe(Operation *operation,ps3_spurs_ready_count_operation kind,ps3_spurs_ready_count_stage stage,
                   uint32_t address,const unsigned char *bytes,size_t length) {
    const ps3_spurs_ready_count_event event={kind,stage,address,length,bytes};
    ++operation->result->observer_calls;
    if (!operation->config->observe(operation->config->observer_context,&event))
        return stop(operation,PS3_SPURS_READY_COUNT_OBSERVER);
    ++operation->result->publications; return 1;
}
static int read_value(Operation *operation,uint64_t address,size_t length,ps3_spurs_ready_count_stage stage,uint32_t *value) {
    unsigned char bytes[4];
    if (!admit_memory(operation,address,length,0) ||
        !memory_result(operation,ps3_atomic_owner_read(operation->config->domain,operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->reads;
    if (!observe(operation,PS3_SPURS_READY_COUNT_READ,stage,(uint32_t)address,bytes,length)) return 0;
    *value=0;
    for (size_t i=0;i<length;++i) *value=(*value<<8)|bytes[i];
    return 1;
}
static void sdk_result(Operation *operation,uint32_t value) {
    if (boundary(operation)) { operation->result->has_sdk_result=1; operation->result->sdk_result=value; }
}
int ps3_spurs_ready_count_validate_address(const ps3_spurs_ready_count_request *request,uint32_t *result) {
    if (!request || !result) return 0;
    *result=!request->spurs ? POLICY_NULL : (request->spurs&127u) ? POLICY_ALIGN : 0;
    return 1;
}
static int publish_ready_count(Operation *operation,uint32_t address,unsigned char value) {
    if (!admit_memory(operation,address,1,1)) return 0;
    ps3_spurs_ready_count_result *result=operation->result;
    result->store_attempted=1;
    /* Pair prior guest job/queue writes with the reference's release byte store.
     * The coherent port serializes the byte write and invalidates reservations;
     * it alone owns notification of the resulting reservation loss. */
    atomic_thread_fence(memory_order_release);
    ++result->release_fences;
    if (!memory_result(operation,ps3_atomic_owner_write(operation->config->domain,operation->config->owner,address,&value,1))) return 0;
    result->stored=1; ++result->writes; ++result->written_bytes;
    return observe(operation,PS3_SPURS_READY_COUNT_WRITE,PS3_SPURS_READY_COUNT_STORE,address,&value,1);
}
static void apply_owned(Operation *operation,const ps3_spurs_ready_count_request *request) {
    uint32_t flags,enabled,exception,state;
    const uint64_t base=request->spurs;
    if (!read_value(operation,base+FLAGS_OFFSET,1,PS3_SPURS_READY_COUNT_FLAGS,&flags)) return;
    const unsigned limit=(flags&SPURS2_FLAG)?SECONDARY_WORKLOADS:PRIMARY_WORKLOADS;
    if (request->wid>=limit || request->value>UINT8_MAX) { sdk_result(operation,POLICY_INVAL); return; }
    if (!read_value(operation,base+ENABLED_OFFSET,4,PS3_SPURS_READY_COUNT_ENABLED,&enabled)) return;
    if (!(enabled&(UINT32_C(0x80000000)>>request->wid))) { sdk_result(operation,POLICY_SRCH); return; }
    if (!read_value(operation,base+EXCEPTION_OFFSET,4,PS3_SPURS_READY_COUNT_EXCEPTION,&exception)) return;
    if (exception) { sdk_result(operation,POLICY_STAT); return; }
    const uint64_t state_address=base+(request->wid<PRIMARY_WORKLOADS
        ? PRIMARY_STATE_OFFSET+request->wid : SECONDARY_STATE_OFFSET+(request->wid&15u));
    if (!read_value(operation,state_address,1,PS3_SPURS_READY_COUNT_STATE,&state)) return;
    if (state!=RUNNABLE_STATE) { sdk_result(operation,POLICY_STAT); return; }
    /* Both reference ready-count arrays are contiguous at offsets 0..31. */
    operation->result->ready_address=(uint32_t)(base+request->wid);
    if (publish_ready_count(operation,operation->result->ready_address,(unsigned char)request->value)) sdk_result(operation,0);
}
void ps3_spurs_ready_count_apply(const ps3_spurs_ready_count_config *config,
    const ps3_spurs_ready_count_request *request,ps3_spurs_ready_count_result *result) {
    if (!result) return;
    ps3_spurs_ready_count_config captured_config;
    ps3_spurs_ready_count_request captured_request;
    if (config) captured_config=*config;
    if (request) captured_request=*request;
    memset(result,0,sizeof(*result)); Operation operation={&captured_config,result};
    if (!config || !request || !captured_config.domain || captured_config.owner.domain!=captured_config.domain ||
        !captured_config.observe ||
        (captured_config.owned_instance_bytes!=PRIMARY_BYTES && captured_config.owned_instance_bytes!=SECONDARY_BYTES)) {
        stop(&operation,PS3_SPURS_READY_COUNT_INVALID_ARGUMENT); return;
    }
    uint32_t validation;
    if (!ps3_spurs_ready_count_validate_address(&captured_request,&validation)) {
        stop(&operation,PS3_SPURS_READY_COUNT_INVALID_ARGUMENT); return;
    }
    if (validation) { sdk_result(&operation,validation); return; }
    if (!captured_config.active_owned_instance || captured_request.spurs!=captured_config.instance) {
        stop(&operation,PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE); return;
    }
    apply_owned(&operation,&captured_request);
}

/* ---- cellSpursCreateTask's signal path: 0xA750 (mask raise) then 0x85D0 (signal byte) -------
 * Everything below is additive:
 * the functions above are untouched, and this code shares only `Observe`-style plumbing that is
 * re-expressed here so no existing symbol changes shape. */
enum { MASK_PRIMARY_OFFSET=0x70, MASK_SECONDARY_OFFSET=0x78, MASK_PRIMARY_LIMIT=16,
       MASK_SECONDARY_LIMIT=32, MASK_ENABLED_OFFSET=0xB0, MASK_EXCEPTION_OFFSET=0xD6C,
       MASK_SIGNAL_OFFSET=0xD64, MASK_POLICY_NULL=0x80410811u, MASK_POLICY_ALIGN=0x80410810u,
       MASK_POLICY_INVAL=0x80410802u, MASK_POLICY_SRCH=0x80410805u,
       MASK_POLICY_STAT=0x8041080Fu };

typedef struct {
    const ps3_spurs_ready_count_config *config;
    ps3_spurs_ready_mask_result *result;
} MaskOperation;

static int mask_stop(MaskOperation *operation,ps3_spurs_ready_count_stop reason) {
    operation->result->stop=reason; return 0;
}
static int mask_boundary(MaskOperation *operation) {
    const ps3_spurs_ready_count_config *config=operation->config;
    return !config->may_continue || config->may_continue(config->poll_context)
        ? 1 : mask_stop(operation,PS3_SPURS_READY_COUNT_CANCELLED);
}
static int mask_admit(MaskOperation *operation,uint64_t address,size_t length,unsigned write) {
    if (!mask_boundary(operation)) return 0;
    const ps3_spurs_ready_count_config *config=operation->config;
    if (address>UINT32_MAX || length>PS3_GUEST_ADDRESS_BYTES-address || address<config->instance ||
        address-config->instance>config->owned_instance_bytes ||
        length>config->owned_instance_bytes-(address-config->instance)) {
        operation->result->memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
        return mask_stop(operation,PS3_SPURS_READY_COUNT_MEMORY);
    }
    const ps3_spurs_ready_count_memory_scope *scope=&config->memory_scope;
    if (scope->permits && !scope->permits(scope->context,(uint32_t)address,length,write)) {
        operation->result->memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
        return mask_stop(operation,PS3_SPURS_READY_COUNT_MEMORY);
    }
    return 1;
}
static int mask_observe(MaskOperation *operation,ps3_spurs_ready_count_operation kind,
                        ps3_spurs_ready_count_stage stage,uint32_t address,
                        const unsigned char *bytes,size_t length) {
    const ps3_spurs_ready_count_event event={kind,stage,address,length,bytes};
    ++operation->result->observer_calls;
    if (!operation->config->observe(operation->config->observer_context,&event))
        return mask_stop(operation,PS3_SPURS_READY_COUNT_OBSERVER);
    ++operation->result->publications; return 1;
}
static int mask_memory(MaskOperation *operation,ps3_guest_memory_status status) {
    if (status==PS3_GUEST_MEMORY_OK) return 1;
    operation->result->memory_status=status;
    return mask_stop(operation,PS3_SPURS_READY_COUNT_MEMORY);
}
static int mask_read(MaskOperation *operation,uint32_t address,size_t length,
                     ps3_spurs_ready_count_stage stage,uint32_t *value) {
    unsigned char bytes[4];
    if (length>sizeof bytes) return mask_stop(operation,PS3_SPURS_READY_COUNT_INVALID_ARGUMENT);
    if (!mask_admit(operation,address,length,0) ||
        !mask_memory(operation,ps3_atomic_owner_read(operation->config->domain,
            operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->reads;
    if (!mask_observe(operation,PS3_SPURS_READY_COUNT_READ,stage,address,bytes,length)) return 0;
    *value=0; for (size_t i=0;i<length;++i) *value=(*value<<8)|bytes[i];
    return 1;
}
static int mask_write(MaskOperation *operation,uint32_t address,const unsigned char *bytes,
                      size_t length,ps3_spurs_ready_count_stage stage) {
    if (!mask_admit(operation,address,length,1) ||
        !mask_memory(operation,ps3_atomic_owner_write(operation->config->domain,
            operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->writes; operation->result->written_bytes+=length;
    return mask_observe(operation,PS3_SPURS_READY_COUNT_WRITE,stage,address,bytes,length);
}
static void mask_sdk_result(MaskOperation *operation,uint32_t value) {
    if (mask_boundary(operation)) {
        operation->result->has_sdk_result=1; operation->result->sdk_result=value;
    }
}

int ps3_spurs_ready_mask_validate_address(const ps3_spurs_ready_mask_request *request,uint32_t *result) {
    if (!request || !result) return 0;
    *result=!request->spurs ? MASK_POLICY_NULL : (request->spurs&127u) ? MASK_POLICY_ALIGN : 0;
    return 1;
}

static int mask_raise(MaskOperation *operation,const ps3_spurs_ready_mask_request *request) {
    const uint32_t base=request->spurs;
    uint32_t flags,enabled,exception,current;
    if (!mask_read(operation,base+FLAGS_OFFSET,1,PS3_SPURS_READY_COUNT_FLAGS,&flags)) return 0;
    const unsigned limit=(flags&SPURS2_FLAG)?MASK_SECONDARY_LIMIT:MASK_PRIMARY_LIMIT;
    if (request->wid>=limit || request->mask_bit==0u) { mask_sdk_result(operation,MASK_POLICY_INVAL); return 0; }
    if (!mask_read(operation,base+MASK_ENABLED_OFFSET,4,PS3_SPURS_READY_COUNT_ENABLED,&enabled)) return 0;
    if (!(enabled&(UINT32_C(0x80000000)>>request->wid))) { mask_sdk_result(operation,MASK_POLICY_SRCH); return 0; }
    if (!mask_read(operation,base+MASK_EXCEPTION_OFFSET,4,PS3_SPURS_READY_COUNT_EXCEPTION,&exception)) return 0;
    if (exception) { mask_sdk_result(operation,MASK_POLICY_STAT); return 0; }
    const uint32_t address=base+(request->wid<MASK_PRIMARY_LIMIT?MASK_PRIMARY_OFFSET:MASK_SECONDARY_OFFSET);
    if (!mask_read(operation,address,4,PS3_SPURS_READY_COUNT_STATE,&current)) return 0;
    operation->result->mask_address=address;
    unsigned char word[4];
    const uint32_t desired=current|request->mask_bit;
    for (size_t i=0;i<4;++i) word[i]=(unsigned char)(desired>>(8u*(3u-i)));
    if (mask_write(operation,address,word,4,PS3_SPURS_READY_COUNT_STATE)) operation->result->mask_written=1;
    return 1;
}
static void mask_apply_owned(MaskOperation *operation,const ps3_spurs_ready_mask_request *request) {
    if (!mask_raise(operation,request)) return;
    /* 0x85D0: the signal byte, written only when the exception word read zero. mask_raise already
     * proved it, so the store is unconditional here -- exactly as the signal store is reached only past
     * the exception-word branch. */
    const unsigned char signal=1u;
    const uint32_t address=request->spurs+MASK_SIGNAL_OFFSET;
    if (!mask_write(operation,address,&signal,1,PS3_SPURS_READY_COUNT_STORE)) return;
    operation->result->signal_written=1;
    mask_sdk_result(operation,0);
}

void ps3_spurs_ready_mask_apply(const ps3_spurs_ready_count_config *config,
    const ps3_spurs_ready_mask_request *request,ps3_spurs_ready_mask_result *result) {
    if (!result) return;
    ps3_spurs_ready_count_config captured_config;
    ps3_spurs_ready_mask_request captured_request;
    if (config) captured_config=*config;
    if (request) captured_request=*request;
    memset(result,0,sizeof(*result));
    MaskOperation operation={&captured_config,result};
    if (!config || !request || !captured_config.domain ||
        captured_config.owner.domain!=captured_config.domain || !captured_config.observe ||
        captured_config.owned_instance_bytes!=PRIMARY_BYTES) {
        mask_stop(&operation,PS3_SPURS_READY_COUNT_INVALID_ARGUMENT); return;
    }
    uint32_t validation;
    if (!ps3_spurs_ready_mask_validate_address(&captured_request,&validation)) {
        mask_stop(&operation,PS3_SPURS_READY_COUNT_INVALID_ARGUMENT); return;
    }
    if (validation) { mask_sdk_result(&operation,validation); return; }
    if (!captured_config.active_owned_instance || captured_request.spurs!=captured_config.instance) {
        mask_stop(&operation,PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE); return;
    }
    mask_apply_owned(&operation,&captured_request);
}
