/* Host tests for the native GCM-init service and the RSX front end.
 *
 * No global state: every case builds its own fake guest, address space and service, then
 * frees them. The fake guest is a list of explicit byte regions; its write port records
 * every write, can fail a chosen address, and can run a test hook after each write so a
 * case can tamper with the command image between the service's write and its walk.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcm_init_expected_methods.h"
#include "gcm_init_image.h"
#include "gcm_init_service.h"
#include "gcm_offset_table_expected.h"

#define IO_EA 0x40700000u
#define CTX_OUT_EA 0x009B56E8u
#define CMD_SIZE 0x00010000u
#define IO_SIZE 0x08500000u
#define CONTEXT_EA 0x10001000u
#define OPD_EA 0x10002000u
#define CONTROL_EA 0x10003000u
#define REPORTS_EA 0x10004000u
#define REPORTS_SIZE 0x1000u
#define OFFSET_TABLE_EA 0x10010000u
#define IO_TABLE_EA 0x10012000u
#define CALLBACK_CODE 0x0000BE54u
#define CALLBACK_TOC 0x00140000u

#define IMAGE_END (IO_EA + GCM_INIT_IMAGE_PUT)

static int checks;
static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                            \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                               \
    do {                                                                             \
        uint32_t a_ = (uint32_t)(actual);                                             \
        uint32_t e_ = (uint32_t)(expected);                                           \
        checks++;                                                                    \
        if (a_ != e_) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == 0x%08x, expected 0x%08x\n", __FILE__,  \
                    __LINE__, #actual, a_, e_);                                       \
        }                                                                            \
    } while (0)

/* ------------------------------------------------------------------------------------ */
/* Fake guest                                                                           */
/* ------------------------------------------------------------------------------------ */

#define FAKE_MAX_REGIONS 8
#define FAKE_MAX_LOG 64

typedef struct fake_region {
    uint32_t ea, size;
    uint8_t *bytes;
} fake_region;

typedef struct fake_guest {
    fake_region region[FAKE_MAX_REGIONS];
    int count;
    uint32_t fail_write_ea;    /* nonzero: writes starting at this EA fail */
    int fail_write_index;      /* >= 0: fail the Nth write attempt (0-based) */
    int fail_write_all;
    uint32_t fail_read_ea;
    void (*after_write)(struct fake_guest *g);
    void *hook_user;
    uint32_t write_count;      /* every write attempt */
    uint32_t write_ea[FAKE_MAX_LOG];
    uint32_t write_len[FAKE_MAX_LOG];
} fake_guest;

static fake_region *fake_find(fake_guest *g, uint32_t ea, uint32_t length)
{
    for (int i = 0; i < g->count; i++) {
        fake_region *r = &g->region[i];
        if (ea >= r->ea && (uint64_t)ea + length <= (uint64_t)r->ea + r->size) return r;
    }
    return NULL;
}

static void fake_add(fake_guest *g, uint32_t ea, uint32_t size)
{
    if (g->count >= FAKE_MAX_REGIONS) {
        fprintf(stderr, "fake guest: too many regions\n");
        exit(2);
    }
    fake_region *r = &g->region[g->count++];
    r->ea = ea;
    r->size = size;
    r->bytes = calloc(size ? size : 1u, 1);
    if (!r->bytes) {
        fprintf(stderr, "fake guest: out of memory\n");
        exit(2);
    }
}

static void fake_free(fake_guest *g)
{
    for (int i = 0; i < g->count; i++) free(g->region[i].bytes);
    memset(g, 0, sizeof *g);
}

