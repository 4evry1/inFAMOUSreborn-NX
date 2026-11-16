/* Host tests for the platform-free game-data service (native/domain/gamedata_service.c).
 *
 * The storage port is a fake kept entirely in memory: one game-data directory, one file
 * (PARAM.SFO), a free-space counter, three timestamps, and a per-operation failure switch so
 * every port failure path can be driven directly. No filesystem, no globals.
 *
 * Expected StatGet buffers are produced by `expected_statget()` and then differ byte for byte
 * only where a case deliberately changes one field, so a full-record comparison is possible.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamedata_service.h"

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
        long a_ = (long)(actual);                                                    \
        long e_ = (long)(expected);                                                  \
        checks++;                                                                    \
        if (a_ != e_) {                                                              \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__,         \
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
/* Fake storage port                                                                    */
/* ------------------------------------------------------------------------------------ */

#define FAKE_FILE_CAP 1024u

enum fake_op {
    FAKE_OP_FREE_KB = 0,
    FAKE_OP_STAT_DIR,
    FAKE_OP_READ_FILE,
    FAKE_OP_CREATE_DIR,
    FAKE_OP_WRITE_FILE,
    FAKE_OP_COUNT
};

typedef struct fake_storage {
    char dir_name[GAMEDATA_DIRNAME_MAX + 1u];
    int dir_exists;
    int dir_created_calls;
    int usrdir_created;
    uint64_t size_kb;
    int64_t times[3];
    uint64_t free_kb;

    char file_name[64];
    uint8_t file[FAKE_FILE_CAP];
    size_t file_len;
    int file_exists;
    int write_calls;

    /* Per-operation injected status; GAMEDATA_STORAGE_OK means "behave normally". */
    gamedata_storage_status fail[FAKE_OP_COUNT];
    int calls[FAKE_OP_COUNT];
} fake_storage;

