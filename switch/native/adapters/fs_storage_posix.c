/* POSIX adapter for the platform-free file-system service. See fs_storage_posix.h.
 *
 * Every helper is total and returns an fs_storage_status; a host errno is only used to pick
 * the status and is remembered in self->last_errno for the report.
 *
 * No `__SWITCH__` branch and no libnx include: libnx's `sdmc:` fsdev answers the same POSIX
 * calls, so `root` may be "/tmp/..." on the host and "sdmc:/switch/..." on the console.
 * Two host facts are used as-is and both are fsdev-compatible: `O_NOFOLLOW` (which devkitA64
 * newlib defines; the value is ignored by fsdev) and `lstat` (which newlib provides).
 */
/* dirfd helpers and d_type's DT_* names are not in the strict POSIX 2008 header set */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "fs_storage_posix.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A host path the adapter will build: <root> + '/' + <confined components>. */
#define FS_STORAGE_PATH_MAX (FS_STORAGE_POSIX_ROOT_MAX + FS_PATH_MAX + 2u)
/* Filename limit (filesystem-reference.md §"Virtual paths": "Filename limit255"). */
#define FS_STORAGE_COMPONENT_MAX 255u
/* mkdir mode for every directory the service creates: the confined root is the only reader
 * and the guest has no permission model of its own here. */
#define FS_STORAGE_DIR_MODE 0777

/* The host rename the adapter calls. It is a pointer so the host test can make one call
 * answer EEXIST, which is what a FAT-style card does and what no Linux host does. Production
 * leaves it at `rename`; only the test setter below ever writes it. */
static int (*g_host_rename)(const char *, const char *) = rename;

/* ------------------------------------------------------------------------------------ */
/* Device prefix (copied from gamedata_storage_posix.c; that file is not edited)         */
/* ------------------------------------------------------------------------------------ */

int fs_storage_posix_is_device_prefix(const char *prefix)
{
    if (!prefix || !prefix[0]) return 0;
    return prefix[strlen(prefix) - 1u] == ':';
}

/* ------------------------------------------------------------------------------------ */
/* Confinement                                                                          */
/* ------------------------------------------------------------------------------------ */

int fs_storage_posix_relative_ok(const char *relative)
{
    const char *cursor;

    if (!relative) return 0;
    if (!relative[0]) return 0;                           /* empty */
    if (relative[0] == '/') return 0;                     /* absolute: would escape the root */
    if (relative[strlen(relative) - 1u] == '/') return 0; /* the domain never produces one */

    cursor = relative;
    for (;;) {
        const char *end = strchr(cursor, '/');
        const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        char component[FS_STORAGE_COMPONENT_MAX + 1u];

        if (length == 0u) return 0;                                            /* "a//b" */
        if (length > FS_STORAGE_COMPONENT_MAX) return 0;                       /* > 255 bytes */
        if (memchr(cursor, '\\', length) != NULL) return 0;                    /* separator on some hosts */
        if (length == 1u && cursor[0] == '.') return 0;
        if (length == 2u && cursor[0] == '.' && cursor[1] == '.') return 0;
        if (!end) return 1; /* the only component, and it is a plain name */

        /* A component that ends in ':' would spell an fsdev device ("sdmc:"). ':' is an
         * ordinary byte in a POSIX filename, but the adapter refuses it rather than risk a
         * path that a later component could turn into a different device. */
        memcpy(component, cursor, length);
        component[length] = 0;
        if (fs_storage_posix_is_device_prefix(component)) return 0;

        cursor = end + 1;
    }
}

