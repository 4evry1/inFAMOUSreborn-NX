#define _POSIX_C_SOURCE 200809L
#include "../domain/spurs_workload_registration.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum { BASE=0x1000, OUTPUT=0x3000, CAPACITY=0x4000 };
typedef struct {
    unsigned char ram[CAPACITY];
    ps3_pthread_guard atomic_guard, registration_guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[4];
    ps3_atomic_pin_record pins[2];
    ps3_atomic_owner owner, reservation_owner;
    ps3_spurs_registration_config config;
    size_t write_attempts, confirmed_writes, fail_write, observed, reject_event;
    unsigned cancel, race_once, race_line;
    size_t cancel_after_event;
} Fixture;
static ps3_guest_memory_status map(void *p,uint32_t a,uint64_t n) {
    (void)p;return a<=CAPACITY && n<=CAPACITY-a?PS3_GUEST_MEMORY_OK:PS3_GUEST_MEMORY_INVALID_RANGE;
}
static ps3_guest_memory_status read_memory(void *p,uint32_t a,void *bytes,size_t n) {
    Fixture *f=p;if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;
    memcpy(bytes,f->ram+a,n);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status write_memory(void *p,uint32_t a,const void *bytes,size_t n) {
    Fixture *f=p;if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;
    if(++f->write_attempts==f->fail_write)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    memcpy(f->ram+a,bytes,n);++f->confirmed_writes;return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status zero_memory(void *p,uint32_t a,uint64_t n) {
    if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;
    memset(((Fixture*)p)->ram+a,0,(size_t)n);return PS3_GUEST_MEMORY_OK;
}
static int observe(void *p,const ps3_spurs_registration_event *event) {
    Fixture *f=p;++f->observed;
    if(f->race_once && event->operation==PS3_SPURS_REGISTRATION_READ && event->address==BASE+0xb0) {
        unsigned char enabled[4]={0x80,0,0,0};f->race_once=0;
        assert(ps3_atomic_write(&f->domain,BASE+0xb0,enabled,4)==PS3_GUEST_MEMORY_OK);
    }
    if(f->race_line && event->operation==PS3_SPURS_REGISTRATION_READ && event->length==128) {
        unsigned char value=0;f->race_line=0;
        assert(ps3_atomic_write(&f->domain,BASE+0x9f,&value,1)==PS3_GUEST_MEMORY_OK);
    }
    if(f->observed==f->cancel_after_event)f->cancel=1;
    return f->observed!=f->reject_event;
}
static int may_continue(void *p){return !((Fixture*)p)->cancel;}
static void init(Fixture *f) {
    memset(f,0,sizeof *f);
    assert(!ps3_pthread_guard_init(&f->atomic_guard));assert(!ps3_pthread_guard_init(&f->registration_guard));
    ps3_guest_memory memory={f,map,map,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->atomic_guard),f->owners,4,f->pins,2)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_owner_acquire(&f->domain,&f->owner)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_owner_acquire(&f->domain,&f->reservation_owner)==PS3_GUEST_MEMORY_OK);
    f->config=(ps3_spurs_registration_config){.domain=&f->domain,.owner=f->owner,
        .registration_guard=ps3_pthread_guard_port(&f->registration_guard),.instance=BASE,
        .active_owned_instance=1,.max_attempts=16,.poll_context=f,.may_continue=may_continue,
        .observer_context=f,.observe=observe};
    f->ram[BASE+0x77]=0xff;
    for(unsigned i=0;i<16;++i) {
        memset(f->ram+BASE+0x100+i*0x80,0xa5,0x80);
        f->ram[BASE+0x50+i]=0xa0;
    }
}
static void finish(Fixture *f){
    assert(ps3_atomic_owner_release(&f->domain,&f->reservation_owner)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_owner_release(&f->domain,&f->owner)==PS3_GUEST_MEMORY_OK);
    assert(!ps3_pthread_guard_destroy(&f->registration_guard));assert(!ps3_pthread_guard_destroy(&f->atomic_guard));
}
static ps3_spurs_registration_request request(void){
    ps3_spurs_registration_request r={.spurs=BASE,.wid_output=OUTPUT,.policy_module=0x69c880,.size=0x2d80,
        .data=0x404e9300,.min_contention=1,.max_contention=5,.name_class=0x5ecef8,.name_instance=0x915c80};
    for(unsigned i=0;i<5;++i)r.priority[i]=2;
    return r;
}
static uint32_t be32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void success_and_reservations(void){
    Fixture f;init(&f);ps3_spurs_registration_request q=request();ps3_spurs_registration_result r;
    uint32_t old;assert(ps3_atomic_load_reserve32(&f.domain,f.reservation_owner,BASE+0xb0,&old)==PS3_GUEST_MEMORY_OK);
    ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.stop==PS3_SPURS_REGISTRATION_RUNNING && r.has_sdk_result && !r.sdk_result && r.wid==0 && r.unique_id==0);
    assert(be32(f.ram+BASE+0xb0)==0x80000000 && be32(f.ram+BASE+0xb4)==0x80000000);
    assert(f.ram[BASE+0x80]==2 && f.ram[BASE+0x50]==0xa5 && f.ram[BASE+0x40]==1);
    assert(f.ram[BASE+0x72]==0xff && f.ram[BASE+0xbd]==0xff);
    for(unsigned i=0;i<0x80;++i)assert(f.ram[BASE+0x100+i]==(i>=0x20&&i<0x28?0xa5:0));
    assert(be32(f.ram+BASE+0xb00+4)==0x69c880 && be32(f.ram+BASE+0xb08+4)==0x404e9300);
    assert(be32(f.ram+BASE+0xb10)==0x2d80 && !memcmp(f.ram+BASE+0xb18,q.priority,8));
    assert(be32(f.ram+BASE+0xe04)==q.name_class && be32(f.ram+BASE+0xe0c)==q.name_instance);
    int lost=0;assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.reservation_owner,&lost)==PS3_GUEST_MEMORY_OK && lost);
    assert(r.effects==(PS3_SPURS_REGISTRATION_SLOT_CLAIMED|PS3_SPURS_REGISTRATION_WID_WRITTEN|PS3_SPURS_REGISTRATION_PREPARING|
        PS3_SPURS_REGISTRATION_UNIQUE_ID_WRITTEN|PS3_SPURS_REGISTRATION_RUNNABLE_PUBLISHED|
        PS3_SPURS_REGISTRATION_UPDATE_MESSAGE|PS3_SPURS_REGISTRATION_SERVICE_MESSAGE));
    assert(r.writes==f.confirmed_writes && r.observer_calls==r.publications);
    q.wid_output+=4;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.has_sdk_result && r.sdk_result==0 && r.wid==1 && r.unique_id==0);
    assert(be32(f.ram+BASE+0xb4)==0xc0000000);
    q.policy_module+=0x10;q.wid_output+=4;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.has_sdk_result && r.wid==2 && r.unique_id==1);finish(&f);
}
static void validation_and_full(void){
    Fixture f;init(&f);ps3_spurs_registration_request q=request();ps3_spurs_registration_result r;
    q.spurs=1;q.policy_module=0;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x80410811u&&!r.writes);
    q=request();q.spurs++;q.min_contention=0;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410810u);
    q=request();q.priority[7]=16;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410802u);
    q=request();f.ram[BASE+0xd6f]=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.sdk_result==0x8041080fu);
    f.ram[BASE+0xd6f]=0;f.ram[BASE+0x74]=0x40;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_UNSUPPORTED_VARIANT&&!r.writes);
    f.ram[BASE+0x74]=0;q.wid_output=BASE+0xb0;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_UNSUPPORTED_ALIAS&&!r.writes);
    q=request();memset(f.ram+BASE+0xb0,0xff,4);ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.has_sdk_result&&r.sdk_result==0x80410801u&&r.wid==32&&be32(f.ram+OUTPUT)==32&&r.writes==1);
    finish(&f);
}
static void native_fences(void){
    Fixture f;init(&f);ps3_spurs_registration_request q=request();ps3_spurs_registration_result r;
    f.config.active_owned_instance=0;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_REGISTRATION_UNOWNED_INSTANCE&&!r.writes);
    f.config.active_owned_instance=1;f.ram[BASE+0x20]=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_REGISTRATION_CORRUPT_STATE&&!r.writes);
    f.ram[BASE+0x20]=0;f.cancel=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_REGISTRATION_CANCELLED&&!r.writes);
    f.cancel=0;f.config.registration_guard=f.domain.guard;ps3_spurs_workload_registration_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_REGISTRATION_INVALID_ARGUMENT);
    finish(&f);
}
static void faults_and_observer_order(void){
    Fixture baseline;init(&baseline);ps3_spurs_registration_request q=request();ps3_spurs_registration_result r;
    ps3_spurs_workload_registration_apply(&baseline.config,&q,&r);assert(r.has_sdk_result&&!r.sdk_result);
    size_t writes=baseline.write_attempts,events=baseline.observed;finish(&baseline);
    for(size_t fail=1;fail<=writes;++fail){
        Fixture f;init(&f);f.fail_write=fail;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
        assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_MEMORY);
        assert(r.writes==f.confirmed_writes&&r.writes==fail-1);
        if(fail>1)assert(r.effects&PS3_SPURS_REGISTRATION_SLOT_CLAIMED);
        finish(&f);
    }
    for(size_t reject=1;reject<=events;++reject){
        Fixture f;init(&f);f.reject_event=reject;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
        assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_OBSERVER);
        assert(r.writes==f.confirmed_writes&&r.observer_calls==reject&&r.publications==reject-1);
        if(r.effects&PS3_SPURS_REGISTRATION_WID_WRITTEN)assert(be32(f.ram+OUTPUT)==r.wid);
        finish(&f);
    }
}
typedef struct{Fixture *f;uint32_t output;ps3_spurs_registration_result result;} Worker;
static void *register_worker(void *p){Worker *w=p;ps3_spurs_registration_request q=request();q.wid_output=w->output;ps3_spurs_workload_registration_apply(&w->f->config,&q,&w->result);return 0;}
static void concurrency(void){
    Fixture f;init(&f);pthread_t threads[16];Worker workers[16];unsigned seen=0;
    for(unsigned i=0;i<16;++i){workers[i]=(Worker){.f=&f,.output=OUTPUT+4*i};assert(!pthread_create(&threads[i],0,register_worker,&workers[i]));}
    for(unsigned i=0;i<16;++i){assert(!pthread_join(threads[i],0));assert(workers[i].result.has_sdk_result&&!workers[i].result.sdk_result);unsigned wid=workers[i].result.wid;assert(wid<16&&!(seen&(1u<<wid)));seen|=1u<<wid;assert(workers[i].result.unique_id==0);}
    assert(seen==0xffff && be32(f.ram+BASE+0xb0)==0xffff0000 && be32(f.ram+BASE+0xb4)==0xffff0000);finish(&f);
}

