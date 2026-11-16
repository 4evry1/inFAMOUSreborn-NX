/* Platform-free pad service. See pad_service.h for the contract.
 *
 * No globals, no allocation, no OS calls: every byte the service hands back is written into
 * caller-provided buffers, and every controller fact arrives through the injected port.
 */
#include "pad_service.h"

#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Guest byte accessors                                                                 */
/* ------------------------------------------------------------------------------------ */

static void store_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

/* Writes a big-endian word at `offset` in `buf`, bounded by `cap`. Returns 1 when the whole
 * word fitted. */
static int store_be32_at(uint8_t *buf, size_t cap, size_t offset, uint32_t value)
{
    if (buf == NULL || offset > cap || cap - offset < 4u) {
        return 0;
    }
    store_be32(buf + offset, value);
    return 1;
}

/* The guest return value. Every PAD_ERROR_* macro already carries the negative int32 the
 * firmware's `extsw` produces, so this is only a readability hook. */
#define PAD_GUEST_RETURN(code) ((int32_t)(code))

/* ------------------------------------------------------------------------------------ */
/* cellPadInit argument predicate (§2.1)                                                 */
/* ------------------------------------------------------------------------------------ */

int pad_init_argument_valid(uint32_t count)
{
    /* The argument range check:
     * `count - 1 <= 0x7E` unsigned, i.e. count in [1, 0x7F]. Zero wraps to 0xFFFFFFFF and
     * fails; 0x80 gives 0x7F and fails; 0x7F gives 0x7E and passes. */
    if (count == 0u) {
        return 0;
    }
    return count - 1u <= PAD_MAX_PADS_ARG - 1u;
}

/* ------------------------------------------------------------------------------------ */
/* cellPadInit (§2.3, §2.4, §2.6)                                                        */
/* ------------------------------------------------------------------------------------ */

int32_t pad_init_guard(const pad_state *state)
{
    if (state == NULL) {
        return PAD_OK;
    }
    /* The already-initialized short circuit (the flag byte is tested against 0).
     * The branch target is the exit with r31 still holding 0x80121103. */
    if (state->initialized != 0u) {
        return PAD_GUEST_RETURN(PAD_ERROR_ALREADY_INITIALIZED);
    }
    return PAD_OK;
}

int32_t pad_end(pad_state *state)
{
    if (state == NULL) {
        return 0;
    }
    /* cellPadEnd (0x4D9B75D5) clears the flag byte and the handle word,
     * and returns 0. The whole state object goes away with the handle,
     * so the named fields are cleared here. */
    memset(state, 0, sizeof(*state));
    return 0;
}

size_t pad_state_image(const pad_state *state, uint8_t *out, size_t cap)
{
    size_t want = 0;

    if (state == NULL || out == NULL || cap == 0u) {
        return 0;
    }

    /* The image is the state object's own bytes, zero everywhere the contract does not name
     * (0x4E4C zeroes +0 and walks the record table; the named fields are written by the
     * init path). Build it in order so the buffer never needs to be fully materialised
     * before the fields are known. */
    want = cap < PAD_STATE_BYTES ? cap : PAD_STATE_BYTES;
    memset(out, 0, want);

    /* state+0x0A70 attached-port count. */
    (void)store_be32_at(out, want, PAD_STATE_ATTACHED, state->attached_pads);
    /* state+0x0A74 live-port count: equal to attached after a successful init. */
    (void)store_be32_at(out, want, PAD_STATE_LIVE, state->attached_pads);
    /* state+0x76F4 the count argument. */
    (void)store_be32_at(out, want, PAD_STATE_MAX_PADS, state->max_pads);
    /* state+0x76F8 and +0x7700 are explicitly zeroed by 0x5198/0x519C; the memset covers it. */
    /* state+0x7704 the syscall-512 boolean. */
    if (PAD_STATE_SC512_FLAG < want) {
        out[PAD_STATE_SC512_FLAG] = (uint8_t)(state->sc512_flag != 0u ? 1u : 0u);
    }
    /* Record 0 head: +0x18 gate, +0x1C field A, +0x20 field B, when the caller previously
     * drove a setter call on port 0. The record image is a raw overlay, so it is copied
     * verbatim rather than re-derived. */
    if (PAD_RECORD_BASE + PAD_RECORD_STRIDE <= want) {
        memcpy(out + PAD_RECORD_BASE, state->records, PAD_RECORD_STRIDE);
    }
    return want;
}

