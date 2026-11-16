#ifndef NATIVE_LV2_OBJECTS_H
#define NATIVE_LV2_OBJECTS_H
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/sync/guard.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Caller-owned lv2 semaphores, event queues and event ports for native HLE
 * services. Pinned validation order (sys_semaphore.cpp:33-79, sys_event.cpp:
 * 231-274, 631-711, 780-841). IDs are base + 0x100*serial, never reused [POL].
 * A queue bound to a native service accepts only that service's owned event
 * pattern; any other send answers FENCED and enqueues/wakes nothing, and the
 * caller must stop. Blocking receive uses the injected wait port and cancel
 * token. Composition keeps storage, guard and port alive until every receiver
 * has returned and been joined. */
enum {
    PS3_LV2_OK = 0, PS3_LV2_EAGAIN = 0x80010001u, PS3_LV2_EINVAL = 0x80010002u, PS3_LV2_ESRCH = 0x80010005u,
    PS3_LV2_EBUSY = 0x8001000Au, PS3_LV2_EFAULT = 0x8001000Du, PS3_LV2_EISCONN = 0x80010015u,
    PS3_LV2_ENOTCONN = 0x80010016u, PS3_LV2_FENCED = 0xFFFF0001u, PS3_LV2_CANCELLED = 0xFFFF0002u,
    PS3_LV2_PROVIDER_FAILURE = 0xFFFF0003u,
    PS3_LV2_UNSUPPORTED = 0xFFFF0004u   /* legal lv2 input this native layer does not own: composition maps to a stop, never a guest error */
};
enum { PS3_SYNC_FIFO = 1, PS3_SYNC_PRIORITY = 2, PS3_PPU_QUEUE = 1, PS3_SPU_QUEUE = 2,
       PS3_EVENT_PORT_LOCAL = 1, PS3_EVENT_PORT_IPC = 3, PS3_EVENT_QUEUE_DEPTH_MAX = 127 };
enum { PS3_LV2_SEMAPHORE_BASE = 0x96000000u, PS3_LV2_QUEUE_BASE = 0x8D000000u, PS3_LV2_PORT_BASE = 0x0E000000u,
       PS3_LV2_EVENT_FLAG_BASE = 0x9F000000u, PS3_LV2_ID_STEP = 0x100u };
#ifdef PS3_NATIVE_LLE_PROFILE
/* Root (2026-11-11, hardware run r8): a real game creates far more than 8 event queues and ports (the
 * primary alone stopped at its ninth queue while setting up a timer). Build-wide profile, so every unit
 * sees the same struct sizes. A queue is about 4.2 KiB (127-event ring): 64 of them are about 270 KiB. */
/* 512 semaphores: lv2 mutexes (syscalls 100..104) are built on them by the adapter. */
enum { PS3_LV2_SEMAPHORE_CAP = 512, PS3_LV2_QUEUE_CAP = 64, PS3_LV2_PORT_CAP = 64, PS3_LV2_EVENT_FLAG_CAP = 64 };
#else
enum { PS3_LV2_SEMAPHORE_CAP = 64, PS3_LV2_QUEUE_CAP = 8, PS3_LV2_PORT_CAP = 8, PS3_LV2_EVENT_FLAG_CAP = 16 };
#endif
/* lv2 event-flag semantics (sys_event_flag.cpp). mode bits are a mask, not an enum: exactly one of
 * AND/OR must be set, and CLEAR may accompany either. A SINGLE-type flag admits one waiter at a time;
 * a MULTIPLE-type flag admits many (and refuses a single-service waiter on the same object). */
enum { PS3_EVENT_FLAG_AND = 0x1, PS3_EVENT_FLAG_OR = 0x2, PS3_EVENT_FLAG_CLEAR = 0x10, PS3_EVENT_FLAG_CLEAR_ALL = 0x20,
       PS3_SYNC_WAITER_SINGLE = 0x10000, PS3_SYNC_WAITER_MULTIPLE = 0x20000 };
typedef struct ps3_lv2_event_flag {
    uint32_t id, protocol, attribute, waiters, blocked_waiters;
    unsigned active;
    uint64_t bits, sequence;      /* sequence is the futex word bumped on set/clear/destroy */
} ps3_lv2_event_flag;
typedef struct ps3_lv2_event { uint64_t source, data1, data2, data3; } ps3_lv2_event;
typedef struct ps3_lv2_semaphore_attr { uint32_t protocol; } ps3_lv2_semaphore_attr;
typedef struct ps3_lv2_queue_attr { uint32_t protocol, type; uint64_t name; } ps3_lv2_queue_attr;
/* `sequence` is the futex word a waiter sleeps on: post and destroy bump it. Appended last so the
 * former five positional fields stay valid. */
