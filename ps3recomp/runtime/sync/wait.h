#ifndef PS3_RUNTIME_WAIT_H
#define PS3_RUNTIME_WAIT_H
#include "guard.h"
#include <stdint.h>
typedef enum ps3_wait_status {
    PS3_WAIT_READY = 0,
    PS3_WAIT_TIMEOUT,
    PS3_WAIT_CANCELLED,
    PS3_WAIT_FAILURE
} ps3_wait_status;
typedef struct ps3_wait_cancel {
    void *context;
    int (*is_cancelled)(void *context);
} ps3_wait_cancel;
/* now and absolute deadlines share a monotonic nanosecond clock. UINT64_MAX
 * means infinite. wait atomically releases/reacquires this exact guard, including
 * on error. Spurious READY is allowed. notify_all is total for a live adapter.
 * Cancellation predicates are total/nonblocking and never reenter the registry.
 * Composition disables all host thread cancellation (including deferred
 * pthread cancellation) and nonlocal unwinding throughout registry operations.
 * Only ps3_wait_cancel is supported. Composition owns adapter lifetime. */
typedef struct ps3_wait_port {
    void *context;
    ps3_wait_status (*now)(void *context, uint64_t *nanoseconds);
    ps3_wait_status (*wait)(void *context, const ps3_instance_guard *guard,
                            uint64_t absolute_deadline, ps3_wait_cancel cancel);
    void (*notify_all)(void *context);
} ps3_wait_port;
#endif
