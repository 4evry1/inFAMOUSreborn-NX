#ifndef SPURS_LFQUEUE_ATTACH_H
#define SPURS_LFQUEUE_ATTACH_H
/* Platform-free, instance-free contract for the cellSpurs entry inFAMOUS reaches immediately after
 * _cellSpursLFQueueInitialize returns 0:
 *
 *   cellSpursLFQueueAttachLv2EventQueue   NID 0x1656D49F   (name recovered)
 *
 * Behaviour taken from the firmware library libsre (decrypted module SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6).
 *
 * What the entry does, in one paragraph. Its single argument is a 128-byte CellSyncLFQueue /
 * CellSpursLFQueue line that _cellSpursLFQueueInitialize (NID 0x011EE38B) has already
 * built. On that line's `+0x74` word it finds its owner, resolves that to
 * the CellSpurs EA `0xAF2C` connects to (`owner & ~0xF` when the low nibble is set, otherwise the
 * taskset's own `+0x64`), and then takes one of two shapes selected by the line's
 * `+0x24` VERSION word, which is the DIRECTION `_cellSpursLFQueueAttachLv2EventQueue`'s predecessor
 * stored there:
 *
 *   `+0x24 == 3`  the SPURS shape, iterations = 2
 *                 each iteration creates ONE lv2 event queue (protocol 1,
 *                 type 1, name 0, key 0, depth 1) and attaches it to the group (through the
 *                 0xAF2C attach call, keyed): one connect syscall (251) with the mask `1 << 15`, SPU port 15, NOT the dynamic mask.
 *                 Then it stores 0x78/0x7C/0x20/0x21/0x28/0x30/0x50 from its own stack frame.
 *
 *   `+0x24 != 3`  the CellSyncLFQueue shape, iterations = 1
 *                 the queue it attaches is the one ALREADY RECORDED at line `+0x20`, so it creates no queue. It does one
 *                 connect syscall (251) with the SAME mask `1 << 15` and then writes the chosen port to `+0x20`
 *                 and the attach result to `+0x78`, with a memory barrier between.
 *
 * Both shapes then run the same unwind-and-report block on the iteration count
 * and only publish on the LAST iteration.
 *
 * **inFAMOUS's line is the second shape**, because g14b established that the line is rebuilt from
 * zero on every run and that `_cellSpursLFQueueInitialize` stores `2` into the STATE word at
 * `+0x2C`, never into `+0x24`... but this entry reads `+0x24`, and the fourth argument of the game's
 * `_cellSpursLFQueueInitialize` call is the direction `3`. The recovered contract fixes the
 * byte inFAMOUS actually has at `+0x24` and this header follows it.
 *
 * What this unit owns: the argument gates and their exact codes, the owner resolution, the shape
 * selector and the iteration count, the two buffer images the entry itself assembles (the queue-create
 * request packet and the connect/port bookkeeping), the exact guest writes and their order, and the
 * request struct a native adapter hands to the lv2 registry + SPURS attach service.
 *
 * What it does NOT own (the adapter owns it): reading the line from guest memory, the traced HLE
 * publications, the TOC, r3, and the actual lv2/SPURS calls.
 *
 * No OS calls, no allocation, no globals, no guest-memory access, no libc.
 *
 * Linkage (the g10/g13/g14 arrangement): PS3_SPURS_LFQA_FN defaults to `static inline`, so a
 * translation unit that includes this header needs no extra object at link time.
 * `spurs_lfqueue_attach.c` redefines it empty so the SAME code is also emitted as ordinary external
 * symbols for the host test. One implementation, no header/object divergence. */

#include <stddef.h>
#include <stdint.h>

#ifndef PS3_SPURS_LFQA_FN
#define PS3_SPURS_LFQA_FN static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* The export                                                                             */
/* ------------------------------------------------------------------------------------ */

/* Recovered by hashing, not guessed. The rule is sha1(name + the 16-byte SONY suffix
 * 67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A) read little-endian -- the same rule that
 * reproduces cellSpursLFQueueInitialize = 0x363D97D3 and cellSpursEventFlagAttachLv2EventQueue =
 * 0x87630976 in this very table. The 65-bit-integer suffix string g9/g14 quoted
 * ("6759659904250490566427499489741a") reproduces NEITHER of those and was wrong
 * lists every candidate tried under both rules. */
#define PS3_SPURS_LFQA_NID 0x1656D49Fu
#define PS3_SPURS_LFQA_NAME "cellSpursLFQueueAttachLv2EventQueue"

/* ------------------------------------------------------------------------------------ */
/* Sizes and offsets                                                                      */
/* ------------------------------------------------------------------------------------ */

/* The entry reads and writes the same 128-byte cache line _cellSpursLFQueueInitialize built. */
#define PS3_SPURS_LFQA_BYTES 128u

