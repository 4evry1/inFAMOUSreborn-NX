#ifndef PS3_NATIVE_SPURS_WORKLOAD_REGISTRATION_H
#define PS3_NATIVE_SPURS_WORKLOAD_REGISTRATION_H
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_SPURS_REGISTRATION_PRIMARY_BYTES = 0x1000 };
typedef enum {
    PS3_SPURS_REGISTRATION_RUNNING = 0,
    PS3_SPURS_REGISTRATION_INVALID_ARGUMENT,
    PS3_SPURS_REGISTRATION_UNOWNED_INSTANCE,
    PS3_SPURS_REGISTRATION_UNSUPPORTED_VARIANT,
    PS3_SPURS_REGISTRATION_UNSUPPORTED_ALIAS,
    PS3_SPURS_REGISTRATION_CORRUPT_STATE,
    PS3_SPURS_REGISTRATION_MEMORY,
    PS3_SPURS_REGISTRATION_CANCELLED,
    PS3_SPURS_REGISTRATION_ATTEMPTS,
    PS3_SPURS_REGISTRATION_OBSERVER,
    PS3_SPURS_REGISTRATION_NO_EXECUTION_SERVICES
} ps3_spurs_registration_stop;
typedef enum {
    PS3_SPURS_REGISTRATION_READ,
    PS3_SPURS_REGISTRATION_WRITE,
    PS3_SPURS_REGISTRATION_COMPARE_EXCHANGE,
    PS3_SPURS_REGISTRATION_CONDITIONAL_LINE
} ps3_spurs_registration_operation;
enum {
    PS3_SPURS_REGISTRATION_SLOT_CLAIMED = 1u << 0,
    PS3_SPURS_REGISTRATION_WID_WRITTEN = 1u << 1,
    PS3_SPURS_REGISTRATION_PREPARING = 1u << 2,
    PS3_SPURS_REGISTRATION_UNIQUE_ID_WRITTEN = 1u << 3,
    PS3_SPURS_REGISTRATION_RUNNABLE_PUBLISHED = 1u << 4,
    PS3_SPURS_REGISTRATION_UPDATE_MESSAGE = 1u << 5,
    PS3_SPURS_REGISTRATION_SERVICE_MESSAGE = 1u << 6
};
typedef struct {
    ps3_spurs_registration_operation operation;
    uint32_t address;
    size_t length;
    /* Valid only during callback. READ carries confirmed read bytes; WRITE
     * carries confirmed written bytes. CAS/conditional-line carry the proposed
     * desired payload even on failure: only committed=true confirms a write.
     * CAS observed/desired describe the actual comparison; failed SC is not a
     * confirmed read or write of these proposed bytes. */
    const unsigned char *bytes;
    uint64_t observed, desired; /* Numeric BE scalar for compare-exchange. */
    unsigned committed;
} ps3_spurs_registration_event;
typedef struct {
    uint32_t spurs, wid_output, policy_module, size;
    uint64_t data;
    unsigned char priority[8];
    uint32_t min_contention, max_contention, name_class, name_instance, hook, hook_argument;
} ps3_spurs_registration_request;
typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner; /* Dedicated serialized registration execution agent. */
    ps3_instance_guard registration_guard; /* Shared per instance; distinct from domain.guard. */
    uint32_t instance;
    unsigned active_owned_instance;
    unsigned max_attempts; /* Each CAS/LLSC retry loop has this finite limit. */
    void *poll_context;
    int (*may_continue)(void *); /* Optional; false stops before next operation. */
    void *observer_context;
    int (*observe)(void *, const ps3_spurs_registration_event *); /* Required; false stops after effect. */
} ps3_spurs_registration_config;
typedef struct {
    ps3_spurs_registration_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result, wid, unique_id, effects;
    size_t reads, writes, written_bytes, observer_calls, publications, atomic_attempts;
} ps3_spurs_registration_result;

/* Caller owns all storage and keeps domain/instance/guard/owner alive until joined.
 * Config, result and observer storage must be disjoint. Request is snapshotted
 * before effects. No guest attribute or names/PM code are read here. Wrapper
 * attr NULL/alignment/revision validation precedes this helper in the adapter.
 * Every confirmed effect is counted before observer publication. A later failure
 * preserves it, sets native stop and leaves has_sdk_result false. No rollback.
 * The observer must not reenter registration; the injected guard is held.
 * SPURS1 only. wid overlapping the owned instance range stops natively before claim.
 * Successful registration publishes runnable metadata, not proof of SPU execution. */
/* Pure helper validation only: returns 0 for invalid host pointers (output untouched),
 * 1 otherwise; sdk_result receives 0 or the reference NULL/ALIGN/INVAL code.
 * Exception STAT and all guest access/ownership remain in apply/runtime. */
int ps3_spurs_workload_registration_validate_request(const ps3_spurs_registration_request *, uint32_t *sdk_result);
void ps3_spurs_workload_registration_apply(const ps3_spurs_registration_config *,
    const ps3_spurs_registration_request *, ps3_spurs_registration_result *);

#ifdef __cplusplus
}
#endif
#endif
