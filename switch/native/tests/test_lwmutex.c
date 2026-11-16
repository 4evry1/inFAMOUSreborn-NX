#include "sync_test_fixture.h"

static void test_first_image_and_ownership(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,PS3_LWMUTEX_PRIORITY,PS3_LWMUTEX_RECURSIVE).status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==UINT32_MAX && word(&f,4)==0 && word(&f,8)==0x12 && word(&f,12)==0 && word(&f,16)!=0 && word(&f,20)==0);
    assert(ps3_atomic_decommit(&f.domain,OBJECT_ADDRESS,4096)==PS3_GUEST_MEMORY_BUSY);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS+8,1,0x20).status==PS3_LWMUTEX_ALREADY_EXISTS);
    assert(ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(1,100),0,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==1 && word(&f,12)==0);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_OK && word(&f,12)==1);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(2,100)).status==PS3_LWMUTEX_NOT_OWNER);
    assert(ps3_lwmutex_destroy(&f.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_BUSY);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_OK && word(&f,12)==0);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_OK && word(&f,0)==UINT32_MAX);
    assert(ps3_lwmutex_destroy(&f.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_OK && word(&f,0)==UINT32_MAX-1);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_DEADLOCK);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,100)).status==PS3_LWMUTEX_OK);
    teardown(&f);
}


static void wait_for_queue(fixture *f, unsigned count)
{
    for (unsigned attempt = 0; attempt < 2000; ++attempt) {
        if (word(f, 4) == count) return;
        pause_ms(1);
    }
    assert(!"bounded queue observation timed out");
}

static int is_cancelled(void *context)
{
    return atomic_load((atomic_int *)context);
}

typedef struct locking_worker {
    fixture *f;
    pthread_t host_thread;
    ps3_lwmutex_thread thread;
    uint64_t timeout_us;
    atomic_int cancel;
    ps3_lwmutex_result outcome;
    uint32_t *order;
    atomic_uint *order_count;
    unsigned iterations;
    unsigned *protected_counter;
    atomic_int *inside;
} locking_worker;

static int worker_cancelled(void *context)
{
    locking_worker *worker=context;
    assert(pthread_equal(worker->host_thread,pthread_self()));
    return atomic_load(&worker->cancel);
}

static void *lock_worker(void *context)
{
    locking_worker *worker = context;
    worker->host_thread=pthread_self();
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL));
    for (unsigned iteration = 0; iteration < worker->iterations; ++iteration) {
        worker->outcome = ps3_lwmutex_lock(&worker->f->registry, OBJECT_ADDRESS,
            worker->thread, worker->timeout_us, (ps3_wait_cancel){worker, worker_cancelled});
        if (worker->outcome.status != PS3_LWMUTEX_OK) return NULL;
        assert(word(worker->f, 0) == worker->thread.id);
        if (worker->inside) assert(atomic_fetch_add(worker->inside, 1) == 0);
        if (worker->order) worker->order[atomic_fetch_add(worker->order_count, 1)] = worker->thread.id;
        if (worker->protected_counter) {
            unsigned prior = *worker->protected_counter;
            struct timespec pause = {0, 1000};
            assert(!nanosleep(&pause, NULL));
            *worker->protected_counter = prior + 1;
        }
        if (worker->inside) assert(atomic_fetch_sub(worker->inside, 1) == 1);
        assert(ps3_lwmutex_unlock(&worker->f->registry, OBJECT_ADDRESS, worker->thread).status == PS3_LWMUTEX_OK);
    }
    return NULL;
}


