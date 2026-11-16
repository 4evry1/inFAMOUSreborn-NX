#ifndef PAD_SERVICE_H
#define PAD_SERVICE_H
/* Platform-free, instance-owned native pad service for the two `sys_io` entries batch B3
 * needed on the game's path to its first frame:
 *
 *   0x1CF98800  cellPadInit            (sys_io export 5)
 *   0x578E3C98  cellPadSetPortSetting  (sys_io export 23)
 *
 * `0x578E3C98` is NOT `cellPadGetCapabilityInfo` (that name hashes to 0xDBF4C59C, a
 * different export); the reference runtime's table is wrong for this NID.
 *
 * The service owns no globals and makes no OS calls. Controller facts arrive through an
 * injected `pad_port`; the caller decides what "how many pads are connected" and "what
 * capabilities does pad N have" mean on the host (a Horizon HID adapter, a test fake, or
 * nothing at all). Guest memory is never touched: every value the firmware writes is
 * produced as a big-endian byte buffer the adapter copies out.
 *
 * Contract sources (CONTRACT.md section in parentheses):
 *   §2.1  the single `cellPadInit` argument and its accepted set [1, 0x7F];
 *   §2.3  the ordered body of cellPadInit and its two module-static cells;
 *   §2.4  the exact return codes and which of them are reachable;
 *   §2.6  the 30496-byte state object and the fields this contract names;
 *   §3.1  the `cellPadSetPortSetting` signature (r3 port, r4 setting);
 *   §3.2  the body of 0x33F8 and its record write rule;
 *   §3.3  the two guest writes 0x567C/0x56F8 and the syscall-503 condition;
 *   §3.4  the exact return codes;
 *   §4.2  the game's two calls: ports 0 and 1, setting 6, return ignored.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* Firmware constants                                               */
/* ------------------------------------------------------------------------------------ */

/* Error codes. Every one is a load of 0x8012 into the high half plus a low part, so the 32-bit value is 0x8012xxxx and the
 * firmware sign-extends it before returning, which makes it a negative
 * int32 in the guest's `r3`. These macros carry the same sign. */
#define PAD_OK 0
#define PAD_ERROR_INVALID_ALIGNMENT ((int32_t)0x80121101) /* CELL_PAD_ERROR_INVALID_ALIGNMENT */
#define PAD_ERROR_INVALID_ARGUMENT ((int32_t)0x80121102)  /* CELL_PAD_ERROR_INVALID_ARGUMENT */
#define PAD_ERROR_ALREADY_INITIALIZED ((int32_t)0x80121103) /* ALREADY_INITIALIZED */
#define PAD_ERROR_UNINITIALIZED ((int32_t)0x80121104)     /* CELL_PAD_ERROR_UNINITIALIZED */
#define PAD_ERROR_NO_MEMORY ((int32_t)0x80121105)         /* CELL_PAD_ERROR_NO_MEMORY */
#define PAD_ERROR_NO_DEVICE ((int32_t)0x80121107)         /* CELL_PAD_ERROR_NO_DEVICE */

/* `cellPadInit` argument range: `r4 - 1 <= 0x7E` unsigned accepts
 * `r4` in [1, 0x7F]. Zero and anything >= 0x80 are rejected with
 * PAD_ERROR_INVALID_ARGUMENT. Note 0x7F passes: the bound is inclusive, not [1,0x7E]. */
#define PAD_MAX_PADS_ARG 0x7Fu

/* `cellPadSetPortSetting` port bound:
 * `port > 0x7E` is rejected with PAD_ERROR_INVALID_ARGUMENT, so ports 0..0x7E pass. */
#define PAD_PORT_MAX 0x7Eu

/* `CellPadGetInfo2`/`cellPadGetData`/`cellPadSetActDirect` port bound:
 * reject on >= (CELL_PAD_MAX_PORT_NUM). Narrower than PAD_PORT_MAX. */
#define PAD_READ_PORT_MAX 7u

/* Setting bits the firmware acts on. The guest value is a bit
 * mask; only bits 1 and 2 are read. */
#define PAD_SETTING_BIT1 (1u << 1) /* -> record +0x1C, write sub-op 3 when +0x18 bit1 set */
#define PAD_SETTING_BIT2 (1u << 2) /* -> record +0x20, write sub-op 5 when +0x18 bit2 set */

