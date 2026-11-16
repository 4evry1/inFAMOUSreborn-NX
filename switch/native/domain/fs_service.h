#ifndef FS_SERVICE_H
#define FS_SERVICE_H
/* Platform-free, instance-owned native file-system service for the six `sys_fs` entries
 * batch B2 needed on the game's path to its first frame, as extracted from the firmware
 * and designed in
 * DESIGN.md, with decision 24 of evidence/root-gcm-next-boundary-prep-r1/DECISIONS.md
 * overriding DESIGN.md §5.
 *
 *   0x3F61245C  cellFsOpendir     lv2 sc 805
 *   0x5C74903D  cellFsReaddir     lv2 sc 806
 *   0xFF42DCC3  cellFsClosedir    lv2 sc 807
 *   0x7DE6DCED  cellFsStat        lv2 sc 808
 *   0x2796FDF3  cellFsRmdir       lv2 sc 813
 *   0x7F4677A8  cellFsUnlink      lv2 sc 814
 *
 * The service owns no globals and makes no OS calls. All storage access arrives through an
 * injected `fs_storage_port`; the caller decides what a host path means there (a POSIX
 * adapter, a test fake, or nothing at all). Guest memory is never touched: every record the
 * firmware writes is produced as a big-endian byte buffer the adapter copies out.
 *
 * What decision 24 changes against DESIGN.md §5:
 *   - guest mounts map under a configurable host root token (production root token
 *     `sdmc:/switch/infamous-native-spu-stage31`), one host directory per mount, with
 *     strict confinement: `..` is clamped at the mount root and never escapes, and a host
 *     path the guest can name is only ever `<mount host token>/<confined components>`;
 *   - `cellFsRmdir`/`cellFsUnlink` really delete, but only inside that root. They never
 *     return EROFS for a confined path.
 *
 * Contract sources (document in parentheses):
 *   CONTRACT.md §1     the six entries and their lv2 syscall numbers;
 *   CONTRACT.md §1a/§1b the EFAULT guards the public wrappers apply before any syscall;
 *   CONTRACT.md §1c    `sizeof(CellFsStat) == 52`, all big-endian, field order per
 *                      filesystem-reference.md §ABI;
 *   CONTRACT.md §3     the compact dirent: byte 0 is the 1/2 type tag the guest compares,
 *                      the name follows at +2;
 *   CALLS.md §3a       the walker: `nread == 0` is the only end-of-directory signal, and
 *                      the same handle is read again and again — so index-based, never
 *                      stream-based (see fs_readdir's note);
 *   filesystem-reference.md §"Virtual paths"  max 1024 + NUL, non-absolute -> ENOENT,
 *                      too long -> ENAMETOOLONG, root parent clamped, mountpoint 31.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* Guest error codes (filesystem-reference.md §"Flags, errors and operations")            */
/* ------------------------------------------------------------------------------------ */

/* These are the unsigned 32-bit `0x8001xxxx` bit patterns the firmware leaves in r3 after
 * the sign extension of the result -- i.e. negative int32 values in the guest's register. The
 * macros carry that sign, exactly as `pad_service.h` does for CELL_PAD_*. */
#define FS_OK 0
#define FS_ERROR_ENOENT ((int32_t)0x80010006)      /* CELL_FS_ENOENT */
#define FS_ERROR_EINVAL ((int32_t)0x80010002)      /* CELL_FS_EINVAL */
#define FS_ERROR_EPERM ((int32_t)0x80010009)       /* CELL_FS_EPERM */
#define FS_ERROR_EEXIST ((int32_t)0x80010014)      /* CELL_FS_EEXIST */
#define FS_ERROR_EISDIR ((int32_t)0x80010012)      /* CELL_FS_EISDIR */
#define FS_ERROR_EROFS ((int32_t)0x80010026)       /* CELL_FS_EROFS (never used for a confined path) */
#define FS_ERROR_EACCES ((int32_t)0x80010029)      /* CELL_FS_EACCES */
#define FS_ERROR_EBADF ((int32_t)0x8001002A)       /* CELL_FS_EBADF */
#define FS_ERROR_EIO ((int32_t)0x8001002B)         /* CELL_FS_EIO */
#define FS_ERROR_EMFILE ((int32_t)0x8001002C)      /* CELL_FS_EMFILE */
#define FS_ERROR_ENOTDIR ((int32_t)0x8001002E)     /* CELL_FS_ENOTDIR */
#define FS_ERROR_ENAMETOOLONG ((int32_t)0x80010034) /* CELL_FS_ENAMETOOLONG */
#define FS_ERROR_ENOTSUP ((int32_t)0x80010037)     /* CELL_FS_ENOTSUP */
#define FS_ERROR_EFAULT ((int32_t)0x8001000D)      /* CELL_FS_EFAULT, the wrapper guards */