static fs_storage_status fail(fs_storage_posix *self, int host_errno)
{
    self->last_errno = host_errno;
    switch (host_errno) {
    case ENOENT:
        self->last_status = FS_STORAGE_NOT_FOUND;
        break;
    case ENOTDIR:
        self->last_status = FS_STORAGE_NOT_A_DIRECTORY;
        break;
    case EISDIR:
        self->last_status = FS_STORAGE_IS_A_DIRECTORY;
        break;
    case EACCES:
    case EPERM:
        self->last_status = FS_STORAGE_ACCESS_DENIED;
        break;
    case ENOTEMPTY:
    case EEXIST:
    case ELOOP:
    case ENOSYS:
        self->last_status = FS_STORAGE_NOT_SUPPORTED;
        break;
    case ENAMETOOLONG:
        self->last_status = FS_STORAGE_NAME_TOO_LONG;
        break;
    default:
        self->last_status = FS_STORAGE_IO_ERROR;
        break;
    }
    return self->last_status;
}

static fs_storage_status succeed(fs_storage_posix *self)
{
    self->last_errno = 0;
    self->last_status = FS_STORAGE_OK;
    return self->last_status;
}

/* Does `path` begin with the root, followed by '/' or NUL? */
static int under_root(const fs_storage_posix *self, const char *path)
{
    const size_t root_length = strlen(self->root);
    if (!root_length) return 0;
    if (strncmp(path, self->root, root_length) != 0) return 0;
    if (self->root[root_length - 1u] == '/') return 1; /* root is "/" */
    return path[root_length] == '/' || path[root_length] == 0;
}

/* Validates a fully resolved domain path and copies it into `out`.
 * 1 = accepted, 0 = refused (and counted). The refusals here are the adapter's own, not the
 * domain's: an absolute host path the guest somehow named, a '..' anywhere, an empty name,
 * a '/' inside a single component, or a path outside the configured root. */
static int accept_host_path(fs_storage_posix *self, const char *host_path, char *out, size_t cap)
{
    size_t length;
    size_t root_length;
    const char *relative;

    if (!host_path || !host_path[0]) {
        ++self->refused;
        return 0;
    }
    if (!under_root(self, host_path)) {
        ++self->refused;
        return 0;
    }
    length = strlen(host_path);
    if (length >= cap) {
        ++self->refused;
        return 0;
    }
    root_length = strlen(self->root);
    relative = host_path + root_length;
    if (*relative == '/') {
        if (!fs_storage_posix_relative_ok(relative + 1)) {
            ++self->refused;
            return 0;
        }
    } else if (*relative != 0) {
        ++self->refused;
        return 0;
    }
    memcpy(out, host_path, length + 1u);
    return 1;
}

/* `<self->root>/<name>` for the entry probe in read_dir. 0 on success. */
static int join_entry(const fs_storage_posix *self, const char *name, char *out, size_t cap)
{
    const int written = snprintf(out, cap, "%s/%s", self->root, name);
    return (written > 0 && (size_t)written < cap) ? 0 : -1;
}

/* ------------------------------------------------------------------------------------ */
/* Port: stat                                                                          */
/* ------------------------------------------------------------------------------------ */

static void fill_stat(const struct stat *info, fs_stat_info *out)
{
    memset(out, 0, sizeof *out);
    out->is_directory = S_ISDIR(info->st_mode) ? 1u : 0u;
    out->mode = (uint32_t)(info->st_mode & 0777);
    out->uid = (int32_t)info->st_uid;
    out->gid = (int32_t)info->st_gid;
    out->atime = (int64_t)info->st_atime;
    out->mtime = (int64_t)info->st_mtime;
    out->ctime = (int64_t)info->st_ctime;
    out->size = (uint64_t)info->st_size;
    out->blksize = (uint64_t)info->st_blksize;
}

