/* Host tests for the platform-free file-system service (native/domain/fs_service.c) and its
 * POSIX adapter (native/adapters/fs_storage_posix.c).
 *
 * Two halves:
 *   1. the domain against an in-memory port that records the exact host path it was handed,
 *      so confinement can be asserted on the bytes the service produced;
 *   2. the adapter over a REAL scratch tree below the build dir the verifier passes:
 *      `mkdtemp` inside that dir, the root token set to that scratch root, and a sibling
 *      sentinel directory OUTSIDE the root that must survive every delete.
 *
 * Every guest path is passed as a literal through LEN(), so a length argument can never
 * disagree with its string. Nothing here touches an SD card, a game, a GPU or the network.
 *
 * Usage: test_fs_service <scratch-build-dir>
 */
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs_service.h"
#include "fs_storage_posix.h"

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
        const unsigned char *a_ = (const unsigned char *)(actual);                     \
        const unsigned char *e_ = (const unsigned char *)(expected);                   \
        size_t i_ = 0;                                                                \
        while (i_ < n_ && a_[i_] == e_[i_]) i_++;                                     \
        checks++;                                                                     \
        if (i_ != n_) {                                                               \
            failures++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s differs at 0x%zX (got 0x%02X want 0x%02X)\n", \
                    __FILE__, __LINE__, #actual, i_, a_[i_], e_[i_]);                 \
        }                                                                            \
    } while (0)

/* strlen of a literal / string, at the call site, so the length argument is derived and
 * can never drift from the text. */
#define LEN(text) ((size_t)strlen(text))

/* A production-shaped root token. Nothing is written there: the domain test uses an
 * in-memory port, and the adapter test builds its own scratch root. */
#define PRODUCTION_ROOT "sdmc:/switch/infamous-native-spu-stage31"

/* ------------------------------------------------------------------------------------ */
/* 1. Domain: in-memory port                                                             */
/* ------------------------------------------------------------------------------------ */

#define FAKE_CHILDREN 8u
#define FAKE_TREE_NODES 24u
#define FAKE_TREE_HANDLES 8u

typedef struct fake_entry {
    int present;
    int is_directory;
    uint64_t size;
} fake_entry;

typedef struct fake_tree_node {
    char path[512];
    int is_directory;
    uint64_t size;
} fake_tree_node;

typedef struct fake_tree_handle {
    int used;
    char path[512];
} fake_tree_handle;

typedef struct fake_port {
    char last_path[2048];
    unsigned stat_calls, open_calls, read_calls, close_calls, remove_calls, unlink_calls;
    unsigned fail_open; /* 1 -> open_dir reports NOT_FOUND */
    fs_storage_status fail_stat_status;
    int fail_stat;
    fake_entry entry; /* the one object `stat` describes */
    fs_dir_entry entries[FAKE_CHILDREN];
    unsigned entry_count;
    int tree_mode;
    fake_tree_node tree[FAKE_TREE_NODES];
    unsigned tree_count;
    fake_tree_handle tree_handles[FAKE_TREE_HANDLES];
    int live_handles;
    unsigned open_file_calls, read_file_calls, write_file_calls, close_file_calls, file_size_calls;
    unsigned make_dir_calls, rename_calls, sync_calls;
    fs_storage_status rename_status; /* what `rename_path` answers; OK renames in `tree` */
    fs_storage_status next_open_status;
    uint8_t file_data[512];
    uint32_t file_size;
    uint32_t opened_access;
    int live_files;
} fake_port;

static void fake_note(fake_port *self, const char *host_path)
{
    memset(self->last_path, 0, sizeof self->last_path);
    if (host_path) snprintf(self->last_path, sizeof self->last_path, "%s", host_path);
}

static fs_storage_status fake_stat(void *context, const char *host_path, fs_stat_info *out)
{
    fake_port *self = context;
    ++self->stat_calls;
    fake_note(self, host_path);
    if (self->fail_stat) return self->fail_stat_status;
    if (self->tree_mode) {
        unsigned index;
        for (index = 0; index < self->tree_count; ++index) {
            if (strcmp(self->tree[index].path, host_path) != 0) continue;
            memset(out, 0, sizeof *out);
            out->is_directory = self->tree[index].is_directory ? 1u : 0u;
            out->mode = self->tree[index].is_directory ? 0555u : 0444u;
            out->size = self->tree[index].size;
            out->blksize = 4096u;
            return FS_STORAGE_OK;
        }
        return FS_STORAGE_NOT_FOUND;
    }
    if (!self->entry.present) return FS_STORAGE_NOT_FOUND;
    memset(out, 0, sizeof *out);
    out->is_directory = (uint32_t)self->entry.is_directory;
    out->mode = self->entry.is_directory ? 0555u : 0444u;
    out->uid = 0;
    out->gid = 0;
    out->atime = 0x1111111111111111LL;
    out->mtime = 0x2222222222222222LL;
    out->ctime = 0x3333333333333333LL;
    out->size = self->entry.size;
    out->blksize = 4096u;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_open_dir(void *context, const char *host_path, void **out_handle)
{
    fake_port *self = context;
    ++self->open_calls;
    fake_note(self, host_path);
    if (self->fail_open) return FS_STORAGE_NOT_FOUND;
    if (self->tree_mode) {
        unsigned index;
        int found = 0;
        for (index = 0; index < self->tree_count; ++index) {
            if (strcmp(self->tree[index].path, host_path) == 0 &&
                self->tree[index].is_directory) { found = 1; break; }
        }
        if (!found) return FS_STORAGE_NOT_FOUND;
        for (index = 0; index < FAKE_TREE_HANDLES; ++index) {
            fake_tree_handle *opened = &self->tree_handles[index];
            if (opened->used) continue;
            opened->used = 1;
            memset(opened->path, 0, sizeof opened->path);
            snprintf(opened->path, sizeof opened->path, "%s", host_path);
            ++self->live_handles;
            *out_handle = opened;
            return FS_STORAGE_OK;
        }
        return FS_STORAGE_IO_ERROR;
    }
    ++self->live_handles;
    *out_handle = self;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_read_dir(void *context, void *handle, uint32_t index, fs_dir_entry *out,
                                       uint32_t *out_has_entry)
{
    fake_port *self = context;
    (void)handle;
    ++self->read_calls;
    if (self->tree_mode) {
        fake_tree_handle *opened = (fake_tree_handle *)handle;
        size_t base_length = strlen(opened->path);
        unsigned seen = 0, node_index;
        for (node_index = 0; node_index < self->tree_count; ++node_index) {
            const fake_tree_node *node = &self->tree[node_index];
            const char *name;
            size_t name_length;
            if (strncmp(node->path, opened->path, base_length) != 0 ||
                node->path[base_length] != '/') continue;
            name = node->path + base_length + 1u;
            if (strchr(name, '/') != NULL) continue;
            if (seen++ != index) continue;
            memset(out, 0, sizeof *out);
            name_length = strlen(name);
            out->is_directory = node->is_directory ? 1u : 0u;
            out->name_length = (uint32_t)name_length;
            memcpy(out->name, name, name_length + 1u);
            *out_has_entry = 1u;
            return FS_STORAGE_OK;
        }
        *out_has_entry = 0u;
        return FS_STORAGE_OK;
    }
    *out_has_entry = index < self->entry_count ? 1u : 0u;
    if (!*out_has_entry) return FS_STORAGE_OK;
    *out = self->entries[index];
    return FS_STORAGE_OK;
}

static fs_storage_status fake_close_dir(void *context, void *handle)
{
    fake_port *self = context;
    (void)context;
    ++self->close_calls;
    if (self->tree_mode) {
        fake_tree_handle *opened = (fake_tree_handle *)handle;
        if (opened && opened->used) {
            opened->used = 0;
            if (self->live_handles > 0) --self->live_handles;
        }
        return FS_STORAGE_OK;
    }
    if (self->live_handles > 0) --self->live_handles;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_remove_dir(void *context, const char *host_path)
{
    fake_port *self = context;
    ++self->remove_calls;
    fake_note(self, host_path);
    if (self->tree_mode) return FS_STORAGE_NOT_FOUND;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_unlink_file(void *context, const char *host_path)
{
    fake_port *self = context;
    ++self->unlink_calls;
    fake_note(self, host_path);
    if (self->tree_mode) return FS_STORAGE_NOT_FOUND;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_open_file(void *context, const char *host_path, uint32_t access,
                                        uint32_t create, uint32_t exclusive, uint32_t truncate,
                                        void **out_handle)
{
    fake_port *self = context;
    ++self->open_file_calls;
    fake_note(self, host_path);
    self->opened_access = access;
    if (self->next_open_status != FS_STORAGE_OK) return self->next_open_status;
    if (self->tree_mode) {
        unsigned index;
        int found = 0, is_directory = 0;
        for (index = 0; index < self->tree_count; ++index) {
            if (strcmp(self->tree[index].path, host_path) != 0) continue;
            found = 1;
            is_directory = self->tree[index].is_directory;
            break;
        }
        if (!found && !create) return FS_STORAGE_NOT_FOUND;
        if (found && create && exclusive) return FS_STORAGE_ALREADY_EXISTS;
        if (found && is_directory) return FS_STORAGE_IS_A_DIRECTORY;
        ++self->live_files;
        *out_handle = self;
        return FS_STORAGE_OK;
    }
    if (!self->entry.present && !create) return FS_STORAGE_NOT_FOUND;
    if (create && exclusive && self->entry.present) return FS_STORAGE_ALREADY_EXISTS;
    if (self->entry.is_directory) return FS_STORAGE_IS_A_DIRECTORY;
    if (create) self->entry.present = 1;
    if (truncate) self->file_size = 0u;
    ++self->live_files;
    *out_handle = self;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_read_file(void *context, void *handle, uint64_t offset,
                                        uint8_t *buffer, uint32_t length, uint32_t *out_read)
{
    fake_port *self = context;
    uint32_t count = 0;
    (void)handle;
    ++self->read_file_calls;
    if (offset < self->file_size) {
        const uint64_t available = (uint64_t)self->file_size - offset;
        count = available < length ? (uint32_t)available : length;
        if (count != 0u) memcpy(buffer, self->file_data + offset, count);
    }
    *out_read = count;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_write_file(void *context, void *handle, uint64_t offset,
                                         const uint8_t *buffer, uint32_t length,
                                         uint32_t *out_written)
{
    fake_port *self = context;
    (void)handle;
    ++self->write_file_calls;
    if (length != 0u) memcpy(self->file_data + offset, buffer, length);
    if (offset + length > self->file_size) self->file_size = (uint32_t)(offset + length);
    *out_written = length;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_file_size(void *context, void *handle, uint64_t *out_size)
{
    fake_port *self = context;
    (void)handle;
    ++self->file_size_calls;
    *out_size = self->file_size;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_close_file(void *context, void *handle)
{
    fake_port *self = context;
    (void)handle;
    ++self->close_file_calls;
    if (self->live_files > 0) --self->live_files;
    return FS_STORAGE_OK;
}

static fs_storage_status fake_make_dir(void *context, const char *host_path)
{
    fake_port *self = context;
    ++self->make_dir_calls;
    fake_note(self, host_path);
    return FS_STORAGE_OK;
}

static fs_storage_status fake_rename_path(void *context, const char *host_from,
                                          const char *host_to)
{
    fake_port *self = context;
    unsigned index;

    ++self->rename_calls;
    fake_note(self, host_to);
    if (self->rename_status != FS_STORAGE_OK)
        return self->rename_status;
    for (index = 0; index < self->tree_count; ++index) {
        if (strcmp(self->tree[index].path, host_from) != 0) continue;
        snprintf(self->tree[index].path, sizeof self->tree[index].path, "%s", host_to);
        return FS_STORAGE_OK;
    }
    return FS_STORAGE_OK;
}

static fs_storage_status fake_sync_file(void *context, void *handle)
{
    fake_port *self = context;
    (void)handle;
    ++self->sync_calls;
    return FS_STORAGE_OK;
}

static fs_storage_port fake_make_port(fake_port *self)
{
    fs_storage_port port;
    memset(&port, 0, sizeof port);
    port.context = self;
    port.stat = fake_stat;
    port.open_dir = fake_open_dir;
    port.read_dir = fake_read_dir;
    port.close_dir = fake_close_dir;
    port.remove_dir = fake_remove_dir;
    port.unlink_file = fake_unlink_file;
    port.open_file = fake_open_file;
    port.read_file = fake_read_file;
    port.write_file = fake_write_file;
    port.file_size = fake_file_size;
    port.close_file = fake_close_file;
    port.make_dir = fake_make_dir;
    port.rename_path = fake_rename_path;
    port.sync_file = fake_sync_file;
    return port;
}

static void fake_add_entry(fake_port *self, const char *name, int is_directory)
{
    fs_dir_entry *entry;
    if (self->entry_count >= FAKE_CHILDREN) return;
    entry = &self->entries[self->entry_count++];
    memset(entry, 0, sizeof *entry);
    entry->is_directory = (uint32_t)is_directory;
    entry->name_length = (uint32_t)strlen(name);
    memcpy(entry->name, name, entry->name_length + 1u);
}

static void fake_tree_add(fake_port *self, const char *path, int is_directory, uint64_t size)
{
    fake_tree_node *node;
    if (self->tree_count >= FAKE_TREE_NODES) return;
    node = &self->tree[self->tree_count++];
    memset(node, 0, sizeof *node);
    snprintf(node->path, sizeof node->path, "%s", path);
    node->is_directory = is_directory;
    node->size = size;
}

/* The 52-byte CellFsStat record, byte for byte (CONTRACT.md §1c offsets, big-endian). */
static void test_stat_record_bytes(void)
{
    fs_stat_info info;
    uint8_t got[52];
    static const uint8_t expected[52] = {
        /* +0x00 mode = IFDIR|0555 = 0o040555 = 0x0000416D */
        0x00, 0x00, 0x41, 0x6D,
        /* +0x04 uid */
        0x00, 0x00, 0x00, 0x07,
        /* +0x08 gid */
        0x00, 0x00, 0x00, 0x09,
        /* +0x0C atime */
        0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
        /* +0x14 mtime */
        0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
        /* +0x1C ctime */
        0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33,
        /* +0x24 size */
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
        /* +0x2C blksize */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    };

    CHECK_EQ_INT(FS_CELLFS_STAT_BYTES, 52u);
    CHECK_EQ_INT(sizeof got, 52u);

    memset(&info, 0, sizeof info);
    info.is_directory = 1;
    info.mode = 0555u;
    info.uid = 7;
    info.gid = 9;
    info.atime = 0x1111111111111111LL;
    info.mtime = 0x2222222222222222LL;
    info.ctime = 0x3333333333333333LL;
    info.size = 0x0000000100000002ULL;
    info.blksize = 4096u;

    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_stat(&info, got);
    CHECK_EQ_MEM(got, expected, 52u);

    /* a regular file: IFREG 0100000 | 0444 = 0o100444 = 0x00008124 */
    info.is_directory = 0;
    info.mode = 0444u;
    fs_encode_cellfs_stat(&info, got);
    CHECK_EQ_INT(got[0], 0x00);
    CHECK_EQ_INT(got[1], 0x00);
    CHECK_EQ_INT(got[2], 0x81);
    CHECK_EQ_INT(got[3], 0x24);

    /* the size lives exactly at 0x24..0x2B */
    info.size = 0xDEADBEEFCAFEF00DULL;
    fs_encode_cellfs_stat(&info, got);
    CHECK_EQ_INT(got[0x24], 0xDE);
    CHECK_EQ_INT(got[0x25], 0xAD);
    CHECK_EQ_INT(got[0x26], 0xBE);
    CHECK_EQ_INT(got[0x27], 0xEF);
    CHECK_EQ_INT(got[0x28], 0xCA);
    CHECK_EQ_INT(got[0x29], 0xFE);
    CHECK_EQ_INT(got[0x2A], 0xF0);
    CHECK_EQ_INT(got[0x2B], 0x0D);
    /* blksize at 0x2C..0x33 and nothing past 52 */
    CHECK_EQ_INT(got[0x2C], 0x00);
    CHECK_EQ_INT(got[0x2F], 0x00);
    CHECK_EQ_INT(got[0x30], 0x00);
    CHECK_EQ_INT(got[0x31], 0x00);
    CHECK_EQ_INT(got[0x32], 0x10);
    CHECK_EQ_INT(got[0x33], 0x00);
}

/* The dirent encoder: byte 0 is the 1/2 tag the walker compares (CALLS.md §3a), byte 1 the
 * length, the name from byte 2, NUL-terminated, nothing written past byte 18. */
static void test_dirent_bytes(void)
{
    fs_dir_entry entry;
    uint8_t got[FS_CELLFS_DIRENT_BYTES + 8u];

    CHECK_EQ_INT(FS_CELLFS_DIRENT_BYTES, 258u);   /* root 2026-11-11: the real CellFsDirent (was the 19-byte compact form) */
    CHECK_EQ_INT(FS_CELLFS_DTYPE_FILE, 1u);
    CHECK_EQ_INT(FS_CELLFS_DTYPE_DIR, 2u);

    memset(&entry, 0, sizeof entry);
    entry.is_directory = 0;
    entry.name_length = 5;
    memcpy(entry.name, "a.txt", 6u);
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_dirent(&entry, got);
    CHECK_EQ_INT(got[0], FS_CELLFS_DTYPE_FILE);
    CHECK_EQ_INT(got[1], 5);
    CHECK_EQ_MEM(got + 2, "a.txt", 6u);
    CHECK_EQ_INT(got[8], 0x00); /* padded */
    CHECK_EQ_INT(got[FS_CELLFS_DIRENT_BYTES - 1u], 0x00);
    CHECK_EQ_INT(got[FS_CELLFS_DIRENT_BYTES], 0xA5); /* nothing past the record */

    entry.is_directory = 1;
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_dirent(&entry, got);
    CHECK_EQ_INT(got[0], FS_CELLFS_DTYPE_DIR);
    CHECK_EQ_INT(got[0], 2); /* the literal the guest compares against */

    /* a 16-byte name is the maximum that fits, and it is still NUL-terminated */
    memset(&entry, 0, sizeof entry);
    entry.name_length = 16;
    memcpy(entry.name, "0123456789abcdef", 16u);
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_dirent(&entry, got);
    CHECK_EQ_INT(got[1], 16);
    CHECK_EQ_MEM(got + 2, "0123456789abcdef", 16u);
    CHECK_EQ_INT(got[2u + 16u], 0x00);

    /* root 2026-11-11: a 20-character name is kept whole (the limit is 255 now) and stays a C string */
    entry.name_length = 20;
    memcpy(entry.name, "0123456789abcdefghij", 20u);
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_dirent(&entry, got);
    CHECK_EQ_INT(got[1], 20);
    CHECK_EQ_MEM(got + 2, "0123456789abcdefghij", 20u);
    CHECK_EQ_INT(got[2u + 20u], 0x00);
    /* a name longer than the limit is truncated to 255 */
    entry.name_length = FS_DIR_ENTRY_NAME_MAX;
    memset(entry.name, 'x', sizeof entry.name);
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_dirent(&entry, got);
    CHECK_EQ_INT(got[1], 255);
    CHECK_EQ_INT(got[2u + 255u], 0x00);
    CHECK_EQ_INT(got[FS_CELLFS_DIRENT_BYTES], 0xA5);

    /* nread: 8 bytes, big-endian, 0 only at end of directory */
    CHECK_EQ_INT(FS_CELLFS_NREAD_BYTES, 8u);
    memset(got, 0xA5, sizeof got);
    fs_encode_cellfs_nread(1u, got);
    CHECK_EQ_INT(got[0], 0);
    CHECK_EQ_INT(got[7], 1);
    fs_encode_cellfs_nread(0u, got);
    CHECK_EQ_INT(got[7], 0);
    CHECK_EQ_INT(got[0], 0);
}

/* Every resolution rule, against a service whose mounts all live under the production root. */
static void test_path_confinement(void)
{
    fs_service service;
    fs_storage_port port;
    fake_port fake;
    fs_resolved_path resolved;
    unsigned index;
    char long_path[FS_PATH_MAX + 64u];
    char long_component[FS_DIR_ENTRY_NAME_MAX + 8u];

    memset(&fake, 0, sizeof fake);
    port = fake_make_port(&fake);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), PRODUCTION_ROOT), 0);
    CHECK_EQ_INT(service.mount_count, FS_MOUNT_CAPACITY);

    /* the canonical host token is exactly what decision 24 names */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0", LEN("/dev_hdd0"), &resolved), FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0") == 0);
    CHECK_EQ_INT(strcmp(resolved.relative, ""), 0);
    CHECK_EQ_INT(resolved.component_count, 0u);

    CHECK_EQ_INT(fs_resolve_path(&service, "/app_home/PS3_GAME/USRDIR/x.psarc",
                                 LEN("/app_home/PS3_GAME/USRDIR/x.psarc"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/app_home/PS3_GAME/USRDIR/x.psarc") == 0);
    CHECK(strcmp(resolved.relative, "PS3_GAME/USRDIR/x.psarc") == 0);

    /* '/' and '.' collapse; a trailing '/' asks for a directory */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0//cache/./psarc/",
                                 LEN("/dev_hdd0//cache/./psarc/"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0/cache/psarc") == 0);
    CHECK_EQ_INT(resolved.requires_directory, 1u);

    /* ".." pops one component */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/cache/psarc/..",
                                 LEN("/dev_hdd0/cache/psarc/.."), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0/cache") == 0);

    /* ".." at the mount root is clamped: it never leaves the mount */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/..", LEN("/dev_hdd0/.."), &resolved), FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0") == 0);
    CHECK_EQ_INT(resolved.requires_directory, 1u);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/../../../..",
                                 LEN("/dev_hdd0/../../../.."), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0") == 0);

    /* ".." cannot jump from one mount into another or above the token */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/../hdd1/x",
                                 LEN("/dev_hdd0/../hdd1/x"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0/hdd1/x") == 0);
    CHECK_EQ_INT(fs_resolve_path(&service, "/app_home/a/../../b",
                                 LEN("/app_home/a/../../b"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/app_home/b") == 0);

    /* an absolute host path the guest could name stays a relative component of the mount */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0//etc/passwd",
                                 LEN("/dev_hdd0//etc/passwd"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/hdd0/etc/passwd") == 0);
    CHECK_EQ_INT(fs_resolve_path(&service, "/host_root/dev_hdd0/x",
                                 LEN("/host_root/dev_hdd0/x"), &resolved),
                 FS_OK);
    CHECK(strcmp(resolved.host, PRODUCTION_ROOT "/host_root/dev_hdd0/x") == 0);

    /* non-absolute -> ENOENT */
    CHECK_EQ_INT(fs_resolve_path(&service, "dev_hdd0/x", LEN("dev_hdd0/x"), &resolved),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_resolve_path(&service, "", 0u, &resolved), FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_resolve_path(&service, "sdmc:/x", LEN("sdmc:/x"), &resolved),
                 FS_ERROR_ENOENT);

    /* unknown mount -> ENOENT, and matching is on a component boundary */
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_whatever/x", LEN("/dev_whatever/x"), &resolved),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0x/y", LEN("/dev_hdd0x/y"), &resolved),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_resolve_path(&service, "/DEV_HDD0/y", LEN("/DEV_HDD0/y"), &resolved),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0", LEN("/dev_hdd0"), &resolved), FS_OK);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), &resolved), FS_OK);

    /* > 1024 bytes -> ENAMETOOLONG (the limit includes the NUL) */
    memset(long_path, 'a', sizeof long_path);
    memcpy(long_path, "/dev_hdd0/", LEN("/dev_hdd0/"));
    long_path[FS_PATH_MAX - 1u] = 0; /* exactly 1024 bytes, no NUL inside them */
    CHECK_EQ_INT(fs_resolve_path(&service, long_path, FS_PATH_MAX - 1u, &resolved),
                 FS_ERROR_ENAMETOOLONG);
    CHECK_EQ_INT(fs_resolve_path(&service, long_path, FS_PATH_MAX, &resolved),
                 FS_ERROR_ENAMETOOLONG);
    /* a path with no NUL inside the stated length is too long as well */
    memset(long_path, '/', 32u);
    CHECK_EQ_INT(fs_resolve_path(&service, long_path, 32u, &resolved), FS_ERROR_ENAMETOOLONG);
    /* a single component over 16 bytes -> ENAMETOOLONG (the dirent name field) */
    memset(long_component, 'b', sizeof long_component);
    long_component[FS_DIR_ENTRY_NAME_MAX + 1u] = 0;
    snprintf(long_path, sizeof long_path, "/dev_hdd0/%s", long_component);
    CHECK_EQ_INT(fs_resolve_path(&service, long_path, strlen(long_path), &resolved),
                 FS_ERROR_ENAMETOOLONG);

    /* an unregistered mount is ENOENT even when it is canonical */
    fs_mount_clear(&service);
    CHECK_EQ_INT(service.mount_count, 0u);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0", LEN("/dev_hdd0"), &resolved),
                 FS_ERROR_ENOENT);

    /* only canonical prefixes can be mounted, and a repeated add replaces rather than doubles */
    CHECK_EQ_INT(fs_mount_add(&service, "/tmp", PRODUCTION_ROOT), -1);
    CHECK_EQ_INT(fs_mount_add(&service, "/dev_hdd0/", PRODUCTION_ROOT), -1);
    CHECK_EQ_INT(fs_mount_add(&service, "/dev_hdd0", ""), -1);
    CHECK_EQ_INT(fs_mount_add(&service, "/dev_hdd0", PRODUCTION_ROOT), 0);
    CHECK_EQ_INT(fs_mount_add(&service, "/dev_hdd0", "/other/root/"), 0);
    CHECK_EQ_INT(service.mount_count, 1u);
    CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), &resolved), FS_OK);
    CHECK(strcmp(resolved.host, "/other/root/hdd0/x") == 0); /* the trailing '/' is normalised */
}

/* stat / opendir / readdir / closedir / rmdir / unlink over the fake port. */
static void test_operations(void)
{
    fs_service service;
    fs_storage_port port;
    fake_port fake;
    uint8_t stat_bytes[52];
    uint8_t dirent[FS_CELLFS_DIRENT_BYTES];
    uint8_t nread[FS_CELLFS_NREAD_BYTES];
    uint32_t handle = 0;
    uint32_t handle2 = 0;
    unsigned index;

    memset(&fake, 0, sizeof fake);
    port = fake_make_port(&fake);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        (void)fs_mount_add(&service, fs_canonical_guest_mount(index), PRODUCTION_ROOT);

    /* ---- stat: EFAULT on a NULL record, ENOENT from resolution, ENOENT from the port ---- */
    fake.entry.present = 1;
    fake.entry.is_directory = 1;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_stat(&service, "relative", LEN("relative"), stat_bytes), FS_ERROR_ENOENT);
    CHECK_EQ_INT(fake.stat_calls, 0u); /* neither refused call reached the port */
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), stat_bytes), FS_OK);
    CHECK_EQ_INT(fake.stat_calls, 1u);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/x") == 0);
    CHECK_EQ_INT(stat_bytes[3], 0x6D); /* IFDIR|0555 = 0o040555 = 0x0000416D */
    CHECK_EQ_INT(stat_bytes[0], 0x00);
    CHECK_EQ_INT(stat_bytes[2], 0x41);
    fake.entry.present = 0;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/gone", LEN("/dev_hdd0/gone"), stat_bytes),
                 FS_ERROR_ENOENT);
    fake.entry.present = 1;
    fake.fail_stat = 1;
    fake.fail_stat_status = FS_STORAGE_ACCESS_DENIED;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), stat_bytes),
                 FS_ERROR_EACCES);
    fake.fail_stat = 0;

    /* a path that can only be a directory, but stats as a regular file -> ENOTDIR */
    fake.entry.is_directory = 0;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x/", LEN("/dev_hdd0/x/"), stat_bytes),
                 FS_ERROR_ENOTDIR);
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x/..", LEN("/dev_hdd0/x/.."), stat_bytes),
                 FS_ERROR_ENOTDIR);
    fake.entry.is_directory = 1;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/x/", LEN("/dev_hdd0/x/"), stat_bytes), FS_OK);

    /* ---- opendir: EFAULT on a NULL out, ENOENT from the port ---- */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/d", LEN("/dev_hdd0/d"), NULL), FS_ERROR_EFAULT);
    fake.fail_open = 1;
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/d", LEN("/dev_hdd0/d"), &handle),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(handle, 0u);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/d") == 0);
    fake.fail_open = 0;

    /* ---- a 3-level tree, enumerated in order, nread == 0 exactly at the end ---- */
    fake.entry_count = 0;
    fake_add_entry(&fake, "file1", 0);
    fake_add_entry(&fake, "sub1", 1);
    fake_add_entry(&fake, "file2", 0);
    fake_add_entry(&fake, "sub-d", 1);
    fake_add_entry(&fake, "0123456789abcdef", 0); /* exactly 16 bytes: the field maximum */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"), &handle), FS_OK);
    CHECK(handle != 0u);
    CHECK_EQ_INT(fs_open_descriptor_count(&service), 1u);

    {
        static const char *const names[] = {"file1", "sub1", "file2", "sub-d", "0123456789abcdef"};
        static const int is_dir[] = {0, 1, 0, 1, 0};
        unsigned seen;
        CHECK_EQ_INT(sizeof names / sizeof names[0], 5u);
        for (seen = 0; seen < 5u; ++seen) {
            memset(dirent, 0xA5, sizeof dirent);
            memset(nread, 0xA5, sizeof nread);
            CHECK_EQ_INT(fs_readdir(&service, handle, dirent, nread), FS_OK);
            CHECK_EQ_INT(nread[7], 1); /* an entry */
            CHECK_EQ_INT(dirent[0], is_dir[seen] ? FS_CELLFS_DTYPE_DIR : FS_CELLFS_DTYPE_FILE);
            CHECK_EQ_INT(dirent[1], (int)strlen(names[seen]));
            CHECK(strncmp((const char *)dirent + 2, names[seen], strlen(names[seen])) == 0);
        }
        for (seen = 0; seen < 3u; ++seen) {
            memset(dirent, 0xA5, sizeof dirent);
            memset(nread, 0xA5, sizeof nread);
            CHECK_EQ_INT(fs_readdir(&service, handle, dirent, nread), FS_OK);
            CHECK_EQ_INT(nread[7], 0);     /* end of directory, and it stays there */
            CHECK_EQ_INT(dirent[0], 0xA5); /* the record is not written at end of directory */
        }
    }
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_OK);
    CHECK_EQ_INT(fs_open_descriptor_count(&service), 0u);
    CHECK_EQ_INT(fake.live_handles, 0);

    /* ---- double close and a stale handle ---- */
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_ERROR_EBADF);
    {
        uint32_t stale = handle;
        CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"), &handle2), FS_OK);
        CHECK(handle2 != handle); /* the generation changed, so the old value stays invalid */
        memset(nread, 0, sizeof nread);
        CHECK_EQ_INT(fs_readdir(&service, stale, dirent, nread), FS_ERROR_EBADF);
        CHECK_EQ_INT(fs_closedir(&service, stale), FS_ERROR_EBADF);
        CHECK_EQ_INT(fs_closedir(&service, handle2), FS_OK);
        CHECK_EQ_INT(fs_closedir(&service, handle2), FS_ERROR_EBADF);
        CHECK_EQ_INT(fs_closedir(&service, 0u), FS_ERROR_EBADF);
        CHECK_EQ_INT(fs_closedir(&service, 0xFFFFFFFFu), FS_ERROR_EBADF);
    }

    /* ---- EFAULT on a NULL dirent or nread ---- */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"), &handle), FS_OK);
    CHECK_EQ_INT(fs_readdir(&service, handle, NULL, nread), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_readdir(&service, handle, dirent, NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_readdir(&service, 0u, dirent, nread), FS_ERROR_EBADF);

    /* ---- EMFILE at capacity, then the table drains again ---- */
    {
        uint32_t handles[FS_DESCRIPTOR_CAPACITY];
        unsigned opened;
        for (opened = 0; opened < FS_DESCRIPTOR_CAPACITY - 1u; ++opened)
            CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"),
                                    &handles[opened]),
                         FS_OK);
        CHECK_EQ_INT(fs_open_descriptor_count(&service), FS_DESCRIPTOR_CAPACITY);
        CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"), &handle),
                     FS_ERROR_EMFILE);
        CHECK_EQ_INT(service.counters.refusals_capacity, 1u);
        for (opened = 0; opened < FS_DESCRIPTOR_CAPACITY - 1u; ++opened)
            CHECK_EQ_INT(fs_closedir(&service, handles[opened]), FS_OK);
        CHECK_EQ_INT(fs_open_descriptor_count(&service), 1u);
        CHECK_EQ_INT(fs_closedir(&service, handle), FS_OK);
        CHECK_EQ_INT(fs_open_descriptor_count(&service), 0u);
    }

    /* ---- rmdir / unlink: real deletes, confined, never EROFS ---- */
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/cache/x", LEN("/dev_hdd0/cache/x")), FS_OK);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/cache/x") == 0);
    CHECK_EQ_INT(service.counters.deletes_dir_ok, 1u);
    CHECK_EQ_INT(fs_unlink(&service, "/dev_hdd0/cache/x/a.psarc_s",
                           LEN("/dev_hdd0/cache/x/a.psarc_s")),
                 FS_OK);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/cache/x/a.psarc_s") == 0);
    CHECK_EQ_INT(service.counters.deletes_file_ok, 1u);

    /* the mount root itself is not removable */
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0", LEN("/dev_hdd0")), FS_ERROR_EACCES);
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/", LEN("/dev_hdd0/")), FS_ERROR_EACCES);
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/..", LEN("/dev_hdd0/..")), FS_ERROR_EACCES);
    CHECK_EQ_INT(fs_unlink(&service, "/dev_hdd0", LEN("/dev_hdd0")), FS_ERROR_EACCES);
    CHECK_EQ_INT(fake.remove_calls, 1u);
    CHECK_EQ_INT(fake.unlink_calls, 1u);

    /* ".." is clamped, so a delete can never be aimed above the mount root */
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/../../../etc", LEN("/dev_hdd0/../../../etc")),
                 FS_OK);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/etc") == 0);
    CHECK_EQ_INT(fs_unlink(&service, "/dev_hdd0/../hdd1/x", LEN("/dev_hdd0/../hdd1/x")), FS_OK);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/hdd1/x") == 0);

    /* an unresolved path never reaches the port */
    {
        const unsigned before = fake.remove_calls;
        CHECK_EQ_INT(fs_rmdir(&service, "dev_hdd0/x", LEN("dev_hdd0/x")), FS_ERROR_ENOENT);
        CHECK_EQ_INT(fs_rmdir(&service, "/dev_unknown/x", LEN("/dev_unknown/x")), FS_ERROR_ENOENT);
        CHECK_EQ_INT(fake.remove_calls, before);
    }

    /* a missing port function is ENOTSUP, not a silent success */
    {
        fs_storage_port empty;
        fs_service bare;
        memset(&empty, 0, sizeof empty);
        fs_service_init(&bare, &empty);
        (void)fs_mount_add(&bare, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_stat(&bare, "/dev_hdd0/x", LEN("/dev_hdd0/x"), stat_bytes),
                     FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_opendir(&bare, "/dev_hdd0/x", LEN("/dev_hdd0/x"), &handle),
                     FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_rmdir(&bare, "/dev_hdd0/x", LEN("/dev_hdd0/x")), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_unlink(&bare, "/dev_hdd0/x", LEN("/dev_hdd0/x")), FS_ERROR_ENOTSUP);
    }

    /* the counters exist for the integrator's report */
    CHECK(service.counters.calls_stat > 0u);
    CHECK(service.counters.calls_opendir > 0u);
    CHECK(service.counters.calls_readdir > 0u);
    CHECK(service.counters.calls_closedir > 0u);
    CHECK(service.counters.refusals_stale_handle >= 4u);
}

