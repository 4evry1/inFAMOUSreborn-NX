/* Native RSX front end. Contract: gcm_rsx_frontend.h. */
#include "gcm_rsx_frontend.h"

#include <string.h>

static int allow_is_sorted_unique(const gcm_fe_allow *allow, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        const gcm_fe_allow *prev = &allow[i - 1];
        const gcm_fe_allow *cur = &allow[i];
        if (cur->cls < prev->cls) return 0;
        if (cur->cls == prev->cls && cur->method <= prev->method) return 0;
    }
    return 1;
}

gcm_fe_status gcm_rsx_frontend_init(gcm_rsx_frontend *fe, const gcm_fe_ports *ports,
                                    const gcm_fe_allow *allow, uint32_t allow_count)
{
    if (!fe || !ports || !ports->semaphore_read || !ports->publish_reference)
        return GCM_FE_INVALID_ARGUMENT;
    if (allow_count && !allow) return GCM_FE_INVALID_ARGUMENT;
    if (allow_count && !allow_is_sorted_unique(allow, allow_count))
        return GCM_FE_INVALID_ARGUMENT;
    memset(fe, 0, sizeof *fe);
    fe->ports = *ports;
    fe->allow = allow;
    fe->allow_count = allow_count;
    return GCM_FE_OK;
}

static gcm_fe_class class_of_handle(uint32_t handle)
{
    switch (handle) {
    case 0x31337000u: return GCM_FE_CLASS_NV4097;
    case 0x31337303u: return GCM_FE_CLASS_NV0039;
    case 0x313371C3u: return GCM_FE_CLASS_NV3062;
    case 0x31337A73u: return GCM_FE_CLASS_NV309E;
    case 0x31337808u: return GCM_FE_CLASS_NV308A;
    case 0x3137AF00u: return GCM_FE_CLASS_NV3089;
    default: return GCM_FE_CLASS_UNKNOWN_OBJECT;
    }
}

static const gcm_fe_allow *find_allow(const gcm_rsx_frontend *fe, gcm_fe_class cls, uint32_t method)
{
    uint32_t lo = 0;
    uint32_t hi = fe->allow_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const gcm_fe_allow *e = &fe->allow[mid];
        int before = ((uint32_t)e->cls < (uint32_t)cls) ||
                     ((uint32_t)e->cls == (uint32_t)cls && (uint32_t)e->method < method);
        if (before) lo = mid + 1;
        else hi = mid;
    }
    if (lo < fe->allow_count && (uint32_t)fe->allow[lo].cls == (uint32_t)cls &&
        (uint32_t)fe->allow[lo].method == method)
        return &fe->allow[lo];
    return NULL;
}

/* Latch the first refusal. A port result of 2 means the port failed after a possible
 * write, so the word's effect cannot be vouched for. */
static int refuse(gcm_rsx_frontend *fe, gcm_fe_status status, uint32_t subchannel,
                  uint32_t method, uint32_t data, uint32_t io_offset, int port_result,
                  rsx_fifo_effect *effect)
{
    if (!fe->failed) {
        fe->failed = 1;
        fe->failure.status = status;
        fe->failure.subchannel = subchannel;
        fe->failure.method = method;
        fe->failure.data = data;
        fe->failure.io_offset = io_offset;
        fe->failure.port_result = port_result;
    }
    if (effect) *effect = (port_result == 2) ? RSX_FIFO_EFFECT_UNKNOWN : RSX_FIFO_EFFECT_NONE;
    return (int)status;
}

static int accept(gcm_rsx_frontend *fe)
{
    fe->accepted_methods++;
    return 0;
}

