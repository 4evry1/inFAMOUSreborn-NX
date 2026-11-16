#define _POSIX_C_SOURCE 200809L
#include "../domain/spurs_idle_request.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sched.h>
enum { BASE=0x1000, CAPACITY=0x4000 };
typedef struct {
    unsigned char ram[CAPACITY];
    ps3_pthread_guard domain_guard, workload_guard;
    ps3_pthread_wait_port wait_storage;
    ps3_notify_port real_wait;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[2];
    ps3_atomic_owner owner;
    ps3_spurs_idle_config config;
    size_t read_attempts, fail_read, write_attempts, fail_write, observed, reject, cancel_after;
    uint32_t observed_addresses[4];
    unsigned cancel, deny_write;
    uint32_t deny_read_address;
    size_t scope_calls;
    atomic_uint wakes, wait_entered;
} Fixture;
static uint64_t now_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000u+(unsigned)t.tv_nsec;}
static ps3_guest_memory_status map(void *p,uint32_t a,uint64_t n){(void)p;return a<=CAPACITY&&n<=CAPACITY-a?PS3_GUEST_MEMORY_OK:PS3_GUEST_MEMORY_INVALID_RANGE;}
static ps3_guest_memory_status read_memory(void *p,uint32_t a,void *bytes,size_t n){
    Fixture *f=p;if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;
    if(++f->read_attempts==f->fail_read)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    memcpy(bytes,f->ram+a,n);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status write_memory(void *p,uint32_t a,const void *bytes,size_t n){
    Fixture *f=p;if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;
    if(++f->write_attempts==f->fail_write)return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    memcpy(f->ram+a,bytes,n);return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status zero_memory(void *p,uint32_t a,uint64_t n){if(map(p,a,n))return PS3_GUEST_MEMORY_INVALID_RANGE;memset(((Fixture*)p)->ram+a,0,(size_t)n);return PS3_GUEST_MEMORY_OK;}
static ps3_notify_wait_result wait_real(void *p,const uint64_t *word,uint64_t expected,const uint32_t *cancel,uint64_t deadline){
    Fixture *f=p;atomic_store(&f->wait_entered,1);return f->real_wait.wait(f->real_wait.context,word,expected,cancel,deadline);
}
static void wake_real(void *p){Fixture *f=p;atomic_fetch_add(&f->wakes,1);f->real_wait.wake_all(f->real_wait.context);}
static int poll(void *p){return !((Fixture*)p)->cancel;}
static int observe(void *p,const ps3_spurs_idle_event *event){
    Fixture *f=p;assert(f->observed<4);f->observed_addresses[f->observed]=event->address;++f->observed;
    if(f->cancel_after==f->observed)f->cancel=1;
    if(event->operation==PS3_SPURS_IDLE_WRITE)assert(event->length==1&&f->ram[event->address]==event->bytes[0]);
    return f->observed!=f->reject;
}
static void init(Fixture *f){
    memset(f,0,sizeof *f);atomic_init(&f->wakes,0);atomic_init(&f->wait_entered,0);
    assert(!ps3_pthread_guard_init(&f->domain_guard)&&!ps3_pthread_guard_init(&f->workload_guard));
    assert(!ps3_pthread_wait_port_init(&f->wait_storage));f->real_wait=ps3_pthread_wait_port_port(&f->wait_storage);
    ps3_guest_memory memory={f,map,map,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->domain_guard),f->records,2,NULL,0)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_domain_attach_wait_port(&f->domain,(ps3_notify_port){f,wait_real,wake_real})==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_owner_acquire(&f->domain,&f->owner)==PS3_GUEST_MEMORY_OK);
    f->ram[BASE]=0;f->ram[BASE+0x10]=3;f->ram[BASE+0xb0]=0x80;
    f->config=(ps3_spurs_idle_config){&f->domain,BASE,1,ps3_pthread_guard_port(&f->workload_guard),f,poll,f,observe,{0}};
}
static void finish(Fixture *f){assert(ps3_atomic_owner_release(&f->domain,&f->owner)==PS3_GUEST_MEMORY_OK);assert(!ps3_pthread_guard_destroy(&f->domain_guard)&&!ps3_pthread_guard_destroy(&f->workload_guard));ps3_pthread_wait_port_destroy(&f->wait_storage);}
static void success_and_layout(void){
    Fixture f;init(&f);ps3_spurs_idle_request request={BASE,0,7};ps3_spurs_idle_result result;
    unsigned char before[CAPACITY];memcpy(before,f.ram,sizeof before);
    ps3_spurs_idle_request_apply(&f.config,&request,&result);
    assert(result.stop==PS3_SPURS_IDLE_RUNNING&&result.has_sdk_result&&!result.sdk_result);
    assert(result.reads==3&&result.writes==1&&result.written_bytes==1&&result.idle_byte_written);
    assert(f.observed==4&&result.observer_calls==4&&result.publications==4);
    const uint32_t order[]={BASE+0x74,BASE+0xb0,BASE+0xd6c,BASE+0x10};assert(!memcmp(order,f.observed_addresses,sizeof order));
    before[BASE+0x10]=7;assert(!memcmp(before,f.ram,sizeof before));assert(f.ram[BASE]==0); /* idle7 does not fabricate readiness */
    finish(&f);
    for(unsigned count=0;count<8;++count){
        init(&f);request=(ps3_spurs_idle_request){BASE,15,count};f.ram[BASE+0xb0]=0;f.ram[BASE+0xb1]=1;
        ps3_spurs_idle_request_apply(&f.config,&request,&result);assert(result.has_sdk_result&&!result.sdk_result&&f.ram[BASE+0x1f]==count);finish(&f);
    }
}
static void precedence(void){
    Fixture f;init(&f);ps3_spurs_idle_request q={0,UINT32_MAX,UINT32_MAX};ps3_spurs_idle_result r;uint32_t code=77;
    assert(!ps3_spurs_idle_request_validate_address(NULL,&code)&&code==77);
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x80410711u&&!r.reads);
    q.spurs=BASE+1;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410710u&&!r.reads);
    q.spurs=BASE;f.ram[BASE+0x74]=0x40;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x8041070fu&&r.reads==1);
    f.observed=0;f.ram[BASE+0x74]=0;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410702u&&r.reads==1);
    f.observed=0;q.wid=0;q.count=8;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410702u&&r.reads==1);
    f.observed=0;q.count=7;f.ram[BASE+0xb0]=0;f.ram[BASE+0xd6f]=1;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x80410705u&&r.reads==2);
    f.observed=0;f.ram[BASE+0xb0]=0x80;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.sdk_result==0x8041070fu&&r.reads==3);
    assert(!r.writes);finish(&f);
}
static void failures(void){
    Fixture f;ps3_spurs_idle_request q={BASE,0,7};ps3_spurs_idle_result r;
    for(unsigned i=1;i<=3;++i){init(&f);f.fail_read=i;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_MEMORY&&!r.has_sdk_result&&!r.writes&&r.reads==i-1);finish(&f);}
    init(&f);f.fail_write=1;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_MEMORY&&!r.has_sdk_result&&!r.idle_byte_written&&f.ram[BASE+0x10]==3);finish(&f);
    for(unsigned i=1;i<=4;++i){init(&f);f.reject=i;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_OBSERVER&&!r.has_sdk_result&&r.observer_calls==i&&r.publications==i-1);assert(r.idle_byte_written==(i==4));if(i==4)assert(r.written_bytes==1&&f.ram[BASE+0x10]==7);finish(&f);}
    for(unsigned i=1;i<=4;++i){init(&f);f.cancel_after=i;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_CANCELLED&&!r.has_sdk_result&&r.observer_calls==i);assert(r.idle_byte_written==(i==4));finish(&f);}
    init(&f);f.cancel=1;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_CANCELLED&&!r.reads&&!r.writes);finish(&f);
    init(&f);f.config.active_owned_instance=0;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_UNOWNED_INSTANCE&&!r.reads);finish(&f);
    init(&f);f.config.workload_guard=f.domain.guard;ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_INVALID_ARGUMENT&&!r.reads);finish(&f);
}
typedef struct{Fixture *fixture;ps3_atomic_wait_result result;uint64_t mask;} Waiter;
static void *wait_for_loss(void *p){Waiter *w=p;ps3_notify_cancel cancel={0};w->result=ps3_atomic_owners_wait_reservation_loss(&w->fixture->domain,&w->fixture->owner,1,now_ns()+2000000000u,&cancel,&w->mask);return NULL;}
static void actual_reservation_notification(void){
    Fixture f;init(&f);unsigned char line[128];assert(ps3_atomic_load_reserve128(&f.domain,f.owner,BASE,line)==PS3_GUEST_MEMORY_OK);
    Waiter waiter={.fixture=&f};pthread_t thread;assert(!pthread_create(&thread,NULL,wait_for_loss,&waiter));uint64_t deadline=now_ns()+1000000000u;
    while(!atomic_load(&f.wait_entered)){assert(now_ns()<deadline);sched_yield();}
    ps3_spurs_idle_request q={BASE,0,7};ps3_spurs_idle_result r;ps3_spurs_idle_request_apply(&f.config,&q,&r);
    assert(r.has_sdk_result&&!r.sdk_result);assert(!pthread_join(thread,NULL));
    assert(waiter.result==PS3_ATOMIC_WAIT_LOST&&waiter.mask==1&&atomic_load(&f.wakes)>0);
    int lost=0;assert(ps3_atomic_owner_take_reservation_loss(&f.domain,f.owner,&lost)==PS3_GUEST_MEMORY_OK&&lost);assert(f.ram[BASE]==0);finish(&f);
}

