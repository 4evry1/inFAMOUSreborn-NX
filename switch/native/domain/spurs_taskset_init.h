#ifndef SPURS_TASKSET_INIT_H
#define SPURS_TASKSET_INIT_H
/* Platform-free, instance-free byte-image builders for the two pure struct-initialisation
 * SPURS entry points the game reaches in its taskset setup block.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6):
 *
 *   _cellSpursTasksetAttributeInitialize  NID 0x16394A4E
 *   _cellSpursEventFlagInitialize         NID 0x5EF96465
 *
 * Neither entry makes a syscall, starts SPU work, or keeps state: each validates its
 * arguments and writes one big-endian struct image into guest memory. This unit owns the
 * validation order, the error codes and the exact image; the adapter owns the guest
 * reads/writes and the r3 result.
 *
 * No OS calls, no globals, no guest-memory access: the caller supplies the argument
 * values (including the `priority` bytes it read from guest memory) and receives a byte
 * buffer plus a result code.
 *
 * Every builder is `static inline`, so a translation unit that includes this header needs no
 * extra object at link time. `spurs_taskset_init.c` instantiates the same functions once for
 * the host test and the Switch compile check, so there is exactly one implementation and no
 * header/object divergence. */

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------ */
/* Sizes                                                                                 */
/* ------------------------------------------------------------------------------------ */

/* CellSpursTasksetAttribute: the firmware memsets 512 (0x200) bytes, which
 * is the whole struct. */
#define PS3_SPURS_TASKSET_ATTRIBUTE_BYTES 512u

/* CellSpursEventFlag: the firmware memsets 128 (0x80) bytes. */
#define PS3_SPURS_EVENT_FLAG_BYTES 128u

/* The attribute's `priority` array is u8[8] and is copied byte for byte. */
#define PS3_SPURS_TASKSET_PRIORITY_BYTES 8u

/* `taskset_size` written at +0x20. */
#define PS3_SPURS_TASKSET_SIZE 6400u

/* `spuPort` on the success path, CELL_SPURS_EVENT_FLAG_INVALID_SPU_PORT. */
#define PS3_SPURS_EVENT_FLAG_SPU_PORT_INVALID 0xFFu

/* Limits the firmware enforces. */
#define PS3_SPURS_TASKSET_PRIORITY_MAX 0xFu   /* largest accepted priority byte  */
#define PS3_SPURS_MAX_WORKLOAD2 32u           /* workload-id bound (ids above 0x1F are refused) */
#define PS3_SPURS_EVENT_FLAG_LAST 3u          /* largest direction value */
#define PS3_SPURS_EVENT_FLAG_CLEAR_LAST 1u    /* largest clear_mode value */

/* Pointer alignment masks. The attribute is checked against 8;
 * the three event-flag pointers
 * are checked against 128. */
#define PS3_SPURS_TASKSET_ATTRIBUTE_ALIGN 8u
#define PS3_SPURS_EVENT_FLAG_ALIGN 128u

/* ------------------------------------------------------------------------------------ */
/* Result codes                                                                          */
/* ------------------------------------------------------------------------------------ */

/* The values the firmware returns in r3, already sign-extended as the epilogues
 * produce them. Every builder below writes its code into `*r3` (when non-NULL)
 * and returns a domain status describing whether the image is valid. */
#define PS3_SPURS_TASKSET_INIT_OK 0u
#define PS3_SPURS_TASKSET_INIT_NULL 0x80410911u
#define PS3_SPURS_TASKSET_INIT_ALIGN 0x80410910u         /* alignment refusal        */
#define PS3_SPURS_TASKSET_INIT_INVAL 0x80410902u         /* invalid argument        */
/* g11/g12: the event-flag attach's two extra gate codes. */
#define PS3_SPURS_EVENT_FLAG_ATTACH_NO_WAIT 0x80410909u    /* direction not accepted        */
#define PS3_SPURS_EVENT_FLAG_ATTACH_SPU_PORT 0x8041090Fu   /* spu_port not 0xFF        */

