#ifndef SPURS_TASKSET_CREATE_H
#define SPURS_TASKSET_CREATE_H
/* Platform-free, instance-free builder for cellSpursCreateTasksetWithAttribute.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6):
 *
 *   cellSpursCreateTasksetWithAttribute NID 0xC10931CB
 *     continuation                        0x14D4C
 *     worker (create_taskset)             0x14A58
 *     register workload (AddWorkload)     0xA000  -> 0x9F0C -> 0x97E4
 *     event-flag setter on the taskset    0xDC5C
 *
 * The entry validates three things, the worker writes the CellSpursTaskset
 * image, and the workload registration reads back from the SAME attribute to
 * fill the CellSpursWorkloadAttribute. This unit owns all three: the validation
 * order and codes, the exact 6400-byte taskset image, and the mapping from the taskset
 * attribute onto the registration request the native workload-registration domain consumes.
 *
 * It does not touch guest memory, keeps no state, and makes no OS call: the caller supplies
 * the argument values (the twelve taskset-attribute fields and the two strings the firmware
 * dereferences) and receives byte buffers plus result codes.
 *
 * Every builder is `static inline` (PS3_SPURS_TSI, the same convention
 * native/domain/spurs_taskset_init.h uses), so a translation unit that includes this header
 * needs no extra object at link time. `spurs_taskset_create.c` instantiates the same functions
 * once as external symbols for the host test and the Switch compile check, so there is exactly
 * one implementation and no header/object divergence. */

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------ */
/* Sizes                                                                                 */
/* ------------------------------------------------------------------------------------ */

/* CellSpursTaskset: the firmware memsets 6400 (0x1900) bytes at 0x14B50 -- r5 = r30 =
 * attribute->taskset_size, which _cellSpursTasksetAttributeInitialize fixes at 6400
 * (row 7 of the attribute layout). */
#define PS3_SPURS_TASKSET_BYTES 6400u

/* The attributes the firmware reads to build the workload registration. */
#define PS3_SPURS_TASKSET_ATTRIBUTE_BYTES_LOCAL 512u

/* CellSpursWorkloadAttribute image the firmware builds on its own stack and never publishes;
 * replicated here because the native registration request is its projection. */
#define PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES 512u

/* `taskset_size > 0x28FF` selects the CellSpursTaskset2 continuation; the code that
 * path hands over to is NOT on inFAMOUS's path (attribute->taskset_size is 6400). */
#define PS3_SPURS_TASKSET2_SIZE_THRESHOLD 0x28FFu

/* The two policy-module values the firmware hands to AddWorkload:
 *   r2-0x7c4c -> 0x00023680 the PM image EA
 *   r2-0x7c48 -> 0x00001e40 the PM size
 * Both are static libsre slots decoded from the PRX R segment through TOC 0x39AB0.
 * The image itself is
 * SPU code that this runtime does not run, and the ready count stays 0, so registration never
 * raises it. The worker passes them in r6/r7, which 0xA000 forwards unchanged into the workload
 * attribute's +0x08 and +0x0C words. */
#define PS3_SPURS_TASKSET_PM_EA 0x00023680u
#define PS3_SPURS_TASKSET_PM_SIZE 0x1E40u

/* The name strings the firmware dereferences through r2:
 *   r2-0x7c3c -> 0x0001EDC8  "_cellSpursCreateTaskset"   (nameInstance, on the 0xA000 failure path)
 *   r2-0x7c44 -> 0x00030FA8 .. (the nameClass the two setters at 0x975C/0x979C store)
 * They are guest EAs the firmware COPIES into the workload attribute; nothing native reads the
 * bytes, so this unit carries the EAs and never a buffer. */
#define PS3_SPURS_TASKSET_NAME_INSTANCE_EA 0x0001EDC8u

/* `wkl_flag_wait_task = 0x80`. */
#define PS3_SPURS_TASKSET_WKL_FLAG_WAIT_TASK 0x80u

/* The `sdkVersion` the worker passes to _cellSpursWorkloadAttributeInitialize: the immediate 0x33
 * loaded into the high half with no low part, i.e. 0x00330000. */
#define PS3_SPURS_WORKLOAD_SDK_VERSION 0x00330000u

/* The workload-attribute revision the worker passes (the constant 1). */
#define PS3_SPURS_WORKLOAD_ATTRIBUTE_REVISION 1u

/* ------------------------------------------------------------------------------------ */
/* CellSpursTaskset field offsets                                   */
/* ------------------------------------------------------------------------------------ */