/* open / read / write / lseek / fstat / close over the fake port. */
static void test_file_operations(void)
{
    fs_service service;
    fs_storage_port port;
    fake_port fake;
    uint8_t buffer[64];
    uint8_t stat_bytes[52];
    uint32_t fd = 0xFFFFFFFFu;
    uint32_t other = 0;
    uint64_t count = 0;
    unsigned index;

    memset(&fake, 0, sizeof fake);
    port = fake_make_port(&fake);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        (void)fs_mount_add(&service, fs_canonical_guest_mount(index), PRODUCTION_ROOT);
    fake.entry.present = 1;
    fake.entry.is_directory = 0;

    /* ---- fs_open argument and path refusals, none of which reach the port ---- */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, NULL),
                 FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), 000004u, &fd),
                 FS_ERROR_EINVAL);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"),
                         FS_O_RDWR | FS_O_WRONLY, &fd),
                 FS_ERROR_EINVAL);
    CHECK_EQ_INT(fs_open(&service, "relative", LEN("relative"), FS_O_RDONLY, &fd),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0", LEN("/dev_hdd0"), FS_O_RDONLY, &fd),
                 FS_ERROR_EISDIR);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/", LEN("/dev_hdd0/"), FS_O_RDONLY, &fd),
                 FS_ERROR_EISDIR);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/x/", LEN("/dev_hdd0/x/"), FS_O_RDONLY, &fd),
                 FS_ERROR_EISDIR);
    CHECK_EQ_INT(fake.open_file_calls, 0u);
    CHECK_EQ_INT(fd, 0xFFFFFFFFu);

    /* ---- every storage status maps to the guest code fs_open promises ---- */
    {
        static const fs_storage_status statuses[] = {
            FS_STORAGE_NOT_FOUND, FS_STORAGE_IS_A_DIRECTORY, FS_STORAGE_ALREADY_EXISTS,
            FS_STORAGE_ACCESS_DENIED, FS_STORAGE_NOT_A_DIRECTORY, FS_STORAGE_NAME_TOO_LONG,
            FS_STORAGE_NOT_SUPPORTED, FS_STORAGE_IO_ERROR,
        };
        static const int32_t codes[] = {
            FS_ERROR_ENOENT, FS_ERROR_EISDIR, FS_ERROR_EEXIST, FS_ERROR_EACCES,
            FS_ERROR_ENOTDIR, FS_ERROR_ENAMETOOLONG, FS_ERROR_ENOTSUP, FS_ERROR_EIO,
        };
        unsigned status_index;
        CHECK_EQ_INT(sizeof statuses / sizeof statuses[0], sizeof codes / sizeof codes[0]);
        for (status_index = 0; status_index < sizeof statuses / sizeof statuses[0]; ++status_index) {
            fake.next_open_status = statuses[status_index];
            fd = 0xFFFFFFFFu;
            CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY, &fd),
                         codes[status_index]);
            CHECK_EQ_INT(fd, 0xFFFFFFFFu);
        }
        fake.next_open_status = FS_STORAGE_OK;
    }

    /* ---- fd numbering: first is 3, the lowest free slot is reused after close ---- */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fd, FS_FILE_FD_BASE);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/b", LEN("/dev_hdd0/b"), FS_O_RDONLY, &other),
                 FS_OK);
    CHECK_EQ_INT(other, FS_FILE_FD_BASE + 1u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/c", LEN("/dev_hdd0/c"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fd, FS_FILE_FD_BASE);
    CHECK_EQ_INT(fs_close(&service, other), FS_OK);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_close(&service, fd), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, 0u), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, 2u), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, FS_FILE_FD_BASE + FS_FILE_CAPACITY), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_open_file_count(&service), 0u);

    /* ---- access mode and NULL-buffer refusals ---- */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_WRONLY, &fd), FS_OK);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 4u, &count), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, buffer, 4u, &count), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 4u, NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_read(&service, fd, NULL, 4u, &count), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_read(&service, fd, NULL, 0u, &count), FS_OK);
    CHECK_EQ_INT(count, 0u);
    CHECK_EQ_INT(fs_read(&service, 0u, buffer, 4u, &count), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDWR, &fd), FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, buffer, 4u, NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_write(&service, fd, NULL, 4u, &count), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_write(&service, 0u, buffer, 1u, &count), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* ---- EMFILE at the 62nd open, with no port call for the refused one ---- */
    {
        uint32_t fds[FS_FILE_CAPACITY];
        unsigned opened;
        const unsigned calls_before = fake.open_file_calls;
        for (opened = 0; opened < FS_FILE_CAPACITY; ++opened) {
            CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/f", LEN("/dev_hdd0/f"), FS_O_RDONLY,
                                 &fds[opened]),
                         FS_OK);
            CHECK_EQ_INT(fds[opened], FS_FILE_FD_BASE + opened);
        }
        CHECK_EQ_INT(fake.open_file_calls, calls_before + FS_FILE_CAPACITY);
        fd = 0xFFFFFFFFu;
        CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/f", LEN("/dev_hdd0/f"), FS_O_RDONLY, &fd),
                     FS_ERROR_EMFILE);
        CHECK_EQ_INT(fd, 0xFFFFFFFFu);
        CHECK_EQ_INT(fake.open_file_calls, calls_before + FS_FILE_CAPACITY);
        CHECK_EQ_INT(fs_open_file_count(&service), FS_FILE_CAPACITY);
        CHECK_EQ_INT(fs_close_all(&service), FS_FILE_CAPACITY);
        CHECK_EQ_INT(fs_open_file_count(&service), 0u);
        CHECK_EQ_INT(fake.live_files, 0);
    }

    /* ---- position bookkeeping across read and lseek ---- */
    fake.file_size = 10u;
    memcpy(fake.file_data, "abcdefghij", 10u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 4u, &count), FS_OK);
    CHECK_EQ_INT(count, 4u);
    CHECK_EQ_MEM(buffer, "abcd", 4u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 20u, &count), FS_OK);
    CHECK_EQ_INT(count, 6u);
    CHECK_EQ_MEM(buffer, "efghij", 6u);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 5u, &count), FS_OK);
    CHECK_EQ_INT(count, 0u);
    CHECK_EQ_INT(fs_lseek(&service, fd, 2, FS_SEEK_SET, &count), FS_OK);
    CHECK_EQ_INT(count, 2u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 3u, &count), FS_OK);
    CHECK_EQ_INT(count, 3u);
    CHECK_EQ_MEM(buffer, "cde", 3u);
    CHECK_EQ_INT(fs_lseek(&service, fd, -1, FS_SEEK_CUR, &count), FS_OK);
    CHECK_EQ_INT(count, 4u);
    CHECK_EQ_INT(fs_lseek(&service, fd, -3, FS_SEEK_END, &count), FS_OK);
    CHECK_EQ_INT(count, 7u);
    CHECK_EQ_INT(fs_lseek(&service, fd, 100, FS_SEEK_SET, &count), FS_OK);
    CHECK_EQ_INT(count, 100u);
    count = 0xDEADBEEFCAFEF00DULL;
    CHECK_EQ_INT(fs_lseek(&service, fd, -1, FS_SEEK_SET, &count), FS_ERROR_EINVAL);
    CHECK_EQ_INT(count, 0xDEADBEEFCAFEF00DULL);
    CHECK_EQ_INT(fs_lseek(&service, fd, 0, 3u, &count), FS_ERROR_EINVAL);
    CHECK_EQ_INT(fs_lseek(&service, fd, INT64_MAX, FS_SEEK_CUR, &count), FS_ERROR_EINVAL);
    CHECK_EQ_INT(fs_lseek(&service, fd, 0, FS_SEEK_SET, NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_lseek(&service, 0u, 0, FS_SEEK_SET, &count), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* ---- positional write and append-at-end ---- */
    fake.file_size = 5u;
    memcpy(fake.file_data, "hello", 5u);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/w", LEN("/dev_hdd0/w"), FS_O_RDWR, &fd), FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"XYZ", 3u, &count), FS_OK);
    CHECK_EQ_INT(count, 3u);
    CHECK_EQ_INT(fs_lseek(&service, fd, 1, FS_SEEK_SET, &count), FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"AB", 2u, &count), FS_OK);
    CHECK_EQ_INT(count, 2u);
    CHECK_EQ_INT(fake.file_size, 5u);
    CHECK_EQ_MEM(fake.file_data, "XABlo", 5u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/w", LEN("/dev_hdd0/w"),
                         FS_O_WRONLY | FS_O_APPEND, &fd),
                 FS_OK);
    CHECK_EQ_INT(fake.opened_access, FS_O_WRONLY);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"!", 1u, &count), FS_OK);
    CHECK_EQ_INT(count, 1u);
    CHECK_EQ_INT(fake.file_size, 6u);
    CHECK_EQ_MEM(fake.file_data, "XABlo!", 6u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* ---- fstat reads the opened path through the port's stat ---- */
    fake.entry.size = 42u;
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, &fd), FS_OK);
    memset(stat_bytes, 0xA5, sizeof stat_bytes);
    CHECK_EQ_INT(fs_fstat(&service, fd, stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 42);
    CHECK_EQ_INT(fs_fstat(&service, fd, NULL), FS_ERROR_EFAULT);
    CHECK_EQ_INT(fs_fstat(&service, 0u, stat_bytes), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* ---- fs_close_all closes directories and files together ---- */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/tree", LEN("/dev_hdd0/tree"), &other), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fs_close_all(&service), 2u);
    CHECK_EQ_INT(fs_open_descriptor_count(&service), 0u);
    CHECK_EQ_INT(fs_open_file_count(&service), 0u);
    CHECK_EQ_INT(fake.live_files, 0);
    CHECK_EQ_INT(fake.live_handles, 0);

    /* ---- a missing port function is ENOTSUP, not a silent success ---- */
    {
        fs_storage_port empty;
        fs_service bare;
        memset(&empty, 0, sizeof empty);
        fs_service_init(&bare, &empty);
        (void)fs_mount_add(&bare, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&bare, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY, &fd),
                     FS_ERROR_ENOTSUP);
    }
    {
        fake_port partial_fake;
        fs_storage_port partial_port;
        fs_service partial_service;
        memset(&partial_fake, 0, sizeof partial_fake);
        partial_fake.entry.present = 1;
        partial_port = fake_make_port(&partial_fake);
        partial_port.read_file = NULL;
        fs_service_init(&partial_service, &partial_port);
        (void)fs_mount_add(&partial_service, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&partial_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY,
                             &fd),
                     FS_OK);
        CHECK_EQ_INT(fs_read(&partial_service, fd, buffer, 1u, &count), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_close(&partial_service, fd), FS_OK);
    }
    {
        fake_port partial_fake;
        fs_storage_port partial_port;
        fs_service partial_service;
        memset(&partial_fake, 0, sizeof partial_fake);
        partial_fake.entry.present = 1;
        partial_port = fake_make_port(&partial_fake);
        partial_port.write_file = NULL;
        fs_service_init(&partial_service, &partial_port);
        (void)fs_mount_add(&partial_service, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&partial_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_WRONLY,
                             &fd),
                     FS_OK);
        CHECK_EQ_INT(fs_write(&partial_service, fd, buffer, 1u, &count), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_close(&partial_service, fd), FS_OK);
    }
    {
        fake_port partial_fake;
        fs_storage_port partial_port;
        fs_service partial_service;
        memset(&partial_fake, 0, sizeof partial_fake);
        partial_fake.entry.present = 1;
        partial_port = fake_make_port(&partial_fake);
        partial_port.file_size = NULL;
        fs_service_init(&partial_service, &partial_port);
        (void)fs_mount_add(&partial_service, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&partial_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY,
                             &fd),
                     FS_OK);
        CHECK_EQ_INT(fs_lseek(&partial_service, fd, 0, FS_SEEK_END, &count), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_close(&partial_service, fd), FS_OK);
    }
    {
        fake_port partial_fake;
        fs_storage_port partial_port;
        fs_service partial_service;
        memset(&partial_fake, 0, sizeof partial_fake);
        partial_fake.entry.present = 1;
        partial_port = fake_make_port(&partial_fake);
        partial_port.stat = NULL;
        fs_service_init(&partial_service, &partial_port);
        (void)fs_mount_add(&partial_service, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&partial_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY,
                             &fd),
                     FS_OK);
        CHECK_EQ_INT(fs_fstat(&partial_service, fd, stat_bytes), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_close(&partial_service, fd), FS_OK);
    }
    {
        fake_port partial_fake;
        fs_storage_port partial_port;
        fs_service partial_service;
        memset(&partial_fake, 0, sizeof partial_fake);
        partial_fake.entry.present = 1;
        partial_port = fake_make_port(&partial_fake);
        partial_port.close_file = NULL;
        fs_service_init(&partial_service, &partial_port);
        (void)fs_mount_add(&partial_service, "/dev_hdd0", PRODUCTION_ROOT);
        CHECK_EQ_INT(fs_open(&partial_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), FS_O_RDONLY,
                             &fd),
                     FS_OK);
        CHECK_EQ_INT(fs_close(&partial_service, fd), FS_ERROR_ENOTSUP);
        CHECK_EQ_INT(fs_open_file_count(&partial_service), 0u);
    }

    /* ---- the file counters exist for the integrator's report ---- */
    CHECK(service.counters.calls_open > 0u);
    CHECK(service.counters.calls_read > 0u);
    CHECK(service.counters.calls_write > 0u);
    CHECK(service.counters.calls_lseek > 0u);
    CHECK(service.counters.calls_fstat > 0u);
    CHECK(service.counters.calls_close > 0u);
    CHECK(service.counters.bytes_read > 0u);
    CHECK(service.counters.bytes_written > 0u);
}

