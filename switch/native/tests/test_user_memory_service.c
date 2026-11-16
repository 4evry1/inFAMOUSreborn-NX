#include "user_memory_service.h"
#include "guest_space_host.h"
#include "guard_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    ps3_guest_memory backing;
    int fail_write, fail_read, fail_commit, fail_decommit, race;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered, released;
} Faults;
static ps3_guest_memory_status commit(void *p, uint32_t a, uint64_t n) {
    Faults *f=p;
    if(f->fail_commit) return PS3_GUEST_MEMORY_NO_MEMORY;
    return f->backing.commit(f->backing.context,a,n);
}
static ps3_guest_memory_status decommit(void *p, uint32_t a, uint64_t n) {
    Faults *f=p;
    if(f->fail_decommit) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.decommit(f->backing.context,a,n);
}
static ps3_guest_memory_status read_memory(void *p,uint32_t a,void *v,size_t n) {
    Faults *f=p;
    if(f->fail_read) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.read(f->backing.context,a,v,n);
}
static ps3_guest_memory_status write_memory(void *p,uint32_t a,const void *v,size_t n) {
    Faults *f=p;
    if(f->fail_write) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    return f->backing.write(f->backing.context,a,v,n);
}
static ps3_guest_memory_status zero(void *p,uint32_t a,uint64_t n) {
    Faults *f=p; return f->backing.zero(f->backing.context,a,n);
}
typedef struct {
    ElfTestHostSpace *space;
    Faults faults;
    ps3_pthread_guard domain_guard, allocator_guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[2];
    ps3_atomic_pin_record pins[4];
    ps3_user_memory allocator;
    ps3_user_memory_slot slots[4];
    ps3_user_memory_service service;
} Fixture;
static void allocator_lock(void *p) {
    Fixture *f=p; assert(!pthread_mutex_lock(&f->allocator_guard.mutex));
}
static void allocator_unlock(void *p) {
    Fixture *f=p;
    assert(!pthread_mutex_unlock(&f->allocator_guard.mutex));
    if(f->faults.race) {
        assert(!pthread_mutex_lock(&f->faults.mutex));
        f->faults.entered=1; assert(!pthread_cond_broadcast(&f->faults.condition));
        while(!f->faults.released) assert(!pthread_cond_wait(&f->faults.condition,&f->faults.mutex));
        assert(!pthread_mutex_unlock(&f->faults.mutex));
    }
}
static void init(Fixture *f) {
    memset(f,0,sizeof(*f));
    f->space=elf_test_host_space_create(4*1048576); assert(f->space);
    f->faults.backing=elf_test_host_space_port(f->space);
    assert(!pthread_mutex_init(&f->faults.mutex,NULL));
    assert(!pthread_cond_init(&f->faults.condition,NULL));
    assert(!ps3_pthread_guard_init(&f->domain_guard));
    assert(!ps3_pthread_guard_init(&f->allocator_guard));
    ps3_guest_memory memory={&f->faults,commit,decommit,read_memory,write_memory,zero};
    assert(!ps3_atomic_domain_init(&f->domain,memory,ps3_pthread_guard_port(&f->domain_guard),f->owners,2,f->pins,4));
    ps3_user_memory_config config={1048576,4*1048576,2*1048576,
        ps3_atomic_domain_memory(&f->domain),{f,allocator_lock,allocator_unlock}};
    assert(!ps3_user_memory_init(&f->allocator,&config,f->slots,4));
    f->service=(ps3_user_memory_service){&f->allocator,&f->domain};
    assert(!ps3_atomic_commit(&f->domain,0,8192));
    unsigned char bytes[8]; memset(bytes,0xa5,sizeof bytes);
    assert(!ps3_atomic_write(&f->domain,4092,bytes,sizeof bytes));
}
static uint64_t used(Fixture *f) {
    ps3_user_memory_info info; assert(!ps3_user_memory_query(&f->allocator,&info)); return info.used_bytes;
}
static void unchanged(Fixture *f) {
    unsigned char bytes[8]; assert(!ps3_atomic_read(&f->domain,4092,bytes,8));
    for(unsigned i=0;i<8;++i) assert(bytes[i]==0xa5);
}
static void finish(Fixture *f) {
    f->faults.fail_decommit=0;
    assert(!ps3_user_memory_cleanup(&f->allocator).status);
    for(unsigned i=0;i<4;++i) assert(!f->pins[i].active);
    assert(!ps3_pthread_guard_destroy(&f->allocator_guard));
    assert(!ps3_pthread_guard_destroy(&f->domain_guard));
    assert(!pthread_cond_destroy(&f->faults.condition));
    assert(!pthread_mutex_destroy(&f->faults.mutex));
    elf_test_host_space_destroy(f->space);
}
static void *publish(void *p) {
    Fixture *f=p;
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL));
    ps3_user_memory_service_result r=ps3_user_memory_query_and_publish_be32(&f->service,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OK); return NULL;
}
static void *allocate_publish(void *p) {
    Fixture *f=p;
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL));
    ps3_user_memory_service_result r=ps3_user_memory_allocate_and_publish_be32(&f->service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OK && r.published && !r.retained_pin);
    return NULL;
}
int main(void) {
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL));
    Fixture f; init(&f);
    ps3_user_memory_service_result r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OK && r.published && !r.retained_pin && used(&f)==65536);
    unsigned char bytes[8]; assert(!ps3_atomic_read(&f.domain,4092,bytes,4));
    assert(!memcmp(bytes,"\x00\x10\x00\x00",4));
    assert(!ps3_user_memory_free_owned(&f.service,1048576).status);
    assert(!ps3_user_memory_query_and_publish_be32(&f.service,4092).status);
    assert(!ps3_atomic_read(&f.domain,4092,bytes,8));
    assert(!memcmp(bytes,"\x00\x20\x00\x00\x00\x20\x00\x00",8));
    finish(&f);

    init(&f); f.faults.fail_write=1;
    r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE && !r.published && !r.retained_allocation && used(&f)==0);
    assert(r.memory_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE); unchanged(&f);
    f.faults.fail_decommit=1;
    r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_ROLLBACK_FAILURE && r.retained_allocation);
    assert(r.retained_address==1048576 && used(&f)==65536);
    assert(r.rollback.memory_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE); unchanged(&f);
    f.faults.fail_decommit=0;
    assert(!ps3_user_memory_free_owned(&f.service,r.retained_address).status && used(&f)==0);
    assert(ps3_user_memory_query_and_publish_be32(&f.service,4092).status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE);
    unchanged(&f); finish(&f);

    init(&f);
    assert(ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,8192).status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE);
    assert(ps3_user_memory_query_and_publish_be32(&f.service,UINT64_MAX).status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE);
    assert(ps3_user_memory_query_and_publish_be32(&f.service,UINT32_MAX-3ULL).status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE);
    assert(ps3_user_memory_allocate_and_publish_be32(&f.service,1,65536,4092).status==PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE);
    assert(ps3_user_memory_allocate_and_publish_be32(&f.service,3*1048576,65536,4092).allocator.status==PS3_USER_MEMORY_QUOTA_EXCEEDED);
    f.faults.fail_read=1;
    r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE && r.memory_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    f.faults.fail_read=0;
    f.faults.fail_commit=1;
    r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE && r.allocator.memory_status==PS3_GUEST_MEMORY_NO_MEMORY);
    assert(used(&f)==0); unchanged(&f); finish(&f);

    init(&f);
    assert(!ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092).status);
    f.faults.fail_decommit=1;
    r=ps3_user_memory_free_owned(&f.service,1048576);
    assert(r.status==PS3_USER_MEMORY_SERVICE_ALLOCATOR_FAILURE && r.retained_allocation && r.retained_address==1048576 && used(&f)==65536);
    assert(r.allocator.memory_status==PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    f.faults.fail_decommit=0;
    ps3_atomic_pin live;
    assert(!ps3_atomic_pin_acquire(&f.domain,1048576,1,&live));
    r=ps3_user_memory_free_owned(&f.service,1048576);
    assert(r.allocator.memory_status==PS3_GUEST_MEMORY_BUSY && used(&f)==65536);
    assert(!ps3_atomic_pin_release(&f.domain,&live));
    assert(!ps3_user_memory_free_owned(&f.service,1048576).status && used(&f)==0);
    finish(&f);

    init(&f); assert(!ps3_atomic_decommit(&f.domain,4096,4096));
    assert(ps3_user_memory_query_and_publish_be32(&f.service,4092).status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE);
    assert(used(&f)==0); finish(&f);

    init(&f);
    /* Valid core configuration can represent a 4 GiB quota even with bounded
     * host backing: query must not truncate that quota to a guest BE32. */
    ps3_user_memory_config wide={0,UINT64_C(0x100000000),UINT64_C(0x100000000),
        ps3_atomic_domain_memory(&f.domain),{&f,allocator_lock,allocator_unlock}};
    ps3_user_memory wide_allocator; ps3_user_memory_slot wide_slots[2];
    assert(!ps3_user_memory_init(&wide_allocator,&wide,wide_slots,2));
    ps3_user_memory_service wide_service={&wide_allocator,&f.domain};
    assert(ps3_user_memory_query_and_publish_be32(&wide_service,4092).status==PS3_USER_MEMORY_SERVICE_OVERFLOW);
    unchanged(&f);
    assert(ps3_user_memory_free_owned(&f.service,1048576).allocator.status==PS3_USER_MEMORY_NOT_OWNED);
    assert(ps3_user_memory_query_and_publish_be32(NULL,4092).status==PS3_USER_MEMORY_SERVICE_ARGUMENT);
    ps3_atomic_pin pins[4];
    for(unsigned i=0;i<4;++i) assert(!ps3_atomic_pin_acquire(&f.domain,0,1,&pins[i]));
    r=ps3_user_memory_allocate_and_publish_be32(&f.service,65536,65536,4092);
    assert(r.status==PS3_USER_MEMORY_SERVICE_OUTPUT_FAILURE && r.memory_status==PS3_GUEST_MEMORY_NO_MEMORY);
    for(unsigned i=0;i<4;++i) assert(!ps3_atomic_pin_release(&f.domain,&pins[i]));
    assert(used(&f)==0); unchanged(&f); finish(&f);

    init(&f); f.faults.race=1; pthread_t writer;
    assert(!pthread_create(&writer,NULL,publish,&f));
    assert(!pthread_mutex_lock(&f.faults.mutex));
    while(!f.faults.entered) assert(!pthread_cond_wait(&f.faults.condition,&f.faults.mutex));
    /* The query snapshot is complete, output is still pinned, and neither
     * allocator nor domain guard is held: retirement must observe BUSY. */
    assert(ps3_atomic_decommit(&f.domain,4096,4096)==PS3_GUEST_MEMORY_BUSY);
    f.faults.released=1; assert(!pthread_cond_broadcast(&f.faults.condition));
    assert(!pthread_mutex_unlock(&f.faults.mutex));
    assert(!pthread_join(writer,NULL));
    assert(!ps3_atomic_decommit(&f.domain,4096,4096));
    f.faults.race=0;
    finish(&f);

    init(&f); f.faults.race=1;
    assert(!pthread_create(&writer,NULL,allocate_publish,&f));
    assert(!pthread_mutex_lock(&f.faults.mutex));
    while(!f.faults.entered) assert(!pthread_cond_wait(&f.faults.condition,&f.faults.mutex));
    assert(ps3_atomic_decommit(&f.domain,0,4096)==PS3_GUEST_MEMORY_BUSY);
    f.faults.released=1; assert(!pthread_cond_broadcast(&f.faults.condition));
    assert(!pthread_mutex_unlock(&f.faults.mutex));
    assert(!pthread_join(writer,NULL)); f.faults.race=0;
    assert(used(&f)==65536);
    finish(&f);
    puts("USER_MEMORY_SERVICE_PASS"); return 0;
}
