#ifndef PS3_RUNTIME_TIMEBASE_H
#define PS3_RUNTIME_TIMEBASE_H
#include <stdint.h>

#define PS3_TIMEBASE_FREQUENCY UINT64_C(79800000)

#ifdef __cplusplus
extern "C" {
#endif
/* Convert elapsed microseconds to floor(us * frequency / 1000000), modulo
 * 2^64. Every uint64_t input is defined. This neither samples a clock nor
 * changes its epoch; conversion does not increase source clock resolution. */
uint64_t ps3_timebase_from_microseconds(uint64_t elapsed_microseconds);
#ifdef __cplusplus
}
#endif
#endif
