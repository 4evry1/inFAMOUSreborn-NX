#ifndef SPURS_LFQUEUE_H
#define SPURS_LFQUEUE_H
/* Platform-free, instance-free byte-image builders for the cellSpurs lock-free-queue entry the game
 * reaches immediately after its taskset setup block:
 *
 *   _cellSpursLFQueueInitialize NID 0x011EE38B
 *
 * Behaviour taken from the firmware library libsre (decrypted module SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6).
 *
 * The entry validates its arguments, dispatches on the low bit of argument 1 (set = the owner is a
 * CellSpursTaskset, clear = it is a CellSpurs instance), and builds the CellSyncLFQueue cache line
 * through the module's `0x1F6C` builder. It writes two versioned shapes -- CellSpursLFQueue
 * (`taskset+0x24 == 3`) and CellSyncLFQueue (any other taskset version with `elem_size == 128`).
 * It makes no syscall and starts no SPU work on inFAMOUS's path; CONTRACT §D records the two
 * syscalls that exist only in branches the game's arguments do not select.
 *
 * CORRECTION (g14b §2): the first byte the dispatcher 0x209C reads is the STATE word at queue+0x2C,
 * not a version. inFAMOUS's taskset is rebuilt from zero on every run and nothing writes +0x2C, so
 * the entry takes the builder branch, stores 2 there, and returns 0 (r31 still holds 0 from the
 * argument-gate setup). NO probe is reached. g14's `0x80410909`
 * came from reading the dispatcher's first branch as a queue check: it is the state-0 entry to
 * the next block, i.e. the success path.
 *
 * What this unit owns: the argument gates and their exact codes, the owner tag and its branch,
 * the builder's field set, the versioned byte images, the auxiliary zero runs and the code
 * collapse. What it does NOT own (the adapter owns it): the guest reads and writes themselves,
 * the traced HLE publications, the TOC and r3 publication, and the probe that only the
 * CellSyncLFQueue shape reaches.
 *
 * Linkage (the g10/g13 arrangement): PS3_SPURS_LFQ_FN defaults to `static inline`, so a
 * translation unit that includes this header needs no extra object at link time. `spurs_lfqueue.c`
 * defines it empty so the SAME code is also emitted as ordinary external symbols for the host test
 * and the Switch compile check. One implementation, no header/object divergence.
 *
 * No OS calls, no allocation, no globals, no guest-memory access. */

#include <stddef.h>
#include <stdint.h>

#ifndef PS3_SPURS_LFQ_FN
#define PS3_SPURS_LFQ_FN static inline
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* Shape                                                                                 */
/* ------------------------------------------------------------------------------------ */

/* The builder's stores cover exactly one 128-byte cache line, which is what the SPU consumer
 * GETLLARs whole. The caller supplies a buffer of at least PS3_SPURS_LFQ_BYTES. */
#define PS3_SPURS_LFQ_BYTES 128u

/* ------------------------------------------------------------------------------------ */
/* Result codes                                                                          */
/* ------------------------------------------------------------------------------------ */

/* The 32-bit values the firmware assembles in r3. Every exit sign-extends the code, so the
 * adapter sign-extends what this unit returns. The namespace is the TASK namespace the taskset
 * block already uses (0x804109xx), NOT the core namespace: the entry's leave-one
 * exit ORs 0x900 into the code and so builds every code from 0x80410000. */
#define PS3_SPURS_LFQ_OK 0u
#define PS3_SPURS_LFQ_NULL 0x80410911u      /* entry 0x900 collapse       */
#define PS3_SPURS_LFQ_ALIGN 0x80410910u     /* entry 0x900 collapse       */
#define PS3_SPURS_LFQ_INVAL 0x80410902u     /* entry 0x900 collapse       */
#define PS3_SPURS_LFQ_RETRY 0x80410914u     /* the probe's ESYNC remap      */
#define PS3_SPURS_LFQ_STAT 0x8041090Fu      /* the setter's EPERM, already folded     */

/* The probe's own codes, before the entry collapses them. 0x209C's inner helpers speak the core
 * namespace; 0x17180..0x1718C folds each of them with `(code & 0xFFFFFF00) | 0x80410900` and then
 * remaps two of them onto 0x80410914. None of them is reached on inFAMOUS's arguments (g14b §4). */
#define PS3_SPURS_LFQ_PROBE_INVAL 0x80410109u
#define PS3_SPURS_LFQ_PROBE_FULL 0x80410101u
#define PS3_SPURS_LFQ_PROBE_STAT 0x8041010Fu

/* The two owner branches' refusal limits: the spurs arm reads `spurs+0x74` and
 * refuses anything above 0x1F *before* writing any guest byte. */
#define PS3_SPURS_LFQ_SPURS_VERSION_MAX 0x1Fu

/* Domain status. Distinct from the guest r3: a refused call produces no image. */
typedef enum spurs_lfq_status {
    PS3_SPURS_LFQ_STATUS_OK = 0,          /* image valid, r3 = 0 */
    PS3_SPURS_LFQ_STATUS_REFUSED = 1,     /* image invalid, r3 = the firmware code */
    PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT = 2 /* NULL output buffer / wrong capacity / NULL r3 */
} spurs_lfq_status;

/* ------------------------------------------------------------------------------------ */
/* CellSyncLFQueue / CellSpursLFQueue field offsets                      */
/* ------------------------------------------------------------------------------------ */

