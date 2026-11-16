#define _POSIX_C_SOURCE 200809L
#include "calendar_clock_horizon.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d expression=%s\n",__LINE__,#x); } } while (0)
#ifdef CALENDAR_CLOCK_TEST_SHIM
static struct timespec scripted;
static int returned;
static unsigned calls;
static clockid_t observed_clock;
int calendar_clock_test_gettime(clockid_t clock,struct timespec *out) {
    ++calls; observed_clock=clock; *out=scripted; errno=EIO; return returned;
}
#endif
int main(void) {
    ps3_calendar_clock clock=calendar_clock_horizon_port();
    CHECK(clock.context==NULL && clock.sample!=NULL);
    ps3_calendar_time out={-90,91};
#ifdef CALENDAR_CLOCK_TEST_SHIM
    CHECK(calls==0);
    CHECK(clock.sample(clock.context,NULL)==PS3_CALENDAR_CLOCK_INVALID_ARGUMENT && calls==0);
    scripted=(struct timespec){123,456}; returned=0;
    CHECK(clock.sample(clock.context,&out)==PS3_CALENDAR_CLOCK_OK);
    CHECK(out.seconds==123 && out.nanoseconds==456 && calls==1 && observed_clock==CLOCK_REALTIME);
    const int errors[]={-1,1};
    for(unsigned n=0;n<2;++n) {
        returned=errors[n]; scripted=(struct timespec){777,888}; out=(ps3_calendar_time){-90,91};
        CHECK(clock.sample(clock.context,&out)==PS3_CALENDAR_CLOCK_PROVIDER_FAILURE);
        CHECK(out.seconds==-90 && out.nanoseconds==91);
    }
    returned=0;
    const long invalid[]={-1,1000000000L,LONG_MAX
#if LONG_MAX > UINT32_MAX
        ,4294967303L
#endif
    };
    for(unsigned n=0;n<sizeof(invalid)/sizeof(invalid[0]);++n) {
        scripted=(struct timespec){777,invalid[n]}; out=(ps3_calendar_time){-90,91};
        CHECK(clock.sample(clock.context,&out)==PS3_CALENDAR_CLOCK_INVALID_SAMPLE);
        CHECK(out.seconds==-90 && out.nanoseconds==91);
    }
    const int64_t seconds[]={INT64_MIN,INT64_MAX,-1,0};
    for(unsigned n=0;n<4;++n) {
        scripted=(struct timespec){(time_t)seconds[n],999999999L};
        CHECK(clock.sample(clock.context,&out)==PS3_CALENDAR_CLOCK_OK);
        CHECK(out.seconds==seconds[n] && out.nanoseconds==999999999u);
    }
    returned=-1; out=(ps3_calendar_time){-90,91};
    CHECK(ps3_calendar_clock_read(&clock,&out)==PS3_CALENDAR_CLOCK_PROVIDER_FAILURE);
    CHECK(out.seconds==-90 && out.nanoseconds==91);
    puts("CALENDAR_ADAPTER_HOST_SHIM sdk_provider_executed=false");
#else
    CHECK(ps3_calendar_clock_read(&clock,&out)==PS3_CALENDAR_CLOCK_OK);
    CHECK(out.nanoseconds<1000000000u);
    printf("CALENDAR_ADAPTER_HOST_POSIX seconds=%lld nanoseconds=%u sdk_provider_executed=false\n",(long long)out.seconds,out.nanoseconds);
#endif
    printf("CALENDAR_CLOCK_HORIZON_HOST_%s checks=%u failures=%u native_execution=false\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
