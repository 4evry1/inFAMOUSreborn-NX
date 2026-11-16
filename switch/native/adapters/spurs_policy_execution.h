#ifndef PS3_NATIVE_SPURS_POLICY_EXECUTION_H
#define PS3_NATIVE_SPURS_POLICY_EXECUTION_H
#include "../domain/spurs_policy_execution_port.h"
#include "spu_executor.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_SPURS_POLICY_EXECUTION_LANES=5 };
typedef struct {
    const ps3_spu_linked_image *image;
    ps3_spu_lifted_entry entry;
    ps3_clock clock;
    ps3_spu_executor_services services;
    uint32_t kernel_select_pc, kernel_exit_pc;
} ps3_spurs_policy_execution_config;
struct ps3_spurs_policy_execution;
typedef struct {
    ps3_spu_executor executor;
    struct ps3_spurs_policy_execution *adapter;
    ps3_spurs_policy_execution_request request;
    ps3_spurs_policy_execution_result last;
    unsigned active, entered, cancel_requested, retained;
} ps3_spurs_policy_execution_lane;
typedef struct ps3_spurs_policy_execution {
    ps3_spurs_policy_execution_config config;
    ps3_spurs_policy_execution_lane lanes[PS3_SPURS_POLICY_EXECUTION_LANES];
    unsigned initialized;
} ps3_spurs_policy_execution;
/* Fresh caller-owned, zeroed adapter. Image/clock/services and all pointed-to
 * state remain live until every lane is released. No allocation, registration
 * globals, worker creation or generated-image lookup occurs here. */
ps3_spurs_policy_execution_status ps3_spurs_policy_execution_init(ps3_spurs_policy_execution *,const ps3_spurs_policy_execution_config *);
ps3_spurs_policy_execution_port ps3_spurs_policy_execution_bind(ps3_spurs_policy_execution *);
#ifdef __cplusplus
}
#endif
#endif