/* Big-endian guest fields inside the 128-byte line. Every other byte of the line is zero. These
 * are the offsets 0x1FA0..0x2094 addresses, named in the firmware's own store order below. */
#define PS3_SPURS_LFQ_H0 0x00u          /* u16: pop index                                 */
#define PS3_SPURS_LFQ_H1 0x02u          /* u16: (pop + 1) mod (2 * depth)                 */
#define PS3_SPURS_LFQ_H6 0x06u          /* u16                                            */
#define PS3_SPURS_LFQ_H8 0x08u          /* u16: the push index the probe reserves         */
#define PS3_SPURS_LFQ_H10 0x0Au         /* u16                                            */
#define PS3_SPURS_LFQ_H14 0x0Eu         /* u16                                            */
#define PS3_SPURS_LFQ_ELEM_SIZE 0x10u   /* u32                                            */
#define PS3_SPURS_LFQ_DEPTH 0x14u       /* u32                                            */
#define PS3_SPURS_LFQ_BUFFER 0x18u      /* u64                                            */
#define PS3_SPURS_LFQ_B20 0x20u         /* u8 (spurs shape: 0xFF) or u32 0xFFFFFFFF        */
#define PS3_SPURS_LFQ_B21 0x21u         /* u8, spurs shape only: 0xFF                     */
#define PS3_SPURS_LFQ_DIRECTION 0x24u   /* u32                                            */
#define PS3_SPURS_LFQ_V28 0x28u         /* u32                                            */
#define PS3_SPURS_LFQ_AUX_BASE 0x30u    /* the first auxiliary run's first byte            */
#define PS3_SPURS_LFQ_AUX2_BASE 0x50u   /* the second run (the spurs shape also writes a
                                         * u16 0xFFFF here at 0x208C)                     */
#define PS3_SPURS_LFQ_TASKSET 0x4Cu      /* inside the second auxiliary run                 */
#define PS3_SPURS_LFQ_OWNER 0x70u       /* u64: the caller's r3, TAGGED (low bit kept)     */
#define PS3_SPURS_LFQ_V78 0x78u         /* u32 (spurs shape) or u64 (other shape)          */
#define PS3_SPURS_LFQ_V7C 0x7Cu         /* u32, spurs shape only                           */

/* The auxiliary zero runs: two 15-iteration loops of 2-byte zero stores at stride 2 from `queue+0x30` and
 * `queue+0x50`, i.e. 0x30..0x4C and 0x50..0x6C. Bytes 0x4C..0x50 are NOT written by them. */
#define PS3_SPURS_LFQ_AUX_COUNT 15u
#define PS3_SPURS_LFQ_AUX_STRIDE 2u
#define PS3_SPURS_LFQ_AUX_RUN_BYTES (PS3_SPURS_LFQ_AUX_COUNT * PS3_SPURS_LFQ_AUX_STRIDE)

/* The two words the entry reads, one per owner branch.
 *
 * The spurs arm reads a VERSION from its owner. The taskset arm does not: the word at +0x24
 * is what the builder later overwrites with the DIRECTION,
 * and the dispatcher reads that same word back later. */
#define PS3_SPURS_LFQ_TASKSET_VERSION 0x24u  /* taskset+0x24     */
#define PS3_SPURS_LFQ_SPURS_VERSION 0x74u    /* spurs+0x74      */
#define PS3_SPURS_LFQ_VERSION_SPURS 3u       /* the only value selecting 0x3A18's shape       */
#define PS3_SPURS_LFQ_STATE 0x2Cu            /* queue+0x2C   */
#define PS3_SPURS_LFQ_STATE_INITIALIZED 2u

/* The syscalls inside the closure that inFAMOUS's arguments cannot reach. Named so
 * a later slice that serves a real producer for those arms knows what the firmware called. */
#define PS3_SPURS_LFQ_SYSCALL_GETPID 0x01u
#define PS3_SPURS_LFQ_SYSCALL_ESYNC 0x82u
#define PS3_SPURS_LFQ_SPU_PLACEHOLDER 0x00493000u /* 0x2200: the local "(no SPU)" value      */

/* ------------------------------------------------------------------------------------ */
/* The arguments                                                                         */
/* ------------------------------------------------------------------------------------ */

typedef struct spurs_lfq_request {
    uint32_t owner;      /* r3, before the low-bit tag is stripped */
    uint32_t queue;      /* r4 */
    uint32_t buffer;     /* r5 */
    uint32_t elem_size;  /* r6 */
    uint32_t depth;      /* r7 */
    uint32_t direction;  /* r8 */
} spurs_lfq_request;

/* `r3 & 1` set means the owner is a CellSpursTaskset; clear means it is a CellSpurs instance. The
 * stripped base is a 32-bit EA (each arm keeps `rldicl ..., 0, 32`). */
typedef enum spurs_lfq_owner_kind {
    PS3_SPURS_LFQ_OWNER_SPURS = 0,
    PS3_SPURS_LFQ_OWNER_TASKSET = 1
} spurs_lfq_owner_kind;

/* The tag, with no masking: `owner & 1`. */
PS3_SPURS_LFQ_FN spurs_lfq_owner_kind ps3_spurs_lfq_owner_of(uint32_t owner);

/* The owner base the queue stores, i.e. `r3 & ~1` as a 32-bit EA. */
PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_owner_base(uint32_t owner);