static void fake_store(fake_guest *g, uint32_t ea, uint32_t value)
{
    fake_region *r = fake_find(g, ea, 4);
    if (!r) {
        fprintf(stderr, "fake guest: no region for store at 0x%08x\n", ea);
        exit(2);
    }
    uint8_t *p = r->bytes + (ea - r->ea);
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint32_t fake_load(const fake_guest *g, uint32_t ea)
{
    for (int i = 0; i < g->count; i++) {
        const fake_region *r = &g->region[i];
        if (ea >= r->ea && (uint64_t)ea + 4 <= (uint64_t)r->ea + r->size) {
            const uint8_t *p = r->bytes + (ea - r->ea);
            return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
        }
    }
    fprintf(stderr, "fake guest: no region for load at 0x%08x\n", ea);
    exit(2);
}

static int fake_port_write(void *user, uint32_t ea, const void *bytes, size_t length)
{
    fake_guest *g = user;
    const uint32_t index = g->write_count;
    if (g->write_count < FAKE_MAX_LOG) {
        g->write_ea[g->write_count] = ea;
        g->write_len[g->write_count] = (uint32_t)length;
    }
    g->write_count++;
    fake_region *r = fake_find(g, ea, (uint32_t)length);
    if (!r) return 1;
    if (g->fail_write_all) return 1;
    if (g->fail_write_index >= 0 && index == (uint32_t)g->fail_write_index) return 1;
    if (g->fail_write_ea && ea == g->fail_write_ea) return 1;
    memcpy(r->bytes + (ea - r->ea), bytes, length);
    if (g->after_write) g->after_write(g);
    return 0;
}

static int fake_port_read(void *user, uint32_t ea, void *bytes, size_t length)
{
    fake_guest *g = user;
    fake_region *r = fake_find(g, ea, (uint32_t)length);
    if (!r) return 1;
    if (g->fail_read_ea && ea == g->fail_read_ea) return 1;
    memcpy(bytes, r->bytes + (ea - r->ea), length);
    return 0;
}

/* Guest regions used by every service case. */
static void fake_init(fake_guest *g)
{
    memset(g, 0, sizeof *g);
    g->fail_write_index = -1;
    fake_add(g, IO_EA, GCM_INIT_IMAGE_PUT);          /* covers both image runs */
    fake_add(g, CTX_OUT_EA, 4);
    fake_add(g, CONTEXT_EA, 16);
    fake_add(g, OPD_EA, 8);
    fake_add(g, CONTROL_EA, 12);
    fake_add(g, REPORTS_EA, REPORTS_SIZE);
    fake_add(g, OFFSET_TABLE_EA, GCM_INIT_EA_TABLE_BYTES);
    fake_add(g, IO_TABLE_EA, GCM_INIT_IO_TABLE_BYTES);
}

static gcm_init_layout test_layout(void)
{
    gcm_init_layout layout;
    memset(&layout, 0, sizeof layout);
    layout.context_ea = CONTEXT_EA;
    layout.callback_opd_ea = OPD_EA;
    layout.callback_code = CALLBACK_CODE;
    layout.callback_toc = CALLBACK_TOC;
    layout.control_ea = CONTROL_EA;
    layout.reports_ea = REPORTS_EA;
    layout.reports_size = REPORTS_SIZE;
    layout.offset_table_ea = OFFSET_TABLE_EA;
    layout.io_table_ea = IO_TABLE_EA;
    return layout;
}

static gcm_guest_ports ports_for(fake_guest *g)
{
    gcm_guest_ports ports;
    ports.user = g;
    ports.write = fake_port_write;
    ports.read = fake_port_read;
    return ports;
}

static gcm_init_request exact_request(void)
{
    gcm_init_request req;
    req.ctx_out_ea = CTX_OUT_EA;
    req.cmd_size = CMD_SIZE;
    req.io_size = IO_SIZE;
    req.io_ea = IO_EA;
    return req;
}

static gcm_init_service *service_new(fake_guest *g)
{
    gcm_init_layout layout = test_layout();
    gcm_guest_ports ports = ports_for(g);
    gcm_init_service *svc = malloc(sizeof *svc);
    if (!svc) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    gcm_init_status st = gcm_init_service_create(svc, &layout, &ports);
    if (st != GCM_INIT_OK) {
        fprintf(stderr, "service create failed: %s\n", gcm_init_status_name(st));
        exit(2);
    }
    return svc;
}

/* ------------------------------------------------------------------------------------ */
/* Snapshot helper                                                                      */
/* ------------------------------------------------------------------------------------ */

static size_t fake_bytes(const fake_guest *g)
{
    size_t total = 0;
    for (int i = 0; i < g->count; i++) total += g->region[i].size;
    return total;
}

static void fake_snapshot(const fake_guest *g, uint8_t *out)
{
    size_t used = 0;
    for (int i = 0; i < g->count; i++) {
        memcpy(out + used, g->region[i].bytes, g->region[i].size);
        used += g->region[i].size;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Image helpers                                                                        */
/* ------------------------------------------------------------------------------------ */

static int image_last_header(uint32_t *offset_out, uint32_t *word_out)
{
    int found = 0;
    for (uint32_t i = 0; i < gcm_init_image_word_count; i++) {
        const uint32_t off = gcm_init_image_words[i].io_offset;
        const uint32_t w = gcm_init_image_words[i].word;
        if (off >= GCM_INIT_IMAGE_PUT) continue;
        if ((w & 0xE0000003u) == 0x20000000u) continue;
        if ((w & 3u) == 1u || (w & 3u) == 2u) continue;
        if (w == 0x00020000u) continue;
        if ((w & 0xA0030003u) != 0) continue;
        *offset_out = off;
        *word_out = w;
        found = 1;
    }
    return found;
}

/* ------------------------------------------------------------------------------------ */
/* Case 1: generator / walker / front-end equivalence                                   */
/* ------------------------------------------------------------------------------------ */

typedef struct fe_test_ports {
    uint32_t flip;
    uint32_t ref_count;
    uint32_t ref_value;
    int publish_rc;
} fe_test_ports;

static int fe_semaphore_read(void *user, uint32_t context_handle, uint32_t offset,
                             uint32_t *value_out)
{
    fe_test_ports *p = user;
    if (context_handle == 0x56616661u && offset == 0x30u) {
        *value_out = p->flip;
        return 0;
    }
    return 1;
}

static int fe_publish_reference(void *user, uint32_t value)
{
    fe_test_ports *p = user;
    if (p->publish_rc != 0) return p->publish_rc;
    p->ref_count++;
    p->ref_value = value;
    return 0;
}

typedef struct recording_sink {
    gcm_rsx_frontend fe;
    uint32_t count;
    gcm_init_expected_method rec[GCM_INIT_EXPECTED_METHOD_COUNT + 8];
} recording_sink;

static int recording_sink_method(void *user, uint32_t subchannel, uint32_t method, uint32_t data,
                                 uint32_t io_offset, rsx_fifo_effect *effect)
{
    recording_sink *r = user;
    if (r->count < GCM_INIT_EXPECTED_METHOD_COUNT + 8) {
        r->rec[r->count].subchannel = subchannel;
        r->rec[r->count].method = method;
        r->rec[r->count].data = data;
        r->rec[r->count].io_offset = io_offset;
    }
    r->count++;
    return gcm_rsx_frontend_sink(&r->fe, subchannel, method, data, io_offset, effect);
}

typedef struct test_reader {
    fake_guest *guest;
    const gcm_address_space *as;
} test_reader;

static rsx_fifo_read_result test_read_word(void *user, uint32_t io_offset, uint8_t out_be[4])
{
    test_reader *r = user;
    uint32_t ea = 0;
    if (gcm_as_io_to_address(r->as, io_offset, 4, &ea) != GCM_AS_OK)
        return RSX_FIFO_IO_UNMAPPED;
    if (fake_port_read(r->guest, ea, out_be, 4) != 0) return RSX_FIFO_IO_FAULT;
    return RSX_FIFO_IO_OK;
}

static void write_image_into_guest(fake_guest *g)
{
    uint32_t i = 0;
    while (i < gcm_init_image_word_count) {
        const uint32_t start = gcm_init_image_words[i].io_offset;
        uint32_t count = 0;
        while (i + count < gcm_init_image_word_count &&
               gcm_init_image_words[i + count].io_offset == start + 4u * count)
            count++;
        for (uint32_t k = 0; k < count; k++)
            fake_store(g, IO_EA + gcm_init_image_words[i + k].io_offset,
                       gcm_init_image_words[i + k].word);
        i += count;
    }
}

static void test_generator_walker_equivalence(void)
{
    fake_guest guest;
    fake_init(&guest);
    write_image_into_guest(&guest);

    gcm_as_mapping slots[1];
    gcm_as_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_base = GCM_INIT_LOCAL_BASE;
    cfg.local_size = GCM_INIT_LOCAL_SIZE;
    cfg.io_ea = IO_EA;
    cfg.io_size = IO_SIZE;
    cfg.io_space_size = GCM_INIT_IO_SPACE_SIZE;
    cfg.slots = slots;
    cfg.slot_count = 1;
    gcm_address_space as;
    CHECK(gcm_as_init(&as, &cfg) == GCM_AS_OK);

    recording_sink *rec = malloc(sizeof *rec);
    if (!rec) exit(2);
    memset(rec, 0, sizeof *rec);
    fe_test_ports fp;
    memset(&fp, 0, sizeof fp);
    fp.flip = 1;
    gcm_fe_ports ports;
    ports.user = &fp;
    ports.semaphore_read = fe_semaphore_read;
    ports.publish_reference = fe_publish_reference;
    CHECK(gcm_rsx_frontend_init(&rec->fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);

    test_reader reader;
    reader.guest = &guest;
    reader.as = &as;
    rsx_fifo_reader fifo_reader;
    fifo_reader.user = &reader;
    fifo_reader.read_word = test_read_word;
    rsx_fifo_sink sink;
    sink.user = rec;
    sink.method = recording_sink_method;

    rsx_fifo_walker walker;
    memset(&walker, 0, sizeof walker);
    CHECK(rsx_fifo_walker_init(&walker, fifo_reader, sink, GCM_INIT_IMAGE_START_GET) ==
          RSX_FIFO_REACHED_PUT);
    CHECK(rsx_fifo_walker_publish_put(&walker, GCM_INIT_IMAGE_PUT) == RSX_FIFO_REACHED_PUT);
    rsx_fifo_pump_report report;
    rsx_fifo_status st = rsx_fifo_walker_pump(&walker, GCM_INIT_WALK_BUDGET, NULL, &report);
    CHECK(st == RSX_FIFO_REACHED_PUT);
    CHECK_EQ_U32(report.get, GCM_INIT_IMAGE_PUT);
    CHECK(!gcm_rsx_frontend_failed(&rec->fe));
    CHECK_EQ_U32(rec->count, GCM_INIT_EXPECTED_METHOD_COUNT);
    CHECK_EQ_U32(rec->fe.accepted_methods, GCM_INIT_IMAGE_METHOD_COUNT);
    CHECK_EQ_U32(fp.ref_count, 1);
    CHECK_EQ_U32(fp.ref_value, 0xFFFFFFFFu);

    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < GCM_INIT_EXPECTED_METHOD_COUNT && i < rec->count; i++) {
        const gcm_init_expected_method *want = &gcm_init_expected_methods[i];
        if (rec->rec[i].subchannel != want->subchannel || rec->rec[i].method != want->method ||
            rec->rec[i].data != want->data || rec->rec[i].io_offset != want->io_offset) {
            if (mismatches < 4)
                fprintf(stderr,
                        "method %u: got (sub %u, m 0x%x, d 0x%08x, io 0x%x) want (sub %u, m 0x%x,"
                        " d 0x%08x, io 0x%x)\n",
                        i, rec->rec[i].subchannel, rec->rec[i].method, rec->rec[i].data,
                        rec->rec[i].io_offset, want->subchannel, want->method, want->data,
                        want->io_offset);
            mismatches++;
        }
    }
    CHECK_EQ_U32(mismatches, 0);
    CHECK(GCM_INIT_EXPECTED_METHOD_COUNT == GCM_INIT_IMAGE_METHOD_COUNT);

    free(rec);
    fake_free(&guest);
}

/* ------------------------------------------------------------------------------------ */
/* Case 2: full run                                                                     */
/* ------------------------------------------------------------------------------------ */

static void check_success_memory(const fake_guest *guest)
{
    const uint32_t begin = fake_load(guest, CONTEXT_EA + 0);
    const uint32_t end = fake_load(guest, CONTEXT_EA + 4);
    const uint32_t current = fake_load(guest, CONTEXT_EA + 8);
    CHECK_EQ_U32(begin, IO_EA + GCM_INIT_IMAGE_START_GET);
    CHECK_EQ_U32(end, IO_EA + GCM_INIT_CONTEXT_END_OFFSET);
    CHECK_EQ_U32(current, IO_EA + GCM_INIT_IMAGE_PUT);
    /* The first 32 KiB segment ends at the reserved last word, at or after the put pointer. */
    CHECK(current <= end);
    CHECK_EQ_U32(fake_load(guest, CONTEXT_EA + 12), OPD_EA);
    CHECK_EQ_U32(fake_load(guest, OPD_EA + 0), CALLBACK_CODE);
    CHECK_EQ_U32(fake_load(guest, OPD_EA + 4), CALLBACK_TOC);
    CHECK_EQ_U32(fake_load(guest, REPORTS_EA + 0x00), 0);
    CHECK_EQ_U32(fake_load(guest, REPORTS_EA + 0x10), 0);
    CHECK_EQ_U32(fake_load(guest, REPORTS_EA + 0x20), 1);
    CHECK_EQ_U32(fake_load(guest, CONTROL_EA + 0), GCM_INIT_IMAGE_PUT);
    CHECK_EQ_U32(fake_load(guest, CONTROL_EA + 4), GCM_INIT_IMAGE_PUT);
    CHECK_EQ_U32(fake_load(guest, CONTROL_EA + 8), 0xFFFFFFFFu);
    CHECK_EQ_U32(fake_load(guest, CTX_OUT_EA), CONTEXT_EA);

    uint32_t bad = 0;
    for (uint32_t i = 0; i < gcm_init_image_word_count; i++) {
        if (fake_load(guest, IO_EA + gcm_init_image_words[i].io_offset) !=
            gcm_init_image_words[i].word)
            bad++;
    }
    CHECK_EQ_U32(bad, 0);
}

static void test_full_run(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);

    CHECK_EQ_U32((uint32_t)result.r3, 0u);
    CHECK(result.status == GCM_INIT_OK);
    CHECK(result.failed_step == GCM_INIT_STEP_NONE);
    CHECK_EQ_U32(result.committed, 1u);
    CHECK(!gcm_rsx_frontend_failed(&svc->fe));
    CHECK_EQ_U32(result.methods_accepted, GCM_INIT_IMAGE_METHOD_COUNT);
    CHECK_EQ_U32(result.references_published, 1u);
    CHECK_EQ_U32(result.final_ref, 0xFFFFFFFFu);
    CHECK_EQ_U32(result.walker_status, RSX_FIFO_REACHED_PUT);
    CHECK_EQ_U32(result.final_put, GCM_INIT_IMAGE_PUT);
    CHECK_EQ_U32(result.final_get, GCM_INIT_IMAGE_PUT);
    CHECK_EQ_U32(result.io_offset, GCM_INIT_IMAGE_PUT);
    CHECK_EQ_U32(result.acquires_satisfied, 1u);

    check_success_memory(&guest);

    /* Exactly one active mapping: {ea IO_EA, io 0, size IO_SIZE}. */
    CHECK_EQ_U32(gcm_as_active_count(&svc->as), 1u);
    gcm_as_mapping info;
    CHECK(gcm_as_get_mapping(&svc->as, 0, &info) == GCM_AS_OK);
    CHECK_EQ_U32(info.ea, IO_EA);
    CHECK_EQ_U32(info.io, 0u);
    CHECK_EQ_U32(info.size, IO_SIZE);
    CHECK(info.active);
    CHECK(info.initial);

    /* A second run is refused and writes nothing. */
    uint8_t *before = malloc(fake_bytes(&guest));
    uint8_t *after = malloc(fake_bytes(&guest));
    if (!before || !after) exit(2);
    fake_snapshot(&guest, before);
    uint32_t writes_before = guest.write_count;
    gcm_init_result second;
    CHECK(gcm_init_service_run(svc, &req, &second) == GCM_INIT_ALREADY_INITIALIZED);
    CHECK_EQ_U32(guest.write_count, writes_before);
    fake_snapshot(&guest, after);
    CHECK(memcmp(before, after, fake_bytes(&guest)) == 0);
    CHECK(second.status == GCM_INIT_ALREADY_INITIALIZED);

    free(before);
    free(after);
    free(svc);
    fake_free(&guest);
}

/* ------------------------------------------------------------------------------------ */
/* Case 3: unsupported requests                                                         */
/* ------------------------------------------------------------------------------------ */

static void test_request_validation(void)
{
    static const int field_count = 4;
    for (int field = 0; field < field_count; field++) {
        fake_guest guest;
        fake_init(&guest);
        gcm_init_service *svc = service_new(&guest);
        gcm_init_request req = exact_request();
        switch (field) {
        case 0: req.ctx_out_ea += 4u; break;
        case 1: req.cmd_size += 4u; break;
        case 2: req.io_size -= 4u; break;
        case 3: req.io_ea += 4u; break;
        default: break;
        }
        uint8_t *before = malloc(fake_bytes(&guest));
        uint8_t *after = malloc(fake_bytes(&guest));
        if (!before || !after) exit(2);
        fake_snapshot(&guest, before);
        gcm_init_result result;
        CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_UNSUPPORTED_REQUEST);
        CHECK(result.failed_step == GCM_INIT_STEP_REQUEST);
        CHECK_EQ_U32(result.committed, 0u);
        CHECK_EQ_U32(guest.write_count, 0u);
        fake_snapshot(&guest, after);
        CHECK(memcmp(before, after, fake_bytes(&guest)) == 0);
        free(before);
        free(after);
        free(svc);
        fake_free(&guest);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Case 4: layout validation                                                            */
/* ------------------------------------------------------------------------------------ */

static void test_layout_validation(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_guest_ports ports = ports_for(&guest);
    gcm_init_service *svc = malloc(sizeof *svc);
    if (!svc) exit(2);
    gcm_init_layout layout = test_layout();

    layout = test_layout();
    layout.context_ea += 8u;   /* 4-aligned but not 16-aligned */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.callback_opd_ea += 4u;   /* not 8-aligned */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.control_ea = CONTEXT_EA + 4u;   /* overlaps context[16] */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.reports_size = GCM_INIT_REPORTS_MIN_SIZE - 4u;
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.reports_ea = 0;
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.callback_opd_ea = CONTROL_EA;   /* OPD[8] inside control[12] */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea = 0;
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.io_table_ea = 0;
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea += 2u;   /* not 4-aligned */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.io_table_ea += 2u;   /* not 4-aligned */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.io_table_ea = OFFSET_TABLE_EA + GCM_INIT_EA_TABLE_BYTES - 4u;   /* tables overlap */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea = CONTEXT_EA;   /* overlaps context[16] */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea = OPD_EA;   /* overlaps OPD[8] */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea = CONTROL_EA;   /* overlaps control[12] */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.offset_table_ea = REPORTS_EA;   /* overlaps reports */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    layout.io_table_ea = REPORTS_EA;   /* overlaps reports */
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    layout = test_layout();
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_OK);

    layout = test_layout();
    ports.write = NULL;
    CHECK(gcm_init_service_create(svc, &layout, &ports) == GCM_INIT_INVALID_ARGUMENT);

    free(svc);
    fake_free(&guest);
}

/* ------------------------------------------------------------------------------------ */
/* Case 5: forced-unsatisfied acquire                                                   */
/* ------------------------------------------------------------------------------------ */

static void test_flip_semaphore_zero(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_service_force_flip_semaphore(svc, 0);
    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_FRONTEND_FAILED);
    CHECK(result.failed_step == GCM_INIT_STEP_WALK);
    CHECK(result.fe_status == GCM_FE_ACQUIRE_UNSATISFIED);
    CHECK_EQ_U32(result.fe_method, 0x68u);
    CHECK_EQ_U32(result.walker_status, RSX_FIFO_SINK_FAILED);
    CHECK_EQ_U32(result.walker_effect, RSX_FIFO_EFFECT_NONE);
    CHECK_EQ_U32(result.committed, 1u);
    CHECK_EQ_U32(fake_load(&guest, CONTROL_EA + 8), 0u);            /* ref not published */
    CHECK_EQ_U32(fake_load(&guest, CONTROL_EA + 4), GCM_INIT_IMAGE_START_GET); /* get frozen */
    CHECK_EQ_U32(result.references_published, 0u);
    CHECK_EQ_U32(result.acquires_satisfied, 0u);
    /* FAILED is terminal: a second run is refused without a write. */
    uint32_t writes_before = guest.write_count;
    gcm_init_result second;
    CHECK(gcm_init_service_run(svc, &req, &second) == GCM_INIT_ALREADY_FAILED);
    CHECK_EQ_U32(guest.write_count, writes_before);
    free(svc);
    fake_free(&guest);
}

/* ------------------------------------------------------------------------------------ */
/* Case 6: tampered images                                                              */
/* ------------------------------------------------------------------------------------ */

typedef struct patch {
    uint32_t ea, value;
} patch;

typedef struct tamper_state {
    patch patches[4];
    int count;
} tamper_state;

static void tamper_after_write(fake_guest *g)
{
    tamper_state *t = g->hook_user;
    for (int i = 0; i < t->count; i++) fake_store(g, t->patches[i].ea, t->patches[i].value);
}

static void run_tampered(const char *name, const patch *patches, int count,
                         gcm_fe_status want_status, uint32_t want_ref, uint32_t want_get)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    tamper_state tamper;
    memset(&tamper, 0, sizeof tamper);
    for (int i = 0; i < count; i++) tamper.patches[i] = patches[i];
    tamper.count = count;
    guest.hook_user = &tamper;
    guest.after_write = tamper_after_write;

    gcm_init_request req = exact_request();
    gcm_init_result result;
    gcm_init_status st = gcm_init_service_run(svc, &req, &result);
    if (st == GCM_INIT_OK)
        fprintf(stderr, "tamper case %s unexpectedly succeeded\n", name);
    CHECK(st == GCM_INIT_FRONTEND_FAILED);
    if (result.fe_status != want_status)
        fprintf(stderr, "tamper case %s: fe_status %s, want %s\n", name,
                gcm_fe_status_name(result.fe_status), gcm_fe_status_name(want_status));
    CHECK(result.fe_status == want_status);
    CHECK(result.failed_step == GCM_INIT_STEP_WALK);
    CHECK_EQ_U32(result.walker_status, RSX_FIFO_SINK_FAILED);
    CHECK_EQ_U32(result.walker_effect, RSX_FIFO_EFFECT_NONE);
    CHECK_EQ_U32(result.committed, 1u);
    CHECK_EQ_U32(fake_load(&guest, CONTROL_EA + 8), want_ref);
    CHECK_EQ_U32(fake_load(&guest, CONTROL_EA + 4), want_get);

    gcm_init_result second;
    CHECK(gcm_init_service_run(svc, &req, &second) == GCM_INIT_ALREADY_FAILED);

    free(svc);
    fake_free(&guest);
}

