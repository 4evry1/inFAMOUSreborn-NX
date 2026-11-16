/* Host test for native/domain/spurs_task_create.h: cellSpursCreateTask, NID 0xBEB600AC,
 * behaviour taken from the firmware library libsre.
 *
 * Every fixture is built from the game's real call and from the objects the EARLIER native calls
 * produced: the taskset EA is a 128-byte-aligned object of the size
 * cellSpursCreateTasksetWithAttribute writes (6400), the record slot is derived the same way, the
 * ELF header is a minimal valid SPU ELF32 big-endian header, and `r8`/`r9` are real guest POINTERS
 * to 16-byte blocks -- r9 = &frame[0x78] holding the size word, r8 = a TOC-loaded pointer to the
 * lsPattern block. No fixture aliases two different guest objects (the g15 lesson). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "spurs_task_create.h"

static int failures;
static int checks;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } \
} while (0)
#define CHECK_EQ(actual, expected) do { \
    ++checks; \
    const unsigned long long a_ = (unsigned long long)(actual), e_ = (unsigned long long)(expected); \
    if (a_ != e_) { ++failures; \
        fprintf(stderr, "FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, \
            #actual, a_, e_); } \
} while (0)

/* The game's own numbers (ppu_recomp_001.cpp:596167 func_00424714 + func_0048B838). */
static const uint32_t TASKSET = 0x7D1800;      /* 128-byte aligned, g13's object EA            */
static const uint32_t TASK_ID_PTR = 0x7D0080;  /* manager+0x80                                 */
static const uint32_t ELF_EA = 0x7F0000;       /* 16-byte aligned                              */
static const uint32_t CONTEXT = 0x7D4000;      /* 16-byte aligned (context & 0xF gate)         */
static const uint32_t SIZE = 0xC00;            /* r7 = 0xC00 literal                           */
/* r9 = &frame[0x78]. The frame word at +0x78 is r5 of the caller (the size again), so the 16-byte
 * argument block's first doubleword is 0x0000000000000C00. */
static const uint32_t ARGUMENT_PTR = 0x7D1078;
static const uint64_t ARGUMENT_LO = 0x0000000000000C00ull;
static const uint64_t ARGUMENT_HI = 0;
/* r8 = a TOC-loaded pointer to the lsPattern block. */
static const uint32_t LS_PATTERN_PTR = 0x7D2000;
static const uint64_t LS_PATTERN_LO = 0;
static const uint64_t LS_PATTERN_HI = 0;
static const uint32_t SPURS = 0x7CF100;        /* 128-byte aligned CellSpurs                    */
static const uint32_t WID = 0;

/* 0x11E04 for size 0xC00: (0xC00 - 0x400) >> 11 = 1, so the stored context word at +0x18 is
 */
static const uint64_t EXPECTED_CONTEXT_WORD = 0x7D4001ull;

const spurs_task_elf_segment VALID_SEGMENTS[] = {
    {PS3_SPURS_TKC_PT_LOAD, 0x4000u},       /* the text segment: > 0x2FFF, the walk continues */
};

void put_be(unsigned char *block, size_t offset, size_t width, uint64_t value)
{
    for (size_t index = 0; index < width; ++index)
        block[offset + index] = (unsigned char)(value >> (8u * (width - 1u - index)));
}

spurs_task_create_request game_request()
{
    spurs_task_create_request request;
    memset(&request, 0, sizeof request);
    request.taskset = TASKSET;
    request.task_id_ptr = TASK_ID_PTR;
    request.elf_address = ELF_EA;
    request.context = CONTEXT;
    request.size = SIZE;
    request.argument_ptr = ARGUMENT_PTR;
    request.ls_pattern_ptr = LS_PATTERN_PTR;
    put_be(request.argument_block, 0x0, 8, ARGUMENT_LO);
    put_be(request.argument_block, 0x8, 8, ARGUMENT_HI);
    put_be(request.ls_pattern_block, 0x0, 8, LS_PATTERN_LO);
    put_be(request.ls_pattern_block, 0x8, 8, LS_PATTERN_HI);
    request.elf.ident = PS3_SPURS_TKC_ELF_IDENT;
    request.elf.machine = PS3_SPURS_TKC_EM_SPU;
    request.elf.entry = 0x3000u;   /* root g17e: a task entry must be above 0x2FFF */
    request.elf.ph_entry_size = 32u;
    request.elf.ph_num = 1u;
    request.elf.ph_offset = 0x34u;
    request.elf.segments = VALID_SEGMENTS;
    request.elf.segment_count = sizeof VALID_SEGMENTS / sizeof VALID_SEGMENTS[0];
    return request;
}