pad_status pad_init(pad_state *state, const pad_port *port, uint32_t count,
                    uint32_t pad_assumed_count, uint8_t *state_out, size_t state_bytes,
                    int32_t *result_out)
{
    uint32_t host_count = 0;
    pad_port_status port_status = PAD_PORT_UNAVAILABLE;

    if (state == NULL) {
        return PAD_SERVICE_BAD_ARGUMENT;
    }

    /* 1. 0x248C..0x24A0: the flag short circuit, before the argument is even looked at. */
    if (state->initialized != 0u) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_ALREADY_INITIALIZED);
        }
        return PAD_SERVICE_OK;
    }

    /* 2. 0x5130..0x5154 in 0x5120: the count range check. */
    if (!pad_init_argument_valid(count)) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_OK;
    }

    /* 3. Ask the host. The firmware never compares its argument against a host count (§2.1
     * is a range check only), so this answer decides `attached_pads`, not the return code. */
    if (port != NULL && port->pad_count != NULL) {
        port_status = port->pad_count(port->context, &host_count);
    }
    if (port_status == PAD_PORT_ERROR) {
        /* The firmware's allocation-failure arm (0x2564 -> 0x257C -> exit with r31 still
         * holding 0x80121105). */
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_NO_MEMORY);
        }
        return PAD_SERVICE_PORT_ERROR;
    }
    if (port_status != PAD_PORT_OK) {
        host_count = pad_assumed_count;
    }
    if (host_count > PAD_MAX_PADS_ARG) {
        host_count = PAD_MAX_PADS_ARG;
    }

    /* 4. 0x5180 (max pads), 0x5198/0x519C (zeroed), 0x51FC (sc 512 result), 0x24E8
     *    (initialized = 1). */
    state->max_pads = count;
    state->sc512_flag = 0;
    state->attached_pads = host_count;
    state->lock_held = 0;
    memset(state->records, 0, sizeof(state->records));
    /* The reader state starts as "every port is not connected and has reported nothing", so
     * the first report of a pad is a change and a port that never held one never is. */
    memset(state->reader, 0, sizeof(state->reader));
    state->initialized = 1;

    if (state_out != NULL && state_bytes != 0u) {
        (void)pad_state_image(state, state_out, state_bytes);
    }
    if (result_out != NULL) {
        *result_out = PAD_OK;
    }
    return PAD_SERVICE_OK;
}

/* ------------------------------------------------------------------------------------ */
/* cellPadSetPortSetting (§3.1..§3.5)                                                    */
/* ------------------------------------------------------------------------------------ */

int pad_record_gate_open(uint8_t gate_flags, uint32_t subop)
{
    /* Sub-op 3 requires bit 1 of the record's flags byte;
     * sub-op 5 requires bit 2. */
    if (subop == PAD_SYSCALL_SUBOP_A) {
        return (gate_flags & 0x02u) != 0u;
    }
    if (subop == PAD_SYSCALL_SUBOP_B) {
        return (gate_flags & 0x04u) != 0u;
    }
    return 0;
}

