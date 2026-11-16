/* The register restore sequence pm_wwsjob writes to the top of the local store at run time (Decision 134), as lifted code.
 *
 * Observed on the console (run rv69s, a pm_wwsjob thread at its 4096th job call): LS [0x3FEC0, 0x3FFF4) holds
 *     a load of register N from the save area at 0x3F9C0 + 16 * N, for N = 0 .. 75,
 *     then a branch through register 79
 * (the words after it repeat the branch). pm_wwsjob jumps there to reload the register file it saved at 0x3F9C0 and continues at r79. It ran on the
 * interpreter: the "other (stubs, top of LS)" row of STARTUP_SPU_IMAGES, 305 M instructions in 600 s (about 4 M runs of 77 instructions), each
 * one an escape from lifted code and back.
 *
 * The bytes are rewritten at run time (the kernel's workload-info DMA lands on 0x3FFE0..0x3FFFF and the policy module writes the sequence
 * again), so this function checks them itself every time it runs: the 77 words must be exactly the sequence above (built here from the
 * pattern; no firmware byte is stored), else the interpreter runs them (an escape, as for any pc without lifted code). It is laid into every
 * image while the SPURS kernel is armed (module kind PS3_LIFTED_KIND_GUARDED, spu_lifted_hybrid.c). */
#include "jobmod_lifted_prelude.h"
#include "wws_restore_stub.h"

enum { STUB_PC = 0x3FEC0u, SAVE_AREA = 0x3F9C0u, RESTORED_REGISTERS = 76u, RETURN_REGISTER = 79u,
       LQA_OPCODE = 0x30800000u, BI_OPCODE = 0x35000000u, STUB_WORDS = RESTORED_REGISTERS + 1u };

static inline uint32_t ls_word(const spu_context *ctx, uint32_t address) {
    const uint8_t *p = ctx->ls + address;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint32_t ps3_wws_restore_stub_word(unsigned index) {
    if (index < RESTORED_REGISTERS) return LQA_OPCODE | (((SAVE_AREA + 16u * index) >> 2) & 0xFFFFu) << 7 | index;
    return BI_OPCODE | RETURN_REGISTER << 7;
}
int ps3_wws_restore_stub_present(const spu_context *ctx) {
    for (unsigned i = 0; i < STUB_WORDS; ++i)
        if (ls_word(ctx, STUB_PC + 4u * i) != ps3_wws_restore_stub_word(i)) return 0;
    return 1;
}

void ps3_wws_restore_stub(spu_context *ctx) {
    PS3_JOBMOD_TRACE_ENTRY(ctx, STUB_PC);
    if (!ps3_wws_restore_stub_present(ctx)) { ctx->pc = STUB_PC; ps3_lifted_escape(ctx); }
    uint8_t *spu_rc_ls = ctx->ls;   /* the LS base the vector LS accessors use (spu_helpers_vec.h), as in generated code */
    for (unsigned n = 0; n < RESTORED_REGISTERS; ++n) ctx->gpr[n] = spu_ls_read128(ctx, SAVE_AREA + 16u * n);
    ctx->pc = ctx->gpr[RETURN_REGISTER]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch; return;   /* branch through register 79, as the generated code says it */
}

static const ps3_spu_lifted_row k_rows[1] = { { STUB_PC, ps3_wws_restore_stub, PS3_LIFTED_ROW_GENUINE } };
const ps3_spu_lifted_module ps3_wws_restore_stub_module = {
    "wws_restore_stub_e3FEC0", 188, STUB_PC, STUB_PC, STUB_PC + 4u * STUB_WORDS,
    0, 0, k_rows, 1, 0, 0, NULL, 0, PS3_LIFTED_KIND_GUARDED, 0 };