static fs_storage_status port_stat(void *context, const char *host_path, fs_stat_info *out)
{
    fs_storage_posix *self = context;
    struct stat info;
    char path[FS_STORAGE_PATH_MAX];

    if (!self || !out) return FS_STORAGE_IO_ERROR;
    ++self->calls_stat;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    /* lstat, not stat: a symlink must not be followed out of the root */
    if (lstat(path, &info) != 0) return fail(self, errno);
    if (S_ISLNK(info.st_mode)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    fill_stat(&info, out);
    return succeed(self);
}

/* ------------------------------------------------------------------------------------ */
/* Port: directory handles                                                              */
/* ------------------------------------------------------------------------------------ */

typedef struct fs_storage_dir {
    DIR *stream;
    char path[FS_STORAGE_PATH_MAX];
} fs_storage_dir;

static fs_storage_status port_open_dir(void *context, const char *host_path, void **out_handle)
{
    fs_storage_posix *self = context;
    fs_storage_dir *handle;
    struct stat info;
    char path[FS_STORAGE_PATH_MAX];

    if (!self || !out_handle) return FS_STORAGE_IO_ERROR;
    ++self->calls_open_dir;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (lstat(path, &info) != 0) return fail(self, errno);
    if (!S_ISDIR(info.st_mode)) {
        self->last_errno = ENOTDIR;
        return fail(self, ENOTDIR);
    }
    handle = (fs_storage_dir *)calloc(1u, sizeof *handle);
    if (!handle) {
        self->last_errno = ENOMEM;
        return fail(self, ENOMEM);
    }
    handle->stream = opendir(path);
    if (!handle->stream) {
        const int saved = errno;
        free(handle);
        return fail(self, saved);
    }
    memcpy(handle->path, path, strlen(path) + 1u);
    *out_handle = handle;
    return succeed(self);
}

/* The stream is sequential, so entry `index` is published by skipping to it. A DIR stream
 * also hides the previous cursor, so `index` is authoritative and the domain keeps it: a
 * caller that asks for index 0 again rewinds. Skip order is the host's enumeration order,
 * which is the order the guest walker uses (it just loops until nread == 0). */
static fs_storage_status port_read_dir(void *context, void *handle, uint32_t index, fs_dir_entry *out,
                                       uint32_t *out_has_entry)
{
    fs_storage_posix *self = context;
    fs_storage_dir *state = handle;
    uint32_t published = 0;

    if (!self || !state || !state->stream || !out || !out_has_entry) return FS_STORAGE_IO_ERROR;
    ++self->calls_read_dir;
    *out_has_entry = 0;
    rewinddir(state->stream);
    for (;;) {
        const struct dirent *entry = readdir(state->stream);
        size_t name_length;
        if (!entry) return succeed(self);
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        name_length = strlen(entry->d_name);
        if (name_length == 0u) continue; /* never publish an empty name */
        if (published++ < index) continue;
        if (name_length >= sizeof out->name) {
            self->last_errno = ENAMETOOLONG;
            return fail(self, ENAMETOOLONG);
        }
        memset(out, 0, sizeof *out);
        memcpy(out->name, entry->d_name, name_length + 1u);
        out->name_length = (uint32_t)name_length;
        /* DT_UNKNOWN is common on fsdev and on some Linux filesystems, so the type is read
         * from the entry itself rather than trusted from d_type. */
        if (entry->d_type == DT_DIR) {
            out->is_directory = 1u;
        } else if (entry->d_type == DT_REG) {
            out->is_directory = 0u;
        } else {
            struct stat info;
            char path[FS_STORAGE_PATH_MAX];
            if (join_entry(self, entry->d_name, path, sizeof path) != 0) {
                self->last_errno = ENAMETOOLONG;
                return fail(self, ENAMETOOLONG);
            }
            if (lstat(path, &info) != 0) return fail(self, errno);
            /* A symlink is published as a non-directory that the guest cannot recurse into.
             * The rest of the port refuses symlinks (see accept_host_path's callers), so the
             * walker's unlink of it is refused too: it is listed, never followed, never
             * deleted. Failing the whole enumeration instead would hide every sibling. */
            out->is_directory = S_ISDIR(info.st_mode) ? 1u : 0u;
        }
        *out_has_entry = 1u;
        return succeed(self);
    }
}

static fs_storage_status port_close_dir(void *context, void *handle)
{
    fs_storage_posix *self = context;
    fs_storage_dir *state = handle;

    if (!self || !state) return FS_STORAGE_IO_ERROR;
    ++self->calls_close_dir;
    if (state->stream) {
        if (closedir(state->stream) != 0) {
            const int saved = errno;
            free(state);
            return fail(self, saved);
        }
    }
    free(state);
    return succeed(self);
}

/* ------------------------------------------------------------------------------------ */
/* Port: removal (decision 24: real, confined)                                          */
/* ------------------------------------------------------------------------------------ */

static fs_storage_status port_remove_dir(void *context, const char *host_path)
{
    fs_storage_posix *self = context;
    struct stat info;
    char path[FS_STORAGE_PATH_MAX];
    const size_t root_length = strlen(self->root);

    if (!self) return FS_STORAGE_IO_ERROR;
    ++self->calls_remove_dir;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    /* the root itself is never removable, even if the domain's own check were bypassed */
    if (strlen(path) <= root_length) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (lstat(path, &info) != 0) return fail(self, errno);
    if (S_ISLNK(info.st_mode)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (!S_ISDIR(info.st_mode)) {
        self->last_errno = ENOTDIR;
        return fail(self, ENOTDIR);
    }
    /* rmdir, not recursive removal: emptying a tree is the guest walker's job (CALLS.md §3a)
     * and a native recursive delete would be a second, unaudited delete path */
    if (rmdir(path) != 0) return fail(self, errno);
    return succeed(self);
}

static fs_storage_status port_unlink_file(void *context, const char *host_path)
{
    fs_storage_posix *self = context;
    struct stat info;
    char path[FS_STORAGE_PATH_MAX];
    const size_t root_length = strlen(self->root);

    if (!self) return FS_STORAGE_IO_ERROR;
    ++self->calls_unlink_file;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (strlen(path) <= root_length) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (lstat(path, &info) != 0) return fail(self, errno);
    if (S_ISLNK(info.st_mode)) {
        /* removing the link itself would be safe, but the service does not model symlinks at
         * all, so a link is a refusal rather than a silently different object */
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    if (S_ISDIR(info.st_mode)) {
        self->last_errno = EISDIR;
        return fail(self, EISDIR);
    }
    if (unlink(path) != 0) return fail(self, errno);
    return succeed(self);
}

/* ------------------------------------------------------------------------------------ */
/* Port: regular-file handles                                                           */
/* ------------------------------------------------------------------------------------ */

typedef struct fs_storage_file {
    int fd;
} fs_storage_file;

static fs_storage_status fail_open(fs_storage_posix *self, int host_errno)
{
    self->last_errno = host_errno;
    switch (host_errno) {
    case ENOENT:
        self->last_status = FS_STORAGE_NOT_FOUND;
        break;
    case EEXIST:
        self->last_status = FS_STORAGE_ALREADY_EXISTS;
        break;
    case EISDIR:
        self->last_status = FS_STORAGE_IS_A_DIRECTORY;
        break;
    case ELOOP:
    case EACCES:
    case EPERM:
        self->last_status = FS_STORAGE_ACCESS_DENIED;
        break;
    case ENOTDIR:
        self->last_status = FS_STORAGE_NOT_A_DIRECTORY;
        break;
    case ENAMETOOLONG:
        self->last_status = FS_STORAGE_NAME_TOO_LONG;
        break;
    default:
        self->last_status = FS_STORAGE_IO_ERROR;
        break;
    }
    return self->last_status;
}

static fs_storage_status port_open_file(void *context, const char *host_path, uint32_t access,
                                        uint32_t create, uint32_t exclusive, uint32_t truncate,
                                        void **out_handle)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle;
    struct stat info;
    char path[FS_STORAGE_PATH_MAX];
    int flags;

    if (!self || !out_handle) return FS_STORAGE_IO_ERROR;
    ++self->calls_open_file;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail_open(self, EACCES);
    }
    switch (access) {
    case FS_O_RDONLY: flags = O_RDONLY; break;
    case FS_O_WRONLY: flags = O_WRONLY; break;
    case FS_O_RDWR: flags = O_RDWR; break;
    default:
        self->last_errno = EINVAL;
        return fail_open(self, EINVAL);
    }
    flags |= O_NOFOLLOW;
    if (create) flags |= O_CREAT;
    if (exclusive) flags |= O_EXCL;
    if (truncate) flags |= O_TRUNC;

    handle = (fs_storage_file *)calloc(1u, sizeof *handle);
    if (!handle) {
        self->last_errno = ENOMEM;
        return fail_open(self, ENOMEM);
    }
    handle->fd = open(path, flags, 0666);
    if (handle->fd < 0) {
        const int saved = errno;
        free(handle);
        return fail_open(self, saved);
    }
    if (fstat(handle->fd, &info) != 0) {
        const int saved = errno;
        (void)close(handle->fd);
        free(handle);
        return fail_open(self, saved);
    }
    if (S_ISDIR(info.st_mode)) {
        (void)close(handle->fd);
        free(handle);
        self->last_errno = EISDIR;
        return fail_open(self, EISDIR);
    }
    *out_handle = handle;
    return succeed(self);
}

static fs_storage_status port_read_file(void *context, void *opaque_handle, uint64_t offset,
                                        uint8_t *buffer, uint32_t length, uint32_t *out_read)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle = opaque_handle;
    uint32_t total = 0;

    if (!self || !handle || !out_read) return FS_STORAGE_IO_ERROR;
    ++self->calls_read_file;
    *out_read = 0u;
    if (!buffer && length != 0u) return FS_STORAGE_IO_ERROR;
    if (lseek(handle->fd, (off_t)offset, SEEK_SET) < 0) return fail(self, errno);
    while (total < length) {
        const ssize_t got = read(handle->fd, buffer + total, length - total);
        if (got < 0) {
            if (errno == EINTR) continue;
            *out_read = total;
            return fail(self, errno);
        }
        if (got == 0) break;
        total += (uint32_t)got;
    }
    *out_read = total;
    return succeed(self);
}

