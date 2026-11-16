#ifndef SPURS_TASK_CREATE_H
#define SPURS_TASK_CREATE_H
/* Platform-free plan builder for cellSpursCreateTask, NID 0xBEB600AC.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6).
 *
 *   cellSpursCreateTask        0x12520   entry gates, 0x11ACC, 0x10C6C, *taskId = slot
 *   validation + allocation    0x11ACC   ELF/context/size gates, slot sweep, six record stores
 *   signal                     0x10C6C   bit in taskset+0x20, 0xA750, 0x85D0
 *
 * POINTERS, NOT VALUES (g17c fix 1). The firmware dereferences three of its arguments:
 *   r28 = r9 (argument): read as two 64-bit words -- a 16-byte block.
 *   r27 = r25 = r8 (lsPattern): a 16-byte block read as a word and two 64-bit words;
 *                        its first word is ALSO the popcount probe and the 0xFC000000 gate.
 * So the request carries the two EAs and the caller hands over the blocks it read from them.
 *
 * The unit touches no guest memory, keeps no state and makes no syscall.
 *
 * Every builder is `static inline` (PS3_SPURS_TKC), so a translation unit that includes this
 * header needs no extra object at link time. spurs_task_create.c instantiates the same functions
 * once as external symbols for the host test and the Switch compile check. */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------------------------ */
/* Firmware constants                      */
/* ------------------------------------------------------------------------------------ */

#define PS3_SPURS_TKC_TASKSET_ALIGN 0x7Fu
#define PS3_SPURS_TKC_TASK_ALIGN 0xFu         /* elf & 0xF, context & 0xF  */
#define PS3_SPURS_TKC_MIN_SIZE 0x3FFu
#define PS3_SPURS_TKC_MAX_ELF_BYTES 0x3D400u
#define PS3_SPURS_TKC_ALIGN_BUDGET 122u
#define PS3_SPURS_TKC_ALIGN_SHIFT 11
#define PS3_SPURS_TKC_ALIGN_BASELINE 1024u
#define PS3_SPURS_TKC_LS_PATTERN_MASK 0xFC000000u

/* The firmware builds this doubleword in r0 and compares it with the first eight
 * guest bytes at the ELF EA. Big-endian 7F 45 4C 46 01 02 01 00:
 * EI_MAG 7F454C46, EI_CLASS 1 (ELFCLASS32), EI_DATA 2 (ELFDATA2MSB), EI_VERSION 1, EI_OSABI 0. */
#define PS3_SPURS_TKC_ELF_IDENT 0x7F454C4601020100ull

#define PS3_SPURS_TKC_EM_SPU 23u              /* e_machine +0x12  */
#define PS3_SPURS_TKC_ENTRY_MAX 0x2FFFu       /* e_entry +0x18 */
#define PS3_SPURS_TKC_PT_LOAD 1u
#define PS3_SPURS_TKC_SEGMENT_MAX 0x2FFFu     /* p_vaddr     */

#define PS3_SPURS_TKC_SLOT_COUNT 128u         /* four words of 32 bits each                   */
#define PS3_SPURS_TKC_SET_WORDS 4u
#define PS3_SPURS_TKC_WORD_BYTES 4u
#define PS3_SPURS_TKC_RECORD_BASE 0x80u       /* g11 places the record array at +0x80         */
#define PS3_SPURS_TKC_RECORD_BYTES 48u        /* R = slot*48               */
#define PS3_SPURS_TKC_SET_ALLOCATED 0x30u
#define PS3_SPURS_TKC_SET_READY 0x20u

/* The 16-byte blocks the firmware loads through its two pointer arguments. */
#define PS3_SPURS_TKC_BLOCK_BYTES 16u

/* Record field offsets relative to `taskset + 0x80 + slot*48` (CONTRACT §C.6, g17c fix 3). */
#define PS3_SPURS_TKC_RECORD_ARGUMENT 0x00u
#define PS3_SPURS_TKC_RECORD_ARGUMENT_HI 0x08u
#define PS3_SPURS_TKC_RECORD_ELF 0x10u
#define PS3_SPURS_TKC_RECORD_CONTEXT 0x18u
#define PS3_SPURS_TKC_RECORD_LS_PATTERN 0x20u
#define PS3_SPURS_TKC_RECORD_LS_HI 0x28u

