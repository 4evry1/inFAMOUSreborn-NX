#ifndef NATIVE_LWMUTEX_FAST_HOOK_H
#define NATIVE_LWMUTEX_FAST_HOOK_H
/* ppu-speed-20261113 item 1: force-included (-include) into the existing sync tests (test_lwmutex.c, test_lwcond.c) by
 * evidence/ppu-speed-20261113/tests/run_lwmutex_tests.sh with -DLWMUTEX_TEST_FAST=1, so the very same scenarios also run with
 * the lock-free fast path enabled. The fixtures' ps3_lwmutex_init_with_conditions call is wrapped: after the real init the
 * registry gets a fast port into the fixture's host memory (domain->backing.context is the fixture's host_memory, whose first
 * member is the byte array). Nothing else in the tests changes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sync/lwmutex.h"
#ifdef LWMUTEX_TEST_FAST
enum { LWMUTEX_HOOK_MEMORY_BYTES = 65536 };
struct lwmutex_hook_memory { unsigned char *bytes; };
static inline unsigned char *lwmutex_hook_pointer(void *context, uint32_t address, uint32_t bytes)
{
    struct lwmutex_hook_memory *memory = context;
    if (address > LWMUTEX_HOOK_MEMORY_BYTES || bytes > LWMUTEX_HOOK_MEMORY_BYTES - address) return 0;
    return memory->bytes + address;
}
static inline ps3_lwmutex_result lwmutex_hook_init(ps3_lwmutex_registry *registry, ps3_atomic_domain *domain,
    ps3_instance_guard guard, ps3_wait_port wait, ps3_lwmutex_object *objects, size_t object_capacity,
    ps3_lwmutex_waiter *waiters, size_t waiter_capacity, size_t condition_binding_capacity)
{
    ps3_lwmutex_result outcome = ps3_lwmutex_init_with_conditions(registry, domain, guard, wait, objects, object_capacity,
        waiters, waiter_capacity, condition_binding_capacity);
    if (outcome.status != PS3_LWMUTEX_OK) return outcome;
    return ps3_lwmutex_enable_fast_path(registry, (ps3_lwmutex_fast_port){domain->backing.context, lwmutex_hook_pointer});
}
#define ps3_lwmutex_init_with_conditions lwmutex_hook_init
#endif
#endif
