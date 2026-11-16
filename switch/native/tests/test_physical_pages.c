#include <switch.h>
#include <inttypes.h>
#include <stdio.h>

enum { PAGE_BYTES = 4096, MAP_PHYSICAL_SVC = 0x2c, UNMAP_PHYSICAL_SVC = 0x2d };
static const size_t GUEST_WINDOW_BYTES = UINT64_C(1) << 32;

static bool covers_free_window(uintptr_t address)
{
    MemoryInfo info = {0};
    u32 page_info = 0;
    return R_SUCCEEDED(svcQueryMemory(&info, &page_info, address)) &&
        (info.type & MemState_Type) == MemType_Unmapped &&
        info.addr <= address && info.size >= GUEST_WINDOW_BYTES &&
        address - info.addr <= info.size - GUEST_WINDOW_BYTES;
}

int native_test_physical_pages(void)
{
    if (!envIsSyscallHinted(MAP_PHYSICAL_SVC) ||
        !envIsSyscallHinted(UNMAP_PHYSICAL_SVC)) {
        printf("PHYSICAL_PAGES BLOCKED map/unmap-physical not advertised\n");
        return 1;
    }
    u64 alias_base = 0, alias_size = 0;
    Result result = svcGetInfo(&alias_base, InfoType_AliasRegionAddress,
                               CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(result))
        result = svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(result) || alias_size < GUEST_WINDOW_BYTES ||
        alias_base > UINTPTR_MAX - alias_size) {
        printf("PHYSICAL_PAGES FAIL alias bounds result=0x%x\n", result);
        return 1;
    }
    uintptr_t base = (alias_base + GUEST_WINDOW_BYTES - 1) & ~(GUEST_WINDOW_BYTES - 1);
    const uintptr_t last_base = alias_base + alias_size - GUEST_WINDOW_BYTES;
    VirtmemReservation *reservation = NULL;
    virtmemLock();
    for (; base <= last_base; base += GUEST_WINDOW_BYTES) {
        if (covers_free_window(base)) {
            reservation = virtmemAddReservation((void *)base, GUEST_WINDOW_BYTES);
            break;
        }
    }
    virtmemUnlock();
    if (!reservation) {
        printf("PHYSICAL_PAGES FAIL no alias window\n");
        return 1;
    }

    const uintptr_t pages[] = {base + 0x10000, base + 0xd0010000};
    bool mapped[2] = {false, false};
    int failures = 0;
    printf("PHYSICAL_PAGES base=0x%" PRIxPTR " window=0x%zx\n", base, GUEST_WINDOW_BYTES);
    for (size_t i = 0; i < 2; ++i) {
        result = svcMapPhysicalMemory((void *)pages[i], PAGE_BYTES);
        printf("PHYSICAL_PAGES map index=%zu result=0x%x\n", i, result);
        if (R_FAILED(result)) { ++failures; goto cleanup; }
        mapped[i] = true;
        volatile u32 *page = (volatile u32 *)pages[i];
        for (size_t word = 0; word < PAGE_BYTES / sizeof(u32); ++word) {
            failures += page[word] != 0;
            page[word] = (u32)(0xabcd0000u + (i << 12) + word);
        }
    }
    for (size_t i = 0; i < 2; ++i) {
        volatile u32 *page = (volatile u32 *)pages[i];
        for (size_t word = 0; word < PAGE_BYTES / sizeof(u32); ++word)
            failures += page[word] != (u32)(0xabcd0000u + (i << 12) + word);
    }
    result = svcMapPhysicalMemory((void *)pages[0], PAGE_BYTES);
    printf("PHYSICAL_PAGES recommit result=0x%x\n", result);
    failures += R_FAILED(result);
    for (size_t word = 0; word < PAGE_BYTES / sizeof(u32); ++word)
        failures += ((volatile u32 *)pages[0])[word] != (u32)(0xabcd0000u + word);
    result = svcUnmapPhysicalMemory((void *)pages[0], PAGE_BYTES);
    printf("PHYSICAL_PAGES decommit result=0x%x\n", result);
    if (R_FAILED(result)) { ++failures; goto cleanup; }
    mapped[0] = false;
    result = svcMapPhysicalMemory((void *)pages[0], PAGE_BYTES);
    printf("PHYSICAL_PAGES remap result=0x%x\n", result);
    if (R_FAILED(result)) { ++failures; goto cleanup; }
    mapped[0] = true;
    for (size_t word = 0; word < PAGE_BYTES / sizeof(u32); ++word)
        failures += ((volatile u32 *)pages[0])[word] != 0;
    for (size_t word = 0; word < PAGE_BYTES / sizeof(u32); ++word)
        failures += ((volatile u32 *)pages[1])[word] != (u32)(0xabcd1000u + word);

cleanup:
    for (size_t i = 0; i < 2; ++i) {
        if (!mapped[i]) continue;
        result = svcUnmapPhysicalMemory((void *)pages[i], PAGE_BYTES);
        printf("PHYSICAL_PAGES unmap index=%zu result=0x%x\n", i, result);
        failures += R_FAILED(result);
    }
    failures += !covers_free_window(base);
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    printf("PHYSICAL_PAGES %s failed_checks=%d game_memory_tested=false\n",
           failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