uint64_t decode_be(const unsigned char *bytes, size_t length)
{
    uint64_t value = 0;
    for (size_t index = 0; index < length; ++index) value = value << 8 | bytes[index];
    return value;
}

/* ---- the exact game path, and the exact writes it produces ----------------------------- */

void test_game_path_plan()
{
    spurs_task_create_request request = game_request();
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(plan.result_code, PS3_SPURS_TKC_OK);
    CHECK_EQ(plan.slot, 0u);
    /* EIGHT actions: six record fields, the
     * allocation mask and 0x10C6C's ready bit. The caller's *taskId is NOT one of them
     * (g17c fix 4). Byte total in the adapter: 6*8 + 4 + 4 + 4 (the *taskId store at 0x125D0) = 60. */
    CHECK_EQ(plan.write_count, 8u);
    const uint32_t record = TASKSET + PS3_SPURS_TKC_RECORD_BASE + 0u * PS3_SPURS_TKC_RECORD_BYTES;
    /* Firmware order: 0x11E84 elf, 0x11E88 context, 0x11E9C/0x11EB0 argument, 0x11ECC/0x11EE4 ls. */
    CHECK_EQ(plan.writes[0].address, record + PS3_SPURS_TKC_RECORD_ELF);
    CHECK_EQ(decode_be(plan.writes[0].bytes, plan.writes[0].length), ELF_EA);
    CHECK_EQ(plan.writes[1].address, record + PS3_SPURS_TKC_RECORD_CONTEXT);
    CHECK_EQ(decode_be(plan.writes[1].bytes, plan.writes[1].length), EXPECTED_CONTEXT_WORD);
    CHECK_EQ(plan.writes[2].address, record + PS3_SPURS_TKC_RECORD_ARGUMENT);
    CHECK_EQ(decode_be(plan.writes[2].bytes, plan.writes[2].length), ARGUMENT_LO);
    CHECK_EQ(plan.writes[3].address, record + PS3_SPURS_TKC_RECORD_ARGUMENT_HI);
    CHECK_EQ(decode_be(plan.writes[3].bytes, plan.writes[3].length), ARGUMENT_HI);
    CHECK_EQ(plan.writes[4].address, record + PS3_SPURS_TKC_RECORD_LS_PATTERN);
    CHECK_EQ(decode_be(plan.writes[4].bytes, plan.writes[4].length), LS_PATTERN_LO);
    CHECK_EQ(plan.writes[5].address, record + PS3_SPURS_TKC_RECORD_LS_HI);
    CHECK_EQ(decode_be(plan.writes[5].bytes, plan.writes[5].length), LS_PATTERN_HI);
    CHECK_EQ(plan.writes[6].address, TASKSET + PS3_SPURS_TKC_SET_ALLOCATED);
    CHECK_EQ(decode_be(plan.writes[6].bytes, plan.writes[6].length), 0x80000000u);
    CHECK_EQ(plan.has_signal, 0u);
    CHECK_EQ(plan.writes[7].address, TASKSET + PS3_SPURS_TKC_SET_READY);
    CHECK_EQ(decode_be(plan.writes[7].bytes, plan.writes[7].length), 0x80000000u);
}

/* The record offsets really are the six store sites, and nothing addresses the caller's id word. */
void test_plan_never_touches_caller_task_id()
{
    spurs_task_create_request request = game_request();
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_OK);
    for (size_t index = 0; index < plan.write_count; ++index)
        CHECK(plan.writes[index].address != TASK_ID_PTR);
}

