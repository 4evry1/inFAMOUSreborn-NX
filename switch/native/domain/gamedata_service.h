#ifndef GAMEDATA_SERVICE_H
#define GAMEDATA_SERVICE_H
/* Platform-free, instance-owned native game-data service for the PS3 `cellSysutil`
 * game-data family (`cellGameDataCheckCreate2`, NID 0xC9645C41) as recovered from the firmware.
 *
 * The service owns no globals and makes no OS calls. All storage access arrives through an
 * injected `gamedata_storage_port`; the caller decides what "create directory", "write
 * PARAM.SFO" and "read the free-space counter" mean on the host. Guest memory is never
 * touched: records are produced as big-endian byte buffers the adapter copies out.
 *
 * Contract sources (report section in parentheses):
 *   §1.3  the 8688-byte (0x21F0) firmware frame and the three records handed to funcStat:
 *         CellGameDataCBResult at frame+0xD0 (the game writes +0xC), CellGameDataStatGet at
 *         frame+0xCF8 (0x1448 bytes, zeroed by the firmware memset before the fill), and
 *         CellGameDataStatSet at frame+0x78 (8 bytes, zeroed; +0 setParam, +4 reserved);
 *   §1.3  the per-field guest offsets of CellGameDataStatGet (not a bulk copy);
 *   §1.5  the firmware's dispatch on cbResult->result;
 *   §1.6  the return-register quirk;
 *   §3/§5 the cache-supplied values, including the documented `sysSizeKB` deviation.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte sizes (report §1.3). All big-endian in guest memory. */
#define GAMEDATA_STATGET_BYTES 0x1448u  /* CellGameDataStatGet, frame+0xCF8 */
#define GAMEDATA_CBRESULT_BYTES 16u     /* CellGameDataCBResult, frame+0xD0; the game writes +0xC */
#define GAMEDATA_STATSET_BYTES 8u       /* CellGameDataStatSet, frame+0x78 */
#define GAMEDATA_PARAM_BYTES 0xA9Cu     /* CellGameDataSystemFileParam (statGet->getParam) */
#define GAMEDATA_DIRNAME_MAX 32u        /* sysutil_check_name_string maxlen */

/* The firmware stack frame (8688 bytes). */
#define GAMEDATA_FIRMWARE_FRAME 8688u
#define GAMEDATA_FRAME_CBRESULT 0xD0u
#define GAMEDATA_FRAME_STATGET 0xCF8u
#define GAMEDATA_FRAME_STATSET 0x78u
#define GAMEDATA_FRAME_REQUIRED (GAMEDATA_FRAME_STATGET + GAMEDATA_STATGET_BYTES)

/* CellGameDataStatGet guest offsets (report §1.3). Everything not named here is zero. */
#define GAMEDATA_STATGET_HDD_FREE_SIZE_KB 0x000u
#define GAMEDATA_STATGET_IS_NEW_DATA 0x004u
#define GAMEDATA_STATGET_CONTENT_INFO_PATH 0x008u
#define GAMEDATA_STATGET_CONTENT_INFO_PATH_BYTES 0x41Fu /* incl. NUL */
#define GAMEDATA_STATGET_GAME_DATA_PATH 0x427u
#define GAMEDATA_STATGET_GAME_DATA_PATH_BYTES 0x421u   /* incl. NUL */
#define GAMEDATA_STATGET_ST_ATIME 0x848u
#define GAMEDATA_STATGET_ST_CTIME 0x850u
#define GAMEDATA_STATGET_ST_MTIME 0x858u
#define GAMEDATA_STATGET_GET_PARAM 0x860u
#define GAMEDATA_STATGET_SIZE_KB 0x13FCu
#define GAMEDATA_STATGET_SYS_SIZE_KB 0x1400u

/* CellGameDataSystemFileParam guest offsets (report §1.3). */
#define GAMEDATA_PARAM_TITLE 0x000u
#define GAMEDATA_PARAM_TITLE_LANG 0x080u
#define GAMEDATA_PARAM_TITLE_LANG_COUNT 20u
#define GAMEDATA_PARAM_TITLE_SIZE 128u
#define GAMEDATA_PARAM_TITLE_ID 0xA80u
#define GAMEDATA_PARAM_TITLE_ID_SIZE 10u
#define GAMEDATA_PARAM_RESERVED0 0xA8Au
#define GAMEDATA_PARAM_DATA_VERSION 0xA8Cu
/* dataVersion is a 6-byte guest field followed by reserved1[2]: an 8-byte, NUL-terminated
 * window. The field keeps the terminator inside `dataVersion[8]` (§1.3 / §7); the approved
 * design also fixes VERSION (PARAM.SFO) max_len at 8, which rules out 6. */
