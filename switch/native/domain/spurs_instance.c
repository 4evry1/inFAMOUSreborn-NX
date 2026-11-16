#include "spurs_instance.h"
#include <string.h>

/* CellSpurs v1 field offsets (CONTRACT-CLOSURE s2). */
enum {
    F_WKL_FLAG = 0x06C, F_FLAGS1 = 0x074, F_NSPUS = 0x076, F_WKL_FLAG_RECEIVER = 0x077, F_WKL_ENABLED = 0x0B0,
    F_SYS_SRV_PREEMPT = 0x0C0, F_SPU_PORT = 0x0C9, F_SYS_SRV_TRACE = 0x0CC, F_WKL_F1 = 0x120, F_TRACE_BUFFER = 0x900,
    F_SEM_PRV = 0x980, F_WKL_INFO_SYS_SRV = 0xD00, F_PPU0 = 0xD20, F_SPUS = 0xD34, F_EVENT_QUEUE = 0xD5C,
    F_EVENT_PORT = 0xD60, F_FLAGS_0xD64 = 0xD64, F_ENABLE_EH = 0xD68, F_SPU_IMG = 0xD70, F_FLAGS = 0xD80,
    F_SPU_PRIORITY = 0xD84, F_PPU_PRIORITY = 0xD88, F_PREFIX = 0xD8C, F_PREFIX_SIZE = 0xD9B, F_REVISION = 0xDA0,
    F_SDK = 0xDA4, F_LWMUTEX = 0xDB0, F_LWCOND = 0xDC8, F_PORT_MUX = 0xF00,
    F_SPU_GROUP = 0xD30, F_EXCEPTION = 0xD6C, F_SPU_PORT_BITS = 0xDA8,
    F_PPU1 = 0xD28, F_TRACE_SIZE = 0x948,   /* cellSpursGetInfo sources (pinned RPCS3 63996a47 cellSpurs.h CellSpurs) */
    A_REVISION = 0, A_SDK = 4, A_NSPUS = 8, A_SPU_PRIORITY = 12, A_PPU_PRIORITY = 16, A_EXIT = 20, A_PREFIX = 0x15,
    A_PREFIX_SIZE = 0x24, A_FLAGS = 0x28,
    KERNEL1_ENTRY = 0x818, HELPER_STACK = 0x8000, HANDLER_STACK = 0x4000, HELPER_PRIORITY = 249,
    QUEUE_SIZE = 0x2A, SPUP_MASK_SHIFT = 16,
    ATTACH_SDK_PORT_LIMIT = 0x180000, ATTACH_MAX_PORT = 0x3F, ATTACH_SDK_MAX_PORT = 0xF, ATTACH_USER_PORTS = 16
};
static const ps3_lv2_event helper_stop_event = {0, 0, 1, 0};   /* port send (0,1,0): stop_event_helper */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static const ps3_spurs_runtime_config *cfg_of(const ps3_spurs_instance *in) { return &in->runtime->config; }

/* ---- guest writes (big-endian) ---- */
static int put(ps3_spurs_instance *in, uint32_t offset, const void *bytes, size_t length) {
    const ps3_spurs_runtime_config *c = cfg_of(in);
    const uint32_t ea = in->spurs_ea + offset;
    if (c->writer.write) return c->writer.write(c->writer.context, ea, bytes, length);
    return ps3_atomic_write(c->domain, ea, bytes, length) == PS3_GUEST_MEMORY_OK ? 0 : -1;
}
static int put8(ps3_spurs_instance *in, uint32_t offset, uint8_t v) { return put(in, offset, &v, 1); }
static int put32(ps3_spurs_instance *in, uint32_t offset, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return put(in, offset, b, 4);
}
static int put64(ps3_spurs_instance *in, uint32_t offset, uint64_t v) {
    const uint8_t b[8] = {(uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40), (uint8_t)(v >> 32),
                          (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return put(in, offset, b, 8);
}

/* ---- teardown / rollback: reverse creation order, idempotent and retryable ---- */
static unsigned unwind(ps3_spurs_instance *in) {
    ps3_spurs_runtime *rt = in->runtime;
    const ps3_spurs_runtime_config *c = &rt->config;
    unsigned stuck = 0;
    if (in->handler_live) {   /* wake its blocked join/receive, then a CONFIRMED join */
        ps3_notify_cancel_request(&in->handler_cancel, &c->notify);
        if (ps3_ppu_service_join(c->ppu, in->handler, NULL) == PS3_PPU_OK) in->handler_live = 0;
        else return in->unwinding_failed = 1;
    }
    if (in->kernel_initialized) {   /* worker joined before owners/LS are released */
        if (ps3_spurs_k1_cancel_and_join(&in->kernel) != 0) return in->unwinding_failed = 1;
        in->kernel_initialized = 0;
    }
    if (in->group_created) {
        ps3_spu_group_record g;
        if (ps3_spu_group_snapshot(c->policy, in->group, &g) == PS3_LV2_OK && g.executor == &in->kernel) {
            if (ps3_spu_group_mark_stopped(c->policy, in->group, &in->kernel) != PS3_LV2_OK ||
                ps3_spu_group_release_executor(c->policy, in->group, &in->kernel) != PS3_LV2_OK) stuck = 1;
        }
    }
    /* A slot is freed only after its owner is really released; the first failure stops here with owners_acquired
     * still counting that owner, so a retry (or the stuck derivation below) never forgets it (review M2). */
    for (unsigned i = in->owners_acquired; i-- > 0;) {
        if (ps3_atomic_owner_release(c->domain, &in->owners[i]) != PS3_GUEST_MEMORY_OK) { stuck = 1; break; }
        rt->ls_used[in->ls_slot[i]] = 0;
        in->ls[i] = NULL;
        in->owners_acquired = i;
    }
    if (in->helper_live) {
        ps3_notify_cancel_request(&in->helper_cancel, &c->notify);
        if (ps3_ppu_service_join(c->ppu, in->helper, NULL) == PS3_PPU_OK) in->helper_live = 0;
        else return in->unwinding_failed = 1;
    }
    /* Guest attach bindings are detached exactly (never the unconditional variant) once every executor is joined and BEFORE
     * the group can be destroyed; a failed detach keeps its record, blocks the group destroy and the pool reset. The borrowed
     * queues themselves belong to their owners and are never touched. */
    unsigned user_stuck = 0;
    for (unsigned port = 0; port < PS3_SPU_PORTS; ++port) {
        if (!((in->user_port_bits >> port) & 1)) continue;
        if (ps3_spu_group_disconnect_event_all_threads_exact(c->policy, in->group, (uint8_t)port, in->user_port_queue[port]) == PS3_LV2_OK) {
            in->user_port_bits &= ~(UINT64_C(1) << port);
            in->user_port_queue[port] = 0;
        } else user_stuck = 1;
    }
    if (in->port_connected && ps3_lv2_port_disconnect(c->lv2, in->port) == PS3_LV2_OK) in->port_connected = 0;
    if (in->port_created && !in->port_connected && ps3_lv2_port_destroy(c->lv2, in->port) == PS3_LV2_OK) in->port_created = 0;
    if (in->attached && ps3_spu_group_disconnect_event_all_threads(c->policy, in->group, (uint8_t)in->spup) == PS3_LV2_OK) in->attached = 0;
    if (in->queue_bound && ps3_lv2_queue_unbind_service(c->lv2, in->queue) == PS3_LV2_OK) in->queue_bound = 0;
    if (in->queue_created && !in->queue_bound && ps3_lv2_queue_destroy(c->lv2, in->queue) == PS3_LV2_OK) in->queue_created = 0;
    if (in->lwcond_created && c->sync.lwcond_destroy(c->sync.context, in->spurs_ea + F_LWCOND) == 0) in->lwcond_created = 0;
    if (in->lwmutex_created && !in->lwcond_created && c->sync.lwmutex_destroy(c->sync.context, in->spurs_ea + F_LWMUTEX) == 0) in->lwmutex_created = 0;
    if (in->group_created && !user_stuck && ps3_spu_group_destroy(c->policy, in->group) == PS3_LV2_OK) in->group_created = 0;
    while (in->semaphore_count > 0)
        if (ps3_lv2_semaphore_destroy(c->lv2, in->semaphores[in->semaphore_count - 1]) == PS3_LV2_OK) --in->semaphore_count;
        else { stuck = 1; break; }
    stuck |= in->port_created | in->port_connected | in->attached | in->queue_bound | in->queue_created | in->lwcond_created |
             in->lwmutex_created | in->group_created | (in->semaphore_count != 0) | (in->owners_acquired != 0) | user_stuck |
             (in->user_port_bits != 0);
    in->unwinding_failed = stuck;
    if (!stuck) {   /* record returns to the pool; services' storage is no longer referenced */
        const uint32_t index = in->index;
        ps3_spurs_runtime *owner = in->runtime;
        memset(in, 0, sizeof *in);
        in->runtime = owner;
        in->index = index;
    }
    return stuck;
}

static ps3_spurs_result result_of(const ps3_spurs_instance *in, ps3_spurs_outcome o, uint32_t rc, uint32_t reason, uint32_t step, uint32_t detail) {
    ps3_spurs_result r;
    memset(&r, 0, sizeof r);
    r.outcome = o; r.rc = rc; r.reason = reason; r.step = step; r.detail = detail; r.instance = in ? in->index : 0;
    return r;
}
static ps3_spurs_result finish_stop(ps3_spurs_instance *in, ps3_spurs_stop_reason reason, uint32_t step, uint32_t detail) {
    in->state = PS3_SPURS_INSTANCE_RETAINED;      /* terminal: nothing is rolled back or restored */
    return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, reason, step, detail);
}
static ps3_spurs_result finish_failed(ps3_spurs_instance *in, uint32_t rc, uint32_t step) {
    if (unwind(in)) { in->state = PS3_SPURS_INSTANCE_RETAINED; return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_ROLLBACK_FAILED, step, rc); }
    in->state = PS3_SPURS_INSTANCE_FREE;
    return result_of(in, PS3_SPURS_OUTCOME_FAILED, rc, 0, step, 0);
}
/* lv2 rc routing: unowned-but-legal / provider failures are stops, resource failures return the rc. */
static ps3_spurs_result route(ps3_spurs_instance *in, uint32_t rc, uint32_t step, uint32_t returned) {
    if (rc == PS3_LV2_UNSUPPORTED) return finish_stop(in, PS3_SPURS_STOP_UNSUPPORTED_LV2, step, rc);
    if (rc == PS3_LV2_PROVIDER_FAILURE) return finish_stop(in, PS3_SPURS_STOP_PROVIDER_FAILURE, step, rc);
    return finish_failed(in, returned, step);
}