static void test_tampered_images(void)
{
    uint32_t hdr_off = 0;
    uint32_t hdr_word = 0;
    CHECK(image_last_header(&hdr_off, &hdr_word));
    CHECK_EQ_U32(hdr_off, 0x1398u);

    /* Unknown 3D method on the bound NV4097 subchannel. */
    {
        patch p[1];
        p[0].ea = IO_EA + hdr_off;
        p[0].value = (hdr_word & ~0x1FFCu) | 0x1FFCu;
        run_tampered("not-allowlisted", p, 1, GCM_FE_NOT_ALLOWLISTED, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
    /* CLEAR_SURFACE selecting a buffer. */
    {
        patch p[2];
        p[0].ea = IO_EA + hdr_off;
        p[0].value = (hdr_word & ~0x1FFCu) | 0x1D94u;
        p[1].ea = IO_EA + hdr_off + 4u;
        p[1].value = 0x1u;
        run_tampered("clear-surface", p, 2, GCM_FE_ACTION_UNSUPPORTED, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
    /* IMAGE_IN with a non-zero clip size and a non-zero output size. */
    {
        patch p[2];
        p[0].ea = IO_EA + 0x1360u;   /* NV3089 CLIP_SIZE 0x030c */
        p[0].value = 0x00010001u;
        p[1].ea = IO_EA + 0x1368u;   /* NV3089 IMAGE_OUT_SIZE 0x0314 */
        p[1].value = 0x00010001u;
        run_tampered("image-in", p, 2, GCM_FE_ACTION_UNSUPPORTED, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
    /* A method on the subchannel bound to the unknown object 0xcafebabe. */
    {
        patch p[1];
        p[0].ea = IO_EA + hdr_off;
        p[0].value = (hdr_word & ~0xE000u & ~0x1FFCu) | (7u << 13) | 0x0100u;
        run_tampered("unknown-object", p, 1, GCM_FE_UNKNOWN_OBJECT_METHOD, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
    /* An unknown channel method (0x0040 < 0x100). */
    {
        patch p[1];
        p[0].ea = IO_EA + hdr_off;
        p[0].value = (hdr_word & ~0x1FFCu) | 0x0040u;
        run_tampered("channel-method", p, 1, GCM_FE_UNSUPPORTED_CHANNEL_METHOD, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
    /* A DMA semaphore context the service refuses. */
    {
        patch p[1];
        p[0].ea = IO_EA + 0x1004u;   /* SET_CONTEXT_DMA_SEMAPHORE data */
        p[0].value = 0x12345678u;
        run_tampered("semaphore-context", p, 1, GCM_FE_SEMAPHORE_CONTEXT_UNKNOWN, 0u,
                     GCM_INIT_IMAGE_START_GET);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Case 7: port failures                                                                */
/* ------------------------------------------------------------------------------------ */

static void test_write_failures(void)
{
    /* Reference run: record where every guest write goes. */
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    CHECK_EQ_U32(guest.write_count, 13u);
    CHECK_EQ_U32(guest.write_ea[0], IO_EA);
    CHECK_EQ_U32(guest.write_ea[1], IO_EA + GCM_INIT_IMAGE_START_GET);
    CHECK_EQ_U32(guest.write_ea[2], CONTEXT_EA);
    CHECK_EQ_U32(guest.write_ea[3], OPD_EA);
    CHECK_EQ_U32(guest.write_ea[4], REPORTS_EA + 0x20u);
    CHECK_EQ_U32(guest.write_ea[5], REPORTS_EA + 0x10u);
    CHECK_EQ_U32(guest.write_ea[6], REPORTS_EA + 0x00u);
    CHECK_EQ_U32(guest.write_ea[7], CONTROL_EA);
    CHECK_EQ_U32(guest.write_ea[8], CONTROL_EA + 4u);
    CHECK_EQ_U32(guest.write_ea[9], CONTROL_EA);
    CHECK_EQ_U32(guest.write_ea[10], CONTROL_EA + 8u);
    CHECK_EQ_U32(guest.write_ea[11], CONTROL_EA + 4u);
    CHECK_EQ_U32(guest.write_ea[12], CTX_OUT_EA);
    CHECK_EQ_U32(guest.write_len[0], 0x990u);
    CHECK_EQ_U32(guest.write_len[1], 0x3A0u);
    CHECK_EQ_U32(guest.write_len[10], 4u);

    uint32_t eaidx[13];
    uint32_t expected_step[13];
    for (int i = 0; i < 13; i++) eaidx[i] = guest.write_ea[i];
    expected_step[0] = GCM_INIT_STEP_WRITE_IMAGE;
    expected_step[1] = GCM_INIT_STEP_WRITE_IMAGE;
    expected_step[2] = GCM_INIT_STEP_WRITE_CONTEXT;
    expected_step[3] = GCM_INIT_STEP_WRITE_OPD;
    expected_step[4] = GCM_INIT_STEP_WRITE_REPORTS;
    expected_step[5] = GCM_INIT_STEP_WRITE_REPORTS;
    expected_step[6] = GCM_INIT_STEP_WRITE_REPORTS;
    expected_step[7] = GCM_INIT_STEP_WRITE_CONTROL;
    expected_step[8] = GCM_INIT_STEP_WRITE_CONTROL;
    expected_step[9] = GCM_INIT_STEP_WRITE_CONTROL;
    expected_step[10] = GCM_INIT_STEP_WALK;            /* SET_REFERENCE publish */
    expected_step[11] = GCM_INIT_STEP_WRITE_CONTROL_GET;
    expected_step[12] = GCM_INIT_STEP_WRITE_CTX_OUT;
    free(svc);
    fake_free(&guest);

    for (int i = 0; i < 13; i++) {
        fake_init(&guest);
        guest.fail_write_index = i;
        svc = service_new(&guest);
        gcm_init_result r;
        gcm_init_status st = gcm_init_service_run(svc, &req, &r);
        CHECK_EQ_U32(r.failed_step, expected_step[i]);
        CHECK_EQ_U32(r.committed, 1u);
        CHECK(r.r3 != 0);
        if (i == 10) {
            /* The publish port failed after a possible write: effect UNKNOWN, latched. */
            CHECK(st == GCM_INIT_FRONTEND_FAILED);
            CHECK(r.fe_status == GCM_FE_PORT_FAILED);
            CHECK_EQ_U32(r.fe_port_result, 2);
            CHECK_EQ_U32(r.walker_status, RSX_FIFO_SINK_FAILED);
            CHECK_EQ_U32(r.walker_effect, RSX_FIFO_EFFECT_UNKNOWN);
        } else {
            CHECK(st == GCM_INIT_GUEST_WRITE_FAILED);
        }
        if (st != GCM_INIT_OK || r.failed_step != expected_step[i])
            fprintf(stderr, "write %d at 0x%08x: status %s step %s\n", i, eaidx[i],
                    gcm_init_status_name(st), gcm_init_step_name(r.failed_step));
        free(svc);
        fake_free(&guest);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Case 8: front-end units                                                              */
/* ------------------------------------------------------------------------------------ */

static void test_frontend_units(void)
{
    gcm_rsx_frontend *fe = malloc(sizeof *fe);
    if (!fe) exit(2);
    fe_test_ports fp;
    memset(&fp, 0, sizeof fp);
    fp.flip = 1;
    gcm_fe_ports ports;
    ports.user = &fp;
    ports.semaphore_read = fe_semaphore_read;
    ports.publish_reference = fe_publish_reference;

    /* Ports are required. */
    gcm_fe_ports bad = ports;
    bad.semaphore_read = NULL;
    CHECK(gcm_rsx_frontend_init(fe, &bad, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_INVALID_ARGUMENT);

    /* The allowlist must be sorted by (cls, method) and unique. */
    {
        gcm_fe_allow unsorted[3];
        gcm_fe_allow duplicate[3];
        unsorted[0].cls = (uint8_t)GCM_FE_CLASS_NV4097;
        unsorted[0].method = 0x0180u;
        unsorted[1].cls = (uint8_t)GCM_FE_CLASS_NV4097;
        unsorted[1].method = 0x0100u;
        unsorted[2].cls = (uint8_t)GCM_FE_CLASS_NV0039;
        unsorted[2].method = 0x0100u;
        duplicate[0] = unsorted[1];
        duplicate[1] = unsorted[1];
        duplicate[2] = unsorted[2];
        CHECK(gcm_rsx_frontend_init(fe, &ports, unsorted, 3) == GCM_FE_INVALID_ARGUMENT);
        CHECK(gcm_rsx_frontend_init(fe, &ports, duplicate, 3) == GCM_FE_INVALID_ARGUMENT);
        CHECK(gcm_rsx_frontend_init(fe, &ports, NULL, 1) == GCM_FE_INVALID_ARGUMENT);
    }

    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    rsx_fifo_effect effect = RSX_FIFO_EFFECT_UNKNOWN;

    /* Unbound subchannel. */
    CHECK(gcm_rsx_frontend_sink(fe, 2, 0x0180u, 0u, 0x100u, &effect) != 0);
    CHECK(effect == RSX_FIFO_EFFECT_NONE);
    CHECK(gcm_rsx_frontend_failure(fe) != NULL);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_UNBOUND_SUBCHANNEL);
    CHECK(gcm_rsx_frontend_failure(fe)->subchannel == 2);

    /* Latch keeps the first failure. */
    effect = RSX_FIFO_EFFECT_UNKNOWN;
    CHECK(gcm_rsx_frontend_sink(fe, 0, 0x0180u, 0u, 0x104u, &effect) == (int)GCM_FE_LATCHED);
    CHECK(effect == RSX_FIFO_EFFECT_NONE);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_UNBOUND_SUBCHANNEL);
    CHECK_EQ_U32(gcm_rsx_frontend_failure(fe)->subchannel, 2u);
    CHECK_EQ_U32(gcm_rsx_frontend_failure(fe)->io_offset, 0x100u);

    /* Fresh front end: bind + shadow recording. */
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    CHECK_EQ_U32(gcm_rsx_frontend_bound_handle(fe, 0), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0000u, 0x31337000u, 0x0u, &effect), 0u);
    CHECK(gcm_rsx_frontend_bound_handle(fe, 0) == 0x31337000u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0180u, 0xDEADBEEFu, 0x4u, &effect), 0u);
    uint32_t value = 0;
    CHECK(gcm_rsx_frontend_shadow_get(fe, GCM_FE_CLASS_NV4097, 0x0180u, &value) == 1);
    CHECK_EQ_U32(value, 0xDEADBEEFu);
    CHECK(gcm_rsx_frontend_shadow_get(fe, GCM_FE_CLASS_NV4097, 0x0184u, &value) == 0);
    CHECK(gcm_rsx_frontend_shadow_get(fe, GCM_FE_CLASS_NV0039, 0x0180u, &value) == 0);
    CHECK_EQ_U32(fe->shadow_writes, 1u);
    CHECK_EQ_U32(fe->accepted_methods, 2u);

    /* Unknown object: the binding itself is accepted, later class methods are not. */
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 3, 0x0000u, 0xCAFEBABEu, 0x8u, &effect), 0u);
    effect = RSX_FIFO_EFFECT_UNKNOWN;
    CHECK(gcm_rsx_frontend_sink(fe, 3, 0x0180u, 0u, 0xCu, &effect) != 0);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_UNKNOWN_OBJECT_METHOD);

    /* Channel methods: state missing, then a refused context. */
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    CHECK(gcm_rsx_frontend_sink(fe, 0, 0x0068u, 1u, 0x0u, &effect) != 0);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_SEMAPHORE_STATE_MISSING);
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0060u, 0x12345678u, 0x0u, &effect), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0064u, 0x30u, 0x4u, &effect), 0u);
    effect = RSX_FIFO_EFFECT_UNKNOWN;
    CHECK(gcm_rsx_frontend_sink(fe, 0, 0x0068u, 1u, 0x8u, &effect) != 0);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_SEMAPHORE_CONTEXT_UNKNOWN);

    /* NV406E semaphore state is channel state, not per subchannel: set it on subchannel 0,
     * rebind subchannel 0 with SET_OBJECT, then ACQUIRE on subchannel 1 uses the same state. */
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    fp.flip = 1;
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0060u, 0x56616661u, 0x0u, &effect), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0064u, 0x30u, 0x4u, &effect), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 0, 0x0000u, 0x31337000u, 0x8u, &effect), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 1, 0x0000u, 0x31337000u, 0xCu, &effect), 0u);
    CHECK_EQ_U32(gcm_rsx_frontend_sink(fe, 1, 0x0068u, 1u, 0x10u, &effect), 0u);
    CHECK_EQ_U32(fe->acquires_satisfied, 1u);

    /* SET_REFERENCE port refusal (result 1) reports NONE. */
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    fp.publish_rc = 1;
    effect = RSX_FIFO_EFFECT_UNKNOWN;
    CHECK(gcm_rsx_frontend_sink(fe, 0, 0x0050u, 0xFFFFFFFFu, 0x0u, &effect) != 0);
    CHECK(effect == RSX_FIFO_EFFECT_NONE);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_PORT_REFUSED);
    CHECK_EQ_U32(gcm_rsx_frontend_failure(fe)->port_result, 1u);

    /* SET_REFERENCE port failure (result 2) reports UNKNOWN. */
    CHECK(gcm_rsx_frontend_init(fe, &ports, gcm_init_state_allowlist,
                                gcm_init_state_allowlist_count) == GCM_FE_OK);
    fp.publish_rc = 2;
    effect = RSX_FIFO_EFFECT_NONE;
    CHECK(gcm_rsx_frontend_sink(fe, 0, 0x0050u, 0xFFFFFFFFu, 0x0u, &effect) != 0);
    CHECK(effect == RSX_FIFO_EFFECT_UNKNOWN);
    CHECK(gcm_rsx_frontend_failure(fe)->status == GCM_FE_PORT_FAILED);
    CHECK_EQ_U32(gcm_rsx_frontend_failure(fe)->port_result, 2u);
    fp.publish_rc = 0;

    free(fe);
}

