/* Native GCM-init service. Contract: gcm_init_service.h. */
#include "gcm_init_service.h"

#include <string.h>

#define SERVICE_STATE_READY 0
#define SERVICE_STATE_INITIALIZED 1
#define SERVICE_STATE_FAILED 2

static void put_be32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get_be32(const uint8_t in[4])
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static void put_be16(uint8_t out[2], uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static int ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size)
{
    uint64_t a0 = a, a1 = (uint64_t)a + a_size;
    uint64_t b0 = b, b1 = (uint64_t)b + b_size;
    return a0 < b1 && b0 < a1;
}

gcm_init_status gcm_init_service_create(gcm_init_service *svc, const gcm_init_layout *layout,
                                        const gcm_guest_ports *ports)
{
    if (!svc || !layout || !ports) return GCM_INIT_INVALID_ARGUMENT;
    if (!ports->write || !ports->read) return GCM_INIT_INVALID_ARGUMENT;

    const uint32_t ea[6] = {layout->context_ea, layout->callback_opd_ea, layout->control_ea,
                            layout->reports_ea, layout->offset_table_ea,
                            layout->io_table_ea};
    const uint32_t size[6] = {16u, 8u, 12u, layout->reports_size,
                              GCM_INIT_EA_TABLE_BYTES, GCM_INIT_IO_TABLE_BYTES};
    for (int i = 0; i < 6; i++) {
        if (ea[i] == 0) return GCM_INIT_INVALID_ARGUMENT;
        if ((ea[i] & 3u) != 0) return GCM_INIT_INVALID_ARGUMENT;
        if ((uint64_t)ea[i] + size[i] > 0x100000000ull) return GCM_INIT_INVALID_ARGUMENT;
    }
    if ((layout->context_ea & 15u) != 0) return GCM_INIT_INVALID_ARGUMENT;
    if ((layout->callback_opd_ea & 7u) != 0) return GCM_INIT_INVALID_ARGUMENT;
    if (layout->reports_size < GCM_INIT_REPORTS_MIN_SIZE) return GCM_INIT_INVALID_ARGUMENT;
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (ranges_overlap(ea[i], size[i], ea[j], size[j]))
                return GCM_INIT_INVALID_ARGUMENT;

    memset(svc, 0, sizeof *svc);
    svc->layout = *layout;
    svc->ports = *ports;
    svc->state = SERVICE_STATE_READY;
    return GCM_INIT_OK;
}

void gcm_init_service_force_flip_semaphore(gcm_init_service *svc, uint32_t value)
{
    if (!svc) return;
    svc->flip_semaphore_override = value;
    svc->flip_semaphore_override_valid = 1;
}

static int guest_write(gcm_init_service *svc, uint32_t ea, const void *bytes, size_t length)
{
    svc->guest_attempted = 1;
    return svc->ports.write(svc->ports.user, ea, bytes, length) != 0;
}

static rsx_fifo_read_result service_read_word(void *user, uint32_t io_offset, uint8_t out_be[4])
{
    gcm_init_service *svc = user;
    uint32_t ea = 0;
    if (gcm_as_io_to_address(&svc->as, io_offset, 4, &ea) != GCM_AS_OK)
        return RSX_FIFO_IO_UNMAPPED;
    if (svc->ports.read(svc->ports.user, ea, out_be, 4) != 0) return RSX_FIFO_IO_FAULT;
    return RSX_FIFO_IO_OK;
}

static int service_semaphore_read(void *user, uint32_t context_handle, uint32_t offset,
                                  uint32_t *value_out)
{
    gcm_init_service *svc = user;
    if (context_handle == 0x56616661u) {   /* native device words */
        if (offset == GCM_INIT_FLIP_SEMAPHORE_OFFSET) {
            *value_out = svc->device[GCM_INIT_FLIP_SEMAPHORE_OFFSET / 4];
            return 0;
        }
        return 1;                          /* refused, nothing written */
    }
    if (context_handle == 0x66616661u) {   /* guest report buffer */
        if (offset > svc->layout.reports_size ||
            svc->layout.reports_size - offset < 4u)
            return 1;
        uint8_t bytes[4];
        if (svc->ports.read(svc->ports.user, svc->layout.reports_ea + offset, bytes, 4) != 0)
            return 2;                      /* failed after a possible write */
        *value_out = get_be32(bytes);
        return 0;
    }
    return 1;
}

