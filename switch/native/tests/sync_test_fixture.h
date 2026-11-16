#ifndef NATIVE_SYNC_TEST_FIXTURE_H
#define NATIVE_SYNC_TEST_FIXTURE_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sync/lwmutex.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { MEMORY_BYTES = 65536, OBJECT_ADDRESS = 0x1000 };
typedef struct host_memory {
    unsigned char *bytes;
    pthread_mutex_t mutex;
    atomic_int fail_read, fail_write, read_delay_us;
    atomic_uint write_calls, fail_write_at;
    atomic_int pause_write, write_entered, release_write;
    unsigned char committed[MEMORY_BYTES/4096];
} host_memory;
static inline int mapped(host_memory *memory, uint32_t address, uint64_t size)
{
    if (address > MEMORY_BYTES || size > MEMORY_BYTES-address) return 0;
    if (!size) return 1;
    for (uint64_t page=address/4096; page<=((uint64_t)address+size-1)/4096; ++page) {
        if (!memory->committed[page]) return 0;
    }
    return 1;
}
static inline ps3_guest_memory_status host_read(void *context, uint32_t address, void *output, size_t size)
{
    host_memory *memory = context;
    if (atomic_load(&memory->fail_read)) return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    int delay=atomic_load(&memory->read_delay_us);
    if (delay) { struct timespec pause={delay/1000000,(delay%1000000)*1000}; assert(!nanosleep(&pause,NULL)); }
    assert(!pthread_mutex_lock(&memory->mutex));
    ps3_guest_memory_status status=PS3_GUEST_MEMORY_UNMAPPED;
    if (mapped(memory,address,size)) { memcpy(output,memory->bytes+address,size); status=PS3_GUEST_MEMORY_OK; }
    assert(!pthread_mutex_unlock(&memory->mutex));
    return status;
}
static inline ps3_guest_memory_status host_write(void *context, uint32_t address, const void *input, size_t size)
{
    host_memory *memory = context;
    unsigned call=atomic_fetch_add(&memory->write_calls,1)+1;
    if (atomic_load(&memory->pause_write)) {
        atomic_store(&memory->write_entered,1);
        while (!atomic_load(&memory->release_write)) {
            struct timespec tick={0,100000}; assert(!nanosleep(&tick,NULL));
        }
    }
    if (atomic_load(&memory->fail_write) || call==atomic_load(&memory->fail_write_at)) return PS3_GUEST_MEMORY_NO_MEMORY;
    assert(!pthread_mutex_lock(&memory->mutex));
    ps3_guest_memory_status status=PS3_GUEST_MEMORY_UNMAPPED;
    if (mapped(memory,address,size)) { memcpy(memory->bytes+address,input,size); status=PS3_GUEST_MEMORY_OK; }
    assert(!pthread_mutex_unlock(&memory->mutex));
    return status;
}
static inline ps3_guest_memory_status host_zero(void *context, uint32_t address, uint64_t size)
{
    host_memory *memory = context;
    assert(!pthread_mutex_lock(&memory->mutex));
    ps3_guest_memory_status status=PS3_GUEST_MEMORY_UNMAPPED;
    if (mapped(memory,address,size)) { memset(memory->bytes+address,0,size); status=PS3_GUEST_MEMORY_OK; }
    assert(!pthread_mutex_unlock(&memory->mutex));
    return status;
}
static inline ps3_guest_memory_status host_commit(void *context, uint32_t address, uint64_t size)
{
    host_memory *memory=context;
    if (address%4096 || !size || size%4096 || address>MEMORY_BYTES || size>MEMORY_BYTES-address) return PS3_GUEST_MEMORY_INVALID_RANGE;
    assert(!pthread_mutex_lock(&memory->mutex));
    assert(!mprotect(memory->bytes+address,size,PROT_READ|PROT_WRITE));
    for (uint64_t page=address/4096; page<((uint64_t)address+size)/4096; ++page) memory->committed[page]=1;
    assert(!pthread_mutex_unlock(&memory->mutex));
    return PS3_GUEST_MEMORY_OK;
}
static inline ps3_guest_memory_status host_decommit(void *context, uint32_t address, uint64_t size)
{
    host_memory *memory = context;
    if (address % 4096 || !size || size % 4096 || address > MEMORY_BYTES || size > MEMORY_BYTES-address) return PS3_GUEST_MEMORY_INVALID_RANGE;
    assert(!pthread_mutex_lock(&memory->mutex));
    assert(!madvise(memory->bytes+address,size,MADV_DONTNEED));
    assert(!mprotect(memory->bytes+address,size,PROT_NONE));
    for (uint64_t page=address/4096; page<((uint64_t)address+size)/4096; ++page) memory->committed[page]=0;
    assert(!pthread_mutex_unlock(&memory->mutex));
    return PS3_GUEST_MEMORY_OK;
}
static inline void guard_lock(void *context) { assert(!pthread_mutex_lock(context)); }
static inline void guard_unlock(void *context) { assert(!pthread_mutex_unlock(context)); }
typedef struct host_wait {
    pthread_cond_t condition;
    atomic_int fail_clock, fail_wait, early_timeouts;
    host_memory *fail_cleanup_memory;
    atomic_uint waits;
} host_wait;
static inline ps3_wait_status host_now(void *context, uint64_t *now)
{
    host_wait *wait = context;
    if (atomic_load(&wait->fail_clock)) return PS3_WAIT_FAILURE;
    struct timespec stamp;
    assert(!clock_gettime(CLOCK_MONOTONIC, &stamp));
    *now = (uint64_t)stamp.tv_sec * 1000000000 + stamp.tv_nsec;
    return PS3_WAIT_READY;
}
static inline ps3_wait_status host_wait_once(void *context, const ps3_instance_guard *guard,
                                    uint64_t deadline, ps3_wait_cancel cancel)
{
    host_wait *wait = context;
    atomic_fetch_add(&wait->waits, 1);
    if (atomic_load(&wait->fail_wait)) {
        if (wait->fail_cleanup_memory) atomic_store(&wait->fail_cleanup_memory->fail_write,1);
        return PS3_WAIT_FAILURE;
    }
    uint64_t now;
    if (host_now(context, &now) != PS3_WAIT_READY) return PS3_WAIT_FAILURE;
    if (cancel.is_cancelled && cancel.is_cancelled(cancel.context)) return PS3_WAIT_CANCELLED;
    if (now >= deadline) return PS3_WAIT_TIMEOUT;
    uint64_t tick = now > UINT64_MAX-2000000 ? UINT64_MAX : now+2000000;
    if (tick > deadline) tick=deadline;
    struct timespec until={(time_t)(tick/1000000000),(long)(tick%1000000000)};
    int result=pthread_cond_timedwait(&wait->condition, guard->context, &until);
    assert(result==0 || result==ETIMEDOUT);
    return atomic_load(&wait->early_timeouts) ? PS3_WAIT_TIMEOUT : PS3_WAIT_READY;
}
static inline void host_notify(void *context) { host_wait *wait=context; assert(!pthread_cond_broadcast(&wait->condition)); }
typedef struct fixture {
    host_memory memory;
    host_wait wait;
    pthread_mutex_t domain_mutex, registry_mutex;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[8];
    ps3_atomic_pin_record pins[8];
    ps3_lwmutex_registry registry;
    ps3_lwmutex_object objects[4];
    ps3_lwmutex_waiter waiters[8];
} fixture;
static inline void setup(fixture *f)
{
    memset(f,0,sizeof(*f));
    f->memory.bytes=mmap(NULL,MEMORY_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(f->memory.bytes!=MAP_FAILED);
    assert(!pthread_mutex_init(&f->memory.mutex,NULL));
    memset(f->memory.committed,1,sizeof(f->memory.committed));
    assert(!pthread_mutex_init(&f->domain_mutex,NULL)); assert(!pthread_mutex_init(&f->registry_mutex,NULL));
    pthread_condattr_t attr; assert(!pthread_condattr_init(&attr)); assert(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
    assert(!pthread_cond_init(&f->wait.condition,&attr)); assert(!pthread_condattr_destroy(&attr));
    ps3_guest_memory memory={&f->memory,host_commit,host_decommit,host_read,host_write,host_zero};
    ps3_instance_guard domain_guard={&f->domain_mutex,guard_lock,guard_unlock};
    assert(ps3_atomic_domain_init(&f->domain,memory,domain_guard,f->owners,8,f->pins,8)==PS3_GUEST_MEMORY_OK);
    ps3_instance_guard registry_guard={&f->registry_mutex,guard_lock,guard_unlock};
    ps3_wait_port wait={&f->wait,host_now,host_wait_once,host_notify};
    assert(ps3_lwmutex_init_with_conditions(&f->registry,&f->domain,registry_guard,wait,f->objects,4,f->waiters,8,8).status==PS3_LWMUTEX_OK);
}
static inline void teardown(fixture *f)
{
    assert(ps3_lwmutex_cleanup(&f->registry).status==PS3_LWMUTEX_OK);
    assert(!pthread_cond_destroy(&f->wait.condition)); assert(!pthread_mutex_destroy(&f->registry_mutex)); assert(!pthread_mutex_destroy(&f->domain_mutex));
    assert(!pthread_mutex_destroy(&f->memory.mutex));
    assert(!munmap(f->memory.bytes,MEMORY_BYTES));
}
static inline uint32_t word(fixture *f, unsigned offset)
{
    unsigned char bytes[4]; assert(ps3_atomic_read(&f->domain,OBJECT_ADDRESS+offset,bytes,4)==PS3_GUEST_MEMORY_OK);
    return (uint32_t)bytes[0]<<24 | (uint32_t)bytes[1]<<16 | (uint32_t)bytes[2]<<8 | bytes[3];
}
static inline ps3_lwmutex_thread identity(uint32_t id,int32_t priority) { return (ps3_lwmutex_thread){id,priority}; }
static inline void pause_ms(unsigned milliseconds)
{
    struct timespec pause = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000};
    assert(!nanosleep(&pause, NULL));
}

static inline void bounded_join(pthread_t thread)
{
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 10;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
}
#endif