static void test_order(unsigned protocol)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry, OBJECT_ADDRESS, protocol, 0x20).status == PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry, OBJECT_ADDRESS, identity(1, 0)).status == PS3_LWMUTEX_OK);
    uint32_t order[3] = {0};
    atomic_uint order_count = 0;
    locking_worker workers[3] = {
        {.f=&f, .thread={2,50}, .order=order, .order_count=&order_count, .iterations=1},
        {.f=&f, .thread={3,10}, .order=order, .order_count=&order_count, .iterations=1},
        {.f=&f, .thread={4,10}, .order=order, .order_count=&order_count, .iterations=1}
    };
    pthread_t threads[3];
    for (unsigned index=0; index<3; ++index) {
        assert(!pthread_create(&threads[index], NULL, lock_worker, &workers[index]));
        wait_for_queue(&f,index+1);
    }
    assert(ps3_lwmutex_destroy(&f.registry,OBJECT_ADDRESS).status == PS3_LWMUTEX_BUSY);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(2,50)).status == PS3_LWMUTEX_BUSY);
    for (unsigned wake=0; wake<30; ++wake) host_notify(&f.wait);
    assert(word(&f,0)==1 && word(&f,4)==3);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status == PS3_LWMUTEX_OK);
    for (unsigned index=0; index<3; ++index) {
        bounded_join(threads[index]);
        assert(workers[index].outcome.status == PS3_LWMUTEX_OK);
    }
    assert(atomic_load(&order_count)==3);
    if (protocol == PS3_LWMUTEX_FIFO) assert(order[0]==2 && order[1]==3 && order[2]==4);
    else assert(order[0]==3 && order[1]==4 && order[2]==2);
    assert(word(&f,0)==PS3_LWMUTEX_FREE && word(&f,4)==0);
    teardown(&f);
}

static void test_mutual_exclusion(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry, OBJECT_ADDRESS, 1, 0x20).status == PS3_LWMUTEX_OK);
    unsigned counter=0;
    atomic_int inside=0;
    pthread_t threads[4];
    locking_worker workers[4];
    for (unsigned index=0; index<4; ++index) {
        workers[index]=(locking_worker){.f=&f, .thread={index+1,100}, .iterations=200,
            .protected_counter=&counter, .inside=&inside};
        assert(!pthread_create(&threads[index],NULL,lock_worker,&workers[index]));
    }
    for (unsigned index=0; index<4; ++index) {
        bounded_join(threads[index]);
        assert(workers[index].outcome.status==PS3_LWMUTEX_OK);
    }
    assert(counter==800 && atomic_load(&inside)==0);
    teardown(&f);
}

static void test_wait_outcomes(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry, OBJECT_ADDRESS, 1, 0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry, OBJECT_ADDRESS, identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(2,0),UINT64_MAX,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    atomic_store(&f.wait.early_timeouts,1);
    locking_worker timeout={.f=&f,.thread={2,100},.timeout_us=10000,.iterations=1};
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,lock_worker,&timeout));
    wait_for_queue(&f,1);
    bounded_join(thread);
    assert(timeout.outcome.status==PS3_LWMUTEX_TIMED_OUT && word(&f,4)==0 && word(&f,0)==1);
    assert(atomic_load(&f.wait.waits)>=2);
    atomic_store(&f.wait.early_timeouts,0);
    locking_worker cancelled_worker={.f=&f,.thread={3,100},.iterations=1};
    assert(!pthread_create(&thread,NULL,lock_worker,&cancelled_worker));
    wait_for_queue(&f,1);
    atomic_store(&cancelled_worker.cancel,1);
    host_notify(&f.wait);
    bounded_join(thread);
    assert(cancelled_worker.outcome.status==PS3_LWMUTEX_CANCELLED && word(&f,4)==0);
    atomic_store(&f.wait.fail_wait,1);
    ps3_lwmutex_result outcome=ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(4,100),0,(ps3_wait_cancel){0});
    assert(outcome.status==PS3_LWMUTEX_WAIT_FAILURE && outcome.wait_status==PS3_WAIT_FAILURE && word(&f,4)==0);
    f.wait.fail_cleanup_memory=&f.memory;
    outcome=ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(4,100),0,(ps3_wait_cancel){0});
    assert(outcome.status==PS3_LWMUTEX_CLEANUP_REQUIRED && outcome.memory_status==PS3_GUEST_MEMORY_NO_MEMORY && outcome.wait_status==PS3_WAIT_FAILURE);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    atomic_store(&f.memory.fail_write,0);
    atomic_store(&f.wait.fail_wait,0);
    f.wait.fail_cleanup_memory=NULL;
    assert(word(&f,4)==1);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(word(&f,4)==0 && word(&f,0)==PS3_LWMUTEX_FREE);
    atomic_store(&f.wait.fail_clock,1);
    outcome=ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(5,0),1,(ps3_wait_cancel){0});
    assert(outcome.status==PS3_LWMUTEX_WAIT_FAILURE && word(&f,0)==PS3_LWMUTEX_FREE);
    atomic_store(&f.wait.fail_clock,0);
    teardown(&f);
}

