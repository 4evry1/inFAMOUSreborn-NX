#define _POSIX_C_SOURCE 200809L
#include "guard_pthread.h"
#include <stdlib.h>
static void lock(void *context) {
    if (pthread_mutex_lock(&((ps3_pthread_guard *)context)->mutex)) abort();
}
static void unlock(void *context) {
    if (pthread_mutex_unlock(&((ps3_pthread_guard *)context)->mutex)) abort();
}
int ps3_pthread_guard_init(ps3_pthread_guard *guard) {
    pthread_mutexattr_t attributes;
    int result = pthread_mutexattr_init(&attributes);
    if (result) return result;
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
    if (!result) result = pthread_mutex_init(&guard->mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    return result;
}
int ps3_pthread_guard_destroy(ps3_pthread_guard *guard) {
    return pthread_mutex_destroy(&guard->mutex);
}
ps3_instance_guard ps3_pthread_guard_port(ps3_pthread_guard *guard) {
    ps3_instance_guard port = {guard, lock, unlock};
    return port;
}