/* ---- services ---- */
static void record_helper_stop(ps3_spurs_instance *in, ps3_spurs_stop_category category,
    uint32_t receive_status, const ps3_lv2_event *event) {
    ps3_spurs_helper_stop_capture *capture = &in->runtime->helper_stops[in->index];
    capture->spurs_ea = in->spurs_ea;
    capture->queue = in->queue;
    capture->category = category;
    capture->receive_status = receive_status;
    if (event) capture->event = *event;
    __atomic_store_n(&capture->ready, 1, __ATOMIC_RELEASE);
}
static void helper_body(ps3_ppu_service *self, void *argument) {
    ps3_spurs_instance *in = argument;
    for (;;) {
        ps3_lv2_event e;
        uint32_t rc = ps3_lv2_queue_receive(in->runtime->config.lv2, in->queue, &e, &in->helper_cancel, &self->activity);
        if (rc != PS3_LV2_OK) {
            if (rc != PS3_LV2_CANCELLED || !ps3_notify_cancel_is_requested(&in->helper_cancel))
                record_helper_stop(in, PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE, rc, NULL);
            ps3_ppu_service_exit(self, 2);
            return;
        }
        if (e.source == in->helper_stop_source && e.data1 == helper_stop_event.data1 &&
            e.data2 == helper_stop_event.data2 && e.data3 == helper_stop_event.data3) {
            ps3_ppu_service_exit(self, 0);
            return;
        }
        record_helper_stop(in, PS3_SPURS_STOP_CATEGORY_HELPER_EVENT, PS3_LV2_OK, &e);
        ps3_ppu_service_exit(self, 3);
        return;
    }
}
static void handler_body(ps3_ppu_service *self, void *argument) {
    ps3_spurs_instance *in = argument;
    const ps3_spurs_runtime_config *c = &in->runtime->config;
    ps3_spurs_k1_config k = {c->domain, c->notify, c->threads[in->index][PS3_SPURS_THREAD_KERNEL], in->spurs_ea, in->nspus,
                             in->owners, in->ls, NULL};
    if (ps3_spurs_k1_init(&in->kernel, &k) != PS3_SPURS_K1_OK) {
        __atomic_store_n(&in->handler_status, PS3_SPURS_HANDLER_START_FAILED, __ATOMIC_SEQ_CST);
        ps3_ppu_service_exit(self, 3);
        return;
    }
    in->kernel_initialized = 1;
    if (in->runtime->workload_services_configured &&
        ps3_spurs_k1_set_policy_execution(&in->kernel, &in->runtime->workload_execution[in->index]) != PS3_SPURS_K1_OK) {
        __atomic_store_n(&in->handler_status, PS3_SPURS_HANDLER_START_FAILED, __ATOMIC_SEQ_CST);
        ps3_ppu_service_exit(self, 3);
        return;
    }
    ps3_spurs_k1_stop stop;
    memset(&stop, 0, sizeof stop);
    ps3_spurs_k1_status st = ps3_spurs_k1_start(&in->kernel, &stop);   /* group_start: contexts run to their L0 wait */
    if (st == PS3_SPURS_K1_STOPPED) {
        in->kernel_stop = stop;
        __atomic_store_n(&in->handler_status, PS3_SPURS_HANDLER_KERNEL_STOPPED, __ATOMIC_SEQ_CST);
        ps3_ppu_service_exit(self, 4);
        return;
    }
    if (st != PS3_SPURS_K1_OK ||
        ps3_spu_group_mark_running(c->policy, in->group, &in->kernel) != PS3_LV2_OK) {
        __atomic_store_n(&in->handler_status, st == PS3_SPURS_K1_CANCELLED ? PS3_SPURS_HANDLER_KERNEL_CANCELLED : PS3_SPURS_HANDLER_START_FAILED, __ATOMIC_SEQ_CST);
        ps3_ppu_service_exit(self, 5);
        return;
    }
    __atomic_store_n(&in->handler_status, PS3_SPURS_HANDLER_BLOCKED_JOIN, __ATOMIC_SEQ_CST);
    uint32_t rc = ps3_spu_group_join(c->policy, in->group, &in->handler_cancel, &self->activity);   /* BLOCKED_JOIN */
    __atomic_store_n(&in->handler_status, rc == PS3_LV2_OK ? PS3_SPURS_HANDLER_JOINED : PS3_SPURS_HANDLER_JOIN_CANCELLED, __ATOMIC_SEQ_CST);
    ps3_ppu_service_exit(self, rc == PS3_LV2_OK ? 0 : 6);
}

int ps3_spurs_runtime_init(ps3_spurs_runtime *rt, const ps3_spurs_runtime_config *config) {
    if (!rt || !config || rt->initialized) return -1;
    if (!config->domain || !config->notify.wait || !config->notify.wake_all || !config->lv2 || !config->policy || !config->ppu ||
        !config->sync.lwmutex_create || !config->sync.lwmutex_destroy || !config->sync.lwcond_create || !config->sync.lwcond_destroy ||
        config->creator_priority == 0 || config->creator_priority > 3071) return -1;
    if (!ps3_ppu_threads_main_id(config->ppu)) return -1;   /* L5: the real main thread id must be adopted first */
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        for (unsigned r = 0; r < 3; ++r)
            if (!config->threads[i][r].start || !config->threads[i][r].join) return -1;
    for (unsigned i = 0; i < PS3_SPURS_LS_SLOTS; ++i) if (!config->local_stores[i]) return -1;
    memset(rt, 0, sizeof *rt);
    rt->config = *config;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) { rt->instances[i].runtime = rt; rt->instances[i].index = i; }
    rt->initialized = 1;
    return 0;
}

int ps3_spurs_runtime_set_workload_services(ps3_spurs_runtime *rt,
    const ps3_instance_guard guards[PS3_SPURS_MAX_INSTANCES],
    const ps3_spurs_policy_execution_port ports[PS3_SPURS_MAX_INSTANCES]) {
    if (!rt || !rt->initialized || !guards || !ports || rt->workload_services_configured) return -1;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) {
        if (rt->instances[i].state != PS3_SPURS_INSTANCE_FREE || !guards[i].lock || !guards[i].unlock ||
            guards[i].context == rt->config.domain->guard.context ||
            !ports[i].begin || !ports[i].step || !ports[i].resume || !ports[i].cancel || !ports[i].release) return -1;
    }
    memcpy(rt->workload_guards, guards, sizeof rt->workload_guards);
    memcpy(rt->workload_execution, ports, sizeof rt->workload_execution);
    rt->workload_services_configured = 1;
    return 0;
}
void ps3_spurs_runtime_register_workload(ps3_spurs_runtime *rt, const ps3_spurs_registration_request *request,
    ps3_atomic_owner borrowed_ppu_owner, unsigned max_attempts,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_registration_event *),
    ps3_spurs_registration_result *result) {
    if (!result) return;
    ps3_spurs_registration_request captured;
    if (request) captured = *request;
    memset(result, 0, sizeof *result);
    if (!rt || !rt->initialized || !request || !observe || !max_attempts || max_attempts > 1024) {
        result->stop = PS3_SPURS_REGISTRATION_INVALID_ARGUMENT; return;
    }
    if (may_continue && !may_continue(poll_context)) {
        result->stop = PS3_SPURS_REGISTRATION_CANCELLED; return;
    }
    uint32_t validation;
    if (!ps3_spurs_workload_registration_validate_request(&captured, &validation)) {
        result->stop = PS3_SPURS_REGISTRATION_INVALID_ARGUMENT; return;
    }
    if (validation) { result->has_sdk_result = 1; result->sdk_result = validation; return; }
    ps3_spurs_instance *owned = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state == PS3_SPURS_INSTANCE_ACTIVE && rt->instances[i].spurs_ea == captured.spurs)
            owned = &rt->instances[i];
    if (!owned) { result->stop = PS3_SPURS_REGISTRATION_UNOWNED_INSTANCE; return; }
    if (!rt->workload_services_configured || !owned->kernel_initialized) {
        result->stop = PS3_SPURS_REGISTRATION_NO_EXECUTION_SERVICES; return;
    }
    ps3_spurs_registration_config config = {
        .domain = rt->config.domain, .owner = borrowed_ppu_owner,
        .registration_guard = rt->workload_guards[owned->index], .instance = owned->spurs_ea,
        .active_owned_instance = 1, .max_attempts = max_attempts,
        .poll_context = poll_context, .may_continue = may_continue,
        .observer_context = observer_context, .observe = observe
    };
    ps3_spurs_workload_registration_apply(&config, &captured, result);
    /* Partial native stops leave this graph owned; parent STOP/join performs cleanup. */
}

