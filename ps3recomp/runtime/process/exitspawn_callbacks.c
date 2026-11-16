#include "exitspawn_callbacks.h"

ps3_exitspawn_status ps3_exitspawn_init(ps3_exitspawn_registry *registry,
    ps3_instance_guard guard)
{
    if (!registry || registry->initialized || !guard.lock || !guard.unlock)
        return PS3_EXITSPAWN_INVALID_ARGUMENT;
    *registry=(ps3_exitspawn_registry){.guard=guard,.initialized=1};
    return PS3_EXITSPAWN_OK;
}

ps3_exitspawn_result ps3_exitspawn_register(ps3_exitspawn_registry *registry,
    ps3_exitspawn_kind kind, uint32_t descriptor_address)
{
    ps3_exitspawn_result result={.status=PS3_EXITSPAWN_INVALID_ARGUMENT};
    if (!registry || (kind!=PS3_EXITSPAWN_ATEXITSPAWN && kind!=PS3_EXITSPAWN_AT_EXITSPAWN))
        return result;
    if (!registry->initialized) { result.status=PS3_EXITSPAWN_NOT_INITIALIZED; return result; }
    registry->guard.lock(registry->guard.context);
    uint32_t *slot=kind==PS3_EXITSPAWN_ATEXITSPAWN?
        &registry->slots.atexitspawn:&registry->slots.at_Exitspawn;
    if (*slot) result.decision=PS3_EXITSPAWN_RETAINED;
    else if (descriptor_address) {
        *slot=descriptor_address;
        result.decision=PS3_EXITSPAWN_INSERTED;
    } else result.decision=PS3_EXITSPAWN_NULL;
    result.status=PS3_EXITSPAWN_OK;
    result.snapshot=registry->slots;
    registry->guard.unlock(registry->guard.context);
    return result;
}

ps3_exitspawn_status ps3_exitspawn_read(ps3_exitspawn_registry *registry,
    ps3_exitspawn_snapshot *output)
{
    if (!registry || !output) return PS3_EXITSPAWN_INVALID_ARGUMENT;
    if (!registry->initialized) return PS3_EXITSPAWN_NOT_INITIALIZED;
    registry->guard.lock(registry->guard.context);
    *output=registry->slots;
    registry->guard.unlock(registry->guard.context);
    return PS3_EXITSPAWN_OK;
}
