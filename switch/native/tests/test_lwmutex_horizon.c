#include "../adapters/guest_space_horizon.h"
#include "../adapters/lwmutex_wait_horizon.h"
#include "lwmutex.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { OBJECT_ADDRESS = 0x40000000, WORKER_CYCLES = 64 };
typedef struct native_fixture {
    ps3_horizon_guest_space *space;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record owners[8];
    ps3_atomic_pin_record pins[8];
    Mutex domain_mutex;
    ps3_horizon_lwmutex_wait wait;
    ps3_lwmutex_registry registry;
    ps3_lwmutex_object objects[4];
    ps3_lwmutex_waiter waiters[8];
} native_fixture;

static void lock_domain(void *context) { mutexLock(context); }
static void unlock_domain(void *context) { mutexUnlock(context); }
static int token_cancelled(void *context) { return atomic_load((atomic_int *)context); }

static int read_word(native_fixture *fixture, unsigned offset, uint32_t *value)
{
    unsigned char bytes[4];
    if (ps3_atomic_read(&fixture->domain, OBJECT_ADDRESS + offset, bytes, 4) != PS3_GUEST_MEMORY_OK) return 0;
    *value = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
    return 1;
}

static int wait_for_queue(native_fixture *fixture, uint32_t expected)
{
    for (unsigned retry = 0; retry < 1000; ++retry) {
        uint32_t count;
        if (!read_word(fixture, 4, &count)) return 0;
        if (count == expected) return 1;
        svcSleepThread(1000000);
    }
    return 0;
}

typedef struct mutex_worker {
    native_fixture *fixture;
    ps3_lwmutex_thread identity;
    ps3_lwmutex_result result;
    atomic_int cancel;
    unsigned iterations, completed;
    uint64_t timeout_us;
    unsigned *counter;
    atomic_int *inside;
    uint32_t *order;
    atomic_uint *order_count;
    int failures;
} mutex_worker;

static void acquire_and_release(void *context)
{
    mutex_worker *worker = context;
    for (unsigned cycle = 0; cycle < worker->iterations; ++cycle) {
        worker->result = ps3_lwmutex_lock(&worker->fixture->registry, OBJECT_ADDRESS,
            worker->identity, worker->timeout_us, (ps3_wait_cancel){&worker->cancel, token_cancelled});
        if (worker->result.status != PS3_LWMUTEX_OK) return;
        uint32_t owner;
        worker->failures += !read_word(worker->fixture, 0, &owner) || owner != worker->identity.id;
        if (worker->inside) worker->failures += atomic_fetch_add(worker->inside, 1) != 0;
        if (worker->order) worker->order[atomic_fetch_add(worker->order_count, 1)] = worker->identity.id;
        if (worker->counter) {
            unsigned prior = *worker->counter;
            svcSleepThread(1000);
            *worker->counter = prior + 1;
        }
        if (worker->inside) worker->failures += atomic_fetch_sub(worker->inside, 1) != 1;
        ps3_lwmutex_result released = ps3_lwmutex_unlock(
            &worker->fixture->registry, OBJECT_ADDRESS, worker->identity);
        if (released.status != PS3_LWMUTEX_OK) { ++worker->failures; return; }
        ++worker->completed;
    }
}

static int start_worker(Thread *thread, mutex_worker *worker)
{
    Result result = threadCreate(thread, acquire_and_release, worker, NULL, 0x10000, 0x2c, -2);
    if (R_FAILED(result)) return 0;
    result = threadStart(thread);
    if (R_FAILED(result)) { threadClose(thread); return 0; }
    return 1;
}

static _Noreturn void stop_with_live_resources(const char *reason)
{
    printf("LWMUTEX_HORIZON FAIL %s live_resources_preserved=true\n", reason);
    fflush(stdout);
    svcExitProcess();
    __builtin_trap();
}

static int join_worker(Thread *thread)
{
    Result result = svcWaitSynchronizationSingle(thread->handle, 10000000000ULL);
    if (R_FAILED(result)) {
        printf("LWMUTEX_HORIZON bounded_join_result=0x%x\n", result);
        stop_with_live_resources("bounded_join");
    }
    if (R_FAILED(threadWaitForExit(thread))) {
        stop_with_live_resources("worker_join");
    }
    return R_SUCCEEDED(threadClose(thread));
}

