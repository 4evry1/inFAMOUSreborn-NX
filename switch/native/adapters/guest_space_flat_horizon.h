#ifndef PS3_GUEST_SPACE_FLAT_HORIZON_H
#define PS3_GUEST_SPACE_FLAT_HORIZON_H
/*
 * Flat guest memory for an NRO on Horizon (root, 2026-11-12; Decision 57).
 *
 * The heap backend (heap_guest_space) has no contiguous guest base: every access is a table walk through a port.
 * A renderer needs real pointers to guest textures and vertex buffers, and fast lifted code needs `base + address`.
 * svcMapPhysicalMemory (guest_space_horizon) gives that but is not available to an NRO.
 *
 * This adapter reserves a 4 GiB window of virtual space and backs it on demand in 2 MiB slabs. A slab is heap
 * memory that is turned into a code alias and then mirrored into the window:
 *     memalign -> virtmemFindCodeMemory -> svcMapProcessCodeMemory -> svcSetProcessMemoryPermission(RW)
 *     -> svcMapProcessMemory(window + slab offset)
 * (heap memory cannot be the source of svcMapProcessMemory directly). The sequence is the one the user's
 * Xbox 360 ports use on the same console (captain-america-nx, sdk/src/core/guest_memory_switch.*); this file is an
 * independent implementation of it, not a copy.
 *
 * It implements the ps3_guest_memory port with the same rules as the other backends (page-granular commit, fresh
 * pages zero-filled, read/write/zero refuse a range with an uncommitted page), and also exposes the window base.
 * A pointer into the window is valid while its pages stay committed; the port's lock does not cover it.
 */
#include "../../../ps3recomp/runtime/memory/guest_space.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_flat_guest_space ps3_flat_guest_space;
typedef enum ps3_flat_guest_stage {
    PS3_FLAT_GUEST_COMPLETE = 0,
    PS3_FLAT_GUEST_ARGUMENT,
    PS3_FLAT_GUEST_ALLOCATE,
    PS3_FLAT_GUEST_PROCESS_HANDLE,
    PS3_FLAT_GUEST_RESERVE_WINDOW,
    PS3_FLAT_GUEST_PROBE_SLAB
} ps3_flat_guest_stage;
typedef struct ps3_flat_guest_diagnostic {
    ps3_flat_guest_stage stage;
    uint32_t kernel_result;     /* of the last failing kernel call, 0 when none */
    uint32_t slab_step;         /* which step of the slab sequence failed: 1 memalign, 2 find code space,
                                   3 map code memory, 4 set permission, 5 map into the window */
} ps3_flat_guest_diagnostic;
typedef struct ps3_flat_guest_stats {
    uint64_t committed_pages;
    uint64_t slabs;             /* live 2 MiB slabs */
    uint64_t slab_acquires, slab_releases, slab_failures;
    uint32_t last_kernel_result;
    uint32_t last_slab_step;
} ps3_flat_guest_stats;
/* max_pages is the logical quota in 4 KiB pages. Creation proves the whole slab sequence once (one slab is
 * acquired and released) so a platform that refuses it is found here, before any guest state exists. */
ps3_guest_memory_status ps3_flat_guest_space_create(ps3_flat_guest_space **out, uint64_t max_pages,
                                                    ps3_flat_guest_diagnostic *diagnostic);
const char *ps3_flat_guest_stage_name(ps3_flat_guest_stage stage);
ps3_guest_memory ps3_flat_guest_space_port(ps3_flat_guest_space *space);
uintptr_t ps3_flat_guest_space_base(ps3_flat_guest_space *space);
/* A host pointer to [address, address + length) when every page of it is committed, else NULL. The pointer stays
 * valid until one of those pages is decommitted; the caller coordinates that (the renderer reads guest memory the
 * game has handed to the RSX, which the game does not unmap while it is in use). */
const void *ps3_flat_guest_space_pointer(ps3_flat_guest_space *space, uint32_t address, uint64_t length);
/* Lock-free variant for the hot path of generated code, on the process's space (g_ps3_flat_guest_space): a host
 * pointer when every page of [address, address + length) is committed, else NULL (also when there is no flat
 * space). `length` is at most one page. It reads the commit bitmap without the lock, so it is only for callers
 * that already rely on the guest not unmapping memory it is still using. */
void *ps3_flat_guest_fast_pointer(uint32_t address, uint32_t length);
/* Root (2026-11-15): the same for a range of any length (not limited to one page): a host pointer when every page of
 * [address, address + length) is committed, else NULL. Lock-free, same reliance as ps3_flat_guest_fast_pointer; for the RSX consumer
 * and the renderer's guest reader (the locked ps3_flat_guest_space_pointer stays for everything else). */
const void *ps3_flat_guest_fast_range(uint32_t address, uint64_t length);
const unsigned char *ps3_flat_guest_committed_bitmap(void);
ps3_flat_guest_stats ps3_flat_guest_space_stats(ps3_flat_guest_space *space);
/* Caller must stop every user first. */
ps3_guest_memory_status ps3_flat_guest_space_destroy(ps3_flat_guest_space **space);
/* The window base of the process's flat guest space, or 0 while there is none (set by the composition). */
extern uintptr_t g_ps3_flat_guest_base;
extern volatile uint32_t g_ps3_flat_guest_stage;
extern const volatile uint32_t *g_ps3_flat_guest_lock_word;
/* The process's flat guest space itself, or NULL (set by the composition together with the base). */
extern ps3_flat_guest_space *g_ps3_flat_guest_space;
#ifdef __cplusplus
}
#endif
#endif