/* The line words this entry touches.
 *
 * `+0x74` IS the owner word (decision 27): the entry loads it on EVERY arm, before the
 * SDK compare, and it is the low word of the u64 the init stored at `+0x70`. A tagged owner
 * (`|1`) is a CellSpurs -- its EA is the word with the low nibble cleared -- and an UNTAGGED one is a
 * taskset, whose own `+0x64` holds the CellSpurs it belongs to. The init's bound check on that
 * word is the taskset's own `wid`, which is why the tag reads that way round.
 */
#define PS3_SPURS_LFQA_F_OWNER 0x74u      /* be u64: the owner EA or id */
#define PS3_SPURS_LFQA_OWNER_TAG 0x64u  /* +0x64 of the owner, when the owner EA's low nibble is 0 */
#define PS3_SPURS_LFQA_VERSION 0x24u    /* be u32: the shape selector         */
#define PS3_SPURS_LFQA_QUEUE 0x20u      /* be u32/u8: the event queue id OR the port byte         */
#define PS3_SPURS_LFQA_RESULT 0x78u     /* the attach result, OUT              */
#define PS3_SPURS_LFQA_PORT2 0x21u      /* be u8, SPURS shape only                                 */
#define PS3_SPURS_LFQA_W28 0x28u        /* be u32, SPURS shape only                                */
#define PS3_SPURS_LFQA_W30 0x30u        /* be u16, SPURS shape only                                */
#define PS3_SPURS_LFQA_W50 0x50u        /* be u16, SPURS shape only                                */
#define PS3_SPURS_LFQA_W7C 0x7Cu        /* be u32, SPURS shape only                                */

/* The version that selects the SPURS shape. */
#define PS3_SPURS_LFQA_VERSION_SPURS 3u

/* The iteration count of each shape. The loop index is compared against a value
 * derived from `+0x24 ^ 3`, which is 2 for +0x24 == 3 and 1 otherwise:
 * the arithmetic form of "two queues for the SPURS shape, one for the CellSync shape". */
#define PS3_SPURS_LFQA_ITERATIONS_SPURS 2u
#define PS3_SPURS_LFQA_ITERATIONS_CELLSYNC 1u

/* ------------------------------------------------------------------------------------ */
/* The entry's own constants                                                              */
/* ------------------------------------------------------------------------------------ */

/* The event mask of the connect syscall: bits **16..63**, every one set. `0xAF2C` builds it
 * on the `r6 != 0` arm -- the SAME value and the SAME
 * arm the event-flag attach uses. It is asserted equal to
 * ps3_spurs_instance.h's PS3_SPURS_DYNAMIC_PORT_MASK in the host test, so the two cannot drift.
 *
 * NOT `1 << port`: this entry never chooses a port. lv2 picks the lowest free port in 16..63 and
 * returns it in the `spup` out-word, which the SPURS shape then records
 * at line +0x20. `PS3_SPURS_LFQA_SPU_PORT` is only the value the firmware then reports on
 * the inFAMOUS path; it is never an input to the connect. */
#define PS3_SPURS_LFQA_EVENT_MASK UINT64_C(0xFFFFFFFFFFFF0000)
#define PS3_SPURS_LFQA_SPU_PORT 15u

/* The lv2 event queue the SPURS shape creates: protocol 1 (FIFO), type 1 (PPU), name 0,
 * ipc_key 0, depth 1. The immediate operands are a size of 2, a type of 1 and a
 * zero name byte. */
#define PS3_SPURS_LFQA_QUEUE_PROTOCOL 1u
#define PS3_SPURS_LFQA_QUEUE_TYPE 1u
#define PS3_SPURS_LFQA_QUEUE_NAME 0
#define PS3_SPURS_LFQA_QUEUE_IPC_KEY 0
#define PS3_SPURS_LFQA_QUEUE_DEPTH 1

/* The lv2 event queue id an unattached line carries: 0xFFFFFFFF, which is the flag
 * the CellSync shape tests. */
#define PS3_SPURS_LFQA_QUEUE_NONE 0xFFFFFFFFu

/* The SDK bound 0xAF2C applies: at or above 0x180000 the
 * requested port may only be 0..15. Port 15 is inside it, so the bound never refuses on this path. */
#define PS3_SPURS_LFQA_SDK_PLAIN_MAX 0x17FFFFu

/* The syscalls in the closure, named so a later slice that serves a different producer knows what
 * the firmware called. Only 128, 135, 136, 129, 137 and 251 are on THIS entry's path. */
#define PS3_SPURS_LFQA_SC_QUEUE_CREATE 128u
#define PS3_SPURS_LFQA_SC_QUEUE_DESTROY 129u
#define PS3_SPURS_LFQA_SC_GROUP_CONNECT 251u
#define PS3_SPURS_LFQA_SC_GROUP_DISCONNECT 252u
#define PS3_SPURS_LFQA_SC_SPU_GET_STATUS 134u
#define PS3_SPURS_LFQA_SC_SPU_SET_RUNNING 135u
#define PS3_SPURS_LFQA_SC_SPU_GET_GROUP_ID 136u
#define PS3_SPURS_LFQA_SC_GROUP_START 137u