static int test_order(native_fixture *fixture, uint32_t protocol)
{
    int failures = 0;
    ps3_lwmutex_thread holder = {1, 0};
    if (ps3_lwmutex_create(&fixture->registry, OBJECT_ADDRESS, protocol, 0x20).status != PS3_LWMUTEX_OK) return 1;
    if (ps3_lwmutex_try_lock(&fixture->registry, OBJECT_ADDRESS, holder).status != PS3_LWMUTEX_OK) return 1;
    uint32_t order[3] = {0};
    atomic_uint order_count = 0;
    mutex_worker workers[3] = {
        {.fixture=fixture,.identity={2,50},.iterations=1,.timeout_us=2000000,.order=order,.order_count=&order_count},
        {.fixture=fixture,.identity={3,10},.iterations=1,.timeout_us=2000000,.order=order,.order_count=&order_count},
        {.fixture=fixture,.identity={4,10},.iterations=1,.timeout_us=2000000,.order=order,.order_count=&order_count}
    };
    Thread threads[3];
    bool started[3] = {false};
    for (unsigned index=0; index<3; ++index) {
        started[index] = start_worker(&threads[index], &workers[index]);
        if (!started[index]) { ++failures; break; }
        if (!wait_for_queue(fixture, index+1)) { ++failures; break; }
    }
    failures += ps3_lwmutex_destroy(&fixture->registry, OBJECT_ADDRESS).status != PS3_LWMUTEX_BUSY;
    ps3_wait_port wait = ps3_horizon_lwmutex_wait_port(&fixture->wait);
    for (unsigned wake=0; wake<8; ++wake) wait.notify_all(wait.context);
    failures += ps3_lwmutex_unlock(&fixture->registry, OBJECT_ADDRESS, holder).status != PS3_LWMUTEX_OK;
    for (unsigned index=0; index<3; ++index) {
        if (!started[index]) continue;
        failures += !join_worker(&threads[index]);
        failures += workers[index].failures;
        failures += workers[index].result.status != PS3_LWMUTEX_OK || workers[index].completed != 1;
    }
    failures += atomic_load(&order_count) != 3;
    if (protocol == PS3_LWMUTEX_FIFO) failures += order[0]!=2 || order[1]!=3 || order[2]!=4;
    else failures += order[0]!=3 || order[1]!=4 || order[2]!=2;
    failures += ps3_lwmutex_destroy(&fixture->registry, OBJECT_ADDRESS).status != PS3_LWMUTEX_OK;
    printf("LWMUTEX_HORIZON protocol=%u order=%u,%u,%u failures=%d\n", protocol, order[0], order[1], order[2], failures);
    return failures;
}

static int test_contending_workers(native_fixture *fixture)
{
    if (ps3_lwmutex_create(&fixture->registry, OBJECT_ADDRESS, 1, 0x20).status != PS3_LWMUTEX_OK) return 1;
    unsigned counter = 0;
    atomic_int inside = 0;
    mutex_worker workers[2] = {
        {.fixture=fixture,.identity={10,100},.iterations=WORKER_CYCLES,.timeout_us=2000000,.counter=&counter,.inside=&inside},
        {.fixture=fixture,.identity={11,100},.iterations=WORKER_CYCLES,.timeout_us=2000000,.counter=&counter,.inside=&inside}
    };
    Thread threads[2];
    bool started[2] = {false};
    int failures = 0;
    for (unsigned index=0; index<2; ++index) {
        started[index] = start_worker(&threads[index], &workers[index]);
        failures += !started[index];
    }
    for (unsigned index=0; index<2; ++index) {
        if (!started[index]) continue;
        failures += !join_worker(&threads[index]);
        failures += workers[index].failures;
        failures += workers[index].completed != WORKER_CYCLES || workers[index].result.status != PS3_LWMUTEX_OK;
    }
    failures += counter != 2*WORKER_CYCLES || atomic_load(&inside) != 0;
    failures += ps3_lwmutex_destroy(&fixture->registry, OBJECT_ADDRESS).status != PS3_LWMUTEX_OK;
    printf("LWMUTEX_HORIZON workers=2 completed=%u,%u counter=%u failures=%d\n",
        workers[0].completed, workers[1].completed, counter, failures);
    return failures;
}