/* Domain status. Distinct from the guest r3: a refused call produces no image. */
typedef enum spurs_taskset_init_status {
    PS3_SPURS_TASKSET_INIT_STATUS_OK = 0,      /* image valid, r3 = 0 */
    PS3_SPURS_TASKSET_INIT_STATUS_REFUSED = 1, /* image invalid, r3 = the firmware code */
    PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT = 2, /* NULL output buffer or wrong capacities */
    /* g12c, event-flag attach only: every gate passed and the argument word at F+0x74 is readable, so the
     * caller must now run the firmware's continuation: create the lv2 event queue
     * with PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_*, connect it through the SPURS attach domain in DYNAMIC port mode,
     * then write F+0x7C and F+0x0C. This unit cannot do that itself (it is platform-free and owns no lv2
     * registry, no SPURS runtime and no guest memory): it hands the exact parameters over and names the status.
     * Replaces g11/g12's PS3_SPURS_EVENT_FLAG_ATTACH_UNSUPPORTED, whose blocker g12b refuted. */
    PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE = 3
} spurs_taskset_init_status;

/* ------------------------------------------------------------------------------------ */
/* Struct field offsets                                            */
/* ------------------------------------------------------------------------------------ */

/* CellSpursTasksetAttribute, big-endian guest fields. Everything else in the 512 bytes is
 * zero. */
#define PS3_SPURS_TSATTR_REVISION 0x00u      /* u32 <- arg 2 */
#define PS3_SPURS_TSATTR_SDK_VERSION 0x04u   /* u32 <- arg 3 */
#define PS3_SPURS_TSATTR_ARGS 0x08u          /* u64 <- arg 4 */
#define PS3_SPURS_TSATTR_PRIORITY 0x10u      /* u8[8] copied verbatim from arg 5 */
#define PS3_SPURS_TSATTR_MAX_CONTENTION 0x18u /* u32 <- arg 6 */
#define PS3_SPURS_TSATTR_NAME 0x1Cu          /* u32 0 (zeroed by the memset)   */
#define PS3_SPURS_TSATTR_TASKSET_SIZE 0x20u  /* u32 6400 */
#define PS3_SPURS_TSATTR_ENABLE_CLEAR_LS 0x24u /* u32 0 (zeroed by the memset) */

/* CellSpursEventFlag, big-endian guest fields. Everything else is zero. */
#define PS3_SPURS_EVFLAG_SPU_PORT 0x0Cu  /* u8  0xFF */
#define PS3_SPURS_EVFLAG_IS_IWL 0x0Du    /* u8  1 on the taskset==NULL branch only */
#define PS3_SPURS_EVFLAG_DIRECTION 0x0Eu /* u8  <- arg 5 */
#define PS3_SPURS_EVFLAG_CLEAR_MODE 0x0Fu /* u8 <- arg 4 */
#define PS3_SPURS_EVFLAG_TASKSET_PTR 0x74u /* the word the event-flag attach reads        */
#define PS3_SPURS_EVFLAG_ADDR 0x70u      /* u64: taskset EA, or spurs EA when taskset==NULL */
/* g12 §C.2: the two fields the attach's successful direction-1 path writes.
 * Named so a later slice that serves the passage past the gates cannot silently retarget them. */
#define PS3_SPURS_EVFLAG_QUEUE_ID 0x7Cu  /* u32 <- the lv2 queue id the helper created */
#define PS3_SPURS_EVFLAG_PORT_ID 0x78u   /* u32 <- direction 3 only, never taken */

/* g11 additions, the g12 extension, g12b's corrections and g12c's dynamic connect.
 *
 *   taskset attribute name setter   NID 0x652B70E2
 *   event-flag attach               NID 0x87630976
 *
 * The name setter is a leaf that stores arg 2 verbatim at attribute+0x1C
 * and never dereferences it: the firmware stores the POINTER, it does not copy the
 * string, and it reads no guest byte at all.
 *
 * The event-flag attach validates its four gates and then names the continuation it hands over
 * (PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE): lv2 128 with the decoded packet, the SPURS attach in
 * DYNAMIC port mode, the two guest writes. g11/g12 stopped there honestly because the packet was
 * undecoded and the connect looked unsupported; g12b decoded the packet (it is the string pointer
 * `*(r2-0x7BF0)` = "_spuEvF") and g12c decoded the connect (`0xAF2C`'s dynamic branch: mask
 * 0xFFFFFFFFFFFF0000 over ports 16..63, one connect syscall, the kernel choosing). Both are now served, so
 * the stop is gone. */