/* The namespace the fold belongs to:
 * 0x80410900 as the base, ORed with the low byte. */
#define PS3_SPURS_LFQA_ATTACH_NS 0x80410900u

/* ------------------------------------------------------------------------------------ */
/* Result codes                                                                           */
/* ------------------------------------------------------------------------------------ */

/* The values the entry leaves in r3, as its single epilogue produces them (sign-extended).
 * Every 0x8041xxxx code the entry assembles itself is in the CORE namespace (0x804109xx), not the
 * task namespace 0x804109xx -- both share the low byte, so the distinction matters to a reader. */
#define PS3_SPURS_LFQA_OK 0u
#define PS3_SPURS_LFQA_ALIGN 0x80410910u
#define PS3_SPURS_LFQA_NULL 0x80410911u
#define PS3_SPURS_LFQA_OWNER_NULL 0x80410902u      /* the owner resolved to NULL     */
#define PS3_SPURS_LFQA_PORT 0x8041090Fu       /* +0x20 != -1 on the SPURS shape */
#define PS3_SPURS_LFQA_SDK 0x80410909u        /* SDK outside every modelled arm */

/* The codes the callees produce, before the entry folds them. The entry replaces the top
 * three bytes with 0x804109 and keeps the low byte, and a result whose `(code>>16)&0xF ==
 * 0x41` result and folds it the same way. Kept here so the adapter applies the firmware's fold
 * instead of inventing one. */
#define PS3_SPURS_LFQA_LV2_OK 0u
#define PS3_SPURS_LFQA_LV2_EAGAIN 0x80010001u
#define PS3_SPURS_LFQA_LV2_ESRCH 0x80010005u
#define PS3_SPURS_LFQA_LV2_EBUSY 0x8001000Au
#define PS3_SPURS_LFQA_LV2_EISCONN 0x80010015u  /* 0xAF2C turns this into 0x8041070A, but it is still a nonzero lv2 code */

/* The two codes 0xAF2C itself produces on this path. */
#define PS3_SPURS_LFQA_AF2C_NULL 0x80410711u
#define PS3_SPURS_LFQA_AF2C_PORT 0x80410702u

/* Domain status. Distinct from the guest r3: a refused call produces no image. */
typedef enum spurs_lfqa_status {
    PS3_SPURS_LFQA_STATUS_OK = 0,          /* the request is fully determined, r3 = 0 */
    PS3_SPURS_LFQA_STATUS_REFUSED = 1,     /* gateway refused, r3 = the firmware code */
    PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT = 2 /* NULL request / NULL out parameter */
} spurs_lfqa_status;

/* ------------------------------------------------------------------------------------ */
/* The request the adapter hands to the native services                                   */
/* ------------------------------------------------------------------------------------ */

/* One fully determined lv2 + SPURS action for one loop iteration.
 *
 * `kind` distinguishes the two shapes' iteration 0, which are not the same call:
 *   CREATE_AND_ATTACH  (SPURS shape, both iterations) the queue does not exist yet: the create syscall first,
 *                      then the keyed attach of THAT queue.
 *   ATTACH_ONLY        (CellSync shape) the queue id comes from the line's `+0x20`; no create syscall.
 *   ROLLBACK           the unwind block: the destroy syscall plus, for
 *                      an iteration that had already been attached, the disconnect syscall first.
 */
typedef enum spurs_lfqa_action_kind {
    PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH = 0,
    PS3_SPURS_LFQA_ACTION_ATTACH_ONLY = 1,
    PS3_SPURS_LFQA_ACTION_ROLLBACK = 2
} spurs_lfqa_action_kind;

typedef struct spurs_lfqa_request {
    /* Gate order: the
     * line EA must be 128-byte aligned, and NULL is answered by the SECOND gate. Both are the
     * entry's own, before any guest read. */
    uint32_t line;
    /* The line's `+0x74` word, as the adapter read it. The entry uses
     * its low 32 bits. A tagged word IS the owner's CellSpurs EA with its low nibble set; an untagged one
     * is a taskset EA. */
    uint32_t owner;
    /* The line's `+0x24` word, as the adapter read it. */
    uint32_t version;
    /* The line's `+0x20` word, as the adapter read it. Only consulted on the non-3 shape,
     * and only its `== 0xFFFFFFFF` test matters. */
    uint32_t recorded_queue;
    /* The effective PROCESS sdk version (the SDK bound), never the guest CellSpurs+0xDA4. */
    uint32_t sdk_version;
} spurs_lfqa_request;

