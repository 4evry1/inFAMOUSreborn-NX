#include <switch.h>
#include <inttypes.h>
#include <stdio.h>

int native_test_sparse_pages(uintptr_t base);

int native_test_memory_layout(bool test_sparse_pages)
{
    const struct { const char *name; InfoType type; } queries[] = {
        {"total_memory", InfoType_TotalMemorySize},
        {"used_memory", InfoType_UsedMemorySize},
        {"alias_base", InfoType_AliasRegionAddress},
        {"alias_size", InfoType_AliasRegionSize},
        {"stack_base", InfoType_StackRegionAddress},
        {"stack_size", InfoType_StackRegionSize},
        {"address_space_base", InfoType_AslrRegionAddress},
        {"address_space_size", InfoType_AslrRegionSize},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        u64 value = 0;
        Result result = svcGetInfo(&value, queries[i].type, CUR_PROCESS_HANDLE, 0);
        printf("MEMORY %s=0x%" PRIx64 " result=0x%x\n", queries[i].name, value, result);
        failures += R_FAILED(result);
    }
    printf("MEMORY applet_type=%d shared_memory_create_hint=%d\n",
           appletGetAppletType(), envIsSyscallHinted(0x50));

    const size_t guest_size = UINT64_C(1) << 32;
    virtmemLock();
    void *slice = virtmemFindAslr(guest_size * 2, 4096);
    if (!slice) {
        virtmemUnlock();
        printf("GUEST_WINDOW_VA FAIL no free aligned-window candidate\n");
        return 1;
    }
    uintptr_t base = ((uintptr_t)slice + guest_size - 1) & ~(guest_size - 1);
    VirtmemReservation *reservation = virtmemAddReservation((void *)base, guest_size);
    virtmemUnlock();
    if (!reservation) {
        printf("GUEST_WINDOW_VA FAIL reservation allocation\n");
        return 1;
    }
    MemoryInfo info = {0};
    u32 page_info = 0;
    Result result = svcQueryMemory(&info, &page_info, base);
    const bool is_free = R_SUCCEEDED(result) &&
        (info.type & MemState_Type) == MemType_Unmapped &&
        info.addr <= base && info.size >= guest_size &&
        base - info.addr <= info.size - guest_size;
    failures += !is_free;
    printf("GUEST_WINDOW_VA %s base=0x%" PRIxPTR " size=0x%zx "
           "reservation_only=true physical_backing_tested=false\n",
           is_free ? "PASS" : "FAIL", base, guest_size);
    if (is_free && test_sparse_pages) failures += native_test_sparse_pages(base);
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    return failures != 0;
}
