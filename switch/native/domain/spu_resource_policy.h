#ifndef NATIVE_SPU_RESOURCE_POLICY_H
#define NATIVE_SPU_RESOURCE_POLICY_H
#include "../../../ps3recomp/runtime/sync/guard.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#include "../../../ps3recomp/runtime/memory/user_memory.h"
#include "lv2_objects.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_SPU_COMPATIBILITY_UNITS = 6, PS3_SPU_RAW_LIMIT = 5 };
typedef enum {
    PS3_SPU_POLICY_OK,
    PS3_SPU_POLICY_INVALID_RAW,
    PS3_SPU_POLICY_INVALID_STATE,
    PS3_SPU_POLICY_BUSY            /* live groups exceed the requested managed limit (pinned check_busy) */
} ps3_spu_policy_status;
typedef struct { uint32_t managed_limit, raw_limit; } ps3_spu_policy_limits;
/* SPU thread-group authority. Group/thread records, admission (pinned max
 * rule, sys_spu.cpp:419-478), the default-container charge (shared user-memory
 * quota) and event connections live here under the same guard as
 * reconfiguration. Thread images are typed native descriptors only (R-IMG):
 * no guest SPU image is admitted. No executor is bound here. */
enum { PS3_SPU_GROUP_CAP = 4, PS3_SPU_GROUP_THREADS = 8, PS3_SPU_PORTS = 64,
       PS3_SPU_GROUP_BASE = 0x04000100u, PS3_SPU_GROUP_STEP = 0x100u, PS3_SPU_LS_CHARGE = 0x40000u };
typedef enum { PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1 = 1 } ps3_spu_image_kind;
typedef enum { PS3_SPU_GROUP_NOT_INITIALIZED = 1, PS3_SPU_GROUP_INITIALIZED, PS3_SPU_GROUP_RUNNING,
               PS3_SPU_GROUP_STOPPED } ps3_spu_group_state;
typedef struct {
    uint32_t id, image_kind, option;
    uint64_t arg1, arg2;
    uint32_t port_queue[PS3_SPU_PORTS];  /* spup -> connected event queue */
    unsigned active;
} ps3_spu_thread_record;
typedef struct {
    uint32_t id, num, init_count, state;
    int32_t prio, type;
    uint32_t exception_queue;
    const void *executor;   /* token of the started executor (bound by mark_running, released after its join) */
    unsigned joiner;        /* a blocked joiner exists: the record cannot be destroyed or recycled */
    ps3_user_memory_charge_token charge;
    ps3_spu_thread_record threads[PS3_SPU_GROUP_THREADS];
    uint64_t terminal;   /* futex word: 1 once the group stopped */
    unsigned active;
} ps3_spu_group_record;
typedef struct {
    ps3_instance_guard guard;
    ps3_spu_policy_limits limits;
    ps3_user_memory *container;
    ps3_lv2_objects *lv2;
    ps3_notify_port port;
    uint32_t serial;
    ps3_spu_group_record groups[PS3_SPU_GROUP_CAP];
} ps3_spu_resource_policy;
typedef struct {
    ps3_spu_policy_status status;
    ps3_spu_policy_limits previous, current;
    int has_snapshot;
} ps3_spu_policy_result;
/* Storage contract: zeroed, constructed once; a second init is refused (INVALID_STATE). Lock order:
 * policy guard -> lv2 guard -> notify port; attach refuses a shared guard. All components that use
 * the blocked-word handshake share one notify port. */
