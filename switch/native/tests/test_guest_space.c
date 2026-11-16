#include "../adapters/guest_space_horizon.h"
#include <stdio.h>
#include <switch.h>
#include <string.h>

enum { CONCURRENT_READS = 400, CONCURRENT_MAPPING_CYCLES = 200 };
typedef struct concurrent_guest_test {
    ps3_guest_memory memory;
    int failures;
} concurrent_guest_test;

static void read_while_decommitting(void *argument)
{
    concurrent_guest_test *test = argument;
    for (unsigned iteration = 0; iteration < CONCURRENT_READS; ++iteration) {
        unsigned char output[32];
        memset(output, 0x55, sizeof(output));
        ps3_guest_memory_status status = test->memory.read(
            test->memory.context, 0x30000, output, sizeof(output));
        if (status == PS3_GUEST_MEMORY_UNMAPPED) {
            for (unsigned byte = 0; byte < sizeof(output); ++byte) {
                test->failures += output[byte] != 0x55;
            }
        } else if (status == PS3_GUEST_MEMORY_OK) {
            unsigned char value = output[0];
            test->failures += value != 0 && value != 0xab;
            for (unsigned byte = 0; byte < sizeof(output); ++byte) {
                test->failures += output[byte] != value;
            }
        } else {
            ++test->failures;
        }
        svcSleepThread(1000000);
    }
}

typedef struct ownership_stress {
    int failures;
} ownership_stress;

static void create_and_destroy_owners(void *argument)
{
    ownership_stress *stress = argument;
    for (unsigned iteration = 0; iteration < 200; ++iteration) {
        ps3_horizon_guest_space *space = NULL;
        if (ps3_horizon_guest_space_create(&space) != PS3_GUEST_MEMORY_OK) {
            ++stress->failures;
            return;
        }
        ps3_guest_memory memory = ps3_horizon_guest_space_port(space);
        unsigned char value = (unsigned char)iteration;
        unsigned char readback = 0;
        stress->failures += memory.commit(memory.context, 0x50000, 4096) != PS3_GUEST_MEMORY_OK;
        stress->failures += memory.write(memory.context, 0x50000, &value, 1) != PS3_GUEST_MEMORY_OK;
        stress->failures += memory.read(memory.context, 0x50000, &readback, 1) != PS3_GUEST_MEMORY_OK;
        stress->failures += readback != value;
        svcSleepThread(0);
        if (ps3_horizon_guest_space_destroy(&space) != PS3_GUEST_MEMORY_OK) {
            ++stress->failures;
            for (unsigned retry = 0; space && retry < 20; ++retry) {
                svcSleepThread(1000000);
                ps3_horizon_guest_space_destroy(&space);
            }
            if (space) return;
        }
    }
}

static int test_concurrent_ownership(void)
{
    ownership_stress workers[2] = {{0}, {0}};
    Thread threads[2];
    bool started[2] = {false, false};
    int failures = 0;
    for (unsigned index = 0; index < 2; ++index) {
        Result result = threadCreate(&threads[index], create_and_destroy_owners,
                                     &workers[index], NULL, 0x10000, 0x2c, -2);
        if (R_FAILED(result)) {
            ++failures;
            continue;
        }
        result = threadStart(&threads[index]);
        if (R_FAILED(result)) {
            ++failures;
            threadClose(&threads[index]);
            continue;
        }
        started[index] = true;
    }
    for (unsigned index = 0; index < 2; ++index) {
        if (!started[index]) continue;
        failures += R_FAILED(threadWaitForExit(&threads[index]));
        failures += R_FAILED(threadClose(&threads[index]));
        failures += workers[index].failures;
    }
    printf("GUEST_SPACE ownership_stress_threads=2 cycles_per_thread=200 failures=%d\n", failures);
    return failures;
}

