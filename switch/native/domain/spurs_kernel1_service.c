#include "spurs_kernel1_service.h"
#include <string.h>

/* CellSpurs instance offsets (relative to B). L0 = [B, B+0x80), L1 = [B+0x80, B+0x100). */
enum {
    SP_L0 = 0x00, SP_L1 = 0x80,
    SP_WKL_FLAG_RECEIVER = 0x77, SP_SYS_SRV_MESSAGE = 0x72, SP_SPU_IDLING = 0x73,
    SP_FLAGS1 = 0x74, SP_NSPUS = 0x76,
    SP_WKL_READY1 = 0x00, SP_WKL_CURRENT_CONTENTION = 0x20, SP_WKL_PENDING_CONTENTION = 0x30,
    SP_WKL_STATE1 = 0x80, SP_WKL_STATUS1 = 0x90, SP_WKL_INFO1 = 0xB00,
    SP_WKL_MIN_CONTENTION = 0x40, SP_WKL_MAX_CONTENTION = 0x50, SP_WKL_FLAG = 0x6C, SP_WKL_SIGNAL1 = 0x70,
    SP_MSG_UPDATE_WORKLOAD = 0xBD, SP_MSG_TERMINATE = 0xBF, SP_PREEMPT_WKL_ID = 0xC0,
    SP_SYS_SRV_ON_SPU = 0xC8, SP_TRACE_INITIALISED = 0xCC, SP_TRACE_NOTIFY_COMPLETE = 0xCD,
    SP_TRACE_MSG_UPDATE = 0xCE, SP_TRACE_LINE = 0x900, SP_WKL_INFO_SYS_SRV = 0xD00,
    SF1_EXIT_IF_NO_WORK = 0x80,
    SYS_SERVICE_WID = 32, SYS_SRV_IMAGE_SENTINEL = 0x100, LINE = PS3_RESERVATION_GRANULE_BYTES,
    MAX_CONDITIONAL_RETRIES = 1 << 20, POLICY_HOP_BUDGET = 1024, LS_ACTIVATION_INFO = 0x30000
};
/* SpursKernelContext in LS at 0x100 (cellSpurs.h:1198-1234). */
enum {
    LS_TRACE_LINE = 0x80, LS_CTX = 0x100, LS_CTX_END = 0x290, LS_LOC_CONTENTION = 0x180, LS_LOC_PENDING = 0x190,
    LS_PRIORITY = 0x1A0, LS_UNIQUE_ID = 0x220,
    LS_SPURS = 0x1C0, LS_SPU_NUM = 0x1C8, LS_DMA_TAG = 0x1CC, LS_WKL_ADDR = 0x1D0,
    LS_WKL_UNIQUE = 0x1D8, LS_WKL_CURRENT = 0x1DC, LS_EXIT_ADDR = 0x1E0, LS_SELECT_ADDR = 0x1E4,
    LS_MODULE_ID = 0x1E8, LS_SYS_SRV_INIT = 0x1EA, LS_SPU_IDLING = 0x1EB, LS_WKL_RUNNABLE1 = 0x1EC,
    LS_X1F0 = 0x1F0, LS_X200 = 0x200, LS_TRACE_BUFFER = 0x210, LS_TRACE_MSG_COUNT = 0x218,
    LS_TRACE_MAX_COUNT = 0x21C, LS_GUID = 0x280, LS_L1_MIRROR = 0x2D80, LS_WKL_INFO = 0x3FFE0,
    KERNEL_ENTRY_SELECT = 0x290, KERNEL_EXIT = 0x808, KERNEL_DMA_TAG = 31, POLICY_PC = 0xA00,
    POLICY_STACK = 0x3FFB0
};
static const uint32_t kernel_guid[4] = {0x423A3A02u, 0x43F43A82u, 0x43F26502u, 0x420EB382u};

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void put64(unsigned char *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint32_t get32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t get64(const unsigned char *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }
static uint64_t digest(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211u;
    return h;
}
static uint32_t load32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void bump(uint64_t *p) { __atomic_fetch_add(p, 1, __ATOMIC_RELAXED); }
static void mark(ps3_spurs_k1_context *c, unsigned step) { __atomic_fetch_or(&c->steps_completed, 1u << step, __ATOMIC_RELAXED); }

typedef struct { uint32_t reason, step; uint64_t digest; } Outcome;
static const Outcome proceed = {PS3_SPURS_K1_STOP_NONE, 0, 0};
static Outcome halt(uint32_t reason, unsigned step, const unsigned char *snapshot, size_t n) {
    Outcome o = {reason, step, snapshot ? digest(snapshot, n) : 0};
    return o;
}

static void publish_phase(ps3_spurs_k1_service *s, uint64_t phase) {
    __atomic_store_n(&s->phase, phase, __ATOMIC_SEQ_CST);
    s->config.wait_port.wake_all(s->config.wait_port.context);
}
static void publish_settled(ps3_spurs_k1_service *s) {
    __atomic_fetch_add(&s->settled, 1, __ATOMIC_SEQ_CST);
    s->config.wait_port.wake_all(s->config.wait_port.context);
}

/* A mutator inspects the reserved line snapshot and either mutates it (commit
 * follows) or returns a stop outcome, in which case nothing is stored. */
typedef Outcome (*line_mutator)(ps3_spurs_k1_service *, ps3_spurs_k1_context *, unsigned char *line);
static Outcome reservation_op(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, uint32_t offset, unsigned step,
                              line_mutator mutate, unsigned char *committed) {
    ps3_atomic_domain *d = s->config.domain;
    for (unsigned attempt = 0; attempt < MAX_CONDITIONAL_RETRIES; ++attempt) {
        unsigned char line[LINE];
        if (ps3_atomic_load_reserve128(d, c->owner, (uint64_t)s->config.instance + offset, line) != PS3_GUEST_MEMORY_OK)
            return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, step, NULL, 0);
        Outcome o = mutate(s, c, line);
        if (o.reason != PS3_SPURS_K1_STOP_NONE) {
            ps3_atomic_owner_clear_reservation(d, c->owner);
            return o;
        }
        int stored = 0;
        if (ps3_atomic_store_conditional128(d, c->owner, (uint64_t)s->config.instance + offset, line, &stored) != PS3_GUEST_MEMORY_OK)
            return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, step, line, LINE);
        if (stored) {
            if (committed) memcpy(committed, line, LINE);
            mark(c, step);
            return proceed;
        }
        bump(&c->conditional_failures);
    }
    return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, step, NULL, 0);
}