/* The /dev_bdvd-only ASCII case fallback, over a fake tree with upper-case names. */
static void test_case_insensitive_lookup(void)
{
    fake_port fake;
    fs_storage_port port;
    fs_service service;
    uint8_t stat_bytes[52];
    uint8_t dirent[FS_CELLFS_DIRENT_BYTES];
    uint8_t nread[FS_CELLFS_NREAD_BYTES];
    uint32_t fd = 0, handle = 0;
    unsigned index, open_before, names = 0;
    uint64_t fallbacks;

    memset(&fake, 0, sizeof fake);
    fake.tree_mode = 1;
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd", 1, 0u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME", 1, 0u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR", 1, 0u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR/CACHE", 1, 0u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR/CACHE/A.BIN", 0, 77u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR/CACHE/Data.bin", 0, 1u);
    fake_tree_add(&fake, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR/CACHE/DATA.BIN", 0, 2u);
    port = fake_make_port(&fake);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), PRODUCTION_ROOT), 0);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/ps3_game/usrdir/cache/a.bin",
                         LEN("/dev_bdvd/ps3_game/usrdir/cache/a.bin"), stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 77);
    CHECK_EQ_INT(service.counters.case_fallbacks, 1u);
    CHECK_EQ_INT(fake.open_calls, open_before + 4u);
    CHECK_EQ_INT(fake.close_calls, fake.open_calls);
    CHECK_EQ_INT(fake.live_handles, 0);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/PS3_GAME/USRDIR/CACHE/A.BIN",
                         LEN("/dev_bdvd/PS3_GAME/USRDIR/CACHE/A.BIN"), stat_bytes), FS_OK);
    CHECK_EQ_INT(fake.open_calls, open_before);
    CHECK_EQ_INT(service.counters.case_fallbacks, 1u);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/ps3_game/usrdir/cache/a.bin",
                         LEN("/dev_hdd0/ps3_game/usrdir/cache/a.bin"), stat_bytes),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fake.open_calls, open_before);
    CHECK_EQ_INT(service.counters.case_fallbacks, 1u);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_open(&service, "/dev_bdvd/ps3_game/usrdir/cache/a.bin",
                         LEN("/dev_bdvd/ps3_game/usrdir/cache/a.bin"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(fake.open_calls, open_before + 4u);
    CHECK_EQ_INT(service.counters.case_fallbacks, 2u);
    CHECK_EQ_INT(fs_fstat(&service, fd, stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 77);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/bdvd/PS3_GAME/USRDIR/CACHE/A.BIN") == 0);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/PS3_GAME/USRDIR/CACHE/data.bin",
                         LEN("/dev_bdvd/PS3_GAME/USRDIR/CACHE/data.bin"), stat_bytes),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(service.counters.case_ambiguous, 1u);
    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/PS3_GAME/USRDIR/CACHE/DATA.BIN",
                         LEN("/dev_bdvd/PS3_GAME/USRDIR/CACHE/DATA.BIN"), stat_bytes), FS_OK);
    CHECK_EQ_INT(service.counters.case_ambiguous, 1u);

    open_before = fake.open_calls;
    fallbacks = service.counters.case_fallbacks;
    CHECK_EQ_INT(fs_open(&service, "/dev_bdvd/ps3_game/usrdir/cache/new.bin",
                         LEN("/dev_bdvd/ps3_game/usrdir/cache/new.bin"),
                         FS_O_WRONLY | FS_O_CREAT, &fd), FS_OK);
    CHECK_EQ_INT(fake.open_calls, open_before);
    CHECK_EQ_INT(service.counters.case_fallbacks, fallbacks);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_unlink(&service, "/dev_bdvd/ps3_game/usrdir/cache/a.bin",
                           LEN("/dev_bdvd/ps3_game/usrdir/cache/a.bin")), FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_bdvd/ps3_game/usrdir/cache",
                          LEN("/dev_bdvd/ps3_game/usrdir/cache")), FS_ERROR_ENOENT);
    CHECK_EQ_INT(fake.open_calls, open_before);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/PS3_GAME/usrdir/CACHE/A.BIN",
                         LEN("/dev_bdvd/PS3_GAME/usrdir/CACHE/A.BIN"), stat_bytes), FS_OK);
    CHECK_EQ_INT(fake.open_calls, open_before + 1u);

    open_before = fake.open_calls;
    CHECK_EQ_INT(fs_opendir(&service, "/dev_bdvd/ps3_game/usrdir/cache",
                            LEN("/dev_bdvd/ps3_game/usrdir/cache"), &handle), FS_OK);
    CHECK_EQ_INT(fake.open_calls, open_before + 5u);
    for (;;) {
        uint64_t count = 0;
        unsigned byte;
        CHECK_EQ_INT(fs_readdir(&service, handle, dirent, nread), FS_OK);
        for (byte = 0; byte < FS_CELLFS_NREAD_BYTES; ++byte)
            count = (count << 8) | nread[byte];
        if (count == 0u) break;
        ++names;
    }
    CHECK_EQ_INT(names, 3u);
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_OK);
    CHECK_EQ_INT(fake.live_handles, 0);
}

