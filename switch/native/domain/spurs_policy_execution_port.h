#ifndef PS3_SPURS_POLICY_EXECUTION_PORT_H
#define PS3_SPURS_POLICY_EXECUTION_PORT_H
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS3_SPURS_POLICY_INVALID = 1,
    PS3_SPURS_POLICY_BUSY,
    PS3_SPURS_POLICY_READY,
    PS3_SPURS_POLICY_SLICE_CONTINUE,
    PS3_SPURS_POLICY_KERNEL_SELECT,
    PS3_SPURS_POLICY_KERNEL_EXIT,
    PS3_SPURS_POLICY_STOP,
    PS3_SPURS_POLICY_CANCELLED,
    PS3_SPURS_POLICY_UNSUPPORTED,
    PS3_SPURS_POLICY_FAULT
} ps3_spurs_policy_execution_status;

typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner;
    uint8_t *local_store;
    size_t local_store_size;
    uint32_t instance_address;
    uint32_t spu_index;
    uint32_t workload_id;
    uint32_t module_address;
    uint32_t module_size;
    uint32_t unique_id;
    uint32_t poll_status;
    unsigned module_reloaded;
    uint64_t data;
    void *selection_context;
    /* Synchronous kernel selection: 0 succeeds, CANCELLED cancels,
     * UNSUPPORTED refuses before selection, other
     * nonzero values fault. Never dispatch recursively from this callback. */
    int (*select)(void *,uint32_t,unsigned,uint64_t *);
} ps3_spurs_policy_execution_request;

typedef struct {
    ps3_spurs_policy_execution_status status;
    uint32_t pc;
    uint32_t channel;
    uint32_t stop_code;
    uint64_t hops;
    unsigned resumable;
} ps3_spurs_policy_execution_result;

/* One port context belongs to one SPURS instance. Caller retains its LS and
 * coherent owners until the instance is joined and this adapter is discarded;
 * release ends one module activation, preserving the physical SPU's retained
 * state after KERNEL_EXIT. begin copies the request (no ephemeral request
 * pointer retained). Only one step/lifecycle operation may touch a slot at a
 * time. Selection callbacks are nonthrowing, non-reentrant and cannot dispatch
 * policy code. READY establishes
 * initialized ownership only. SLICE_CONTINUE retains architectural state and promises
 * the next step can resume without replaying effects. Unsupported and fault
 * outcomes never promise policy, job, or frame completion. */
typedef struct {
    void *context;
    ps3_spurs_policy_execution_result (*begin)(void *,const ps3_spurs_policy_execution_request *);
    ps3_spurs_policy_execution_result (*step)(void *,uint32_t,uint64_t);
    ps3_spurs_policy_execution_result (*resume)(void *,uint32_t,uint64_t);
    ps3_spurs_policy_execution_status (*cancel)(void *,uint32_t);
    ps3_spurs_policy_execution_status (*release)(void *,uint32_t);
} ps3_spurs_policy_execution_port;

#ifdef __cplusplus
}
#endif
#endif