/* The value the builder actually stores at queue+0x70: the caller's r3 VERBATIM
 * (the low 32 bits of the tagged argument, stored as a 64-bit word). inFAMOUS therefore leaves an ODD
 * EA in the field. */
PS3_SPURS_LFQ_FN int ps3_spurs_lfq_owner_field(uint32_t owner, uint32_t *field);

/* Runs the gates in the firmware's order, first failure wins.
 *
 * The register names below are the DISPATCHER's (0x209C), which the entry fills at 0x17150..0x17164:
 *   0x209C r3 = the caller's r4 (queue)      0x209C r4 = the caller's r5 (buffer)
 *   0x209C r5 = the caller's r6 (elem size)  0x209C r6 = the caller's r7 (depth)
 *   0x209C r7 = the caller's r8 (direction)  0x209C r8 = the caller's r3 (the owner, TAGGED)
 *
 * NOTE the owner: the entry passes its OWN r3 on with the tag STILL
 * SET. Only the dispatcher's r28 drops it.
 * Gate 8 below therefore masks the TAGGED value.
 *
 *   1.  queue == 0                     -> 0x80410911
 *   2.  elem_size == 0                 -> 0x80410911
 *   3.  depth == 0                     -> 0x80410902
 *   4.  elem_size > 0x4000             -> 0x80410902
 *   5.  elem_size & 0xF                -> 0x80410902
 *   6.  depth & 0xFFFF0000             -> 0x80410902
 *   7.  direction > 3                  -> 0x80410902
 *   8.  owner & 0x7F                   -> 0x80410910
 *   9.  buffer & 0xF                   -> 0x80410910   (the buffer)
 *  10.  the owner arm's version gate (see ps3_spurs_lfq_version_gate).
 *
 * Gates 3-7 share one code word, so all report 0x80410902. Gates 1
 * and 2 share one code word, 0x80410911. Gates 8 and 9 share one, 0x80410910.
 * The QUEUE is never alignment-checked.
 *
 * WHAT SUCCESS LOOKS LIKE: the dispatcher's first branch is NOT a refusal. When the dispatcher's r3
 * (the queue) is non-zero -- always, on inFAMOUS's call -- it is also not taken. The refusal for a
 * NULL queue comes the other way round: 0x20E8 takes 0x2140 only for r3 == 0, and 0x2140 is where
 * gates 1 and 2 both land. A non-zero queue walks 0x20EC.. and reaches 0x2268, the state probe.
 *
 * Direction IS gated (<= 3); inFAMOUS passes exactly 3, the boundary value. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_validate(const spurs_lfq_request *request,
    uint32_t *r3);

/* The owner arm's version gate, run only after gates 1-10 passed. `version_field` is the value the
 * adapter read from `taskset+0x24` (taskset arm) or `spurs+0x74` (spurs arm):
 *   spurs arm, value > 0x1F -> REFUSED, 0x80410902, nothing written
 *   spurs arm, value <= 0x1F -> OK, the CellSyncLFQueue shape
 *   taskset arm, anything -> OK; value == 3 selects the CellSpursLFQueue shape,
 *                                 every other value the CellSyncLFQueue shape.
 * Writes the guest r3 into `*r3`. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_version_gate(const spurs_lfq_request *request,
    uint32_t version_field, uint32_t *r3);

/* The shape predicate: true exactly for the taskset arm with `taskset+0x24 == 3`. */
PS3_SPURS_LFQ_FN int ps3_spurs_lfq_uses_spurs_shape(const spurs_lfq_request *request,
    uint32_t version_field);

/* ------------------------------------------------------------------------------------ */
/* The byte images                                                                       */
/* ------------------------------------------------------------------------------------ */

/* Big-endian store helper. Returns 0 on success, -1 when the write would leave the buffer. */
PS3_SPURS_LFQ_FN int ps3_spurs_lfq_put_be(unsigned char *out, size_t capacity, size_t offset,
    size_t width, uint64_t value);

/* Big-endian load of `width` bytes at `offset`. A read that would leave the buffer yields the
 * all-ones sentinel, which no compared field can equal on the paths that use it. */
PS3_SPURS_LFQ_FN uint64_t ps3_spurs_lfq_field(const unsigned char *in, size_t capacity,
    size_t offset, size_t width);