typedef struct { uint32_t id, protocol; int32_t value, max; unsigned active; uint64_t sequence; } ps3_lv2_semaphore;
typedef struct {
    uint32_t id, protocol, type, size, head, count, receivers, blocked_receivers;
    unsigned active, service_bound;
    unsigned forced;              /* destroyed with force while receivers were registered */
    ps3_lv2_event owned;          /* the bound service's only accepted event (source ignored) */
    uint64_t sequence;            /* futex word bumped on enqueue/destroy */
    ps3_lv2_event ring[PS3_EVENT_QUEUE_DEPTH_MAX];
    uint64_t ipc_key;             /* 0 = local; a non-zero key names the queue inside this one process (LLE profile) */
} ps3_lv2_queue;
/* `name` is the full 64-bit lv2 port name stored verbatim at creation (0 = NO_NAME). Names are NOT unique: the pinned
 * sys_event.cpp (sys_event_port_create -> idm::make<lv2_event_port>(type, name)) performs no lookup, so duplicates are valid.
 * Appended last so positional initialisers of the former four fields stay valid. */
typedef struct { uint32_t id, type, queue; unsigned active; uint64_t name; } ps3_lv2_port;
typedef struct ps3_lv2_objects {
    ps3_instance_guard guard;
    ps3_notify_port port;
    uint32_t capacity_limit[4];   /* composition-controlled exhaustion (<= CAP) */
    uint32_t serial[4];
    ps3_lv2_semaphore semaphores[PS3_LV2_SEMAPHORE_CAP];
    ps3_lv2_queue queues[PS3_LV2_QUEUE_CAP];
    ps3_lv2_port ports[PS3_LV2_PORT_CAP];
    ps3_lv2_event_flag event_flags[PS3_LV2_EVENT_FLAG_CAP];
    unsigned initialized;
} ps3_lv2_objects;
enum { PS3_LV2_KIND_SEMAPHORE, PS3_LV2_KIND_QUEUE, PS3_LV2_KIND_PORT, PS3_LV2_KIND_EVENT_FLAG };
/* Storage contract: zero-filled caller storage, constructed once. A second init is refused
 * (EINVAL, state untouched), so a live queue/receiver can never be silently reset. */
uint32_t ps3_lv2_objects_init(ps3_lv2_objects *, ps3_instance_guard, ps3_notify_port);
void ps3_lv2_objects_limit(ps3_lv2_objects *, int kind, uint32_t live_capacity);
uint32_t ps3_lv2_semaphore_create(ps3_lv2_objects *, uint32_t *id, const ps3_lv2_semaphore_attr *, int32_t initial, int32_t max);
uint32_t ps3_lv2_semaphore_destroy(ps3_lv2_objects *, uint32_t id);
uint32_t ps3_lv2_semaphore_get_value(ps3_lv2_objects *, uint32_t id, int32_t *value);
uint32_t ps3_lv2_queue_create(ps3_lv2_objects *, uint32_t *id, const ps3_lv2_queue_attr *, uint64_t ipc_key, int32_t size);
/* All components sharing one blocked-word handshake must share ONE notify port (a creator
 * waiting for a service block sleeps on its own port and is woken by the primitive's port). */
/* Non-force destroy refuses (EBUSY) while receivers are registered. */
uint32_t ps3_lv2_queue_destroy(ps3_lv2_objects *, uint32_t id);
/* Bind the single native service receiver; it accepts only `owned` (source ignored). */
uint32_t ps3_lv2_queue_bind_service(ps3_lv2_objects *, uint32_t id, ps3_lv2_event owned);
/* EBUSY while a receiver is registered: the owned-or-FENCED guarantee holds until it joined. */
uint32_t ps3_lv2_queue_unbind_service(ps3_lv2_objects *, uint32_t id);
/* Blocking receive. *blocked_word (optional) is set to 1 under the guard
 * immediately before the receiver sleeps on an empty queue and back to 0 on
 * wake, and the port is woken, so a creator can observe a real block. */
uint32_t ps3_lv2_queue_receive(ps3_lv2_objects *, uint32_t id, ps3_lv2_event *, ps3_notify_cancel *, uint64_t *blocked_word);
uint32_t ps3_lv2_queue_snapshot(ps3_lv2_objects *, uint32_t id, ps3_lv2_queue *out);
/* Decision 33 / C1b: send an event STRAIGHT to a queue, with the caller's own source and data words. This is what an
 * SPU-to-PPU user event needs: the queue is named by the SPU thread's spup binding, not by a port, and lv2 chooses
 * the source word (0xFFFFFFFF53505501 for SYS_SPU_THREAD_EVENT_USER_KEY). Wake a blocked receiver, EBUSY when the
 * queue is full, EBUSY while a service receiver is registered, and the same FENCED rule as port_send for a
 * service-bound queue. EINVAL for a bad argument or an uninitialized set, ESRCH when the queue does not exist or was
 * destroyed, EFAULT for a destroyed-with-force queue. Never invents a delivery: every refusal enqueues nothing. */