/* ------------------------------------------------------------------------------------ */
/* cellPadGetInfo2 / cellPadGetData / cellPadSetActDirect shape                          */
/* ------------------------------------------------------------------------------------ */

/* `CellPadInfo2` on the wire: max_connect, now_connect, system_info, then four u32 arrays
 * of one word per port (source/ps3recomp/libs/input/cellPad.h): 3*4 + 4*7*4. */
#define PAD_INFO2_BYTES 124u
/* `CellPadData` on the wire: s32 len, then CELL_PAD_MAX_CODES u16 entries: 4 + 64*2. */
#define PAD_DATA_BYTES 132u

/* Ports a `CellPadInfo2` word array can describe (CELL_PAD_MAX_PORT_NUM). */
#define PAD_INFO_PORT_COUNT 7u
/* `CellPadData.button` entries the service fills (CELL_PAD_MAX_CODES). */
#define PAD_DATA_WORD_COUNT 64u
/* The three wire lengths a reported change can have (CELL_PAD_LEN_CHANGE_*): len counts
 * u16 entries, so the byte counts are `2*len`. */
#define PAD_DATA_LEN_DEFAULT 8u   /* digital words only, no port setting bit */
#define PAD_DATA_LEN_PRESS 20u    /* + the twelve pressure words (setting bit 1) */
#define PAD_DATA_LEN_SENSOR 24u   /* + the four sensor words (setting bit 2) */
#define PAD_DATA_LEN_NO_CHANGE 0u /* nothing new on this port since the last read */
/* `CellPadData.button[1]` is the libpad packet header: pad mode 0x7 in bits 7..4 and the
 * reported length halved (capped at the low nibble) in bits 3..0. */
#define PAD_DATA_HEADER_PAD_MODE 0x70u
#define PAD_DATA_HEADER_LENGTH_MASK 0x0Fu

/* `CellPadInfo2.port_status` bits. */
#define PAD_STATUS_BIT_CONNECTED 1u      /* CELL_PAD_STATUS_CONNECTED */
#define PAD_STATUS_BIT_ASSIGN_CHANGES 2u /* CELL_PAD_STATUS_ASSIGN_CHANGES */
/* What an attached standard pad advertises: PS3 conformity, press mode, sensor mode,
 * high-precision analog stick, actuator (CELL_PAD_CAPABILITY_* bits 0..4). */
#define PAD_DEVICE_CAPABILITY_STANDARD 0x1Fu
/* CELL_PAD_DEV_TYPE_STANDARD. */
#define PAD_DEVICE_TYPE_STANDARD 0u

/* `CellPadData.button` word indices (CELL_PAD_BTN_OFFSET_*). */
#define PAD_BTN_OFFSET_PACKET_HEADER 1u
#define PAD_BTN_OFFSET_DIGITAL1 2u
#define PAD_BTN_OFFSET_DIGITAL2 3u
#define PAD_BTN_OFFSET_ANALOG_RIGHT_X 4u
#define PAD_BTN_OFFSET_ANALOG_RIGHT_Y 5u
#define PAD_BTN_OFFSET_ANALOG_LEFT_X 6u
#define PAD_BTN_OFFSET_ANALOG_LEFT_Y 7u
#define PAD_BTN_OFFSET_PRESS_RIGHT 8u
#define PAD_BTN_OFFSET_PRESS_LEFT 9u
#define PAD_BTN_OFFSET_PRESS_UP 10u
#define PAD_BTN_OFFSET_PRESS_DOWN 11u
#define PAD_BTN_OFFSET_PRESS_TRIANGLE 12u
#define PAD_BTN_OFFSET_PRESS_CIRCLE 13u
#define PAD_BTN_OFFSET_PRESS_CROSS 14u
#define PAD_BTN_OFFSET_PRESS_SQUARE 15u
#define PAD_BTN_OFFSET_PRESS_L1 16u
#define PAD_BTN_OFFSET_PRESS_R1 17u
#define PAD_BTN_OFFSET_PRESS_L2 18u
#define PAD_BTN_OFFSET_PRESS_R2 19u
#define PAD_BTN_OFFSET_SENSOR_X 20u
#define PAD_BTN_OFFSET_SENSOR_Y 21u
#define PAD_BTN_OFFSET_SENSOR_Z 22u
#define PAD_BTN_OFFSET_SENSOR_G 23u