/* Builds the exact 128-byte line. Zero-filled first, then, in the firmware's store
 * order, the ten scalar fields and the versioned tail:
 *
 *   2-byte 0         -> +0x00
 *   4-byte elem_size -> +0x10
 *   2-byte 0         -> +0x02
 *   4-byte depth     -> +0x14
 *   2-byte 0         -> +0x06
 *   8-byte buffer    -> +0x18
 *   2-byte 0         -> +0x08
 *   4-byte direction -> +0x24
 *   2-byte 0         -> +0x0A
 *   2-byte 0         -> +0x0E
 *   2-byte 0         -> +0x30   (the first auxiliary word; the second run starts at +0x50)
 *   8-byte owner     -> +0x70   (TAGGED: the caller's r3 verbatim)
 *
 * and then exactly one tail. The CellSpursLFQueue tail, taken only when `r8 == 3`
 * (r8 is the OWNER ARGUMENT, re-used here as
 * the builder's version argument, which the dispatcher has already replaced by `1` or `2`; the
 * tail's condition is therefore the CALLER's `version == 3`):
 *   0x2068/0x2074 buffer |= 1     -> +0x18   (u64, the low bit set)
 *   2-byte 0                 -> +0x04
 *   1-byte 0xFF              -> +0x21
 *   2-byte 0xFFFF            -> +0x50
 *   2-byte 0                 -> +0x0C
 *   1-byte 0xFF              -> +0x20
 *   4-byte 0xFFFFFFFF        -> +0x28
 *   2-byte 0xFFFF            -> +0x30
 *   4-byte 0                 -> +0x78
 *   4-byte 0                 -> +0x7C
 * The CellSyncLFQueue tail (0x2028..0x2048), for every other version:
 *   4-byte 0                 -> +0x28
 *   4-byte 0xFFFFFFFF        -> +0x20
 *   8-byte 0                 -> +0x78
 *
 * Both tails leave +0x04 and +0x0C at 0 and neither touches +0x21 or +0x50 except the spurs one.
 *
 * The two auxiliary runs are NOT part of this image: they are a separate pair of writes into the
 * caller's own object (ps3_spurs_lfq_aux_image), which is why they are not in the 128 bytes.
 *
 * Returns STATUS_BAD_ARGUMENT for a NULL/undersized buffer. The image is produced for the values
 * in `request` and the strippable `owner`; the caller passes its OWN object's pre-call byte for
 * nothing: bytes the firmware does not write are zero here and the caller must not rely on them
 * unless it also zeroes the object (the adapter writes this image whole). */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_image(const spurs_lfq_request *request,
    uint32_t version_field, unsigned char *out, size_t capacity);

/* The same image, but over the caller's OWN 128 bytes: the fields the firmware does not write keep
 * whatever `io` already held. This is the shape the adapter uses, because the real object at the
 * game's `queue` EA is not zero on entry. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_image_in_place(const spurs_lfq_request *request,
    uint32_t version_field, unsigned char *io, size_t capacity);

/* The auxiliary zero runs. `index` 0 is 0x30..0x4C, 1 is 0x50..0x6C; each is 30 zero bytes. Both
 * are part of the entry's effect on the caller's object, outside the 128-byte line. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_aux_image(unsigned index, unsigned char *out,
    size_t capacity);

/* The guest address of run `index`'s first byte, or -1 on a bad index. */
PS3_SPURS_LFQ_FN int ps3_spurs_lfq_aux_address(uint32_t queue, unsigned index, uint32_t *address);

/* ------------------------------------------------------------------------------------ */
/* The probe fold and the state word                                                     */
/* ------------------------------------------------------------------------------------ */

/* The entry folds a NEGATIVE probe code into the task namespace before returning:
 *   the low byte of the code is kept (`code & 0xFF`), the high half becomes 0x8041 and 0x900 is ORed in,
 *   guarded by a sign test (a non-negative code skips the fold).
 * Only the last byte of the probe's code survives, and only when the code is negative.
 * Two codes are remapped to 0x80410914 BEFORE that fold, onto the same word at 0x172D8:
 *   one for the code 0x80410902,
 *   one for the code 0x8041090F.
 * `xoris` XORs the high half, so the comparison is against the WHOLE 32-bit code: 0x80410902 and
 * 0x8041090F. A non-negative code is otherwise returned unchanged. */
PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_collapse(uint32_t probe_code);

/* The 2-byte image writer the adapter uses for the builder's `+0x08` store.
 * Both shapes leave `+0x08 == 0`, which is why the adapter writes it as its own traced image and
 * why the value is passed in rather than assumed: the CELL it feeds is the reserved-slot word, not
 * the object state. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_state_image(unsigned char *out, size_t capacity);


/* ------------------------------------------------------------------------------------ */
/* Big-endian store                                                                      */
/* ------------------------------------------------------------------------------------ */

PS3_SPURS_LFQ_FN int ps3_spurs_lfq_put_be(unsigned char *out, size_t capacity, size_t offset,
    size_t width, uint64_t value)
{
    if (!out) return -1;
    if (width != 1u && width != 2u && width != 4u && width != 8u) return -1;
    if (offset > capacity || width > capacity - offset) return -1;
    for (size_t index = 0; index < width; ++index)
        out[offset + index] = (unsigned char)(value >> (8u * (width - 1u - index)));
    return 0;
}

PS3_SPURS_LFQ_FN uint64_t ps3_spurs_lfq_field(const unsigned char *in, size_t capacity,
    size_t offset, size_t width)
{
    if (!in || (width != 1u && width != 2u && width != 4u && width != 8u)) return UINT64_MAX;
    if (offset > capacity || width > capacity - offset) return UINT64_MAX;
    uint64_t loaded = 0;
    for (size_t index = 0; index < width; ++index) loaded = loaded << 8 | in[offset + index];
    return loaded;
}

/* ------------------------------------------------------------------------------------ */
/* The owner tag                                                                         */
/* ------------------------------------------------------------------------------------ */

PS3_SPURS_LFQ_FN spurs_lfq_owner_kind ps3_spurs_lfq_owner_of(uint32_t owner)
{
    /* Decision 27: these "taskset"/"spurs" labels are SWAPPED -- a tagged (`|1`) owner is a CellSpurs and
     * an untagged one is a taskset; the behaviour below is correct, only the two names are the old ones.
     * an all-zero bottom nibble is the spurs arm. A non-zero one is the taskset arm, whose
     * dispatcher (0x209C) then splits again on bit 1 (0x20F4). */
    return (owner & 1u) != 0u ? PS3_SPURS_LFQ_OWNER_TASKSET : PS3_SPURS_LFQ_OWNER_SPURS;
}

PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_owner_base(uint32_t owner)
{
    /* The taskset arm clears bit 0 before 0x209C (bit 0 dropped); the spurs arm
     * hands r3 straight through. Both then keep `rldicl ..., 0, 32`. */
    return ps3_spurs_lfq_owner_of(owner) == PS3_SPURS_LFQ_OWNER_TASKSET ? (owner & ~1u) : owner;
}

PS3_SPURS_LFQ_FN int ps3_spurs_lfq_owner_field(uint32_t owner, uint32_t *field)
{
    if (!field) return -1;
    /* The owner word is the dispatcher's r3, i.e. the caller's r3 as the
     * dispatcher rewrote it (tagged for the taskset arm); the builder stores it
     * whole, so the tag survives into guest memory. */
    *field = owner;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* The gates                                                                             */
/* ------------------------------------------------------------------------------------ */

/* The ARM the entry is about to take, as far as its argument gates are concerned
 * (G14c).
 *
 * A ZERO low nibble sends control to the spurs-arm version check, whose in-range case goes to the dispatcher 0x209C and
 * whose `> 0x1F` arm returns 0x80410902. A NON-ZERO low nibble calls the dispatcher with the caller's
 * registers remapped at 0x17150..0x17164, and 0x17170..0x171A0's fold. The two arms therefore gate the same
 * six arguments in the same order, but the nibble-0 arm OWNS `ps3_spurs_lfq_version_gate` (0x171AC's
 * 0x1F bound), which must be answered before the dispatcher's first guest access, while the nibble!=0 arm
 * has no earlier gate at all and reads the `+0x24` word next. */
typedef enum spurs_lfq_arm {
    PS3_SPURS_LFQ_ARM_SPURS = 0,     /* version-gated, then 0x1716C -> 0x209C  */
    PS3_SPURS_LFQ_ARM_TASKSET = 1    /* 0x1716C directly: 0x17228 reads taskset+0x24      */
} spurs_lfq_arm;

PS3_SPURS_LFQ_FN spurs_lfq_arm ps3_spurs_lfq_arm_of(const spurs_lfq_request *request)
{
    if (!request) return PS3_SPURS_LFQ_ARM_SPURS;
    return (request->owner & 0xFu) != 0u ? PS3_SPURS_LFQ_ARM_TASKSET : PS3_SPURS_LFQ_ARM_SPURS;
}

/* The dispatcher 0x209C's nine argument gates, in the firmware's order, first failure wins.
 *
 * WHICH ARM THEY GATE. 0x209C is entered by both arms (for the nibble!=0 owner, and
 * for the nibble-0 owner whose version passed), and its gate chain is the same
 * register tests either way. What differs is the register remap that precedes it:
 *
 *   nibble != 0 (the TASKSET arm): 0x17150..0x17164 map the caller's r4..r8 into 0x209C's r3..r7 and the
 *     caller's r3 (TAGGED) into 0x209C's r8. 0x209C's r3 is the QUEUE, r4 the BUFFER, r5 the ELEMENT
 *     SIZE, r6 the DEPTH, r7 the DIRECTION and r8 the OWNER. That is the table this function uses.
 *   nibble == 0 (the SPURS arm): 0x1716C is reached with `ble` and the remap has NOT happened, so the
 *     0x209C registers are the caller's own: r3 is the owner argument (the CellSpurs), r8 is whatever the
 *     caller left in r8 and no caller-supplied buffer exists at all. Its gates therefore never see a
 *     buffer, an element size or a depth, and the only gate that can fire is r3 == 0 (the owner).
 *     Running the taskset table on it would invent refusals the firmware does not make, so it is not run:
 *     the arm's own refusal is ps3_spurs_lfq_version_gate's 0x80410902, answered before any guest access.
 *
 * That split is what G14c fixed: the spurs arm's version gate is the ENTRY's first access and
 * must precede the queue line's read at 0x17228, not follow it. */
PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_validate(const spurs_lfq_request *request,
    uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    /* The spurs arm (nibble 0): 0x209C is reached only through the version check's in-range branch, and only its r3 == 0 gate
     * can fire on that arm's own registers (see the note above). Nothing else is gated here. */
    if (ps3_spurs_lfq_arm_of(request) == PS3_SPURS_LFQ_ARM_SPURS) {
        *r3 = PS3_SPURS_LFQ_OK;
        return PS3_SPURS_LFQ_STATUS_OK;
    }
    uint32_t code = PS3_SPURS_LFQ_OK;

    /* Gate 1. The entry splits on the owner's low bit and reaches the
     * dispatcher 0x209C only on a non-zero low nibble. The dispatcher's first gate is
     *   a test that 0x209C's r3 == 0 (refusal word 0x80410111),
     * which the entry folds to 0x80410911. The spurs arm's version check refuses only on
     * `spurs+0x74 > 0x1F` (ps3_spurs_lfq_version_gate); it does not inspect the queue, so this
     * check is applied unconditionally here: a NULL queue can never produce a usable image and the
     * builder would fault on it. */
    if (request->queue == 0u) code = PS3_SPURS_LFQ_NULL;

    /* Gate 2: the elem_size == 0 test; on the taskset arm r5 == 0 falls
     * into 0x2140. The dispatcher 0x209C's r5 is the caller's r6 (`0x17158`), i.e. the ELEMENT SIZE,
     * not the buffer. */
    if (code == PS3_SPURS_LFQ_OK && request->elem_size == 0u) code = PS3_SPURS_LFQ_NULL;

    /* Gates 4-8 all converge on one shared code word (0x80410902). With the
     * mapping of the caller's registers into 0x209C's, the dispatcher's r5 = the caller's r6 (elem
     * size), r6 = the caller's r7 (depth) and r7 = the caller's r8 (direction):
     *   depth == 0                     -> the depth-zero path (r27 = 0x209C's r6)
     *   elem_size > 0x4000             -> the shared word
     *   elem_size & 0xF                -> the shared word
     *   depth & 0xFFFF0000             -> the shared word
     *   direction > 3                  -> the shared word
     * The BUFFER (0x209C's r4 = the caller's r5) is only alignment-checked, at gate 10. */
    if (code == PS3_SPURS_LFQ_OK) {
        if (request->depth == 0u) {
            code = PS3_SPURS_LFQ_INVAL;
        } else if (request->elem_size > 0x4000u) {
            code = PS3_SPURS_LFQ_INVAL;
        } else if ((request->elem_size & 0xFu) != 0u) {
            code = PS3_SPURS_LFQ_INVAL;
        } else if ((request->depth & 0xFFFF0000u) != 0u) {
            code = PS3_SPURS_LFQ_INVAL;
        } else if (request->direction > 3u) {
            code = PS3_SPURS_LFQ_INVAL;
        }
    }

    /* Gates 9 and 10 share one code word -> 0x80410910:
     *   gate 9 -- the
     *        OWNER, masked to its low 7 bits with the tag bit 0 already dropped by 0x209C's caller
     *   gate 10 -- r26 is 0x209C's r4, i.e. the
     *        caller's r5 (the BUFFER), masked `& 0xF`.
     * The QUEUE (the caller's r4 = 0x209C's r3 = r28/r30) is not alignment-checked at all here. */
    if (code == PS3_SPURS_LFQ_OK) {
        if ((request->owner & ~1u & 0x7Fu) != 0u) {
            code = PS3_SPURS_LFQ_ALIGN;
        } else if ((request->buffer & 0xFu) != 0u) {
            code = PS3_SPURS_LFQ_ALIGN;
        }
    }

    *r3 = code;
    return code == PS3_SPURS_LFQ_OK ? PS3_SPURS_LFQ_STATUS_OK : PS3_SPURS_LFQ_STATUS_REFUSED;
}

PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_version_gate(const spurs_lfq_request *request,
    uint32_t version_field, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    /* The spurs arm reads the owner's version word and compares it against 0x1F:
     * the spurs arm keeps <= 0x1F and refuses above with 0x80410902. The taskset arm's word is never
     * gated -- it loads taskset+0x24 and only compares it to 3; on the builder
     * branch it is not looked at again at all. */
    if (ps3_spurs_lfq_owner_of(request->owner) == PS3_SPURS_LFQ_OWNER_TASKSET) {
        *r3 = PS3_SPURS_LFQ_OK;
        return PS3_SPURS_LFQ_STATUS_OK;
    }
    if (version_field > PS3_SPURS_LFQ_SPURS_VERSION_MAX) {
        *r3 = PS3_SPURS_LFQ_INVAL;
        return PS3_SPURS_LFQ_STATUS_REFUSED;
    }
    *r3 = PS3_SPURS_LFQ_OK;
    return PS3_SPURS_LFQ_STATUS_OK;
}

PS3_SPURS_LFQ_FN int ps3_spurs_lfq_uses_spurs_shape(const spurs_lfq_request *request,
    uint32_t version_field)
{
    if (!request) return 0;
    /* The taskset arm loads the owner's +0x24 word and compares it against 3.
     * The spurs arm never reads this field (its in-range branch goes to the dispatcher 0x209C, not to
     * the taskset path), so its shape is always the CellSyncLFQueue one.
     *
     * The dispatcher hands the builder `1` on this branch, or `2` on the 0x3A18
     * branch (0x3A18 sets it). The value 3 never reaches the builder, so its `r8 == 3` test
     * is false and the CellSyncLFQueue tail is the one written. */
    if (ps3_spurs_lfq_owner_of(request->owner) != PS3_SPURS_LFQ_OWNER_TASKSET) return 0;
    return version_field == PS3_SPURS_LFQ_VERSION_SPURS ? 1 : 0;
}

/* ------------------------------------------------------------------------------------ */
/* The 128-byte line                                                                     */
/* ------------------------------------------------------------------------------------ */

PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_image_in_place(const spurs_lfq_request *request,
    uint32_t version_field, unsigned char *io, size_t capacity)
{
    if (!request || !io || capacity < PS3_SPURS_LFQ_BYTES)
        return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;

    /* The ten scalars, in the order 0x1FA0..0x2020 writes them. "Zero" fields are written because
     * the firmware writes them; here they are explicit stores over the caller's bytes. */
    if (ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H0, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_ELEM_SIZE, 4u,
            request->elem_size) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H1, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_DEPTH, 4u, request->depth) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H6, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_BUFFER, 8u,
            request->buffer) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H8, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_DIRECTION, 4u,
            request->direction) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H10, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_H14, 2u, 0u) != 0 ||
        ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_AUX_BASE, 2u, 0u) != 0)
        return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;

    /* 0x201C/0x2020: the owner, TAGGED, as a u64. */
    {
        uint32_t field = 0;
        if (ps3_spurs_lfq_owner_field(request->owner, &field) != 0)
            return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
        if (ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_OWNER, 8u, field) != 0)
            return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    }

    if (ps3_spurs_lfq_uses_spurs_shape(request, version_field)) {
        /* 0x2050..0x2098. +0x18 keeps the buffer value and gains the low bit (0x2068/0x2074), so
         * the u64 at +0x18 is `buffer | 1`. */
        if (ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_BUFFER, 8u,
                (uint64_t)request->buffer | 1u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, 0x04u, 2u, 0u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_B21, 1u, 0xFFu) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_AUX2_BASE, 2u, 0xFFFFu) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, 0x0Cu, 2u, 0u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_B20, 1u, 0xFFu) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_V28, 4u, 0xFFFFFFFFu) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_AUX_BASE, 2u, 0xFFFFu) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_V78, 4u, 0u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_V7C, 4u, 0u) != 0)
            return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    } else {
        /* 0x2028..0x2048: +0x28 = 0, +0x78 = 0 (u64), +0x20 = 0xFFFFFFFF. */
        if (ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_V28, 4u, 0u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_V78, 8u, 0u) != 0 ||
            ps3_spurs_lfq_put_be(io, capacity, PS3_SPURS_LFQ_B20, 4u, 0xFFFFFFFFu) != 0)
            return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    }
    return PS3_SPURS_LFQ_STATUS_OK;
}

PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_image(const spurs_lfq_request *request,
    uint32_t version_field, unsigned char *out, size_t capacity)
{
    if (!out || capacity < PS3_SPURS_LFQ_BYTES) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    /* The empty state: the whole line zero, then the fields the firmware writes. */
    for (size_t index = 0; index < PS3_SPURS_LFQ_BYTES; ++index) out[index] = 0;
    return ps3_spurs_lfq_image_in_place(request, version_field, out, capacity);
}

/* ------------------------------------------------------------------------------------ */
/* The two auxiliary runs                                                                */
/* ------------------------------------------------------------------------------------ */

PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_aux_image(unsigned index, unsigned char *out,
    size_t capacity)
{
    /* The aux loops step by 2 bytes (15 iterations each):
     * run 0 is `queue+0x30 + 2*i`, run 1 is `queue+0x50 + 2*i`, i = 0..14. The
     * first loop's initial store writes 0x30 once before it starts. */
    if (!out || capacity < PS3_SPURS_LFQ_AUX_RUN_BYTES) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    if (index > 1u) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    for (size_t item = 0; item < PS3_SPURS_LFQ_AUX_RUN_BYTES; ++item) out[item] = 0;
    return PS3_SPURS_LFQ_STATUS_OK;
}

PS3_SPURS_LFQ_FN int ps3_spurs_lfq_aux_address(uint32_t queue, unsigned index, uint32_t *address)
{
    if (!address || index > 1u) return -1;
    const uint64_t base = (uint64_t)queue + (index ? PS3_SPURS_LFQ_AUX2_BASE : PS3_SPURS_LFQ_AUX_BASE);
    if (base + PS3_SPURS_LFQ_AUX_RUN_BYTES > UINT64_C(0x100000000)) return -1;
    *address = (uint32_t)base;
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* The fold, the state word                                                              */
/* ------------------------------------------------------------------------------------ */

PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_collapse(uint32_t probe_code)
{
    /* The probe codes are recognised by XORing the HIGH half of the code with 0x8041 and comparing the
     * result with 2306 / 2319 (0x902 / 0x90F). The XOR touches the HIGH half, so the comparison is
     * against the WHOLE 32-bit code: 0x80410902 and 0x8041090F
     * (not the probe's core-namespace spellings) both land on the 0x914 word. */
    if (probe_code == PS3_SPURS_LFQ_INVAL || probe_code == PS3_SPURS_LFQ_STAT)
        return PS3_SPURS_LFQ_RETRY;
    /* The code is kept as it came,
     * and only a NEGATIVE code (the sign test not taken) is replaced by
     * `(code & 0xFF) | 0x80410900` -- a mask of the low byte (bits 24..31 in PowerPC numbering). A
     * non-negative code is returned unchanged. */
    if ((int32_t)probe_code >= 0) return probe_code;
    return (probe_code & 0x000000FFu) | 0x80410900u;
}

/* What the entry does with the object, decided by the STATE word at queue+0x2C.
 *
 * The dispatcher 0x209C probes that word before anything is written:
 *   a reservation load of queue+0x2C, a zero test, and on zero either a CAS of
 *   1 or, past the SPU-count bound, the 16-word non-zero scan
 *   scan. It then decides from the word in r11:
 *     state == 2 -> STATE_MATCH (validation only)
 *     the builder call, r8 = the TAGGED owner
 *     then 2 is stored at queue+0x2C: the object is now initialized
 *     r31 = 0, so r3 = 0
 *
 * inFAMOUS's taskset is rebuilt from zero every run and only cellSpursCreateTasksetWithAttribute
 * writes it (g13 image: 0x60, 0x68, 0x70, 0x72, 0x74, 0x100+wid*128, 0x1890), so queue+0x2C is 0.
 * The FIRST call therefore takes the builder branch and returns 0. A SECOND call on the same object reads 2 and
 * is validation-only: it compares the line's own +0x10/+0x14/+0x18/+0x70/+0x24 against
 * the arguments and writes nothing, returning 0 when they agree. */
typedef enum spurs_lfq_state {
    PS3_SPURS_LFQ_STATE_REBUILD = 0,   /* queue+0x2C != 2: the builder runs and 2 is stored */
    PS3_SPURS_LFQ_STATE_MATCH = 1      /* queue+0x2C == 2: validation only, nothing written */
} spurs_lfq_state;

#define PS3_SPURS_LFQ_PROBE_REFUSED 0x80410109u

/* The dispatcher's decision, from the word it read. */
PS3_SPURS_LFQ_FN spurs_lfq_state ps3_spurs_lfq_state_of(uint32_t state_word)
{
    return state_word == PS3_SPURS_LFQ_STATE_INITIALIZED ? PS3_SPURS_LFQ_STATE_MATCH
                                                         : PS3_SPURS_LFQ_STATE_REBUILD;
}

/* The validation-only arm. `line` is the caller's own 128 bytes. Returns 0 when
 * every field the firmware compares agrees, else 0x80410902 -- the word gates 3-7 share, reached
 * from all five compare-and-branch sites. */
PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_match(const spurs_lfq_request *request,
    const unsigned char *line, size_t capacity)
{
    if (!request || !line || capacity < PS3_SPURS_LFQ_BYTES) return PS3_SPURS_LFQ_INVAL;
    /* The +0x10 word is compared as a 32-bit value;
     * the argument side keeps its low 32 bits (a masked copy of the caller's register). */
    if (ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_ELEM_SIZE, 4u) != request->elem_size)
        return PS3_SPURS_LFQ_INVAL;
    /* The +0x14 word is compared the same way. */
    if (ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_DEPTH, 4u) != request->depth)
        return PS3_SPURS_LFQ_INVAL;
    /* The +0x18 word is compared as a 64-bit value (the argument's low 32 bits). */
    if (ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_BUFFER, 8u) != (uint64_t)request->buffer)
        return PS3_SPURS_LFQ_INVAL;
    /* The +0x70 owner word is compared as a 64-bit value (the argument's low 32 bits). */
    if (ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_OWNER, 8u) != (uint64_t)request->owner)
        return PS3_SPURS_LFQ_INVAL;
    /* The +0x24 direction word is compared as a 32-bit value. */
    if (ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_DIRECTION, 4u) != request->direction)
        return PS3_SPURS_LFQ_INVAL;
    return PS3_SPURS_LFQ_OK;
}