static void test_memory_transactions_and_invariants(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,4,0x20).status==PS3_LWMUTEX_UNSUPPORTED);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS+1,1,0x20).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_create(&f.registry,0xfffffff8,1,0x20).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_create(&f.registry,MEMORY_BYTES,1,0x20).status==PS3_LWMUTEX_MEMORY_FAILURE);
    atomic_store(&f.memory.fail_write,1);
    ps3_lwmutex_result outcome=ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x10);
    assert(outcome.status==PS3_LWMUTEX_MEMORY_FAILURE && outcome.memory_status==PS3_GUEST_MEMORY_NO_MEMORY);
    atomic_store(&f.memory.fail_write,0);
    assert(word(&f,0)==0 && ps3_atomic_decommit(&f.domain,OBJECT_ADDRESS,4096)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_commit(&f.domain,OBJECT_ADDRESS,4096)==PS3_GUEST_MEMORY_OK);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x10).status==PS3_LWMUTEX_OK);
    ps3_atomic_owner owner; uint64_t reserved;
    assert(ps3_atomic_owner_acquire(&f.domain,&owner)==PS3_GUEST_MEMORY_OK);
    assert(ps3_atomic_load_reserve64(&f.domain,owner,OBJECT_ADDRESS,&reserved)==PS3_GUEST_MEMORY_OK);
    atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_MEMORY_FAILURE && word(&f,0)==PS3_LWMUTEX_FREE);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    int stored=1;
    assert(ps3_atomic_store_conditional64(&f.domain,owner,OBJECT_ADDRESS,reserved,&stored)==PS3_GUEST_MEMORY_OK && !stored);
    unsigned char maximum[4]={255,255,255,255};
    assert(ps3_atomic_write(&f.domain,OBJECT_ADDRESS+12,maximum,4)==PS3_GUEST_MEMORY_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OVERFLOW);
    unsigned char zero[4]={0};
    assert(ps3_atomic_write(&f.domain,OBJECT_ADDRESS+12,zero,4)==PS3_GUEST_MEMORY_OK);
    atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_MEMORY_FAILURE && word(&f,0)==1);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    unsigned char bad[4]={0,0,0,1};
    assert(ps3_atomic_write(&f.domain,OBJECT_ADDRESS+20,bad,4)==PS3_GUEST_MEMORY_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(2,0)).status==PS3_LWMUTEX_INCONSISTENT);
    assert(ps3_atomic_write(&f.domain,OBJECT_ADDRESS+20,zero,4)==PS3_GUEST_MEMORY_OK);
    atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_destroy(&f.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_MEMORY_FAILURE);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_atomic_decommit(&f.domain,OBJECT_ADDRESS,4096)==PS3_GUEST_MEMORY_BUSY);
    assert(ps3_atomic_owner_release(&f.domain,&owner)==PS3_GUEST_MEMORY_OK);
    teardown(&f);
}


