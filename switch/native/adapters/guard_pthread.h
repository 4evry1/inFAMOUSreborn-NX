#ifndef PS3_PTHREAD_GUARD_H
#define PS3_PTHREAD_GUARD_H
#include <pthread.h>
#include "../../../ps3recomp/runtime/sync/guard.h"
typedef struct { pthread_mutex_t mutex; } ps3_pthread_guard;
int ps3_pthread_guard_init(ps3_pthread_guard *);
int ps3_pthread_guard_destroy(ps3_pthread_guard *);
ps3_instance_guard ps3_pthread_guard_port(ps3_pthread_guard *);
#endif
