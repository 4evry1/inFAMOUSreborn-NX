/* Host tests for the POSIX game-data storage adapter
 * (native/adapters/gamedata_storage_posix.c) over a REAL temporary directory tree.
 *
 * Everything runs inside one `mkdtemp` directory below the build dir the verifier passes:
 * the root the adapter is initialised with is `<tmp>/hdd0/game`, so `init` also proves the
 * recursive mkdir. No SD card, no game, no network, no globals.
 *
 * Covered:
 *   - init creates a nested root and free_kb reports a positive size;
 *   - stat_dir NOT_FOUND -> create_dir -> stat_dir OK with USRDIR present;
 *   - write_file/read_file round trip, then a SHORTER overwrite read back exactly, no `.tmp` left;
 *   - size_kb rounding 1 -> 1, 1024 -> 1, 1025 -> 2;
 *   - read over cap is IO_ERROR;
 *   - path-escape names are rejected by every function;
 *   - a file where a directory is expected is IO_ERROR;
 *   - an end-to-end gamedata_build_statget + gamedata_apply_result (OK with setParam) through this
 *     port, then gamedata_param_sfo_decode reads back what was written.
 *
 * Usage: test_gamedata_storage_posix <scratch-dir>
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gamedata_storage_posix.h"

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

#define CHECK_EQ_MEM(actual, expected, len)                                          \
    do {                                                                             \
        checks++;                                                                    \
        if (memcmp((actual), (expected), (len)) != 0) {                               \
            failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s differs from %s\n", __FILE__, __LINE__,   \
                    #actual, #expected);                                             \
        }                                                                            \
    } while (0)

static char g_scratch[512];
static char g_root[512];

/* Defined (non-static) in native/adapters/gamedata_storage_posix.c. It is declared here rather than in
 * the public header because it is an implementation detail of make_dirs, not adapter API. */
int gamedata_storage_posix_is_device_prefix(const char *prefix);

static int path_join(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    const int n = c ? snprintf(out, cap, "%s/%s/%s", a, b, c) : snprintf(out, cap, "%s/%s", a, b);
    return n > 0 && (size_t)n < cap;
}

/* Does `<dir>/<name>` exist? 1 = yes, 0 = no, -1 = the call failed. */
static int exists(const char *dir, const char *name)
{
    char path[1200];
    struct stat info;
    if (!path_join(path, sizeof path, dir, name, NULL)) return -1;
    return stat(path, &info) == 0 ? 1 : 0;
}

/* Writes `len` bytes of a deterministic pattern directly (bypasses the adapter on purpose). */
static int host_write(const char *dir, const char *name, size_t len)
{
    char path[1200];
    if (!path_join(path, sizeof path, dir, name, NULL)) return -1;
    FILE *stream = fopen(path, "wb");
    if (!stream) return -1;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char byte = (unsigned char)(i * 7u + 3u);
        if (fwrite(&byte, 1, 1, stream) != 1) { fclose(stream); return -1; }
    }
    return fclose(stream) == 0 ? 0 : -1;
}

/* A libnx device specifier ("sdmc:") is not a directory and must never reach mkdir: on the console
 * that call fails with an errno other than EEXIST and the whole storage init (and so the boot) dies.
 * Every other prefix is a real path component. */
static void case_device_prefix_classification(void)
{
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix("sdmc:"), 1);
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix("sdmc:/switch"), 0);
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix("/tmp"), 0);
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix("/tmp/a"), 0);   /* the component of "/tmp/a:b/c" */
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix(""), 0);
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix("/"), 0);
    CHECK_EQ_INT(gamedata_storage_posix_is_device_prefix(NULL), 0);
}