void ps3_spurs_runtime_request_idle_spu(ps3_spurs_runtime *rt, const ps3_spurs_idle_request *request,
    ps3_spurs_idle_memory_scope memory_scope,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_idle_event *),
    ps3_spurs_idle_result *result) {
    if (!result) return;
    ps3_spurs_idle_request captured;
    if (request) captured = *request;
    memset(result, 0, sizeof *result);
    if (!rt || !rt->initialized || !request || !observe) {
        result->stop = PS3_SPURS_IDLE_INVALID_ARGUMENT; return;
    }
    if (may_continue && !may_continue(poll_context)) {
        result->stop = PS3_SPURS_IDLE_CANCELLED; return;
    }
    uint32_t validation;
    if (!ps3_spurs_idle_request_validate_address(&captured, &validation)) {
        result->stop = PS3_SPURS_IDLE_INVALID_ARGUMENT; return;
    }
    if (validation) { result->has_sdk_result = 1; result->sdk_result = validation; return; }
    ps3_spurs_instance *owned = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state == PS3_SPURS_INSTANCE_ACTIVE && rt->instances[i].spurs_ea == captured.spurs)
            owned = &rt->instances[i];
    if (!owned) { result->stop = PS3_SPURS_IDLE_UNOWNED_INSTANCE; return; }
    if (!rt->workload_services_configured || !owned->kernel_initialized) {
        result->stop = PS3_SPURS_IDLE_NO_EXECUTION_SERVICES; return;
    }
    ps3_spurs_idle_config config = {
        .domain = rt->config.domain, .instance = owned->spurs_ea, .active_owned_instance = 1,
        .workload_guard = rt->workload_guards[owned->index], .poll_context = poll_context,
        .may_continue = may_continue, .observer_context = observer_context, .observe = observe,
        .memory_scope = memory_scope
    };
    ps3_spurs_idle_request_apply(&config, &captured, result);
    /* A confirmed idle byte remains owned on native STOP; parent stop/join cleans up. */
}

typedef struct {
    ps3_spurs_runtime *runtime;
    void *caller_context;
    int (*caller_may_continue)(void *);
} workload_poll_context;
static int workload_may_continue(void *context) {
    workload_poll_context *poll = context;
    if (ps3_spurs_runtime_poll_stop(poll->runtime, NULL)) return 0;
    if (poll->caller_may_continue && !poll->caller_may_continue(poll->caller_context)) return 0;
    return !ps3_spurs_runtime_poll_stop(poll->runtime, NULL);
}
void ps3_spurs_runtime_exception_handler(ps3_spurs_runtime *rt, const ps3_spurs_exception_request *request,
    ps3_spurs_exception_memory_scope memory_scope, ps3_atomic_owner borrowed_ppu_owner, size_t max_claim_attempts,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_exception_event *),
    ps3_spurs_exception_result *result) {
    if (!result) return;
    ps3_spurs_exception_request captured;
    if (request) captured = *request;
    memset(result, 0, sizeof *result);
    if (!rt || !rt->initialized || !request || !observe || !max_claim_attempts || max_claim_attempts > 1024) {
        result->stop = PS3_SPURS_EXCEPTION_INVALID_ARGUMENT; return;
    }
    workload_poll_context poll = {rt, poll_context, may_continue};
    if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_EXCEPTION_CANCELLED; return; }
    uint32_t validation;
    if (!ps3_spurs_exception_validate_address(&captured, &validation)) {
        result->stop = PS3_SPURS_EXCEPTION_INVALID_ARGUMENT; return;
    }
    if (validation) {
        if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_EXCEPTION_CANCELLED; return; }
        result->has_sdk_result = 1; result->sdk_result = validation; return;
    }
    ps3_spurs_instance *owned = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state == PS3_SPURS_INSTANCE_ACTIVE && rt->instances[i].spurs_ea == captured.spurs)
            owned = &rt->instances[i];
    if (!owned) { result->stop = PS3_SPURS_EXCEPTION_UNOWNED_INSTANCE; return; }
    if (!rt->workload_services_configured || !owned->kernel_initialized) {
        result->stop = PS3_SPURS_EXCEPTION_NO_EXECUTION_SERVICES; return;
    }
    ps3_spurs_exception_config config = {
        .domain = rt->config.domain, .owner = borrowed_ppu_owner, .instance = owned->spurs_ea,
        .owned_instance_bytes = PS3_SPURS_EXTENT, .active_owned_instance = 1,
        .max_claim_attempts = max_claim_attempts, .poll_context = &poll, .may_continue = workload_may_continue,
        .observer_context = observer_context, .observe = observe, .memory_scope = memory_scope
    };
    ps3_spurs_exception_handler_apply(&config, &captured, result);
    if (result->has_sdk_result && !workload_may_continue(&poll)) {
        result->has_sdk_result = 0; result->sdk_result = 0; result->stop = PS3_SPURS_EXCEPTION_CANCELLED;
    }
    /* Registration effects survive a stop; caller stops/joins the owned graph. */
}

void ps3_spurs_runtime_ready_count_store(ps3_spurs_runtime *rt, const ps3_spurs_ready_count_request *request,
    ps3_spurs_ready_count_memory_scope memory_scope, ps3_atomic_owner borrowed_ppu_owner,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_ready_count_event *),
    ps3_spurs_ready_count_result *result) {
    if (!result) return;
    ps3_spurs_ready_count_request captured;
    if (request) captured = *request;
    memset(result, 0, sizeof *result);
    if (!rt || !rt->initialized || !request || !observe) {
        result->stop = PS3_SPURS_READY_COUNT_INVALID_ARGUMENT; return;
    }
    workload_poll_context poll = {rt, poll_context, may_continue};
    if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_READY_COUNT_CANCELLED; return; }
    uint32_t validation;
    if (!ps3_spurs_ready_count_validate_address(&captured, &validation)) {
        result->stop = PS3_SPURS_READY_COUNT_INVALID_ARGUMENT; return;
    }
    if (validation) {
        if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_READY_COUNT_CANCELLED; return; }
        result->has_sdk_result = 1; result->sdk_result = validation; return;
    }
    ps3_spurs_instance *owned = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state == PS3_SPURS_INSTANCE_ACTIVE && rt->instances[i].spurs_ea == captured.spurs)
            owned = &rt->instances[i];
    if (!owned) { result->stop = PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE; return; }
    if (!rt->workload_services_configured || !owned->kernel_initialized) {
        result->stop = PS3_SPURS_READY_COUNT_NO_EXECUTION_SERVICES; return;
    }
    ps3_spurs_ready_count_config config = {
        .domain = rt->config.domain, .owner = borrowed_ppu_owner, .instance = owned->spurs_ea,
        .owned_instance_bytes = PS3_SPURS_EXTENT, .active_owned_instance = 1,
        .poll_context = &poll, .may_continue = workload_may_continue,
        .observer_context = observer_context, .observe = observe, .memory_scope = memory_scope
    };
    ps3_spurs_ready_count_apply(&config, &captured, result);
    if (result->has_sdk_result && !workload_may_continue(&poll)) {
        result->has_sdk_result = 0; result->sdk_result = 0; result->stop = PS3_SPURS_READY_COUNT_CANCELLED;
    }
    /* A committed ready byte stays on native STOP; parent stop/join owns cleanup. */
}

