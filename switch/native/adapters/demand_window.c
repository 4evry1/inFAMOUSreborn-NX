#include "demand_window.h"
#include <string.h>

static int intersect(const ps3_demand_window *window, uint32_t address, uint64_t length)
{
    return length && (uint64_t)address < (uint64_t)window->base + window->size &&
        (uint64_t)window->base < (uint64_t)address + length;
}

static int contained(const ps3_demand_window *window, uint32_t address, uint64_t length)
{
    return length && (uint64_t)address >= window->base &&
        (uint64_t)address + length <= (uint64_t)window->base + window->size;
}

static int bit_set(const ps3_demand_window *window, uint32_t granule_index)
{
    return window->bitmap[granule_index / 8] & (1u << (granule_index % 8));
}

static void set_bit(ps3_demand_window *window, uint32_t granule_index)
{
    window->bitmap[granule_index / 8] |= (uint8_t)(1u << (granule_index % 8));
}

static uint32_t granule_offset(const ps3_demand_window *window, uint32_t granule_index)
{
    return granule_index * window->granule;
}

static void record_exhaustion(ps3_demand_window *window, uint32_t kind,
                              uint32_t offset, uint32_t status)
{
    if (window->stats.exhaustion != PS3_DEMAND_WINDOW_NOT_EXHAUSTED) return;
    window->stats.exhaustion = kind;
    window->stats.exhaustion_offset = offset;
    window->stats.exhaustion_status = status;
}

static void record_commit(ps3_demand_window *window, uint32_t offset)
{
    if (window->stats.granule_commits == 1 || offset < window->stats.lowest_offset)
        window->stats.lowest_offset = offset;
    if (offset + window->granule > window->stats.highest_end_offset)
        window->stats.highest_end_offset = offset + window->granule;
    if (window->stats.journal_count < PS3_DEMAND_WINDOW_JOURNAL)
        window->stats.journal[window->stats.journal_count++] = offset;
}

typedef enum transfer_kind { TRANSFER_READ, TRANSFER_WRITE, TRANSFER_ZERO } transfer_kind;

/* Commits every clear granule covering the contained range. Returns OK,
 * NO_MEMORY (cap) or the backing's status; on a backing failure the granules
 * this call committed are undone, so only the exhaustion record changes. */
static ps3_guest_memory_status commit_range(ps3_demand_window *window,
    uint32_t address, uint64_t length, transfer_kind kind)
{
    uint32_t first = (address - window->base) / window->granule;
    uint32_t last = (uint32_t)(((uint64_t)address + length - 1 - window->base) / window->granule);
    uint32_t new_count = 0;
    for (uint32_t index = first; index <= last; ++index)
        if (!bit_set(window, index)) ++new_count;
    if (window->stats.committed_bytes + (uint64_t)new_count * window->granule > window->cap_bytes) {
        uint32_t offender = first;
        while (offender <= last && bit_set(window, offender)) ++offender;
        record_exhaustion(window, PS3_DEMAND_WINDOW_CAP, granule_offset(window, offender),
            PS3_GUEST_MEMORY_NO_MEMORY);
        return PS3_GUEST_MEMORY_NO_MEMORY;
    }
    for (uint32_t index = first; index <= last; ++index) {
        if (bit_set(window, index)) continue;
        const uint32_t offset = granule_offset(window, index);
        const ps3_guest_memory_status status =
            window->backing.commit(window->backing.context, window->base + offset, window->granule);
        if (status != PS3_GUEST_MEMORY_OK) {
            for (uint32_t undo = index; undo > first;) {
                --undo;
                /* Bits for this call's earlier granules are still clear; only
                 * clear granules below the failing one were committed here. */
                if (bit_set(window, undo)) continue;
                window->backing.decommit(window->backing.context,
                    window->base + granule_offset(window, undo), window->granule);
            }
            record_exhaustion(window, PS3_DEMAND_WINDOW_BACKING, offset, (uint32_t)status);
            return status;
        }
    }
    for (uint32_t index = first; index <= last; ++index) {
        if (bit_set(window, index)) continue;
        set_bit(window, index);
        ++window->stats.granule_commits;
        record_commit(window, granule_offset(window, index));
    }
    window->stats.committed_bytes += (uint64_t)new_count * window->granule;
    if (new_count) {
        if (kind == TRANSFER_READ) ++window->stats.triggering_reads;
        else if (kind == TRANSFER_WRITE) ++window->stats.triggering_writes;
        else ++window->stats.triggering_zeros;
    }
    return PS3_GUEST_MEMORY_OK;
}

