/* Platform-free audio service. See audio_service.h for the contract, the guest values and
 * their provenance.
 *
 * No globals, no allocation, no OS calls: every byte the service hands back is written into a
 * caller-provided buffer, the guest addresses it reports come from the injected
 * `audio_port_layout`, and time arrives as a `now_us` argument.
 */
#include "audio_service.h"

#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Guest byte accessors                                                                  */
/* ------------------------------------------------------------------------------------ */

static void store_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void store_be64(uint8_t *dst, uint64_t value)
{
    store_be32(dst, (uint32_t)(value >> 32));
    store_be32(dst + 4, (uint32_t)value);
}

/* The sound mode every device of this console reports: LPCM, two channels, 48 kHz, 2ch
 * speaker layout. */
static void store_sound_mode(uint8_t *dst)
{
    dst[0] = (uint8_t)AUDIO_CODING_TYPE_LPCM;
    dst[1] = (uint8_t)AUDIO_CHANNEL_2;
    dst[2] = (uint8_t)AUDIO_FS_48KHZ;
    dst[3] = 0u; /* reserved */
    store_be32(dst + 4, AUDIO_OUT_SPEAKER_LAYOUT_2CH);
}

/* ------------------------------------------------------------------------------------ */
/* Argument predicates                                                                   */
/* ------------------------------------------------------------------------------------ */

static int is_accepted_channel_count(uint32_t n_channel)
{
    return n_channel == AUDIO_CHANNEL_2 || n_channel == AUDIO_CHANNEL_6 ||
           n_channel == AUDIO_CHANNEL_8;
}

static int is_accepted_block_count(uint32_t n_block)
{
    return n_block == AUDIO_BLOCKS_8 || n_block == AUDIO_BLOCKS_16 ||
           n_block == AUDIO_BLOCKS_32;
}

static int is_port_number(uint32_t port)
{
    return port < AUDIO_PORT_COUNT;
}

/* PRIMARY is the only device the console reports; everything else is a device id the
 * library rejects before it looks at any buffer. */
static int is_primary_device(uint32_t audio_out)
{
    return audio_out == AUDIO_OUT_PRIMARY;
}

/* ------------------------------------------------------------------------------------ */
/* cellAudioOut* configuration queries                                                   */
/* ------------------------------------------------------------------------------------ */

int32_t audio_out_get_sound_availability(audio_service *service, uint32_t audio_out,
                                         uint32_t type, uint32_t fs, uint32_t option)
{
    (void)option;
    if (service == NULL) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    service->calls[AUDIO_IMPORT_OUT_SOUND_AVAILABILITY]++;
    if (!is_primary_device(audio_out)) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    if (type != AUDIO_CODING_TYPE_LPCM || fs != AUDIO_FS_48KHZ) {
        return 0;
    }
    return (int32_t)AUDIO_CHANNEL_2;
}

int32_t audio_out_get_number_of_device(audio_service *service, uint32_t audio_out)
{
    if (service == NULL) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    service->calls[AUDIO_IMPORT_OUT_NUMBER_OF_DEVICE]++;
    if (!is_primary_device(audio_out)) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    return 1;
}

int32_t audio_out_get_device_info(audio_service *service, uint32_t audio_out,
                                  uint32_t device_index, uint8_t *info_out)
{
    if (service == NULL) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    service->calls[AUDIO_IMPORT_OUT_DEVICE_INFO]++;
    if (!is_primary_device(audio_out)) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    if (device_index != 0u) {
        return AUDIO_OUT_ERROR_DEVICE_NOT_FOUND;
    }
    if (info_out == NULL) {
        return AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    }

    memset(info_out, 0, AUDIO_OUT_DEVICE_INFO_BYTES);
    info_out[0] = (uint8_t)AUDIO_OUT_PORT_TYPE_HDMI;
    info_out[1] = 1u; /* availableModeCount: the one mode below */
    info_out[2] = (uint8_t)AUDIO_OUT_DEVICE_STATE_ENABLED;
    /* +3..+5 reserved and already zero. */
    info_out[6] = (uint8_t)(AUDIO_OUT_DEVICE_LATENCY >> 8);
    info_out[7] = (uint8_t)AUDIO_OUT_DEVICE_LATENCY;
    /* mode 0 at +8, the fifteen remaining modes stay zero. */
    store_sound_mode(info_out + 8u);
    return AUDIO_OK;
}

