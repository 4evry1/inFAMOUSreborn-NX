#ifndef GCM_INIT_SERVICE_H
#define GCM_INIT_SERVICE_H
/* Platform-free, instance-owned native `_cellGcmInitBody` service.
 *
 * Given the game's exact traced call (ctx_out 0x009B56E8, cmd_size 0x10000,
 * io_size 0x08500000, io_ea 0x40700000) the service:
 *   1. builds its own GCM address space (local window 0xC0000000 + 0x0F900000, IO space
 *      0x20000000) whose init mapping is exactly {ea 0x40700000, io 0, size 0x08500000};
 *   2. sets the native flip-semaphore word the stream acquires (device+0x30 = 1);
 *   3. writes the generated init command image, the context descriptor D (begin at the first
 *      command word, end at io_ea + 0x7ffc, current at the put offset, callback OPD), the callback OPD,
 *      the three report words and the control put/get into guest memory through the guest
 *      port (big-endian guest order);
 *   4. consumes that image for real through rsx_fifo_walker with the gcm_rsx_frontend as the
 *      sink, translating IO offsets through the address space and reading guest memory;
 *   5. publishes the context EA at the game's out pointer and advances control get.
 *
 * `run` succeeds only when the walker reached PUT, every one of the 530 methods was
 * accepted, the front end did not fail and exactly one SET_REFERENCE published
 * 0xFFFFFFFF.
 *
 * States: READY -> INITIALIZED (success) or FAILED (failure after the first guest write).
 * A failure before any guest write leaves the service READY (nothing guest-visible
 * happened).
 *
 * Recorded but not guest-visible (documented inferences, not measurements): the local
 * memory reservation is `backed = 0` (no guest backing in this slice), the firmware's
 * `_gcm_intr_thread` is not created (`intr_thread_created = 0`), and the callback is a
 * sentinel code address the composition leaves unregistered.
 *
 * No globals, no allocation: everything lives in the caller-owned gcm_init_service.
 */
#include <stddef.h>
#include <stdint.h>

#include "gcm_address_space.h"
#include "gcm_init_image.h"
#include "gcm_rsx_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GCM_INIT_SERVICE_SLOT_COUNT 1u
#define GCM_INIT_DEVICE_WORDS 13u      /* bytes [0x00, 0x34); the flip semaphore is word 0x0C */
#define GCM_INIT_FLIP_SEMAPHORE_OFFSET 0x30u
#define GCM_INIT_CONTEXT_END_OFFSET 0x7ffcu   /* first 32 KiB segment, last word reserved */
#define GCM_INIT_REPORTS_MIN_SIZE 0x1000u
#define GCM_INIT_WALK_BUDGET 4096u

#define GCM_INIT_EXPECTED_CTX_OUT_EA 0x009B56E8u
#define GCM_INIT_EXPECTED_CMD_SIZE 0x00010000u
#define GCM_INIT_EXPECTED_IO_SIZE 0x08500000u
#define GCM_INIT_EXPECTED_IO_EA 0x40700000u
#define GCM_INIT_LOCAL_BASE 0xC0000000u
#define GCM_INIT_LOCAL_SIZE 0x0F900000u
#define GCM_INIT_IO_SPACE_SIZE 0x20000000u
#define GCM_INIT_MEMORY_FREQUENCY 650000000u  /* 0x26BE3680 */
#define GCM_INIT_CORE_FREQUENCY 500000000u    /* 0x1DCD6500 */

#define GCM_INIT_TILE_COUNT 15u
#define GCM_INIT_ZCULL_COUNT 8u
#define GCM_INIT_DISPLAY_BUFFER_COUNT 8u

#define GCM_INIT_EA_TABLE_BYTES 0x1800u   /* EA -> IO: 0xC00 BE u16 entries */
#define GCM_INIT_IO_TABLE_BYTES 0x400u    /* IO -> EA: 0x200 BE u16 entries */
#define GCM_INIT_EA_TABLE_PAGES (GCM_INIT_EA_TABLE_BYTES / 2u)
#define GCM_INIT_IO_TABLE_PAGES (GCM_INIT_IO_TABLE_BYTES / 2u)