/* ------------------------------------------------------------------------------------ */
/* Bounds                                                                                */
/* ------------------------------------------------------------------------------------ */

/* filesystem-reference.md: "max1024 plus terminator". Includes the NUL. */
#define FS_PATH_MAX 1025u
/* filesystem-reference.md: "mountpoint31". */
#define FS_MOUNTPOINT_MAX 31u
/* The guest dirent's name field. CONTRACT.md §3: the walker's name lives at sp+0xDE (dirent
 * + 2) and it copies at most 0x10 bytes from there (CALLS.md §3a, the constant 0x104 at
 * ppu_recomp_000.cpp:280349 is the string's capacity, and `func_00362908` truncates to it),
 * so 16 bytes is the field this caller needs. */
/* Root (2026-11-11, Eden r31): was 16. The real CellFsDirent is u8 d_type, u8 d_namlen, char d_name[256]
 * (258 bytes); with 16 the game's installer saw "infamous1.psarc_" and "jbintro_blastwal" and every stat of
 * such a name failed. */
#define FS_DIRENT_NAME_MAX 255u
/* `sizeof(CellFsStat)`, measured twice in the firmware wrapper (the constant 52
 * is loaded in both places). */
#define FS_CELLFS_STAT_BYTES 52u

/* Guest access flags, the lv2 octal values. */
#define FS_O_RDONLY 000000u
#define FS_O_WRONLY 000001u
#define FS_O_RDWR 000002u
#define FS_O_ACCMODE 000003u
#define FS_O_CREAT 000100u
#define FS_O_EXCL 000200u
#define FS_O_TRUNC 001000u
#define FS_O_APPEND 002000u
#define FS_O_SUPPORTED (FS_O_ACCMODE | FS_O_CREAT | FS_O_EXCL | FS_O_TRUNC | FS_O_APPEND)
#define FS_SEEK_SET 0u
#define FS_SEEK_CUR 1u
#define FS_SEEK_END 2u
/* Guest file descriptors 3..63. */
#define FS_FILE_CAPACITY 61u
#define FS_FILE_FD_BASE 3u

/* d_type + d_namlen, then the name, then one NUL: 2 + 16 + 1 = 19 bytes. The encoder writes
 * exactly this many bytes and nothing beyond; a larger guest buffer is left untouched. */
#define FS_CELLFS_DIRENT_BYTES (2u + FS_DIRENT_NAME_MAX + 1u)

/* ------------------------------------------------------------------------------------ */
/* Port                                                                                  */
/* ------------------------------------------------------------------------------------ */

/* Cells are the guest mount prefixes this service can carry (DESIGN.md §2 + decision 24).
 * A mount is optional: only the ones the caller configures exist, and a path under an
 * unconfigured mount is ENOENT. */
#define FS_MOUNT_CAPACITY 5u
/* Directory handles alive at once. CALLS.md §3a: the walker holds the parent handle across
 * the recursive call, so at least one descriptor per recursion depth is live. */
#define FS_DESCRIPTOR_CAPACITY 32u

/* Port status. Every function is total: it returns one of these and never throws. The port
 * never sees a guest error code and never sees a path outside the root it was configured
 * with — the domain resolves and confines first. */
typedef enum fs_storage_status {
    FS_STORAGE_OK = 0,
    FS_STORAGE_NOT_FOUND = 1, /* the path does not exist */
    FS_STORAGE_NOT_A_DIRECTORY = 2,
    FS_STORAGE_IS_A_DIRECTORY = 3,
    FS_STORAGE_ACCESS_DENIED = 4,
    FS_STORAGE_NOT_SUPPORTED = 5,
    FS_STORAGE_IO_ERROR = 6, /* any other host failure */
    FS_STORAGE_NAME_TOO_LONG = 7,
    FS_STORAGE_ALREADY_EXISTS = 8
} fs_storage_status;