static int service_publish_reference(void *user, uint32_t value)
{
    gcm_init_service *svc = user;
    uint8_t bytes[4];
    put_be32(bytes, value);
    if (guest_write(svc, svc->layout.control_ea + 8u, bytes, 4)) return 2;
    svc->last_reference = value;
    return 0;
}

static gcm_init_status fail(gcm_init_service *svc, gcm_init_result *result,
                            gcm_init_status status, gcm_init_step step)
{
    result->status = status;
    result->failed_step = step;
    result->r3 = (int32_t)status;
    result->committed = svc->guest_attempted;
    if (svc->guest_attempted) svc->state = SERVICE_STATE_FAILED;
    return status;
}

static void copy_front_end_failure(gcm_init_service *svc, gcm_init_result *result)
{
    const gcm_fe_failure *f = gcm_rsx_frontend_failure(&svc->fe);
    if (!f) {
        result->fe_status = GCM_FE_OK;
        return;
    }
    result->fe_status = f->status;
    result->fe_subchannel = f->subchannel;
    result->fe_method = f->method;
    result->fe_data = f->data;
    result->fe_io_offset = f->io_offset;
    result->fe_port_result = f->port_result;
}

static void copy_front_end_counters(gcm_init_service *svc, gcm_init_result *result)
{
    result->methods_accepted = svc->fe.accepted_methods;
    result->shadow_writes = svc->fe.shadow_writes;
    result->noop_actions = svc->fe.noop_actions;
    result->references_published = svc->fe.references_published;
    result->acquires_satisfied = svc->fe.acquires_satisfied;
    result->final_ref = svc->last_reference;
}