#define PS3_SPURS_CSTS_RUNNING 0x0000u          /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_READY 0x0010u            /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_PENDING_READY 0x0020u    /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_ENABLED 0x0030u          /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_SIGNALLED 0x0040u        /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_WAITING 0x0050u          /* be u32[4] -- zeroed by the memset      */
#define PS3_SPURS_CSTS_SPURS 0x0060u            /* be u64 <- arg 1              */
#define PS3_SPURS_CSTS_ARGS 0x0068u             /* be u64 <- attribute+0x08     */
#define PS3_SPURS_CSTS_ENABLE_CLEAR_LS 0x0070u  /* u8 <- attribute+0x27 != 0 */
#define PS3_SPURS_CSTS_X71 0x0071u              /* u8     -- zeroed by the memset         */
#define PS3_SPURS_CSTS_WKL_FLAG_WAIT_TASK 0x0072u
#define PS3_SPURS_CSTS_LAST_SCHEDULED 0x0073u   /* u8     -- zeroed by the memset         */
#define PS3_SPURS_CSTS_WID 0x0074u              /* be u32 <- the claimed wid    */
#define PS3_SPURS_CSTS_X78 0x0078u              /* be u64 -- zeroed by the memset         */
#define PS3_SPURS_CSTS_TASKINFO 0x0080u         /* 128 * 48 bytes -- zeroed by the memset */
#define PS3_SPURS_CSTS_EXC_HANDLER 0x1880u      /* be u64 -- zeroed by the memset         */
#define PS3_SPURS_CSTS_EXC_HANDLER_ARG 0x1888u  /* be u64 -- zeroed by the memset         */
#define PS3_SPURS_CSTS_SIZE 0x1890u             /* be u32 <- attribute->taskset_size      */
#define PS3_SPURS_CSTS_UNK2 0x1894u             /* be u32 -- zeroed by the memset         */
#define PS3_SPURS_CSTS_EVENT_FLAG_ID1 0x1898u   /* be u32 -- zeroed by the memset         */
#define PS3_SPURS_CSTS_EVENT_FLAG_ID2 0x189Cu   /* be u32 -- zeroed by the memset         */

/* The one CellSpursEventFlag word the firmware's event-flag setter touches
 * (0xDC5C reads the word at +0x70, then stores the flag
 * byte at 0x100 + wid*128 for wid <= 15). */
#define PS3_SPURS_CSTS_EVENT_FLAG_SET_WORD 0x0070u

/* ------------------------------------------------------------------------------------ */
/* Result codes                                                                          */
/* ------------------------------------------------------------------------------------ */

/* The values the firmware returns in r3, already sign-extended as the epilogues
 * produce them (for the entry and for the worker). */
#define PS3_SPURS_TASKSET_CREATE_OK 0u
#define PS3_SPURS_TASKSET_CREATE_NULL 0x80410911u      /* 0x14D24 (attr NULL) / 0x14AB8      */
#define PS3_SPURS_TASKSET_CREATE_ALIGN 0x80410910u     /* 0x14D50 (attr & 7) / 0x14B24       */
#define PS3_SPURS_TASKSET_CREATE_INVAL 0x80410902u     /* 0x14D78 (revision != 1)            */
#define PS3_SPURS_TASKSET_CREATE_AGAIN 0x80410901u     /* 0x14C9C (worker: no free wid)      */
#define PS3_SPURS_TASKSET_CREATE_STAT 0x8041090Fu      /* 0x14CE4 (worker: exception latched)*/

/* Domain status. Distinct from the guest r3: a refused call produces no image. */
typedef enum spurs_taskset_create_status {
    PS3_SPURS_TASKSET_CREATE_STATUS_OK = 0,          /* image valid, r3 = 0 */
    PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED = 1,     /* image invalid, r3 = the firmware code */
    PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT = 2 /* NULL output buffer or wrong capacities */
} spurs_taskset_create_status;

/* The seven policy-module errors 0xA000 collapses and the one the three
 * policies the worker itself synthesizes. Exposed so the adapter can apply the
 * collapse the firmware applies, instead of inventing one. */
typedef enum spurs_taskset_create_collapse {
    PS3_SPURS_TASKSET_CREATE_PASS = 0,  /* the worker returns the code unchanged        */
    PS3_SPURS_TASKSET_CREATE_AGAIN_ = 1,
    PS3_SPURS_TASKSET_CREATE_INVAL_ = 2,
    PS3_SPURS_TASKSET_CREATE_STAT_ = 3
} spurs_taskset_create_collapse;