/* Attribute offsets the name setter writes and the event flag reads. */
#define PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES 4u /* a u32 EA */

/* The event-flag attach's alignment is the event flag's. */
#define PS3_SPURS_EVENT_FLAG_ATTACH_ALIGN PS3_SPURS_EVENT_FLAG_ALIGN
/* The attach accepts exactly these two directions (CELL_SPURS_EVENT_FLAG_OR = 1,
 * CELL_SPURS_EVENT_FLAG_ANY2ANY = 3); every other value fails with 0x80410909. */
#define PS3_SPURS_EVENT_FLAG_ATTACH_OR 1u
#define PS3_SPURS_EVENT_FLAG_ATTACH_ANY2ANY 3u

/* The firmware reads one word from the taskset, not the event flag, and only on the
 * `taskset != NULL` branch: the taskset's workload id
 * (CellSpursTaskset::wid, spurs_taskset.h CSTS_WID). The caller reads it; the contract below
 * says the limit is 0x1F. */
#define PS3_SPURS_TASKSET_WID_OFFSET 0x74u

#ifdef __cplusplus
extern "C" {
#endif

/* Linkage of every builder below. The default is `static inline`, so including this header
 * never creates an undefined symbol at link time; spurs_taskset_init.c redefines it to plain
 * `extern`-linked definitions, so the same code is also available as an object. */
#ifndef PS3_SPURS_TSI
#define PS3_SPURS_TSI static inline
#endif

/* ------------------------------------------------------------------------------------ */
/* Internal helpers (part of this unit's interface, not the firmware ABI)                 */
/* ------------------------------------------------------------------------------------ */

/* Writes `width` (1, 2, 4 or 8) big-endian bytes of `value` at `offset`. Returns 0 when the
 * span fits inside `capacity`, -1 otherwise (and then writes nothing). */
PS3_SPURS_TSI int ps3_spurs_taskset_init_put_be(uint8_t *out, size_t capacity, size_t offset,
    size_t width, uint64_t value)
{
    if (!out) return -1;
    if (width != 1u && width != 2u && width != 4u && width != 8u) return -1;
    if (offset > capacity || width > capacity - offset) return -1;
    for (size_t index = 0; index < width; ++index)
        out[offset + index] = (uint8_t)(value >> (8u * (width - 1u - index)));
    return 0;
}

/* The firmware's sign extension of a result code (the same in the attribute and the
 * event-flag entries): ERROR codes come back as
 * 0xFFFFFFFF8....... in the 64-bit r3. 0 stays 0. */
PS3_SPURS_TSI uint64_t ps3_spurs_taskset_init_sign_extend(uint32_t code)
{
    return (uint64_t)(int64_t)(int32_t)code;
}

/* ------------------------------------------------------------------------------------ */
/* _cellSpursTasksetAttributeInitialize                                                  */
/* ------------------------------------------------------------------------------------ */

/* The six arguments the entry reads. The caller resolves the priority
 * pointer to bytes and passes the u64 `priority_value` it read; the firmware masks that
 * pointer to 32 bits, so only the eight bytes take part in validation and copy. */
typedef struct spurs_taskset_attribute_request {
    uint32_t attribute;      /* arg 1, r3: guest EA of the attribute struct */
    uint32_t revision;       /* arg 2, r4 */
    uint32_t sdk_version;    /* arg 3, r5 */
    uint64_t args;           /* arg 4, r6 (64-bit) */
    uint64_t priority_value; /* arg 5, r7: the 8 bytes at the priority EA, big-endian */
    uint32_t max_contention; /* arg 6, r8 */
} spurs_taskset_attribute_request;

/* Builds the 512-byte CellSpursTasksetAttribute image and the firmware result code.
 *
 * Validation order, first failure wins:
 *   1. attribute == 0                -> PS3_SPURS_TASKSET_INIT_NULL
 *   2. attribute % 8 != 0            -> PS3_SPURS_TASKSET_INIT_ALIGN
 *   3. any priority byte in the 8-byte value > 0xF
 *                                    -> PS3_SPURS_TASKSET_INIT_INVAL
 *
 * On success `out` holds exactly the firmware's 512 bytes and `*r3` is 0.
 * On refusal `out` is left untouched and `*r3` holds the firmware code. `*r3` may be NULL.
 *
 * Returns STATUS_OK, STATUS_REFUSED, or STATUS_BAD_ARGUMENT when `request`/`out` is NULL or
 * `capacity < PS3_SPURS_TASKSET_ATTRIBUTE_BYTES`. Nothing is written past `capacity`. */
PS3_SPURS_TSI spurs_taskset_init_status ps3_spurs_taskset_attribute_init(
    const spurs_taskset_attribute_request *request, uint8_t *out, size_t capacity, uint32_t *r3)
{
    if (!request || !out || capacity < PS3_SPURS_TASKSET_ATTRIBUTE_BYTES)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    /* the NULL test precedes the alignment test. */
    uint32_t code = PS3_SPURS_TASKSET_INIT_OK;
    if (request->attribute == 0u) {
        code = PS3_SPURS_TASKSET_INIT_NULL;
    } else if ((request->attribute & (PS3_SPURS_TASKSET_ATTRIBUTE_ALIGN - 1u)) != 0u) {
        /* attribute & 7. */
        code = PS3_SPURS_TASKSET_INIT_ALIGN;
    } else {
        /* eight iterations, each comparing one byte with 0xF. The caller hands
         * the bytes over big-endian, so byte i is element 7-i of the value. */
        for (unsigned index = 0; index < PS3_SPURS_TASKSET_PRIORITY_BYTES; ++index) {
            const unsigned shift = 8u * (PS3_SPURS_TASKSET_PRIORITY_BYTES - 1u - index);
            if (((request->priority_value >> shift) & UINT64_C(0xFF)) >
                PS3_SPURS_TASKSET_PRIORITY_MAX) {
                code = PS3_SPURS_TASKSET_INIT_INVAL;
                break;
            }
        }
    }
    if (r3) *r3 = code;
    if (code != PS3_SPURS_TASKSET_INIT_OK) return PS3_SPURS_TASKSET_INIT_STATUS_REFUSED;

    /* The image, field by field. The firmware memsets 512 bytes first
     * and then stores the six fields; zero-filling and then storing the same six fields is
     * byte-identical, because every byte the memset writes and the stores do not overwrite is
     * zero in both. */
    for (size_t index = 0; index < PS3_SPURS_TASKSET_ATTRIBUTE_BYTES; ++index) out[index] = 0u;
    if (ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_REVISION, 4u,
            request->revision) != 0 ||
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_SDK_VERSION, 4u,
            request->sdk_version) != 0 ||
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_ARGS, 8u,
            request->args) != 0 ||
        /* memcpy(attr+0x10, priority, 8): byte for byte. */
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_PRIORITY, 8u,
            request->priority_value) != 0 ||
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_MAX_CONTENTION, 4u,
            request->max_contention) != 0 ||
        /* the task-set size constant 6400 is stored at +0x20. */
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_TSATTR_TASKSET_SIZE, 4u,
            PS3_SPURS_TASKSET_SIZE) != 0)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    /* +0x1C (name) and +0x24 (enable_clear_ls) stay at the memset's zero. */
    return PS3_SPURS_TASKSET_INIT_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* _cellSpursEventFlagInitialize                                                          */
