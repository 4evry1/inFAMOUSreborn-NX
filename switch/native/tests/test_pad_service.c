/* Host tests for the platform-free pad service (native/domain/pad_service.c).
 *
 * The controller port is a fake kept entirely in memory: a pad count, a per-port capability
 * table, and a per-operation failure switch so every port path can be driven directly. No
 * filesystem, no globals, no OS calls.
 *
 * Every case that checks a guest image compares the full big-endian buffer, so a wrong
 * offset or a wrong byte order fails byte-for-byte. Expected values come from
 * (section cited per case).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad_service.h"

static int checks;
static int failures;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                            \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                               \
    do {                                                                             \
        long long a_ = (long long)(actual);                                          \
        long long e_ = (long long)(expected);                                        \
        checks++;                                                                    \
        if (a_ != e_) {                                                              \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__,        \
                    __LINE__, #actual, a_, e_);                                      \
        }                                                                            \
    } while (0)

#define CHECK_EQ_MEM(actual, expected, len)                                           \
    do {                                                                             \
        const size_t n_ = (size_t)(len);                                              \
        const uint8_t *a_ = (const uint8_t *)(actual);                                \
        const uint8_t *e_ = (const uint8_t *)(expected);                              \
        checks++;                                                                     \
        if (memcmp(a_, e_, n_) != 0) {                                                \
            size_t i_ = 0;                                                            \
            while (i_ < n_ && a_[i_] == e_[i_]) {                                      \
                i_++;                                                                 \
            }                                                                         \
            failures++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s differs at 0x%zX (got 0x%02X want 0x%02X)\n", \
                    __FILE__, __LINE__, #actual, i_, i_ < n_ ? a_[i_] : 0u,            \
                    i_ < n_ ? e_[i_] : 0u);                                          \
        }                                                                             \
    } while (0)

/* ------------------------------------------------------------------------------------ */
/* Fake controller port                                                                  */
/* ------------------------------------------------------------------------------------ */

typedef struct fake_port {
    uint32_t pad_count;
    int count_status;          /* pad_port_status returned by pad_count */
    int count_calls;
    int capability_status;     /* pad_port_status returned by capability */
    int capability_calls;
    pad_capability caps[PAD_PORT_MAX + 1u];
    int state_status;          /* pad_port_status returned by read_state */
    int state_calls;
    pad_host_state states[PAD_INFO_PORT_COUNT];
} fake_port;

static pad_port_status fake_count(void *context, uint32_t *out_count)
{
    fake_port *const fake = (fake_port *)context;
    fake->count_calls++;
    if (fake->count_status != PAD_PORT_OK) {
        return (pad_port_status)fake->count_status;
    }
    *out_count = fake->pad_count;
    return PAD_PORT_OK;
}

static pad_port_status fake_capability(void *context, uint32_t port_no, pad_capability *out)
{
    fake_port *const fake = (fake_port *)context;
    fake->capability_calls++;
    if (fake->capability_status != PAD_PORT_OK) {
        return (pad_port_status)fake->capability_status;
    }
    if (port_no > PAD_PORT_MAX) {
        return PAD_PORT_UNAVAILABLE;
    }
    *out = fake->caps[port_no];
    return PAD_PORT_OK;
}

static pad_port_status fake_read_state(void *context, uint32_t port_no, pad_host_state *out)
{
    fake_port *const fake = (fake_port *)context;
    fake->state_calls++;
    if (fake->state_status != PAD_PORT_OK) {
        return (pad_port_status)fake->state_status;
    }
    if (port_no >= PAD_INFO_PORT_COUNT) {
        return PAD_PORT_UNAVAILABLE;
    }
    *out = fake->states[port_no];
    return PAD_PORT_OK;
}

static void fake_init(fake_port *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->pad_count = 1u;
    fake->count_status = PAD_PORT_OK;
    fake->capability_status = PAD_PORT_OK;
    fake->state_status = PAD_PORT_OK;
    /* The service's documented default: a single connected standard pad on port 0. */
    fake->caps[0].exists = 1u;
    fake->caps[0].supports_press_mode = 1u;
    fake->caps[0].supports_sensor_mode = 0u;
}

static pad_port fake_as_port(fake_port *fake)
{
    pad_port port;
    port.context = fake;
    port.pad_count = fake_count;
    port.capability = fake_capability;
    port.read_state = fake_read_state;
    return port;
}

/* ------------------------------------------------------------------------------------ */
/* Expected state image                                                                  */
/* ------------------------------------------------------------------------------------ */

/* A 30496-byte image with an explicit (offset, big-endian value) patch list, so a case can
 * state exactly which words the contract fixes and leave everything else zero. */
static uint8_t *expected_state(const uint32_t *patches, size_t count)
{
    uint8_t *const image = (uint8_t *)calloc(PAD_STATE_BYTES, 1);
    size_t i;

    if (image == NULL) {
        fprintf(stderr, "calloc failed\n");
        exit(2);
    }
    for (i = 0; i < count; i++) {
        const uint32_t offset = patches[i * 2u];
        const uint32_t value = patches[i * 2u + 1u];
        image[offset + 0u] = (uint8_t)(value >> 24);
        image[offset + 1u] = (uint8_t)(value >> 16);
        image[offset + 2u] = (uint8_t)(value >> 8);
        image[offset + 3u] = (uint8_t)value;
    }
    return image;
}

/* ------------------------------------------------------------------------------------ */
/* §2.1 the argument predicate                                                           */
/* ------------------------------------------------------------------------------------ */

static void test_init_argument_valid(void)
{
    /* 0x5130..0x5154: [1, 0x7F] passes. */
    CHECK_EQ_INT(pad_init_argument_valid(1u), 1);
    CHECK_EQ_INT(pad_init_argument_valid(2u), 1);
    CHECK_EQ_INT(pad_init_argument_valid(0x7Eu), 1);
    CHECK_EQ_INT(pad_init_argument_valid(0x7Fu), 1); /* the inclusive upper bound */
    /* 0 and >= 0x80 fail. */
    CHECK_EQ_INT(pad_init_argument_valid(0u), 0);
    CHECK_EQ_INT(pad_init_argument_valid(0x80u), 0);
    CHECK_EQ_INT(pad_init_argument_valid(0xFFFFFFFFu), 0);
}

