#include "../adapters/clock_horizon.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>

enum { CLOCK_WORKERS = 2, CLOCK_SAMPLES = 256 };
typedef struct clock_reader {
    ps3_clock port;
    unsigned completed;
    int failures;
} clock_reader;
static void read_clock(void *context)
{
    clock_reader *reader = context;
    uint64_t previous = 0;
    for (unsigned index = 0; index < CLOCK_SAMPLES; ++index) {
        uint64_t current = UINT64_MAX;
        if (reader->port.now_microseconds(reader->port.context, &current) != PS3_CLOCK_OK || current < previous) {
            ++reader->failures;
            return;
        }
        previous = current;
        ++reader->completed;
    }
}
int native_test_clock_horizon(void)
{
    int failures = 0;
#define CHECK(value) do { if (!(value)) { printf("CLOCK_HORIZON_FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    ps3_horizon_clock first = {0}, second = {0};
    CHECK(ps3_horizon_clock_init(&first) == PS3_CLOCK_OK);
    ps3_clock port = ps3_horizon_clock_port(&first);
    uint64_t before = 0, after = 0;
    CHECK(port.now_microseconds(port.context, &before) == PS3_CLOCK_OK);
    svcSleepThread(4000000);
    CHECK(port.now_microseconds(port.context, &after) == PS3_CLOCK_OK);
    CHECK(after >= before && after - before >= 3000);
    CHECK(ps3_horizon_clock_init(&second) == PS3_CLOCK_OK);
    uint64_t older = 0, newer = 0;
    ps3_clock second_port = ps3_horizon_clock_port(&second);
    CHECK(second_port.now_microseconds(second_port.context, &newer) == PS3_CLOCK_OK);
    CHECK(port.now_microseconds(port.context, &older) == PS3_CLOCK_OK);
    CHECK(older >= newer + 3000);
    ps3_horizon_clock invalid = {0};
    ps3_clock invalid_port = ps3_horizon_clock_port(&invalid);
    uint64_t unchanged = UINT64_C(0x123456789abcdef0);
    CHECK(invalid_port.now_microseconds(invalid_port.context, &unchanged) == PS3_CLOCK_ARGUMENT);
    CHECK(unchanged == UINT64_C(0x123456789abcdef0));
    clock_reader readers[CLOCK_WORKERS] = {{.port=port}, {.port=port}};
    Thread workers[CLOCK_WORKERS];
    bool started[CLOCK_WORKERS] = {false};
    for (unsigned index = 0; index < CLOCK_WORKERS; ++index) {
        Result result = threadCreate(&workers[index], read_clock, &readers[index], NULL, 0x8000, 0x2c, -2);
        CHECK(R_SUCCEEDED(result));
        if (R_FAILED(result)) continue;
        result = threadStart(&workers[index]);
        CHECK(R_SUCCEEDED(result));
        if (R_FAILED(result)) { CHECK(R_SUCCEEDED(threadClose(&workers[index]))); continue; }
        started[index] = true;
    }
    for (unsigned index = 0; index < CLOCK_WORKERS; ++index) {
        if (!started[index]) continue;
        if (R_FAILED(svcWaitSynchronizationSingle(workers[index].handle, INT64_C(10000000000))) ||
            R_FAILED(threadWaitForExit(&workers[index])) || R_FAILED(threadClose(&workers[index]))) {
            printf("CLOCK_HORIZON_FATAL worker_unjoined=true clock_retained=true\n");
            fflush(stdout);
            svcExitProcess();
        }
        CHECK(readers[index].completed == CLOCK_SAMPLES && readers[index].failures == 0);
    }
    printf("CLOCK_HORIZON_%s failures=%d elapsed_us=%llu frequency=%llu concurrent_samples=%u,%u\n",
        failures ? "FAIL" : "PASS", failures, (unsigned long long)(after-before),
        (unsigned long long)first.frequency, readers[0].completed, readers[1].completed);
    return failures != 0;
#undef CHECK
}
