#include "spurs_idle_request.h"
#include <string.h>

enum { CORE_INVAL=0x80410702u, CORE_SRCH=0x80410705u, CORE_STAT=0x8041070fu,
       CORE_ALIGN=0x80410710u, CORE_NULL=0x80410711u,
       FLAGS=0x74, ENABLED=0xb0, EXCEPTION=0xd6c, IDLE=0x10,
       SPURS2=0x40, WORKLOADS=16, SPUS=8, INSTANCE_BYTES=0x1000 };
typedef struct { const ps3_spurs_idle_config *config; ps3_spurs_idle_result *result; } Operation;
static int stop(Operation *op,ps3_spurs_idle_stop reason) {op->result->stop=reason;return 0;}
static int boundary(Operation *op) {
    return !op->config->may_continue || op->config->may_continue(op->config->poll_context)
        ? 1 : stop(op,PS3_SPURS_IDLE_CANCELLED);
}
static int memory_result(Operation *op,ps3_guest_memory_status status) {
    if(status==PS3_GUEST_MEMORY_OK)return 1;
    op->result->memory_status=status;return stop(op,PS3_SPURS_IDLE_MEMORY);
}
static int observe(Operation *op,ps3_spurs_idle_operation operation,uint32_t address,
                   const unsigned char *bytes,size_t length) {
    ps3_spurs_idle_event event={operation,address,length,bytes};
    ++op->result->observer_calls;
    if(!op->config->observe(op->config->observer_context,&event))return stop(op,PS3_SPURS_IDLE_OBSERVER);
    ++op->result->publications;return 1;
}
static int admit_memory(Operation *op,uint32_t address,size_t length,unsigned write) {
    if(!boundary(op))return 0;
    const ps3_spurs_idle_memory_scope *scope=&op->config->memory_scope;
    if(scope->permits&&!scope->permits(scope->context,address,length,write)){
        op->result->memory_status=PS3_GUEST_MEMORY_INVALID_RANGE;
        return stop(op,PS3_SPURS_IDLE_MEMORY);
    }
    return 1;
}
static int read_value(Operation *op,uint32_t address,size_t length,uint32_t *value) {
    unsigned char bytes[4];
    if(!admit_memory(op,address,length,0)||!memory_result(op,ps3_atomic_read(op->config->domain,address,bytes,length)))return 0;
    ++op->result->reads;
    if(!observe(op,PS3_SPURS_IDLE_READ,address,bytes,length))return 0;
    *value=0;for(size_t i=0;i<length;++i)*value=(*value<<8)|bytes[i];return 1;
}
static void sdk_result(Operation *op,uint32_t value) {
    if(boundary(op)){op->result->has_sdk_result=1;op->result->sdk_result=value;}
}
int ps3_spurs_idle_request_validate_address(const ps3_spurs_idle_request *request,uint32_t *result) {
    if(!request||!result)return 0;
    *result=!request->spurs?CORE_NULL:(request->spurs&127)?CORE_ALIGN:0;
    return 1;
}
static void apply_locked(Operation *op,const ps3_spurs_idle_request *request) {
    uint32_t flags,enabled,exception;
    if(!read_value(op,request->spurs+FLAGS,1,&flags))return;
    if(flags&SPURS2){sdk_result(op,CORE_STAT);return;}
    if(request->wid>=WORKLOADS||request->count>=SPUS){sdk_result(op,CORE_INVAL);return;}
    if(!read_value(op,request->spurs+ENABLED,4,&enabled))return;
    if(!(enabled&(0x80000000u>>request->wid))){sdk_result(op,CORE_SRCH);return;}
    if(!read_value(op,request->spurs+EXCEPTION,4,&exception))return;
    if(exception){sdk_result(op,CORE_STAT);return;}
    const uint32_t address=request->spurs+IDLE+request->wid;
    const unsigned char count=(unsigned char)request->count;
    if(!admit_memory(op,address,1,1))return;
    op->result->idle_byte_write_attempted=1;
    if(!memory_result(op,ps3_atomic_write(op->config->domain,address,&count,1)))return;
    ++op->result->writes;++op->result->written_bytes;op->result->idle_byte_written=1;
    if(!observe(op,PS3_SPURS_IDLE_WRITE,address,&count,1))return;
    sdk_result(op,0);
}
void ps3_spurs_idle_request_apply(const ps3_spurs_idle_config *config,
    const ps3_spurs_idle_request *request,ps3_spurs_idle_result *result) {
    if(!result)return;
    ps3_spurs_idle_request captured;
    if(request)captured=*request;
    memset(result,0,sizeof *result);
    Operation op={config,result};
    if(!config||!request||!config->domain||!config->observe||!config->workload_guard.lock
        ||!config->workload_guard.unlock||config->workload_guard.context==config->domain->guard.context){
        stop(&op,PS3_SPURS_IDLE_INVALID_ARGUMENT);return;
    }
    uint32_t validation;
    if(!ps3_spurs_idle_request_validate_address(&captured,&validation)){
        stop(&op,PS3_SPURS_IDLE_INVALID_ARGUMENT);return;
    }
    if(validation){sdk_result(&op,validation);return;}
    if(!config->active_owned_instance||captured.spurs!=config->instance){stop(&op,PS3_SPURS_IDLE_UNOWNED_INSTANCE);return;}
    if((uint64_t)captured.spurs+INSTANCE_BYTES>UINT64_C(0x100000000)){
        stop(&op,PS3_SPURS_IDLE_INVALID_ARGUMENT);return;
    }
    config->workload_guard.lock(config->workload_guard.context);
    apply_locked(&op,&captured);
    config->workload_guard.unlock(config->workload_guard.context);
}