#define GAMEDATA_PARAM_DATA_VERSION_SIZE 8u
#define GAMEDATA_PARAM_RESERVED1 0xA92u
#define GAMEDATA_PARAM_PARENTAL_LEVEL 0xA94u
#define GAMEDATA_PARAM_ATTRIBUTE 0xA98u

/* PARAM.SFO string max_len per key (report-independent format constant chosen by this
 * service; PARAM.SFO v1.1 `fmt 0x0204` utf8 needs the terminator inside max_len). */
#define GAMEDATA_SFO_TITLE_MAX 128u
#define GAMEDATA_SFO_TITLE_ID_MAX 16u
#define GAMEDATA_SFO_VERSION_MAX 8u

#define GAMEDATA_SFO_MAX_BYTES 0x10000u /* read_file cap for PARAM.SFO */

/* Values the firmware writes into CellGameDataStatGet (report §1.3/§3). */
#define GAMEDATA_IS_NEW_DATA_NO 0u
#define GAMEDATA_IS_NEW_DATA_YES 1u

/* Storage port status. Every port function is total: it returns one of these and never
 * throws. READ_FILE reports NOT_FOUND when the file is absent. */
typedef enum gamedata_storage_status {
    GAMEDATA_STORAGE_OK = 0,
    GAMEDATA_STORAGE_NOT_FOUND = 1,
    GAMEDATA_STORAGE_NO_SPACE = 2,
    GAMEDATA_STORAGE_IO_ERROR = 3
} gamedata_storage_status;

/* The injected storage port. `dir_name` is the validated NUL-terminated game-data
 * directory name (no path separators, no leading '_'); the port owns the "/dev_hdd0/game"
 * prefix and everything below it.
 *
 *   free_kb    writes the current free size in KB.
 *   stat_dir   NOT_FOUND when <dir>/ is absent; otherwise writes the directory size in KB
 *              and times[3] = {atime, ctime, mtime} in seconds.
 *   read_file  copies at most cap bytes of <dir>/<file> and writes the length; NOT_FOUND
 *              when the file is absent.
 *   create_dir creates <dir>/ and <dir>/USRDIR, idempotent.
 *   write_file atomically replaces <dir>/<file>. */
typedef struct gamedata_storage_port {
    void *context;
    gamedata_storage_status (*free_kb)(void *context, uint64_t *out_kb);
    gamedata_storage_status (*stat_dir)(void *context, const char *dir_name, uint64_t *size_kb,
                                        int64_t times[3]);
    gamedata_storage_status (*read_file)(void *context, const char *dir_name, const char *file,
                                         uint8_t *buf, size_t cap, size_t *len);
    gamedata_storage_status (*create_dir)(void *context, const char *dir_name);
    gamedata_storage_status (*write_file)(void *context, const char *dir_name, const char *file,
                                          const uint8_t *buf, size_t len);
} gamedata_storage_port;

/* Service status. Distinct from the guest return value, which is always 0 (§1.6). */
typedef enum gamedata_status {
    GAMEDATA_OK = 0,
    GAMEDATA_BAD_ARGUMENT = 1, /* NULL port, NULL port function, NULL output, nameless dir */
    GAMEDATA_BROKEN_PARAM = 2, /* PARAM.SFO exists but cannot be decoded (§1.5 ERR_BROKEN) */
    GAMEDATA_STORAGE_ERROR = 3 /* any port failure on a required operation */
} gamedata_status;

/* What the callback result asked the firmware to do (report §1.5). */
typedef enum gamedata_action_kind {
    GAMEDATA_ACTION_NONE = 0,
    GAMEDATA_ACTION_CREATED_OR_UPDATED = 1, /* directory created and PARAM.SFO written */
    GAMEDATA_ACTION_CBRESULT_ERROR = 2,     /* -1/-3/-4/-5: dialog only, no disk effect */
    GAMEDATA_ACTION_PARAM_ERROR = 3,        /* -2, or OK with new data and no setParam */
    GAMEDATA_ACTION_UNKNOWN_RESULT = 4      /* outside [-5,1] */
} gamedata_action_kind;