#define PS3_SPURS_TKC_READY_COUNT_PRIMARY 0x70u   /* ready-count word for wid <= 15   */
#define PS3_SPURS_TKC_READY_COUNT_SECONDARY 0x78u /* ready-count word for wid > 15    */
#define PS3_SPURS_TKC_SIGNAL_BYTE 0xD64u
#define PS3_SPURS_TKC_EXCEPTION_WORD 0xD6Cu
#define PS3_SPURS_TKC_TASKSET_SPURS 0x64u
#define PS3_SPURS_TKC_TASKSET_WID 0x74u
#define PS3_SPURS_TKC_PRIMARY_WORKLOADS 16u       /* workloads in the primary range (wid <= 15)               */

/* ------------------------------------------------------------------------------------ */
/* Result codes (every path sign-extends its result)                                */
/* ------------------------------------------------------------------------------------ */

#define PS3_SPURS_TKC_OK 0u
#define PS3_SPURS_TKC_NULL 0x80410911u
#define PS3_SPURS_TKC_ALIGN 0x80410910u
#define PS3_SPURS_TKC_INVAL 0x80410902u
#define PS3_SPURS_TKC_AGAIN 0x80410901u   /* no free slot                        */
#define PS3_SPURS_TKC_STAT 0x8041090Fu    /* 0xA750/0x85D0 answered 0x8041080F   */
#define PS3_SPURS_TKC_ELF_BAD 0x80410907u /* the ELF header gates      */

#define PS3_SPURS_TKC_POLICY_STAT 0x8041080Fu

typedef enum spurs_task_create_status {
    PS3_SPURS_TKC_STATUS_OK = 0,
    PS3_SPURS_TKC_STATUS_REFUSED = 1,
    PS3_SPURS_TKC_STATUS_BAD_ARGUMENT = 2
} spurs_task_create_status;

/* One program header, as the caller read it out of guest memory. */
typedef struct spurs_task_elf_segment {
    uint32_t type;      /* phdr + 0x0 */
    uint32_t file_size; /* phdr + 0x8 */
} spurs_task_elf_segment;

typedef struct spurs_task_elf {
    uint64_t ident;             /* elf + 0x00                    */
    uint16_t machine;           /* elf + 0x12                  */
    uint32_t entry;             /* elf + 0x18                  */
    uint16_t ph_entry_size;     /* elf + 0x2A                  */
    uint16_t ph_num;            /* elf + 0x2C                  */
    uint32_t ph_offset;         /* elf + 0x1C                  */
    const spurs_task_elf_segment *segments;
    size_t segment_count;
} spurs_task_elf;

typedef struct spurs_task_create_request {
    uint32_t taskset;
    uint32_t task_id_ptr;      /* the CALLER's pointer; written only after the signal */
    uint32_t elf_address;
    uint64_t context;
    uint32_t size;
    uint32_t argument_ptr;     /* r9 -> a 16-byte block read by 0x11E94/0x11EA0                */
    uint32_t ls_pattern_ptr;   /* r8 -> a 16-byte block read by 0x11C38/0x11EC4/0x11ED0       */
    spurs_task_elf elf;
    /* The blocks behind the two pointers, as the caller read them. Only used on the branches the
     * firmware dereferences them on. */
    unsigned char argument_block[PS3_SPURS_TKC_BLOCK_BYTES];
    unsigned char ls_pattern_block[PS3_SPURS_TKC_BLOCK_BYTES];
    uint32_t allocated_words[PS3_SPURS_TKC_SET_WORDS];
    uint32_t ready_words[PS3_SPURS_TKC_SET_WORDS];
} spurs_task_create_request;

typedef struct spurs_task_write {
    uint32_t address;
    unsigned char bytes[8];
    unsigned char length;
} spurs_task_write;

typedef struct spurs_task_signal {
    uint32_t taskset;
    uint32_t spurs;
    uint32_t workload_id;
    uint32_t ready_count_address;
    uint32_t ready_mask_bit;
    uint32_t signal_address;
    uint32_t exception_address;
} spurs_task_signal;

