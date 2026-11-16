#define _POSIX_C_SOURCE 200809L
#include "../domain/spurs_workload_info.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { RAM_BYTES=0x8000,BASE=0x1000,OUT=0x5001,
    POLICY_NULL=0x80410811u,POLICY_ALIGN=0x80410810u,POLICY_INVAL=0x80410802u,POLICY_SRCH=0x80410805u };
typedef struct {
    unsigned char ram[RAM_BYTES];
    ps3_pthread_guard guard; ps3_atomic_domain domain;
    ps3_atomic_owner_record records[3]; ps3_atomic_owner owners[2];
    size_t operations,fail_operation;
} Fixture;
typedef struct { unsigned write; uint32_t address; size_t length; unsigned char bytes[8]; } Event;
typedef struct {
    Fixture *fixture; ps3_spurs_workload_info_config config; ps3_spurs_workload_info_request request;
    ps3_spurs_workload_info_result result; Event events[64]; size_t count,scope_calls;
    size_t reject,cancel_after,deny_operation;
    atomic_uint cancelled; unsigned mutate_capture,mutate_priority;
    uint32_t priority_address; pthread_barrier_t *barrier;
} Call;
typedef struct { uint32_t p,n,w; } Sources;
static int range(uint32_t address,uint64_t bytes) { return address<=RAM_BYTES && bytes<=RAM_BYTES-address; }
static ps3_guest_memory_status map_memory(void *context,uint32_t address,uint64_t bytes) {
    (void)context; return range(address,bytes)?PS3_GUEST_MEMORY_OK:PS3_GUEST_MEMORY_INVALID_RANGE;
}
static ps3_guest_memory_status read_memory(void *context,uint32_t address,void *out,size_t bytes) {
    Fixture *f=context;
    if (++f->operations==f->fail_operation) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memcpy(out,f->ram+address,bytes); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status write_memory(void *context,uint32_t address,const void *in,size_t bytes) {
    Fixture *f=context;
    if (++f->operations==f->fail_operation) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memcpy(f->ram+address,in,bytes); return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status zero_memory(void *context,uint32_t address,uint64_t bytes) {
    Fixture *f=context; if (!range(address,bytes)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    memset(f->ram+address,0,(size_t)bytes); return PS3_GUEST_MEMORY_OK;
}
static void put(Fixture *f,uint32_t address,size_t bytes,uint64_t value) {
    for (size_t i=0;i<bytes;++i) f->ram[address+bytes-i-1]=(unsigned char)(value>>(i*8));
}
static uint32_t get32(const Fixture *f,uint32_t address) {
    return (uint32_t)f->ram[address]<<24 | (uint32_t)f->ram[address+1]<<16 |
        (uint32_t)f->ram[address+2]<<8 | f->ram[address+3];
}
static void initialize(Fixture *f) {
    memset(f,0,sizeof(*f)); assert(!ps3_pthread_guard_init(&f->guard));
    ps3_guest_memory memory={f,map_memory,map_memory,read_memory,write_memory,zero_memory};
    assert(ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->guard),f->records,3,NULL,0)==PS3_GUEST_MEMORY_OK);
    for (unsigned i=0;i<2;++i) assert(ps3_atomic_owner_acquire(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
}
static Sources seed(Fixture *f,unsigned wid,unsigned flags32) {
    unsigned i=wid&15u;
    Sources s={BASE+(wid<16?0xB00:0x1000)+32*i,BASE+(wid<16?0xE00:0x1A00)+16*i,
        BASE+(wid<16?0x100:0x1200)+128*i};
    put(f,BASE+0x70,8,(uint64_t)(flags32?(1u<<i):(0x8000u>>wid))<<48 | (flags32?UINT64_C(0x40000000):0));
    put(f,BASE+0x78,2,1u<<i); put(f,BASE+0xB0,4,UINT32_C(0x80000000)>>wid);
    put(f,s.p,8,UINT64_C(0x1122334477889900)); put(f,s.p+8,8,UINT64_C(0x0102030405060708));
    put(f,s.p+0x10,4,0x2D80); put(f,s.p+0x18,8,UINT64_C(0xA1A2A3A4A5A6A7A8));
    put(f,s.n,8,UINT64_C(0x123456789ABCDEF0)); put(f,s.n+8,8,UINT64_C(0xFEDCBA980BADBEEF));
    put(f,s.w+0x30,8,UINT64_C(0xAA55CCDD12345678)); put(f,s.w+0x38,8,UINT64_C(0xEEFF001187654321));
    f->ram[BASE+0x20+i]=0xB4; f->ram[BASE+0x50+i]=0xC5; f->ram[BASE+0x40+i]=7;
    f->ram[BASE+i]=9; f->ram[BASE+0x10+i]=6;
    /* These values must not create a made-up STAT error. */
    put(f,BASE+0xD6C,4,1); f->ram[BASE+0x80+i]=0;
    memset(f->ram+OUT-1,0xA5,50); return s;
}
static void finish(Fixture *f) {
    for (unsigned i=0;i<2;++i) {
        assert(ps3_atomic_owner_read(&f->domain,f->owners[i],0,NULL,0)==PS3_GUEST_MEMORY_OK);
        assert(ps3_atomic_owner_release(&f->domain,&f->owners[i])==PS3_GUEST_MEMORY_OK);
    }
    assert(!ps3_pthread_guard_destroy(&f->guard));
}
static int poll(void *context) { return !atomic_load(&((Call *)context)->cancelled); }
static int permits(void *context,uint32_t address,size_t length,unsigned write) {
    Call *c=context; (void)address; (void)length; (void)write;
    return ++c->scope_calls!=c->deny_operation;
}
static void barrier_wait(pthread_barrier_t *barrier) {
    int status=pthread_barrier_wait(barrier); assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
}
static int observe(void *context,const ps3_spurs_workload_info_event *event) {
    Call *c=context; assert(c->count<64 && event->length<=8 && event->bytes);
    Event *copy=&c->events[c->count++];
    *copy=(Event){event->operation==PS3_SPURS_WORKLOAD_INFO_WRITE,event->address,event->length,{0}};
    memcpy(copy->bytes,event->bytes,event->length);
    assert(ps3_atomic_owner_read(&c->fixture->domain,c->fixture->owners[0],0,NULL,0)==PS3_GUEST_MEMORY_OK);
    if (c->mutate_capture && c->count==1) { c->request=(ps3_spurs_workload_info_request){0,UINT32_MAX,0}; c->config.domain=NULL; }
    if (c->mutate_priority && c->count==4) {
        const unsigned char changed[8]={0};
        assert(ps3_atomic_owner_write(&c->fixture->domain,c->fixture->owners[1],c->priority_address,changed,8)==PS3_GUEST_MEMORY_OK);
    }
    if (c->barrier && c->count==28) { barrier_wait(c->barrier); barrier_wait(c->barrier); }
    if (c->count==c->cancel_after) atomic_store(&c->cancelled,1);
    return c->count!=c->reject;
}
static void prepare(Call *c,Fixture *f,unsigned wid) {
    memset(c,0,sizeof(*c)); c->fixture=f; atomic_init(&c->cancelled,0);
    c->request=(ps3_spurs_workload_info_request){BASE,wid,OUT};
    c->config=(ps3_spurs_workload_info_config){.domain=&f->domain,.owner=f->owners[0],.instance=BASE,
        .owned_instance_bytes=0x2000,.active_owned_instance=1,.poll_context=c,.may_continue=poll,
        .observer_context=c,.observe=observe,.memory_scope={c,permits}};
}
static void apply(Call *c) { ps3_spurs_workload_info_apply(&c->config,&c->request,&c->result); }
static void sdk(const Call *c,uint32_t value) {
    assert(c->result.stop==PS3_SPURS_WORKLOAD_INFO_RUNNING && c->result.has_sdk_result && c->result.sdk_result==value);
}
static void operation(const Call *c,size_t index,unsigned write,uint32_t address,size_t length) {
    assert(index<c->count); const Event *e=&c->events[index];
    assert(e->write==write && e->address==address && e->length==length);
}
static void ledger(const Call *c,Sources s,unsigned flags32,unsigned wid) {
    size_t at=0; unsigned i=wid&15u; uint32_t out=c->request.info;
#define R(a,n) operation(c,at++,0,(a),(n))
#define W(a,n) operation(c,at++,1,(a),(n))
    R(BASE+0x70,8); R(BASE+0xB0,4); R(s.p+0x18,8);
    for (unsigned offset=15;offset>=8;--offset) W(out+offset,1);
    R(s.p+8,8); R(s.p+0x10,4); W(out,8); R(s.p,8); W(out+0x14,4);
    R(s.n+8,8); R(s.w+0x38,8); R(s.n,8); R(s.w+0x30,8);
    W(out+0x1C,4); W(out+0x10,4); W(out+0x18,4); W(out+0x28,4); W(out+0x2C,4); R(BASE+0x70,8);
    if (!flags32) {
        R(BASE+0x20+wid,1); W(out+0x20,1); R(BASE+0x50+wid,1); W(out+0x22,1);
        R(BASE+0x40+wid,1); W(out+0x21,1); R(BASE+wid,1); W(out+0x23,1);
        R(BASE+0x10+wid,1); W(out+0x24,1); R(BASE+0x70,2); W(out+0x25,1);
    } else {
        R(BASE+0x20+i,1); W(out+0x20,1); R(BASE+0x50+i,1); W(out+0x21,1); W(out+0x22,1);
        R(BASE+(wid<16?0:0x10)+i,1); W(out+0x24,1); W(out+0x23,1);
        R(BASE+(wid<16?0x70:0x78),2); W(out+0x25,1);
    }
    assert(at==c->count);
#undef R
#undef W
}
static void exact_results(void) {
    const unsigned wids[]={0,15,3,19,31}; const unsigned modes[]={0,0,1,1,1};
    for (unsigned which=0;which<5;++which) {
        Fixture f; initialize(&f); Sources s=seed(&f,wids[which],modes[which]); Call c; prepare(&c,&f,wids[which]);
        unsigned char expected[RAM_BYTES]; memcpy(expected,f.ram,sizeof(expected));
        const unsigned char header[]={1,2,3,4,5,6,7,8,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,
            0x77,0x88,0x99,0,0,0,0x2D,0x80,0x9A,0xBC,0xDE,0xF0,0x0B,0xAD,0xBE,0xEF};
        memcpy(expected+OUT,header,32);
        unsigned char counters[]={0xB4,7,0xC5,9,6,1};
        if (modes[which]) { counters[0]=wids[which]<16?4:11; counters[1]=0; counters[2]=wids[which]<16?5:12; counters[3]=wids[which]<16?9:6; counters[4]=0; }
        memcpy(expected+OUT+0x20,counters,6);
        const unsigned char hooks[]={0x12,0x34,0x56,0x78,0x87,0x65,0x43,0x21}; memcpy(expected+OUT+0x28,hooks,8);
        apply(&c); sdk(&c,0); ledger(&c,s,modes[which],wids[which]);
        assert(c.result.reads==(modes[which]?15u:17u) && c.result.writes==21 && c.result.write_attempts==21 && c.result.written_bytes==46);
        assert(!memcmp(expected,f.ram,sizeof(expected)) && c.result.observer_calls==c.count && c.result.publications==c.count);
        finish(&f);
    }
}
static void validation(void) {
    Fixture f; initialize(&f); seed(&f,0,0); Call c; uint32_t value;
    prepare(&c,&f,0); c.request.spurs=BASE+1; c.request.info=0; c.config.active_owned_instance=0;
    assert(ps3_spurs_workload_info_validate_address(&c.request,&value) && value==POLICY_NULL); apply(&c); sdk(&c,POLICY_NULL); assert(!c.count);
    prepare(&c,&f,0); c.request.spurs=0; apply(&c); sdk(&c,POLICY_NULL); assert(!c.count);
    prepare(&c,&f,0); c.request.spurs=BASE+1; apply(&c); sdk(&c,POLICY_ALIGN); assert(!c.count);
    prepare(&c,&f,16); apply(&c); sdk(&c,POLICY_INVAL); assert(c.count==1);
    prepare(&c,&f,UINT32_MAX); c.deny_operation=1; apply(&c); assert(c.result.stop==PS3_SPURS_WORKLOAD_INFO_MEMORY && !c.result.has_sdk_result);
    put(&f,BASE+0xB0,4,0); prepare(&c,&f,0); c.request.info=0xFFFFFFF0u; apply(&c); sdk(&c,POLICY_SRCH); assert(c.count==2 && !c.result.writes);
    prepare(&c,&f,0); c.config.active_owned_instance=0; apply(&c); assert(c.result.stop==PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE && !c.count);
    prepare(&c,&f,0); c.config.owner.generation++; apply(&c); assert(c.result.stop==PS3_SPURS_WORKLOAD_INFO_MEMORY && !c.count);
    seed(&f,19,1); prepare(&c,&f,19); c.config.owned_instance_bytes=0x1000; apply(&c);
    assert(c.result.stop==PS3_SPURS_WORKLOAD_INFO_MEMORY && c.count==2 && !c.result.writes && !c.result.has_sdk_result);
    assert(!ps3_spurs_workload_info_validate_address(NULL,&value)); finish(&f);
}
static void prefix_result(const Call *c,const Event *gold,size_t operations,const unsigned char *before,unsigned attempted_extra) {
    unsigned char expected[RAM_BYTES]; memcpy(expected,before,sizeof(expected)); size_t reads=0,writes=0,bytes=0;
    for (size_t i=0;i<operations;++i) {
        assert(c->events[i].write==gold[i].write && c->events[i].address==gold[i].address && c->events[i].length==gold[i].length);
        assert(!memcmp(c->events[i].bytes,gold[i].bytes,gold[i].length));
        if (gold[i].write) { ++writes; bytes+=gold[i].length; memcpy(expected+gold[i].address,gold[i].bytes,gold[i].length); } else ++reads;
    }
    assert(c->count==operations && c->result.reads==reads && c->result.writes==writes && c->result.written_bytes==bytes);
    assert(c->result.write_attempts==writes+attempted_extra && !c->result.has_sdk_result);
    assert(!memcmp(expected,c->fixture->ram,sizeof(expected)));
}
static void partial_prefixes(void) {
    Fixture gold_fixture; initialize(&gold_fixture); seed(&gold_fixture,3,0); Call golden; prepare(&golden,&gold_fixture,3);
    unsigned char before[RAM_BYTES]; memcpy(before,gold_fixture.ram,sizeof(before)); apply(&golden); sdk(&golden,0);
    for (unsigned kind=0;kind<4;++kind) for (size_t at=1;at<=golden.count;++at) {
        Fixture f; initialize(&f); seed(&f,3,0); Call c; prepare(&c,&f,3);
        if (kind==0) f.fail_operation=at;
        else if (kind==1) c.deny_operation=at;
        else if (kind==2) c.reject=at;
        else c.cancel_after=at;
        apply(&c); assert(c.result.stop==(kind<2?PS3_SPURS_WORKLOAD_INFO_MEMORY:kind==2?PS3_SPURS_WORKLOAD_INFO_OBSERVER:PS3_SPURS_WORKLOAD_INFO_CANCELLED));
        const size_t committed=kind<2?at-1:at;
        prefix_result(&c,golden.events,committed,before,kind==0 && golden.events[at-1].write);
        assert(c.result.observer_calls==committed && c.result.publications==committed-(kind==2));
        if (kind==0) assert(c.result.memory_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE);
        finish(&f);
    }
    Fixture f; initialize(&f); seed(&f,3,0); Call c; prepare(&c,&f,3); atomic_store(&c.cancelled,1); apply(&c);
    assert(c.result.stop==PS3_SPURS_WORKLOAD_INFO_CANCELLED && !c.count && !c.scope_calls); finish(&f); finish(&gold_fixture);
}
static void aliases_and_capture(void) {
    Fixture f; initialize(&f); Sources s=seed(&f,3,0); Call c; prepare(&c,&f,3); c.request.info=s.p+8;
    apply(&c); sdk(&c,0); assert(get32(&f,c.request.info+0x14)==0xA1A2A3A4); ledger(&c,s,0,3); finish(&f);
    initialize(&f); s=seed(&f,3,0); prepare(&c,&f,3); c.request.info=BASE+0x64;
    apply(&c); sdk(&c,0); ledger(&c,s,1,3);
    assert(!(c.events[0].bytes[4]&0x40) && (c.events[25].bytes[4]&0x40));
    assert(f.ram[c.request.info+0x20]==4 && !f.ram[c.request.info+0x21] && f.ram[c.request.info+0x22]==5); finish(&f);
    initialize(&f); s=seed(&f,19,1); put(&f,s.p,8,0x00112233); prepare(&c,&f,19); c.request.info=BASE+0x64;
    apply(&c); sdk(&c,0); ledger(&c,s,0,19); assert(!f.ram[c.request.info+0x25]); finish(&f);
    initialize(&f); s=seed(&f,3,0); prepare(&c,&f,3); c.request.info=BASE+0x13;
    apply(&c); sdk(&c,0); assert(f.ram[c.request.info+0x20]==0x77 && f.ram[c.request.info+0x24]==1); finish(&f);
    initialize(&f); s=seed(&f,3,0); prepare(&c,&f,3); c.mutate_priority=1; c.priority_address=s.p+0x18;
    apply(&c); sdk(&c,0); const unsigned char priority[]={0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8};
    assert(!memcmp(f.ram+OUT+8,priority,8)); finish(&f);
    initialize(&f); seed(&f,3,0); prepare(&c,&f,3); c.mutate_capture=1; apply(&c); sdk(&c,0); assert(c.result.writes==21); finish(&f);
}
typedef struct { Fixture *fixture; pthread_barrier_t *barrier; } Concurrent;
static void *change_counter(void *context) {
    Concurrent *concurrent=context; barrier_wait(concurrent->barrier);
    const unsigned char value=0x42;
    assert(ps3_atomic_owner_write(&concurrent->fixture->domain,concurrent->fixture->owners[1],BASE+0x53,&value,1)==PS3_GUEST_MEMORY_OK);
    barrier_wait(concurrent->barrier); return NULL;
}
static void concurrent_counter_changes(void) {
    Fixture f; initialize(&f); seed(&f,3,0); Call c; prepare(&c,&f,3); pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier,NULL,2)); c.barrier=&barrier;
    Concurrent concurrent={&f,&barrier}; pthread_t thread; assert(!pthread_create(&thread,NULL,change_counter,&concurrent));
    apply(&c); assert(!pthread_join(thread,NULL)); sdk(&c,0);
    assert(f.ram[OUT+0x20]==0xB4 && f.ram[OUT+0x22]==0x42); assert(!pthread_barrier_destroy(&barrier)); finish(&f);
}
int main(void) {
    exact_results(); validation(); partial_prefixes(); aliases_and_capture(); concurrent_counter_changes();
    puts("SPURS_WORKLOAD_INFO_PASS exact_ledger=1 stores21_bytes46=1 padding_untouched=1 aliases=1 partial_prefixes=1 concurrent_counter_change=1");
    return 0;
}