gcm_init_status gcm_init_service_run(gcm_init_service *svc, const gcm_init_request *req,
                                     gcm_init_result *result)
{
    if (!svc || !req || !result) return GCM_INIT_INVALID_ARGUMENT;
    memset(result, 0, sizeof *result);
    result->failed_step = GCM_INIT_STEP_NONE;
    result->fe_status = GCM_FE_OK;

    if (svc->state == SERVICE_STATE_INITIALIZED) {
        result->status = GCM_INIT_ALREADY_INITIALIZED;
        return GCM_INIT_ALREADY_INITIALIZED;
    }
    if (svc->state == SERVICE_STATE_FAILED) {
        result->status = GCM_INIT_ALREADY_FAILED;
        return GCM_INIT_ALREADY_FAILED;
    }

    /* Step 0: only the game's traced call is supported, and a refused request writes nothing. */
    if (req->ctx_out_ea != GCM_INIT_EXPECTED_CTX_OUT_EA ||
        req->cmd_size != GCM_INIT_EXPECTED_CMD_SIZE ||
        req->io_size != GCM_INIT_EXPECTED_IO_SIZE ||
        req->io_ea != GCM_INIT_EXPECTED_IO_EA)
        return fail(svc, result, GCM_INIT_UNSUPPORTED_REQUEST, GCM_INIT_STEP_REQUEST);

    /* Step 1: the address space. gcm_as_init publishes the init mapping itself
     * ({ea io_ea, io 0, size io_size}) in slot 0, so no map_main_memory call is needed. */
    {
        gcm_as_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.local_base = GCM_INIT_LOCAL_BASE;
        cfg.local_size = GCM_INIT_LOCAL_SIZE;
        cfg.io_ea = req->io_ea;
        cfg.io_size = req->io_size;
        cfg.io_space_size = GCM_INIT_IO_SPACE_SIZE;
        cfg.slots = svc->slots;
        cfg.slot_count = GCM_INIT_SERVICE_SLOT_COUNT;
        if (gcm_as_init(&svc->as, &cfg) != GCM_AS_OK)
            return fail(svc, result, GCM_INIT_ADDRESS_SPACE_FAILED,
                        GCM_INIT_STEP_ADDRESS_SPACE);
        gcm_as_mapping info;
        if (gcm_as_active_count(&svc->as) != 1 ||
            gcm_as_get_mapping(&svc->as, 0, &info) != GCM_AS_OK || !info.active ||
            !info.initial || info.ea != req->io_ea || info.io != 0 ||
            info.size != req->io_size)
            return fail(svc, result, GCM_INIT_MAPPING_MISMATCH,
                        GCM_INIT_STEP_ADDRESS_SPACE);
    }

    /* Step 2: native device state. The stream acquires value 1 on device+0x30 before any
     * flip, and real init returns, so the device must already hold 1 (inference). */
    svc->device[GCM_INIT_FLIP_SEMAPHORE_OFFSET / 4] =
        svc->flip_semaphore_override_valid ? svc->flip_semaphore_override : 1u;

    /* Step 3a: the generated init command image, one guest write per contiguous run. */
    {
        uint32_t i = 0;
        while (i < gcm_init_image_word_count) {
            const uint32_t start = gcm_init_image_words[i].io_offset;
            size_t used = 0;
            while (i < gcm_init_image_word_count &&
                   gcm_init_image_words[i].io_offset == start + (uint32_t)used &&
                   used + 4u <= sizeof svc->image_run) {
                put_be32(&svc->image_run[used], gcm_init_image_words[i].word);
                used += 4u;
                i++;
            }
            if (guest_write(svc, req->io_ea + start, svc->image_run, used))
                return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED,
                            GCM_INIT_STEP_WRITE_IMAGE);
        }
    }

    /* Step 3b: context descriptor D (16 bytes). */
    {
        uint8_t d[16];
        put_be32(&d[0], req->io_ea + GCM_INIT_IMAGE_START_GET);
        put_be32(&d[4], req->io_ea + GCM_INIT_CONTEXT_END_OFFSET);
        put_be32(&d[8], req->io_ea + GCM_INIT_IMAGE_PUT);
        put_be32(&d[12], svc->layout.callback_opd_ea);
        if (guest_write(svc, svc->layout.context_ea, d, sizeof d))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED,
                        GCM_INIT_STEP_WRITE_CONTEXT);
    }

    /* Step 3c: callback OPD {code, toc}. */
    {
        uint8_t opd[8];
        put_be32(&opd[0], svc->layout.callback_code);
        put_be32(&opd[4], svc->layout.callback_toc);
        if (guest_write(svc, svc->layout.callback_opd_ea, opd, sizeof opd))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED, GCM_INIT_STEP_WRITE_OPD);
    }

    /* Step 3d: report words. */
    {
        static const uint32_t offsets[3] = {0x20u, 0x10u, 0x00u};
        static const uint32_t values[3] = {1u, 0u, 0u};
        for (uint32_t k = 0; k < 3; k++) {
            uint8_t word[4];
            put_be32(word, values[k]);
            if (guest_write(svc, svc->layout.reports_ea + offsets[k], word, 4))
                return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED,
                            GCM_INIT_STEP_WRITE_REPORTS);
        }
    }

    /* Step 3e: control put = get = 0x1000, then the flush (put = 0x13a0). */
    {
        uint8_t word[4];
        put_be32(word, GCM_INIT_IMAGE_START_GET);
        if (guest_write(svc, svc->layout.control_ea, word, 4))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED, GCM_INIT_STEP_WRITE_CONTROL);
        if (guest_write(svc, svc->layout.control_ea + 4u, word, 4))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED, GCM_INIT_STEP_WRITE_CONTROL);
        put_be32(word, GCM_INIT_IMAGE_PUT);
        if (guest_write(svc, svc->layout.control_ea, word, 4))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED, GCM_INIT_STEP_WRITE_CONTROL);
    }

    /* Step 4: consume the command stream for real. */
    {
        gcm_fe_ports fe_ports;
        fe_ports.user = svc;
        fe_ports.semaphore_read = service_semaphore_read;
        fe_ports.publish_reference = service_publish_reference;
        if (gcm_rsx_frontend_init(&svc->fe, &fe_ports, gcm_init_state_allowlist,
                                  gcm_init_state_allowlist_count) != GCM_FE_OK)
            return fail(svc, result, GCM_INIT_FRONTEND_FAILED, GCM_INIT_STEP_WALK);

        rsx_fifo_reader reader;
        reader.user = svc;
        reader.read_word = service_read_word;
        rsx_fifo_sink sink;
        sink.user = &svc->fe;
        sink.method = gcm_rsx_frontend_sink;

        rsx_fifo_walker walker;
        memset(&walker, 0, sizeof walker);
        if (rsx_fifo_walker_init(&walker, reader, sink, GCM_INIT_IMAGE_START_GET) !=
            RSX_FIFO_REACHED_PUT)
            return fail(svc, result, GCM_INIT_WALK_FAILED, GCM_INIT_STEP_WALK);
        rsx_fifo_walker_publish_put(&walker, GCM_INIT_IMAGE_PUT);

        rsx_fifo_pump_report report;
        rsx_fifo_status walk_status =
            rsx_fifo_walker_pump(&walker, GCM_INIT_WALK_BUDGET, NULL, &report);
        result->walker_status = (uint32_t)walk_status;
        result->io_offset = report.get;
        result->final_put = report.put;
        result->final_get = report.get;
        {
            const rsx_fifo_failure *wf = rsx_fifo_walker_failure(&walker);
            if (wf) {
                result->walker_failure_offset = wf->offset;
                result->walker_effect = (uint32_t)wf->effect;
            }
        }
        copy_front_end_failure(svc, result);
        copy_front_end_counters(svc, result);

        if (walk_status != RSX_FIFO_REACHED_PUT)
            return fail(svc, result,
                        gcm_rsx_frontend_failed(&svc->fe) ? GCM_INIT_FRONTEND_FAILED
                                                          : GCM_INIT_WALK_FAILED,
                        GCM_INIT_STEP_WALK);
        if (report.get != GCM_INIT_IMAGE_PUT)
            return fail(svc, result, GCM_INIT_WALK_FAILED, GCM_INIT_STEP_WALK);
        if (gcm_rsx_frontend_failed(&svc->fe))
            return fail(svc, result, GCM_INIT_FRONTEND_FAILED, GCM_INIT_STEP_WALK);
        if (svc->fe.accepted_methods != GCM_INIT_IMAGE_METHOD_COUNT)
            return fail(svc, result, GCM_INIT_METHOD_COUNT_MISMATCH, GCM_INIT_STEP_WALK);
        if (svc->fe.references_published != 1u || svc->fe.last_reference != 0xFFFFFFFFu)
            return fail(svc, result, GCM_INIT_REFERENCE_MISMATCH, GCM_INIT_STEP_WALK);
    }

    /* Step 5: acknowledge on control get, then publish the context EA. */
    {
        uint8_t word[4];
        put_be32(word, GCM_INIT_IMAGE_PUT);
        if (guest_write(svc, svc->layout.control_ea + 4u, word, 4))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED,
                        GCM_INIT_STEP_WRITE_CONTROL_GET);
        put_be32(word, svc->layout.context_ea);
        if (guest_write(svc, req->ctx_out_ea, word, 4))
            return fail(svc, result, GCM_INIT_GUEST_WRITE_FAILED,
                        GCM_INIT_STEP_WRITE_CTX_OUT);
    }

    svc->state = SERVICE_STATE_INITIALIZED;
    result->status = GCM_INIT_OK;
    result->failed_step = GCM_INIT_STEP_NONE;
    result->r3 = 0;
    result->committed = svc->guest_attempted;
    return GCM_INIT_OK;
}