static int test_wait_outcomes(native_fixture *fixture)
{
    if (ps3_lwmutex_create(&fixture->registry, OBJECT_ADDRESS, 1, 0x20).status != PS3_LWMUTEX_OK) return 1;
    ps3_lwmutex_thread holder = {1, 0};
    if (ps3_lwmutex_try_lock(&fixture->registry, OBJECT_ADDRESS, holder).status != PS3_LWMUTEX_OK) return 1;
    mutex_worker timeout = {.fixture=fixture,.identity={2,100},.iterations=1,.timeout_us=100000};
    Thread thread;
    int failures = 0;
    if (start_worker(&thread, &timeout)) {
        failures += !wait_for_queue(fixture, 1);
        failures += !join_worker(&thread);
        failures += timeout.result.status != PS3_LWMUTEX_TIMED_OUT || timeout.completed != 0;
    } else ++failures;
    mutex_worker cancelled_worker = {.fixture=fixture,.identity={3,100},.iterations=1};
    if (start_worker(&thread, &cancelled_worker)) {
        failures += !wait_for_queue(fixture, 1);
        atomic_store(&cancelled_worker.cancel, 1);
        ps3_wait_port wait = ps3_horizon_lwmutex_wait_port(&fixture->wait);
        wait.notify_all(wait.context);
        failures += !join_worker(&thread);
        failures += cancelled_worker.result.status != PS3_LWMUTEX_CANCELLED || cancelled_worker.completed != 0;
    } else ++failures;
    uint32_t owner = 0, queued = UINT32_MAX;
    failures += !read_word(fixture, 0, &owner) || owner != 1;
    failures += !read_word(fixture, 4, &queued) || queued != 0;
    failures += ps3_lwmutex_unlock(&fixture->registry, OBJECT_ADDRESS, holder).status != PS3_LWMUTEX_OK;
    failures += ps3_lwmutex_destroy(&fixture->registry, OBJECT_ADDRESS).status != PS3_LWMUTEX_OK;
    printf("LWMUTEX_HORIZON timeout_cancel failures=%d\n", failures);
    return failures;
}

static int test_wait_adapter(native_fixture *fixture)
{
    ps3_wait_port wait = ps3_horizon_lwmutex_wait_port(&fixture->wait);
    ps3_instance_guard guard = ps3_horizon_lwmutex_wait_guard(&fixture->wait);
    int failures = 0;
    failures += ps3_horizon_lwmutex_wait_init(NULL, 2000000) != PS3_WAIT_FAILURE;
    ps3_horizon_lwmutex_wait unused;
    failures += ps3_horizon_lwmutex_wait_init(&unused, 0) != PS3_WAIT_FAILURE;
    guard.lock(guard.context);
    uint64_t started = 0, after = 0;
    failures += wait.now(wait.context, &started) != PS3_WAIT_READY;
    ps3_wait_status status = PS3_WAIT_READY;
    for (unsigned retry = 0; retry < 100 && status == PS3_WAIT_READY; ++retry) {
        status = wait.wait(wait.context, &guard, started + 2000000, (ps3_wait_cancel){0});
        failures += !mutexIsLockedByCurrentThread(&fixture->wait.mutex);
    }
    failures += status != PS3_WAIT_TIMEOUT;
    failures += wait.now(wait.context, &after) != PS3_WAIT_READY || after < started + 2000000;
    atomic_int cancelled = 1;
    failures += wait.wait(wait.context, &guard, UINT64_MAX,
        (ps3_wait_cancel){&cancelled, token_cancelled}) != PS3_WAIT_CANCELLED;
    failures += !mutexIsLockedByCurrentThread(&fixture->wait.mutex);
    uint64_t frequency = fixture->wait.frequency;
    fixture->wait.frequency = 0;
    uint64_t unchanged = 123;
    failures += wait.now(wait.context, &unchanged) != PS3_WAIT_FAILURE || unchanged != 123;
    failures += wait.wait(wait.context, &guard, UINT64_MAX, (ps3_wait_cancel){0}) != PS3_WAIT_FAILURE;
    failures += !mutexIsLockedByCurrentThread(&fixture->wait.mutex);
    fixture->wait.frequency = frequency;
    guard.unlock(guard.context);
    Mutex other;
    mutexInit(&other);
    mutexLock(&other);
    ps3_instance_guard wrong = guard;
    wrong.context = &other;
    failures += wait.wait(wait.context, &wrong, UINT64_MAX, (ps3_wait_cancel){0}) != PS3_WAIT_FAILURE;
    failures += !mutexIsLockedByCurrentThread(&other);
    mutexUnlock(&other);
    printf("LWMUTEX_HORIZON wait_adapter failures=%d frequency=%llu\n", failures,
        (unsigned long long)fixture->wait.frequency);
    return failures;
}