/* What one host path is. Enough for `fs_encode_cellfs_stat` and for the dirent type tag;
 * deliberately not a host `struct stat` (CONTRACT.md §1c: "do not copy a host stat struct or
 * assume host time_t layout"). */
typedef struct fs_stat_info {
    uint32_t is_directory; /* 1 -> IFDIR, 0 -> IFREG */
    uint32_t mode;         /* permission bits only (0777 mask); the type bits come from is_directory */
    int32_t uid;
    int32_t gid;
    int64_t atime; /* seconds */
    int64_t mtime;
    int64_t ctime;
    uint64_t size;
    uint64_t blksize;
} fs_stat_info;

/* One directory entry as the host reports it. `name` is a NUL-terminated single component:
 * never ".", never "..", never containing '/'. */
#define FS_DIR_ENTRY_NAME_MAX 256u

typedef struct fs_dir_entry {
    uint32_t is_directory; /* 1 -> the guest tag is 2, 0 -> 1 */
    uint32_t name_length;  /* bytes in `name`, excluding the NUL */
    char name[FS_DIR_ENTRY_NAME_MAX];
} fs_dir_entry;

/* The injected storage port. `host_path` is always a fully resolved, confined path produced
 * by `fs_resolve_path`; the port never parses a guest string.
 *
 *   stat      fills `out`; NOT_FOUND when the path is absent.
 *   open_dir  opens `host_path` for enumeration and writes an opaque handle. The handle is
 *             freed by close_dir and by nothing else.
 *   read_dir  publishes entry `index` (0-based, in the host's enumeration order, with "."
 *             and ".." skipped). `*out_has_entry` is 0 when `index` is past the last entry;
 *             the service then reports nread == 0. Random access is required because the
 *             guest reads one handle repeatedly and may interleave that with other calls
 *             (CALLS.md §3a); a stream cursor would couple the two.
 *   close_dir closes `handle`. Called exactly once per successful open_dir.
 *   remove_dir removes an empty directory. NOT_A_DIRECTORY when the path is a file,
 *             NOT_FOUND when absent, NOT_SUPPORTED when the host refuses non-empty removal.
 *   unlink_file removes a regular file. IS_A_DIRECTORY when the path is a directory,
 *             NOT_FOUND when absent.
 *   make_dir  creates one directory. ALREADY_EXISTS when the path exists at all (a file or a
 *             directory); NOT_FOUND when the parent is absent; NOT_A_DIRECTORY when the
 *             parent is a file. Never recursive.
 *   rename_path renames a file or a directory. NOT_FOUND when `host_from` is absent. An
 *             existing regular-file `host_to` is replaced; an existing directory `host_to`
 *             is ALREADY_EXISTS. A host that will not replace (a FAT-style card, as on the
 *             Switch's `sdmc:` fsdev) reports ALREADY_EXISTS and the service reports
 *             FS_ERROR_EEXIST unchanged, which is what lv2 does there too.
 *   sync_file flushes one open file handle to storage. NOT_SUPPORTED when the host file
 *             system has no flush at all (a host that answers EINVAL/ENOTSUP).
 *
 * Any function pointer may be NULL, which the service reports as FS_ERROR_ENOTSUP rather
 * than pretending to succeed. */