typedef struct gamedata_action {
    gamedata_action_kind action;
} gamedata_action;

/* Per-call state. Instance-owned: the caller allocates it, the service only writes it. */
typedef struct gamedata_check {
    char dir_name[GAMEDATA_DIRNAME_MAX + 1u];
    uint32_t dir_name_length;
    int32_t is_new;    /* StatGet isNewData as built */
    int32_t size_kb;   /* StatGet sizeKB, the cached value `cellGameDataGetSizeKB` returns (§2) */
    int32_t free_kb;   /* StatGet hddFreeSizeKB after the INT32_MAX clamp */
} gamedata_check;

/* ------------------------------------------------------------------------------------ */
/* Name validation                                                                       */
/* ------------------------------------------------------------------------------------ */

/* `sysutil_check_name_string(src, minlen=1, maxlen=0x20)`
 * reduced to the firmware rule:
 *
 *   the SDK version selects the terminator boundary:
 *       sdk_version > 0x36FFFF  -> last position 31  (max_len = 31)
 *       otherwise               -> max_len = 32
 *   return -1 if src[0] == '_' or any byte is outside A-Z 0-9 '-' '_';
 *   return -2 if the string length is 0 or greater than max_len, or if no NUL is found
 *             inside `readable` bytes;
 *   return 0 otherwise.
 *
 * Quoting the report (§1.2): "It reads the SDK version first ..., and chooses the
 * terminator boundary: SDK > 0x36FFFF -> last position maxlen-1 = 31, else maxlen = 32. It
 * then: rejects src[0] == '_' -> -1; rejects any byte outside A-Z, 0-9, -, _ -> -1; accepts
 * end-of-string only when the length is in [1,32] and the last position rule holds;
 * otherwise -2." The body turns any nonzero validator result into an immediate return of
 * that value (§1.2: "i.e. -1/-2, not a CELL_GAMEDATA_ERROR_* constant").
 *
 * `readable` is how many bytes of `src` the caller may read. Nothing past `readable` is
 * touched. */
int32_t gamedata_validate_dir_name(const uint8_t *src, size_t readable, uint32_t sdk_version);

/* ------------------------------------------------------------------------------------ */
/* Records                                                                               */
/* ------------------------------------------------------------------------------------ */

/* Builds a CellGameDataStatGet (report §1.3, 0x1448 bytes, big-endian).
 *
 * `out` is zero-filled first (the firmware memset at 0x268), then:
 *   +0x000 hddFreeSizeKB = min(free_kb, INT32_MAX)
 *   +0x004 isNewData
 *   +0x008 contentInfoPath = "/dev_hdd0/game/<dir>"
 *   +0x427 gameDataPath    = "/dev_hdd0/game/<dir>/USRDIR"
 *   +0x848 st_atime, +0x850 st_ctime, +0x858 st_mtime (seconds)
 *   +0x860 getParam (0xA9C bytes, decoded from PARAM.SFO when the directory exists)
 *   +0x13FC sizeKB = min(size_kb, INT32_MAX)
 *   +0x1400 sysSizeKB = 0
 *
 * Documented deviation: `sysSizeKB` is always 0. The system-cache value it mirrors in
 * firmware is not determinate offline (report §5: the size values "are supplied by the
 * system cache over shared memory ... they cannot be derived from the module image
 * alone"). Nothing else in the record deviates. When `stat_dir` reports NOT_FOUND the
 * record is "new data": isNewData = 1, sizeKB = 0, times = 0, getParam all zero.
 *
 * A free_kb result of NO_SPACE is a valid outcome, not a port failure: the record then
 * carries hddFreeSizeKB = 0 (a full device reports 0 KB free). NO_SPACE from stat_dir is a
 * genuine error and returns GAMEDATA_STORAGE_ERROR.
 *
 * Returns GAMEDATA_BROKEN_PARAM when the directory exists but its PARAM.SFO cannot be
 * decoded; `out` then holds the zero fill and nothing else. An unreadable (not merely
 * absent) PARAM.SFO is a port failure and returns GAMEDATA_STORAGE_ERROR. Any other port
 * failure returns GAMEDATA_STORAGE_ERROR too. An invalid `dir_name` returns
 * GAMEDATA_BAD_ARGUMENT before any port call. */
gamedata_status gamedata_build_statget(const gamedata_storage_port *port, const char *dir_name,
                                       uint8_t out[GAMEDATA_STATGET_BYTES],
                                       gamedata_check *state);

