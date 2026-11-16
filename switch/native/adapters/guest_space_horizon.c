#include "guest_space_horizon.h"
#include <switch.h>
#include <stdlib.h>
#include <string.h>

enum {
    PAGE_COUNT = 1048576,
    BITMAP_BYTES = PAGE_COUNT / 8,
    BITS_PER_BYTE = 8
};
static const uint64_t CANDIDATE_STRIDE = PS3_GUEST_ADDRESS_BYTES * 2;
typedef enum transfer_operation { READ_GUEST, WRITE_GUEST, ZERO_GUEST } transfer_operation;
struct ps3_horizon_guest_space {
    uintptr_t base;
    VirtmemReservation *reservation;
    Mutex mutex;
    ps3_horizon_guest_stats stats;
    unsigned char committed[BITMAP_BYTES];
};
static bool valid_range(uint32_t address, uint64_t length)
{
    return length <= PS3_GUEST_ADDRESS_BYTES - address;
}

static bool is_committed(ps3_horizon_guest_space *space, uint64_t page)
{
    return (space->committed[page / BITS_PER_BYTE] >> (page % BITS_PER_BYTE)) & 1;
}

static void record_pages(ps3_horizon_guest_space *space, uint32_t address,
                         uint64_t length, bool commit)
{
    const uint64_t last_page = ((uint64_t)address + length) / PS3_GUEST_PAGE_BYTES;
    for (uint64_t page = address / PS3_GUEST_PAGE_BYTES; page < last_page; ++page) {
        const bool was_committed = is_committed(space, page);
        const unsigned char mask = 1u << (page % BITS_PER_BYTE);
        if (commit) space->committed[page / BITS_PER_BYTE] |= mask;
        else space->committed[page / BITS_PER_BYTE] &= ~mask;
        if (was_committed != commit) {
            if (commit) ++space->stats.committed_pages;
            else --space->stats.committed_pages;
        }
    }
}

