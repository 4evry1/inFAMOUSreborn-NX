/* Platform-free game-data service. See gamedata_service.h for the contract.
 *
 * No globals, no allocation, no OS calls: every byte the service hands back is written into
 * caller-provided buffers, and every host effect goes through the injected port.
 */
#include "gamedata_service.h"

#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Guest byte accessors                                                                 */
/* ------------------------------------------------------------------------------------ */

static void store_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static void store_be64(uint8_t *dst, uint64_t value)
{
    store_be32(dst, (uint32_t)(value >> 32));
    store_be32(dst + 4, (uint32_t)value);
}

static uint32_t load_be32(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

/* PARAM.SFO is little-endian (report-independent format fact). */
static uint16_t load_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t load_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void store_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void store_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int32_t saturate_i32(uint64_t value)
{
    return value > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)value;
}

/* ------------------------------------------------------------------------------------ */
/* Name validation (§1.2)                                                               */
/* ------------------------------------------------------------------------------------ */

static int name_char_ok(uint8_t c)
{
    return (c >= (uint8_t)'A' && c <= (uint8_t)'Z') || (c >= (uint8_t)'0' && c <= (uint8_t)'9') ||
           c == (uint8_t)'-' || c == (uint8_t)'_';
}

int32_t gamedata_validate_dir_name(const uint8_t *src, size_t readable, uint32_t sdk_version)
{
    /* SDK > 0x36FFFF moves the terminator boundary in by one position (last position 31). */
    const size_t max_len = sdk_version > 0x36FFFFu ? 31u : GAMEDATA_DIRNAME_MAX;
    size_t length = 0;
    int terminated = 0;

    if (src == NULL || readable == 0u) {
        return -2;
    }

    /* src[0] == '_' is rejected outright, before the character loop. */
    if (src[0] == (uint8_t)'_') {
        return -1;
    }

    while (length < readable) {
        const uint8_t c = src[length];
        if (c == 0u) {
            terminated = 1;
            break;
        }
        if (!name_char_ok(c)) {
            return -1;
        }
        length++;
    }

    if (!terminated) {
        return -2; /* no NUL inside the readable range */
    }
    if (length == 0u || length > max_len) {
        return -2;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* getParam decoding helpers                                                            */
/* ------------------------------------------------------------------------------------ */

static size_t param_string(const uint8_t *param, uint32_t offset, uint32_t cap, char *dst)
{
    size_t i;
    /* `cap` is the guest field size; the value must terminate inside it (the last byte is
     * kept for the NUL), so an unterminated field is truncated rather than read past. */
    for (i = 0; i + 1u < cap && param[offset + i] != 0u; i++) {
        dst[i] = (char)param[offset + i];
    }
    dst[i] = '\0';
    return i;
}

/* Bounded UTF-8 -> guest field copy. Valid UTF-8 sequences pass through; an invalid or
 * overlong sequence becomes '?', so the field is always valid UTF-8 and byte-identical to the
 * source for ASCII input. The trailing NUL always fits inside `cap`. */
static void utf8_copy(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t i = 0;
    size_t o = 0;

    if (cap == 0u) {
        return;
    }
    while (i < len && o + 1u < cap) {
        const uint8_t c = src[i];
        size_t need;
        size_t k;
        int ok = 1;

        if (c < 0x80u) {
            dst[o++] = (char)c;
            i++;
            continue;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            need = 1u;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            need = 2u;
        } else if (c >= 0xF0u && c <= 0xF4u) {
            need = 3u;
        } else {
            dst[o++] = '?';
            i++;
            continue;
        }

        if (len - i - 1u < need) {
            dst[o++] = '?';
            i++;
            continue;
        }
        for (k = 1u; k <= need; k++) {
            if ((src[i + k] & 0xC0u) != 0x80u) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            /* Reject overlong encodings and surrogates. */
            if (c == 0xE0u && src[i + 1u] < 0xA0u) {
                ok = 0;
            } else if (c == 0xEDu && src[i + 1u] > 0x9Fu) {
                ok = 0;
            } else if (c == 0xF0u && src[i + 1u] < 0x90u) {
                ok = 0;
            } else if (c == 0xF4u && src[i + 1u] > 0x8Fu) {
                ok = 0;
            }
        }
        if (!ok || o + need + 1u >= cap) {
            dst[o++] = '?';
            i++;
            continue;
        }
        for (k = 0; k <= need; k++) {
            dst[o++] = (char)src[i + k];
        }
        i += need + 1u;
    }
    dst[o] = '\0';
}

/* Bounded string copy for a decoded PARAM.SFO value into a guest field of `cap` bytes. The
 * source is a NUL-terminated SFO string; decoding is validated to UTF-8 (invalid sequences
 * become '?') so the guest field is always well-formed. */
static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    size_t n = 0;
    while (n < len && src[n] != 0u) {
        n++;
    }
    utf8_copy(dst, cap, src, n);
}

/* ------------------------------------------------------------------------------------ */
/* PARAM.SFO (v1.1)                                                                     */
/* ------------------------------------------------------------------------------------ */

#define SFO_MAGIC "\0PSF"
#define SFO_MAGIC_BYTES 4u
#define SFO_VERSION 0x0101u
#define SFO_HEADER_BYTES 20u
/* Index entry, 16 bytes: +0 key_offset u16, +2 fmt u16, +4 len u32, +8 max_len u32,
 * +12 data_offset u32 (relative to the data table). */
#define SFO_INDEX_ENTRY_BYTES 16u
#define SFO_INDEX_KEY_OFFSET 0u
#define SFO_INDEX_FMT 2u
#define SFO_INDEX_LEN 4u
#define SFO_INDEX_MAX_LEN 8u
#define SFO_INDEX_DATA_OFFSET 12u
#define SFO_FMT_UTF8 0x0204u
#define SFO_FMT_INT32 0x0404u
#define SFO_MAX_ENTRIES 32u
#define SFO_KEY_CAP 16u
#define SFO_ALIGN 4u

typedef struct sfo_out_entry {
    const char *key;
    size_t key_len;
    uint16_t fmt;
    uint32_t max_len;
    int32_t value_int;
    const char *value_str; /* NULL for fmt int32 */
    uint32_t value_len;    /* incl. NUL for strings */
} sfo_out_entry;

typedef struct sfo_in_entry {
    char key[SFO_KEY_CAP];
    uint16_t fmt;
    uint32_t max_len;
    uint32_t data_offset; /* relative to the data table */
    uint32_t value_len;
} sfo_in_entry;

static int key_less(const sfo_out_entry *a, const sfo_out_entry *b)
{
    const size_t n = a->key_len < b->key_len ? a->key_len : b->key_len;
    const int c = memcmp(a->key, b->key, n);
    if (c != 0) {
        return c < 0;
    }
    return a->key_len < b->key_len;
}

static size_t align4(size_t value)
{
    return (value + (SFO_ALIGN - 1u)) & ~(size_t)(SFO_ALIGN - 1u);
}

/* Key comparison without strcmp: the object's symbol closure is restricted to the mem*
 * functions and strlen, so the decoder compares keys inline. */
static int key_is(const char *key, const char *literal)
{
    size_t i;
    for (i = 0; literal[i] != '\0'; i++) {
        if (key[i] != literal[i]) {
            return 0;
        }
    }
    return key[i] == '\0';
}

static int has_nul_byte(const uint8_t *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (data[i] == 0u) {
            return 1;
        }
    }
    return 0;
}