/* ------------------------------------------------------------------------------------ */
/* §2.3/§2.4 cellPadInit                                                                 */
/* ------------------------------------------------------------------------------------ */

static void test_init_guard(void)
{
    pad_state state;

    memset(&state, 0, sizeof(state));
    /* Flag clear: the firmware proceeds (0x24A0 not taken). */
    CHECK_EQ_INT(pad_init_guard(&state), PAD_OK);
    CHECK_EQ_INT(pad_init_guard(NULL), PAD_OK);

    /* Flag set: 0x2484/0x249C left 0x80121103 in r31 and 0x24A0 branches to the exit. */
    state.initialized = 1u;
    CHECK_EQ_INT(pad_init_guard(&state), PAD_ERROR_ALREADY_INITIALIZED);
    CHECK_EQ_INT(PAD_ERROR_ALREADY_INITIALIZED, (int32_t)0x80121103u);
    /* `extsw` makes it negative, which is what the caller's compare sees. */
    CHECK(pad_init_guard(&state) < 0);
}

static void test_init_success_image(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t image[PAD_STATE_BYTES];
    uint8_t *expected;
    int32_t result = 12345;
    pad_status status;
    const uint32_t patches[] = {
        PAD_STATE_MAX_PADS, 2u,
        PAD_STATE_ATTACHED, 1u,     /* host reported one pad */
        PAD_STATE_LIVE, 1u,
    };

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));

    status = pad_init(&state, &port, 2u, 0u, image, sizeof(image), &result);

    /* The game passes r3 = 2 and this is the only path that continues (§4.1). */
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(state.initialized, 1u);
    CHECK_EQ_INT(state.max_pads, 2u);
    CHECK_EQ_INT(state.attached_pads, 1u);
    CHECK_EQ_INT(state.sc512_flag, 0u);
    CHECK_EQ_INT(fake.count_calls, 1);

    /* The exact big-endian image: only the three named words are non-zero. */
    expected = expected_state(patches, 3u);
    CHECK_EQ_MEM(image, expected, PAD_STATE_BYTES);
    free(expected);

    /* A second init is the already-initialized short circuit and leaves the image alone. */
    memset(image, 0xAA, sizeof(image));
    status = pad_init(&state, &port, 2u, 0u, image, sizeof(image), &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_ALREADY_INITIALIZED);
    CHECK_EQ_INT(image[0], 0xAA); /* untouched */
    CHECK_EQ_INT(fake.count_calls, 1); /* the port was not consulted again */
}

static void test_init_bad_argument(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t image[PAD_STATE_BYTES];
    int32_t result = 0;
    pad_status status;

    fake_init(&fake);
    port = fake_as_port(&fake);

    /* count = 0 */
    memset(&state, 0, sizeof(state));
    memset(image, 0x5A, sizeof(image));
    status = pad_init(&state, &port, 0u, 0u, image, sizeof(image), &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(PAD_ERROR_INVALID_ARGUMENT, (int32_t)0x80121102u);
    CHECK_EQ_INT(state.initialized, 0u);
    CHECK_EQ_INT(image[0], 0x5A); /* no image written */
    CHECK_EQ_INT(fake.count_calls, 0);

    /* count = 0x80 */
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, &port, 0x80u, 0u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(state.initialized, 0u);
    CHECK_EQ_INT(fake.count_calls, 0);

    /* count = 0x7F passes the range check. */
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, &port, 0x7Fu, 0u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(state.max_pads, 0x7Fu);
}

static void test_init_port_paths(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    int32_t result = 0;
    pad_status status;

    /* No port at all: the assumed count decides attached_pads, the call still succeeds. */
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, NULL, 2u, 1u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(state.attached_pads, 1u);

    /* A port with no pad_count callback behaves like no port. */
    memset(&state, 0, sizeof(state));
    {
        pad_port bare;
        bare.context = NULL;
        bare.pad_count = NULL;
        bare.capability = NULL;
        status = pad_init(&state, &bare, 2u, 4u, NULL, 0, &result);
    }
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(state.attached_pads, 4u);

    /* UNAVAILABLE from the port falls back to the assumed count. */
    fake_init(&fake);
    fake.count_status = PAD_PORT_UNAVAILABLE;
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, &port, 2u, 3u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(state.attached_pads, 3u);

    /* A hard port failure is the allocation-failure arm: 0x80121105, no init. */
    fake_init(&fake);
    fake.count_status = PAD_PORT_ERROR;
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, &port, 2u, 1u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_PORT_ERROR);
    CHECK_EQ_INT(result, PAD_ERROR_NO_MEMORY);
    CHECK_EQ_INT(PAD_ERROR_NO_MEMORY, (int32_t)0x80121105u);
    CHECK_EQ_INT(state.initialized, 0u);

    /* A host count above the table size is clamped, not an error. */
    fake_init(&fake);
    fake.pad_count = 4096u;
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    status = pad_init(&state, &port, 0x7Fu, 0u, NULL, 0, &result);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(state.attached_pads, PAD_MAX_PADS_ARG);

    /* NULL state is a caller error, not a guest code. */
    CHECK_EQ_INT(pad_init(NULL, &port, 2u, 1u, NULL, 0, &result),
                 PAD_SERVICE_BAD_ARGUMENT);
}

/* ------------------------------------------------------------------------------------ */
/* §2.6 the state image                                                                  */
/* ------------------------------------------------------------------------------------ */

