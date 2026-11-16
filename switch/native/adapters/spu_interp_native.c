/* Native SPU interpreter (Decision 33 / C1a).
 *
 * Derived from source/ps3recomp/runtime/spu/spu_interp.c, which is the validated desktop interpreter of this
 * repository. Both files are GPL; this one carries no new provenance.
 *
 * What was taken: the instruction decoder and the per-instruction execute switch, which call the SAME spu_<mnemonic>
 * helpers in spu_helpers.h that the lifter emits, so interpreted and lifted code stay bit-identical.
 *
 * What was dropped, because the desktop file is coupled to the desktop runtime and the lifted pipeline:
 *   - x86 SSE intrinsics (<xmmintrin.h>, _mm_getcsr, __rdtsc): double precision uses SPU__DP_BEGIN/END, the
 *     external floating-point-control form spu_helpers.h already provides;
 *   - the process-wide float-policy switch (g_spu_xfloat / g_spu_xflim, poisoned natively) and getenv;
 *   - the lifted-function registry, plugin rejoin, taskset syscall, spell/policy hooks, call-trace and step dumps;
 *   - the decode cache and every other piece of cross-run mutable state. Decode tables are const and the only
 *     writer-visible state a run touches is the owner's spu_context.
 *
 * Channels go through spu_rdch/spu_wrch/spu_rchcnt (spu_channels.c under PS3_NATIVE_SPU_EXECUTOR). A channel the
 * interpreter cannot satisfy is either a PAUSE -- the owner is left at the channel instruction so the caller can
 * retry it after the PPU acts -- or a FAULT through the executor's permanent fail path.
 */
#include <stdlib.h>
#include "spu_executor.h"
#include "spu_helpers.h"
#include <string.h>
#include "../../../ps3recomp/runtime/spu/spu_interp_tables.inc"

enum { SPU_INSN_BYTES = 4, SPU_STOP_CODE_MASK = 0x3FFF, SPU_LS_ADDRESS_MASK = 0x3FFFC,
       SPU_HALT_STOP_CODE = 0x1000, SPU_LINK_MASK = 0x3FFFC };

/* ---- decode: 32-bit insn -> fields (mirrors spu_disasm.spu_decode order) ---- */
typedef struct {
    spu_op op;
    uint8_t rt, ra, rb, rc, ch;
    int32_t imm;    /* sign/zero-extended immediate or shift, per format */
    uint32_t tgt;   /* absolute LS branch target (br-family) */
} spu_ins;