/* mkdir / rename / fsync / utime over the fake port: the guest-code mapping, the refusals
 * that must never reach the port, and the guest fds that survive a rename. */
static void test_create_rename_sync_utime(void)
{
    fs_service service;
    fs_storage_port port;
    fs_storage_port null_port;
    fake_port fake;
    uint8_t buffer[32];
    uint32_t fd = 0;
    uint32_t handle = 0;
    uint64_t count = 0;
    unsigned index;

    memset(&fake, 0, sizeof fake);
    port = fake_make_port(&fake);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        (void)fs_mount_add(&service, fs_canonical_guest_mount(index), PRODUCTION_ROOT);

    /* ---- mkdir ---- */
    fake.entry.present = 1;
    fake.entry.is_directory = 1;
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/newdir", LEN("/dev_hdd0/newdir"), 0777u), FS_OK);
    CHECK_EQ_INT(fake.make_dir_calls, 1u);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/newdir") == 0);
    CHECK_EQ_INT(service.counters.calls_mkdir, 1u);
    CHECK_EQ_INT(service.counters.creates_dir_ok, 1u);
    /* a trailing '/' names a directory too */
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/newdir/", LEN("/dev_hdd0/newdir/"), 0755u),
                 FS_OK);
    CHECK_EQ_INT(fake.make_dir_calls, 2u);
    /* the mount root is already there, and the guest is told so */
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0", LEN("/dev_hdd0"), 0777u), FS_ERROR_EEXIST);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/", LEN("/dev_hdd0/"), 0777u), FS_ERROR_EEXIST);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/..", LEN("/dev_hdd0/.."), 0777u),
                 FS_ERROR_EEXIST);
    CHECK_EQ_INT(fake.make_dir_calls, 2u);
    /* the disc is read-only for the guest: refused before the port is reached */
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_bdvd/PS3_GAME/x", LEN("/dev_bdvd/PS3_GAME/x"),
                          0777u),
                 FS_ERROR_EROFS);
    CHECK_EQ_INT(fake.make_dir_calls, 2u);
    /* the port's statuses map onto the lv2 ones; a resolution failure never reaches it */
    CHECK_EQ_INT(fs_mkdir(&service, "relative/dir", LEN("relative/dir"), 0777u),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fake.make_dir_calls, 2u);

    /* ---- rename ---- */
    fake_tree_add(&fake, PRODUCTION_ROOT "/hdd0/old.txt", 0, 5u);
    fake.tree_mode = 1;
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/old.txt", LEN("/dev_hdd0/old.txt"),
                           "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt")),
                 FS_OK);
    CHECK_EQ_INT(fake.rename_calls, 1u);
    CHECK_EQ_INT(service.counters.calls_rename, 1u);
    CHECK_EQ_INT(service.counters.renames_ok, 1u);
    CHECK(strcmp(fake.last_path, PRODUCTION_ROOT "/hdd0/new.txt") == 0);
    /* a mount root is never movable and never a target */
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0", LEN("/dev_hdd0"),
                           "/dev_hdd0/moved", LEN("/dev_hdd0/moved")),
                 FS_ERROR_EACCES);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                           "/dev_hdd0/..", LEN("/dev_hdd0/..")),
                 FS_ERROR_EACCES);
    /* two mounts are a cross-device rename */
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                           "/dev_hdd1/new.txt", LEN("/dev_hdd1/new.txt")),
                 FS_ERROR_EINVAL);
    /* the disc is read-only on either side */
    CHECK_EQ_INT(fs_rename(&service, "/dev_bdvd/a", LEN("/dev_bdvd/a"),
                           "/dev_bdvd/b", LEN("/dev_bdvd/b")),
                 FS_ERROR_EROFS);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                           "/dev_bdvd/new.txt", LEN("/dev_bdvd/new.txt")),
                 FS_ERROR_EROFS);
    CHECK_EQ_INT(fake.rename_calls, 1u);
    /* an unconfigured mount on the destination is a resolution failure */
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                           "/dev_whatever/x", LEN("/dev_whatever/x")),
                 FS_ERROR_ENOENT);
    fake.rename_status = FS_STORAGE_NOT_FOUND;
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/gone.txt", LEN("/dev_hdd0/gone.txt"),
                           "/dev_hdd0/x.txt", LEN("/dev_hdd0/x.txt")),
                 FS_ERROR_ENOENT);
    fake.rename_status = FS_STORAGE_ALREADY_EXISTS;
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                           "/dev_hdd0/dir", LEN("/dev_hdd0/dir")),
                 FS_ERROR_EEXIST);
    fake.rename_status = FS_STORAGE_OK;
    CHECK_EQ_INT(service.counters.renames_ok, 1u);
    fake.tree_mode = 0;

    /* ---- fsync ---- */
    memset(&fake.entry, 0, sizeof fake.entry); /* no such file yet: the create makes it */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/fsync.txt", LEN("/dev_hdd0/fsync.txt"),
                         FS_O_WRONLY | FS_O_CREAT, &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"flushed", 7u, &count), FS_OK);
    CHECK_EQ_INT(fs_fsync(&service, fd), FS_OK);
    CHECK_EQ_INT(fake.sync_calls, 1u);
    CHECK_EQ_INT(service.counters.calls_fsync, 1u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    /* a closed fd, and a directory handle, are both EBADF */
    CHECK_EQ_INT(fs_fsync(&service, fd), FS_ERROR_EBADF);
    fake.entry.present = 1;
    fake.entry.is_directory = 1;
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), &handle), FS_OK);
    CHECK_EQ_INT(fs_fsync(&service, handle), FS_ERROR_EBADF);
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_OK);
    CHECK_EQ_INT(fake.sync_calls, 1u);
    /* a read-only fd has nothing to flush: OK, and the port is not asked */
    fake.entry.is_directory = 0;
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/fsync.txt", LEN("/dev_hdd0/fsync.txt"),
                         FS_O_RDONLY, &fd),
                 FS_OK);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 7u, &count), FS_OK);
    CHECK_EQ_MEM(buffer, "flushed", 7u);
    CHECK_EQ_INT(fs_fsync(&service, fd), FS_OK);
    CHECK_EQ_INT(fake.sync_calls, 1u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* ---- utime ---- */
    fake.entry.present = 1;
    fake.entry.is_directory = 0;
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/fsync.txt", LEN("/dev_hdd0/fsync.txt"),
                          1000, 2000),
                 FS_OK);
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/dir", LEN("/dev_hdd0/dir"), 0, 0), FS_OK);
    CHECK_EQ_INT(service.counters.calls_utime, 2u);
    fake.entry.present = 0;
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/absent", LEN("/dev_hdd0/absent"), 0, 0),
                 FS_ERROR_ENOENT);
    fake.entry.present = 1;
    CHECK_EQ_INT(fs_utime(&service, "/dev_bdvd/EBOOT.BIN", LEN("/dev_bdvd/EBOOT.BIN"), 0, 0),
                 FS_ERROR_EROFS);
    CHECK_EQ_INT(fs_utime(&service, "relative", LEN("relative"), 0, 0), FS_ERROR_ENOENT);

    /* ---- a port without the new entries is ENOTSUP, not a pretend success ---- */
    memset(&null_port, 0, sizeof null_port);
    null_port.context = &fake;
    fs_service_init(&service, &null_port);
    (void)fs_mount_add(&service, "/dev_hdd0", PRODUCTION_ROOT);
    (void)fs_mount_add(&service, "/dev_bdvd", PRODUCTION_ROOT);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), 0777u),
                 FS_ERROR_ENOTSUP);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"),
                           "/dev_hdd0/y", LEN("/dev_hdd0/y")),
                 FS_ERROR_ENOTSUP);
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), 0, 0),
                 FS_ERROR_ENOTSUP);
}

