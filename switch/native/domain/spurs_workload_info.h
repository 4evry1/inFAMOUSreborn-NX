#ifndef PS3_NATIVE_SPURS_WORKLOAD_INFO_H
#define PS3_NATIVE_SPURS_WORKLOAD_INFO_H
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PS3_SPURS_WORKLOAD_INFO_RUNNING=0,
    PS3_SPURS_WORKLOAD_INFO_INVALID_ARGUMENT,
    PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE,
    PS3_SPURS_WORKLOAD_INFO_MEMORY,
    PS3_SPURS_WORKLOAD_INFO_CANCELLED,
    PS3_SPURS_WORKLOAD_INFO_OBSERVER
} ps3_spurs_workload_info_stop;
typedef enum { PS3_SPURS_WORKLOAD_INFO_READ,PS3_SPURS_WORKLOAD_INFO_WRITE } ps3_spurs_workload_info_operation;
typedef enum {
    PS3_SPURS_WORKLOAD_INFO_FLAGS=1,PS3_SPURS_WORKLOAD_INFO_ENABLED,
    PS3_SPURS_WORKLOAD_INFO_PRIORITY,PS3_SPURS_WORKLOAD_INFO_DATA,PS3_SPURS_WORKLOAD_INFO_SIZE,
    PS3_SPURS_WORKLOAD_INFO_POLICY,PS3_SPURS_WORKLOAD_INFO_INSTANCE_NAME,
    PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_ARGUMENT,PS3_SPURS_WORKLOAD_INFO_CLASS_NAME,
    PS3_SPURS_WORKLOAD_INFO_SHUTDOWN_HOOK,PS3_SPURS_WORKLOAD_INFO_FLAGS_RECHECK,
    PS3_SPURS_WORKLOAD_INFO_CONTENTION,PS3_SPURS_WORKLOAD_INFO_MAX_CONTENTION,
    PS3_SPURS_WORKLOAD_INFO_MIN_CONTENTION,PS3_SPURS_WORKLOAD_INFO_READY,
    PS3_SPURS_WORKLOAD_INFO_IDLE,PS3_SPURS_WORKLOAD_INFO_SIGNAL
} ps3_spurs_workload_info_stage;
typedef struct { uint32_t spurs,wid,info; } ps3_spurs_workload_info_request;
typedef struct {
    ps3_spurs_workload_info_operation operation;
    ps3_spurs_workload_info_stage stage;
    uint32_t address;
    size_t length;
    const unsigned char *bytes;
} ps3_spurs_workload_info_event;
typedef struct {
    void *context;
    /* Last argument: writing=0 for read, 1 for write. */
    int (*permits)(void *,uint32_t,size_t,unsigned);
} ps3_spurs_workload_info_memory_scope;
typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner;
    uint32_t instance,owned_instance_bytes;
    unsigned active_owned_instance;
    void *poll_context;
    int (*may_continue)(void *);
    void *observer_context;
    int (*observe)(void *,const ps3_spurs_workload_info_event *);
    ps3_spurs_workload_info_memory_scope memory_scope;
} ps3_spurs_workload_info_config;
typedef struct {
    ps3_spurs_workload_info_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result;
    /* Actual operation counters precede observation; publications counts
     * successful observer returns, not guest readiness publications. */
    size_t reads,writes,written_bytes,write_attempts,observer_calls,publications;
} ps3_spurs_workload_info_result;

/* Only authentic scalar NULL and SPURS alignment checks; no output alignment. */
int ps3_spurs_workload_info_validate_address(const ps3_spurs_workload_info_request *,uint32_t *sdk_result);
/* Captures config/request by value and borrows the live coherent owner. No
 * whole-call snapshot/guard, retry, output preflight, barrier, or padding write.
 * Source reads stay inside the owned instance; output may alias any scoped RAM.
 * Poll/scope precede each operation. False scope denies it before effects.
 * Nonthrowing observers run outside the guard after confirmed operations;
 * event bytes live only during the callback. Later stops retain earlier writes
 * without claiming SDK return. No execution-policy services are prerequisites. */
void ps3_spurs_workload_info_apply(const ps3_spurs_workload_info_config *,
    const ps3_spurs_workload_info_request *,ps3_spurs_workload_info_result *);
#ifdef __cplusplus
}
#endif
#endif