static void test_state_image(void)
{
    pad_state state;
    uint8_t image[PAD_STATE_BYTES];
    uint8_t *expected;
    size_t written;
    const uint32_t patches[] = {
        PAD_STATE_ATTACHED, 1u,
        PAD_STATE_LIVE, 1u,
        PAD_STATE_MAX_PADS, 2u,
    };

    /* An all-zero state is an all-zero image. */
    memset(&state, 0, sizeof(state));
    written = pad_state_image(&state, image, sizeof(image));
    CHECK_EQ_INT(written, PAD_STATE_BYTES);
    expected = expected_state(patches, 0u);
    CHECK_EQ_MEM(image, expected, PAD_STATE_BYTES);
    free(expected);

    /* The named fields land at their contract offsets, and the sc512 flag is a byte. */
    state.initialized = 1u;
    state.max_pads = 2u;
    state.attached_pads = 1u;
    state.sc512_flag = 1u;
    written = pad_state_image(&state, image, sizeof(image));
    CHECK_EQ_INT(written, PAD_STATE_BYTES);
    expected = expected_state(patches, 3u);
    expected[PAD_STATE_SC512_FLAG] = 1u;
    CHECK_EQ_MEM(image, expected, PAD_STATE_BYTES);
    free(expected);

    /* Offsets are the contract's, not adjacent guesses. */
    CHECK_EQ_INT(PAD_STATE_MAX_PADS, 0x76F4);
    CHECK_EQ_INT(PAD_STATE_ZERO_A, 0x76F8);
    CHECK_EQ_INT(PAD_STATE_ZERO_B, 0x7700);
    CHECK_EQ_INT(PAD_STATE_SC512_FLAG, 0x7704);
    CHECK_EQ_INT(PAD_STATE_LOCK, 0x7708);
    CHECK_EQ_INT(PAD_STATE_ATTACHED, 0x0A70);
    CHECK_EQ_INT(PAD_STATE_LIVE, 0x0A74);
    CHECK_EQ_INT(PAD_RECORD_BASE, 0x0104);
    CHECK_EQ_INT(PAD_RECORD_STRIDE, 232);
    CHECK_EQ_INT(PAD_RECORD_COUNT, 127);
    CHECK_EQ_INT(PAD_STATE_BYTES, 30496);
    /* 127 records of 232 bytes fit before the tail array at +0x741C. */
    CHECK(PAD_RECORD_BASE + PAD_RECORD_STRIDE * PAD_RECORD_COUNT <= 0x741Cu);

    /* A short buffer is filled as far as it goes and nothing past it is touched. */
    memset(image, 0xEE, sizeof(image));
    written = pad_state_image(&state, image, 32u);
    CHECK_EQ_INT(written, 32u);
    CHECK_EQ_INT(image[32], 0xEE);

    /* Guard against a NULL/short argument eating memory. */
    CHECK_EQ_INT(pad_state_image(NULL, image, sizeof(image)), 0u);
    CHECK_EQ_INT(pad_state_image(&state, NULL, sizeof(image)), 0u);
    CHECK_EQ_INT(pad_state_image(&state, image, 0u), 0u);
}

/* ------------------------------------------------------------------------------------ */
/* §2.7 cellPadEnd                                                                       */
/* ------------------------------------------------------------------------------------ */

static void test_end(void)
{
    pad_state state;

    memset(&state, 0, sizeof(state));
    state.initialized = 1u;
    state.max_pads = 2u;
    state.attached_pads = 1u;

    /* 0x2418..0x2424 clear the flag and the handle, then r3 = 0. */
    CHECK_EQ_INT(pad_end(&state), 0);
    CHECK_EQ_INT(state.initialized, 0u);
    CHECK_EQ_INT(state.max_pads, 0u);
    CHECK_EQ_INT(state.attached_pads, 0u);
    CHECK_EQ_INT(pad_end(NULL), 0);

    /* After end the guard lets an init through again. */
    CHECK_EQ_INT(pad_init_guard(&state), PAD_OK);
}

/* ------------------------------------------------------------------------------------ */
/* §3.1..§3.4 cellPadSetPortSetting                                                      */
/* ------------------------------------------------------------------------------------ */

static void test_set_port_setting_uninitialized(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    int32_t result = 0;
    pad_setting_result detail;
    uint8_t record[12];
    pad_status status;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));

    memset(record, 0x77, sizeof(record));
    status = pad_set_port_setting(&state, &port, 0u, 6u, &result, record, sizeof(record),
                                  &detail);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    /* 0x1918 -> 0x1920: r11 = 0x80121104. */
    CHECK_EQ_INT(result, PAD_ERROR_UNINITIALIZED);
    CHECK_EQ_INT(PAD_ERROR_UNINITIALIZED, (int32_t)0x80121104u);
    CHECK_EQ_INT(detail.applied, 0u);
    CHECK_EQ_INT(record[4], 0x77); /* nothing written */
    /* The firmware returns before the handle is even read, so the port is not consulted. */
    CHECK_EQ_INT(fake.capability_calls, 0);
}

static void test_set_port_setting_bad_port(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t image[PAD_STATE_BYTES];
    int32_t result = 0;
    pad_setting_result detail;
    pad_status status;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, image, sizeof(image), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);

    /* port > 0x7E -> returns 0x80121102. */
    status = pad_set_port_setting(&state, &port, PAD_PORT_MAX + 1u, 6u, &result, NULL, 0,
                                  &detail);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(detail.applied, 0u);
    CHECK_EQ_INT(fake.capability_calls, 0);

    /* The boundary itself passes: 0x7E is not rejected. */
    status = pad_set_port_setting(&state, &port, PAD_PORT_MAX, 6u, &result, NULL, 0, &detail);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.applied, 1u);
    /* 0x0104 + 232*0x7E = 0x0104 + 0x7250 */
    CHECK_EQ_INT(detail.record_offset, 0x0104u + 232u * 0x7Eu);
}