static fs_storage_status port_write_file(void *context, void *opaque_handle, uint64_t offset,
                                         const uint8_t *buffer, uint32_t length,
                                         uint32_t *out_written)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle = opaque_handle;
    uint32_t total = 0;

    if (!self || !handle || !out_written) return FS_STORAGE_IO_ERROR;
    ++self->calls_write_file;
    *out_written = 0u;
    if (!buffer && length != 0u) return FS_STORAGE_IO_ERROR;
    if (lseek(handle->fd, (off_t)offset, SEEK_SET) < 0) return fail(self, errno);
    while (total < length) {
        const ssize_t put = write(handle->fd, buffer + total, length - total);
        if (put < 0) {
            if (errno == EINTR) continue;
            *out_written = total;
            return fail(self, errno);
        }
        if (put == 0) break;
        total += (uint32_t)put;
    }
    *out_written = total;
    return succeed(self);
}

static fs_storage_status port_file_size(void *context, void *opaque_handle, uint64_t *out_size)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle = opaque_handle;
    struct stat info;

    if (!self || !handle || !out_size) return FS_STORAGE_IO_ERROR;
    ++self->calls_file_size;
    if (fstat(handle->fd, &info) != 0) return fail(self, errno);
    *out_size = (uint64_t)info.st_size;
    return succeed(self);
}

