#ifndef NATIVE_DISC_CHANGE_CALLBACKS_H
#define NATIVE_DISC_CHANGE_CALLBACKS_H
#include "../../../ps3recomp/runtime/sync/guard.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PS3_DISC_CHANGE_UNCONFIGURED=0,
    PS3_DISC_CHANGE_IMMUTABLE_SELECTED_PACKAGE
} ps3_disc_change_policy;
typedef enum {
    PS3_DISC_CHANGE_OK=0,
    PS3_DISC_CHANGE_INVALID_ARGUMENT,
    PS3_DISC_CHANGE_NOT_INITIALIZED,
    PS3_DISC_CHANGE_UNSUPPORTED_POLICY,
    PS3_DISC_CHANGE_ALREADY_INITIALIZED
} ps3_disc_change_status;
typedef struct {
    uint32_t eject_descriptor,insert_descriptor;
} ps3_disc_change_pair;
typedef struct ps3_disc_change_registry {
    ps3_instance_guard guard;
    ps3_disc_change_policy policy;
    ps3_disc_change_pair current;
    unsigned initialized;
} ps3_disc_change_registry;
typedef struct {
    ps3_disc_change_status status;
    unsigned committed;
    ps3_disc_change_pair previous,current;
} ps3_disc_change_result;

/* Process-owned numeric registration only; policy is independent of boot
 * metadata, mounts and physical media. The admitted environment produces no
 * media-change events. No API resolves, reads, pins or invokes either OPD.
 * Guard callbacks are total, nonthrowing, noncancelling void operations: this
 * port cannot report a recoverable lock/unlock failure. No callback runs while
 * locked other than that guard itself. The native guard provides consistent
 * pair snapshots, not a claim of pair-atomic publication in the Sony wrapper.
 *
 * Init requires zero-filled storage and exclusive ownership; a second init
 * fails without changing anything. Guard/policy/initialized stay immutable
 * while live. Never copy/move/reinitialize or directly mutate a live registry.
 * Retain registry and guard through all borrowing PPU joins; discard only when
 * quiescent, without invoking callbacks. */
ps3_disc_change_status ps3_disc_change_init(ps3_disc_change_registry *,ps3_instance_guard,ps3_disc_change_policy);
/* Replace both opaque low32 values, including zero, insert then eject under the
 * native guard. Identical registration still commits. Result is captured under
 * the guard and returned after unlock; later HLE failure cannot undo the pair. */
ps3_disc_change_result ps3_disc_change_register(ps3_disc_change_registry *,ps3_disc_change_pair);
/* Snapshot must not alias registry storage. Failure leaves output unchanged. */
ps3_disc_change_status ps3_disc_change_read(ps3_disc_change_registry *,ps3_disc_change_pair *);

#ifdef __cplusplus
}
#endif
#endif