typedef struct spurs_task_create_plan {
    uint32_t result_code;
    unsigned slot;
    spurs_task_write writes[8];
    size_t write_count;
    spurs_task_signal signal;
    unsigned has_signal;
} spurs_task_create_plan;

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PS3_SPURS_TKC
#define PS3_SPURS_TKC static inline
#endif

/* The sign extension of every firmware epilogue. */
PS3_SPURS_TKC uint64_t ps3_spurs_task_sign_extend(uint32_t code)
{
    return (uint64_t)(int64_t)(int32_t)code;
}

PS3_SPURS_TKC int ps3_spurs_task_put_be(unsigned char *out, size_t capacity, uint64_t value,
    size_t width)
{
    if (!out) return -1;
    if (width != 1u && width != 2u && width != 4u && width != 8u) return -1;
    if (width > capacity) return -1;
    for (size_t index = 0; index < width; ++index)
        out[index] = (unsigned char)(value >> (8u * (width - 1u - index)));
    return 0;
}

PS3_SPURS_TKC uint64_t ps3_spurs_task_load_be(const unsigned char *bytes, size_t width)
{
    uint64_t value = 0;
    for (size_t index = 0; index < width; ++index) value = value << 8 | bytes[index];
    return value;
}

/* The alignment budget: `size > 0x3D400` keeps the literal 122; otherwise `(size - 1024) >> 11`.
 * For the game's size 0xC00 this is 1, so the stored context word at +0x18 is `context | 1`
 */
PS3_SPURS_TKC uint32_t ps3_spurs_task_alignment_budget(uint32_t size)
{
    if (size > PS3_SPURS_TKC_MAX_ELF_BYTES) return PS3_SPURS_TKC_ALIGN_BUDGET;
    return (size - PS3_SPURS_TKC_ALIGN_BASELINE) >> PS3_SPURS_TKC_ALIGN_SHIFT;
}

/* The lsPattern population count, folded with the masks 0x11C3C..0x11D68 builds, over the 16
 * GUEST BYTES behind r25/r27 -- not over the pointer (g17c fix 1). */
static inline uint32_t ps3_spurs_task_popcount_word(uint32_t word)
{
    uint32_t value = word;
    value = value - ((value >> 1) & 0x55555555u);
    value = (value & 0x33333333u) + ((value >> 2) & 0x33333333u);
    value = (value + (value >> 4)) & 0x0F0F0F0Fu;
    value = value + (value >> 8);
    value = value + (value >> 16);
    return value & 0x3Fu;
}
PS3_SPURS_TKC uint32_t ps3_spurs_task_ls_pattern_count(const unsigned char *block)
{
    if (!block) return 0u;
    return ps3_spurs_task_popcount_word((uint32_t)ps3_spurs_task_load_be(block, 4u)) +
           ps3_spurs_task_popcount_word((uint32_t)ps3_spurs_task_load_be(block + 4, 4u)) +
           ps3_spurs_task_popcount_word((uint32_t)ps3_spurs_task_load_be(block + 8, 4u)) +
           ps3_spurs_task_popcount_word((uint32_t)ps3_spurs_task_load_be(block + 12, 4u));
}

/* For each program header: a non-PT_LOAD header just advances the walk
 * to the next header; a PT_LOAD with `p_vaddr > 0x2FFF` continues
 * and one with `p_vaddr <= 0x2FFF` answers 0x80410907.
 * `ph_num == 0` skips the walk entirely. */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_segments_ok(
    const spurs_task_elf *elf, uint32_t *r3)
{
    if (!elf || !r3) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    if (elf->ph_num != 0u && (!elf->segments || elf->segment_count < elf->ph_num)) {
        *r3 = PS3_SPURS_TKC_ELF_BAD;
        return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    }
    uint32_t code = PS3_SPURS_TKC_OK;
    for (size_t index = 0; index < elf->ph_num; ++index) {
        if (elf->segments[index].type != PS3_SPURS_TKC_PT_LOAD) continue;
        if (elf->segments[index].file_size > PS3_SPURS_TKC_SEGMENT_MAX) continue;
        code = PS3_SPURS_TKC_ELF_BAD;
        break;
    }
    *r3 = code;
    return code == PS3_SPURS_TKC_OK ? PS3_SPURS_TKC_STATUS_OK : PS3_SPURS_TKC_STATUS_REFUSED;
}