static void test_capacity_and_isolation(void)
{
    fixture first, second; setup(&first); setup(&second);
    for (unsigned index=0; index<4; ++index) {
        assert(ps3_lwmutex_create(&first.registry,OBJECT_ADDRESS+32*index,1,0x20).status==PS3_LWMUTEX_OK);
    }
    assert(ps3_lwmutex_create(&first.registry,OBJECT_ADDRESS+128,1,0x20).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    uint32_t old_queue=word(&first,16);
    assert(ps3_lwmutex_destroy(&first.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_create(&first.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK && word(&first,16)!=old_queue);
    assert(ps3_lwmutex_create(&second.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&first.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&second.registry,OBJECT_ADDRESS,identity(2,0)).status==PS3_LWMUTEX_OK);
    assert(word(&first,0)==1 && word(&second,0)==2);
    assert(ps3_lwmutex_unlock(&first.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_unlock(&second.registry,OBJECT_ADDRESS,identity(2,0)).status==PS3_LWMUTEX_OK);
    teardown(&first); teardown(&second);

    fixture f; setup(&f);
    ps3_instance_guard guard={&f.registry_mutex,guard_lock,guard_unlock};
    ps3_wait_port wait={&f.wait,host_now,host_wait_once,host_notify};
    assert(ps3_lwmutex_init(&f.registry,&f.domain,guard,wait,f.objects,4,f.waiters,2).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    locking_worker workers[2]={{.f=&f,.thread={2,0},.iterations=1},{.f=&f,.thread={3,0},.iterations=1}};
    pthread_t threads[2];
    for (unsigned index=0; index<2; ++index) {
        assert(!pthread_create(&threads[index],NULL,lock_worker,&workers[index]));
        wait_for_queue(&f,index+1);
    }
    assert(ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(4,0),0,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,4)==2);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    for (unsigned index=0; index<2; ++index) { bounded_join(threads[index]); assert(workers[index].outcome.status==PS3_LWMUTEX_OK); }
    teardown(&f);
}

static void test_deadline_and_release_races(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    atomic_store(&f.memory.read_delay_us,5000);
    assert(ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(1,0),1000,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_TIMED_OUT);
    atomic_store(&f.memory.read_delay_us,0);
    assert(word(&f,0)==PS3_LWMUTEX_FREE && word(&f,4)==0);
    for (unsigned cycle=0; cycle<30; ++cycle) {
        assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
        locking_worker worker={.f=&f,.thread={2,0},.iterations=1,.timeout_us=10000};
        pthread_t thread; assert(!pthread_create(&thread,NULL,lock_worker,&worker));
        wait_for_queue(&f,1);
        if (cycle%3==0) atomic_store(&worker.cancel,1);
        if (cycle%3==1) pause_ms(15);
        assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
        if (cycle%3==2) atomic_store(&worker.cancel,1);
        host_notify(&f.wait);
        bounded_join(thread);
        assert(worker.outcome.status==PS3_LWMUTEX_OK || worker.outcome.status==PS3_LWMUTEX_CANCELLED || worker.outcome.status==PS3_LWMUTEX_TIMED_OUT);
        if (cycle%3==0) assert(worker.outcome.status==PS3_LWMUTEX_CANCELLED);
        if (cycle%3==1) assert(worker.outcome.status==PS3_LWMUTEX_TIMED_OUT);
        assert(word(&f,0)==PS3_LWMUTEX_FREE && word(&f,4)==0);
    }
    teardown(&f);
}


static void test_exhaustion_boundaries(void)
{
    fixture f; setup(&f);
    /* Seed otherwise impractically distant exhaustion states in quiescent
     * private records; no active guest identity or queue is manufactured. */
    for (unsigned index=0; index<4; ++index) f.objects[index].generation=UINT64_MAX;
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    for (unsigned index=0; index<4; ++index) f.objects[index].generation=0;
    f.registry.next_queue_id=UINT32_MAX;
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    f.registry.next_queue_id=1;
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    f.registry.next_sequence=UINT64_MAX;
    assert(ps3_lwmutex_lock(&f.registry,OBJECT_ADDRESS,identity(2,0),0,(ps3_wait_cancel){0}).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,4)==0 && word(&f,0)==1);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

typedef struct condition_batch_worker {
    fixture *f;
    ps3_lwmutex_condition_wait **tokens;
    ps3_sync_publication_report *report;
    size_t promoted;
    ps3_lwmutex_result result;
} condition_batch_worker;
static void *promote_batch_worker(void *context)
{
    condition_batch_worker *worker=context;
    worker->result=ps3_lwmutex_condition_promote_many(&worker->f->registry,
        worker->tokens,2,&worker->promoted,worker->report);
    return NULL;
}
static void test_condition_bulk_excludes_interleaved_admission(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0}; ps3_lwmutex_condition_wait tokens[2]={{0}};
    ps3_lwmutex_condition_wait *items[]={&tokens[0],&tokens[1]};
    ps3_sync_publication_report report={0};
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    for(unsigned i=0;i<2;++i) {
        assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(i+1,0)).status==PS3_LWMUTEX_OK);
        assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(i+1,0),items[i],&report).status==PS3_LWMUTEX_OK);
    }
    uint64_t first=f.registry.next_sequence;
    atomic_store(&f.memory.pause_write,1);
    condition_batch_worker batch={.f=&f,.tokens=items,.report=&report}; pthread_t batch_thread,ordinary_thread;
    assert(!pthread_create(&batch_thread,NULL,promote_batch_worker,&batch));
    for(unsigned i=0;i<2000 && !atomic_load(&f.memory.write_entered);++i) pause_ms(1);
    assert(atomic_load(&f.memory.write_entered));
    locking_worker ordinary={.f=&f,.thread={3,0},.iterations=1};
    assert(!pthread_create(&ordinary_thread,NULL,lock_worker,&ordinary));
    pause_ms(5);
    atomic_store(&f.memory.release_write,1);
    bounded_join(batch_thread);
    assert(batch.result.status==PS3_LWMUTEX_OK && batch.promoted==2 && report.count==4);
    atomic_store(&f.memory.pause_write,0);
    wait_for_queue(&f,3);
    guard_lock(&f.registry_mutex);
    assert(f.waiters[tokens[0].waiter_index].sequence==first);
    assert(f.waiters[tokens[1].waiter_index].sequence==first+1);
    unsigned found=0;
    for(unsigned i=0;i<8;++i) if(f.waiters[i].state && f.waiters[i].thread.id==3) {
        assert(f.waiters[i].sequence==first+2); found=1;
    }
    guard_unlock(&f.registry_mutex); assert(found);
    for(unsigned i=0;i<2;++i) assert(ps3_lwmutex_condition_abort(&f.registry,items[i],&report).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    bounded_join(ordinary_thread); assert(ordinary.outcome.status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

static void test_condition_binding_quota_and_legacy_init(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding bindings[9]={{0}};
    for(unsigned i=0;i<8;++i) assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&bindings[i]).status==PS3_LWMUTEX_OK);
    assert(f.registry.condition_binding_count==8);
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&bindings[8]).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    for(unsigned i=0;i<8;++i) assert(ps3_lwmutex_condition_unbind(&f.registry,&bindings[i]).status==PS3_LWMUTEX_OK);
    assert(f.registry.condition_binding_count==0);
    teardown(&f);
    setup(&f);
    assert(ps3_lwmutex_init(&f.registry,&f.domain,f.registry.guard,f.registry.wait,f.objects,4,f.waiters,8).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&bindings[0]).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    teardown(&f);
}

