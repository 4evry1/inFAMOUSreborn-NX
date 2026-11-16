#include "disc_change_callbacks.h"

ps3_disc_change_status ps3_disc_change_init(ps3_disc_change_registry *registry,
        ps3_instance_guard guard,ps3_disc_change_policy policy) {
    if(!registry) return PS3_DISC_CHANGE_INVALID_ARGUMENT;
    if(registry->initialized) return PS3_DISC_CHANGE_ALREADY_INITIALIZED;
    if(!guard.lock || !guard.unlock) return PS3_DISC_CHANGE_INVALID_ARGUMENT;
    if(policy!=PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE) return PS3_DISC_CHANGE_UNSUPPORTED_POLICY;
    registry->guard=guard;
    registry->policy=policy;
    registry->current=(ps3_disc_change_pair){0,0};
    registry->initialized=1;
    return PS3_DISC_CHANGE_OK;
}

ps3_disc_change_result ps3_disc_change_register(ps3_disc_change_registry *registry,ps3_disc_change_pair pair) {
    ps3_disc_change_result result={.status=PS3_DISC_CHANGE_INVALID_ARGUMENT};
    if(!registry) return result;
    if(!registry->initialized) { result.status=PS3_DISC_CHANGE_NOT_INITIALIZED;return result; }
    if(registry->policy!=PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE) {
        result.status=PS3_DISC_CHANGE_UNSUPPORTED_POLICY;return result;
    }
    registry->guard.lock(registry->guard.context);
    result.previous=registry->current;
    registry->current.insert_descriptor=pair.insert_descriptor;
    registry->current.eject_descriptor=pair.eject_descriptor;
    result.current=registry->current;
    result.committed=1;
    result.status=PS3_DISC_CHANGE_OK;
    registry->guard.unlock(registry->guard.context);
    return result;
}

ps3_disc_change_status ps3_disc_change_read(ps3_disc_change_registry *registry,ps3_disc_change_pair *snapshot) {
    if(!registry || !snapshot) return PS3_DISC_CHANGE_INVALID_ARGUMENT;
    if(!registry->initialized) return PS3_DISC_CHANGE_NOT_INITIALIZED;
    if(registry->policy!=PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE) return PS3_DISC_CHANGE_UNSUPPORTED_POLICY;
    registry->guard.lock(registry->guard.context);
    const ps3_disc_change_pair captured=registry->current;
    registry->guard.unlock(registry->guard.context);
    *snapshot=captured;
    return PS3_DISC_CHANGE_OK;
}