#ifdef __cplusplus
extern "C" {
#endif

/* Linkage of every builder below: `static inline` by default (see the header comment), plain
 * extern definitions in spurs_taskset_create.c. */
#ifndef PS3_SPURS_TSC
#define PS3_SPURS_TSC static inline
#endif

/* ------------------------------------------------------------------------------------ */
/* The full taskset attribute, as the caller read it out of guest memory                 */
/* ------------------------------------------------------------------------------------ */

/* The twelve fields _cellSpursTasksetAttributeInitialize + cellSpursTasksetAttributeSetName
 * leave in CellSpursTasksetAttribute (g10 CONTRACT §A.4, g11 CONTRACT §A.4). The caller reads
 * each one and hands it over; this unit never dereferences anything.
 *
 * `enable_clear_ls_byte` is the raw u8 at attribute+0x27 -- the entry loads that byte
 * and the worker turns it into a 0/1 byte. It is NOT +0x24 (`enable_clear_ls` as a u32
 * would be at +0x24); the firmware reads the last byte of that word. */
typedef struct spurs_taskset_attribute {
    uint32_t revision;            /* +0x00 u32, checked by the entry   */
    uint64_t args;                /* +0x08 u64, becomes taskset->args           */
    unsigned char priority[8];    /* +0x10  u8[8], validated at 0xA0E8 and copied          */
    uint32_t max_contention;      /* +0x18  u32, becomes wklMaxContention                  */
    uint32_t name;                /* +0x1C  u32, the nameInstance the workload is named by */
    uint32_t taskset_size;        /* +0x20  u32, the memset length AND taskset->size       */
    unsigned char enable_clear_ls_byte; /* +0x27 u8, the last byte of the +0x24 word       */
} spurs_taskset_attribute;

/* The three arguments of cellSpursCreateTasksetWithAttribute:
 *   r3 spurs, r4 taskset, r5 attr -- each is the low 32 bits of its register. */
typedef struct spurs_taskset_create_request {
    uint32_t spurs;   /* arg 1, r3: the CellSpurs EA, copied to taskset+0x60 */
    uint32_t taskset; /* arg 2, r4: the CellSpursTaskset EA, the write target */
    uint32_t attr;    /* arg 3, r5: the attribute EA, validated then read      */
    /* The twelve fields of the object at `attr`, which the caller read before calling (nothing
     * is read here). Only used when the entry's own validation passes. */
    spurs_taskset_attribute attribute;
} spurs_taskset_create_request;

/* ------------------------------------------------------------------------------------ */
/* argument validation                                        */
/* ------------------------------------------------------------------------------------ */

/* The three gates, in the firmware's order, first failure wins:
 *   1. attr == 0 -> PS3_SPURS_TASKSET_CREATE_NULL
 *   2. attr & 7 != 0 -> PS3_SPURS_TASKSET_CREATE_ALIGN
 *   3. attr->revision != 1 -> PS3_SPURS_TASKSET_CREATE_INVAL
 *
 * Gate 3 reads attribute+0x00, i.e. it happens AFTER gates 1 and 2: a NULL or misaligned
 * attribute never faults on that read, which is why `request->attribute.revision` is only
 * consulted once gate 1 and gate 2 pass. `spurs` (r3) and `taskset` (r4) are NOT validated by
 * the entry; the worker validates them (row 0 of §A.4 below).
 *
 * Returns STATUS_OK with `*r3` = 0, or STATUS_REFUSED with `*r3` set to the firmware code.
 * BAD_ARGUMENT for a NULL `request` or `r3`. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_validate(
    const spurs_taskset_create_request *request, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TASKSET_CREATE_OK;
    if (request->attr == 0u) {
        code = PS3_SPURS_TASKSET_CREATE_NULL;
    } else if ((request->attr & 7u) != 0u) {
        code = PS3_SPURS_TASKSET_CREATE_ALIGN;
    } else if (request->attribute.revision != PS3_SPURS_WORKLOAD_ATTRIBUTE_REVISION) {
        /* a revision other than 1 is refused with 0x80410902. */
        code = PS3_SPURS_TASKSET_CREATE_INVAL;
    }
    *r3 = code;
    return code == PS3_SPURS_TASKSET_CREATE_OK ? PS3_SPURS_TASKSET_CREATE_STATUS_OK
                                               : PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED;
}