static fake_storage *fake_new(const char *dir_name)
{
    fake_storage *const fake = calloc(1, sizeof(*fake));
    if (fake == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    strncpy(fake->dir_name, dir_name, sizeof(fake->dir_name) - 1u);
    fake->free_kb = 1000u;
    return fake;
}

static void fake_free(fake_storage *fake)
{
    free(fake);
}

static gamedata_storage_status port_free_kb(void *context, uint64_t *out_kb)
{
    fake_storage *const fake = context;
    fake->calls[FAKE_OP_FREE_KB]++;
    if (out_kb == NULL) {
        return GAMEDATA_STORAGE_IO_ERROR;
    }
    if (fake->fail[FAKE_OP_FREE_KB] != GAMEDATA_STORAGE_OK) {
        return fake->fail[FAKE_OP_FREE_KB];
    }
    *out_kb = fake->free_kb;
    return GAMEDATA_STORAGE_OK;
}

static gamedata_storage_status port_stat_dir(void *context, const char *dir_name,
                                             uint64_t *size_kb, int64_t times[3])
{
    fake_storage *const fake = context;
    fake->calls[FAKE_OP_STAT_DIR]++;
    if (fake->fail[FAKE_OP_STAT_DIR] != GAMEDATA_STORAGE_OK) {
        return fake->fail[FAKE_OP_STAT_DIR];
    }
    if (dir_name == NULL || size_kb == NULL || times == NULL ||
        strcmp(dir_name, fake->dir_name) != 0 || !fake->dir_exists) {
        return GAMEDATA_STORAGE_NOT_FOUND;
    }
    *size_kb = fake->size_kb;
    times[0] = fake->times[0];
    times[1] = fake->times[1];
    times[2] = fake->times[2];
    return GAMEDATA_STORAGE_OK;
}

static gamedata_storage_status port_read_file(void *context, const char *dir_name,
                                              const char *file, uint8_t *buf, size_t cap,
                                              size_t *len)
{
    fake_storage *const fake = context;
    fake->calls[FAKE_OP_READ_FILE]++;
    if (fake->fail[FAKE_OP_READ_FILE] != GAMEDATA_STORAGE_OK) {
        return fake->fail[FAKE_OP_READ_FILE];
    }
    if (dir_name == NULL || file == NULL || buf == NULL || len == NULL ||
        strcmp(dir_name, fake->dir_name) != 0 || !fake->file_exists ||
        strcmp(file, fake->file_name) != 0) {
        return GAMEDATA_STORAGE_NOT_FOUND;
    }
    if (fake->file_len > cap) {
        return GAMEDATA_STORAGE_IO_ERROR;
    }
    memcpy(buf, fake->file, fake->file_len);
    *len = fake->file_len;
    return GAMEDATA_STORAGE_OK;
}

static gamedata_storage_status port_create_dir(void *context, const char *dir_name)
{
    fake_storage *const fake = context;
    fake->calls[FAKE_OP_CREATE_DIR]++;
    if (fake->fail[FAKE_OP_CREATE_DIR] != GAMEDATA_STORAGE_OK) {
        return fake->fail[FAKE_OP_CREATE_DIR];
    }
    if (dir_name == NULL || strcmp(dir_name, fake->dir_name) != 0) {
        return GAMEDATA_STORAGE_IO_ERROR;
    }
    fake->dir_exists = 1;
    fake->usrdir_created = 1;
    fake->dir_created_calls++;
    return GAMEDATA_STORAGE_OK;
}

static gamedata_storage_status port_write_file(void *context, const char *dir_name,
                                               const char *file, const uint8_t *buf, size_t len)
{
    fake_storage *const fake = context;
    fake->calls[FAKE_OP_WRITE_FILE]++;
    if (fake->fail[FAKE_OP_WRITE_FILE] != GAMEDATA_STORAGE_OK) {
        return fake->fail[FAKE_OP_WRITE_FILE];
    }
    if (dir_name == NULL || file == NULL || buf == NULL || dir_name == NULL ||
        strcmp(dir_name, fake->dir_name) != 0 || len > FAKE_FILE_CAP) {
        return GAMEDATA_STORAGE_IO_ERROR;
    }
    strncpy(fake->file_name, file, sizeof(fake->file_name) - 1u);
    memcpy(fake->file, buf, len);
    fake->file_len = len;
    fake->file_exists = 1;
    fake->write_calls++;
    return GAMEDATA_STORAGE_OK;
}

static gamedata_storage_port fake_port_bound(fake_storage *fake)
{
    gamedata_storage_port port;

    port.context = fake;
    port.free_kb = port_free_kb;
    port.stat_dir = port_stat_dir;
    port.read_file = port_read_file;
    port.create_dir = port_create_dir;
    port.write_file = port_write_file;
    return port;
}

/* ------------------------------------------------------------------------------------ */
/* Expected-record helpers                                                              */
/* ------------------------------------------------------------------------------------ */

static void put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void put_be64(uint8_t *dst, uint64_t value)
{
    put_be32(dst, (uint32_t)(value >> 32));
    put_be32(dst + 4, (uint32_t)value);
}

static uint32_t get_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

static uint64_t get_be64(const uint8_t *src)
{
    return ((uint64_t)get_be32(src) << 32) | (uint64_t)get_be32(src + 4);
}

/* The whole 0x1448-byte record the service is expected to produce. `is_new` selects the two
 * branches of report §1.3; getParam is copied from `param` for the existing case. */
static void expected_statget(const char *dir_name, int32_t free_kb, uint32_t is_new,
                            int32_t size_kb, const int64_t times[3], const uint8_t *param,
                            uint8_t out[GAMEDATA_STATGET_BYTES])
{
    /* The guest fields have no NUL terminator of their own when filled to capacity, so the
     * expected path buffers are one byte larger than the field to keep snprintf honest. */
    char content_info[0x41Fu + 1u];
    char game_data[0x421u + 1u];

    memset(out, 0, GAMEDATA_STATGET_BYTES);
    snprintf(content_info, sizeof(content_info), "/dev_hdd0/game/%s", dir_name);
    snprintf(game_data, sizeof(game_data), "/dev_hdd0/game/%s/USRDIR", dir_name);

    put_be32(out + 0x000, (uint32_t)free_kb);
    put_be32(out + 0x004, is_new);
    memcpy(out + 0x008, content_info, strlen(content_info) + 1u);
    memcpy(out + 0x427, game_data, strlen(game_data) + 1u);
    put_be64(out + 0x848, (uint64_t)times[0]);
    put_be64(out + 0x850, (uint64_t)times[1]);
    put_be64(out + 0x858, (uint64_t)times[2]);
    if (param != NULL) {
        memcpy(out + 0x860, param, GAMEDATA_PARAM_BYTES);
    }
    put_be32(out + 0x13FC, (uint32_t)size_kb);
    put_be32(out + 0x1400, 0u);
}

/* A getParam with every field populated, in guest (big-endian) order. */
static void sample_param(uint8_t param[GAMEDATA_PARAM_BYTES])
{
    memset(param, 0, GAMEDATA_PARAM_BYTES);
    memcpy(param + 0x000, "inFAMOUS", 9u);
    memcpy(param + 0x080, "inFAMOUS [ja]", 14u);
    memcpy(param + 0x100, "inFAMOUS [de]", 14u);
    memcpy(param + 0xA80, "BCUS98146", 10u);
    memcpy(param + 0xA8C, "01.00", 6u);
    put_be32(param + 0xA94, 5u);
    put_be32(param + 0xA98, 0x20u);
}

/* ------------------------------------------------------------------------------------ */
/* Name validation (report §1.2)                                                        */
/* ------------------------------------------------------------------------------------ */

static int32_t validate_cstr(const char *name, uint32_t sdk)
{
    return gamedata_validate_dir_name((const uint8_t *)name, strlen(name) + 1u, sdk);
}

static void test_name_validation(void)
{
    const uint32_t sdk_old = 0x360001u; /* <= 0x36FFFF: max_len 32 */
    const uint32_t sdk_new = 0x480000u; /* >  0x36FFFF: max_len 31 */
    char name[40];

    CHECK_EQ_INT(validate_cstr("TEST00000", sdk_new), 0);
    CHECK_EQ_INT(validate_cstr("TEST00000", sdk_old), 0);
    CHECK_EQ_INT(validate_cstr("A-Z09_", sdk_old), 0);

    CHECK_EQ_INT(validate_cstr("_A", sdk_old), -1);
    CHECK_EQ_INT(validate_cstr("_A", sdk_new), -1);
    CHECK_EQ_INT(validate_cstr("lower", sdk_old), -1);
    CHECK_EQ_INT(validate_cstr("A.B", sdk_old), -1);
    CHECK_EQ_INT(validate_cstr("A/B", sdk_old), -1);
    CHECK_EQ_INT(validate_cstr("A B", sdk_old), -1);

    CHECK_EQ_INT(validate_cstr("", sdk_old), -2);
    CHECK_EQ_INT(validate_cstr("", sdk_new), -2);

    /* Length 31 fits both SDK rules. */
    memset(name, 'A', sizeof(name));
    name[31] = '\0';
    CHECK_EQ_INT(validate_cstr(name, sdk_old), 0);
    CHECK_EQ_INT(validate_cstr(name, sdk_new), 0);

    /* Length 32: only the older SDK accepts it. */
    memset(name, 'A', sizeof(name));
    name[32] = '\0';
    CHECK_EQ_INT(validate_cstr(name, sdk_old), 0);
    CHECK_EQ_INT(validate_cstr(name, sdk_new), -2);

    /* Length 33: rejected by both. */
    memset(name, 'A', sizeof(name));
    name[33] = '\0';
    CHECK_EQ_INT(validate_cstr(name, sdk_old), -2);
    CHECK_EQ_INT(validate_cstr(name, sdk_new), -2);

    /* No NUL inside the readable range: the buffer is valid for 12 bytes but `readable` is
     * 12 while the string runs past it. */
    memcpy(name, "ABCDEFGHIJKLM", 14u);
    CHECK_EQ_INT(gamedata_validate_dir_name((const uint8_t *)name, 12u, sdk_old), -2);
    CHECK_EQ_INT(gamedata_validate_dir_name((const uint8_t *)name, 14u, sdk_old), 0);

    /* Nothing past `readable` is examined: a NUL exactly at the boundary is accepted. */
    name[0] = (uint8_t)'A';
    name[1] = (uint8_t)'B';
    name[2] = 0;
    CHECK_EQ_INT(gamedata_validate_dir_name((const uint8_t *)name, 3u, sdk_old), 0);
    CHECK_EQ_INT(gamedata_validate_dir_name((const uint8_t *)name, 2u, sdk_old), -2);

    /* NULL / zero readable are rejected without reading anything. */
    CHECK_EQ_INT(gamedata_validate_dir_name(NULL, 8u, sdk_old), -2);
    CHECK_EQ_INT(gamedata_validate_dir_name((const uint8_t *)"AB", 0u, sdk_old), -2);
}

/* ------------------------------------------------------------------------------------ */
/* StatGet construction (report §1.3)                                                   */
/* ------------------------------------------------------------------------------------ */

static void test_build_statget_new_data(void)
{
    fake_storage *const fake = fake_new("TEST00000");
    const gamedata_storage_port port = fake_port_bound(fake);
    uint8_t out[GAMEDATA_STATGET_BYTES];
    uint8_t expected[GAMEDATA_STATGET_BYTES];
    gamedata_check state;
    const int64_t zero_times[3] = {0, 0, 0};

    /* New data: no directory, no PARAM.SFO. */
    fake->dir_exists = 0;
    fake->free_kb = 5000000000ull; /* 5e9 KB -> clamped to INT32_MAX */
    memset(&state, 0xCD, sizeof(state));
    memset(out, 0xAA, sizeof(out));

    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);

    expected_statget("TEST00000", INT32_MAX, 1u, 0, zero_times, NULL, expected);
    CHECK_EQ_MEM(out, expected, GAMEDATA_STATGET_BYTES);

    /* Spot-check the fields the game actually uses (report §7). */
    CHECK_EQ_INT(get_be32(out + 0x000), INT32_MAX);
    CHECK_EQ_INT(get_be32(out + 0x004), 1u);
    CHECK(strcmp((const char *)(out + 0x008), "/dev_hdd0/game/TEST00000") == 0);
    CHECK(strcmp((const char *)(out + 0x427), "/dev_hdd0/game/TEST00000/USRDIR") == 0);
    CHECK_EQ_INT(get_be64(out + 0x848), 0);
    CHECK_EQ_INT(get_be64(out + 0x850), 0);
    CHECK_EQ_INT(get_be64(out + 0x858), 0);
    CHECK_EQ_INT(get_be32(out + 0x13FC), 0u);
    CHECK_EQ_INT(get_be32(out + 0x1400), 0u);
    /* No read attempt for a directory that does not exist. */
    CHECK_EQ_INT(fake->calls[FAKE_OP_READ_FILE], 0);
    CHECK_EQ_INT(fake->calls[FAKE_OP_STAT_DIR], 1);

    /* The state the caller keeps for cellGameDataGetSizeKB (report §2). */
    CHECK(strcmp(state.dir_name, "TEST00000") == 0);
    CHECK_EQ_INT(state.dir_name_length, 9);
    CHECK_EQ_INT(state.is_new, 1);
    CHECK_EQ_INT(state.size_kb, 0);
    CHECK_EQ_INT(state.free_kb, INT32_MAX);

    /* A free value that fits is passed through unchanged. */
    fake->free_kb = 700000u;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);
    CHECK_EQ_INT(get_be32(out + 0x000), 700000);
    CHECK_EQ_INT(state.free_kb, 700000);

    fake_free(fake);
}

