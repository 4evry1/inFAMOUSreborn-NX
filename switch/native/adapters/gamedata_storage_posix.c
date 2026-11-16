#define _POSIX_C_SOURCE 200809L
#include "gamedata_storage_posix.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Every helper below is total and returns a gamedata_storage_status; host errno is only used to pick
 * between NO_SPACE and IO_ERROR and is remembered in self->last_errno for the report. */

#define PATH_MAX_BYTES 1024u
#define PATH_FMT "%s/%s"

static gamedata_storage_status fail(gamedata_storage_posix *self, int host_errno)
{
    self->last_errno = host_errno;
    return host_errno == ENOSPC ? GAMEDATA_STORAGE_NO_SPACE : GAMEDATA_STORAGE_IO_ERROR;
}

/* A single path component the port may join below the root: non-empty, no '/', no "..". */
static int name_ok(const char *name)
{
    if (!name || !name[0]) return 0;
    if (!strcmp(name, "..") || !strcmp(name, ".")) return 0;
    for (const char *p = name; *p; ++p)
        if (*p == '/') return 0;
    return 1;
}

/* <root>/<dir_name> into `out`; refuses truncation. */
static int join_dir(const gamedata_storage_posix *self, const char *dir_name, char *out, size_t cap)
{
    const int written = snprintf(out, cap, PATH_FMT, self->root, dir_name);
    return written > 0 && (size_t)written < cap;
}

/* <root>/<dir_name>/<file> into `out`; refuses truncation. */
static int join_file(const gamedata_storage_posix *self, const char *dir_name, const char *file, char *out, size_t cap)
{
    const int written = snprintf(out, cap, "%s/%s/%s", self->root, dir_name, file);
    return written > 0 && (size_t)written < cap;
}

/* Is `prefix` a libnx device specifier rather than a directory? A trailing ':' marks one, so "sdmc:"
 * is a device name (fsdev) and mkdir must never be called on it, while "/tmp" and "sdmc:/a" are not.
 * Non-static so the host test can exercise the decision directly. */
int gamedata_storage_posix_is_device_prefix(const char *prefix)
{
    if (!prefix || !prefix[0]) return 0;
    return prefix[strlen(prefix) - 1u] == ':';
}

/* mkdir -p over `path`, keeping a trailing "sdmc:" style prefix intact: the device component is never
 * passed to mkdir, every later component is. EEXIST is accepted (a later stat decides whether what
 * exists really is a directory). */