static void test_set_port_setting_writes(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    int32_t result = 0;
    pad_setting_result detail;
    uint8_t record[12];
    pad_status status;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, NULL, 0, &result), PAD_SERVICE_OK);

    /* The game's exact call: port 0, setting 6 = 0b110 -> bit1 and bit2 both set. */
    status = pad_set_port_setting(&state, &port, 0u, 6u, &result, record, sizeof(record),
                                  &detail);

    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.applied, 1u);
    CHECK_EQ_INT(detail.field_a, 1u); /* +0x1C  (0x567C with r4 = 1) */
    CHECK_EQ_INT(detail.field_b, 1u); /* +0x20  (0x56F8 with r4 = 1) */
    CHECK_EQ_INT(detail.record_offset, PAD_RECORD_BASE);
    /* The record image: +0x18 gate at offset 0 stays 0, +0x1C at 4, +0x20 at 8, be32. */
    {
        const uint8_t expected[12] = {0, 0, 0, 0,  0x00, 0x00, 0x00, 0x01,
                                      0x00, 0x00, 0x00, 0x01};
        CHECK_EQ_MEM(record, expected, sizeof(expected));
    }
    /* The state image carries the same two words at the record offsets. */
    {
        uint8_t image[PAD_STATE_BYTES];
        (void)pad_state_image(&state, image, sizeof(image));
        CHECK_EQ_INT(image[PAD_RECORD_BASE + PAD_RECORD_FIELD_A + 3u], 0x01);
        CHECK_EQ_INT(image[PAD_RECORD_BASE + PAD_RECORD_FIELD_B + 3u], 0x01);
        CHECK_EQ_INT(image[PAD_RECORD_BASE + PAD_RECORD_FIELD_A + 0u], 0x00);
    }
    /* The gate is closed on a host with no opened record, so no syscall is replayed. */
    CHECK_EQ_INT(detail.syscall_needed, 0u);
    CHECK_EQ_INT(detail.syscall_number, 0u);

    /* setting 0: both bits clear -> both fields 0, still applied and still 0. */
    status = pad_set_port_setting(&state, &port, 0u, 0u, &result, record, sizeof(record),
                                  &detail);
    CHECK_EQ_INT(status, PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.field_a, 0u);
    CHECK_EQ_INT(detail.field_b, 0u);

    /* setting 2 (only bit 1) -> A = 1, B = 0. */
    status = pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT1, &result, record,
                                  sizeof(record), &detail);
    CHECK_EQ_INT(detail.field_a, 1u);
    CHECK_EQ_INT(detail.field_b, 0u);

    /* setting 4 (only bit 2) -> A = 0, B = 1. */
    status = pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT2, &result, record,
                                  sizeof(record), &detail);
    CHECK_EQ_INT(detail.field_a, 0u);
    CHECK_EQ_INT(detail.field_b, 1u);

    /* setting 1 (bit 0 only) -> the firmware reads neither bit, so both stay 0. */
    status = pad_set_port_setting(&state, &port, 0u, 1u, &result, record, sizeof(record),
                                  &detail);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.field_a, 0u);
    CHECK_EQ_INT(detail.field_b, 0u);

    /* Every other bit is ignored: 0xFFFFFFF9 has bit1 and bit2 clear. */
    status = pad_set_port_setting(&state, &port, 0u, 0xFFFFFFF9u, &result, NULL, 0, &detail);
    CHECK_EQ_INT(detail.field_a, 0u);
    CHECK_EQ_INT(detail.field_b, 0u);

    /* Port 1, the game's second call: record offset moves by one stride. */
    status = pad_set_port_setting(&state, &port, 1u, 6u, &result, record, sizeof(record),
                                  &detail);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.record_offset, PAD_RECORD_BASE + PAD_RECORD_STRIDE);
    CHECK_EQ_INT(detail.field_a, 1u);
    CHECK_EQ_INT(detail.field_b, 1u);

    /* NULL state is a caller error. */
    CHECK_EQ_INT(pad_set_port_setting(NULL, &port, 0u, 6u, &result, NULL, 0, NULL),
                 PAD_SERVICE_BAD_ARGUMENT);
}

static void test_set_port_setting_port_paths(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    int32_t result = 0;
    pad_setting_result detail;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, NULL, 0, &result), PAD_SERVICE_OK);

    /* A silent port (UNAVAILABLE) changes nothing: the firmware reads no capability. */
    fake.capability_status = PAD_PORT_UNAVAILABLE;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.applied, 1u);

    /* A hard port error is reported to the caller but the guest code is unchanged only
     * through this service's contract; the firmware's own 0x33F8 cannot fail this way. */
    fake.capability_status = PAD_PORT_ERROR;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_PORT_ERROR);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(detail.applied, 0u);

    /* No port at all is fine. */
    CHECK_EQ_INT(pad_set_port_setting(&state, NULL, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(detail.applied, 1u);
}

/* ------------------------------------------------------------------------------------ */
/* §3.3 the record gate and the syscall-503 condition                                    */
/* ------------------------------------------------------------------------------------ */

static void test_record_gate(void)
{
    /* 0x56C8: bit 1 gates sub-op 3. 0x5744: bit 2 gates sub-op 5. */
    CHECK_EQ_INT(pad_record_gate_open(0x00u, PAD_SYSCALL_SUBOP_A), 0);
    CHECK_EQ_INT(pad_record_gate_open(0x02u, PAD_SYSCALL_SUBOP_A), 1);
    CHECK_EQ_INT(pad_record_gate_open(0x04u, PAD_SYSCALL_SUBOP_A), 0);
    CHECK_EQ_INT(pad_record_gate_open(0x06u, PAD_SYSCALL_SUBOP_A), 1);

    CHECK_EQ_INT(pad_record_gate_open(0x00u, PAD_SYSCALL_SUBOP_B), 0);
    CHECK_EQ_INT(pad_record_gate_open(0x04u, PAD_SYSCALL_SUBOP_B), 1);
    CHECK_EQ_INT(pad_record_gate_open(0x02u, PAD_SYSCALL_SUBOP_B), 0);
    CHECK_EQ_INT(pad_record_gate_open(0x06u, PAD_SYSCALL_SUBOP_B), 1);

    /* An unknown sub-op never opens. */
    CHECK_EQ_INT(pad_record_gate_open(0xFFu, 99u), 0);

    CHECK_EQ_INT(PAD_SYSCALL_RECORD_WRITE, 503u);
    CHECK_EQ_INT(PAD_SYSCALL_SUBOP_A, 3u);
    CHECK_EQ_INT(PAD_SYSCALL_SUBOP_B, 5u);
}

/* The gate is armed by the record-open path (0xB8C4), not by the setter; when it is set the
 * setter must report the syscall the firmware would issue. */
static void test_set_port_setting_gate(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    int32_t result = 0;
    pad_setting_result detail;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, NULL, 0, &result), PAD_SERVICE_OK);

    /* Arm bit 1 in the record's flags byte: the firmware would issue sc 503 sub-op 3. */
    state.records[PAD_RECORD_FLAGS] = 0x02u;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(detail.syscall_needed, 1u);
    CHECK_EQ_INT(detail.syscall_number, PAD_SYSCALL_RECORD_WRITE);
    CHECK_EQ_INT(detail.syscall_subop, PAD_SYSCALL_SUBOP_A);

    /* Only bit 2 armed: sub-op 5. */
    state.records[PAD_RECORD_FLAGS] = 0x04u;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(detail.syscall_needed, 1u);
    CHECK_EQ_INT(detail.syscall_subop, PAD_SYSCALL_SUBOP_B);

    /* Everything but bits 1/2 armed: still closed. */
    state.records[PAD_RECORD_FLAGS] = 0xF9u;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, &detail),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(detail.syscall_needed, 0u);
}