static fs_storage_status port_close_file(void *context, void *opaque_handle)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle = opaque_handle;
    int saved = 0;

    if (!self || !handle) return FS_STORAGE_IO_ERROR;
    ++self->calls_close_file;
    if (close(handle->fd) != 0) saved = errno;
    free(handle);
    if (saved) return fail(self, saved);
    return succeed(self);
}

static fs_storage_status fail_rename(fs_storage_posix *self, int host_errno)
{
    self->last_errno = host_errno;
    switch (host_errno) {
    case ENOENT:
        self->last_status = FS_STORAGE_NOT_FOUND;
        break;
    case EEXIST:
    case ENOTEMPTY:
        self->last_status = FS_STORAGE_ALREADY_EXISTS;
        break;
    case ENOTDIR:
        self->last_status = FS_STORAGE_NOT_A_DIRECTORY;
        break;
    case EISDIR:
        self->last_status = FS_STORAGE_IS_A_DIRECTORY;
        break;
    case EACCES:
    case EPERM:
        self->last_status = FS_STORAGE_ACCESS_DENIED;
        break;
    case ENOSYS:
        self->last_status = FS_STORAGE_NOT_SUPPORTED;
        break;
    case ENAMETOOLONG:
        self->last_status = FS_STORAGE_NAME_TOO_LONG;
        break;
    default:
        self->last_status = FS_STORAGE_IO_ERROR;
        break;
    }
    return self->last_status;
}