/* 0x12520's register gates: taskset == 0 -> 0x80410911, then
 * `taskset & 0x7F` -> 0x80410910. */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_create_validate_taskset(
    const spurs_task_create_request *request, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    uint32_t code = PS3_SPURS_TKC_OK;
    if (request->taskset == 0u) code = PS3_SPURS_TKC_NULL;
    else if ((request->taskset & PS3_SPURS_TKC_TASKSET_ALIGN) != 0u) code = PS3_SPURS_TKC_ALIGN;
    *r3 = code;
    return code == PS3_SPURS_TKC_OK ? PS3_SPURS_TKC_STATUS_OK : PS3_SPURS_TKC_STATUS_REFUSED;
}

/* 0x11ACC's gates 1..10 in firmware order, first failure wins:
 *   1. elf == 0 -> 0x80410911
 *   2. elf & 0xF -> 0x80410910
 *   3. context & 0xF -> 0x80410910
 *   4. context != 0 && size <= 0x3FF -> 0x80410902
 *   5. ident != 7F454C4601020100 -> 0x80410907
 *   6. machine != EM_SPU -> 0x80410907
 *   7. entry <= 0x2FFF                 -> 0x80410907  (0x11DA0/0x11DA4; tasks link above the
 *                                         0x0..0x2FFF policy-module area)
 *   8. a PT_LOAD with p_vaddr <= 0x2FFF  -> 0x80410907 (0x11EF8..0x11F04; phdr +0x8 is p_vaddr)
 *   9. lsPattern count > budget -> 0x80410902
 *  10. lsPattern first word & 0xFC000000 -> 0x80410902
 *
 * Gates 4 and 7 read `ble`, taken when GT=0 or EQ=1: `size <= 0x3FF` and `entry <= 0x2FFF` are
 * the FAILING sides. Gates 9 and 10 run only when `context != 0` AND `ls_pattern_ptr != 0`:
 * a zero context takes the shortcut (r12 = 0), skipping the budget
 * and the probe; a non-zero context goes through both. */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_create_validate(
    const spurs_task_create_request *request, uint32_t *r3)
{
    if (!request || !r3) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    const spurs_task_elf *elf = &request->elf;
    uint32_t code = PS3_SPURS_TKC_OK;
    if (request->elf_address == 0u) {
        code = PS3_SPURS_TKC_NULL;
    } else if ((request->elf_address & PS3_SPURS_TKC_TASK_ALIGN) != 0u) {
        code = PS3_SPURS_TKC_ALIGN;
    } else if ((request->context & PS3_SPURS_TKC_TASK_ALIGN) != 0u) {
        code = PS3_SPURS_TKC_ALIGN;
    } else if (request->context != 0u && request->size <= PS3_SPURS_TKC_MIN_SIZE) {
        code = PS3_SPURS_TKC_INVAL;
    } else if (elf->ident != PS3_SPURS_TKC_ELF_IDENT) {
        code = PS3_SPURS_TKC_ELF_BAD;
    } else if (elf->machine != PS3_SPURS_TKC_EM_SPU) {
        code = PS3_SPURS_TKC_ELF_BAD;
    } else if (elf->entry <= PS3_SPURS_TKC_ENTRY_MAX) {   /* root g17e */
        code = PS3_SPURS_TKC_ELF_BAD;
    } else if (ps3_spurs_task_segments_ok(elf, &code) != PS3_SPURS_TKC_STATUS_OK) {
        /* code carries the segment verdict */
    } else if (request->context != 0u && request->ls_pattern_ptr != 0u &&
               ps3_spurs_task_ls_pattern_count(request->ls_pattern_block) >
                   ps3_spurs_task_alignment_budget(request->size)) {
        code = PS3_SPURS_TKC_INVAL;
    } else if (request->context != 0u && request->ls_pattern_ptr != 0u &&
               ((uint32_t)ps3_spurs_task_load_be(request->ls_pattern_block, 4u) &
                PS3_SPURS_TKC_LS_PATTERN_MASK) != 0u) {
        code = PS3_SPURS_TKC_INVAL;
    }
    *r3 = code;
    return code == PS3_SPURS_TKC_OK ? PS3_SPURS_TKC_STATUS_OK : PS3_SPURS_TKC_STATUS_REFUSED;
}