static Outcome k4_mark_on_spu(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l1) {
    (void)s;
    unsigned char bit = (unsigned char)(1u << c->spu);
    if (l1[SP_SYS_SRV_ON_SPU - SP_L1] & bit) return halt(PS3_SPURS_K1_STOP_ALREADY_INIT_HALT, 4, l1, LINE);
    l1[SP_SYS_SRV_ON_SPU - SP_L1] |= bit;
    return proceed;
}
static Outcome k5_trace_update(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l1) {
    (void)s;
    unsigned char bit = (unsigned char)(1u << c->spu);
    /* Only the arg2=1/arg3=1/forceNotify=0 init call with no pending trace update
     * message is supported; a pending message selects the notify/send_event path. */
    if (l1[SP_TRACE_MSG_UPDATE - SP_L1] & bit) return halt(PS3_SPURS_K1_STOP_SERVICE_REQUEST, 5, l1, LINE);
    l1[SP_TRACE_INITIALISED - SP_L1] = (unsigned char)((l1[SP_TRACE_INITIALISED - SP_L1] & ~bit) | bit);
    return proceed;
}
static Outcome k6_cleanup(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l1) {
    (void)s;
    if (l1[SP_PREEMPT_WKL_ID - SP_L1 + c->spu] != 0xFF) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 6, l1, LINE);
    return proceed; /* reference void reservation_op still commits the unchanged line */
}
static Outcome k8_process_requests(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l1) {
    unsigned char bit = (unsigned char)(1u << c->spu);
    if ((l1[SP_MSG_TERMINATE - SP_L1] | l1[SP_TRACE_MSG_UPDATE - SP_L1] |
        (s->policy_execution.begin ? 0 : l1[SP_MSG_UPDATE_WORKLOAD - SP_L1])) & bit)
        return halt(PS3_SPURS_K1_STOP_SERVICE_REQUEST, 8, l1, LINE);
    c->workload_update_requested = (l1[SP_MSG_UPDATE_WORKLOAD - SP_L1] & bit) != 0;
    l1[SP_MSG_UPDATE_WORKLOAD - SP_L1] &= (unsigned char)~bit;
    return proceed;
}
static Outcome activate_workload_line(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l1) {
    (void)s;
    const unsigned char bit = (unsigned char)(1u << c->spu);
    /* Shutdown completion requires a separately supplied real notification contract. */
    for (unsigned i = 0; i < 16; ++i)
        if (l1[i] > 4 || (l1[i] == 3 && (l1[SP_WKL_STATUS1 - SP_L1 + i] & bit)))
            return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 8, l1, LINE);
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char *status = &l1[SP_WKL_STATUS1 - SP_L1 + i];
        if (l1[i] == 2) *status |= bit;
        else *status &= (unsigned char)~bit;
    }
    return proceed;
}
static Outcome activate_workloads(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    unsigned char info[0x200], committed[LINE];
    if (ps3_atomic_owner_read(s->config.domain, c->owner, (uint64_t)s->config.instance + SP_WKL_INFO1, info, sizeof info) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 8, NULL, 0);
    for (unsigned i = 0; i < 16; ++i)
        if (info[i * 0x20 + 0x18 + c->spu] > 15)
            return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 8, info, sizeof info);
    Outcome o = reservation_op(s, c, SP_L1, 8, activate_workload_line, committed);
    if (o.reason) return o;
    unsigned runnable = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned priority = info[i * 0x20 + 0x18 + c->spu];
        c->workload_priority[i] = priority ? (uint8_t)(16 - priority) : 0;
        c->workload_unique_id[i] = info[i * 0x20 + 0x14];
        if (committed[i] == 2) runnable |= 0x8000u >> i;
    }
    c->ls[LS_WKL_RUNNABLE1] = (unsigned char)(runnable >> 8);
    c->ls[LS_WKL_RUNNABLE1 + 1] = (unsigned char)runnable;
    memcpy(c->ls + LS_ACTIVATION_INFO, info, sizeof info);
    memcpy(c->ls + LS_PRIORITY, c->workload_priority, 16);
    memcpy(c->ls + LS_UNIQUE_ID, c->workload_unique_id, 16);
    memcpy(c->ls + LS_L1_MIRROR, committed, LINE);
    return proceed;
}
static Outcome k9_poll_select(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned char *l0) {
    const unsigned char *ls = c->ls;
    const unsigned current = get32(ls + LS_WKL_CURRENT), poll = c->selection_is_poll;
    if ((l0[SP_SYS_SRV_MESSAGE] & (1u << c->spu)) && !s->policy_execution.begin)
        return halt(PS3_SPURS_K1_STOP_MESSAGE_WAKE, 9, l0, LINE);
    unsigned contention[16], pending[16];
    if (current != SYS_SERVICE_WID && current >= 16) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, l0, LINE);
    for (unsigned i = 0; i < 16; ++i) {
        if (l0[SP_WKL_CURRENT_CONTENTION + i] < ls[LS_LOC_CONTENTION + i] ||
            l0[SP_WKL_PENDING_CONTENTION + i] < ls[LS_LOC_PENDING + i])
            return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, l0, LINE);
        contention[i] = l0[SP_WKL_CURRENT_CONTENTION + i] - ls[LS_LOC_CONTENTION + i];
        pending[i] = l0[SP_WKL_PENDING_CONTENTION + i] - ls[LS_LOC_PENDING + i];
        if (poll && i != current) contention[i] += pending[i];
        if (contention[i] > PS3_SPURS_K1_MAX_SPUS || pending[i] > PS3_SPURS_K1_MAX_SPUS)
            return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, l0, LINE);
    }
    unsigned selected = SYS_SERVICE_WID, status = 0, max_weight = 0;
    const unsigned bit = 1u << c->spu;
    if (l0[SP_SYS_SRV_MESSAGE] & bit) {
        if (!poll || current == SYS_SERVICE_WID) l0[SP_SYS_SRV_MESSAGE] &= (unsigned char)~bit;
    } else {
        const unsigned runnable = (unsigned)ls[LS_WKL_RUNNABLE1] << 8 | ls[LS_WKL_RUNNABLE1 + 1];
        const unsigned signal = (unsigned)l0[SP_WKL_SIGNAL1] << 8 | l0[SP_WKL_SIGNAL1 + 1];
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned signalled = (signal & (0x8000u >> i)) != 0;
            const unsigned flag = get32(l0 + SP_WKL_FLAG) == 0 && l0[SP_WKL_FLAG_RECEIVER] == i;
            const unsigned ready = l0[i] > 8 ? 8 : l0[i];
            const unsigned idle = l0[0x10 + i] > 8 ? 8 : l0[0x10 + i];
            if (!(runnable & (0x8000u >> i)) || !ls[LS_PRIORITY + i] || l0[SP_WKL_MAX_CONTENTION + i] <= contention[i] ||
                !(flag || signalled || (ready && ready + idle > contention[i]))) continue;
            unsigned weight = (flag || signalled || ready > contention[i]) ? 0x8000 : 0;
            weight |= (ls[LS_PRIORITY + i] & 0x7fu) << 8;
            weight |= i == current ? 0x80 : 0;
            weight |= contention[i] && l0[SP_WKL_MIN_CONTENTION + i] > contention[i] ? 0x40 : 0;
            weight |= ((8 - contention[i]) & 15u) << 2;
            weight |= ls[LS_UNIQUE_ID + i] == current ? 2 : 0;
            weight |= 1;
            if (weight > max_weight) { selected = i; max_weight = weight; status = (ready > contention[i] ? 1u : 0u) | (signalled ? 2u : 0u) | (flag ? 4u : 0u); }
        }
        if ((!poll || selected == current) && selected < 16) {
            const unsigned remaining = signal & ~(0x8000u >> selected);
            l0[SP_WKL_SIGNAL1] = (unsigned char)(remaining >> 8); l0[SP_WKL_SIGNAL1 + 1] = (unsigned char)remaining;
            if (selected == l0[SP_WKL_FLAG_RECEIVER]) put32(l0 + SP_WKL_FLAG, UINT32_MAX);
        } else if (selected == SYS_SERVICE_WID && l0[SP_WKL_FLAG_RECEIVER] == SYS_SERVICE_WID) {
            return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, l0, LINE);
        }
    }
    if (selected < 16 && !s->policy_execution.begin)
        return halt(PS3_SPURS_K1_STOP_DISPATCH_REQUIRED, 9, l0, LINE);
    memset(c->selected_contention, 0, 16); memset(c->selected_pending, 0, 16);
    if (!poll) {
        if (selected < 16) ++contention[selected];
        for (unsigned i = 0; i < 16; ++i) { l0[SP_WKL_CURRENT_CONTENTION + i] = (unsigned char)contention[i]; l0[SP_WKL_PENDING_CONTENTION + i] = (unsigned char)pending[i]; }
        if (selected < 16) c->selected_contention[selected] = 1;
    } else {
        if (selected != current && selected < 16) ++pending[selected];
        for (unsigned i = 0; i < 16; ++i) l0[SP_WKL_PENDING_CONTENTION + i] = (unsigned char)pending[i];
        if (selected != current && selected < 16) c->selected_pending[selected] = 1;
        memcpy(c->selected_contention, ls + LS_LOC_CONTENTION, 16);
    }
    c->selected_workload = selected; c->selected_poll_status = status;
    return proceed;
}
static Outcome select_workload(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, unsigned poll) {
    c->selection_is_poll = poll;
    Outcome o = reservation_op(s, c, SP_L0, 9, k9_poll_select, c->ls + LS_CTX);
    if (o.reason) return o;
    memcpy(c->ls + LS_LOC_CONTENTION, c->selected_contention, 16);
    memcpy(c->ls + LS_LOC_PENDING, c->selected_pending, 16);
    if (!poll) put32(c->ls + LS_WKL_CURRENT, c->selected_workload);
    c->ls[LS_SPU_IDLING] = c->selected_workload == SYS_SERVICE_WID;
    return proceed;
}

