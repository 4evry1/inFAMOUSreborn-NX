#ifndef PS3_NATIVE_SPURS_IDLE_REQUEST_H
#define PS3_NATIVE_SPURS_IDLE_REQUEST_H
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PS3_SPURS_IDLE_RUNNING = 0,
    PS3_SPURS_IDLE_INVALID_ARGUMENT,
    PS3_SPURS_IDLE_UNOWNED_INSTANCE,
    PS3_SPURS_IDLE_NO_EXECUTION_SERVICES,
    PS3_SPURS_IDLE_MEMORY,
    PS3_SPURS_IDLE_CANCELLED,
    PS3_SPURS_IDLE_OBSERVER
} ps3_spurs_idle_stop;
typedef enum { PS3_SPURS_IDLE_READ, PS3_SPURS_IDLE_WRITE } ps3_spurs_idle_operation;
typedef struct { uint32_t spurs, wid, count; } ps3_spurs_idle_request;
typedef struct {
    ps3_spurs_idle_operation operation;
    uint32_t address;
    size_t length;
    const unsigned char *bytes; /* Confirmed operation bytes; valid only during callback. */
} ps3_spurs_idle_event;
typedef struct {
    void *context;
    /* Optional. Called after cancellation polling, before each actual operation.
     * Nonthrowing/nonreentrant; no provider access or effect is authorized on false. */
    int (*permits)(void *, uint32_t address, size_t length, unsigned write);
} ps3_spurs_idle_memory_scope;
typedef struct {
    ps3_atomic_domain *domain;
    uint32_t instance;
    unsigned active_owned_instance;
    ps3_instance_guard workload_guard; /* Distinct from domain.guard; owned by composition. */
    void *poll_context;
    int (*may_continue)(void *);
    void *observer_context;
    int (*observe)(void *, const ps3_spurs_idle_event *);
    ps3_spurs_idle_memory_scope memory_scope;
} ps3_spurs_idle_config;
typedef struct {
    ps3_spurs_idle_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result;
    size_t reads, writes, written_bytes, observer_calls, publications;
    unsigned idle_byte_write_attempted, idle_byte_written;
} ps3_spurs_idle_result;
/* Pure NULL/alignment checks only. Invalid host pointers return0/output untouched;
 * otherwise return1 and output0 or CORE NULL/ALIGN. flags must precede wid/count. */
int ps3_spurs_idle_request_validate_address(const ps3_spurs_idle_request *, uint32_t *sdk_result);
/* Caller captures low32 guest arguments and owns stable config/result storage.
 * Request is snapshotted before effects. Confirmed effects advance before the
 * observer, and remain on observer/cancellation failure without SDK return.
 * Observer must not reenter the workload guard. No ready-count write or synthetic wake. */
void ps3_spurs_idle_request_apply(const ps3_spurs_idle_config *,
    const ps3_spurs_idle_request *, ps3_spurs_idle_result *);
#ifdef __cplusplus
}
#endif
#endif