static void test_condition_bulk_headroom_and_prefix(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0};
    ps3_lwmutex_condition_wait tokens[2]={{0}};
    ps3_lwmutex_condition_wait *items[]={&tokens[0],&tokens[1]};
    ps3_sync_publication_report report={0}; size_t promoted=99;
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    for(unsigned i=0;i<2;++i) {
        assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(i+1,0)).status==PS3_LWMUTEX_OK);
        assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(i+1,0),items[i],&report).status==PS3_LWMUTEX_OK);
    }
    size_t writes=report.count; f.registry.next_sequence=UINT64_MAX-1;
    assert(ps3_lwmutex_condition_promote_many(&f.registry,items,2,&promoted,&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(promoted==0 && report.count==writes && word(&f,4)==0);
    assert(tokens[0].state==PS3_LWMUTEX_CONDITION_DORMANT && tokens[1].state==PS3_LWMUTEX_CONDITION_DORMANT);
    assert(f.registry.next_sequence==UINT64_MAX-1);
    f.registry.next_sequence=10;
    report.count=PS3_SYNC_PUBLICATION_CAPACITY-1;
    assert(ps3_lwmutex_condition_promote_many(&f.registry,items,2,&promoted,&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(promoted==0 && f.registry.next_sequence==10 && word(&f,4)==0 && report.count==PS3_SYNC_PUBLICATION_CAPACITY-1);
    report.count=writes;
    ps3_lwmutex_condition_wait copy=tokens[0]; ps3_lwmutex_condition_wait *bad[]={items[0],&copy};
    assert(ps3_lwmutex_condition_promote_many(&f.registry,bad,2,&promoted,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    bad[1]=items[0];
    assert(ps3_lwmutex_condition_promote_many(&f.registry,bad,2,&promoted,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(report.count==writes && f.registry.next_sequence==10);
    atomic_store(&f.memory.write_calls,0); atomic_store(&f.memory.fail_write_at,2);
    assert(ps3_lwmutex_condition_promote_many(&f.registry,items,2,&promoted,&report).status==PS3_LWMUTEX_MEMORY_FAILURE);
    assert(promoted==1 && report.count==writes+1 && word(&f,4)==1 && f.registry.next_sequence==12);
    assert(tokens[0].state==PS3_LWMUTEX_CONDITION_ELIGIBLE && tokens[1].state==PS3_LWMUTEX_CONDITION_DORMANT);
    assert(f.waiters[tokens[0].waiter_index].sequence==10);
    tokens[0].state=PS3_LWMUTEX_CONDITION_DORMANT;
    assert(ps3_lwmutex_condition_promote_many(&f.registry,items,2,&promoted,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(word(&f,4)==1 && f.registry.next_sequence==12);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(4,0)).status==PS3_LWMUTEX_BUSY);
    assert(report.writes[writes].size==24 && report.writes[writes].bytes[7]==1);
    tokens[0].state=PS3_LWMUTEX_CONDITION_ELIGIBLE;
    atomic_store(&f.memory.fail_write_at,0);
    assert(ps3_lwmutex_condition_promote_many(&f.registry,&items[1],1,&promoted,&report).status==PS3_LWMUTEX_OK && promoted==1);
    assert(f.waiters[tokens[1].waiter_index].sequence==12);
    for(unsigned i=0;i<2;++i) assert(ps3_lwmutex_condition_abort(&f.registry,items[i],&report).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

static void test_condition_binding_and_full_release(void)
{
    fixture f, other; setup(&f); setup(&other);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x10).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_create(&other.registry,OBJECT_ADDRESS,1,0x10).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0};
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_destroy(&f.registry,OBJECT_ADDRESS).status==PS3_LWMUTEX_BUSY);
    ps3_lwmutex_condition_binding copy=binding;
    assert(ps3_lwmutex_condition_unbind(&f.registry,&copy).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_condition_unbind(&other.registry,&binding).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_wait token={0}; ps3_sync_publication_report report={0};
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(2,0),&token,&report).status==PS3_LWMUTEX_NOT_OWNER);
    assert(report.count==0 && word(&f,0)==1 && word(&f,12)==2);
    report.count=PS3_SYNC_PUBLICATION_CAPACITY;
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,0)==1 && word(&f,12)==2);
    report.count=0; atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_MEMORY_FAILURE);
    assert(report.count==0 && word(&f,0)==1 && word(&f,12)==2);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==PS3_LWMUTEX_FREE && word(&f,12)==0 && word(&f,4)==0);
    assert(report.count==1 && report.writes[0].address==OBJECT_ADDRESS && report.writes[0].size==24);
    assert(!memcmp(report.writes[0].bytes,f.memory.bytes+OBJECT_ADDRESS,24));
    ps3_lwmutex_condition_wait copied=token;
    assert(ps3_lwmutex_condition_promote(&f.registry,&copied,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_condition_promote(&other.registry,&token,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_BUSY);
    assert(ps3_lwmutex_condition_promote(&f.registry,&token,&report).status==PS3_LWMUTEX_OK);
    assert(word(&f,4)==1 && report.count==2);
    assert(ps3_lwmutex_condition_reacquire(&f.registry,&token,(ps3_wait_cancel){0},&report).status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==1 && word(&f,12)==2 && word(&f,4)==0 && report.count==3);
    assert(ps3_lwmutex_condition_promote(&f.registry,&copied,&report).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    for(unsigned i=0;i<3;++i) assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f); teardown(&other);
}

static void test_condition_poison_and_cleanup(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0}; ps3_lwmutex_condition_wait token={0};
    ps3_sync_publication_report report={0};
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_promote(&f.registry,&token,&report).status==PS3_LWMUTEX_OK);
    atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_condition_abort(&f.registry,&token,&report).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    assert(report.count==2 && word(&f,4)==1 && token.state==PS3_LWMUTEX_CONDITION_POISONED);
    atomic_store(&f.memory.fail_write,0);
    /* Poison remains counted for consistency but cannot receive ownership. */
    locking_worker worker={.f=&f,.thread={2,0},.iterations=1}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,lock_worker,&worker));
    bounded_join(thread); assert(worker.outcome.status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==PS3_LWMUTEX_FREE && word(&f,4)==1);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(word(&f,4)==0 && report.count==3 && token.state==0);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