/* ------------------------------------------------------------------------------------ */
/* 2. Adapter: a real scratch tree                                                       */
/* ------------------------------------------------------------------------------------ */

static char g_scratch[1024]; /* the build dir the verifier passed */
static char g_world[1024];   /* <scratch>/fs-service-world-XXXXXX */
static char g_root[1024];    /* <world>/root  — the mount root token */
static char g_outside[1024]; /* <world>/OUTSIDE — the sibling sentinel */

static int make_path(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    const int written = c ? snprintf(out, cap, "%s/%s/%s", a, b, c)
                          : (b ? snprintf(out, cap, "%s/%s", a, b) : snprintf(out, cap, "%s", a));
    return written > 0 && (size_t)written < cap;
}

static int write_file(const char *path, const char *text)
{
    FILE *stream = fopen(path, "wb");
    int ok;
    if (!stream) return 0;
    ok = fwrite(text, 1u, strlen(text), stream) == strlen(text);
    if (fclose(stream) != 0) ok = 0;
    return ok;
}

static int path_exists(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0;
}

/* Removes a tree the test created, bounded and only inside `g_world`. */
static void remove_tree(const char *path, unsigned depth)
{
    DIR *dir;
    struct dirent *entry;
    if (depth > 8u) return;
    dir = opendir(path);
    if (!dir) {
        (void)unlink(path);
        return;
    }
    while ((entry = readdir(dir)) != NULL) {
        char child[1200];
        struct stat info;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!make_path(child, sizeof child, path, entry->d_name, NULL)) continue;
        if (lstat(child, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) remove_tree(child, depth + 1u);
        else (void)unlink(child);
    }
    closedir(dir);
    (void)rmdir(path);
}

