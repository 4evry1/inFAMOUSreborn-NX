/* lv2 timer objects. See lv2_timer.h for the contract and its provenance. */
#include "lv2_timer.h"

#include <string.h>

void lv2_timer_table_init(lv2_timer_table *table, uint32_t process_id)
{
    if (table == NULL) return;
    memset(table, 0, sizeof(*table));
    table->process_id = process_id;
}

static lv2_timer *find(lv2_timer_table *table, uint32_t id)
{
    uint32_t slot;
    if (table == NULL || (id & 0xFF000000u) != LV2_TIMER_ID_BASE) return NULL;
    slot = id & 0xFFu;
    if (slot >= LV2_TIMER_CAPACITY) return NULL;
    if (!table->timers[slot].in_use || table->timers[slot].id != id) return NULL;
    return &table->timers[slot];
}

uint32_t lv2_timer_create(lv2_timer_table *table, uint32_t *id_out)
{
    uint32_t slot;
    if (table == NULL || id_out == NULL) return LV2_TIMER_EINVAL;
    for (slot = 0; slot < LV2_TIMER_CAPACITY; ++slot) {
        lv2_timer *timer = &table->timers[slot];
        if (timer->in_use) continue;
        {
            const uint32_t generation = (timer->generation + 1u) & 0xFFFFu;
            memset(timer, 0, sizeof(*timer));
            timer->generation = generation;
            timer->in_use = 1u;
            timer->id = LV2_TIMER_ID_BASE | (generation << 8) | slot;
        }
        *id_out = timer->id;
        ++table->creates;
        return LV2_TIMER_OK;
    }
    return LV2_TIMER_EAGAIN;
}

uint32_t lv2_timer_destroy(lv2_timer_table *table, uint32_t id)
{
    lv2_timer *timer = find(table, id);
    if (timer == NULL) return LV2_TIMER_ESRCH;
    if (timer->connected) return LV2_TIMER_EISCONN;
    timer->in_use = 0u;
    timer->running = 0u;
    return LV2_TIMER_OK;
}

uint32_t lv2_timer_get_information(const lv2_timer_table *table, uint32_t id, lv2_timer_information *out)
{
    const lv2_timer *timer = find((lv2_timer_table *)table, id);
    if (timer == NULL) return LV2_TIMER_ESRCH;
    if (out == NULL) return LV2_TIMER_EINVAL;
    memset(out, 0, sizeof(*out));
    if (timer->running) {
        out->next_us = timer->next_us;
        out->period_us = timer->period_us;
        out->state = LV2_TIMER_STATE_RUN;
    }
    return LV2_TIMER_OK;
}

uint32_t lv2_timer_start(lv2_timer_table *table, uint32_t id, uint64_t base_us, uint64_t period_us, uint64_t now_us)
{
    lv2_timer *timer = find(table, id);
    uint64_t first;
    if (timer == NULL) return LV2_TIMER_ESRCH;
    if (period_us != 0u && period_us < LV2_TIMER_MIN_PERIOD_US) return LV2_TIMER_EINVAL;
    if (period_us == 0u && base_us == 0u) return LV2_TIMER_EINVAL;
    if (!timer->connected) return LV2_TIMER_ENOTCONN;
    if (timer->running) return LV2_TIMER_EBUSY;
    if (period_us == 0u) {
        if (base_us <= now_us) return LV2_TIMER_ETIMEDOUT;
        first = base_us;
    } else if (base_us == 0u) {
        first = now_us + period_us;
    } else if (base_us > now_us) {
        first = base_us;
    } else {
        /* A base in the past: the next point of the same grid after now. */
        first = base_us + ((now_us - base_us) / period_us + 1u) * period_us;
    }
    timer->next_us = first;
    timer->period_us = period_us;
    timer->running = 1u;
    ++table->starts;
    return LV2_TIMER_OK;
}

uint32_t lv2_timer_stop(lv2_timer_table *table, uint32_t id)
{
    lv2_timer *timer = find(table, id);
    if (timer == NULL) return LV2_TIMER_ESRCH;
    timer->running = 0u;
    return LV2_TIMER_OK;
}

uint32_t lv2_timer_connect(lv2_timer_table *table, uint32_t id, uint32_t queue_id, uint64_t name,
                           uint64_t data1, uint64_t data2)
{
    lv2_timer *timer = find(table, id);
    if (timer == NULL) return LV2_TIMER_ESRCH;
    if (timer->connected) return LV2_TIMER_EISCONN;
    timer->connected = 1u;
    timer->queue_id = queue_id;
    timer->source = name != 0u ? name : (((uint64_t)table->process_id) << 32) | (uint64_t)id;
    timer->data1 = data1;
    timer->data2 = data2;
    return LV2_TIMER_OK;
}

uint32_t lv2_timer_disconnect(lv2_timer_table *table, uint32_t id)
{
    lv2_timer *timer = find(table, id);
    if (timer == NULL) return LV2_TIMER_ESRCH;
    if (!timer->connected) return LV2_TIMER_ENOTCONN;
    timer->connected = 0u;
    timer->running = 0u;
    return LV2_TIMER_OK;
}

size_t lv2_timer_collect_due(lv2_timer_table *table, uint64_t now_us, lv2_timer_event *out, size_t capacity)
{
    size_t count = 0;
    uint32_t slot;
    if (table == NULL || out == NULL) return 0;
    for (slot = 0; slot < LV2_TIMER_CAPACITY && count < capacity; ++slot) {
        lv2_timer *timer = &table->timers[slot];
        if (!timer->in_use || !timer->running || !timer->connected || timer->next_us > now_us) continue;
        out[count].queue_id = timer->queue_id;
        out[count].source = timer->source;
        out[count].data1 = timer->data1;
        out[count].data2 = timer->data2;
        ++count;
        ++timer->fired;
        ++table->events;
        if (timer->period_us == 0u) {
            timer->running = 0u;
        } else {
            timer->next_us += timer->period_us;
            if (timer->next_us <= now_us) timer->next_us = now_us + timer->period_us;
        }
    }
    return count;
}