typedef struct fs_storage_port {
    void *context;
    fs_storage_status (*stat)(void *context, const char *host_path, fs_stat_info *out);
    fs_storage_status (*open_dir)(void *context, const char *host_path, void **out_handle);
    fs_storage_status (*read_dir)(void *context, void *handle, uint32_t index, fs_dir_entry *out,
                                  uint32_t *out_has_entry);
    fs_storage_status (*close_dir)(void *context, void *handle);
    fs_storage_status (*remove_dir)(void *context, const char *host_path);
    fs_storage_status (*unlink_file)(void *context, const char *host_path);

    /* `access` is FS_O_RDONLY / FS_O_WRONLY / FS_O_RDWR. create, exclusive and truncate
     * are 0 or 1. NOT_FOUND when absent and !create; IS_A_DIRECTORY for a directory;
     * ALREADY_EXISTS when create && exclusive and it exists. */
    fs_storage_status (*open_file)(void *context, const char *host_path, uint32_t access,
                                   uint32_t create, uint32_t exclusive, uint32_t truncate,
                                   void **out_handle);
    /* Reads up to `length` bytes at absolute `offset`. *out_read < length only at end of file. */
    fs_storage_status (*read_file)(void *context, void *handle, uint64_t offset, uint8_t *buffer,
                                   uint32_t length, uint32_t *out_read);
    /* Writes `length` bytes at absolute `offset` (extending the file). *out_written is the
     * count written. */
    fs_storage_status (*write_file)(void *context, void *handle, uint64_t offset,
                                    const uint8_t *buffer, uint32_t length,
                                    uint32_t *out_written);
    fs_storage_status (*file_size)(void *context, void *handle, uint64_t *out_size);
    fs_storage_status (*close_file)(void *context, void *handle); /* exactly once per open */
    fs_storage_status (*make_dir)(void *context, const char *host_path);
    fs_storage_status (*rename_path)(void *context, const char *host_from, const char *host_to);
    fs_storage_status (*sync_file)(void *context, void *handle);
} fs_storage_port;

/* ------------------------------------------------------------------------------------ */
/* Service                                                                               */
/* ------------------------------------------------------------------------------------ */

typedef struct fs_mount {
    char guest[FS_MOUNTPOINT_MAX + 1u]; /* "/dev_hdd0", ... never with a trailing '/' */
    char host[224];                     /* host-side token: "<root token>/<leaf>", e.g.
                                         * "sdmc:/switch/infamous-native-spu-stage31/hdd0" */
    uint32_t present;
    uint32_t case_insensitive; /* 1 only for "/dev_bdvd": the disc's file system matches
                                * ASCII case, while the PS3 hard disk and the other mounts are
                                * exact. Set by fs_mount_add. */
} fs_mount;

/* One entry of the bounded directory-handle registry. */
typedef struct fs_descriptor {
    uint32_t in_use;
    uint32_t generation; /* bumped on every close; guest fd = (generation << 8) | index */
    uint32_t cursor;     /* next entry index this handle will publish (fs_readdir) */
    void *handle;        /* port-owned */
    char host_path[FS_PATH_MAX + 224u]; /* the confined path this handle was opened on */
} fs_descriptor;

/* One entry of the bounded regular-file registry. Guest fd = FS_FILE_FD_BASE + index. */
typedef struct fs_file {
    uint32_t in_use;
    uint32_t access;     /* FS_O_RDONLY / FS_O_WRONLY / FS_O_RDWR */
    uint32_t append;
    uint64_t position;
    void *handle;        /* port-owned */
    char host_path[FS_PATH_MAX + 224u];
} fs_file;

/* Counters the integrator reports (INTEGRATION.md). Instance-owned; never global. */
typedef struct fs_counters {
    uint64_t calls_stat, calls_opendir, calls_readdir, calls_closedir, calls_rmdir, calls_unlink;
    uint64_t refusals_guest_path;  /* ENOENT/EACCES/... decided without touching the port */
    uint64_t refusals_capacity;    /* EMFILE */
    uint64_t refusals_stale_handle;/* EBADF on an unknown or closed descriptor */
    uint64_t deletes_dir_ok;       /* rmdir that the port really performed */
    uint64_t deletes_file_ok;      /* unlink that the port really performed */
    uint64_t calls_open, calls_read, calls_write, calls_lseek, calls_fstat, calls_close;
    uint64_t bytes_read, bytes_written;
    uint64_t case_fallbacks; /* lookups that succeeded through the /dev_bdvd search */
    uint64_t case_ambiguous; /* /dev_bdvd directories with more than one case-insensitive match */
    /* Appended here so every counter above keeps its meaning and its offset. */
    uint64_t calls_mkdir, calls_rename, calls_fsync, calls_utime;
    uint64_t creates_dir_ok; /* mkdir that the port really performed */
    uint64_t renames_ok;     /* rename that the port really performed */
} fs_counters;

