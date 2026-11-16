#include "heap_guest_space.h"
#include <stdbool.h>
#include <string.h>

enum {
    BLOCK_BYTES = 65536,
    PAGES_PER_BLOCK = 16,
    BLOCK_COUNT = 65536,
    PAGE_COUNT = 1048576
};
typedef struct heap_block {
    struct heap_block *pending_next;
    uint32_t index;
    uint16_t mask;
    unsigned char data[BLOCK_BYTES];
} heap_block;
struct ps3_heap_guest_space {
    ps3_heap_guest_allocator allocator;
    ps3_instance_guard guard;
    uint64_t max_pages;
    ps3_heap_guest_stats stats;
    heap_block *blocks[BLOCK_COUNT];
};

static bool valid_range(uint32_t address, uint64_t length)
{
    return length <= PS3_GUEST_ADDRESS_BYTES - address;
}
static bool page_range(uint32_t address, uint64_t length)
{
    return length && valid_range(address, length) &&
        address % PS3_GUEST_PAGE_BYTES == 0 && length % PS3_GUEST_PAGE_BYTES == 0;
}
static bool committed(ps3_heap_guest_space *space, uint64_t page)
{
    heap_block *block = space->blocks[page / PAGES_PER_BLOCK];
    return block && (block->mask & (UINT32_C(1) << (page % PAGES_PER_BLOCK)));
}
static bool fully_committed(ps3_heap_guest_space *space, uint32_t address, uint64_t length)
{
    if (!length) return true;
    uint64_t last = ((uint64_t)address + length - 1) / PS3_GUEST_PAGE_BYTES;
    for (uint64_t page = address / PS3_GUEST_PAGE_BYTES; page <= last; ++page)
        if (!committed(space, page)) return false;
    return true;
}
static void free_pending(ps3_heap_guest_space *space, heap_block *pending)
{
    while (pending) {
        heap_block *next = pending->pending_next;
        space->allocator.deallocate(space->allocator.context, pending);
        pending = next;
    }
}
static ps3_guest_memory_status commit(void *context, uint32_t address, uint64_t length)
{
    ps3_heap_guest_space *space = context;
    if (!space || !page_range(address, length)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    space->guard.lock(space->guard.context);
    uint64_t first_page = address / PS3_GUEST_PAGE_BYTES;
    uint64_t end_page = ((uint64_t)address + length) / PS3_GUEST_PAGE_BYTES;
    uint64_t new_pages = 0;
    for (uint64_t page = first_page; page < end_page; ++page)
        if (!committed(space, page)) ++new_pages;
    if (new_pages > space->max_pages - space->stats.committed_pages) {
        space->guard.unlock(space->guard.context);
        return PS3_GUEST_MEMORY_NO_MEMORY;
    }
    heap_block *pending = NULL;
    uint64_t first_block = first_page / PAGES_PER_BLOCK;
    uint64_t last_block = (end_page - 1) / PAGES_PER_BLOCK;
    for (uint64_t index = first_block; index <= last_block; ++index) {
        if (space->blocks[index]) continue;
        heap_block *block = space->allocator.allocate(space->allocator.context, sizeof(*block));
        if (!block) {
            free_pending(space, pending);
            space->guard.unlock(space->guard.context);
            return PS3_GUEST_MEMORY_NO_MEMORY;
        }
        block->mask = 0;
        block->index = (uint32_t)index;
        block->pending_next = pending;
        pending = block;
    }
    /* No fallible operation follows publication of the prepared blocks. */
    while (pending) {
        heap_block *next = pending->pending_next;
        space->blocks[pending->index] = pending;
        pending->pending_next = NULL;
        ++space->stats.backing_blocks;
        space->stats.backing_bytes += sizeof(*pending);
        pending = next;
    }
    for (uint64_t page = first_page; page < end_page; ++page) {
        heap_block *block = space->blocks[page / PAGES_PER_BLOCK];
        uint16_t bit = UINT32_C(1) << (page % PAGES_PER_BLOCK);
        if (!(block->mask & bit)) {
            memset(block->data + (page % PAGES_PER_BLOCK) * PS3_GUEST_PAGE_BYTES,
                   0, PS3_GUEST_PAGE_BYTES);
            block->mask |= bit;
        }
    }
    space->stats.committed_pages += new_pages;
    space->guard.unlock(space->guard.context);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status decommit(void *context, uint32_t address, uint64_t length)
{
    ps3_heap_guest_space *space = context;
    if (!space || !page_range(address, length)) return PS3_GUEST_MEMORY_INVALID_RANGE;
    space->guard.lock(space->guard.context);
    uint64_t end_page = ((uint64_t)address + length) / PS3_GUEST_PAGE_BYTES;
    for (uint64_t page = address / PS3_GUEST_PAGE_BYTES; page < end_page; ++page) {
        heap_block *block = space->blocks[page / PAGES_PER_BLOCK];
        uint16_t bit = UINT32_C(1) << (page % PAGES_PER_BLOCK);
        if (!block || !(block->mask & bit)) continue;
        block->mask &= (uint16_t)~bit;
        --space->stats.committed_pages;
        if (!block->mask) {
            space->blocks[page / PAGES_PER_BLOCK] = NULL;
            --space->stats.backing_blocks;
            space->stats.backing_bytes -= sizeof(*block);
            space->allocator.deallocate(space->allocator.context, block);
        }
    }
    space->guard.unlock(space->guard.context);
    return PS3_GUEST_MEMORY_OK;
}
typedef enum transfer_kind { READ_BYTES, WRITE_BYTES, ZERO_BYTES } transfer_kind;
static bool overlaps_blocks(ps3_heap_guest_space *space, uint32_t address,
                            size_t length, const void *buffer)
{
    uintptr_t host = (uintptr_t)buffer;
    uint64_t first = address / BLOCK_BYTES;
    uint64_t last = ((uint64_t)address + length - 1) / BLOCK_BYTES;
    for (uint64_t index = first; index <= last; ++index) {
        uintptr_t block = (uintptr_t)space->blocks[index]->data;
        if (host < block + BLOCK_BYTES && block < host + length) return true;
    }
    return false;
}
static void copy_chunks(ps3_heap_guest_space *space, uint32_t address,
                        unsigned char *buffer, uint64_t length, transfer_kind operation)
{
    uint64_t cursor = address;
    while (length) {
        size_t offset = cursor % BLOCK_BYTES;
        size_t count = BLOCK_BYTES - offset;
        if (count > length) count = (size_t)length;
        unsigned char *guest = space->blocks[cursor / BLOCK_BYTES]->data + offset;
        if (operation == READ_BYTES) memcpy(buffer, guest, count);
        else if (operation == WRITE_BYTES) memcpy(guest, buffer, count);
        else memset(guest, 0, count);
        if (operation != ZERO_BYTES) buffer += count;
        cursor += count;
        length -= count;
    }
}
static ps3_guest_memory_status transfer(void *context, uint32_t address, void *buffer,
                                       uint64_t length, transfer_kind operation)
{
    ps3_heap_guest_space *space = context;
    if (!space || !valid_range(address, length) || length > SIZE_MAX ||
        (operation != ZERO_BYTES && length &&
         (!buffer || length > UINTPTR_MAX - (uintptr_t)buffer)))
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    space->guard.lock(space->guard.context);
    if (!fully_committed(space, address, length)) {
        space->guard.unlock(space->guard.context);
        return PS3_GUEST_MEMORY_UNMAPPED;
    }
    unsigned char *scratch = NULL;
    if (length && operation != ZERO_BYTES && overlaps_blocks(space, address, (size_t)length, buffer)) {
        scratch = space->allocator.allocate(space->allocator.context, (size_t)length);
        if (!scratch) {
            space->guard.unlock(space->guard.context);
            return PS3_GUEST_MEMORY_NO_MEMORY;
        }
        if (operation == WRITE_BYTES) memcpy(scratch, buffer, (size_t)length);
    }
    copy_chunks(space, address, scratch ? scratch : buffer, length, operation);
    if (scratch) {
        if (operation == READ_BYTES) memmove(buffer, scratch, (size_t)length);
        space->allocator.deallocate(space->allocator.context, scratch);
    }
    space->guard.unlock(space->guard.context);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status read_bytes(void *s, uint32_t a, void *b, size_t n)
{
    return transfer(s, a, b, n, READ_BYTES);
}
static ps3_guest_memory_status write_bytes(void *s, uint32_t a, const void *b, size_t n)
{
    return transfer(s, a, (void *)b, n, WRITE_BYTES);
}
static ps3_guest_memory_status zero_bytes(void *s, uint32_t a, uint64_t n)
{
    return transfer(s, a, NULL, n, ZERO_BYTES);
}
ps3_guest_memory_status ps3_heap_guest_space_create(ps3_heap_guest_space **output,
    ps3_heap_guest_allocator allocator, ps3_instance_guard guard, uint64_t max_pages)
{
    if (!output || *output || !allocator.allocate || !allocator.deallocate ||
        !guard.lock || !guard.unlock || !max_pages || max_pages > PAGE_COUNT)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_heap_guest_space *space = allocator.allocate(allocator.context, sizeof(*space));
    if (!space) return PS3_GUEST_MEMORY_NO_MEMORY;
    memset(space, 0, sizeof(*space));
    space->allocator = allocator;
    space->guard = guard;
    space->max_pages = max_pages;
    space->stats.metadata_bytes = sizeof(*space);
    *output = space;
    return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory ps3_heap_guest_space_port(ps3_heap_guest_space *space)
{
    return (ps3_guest_memory){space, commit, decommit, read_bytes, write_bytes, zero_bytes};
}
ps3_heap_guest_stats ps3_heap_guest_space_stats(ps3_heap_guest_space *space)
{
    if (!space) return (ps3_heap_guest_stats){0};
    space->guard.lock(space->guard.context);
    ps3_heap_guest_stats result = space->stats;
    space->guard.unlock(space->guard.context);
    return result;
}
ps3_guest_memory_status ps3_heap_guest_space_destroy(ps3_heap_guest_space **owner)
{
    if (!owner || !*owner) return PS3_GUEST_MEMORY_INVALID_RANGE;
    ps3_heap_guest_space *space = *owner;
    for (size_t index = 0; index < BLOCK_COUNT; ++index)
        if (space->blocks[index]) space->allocator.deallocate(space->allocator.context, space->blocks[index]);
    ps3_heap_guest_allocator allocator = space->allocator;
    allocator.deallocate(allocator.context, space);
    *owner = NULL;
    return PS3_GUEST_MEMORY_OK;
}