static int make_dirs(const char *path)
{
    char buffer[PATH_MAX_BYTES];
    const size_t length = strlen(path);
    if (!length || length >= sizeof buffer) { errno = ENAMETOOLONG; return -1; }
    memcpy(buffer, path, length + 1u);
    for (size_t i = 1; i < length; ++i) {
        if (buffer[i] != '/') continue;
        buffer[i] = 0;
        /* "sdmc:" names a device, not a directory: it exists already, so mkdir is never called on it */
        if (!gamedata_storage_posix_is_device_prefix(buffer) && mkdir(buffer, 0777) != 0 && errno != EEXIST) return -1;
        buffer[i] = '/';
    }
    if (!gamedata_storage_posix_is_device_prefix(buffer) && mkdir(buffer, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Depth- and count-bounded recursive sum of regular-file sizes below `path`. */
typedef struct walk_state {
    uint64_t bytes;
    unsigned entries;
} walk_state;

static int walk_sum(const char *path, unsigned depth, walk_state *state)
{
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int result = 0;
    for (;;) {
        const struct dirent *entry = readdir(dir);
        if (!entry) break;
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (state->entries >= GAMEDATA_STORAGE_POSIX_MAX_ENTRIES) { result = -1; errno = EOVERFLOW; break; }
        ++state->entries;
        char child[PATH_MAX_BYTES];
        const int written = snprintf(child, sizeof child, "%s/%s", path, name);
        if (written <= 0 || (size_t)written >= sizeof child) { result = -1; errno = ENAMETOOLONG; break; }
        struct stat info;
        if (lstat(child, &info) != 0) { result = -1; break; }
        if (S_ISDIR(info.st_mode)) {
            /* below the maximum depth a subdirectory is reported instead of guessed at */
            if (depth + 1u > GAMEDATA_STORAGE_POSIX_MAX_DEPTH) { result = -1; errno = EOVERFLOW; break; }
            if (walk_sum(child, depth + 1u, state) != 0) { result = -1; break; }
        } else if (S_ISREG(info.st_mode)) {
            state->bytes += (uint64_t)info.st_size;
        }
    }
    closedir(dir);
    return result;
}

/* ---- port functions ------------------------------------------------------------------- */

static gamedata_storage_status port_free_kb(void *context, uint64_t *out_kb)
{
    gamedata_storage_posix *self = context;
    if (!self || !out_kb) return GAMEDATA_STORAGE_IO_ERROR;
    ++self->calls_free;
    struct statvfs info;
    if (statvfs(self->root, &info) != 0) { self->last_status = fail(self, errno); return self->last_status; }
    self->last_errno = 0;
    /* the true 64-bit value: clamping to INT32_MAX is the domain's job */
    *out_kb = (uint64_t)info.f_bavail * (uint64_t)info.f_frsize / 1024u;
    self->last_status = GAMEDATA_STORAGE_OK;
    return self->last_status;
}

static gamedata_storage_status port_stat_dir(void *context, const char *dir_name, uint64_t *size_kb, int64_t times[3])
{
    gamedata_storage_posix *self = context;
    if (!self || !size_kb || !times || !name_ok(dir_name)) return GAMEDATA_STORAGE_IO_ERROR;
    ++self->calls_stat;
    char path[PATH_MAX_BYTES];
    if (!join_dir(self, dir_name, path, sizeof path)) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    struct stat info;
    if (stat(path, &info) != 0) {
        self->last_status = errno == ENOENT ? GAMEDATA_STORAGE_NOT_FOUND : fail(self, errno);
        if (self->last_status == GAMEDATA_STORAGE_NOT_FOUND) self->last_errno = ENOENT;
        return self->last_status;
    }
    if (!S_ISDIR(info.st_mode)) { self->last_status = fail(self, ENOTDIR); return self->last_status; }
    walk_state state = {0, 0};
    if (walk_sum(path, 0, &state) != 0) { self->last_status = fail(self, errno); return self->last_status; }
    self->last_errno = 0;
    *size_kb = (state.bytes + 1023u) / 1024u;   /* ceil */
    times[0] = (int64_t)info.st_atime;
    times[1] = (int64_t)info.st_ctime;
    times[2] = (int64_t)info.st_mtime;
    self->last_status = GAMEDATA_STORAGE_OK;
    return self->last_status;
}

static gamedata_storage_status port_read_file(void *context, const char *dir_name, const char *file, uint8_t *buf,
                                              size_t cap, size_t *len)
{
    gamedata_storage_posix *self = context;
    if (!self || !name_ok(dir_name) || !name_ok(file) || !len) return GAMEDATA_STORAGE_IO_ERROR;
    if (!buf && cap) return GAMEDATA_STORAGE_IO_ERROR;
    ++self->calls_read;
    char path[PATH_MAX_BYTES];
    if (!join_file(self, dir_name, file, path, sizeof path)) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    struct stat info;
    if (stat(path, &info) != 0) {
        self->last_status = errno == ENOENT ? GAMEDATA_STORAGE_NOT_FOUND : fail(self, errno);
        if (self->last_status == GAMEDATA_STORAGE_NOT_FOUND) self->last_errno = ENOENT;
        return self->last_status;
    }
    if (!S_ISREG(info.st_mode)) { self->last_status = fail(self, EISDIR); return self->last_status; }
    if ((uint64_t)info.st_size > (uint64_t)cap) { self->last_status = fail(self, EOVERFLOW); return self->last_status; }
    FILE *stream = fopen(path, "rb");
    if (!stream) { self->last_status = fail(self, errno); return self->last_status; }
    size_t read_bytes = 0;
    if (info.st_size > 0) read_bytes = fread(buf, 1, (size_t)info.st_size, stream);
    const int read_error = ferror(stream) ? EIO : 0;
    if (fclose(stream) != 0 && !read_error) {
        self->last_status = fail(self, errno);
        return self->last_status;
    }
    if (read_error || read_bytes != (size_t)info.st_size) { self->last_status = fail(self, read_error ? read_error : EIO); return self->last_status; }
    *len = read_bytes;
    self->last_errno = 0;
    self->last_status = GAMEDATA_STORAGE_OK;
    return self->last_status;
}

/* mkdir of one component; an existing directory is fine, an existing non-directory is not. */
static int make_dir_component(const char *path)
{
    if (mkdir(path, 0777) != 0) {
        if (errno != EEXIST) return -1;
        struct stat info;
        if (stat(path, &info) != 0) return -1;
        if (!S_ISDIR(info.st_mode)) { errno = ENOTDIR; return -1; }
    }
    return 0;
}

static gamedata_storage_status port_create_dir(void *context, const char *dir_name)
{
    gamedata_storage_posix *self = context;
    if (!self || !name_ok(dir_name)) return GAMEDATA_STORAGE_IO_ERROR;
    ++self->calls_create;
    char dir[PATH_MAX_BYTES], usrdir[PATH_MAX_BYTES];
    if (!join_dir(self, dir_name, dir, sizeof dir)) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    const int written = snprintf(usrdir, sizeof usrdir, "%s/USRDIR", dir);
    if (written <= 0 || (size_t)written >= sizeof usrdir) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    if (make_dir_component(dir) != 0) { self->last_status = fail(self, errno); return self->last_status; }
    if (make_dir_component(usrdir) != 0) { self->last_status = fail(self, errno); return self->last_status; }
    self->last_errno = 0;
    self->last_status = GAMEDATA_STORAGE_OK;
    return self->last_status;
}

/* Write `buf` completely: fwrite + fflush + fclose, each checked. The first failing step's errno is
 * kept, so ENOSPC from any of the three still reaches the caller. */
static int write_complete(const char *path, const uint8_t *buf, size_t len)
{
    int saved = 0;
    FILE *stream = fopen(path, "wb");
    if (!stream) return -1;
    const size_t written = len ? fwrite(buf, 1, len, stream) : 0;
    if (written != len || ferror(stream)) saved = errno ? errno : EIO;
    if (fflush(stream) != 0 && !saved) saved = errno ? errno : EIO;
    if (fclose(stream) != 0 && !saved) saved = errno ? errno : EIO;
    if (saved) { errno = saved; return -1; }
    return 0;
}

static gamedata_storage_status port_write_file(void *context, const char *dir_name, const char *file,
                                               const uint8_t *buf, size_t len)
{
    gamedata_storage_posix *self = context;
    if (!self || !name_ok(dir_name) || !name_ok(file) || (!buf && len)) return GAMEDATA_STORAGE_IO_ERROR;
    ++self->calls_write;
    char target[PATH_MAX_BYTES], temporary[PATH_MAX_BYTES];
    if (!join_file(self, dir_name, file, target, sizeof target)) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    const int written = snprintf(temporary, sizeof temporary, "%s.tmp", target);
    if (written <= 0 || (size_t)written >= sizeof temporary) { self->last_status = fail(self, ENAMETOOLONG); return self->last_status; }
    if (write_complete(temporary, buf, len) != 0) {
        const int saved = errno;
        (void)unlink(temporary);
        self->last_status = fail(self, saved);
        return self->last_status;
    }
    if (rename(temporary, target) != 0) {
        /* fsdev on FAT/exFAT may refuse rename-over an existing file: remove the target, retry ONCE */
        if (errno != EEXIST && errno != ENOTEMPTY && errno != EACCES && errno != EPERM) {
            const int saved = errno;
            (void)unlink(temporary);
            self->last_status = fail(self, saved);
            return self->last_status;
        }
        if (unlink(target) != 0 && errno != ENOENT) {
            const int saved = errno;
            (void)unlink(temporary);
            self->last_status = fail(self, saved);
            return self->last_status;
        }
        if (rename(temporary, target) != 0) {
            const int saved = errno;
            (void)unlink(temporary);
            self->last_status = fail(self, saved);
            return self->last_status;
        }
    }
    self->last_errno = 0;
    self->last_status = GAMEDATA_STORAGE_OK;
    return self->last_status;
}

/* ---- public --------------------------------------------------------------------------- */

int gamedata_storage_posix_init(gamedata_storage_posix *self, const char *root)
{
    if (!self) return -1;
    memset(self, 0, sizeof *self);
    self->last_status = GAMEDATA_STORAGE_OK;
    if (!root || !root[0]) { self->last_errno = EINVAL; return -1; }
    const size_t length = strlen(root);
    if (length >= GAMEDATA_STORAGE_POSIX_ROOT_MAX) { self->last_errno = ENAMETOOLONG; return -1; }
    memcpy(self->root, root, length + 1u);
    /* normalize one trailing '/' away ("sdmc:/x/" and "/x/" are the same root) */
    if (length > 1u && self->root[length - 1u] == '/' &&
        !(length == 5u && !strcmp(self->root, "sdmc:"))) {
        self->root[length - 1u] = 0;
    }
    if (make_dirs(self->root) != 0) { self->last_errno = errno; return -1; }
    struct stat info;
    if (stat(self->root, &info) != 0) { self->last_errno = errno; return -1; }
    if (!S_ISDIR(info.st_mode)) { self->last_errno = ENOTDIR; return -1; }
    self->last_errno = 0;
    return 0;
}

gamedata_storage_port gamedata_storage_posix_port(gamedata_storage_posix *self)
{
    gamedata_storage_port port;
    memset(&port, 0, sizeof port);
    port.context = self;
    port.free_kb = port_free_kb;
    port.stat_dir = port_stat_dir;
    port.read_file = port_read_file;
    port.create_dir = port_create_dir;
    port.write_file = port_write_file;
    return port;
}
