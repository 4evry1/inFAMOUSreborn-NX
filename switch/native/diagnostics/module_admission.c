#include "module_admission.h"

ps3_diagnostic_module_status ps3_diagnostic_module_init(
    ps3_diagnostic_module_state *state, ps3_instance_guard guard,
    const uint16_t *catalog, size_t catalog_count,
    ps3_diagnostic_module_slot *slots, size_t slot_capacity)
{
    if (!state || state->initialized || !guard.lock || !guard.unlock ||
        !catalog || !slots || !catalog_count || catalog_count > slot_capacity)
        return PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT;

    for (size_t i = 0; i < catalog_count; ++i)
        for (size_t j = 0; j < i; ++j)
            if (catalog[i] == catalog[j])
                return PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT;

    for (size_t i = 0; i < catalog_count; ++i) {
        slots[i].module_id = catalog[i];
        slots[i].admission_count = 0;
    }
    state->guard = guard;
    state->slots = slots;
    state->slot_count = catalog_count;
    state->total_admissions = 0;
    state->initialized = 1;
    return PS3_DIAGNOSTIC_MODULE_OK;
}

static ps3_diagnostic_module_slot *find_slot(
    ps3_diagnostic_module_state *state, uint16_t module_id)
{
    for (size_t i = 0; i < state->slot_count; ++i)
        if (state->slots[i].module_id == module_id)
            return &state->slots[i];
    return NULL;
}

ps3_diagnostic_module_result ps3_diagnostic_module_admit(
    ps3_diagnostic_module_state *state, uint16_t module_id)
{
    ps3_diagnostic_module_result result = {
        PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT, module_id, 0, 0, 0
    };
    if (!state)
        return result;
    if (!state->initialized) {
        result.status = PS3_DIAGNOSTIC_MODULE_NOT_INITIALIZED;
        return result;
    }

    state->guard.lock(state->guard.context);
    ps3_diagnostic_module_slot *slot = find_slot(state, module_id);
    result.total_admissions = state->total_admissions;
    if (!slot) {
        result.status = PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE;
    } else {
        result.previous_count = slot->admission_count;
        result.current_count = slot->admission_count;
        if (slot->admission_count == UINT32_MAX || state->total_admissions == UINT64_MAX) {
            result.status = PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED;
        } else {
            result.status = PS3_DIAGNOSTIC_MODULE_OK;
            result.current_count = ++slot->admission_count;
            result.total_admissions = ++state->total_admissions;
        }
    }
    state->guard.unlock(state->guard.context);
    return result;
}

ps3_diagnostic_module_status ps3_diagnostic_module_read(
    ps3_diagnostic_module_state *state, uint16_t module_id,
    ps3_diagnostic_module_result *snapshot)
{
    if (!state || !snapshot)
        return PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT;
    if (!state->initialized)
        return PS3_DIAGNOSTIC_MODULE_NOT_INITIALIZED;

    state->guard.lock(state->guard.context);
    ps3_diagnostic_module_slot *slot = find_slot(state, module_id);
    ps3_diagnostic_module_status status = PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE;
    if (slot) {
        ps3_diagnostic_module_result result = {
            PS3_DIAGNOSTIC_MODULE_OK, module_id,
            slot->admission_count, slot->admission_count, state->total_admissions
        };
        *snapshot = result;
        status = PS3_DIAGNOSTIC_MODULE_OK;
    }
    state->guard.unlock(state->guard.context);
    return status;
}