static int scoped_permits(void *context,uint32_t address,size_t length,unsigned write){
    Fixture *f=context;++f->scope_calls;assert(length==1||length==4);
    return write?!f->deny_write:address!=f->deny_read_address;
}
static void memory_scope_and_sdk_precedence(void){
    Fixture f;ps3_spurs_idle_request q={BASE,0,7};ps3_spurs_idle_result r;
    const uint32_t addresses[]={BASE+0x74,BASE+0xb0,BASE+0xd6c};
    for(unsigned i=0;i<3;++i){
        init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_read_address=addresses[i];
        ps3_spurs_idle_request_apply(&f.config,&q,&r);
        assert(r.stop==PS3_SPURS_IDLE_MEMORY&&r.memory_status==PS3_GUEST_MEMORY_INVALID_RANGE&&!r.has_sdk_result);
        assert(r.reads==i&&f.read_attempts==i&&f.scope_calls==i+1&&!r.idle_byte_write_attempted&&!r.writes);finish(&f);
    }
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_write=1;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);
    assert(r.stop==PS3_SPURS_IDLE_MEMORY&&r.memory_status==PS3_GUEST_MEMORY_INVALID_RANGE&&!r.has_sdk_result);
    assert(r.reads==3&&f.scope_calls==4&&!r.idle_byte_write_attempted&&!f.write_attempts&&f.ram[BASE+0x10]==3);finish(&f);
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_write=1;q.count=8;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x80410702u&&r.reads==1&&f.scope_calls==1&&!r.writes);finish(&f);
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_write=1;q.count=7;f.ram[BASE+0xb0]=0;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x80410705u&&r.reads==2&&f.scope_calls==2&&!r.writes);finish(&f);
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_write=1;f.ram[BASE+0xd6f]=1;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x8041070fu&&r.reads==3&&f.scope_calls==3&&!r.writes);finish(&f);
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.deny_write=1;f.ram[BASE+0x74]=0x40;q.wid=UINT32_MAX;q.count=UINT32_MAX;
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.has_sdk_result&&r.sdk_result==0x8041070fu&&r.reads==1&&f.scope_calls==1&&!r.writes);finish(&f);
    init(&f);f.config.memory_scope=(ps3_spurs_idle_memory_scope){&f,scoped_permits};f.cancel=1;q=(ps3_spurs_idle_request){BASE,0,7};
    ps3_spurs_idle_request_apply(&f.config,&q,&r);assert(r.stop==PS3_SPURS_IDLE_CANCELLED&&!f.scope_calls&&!r.reads);finish(&f);
}

int main(void){success_and_layout();precedence();failures();actual_reservation_notification();memory_scope_and_sdk_precedence();puts("SPURS_IDLE_REQUEST_TESTS_OK");return 0;}