typedef enum { IDLE_BLOCK, IDLE_PUBLISHED, IDLE_SERVICE } IdleDecision;
/* K11/K12/K13 one pass on a fresh L0 reservation snapshot held in LS 0x100. */
static Outcome idle_pass(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, IdleDecision *decision) {
    ps3_atomic_domain *d = s->config.domain;
    unsigned char *snap = c->ls + LS_CTX;
    int lost = 0;
    ps3_atomic_owner_take_reservation_loss(d, c->owner, &lost); /* stale latches belong to older reservations */
    if (ps3_atomic_load_reserve128(d, c->owner, s->config.instance, snap) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 11, NULL, 0);
    mark(c, 11);
    unsigned char bit = (unsigned char)(1u << c->spu);
    unsigned idle = (unsigned)__builtin_popcount(snap[SP_SPU_IDLING]);
    /* K4 (root): every fresh L0 snapshot is revalidated against the supported shape. Fence first:
     * nothing below has stored anything for this pass. Permitted no-work writes stay valid. */
    if (snap[SP_NSPUS] != s->config.spu_count || (snap[SP_FLAGS1] & ~SF1_EXIT_IF_NO_WORK)) {
        ps3_atomic_owner_clear_reservation(d, c->owner);
        return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 11, snap, LINE);
    }
    int should_exit = idle == snap[SP_NSPUS] && (snap[SP_FLAGS1] & SF1_EXIT_IF_NO_WORK);
    int message = (snap[SP_SYS_SRV_MESSAGE] & bit) != 0;
    /* Fence: leaving transitions are decided on the snapshot, before any PUTLLC. */
    if (message && !should_exit) {
        ps3_atomic_owner_clear_reservation(d, c->owner);
        if (!s->policy_execution.begin) return halt(PS3_SPURS_K1_STOP_MESSAGE_WAKE, 11, snap, LINE);
        *decision = IDLE_SERVICE; return proceed;
    }
    if (s->policy_execution.begin) {
        unsigned char selection_snapshot[LINE];
        memcpy(selection_snapshot, snap, LINE);
        c->selection_is_poll = 1;
        Outcome selection = k9_poll_select(s, c, selection_snapshot);
        if (selection.reason) { ps3_atomic_owner_clear_reservation(d, c->owner); return selection; }
        if (c->selected_workload < 16) {
            ps3_atomic_owner_clear_reservation(d, c->owner);
            *decision = IDLE_SERVICE; return proceed;
        }
    }
    if (should_exit) { ps3_atomic_owner_clear_reservation(d, c->owner); return halt(PS3_SPURS_K1_STOP_EXIT_REQUESTED, 11, snap, LINE); }
    int was_idle = (snap[SP_SPU_IDLING] & bit) != 0;
    if (was_idle) {               /* K13: block holding the L0 reservation, no commit */
        *decision = IDLE_BLOCK;
        mark(c, 13);
        return proceed;
    }
    snap[SP_SPU_IDLING] |= bit;   /* K12 */
    int stored = 0;
    if (ps3_atomic_store_conditional128(d, c->owner, s->config.instance, snap, &stored) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 12, snap, LINE);
    if (stored) { bump(&c->idle_publications); mark(c, 12); }
    else bump(&c->conditional_failures);
    *decision = IDLE_PUBLISHED;
    return proceed;
}
static Outcome idle_until_blocked(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    for (unsigned attempt = 0; attempt < MAX_CONDITIONAL_RETRIES; ++attempt) {
        IdleDecision decision = IDLE_PUBLISHED;
        Outcome o = idle_pass(s, c, &decision);
        if (o.reason != PS3_SPURS_K1_STOP_NONE) return o;
        if (decision == IDLE_SERVICE) { store32(&c->state, PS3_SPURS_K1_CTX_RUNNING); return proceed; }
        if (decision == IDLE_BLOCK) { store32(&c->state, PS3_SPURS_K1_CTX_BLOCKED_LR); return proceed; }
    }
    return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 12, NULL, 0);
}

