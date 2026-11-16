#ifndef NATIVE_FS_STORAGE_POSIX_H
#define NATIVE_FS_STORAGE_POSIX_H
/* POSIX adapter for the platform-free file-system service (native/domain/fs_service.h).
 *
 * One host directory stands for the root the domain's mount tokens are built under
 * (decision 24: `sdmc:/switch/infamous-native-spu-stage31` in production). Every path this
 * adapter touches is at or below `<root>`, and it refuses anything else:
 *
 *   - a path that does not begin with `<root>` followed by '/' or NUL is refused;
 *   - any component that is empty, ".", ".." or contains '/' is refused;
 *   - a trailing '/' is refused (the domain never produces one);
 *   - a symlink is refused at every component (`O_NOFOLLOW`) and at `remove`, so the tree the
 *     service describes is the tree the guest sees.
 *
 * Removal is real (decision 24) and confined: `remove_dir` and `unlink_file` only ever act on
 * a path that passed the checks above. `make_dir` and `rename_path` pass the same checks,
 * for both ends of a rename: neither can create or move anything outside the root.
 *
 * Portability: plain C11 and the POSIX file API only (opendir/readdir/closedir/rewinddir,
 * lstat, open with O_NOFOLLOW, unlink, rmdir). There is no libnx include and no `__SWITCH__`
 * branch: on the Switch, libnx's `sdmc:` fsdev already provides this API, so the same object
 * works on the host tests and on the console once `root` names "sdmc:/...".
 *
 * Ownership: the caller allocates the struct. `init` stores `root` and creates nothing (a
 * missing root is simply an empty tree: every lookup is NOT_FOUND). Directory handles are
 * heap-allocated by `open_dir` and freed by `close_dir`; `fs_close_all` on the domain frees
 * every handle that is still open.
 */
#include "../domain/fs_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds. They exist so one call can never walk an unbounded tree. */
#define FS_STORAGE_POSIX_ROOT_MAX 256u /* including the NUL terminator */

typedef struct fs_storage_posix {
    char root[FS_STORAGE_POSIX_ROOT_MAX]; /* host directory, no trailing '/' (except "/") */
    unsigned calls_stat, calls_open_dir, calls_read_dir, calls_close_dir, calls_remove_dir,
        calls_unlink_file, calls_open_file, calls_read_file, calls_write_file, calls_file_size,
        calls_close_file, calls_make_dir, calls_rename_path, calls_sync_file, refused;
    unsigned rename_replacements; /* renames that needed the non-replacing host's unlink-and-retry */
    int last_errno; /* errno of the last failing host call, 0 if none */
    fs_storage_status last_status;
    unsigned parents_created;   /* Decision 81: mkdir calls that had to create a missing parent first (appended) */
} fs_storage_posix;

/* Copies `root` (non-empty, shorter than FS_STORAGE_POSIX_ROOT_MAX), drops one trailing '/'
 * and verifies that what exists at `root` is a directory when it exists at all. Nothing is
 * created: the tree is expected to be provisioned by the launcher or the test.
 *
 * Returns 0 on success, -1 otherwise (errno kept in last_errno). A root that does not exist
 * yet is not an error; the adapter then behaves as an empty tree. */
int fs_storage_posix_init(fs_storage_posix *self, const char *root);

/* Fills the port functions with context = self. */
fs_storage_port fs_storage_posix_port(fs_storage_posix *self);

/* Is `prefix` a libnx device specifier rather than a directory? A trailing ':' marks one, so
 * "sdmc:" is a device name (fsdev) and must never be passed to mkdir or stat. Copied from
 * `gamedata_storage_posix_is_device_prefix` (that file is not edited). */
int fs_storage_posix_is_device_prefix(const char *prefix);

/* Would this relative path, appended to the adapter's root, stay inside it? 1 = yes,
 * 0 = no. Exposed so the host test can drive the confinement decision directly. Rules:
 * empty, a leading '/', an empty component, "." , "..", a component containing '/', a
 * trailing '/', and any component longer than 255 bytes are all refused. */
int fs_storage_posix_relative_ok(const char *relative);

/* Test seam: installs the host rename the adapter calls, or restores the real `rename` for a
 * NULL. A FAT-style card (the Switch's `sdmc:` fsdev) refuses to replace an existing file
 * with EEXIST, which no Linux host does, so the unlink-and-retry path can only be reached on
 * the host this way. Returns 0 always; the adapter is never told which host it runs on. */
int fs_storage_posix_set_host_rename(int (*rename_fn)(const char *, const char *));

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_FS_STORAGE_POSIX_H */