int32_t audio_out_get_state(audio_service *service, uint32_t audio_out, uint32_t device_index,
                            uint8_t *state_out)
{
    if (service == NULL) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    service->calls[AUDIO_IMPORT_OUT_STATE]++;
    if (!is_primary_device(audio_out)) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    if (device_index != 0u) {
        return AUDIO_OUT_ERROR_DEVICE_NOT_FOUND;
    }
    if (state_out == NULL) {
        return AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    }

    memset(state_out, 0, AUDIO_OUT_STATE_BYTES);
    state_out[0] = (uint8_t)AUDIO_OUT_DEVICE_STATE_ENABLED;
    state_out[1] = (uint8_t)AUDIO_CODING_TYPE_LPCM;
    /* +2..+7 reserved and already zero, +8 downMixer zero. */
    store_sound_mode(state_out + 12u);
    return AUDIO_OK;
}

int32_t audio_out_configure(audio_service *service, uint32_t audio_out, int has_config)
{
    if (service == NULL) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    service->calls[AUDIO_IMPORT_OUT_CONFIGURE]++;
    if (!is_primary_device(audio_out)) {
        return AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT;
    }
    if (!has_config) {
        return AUDIO_OUT_ERROR_ILLEGAL_PARAMETER;
    }
    return AUDIO_OK;
}

/* ------------------------------------------------------------------------------------ */
/* cellAudio port lifecycle                                                              */
/* ------------------------------------------------------------------------------------ */

void audio_service_init(audio_service *service, const audio_port_layout *layout)
{
    if (service == NULL) {
        return;
    }
    memset(service, 0, sizeof(*service));
    if (layout != NULL) {
        service->layout = *layout;
    }
}

int32_t audio_init(audio_service *service)
{
    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_INIT]++;
    if (service->initialized != 0u) {
        return AUDIO_ERROR_ALREADY_INIT;
    }
    service->initialized = 1u;
    return AUDIO_OK;
}

int32_t audio_quit(audio_service *service)
{
    uint32_t port;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_QUIT]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    for (port = 0u; port < AUDIO_PORT_COUNT; port++) {
        memset(&service->ports[port], 0, sizeof(service->ports[port]));
    }
    service->initialized = 0u;
    return AUDIO_OK;
}

int32_t audio_port_open(audio_service *service, uint32_t n_channel, uint32_t n_block,
                        uint64_t attr, uint32_t level_bits, uint32_t *out_port_num)
{
    audio_port *free_port = NULL;
    uint32_t port;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_PORT_OPEN]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_accepted_channel_count(n_channel) || !is_accepted_block_count(n_block)) {
        return AUDIO_ERROR_PARAM;
    }
    if (out_port_num == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    for (port = 0u; port < AUDIO_PORT_COUNT; port++) {
        if (service->ports[port].open == 0u) {
            free_port = &service->ports[port];
            break;
        }
    }
    if (free_port == NULL) {
        return AUDIO_ERROR_PORT_FULL;
    }

    *free_port = (audio_port){0};
    free_port->open = 1u;
    free_port->running = 0u;
    free_port->channels = n_channel;
    free_port->blocks = n_block;
    free_port->attr = attr;
    free_port->level_bits = level_bits;
    *out_port_num = port;
    return AUDIO_OK;
}

int32_t audio_port_start(audio_service *service, uint32_t port, uint64_t now_us)
{
    audio_port *target;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_PORT_START]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_port_number(port)) {
        return AUDIO_ERROR_PARAM;
    }
    target = &service->ports[port];
    if (target->open == 0u) {
        return AUDIO_ERROR_PORT_NOT_OPEN;
    }
    if (target->running != 0u) {
        return AUDIO_ERROR_PORT_ALREADY_RUN;
    }

    target->running = 1u;
    target->start_us = now_us;
    target->counter = 0u;
    target->start_calls++;
    return AUDIO_OK;
}

int32_t audio_port_stop(audio_service *service, uint32_t port)
{
    audio_port *target;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_PORT_STOP]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_port_number(port)) {
        return AUDIO_ERROR_PARAM;
    }
    target = &service->ports[port];
    if (target->open == 0u) {
        return AUDIO_ERROR_PORT_NOT_OPEN;
    }
    if (target->running == 0u) {
        return AUDIO_ERROR_PORT_NOT_RUN;
    }

    target->running = 0u;
    target->stop_calls++;
    return AUDIO_OK;
}

