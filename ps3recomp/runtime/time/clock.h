#ifndef PS3_RUNTIME_CLOCK_H
#define PS3_RUNTIME_CLOCK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum ps3_clock_status {
    PS3_CLOCK_OK = 0,
    PS3_CLOCK_ARGUMENT,
    PS3_CLOCK_RANGE,
    PS3_CLOCK_PLATFORM_FAILURE
} ps3_clock_status;
/* Caller owns the clock and its immutable epoch until all users join. All
 * instance workers share this epoch. Successful samples are real elapsed
 * microseconds; equal adjacent readings are allowed. Failure leaves output
 * unchanged. Provider calls return normally and support concurrent readers.
 * There is no guest-visible CELL error encoded in a timestamp. */
typedef struct ps3_clock {
    void *context;
    ps3_clock_status (*now_microseconds)(void *, uint64_t *);
} ps3_clock;
ps3_clock_status ps3_clock_elapsed_microseconds(
    uint64_t origin, uint64_t ticks, uint64_t frequency, uint64_t *output);
#ifdef __cplusplus
}
#endif
#endif