typedef struct fs_service {
    fs_storage_port port;
    fs_mount mounts[FS_MOUNT_CAPACITY];
    uint32_t mount_count;
    fs_descriptor descriptors[FS_DESCRIPTOR_CAPACITY];
    fs_file files[FS_FILE_CAPACITY];
    fs_counters counters;
    fs_storage_status last_storage_status;
    int32_t last_guest_result;
} fs_service;

/* ------------------------------------------------------------------------------------ */
/* Lifecycle and mounts                                                                  */
/* ------------------------------------------------------------------------------------ */

/* Zero-fills `service` and installs `port` (a NULL `port` installs an all-NULL one, which
 * makes every operation FS_ERROR_ENOTSUP). No mount exists afterwards. */
void fs_service_init(fs_service *service, const fs_storage_port *port);

/* The mount leaf for guest prefix `guest` (decision 24: `<leaf>` under the root token) and
 * the canonical guest prefix list, i.e. {"/dev_hdd0", "/dev_hdd1", "/dev_bdvd",
 * "/app_home", "/host_root"}. Returns NULL for an index >= FS_MOUNT_CAPACITY, and an empty
 * string when `out`/`cap` are unusable. */
const char *fs_canonical_guest_mount(uint32_t index);
/* The host leaf that stands for a canonical guest mount ("hdd0", "hdd1", "bdvd",
 * "app_home", "host_root"). Returned for a canonical index, NULL otherwise. */
const char *fs_canonical_host_leaf(uint32_t index);

/* Adds or replaces the mount for `guest` (must be canonical, i.e. one of
 * `fs_canonical_guest_mount(i)`). `root_token` is the configurable host root the whole tree
 * lives under: production `sdmc:/switch/infamous-native-spu-stage31`, and the test's own
 * scratch directory otherwise. The mount's host token becomes
 * `<root_token>/<fs_canonical_host_leaf(guest)>`.
 *
 * The root token itself is never given to the port: the service does not need to know where
 * the roots are, only how to build a confined path under them, and the adapter refuses
 * anything that is not under the root it was configured with.
 *
 * Returns 0 on success, -1 on a bad argument (not canonical, empty, too long, no capacity).
 * A repeated guest prefix replaces the existing entry, so a caller cannot exhaust the table
 * by re-registering. */
int fs_mount_add(fs_service *service, const char *guest, const char *root_token);

/* Removes every mount. */
void fs_mount_clear(fs_service *service);

/* ------------------------------------------------------------------------------------ */
/* Resolution and confinement (B2-S1)                                                    */
/* ------------------------------------------------------------------------------------ */

/* What `fs_resolve_path` produced. */
typedef struct fs_resolved_path {
    uint32_t mount_index;      /* which mount matched */
    const char *host;          /* `host_path` below */
    char host_path[FS_PATH_MAX + 224u]; /* mount host token + confined components */
    char relative[FS_PATH_MAX];         /* confined components below the mount, no leading '/' */
    uint32_t component_count;
    /* 1 when the guest path ended in '/', '.', or a ".." that was clamped at the mount root.
     * The path may only name a directory (filesystem-reference.md: "sys_fs additionally
     * checks traversing a regular file (including trailing slash/dot/dotdot) and may return
     * ENOTDIR"). The service uses it to decide ENOTDIR from a stat. */
    uint32_t requires_directory;
} fs_resolved_path;

/* Resolves a guest path into a confined host path.
 *
 * Rules, all from filesystem-reference.md §"Virtual paths" unless marked:
 *   - `length` must be <= 1024; anything larger is FS_ERROR_ENAMETOOLONG;
 *   - the path must be absolute; otherwise FS_ERROR_ENOENT;
 *   - a path with no NUL inside `length` bytes is FS_ERROR_ENAMETOOLONG;
 *   - the mount prefix is matched on a COMPONENT boundary ("/dev_hdd0x" is not
 *     "/dev_hdd0"); no such mount -> FS_ERROR_ENOENT;
 *   - repeated '/' collapse, '.' components vanish, ".." pops one component and is clamped
 *     at the mount root, so nothing above the root is ever produced;
 *   - a single component longer than FS_DIR_ENTRY_NAME_MAX is FS_ERROR_ENAMETOOLONG.
 *
 * The host path is `<mount.host>/<components...>` (or `<mount.host>` for the mount root
 * itself), so a guest path can never name anything outside the mount, and a guest byte of
 * '\\' or ':' cannot change which device is used because the token is prepended, not
 * concatenated from guest bytes.
 *
 * Returns FS_OK and fills `out`, or an FS_ERROR_* code. `out` is unspecified on failure. */
