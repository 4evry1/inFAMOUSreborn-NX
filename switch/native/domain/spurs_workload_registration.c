#include "spurs_workload_registration.h"
#include <string.h>

enum { POLICY_AGAIN=0x80410801u, POLICY_INVAL=0x80410802u, POLICY_STAT=0x8041080fu,
       POLICY_ALIGN=0x80410810u, POLICY_NULL=0x80410811u,
       ENABLED=0xb0, PUBLISHED=0xb4, STATE=0x80, INFO=0xb00, NAMES=0xe00, F_RECORD=0x100,
       FLAGS=0x74, EXCEPTION=0xd6c, PRIMARY_COUNT=16, PREPARING=1, RUNNABLE=2, SPURS2=0x40 };
typedef struct {
    const ps3_spurs_registration_config *config;
    ps3_spurs_registration_result *result;
} Operation;
static uint64_t decode(const unsigned char *bytes,size_t length) {
    uint64_t value=0;for(size_t i=0;i<length;++i)value=(value<<8)|bytes[i];return value;
}
static void encode(unsigned char *bytes,size_t length,uint64_t value) {
    for(size_t i=length;i>0;--i){bytes[i-1]=(unsigned char)value;value>>=8;}
}
static int stop(Operation *op,ps3_spurs_registration_stop reason) {op->result->stop=reason;return 0;}
static int boundary(Operation *op) {
    return !op->config->may_continue || op->config->may_continue(op->config->poll_context)
        ? 1 : stop(op,PS3_SPURS_REGISTRATION_CANCELLED);
}
static int memory_status(Operation *op,ps3_guest_memory_status status) {
    if(status==PS3_GUEST_MEMORY_OK)return 1;
    op->result->memory_status=status;return stop(op,PS3_SPURS_REGISTRATION_MEMORY);
}
static int publish(Operation *op,ps3_spurs_registration_event event) {
    ++op->result->observer_calls;
    if(!op->config->observe(op->config->observer_context,&event))return stop(op,PS3_SPURS_REGISTRATION_OBSERVER);
    ++op->result->publications;return 1;
}
static int read_bytes(Operation *op,uint32_t address,unsigned char *bytes,size_t length) {
    if(!boundary(op)||!memory_status(op,ps3_atomic_read(op->config->domain,address,bytes,length)))return 0;
    ++op->result->reads;
    return publish(op,(ps3_spurs_registration_event){PS3_SPURS_REGISTRATION_READ,address,length,bytes,0,0,0});
}
static int read_value(Operation *op,uint32_t address,size_t length,uint64_t *value) {
    unsigned char bytes[8];
    if(!read_bytes(op,address,bytes,length))return 0;
    *value=decode(bytes,length);return 1;
}
static int write_bytes(Operation *op,uint32_t address,const unsigned char *bytes,size_t length,unsigned effect) {
    if(!boundary(op)||!memory_status(op,ps3_atomic_write(op->config->domain,address,bytes,length)))return 0;
    ++op->result->writes;op->result->written_bytes+=length;op->result->effects|=effect;
    return publish(op,(ps3_spurs_registration_event){PS3_SPURS_REGISTRATION_WRITE,address,length,bytes,0,0,1});
}
static int write_value(Operation *op,uint32_t address,size_t length,uint64_t value,unsigned effect) {
    unsigned char bytes[8];encode(bytes,length,value);return write_bytes(op,address,bytes,length,effect);
}
static int compare32(Operation *op,uint32_t address,uint32_t expected,uint32_t desired,unsigned effect,int *committed) {
    ps3_atomic_compare_result result;
    if(!boundary(op))return 0;
    ++op->result->atomic_attempts;
    if(!memory_status(op,ps3_atomic_compare_exchange32(op->config->domain,address,expected,desired,&result)))return 0;
    ++op->result->reads;*committed=result.exchanged;
    unsigned char bytes[4];encode(bytes,4,desired);
    if(result.exchanged){++op->result->writes;op->result->written_bytes+=4;op->result->effects|=effect;}
    return publish(op,(ps3_spurs_registration_event){PS3_SPURS_REGISTRATION_COMPARE_EXCHANGE,address,4,bytes,result.observed,desired,(unsigned)result.exchanged});
}
static unsigned leading_ones(uint32_t value) {
    unsigned count=0;while(value&0x80000000u){++count;value<<=1;}return count;
}
static int sdk_result(Operation *op,uint32_t result) {
    if(!boundary(op))return 0;
    op->result->has_sdk_result=1;op->result->sdk_result=result;return 1;
}
static int validate_published(Operation *op,uint32_t base,uint32_t enabled,uint32_t published) {
    if((published&0xffffu)||(published&~enabled))return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
    for(unsigned wid=0;wid<PRIMARY_COUNT;++wid)if(published&(0x80000000u>>wid)) {
        uint64_t state,identity,address;
        if(!read_value(op,base+STATE+wid,1,&state)||!read_value(op,base+INFO+wid*0x20+0x14,1,&identity)
            ||!read_value(op,base+INFO+wid*0x20,8,&address))return 0;
        if(state<2||state>4||identity>31||!address)return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
    }
    return 1;
}
static int claim(Operation *op,uint32_t base) {
    for(unsigned attempt=0;attempt<op->config->max_attempts;++attempt) {
        uint64_t enabled,published,current,pending;
        if(!read_value(op,base+ENABLED,4,&enabled)||!read_value(op,base+PUBLISHED,4,&published))return 0;
        unsigned wid=leading_ones((uint32_t)enabled);op->result->wid=wid;
        if(wid>=PRIMARY_COUNT)return 1; /* Full mask still has a required wid output effect. */
        if(!validate_published(op,base,(uint32_t)enabled,(uint32_t)published)
            ||!read_value(op,base+0x20+wid,1,&current)||!read_value(op,base+0x30+wid,1,&pending))return 0;
        if((current|pending)&0xf)return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
        int committed=0;
        if(!compare32(op,base+ENABLED,(uint32_t)enabled,(uint32_t)enabled|(0x80000000u>>wid),PS3_SPURS_REGISTRATION_SLOT_CLAIMED,&committed))return 0;
        if(committed)return 1;
    }
    return stop(op,PS3_SPURS_REGISTRATION_ATTEMPTS);
}
static int update_bits(Operation *op,uint32_t address,uint32_t clear,uint32_t set,int receiver) {
    for(unsigned attempt=0;attempt<op->config->max_attempts;++attempt) {
        uint64_t value;if(!read_value(op,address,4,&value))return 0;
        if(receiver && ((uint32_t)value&0xffu)!=op->result->wid)return 1;
        uint32_t desired=((uint32_t)value&~clear)|set;int committed=0;
        if(!compare32(op,address,(uint32_t)value,desired,0,&committed))return 0;
        if(committed)return 1;
    }
    return stop(op,PS3_SPURS_REGISTRATION_ATTEMPTS);
}
static int initialize_record(Operation *op,const ps3_spurs_registration_request *request) {
    uint32_t base=request->spurs,wid=op->result->wid,info=base+INFO+wid*0x20,record=base+F_RECORD+wid*0x80;
    unsigned char zeroes[0x58]={0};
    if(!write_value(op,base+STATE+wid,1,PREPARING,PS3_SPURS_REGISTRATION_PREPARING)
        ||!write_value(op,base+0x90+wid,1,0,0)||!write_value(op,base+0xa0+wid,1,0,0)
        ||!write_value(op,info,8,request->policy_module,0)||!write_value(op,info+8,8,request->data,0)
        ||!write_value(op,info+0x10,4,request->size,0))return 0;
    for(unsigned i=0;i<8;++i)if(!write_value(op,info+0x18+i,1,request->priority[i],0))return 0;
    if(!write_value(op,base+NAMES+wid*0x10,8,request->name_class,0)
        ||!write_value(op,base+NAMES+wid*0x10+8,8,request->name_instance,0)
        ||!write_bytes(op,record,zeroes,0x20,0)||!write_bytes(op,record+0x28,zeroes,0x58,0))return 0;
    if(request->hook && (!write_value(op,record+0x30,8,request->hook,0)
        ||!write_value(op,record+0x38,8,request->hook_argument,0)||!write_value(op,base+0xa0+wid,1,2,0)))return 0;
    if(!write_value(op,base+0x10+wid,1,0,0)
        ||!write_value(op,base+0x40+wid,1,request->min_contention>8?8:request->min_contention,0)
        ||!write_value(op,base+wid,1,0,0))return 0;
    unsigned shift=(3-(wid&3))*8;
    if(!update_bits(op,base+0x50+(wid&~3u),0xfu<<shift,(request->max_contention>8?8:request->max_contention)<<shift,0)
        ||!update_bits(op,base+0x70,(0x8000u>>wid)<<16,0,0)
        ||!update_bits(op,base+0x74,0xffu,0xffu,1))return 0;
    return 1;
}
static int publish_module(Operation *op,const ps3_spurs_registration_request *request) {
    uint32_t base=request->spurs,wid=op->result->wid;uint64_t published;
    if(!read_value(op,base+PUBLISHED,4,&published))return 0;
    uint32_t mask=(uint32_t)published&~(0x80000000u>>wid),used=0;unsigned identity=0;
    for(unsigned i=0;i<PRIMARY_COUNT;++i)if(mask&(0x80000000u>>i)) {
        uint64_t address,current;
        if(!read_value(op,base+INFO+i*0x20,8,&address)||!read_value(op,base+INFO+i*0x20+0x14,1,&current))return 0;
        if(current>31)return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
        if(address==request->policy_module){identity=(unsigned)current;goto identity_ready;}
        used|=0x80000000u>>(unsigned)current;identity=leading_ones(used);
    }
identity_ready:
    if(identity>31)return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
    op->result->unique_id=identity;
    if(!write_value(op,base+INFO+wid*0x20+0x14,1,identity,PS3_SPURS_REGISTRATION_UNIQUE_ID_WRITTEN))return 0;
    for(unsigned attempt=0;attempt<op->config->max_attempts;++attempt) {
        unsigned char line[128];int stored=0;
        if(!boundary(op)||!memory_status(op,ps3_atomic_load_reserve128(op->config->domain,op->config->owner,base+0x80,line)))return 0;
        ++op->result->reads;
        if(!publish(op,(ps3_spurs_registration_event){PS3_SPURS_REGISTRATION_READ,base+0x80,128,line,0,0,0}))return 0;
        uint32_t current_mask=(uint32_t)decode(line+0x34,4);
        if(current_mask!=(uint32_t)published || line[wid]!=PREPARING)return stop(op,PS3_SPURS_REGISTRATION_CORRUPT_STATE);
        encode(line+0x34,4,mask|(0x80000000u>>wid));line[wid]=RUNNABLE;
        if(!boundary(op))return 0;
        ++op->result->atomic_attempts;
        if(!memory_status(op,ps3_atomic_store_conditional128(op->config->domain,op->config->owner,base+0x80,line,&stored)))return 0;
        if(stored){++op->result->writes;op->result->written_bytes+=128;op->result->effects|=PS3_SPURS_REGISTRATION_RUNNABLE_PUBLISHED;}
        if(!publish(op,(ps3_spurs_registration_event){PS3_SPURS_REGISTRATION_CONDITIONAL_LINE,base+0x80,128,line,0,0,(unsigned)stored}))return 0;
        if(stored)return 1;
    }
    return stop(op,PS3_SPURS_REGISTRATION_ATTEMPTS);
}
static void apply_locked(Operation *op,const ps3_spurs_registration_request *request) {
    uint32_t base=request->spurs;uint64_t exception,flags;
    if(!boundary(op)||!read_value(op,base+EXCEPTION,4,&exception))return;
    if(exception){sdk_result(op,POLICY_STAT);return;}
    if(!read_value(op,base+FLAGS,1,&flags))return;
    if(flags&SPURS2){stop(op,PS3_SPURS_REGISTRATION_UNSUPPORTED_VARIANT);return;}
    if(!claim(op,base)||!write_value(op,request->wid_output,4,op->result->wid,PS3_SPURS_REGISTRATION_WID_WRITTEN))return;
    if(op->result->wid>=PRIMARY_COUNT){sdk_result(op,POLICY_AGAIN);return;}
    if(!initialize_record(op,request)||!publish_module(op,request)
        ||!write_value(op,base+0xbd,1,0xff,PS3_SPURS_REGISTRATION_UPDATE_MESSAGE)
        ||!write_value(op,base+0x72,1,0xff,PS3_SPURS_REGISTRATION_SERVICE_MESSAGE))return;
    sdk_result(op,0);
}
int ps3_spurs_workload_registration_validate_request(const ps3_spurs_registration_request *request,uint32_t *result) {
    if(!request||!result)return 0;
    uint32_t code=0;
    if(!request->spurs||!request->wid_output||!request->policy_module)code=POLICY_NULL;
    else if((request->spurs&127)||(request->policy_module&15))code=POLICY_ALIGN;
    else {
        unsigned invalid=request->min_contention==0;
        for(unsigned i=0;i<8;++i)invalid|=request->priority[i]&0xf0;
        if(invalid)code=POLICY_INVAL;
    }
    *result=code;return 1;
}

