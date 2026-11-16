#ifndef PS3_HEAP_GUEST_SPACE_H
#define PS3_HEAP_GUEST_SPACE_H
#include "../../../ps3recomp/runtime/memory/guest_space.h"
#include "../../../ps3recomp/runtime/sync/guard.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_heap_guest_space ps3_heap_guest_space;
typedef struct ps3_heap_guest_allocator {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*deallocate)(void *, void *);
} ps3_heap_guest_allocator;
typedef struct ps3_heap_guest_stats {
    uint64_t committed_pages;
    uint64_t backing_blocks;
    uint64_t metadata_bytes;
    uint64_t backing_bytes;
} ps3_heap_guest_stats;
/* Composition owns the allocator/guard and their contexts until destroy.
 * Allocations must be disjoint, suitably aligned for any C object; failure is
 * NULL. Deallocation is infallible. Callbacks are total/nonthrowing, cannot
 * reenter this adapter, and are not cancellation points. The guard must be
 * distinct from a decorating atomic domain's guard (domain -> backing order).
 * No direct guest base/pointer is provided. Transfers retain memmove semantics
 * for overlapping host buffers; overlap scratch failure leaves state intact.
 * max_pages is explicit logical quota in [1, 1048576], not host capacity. */
ps3_guest_memory_status ps3_heap_guest_space_create(ps3_heap_guest_space **,
    ps3_heap_guest_allocator, ps3_instance_guard, uint64_t max_pages);
ps3_guest_memory ps3_heap_guest_space_port(ps3_heap_guest_space *);
ps3_heap_guest_stats ps3_heap_guest_space_stats(ps3_heap_guest_space *);
/* Caller must quiesce/join users and release decorator pins/owners first. */
ps3_guest_memory_status ps3_heap_guest_space_destroy(ps3_heap_guest_space **);
#ifdef __cplusplus
}
#endif
#endif
