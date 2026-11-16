#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H
/* Platform-free, instance-owned audio service for the thirteen audio imports the game
 * makes:
 *
 *   cellAudioOutGetSoundAvailability  cellAudioInit            cellAudioPortStop
 *   cellAudioOutGetNumberOfDevice     cellAudioQuit            cellAudioPortClose
 *   cellAudioOutGetDeviceInfo         cellAudioPortOpen        cellAudioGetPortConfig
 *   cellAudioOutGetState              cellAudioPortStart       cellAudioGetPortBlockTag
 *   cellAudioOutConfigure
 *
 * The game imports no notify-event-queue and no timestamp entry, so it follows playback by
 * reading a port's read index and block tags. This service therefore models the port
 * lifecycle plus a block clock, and nothing else.
 *
 * Provenance: every constant and layout below is the coordinator's recollection of the
 * public Sony SDK headers / RPCS3, not extracted from firmware.
 *
 * The service owns no globals, allocates nothing, makes no OS calls and never touches guest
 * memory: the caller supplies the guest addresses it should report through
 * `audio_port_layout`, every value the firmware would write is produced as a big-endian
 * byte image the adapter copies out, and time arrives as a `now_us` argument.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* Guest values                                                                          */
/* ------------------------------------------------------------------------------------ */

#define AUDIO_OK 0

/* cellAudio error codes. The firmware leaves these in r3 as a negative int32, so each
 * macro carries the same sign as the guest sees. */
#define AUDIO_ERROR_ALREADY_INIT ((int32_t)0x80310701)    /* CELL_AUDIO_ERROR_ALREADY_INIT */
#define AUDIO_ERROR_NOT_INIT ((int32_t)0x80310703)        /* CELL_AUDIO_ERROR_NOT_INIT */
#define AUDIO_ERROR_PARAM ((int32_t)0x80310704)           /* CELL_AUDIO_ERROR_PARAM */
#define AUDIO_ERROR_PORT_FULL ((int32_t)0x80310705)       /* CELL_AUDIO_ERROR_PORT_FULL */
#define AUDIO_ERROR_PORT_ALREADY_RUN ((int32_t)0x80310706) /* CELL_AUDIO_ERROR_PORT_ALREADY_RUN */
#define AUDIO_ERROR_PORT_NOT_OPEN ((int32_t)0x80310707)   /* CELL_AUDIO_ERROR_PORT_NOT_OPEN */
#define AUDIO_ERROR_PORT_NOT_RUN ((int32_t)0x80310708)    /* CELL_AUDIO_ERROR_PORT_NOT_RUN */

/* cellAudioOut error codes. */
#define AUDIO_OUT_ERROR_ILLEGAL_PARAMETER ((int32_t)0x8002b242)   /* CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER */
#define AUDIO_OUT_ERROR_DEVICE_NOT_FOUND ((int32_t)0x8002b244)    /* CELL_AUDIO_OUT_ERROR_DEVICE_NOT_FOUND */
#define AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT ((int32_t)0x8002b245) /* UNSUPPORTED_AUDIO_OUT */

/* Port status words (CELL_AUDIO_STATUS_*). */
#define AUDIO_STATUS_READY 1u
#define AUDIO_STATUS_RUN 2u
/* What cellAudioGetPortConfig reports for a port that is not open. */
#define AUDIO_STATUS_CLOSED 0x1010u

/* Device ids. */
#define AUDIO_OUT_PRIMARY 0u
#define AUDIO_OUT_SECONDARY 1u

/* Coding types (CELL_AUDIO_OUT_CODING_TYPE_*). */
#define AUDIO_CODING_TYPE_LPCM 0u

/* Sampling rates (CELL_AUDIO_OUT_FS_*): a bit mask, not an index. */
#define AUDIO_FS_44KHZ 0x02u
#define AUDIO_FS_48KHZ 0x04u

/* Channel counts a port may be opened with (CELL_AUDIO_PORT_*CH). */
#define AUDIO_CHANNEL_2 2u
#define AUDIO_CHANNEL_6 6u
#define AUDIO_CHANNEL_8 8u

/* Block counts a port may be opened with (CELL_AUDIO_BLOCK_*). */
#define AUDIO_BLOCKS_8 8u
#define AUDIO_BLOCKS_16 16u
#define AUDIO_BLOCKS_32 32u

