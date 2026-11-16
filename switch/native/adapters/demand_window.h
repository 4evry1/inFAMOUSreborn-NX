#ifndef PS3_DEMAND_WINDOW_H
#define PS3_DEMAND_WINDOW_H
/* Commit-on-demand guest window decorator (RSX local memory policy B).
 *
 * The decorated range [base, base + size) is split into power-of-two granules.
 * Read/write/zero transfers that lie wholly inside the range first commit every
 * granule they cover through the backing port (the backing's commit zero-fills
 * new pages), then forward the transfer. Transfers that straddle a window edge
 * are refused with INVALID_RANGE and never forwarded: partial coverage would
 * fabricate data the guest never wrote. commit/decommit calls intersecting the
 * window are refused the same way; the window owns its granules.
 *
 * The committed set is bounded twice: a declared cap_bytes (a multiple of the
 * granule, never more than size) and the backing's own quota. Exhaustion is
 * recorded once (first exhaustion only) with the granule offset and status, and
 * returned to the caller; every granule this call committed before a backing
 * failure is decommitted again, so a failed access leaves no kernel state.
 * Nothing is ever fabricated and no host pointer is exposed.
 *
 * Lock order is caller domain guard, then this window's guard, then the
 * backing's guard. The backing never calls back into this port and the guard is
 * never held while a transfer is forwarded. */
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/memory/guest_space.h"
#include "../../../ps3recomp/runtime/sync/guard.h"
#ifdef __cplusplus
extern "C" {
#endif
#define PS3_DEMAND_WINDOW_MAX_GRANULES 4096u
#define PS3_DEMAND_WINDOW_JOURNAL 64u
typedef enum ps3_demand_window_exhaustion {
    PS3_DEMAND_WINDOW_NOT_EXHAUSTED = 0,
    PS3_DEMAND_WINDOW_CAP = 1,
    PS3_DEMAND_WINDOW_BACKING = 2
} ps3_demand_window_exhaustion;
typedef struct ps3_demand_window_stats {
    uint64_t granule_commits;        /* successful on-demand granule commits */
    uint64_t committed_bytes;        /* granule_commits * granule */
    uint64_t triggering_reads, triggering_writes, triggering_zeros; /* transfers that committed >= 1 granule */
    uint32_t lowest_offset;          /* lowest committed granule offset (relative to base); 0 when none */
    uint32_t highest_end_offset;     /* highest committed granule offset + granule; 0 when none */
    uint32_t journal_count;          /* <= PS3_DEMAND_WINDOW_JOURNAL */
    uint32_t journal[PS3_DEMAND_WINDOW_JOURNAL]; /* offsets of the first 64 committed granules, commit order */
    uint32_t exhaustion;             /* first exhaustion only (ps3_demand_window_exhaustion) */
    uint32_t exhaustion_offset;      /* granule offset that could not be committed */
    uint32_t exhaustion_status;      /* NO_MEMORY for CAP; the backing's status for BACKING */
    uint64_t refused_window_commits; /* commit/decommit calls refused because they intersect the window */
    uint64_t refused_partial;        /* transfers straddling a window edge */
} ps3_demand_window_stats;
typedef struct ps3_demand_window {   /* caller-owned storage; zero-initialise before init; fields private */
    ps3_guest_memory backing;
    ps3_instance_guard guard;
    uint32_t base, size, granule;
    uint64_t cap_bytes;
    uint8_t initialized;
    uint8_t bitmap[PS3_DEMAND_WINDOW_MAX_GRANULES / 8];
    ps3_demand_window_stats stats;
} ps3_demand_window;
/* Composition owns the backing, the guard and their contexts until the window
 * is retired. The guard must be distinct from the backing's guard and every
 * decorator's guard. Init rejects a nonempty pointer already set, a partially
 * initialised storage and every geometric violation with INVALID_RANGE, leaving
 * the storage unchanged. */
ps3_guest_memory_status ps3_demand_window_init(ps3_demand_window *window,
    ps3_guest_memory backing, ps3_instance_guard guard, uint32_t base, uint32_t size,
    uint32_t granule, uint64_t cap_bytes);
ps3_guest_memory ps3_demand_window_port(ps3_demand_window *window);
ps3_demand_window_stats ps3_demand_window_read_stats(ps3_demand_window *window);
#ifdef __cplusplus
}
#endif
#endif