/* Applies the callback result exactly as the firmware does in report §1.5.
 *
 *   result  0  OK            setParam != NULL: create_dir, then write PARAM.SFO encoded from
 *                            the 0xA9C-byte guest setParam -> CREATED_OR_UPDATED.
 *                            setParam == NULL and state->is_new: PARAM_ERROR (no disk effect).
 *                            setParam == NULL and existing: NONE.
 *   result  1  OK_CANCEL     NONE.
 *   result -1,-3,-4,-5       CBRESULT_ERROR (never a dialog: errDialog belongs to the adapter).
 *   result -2                PARAM_ERROR.
 *   otherwise                UNKNOWN_RESULT.
 *
 * A port failure inside the OK arm returns GAMEDATA_STORAGE_ERROR and sets
 * `action->action` to GAMEDATA_ACTION_NONE. Every other outcome returns GAMEDATA_OK.
 *
 * `set_param` is NULL or 0xA9C bytes copied verbatim from guest getParam (big-endian, as the
 * guest wrote it); the encoder translates it. An unusable port is only a failure on the arm
 * that needs it (result 0 with a non-NULL setParam). */
gamedata_status gamedata_apply_result(const gamedata_storage_port *port, gamedata_check *state,
                                      int32_t result, const uint8_t *set_param,
                                      gamedata_action *action);

/* The value `cellGameDataCheckCreate2` actually leaves in r3 (report §1.6): 0.
 *
 * Quoting the report (§1.6): "Every post-callback arm converges on 0x6CC0. That path ...
 * leaves r3 = 0 ... The CELL_GAMEDATA_ERROR_* value lives in r31 and is restored, not
 * returned." The GP value constants the firmware loads (0x8002B601, 0x8002B604,
 * 0x8002BA04) never reach r3, and the game does not branch on the return value (§7).
 *
 * This is the firmware/image quirk; do not "fix" it here. What the guest asked for is
 * reported through `gamedata_apply_result`'s `action` instead. */
int32_t gamedata_guest_return(void);

/* ------------------------------------------------------------------------------------ */
/* PARAM.SFO                                                                            */
/* ------------------------------------------------------------------------------------ */

/* Encodes `param` (0xA9C guest getParam bytes of the layout above) as a PARAM.SFO v1.1 file.
 *
 * Format: magic "\0PSF", version 0x0101, little-endian header and index, key table, and a
 * 4-byte-aligned data table. Emitted keys, sorted bytewise:
 *   ATTRIBUTE (fmt 0x0404 int32), CATEGORY ("GD", utf8), PARENTAL_LEVEL (int32),
 *   TITLE, TITLE_00..TITLE_19 (only non-empty titleLang entries), TITLE_ID, VERSION.
 * String max_len: TITLE* 128, TITLE_ID 16, VERSION 8.
 *
 * A TITLE* string that does not terminate inside its 128-byte guest field is truncated to
 * 127 bytes plus NUL, so the encoded value still terminates inside max_len = 128.
 *
 * Returns the number of bytes written, or 0 when the data does not fit in `cap` (or a
 * string has no NUL inside its max_len). Nothing is written past `cap`. */
size_t gamedata_param_sfo_encode(const uint8_t param[GAMEDATA_PARAM_BYTES], uint8_t *out,
                                 size_t cap);

/* Decodes a PARAM.SFO v1.1 image into the 0xA9C-byte guest getParam layout.
 *
 * `param_out` is zero-filled first. Any key order is accepted. Unknown keys are ignored.
 * The decoded keys are exactly the fields `CellGameDataSystemFileParam` carries: TITLE,
 * TITLE_00..TITLE_19, TITLE_ID, VERSION -> dataVersion, PARENTAL_LEVEL, ATTRIBUTE. Nothing
 * outside the guest field is inferred (localisation of APP_VER is an adapter policy, not part
 * of this record contract).
 *
 * Rejects (GAMEDATA_BROKEN_PARAM, `param_out` left zero-filled): bad magic, a truncated
 * header or index, a table offset past the end, a data offset/len past the end, a value that
 * overflows its guest field, and a string with no NUL inside its max_len. */
gamedata_status gamedata_param_sfo_decode(const uint8_t *sfo, size_t len,
                                          uint8_t param_out[GAMEDATA_PARAM_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* GAMEDATA_SERVICE_H */