size_t gamedata_param_sfo_encode(const uint8_t param[GAMEDATA_PARAM_BYTES], uint8_t *out,
                                 size_t cap)
{
    static const char *const lang_key[GAMEDATA_PARAM_TITLE_LANG_COUNT] = {
        "TITLE_00", "TITLE_01", "TITLE_02", "TITLE_03", "TITLE_04",
        "TITLE_05", "TITLE_06", "TITLE_07", "TITLE_08", "TITLE_09",
        "TITLE_10", "TITLE_11", "TITLE_12", "TITLE_13", "TITLE_14",
        "TITLE_15", "TITLE_16", "TITLE_17", "TITLE_18", "TITLE_19"};
    sfo_out_entry entry[SFO_MAX_ENTRIES];
    char title_lang[GAMEDATA_PARAM_TITLE_LANG_COUNT][GAMEDATA_PARAM_TITLE_SIZE];
    char title[GAMEDATA_PARAM_TITLE_SIZE];
    char title_id[GAMEDATA_SFO_TITLE_ID_MAX];
    char version[GAMEDATA_SFO_VERSION_MAX];
    size_t count = 0;
    size_t i;
    size_t key_table = 0;
    size_t data_table;
    size_t index_at;
    size_t key_at;
    size_t data_at;
    size_t total;

    if (param == NULL || out == NULL) {
        return 0;
    }

    param_string(param, GAMEDATA_PARAM_TITLE, GAMEDATA_PARAM_TITLE_SIZE, title);
    param_string(param, GAMEDATA_PARAM_TITLE_ID, GAMEDATA_PARAM_TITLE_ID_SIZE, title_id);
    param_string(param, GAMEDATA_PARAM_DATA_VERSION, GAMEDATA_PARAM_DATA_VERSION_SIZE, version);

    memset(entry, 0, sizeof(entry));
    memset(title_lang, 0, sizeof(title_lang));

    entry[count].key = "ATTRIBUTE";
    entry[count].fmt = SFO_FMT_INT32;
    entry[count].max_len = 4u;
    entry[count].value_len = 4u;
    entry[count].value_int = (int32_t)load_be32(param + GAMEDATA_PARAM_ATTRIBUTE);
    count++;

    entry[count].key = "CATEGORY";
    entry[count].fmt = SFO_FMT_UTF8;
    entry[count].max_len = 3u;
    entry[count].value_str = "GD";
    entry[count].value_len = 3u;
    count++;

    entry[count].key = "PARENTAL_LEVEL";
    entry[count].fmt = SFO_FMT_INT32;
    entry[count].max_len = 4u;
    entry[count].value_len = 4u;
    entry[count].value_int = (int32_t)load_be32(param + GAMEDATA_PARAM_PARENTAL_LEVEL);
    count++;

    entry[count].key = "TITLE";
    entry[count].fmt = SFO_FMT_UTF8;
    entry[count].max_len = GAMEDATA_SFO_TITLE_MAX;
    entry[count].value_str = title;
    entry[count].value_len = (uint32_t)strlen(title) + 1u;
    count++;

    for (i = 0; i < GAMEDATA_PARAM_TITLE_LANG_COUNT; i++) {
        const uint32_t offset =
            GAMEDATA_PARAM_TITLE_LANG + (uint32_t)i * GAMEDATA_PARAM_TITLE_SIZE;
        const size_t lang_len =
            param_string(param, offset, GAMEDATA_PARAM_TITLE_SIZE, title_lang[i]);
        if (lang_len == 0u) {
            continue; /* only non-empty titleLang entries are emitted */
        }
        entry[count].key = lang_key[i];
        entry[count].fmt = SFO_FMT_UTF8;
        entry[count].max_len = GAMEDATA_SFO_TITLE_MAX;
        entry[count].value_str = title_lang[i];
        entry[count].value_len = (uint32_t)lang_len + 1u;
        count++;
    }

    entry[count].key = "TITLE_ID";
    entry[count].fmt = SFO_FMT_UTF8;
    entry[count].max_len = GAMEDATA_SFO_TITLE_ID_MAX;
    entry[count].value_str = title_id;
    entry[count].value_len = (uint32_t)strlen(title_id) + 1u;
    count++;

    entry[count].key = "VERSION";
    entry[count].fmt = SFO_FMT_UTF8;
    entry[count].max_len = GAMEDATA_SFO_VERSION_MAX;
    entry[count].value_str = version;
    entry[count].value_len = (uint32_t)strlen(version) + 1u;
    count++;

    for (i = 0; i < count; i++) {
        entry[i].key_len = strlen(entry[i].key);
    }
    /* Stable insertion sort: the index must be in bytewise ascending key order. */
    for (i = 1; i < count; i++) {
        const sfo_out_entry current = entry[i];
        size_t j = i;
        while (j > 0 && key_less(&current, &entry[j - 1])) {
            entry[j] = entry[j - 1];
            j--;
        }
        entry[j] = current;
    }

    for (i = 0; i < count; i++) {
        key_table += entry[i].key_len + 1u;
    }

    data_table = align4(SFO_HEADER_BYTES + count * SFO_INDEX_ENTRY_BYTES + key_table);

    total = data_table;
    for (i = 0; i < count; i++) {
        total += entry[i].fmt == SFO_FMT_INT32 ? 4u : entry[i].value_len;
        total = align4(total);
    }
    if (total > cap) {
        return 0;
    }

    memset(out, 0, total);
    memcpy(out, SFO_MAGIC, SFO_MAGIC_BYTES);
    store_le32(out + 4, SFO_VERSION);
    /* +8 is where the index table starts (right after the header), not where the key table
     * starts: the index entries themselves carry the key offsets. */
    store_le32(out + 8, (uint32_t)SFO_HEADER_BYTES);
    store_le32(out + 12, (uint32_t)count);
    store_le32(out + 16, (uint32_t)data_table);

    index_at = SFO_HEADER_BYTES;
    key_at = SFO_HEADER_BYTES + count * SFO_INDEX_ENTRY_BYTES;
    data_at = data_table;
    for (i = 0; i < count; i++) {
        store_le16(out + index_at + SFO_INDEX_KEY_OFFSET, (uint16_t)key_at);
        store_le16(out + index_at + SFO_INDEX_FMT, entry[i].fmt);
        store_le32(out + index_at + SFO_INDEX_LEN, entry[i].value_len);
        store_le32(out + index_at + SFO_INDEX_MAX_LEN, entry[i].max_len);
        /* Index data offsets are relative to the data table, not to the file. */
        store_le32(out + index_at + SFO_INDEX_DATA_OFFSET, (uint32_t)(data_at - data_table));
        memcpy(out + key_at, entry[i].key, entry[i].key_len + 1u);
        key_at += entry[i].key_len + 1u;
        index_at += SFO_INDEX_ENTRY_BYTES;

        if (entry[i].fmt == SFO_FMT_INT32) {
            store_le32(out + data_at, (uint32_t)entry[i].value_int);
            data_at += 4u;
        } else {
            memcpy(out + data_at, entry[i].value_str, entry[i].value_len);
            data_at += entry[i].value_len;
        }
        data_at = align4(data_at);
    }
    return total;
}

