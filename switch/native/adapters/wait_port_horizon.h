#ifndef PS3_NATIVE_WAIT_PORT_HORIZON_H
#define PS3_NATIVE_WAIT_PORT_HORIZON_H
#include <stdint.h>
#include <switch.h>
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Horizon realisation of ps3_notify_port: libnx Mutex/CondVar with relative
 * timeouts derived from the monotonic system tick. Deadlines use
 * ps3_horizon_wait_port_now_ns(). Composition owns storage; destroy only after
 * every waiter returned. */
typedef struct ps3_horizon_wait_port { Mutex mutex; CondVar cond; } ps3_horizon_wait_port;
void ps3_horizon_wait_port_init(ps3_horizon_wait_port *);
ps3_notify_port ps3_horizon_wait_port_port(ps3_horizon_wait_port *);
uint64_t ps3_horizon_wait_port_now_ns(void);
#ifdef __cplusplus
}
#endif
#endif