pad_status pad_set_port_setting(pad_state *state, const pad_port *port, uint32_t port_no,
                                uint32_t setting, int32_t *result_out, uint8_t *record_out,
                                size_t record_bytes, pad_setting_result *detail_out)
{
    uint32_t field_a;
    uint32_t field_b;
    uint32_t record_offset;

    if (detail_out != NULL) {
        memset(detail_out, 0, sizeof(*detail_out));
    }
    if (state == NULL) {
        return PAD_SERVICE_BAD_ARGUMENT;
    }

    /* 1. 0x18F4:0x18F4..0x1918 -> 0x1920 returns 0x80121104 when the flag is clear. The
     *    same code covers "flag set but the handle cell is 0" (0x1950); this service folds
     *    the handle into `initialized`, so there is one predicate. */
    if (state->initialized == 0u) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_UNINITIALIZED);
        }
        return PAD_SERVICE_OK;
    }

    /* 2. port > PAD_PORT_MAX returns
     *    0x80121102 after unlocking. Ports 0..0x7E pass. */
    if (port_no > PAD_PORT_MAX) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_OK;
    }

    /* The port is consulted here only so an adapter with a real HID source can say "no pad
     * on this port". The firmware's 0x33F8 never reads a capability (§0.3), so a missing or
     * silent port changes nothing: PAD_PORT_UNAVAILABLE is not a failure. A hard port error
     * is the only thing that can turn this call into PAD_SERVICE_PORT_ERROR, and even then
     * the guest code is unchanged, because the game ignores it (§4.2). */
    if (port != NULL && port->capability != NULL) {
        pad_capability capability;
        memset(&capability, 0, sizeof(capability));
        if (port->capability(port->context, port_no, &capability) == PAD_PORT_ERROR) {
            if (result_out != NULL) {
                *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
            }
            return PAD_SERVICE_PORT_ERROR;
        }
    }

    /* 3. 0x3450: bit 1 -> +0x1C via 0x567C ; 0x3478: bit 2 -> +0x20 via 0x56F8. */
    field_a = (setting & PAD_SETTING_BIT1) != 0u ? 1u : 0u;
    field_b = (setting & PAD_SETTING_BIT2) != 0u ? 1u : 0u;
    record_offset = PAD_RECORD_BASE + port_no * PAD_RECORD_STRIDE;

    /* Overlay the record head in this instance's image. Only port 0 has an image slot in
     * `pad_state.records`; a higher port's record is reported through `detail_out` and
     * `record_out` instead of being stored, because the service owns no per-port array. */
    if (port_no == 0u) {
        store_be32(state->records + PAD_RECORD_FIELD_A, field_a);
        store_be32(state->records + PAD_RECORD_FIELD_B, field_b);
    }

    /* The raw setting is what `cellPadGetInfo2.port_setting` reports back and what decides
     * the length `cellPadGetData` answers with, so it is kept per port (ports a reader entry
     * can describe only; a higher port's setting has no reader). */
    if (port_no < PAD_INFO_PORT_COUNT) {
        state->reader[port_no].setting = setting;
    }

    if (record_out != NULL && record_bytes >= 12u) {
        /* offset 0 is the +0x18 gate byte as the adapter knows it; this service leaves it
         * untouched (it is armed by the port-open path, 0xB8C4, not by this entry) and
         * reports it as 0 here. offsets 4 and 8 carry the two writes, big-endian. */
        store_be32(record_out + 4u, field_a);
        store_be32(record_out + 8u, field_b);
    }

    if (detail_out != NULL) {
        detail_out->applied = 1u;
        detail_out->field_a = field_a;
        detail_out->field_b = field_b;
        detail_out->record_offset = record_offset;
        /* 0x56C8 / 0x5744: the syscall only runs when the record's own +0x18 gate bit is
         * set. On a host with no opened record the gate is closed, so this stays 0. */
        if (pad_record_gate_open(state->records[PAD_RECORD_FLAGS], PAD_SYSCALL_SUBOP_A)) {
            detail_out->syscall_needed = 1u;
            detail_out->syscall_number = PAD_SYSCALL_RECORD_WRITE;
            detail_out->syscall_subop = PAD_SYSCALL_SUBOP_A;
        } else if (pad_record_gate_open(state->records[PAD_RECORD_FLAGS],
                                        PAD_SYSCALL_SUBOP_B)) {
            detail_out->syscall_needed = 1u;
            detail_out->syscall_number = PAD_SYSCALL_RECORD_WRITE;
            detail_out->syscall_subop = PAD_SYSCALL_SUBOP_B;
        }
    }

    if (result_out != NULL) {
        *result_out = PAD_OK;
    }
    return PAD_SERVICE_OK;
}

/* ------------------------------------------------------------------------------------ */
/* The reader entries: cellPadGetInfo2, cellPadGetData, cellPadSetActDirect              */
/* ------------------------------------------------------------------------------------ */

static void store_be16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

/* Writes a big-endian u16 at `offset` in `buf`, bounded by `cap`. Returns 1 when the whole
 * word fitted. */
static int store_be16_at(uint8_t *buf, size_t cap, size_t offset, uint16_t value)
{
    if (buf == NULL || offset > cap || cap - offset < 2u) {
        return 0;
    }
    store_be16(buf + offset, value);
    return 1;
}

/* One `CellPadData.button` word: the payload starts after the four-byte `len`. */
#define PAD_DATA_WORD_BYTES 2u
#define PAD_DATA_LEN_BYTES 4u

static int store_data_word(uint8_t *data_out, size_t data_bytes, uint32_t word, uint16_t value)
{
    return store_be16_at(data_out, data_bytes, PAD_DATA_LEN_BYTES + word * PAD_DATA_WORD_BYTES,
                         value);
}