static void test_build_statget_existing(void)
{
    fake_storage *const fake = fake_new("TEST00000");
    gamedata_storage_port port = fake_port_bound(fake);
    uint8_t param[GAMEDATA_PARAM_BYTES];
    uint8_t round_trip[GAMEDATA_PARAM_BYTES];
    uint8_t out[GAMEDATA_STATGET_BYTES];
    uint8_t expected[GAMEDATA_STATGET_BYTES];
    gamedata_check state;
    size_t encoded_len;
    const int64_t times[3] = {0x1122334455ll, 0x66778899AAll, -2}; /* negative = pre-epoch */

    sample_param(param);
    encoded_len = gamedata_param_sfo_encode(param, fake->file, FAKE_FILE_CAP);
    CHECK(encoded_len > 0);
    fake->file_len = encoded_len;
    fake->file_exists = 1;
    strcpy(fake->file_name, "PARAM.SFO");
    fake->dir_exists = 1;
    fake->size_kb = 1392640u; /* 0x153800, the game's required size */
    fake->times[0] = times[0];
    fake->times[1] = times[1];
    fake->times[2] = times[2];
    fake->free_kb = 1500000u;

    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);

    /* getParam round-trips byte-exact over every field the encoder emits. */
    CHECK_EQ_INT(gamedata_param_sfo_decode(fake->file, fake->file_len, round_trip), GAMEDATA_OK);
    CHECK_EQ_MEM(round_trip, param, GAMEDATA_PARAM_BYTES);
    CHECK_EQ_MEM(out + 0x860, param, GAMEDATA_PARAM_BYTES);

    expected_statget("TEST00000", 1500000, 0u, 1392640, times, param, expected);
    CHECK_EQ_MEM(out, expected, GAMEDATA_STATGET_BYTES);

    CHECK_EQ_INT(get_be32(out + 0x004), 0u);
    CHECK_EQ_INT(get_be32(out + 0x13FC), 1392640);
    CHECK_EQ_INT(get_be64(out + 0x848), 0x1122334455ll);
    CHECK_EQ_INT(get_be64(out + 0x850), 0x66778899AAll);
    CHECK_EQ_INT((int64_t)get_be64(out + 0x858), -2);
    CHECK_EQ_INT(get_be32(out + 0x1400), 0u); /* documented §5 deviation */
    CHECK_EQ_INT(state.is_new, 0);
    CHECK_EQ_INT(state.size_kb, 1392640);

    /* Size clamp is independent of the free clamp. */
    fake->size_kb = 5000000000ull;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);
    CHECK_EQ_INT((int32_t)get_be32(out + 0x13FC), INT32_MAX);
    CHECK_EQ_INT(state.size_kb, INT32_MAX);

    /* Broken PARAM.SFO: BROKEN_PARAM and nothing else written. */
    memcpy(fake->file, "XXXXnot-a-psf", 13u);
    fake->file_len = 13u;
    memset(out, 0x5A, sizeof(out));
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_BROKEN_PARAM);
    for (size_t i = 0; i < GAMEDATA_STATGET_BYTES; i++) {
        if (out[i] != 0) {
            checks++;
            failures++;
            fprintf(stderr, "FAIL %s:%d: broken PARAM.SFO left out[0x%zX]=0x%02X\n", __FILE__,
                    __LINE__, i, out[i]);
            break;
        }
    }
    checks++; /* the loop above is a single assertion when it passes */

    /* An absent PARAM.SFO with an existing directory is a storage error, not new data. */
    fake->file_exists = 0;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state),
                 GAMEDATA_STORAGE_ERROR);

    /* Port failures surface as STORAGE_ERROR. Restore a valid PARAM.SFO first: the broken
     * image above overwrote the file contents, not just its length. */
    fake->file_exists = 1;
    fake->file_len = gamedata_param_sfo_encode(param, fake->file, FAKE_FILE_CAP);
    CHECK(fake->file_len > 0);
    fake->fail[FAKE_OP_FREE_KB] = GAMEDATA_STORAGE_IO_ERROR;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state),
                 GAMEDATA_STORAGE_ERROR);
    fake->fail[FAKE_OP_FREE_KB] = GAMEDATA_STORAGE_OK;

    fake->fail[FAKE_OP_STAT_DIR] = GAMEDATA_STORAGE_IO_ERROR;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state),
                 GAMEDATA_STORAGE_ERROR);
    fake->fail[FAKE_OP_STAT_DIR] = GAMEDATA_STORAGE_OK;

    fake->fail[FAKE_OP_READ_FILE] = GAMEDATA_STORAGE_IO_ERROR;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state),
                 GAMEDATA_STORAGE_ERROR);
    fake->fail[FAKE_OP_READ_FILE] = GAMEDATA_STORAGE_OK;

    /* NO_SPACE from stat_dir is a storage error; NO_SPACE from free_kb means "no free space",
     * which the firmware reports as 0. */
    fake->fail[FAKE_OP_STAT_DIR] = GAMEDATA_STORAGE_NO_SPACE;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state),
                 GAMEDATA_STORAGE_ERROR);
    fake->fail[FAKE_OP_STAT_DIR] = GAMEDATA_STORAGE_OK;
    fake->fail[FAKE_OP_FREE_KB] = GAMEDATA_STORAGE_NO_SPACE;
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);
    CHECK_EQ_INT(get_be32(out + 0x000), 0u);
    fake->fail[FAKE_OP_FREE_KB] = GAMEDATA_STORAGE_OK;

    /* Argument validation: an unusable port, a NULL record or an invalid name is rejected
     * before any port call. */
    CHECK_EQ_INT(gamedata_build_statget(NULL, "TEST00000", out, &state), GAMEDATA_BAD_ARGUMENT);
    CHECK_EQ_INT(gamedata_build_statget(&port, NULL, out, &state), GAMEDATA_BAD_ARGUMENT);
    CHECK_EQ_INT(gamedata_build_statget(&port, "bad.name", out, &state), GAMEDATA_BAD_ARGUMENT);
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", NULL, &state),
                 GAMEDATA_BAD_ARGUMENT);
    CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, NULL), GAMEDATA_BAD_ARGUMENT);
    {
        gamedata_storage_port incomplete = fake_port_bound(fake);
        incomplete.read_file = NULL;
        CHECK_EQ_INT(gamedata_build_statget(&incomplete, "TEST00000", out, &state),
                     GAMEDATA_BAD_ARGUMENT);
    }

    (void)port;
    fake_free(fake);
}