/* The owner resolution. The low nibble of the owner word is examined:
 *   nibble != 0                          -> PHASE C: the word with
 *                                           its low nibble cleared: the CellSpurs EA itself.
 *   nibble == 0 and the word != 0        -> PHASE B: read `+0x64` of the word: an
 *                                           UNTAGGED owner is a taskset and this is its own CellSpurs.
 *   nibble == 0 and the word == 0        -> PHASE A: nothing to resolve, so the entry
 *                                           refuses with 0x80410902.
 * The result lands in `*group`; it is a CellSpurs EA, never an SPU thread group id. Its legacy name is
 * kept so no identifier moves -- the tag meaning is decision 27's. */
typedef enum spurs_lfqa_owner_phase {
    PS3_SPURS_LFQA_OWNER_PHASE_A = 0,  /* the owner word is 0: the local fallback arm              */
    PS3_SPURS_LFQA_OWNER_PHASE_B = 1,  /* `owner + 0x64` holds the group id                        */
    PS3_SPURS_LFQA_OWNER_PHASE_C = 2   /* `owner & ~0xF` IS the group id                           */
} spurs_lfqa_owner_phase;

/* ------------------------------------------------------------------------------------ */
/* Helpers                                                                                */
/* ------------------------------------------------------------------------------------ */

/* Big-endian store. Returns 0 on success, -1 when the write would leave the buffer. */
PS3_SPURS_LFQA_FN int ps3_spurs_lfqa_put_be(unsigned char *out, size_t capacity, size_t offset,
    size_t width, uint64_t value)
{
    if (!out) return -1;
    if (width != 1u && width != 2u && width != 4u && width != 8u) return -1;
    if (offset > capacity || width > capacity - offset) return -1;
    for (size_t index = 0; index < width; ++index)
        out[offset + index] = (uint8_t)(value >> (8u * (width - 1u - index)));
    return 0;
}

/* Big-endian load of `width` bytes; an out-of-range read yields 0, which no compared field uses. */
PS3_SPURS_LFQA_FN uint64_t ps3_spurs_lfqa_get_be(const unsigned char *in, size_t capacity,
    size_t offset, size_t width)
{
    if (!in || width == 0u || width > 8u) return 0;
    if (offset > capacity || width > capacity - offset) return 0;
    uint64_t value = 0;
    for (size_t index = 0; index < width; ++index) value = value << 8 | in[offset + index];
    return value;
}

/* The sign extension every exit applies to the result. */
PS3_SPURS_LFQA_FN uint64_t ps3_spurs_lfqa_sign_extend(uint32_t code)
{
    return (uint64_t)(int64_t)(int32_t)code;
}

/* ------------------------------------------------------------------------------------ */
/* The two entry gates                                         */
/* ------------------------------------------------------------------------------------ */

/* In the firmware's order, first failure wins:
 *   1. line & 0x7F  -> PS3_SPURS_LFQA_ALIGN
 *   2. line == 0    -> PS3_SPURS_LFQA_NULL
 * Gate 1 runs BEFORE gate 2 -- the reverse of the usual order. A NULL
 * line therefore reports the ALIGN code, not the NULL code; the NULL code is only reachable for a
 * line whose low 7 bits are clear, i.e. an aligned zero. Both are pure register tests: no guest
 * byte is read before them.
 *
 * `*r3` receives 0 or the firmware code. BAD_ARGUMENT for a NULL request or out pointer. */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_validate(const spurs_lfqa_request *request,
    uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_LFQA_OK;
    if ((request->line & 0x7Fu) != 0u)
        code = PS3_SPURS_LFQA_ALIGN;
    else if (request->line == 0u)
        code = PS3_SPURS_LFQA_NULL;
    *r3 = code;
    return code == PS3_SPURS_LFQA_OK ? PS3_SPURS_LFQA_STATUS_OK : PS3_SPURS_LFQA_STATUS_REFUSED;
}

/* ------------------------------------------------------------------------------------ */
/* The owner resolution                                        */
/* ------------------------------------------------------------------------------------ */