/* ------------------------------------------------------------------------------------ */
/* Case 9: post-init queries                                                           */
/* ------------------------------------------------------------------------------------ */

static void test_flip_mode(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);

    CHECK(gcm_init_service_set_flip_mode(svc, 2) == GCM_INIT_NOT_INITIALIZED);
    CHECK_EQ_U32(svc->flip_mode, 0u);

    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    CHECK_EQ_U32(guest.write_count, 13u);
    CHECK_EQ_U32(svc->flip_mode, 0u);

    /* 1 (HSYNC) and 2 (VSYNC) are recorded; neither writes guest memory. */
    CHECK(gcm_init_service_set_flip_mode(svc, 2) == GCM_INIT_OK);
    CHECK_EQ_U32(svc->flip_mode, 2u);
    CHECK(gcm_init_service_set_flip_mode(svc, 1) == GCM_INIT_OK);
    CHECK_EQ_U32(svc->flip_mode, 1u);
    CHECK_EQ_U32(guest.write_count, 13u);

    /* Any other mode is refused and leaves the recorded mode untouched. */
    CHECK(gcm_init_service_set_flip_mode(svc, 0) == GCM_INIT_UNSUPPORTED_FLIP_MODE);
    CHECK(gcm_init_service_set_flip_mode(svc, 3) == GCM_INIT_UNSUPPORTED_FLIP_MODE);
    CHECK_EQ_U32(svc->flip_mode, 1u);
    CHECK(gcm_init_service_set_flip_mode(NULL, 2) == GCM_INIT_INVALID_ARGUMENT);
    CHECK_EQ_U32(guest.write_count, 13u);
    CHECK(strcmp(gcm_init_status_name(GCM_INIT_UNSUPPORTED_FLIP_MODE),
                 "UNSUPPORTED_FLIP_MODE") == 0);

    free(svc);
    fake_free(&guest);
}