static fs_storage_status port_make_dir(void *context, const char *host_path)
{
    fs_storage_posix *self = context;
    char path[FS_STORAGE_PATH_MAX];

    if (!self) return FS_STORAGE_IO_ERROR;
    ++self->calls_make_dir;
    if (!accept_host_path(self, host_path, path, sizeof path)) {
        self->last_errno = EACCES;
        return fail(self, EACCES);
    }
    /* one directory, never a tree: the guest's own walker creates the levels it needs */
#ifdef PS3_NATIVE_LLE_PROFILE
    /* Root (2026-11-12, Decision 81, console run rv8): EXCEPT the levels the PS3 itself provides. inFAMOUS makes
     * /dev_hdd0/game/<title>/USRDIR/cache and expects .../USRDIR to be there (on a console the system creates the
     * title's game-data directory); it never creates it through the game-data service. On the SD card that
     * parent did not exist, all 19 mkdir calls of the install answered ENOENT and the run ended. The emulator's
     * SD card creates missing parents by itself, which hid this. So a missing parent is created here, level by
     * level, below the confined root only, and the directory itself is then made as before. */
    if (mkdir(path, FS_STORAGE_DIR_MODE) == 0) return succeed(self);
    if (errno == ENOENT) {
        const size_t root_length = strlen(self->root);
        for (size_t at = root_length + 1u; path[at]; ++at) {
            if (path[at] != '/') continue;
            path[at] = '\0';
            if (mkdir(path, FS_STORAGE_DIR_MODE) != 0 && errno != EEXIST) { const int code = errno; path[at] = '/'; return fail(self, code); }
            path[at] = '/';
        }
        ++self->parents_created;
    }
#endif
    if (mkdir(path, FS_STORAGE_DIR_MODE) != 0) {
        /* EEXIST from mkdir names the exact thing the port contract calls ALREADY_EXISTS,
         * for a file and for a directory alike; the shared mapper would call it unsupported */
        if (errno == EEXIST) {
            self->last_errno = EEXIST;
            self->last_status = FS_STORAGE_ALREADY_EXISTS;
            return self->last_status;
        }
        return fail(self, errno);
    }
    return succeed(self);
}

/* Renames within the root. A source that is absent or a symlink is refused before the host
 * call; the destination is only inspected, never followed. */