static int policy_select(void *context, uint32_t spu, unsigned poll, uint64_t *packed) {
    ps3_spurs_k1_service *s = context;
    if (!packed || spu >= s->config.spu_count || poll > 1) return PS3_SPURS_POLICY_FAULT;
    if (ps3_notify_cancel_is_requested(&s->cancel)) return PS3_SPURS_POLICY_CANCELLED;
    ps3_spurs_k1_context *c = &s->contexts[spu];
    Outcome o = select_workload(s, c, poll);
    if (o.reason) return PS3_SPURS_POLICY_FAULT;
    *packed = (uint64_t)c->selected_workload << 32 | c->selected_poll_status;
    return 0;
}
static Outcome dispatch_policy(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    unsigned char info[0x20];
    if (c->selected_workload >= 16 || !s->policy_execution.begin)
        return halt(PS3_SPURS_K1_STOP_DISPATCH_REQUIRED, 9, NULL, 0);
    if (ps3_atomic_owner_read(s->config.domain, c->owner, (uint64_t)s->config.instance + SP_WKL_INFO1 + 0x20 * c->selected_workload, info, sizeof info) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 9, NULL, 0);
    const uint64_t address = get64(info);
    const uint32_t size = get32(info + 0x10);
    if (!address || address > UINT32_MAX || address % 16 || !size || size > PS3_SPURS_K1_LS_BYTES - POLICY_PC || address + size > PS3_GUEST_ADDRESS_BYTES)
        return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, info, sizeof info);
    const unsigned module_reloaded = get64(c->ls + LS_WKL_ADDR) != address;
    if (module_reloaded) {
        if (ps3_atomic_owner_read(s->config.domain, c->owner, address, c->ls + POLICY_PC, size) != PS3_GUEST_MEMORY_OK)
            return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 9, info, sizeof info);
        put64(c->ls + LS_WKL_ADDR, address); put32(c->ls + LS_WKL_UNIQUE, info[0x14]);
    }
    memcpy(c->ls + LS_WKL_INFO, info, sizeof info);
    c->ls[LS_MODULE_ID] = c->ls[LS_MODULE_ID + 1] = 0;
    ps3_spurs_policy_execution_request request = {
        .domain = s->config.domain, .owner = c->owner, .local_store = c->ls, .local_store_size = PS3_SPURS_K1_LS_BYTES,
        .instance_address = s->config.instance, .spu_index = c->spu, .workload_id = c->selected_workload,
        .module_address = (uint32_t)address, .module_size = size, .unique_id = info[0x14],
        .module_reloaded = module_reloaded,
        .poll_status = c->selected_poll_status, .data = get64(info + 8),
        .selection_context = s, .select = policy_select
    };
    ps3_spurs_policy_execution_result result = s->policy_execution.begin(s->policy_execution.context, &request);
    if (result.status != PS3_SPURS_POLICY_READY) return halt(PS3_SPURS_K1_STOP_DISPATCH_REQUIRED, 9, info, sizeof info);
    c->policy_active = 1; store32(&c->state, PS3_SPURS_K1_CTX_RUNNING);
    return proceed;
}
static Outcome run_system_cycle(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    put64(c->ls + LS_WKL_ADDR, SYS_SRV_IMAGE_SENTINEL);
    Outcome o = reservation_op(s, c, SP_L1, 8, k8_process_requests, c->ls + LS_L1_MIRROR);
    if (o.reason) return o;
    if (c->workload_update_requested) { o = activate_workloads(s, c); if (o.reason) return o; }
    o = select_workload(s, c, 1);
    if (o.reason) return o;
    if (c->selected_workload < 16) {
        o = select_workload(s, c, 0);
        if (o.reason) return o;
        if (c->selected_workload < 16) return dispatch_policy(s, c);
    }
    mark(c, 10);
    return idle_until_blocked(s, c);
}
static Outcome step_policy(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    ps3_spurs_policy_execution_result result = s->policy_execution.step(s->policy_execution.context, c->spu, POLICY_HOP_BUDGET);
    if (result.status == PS3_SPURS_POLICY_CANCELLED && ps3_notify_cancel_is_requested(&s->cancel)) return proceed;
    if (result.status == PS3_SPURS_POLICY_SLICE_CONTINUE && result.resumable) return proceed;
    if (result.status == PS3_SPURS_POLICY_KERNEL_SELECT && result.resumable) {
        Outcome o = select_workload(s, c, 1);
        if (o.reason) return o;
        result = s->policy_execution.resume(s->policy_execution.context, c->spu, (uint64_t)c->selected_workload << 32 | c->selected_poll_status);
        if (result.status == PS3_SPURS_POLICY_READY) return proceed;
    } else if (result.status == PS3_SPURS_POLICY_KERNEL_EXIT) {
        if (s->policy_execution.release(s->policy_execution.context, c->spu) != PS3_SPURS_POLICY_READY)
            return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 9, NULL, 0);
        c->policy_active = 0;
        Outcome o = select_workload(s, c, 0);
        if (o.reason) return o;
        if (c->selected_workload < 16) return dispatch_policy(s, c);
        return run_system_cycle(s, c);
    }
    return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 9, NULL, 0);
}