static void test_tile_info(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    uint32_t writes;

    CHECK_EQ_U32(guest.write_count, 0u);
    CHECK(gcm_init_service_set_tile_info(svc, 0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu,
                                         1, 1) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_bind_tile(svc, 0) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_tile(svc, 0)->set == 0);

    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    writes = guest.write_count;
    CHECK_EQ_U32(writes, 13u);
    for (uint32_t i = 0; i < GCM_INIT_TILE_COUNT; i++) {
        const gcm_init_tile *t = gcm_init_service_tile(svc, i);
        CHECK(t != NULL);
        CHECK(t->set == 0 && t->bound == 0);
    }

    /* The observed call: index 0, local, 64 KiB-aligned offset/size, pitch 0x1400,
     * comp 0xB, base 1, bank 1. Then bind it. No guest write. */
    CHECK(gcm_init_service_set_tile_info(svc, 0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu,
                                         1, 1) == GCM_INIT_OK);
    const gcm_init_tile *tile = gcm_init_service_tile(svc, 0);
    CHECK_EQ_U32(tile->location, 0u);
    CHECK_EQ_U32(tile->offset, 0x01010000u);
    CHECK_EQ_U32(tile->size, 0x003A0000u);
    CHECK_EQ_U32(tile->pitch, 0x1400u);
    CHECK_EQ_U32(tile->comp, 0xBu);
    CHECK_EQ_U32(tile->base, 1u);
    CHECK_EQ_U32(tile->bank, 1u);
    CHECK(tile->set == 1 && tile->bound == 0);
    CHECK(gcm_init_service_bind_tile(svc, 0) == GCM_INIT_OK);
    CHECK(tile->bound == 1);
    CHECK_EQ_U32(guest.write_count, writes);

    /* Highest accepted index and boundary values. */
    CHECK(gcm_init_service_set_tile_info(svc, 14, 0, 0, GCM_INIT_LOCAL_SIZE, 0x100u, 0, 0,
                                         3) == GCM_INIT_OK);
    tile = gcm_init_service_tile(svc, 14);
    CHECK(tile->set == 1 && tile->bound == 0 && tile->size == GCM_INIT_LOCAL_SIZE &&
          tile->pitch == 0x100u && tile->bank == 3);

    /* Every refusal leaves both the target tile and every other tile unchanged. */
    struct Case {
        uint32_t index, location, offset, size, pitch, bank;
    };
    const struct Case cases[] = {
        {15, 0, 0x01010000u, 0x003A0000u, 0x1400u, 1},
        {0, 1, 0x01010000u, 0x003A0000u, 0x1400u, 1},
        {0, 0, 0x01010001u, 0x003A0000u, 0x1400u, 1},
        {0, 0, 0x01010000u, 0x003A0001u, 0x1400u, 1},
        {0, 0, 0x01010000u, 0, 0x1400u, 1},
        {0, 0, 0x0F800000u, 0x00200000u, 0x1400u, 1},
        {0, 0, 0x01010000u, 0x003A0000u, 0, 1},
        {0, 0, 0x01010000u, 0x003A0000u, 0x1410u, 1},
        {0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 4},
        {0, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xFFFFFFFFu}};
    tile = gcm_init_service_tile(svc, 0);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct Case *c = &cases[i];
        CHECK(gcm_init_service_set_tile_info(svc, c->index, c->location, c->offset, c->size,
                                             c->pitch, 0xBu, 1, c->bank) ==
              GCM_INIT_UNSUPPORTED_TILE);
        CHECK(tile->location == 0 && tile->offset == 0x01010000u &&
              tile->size == 0x003A0000u && tile->pitch == 0x1400u && tile->comp == 0xBu &&
              tile->base == 1 && tile->bank == 1 && tile->set == 1 && tile->bound == 1);
        CHECK(gcm_init_service_tile(svc, 14)->set == 1);
        CHECK_EQ_U32(guest.write_count, writes);
    }

    /* Bind requires a set tile; out-of-range and unset indices are refused. */
    CHECK(gcm_init_service_bind_tile(svc, 1) == GCM_INIT_UNSUPPORTED_TILE);
    CHECK(gcm_init_service_bind_tile(svc, 15) == GCM_INIT_UNSUPPORTED_TILE);
    CHECK(gcm_init_service_tile(svc, 1)->bound == 0);

    /* Argument and range checks. */
    CHECK(gcm_init_service_set_tile_info(NULL, 0, 0, 0, 0x10000u, 0x100u, 0, 0, 0) ==
          GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_bind_tile(NULL, 0) == GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_tile(NULL, 0) == NULL);
    CHECK(gcm_init_service_tile(svc, GCM_INIT_TILE_COUNT) == NULL);
    CHECK(strcmp(gcm_init_status_name(GCM_INIT_UNSUPPORTED_TILE), "UNSUPPORTED_TILE") == 0);

    free(svc);
    fake_free(&guest);
}

