#include "spu_resource_policy.h"
#include <stddef.h>
#include <string.h>

static int capability_valid(const ps3_spu_resource_policy *policy)
{
    return policy && policy->guard.context && policy->guard.lock && policy->guard.unlock;
}
static int limits_valid(ps3_spu_policy_limits limits)
{
    return limits.raw_limit <= PS3_SPU_RAW_LIMIT &&
        limits.managed_limit == PS3_SPU_COMPATIBILITY_UNITS - limits.raw_limit;
}
ps3_spu_policy_status ps3_spu_resource_policy_init(ps3_spu_resource_policy *policy, ps3_instance_guard guard)
{
    if (!policy || !guard.context || !guard.lock || !guard.unlock) return PS3_SPU_POLICY_INVALID_STATE;
    /* Construction is exclusively owned by composition, on zeroed storage, once. */
    if (policy->guard.lock) return PS3_SPU_POLICY_INVALID_STATE;
    memset(policy, 0, sizeof *policy);
    policy->guard = guard;
    policy->limits = (ps3_spu_policy_limits){PS3_SPU_COMPATIBILITY_UNITS, 0};
    return PS3_SPU_POLICY_OK;
}
static ps3_spu_policy_result access_policy(ps3_spu_resource_policy *policy, int configure, uint32_t raw)
{
    ps3_spu_policy_result result = {PS3_SPU_POLICY_INVALID_STATE, {0, 0}, {0, 0}, 0};
    if (!capability_valid(policy)) return result;
    policy->guard.lock(policy->guard.context);
    if (limits_valid(policy->limits)) {
        result.previous = policy->limits;
        result.status = configure && raw > PS3_SPU_RAW_LIMIT ? PS3_SPU_POLICY_INVALID_RAW : PS3_SPU_POLICY_OK;
        if (configure && result.status == PS3_SPU_POLICY_OK) {   /* pinned check_busy: live groups must still fit */
            for (size_t i = 0; i < PS3_SPU_GROUP_CAP; ++i)
                if (policy->groups[i].active && policy->groups[i].num > PS3_SPU_COMPATIBILITY_UNITS - raw) result.status = PS3_SPU_POLICY_BUSY;
        }
        if (configure && result.status == PS3_SPU_POLICY_OK)
            policy->limits = (ps3_spu_policy_limits){PS3_SPU_COMPATIBILITY_UNITS - raw, raw};
        result.current = policy->limits;
        result.has_snapshot = 1;
    }
    policy->guard.unlock(policy->guard.context);
    return result;
}
ps3_spu_policy_result ps3_spu_resource_policy_configure(ps3_spu_resource_policy *policy, uint32_t usable, uint32_t raw)
{
    (void)usable; /* Pinned sys_spu_initialize compatibility policy ignores it. */
    return access_policy(policy, 1, raw);
}
ps3_spu_policy_result ps3_spu_resource_policy_snapshot(ps3_spu_resource_policy *policy)
{
    return access_policy(policy, 0, 0);
}

