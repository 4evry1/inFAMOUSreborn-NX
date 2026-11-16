#ifndef PS3_NATIVE_SPURS_EXCEPTION_HANDLER_H
#define PS3_NATIVE_SPURS_EXCEPTION_HANDLER_H
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PS3_SPURS_EXCEPTION_RUNNING=0,
    PS3_SPURS_EXCEPTION_INVALID_ARGUMENT,
    PS3_SPURS_EXCEPTION_UNOWNED_INSTANCE,
    PS3_SPURS_EXCEPTION_MEMORY,
    PS3_SPURS_EXCEPTION_CANCELLED,
    PS3_SPURS_EXCEPTION_OBSERVER,
    PS3_SPURS_EXCEPTION_ATTEMPT_LIMIT,
    PS3_SPURS_EXCEPTION_NO_EXECUTION_SERVICES
} ps3_spurs_exception_stop;
typedef enum { PS3_SPURS_EXCEPTION_SET, PS3_SPURS_EXCEPTION_UNSET } ps3_spurs_exception_action;
typedef enum { PS3_SPURS_EXCEPTION_READ, PS3_SPURS_EXCEPTION_WRITE } ps3_spurs_exception_operation;
typedef enum {
    PS3_SPURS_EXCEPTION_FLAGS=1, PS3_SPURS_EXCEPTION_ENABLED, PS3_SPURS_EXCEPTION_STATE,
    PS3_SPURS_EXCEPTION_LOAD_RESERVE, PS3_SPURS_EXCEPTION_CLAIM,
    PS3_SPURS_EXCEPTION_ARGUMENT, PS3_SPURS_EXCEPTION_PUBLISH, PS3_SPURS_EXCEPTION_CLEAR
} ps3_spurs_exception_stage;
typedef struct {
    ps3_spurs_exception_action action;
    uint32_t spurs,wid,handler,argument;
} ps3_spurs_exception_request;
typedef struct {
    ps3_spurs_exception_operation operation;
    ps3_spurs_exception_stage stage;
    uint32_t address;
    size_t length;
    const unsigned char *bytes;
} ps3_spurs_exception_event;
typedef struct {
    void *context;
    int (*permits)(void *,uint32_t,size_t,unsigned);
} ps3_spurs_exception_memory_scope;
typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner;
    uint32_t instance,owned_instance_bytes;
    unsigned active_owned_instance;
    size_t max_claim_attempts;
    void *poll_context;
    int (*may_continue)(void *);
    void *observer_context;
    int (*observe)(void *,const ps3_spurs_exception_event *);
    ps3_spurs_exception_memory_scope memory_scope;
} ps3_spurs_exception_config;
typedef struct {
    ps3_spurs_exception_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result,slot;
    size_t reads,writes,written_bytes,observer_calls,publications;
    size_t claim_attempts,conditional_attempts,conditional_failures,release_fences;
    unsigned claim_write_attempted,claim_written;
    unsigned argument_write_attempted,argument_written;
    unsigned handler_write_attempted,handler_written;
    unsigned unset_write_attempted,handler_cleared;
} ps3_spurs_exception_result;
/* Pure authentic NULL/handler-NULL/alignment checks; no ownership or memory
 * access. Caller captures the ABI low32 values before constructing request. */
int ps3_spurs_exception_validate_address(const ps3_spurs_exception_request *,uint32_t *sdk_result);
/* Config/request are captured by value. The owner is borrowed, never acquired,
 * cleared or unregistered here; it must remain valid for this operation. No
 * whole-operation mutex is taken: authentic setter/unsetter may interleave.
 * owned_instance_bytes is 0x1000 or 0x2000. Scope/poll run before each actual
 * access; observer runs after each confirmed read/committed write, outside the
 * coherent-memory guard. Callbacks are nonthrowing; bytes live only during the
 * observer call. Failed conditional stores publish no proposed-byte event.
 * Native attempt/cancellation/provider/observer stops never invent SDK results
 * or roll back a confirmed sentinel, argument or handler publication. This API
 * implements registration bytes, not exception generation or callback delivery. */
void ps3_spurs_exception_handler_apply(const ps3_spurs_exception_config *,
    const ps3_spurs_exception_request *,ps3_spurs_exception_result *);
#ifdef __cplusplus
}
#endif
#endif
