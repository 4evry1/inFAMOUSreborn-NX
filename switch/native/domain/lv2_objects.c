#define _POSIX_C_SOURCE 200809L
#include "lv2_objects.h"
#include <string.h>
#include <time.h>

static const uint32_t kind_base[4] = {PS3_LV2_SEMAPHORE_BASE, PS3_LV2_QUEUE_BASE, PS3_LV2_PORT_BASE,
    PS3_LV2_EVENT_FLAG_BASE};
static const uint32_t kind_cap[4] = {PS3_LV2_SEMAPHORE_CAP, PS3_LV2_QUEUE_CAP, PS3_LV2_PORT_CAP,
    PS3_LV2_EVENT_FLAG_CAP};
static void lock(ps3_lv2_objects *o) { o->guard.lock(o->guard.context); }
static uint32_t unlock(ps3_lv2_objects *o, uint32_t rc) { o->guard.unlock(o->guard.context); return rc; }
static void wake(ps3_lv2_objects *o) { o->port.wake_all(o->port.context); }

uint32_t ps3_lv2_objects_init(ps3_lv2_objects *o, ps3_instance_guard guard, ps3_notify_port port) {
    if (!o || !guard.lock || !guard.unlock || !port.wait || !port.wake_all) return PS3_LV2_EINVAL;
    if (o->initialized) return PS3_LV2_EINVAL;   /* one-shot on zeroed storage; never reset live state */
    memset(o, 0, sizeof *o);
    o->guard = guard;
    o->port = port;
    for (int k = 0; k < 4; ++k) o->capacity_limit[k] = kind_cap[k];
    o->initialized = 1;
    return PS3_LV2_OK;
}
void ps3_lv2_objects_limit(ps3_lv2_objects *o, int kind, uint32_t live_capacity) {
    lock(o);
    if (kind >= 0 && kind < 4) o->capacity_limit[kind] = live_capacity < kind_cap[kind] ? live_capacity : kind_cap[kind];
    unlock(o, 0);
}
static size_t live(ps3_lv2_objects *o, int kind) {
    size_t n = 0;
    if (kind == PS3_LV2_KIND_SEMAPHORE) { for (size_t i = 0; i < PS3_LV2_SEMAPHORE_CAP; ++i) n += o->semaphores[i].active; }
    else if (kind == PS3_LV2_KIND_QUEUE) { for (size_t i = 0; i < PS3_LV2_QUEUE_CAP; ++i) n += o->queues[i].active; }
    else if (kind == PS3_LV2_KIND_PORT) { for (size_t i = 0; i < PS3_LV2_PORT_CAP; ++i) n += o->ports[i].active; }
    else { for (size_t i = 0; i < PS3_LV2_EVENT_FLAG_CAP; ++i) n += o->event_flags[i].active; }
    return n;
}
size_t ps3_lv2_live_count(ps3_lv2_objects *o, int kind) { lock(o); size_t n = live(o, kind); unlock(o, 0); return n; }
/* Under the guard: next never-reused ID, or 0 when exhausted (EAGAIN). */
static uint32_t allocate_id(ps3_lv2_objects *o, int kind) {
    if (live(o, kind) >= o->capacity_limit[kind] || o->serial[kind] >= 0xFFFFFFu) return 0;
    return kind_base[kind] + PS3_LV2_ID_STEP * o->serial[kind]++;
}
static ps3_lv2_semaphore *find_semaphore(ps3_lv2_objects *o, uint32_t id) {
    for (size_t i = 0; i < PS3_LV2_SEMAPHORE_CAP; ++i) if (o->semaphores[i].active && o->semaphores[i].id == id) return &o->semaphores[i];
    return NULL;
}
static ps3_lv2_queue *find_queue(ps3_lv2_objects *o, uint32_t id) {
    for (size_t i = 0; i < PS3_LV2_QUEUE_CAP; ++i) if (o->queues[i].active && o->queues[i].id == id) return &o->queues[i];
    return NULL;
}
static ps3_lv2_event_flag *find_event_flag(ps3_lv2_objects *o, uint32_t id) {
    for (size_t i = 0; i < PS3_LV2_EVENT_FLAG_CAP; ++i)
        if (o->event_flags[i].active && o->event_flags[i].id == id) return &o->event_flags[i];
    return NULL;
}
/* MONOTONIC now in nanoseconds. A failing clock is a provider failure, never a fabricated 0. */
static int now_ns(uint64_t *out) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    *out = (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
    return 1;
}
/* Microsecond timeout -> MONOTONIC deadline. 0 means "no deadline" (block forever), as lv2 does. */
static int deadline_from_us(uint64_t timeout_us, uint64_t *deadline) {
    if (!timeout_us) { *deadline = UINT64_MAX; return 1; }
    uint64_t now = 0;
    if (!now_ns(&now)) return 0;
    const uint64_t add = timeout_us > (UINT64_MAX - now) / 1000u ? UINT64_MAX - now : timeout_us * 1000u;
    *deadline = now + add;
    return 1;
}

