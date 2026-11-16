#ifndef PS3_NATIVE_SPURS_READY_COUNT_H
#define PS3_NATIVE_SPURS_READY_COUNT_H
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PS3_SPURS_READY_COUNT_RUNNING=0,
    PS3_SPURS_READY_COUNT_INVALID_ARGUMENT,
    PS3_SPURS_READY_COUNT_UNOWNED_INSTANCE,
    PS3_SPURS_READY_COUNT_MEMORY,
    PS3_SPURS_READY_COUNT_CANCELLED,
    PS3_SPURS_READY_COUNT_OBSERVER,
    PS3_SPURS_READY_COUNT_NO_EXECUTION_SERVICES
} ps3_spurs_ready_count_stop;
typedef enum { PS3_SPURS_READY_COUNT_READ, PS3_SPURS_READY_COUNT_WRITE } ps3_spurs_ready_count_operation;
typedef enum {
    PS3_SPURS_READY_COUNT_FLAGS=1, PS3_SPURS_READY_COUNT_ENABLED,
    PS3_SPURS_READY_COUNT_EXCEPTION, PS3_SPURS_READY_COUNT_STATE, PS3_SPURS_READY_COUNT_STORE
} ps3_spurs_ready_count_stage;
typedef struct { uint32_t spurs,wid,value; } ps3_spurs_ready_count_request;
typedef struct {
    ps3_spurs_ready_count_operation operation;
    ps3_spurs_ready_count_stage stage;
    uint32_t address;
    size_t length;
    const unsigned char *bytes;
} ps3_spurs_ready_count_event;
typedef struct {
    void *context;
    int (*permits)(void *,uint32_t,size_t,unsigned);
} ps3_spurs_ready_count_memory_scope;
typedef struct {
    ps3_atomic_domain *domain;
    ps3_atomic_owner owner;
    uint32_t instance,owned_instance_bytes;
    unsigned active_owned_instance;
    void *poll_context;
    int (*may_continue)(void *);
    void *observer_context;
    int (*observe)(void *,const ps3_spurs_ready_count_event *);
    ps3_spurs_ready_count_memory_scope memory_scope;
} ps3_spurs_ready_count_config;
typedef struct {
    ps3_spurs_ready_count_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result,ready_address;
    size_t reads,writes,written_bytes,observer_calls,publications,release_fences;
    unsigned store_attempted,stored;
} ps3_spurs_ready_count_result;

/* cellSpursCreateTask's signal path (0x10C6C -> 0xA750 -> 0x85D0).
 *
 * 0xA750 ORs ONE bit into a 32-bit ready-count mask word: `spurs+0x70` for wid <= 15, `spurs+0x78`
 * for wid > 15. That is a different effect from
 * ps3_spurs_ready_count_apply, which publishes one byte at `spurs+wid`; the existing API cannot
 * express it, so this is the additive extension. Existing behaviour is untouched.
 *
 * 0x85D0 then stores 1 at `spurs+0xD64` when `spurs+0xD6C` (the exception word) reads zero.
 *
 * The gates are the firmware's, in its order, first failure wins:
 *   0xA758 spurs == 0            -> 0x80410811
 *   0xA76C spurs & 0x7F          -> 0x80410810
 *   0xA7A4 wid >= 0x20 / 0x40    -> 0x80410802
 *   0xA7B0 wklEnabled bit clear  -> 0x80410805
 *   0xA7D0 exception word != 0   -> 0x8041080F
 * A mask write that the firmware would refuse writes nothing. */
typedef struct {
    uint32_t spurs, wid, mask_bit;
} ps3_spurs_ready_mask_request;

typedef struct {
    ps3_spurs_ready_count_stop stop;
    ps3_guest_memory_status memory_status;
    unsigned has_sdk_result;
    uint32_t sdk_result, mask_address;
    size_t reads, writes, written_bytes, observer_calls, publications;
    unsigned mask_written, signal_written;
} ps3_spurs_ready_mask_result;

/* Same pure NULL/alignment validation the byte form uses (0xA758/0xA76C). */
int ps3_spurs_ready_mask_validate_address(const ps3_spurs_ready_mask_request *,uint32_t *sdk_result);
/* Applies 0xA750 then 0x85D0 for one wid. Config/storage rules are those of
 * ps3_spurs_ready_count_apply: owner borrowed and registered, callbacks non-throwing, a false
 * memory scope denies the access before any effect, and every confirmed effect is published to
 * the observer outside the coherent guard. */
void ps3_spurs_ready_mask_apply(const ps3_spurs_ready_count_config *,
    const ps3_spurs_ready_mask_request *,ps3_spurs_ready_mask_result *);

/* Pure SDK NULL/alignment validation, before ownership or memory access. */
int ps3_spurs_ready_count_validate_address(const ps3_spurs_ready_count_request *,uint32_t *sdk_result);
/* Captures config/request by value. The owner is borrowed and remains registered.
 * Callbacks must not throw or contain host cancellation points. False scope
 * denies that access before effects. Poll/scope run before each access;
 * observers receive only confirmed effects,
 * outside the coherent-memory guard. Event bytes live only during the callback.
 * A native stop after publication retains the store without claiming SDK return.
 * Publication uses the existing coherent reservation-loss path; no extra wake. */
void ps3_spurs_ready_count_apply(const ps3_spurs_ready_count_config *,
    const ps3_spurs_ready_count_request *,ps3_spurs_ready_count_result *);
#ifdef __cplusplus
}
#endif
#endif