static fs_storage_status port_rename_path(void *context, const char *host_from,
                                          const char *host_to)
{
    fs_storage_posix *self = context;
    struct stat from_info;
    struct stat to_info;
    char from[FS_STORAGE_PATH_MAX];
    char to[FS_STORAGE_PATH_MAX];
    int to_is_regular = 0;

    if (!self) return FS_STORAGE_IO_ERROR;
    ++self->calls_rename_path;
    if (!accept_host_path(self, host_from, from, sizeof from) ||
        !accept_host_path(self, host_to, to, sizeof to)) {
        self->last_errno = EACCES;
        return fail_rename(self, EACCES);
    }
    if (lstat(from, &from_info) != 0) return fail_rename(self, errno);
    if (S_ISLNK(from_info.st_mode)) {
        self->last_errno = EACCES;
        return fail_rename(self, EACCES);
    }
    if (lstat(to, &to_info) == 0) {
        if (S_ISLNK(to_info.st_mode)) {
            self->last_errno = EACCES;
            return fail_rename(self, EACCES);
        }
        if (S_ISDIR(to_info.st_mode)) {
            self->last_errno = EEXIST;
            self->last_status = FS_STORAGE_ALREADY_EXISTS;
            return self->last_status;
        }
        to_is_regular = S_ISREG(to_info.st_mode) ? 1 : 0;
    } else if (errno != ENOENT) {
        return fail_rename(self, errno);
    }
    if (g_host_rename(from, to) == 0) return succeed(self);
    if (errno != EEXIST || !to_is_regular) return fail_rename(self, errno);
    /* A FAT-style host (the Switch's own `sdmc:` fsdev) refuses to replace an existing file,
     * while the guest contract and every POSIX host replace it. The guest must see one
     * behaviour, so the destination regular file is removed and the rename is tried once
     * more: exactly one retry, only for a regular file, so a directory or a symlink is never
     * unlinked. A failure of the second call is reported as it is. */
    if (unlink(to) != 0) return fail_rename(self, errno);
    ++self->rename_replacements;
    if (g_host_rename(from, to) != 0) return fail_rename(self, errno);
    return succeed(self);
}

int fs_storage_posix_set_host_rename(int (*rename_fn)(const char *, const char *))
{
    g_host_rename = rename_fn ? rename_fn : rename;
    return 0;
}

static fs_storage_status port_sync_file(void *context, void *opaque_handle)
{
    fs_storage_posix *self = context;
    fs_storage_file *handle = opaque_handle;

    if (!self || !handle) return FS_STORAGE_IO_ERROR;
    ++self->calls_sync_file;
    if (fsync(handle->fd) == 0) return succeed(self);
    switch (errno) {
    case EINVAL:
    case ENOSYS:
    case ENOTSUP:
        /* the host file system has no flush to give: the bytes are already in the only
         * layer it has, which is what the guest asked for */
        return succeed(self);
    default:
        return fail(self, errno);
    }
}

/* ------------------------------------------------------------------------------------ */
/* Public                                                                               */
/* ------------------------------------------------------------------------------------ */

int fs_storage_posix_init(fs_storage_posix *self, const char *root)
{
    size_t length;
    struct stat info;

    if (!self) return -1;
    memset(self, 0, sizeof *self);
    self->last_status = FS_STORAGE_OK;
    if (!root || !root[0]) {
        self->last_errno = EINVAL;
        return -1;
    }
    length = strlen(root);
    if (length >= sizeof self->root) {
        self->last_errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(self->root, root, length + 1u);
    /* one trailing '/' away: "sdmc:/x/" and "sdmc:/x" are the same root */
    if (length > 1u && self->root[length - 1u] == '/') self->root[length - 1u] = 0;

    /* Nothing is created. A root that does not exist yet is an empty tree: the launcher or
     * the test provisions it, and a missing card is a NOT_FOUND per lookup rather than a
     * hard failure at init. */
    if (lstat(self->root, &info) != 0) {
        if (errno == ENOENT) {
            (void)succeed(self);
            return 0;
        }
        self->last_errno = errno;
        return -1;
    }
    if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode)) {
        self->last_errno = ENOTDIR;
        return -1;
    }
    self->last_errno = 0;
    return 0;
}

fs_storage_port fs_storage_posix_port(fs_storage_posix *self)
{
    fs_storage_port port;
    memset(&port, 0, sizeof port);
    port.context = self;
    port.stat = port_stat;
    port.open_dir = port_open_dir;
    port.read_dir = port_read_dir;
    port.close_dir = port_close_dir;
    port.remove_dir = port_remove_dir;
    port.unlink_file = port_unlink_file;
    port.open_file = port_open_file;
    port.read_file = port_read_file;
    port.write_file = port_write_file;
    port.file_size = port_file_size;
    port.close_file = port_close_file;
    port.make_dir = port_make_dir;
    port.rename_path = port_rename_path;
    port.sync_file = port_sync_file;
    return port;
}
