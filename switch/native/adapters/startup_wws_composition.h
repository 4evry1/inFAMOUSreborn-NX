#ifndef PS3_NATIVE_STARTUP_WWS_COMPOSITION_H
#define PS3_NATIVE_STARTUP_WWS_COMPOSITION_H
#include "../domain/spurs_policy_execution_port.h"
#include "clock.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_startup_wws_composition ps3_startup_wws_composition;
enum { PS3_STARTUP_WWS_LANES=5 };
typedef struct {
    ps3_clock clock;
    void *cancellation_context;
    int (*is_cancelled)(void *);
} ps3_startup_wws_config;
typedef struct {
    unsigned initialized,active,entered,retained;
    ps3_spurs_policy_execution_result last;
    int terminal_code;
    ps3_guest_memory_status provider_status;
    unsigned effect_committed;
    uint64_t request_ea;
    uint32_t request_lsa,request_bytes,request_command;
} ps3_startup_wws_diagnostics;
/* C composition root for the actual linked WWS image. One stable owned handle
 * per instance; no SPU register/vector types cross this boundary. Clock and
 * cancellation context stay live until users join. Failure preserves *out.
 * Trace diagnostics are disabled; coherent DMA/atomics still execute normally. */
ps3_spurs_policy_execution_status ps3_startup_wws_create(const ps3_startup_wws_config *,ps3_startup_wws_composition **out);
ps3_spurs_policy_execution_port ps3_startup_wws_port(ps3_startup_wws_composition *);
/* Caller must have confirmed every borrowing worker joined before querying or
 * destroying. Query copies scalar diagnostics; it exposes no borrowed state. */
ps3_spurs_policy_execution_status ps3_startup_wws_read_joined(const ps3_startup_wws_composition *,uint32_t,ps3_startup_wws_diagnostics *);
ps3_spurs_policy_execution_status ps3_startup_wws_destroy_joined(ps3_startup_wws_composition **);
#ifdef __cplusplus
}
#endif
#endif