int ps3_spu_resource_policy_attach(ps3_spu_resource_policy *p, ps3_user_memory *container, ps3_lv2_objects *lv2, ps3_notify_port port)
{
    if (!capability_valid(p) || !container || !lv2 || !port.wait || !port.wake_all || p->container ||
        lv2->guard.context == p->guard.context) return 0;   /* policy->lv2 nesting: a shared guard would self-deadlock */
    p->container = container;
    p->lv2 = lv2;
    p->port = port;
    return 1;
}
static uint32_t unlock_with(ps3_spu_resource_policy *p, uint32_t rc) { p->guard.unlock(p->guard.context); return rc; }
static ps3_spu_group_record *find_group(ps3_spu_resource_policy *p, uint32_t id)
{
    for (size_t i = 0; i < PS3_SPU_GROUP_CAP; ++i) if (p->groups[i].active && p->groups[i].id == id) return &p->groups[i];
    return NULL;
}
static int queue_exists(ps3_spu_resource_policy *p, uint32_t queue)
{
    ps3_lv2_queue snapshot;
    return ps3_lv2_queue_snapshot(p->lv2, queue, &snapshot) == PS3_LV2_OK;
}
uint32_t ps3_spu_group_create(ps3_spu_resource_policy *p, uint32_t *id, uint32_t num, int32_t prio, int32_t type, uint32_t name_len)
{
    if (!capability_valid(p) || !p->container) return PS3_LV2_EINVAL;
    if (!id) return PS3_LV2_EFAULT;
    if (name_len > 0x80 || !num) return PS3_LV2_EINVAL;
    if (num > PS3_SPU_COMPATIBILITY_UNITS || prio < 16 || prio > 255) return PS3_LV2_EINVAL;
    if (type != 0) return PS3_LV2_UNSUPPORTED;            /* legal lv2 type not owned natively: stop, not a guest error */
    p->guard.lock(p->guard.context);
    ps3_user_memory_charge_token charge;
    if (ps3_user_memory_charge(p->container, (uint64_t)PS3_SPU_LS_CHARGE * num, &charge) != PS3_USER_MEMORY_OK)
        return unlock_with(p, PS3_SPU_ENOMEM);
    uint32_t controllable = num;                          /* check_valid: max rule, not a sum */
    for (size_t i = 0; i < PS3_SPU_GROUP_CAP; ++i)
        if (p->groups[i].active && p->groups[i].num > controllable) controllable = p->groups[i].num;
    if (controllable > p->limits.managed_limit) {
        ps3_user_memory_uncharge(p->container, &charge);
        return unlock_with(p, PS3_LV2_EINVAL);
    }
    /* check_busy: only raw/exclusive groups can be busy; native creates neither [POL]. */
    ps3_spu_group_record *g = NULL;
    for (size_t i = 0; i < PS3_SPU_GROUP_CAP && !g; ++i) if (!p->groups[i].active) g = &p->groups[i];
    if (!g || p->serial >= 255) {
        ps3_user_memory_uncharge(p->container, &charge);
        return unlock_with(p, PS3_LV2_EAGAIN);
    }
    memset(g, 0, sizeof *g);
    g->id = PS3_SPU_GROUP_BASE + PS3_SPU_GROUP_STEP * p->serial++;
    g->num = num; g->prio = prio; g->type = type; g->state = PS3_SPU_GROUP_NOT_INITIALIZED; g->charge = charge; g->active = 1;
    *id = g->id;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_thread_initialize(ps3_spu_resource_policy *p, uint32_t *tid, uint32_t group, uint32_t spu_num,
                                   uint32_t image_kind, uint64_t arg1, uint64_t arg2, uint32_t option, uint32_t name_len)
{
    if (!capability_valid(p)) return PS3_LV2_EINVAL;
    if (spu_num >= PS3_SPU_GROUP_THREADS) return PS3_LV2_EINVAL;
    if (!tid) return PS3_LV2_EFAULT;
    if (name_len > 0x80) return PS3_LV2_EINVAL;
    if (option & ~3u) return PS3_LV2_EINVAL;
    if (image_kind != PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->state != PS3_SPU_GROUP_NOT_INITIALIZED) return unlock_with(p, PS3_LV2_EBUSY);
    if (spu_num >= g->num || g->threads[spu_num].active) return unlock_with(p, PS3_LV2_EBUSY);
    ps3_spu_thread_record *t = &g->threads[spu_num];
    memset(t, 0, sizeof *t);
    t->id = (g->init_count << 24) | (g->id & 0xFFFFFFu);
    t->image_kind = image_kind; t->option = option; t->arg1 = arg1; t->arg2 = arg2; t->active = 1;
    *tid = t->id;
    if (++g->init_count == g->num) g->state = PS3_SPU_GROUP_INITIALIZED;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_connect_event_all_threads(ps3_spu_resource_policy *p, uint32_t group, uint32_t queue, uint64_t req, uint8_t *spup)
{
    if (!capability_valid(p) || !spup) return PS3_LV2_EFAULT;
    if (!req) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g || !queue_exists(p, queue)) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->state < PS3_SPU_GROUP_INITIALIZED) return unlock_with(p, PS3_SPU_ESTAT);
    for (unsigned port = 0; port < PS3_SPU_PORTS; ++port) {
        if (!(req & (UINT64_C(1) << port))) continue;
        int taken = 0;
        /* L4: a binding whose queue was destroyed no longer occupies the port (pinned lv2_obj::check()).
         * Liveness comes from the lv2 already attached (policy -> lv2); ids are never reused, so a dead id
         * can never alias a new queue. */
        for (uint32_t n = 0; n < g->num; ++n)
            taken |= g->threads[n].port_queue[port] != 0 && queue_exists(p, g->threads[n].port_queue[port]);
        if (taken) continue;
        for (uint32_t n = 0; n < g->num; ++n) g->threads[n].port_queue[port] = queue;
        *spup = (uint8_t)port;
        return unlock_with(p, PS3_LV2_OK);
    }
    return unlock_with(p, PS3_LV2_EISCONN);
}
uint32_t ps3_spu_group_disconnect_event_all_threads(ps3_spu_resource_policy *p, uint32_t group, uint8_t spup)
{
    if (!capability_valid(p)) return PS3_LV2_EINVAL;
    if (spup >= PS3_SPU_PORTS) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    for (uint32_t n = 0; n < g->num; ++n) g->threads[n].port_queue[spup] = 0;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_disconnect_event_all_threads_exact(ps3_spu_resource_policy *p, uint32_t group, uint8_t spup, uint32_t queue)
{
    if (!capability_valid(p) || spup >= PS3_SPU_PORTS || !queue) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    for (uint32_t n = 0; n < g->num; ++n)   /* full STORED-id match first: a single foreign/clear/different-id thread refuses with nothing written; a dead binding still storing `queue` matches */
        if (g->threads[n].port_queue[spup] != queue) return unlock_with(p, PS3_LV2_ESRCH);
    for (uint32_t n = 0; n < g->num; ++n) g->threads[n].port_queue[spup] = 0;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_connect_event(ps3_spu_resource_policy *p, uint32_t group, uint32_t queue, uint32_t event_type)
{
    if (!capability_valid(p)) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (event_type == PS3_SPU_EVENT_RUN || event_type == PS3_SPU_EVENT_SYSTEM_MODULE) return unlock_with(p, PS3_LV2_UNSUPPORTED);
    if (event_type != PS3_SPU_EVENT_EXCEPTION) return unlock_with(p, PS3_LV2_EINVAL);
    if (g->exception_queue && queue_exists(p, g->exception_queue)) return unlock_with(p, PS3_LV2_EBUSY);   /* L4: dead queue frees the slot */
    if (!queue_exists(p, queue)) return unlock_with(p, PS3_LV2_ESRCH);   /* pinned: after EBUSY */
    g->exception_queue = queue;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_mark_running(ps3_spu_resource_policy *p, uint32_t group, const void *executor)
{
    if (!capability_valid(p) || !executor) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->state != PS3_SPU_GROUP_INITIALIZED || g->executor) return unlock_with(p, PS3_SPU_ESTAT);
    g->state = PS3_SPU_GROUP_RUNNING;
    g->executor = executor;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_mark_stopped(ps3_spu_resource_policy *p, uint32_t group, const void *executor)
{
    if (!capability_valid(p) || !executor) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->executor != executor) return unlock_with(p, PS3_LV2_EINVAL);
    g->state = PS3_SPU_GROUP_STOPPED;
    __atomic_store_n(&g->terminal, 1, __ATOMIC_SEQ_CST);
    p->port.wake_all(p->port.context);
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_release_executor(ps3_spu_resource_policy *p, uint32_t group, const void *executor)
{
    if (!capability_valid(p) || !executor) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->executor != executor) return unlock_with(p, PS3_LV2_EINVAL);
    if (g->state != PS3_SPU_GROUP_STOPPED) return unlock_with(p, PS3_LV2_EBUSY);
    g->executor = NULL;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_join(ps3_spu_resource_policy *p, uint32_t group, ps3_notify_cancel *cancel, uint64_t *blocked_word)
{
    if (!capability_valid(p) || !p->container) return PS3_LV2_EINVAL;
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->state < PS3_SPU_GROUP_INITIALIZED) return unlock_with(p, PS3_SPU_ESTAT);
    if (g->joiner) return unlock_with(p, PS3_LV2_EBUSY);
    g->joiner = 1;                                   /* destroy refuses until this joiner has left */
    const size_t slot = (size_t)(g - p->groups);
    const uint32_t id = g->id;
    uint64_t *terminal = &g->terminal;
    if (blocked_word) { __atomic_store_n(blocked_word, 1, __ATOMIC_SEQ_CST); p->port.wake_all(p->port.context); }
    p->guard.unlock(p->guard.context);
    uint32_t rc = PS3_LV2_OK;
    while (!__atomic_load_n(terminal, __ATOMIC_SEQ_CST)) {
        if (ps3_notify_cancel_is_requested(cancel)) { rc = PS3_LV2_CANCELLED; break; }
        if (p->port.wait(p->port.context, terminal, 0, cancel ? &cancel->requested : NULL, UINT64_MAX) == PS3_NOTIFY_FAILURE) {
            rc = PS3_LV2_PROVIDER_FAILURE; break;
        }
    }
    p->guard.lock(p->guard.context);
    if (!p->groups[slot].active || p->groups[slot].id != id) rc = PS3_LV2_ESRCH;   /* invariant: unreachable while joiner pins the slot */
    else p->groups[slot].joiner = 0;
    if (blocked_word) __atomic_store_n(blocked_word, 0, __ATOMIC_SEQ_CST);
    return unlock_with(p, rc);
}
uint32_t ps3_spu_group_destroy(ps3_spu_resource_policy *p, uint32_t group)
{
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g) return unlock_with(p, PS3_LV2_ESRCH);
    if (g->state == PS3_SPU_GROUP_RUNNING || g->executor || g->joiner) return unlock_with(p, PS3_LV2_EBUSY);
    ps3_user_memory_uncharge(p->container, &g->charge);
    g->active = 0;
    return unlock_with(p, PS3_LV2_OK);
}
uint32_t ps3_spu_group_snapshot(ps3_spu_resource_policy *p, uint32_t group, ps3_spu_group_record *out)
{
    p->guard.lock(p->guard.context);
    ps3_spu_group_record *g = find_group(p, group);
    if (!g || !out) return unlock_with(p, PS3_LV2_ESRCH);
    *out = *g;
    return unlock_with(p, PS3_LV2_OK);
}
