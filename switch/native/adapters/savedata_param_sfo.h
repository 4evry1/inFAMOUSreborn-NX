#ifndef NATIVE_SAVEDATA_PARAM_SFO_H
#define NATIVE_SAVEDATA_PARAM_SFO_H
/* Root (2026-11-16, Decision 129): the pure part of the native cellSaveData service in startup_execution.cpp.
 *
 *   - the PARAM.SFO every save directory carries (built from the game's CellSaveDataSystemFileParam, read back into
 *     CellSaveDataStatGet.getParam and the load list);
 *   - the name rules for save directories and save files;
 *   - the list of SECUREFILE names. Secure files are stored as PLAIN files (no PFD, no encryption: nothing outside this
 *     port ever reads them). PARAM.SFO remembers which files were written as SECUREFILE with one "*<file name>" key per
 *     file holding the game's 16-byte secureFileId, so the stat file list can report the right fileType again.
 *
 * No guest memory and no file system here: savedata_param_sfo_host_test.cpp builds it with plain g++. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace savedata_sfo {

constexpr size_t title_bytes = 128, sub_title_bytes = 128, detail_bytes = 1024, list_param_bytes = 8,
    directory_bytes = 64, dir_name_bytes = 32, file_name_bytes = 13, secure_id_bytes = 16, secure_file_max = 113;

/* CellSaveDataFileSet.fileType */
enum : uint32_t {
    file_type_secure = 0, file_type_normal = 1, file_type_icon0 = 2, file_type_icon1 = 3, file_type_pic1 = 4,
    file_type_snd0 = 5,
};

struct SecureFile {
    char name[file_name_bytes];
    uint8_t id[secure_id_bytes];
};

/* What one save directory's PARAM.SFO says. Every string is NUL-terminated inside its array. */
struct Params {
    char title[title_bytes];
    char sub_title[sub_title_bytes];
    char detail[detail_bytes];
    uint32_t attribute;
    char list_param[list_param_bytes];
    char directory[directory_bytes];
    uint32_t secure_count;
    SecureFile secure[secure_file_max];
};

/* Copies at most `capacity - 1` bytes of `source` (which need not be terminated inside `source_bytes`) and terminates. */
inline void copy_bounded(char *target, size_t capacity, const char *source, size_t source_bytes)
{
    size_t length = 0;
    while (length + 1 < capacity && length < source_bytes && source[length]) ++length;
    memcpy(target, source, length);
    memset(target + length, 0, capacity - length);
}

/* ---- names ------------------------------------------------------------------------------------------------------ */

/* A save directory name: 1..31 characters of A-Z a-z 0-9 '-' '_' (the console's rule is upper case only; lower case
 * is accepted because it can do no harm to the path). Anything else could name a path below the savedata folder. */
inline bool dir_name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    size_t length = 0;
    for (; name[length]; ++length) {
        const char c = name[length];
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed || length + 1 >= dir_name_bytes) return false;
    }
    return true;
}

inline bool is_system_file(const char *name)
{
    return !strcmp(name, "PARAM.SFO") || !strcmp(name, "PARAM.PFD");
}

/* A file the game names in CellSaveDataFileSet.fileName: 1..12 printable characters, no path separator, not "." or
 * "..", not one of the files the service itself owns. */
inline bool file_name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    size_t length = 0;
    for (; name[length]; ++length) {
        const unsigned char c = static_cast<unsigned char>(name[length]);
        if (c < 0x21 || c > 0x7E || c == '/' || c == '\\' || c == ':' || length + 1 >= file_name_bytes) return false;
    }
    return strcmp(name, ".") && strcmp(name, "..") && !is_system_file(name);
}

/* The fixed name of a content file type, or nullptr for SECUREFILE / NORMALFILE (named by the game) and bad types. */
inline const char *content_file_name(uint32_t file_type)
{
    switch (file_type) {
    case file_type_icon0: return "ICON0.PNG";
    case file_type_icon1: return "ICON1.PAM";
    case file_type_pic1: return "PIC1.PNG";
    case file_type_snd0: return "SND0.AT3";
    default: return nullptr;
    }
}

/* CellSaveDataSetList.dirNamePrefix: one or more prefixes separated by '|'. An empty list matches every name. */
inline bool prefix_matches(const char *prefixes, const char *name)
{
    if (!prefixes || !prefixes[0]) return true;
    const char *start = prefixes;
    for (;;) {
        const char *end = strchr(start, '|');
        const size_t length = end ? size_t(end - start) : strlen(start);
        if (!strncmp(name, start, length)) return true;
        if (!end) return false;
        start = end + 1;
    }
}

/* ---- the secure file list ------------------------------------------------------------------------------------- */

inline int secure_find(const Params &params, const char *name)
{
    for (uint32_t index = 0; index < params.secure_count; ++index)
        if (!strcmp(params.secure[index].name, name)) return int(index);
    return -1;
}

