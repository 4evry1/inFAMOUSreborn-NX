#ifndef PS3_DIAGNOSTIC_MODULE_ADMISSION_H
#define PS3_DIAGNOSTIC_MODULE_ADMISSION_H

#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/sync/guard.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental assumption accounting only; never provider availability or CELL status. */
typedef enum ps3_diagnostic_module_status {
    PS3_DIAGNOSTIC_MODULE_OK = 0,
    PS3_DIAGNOSTIC_MODULE_INVALID_ARGUMENT,
    PS3_DIAGNOSTIC_MODULE_NOT_INITIALIZED,
    PS3_DIAGNOSTIC_MODULE_UNKNOWN_MODULE,
    PS3_DIAGNOSTIC_MODULE_COUNT_EXHAUSTED
} ps3_diagnostic_module_status;

typedef struct ps3_diagnostic_module_slot {
    uint16_t module_id;
    uint32_t admission_count;
} ps3_diagnostic_module_slot;

typedef struct ps3_diagnostic_module_state {
    ps3_instance_guard guard;
    ps3_diagnostic_module_slot *slots;
    size_t slot_count;
    uint64_t total_admissions;
    unsigned initialized;
} ps3_diagnostic_module_state;

typedef struct ps3_diagnostic_module_result {
    ps3_diagnostic_module_status status;
    uint16_t module_id;
    uint32_t previous_count;
    uint32_t current_count;
    uint64_t total_admissions;
} ps3_diagnostic_module_result;

/* Fresh zeroed caller-owned state; init and disposal require quiescence. Catalog,
 * state and slot storage must be disjoint. Successful init copies catalog IDs.
 * Retain state, slots and guard context until every worker has joined. No live
 * copy/move/reinit, direct mutation, reentry or callbacks while guarded. */
ps3_diagnostic_module_status ps3_diagnostic_module_init(
    ps3_diagnostic_module_state *state, ps3_instance_guard guard,
    const uint16_t *catalog, size_t catalog_count,
    ps3_diagnostic_module_slot *slots, size_t slot_capacity);

ps3_diagnostic_module_result ps3_diagnostic_module_admit(
    ps3_diagnostic_module_state *state, uint16_t module_id);

/* Output must not alias state/slots. Failure leaves output unchanged; a successful
 * read sets previous_count == current_count and never changes admission counts. */
ps3_diagnostic_module_status ps3_diagnostic_module_read(
    ps3_diagnostic_module_state *state, uint16_t module_id,
    ps3_diagnostic_module_result *snapshot);

#ifdef __cplusplus
}
#endif
#endif