/* The code the entry RETURNS for a call that passed every gate.
 *
 * On inFAMOUS's arguments this is **0**, and for a structural reason: the builder branch
 * (the builder branch) ends by returning r31, which
 * 0x214C loaded as 0x80410111 and the gate chain never overwrote on the way in. **No probe runs on
 * this path**: 0x24F4, 0x30B8, 0x2708 and 0x3A18 are only ever entered from entry 2 (0x17344, the
 * setter) and from 0x174F8, never from 0x171B8..0x17340.
 *
 * `line` is the caller's 128 bytes as they were BEFORE this call wrote them. */
PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_result(const spurs_lfq_request *request, uint32_t version_field,
    const unsigned char *line, size_t capacity)
{
    if (!request || !line || capacity < PS3_SPURS_LFQ_BYTES) return PS3_SPURS_LFQ_INVAL;
    (void)version_field;   /* the shape changes the line's tail, not the returned code */
    /* 0x226C/0x2270: an object that does not already hold 2 is rebuilt and 0 is returned. */
    if (ps3_spurs_lfq_state_of((uint32_t)ps3_spurs_lfq_field(line, capacity, PS3_SPURS_LFQ_STATE,
            4u)) == PS3_SPURS_LFQ_STATE_REBUILD)
        return PS3_SPURS_LFQ_OK;
    /* 0x22E4..0x2338: otherwise the firmware only validates what is already there. */
    return ps3_spurs_lfq_match(request, line, capacity);
}

