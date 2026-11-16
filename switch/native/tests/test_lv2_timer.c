/* Host test for native/domain/lv2_timer.{h,c}. */
#include "../domain/lv2_timer.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_lifecycle(void)
{
    lv2_timer_table t;
    lv2_timer_information info;
    uint32_t id = 0, other = 0, ids[LV2_TIMER_CAPACITY], i;
    lv2_timer_table_init(&t, 0x1000);

    CHECK(lv2_timer_create(&t, NULL) == LV2_TIMER_EINVAL);
    CHECK(lv2_timer_create(&t, &id) == LV2_TIMER_OK);
    CHECK((id & 0xFF000000u) == LV2_TIMER_ID_BASE && id != 0);
    CHECK(lv2_timer_destroy(&t, 0x12345678u) == LV2_TIMER_ESRCH);
    CHECK(lv2_timer_stop(&t, id + 1) == LV2_TIMER_ESRCH);

    CHECK(lv2_timer_start(&t, id, 0, 1000, 0) == LV2_TIMER_ENOTCONN);
    CHECK(lv2_timer_disconnect(&t, id) == LV2_TIMER_ENOTCONN);
    CHECK(lv2_timer_connect(&t, id, 77, 0, 5, 6) == LV2_TIMER_OK);
    CHECK(lv2_timer_connect(&t, id, 78, 0, 5, 6) == LV2_TIMER_EISCONN);
    CHECK(t.timers[id & 0xFF].source == (((uint64_t)0x1000) << 32 | id));
    CHECK(lv2_timer_destroy(&t, id) == LV2_TIMER_EISCONN);

    CHECK(lv2_timer_start(&t, id, 0, 99, 0) == LV2_TIMER_EINVAL);
    CHECK(lv2_timer_start(&t, id, 0, 0, 0) == LV2_TIMER_EINVAL);
    CHECK(lv2_timer_start(&t, id, 500, 0, 500) == LV2_TIMER_ETIMEDOUT);
    CHECK(lv2_timer_get_information(&t, id, &info) == LV2_TIMER_OK && info.state == LV2_TIMER_STATE_STOP && info.next_us == 0);
    CHECK(lv2_timer_start(&t, id, 0, 1000, 10000) == LV2_TIMER_OK);
    CHECK(lv2_timer_start(&t, id, 0, 1000, 10000) == LV2_TIMER_EBUSY);
    CHECK(lv2_timer_get_information(&t, id, &info) == LV2_TIMER_OK && info.state == LV2_TIMER_STATE_RUN &&
          info.next_us == 11000 && info.period_us == 1000);
    CHECK(lv2_timer_get_information(&t, id, NULL) == LV2_TIMER_EINVAL);
    CHECK(lv2_timer_stop(&t, id) == LV2_TIMER_OK);
    CHECK(lv2_timer_stop(&t, id) == LV2_TIMER_OK);
    CHECK(lv2_timer_disconnect(&t, id) == LV2_TIMER_OK);
    CHECK(lv2_timer_destroy(&t, id) == LV2_TIMER_OK);
    CHECK(lv2_timer_destroy(&t, id) == LV2_TIMER_ESRCH);

    CHECK(lv2_timer_create(&t, &other) == LV2_TIMER_OK);
    CHECK(other != id && (other & 0xFF) == (id & 0xFF));     /* same slot, new generation: the old id is dead */
    CHECK(lv2_timer_stop(&t, id) == LV2_TIMER_ESRCH);
    CHECK(lv2_timer_destroy(&t, other) == LV2_TIMER_OK);

    for (i = 0; i < LV2_TIMER_CAPACITY; ++i) CHECK(lv2_timer_create(&t, &ids[i]) == LV2_TIMER_OK);
    id = 0xDEAD;
    CHECK(lv2_timer_create(&t, &id) == LV2_TIMER_EAGAIN && id == 0xDEAD);
}

static void test_expiry(void)
{
    lv2_timer_table t;
    lv2_timer_event ev[4];
    uint32_t periodic = 0, shot = 0;
    lv2_timer_table_init(&t, 1);
    CHECK(lv2_timer_create(&t, &periodic) == LV2_TIMER_OK);
    CHECK(lv2_timer_create(&t, &shot) == LV2_TIMER_OK);
    CHECK(lv2_timer_connect(&t, periodic, 10, 0xABCD, 1, 2) == LV2_TIMER_OK);
    CHECK(lv2_timer_connect(&t, shot, 11, 0, 3, 4) == LV2_TIMER_OK);

    CHECK(lv2_timer_start(&t, periodic, 0, 5333, 100000) == LV2_TIMER_OK);
    CHECK(lv2_timer_start(&t, shot, 108000, 0, 100000) == LV2_TIMER_OK);
    CHECK(lv2_timer_collect_due(&t, 105332, ev, 4) == 0);
    CHECK(lv2_timer_collect_due(&t, 105333, ev, 4) == 1);
    CHECK(ev[0].queue_id == 10 && ev[0].source == 0xABCD && ev[0].data1 == 1 && ev[0].data2 == 2);
    CHECK(lv2_timer_collect_due(&t, 105333, ev, 4) == 0);
    CHECK(lv2_timer_collect_due(&t, 108000, ev, 4) == 1);    /* the one shot */
    CHECK(ev[0].queue_id == 11 && ev[0].source == (((uint64_t)1) << 32 | shot) && ev[0].data1 == 3);
    CHECK(t.timers[shot & 0xFF].running == 0);
    CHECK(lv2_timer_collect_due(&t, 110666, ev, 4) == 1);    /* second period, on the grid */
    CHECK(t.timers[periodic & 0xFF].next_us == 100000 + 3 * 5333);
    /* a long stall: one event, then one period from now (no burst) */
    CHECK(lv2_timer_collect_due(&t, 900000, ev, 4) == 1);
    CHECK(t.timers[periodic & 0xFF].next_us == 900000 + 5333);
    CHECK(lv2_timer_collect_due(&t, 900001, ev, 4) == 0);
    CHECK(t.timers[periodic & 0xFF].fired == 3 && t.events == 4);

    /* base in the past with a period: next grid point after now */
    CHECK(lv2_timer_stop(&t, periodic) == LV2_TIMER_OK);
    CHECK(lv2_timer_start(&t, periodic, 1000, 1000, 5500) == LV2_TIMER_OK);
    CHECK(t.timers[periodic & 0xFF].next_us == 6000);
    CHECK(lv2_timer_stop(&t, periodic) == LV2_TIMER_OK);
    CHECK(lv2_timer_start(&t, periodic, 9000, 1000, 5500) == LV2_TIMER_OK);
    CHECK(t.timers[periodic & 0xFF].next_us == 9000);
    /* capacity 0 and a disconnected timer deliver nothing */
    CHECK(lv2_timer_collect_due(&t, 20000, ev, 0) == 0);
    CHECK(lv2_timer_disconnect(&t, periodic) == LV2_TIMER_OK);
    CHECK(lv2_timer_collect_due(&t, 30000, ev, 4) == 0);
}

int main(void)
{
    test_lifecycle();
    test_expiry();
    printf("LV2_TIMER checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