int32_t audio_port_close(audio_service *service, uint32_t port)
{
    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_PORT_CLOSE]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_port_number(port)) {
        return AUDIO_ERROR_PARAM;
    }
    if (service->ports[port].open == 0u) {
        return AUDIO_ERROR_PORT_NOT_OPEN;
    }

    memset(&service->ports[port], 0, sizeof(service->ports[port]));
    return AUDIO_OK;
}

int32_t audio_get_port_config(audio_service *service, uint32_t port, uint8_t *config_out)
{
    const audio_port *target;
    uint32_t status;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_GET_PORT_CONFIG]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_port_number(port)) {
        return AUDIO_ERROR_PARAM;
    }
    if (config_out == NULL) {
        return AUDIO_ERROR_PARAM;
    }

    target = &service->ports[port];
    status = AUDIO_STATUS_CLOSED;
    if (target->open != 0u) {
        status = target->running != 0u ? AUDIO_STATUS_RUN : AUDIO_STATUS_READY;
    }

    memset(config_out, 0, AUDIO_PORT_CONFIG_BYTES);
    store_be32(config_out + 0u, service->layout.index_base + 8u * port);
    store_be32(config_out + 4u, status);
    if (target->open != 0u) {
        store_be64(config_out + 8u, target->channels);
        store_be64(config_out + 16u, target->blocks);
        store_be32(config_out + 24u,
                   target->channels * target->blocks * AUDIO_BLOCK_SAMPLES *
                       (uint32_t)sizeof(float));
        store_be32(config_out + 28u, service->layout.buffer_base + port * service->layout.buffer_stride);
    }
    return AUDIO_OK;
}

int32_t audio_get_port_block_tag(audio_service *service, uint32_t port, uint64_t block_no,
                                 uint64_t *tag_out)
{
    const audio_port *target;
    uint64_t blocks_ago;
    uint64_t current_block;
    uint64_t tag;

    if (service == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    service->calls[AUDIO_IMPORT_GET_PORT_BLOCK_TAG]++;
    if (service->initialized == 0u) {
        return AUDIO_ERROR_NOT_INIT;
    }
    if (!is_port_number(port)) {
        return AUDIO_ERROR_PARAM;
    }
    target = &service->ports[port];
    if (target->open == 0u) {
        return AUDIO_ERROR_PORT_NOT_OPEN;
    }
    if (block_no >= (uint64_t)target->blocks) {
        return AUDIO_ERROR_PARAM;
    }
    if (tag_out == NULL) {
        return AUDIO_ERROR_PARAM;
    }
    if (target->running == 0u) {
        return AUDIO_ERROR_PORT_NOT_RUN;
    }

    current_block = target->counter % (uint64_t)target->blocks;
    blocks_ago = (current_block + (uint64_t)target->blocks - block_no) % (uint64_t)target->blocks;
    tag = target->counter >= blocks_ago ? target->counter - blocks_ago : 0u;
    *tag_out = tag;
    return AUDIO_OK;
}

uint32_t audio_tick(audio_service *service, uint64_t now_us, uint64_t *read_index_out,
                    size_t read_index_bytes)
{
    uint32_t changed = 0u;
    uint32_t port;

    if (service == NULL) {
        return 0u;
    }
    for (port = 0u; port < AUDIO_PORT_COUNT; port++) {
        audio_port *const target = &service->ports[port];
        uint64_t read_index = 0u;

        if (target->open == 0u) {
            if (read_index_out != NULL && port < read_index_bytes) {
                read_index_out[port] = 0u;
            }
            continue;
        }
        if (target->running != 0u && now_us > target->start_us) {
            const uint64_t elapsed_us = now_us - target->start_us;
            const uint64_t played =
                (elapsed_us / AUDIO_TICK_DENOMINATOR) * AUDIO_TICK_NUMERATOR +
                ((elapsed_us % AUDIO_TICK_DENOMINATOR) * AUDIO_TICK_NUMERATOR) /
                    AUDIO_TICK_DENOMINATOR;
            /* A backwards clock must not rewind playback. */
            if (played > target->counter) {
                target->counter = played;
            }
        }

        read_index = target->counter % (uint64_t)target->blocks;
        if (read_index != target->read_index && port < read_index_bytes) {
            changed |= 1u << port;
        }
        target->read_index = read_index;
        if (read_index_out != NULL && port < read_index_bytes) {
            read_index_out[port] = read_index;
        }
    }
    return changed;
}