static ps3_lv2_port *find_port(ps3_lv2_objects *o, uint32_t id) {
    for (size_t i = 0; i < PS3_LV2_PORT_CAP; ++i) if (o->ports[i].active && o->ports[i].id == id) return &o->ports[i];
    return NULL;
}

uint32_t ps3_lv2_semaphore_create(ps3_lv2_objects *o, uint32_t *id, const ps3_lv2_semaphore_attr *attr, int32_t initial, int32_t max) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!id || !attr) return PS3_LV2_EFAULT;
    if (max <= 0 || initial > max || initial < 0) return PS3_LV2_EINVAL;
    if (attr->protocol != PS3_SYNC_FIFO && attr->protocol != PS3_SYNC_PRIORITY) return PS3_LV2_EINVAL;
    lock(o);
    uint32_t new_id = allocate_id(o, PS3_LV2_KIND_SEMAPHORE);
    if (!new_id) return unlock(o, PS3_LV2_EAGAIN);
    for (size_t i = 0; i < PS3_LV2_SEMAPHORE_CAP; ++i) if (!o->semaphores[i].active) {
        o->semaphores[i] = (ps3_lv2_semaphore){new_id, attr->protocol, initial, max, 1, 0};
        break;
    }
    *id = new_id;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_semaphore_destroy(ps3_lv2_objects *o, uint32_t id) {
    lock(o);
    ps3_lv2_semaphore *s = find_semaphore(o, id);
    if (!s) return unlock(o, PS3_LV2_ESRCH);
    s->active = 0;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_semaphore_get_value(ps3_lv2_objects *o, uint32_t id, int32_t *value) {
    if (!value) return PS3_LV2_EFAULT;
    lock(o);
    ps3_lv2_semaphore *s = find_semaphore(o, id);
    if (!s) return unlock(o, PS3_LV2_ESRCH);
    *value = s->value;
    return unlock(o, PS3_LV2_OK);
}
/* ---- B2a semaphore wait/trywait/post ------------------------------------------------------- */
/* Wait takes one count. The sleeper releases the guard while it sleeps, so a post from any thread
 * (guest or host) can take it; the wake word is the object's own `sequence`, bumped by post and
 * destroy. A destroyed semaphore wakes its sleepers with PS3_LV2_CANCELLED. */
uint32_t ps3_lv2_semaphore_wait(ps3_lv2_objects *o, uint32_t id, uint64_t timeout_us,
                                ps3_notify_cancel *cancel, uint64_t *blocked_word) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    uint64_t deadline = 0;
    if (!deadline_from_us(timeout_us, &deadline)) return PS3_LV2_PROVIDER_FAILURE;
    lock(o);
    ps3_lv2_semaphore *s = find_semaphore(o, id);
    if (!s) return unlock(o, PS3_LV2_ESRCH);
    for (;;) {
        if (s->value > 0) { --s->value; return unlock(o, PS3_LV2_OK); }
        if (ps3_notify_cancel_is_requested(cancel)) return unlock(o, PS3_LV2_CANCELLED);
        uint64_t sequence = __atomic_load_n(&s->sequence, __ATOMIC_SEQ_CST);
        if (blocked_word) { __atomic_store_n(blocked_word, 1, __ATOMIC_SEQ_CST); wake(o); }
        unlock(o, 0);
        ps3_notify_wait_result r = o->port.wait(o->port.context, &s->sequence, sequence,
            cancel ? &cancel->requested : NULL, deadline);
        lock(o);
        if (blocked_word) __atomic_store_n(blocked_word, 0, __ATOMIC_SEQ_CST);
        if (r == PS3_NOTIFY_FAILURE) return unlock(o, PS3_LV2_PROVIDER_FAILURE);
        if (r == PS3_NOTIFY_TIMEOUT) return unlock(o, PS3_LV2_TIMED_OUT);
        if (!s->active) return unlock(o, PS3_LV2_CANCELLED);   /* destroyed while sleeping */
    }
}
uint32_t ps3_lv2_semaphore_trywait(ps3_lv2_objects *o, uint32_t id) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_semaphore *s = find_semaphore(o, id);
    if (!s) return unlock(o, PS3_LV2_ESRCH);
    if (s->value <= 0) return unlock(o, PS3_LV2_EBUSY);   /* lv2: CELL_EBUSY, not EAGAIN */
    --s->value;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_semaphore_post(ps3_lv2_objects *o, uint32_t id, int32_t count) {
    if (count < 0) return PS3_LV2_EINVAL;
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_semaphore *s = find_semaphore(o, id);
    if (!s) return unlock(o, PS3_LV2_ESRCH);
    if (count > s->max - s->value) return unlock(o, PS3_LV2_EBUSY);   /* would exceed max */
    s->value += count;
    if (count) { __atomic_fetch_add(&s->sequence, 1, __ATOMIC_SEQ_CST); wake(o); }
    return unlock(o, PS3_LV2_OK);
}