static int channel_method(gcm_rsx_frontend *fe, uint32_t subchannel, uint32_t method,
                          uint32_t data, uint32_t io_offset, rsx_fifo_effect *effect)
{
    switch (method) {
    case 0x0050u: {   /* SET_REFERENCE */
        int rc = fe->ports.publish_reference(fe->ports.user, data);
        if (rc == 0) {
            fe->references_published++;
            fe->last_reference = data;
            return accept(fe);
        }
        if (rc == 1)
            return refuse(fe, GCM_FE_PORT_REFUSED, subchannel, method, data, io_offset, 1, effect);
        return refuse(fe, GCM_FE_PORT_FAILED, subchannel, method, data, io_offset, 2, effect);
    }
    case 0x0060u: {   /* SET_CONTEXT_DMA_SEMAPHORE: channel state */
        fe->semaphore_context = data;
        fe->semaphore_context_valid = 1;
        return accept(fe);
    }
    case 0x0064u: {   /* SEMAPHORE_OFFSET: channel state */
        fe->semaphore_offset = data;
        fe->semaphore_offset_valid = 1;
        return accept(fe);
    }
    case 0x0068u: {   /* SEMAPHORE_ACQUIRE */
        if (!fe->semaphore_context_valid || !fe->semaphore_offset_valid)
            return refuse(fe, GCM_FE_SEMAPHORE_STATE_MISSING, subchannel, method, data,
                          io_offset, 0, effect);
        uint32_t value = 0;
        int rc = fe->ports.semaphore_read(fe->ports.user, fe->semaphore_context,
                                          fe->semaphore_offset, &value);
        if (rc == 1)
            return refuse(fe, GCM_FE_SEMAPHORE_CONTEXT_UNKNOWN, subchannel, method, data,
                          io_offset, 1, effect);
        if (rc != 0)
            return refuse(fe, GCM_FE_PORT_FAILED, subchannel, method, data, io_offset, 2, effect);
        /* The synchronous consumer never waits: a mismatched value is a refusal. */
        if (value != data)
            return refuse(fe, GCM_FE_ACQUIRE_UNSATISFIED, subchannel, method, data, io_offset,
                          0, effect);
        fe->acquires_satisfied++;
        return accept(fe);
    }
    default:
        return refuse(fe, GCM_FE_UNSUPPORTED_CHANNEL_METHOD, subchannel, method, data,
                      io_offset, 0, effect);
    }
}

static int zero_dimension(uint32_t v)
{
    return (v & 0xFFFFu) == 0 || (v >> 16) == 0;
}

static int image_in_supported(const gcm_rsx_frontend *fe)
{
    uint32_t v = 0;
    if (gcm_rsx_frontend_shadow_get(fe, GCM_FE_CLASS_NV3089, 0x030Cu, &v) && zero_dimension(v))
        return 1;
    if (gcm_rsx_frontend_shadow_get(fe, GCM_FE_CLASS_NV3089, 0x0314u, &v) && zero_dimension(v))
        return 1;
    return 0;
}

static int class_method(gcm_rsx_frontend *fe, uint32_t subchannel, uint32_t method,
                        uint32_t data, uint32_t io_offset, rsx_fifo_effect *effect)
{
    gcm_fe_class cls = fe->sub[subchannel].cls;
    if (cls == GCM_FE_CLASS_NONE)
        return refuse(fe, GCM_FE_UNBOUND_SUBCHANNEL, subchannel, method, data, io_offset, 0,
                      effect);
    if (cls == GCM_FE_CLASS_UNKNOWN_OBJECT)
        return refuse(fe, GCM_FE_UNKNOWN_OBJECT_METHOD, subchannel, method, data, io_offset, 0,
                      effect);

    if (cls == GCM_FE_CLASS_NV4097 && method == 0x0100u) {          /* NO_OPERATION */
        fe->noop_actions++;
        return accept(fe);
    }
    if (cls == GCM_FE_CLASS_NV4097 && method == 0x1D94u) {          /* CLEAR_SURFACE */
        if ((data & 0xF3u) != 0)
            return refuse(fe, GCM_FE_ACTION_UNSUPPORTED, subchannel, method, data, io_offset, 0,
                          effect);
        fe->noop_actions++;
        return accept(fe);
    }
    if (cls == GCM_FE_CLASS_NV3089 && method == 0x040Cu) {          /* IMAGE_IN */
        if (!image_in_supported(fe))
            return refuse(fe, GCM_FE_ACTION_UNSUPPORTED, subchannel, method, data, io_offset, 0,
                          effect);
        fe->noop_actions++;
        return accept(fe);
    }
    if (method > 0x1FFCu || !find_allow(fe, cls, method))
        return refuse(fe, GCM_FE_NOT_ALLOWLISTED, subchannel, method, data, io_offset, 0,
                      effect);
    {
        uint32_t slot = method >> 2;
        fe->value[cls][slot] = data;
        fe->written[cls][slot] = 1;
    }
    fe->shadow_writes++;
    return accept(fe);
}