ps3_spu_policy_status ps3_spu_resource_policy_init(ps3_spu_resource_policy *, ps3_instance_guard);
ps3_spu_policy_result ps3_spu_resource_policy_configure(ps3_spu_resource_policy *, uint32_t usable, uint32_t raw);
ps3_spu_policy_result ps3_spu_resource_policy_snapshot(ps3_spu_resource_policy *);
/* Attach the default container, lv2 objects and wait port once, before use. */
int ps3_spu_resource_policy_attach(ps3_spu_resource_policy *, ps3_user_memory *, ps3_lv2_objects *, ps3_notify_port);
/* Return codes are lv2 error numbers (PS3_LV2_*); ENOMEM = 0x80010004. */
enum { PS3_SPU_ENOMEM = 0x80010004u, PS3_SPU_ESTAT = 0x8001000Fu };
uint32_t ps3_spu_group_create(ps3_spu_resource_policy *, uint32_t *id, uint32_t num, int32_t prio, int32_t type, uint32_t name_len);
uint32_t ps3_spu_thread_initialize(ps3_spu_resource_policy *, uint32_t *tid, uint32_t group, uint32_t spu_num,
                                   uint32_t image_kind, uint64_t arg1, uint64_t arg2, uint32_t option, uint32_t name_len);
uint32_t ps3_spu_group_connect_event_all_threads(ps3_spu_resource_policy *, uint32_t group, uint32_t queue, uint64_t req, uint8_t *spup);
/* L4: a stored queue binding (SPU port / EXCEPTION slot) whose lv2 queue no longer exists is dead and does not
 * occupy its slot; liveness is read through the attached lv2 (no reverse lv2->policy dependency). */
uint32_t ps3_spu_group_disconnect_event_all_threads(ps3_spu_resource_policy *, uint32_t group, uint8_t spup);
/* Exact, all-or-nothing counterpart for a binding some other owner made: under the policy guard EVERY thread of the
 * group must STORE `queue` on `spup` (stored-id match only, no lv2 liveness lookup), then all are cleared. Any
 * foreign / already-clear / different-id slot on even one thread writes nothing and returns ESRCH, so a foreign binding is
 * never stripped. A DEAD binding (its lv2 queue destroyed) that still stores `queue` on every thread is cleared and returns OK:
 * our own stale binding occupies nothing and must not wedge shutdown. EINVAL for spup >= PS3_SPU_PORTS or queue == 0; ESRCH for
 * an unknown group. The unconditional variant above is unchanged. */
uint32_t ps3_spu_group_disconnect_event_all_threads_exact(ps3_spu_resource_policy *, uint32_t group, uint8_t spup, uint32_t queue);
/* lv2 numbering (sys_spu.h:35-37). RUN and SYSTEM_MODULE are legal but unowned -> UNSUPPORTED. */
enum { PS3_SPU_EVENT_RUN = 1, PS3_SPU_EVENT_EXCEPTION = 2, PS3_SPU_EVENT_SYSTEM_MODULE = 4 };
uint32_t ps3_spu_group_connect_event(ps3_spu_resource_policy *, uint32_t group, uint32_t queue, uint32_t event_type);
/* RUNNING is entered only with the token of the executor that really started the group; destroy
 * refuses (EBUSY) while that binding exists. mark_stopped requires the same token (EINVAL otherwise)
 * and is published only after the executor's confirmed join; release_executor then requires STOPPED. */
uint32_t ps3_spu_group_mark_running(ps3_spu_resource_policy *, uint32_t group, const void *executor);
uint32_t ps3_spu_group_mark_stopped(ps3_spu_resource_policy *, uint32_t group, const void *executor);
uint32_t ps3_spu_group_release_executor(ps3_spu_resource_policy *, uint32_t group, const void *executor);
/* ESRCH unknown, ESTAT below INITIALIZED, EBUSY second joiner; blocks until the group stops (OK) or
 * cancel (PS3_LV2_CANCELLED); *blocked_word as lv2 receive. Destroy refuses while a joiner is parked. */
uint32_t ps3_spu_group_join(ps3_spu_resource_policy *, uint32_t group, ps3_notify_cancel *, uint64_t *blocked_word);
uint32_t ps3_spu_group_destroy(ps3_spu_resource_policy *, uint32_t group);
uint32_t ps3_spu_group_snapshot(ps3_spu_resource_policy *, uint32_t group, ps3_spu_group_record *out);
#ifdef __cplusplus
}
#endif
#endif
