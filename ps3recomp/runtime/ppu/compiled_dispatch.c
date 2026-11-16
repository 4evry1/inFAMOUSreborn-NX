#include "compiled_dispatch.h"
#include <stddef.h>

static ps3_compiled_dispatch_result result(ps3_compiled_dispatch_status status,
    bool lookup_attempted, ps3_compiled_code_status lookup_status)
{
    ps3_compiled_dispatch_result value = {status, lookup_attempted, lookup_status};
    return value;
}

static ps3_compiled_dispatch_status stop_status(
    ps3_compiled_dispatch_stop_reason reason)
{
    switch (reason) {
    case PS3_COMPILED_DISPATCH_CONTINUE:
        return PS3_COMPILED_DISPATCH_OK;
    case PS3_COMPILED_DISPATCH_CANCELLED:
        return PS3_COMPILED_DISPATCH_STOP_CANCELLED;
    case PS3_COMPILED_DISPATCH_DEADLINE:
        return PS3_COMPILED_DISPATCH_STOP_DEADLINE;
    default:
        return PS3_COMPILED_DISPATCH_INVALID_STOP_REASON;
    }
}

ps3_compiled_dispatch_result ps3_compiled_dispatch(
    ps3_compiled_dispatch_state *state,
    ps3_compiled_code_lookup lookup,
    ps3_compiled_dispatch_stop_port stop,
    struct ppu_context *cpu,
    uint64_t captured_ctr)
{
    const ps3_compiled_code_status not_looked_up = PS3_COMPILED_CODE_NOT_ADMITTED;
    if (!state || !cpu || !lookup.find || !stop.stop_requested) {
        return result(PS3_COMPILED_DISPATCH_INVALID_ARGUMENT, false, not_looked_up);
    }
    if (state->limit == 0 || state->depth > state->limit) {
        return result(PS3_COMPILED_DISPATCH_INVALID_STATE, false, not_looked_up);
    }
    const uint32_t address = (uint32_t)captured_ctr;
    if (address == 0 || (address & 3) != 0) {
        return result(PS3_COMPILED_DISPATCH_INVALID_ADDRESS, false, not_looked_up);
    }
    if (state->depth == state->limit) {
        return result(PS3_COMPILED_DISPATCH_DEPTH_EXHAUSTED, false, not_looked_up);
    }
    ps3_compiled_dispatch_status status = stop_status(stop.stop_requested(stop.context));
    if (status != PS3_COMPILED_DISPATCH_OK) {
        return result(status, false, not_looked_up);
    }
    ps3_compiled_function target = NULL;
    const ps3_compiled_code_status lookup_status = lookup.find(lookup.context,
        address, &target);
    if (lookup_status != PS3_COMPILED_CODE_OK) {
        return result(PS3_COMPILED_DISPATCH_LOOKUP_FAILURE, true, lookup_status);
    }
    if (!target) {
        return result(PS3_COMPILED_DISPATCH_NULL_TARGET, true, lookup_status);
    }
    status = stop_status(stop.stop_requested(stop.context));
    if (status != PS3_COMPILED_DISPATCH_OK) {
        return result(status, true, lookup_status);
    }
    ++state->depth;
    target(cpu);
    --state->depth;
    return result(PS3_COMPILED_DISPATCH_OK, true, lookup_status);
}