void ps3_spurs_runtime_get_workload_info(ps3_spurs_runtime *rt, const ps3_spurs_workload_info_request *request,
    ps3_spurs_workload_info_memory_scope memory_scope, ps3_atomic_owner borrowed_ppu_owner,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_workload_info_event *),
    ps3_spurs_workload_info_result *result) {
    if (!result) return;
    ps3_spurs_workload_info_request captured;
    if (request) captured = *request;
    memset(result, 0, sizeof *result);
    if (!rt || !rt->initialized || !request || !observe) {
        result->stop = PS3_SPURS_WORKLOAD_INFO_INVALID_ARGUMENT; return;
    }
    workload_poll_context poll = {rt, poll_context, may_continue};
    if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_WORKLOAD_INFO_CANCELLED; return; }
    uint32_t validation;
    if (!ps3_spurs_workload_info_validate_address(&captured, &validation)) {
        result->stop = PS3_SPURS_WORKLOAD_INFO_INVALID_ARGUMENT; return;
    }
    if (validation) {
        if (!workload_may_continue(&poll)) { result->stop = PS3_SPURS_WORKLOAD_INFO_CANCELLED; return; }
        result->has_sdk_result = 1; result->sdk_result = validation; return;
    }
    ps3_spurs_instance *owned = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state == PS3_SPURS_INSTANCE_ACTIVE && rt->instances[i].spurs_ea == captured.spurs)
            owned = &rt->instances[i];
    if (!owned) { result->stop = PS3_SPURS_WORKLOAD_INFO_UNOWNED_INSTANCE; return; }
    ps3_spurs_workload_info_config config = {
        .domain = rt->config.domain, .owner = borrowed_ppu_owner, .instance = owned->spurs_ea,
        .owned_instance_bytes = PS3_SPURS_EXTENT, .active_owned_instance = 1,
        .poll_context = &poll, .may_continue = workload_may_continue,
        .observer_context = observer_context, .observe = observe, .memory_scope = memory_scope
    };
    ps3_spurs_workload_info_apply(&config, &captured, result);
    if (result->has_sdk_result && !workload_may_continue(&poll)) {
        result->has_sdk_result = 0; result->sdk_result = 0; result->stop = PS3_SPURS_WORKLOAD_INFO_CANCELLED;
    }
    /* A late fence retains each actual output store, including source aliases. */
}

ps3_spurs_result ps3_spurs_runtime_initialize(ps3_spurs_runtime *rt, const ps3_spurs_request *req) {
    if (!rt || !rt->initialized || !req) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, 0, 0);
    const ps3_spurs_runtime_config *c = &rt->config;
    /* S0 steps 1-3: attribute checks (no effects). */
    if (req->attribute_ea == 0) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_NULL, 0, 0, 0);
    if (req->attribute_ea % 8) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_ALIGN, 0, 0, 0);
    if (!req->attribute) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, 0, 0);
    const uint8_t *a = req->attribute;
    if (be32(a + A_REVISION) > 2) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_INVAL, 0, 0, 0);
    /* steps 5-8 */
    if (req->spurs_ea == 0) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_NULL, 0, 0, 0);
    if (req->spurs_ea % 128) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_ALIGN, 0, 0, 0);
    const uint32_t prefix_size = be32(a + A_PREFIX_SIZE);
    if (prefix_size > 15) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_INVAL, 0, 0, 0);
    const uint32_t region = req->spurs_ea >> 28;
    if (region == 0xD) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_PERM, 0, 0, 0);
    if (region > 2) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, 0, region);   /* GAP-N3 */
    /* Fail closed on every attribute field this reconstruction does not implement. */
    if (be32(a + A_FLAGS) != 0 || a[A_EXIT] != 0 || (uint64_t)req->spurs_ea + PS3_SPURS_EXTENT > UINT64_C(0x100000000))
        return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE, 0, be32(a + A_FLAGS));
    ps3_spurs_instance *in = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES && !in; ++i) if (rt->instances[i].state == PS3_SPURS_INSTANCE_FREE) in = &rt->instances[i];
    if (!in) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INSTANCE_CAPACITY, 0, 0);
    /* FREE proves the prior services joined; old immutable diagnostics may now reset. */
    memset(&rt->helper_stops[in->index], 0, sizeof rt->helper_stops[in->index]);
    in->state = PS3_SPURS_INSTANCE_BUILDING;
    in->spurs_ea = req->spurs_ea;
    in->nspus = be32(a + A_NSPUS);
    in->spu_priority = (int32_t)be32(a + A_SPU_PRIORITY);
    in->ppu_priority = be32(a + A_PPU_PRIORITY);
    in->prefix_size = prefix_size;
    static const unsigned char zeros[PS3_SPURS_EXTENT];
    unsigned char ff8[8];
    memset(ff8, 0xFF, sizeof ff8);