static int service_is_initialized(const gcm_init_service *svc)
{
    return svc && svc->state == SERVICE_STATE_INITIALIZED;
}
/* Root (2026-11-11): the live RSX consumer starts only after cellGcmInit has run. A plain int read. */
int gcm_init_service_is_initialized(const gcm_init_service *svc) { return service_is_initialized(svc); }

gcm_init_status gcm_init_service_configuration(const gcm_init_service *svc,
                                               gcm_init_config *out)
{
    if (!svc || !out) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    out->local_address = GCM_INIT_LOCAL_BASE;
    out->io_address = svc->as.cfg.io_ea;
    out->local_size = GCM_INIT_LOCAL_SIZE;
    out->io_size = svc->as.cfg.io_size;
    out->memory_frequency = GCM_INIT_MEMORY_FREQUENCY;
    out->core_frequency = GCM_INIT_CORE_FREQUENCY;
    return GCM_INIT_OK;
}

void gcm_init_config_encode(const gcm_init_config *config, uint8_t out[24])
{
    if (!config || !out) return;
    put_be32(&out[0], config->local_address);
    put_be32(&out[4], config->io_address);
    put_be32(&out[8], config->local_size);
    put_be32(&out[12], config->io_size);
    put_be32(&out[16], config->memory_frequency);
    put_be32(&out[20], config->core_frequency);
}