static Outcome run_context(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c) {
    ps3_atomic_domain *d = s->config.domain;
    unsigned char *ls = c->ls;
    const uint32_t b = s->config.instance;
    store32(&c->state, PS3_SPURS_K1_CTX_RUNNING);
    /* K1 spursKernelEntry */
    unsigned char flags1 = 0;
    if (ps3_atomic_owner_read(d, c->owner, (uint64_t)b + SP_FLAGS1, &flags1, 1) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 1, NULL, 0);
    if (flags1 & ~SF1_EXIT_IF_NO_WORK) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 1, &flags1, 1);
    memset(ls + LS_CTX, 0, LS_CTX_END - LS_CTX);
    put64(ls + LS_SPURS, b);
    put32(ls + LS_SPU_NUM, c->spu);
    put32(ls + LS_DMA_TAG, KERNEL_DMA_TAG);
    put32(ls + LS_WKL_UNIQUE, SYS_SERVICE_WID);
    put32(ls + LS_WKL_CURRENT, SYS_SERVICE_WID);
    put32(ls + LS_EXIT_ADDR, KERNEL_EXIT);
    put32(ls + LS_SELECT_ADDR, KERNEL_ENTRY_SELECT);
    put32(ls + LS_X1F0, 0xF0020000u);
    put32(ls + LS_X200, 0x00020000u);
    for (unsigned i = 0; i < 4; ++i) put32(ls + LS_GUID + 4 * i, kernel_guid[i]);
    mark(c, 1);
    /* K2 DispatchWorkload(wid 32) */
    if (ps3_atomic_owner_read(d, c->owner, (uint64_t)b + SP_FLAGS1, &flags1, 1) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 2, NULL, 0);
    if (flags1 & ~SF1_EXIT_IF_NO_WORK) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 2, &flags1, 1);
    unsigned char info[0x20];
    if (ps3_atomic_owner_read(d, c->owner, (uint64_t)b + SP_WKL_INFO_SYS_SRV, info, sizeof info) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 2, NULL, 0);
    if (get64(info) != SYS_SRV_IMAGE_SENTINEL) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 2, info, sizeof info);
    memcpy(ls + LS_WKL_INFO, info, sizeof info);
    put64(ls + LS_WKL_ADDR, get64(info));
    put32(ls + LS_WKL_UNIQUE, info[0x14]);
    ls[LS_MODULE_ID] = ls[LS_MODULE_ID + 1] = 0;
    const uint32_t regs[6] = {KERNEL_EXIT, POLICY_STACK, SYS_SRV_IMAGE_SENTINEL, 0, 0, POLICY_PC};
    memcpy(c->regs, regs, sizeof regs);
    mark(c, 2);
    /* K3 SysServiceEntry -> Main */
    if (get32(ls + LS_WKL_CURRENT) != SYS_SERVICE_WID || get64(ls + LS_SPURS) % LINE)
        return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 3, NULL, 0);
    ls[LS_SYS_SRV_INIT] = 1;
    mark(c, 3);
    /* K4 */
    Outcome o = reservation_op(s, c, SP_L1, 4, k4_mark_on_spu, ls + LS_L1_MIRROR);
    if (o.reason) return o;
    put64(ls + LS_TRACE_BUFFER, 0);
    put32(ls + LS_TRACE_MSG_COUNT, 0xFFFFFFFFu);
    /* K5 TraceUpdate(1,1,0) */
    o = reservation_op(s, c, SP_L1, 5, k5_trace_update, ls + LS_L1_MIRROR);
    if (o.reason) return o;
    /* K5b: arg3=1 reads the trace line (intent GETLLAR into LS 0x80) */
    if (ps3_atomic_load_reserve128(d, c->owner, (uint64_t)b + SP_TRACE_LINE, ls + LS_TRACE_LINE) != PS3_GUEST_MEMORY_OK)
        return halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 5, NULL, 0);
    ps3_atomic_owner_clear_reservation(d, c->owner);
    const unsigned char *trace = ls + LS_TRACE_LINE;
    if (get64(trace)) return halt(PS3_SPURS_K1_STOP_TRACE_BUFFER_NONZERO, 5, trace, LINE);
    const uint32_t start_index = get32(trace + 8 + 4 * c->spu), max_count = get32(trace + 12) - get32(trace + 8);
    if (start_index || max_count) return halt(PS3_SPURS_K1_STOP_UNSUPPORTED_STATE, 5, trace, LINE);
    put64(ls + LS_TRACE_BUFFER, 0);
    put32(ls + LS_TRACE_MAX_COUNT, 0);
    put32(ls + LS_TRACE_MSG_COUNT, 0);
    /* K6 CleanupAfterSystemWorkload */
    o = reservation_op(s, c, SP_L1, 6, k6_cleanup, NULL);
    if (o.reason) return o;
    /* K7 PutTrace INIT/START: no guest effect while traceBuffer == 0 */
    mark(c, 7);
    return run_system_cycle(s, c);
}

