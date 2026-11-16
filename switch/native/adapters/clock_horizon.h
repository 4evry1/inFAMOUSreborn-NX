#ifndef PS3_CLOCK_HORIZON_H
#define PS3_CLOCK_HORIZON_H
#include "../../../ps3recomp/runtime/time/clock.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_horizon_clock {
    uint64_t origin_ticks;
    uint64_t frequency;
    unsigned initialized;
} ps3_horizon_clock;
/* Fresh/quiescent storage only. Init failure leaves storage unchanged. After
 * success storage is immutable and borrowed by every port until users join. */
ps3_clock_status ps3_horizon_clock_init(ps3_horizon_clock *clock);
ps3_clock ps3_horizon_clock_port(ps3_horizon_clock *clock);
#ifdef __cplusplus
}
#endif
#endif