#define W(step, expr) do { if ((expr) != 0) return finish_stop(in, PS3_SPURS_STOP_WRITE_FAILED, (step), 0); } while (0)
    /* S2: W0-W13 */
    W(2, put(in, 0, zeros, sizeof zeros));
    W(2, put32(in, F_REVISION, be32(a + A_REVISION)));
    W(2, put32(in, F_SDK, be32(a + A_SDK)));
    W(2, put64(in, F_PPU0, 0xFFFFFFFFu));
    W(2, put64(in, F_PPU0 + 8, 0xFFFFFFFFu));
    W(2, put32(in, F_FLAGS, 0));
    if (prefix_size) W(2, put(in, F_PREFIX, a + A_PREFIX, prefix_size));
    W(2, put8(in, F_PREFIX_SIZE, (uint8_t)prefix_size));
    W(2, put32(in, F_WKL_ENABLED, 0x0000FFFFu));
    W(2, put32(in, F_SYS_SRV_TRACE, 0));
    W(2, put(in, F_SYS_SRV_PREEMPT, ff8, 8));
    W(2, put64(in, F_WKL_INFO_SYS_SRV, 0x100));
    W(2, put32(in, F_WKL_INFO_SYS_SRV + 0x10, 0x2200));
    W(2, put64(in, F_WKL_INFO_SYS_SRV + 8, 0));
    W(2, put8(in, F_WKL_INFO_SYS_SRV + 0x14, 0xFF));
    /* S3: 17 semaphores, W14-W16 */
    const ps3_lv2_semaphore_attr sem_attr = {PS3_SYNC_PRIORITY};
    for (unsigned i = 0; i < PS3_SPURS_SEMAPHORES; ++i) {
        uint32_t id = 0;
        uint32_t rc = ps3_lv2_semaphore_create(c->lv2, &id, &sem_attr, 0, 1);
        if (rc != PS3_LV2_OK) return route(in, rc, 3, rc);
        in->semaphores[in->semaphore_count++] = id;
        W(3, put64(in, i < 16 ? F_WKL_F1 + 0x80 * i : F_SEM_PRV, id));
    }
    W(3, put32(in, F_SEM_PRV + 8, 0xFFFFFFFFu));
    W(3, put32(in, F_SEM_PRV + 12, 0xFFFFFFFFu));
    W(3, put64(in, F_SEM_PRV + 16, 0));
    /* S4: W17-W19 (spuImg descriptor bytes only; no guest image is admitted, R-IMG) */
    W(4, put8(in, F_NSPUS, (uint8_t)in->nspus));
    W(4, put32(in, F_SPU_PRIORITY, (uint32_t)in->spu_priority));
    W(4, put32(in, F_SPU_IMG, 0));
    W(4, put32(in, F_SPU_IMG + 4, KERNEL1_ENTRY));
    W(4, put32(in, F_SPU_IMG + 8, 0));
    W(4, put32(in, F_SPU_IMG + 12, 0));
    /* S5: group_create(num=nSpus, prio=spuPriority, NORMAL). */
    {
        uint32_t rc = ps3_spu_group_create(c->policy, &in->group, in->nspus, in->spu_priority, 0, 20 + prefix_size);
        if (rc != PS3_LV2_OK) return route(in, rc, 5, rc);   /* EINVAL (nSpus/prio/check_valid) is a guest-visible rc: rollback + FAILED */
        in->group_created = 1;
        W(5, put32(in, F_SPU_GROUP, in->group));
    }
    /* S6: thread_initialize x nSpus (owner + LS buffer per SPU), W21 */
    if (in->nspus > PS3_SPURS_K1_MAX_SPUS) return finish_stop(in, PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE, 6, in->nspus);
    for (uint32_t n = 0; n < in->nspus; ++n) {
        unsigned slot = 0;
        while (slot < PS3_SPURS_LS_SLOTS && rt->ls_used[slot]) ++slot;
        if (slot == PS3_SPURS_LS_SLOTS || ps3_atomic_owner_acquire(c->domain, &in->owners[n]) != PS3_GUEST_MEMORY_OK)
            return finish_failed(in, PS3_LV2_EAGAIN, 6);
        rt->ls_used[slot] = 1;
        in->ls_slot[n] = (uint8_t)slot;
        in->ls[n] = c->local_stores[slot];
        in->owners_acquired = n + 1;
        uint32_t tid = 0;
        uint32_t rc = ps3_spu_thread_initialize(c->policy, &tid, in->group, n, PS3_SPU_IMAGE_NATIVE_SPURS_KERNEL1,
                                                (uint64_t)n << 32, in->spurs_ea, 2, 16 + prefix_size);
        if (rc != PS3_LV2_OK) return route(in, rc, 6, rc);
        in->spu_threads[n] = tid;
        in->threads_initialized = n + 1;
        W(6, put32(in, F_SPUS + 4 * n, tid));
    }
    /* S7: lwmutex (PRIORITY, NOT_RECURSIVE=0x20) W22, lwcond W23 */
    {
        uint32_t rc = c->sync.lwmutex_create(c->sync.context, in->spurs_ea + F_LWMUTEX, PS3_SYNC_PRIORITY, 0x20);
        if (rc != 0) return route(in, rc, 7, rc);   /* provider failures are STOPs, resource rcs roll back (review M1) */
        in->lwmutex_created = 1;
        rc = c->sync.lwcond_create(c->sync.context, in->spurs_ea + F_LWCOND, in->spurs_ea + F_LWMUTEX);
        if (rc != 0) return route(in, rc, 7, rc);
        in->lwcond_created = 1;
    }
    /* S8: W24-W28 */
    W(8, put8(in, F_FLAGS1, 0));
    W(8, put8(in, F_WKL_FLAG_RECEIVER, 0xFF));
    W(8, put32(in, F_WKL_FLAG, 0xFFFFFFFFu));
    W(8, put(in, F_FLAGS_0xD64, zeros, 3));
    W(8, put32(in, F_PPU_PRIORITY, in->ppu_priority));
    /* S9: queue(0x2A, PRIORITY, PPU) W29; attach to all threads (bits 16..63) W30 */
    {
        const ps3_lv2_queue_attr qa = {PS3_SYNC_PRIORITY, PS3_PPU_QUEUE, 0};
        uint32_t rc = ps3_lv2_queue_create(c->lv2, &in->queue, &qa, 0, QUEUE_SIZE);
        if (rc != PS3_LV2_OK) return route(in, rc, 9, rc);
        in->queue_created = 1;
        W(9, put32(in, F_EVENT_QUEUE, in->queue));
        uint8_t spup = 0;
        rc = ps3_spu_group_connect_event_all_threads(c->policy, in->group, in->queue, ~UINT64_C(0) << SPUP_MASK_SHIFT, &spup);
        if (rc != PS3_LV2_OK) return finish_stop(in, PS3_SPURS_STOP_ATTACH_FAILED, 9, rc);   /* reference defect not copied */
        in->attached = 1;
        in->spup = spup;
        W(9, put8(in, F_SPU_PORT, spup));
    }
    /* S10: port create + local connect, W31 */
    {
        uint32_t rc = ps3_lv2_port_create(c->lv2, &in->port, PS3_EVENT_PORT_LOCAL, 0);
        if (rc == PS3_LV2_UNSUPPORTED || rc == PS3_LV2_PROVIDER_FAILURE) return route(in, rc, 10, rc);
        if (rc != PS3_LV2_OK) return finish_failed(in, PS3_SPURS_RC_AGAIN, 10);
        in->port_created = 1;
        rc = ps3_lv2_port_source(c->lv2, in->port, &in->helper_stop_source);
        if (rc != PS3_LV2_OK) return finish_stop(in, PS3_SPURS_STOP_SERVICE_ANOMALY, 10, rc);
        rc = ps3_lv2_port_connect_local(c->lv2, in->port, in->queue);
        if (rc != PS3_LV2_OK) return finish_failed(in, PS3_SPURS_RC_STAT, 10);
        in->port_connected = 1;
        W(10, put32(in, F_EVENT_PORT, in->port));
    }
    /* S11: event helper service (prio 249): must really block in its receive before ppu1 (W32) */
    {
        if (ps3_lv2_queue_bind_service(c->lv2, in->queue, helper_stop_event) != PS3_LV2_OK)
            return finish_stop(in, PS3_SPURS_STOP_SERVICE_ANOMALY, 11, 0);
        in->queue_bound = 1;
        uint32_t rc = ps3_ppu_service_create(c->ppu, 1, HELPER_PRIORITY, c->creator_priority, HELPER_STACK,
                                             c->threads[in->index][PS3_SPURS_THREAD_HELPER], helper_body, in, &in->helper);
        if (rc == PS3_PPU_OWNED_FAILURE) { in->helper_live = 1; return finish_stop(in, PS3_SPURS_STOP_PROVIDER_FAILURE, 11, rc); }
        if (rc == PS3_PPU_UNSUPPORTED) return finish_stop(in, PS3_SPURS_STOP_UNSUPPORTED_LV2, 11, rc);
        if (rc != PS3_PPU_OK) return finish_failed(in, PS3_SPURS_RC_STAT, 11);
        in->helper_live = 1;
        if (!ps3_ppu_service_is_blocked(c->ppu, in->helper)) return finish_stop(in, PS3_SPURS_STOP_SERVICE_ANOMALY, 11, 1);
        W(11, put64(in, F_PPU0 + 8, in->helper.id));   /* ppu1 */
    }
    /* S12: handler service (prio ppuPriority): its synchronous prefix runs group_start (contexts execute to the
     * L0 reservation wait) and ends BLOCKED_JOIN. ppu0 (W33) only after that. */
    {
        uint32_t rc = ps3_ppu_service_create(c->ppu, 2, in->ppu_priority, c->creator_priority, HANDLER_STACK,
                                             c->threads[in->index][PS3_SPURS_THREAD_HANDLER], handler_body, in, &in->handler);
        if (rc == PS3_PPU_OWNED_FAILURE) { in->handler_live = 1; return finish_stop(in, PS3_SPURS_STOP_PROVIDER_FAILURE, 12, rc); }
        if (rc == PS3_PPU_UNSUPPORTED) return finish_stop(in, PS3_SPURS_STOP_UNSUPPORTED_LV2, 12, rc);
        if (rc != PS3_PPU_OK) return finish_failed(in, PS3_SPURS_RC_STAT, 12);   /* helper stopped+joined by unwind */
        in->handler_live = 1;
        const uint64_t status = __atomic_load_n(&in->handler_status, __ATOMIC_SEQ_CST);
        if (status == PS3_SPURS_HANDLER_KERNEL_STOPPED) {
            ps3_spurs_result r = finish_stop(in, PS3_SPURS_STOP_KERNEL, 12, in->kernel_stop.reason);   /* no ppu0, no rc */
            r.kernel_spu = in->kernel_stop.spu; r.kernel_step = in->kernel_stop.step;
            return r;
        }
        if (status != PS3_SPURS_HANDLER_BLOCKED_JOIN || !ps3_ppu_service_is_blocked(c->ppu, in->handler))
            return finish_stop(in, PS3_SPURS_STOP_SERVICE_ANOMALY, 12, (uint32_t)status);
        W(12, put64(in, F_PPU0, in->handler.id));         /* ppu0 */
    }
    /* S13: enableEH then the group EXCEPTION -> queue connection (failure = invariant stop) */
    W(13, put32(in, F_ENABLE_EH, 1));
    {
        uint32_t rc = ps3_spu_group_connect_event(c->policy, in->group, in->queue, PS3_SPU_EVENT_EXCEPTION);
        if (rc != PS3_LV2_OK) return finish_stop(in, PS3_SPURS_STOP_EXCEPTION_CONNECT_FAILED, 13, rc);
        in->exception_connected = 1;
    }
    /* S14, S15 */
    W(14, put64(in, F_TRACE_BUFFER, 0));
    W(15, put(in, F_PORT_MUX, zeros, 0x80));
    W(15, put32(in, F_PORT_MUX + 4, in->spup));
    W(15, put32(in, F_PORT_MUX + 8, 3));
    W(15, put64(in, F_PORT_MUX + 0x10, in->port));
#undef W
    in->state = PS3_SPURS_INSTANCE_ACTIVE;
    return result_of(in, PS3_SPURS_OUTCOME_OK, PS3_SPURS_RC_OK, 0, 16, 0);
}

int ps3_spurs_runtime_poll_stop(ps3_spurs_runtime *rt, ps3_spurs_stop_info *info) {
    if (!rt || !rt->initialized) return 0;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i) {
        ps3_spurs_instance *in = &rt->instances[i];
        ps3_spurs_k1_stop stop;
        if (in->state == PS3_SPURS_INSTANCE_ACTIVE && in->kernel_initialized && ps3_spurs_k1_poll_stop(&in->kernel, &stop)) {
            if (info) *info = (ps3_spurs_stop_info){.instance=i, .reason=stop.reason, .spu=stop.spu, .step=stop.step,
                .category=PS3_SPURS_STOP_CATEGORY_KERNEL};
            return 1;
        }
        const ps3_spurs_helper_stop_capture *capture = &rt->helper_stops[i];
        if (in->state == PS3_SPURS_INSTANCE_ACTIVE && __atomic_load_n(&capture->ready, __ATOMIC_ACQUIRE)) {
            if (info) {
                *info = (ps3_spurs_stop_info){.instance=i,
                    .reason=capture->category == PS3_SPURS_STOP_CATEGORY_HELPER_EVENT ?
                        PS3_SPURS_STOP_UNEXPECTED_HELPER_EVENT : PS3_SPURS_STOP_HELPER_RECEIVE_FAILURE,
                    .category=capture->category, .spurs_ea=capture->spurs_ea, .queue=capture->queue,
                    .receive_status=capture->receive_status,
                    .has_event=capture->category == PS3_SPURS_STOP_CATEGORY_HELPER_EVENT};
                if (info->has_event) info->event = capture->event;
            }
            return 1;
        }
    }
    return 0;
}
/* ---- guest cellSpursAttachLv2EventQueue (static), reference attach_lv2_eq with spursCreated == false ---- */
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
/* After the connect: exact detach of what this attach bound. The record is cleared only when that is CONFIRMED; otherwise the
 * binding stays recorded (shutdown retries it) and the stop says so. Either way the instance is terminal (RETAINED). */