/* `CELL_PAD_CTRL_*` bits of `pad_host_state.digital1` (button word 2). */
#define PAD_CTRL_SELECT 0x01u
#define PAD_CTRL_L3 0x02u
#define PAD_CTRL_R3 0x04u
#define PAD_CTRL_START 0x08u
#define PAD_CTRL_UP 0x10u
#define PAD_CTRL_RIGHT 0x20u
#define PAD_CTRL_DOWN 0x40u
#define PAD_CTRL_LEFT 0x80u

/* `CELL_PAD_CTRL_*` bits of `pad_host_state.digital2` (button word 3). */
#define PAD_CTRL_L2 0x01u
#define PAD_CTRL_R2 0x02u
#define PAD_CTRL_L1 0x04u
#define PAD_CTRL_R1 0x08u
#define PAD_CTRL_TRIANGLE 0x10u
#define PAD_CTRL_CIRCLE 0x20u
#define PAD_CTRL_CROSS 0x40u
#define PAD_CTRL_SQUARE 0x80u

/* A pressure word for a button that is pressed: the firmware reports full scale. */
#define PAD_PRESSURE_PRESSED 0xFFu
#define PAD_ANALOG_CENTRE 0x80u
/* Accelerometer at rest, in DS3 units (1g centre, gravity on Y). */
#define PAD_SENSOR_ACCEL_X 512u
#define PAD_SENSOR_ACCEL_Y 399u
#define PAD_SENSOR_ACCEL_Z 512u
#define PAD_SENSOR_GYRO 512u

/* The state object `0x4F38` allocates. */
#define PAD_STATE_BYTES 30496u
#define PAD_STATE_MAX_PADS 0x76F4u     /* u32: the cellPadInit count argument */
#define PAD_STATE_ZERO_A 0x76F8u       /* u32: zeroed by 0x5198 */
#define PAD_STATE_ZERO_B 0x7700u       /* u32: zeroed by 0x519C */
#define PAD_STATE_SC512_FLAG 0x7704u   /* u8: syscall-512 result was non-zero (0x51FC) */
#define PAD_STATE_LOCK 0x7708u         /* the 0xC3AC/0xC3CC lock object (0x4A4C, 0x3408) */
#define PAD_STATE_ATTACHED 0x0A70u     /* u32: attached-port count (0xA8A4) */
#define PAD_STATE_LIVE 0x0A74u         /* u32: live-port count (0xA8E8, 0xB62C) */

/* Per-port record table: 127 records of 232 bytes starting at
 * state+0x104 (record step 0xE8). */
#define PAD_RECORD_BASE 0x0104u
#define PAD_RECORD_STRIDE 232u
#define PAD_RECORD_COUNT 127u
/* Record fields the setter touches. */
#define PAD_RECORD_FLAGS 0x18u /* u8: gate for the syscall-503 replay */
#define PAD_RECORD_FIELD_A 0x1Cu /* u32 (be32 in guest memory): bit 1 of the setting */
#define PAD_RECORD_FIELD_B 0x20u /* u32 (be32 in guest memory): bit 2 of the setting */

/* The syscall the record writer issues when the record's own flag bit is set
 * syscall 503, r4 = 3 for bit 1 and 5 for bit 2, r5 = &value, r6 = 4. */
#define PAD_SYSCALL_RECORD_WRITE 503u
#define PAD_SYSCALL_SUBOP_A 3u
#define PAD_SYSCALL_SUBOP_B 5u

/* ------------------------------------------------------------------------------------ */
/* Injected controller port                                                              */
/* ------------------------------------------------------------------------------------ */

/* The adapter supplies controller facts. Every function is total: it returns one of these
 * and never throws. There is no global "pad count": the port answers per question. */
typedef enum pad_port_status {
    PAD_PORT_OK = 0,
    PAD_PORT_UNAVAILABLE = 1, /* the host has no controller source configured */
    PAD_PORT_ERROR = 2        /* the source exists but failed */
} pad_port_status;

/* Capabilities of one pad, as the host knows them. Deliberately coarse: B3's firmware
 * contract reads no capability struct, so the only capability fact the
 * native side needs is whether a pad exists and whether it can accept a port setting. The
 * struct is here so a Horizon HID adapter can fill it later without changing the service. */
typedef struct pad_capability {
    uint32_t exists;              /* 0 = nothing plugged into this port */
    uint32_t supports_press_mode; /* the pad has a pressure-sensitive path */
    uint32_t supports_sensor_mode;/* the pad has a motion-source path */
} pad_capability;

