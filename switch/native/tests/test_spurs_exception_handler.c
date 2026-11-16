#define _POSIX_C_SOURCE 200809L
#include "../domain/spurs_exception_handler.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { RAM_BYTES=0x4000, BASE=0x1000, PRIMARY_SLOT=BASE+0x100,
       POLICY_NULL=0x80410811u, POLICY_ALIGN=0x80410810u, POLICY_INVAL=0x80410802u,
       POLICY_SRCH=0x80410805u, POLICY_STAT=0x8041080Fu, POLICY_BUSY=0x8041080Au };
typedef struct {
    unsigned char ram[RAM_BYTES];
    ps3_pthread_guard guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[4];
    ps3_atomic_owner owners[3];
    size_t reads,writes,fail_read,fail_write;
} Fixture;
typedef struct {
    ps3_spurs_exception_operation operation;
    ps3_spurs_exception_stage stage;
    uint32_t address;
    size_t length;
    unsigned char bytes[8];
} Event;
typedef struct {
    Fixture *fixture;
    ps3_spurs_exception_config config;
    ps3_spurs_exception_request request;
    ps3_spurs_exception_result result;
    Event events[64];
    size_t count,scope_calls;
    atomic_uint cancelled;
    ps3_spurs_exception_stage reject,cancel_after,nested_unset;
    unsigned interference,reserve_reads,nested_cleared;
    uint32_t denied_address;
    int denied_write;
    pthread_barrier_t *barrier;
} Call;
static int range(uint32_t address,uint64_t bytes) { return address<=RAM_BYTES && bytes<=RAM_BYTES-address; }
static ps3_guest_memory_status map_memory(void *context,uint32_t address,uint64_t bytes) {
    (void)context; return range(address,bytes)?PS3_GUEST_MEMORY_OK:PS3_GUEST_MEMORY_INVALID_RANGE;
}
static ps3_guest_memory_status read_memory(void *context,uint32_t address,void *out,size_t bytes) {
    Fixture *f=context;
    if (++f->reads==f->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memcpy(out,f->ram+address,bytes); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status write_memory(void *context,uint32_t address,const void *in,size_t bytes) {
    Fixture *f=context;
    if (++f->writes==f->fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memcpy(f->ram+address,in,bytes); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status zero_memory(void *context,uint32_t address,uint64_t bytes) {
    Fixture *f=context;
    if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memset(f->ram+address,0,(size_t)bytes); return PS3_GUEST_MEMORY_OK;
}
static void put(Fixture *f,uint32_t address,size_t bytes,uint64_t value) {
    for (size_t i=0;i<bytes;++i) f->ram[address+bytes-i-1]=(unsigned char)(value>>(i*8));
}
static uint64_t get(const Fixture *f,uint32_t address,size_t bytes) {
    uint64_t value=0;
    for (size_t i=0;i<bytes;++i) value=(value<<8)|f->ram[address+i];
    return value;
}
static void initialize(Fixture *f) {
    memset(f,0,sizeof(*f)); assert(!ps3_pthread_guard_init(&f->guard));
    ps3_guest_memory memory={f,map_memory,map_memory,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->guard),f->records,4,NULL,0)==PS3_GUEST_MEMORY_OK);
    for (unsigned i=0;i<3;++i) assert(ps3_atomic_owner_acquire(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
    put(f,BASE+0xB0,4,UINT32_MAX);
    put(f,PRIMARY_SLOT+8,8,UINT64_C(0xDEADBEEF11223344));
}
static void finish(Fixture *f) {
    for (unsigned i=0;i<3;++i) {
        assert(ps3_atomic_owner_read(&f->domain,f->owners[i],0,NULL,0)==PS3_GUEST_MEMORY_OK);
        assert(ps3_atomic_owner_release(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
    }
    assert(!ps3_pthread_guard_destroy(&f->guard));
}
static int may_continue(void *context) { return !atomic_load(&((Call *)context)->cancelled); }
static int permits(void *context,uint32_t address,size_t bytes,unsigned write) {
    Call *c=context; (void)bytes; ++c->scope_calls;
    return address!=c->denied_address || (int)write!=c->denied_write;
}
static void prepare(Call *,Fixture *,unsigned);
static int observe(void *context,const ps3_spurs_exception_event *event) {
    Call *c=context; assert(c->count<64 && event->length<=8 && event->bytes);
    Event *copy=&c->events[c->count++];
    *copy=(Event){event->operation,event->stage,event->address,event->length,{0}};
    memcpy(copy->bytes,event->bytes,event->length);
    if (event->stage==PS3_SPURS_EXCEPTION_LOAD_RESERVE) {
        ++c->reserve_reads;
        if (c->barrier && c->reserve_reads==1) {
            int status=pthread_barrier_wait(c->barrier); assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
        }
        if (c->interference && (c->interference==2 || c->reserve_reads==1)) {
            const unsigned char byte=1;
            assert(ps3_atomic_write(&c->fixture->domain,event->address+64,&byte,1)==PS3_GUEST_MEMORY_OK);
        }
    }
    if (c->nested_unset==event->stage) {
        Call nested; prepare(&nested,c->fixture,2);
        nested.request.action=PS3_SPURS_EXCEPTION_UNSET; nested.request.wid=c->request.wid;
        ps3_spurs_exception_handler_apply(&nested.config,&nested.request,&nested.result);
        assert(nested.result.has_sdk_result && !nested.result.sdk_result && nested.result.handler_cleared);
        c->nested_cleared=1;
    }
    if (c->cancel_after==event->stage) atomic_store(&c->cancelled,1);
    return c->reject!=event->stage;
}
static void prepare(Call *c,Fixture *f,unsigned owner) {
    memset(c,0,sizeof(*c)); c->fixture=f; atomic_init(&c->cancelled,0); c->denied_write=-1;
    c->request=(ps3_spurs_exception_request){PS3_SPURS_EXCEPTION_SET,BASE,0,0x7832F8,0x915C70};
    c->config=(ps3_spurs_exception_config){.domain=&f->domain,.owner=f->owners[owner],.instance=BASE,
        .owned_instance_bytes=0x2000,.active_owned_instance=1,.max_claim_attempts=8,
        .poll_context=c,.may_continue=may_continue,.observer_context=c,.observe=observe,.memory_scope={c,permits}};
}
static void apply(Call *c) { ps3_spurs_exception_handler_apply(&c->config,&c->request,&c->result); }
static void expect_sdk(const Call *c,uint32_t value) {
    assert(c->result.stop==PS3_SPURS_EXCEPTION_RUNNING && c->result.has_sdk_result && c->result.sdk_result==value);
}
static void success_and_unset(void) {
    Fixture f; Call c; initialize(&f); prepare(&c,&f,0); apply(&c); expect_sdk(&c,0);
    assert(c.result.slot==PRIMARY_SLOT && c.result.reads==4 && c.result.writes==3 && c.result.written_bytes==24);
    assert(c.result.claim_written && c.result.argument_written && c.result.handler_written);
    assert(c.result.claim_attempts==1 && c.result.conditional_attempts==1 && !c.result.conditional_failures);
    assert(c.result.release_fences==2 && c.count==7 && c.result.publications==7);
    assert(c.events[0].address==BASE+0x70 && c.events[0].length==8);
    assert(c.events[3].stage==PS3_SPURS_EXCEPTION_LOAD_RESERVE && c.events[4].bytes[7]==1);
    assert(get(&f,PRIMARY_SLOT,8)==0x7832F8 && get(&f,PRIMARY_SLOT+8,8)==0x915C70);
    prepare(&c,&f,0); c.request.action=PS3_SPURS_EXCEPTION_UNSET; apply(&c); expect_sdk(&c,0);
    assert(c.result.reads==3 && c.result.writes==1 && c.result.handler_cleared && c.result.release_fences==1);
    assert(!get(&f,PRIMARY_SLOT,8) && get(&f,PRIMARY_SLOT+8,8)==0x915C70);
    finish(&f);
}
static void validation_order(void) {
    Fixture f; Call c; initialize(&f);
    prepare(&c,&f,0); c.request.spurs=0; c.request.handler=0; apply(&c); expect_sdk(&c,POLICY_NULL); assert(!f.reads);
    prepare(&c,&f,0); c.request.spurs=BASE+1; c.request.handler=0; apply(&c); expect_sdk(&c,POLICY_NULL); assert(!f.reads);
    prepare(&c,&f,0); c.request.spurs=BASE+1; apply(&c); expect_sdk(&c,POLICY_ALIGN); assert(!f.reads);
    prepare(&c,&f,0); c.request.wid=16; c.denied_address=BASE+0xB0; c.denied_write=0; apply(&c); expect_sdk(&c,POLICY_INVAL); assert(c.result.reads==1);
    put(&f,BASE+0xB0,4,0); put(&f,BASE+0xD6C,4,1);
    prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_SRCH); assert(c.result.reads==2);
    put(&f,BASE+0xB0,4,UINT32_MAX); prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_STAT); assert(c.result.reads==3);
    put(&f,BASE+0xD6C,4,0); put(&f,PRIMARY_SLOT,8,UINT64_C(0x8000000000000000));
    prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_BUSY); assert(!c.result.writes && !c.result.conditional_attempts);
    int stored=0; assert(ps3_atomic_store_conditional64(&f.domain,f.owners[0],PRIMARY_SLOT,9,&stored)==PS3_GUEST_MEMORY_OK && stored);
    prepare(&c,&f,0); c.request.action=PS3_SPURS_EXCEPTION_UNSET; c.request.handler=0; apply(&c); expect_sdk(&c,0);
    finish(&f);
}
static void second_bank(void) {
    Fixture f; Call c; initialize(&f); put(&f,BASE+0x70,8,UINT64_C(0x40000000));
    put(&f,BASE+0xB0,4,1);
    prepare(&c,&f,0); c.request.wid=31; c.request.handler=1; c.request.argument=UINT32_MAX; apply(&c); expect_sdk(&c,0);
    assert(c.result.slot==BASE+0x1980 && get(&f,c.result.slot,8)==1 && get(&f,c.result.slot+8,8)==UINT32_MAX);
    prepare(&c,&f,0); c.request.wid=32; apply(&c); expect_sdk(&c,POLICY_INVAL);
    put(&f,BASE+0xB0,4,0x8000);
    prepare(&c,&f,0); c.request.wid=16; c.config.owned_instance_bytes=0x1000; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_MEMORY && !c.result.has_sdk_result && c.result.reads==3);
    finish(&f);
}
static void partial_effects(void) {
    for (size_t fail=1;fail<=4;++fail) {
        Fixture f; Call c; initialize(&f); prepare(&c,&f,0); f.fail_read=fail; apply(&c);
        assert(c.result.stop==PS3_SPURS_EXCEPTION_MEMORY && !c.result.has_sdk_result);
        assert(c.result.reads==fail-1 && !c.result.writes && c.count==fail-1);
        assert(!get(&f,PRIMARY_SLOT,8)); finish(&f);
    }
    for (size_t fail=1;fail<=3;++fail) {
        Fixture f; Call c; initialize(&f); prepare(&c,&f,0); f.fail_write=fail; apply(&c);
        assert(c.result.stop==PS3_SPURS_EXCEPTION_MEMORY && !c.result.has_sdk_result && c.result.writes==fail-1);
        assert(get(&f,PRIMARY_SLOT,8)==(fail==1?0:1));
        assert(get(&f,PRIMARY_SLOT+8,8)==(fail==3?0x915C70:UINT64_C(0xDEADBEEF11223344))); finish(&f);
    }
    for (unsigned stage=PS3_SPURS_EXCEPTION_FLAGS;stage<=PS3_SPURS_EXCEPTION_PUBLISH;++stage) {
        Fixture f; Call c; initialize(&f); prepare(&c,&f,0); c.reject=(ps3_spurs_exception_stage)stage; apply(&c);
        assert(c.result.stop==PS3_SPURS_EXCEPTION_OBSERVER && !c.result.has_sdk_result);
        assert(c.result.observer_calls==c.result.publications+1);
        assert(get(&f,PRIMARY_SLOT,8)==(stage<PS3_SPURS_EXCEPTION_CLAIM?0:stage<PS3_SPURS_EXCEPTION_PUBLISH?1:0x7832F8)); finish(&f);
    }
    for (unsigned stage=PS3_SPURS_EXCEPTION_CLAIM;stage<=PS3_SPURS_EXCEPTION_PUBLISH;++stage) {
        Fixture f; Call c; initialize(&f); prepare(&c,&f,0); c.cancel_after=(ps3_spurs_exception_stage)stage; apply(&c);
        assert(c.result.stop==PS3_SPURS_EXCEPTION_CANCELLED && !c.result.has_sdk_result && c.result.claim_written);
        assert(get(&f,PRIMARY_SLOT,8)==(stage==PS3_SPURS_EXCEPTION_PUBLISH?0x7832F8:1)); finish(&f);
    }
    Fixture f; Call c; initialize(&f); put(&f,PRIMARY_SLOT,8,0x7832F8);
    prepare(&c,&f,0); c.request.action=PS3_SPURS_EXCEPTION_UNSET; c.reject=PS3_SPURS_EXCEPTION_CLEAR; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_OBSERVER && !c.result.has_sdk_result && c.result.handler_cleared);
    assert(!get(&f,PRIMARY_SLOT,8) && get(&f,PRIMARY_SLOT+8,8)==UINT64_C(0xDEADBEEF11223344)); finish(&f);
}
static void scope_and_cancellation(void) {
    Fixture f; Call c; initialize(&f); prepare(&c,&f,0); c.denied_address=BASE+0x70; c.denied_write=0; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_MEMORY && !f.reads && !f.writes && !c.count);
    prepare(&c,&f,0); c.denied_address=PRIMARY_SLOT+8; c.denied_write=1; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_MEMORY && c.result.claim_written && !c.result.argument_write_attempted);
    assert(get(&f,PRIMARY_SLOT,8)==1);
    prepare(&c,&f,0); atomic_store(&c.cancelled,1); c.request.spurs=0; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_CANCELLED && !c.result.has_sdk_result);
    finish(&f);
}
static void reservation_loss_and_budget(void) {
    Fixture f; Call c; initialize(&f); uint64_t previous=0;
    assert(ps3_atomic_load_reserve64(&f.domain,f.owners[1],PRIMARY_SLOT+16,&previous)==PS3_GUEST_MEMORY_OK);
    prepare(&c,&f,0); c.interference=1; apply(&c); expect_sdk(&c,0);
    assert(c.result.claim_attempts==2 && c.result.conditional_attempts==2 && c.result.conditional_failures==1 && c.result.writes==3);
    unsigned claims=0;
    for (size_t i=0;i<c.count;++i) claims+=c.events[i].operation==PS3_SPURS_EXCEPTION_WRITE && c.events[i].stage==PS3_SPURS_EXCEPTION_CLAIM;
    assert(claims==1 && c.count==c.result.reads+c.result.writes);
    int lost=0; assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owners[1],&lost)==PS3_GUEST_MEMORY_OK && lost);
    finish(&f);
    initialize(&f); prepare(&c,&f,0); c.interference=2; c.config.max_claim_attempts=2; apply(&c);
    assert(c.result.stop==PS3_SPURS_EXCEPTION_ATTEMPT_LIMIT && !c.result.has_sdk_result);
    assert(c.result.conditional_failures==2 && c.result.claim_attempts==2 && !c.result.writes && !get(&f,PRIMARY_SLOT,8)); finish(&f);
    for (size_t i=0;i<c.count;++i) assert(c.events[i].operation==PS3_SPURS_EXCEPTION_READ);
}
static void unset_interleavings(void) {
    for (unsigned stage=PS3_SPURS_EXCEPTION_CLAIM;stage<=PS3_SPURS_EXCEPTION_PUBLISH;++stage) {
        Fixture f; Call c; initialize(&f); prepare(&c,&f,0); c.nested_unset=(ps3_spurs_exception_stage)stage; apply(&c); expect_sdk(&c,0);
        assert(c.nested_cleared && c.result.handler_written && get(&f,PRIMARY_SLOT+8,8)==0x915C70);
        assert(get(&f,PRIMARY_SLOT,8)==(stage==PS3_SPURS_EXCEPTION_PUBLISH?0:0x7832F8)); finish(&f);
    }
}
static void *run_call(void *context) { apply(context); return NULL; }
static void concurrent_claims(void) {
    Fixture f; Call calls[2]; pthread_barrier_t barrier; initialize(&f); assert(!pthread_barrier_init(&barrier,NULL,2));
    pthread_t threads[2];
    for (unsigned i=0;i<2;++i) {
        prepare(&calls[i],&f,i); calls[i].barrier=&barrier; calls[i].request.handler=0x3000+i*8;
        assert(!pthread_create(&threads[i],NULL,run_call,&calls[i]));
    }
    for (unsigned i=0;i<2;++i) assert(!pthread_join(threads[i],NULL));
    unsigned successes=0,busy=0; size_t writes=0,failures=0;
    for (unsigned i=0;i<2;++i) {
        assert(calls[i].result.has_sdk_result); successes+=calls[i].result.sdk_result==0; busy+=calls[i].result.sdk_result==POLICY_BUSY;
        writes+=calls[i].result.writes; failures+=calls[i].result.conditional_failures;
    }
    assert(successes==1 && busy==1 && writes==3 && failures==1);
    assert(!pthread_barrier_destroy(&barrier)); finish(&f);
}
int main(void) {
    success_and_unset(); validation_order(); second_bank(); partial_effects(); scope_and_cancellation();
    reservation_loss_and_budget(); unset_interleavings(); concurrent_claims();
    puts("SPURS_EXCEPTION_DOMAIN_PASS authentic_errors=1 llsc64=1 bank32=1 partial_effects=1 unset_interleaving=1 borrowed_owner=1 callback_delivery=0");
    return 0;
}