/* g14's reading of what a push attempt WOULD answer, kept for the setter that reaches
 * those probes. It is NOT part of _cellSpursLFQueueInitialize and nothing on inFAMOUS's path calls
 * it; see CONTRACT-CORRECTION §4 for why g14's use of it here was the wrong step. */
PS3_SPURS_LFQ_FN uint32_t ps3_spurs_lfq_probe_result(const spurs_lfq_request *request,
    uint32_t version_field)
{
    if (!request) return PS3_SPURS_LFQ_PROBE_REFUSED;
    if (ps3_spurs_lfq_uses_spurs_shape(request, version_field)) return 0u;
    if (request->elem_size == 3u)
        return request->direction == 1u ? 0u : ps3_spurs_lfq_collapse(PS3_SPURS_LFQ_PROBE_REFUSED);
    return request->direction == 2u ? 0u : ps3_spurs_lfq_collapse(PS3_SPURS_LFQ_PROBE_REFUSED);
}

PS3_SPURS_LFQ_FN spurs_lfq_status ps3_spurs_lfq_state_image(unsigned char *out, size_t capacity)
{
    if (!out || capacity < 2u) return PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
    return ps3_spurs_lfq_put_be(out, capacity, 0, 2u, 0u) == 0 ? PS3_SPURS_LFQ_STATUS_OK
                                                              : PS3_SPURS_LFQ_STATUS_BAD_ARGUMENT;
}

#ifdef __cplusplus
}
#endif

#endif /* SPURS_LFQUEUE_H */