static int32_t sx(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static void spu_decode1(uint32_t insn, uint32_t pc, spu_ins *d) {
    uint32_t op4 = (insn >> 28) & 0xF, op7 = (insn >> 25) & 0x7F;
    uint32_t op8 = (insn >> 24) & 0xFF, op9 = (insn >> 23) & 0x1FF;
    uint32_t op10 = (insn >> 22) & 0x3FF, op11 = (insn >> 21) & 0x7FF;
    d->rt = insn & 0x7F; d->ra = (insn >> 7) & 0x7F;
    d->rb = (insn >> 14) & 0x7F; d->rc = (insn >> 21) & 0x7F;
    d->ch = (insn >> 7) & 0x7F; d->imm = 0; d->tgt = 0;
    const int32_t i7 = sx(d->rb, 7);
    const int32_t i10 = sx((insn >> 14) & 0x3FF, 10);
    const int32_t i16 = sx((insn >> 7) & 0xFFFF, 16);
    const uint32_t i18 = (insn >> 7) & 0x3FFFF;

    /* RRR */
    if ((d->op = spu_dec_rrr(op4)) != SPU_word) return;
    /* RI18 */
    if ((d->op = spu_dec_ri18(op7)) != SPU_word) {
        d->imm = (d->op == SPU_ila) ? (int32_t)i18 : (int32_t)(i18 & 0xFFFF);
        return;
    }
    /* RI16 -- before RI10 (shared top-8-bit opcodes) */
    if ((d->op = spu_dec_ri16(op9)) != SPU_word) {
        switch (d->op) {
        case SPU_lqa: case SPU_stqa: d->tgt = ((uint32_t)(i16 & 0xFFFF) << 2) & 0x3FFF0; break;
        case SPU_lqr: case SPU_stqr: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFF0; break;
        case SPU_br: case SPU_brsl: case SPU_brz: case SPU_brnz:
        case SPU_brhz: case SPU_brhnz: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFFF; break;
        case SPU_bra: case SPU_brasl: d->tgt = ((uint32_t)(i16 * 4)) & 0x3FFFF; break;
        case SPU_il: d->imm = i16; break;
        default: d->imm = (int32_t)(i16 & 0xFFFF); break;   /* ilh/ilhu/iohl */
        }
        return;
    }
    /* channel ops -- before RI10 (wrch/shli op8 clash) */
    if (op11 == 0x00D) { d->op = SPU_rdch;   return; }
    if (op11 == 0x10D) { d->op = SPU_wrch;   return; }
    if (op11 == 0x00F) { d->op = SPU_rchcnt; return; }
    /* RI10 */
    if ((d->op = spu_dec_ri10(op8)) != SPU_word) {
        /* lqd/stqd carry a quadword-scaled immediate. The shift is done on the unsigned bits: a signed negative
         * shift is undefined behaviour, and the LSA is a 32-bit address anyway. */
        d->imm = (d->op == SPU_lqd || d->op == SPU_stqd) ? (int32_t)((uint32_t)i10 << 4) : i10;
        return;
    }
    /* RI8 float<->int conversions */
    if ((d->op = spu_dec_ri8(op10)) != SPU_word) { d->imm = (insn >> 14) & 0xFF; return; }
    /* RR */
    if ((d->op = spu_dec_rr(op11)) != SPU_word) {
        if (d->op == SPU_stop) d->imm = insn & SPU_STOP_CODE_MASK;
        return;
    }
    /* RI7 rotate/shift/gen-control immediates */
    if ((d->op = spu_dec_ri7(op11)) != SPU_word) {
        d->imm = spu_ri7_unsigned(d->op) ? (int32_t)((uint32_t)i7 & 0x7Fu) : i7;
        return;
    }
    /* LSX (redundant with RR's lqx/stqx, kept for order-parity) */
    if ((d->op = spu_dec_lsx(op11)) != SPU_word) return;
    d->op = SPU_word;
}

/* ---- one interpreter run ---- */

/* `spu_rdch`/`spu_wrch`/`spu_rchcnt` leave ctx->pc at the channel instruction when they cannot satisfy it (a PAUSE),
 * and a FAULT leaves through the executor's escape scope without touching pc. Nothing else moves pc between the
 * read of ctx->pc and the store of `next`, so a terminal outcome needs no pc repair. */
enum spu_step_outcome { SPU_STEP_CONTINUE, SPU_STEP_STOP, SPU_STEP_HALT };

#define A   (ctx->gpr[d.ra])
#define B   (ctx->gpr[d.rb])
#define T   (ctx->gpr[d.rt])          /* also the 3rd source in RRR */
#define DST (ctx->gpr[d.rt])          /* default destination */
#define DSTC (ctx->gpr[d.rc])         /* RRR destination */
#define I   (d.imm)
#define PREF(r) ((r)._u32[0])

/* Report a channel operation to the owner's optional observation hook (NULL in composition). */
static void spu_observe_channel(spu_context *ctx, unsigned kind, uint32_t channel) {
    ps3_spu_executor *o = (ps3_spu_executor *)ctx;
    if (o->observe_channel) o->observe_channel(o->observe_context, kind, channel);
}

/* Execute one already-decoded instruction. STOP leaves pc after the instruction (hardware stop); HALT keeps pc at
 * the halting instruction and is a terminal fault, never a resumable stop. */
static enum spu_step_outcome spu_step_impl(spu_context *ctx, const uint32_t pc, const spu_ins d) {
    uint32_t next = pc + SPU_INSN_BYTES;

    switch (d.op) {
    /* immediates / loads */
    case SPU_il:   DST = spu_splat_u32((uint32_t)(int32_t)I); break;
    case SPU_ilh:  DST = spu_ilh((uint16_t)I); break;
    case SPU_ilhu: DST = spu_splat_u32((uint32_t)I << 16); break;
    case SPU_ila:  DST = spu_splat_u32((uint32_t)I & 0x3FFFF); break;
    case SPU_iohl: DST = spu_ori(DST, (int32_t)((uint32_t)I & 0xFFFF)); break;
    case SPU_fsmbi: DST = spu_fsmbi(I); break;
    case SPU_lqd: case SPU_lqx: case SPU_lqa: case SPU_lqr: {
        uint32_t a = (d.op==SPU_lqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_lqx) ? PREF(A)+PREF(B) : d.tgt;
        DST = spu_ls_read128(ctx, a); break; }
    case SPU_stqd: case SPU_stqx: case SPU_stqa: case SPU_stqr: {
        uint32_t a = (d.op==SPU_stqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_stqx) ? PREF(A)+PREF(B) : d.tgt;
        spu_ls_write128(ctx, a, DST); break; }
    /* integer arithmetic */
    case SPU_a:  DST = spu_a(A,B); break;
    case SPU_ai: DST = spu_ai(A,I); break;
    case SPU_sf: DST = spu_sf(A,B); break;
    case SPU_sfi:DST = spu_sfi(A,I); break;
    case SPU_ah: DST = spu_ah(A,B); break;
    case SPU_ahi:DST = spu_ahi(A,I); break;
    case SPU_sfh:DST = spu_sfh(A,B); break;
    case SPU_sfhi:DST = spu_sfhi(A,I); break;
    case SPU_addx: DST = spu_addx(A,B,DST); break;
    case SPU_sfx:  DST = spu_sfx(A,B,DST); break;
    case SPU_cg:   DST = spu_cg(A,B); break;
    case SPU_cgx:  DST = spu_cgx(A,B,DST); break;
    case SPU_mpyi: DST = spu_mpyi(A,I); break;
    /* logical */
    case SPU_and: DST = spu_and(A,B); break;
    case SPU_or:  DST = spu_or(A,B); break;
    case SPU_xor: DST = spu_xor(A,B); break;
    case SPU_nand:DST = spu_nand(A,B); break;
    case SPU_nor: DST = spu_nor(A,B); break;
    case SPU_andc:DST = spu_andc(A,B); break;
    case SPU_orc: DST = spu_orc(A,B); break;
    case SPU_eqv: DST = spu_eqv(A,B); break;
    case SPU_andi:DST = spu_andi(A,I); break;
    case SPU_ori: DST = spu_ori(A,I); break;
    case SPU_xori:DST = spu_xori(A,I); break;
    case SPU_andbi:DST = spu_andbi(A,I); break;
    case SPU_andhi:DST = spu_andhi(A,I); break;
    case SPU_orhi: DST = spu_orhi(A,I); break;
    case SPU_orbi: DST = spu_orbi(A,I); break;
    case SPU_xorhi:DST = spu_xorhi(A,I); break;
    case SPU_xorbi:DST = spu_xorbi(A,I); break;
    case SPU_orx:  DST = spu_orx(A); break;
    /* compares */
    case SPU_ceq: DST = spu_ceq(A,B); break;
    case SPU_ceqh:DST = spu_ceqh(A,B); break;
    case SPU_ceqb:DST = spu_ceqb(A,B); break;
    case SPU_ceqi:DST = spu_ceqi(A,I); break;
    case SPU_cgt: DST = spu_cgt(A,B); break;
    case SPU_cgth:DST = spu_cgth(A,B); break;
    case SPU_cgtb:DST = spu_cgtb(A,B); break;
    case SPU_cgti:DST = spu_cgti(A,I); break;
    case SPU_clgt: DST = spu_clgt(A,B); break;
    case SPU_clgth:DST = spu_clgth(A,B); break;
    case SPU_clgtb:DST = spu_clgtb(A,B); break;
    case SPU_clgti:DST = spu_clgti(A,I); break;
    /* shifts / rotates */
    case SPU_shl:  DST = spu_shl(A,B); break;
    case SPU_shli: DST = spu_shli(A,I); break;
    case SPU_shlh: DST = spu_shlh(A,B); break;
    case SPU_shlhi:DST = spu_shlhi(A,I); break;
    case SPU_rot:  DST = spu_rot(A,B); break;
    case SPU_roti: DST = spu_roti(A,I); break;
    case SPU_rotm: DST = spu_rotm(A,B); break;
    case SPU_rotmi:DST = spu_rotmi(A,I); break;
    case SPU_rotma:DST = spu_rotma(A,B); break;
    case SPU_rotmai:DST = spu_rotmai(A,I); break;
    case SPU_roth: DST = spu_roth(A,B); break;
    case SPU_rothi:DST = spu_rothi(A,I); break;
    case SPU_shlqbi: DST = spu_shlqbi(A,B); break;
    case SPU_shlqbii:DST = spu_shlqbii(A,I); break;
    case SPU_shlqby: DST = spu_shlqby(A,B); break;
    case SPU_shlqbyi:DST = spu_shlqbyi(A,I); break;
    case SPU_rotqbi: DST = spu_rotqbi(A,B); break;
    case SPU_rotqbii:DST = spu_rotqbii(A,I); break;
    case SPU_rotqby: DST = spu_rotqby(A,B); break;
    case SPU_rotqbyi:DST = spu_rotqbyi(A,I); break;
    case SPU_rotqmby: DST = spu_rotqmby(A,B); break;
    case SPU_rotqmbyi:DST = spu_rotqmbyi(A,I); break;
    /* select / shuffle / masks / bitcount (RRR: dest=rc, srcs=ra,rb,rt) */
    case SPU_selb:  DSTC = spu_selb(A,B,T); break;
    case SPU_shufb: DSTC = spu_shufb(A,B,T); break;
    case SPU_fsm:  DST = spu_fsm(A); break;
    case SPU_fsmb: DST = spu_fsmb(A); break;
    case SPU_fsmh: DST = spu_fsmh(A); break;
    case SPU_gb:   DST = spu_gb(A); break;
    case SPU_gbb:  DST = spu_gbb(A); break;
    case SPU_gbh:  DST = spu_gbh(A); break;
    case SPU_clz:  DST = spu_clz(A); break;
    case SPU_cntb: DST = spu_cntb(A); break;
    /* gen-controls for insertion */
    case SPU_cbd: DST = spu_cbd(A,I); break;
    case SPU_chd: DST = spu_chd(A,I); break;
    case SPU_cwd: DST = spu_cwd(A,I); break;
    case SPU_cdd: DST = spu_cdd(A,I); break;
    case SPU_cbx: DST = spu_cbx(A,B); break;
    case SPU_chx: DST = spu_chx(A,B); break;
    case SPU_cwx: DST = spu_cwx(A,B); break;
    case SPU_cdx: DST = spu_cdx(A,B); break;
    /* multiply */
    case SPU_mpy:  DST = spu_mpy(A,B); break;
    case SPU_mpyu: DST = spu_mpyu(A,B); break;
    case SPU_mpyui:DST = spu_mpyui(A,I); break;
    case SPU_mpyh: DST = spu_mpyh(A,B); break;
    case SPU_mpya: DSTC = spu_mpya(A,B,T); break;
    /* sign-extend */
    case SPU_xsbh: DST = spu_xsbh(A); break;
    case SPU_xshw: DST = spu_xshw(A); break;
    case SPU_xswd: DST = spu_xswd(A); break;
    /* float */
    case SPU_fa: DST = spu_fa(A,B); break;
    case SPU_fs: DST = spu_fs(A,B); break;
    case SPU_fm: DST = spu_fm(A,B); break;
    case SPU_fcgt: DST = spu_fcgt(A,B); break;
    case SPU_fceq: DST = spu_fceq(A,B); break;
    case SPU_cflts: DST = spu_cflts(A,I); break;
    case SPU_cfltu: DST = spu_cfltu(A,I); break;
    case SPU_csflt: DST = spu_csflt(A,I); break;
    case SPU_cuflt: DST = spu_cuflt(A,I); break;
    case SPU_frest:  DST = spu_frest(A); break;
    case SPU_frsqest:DST = spu_frsqest(A); break;
    case SPU_fi:     DST = spu_fi(A,B); break;
    case SPU_fcmgt:  DST = spu_fcmgt(A,B); break;
    case SPU_fcmeq:  DST = spu_fcmeq(A,B); break;
    /* Double precision rounds to nearest and keeps denormals; the single-precision run uses truncate + FTZ.
     * SPU__DP_BEGIN/END switch for the op exactly as the lifted helpers do. */
#define SPU_DP(expr) do { SPU__DP_BEGIN DST = (expr); SPU__DP_END } while (0)
    case SPU_dfa:   SPU_DP(spu_dfa(A,B)); break;
    case SPU_dfs:   SPU_DP(spu_dfs(A,B)); break;
    case SPU_dfm:   SPU_DP(spu_dfm(A,B)); break;
    case SPU_dfma:  SPU_DP(spu_dfma(A,B,DST)); break;
    case SPU_dfms:  SPU_DP(spu_dfms(A,B,DST)); break;
    case SPU_dfnms: SPU_DP(spu_dfnms(A,B,DST)); break;
    case SPU_dfnma: SPU_DP(spu_dfnma(A,B,DST)); break;
    case SPU_dfceq: DST = spu_dfceq(A,B); break;
    case SPU_dfcmeq:DST = spu_dfcmeq(A,B); break;
    case SPU_dfcgt: DST = spu_dfcgt(A,B); break;
    case SPU_dfcmgt:DST = spu_dfcmgt(A,B); break;
    case SPU_dftsv: DST = spu_dftsv(A,(int32_t)I); break;
    case SPU_bisled: { uint32_t t = PREF(A) & SPU_LS_ADDRESS_MASK; DST = spu_link(pc + SPU_INSN_BYTES);
                       if ((ctx->event_status & ctx->event_mask) != 0) next = t;
                       break; }
    case SPU_fesd:   DST = spu_fesd(A); break;
    case SPU_frds:   DST = spu_frds(A); break;
    case SPU_fma:  DSTC = spu_fma(A,B,T); break;
    case SPU_fms:  DSTC = spu_fms(A,B,T); break;
    case SPU_fnms: DSTC = spu_fnms(A,B,T); break;
    /* immediate byte/halfword compares */
    case SPU_ceqbi: DST = spu_ceqbi(A,I); break;
    case SPU_cgtbi: DST = spu_cgtbi(A,I); break;
    case SPU_clgtbi:DST = spu_clgtbi(A,I); break;
    case SPU_ceqhi: DST = spu_ceqhi(A,I); break;
    case SPU_cgthi: DST = spu_cgthi(A,I); break;
    case SPU_clgthi:DST = spu_clgthi(A,I); break;
    /* integer / byte extras */
    case SPU_bg:    DST = spu_bg(A,B); break;
    case SPU_absdb: DST = spu_absdb(A,B); break;
    case SPU_avgb:  DST = spu_avgb(A,B); break;
    case SPU_sumb:  DST = spu_sumb(A,B); break;
    case SPU_mpys:  DST = spu_mpys(A,B); break;
    case SPU_mpyhh: DST = spu_mpyhh(A,B); break;
    case SPU_mpyhhu:DST = spu_mpyhhu(A,B); break;
    /* mpyhha / mpyhhau are RR-form: rt is BOTH the accumulator and the destination. */
    case SPU_mpyhha: DST = spu_mpyhha(A,B,DST); break;
    case SPU_mpyhhau:DST = spu_mpyhhau(A,B,DST); break;
    case SPU_bgx:    DST = spu_bgx(A,B,DST); break;
    case SPU_rothmi: DST = spu_rothmi(A,I); break;
    case SPU_rotmahi:DST = spu_rotmahi(A,I); break;
    case SPU_rotmah:
        for (int i = 0; i < 8; i++) { uint32_t sh = (0u - B._u16[i]) & 0x1F;
            DST._s16[i] = (sh > 15) ? (int16_t)(A._s16[i] >> 15) : (int16_t)(A._s16[i] >> sh); }
        break;
    /* rotate / shift extras */
    case SPU_rothm:    DST = spu_rothm(A,B); break;
    case SPU_rotqmbi:  DST = spu_rotqmbi(A,B); break;
    case SPU_rotqmbii: DST = spu_rotqmbii(A,(int)I); break;
    case SPU_rotqbybi: DST = spu_rotqbybi(A,B); break;
    case SPU_rotqmbybi:DST = spu_rotqmbybi(A,B); break;
    case SPU_shlqbybi: DST = spu_shlqbybi(A,B); break;
    /* channels. A PAUSE or FAULT inside spu_rdch/spu_wrch/spu_rchcnt leaves ctx->pc at the channel instruction (a
     * channel TU never moves it), so a PAUSE is retried by re-running from that pc. */
    case SPU_wrch:   spu_observe_channel(ctx, PS3_SPU_OBS_WRITE_CHANNEL, d.ch);
                     spu_wrch(ctx, d.ch, DST); break;
    case SPU_rdch:   spu_observe_channel(ctx, PS3_SPU_OBS_READ_CHANNEL, d.ch);
                     DST = spu_rdch(ctx, d.ch); break;
    case SPU_rchcnt: spu_observe_channel(ctx, PS3_SPU_OBS_READ_CHANNEL_COUNT, d.ch);
                     DST = spu_splat_u32(spu_rchcnt(ctx, d.ch)); break;
    /* hints / no-ops */
    case SPU_nop: case SPU_lnop: case SPU_sync: case SPU_dsync:
    case SPU_hbr: case SPU_hbra: case SPU_hbrr: case SPU_mtspr:
    case SPU_mfspr: case SPU_fscrwr: break;
    /* Decision 104 (2026-11-13): fscrrd WRITES rt. It used to be a no-op that left a stale register behind; the lifted job code (and the
     * spu_fscrrd helper, FPSCR with no exception flag set = 0) wrote zero, and the differential test of the job modules caught the
     * difference (jobmod_542edebc6499 0x1A1E0: the exception-flag helper stored whatever r19 held). */
    case SPU_fscrrd: DST = spu_fscrrd(A); break;
    /* control flow */
    case SPU_br: case SPU_bra: next = d.tgt; break;
    case SPU_brsl: case SPU_brasl: DST = spu_link(pc + SPU_INSN_BYTES); next = d.tgt; break;
    case SPU_brz:  if (PREF(DST) == 0) next = d.tgt; break;
    case SPU_brnz: if (PREF(DST) != 0) next = d.tgt; break;
    case SPU_brhz: if ((PREF(DST) & 0xFFFF) == 0) next = d.tgt; break;
    case SPU_brhnz:if ((PREF(DST) & 0xFFFF) != 0) next = d.tgt; break;
    case SPU_bi:   next = PREF(A) & SPU_LS_ADDRESS_MASK; break;
    /* Read the target BEFORE writing the link: `bisl $0,$0` has rt == ra. */
    case SPU_bisl: next = PREF(A) & SPU_LS_ADDRESS_MASK; DST = spu_link(pc + SPU_INSN_BYTES); break;
    case SPU_iret: next = ctx->srr0 & SPU_LS_ADDRESS_MASK; break;
    case SPU_biz:  if (PREF(DST) == 0) next = PREF(A) & SPU_LS_ADDRESS_MASK; break;
    case SPU_binz: if (PREF(DST) != 0) next = PREF(A) & SPU_LS_ADDRESS_MASK; break;
    case SPU_bihz: if ((PREF(DST) & 0xFFFF) == 0) next = PREF(A) & SPU_LS_ADDRESS_MASK; break;
    case SPU_bihnz:if ((PREF(DST) & 0xFFFF) != 0) next = PREF(A) & SPU_LS_ADDRESS_MASK; break;
    /* `stop` (and its alias `stopd` in the decoder) is a resumable architectural stop: the code is reported and
     * NOT interpreted here -- the caller names what it means. */
    case SPU_stop: case SPU_stopd:
        ctx->pc = next; ctx->stop_code = (uint32_t)I;
        ctx->status = SPU_STATUS_STOPPED_BY_STOP; return SPU_STEP_STOP;
    /* Conditional halts are assertions: pc stays at the halting instruction. */
    case SPU_heq: case SPU_heqi: case SPU_hgt: case SPU_hgti:
    case SPU_hlgt: case SPU_hlgti: {
        uint32_t a = PREF(A), rhs = (d.op==SPU_heq||d.op==SPU_hgt||d.op==SPU_hlgt) ? PREF(B) : (uint32_t)I;
        int hit = (d.op==SPU_heq||d.op==SPU_heqi)   ? (a == rhs)
                : (d.op==SPU_hgt||d.op==SPU_hgti)   ? ((int32_t)a > (int32_t)rhs)
                :                                     (a > rhs);      /* hlgt/hlgti */
        if (hit) { ctx->pc = pc; ctx->stop_code = SPU_HALT_STOP_CODE;
                   ctx->status = SPU_STATUS_STOPPED_BY_HALT; return SPU_STEP_HALT; }
        break; }
    default:
        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return SPU_STEP_HALT;
    }
    ctx->pc = next;
    return SPU_STEP_CONTINUE;
}

/* A halt is terminal: it leaves through the executor's permanent fail path, which records PS3_SPU_EXEC_UNSUPPORTED in
 * the owner's terminal (kind FAULT). Two things are reported separately, because they name different facts:
 *   stop_code -- the architectural halt value. A decoded heq/hgt/hlgt step already set it to SPU_HALT_STOP_CODE
 *     (0x1000, the assertion HALT status); an undecodable word leaves the zero a fresh run starts with.
 *   *opcode   -- the raw instruction word that halted, so an undecodable opcode is named by its own number.
 * The caller copies *opcode into ps3_spu_interp_result.opcode after the escape returns. */
static _Noreturn void spu_halt(spu_context *ctx, uint32_t opcode_word, uint32_t *out_opcode) {
    *out_opcode=opcode_word;
    ps3_spu_execution_fail(ctx, PS3_SPU_EXEC_UNSUPPORTED);
}

/* Root (2026-11-12, Decision 73): predecode cache. spu_decode1 walks up to nine decode tables for every instruction
 * executed; the movie decoder ran about 10 million instructions per second under Eden with it. One table per SPU
 * context holds the decoded form of each local-store word next to the raw word it was decoded from. An entry is
 * used only when its raw word equals the word in the local store NOW, so code the program (or a DMA) rewrites is
 * decoded again: the cache can never run stale code. Contexts are found by address in a small fixed list; a context
 * that gets no table (list full, no memory) runs exactly as before. Tables live until the process ends. */
typedef struct { uint32_t raw; uint32_t valid; spu_ins ins; } spu_predecode_entry;
enum { SPU_PREDECODE_CONTEXTS = 16, SPU_PREDECODE_WORDS = 0x40000 / 4 };
static struct { spu_context *owner; spu_predecode_entry *table; } g_spu_predecode[SPU_PREDECODE_CONTEXTS];
static spu_predecode_entry *spu_predecode_table(spu_context *ctx) {
    for (unsigned index = 0; index < SPU_PREDECODE_CONTEXTS; ++index) {
        spu_context *owner = __atomic_load_n(&g_spu_predecode[index].owner, __ATOMIC_ACQUIRE);
        if (owner == ctx) return __atomic_load_n(&g_spu_predecode[index].table, __ATOMIC_ACQUIRE);
        if (owner == NULL) {
            spu_context *expected = NULL;
            if (!__atomic_compare_exchange_n(&g_spu_predecode[index].owner, &expected, ctx, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (expected == ctx) return __atomic_load_n(&g_spu_predecode[index].table, __ATOMIC_ACQUIRE);
                continue;
            }
            /* Only the thread that runs this context reaches here for it, so the table is published before any use. */
            spu_predecode_entry *table = calloc(SPU_PREDECODE_WORDS, sizeof *table);
            __atomic_store_n(&g_spu_predecode[index].table, table, __ATOMIC_RELEASE);
            return table;
        }
    }
    return NULL;
}

/* Decision 104 (2026-11-13): with owner->region_map set, a TAKEN transfer (never a sequential instruction) that changes the
 * local-store region (kernel / policy module / job window / other) attributes the instructions executed since the last change
 * to the region it leaves (owner->acct_instr[slot]: a few adds per region change, nothing per instruction). Two transfers also
 * end the run (kind BUDGET, owner->watch_hit says why) so the hybrid glue can act on them:
 *   PS3_SPU_WATCH_JOB_CALL      a link-setting branch (bisl/brsl/brasl/bisled) from outside the job window into it: a job starts;
 *   PS3_SPU_WATCH_LIFTED_ENTRY  any taken transfer to a genuine lifted entry of the armed job module (owner->watch_entry_bits).
 * Bits and the map are owned by the glue; nothing here decides what a module is. */
static inline int spu_is_link_branch(spu_op op) {
    return op==SPU_bisl || op==SPU_brasl || op==SPU_brsl || op==SPU_bisled;
}
void ps3_spu_interp_run(spu_context *ctx, uint64_t budget_instructions, ps3_spu_interp_result *out) {
    out->kind=PS3_SPU_INTERP_BUDGET; out->code=PS3_SPU_EXEC_OK; out->channel=0; out->stop_code=0; out->opcode=0;
    out->instructions=0;
    ctx->status=SPU_STATUS_RUNNING;
    ps3_spu_executor *const owner=(ps3_spu_executor *)ctx;   /* closed first-subobject contract, as spu_observe_channel */
    const uint8_t *const region_map=owner->region_map;
    owner->watch_hit=PS3_SPU_WATCH_NONE;
    owner->acct_mark=0;
    if (region_map) owner->acct_region=region_map[(ctx->pc & SPU_LS_ADDRESS_MASK)>>8];
    spu_predecode_entry *const predecode=spu_predecode_table(ctx);
    while (out->instructions<budget_instructions) {
        /* Root (2026-11-11): SPU interrupt entry (CBE; RPCS3 SPUThread check_mfc_interrupts). With interrupts
         * enabled and an event pending that the mask selects, the next instruction is not run: SRR0 takes its
         * address, interrupts are disabled and execution continues at local-store address 0, where the program
         * planted its handler. The handler returns with iret (and usually re-enables with its E bit). Checked
         * between instructions, so a blocked channel instruction is interrupted at its own address and retried
         * after the handler, as on hardware. */
        if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
            ctx->srr0=ctx->pc & SPU_LS_ADDRESS_MASK;
            ctx->int_enable=0;
            ctx->pc=0;
            if (region_map) {   /* the vector is a transfer too: attribute what ran so far to the region it came from */
                owner->acct_instr[owner->acct_slot[owner->acct_region]]+=out->instructions-owner->acct_mark;
                owner->acct_mark=out->instructions; owner->acct_region=region_map[0];
            }
        }
        const uint32_t pc=ctx->pc & SPU_LS_ADDRESS_MASK;
        const uint32_t insn=spu_ls_read32(ctx,pc);
        spu_ins d;
        if (predecode) {
            spu_predecode_entry *const entry=&predecode[pc>>2];
            if (!entry->valid || entry->raw!=insn) { spu_decode1(insn,pc,&entry->ins); entry->raw=insn; entry->valid=1; }
            d=entry->ins;
        } else spu_decode1(insn,pc,&d);
        ++out->instructions;
        switch (spu_step_impl(ctx,pc,d)) {
        case SPU_STEP_CONTINUE:
            /* The E (bit 18) and D (bit 19) bits of the indirect-branch family set and clear the interrupt
             * enable. bi, bisl and iret always apply them; the conditional forms and bisled only when the
             * branch is taken. */
            if (insn & 0x000C0000u) {
                int applies=0;
                switch (d.op) {
                case SPU_bi: case SPU_bisl: case SPU_iret: applies=1; break;
                case SPU_biz: case SPU_binz: case SPU_bihz: case SPU_bihnz: case SPU_bisled:
                    applies=(ctx->pc & SPU_LS_ADDRESS_MASK)!=((pc+SPU_INSN_BYTES) & SPU_LS_ADDRESS_MASK); break;
                default: break;
                }
                if (applies) {
                    if (insn & 0x00040000u) ctx->int_enable=1;
                    else if (insn & 0x00080000u) ctx->int_enable=0;
                }
            }
            if (region_map) {
                const uint32_t npc=ctx->pc & SPU_LS_ADDRESS_MASK;
                if (npc!=pc+SPU_INSN_BYTES) {                                  /* a taken transfer */
                    const unsigned region=region_map[npc>>8];
                    if (region!=owner->acct_region) {                          /* ... that leaves its region */
                        owner->acct_instr[owner->acct_slot[owner->acct_region]]+=out->instructions-owner->acct_mark;
                        owner->acct_mark=out->instructions;
                        const unsigned from=owner->acct_region;
                        owner->acct_region=(uint8_t)region;
                        if (region==PS3_SPU_REGION_JOB && from!=PS3_SPU_REGION_JOB && spu_is_link_branch(d.op)) {
                            owner->watch_hit=PS3_SPU_WATCH_JOB_CALL; return;
                        }
                    }
                    const uint8_t *const bits=owner->watch_entry_bits;
                    if (bits && ((bits[npc>>5]>>((npc>>2)&7u))&1u)) { owner->watch_hit=PS3_SPU_WATCH_LIFTED_ENTRY; return; }
                }
            }
            break;
        case SPU_STEP_STOP: out->kind=PS3_SPU_INTERP_STOPPED; out->code=ctx->stop_code; return;
        case SPU_STEP_HALT: spu_halt(ctx,insn,&out->opcode);
        }
    }
}