/* Resolves the group id from the line's `+0x74` word:
 *
 *   r0 = owner & 0xF
 *   r28 = owner & ~0xF      (phase C's value, a CellSpurs EA)
 *   nibble != 0                  -> phase C
 *   nibble == 0 and word == 0    -> 0x80410902
 *   phase B: r28 = the untagged TASKSET's own CellSpurs (+0x64)
 *   resolved value == 0          -> 0x80410902
 *
 * NOTE the two ways to reach `0x80410902` and the ONE way to reach it for the nibble-0/
 * word-0 case: that case does NOT read `+0x64`, it goes straight to the refusal, because the word is
 * zero and `r28` would be zero anyway. The phase is reported so the adapter can trace which of the
 * three arms ran, and can decide whether to read `owner + 0x64` at all.
 *
 * `*group` is written on OK; a refusal leaves it at 0.
 * BAD_ARGUMENT for a NULL request/out pointer; REFUSED with PS3_SPURS_LFQA_OWNER_NULL otherwise. */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_owner(const spurs_lfqa_request *request,
    uint32_t *group, spurs_lfqa_owner_phase *phase, uint32_t *r3)
{
    if (!request || !group || !phase || !r3) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    *group = 0u;
    *r3 = PS3_SPURS_LFQA_OK;
    const uint32_t owner = request->owner;
    if ((owner & 0xFu) != 0u) {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_C;
        *group = owner & ~0xFu;
    } else if (owner == 0u) {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
    } else {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_B;
        /* The value at owner+0x64 is a GUEST read the adapter performs; the domain receives it as
         * `request->owner` already resolved by the caller when it calls
         * ps3_spurs_lfqa_owner_with_tag() instead. Here the caller passes the tagged value in
         * `*group` through ps3_spurs_lfqa_owner_tagged(), below. */
    }
    if (*group == 0u) {
        *r3 = PS3_SPURS_LFQA_OWNER_NULL;
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }
    return PS3_SPURS_LFQA_STATUS_OK;
}