/* Device info image (CellAudioOutDeviceInfo). */
#define AUDIO_OUT_PORT_TYPE_HDMI 0u
#define AUDIO_OUT_DEVICE_STATE_ENABLED 0u
#define AUDIO_OUT_MODE_COUNT 16u
#define AUDIO_OUT_MODE_BYTES 8u
#define AUDIO_OUT_DEVICE_LATENCY 1000u
#define AUDIO_OUT_SPEAKER_LAYOUT_2CH 1u
/* CellAudioOutDeviceInfo: portType, availableModeCount, state, 3 reserved, latency, then
 * 16 modes: 8 + 16*8 = 136. */
#define AUDIO_OUT_DEVICE_INFO_BYTES 136u
/* CellAudioOutState: state, encoder, 6 reserved, downMixer, then one mode: 12 + 8 = 20. */
#define AUDIO_OUT_STATE_BYTES 20u

/* CellAudioPortConfig: readIndexAddr u32, status u32, nChannel u64, nBlock u64,
 * portSize u32, portAddr u32 = 32 bytes. */
#define AUDIO_PORT_CONFIG_BYTES 32u

#define AUDIO_PORT_COUNT 8u
#define AUDIO_BLOCK_SAMPLES 256u
#define AUDIO_SAMPLE_RATE 48000u
/* Bytes per port buffer at the largest accepted shape: 8 ch * 32 blocks * 256 * 4. */
#define AUDIO_PORT_MAX_BYTES (AUDIO_CHANNEL_8 * AUDIO_BLOCKS_32 * AUDIO_BLOCK_SAMPLES * \
                              (uint32_t)sizeof(float))
/* Blocks per microsecond as the tick's exact rational factor: 48000 blocks/s over
 * 1000000 us/s reduces to 3/16000, so counter = (now - start) * 3 / 16000. */
#define AUDIO_TICK_NUMERATOR 3u
#define AUDIO_TICK_DENOMINATOR 16000u

/* ------------------------------------------------------------------------------------ */
/* Import identity                                                                       */
/* ------------------------------------------------------------------------------------ */

/* One call counter per import, in the order the game lifts them. */
typedef enum audio_import {
    AUDIO_IMPORT_OUT_SOUND_AVAILABILITY = 0,
    AUDIO_IMPORT_OUT_NUMBER_OF_DEVICE,
    AUDIO_IMPORT_OUT_DEVICE_INFO,
    AUDIO_IMPORT_OUT_STATE,
    AUDIO_IMPORT_OUT_CONFIGURE,
    AUDIO_IMPORT_INIT,
    AUDIO_IMPORT_QUIT,
    AUDIO_IMPORT_PORT_OPEN,
    AUDIO_IMPORT_PORT_START,
    AUDIO_IMPORT_PORT_STOP,
    AUDIO_IMPORT_PORT_CLOSE,
    AUDIO_IMPORT_GET_PORT_CONFIG,
    AUDIO_IMPORT_GET_PORT_BLOCK_TAG,
    AUDIO_IMPORT_COUNT
} audio_import;

/* ------------------------------------------------------------------------------------ */
/* State                                                                                 */
/* ------------------------------------------------------------------------------------ */

/* Given by the adapter: where the guest sees port data. The service never reads or writes
 * these addresses, it only reports them in CellAudioPortConfig. */
typedef struct audio_port_layout {
    uint32_t index_base;    /* guest address of 8 consecutive be64 read-index words */
    uint32_t buffer_base;   /* guest address of port 0's buffer */
    uint32_t buffer_stride; /* bytes reserved per port; must hold AUDIO_PORT_MAX_BYTES */
} audio_port_layout;

typedef struct audio_port {
    uint32_t open;
    uint32_t running;
    uint32_t channels; /* as accepted at open */
    uint32_t blocks;   /* as accepted at open */
    uint64_t attr;         /* stored, not interpreted */
    uint32_t level_bits;   /* the guest's float level, raw; stored, not interpreted */
    uint64_t start_us;     /* clock value at the last start */
    uint64_t counter;      /* blocks played since the last start */
    uint64_t start_calls;
    uint64_t stop_calls;
    uint64_t read_index;      /* the index audio_tick last reported: counter % blocks */
} audio_port;

