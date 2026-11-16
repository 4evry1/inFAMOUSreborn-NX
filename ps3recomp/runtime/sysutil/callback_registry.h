#ifndef PS3_SYSUTIL_CALLBACK_REGISTRY_H
#define PS3_SYSUTIL_CALLBACK_REGISTRY_H

#include <stdint.h>
#include "../sync/guard.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_SYSUTIL_CALLBACK_SLOT_COUNT = 4 };

typedef enum ps3_sysutil_callback_status {
    PS3_SYSUTIL_CALLBACK_OK = 0,
    PS3_SYSUTIL_CALLBACK_INVALID_ARGUMENT,
    PS3_SYSUTIL_CALLBACK_NOT_INITIALIZED,
    PS3_SYSUTIL_CALLBACK_INVALID_SLOT
} ps3_sysutil_callback_status;

typedef struct ps3_sysutil_callback_pair {
    uint32_t callback_descriptor_address;
    uint32_t userdata_address;
} ps3_sysutil_callback_pair;

typedef struct ps3_sysutil_callback_registry {
    ps3_instance_guard guard;
    ps3_sysutil_callback_pair pairs[PS3_SYSUTIL_CALLBACK_SLOT_COUNT];
    unsigned initialized;
} ps3_sysutil_callback_registry;

typedef struct ps3_sysutil_callback_result {
    ps3_sysutil_callback_status status;
    uint32_t slot;
    ps3_sysutil_callback_pair previous;
    ps3_sysutil_callback_pair current;
} ps3_sysutil_callback_result;

/* Fresh zeroed caller-owned registry. Init/disposal require quiescence. Retain
 * registry and guard context through confirmed worker join; no live copy/move,
 * reinit, direct mutation or guarded reentry. Addresses are opaque guest values,
 * including zero/odd/unmapped values: registration never resolves or invokes. */
ps3_sysutil_callback_status ps3_sysutil_callback_init(
    ps3_sysutil_callback_registry *registry, ps3_instance_guard guard);

/* Atomically replaces both fields, retaining userdata even for a null callback.
 * Status is an inward result; guest CELL encoding belongs to the adapter. */
ps3_sysutil_callback_result ps3_sysutil_callback_register(
    ps3_sysutil_callback_registry *registry, uint32_t slot,
    ps3_sysutil_callback_pair pair);

/* Snapshot must not alias registry. Every failure leaves snapshot unchanged. */
ps3_sysutil_callback_status ps3_sysutil_callback_read(
    ps3_sysutil_callback_registry *registry, uint32_t slot,
    ps3_sysutil_callback_pair *snapshot);

#ifdef __cplusplus
}
#endif
#endif