/* Guest port results: 0 = OK, nonzero = failure. Bytes are already big-endian guest order. */
typedef struct gcm_guest_ports {
    void *user;
    int (*write)(void *user, uint32_t ea, const void *bytes, size_t length);
    int (*read)(void *user, uint32_t ea, void *bytes, size_t length);
} gcm_guest_ports;

/* Guest-memory layout, chosen by composition. All six regions must not overlap. */
typedef struct gcm_init_layout {
    uint32_t context_ea;      /* 16-aligned: the 16-byte context descriptor D */
    uint32_t callback_opd_ea; /* 8-aligned:  {code, toc} */
    uint32_t callback_code;   /* sentinel code address recorded in D and the OPD */
    uint32_t callback_toc;
    uint32_t control_ea;      /* 4-aligned: put, get, ref */
    uint32_t reports_ea;      /* 4-aligned: report buffer, reports_size bytes */
    uint32_t reports_size;    /* >= 0x1000 */
    uint32_t offset_table_ea; /* 4-aligned: EA -> IO table, GCM_INIT_EA_TABLE_BYTES */
    uint32_t io_table_ea;     /* 4-aligned: IO -> EA table, GCM_INIT_IO_TABLE_BYTES */
} gcm_init_layout;

typedef struct gcm_init_request {
    uint32_t ctx_out_ea, cmd_size, io_size, io_ea;
} gcm_init_request;

/* cellGcmSetTileInfo / cellGcmBindTile record. `location` 0 is the only accepted surface
 * (local memory); the raw firmware fields are stored as given. No GPU effect. */
typedef struct gcm_init_tile {
    uint32_t location, offset, size, pitch, comp, base, bank;
    uint8_t set, bound;
} gcm_init_tile;

/* cellGcmBindZcull record. The eleven raw firmware arguments are stored as given once the
 * acceptance rules pass; `set`/`bound` follow the bind and unbind calls. No GPU effect. */
typedef struct gcm_init_zcull {
    uint32_t offset, width, height, cull_start;
    uint32_t z_format, aa_format, zcull_dir, zcull_format;
    uint32_t s_func, s_ref, s_mask;
    uint8_t set, bound;
} gcm_init_zcull;

/* cellGcmSetDisplayBuffer record: the four raw firmware fields are stored once the
 * acceptance rules pass; `set` marks a recorded buffer. No GPU or display effect. */
typedef struct gcm_init_display_buffer {
    uint32_t offset, pitch, width, height;
    uint8_t set;
} gcm_init_display_buffer;

/* cellGcmGetConfiguration output: the traced firmware values. */
typedef struct gcm_init_config {
    uint32_t local_address, io_address, local_size, io_size;
    uint32_t memory_frequency, core_frequency;
} gcm_init_config;

typedef enum gcm_init_status {
    GCM_INIT_OK = 0,
    GCM_INIT_INVALID_ARGUMENT,
    GCM_INIT_UNSUPPORTED_REQUEST,
    GCM_INIT_ALREADY_INITIALIZED,
    GCM_INIT_ALREADY_FAILED,
    GCM_INIT_ADDRESS_SPACE_FAILED,
    GCM_INIT_MAPPING_MISMATCH,
    GCM_INIT_GUEST_WRITE_FAILED,
    GCM_INIT_WALK_FAILED,
    GCM_INIT_FRONTEND_FAILED,
    GCM_INIT_METHOD_COUNT_MISMATCH,
    GCM_INIT_REFERENCE_MISMATCH,
    GCM_INIT_NOT_INITIALIZED,
    GCM_INIT_UNSUPPORTED_FLIP_MODE,
    GCM_INIT_UNSUPPORTED_TILE,
    GCM_INIT_UNSUPPORTED_ZCULL,
    GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER
} gcm_init_status;