/* ------------------------------------------------------------------------------------ */
/* The game's exact sequence (§4.1/§4.2)                                                 */
/* ------------------------------------------------------------------------------------ */

static void test_game_sequence(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t image[PAD_STATE_BYTES];
    int32_t result = 0;
    uint32_t port_no;

    fake_init(&fake); /* one connected standard pad on port 0 */
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));

    /* func_00154194: cellPadInit(2). */
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, image, sizeof(image), &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK); /* otherwise the game returns silently (§4.1) */

    /* The `(cr >> 0) & 2` loop: ports 0 and 1, setting 6, return ignored. */
    for (port_no = 0u; port_no < 2u; port_no++) {
        pad_setting_result detail;
        CHECK_EQ_INT(pad_set_port_setting(&state, &port, port_no, 6u, &result, NULL, 0,
                                          &detail),
                     PAD_SERVICE_OK);
        CHECK_EQ_INT(result, PAD_OK);
        CHECK_EQ_INT(detail.field_a, 1u);
        CHECK_EQ_INT(detail.field_b, 1u);
        CHECK_EQ_INT(detail.record_offset, PAD_RECORD_BASE + port_no * PAD_RECORD_STRIDE);
    }

    /* cellPadEnd, then init again works. */
    CHECK_EQ_INT(pad_end(&state), 0);
    CHECK_EQ_INT(pad_init(&state, &port, 2u, 0u, NULL, 0, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
}

/* ------------------------------------------------------------------------------------ */
/* A readable pad on port 0                                                              */
/* ------------------------------------------------------------------------------------ */

#define PAD_TEST_DATA_GUARD 8u
#define PAD_TEST_DATA_GUARD_BYTE 0xA5u
#define PAD_TEST_INFO_BYTES PAD_INFO2_BYTES

/* An initialised service with `max_pads` = 2 and `host` installed on port 0. */
static void init_with_host_port(fake_port *fake, pad_port *port, pad_state *state,
                                const pad_host_state *host)
{
    int32_t result = 0;

    fake_init(fake);
    *port = fake_as_port(fake);
    memset(state, 0, sizeof(*state));
    if (host != NULL) {
        fake->states[0] = *host;
    }
    if (pad_init(state, port, 2u, 0u, NULL, 0, &result) != PAD_SERVICE_OK || result != PAD_OK) {
        fprintf(stderr, "fake setup: pad_init failed\n");
        exit(2);
    }
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

/* The u16 `word` of a `CellPadData` byte image, word 0 being `len`'s low half. */
static uint16_t data_word(const uint8_t *data_out, uint32_t word)
{
    return read_be16(data_out + 4u + word * 2u);
}

/* The `CellPadInfo2` array entry for `port_no`, given the byte offset of its array. */
static uint32_t info2_entry(const uint8_t *info_out, size_t array_base, uint32_t port_no)
{
    return read_be32(info_out + array_base + port_no * 4u);
}

#define PAD_TEST_PORT_STATUS_BASE 12u
#define PAD_TEST_PORT_SETTING_BASE (PAD_TEST_PORT_STATUS_BASE + PAD_INFO_PORT_COUNT * 4u)
#define PAD_TEST_CAPABILITY_BASE (PAD_TEST_PORT_SETTING_BASE + PAD_INFO_PORT_COUNT * 4u)
#define PAD_TEST_DEVICE_TYPE_BASE (PAD_TEST_CAPABILITY_BASE + PAD_INFO_PORT_COUNT * 4u)

/* One connected pad, nothing pressed, sticks at centre. */
static pad_host_state centred_host_state(void)
{
    pad_host_state host;
    memset(&host, 0, sizeof(host));
    host.connected = 1u;
    host.right_x = PAD_ANALOG_CENTRE;
    host.right_y = PAD_ANALOG_CENTRE;
    host.left_x = PAD_ANALOG_CENTRE;
    host.left_y = PAD_ANALOG_CENTRE;
    return host;
}

static void test_get_info2_uninitialized(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t info[PAD_INFO2_BYTES];
    int32_t result = 777;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    memset(info, 0x5A, sizeof(info));

    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_UNINITIALIZED);
    CHECK_EQ_INT(result, (int32_t)0x80121104u);
    CHECK_EQ_INT(info[0], 0x5A); /* nothing written */
    CHECK_EQ_INT(fake.state_calls, 0);

    /* A short or absent buffer is a bad argument, not a guest error path. */
    state.initialized = 1u;
    CHECK_EQ_INT(pad_get_info2(&state, &port, NULL, PAD_INFO2_BYTES, &result),
                 PAD_SERVICE_BAD_ARGUMENT);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, PAD_INFO2_BYTES - 1u, &result),
                 PAD_SERVICE_BAD_ARGUMENT);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
}