static ps3_guest_memory_status map_range(void *context, uint32_t address,
                                        uint64_t length, bool commit)
{
    ps3_horizon_guest_space *space = context;
    if (!space || !length || !valid_range(address, length) ||
        address % PS3_GUEST_PAGE_BYTES || length % PS3_GUEST_PAGE_BYTES) {
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    mutexLock(&space->mutex);
    /* One kernel transaction avoids partially applying a sequence of page calls. */
    Result result = commit
        ? svcMapPhysicalMemory((void *)(space->base + address), length)
        : svcUnmapPhysicalMemory((void *)(space->base + address), length);
    space->stats.last_kernel_result = result;
    if (R_SUCCEEDED(result)) record_pages(space, address, length, commit);
    mutexUnlock(&space->mutex);
    return R_SUCCEEDED(result) ? PS3_GUEST_MEMORY_OK : PS3_GUEST_MEMORY_PLATFORM_FAILURE;
}

static ps3_guest_memory_status commit_range(void *space, uint32_t address, uint64_t length)
{
    return map_range(space, address, length, true);
}

static ps3_guest_memory_status decommit_range(void *space, uint32_t address, uint64_t length)
{
    return map_range(space, address, length, false);
}

static bool fully_committed(ps3_horizon_guest_space *space, uint32_t address, uint64_t length)
{
    if (!length) return true;
    const uint64_t last_page = ((uint64_t)address + length - 1) / PS3_GUEST_PAGE_BYTES;
    for (uint64_t page = address / PS3_GUEST_PAGE_BYTES; page <= last_page; ++page) {
        if (!is_committed(space, page)) return false;
    }
    return true;
}

static ps3_guest_memory_status transfer(void *context, uint32_t address, void *buffer,
                                       uint64_t length, transfer_operation operation)
{
    ps3_horizon_guest_space *space = context;
    if (!space || !valid_range(address, length) ||
        (operation != ZERO_GUEST && length && !buffer)) {
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    mutexLock(&space->mutex);
    if (!fully_committed(space, address, length)) {
        mutexUnlock(&space->mutex);
        return PS3_GUEST_MEMORY_UNMAPPED;
    }
    if (length) {
        void *guest = (void *)(space->base + address);
        if (operation == READ_GUEST) memmove(buffer, guest, length);
        else if (operation == WRITE_GUEST) memmove(guest, buffer, length);
        else memset(guest, 0, length);
    }
    mutexUnlock(&space->mutex);
    return PS3_GUEST_MEMORY_OK;
}

static ps3_guest_memory_status read_range(void *space, uint32_t address,
                                         void *buffer, size_t length)
{
    return transfer(space, address, buffer, length, READ_GUEST);
}

static ps3_guest_memory_status write_range(void *space, uint32_t address,
                                          const void *buffer, size_t length)
{
    return transfer(space, address, (void *)buffer, length, WRITE_GUEST);
}

static ps3_guest_memory_status zero_range(void *space, uint32_t address, uint64_t length)
{
    return transfer(space, address, NULL, length, ZERO_GUEST);
}

static bool query_free_range(uintptr_t address, uint64_t length, Result *query_result)
{
    MemoryInfo info = {0};
    u32 page_info = 0;
    Result result = svcQueryMemory(&info, &page_info, address);
    if (query_result) *query_result = result;
    return R_SUCCEEDED(result) &&
        (info.type & MemState_Type) == MemType_Unmapped &&
        info.addr <= address && info.size >= length &&
        address - info.addr <= info.size - length;
}

static bool is_free_range(uintptr_t address, uint64_t length)
{
    return query_free_range(address, length, NULL);
}

static void create_diagnostic(ps3_horizon_guest_create_diagnostic *diagnostic,
    ps3_horizon_guest_create_stage stage, int result_valid, Result result)
{
    if (diagnostic) *diagnostic = (ps3_horizon_guest_create_diagnostic){stage, result, result_valid};
}

const char *ps3_horizon_guest_create_stage_name(ps3_horizon_guest_create_stage stage)
{
    switch (stage) {
    case PS3_HORIZON_CREATE_COMPLETE: return "complete";
    case PS3_HORIZON_CREATE_ARGUMENT: return "validate-output";
    case PS3_HORIZON_CREATE_ALLOCATE: return "calloc-context";
    case PS3_HORIZON_CREATE_ALIAS_ADDRESS: return "svcGetInfo-alias-address";
    case PS3_HORIZON_CREATE_ALIAS_SIZE: return "svcGetInfo-alias-size";
    case PS3_HORIZON_CREATE_ALIAS_BOUNDS: return "validate-alias-bounds";
    case PS3_HORIZON_CREATE_QUERY_MEMORY: return "svcQueryMemory-window";
    case PS3_HORIZON_CREATE_FIND_WINDOW: return "find-unmapped-window";
    case PS3_HORIZON_CREATE_RESERVATION: return "virtmemAddReservation";
    case PS3_HORIZON_CREATE_MAP_MARKER: return "svcMapPhysicalMemory-marker";
    default: return "unknown";
    }
}

ps3_guest_memory_status ps3_horizon_guest_space_create(ps3_horizon_guest_space **output)
{
    return ps3_horizon_guest_space_create_diagnostic(output, NULL);
}

ps3_guest_memory_status ps3_horizon_guest_space_create_diagnostic(
    ps3_horizon_guest_space **output, ps3_horizon_guest_create_diagnostic *diagnostic)
{
    create_diagnostic(diagnostic, PS3_HORIZON_CREATE_ARGUMENT, 0, 0);
    if (!output || *output) return PS3_GUEST_MEMORY_INVALID_RANGE;
    create_diagnostic(diagnostic, PS3_HORIZON_CREATE_ALLOCATE, 0, 0);
    ps3_horizon_guest_space *space = calloc(1, sizeof(*space));
    if (!space) return PS3_GUEST_MEMORY_NO_MEMORY;
    mutexInit(&space->mutex);

    u64 alias_address = 0;
    u64 alias_size = 0;
    Result result = svcGetInfo(&alias_address, InfoType_AliasRegionAddress,
                               CUR_PROCESS_HANDLE, 0);
    create_diagnostic(diagnostic, PS3_HORIZON_CREATE_ALIAS_ADDRESS, 1, result);
    if (R_SUCCEEDED(result)) {
        result = svcGetInfo(&alias_size, InfoType_AliasRegionSize,
                            CUR_PROCESS_HANDLE, 0);
        create_diagnostic(diagnostic, PS3_HORIZON_CREATE_ALIAS_SIZE, 1, result);
    }
    const uint64_t reservation_bytes = PS3_GUEST_ADDRESS_BYTES + PS3_GUEST_PAGE_BYTES;
    if (R_FAILED(result) || alias_size < reservation_bytes ||
        alias_address > UINTPTR_MAX - alias_size) {
        if (R_SUCCEEDED(result))
            create_diagnostic(diagnostic, PS3_HORIZON_CREATE_ALIAS_BOUNDS, 0, 0);
        free(space);
        return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    }

    uintptr_t base = (alias_address + reservation_bytes - 1) &
                     ~(PS3_GUEST_ADDRESS_BYTES - 1);
    const uintptr_t last_base = alias_address + alias_size - PS3_GUEST_ADDRESS_BYTES;
    ps3_guest_memory_status status = PS3_GUEST_MEMORY_NO_MEMORY;
    create_diagnostic(diagnostic, PS3_HORIZON_CREATE_FIND_WINDOW, 0, 0);
    virtmemLock();
    while (base <= last_base) {
        const uintptr_t marker = base - PS3_GUEST_PAGE_BYTES;
        Result query_result = 0;
        if (query_free_range(marker, reservation_bytes, &query_result)) {
            create_diagnostic(diagnostic, PS3_HORIZON_CREATE_RESERVATION, 0, 0);
            space->reservation = virtmemAddReservation((void *)marker, reservation_bytes);
            if (space->reservation) {
                result = svcMapPhysicalMemory((void *)marker, PS3_GUEST_PAGE_BYTES);
                create_diagnostic(diagnostic, PS3_HORIZON_CREATE_MAP_MARKER, 1, result);
                if (R_SUCCEEDED(result)) {
                    space->base = base;
                    space->stats.ownership_pages = 1;
                    status = PS3_GUEST_MEMORY_OK;
                } else {
                    virtmemRemoveReservation(space->reservation);
                    space->reservation = NULL;
                    status = PS3_GUEST_MEMORY_PLATFORM_FAILURE;
                }
                break;
            }
        } else if (R_FAILED(query_result)) {
            create_diagnostic(diagnostic, PS3_HORIZON_CREATE_QUERY_MEMORY, 1, query_result);
        }
        /* Fixed 8 GiB slots keep each marker outside every other guest window.
         * A 4 GiB stride would put the next marker in the previous guest's end. */
        if (last_base - base < CANDIDATE_STRIDE) break;
        base += CANDIDATE_STRIDE;
    }
    virtmemUnlock();
    if (status != PS3_GUEST_MEMORY_OK) {
        free(space);
        return status;
    }
    *output = space;
    create_diagnostic(diagnostic, PS3_HORIZON_CREATE_COMPLETE, 0, 0);
    return PS3_GUEST_MEMORY_OK;
}

ps3_guest_memory_status ps3_horizon_guest_space_destroy(ps3_horizon_guest_space **owner)
{
    if (!owner || !*owner) return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_horizon_guest_space *space = *owner;
    for (uint64_t page = 0; page < PAGE_COUNT;) {
        if (!is_committed(space, page)) {
            ++page;
            continue;
        }
        const uint64_t run_start = page;
        while (page < PAGE_COUNT && is_committed(space, page)) ++page;
        ps3_guest_memory_status status = decommit_range(space,
            run_start * PS3_GUEST_PAGE_BYTES, (page - run_start) * PS3_GUEST_PAGE_BYTES);
        if (status != PS3_GUEST_MEMORY_OK) return status;
    }
    if (!is_free_range(space->base, PS3_GUEST_ADDRESS_BYTES)) {
        return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    }
    const uintptr_t marker = space->base - PS3_GUEST_PAGE_BYTES;
    /* Creation uses the same lock: a released marker cannot be claimed before
     * this owner finishes checking and removing its virtual reservation. */
    virtmemLock();
    if (space->stats.ownership_pages) {
        Result result = svcUnmapPhysicalMemory((void *)marker, PS3_GUEST_PAGE_BYTES);
        space->stats.last_kernel_result = result;
        if (R_FAILED(result)) {
            virtmemUnlock();
            return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        }
        space->stats.ownership_pages = 0;
    }
    if (!is_free_range(marker, PS3_GUEST_ADDRESS_BYTES + PS3_GUEST_PAGE_BYTES)) {
        virtmemUnlock();
        return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    }
    virtmemRemoveReservation(space->reservation);
    virtmemUnlock();
    free(space);
    *owner = NULL;
    return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory ps3_horizon_guest_space_port(ps3_horizon_guest_space *space)
{
    return (ps3_guest_memory){space, commit_range, decommit_range,
                              read_range, write_range, zero_range};
}

uintptr_t ps3_horizon_guest_space_base(ps3_horizon_guest_space *space)
{
    return space ? space->base : 0;
}

ps3_horizon_guest_stats ps3_horizon_guest_space_stats(ps3_horizon_guest_space *space)
{
    if (!space) return (ps3_horizon_guest_stats){0};
    mutexLock(&space->mutex);
    ps3_horizon_guest_stats result = space->stats;
    mutexUnlock(&space->mutex);
    return result;
}