/* One `CellPadInfo2` array entry. `array_base` is the offset of the array the word belongs
 * to, `index` the port within it. */
static int store_info2_word(uint8_t *info_out, size_t info_bytes, size_t array_base,
                            uint32_t index, uint32_t value)
{
    return store_be32_at(info_out, info_bytes, array_base + index * 4u, value);
}

/* The number of ports the guest can address: never more than the arrays hold, never more
 * than `cellPadInit` accepted. */
static uint32_t pack_port_limit(const pad_state *state)
{
    if (state->max_pads < PAD_INFO_PORT_COUNT) {
        return state->max_pads;
    }
    return PAD_INFO_PORT_COUNT;
}

/* The host's answer for one port, normalised: 1 when a controller is on it. Any port
 * failure, a missing reader and `connected == 0` all mean "empty port" here, because a
 * reader has no guest code to report a host failure with. */
static int read_host_state(const pad_port *port, uint32_t port_no, pad_host_state *out)
{
    if (port == NULL || port->read_state == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (port->read_state(port->context, port_no, out) != PAD_PORT_OK) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return out->connected != 0u;
}

/* A pressure word: the button's full-scale value when its digital bit is set or when its
 * analog channel already reports pressure, otherwise 0. */
static uint16_t pressure_word(uint32_t digital, uint16_t digital_bit, uint8_t analog)
{
    if (analog != 0u) {
        return (uint16_t)analog;
    }
    if ((digital & digital_bit) != 0u) {
        return (uint16_t)PAD_PRESSURE_PRESSED;
    }
    return 0u;
}

static int host_states_match(const pad_host_state *left, const pad_host_state *right)
{
    return left->connected == right->connected && left->digital1 == right->digital1 &&
           left->digital2 == right->digital2 && left->right_x == right->right_x &&
           left->right_y == right->right_y && left->left_x == right->left_x &&
           left->left_y == right->left_y && left->l2_analog == right->l2_analog &&
           left->r2_analog == right->r2_analog;
}

/* `CellPadInfo2` is three u32 counters then four u32 arrays of PAD_INFO_PORT_COUNT entries. */
#define PAD_INFO2_PORT_STATUS_BASE 12u
#define PAD_INFO2_PORT_SETTING_BASE (PAD_INFO2_PORT_STATUS_BASE + PAD_INFO_PORT_COUNT * 4u)
#define PAD_INFO2_DEVICE_CAPABILITY_BASE (PAD_INFO2_PORT_SETTING_BASE + PAD_INFO_PORT_COUNT * 4u)
#define PAD_INFO2_DEVICE_TYPE_BASE \
    (PAD_INFO2_DEVICE_CAPABILITY_BASE + PAD_INFO_PORT_COUNT * 4u)

pad_status pad_get_info2(pad_state *state, const pad_port *port, uint8_t *info_out,
                         size_t info_bytes, int32_t *result_out)
{
    uint32_t connected_count = 0;
    uint32_t port_no;

    if (state == NULL) {
        return PAD_SERVICE_BAD_ARGUMENT;
    }
    if (state->initialized == 0u) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_UNINITIALIZED);
        }
        return PAD_SERVICE_OK;
    }
    if (info_out == NULL || info_bytes < PAD_INFO2_BYTES) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_BAD_ARGUMENT;
    }

    memset(info_out, 0, PAD_INFO2_BYTES);

    for (port_no = 0u; port_no < pack_port_limit(state); port_no++) {
        pad_host_state host;
        pad_reader_port *reader = &state->reader[port_no];
        uint32_t status_word = 0u;
        uint32_t connection_flag;
        int is_connected = read_host_state(port, port_no, &host);
        int announced = 0;

        connection_flag = is_connected ? 1u : 0u;
        /* The assignment-change bit is a one-shot edge on the connection flag, which starts
         * out "not connected" (see pad_init): a port that never held a pad never shows it,
         * and one that changes state shows it exactly once. */
        if (reader->reported_connected != connection_flag) {
            announced = 1;
            reader->reported_connected = connection_flag;
        }
        if (is_connected) {
            connected_count++;
            status_word |= PAD_STATUS_BIT_CONNECTED;
            (void)store_info2_word(info_out, info_bytes, PAD_INFO2_DEVICE_CAPABILITY_BASE,
                                   port_no, PAD_DEVICE_CAPABILITY_STANDARD);
            (void)store_info2_word(info_out, info_bytes, PAD_INFO2_DEVICE_TYPE_BASE, port_no,
                                   PAD_DEVICE_TYPE_STANDARD);
        }
        if (announced) {
            status_word |= PAD_STATUS_BIT_ASSIGN_CHANGES;
        }
        (void)store_info2_word(info_out, info_bytes, PAD_INFO2_PORT_STATUS_BASE, port_no,
                               status_word);
        (void)store_info2_word(info_out, info_bytes, PAD_INFO2_PORT_SETTING_BASE, port_no,
                               reader->setting);
    }

    (void)store_be32_at(info_out, info_bytes, 0u, state->max_pads);
    (void)store_be32_at(info_out, info_bytes, 4u, connected_count);
    (void)store_be32_at(info_out, info_bytes, 8u, 0u);

    if (result_out != NULL) {
        *result_out = PAD_OK;
    }
    return PAD_SERVICE_OK;
}

