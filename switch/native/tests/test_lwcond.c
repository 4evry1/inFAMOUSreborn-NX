#include "sync_test_fixture.h"
#include "sync/lwcond.h"
enum { CONDITION_ADDRESS=0x2000 };
typedef struct condition_fixture {
    fixture mutex;
    pthread_mutex_t guard;
    host_wait wait;
    ps3_lwcond_registry registry;
    ps3_lwcond_object objects[4];
    ps3_lwcond_waiter waiters[8];
    atomic_int query_failure, post_wake_status;
    atomic_int token_sensitive_park, park_entered;
} condition_fixture;
static ps3_lwcond_thread_status thread_exists(void *context,uint32_t id,unsigned *exists)
{
    condition_fixture *f=context;
    if(atomic_load(&f->query_failure)) return PS3_LWCOND_THREAD_QUERY_FAILURE;
    *exists=id>=1 && id<=8; return PS3_LWCOND_THREAD_OK;
}
static ps3_wait_status condition_now(void *context,uint64_t *now)
{ condition_fixture *f=context; return host_now(&f->wait,now); }
static void condition_notify(void *context)
{ condition_fixture *f=context; host_notify(&f->wait); }
static ps3_wait_status condition_park(void *context,const ps3_instance_guard *guard,uint64_t deadline,ps3_wait_cancel cancel)
{
    condition_fixture *f=context;
    atomic_store(&f->park_entered,1);
    ps3_wait_status status;
    if(atomic_load(&f->token_sensitive_park) && !cancel.is_cancelled && deadline==UINT64_MAX) {
        assert(!pthread_cond_wait(&f->wait.condition,guard->context));
        status=PS3_WAIT_READY;
    } else status=host_wait_once(&f->wait,guard,deadline,cancel);
    int injected=atomic_exchange(&f->post_wake_status,0);
    return injected?(ps3_wait_status)injected:status;
}
static void condition_setup(condition_fixture *f)
{
    memset(f,0,sizeof(*f)); setup(&f->mutex);
    assert(!pthread_mutex_init(&f->guard,NULL));
    pthread_condattr_t attr; assert(!pthread_condattr_init(&attr));
    assert(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));
    assert(!pthread_cond_init(&f->wait.condition,&attr)); assert(!pthread_condattr_destroy(&attr));
    assert(ps3_lwmutex_create(&f->mutex.registry,OBJECT_ADDRESS,PS3_LWMUTEX_FIFO,PS3_LWMUTEX_RECURSIVE).status==PS3_LWMUTEX_OK);
    ps3_instance_guard guard={&f->guard,guard_lock,guard_unlock};
    ps3_wait_port wait={f,condition_now,condition_park,condition_notify};
    ps3_lwcond_thread_existence threads={f,thread_exists};
    assert(ps3_lwcond_init(&f->registry,&f->mutex.registry,guard,wait,threads,f->objects,4,f->waiters,8).status==PS3_LWCOND_OK);
}
static void condition_teardown(condition_fixture *f)
{
    ps3_lwcond_report report={0};
    assert(ps3_lwcond_cleanup(&f->registry,&report).status==PS3_LWCOND_OK);
    teardown(&f->mutex); assert(!pthread_cond_destroy(&f->wait.condition)); assert(!pthread_mutex_destroy(&f->guard));
}
static void create_condition(condition_fixture *f,uint32_t address)
{
    const unsigned char name[8]={0,255,1,2,3,4,5,6}; ps3_lwcond_report report={0};
    assert(ps3_lwcond_create(&f->registry,address,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_OK);
    assert(report.publications.count==1 && report.publications.writes[0].size==8);
    assert(!memcmp(report.publications.writes[0].bytes,f->mutex.memory.bytes+address,8));
}
typedef struct condition_worker {
    condition_fixture *f;
    uint32_t id;
    unsigned depth;
    int32_t priority;
    uint32_t *order;
    atomic_uint *order_count;
    uint64_t timeout;
    atomic_int cancel,stop,done;
    unsigned ordinary_cancel_disabled;
    pthread_t owner;
    ps3_lwcond_result result;
    ps3_lwcond_report report;
} condition_worker;
static int condition_cancel(void *context)
{
    condition_worker *worker=context; assert(pthread_equal(worker->owner,pthread_self()));
    return atomic_load(&worker->cancel);
}
static int condition_stop(void *context)
{
    condition_worker *worker=context; assert(pthread_equal(worker->owner,pthread_self()));
    return atomic_load(&worker->stop);
}
static void *condition_worker_run(void *context)
{
    condition_worker *worker=context; worker->owner=pthread_self();
    for(unsigned i=0;i<worker->depth;++i) assert(ps3_lwmutex_lock(&worker->f->mutex.registry,OBJECT_ADDRESS,identity(worker->id,worker->priority),0,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_OK);
    worker->result=ps3_lwcond_wait(&worker->f->registry,CONDITION_ADDRESS,identity(worker->id,worker->priority),worker->timeout,
        worker->ordinary_cancel_disabled?(ps3_wait_cancel){0}:(ps3_wait_cancel){worker,condition_cancel},
        (ps3_wait_cancel){worker,condition_stop},&worker->report);
    if(worker->result.mutex_reacquired) {
        if(worker->order) worker->order[atomic_fetch_add(worker->order_count,1)]=worker->id;
        assert(word(&worker->f->mutex,0)==worker->id && word(&worker->f->mutex,12)==worker->depth-1);
        for(unsigned i=0;i<worker->depth;++i) assert(ps3_lwmutex_unlock(&worker->f->mutex.registry,OBJECT_ADDRESS,identity(worker->id,worker->priority)).status==PS3_LWMUTEX_OK);
    }
    atomic_store(&worker->done,1);
    return NULL;
}
static void await_condition_waiters(condition_fixture *f,unsigned count)
{
    for(unsigned tries=0;tries<2000;++tries) {
        guard_lock(&f->guard); size_t active=f->objects[0].active_waiters; guard_unlock(&f->guard);
        if(active==count) return;
        pause_ms(1);
    }
    assert(!"condition wait registration timed out");
}
static void test_condition_lifecycle_and_targets(void)
{
    condition_fixture f; condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    assert(!memcmp(f.objects[0].name,(unsigned char[]){0,255,1,2,3,4,5,6},8));
    assert(ps3_lwmutex_destroy(&f.mutex.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_BUSY);
    ps3_lwcond_report report={0}; const unsigned char name[8]={0};
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_UNSUPPORTED);
    assert(report.publications.count==0);
    assert(ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,0,&report).status==PS3_LWCOND_TARGET_NOT_FOUND);
    assert(ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,7,&report).status==PS3_LWCOND_TARGET_NOT_WAITING);
    assert(ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,UINT64_C(0x100000001),&report).status==PS3_LWCOND_UNSUPPORTED);
    assert(ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,PS3_LWMUTEX_RESERVED,&report).status==PS3_LWCOND_UNSUPPORTED);
    atomic_store(&f.query_failure,1);
    assert(ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,7,&report).status==PS3_LWCOND_THREAD_FAILURE);
    atomic_store(&f.query_failure,0);
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK && report.publications.count==0);
    assert(ps3_lwcond_destroy(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK);
    assert(report.publications.count==1 && report.publications.writes[0].bytes[7]==254);
    condition_teardown(&f);
}
static void test_condition_wait_restore_and_terminal_outcomes(void)
{
    condition_fixture f; condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    condition_worker worker={.f=&f,.id=1,.depth=3}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
    ps3_lwcond_report report={0};
    assert(ps3_lwcond_destroy(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_BUSY);
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK && report.selected_count==1);
    bounded_join(thread);
    assert(worker.result.status==PS3_LWCOND_OK && worker.result.condition_outcome==PS3_LWCOND_SELECTED && worker.result.mutex_reacquired);
    assert(worker.report.publications.count==2 && report.publications.count==1);
    for(unsigned mode=0;mode<3;++mode) {
        worker=(condition_worker){.f=&f,.id=1,.depth=2,.timeout=mode==0?2000:0};
        assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
        if(mode==1) atomic_store(&worker.cancel,1);
        if(mode==2) { atomic_store(&worker.cancel,1); atomic_store(&worker.stop,1); }
        host_notify(&f.wait); bounded_join(thread);
        if(mode==0) assert(worker.result.status==PS3_LWCOND_TIMED_OUT && worker.result.mutex_reacquired && worker.report.publications.count==3);
        if(mode==1) assert(worker.result.status==PS3_LWCOND_CANCELLED && worker.result.mutex_reacquired);
        if(mode==2) assert(worker.result.status==PS3_LWCOND_STOPPED && worker.result.must_not_resume && !worker.result.mutex_reacquired);
    }
    condition_teardown(&f);
}
static void test_selected_outcome_survives_cancel_return(void)
{
    condition_fixture f; condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    condition_worker worker={.f=&f,.id=1,.depth=1}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
    atomic_store(&f.post_wake_status,PS3_WAIT_CANCELLED);
    ps3_lwcond_report report={0};
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK);
    bounded_join(thread);
    assert(worker.result.condition_outcome==PS3_LWCOND_SELECTED && worker.result.status==PS3_LWCOND_OK && worker.result.mutex_reacquired);
    condition_teardown(&f);
    condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    worker=(condition_worker){.f=&f,.id=1,.depth=1};
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
    atomic_store(&f.mutex.memory.write_calls,0); atomic_store(&f.mutex.memory.fail_write_at,2);
    report=(ps3_lwcond_report){0};
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK);
    bounded_join(thread);
    assert(worker.result.condition_outcome==PS3_LWCOND_SELECTED && worker.result.status==PS3_LWCOND_CLEANUP_REQUIRED);
    assert(worker.result.must_not_resume && worker.result.cleanup_retained && !worker.result.mutex_reacquired);
    assert(word(&f.mutex,0)==PS3_LWMUTEX_FREE && word(&f.mutex,4)==0);
    condition_teardown(&f);
}
static void test_condition_selection_order(unsigned protocol,int targeted)
{
    condition_fixture f; condition_setup(&f);
    assert(ps3_lwmutex_destroy(&f.mutex.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_create(&f.mutex.registry,OBJECT_ADDRESS,protocol,PS3_LWMUTEX_RECURSIVE).status==PS3_LWMUTEX_OK);
    create_condition(&f,CONDITION_ADDRESS);
    uint32_t order[2]={0}; atomic_uint count=0;
    condition_worker workers[2]={{.f=&f,.id=1,.depth=1,.priority=50,.order=order,.order_count=&count},
        {.f=&f,.id=2,.depth=1,.priority=10,.order=order,.order_count=&count}};
    pthread_t threads[2];
    for(unsigned i=0;i<2;++i) { assert(!pthread_create(&threads[i],NULL,condition_worker_run,&workers[i])); await_condition_waiters(&f,i+1); }
    ps3_lwcond_report report={0};
    ps3_lwcond_result selected=targeted?ps3_lwcond_signal_to(&f.registry,CONDITION_ADDRESS,2,&report):ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report);
    assert(selected.status==PS3_LWCOND_OK && report.selected_count==1);
    for(unsigned tries=0;tries<2000 && !atomic_load(&count);++tries) pause_ms(1);
    assert(atomic_load(&count)==1 && order[0]==(targeted || protocol==PS3_LWMUTEX_PRIORITY?2u:1u));
    report=(ps3_lwcond_report){0};
    assert(ps3_lwcond_signal_all(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK && report.selected_count==1);
    for(unsigned i=0;i<2;++i) { bounded_join(threads[i]); assert(workers[i].result.status==PS3_LWCOND_OK); }
    condition_teardown(&f);
}
static void test_condition_partial_all_and_restore_stop(void)
{
    condition_fixture f; condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    condition_worker workers[2]={{.f=&f,.id=1,.depth=1},{.f=&f,.id=2,.depth=1}}; pthread_t threads[2];
    for(unsigned i=0;i<2;++i) { assert(!pthread_create(&threads[i],NULL,condition_worker_run,&workers[i])); await_condition_waiters(&f,i+1); }
    atomic_store(&f.mutex.memory.write_calls,0); atomic_store(&f.mutex.memory.fail_write_at,2);
    ps3_lwcond_report report={0}; ps3_lwcond_result partial=ps3_lwcond_signal_all(&f.registry,CONDITION_ADDRESS,&report);
    assert(partial.must_not_resume && partial.status!=PS3_LWCOND_OK && report.selected_count==1 && report.publications.count==1);
    for(unsigned i=0;i<2;++i) atomic_store(&workers[i].stop,1);
    host_notify(&f.wait); host_notify(&f.mutex.wait);
    for(unsigned i=0;i<2;++i) bounded_join(threads[i]);
    assert(workers[1].result.status==PS3_LWCOND_STOPPED && workers[1].result.must_not_resume);
    condition_teardown(&f);

    condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    condition_worker worker={.f=&f,.id=1,.depth=2}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
    assert(ps3_lwmutex_try_lock(&f.mutex.registry,OBJECT_ADDRESS,identity(3,0)).status==PS3_LWMUTEX_OK);
    report=(ps3_lwcond_report){0};
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_OK);
    for(unsigned tries=0;tries<2000 && !atomic_load(&f.mutex.wait.waits);++tries) pause_ms(1);
    assert(atomic_load(&f.mutex.wait.waits));
    assert(ps3_lwcond_destroy(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_BUSY);
    atomic_store(&worker.stop,1); host_notify(&f.mutex.wait); bounded_join(thread);
    assert(worker.result.status==PS3_LWCOND_STOPPED && worker.result.condition_outcome==PS3_LWCOND_SELECTED && !worker.result.mutex_reacquired);
    assert(word(&f.mutex,0)==3 && word(&f.mutex,4)==0);
    assert(ps3_lwmutex_unlock(&f.mutex.registry,OBJECT_ADDRESS,identity(3,0)).status==PS3_LWMUTEX_OK);
    condition_teardown(&f);
}
static void test_condition_failures_and_pin_lifetime(void)
{
    condition_fixture f; condition_setup(&f); const unsigned char name[8]={0}; ps3_lwcond_report report={0};
    atomic_store(&f.mutex.memory.fail_write,1);
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_MEMORY_FAILURE);
    assert(!report.publications.count && !f.objects[0].state && !f.mutex.registry.condition_binding_count);
    atomic_store(&f.mutex.memory.fail_write,0); create_condition(&f,CONDITION_ADDRESS);
    assert(ps3_atomic_decommit(&f.mutex.domain,CONDITION_ADDRESS,4096)==PS3_GUEST_MEMORY_BUSY);
    ps3_lwcond_result nonowner=ps3_lwcond_wait(&f.registry,CONDITION_ADDRESS,identity(2,0),0,(ps3_wait_cancel){0},(ps3_wait_cancel){0},&report);
    assert(nonowner.status==PS3_LWCOND_NOT_OWNER && !report.publications.count);
    condition_worker worker={.f=&f,.id=1,.depth=1}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); await_condition_waiters(&f,1);
    atomic_store(&f.wait.fail_clock,1);
    ps3_lwcond_result failure=ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report);
    assert(failure.status==PS3_LWCOND_WAIT_FAILURE && failure.must_not_resume && !report.selected_count);
    atomic_store(&f.wait.fail_clock,0); atomic_store(&worker.stop,1); host_notify(&f.wait); bounded_join(thread);
    condition_teardown(&f);
}
static void test_condition_boundaries_and_provider_failure(void)
{
    condition_fixture f; condition_setup(&f); const unsigned char name[8]={0}; ps3_lwcond_report report={0};
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS+1,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_INVALID_ARGUMENT);
    assert(ps3_lwcond_create(&f.registry,OBJECT_ADDRESS+4,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_INVALID_ARGUMENT);
    assert(ps3_lwcond_create(&f.registry,UINT32_MAX-3,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_INVALID_ARGUMENT);
    report.publications.count=PS3_SYNC_PUBLICATION_CAPACITY;
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_RESOURCE_EXHAUSTED);
    report.publications.count=0;
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS,OBJECT_ADDRESS+32,name,&report).status==PS3_LWCOND_NOT_FOUND);
    assert(!f.objects[0].state && !report.publications.count);
    f.registry.next_queue_id=PS3_LWMUTEX_RESERVED;
    assert(ps3_lwcond_create(&f.registry,CONDITION_ADDRESS,OBJECT_ADDRESS,name,&report).status==PS3_LWCOND_RESOURCE_EXHAUSTED);
    f.registry.next_queue_id=1; create_condition(&f,CONDITION_ADDRESS);
    unsigned char bad[4]={0,0,0,0};
    assert(ps3_atomic_write(&f.mutex.domain,CONDITION_ADDRESS+4,bad,4)==PS3_GUEST_MEMORY_OK);
    assert(ps3_lwcond_signal(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_INCONSISTENT);
    bad[3]=(unsigned char)f.objects[0].queue_id;
    assert(ps3_atomic_write(&f.mutex.domain,CONDITION_ADDRESS+4,bad,4)==PS3_GUEST_MEMORY_OK);
    atomic_store(&f.mutex.memory.fail_write,1);
    assert(ps3_lwcond_destroy(&f.registry,CONDITION_ADDRESS,&report).status==PS3_LWCOND_MEMORY_FAILURE);
    assert(f.objects[0].state && !report.publications.count);
    atomic_store(&f.mutex.memory.fail_write,0);
    condition_worker worker={.f=&f,.id=1,.depth=1}; pthread_t thread;
    atomic_store(&f.wait.fail_wait,1);
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker)); bounded_join(thread);
    assert(worker.result.status==PS3_LWCOND_WAIT_FAILURE && worker.result.condition_outcome==PS3_LWCOND_FAILED && worker.result.mutex_reacquired && worker.result.must_not_resume);
    atomic_store(&f.wait.fail_wait,0);
    condition_teardown(&f);
}
static void test_instance_stop_reaches_token_sensitive_untimed_park(void)
{
    condition_fixture f; condition_setup(&f); create_condition(&f,CONDITION_ADDRESS);
    atomic_store(&f.token_sensitive_park,1);
    condition_worker worker={.f=&f,.id=1,.depth=1,.ordinary_cancel_disabled=1}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,condition_worker_run,&worker));
    for(unsigned i=0;i<2000 && !atomic_load(&f.park_entered);++i) pause_ms(1);
    assert(atomic_load(&f.park_entered));
    atomic_store(&worker.stop,1);
    for(unsigned i=0;i<100 && !atomic_load(&worker.done);++i) pause_ms(1);
    int observed_without_notification=atomic_load(&worker.done);
    if(!observed_without_notification) {
        /* Bounded rescue after observing the failure; no leaked parked thread. */
        guard_lock(&f.guard); host_notify(&f.wait); guard_unlock(&f.guard);
    }
    bounded_join(thread);
    assert(worker.result.status==PS3_LWCOND_STOPPED && worker.result.must_not_resume && !worker.result.mutex_reacquired);
    condition_teardown(&f);
    assert(observed_without_notification);
}
int main(void)
{
    test_instance_stop_reaches_token_sensitive_untimed_park();
    test_condition_boundaries_and_provider_failure();
    test_condition_failures_and_pin_lifetime();
    test_condition_partial_all_and_restore_stop();
    test_condition_selection_order(PS3_LWMUTEX_FIFO,0);
    test_condition_selection_order(PS3_LWMUTEX_PRIORITY,0);
    test_condition_selection_order(PS3_LWMUTEX_FIFO,1);
    test_selected_outcome_survives_cancel_return();
    test_condition_lifecycle_and_targets(); test_condition_wait_restore_and_terminal_outcomes();
    printf("lwcond layout object=%zu waiter=%zu registry=%zu result=%zu report=%zu\n",sizeof(ps3_lwcond_object),sizeof(ps3_lwcond_waiter),sizeof(ps3_lwcond_registry),sizeof(ps3_lwcond_result),sizeof(ps3_lwcond_report));
    puts("LWCOND_CONTRACT_PASS");
    return 0;
}