/* The two pointers the worker validates after the entry passed (CONTRACT §A.4 row 0):
 *   0x14B08 r4 == 0 -> 0x80410911 ; 0x14B10 r3 & 0x7F -> 0x80410910 ;
 *   0x14B2C r4 & 0x7F -> 0x80410910. Both are 128-byte alignment checks.
 * The firmware reaches this only after the entry's three gates, so a NULL/misaligned attr is
 * reported by ps3_spurs_taskset_create_validate, never here. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_validate_arguments(
    const spurs_taskset_create_request *request, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TASKSET_CREATE_OK;
    if (request->spurs == 0u || request->taskset == 0u) {
        code = PS3_SPURS_TASKSET_CREATE_NULL;
    } else if ((request->spurs & 0x7Fu) != 0u || (request->taskset & 0x7Fu) != 0u) {
        code = PS3_SPURS_TASKSET_CREATE_ALIGN;
    }
    *r3 = code;
    return code == PS3_SPURS_TASKSET_CREATE_OK ? PS3_SPURS_TASKSET_CREATE_STATUS_OK
                                               : PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED;
}

/* ------------------------------------------------------------------------------------ */
/* the CellSpursTaskset image                                 */
/* ------------------------------------------------------------------------------------ */

/* Big-endian store helper, identical in behaviour to the one in spurs_taskset_init.h. */
PS3_SPURS_TSC int ps3_spurs_taskset_create_put_be(uint8_t *out, size_t capacity, size_t offset,
    size_t width, uint64_t value)
{
    if (!out) return -1;
    if (width != 1u && width != 2u && width != 4u && width != 8u) return -1;
    if (offset > capacity || width > capacity - offset) return -1;
    for (size_t index = 0; index < width; ++index)
        out[offset + index] = (uint8_t)(value >> (8u * (width - 1u - index)));
    return 0;
}

/* The sign extension every firmware epilogue applies. */
PS3_SPURS_TSC uint64_t ps3_spurs_taskset_create_sign_extend(uint32_t code)
{
    return (uint64_t)(int64_t)(int32_t)code;
}

/* Byte 0x70 of the taskset, i.e. `enable_clear_ls`, as the worker computes it:
 *   the raw byte is attribute+0x27, zero-extended
 *   negating it gives 0 if the byte is 0, otherwise "negative" (bit 63 set for 0x01..0xFF)
 *   keeping the inverted sign bit gives 1 iff the byte was non-zero
 * So the stored byte is exactly `raw != 0`, i.e. a normalised boolean. */
PS3_SPURS_TSC unsigned char ps3_spurs_taskset_create_enable_clear_ls(unsigned char raw)
{
    return raw != 0u ? 1u : 0u;
}

