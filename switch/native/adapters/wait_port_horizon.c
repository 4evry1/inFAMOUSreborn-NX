#include "wait_port_horizon.h"

uint64_t ps3_horizon_wait_port_now_ns(void) { return armTicksToNs(armGetSystemTick()); }
void ps3_horizon_wait_port_init(ps3_horizon_wait_port *p) {
    mutexInit(&p->mutex);
    condvarInit(&p->cond);
}
static int still_waiting(const uint64_t *word, uint64_t expected, const uint32_t *cancel) {
    return __atomic_load_n(word, __ATOMIC_SEQ_CST) == expected &&
           !(cancel && __atomic_load_n(cancel, __ATOMIC_SEQ_CST));
}
static ps3_notify_wait_result wait_impl(void *context, const uint64_t *word, uint64_t expected,
                                      const uint32_t *cancel, uint64_t deadline_ns) {
    ps3_horizon_wait_port *p = context;
    ps3_notify_wait_result result = PS3_NOTIFY_WOKEN;
    mutexLock(&p->mutex);
    while (still_waiting(word, expected, cancel)) {
        uint64_t timeout = UINT64_MAX;
        if (deadline_ns != UINT64_MAX) {
            uint64_t now = ps3_horizon_wait_port_now_ns();
            if (now >= deadline_ns) { result = PS3_NOTIFY_TIMEOUT; break; }
            timeout = deadline_ns - now;
        }
        Result rc = condvarWaitTimeout(&p->cond, &p->mutex, timeout);
        if (R_FAILED(rc) && R_VALUE(rc) != KERNELRESULT(TimedOut)) { result = PS3_NOTIFY_FAILURE; break; }
    }
    mutexUnlock(&p->mutex);
    return result;
}
static void wake_all_impl(void *context) {
    ps3_horizon_wait_port *p = context;
    mutexLock(&p->mutex);
    condvarWakeAll(&p->cond);
    mutexUnlock(&p->mutex);
}
ps3_notify_port ps3_horizon_wait_port_port(ps3_horizon_wait_port *p) {
    ps3_notify_port port = {p, wait_impl, wake_all_impl};
    return port;
}
