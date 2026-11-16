#ifndef PS3_GUEST_SPACE_H
#define PS3_GUEST_SPACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_GUEST_ADDRESS_BYTES UINT64_C(0x100000000)
#define PS3_GUEST_PAGE_BYTES UINT64_C(4096)

typedef enum ps3_guest_memory_status {
    PS3_GUEST_MEMORY_OK = 0,
    PS3_GUEST_MEMORY_INVALID_RANGE,
    PS3_GUEST_MEMORY_UNMAPPED,
    PS3_GUEST_MEMORY_NO_MEMORY,
    PS3_GUEST_MEMORY_PLATFORM_FAILURE,
    PS3_GUEST_MEMORY_BUSY
} ps3_guest_memory_status;

/* The caller owns context and its lifetime. Commit/decommit require page-aligned
 * nonempty ranges. Commit is idempotent and preserves previously committed data;
 * newly committed pages are zero-filled. Failed operations leave the prior state
 * intact. Read/write/zero reject ranges containing any uncommitted page before
 * touching memory. All callbacks serialize against decommit for their duration.
 * A mapping-pinning decorator returns BUSY before a decommit that overlaps a
 * live pin; this failure also leaves all prior state intact.
 * No pointer returned by another API inherits that synchronization guarantee. */
typedef struct ps3_guest_memory {
    void *context;
    ps3_guest_memory_status (*commit)(void *, uint32_t, uint64_t);
    ps3_guest_memory_status (*decommit)(void *, uint32_t, uint64_t);
    ps3_guest_memory_status (*read)(void *, uint32_t, void *, size_t);
    ps3_guest_memory_status (*write)(void *, uint32_t, const void *, size_t);
    ps3_guest_memory_status (*zero)(void *, uint32_t, uint64_t);
} ps3_guest_memory;

#ifdef __cplusplus
}
#endif
#endif