/* ------------------------------------------------------------------------------------ */

/* The five arguments the entry reads. The game takes the `taskset == 0`
 * branch, which still requires `spurs` to be 128-byte aligned. */
typedef struct spurs_event_flag_request {
    uint32_t spurs;      /* arg 1, r3: CellSpurs instance EA (0 is accepted with a taskset) */
    uint32_t taskset;    /* arg 2, r4: 0 for the branch the game takes */
    uint32_t event_flag; /* arg 3, r5: guest EA of the struct to initialise */
    uint32_t clear_mode; /* arg 4, r6 */
    uint32_t direction;  /* arg 5, r7 */
    /* The `wid` word at taskset+0x74, which the firmware reads only when taskset != 0
     * The caller supplies it; it is ignored when taskset == 0. */
    uint32_t taskset_wid;
} spurs_event_flag_request;

/* Builds the 128-byte CellSpursEventFlag image and the firmware result code.
 *
 * Validation order, first failure wins:
 *   1. (taskset != 0 and event_flag == 0) or (taskset == 0 and (spurs == 0 or
 *      event_flag == 0))                          -> PS3_SPURS_TASKSET_INIT_NULL
 *   2. spurs % 128 != 0, or taskset % 128 != 0, or event_flag % 128 != 0
 *                                                 -> PS3_SPURS_TASKSET_INIT_ALIGN
 *   3. taskset != 0 and taskset_wid > 0x1F       -> PS3_SPURS_TASKSET_INIT_INVAL
 *   4. direction > 3                               -> PS3_SPURS_TASKSET_INIT_INVAL
 *   5. clear_mode > 1                              -> PS3_SPURS_TASKSET_INIT_INVAL
 *
 * `spurs` is checked unconditionally at step 2, exactly as the firmware does (the check is
 * reached on both branches), so `spurs == 0` combined with `taskset == 0` is refused at
 * step 1 before the alignment test.
 *
 * On success `out` holds exactly the firmware's 128 bytes and `*r3` is 0.
 * On refusal `out` is left untouched and `*r3` holds the firmware code. `*r3` may be NULL.
 *
 * Returns STATUS_OK, STATUS_REFUSED, or STATUS_BAD_ARGUMENT when `request`/`out` is NULL or
 * `capacity < PS3_SPURS_EVENT_FLAG_BYTES`. Nothing is written past `capacity`. */