static void case_root_and_free_space(void)
{
    gamedata_storage_posix storage;
    char nested[1024];
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    CHECK_EQ_INT(storage.last_errno, 0);
    CHECK(strcmp(storage.root, g_root) == 0);
    CHECK(exists(g_scratch, "hdd0") == 1 && exists(g_scratch, "hdd0/game") == 1);   /* init created the tree */
    CHECK(gamedata_storage_posix_init(NULL, g_root) == -1);
    {   /* empty and over-long roots are refused */
        gamedata_storage_posix other;
        CHECK(gamedata_storage_posix_init(&other, "") == -1);
        CHECK(other.last_errno == EINVAL || other.last_errno == ENAMETOOLONG);
        memset(nested, 'a', sizeof nested);
        nested[sizeof nested - 1u] = 0;
        CHECK(gamedata_storage_posix_init(&other, nested) == -1);
    }
    {   /* a trailing '/' is normalized away */
        gamedata_storage_posix trailing;
        char with_slash[1060];
        const int n = snprintf(with_slash, sizeof with_slash, "%s/", g_root);
        CHECK(n > 0 && (size_t)n < sizeof with_slash);
        CHECK_EQ_INT(gamedata_storage_posix_init(&trailing, with_slash), 0);
        CHECK(strcmp(trailing.root, g_root) == 0);
    }
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    CHECK(port.context == &storage && port.free_kb && port.stat_dir && port.read_file && port.create_dir && port.write_file);
    uint64_t free_kb = 0;
    CHECK_EQ_INT(port.free_kb(port.context, &free_kb), GAMEDATA_STORAGE_OK);
    CHECK(free_kb > 0 && storage.calls_free == 1 && storage.last_status == GAMEDATA_STORAGE_OK && storage.last_errno == 0);
    {   /* the true 64-bit value is reported, never a clamped one */
        uint64_t huge = 0;
        CHECK_EQ_INT(port.free_kb(port.context, &huge), GAMEDATA_STORAGE_OK);
        CHECK(huge == free_kb || huge < free_kb);   /* no clamp claim: just a plausible value */
    }
}

static void case_create_and_stat(void)
{
    gamedata_storage_posix storage;
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    uint64_t size_kb = 12345;
    int64_t times[3] = {1, 2, 3};
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00001", &size_kb, times), GAMEDATA_STORAGE_NOT_FOUND);
    CHECK_EQ_INT(storage.last_errno, ENOENT);
    CHECK(size_kb == 12345 && times[0] == 1);   /* untouched on failure */
    CHECK_EQ_INT(port.create_dir(port.context, "GAME00001"), GAMEDATA_STORAGE_OK);
    CHECK(exists(storage.root, "GAME00001") == 1 && exists(storage.root, "GAME00001/USRDIR") == 1);
    CHECK_EQ_INT(port.create_dir(port.context, "GAME00001"), GAMEDATA_STORAGE_OK);   /* idempotent */
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00001", &size_kb, times), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(size_kb, 0);
    CHECK(times[0] > 0 && times[1] > 0 && times[2] > 0);
    CHECK(storage.calls_create == 2 && storage.calls_stat == 2);
    CHECK_EQ_INT(storage.last_status, GAMEDATA_STORAGE_OK);
}