/* The same resolution when the adapter has ALREADY performed the one guest read (owner+0x64). Pass
 * `tag` = 0 when no read was needed (nibble != 0), and the value read from owner+0x64 when it was.
 * This is the function the adapter should call; ps3_spurs_lfqa_owner() above is the same logic with
 * the phase-B read left to the caller. */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_owner_tagged(uint32_t owner, uint32_t tag,
    uint32_t *group, spurs_lfqa_owner_phase *phase, uint32_t *r3)
{
    if (!group || !phase || !r3) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    *r3 = PS3_SPURS_LFQA_OK;
    if ((owner & 0xFu) != 0u) {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_C;
        *group = owner & ~0xFu;
    } else if (owner == 0u) {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        *group = 0u;
    } else {
        *phase = PS3_SPURS_LFQA_OWNER_PHASE_B;
        *group = tag;
    }
    if (*group == 0u) {
        *r3 = PS3_SPURS_LFQA_OWNER_NULL;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }
    return PS3_SPURS_LFQA_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* The shape selector and its one extra gate                          */
/* ------------------------------------------------------------------------------------ */

/* True exactly for `version == 3`. */
PS3_SPURS_LFQA_FN int ps3_spurs_lfqa_is_spurs_shape(uint32_t version)
{
    return version == PS3_SPURS_LFQA_VERSION_SPURS ? 1 : 0;
}

/* The loop trip count: 2 for the SPURS shape, 1 for the CellSync shape (see
 * PS3_SPURS_LFQA_ITERATIONS_*). */
PS3_SPURS_LFQA_FN uint32_t ps3_spurs_lfqa_iterations(uint32_t version)
{
    return ps3_spurs_lfqa_is_spurs_shape(version) ? PS3_SPURS_LFQA_ITERATIONS_SPURS
                                                  : PS3_SPURS_LFQA_ITERATIONS_CELLSYNC;
}

/* The shape gate the CellSync arm runs BEFORE its first attach:
 * A line that carries no recorded event queue id (`+0x20` != -1) is refused with PS3_SPURS_LFQA_PORT. The
 * SPURS shape has its own, different check: both `+0x20`
 * and `+0x21` must be 0xFF, and `+0x20` is tested first exactly as `+0x21` is tested second.
 *
 * Returns STATUS_OK (with `*r3` = 0) or STATUS_REFUSED (`*r3` = the code). */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_shape_gate(const spurs_lfqa_request *request,
    uint32_t port2, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    *r3 = PS3_SPURS_LFQA_OK;
    if (ps3_spurs_lfqa_is_spurs_shape(request->version)) {
        /* ONE byte at +0x20 is tested first, then
         * the byte at +0x21. A `0x123456FF` word therefore PASSES (its
         * low byte is 0xFF) and `0xFFFFFFFE` is refused. The two offsets are the u8 fields the
         * CellSpursLFQueue tail stores 0xFF into,
         * which is why the SPURS shape expects 0xFF in both. */
        if ((request->recorded_queue & 0xFFu) != 0xFFu ||
            (port2 & 0xFFu) != 0xFFu) {
            *r3 = PS3_SPURS_LFQA_PORT;
            return PS3_SPURS_LFQA_STATUS_REFUSED;
        }
        return PS3_SPURS_LFQA_STATUS_OK;
    }
    if (request->recorded_queue == PS3_SPURS_LFQA_QUEUE_NONE) {
        *r3 = PS3_SPURS_LFQA_PORT;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }
    return PS3_SPURS_LFQA_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* The queue-create packet                                 */
/* ------------------------------------------------------------------------------------ */

/* 16 bytes, big-endian, exactly what the entry hands to the create syscall:
 *   +0x0 u32 protocol = 1        (the immediate 2 is the SIZE argument r5, not the protocol)
 *   +0x4 u32 type     = 1
 *   +0x8 u64 name     = 0        (a copy of a zero constant, then one byte stored at +7)
 *   the trailing u64 ipc_key is the caller's `r5` (set to 0) and is NOT part of this
 *   packet: it is a separate register. The packet is therefore 16 bytes and the depth is `r6 = 1`
 *
 * Returns 0, or -1 for a NULL/undersized buffer. */
PS3_SPURS_LFQA_FN int ps3_spurs_lfqa_queue_image(unsigned char *out, size_t capacity)
{
    if (!out || capacity < 16u) return -1;
    for (size_t index = 0; index < 16u; ++index) out[index] = 0;
    if (ps3_spurs_lfqa_put_be(out, capacity, 0x0, 4u, PS3_SPURS_LFQA_QUEUE_PROTOCOL) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(out, capacity, 0x4, 4u, PS3_SPURS_LFQA_QUEUE_TYPE) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(out, capacity, 0x8, 8u, (uint64_t)PS3_SPURS_LFQA_QUEUE_NAME) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* The keyed, STATIC attach mask                                */
/* ------------------------------------------------------------------------------------ */

/* The connect syscall the attach performs:
 *
 *   r4 = the lv2 event queue id
 *   r3 = the SPU thread group id
 *   r6 = &frame[0x70]  -- OUT: the bound port
 *   syscall 251: sys_spu_thread_group_connect_event_all_threads
 *
 * and the mask is the dynamic one, bits 16..63. `ps3_spurs_lfqa_event_mask()` therefore returns
 * PS3_SPURS_LFQA_EVENT_MASK for any input, and its `port` parameter exists only so the adapter can
 * ask "the mask a per-port connect WOULD use" when it composes against a primitive that needs one
 * (the native static attach mode). That per-port form is NOT what this entry performs: see the note
 * above PS3_SPURS_LFQA_EVENT_MASK.
 *
 * Returns PS3_SPURS_LFQA_EVENT_MASK when `port` is `PS3_SPURS_LFQA_EVENT_PORT_WILDCARD`; otherwise
 * the single-bit mask `1 << port`, which is a DIFFERENT primitive's shape and is returned only so a
 * caller can tell the two apart. */
#define PS3_SPURS_LFQA_EVENT_PORT_WILDCARD 0xFFu
PS3_SPURS_LFQA_FN uint64_t ps3_spurs_lfqa_event_mask(uint32_t port)
{
    if (port == PS3_SPURS_LFQA_EVENT_PORT_WILDCARD) return PS3_SPURS_LFQA_EVENT_MASK;
    if (port >= 64u) return 0;
    return UINT64_C(1) << port;
}

/* ------------------------------------------------------------------------------------ */
/* The publication                                   */
/* ------------------------------------------------------------------------------------ */

/* The LAST iteration's writes, in the firmware's order, over the caller's own line bytes.
 *
 * CellSync shape:
 *   +0x78 = the created/(re)used queue id (4 bytes)
 *   +0x7C = the SECOND queue id           (4 bytes)
 *   (memory barrier)
 *   +0x20 = the bound port                (1 byte)
 *   +0x21 = the second port               (1 byte)
 *   +0x28                                 (4 bytes)
 *   +0x30                                 (2 bytes)
 *   +0x50                                 (2 bytes)
 *
 * SPURS shape:
 *   +0x78 = a 64-bit word                 (8 bytes)
 *   (memory barrier)
 *   +0x20 = a 32-bit word                 (4 bytes)
 *
 * NOTE the widths: the two shapes write the SAME OFFSET at different widths (+0x78 is 8 bytes for
 * the SPURS shape and 4 for the CellSync one; +0x20 is 4 bytes for the SPURS shape and 1 for the
 * CellSync one). A builder that picks one width for both would be wrong.
 *
 * `frame` is the adapter's own 0x80-byte block, i.e. the bytes the entry keeps at `r1+0x70`; the
 * domain never sees the firmware's stack, so the adapter fills the six/eight byte-runs from its own
 * loop bookkeeping and the domain lays them out in the firmware's order. `spurs_shape` selects the
 * arm. Returns 0, or -1 on a bad buffer. */
PS3_SPURS_LFQA_FN int ps3_spurs_lfqa_publication(int spurs_shape, uint64_t id64, uint32_t id_lo,
    uint32_t id_hi, uint32_t port0, uint32_t port1, uint32_t w28, uint32_t w30, uint32_t w50,
    unsigned char *io, size_t capacity)
{
    if (!io || capacity < PS3_SPURS_LFQA_BYTES) return -1;
    if (spurs_shape) {
        if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_RESULT, 8u, id64) != 0) return -1;
        if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_QUEUE, 4u, port0) != 0) return -1;
        return 0;
    }
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_RESULT, 4u, id_lo) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_W7C, 4u, id_hi) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_QUEUE, 1u, port0 & 0xFFu) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_PORT2, 1u, port1 & 0xFFu) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_W28, 4u, w28) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_W30, 2u, w30 & 0xFFFFu) != 0) return -1;
    if (ps3_spurs_lfqa_put_be(io, capacity, PS3_SPURS_LFQA_W50, 2u, w50 & 0xFFFFu) != 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* The full plan: what the adapter must do, in order                                      */
/* ------------------------------------------------------------------------------------ */

/* A plan is fully determined before anything is performed, so the adapter can preflight every
 * guest write and the whole lv2/SPURS effect set before committing to the first one -- the same
 * discipline the other SPURS dispatches use. */
#define PS3_SPURS_LFQA_MAX_ACTIONS 3u

typedef struct spurs_lfqa_action {
    spurs_lfqa_action_kind kind;
    uint32_t iteration;      /* 0-based index inside the shape's loop                        */
    uint32_t queue;          /* ATTACH_ONLY: the id from the line; CREATE_AND_ATTACH: 0 (output)*/
    uint64_t mask;           /* the connect mask: PS3_SPURS_LFQA_EVENT_MASK (bits 16..63)    */
} spurs_lfqa_action;

typedef struct spurs_lfqa_plan {
    spurs_lfqa_status status;
    uint32_t r3;                         /* the firmware's code, sign-extended by the caller   */
    spurs_lfqa_owner_phase owner_phase;
    uint32_t group;                      /* the resolved CellSpurs EA 0xAF2C connects to (r28)  */
    uint32_t iterations;                 /* 1 or 2                                             */
    int spurs_shape;
    uint32_t actions_count;
    spurs_lfqa_action actions[PS3_SPURS_LFQA_MAX_ACTIONS];
    /* The publication the LAST iteration performs (only valid when status == OK). */
    unsigned char publication[PS3_SPURS_LFQA_BYTES];
} spurs_lfqa_plan;

/* Builds the plan from the request and the one value the adapter must supply from guest memory
 * (`tag`, the word at owner+0x64, read only on phase B), plus the two optional line bytes the SPURS
 * shape's own gate inspects (`port2` = `+0x21`; pass 0xFF when the shape does not need it).
 *
 * Order, matching the firmware:
 *   1. the two entry gates
 *   2. the owner resolution and its 0x80410902
 *   3. the shape gate, AFTER the owner and BEFORE any write
 *   4. the SDK bound 0xAF2C applies to the requested port
 *   5. the actions: for the SPURS shape two CREATE_AND_ATTACH, for the CellSync shape one
 *      ATTACH_ONLY; each with the `1 << 15` mask
 *   6. the publication for the last iteration
 * Nothing is performed here: no lv2 call, no guest write.
 *
 * REFUSED plans leave `publication` zero and `actions_count` 0, because every refusal above sits
 * before the firmware's first guest write. */
PS3_SPURS_LFQA_FN void ps3_spurs_lfqa_plan_build(const spurs_lfqa_request *request, uint32_t tag,
    uint32_t port2, spurs_lfqa_plan *plan)
{
    if (!plan) return;
    for (size_t index = 0; index < sizeof(*plan); ++index) ((unsigned char *)plan)[index] = 0;
    plan->status = PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    if (!request) return;
    uint32_t r3 = 0;
    if (ps3_spurs_lfqa_validate(request, &r3) != PS3_SPURS_LFQA_STATUS_OK) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = r3;
        plan->owner_phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        return;
    }
    uint32_t group = 0;
    spurs_lfqa_owner_phase phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
    if (ps3_spurs_lfqa_owner_tagged(request->owner, tag, &group, &phase, &r3) != PS3_SPURS_LFQA_STATUS_OK) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = r3;
        plan->owner_phase = phase;
        return;
    }
    plan->owner_phase = phase;
    plan->group = group;
    if (ps3_spurs_lfqa_shape_gate(request, port2, &r3) != PS3_SPURS_LFQA_STATUS_OK) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = r3;
        return;
    }
    plan->spurs_shape = ps3_spurs_lfqa_is_spurs_shape(request->version);
    plan->iterations = ps3_spurs_lfqa_iterations(request->version);
    /* The requested port is the SPU port the entry's own frame carries, which both shapes fix at 15
     * 0xAF2C's SDK bound accepts 0..15 at or above 0x180000 and
     * 0..63 below it; 15 is inside both, so the bound can never refuse here. It is evaluated anyway
     * so a caller that ever changes the port sees the firmware's answer. */
    const uint32_t port = PS3_SPURS_LFQA_SPU_PORT;
    if (request->sdk_version >= 0x180000u && port > 0xFu) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = PS3_SPURS_LFQA_SDK;
        plan->actions_count = 0;
        return;
    }
    if (port > 0x3Fu) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = PS3_SPURS_LFQA_AF2C_PORT;
        plan->actions_count = 0;
        return;
    }
    plan->actions_count = plan->iterations;
    for (uint32_t index = 0; index < plan->iterations && index < PS3_SPURS_LFQA_MAX_ACTIONS; ++index) {
        plan->actions[index].iteration = index;
        plan->actions[index].kind = plan->spurs_shape ? PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH
                                                      : PS3_SPURS_LFQA_ACTION_ATTACH_ONLY;
        plan->actions[index].queue = plan->spurs_shape ? 0u : request->recorded_queue;
        plan->actions[index].mask = ps3_spurs_lfqa_event_mask(PS3_SPURS_LFQA_EVENT_PORT_WILDCARD);
    }
    plan->status = PS3_SPURS_LFQA_STATUS_OK;
    plan->r3 = PS3_SPURS_LFQA_OK;
}

