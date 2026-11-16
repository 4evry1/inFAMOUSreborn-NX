#define _GNU_SOURCE
#include "../adapters/heap_guest_space.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct allocation_record { void *pointer; size_t size; } allocation_record;
typedef struct fixture {
    pthread_mutex_t mutex;
    size_t attempts, fail_at, live, bytes;
    allocation_record records[128];
} fixture;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "HEAP_GUEST_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
static void lock(void *p) { if (pthread_mutex_lock(p)) abort(); }
static void unlock(void *p) { if (pthread_mutex_unlock(p)) abort(); }
static void *allocate(void *p, size_t size)
{
    fixture *f = p;
    if (++f->attempts == f->fail_at) return NULL;
    void *memory = malloc(size);
    if (!memory) return NULL;
    for (size_t i = 0; i < 128; ++i) if (!f->records[i].pointer) {
        f->records[i] = (allocation_record){memory, size};
        ++f->live; f->bytes += size;
        return memory;
    }
    abort();
}
static void deallocate(void *p, void *memory)
{
    fixture *f = p;
    for (size_t i = 0; i < 128; ++i) if (f->records[i].pointer == memory) {
        --f->live; f->bytes -= f->records[i].size;
        f->records[i] = (allocation_record){0}; free(memory); return;
    }
    abort();
}
static ps3_heap_guest_space *create(fixture *f, uint64_t quota)
{
    ps3_heap_guest_space *s = NULL;
    ps3_heap_guest_allocator a = {f, allocate, deallocate};
    ps3_instance_guard g = {&f->mutex, lock, unlock};
    CHECK(ps3_heap_guest_space_create(&s, a, g, quota) == PS3_GUEST_MEMORY_OK);
    return s;
}
static void basic_and_failure(void)
{
    fixture f = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_space *s = create(&f, 32);
    ps3_guest_memory m = ps3_heap_guest_space_port(s);
    unsigned char in[64], out[64]; memset(in, 0xAB, sizeof(in)); memset(out, 0x55, sizeof(out));
    CHECK(m.commit(m.context, 1, 4096) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(m.commit(m.context, 0, 0) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(m.commit(m.context, 0xFFFFF000, 8192) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(m.read(m.context, 0xFFFFFFF0, out, 64) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(m.read(m.context, 0x1000, out, 64) == PS3_GUEST_MEMORY_UNMAPPED);
    for (size_t i=0;i<64;++i) CHECK(out[i] == 0x55);
    CHECK(m.commit(m.context, 0xF000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(m.write(m.context, 0xFFF0, in, 64) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(m.read(m.context, 0xFFF0, out, 16) == PS3_GUEST_MEMORY_OK);
    for (size_t i=0;i<16;++i) CHECK(out[i] == 0);
    CHECK(m.write(m.context, 0xFFF0, in, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(m.zero(m.context, 0xFFF0, 64) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(m.read(m.context, 0xFFF0, out, 16) == PS3_GUEST_MEMORY_OK);
    for (size_t i=0;i<16;++i) CHECK(out[i] == 0xAB);
    CHECK(m.write(m.context, 0xF000, in, 64) == PS3_GUEST_MEMORY_OK);
    size_t before_bytes = f.bytes, before_live = f.live, before_attempts = f.attempts;
    f.fail_at = f.attempts + 2; /* First new block succeeds, second allocation fails. */
    CHECK(m.commit(m.context, 0xE000, 0x13000) == PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(f.attempts == before_attempts + 2);
    CHECK(f.bytes == before_bytes && f.live == before_live);
    CHECK(ps3_heap_guest_space_stats(s).committed_pages == 1);
    CHECK(m.read(m.context, 0xE000, out, 64) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(m.read(m.context, 0xF000, out, 64) == PS3_GUEST_MEMORY_OK);
    CHECK(!memcmp(in, out, 64));
    f.fail_at = 0;
    CHECK(m.commit(m.context, 0xF000, 8192) == PS3_GUEST_MEMORY_OK);
    CHECK(m.write(m.context, 0xFFF0, in, 64) == PS3_GUEST_MEMORY_OK);
    CHECK(m.commit(m.context, 0xF000, 8192) == PS3_GUEST_MEMORY_OK);
    CHECK(m.read(m.context, 0xFFF0, out, 64) == PS3_GUEST_MEMORY_OK && !memcmp(in,out,64));
    CHECK(m.zero(m.context, 0xFFF8, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(m.read(m.context, 0xFFF0, out, 64) == PS3_GUEST_MEMORY_OK);
    for (size_t i=0;i<64;++i) CHECK(out[i] == (i>=8 && i<40 ? 0 : 0xAB));
    CHECK(m.decommit(m.context, 0xF000, 4096) == PS3_GUEST_MEMORY_OK);
    memset(out,0x55,64);
    CHECK(m.read(m.context, 0xFFF0, out, 64) == PS3_GUEST_MEMORY_UNMAPPED);
    for (size_t i=0;i<64;++i) CHECK(out[i] == 0x55);
    CHECK(m.commit(m.context, 0xF000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(m.read(m.context, 0xF000, out, 64) == PS3_GUEST_MEMORY_OK);
    for (size_t i=0;i<64;++i) CHECK(out[i] == 0);
    CHECK(m.commit(m.context, 0xFFFFF000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(m.commit(m.context, 0xD0010000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(m.write(m.context, 0xFFFFFFC0, in, 64) == PS3_GUEST_MEMORY_OK);
    CHECK(m.read(m.context, 0xFFFFFFC0, out, 64) == PS3_GUEST_MEMORY_OK && !memcmp(in,out,64));
    ps3_heap_guest_stats stats = ps3_heap_guest_space_stats(s);
    CHECK(stats.committed_pages == 4 && stats.backing_blocks == 4);
    CHECK(stats.metadata_bytes + stats.backing_bytes == f.bytes);
    printf("HEAP_GUEST_STATS logical_bytes=%llu metadata_bytes=%llu backing_bytes_including_headers=%llu blocks=%llu\n",
        (unsigned long long)(stats.committed_pages*4096), (unsigned long long)stats.metadata_bytes,
        (unsigned long long)stats.backing_bytes, (unsigned long long)stats.backing_blocks);
    CHECK(m.commit(m.context,0,33*4096) == PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(ps3_heap_guest_space_stats(s).committed_pages == 4);
    CHECK(ps3_heap_guest_space_destroy(&s) == PS3_GUEST_MEMORY_OK && !s);
    CHECK(!f.live && !f.bytes);
    pthread_mutex_destroy(&f.mutex);
}
static void independent_and_quota(void)
{
    fixture a = {.mutex=PTHREAD_MUTEX_INITIALIZER}, b = {.mutex=PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_space *x=create(&a,1), *y=create(&b,1);
    ps3_guest_memory p=ps3_heap_guest_space_port(x),q=ps3_heap_guest_space_port(y);
    unsigned char value=9, out=0;
    CHECK(p.commit(p.context,0,4096)==PS3_GUEST_MEMORY_OK);
    CHECK(q.commit(q.context,0,4096)==PS3_GUEST_MEMORY_OK);
    CHECK(p.write(p.context,0,&value,1)==PS3_GUEST_MEMORY_OK);
    CHECK(q.read(q.context,0,&out,1)==PS3_GUEST_MEMORY_OK && out==0);
    CHECK(p.commit(p.context,0,4096)==PS3_GUEST_MEMORY_OK);
    CHECK(p.commit(p.context,4096,4096)==PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(ps3_heap_guest_space_destroy(&x)==PS3_GUEST_MEMORY_OK);
    CHECK(q.read(q.context,0,&out,1)==PS3_GUEST_MEMORY_OK && out==0);
    CHECK(ps3_heap_guest_space_destroy(&y)==PS3_GUEST_MEMORY_OK);
    CHECK(!a.live && !b.live); pthread_mutex_destroy(&a.mutex); pthread_mutex_destroy(&b.mutex);
}
static unsigned char *find_owned_payload(fixture *f, const unsigned char *pattern, size_t length)
{
    for (size_t i=0;i<128;++i) {
        allocation_record record=f->records[i];
        if (record.size < 65536 || record.size > 66000) continue;
        for (size_t offset=0;offset+length<=128;++offset)
            if (!memcmp((unsigned char *)record.pointer+offset,pattern,length))
                return (unsigned char *)record.pointer+offset;
    }
    return NULL;
}
static void overlap(void)
{
    fixture f={.mutex=PTHREAD_MUTEX_INITIALIZER}; ps3_heap_guest_space *s=create(&f,32);
    ps3_guest_memory m=ps3_heap_guest_space_port(s);
    unsigned char pattern[512], expected[512], output[512];
    for(size_t i=0;i<sizeof(pattern);++i) pattern[i]=(unsigned char)(i*13+11);
    CHECK(m.commit(m.context,0,0x11000)==PS3_GUEST_MEMORY_OK);
    CHECK(m.write(m.context,0,pattern,sizeof(pattern))==PS3_GUEST_MEMORY_OK);
    /* The allocator owns these host allocations; no guest-pointer API is used. */
    unsigned char *payload=find_owned_payload(&f,pattern,64); CHECK(payload!=NULL);
    if(payload) {
        memcpy(expected,pattern,sizeof(pattern)); memmove(expected+1,expected,256);
        CHECK(m.write(m.context,1,payload,256)==PS3_GUEST_MEMORY_OK);
        CHECK(m.read(m.context,0,output,sizeof(output))==PS3_GUEST_MEMORY_OK && !memcmp(output,expected,sizeof(output)));
        CHECK(m.write(m.context,0,pattern,sizeof(pattern))==PS3_GUEST_MEMORY_OK);
        CHECK(m.read(m.context,0,payload+1,256)==PS3_GUEST_MEMORY_OK);
        CHECK(!memcmp(payload,expected,sizeof(expected)));
        CHECK(m.write(m.context,0,pattern,sizeof(pattern))==PS3_GUEST_MEMORY_OK);
        size_t bytes=f.bytes; f.fail_at=f.attempts+1;
        CHECK(m.write(m.context,1,payload,256)==PS3_GUEST_MEMORY_NO_MEMORY);
        CHECK(!memcmp(payload,pattern,sizeof(pattern)) && f.bytes==bytes);
        f.fail_at=f.attempts+1;
        CHECK(m.read(m.context,0,payload+1,256)==PS3_GUEST_MEMORY_NO_MEMORY);
        CHECK(!memcmp(payload,pattern,sizeof(pattern)) && f.bytes==bytes);
        f.fail_at=0;
        CHECK(m.write(m.context,0xFF80,payload,256)==PS3_GUEST_MEMORY_OK);
        CHECK(m.read(m.context,0xFF80,output,256)==PS3_GUEST_MEMORY_OK && !memcmp(output,pattern,256));
    }
    CHECK(ps3_heap_guest_space_destroy(&s)==PS3_GUEST_MEMORY_OK && !f.live);
    pthread_mutex_destroy(&f.mutex);
}
static void invalid_and_create_failure(void)
{
    fixture f={.mutex=PTHREAD_MUTEX_INITIALIZER};
    ps3_heap_guest_allocator a={&f,allocate,deallocate}; ps3_instance_guard g={&f.mutex,lock,unlock};
    ps3_heap_guest_space *s=NULL;
    CHECK(ps3_heap_guest_space_create(NULL,a,g,1)==PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(ps3_heap_guest_space_create(&s,a,g,0)==PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(ps3_heap_guest_space_create(&s,a,g,1048577)==PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(!s && !f.live);
    f.fail_at=f.attempts+1;
    CHECK(ps3_heap_guest_space_create(&s,a,g,1)==PS3_GUEST_MEMORY_NO_MEMORY);
    CHECK(!s && !f.live);
    CHECK(ps3_heap_guest_space_destroy(&s)==PS3_GUEST_MEMORY_INVALID_RANGE);
    pthread_mutex_destroy(&f.mutex);
}
typedef struct concurrent { ps3_guest_memory memory; int failed; } concurrent;
static void *reader(void *p)
{
    concurrent *c=p;
    for(unsigned n=0;n<2000;++n) {
        unsigned char out[64]; memset(out,0x55,sizeof(out));
        ps3_guest_memory_status status=c->memory.read(c->memory.context,0x1000,out,sizeof(out));
        if(status!=PS3_GUEST_MEMORY_OK && status!=PS3_GUEST_MEMORY_UNMAPPED) c->failed=1;
        for(size_t i=0;i<sizeof(out);++i) if(out[i]!=(status==PS3_GUEST_MEMORY_OK?0:0x55)) c->failed=1;
    }
    return NULL;
}
static void concurrency(void)
{
    fixture f={.mutex=PTHREAD_MUTEX_INITIALIZER}; ps3_heap_guest_space *s=create(&f,16);
    ps3_guest_memory m=ps3_heap_guest_space_port(s); concurrent c={m,0}; pthread_t t;
    CHECK(!pthread_create(&t,NULL,reader,&c));
    for(unsigned n=0;n<2000;++n) { CHECK(m.commit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK); CHECK(m.decommit(m.context,0x1000,4096)==PS3_GUEST_MEMORY_OK); }
    CHECK(!pthread_join(t,NULL)); CHECK(!c.failed);
    CHECK(ps3_heap_guest_space_destroy(&s)==PS3_GUEST_MEMORY_OK); CHECK(!f.live);
    pthread_mutex_destroy(&f.mutex);
}
typedef struct transfer_gate {
    pthread_mutex_t *backing;
    pthread_mutex_t coordination;
    pthread_cond_t condition;
    int arm, transfer_held, release_transfer, decommit_attempted, decommit_finished;
    ps3_guest_memory memory;
    ps3_guest_memory_status read_status, decommit_status;
    unsigned char output[64];
} transfer_gate;
static struct timespec deadline(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME,&value)) abort();
    value.tv_sec+=5;
    return value;
}
static void wait_flag(transfer_gate *gate, int *flag)
{
    struct timespec until=deadline();
    while(!*flag) if(pthread_cond_timedwait(&gate->condition,&gate->coordination,&until)) abort();
}
static void gated_lock(void *context)
{
    transfer_gate *gate=context;
    lock(&gate->coordination);
    if(gate->transfer_held && !gate->release_transfer) {
        gate->decommit_attempted=1;
        pthread_cond_broadcast(&gate->condition);
    }
    unlock(&gate->coordination);
    lock(gate->backing);
    lock(&gate->coordination);
    if(gate->arm) {
        gate->arm=0; gate->transfer_held=1;
        pthread_cond_broadcast(&gate->condition);
        wait_flag(gate,&gate->release_transfer);
    }
    unlock(&gate->coordination);
}
static void gated_unlock(void *context)
{
    transfer_gate *gate=context;
    unlock(gate->backing);
}
static void *gated_reader(void *context)
{
    transfer_gate *gate=context;
    /* Test coordination cannot cancel a worker while it owns the real guard. */
    if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL)) abort();
    gate->read_status=gate->memory.read(gate->memory.context,0,gate->output,64);
    return NULL;
}
static void *gated_decommit(void *context)
{
    transfer_gate *gate=context;
    if(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL)) abort();
    gate->decommit_status=gate->memory.decommit(gate->memory.context,0,4096);
    lock(&gate->coordination); gate->decommit_finished=1;
    pthread_cond_broadcast(&gate->condition); unlock(&gate->coordination);
    return NULL;
}
static void controlled_overlap(void)
{
    fixture f={.mutex=PTHREAD_MUTEX_INITIALIZER};
    transfer_gate gate={.backing=&f.mutex,.coordination=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    ps3_heap_guest_space *space=NULL;
    ps3_heap_guest_allocator allocator={&f,allocate,deallocate};
    ps3_instance_guard guard={&gate,gated_lock,gated_unlock};
    CHECK(ps3_heap_guest_space_create(&space,allocator,guard,1)==PS3_GUEST_MEMORY_OK);
    gate.memory=ps3_heap_guest_space_port(space);
    unsigned char input[64]; memset(input,0x37,sizeof(input));
    CHECK(gate.memory.commit(gate.memory.context,0,4096)==PS3_GUEST_MEMORY_OK);
    CHECK(gate.memory.write(gate.memory.context,0,input,64)==PS3_GUEST_MEMORY_OK);
    gate.arm=1;
    pthread_t reading, retiring;
    if(pthread_create(&reading,NULL,gated_reader,&gate)) abort();
    lock(&gate.coordination); wait_flag(&gate,&gate.transfer_held); unlock(&gate.coordination);
    if(pthread_create(&retiring,NULL,gated_decommit,&gate)) abort();
    lock(&gate.coordination); wait_flag(&gate,&gate.decommit_attempted);
    CHECK(!gate.decommit_finished);
    gate.release_transfer=1; pthread_cond_broadcast(&gate.condition); unlock(&gate.coordination);
    struct timespec until=deadline(); if(pthread_timedjoin_np(reading,NULL,&until)) abort();
    until=deadline(); if(pthread_timedjoin_np(retiring,NULL,&until)) abort();
    CHECK(gate.read_status==PS3_GUEST_MEMORY_OK && !memcmp(gate.output,input,64));
    CHECK(gate.decommit_status==PS3_GUEST_MEMORY_OK && gate.decommit_finished);
    memset(gate.output,0x55,64);
    CHECK(gate.memory.read(gate.memory.context,0,gate.output,64)==PS3_GUEST_MEMORY_UNMAPPED);
    for(size_t i=0;i<64;++i) CHECK(gate.output[i]==0x55);
    CHECK(ps3_heap_guest_space_destroy(&space)==PS3_GUEST_MEMORY_OK && !f.live);
    pthread_cond_destroy(&gate.condition); pthread_mutex_destroy(&gate.coordination); pthread_mutex_destroy(&f.mutex);
    printf("HEAP_GUEST_OVERLAP held_transfer=true retirement_attempted=true retirement_blocked=true joined=true\n");
}
static void coherent_domain(void)
{
    fixture f={.mutex=PTHREAD_MUTEX_INITIALIZER}; pthread_mutex_t domain_mutex=PTHREAD_MUTEX_INITIALIZER;
    ps3_heap_guest_space *s=create(&f,32); ps3_atomic_domain domain={0};
    ps3_atomic_owner_record owners[2]={0}; ps3_atomic_pin_record pins[2]={0};
    ps3_instance_guard guard={&domain_mutex,lock,unlock};
    CHECK(ps3_atomic_domain_init(&domain,ps3_heap_guest_space_port(s),guard,owners,2,pins,2)==PS3_GUEST_MEMORY_OK);
    ps3_guest_memory memory=ps3_atomic_domain_memory(&domain); ps3_atomic_owner owner={0}; ps3_atomic_pin pin={0};
    CHECK(ps3_atomic_owner_acquire(&domain,&owner)==PS3_GUEST_MEMORY_OK);
    CHECK(memory.commit(memory.context,0,8192)==PS3_GUEST_MEMORY_OK);
    uint32_t value=99; int stored=0; unsigned char bytes[4]={0};
    CHECK(ps3_atomic_load_reserve32(&domain,owner,0,&value)==PS3_GUEST_MEMORY_OK && value==0);
    CHECK(ps3_atomic_store_conditional32(&domain,owner,0,0x11223344,&stored)==PS3_GUEST_MEMORY_OK && stored);
    CHECK(memory.read(memory.context,0,bytes,4)==PS3_GUEST_MEMORY_OK);
    CHECK(bytes[0]==0x11 && bytes[1]==0x22 && bytes[2]==0x33 && bytes[3]==0x44);
    CHECK(ps3_atomic_load_reserve32(&domain,owner,0,&value)==PS3_GUEST_MEMORY_OK && value==0x11223344);
    unsigned char changed=0x99;
    CHECK(memory.write(memory.context,64,&changed,1)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_store_conditional32(&domain,owner,0,0x55667788,&stored)==PS3_GUEST_MEMORY_OK && !stored);
    CHECK(ps3_atomic_pin_acquire(&domain,4090,12,&pin)==PS3_GUEST_MEMORY_OK);
    CHECK(memory.decommit(memory.context,0,8192)==PS3_GUEST_MEMORY_BUSY);
    CHECK(ps3_heap_guest_space_stats(s).committed_pages==2);
    CHECK(ps3_atomic_pin_release(&domain,&pin)==PS3_GUEST_MEMORY_OK);
    CHECK(memory.decommit(memory.context,0,8192)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_owner_release(&domain,&owner)==PS3_GUEST_MEMORY_OK);
    CHECK(ps3_heap_guest_space_destroy(&s)==PS3_GUEST_MEMORY_OK && !f.live);
    pthread_mutex_destroy(&domain_mutex); pthread_mutex_destroy(&f.mutex);
}
int main(void)
{
    invalid_and_create_failure(); basic_and_failure(); independent_and_quota(); overlap(); concurrency(); controlled_overlap(); coherent_domain();
    printf("HEAP_GUEST_%s failures=%d\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
