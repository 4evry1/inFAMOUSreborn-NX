#include "guest_space_flat_horizon.h"
#include "committed_range.h"
#include <switch.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Decision 136 (2026-11-16, emulator testing): -DPS3_FLAT_SLAB_BYTES=<power of two, 2 MiB..64 MiB> changes the slab size. The console
 * keeps 2 MiB. Eglantine refuses the 82nd svcMapProcessCodeMemory (0xd401 after 81 slabs, ~162 MB); bigger slabs need fewer mappings. */
#ifndef PS3_FLAT_SLAB_BYTES
#define PS3_FLAT_SLAB_BYTES 0x200000
#endif
enum {
    SLAB_BYTES = PS3_FLAT_SLAB_BYTES,
    SLAB_PAGES = SLAB_BYTES / 4096,
    SLAB_COUNT = 0x100000000ull / PS3_FLAT_SLAB_BYTES,   /* 4 GiB / slab */
    PAGE_COUNT = 1048576,
    BITMAP_BYTES = PAGE_COUNT / 8
};
typedef struct flat_slab {
    void *backing;               /* heap block; inaccessible while it is a code alias */
    void *alias;                 /* the code alias, RW */
    uint32_t committed_pages;
} flat_slab;
struct ps3_flat_guest_space {
    uintptr_t base;
    VirtmemReservation *reservation;
    Handle process;
    Mutex mutex;
    uint64_t max_pages;
    ps3_flat_guest_stats stats;
    flat_slab slabs[SLAB_COUNT];
    unsigned char committed[BITMAP_BYTES];
};
typedef enum transfer_operation { READ_GUEST, WRITE_GUEST, ZERO_GUEST } transfer_operation;

uintptr_t g_ps3_flat_guest_base = 0;
/* Diagnostic: the step the backend is in (0 idle; 1..5 the slab steps; 6 releasing a slab; 7 zeroing fresh pages;
 * 8 a transfer) and the raw word of its lock, for the composition's heartbeat. */
volatile uint32_t g_ps3_flat_guest_stage = 0;
const volatile uint32_t *g_ps3_flat_guest_lock_word = NULL;
ps3_flat_guest_space *g_ps3_flat_guest_space = NULL;

/* ---- a real handle of this process -------------------------------------------------------------------------- */
/* nx-hbloader passes one. A loader that does not (an emulator loading the NRO directly) leaves only the pseudo
 * handle, which svcMapProcessMemory refuses; then the handle is copied to ourselves through a local IPC session,
 * the way nx-hbloader obtains its own. */