int native_test_lwmutex_horizon(void)
{
    native_fixture fixture = {0};
    if (ps3_horizon_guest_space_create(&fixture.space) != PS3_GUEST_MEMORY_OK) return 1;
    mutexInit(&fixture.domain_mutex);
    ps3_instance_guard domain_guard = {&fixture.domain_mutex, lock_domain, unlock_domain};
    ps3_guest_memory backing = ps3_horizon_guest_space_port(fixture.space);
    int failures = 0;
    if (ps3_atomic_domain_init(&fixture.domain, backing, domain_guard,
            fixture.owners, 8, fixture.pins, 8) != PS3_GUEST_MEMORY_OK) { ++failures; goto destroy; }
    if (ps3_atomic_commit(&fixture.domain, OBJECT_ADDRESS, 4096) != PS3_GUEST_MEMORY_OK) { ++failures; goto destroy; }
    if (ps3_horizon_lwmutex_wait_init(&fixture.wait, 2000000) != PS3_WAIT_READY) { ++failures; goto destroy; }
    failures += test_wait_adapter(&fixture);
    if (ps3_lwmutex_init(&fixture.registry, &fixture.domain,
            ps3_horizon_lwmutex_wait_guard(&fixture.wait), ps3_horizon_lwmutex_wait_port(&fixture.wait),
            fixture.objects, 4, fixture.waiters, 8).status != PS3_LWMUTEX_OK) { ++failures; goto destroy; }
#define CHECK(condition) do { if (!(condition)) { printf("LWMUTEX_HORIZON FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    CHECK(ps3_lwmutex_create(&fixture.registry, OBJECT_ADDRESS, 2, 0x10).status == PS3_LWMUTEX_OK);
    uint32_t expected[6] = {PS3_LWMUTEX_FREE, 0, 0x12, 0, 0, 0};
    for (unsigned index=0; index<6; ++index) {
        uint32_t actual = 0;
        CHECK(read_word(&fixture, index*4, &actual));
        CHECK(index==4 ? actual!=0 : actual==expected[index]);
    }
    CHECK(ps3_atomic_decommit(&fixture.domain, OBJECT_ADDRESS, 4096) == PS3_GUEST_MEMORY_BUSY);
    ps3_lwmutex_thread holder = {1, 100};
    CHECK(ps3_lwmutex_try_lock(&fixture.registry, OBJECT_ADDRESS, holder).status == PS3_LWMUTEX_OK);
    CHECK(ps3_lwmutex_try_lock(&fixture.registry, OBJECT_ADDRESS, holder).status == PS3_LWMUTEX_OK);
    uint32_t recursion = 0;
    CHECK(read_word(&fixture, 12, &recursion) && recursion == 1);
    CHECK(ps3_lwmutex_unlock(&fixture.registry, OBJECT_ADDRESS, (ps3_lwmutex_thread){2,100}).status == PS3_LWMUTEX_NOT_OWNER);
    CHECK(ps3_lwmutex_unlock(&fixture.registry, OBJECT_ADDRESS, holder).status == PS3_LWMUTEX_OK);
    CHECK(ps3_lwmutex_unlock(&fixture.registry, OBJECT_ADDRESS, holder).status == PS3_LWMUTEX_OK);
    CHECK(ps3_lwmutex_destroy(&fixture.registry, OBJECT_ADDRESS).status == PS3_LWMUTEX_OK);
    CHECK(ps3_atomic_decommit(&fixture.domain, OBJECT_ADDRESS, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_atomic_commit(&fixture.domain, OBJECT_ADDRESS, 4096) == PS3_GUEST_MEMORY_OK);
    failures += test_order(&fixture, PS3_LWMUTEX_FIFO);
    failures += test_order(&fixture, PS3_LWMUTEX_PRIORITY);
    failures += test_contending_workers(&fixture);
    failures += test_wait_outcomes(&fixture);
    ps3_lwmutex_result cleaned = ps3_lwmutex_cleanup(&fixture.registry);
    if (cleaned.status != PS3_LWMUTEX_OK) {
        printf("LWMUTEX_HORIZON cleanup_status=%d memory_status=%d\n", cleaned.status, cleaned.memory_status);
        stop_with_live_resources("cleanup");
    }
    CHECK(ps3_atomic_decommit(&fixture.domain, OBJECT_ADDRESS, 4096) == PS3_GUEST_MEMORY_OK);
destroy:
    if (ps3_horizon_guest_space_destroy(&fixture.space) != PS3_GUEST_MEMORY_OK) ++failures;
    printf("LWMUTEX_HORIZON %s failed_checks=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
#undef CHECK
}