static void test_adapter_confinement_rules(void)
{
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a"), 1);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/b/c"), 1);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a.txt"), 1);
    CHECK_EQ_INT(fs_storage_posix_relative_ok(".."), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("../x"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/.."), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/../b"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("..hidden"), 1); /* a name, not a traversal */
    CHECK_EQ_INT(fs_storage_posix_relative_ok("."), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/."), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok(""), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("/a"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a//b"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a\\b"), 0);
    CHECK_EQ_INT(fs_storage_posix_relative_ok("sdmc:/x"), 0); /* a device component */
    CHECK_EQ_INT(fs_storage_posix_relative_ok("a/b:/c"), 0);
    {
        char long_name[300];
        memset(long_name, 'n', sizeof long_name);
        long_name[256] = 0; /* 256 bytes > the 255 limit */
        CHECK_EQ_INT(fs_storage_posix_relative_ok(long_name), 0);
        long_name[255] = 0;
        CHECK_EQ_INT(fs_storage_posix_relative_ok(long_name), 1);
    }
    CHECK_EQ_INT(fs_storage_posix_is_device_prefix("sdmc:"), 1);
    CHECK_EQ_INT(fs_storage_posix_is_device_prefix("sdmc:/x"), 0);
    CHECK_EQ_INT(fs_storage_posix_is_device_prefix(""), 0);
}

/* The adapter refuses a host path that is outside its root or that a guest could have made
 * dangerous, even though the domain never produces one. */
static void test_adapter_refuses_unconfined_paths(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_stat_info info;
    char path[1200];
    void *handle = NULL;

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);

    /* outside the root, absolute, and traversal spellings are all refused */
    CHECK_EQ_INT(port.stat(port.context, g_outside, &info), FS_STORAGE_ACCESS_DENIED);
    CHECK_EQ_INT(port.stat(port.context, "/etc/passwd", &info), FS_STORAGE_ACCESS_DENIED);
    CHECK_EQ_INT(port.stat(port.context, "", &info), FS_STORAGE_ACCESS_DENIED);
    CHECK_EQ_INT(port.stat(port.context, NULL, &info), FS_STORAGE_ACCESS_DENIED);
    (void)make_path(path, sizeof path, g_root, "..", "OUTSIDE/sentinel.txt");
    CHECK_EQ_INT(port.stat(port.context, path, &info), FS_STORAGE_ACCESS_DENIED);
    (void)make_path(path, sizeof path, g_root, ".", "..");
    CHECK_EQ_INT(port.stat(port.context, path, &info), FS_STORAGE_ACCESS_DENIED);
    (void)make_path(path, sizeof path, g_root, "hdd0//sneaky", NULL);
    CHECK_EQ_INT(port.stat(port.context, path, &info), FS_STORAGE_ACCESS_DENIED);

    /* a prefix that only LOOKS like the root ("<root>X/...") is refused */
    snprintf(path, sizeof path, "%sX/hdd0/sentinel.txt", g_root);
    CHECK_EQ_INT(port.stat(port.context, path, &info), FS_STORAGE_ACCESS_DENIED);

    (void)make_path(path, sizeof path, g_root, "..", NULL);
    CHECK_EQ_INT(port.open_dir(port.context, path, &handle), FS_STORAGE_ACCESS_DENIED);
    CHECK(handle == NULL);
    (void)make_path(path, sizeof path, g_root, "..", "OUTSIDE");
    CHECK_EQ_INT(port.remove_dir(port.context, path), FS_STORAGE_ACCESS_DENIED);
    CHECK_EQ_INT(port.unlink_file(port.context, g_outside), FS_STORAGE_ACCESS_DENIED);
    /* nothing outside the root moved */
    CHECK(path_exists(g_outside));
    (void)make_path(path, sizeof path, g_outside, "sentinel.txt", NULL);
    CHECK(path_exists(path));
    CHECK(storage.refused >= 10u);

    /* the root itself is rejected by both removal verbs */
    CHECK_EQ_INT(port.remove_dir(port.context, g_root), FS_STORAGE_ACCESS_DENIED);
    CHECK_EQ_INT(port.unlink_file(port.context, g_root), FS_STORAGE_ACCESS_DENIED);

    /* a symlink pointing outside the root is not followed and not removed */
    {
        char link[1200], target[1200];
        (void)make_path(link, sizeof link, g_root, "hdd0/escape", NULL);
        (void)make_path(target, sizeof target, g_outside, "sentinel.txt", NULL);
        if (symlink(target, link) == 0) {
            CHECK_EQ_INT(port.stat(port.context, link, &info), FS_STORAGE_ACCESS_DENIED);
            CHECK_EQ_INT(port.unlink_file(port.context, link), FS_STORAGE_ACCESS_DENIED);
            CHECK(path_exists(link));
            CHECK(path_exists(target));
            (void)unlink(link);
        }
    }
}

/* The guest walker of CALLS.md §3a, reduced to what it does to the service: opendir, readdir
 * until nread == 0, collect files and directories, recurse, then unlink the files and rmdir
 * the subdirectories. The depth is bounded so a confinement bug cannot spin forever. */
typedef struct walk_stats {
    unsigned directories_visited;
    unsigned files_unlinked;
    unsigned dirs_removed;
    unsigned readdir_calls;
    unsigned files_seen;
    unsigned dirs_seen;
} walk_stats;

static void walk_and_purge(fs_service *service, const char *guest_path, unsigned depth,
                           walk_stats *stats)
{
    uint32_t handle = 0;
    uint8_t dirent[FS_CELLFS_DIRENT_BYTES];
    uint8_t nread[FS_CELLFS_NREAD_BYTES];
    char files[64][256];
    char dirs[64][256];
    unsigned file_count = 0, dir_count = 0, i;

    if (depth > 6u) return;
    if (fs_opendir(service, guest_path, strlen(guest_path), &handle) != FS_OK) return;
    ++stats->directories_visited;
    if (handle == 0u) return;

    for (;;) {
        uint64_t count = 0;
        char child[256];
        if (fs_readdir(service, handle, dirent, nread) != FS_OK) break;
        ++stats->readdir_calls;
        for (i = 0; i < FS_CELLFS_NREAD_BYTES; ++i) count = (count << 8) | nread[i];
        if (count == 0u) break; /* the only end-of-directory signal */
        memset(child, 0, sizeof child);
        if (!make_path(child, sizeof child, guest_path, (const char *)dirent + 2, NULL)) break;
        if (dirent[0] == FS_CELLFS_DTYPE_DIR) {
            ++stats->dirs_seen;
            if (dir_count < 64u) memcpy(dirs[dir_count++], child, sizeof dirs[0]);
        } else if (dirent[0] == FS_CELLFS_DTYPE_FILE) {
            ++stats->files_seen;
            if (file_count < 64u) memcpy(files[file_count++], child, sizeof files[0]);
        }
    }
    (void)fs_closedir(service, handle);

    for (i = 0; i < file_count; ++i)
        if (fs_unlink(service, files[i], strlen(files[i])) == FS_OK) ++stats->files_unlinked;
    for (i = 0; i < dir_count; ++i) {
        walk_and_purge(service, dirs[i], depth + 1u, stats);
        if (fs_rmdir(service, dirs[i], strlen(dirs[i])) == FS_OK) ++stats->dirs_removed;
    }
}

/* The whole service over the real tree: a 3-level walk, nread == 0 exactly at the end, the
 * stat record from a real file, the recursive delete inside the root, and the sentinel
 * outside it untouched. */
static void test_adapter_end_to_end(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_service service;
    unsigned index;
    char path[1200];
    char parent[1200];
    uint8_t stat_bytes[52];
    uint8_t dirent[FS_CELLFS_DIRENT_BYTES];
    uint8_t nread[FS_CELLFS_NREAD_BYTES];
    uint32_t handle = 0;
    walk_stats stats;

    /* The tree: <root>/hdd0/a/b/c/file3.txt, <root>/hdd0/a/b/file2.txt,
     *           <root>/hdd0/a/root.txt, <root>/bdvd/PS3_GAME/USRDIR/EBOOT.BIN */
    (void)make_path(path, sizeof path, g_root, "hdd0/a", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/a/b", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/a/b/c", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/a/b/c/file3.txt", NULL);
    CHECK_EQ_INT(write_file(path, "level 3\n"), 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/a/b/file2.txt", NULL);
    CHECK_EQ_INT(write_file(path, "level 2\n"), 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/a/root.txt", NULL);
    CHECK_EQ_INT(write_file(path, "level 1\n"), 1);
    (void)make_path(parent, sizeof parent, g_root, "bdvd/PS3_GAME", NULL);
    CHECK_EQ_INT(mkdir(parent, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/USRDIR", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/USRDIR/EBOOT.BIN", NULL);
    CHECK_EQ_INT(write_file(path, "not a real ELF"), 1);

    /* the sentinel, deliberately OUTSIDE the mount root but inside the scratch world */
    (void)make_path(path, sizeof path, g_outside, "sentinel.txt", NULL);
    CHECK_EQ_INT(write_file(path, "must survive\n"), 1);

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), g_root), 0);

    /* the host token is the scratch root, not the production one */
    {
        fs_resolved_path resolved;
        CHECK_EQ_INT(fs_resolve_path(&service, "/dev_hdd0/a/b", LEN("/dev_hdd0/a/b"), &resolved),
                     FS_OK);
        (void)make_path(path, sizeof path, g_root, "hdd0/a/b", NULL);
        CHECK(strcmp(resolved.host, path) == 0);
    }

    /* ---- stat over a real file and a real directory ---- */
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a/root.txt", LEN("/dev_hdd0/a/root.txt"),
                         stat_bytes),
                 FS_OK);
    CHECK_EQ_INT(stat_bytes[0], 0x00);
    CHECK_EQ_INT(stat_bytes[2], 0x81); /* IFREG 0100000 */
    CHECK_EQ_INT(stat_bytes[0x2B], 8); /* "level 1\n" is 8 bytes */
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a/b", LEN("/dev_hdd0/a/b"), stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[2], 0x41); /* IFDIR 0040000 */
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a/missing", LEN("/dev_hdd0/a/missing"), stat_bytes),
                 FS_ERROR_ENOENT);
    /* a file named where a directory is required */
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a/root.txt/", LEN("/dev_hdd0/a/root.txt/"),
                         stat_bytes),
                 FS_ERROR_ENOTDIR);
    /* opendir on a file is ENOTDIR */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/a/root.txt", LEN("/dev_hdd0/a/root.txt"),
                            &handle),
                 FS_ERROR_ENOTDIR);
    /* unlink on a directory is EISDIR */
    CHECK_EQ_INT(fs_unlink(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a")), FS_ERROR_EISDIR);
    /* rmdir on a non-empty directory is refused, and the directory survives */
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a")), FS_ERROR_ENOTSUP);
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), stat_bytes), FS_OK);

    /* ---- the 3-level tree, in the walker's order, nread == 0 exactly at the end ---- */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), &handle), FS_OK);
    {
        unsigned names_seen = 0, end_seen = 0, guard = 0;
        while (guard++ < 32u) {
            uint64_t count = 0;
            unsigned byte;
            /* poison both buffers so "at end of directory the record is untouched" is a real
             * assertion and not an accident of the previous iteration */
            memset(dirent, 0xA5, sizeof dirent);
            memset(nread, 0xA5, sizeof nread);
            CHECK_EQ_INT(fs_readdir(&service, handle, dirent, nread), FS_OK);
            for (byte = 0; byte < FS_CELLFS_NREAD_BYTES; ++byte) count = (count << 8) | nread[byte];
            if (count == 0u) {
                ++end_seen;
                if (end_seen == 1u) CHECK_EQ_INT(dirent[0], 0xA5); /* untouched at end */
                if (end_seen >= 2u) break; /* nread stays 0 however often it is asked */
                continue;
            }
            CHECK_EQ_INT(count, 1u);
            ++names_seen;
            CHECK(dirent[0] == FS_CELLFS_DTYPE_DIR || dirent[0] == FS_CELLFS_DTYPE_FILE);
            CHECK_EQ_INT(dirent[2u + dirent[1]], 0); /* always NUL-terminated */
            CHECK(dirent[1] > 0u);
            CHECK(dirent[1] <= FS_DIRENT_NAME_MAX);
            /* "." and ".." are never published */
            CHECK(!(dirent[1] == 1u && dirent[2] == '.'));
            CHECK(!(dirent[1] == 2u && dirent[2] == '.' && dirent[3] == '.'));
        }
        CHECK_EQ_INT(names_seen, 2u); /* "b" and "root.txt" */
        CHECK(end_seen >= 1u);
    }
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_OK);
    CHECK_EQ_INT(fs_closedir(&service, handle), FS_ERROR_EBADF);

    /* ---- the recursive delete, confined ---- */
    memset(&stats, 0, sizeof stats);
    walk_and_purge(&service, "/dev_hdd0/a", 0u, &stats);
    CHECK_EQ_INT(stats.files_seen, 3u);   /* root.txt, b/file2.txt, b/c/file3.txt */
    CHECK_EQ_INT(stats.dirs_seen, 2u);    /* b (seen in a) and c (seen in b) */
    CHECK_EQ_INT(stats.files_unlinked, 3u);
    CHECK_EQ_INT(stats.dirs_removed, 2u); /* b/c, then b */
    CHECK_EQ_INT(stats.directories_visited, 3u); /* a, a/b, a/b/c */
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a")), FS_OK);
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/a", LEN("/dev_hdd0/a"), stat_bytes),
                 FS_ERROR_ENOENT);
    (void)make_path(path, sizeof path, g_root, "hdd0/a", NULL);
    CHECK_EQ_INT(path_exists(path), 0); /* nothing left behind */

    /* the sentinel outside the root survives every delete above */
    (void)make_path(path, sizeof path, g_outside, "sentinel.txt", NULL);
    CHECK_EQ_INT(path_exists(path), 1);

    /* the other mount is untouched */
    CHECK_EQ_INT(fs_stat(&service, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN",
                         LEN("/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN"), stat_bytes),
                 FS_OK);
    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/USRDIR/EBOOT.BIN", NULL);
    CHECK_EQ_INT(path_exists(path), 1);

    /* a delete aimed at the sibling sentinel lands inside the root instead */
    CHECK_EQ_INT(fs_unlink(&service, "/dev_hdd0/../OUTSIDE/sentinel.txt",
                           LEN("/dev_hdd0/../OUTSIDE/sentinel.txt")),
                 FS_ERROR_ENOENT);
    (void)make_path(path, sizeof path, g_outside, "sentinel.txt", NULL);
    CHECK_EQ_INT(path_exists(path), 1);
    CHECK_EQ_INT(fs_rmdir(&service, "/dev_hdd0/..", LEN("/dev_hdd0/..")), FS_ERROR_EACCES);

    /* ---- the counters the integrator reports ---- */
    CHECK_EQ_INT(service.counters.deletes_file_ok, 3u);
    CHECK_EQ_INT(service.counters.deletes_dir_ok, 3u);
    CHECK(service.counters.calls_stat > 0u);
    CHECK(service.counters.calls_readdir > 0u);
    CHECK(service.counters.refusals_stale_handle >= 1u);
    CHECK_EQ_INT(fs_open_descriptor_count(&service), 0u);
    /* unlink calls on this adapter: the 3 real file deletes of the walk, the refused
     * directory probe (EISDIR) before it, and the refused "../OUTSIDE/sentinel.txt" attempt
     * (which resolves inside the root and finds nothing). rmdir calls: the refused non-empty
     * probe, the 2 subdirectory removals of the walk, and the final removal of a itself. */
    CHECK_EQ_INT(storage.calls_unlink_file, 5u);
    CHECK_EQ_INT(storage.calls_remove_dir, 4u);

    /* a root that does not exist behaves as an empty tree rather than failing init */
    {
        fs_storage_posix empty_storage;
        fs_storage_port empty_port;
        fs_service empty_service;
        char missing[1200];
        (void)make_path(missing, sizeof missing, g_scratch, "fs-service-no-such-root", NULL);
        CHECK_EQ_INT(fs_storage_posix_init(&empty_storage, missing), 0);
        empty_port = fs_storage_posix_port(&empty_storage);
        fs_service_init(&empty_service, &empty_port);
        CHECK_EQ_INT(fs_mount_add(&empty_service, "/dev_hdd0", missing), 0);
        CHECK_EQ_INT(fs_stat(&empty_service, "/dev_hdd0/x", LEN("/dev_hdd0/x"), stat_bytes),
                     FS_ERROR_ENOENT);
        CHECK_EQ_INT(fs_rmdir(&empty_service, "/dev_hdd0/x", LEN("/dev_hdd0/x")), FS_ERROR_ENOENT);
    }
}