/* ---- B2a event flags ------------------------------------------------------------------------ */
static int event_mode_valid(uint32_t mode) {
    const uint32_t kind = mode & (PS3_EVENT_FLAG_AND | PS3_EVENT_FLAG_OR);
    const uint32_t extra = mode & ~(uint32_t)(PS3_EVENT_FLAG_AND | PS3_EVENT_FLAG_OR |
        PS3_EVENT_FLAG_CLEAR | PS3_EVENT_FLAG_CLEAR_ALL);
    return (kind == PS3_EVENT_FLAG_AND || kind == PS3_EVENT_FLAG_OR) && !extra;
}
static int event_flag_matches(uint64_t bits, uint64_t pattern, uint32_t mode) {
    if (!pattern) return 1;   /* an empty pattern matches immediately (lv2 returns 0 bits) */
    return (mode & PS3_EVENT_FLAG_AND) ? ((bits & pattern) == pattern) : ((bits & pattern) != 0);
}
/* The matched-value / clear step shared by wait and trywait. Under the guard. */
static uint64_t event_flag_consume(ps3_lv2_event_flag *f, uint64_t pattern, uint32_t mode) {
    const uint64_t matched = (mode & PS3_EVENT_FLAG_AND) ? pattern : (f->bits & pattern);
    if (mode & (PS3_EVENT_FLAG_CLEAR | PS3_EVENT_FLAG_CLEAR_ALL)) {
        /* CLEAR_ALL clears every bit if the pattern matched; CLEAR clears only the matched bits. */
        const uint64_t drop = (mode & PS3_EVENT_FLAG_CLEAR_ALL) ? ~UINT64_C(0) : pattern;
        __atomic_fetch_and(&f->bits, ~drop, __ATOMIC_SEQ_CST);
        __atomic_fetch_add(&f->sequence, 1, __ATOMIC_SEQ_CST);
    }
    return matched;
}
uint32_t ps3_lv2_event_flag_create(ps3_lv2_objects *o, uint32_t *id, uint32_t protocol, uint32_t type, uint64_t bits) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!id) return PS3_LV2_EFAULT;
    if (protocol != PS3_SYNC_FIFO && protocol != PS3_SYNC_PRIORITY) return PS3_LV2_EINVAL;
    if (type != PS3_SYNC_WAITER_SINGLE && type != PS3_SYNC_WAITER_MULTIPLE) return PS3_LV2_EINVAL;
    lock(o);
    uint32_t new_id = allocate_id(o, PS3_LV2_KIND_EVENT_FLAG);
    if (!new_id) return unlock(o, PS3_LV2_EAGAIN);
    for (size_t i = 0; i < PS3_LV2_EVENT_FLAG_CAP; ++i) if (!o->event_flags[i].active) {
        memset(&o->event_flags[i], 0, sizeof o->event_flags[i]);
        o->event_flags[i].id = new_id;
        o->event_flags[i].protocol = protocol;
        o->event_flags[i].attribute = type;
        o->event_flags[i].bits = bits;
        o->event_flags[i].active = 1;
        break;
    }
    *id = new_id;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_event_flag_destroy(ps3_lv2_objects *o, uint32_t id) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_event_flag *f = find_event_flag(o, id);
    if (!f) return unlock(o, PS3_LV2_ESRCH);
    f->active = 0;
    __atomic_fetch_add(&f->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_event_flag_wait(ps3_lv2_objects *o, uint32_t id, uint64_t bitptn, uint32_t mode,
                                 uint64_t *result, uint64_t timeout_us, ps3_notify_cancel *cancel,
                                 uint64_t *blocked_word) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!event_mode_valid(mode)) return PS3_LV2_EINVAL;
    if (!result) return PS3_LV2_EFAULT;
    uint64_t deadline = 0;
    if (!deadline_from_us(timeout_us, &deadline)) return PS3_LV2_PROVIDER_FAILURE;
    lock(o);
    ps3_lv2_event_flag *f = find_event_flag(o, id);
    if (!f) return unlock(o, PS3_LV2_ESRCH);
    for (;;) {
        if (event_flag_matches(f->bits, bitptn, mode)) {
            *result = event_flag_consume(f, bitptn, mode);
            return unlock(o, PS3_LV2_OK);
        }
        if (ps3_notify_cancel_is_requested(cancel)) return unlock(o, PS3_LV2_CANCELLED);
        /* SINGLE admits exactly one waiter; a second one is EPERM, as lv2 answers. */
        if (f->attribute == PS3_SYNC_WAITER_SINGLE && f->waiters) return unlock(o, PS3_LV2_EPERM);
        uint64_t sequence = __atomic_load_n(&f->sequence, __ATOMIC_SEQ_CST);
        ++f->waiters; ++f->blocked_waiters;
        if (blocked_word) { __atomic_store_n(blocked_word, 1, __ATOMIC_SEQ_CST); wake(o); }
        unlock(o, 0);
        ps3_notify_wait_result r = o->port.wait(o->port.context, &f->sequence, sequence,
            cancel ? &cancel->requested : NULL, deadline);
        lock(o);
        if (blocked_word) __atomic_store_n(blocked_word, 0, __ATOMIC_SEQ_CST);
        --f->waiters; --f->blocked_waiters;
        if (r == PS3_NOTIFY_FAILURE) return unlock(o, PS3_LV2_PROVIDER_FAILURE);
        if (r == PS3_NOTIFY_TIMEOUT) return unlock(o, PS3_LV2_TIMED_OUT);
        if (!f->active) return unlock(o, PS3_LV2_CANCELLED);
    }
}
uint32_t ps3_lv2_event_flag_trywait(ps3_lv2_objects *o, uint32_t id, uint64_t bitptn, uint32_t mode,
                                    uint64_t *result) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!event_mode_valid(mode)) return PS3_LV2_EINVAL;
    if (!result) return PS3_LV2_EFAULT;
    lock(o);
    ps3_lv2_event_flag *f = find_event_flag(o, id);
    if (!f) return unlock(o, PS3_LV2_ESRCH);
    /* root fix (B2a review): lv2 trywait answers CELL_EBUSY on no match; PS3_LV2_TIMED_OUT is an internal
     * code that the adapter would otherwise hand to the guest verbatim. */
    if (!event_flag_matches(f->bits, bitptn, mode)) return unlock(o, PS3_LV2_EBUSY);
    *result = event_flag_consume(f, bitptn, mode);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_event_flag_set(ps3_lv2_objects *o, uint32_t id, uint64_t bitptn) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_event_flag *f = find_event_flag(o, id);
    if (!f) return unlock(o, PS3_LV2_ESRCH);
    __atomic_fetch_or(&f->bits, bitptn, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&f->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_event_flag_clear(ps3_lv2_objects *o, uint32_t id, uint64_t bitptn) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_event_flag *f = find_event_flag(o, id);
    if (!f) return unlock(o, PS3_LV2_ESRCH);
    __atomic_fetch_and(&f->bits, ~bitptn, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&f->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}

/* ---- B2a queue tryreceive and force destroy -------------------------------------------------- */
uint32_t ps3_lv2_queue_tryreceive(ps3_lv2_objects *o, uint32_t id, ps3_lv2_event *array, uint32_t size,
                                  uint32_t *number) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!array || !number) return PS3_LV2_EFAULT;
    if (size == 0) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    uint32_t taken = 0;
    while (taken < size && q->count) {
        array[taken++] = q->ring[q->head];
        q->head = (q->head + 1) % PS3_EVENT_QUEUE_DEPTH_MAX;
        --q->count;
    }
    *number = taken;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_destroy_force(ps3_lv2_objects *o, uint32_t id, int force) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    if (q->receivers && !force) return unlock(o, PS3_LV2_EBUSY);
    for (size_t i = 0; i < PS3_LV2_PORT_CAP; ++i) if (o->ports[i].active && o->ports[i].queue == id) o->ports[i].queue = 0;
    q->active = 0;
    q->forced = force && q->receivers;
    __atomic_fetch_add(&q->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}