static void bounded_races_and_cancellation(void){
    Fixture f;ps3_spurs_registration_request q=request();ps3_spurs_registration_result r;
    init(&f);f.race_once=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.has_sdk_result&&!r.sdk_result&&r.wid==1&&be32(f.ram+BASE+0xb0)==0xc0000000);finish(&f);
    init(&f);f.race_once=1;f.config.max_attempts=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_ATTEMPTS&&!r.effects&&r.atomic_attempts==1);finish(&f);
    init(&f);f.race_line=1;f.config.max_attempts=1;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_ATTEMPTS);
    assert((r.effects&PS3_SPURS_REGISTRATION_PREPARING)&&!(r.effects&PS3_SPURS_REGISTRATION_RUNNABLE_PUBLISHED));
    assert(f.ram[BASE+0x80]==1&&be32(f.ram+BASE+0xb4)==0&&!f.ram[BASE+0xbd]);finish(&f);
    init(&f);ps3_spurs_workload_registration_apply(&f.config,&q,&r);size_t events=f.observed;finish(&f);
    for(size_t event=1;event<=events;++event){
        init(&f);f.cancel_after_event=event;ps3_spurs_workload_registration_apply(&f.config,&q,&r);
        assert(!r.has_sdk_result&&r.stop==PS3_SPURS_REGISTRATION_CANCELLED);
        assert(r.writes==f.confirmed_writes&&r.observer_calls==event&&r.publications==event);finish(&f);
    }
    init(&f);q.size=0;q.max_contention=0;q.min_contention=99;q.hook=0x12345678;q.hook_argument=0x87654321;
    ps3_spurs_workload_registration_apply(&f.config,&q,&r);
    assert(r.has_sdk_result&&!r.sdk_result&&f.ram[BASE+0x40]==8&&f.ram[BASE+0x50]==0xa0);
    assert(be32(f.ram+BASE+0x134)==q.hook&&be32(f.ram+BASE+0x13c)==q.hook_argument&&f.ram[BASE+0xa0]==2);
    finish(&f);
}

int main(void){success_and_reservations();validation_and_full();native_fences();faults_and_observer_order();bounded_races_and_cancellation();concurrency();puts("SPURS_REGISTRATION_TESTS_OK");return 0;}
