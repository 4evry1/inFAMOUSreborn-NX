#ifndef NATIVE_SERVICE_THREAD_PTHREAD_H
#define NATIVE_SERVICE_THREAD_PTHREAD_H
#include <pthread.h>
#include "../domain/service_thread.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ps3_pthread_service_thread {
    pthread_t thread;
    void (*entry)(void *);
    void *argument;
    int fail_next_start; /* composition/test-controlled resource exhaustion */
    int fail_next_join;  /* test-controlled: report a join failure WITHOUT joining (retry later succeeds) */
    int started;
} ps3_pthread_service_thread;
ps3_service_thread_port ps3_pthread_service_thread_port(ps3_pthread_service_thread *);
#ifdef __cplusplus
}
#endif
#endif