/* Builds the exact 6400-byte CellSpursTaskset image and its result code.
 *
 * The worker's own pre-checks come first, in order, first failure wins:
 *   0a. spurs == 0 or taskset == 0           -> 0x80410911
 *   0b. spurs & 0x7F or taskset & 0x7F       -> 0x80410910
 * Then the ten stores, in the firmware's order:
 *   1  memset(taskset, 0, attribute->taskset_size)           [the whole image is zero]
 *   2  store args                       -> +0x68   (be u64)
 *   3  store enable_clear_ls            -> +0x70   (u8, normalised 0/1)
 *   4  store taskset_size               -> +0x1890 (be u32)
 *   5  store spurs                      -> +0x60   (be u64, zero-extended from r22)
 *   6  store 0x80                       -> +0x72   (wkl_flag_wait_task)
 *   7  store wid                        -> +0x74   (be u32)
 *   8  the 0xDC5C reservation store     -> +0x100 + wid*128, 8 bytes, 1
 * plus, from the same run, the workload registration and its two publication messages.
 * Step 8 is written by the caller through `ps3_spurs_taskset_create_flag_address` and
 * `ps3_spurs_taskset_create_flag_value_image`, after step 7, because it addresses the slot of
 * the wid the registration returned; its target is inside the 6400 bytes step 1 zeroed.
 *
 * This builder produces the OBJECT only: it performs steps 1-5 and 6 (the stores that do not
 * depend on the claimed wid) and leaves the wid slot at 0x74 zero, because the wid is not known
 * until the native registration claims it. `ps3_spurs_taskset_create_apply_wid` writes it in a
 * 4-byte image once the registration has produced one -- so the caller writes the wid exactly
 * as 0x14BE4 does, and only on success.
 *
 * Other consequences the image records, quoted from the listing:
 *   - `wkl_flag_wait_task = 0x80` at +0x72 happens on the SUCCESS path only (0x14BC8 sits in
 *     the `cr4 == 0` arm of 0x14BAC), so a failed registration must not write it. The builder
 *     returns that byte in `*wkl_flag_wait_byte_image` and the caller writes it only on success.
 *   - +0x1894 (`unk2`) stays 0: the only store there is 0x14E44 inside the CellSpursTaskset2
 *     continuation, which a 6400-byte taskset never enters (0x14DA0 compares
 *     attribute->taskset_size > 0x28FF).
 *   - step 8 (`taskset+0x100+wid*128 = 1`) runs on 0x14BE8 in the same arm as steps 6 and 7, and
 *     therefore only when the folded registration code is 0 or 0x80410910. It is part of this
 *     call's effect: the address lies inside the object this call zero-filled, so no other
 *     owner exists for it.
 *
 * `*r3` receives 0 on success. On refusal `out` is untouched. Returns STATUS_BAD_ARGUMENT when
 * `request`/`out`/`r3` is NULL or `capacity < PS3_SPURS_TASKSET_BYTES`. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_image(
    const spurs_taskset_create_request *request, uint8_t *out, size_t capacity,
    uint8_t *wkl_flag_wait_byte_image, uint32_t *r3)
{
    if (!request || !out || !r3 || capacity < PS3_SPURS_TASKSET_BYTES)
        return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    uint32_t code = 0;
    if (ps3_spurs_taskset_create_validate_arguments(request, &code) !=
        PS3_SPURS_TASKSET_CREATE_STATUS_OK) {
        *r3 = code;
        return PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED;
    }
    /* The zeroing call takes r3 = taskset, r4 = 0, r5 = r30 = attribute->taskset_size. The
     * length is the attribute's own field, and for a 6400-byte taskset it is the whole struct;
     * a caller that asked for a bigger buffer still gets exactly PS3_SPURS_TASKSET_BYTES zeroed
     * and nothing past it. */
    for (size_t index = 0; index < PS3_SPURS_TASKSET_BYTES; ++index) out[index] = 0u;
    /* args (r26 = r5 of the worker = attribute+0x08). */
    if (ps3_spurs_taskset_create_put_be(out, capacity, PS3_SPURS_CSTS_ARGS, 8u,
            request->attribute.args) != 0 ||
        /* the normalised enable_clear_ls byte. */
        ps3_spurs_taskset_create_put_be(out, capacity, PS3_SPURS_CSTS_ENABLE_CLEAR_LS, 1u,
            ps3_spurs_taskset_create_enable_clear_ls(request->attribute.enable_clear_ls_byte)) != 0 ||
        /* r30 = attribute->taskset_size. */
        ps3_spurs_taskset_create_put_be(out, capacity, PS3_SPURS_CSTS_SIZE, 4u,
            request->attribute.taskset_size) != 0 ||
        /* the CellSpurs EA (the low 32 bits of the spurs argument). */
        ps3_spurs_taskset_create_put_be(out, capacity, PS3_SPURS_CSTS_SPURS, 8u,
            request->spurs) != 0)
        return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    /* +0x74 (wid) stays 0 here: ps3_spurs_taskset_create_apply_wid writes it after the claim.
     * +0x72 (wkl_flag_wait_task) is handed back for the success path only. */
    if (wkl_flag_wait_byte_image)
        wkl_flag_wait_byte_image[0] = (uint8_t)PS3_SPURS_TASKSET_WKL_FLAG_WAIT_TASK;
    *r3 = PS3_SPURS_TASKSET_CREATE_OK;
    return PS3_SPURS_TASKSET_CREATE_STATUS_OK;
}

/* The 4-byte big-endian wid image 0x14BE4 writes at taskset+0x74
 * (the wid the registration produced).
 * Returns STATUS_OK and fills `out` (4 bytes) or STATUS_BAD_ARGUMENT. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_wid_image(uint32_t wid,
    uint8_t *out, size_t capacity)
{
    if (!out || capacity < 4u) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    return ps3_spurs_taskset_create_put_be(out, capacity, 0, 4u, wid) == 0
        ? PS3_SPURS_TASKSET_CREATE_STATUS_OK : PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
}

/* The 1-byte wkl_flag_wait_task image 0x14BD8 writes at taskset+0x72. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_flag_wait_image(
    uint8_t *out, size_t capacity)
{
    if (!out || capacity < 1u) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    out[0] = (uint8_t)PS3_SPURS_TASKSET_WKL_FLAG_WAIT_TASK;
    return PS3_SPURS_TASKSET_CREATE_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* 0xA000 / 0x9F0C / 0x97E4 -- the workload registration                   */
