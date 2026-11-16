#ifndef NATIVE_SERVICE_THREAD_H
#define NATIVE_SERVICE_THREAD_H
#ifdef __cplusplus
extern "C" {
#endif
/* Injected host-thread port for one owned native service worker.
 * start: create exactly one thread running entry(argument); 0 on success,
 * nonzero leaves no thread. join: wait for that thread; called exactly once
 * after a successful start. OS details live only in adapters. */
typedef struct ps3_service_thread_port {
    void *context;
    int (*start)(void *context, void (*entry)(void *), void *argument);
    int (*join)(void *context);
} ps3_service_thread_port;
#ifdef __cplusplus
}
#endif
#endif
