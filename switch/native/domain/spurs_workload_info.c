#include "spurs_workload_info.h"
#include <string.h>

enum {
    POLICY_NULL=0x80410811u,POLICY_ALIGN=0x80410810u,POLICY_INVAL=0x80410802u,POLICY_SRCH=0x80410805u,
    FLAGS_OFFSET=0x70,ENABLED_OFFSET=0xB0,SPURS2_FLAG=0x40000000u,
    PRIMARY_BYTES=0x1000,SECONDARY_BYTES=0x2000,PRIMARY_WORKLOADS=16,SECONDARY_WORKLOADS=32
};
typedef struct {
    const ps3_spurs_workload_info_config *config;
    ps3_spurs_workload_info_result *result;
} Operation;
static int stop(Operation *operation,ps3_spurs_workload_info_stop reason) {
    operation->result->stop=reason; return 0;
}
static int boundary(Operation *operation) {
    const ps3_spurs_workload_info_config *config=operation->config;
    return !config->may_continue || config->may_continue(config->poll_context)
        ? 1 : stop(operation,PS3_SPURS_WORKLOAD_INFO_CANCELLED);
}
static int memory_result(Operation *operation,ps3_guest_memory_status status) {
    if (status==PS3_GUEST_MEMORY_OK) return 1;
    operation->result->memory_status=status; return stop(operation,PS3_SPURS_WORKLOAD_INFO_MEMORY);
}
static int admit_memory(Operation *operation,uint64_t address,size_t length,unsigned writing) {
    if (!boundary(operation)) return 0;
    const ps3_spurs_workload_info_config *config=operation->config;
    if (address>UINT32_MAX || length>PS3_GUEST_ADDRESS_BYTES-address)
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    if (!writing && (address<config->instance || address-config->instance>config->owned_instance_bytes ||
        length>config->owned_instance_bytes-(address-config->instance)))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    const ps3_spurs_workload_info_memory_scope *scope=&config->memory_scope;
    if (scope->permits && !scope->permits(scope->context,(uint32_t)address,length,writing))
        return memory_result(operation,PS3_GUEST_MEMORY_INVALID_RANGE);
    return 1;
}
static int observe(Operation *operation,unsigned writing,ps3_spurs_workload_info_stage stage,
                   uint32_t address,const unsigned char *bytes,size_t length) {
    const ps3_spurs_workload_info_event event={writing?PS3_SPURS_WORKLOAD_INFO_WRITE:PS3_SPURS_WORKLOAD_INFO_READ,
        stage,address,length,bytes};
    ++operation->result->observer_calls;
    if (!operation->config->observe(operation->config->observer_context,&event)) return stop(operation,PS3_SPURS_WORKLOAD_INFO_OBSERVER);
    ++operation->result->publications; return 1;
}
static int read_value(Operation *operation,uint64_t address,size_t length,ps3_spurs_workload_info_stage stage,uint64_t *value) {
    unsigned char bytes[8];
    if (!admit_memory(operation,address,length,0) ||
        !memory_result(operation,ps3_atomic_owner_read(operation->config->domain,operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->reads;
    if (!observe(operation,0,stage,(uint32_t)address,bytes,length)) return 0;
    *value=0; for (size_t i=0;i<length;++i) *value=(*value<<8)|bytes[i];
    return 1;
}
static int write_value(Operation *operation,uint64_t address,size_t length,uint64_t value,ps3_spurs_workload_info_stage stage) {
    unsigned char bytes[8];
    for (size_t i=0;i<length;++i) bytes[length-1-i]=(unsigned char)(value>>(i*8));
    if (!admit_memory(operation,address,length,1)) return 0;
    ++operation->result->write_attempts;
    if (!memory_result(operation,ps3_atomic_owner_write(operation->config->domain,operation->config->owner,address,bytes,length))) return 0;
    ++operation->result->writes; operation->result->written_bytes+=length;
    return observe(operation,1,stage,(uint32_t)address,bytes,length);
}
static void sdk_result(Operation *operation,uint32_t value) {
    if (boundary(operation)) { operation->result->has_sdk_result=1; operation->result->sdk_result=value; }
}
int ps3_spurs_workload_info_validate_address(const ps3_spurs_workload_info_request *request,uint32_t *result) {
    if (!request || !result) return 0;
    *result=(!request->spurs || !request->info)?POLICY_NULL:(request->spurs&127u)?POLICY_ALIGN:0;
    return 1;
}
static int copy_common(Operation *operation,const ps3_spurs_workload_info_request *request) {
    const unsigned index=request->wid&15u;
    const uint64_t base=request->spurs,out=request->info;
    const uint64_t primary=base+(request->wid<PRIMARY_WORKLOADS?0xB00:0x1000)+32*index;
    const uint64_t names=base+(request->wid<PRIMARY_WORKLOADS?0xE00:0x1A00)+16*index;
    const uint64_t workload=base+(request->wid<PRIMARY_WORKLOADS?0x100:0x1200)+128*index;
    uint64_t priority,data,size,policy,instance,argument,class_name,hook;
    if (!read_value(operation,primary+0x18,8,PS3_SPURS_WORKLOAD_INFO_PRIORITY,&priority)) return 0;
    for (unsigned offset=15;offset>=8;--offset) {
        if (!write_value(operation,out+offset,1,priority&255u,PS3_SPURS_WORKLOAD_INFO_PRIORITY)) return 0;
        priority>>=8;
    }
    if (!read_value(operation,primary+8,8,PS3_SPURS_WORKLOAD_INFO_DATA,&data) ||
        !read_value(operation,primary+0x10,4,PS3_SPURS_WORKLOAD_INFO_SIZE,&size) ||
        !write_value(operation,out,8,data,PS3_SPURS_WORKLOAD_INFO_DATA) ||
        !read_value(operation,primary,8,PS3_SPURS_WORKLOAD_INFO_POLICY,&policy) ||
        !write_value(operation,out+0x14,4,size,PS3_SPURS_WORKLOAD_INFO_SIZE) ||
        !read_value(operation,names+8,8,PS3_SPURS_WORKLOAD_INFO_INSTANCE_NAME,&instance) ||
        !read_value(operation,workload+0x38,8,PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_ARGUMENT,&argument) ||
        !read_value(operation,names,8,PS3_SPURS_WORKLOAD_INFO_CLASS_NAME,&class_name) ||
        !read_value(operation,workload+0x30,8,PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_HOOK,&hook)) return 0;
    return write_value(operation,out+0x1C,4,instance,PS3_SPURS_WORKLOAD_INFO_INSTANCE_NAME) &&
        write_value(operation,out+0x10,4,policy,PS3_SPURS_WORKLOAD_INFO_POLICY) &&
        write_value(operation,out+0x18,4,class_name,PS3_SPURS_WORKLOAD_INFO_CLASS_NAME) &&
        write_value(operation,out+0x28,4,hook,PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_HOOK) &&
        write_value(operation,out+0x2C,4,argument,PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_ARGUMENT);
}
static int copy_counters(Operation *operation,const ps3_spurs_workload_info_request *request,uint64_t flags) {
    const uint64_t base=request->spurs,out=request->info;
    uint64_t value;
    if (!(flags&SPURS2_FLAG)) {
        const unsigned wid=request->wid;
        return read_value(operation,base+0x20+wid,1,PS3_SPURS_WORKLOAD_INFO_CONTENTION,&value) &&
            write_value(operation,out+0x20,1,value,PS3_SPURS_WORKLOAD_INFO_CONTENTION) &&
            read_value(operation,base+0x50+wid,1,PS3_SPURS_WORKLOAD_INFO_MAX_CONTENTION,&value) &&
            write_value(operation,out+0x22,1,value,PS3_SPURS_WORKLOAD_INFO_MAX_CONTENTION) &&
            read_value(operation,base+0x40+wid,1,PS3_SPURS_WORKLOAD_INFO_MIN_CONTENTION,&value) &&
            write_value(operation,out+0x21,1,value,PS3_SPURS_WORKLOAD_INFO_MIN_CONTENTION) &&
            read_value(operation,base+wid,1,PS3_SPURS_WORKLOAD_INFO_READY,&value) &&
            write_value(operation,out+0x23,1,value,PS3_SPURS_WORKLOAD_INFO_READY) &&
            read_value(operation,base+0x10+wid,1,PS3_SPURS_WORKLOAD_INFO_IDLE,&value) &&
            write_value(operation,out+0x24,1,value,PS3_SPURS_WORKLOAD_INFO_IDLE) &&
            read_value(operation,base+0x70,2,PS3_SPURS_WORKLOAD_INFO_SIGNAL,&value) &&
            write_value(operation,out+0x25,1,(value&(UINT32_C(0x8000)>>wid))!=0,PS3_SPURS_WORKLOAD_INFO_SIGNAL);
    }
    const unsigned index=request->wid&15u,shift=request->wid<PRIMARY_WORKLOADS?0:4;
    if (!read_value(operation,base+0x20+index,1,PS3_SPURS_WORKLOAD_INFO_CONTENTION,&value) ||
        !write_value(operation,out+0x20,1,(value>>shift)&15u,PS3_SPURS_WORKLOAD_INFO_CONTENTION) ||
        !read_value(operation,base+0x50+index,1,PS3_SPURS_WORKLOAD_INFO_MAX_CONTENTION,&value) ||
        !write_value(operation,out+0x21,1,0,PS3_SPURS_WORKLOAD_INFO_MIN_CONTENTION) ||
        !write_value(operation,out+0x22,1,(value>>shift)&15u,PS3_SPURS_WORKLOAD_INFO_MAX_CONTENTION) ||
        !read_value(operation,base+(request->wid<PRIMARY_WORKLOADS?0:0x10)+index,1,PS3_SPURS_WORKLOAD_INFO_READY,&value) ||
        !write_value(operation,out+0x24,1,0,PS3_SPURS_WORKLOAD_INFO_IDLE) ||
        !write_value(operation,out+0x23,1,value,PS3_SPURS_WORKLOAD_INFO_READY) ||
        !read_value(operation,base+(request->wid<PRIMARY_WORKLOADS?0x70:0x78),2,PS3_SPURS_WORKLOAD_INFO_SIGNAL,&value)) return 0;
    return write_value(operation,out+0x25,1,(value>>index)&1u,PS3_SPURS_WORKLOAD_INFO_SIGNAL);
}
static void apply_owned(Operation *operation,const ps3_spurs_workload_info_request *request) {
    uint64_t flags,enabled;
    if (!read_value(operation,(uint64_t)request->spurs+FLAGS_OFFSET,8,PS3_SPURS_WORKLOAD_INFO_FLAGS,&flags)) return;
    if (request->wid>=((flags&SPURS2_FLAG)?SECONDARY_WORKLOADS:PRIMARY_WORKLOADS)) { sdk_result(operation,POLICY_INVAL); return; }
    if (!read_value(operation,(uint64_t)request->spurs+ENABLED_OFFSET,4,PS3_SPURS_WORKLOAD_INFO_ENABLED,&enabled)) return;
    if (!(enabled&(UINT32_C(0x80000000)>>request->wid))) { sdk_result(operation,POLICY_SRCH); return; }
    if (copy_common(operation,request) &&
        read_value(operation,(uint64_t)request->spurs+FLAGS_OFFSET,8,PS3_SPURS_WORKLOAD_INFO_FLAGS_RECHECK,&flags) &&
        copy_counters(operation,request,flags)) sdk_result(operation,0);
}
void ps3_spurs_workload_info_apply(const ps3_spurs_workload_info_config *config,
    const ps3_spurs_workload_info_request *request,ps3_spurs_workload_info_result *result) {
    if (!result) return;
    ps3_spurs_workload_info_config captured_config;
    ps3_spurs_workload_info_request captured_request;
    if (config) captured_config=*config;
    if (request) captured_request=*request;
    memset(result,0,sizeof(*result)); Operation operation={&captured_config,result};
    if (!config || !request || !captured_config.domain || captured_config.owner.domain!=captured_config.domain ||
        !captured_config.observe ||
        (captured_config.owned_instance_bytes!=PRIMARY_BYTES && captured_config.owned_instance_bytes!=SECONDARY_BYTES)) {
        stop(&operation,PS3_SPURS_WORKLOAD_INFO_INVALID_ARGUMENT); return;
    }
    uint32_t validation;
    if (!ps3_spurs_workload_info_validate_address(&captured_request,&validation)) {
        stop(&operation,PS3_SPURS_WORKLOAD_INFO_INVALID_ARGUMENT); return;
    }
    if (validation) { sdk_result(&operation,validation); return; }
    if (!captured_config.active_owned_instance || captured_request.spurs!=captured_config.instance) {
        stop(&operation,PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE); return;
    }
    apply_owned(&operation,&captured_request);
}