inline void secure_remove(Params &params, const char *name)
{
    const int found = secure_find(params, name);
    if (found < 0) return;
    for (uint32_t index = uint32_t(found); index + 1 < params.secure_count; ++index) params.secure[index] = params.secure[index + 1];
    --params.secure_count;
}

/* Records `name` as a secure file with `id`. Returns false when the list is full (113 entries, the console's limit). */
inline bool secure_set(Params &params, const char *name, const uint8_t id[secure_id_bytes])
{
    int found = secure_find(params, name);
    if (found < 0) {
        if (params.secure_count >= secure_file_max) return false;
        found = int(params.secure_count++);
        copy_bounded(params.secure[found].name, file_name_bytes, name, file_name_bytes);
    }
    memcpy(params.secure[found].id, id, secure_id_bytes);
    return true;
}

inline uint32_t file_type_of(const Params &params, const char *name)
{
    for (uint32_t type = file_type_icon0; type <= file_type_snd0; ++type)
        if (!strcmp(content_file_name(type), name)) return type;
    return secure_find(params, name) >= 0 ? uint32_t(file_type_secure) : uint32_t(file_type_normal);
}

/* ---- PARAM.SFO ------------------------------------------------------------------------------------------------ */
/* Layout (all little-endian): a 20-byte header {"\0PSF", version 0x0101, key table offset, data table offset, entry
 * count}, then 16-byte index entries {u16 key offset, u16 format, u32 used length, u32 max length, u32 data offset},
 * then the NUL-terminated keys (padded to 4), then the data. Keys are sorted. */
constexpr uint32_t sfo_version = 0x0101, sfo_header_bytes = 20, sfo_index_bytes = 16;
enum : uint16_t { sfo_utf8_special = 0x0004, sfo_utf8 = 0x0204, sfo_int32 = 0x0404 };
constexpr uint32_t sfo_category_max = 4, sfo_secure_key_bytes = 1 + file_name_bytes;
constexpr uint32_t sfo_fixed_entries = 9;
constexpr uint32_t sfo_entry_max = sfo_fixed_entries + secure_file_max;

inline void put_le16(uint8_t *at, uint32_t value) { at[0] = uint8_t(value); at[1] = uint8_t(value >> 8); }
inline void put_le32(uint8_t *at, uint32_t value)
{
    at[0] = uint8_t(value); at[1] = uint8_t(value >> 8); at[2] = uint8_t(value >> 16); at[3] = uint8_t(value >> 24);
}
inline uint32_t get_le16(const uint8_t *at) { return uint32_t(at[0]) | uint32_t(at[1]) << 8; }
inline uint32_t get_le32(const uint8_t *at)
{
    return uint32_t(at[0]) | uint32_t(at[1]) << 8 | uint32_t(at[2]) << 16 | uint32_t(at[3]) << 24;
}
inline uint32_t align4(uint32_t value) { return (value + 3u) & ~3u; }

struct SfoEntry {
    const char *key;
    uint16_t format;
    const void *data;      // string, raw bytes, or nullptr for an int
    uint32_t used, max;
    uint32_t number;
};

inline size_t bounded_length(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length]) ++length;
    return length;
}

inline SfoEntry sfo_string(const char *key, const char *text, uint32_t max)
{
    const uint32_t used = uint32_t(bounded_length(text, max - 1) + 1);
    return SfoEntry{key, sfo_utf8, text, used, max, 0};
}