uint32_t ps3_lv2_queue_create(ps3_lv2_objects *o, uint32_t *id, const ps3_lv2_queue_attr *attr, uint64_t ipc_key, int32_t size) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (size <= 0 || size > PS3_EVENT_QUEUE_DEPTH_MAX) return PS3_LV2_EINVAL;
    if (!id || !attr) return PS3_LV2_EFAULT;
    if (attr->protocol != PS3_SYNC_FIFO && attr->protocol != PS3_SYNC_PRIORITY) return PS3_LV2_EINVAL;
    if (attr->type != PS3_PPU_QUEUE && attr->type != PS3_SPU_QUEUE) return PS3_LV2_EINVAL;
#ifndef PS3_NATIVE_LLE_PROFILE
    if (ipc_key) return PS3_LV2_UNSUPPORTED; /* legal lv2 IPC key: not owned natively -> stop, not a fake EINVAL */
#endif
    lock(o);
#ifdef PS3_NATIVE_LLE_PROFILE
    /* Root (2026-11-11, Eden r13): the game creates an event queue with an IPC key. There is one process, so a
     * key is only a name inside this registry: a second create with a live key is EEXIST (0x80010014), as lv2
     * answers for a key that is already taken. No other process can ever look it up. */
    if (ipc_key) for (size_t i = 0; i < PS3_LV2_QUEUE_CAP; ++i)
        if (o->queues[i].active && o->queues[i].ipc_key == ipc_key) return unlock(o, 0x80010014u);
