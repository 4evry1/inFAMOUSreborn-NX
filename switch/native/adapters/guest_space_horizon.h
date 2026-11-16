#ifndef PS3_GUEST_SPACE_HORIZON_H
#define PS3_GUEST_SPACE_HORIZON_H
#include "../../../ps3recomp/runtime/memory/guest_space.h"
typedef struct ps3_horizon_guest_space ps3_horizon_guest_space;
typedef struct ps3_horizon_guest_stats {
    uint64_t committed_pages;
    uint64_t ownership_pages;
    uint32_t last_kernel_result;
} ps3_horizon_guest_stats;
typedef enum ps3_horizon_guest_create_stage {
    PS3_HORIZON_CREATE_COMPLETE = 0,
    PS3_HORIZON_CREATE_ARGUMENT,
    PS3_HORIZON_CREATE_ALLOCATE,
    PS3_HORIZON_CREATE_ALIAS_ADDRESS,
    PS3_HORIZON_CREATE_ALIAS_SIZE,
    PS3_HORIZON_CREATE_ALIAS_BOUNDS,
    PS3_HORIZON_CREATE_QUERY_MEMORY,
    PS3_HORIZON_CREATE_FIND_WINDOW,
    PS3_HORIZON_CREATE_RESERVATION,
    PS3_HORIZON_CREATE_MAP_MARKER
} ps3_horizon_guest_create_stage;
typedef struct ps3_horizon_guest_create_diagnostic {
    ps3_horizon_guest_create_stage stage;
    uint32_t kernel_result;
    int kernel_result_valid;
} ps3_horizon_guest_create_diagnostic;
/* Diagnostic is caller-owned; kernel_result is applicable only when valid. */
ps3_guest_memory_status ps3_horizon_guest_space_create_diagnostic(
    ps3_horizon_guest_space **out, ps3_horizon_guest_create_diagnostic *diagnostic);
const char *ps3_horizon_guest_create_stage_name(ps3_horizon_guest_create_stage stage);
ps3_guest_memory_status ps3_horizon_guest_space_create(ps3_horizon_guest_space **out);
/* Caller must stop other users before destroy. On failure context remains owned
 * by caller with remaining mappings recorded, allowing cleanup retry. */
ps3_guest_memory_status ps3_horizon_guest_space_destroy(ps3_horizon_guest_space **space);
ps3_guest_memory ps3_horizon_guest_space_port(ps3_horizon_guest_space *space);
/* Direct pointer accesses require caller synchronization against decommit. */
uintptr_t ps3_horizon_guest_space_base(ps3_horizon_guest_space *space);
ps3_horizon_guest_stats ps3_horizon_guest_space_stats(ps3_horizon_guest_space *space);
#endif
