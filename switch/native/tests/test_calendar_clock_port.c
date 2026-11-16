#include "calendar_clock.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d expression=%s\n",__LINE__,#x); } } while (0)
struct provider { ps3_calendar_clock_status status; ps3_calendar_time sample; unsigned calls; };
static ps3_calendar_clock_status sample(void *context,ps3_calendar_time *out) {
    struct provider *state=context; ++state->calls; *out=state->sample; return state->status;
}
static ps3_calendar_clock_status stateless(void *context,ps3_calendar_time *out) {
    (void)context; *out=(ps3_calendar_time){-7,19}; return PS3_CALENDAR_CLOCK_OK;
}
int main(void) {
    struct provider state={PS3_CALENDAR_CLOCK_OK,{123,456},0};
    ps3_calendar_clock clock={&state,sample},missing={&state,NULL};
    ps3_calendar_time out={-91,92};
    CHECK(ps3_calendar_clock_read(NULL,&out)==PS3_CALENDAR_CLOCK_INVALID_ARGUMENT);
    CHECK(ps3_calendar_clock_read(&missing,&out)==PS3_CALENDAR_CLOCK_INVALID_ARGUMENT);
    CHECK(ps3_calendar_clock_read(&clock,NULL)==PS3_CALENDAR_CLOCK_INVALID_ARGUMENT);
    CHECK(out.seconds==-91 && out.nanoseconds==92 && state.calls==0);
    CHECK(ps3_calendar_clock_read(&clock,&out)==PS3_CALENDAR_CLOCK_OK);
    CHECK(out.seconds==123 && out.nanoseconds==456 && state.calls==1);
    const ps3_calendar_clock_status errors[]={PS3_CALENDAR_CLOCK_INVALID_ARGUMENT,PS3_CALENDAR_CLOCK_PROVIDER_FAILURE,PS3_CALENDAR_CLOCK_INVALID_SAMPLE,(ps3_calendar_clock_status)99,(ps3_calendar_clock_status)-1};
    for(size_t n=0;n<sizeof(errors)/sizeof(errors[0]);++n) {
        state.status=errors[n]; state.sample=(ps3_calendar_time){777,UINT32_MAX}; out=(ps3_calendar_time){-91,92};
        CHECK(ps3_calendar_clock_read(&clock,&out)==(n<3?errors[n]:PS3_CALENDAR_CLOCK_INVALID_SAMPLE));
        CHECK(out.seconds==-91 && out.nanoseconds==92);
    }
    state.status=PS3_CALENDAR_CLOCK_OK;
    const uint32_t invalid[]={1000000000u,UINT32_MAX};
    for(size_t n=0;n<sizeof(invalid)/sizeof(invalid[0]);++n) {
        state.sample=(ps3_calendar_time){777,invalid[n]}; out=(ps3_calendar_time){-91,92};
        CHECK(ps3_calendar_clock_read(&clock,&out)==PS3_CALENDAR_CLOCK_INVALID_SAMPLE);
        CHECK(out.seconds==-91 && out.nanoseconds==92);
    }
    const ps3_calendar_time valid[]={{INT64_MIN,0},{INT64_MAX,999999999u},{-1,1},{0,0}};
    for(size_t n=0;n<sizeof(valid)/sizeof(valid[0]);++n) {
        const unsigned before=state.calls; state.sample=valid[n];
        CHECK(ps3_calendar_clock_read(&clock,&out)==PS3_CALENDAR_CLOCK_OK);
        CHECK(out.seconds==valid[n].seconds && out.nanoseconds==valid[n].nanoseconds && state.calls==before+1);
    }
    ps3_calendar_clock null_context={NULL,stateless};
    CHECK(ps3_calendar_clock_read(&null_context,&out)==PS3_CALENDAR_CLOCK_OK);
    CHECK(out.seconds==-7 && out.nanoseconds==19);
    printf("CALENDAR_CLOCK_PORT_%s checks=%u failures=%u pure_contract_only=true\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