/* the free-slot search. Returns STATUS_OK with `*slot` (0..127), `*word` (which
 * of the four +0x30 words) and `*mask` (0x80000000 >> (slot & 31)); STATUS_REFUSED with
 * PS3_SPURS_TKC_AGAIN when all 128 bits are set. */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_find_slot(
    const spurs_task_create_request *request, unsigned *slot, unsigned *word, uint32_t *mask)
{
    if (!request || !slot || !word || !mask) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    for (unsigned index = 0; index < PS3_SPURS_TKC_SET_WORDS; ++index) {
        const uint32_t inverted = ~request->allocated_words[index];
        if (inverted == 0u) continue;   /* count-leading-zeros(0) = 32 > 31, next word */
        unsigned leading = 0;
        uint32_t probe = inverted;
        while ((probe & 0x80000000u) == 0u) { probe <<= 1; ++leading; }
        *word = index;
        *slot = index * 32u + leading;
        *mask = 0x80000000u >> (*slot & 31u);
        return PS3_SPURS_TKC_STATUS_OK;
    }
    return PS3_SPURS_TKC_STATUS_REFUSED;
}

PS3_SPURS_TKC int ps3_spurs_task_push(spurs_task_create_plan *plan, uint32_t address,
    uint64_t value, size_t width)
{
    if (!plan || plan->write_count >= sizeof plan->writes / sizeof plan->writes[0]) return -1;
    spurs_task_write *write = &plan->writes[plan->write_count];
    write->address = address;
    write->length = (unsigned char)width;
    if (ps3_spurs_task_put_be(write->bytes, sizeof write->bytes, value, width) != 0) return -1;
    ++plan->write_count;
    return 0;
}

/* Builds the plan. The seven writes, in the firmware's order:
 *   1. R + 0x10  u64 elf
 *   2. R + 0x18  u64 context | r12
 *   3. R + 0x00  u64 argument[0..7]  (zeros if r24 == 0)
 *   4. R + 0x08  u64 argument[8..15]  (zeros if r24 == 0)
 *   5. R + 0x20  u64 ls[0..7]  (zeros if cr6 or !r25)
 *   6. R + 0x28  u64 ls[8..15]  (zeros in the same case)
 *   7. taskset+0x30+word*4  u32 allocated | mask
 * plus the ready bit at taskset+0x20 set by the signal step.
 *
 * The caller's `*taskId` is NOT a plan action (g17c fix 4): the firmware stores to the entry's own
 * frame slot (r23 = r4, a frame address) and the caller's pointer is written only
 * after the signal step returned 0. The adapter performs that store itself, once, last. */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_create_plan_build(
    const spurs_task_create_request *request, spurs_task_create_plan *plan)
{
    if (!request || !plan) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    plan->write_count = 0;
    plan->has_signal = 0;
    plan->slot = 0;
    uint32_t code = 0;
    if (ps3_spurs_task_create_validate_taskset(request, &code) != PS3_SPURS_TKC_STATUS_OK) {
        plan->result_code = code;
        return PS3_SPURS_TKC_STATUS_REFUSED;
    }
    if (ps3_spurs_task_create_validate(request, &code) != PS3_SPURS_TKC_STATUS_OK) {
        plan->result_code = code;
        return PS3_SPURS_TKC_STATUS_REFUSED;
    }
    unsigned slot = 0, word = 0;
    uint32_t mask = 0;
    if (ps3_spurs_task_find_slot(request, &slot, &word, &mask) != PS3_SPURS_TKC_STATUS_OK) {
        plan->result_code = PS3_SPURS_TKC_AGAIN;
        return PS3_SPURS_TKC_STATUS_REFUSED;
    }
    plan->slot = slot;
    const uint32_t record = request->taskset + PS3_SPURS_TKC_RECORD_BASE +
        slot * PS3_SPURS_TKC_RECORD_BYTES;
    /* a NULL argument or lsPattern pointer stores eight zero bytes at the same
     * offsets it would otherwise load. Modelled as the write the dump performs. */
    const uint64_t argument_lo = request->argument_ptr != 0u
        ? ps3_spurs_task_load_be(request->argument_block, 8u) : 0u;
    const uint64_t argument_hi = request->argument_ptr != 0u
        ? ps3_spurs_task_load_be(request->argument_block + 8, 8u) : 0u;
    const bool writes_ls = request->context != 0u && request->ls_pattern_ptr != 0u;
    const uint64_t ls_lo = writes_ls
        ? ps3_spurs_task_load_be(request->ls_pattern_block, 8u) : 0u;
    const uint64_t ls_hi = writes_ls
        ? ps3_spurs_task_load_be(request->ls_pattern_block + 8, 8u) : 0u;
    /* a zero context stores 0, not the budget. */
    const uint64_t context_word = request->context == 0u ? 0u : (request->context |
        (uint64_t)ps3_spurs_task_alignment_budget(request->size));
    if (ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_ELF, request->elf_address, 8u) != 0 ||
        ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_CONTEXT, context_word, 8u) != 0 ||
        ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_ARGUMENT, argument_lo, 8u) != 0 ||
        ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_ARGUMENT_HI, argument_hi, 8u) != 0 ||
        ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_LS_PATTERN, ls_lo, 8u) != 0 ||
        ps3_spurs_task_push(plan, record + PS3_SPURS_TKC_RECORD_LS_HI, ls_hi, 8u) != 0 ||
        ps3_spurs_task_push(plan, request->taskset + PS3_SPURS_TKC_SET_ALLOCATED +
            word * PS3_SPURS_TKC_WORD_BYTES,
            request->allocated_words[word] | mask, 4u) != 0 ||
        ps3_spurs_task_push(plan, request->taskset + PS3_SPURS_TKC_SET_READY +
            (slot >> 5) * PS3_SPURS_TKC_WORD_BYTES,
            request->ready_words[slot >> 5] | (0x80000000u >> (slot & 31u)), 4u) != 0)
        return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    plan->result_code = PS3_SPURS_TKC_OK;
    return PS3_SPURS_TKC_STATUS_OK;
}

