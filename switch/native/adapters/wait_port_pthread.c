#define _POSIX_C_SOURCE 200809L
#include "wait_port_pthread.h"
#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* Host realisation: the condvar is bound to CLOCK_MONOTONIC (no realtime
 * substitution); init fails if the clock cannot be selected. */
uint64_t ps3_pthread_wait_port_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) abort();
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}
int ps3_pthread_wait_port_init(ps3_pthread_wait_port *p) {
    if (!p) return EINVAL;
    p->wait_calls = 0;
    p->sleepers = 0;
    pthread_condattr_t attributes;
    int rc = pthread_condattr_init(&attributes);
    if (rc) return rc;
    rc = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_mutex_init(&p->mutex, NULL);
    if (!rc) {
        rc = pthread_cond_init(&p->cond, &attributes);
        if (rc) pthread_mutex_destroy(&p->mutex);
    }
    pthread_condattr_destroy(&attributes);
    return rc;
}
void ps3_pthread_wait_port_destroy(ps3_pthread_wait_port *p) {
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mutex);
}
static int still_waiting(const uint64_t *word, uint64_t expected, const uint32_t *cancel) {
    return __atomic_load_n(word, __ATOMIC_SEQ_CST) == expected &&
           !(cancel && __atomic_load_n(cancel, __ATOMIC_SEQ_CST));
}
static ps3_notify_wait_result wait_impl(void *context, const uint64_t *word, uint64_t expected,
                                      const uint32_t *cancel, uint64_t deadline_ns) {
    ps3_pthread_wait_port *p = context;
    ps3_notify_wait_result result = PS3_NOTIFY_WOKEN;
    if (pthread_mutex_lock(&p->mutex)) return PS3_NOTIFY_FAILURE;
    ++p->wait_calls;
    ++p->sleepers;
    struct timespec at = {(time_t)(deadline_ns / 1000000000u), (long)(deadline_ns % 1000000000u)};
    while (still_waiting(word, expected, cancel)) {
        int rc;
        if (deadline_ns == UINT64_MAX) rc = pthread_cond_wait(&p->cond, &p->mutex);
        else {
            if (ps3_pthread_wait_port_now_ns() >= deadline_ns) { result = PS3_NOTIFY_TIMEOUT; break; }
            rc = pthread_cond_timedwait(&p->cond, &p->mutex, &at);
            if (rc == ETIMEDOUT) rc = 0; /* loop re-evaluates predicate and deadline */
        }
        if (rc) { result = PS3_NOTIFY_FAILURE; break; }
    }
    --p->sleepers;
    if (pthread_mutex_unlock(&p->mutex)) return PS3_NOTIFY_FAILURE;
    return result;
}
static void wake_all_impl(void *context) {
    ps3_pthread_wait_port *p = context;
    if (pthread_mutex_lock(&p->mutex) || pthread_cond_broadcast(&p->cond) || pthread_mutex_unlock(&p->mutex)) abort();
}
ps3_notify_port ps3_pthread_wait_port_port(ps3_pthread_wait_port *p) {
    ps3_notify_port port = {p, wait_impl, wake_all_impl};
    return port;
}
uint64_t ps3_pthread_wait_port_wait_calls(ps3_pthread_wait_port *p) {
    pthread_mutex_lock(&p->mutex);
    uint64_t v = p->wait_calls;
    pthread_mutex_unlock(&p->mutex);
    return v;
}
uint32_t ps3_pthread_wait_port_sleepers(ps3_pthread_wait_port *p) {
    pthread_mutex_lock(&p->mutex);
    uint32_t v = p->sleepers;
    pthread_mutex_unlock(&p->mutex);
    return v;
}