/* Encodes `params` (plus the fixed console keys) into `out`. Returns the byte count, or 0 when `capacity` is short. */
inline size_t encode(const Params &params, uint8_t *out, size_t capacity)
{
    static const char account_id[16] = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'};
    char secure_keys[secure_file_max][sfo_secure_key_bytes];
    SfoEntry entries[sfo_entry_max];
    uint32_t count = 0;
    const uint32_t secure_count = params.secure_count < secure_file_max ? params.secure_count : uint32_t(secure_file_max);
    for (uint32_t index = 0; index < secure_count; ++index) {
        secure_keys[index][0] = '*';
        copy_bounded(secure_keys[index] + 1, file_name_bytes, params.secure[index].name, file_name_bytes);
        entries[count++] = SfoEntry{secure_keys[index], sfo_utf8_special, params.secure[index].id, secure_id_bytes, secure_id_bytes, 0};
    }
    entries[count++] = SfoEntry{"ACCOUNT_ID", sfo_utf8_special, account_id, sizeof account_id, sizeof account_id, 0};
    entries[count++] = SfoEntry{"ATTRIBUTE", sfo_int32, nullptr, 4, 4, params.attribute};
    entries[count++] = sfo_string("CATEGORY", "SD", sfo_category_max);
    entries[count++] = sfo_string("DETAIL", params.detail, detail_bytes);
    entries[count++] = SfoEntry{"PARENTAL_LEVEL", sfo_int32, nullptr, 4, 4, 0};
    entries[count++] = sfo_string("SAVEDATA_DIRECTORY", params.directory, directory_bytes);
    entries[count++] = sfo_string("SAVEDATA_LIST_PARAM", params.list_param, list_param_bytes);
    entries[count++] = sfo_string("SUB_TITLE", params.sub_title, sub_title_bytes);
    entries[count++] = sfo_string("TITLE", params.title, title_bytes);
    for (uint32_t index = 1; index < count; ++index)
        for (uint32_t at = index; at > 0 && strcmp(entries[at - 1].key, entries[at].key) > 0; --at) {
            const SfoEntry swap = entries[at];
            entries[at] = entries[at - 1];
            entries[at - 1] = swap;
        }

    uint32_t key_bytes = 0, data_bytes = 0;
    for (uint32_t index = 0; index < count; ++index) {
        key_bytes += uint32_t(strlen(entries[index].key) + 1);
        data_bytes += entries[index].max;
    }
    const uint32_t key_table = sfo_header_bytes + count * sfo_index_bytes;
    const uint32_t data_table = align4(key_table + key_bytes);
    const uint32_t total = data_table + data_bytes;
    if (!out || total > capacity) return 0;
    memset(out, 0, total);
    out[0] = 0; out[1] = 'P'; out[2] = 'S'; out[3] = 'F';
    put_le32(out + 4, sfo_version);
    put_le32(out + 8, key_table);
    put_le32(out + 12, data_table);
    put_le32(out + 16, count);
    uint32_t key_at = 0, data_at = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const SfoEntry &entry = entries[index];
        uint8_t *slot = out + sfo_header_bytes + index * sfo_index_bytes;
        put_le16(slot, key_at);
        put_le16(slot + 2, entry.format);
        put_le32(slot + 4, entry.used);
        put_le32(slot + 8, entry.max);
        put_le32(slot + 12, data_at);
        const size_t key_length = strlen(entry.key) + 1;
        memcpy(out + key_table + key_at, entry.key, key_length);
        key_at += uint32_t(key_length);
        uint8_t *data = out + data_table + data_at;
        if (entry.format == sfo_int32) put_le32(data, entry.number);
        else if (entry.format == sfo_utf8) memcpy(data, entry.data, entry.used - 1);   // the NUL is already there
        else memcpy(data, entry.data, entry.used);
        data_at += entry.max;
    }
    return total;
}

/* Decodes a PARAM.SFO into `params` (zeroed first). Unknown keys are skipped. False on a malformed file. */
inline bool decode(const uint8_t *in, size_t size, Params &params)
{
    memset(&params, 0, sizeof params);
    if (!in || size < sfo_header_bytes || in[0] != 0 || in[1] != 'P' || in[2] != 'S' || in[3] != 'F') return false;
    const uint32_t key_table = get_le32(in + 8), data_table = get_le32(in + 12), count = get_le32(in + 16);
    if (key_table > size || data_table > size || count > (size - sfo_header_bytes) / sfo_index_bytes) return false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint8_t *slot = in + sfo_header_bytes + index * sfo_index_bytes;
        const uint32_t key_at = key_table + get_le16(slot), format = get_le16(slot + 2), used = get_le32(slot + 4);
        const uint64_t data_at = uint64_t(data_table) + get_le32(slot + 12);
        if (key_at >= size || data_at > size || used > size - data_at) return false;
        const char *key = reinterpret_cast<const char *>(in + key_at);
        if (!memchr(key, 0, size - key_at)) return false;
        const char *text = reinterpret_cast<const char *>(in + data_at);
        if (key[0] == '*') {
            if (used < secure_id_bytes || params.secure_count >= secure_file_max || !file_name_valid(key + 1)) continue;
            SecureFile &file = params.secure[params.secure_count++];
            copy_bounded(file.name, file_name_bytes, key + 1, file_name_bytes);
            memcpy(file.id, in + data_at, secure_id_bytes);
        } else if (!strcmp(key, "ATTRIBUTE")) {
            if (format == sfo_int32 && used >= 4) params.attribute = get_le32(in + data_at);
        } else if (!strcmp(key, "TITLE")) {
            copy_bounded(params.title, title_bytes, text, used);
        } else if (!strcmp(key, "SUB_TITLE")) {
            copy_bounded(params.sub_title, sub_title_bytes, text, used);
        } else if (!strcmp(key, "DETAIL")) {
            copy_bounded(params.detail, detail_bytes, text, used);
        } else if (!strcmp(key, "SAVEDATA_LIST_PARAM")) {
            copy_bounded(params.list_param, list_param_bytes, text, used);
        } else if (!strcmp(key, "SAVEDATA_DIRECTORY")) {
            copy_bounded(params.directory, directory_bytes, text, used);
        }
    }
    return true;
}

}  // namespace savedata_sfo

#endif /* NATIVE_SAVEDATA_PARAM_SFO_H */