/* The reported length: the port setting selects pressure words (bit 1) and sensor words
 * (bit 2), sensor taking precedence, as the firmware's CELL_PAD_LEN_CHANGE_* choice does. */
static uint32_t reported_data_length(uint32_t setting)
{
    if ((setting & PAD_SETTING_BIT2) != 0u) {
        return PAD_DATA_LEN_SENSOR;
    }
    if ((setting & PAD_SETTING_BIT1) != 0u) {
        return PAD_DATA_LEN_PRESS;
    }
    return PAD_DATA_LEN_DEFAULT;
}

/* `CellPadData.button[1]`: pad mode in the high nibble, half the reported length in the low
 * one. Only ever called for a packet that carries data, so `length` is non-zero. */
static uint16_t packet_header_word(uint32_t length)
{
    uint32_t half_length = length / 2u;

    if (half_length > PAD_DATA_HEADER_LENGTH_MASK) {
        half_length = PAD_DATA_HEADER_LENGTH_MASK;
    }
    return (uint16_t)(PAD_DATA_HEADER_PAD_MODE | half_length);
}

static void write_digital_words(uint8_t *data_out, size_t data_bytes, const pad_host_state *host)
{
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_DIGITAL1, host->digital1);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_DIGITAL2, host->digital2);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_ANALOG_RIGHT_X, host->right_x);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_ANALOG_RIGHT_Y, host->right_y);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_ANALOG_LEFT_X, host->left_x);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_ANALOG_LEFT_Y, host->left_y);
}

/* The eight pressure words that report full scale while their button is down. */
static void write_face_pressure_words(uint8_t *data_out, size_t data_bytes,
                                      const pad_host_state *host)
{
    static const struct {
        uint32_t word;
        uint16_t digital_bit;
    } pressure[] = {
        {PAD_BTN_OFFSET_PRESS_TRIANGLE, PAD_CTRL_TRIANGLE},
        {PAD_BTN_OFFSET_PRESS_CIRCLE, PAD_CTRL_CIRCLE},
        {PAD_BTN_OFFSET_PRESS_CROSS, PAD_CTRL_CROSS},
        {PAD_BTN_OFFSET_PRESS_SQUARE, PAD_CTRL_SQUARE},
        {PAD_BTN_OFFSET_PRESS_L1, PAD_CTRL_L1},
        {PAD_BTN_OFFSET_PRESS_R1, PAD_CTRL_R1},
        {PAD_BTN_OFFSET_PRESS_L2, PAD_CTRL_L2},
        {PAD_BTN_OFFSET_PRESS_R2, PAD_CTRL_R2},
    };
    size_t i;

    for (i = 0u; i < sizeof(pressure) / sizeof(pressure[0]); i++) {
        uint16_t value;
        if (pressure[i].word == PAD_BTN_OFFSET_PRESS_L2) {
            value = pressure_word(host->digital2, pressure[i].digital_bit, host->l2_analog);
        } else if (pressure[i].word == PAD_BTN_OFFSET_PRESS_R2) {
            value = pressure_word(host->digital2, pressure[i].digital_bit, host->r2_analog);
        } else {
            value = pressure_word(host->digital2, pressure[i].digital_bit, 0u);
        }
        (void)store_data_word(data_out, data_bytes, pressure[i].word, value);
    }
    /* The four D-pad directions carry no analog channel on the host: the digital bit alone
     * decides their pressure word. */
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_PRESS_UP,
                          pressure_word(host->digital1, PAD_CTRL_UP, 0u));
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_PRESS_DOWN,
                          pressure_word(host->digital1, PAD_CTRL_DOWN, 0u));
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_PRESS_LEFT,
                          pressure_word(host->digital1, PAD_CTRL_LEFT, 0u));
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_PRESS_RIGHT,
                          pressure_word(host->digital1, PAD_CTRL_RIGHT, 0u));
}