gamedata_status gamedata_param_sfo_decode(const uint8_t *sfo, size_t len,
                                          uint8_t param_out[GAMEDATA_PARAM_BYTES])
{
    sfo_in_entry entries[SFO_MAX_ENTRIES];
    char title[GAMEDATA_PARAM_TITLE_SIZE];
    char title_id[GAMEDATA_SFO_TITLE_ID_MAX];
    char lang[GAMEDATA_PARAM_TITLE_LANG_COUNT][GAMEDATA_PARAM_TITLE_SIZE];
    char version[GAMEDATA_SFO_VERSION_MAX];
    int lang_present[GAMEDATA_PARAM_TITLE_LANG_COUNT];
    int have_version = 0;
    uint32_t count;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t i;

    if (param_out == NULL) {
        return GAMEDATA_BAD_ARGUMENT;
    }
    memset(param_out, 0, GAMEDATA_PARAM_BYTES);

    if (sfo == NULL || len < SFO_HEADER_BYTES) {
        return GAMEDATA_BROKEN_PARAM;
    }
    if (memcmp(sfo, SFO_MAGIC, SFO_MAGIC_BYTES) != 0 || load_le32(sfo + 4) != SFO_VERSION) {
        return GAMEDATA_BROKEN_PARAM;
    }
    index_offset = load_le32(sfo + 8);
    count = load_le32(sfo + 12);
    data_offset = load_le32(sfo + 16);

    if (count > SFO_MAX_ENTRIES) {
        return GAMEDATA_BROKEN_PARAM;
    }
    /* The image must hold the whole index, and `index_offset <= data_offset <= len` must be
     * established before the index region is measured against it. */
    if ((uint64_t)index_offset < (uint64_t)SFO_HEADER_BYTES ||
        (uint64_t)index_offset > (uint64_t)len ||
        (uint64_t)count * (uint64_t)SFO_INDEX_ENTRY_BYTES >
            (uint64_t)len - (uint64_t)index_offset) {
        return GAMEDATA_BROKEN_PARAM; /* truncated index */
    }
    if ((uint64_t)data_offset > (uint64_t)len ||
        (uint64_t)data_offset < (uint64_t)index_offset) {
        return GAMEDATA_BROKEN_PARAM; /* data table outside the image */
    }
    memset(entries, 0, sizeof(entries));
    for (i = 0; i < count; i++) {
        const uint8_t *const e = sfo + index_offset + (size_t)i * SFO_INDEX_ENTRY_BYTES;
        const uint32_t key_offset = load_le16(e + SFO_INDEX_KEY_OFFSET);
        const uint32_t value_len = load_le32(e + SFO_INDEX_LEN);
        const uint32_t value_offset = load_le32(e + SFO_INDEX_DATA_OFFSET);
        size_t k;

        entries[i].fmt = load_le16(e + SFO_INDEX_FMT);
        entries[i].max_len = load_le32(e + SFO_INDEX_MAX_LEN);
        entries[i].data_offset = value_offset;
        entries[i].value_len = value_len;

        /* The key must start inside the key table (between the index and the data table)
         * and be NUL-terminated before the data table. */
        if ((uint64_t)key_offset < (uint64_t)index_offset ||
            (uint64_t)key_offset >= (uint64_t)data_offset) {
            return GAMEDATA_BROKEN_PARAM;
        }
        entries[i].key[0] = '\0'; /* so a key that starts exactly at the terminator is valid */
        for (k = 0; k + 1u < SFO_KEY_CAP; k++) {
            const size_t at = (size_t)key_offset + k;
            if (at >= (size_t)data_offset) {
                return GAMEDATA_BROKEN_PARAM;
            }
            entries[i].key[k] = (char)sfo[at];
            if (sfo[at] == 0u) {
                break;
            }
        }
        /* A key that fills SFO_KEY_CAP-1 bytes without a terminator is rejected too: the loop
         * leaves key[k] holding the last byte read, not a NUL. */
        if (k >= SFO_KEY_CAP - 1u || entries[i].key[k] != '\0') {
            return GAMEDATA_BROKEN_PARAM;
        }

        /* The value must lie inside the file, inside the data table. */
        if ((uint64_t)value_offset > (uint64_t)((size_t)len - (size_t)data_offset) ||
            (uint64_t)value_offset + (uint64_t)value_len >
                (uint64_t)((size_t)len - (size_t)data_offset)) {
            return GAMEDATA_BROKEN_PARAM; /* data offset past the end */
        }
    }

    title[0] = '\0';
    title_id[0] = '\0';
    version[0] = '\0';
    for (i = 0; i < GAMEDATA_PARAM_TITLE_LANG_COUNT; i++) {
        lang[i][0] = '\0';
        lang_present[i] = 0;
    }

    for (i = 0; i < count; i++) {
        const sfo_in_entry *const e = &entries[i];
        const uint8_t *const value = sfo + data_offset + e->data_offset;
        const size_t value_len = (size_t)e->value_len;

        if (e->fmt == SFO_FMT_INT32) {
            const int32_t raw = (int32_t)load_le32(value);
            if (e->max_len < 4u) {
                continue; /* the field could not hold an int32 */
            }
            if (key_is(e->key, "ATTRIBUTE")) {
                store_be32(param_out + GAMEDATA_PARAM_ATTRIBUTE, (uint32_t)raw);
            } else if (key_is(e->key, "PARENTAL_LEVEL")) {
                store_be32(param_out + GAMEDATA_PARAM_PARENTAL_LEVEL, (uint32_t)raw);
            }
            /* Unknown keys are ignored. */
        } else if (e->fmt == SFO_FMT_UTF8) {
            if (value_len == 0u || !has_nul_byte(value, value_len)) {
                return GAMEDATA_BROKEN_PARAM; /* string without NUL inside max_len */
            }
            if (key_is(e->key, "TITLE")) {
                if (e->max_len > GAMEDATA_SFO_TITLE_MAX) {
                    return GAMEDATA_BROKEN_PARAM;
                }
                copy_field(title, GAMEDATA_PARAM_TITLE_SIZE, value, value_len);
            } else if (key_is(e->key, "TITLE_ID")) {
                if (e->max_len > GAMEDATA_SFO_TITLE_ID_MAX) {
                    return GAMEDATA_BROKEN_PARAM;
                }
                copy_field(title_id, GAMEDATA_SFO_TITLE_ID_MAX, value, value_len);
            } else if (key_is(e->key, "VERSION")) {
                if (e->max_len > GAMEDATA_SFO_VERSION_MAX) {
                    return GAMEDATA_BROKEN_PARAM;
                }
                copy_field(version, GAMEDATA_SFO_VERSION_MAX, value, value_len);
                have_version = 1;
            } else if (e->key[0] == 'T' && e->key[1] == 'I' && e->key[2] == 'T' &&
                       e->key[3] == 'L' && e->key[4] == 'E' && e->key[5] == '_' &&
                       e->key[6] >= '0' && e->key[6] <= '9' && e->key[7] >= '0' &&
                       e->key[7] <= '9' && e->key[8] == '\0') {
                const uint32_t index =
                    (uint32_t)(e->key[6] - '0') * 10u + (uint32_t)(e->key[7] - '0');
                if (index >= GAMEDATA_PARAM_TITLE_LANG_COUNT) {
                    continue; /* outside the guest array */
                }
                if (e->max_len > GAMEDATA_SFO_TITLE_MAX) {
                    return GAMEDATA_BROKEN_PARAM;
                }
                copy_field(lang[index], GAMEDATA_PARAM_TITLE_SIZE, value, value_len);
                lang_present[index] = 1;
            }
            /* Unknown keys are ignored. */
        }
        /* Unknown formats are ignored. */
    }

    memcpy(param_out + GAMEDATA_PARAM_TITLE, title, strlen(title) + 1u);
    memcpy(param_out + GAMEDATA_PARAM_TITLE_ID, title_id, strlen(title_id) + 1u);
    if (have_version) {
        memcpy(param_out + GAMEDATA_PARAM_DATA_VERSION, version, strlen(version) + 1u);
    }
    for (i = 0; i < GAMEDATA_PARAM_TITLE_LANG_COUNT; i++) {
        if (lang_present[i]) {
            memcpy(param_out + GAMEDATA_PARAM_TITLE_LANG + i * GAMEDATA_PARAM_TITLE_SIZE, lang[i],
                   strlen(lang[i]) + 1u);
        }
    }
    return GAMEDATA_OK;
}

