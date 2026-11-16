#define _POSIX_C_SOURCE 200809L
#include "service_thread_pthread.h"
#include <errno.h>
static void *trampoline(void *p) {
    ps3_pthread_service_thread *t = p;
    t->entry(t->argument);
    return NULL;
}
static int start_impl(void *context, void (*entry)(void *), void *argument) {
    ps3_pthread_service_thread *t = context;
    if (!entry || t->started) return EINVAL;
    if (t->fail_next_start) { t->fail_next_start = 0; return EAGAIN; }
    t->entry = entry;
    t->argument = argument;
    int rc = pthread_create(&t->thread, NULL, trampoline, t);
    if (!rc) t->started = 1;
    return rc;
}
static int join_impl(void *context) {
    ps3_pthread_service_thread *t = context;
    if (!t->started) return EINVAL;
    if (t->fail_next_join) { t->fail_next_join = 0; return EIO; }
    int rc = pthread_join(t->thread, NULL);
    if (!rc) t->started = 0;
    return rc;
}
ps3_service_thread_port ps3_pthread_service_thread_port(ps3_pthread_service_thread *t) {
    ps3_service_thread_port port = {t, start_impl, join_impl};
    return port;
}