/* ------------------------------------------------------------------------------------ */
/* The result fold                                                        */
/* ------------------------------------------------------------------------------------ */

/* The entry folds a callee's code twice, and the two folds are not the same operation:
 *
 *   a) the fold replaces the top three bytes, keeping the low byte:
 *        r0 = 0x80410900          (the namespace constant)
 *        r9 = code & 0xFF         (mask bits 24..31)
 *        r31 = r9 | r0            = 0x80410900 | (code & 0xFF)
 *      Applied to the failure of the queue-create helper and to a 0x8041 code from
 *      0xB244. So `0x80410109` -> `0x80410909` and
 *      `0x80410711` -> `0x80410911`: the SECOND byte of the input is discarded, the low byte kept.
 *
 *   b) passing the code through unchanged: two of the exit branches
 *      keep r31 exactly as the callee left it when the code is already zero,
 *      and a nonzero code branches to the fold. There is no remap for anything else.
 *
 * The guard:
 *
 *       r0 = ROTL64(code, 48) with bits 52..63 cleared; r0 == 0x41 selects the (a) fold
 *
 * The rotate-left-and-clear form rotates the 64-bit register left by SH and keeps bits 0..MB-1 of the
 * result. With MB = 52 the survivor mask is the low 52 bits. For a zero-extended 32-bit word the
 * rotate by 48 puts bits 16..31 in positions 0..15, so the value compared is
 * `(code >> 16) & 0xFFF` -- and that must equal 0x041. The guard is therefore
 * `((code >> 16) & 0xFFFu) == 0x041`, i.e. *bits 16..23 of the word are 0x41*.
 *
 * That is why it fires on EVERY 0x8041xxxx code the entry meets -- `0x80410109`, `0x8041070A`,
 * `0x80410711` alike, all of whose bits 16..23 are 0x41 -- and on nothing else: the `0x8001 xxxx`
 * lv2 codes have bits 16..23 = 0x01 and are returned unchanged. Measured on the listing, not
 * assumed: the entry's own codes already carry the 0x804109 top, so the fold leaves them alone, and
 * the only codes it visibly rewrites are the callees' 0x804107xx / 0x804101xx ones. */