gcm_init_status gcm_init_service_label_address(const gcm_init_service *svc, uint32_t index,
                                               uint32_t *ea_out)
{
    if (!svc || !ea_out) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    *ea_out = svc->layout.reports_ea + ((index << 4) & 0xFF0u);
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_control_register(const gcm_init_service *svc,
                                                  uint32_t *ea_out)
{
    if (!svc || !ea_out) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    *ea_out = svc->layout.control_ea;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_offset_tables(gcm_init_service *svc,
                                               uint32_t *ea_table_ea_out,
                                               uint32_t *io_table_ea_out)
{
    if (!svc || !ea_table_ea_out || !io_table_ea_out) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;

    if (!svc->tables_materialized) {
        uint8_t ea_to_io[GCM_INIT_EA_TABLE_BYTES];
        uint8_t io_to_ea[GCM_INIT_IO_TABLE_BYTES];
        for (uint32_t i = 0; i < GCM_INIT_EA_TABLE_PAGES; i++) {
            put_be16(&ea_to_io[i * 2u], 0xFFFFu);
        }
        for (uint32_t i = 0; i < GCM_INIT_IO_TABLE_PAGES; i++) {
            put_be16(&io_to_ea[i * 2u], 0xFFFFu);
        }
        for (uint32_t slot = 0; slot < svc->as.cfg.slot_count; slot++) {
            gcm_as_mapping info;
            if (gcm_as_get_mapping(&svc->as, slot, &info) != GCM_AS_OK || !info.active)
                continue;
            const uint32_t pages = info.size >> 20;
            for (uint32_t p = 0; p < pages; p++) {
                const uint32_t ea_page = (info.ea >> 20) + p;
                const uint32_t io_page = (info.io >> 20) + p;
                if (ea_page < GCM_INIT_EA_TABLE_PAGES && io_page <= 0xFFFFu) {
                    put_be16(&ea_to_io[ea_page * 2u], (uint16_t)io_page);
                }
                if (io_page < GCM_INIT_IO_TABLE_PAGES && ea_page <= 0xFFFFu) {
                    put_be16(&io_to_ea[io_page * 2u], (uint16_t)ea_page);
                }
            }
        }
        if (svc->ports.write(svc->ports.user, svc->layout.offset_table_ea, ea_to_io,
                             sizeof ea_to_io) != 0)
            return GCM_INIT_GUEST_WRITE_FAILED;
        if (svc->ports.write(svc->ports.user, svc->layout.io_table_ea, io_to_ea,
                             sizeof io_to_ea) != 0)
            return GCM_INIT_GUEST_WRITE_FAILED;
        svc->tables_materialized = 1;
    }

    *ea_table_ea_out = svc->layout.offset_table_ea;
    *io_table_ea_out = svc->layout.io_table_ea;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_set_flip_mode(gcm_init_service *svc, uint32_t mode)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (mode != 1u && mode != 2u) return GCM_INIT_UNSUPPORTED_FLIP_MODE;
    svc->flip_mode = mode;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_set_tile_info(gcm_init_service *svc, uint32_t index,
                                               uint32_t location, uint32_t offset,
                                               uint32_t size, uint32_t pitch,
                                               uint32_t comp, uint32_t base, uint32_t bank)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (index >= GCM_INIT_TILE_COUNT) return GCM_INIT_UNSUPPORTED_TILE;
    if (location != 0u) return GCM_INIT_UNSUPPORTED_TILE;
    if ((offset & 0xFFFFu) != 0u) return GCM_INIT_UNSUPPORTED_TILE;
    if ((size & 0xFFFFu) != 0u) return GCM_INIT_UNSUPPORTED_TILE;
    if (size == 0u) return GCM_INIT_UNSUPPORTED_TILE;
    if ((uint64_t)offset + (uint64_t)size > (uint64_t)GCM_INIT_LOCAL_SIZE)
        return GCM_INIT_UNSUPPORTED_TILE;
    if (pitch == 0u || (pitch & 0xFFu) != 0u) return GCM_INIT_UNSUPPORTED_TILE;
    if (bank >= 4u) return GCM_INIT_UNSUPPORTED_TILE;

    gcm_init_tile *tile = &svc->tiles[index];
    memset(tile, 0, sizeof *tile);
    tile->location = location;
    tile->offset = offset;
    tile->size = size;
    tile->pitch = pitch;
    tile->comp = comp;
    tile->base = base;
    tile->bank = bank;
    tile->set = 1;
    tile->bound = 0;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_bind_tile(gcm_init_service *svc, uint32_t index)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (index >= GCM_INIT_TILE_COUNT) return GCM_INIT_UNSUPPORTED_TILE;
    if (!svc->tiles[index].set) return GCM_INIT_UNSUPPORTED_TILE;
    svc->tiles[index].bound = 1;
    return GCM_INIT_OK;
}

const gcm_init_tile *gcm_init_service_tile(const gcm_init_service *svc, uint32_t index)
{
    if (!svc || index >= GCM_INIT_TILE_COUNT) return NULL;
    return &svc->tiles[index];
}

gcm_init_status gcm_init_service_bind_zcull(gcm_init_service *svc, uint32_t index,
                                            const gcm_init_zcull *args)
{
    if (!svc || !args) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (index >= GCM_INIT_ZCULL_COUNT) return GCM_INIT_UNSUPPORTED_ZCULL;
    if ((args->offset & 0xFFFFu) != 0u) return GCM_INIT_UNSUPPORTED_ZCULL;
    if (args->offset >= GCM_INIT_LOCAL_SIZE) return GCM_INIT_UNSUPPORTED_ZCULL;
    if (args->width == 0u || (args->width & 63u) != 0u || args->width > 4096u)
        return GCM_INIT_UNSUPPORTED_ZCULL;
    if (args->height == 0u || (args->height & 63u) != 0u || args->height > 4096u)
        return GCM_INIT_UNSUPPORTED_ZCULL;
    if ((args->cull_start & 0xFFFu) != 0u) return GCM_INIT_UNSUPPORTED_ZCULL;
    if (args->z_format != 1u && args->z_format != 2u) return GCM_INIT_UNSUPPORTED_ZCULL;

    gcm_init_zcull *zcull = &svc->zculls[index];
    memset(zcull, 0, sizeof *zcull);
    zcull->offset = args->offset;
    zcull->width = args->width;
    zcull->height = args->height;
    zcull->cull_start = args->cull_start;
    zcull->z_format = args->z_format;
    zcull->aa_format = args->aa_format;
    zcull->zcull_dir = args->zcull_dir;
    zcull->zcull_format = args->zcull_format;
    zcull->s_func = args->s_func;
    zcull->s_ref = args->s_ref;
    zcull->s_mask = args->s_mask;
    zcull->set = 1;
    zcull->bound = 1;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_unbind_zcull(gcm_init_service *svc, uint32_t index)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (index >= GCM_INIT_ZCULL_COUNT) return GCM_INIT_UNSUPPORTED_ZCULL;
    svc->zculls[index].set = 0;
    svc->zculls[index].bound = 0;
    return GCM_INIT_OK;
}

gcm_init_status gcm_init_service_unbind_tile(gcm_init_service *svc, uint32_t index)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (index >= GCM_INIT_TILE_COUNT) return GCM_INIT_UNSUPPORTED_TILE;
    svc->tiles[index].bound = 0;
    return GCM_INIT_OK;
}

const gcm_init_zcull *gcm_init_service_zcull(const gcm_init_service *svc, uint32_t index)
{
    if (!svc || index >= GCM_INIT_ZCULL_COUNT) return NULL;
    return &svc->zculls[index];
}

gcm_init_status gcm_init_service_set_display_buffer(gcm_init_service *svc, uint32_t id,
                                                    uint32_t offset, uint32_t pitch,
                                                    uint32_t width, uint32_t height)
{
    if (!svc) return GCM_INIT_INVALID_ARGUMENT;
    if (!service_is_initialized(svc)) return GCM_INIT_NOT_INITIALIZED;
    if (id >= GCM_INIT_DISPLAY_BUFFER_COUNT) return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;
    if (width == 0u || width > 4096u) return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;
    if (height == 0u || height > 4096u) return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;
    if (pitch == 0u || (pitch & 63u) != 0u) return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;
    if ((offset & 0xFFu) != 0u) return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;
    if ((uint64_t)offset + (uint64_t)pitch * (uint64_t)height > (uint64_t)GCM_INIT_LOCAL_SIZE)
        return GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER;

    gcm_init_display_buffer *buffer = &svc->displays[id];
    memset(buffer, 0, sizeof *buffer);
    buffer->offset = offset;
    buffer->pitch = pitch;
    buffer->width = width;
    buffer->height = height;
    buffer->set = 1;
    return GCM_INIT_OK;
}

const gcm_init_display_buffer *gcm_init_service_display_buffer(const gcm_init_service *svc,
                                                              uint32_t id)
{
    if (!svc || id >= GCM_INIT_DISPLAY_BUFFER_COUNT) return NULL;
    return &svc->displays[id];
}

const char *gcm_init_status_name(gcm_init_status status)
{
    switch (status) {
    case GCM_INIT_OK: return "OK";
    case GCM_INIT_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case GCM_INIT_UNSUPPORTED_REQUEST: return "UNSUPPORTED_REQUEST";
    case GCM_INIT_ALREADY_INITIALIZED: return "ALREADY_INITIALIZED";
    case GCM_INIT_ALREADY_FAILED: return "ALREADY_FAILED";
    case GCM_INIT_ADDRESS_SPACE_FAILED: return "ADDRESS_SPACE_FAILED";
    case GCM_INIT_MAPPING_MISMATCH: return "MAPPING_MISMATCH";
    case GCM_INIT_GUEST_WRITE_FAILED: return "GUEST_WRITE_FAILED";
    case GCM_INIT_WALK_FAILED: return "WALK_FAILED";
    case GCM_INIT_FRONTEND_FAILED: return "FRONTEND_FAILED";
    case GCM_INIT_METHOD_COUNT_MISMATCH: return "METHOD_COUNT_MISMATCH";
    case GCM_INIT_REFERENCE_MISMATCH: return "REFERENCE_MISMATCH";
    case GCM_INIT_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case GCM_INIT_UNSUPPORTED_FLIP_MODE: return "UNSUPPORTED_FLIP_MODE";
    case GCM_INIT_UNSUPPORTED_TILE: return "UNSUPPORTED_TILE";
    case GCM_INIT_UNSUPPORTED_ZCULL: return "UNSUPPORTED_ZCULL";
    case GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER: return "UNSUPPORTED_DISPLAY_BUFFER";
    }
    return "UNKNOWN";
}

const char *gcm_init_step_name(gcm_init_step step)
{
    switch (step) {
    case GCM_INIT_STEP_NONE: return "NONE";
    case GCM_INIT_STEP_REQUEST: return "REQUEST";
    case GCM_INIT_STEP_ADDRESS_SPACE: return "ADDRESS_SPACE";
    case GCM_INIT_STEP_DEVICE: return "DEVICE";
    case GCM_INIT_STEP_WRITE_IMAGE: return "WRITE_IMAGE";
    case GCM_INIT_STEP_WRITE_CONTEXT: return "WRITE_CONTEXT";
    case GCM_INIT_STEP_WRITE_OPD: return "WRITE_OPD";
    case GCM_INIT_STEP_WRITE_REPORTS: return "WRITE_REPORTS";
    case GCM_INIT_STEP_WRITE_CONTROL: return "WRITE_CONTROL";
    case GCM_INIT_STEP_WALK: return "WALK";
    case GCM_INIT_STEP_WRITE_CONTROL_GET: return "WRITE_CONTROL_GET";
    case GCM_INIT_STEP_WRITE_CTX_OUT: return "WRITE_CTX_OUT";
    case GCM_INIT_STEP_COUNT: break;
    }
    return "UNKNOWN";
}
