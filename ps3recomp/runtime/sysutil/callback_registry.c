#include "callback_registry.h"

ps3_sysutil_callback_status ps3_sysutil_callback_init(
    ps3_sysutil_callback_registry *registry, ps3_instance_guard guard)
{
    if (!registry || registry->initialized || !guard.lock || !guard.unlock)
        return PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT;

    for (unsigned slot = 0; slot < PS3_SYSUTIL_CALLBACK_SLOT_COUNT; ++slot) {
        registry->pairs[slot].callback_descriptor_address = 0;
        registry->pairs[slot].userdata_address = 0;
    }
    registry->guard = guard;
    registry->initialized = 1;
    return PS3_SYSUTIL_CALLBACK_OK;
}

ps3_sysutil_callback_result ps3_sysutil_callback_register(
    ps3_sysutil_callback_registry *registry, uint32_t slot,
    ps3_sysutil_callback_pair pair)
{
    ps3_sysutil_callback_result result = {
        PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT, slot, {0, 0}, {0, 0}
    };
    if (!registry)
        return result;
    if (!registry->initialized) {
        result.status = PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED;
        return result;
    }
    if (slot >= PS3_SYSUTIL_CALLBACK_SLOT_COUNT) {
        result.status = PS3_SYSUTIL_CALLBACK_INVALID_SLOT;
        return result;
    }

    registry->guard.lock(registry->guard.context);
    result.previous = registry->pairs[slot];
    registry->pairs[slot] = pair;
    result.current = pair;
    result.status = PS3_SYSUTIL_CALLBACK_OK;
    registry->guard.unlock(registry->guard.context);
    return result;
}

ps3_sysutil_callback_status ps3_sysutil_callback_read(
    ps3_sysutil_callback_registry *registry, uint32_t slot,
    ps3_sysutil_callback_pair *snapshot)
{
    if (!registry || !snapshot)
        return PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT;
    if (!registry->initialized)
        return PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED;
    if (slot >= PS3_SYSUTIL_CALLBACK_SLOT_COUNT)
        return PS3_SYSUTIL_CALLBACK_INVALID_SLOT;

    registry->guard.lock(registry->guard.context);
    *snapshot = registry->pairs[slot];
    registry->guard.unlock(registry->guard.context);
    return PS3_SYSUTIL_CALLBACK_OK;
}
