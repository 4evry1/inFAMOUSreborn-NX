/* Empty and short MFC DMA lists (console run rv41s, 2026-11-13). Real channel engine/owner fixture of test_spu_executor.c.
 *
 * inFAMOUS's SPURS jobs issue `putl` (0x24) with a list size of 0; the executor answered PS3_SPU_EXEC_UNSUPPORTED, the worker ended the thread
 * (state fault, terminal 0x2) and three threads of one SPURS instance died in the same second: the job indices they had started never completed
 * and the game's main thread waited for them for ever. The rules pinned here:
 *   - a list command whose size is 0, or smaller than one 8-byte element, reads nothing, transfers nothing, raises nothing and completes its tag;
 *   - a size that is not a multiple of 8 runs its whole elements and ignores the tail;
 *   - a list whose elements are all empty (size 0, no stall bit) completes the tag without touching memory;
 *   - an empty element WITH the stall-and-notify bit still stalls the list (the WWS sentinel element is exactly 0x80000000), and the stall ack completes it;
 *   - what stays refused: a list longer than 16 KiB, a list address that is not 8-byte aligned, a list that runs off the local store (all with elements).
 * The same channel code serves the interpreter's `wrch MFC_Cmd` and lifted code's ps3_lifted_wrch (one spu_wrch); the interpreter path is also run through
 * the group runner (test_spu_group_runner.c, case_empty_list). */
#define main ps3_existing_spu_executor_regressions
#include "test_spu_executor.c"
#undef main