/* The injected port.
 *
 *   pad_count   writes how many pads the host currently reports. This is what
 *               `cellPadInit`'s argument must be able to cover (CONTRACT.md §2.1 is a
 *               range check on that argument, not a comparison against this value: the
 *               firmware never reads a host count). UNAVAILABLE means "no controller
 *               source"; the caller then supplies `pad_assumed_count` instead.
 *   capability  NOT_FOUND/UNAVAILABLE when the port is empty, otherwise fills `out`.
 *               `port_no` is [0, PAD_PORT_MAX].
 *
 * All three function pointers may be NULL. A NULL `pad_count` means "the host cannot say",
 * in which case the service falls back to `pad_assumed_count`; a NULL `capability` means
 * "no pad on any port". */
typedef struct pad_host_state {
    uint32_t connected;  /* 0 = nothing on this port */
    uint16_t digital1;   /* PAD_CTRL_SELECT..PAD_CTRL_LEFT (button word 2) */
    uint16_t digital2;   /* PAD_CTRL_L2..PAD_CTRL_SQUARE (button word 3) */
    uint8_t right_x;     /* 0..255, PAD_ANALOG_CENTRE is centre */
    uint8_t right_y;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t l2_analog;   /* 0..255 */
    uint8_t r2_analog;   /* 0..255 */
} pad_host_state;

typedef struct pad_port {
    void *context;
    pad_port_status (*pad_count)(void *context, uint32_t *out_count);
    pad_port_status (*capability)(void *context, uint32_t port_no, pad_capability *out);
    /* What the host controller on `port_no` looks like right now, already in PS3 terms.
     * NULL means "this host reports no pad state": every reader then sees an empty port. */
    pad_port_status (*read_state)(void *context, uint32_t port_no, pad_host_state *out);
} pad_port;

/* ------------------------------------------------------------------------------------ */
/* Service status                                                                        */
/* ------------------------------------------------------------------------------------ */

/* Distinct from the guest return value, which is a PAD_* code or 0. */
typedef enum pad_status {
    PAD_SERVICE_OK = 0,
    PAD_SERVICE_BAD_ARGUMENT = 1, /* NULL output, invalid argument, port out of range */
    PAD_SERVICE_PORT_ERROR = 2    /* the injected port failed where its answer was needed */
} pad_status;

/* ------------------------------------------------------------------------------------ */
/* Per-call state                                                                        */
/* ------------------------------------------------------------------------------------ */

/* Per-port service state for the three reader entries. `pad_state` keeps one of these for
 * every port a `CellPadData`/`CellPadInfo2` word array can describe. */
typedef struct pad_reader_port {
    uint32_t setting;         /* the raw value the last pad_set_port_setting stored */
    pad_host_state reported;  /* the last host state handed to the guest */
    uint32_t has_reported;    /* 1 once `reported` holds a state that was written out */
    uint32_t reported_connected; /* the connection flag last announced, 0 = never had a pad */
} pad_reader_port;

/* Instance-owned. The caller allocates it, the service only writes it. This mirrors the
 * module-static pair `cellPadInit` keeps plus the state object's
 * named fields, without any global storage. */
typedef struct pad_state {
    uint32_t initialized;   /* the module flag: 0 before a successful init, 1 after */
    uint32_t max_pads;      /* state+0x76F4: the count `cellPadInit` accepted */
    uint32_t lock_held;     /* state+0x7708 lock, local to a call; never left set */
    uint32_t attached_pads; /* state+0x0A70: pads the host reported at init time */
    uint32_t sc512_flag;    /* state+0x7704: reserved; see pad_init's note */
    uint8_t records[PAD_RECORD_STRIDE]; /* record 0's first bytes, for inspection only */
    /* Appended: one entry per port 0..PAD_INFO_PORT_COUNT-1. */
    pad_reader_port reader[PAD_INFO_PORT_COUNT];
} pad_state;

/* What one `pad_set_port_setting` call decided to do, so the adapter knows what to copy
 * out and which syscall (if any) to replay. */
typedef struct pad_setting_result {
    uint32_t applied;      /* 1 when the two record writes happened, 0 on any failure */
    uint32_t field_a;      /* record+0x1C value written: bit 1 of the setting, 0 or 1 */
    uint32_t field_b;      /* record+0x20 value written: bit 2 of the setting, 0 or 1 */
    uint32_t record_offset;/* state-relative offset of the record, PAD_RECORD_BASE+232*n */
    uint32_t syscall_needed;  /* 1 when the record's +0x18 gate bit asked for syscall 503 */
    uint32_t syscall_number;  /* PAD_SYSCALL_RECORD_WRITE when syscall_needed, else 0 */
    uint32_t syscall_subop;   /* PAD_SYSCALL_SUBOP_A / _B, whichever the gate selected */
} pad_setting_result;