/* ------------------------------------------------------------------------------------ */
/* apply_result (report §1.5)                                                           */
/* ------------------------------------------------------------------------------------ */

static void test_apply_result(void)
{
    fake_storage *const fake = fake_new("TEST00000");
    const gamedata_storage_port port = fake_port_bound(fake);
    uint8_t param[GAMEDATA_PARAM_BYTES];
    uint8_t decoded[GAMEDATA_PARAM_BYTES];
    gamedata_check state;
    gamedata_action action;
    const int32_t error_results[4] = {-1, -3, -4, -5};
    size_t i;

    sample_param(param);
    memset(&state, 0, sizeof(state));
    strcpy(state.dir_name, "TEST00000");
    state.dir_name_length = 9;
    state.is_new = 0;

    /* -- OK with existing data and no setParam: nothing to do (arm 0x7324, else-branch). */
    action.action = GAMEDATA_ACTION_UNKNOWN_RESULT;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, NULL, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_NONE);
    CHECK_EQ_INT(fake->calls[FAKE_OP_CREATE_DIR], 0);
    CHECK_EQ_INT(fake->calls[FAKE_OP_WRITE_FILE], 0);

    /* -- OK with new data and no setParam: param error, still no disk effect. */
    state.is_new = 1;
    action.action = GAMEDATA_ACTION_UNKNOWN_RESULT;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, NULL, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_PARAM_ERROR);
    CHECK_EQ_INT(fake->calls[FAKE_OP_CREATE_DIR], 0);
    CHECK_EQ_INT(fake->calls[FAKE_OP_WRITE_FILE], 0);
    state.is_new = 0;

    /* -- OK with setParam: create the directory and write a decodable PARAM.SFO. */
    action.action = GAMEDATA_ACTION_NONE;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK_EQ_INT(fake->dir_created_calls, 1);
    CHECK_EQ_INT(fake->usrdir_created, 1);
    CHECK_EQ_INT(fake->write_calls, 1);
    CHECK(strcmp(fake->file_name, "PARAM.SFO") == 0);
    CHECK(fake->file_len > 0);
    CHECK_EQ_INT(gamedata_param_sfo_decode(fake->file, fake->file_len, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);

    /* The written file is what a later build_statget reads back unchanged. */
    {
        uint8_t out[GAMEDATA_STATGET_BYTES];
        fake->size_kb = 12u;
        CHECK_EQ_INT(gamedata_build_statget(&port, "TEST00000", out, &state), GAMEDATA_OK);
        CHECK_EQ_INT(get_be32(out + 0x004), 0u);
        CHECK_EQ_MEM(out + 0x860, param, GAMEDATA_PARAM_BYTES);
    }

    /* Idempotent: a second OK re-creates and re-writes with the same bytes. */
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK_EQ_INT(fake->dir_created_calls, 2);
    CHECK_EQ_INT(fake->write_calls, 2);

    /* -- OK_CANCEL: no disk effect at all. */
    fake->calls[FAKE_OP_CREATE_DIR] = 0;
    fake->calls[FAKE_OP_WRITE_FILE] = 0;
    action.action = GAMEDATA_ACTION_UNKNOWN_RESULT;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 1, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_NONE);
    CHECK_EQ_INT(fake->calls[FAKE_OP_CREATE_DIR], 0);
    CHECK_EQ_INT(fake->calls[FAKE_OP_WRITE_FILE], 0);

    /* -- -1/-3/-4/-5: CBRESULT_ERROR, never a dialog and never a disk effect. */
    for (i = 0; i < sizeof(error_results) / sizeof(error_results[0]); i++) {
        action.action = GAMEDATA_ACTION_UNKNOWN_RESULT;
        CHECK_EQ_INT(gamedata_apply_result(&port, &state, error_results[i], param, &action),
                     GAMEDATA_OK);
        CHECK_EQ_INT(action.action, GAMEDATA_ACTION_CBRESULT_ERROR);
        CHECK_EQ_INT(fake->calls[FAKE_OP_CREATE_DIR], 0);
        CHECK_EQ_INT(fake->calls[FAKE_OP_WRITE_FILE], 0);
    }

    /* -- -2: PARAM_ERROR. */
    action.action = GAMEDATA_ACTION_NONE;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, -2, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_PARAM_ERROR);
    CHECK_EQ_INT(fake->calls[FAKE_OP_CREATE_DIR], 0);

    /* -- outside [-5,1]: UNKNOWN_RESULT. */
    {
        const int32_t unknown[6] = {2, 3, -6, -7, 100, -2147483647 - 1};
        for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
            action.action = GAMEDATA_ACTION_NONE;
            CHECK_EQ_INT(gamedata_apply_result(&port, &state, unknown[i], param, &action),
                         GAMEDATA_OK);
            CHECK_EQ_INT(action.action, GAMEDATA_ACTION_UNKNOWN_RESULT);
        }
    }

    /* -- Port failures inside the OK arm surface as STORAGE_ERROR with action NONE. */
    fake->fail[FAKE_OP_CREATE_DIR] = GAMEDATA_STORAGE_IO_ERROR;
    action.action = GAMEDATA_ACTION_CREATED_OR_UPDATED;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action),
                 GAMEDATA_STORAGE_ERROR);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_NONE);
    CHECK_EQ_INT(fake->calls[FAKE_OP_WRITE_FILE], 0);
    fake->fail[FAKE_OP_CREATE_DIR] = GAMEDATA_STORAGE_OK;

    fake->fail[FAKE_OP_WRITE_FILE] = GAMEDATA_STORAGE_NO_SPACE;
    action.action = GAMEDATA_ACTION_CREATED_OR_UPDATED;
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action),
                 GAMEDATA_STORAGE_ERROR);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_NONE);
    fake->fail[FAKE_OP_WRITE_FILE] = GAMEDATA_STORAGE_OK;

    /* An unusable port only matters on the arm that needs it. */
    CHECK_EQ_INT(gamedata_apply_result(NULL, &state, 1, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(gamedata_apply_result(NULL, &state, -1, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(gamedata_apply_result(NULL, &state, 0, param, &action),
                 GAMEDATA_STORAGE_ERROR);

    /* Bad arguments. */
    CHECK_EQ_INT(gamedata_apply_result(&port, NULL, 0, param, &action), GAMEDATA_BAD_ARGUMENT);
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, NULL), GAMEDATA_BAD_ARGUMENT);

    /* The guest return value is always 0 (report §1.6). */
    CHECK_EQ_INT(gamedata_guest_return(), 0);

    fake_free(fake);
}

/* ------------------------------------------------------------------------------------ */
/* PARAM.SFO encoding and decoding                                                      */
/* ------------------------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

/* Finds an index entry by key. Returns NULL when absent. Header word +8 is the index table
 * offset, +12 the entry count and +16 the data table offset; each 16-byte entry is
 * +0 key_offset u16, +2 fmt u16, +4 len u32, +8 max_len u32, +12 value offset (relative to
 * the data table). */
static const uint8_t *find_entry(const uint8_t *sfo, size_t len, const char *key)
{
    const uint32_t index_offset = le32(sfo + 8);
    const uint32_t count = le32(sfo + 12);
    const uint32_t data_offset = le32(sfo + 16);
    uint32_t i;

    for (i = 0; i < count; i++) {
        const uint8_t *const e = sfo + index_offset + (size_t)i * 16u;
        const uint32_t key_offset = le16(e + 0);
        if (key_offset < index_offset || (size_t)key_offset >= len ||
            (size_t)key_offset >= data_offset) {
            return NULL;
        }
        if (strcmp((const char *)(sfo + key_offset), key) == 0) {
            return e;
        }
    }
    return NULL;
}

static void test_sfo_format(void)
{
    uint8_t param[GAMEDATA_PARAM_BYTES];
    uint8_t sfo[FAKE_FILE_CAP];
    uint8_t decoded[GAMEDATA_PARAM_BYTES];
    size_t len;
    const uint8_t *e;

    sample_param(param);
    memset(sfo, 0xEE, sizeof(sfo));
    len = gamedata_param_sfo_encode(param, sfo, sizeof(sfo));
    CHECK(len > 0);

    /* Magic, version, and the four header words. */
    CHECK_EQ_INT(memcmp(sfo, "\0PSF", 4u), 0);
    CHECK_EQ_INT(le32(sfo + 4), 0x0101u);
    CHECK(le32(sfo + 8) >= 20u);
    CHECK(le32(sfo + 12) >= 8u); /* ATTRIBUTE, CATEGORY, PARENTAL_LEVEL, TITLE, TITLE_00,
                                  * TITLE_01, TITLE_ID, VERSION */
    CHECK(le32(sfo + 16) <= len);
    CHECK_EQ_INT(le32(sfo + 16) % 4u, 0u); /* data table is 4-byte aligned */
    CHECK_EQ_INT(len % 4u, 0u);            /* every value is padded to 4 bytes */

    /* Keys are bytewise ascending. */
    {
        const uint32_t index_offset = le32(sfo + 8);
        const uint32_t count = le32(sfo + 12);
        char previous[64];
        uint32_t i;
        previous[0] = '\0';
        for (i = 0; i < count; i++) {
            const uint8_t *const entry = sfo + index_offset + (size_t)i * 16u;
            const char *const key = (const char *)(sfo + le16(entry + 0));
            CHECK(i == 0 || strcmp(previous, key) < 0);
            strncpy(previous, key, sizeof(previous) - 1u);
            previous[sizeof(previous) - 1u] = '\0';
        }
    }

    /* The exact key set: only the non-empty titleLang entries are emitted. */
    CHECK(find_entry(sfo, len, "ATTRIBUTE") != NULL);
    CHECK(find_entry(sfo, len, "CATEGORY") != NULL);
    CHECK(find_entry(sfo, len, "PARENTAL_LEVEL") != NULL);
    CHECK(find_entry(sfo, len, "TITLE") != NULL);
    CHECK(find_entry(sfo, len, "TITLE_00") != NULL);
    CHECK(find_entry(sfo, len, "TITLE_01") != NULL);
    CHECK(find_entry(sfo, len, "TITLE_02") == NULL);
    CHECK(find_entry(sfo, len, "TITLE_19") == NULL);
    CHECK(find_entry(sfo, len, "TITLE_ID") != NULL);
    CHECK(find_entry(sfo, len, "VERSION") != NULL);
    CHECK_EQ_INT(le32(sfo + 12), 8u);

    /* Field formats, max_len and values. */
    e = find_entry(sfo, len, "ATTRIBUTE");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0404u);
        CHECK_EQ_INT(le32(e + 8), 4u);
        CHECK_EQ_INT(le32(e + 4), 4u);
        CHECK_EQ_INT(le32(sfo + le32(sfo + 16) + le32(e + 12)), 0x20u);
    }
    e = find_entry(sfo, len, "PARENTAL_LEVEL");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0404u);
        CHECK_EQ_INT(le32(sfo + le32(sfo + 16) + le32(e + 12)), 5u);
    }
    e = find_entry(sfo, len, "CATEGORY");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0204u);
        CHECK(strcmp((const char *)(sfo + le32(sfo + 16) + le32(e + 12)), "GD") == 0);
    }
    e = find_entry(sfo, len, "TITLE");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0204u);
        CHECK_EQ_INT(le32(e + 8), 128u);
        CHECK(strcmp((const char *)(sfo + le32(sfo + 16) + le32(e + 12)), "inFAMOUS") == 0);
    }
    e = find_entry(sfo, len, "TITLE_ID");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0204u);
        CHECK_EQ_INT(le32(e + 8), 16u);
        CHECK(strcmp((const char *)(sfo + le32(sfo + 16) + le32(e + 12)), "BCUS98146") == 0);
    }
    e = find_entry(sfo, len, "VERSION");
    CHECK(e != NULL);
    if (e != NULL) {
        CHECK_EQ_INT(le16(e + 2), 0x0204u);
        CHECK_EQ_INT(le32(e + 8), 8u);
        CHECK(strcmp((const char *)(sfo + le32(sfo + 16) + le32(e + 12)), "01.00") == 0);
    }

    /* Nothing past the returned length is written. */
    for (size_t i = len; i < sizeof(sfo); i++) {
        if (sfo[i] != 0xEE) {
            checks++;
            failures++;
            fprintf(stderr, "FAIL %s:%d: encode wrote past its length at 0x%zX\n", __FILE__,
                    __LINE__, i);
            break;
        }
    }
    checks++;

    /* Decode of the encoder output is the identity. */
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, len, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);

    /* An undersized cap yields 0 and writes nothing. */
    {
        uint8_t tiny[16];
        memset(tiny, 0x5A, sizeof(tiny));
        CHECK_EQ_INT(gamedata_param_sfo_encode(param, tiny, sizeof(tiny)), 0u);
        CHECK_EQ_INT(tiny[0], 0x5A);
    }

    /* A zeroed getParam still encodes (empty strings, zero integers) and decodes. */
    memset(param, 0, sizeof(param));
    len = gamedata_param_sfo_encode(param, sfo, sizeof(sfo));
    CHECK(len > 0);
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, len, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);
    CHECK(find_entry(sfo, len, "TITLE_00") == NULL);

    /* Full-title fields: a 127-byte title still round-trips. */
    sample_param(param);
    memset(param + 0x000, 'A', 127u);
    param[127] = 0;
    len = gamedata_param_sfo_encode(param, sfo, sizeof(sfo));
    CHECK(len > 0);
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, len, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);
}