PS3_SPURS_TSI spurs_taskset_init_status ps3_spurs_event_flag_init(
    const spurs_event_flag_request *request, uint8_t *out, size_t capacity, uint32_t *r3)
{
    if (!request || !out || capacity < PS3_SPURS_EVENT_FLAG_BYTES)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    const int taskset_present = request->taskset != 0u;
    const uint32_t mask = PS3_SPURS_EVENT_FLAG_ALIGN - 1u;
    uint32_t code = PS3_SPURS_TASKSET_INIT_OK;
    /* The branch on taskset presence selects the taskset==NULL argument path; the code that follows is
     * the three NULL tests. cr4 is taken once and preserved across the frame. */
    if (taskset_present ? request->event_flag == 0u
                        : (request->spurs == 0u || request->event_flag == 0u)) {
        code = PS3_SPURS_TASKSET_INIT_NULL;
    } else if ((request->spurs & mask) != 0u || (request->taskset & mask) != 0u ||
               (request->event_flag & mask) != 0u) {
        /* the three event-flag pointers are tested against 0x7F; any misalignment yields 0x80410910. */
        code = PS3_SPURS_TASKSET_INIT_ALIGN;
    } else if (taskset_present && request->taskset_wid > (PS3_SPURS_MAX_WORKLOAD2 - 1u)) {
        /* a taskset wid above 0x1F is refused. */
        code = PS3_SPURS_TASKSET_INIT_INVAL;
    } else if (request->direction > PS3_SPURS_EVENT_FLAG_LAST ||
               request->clear_mode > PS3_SPURS_EVENT_FLAG_CLEAR_LAST) {
        /* direction and clear_mode share the refusal exit. */
        code = PS3_SPURS_TASKSET_INIT_INVAL;
    }
    if (r3) *r3 = code;
    if (code != PS3_SPURS_TASKSET_INIT_OK) return PS3_SPURS_TASKSET_INIT_STATUS_REFUSED;

    /* The image, field by field: memset 128, then
     *   +0x0E direction, +0x0C 0xFF, +0x0F clear_mode,
     *   and either (+0x70 taskset EA) or (+0x70 spurs EA, +0x0D isIwl = 1). */
    for (size_t index = 0; index < PS3_SPURS_EVENT_FLAG_BYTES; ++index) out[index] = 0u;
    if (ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_DIRECTION, 1u,
            request->direction) != 0 ||
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_SPU_PORT, 1u,
            PS3_SPURS_EVENT_FLAG_SPU_PORT_INVALID) != 0 ||
        ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_CLEAR_MODE, 1u,
            request->clear_mode) != 0)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    if (taskset_present) {
        /* addr = taskset EA, isIwl left 0 by the memset. */
        if (ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_ADDR, 8u,
                request->taskset) != 0)
            return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    } else {
        /* addr = spurs EA, isIwl = 1. */
        if (ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_ADDR, 8u,
                request->spurs) != 0 ||
            ps3_spurs_taskset_init_put_be(out, capacity, PS3_SPURS_EVFLAG_IS_IWL, 1u, 1u) != 0)
            return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    }
    return PS3_SPURS_TASKSET_INIT_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* 0x652B70E2 -- taskset attribute name setter                             */