/* ------------------------------------------------------------------------------------ */
/* cellPadInit (0x1CF98800)                                                              */
/* ------------------------------------------------------------------------------------ */

/* The accepted-argument predicate of CONTRACT.md §2.1. Returns 1 when `count` is in
 * [1, PAD_MAX_PADS_ARG], 0 otherwise. Pure; no port access. */
int pad_init_argument_valid(uint32_t count);

/* The value `cellPadInit` returns *without* consulting the port at all: the
 * already-initialized short circuit at 0x248C..0x24A0.
 *
 * Returns PAD_ERROR_ALREADY_INITIALIZED when `state->initialized` is non-zero, else
 * PAD_OK. This is exactly what the firmware decides at 0x2484..0x24A0 before it touches
 * anything else, and it is the branch inFAMOUS actually takes on a second init. */
int32_t pad_init_guard(const pad_state *state);

/* Runs `cellPadInit(count)`.
 *
 * `state_out` and `state_bytes` receive the byte image of the fields this contract names,
 * written big-endian. Only a non-NULL `state_out` with `state_bytes >= PAD_STATE_BYTES` is
 * filled in full; a smaller buffer is filled as far as it goes and the call still
 * succeeds, so a caller can inspect just the head. Pass NULL/0 to skip it.
 *
 * `result_out` receives the guest return value (`r3`, sign-extended).
 *
 * Order of operations, matching the firmware:
 *   1. already initialized           -> PAD_ERROR_ALREADY_INITIALIZED, no state change.
 *   2. count outside [1, 0x7F]       -> PAD_ERROR_INVALID_ARGUMENT, no state change.
 *   3. the port is asked for a pad count. UNAVAILABLE/A NULL port falls back to
 *      `pad_assumed_count`; a hard port failure returns PAD_SERVICE_PORT_ERROR with
 *      PAD_ERROR_NO_MEMORY in `result_out` (the firmware's allocation-failure code, which
 *      is the arm a host with no controller source would take).
 *   4. max_pads = count, sc512_flag = 0, attached_pads = the host count, initialized = 1,
 *      result 0.
 *
 * `pad_assumed_count` is the caller's decision about a host with no controller source. It
 * is only consulted when the port cannot answer. */
pad_status pad_init(pad_state *state, const pad_port *port, uint32_t count,
                    uint32_t pad_assumed_count, uint8_t *state_out, size_t state_bytes,
                    int32_t *result_out);

/* Builds the PAD_STATE_BYTES image of `state` alone. Returns the bytes
 * written, which is `min(cap, PAD_STATE_BYTES)`. Nothing is written past `cap`. */
size_t pad_state_image(const pad_state *state, uint8_t *out, size_t cap);

/* `cellPadEnd` (0x4D9B75D5, CONTRACT.md §2.7) reduced to the state this service owns:
 * clears `initialized`, `max_pads`, `attached_pads` and the record image. Returns 0, the
 * value the firmware leaves in r3. */
int32_t pad_end(pad_state *state);

/* ------------------------------------------------------------------------------------ */
/* cellPadSetPortSetting (0x578E3C98)                                                    */
/* ------------------------------------------------------------------------------------ */