/* Fills the native signal request from the taskset's own fields:
 *   wid    = *(u32*)(taskset+0x74)
 *   spurs  = *(u32*)(taskset+0x64)
 *   ready  = spurs + (wid <= 15 ? 0x70 : 0x78)
 *   bit    = 0x80000000 >> (wid & 31)
 *   signal = spurs + 0xD64            0x8634
 *   except = spurs + 0xD6C            0x8618 */
PS3_SPURS_TKC spurs_task_create_status ps3_spurs_task_signal_build(uint32_t taskset, uint32_t spurs,
    uint32_t workload_id, spurs_task_signal *signal)
{
    if (!signal) return PS3_SPURS_TKC_STATUS_BAD_ARGUMENT;
    if (spurs == 0u) return PS3_SPURS_TKC_STATUS_REFUSED;   /* 0xA758 -> 0x80410811 */
    if ((spurs & PS3_SPURS_TKC_TASKSET_ALIGN) != 0u) return PS3_SPURS_TKC_STATUS_REFUSED;
    if (workload_id >= PS3_SPURS_TKC_SLOT_COUNT) return PS3_SPURS_TKC_STATUS_REFUSED;
    const uint64_t ready_base = (uint64_t)spurs +
        (workload_id < PS3_SPURS_TKC_PRIMARY_WORKLOADS ? PS3_SPURS_TKC_READY_COUNT_PRIMARY
                                                       : PS3_SPURS_TKC_READY_COUNT_SECONDARY);
    if (ready_base > 0xFFFFFFFFu) return PS3_SPURS_TKC_STATUS_REFUSED;
    signal->taskset = taskset;
    signal->spurs = spurs;
    signal->workload_id = workload_id;
    signal->ready_count_address = (uint32_t)ready_base;
    signal->ready_mask_bit = 0x80000000u >> (workload_id & 31u);
    signal->signal_address = spurs + PS3_SPURS_TKC_SIGNAL_BYTE;
    signal->exception_address = spurs + PS3_SPURS_TKC_EXCEPTION_WORD;
    return PS3_SPURS_TKC_STATUS_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* SPURS_TASK_CREATE_H */