/* ------------------------------------------------------------------------------------ */
/* StatGet construction (§1.3)                                                          */
/* ------------------------------------------------------------------------------------ */

static int port_usable(const gamedata_storage_port *port)
{
    return port != NULL && port->free_kb != NULL && port->stat_dir != NULL &&
           port->read_file != NULL && port->create_dir != NULL && port->write_file != NULL;
}

static size_t append_literal(char *dst, size_t cap, size_t at, const char *text)
{
    size_t i;
    for (i = 0; text[i] != '\0' && at + 1u < cap; i++) {
        dst[at++] = text[i];
    }
    dst[at] = '\0';
    return at;
}

/* Builds "/dev_hdd0/game/<dir>" (and ".../USRDIR") into the guest field, bounded by its
 * documented capacity (0x41F / 0x421 including the NUL). The result is always terminated
 * inside `cap`: an over-long composition is truncated, never left unterminated. */
static void build_path(uint8_t *field, size_t cap, const char *dir_name, int with_usrdir)
{
    char path[0x421];
    size_t at = 0;

    path[0] = '\0';
    at = append_literal(path, sizeof(path), at, "/dev_hdd0/game/");
    at = append_literal(path, sizeof(path), at, dir_name);
    if (with_usrdir) {
        at = append_literal(path, sizeof(path), at, "/USRDIR");
    }

    memset(field, 0, cap);
    if (at + 1u > cap) {
        at = cap - 1u;
    }
    memcpy(field, path, at);
    field[at] = '\0';
}

