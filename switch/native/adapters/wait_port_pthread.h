#ifndef PS3_NATIVE_WAIT_PORT_PTHREAD_H
#define PS3_NATIVE_WAIT_PORT_PTHREAD_H
#include <pthread.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
/* pthread mutex/condvar realisation of ps3_notify_port. Deadlines use
 * ps3_pthread_wait_port_now_ns() (CLOCK_MONOTONIC). Composition owns the
 * storage and destroys it only after every waiter has returned. */
typedef struct ps3_pthread_wait_port {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint64_t wait_calls;
    uint32_t sleepers;
} ps3_pthread_wait_port;
int ps3_pthread_wait_port_init(ps3_pthread_wait_port *);
void ps3_pthread_wait_port_destroy(ps3_pthread_wait_port *);
ps3_notify_port ps3_pthread_wait_port_port(ps3_pthread_wait_port *);
uint64_t ps3_pthread_wait_port_now_ns(void);
/* Diagnostics (race-free reads). */
uint64_t ps3_pthread_wait_port_wait_calls(ps3_pthread_wait_port *);
uint32_t ps3_pthread_wait_port_sleepers(ps3_pthread_wait_port *);
#ifdef __cplusplus
}
#endif
#endif