/* ------------------------------------------------------------------------------------ */

/* The two arguments the leaf reads. `name` is a guest EA, not a buffer: the
 * firmware stores it, so the caller must NOT read the string (the entry has
 * no guest read at all). */
typedef struct spurs_taskset_attribute_name_request {
    uint32_t attribute; /* arg 1, r3: the attribute EA the init built */
    uint32_t name;      /* arg 2, r4: the name string EA, stored verbatim */
} spurs_taskset_attribute_name_request;

/* Builds the 4-byte image the setter stores and the firmware result code.
 *
 * Validation order, first failure wins; measured, not assumed:
 *   1. attribute == 0 -> PS3_SPURS_TASKSET_INIT_NULL
 *   2. name == 0 -> PS3_SPURS_TASKSET_INIT_NULL
 *   3. attribute % 8 != 0 -> PS3_SPURS_TASKSET_INIT_ALIGN
 * Step 2 precedes step 3 in the listing, so an attribute that is both misaligned and carries a
 * NULL name returns 0x80410911, not 0x80410910.
 *
 * On success `out` holds the 4 big-endian bytes of `name` and `*r3` is 0; the caller writes
 * them at attribute+0x1C. On refusal `out` is left untouched. `*r3` may be NULL.
 *
 * Returns STATUS_OK, STATUS_REFUSED, or STATUS_BAD_ARGUMENT when `request`/`out` is NULL or
 * `capacity < PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES`. */