typedef struct handle_request { Handle server, received; } handle_request;
static void receive_process_handle(void *argument)
{
    handle_request *request = argument;
    void *tls = armGetTls();
    s32 index = 0;
    hipcMakeRequest(tls, (HipcMetadata){0});
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &request->server, 1, INVALID_HANDLE, 5000000000ULL))) {
        const HipcParsedRequest message = hipcParseRequest(tls);
        if (message.meta.num_copy_handles == 1) request->received = message.data.copy_handles[0];
    }
    svcCloseHandle(request->server);   /* also wakes the synchronous sender */
}
static Handle own_process_handle(void)
{
    handle_request request = {INVALID_HANDLE, INVALID_HANDLE};
    Handle client = INVALID_HANDLE;
    Thread receiver;
    const Handle supplied = envGetOwnProcessHandle();
    if (supplied != INVALID_HANDLE) return supplied;
    if (R_FAILED(svcCreateSession(&request.server, &client, 0, 0))) return INVALID_HANDLE;
    memset(&receiver, 0, sizeof receiver);
    if (R_FAILED(threadCreate(&receiver, receive_process_handle, &request, NULL, 0x4000, 0x20, 0))) {
        svcCloseHandle(request.server); svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    if (R_FAILED(threadStart(&receiver))) {
        threadClose(&receiver); svcCloseHandle(request.server); svcCloseHandle(client);
        return INVALID_HANDLE;
    }
    {
        HipcMetadata metadata = {0};
        metadata.num_copy_handles = 1;
        hipcMakeRequest(armGetTls(), metadata).copy_handles[0] = CUR_PROCESS_HANDLE;
    }
    svcSendSyncRequest(client);
    svcCloseHandle(client);
    threadWaitForExit(&receiver);
    threadClose(&receiver);
    return request.received;
}

/* ---- slabs --------------------------------------------------------------------------------------------------- */
static bool slab_acquire(ps3_flat_guest_space *space, uint32_t index)
{
    flat_slab *slab = &space->slabs[index];
    void *backing, *alias;
    Result result;
    uint32_t step = 1;
    g_ps3_flat_guest_stage = 1;
    backing = memalign(SLAB_BYTES, SLAB_BYTES);
    if (!backing) { result = 0; goto failed; }
    memset(backing, 0, SLAB_BYTES);
    step = 2; g_ps3_flat_guest_stage = 2;
    virtmemLock();
    alias = virtmemFindCodeMemory(SLAB_BYTES, SLAB_BYTES);
    result = 0;
    if (alias) { step = 3; g_ps3_flat_guest_stage = 3; result = svcMapProcessCodeMemory(space->process, (u64)(uintptr_t)alias, (u64)(uintptr_t)backing, SLAB_BYTES); }
    virtmemUnlock();
    if (!alias || R_FAILED(result)) { free(backing); goto failed; }
    step = 4; g_ps3_flat_guest_stage = 4;
    result = svcSetProcessMemoryPermission(space->process, (u64)(uintptr_t)alias, SLAB_BYTES, Perm_Rw);
    if (R_SUCCEEDED(result)) {
        step = 5; g_ps3_flat_guest_stage = 5;
        result = svcMapProcessMemory((void *)(space->base + (uintptr_t)index * SLAB_BYTES), space->process,
                                     (u64)(uintptr_t)alias, SLAB_BYTES);
    }
    if (R_FAILED(result)) {
        svcUnmapProcessCodeMemory(space->process, (u64)(uintptr_t)alias, (u64)(uintptr_t)backing, SLAB_BYTES);
        free(backing);
        goto failed;
    }
    g_ps3_flat_guest_stage = 0;
    slab->backing = backing;
    slab->alias = alias;
    slab->committed_pages = 0;
    ++space->stats.slabs;
    ++space->stats.slab_acquires;
    return true;
failed:
    space->stats.last_kernel_result = result;
    space->stats.last_slab_step = step;
    ++space->stats.slab_failures;
    return false;
}
static void slab_release(ps3_flat_guest_space *space, uint32_t index)
{
    flat_slab *slab = &space->slabs[index];
    if (!slab->backing) return;
    g_ps3_flat_guest_stage = 6;
    svcUnmapProcessMemory((void *)(space->base + (uintptr_t)index * SLAB_BYTES), space->process,
                          (u64)(uintptr_t)slab->alias, SLAB_BYTES);
    svcUnmapProcessCodeMemory(space->process, (u64)(uintptr_t)slab->alias, (u64)(uintptr_t)slab->backing, SLAB_BYTES);
    free(slab->backing);
    memset(slab, 0, sizeof *slab);
    --space->stats.slabs;
    ++space->stats.slab_releases;
    g_ps3_flat_guest_stage = 0;
}

/* ---- the port ------------------------------------------------------------------------------------------------ */
static bool valid_range(uint32_t address, uint64_t length)
{
    return length <= PS3_GUEST_ADDRESS_BYTES - address;
}
static bool page_range(uint32_t address, uint64_t length)
{
    return length && valid_range(address, length) && address % PS3_GUEST_PAGE_BYTES == 0 &&
           length % PS3_GUEST_PAGE_BYTES == 0;
}
static bool is_committed(const ps3_flat_guest_space *space, uint64_t page)
{
    return (space->committed[page / 8] >> (page % 8)) & 1;
}
static bool fully_committed(const ps3_flat_guest_space *space, uint32_t address, uint64_t length)
{
    uint64_t page, last;
    if (!length) return true;
    last = ((uint64_t)address + length - 1) / PS3_GUEST_PAGE_BYTES;
    for (page = address / PS3_GUEST_PAGE_BYTES; page <= last; ++page)
        if (!is_committed(space, page)) return false;
    return true;
}
static ps3_guest_memory_status commit_range(void *context, uint32_t address, uint64_t length)
{
    ps3_flat_guest_space *space = context;
    uint64_t first_page, end_page, page, new_pages = 0;
    uint32_t first_slab, last_slab, slab;
    if (!space || !page_range(address, length)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    first_page = address / PS3_GUEST_PAGE_BYTES;
    end_page = ((uint64_t)address + length) / PS3_GUEST_PAGE_BYTES;
    first_slab = (uint32_t)(first_page / SLAB_PAGES);
    last_slab = (uint32_t)((end_page - 1) / SLAB_PAGES);
    mutexLock(&space->mutex);
    for (page = first_page; page < end_page; ++page)
        if (!is_committed(space, page)) ++new_pages;
    if (new_pages > space->max_pages - space->stats.committed_pages) {
        mutexUnlock(&space->mutex);
        return PS3_GUEST_MEMORY_NO_MEMORY;
    }
    for (slab = first_slab; slab <= last_slab; ++slab) {
        if (space->slabs[slab].backing) continue;
        if (!slab_acquire(space, slab)) {
            /* undo the slabs this call brought in: they hold no committed page yet */
            const bool no_memory = space->stats.last_slab_step == 1;
            uint32_t undo;
            for (undo = first_slab; undo < slab; ++undo)
                if (space->slabs[undo].backing && !space->slabs[undo].committed_pages) slab_release(space, undo);
            mutexUnlock(&space->mutex);
            return no_memory ? PS3_GUEST_MEMORY_NO_MEMORY : PS3_GUEST_MEMORY_PLATFORM_FAILURE;
        }
    }
    for (page = first_page; page < end_page; ++page) {
        if (is_committed(space, page)) continue;
        /* a page that was committed and decommitted before may hold old bytes: fresh pages are zero */
        memset((void *)(space->base + page * PS3_GUEST_PAGE_BYTES), 0, PS3_GUEST_PAGE_BYTES);
        space->committed[page / 8] |= (unsigned char)(1u << (page % 8));
        ++space->slabs[page / SLAB_PAGES].committed_pages;
    }
    space->stats.committed_pages += new_pages;
    mutexUnlock(&space->mutex);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status decommit_range(void *context, uint32_t address, uint64_t length)
{
    ps3_flat_guest_space *space = context;
    uint64_t page, end_page;
    if (!space || !page_range(address, length)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    end_page = ((uint64_t)address + length) / PS3_GUEST_PAGE_BYTES;
    mutexLock(&space->mutex);
    for (page = address / PS3_GUEST_PAGE_BYTES; page < end_page; ++page) {
        const uint32_t slab = (uint32_t)(page / SLAB_PAGES);
        if (!is_committed(space, page)) continue;
        space->committed[page / 8] &= (unsigned char)~(1u << (page % 8));
        --space->stats.committed_pages;
        if (--space->slabs[slab].committed_pages == 0) slab_release(space, slab);
    }
    mutexUnlock(&space->mutex);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status transfer(void *context, uint32_t address, void *buffer, uint64_t length,
                                        transfer_operation operation)
{
    ps3_flat_guest_space *space = context;
    if (!space || !valid_range(address, length) || (operation != ZERO_GUEST && length && !buffer))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
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
static ps3_guest_memory_status read_range(void *space, uint32_t address, void *buffer, size_t length)
{
    return transfer(space, address, buffer, length, READ_GUEST);
}
static ps3_guest_memory_status write_range(void *space, uint32_t address, const void *buffer, size_t length)
{
    return transfer(space, address, (void *)buffer, length, WRITE_GUEST);
}
static ps3_guest_memory_status zero_range(void *space, uint32_t address, uint64_t length)
{
    return transfer(space, address, NULL, length, ZERO_GUEST);
}

/* ---- lifetime ------------------------------------------------------------------------------------------------ */
static void set_diagnostic(ps3_flat_guest_diagnostic *diagnostic, ps3_flat_guest_stage stage, uint32_t result,
                           uint32_t step)
{
    if (diagnostic) { diagnostic->stage = stage; diagnostic->kernel_result = result; diagnostic->slab_step = step; }
}
const char *ps3_flat_guest_stage_name(ps3_flat_guest_stage stage)
{
    switch (stage) {
    case PS3_FLAT_GUEST_COMPLETE: return "complete";
    case PS3_FLAT_GUEST_ARGUMENT: return "validate-arguments";
    case PS3_FLAT_GUEST_ALLOCATE: return "calloc-context";
    case PS3_FLAT_GUEST_PROCESS_HANDLE: return "own-process-handle";
    case PS3_FLAT_GUEST_RESERVE_WINDOW: return "reserve-4GiB-window";
    case PS3_FLAT_GUEST_PROBE_SLAB: return "probe-slab";
    default: return "unknown";
    }
}
ps3_guest_memory_status ps3_flat_guest_space_create(ps3_flat_guest_space **out, uint64_t max_pages,
                                                    ps3_flat_guest_diagnostic *diagnostic)
{
    ps3_flat_guest_space *space;
    set_diagnostic(diagnostic, PS3_FLAT_GUEST_ARGUMENT, 0, 0);
    if (!out || *out || !max_pages || max_pages > PAGE_COUNT) return PS3_GUEST_MEMORY_INVALID_RANGE;
    set_diagnostic(diagnostic, PS3_FLAT_GUEST_ALLOCATE, 0, 0);
    space = calloc(1, sizeof *space);
    if (!space) return PS3_GUEST_MEMORY_NO_MEMORY;
    mutexInit(&space->mutex);
    g_ps3_flat_guest_lock_word = (const volatile uint32_t *)&space->mutex;
    space->max_pages = max_pages;
    /* Before anything can hold libnx's virtmem lock: the fallback starts a thread, which takes that lock. */
    set_diagnostic(diagnostic, PS3_FLAT_GUEST_PROCESS_HANDLE, 0, 0);
    space->process = own_process_handle();
    if (space->process == INVALID_HANDLE) { free(space); return PS3_GUEST_MEMORY_PLATFORM_FAILURE; }
    set_diagnostic(diagnostic, PS3_FLAT_GUEST_RESERVE_WINDOW, 0, 0);
    virtmemLock();
    space->base = (uintptr_t)virtmemFindAslr(PS3_GUEST_ADDRESS_BYTES, SLAB_BYTES);
    if (space->base) space->reservation = virtmemAddReservation((void *)space->base, PS3_GUEST_ADDRESS_BYTES);
    virtmemUnlock();
    if (!space->base || !space->reservation) { free(space); return PS3_GUEST_MEMORY_PLATFORM_FAILURE; }
    /* Prove the slab sequence once, on a slab that then goes away again. */
    if (!slab_acquire(space, 0)) {
        set_diagnostic(diagnostic, PS3_FLAT_GUEST_PROBE_SLAB, space->stats.last_kernel_result, space->stats.last_slab_step);
        virtmemLock(); virtmemRemoveReservation(space->reservation); virtmemUnlock();
        free(space);
        return PS3_GUEST_MEMORY_PLATFORM_FAILURE;
    }
    ((volatile unsigned char *)space->base)[0] = 0x5A;
    ((volatile unsigned char *)space->base)[SLAB_BYTES - 1] = 0xA5;
    slab_release(space, 0);
    set_diagnostic(diagnostic, PS3_FLAT_GUEST_COMPLETE, 0, 0);
    *out = space;
    return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory ps3_flat_guest_space_port(ps3_flat_guest_space *space)
{
    return (ps3_guest_memory){space, commit_range, decommit_range, read_range, write_range, zero_range};
}
uintptr_t ps3_flat_guest_space_base(ps3_flat_guest_space *space)
{
    return space ? space->base : 0;
}
const void *ps3_flat_guest_space_pointer(ps3_flat_guest_space *space, uint32_t address, uint64_t length)
{
    const void *pointer = NULL;
    if (!space || !length || !valid_range(address, length)) return NULL;
    mutexLock(&space->mutex);
    if (fully_committed(space, address, length)) pointer = (const void *)(space->base + address);
    mutexUnlock(&space->mutex);
    return pointer;
}
/* Root (2026-11-15, renderer CPU work): ps3_flat_guest_space_pointer without the mutex, for any length. The renderer's guest reader and
 * the RSX consumer ask for a pointer thousands of times a frame; each ask took the space's mutex (the one every guarded read, write and
 * commit of the whole guest also takes, and a libnx Mutex hands a contended lock over to its first waiter, so a busy moment costs a
 * time slice). This reads the commit bitmap with relaxed byte loads, like ps3_flat_guest_fast_pointer: same reliance, that the guest
 * does not unmap memory it has handed to the RSX while it is in use. NULL for length 0, past the 4 GiB space, or any uncommitted page. */
const void *ps3_flat_guest_fast_range(uint32_t address, uint64_t length)
{
    ps3_flat_guest_space *space = g_ps3_flat_guest_space;
    if (!space || !ps3_committed_range_ok(space->committed, address, length)) return NULL;
    return (const void *)(space->base + address);
}
/* Root (2026-11-13, Decision 110): the commit bitmap of the active space (one bit per PS3_GUEST_PAGE_BYTES page),
 * or NULL. For the generated-code memory path, which tests the bit inline instead of calling in here per access.
 * The bitmap lives as long as the space; same reliance as ps3_flat_guest_fast_pointer below. */
const unsigned char *ps3_flat_guest_committed_bitmap(void)
{
    ps3_flat_guest_space *space = g_ps3_flat_guest_space;
    return space ? space->committed : NULL;
}
void *ps3_flat_guest_fast_pointer(uint32_t address, uint32_t length)
{
    ps3_flat_guest_space *space = g_ps3_flat_guest_space;
    uint64_t first, last;
    if (!space || !length || (uint64_t)address + length > PS3_GUEST_ADDRESS_BYTES) return NULL;
    first = address / PS3_GUEST_PAGE_BYTES;
    last = ((uint64_t)address + length - 1) / PS3_GUEST_PAGE_BYTES;
    if (!((space->committed[first / 8] >> (first % 8)) & 1)) return NULL;
    if (last != first && !((space->committed[last / 8] >> (last % 8)) & 1)) return NULL;
    return (void *)(space->base + address);
}
/* Root (2026-11-12): the numbers of the active space for the final log, without the caller needing this header. */
void ps3_flat_guest_debug(uint64_t out[7])
{
    ps3_flat_guest_space *space = g_ps3_flat_guest_space;
    memset(out, 0, 7 * sizeof out[0]);
    if (!space) return;
    out[0] = space->stats.committed_pages; out[1] = space->stats.slabs; out[2] = space->stats.slab_acquires;
    out[3] = space->stats.slab_releases; out[4] = space->stats.slab_failures; out[5] = space->stats.last_kernel_result;
    out[6] = space->stats.last_slab_step;
}
ps3_flat_guest_stats ps3_flat_guest_space_stats(ps3_flat_guest_space *space)
{
    ps3_flat_guest_stats stats;
    memset(&stats, 0, sizeof stats);
    if (!space) return stats;
    mutexLock(&space->mutex);
    stats = space->stats;
    mutexUnlock(&space->mutex);
    return stats;
}
ps3_guest_memory_status ps3_flat_guest_space_destroy(ps3_flat_guest_space **output)
{
    ps3_flat_guest_space *space;
    uint32_t slab;
    if (!output || !*output) return PS3_GUEST_MEMORY_INVALID_RANGE;
    space = *output;
    mutexLock(&space->mutex);
    for (slab = 0; slab < SLAB_COUNT; ++slab) slab_release(space, slab);
    mutexUnlock(&space->mutex);
    virtmemLock(); virtmemRemoveReservation(space->reservation); virtmemUnlock();
    free(space);
    *output = NULL;
    return PS3_GUEST_MEMORY_OK;
}
