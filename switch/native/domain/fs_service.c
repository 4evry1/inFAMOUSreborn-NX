/* Platform-free native file-system service. See fs_service.h for the contract.
 *
 * No globals, no OS calls, no heap: every buffer is either a caller out-parameter or a fixed
 * member of fs_service / fs_resolved_path.
 */
#include "fs_service.h"

#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Constants                                                                            */
/* ------------------------------------------------------------------------------------ */

#define FS_IFDIR 0040000u
#define FS_IFREG 0100000u
#define FS_PERM_MASK 0777u

/* Guest handle encoding: index in the low byte, generation in the rest. 0 is reserved so a
 * handle value of 0 always means "nothing", matching what the walker treats as failure. */
#define FS_HANDLE_INDEX_MASK 0xFFu
#define FS_HANDLE_GENERATION_SHIFT 8
#define FS_HANDLE_GENERATION_MAX 0x00FFFFFFu
#define FS_CASE_SCAN_LIMIT 4096u

/* ------------------------------------------------------------------------------------ */
/* Little helpers                                                                        */
/* ------------------------------------------------------------------------------------ */

/* Byte writes, not a cast to uint32_t*: the record must be big-endian and byte-addressable
 * whatever the host's alignment rules are (CONTRACT.md §1c: "do not copy a host stat struct").
 * Nothing here reads memory through a wider type, so UBSan has nothing to object to. */
static void put_u8(uint8_t *out, uint8_t value)
{
    out[0] = value;
}

static void put_be32(uint8_t *out, uint32_t value)
{
    put_u8(out + 0, (uint8_t)(value >> 24));
    put_u8(out + 1, (uint8_t)(value >> 16));
    put_u8(out + 2, (uint8_t)(value >> 8));
    put_u8(out + 3, (uint8_t)value);
}

static void put_be64(uint8_t *out, uint64_t value)
{
    put_be32(out + 0, (uint32_t)(value >> 32));
    put_be32(out + 4, (uint32_t)value);
}

/* `memchr` and friends require a non-NULL pointer even for 0 bytes; UBSan enforces it. The
 * service therefore keeps a valid empty string to point at instead of NULL. */
#define FS_EMPTY ""

static size_t bounded_length(const char *text, size_t cap)
{
    size_t length = 0;
    if (!text) return 0;
    while (length < cap && text[length]) ++length;
    return length;
}