/* ------------------------------------------------------------------------------------ */

/* The registration projection of the SAME attribute, i.e. what 0x14B58..0x14B8C hands to
 * 0xA000 and what 0xA000's continuation copies into the 512-byte CellSpursWorkloadAttribute it
 * builds on its own stack (0xA11C..0xA144). The native workload-registration request is this
 * projection (evidence §D): the fields map one to one.
 *
 *   pm         = 0x00023680   (r6 = *(r2-0x7C4C), 0x14B5C)          policy module EA
 *   size       = 0x00001E40   (r7 = *(r2-0x7C48), 0x14B64)          policy module size
 *   data       = the TASKSET EA (the low 32 bits of the worker's taskset argument), the
 *                same value the worker stores at taskset+0x68 -- NOT attribute->args.
 *                attribute->args is the worker's r26 (saved on entry) and 0xA000 writes it
 *                into word +0x0C, while `data` is an independent store.
 *   priority[] = attribute+0x10  (r9 = r28, 0x14B78)                  8 bytes, validated
 *   min_cont = 8              (r10 = 8, 0x14B8C)                     literal, never attribute
 *   max_cont = attribute+0x18  (r8 of the entry)
 *   nameClass / nameInstance   -- see below
 *   hook / hook_argument       = 0 (0xA000 is passed only 10 arguments; the worker stores the
 *                                4-byte frame junk at r1+0x70 as the u64 whose low word is
 *                                zero, so hook and hookArg are both zero)
 *
 * The `max_contention` the entry passes is `r8 = *(u32*)(attr+0x18)`, and the
 * `min_contention` is the literal 8 even though the attribute has a max_contention
 * field only -- the firmware's own asymmetry, recorded not corrected.
 *
 * The two name pointers: the worker's success path calls 0x9F0C(spurs, &wid, attr) which
 * calls 0x97E4(spurs, &wid, pm, data, size, priority, min, max, nameClass, nameInstance,
 * hook, hookArg), and 0x9F0C reads those four from ITS attribute argument's +0x28..+0x37,
 * not from the taskset attribute. `nameClass` is the EA the two setters stored there
 * (read through *(r2-0x7C44)), `nameInstance` the EA at
 * attribute+0x1C (`cellSpursTasksetAttributeSetName`). This builder hands
 * both EAs through unchanged: nothing native dereferences them.
 */
typedef struct spurs_taskset_registration {
    uint32_t spurs;              /* the CellSpurs EA the claim runs on */
    uint64_t data;               /* the TASKSET EA (0x14B88 r8 = r21), zero-extended */
    unsigned char priority[8];   /* attribute+0x10, validated <= 0xF per byte */
    uint32_t min_contention;     /* the literal 8 */
    uint32_t max_contention;     /* attribute+0x18 */
    uint32_t policy_module;      /* PS3_SPURS_TASKSET_PM_EA */
    uint32_t size;               /* PS3_SPURS_TASKSET_PM_SIZE */
    uint32_t name_class;         /* attribute+0x28, carried verbatim */
    uint32_t name_instance;      /* attribute+0x1C, carried verbatim */
    uint32_t hook;               /* 0 */
    uint32_t hook_argument;      /* 0 */
} spurs_taskset_registration;