PS3_SPURS_TSI spurs_taskset_init_status ps3_spurs_taskset_attribute_set_name(
    const spurs_taskset_attribute_name_request *request, uint8_t *out, size_t capacity,
    uint32_t *r3)
{
    if (!request || !out || capacity < PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TASKSET_INIT_OK;
    if (request->attribute == 0u) {
        /* a NULL attribute. */
        code = PS3_SPURS_TASKSET_INIT_NULL;
    } else if (request->name == 0u) {
        /* A NULL name: r3 still holds 0x80410911
         * loaded on entry, so a NULL name reports the same code as a NULL
         * attribute. */
        code = PS3_SPURS_TASKSET_INIT_NULL;
    } else if ((request->attribute & (PS3_SPURS_TASKSET_ATTRIBUTE_ALIGN - 1u)) != 0u) {
        /* attribute & 7. */
        code = PS3_SPURS_TASKSET_INIT_ALIGN;
    }
    if (r3) *r3 = code;
    if (code != PS3_SPURS_TASKSET_INIT_OK) return PS3_SPURS_TASKSET_INIT_STATUS_REFUSED;
    /* the POINTER, big-endian, 4 bytes. */
    if (ps3_spurs_taskset_init_put_be(out, capacity, 0, PS3_SPURS_TASKSET_ATTRIBUTE_NAME_BYTES,
            request->name) != 0)
        return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    return PS3_SPURS_TASKSET_INIT_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* 0x87630976 -- event-flag attach to an lv2 event queue            */
/* ------------------------------------------------------------------------------------ */

/* Everything the four gates read, plus the argument word the
 * firmware reads past them. The caller reads the three one-byte fields and, on the isIwl path, the u32 at
 * F+0x74; nothing else takes part. */
typedef struct spurs_event_flag_attach_request {
    uint32_t event_flag;   /* arg 1, r3: the object to attach */
    uint8_t direction;     /* +0x0E, compared as a signed byte */
    uint8_t spu_port;      /* +0x0C */
    uint8_t is_iwl;        /* +0x0D, selects the addr source */
    /* F+0x74, read only on the isIwl branch. On that branch it is the CellSpurs EA that owns the
     * attach; 0 is passed through to the continuation unchanged, which then has no owner (a STOP there). */
    uint32_t addr;
} spurs_event_flag_attach_request;

/* The continuation's parameters, decoded from the helper's queue-create call:
 *
 *   +0x00 protocol  1  PS3_SYNC_FIFO
 *   +0x04 type      1  PS3_PPU_QUEUE
 *   +0x08 name      "_spuEvF"            (from *(r2-0x7BF0) = 0x20050, last byte forced to 0)
 *   ipc key         0
 *   ring depth      1
 * Every field is accepted by ps3_lv2_queue_create (size 1 in 1..127, protocol 1, type 1, key 0, name carried
 * verbatim), so the create is representable (g12b §3.2). */
#define PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_PROTOCOL 1u     /* PS3_SYNC_FIFO */
#define PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_TYPE 1u         /* PS3_PPU_QUEUE */
#define PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_IPC_KEY 0u
#define PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_DEPTH 1
/* The 8 name bytes as lv2 stores them (big-endian u64; the firmware forces the 8th to NUL at 0xB294). */
#define PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_NAME UINT64_C(0x5F73707545764600)   /* "_spuEvF\0" */

/* The error collapse the attach applies to the helper's code:
 * The rule isolates bits 16..19; when that nibble is 0x41 the code becomes
 * 0x80410900 | (code & 0xFF), else the raw code is returned. In practice no code 0xAF2C can produce for
 * inFAMOUS has that nibble, so every reachable failure passes through unchanged -- but the rule is stated
 * and applied here so the adapter cannot invent a different mapping. */
PS3_SPURS_TSI uint32_t ps3_spurs_event_flag_attach_collapse(uint32_t code)
{
    if (code == 0u) return 0u;
    if (((code >> 16) & 0xFu) != 0x4u) return code;
    return 0x80410900u | (code & 0xFFu);
}

/* Validates the four gates the firmware applies before it reaches the runtime call.
 *
 * Order, first failure wins:
 *   1. event_flag == 0 -> 0x80410911
 *   2. event_flag % 128 != 0 -> 0x80410910
 *   3. direction not in {1, 3} -> 0x80410909
 *   4. spu_port != 0xFF -> 0x8041090F
 *
 * Returns STATUS_REFUSED with `*r3` set to the firmware code for gates 1-4. When every gate passes it
 * returns PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE and leaves `*r3` at 0: the caller must run the continuation
 * with PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE_* and `request->addr` as the CellSpurs, write F+0x7C (the queue id)
 * and F+0x0C (the bound port) in that order, and return
 * ps3_spurs_event_flag_attach_collapse(whatever the continuation returned).
 *
 * The gates and both writes, the packet (success is 0) and the dynamic connect are what
 * g11's `UNSUPPORTED` stop was waiting for.
 *
 * `*r3` may be NULL. `request` may not: a NULL request is BAD_ARGUMENT. */
PS3_SPURS_TSI spurs_taskset_init_status ps3_spurs_event_flag_attach(
    const spurs_event_flag_attach_request *request, uint32_t *r3)
{
    if (!request) return PS3_SPURS_TASKSET_INIT_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TASKSET_INIT_OK;
    if (request->event_flag == 0u) {
        code = PS3_SPURS_TASKSET_INIT_NULL;
    } else if ((request->event_flag & (PS3_SPURS_EVENT_FLAG_ATTACH_ALIGN - 1u)) != 0u) {
        code = PS3_SPURS_TASKSET_INIT_ALIGN;
    } else if (request->direction != PS3_SPURS_EVENT_FLAG_ATTACH_OR &&
               request->direction != PS3_SPURS_EVENT_FLAG_ATTACH_ANY2ANY) {
        /* A signed-byte extension makes the comparison SIGNED: a byte with bit 7 set is negative and
         * therefore unequal to both +1 and +3. */
        code = PS3_SPURS_EVENT_FLAG_ATTACH_NO_WAIT;
    } else if (request->spu_port != PS3_SPURS_EVENT_FLAG_SPU_PORT_INVALID) {
        code = PS3_SPURS_EVENT_FLAG_ATTACH_SPU_PORT;
    }
    if (code != PS3_SPURS_TASKSET_INIT_OK) {
        if (r3) *r3 = code;
        return PS3_SPURS_TASKSET_INIT_STATUS_REFUSED;
    }
    if (r3) *r3 = 0u;
    return PS3_SPURS_EVENT_FLAG_ATTACH_QUEUE;
}

#ifdef __cplusplus
}
#endif

#endif /* SPURS_TASKSET_INIT_H */
