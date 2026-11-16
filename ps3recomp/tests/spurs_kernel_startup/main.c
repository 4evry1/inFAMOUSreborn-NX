#include "spurs_kernel_startup.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct worker {
    volatile int32_t state;
    volatile int32_t *gate;
    uint32_t part;
    uint32_t nparts;
    int observed_part;
    int observed_nparts;
};

static void* run_worker(void* opaque)
{
    struct worker* worker = (struct worker*)opaque;
    if (!ps3_spurs_kernel_claim_start(&worker->state)) return NULL;
    while (!__atomic_load_n(worker->gate, __ATOMIC_ACQUIRE)) sched_yield();
    worker->observed_part = (int)worker->part;
    worker->observed_nparts = (int)worker->nparts;
    ps3_spurs_kernel_mark_live(&worker->state);
    return NULL;
}

static void wait_until_started(struct worker* worker)
{
    while (!ps3_spurs_kernel_is_started(&worker->state)) sched_yield();
}

static void require(int condition, const char* message)
{
    if (condition) return;
    fprintf(stderr, "spurs_kernel_startup: %s\n", message);
    exit(1);
}

static void test_state_transitions(void)
{
    struct worker worker = {0};
    require(ps3_spurs_kernel_claim_start(&worker.state), "first claim must succeed");
    require(!ps3_spurs_kernel_claim_start(&worker.state), "a second claim must fail");
    require(!ps3_spurs_kernel_abandon(&worker.state), "a started worker cannot be abandoned");
    ps3_spurs_kernel_mark_live(&worker.state);
    require(ps3_spurs_kernel_is_live(&worker.state), "worker must report live");
    require(!ps3_spurs_kernel_abandon(&worker.state), "a live worker cannot be abandoned");
}

static void test_gate_assigns_only_live_parts(void)
{
    enum { WORKERS = 4 };
    volatile int32_t gate = 0;
    struct worker workers[WORKERS] = {0};
    pthread_t threads[WORKERS];

    for (int i = 0; i < WORKERS; i++) {
        workers[i].gate = &gate;
        require(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0,
                "worker thread creation failed");
    }
    for (int i = 0; i < WORKERS; i++) wait_until_started(&workers[i]);

    /* Parts are assigned only after every live thread has claimed its slot and
     * before the gate opens. The production path uses the same ordering. */
    for (int i = 0; i < WORKERS; i++) {
        workers[i].part = (uint32_t)i;
        workers[i].nparts = WORKERS;
    }
    __atomic_store_n(&gate, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < WORKERS; i++) {
        require(pthread_join(threads[i], NULL) == 0, "worker join failed");
        require(workers[i].observed_part == i, "worker read the wrong final part");
        require(workers[i].observed_nparts == WORKERS, "worker read the wrong final count");
        require(ps3_spurs_kernel_is_live(&workers[i].state), "worker did not become live");
    }
}

static void test_late_worker_cannot_claim_abandoned_part(void)
{
    volatile int32_t gate = 0;
    struct worker worker = {0};
    pthread_t thread;
    worker.gate = &gate;
    worker.part = 7;
    worker.nparts = 8;
    worker.observed_part = -1;

    require(ps3_spurs_kernel_abandon(&worker.state), "pending worker must be abandonable");
    require(pthread_create(&thread, NULL, run_worker, &worker) == 0,
            "late worker thread creation failed");
    __atomic_store_n(&gate, 1, __ATOMIC_RELEASE);
    require(pthread_join(thread, NULL) == 0, "late worker join failed");

    require(worker.state == PS3_SPURS_KERNEL_ABANDONED, "late worker changed abandoned state");
    require(worker.observed_part == -1, "late worker consumed an abandoned part");
}

int main(void)
{
    test_state_transitions();
    test_gate_assigns_only_live_parts();
    test_late_worker_cannot_claim_abandoned_part();
    puts("spurs_kernel_startup: all tests passed");
    return 0;
}