static void test_zcull_bind_unbind(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    uint32_t writes;

    gcm_init_zcull observed = {0};
    observed.offset = 0x01010000u;
    observed.width = 0x500u;
    observed.height = 0x300u;
    observed.cull_start = 0;
    observed.z_format = 2;
    observed.aa_format = 0;
    observed.zcull_dir = 0;
    observed.zcull_format = 1;
    observed.s_func = 1;
    observed.s_ref = 0x80u;
    observed.s_mask = 0xFFu;

    /* Before init: nothing is stored. */
    CHECK_EQ_U32(guest.write_count, 0u);
    CHECK(gcm_init_service_bind_zcull(svc, 0, &observed) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_unbind_zcull(svc, 0) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_unbind_tile(svc, 0) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_zcull(svc, 0)->set == 0);
    CHECK(gcm_init_service_zcull(svc, 0)->bound == 0);

    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    writes = guest.write_count;
    CHECK_EQ_U32(writes, 13u);
    for (uint32_t i = 0; i < GCM_INIT_ZCULL_COUNT; i++) {
        const gcm_init_zcull *z = gcm_init_service_zcull(svc, i);
        CHECK(z != NULL);
        CHECK(z->set == 0 && z->bound == 0);
    }

    /* The observed call: index 0, offset 0x01010000, 0x500 x 0x300, cull start 0, z_format 2,
     * the four stack parameters 1/1/0x80/0xff. Bind sets both flags; no guest write. */
    CHECK(gcm_init_service_bind_zcull(svc, 0, &observed) == GCM_INIT_OK);
    const gcm_init_zcull *zcull = gcm_init_service_zcull(svc, 0);
    CHECK_EQ_U32(zcull->offset, 0x01010000u);
    CHECK_EQ_U32(zcull->width, 0x500u);
    CHECK_EQ_U32(zcull->height, 0x300u);
    CHECK_EQ_U32(zcull->cull_start, 0u);
    CHECK_EQ_U32(zcull->z_format, 2u);
    CHECK_EQ_U32(zcull->aa_format, 0u);
    CHECK_EQ_U32(zcull->zcull_dir, 0u);
    CHECK_EQ_U32(zcull->zcull_format, 1u);
    CHECK_EQ_U32(zcull->s_func, 1u);
    CHECK_EQ_U32(zcull->s_ref, 0x80u);
    CHECK_EQ_U32(zcull->s_mask, 0xFFu);
    CHECK(zcull->set == 1 && zcull->bound == 1);
    CHECK_EQ_U32(guest.write_count, writes);

    /* Highest accepted index and boundary values (64x multiple 4096, 4096-aligned cull start). */
    gcm_init_zcull highest = observed;
    highest.offset = GCM_INIT_LOCAL_SIZE - 0x10000u;
    highest.width = 4096u;
    highest.height = 4096u;
    highest.cull_start = 0x1000u;
    highest.z_format = 1u;
    highest.zcull_format = 5u;
    highest.s_func = 3u;
    highest.s_mask = 0xFFFFFFFFu;
    CHECK(gcm_init_service_bind_zcull(svc, 7, &highest) == GCM_INIT_OK);
    zcull = gcm_init_service_zcull(svc, 7);
    CHECK(zcull->set == 1 && zcull->bound == 1 && zcull->offset == GCM_INIT_LOCAL_SIZE - 0x10000u &&
          zcull->width == 4096u && zcull->height == 4096u && zcull->cull_start == 0x1000u &&
          zcull->z_format == 1u && zcull->zcull_format == 5u && zcull->s_func == 3u &&
          zcull->s_mask == 0xFFFFFFFFu);

    /* Every refusal leaves entry 0 and entry 7 unchanged. */
    struct Case {
        uint32_t index, offset, width, height, cull_start, z_format;
    };
    const struct Case cases[] = {
        {8, 0x01010000u, 0x500u, 0x300u, 0, 2},
        {0xFFFFFFFFu, 0x01010000u, 0x500u, 0x300u, 0, 2},
        {0, 0x01010001u, 0x500u, 0x300u, 0, 2},
        {0, GCM_INIT_LOCAL_SIZE, 0x500u, 0x300u, 0, 2},
        {0, 0x01010000u, 0, 0x300u, 0, 2},
        {0, 0x01010000u, 0x501u, 0x300u, 0, 2},
        {0, 0x01010000u, 0x1040u, 0x300u, 0, 2},
        {0, 0x01010000u, 0x500u, 0, 0, 2},
        {0, 0x01010000u, 0x500u, 0x301u, 0, 2},
        {0, 0x01010000u, 0x500u, 0x1040u, 0, 2},
        {0, 0x01010000u, 0x500u, 0x300u, 1, 2},
        {0, 0x01010000u, 0x500u, 0x300u, 0xFFFFF001u, 2},
        {0, 0x01010000u, 0x500u, 0x300u, 0, 0},
        {0, 0x01010000u, 0x500u, 0x300u, 0, 3}};
    zcull = gcm_init_service_zcull(svc, 0);
    const gcm_init_zcull kept = *zcull;
    const gcm_init_zcull kept7 = *gcm_init_service_zcull(svc, 7);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct Case *c = &cases[i];
        gcm_init_zcull args = observed;
        args.offset = c->offset;
        args.width = c->width;
        args.height = c->height;
        args.cull_start = c->cull_start;
        args.z_format = c->z_format;
        CHECK(gcm_init_service_bind_zcull(svc, c->index, &args) == GCM_INIT_UNSUPPORTED_ZCULL);
        CHECK(memcmp(zcull, &kept, sizeof kept) == 0);
        CHECK(memcmp(gcm_init_service_zcull(svc, 7), &kept7, sizeof kept7) == 0);
        CHECK_EQ_U32(guest.write_count, writes);
    }

    /* UnbindZcull clears set and bound of exactly that entry; out of range is refused. */
    CHECK(gcm_init_service_unbind_zcull(svc, 0) == GCM_INIT_OK);
    CHECK(zcull->set == 0 && zcull->bound == 0);
    CHECK_EQ_U32(zcull->offset, 0x01010000u);   // the record stays
    CHECK(gcm_init_service_zcull(svc, 7)->bound == 1);
    CHECK(gcm_init_service_unbind_zcull(svc, 8) == GCM_INIT_UNSUPPORTED_ZCULL);
    CHECK(gcm_init_service_unbind_zcull(svc, 0xFFFFFFFFu) == GCM_INIT_UNSUPPORTED_ZCULL);
    CHECK(zcull->set == 0 && zcull->bound == 0);

    /* UnbindTile clears bound and keeps the stored tile record; out of range is refused. */
    CHECK(gcm_init_service_set_tile_info(svc, 3, 0, 0x01010000u, 0x003A0000u, 0x1400u, 0xBu,
                                         1, 1) == GCM_INIT_OK);
    CHECK(gcm_init_service_bind_tile(svc, 3) == GCM_INIT_OK);
    CHECK(gcm_init_service_tile(svc, 3)->bound == 1);
    CHECK(gcm_init_service_unbind_tile(svc, 3) == GCM_INIT_OK);
    CHECK(gcm_init_service_tile(svc, 3)->bound == 0);
    CHECK(gcm_init_service_tile(svc, 3)->set == 1);
    CHECK_EQ_U32(gcm_init_service_tile(svc, 3)->offset, 0x01010000u);
    CHECK_EQ_U32(gcm_init_service_tile(svc, 3)->size, 0x003A0000u);
    CHECK(gcm_init_service_unbind_tile(svc, 15) == GCM_INIT_UNSUPPORTED_TILE);
    CHECK(gcm_init_service_unbind_tile(svc, 0xFFFFFFFFu) == GCM_INIT_UNSUPPORTED_TILE);
    CHECK(gcm_init_service_tile(svc, 3)->bound == 0);
    CHECK_EQ_U32(guest.write_count, writes);

    /* Argument and range checks. */
    CHECK(gcm_init_service_bind_zcull(NULL, 0, &observed) == GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_bind_zcull(svc, 0, NULL) == GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_unbind_zcull(NULL, 0) == GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_unbind_tile(NULL, 0) == GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_zcull(NULL, 0) == NULL);
    CHECK(gcm_init_service_zcull(svc, GCM_INIT_ZCULL_COUNT) == NULL);
    CHECK(strcmp(gcm_init_status_name(GCM_INIT_UNSUPPORTED_ZCULL), "UNSUPPORTED_ZCULL") == 0);

    free(svc);
    fake_free(&guest);
}