/* Which step of `run` failed (GCM_INIT_STEP_NONE on success). */
typedef enum gcm_init_step {
    GCM_INIT_STEP_NONE = 0,
    GCM_INIT_STEP_REQUEST,
    GCM_INIT_STEP_ADDRESS_SPACE,
    GCM_INIT_STEP_DEVICE,
    GCM_INIT_STEP_WRITE_IMAGE,
    GCM_INIT_STEP_WRITE_CONTEXT,
    GCM_INIT_STEP_WRITE_OPD,
    GCM_INIT_STEP_WRITE_REPORTS,
    GCM_INIT_STEP_WRITE_CONTROL,
    GCM_INIT_STEP_WALK,
    GCM_INIT_STEP_WRITE_CONTROL_GET,
    GCM_INIT_STEP_WRITE_CTX_OUT,
    GCM_INIT_STEP_COUNT
} gcm_init_step;

/* Observation record. `committed` is set as soon as a guest write was attempted (a port
 * failure may still have written part of it). */
typedef struct gcm_init_result {
    int32_t r3;                  /* 0 on success, GCM_INIT_* otherwise */
    gcm_init_status status;
    gcm_init_step failed_step;
    int committed;
    /* Front-end failure detail (zero/OK when the front end did not fail). */
    gcm_fe_status fe_status;
    uint32_t fe_subchannel, fe_method, fe_data, fe_io_offset;
    int fe_port_result;
    /* Walker failure detail. */
    uint32_t walker_status;
    uint32_t walker_effect;
    uint32_t walker_failure_offset;
    /* Progress / state observations. */
    uint32_t io_offset;          /* get after the walk */
    uint32_t methods_accepted;
    uint32_t shadow_writes;
    uint32_t noop_actions;
    uint32_t references_published;
    uint32_t acquires_satisfied;
    uint32_t final_put, final_get, final_ref;
} gcm_init_result;

typedef struct gcm_init_service {
    int state;                   /* READY / INITIALIZED / FAILED */
    gcm_init_layout layout;
    gcm_guest_ports ports;
    gcm_as_mapping slots[GCM_INIT_SERVICE_SLOT_COUNT];
    gcm_address_space as;
    gcm_rsx_frontend fe;
    uint32_t device[GCM_INIT_DEVICE_WORDS];
    uint8_t image_run[GCM_INIT_IMAGE_MAX_RUN_BYTES];
    uint32_t last_reference;
    int guest_attempted;         /* a guest write port call happened (may have been partial) */
    int tables_materialized;     /* both offset tables were written successfully */
    int flip_semaphore_override_valid;
    uint32_t flip_semaphore_override;
    uint32_t flip_mode;          /* 0 unset, 1 HSYNC, 2 VSYNC */
    gcm_init_tile tiles[GCM_INIT_TILE_COUNT];
    gcm_init_zcull zculls[GCM_INIT_ZCULL_COUNT];
    gcm_init_display_buffer displays[GCM_INIT_DISPLAY_BUFFER_COUNT];
} gcm_init_service;

/* Validates the layout and the ports completely before publishing anything. */
gcm_init_status gcm_init_service_create(gcm_init_service *svc, const gcm_init_layout *layout,
                                        const gcm_guest_ports *ports);

gcm_init_status gcm_init_service_run(gcm_init_service *svc, const gcm_init_request *req,
                                     gcm_init_result *result);
/* 1 once cellGcmInit has completed on this service. */
int gcm_init_service_is_initialized(const gcm_init_service *svc);

/* Test hook (not used by the composition): force the native flip-semaphore word to `value`
 * instead of 1, to exercise an unsatisfied SEMAPHORE_ACQUIRE. */
void gcm_init_service_force_flip_semaphore(gcm_init_service *svc, uint32_t value);

/* Post-init queries. All require the service to be INITIALIZED; they write no guest
 * memory except gcm_init_service_offset_tables on its first successful call. */