static void finish_stopped(ps3_spurs_k1_service *s, ps3_spurs_k1_context *c, Outcome o) {
    store32(&c->state, PS3_SPURS_K1_CTX_STOPPED);
    s->stop = (ps3_spurs_k1_stop){o.reason, c->spu, o.step, o.digest};
    publish_phase(s, PS3_SPURS_K1_PHASE_STOPPED);
}
static void finish_cancelled(ps3_spurs_k1_service *s) {
    for (uint32_t i = 0; i < s->config.spu_count; ++i) store32(&s->contexts[i].state, PS3_SPURS_K1_CTX_CANCELLED);
    publish_phase(s, PS3_SPURS_K1_PHASE_CANCELLED);
}
static void release_policy_sessions(ps3_spurs_k1_service *s) {
    /* Request cancellation for every live lane before releasing any LS owner. */
    for (uint32_t i = 0; i < s->config.spu_count; ++i)
        if (s->contexts[i].policy_active) s->policy_execution.cancel(s->policy_execution.context, i);
    for (uint32_t i = 0; i < s->config.spu_count; ++i) {
        ps3_spurs_k1_context *c = &s->contexts[i];
        if (!c->policy_active) continue;
        if (s->policy_execution.release(s->policy_execution.context, i) == PS3_SPURS_POLICY_READY)
            c->policy_active = 0;
    }
}