/* Builds the registration projection from the taskset attribute.
 *
 * Validation the worker performs before it builds this, first failure wins:
 *   - any priority byte > 0xF            -> 0x80410802 (0xA0E8..0xA0F0, then 0xA14C)
 *   - min_contention == 0                -> 0x80410802 (the native request rejects 0)
 * `min_contention` is the literal 8 here, so the second can never fire from this builder; it is
 * still applied because the native request validates it and this unit must not hand over a
 * request that request would reject. `max_contention` has no bound in the firmware and is
 * clamped to 8 by the registration domain, exactly as 0x97E4 does
 * (`maxContention > 8 ? 8 : maxContention`).
 *
 * `policy_module`/`size` are the decoded constants; `name_class` comes from the caller because
 * it lives at attribute+0x28, which this builder does not own (it is not part of the twelve
 * fields g10/g11 define), and `name_instance` is attribute+0x1C.
 *
 * Returns STATUS_OK with `*out` filled, or STATUS_REFUSED with `*r3` set. BAD_ARGUMENT for a
 * NULL pointer. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_registration(
    const spurs_taskset_create_request *request, uint32_t name_class,
    spurs_taskset_registration *out, uint32_t *r3)
{
    if (!request || !out || !r3) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TASKSET_CREATE_OK;
    for (unsigned index = 0; index < 8u; ++index)
        if (request->attribute.priority[index] > 0xFu) {
            /* a priority above 0xF is refused with 0x80410802. */
            code = 0x80410802u;
            break;
        }
    if (code != PS3_SPURS_TASKSET_CREATE_OK) {
        *r3 = code;
        return PS3_SPURS_TASKSET_CREATE_STATUS_REFUSED;
    }
    out->spurs = request->spurs;
    /* data = the taskset EA
     * the worker was given, not attribute->args. */
    out->data = request->taskset;
    for (unsigned index = 0; index < 8u; ++index) out->priority[index] = request->attribute.priority[index];
    out->min_contention = 8u;
    out->max_contention = request->attribute.max_contention;
    out->policy_module = PS3_SPURS_TASKSET_PM_EA;
    out->size = PS3_SPURS_TASKSET_PM_SIZE;
    out->name_class = name_class;
    out->name_instance = request->attribute.name;
    out->hook = 0u;
    out->hook_argument = 0u;
    *r3 = PS3_SPURS_TASKSET_CREATE_OK;
    return PS3_SPURS_TASKSET_CREATE_STATUS_OK;
}

/* 0xA000's continuation: the CellSpursWorkloadAttribute it builds, whose fields are the
 * registration projection. Exposed as an image because the firmware's own copy is what the
 * readback tests compare against. `*out` must hold
 * PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES bytes. Returns STATUS_OK or BAD_ARGUMENT. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_workload_attribute_image(
    const spurs_taskset_registration *registration, uint8_t *out, size_t capacity)
{
    if (!registration || !out || capacity < PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES)
        return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    for (size_t index = 0; index < PS3_SPURS_WORKLOAD_ATTRIBUTE_BYTES; ++index) out[index] = 0u;
    if (ps3_spurs_taskset_create_put_be(out, capacity, 0x00, 4u,
            PS3_SPURS_WORKLOAD_ATTRIBUTE_REVISION) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x04, 4u,
            PS3_SPURS_WORKLOAD_SDK_VERSION) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x08, 4u, registration->policy_module) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x0C, 4u, registration->size) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x10, 8u, registration->data) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x18, 8u,
            ((uint64_t)registration->priority[0] << 56) | ((uint64_t)registration->priority[1] << 48) |
            ((uint64_t)registration->priority[2] << 40) | ((uint64_t)registration->priority[3] << 32) |
            ((uint64_t)registration->priority[4] << 24) | ((uint64_t)registration->priority[5] << 16) |
            ((uint64_t)registration->priority[6] << 8) | (uint64_t)registration->priority[7]) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x20, 4u, registration->min_contention) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x24, 4u, registration->max_contention) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x28, 4u, registration->name_class) != 0 ||
        ps3_spurs_taskset_create_put_be(out, capacity, 0x2C, 4u, registration->name_instance) != 0)
        return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    return PS3_SPURS_TASKSET_CREATE_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* the seven-code collapse                          */
/* ------------------------------------------------------------------------------------ */

/* 0xA000 returns a POLICY_MODULE code (0x804108xx); the worker maps four of them onto the
 * TASK namespace before returning. The three measured at 0x14C74..0x14CF4 (the only ones the
 * worker reaches through the registration service it uses) are:
 *   0x80410802 -> 0x80410902   (0x14C78/0x14C7C -> 0x14CEC/0x14CF0)
 *   0x8041080F -> 0x8041090F   (0x14C84/0x14C88 -> 0x14CE0/0x14CE4)
 *   0x80410801 -> 0x80410901   (0x14C90/0x14C94 -> 0x14C98/0x14C9C)
 * and every other code passes through unchanged (i.e. the sign extension of
 * the code 0xA000 produced). 0x80410811 and 0x80410810 take the earlier arms: 0x14BB8 detects
 * 0x...0911 and 0x14BC4 fills in the event-flag/`wkl_flag_wait_task` fields before returning
 * that code, and the remaining POLICY codes fall to 0x14C74 and pass through.
 *
 * A return of 0 is the success arm: the wid store, `wkl_flag_wait_task` and the
 * event-flag setter all belong to it and to the 0x0910 arm only. */