static ps3_spurs_result attach_rollback(ps3_spurs_instance *in, ps3_spurs_attach_info *info, uint32_t queue, unsigned port,
                                        ps3_spurs_stop_reason reason, uint32_t step, uint32_t detail) {
    info->cause = reason;
    const uint32_t rc = ps3_spu_group_disconnect_event_all_threads_exact(cfg_of(in)->policy, in->group, (uint8_t)port, queue);
    if (rc != PS3_LV2_OK) return finish_stop(in, PS3_SPURS_STOP_ROLLBACK_FAILED, step, rc);
    in->user_port_bits &= ~(UINT64_C(1) << port);
    in->user_port_queue[port] = 0;
    return finish_stop(in, reason, step, detail);
}
ps3_spurs_result ps3_spurs_runtime_attach_lv2_event_queue(ps3_spurs_runtime *rt, const ps3_spurs_attach_request *rq,
                                                          const ps3_spurs_attach_memory *mem, ps3_spurs_attach_info *info_out) {
    ps3_spurs_attach_info scratch;
    ps3_spurs_attach_info *info = info_out ? info_out : &scratch;
    memset(info, 0, sizeof *info);
    /* Host contract first: a bad request or port is a native bug, not guest behavior, and nothing has been touched.
     * sdk_version is the EFFECTIVE PROCESS sdk (reference get_sdk_version: s32, raw -1 mapped to 0x485000 by the ADAPTER, never
     * read from CellSpurs+0xDA4). An unmapped negative (> INT32_MAX as uint32) is a host bug: stop, so the unsigned compare
     * below is only ever applied to the valid non-negative range (the reference's signed compare, unchanged there). */
    if (!rt || !rt->initialized || !rq || !mem || !mem->read || !mem->write || !mem->compare_exchange64 || rq->max_cas_attempts == 0 ||
        rq->sdk_version > (uint32_t)INT32_MAX)
        return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_REQUEST, 0);
    const ps3_spurs_runtime_config *c = &rt->config;
    if (rq->spurs_ea == 0 || rq->port_ea == 0) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_NULL, 0, PS3_SPURS_ATTACH_STEP_REQUEST, 0);
    if (rq->spurs_ea % 128) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_ALIGN, 0, PS3_SPURS_ATTACH_STEP_REQUEST, 0);
    ps3_spurs_instance *in = NULL;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state != PS3_SPURS_INSTANCE_FREE && rt->instances[i].spurs_ea == rq->spurs_ea) in = &rt->instances[i];
    if (!in) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_ATTACH_STEP_OWNER, rq->spurs_ea);
    if (in->state != PS3_SPURS_INSTANCE_ACTIVE)   /* never add effects to a building or retained instance */
        return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_ATTACH_FAILED, PS3_SPURS_ATTACH_STEP_OWNER, in->state);
#define PRE_STOP(reason, step, detail) return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, (reason), (step), (detail))
#define PRE_FAILED(code, step) return result_of(in, PS3_SPURS_OUTCOME_FAILED, (code), 0, (step), 0)
    uint8_t raw[8];
    int io = mem->read(mem->context, rq->spurs_ea + F_EXCEPTION, raw, 4);
    if (io) PRE_STOP(PS3_SPURS_STOP_READ_FAILED, PS3_SPURS_ATTACH_STEP_EXCEPTION, (uint32_t)io);
    if (be32(raw) != 0) PRE_FAILED(PS3_SPURS_RC_STAT, PS3_SPURS_ATTACH_STEP_EXCEPTION);
    /* Two port modes:
     *   STATIC  (is_dynamic == 0): read the requested port byte; it is validated and then rewritten.
     *   DYNAMIC (is_dynamic != 0): the firmware's `0xAFEC` requested-port read is NOT executed -- libsre 0xAFE8 branches
     *           straight to the mask build with 16..63 as the candidate set. port_ea is output-only. Nothing is read. */
    const unsigned dynamic = rq->is_dynamic != 0;
    uint8_t port = 0;
    if (!dynamic) {
        io = mem->read(mem->context, rq->port_ea, &port, 1);
        if (io) PRE_STOP(PS3_SPURS_STOP_READ_FAILED, PS3_SPURS_ATTACH_STEP_PORT_READ, (uint32_t)io);
        if (port > ATTACH_MAX_PORT) PRE_FAILED(PS3_SPURS_RC_INVAL, PS3_SPURS_ATTACH_STEP_PORT_RANGE);
        if (rq->sdk_version >= ATTACH_SDK_PORT_LIMIT && port > ATTACH_SDK_MAX_PORT) PRE_FAILED(PS3_SPURS_RC_PERM, PS3_SPURS_ATTACH_STEP_PORT_RANGE);
        if (port >= ATTACH_USER_PORTS)   /* legal on an old SDK, but 16..63 hold SPURS's own binding: not modeled */
            PRE_STOP(PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_ATTACH_STEP_PORT_RANGE, port);
    }
    /* A live queue must be a plain PPU queue that no service owns (SPURS's own internal queue is service-bound, so it is
     * refused here too). An ABSENT queue is answered here (ESRCH, as the reference kernel connect answers it first) BEFORE the
     * DA8 read and the mutating connect, so a queue created concurrently under that id can never bind past this preflight.
     * The caller keeps an existing queue alive (borrowing contract). Any other snapshot status is unexpected: fail closed. */
    {
        ps3_lv2_queue q;
        const uint32_t qs = ps3_lv2_queue_snapshot(c->lv2, rq->queue, &q);
        if (qs == PS3_LV2_ESRCH) PRE_FAILED(qs, PS3_SPURS_ATTACH_STEP_QUEUE);
        if (qs != PS3_LV2_OK) PRE_STOP(PS3_SPURS_STOP_PROVIDER_FAILURE, PS3_SPURS_ATTACH_STEP_QUEUE, qs);
        if (q.type != PS3_PPU_QUEUE || q.service_bound)
            PRE_STOP(PS3_SPURS_STOP_UNSUPPORTED_LV2, PS3_SPURS_ATTACH_STEP_QUEUE, q.type != PS3_PPU_QUEUE ? q.type : 0x100);
    }
    uint64_t observed = 0;
    if (dynamic) {   /* no DA8 seed: the firmware's keyed path never touches 0xDA8 (0xB074 skips 0xB080) */
        observed = 0;
    } else {
        io = mem->read(mem->context, rq->spurs_ea + F_SPU_PORT_BITS, raw, 8);   /* seed for the CAS loop */
        if (io) PRE_STOP(PS3_SPURS_STOP_READ_FAILED, PS3_SPURS_ATTACH_STEP_BITS_READ, (uint32_t)io);
        observed = be64(raw);
    }
    /* Connect to every thread (all-or-none under the policy guard). Kernel codes are returned unchanged, EISCONN is BUSY.
     * DYNAMIC: the request mask is libsre's 0xFFFFFFFFFFFF0000 (ports 16..63) and the policy answers with the lowest free
     * port in it -- the choice syscall 251 makes for the firmware. STATIC: the single validated port. */
    uint8_t spup = 0xFF;
    const uint32_t rc = ps3_spu_group_connect_event_all_threads(c->policy, in->group, rq->queue,
        dynamic ? PS3_SPURS_DYNAMIC_PORT_MASK : UINT64_C(1) << port, &spup);
    if (rc == PS3_LV2_UNSUPPORTED) PRE_STOP(PS3_SPURS_STOP_UNSUPPORTED_LV2, PS3_SPURS_ATTACH_STEP_CONNECT, rc);
    if (rc == PS3_LV2_PROVIDER_FAILURE) PRE_STOP(PS3_SPURS_STOP_PROVIDER_FAILURE, PS3_SPURS_ATTACH_STEP_CONNECT, rc);
    if (rc == PS3_LV2_EISCONN) PRE_FAILED(PS3_SPURS_RC_BUSY, PS3_SPURS_ATTACH_STEP_CONNECT);
    if (rc != PS3_LV2_OK) PRE_FAILED(rc, PS3_SPURS_ATTACH_STEP_CONNECT);