static void test_get_info2_image(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t info[PAD_INFO2_BYTES];
    uint8_t expected[PAD_INFO2_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 777;
    uint32_t port_no;

    init_with_host_port(&fake, &port, &state, &host);
    /* The game's two calls: ports 0 and 1, setting 6. */
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 1u, 6u, &result, NULL, 0, NULL),
                 PAD_SERVICE_OK);

    memset(info, 0xEE, sizeof(info));
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);

    memset(expected, 0, sizeof(expected));
    /* max_connect = 2, now_connect = 1, system_info = 0. */
    expected[3] = 2u;
    expected[7] = 1u;
    /* Port 0: connected, plus the change bit on the first report of a pad; the setting it
     * was given is stored and reported back. */
    expected[PAD_TEST_PORT_STATUS_BASE + 3u] = PAD_STATUS_BIT_CONNECTED |
                                               PAD_STATUS_BIT_ASSIGN_CHANGES;
    expected[PAD_TEST_PORT_SETTING_BASE + 3u] = 6u;
    expected[PAD_TEST_CAPABILITY_BASE + 3u] = 0x1Fu;
    /* Port 1: empty, so all four of its words stay zero, and the setting it was given is
     * reported back anyway (the hardware keeps the request per port). */
    expected[PAD_TEST_PORT_SETTING_BASE + 4u + 3u] = 6u;
    /* Ports 2..6 are outside max_connect and stay zero. */
    CHECK_EQ_MEM(info, expected, sizeof(expected));

    /* Byte-level spot checks so a wrong array base cannot pass by symmetry. */
    CHECK_EQ_INT(read_be32(&info[0]), 2u);
    CHECK_EQ_INT(read_be32(&info[4]), 1u);
    CHECK_EQ_INT(read_be32(&info[8]), 0u);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u),
                 PAD_STATUS_BIT_CONNECTED | PAD_STATUS_BIT_ASSIGN_CHANGES);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 1u), 0u);
    for (port_no = 2u; port_no < PAD_INFO_PORT_COUNT; port_no++) {
        CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, port_no), 0u);
        CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_SETTING_BASE, port_no), 0u);
        CHECK_EQ_INT(info2_entry(info, PAD_TEST_CAPABILITY_BASE, port_no), 0u);
        CHECK_EQ_INT(info2_entry(info, PAD_TEST_DEVICE_TYPE_BASE, port_no), 0u);
    }
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_DEVICE_TYPE_BASE, 0u), 0u);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_CAPABILITY_BASE, 1u), 0u);

    /* Changing the setting alone does not move the connection flag, so no change bit. */
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT1, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), PAD_STATUS_BIT_CONNECTED);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_SETTING_BASE, 0u), PAD_SETTING_BIT1);
}

static void test_get_info2_assign_changes_edge(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t info[PAD_INFO2_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    init_with_host_port(&fake, &port, &state, &host);

    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), PAD_STATUS_BIT_CONNECTED |
                                                                 PAD_STATUS_BIT_ASSIGN_CHANGES);
    /* Second read, same state: the bit is spent. */
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), PAD_STATUS_BIT_CONNECTED);
    CHECK_EQ_INT(read_be32(&info[4]), 1u);

    /* Unplug: the port drops out and the change is announced once. */
    host.connected = 0u;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u),
                 PAD_STATUS_BIT_ASSIGN_CHANGES);
    CHECK_EQ_INT(read_be32(&info[4]), 0u); /* now_connect */
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_CAPABILITY_BASE, 0u), 0u);
    /* Third read: the unplug edge is spent too, so the port reads fully disconnected. */
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), 0u);

    /* Plug back in: connected + change again. */
    host.connected = 1u;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), PAD_STATUS_BIT_CONNECTED |
                                                                 PAD_STATUS_BIT_ASSIGN_CHANGES);
    CHECK_EQ_INT(read_be32(&info[4]), 1u);
}

static void test_get_info2_no_state_reader(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t info[PAD_INFO2_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    init_with_host_port(&fake, &port, &state, &host);
    port.read_state = NULL;

    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    CHECK_EQ_INT(read_be32(&info[0]), 2u);
    CHECK_EQ_INT(read_be32(&info[4]), 0u); /* now_connect: nothing to read */
    /* A port that never held a pad is not a change: the remembered flag starts "absent". */
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), 0u);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 1u), 0u);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_CAPABILITY_BASE, 0u), 0u);

    /* A failing reader is just as empty as a missing one. */
    port.read_state = fake_read_state;
    fake.state_status = PAD_PORT_ERROR;
    CHECK_EQ_INT(pad_get_info2(&state, &port, info, sizeof(info), &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(&info[4]), 0u);
    CHECK_EQ_INT(info2_entry(info, PAD_TEST_PORT_STATUS_BASE, 0u), 0u);
}