static void worker_main(void *argument) {
    ps3_spurs_k1_service *s = argument;
    const uint32_t n = s->config.spu_count;
    for (uint32_t k = 0; k < n; ++k) {
        if (ps3_notify_cancel_is_requested(&s->cancel)) { release_policy_sessions(s); finish_cancelled(s); return; }
        ps3_spurs_k1_context *c = &s->contexts[s->order[k]];
        Outcome o = run_context(s, c);
        if (o.reason) { release_policy_sessions(s); finish_stopped(s, c, o); return; }
    }
    ps3_atomic_owner owners[PS3_SPURS_K1_MAX_SPUS];
    for (uint32_t i = 0; i < n; ++i) owners[i] = s->contexts[i].owner;
    uint32_t rotation = 0;
    for (;;) {
        if (ps3_notify_cancel_is_requested(&s->cancel)) { release_policy_sessions(s); finish_cancelled(s); return; }
        int reran = 0;
        for (uint32_t k = 0; k < n; ++k) {
            ps3_spurs_k1_context *c = &s->contexts[(rotation + k) % n];
            if (c->policy_active || load32(&c->state) == PS3_SPURS_K1_CTX_RUNNING) {
                reran = 1;
                Outcome o = c->policy_active ? step_policy(s, c) : run_system_cycle(s, c);
                if (o.reason) { release_policy_sessions(s); finish_stopped(s, c, o); return; }
                continue;
            }
            int lost = 0;
            if (ps3_atomic_owner_take_reservation_loss(s->config.domain, c->owner, &lost) != PS3_GUEST_MEMORY_OK) {
                finish_stopped(s, c, halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 13, NULL, 0));
                return;
            }
            if (!lost) continue;
            reran = 1;
            bump(&c->wake_evaluations);
            store32(&c->state, PS3_SPURS_K1_CTX_RUNNING);
            Outcome o = idle_until_blocked(s, c);
            if (o.reason) { release_policy_sessions(s); finish_stopped(s, c, o); return; }
        }
        if (reran) { rotation = (rotation + 1) % n; continue; }
        if (__atomic_load_n(&s->phase, __ATOMIC_SEQ_CST) != PS3_SPURS_K1_PHASE_QUIESCENT)
            publish_phase(s, PS3_SPURS_K1_PHASE_QUIESCENT);
        publish_settled(s);
        uint64_t mask = 0;
        ps3_atomic_wait_result r = ps3_atomic_owners_wait_reservation_loss(s->config.domain, owners, n, UINT64_MAX, &s->cancel, &mask);
        if (r == PS3_ATOMIC_WAIT_CANCELLED) { finish_cancelled(s); return; }
        if (r != PS3_ATOMIC_WAIT_LOST) { finish_stopped(s, &s->contexts[0], halt(PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 13, NULL, 0)); return; }
        rotation = (uint32_t)__builtin_ctzll(mask) % n;
    }
}

