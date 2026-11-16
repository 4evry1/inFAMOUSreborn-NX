#ifndef GCM_RSX_FRONTEND_H
#define GCM_RSX_FRONTEND_H
/* Platform-free, instance-owned RSX front end for the native GCM-init service.
 *
 * The front end is the `rsx_fifo_sink` the walker calls for every decoded method word. It
 * implements the state-recording policy: SET_OBJECT (method 0) binds a subchannel to an RSX
 * object class; channel methods (0 < method < 0x100) are acted on (SET_REFERENCE,
 * DMA semaphore context/offset/acquire); class methods (method >= 0x100) are admitted only
 * when the generated state allowlist names (class, method), and their data is recorded in
 * that class's shadow register file. Three class methods are explicit actions instead of
 * allowlist entries (NV4097 0x0100 NO_OPERATION, NV4097 0x1D94 CLEAR_SURFACE,
 * NV3089 0x040C IMAGE_IN).
 *
 * No globals, no allocation: the whole state, including the shadow register files, lives in
 * the caller-owned gcm_rsx_frontend.
 *
 * Effects handed back to the walker: a refusal that changes nothing reports
 * RSX_FIFO_EFFECT_NONE. A port that failed after a possible write (port result 2) reports
 * RSX_FIFO_EFFECT_UNKNOWN. The first failure is latched; later words are refused with
 * GCM_FE_LATCHED and the first record is preserved.
 */
#include <stdint.h>

#include "rsx_fifo_walker.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_FE_SUBCHANNEL_COUNT 8u
#define GCM_FE_METHOD_SLOTS 2048u   /* method 0x1FFC >> 2 == 0x7FF */

typedef enum gcm_fe_class {
    GCM_FE_CLASS_NONE = 0,
    GCM_FE_CLASS_NV4097,
    GCM_FE_CLASS_NV0039,
    GCM_FE_CLASS_NV3062,
    GCM_FE_CLASS_NV309E,
    GCM_FE_CLASS_NV308A,
    GCM_FE_CLASS_NV3089,
    GCM_FE_CLASS_UNKNOWN_OBJECT,
    GCM_FE_CLASS_COUNT
} gcm_fe_class;

/* One state-allowlist entry. cls is a gcm_fe_class value; entries are sorted by
 * (cls, method) and unique. The front end only reads this array. */
typedef struct gcm_fe_allow {
    uint8_t cls;
    uint16_t method;
} gcm_fe_allow;

typedef enum gcm_fe_status {
    GCM_FE_OK = 0,
    GCM_FE_INVALID_ARGUMENT,
    GCM_FE_UNBOUND_SUBCHANNEL,
    GCM_FE_UNKNOWN_OBJECT_METHOD,
    GCM_FE_UNSUPPORTED_CHANNEL_METHOD,
    GCM_FE_NOT_ALLOWLISTED,
    GCM_FE_ACTION_UNSUPPORTED,
    GCM_FE_SEMAPHORE_STATE_MISSING,
    GCM_FE_SEMAPHORE_CONTEXT_UNKNOWN,
    GCM_FE_ACQUIRE_UNSATISFIED,
    GCM_FE_PORT_REFUSED,
    GCM_FE_PORT_FAILED,
    GCM_FE_LATCHED
} gcm_fe_status;

/* Port results: 0 = OK, 1 = refused with nothing written, 2 = failed after a possible write. */
typedef struct gcm_fe_ports {
    void *user;
    int (*semaphore_read)(void *user, uint32_t context_handle, uint32_t offset, uint32_t *value_out);
    int (*publish_reference)(void *user, uint32_t value);
} gcm_fe_ports;

typedef struct gcm_fe_failure {
    gcm_fe_status status;
    uint32_t subchannel, method, data, io_offset;
    int port_result;   /* 0 when no port was involved */
} gcm_fe_failure;

typedef struct gcm_fe_subchannel {
    uint32_t handle;
    gcm_fe_class cls;
} gcm_fe_subchannel;

typedef struct gcm_rsx_frontend {
    gcm_fe_ports ports;
    const gcm_fe_allow *allow;
    uint32_t allow_count;
    gcm_fe_subchannel sub[GCM_FE_SUBCHANNEL_COUNT];
    /* NV406E semaphore state is channel state, not per subchannel: SET_OBJECT only rebinds the
     * subchannel and must leave this state intact. */
    int semaphore_context_valid, semaphore_offset_valid;
    uint32_t semaphore_context, semaphore_offset;
    uint32_t value[GCM_FE_CLASS_COUNT][GCM_FE_METHOD_SLOTS];
    uint8_t written[GCM_FE_CLASS_COUNT][GCM_FE_METHOD_SLOTS];
    gcm_fe_failure failure;
    int failed;
    uint32_t accepted_methods;
    uint32_t shadow_writes;
    uint32_t noop_actions;
    uint32_t references_published;
    uint32_t acquires_satisfied;
    uint32_t last_reference;
} gcm_rsx_frontend;

/* Validates the ports (both callbacks non-NULL) and that allow is sorted by (cls, method)
 * with no duplicates. Nothing is written on failure. */
gcm_fe_status gcm_rsx_frontend_init(gcm_rsx_frontend *fe, const gcm_fe_ports *ports,
                                    const gcm_fe_allow *allow, uint32_t allow_count);

/* rsx_fifo_sink.method callback: 0 = accepted, nonzero = refusal (walker reports
 * RSX_FIFO_SINK_FAILED with the sink's *effect). */
int gcm_rsx_frontend_sink(void *user, uint32_t subchannel, uint32_t method, uint32_t data,
                          uint32_t io_offset, rsx_fifo_effect *effect);

/* First latched failure, or NULL when no word has been refused. */
const gcm_fe_failure *gcm_rsx_frontend_failure(const gcm_rsx_frontend *fe);
int gcm_rsx_frontend_failed(const gcm_rsx_frontend *fe);
/* 1 and *value_out set when (cls, method) was recorded, 0 otherwise. */
int gcm_rsx_frontend_shadow_get(const gcm_rsx_frontend *fe, gcm_fe_class cls, uint32_t method,
                                uint32_t *value_out);
/* Handle a subchannel is bound to (0 when unbound). */
uint32_t gcm_rsx_frontend_bound_handle(const gcm_rsx_frontend *fe, uint32_t subchannel);
/* Stable name for evidence logs. */
const char *gcm_fe_status_name(gcm_fe_status status);

#ifdef __cplusplus
}
#endif
#endif
