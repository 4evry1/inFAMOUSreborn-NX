#include "../adapters/guest_space_horizon.h"
#include "user_memory.h"
#include "atomic_domain.h"
#include <stdio.h>
#include <string.h>
#include <switch.h>

enum { ALLOCATION_CYCLES = 32, WORKER_COUNT = 2 };
#define WORKER_JOIN_TIMEOUT_NS INT64_C(10000000000)
#define USER_BEGIN UINT32_C(0x40000000)
#define SMALL_PAGE PS3_USER_MEMORY_ALIGN_64K
#define LARGE_PAGE PS3_USER_MEMORY_ALIGN_1M

static void guard_lock(void *context)
{
    mutexLock(context);
}

static void guard_unlock(void *context)
{
    mutexUnlock(context);
}

typedef struct allocation_worker {
    ps3_user_memory *allocator;
    ps3_guest_memory memory;
    unsigned char pattern;
    unsigned completed;
    int failures;
} allocation_worker;

static void allocate_write_and_free(void *context)
{
    allocation_worker *worker = context;
    for (unsigned cycle = 0; cycle < ALLOCATION_CYCLES; ++cycle) {
        uint32_t address = 0;
        ps3_user_memory_result allocated = ps3_user_memory_allocate(
            worker->allocator, SMALL_PAGE, SMALL_PAGE, &address);
        if (allocated.status != PS3_USER_MEMORY_OK) {
            ++worker->failures;
            return;
        }
        unsigned char source[64], output[64];
        memset(source, worker->pattern, sizeof(source));
        memset(output, 0, sizeof(output));
        worker->failures += worker->memory.write(worker->memory.context,
            address, source, sizeof(source)) != PS3_GUEST_MEMORY_OK;
        svcSleepThread(1000000);
        worker->failures += worker->memory.read(worker->memory.context,
            address, output, sizeof(output)) != PS3_GUEST_MEMORY_OK;
        worker->failures += memcmp(source, output, sizeof(source)) != 0;
        ps3_user_memory_result freed = ps3_user_memory_free(worker->allocator, address);
        if (freed.status != PS3_USER_MEMORY_OK) {
            ++worker->failures;
            return;
        }
        ++worker->completed;
    }
}

static int test_workers(ps3_user_memory *allocator, ps3_guest_memory memory)
{
    Thread threads[WORKER_COUNT];
    bool started[WORKER_COUNT] = {false};
    allocation_worker workers[WORKER_COUNT] = {
        {.allocator = allocator, .memory = memory, .pattern = 0x35},
        {.allocator = allocator, .memory = memory, .pattern = 0xca}
    };
    int failures = 0;
    for (unsigned index = 0; index < WORKER_COUNT; ++index) {
        Result result = threadCreate(&threads[index], allocate_write_and_free,
            &workers[index], NULL, 0x10000, 0x2c, -2);
        if (R_FAILED(result)) {
            ++failures;
            continue;
        }
        result = threadStart(&threads[index]);
        if (R_FAILED(result)) {
            ++failures;
            failures += R_FAILED(threadClose(&threads[index]));
            continue;
        }
        started[index] = true;
    }
    for (unsigned index = 0; index < WORKER_COUNT; ++index) {
        if (!started[index]) continue;
        Result joined = svcWaitSynchronizationSingle(
            threads[index].handle, WORKER_JOIN_TIMEOUT_NS);
        if (R_FAILED(joined) || R_FAILED(threadWaitForExit(&threads[index]))) {
            printf("USER_MEMORY_HORIZON FAIL worker_join index=%u result=0x%x memory_not_reused=true\n",
                   index, joined);
            fflush(stdout);
            svcExitProcess();
        }
        failures += R_FAILED(threadClose(&threads[index]));
        failures += workers[index].failures;
        failures += workers[index].completed != ALLOCATION_CYCLES;
    }
    printf("USER_MEMORY_HORIZON threads=2 cycles_per_thread=%u completed=%u,%u failures=%d\n",
        ALLOCATION_CYCLES, workers[0].completed, workers[1].completed, failures);
    return failures;
}