int gcm_rsx_frontend_sink(void *user, uint32_t subchannel, uint32_t method, uint32_t data,
                          uint32_t io_offset, rsx_fifo_effect *effect)
{
    gcm_rsx_frontend *fe = user;
    if (!fe) return (int)GCM_FE_INVALID_ARGUMENT;
    if (fe->failed) {
        if (effect) *effect = RSX_FIFO_EFFECT_NONE;
        return (int)GCM_FE_LATCHED;
    }
    if (subchannel >= GCM_FE_SUBCHANNEL_COUNT)
        return refuse(fe, GCM_FE_UNBOUND_SUBCHANNEL, subchannel, method, data, io_offset, 0,
                      effect);
    if (method == 0) {   /* SET_OBJECT: rebind only; channel semaphore state is unchanged */
        gcm_fe_subchannel *s = &fe->sub[subchannel];
        s->handle = data;
        s->cls = class_of_handle(data);
        return accept(fe);
    }
    if (method < 0x100u)
        return channel_method(fe, subchannel, method, data, io_offset, effect);
    return class_method(fe, subchannel, method, data, io_offset, effect);
}

const gcm_fe_failure *gcm_rsx_frontend_failure(const gcm_rsx_frontend *fe)
{
    if (!fe || !fe->failed) return NULL;
    return &fe->failure;
}

int gcm_rsx_frontend_failed(const gcm_rsx_frontend *fe)
{
    return fe && fe->failed;
}

int gcm_rsx_frontend_shadow_get(const gcm_rsx_frontend *fe, gcm_fe_class cls, uint32_t method,
                                uint32_t *value_out)
{
    if (!fe || !value_out) return 0;
    if ((uint32_t)cls >= (uint32_t)GCM_FE_CLASS_COUNT || method > 0x1FFCu) return 0;
    if (!fe->written[cls][method >> 2]) return 0;
    *value_out = fe->value[cls][method >> 2];
    return 1;
}

uint32_t gcm_rsx_frontend_bound_handle(const gcm_rsx_frontend *fe, uint32_t subchannel)
{
    if (!fe || subchannel >= GCM_FE_SUBCHANNEL_COUNT) return 0;
    return fe->sub[subchannel].handle;
}

const char *gcm_fe_status_name(gcm_fe_status status)
{
    switch (status) {
    case GCM_FE_OK: return "OK";
    case GCM_FE_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case GCM_FE_UNBOUND_SUBCHANNEL: return "UNBOUND_SUBCHANNEL";
    case GCM_FE_UNKNOWN_OBJECT_METHOD: return "UNKNOWN_OBJECT_METHOD";
    case GCM_FE_UNSUPPORTED_CHANNEL_METHOD: return "UNSUPPORTED_CHANNEL_METHOD";
    case GCM_FE_NOT_ALLOWLISTED: return "NOT_ALLOWLISTED";
    case GCM_FE_ACTION_UNSUPPORTED: return "ACTION_UNSUPPORTED";
    case GCM_FE_SEMAPHORE_STATE_MISSING: return "SEMAPHORE_STATE_MISSING";
    case GCM_FE_SEMAPHORE_CONTEXT_UNKNOWN: return "SEMAPHORE_CONTEXT_UNKNOWN";
    case GCM_FE_ACQUIRE_UNSATISFIED: return "ACQUIRE_UNSATISFIED";
    case GCM_FE_PORT_REFUSED: return "PORT_REFUSED";
    case GCM_FE_PORT_FAILED: return "PORT_FAILED";
    case GCM_FE_LATCHED: return "LATCHED";
    }
    return "UNKNOWN";
}