/* The service over a real file: open/read/write/lseek/fstat/close, the create/exclusive/
 * truncate flags, and the confinements that keep every handle inside the mount root. */
static void test_adapter_file_io(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_service service;
    unsigned index;
    char path[1200];
    char target[1200];
    uint8_t buffer[64];
    uint8_t stat_bytes[52];
    uint32_t fd = 0;
    uint64_t count = 0;
    void *handle = NULL;

    (void)make_path(path, sizeof path, g_root, "hdd0/io.txt", NULL);
    CHECK_EQ_INT(write_file(path, "0123456789abcdef"), 1);

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), g_root), 0);

    /* read in two pieces, then at end of file */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/io.txt", LEN("/dev_hdd0/io.txt"), FS_O_RDONLY,
                         &fd),
                 FS_OK);
    CHECK_EQ_INT(fd, FS_FILE_FD_BASE);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 5u, &count), FS_OK);
    CHECK_EQ_INT(count, 5u);
    CHECK_EQ_MEM(buffer, "01234", 5u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 11u, &count), FS_OK);
    CHECK_EQ_INT(count, 11u);
    CHECK_EQ_MEM(buffer, "56789abcdef", 11u);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 32u, &count), FS_OK);
    CHECK_EQ_INT(count, 0u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* SET / CUR / END seeks over the same real file */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/io.txt", LEN("/dev_hdd0/io.txt"), FS_O_RDONLY,
                         &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_lseek(&service, fd, 3, FS_SEEK_SET, &count), FS_OK);
    CHECK_EQ_INT(count, 3u);
    CHECK_EQ_INT(fs_lseek(&service, fd, 2, FS_SEEK_CUR, &count), FS_OK);
    CHECK_EQ_INT(count, 5u);
    CHECK_EQ_INT(fs_lseek(&service, fd, -2, FS_SEEK_END, &count), FS_OK);
    CHECK_EQ_INT(count, 14u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 8u, &count), FS_OK);
    CHECK_EQ_INT(count, 2u);
    CHECK_EQ_MEM(buffer, "ef", 2u);
    CHECK_EQ_INT(fs_fstat(&service, fd, stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 16);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* open refusals: missing, directory, create+exclusive on an existing file */
    fd = 0xFFFFFFFFu;
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/missing.txt", LEN("/dev_hdd0/missing.txt"),
                         FS_O_RDONLY, &fd),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fd, 0xFFFFFFFFu);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0", LEN("/dev_hdd0"), FS_O_RDONLY, &fd),
                 FS_ERROR_EISDIR);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/io.txt", LEN("/dev_hdd0/io.txt"),
                         FS_O_WRONLY | FS_O_CREAT | FS_O_EXCL, &fd),
                 FS_ERROR_EEXIST);

    /* create, write, close, re-open and read back */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"),
                         FS_O_WRONLY | FS_O_CREAT, &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"hello", 5u, &count), FS_OK);
    CHECK_EQ_INT(count, 5u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/new.txt", LEN("/dev_hdd0/new.txt"), FS_O_RDONLY,
                         &fd),
                 FS_OK);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, 5u);
    CHECK_EQ_MEM(buffer, "hello", 5u);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, 1u, &count), FS_OK);
    CHECK_EQ_INT(count, 0u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* O_TRUNC empties an existing file */
    CHECK_EQ_INT(fs_open(&service, "/dev/hdd0/io.txt", LEN("/dev/hdd0/io.txt"), FS_O_RDONLY,
                         &fd),
                 FS_ERROR_ENOENT); /* no such mount, not a truncation */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/io.txt", LEN("/dev_hdd0/io.txt"),
                         FS_O_WRONLY | FS_O_TRUNC, &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_fstat(&service, fd, stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 0);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/io.txt", LEN("/dev_hdd0/io.txt"), FS_O_RDONLY,
                         &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, 0u);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* a symlink component is refused, and an escape resolves inside the root */
    (void)make_path(path, sizeof path, g_root, "hdd0/link.txt", NULL);
    (void)make_path(target, sizeof target, g_outside, "sentinel.txt", NULL);
    if (symlink(target, path) == 0) {
        CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/link.txt", LEN("/dev_hdd0/link.txt"),
                             FS_O_RDONLY, &fd),
                     FS_ERROR_EACCES);
        CHECK_EQ_INT(port.open_file(port.context, path, FS_O_RDONLY, 0u, 0u, 0u, &handle),
                     FS_STORAGE_ACCESS_DENIED);
        CHECK(handle == NULL);
        CHECK(path_exists(path));
        (void)unlink(path);
    }
    (void)make_path(path, sizeof path, g_outside, "sentinel.txt", NULL);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/../../OUTSIDE/sentinel.txt",
                         LEN("/dev_hdd0/../../OUTSIDE/sentinel.txt"), FS_O_RDONLY, &fd),
                 /* Root (2026-11-11): the ".." components are clamped at the mount root, so this names
                  * <root>/OUTSIDE/sentinel.txt, which does not exist. It was EISDIR while a dot component
                  * anywhere made the path a directory; confinement is unchanged (the file outside the root
                  * is still there, next check). */
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(path_exists(path), 1);
    CHECK_EQ_INT(port.open_file(port.context, g_outside, FS_O_RDONLY, 0u, 0u, 0u, &handle),
                 FS_STORAGE_ACCESS_DENIED);
    CHECK(handle == NULL);

    CHECK_EQ_INT(fs_open_file_count(&service), 0u);
    CHECK(storage.calls_open_file > 0u);
    CHECK(storage.calls_read_file > 0u);
    CHECK(storage.calls_write_file > 0u);
    CHECK(storage.calls_file_size > 0u);
    CHECK(storage.calls_close_file > 0u);

    (void)make_path(path, sizeof path, g_root, "hdd0/io.txt", NULL);
    (void)unlink(path);
    (void)make_path(path, sizeof path, g_root, "hdd0/new.txt", NULL);
    (void)unlink(path);
}

/* The /dev_bdvd case fallback over the real POSIX adapter and the scratch world. */
static void test_adapter_case_insensitive(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_service service;
    unsigned index;
    char path[1200];
    uint8_t buffer[32];
    uint8_t stat_bytes[52];
    uint32_t fd = 0;
    uint64_t count = 0;

    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/USRDIR/CACHE", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);
    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/USRDIR/CACHE/X.BIN", NULL);
    CHECK_EQ_INT(write_file(path, "case-bytes"), 1);

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), g_root), 0);

    CHECK_EQ_INT(fs_open(&service, "/dev_bdvd/ps3_game/usrdir/cache/x.bin",
                         LEN("/dev_bdvd/ps3_game/usrdir/cache/x.bin"), FS_O_RDONLY, &fd), FS_OK);
    CHECK_EQ_INT(service.counters.case_fallbacks, 1u);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, 10u);
    CHECK_EQ_MEM(buffer, "case-bytes", 10u);
    CHECK_EQ_INT(fs_fstat(&service, fd, stat_bytes), FS_OK);
    CHECK_EQ_INT(stat_bytes[0x2B], 10);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
}