PS3_SPURS_LFQA_FN uint32_t ps3_spurs_lfqa_fold(uint32_t code)
{
    if (code != 0u && ((code >> 16) & 0xFu) == 0x1u)
        return PS3_SPURS_LFQA_ATTACH_NS | (code & 0xFFu);
    return code;
}

/* The whole-entry result: the last callee code, folded, and zero exactly when nothing failed.
 * A loop-count test implements the "only the last iteration's code is
 * published" rule; the earlier iterations' codes are recorded in the unwind path, not in r3. */
PS3_SPURS_LFQA_FN uint32_t ps3_spurs_lfqa_result(uint32_t last_code)
{
    return ps3_spurs_lfqa_fold(last_code);
}

/* ------------------------------------------------------------------------------------ */
/* The image the game's own line must satisfy (sanity, not a gate)                        */
/* ------------------------------------------------------------------------------------ */

/* `_cellSpursLFQueueInitialize` (NID 0x011EE38B) stores the DIRECTION at `+0x24`, and
 * inFAMOUS passes 3. A line whose `+0x24` is therefore 3 selects the SPURS shape, and this helper
 * says so from the line bytes alone -- so a reader comparing the two entries' contracts can see the
 * linkage without reading either listing again. Returns 1, 0, or -1 for a short buffer. */
PS3_SPURS_LFQA_FN int ps3_spurs_lfqa_line_is_spurs_shape(const unsigned char *line, size_t capacity)
{
    if (!line || capacity < PS3_SPURS_LFQA_BYTES) return -1;
    return ps3_spurs_lfqa_is_spurs_shape(
        (uint32_t)ps3_spurs_lfqa_get_be(line, capacity, PS3_SPURS_LFQA_VERSION, 4u));
}

#ifdef __cplusplus
}
#endif
#endif