typedef struct audio_service {
    uint32_t initialized;
    audio_port_layout layout;
    audio_port ports[AUDIO_PORT_COUNT];
    uint64_t calls[AUDIO_IMPORT_COUNT];
} audio_service;

/* Zero-fills the service and stores the layout. A NULL layout stores an all-zero one. */
void audio_service_init(audio_service *service, const audio_port_layout *layout);

/* ------------------------------------------------------------------------------------ */
/* cellAudioOut* configuration queries                                                   */
/* ------------------------------------------------------------------------------------ */

/* Runs `cellAudioOutGetSoundAvailability(audioOut, type, fs, option)`.
 *
 * Only PRIMARY and only LPCM at 48 kHz are available, with two channels; there is no
 * variant that is available with zero channels, so the answer for anything else is 0 and
 * never a negative error. `audio_out != PRIMARY` -> UNSUPPORTED_AUDIO_OUT. */
int32_t audio_out_get_sound_availability(audio_service *service, uint32_t audio_out,
                                         uint32_t type, uint32_t fs, uint32_t option);

/* Runs `cellAudioOutGetNumberOfDevice(audioOut)`: 1 for PRIMARY, else
 * UNSUPPORTED_AUDIO_OUT. */
int32_t audio_out_get_number_of_device(audio_service *service, uint32_t audio_out);

/* Runs `cellAudioOutGetDeviceInfo(audioOut, deviceIndex, &info)`.
 *
 * 1. `audio_out != PRIMARY`        -> UNSUPPORTED_AUDIO_OUT, nothing written.
 * 2. `device_index != 0`           -> DEVICE_NOT_FOUND, nothing written.
 * 3. NULL `info_out`               -> ILLEGAL_PARAMETER, nothing written.
 * 4. `info_out` receives the AUDIO_OUT_DEVICE_INFO_BYTES image: one HDMI device, enabled,
 *    latency 1000 us, one available mode (LPCM, 2 ch, 48 kHz, layout 2ch) and fifteen
 *    zeroed modes. Result 0.
 *
 * The device table is static, so the image does not depend on `service`. */
int32_t audio_out_get_device_info(audio_service *service, uint32_t audio_out,
                                  uint32_t device_index, uint8_t *info_out);

/* Runs `cellAudioOutGetState(audioOut, deviceIndex, &state)` with the same two argument
 * rules, then writes the AUDIO_OUT_STATE_BYTES image: state enabled, encoder LPCM, no down
 * mixer, one sound mode as above. NULL `state_out` is ILLEGAL_PARAMETER. Result 0. */
int32_t audio_out_get_state(audio_service *service, uint32_t audio_out, uint32_t device_index,
                            uint8_t *state_out);

/* Runs `cellAudioOutConfigure(audioOut, config, option, optionSize)` reduced to what the
 * firmware does on this console: the single fixed mode is already in force, so a
 * configuration is accepted and nothing is stored. `has_config == 0` is the adapter saying
 * the guest passed a NULL configuration, which the library rejects. */
int32_t audio_out_configure(audio_service *service, uint32_t audio_out, int has_config);

/* ------------------------------------------------------------------------------------ */
/* cellAudio port lifecycle                                                              */
/* ------------------------------------------------------------------------------------ */

/* Runs `cellAudioInit()`: ALREADY_INIT when already initialised, else set the flag. */
int32_t audio_init(audio_service *service);

/* Runs `cellAudioQuit()`: NOT_INIT when not initialised, else every port is closed and the
 * flag cleared. */
int32_t audio_quit(audio_service *service);

/* Opens a port. Order:
 *   1. not initialised                              -> NOT_INIT.
 *   2. `n_channel` not in {2, 6, 8} or `n_block`
 *      not in {8, 16, 32}                           -> PARAM.
 *   3. NULL `out_port_num`                          -> PARAM.
 *   4. no free port                                 -> PORT_FULL.
 *   5. otherwise the lowest free port becomes open and stopped, `*out_port_num` receives its
 *      number, result 0. `attr` and `level_bits` are stored and never interpreted. */
int32_t audio_port_open(audio_service *service, uint32_t n_channel, uint32_t n_block,
                        uint64_t attr, uint32_t level_bits, uint32_t *out_port_num);