#undef PRE_FAILED
#undef PRE_STOP
    /* Bound. STATIC: the ledger index is the VALIDATED requested port (0..15, the policy request mask had only that bit),
     * never the policy's returned spup: a mismatch is a sibling bug, recorded on the requested port and rolled back exactly
     * there. DYNAMIC: the policy's answer IS the port the kernel chose, so it is the index for every later effect. */
    if (dynamic) {
        if (spup >= PS3_SPU_PORTS)   /* outside the 64-slot mask the policy was given: it cannot have chosen it */
            return attach_rollback(in, info, rq->queue, spup, PS3_SPURS_STOP_PROVIDER_FAILURE, PS3_SPURS_ATTACH_STEP_CONNECT, spup);
        port = spup;
    }
    info->spup = port;
    in->user_port_queue[port] = rq->queue;   /* recorded NOW, before any fallible guest write; every failure below rolls back exactly */
    in->user_port_bits |= UINT64_C(1) << port;
    if (!dynamic && spup != port) return attach_rollback(in, info, rq->queue, port, PS3_SPURS_STOP_PROVIDER_FAILURE, PS3_SPURS_ATTACH_STEP_CONNECT, spup);
    io = mem->write(mem->context, rq->port_ea, &port, 1);   /* the syscall's *port = spup (== port), before the bit is set */
    if (io) return attach_rollback(in, info, rq->queue, port, PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_ATTACH_STEP_PORT_WRITE, (uint32_t)io);
    info->port_written = 1;
    /* DYNAMIC: done here. The firmware's keyed 0xAF2C path makes no guest write beyond the caller's OUT byte (CONTRACT-DYNAMIC
     * §4), so there is no DA8 CAS and nothing can fail after the byte write. OK, with cas_attempts left 0. */
    if (dynamic) return result_of(in, PS3_SPURS_OUTCOME_OK, PS3_SPURS_RC_OK, 0, PS3_SPURS_ATTACH_STEP_DONE, 0);
    const uint64_t bit = UINT64_C(1) << port;
    for (uint32_t attempt = 0; attempt < rq->max_cas_attempts; ++attempt) {
        ps3_atomic_compare_result cr = {0, 0};
        io = mem->compare_exchange64(mem->context, rq->spurs_ea + F_SPU_PORT_BITS, observed, observed | bit, &cr);
        info->cas_attempts = attempt + 1;
        if (cr.exchanged) {   /* committed: never undone, whatever the callback then reported */
            info->prior_port_bits = observed;
            info->prior_bit_set = (observed & bit) != 0;
            info->commit_status = io;
            /* F2: a committed CAS whose callback failed is NOT success (info is optional, so OK would fail open). Report it as a
             * STOP at the appended COMMITTED step with the callback code; no rollback: the DA8 bit, port byte, ledger and every
             * SPU binding stay, the instance is RETAINED and shutdown detaches exactly (DA8 residue is not cleared). */
            if (io) return finish_stop(in, PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_ATTACH_STEP_COMMITTED, (uint32_t)io);
            return result_of(in, PS3_SPURS_OUTCOME_OK, PS3_SPURS_RC_OK, 0, PS3_SPURS_ATTACH_STEP_DONE, 0);
        }
        if (io) return attach_rollback(in, info, rq->queue, port, PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_ATTACH_STEP_BITS_CAS, (uint32_t)io);
        observed = cr.observed;   /* another writer won: retry from the value the strong CAS saw */
    }
    return attach_rollback(in, info, rq->queue, port, PS3_SPURS_STOP_ATTACH_FAILED, PS3_SPURS_ATTACH_STEP_BITS_CAS, rq->max_cas_attempts);
}
/* ---- guest cellSpursGetInfo (static), reference cellSpursGetInfo RPCS3 63996a47 cellSpurs.cpp:1970-2007 ----
 * Pinned source offsets (cellSpurs.h CellSpurs) and output offsets (CellSpursInfo h:308-329) are re-stated as literals and
 * checked against the shared F_* constants at compile time. */
_Static_assert(F_NSPUS == 0x076 && F_TRACE_BUFFER == 0x900 && F_TRACE_SIZE == 0x948 && F_PPU0 == 0xD20 && F_PPU1 == 0xD28 &&
               F_SPU_GROUP == 0xD30 && F_SPUS == 0xD34 && F_FLAGS == 0xD80 && F_SPU_PRIORITY == 0xD84 && F_PPU_PRIORITY == 0xD88 &&
               F_PREFIX == 0xD8C && F_PREFIX_SIZE == 0xD9B, "CellSpurs source offsets must match the pinned header");
enum {
    I_NSPUS = 0x00, I_SPU_PRIORITY = 0x04, I_PPU_PRIORITY = 0x08, I_EXIT_IF_NO_WORK = 0x0C, I_SPURS2 = 0x0D, I_TRACE_BUFFER = 0x10,
    I_TRACE_SIZE = 0x18, I_TRACE_MODE = 0x20, I_GROUP = 0x24, I_SPU_THREADS = 0x28, I_HANDLER0 = 0x48, I_HANDLER1 = 0x50,
    I_NAME = 0x58, I_NAME_LENGTH = 0x68, I_MISS = 0x6C, I_MEET = 0x70,
    SAF_EXIT_IF_NO_WORK = 1, SAF_SECOND_VERSION = 4, INFO_NAME_MAX = 15, INFO_SPUS = 8
};
_Static_assert(PS3_SPURS_INFO_BYTES == 0x118 && (unsigned)INFO_SPUS == (unsigned)PS3_SPURS_K1_MAX_SPUS, "CellSpursInfo size / SPU id count");
typedef struct {
    const ps3_spurs_info_memory *mem;
    ps3_spurs_info_report *rep;
    const ps3_spurs_instance *in;
    uint32_t b, i;
    ps3_spurs_result res;
    /* values observed so far (the only inputs of the diagnostic mirror comparison) */
    unsigned seen;   /* bit n: mirror source n observed */
    uint32_t nspus, spu_prio, ppu_prio, group, ids[INFO_SPUS], pfx_size;
    uint64_t h0, h1;
} gi_ctx;
static int gi_fail(gi_ctx *g, ps3_spurs_stop_reason reason, uint32_t step, uint32_t detail) {
    g->res = result_of(g->in, PS3_SPURS_OUTCOME_STOP, 0, reason, step, detail);
    return 1;
}
static int gi_read(gi_ctx *g, uint32_t step, uint32_t offset, void *bytes, size_t length) {
    const int rc = g->mem->read(g->mem->context, g->b + offset, bytes, length);
    if (rc) return gi_fail(g, PS3_SPURS_STOP_READ_FAILED, step, (uint32_t)rc);
    ++g->rep->reads;
    return 0;
}
static int gi_write(gi_ctx *g, uint32_t step, uint32_t offset, const void *bytes, size_t length) {
    const int rc = g->mem->write(g->mem->context, g->i + offset, bytes, length);
    if (rc) return gi_fail(g, PS3_SPURS_STOP_WRITE_FAILED, step, (uint32_t)rc);
    ++g->rep->writes;
    g->rep->last_write_offset = offset;
    return 0;
}
static int gi_r8(gi_ctx *g, uint32_t step, uint32_t offset, uint8_t *v) { return gi_read(g, step, offset, v, 1); }
static int gi_r32(gi_ctx *g, uint32_t step, uint32_t offset, uint32_t *v) {
    uint8_t raw[4];
    if (gi_read(g, step, offset, raw, 4)) return 1;
    *v = be32(raw);
    return 0;
}
static int gi_r64(gi_ctx *g, uint32_t step, uint32_t offset, uint64_t *v) {
    uint8_t raw[8];
    if (gi_read(g, step, offset, raw, 8)) return 1;
    *v = be64(raw);
    return 0;
}
static int gi_w8(gi_ctx *g, uint32_t step, uint32_t offset, uint8_t v) { return gi_write(g, step, offset, &v, 1); }
static int gi_w32(gi_ctx *g, uint32_t step, uint32_t offset, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return gi_write(g, step, offset, b, 4);
}
static int gi_w64(gi_ctx *g, uint32_t step, uint32_t offset, uint64_t v) {
    const uint8_t b[8] = {(uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40), (uint8_t)(v >> 32),
                          (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return gi_write(g, step, offset, b, 8);
}
/* Diagnostic only. Bounds come from the NATIVE record (validated in->nspus <= PS3_SPURS_K1_MAX_SPUS, 8-entry arrays); the guest nSpus is a
 * value to compare, never an index bound. Only sources already observed contribute. */
static uint32_t gi_mirror(const gi_ctx *g) {
    const ps3_spurs_instance *in = g->in;
    const uint32_t native_n = in->nspus < INFO_SPUS ? in->nspus : INFO_SPUS;
    uint32_t bits = 0;
    if ((g->seen & 1u) && g->nspus != in->nspus) bits |= 1u << 0;
    if ((g->seen & 2u) && (int32_t)g->spu_prio != in->spu_priority) bits |= 1u << 1;
    if ((g->seen & 4u) && g->ppu_prio != in->ppu_priority) bits |= 1u << 2;
    if ((g->seen & 8u) && g->group != in->group) bits |= 1u << 3;
    if (g->seen & 16u)
        for (uint32_t k = 0; k < INFO_SPUS; ++k)
            if (g->ids[k] != (k < native_n ? in->spu_threads[k] : 0u)) { bits |= 1u << 4; break; }
    if ((g->seen & 32u) && g->h0 != (uint64_t)in->handler.id) bits |= 1u << 5;
    if ((g->seen & 64u) && g->h1 != (uint64_t)in->helper.id) bits |= 1u << 6;
    if ((g->seen & 128u) && g->pfx_size != in->prefix_size) bits |= 1u << 7;
    return bits;
}
static ps3_spurs_result gi_done(gi_ctx *g) {
    g->rep->partial = g->rep->writes > 0 && g->res.outcome != PS3_SPURS_OUTCOME_OK;
    return g->res;
}
ps3_spurs_result ps3_spurs_runtime_get_info(ps3_spurs_runtime *rt, const ps3_spurs_info_request *rq, const ps3_spurs_info_memory *mem,
                                            ps3_spurs_info_report *report) {
    ps3_spurs_info_report scratch;
    ps3_spurs_info_report *rep = report ? report : &scratch;
    memset(rep, 0, sizeof *rep);
    rep->last_write_offset = UINT32_MAX;
    /* Host contract first: nothing is consulted and nothing is called. */
    if (!rt || !rt->initialized || !rq || !mem || !mem->preflight || !mem->read || !mem->write)
        return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INVALID_REQUEST, PS3_SPURS_INFO_STEP_REQUEST, 0);
    /* Reference order: NULL (either pointer), then SPURS alignment, only then the native ownership lookup. */
    if (rq->spurs_ea == 0 || rq->info_ea == 0) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_NULL, 0, PS3_SPURS_INFO_STEP_REQUEST, 0);
    if (rq->spurs_ea % 128) return result_of(NULL, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_RC_ALIGN, 0, PS3_SPURS_INFO_STEP_REQUEST, 0);
    const ps3_spurs_instance *in = NULL;
    for (unsigned k = 0; k < PS3_SPURS_MAX_INSTANCES; ++k)
        if (rt->instances[k].state != PS3_SPURS_INSTANCE_FREE && rt->instances[k].spurs_ea == rq->spurs_ea) in = &rt->instances[k];
    if (!in) return result_of(NULL, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_INFO_STEP_OWNER, rq->spurs_ea);
    if (in->state != PS3_SPURS_INSTANCE_ACTIVE)
        return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_INSTANCE_NOT_ACTIVE, PS3_SPURS_INFO_STEP_OWNER, in->state);
    /* The output must be addressable without wrapping and must not intersect the whole owned instance (the reference copies source
     * fields and output fields interleaved; overlap is explicitly unsupported here, never a silent altered read order). */
    if (rq->info_ea > UINT32_MAX - (PS3_SPURS_INFO_BYTES - 1))
        return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_PREFLIGHT_REFUSED, PS3_SPURS_INFO_STEP_RANGE, 0);
    const uint64_t info_end = (uint64_t)rq->info_ea + PS3_SPURS_INFO_BYTES, spurs_end = (uint64_t)rq->spurs_ea + PS3_SPURS_EXTENT;
    if ((uint64_t)rq->info_ea < spurs_end && (uint64_t)rq->spurs_ea < info_end)
        return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_OUTPUT_OVERLAP, PS3_SPURS_INFO_STEP_OVERLAP, rq->info_ea);
    const uint32_t B = rq->spurs_ea, I = rq->info_ea;
    {
        const ps3_spurs_span reads[PS3_SPURS_INFO_READ_SPANS] = {{B + F_NSPUS, 1}, {B + F_TRACE_BUFFER, 8}, {B + F_TRACE_SIZE, 8}, {B + F_PPU0, 0x7C}};
        const ps3_spurs_span writes[PS3_SPURS_INFO_WRITE_SPANS] = {{I + 0x00, 0x0E}, {I + I_TRACE_BUFFER, 4}, {I + 0x18, 0x5C}};
        const int pf = mem->preflight(mem->context, reads, PS3_SPURS_INFO_READ_SPANS, writes, PS3_SPURS_INFO_WRITE_SPANS, PS3_SPURS_INFO_MAX_ACCESSES);
        if (pf) return result_of(in, PS3_SPURS_OUTCOME_STOP, 0, PS3_SPURS_STOP_PREFLIGHT_REFUSED, PS3_SPURS_INFO_STEP_PREFLIGHT, (uint32_t)pf);
    }
    gi_ctx g;
    memset(&g, 0, sizeof g);
    g.mem = mem; g.rep = rep; g.in = in; g.b = B; g.i = I;