/* A signal refusal must leave the caller's *taskId alone: the domain never plans that store, so
 * the property is structural -- but assert the plan's address set on a refused call too. */
void test_refusal_leaves_task_id_alone()
{
    spurs_task_create_request request = game_request();
    request.elf.machine = 21;                 /* not EM_SPU -> 0x80410907 */
    spurs_task_create_plan plan;
    memset(&plan, 0xA5, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_REFUSED);
    CHECK_EQ(plan.result_code, PS3_SPURS_TKC_ELF_BAD);
    CHECK_EQ(plan.write_count, 0u);
}

/* ---- the argument and lsPattern are POINTERS (g17c fix 1) ------------------------------ */

void test_null_pointers_store_zeroes()
{
    /* argument == 0 stores eight zero bytes at +0x00 and continues at 0x11EA4. */
    spurs_task_create_request request = game_request();
    request.argument_ptr = 0u;
    memset(request.argument_block, 0xFF, sizeof request.argument_block);
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(decode_be(plan.writes[2].bytes, plan.writes[2].length), 0u);
    CHECK_EQ(decode_be(plan.writes[3].bytes, plan.writes[3].length), 0u);

    /* context == 0 OR lsPattern == 0 stores eight zero bytes at +0x20. */
    spurs_task_create_request no_ls = game_request();
    no_ls.ls_pattern_ptr = 0u;
    memset(no_ls.ls_pattern_block, 0xFF, sizeof no_ls.ls_pattern_block);
    spurs_task_create_plan no_ls_plan;
    memset(&no_ls_plan, 0, sizeof no_ls_plan);
    CHECK(ps3_spurs_task_create_plan_build(&no_ls, &no_ls_plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(decode_be(no_ls_plan.writes[4].bytes, no_ls_plan.writes[4].length), 0u);
    CHECK_EQ(decode_be(no_ls_plan.writes[5].bytes, no_ls_plan.writes[5].length), 0u);

    spurs_task_create_request no_context = game_request();
    no_context.context = 0u;
    /* Root g17d: a block that FAILS both lsPattern gates (all bits set: the count exceeds the
     * size-0xC00 budget of 1 at 0x11D6C, and the first word has the 0xFC000000 bits at 0x11D74).
     * A zero context takes the shortcut and never runs either gate, so
     * the call is served. */
    memset(no_context.ls_pattern_block, 0xFF, sizeof no_context.ls_pattern_block);
    spurs_task_create_plan no_context_plan;
    memset(&no_context_plan, 0, sizeof no_context_plan);
    CHECK(ps3_spurs_task_create_plan_build(&no_context, &no_context_plan) == PS3_SPURS_TKC_STATUS_OK);
    /* cr6 (context == 0) sends 0x11EB4 to the zero branch, so +0x20 is zero too. */
    CHECK_EQ(decode_be(no_context_plan.writes[4].bytes, no_context_plan.writes[4].length), 0u);
    /* with context == 0 the stored word is 0 | 0 = 0 (root g17d). */
    CHECK_EQ(decode_be(no_context_plan.writes[1].bytes, no_context_plan.writes[1].length), 0u);
}

/* The popcount and the top-bits gate run on the BLOCK, not on the pointer. */
void test_ls_pattern_is_the_block()
{
    /* A pointer whose own value would trip the 0xFC000000 gate passes when the block is clear. */
    spurs_task_create_request request = game_request();
    request.ls_pattern_ptr = 0xFC000000u;
    CHECK(request.ls_pattern_ptr & PS3_SPURS_TKC_LS_PATTERN_MASK);
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_OK);

    /* And a clear pointer with a set block is refused by the 0x11D74 gate. */
    spurs_task_create_request blocked = game_request();
    blocked.ls_pattern_ptr = 0x1000u;
    put_be(blocked.ls_pattern_block, 0x0, 4, 0x04000000u);
    CHECK(ps3_spurs_task_create_plan_build(&blocked, &plan) == PS3_SPURS_TKC_STATUS_REFUSED);
    CHECK_EQ(plan.result_code, PS3_SPURS_TKC_INVAL);

    /* The budget gate counts the block's bits. */
    spurs_task_create_request dense = game_request();
    dense.size = 0x400u;                       /* budget = (0x400-0x400)>>11 = 0 */
    put_be(dense.ls_pattern_block, 0x0, 8, 1ull);
    CHECK(ps3_spurs_task_create_plan_build(&dense, &plan) == PS3_SPURS_TKC_STATUS_REFUSED);
    CHECK_EQ(plan.result_code, PS3_SPURS_TKC_INVAL);
}

/* ---- every refusal, effect-free ------------------------------------------------------- */

void test_refusals_are_effect_free()
{
    struct Case { const char *name; uint32_t expected; spurs_task_create_request request; };
    spurs_task_create_request base = game_request();
    struct Case cases[13];
    size_t count = 0;

    { spurs_task_create_request r = base; r.taskset = 0; cases[count].name = "taskset NULL"; cases[count].expected = PS3_SPURS_TKC_NULL; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.taskset = TASKSET + 4; cases[count].name = "taskset misaligned"; cases[count].expected = PS3_SPURS_TKC_ALIGN; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf_address = 0; cases[count].name = "elf NULL"; cases[count].expected = PS3_SPURS_TKC_NULL; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf_address = ELF_EA + 8; cases[count].name = "elf misaligned"; cases[count].expected = PS3_SPURS_TKC_ALIGN; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.context = CONTEXT + 8; cases[count].name = "context misaligned"; cases[count].expected = PS3_SPURS_TKC_ALIGN; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.size = 0x3FF; cases[count].name = "size too small"; cases[count].expected = PS3_SPURS_TKC_INVAL; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf.ident = 0x7F454C4601010100ull; cases[count].name = "ELFCLASS32 little-endian"; cases[count].expected = PS3_SPURS_TKC_ELF_BAD; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf.machine = 21; cases[count].name = "not EM_SPU"; cases[count].expected = PS3_SPURS_TKC_ELF_BAD; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf.entry = 0x2FFF; cases[count].name = "entry at or below 0x2FFF"; cases[count].expected = PS3_SPURS_TKC_ELF_BAD; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.ls_pattern_ptr = LS_PATTERN_PTR; put_be(r.ls_pattern_block, 0x0, 4, 0xFC000000u); cases[count].name = "lsPattern block high bits"; cases[count].expected = PS3_SPURS_TKC_INVAL; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; for (unsigned i = 0; i < PS3_SPURS_TKC_SET_WORDS; ++i) r.allocated_words[i] = 0xFFFFFFFFu; cases[count].name = "taskset full"; cases[count].expected = PS3_SPURS_TKC_AGAIN; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf.segment_count = 1;
      static const spurs_task_elf_segment small[] = {{PS3_SPURS_TKC_PT_LOAD, 0x1000u}};
      r.elf.segments = small; cases[count].name = "PT_LOAD p_filesz <= 0x2FFF"; cases[count].expected = PS3_SPURS_TKC_ELF_BAD; cases[count].request = r; ++count; }
    { spurs_task_create_request r = base; r.elf.ph_num = 4u; r.elf.segment_count = 1u;
      cases[count].name = "short segment table"; cases[count].expected = PS3_SPURS_TKC_ELF_BAD; cases[count].request = r; ++count; }

    for (size_t index = 0; index < count; ++index) {
        spurs_task_create_plan plan;
        memset(&plan, 0xA5, sizeof plan);
        const spurs_task_create_status status = ps3_spurs_task_create_plan_build(&cases[index].request, &plan);
        ++checks;
        if (status != PS3_SPURS_TKC_STATUS_REFUSED) {
            ++failures;
            fprintf(stderr, "FAIL refusal %s returned status %d\n", cases[index].name, (int)status);
            continue;
        }
        const unsigned char poison = 0xA5u;
        ++checks;
        if (plan.result_code != cases[index].expected) {
            ++failures;
            fprintf(stderr, "FAIL refusal %s -> 0x%08x, expected 0x%08x\n", cases[index].name,
                plan.result_code, cases[index].expected);
        }
        ++checks;
        if (plan.writes[0].bytes[0] != poison) {
            ++failures;
            fprintf(stderr, "FAIL refusal %s wrote into the action list\n", cases[index].name);
        }
    }
}

/* ---- the free-slot search ------------------------------------------------------------- */

void test_second_task_next_slot()
{
    spurs_task_create_request request = game_request();
    request.allocated_words[0] = 0x80000000u;   // slot 0 already allocated
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(&request, &plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(plan.slot, 1u);
    CHECK_EQ(decode_be(plan.writes[6].bytes, plan.writes[6].length), 0xC0000000u); // +0x30 word
    /* The ready bit is the eighth action. */
    CHECK_EQ(decode_be(plan.writes[7].bytes, plan.writes[7].length), 0x40000000u); // +0x20 ready
    const uint32_t record = TASKSET + PS3_SPURS_TKC_RECORD_BASE + 1u * PS3_SPURS_TKC_RECORD_BYTES;
    CHECK_EQ(plan.writes[0].address, record + PS3_SPURS_TKC_RECORD_ELF);
    CHECK_EQ(plan.writes[2].address, record + PS3_SPURS_TKC_RECORD_ARGUMENT);

    spurs_task_create_request deep = game_request();
    for (unsigned i = 0; i < 3u; ++i) deep.allocated_words[i] = 0xFFFFFFFFu;
    deep.allocated_words[3] = 0xE0000000u;      // 96, 97, 98 taken -> the first free bit is 99
    spurs_task_create_plan deep_plan;
    memset(&deep_plan, 0, sizeof deep_plan);
    CHECK(ps3_spurs_task_create_plan_build(&deep, &deep_plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(deep_plan.slot, 99u);
    CHECK_EQ(decode_be(deep_plan.writes[6].bytes, deep_plan.writes[6].length), 0xF0000000u);
    const uint32_t deep_record = TASKSET + PS3_SPURS_TKC_RECORD_BASE + 99u * PS3_SPURS_TKC_RECORD_BYTES;
    CHECK_EQ(deep_plan.writes[0].address, deep_record + PS3_SPURS_TKC_RECORD_ELF);
    CHECK(deep_record + PS3_SPURS_TKC_RECORD_LS_HI + 8u <= TASKSET + 6400u);
    CHECK_EQ(deep_plan.writes[6].address, TASKSET + PS3_SPURS_TKC_SET_ALLOCATED + 3u * 4u);
    CHECK_EQ(deep_plan.writes[7].address, TASKSET + PS3_SPURS_TKC_SET_READY + 3u * 4u);

    spurs_task_create_request top = game_request();
    for (unsigned i = 0; i < 3u; ++i) top.allocated_words[i] = 0xFFFFFFFFu;
    top.allocated_words[3] = 0xFFFFFFFEu;       // bit 0 free == slot 127
    spurs_task_create_plan top_plan;
    memset(&top_plan, 0, sizeof top_plan);
    CHECK(ps3_spurs_task_create_plan_build(&top, &top_plan) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(top_plan.slot, 127u);
    CHECK_EQ(top_plan.writes[7].address, TASKSET + PS3_SPURS_TKC_SET_READY + 3u * 4u);
}

/* ---- the signal request (0xA750 / 0x85D0, CONTRACT §D) -------------------------------- */

void test_signal_build()
{
    spurs_task_signal signal;
    memset(&signal, 0, sizeof signal);
    CHECK(ps3_spurs_task_signal_build(TASKSET, SPURS, WID, &signal) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(signal.taskset, TASKSET);
    CHECK_EQ(signal.spurs, SPURS);
    CHECK_EQ(signal.workload_id, WID);
    CHECK_EQ(signal.ready_count_address, SPURS + 0x70u);
    CHECK_EQ(signal.ready_mask_bit, 0x80000000u);
    CHECK_EQ(signal.signal_address, SPURS + PS3_SPURS_TKC_SIGNAL_BYTE);
    CHECK_EQ(signal.exception_address, SPURS + PS3_SPURS_TKC_EXCEPTION_WORD);

    CHECK(ps3_spurs_task_signal_build(TASKSET, SPURS, 16u, &signal) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(signal.ready_count_address, SPURS + 0x78u);
    CHECK_EQ(signal.ready_mask_bit, 0x8000u);
    CHECK(ps3_spurs_task_signal_build(TASKSET, SPURS, 127u, &signal) == PS3_SPURS_TKC_STATUS_OK);
    CHECK_EQ(signal.ready_mask_bit, 1u);
    CHECK_EQ(signal.ready_count_address, SPURS + 0x78u);
    CHECK(ps3_spurs_task_signal_build(TASKSET, 0u, WID, &signal) == PS3_SPURS_TKC_STATUS_REFUSED);
    CHECK(ps3_spurs_task_signal_build(TASKSET, SPURS + 4u, WID, &signal) == PS3_SPURS_TKC_STATUS_REFUSED);
    CHECK(ps3_spurs_task_signal_build(TASKSET, SPURS, 128u, &signal) == PS3_SPURS_TKC_STATUS_REFUSED);
}

/* ---- the numeric rules the firmware applies ------------------------------------------- */

void test_alignment_budget()
{
    CHECK_EQ(ps3_spurs_task_alignment_budget(0xC00u), 1u);           // (0xC00 - 0x400) >> 11
    CHECK_EQ(ps3_spurs_task_alignment_budget(0x3D400u), 122u);       // the boundary keeps 122
    CHECK_EQ(ps3_spurs_task_alignment_budget(0x3D401u), 122u);       // above it too
    CHECK_EQ(ps3_spurs_task_alignment_budget(0x400u + 0x800u), 1u);  // exactly one unit
    CHECK_EQ(ps3_spurs_task_alignment_budget(0x400u), 0u);
    CHECK_EQ(ps3_spurs_task_alignment_budget(0x1800u), 2u);
}

void test_ls_pattern_count()
{
    unsigned char block[PS3_SPURS_TKC_BLOCK_BYTES];
    memset(block, 0, sizeof block);
    CHECK_EQ(ps3_spurs_task_ls_pattern_count(block), 0u);
    memset(block, 0xFF, sizeof block);
    /* Each word's byte-sum is masked to 8 bits at 0x11D50, so a fully
     * set 16-byte block counts 4 * 32 = 128. */
    CHECK_EQ(ps3_spurs_task_ls_pattern_count(block), 128u);
    memset(block, 0, sizeof block);
    put_be(block, 0x0, 4, 0x80000000u);
    CHECK_EQ(ps3_spurs_task_ls_pattern_count(block), 1u);
    memset(block, 0, sizeof block);
    put_be(block, 0x0, 8, 0x8000000000000001ull);
    CHECK_EQ(ps3_spurs_task_ls_pattern_count(block), 2u);
}

void test_sign_extend()
{
    CHECK_EQ(ps3_spurs_task_sign_extend(0u), 0u);
    CHECK_EQ(ps3_spurs_task_sign_extend(PS3_SPURS_TKC_AGAIN), 0xFFFFFFFF80410901ull);
    CHECK_EQ(ps3_spurs_task_sign_extend(PS3_SPURS_TKC_STAT), 0xFFFFFFFF8041090Full);
}

void test_null_arguments()
{
    spurs_task_create_plan plan;
    memset(&plan, 0, sizeof plan);
    CHECK(ps3_spurs_task_create_plan_build(NULL, &plan) == PS3_SPURS_TKC_STATUS_BAD_ARGUMENT);
    spurs_task_create_request request = game_request();
    CHECK(ps3_spurs_task_create_plan_build(&request, NULL) == PS3_SPURS_TKC_STATUS_BAD_ARGUMENT);
}

int main()
{
    test_game_path_plan();
    test_plan_never_touches_caller_task_id();
    test_refusal_leaves_task_id_alone();
    test_null_pointers_store_zeroes();
    test_ls_pattern_is_the_block();
    test_refusals_are_effect_free();
    test_second_task_next_slot();
    test_signal_build();
    test_alignment_budget();
    test_ls_pattern_count();
    test_sign_extend();
    test_null_arguments();
    printf("spurs_task_create: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