/* mkdir / rename / fsync / utime over the real POSIX adapter and the scratch world. */
static void test_adapter_create_rename_sync_utime(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_service service;
    unsigned index;
    char path[1200];
    char source[1200];
    char target[1200];
    char parent_file[1200];
    uint8_t buffer[32];
    uint8_t stat_bytes[52];
    uint32_t fd = 0;
    uint32_t reader = 0;
    uint64_t count = 0;

    (void)make_path(path, sizeof path, g_root, "hdd0/created", NULL);
    (void)make_path(parent_file, sizeof parent_file, g_root, "hdd0/not-a-dir.txt", NULL);
    CHECK_EQ_INT(write_file(parent_file, "a file\n"), 1);
    (void)make_path(source, sizeof source, g_root, "hdd0/rename-src.txt", NULL);
    CHECK_EQ_INT(write_file(source, "rename bytes\n"), 1);
    (void)make_path(target, sizeof target, g_root, "hdd0/rename-dst.txt", NULL);
    CHECK_EQ_INT(write_file(target, "replaced\n"), 1);
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-into-dir", NULL);
    CHECK_EQ_INT(mkdir(path, 0777) == 0 || errno == EEXIST, 1);

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), g_root), 0);

    /* ---- mkdir: one real directory, then the refusals ---- */
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/created", LEN("/dev_hdd0/created"), 0777u), FS_OK);
    CHECK_EQ_INT(fs_stat(&service, "/dev_hdd0/created", LEN("/dev_hdd0/created"), stat_bytes),
                 FS_OK);
    CHECK_EQ_INT(stat_bytes[2], 0x41); /* IFDIR 0040000 */
    (void)make_path(path, sizeof path, g_root, "hdd0/created", NULL);
    CHECK_EQ_INT(path_exists(path), 1);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/created", LEN("/dev_hdd0/created"), 0777u),
                 FS_ERROR_EEXIST);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/created/missing/deeper",
                          LEN("/dev_hdd0/created/missing/deeper"), 0777u),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0/not-a-dir.txt/child",
                          LEN("/dev_hdd0/not-a-dir.txt/child"), 0777u),
                 FS_ERROR_ENOTDIR);
    CHECK_EQ_INT(fs_mkdir(&service, "/dev_hdd0", LEN("/dev_hdd0"), 0777u), FS_ERROR_EEXIST);
    {
        const unsigned before = storage.calls_make_dir;
        CHECK_EQ_INT(fs_mkdir(&service, "/dev_bdvd/PS3_GAME/newdir",
                              LEN("/dev_bdvd/PS3_GAME/newdir"), 0777u),
                     FS_ERROR_EROFS);
        CHECK_EQ_INT(storage.calls_make_dir, before);
        (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/newdir", NULL);
        CHECK_EQ_INT(path_exists(path), 0);
    }

    /* ---- rename: content follows, and an fd opened first still reads it ---- */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/rename-src.txt", LEN("/dev_hdd0/rename-src.txt"),
                         FS_O_RDONLY, &fd),
                 FS_OK);
    (void)make_path(target, sizeof target, g_root, "hdd0/rename-dst.txt", NULL);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-src.txt", LEN("/dev_hdd0/rename-src.txt"),
                           "/dev_hdd0/rename-mid.txt", LEN("/dev_hdd0/rename-mid.txt")),
                 FS_OK);
    CHECK_EQ_INT(path_exists(source), 0);
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-mid.txt", NULL);
    CHECK_EQ_INT(path_exists(path), 1);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, LEN("rename bytes\n"));
    CHECK_EQ_MEM(buffer, "rename bytes\n", LEN("rename bytes\n"));
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* over an existing regular file: replaced, and the old content is gone */
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-dst.txt", NULL);
    CHECK_EQ_INT(write_file(path, "replaced\n"), 1);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-mid.txt", LEN("/dev_hdd0/rename-mid.txt"),
                           "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt")),
                 FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt"),
                         FS_O_RDONLY, &fd),
                 FS_OK);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_MEM(buffer, "rename bytes\n", LEN("rename bytes\n"));
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* a missing source, and a destination that is already a directory */
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/absent.txt", LEN("/dev_hdd0/absent.txt"),
                           "/dev_hdd0/elsewhere.txt", LEN("/dev_hdd0/elsewhere.txt")),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt"),
                           "/dev_hdd0/rename-into-dir", LEN("/dev_hdd0/rename-into-dir")),
                 FS_ERROR_EEXIST);
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-into-dir", NULL);
    CHECK_EQ_INT(path_exists(path), 1);
    /* an escape is refused by the same rule unlink refuses it with (the resolved path is
     * still inside the root, so this is a cross-mount answer, not a confinement error), and
     * nothing moved */
    (void)make_path(target, sizeof target, g_outside, "sentinel.txt", NULL);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt"),
                           "/dev_hdd0/../../OUTSIDE/stolen.txt",
                           LEN("/dev_hdd0/../../OUTSIDE/stolen.txt")),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(path_exists(target), 1);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt"),
                           "/dev_bdvd/PS3_GAME/stolen.txt",
                           LEN("/dev_bdvd/PS3_GAME/stolen.txt")),
                 FS_ERROR_EROFS);
    (void)make_path(path, sizeof path, g_root, "bdvd/PS3_GAME/stolen.txt", NULL);
    CHECK_EQ_INT(path_exists(path), 0);
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/rename-dst.txt", LEN("/dev_hdd0/rename-dst.txt"),
                           "/dev_hdd1/rename-dst.txt", LEN("/dev_hdd1/rename-dst.txt")),
                 FS_ERROR_EINVAL);
    CHECK_EQ_INT(service.counters.renames_ok, 2u);

    /* ---- fsync: the bytes a write reported are readable through a second open ---- */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/synced.txt", LEN("/dev_hdd0/synced.txt"),
                         FS_O_WRONLY | FS_O_CREAT, &fd),
                 FS_OK);
    CHECK_EQ_INT(fs_write(&service, fd, (const uint8_t *)"on the medium", 13u, &count), FS_OK);
    CHECK_EQ_INT(count, 13u);
    CHECK_EQ_INT(fs_fsync(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/synced.txt", LEN("/dev_hdd0/synced.txt"),
                         FS_O_RDONLY, &reader),
                 FS_OK);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, reader, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, 13u);
    CHECK_EQ_MEM(buffer, "on the medium", 13u);
    CHECK_EQ_INT(fs_close(&service, reader), FS_OK);
    CHECK_EQ_INT(fs_fsync(&service, fd), FS_ERROR_EBADF); /* the fd was closed */
    CHECK_EQ_INT(fs_opendir(&service, "/dev_hdd0", LEN("/dev_hdd0"), &reader), FS_OK);
    CHECK_EQ_INT(fs_fsync(&service, reader), FS_ERROR_EBADF); /* a directory handle */
    CHECK_EQ_INT(fs_closedir(&service, reader), FS_OK);
    CHECK(storage.calls_sync_file > 0u);

    /* ---- utime: an existing file and directory answer OK, the rest do not ---- */
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/synced.txt", LEN("/dev_hdd0/synced.txt"), 1, 2),
                 FS_OK);
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/created", LEN("/dev_hdd0/created"), 1, 2), FS_OK);
    CHECK_EQ_INT(fs_utime(&service, "/dev_hdd0/absent", LEN("/dev_hdd0/absent"), 1, 2),
                 FS_ERROR_ENOENT);
    CHECK_EQ_INT(fs_utime(&service, "/dev_bdvd/EBOOT.BIN", LEN("/dev_bdvd/EBOOT.BIN"), 1, 2),
                 FS_ERROR_EROFS);

    /* tear the files down: they are this test's own, inside the scratch world */
    (void)make_path(path, sizeof path, g_root, "hdd0/synced.txt", NULL);
    (void)unlink(path);
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-dst.txt", NULL);
    (void)unlink(path);
    (void)make_path(path, sizeof path, g_root, "hdd0/not-a-dir.txt", NULL);
    (void)unlink(path);
    (void)make_path(path, sizeof path, g_root, "hdd0/rename-into-dir", NULL);
    (void)rmdir(path);
    (void)make_path(path, sizeof path, g_root, "hdd0/created", NULL);
    (void)rmdir(path);
}

/* A FAT-style host, whose rename refuses to replace an existing file with EEXIST, as the
 * Switch's own `sdmc:` fsdev does. The fake hands the call the refusal while it has one left
 * and delegates every other call to the real `rename`, so the adapter's unlink-and-retry is
 * what makes the guest's replace succeed. */
typedef struct fat_rename_host {
    unsigned calls;
    unsigned refusals; /* how many calls answered EEXIST instead of delegating */
    unsigned refusals_left;
} fat_rename_host;

static fat_rename_host *g_fat_host;

static int fat_host_rename(const char *from, const char *to)
{
    if (g_fat_host) {
        ++g_fat_host->calls;
        if (g_fat_host->refusals_left > 0u) {
            --g_fat_host->refusals_left;
            ++g_fat_host->refusals;
            errno = EEXIST;
            return -1;
        }
    }
    return rename(from, to);
}

static void test_adapter_rename_replaces_on_a_non_replacing_host(void)
{
    fs_storage_posix storage;
    fs_storage_port port;
    fs_service service;
    fat_rename_host host;
    unsigned index;
    char source[1200];
    char target[1200];
    char directory[1200];
    uint8_t buffer[64];
    uint32_t fd = 0;
    uint64_t count = 0;

    memset(&host, 0, sizeof host);
    host.refusals_left = 1u;
    g_fat_host = &host;
    CHECK_EQ_INT(fs_storage_posix_set_host_rename(fat_host_rename), 0);

    (void)make_path(source, sizeof source, g_root, "hdd0/fat-src.txt", NULL);
    (void)make_path(target, sizeof target, g_root, "hdd0/fat-dst.txt", NULL);
    (void)make_path(directory, sizeof directory, g_root, "hdd0/fat-dir", NULL);
    CHECK_EQ_INT(write_file(source, "new bytes\n"), 1);
    CHECK_EQ_INT(write_file(target, "old bytes\n"), 1);
    CHECK_EQ_INT(mkdir(directory, 0777) == 0 || errno == EEXIST, 1);

    CHECK_EQ_INT(fs_storage_posix_init(&storage, g_root), 0);
    port = fs_storage_posix_port(&storage);
    fs_service_init(&service, &port);
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index)
        CHECK_EQ_INT(fs_mount_add(&service, fs_canonical_guest_mount(index), g_root), 0);

    /* the guest replaces an existing file and is told OK, exactly as on a POSIX host */
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/fat-src.txt", LEN("/dev_hdd0/fat-src.txt"),
                           "/dev_hdd0/fat-dst.txt", LEN("/dev_hdd0/fat-dst.txt")),
                 FS_OK);
    CHECK_EQ_INT(host.refusals, 1u); /* the host's refusing call happened */
    CHECK_EQ_INT(host.calls, 2u);    /* and the rename was retried exactly once */
    CHECK_EQ_INT(storage.rename_replacements, 1u);
    CHECK_EQ_INT(path_exists(source), 0); /* the source is gone */
    CHECK_EQ_INT(path_exists(target), 1); /* the destination is there */
    CHECK_EQ_INT(fs_open(&service, "/dev_hdd0/fat-dst.txt", LEN("/dev_hdd0/fat-dst.txt"),
                         FS_O_RDONLY, &fd),
                 FS_OK);
    memset(buffer, 0, sizeof buffer);
    CHECK_EQ_INT(fs_read(&service, fd, buffer, sizeof buffer, &count), FS_OK);
    CHECK_EQ_INT(count, LEN("new bytes\n"));
    CHECK_EQ_MEM(buffer, "new bytes\n", LEN("new bytes\n"));
    CHECK_EQ_INT(fs_close(&service, fd), FS_OK);

    /* a directory destination is never unlinked, even when the host refuses */
    CHECK_EQ_INT(write_file(source, "must survive\n"), 1);
    host.refusals_left = 1u;
    CHECK_EQ_INT(fs_rename(&service, "/dev_hdd0/fat-src.txt", LEN("/dev_hdd0/fat-src.txt"),
                           "/dev_hdd0/fat-dir", LEN("/dev_hdd0/fat-dir")),
                 FS_ERROR_EEXIST);
    CHECK_EQ_INT(path_exists(directory), 1);
    CHECK_EQ_INT(path_exists(source), 1);
    CHECK_EQ_INT(storage.rename_replacements, 1u); /* no replacement was attempted */

    CHECK_EQ_INT(fs_storage_posix_set_host_rename(NULL), 0); /* back to the real rename */
    g_fat_host = NULL;

    (void)unlink(source);
    (void)unlink(target);
    (void)rmdir(directory);
}

/* ------------------------------------------------------------------------------------ */
/* main                                                                                 */
/* ------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch-build-dir>\n", argv[0]);
        return 2;
    }
    if (!make_path(g_scratch, sizeof g_scratch, argv[1], "fs-service-world-XXXXXX", NULL)) return 2;
    if (!mkdtemp(g_scratch)) {
        perror("mkdtemp");
        return 2;
    }
    if (!make_path(g_world, sizeof g_world, g_scratch, NULL, NULL)) return 2;
    if (!make_path(g_root, sizeof g_root, g_world, "root", NULL)) return 2;
    if (!make_path(g_outside, sizeof g_outside, g_world, "OUTSIDE", NULL)) return 2;
    if (mkdir(g_root, 0777) != 0) {
        perror("mkdir root");
        return 2;
    }
    if (mkdir(g_outside, 0777) != 0) {
        perror("mkdir outside");
        return 2;
    }
    {
        char hdd0[1200], bdvd[1200];
        (void)make_path(hdd0, sizeof hdd0, g_root, "hdd0", NULL);
        (void)make_path(bdvd, sizeof bdvd, g_root, "bdvd", NULL);
        if (mkdir(hdd0, 0777) != 0 || mkdir(bdvd, 0777) != 0) {
            perror("mkdir mount");
            return 2;
        }
    }
    /* the sibling sentinel, created before any test so the confinement test can prove it
     * survives every refusal, and the delete test can prove it survives every delete */
    {
        char sentinel[1200];
        (void)make_path(sentinel, sizeof sentinel, g_outside, "sentinel.txt", NULL);
        if (!write_file(sentinel, "must survive\n")) {
            perror("sentinel");
            return 2;
        }
    }

    test_stat_record_bytes();
    test_dirent_bytes();
    test_path_confinement();
    test_operations();
    test_adapter_confinement_rules();
    test_adapter_refuses_unconfined_paths();
    test_adapter_end_to_end();
    test_file_operations();
    test_case_insensitive_lookup();
    test_create_rename_sync_utime();
    test_adapter_file_io();
    test_adapter_case_insensitive();
    test_adapter_create_rename_sync_utime();
    test_adapter_rename_replaces_on_a_non_replacing_host();

    /* tear the scratch world down: it is ours, and it is inside the build dir */
    remove_tree(g_world, 0u);

    printf("FS_SERVICE checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