#define TRY(x) do { if (x) return gi_done(&g); } while (0)
    /* Reference order. Each value is read once and used as read (no snapshot, no re-read); flags are captured once. */
    uint32_t flags;
    TRY(gi_r32(&g, PS3_SPURS_INFO_STEP_FLAGS, F_FLAGS, &flags));
    rep->flags = flags;
    uint8_t nspus;
    TRY(gi_r8(&g, PS3_SPURS_INFO_STEP_NSPUS, F_NSPUS, &nspus));
    rep->nspus = nspus; g.nspus = nspus; g.seen |= 1u;
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_NSPUS, I_NSPUS, nspus));
    uint32_t v;
    TRY(gi_r32(&g, PS3_SPURS_INFO_STEP_SPU_PRIORITY, F_SPU_PRIORITY, &v));
    g.spu_prio = v; g.seen |= 2u;
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_SPU_PRIORITY, I_SPU_PRIORITY, v));
    TRY(gi_r32(&g, PS3_SPURS_INFO_STEP_PPU_PRIORITY, F_PPU_PRIORITY, &v));
    g.ppu_prio = v; g.seen |= 4u;
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_PPU_PRIORITY, I_PPU_PRIORITY, v));
    TRY(gi_w8(&g, PS3_SPURS_INFO_STEP_EXIT_FLAGS, I_EXIT_IF_NO_WORK, (flags & SAF_EXIT_IF_NO_WORK) ? 1 : 0));
    TRY(gi_w8(&g, PS3_SPURS_INFO_STEP_EXIT_FLAGS, I_SPURS2, (flags & SAF_SECOND_VERSION) ? 1 : 0));
    TRY(gi_r32(&g, PS3_SPURS_INFO_STEP_GROUP, F_SPU_GROUP, &v));
    g.group = v; g.seen |= 8u;
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_GROUP, I_GROUP, v));
    for (unsigned k = 0; k < INFO_SPUS; ++k) TRY(gi_r32(&g, PS3_SPURS_INFO_STEP_SPU_IDS, F_SPUS + 4 * k, &g.ids[k]));   /* all 8, whatever nSpus says */
    g.seen |= 16u;
    for (unsigned k = 0; k < INFO_SPUS; ++k) TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_SPU_IDS, I_SPU_THREADS + 4 * k, g.ids[k]));
    TRY(gi_r64(&g, PS3_SPURS_INFO_STEP_HANDLER0, F_PPU0, &g.h0));
    g.seen |= 32u;
    TRY(gi_w64(&g, PS3_SPURS_INFO_STEP_HANDLER0, I_HANDLER0, g.h0));
    TRY(gi_r64(&g, PS3_SPURS_INFO_STEP_HANDLER1, F_PPU1, &g.h1));
    g.seen |= 64u;
    TRY(gi_w64(&g, PS3_SPURS_INFO_STEP_HANDLER1, I_HANDLER1, g.h1));
    uint64_t v64;
    TRY(gi_r64(&g, PS3_SPURS_INFO_STEP_TRACE_SIZE, F_TRACE_SIZE, &v64));
    rep->trace_size = v64;
    TRY(gi_w64(&g, PS3_SPURS_INFO_STEP_TRACE_SIZE, I_TRACE_SIZE, v64));
    TRY(gi_r64(&g, PS3_SPURS_INFO_STEP_TRACE_BUFFER, F_TRACE_BUFFER, &v64));
    rep->trace_buffer_raw = v64;
    if (v64 >> 32) {   /* the output pointer is 32 bits; no guest-visible error is invented for an unrepresentable value */
        rep->mirror_mismatch = gi_mirror(&g);
        gi_fail(&g, PS3_SPURS_STOP_GUEST_STATE, PS3_SPURS_INFO_STEP_TRACE_BUFFER, (uint32_t)v64);
        return gi_done(&g);
    }
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_TRACE_BUFFER, I_TRACE_BUFFER, (uint32_t)v64 & ~3u));
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_TRACE_BUFFER, I_TRACE_MODE, (uint32_t)v64 & 3u));
    uint8_t n;
    TRY(gi_r8(&g, PS3_SPURS_INFO_STEP_PREFIX, F_PREFIX_SIZE, &n));
    rep->prefix_size = n; g.pfx_size = n; g.seen |= 128u;
    if (n > INFO_NAME_MAX) {   /* the reference's fatal ensure(): a late STOP after the earlier reference-ordered writes */
        rep->mirror_mismatch = gi_mirror(&g);
        gi_fail(&g, PS3_SPURS_STOP_GUEST_STATE, PS3_SPURS_INFO_STEP_PREFIX, n);
        return gi_done(&g);
    }
    uint8_t name[INFO_NAME_MAX];
    for (unsigned j = 0; j < n; ++j) TRY(gi_r8(&g, PS3_SPURS_INFO_STEP_PREFIX, F_PREFIX + j, &name[j]));
    for (unsigned j = 0; j < n; ++j) TRY(gi_w8(&g, PS3_SPURS_INFO_STEP_PREFIX, I_NAME + j, name[j]));
    TRY(gi_w8(&g, PS3_SPURS_INFO_STEP_PREFIX, I_NAME + n, 0));
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_PREFIX, I_NAME_LENGTH, n));
    /* Reference TODO (sys_spu_thread_group_syscall_253 for specific group types): meet then miss are plain zeros. Compatibility
     * values copied from the pinned reference, NOT deadline telemetry. */
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_COUNTERS, I_MEET, 0));
    TRY(gi_w32(&g, PS3_SPURS_INFO_STEP_COUNTERS, I_MISS, 0));
#undef TRY
    rep->mirror_mismatch = gi_mirror(&g);
    g.res = result_of(in, PS3_SPURS_OUTCOME_OK, PS3_SPURS_RC_OK, 0, PS3_SPURS_INFO_STEP_DONE, 0);
    return gi_done(&g);
}
unsigned ps3_spurs_runtime_shutdown(ps3_spurs_runtime *rt) {
    unsigned stuck = 0;
    if (!rt || !rt->initialized) return 0;
    for (unsigned i = 0; i < PS3_SPURS_MAX_INSTANCES; ++i)
        if (rt->instances[i].state != PS3_SPURS_INSTANCE_FREE) {
            if (unwind(&rt->instances[i])) ++stuck;
            else rt->instances[i].state = PS3_SPURS_INSTANCE_FREE;
        }
    return stuck;
}