void ps3_spurs_workload_registration_apply(const ps3_spurs_registration_config *config,
    const ps3_spurs_registration_request *request,ps3_spurs_registration_result *result) {
    if(!result)return;
    ps3_spurs_registration_request captured;
    if(request)captured=*request;
    memset(result,0,sizeof *result);
    Operation op={config,result};
    if(!config||!request||!config->domain||!config->registration_guard.lock||!config->registration_guard.unlock
        ||!config->observe||!config->max_attempts||config->max_attempts>1024||config->owner.domain!=config->domain
        ||(config->registration_guard.context==config->domain->guard.context)){
        stop(&op,PS3_SPURS_REGISTRATION_INVALID_ARGUMENT);return;
    }
    uint32_t validation;
    if(!ps3_spurs_workload_registration_validate_request(&captured,&validation)){
        stop(&op,PS3_SPURS_REGISTRATION_INVALID_ARGUMENT);return;
    }
    if(validation){sdk_result(&op,validation);return;}
    if(!config->active_owned_instance||captured.spurs!=config->instance){stop(&op,PS3_SPURS_REGISTRATION_UNOWNED_INSTANCE);return;}
    if((uint64_t)captured.spurs+PS3_SPURS_REGISTRATION_PRIMARY_BYTES>0x100000000ull
        ||(uint64_t)captured.wid_output+4>0x100000000ull){stop(&op,PS3_SPURS_REGISTRATION_INVALID_ARGUMENT);return;}
    if(captured.wid_output<(uint64_t)captured.spurs+PS3_SPURS_REGISTRATION_PRIMARY_BYTES
        &&(uint64_t)captured.wid_output+4>captured.spurs){stop(&op,PS3_SPURS_REGISTRATION_UNSUPPORTED_ALIAS);return;}
    config->registration_guard.lock(config->registration_guard.context);
    if(memory_status(&op,ps3_atomic_owner_clear_reservation(config->domain,config->owner)))
        apply_locked(&op,&captured);
    /* An interrupted line LL must not leave an owned reservation behind. */
    ps3_guest_memory_status cleared=ps3_atomic_owner_clear_reservation(config->domain,config->owner);
    if(cleared!=PS3_GUEST_MEMORY_OK&&result->stop==PS3_SPURS_REGISTRATION_RUNNING){
        result->has_sdk_result=0;memory_status(&op,cleared);
    }
    config->registration_guard.unlock(config->registration_guard.context);
}