gamedata_status gamedata_build_statget(const gamedata_storage_port *port, const char *dir_name,
                                       uint8_t out[GAMEDATA_STATGET_BYTES],
                                       gamedata_check *state)
{
    static const char *const param_sfo = "PARAM.SFO";
    uint8_t sfo[GAMEDATA_SFO_MAX_BYTES];
    size_t sfo_len = 0;
    uint64_t free_kb = 0;
    uint64_t size_kb = 0;
    int64_t times[3] = {0, 0, 0};
    gamedata_storage_status status;

    if (!port_usable(port) || out == NULL || state == NULL ||
        gamedata_validate_dir_name((const uint8_t *)(dir_name == NULL ? "" : dir_name),
                                   dir_name == NULL ? 0u : strlen(dir_name) + 1u,
                                   0u) != 0) {
        return GAMEDATA_BAD_ARGUMENT;
    }

    /* The firmware zeroes the whole record before the field-by-field fill (report §1.3). */
    memset(out, 0, GAMEDATA_STATGET_BYTES);

    status = port->free_kb(port->context, &free_kb);
    if (status == GAMEDATA_STORAGE_NO_SPACE) {
        free_kb = 0;
    } else if (status != GAMEDATA_STORAGE_OK) {
        return GAMEDATA_STORAGE_ERROR;
    }

    status = port->stat_dir(port->context, dir_name, &size_kb, times);
    if (status == GAMEDATA_STORAGE_NOT_FOUND) {
        /* No directory: new data, zero size, zero times, zero getParam. */
        size_kb = 0;
        times[0] = 0;
        times[1] = 0;
        times[2] = 0;
        store_be32(out + GAMEDATA_STATGET_IS_NEW_DATA, GAMEDATA_IS_NEW_DATA_YES);
    } else if (status != GAMEDATA_STORAGE_OK) {
        return GAMEDATA_STORAGE_ERROR;
    } else {
        status = port->read_file(port->context, dir_name, param_sfo, sfo, sizeof(sfo), &sfo_len);
#ifdef PS3_NATIVE_LLE_PROFILE
        /* Root (2026-11-12, Decision 77): the directory is there but has no PARAM.SFO. inFAMOUS never asks this
         * service to create its game-data directory (its callback sets no parameters); it writes its install
         * straight into USRDIR. So on every start after the first the directory exists without a PARAM.SFO, and
         * the answer below (a storage error) ended the run at once: found with a pre-installed disk under Eden,
         * and it would have hit the second start on a console. Such a directory is now reported as new data with
         * its real size and times; the game's own install stamp decides what it does next. */
        if (status == GAMEDATA_STORAGE_NOT_FOUND) {
            store_be32(out + GAMEDATA_STATGET_IS_NEW_DATA, GAMEDATA_IS_NEW_DATA_YES);
        } else
#endif
        if (status != GAMEDATA_STORAGE_OK) {
            return GAMEDATA_STORAGE_ERROR;
        } else {
        if (sfo_len > sizeof(sfo)) {
            sfo_len = sizeof(sfo);
        }
        if (gamedata_param_sfo_decode(sfo, sfo_len, out + GAMEDATA_STATGET_GET_PARAM) !=
            GAMEDATA_OK) {
            /* Decode failure: nothing else is written, per the approved design. */
            return GAMEDATA_BROKEN_PARAM;
        }
        store_be32(out + GAMEDATA_STATGET_IS_NEW_DATA, GAMEDATA_IS_NEW_DATA_NO);
        }
    }

    store_be32(out + GAMEDATA_STATGET_HDD_FREE_SIZE_KB, (uint32_t)saturate_i32(free_kb));
    build_path(out + GAMEDATA_STATGET_CONTENT_INFO_PATH,
               GAMEDATA_STATGET_CONTENT_INFO_PATH_BYTES, dir_name, 0);
    build_path(out + GAMEDATA_STATGET_GAME_DATA_PATH, GAMEDATA_STATGET_GAME_DATA_PATH_BYTES,
               dir_name, 1);
    store_be64(out + GAMEDATA_STATGET_ST_ATIME, (uint64_t)times[0]);
    store_be64(out + GAMEDATA_STATGET_ST_CTIME, (uint64_t)times[1]);
    store_be64(out + GAMEDATA_STATGET_ST_MTIME, (uint64_t)times[2]);
    store_be32(out + GAMEDATA_STATGET_SIZE_KB, (uint32_t)saturate_i32(size_kb));
    /* Documented deviation (report §5): the system-cache value is not determinable offline. */
    store_be32(out + GAMEDATA_STATGET_SYS_SIZE_KB, 0u);

    memset(state->dir_name, 0, sizeof(state->dir_name));
    memcpy(state->dir_name, dir_name, strlen(dir_name));
    state->dir_name_length = (uint32_t)strlen(dir_name);
    state->is_new = (int32_t)load_be32(out + GAMEDATA_STATGET_IS_NEW_DATA);
    state->size_kb = (int32_t)load_be32(out + GAMEDATA_STATGET_SIZE_KB);
    state->free_kb = (int32_t)load_be32(out + GAMEDATA_STATGET_HDD_FREE_SIZE_KB);
    return GAMEDATA_OK;
}