static void test_condition_failure_and_reservation_capacity(void)
{
    fixture f; setup(&f);
    ps3_lwmutex_registry alias={0};
    ps3_instance_guard bad_guard={&f.domain_mutex,guard_lock,guard_unlock};
    assert(ps3_lwmutex_init(&alias,&f.domain,bad_guard,f.registry.wait,f.objects,4,f.waiters,8).status==PS3_LWMUTEX_INVALID_ARGUMENT);
    assert(!alias.initialized);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0}; ps3_lwmutex_condition_wait tokens[8]={{0}};
    ps3_sync_publication_report report={0};
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    for(unsigned i=0;i<8;++i) {
        assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(i+1,0)).status==PS3_LWMUTEX_OK);
        assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(i+1,0),&tokens[i],&report).status==PS3_LWMUTEX_OK);
    }
    assert(word(&f,4)==0);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(9,0)).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_wait extra={0}; size_t before=report.count;
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(9,0),&extra,&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,0)==9 && report.count==before);
    for(unsigned i=0;i<8;++i) assert(ps3_lwmutex_condition_abort(&f.registry,&tokens[i],&report).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(9,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&tokens[0],&report).status==PS3_LWMUTEX_OK);
    f.registry.next_sequence=UINT64_MAX; before=report.count;
    assert(ps3_lwmutex_condition_promote(&f.registry,&tokens[0],&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,4)==0 && report.count==before && tokens[0].state==PS3_LWMUTEX_CONDITION_DORMANT);
    f.registry.next_sequence=1;
    atomic_store(&f.memory.fail_write,1);
    assert(ps3_lwmutex_condition_promote(&f.registry,&tokens[0],&report).status==PS3_LWMUTEX_MEMORY_FAILURE);
    assert(word(&f,4)==0 && report.count==before);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_lwmutex_condition_promote(&f.registry,&tokens[0],&report).status==PS3_LWMUTEX_OK);
    before=report.count; atomic_store(&f.memory.fail_write,1);
    ps3_lwmutex_result failed=ps3_lwmutex_condition_reacquire(&f.registry,&tokens[0],(ps3_wait_cancel){0},&report);
    assert(failed.status==PS3_LWMUTEX_CLEANUP_REQUIRED && failed.memory_status==PS3_GUEST_MEMORY_NO_MEMORY);
    assert(tokens[0].state==PS3_LWMUTEX_CONDITION_POISONED && word(&f,0)==PS3_LWMUTEX_FREE && word(&f,4)==1 && report.count==before);
    atomic_store(&f.memory.fail_write,0);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    f.registry.next_reservation=UINT64_MAX; before=report.count;
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&tokens[0],&report).status==PS3_LWMUTEX_RESOURCE_EXHAUSTED);
    assert(word(&f,0)==1 && report.count==before);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    f.registry.next_reservation=100;
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&tokens[0],&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_promote(&f.registry,&tokens[0],&report).status==PS3_LWMUTEX_OK);
    report.count=PS3_SYNC_PUBLICATION_CAPACITY;
    assert(ps3_lwmutex_condition_abort(&f.registry,&tokens[0],&report).status==PS3_LWMUTEX_CLEANUP_REQUIRED);
    assert(tokens[0].state==PS3_LWMUTEX_CONDITION_POISONED && word(&f,4)==1);
    report.count=0;
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK && report.count==1);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