static void test_get_data_argument_paths(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    memset(data_out, 0x5A, sizeof(data_out));

    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, sizeof(data_out), &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_UNINITIALIZED);
    CHECK_EQ_INT(result, (int32_t)0x80121104u);
    CHECK_EQ_INT(data_out[0], 0x5A);

    init_with_host_port(&fake, &port, &state, &host);

    /* max_pads is 2, so port 2 is out of range even though the pad table holds seven. */
    CHECK_EQ_INT(pad_get_data(&state, &port, 2u, data_out, sizeof(data_out), &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(pad_get_data(&state, &port, PAD_INFO_PORT_COUNT, data_out, sizeof(data_out),
                              &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);

    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, NULL, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_BAD_ARGUMENT);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES - 1u, &result),
                 PAD_SERVICE_BAD_ARGUMENT);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(data_out[0], 0x5A); /* nothing written */

    /* A port with no controller: NO_DEVICE, nothing written. */
    host.connected = 0u;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, sizeof(data_out), &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_NO_DEVICE);
    CHECK_EQ_INT(result, (int32_t)0x80121107u);
    CHECK_EQ_INT(data_out[0], 0x5A);
}

static void test_get_data_first_read_sensor(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES + PAD_TEST_DATA_GUARD];
    uint8_t expected[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    /* CROSS (digital2 0x40) plus LEFT (digital1 0x80), left stick at (0x10, 0xF0). */
    host.digital1 = PAD_CTRL_LEFT;
    host.digital2 = PAD_CTRL_CROSS;
    host.left_x = 0x10u;
    host.left_y = 0xF0u;

    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, NULL),
                 PAD_SERVICE_OK); /* press + sensor -> len 24 */

    memset(data_out, PAD_TEST_DATA_GUARD_BYTE, sizeof(data_out));
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);

    CHECK_EQ_INT(read_be32(data_out), PAD_DATA_LEN_SENSOR);
    CHECK_EQ_INT(read_be32(data_out), 24u);
    CHECK_EQ_INT(data_word(data_out, 0u), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PACKET_HEADER), 0x7Cu); /* 0x70 | 24/2 */
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL1), 0x80u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL2), 0x40u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_RIGHT_X), PAD_ANALOG_CENTRE);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_RIGHT_Y), PAD_ANALOG_CENTRE);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_LEFT_X), 0x10u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_LEFT_Y), 0xF0u);
    /* Pressure words: only the pressed buttons report full scale. */
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_RIGHT), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_LEFT), 0xFFu);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_UP), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_DOWN), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_TRIANGLE), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_CIRCLE), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_CROSS), 0xFFu);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_SQUARE), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_L1), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_R1), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_L2), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_R2), 0u);
    /* Sensors at rest. */
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_SENSOR_X), 512u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_SENSOR_Y), 399u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_SENSOR_Z), 512u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_SENSOR_G), 512u);
    /* Nothing past the 132 bytes, and nothing between the last reported word and them. */
    {   uint8_t guard[PAD_TEST_DATA_GUARD];
        size_t i;
        memset(guard, PAD_TEST_DATA_GUARD_BYTE, sizeof(guard));
        CHECK_EQ_MEM(data_out + PAD_DATA_BYTES, guard, sizeof(guard));
        for (i = 4u + PAD_DATA_LEN_SENSOR * 2u; i < PAD_DATA_BYTES; i++) {
            if (data_out[i] != 0u) {
                failures++;
                fprintf(stderr, "FAIL %s: byte %zu past the reported words is 0x%02X\n",
                        __FILE__, i, data_out[i]);
            }
        }
    }

    /* The whole image, so no word can drift: len 24 followed by the 24 words above. */
    memset(expected, 0, sizeof(expected));
    expected[3] = PAD_DATA_LEN_SENSOR;
    expected[4u + PAD_BTN_OFFSET_PACKET_HEADER * 2u] = 0x00u;
    expected[4u + PAD_BTN_OFFSET_PACKET_HEADER * 2u + 1u] = 0x7Cu;
    expected[4u + PAD_BTN_OFFSET_DIGITAL1 * 2u + 1u] = 0x80u;
    expected[4u + PAD_BTN_OFFSET_DIGITAL2 * 2u + 1u] = 0x40u;
    expected[4u + PAD_BTN_OFFSET_ANALOG_RIGHT_X * 2u + 1u] = PAD_ANALOG_CENTRE;
    expected[4u + PAD_BTN_OFFSET_ANALOG_RIGHT_Y * 2u + 1u] = PAD_ANALOG_CENTRE;
    expected[4u + PAD_BTN_OFFSET_ANALOG_LEFT_X * 2u + 1u] = 0x10u;
    expected[4u + PAD_BTN_OFFSET_ANALOG_LEFT_Y * 2u + 1u] = 0xF0u;
    expected[4u + PAD_BTN_OFFSET_PRESS_LEFT * 2u + 1u] = 0xFFu;
    expected[4u + PAD_BTN_OFFSET_PRESS_CROSS * 2u + 1u] = 0xFFu;
    expected[4u + PAD_BTN_OFFSET_SENSOR_X * 2u] = 512u >> 8;
    expected[4u + PAD_BTN_OFFSET_SENSOR_X * 2u + 1u] = 512u & 0xFFu;
    expected[4u + PAD_BTN_OFFSET_SENSOR_Y * 2u] = 399u >> 8;
    expected[4u + PAD_BTN_OFFSET_SENSOR_Y * 2u + 1u] = 399u & 0xFFu;
    expected[4u + PAD_BTN_OFFSET_SENSOR_Z * 2u] = 512u >> 8;
    expected[4u + PAD_BTN_OFFSET_SENSOR_Z * 2u + 1u] = 512u & 0xFFu;
    expected[4u + PAD_BTN_OFFSET_SENSOR_G * 2u] = 512u >> 8;
    expected[4u + PAD_BTN_OFFSET_SENSOR_G * 2u + 1u] = 512u & 0xFFu;
    CHECK_EQ_MEM(data_out, expected, sizeof(expected));
}