int32_t fs_resolve_path(const fs_service *service, const char *guest_path, size_t length,
                        fs_resolved_path *out);

/* ------------------------------------------------------------------------------------ */
/* Encoders                                                                              */
/* ------------------------------------------------------------------------------------ */

/* The `CellFsStat` record: exactly 52 bytes, big-endian, at the offsets CONTRACT.md §1c and
 * filesystem-reference.md §ABI give:
 *   +0x00 mode s32, +0x04 uid s32, +0x08 gid s32,
 *   +0x0C atime s64, +0x14 mtime s64, +0x1C ctime s64,
 *   +0x24 size u64, +0x2C blksize u64.
 * `mode` carries the type bits from `in->is_directory` (IFDIR 0040000 / IFREG 0100000, octal)
 * ORed with `in->mode & 0777`. Zero-fills `out` first. */
void fs_encode_cellfs_stat(const fs_stat_info *in, uint8_t out[FS_CELLFS_STAT_BYTES]);

/* The compact `CellFsDirent`, isolated in one function so a capture can change the tag
 * without touching anything else (DESIGN.md §7 open question 1):
 *   out[0] = d_type : FS_CELLFS_DTYPE_DIR (2) or FS_CELLFS_DTYPE_FILE (1)
 *   out[1] = d_namlen : the name length, truncated to FS_DIRENT_NAME_MAX
 *   out[2..] = the name, truncated to FS_DIRENT_NAME_MAX bytes, then a NUL
 *
 * CALLS.md §3a reads byte 0 and compares it to 2 and then 1; the walker does not read
 * d_namlen, and it copies its own NUL-terminated name from out+2, so both must be right.
 * Writes all FS_CELLFS_DIRENT_BYTES bytes, zero-filling the tail. */
#define FS_CELLFS_DTYPE_FILE 1u
#define FS_CELLFS_DTYPE_DIR 2u
void fs_encode_cellfs_dirent(const fs_dir_entry *in, uint8_t out[FS_CELLFS_DIRENT_BYTES]);

/* The `nread` value `cellFsReaddir` publishes: 1 for an entry, 0 at end of directory
 * (CALLS.md §3a, `read64(sp+0x78)` == 0 is the only stop signal). Big-endian, 8 bytes. */
#define FS_CELLFS_NREAD_BYTES 8u
void fs_encode_cellfs_nread(uint32_t has_entry, uint8_t out[FS_CELLFS_NREAD_BYTES]);

/* ------------------------------------------------------------------------------------ */
/* Operations (B2-S2 / B2-S3 / B2-S4 under decision 24)                                   */
/* ------------------------------------------------------------------------------------ */

/* `cellFsStat` (808).
 *
 *   1. a NULL `stat_out` is FS_ERROR_EFAULT (CONTRACT.md §1c: the wrapper's own guard);
 *   2. the path is resolved; any resolution failure is returned unchanged;
 *   3. the port stats the confined path;
 *   4. NOT_FOUND -> FS_ERROR_ENOENT (stat does not follow a final symlink on the guest;
 *      the adapter refuses symlinks at open, see INTEGRATION.md);
 *   5. the record is encoded into `stat_bytes` when non-NULL.
 *
 * A resolved path that `requires_directory` and stats as a regular file is
 * FS_ERROR_ENOTDIR, per filesystem-reference.md's traversal check.
 *
 * Returns the guest code; `stat_bytes` is untouched on failure. */
int32_t fs_stat(fs_service *service, const char *guest_path, size_t length, uint8_t *stat_bytes);