/* Runs `cellPadSetPortSetting(port_no, setting)` (CONTRACT.md §3.1..§3.5).
 *
 * The caller passes the setting exactly as the guest did (`r4` zero-extended). Bits 1 and
 * 2 are read; every other bit is ignored, as in the firmware.
 *
 * Order of operations, matching 0x18F4 and 0x33F8:
 *   1. `state->initialized == 0`     -> PAD_ERROR_UNINITIALIZED (0x1920).
 *   2. `port_no > PAD_PORT_MAX`      -> PAD_ERROR_INVALID_ARGUMENT (0x3514).
 *   3. otherwise record field A = bit1(setting), field B = bit2(setting), result 0.
 *      The record's own +0x18 gate byte decides whether the adapter must replay
 *      syscall 503; `result` reports that, because only the adapter knows whether the
 *      gate is set on the real record.
 *
 * The firmware's lock acquire (0xC3AC) cannot fail on a single-threaded host, so
 * PAD_ERROR_INVALID_ALIGNMENT is not produced here; it is named for completeness.
 *
 * `result_out` is optional. `record_out`, when non-NULL, receives the record's `+0x1C` and
 * `+0x20` words as big-endian bytes at offsets 4 and 8 of a 12-byte buffer, with the
 * `+0x18` gate byte at offset 0; `record_bytes` must be >= 12 for that. Pass NULL/0 to
 * skip it.
 *
 * The port is consulted only to confirm the pad exists when the caller asks for it via
 * `state->attached_pads`; on the game's actual path (ports 0 and 1, setting 6) the firmware
 * itself never reads a capability, so this function returns the same codes with or without
 * a port. It is here so a Horizon HID adapter has one place to say "no pad on port 1". */
pad_status pad_set_port_setting(pad_state *state, const pad_port *port, uint32_t port_no,
                                uint32_t setting, int32_t *result_out, uint8_t *record_out,
                                size_t record_bytes, pad_setting_result *detail_out);

/* The gate predicate of CONTRACT.md §3.3: the record writer only issues syscall 503 when
 * the *record's* `+0x18` flags byte has the matching bit set. `gate_flags` is that byte as
 * read from the record; `subop` selects PAD_SYSCALL_SUBOP_A (bit 1) or _B (bit 2). */
int pad_record_gate_open(uint8_t gate_flags, uint32_t subop);

/* ------------------------------------------------------------------------------------ */
/* cellPadGetInfo2 / cellPadGetData / cellPadSetActDirect                                */
/* ------------------------------------------------------------------------------------ */

/* Runs `cellPadGetInfo2(&info)`.
 *
 * 1. `state->initialized == 0`        -> PAD_ERROR_UNINITIALIZED, nothing written.
 * 2. NULL `info_out` or too short      -> PAD_SERVICE_BAD_ARGUMENT, INVALID_PARAMETER.
 * 3. `info_out` receives the PAD_INFO2_BYTES big-endian `CellPadInfo2` image; `result_out`
 *    receives 0.
 *
 * A port counts as connected when `read_state` exists, answers PAD_PORT_OK and reports
 * connected != 0. `PAD_STATUS_BIT_ASSIGN_CHANGES` tracks an edge on that flag, which is
 * remembered per port and starts out clear ("not connected", set by `pad_init`): a pad
 * present on the first report shows it once, a disconnect shows it once, and a port that
 * never held a pad never shows it. */
pad_status pad_get_info2(pad_state *state, const pad_port *port, uint8_t *info_out,
                         size_t info_bytes, int32_t *result_out);

/* Runs `cellPadGetData(port_no, &data)`.
 *
 * 1. `state->initialized == 0`                     -> UNINITIALIZED.
 * 2. `port_no >= min(max_pads, PAD_READ_PORT_MAX)` -> INVALID_PARAMETER.
 * 3. NULL `data_out` or too short                  -> BAD_ARGUMENT, INVALID_PARAMETER.
 * 4. the port is not connected                     -> NO_DEVICE, nothing written.
 * 5. otherwise `data_out` receives the PAD_DATA_BYTES big-endian `CellPadData` image with
 *    len and the words the port's setting selects (8, 20 or 24 entries), result 0.
 *
 * A state identical to the one last reported for this port yields len 0 ("nothing new"),
 * which is what the hardware does and what titles drain on. */
pad_status pad_get_data(pad_state *state, const pad_port *port, uint32_t port_no,
                        uint8_t *data_out, size_t data_bytes, int32_t *result_out);

/* Runs `cellPadSetActDirect(port_no, param)`.
 *
 * 1. uninitialised / `port_no` out of range / `has_param == 0` -> INVALID_PARAMETER (the
 *    firmware answers NOT_OPENED for the first, which the pad_error for "not opened"
 *    shares with the reader entries: see pad_service.c).
 * 2. port not connected -> NO_DEVICE.
 * 3. otherwise 0. The service drives no actuator yet, so nothing is written anywhere;
 *    `has_param` is the adapter's "the guest passed a non-NULL param" answer. */
pad_status pad_set_act_direct(pad_state *state, const pad_port *port, uint32_t port_no,
                              int has_param, int32_t *result_out);

#ifdef __cplusplus
}
#endif

#endif /* PAD_SERVICE_H */
