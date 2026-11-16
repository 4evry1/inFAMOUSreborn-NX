#ifndef PS3_PPU_EXECUTION_SERVICES_H
#define PS3_PPU_EXECUTION_SERVICES_H
#include "tls_initialize.h"
#include "../sync/lwmutex.h"
#include "../memory/user_memory_service.h"
#include "../time/clock.h"
#include "compiled_code.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Caller owns the port, its context and all dependencies until every guest
 * worker using it has joined. Providers return normally on every outcome;
 * cancellation/unsupported execution stops occur only after provider return.
 * A successful TLS result guarantees a nonempty page-aligned allocation fully
 * committed, writable, exclusively owned and disjoint from existing allocations
 * in THIS execution's guest address space; thread_pointer lies in that span.
 * Caller establishes the arena's exclusivity before invocation. Provider leaves
 * the returned span committed until its owner explicitly reclaims it after all
 * users join. Cancellation after success does not silently revoke ownership.
 * A raw-address ABI adapter can validate shape but cannot prove this mapping or
 * exclusivity guarantee; every production/test provider must obey it.
 * A blocking provider must support per-worker cancellation and must not hold
 * unrelated service locks while waiting for another guest worker. */
typedef struct Ps3PpuLwmutexAttributes {
    uint32_t protocol;
    uint32_t recursive;
    unsigned char name[8];
} Ps3PpuLwmutexAttributes;
/* The memory capability and mutex provider operate on the SAME instance.
 * Successful create retains registry/pin ownership until explicit cleanup after
 * all workers join, including cancellation after provider success. Attribute
 * values are copied by the ABI boundary; no guest/CPU pointers enter policy. */
typedef struct Ps3PpuExecutionServices {
    void *context;
    Ps3TlsReport (*initialize_tls)(void *context, const Ps3TlsTemplate *tls_template,
                                   Ps3TlsImage *output);
    const ps3_guest_memory *memory;
    ps3_lwmutex_result (*create_mutex)(void *, uint32_t, const Ps3PpuLwmutexAttributes *);
    ps3_lwmutex_result (*lock_mutex)(void *, uint32_t, ps3_lwmutex_thread, uint64_t, ps3_wait_cancel);
    ps3_lwmutex_result (*try_mutex)(void *, uint32_t, ps3_lwmutex_thread);
    ps3_lwmutex_result (*unlock_mutex)(void *, uint32_t, ps3_lwmutex_thread);
    ps3_lwmutex_result (*destroy_mutex)(void *, uint32_t);
    /* Same instance/guard/domain ownership as memory and mutex capabilities.
     * Full publication/rollback/retained-pin results must survive cancellation
     * until the composition owner joins users and resolves retained resources. */
    ps3_user_memory_service_result (*allocate_memory)(void *, uint64_t, uint64_t, uint64_t);
    ps3_user_memory_service_result (*free_memory)(void *, uint32_t);
    ps3_user_memory_service_result (*query_memory)(void *, uint64_t);
    const ps3_clock *clock;
    /* Borrowed immutable compiled-code capability, shared by this instance's
     * workers. Its table/index storage and port remain live through every join.
     * The execution owner supplies per-worker depth and stop policy separately. */
    const ps3_compiled_code_lookup *compiled_code;
} Ps3PpuExecutionServices;
#ifdef __cplusplus
}
#endif
#endif