int native_test_guest_space(void)
{
    ps3_horizon_guest_space *space = NULL;
    if (ps3_horizon_guest_space_create(&space) != PS3_GUEST_MEMORY_OK) return 1;
    ps3_guest_memory memory = ps3_horizon_guest_space_port(space);
    int failures = 0;
#define CHECK(x) do { if (!(x)) { printf("GUEST_SPACE FAIL line=%d\n", __LINE__); ++failures; } } while (0)
    ps3_horizon_guest_space *second_space = NULL;
    CHECK(ps3_horizon_guest_space_create(&second_space) == PS3_GUEST_MEMORY_OK);
    if (!second_space || ps3_horizon_guest_space_base(space) == ps3_horizon_guest_space_base(second_space)) {
        printf("GUEST_SPACE FAIL fresh-owner collision\n");
        if (second_space) ps3_horizon_guest_space_destroy(&second_space);
        ps3_horizon_guest_space_destroy(&space);
        return 1;
    }
    ps3_guest_memory second_memory = ps3_horizon_guest_space_port(second_space);
    CHECK(ps3_horizon_guest_space_stats(space).committed_pages == 0);
    CHECK(ps3_horizon_guest_space_stats(space).ownership_pages == 1);
    CHECK(ps3_horizon_guest_space_stats(second_space).committed_pages == 0);
    CHECK(ps3_horizon_guest_space_stats(second_space).ownership_pages == 1);
    printf("GUEST_SPACE owners=2 ownership_pages_per_owner=1 distinct_bases=true\n");
    unsigned char first_owner_data[32], second_owner_data[32], owner_readback[32];
    memset(first_owner_data, 0x12, sizeof(first_owner_data));
    memset(second_owner_data, 0x34, sizeof(second_owner_data));
    CHECK(memory.commit(memory.context, 0x80000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(second_memory.commit(second_memory.context, 0x80000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, 0x80000, first_owner_data, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(second_memory.write(second_memory.context, 0x80000, second_owner_data, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, 0x80000, owner_readback, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(owner_readback, first_owner_data, 32) == 0);
    CHECK(second_memory.read(second_memory.context, 0x80000, owner_readback, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(owner_readback, second_owner_data, 32) == 0);
    CHECK(second_memory.commit(second_memory.context, 0xfffff000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(second_memory.write(second_memory.context, 0xffffffe0, second_owner_data, 32) == PS3_GUEST_MEMORY_OK);
    uintptr_t released_base = ps3_horizon_guest_space_base(second_space);
    MemoryInfo marker_info = {0};
    u32 marker_page_info = 0;
    CHECK(R_SUCCEEDED(svcQueryMemory(&marker_info, &marker_page_info,
                                      released_base - PS3_GUEST_PAGE_BYTES)));
    CHECK((marker_info.type & MemState_Type) != MemType_Unmapped);
    CHECK(ps3_horizon_guest_space_destroy(&second_space) == PS3_GUEST_MEMORY_OK);
    CHECK(R_SUCCEEDED(svcQueryMemory(&marker_info, &marker_page_info,
                                      released_base - PS3_GUEST_PAGE_BYTES)));
    CHECK((marker_info.type & MemState_Type) == MemType_Unmapped);
    CHECK(memory.read(memory.context, 0x80000, owner_readback, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(owner_readback, first_owner_data, 32) == 0);
    CHECK(memory.decommit(memory.context, 0x80000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_horizon_guest_space_create(&second_space) == PS3_GUEST_MEMORY_OK);
    CHECK(second_space && ps3_horizon_guest_space_base(second_space) == released_base);
    CHECK(ps3_horizon_guest_space_destroy(&second_space) == PS3_GUEST_MEMORY_OK);
    unsigned char source[32], output[32];
    memset(source, 0xab, sizeof(source));
    memset(output, 0x55, sizeof(output));
    CHECK(memory.commit(memory.context, 1, 4096) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.commit(memory.context, 0, 0) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.commit(memory.context, 0xfffff000, 8192) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.read(memory.context, 0xfffffff0, output, 32) == PS3_GUEST_MEMORY_INVALID_RANGE);
    CHECK(memory.read(memory.context, 0x10000, output, 32) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(output[0] == 0x55 && output[31] == 0x55);
    CHECK(memory.commit(memory.context, 0x10000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, 0x10ff0, source, 32) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(memory.read(memory.context, 0x10ff0, output, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(output[0] == 0 && output[15] == 0);
    CHECK(memory.commit(memory.context, 0x10000, 8192) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, 0x10ff0, source, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.commit(memory.context, 0x10000, 8192) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, 0x10ff0, output, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(source, output, 32) == 0);
    CHECK(memory.zero(memory.context, 0x10ff8, 16) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, 0x10ff0, output, 32) == PS3_GUEST_MEMORY_OK);
    for (int i=0;i<32;++i) CHECK(output[i] == (i>=8 && i<24 ? 0 : 0xab));
    CHECK(memory.decommit(memory.context, 0x10000, 4096) == PS3_GUEST_MEMORY_OK);
    memset(output, 0x55, 32);
    CHECK(memory.read(memory.context, 0x10ff0, output, 32) == PS3_GUEST_MEMORY_UNMAPPED);
    for (int i=0;i<32;++i) CHECK(output[i] == 0x55);
    CHECK(memory.commit(memory.context, 0x10000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, 0x10000, output, 32) == PS3_GUEST_MEMORY_OK);
    for (int i=0;i<32;++i) CHECK(output[i] == 0);
    CHECK(memory.commit(memory.context, 0xfffff000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, 0xffffffe0, source, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.read(memory.context, 0xffffffe0, output, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(source, output, 32) == 0);
    CHECK(memory.commit(memory.context, 0xd0010000, 4096) == PS3_GUEST_MEMORY_OK);
    CHECK(memory.write(memory.context, 0xd0010000, source, 32) == PS3_GUEST_MEMORY_OK);
    ps3_horizon_guest_stats stats = ps3_horizon_guest_space_stats(space);
    CHECK(stats.committed_pages == 4);
    ps3_guest_memory_status rejected = memory.commit(memory.context, 0, PS3_GUEST_ADDRESS_BYTES);
    printf("GUEST_SPACE resource_failure status=%d kernel=0x%x\n", rejected,
           ps3_horizon_guest_space_stats(space).last_kernel_result);
    CHECK(rejected == PS3_GUEST_MEMORY_PLATFORM_FAILURE);
    CHECK(ps3_horizon_guest_space_stats(space).committed_pages == 4);
    CHECK(memory.read(memory.context, 0xffffffe0, output, 32) == PS3_GUEST_MEMORY_OK);
    CHECK(memcmp(source, output, 32) == 0);
    CHECK(memory.read(memory.context, 0x20000, output, 32) == PS3_GUEST_MEMORY_UNMAPPED);
    CHECK(memory.decommit(memory.context, 0x10000, 12288) == PS3_GUEST_MEMORY_OK);
    CHECK(ps3_horizon_guest_space_stats(space).committed_pages == 2);
    concurrent_guest_test concurrent = {memory, 0};
    Thread reader;
    Result thread_result = threadCreate(&reader, read_while_decommitting, &concurrent,
                                        NULL, 0x10000, 0x2c, -2);
    CHECK(R_SUCCEEDED(thread_result));
    if (R_SUCCEEDED(thread_result)) {
        thread_result = threadStart(&reader);
        CHECK(R_SUCCEEDED(thread_result));
        if (R_SUCCEEDED(thread_result)) {
            for (unsigned iteration = 0; iteration < CONCURRENT_MAPPING_CYCLES; ++iteration) {
                CHECK(memory.commit(memory.context,0x30000,4096)==PS3_GUEST_MEMORY_OK);
                CHECK(memory.write(memory.context,0x30000,source,32)==PS3_GUEST_MEMORY_OK);
                svcSleepThread(1000000);
                CHECK(memory.decommit(memory.context,0x30000,4096)==PS3_GUEST_MEMORY_OK);
                svcSleepThread(1000000);
            }
            CHECK(R_SUCCEEDED(threadWaitForExit(&reader)));
            CHECK(concurrent.failures == 0);
        }
        CHECK(R_SUCCEEDED(threadClose(&reader)));
    }
    CHECK((ps3_horizon_guest_space_base(space) & (PS3_GUEST_ADDRESS_BYTES-1)) == 0);
    CHECK(ps3_horizon_guest_space_destroy(&space) == PS3_GUEST_MEMORY_OK);
    CHECK(space == NULL);
    failures += test_concurrent_ownership();
    printf("GUEST_SPACE %s failed_checks=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