#endif
    uint32_t new_id = allocate_id(o, PS3_LV2_KIND_QUEUE);
    if (!new_id) return unlock(o, PS3_LV2_EAGAIN);
    for (size_t i = 0; i < PS3_LV2_QUEUE_CAP; ++i) if (!o->queues[i].active) {
        memset(&o->queues[i], 0, sizeof o->queues[i]);
        o->queues[i].id = new_id; o->queues[i].protocol = attr->protocol; o->queues[i].type = attr->type;
        o->queues[i].size = (uint32_t)size; o->queues[i].active = 1;
        o->queues[i].ipc_key = ipc_key;
        break;
    }
    *id = new_id;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_destroy(ps3_lv2_objects *o, uint32_t id) {
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    if (q->receivers) return unlock(o, PS3_LV2_EBUSY);
    for (size_t i = 0; i < PS3_LV2_PORT_CAP; ++i) if (o->ports[i].active && o->ports[i].queue == id) o->ports[i].queue = 0;
    q->active = 0;
    __atomic_fetch_add(&q->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_bind_service(ps3_lv2_objects *o, uint32_t id, ps3_lv2_event owned) {
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    if (q->service_bound || q->count) return unlock(o, PS3_LV2_EBUSY);
    q->service_bound = 1;
    q->owned = owned;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_unbind_service(ps3_lv2_objects *o, uint32_t id) {
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    if (q->receivers) return unlock(o, PS3_LV2_EBUSY);
    q->service_bound = 0;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_receive(ps3_lv2_objects *o, uint32_t id, ps3_lv2_event *event, ps3_notify_cancel *cancel, uint64_t *blocked_word) {
    if (!o || !event) return PS3_LV2_EFAULT;
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    ++q->receivers;
    for (;;) {
        if (q->count) {
            *event = q->ring[q->head];
            q->head = (q->head + 1) % PS3_EVENT_QUEUE_DEPTH_MAX;
            --q->count;
            --q->receivers;
            return unlock(o, PS3_LV2_OK);
        }
        if (ps3_notify_cancel_is_requested(cancel)) { --q->receivers; return unlock(o, PS3_LV2_CANCELLED); }
        uint64_t sequence = __atomic_load_n(&q->sequence, __ATOMIC_SEQ_CST);
        ++q->blocked_receivers;
        if (blocked_word) { __atomic_store_n(blocked_word, 1, __ATOMIC_SEQ_CST); wake(o); }
        unlock(o, 0);
        ps3_notify_wait_result r = o->port.wait(o->port.context, &q->sequence, sequence, cancel ? &cancel->requested : NULL, UINT64_MAX);
        lock(o);
        if (blocked_word) __atomic_store_n(blocked_word, 0, __ATOMIC_SEQ_CST);
        --q->blocked_receivers;
        if (r == PS3_NOTIFY_FAILURE) { --q->receivers; return unlock(o, PS3_LV2_PROVIDER_FAILURE); }
        /* Destroyed while sleeping: a FORCE destroy cancels the receiver (ECANCELED); a plain one
         * cannot happen (it refuses EBUSY while receivers are registered). */
        if (!q->active) return unlock(o, q->forced ? (uint32_t)PS3_LV2_ECANCELED : (uint32_t)PS3_LV2_CANCELLED);
    }
}
/* Decision 33 / C1b. Modelled on ps3_lv2_port_send, with the caller naming the queue and the source directly. */
uint32_t ps3_lv2_queue_send(ps3_lv2_objects *o, uint32_t id, uint64_t source, uint64_t data1, uint64_t data2, uint64_t data3) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q) return unlock(o, PS3_LV2_ESRCH);
    if (q->service_bound) {
        /* A service receiver accepts only its owned event pattern; anything else is FENCED. The service's own event
         * is not what an SPU user event looks like, so this refuses it rather than feeding the service a lie. */
        if (data1 != q->owned.data1 || data2 != q->owned.data2 || data3 != q->owned.data3)
            return unlock(o, PS3_LV2_FENCED);
    }
    if (q->count >= q->size) return unlock(o, PS3_LV2_EBUSY);
    q->ring[(q->head + q->count) % PS3_EVENT_QUEUE_DEPTH_MAX] = (ps3_lv2_event){source, data1, data2, data3};
    ++q->count;
    __atomic_fetch_add(&q->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_queue_snapshot(ps3_lv2_objects *o, uint32_t id, ps3_lv2_queue *out) {
    lock(o);
    ps3_lv2_queue *q = find_queue(o, id);
    if (!q || !out) return unlock(o, PS3_LV2_ESRCH);
    *out = *q;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_create(ps3_lv2_objects *o, uint32_t *id, uint32_t type, uint64_t name) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (type != PS3_EVENT_PORT_LOCAL && type != PS3_EVENT_PORT_IPC) return PS3_LV2_EINVAL;
    if (!id) return PS3_LV2_EFAULT;
    if (type != PS3_EVENT_PORT_LOCAL) return PS3_LV2_UNSUPPORTED; /* IPC ports are legal lv2 but need an IPC key namespace not owned here: stop */
    /* `name` is stored verbatim (pinned sys_event_port_create: idm::make<lv2_event_port>(type, name); no uniqueness lookup). */
    lock(o);
    uint32_t new_id = allocate_id(o, PS3_LV2_KIND_PORT);
    if (!new_id) return unlock(o, PS3_LV2_EAGAIN);
    for (size_t i = 0; i < PS3_LV2_PORT_CAP; ++i) if (!o->ports[i].active) { o->ports[i] = (ps3_lv2_port){new_id, type, 0, 1, name}; break; }
    *id = new_id;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_snapshot(ps3_lv2_objects *o, uint32_t id, ps3_lv2_port *out) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!out) return PS3_LV2_EFAULT;
    lock(o);
    ps3_lv2_port *p = find_port(o, id);
    if (!p) return unlock(o, PS3_LV2_ESRCH);
    *out = *p;
    return unlock(o, PS3_LV2_OK);
}
static uint64_t port_source_value(const ps3_lv2_port *p) {
    /* Pinned NO_NAME policy uses pid1; named ports preserve all64 supplied bits. */
    return p->name ? p->name : (UINT64_C(1) << 32) | p->id;
}
uint32_t ps3_lv2_port_source(ps3_lv2_objects *o, uint32_t id, uint64_t *out) {
    if (!o || !o->initialized) return PS3_LV2_EINVAL;
    if (!out) return PS3_LV2_EFAULT;
    lock(o);
    ps3_lv2_port *p = find_port(o, id);
    if (!p) return unlock(o, PS3_LV2_ESRCH);
    *out = port_source_value(p);
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_destroy(ps3_lv2_objects *o, uint32_t id) {
    lock(o);
    ps3_lv2_port *p = find_port(o, id);
    if (!p) return unlock(o, PS3_LV2_ESRCH);
    if (p->queue) return unlock(o, PS3_LV2_EISCONN);
    p->active = 0;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_connect_local(ps3_lv2_objects *o, uint32_t port, uint32_t queue) {
    lock(o);
    ps3_lv2_port *p = find_port(o, port);
    if (!p || !find_queue(o, queue)) return unlock(o, PS3_LV2_ESRCH);
    if (p->type != PS3_EVENT_PORT_LOCAL) return unlock(o, PS3_LV2_EINVAL);
    if (p->queue) return unlock(o, PS3_LV2_EISCONN);
    p->queue = queue;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_disconnect(ps3_lv2_objects *o, uint32_t port) {
    lock(o);
    ps3_lv2_port *p = find_port(o, port);
    if (!p) return unlock(o, PS3_LV2_ESRCH);
    if (!p->queue) return unlock(o, PS3_LV2_ENOTCONN);
    p->queue = 0;
    return unlock(o, PS3_LV2_OK);
}
uint32_t ps3_lv2_port_send(ps3_lv2_objects *o, uint32_t port, uint64_t data1, uint64_t data2, uint64_t data3) {
    lock(o);
    ps3_lv2_port *p = find_port(o, port);
    if (!p) return unlock(o, PS3_LV2_ESRCH);
    ps3_lv2_queue *q = p->queue ? find_queue(o, p->queue) : NULL;
    if (!q) return unlock(o, PS3_LV2_ENOTCONN);
    if (q->service_bound && (data1 != q->owned.data1 || data2 != q->owned.data2 || data3 != q->owned.data3))
        return unlock(o, PS3_LV2_FENCED);
    if (q->count >= q->size) return unlock(o, PS3_LV2_EBUSY);
    /* Pinned sys_event_port_send: source = port.name ? port.name : (pid << 32) | port id. [POL] NO_NAME pid = 1. */
    const uint64_t source = port_source_value(p);
    q->ring[(q->head + q->count) % PS3_EVENT_QUEUE_DEPTH_MAX] = (ps3_lv2_event){source, data1, data2, data3};
    ++q->count;
    __atomic_fetch_add(&q->sequence, 1, __ATOMIC_SEQ_CST);
    wake(o);
    return unlock(o, PS3_LV2_OK);
}
