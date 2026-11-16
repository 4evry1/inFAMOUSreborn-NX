#ifndef PS3_RUNTIME_EXITSPAWN_CALLBACKS_H
#define PS3_RUNTIME_EXITSPAWN_CALLBACKS_H
#include "../sync/guard.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum ps3_exitspawn_kind {
    PS3_EXITSPAWN_ATEXITSPAWN=0,
    PS3_EXITSPAWN_AT_EXITSPAWN=1
} ps3_exitspawn_kind;
typedef enum ps3_exitspawn_status {
    PS3_EXITSPAWN_OK=0,
    PS3_EXITSPAWN_INVALID_ARGUMENT,
    PS3_EXITSPAWN_NOT_INITIALIZED
} ps3_exitspawn_status;
typedef enum ps3_exitspawn_decision {
    PS3_EXITSPAWN_NULL=0,
    PS3_EXITSPAWN_INSERTED,
    PS3_EXITSPAWN_RETAINED
} ps3_exitspawn_decision;
typedef struct ps3_exitspawn_snapshot {
    uint32_t atexitspawn, at_Exitspawn;
} ps3_exitspawn_snapshot;
typedef struct ps3_exitspawn_result {
    ps3_exitspawn_status status;
    ps3_exitspawn_decision decision;
    ps3_exitspawn_snapshot snapshot;
} ps3_exitspawn_result;
typedef struct ps3_exitspawn_registry {
    ps3_instance_guard guard;
    ps3_exitspawn_snapshot slots;
    unsigned initialized;
} ps3_exitspawn_registry;
/* Fresh zero registry; initialize once before admission. Ports return normally
 * and never reenter this registry. Live records/guard cannot move, copy or be
 * reinitialized. Retain through all borrowers joining; quiescent teardown may
 * discard the record without invoking callbacks. Output must not alias live
 * registry storage. No API reads/pins/resolves/invokes the opaque guest value.
 * First nonzero wins independently per kind. Occupied slot => RETAINED even
 * for a later zero; initial zero => NULL. Internal results are not CELL codes. */
ps3_exitspawn_status ps3_exitspawn_init(ps3_exitspawn_registry *registry,
    ps3_instance_guard guard);
ps3_exitspawn_result ps3_exitspawn_register(ps3_exitspawn_registry *registry,
    ps3_exitspawn_kind kind, uint32_t descriptor_address);
/* Guarded consistent snapshot; failure leaves output unchanged. */
ps3_exitspawn_status ps3_exitspawn_read(ps3_exitspawn_registry *registry,
    ps3_exitspawn_snapshot *output);
#ifdef __cplusplus
}
#endif
#endif