PS3_SPURS_TSC spurs_taskset_create_collapse ps3_spurs_taskset_create_classify(uint32_t code)
{
    switch (code) {
    case 0x80410801u: return PS3_SPURS_TASKSET_CREATE_AGAIN_;
    case 0x80410802u: return PS3_SPURS_TASKSET_CREATE_INVAL_;
    case 0x8041080Fu: return PS3_SPURS_TASKSET_CREATE_STAT_;
    default: return PS3_SPURS_TASKSET_CREATE_PASS;
    }
}

/* The code the worker returns for a registration result, applying that collapse. */
PS3_SPURS_TSC uint32_t ps3_spurs_taskset_create_registration_code(uint32_t code)
{
    switch (ps3_spurs_taskset_create_classify(code)) {
    case PS3_SPURS_TASKSET_CREATE_AGAIN_: return PS3_SPURS_TASKSET_CREATE_AGAIN;
    case PS3_SPURS_TASKSET_CREATE_INVAL_: return PS3_SPURS_TASKSET_CREATE_INVAL;
    case PS3_SPURS_TASKSET_CREATE_STAT_: return PS3_SPURS_TASKSET_CREATE_STAT;
    default: return code;
    }
}

/* the two arms that publish the wid and `wkl_flag_wait_task` (and then set the
 * event flag) are `code == 0` and `code == 0x80410910`. Any other code skips all three. */
PS3_SPURS_TSC int ps3_spurs_taskset_create_publishes_workload(uint32_t code)
{
    return code == 0u || code == PS3_SPURS_TASKSET_CREATE_ALIGN;
}

/* ------------------------------------------------------------------------------------ */
/* 0xDC5C -- the event-flag store on the taskset                 */
/* ------------------------------------------------------------------------------------ */

/* The wid form 0xDC5C addresses: wid <= 15 -> `taskset + 0x100 + wid*128` (0xDD14/0xDD18);
 * wid > 15 -> `taskset + 0xB00 + (wid & 15)*128` (0xDD00/0xDD08), the reference's `wklFlag2`. */
#define PS3_SPURS_CSTS_WKL_FLAG_BASE 0x0100u
#define PS3_SPURS_CSTS_WKL_FLAG2_BASE 0x0B00u
#define PS3_SPURS_TASKSET_WKL_FLAG_STRIDE 128u

/* The guest address 0xDC5C stores to, from the wid the registration returned:
 *   wid <= 15  taskset + 0x100 + wid*128
 *   wid >  15  taskset + 0xB00 + (wid & 15)*128
 * r10 is the taskset the setter was called with (r6 = r21 = the taskset EA,
 * passed in the setter's r3).
 * Returns 0 when the address does not fit the guest's 32-bit space, which no task can reach
 * (a 6400-byte taskset object plus 0x8FF stays inside one 4 GiB window). */
PS3_SPURS_TSC int ps3_spurs_taskset_create_flag_address(uint32_t taskset, uint32_t wid,
    uint32_t *address)
{
    if (!address) return -1;
    const uint64_t base = wid <= 15u
        ? (uint64_t)taskset + PS3_SPURS_CSTS_WKL_FLAG_BASE
        : (uint64_t)taskset + PS3_SPURS_CSTS_WKL_FLAG2_BASE;
    const uint64_t offset = (wid <= 15u ? (uint64_t)wid : (uint64_t)(wid & 15u)) *
        PS3_SPURS_TASKSET_WKL_FLAG_STRIDE;
    if (base + offset > 0xFFFFFFFFu) return -1;
    *address = (uint32_t)(base + offset);
    return 0;
}

/* The 8-byte big-endian image 0xDC5C stores there: a doubleword, value 1.
 * The firmware does it through a
 * load-reserve/store-conditional pair because it must not clobber a flag another workload set; natively
 * the slot is inside the zeroed 6400-byte image this call just wrote, so the reservation always
 * commits and the result is this image. */
PS3_SPURS_TSC spurs_taskset_create_status ps3_spurs_taskset_create_flag_value_image(
    uint8_t *out, size_t capacity)
{
    if (!out || capacity < 8u) return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    if (ps3_spurs_taskset_create_put_be(out, capacity, 0, 8u, 1u) != 0)
        return PS3_SPURS_TASKSET_CREATE_STATUS_BAD_ARGUMENT;
    return PS3_SPURS_TASKSET_CREATE_STATUS_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* SPURS_TASKSET_CREATE_H */