uint32_t ps3_lv2_queue_send(ps3_lv2_objects *, uint32_t id, uint64_t source, uint64_t data1, uint64_t data2, uint64_t data3);
/* LOCAL ports accept any 64-bit name (stored exactly; nonzero names become the event source, zero keeps the NO_NAME source).
 * IPC ports remain UNSUPPORTED (no IPC key namespace is owned). Validation order: EINVAL type, EFAULT id, UNSUPPORTED, EAGAIN. */
uint32_t ps3_lv2_port_create(ps3_lv2_objects *, uint32_t *id, uint32_t type, uint64_t name);
/* Read-only copy of one live port (ESRCH when absent, EFAULT for a null out). Never changes state. */
uint32_t ps3_lv2_port_snapshot(ps3_lv2_objects *, uint32_t id, ps3_lv2_port *out);
/* Canonical immutable event source used by port_send. Read-only; failures leave
 * output untouched. Source values classify events, not authenticated senders:
 * arbitrary named ports may deliberately share the same source. */
uint32_t ps3_lv2_port_source(ps3_lv2_objects *, uint32_t id, uint64_t *out);
uint32_t ps3_lv2_port_destroy(ps3_lv2_objects *, uint32_t id);
uint32_t ps3_lv2_port_connect_local(ps3_lv2_objects *, uint32_t port, uint32_t queue);
uint32_t ps3_lv2_port_disconnect(ps3_lv2_objects *, uint32_t port);
uint32_t ps3_lv2_port_send(ps3_lv2_objects *, uint32_t port, uint64_t data1, uint64_t data2, uint64_t data3);
size_t ps3_lv2_live_count(ps3_lv2_objects *, int kind);

/* ---- Decision 33 / B2a: semaphore wait/post, event flags, queue tryreceive. Additive only; every
 * function below leaves the existing calls' behaviour byte-identical.
 *
 * Blocking forms share the ONE notify port the primitive was initialised with, so a waker on any
 * thread reaches a sleeper on any other. `cancel` is the same token the queue receive uses;
 * `deadline_ns` is a MONOTONIC deadline (UINT64_MAX = none). A cancelled or timed-out sleep returns
 * PS3_LV2_CANCELLED / PS3_LV2_TIMED_OUT and never a fabricated success. */
enum { PS3_LV2_TIMED_OUT = 0xFFFF0005u, PS3_LV2_EPERM = 0x80010009u, PS3_LV2_ECANCELED = 0x80010019u };
/* 0 and 1 are legal counts; anything else is EINVAL. Posting past `max` is EBUSY. */
uint32_t ps3_lv2_semaphore_wait(ps3_lv2_objects *, uint32_t id, uint64_t timeout_us,
                                ps3_notify_cancel *, uint64_t *blocked_word);
uint32_t ps3_lv2_semaphore_trywait(ps3_lv2_objects *, uint32_t id);
uint32_t ps3_lv2_semaphore_post(ps3_lv2_objects *, uint32_t id, int32_t count);
/* `type` is PS3_SYNC_WAITER_SINGLE or PS3_SYNC_WAITER_MULTIPLE; any other value is EINVAL. */
uint32_t ps3_lv2_event_flag_create(ps3_lv2_objects *, uint32_t *id, uint32_t protocol, uint32_t type, uint64_t bits);
/* Destroy wakes every waiter; each wakes with PS3_LV2_CANCELLED (its object is gone). */
uint32_t ps3_lv2_event_flag_destroy(ps3_lv2_objects *, uint32_t id);
/* Blocking wait. `*result` receives the matched bits (NULL result is EFAULT for every mode). */
uint32_t ps3_lv2_event_flag_wait(ps3_lv2_objects *, uint32_t id, uint64_t bitptn, uint32_t mode,
                                 uint64_t *result, uint64_t timeout_us, ps3_notify_cancel *,
                                 uint64_t *blocked_word);
/* Non-blocking poll of the same predicate. ETIMEDOUT when the bits do not match. */
uint32_t ps3_lv2_event_flag_trywait(ps3_lv2_objects *, uint32_t id, uint64_t bitptn, uint32_t mode,
                                    uint64_t *result);
uint32_t ps3_lv2_event_flag_set(ps3_lv2_objects *, uint32_t id, uint64_t bitptn);
uint32_t ps3_lv2_event_flag_clear(ps3_lv2_objects *, uint32_t id, uint64_t bitptn);
/* Non-blocking receive; *number receives how many were drained (0..size). ESRCH/EINVAL as receive. */
uint32_t ps3_lv2_queue_tryreceive(ps3_lv2_objects *, uint32_t id, ps3_lv2_event *array, uint32_t size,
                                  uint32_t *number);
/* Force destroy: `force` wakes a blocked receiver with PS3_LV2_ECANCELED (mode 1 in lv2 terms);
 * without force a registered receiver keeps the existing EBUSY refusal. */
uint32_t ps3_lv2_queue_destroy_force(ps3_lv2_objects *, uint32_t id, int force);
#ifdef __cplusplus
}
#endif
#endif