gcm_init_status gcm_init_service_configuration(const gcm_init_service *svc,
                                               gcm_init_config *out);
/* Firmware byte order: BE u32 localAddress, ioAddress, localSize, ioSize,
 * memoryFrequency, coreFrequency. */
void gcm_init_config_encode(const gcm_init_config *config, uint8_t out[24]);
/* cellGcmGetLabelAddress: reports_ea + ((index << 4) & 0xFF0), index modulo 256. */
gcm_init_status gcm_init_service_label_address(const gcm_init_service *svc, uint32_t index,
                                               uint32_t *ea_out);
gcm_init_status gcm_init_service_control_register(const gcm_init_service *svc,
                                                  uint32_t *ea_out);
/* cellGcmGetOffsetTable: materialise both guest-visible tables on the first successful
 * call (EA -> IO first, then IO -> EA); later calls only return the two EAs. */
gcm_init_status gcm_init_service_offset_tables(gcm_init_service *svc,
                                               uint32_t *ea_table_ea_out,
                                               uint32_t *io_table_ea_out);
/* cellGcmSetFlipMode: records 1 (HSYNC) or 2 (VSYNC); any other value is refused. */
gcm_init_status gcm_init_service_set_flip_mode(gcm_init_service *svc, uint32_t mode);
/* cellGcmSetTileInfo: local-memory tiles only (location 0), 64 KiB-aligned offset and
 * nonzero 64 KiB-aligned size inside the local window, 256-aligned nonzero pitch, bank < 4.
 * Refusals return GCM_INIT_UNSUPPORTED_TILE and change nothing. */
gcm_init_status gcm_init_service_set_tile_info(gcm_init_service *svc, uint32_t index,
                                               uint32_t location, uint32_t offset,
                                               uint32_t size, uint32_t pitch,
                                               uint32_t comp, uint32_t base, uint32_t bank);
/* cellGcmBindTile: requires a set tile. */
gcm_init_status gcm_init_service_bind_tile(gcm_init_service *svc, uint32_t index);
/* Read-only view; NULL when the service or the index is out of range. */
const gcm_init_tile *gcm_init_service_tile(const gcm_init_service *svc, uint32_t index);

/* cellGcmBindZcull: index < 8, 64 KiB-aligned offset inside the local window, nonzero 64x
 * width and height up to 4096, 4096-aligned cull start, z_format 1 or 2. The other arguments
 * are recorded as given. Refusals return GCM_INIT_UNSUPPORTED_ZCULL and change nothing. */
gcm_init_status gcm_init_service_bind_zcull(gcm_init_service *svc, uint32_t index,
                                            const gcm_init_zcull *args);
/* cellGcmUnbindZcull: index < 8; clears set and bound. */
gcm_init_status gcm_init_service_unbind_zcull(gcm_init_service *svc, uint32_t index);
/* cellGcmUnbindTile: index < 15; clears bound and keeps the stored tile record. */
gcm_init_status gcm_init_service_unbind_tile(gcm_init_service *svc, uint32_t index);
/* Read-only view; NULL when the service or the index is out of range. */
const gcm_init_zcull *gcm_init_service_zcull(const gcm_init_service *svc, uint32_t index);

/* cellGcmSetDisplayBuffer: id < 8, width and height 1..4096, nonzero 64-aligned pitch,
 * 256-aligned offset, and offset + pitch * height inside the local window. Refusals return
 * GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER and change nothing. */
gcm_init_status gcm_init_service_set_display_buffer(gcm_init_service *svc, uint32_t id,
                                                    uint32_t offset, uint32_t pitch,
                                                    uint32_t width, uint32_t height);
/* Read-only view; NULL when the service or the id is out of range. */
const gcm_init_display_buffer *gcm_init_service_display_buffer(const gcm_init_service *svc,
                                                              uint32_t id);

const char *gcm_init_status_name(gcm_init_status status);
const char *gcm_init_step_name(gcm_init_step step);

#ifdef __cplusplus
}
#endif
#endif
