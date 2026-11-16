#define _GNU_SOURCE
#include "process/exitspawn_callbacks.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
typedef struct guard_fixture {
    pthread_mutex_t mutex;
    atomic_uint inside, locks;
} guard_fixture;
static void lock_guard(void *context)
{
    guard_fixture *guard=context;
    assert(!pthread_mutex_lock(&guard->mutex));
    assert(atomic_fetch_add(&guard->inside,1)==0);
    atomic_fetch_add(&guard->locks,1);
}
static void unlock_guard(void *context)
{
    guard_fixture *guard=context;
    assert(atomic_fetch_sub(&guard->inside,1)==1);
    assert(!pthread_mutex_unlock(&guard->mutex));
}
static void setup(ps3_exitspawn_registry *registry,guard_fixture *guard)
{
    *registry=(ps3_exitspawn_registry){0}; *guard=(guard_fixture){0};
    assert(!pthread_mutex_init(&guard->mutex,NULL));
    assert(ps3_exitspawn_init(registry,(ps3_instance_guard){guard,lock_guard,unlock_guard})==PS3_EXITSPAWN_OK);
}
static void test_slots_and_opaque_values(void)
{
    ps3_exitspawn_registry first,second; guard_fixture a,b; setup(&first,&a);setup(&second,&b);
    ps3_exitspawn_result result=ps3_exitspawn_register(&first,PS3_EXITSPAWN_ATEXITSPAWN,0);
    assert(result.status==PS3_EXITSPAWN_OK && result.decision==PS3_EXITSPAWN_NULL && !result.snapshot.atexitspawn);
    result=ps3_exitspawn_register(&first,PS3_EXITSPAWN_ATEXITSPAWN,UINT32_MAX);
    assert(result.status==PS3_EXITSPAWN_OK && result.decision==PS3_EXITSPAWN_INSERTED && result.snapshot.atexitspawn==UINT32_MAX);
    for(unsigned i=0;i<3;++i) {
        result=ps3_exitspawn_register(&first,PS3_EXITSPAWN_ATEXITSPAWN,i==0?UINT32_MAX:i==1?3:0);
        assert(result.status==PS3_EXITSPAWN_OK && result.decision==PS3_EXITSPAWN_RETAINED && result.snapshot.atexitspawn==UINT32_MAX);
    }
    result=ps3_exitspawn_register(&first,PS3_EXITSPAWN_AT_EXITSPAWN,3);
    assert(result.decision==PS3_EXITSPAWN_INSERTED && result.snapshot.at_Exitspawn==3 && result.snapshot.atexitspawn==UINT32_MAX);
    ps3_exitspawn_snapshot snapshot={9,9};
    assert(ps3_exitspawn_read(&second,&snapshot)==PS3_EXITSPAWN_OK && !snapshot.atexitspawn && !snapshot.at_Exitspawn);
    assert(ps3_exitspawn_register(&second,PS3_EXITSPAWN_ATEXITSPAWN,17).decision==PS3_EXITSPAWN_INSERTED);
    assert(ps3_exitspawn_read(&first,&snapshot)==PS3_EXITSPAWN_OK && snapshot.atexitspawn==UINT32_MAX && snapshot.at_Exitspawn==3);
    assert(!atomic_load(&a.inside) && !atomic_load(&b.inside));
    assert(!pthread_mutex_destroy(&a.mutex));assert(!pthread_mutex_destroy(&b.mutex));
}
static void test_invalid_and_lifetime(void)
{
    ps3_exitspawn_registry registry={0}; ps3_exitspawn_snapshot output={11,12};
    assert(ps3_exitspawn_read(&registry,&output)==PS3_EXITSPAWN_NOT_INITIALIZED && output.atexitspawn==11 && output.at_Exitspawn==12);
    assert(ps3_exitspawn_register(&registry,PS3_EXITSPAWN_ATEXITSPAWN,1).status==PS3_EXITSPAWN_NOT_INITIALIZED);
    assert(ps3_exitspawn_init(&registry,(ps3_instance_guard){0})==PS3_EXITSPAWN_INVALID_ARGUMENT && !registry.initialized);
    guard_fixture guard;setup(&registry,&guard);
    assert(ps3_exitspawn_init(&registry,(ps3_instance_guard){&guard,lock_guard,unlock_guard})==PS3_EXITSPAWN_INVALID_ARGUMENT);
    unsigned locks=atomic_load(&guard.locks);
    assert(ps3_exitspawn_register(&registry,(ps3_exitspawn_kind)2,1).status==PS3_EXITSPAWN_INVALID_ARGUMENT);
    assert(atomic_load(&guard.locks)==locks);
    assert(ps3_exitspawn_read(NULL,&output)==PS3_EXITSPAWN_INVALID_ARGUMENT);
    assert(ps3_exitspawn_read(&registry,NULL)==PS3_EXITSPAWN_INVALID_ARGUMENT);
    assert(ps3_exitspawn_register(NULL,PS3_EXITSPAWN_ATEXITSPAWN,1).status==PS3_EXITSPAWN_INVALID_ARGUMENT);
    assert(ps3_exitspawn_read(&registry,&output)==PS3_EXITSPAWN_OK && !output.atexitspawn && !output.at_Exitspawn);
    assert(!pthread_mutex_destroy(&guard.mutex));
}
enum { WORKERS=16 };
typedef struct contender {
    ps3_exitspawn_registry *registry;
    pthread_barrier_t *start;
    ps3_exitspawn_kind kind;
    uint32_t value;
    ps3_exitspawn_result result;
} contender;
static void *register_worker(void *context)
{
    contender *worker=context; int status=pthread_barrier_wait(worker->start);
    assert(status==0 || status==PTHREAD_BARRIER_SERIAL_THREAD);
    worker->result=ps3_exitspawn_register(worker->registry,worker->kind,worker->value);
    return NULL;
}
static void test_concurrent_first_wins_and_snapshots(void)
{
    ps3_exitspawn_registry registry; guard_fixture guard;setup(&registry,&guard);
    pthread_barrier_t start; assert(!pthread_barrier_init(&start,NULL,WORKERS+1));
    contender workers[WORKERS]; pthread_t threads[WORKERS];
    for(unsigned i=0;i<WORKERS;++i) {
        workers[i]=(contender){.registry=&registry,.start=&start,.kind=i%2?PS3_EXITSPAWN_AT_EXITSPAWN:PS3_EXITSPAWN_ATEXITSPAWN,.value=i+1};
        assert(!pthread_create(&threads[i],NULL,register_worker,&workers[i]));
    }
    int status=pthread_barrier_wait(&start);assert(status==0 || status==PTHREAD_BARRIER_SERIAL_THREAD);
    ps3_exitspawn_snapshot observed={0};
    for(unsigned i=0;i<1000;++i) {
        ps3_exitspawn_snapshot current;
        assert(ps3_exitspawn_read(&registry,&current)==PS3_EXITSPAWN_OK);
        if(observed.atexitspawn) assert(current.atexitspawn==observed.atexitspawn);
        if(observed.at_Exitspawn) assert(current.at_Exitspawn==observed.at_Exitspawn);
        assert(!current.atexitspawn || (current.atexitspawn<=WORKERS && current.atexitspawn%2==1));
        assert(!current.at_Exitspawn || (current.at_Exitspawn<=WORKERS && current.at_Exitspawn%2==0));
        observed=current;
    }
    for(unsigned i=0;i<WORKERS;++i) assert(!pthread_join(threads[i],NULL));
    assert(ps3_exitspawn_read(&registry,&observed)==PS3_EXITSPAWN_OK);
    unsigned inserted[2]={0};
    for(unsigned i=0;i<WORKERS;++i) {
        assert(workers[i].result.status==PS3_EXITSPAWN_OK);
        if(workers[i].result.decision==PS3_EXITSPAWN_INSERTED) ++inserted[i%2];
        if(i%2) assert(workers[i].result.snapshot.at_Exitspawn==observed.at_Exitspawn);
        else assert(workers[i].result.snapshot.atexitspawn==observed.atexitspawn);
    }
    assert(inserted[0]==1 && inserted[1]==1 && observed.atexitspawn && observed.at_Exitspawn);
    assert(!atomic_load(&guard.inside));
    assert(!pthread_barrier_destroy(&start));assert(!pthread_mutex_destroy(&guard.mutex));
}
int main(void)
{
    test_slots_and_opaque_values();test_invalid_and_lifetime();test_concurrent_first_wins_and_snapshots();
    printf("exitspawn layout registry=%zu snapshot=%zu result=%zu\n",sizeof(ps3_exitspawn_registry),sizeof(ps3_exitspawn_snapshot),sizeof(ps3_exitspawn_result));
    puts("EXITSPAWN_CALLBACKS_PASS"); return 0;
}
