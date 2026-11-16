#ifndef PS3_RUNTIME_PPU_COMPILED_DISPATCH_H
#define PS3_RUNTIME_PPU_COMPILED_DISPATCH_H
#include "compiled_code.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_compiled_dispatch_state {
    uint32_t depth;
    uint32_t limit;
} ps3_compiled_dispatch_state;

typedef enum ps3_compiled_dispatch_stop_reason {
    PS3_COMPILED_DISPATCH_CONTINUE = 0,
    PS3_COMPILED_DISPATCH_CANCELLED,
    PS3_COMPILED_DISPATCH_DEADLINE
} ps3_compiled_dispatch_stop_reason;

typedef struct ps3_compiled_dispatch_stop_port {
    void *context;
    ps3_compiled_dispatch_stop_reason (*stop_requested)(void *context);
} ps3_compiled_dispatch_stop_port;

typedef enum ps3_compiled_dispatch_status {
    PS3_COMPILED_DISPATCH_OK = 0,
    PS3_COMPILED_DISPATCH_INVALID_ARGUMENT,
    PS3_COMPILED_DISPATCH_INVALID_STATE,
    PS3_COMPILED_DISPATCH_INVALID_ADDRESS,
    PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED,
    PS3_COMPILED_DISPATCH_LOOKUP_FAILURE,
    PS3_COMPILED_DISPATCH_NULL_TARGET,
    PS3_COMPILED_DISPATCH_STOP_CANCELLED,
    PS3_COMPILED_DISPATCH_STOP_DEADLINE,
    PS3_COMPILED_DISPATCH_INVALID_STOP_REASON
} ps3_compiled_dispatch_status;

typedef struct ps3_compiled_dispatch_result {
    ps3_compiled_dispatch_status status;
    bool lookup_attempted;
    /* Authoritative only when lookup_attempted is true. */
    ps3_compiled_code_status lookup_status;
} ps3_compiled_dispatch_result;

/* One worker exclusively owns state and CPU, except synchronous nested calls
 * from its targets. A nonzero limit and depth <= limit are required. Borrowed
 * lookup/stop dependencies and state remain live through the worker's join.
 * Lookup and stop callbacks are total, nonthrowing, may not reenter dispatch or
 * modify state, and may not terminate/unwind the worker. Stop policy, including
 * the clock/deadline domain, is entirely injected. Once a stop reason is reported,
 * it remains latched until composition explicitly resets it between executions.
 *
 * Only the low 32 bits of captured_ctr identify code. This core never reads an
 * OPD, guest memory, or CPU fields. It checks stop before lookup and again before
 * invoking the exact returned function with cpu. No guards are held at entry.
 * Depth counts indirect calls only, not direct calls or trampoline callbacks.
 *
 * Depth increments immediately before target invocation and decrements only on
 * normal return. A target may terminate the worker; retained depth is evidence,
 * not automatic cleanup. Composition must join that worker before inspecting,
 * resetting/reusing, or releasing its state/CPU/dependencies. Target exceptions
 * crossing this C boundary are unsupported. No destructor performs recovery. */
ps3_compiled_dispatch_result ps3_compiled_dispatch(
    ps3_compiled_dispatch_state *state,
    ps3_compiled_code_lookup lookup,
    ps3_compiled_dispatch_stop_port stop,
    struct ppu_context *cpu,
    uint64_t captured_ctr);

#ifdef __cplusplus
}
#endif
#endif