static void test_condition_selection_admission_and_stop(void)
{
    fixture f; setup(&f);
    assert(ps3_lwmutex_create(&f.registry,OBJECT_ADDRESS,1,0x20).status==PS3_LWMUTEX_OK);
    ps3_lwmutex_condition_binding binding={0}; ps3_lwmutex_condition_wait token={0};
    ps3_sync_publication_report report={0};
    assert(ps3_lwmutex_condition_bind(&f.registry,OBJECT_ADDRESS,&binding).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_promote(&f.registry,&token,&report).status==PS3_LWMUTEX_OK);
    locking_worker later={.f=&f,.thread={2,0},.iterations=1}; pthread_t thread;
    assert(!pthread_create(&thread,NULL,lock_worker,&later)); wait_for_queue(&f,2);
    assert(word(&f,0)==PS3_LWMUTEX_FREE);
    assert(ps3_lwmutex_condition_reacquire(&f.registry,&token,(ps3_wait_cancel){0},&report).status==PS3_LWMUTEX_OK);
    assert(word(&f,0)==1);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    bounded_join(thread); assert(later.outcome.status==PS3_LWMUTEX_OK);
    report.count=0;
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(1,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_release(&f.registry,&binding,identity(1,0),&token,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_try_lock(&f.registry,OBJECT_ADDRESS,identity(3,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_promote(&f.registry,&token,&report).status==PS3_LWMUTEX_OK);
    atomic_int stop=1;
    ps3_lwmutex_result stopped=ps3_lwmutex_condition_reacquire(&f.registry,&token,(ps3_wait_cancel){&stop,is_cancelled},&report);
    assert(stopped.status==PS3_LWMUTEX_CLEANUP_REQUIRED && stopped.wait_status==PS3_WAIT_CANCELLED);
    assert(token.state==PS3_LWMUTEX_CONDITION_POISONED && word(&f,0)==3 && word(&f,4)==0);
    assert(ps3_lwmutex_unlock(&f.registry,OBJECT_ADDRESS,identity(3,0)).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_cleanup(&f.registry,&report).status==PS3_LWMUTEX_OK);
    assert(ps3_lwmutex_condition_unbind(&f.registry,&binding).status==PS3_LWMUTEX_OK);
    teardown(&f);
}

int main(void)
{
    assert(!pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL));
    test_condition_bulk_excludes_interleaved_admission();
    test_condition_binding_quota_and_legacy_init();
    test_condition_bulk_headroom_and_prefix();
    test_condition_failure_and_reservation_capacity();
    test_condition_binding_and_full_release();
    test_condition_poison_and_cleanup();
    test_condition_selection_admission_and_stop();
    test_first_image_and_ownership();
    test_order(PS3_LWMUTEX_FIFO);
    test_order(PS3_LWMUTEX_PRIORITY);
    test_mutual_exclusion();
    test_wait_outcomes();
    test_memory_transactions_and_invariants();
    test_capacity_and_isolation();
    test_deadline_and_release_races();
    test_exhaustion_boundaries();
    printf("layout binding=%zu reservation=%zu object=%zu waiter=%zu registry=%zu report=%zu\n", sizeof(ps3_lwmutex_condition_binding), sizeof(ps3_lwmutex_condition_wait), sizeof(ps3_lwmutex_object), sizeof(ps3_lwmutex_waiter), sizeof(ps3_lwmutex_registry), sizeof(ps3_sync_publication_report));
    puts("lwmutex: PASS real pthread/memory FIFO, priority, mutual exclusion, timeout/cancel, transactions");
}
