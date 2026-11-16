/* Host test for native/domain/audio_service.{h,c}: the cellAudioOut* answers, the cellAudio port
 * lifecycle and the block clock. No guest memory, no threads, no OS audio. */
#include "../domain/audio_service.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

static const audio_port_layout LAYOUT = {0x0E200000u, 0x0E210000u, 0x40000u};

static void test_out_queries(void)
{
    audio_service s;
    uint8_t buf[160];
    unsigned i;
    audio_service_init(&s, &LAYOUT);

    CHECK(audio_out_get_sound_availability(&s, 0, AUDIO_CODING_TYPE_LPCM, AUDIO_FS_48KHZ, 0) == 2);
    CHECK(audio_out_get_sound_availability(&s, 0, AUDIO_CODING_TYPE_LPCM, AUDIO_FS_44KHZ, 0) == 0);
    CHECK(audio_out_get_sound_availability(&s, 0, 1u, AUDIO_FS_48KHZ, 0) == 0);
    CHECK(audio_out_get_sound_availability(&s, 1, 0, AUDIO_FS_48KHZ, 0) == AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    CHECK(audio_out_get_number_of_device(&s, 0) == 1);
    CHECK(audio_out_get_number_of_device(&s, 1) == AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);

    memset(buf, 0xA5, sizeof buf);
    CHECK(audio_out_get_device_info(&s, 0, 0, buf) == 0);
    CHECK(buf[0] == 0 && buf[1] == 1 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0 && buf[5] == 0);
    CHECK(buf[6] == 0x03 && buf[7] == 0xE8);
    CHECK(buf[8] == 0 && buf[9] == 2 && buf[10] == 4 && buf[11] == 0 && be32(buf + 12) == 1);
    for (i = 16; i < 136; ++i) CHECK(buf[i] == 0);
    CHECK(buf[136] == 0xA5);
    memset(buf, 0xA5, sizeof buf);
    CHECK(audio_out_get_device_info(&s, 1, 0, buf) == AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    CHECK(audio_out_get_device_info(&s, 0, 1, buf) == AUDIO_OUT_ERROR_DEVICE_NOT_FOUND);
    CHECK(audio_out_get_device_info(&s, 0, 0, NULL) == AUDIO_OUT_ERROR_ILLEGAL_PARAMETER);
    CHECK(buf[0] == 0xA5);

    CHECK(audio_out_get_state(&s, 0, 0, buf) == 0);
    for (i = 0; i < 12; ++i) CHECK(buf[i] == 0);
    CHECK(buf[12] == 0 && buf[13] == 2 && buf[14] == 4 && buf[15] == 0 && be32(buf + 16) == 1);
    CHECK(buf[20] == 0xA5);
    CHECK(audio_out_get_state(&s, 0, 2, buf) == AUDIO_OUT_ERROR_DEVICE_NOT_FOUND);
    CHECK(audio_out_get_state(&s, 0, 0, NULL) == AUDIO_OUT_ERROR_ILLEGAL_PARAMETER);

    CHECK(audio_out_configure(&s, 0, 1) == 0);
    CHECK(audio_out_configure(&s, 0, 0) == AUDIO_OUT_ERROR_ILLEGAL_PARAMETER);
    CHECK(audio_out_configure(&s, 1, 1) == AUDIO_OUT_ERROR_UNSUPPORTED_AUDIO_OUT);
    CHECK(s.calls[AUDIO_IMPORT_OUT_SOUND_AVAILABILITY] == 4 && s.calls[AUDIO_IMPORT_OUT_CONFIGURE] == 3);
}

static void test_port_lifecycle(void)
{
    audio_service s;
    uint8_t cfg[40];
    uint32_t port = 99, i;
    audio_service_init(&s, &LAYOUT);

    CHECK(audio_port_open(&s, 2, 8, 0, 0, &port) == AUDIO_ERROR_NOT_INIT);
    CHECK(audio_quit(&s) == AUDIO_ERROR_NOT_INIT);
    CHECK(audio_get_port_config(&s, 0, cfg) == AUDIO_ERROR_NOT_INIT);
    CHECK(audio_init(&s) == 0);
    CHECK(audio_init(&s) == AUDIO_ERROR_ALREADY_INIT);

    CHECK(audio_port_open(&s, 3, 8, 0, 0, &port) == AUDIO_ERROR_PARAM);
    CHECK(audio_port_open(&s, 2, 7, 0, 0, &port) == AUDIO_ERROR_PARAM);
    CHECK(audio_port_open(&s, 2, 8, 0, 0, NULL) == AUDIO_ERROR_PARAM);
    CHECK(port == 99);
    for (i = 0; i < 8; ++i) {
        CHECK(audio_port_open(&s, i == 3 ? 8u : 2u, i == 3 ? 32u : 8u, 0x10, 0x3F800000u, &port) == 0);
        CHECK(port == i);
    }
    CHECK(audio_port_open(&s, 2, 8, 0, 0, &port) == AUDIO_ERROR_PORT_FULL);

    memset(cfg, 0xA5, sizeof cfg);
    CHECK(audio_get_port_config(&s, 0, cfg) == 0);
    CHECK(be32(cfg) == 0x0E200000u && be32(cfg + 4) == AUDIO_STATUS_READY);
    CHECK(be64(cfg + 8) == 2 && be64(cfg + 16) == 8 && be32(cfg + 24) == 16384u && be32(cfg + 28) == 0x0E210000u);
    CHECK(cfg[32] == 0xA5);
    CHECK(audio_get_port_config(&s, 3, cfg) == 0);
    CHECK(be32(cfg) == 0x0E200018u && be64(cfg + 8) == 8 && be64(cfg + 16) == 32);
    CHECK(be32(cfg + 24) == 262144u && be32(cfg + 28) == 0x0E2D0000u);
    CHECK(audio_get_port_config(&s, 8, cfg) == AUDIO_ERROR_PARAM);
    CHECK(audio_get_port_config(&s, 0, NULL) == AUDIO_ERROR_PARAM);

    CHECK(audio_port_stop(&s, 0) == AUDIO_ERROR_PORT_NOT_RUN);
    CHECK(audio_port_start(&s, 8, 0) == AUDIO_ERROR_PARAM);
    CHECK(audio_port_start(&s, 0, 1000) == 0);
    CHECK(audio_port_start(&s, 0, 1000) == AUDIO_ERROR_PORT_ALREADY_RUN);
    CHECK(audio_get_port_config(&s, 0, cfg) == 0 && be32(cfg + 4) == AUDIO_STATUS_RUN);
    CHECK(audio_port_stop(&s, 0) == 0);
    CHECK(audio_port_close(&s, 5) == 0);
    CHECK(audio_port_close(&s, 5) == AUDIO_ERROR_PORT_NOT_OPEN);
    CHECK(audio_port_start(&s, 5, 0) == AUDIO_ERROR_PORT_NOT_OPEN);
    CHECK(audio_get_port_config(&s, 5, cfg) == 0);
    CHECK(be32(cfg + 4) == AUDIO_STATUS_CLOSED && be64(cfg + 8) == 0 && be32(cfg + 28) == 0);
    CHECK(audio_port_open(&s, 6, 16, 0, 0, &port) == 0 && port == 5);   /* the closed number is reused */
    CHECK(audio_port_start(&s, 1, 0) == 0);
    CHECK(audio_port_close(&s, 1) == 0);                                 /* close while running */
    CHECK(audio_quit(&s) == 0);
    for (i = 0; i < 8; ++i) CHECK(s.ports[i].open == 0);
    CHECK(s.initialized == 0);
}

static void test_clock_and_tags(void)
{
    audio_service s;
    uint64_t idx[8], tag = 77;
    uint32_t port = 0, other = 0;
    audio_service_init(&s, &LAYOUT);
    CHECK(audio_init(&s) == 0);
    CHECK(audio_port_open(&s, 2, 8, 0, 0, &port) == 0 && port == 0);
    CHECK(audio_port_open(&s, 2, 16, 0, 0, &other) == 0 && other == 1);

    CHECK(audio_get_port_block_tag(&s, 0, 0, &tag) == AUDIO_ERROR_PORT_NOT_RUN);
    CHECK(audio_get_port_block_tag(&s, 2, 0, &tag) == AUDIO_ERROR_PORT_NOT_OPEN);
    CHECK(audio_get_port_block_tag(&s, 9, 0, &tag) == AUDIO_ERROR_PARAM);
    CHECK(tag == 77);
    CHECK(audio_port_start(&s, 0, 1000000) == 0);
    CHECK(audio_get_port_block_tag(&s, 0, 8, &tag) == AUDIO_ERROR_PARAM);
    CHECK(audio_get_port_block_tag(&s, 0, 0, NULL) == AUDIO_ERROR_PARAM);

    memset(idx, 0xFF, sizeof idx);
    CHECK(audio_tick(&s, 1000000 + 5333, idx, 8) == 0);
    CHECK(s.ports[0].counter == 0 && idx[0] == 0 && idx[1] == 0 && idx[7] == 0);
    CHECK(audio_tick(&s, 1000000 + 5334, idx, 8) == 1u);     /* only port 0 changed */
    CHECK(s.ports[0].counter == 1 && idx[0] == 1);
    CHECK(audio_tick(&s, 1000000 + 5334, idx, 8) == 0);
    /* counter 1, 8 blocks: current block 1 -> tag 1, previous block 0 -> tag 0, next block 2 is 7 blocks old -> clamped 0 */
    CHECK(audio_get_port_block_tag(&s, 0, 1, &tag) == 0 && tag == 1);
    CHECK(audio_get_port_block_tag(&s, 0, 0, &tag) == 0 && tag == 0);
    CHECK(audio_get_port_block_tag(&s, 0, 2, &tag) == 0 && tag == 0);

    CHECK(audio_tick(&s, 1000000 + 26667, idx, 8) == 1u);    /* 5 blocks */
    CHECK(s.ports[0].counter == 5 && idx[0] == 5);
    CHECK(audio_get_port_block_tag(&s, 0, 5, &tag) == 0 && tag == 5);
    CHECK(audio_get_port_block_tag(&s, 0, 4, &tag) == 0 && tag == 4);
    CHECK(audio_get_port_block_tag(&s, 0, 6, &tag) == 0 && tag == 0);

    CHECK(audio_tick(&s, 2000000, idx, 8) == 1u);            /* one second: 187 blocks */
    CHECK(s.ports[0].counter == 187 && idx[0] == 187 % 8);
    CHECK(audio_tick(&s, 1500000, idx, 8) == 0);             /* backwards clock */
    CHECK(s.ports[0].counter == 187);

    CHECK(audio_port_start(&s, 1, 2000000) == 0);
    CHECK(audio_tick(&s, 2000000 + 533334, idx, 8) == 3u);   /* both ports: 100 blocks on port 1 */
    CHECK(s.ports[1].counter == 100 && idx[1] == 100 % 16);
    CHECK(audio_get_port_block_tag(&s, 1, 4, &tag) == 0 && tag == 100);
    CHECK(audio_get_port_block_tag(&s, 1, 3, &tag) == 0 && tag == 99);
    CHECK(audio_get_port_block_tag(&s, 1, 5, &tag) == 0 && tag == 85);
    CHECK(audio_port_stop(&s, 0) == 0);
    {
        const uint64_t kept = s.ports[0].counter;
        CHECK(audio_tick(&s, 9000000, idx, 8) == 2u);        /* stopped port keeps its index */
        CHECK(s.ports[0].counter == kept && idx[0] == kept % 8);
    }
    CHECK(audio_tick(&s, 9000001, NULL, 0) == 0);            /* NULL array is allowed */
}

int main(void)
{
    test_out_queries();
    test_port_lifecycle();
    test_clock_and_tags();
    printf("AUDIO_SERVICE checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