static void test_display_buffer(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    uint32_t writes;

    /* Before init: nothing is stored. */
    CHECK_EQ_U32(guest.write_count, 0u);
    CHECK(gcm_init_service_set_display_buffer(svc, 0, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u) ==
          GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_display_buffer(svc, 0)->set == 0);

    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    writes = guest.write_count;
    CHECK_EQ_U32(writes, 13u);
    for (uint32_t i = 0; i < GCM_INIT_DISPLAY_BUFFER_COUNT; i++) {
        const gcm_init_display_buffer *b = gcm_init_service_display_buffer(svc, i);
        CHECK(b != NULL);
        CHECK(b->set == 0);
    }

    /* The two observed calls: ids 0 and 1, pitch 0x1400, 1280x720. No guest write. */
    CHECK(gcm_init_service_set_display_buffer(svc, 0, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u) ==
          GCM_INIT_OK);
    const gcm_init_display_buffer *buffer = gcm_init_service_display_buffer(svc, 0);
    CHECK_EQ_U32(buffer->offset, 0x013B0000u);
    CHECK_EQ_U32(buffer->pitch, 0x1400u);
    CHECK_EQ_U32(buffer->width, 0x500u);
    CHECK_EQ_U32(buffer->height, 0x2D0u);
    CHECK(buffer->set == 1);
    CHECK(gcm_init_service_set_display_buffer(svc, 1, 0x01734000u, 0x1400u, 0x500u, 0x2D0u) ==
          GCM_INIT_OK);
    const gcm_init_display_buffer *second = gcm_init_service_display_buffer(svc, 1);
    CHECK_EQ_U32(second->offset, 0x01734000u);
    CHECK_EQ_U32(second->pitch, 0x1400u);
    CHECK_EQ_U32(second->width, 0x500u);
    CHECK_EQ_U32(second->height, 0x2D0u);
    CHECK(second->set == 1);
    CHECK_EQ_U32(guest.write_count, writes);

    /* Highest accepted id and the boundary values of every rule. */
    CHECK(gcm_init_service_set_display_buffer(svc, 7, 0x0F800000u, 0x100u, 4096u, 4096u) ==
          GCM_INIT_OK);
    const gcm_init_display_buffer *highest = gcm_init_service_display_buffer(svc, 7);
    CHECK(highest->set == 1 && highest->offset == 0x0F800000u && highest->pitch == 0x100u &&
          highest->width == 4096u && highest->height == 4096u);

    /* Every refusal leaves entries 0, 1 and 7 unchanged. */
    struct Case {
        uint32_t id, offset, pitch, width, height;
    };
    const struct Case cases[] = {
        {8, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u},
        {0xFFFFFFFFu, 0x013B0000u, 0x1400u, 0x500u, 0x2D0u},
        {0, 0x013B0000u, 0x1400u, 0, 0x2D0u},
        {0, 0x013B0000u, 0x1400u, 4097u, 0x2D0u},
        {0, 0x013B0000u, 0x1400u, 0x500u, 0},
        {0, 0x013B0000u, 0x1400u, 0x500u, 4097u},
        {0, 0x013B0000u, 0, 0x500u, 0x2D0u},
        {0, 0x013B0000u, 0x1410u, 0x500u, 0x2D0u},
        {0, 0x013B0001u, 0x1400u, 0x500u, 0x2D0u},
        {0, 0x0F800100u, 0x100u, 0x500u, 4096u},
        {7, 0x0F900000u, 0x100u, 0x500u, 0x2D0u},
        {7, 0x0F800000u, 0x200u, 0x500u, 4096u}};
    buffer = gcm_init_service_display_buffer(svc, 0);
    const gcm_init_display_buffer kept0 = *buffer;
    const gcm_init_display_buffer kept1 = *gcm_init_service_display_buffer(svc, 1);
    const gcm_init_display_buffer kept7 = *gcm_init_service_display_buffer(svc, 7);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct Case *c = &cases[i];
        CHECK(gcm_init_service_set_display_buffer(svc, c->id, c->offset, c->pitch, c->width,
                                                  c->height) ==
              GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER);
        CHECK(memcmp(gcm_init_service_display_buffer(svc, 0), &kept0, sizeof kept0) == 0);
        CHECK(memcmp(gcm_init_service_display_buffer(svc, 1), &kept1, sizeof kept1) == 0);
        CHECK(memcmp(gcm_init_service_display_buffer(svc, 7), &kept7, sizeof kept7) == 0);
        CHECK_EQ_U32(guest.write_count, writes);
    }

    /* Argument and range checks. */
    CHECK(gcm_init_service_set_display_buffer(NULL, 0, 0, 0x40u, 1, 1) ==
          GCM_INIT_INVALID_ARGUMENT);
    CHECK(gcm_init_service_display_buffer(NULL, 0) == NULL);
    CHECK(gcm_init_service_display_buffer(svc, GCM_INIT_DISPLAY_BUFFER_COUNT) == NULL);
    CHECK(strcmp(gcm_init_status_name(GCM_INIT_UNSUPPORTED_DISPLAY_BUFFER),
                 "UNSUPPORTED_DISPLAY_BUFFER") == 0);

    free(svc);
    fake_free(&guest);
}

