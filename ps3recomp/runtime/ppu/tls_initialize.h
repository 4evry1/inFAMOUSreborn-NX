#ifndef PS3RECOMP_TLS_INITIALIZE_H
#define PS3RECOMP_TLS_INITIALIZE_H
#include "../memory/guest_space.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t next, end;
} Ps3TlsArena;
typedef struct {
    uint32_t source;
    uint64_t file_size, memory_size;
} Ps3TlsTemplate;
typedef struct {
    uint32_t block, thread_pointer;
    uint64_t allocation_size;
} Ps3TlsImage;
typedef enum {
    PS3_TLS_OK, PS3_TLS_ARGUMENT, PS3_TLS_INVALID_RANGE, PS3_TLS_OVERLAP,
    PS3_TLS_NO_SPACE, PS3_TLS_MEMORY_FAILURE, PS3_TLS_CLEANUP_FAILURE
} Ps3TlsStatus;
typedef struct {
    Ps3TlsStatus status, cause;
    ps3_guest_memory_status memory_status, cleanup_status;
} Ps3TlsReport;

/* Caller owns all state and exclusively grants the page-aligned, uncommitted
 * arena [next,end) for this call. Source bytes must remain stable throughout.
 * The arena and memory context must not be concurrently accessed. Scratch is
 * nonempty writable host memory, disjoint from other parameters, contexts, and
 * the backing storage of the guest source and destination ranges.
 * Initialized bytes cannot overlap the newly allocated range. Nonempty input
 * at source zero is invalid rather than silently omitting the template copy.
 * On failure arena and output remain unchanged. A cleanup failure leaves guest
 * ownership unresolved: retain/destroy the arena before any attempted reuse.
 * On success the HLE adapter sets guest r13=thread_pointer and r3=CELL_OK; this
 * operation does not depend on a PPU context or mutate guest registers itself. */
Ps3TlsReport ps3_tls_initialize(const ps3_guest_memory *memory, Ps3TlsArena *arena,
                               const Ps3TlsTemplate *tls_template,
                               void *scratch, size_t scratch_size,
                               Ps3TlsImage *output);

/* Initialize TLS bytes inside already allocated, caller-owned guest storage.
 * Unlike ps3_tls_initialize this performs NO commit, decommit, quota or arena
 * operation and takes no ownership; commit/decommit callbacks may be NULL and
 * are never called. The caller owns the committed, writable range
 * [block,block+capacity): block page-aligned, capacity a whole number of pages
 * (zero is allowed but never holds a TLS image), ending at or below 4 GiB. The
 * range is not otherwise verifiable here, so ordinary storage lifetime rules
 * apply and nothing is retained after the call. Scratch, source stability and
 * single-threaded access rules are those of ps3_tls_initialize; the template
 * source must not overlap the required extent [block,block+allocation_size).
 * Layout, template and bounds are validated in the same order as
 * ps3_tls_initialize before any guest byte is touched; capacity must hold the
 * required extent (NO_SPACE otherwise). Validation failures leave guest memory
 * and output untouched. Only bytes [block,block+memory_size) are written (file
 * bytes copied, remainder zeroed); the rest of the extent is not modified.
 * On success output.allocation_size is the REQUIRED TLS extent, NOT the caller's
 * capacity: the caller must retain the original allocation address and extent
 * for its eventual release. After a read/write/zero failure (MEMORY_FAILURE,
 * cleanup_status OK because no cleanup is attempted) output is unchanged but
 * the storage may be partially initialized and is STILL owned by the caller,
 * who must release it or discard/retry explicitly; no rollback occurs. */
Ps3TlsReport ps3_tls_initialize_allocated(const ps3_guest_memory *memory,
                                          uint32_t block, uint64_t capacity,
                                          const Ps3TlsTemplate *tls_template,
                                          void *scratch, size_t scratch_size,
                                          Ps3TlsImage *output);
#ifdef __cplusplus
}
#endif
#endif