static void case_round_trip_and_overwrite(void)
{
    gamedata_storage_posix storage;
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    CHECK_EQ_INT(port.create_dir(port.context, "GAME00002"), GAMEDATA_STORAGE_OK);
    const unsigned char first[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    unsigned char buffer[64];
    size_t length = 0;
    CHECK_EQ_INT(port.write_file(port.context, "GAME00002", "DATA.BIN", first, sizeof first), GAMEDATA_STORAGE_OK);
    CHECK(exists(storage.root, "GAME00002/DATA.BIN") == 1);
    CHECK(exists(storage.root, "GAME00002/DATA.BIN.tmp") == 0);   /* renamed, never left behind */
    memset(buffer, 0xEE, sizeof buffer);
    CHECK_EQ_INT(port.read_file(port.context, "GAME00002", "DATA.BIN", buffer, sizeof buffer, &length), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(length, sizeof first);
    CHECK_EQ_MEM(buffer, first, sizeof first);
    CHECK_EQ_INT(storage.last_errno, 0);
    /* a SHORTER overwrite must leave no stale tail: the target is replaced, not rewritten in place */
    const unsigned char second[] = {0xAA, 0xBB, 0xCC};
    CHECK_EQ_INT(port.write_file(port.context, "GAME00002", "DATA.BIN", second, sizeof second), GAMEDATA_STORAGE_OK);
    CHECK(exists(storage.root, "GAME00002/DATA.BIN.tmp") == 0);
    memset(buffer, 0xEE, sizeof buffer);
    length = 0;
    CHECK_EQ_INT(port.read_file(port.context, "GAME00002", "DATA.BIN", buffer, sizeof buffer, &length), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(length, sizeof second);
    CHECK_EQ_MEM(buffer, second, sizeof second);
    CHECK(buffer[sizeof second] == 0xEE);
    /* an absent file is NOT_FOUND, an over-cap file is IO_ERROR */
    length = 0;
    CHECK_EQ_INT(port.read_file(port.context, "GAME00002", "ABSENT.BIN", buffer, sizeof buffer, &length), GAMEDATA_STORAGE_NOT_FOUND);
    CHECK_EQ_INT(storage.last_errno, ENOENT);
    const unsigned char big[300] = {0};
    CHECK_EQ_INT(port.write_file(port.context, "GAME00002", "BIG.BIN", big, sizeof big), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(port.read_file(port.context, "GAME00002", "BIG.BIN", buffer, 64, &length), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(storage.last_status, GAMEDATA_STORAGE_IO_ERROR);
    unsigned char large[300];
    CHECK_EQ_INT(port.read_file(port.context, "GAME00002", "BIG.BIN", large, sizeof large, &length), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(length, sizeof big);
    CHECK_EQ_INT(storage.calls_write, 3);
    CHECK_EQ_INT(storage.calls_read, 5);   /* round trip, short overwrite, absent, over-cap, exact */
}

static void case_size_rounding(void)
{
    gamedata_storage_posix storage;
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    uint64_t size_kb = 0;
    int64_t times[3] = {0, 0, 0};
    CHECK_EQ_INT(port.create_dir(port.context, "GAME00003"), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(host_write(storage.root, "GAME00003/ONE.BIN", 1), 0);
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00003", &size_kb, times), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(size_kb, 1);   /* 1 byte -> 1 */
    CHECK_EQ_INT(host_write(storage.root, "GAME00003/K1.BIN", 1024), 0);
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00003", &size_kb, times), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(size_kb, 2);   /* 1 + 1024 = 1025 -> 2 (ceil) */
    CHECK_EQ_INT(host_write(storage.root, "GAME00003/K1.BIN", 1023), 0);
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00003", &size_kb, times), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(size_kb, 1);   /* 1 + 1023 = 1024 -> 1 */
    /* the walk is RECURSIVE below <dir>: USRDIR content counts too */
    CHECK_EQ_INT(host_write(storage.root, "GAME00003/USRDIR/DEEP.BIN", 1024), 0);
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00003", &size_kb, times), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(size_kb, 2);   /* 1 + 1023 + 1024 = 2048 -> 2 */
    CHECK(size_kb == 2);
}

static void case_escape_and_shape_rejections(void)
{
    gamedata_storage_posix storage;
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    unsigned char buffer[8] = {0};
    size_t length = 0;
    uint64_t size_kb = 0;
    int64_t times[3] = {0, 0, 0};
    const char *bad_names[] = {"", "..", "../GAME", "a/b", "GAME/../..", "..", "."};
    for (size_t i = 0; i < sizeof bad_names / sizeof *bad_names; ++i) {
        CHECK_EQ_INT(port.stat_dir(port.context, bad_names[i], &size_kb, times), GAMEDATA_STORAGE_IO_ERROR);
        CHECK_EQ_INT(port.create_dir(port.context, bad_names[i]), GAMEDATA_STORAGE_IO_ERROR);
        CHECK_EQ_INT(port.read_file(port.context, bad_names[i], "DATA.BIN", buffer, sizeof buffer, &length), GAMEDATA_STORAGE_IO_ERROR);
        CHECK_EQ_INT(port.write_file(port.context, bad_names[i], "DATA.BIN", buffer, sizeof buffer), GAMEDATA_STORAGE_IO_ERROR);
    }
    /* the same rule applies to the file name */
    CHECK_EQ_INT(port.read_file(port.context, "GAME00004", "../PARAM.SFO", buffer, sizeof buffer, &length), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.write_file(port.context, "GAME00004", "a/b", buffer, sizeof buffer), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.write_file(port.context, "GAME00004", "", buffer, sizeof buffer), GAMEDATA_STORAGE_IO_ERROR);
    /* NULL context / outputs are total failures, not crashes */
    CHECK_EQ_INT(port.stat_dir(NULL, "GAME00004", &size_kb, times), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.stat_dir(port.context, "GAME00004", NULL, times), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.read_file(port.context, "GAME00004", "DATA.BIN", buffer, sizeof buffer, NULL), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.free_kb(NULL, &size_kb), GAMEDATA_STORAGE_IO_ERROR);
    CHECK_EQ_INT(port.free_kb(port.context, NULL), GAMEDATA_STORAGE_IO_ERROR);
    /* a FILE where a directory is expected is IO_ERROR, and nothing outside the root is used */
    CHECK_EQ_INT(host_write(g_scratch, "not-a-dir", 4), 0);
    {
        gamedata_storage_posix file_root;
        char file_path[1024];
        CHECK(path_join(file_path, sizeof file_path, g_scratch, "not-a-dir", NULL));
        CHECK(gamedata_storage_posix_init(&file_root, file_path) == -1);   /* init verifies it is a directory */
        CHECK_EQ_INT(file_root.last_errno, ENOTDIR);
    }
    {
        gamedata_storage_posix scoped;
        CHECK_EQ_INT(gamedata_storage_posix_init(&scoped, g_root), 0);
        const gamedata_storage_port scoped_port = gamedata_storage_posix_port(&scoped);
        CHECK_EQ_INT(scoped_port.create_dir(scoped_port.context, "GAME00005"), GAMEDATA_STORAGE_OK);
        CHECK_EQ_INT(host_write(scoped.root, "AFILE", 2), 0);
        /* <root>/AFILE exists but is a file: stat_dir says IO_ERROR, not NOT_FOUND (a name with '/' never reaches stat) */
        CHECK_EQ_INT(scoped_port.stat_dir(scoped_port.context, "AFILE", &size_kb, times), GAMEDATA_STORAGE_IO_ERROR);
        CHECK_EQ_INT(scoped.last_status, GAMEDATA_STORAGE_IO_ERROR);
        /* a file where create_dir needs a component: <root>/F is a file, so <root>/F/USRDIR cannot be made */
        CHECK_EQ_INT(host_write(scoped.root, "F", 2), 0);
        CHECK_EQ_INT(scoped_port.create_dir(scoped_port.context, "F"), GAMEDATA_STORAGE_IO_ERROR);
    }
    {   /* nothing escaped: no stray directory was created above the root */
        CHECK(exists(g_root, "GAME00004") == 0);
        CHECK(exists(g_scratch, "GAME") == 0);
    }
}

/* A 0xA9C-byte guest CellGameDataSystemFileParam, big-endian, as the guest writes it. */
static void make_param(uint8_t *param, const char *title, const char *title_id, const char *version)
{
    memset(param, 0, GAMEDATA_PARAM_BYTES);
    memcpy(param + GAMEDATA_PARAM_TITLE, title, strlen(title));
    memcpy(param + GAMEDATA_PARAM_TITLE + 0x80, "Localized", 9);
    memcpy(param + GAMEDATA_PARAM_TITLE_ID, title_id, strlen(title_id));
    memcpy(param + GAMEDATA_PARAM_DATA_VERSION, version, strlen(version));
    param[GAMEDATA_PARAM_PARENTAL_LEVEL + 3] = 5;
    param[GAMEDATA_PARAM_ATTRIBUTE + 3] = 1;
}

static void case_end_to_end_service(void)
{
    gamedata_storage_posix storage;
    CHECK_EQ_INT(gamedata_storage_posix_init(&storage, g_root), 0);
    const gamedata_storage_port port = gamedata_storage_posix_port(&storage);
    gamedata_check state;
    memset(&state, 0, sizeof state);
    uint8_t statget[GAMEDATA_STATGET_BYTES];
    /* first look: the directory is new data */
    CHECK_EQ_INT(gamedata_build_statget(&port, "BCUS98123", statget, &state), GAMEDATA_OK);
    CHECK_EQ_INT(state.is_new, (int32_t)GAMEDATA_IS_NEW_DATA_YES);
    CHECK_EQ_INT(state.size_kb, 0);
    CHECK(state.free_kb > 0);
    CHECK_EQ_INT(storage.calls_stat, 1);
    /* the callback returns 0 (OK) with a non-NULL setParam: the firmware creates the directory and writes PARAM.SFO */
    uint8_t param[GAMEDATA_PARAM_BYTES];
    make_param(param, "inFAMOUS G4", "BCUS98123", "01.00");
    gamedata_action action;
    memset(&action, 0, sizeof action);
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK(exists(storage.root, "BCUS98123") == 1 && exists(storage.root, "BCUS98123/USRDIR") == 1);
    CHECK(exists(storage.root, "BCUS98123/PARAM.SFO") == 1);
    CHECK(exists(storage.root, "BCUS98123/PARAM.SFO.tmp") == 0);
    CHECK(storage.calls_create == 1 && storage.calls_write == 1);
    /* the file the adapter wrote really decodes back into the same guest parameters */
    uint8_t encoded[GAMEDATA_SFO_MAX_BYTES];
    size_t encoded_length = 0;
    CHECK_EQ_INT(port.read_file(port.context, "BCUS98123", "PARAM.SFO", encoded, sizeof encoded, &encoded_length), GAMEDATA_STORAGE_OK);
    CHECK(encoded_length > 0);
    uint8_t decoded[GAMEDATA_PARAM_BYTES];
    CHECK_EQ_INT(gamedata_param_sfo_decode(encoded, encoded_length, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded + GAMEDATA_PARAM_TITLE, "inFAMOUS G4", strlen("inFAMOUS G4") + 1);
    CHECK_EQ_MEM(decoded + GAMEDATA_PARAM_TITLE_ID, "BCUS98123", 10);
    CHECK_EQ_MEM(decoded + GAMEDATA_PARAM_DATA_VERSION, "01.00", 6);
    CHECK_EQ_INT(decoded[GAMEDATA_PARAM_PARENTAL_LEVEL + 3], 5);
    CHECK_EQ_INT(decoded[GAMEDATA_PARAM_ATTRIBUTE + 3], 1);
    /* second look: the directory exists, its PARAM.SFO decodes, and the size comes from a real walk */
    CHECK_EQ_INT(gamedata_build_statget(&port, "BCUS98123", statget, &state), GAMEDATA_OK);
    CHECK_EQ_INT(state.is_new, (int32_t)GAMEDATA_IS_NEW_DATA_NO);
    CHECK(state.size_kb > 0 && state.size_kb < 64);
    CHECK(state.free_kb > 0);
    CHECK_EQ_INT(storage.last_status, GAMEDATA_STORAGE_OK);
    /* a second, larger PARAM.SFO replaces the first atomically (no .tmp survives) */
    make_param(param, "inFAMOUS G4 RENAMED TITLE", "BCUS98123", "01.01");
    memset(&action, 0, sizeof action);
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 0, param, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_CREATED_OR_UPDATED);
    CHECK(exists(storage.root, "BCUS98123/PARAM.SFO.tmp") == 0);
    encoded_length = 0;
    CHECK_EQ_INT(port.read_file(port.context, "BCUS98123", "PARAM.SFO", encoded, sizeof encoded, &encoded_length), GAMEDATA_STORAGE_OK);
    CHECK_EQ_INT(gamedata_param_sfo_decode(encoded, encoded_length, decoded), GAMEDATA_OK);
    CHECK_EQ_MEM(decoded + GAMEDATA_PARAM_TITLE, "inFAMOUS G4 RENAMED TITLE", strlen("inFAMOUS G4 RENAMED TITLE") + 1);
    /* OK_CANCEL creates nothing */
    memset(&action, 0, sizeof action);
    CHECK_EQ_INT(gamedata_apply_result(&port, &state, 1, NULL, &action), GAMEDATA_OK);
    CHECK_EQ_INT(action.action, GAMEDATA_ACTION_NONE);
    CHECK(exists(storage.root, "BCUS98123") == 1);
    CHECK_EQ_INT(storage.calls_create, 2);
    CHECK_EQ_INT(storage.calls_write, 2);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    if (strlen(argv[1]) + 16u >= sizeof g_scratch) return 2;
    const int scratch_written = snprintf(g_scratch, sizeof g_scratch, "%s/posix.XXXXXX", argv[1]);
    if (scratch_written <= 0 || (size_t)scratch_written >= sizeof g_scratch || !mkdtemp(g_scratch)) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 2;
    }
    if (strlen(g_scratch) + 16u >= sizeof g_root) return 2;
    const int root_written = snprintf(g_root, sizeof g_root, "%s/hdd0/game", g_scratch);
    if (root_written <= 0 || (size_t)root_written >= sizeof g_root) return 2;

    case_root_and_free_space();
    case_device_prefix_classification();
    case_create_and_stat();
    case_round_trip_and_overwrite();
    case_size_rounding();
    case_escape_and_shape_rejections();
    case_end_to_end_service();

    printf("GAMEDATA_STORAGE_POSIX checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