static void test_queries_before_init(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_config config;
    uint32_t ea = 0, ea2 = 0;

    CHECK(gcm_init_service_configuration(svc, &config) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_label_address(svc, 0, &ea) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_control_register(svc, &ea) == GCM_INIT_NOT_INITIALIZED);
    CHECK(gcm_init_service_offset_tables(svc, &ea, &ea2) == GCM_INIT_NOT_INITIALIZED);
    CHECK_EQ_U32(guest.write_count, 0u);
    CHECK_EQ_U32(svc->tables_materialized, 0);

    free(svc);
    fake_free(&guest);
}

static void test_queries_after_init(void)
{
    static const uint8_t expected[24] = {
        0xc0, 0x00, 0x00, 0x00, 0x40, 0x70, 0x00, 0x00,
        0x0f, 0x90, 0x00, 0x00, 0x08, 0x50, 0x00, 0x00,
        0x26, 0xbe, 0x36, 0x80, 0x1d, 0xcd, 0x65, 0x00,
    };
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);

    gcm_init_config config;
    memset(&config, 0, sizeof config);
    CHECK(gcm_init_service_configuration(svc, &config) == GCM_INIT_OK);
    CHECK_EQ_U32(config.local_address, GCM_INIT_LOCAL_BASE);
    CHECK_EQ_U32(config.io_address, IO_EA);
    CHECK_EQ_U32(config.local_size, GCM_INIT_LOCAL_SIZE);
    CHECK_EQ_U32(config.io_size, IO_SIZE);
    CHECK_EQ_U32(config.memory_frequency, 0x26BE3680u);
    CHECK_EQ_U32(config.core_frequency, 0x1DCD6500u);
    uint8_t encoded[24];
    memset(encoded, 0xA5, sizeof encoded);
    gcm_init_config_encode(&config, encoded);
    CHECK(memcmp(encoded, expected, sizeof expected) == 0);

    static const uint32_t indices[5] = {0u, 1u, 255u, 256u, 257u};
    static const uint32_t offsets[5] = {0x0u, 0x10u, 0xFF0u, 0x0u, 0x10u};
    for (int i = 0; i < 5; i++) {
        uint32_t ea = 0;
        CHECK(gcm_init_service_label_address(svc, indices[i], &ea) == GCM_INIT_OK);
        CHECK_EQ_U32(ea, REPORTS_EA + offsets[i]);
    }

    uint32_t control = 0;
    CHECK(gcm_init_service_control_register(svc, &control) == GCM_INIT_OK);
    CHECK_EQ_U32(control, CONTROL_EA);

    /* Queries write nothing. */
    CHECK_EQ_U32(guest.write_count, 13u);

    free(svc);
    fake_free(&guest);
}

static void test_offset_tables(void)
{
    fake_guest guest;
    fake_init(&guest);
    gcm_init_service *svc = service_new(&guest);
    gcm_init_request req = exact_request();
    gcm_init_result result;
    CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
    CHECK_EQ_U32(guest.write_count, 13u);

    uint32_t ea_table = 0, io_table = 0;
    CHECK(gcm_init_service_offset_tables(svc, &ea_table, &io_table) == GCM_INIT_OK);
    CHECK_EQ_U32(ea_table, OFFSET_TABLE_EA);
    CHECK_EQ_U32(io_table, IO_TABLE_EA);
    CHECK_EQ_U32(svc->tables_materialized, 1);
    CHECK_EQ_U32(guest.write_count, 15u);
    CHECK_EQ_U32(guest.write_ea[13], OFFSET_TABLE_EA);
    CHECK_EQ_U32(guest.write_len[13], GCM_INIT_EA_TABLE_BYTES);
    CHECK_EQ_U32(guest.write_ea[14], IO_TABLE_EA);
    CHECK_EQ_U32(guest.write_len[14], GCM_INIT_IO_TABLE_BYTES);

    fake_region *r = fake_find(&guest, OFFSET_TABLE_EA, GCM_INIT_EA_TABLE_BYTES);
    CHECK(r != NULL);
    if (r) CHECK(memcmp(r->bytes, gcm_offset_table_ea_to_io_expected,
                        GCM_INIT_EA_TABLE_BYTES) == 0);
    r = fake_find(&guest, IO_TABLE_EA, GCM_INIT_IO_TABLE_BYTES);
    CHECK(r != NULL);
    if (r) CHECK(memcmp(r->bytes, gcm_offset_table_io_to_ea_expected,
                        GCM_INIT_IO_TABLE_BYTES) == 0);

    /* Later calls only return the EAs. */
    uint32_t a = 0, b = 0;
    CHECK(gcm_init_service_offset_tables(svc, &a, &b) == GCM_INIT_OK);
    CHECK_EQ_U32(a, OFFSET_TABLE_EA);
    CHECK_EQ_U32(b, IO_TABLE_EA);
    CHECK_EQ_U32(guest.write_count, 15u);

    free(svc);
    fake_free(&guest);
}

static void test_offset_tables_retry(void)
{
    for (int fail = 0; fail < 2; fail++) {
        fake_guest guest;
        fake_init(&guest);
        gcm_init_service *svc = service_new(&guest);
        gcm_init_request req = exact_request();
        gcm_init_result result;
        CHECK(gcm_init_service_run(svc, &req, &result) == GCM_INIT_OK);
        CHECK_EQ_U32(guest.write_count, 13u);

        guest.fail_write_index = 13 + fail;
        uint32_t ea_table = 0, io_table = 0;
        CHECK(gcm_init_service_offset_tables(svc, &ea_table, &io_table) ==
              GCM_INIT_GUEST_WRITE_FAILED);
        CHECK_EQ_U32(svc->tables_materialized, 0);
        CHECK_EQ_U32(guest.write_count, 14u + (uint32_t)fail);

        guest.fail_write_index = -1;
        uint32_t before = guest.write_count;
        CHECK(gcm_init_service_offset_tables(svc, &ea_table, &io_table) == GCM_INIT_OK);
        CHECK_EQ_U32(svc->tables_materialized, 1);
        CHECK_EQ_U32(guest.write_count, before + 2u);
        CHECK_EQ_U32(ea_table, OFFSET_TABLE_EA);
        CHECK_EQ_U32(io_table, IO_TABLE_EA);
        fake_region *r = fake_find(&guest, OFFSET_TABLE_EA, GCM_INIT_EA_TABLE_BYTES);
        CHECK(r != NULL);
        if (r) CHECK(memcmp(r->bytes, gcm_offset_table_ea_to_io_expected,
                            GCM_INIT_EA_TABLE_BYTES) == 0);
        r = fake_find(&guest, IO_TABLE_EA, GCM_INIT_IO_TABLE_BYTES);
        CHECK(r != NULL);
        if (r) CHECK(memcmp(r->bytes, gcm_offset_table_io_to_ea_expected,
                            GCM_INIT_IO_TABLE_BYTES) == 0);

        free(svc);
        fake_free(&guest);
    }
}

/* ------------------------------------------------------------------------------------ */

int main(void)
{
    test_generator_walker_equivalence();
    test_full_run();
    test_request_validation();
    test_layout_validation();
    test_flip_semaphore_zero();
    test_tampered_images();
    test_write_failures();
    test_frontend_units();
    test_queries_before_init();
    test_queries_after_init();
    test_flip_mode();
    test_tile_info();
    test_zcull_bind_unbind();
    test_display_buffer();
    test_offset_tables();
    test_offset_tables_retry();

    if (failures) {
        fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
