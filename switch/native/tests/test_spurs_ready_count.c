#define _POSIX_C_SOURCE 200809L
#include "../domain/spurs_ready_count.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>

enum { RAM_BYTES=0x4000,BASE=0x1000,
    POLICY_NULL=0x80410811u,POLICY_ALIGN=0x80410810u,POLICY_INVAL=0x80410802u,
    POLICY_SRCH=0x80410805u,POLICY_STAT=0x8041080Fu };
typedef struct {
    unsigned char ram[RAM_BYTES];
    ps3_pthread_guard guard;
    ps3_pthread_wait_port wait;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[4];
    ps3_atomic_owner owners[3];
    size_t reads,writes,fail_read,fail_write;
} Fixture;
typedef struct {
    ps3_spurs_ready_count_operation operation;
    ps3_spurs_ready_count_stage stage;
    uint32_t address;
    size_t length;
    unsigned char bytes[4];
} Event;
typedef struct {
    Fixture *fixture;
    ps3_spurs_ready_count_config config;
    ps3_spurs_ready_count_request request;
    ps3_spurs_ready_count_result result;
    Event events[8];
    size_t count,scope_calls;
    atomic_uint cancelled;
    ps3_spurs_ready_count_stage reject,cancel_after;
    uint32_t deny_address;
    int deny_write;
    unsigned mutate_request;
    unsigned owner_index;
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
static void initialize(Fixture *f) {
    memset(f,0,sizeof(*f)); assert(!ps3_pthread_guard_init(&f->guard));
    assert(!ps3_pthread_wait_port_init(&f->wait));
    ps3_guest_memory memory={f,map_memory,map_memory,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->guard),f->records,4,NULL,0)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_domain_attach_wait_port(&f->domain,ps3_pthread_wait_port_port(&f->wait))==PS3_GUEST_MEMORY_OK);
    for (unsigned i=0;i<3;++i) assert(ps3_atomic_owner_acquire(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
    put(f,BASE+0xB0,4,UINT32_MAX);
    memset(f->ram+BASE+0x80,2,16); memset(f->ram+BASE+0xD0,2,16);
}
static void finish(Fixture *f) {
    for (unsigned i=0;i<3;++i) {
        assert(ps3_atomic_owner_read(&f->domain,f->owners[i],0,NULL,0)==PS3_GUEST_MEMORY_OK);
        assert(ps3_atomic_owner_release(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
    }
    ps3_pthread_wait_port_destroy(&f->wait); assert(!ps3_pthread_guard_destroy(&f->guard));
}
static int may_continue(void *context) { return !atomic_load(&((Call *)context)->cancelled); }
static int permits(void *context,uint32_t address,size_t bytes,unsigned write) {
    Call *c=context; (void)bytes; ++c->scope_calls;
    return address!=c->deny_address || (int)write!=c->deny_write;
}
static int observe(void *context,const ps3_spurs_ready_count_event *event) {
    Call *c=context; assert(c->count<8 && event->bytes && event->length<=4);
    Event *copy=&c->events[c->count++];
    *copy=(Event){event->operation,event->stage,event->address,event->length,{0}};
    memcpy(copy->bytes,event->bytes,event->length);
    /* A coherent read here also proves consumer callbacks run outside its guard. */
    assert(ps3_atomic_owner_read(&c->fixture->domain,c->fixture->owners[c->owner_index],0,NULL,0)==PS3_GUEST_MEMORY_OK);
    if (c->mutate_request && event->stage==PS3_SPURS_READY_COUNT_FLAGS) {
        c->request=(ps3_spurs_ready_count_request){0,31,256}; c->config.domain=NULL;
    }
    if (c->barrier && event->stage==PS3_SPURS_READY_COUNT_STATE) {
        int status=pthread_barrier_wait(c->barrier); assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    if (c->cancel_after==event->stage) atomic_store(&c->cancelled,1);
    return c->reject!=event->stage;
}
static void prepare(Call *c,Fixture *f,unsigned owner) {
    memset(c,0,sizeof(*c)); c->fixture=f; c->owner_index=owner; atomic_init(&c->cancelled,0); c->deny_write=-1;
    c->request=(ps3_spurs_ready_count_request){BASE,0,7};
    c->config=(ps3_spurs_ready_count_config){.domain=&f->domain,.owner=f->owners[owner],.instance=BASE,
        .owned_instance_bytes=0x1000,.active_owned_instance=1,.poll_context=c,.may_continue=may_continue,
        .observer_context=c,.observe=observe,.memory_scope={c,permits}};
}
static void apply(Call *c) { ps3_spurs_ready_count_apply(&c->config,&c->request,&c->result); }
static void expect_sdk(const Call *c,uint32_t value) {
    assert(c->result.stop==PS3_SPURS_READY_COUNT_RUNNING && c->result.has_sdk_result && c->result.sdk_result==value);
}
static void exact_publication(void) {
    const unsigned wids[]={0,15,16,31}; const unsigned values[]={0,255,7,1};
    for (unsigned i=0;i<4;++i) {
        Fixture f; initialize(&f); Call c; prepare(&c,&f,0);
        f.ram[BASE+0x74]=i>=2?0x40:0;
        memset(f.ram+BASE,0xA5,32); c.request.wid=wids[i]; c.request.value=values[i];
        unsigned char before[RAM_BYTES]; memcpy(before,f.ram,sizeof(before));
        apply(&c); expect_sdk(&c,0); assert(c.result.ready_address==BASE+wids[i]);
        assert(c.result.reads==4 && c.result.writes==1 && c.result.written_bytes==1 && c.result.stored && c.result.store_attempted);
        assert(c.result.release_fences==1 && c.count==5 && c.result.publications==5);
        assert(c.events[0].stage==PS3_SPURS_READY_COUNT_FLAGS && c.events[0].address==BASE+0x74 && c.events[0].length==1);
        assert(c.events[1].address==BASE+0xB0 && c.events[1].length==4);
        assert(c.events[2].address==BASE+0xD6C && c.events[2].length==4);
        assert(c.events[3].address==BASE+(wids[i]<16?0x80+wids[i]:0xD0+(wids[i]&15)) && c.events[3].length==1);
        assert(c.events[4].operation==PS3_SPURS_READY_COUNT_WRITE && c.events[4].length==1 && c.events[4].bytes[0]==values[i]);
        before[BASE+wids[i]]=(unsigned char)values[i]; assert(!memcmp(before,f.ram,sizeof(before))); finish(&f);
    }
}
static void validation_order(void) {
    Fixture f; initialize(&f); Call c; uint32_t sdk=0;
    prepare(&c,&f,0); c.request.spurs=0; c.request.wid=UINT32_MAX; c.request.value=256; c.config.active_owned_instance=0;
    assert(ps3_spurs_ready_count_validate_address(&c.request,&sdk) && sdk==POLICY_NULL); apply(&c); expect_sdk(&c,POLICY_NULL); assert(!c.count);
    prepare(&c,&f,0); c.request.spurs=BASE+1; apply(&c); expect_sdk(&c,POLICY_ALIGN); assert(!c.count);
    prepare(&c,&f,0); c.request.wid=16; apply(&c); expect_sdk(&c,POLICY_INVAL); assert(c.count==1);
    prepare(&c,&f,0); c.request.value=256; apply(&c); expect_sdk(&c,POLICY_INVAL); assert(c.count==1);
    prepare(&c,&f,0); c.request.value=256; c.deny_address=BASE+0x74; c.deny_write=0; apply(&c);
    assert(c.result.stop==PS3_SPURS_READY_COUNT_MEMORY && !c.result.has_sdk_result && !c.count);
    put(&f,BASE+0xB0,4,0); put(&f,BASE+0xD6C,4,1); f.ram[BASE+0x80]=0;
    prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_SRCH); assert(c.count==2);
    put(&f,BASE+0xB0,4,0x80000000u); prepare(&c,&f,0); c.deny_address=BASE+0x80; c.deny_write=0;
    apply(&c); expect_sdk(&c,POLICY_STAT); assert(c.count==3);
    put(&f,BASE+0xD6C,4,0); prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_STAT); assert(c.count==4);
    for (unsigned state=0;state<6;++state) if (state!=2) {
        f.ram[BASE+0x80]=(unsigned char)state; prepare(&c,&f,0); apply(&c); expect_sdk(&c,POLICY_STAT);
    }
    f.ram[BASE+0x74]=0x40; put(&f,BASE+0xB0,4,1); prepare(&c,&f,0); c.request.wid=31; apply(&c); expect_sdk(&c,0);
    prepare(&c,&f,0); c.request.wid=32; apply(&c); expect_sdk(&c,POLICY_INVAL); assert(c.count==1);
    prepare(&c,&f,0); c.config.active_owned_instance=0; apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE && !c.count);
    prepare(&c,&f,0); c.request.spurs=BASE+128; apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE && !c.count);
    prepare(&c,&f,0); c.config.owner.generation++; apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_MEMORY && !c.count);
    assert(!ps3_spurs_ready_count_validate_address(NULL,&sdk)); finish(&f);
}
static void partial_effects(void) {
    for (unsigned failure=1;failure<=4;++failure) {
        Fixture f; initialize(&f); Call c; prepare(&c,&f,0); f.fail_read=failure;
        apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_MEMORY && !c.result.has_sdk_result && !c.result.stored);
        assert(c.result.reads==failure-1 && c.count==failure-1 && !c.result.store_attempted); finish(&f);
    }
    for (unsigned stage=PS3_SPURS_READY_COUNT_FLAGS;stage<=PS3_SPURS_READY_COUNT_STORE;++stage) {
        Fixture f; initialize(&f); Call c; prepare(&c,&f,0); c.reject=(ps3_spurs_ready_count_stage)stage;
        apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_OBSERVER && !c.result.has_sdk_result);
        assert(c.result.stored==(stage==PS3_SPURS_READY_COUNT_STORE)); assert(f.ram[BASE]==(stage==PS3_SPURS_READY_COUNT_STORE?7:0));
        prepare(&c,&f,0); c.cancel_after=(ps3_spurs_ready_count_stage)stage;
        apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_CANCELLED && !c.result.has_sdk_result);
        assert(c.result.stored==(stage==PS3_SPURS_READY_COUNT_STORE)); finish(&f);
    }
    Fixture f; initialize(&f); Call c; prepare(&c,&f,0); c.deny_address=BASE; c.deny_write=1;
    apply(&c); assert(c.result.stop==PS3_SPURS_READY_COUNT_MEMORY && !c.result.store_attempted && !c.result.stored && c.count==4);
    prepare(&c,&f,0); f.fail_write=1; apply(&c);
    assert(c.result.stop==PS3_SPURS_READY_COUNT_MEMORY && c.result.store_attempted && !c.result.stored && c.count==4 && !f.ram[BASE]);
    f.fail_write=0; prepare(&c,&f,0); atomic_store(&c.cancelled,1); apply(&c);
    assert(c.result.stop==PS3_SPURS_READY_COUNT_CANCELLED && !c.count && !c.scope_calls);
    prepare(&c,&f,0); c.mutate_request=1; apply(&c); expect_sdk(&c,0); assert(f.ram[BASE]==7 && c.result.stored);
    finish(&f);
}
typedef struct { Fixture *fixture; ps3_atomic_wait_result result; uint64_t mask; } Wait;
static void *wait_for_loss(void *context) {
    Wait *w=context;
    w->result=ps3_atomic_owners_wait_reservation_loss(&w->fixture->domain,&w->fixture->owners[1],1,
        ps3_pthread_wait_port_now_ns()+UINT64_C(2000000000),NULL,&w->mask);
    return NULL;
}
static void reservation_wake(void) {
    Fixture f; initialize(&f); unsigned char snapshot[128];
    assert(ps3_atomic_load_reserve128(&f.domain,f.owners[1],BASE,snapshot)==PS3_GUEST_MEMORY_OK);
    Wait wait={&f,PS3_ATOMIC_WAIT_INVALID,0}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,wait_for_loss,&wait));
    const uint64_t deadline=ps3_pthread_wait_port_now_ns()+UINT64_C(1000000000);
    while (!ps3_pthread_wait_port_sleepers(&f.wait) && ps3_pthread_wait_port_now_ns()<deadline) sched_yield();
    assert(ps3_pthread_wait_port_sleepers(&f.wait)==1);
    Call c; prepare(&c,&f,0); apply(&c); expect_sdk(&c,0);
    assert(!pthread_join(thread,NULL)); assert(wait.result==PS3_ATOMIC_WAIT_LOST && wait.mask==1);
    int lost=0; assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owners[1],&lost)==PS3_GUEST_MEMORY_OK && lost);
    assert(ps3_pthread_wait_port_wait_calls(&f.wait)>0);
    assert(ps3_atomic_load_reserve128(&f.domain,f.owners[1],BASE,snapshot)==PS3_GUEST_MEMORY_OK);
    prepare(&c,&f,0); c.request.value=256; apply(&c); expect_sdk(&c,POLICY_INVAL);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owners[1],&lost)==PS3_GUEST_MEMORY_OK && !lost);
    prepare(&c,&f,0); c.reject=PS3_SPURS_READY_COUNT_STORE; apply(&c);
    assert(c.result.stored && c.result.stop==PS3_SPURS_READY_COUNT_OBSERVER);
    assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owners[1],&lost)==PS3_GUEST_MEMORY_OK && lost);
    for (unsigned i=0;i<2;++i) {
        assert(ps3_atomic_load_reserve128(&f.domain,f.owners[1],BASE,snapshot)==PS3_GUEST_MEMORY_OK);
        prepare(&c,&f,0); c.request.value=0; apply(&c); expect_sdk(&c,0);
        assert(f.ram[BASE]==0 && c.result.stored);
        assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owners[1],&lost)==PS3_GUEST_MEMORY_OK && lost);
    }
    finish(&f);
}
static void *run_call(void *context) { apply(context); return NULL; }
static void concurrent_stores(void) {
    Fixture f; initialize(&f); Call calls[2]; pthread_t threads[2]; pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier,NULL,2));
    for (unsigned i=0;i<2;++i) { prepare(&calls[i],&f,i); calls[i].request.value=17+i; calls[i].barrier=&barrier; }
    for (unsigned i=0;i<2;++i) assert(!pthread_create(&threads[i],NULL,run_call,&calls[i]));
    for (unsigned i=0;i<2;++i) { assert(!pthread_join(threads[i],NULL)); expect_sdk(&calls[i],0); assert(calls[i].result.stored); }
    assert(f.ram[BASE]==17 || f.ram[BASE]==18); assert(f.writes==2); assert(!pthread_barrier_destroy(&barrier)); finish(&f);
}
int main(void) {
    exact_publication(); validation_order(); partial_effects(); reservation_wake(); concurrent_stores();
    puts("SPURS_READY_COUNT_DOMAIN_PASS byte-publication/sdk-order/partial-effects/borrowed-owner/reservation-wake/concurrency");
    return 0;
}