/* Runs `cellAudioPortStart(port)`.
 *   1. not initialised -> NOT_INIT.  2. `port >= 8` -> PARAM.
 *   3. not open        -> PORT_NOT_OPEN.  4. already running -> PORT_ALREADY_RUN.
 *   5. otherwise running with `start_us = now_us` and `counter = 0`, result 0.
 *
 * `now_us` is the caller's monotonic microsecond clock. It is only read here and in
 * audio_tick, so a test drives the block clock without any real time passing. */
int32_t audio_port_start(audio_service *service, uint32_t port, uint64_t now_us);

/* Runs `cellAudioPortStop(port)`.
 *   1. not initialised -> NOT_INIT.  2. `port >= 8` -> PARAM.
 *   3. not open        -> PORT_NOT_OPEN.  4. not running -> PORT_NOT_RUN.
 *   5. otherwise stopped; the counter keeps its last value, result 0. */
int32_t audio_port_stop(audio_service *service, uint32_t port);

/* Runs `cellAudioPortClose(port)`.
 *   1. not initialised -> NOT_INIT.  2. `port >= 8` -> PARAM.
 *   3. not open        -> PORT_NOT_OPEN.
 *   4. otherwise closed, also when running, result 0. The counter is cleared. */
int32_t audio_port_close(audio_service *service, uint32_t port);

/* Runs `cellAudioGetPortConfig(port, &config)`.
 *   1. not initialised -> NOT_INIT.
 *   2. `port >= 8`     -> PARAM.
 *   3. NULL `config_out` -> PARAM.
 *   4. an open port writes the AUDIO_PORT_CONFIG_BYTES image described below; a closed one
 *      writes status AUDIO_STATUS_CLOSED and zeros in every other field. Both return 0.
 *
 * Image, big-endian at the offsets of CellAudioPortConfig:
 *   +0  readIndexAddr = layout.index_base + 8*port
 *   +4  status        = RUN (2) running, READY (1) open and stopped, CLOSED (0x1010) closed
 *   +8  nChannel      = the value accepted at open
 *   +16 nBlock        = the value accepted at open
 *   +24 portSize      = channels * blocks * 256 * 4
 *   +28 portAddr      = layout.buffer_base + port * layout.buffer_stride
 *
 * A port never started but open reports READY: the game drains on status and treats CLOSED
 * as "no port here", so the closed status is the one that must be distinct. */
int32_t audio_get_port_config(audio_service *service, uint32_t port, uint8_t *config_out);

/* Runs `cellAudioGetPortBlockTag(port, blockNo, &tag)`.
 *   1. not initialised -> NOT_INIT.  2. `port >= 8` -> PARAM.
 *   3. `block_no >= blocks` (the count accepted at open) -> PARAM.
 *   4. not open -> PORT_NOT_OPEN.  5. NULL `tag_out` -> PARAM.  6. not running -> PORT_NOT_RUN.
 *   7. `tag_out` receives the block's tag, result 0.
 *
 * With `c = counter` and `i = c % blocks` the block being played now, block number `n` was
 * played `(i + blocks - n) % blocks` blocks ago:
 *   tag = c - ((i + blocks - n) % blocks), clamped at 0.
 * So the current block's tag is `c` and the block before it `c - 1`. At counter 0 every
 * block would subtract, so they all clamp to 0 rather than wrapping below the start. */
int32_t audio_get_port_block_tag(audio_service *service, uint32_t port, uint64_t block_no,
                                 uint64_t *tag_out);

/* The block clock. For every running port
 *   counter = (now_us - start_us) * 3 / 16000
 * (48000 / 256 blocks per second, exact integer math) and the counter never decreases, so a
 * clock that jumps backwards leaves it alone. Ports that are not running keep their last
 * counter.
 *
 * `read_index_out`, when non-NULL, receives one entry per port: `counter % blocks` for an
 * open port, 0 for a closed one. `read_index_bytes` says how many entries the array holds;
 * an array shorter than 8 is filled as far as it goes. The adapter stores them into the
 * guest read-index words itself; the service never touches guest memory.
 *
 * Returns a bit mask of the ports whose read index differs from before this call. */
uint32_t audio_tick(audio_service *service, uint64_t now_us, uint64_t *read_index_out,
                    size_t read_index_bytes);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SERVICE_H */