static void test_sfo_decode_rejects(void)
{
    uint8_t param[GAMEDATA_PARAM_BYTES];
    /* The mutated copies must be as large as the encoder output: `len` is checked against
     * `sizeof(sfo)`, and a 1 KiB source buffer over-runs an encoder that trusts it. */
    uint8_t sfo[4u * FAKE_FILE_CAP];
    uint8_t broken[4u * FAKE_FILE_CAP];
    uint8_t decoded[GAMEDATA_PARAM_BYTES];
    size_t len;
    size_t i;

    sample_param(param);
    len = gamedata_param_sfo_encode(param, sfo, sizeof(sfo));
    CHECK(len > 0);

    /* -- baseline: the untouched encoder output decodes. */
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, len, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);

    /* -- bad magic. */
    memcpy(broken, sfo, len);
    broken[0] = 'X';
    memset(decoded, 0x11, sizeof(decoded));
    CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);
    for (i = 0; i < GAMEDATA_PARAM_BYTES; i++) {
        if (decoded[i] != 0) {
            checks++;
            failures++;
            fprintf(stderr, "FAIL %s:%d: reject left param_out[0x%zX] set\n", __FILE__,
                    __LINE__, i);
            break;
        }
    }
    checks++;
    /* -- wrong version. */
    memcpy(broken, sfo, len);
    broken[4] = 0x02;
    CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);

    /* -- truncated index: claim more entries than the index table holds. */
    memcpy(broken, sfo, len);
    broken[12] = 0xFF;
    broken[13] = 0x00;
    CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);

    /* -- truncated header. */
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, 19u, decoded), GAMEDATA_BROKEN_PARAM);
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, 0u, decoded), GAMEDATA_BROKEN_PARAM);
    CHECK_EQ_INT(gamedata_param_sfo_decode(NULL, 64u, decoded), GAMEDATA_BROKEN_PARAM);
    CHECK_EQ_INT(gamedata_param_sfo_decode(sfo, len, NULL), GAMEDATA_BAD_ARGUMENT);

    /* -- data offset past the end of the image. `find_entry` already returns the entry
     * start, so the byte position is its distance from the image base. */
    memcpy(broken, sfo, len);
    {
        const uint8_t *const e = find_entry(broken, len, "TITLE");
        const size_t at = (size_t)(e - broken);
        CHECK(e != NULL);
        broken[at + 12] = 0xF0;
        broken[at + 13] = 0xFF;
        broken[at + 14] = 0x00;
        broken[at + 15] = 0x00;
    }
    CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);

    /* -- data table itself past the end. */
    memcpy(broken, sfo, len);
    broken[16] = 0xFF;
    broken[17] = 0xFF;
    broken[18] = 0x00;
    broken[19] = 0x00;
    CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);

    /* -- a string without a NUL anywhere inside its data. */
    {
        const uint8_t *const e = find_entry(sfo, len, "TITLE");
        const uint32_t value_offset = le32(e + 12);
        const size_t at = (size_t)le32(sfo + 16) + value_offset;
        /* Overwrite the TITLE bytes and the NUL after them with 'Z'. */
        memcpy(broken, sfo, len);
        for (i = 0; i + 1u < 16u; i++) {
            broken[at + i] = 'Z';
        }
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, at + 1u, decoded),
                     GAMEDATA_BROKEN_PARAM);
    }

    /* -- a string whose declared max_len is larger than the guest field. */
    {
        const uint8_t *const e = find_entry(sfo, len, "TITLE_ID");
        const size_t at = (size_t)(e - sfo);
        memcpy(broken, sfo, len);
        broken[at + 8] = 0x40; /* max_len 0x40 > 16 */
        broken[at + 9] = 0x00;
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);
    }

    /* -- a key offset pointing into the data table. */
    {
        const uint8_t *const e = find_entry(sfo, len, "TITLE");
        const size_t at = (size_t)(e - sfo);
        memcpy(broken, sfo, len);
        broken[at + 0] = 0xFF;
        broken[at + 1] = 0xFF;
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_BROKEN_PARAM);
    }

    /* -- reordering the index entries is accepted (decode takes any key order). */
    {
        const uint32_t index_offset = le32(sfo + 8);
        const uint32_t count = le32(sfo + 12);
        uint8_t entry[16];
        memcpy(broken, sfo, len);
        memcpy(entry, broken + index_offset, 16u);
        memmove(broken + index_offset, broken + index_offset + 16u, (size_t)(count - 1u) * 16u);
        memcpy(broken + index_offset + (size_t)(count - 1u) * 16u, entry, 16u);
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_OK);
        CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);
    }

    /* -- an unknown key is ignored. */
    {
        const uint32_t index_offset = le32(sfo + 8);
        const uint32_t count = le32(sfo + 12);
        uint32_t k;
        int renamed = 0;
        memcpy(broken, sfo, len);
        (void)index_offset;
        for (k = 0; k < count; k++) {
            const size_t at = (size_t)le32(broken + 8) + (size_t)k * 16u;
            char *const key = (char *)(broken + le16(broken + at));
            if (strcmp(key, "CATEGORY") == 0) {
                memcpy(key, "CATEGORZ", 9u);
                renamed = 1;
            }
        }
        CHECK_EQ_INT(renamed, 1);
        CHECK_EQ_INT(gamedata_param_sfo_decode(broken, len, decoded), GAMEDATA_OK);
        CHECK_EQ_MEM(decoded, param, GAMEDATA_PARAM_BYTES);
    }
}

/* ------------------------------------------------------------------------------------ */

int main(void)
{
    test_name_validation();
    test_build_statget_new_data();
    test_build_statget_existing();
    test_apply_result();
    test_sfo_format();
    test_sfo_decode_rejects();

    if (failures) {
        fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
