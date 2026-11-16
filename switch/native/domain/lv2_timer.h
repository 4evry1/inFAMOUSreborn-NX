/* lv2 timer objects: sys_timer_create (70), destroy (71), get_information (72), start (73), stop (74),
 * connect_event_queue (75), disconnect_event_queue (76).
 *
 * Platform-free and instance-owned: no globals, no clock, no threads, no event queue. Time arrives as a
 * `now_us` argument (the same microsecond clock the guest reads through the timebase), and an expired
 * timer is reported to the caller as a `lv2_timer_event` that the adapter sends to the lv2 event queue.
 * The caller serialises every call on one table.
 *
 * Provenance: the semantics and error values are root's recollection of the public lv2 behaviour as
 * RPCS3 models it (sys_timer.cpp), not extracted from firmware:
 *   - a timer must be connected to an event queue before it can start (ENOTCONN);
 *   - starting a running timer is EBUSY; a periodic timer's period must be at least 100 us (EINVAL);
 *   - a one-shot timer (period 0) whose base time has already passed is ETIMEDOUT;
 *   - destroying a connected timer is EISCONN;
 *   - the event is (source, data1, data2, 0), where source is the connect call's `name`, or
 *     (process id << 32) | timer id when the name is 0.
 */
#ifndef LV2_TIMER_H
#define LV2_TIMER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV2_TIMER_OK 0u
#define LV2_TIMER_EINVAL 0x80010002u
#define LV2_TIMER_ESRCH 0x80010005u
#define LV2_TIMER_EBUSY 0x8001000Au
#define LV2_TIMER_ETIMEDOUT 0x8001000Bu
#define LV2_TIMER_EISCONN 0x80010015u
#define LV2_TIMER_ENOTCONN 0x80010016u
#define LV2_TIMER_EAGAIN 0x80010001u /* no free timer slot */

#define LV2_TIMER_CAPACITY 16u
#define LV2_TIMER_ID_BASE 0x11000000u /* ids are LV2_TIMER_ID_BASE | (generation << 8) | slot */
#define LV2_TIMER_MIN_PERIOD_US 100u

#define LV2_TIMER_STATE_STOP 0u
#define LV2_TIMER_STATE_RUN 1u

typedef struct lv2_timer {
    uint32_t in_use;
    uint32_t id;
    uint32_t generation;
    uint32_t connected;
    uint32_t queue_id;
    uint32_t running;
    uint64_t source, data1, data2;
    uint64_t next_us;   /* absolute expiry of the next event */
    uint64_t period_us; /* 0 = one shot */
    uint64_t fired;
} lv2_timer;

typedef struct lv2_timer_table {
    lv2_timer timers[LV2_TIMER_CAPACITY];
    uint32_t process_id;
    uint64_t creates, starts, events;
} lv2_timer_table;

/* One expiry for the adapter to deliver with its event-queue send. */
typedef struct lv2_timer_event {
    uint32_t queue_id;
    uint64_t source, data1, data2;
} lv2_timer_event;

/* What sys_timer_get_information reports. */
typedef struct lv2_timer_information {
    uint64_t next_us;
    uint64_t period_us;
    uint32_t state;
} lv2_timer_information;

void lv2_timer_table_init(lv2_timer_table *table, uint32_t process_id);

/* 70. Writes a non-zero id. EAGAIN when all slots are live; `*id_out` untouched on failure. */
uint32_t lv2_timer_create(lv2_timer_table *table, uint32_t *id_out);
/* 71. ESRCH for an unknown id, EISCONN while connected. */
uint32_t lv2_timer_destroy(lv2_timer_table *table, uint32_t id);
/* 72. A stopped timer reports zeros and LV2_TIMER_STATE_STOP. */
uint32_t lv2_timer_get_information(const lv2_timer_table *table, uint32_t id, lv2_timer_information *out);
/* 73. `base_us` is the absolute time of the first event (0 = one period from now, periodic only).
 * A base time in the past with a period starts at the next multiple of the period after `now_us`. */
uint32_t lv2_timer_start(lv2_timer_table *table, uint32_t id, uint64_t base_us, uint64_t period_us, uint64_t now_us);
/* 74. Stopping a stopped timer is OK. */
uint32_t lv2_timer_stop(lv2_timer_table *table, uint32_t id);
/* 75. The adapter has already checked that `queue_id` names a live queue. EISCONN when connected. */
uint32_t lv2_timer_connect(lv2_timer_table *table, uint32_t id, uint32_t queue_id, uint64_t name,
                           uint64_t data1, uint64_t data2);
/* 76. ENOTCONN when not connected. Also stops the timer. */
uint32_t lv2_timer_disconnect(lv2_timer_table *table, uint32_t id);

/* Collects the timers that have expired at `now_us`: at most one event per timer per call. A periodic
 * timer advances by one period, or to `now_us + period` when it has fallen more than one period behind
 * (so a stalled host does not produce a burst); a one-shot timer stops. Returns the number written. */
size_t lv2_timer_collect_due(lv2_timer_table *table, uint64_t now_us, lv2_timer_event *out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* LV2_TIMER_H */
