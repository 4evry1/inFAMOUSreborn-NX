#ifndef NATIVE_SERVICE_THREAD_HORIZON_H
#define NATIVE_SERVICE_THREAD_HORIZON_H
#include <stddef.h>
#include <switch.h>
#include "../domain/service_thread.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Horizon realisation of ps3_service_thread_port (libnx threadCreate/threadStart/
 * threadWaitForExit/threadClose, pinned devkitA64/libnx). Same semantics as the host pthread
 * adapter: start creates exactly one thread (nonzero = no thread exists and nothing is leaked);
 * join returns 0 only after the thread's exit is CONFIRMED and its handle closed. A failed join
 * (wait or close) leaves the record started/close-pending so the owner keeps every borrowed
 * resource and may retry; callers never free anything the entry uses before a 0 return.
 * Composition owns the storage; the stack is libnx-allocated (stack_bytes) and freed by close. */
typedef struct ps3_horizon_service_thread {
    Thread thread;
    void (*entry)(void *);
    void *argument;
    size_t stack_bytes;
    int priority, cpu;
    unsigned state;   /* 0 idle, 1 running (started), 2 exited, close pending */
    int wide;         /* Root (Decision 108): 1 = `cpu` is only the preferred core; the thread may run on any of cores 0..2 */
} ps3_horizon_service_thread;
/* stack_bytes: page-aligned size for the libnx-allocated stack (0 = 0x10000). */
void ps3_horizon_service_thread_init(ps3_horizon_service_thread *, size_t stack_bytes);
ps3_service_thread_port ps3_horizon_service_thread_port(ps3_horizon_service_thread *);
#ifdef __cplusplus
}
#endif
#endif
