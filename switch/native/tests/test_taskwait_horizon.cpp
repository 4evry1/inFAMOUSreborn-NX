#include "taskwait_horizon.h"
#include <cstdio>
#include <cstdint>
#include <initializer_list>
static unsigned calls;
static int64_t last_sleep;
extern "C" void svcSleepThread(int64_t duration) { ++calls; last_sleep=duration; }
int main() {
    unsigned failures=0;
    auto require=[&](bool ok) { if(!ok)++failures; };
    require(!startup_taskwait_horizon_yield_once(nullptr));
    for(int64_t invalid : {INT64_C(-1),INT64_C(0),STARTUP_TASKWAIT_MAX_SLEEP_NS+1,INT64_MAX}) {
        StartupTaskwaitHorizonConfig config{invalid};require(!startup_taskwait_horizon_yield_once(&config));
    }
    require(calls==0);
    StartupTaskwaitHorizonConfig config{INT64_C(1000000)};
    require(startup_taskwait_horizon_yield_once(&config) && calls==1 && last_sleep==1000000);
    config.sleep_ns=STARTUP_TASKWAIT_MAX_SLEEP_NS;
    require(startup_taskwait_horizon_yield_once(&config) && calls==2 && last_sleep==STARTUP_TASKWAIT_MAX_SLEEP_NS);
    std::printf("TASKWAIT_HORIZON_CONTRACT_FAILURES=%u svc_boundary=host_standin actual_scheduling_proven=false\n",failures);
    return failures?1:0;
}