static ps3_guest_memory_status transfer(void *context, uint32_t address, void *buffer,
    uint64_t length, transfer_kind kind)
{
    ps3_demand_window *window = context;
    /* A length past the end of the guest address space cannot be classified by
     * the window; forward it unchanged and let the backing reject it. */
    if (!length || length > PS3_GUEST_ADDRESS_BYTES - address) {
        if (kind == TRANSFER_READ) return window->backing.read(window->backing.context, address, buffer, (size_t)length);
        if (kind == TRANSFER_WRITE) return window->backing.write(window->backing.context, address, buffer, (size_t)length);
        return window->backing.zero(window->backing.context, address, length);
    }
    if (!intersect(window, address, length)) {
        if (kind == TRANSFER_READ) return window->backing.read(window->backing.context, address, buffer, (size_t)length);
        if (kind == TRANSFER_WRITE) return window->backing.write(window->backing.context, address, buffer, (size_t)length);
        return window->backing.zero(window->backing.context, address, length);
    }
    if (!contained(window, address, length)) {
        window->guard.lock(window->guard.context);
        ++window->stats.refused_partial;
        window->guard.unlock(window->guard.context);
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    window->guard.lock(window->guard.context);
    const ps3_guest_memory_status status = commit_range(window, address, length, kind);
    window->guard.unlock(window->guard.context);
    if (status != PS3_GUEST_MEMORY_OK) return status;
    if (kind == TRANSFER_READ) return window->backing.read(window->backing.context, address, buffer, (size_t)length);
    if (kind == TRANSFER_WRITE) return window->backing.write(window->backing.context, address, buffer, (size_t)length);
    return window->backing.zero(window->backing.context, address, length);
}

static ps3_guest_memory_status commit(void *context, uint32_t address, uint64_t length)
{
    ps3_demand_window *window = context;
    if (intersect(window, address, length)) {
        window->guard.lock(window->guard.context);
        ++window->stats.refused_window_commits;
        window->guard.unlock(window->guard.context);
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    return window->backing.commit(window->backing.context, address, length);
}

static ps3_guest_memory_status decommit(void *context, uint32_t address, uint64_t length)
{
    ps3_demand_window *window = context;
    if (intersect(window, address, length)) {
        window->guard.lock(window->guard.context);
        ++window->stats.refused_window_commits;
        window->guard.unlock(window->guard.context);
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    }
    return window->backing.decommit(window->backing.context, address, length);
}

static ps3_guest_memory_status read_bytes(void *context, uint32_t address, void *buffer, size_t length)
{
    return transfer(context, address, buffer, length, TRANSFER_READ);
}

static ps3_guest_memory_status write_bytes(void *context, uint32_t address, const void *buffer, size_t length)
{
    return transfer(context, address, (void *)buffer, length, TRANSFER_WRITE);
}

static ps3_guest_memory_status zero_bytes(void *context, uint32_t address, uint64_t length)
{
    return transfer(context, address, NULL, length, TRANSFER_ZERO);
}

static int power_of_two(uint32_t value)
{
    return value && !(value & (value - 1));
}

ps3_guest_memory_status ps3_demand_window_init(ps3_demand_window *window,
    ps3_guest_memory backing, ps3_instance_guard guard, uint32_t base, uint32_t size,
    uint32_t granule, uint64_t cap_bytes)
{
    if (!window || window->initialized) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!backing.context || !backing.commit || !backing.decommit || !backing.read ||
        !backing.write || !backing.zero) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!guard.lock || !guard.unlock) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!power_of_two(granule) || (uint64_t)granule < PS3_GUEST_PAGE_BYTES)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!size || size % granule || base % granule) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if ((uint64_t)base + size > PS3_GUEST_ADDRESS_BYTES) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (size / granule > PS3_DEMAND_WINDOW_MAX_GRANULES) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (!cap_bytes || cap_bytes % granule || cap_bytes > size) return PS3_GUEST_MEMORY_INVALID_RANGE;
    window->backing = backing;
    window->guard = guard;
    window->base = base;
    window->size = size;
    window->granule = granule;
    window->cap_bytes = cap_bytes;
    memset(window->bitmap, 0, sizeof(window->bitmap));
    memset(&window->stats, 0, sizeof(window->stats));
    window->initialized = 1;
    return PS3_GUEST_MEMORY_OK;
}

ps3_guest_memory ps3_demand_window_port(ps3_demand_window *window)
{
    return (ps3_guest_memory){window, commit, decommit, read_bytes, write_bytes, zero_bytes};
}

ps3_demand_window_stats ps3_demand_window_read_stats(ps3_demand_window *window)
{
    if (!window || !window->initialized) return (ps3_demand_window_stats){0};
    window->guard.lock(window->guard.context);
    ps3_demand_window_stats result = window->stats;
    window->guard.unlock(window->guard.context);
    return result;
}