/* The four sensor words at rest. The host has no motion source yet, so the pad is always
 * reported level. */
static void write_sensor_words(uint8_t *data_out, size_t data_bytes)
{
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_SENSOR_X, PAD_SENSOR_ACCEL_X);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_SENSOR_Y, PAD_SENSOR_ACCEL_Y);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_SENSOR_Z, PAD_SENSOR_ACCEL_Z);
    (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_SENSOR_G, PAD_SENSOR_GYRO);
}

pad_status pad_get_data(pad_state *state, const pad_port *port, uint32_t port_no,
                        uint8_t *data_out, size_t data_bytes, int32_t *result_out)
{
    pad_host_state host;
    pad_reader_port *reader;
    uint32_t length;

    if (state == NULL) {
        return PAD_SERVICE_BAD_ARGUMENT;
    }
    if (state->initialized == 0u) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_UNINITIALIZED);
        }
        return PAD_SERVICE_OK;
    }
    if (port_no >= PAD_READ_PORT_MAX || port_no >= state->max_pads) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_OK;
    }
    if (data_out == NULL || data_bytes < PAD_DATA_BYTES) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_BAD_ARGUMENT;
    }
    if (!read_host_state(port, port_no, &host)) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_NO_DEVICE);
        }
        return PAD_SERVICE_OK;
    }

    reader = &state->reader[port_no];
    memset(data_out, 0, PAD_DATA_BYTES);

    /* A port hands out one packet per host report: a repeat read of an unchanged state
     * answers len 0 and titles that drain on len stop there, as on hardware. */
    length = PAD_DATA_LEN_NO_CHANGE;
    if (reader->has_reported == 0u || !host_states_match(&reader->reported, &host)) {
        length = reported_data_length(reader->setting);
        /* Word 0 is reserved and stays 0. Word 1 is the libpad packet header: 0x7 in the
         * high nibble (the pad mode) and the reported length halved and capped at 15 in the
         * low one, which is what the lift's cellPad.c emits and a title such as GH3 expects. */
        (void)store_data_word(data_out, data_bytes, PAD_BTN_OFFSET_PACKET_HEADER,
                              packet_header_word(length));
        write_digital_words(data_out, data_bytes, &host);
        if (length >= PAD_DATA_LEN_PRESS) {
            write_face_pressure_words(data_out, data_bytes, &host);
        }
        if (length >= PAD_DATA_LEN_SENSOR) {
            write_sensor_words(data_out, data_bytes);
        }
    }

    (void)store_be32_at(data_out, data_bytes, 0u, length);
    reader->reported = host;
    reader->has_reported = 1u;

    if (result_out != NULL) {
        *result_out = PAD_OK;
    }
    return PAD_SERVICE_OK;
}

pad_status pad_set_act_direct(pad_state *state, const pad_port *port, uint32_t port_no,
                              int has_param, int32_t *result_out)
{
    pad_host_state host;

    if (state == NULL) {
        return PAD_SERVICE_BAD_ARGUMENT;
    }
    if (state->initialized == 0u) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_UNINITIALIZED);
        }
        return PAD_SERVICE_OK;
    }
    if (port_no >= PAD_READ_PORT_MAX || port_no >= state->max_pads || has_param == 0) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_INVALID_ARGUMENT);
        }
        return PAD_SERVICE_OK;
    }
    if (!read_host_state(port, port_no, &host)) {
        if (result_out != NULL) {
            *result_out = PAD_GUEST_RETURN(PAD_ERROR_NO_DEVICE);
        }
        return PAD_SERVICE_OK;
    }

    /* Accepted and dropped: the service drives no actuator, so there is no host call and no
     * state to keep. A rumble-capable adapter adds that behind this entry later. */
    if (result_out != NULL) {
        *result_out = PAD_OK;
    }
    return PAD_SERVICE_OK;
}
