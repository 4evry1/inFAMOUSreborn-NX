#ifndef PS3_RUNTIME_GUEST_TIMEBASE_H
#define PS3_RUNTIME_GUEST_TIMEBASE_H
#include "timebase.h"

/* Bundled recomp compatibility policy, paired with guest mftb/mftbu scaling.
 * Host counter frequency and the shared clock's microsecond unit are separate. */
#define PS3_GUEST_TIMEBASE_FREQUENCY_HZ PS3_TIMEBASE_FREQUENCY

#endif
