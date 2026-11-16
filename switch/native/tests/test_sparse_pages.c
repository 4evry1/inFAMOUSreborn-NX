#include <switch.h>
#include <inttypes.h>
#include <stdio.h>

enum { TEST_PAGE_SIZE = 4096, CREATE_SHARED_MEMORY_SVC = 0x50 };

static int check_page_state(uintptr_t address, MemoryType expected)
{
    MemoryInfo info = {0};
    u32 page_info = 0;
    Result result = svcQueryMemory(&info, &page_info, address);
    return R_FAILED(result) || (info.type & MemState_Type) != expected;
}

int native_test_sparse_pages(uintptr_t base)
{
    if (!envIsSyscallHinted(CREATE_SHARED_MEMORY_SVC)) {
        printf("SPARSE_PAGES BLOCKED shared-memory-create not advertised\n");
        return 1;
    }

    const uintptr_t addresses[] = {base + 0x10000, base + 0xd0010000};
    Handle handles[2] = {INVALID_HANDLE, INVALID_HANDLE};
    bool created[2] = {false, false};
    bool mapped[2] = {false, false};
    size_t backing_created = 0;
    int failures = 0;
    for (size_t i = 0; i < 2; ++i) {
        Result result = svcCreateSharedMemory(&handles[i], TEST_PAGE_SIZE,
                                              Perm_Rw, Perm_Rw);
        printf("SPARSE_PAGES create index=%zu result=0x%x\n", i, result);
        if (R_FAILED(result)) { ++failures; goto cleanup; }
        created[i] = true;
        backing_created += TEST_PAGE_SIZE;
        result = svcMapSharedMemory(handles[i], (void *)addresses[i],
                                    TEST_PAGE_SIZE, Perm_Rw);
        printf("SPARSE_PAGES map index=%zu offset=0x%" PRIxPTR " result=0x%x\n",
               i, addresses[i] - base, result);
        if (R_FAILED(result)) { ++failures; goto cleanup; }
        mapped[i] = true;
        failures += check_page_state(addresses[i], MemType_SharedMem);
    }
    if (failures) goto cleanup;

    for (size_t i = 0; i < 2; ++i) {
        volatile u32 *page = (volatile u32 *)addresses[i];
        for (size_t word = 0; word < TEST_PAGE_SIZE / sizeof(u32); ++word) {
            failures += page[word] != 0;
            page[word] = (u32)(0x12340000u + (i << 12) + word);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        volatile u32 *page = (volatile u32 *)addresses[i];
        for (size_t word = 0; word < TEST_PAGE_SIZE / sizeof(u32); ++word)
            failures += page[word] != (u32)(0x12340000u + (i << 12) + word);
    }
    failures += check_page_state(base + 0x10000000, MemType_Unmapped);

    Result overlap = svcMapSharedMemory(handles[1], (void *)addresses[0],
                                        TEST_PAGE_SIZE, Perm_Rw);
    printf("SPARSE_PAGES overlapping-map result=0x%x expected=failure\n", overlap);
    if (R_SUCCEEDED(overlap)) {
        ++failures;
        svcUnmapSharedMemory(handles[1], (void *)addresses[0], TEST_PAGE_SIZE);
        goto cleanup;
    }
    failures += *(volatile u32 *)addresses[0] != 0x12340000u;
    failures += *(volatile u32 *)addresses[1] != 0x12341000u;

cleanup:
    for (size_t i = 0; i < 2; ++i) {
        if (mapped[i]) {
            Result result = svcUnmapSharedMemory(handles[i], (void *)addresses[i],
                                                TEST_PAGE_SIZE);
            printf("SPARSE_PAGES unmap index=%zu result=0x%x\n", i, result);
            failures += R_FAILED(result);
            failures += check_page_state(addresses[i], MemType_Unmapped);
        }
        if (created[i])
            failures += R_FAILED(svcCloseHandle(handles[i]));
    }
    printf("SPARSE_PAGES %s backing_bytes_created=%zu game_memory_tested=false\n",
           failures ? "FAIL" : "PASS", backing_created);
    return failures != 0;
}