/* ------------------------------------------------------------------------------------ */
/* Callback-result application (§1.5)                                                   */
/* ------------------------------------------------------------------------------------ */

gamedata_status gamedata_apply_result(const gamedata_storage_port *port, gamedata_check *state,
                                      int32_t result, const uint8_t *set_param,
                                      gamedata_action *action)
{
    static const char *const param_sfo = "PARAM.SFO";
    uint8_t encoded[512];
    size_t encoded_len;
    gamedata_storage_status status;

    if (state == NULL || action == NULL) {
        return GAMEDATA_BAD_ARGUMENT;
    }
    action->action = GAMEDATA_ACTION_NONE;

    switch (result) {
    case 0: /* CELL_GAMEDATA_CBRESULT_OK (report §1.5, arm 0x7324) */
        if (set_param == NULL) {
            /* "if isNewData != 0 and setParam == NULL -> param-error path". */
            action->action = state->is_new != 0 ? GAMEDATA_ACTION_PARAM_ERROR
                                               : GAMEDATA_ACTION_NONE;
            return GAMEDATA_OK;
        }
        if (!port_usable(port)) {
            return GAMEDATA_STORAGE_ERROR;
        }
        encoded_len = gamedata_param_sfo_encode(set_param, encoded, sizeof(encoded));
        if (encoded_len == 0u) {
            action->action = GAMEDATA_ACTION_PARAM_ERROR;
            return GAMEDATA_OK;
        }
        status = port->create_dir(port->context, state->dir_name);
        if (status != GAMEDATA_STORAGE_OK) {
            return GAMEDATA_STORAGE_ERROR;
        }
        status = port->write_file(port->context, state->dir_name, param_sfo, encoded,
                                  encoded_len);
        if (status != GAMEDATA_STORAGE_OK) {
            return GAMEDATA_STORAGE_ERROR;
        }
        action->action = GAMEDATA_ACTION_CREATED_OR_UPDATED;
        return GAMEDATA_OK;

    case 1: /* CELL_GAMEDATA_CBRESULT_OK_CANCEL (arm 0x6CC0): no disk effect */
        return GAMEDATA_OK;

    case -1: /* ERR_NOSPACE */
    case -3: /* ERR_BROKEN */
    case -4: /* ERR_NODATA */
    case -5: /* ERR_INVALID */
        /* Arm 0x720C: the errDialog decision belongs to the adapter; never a dialog here. */
        action->action = GAMEDATA_ACTION_CBRESULT_ERROR;
        return GAMEDATA_OK;

    case -2: /* arm 0x73B0: CELL_GAMEDATA_ERROR_PARAM */
        action->action = GAMEDATA_ACTION_PARAM_ERROR;
        return GAMEDATA_OK;

    default: /* arm 0x7308: CELL_GAMEDATA_ERROR_CBRESULT */
        action->action = GAMEDATA_ACTION_UNKNOWN_RESULT;
        return GAMEDATA_OK;
    }
}

int32_t gamedata_guest_return(void)
{
    return 0;
}
