#define _POSIX_C_SOURCE 200809L
#include "../domain/disc_change_callbacks.h"
#include "../adapters/guard_pthread.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct {
    ps3_pthread_guard real;
    ps3_instance_guard real_port;
    ps3_disc_change_registry *registry;
    ps3_disc_change_pair last_unlock;
    unsigned held;
    _Atomic unsigned locks,unlocks;
} Guard;
static void lock_guard(void *context) {
    Guard *g=context;g->real_port.lock(g->real_port.context);
    assert(!g->held);g->held=1;atomic_fetch_add(&g->locks,1);
}
static void unlock_guard(void *context) {
    Guard *g=context;assert(g->held);
    if(g->registry)g->last_unlock=g->registry->current;
    g->held=0;atomic_fetch_add(&g->unlocks,1);g->real_port.unlock(g->real_port.context);
}
static ps3_instance_guard guard_init(Guard *g,ps3_disc_change_registry *r) {
    assert(ps3_pthread_guard_init(&g->real)==0);g->real_port=ps3_pthread_guard_port(&g->real);
    g->registry=r;g->held=0;atomic_init(&g->locks,0);atomic_init(&g->unlocks,0);
    const ps3_instance_guard port={g,lock_guard,unlock_guard};return port;
}
static void guard_finish(Guard *g) {
    assert(!g->held && atomic_load(&g->locks)==atomic_load(&g->unlocks));
    assert(ps3_pthread_guard_destroy(&g->real)==0);
}
static int equal(ps3_disc_change_pair a,ps3_disc_change_pair b) {
    return a.eject_descriptor==b.eject_descriptor && a.insert_descriptor==b.insert_descriptor;
}
static void validation_and_exclusive_init(void) {
    ps3_disc_change_registry r={0};Guard first={0},second={0};
    const ps3_instance_guard g1=guard_init(&first,&r),g2=guard_init(&second,&r);
    ps3_disc_change_pair output={0x1234,0x5678};const ps3_disc_change_pair sentinel=output;
    assert(ps3_disc_change_read(NULL,&output)==PS3_DISC_CHANGE_INVALID_ARGUMENT && equal(output,sentinel));
    assert(ps3_disc_change_read(&r,&output)==PS3_DISC_CHANGE_NOT_INITIALIZED && equal(output,sentinel));
    assert(ps3_disc_change_register(NULL,output).status==PS3_DISC_CHANGE_INVALID_ARGUMENT);
    ps3_disc_change_result result=ps3_disc_change_register(&r,output);
    assert(result.status==PS3_DISC_CHANGE_NOT_INITIALIZED && !result.committed);
    assert(ps3_disc_change_init(NULL,g1,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_INVALID_ARGUMENT);
    ps3_instance_guard bad=g1;bad.lock=NULL;
    assert(ps3_disc_change_init(&r,bad,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_INVALID_ARGUMENT);
    bad=g1;bad.unlock=NULL;
    assert(ps3_disc_change_init(&r,bad,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_INVALID_ARGUMENT);
    assert(ps3_disc_change_init(&r,g1,PS3_DISC_CHANGE_UNCONFIGURED)==PS3_DISC_CHANGE_UNSUPPORTED_POLICY);
    assert(ps3_disc_change_init(&r,g1,(ps3_disc_change_policy)99)==PS3_DISC_CHANGE_UNSUPPORTED_POLICY);
    assert(!r.initialized && !r.guard.lock && !r.guard.unlock && !r.current.eject_descriptor && !r.current.insert_descriptor);
    assert(ps3_disc_change_init(&r,g1,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_OK);
    result=ps3_disc_change_register(&r,sentinel);assert(result.status==PS3_DISC_CHANGE_OK && result.committed);
    const unsigned locks=atomic_load(&first.locks);
    assert(ps3_disc_change_init(&r,g2,PS3_DISC_CHANGE_UNCONFIGURED)==PS3_DISC_CHANGE_ALREADY_INITIALIZED);
    assert(r.guard.context==g1.context && r.guard.lock==g1.lock && r.guard.unlock==g1.unlock);
    assert(r.policy==PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE && equal(r.current,sentinel));
    assert(atomic_load(&first.locks)==locks && !atomic_load(&second.locks));
    assert(ps3_disc_change_read(&r,NULL)==PS3_DISC_CHANGE_INVALID_ARGUMENT);
    guard_finish(&first);guard_finish(&second);
}
static void opaque_replace_and_snapshot(void) {
    ps3_disc_change_registry r={0};Guard guard={0};const ps3_instance_guard port=guard_init(&guard,&r);
    assert(ps3_disc_change_init(&r,port,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_OK);
    const ps3_disc_change_pair requests[]={{1,UINT32_MAX},{0,3},{0xffffffff,0},{0,0},{0,0},{0x781234,0x785678}};
    ps3_disc_change_pair previous={0};
    for(unsigned i=0;i<sizeof requests/sizeof *requests;++i) {
        const ps3_disc_change_result result=ps3_disc_change_register(&r,requests[i]);
        assert(result.status==PS3_DISC_CHANGE_OK && result.committed);
        assert(equal(result.previous,previous) && equal(result.current,requests[i]));
        assert(equal(guard.last_unlock,requests[i]));
        ps3_disc_change_pair snapshot={0};assert(ps3_disc_change_read(&r,&snapshot)==PS3_DISC_CHANGE_OK);
        assert(equal(snapshot,requests[i]));snapshot.insert_descriptor^=0x123;
        assert(equal(r.current,requests[i]));previous=requests[i];
    }
    assert(atomic_load(&guard.locks)==12 && atomic_load(&guard.unlocks)==12);
    guard_finish(&guard);
}
enum { WRITES=2000,READS=6000 };
static const uint32_t PAIR_MASK=0xa5a55a5a;
typedef struct { ps3_disc_change_registry *registry;pthread_barrier_t *gate;uint32_t id; } Worker;
static int consistent(ps3_disc_change_pair p) { return p.eject_descriptor==(p.insert_descriptor^PAIR_MASK); }
static void *writer(void *context) {
    Worker *w=context;const int barrier=pthread_barrier_wait(w->gate);assert(barrier==0 || barrier==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<WRITES;++i) {
        const uint32_t value=(w->id<<24)|i;const ps3_disc_change_pair pair={value^PAIR_MASK,value};
        const ps3_disc_change_result result=ps3_disc_change_register(w->registry,pair);
        assert(result.status==PS3_DISC_CHANGE_OK && result.committed && equal(result.current,pair));
        assert(consistent(result.previous));
    }
    return NULL;
}
static void *reader(void *context) {
    Worker *w=context;const int barrier=pthread_barrier_wait(w->gate);assert(barrier==0 || barrier==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<READS;++i) {
        ps3_disc_change_pair pair={0};assert(ps3_disc_change_read(w->registry,&pair)==PS3_DISC_CHANGE_OK);
        assert(consistent(pair));
    }
    return NULL;
}
static void concurrent_native_guard(void) {
    ps3_disc_change_registry r={0};Guard guard={0};const ps3_instance_guard port=guard_init(&guard,&r);
    assert(ps3_disc_change_init(&r,port,PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE)==PS3_DISC_CHANGE_OK);
    const ps3_disc_change_pair initial={PAIR_MASK,0};assert(ps3_disc_change_register(&r,initial).committed);
    pthread_barrier_t gate;assert(pthread_barrier_init(&gate,NULL,4)==0);
    Worker workers[]={{&r,&gate,1},{&r,&gate,2},{&r,&gate,3}};pthread_t threads[3];
    assert(pthread_create(&threads[0],NULL,writer,&workers[0])==0);
    assert(pthread_create(&threads[1],NULL,writer,&workers[1])==0);
    assert(pthread_create(&threads[2],NULL,reader,&workers[2])==0);
    const int barrier=pthread_barrier_wait(&gate);assert(barrier==0 || barrier==PTHREAD_BARRIER_SERIAL_THREAD);
    for(unsigned i=0;i<3;++i)assert(pthread_join(threads[i],NULL)==0);
    assert(pthread_barrier_destroy(&gate)==0);
    assert(consistent(r.current) && consistent(guard.last_unlock));
    assert(atomic_load(&guard.locks)==1+2*WRITES+READS);
    guard_finish(&guard);
}
int main(void) {
    validation_and_exclusive_init();opaque_replace_and_snapshot();concurrent_native_guard();
    puts("DISC_CHANGE_CALLBACKS_PASS exclusive_init=1 opaque_replace=1 null_duplicate=1 committed_snapshot=1 real_guard_concurrency=1 no_invoke=1");
}