/* Appends `count` bytes of `text` at *offset, refusing to overflow `cap`. */
static int append(char *out, size_t cap, size_t *offset, const char *text, size_t count)
{
    /* <cap leaves room for the terminator, so `count == 0` writes nothing at all and can
     * never be handed a NULL source */
    if (*offset + count >= cap) return 0;
    if (count != 0u) memcpy(out + *offset, text, count);
    *offset += count;
    out[*offset] = 0;
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Canonical mount table                                                                */
/* ------------------------------------------------------------------------------------ */

/* The five guest prefixes the guest image builds paths from (CALLS.md §0 literals) plus the
 * host-side escape hatch `/host_root` the boundary prep names. */
static const char *const k_guest_mounts[FS_MOUNT_CAPACITY] = {
    "/dev_hdd0", "/dev_hdd1", "/dev_bdvd", "/app_home", "/host_root",
};
static const char *const k_host_leaves[FS_MOUNT_CAPACITY] = {
    "hdd0", "hdd1", "bdvd", "app_home", "host_root",
};

const char *fs_canonical_guest_mount(uint32_t index)
{
    if (index >= FS_MOUNT_CAPACITY) return NULL;
    return k_guest_mounts[index];
}

const char *fs_canonical_host_leaf(uint32_t index)
{
    if (index >= FS_MOUNT_CAPACITY) return NULL;
    return k_host_leaves[index];
}

/* Index of the canonical guest prefix `guest`, or FS_MOUNT_CAPACITY when it is not one. */
static uint32_t canonical_index(const char *guest, size_t length)
{
    uint32_t index;
    if (!guest) return FS_MOUNT_CAPACITY;
    for (index = 0; index < FS_MOUNT_CAPACITY; ++index) {
        const char *candidate = k_guest_mounts[index];
        const size_t candidate_length = strlen(candidate);
        if (candidate_length == length && memcmp(candidate, guest, length) == 0) return index;
    }
    return FS_MOUNT_CAPACITY;
}

/* ------------------------------------------------------------------------------------ */
/* Lifecycle and mounts                                                                 */
/* ------------------------------------------------------------------------------------ */

void fs_service_init(fs_service *service, const fs_storage_port *port)
{
    if (!service) return;
    memset(service, 0, sizeof *service);
    if (port) service->port = *port;
    service->last_storage_status = FS_STORAGE_OK;
    service->last_guest_result = FS_OK;
}

void fs_mount_clear(fs_service *service)
{
    if (!service) return;
    memset(service->mounts, 0, sizeof service->mounts);
    service->mount_count = 0;
}

int fs_mount_add(fs_service *service, const char *guest, const char *root_token)
{
    size_t guest_length, token_length, leaf_length, offset = 0;
    uint32_t index, canonical;
    fs_mount *slot = NULL;

    if (!service) return -1;
    if (!guest) guest = FS_EMPTY;
    if (!root_token) root_token = FS_EMPTY;
    guest_length = bounded_length(guest, FS_MOUNTPOINT_MAX + 2u);
    if (guest_length == 0 || guest_length > FS_MOUNTPOINT_MAX) return -1;
    canonical = canonical_index(guest, guest_length);
    if (canonical >= FS_MOUNT_CAPACITY) return -1;

    token_length = bounded_length(root_token, sizeof slot->host + 1u);
    if (token_length == 0) return -1;
    /* one trailing '/' away: "<root>/" and "<root>" are the same root */
    if (token_length > 1u && root_token[token_length - 1u] == '/') --token_length;
    if (token_length == 1u && root_token[0] == '/') return -1; /* "/" would put the mount at the host root */
    leaf_length = strlen(k_host_leaves[canonical]);
    if (token_length + 1u + leaf_length >= sizeof slot->host) return -1;

    /* replace an existing registration of the same prefix, else append */
    for (index = 0; index < service->mount_count; ++index) {
        if (service->mounts[index].present && strlen(service->mounts[index].guest) == guest_length &&
            memcmp(service->mounts[index].guest, guest, guest_length) == 0) {
            slot = &service->mounts[index];
            break;
        }
    }
    if (!slot) {
        if (service->mount_count >= FS_MOUNT_CAPACITY) return -1;
        slot = &service->mounts[service->mount_count];
    }

    memset(slot, 0, sizeof *slot);
    memset(slot->guest, 0, sizeof slot->guest);
    memcpy(slot->guest, guest, guest_length);
    if (!append(slot->host, sizeof slot->host, &offset, root_token, token_length)) return -1;
    if (!append(slot->host, sizeof slot->host, &offset, "/", 1u)) return -1;
    if (!append(slot->host, sizeof slot->host, &offset, k_host_leaves[canonical], leaf_length))
        return -1;
    slot->present = 1;
    slot->case_insensitive = (guest_length == sizeof "/dev_bdvd" - 1u &&
                              memcmp(guest, "/dev_bdvd", guest_length) == 0) ? 1u : 0u;
    if (slot == &service->mounts[service->mount_count]) ++service->mount_count;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* Resolution and confinement                                                           */
/* ------------------------------------------------------------------------------------ */

/* Matches a guest prefix on a component boundary, longest prefix first. Returns the mount
 * index or FS_MOUNT_CAPACITY, and writes the number of characters consumed. */
static uint32_t match_mount(const fs_service *service, const char *path, size_t *consumed)
{
    uint32_t index;
    uint32_t best = FS_MOUNT_CAPACITY;
    size_t best_length = 0;

    *consumed = 0;
    for (index = 0; index < service->mount_count; ++index) {
        const fs_mount *mount = &service->mounts[index];
        size_t length;
        if (!mount->present) continue;
        length = strlen(mount->guest);
        if (length == 0u || length <= best_length) continue;
        if (strncmp(path, mount->guest, length) != 0) continue;
        if (path[length] != 0 && path[length] != '/') continue;
        best = index;
        best_length = length;
    }
    *consumed = best_length;
    return best;
}

/* Pushes one path component onto the confined stack. "." vanishes, ".." pops one component
 * and is clamped at the mount root, and anything longer than a single name is rejected.
 * Returns FS_OK or the guest code. */
static int32_t push_component(char *stack, size_t *stack_length, uint32_t *component_count,
                              const char *text, size_t length)
{
    if (length == 0u) return FS_OK; /* repeated '/' */
    if (length == 1u && text[0] == '.') return FS_OK;
    if (length == 2u && text[0] == '.' && text[1] == '.') {
        while (*stack_length > 0u && stack[*stack_length - 1u] != '/') --*stack_length;
        if (*stack_length > 0u) --*stack_length;
        stack[*stack_length] = 0;
        if (*component_count > 0u) --*component_count;
        return FS_OK;
    }
    if (length > FS_DIR_ENTRY_NAME_MAX) return FS_ERROR_ENAMETOOLONG;
    if (*stack_length != 0u && !append(stack, FS_PATH_MAX, stack_length, "/", 1u))
        return FS_ERROR_ENAMETOOLONG;
    if (!append(stack, FS_PATH_MAX, stack_length, text, length)) return FS_ERROR_ENAMETOOLONG;
    ++*component_count;
    return FS_OK;
}

int32_t fs_resolve_path(const fs_service *service, const char *guest_path, size_t length,
                        fs_resolved_path *out)
{
    char stack[FS_PATH_MAX];
    size_t stack_length = 0;
    size_t position, mount_length = 0, offset = 0;
    uint32_t mount_index;
    uint32_t component_count = 0;
    uint32_t requires_directory = 0;
    int last_component_is_dot = 0;

    if (!service || !out) return FS_ERROR_EFAULT;
    if (!guest_path) guest_path = FS_EMPTY;
    if (length >= FS_PATH_MAX) return FS_ERROR_ENAMETOOLONG;
    if (length == 0u) return FS_ERROR_ENOENT;
    /* the guest path is NUL-terminated and `length` is its strlen, so the terminator sits at
     * guest_path[length]. A path whose terminator is missing or further away than the stated
     * length is not a string this service can walk: report it instead of reading past it.
     * (One byte past `length` is allowed, which is exactly the terminator.) */
    if (memchr(guest_path, 0, length + 1u) == NULL) return FS_ERROR_ENAMETOOLONG;
    if (guest_path[0] != '/') return FS_ERROR_ENOENT;

    mount_index = match_mount(service, guest_path, &mount_length);
    if (mount_index >= FS_MOUNT_CAPACITY || !service->mounts[mount_index].present)
        return FS_ERROR_ENOENT;

    memset(stack, 0, sizeof stack);
    /* a trailing '/' always asks for a directory */
    if (guest_path[length - 1u] == '/') requires_directory = 1u;

    position = mount_length;
    while (position < length) {
        size_t start;
        int32_t status;
        while (position < length && guest_path[position] == '/') ++position;
        start = position;
        while (position < length && guest_path[position] != '/') ++position;
        if (position == start) break;
        /* A path that ENDS in '.' or '..' (clamped or not) names a directory. Root fix (2026-11-11, Eden
         * r28): this used to latch for a dot component anywhere, so the game's
         * ".../USRDIR/../ICON0.PNG.tmp" (a file next to USRDIR) was refused with EISDIR. Only the last
         * component decides; a trailing '/' was already handled above. */
        last_component_is_dot =
            (position - start == 1u && guest_path[start] == '.') ||
            (position - start == 2u && guest_path[start] == '.' && guest_path[start + 1u] == '.');
        status = push_component(stack, &stack_length, &component_count, guest_path + start,
                                position - start);
        if (status != FS_OK) return status;
    }

    if (last_component_is_dot) requires_directory = 1u;
    memset(out, 0, sizeof *out);
    out->mount_index = mount_index;
    if (!append(out->host_path, sizeof out->host_path, &offset,
                service->mounts[mount_index].host, strlen(service->mounts[mount_index].host))) {
        return FS_ERROR_ENAMETOOLONG;
    }
    if (stack_length != 0u) {
        if (!append(out->host_path, sizeof out->host_path, &offset, "/", 1u))
            return FS_ERROR_ENAMETOOLONG;
        if (!append(out->host_path, sizeof out->host_path, &offset, stack, stack_length))
            return FS_ERROR_ENAMETOOLONG;
    }
    if (stack_length != 0u) memcpy(out->relative, stack, stack_length + 1u);
    out->host = out->host_path;
    out->component_count = component_count;
    out->requires_directory = requires_directory;
    return FS_OK;
}

static unsigned char ascii_case_fold(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z') return (unsigned char)(byte + ('a' - 'A'));
    return byte;
}

static int ascii_case_equal(const char *left, size_t left_length,
                            const char *right, size_t right_length)
{
    size_t index;
    if (left_length != right_length) return 0;
    for (index = 0; index < left_length; ++index) {
        if (ascii_case_fold((unsigned char)left[index]) !=
            ascii_case_fold((unsigned char)right[index])) return 0;
    }
    return 1;
}

/* Rebuilds a /dev_bdvd path by matching each component against the host directory listing
 * ignoring ASCII case. The caller only invokes it after the exact confined path was
 * NOT_FOUND. Returns 1 with the corrected path, or 0 when the original result must stand. */
static int fs_case_resolve(fs_service *service, const fs_resolved_path *resolved,
                           char *out, size_t out_capacity)
{
    char current[FS_PATH_MAX + 224u];
    char candidate[FS_PATH_MAX + 224u];
    char matched[FS_DIR_ENTRY_NAME_MAX + 1u];
    const fs_mount *mount;
    const char *relative;
    size_t current_length, position = 0;

    if (!service || !resolved || !out || out_capacity == 0u) return 0;
    if (resolved->mount_index >= service->mount_count) return 0;
    mount = &service->mounts[resolved->mount_index];
    if (!mount->present || !mount->case_insensitive) return 0;
    if (!service->port.stat || !service->port.open_dir || !service->port.read_dir ||
        !service->port.close_dir) return 0;

    current_length = strlen(mount->host);
    if (current_length >= sizeof current) return 0;
    memcpy(current, mount->host, current_length + 1u);
    relative = resolved->relative;

    while (relative[position]) {
        size_t start = position, component_length, candidate_length, matched_length = 0;
        unsigned matches = 0;
        uint32_t index;
        int keep_going = 1;
        fs_storage_status status;
        void *handle = NULL;

        while (relative[position] && relative[position] != '/') ++position;
        component_length = position - start;
        candidate_length = current_length;
        memcpy(candidate, current, current_length + 1u);
        if (!append(candidate, sizeof candidate, &candidate_length, "/", 1u) ||
            !append(candidate, sizeof candidate, &candidate_length, relative + start,
                    component_length)) return 0;

        status = service->port.stat(service->port.context, candidate, &(fs_stat_info){0});
        if (status == FS_STORAGE_OK) {
            memcpy(current, candidate, candidate_length + 1u);
            current_length = candidate_length;
            if (relative[position] == '/') ++position;
            continue;
        }
        if (status != FS_STORAGE_NOT_FOUND) return 0;

        status = service->port.open_dir(service->port.context, current, &handle);
        if (status != FS_STORAGE_OK) return 0;
        if (!handle) {
            if (service->port.close_dir) service->port.close_dir(service->port.context, handle);
            return 0;
        }
        for (index = 0; index <= FS_CASE_SCAN_LIMIT; ++index) {
            fs_dir_entry entry;
            uint32_t has_entry = 0;
            size_t name_length;
            memset(&entry, 0, sizeof entry);
            status = service->port.read_dir(service->port.context, handle, index, &entry,
                                            &has_entry);
            if (status != FS_STORAGE_OK || (index == FS_CASE_SCAN_LIMIT && has_entry)) {
                keep_going = 0;
                break;
            }
            if (!has_entry) break;
            name_length = bounded_length(entry.name, sizeof entry.name);
            if (ascii_case_equal(relative + start, component_length, entry.name, name_length)) {
                ++matches;
                if (matches > 1u) {
                    ++service->counters.case_ambiguous;
                    keep_going = 0;
                    break;
                }
                matched_length = name_length;
                if (matched_length >= sizeof matched) {
                    keep_going = 0;
                    break;
                }
                memcpy(matched, entry.name, matched_length);
                matched[matched_length] = 0;
            }
        }
        if (service->port.close_dir) service->port.close_dir(service->port.context, handle);
        if (!keep_going || matches != 1u) return 0;

        candidate_length = current_length;
        memcpy(candidate, current, current_length + 1u);
        if (!append(candidate, sizeof candidate, &candidate_length, "/", 1u) ||
            !append(candidate, sizeof candidate, &candidate_length, matched, matched_length))
            return 0;
        memcpy(current, candidate, candidate_length + 1u);
        current_length = candidate_length;
        if (relative[position] == '/') ++position;
    }

    if (current_length >= out_capacity) return 0;
    memcpy(out, current, current_length + 1u);
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Encoders                                                                             */
/* ------------------------------------------------------------------------------------ */

void fs_encode_cellfs_stat(const fs_stat_info *in, uint8_t out[FS_CELLFS_STAT_BYTES])
{
    uint32_t mode;
    if (!out) return;
    if (!in) {
        memset(out, 0, FS_CELLFS_STAT_BYTES);
        return;
    }
    mode = (in->is_directory ? FS_IFDIR : FS_IFREG) | (in->mode & FS_PERM_MASK);
    put_be32(out + 0x00, mode);
    put_be32(out + 0x04, (uint32_t)in->uid);
    put_be32(out + 0x08, (uint32_t)in->gid);
    put_be64(out + 0x0C, (uint64_t)in->atime);
    put_be64(out + 0x14, (uint64_t)in->mtime);
    put_be64(out + 0x1C, (uint64_t)in->ctime);
    put_be64(out + 0x24, in->size);
    put_be64(out + 0x2C, in->blksize);
}

void fs_encode_cellfs_dirent(const fs_dir_entry *in, uint8_t out[FS_CELLFS_DIRENT_BYTES])
{
    size_t name_length = 0;
    if (!out) return;
    memset(out, 0, FS_CELLFS_DIRENT_BYTES);
    if (!in) {
        out[0] = (uint8_t)FS_CELLFS_DTYPE_FILE;
        return;
    }
    /* CALLS.md §3a: the guest compares byte 0 to 2 (directory) then 1 (file). 1 is the
     * safer default for anything that is not positively a directory. */
    out[0] = (uint8_t)(in->is_directory ? FS_CELLFS_DTYPE_DIR : FS_CELLFS_DTYPE_FILE);
    name_length = in->name_length;
    if (name_length > FS_DIRENT_NAME_MAX) name_length = FS_DIRENT_NAME_MAX;
    out[1] = (uint8_t)name_length;
    if (name_length) memcpy(out + 2, in->name, name_length);
    out[2u + name_length] = 0;
}

void fs_encode_cellfs_nread(uint32_t has_entry, uint8_t out[FS_CELLFS_NREAD_BYTES])
{
    if (!out) return;
    put_be64(out, has_entry ? 1u : 0u);
}

/* ------------------------------------------------------------------------------------ */
/* Descriptor registry                                                                  */
/* ------------------------------------------------------------------------------------ */

static uint32_t encode_handle(uint32_t index, uint32_t generation)
{
    return ((generation & FS_HANDLE_GENERATION_MAX) << FS_HANDLE_GENERATION_SHIFT) |
           (index & FS_HANDLE_INDEX_MASK);
}

/* Resolves a guest handle to its live descriptor slot, or -1. */
static int find_descriptor(fs_service *service, uint32_t handle, uint32_t *index_out)
{
    const uint32_t index = handle & FS_HANDLE_INDEX_MASK;
    const uint32_t generation = (handle >> FS_HANDLE_GENERATION_SHIFT) & FS_HANDLE_GENERATION_MAX;
    if (handle == 0u) return -1;
    if (index >= FS_DESCRIPTOR_CAPACITY) return -1;
    if (!service->descriptors[index].in_use) return -1;
    if (service->descriptors[index].generation != generation) return -1;
    if (index_out) *index_out = index;
    return 0;
}

uint32_t fs_open_descriptor_count(const fs_service *service)
{
    uint32_t index, live = 0;
    if (!service) return 0;
    for (index = 0; index < FS_DESCRIPTOR_CAPACITY; ++index)
        if (service->descriptors[index].in_use) ++live;
    return live;
}

uint32_t fs_open_file_count(const fs_service *service)
{
    uint32_t index, live = 0;
    if (!service) return 0;
    for (index = 0; index < FS_FILE_CAPACITY; ++index)
        if (service->files[index].in_use) ++live;
    return live;
}

uint32_t fs_close_all(fs_service *service)
{
    uint32_t index, closed = 0, closed_files = 0;
    if (!service) return 0;
    for (index = 0; index < FS_DESCRIPTOR_CAPACITY; ++index) {
        fs_descriptor *descriptor = &service->descriptors[index];
        if (!descriptor->in_use) continue;
        if (service->port.close_dir) service->port.close_dir(service->port.context, descriptor->handle);
        descriptor->in_use = 0u;
        descriptor->handle = NULL;
        descriptor->cursor = 0u;
        descriptor->generation = (descriptor->generation + 1u) & FS_HANDLE_GENERATION_MAX;
        memset(descriptor->host_path, 0, sizeof descriptor->host_path);
        ++closed;
    }
    service->counters.calls_closedir += closed;
    for (index = 0; index < FS_FILE_CAPACITY; ++index) {
        fs_file *file = &service->files[index];
        if (!file->in_use) continue;
        if (service->port.close_file) service->port.close_file(service->port.context, file->handle);
        file->in_use = 0u;
        file->handle = NULL;
        file->position = 0u;
        file->access = 0u;
        file->append = 0u;
        memset(file->host_path, 0, sizeof file->host_path);
        ++closed_files;
    }
    service->counters.calls_close += closed_files;
    return closed + closed_files;
}

/* ------------------------------------------------------------------------------------ */
/* Operations                                                                           */
/* ------------------------------------------------------------------------------------ */

static int32_t map_storage_status(fs_service *service, fs_storage_status status,
                                  int32_t not_found_code)
{
    int32_t code;
    service->last_storage_status = status;
    switch (status) {
    case FS_STORAGE_OK: code = FS_OK; break;
    case FS_STORAGE_NOT_FOUND: code = not_found_code; break;
    case FS_STORAGE_NOT_A_DIRECTORY: code = FS_ERROR_ENOTDIR; break;
    case FS_STORAGE_IS_A_DIRECTORY: code = FS_ERROR_EISDIR; break;
    case FS_STORAGE_ACCESS_DENIED: code = FS_ERROR_EACCES; break;
    case FS_STORAGE_NAME_TOO_LONG: code = FS_ERROR_ENAMETOOLONG; break;
    case FS_STORAGE_ALREADY_EXISTS: code = FS_ERROR_EEXIST; break;
    case FS_STORAGE_IO_ERROR: code = FS_ERROR_EIO; break;
    case FS_STORAGE_NOT_SUPPORTED: code = FS_ERROR_ENOTSUP; break;
    default: code = FS_ERROR_EIO; break;
    }
    return code;
}

static int32_t finish(fs_service *service, int32_t code)
{
    service->last_guest_result = code;
    return code;
}

int32_t fs_stat(fs_service *service, const char *guest_path, size_t length, uint8_t *stat_bytes)
{
    fs_resolved_path resolved;
    fs_stat_info info;
    fs_storage_status status;
    char corrected[FS_PATH_MAX + 224u];
    const char *host;
    int used_fallback = 0;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_stat;
    if (!stat_bytes) return finish(service, FS_ERROR_EFAULT);
    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (!service->port.stat) return finish(service, FS_ERROR_ENOTSUP);
    host = resolved.host;
    memset(&info, 0, sizeof info);
    status = service->port.stat(service->port.context, host, &info);
    if (status == FS_STORAGE_NOT_FOUND && fs_case_resolve(service, &resolved, corrected,
            sizeof corrected)) {
        host = corrected;
        used_fallback = 1;
        memset(&info, 0, sizeof info);
        status = service->port.stat(service->port.context, host, &info);
    }
    if (status != FS_STORAGE_OK) return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
    if (resolved.requires_directory && !info.is_directory)
        return finish(service, FS_ERROR_ENOTDIR);
    if (used_fallback) ++service->counters.case_fallbacks;
    fs_encode_cellfs_stat(&info, stat_bytes);
    return finish(service, FS_OK);
}

int32_t fs_opendir(fs_service *service, const char *guest_path, size_t length, uint32_t *out_handle)
{
    fs_resolved_path resolved;
    fs_storage_status status;
    fs_descriptor *descriptor = NULL;
    void *handle = NULL;
    char corrected[FS_PATH_MAX + 224u];
    const char *host;
    int used_fallback = 0;
    uint32_t index;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_opendir;
    /* CONTRACT.md §1a: the public wrapper returns EFAULT for a NULL r4 before any syscall */
    if (!out_handle) return finish(service, FS_ERROR_EFAULT);
    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    for (index = 0; index < FS_DESCRIPTOR_CAPACITY; ++index) {
        if (!service->descriptors[index].in_use) {
            descriptor = &service->descriptors[index];
            break;
        }
    }
    if (!descriptor) {
        ++service->counters.refusals_capacity;
        return finish(service, FS_ERROR_EMFILE);
    }
    if (!service->port.open_dir) return finish(service, FS_ERROR_ENOTSUP);
    host = resolved.host;
    status = service->port.open_dir(service->port.context, host, &handle);
    if (status == FS_STORAGE_NOT_FOUND && fs_case_resolve(service, &resolved, corrected,
            sizeof corrected)) {
        host = corrected;
        used_fallback = 1;
        handle = NULL;
        status = service->port.open_dir(service->port.context, host, &handle);
    }
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
    if (!handle) {
        /* a port that reports success must have produced a handle */
        if (service->port.close_dir) service->port.close_dir(service->port.context, handle);
        return finish(service, FS_ERROR_EIO);
    }
    descriptor->generation = (descriptor->generation + 1u) & FS_HANDLE_GENERATION_MAX;
    if (descriptor->generation == 0u) descriptor->generation = 1u;
    descriptor->in_use = 1u;
    descriptor->cursor = 0u;
    descriptor->handle = handle;
    memset(descriptor->host_path, 0, sizeof descriptor->host_path);
    memcpy(descriptor->host_path, host, strlen(host) + 1u);
    if (used_fallback) ++service->counters.case_fallbacks;
    *out_handle = encode_handle(index, descriptor->generation);
    return finish(service, FS_OK);
}

int32_t fs_readdir(fs_service *service, uint32_t handle, uint8_t *dirent_out, uint8_t *nread_out)
{
    fs_descriptor *descriptor;
    fs_dir_entry entry;
    fs_storage_status status;
    uint32_t index = 0;
    uint32_t has_entry = 0;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_readdir;
    /* CONTRACT.md §1b: the public wrapper's only guard is r4 == 0 -> EFAULT */
    if (!dirent_out) return finish(service, FS_ERROR_EFAULT);
    if (!nread_out) return finish(service, FS_ERROR_EFAULT);
    if (find_descriptor(service, handle, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    descriptor = &service->descriptors[index];
    if (!service->port.read_dir) return finish(service, FS_ERROR_ENOTSUP);
    /* The guest gives the service no cursor, so the descriptor owns one. It only advances
     * on a real entry, so a caller that keeps reading after the end keeps getting nread == 0
     * instead of wrapping back to the first entry. `.` and `..` are skipped by the port:
     * otherwise the walker would recurse on itself forever. */
    memset(&entry, 0, sizeof entry);
    status = service->port.read_dir(service->port.context, descriptor->handle, descriptor->cursor,
                                    &entry, &has_entry);
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
    fs_encode_cellfs_nread(has_entry, nread_out);
    if (!has_entry) return finish(service, FS_OK);
    ++descriptor->cursor;
    fs_encode_cellfs_dirent(&entry, dirent_out);
    return finish(service, FS_OK);
}

int32_t fs_closedir(fs_service *service, uint32_t handle)
{
    fs_descriptor *descriptor;
    uint32_t index = 0;
    int32_t code = FS_OK;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_closedir;
    if (find_descriptor(service, handle, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    descriptor = &service->descriptors[index];
    if (service->port.close_dir) {
        const fs_storage_status status =
            service->port.close_dir(service->port.context, descriptor->handle);
        code = map_storage_status(service, status, FS_ERROR_EIO);
    }
    /* The slot is withdrawn whether or not the host close failed: a failed close must not
     * leave a handle the guest believes is gone (filesystem-reference.md §"Stat/Fstat"):
     * "close validates fd ... closes real handle then withdraws descriptor"). */
    descriptor->in_use = 0u;
    descriptor->handle = NULL;
    descriptor->cursor = 0u;
    descriptor->generation = (descriptor->generation + 1u) & FS_HANDLE_GENERATION_MAX;
    if (descriptor->generation == 0u) descriptor->generation = 1u;
    memset(descriptor->host_path, 0, sizeof descriptor->host_path);
    return finish(service, code);
}

/* Shared body of rmdir/unlink: resolve, refuse the mount root, call the port. */
static int32_t remove_path(fs_service *service, const char *guest_path, size_t length,
                           fs_storage_status (*operation)(void *, const char *),
                           int32_t directory_code, int32_t file_code)
{
    fs_resolved_path resolved;
    fs_storage_status status;
    int32_t code;

    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (resolved.component_count == 0u) {
        /* decision 24: the confined root is writable, but a mount itself is not removable */
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EACCES);
    }
    if (!operation) return finish(service, FS_ERROR_ENOTSUP);
    status = operation(service->port.context, resolved.host);
    if (status == FS_STORAGE_OK) return finish(service, FS_OK);
    if (status == FS_STORAGE_NOT_SUPPORTED) {
        /* an empty-directory rule the host refused: report the operation-specific code the
         * guest expects rather than guessing a host errno */
        service->last_storage_status = status;
        return finish(service, file_code);
    }
    service->last_storage_status = status;
    if (status == FS_STORAGE_IS_A_DIRECTORY) return finish(service, directory_code);
    return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
}

int32_t fs_rmdir(fs_service *service, const char *guest_path, size_t length)
{
    int32_t code;
    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_rmdir;
    code = remove_path(service, guest_path, length, service->port.remove_dir, FS_ERROR_EISDIR,
                       FS_ERROR_ENOTSUP);
    if (code == FS_OK) ++service->counters.deletes_dir_ok;
    return code;
}

int32_t fs_unlink(fs_service *service, const char *guest_path, size_t length)
{
    int32_t code;
    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_unlink;
    code = remove_path(service, guest_path, length, service->port.unlink_file, FS_ERROR_EISDIR,
                       FS_ERROR_ENOENT);
    if (code == FS_OK) ++service->counters.deletes_file_ok;
    return code;
}


/* ------------------------------------------------------------------------------------ */
/* Regular-file operations                                                              */
/* ------------------------------------------------------------------------------------ */

/* Resolves a guest fd to its live registry slot, or -1. Guest fds are small integers, so a
 * closed fd is detectable until its slot is reused (like lv2). */
static int find_file(fs_service *service, uint32_t fd, uint32_t *index_out)
{
    uint32_t index;
    if (fd < FS_FILE_FD_BASE) return -1;
    index = fd - FS_FILE_FD_BASE;
    if (index >= FS_FILE_CAPACITY) return -1;
    if (!service->files[index].in_use) return -1;
    if (index_out) *index_out = index;
    return 0;
}

static void release_file(fs_file *file)
{
    file->in_use = 0u;
    file->handle = NULL;
    file->position = 0u;
    file->access = 0u;
    file->append = 0u;
    memset(file->host_path, 0, sizeof file->host_path);
}

int32_t fs_open(fs_service *service, const char *guest_path, size_t length, uint32_t flags,
                uint32_t *out_fd)
{
    fs_resolved_path resolved;
    fs_storage_status status;
    fs_file *file = NULL;
    void *handle = NULL;
    char corrected[FS_PATH_MAX + 224u];
    const char *host;
    int used_fallback = 0;
    uint32_t access, index;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_open;
    if (!out_fd) return finish(service, FS_ERROR_EFAULT);
    access = flags & FS_O_ACCMODE;
    if ((flags & ~FS_O_SUPPORTED) != 0u || access == FS_O_ACCMODE)
        return finish(service, FS_ERROR_EINVAL);
    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (resolved.component_count == 0u || resolved.requires_directory)
        return finish(service, FS_ERROR_EISDIR);
    for (index = 0; index < FS_FILE_CAPACITY; ++index) {
        if (!service->files[index].in_use) {
            file = &service->files[index];
            break;
        }
    }
    if (!file) {
        ++service->counters.refusals_capacity;
        return finish(service, FS_ERROR_EMFILE);
    }
    if (!service->port.open_file) return finish(service, FS_ERROR_ENOTSUP);
    host = resolved.host;
    status = service->port.open_file(service->port.context, host, access,
                                     (flags & FS_O_CREAT) ? 1u : 0u,
                                     (flags & FS_O_EXCL) ? 1u : 0u,
                                     (flags & FS_O_TRUNC) ? 1u : 0u, &handle);
    if (status == FS_STORAGE_NOT_FOUND && (flags & FS_O_CREAT) == 0u &&
        fs_case_resolve(service, &resolved, corrected, sizeof corrected)) {
        host = corrected;
        used_fallback = 1;
        handle = NULL;
        status = service->port.open_file(service->port.context, host, access, 0u,
                                         (flags & FS_O_EXCL) ? 1u : 0u,
                                         (flags & FS_O_TRUNC) ? 1u : 0u, &handle);
    }
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
    if (!handle) {
        if (service->port.close_file) service->port.close_file(service->port.context, handle);
        return finish(service, FS_ERROR_EIO);
    }
    file->in_use = 1u;
    file->access = access;
    file->append = (flags & FS_O_APPEND) ? 1u : 0u;
    file->position = 0u;
    file->handle = handle;
    memset(file->host_path, 0, sizeof file->host_path);
    memcpy(file->host_path, host, strlen(host) + 1u);
    if (used_fallback) ++service->counters.case_fallbacks;
    *out_fd = FS_FILE_FD_BASE + index;
    return finish(service, FS_OK);
}

int32_t fs_read(fs_service *service, uint32_t fd, uint8_t *buffer, uint32_t length,
                uint64_t *out_read)
{
    fs_file *file;
    fs_storage_status status;
    uint32_t index = 0;
    uint32_t read_count = 0;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_read;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (file->access == FS_O_WRONLY) return finish(service, FS_ERROR_EBADF);
    if (!out_read) return finish(service, FS_ERROR_EFAULT);
    if (!buffer && length != 0u) return finish(service, FS_ERROR_EFAULT);
    if (length == 0u) {
        *out_read = 0u;
        return finish(service, FS_OK);
    }
    if (!service->port.read_file) return finish(service, FS_ERROR_ENOTSUP);
    status = service->port.read_file(service->port.context, file->handle, file->position, buffer,
                                     length, &read_count);
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
    file->position += read_count;
    service->counters.bytes_read += read_count;
    *out_read = read_count;
    return finish(service, FS_OK);
}

int32_t fs_write(fs_service *service, uint32_t fd, const uint8_t *buffer, uint32_t length,
                 uint64_t *out_written)
{
    fs_file *file;
    fs_storage_status status;
    uint32_t index = 0;
    uint32_t written = 0;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_write;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (file->access == FS_O_RDONLY) return finish(service, FS_ERROR_EBADF);
    if (!out_written) return finish(service, FS_ERROR_EFAULT);
    if (!buffer && length != 0u) return finish(service, FS_ERROR_EFAULT);
    if (length == 0u) {
        *out_written = 0u;
        return finish(service, FS_OK);
    }
    if (file->append) {
        uint64_t size = 0u;
        if (!service->port.file_size) return finish(service, FS_ERROR_ENOTSUP);
        status = service->port.file_size(service->port.context, file->handle, &size);
        if (status != FS_STORAGE_OK)
            return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
        file->position = size;
    }
    if (!service->port.write_file) return finish(service, FS_ERROR_ENOTSUP);
    status = service->port.write_file(service->port.context, file->handle, file->position, buffer,
                                      length, &written);
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
    file->position += written;
    service->counters.bytes_written += written;
    *out_written = written;
    return finish(service, FS_OK);
}

int32_t fs_lseek(fs_service *service, uint32_t fd, int64_t offset, uint32_t whence,
                 uint64_t *out_position)
{
    fs_file *file;
    fs_storage_status status;
    uint32_t index = 0;
    int64_t base;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_lseek;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (!out_position) return finish(service, FS_ERROR_EFAULT);
    switch (whence) {
    case FS_SEEK_SET: base = 0; break;
    case FS_SEEK_CUR:
        if (file->position > (uint64_t)INT64_MAX) return finish(service, FS_ERROR_EINVAL);
        base = (int64_t)file->position;
        break;
    case FS_SEEK_END: {
        uint64_t size = 0u;
        if (!service->port.file_size) return finish(service, FS_ERROR_ENOTSUP);
        status = service->port.file_size(service->port.context, file->handle, &size);
        if (status != FS_STORAGE_OK)
            return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
        if (size > (uint64_t)INT64_MAX) return finish(service, FS_ERROR_EINVAL);
        base = (int64_t)size;
        break;
    }
    default:
        return finish(service, FS_ERROR_EINVAL);
    }
    if (offset > 0 && base > INT64_MAX - offset) return finish(service, FS_ERROR_EINVAL);
    if (offset < 0 && base < INT64_MIN - offset) return finish(service, FS_ERROR_EINVAL);
    base += offset;
    if (base < 0) return finish(service, FS_ERROR_EINVAL);
    file->position = (uint64_t)base;
    *out_position = file->position;
    return finish(service, FS_OK);
}

int32_t fs_fstat(fs_service *service, uint32_t fd, uint8_t *stat_bytes)
{
    fs_file *file;
    fs_stat_info info;
    fs_storage_status status;
    uint32_t index = 0;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_fstat;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (!stat_bytes) return finish(service, FS_ERROR_EFAULT);
    if (!service->port.stat) return finish(service, FS_ERROR_ENOTSUP);
    memset(&info, 0, sizeof info);
    status = service->port.stat(service->port.context, file->host_path, &info);
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
    fs_encode_cellfs_stat(&info, stat_bytes);
    return finish(service, FS_OK);
}

int32_t fs_close(fs_service *service, uint32_t fd)
{
    fs_file *file;
    uint32_t index = 0;
    int32_t code = FS_OK;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_close;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (service->port.close_file) {
        const fs_storage_status status =
            service->port.close_file(service->port.context, file->handle);
        code = map_storage_status(service, status, FS_ERROR_EIO);
    } else {
        code = FS_ERROR_ENOTSUP;
    }
    release_file(file);
    return finish(service, code);
}

/* ------------------------------------------------------------------------------------ */
/* Directory creation, rename, flush and timestamps                                      */
/* ------------------------------------------------------------------------------------ */

#define FS_DISC_GUEST_MOUNT "/dev_bdvd"

/* Is this resolved path under the disc, i.e. on the mount whose guest name is `/dev_bdvd`?
 * The guest name is the authority; `fs_mount_add` is the only place that decides which mount
 * is case-insensitive, and both answers come from the same string. */
static int is_on_disc_mount(const fs_service *service, const fs_resolved_path *resolved)
{
    const fs_mount *mount;
    const size_t disc_length = sizeof FS_DISC_GUEST_MOUNT - 1u;

    if (!service || !resolved) return 0;
    if (resolved->mount_index >= service->mount_count) return 0;
    mount = &service->mounts[resolved->mount_index];
    if (!mount->present) return 0;
    return strlen(mount->guest) == disc_length &&
           memcmp(mount->guest, FS_DISC_GUEST_MOUNT, disc_length) == 0;
}

int32_t fs_mkdir(fs_service *service, const char *guest_path, size_t length, uint32_t mode)
{
    fs_resolved_path resolved;
    fs_storage_status status;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_mkdir;
    (void)mode; /* accepted and ignored: the confined root carries its own permissions */
    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (resolved.component_count == 0u) {
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EEXIST);
    }
    if (is_on_disc_mount(service, &resolved)) {
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EROFS);
    }
    if (!service->port.make_dir) return finish(service, FS_ERROR_ENOTSUP);
    status = service->port.make_dir(service->port.context, resolved.host);
    if (status == FS_STORAGE_OK) {
        ++service->counters.creates_dir_ok;
        return finish(service, FS_OK);
    }
    return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
}

int32_t fs_rename(fs_service *service, const char *guest_from, size_t from_length,
                  const char *guest_to, size_t to_length)
{
    fs_resolved_path from;
    fs_resolved_path to;
    fs_storage_status status;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_rename;
    code = fs_resolve_path(service, guest_from, from_length, &from);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    code = fs_resolve_path(service, guest_to, to_length, &to);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (from.component_count == 0u || to.component_count == 0u) {
        /* a mount itself is neither movable nor replaceable */
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EACCES);
    }
    if (is_on_disc_mount(service, &from) || is_on_disc_mount(service, &to)) {
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EROFS);
    }
    if (from.mount_index != to.mount_index) {
        /* lv2 answers EXDEV for a cross-device rename; that value is not in the captured
         * contract, so report the code that is provably an error instead of a guess */
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EINVAL);
    }
    if (!service->port.rename_path) return finish(service, FS_ERROR_ENOTSUP);
    status = service->port.rename_path(service->port.context, from.host, to.host);
    if (status == FS_STORAGE_OK) {
        ++service->counters.renames_ok;
        return finish(service, FS_OK);
    }
    return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
}

int32_t fs_fsync(fs_service *service, uint32_t fd)
{
    fs_file *file;
    fs_storage_status status;
    uint32_t index = 0;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_fsync;
    if (find_file(service, fd, &index) != 0) {
        ++service->counters.refusals_stale_handle;
        return finish(service, FS_ERROR_EBADF);
    }
    file = &service->files[index];
    if (file->access == FS_O_RDONLY) return finish(service, FS_OK);
    if (!service->port.sync_file) return finish(service, FS_ERROR_ENOTSUP);
    status = service->port.sync_file(service->port.context, file->handle);
    return finish(service, map_storage_status(service, status, FS_ERROR_EIO));
}

int32_t fs_utime(fs_service *service, const char *guest_path, size_t length,
                 int64_t access_time, int64_t modification_time)
{
    fs_resolved_path resolved;
    fs_stat_info info;
    fs_storage_status status;
    int32_t code;

    if (!service) return FS_ERROR_EFAULT;
    ++service->counters.calls_utime;
    /* no host path in the tree carries a timestamp the guest can read back: the times are
     * accepted, the existence of the path is checked, nothing is written */
    (void)access_time;
    (void)modification_time;
    code = fs_resolve_path(service, guest_path, length, &resolved);
    if (code != FS_OK) {
        ++service->counters.refusals_guest_path;
        return finish(service, code);
    }
    if (is_on_disc_mount(service, &resolved)) {
        ++service->counters.refusals_guest_path;
        return finish(service, FS_ERROR_EROFS);
    }
    if (!service->port.stat) return finish(service, FS_ERROR_ENOTSUP);
    memset(&info, 0, sizeof info);
    status = service->port.stat(service->port.context, resolved.host, &info);
    if (status != FS_STORAGE_OK)
        return finish(service, map_storage_status(service, status, FS_ERROR_ENOENT));
    if (resolved.requires_directory && !info.is_directory)
        return finish(service, FS_ERROR_ENOTDIR);
    return finish(service, FS_OK);
}