/* `cellFsOpendir` (805).
 *
 *   1. a NULL `out_handle` is FS_ERROR_EFAULT (CONTRACT.md §1a, measured: r4 == 0 returns
 *      0x8001000D and no syscall is made);
 *   2. the path is resolved;
 *   3. a free descriptor slot is taken, else FS_ERROR_EMFILE (the registry never grows);
 *   4. the port opens the confined path;
 *   5. the guest handle is written: a non-zero int32 that is unique until the counter
 *      wraps, and whose low byte is the slot index. The guest stores it with
 *      `read32`/`(int32_t)` (CALLS.md §3a), so 0 is never a successful handle (the walker
 *      treats 0 as "opendir failed" only after checking r3, so this is belt and braces).
 *
 * `*out_handle` is untouched on failure. */
int32_t fs_opendir(fs_service *service, const char *guest_path, size_t length,
                   uint32_t *out_handle);

/* `cellFsReaddir` (806).
 *
 *   1. a NULL `dirent_out` is FS_ERROR_EFAULT;
 *   2. an unknown or already-closed `handle` is FS_ERROR_EBADF;
 *   3. the port publishes entry *n* where n is this handle's own cursor, then the cursor
 *      advances. End of directory -> nread = 0, `dirent_out` untouched, FS_OK;
 *   4. otherwise the entry is encoded and nread = 1.
 *
 * A NULL `nread_out` is FS_ERROR_EFAULT as well: the firmware's wrapper only substitutes a
 * scratch for a NULL nread in `cellFsRead`, not here, and a native
 * service must not silently drop the only end-of-directory signal the caller has. */
int32_t fs_readdir(fs_service *service, uint32_t handle, uint8_t *dirent_out, uint8_t *nread_out);

/* `cellFsClosedir` (807). An unknown or already-closed `handle` is FS_ERROR_EBADF and the
 * registry is left untouched (so double close is detectable). A handle that was closed and
 * whose slot was reused is EBADF too, because the generation no longer matches. */
int32_t fs_closedir(fs_service *service, uint32_t handle);

/* `cellFsRmdir` (813) — decision 24: a real delete, confined to the mount root.
 *
 *   1. resolve; a resolution failure is returned unchanged;
 *   2. the mount root itself (`/dev_hdd0`, `/dev_hdd0/`, `/dev_hdd0/..`) is
 *      FS_ERROR_EACCES: a mount is not removable;
 *   3. the port removes the empty directory; a non-empty directory is FS_ERROR_ENOTSUP and
 *      a file is FS_ERROR_ENOTDIR.
 *
 * Never returns FS_ERROR_EROFS: the confined root is writable by construction. */
int32_t fs_rmdir(fs_service *service, const char *guest_path, size_t length);

/* `cellFsUnlink` (814) — decision 24: a real delete, confined to the mount root.
 *
 *   1. resolve; a resolution failure is returned unchanged;
 *   2. the mount root itself is FS_ERROR_EACCES;
 *   3. the port removes the regular file; a directory is FS_ERROR_EISDIR.
 *
 * Never returns FS_ERROR_EROFS. */
int32_t fs_unlink(fs_service *service, const char *guest_path, size_t length);

int32_t fs_mkdir(fs_service *service, const char *guest_path, size_t length, uint32_t mode);

/* `cellFsMkdir` (811) — the game makes its save and cache directories under `/dev_hdd0`.
 *
 *   1. resolve; a resolution failure is returned unchanged;
 *   2. the mount root itself is FS_ERROR_EEXIST (the directories are there already);
 *   3. a path under `/dev_bdvd` is FS_ERROR_EROFS and the port is not called: the disc is
 *      the game's own data, and the guest must not be able to write into it;
 *   4. the port creates one directory; ALREADY_EXISTS -> FS_ERROR_EEXIST, NOT_FOUND ->
 *      FS_ERROR_ENOENT, NOT_A_DIRECTORY -> FS_ERROR_ENOTDIR.
 *
 * Never recursive: the lv2 entry creates exactly one directory, and the game expects ENOENT
 * for a missing parent. `mode` is accepted and not forwarded — the confined root has its own
 * permissions and the adapter's `mkdir` decides them.
 *
 * "Under `/dev_bdvd`" is decided on the mount's guest name (`/dev_bdvd`), the same place
 * `fs_mount_add` decides which mount is case-insensitive, so the two cannot drift apart. */