enum { TAG = 4, LIST_LS = 0x400, DEST_LS = 0x100 };
static int tag_done(ps3_spu_executor *o) { return (o->engine.tag_completed >> TAG) & 1u; }
static void nothing_moved(const Fixture *f, ps3_spu_executor *o) {
    assert(!f->reads && !f->writes && !f->reserves && !f->finishes && !f->traces);
    assert(!o->core.list_stall_mask && !o->terminal.code);
}
/* list_size 0..7 with any command of the family: complete, no memory access, no fault */
static void zero_and_short_lists(void) {
    static const uint32_t commands[] = { 0x24, 0x44, 0x25, 0x45, 0x26, 0x46 };      /* putl getl, with the barrier and fence modifiers */
    for (unsigned c = 0; c < sizeof commands / sizeof commands[0]; ++c)
        for (uint32_t size = 0; size < 8; ++size) {
            Fixture f; ps3_spu_executor *o = create(&f);
            descriptor(o, LIST_LS, 16, 0x1000);              /* an element that would move 16 bytes if the list were run */
            parameters(o, DEST_LS, LIST_LS, size, TAG);
            assert(write_channel(o, MFC_Cmd, commands[c]) == PS3_SPU_EXEC_OK);
            assert(tag_done(o));
            nothing_moved(&f, o);
            /* the tag status says so */
            assert(write_channel(o, MFC_WrTagMask, 1u << TAG) == 0 && write_channel(o, MFC_WrTagUpdate, MFC_TAG_UPDATE_ALL) == 0);
            assert(o->binding.status.tag_available && o->core.mfc_tag_status == (1u << TAG));
            finish(&f, o);
        }
}
/* a tag that was busy is freed by the empty list (the command clears the group's completion bit first, then completes it) */
static void empty_list_after_other_traffic(void) {
    Fixture f; ps3_spu_executor *o = create(&f); seed(&f, 32);
    const unsigned writes_before = f.writes;   /* seed() itself writes the guest memory */
    parameters(o, DEST_LS, 0x1000, 16, TAG); assert(write_channel(o, MFC_Cmd, MFC_GET_CMD) == 0 && f.reads == 1);
    parameters(o, DEST_LS, LIST_LS, 0, TAG);
    assert(write_channel(o, MFC_Cmd, 0x24) == 0 && tag_done(o) && f.reads == 1 && f.writes == writes_before);
    finish(&f, o);
}
/* 12 bytes = one whole element and a 4-byte tail: the element runs, the tail is ignored (its bytes would be a wild element if read) */
static void tail_is_ignored(void) {
    Fixture f; ps3_spu_executor *o = create(&f); seed(&f, 64);
    descriptor(o, LIST_LS, 16, 0x1010); spu_ls_write32(&o->core, LIST_LS + 8, 0x7FFF);   /* tail word: would be a 32 KiB element */
    parameters(o, DEST_LS, LIST_LS, 12, TAG);
    assert(write_channel(o, MFC_Cmd, 0x44) == PS3_SPU_EXEC_OK && tag_done(o) && f.reads == 1 && f.traces == 1);
    for (unsigned i = 0; i < 16; ++i) assert(o->core.ls[DEST_LS + i] == (unsigned char)((0x10 + i) ^ 0xa5));
    finish(&f, o);
}
/* elements that are all empty: nothing moves, the tag completes */
static void all_elements_empty(void) {
    Fixture f; ps3_spu_executor *o = create(&f);
    descriptor(o, LIST_LS, 0, 0x1000); descriptor(o, LIST_LS + 8, 0, 0x1010); descriptor(o, LIST_LS + 16, 0, 0xDEADBEF0);
    parameters(o, DEST_LS, LIST_LS, 24, TAG);
    assert(write_channel(o, MFC_Cmd, 0x44) == PS3_SPU_EXEC_OK && tag_done(o));
    nothing_moved(&f, o);
    finish(&f, o);
}
/* the sentinel element 0x80000000 (size 0, stall-and-notify) stalls; the ack completes the tag without a memory access */
static void empty_element_with_stall(void) {
    Fixture f; ps3_spu_executor *o = create(&f);
    descriptor(o, LIST_LS, 0x80000000u, 0x1000);
    parameters(o, DEST_LS, LIST_LS, 8, TAG);
    assert(write_channel(o, MFC_Cmd, 0x44) == PS3_SPU_EXEC_OK);
    assert(o->core.list_stall_mask == (1u << TAG) && !tag_done(o) && (o->core.event_status & PS3_SPU_EVENT_SN) && !f.reads);
    assert(write_channel(o, MFC_WrListStallAck, TAG) == 0);
    assert(!o->core.list_stall_mask && tag_done(o) && !f.reads && !f.writes);
    finish(&f, o);
}
/* what stays refused */
static void still_refused(void) {
    static const struct { uint32_t list_address, size; const char *why; } cases[] = {
        { LIST_LS, 0x4008, "longer than 16 KiB" }, { LIST_LS + 4, 8, "list address not 8-byte aligned" }, { SPU_LS_SIZE - 8, 16, "runs off the local store" } };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; ++k) {   /* a refusal ends the owner: a fresh one each */
        Fixture f; ps3_spu_executor *o = create(&f);
        descriptor(o, LIST_LS, 16, 0x1000);
        parameters(o, DEST_LS, cases[k].list_address, cases[k].size, TAG);
        if (write_channel(o, MFC_Cmd, 0x44) != PS3_SPU_EXEC_UNSUPPORTED) { fprintf(stderr, "list %s was not refused\n", cases[k].why); assert(0); }
        assert(!f.reads);
        finish(&f, o);
    }
}
/* zero-length PLAIN transfers (already completed before this change): kept as a regression */
static void plain_zero_length(void) {
    for (unsigned c = 0; c < 2; ++c) {
        Fixture f; ps3_spu_executor *o = create(&f);
        parameters(o, DEST_LS, 0x1000, 0, TAG);
        assert(write_channel(o, MFC_Cmd, c ? MFC_PUT_CMD : MFC_GET_CMD) == PS3_SPU_EXEC_OK && tag_done(o));
        nothing_moved(&f, o);
        finish(&f, o);
    }
}
int main(void) {
    zero_and_short_lists();
    empty_list_after_other_traffic();
    tail_is_ignored();
    all_elements_empty();
    empty_element_with_stall();
    still_refused();
    plain_zero_length();
    puts("SPU_LIST_DMA_EMPTY zero/short lists complete their tag, tails ignored, stall sentinel stalls, refusals kept");
    puts("overall=PASS");
    return 0;
}