static void test_get_data_repeat_and_change(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES + PAD_TEST_DATA_GUARD];
    uint8_t zeros[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 6u, &result, NULL, 0, NULL),
                 PAD_SERVICE_OK);

    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), PAD_DATA_LEN_SENSOR);

    /* Unchanged state: len 0 and every other byte zero, so a title draining on len stops. */
    memset(data_out, PAD_TEST_DATA_GUARD_BYTE, sizeof(data_out));
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    memset(zeros, 0, sizeof(zeros));
    CHECK_EQ_MEM(data_out, zeros, sizeof(zeros));
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PACKET_HEADER), 0u);
    {   uint8_t guard[PAD_TEST_DATA_GUARD];
        memset(guard, PAD_TEST_DATA_GUARD_BYTE, sizeof(guard));
        CHECK_EQ_MEM(data_out + PAD_DATA_BYTES, guard, sizeof(guard));
    }

    /* A changed button produces a fresh full packet. */
    host.digital1 = PAD_CTRL_UP;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), PAD_DATA_LEN_SENSOR);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL1), PAD_CTRL_UP);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_UP), 0xFFu);

    /* Only the analog channel moving is a change too. */
    host.right_x = 0x00u;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), PAD_DATA_LEN_SENSOR);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_RIGHT_X), 0x00u);
}

static void test_get_data_lengths(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;
    uint32_t word;

    /* Setting bit 2 only: sensor, len 24. */
    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT2, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 24u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PACKET_HEADER), 0x7Cu);

    /* Setting bit 1 only: press, len 20, and the sensor words stay zero. */
    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT1, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 20u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PACKET_HEADER), 0x7Au);
    for (word = PAD_BTN_OFFSET_SENSOR_X; word < PAD_DATA_WORD_COUNT; word++) {
        CHECK_EQ_INT(data_word(data_out, word), 0u);
    }

    /* No setting: len 8, so words 8..23 stay zero. */
    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, 0u, &result, NULL, 0, NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 8u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PACKET_HEADER), 0x74u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_ANALOG_LEFT_Y), PAD_ANALOG_CENTRE);
    for (word = PAD_BTN_OFFSET_PRESS_RIGHT; word < PAD_DATA_WORD_COUNT; word++) {
        CHECK_EQ_INT(data_word(data_out, word), 0u);
    }
}

static void test_get_data_analog_triggers(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    /* L2 held with no analog value: the pressure word still reads full scale. */
    host.digital2 = PAD_CTRL_L2;
    host.l2_analog = 0u;
    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT1, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 20u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_L2), 0xFFu);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_R2), 0u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL2), PAD_CTRL_L2);

    /* A partially pulled trigger reports its own analog value, digital bit or not. */
    host.digital2 = 0u;
    host.l2_analog = 0x40u;
    host.r2_analog = 0x11u;
    host.digital2 = PAD_CTRL_R2;
    init_with_host_port(&fake, &port, &state, &host);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT1, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_L2), 0x40u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_R2), 0x11u);
}

static void test_get_data_second_port(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    uint8_t data_out[PAD_DATA_BYTES];
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    init_with_host_port(&fake, &port, &state, &host);
    /* Port 1 gets its own setting and its own controller. */
    fake.states[1] = centred_host_state();
    fake.states[1].digital2 = PAD_CTRL_CIRCLE;
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 1u, PAD_SETTING_BIT1, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(pad_set_port_setting(&state, &port, 0u, PAD_SETTING_BIT2, &result, NULL, 0,
                                      NULL),
                 PAD_SERVICE_OK);

    CHECK_EQ_INT(pad_get_data(&state, &port, 1u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 20u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL2), PAD_CTRL_CIRCLE);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_PRESS_CIRCLE), 0xFFu);

    CHECK_EQ_INT(pad_get_data(&state, &port, 0u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 24u);
    CHECK_EQ_INT(data_word(data_out, PAD_BTN_OFFSET_DIGITAL2), 0u);
    /* Port 0's packet was not consumed by the port 1 read: each port remembers its own. */
    CHECK_EQ_INT(pad_get_data(&state, &port, 1u, data_out, PAD_DATA_BYTES, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(read_be32(data_out), 0u);
}

static void test_set_act_direct_outcomes(void)
{
    fake_port fake;
    pad_port port;
    pad_state state;
    pad_host_state host = centred_host_state();
    int32_t result = 0;

    fake_init(&fake);
    port = fake_as_port(&fake);
    memset(&state, 0, sizeof(state));
    /* Uninitialised. */
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, 0u, 1, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_UNINITIALIZED);

    init_with_host_port(&fake, &port, &state, &host);
    /* Port out of range: max_pads is 2 and the pad table holds seven. */
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, 2u, 1, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, PAD_INFO_PORT_COUNT, 1, &result),
                 PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    /* No param. */
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, 0u, 0, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_INVALID_ARGUMENT);
    /* Accepted: no rumble output yet, but the call succeeds. */
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, 0u, 1, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_OK);
    /* No controller on the port. */
    host.connected = 0u;
    fake.states[0] = host;
    CHECK_EQ_INT(pad_set_act_direct(&state, &port, 0u, 1, &result), PAD_SERVICE_OK);
    CHECK_EQ_INT(result, PAD_ERROR_NO_DEVICE);
}

int main(void)
{
    test_init_argument_valid();
    test_init_guard();
    test_init_success_image();
    test_init_bad_argument();
    test_init_port_paths();
    test_state_image();
    test_end();
    test_set_port_setting_uninitialized();
    test_set_port_setting_bad_port();
    test_set_port_setting_writes();
    test_set_port_setting_port_paths();
    test_record_gate();
    test_set_port_setting_gate();
    test_game_sequence();

    test_get_info2_uninitialized();
    test_get_info2_image();
    test_get_info2_assign_changes_edge();
    test_get_info2_no_state_reader();
    test_get_data_argument_paths();
    test_get_data_first_read_sensor();
    test_get_data_repeat_and_change();
    test_get_data_lengths();
    test_get_data_analog_triggers();
    test_get_data_second_port();
    test_set_act_direct_outcomes();

    printf("checks=%d failures=%d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