ps3_spurs_k1_status ps3_spurs_k1_init(ps3_spurs_k1_service *s, const ps3_spurs_k1_config *input) {
    if (!s || !input) return PS3_SPURS_K1_INVALID;
    /* Storage contract: *s is zero-filled or a previously initialised service. A service whose
     * worker was started and not yet confirmed joined is live and is never reinitialised. */
    if (s->initialized && s->worker_started && !s->worker_joined) return PS3_SPURS_K1_INVALID;
    if (s->initialized)
        for (uint32_t i = 0; i < s->config.spu_count; ++i)
            if (s->contexts[i].policy_active) return PS3_SPURS_K1_INVALID;
    const ps3_spurs_k1_config config = *input;   /* snapshot: `input` may alias s->config */
    if (!config.domain || !config.wait_port.wait || !config.wait_port.wake_all ||
        !config.worker.start || !config.worker.join || !config.owners || !config.local_stores ||
        config.spu_count == 0 || config.spu_count > PS3_SPURS_K1_MAX_SPUS || config.instance % LINE ||
        (uint64_t)config.instance + 0x1000 > PS3_GUEST_ADDRESS_BYTES || config.domain->wait_port.wait != config.wait_port.wait ||
        config.domain->wait_port.context != config.wait_port.context)
        return PS3_SPURS_K1_INVALID;
    uint8_t seen = 0, order_of[PS3_SPURS_K1_MAX_SPUS];
    ps3_spurs_k1_context built[PS3_SPURS_K1_MAX_SPUS];
    memset(order_of, 0, sizeof order_of);
    memset(built, 0, sizeof built);
    for (uint32_t i = 0; i < config.spu_count; ++i) {   /* validate everything before mutating *s */
        uint8_t order = config.start_order ? config.start_order[i] : (uint8_t)i;
        if (order >= config.spu_count || (seen & (1u << order)) || !config.local_stores[i]) return PS3_SPURS_K1_INVALID;
        for (uint32_t j = 0; j < i; ++j)
            if (config.owners[j].slot == config.owners[i].slot && config.owners[j].domain == config.owners[i].domain)
                return PS3_SPURS_K1_INVALID;
        seen |= (uint8_t)(1u << order);
        order_of[i] = order;
        built[i] = (ps3_spurs_k1_context){.owner = config.owners[i], .ls = config.local_stores[i], .spu = i};
    }
    memset(s, 0, sizeof *s);
    memcpy(s->order, order_of, sizeof order_of);
    memcpy(s->contexts, built, sizeof built);
    s->config = config;
    s->initialized = 1;
    return PS3_SPURS_K1_OK;
}
ps3_spurs_k1_status ps3_spurs_k1_set_policy_execution(ps3_spurs_k1_service *s, const ps3_spurs_policy_execution_port *port) {
    if (!s || !s->initialized || s->worker_started || !port || !port->begin || !port->step || !port->resume || !port->cancel || !port->release)
        return PS3_SPURS_K1_INVALID;
    s->policy_execution = *port;
    return PS3_SPURS_K1_OK;
}
static ps3_spurs_k1_status status_of_phase(uint64_t phase) {
    return phase == PS3_SPURS_K1_PHASE_QUIESCENT ? PS3_SPURS_K1_OK
         : phase == PS3_SPURS_K1_PHASE_CANCELLED ? PS3_SPURS_K1_CANCELLED : PS3_SPURS_K1_STOPPED;
}
ps3_spurs_k1_status ps3_spurs_k1_start(ps3_spurs_k1_service *s, ps3_spurs_k1_stop *stop) {
    if (!s || !s->initialized || s->worker_started || __atomic_load_n(&s->phase, __ATOMIC_SEQ_CST) != PS3_SPURS_K1_PHASE_CREATED)
        return PS3_SPURS_K1_INVALID;
    __atomic_store_n(&s->phase, PS3_SPURS_K1_PHASE_RUNNING, __ATOMIC_SEQ_CST);
    if (s->config.worker.start(s->config.worker.context, worker_main, s) != 0) {
        s->stop = (ps3_spurs_k1_stop){PS3_SPURS_K1_STOP_WORKER_START_FAILED, 0, 0, 0};
        __atomic_store_n(&s->phase, PS3_SPURS_K1_PHASE_STOPPED, __ATOMIC_SEQ_CST);
        if (stop) *stop = s->stop;
        return PS3_SPURS_K1_STOPPED;
    }
    s->worker_started = 1;
    uint64_t phase;
    while ((phase = __atomic_load_n(&s->phase, __ATOMIC_SEQ_CST)) == PS3_SPURS_K1_PHASE_RUNNING)
        if (s->config.wait_port.wait(s->config.wait_port.context, &s->phase, phase, NULL, UINT64_MAX) == PS3_NOTIFY_FAILURE) {
            /* Cannot observe quiescence: stop the worker, then report a terminal provider failure. */
            ps3_spurs_k1_cancel_and_join(s);
            s->stop = (ps3_spurs_k1_stop){PS3_SPURS_K1_STOP_PROVIDER_FAILURE, 0, 0, 0};
            __atomic_store_n(&s->phase, PS3_SPURS_K1_PHASE_STOPPED, __ATOMIC_SEQ_CST);
            if (stop) *stop = s->stop;
            return PS3_SPURS_K1_STOPPED;
        }
    if (phase == PS3_SPURS_K1_PHASE_STOPPED && stop) *stop = s->stop;
    return status_of_phase(phase);
}
int ps3_spurs_k1_poll_stop(const ps3_spurs_k1_service *s, ps3_spurs_k1_stop *stop) {
    if (!s || __atomic_load_n(&s->phase, __ATOMIC_SEQ_CST) != PS3_SPURS_K1_PHASE_STOPPED) return 0;
    if (stop) *stop = s->stop;
    return 1;
}
uint64_t ps3_spurs_k1_settled_count(const ps3_spurs_k1_service *s) { return __atomic_load_n(&s->settled, __ATOMIC_SEQ_CST); }
ps3_spurs_k1_phase ps3_spurs_k1_phase_of(const ps3_spurs_k1_service *s) { return (ps3_spurs_k1_phase)__atomic_load_n(&s->phase, __ATOMIC_SEQ_CST); }
ps3_spurs_k1_phase ps3_spurs_k1_wait_settled_after(ps3_spurs_k1_service *s, uint64_t after, uint64_t deadline_ns) {
    for (;;) {
        uint64_t settled = __atomic_load_n(&s->settled, __ATOMIC_SEQ_CST);
        ps3_spurs_k1_phase phase = ps3_spurs_k1_phase_of(s);
        if (settled > after || phase == PS3_SPURS_K1_PHASE_STOPPED || phase == PS3_SPURS_K1_PHASE_CANCELLED) return phase;
        ps3_notify_wait_result waited = s->config.wait_port.wait(s->config.wait_port.context, &s->settled, settled, NULL, deadline_ns);
        if (waited == PS3_NOTIFY_FAILURE ||
            (waited == PS3_NOTIFY_TIMEOUT && __atomic_load_n(&s->settled, __ATOMIC_SEQ_CST) == settled))
            return ps3_spurs_k1_phase_of(s);
    }
}
ps3_spurs_k1_context_state ps3_spurs_k1_context_state_of(const ps3_spurs_k1_service *s, uint32_t spu) {
    return spu < s->config.spu_count ? (ps3_spurs_k1_context_state)load32(&s->contexts[spu].state) : PS3_SPURS_K1_CTX_NOT_STARTED;
}
uint64_t ps3_spurs_k1_counter(const ps3_spurs_k1_service *s, uint32_t spu, int which) {
    if (spu >= s->config.spu_count) return 0;
    const ps3_spurs_k1_context *c = &s->contexts[spu];
    switch (which) {
    case PS3_SPURS_K1_COUNT_IDLE_PUBLICATIONS: return __atomic_load_n(&c->idle_publications, __ATOMIC_RELAXED);
    case PS3_SPURS_K1_COUNT_WAKE_EVALUATIONS: return __atomic_load_n(&c->wake_evaluations, __ATOMIC_RELAXED);
    case PS3_SPURS_K1_COUNT_CONDITIONAL_FAILURES: return __atomic_load_n(&c->conditional_failures, __ATOMIC_RELAXED);
    case PS3_SPURS_K1_COUNT_STEPS: return load32(&c->steps_completed);
    default: return 0;
    }
}
int ps3_spurs_k1_cancel_and_join(ps3_spurs_k1_service *s) {
    if (!s || !s->initialized) return 0;
    ps3_notify_cancel_request(&s->cancel, &s->config.wait_port);
    if (s->worker_started && !s->worker_joined) {
        /* Only a confirmed join ends the worker's lifetime; on failure everything stays owned. */
        if (s->config.worker.join(s->config.worker.context) != 0) return -1;
        s->worker_joined = 1;
    }
    release_policy_sessions(s);
    for (uint32_t i = 0; i < s->config.spu_count; ++i)
        if (s->contexts[i].policy_active) return -1;
    for (uint32_t i = 0; i < s->config.spu_count; ++i) {
        uint32_t st = load32(&s->contexts[i].state);
        if (st == PS3_SPURS_K1_CTX_RUNNING || st == PS3_SPURS_K1_CTX_BLOCKED_LR) store32(&s->contexts[i].state, PS3_SPURS_K1_CTX_CANCELLED);
        ps3_atomic_owner_clear_reservation(s->config.domain, s->contexts[i].owner);
    }
    return 0;
}