int native_test_user_memory_horizon(void)
{
    ps3_horizon_guest_space *space = NULL;
    ps3_guest_memory_status created = ps3_horizon_guest_space_create(&space);
    if (created != PS3_GUEST_MEMORY_OK) {
        printf("USER_MEMORY_HORIZON FAIL create=%d\n", created);
        return 1;
    }
    ps3_guest_memory backing = ps3_horizon_guest_space_port(space);
    Mutex domain_mutex;
    mutexInit(&domain_mutex);
    ps3_atomic_domain domain = {0};
    ps3_atomic_owner_record owners[2] = {0};
    ps3_atomic_pin_record pins[2] = {0};
    ps3_instance_guard domain_guard = {&domain_mutex, guard_lock, guard_unlock};
    if (ps3_atomic_domain_init(&domain, backing, domain_guard,
            owners, 2, pins, 2) != PS3_GUEST_MEMORY_OK) {
        printf("USER_MEMORY_HORIZON FAIL domain_init\n");
        ps3_horizon_guest_space_destroy(&space);
        return 1;
    }
    ps3_guest_memory memory = ps3_atomic_domain_memory(&domain);
    Mutex allocation_mutex;
    mutexInit(&allocation_mutex);
    ps3_user_memory_slot slots[16];
    ps3_user_memory allocator;
    ps3_user_memory_config config = {
        .window_begin = USER_BEGIN,
        .window_end = USER_BEGIN + 4 * LARGE_PAGE,
        .quota_bytes = 2 * LARGE_PAGE,
        .memory = memory,
        .guard = {&allocation_mutex, guard_lock, guard_unlock}
    };
    int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    printf("USER_MEMORY_HORIZON FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    bool initialized = ps3_user_memory_init(&allocator, &config, slots, 16) == PS3_USER_MEMORY_OK;
    CHECK(initialized);
    if (!initialized) goto destroy;
    uint32_t first = 0, second = 0;
    ps3_user_memory_result result = ps3_user_memory_allocate(
        &allocator, LARGE_PAGE, LARGE_PAGE, &first);
    CHECK(result.status == PS3_USER_MEMORY_OK && first == USER_BEGIN);
    if (result.status != PS3_USER_MEMORY_OK) goto cleanup;
    result = ps3_user_memory_allocate(&allocator, LARGE_PAGE, LARGE_PAGE, &second);
    CHECK(result.status == PS3_USER_MEMORY_OK && second == USER_BEGIN + LARGE_PAGE);
    if (result.status != PS3_USER_MEMORY_OK) goto cleanup;
    uint32_t unchanged = 0x12345678;
    result = ps3_user_memory_allocate(&allocator, SMALL_PAGE, SMALL_PAGE, &unchanged);
    CHECK(result.status == PS3_USER_MEMORY_QUOTA_EXCEEDED && unchanged == 0x12345678);
    CHECK(ps3_user_memory_free(&allocator, first + SMALL_PAGE).status == PS3_USER_MEMORY_NOT_OWNED);
    CHECK(ps3_user_memory_free(&allocator, first).status == PS3_USER_MEMORY_OK);
    uint32_t small = 0;
    result = ps3_user_memory_allocate(&allocator, SMALL_PAGE, SMALL_PAGE, &small);
    CHECK(result.status == PS3_USER_MEMORY_OK && small == first);
    if (result.status != PS3_USER_MEMORY_OK) goto cleanup;
    unsigned char source[64], output[64];
    memset(source, 0x9b, sizeof(source));
    CHECK(memory.write(memory.context, small, source, sizeof(source)) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_user_memory_free(&allocator, small).status == PS3_USER_MEMORY_OK);
    result = ps3_user_memory_allocate(&allocator, SMALL_PAGE, SMALL_PAGE, &small);
    CHECK(result.status == PS3_USER_MEMORY_OK && small == first);
    if (result.status != PS3_USER_MEMORY_OK) goto cleanup;
    memset(output, 0xff, sizeof(output));
    CHECK(memory.read(memory.context, small, output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
    for (unsigned index = 0; index < sizeof(output); ++index) CHECK(output[index] == 0);
    ps3_atomic_pin pin;
    ps3_guest_memory_status pinned = ps3_atomic_pin_acquire(&domain, small, SMALL_PAGE, &pin);
    CHECK(pinned == PS3_GUEST_MEMORY_OK);
    if (pinned == PS3_GUEST_MEMORY_OK) {
        int prior_failures = failures;
        ps3_user_memory_info before, after;
        CHECK(ps3_user_memory_query(&allocator, &before) == PS3_USER_MEMORY_OK);
        result = ps3_user_memory_free(&allocator, small);
        CHECK(result.status == PS3_USER_MEMORY_MEMORY_FAILURE);
        CHECK(result.memory_status == PS3_GUEST_MEMORY_BUSY);
        CHECK(ps3_user_memory_query(&allocator, &after) == PS3_USER_MEMORY_OK);
        CHECK(after.used_bytes == before.used_bytes && after.allocation_count == before.allocation_count);
        CHECK(memory.read(memory.context, small, output, sizeof(output)) == PS3_GUEST_MEMORY_OK);
        CHECK(ps3_atomic_pin_release(&domain, &pin) == PS3_GUEST_MEMORY_OK);
        CHECK(ps3_user_memory_free(&allocator, small).status == PS3_USER_MEMORY_OK);
        printf("USER_MEMORY_HORIZON pin_busy_checks=%s\n",
            failures == prior_failures ? "PASS" : "FAIL");
    }
    CHECK(ps3_user_memory_cleanup(&allocator).status == PS3_USER_MEMORY_OK);
    failures += test_workers(&allocator, memory);
    ps3_user_memory_info info;
    CHECK(ps3_user_memory_query(&allocator, &info) == PS3_USER_MEMORY_OK);
    CHECK(info.used_bytes == 0 && info.allocation_count == 0 && info.available_bytes == 2 * LARGE_PAGE);
cleanup:
    result = ps3_user_memory_cleanup(&allocator);
    CHECK(result.status == PS3_USER_MEMORY_OK);
    CHECK(ps3_horizon_guest_space_stats(space).committed_pages == 0);
destroy:
    CHECK(ps3_horizon_guest_space_destroy(&space) == PS3_GUEST_MEMORY_OK);
    CHECK(space == NULL);
    printf("USER_MEMORY_HORIZON %s failed_checks=%d quota_bytes=%llu\n",
        failures ? "FAIL" : "PASS", failures, (unsigned long long)(2 * LARGE_PAGE));
    return failures != 0;
#undef CHECK
}