int32_t fs_mkdir(fs_service *service, const char *guest_path, size_t length, uint32_t mode);

/* `cellFsRename` (812).
 *
 *   1. both paths are resolved; either resolution failure is returned unchanged;
 *   2. either path being a mount root is FS_ERROR_EACCES;
 *   3. either path under `/dev_bdvd` is FS_ERROR_EROFS and the port is not called;
 *   4. two different mounts is FS_ERROR_EINVAL — lv2's own code for a cross-device rename is
 *      EXDEV, whose value is not confirmed in the captured contract, so the service reports
 *      the one code it can prove an error rather than a guessed bit pattern;
 *   5. the port renames; NOT_FOUND -> FS_ERROR_ENOENT, ALREADY_EXISTS -> FS_ERROR_EEXIST.
 *
 * An fd opened before the rename keeps working: the registry stores the port handle and the
 * bytes the fd reads, not the name, so `fs_file.host_path` is left alone (only `fs_fstat`
 * ever re-stats it, and on a host that did the rename that path is simply gone). */
int32_t fs_rename(fs_service *service, const char *guest_from, size_t from_length,
                  const char *guest_to, size_t to_length);

/* `cellFsFsync` (820).
 *
 *   1. an unknown or already-closed `fd` is FS_ERROR_EBADF;
 *   2. a directory handle is FS_ERROR_EBADF as well: the guest passes an fd, and the
 *      directory registry is a separate namespace;
 *   3. a read-only file is FS_OK without calling the port (there is nothing to flush);
 *   4. a writable file's port flush; a host file system without one is FS_OK, because the
 *      guest's contract is "the bytes are on the medium", and a host that has no flush has
 *      already delivered them to the only layer it has.
 *
 * A write that the port reported as done is written by the time this returns; lv2 makes no
 * promise beyond that, and neither does this service. */
int32_t fs_fsync(fs_service *service, uint32_t fd);

/* `cellFsUtime` (815). The times are accepted and not stored: no host path under the
 * confined root carries a timestamp the guest can observe, and the game uses this to stamp
 * files it just created.
 *
 *   1. resolve; a resolution failure is returned unchanged;
 *   2. a path under `/dev_bdvd` is FS_ERROR_EROFS and the port is not called;
 *   3. the port's `stat` must find the path, else FS_ERROR_ENOENT;
 *   4. otherwise FS_OK. No port write happens, so a read-only mount still reports success for
 *      a path it owns. */
int32_t fs_utime(fs_service *service, const char *guest_path, size_t length,
                 int64_t access_time, int64_t modification_time);

/* Number of live directory handles. Used by the double-close and EMFILE tests; also what an
 * integrator reports as a leak check. */
uint32_t fs_open_descriptor_count(const fs_service *service);

/* Number of live regular-file handles. */
uint32_t fs_open_file_count(const fs_service *service);

/* Closes every live handle through the port. Returns the number closed (directories plus
 * regular files). The service keeps working afterwards. */
uint32_t fs_close_all(fs_service *service);

/* ------------------------------------------------------------------------------------ */
/* Regular-file operations (lv2 syscalls 801-804 and 818)                                */
/* ------------------------------------------------------------------------------------ */

/* Opens `guest_path` and writes guest fd 3..63 to `*out_fd`. A NULL out is EFAULT; an
 * unsupported flag or access == 3 is EINVAL; the mount root or a path requiring a directory
 * is EISDIR; no free slot is EMFILE before the port is called. */
int32_t fs_open(fs_service *service, const char *guest_path, size_t length, uint32_t flags,
                uint32_t *out_fd);
int32_t fs_read(fs_service *service, uint32_t fd, uint8_t *buffer, uint32_t length,
                uint64_t *out_read);
int32_t fs_write(fs_service *service, uint32_t fd, const uint8_t *buffer, uint32_t length,
                 uint64_t *out_written);
int32_t fs_lseek(fs_service *service, uint32_t fd, int64_t offset, uint32_t whence,
                 uint64_t *out_position);
int32_t fs_fstat(fs_service *service, uint32_t fd, uint8_t *stat_bytes);
int32_t fs_close(fs_service *service, uint32_t fd);

#ifdef __cplusplus
}
#endif

#endif /* FS_SERVICE_H */
