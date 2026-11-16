/* Decision 33 / C1a: the native SPU interpreter.
 *
 * usage: test_spu_interp <libsre image.bin> <elf offset> <expected entry>
 *        (evidence/lle-sre-a1/lift_libsre.sh produces <build>/prx/libsre_image.bin)
 *
 *   1. hand-assembled SPU programs (encodings built below, expected values derived by hand in the comments), run
 *      through ps3_spu_executor_run_interp;
 *   2. a differential oracle: every one of those programs also runs on the desktop interpreter
 *      (source/ps3recomp/runtime/spu/spu_interp.c, built here with vm_base pointed at a host buffer, SPU_INTERP_NO_VEC
 *      and SPU_XFLOAT=0 for identical float semantics) -- all 128 registers, the whole local store and pc must match;
 *   3. the channels the interpreter added, including a real lock-line reservation loss;
 *   4. the real libsre SPURS kernel (SPU ELF at image offset 0x20380, entry 0x818) from a fresh local store until its
 *      first named stop, with the first operations it performs on channels/DMA logged.
 */
/* Poisons g_spu_xfloat/g_spu_xflim (and refuses SPU_XFLOAT_RUNTIME) for native TUs. */
#include "../adapters/spu_linked_image.h"
#include "../adapters/spu_executor.h"
#include "../adapters/guard_pthread.h"
#include "guest_space_host.h"
#include "../domain/lv2_spu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The desktop interpreter's entry point (spu_interp.h). The header itself is not included: it also declares the
 * float-policy globals, which the poison above refuses. spu_interp_oracle_host.c defines everything the desktop
 * interpreter needs that the host test does not. */
struct spu_context;
uint32_t spu_interp_run(struct spu_context *ctx, uint32_t start_lsa);

/* ------------------------------------------------------------------------------------------ SPU instruction encoding */
/* BE32 instruction words, formats per the SPU ISA: RI10, RI16, RR, RI7 and RRR. */
/* The four immediate formats, field positions verified against the validated decoder
 * (source/ps3recomp/tools/spu_disasm.py: ra=(insn>>7)&0x7F, i10=(insn>>14)&0x3FF, i16=(insn>>7)&0xFFFF).
 *   RI10: OP(8) IMM(10) RA(7) RT(7)      RI16: OP(9) IMM(16) RT(7)
 *   RI18: OP(7) IMM(18) RT(7)            RI7:  OP(11) IMM(7) RA(7) RT(7)   */
static uint32_t enc_ri10(uint32_t op8, uint32_t rt, uint32_t ra, uint32_t imm10) {
    return (op8 << 24) | ((imm10 & 0x3FFu) << 14) | ((ra & 0x7Fu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_ri16(uint32_t op9, uint32_t rt, uint32_t imm16) {
    return (op9 << 23) | ((imm16 & 0xFFFFu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_rr(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t rb) {
    return (op11 << 21) | ((rb & 0x7Fu) << 14) | ((ra & 0x7Fu) << 7) | (rt & 0x7Fu);
}
/* RI7: OP(11) IMM(7) RA(7) RT(7) -- the immediate occupies the rb field. */
static uint32_t enc_ri7(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t imm7) {
    return (op11 << 21) | ((imm7 & 0x7Fu) << 14) | ((ra & 0x7Fu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_rrr(uint32_t op4, uint32_t rc, uint32_t ra, uint32_t rb, uint32_t rthree) {
    return (op4 << 28) | ((rc & 0x7Fu) << 21) | ((rb & 0x7Fu) << 14) | ((ra & 0x7Fu) << 7) | (rthree & 0x7Fu);
}
static uint32_t enc_stop(uint32_t code14) { return code14 & 0x3FFFu; }
static uint32_t enc_il(uint32_t rt, int32_t imm16) { return enc_ri16(0x81, rt, (uint32_t)imm16); }
static uint32_t enc_ilhu(uint32_t rt, uint32_t imm16) { return enc_ri16(0x82, rt, imm16); }
static uint32_t enc_iohl(uint32_t rt, uint32_t imm16) { return enc_ri16(0xC1, rt, imm16); }
static uint32_t enc_ila(uint32_t rt, uint32_t imm18) { return (0x21u << 25) | ((imm18 & 0x3FFFFu) << 7) | (rt & 0x7Fu); }
/* RI16 relative branches: the target is (i16*4) added to the instruction's own pc. */
static uint32_t enc_br_relative(uint32_t op9, uint32_t rt, uint32_t pc, uint32_t target_ls) {
    return enc_ri16(op9, rt, (uint32_t)(int32_t)(((int32_t)(target_ls - pc)) / 4));
}
#define enc_br(rt, pc, target)   enc_br_relative(0x64, rt, pc, target)
#define enc_brz(rt, pc, target)  enc_br_relative(0x40, rt, pc, target)
#define enc_brnz(rt, pc, target) enc_br_relative(0x42, rt, pc, target)
#define enc_brsl(rt, pc, target) enc_br_relative(0x66, rt, pc, target)
static uint32_t enc_lqa(uint32_t rt, uint32_t target_ls) { return enc_ri16(0x61, rt, target_ls / 4); }
static uint32_t enc_stqa(uint32_t rt, uint32_t target_ls) { return enc_ri16(0x41, rt, target_ls / 4); }
static uint32_t enc_bi(uint32_t rt, uint32_t ra) { return enc_rr(0x1A8, rt, ra, 0); }
static uint32_t enc_rdch(uint32_t rt, uint32_t channel) { return enc_rr(0x00D, rt, channel, 0); }
static uint32_t enc_wrch(uint32_t rt, uint32_t channel) { return enc_rr(0x10D, rt, channel, 0); }
static uint32_t enc_rchcnt(uint32_t rt, uint32_t channel) { return enc_rr(0x00F, rt, channel, 0); }
static uint32_t enc_lqd(uint32_t rt, uint32_t ra, int32_t byte_offset) { return enc_ri10(0x34, rt, ra, (uint32_t)(byte_offset / 16)); }
static uint32_t enc_stqd(uint32_t rt, uint32_t ra, int32_t byte_offset) { return enc_ri10(0x24, rt, ra, (uint32_t)(byte_offset / 16)); }
static uint32_t enc_lqx(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x1C4, rt, ra, rb); }
static uint32_t enc_stqx(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x144, rt, ra, rb); }
static uint32_t enc_a(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0xC0, rt, ra, rb); }
static uint32_t enc_ai(uint32_t rt, uint32_t ra, int32_t imm) { return enc_ri10(0x1C, rt, ra, (uint32_t)imm); }
static uint32_t enc_sf(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x40, rt, ra, rb); }
static uint32_t enc_cgti(uint32_t rt, uint32_t ra, uint32_t imm) { return enc_ri10(0x4C, rt, ra, imm); }
static uint32_t enc_xor(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x241, rt, ra, rb); }
static uint32_t enc_andi(uint32_t rt, uint32_t ra, uint32_t imm) { return enc_ri10(0x14, rt, ra, imm); }
static uint32_t enc_ori(uint32_t rt, uint32_t ra, uint32_t imm) { return enc_ri10(0x04, rt, ra, imm); }
static uint32_t enc_shli(uint32_t rt, uint32_t ra, uint32_t amount) { return enc_ri7(0x7B, rt, ra, amount); }
static uint32_t enc_shl(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x5B, rt, ra, rb); }
static uint32_t enc_roth(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x5C, rt, ra, rb); }
static uint32_t enc_roti(uint32_t rt, uint32_t ra, uint32_t amount) { return enc_ri7(0x78, rt, ra, amount); }
static uint32_t enc_rotqbyi(uint32_t rt, uint32_t ra, uint32_t amount) { return enc_ri7(0x1FC, rt, ra, amount); }
static uint32_t enc_cgt(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x240, rt, ra, rb); }
static uint32_t enc_fa(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x2C4, rt, ra, rb); }
static uint32_t enc_fm(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x2C6, rt, ra, rb); }
static uint32_t enc_fma(uint32_t rc, uint32_t ra, uint32_t rb, uint32_t rthree) { return enc_rrr(0xE, rc, ra, rb, rthree); }
static uint32_t enc_selb(uint32_t rc, uint32_t ra, uint32_t rb, uint32_t rthree) { return enc_rrr(0x8, rc, ra, rb, rthree); }
static uint32_t enc_shufb(uint32_t rc, uint32_t ra, uint32_t rb, uint32_t rthree) { return enc_rrr(0xB, rc, ra, rb, rthree); }
static uint32_t enc_cbd(uint32_t rt, uint32_t ra, uint32_t imm7) { return enc_ri7(0x1F4, rt, ra, imm7); }
static uint32_t enc_cwd(uint32_t rt, uint32_t ra, uint32_t imm7) { return enc_ri7(0x1F6, rt, ra, imm7); }
static uint32_t enc_gb(uint32_t rt, uint32_t ra) { return enc_rr(0x1B0, rt, ra, 0); }
static uint32_t enc_gbh(uint32_t rt, uint32_t ra) { return enc_rr(0x1B1, rt, ra, 0); }
static uint32_t enc_heq(uint32_t rt, uint32_t ra, uint32_t rb) { return enc_rr(0x3D8, rt, ra, rb); }

/* ---------------------------------------------------------------------------------------- host fixture (guest space) */
/* The host space is 48 MiB of guest RAM starting at EA 0 (elf_test_host_space_create is addressed from 0). The SPURS
 * kernel opens with a GET from EA 0xD00, so the whole window is accepted; a DMA past the end is refused, which is
 * what a wild address looks like. */
enum { FIXTURE_LOG_LIMIT = 20, FIXTURE_SPACE_BYTES = 48 * 1024 * 1024,
       FIXTURE_GUEST_LIMIT = FIXTURE_SPACE_BYTES };
typedef struct {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[8];
    unsigned checkpoints, cancel_at_checkpoint;
    /* First operations on channels and DMA, in order, logged for the kernel run. */
    unsigned logged;
    char operations[32][80];
} Fixture;

static int fixture_checkpoint(void *v, unsigned phase) {
    Fixture *f = v; (void)phase;
    ++f->checkpoints;
    return f->cancel_at_checkpoint && f->checkpoints >= f->cancel_at_checkpoint;
}
/* Services return an executor code, PS3_SPU_EXEC_OK (0) for "allowed" -- not a boolean. */
static int fixture_access(void *v, uint64_t address, size_t bytes, int write) {
    (void)v; (void)write;
    /* The whole guest window the fixture backs, so a kernel DMA that stays inside it is allowed. */
    return address < FIXTURE_GUEST_LIMIT && bytes <= FIXTURE_GUEST_LIMIT - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
/* The opcode column names the command family the way the hardware does: 0x20/0x40 plain PUT/GET, 0x24/0x44 list,
 * 0xC0/0xC8/0xCC barrier/EIEIO/sync. */
static const char *command_name(uint32_t command) {
    switch (command & ~0xFu) {
    case 0x20: return "put";
    case 0x40: return "get";
    case 0xC0: return "barrier";
    case 0xC8: return "eieio";
    case 0xCC: return "sync";
    }
    return "other";
}
static int fixture_reserve(void *v, const ps3_spu_transfer *request) {
    Fixture *f = v;
    if (f->logged < FIXTURE_LOG_LIMIT)
        snprintf(f->operations[f->logged++], sizeof f->operations[0], "mfc %s cmd=0x%02X lsa=0x%05X ea=0x%010llX bytes=%u",
                 command_name(request->command), request->command, request->lsa,
                 (unsigned long long)request->ea, request->bytes);
    return 0;
}
static void fixture_finish(void *v, const ps3_spu_transfer *request, ps3_guest_memory_status status, int committed,
                           const void *bytes) { (void)v; (void)request; (void)status; (void)committed; (void)bytes; }
/* The SPU channel numbers the kernel's own code names, for the readable record. */
static const char *channel_name(uint32_t channel) {
    switch (channel) {
    case SPU_RdEventStat: return "SPU_RdEventStat";
    case SPU_WrEventMask: return "SPU_WrEventMask";
    case SPU_WrEventAck: return "SPU_WrEventAck";
    case SPU_WrDec: return "SPU_WrDec";
    case SPU_RdDec: return "SPU_RdDec";
    case SPU_RdEventMask: return "SPU_RdEventMask";
    case SPU_RdMachStat: return "SPU_RdMachStat";
    case SPU_WrSRR0: return "SPU_WrSRR0";
    case SPU_RdSRR0: return "SPU_RdSRR0";
    case SPU_WrOutMbox: return "SPU_WrOutMbox";
    case SPU_RdInMbox: return "SPU_RdInMbox";
    case SPU_WrOutIntrMbox: return "SPU_WrOutIntrMbox";
    case MFC_LSA: return "MFC_LSA";
    case MFC_EAH: return "MFC_EAH";
    case MFC_EAL: return "MFC_EAL";
    case MFC_Size: return "MFC_Size";
    case MFC_TagID: return "MFC_TagID";
    case MFC_Cmd: return "MFC_Cmd";
    case MFC_WrTagMask: return "MFC_WrTagMask";
    case MFC_WrTagUpdate: return "MFC_WrTagUpdate";
    case MFC_RdTagStat: return "MFC_RdTagStat";
    case MFC_RdListStallStat: return "MFC_RdListStallStat";
    case MFC_WrListStallAck: return "MFC_WrListStallAck";
    case MFC_RdAtomicStat: return "MFC_RdAtomicStat";
    case MFC_RdTagMask: return "MFC_RdTagMask";
    }
    return "other";
}
/* Records one channel operation and the DMA it caused, in program order. */
static void fixture_observe_channel(void *v, unsigned kind, uint32_t channel) {
    Fixture *f = v;
    if (f->logged >= FIXTURE_LOG_LIMIT) return;
    static const char *const verbs[PS3_SPU_OBS_READ_CHANNEL_COUNT + 1] = { "?", "wrch", "rdch", "rchcnt" };
    snprintf(f->operations[f->logged++], sizeof f->operations[0], "%s %s (ch %u)",
             kind <= PS3_SPU_OBS_READ_CHANNEL_COUNT ? verbs[kind] : "?", channel_name(channel), channel);
}
static ps3_clock_status fixture_now_microseconds(void *context, uint64_t *out) { (void)context; *out = 1000u; return PS3_CLOCK_OK; }

static void fixture_open(Fixture *f) {
    memset(f, 0, sizeof *f);
    f->space = elf_test_host_space_create(FIXTURE_SPACE_BYTES);
    if (!f->space) { fprintf(stderr, "FIXTURE_SPACE_FAILED\n"); exit(1); }
    if (ps3_pthread_guard_init(&f->guard)) { fprintf(stderr, "FIXTURE_GUARD_FAILED\n"); exit(1); }
    f->backing = elf_test_host_space_port(f->space);
    /* The domain's backing port is the host space's own port, so its context stays the space. */
    ps3_guest_memory memory = f->backing;
    if (ps3_atomic_domain_init(&f->domain, memory, ps3_pthread_guard_port(&f->guard), f->records, 8, NULL, 0)) {
        fprintf(stderr, "FIXTURE_DOMAIN_FAILED\n"); exit(1);
    }
    /* Commit the whole space: the SPURS kernel's first GET reads EA 0xD00, below any conventional page 1. */
    if (ps3_atomic_commit(&f->domain, 0, FIXTURE_SPACE_BYTES)) { fprintf(stderr, "FIXTURE_COMMIT_FAILED\n"); exit(1); }
}
static void fixture_close(Fixture *f) {
    ps3_pthread_guard_destroy(&f->guard);
    elf_test_host_space_destroy(f->space);
}
static ps3_spu_executor_services fixture_services(Fixture *f) {
    ps3_spu_executor_services services = { f, fixture_access, fixture_checkpoint, fixture_reserve, fixture_finish };
    return services;
}
static ps3_spu_executor *interpreter_owner(Fixture *f, uint32_t spu_id) {
    ps3_spu_executor *owner = calloc(1, sizeof *owner);
    if (!owner) { fprintf(stderr, "OWNER_ALLOC_FAILED\n"); exit(1); }
    if (ps3_spu_executor_init(owner, &f->domain, spu_id, fixture_services(f))) { fprintf(stderr, "OWNER_INIT_FAILED\n"); exit(1); }
    ps3_spu_executor_set_channel_observer(owner, fixture_observe_channel, f);
    return owner;
}
/* Admits the owner for interpreted execution, then starts it with the ordinary start config (no linked image). */
static void interpreter_start(ps3_spu_executor *owner) {
    ps3_spu_executor_set_interpreter(owner);
    ps3_spu_executor_start_config config = { .image = NULL, .image_id = -1,
                                             .clock = { NULL, fixture_now_microseconds },
                                             .thread_option = PS3_SPU_THREAD_OPTION_NONE, .hop_budget = 1 };
    int status = ps3_spu_executor_start(owner, &config);
    if (status) { fprintf(stderr, "OWNER_START_FAILED status=%d\n", status); exit(1); }
}
static void interpreter_release(ps3_spu_executor *owner) {
    if (ps3_spu_executor_release(owner)) { fprintf(stderr, "OWNER_RELEASE_FAILED\n"); exit(1); }
    free(owner);
}

/* --------------------------------------------------------------------------------------------- program assembly */
typedef struct { uint32_t pc; } Assembler;
static void emit(ps3_spu_executor *owner, Assembler *a, uint32_t instruction) {
    spu_ls_write32(&owner->core, a->pc, instruction);
    a->pc += 4;
}
static void write_reg(ps3_spu_executor *owner, unsigned reg, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    owner->core.gpr[reg]._u32[0] = w0; owner->core.gpr[reg]._u32[1] = w1;
    owner->core.gpr[reg]._u32[2] = w2; owner->core.gpr[reg]._u32[3] = w3;
}
static uint32_t ls_word(const ps3_spu_executor *owner, uint32_t address) { return spu_ls_read32(&owner->core, address); }

static int failures;
#define REQUIRE(condition, ...) do { if (!(condition)) {                    \
    fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition);       \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

/* ================================================================================ 1. hand-assembled SPU programs */
/* Every case is a builder + the register/LS expectations derived by hand above it. */

/* ---- integer arithmetic, compares and immediates ---- */
enum { ARITHMETIC_PC = 0x1000 };
static uint32_t build_arithmetic(ps3_spu_executor *owner) {
    Assembler a = { ARITHMETIC_PC };
    emit(owner, &a, enc_il(1, 0x1234));          /* r1.w0 = 0x00001234 */
    emit(owner, &a, enc_ilhu(2, 0xBEEF));        /* r2.w0 = 0xBEEF0000 */
    emit(owner, &a, enc_a(3, 1, 2));             /* r3.w0 = 0xBEEF1234 */
    emit(owner, &a, enc_sf(4, 1, 2));            /* r4.w0 = r2 - r1           = 0xBEEEEDCC */
    emit(owner, &a, enc_andi(5, 3, 0x1F0));      /* r5.w0 = 0xBEEF1234 & 0x1F0 = 0x00000030 (0x1234 & 0x1F0 is already 0x30) */
    emit(owner, &a, enc_ori(6, 1, 0x0001));      /* r6.w0 = 0x1234 | 1         = 0x1235 */
    emit(owner, &a, enc_xor(7, 3, 4));           /* r7.w0 = BEEF1234 ^ BEEEEDCC = 0x0001FFF8 */
    emit(owner, &a, enc_cgti(8, 3, 0x100u));     /* r3 (negative) > 0x100 ?     = 0 */
    emit(owner, &a, enc_cgt(9, 1, 3));           /* 0x1234 > 0xBEEF1234 signed? = 0 + (1) */
    emit(owner, &a, enc_il(10, -1));             /* r10.w0 = 0xFFFFFFFF */
    emit(owner, &a, enc_iohl(11, 0x0042));       /* r11.w0 = 0x0042 (r11 was zero) */
    emit(owner, &a, enc_ila(12, 0x2ABCD));       /* r12.w0 = 0x2ABCD */
    emit(owner, &a, enc_ai(13, 3, -1));          /* r13.w0 = 0xBEEF1233 */
    emit(owner, &a, enc_stop(0x3FFF));
    return a.pc;
}
static void check_arithmetic(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    REQUIRE(g[1]._u32[0] == 0x00001234u && g[1]._u32[3] == 0x00001234u, "il r1=%08X (splatted)", g[1]._u32[0]);
    REQUIRE(g[2]._u32[0] == 0xBEEF0000u, "ilhu r2=%08X", g[2]._u32[0]);
    REQUIRE(g[3]._u32[0] == 0xBEEF1234u, "a r3=%08X", g[3]._u32[0]);
    REQUIRE(g[4]._u32[0] == 0xBEEEEDCCu, "sf r4=%08X", g[4]._u32[0]);
    REQUIRE(g[5]._u32[0] == 0x00000030u, "andi 0x1F0 r5=%08X", g[5]._u32[0]);
    REQUIRE(g[6]._u32[0] == 0x00001235u, "ori r6=%08X", g[6]._u32[0]);
    REQUIRE(g[7]._u32[0] == 0x0001FFF8u, "xor r7=%08X", g[7]._u32[0]);
    REQUIRE(g[8]._u32[0] == 0x00000000u, "cgti r8=%08X", g[8]._u32[0]);
    REQUIRE(g[9]._u32[0] == 0xFFFFFFFFu, "cgt 0x1234 > 0xBEEF1234 (signed) r9=%08X", g[9]._u32[0]);
    REQUIRE(g[10]._u32[0] == 0xFFFFFFFFu, "il -1 r10=%08X", g[10]._u32[0]);
    REQUIRE(g[11]._u32[0] == 0x00000042u, "iohl r11=%08X", g[11]._u32[0]);
    REQUIRE(g[12]._u32[0] == 0x0002ABCDu, "ila r12=%08X", g[12]._u32[0]);
    REQUIRE(g[13]._u32[0] == 0xBEEF1233u, "ai r13=%08X", g[13]._u32[0]);
}

/* ---- shifts and rotates ---- */
enum { SHIFT_PC = 0x1400, SHIFT_WORD = 0x00000303u };
static uint32_t build_shifts(ps3_spu_executor *owner) {
    Assembler a = { SHIFT_PC };
    emit(owner, &a, enc_il(1, 0x0303));        /* r1 = { 0x00000303, 0, 0, 0 } */
    emit(owner, &a, enc_shli(2, 1, 8));        /* r2.w0 = 0x00030300 */
    emit(owner, &a, enc_il(3, 4));
    emit(owner, &a, enc_shl(4, 2, 3));         /* r4.w0 = 0x03030000 */
    emit(owner, &a, enc_shli(5, 1, 32));       /* a full-word shift: lanes become 0 */
    emit(owner, &a, enc_roti(6, 1, 32));       /* a full-word rotate: lanes shift by one word */
    emit(owner, &a, enc_roth(7, 1, 3));        /* rotate the halfwords right by r3 (= 4) */
    emit(owner, &a, enc_rotqbyi(8, 1, 2));     /* bytes rotate inside the quadword */
    emit(owner, &a, enc_stop(0x3FFF));
    return a.pc;
}
static void check_shifts(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    REQUIRE(g[1]._u32[0] == SHIFT_WORD && g[1]._u32[3] == SHIFT_WORD, "il r1=%08X (splatted)", g[1]._u32[0]);
    /* shli 8: 0x0303 << 8 = 0x030300. shl by 4: 0x030300 << 4 = 0x0303000 (a 32-bit field, so bit 28+ falls off). */
    REQUIRE(g[2]._u32[0] == 0x00030300u, "shli 8 r2=%08X", g[2]._u32[0]);
    REQUIRE(g[4]._u32[0] == 0x00303000u, "shl 4 of 0x030300 r4=%08X", g[4]._u32[0]);
    /* shifts/rotates are per-word (and per-halfword for roth), so every lane repeats the preferred word. */
    REQUIRE(g[5]._u32[0] == 0x00000000u, "shli 32 r5.w0=%08X", g[5]._u32[0]);
    REQUIRE(g[6]._u32[0] == SHIFT_WORD && g[6]._u32[1] == SHIFT_WORD, "roti 32 (per-word) r6=%08X,%08X",
            g[6]._u32[0], g[6]._u32[1]);
    REQUIRE(g[7]._u32[0] == 0x00003030u, "roth by r3 (=4) halfwords r7.w0=%08X", g[7]._u32[0]);
    /* rotqbyi is the quadword-wide byte rotate: the low byte 0x03 moves to byte 0, byte 0 to byte 1, etc. */
    REQUIRE(g[8]._u32[0] == 0x03030000u && g[8]._u32[3] == 0x03030000u, "rotqbyi 2 r8=%08X,%08X",
            g[8]._u32[0], g[8]._u32[3]);
}

/* ---- loads and stores: lqd/stqd, lqx/stqx, lqa/stqa ---- */
enum { LOADSTORE_PC = 0x1800, SOURCE_QUAD = 0x800, STORED_QUAD = 0x900, INDEXED_QUAD = 0x940, ABSOLUTE_QUAD = 0x980 };
static uint32_t build_loads_stores(ps3_spu_executor *owner) {
    spu_ls_write32(&owner->core, SOURCE_QUAD + 0, 0x11223344u);
    spu_ls_write32(&owner->core, SOURCE_QUAD + 4, 0x55667788u);
    spu_ls_write32(&owner->core, SOURCE_QUAD + 8, 0x99AABBCCu);
    spu_ls_write32(&owner->core, SOURCE_QUAD + 12, 0xDDEEFF00u);
    Assembler a = { LOADSTORE_PC };
    emit(owner, &a, enc_il(1, 0x0800));                      /* r1.w0 = 0x800 */
    emit(owner, &a, enc_il(2, 0));                           /* r2 = 0 */
    emit(owner, &a, enc_lqd(3, 1, 0));                       /* r3 = LS[0x800] */
    emit(owner, &a, enc_stqd(3, 2, STORED_QUAD));            /* LS[0x900] = r3 */
    emit(owner, &a, enc_ai(4, 2, 0x40));                     /* r4 = 0x40 */
    emit(owner, &a, enc_lqx(5, 2, 4));                       /* r5 = LS[0x040] (zero) */
    emit(owner, &a, enc_ila(4, INDEXED_QUAD));               /* r4 = 0x940, the stqx index */
    emit(owner, &a, enc_stqx(3, 2, 4));                      /* LS[0x940] = r3 */
    emit(owner, &a, enc_lqa(6, SOURCE_QUAD));                /* r6 = LS[0x800] */
    emit(owner, &a, enc_stqa(6, ABSOLUTE_QUAD));             /* LS[0x980] = r6 */
    emit(owner, &a, enc_stop(0x3FFF));
    return a.pc;
}
static void check_loads_stores(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    REQUIRE(g[3]._u32[0] == 0x11223344u && g[3]._u32[3] == 0xDDEEFF00u, "lqd r3=%08X,%08X", g[3]._u32[0], g[3]._u32[3]);
    REQUIRE(g[5]._u32[0] == 0 && g[5]._u32[3] == 0, "lqx r5=%08X", g[5]._u32[0]);
    REQUIRE(g[6]._u32[0] == 0x11223344u, "lqa r6=%08X", g[6]._u32[0]);
    REQUIRE(ls_word(owner, STORED_QUAD) == 0x11223344u && ls_word(owner, STORED_QUAD + 12) == 0xDDEEFF00u,
            "stqd LS[900]=%08X", ls_word(owner, STORED_QUAD));
    REQUIRE(ls_word(owner, INDEXED_QUAD + 8) == 0x99AABBCCu, "stqx LS[948]=%08X", ls_word(owner, INDEXED_QUAD + 8));
    REQUIRE(ls_word(owner, ABSOLUTE_QUAD + 4) == 0x55667788u, "stqa LS[984]=%08X", ls_word(owner, ABSOLUTE_QUAD + 4));
}

/* ---- branches: br/brz/brnz, brsl as a call, bi as return ---- */
/* 0x1C00 il r1,0 | 0x1C04 brz (taken -> 0x1C0C) | 0x1C08 il r1,0x7777 (skipped) | 0x1C0C ila r7,0x7777
 * | 0x1C10 il r2,0 | 0x1C14.. loop | ... | call site | stop | callee at 0x1C80. */
/* Root fix (C1a): the loop is ai 0x1C14, cgti 0x1C18, brz 0x1C1C, brnz 0x1C20, il 0x1C24; the worker encoded
 * brz/brnz with pcs one word early and put AFTER_LOOP on top of the brnz. */
enum { BRANCH_PC = 0x1C00, BRANCH_AFTER_LOOP = 0x1C28, BRANCH_LOOP = 0x1C14, BRANCH_CALL_TARGET = 0x1C80 };
static uint32_t build_branches(ps3_spu_executor *owner) {
    Assembler a = { BRANCH_PC };
    emit(owner, &a, enc_il(1, 0));                                  /* r1 = 0 */
    emit(owner, &a, enc_brz(1, BRANCH_PC + 4, BRANCH_PC + 12));     /* r1 == 0: taken, skipping the next word */
    emit(owner, &a, enc_il(1, 0x7777));                             /* skipped */
    emit(owner, &a, enc_ila(7, 0x7777));                            /* r7 = 0x7777 */
    emit(owner, &a, enc_il(2, 0));                                  /* r2 = 0, at 0x1C10 */
    a.pc = BRANCH_LOOP;                                             /* 0x1C14: r2 counts 1..4, exits when r2 > 3 */
    emit(owner, &a, enc_ai(2, 2, 1));                               /* r2 += 1 */
    emit(owner, &a, enc_cgti(3, 2, 3));                             /* r3 = r2 > 3 ? -1 : 0 */
    emit(owner, &a, enc_brz(3, BRANCH_LOOP + 8, BRANCH_LOOP));      /* at 0x1C1C: r3 == 0 -> loop */
    emit(owner, &a, enc_brnz(3, BRANCH_LOOP + 12, BRANCH_AFTER_LOOP)); /* at 0x1C20: r3 != 0 -> 0x1C28 */
    emit(owner, &a, enc_il(6, 0x6666));                             /* unreachable */
    a.pc = BRANCH_AFTER_LOOP;
    emit(owner, &a, enc_brsl(0, BRANCH_AFTER_LOOP, BRANCH_CALL_TARGET));   /* call: r0 <- AFTER_LOOP+4 */
    emit(owner, &a, enc_stop(0x3FFF));
    /* The program ENDS here, at the stop the callee returns into -- not at the callee's own last instruction,
     * which is emitted below only so the assembler layout is readable. */
    const uint32_t program_end = a.pc;
    a.pc = BRANCH_CALL_TARGET;
    emit(owner, &a, enc_il(4, 5));                                  /* r4 = 5 */
    emit(owner, &a, enc_a(5, 4, 2));                                /* r5 = 5 + r2 = 9 */
    emit(owner, &a, enc_bi(0, 0));                                  /* return through r0 */
    return program_end;
}
static void check_branches(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    REQUIRE(g[1]._u32[0] == 0, "brz taken (r1 stays 0) r1=%08X", g[1]._u32[0]);
    REQUIRE(g[7]._u32[0] == 0x7777u, "skipped-instruction guard r7=%08X", g[7]._u32[0]);
    REQUIRE(g[6]._u32[0] == 0, "brnz skipped r6=%08X", g[6]._u32[0]);
    REQUIRE(g[2]._u32[0] == 4, "loop count r2=%08X", g[2]._u32[0]);
    REQUIRE(g[4]._u32[0] == 5 && g[5]._u32[0] == 9, "call r4=%08X r5=%08X", g[4]._u32[0], g[5]._u32[0]);
    REQUIRE(g[0]._u32[0] == BRANCH_AFTER_LOOP + 4, "link r0=%08X", g[0]._u32[0]);
}

/* ---- select, shufb, constant generators, gather ---- */
enum { VECTOR_PC = 0x2000 };
static uint32_t build_vector(ps3_spu_executor *owner) {
    write_reg(owner, 10, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
    write_reg(owner, 11, 0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu);
    write_reg(owner, 12, 0xFFFFFFFFu, 0x00000000u, 0xFFFFFFFFu, 0x00000000u);
    write_reg(owner, 13, 0x0F0E0D0Cu, 0x0B0A0908u, 0x07060504u, 0x03020100u);
    write_reg(owner, 22, 0x00010203u, 0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu);   /* the identity shuffle */
    Assembler a = { VECTOR_PC };
    /* RRR: the destination is the rc field (bits 21-27) and the FOURTH operand is the rt field (bits 0-6). */
    emit(owner, &a, enc_selb(14, 10, 11, 12));    /* r14 = selb(r10, r11, r12) = (r10 & ~r12) | (r11 & r12) */
    emit(owner, &a, enc_shufb(15, 10, 11, 13));   /* r15 = shufb(r10, r11, r13): bytes of r10/r11 by r13 */
    emit(owner, &a, enc_shufb(23, 22, 22, 22));   /* r23 = shufb(r22, r22, r22): identity control */
    emit(owner, &a, enc_il(16, 3));               /* the insertion position for cbd/cwd */
    emit(owner, &a, enc_cbd(17, 16, 0));          /* cbd: r15's selector with r16's preferred byte inserted */
    emit(owner, &a, enc_cwd(18, 16, 0));          /* cwd: same for a word */
    emit(owner, &a, enc_gb(19, 12));              /* gather bits of the FOUR words of r12 */
    emit(owner, &a, enc_gb(20, 10));              /* gather from r10: every word is odd */
    emit(owner, &a, enc_gbh(21, 12));             /* gather 8 halfwords of r12 */
    emit(owner, &a, enc_stop(0x3FFF));
    return a.pc;
}
static void check_vector(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    /* selb a,b,c picks a where c's bit is 0 and b where it is 1 (spu_helpers.h spu_selb). r12 is
     * { FFFFFFFF, 0, FFFFFFFF, 0 }, so lanes 0 and 2 take r11 and lanes 1 and 3 take r10. */
    REQUIRE(g[14]._u32[0] == 0xAAAAAAAAu && g[14]._u32[1] == 0x22222222u && g[14]._u32[2] == 0xCCCCCCCCu &&
            g[14]._u32[3] == 0x44444444u, "selb r14=%08X,%08X,%08X,%08X",
            g[14]._u32[0], g[14]._u32[1], g[14]._u32[2], g[14]._u32[3]);
    /* shufb: control byte 0F^3=0C, 0E^3=0D, ... so result byte t reads a's byte t of the pair (a||a). */
    REQUIRE(g[15]._u32[0] == 0x44444444u && g[15]._u32[1] == 0x33333333u && g[15]._u32[2] == 0x22222222u &&
            g[15]._u32[3] == 0x11111111u, "shufb (index 0F..00 reverses r10) r15=%08X,%08X,%08X,%08X",
            g[15]._u32[0], g[15]._u32[1], g[15]._u32[2], g[15]._u32[3]);
    REQUIRE(g[23]._u32[0] == 0x00010203u && g[23]._u32[3] == 0x0C0D0E0Fu, "shufb identity r23=%08X,%08X",
            g[23]._u32[0], g[23]._u32[3]);
    /* gb: r12's four words are FFFFFFFF, 0, FFFFFFFF, 0 -> their LSBs are 1,0,1,0 -> bit3,1 -> 0b1010 = 0xA. */
    REQUIRE(g[19]._u32[0] == 0x0Au && g[19]._u32[3] == 0, "gb r19=%08X", g[19]._u32[0]);
    /* gb: every word of r10 is odd -> all four LSBs -> 0xF. */
    REQUIRE(g[20]._u32[0] == 0x0Au, "gb (r10: every word odd except none) r20=%08X", g[20]._u32[0]);
    /* gbh: r12's eight halfwords are FFFF,FFFF,0,0,FFFF,FFFF,0,0 -> 1,1,0,0,1,1,0,0 -> 0b11001100 = 0xCC. */
    REQUIRE(g[21]._u32[0] == 0xCCu, "gbh r21=%08X", g[21]._u32[0]);
    /* cbd/cwd build shufb selectors: byte t <- b's byte t (0x10+t) except at the insertion position. */
    REQUIRE(g[17]._u32[0] == 0x10111203u && g[18]._u32[0] == 0x00010203u, "cbd r17=%08X cwd r18=%08X",
            g[17]._u32[0], g[18]._u32[0]);
}

/* ---- single-precision float subset: fa, fm, fma ---- */
enum { FLOAT_PC = 0x2400 };
static uint32_t float_bits(float value) { uint32_t bits = 0; memcpy(&bits, &value, 4); return bits; }
static uint32_t build_float(ps3_spu_executor *owner) {
    write_reg(owner, 1, float_bits(1.5f), float_bits(-2.5f), float_bits(3.0f), float_bits(0.5f));
    write_reg(owner, 2, float_bits(4.0f), float_bits(2.0f), float_bits(-1.0f), float_bits(8.0f));
    write_reg(owner, 3, float_bits(10.0f), float_bits(10.0f), float_bits(10.0f), float_bits(10.0f));
    Assembler a = { FLOAT_PC };
    emit(owner, &a, enc_fa(4, 1, 2));             /* 5.5, -0.5, 2.0, 8.5 */
    emit(owner, &a, enc_fm(5, 1, 2));             /* 6.0, -5.0, -3.0, 4.0 */
    emit(owner, &a, enc_fma(6, 1, 2, 3));         /* r1*r2 + r3 = 16.0, 5.0, 7.0, 14.0 */
    emit(owner, &a, enc_stop(0x3FFF));
    return a.pc;
}
static int nearly(float actual, float expected) {
    float difference = actual - expected; if (difference < 0) difference = -difference;
    return difference <= 1e-6f;
}
static void check_float(ps3_spu_executor *owner) {
    const u128 *g = owner->core.gpr;
    static const float add[4] = { 5.5f, -0.5f, 2.0f, 8.5f }, mul[4] = { 6.0f, -5.0f, -3.0f, 4.0f },
                       fused[4] = { 16.0f, 5.0f, 7.0f, 14.0f };
    for (int lane = 0; lane < 4; ++lane) {
        REQUIRE(nearly(g[4]._f32[lane], add[lane]), "fa lane %d = %f", lane, (double)g[4]._f32[lane]);
        REQUIRE(nearly(g[5]._f32[lane], mul[lane]), "fm lane %d = %f", lane, (double)g[5]._f32[lane]);
        REQUIRE(nearly(g[6]._f32[lane], fused[lane]), "fma lane %d = %f", lane, (double)g[6]._f32[lane]);
    }
}

/* ---- the resumable stop: pc after it, the code reported, the owner still usable ---- */
enum { STOP_PC = 0x2800 };
/* Two stops in one program: 0x110 then 0x3FFF. run_case runs to (and compares) the FIRST, so the builder returns
 * the pc right after it; check_stop resumes from there and verifies the second. */
static uint32_t build_stop(ps3_spu_executor *owner) {
    Assembler a = { STOP_PC };
    emit(owner, &a, enc_il(1, 7));
    emit(owner, &a, enc_stop(0x0110));
    const uint32_t after_first_stop = a.pc;
    emit(owner, &a, enc_il(2, 9));
    emit(owner, &a, enc_stop(0x3FFF));
    return after_first_stop;
}
/* run_case already ran the program to its first stop (0x110 at STOP_PC+4, leaving pc at STOP_PC+8); this verifies
 * what that first stop did and then resumes from it to the program's final stop. */
static void check_stop(ps3_spu_executor *owner) {
    REQUIRE(owner->core.gpr[1]._u32[0] == 7, "first instruction ran r1=%08X", owner->core.gpr[1]._u32[0]);
    REQUIRE(owner->core.gpr[2]._u32[0] == 0, "instruction after the stop not executed r2=%08X", owner->core.gpr[2]._u32[0]);
    REQUIRE(owner->stopped == 0, "a stop is not a fault");
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, owner->core.pc, 100, &result) == 0, "resume");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED && result.code == 0x3FFFu, "resumed code=%X kind=%d", result.code, result.kind);
    REQUIRE(result.pc == STOP_PC + 16, "resumed pc=%05X", result.pc);
    REQUIRE(owner->core.gpr[2]._u32[0] == 9, "resumed r2=%08X", owner->core.gpr[2]._u32[0]);
}

/* ---- a conditional halt is a terminal fault ---- */
enum { HALT_PC = 0x2C00, UNDECODABLE_WORD = 0x01200000u };
static void check_halt_and_unsupported(void) {
    {
        Fixture f; fixture_open(&f);
        ps3_spu_executor *owner = interpreter_owner(&f, 0);
        interpreter_start(owner);
        Assembler a = { HALT_PC };
        emit(owner, &a, enc_il(1, 5));
        emit(owner, &a, enc_il(2, 5));
        emit(owner, &a, enc_heq(1, 1, 2));
        ps3_spu_interp_result result;
        REQUIRE(ps3_spu_executor_run_interp(owner, HALT_PC, 100, &result) == 0, "halt run");
        REQUIRE(result.kind == PS3_SPU_INTERP_FAULT, "halt kind=%d", result.kind);
        REQUIRE(result.code == PS3_SPU_EXEC_UNSUPPORTED, "halt code=%d", result.code);
        REQUIRE(result.stop_code == 0x1000u, "halt stop_code=%X (the assertion HALT value)", result.stop_code);
        /* result.opcode is the raw heq word that halted. */
        REQUIRE(result.opcode == enc_heq(1, 1, 2), "halt opcode=%08X", result.opcode);
        REQUIRE(owner->stopped == 1, "halt stops the owner");
        REQUIRE(ps3_spu_executor_run_interp(owner, HALT_PC, 100, &result) == (int)result.code, "stopped owner refuses");
        interpreter_release(owner); fixture_close(&f);
    }
    {
        Fixture f; fixture_open(&f);
        ps3_spu_executor *owner = interpreter_owner(&f, 0);
        interpreter_start(owner);
        Assembler a = { HALT_PC };
        emit(owner, &a, UNDECODABLE_WORD);               /* 0x01200000: no format table names it */
        ps3_spu_interp_result result;
        REQUIRE(ps3_spu_executor_run_interp(owner, HALT_PC, 100, &result) == 0, "unknown opcode run");
        REQUIRE(result.kind == PS3_SPU_INTERP_FAULT && result.code == PS3_SPU_EXEC_UNSUPPORTED,
                "unknown opcode kind=%d code=%d", result.kind, result.code);
        /* An undecodable word is named by its own number, and no architectural halt value is claimed. */
        REQUIRE(result.opcode == UNDECODABLE_WORD, "unknown opcode word=%08X", result.opcode);
        REQUIRE(result.stop_code == 0u, "unknown opcode stop_code=%X", result.stop_code);
        interpreter_release(owner); fixture_close(&f);
    }
}

/* ---- the budget ---- */
enum { BUDGET_PC = 0x3000, BUDGET_ROUNDS = 10 };
static void check_budget(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 0);
    interpreter_start(owner);
    Assembler a = { BUDGET_PC };
    emit(owner, &a, enc_il(1, 0));                                  /* r1 = 0 */
    for (int round = 0; round < BUDGET_ROUNDS; ++round) emit(owner, &a, enc_ai(1, 1, 1));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, BUDGET_PC, 4, &result) == 0, "budget run");
    REQUIRE(result.kind == PS3_SPU_INTERP_BUDGET, "budget kind=%d", result.kind);
    REQUIRE(result.instructions == 4, "budget instructions=%llu", (unsigned long long)result.instructions);
    REQUIRE(result.pc == BUDGET_PC + 16, "budget pc=%05X", result.pc);
    REQUIRE(owner->core.gpr[1]._u32[0] == 3, "budget r1=%08X", owner->core.gpr[1]._u32[0]);
    REQUIRE(ps3_spu_executor_run_interp(owner, result.pc, 1000, &result) == 0, "budget resume");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED && owner->core.gpr[1]._u32[0] == BUDGET_ROUNDS,
            "budget resume r1=%08X kind=%d", owner->core.gpr[1]._u32[0], result.kind);
    {
        ps3_spu_interp_result refused;
        REQUIRE(ps3_spu_executor_run_interp(owner, BUDGET_PC + 2, 4, &refused) == PS3_SPU_EXEC_INVALID, "misaligned pc");
        REQUIRE(ps3_spu_executor_run_interp(owner, BUDGET_PC, 0, &refused) == PS3_SPU_EXEC_INVALID, "zero budget");
        REQUIRE(ps3_spu_executor_run_interp(owner, SPU_LS_SIZE, 4, &refused) == PS3_SPU_EXEC_INVALID, "pc past LS");
    }
    interpreter_release(owner); fixture_close(&f);
}

/* ---- a lifted-mode owner cannot run interpreted code ---- */
static void check_lifted_owner_refused(void) {
    /* A LIFTED-mode owner refuses to start with no usable linked image, and an unstarted owner refuses to run. */
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 0);
    ps3_spu_linked_image empty_image = { NULL, NULL, 0 };
    ps3_spu_executor_start_config config = { .image = &empty_image, .image_id = 0,
                                             .clock = { NULL, fixture_now_microseconds },
                                             .thread_option = PS3_SPU_THREAD_OPTION_NONE, .hop_budget = 1 };
    REQUIRE(ps3_spu_executor_start(owner, &config) == PS3_SPU_EXEC_INVALID, "empty image refused");
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, 0, 4, &result) == PS3_SPU_EXEC_INVALID, "unstarted owner refused");
    interpreter_release(owner); fixture_close(&f);
    {
        /* An INTERPRETER owner needs no image at all, but still refuses interpreted runs once the owner is unstarted. */
        Fixture g; fixture_open(&g);
        ps3_spu_executor *plain = interpreter_owner(&g, 0);
        ps3_spu_interp_result refused;
        REQUIRE(ps3_spu_executor_run_interp(plain, 0, 4, &refused) == PS3_SPU_EXEC_INVALID,
                "unstarted interpreter owner refused");
        interpreter_release(plain); fixture_close(&g);
    }
}

/* ===================================================================== 2. the differential oracle (desktop spu_interp) */
typedef uint32_t (*program_builder)(ps3_spu_executor *);   /* returns the pc after the program's LAST stop */
/* Runs a case on the native interpreter and on the desktop interpreter from the SAME initial state (the local store
 * after the builder ran -- the programs also embed data -- and the registers the builder or the seeding produced), then
 * compares pc, the stop code, all 128 registers and the whole local store.
 *
 * `expected_stop` is the FIRST stop the program reaches; the expected pc is the builder's return value (the pc after
 * the program's last stop), so a builder always states its own end. A case whose program stops more than once (the
 * resumable-stop case) passes its first stop code here and verifies the rest itself.
 * The desktop oracle is run to the same first stop by giving it the same start pc: run once, it stops at the same
 * instruction, so the comparison covers exactly what the native run did. */
static void run_case(const char *name, uint32_t pc, uint32_t expected_stop,
                     program_builder build, void (*check)(ps3_spu_executor *), int seed_registers, uint64_t budget) {
    static unsigned char initial_ls[SPU_LS_SIZE];
    static u128 initial_gpr[128];
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 0);
    interpreter_start(owner);
    if (seed_registers)
        for (unsigned reg = 0; reg < 128; ++reg) write_reg(owner, reg, 0x1000u + reg * 4u, 0x2000u + reg, 0x3000u, 0x4000u + reg);
    const uint32_t expected_pc = build(owner);
    memcpy(initial_ls, owner->core.ls, SPU_LS_SIZE);
    memcpy(initial_gpr, owner->core.gpr, sizeof initial_gpr);
    ps3_spu_interp_result result;
    const int status = ps3_spu_executor_run_interp(owner, pc, budget, &result);
    REQUIRE(status == 0, "%s: run status=%d", name, status);
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED, "%s: kind=%d", name, result.kind);
    REQUIRE(result.code == expected_stop, "%s: stop code=%X expected %X", name, result.code, expected_stop);
    REQUIRE(result.pc == expected_pc, "%s: pc=%05X expected %05X", name, result.pc, expected_pc);

    /* The oracle runs from the same initial state to the same first stop; compare BEFORE check() runs, because check()
     * may drive the owner further (the resumable-stop case resumes it). */
    static unsigned char desktop_ls[SPU_LS_SIZE];
    static spu_context desktop;
    spu_context_init(&desktop, 0);
    desktop.ls = desktop_ls;
    memcpy(desktop_ls, initial_ls, SPU_LS_SIZE);
    memcpy(desktop.gpr, initial_gpr, sizeof desktop.gpr);
    const uint32_t desktop_stop = spu_interp_run(&desktop, pc);
    int mismatched_register = -1;
    for (unsigned reg = 0; reg < 128; ++reg)
        if (memcmp(&desktop.gpr[reg], &owner->core.gpr[reg], sizeof(u128))) { mismatched_register = (int)reg; break; }
    long mismatched_ls = -1;
    for (uint32_t address = 0; address < SPU_LS_SIZE; ++address)
        if (desktop_ls[address] != owner->core.ls[address]) { mismatched_ls = (long)address; break; }
    REQUIRE(desktop_stop == expected_stop, "%s: oracle stop=%X expected %X", name, desktop_stop, expected_stop);
    REQUIRE(desktop.pc == owner->core.pc, "%s: oracle pc=%05X native %05X", name, desktop.pc, owner->core.pc);
    REQUIRE(mismatched_register < 0, "%s: oracle register r%d differs", name, mismatched_register);
    if (mismatched_register >= 0)
        fprintf(stderr, "  r%d oracle=%08X,%08X,%08X,%08X native=%08X,%08X,%08X,%08X\n", mismatched_register,
                desktop.gpr[mismatched_register]._u32[0], desktop.gpr[mismatched_register]._u32[1],
                desktop.gpr[mismatched_register]._u32[2], desktop.gpr[mismatched_register]._u32[3],
                owner->core.gpr[mismatched_register]._u32[0], owner->core.gpr[mismatched_register]._u32[1],
                owner->core.gpr[mismatched_register]._u32[2], owner->core.gpr[mismatched_register]._u32[3]);
    REQUIRE(mismatched_ls < 0, "%s: oracle LS differs at 0x%05lX", name, mismatched_ls);
    /* The case's own expectations, checked against the state the first stop left (still intact: check() runs here). */
    check(owner);
    interpreter_release(owner); fixture_close(&f);
    if (!failures) printf("INTERP_PROGRAM ok name=%s insns=%llu oracle=desktop_interp\n", name, (unsigned long long)result.instructions);
}

/* ================================================================================ 3. the channels the interpreter added */
static int write_channel(ps3_spu_executor *owner, uint32_t channel, uint32_t value) {
    return ps3_spu_executor_write_channel(owner, channel, spu_make_preferred_u32(value));
}
static uint32_t read_channel(ps3_spu_executor *owner, uint32_t channel) {
    u128 value; memset(&value, 0, sizeof value);
    if (ps3_spu_executor_read_channel(owner, channel, &value)) return 0xDEADBEEFu;
    return value._u32[0];
}

/* The program blocks on a full out-mailbox write, then continues. */
enum { MBOX_PC = 0x3400 };
static void check_out_mailbox_pause(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 7);
    interpreter_start(owner);
    Assembler a = { MBOX_PC };
    emit(owner, &a, enc_il(1, 0xAA));
    emit(owner, &a, enc_wrch(1, SPU_WrOutMbox));   /* 0xAA -> out mailbox */
    emit(owner, &a, enc_il(2, 0xBB));
    emit(owner, &a, enc_wrch(2, SPU_WrOutMbox));   /* blocks: the mailbox is one deep */
    emit(owner, &a, enc_il(3, 0xCC));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, MBOX_PC, 100, &result) == 0, "mbox run");
    REQUIRE(result.kind == PS3_SPU_INTERP_PAUSED, "mbox kind=%d", result.kind);
    REQUIRE(result.channel == SPU_WrOutMbox, "mbox channel=%u", result.channel);
    REQUIRE(result.pc == MBOX_PC + 12, "mbox pc=%05X (at the blocking write)", result.pc);
    REQUIRE(owner->interp_paused == 1 && owner->interp_paused_pc == MBOX_PC + 12, "mbox latched pause");
    REQUIRE(owner->stopped == 0, "a pause is not a fault");
    uint32_t pending = 0, value = 0;
    REQUIRE(ps3_spu_executor_mbox_pending(owner, PS3_SPU_MAILBOX_OUT, &pending) == 0 && pending == 1,
            "mbox pending=%u", pending);
    REQUIRE(ps3_spu_executor_mbox_send(owner, PS3_SPU_MAILBOX_OUT, 1) == PS3_SPU_EXEC_UNSUPPORTED,
            "PPU send refused while full");
    REQUIRE(ps3_spu_executor_mbox_receive(owner, PS3_SPU_MAILBOX_OUT, &value) == 0 && value == 0xAAu,
            "PPU read value=%X", value);
    REQUIRE(ps3_spu_executor_mbox_receive(owner, PS3_SPU_MAILBOX_OUT, &value) == PS3_SPU_EXEC_UNSUPPORTED,
            "PPU read on empty refused");
    REQUIRE(ps3_spu_executor_run_interp(owner, result.pc, 100, &result) == 0, "mbox resume run");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED && result.code == 0x3FFFu, "mbox resume kind=%d", result.kind);
    REQUIRE(owner->core.gpr[3]._u32[0] == 0xCCu && owner->core.gpr[2]._u32[0] == 0xBBu, "mbox resume r2=%08X r3=%08X",
            owner->core.gpr[2]._u32[0], owner->core.gpr[3]._u32[0]);
    REQUIRE(ps3_spu_executor_mbox_receive(owner, PS3_SPU_MAILBOX_OUT, &value) == 0 && value == 0xBBu,
            "mbox second value=%X", value);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_OUT_MAILBOX write=28 pauses_when_full=1 resumes_after_receive=1");
}

/* The program blocks on an empty in-mailbox read; the PPU then sends. Four words fit. */
enum { INBOX_PC = 0x3600 };
static void check_in_mailbox_pause(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 1);
    interpreter_start(owner);
    Assembler a = { INBOX_PC };
    emit(owner, &a, enc_rdch(1, SPU_RdInMbox));
    emit(owner, &a, enc_rdch(2, SPU_RdInMbox));
    emit(owner, &a, enc_rchcnt(3, SPU_RdInMbox));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, INBOX_PC, 100, &result) == 0, "inbox run");
    REQUIRE(result.kind == PS3_SPU_INTERP_PAUSED && result.channel == SPU_RdInMbox, "inbox kind=%d channel=%u",
            result.kind, result.channel);
    REQUIRE(result.pc == INBOX_PC, "inbox pc=%05X (at the blocking read)", result.pc);
    uint32_t pending = 0;
    REQUIRE(ps3_spu_executor_mbox_pending(owner, PS3_SPU_MAILBOX_IN, &pending) == 0 && pending == 0, "inbox empty");
    for (uint32_t word = 0; word < 4; ++word)
        REQUIRE(ps3_spu_executor_mbox_send(owner, PS3_SPU_MAILBOX_IN, 0x40u + word) == 0, "inbox send %u", word);
    REQUIRE(ps3_spu_executor_mbox_send(owner, PS3_SPU_MAILBOX_IN, 0x44) == PS3_SPU_EXEC_UNSUPPORTED,
            "inbox send refused at hardware depth 4");
    REQUIRE(ps3_spu_executor_run_interp(owner, result.pc, 100, &result) == 0, "inbox resume run");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED, "inbox resume kind=%d", result.kind);
    REQUIRE(owner->core.gpr[1]._u32[0] == 0x40u && owner->core.gpr[2]._u32[0] == 0x41u, "inbox r1=%08X r2=%08X",
            owner->core.gpr[1]._u32[0], owner->core.gpr[2]._u32[0]);
    REQUIRE(owner->core.gpr[3]._u32[0] == 2u, "inbox rchcnt=%08X", owner->core.gpr[3]._u32[0]);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_IN_MAILBOX read=29 pauses_when_empty=1 depth=4 resumes_after_send=1 rchcnt=count");
}

/* Events: mask, a lock-line reservation loss fed by a real GETLLAR + a competing store, RdEventStat, WrEventAck. */
enum { EVENT_PC = 0x3800 };
static void check_events(void) {
    Fixture f; fixture_open(&f);
    /* The competing writer is a second owner of the same domain, as on a console. */
    ps3_spu_executor *writer = interpreter_owner(&f, 2);
    ps3_spu_executor *owner = interpreter_owner(&f, 3);
    interpreter_start(owner);
    interpreter_start(writer);   /* a channel write needs a started owner, exactly like any other SPU thread */
    write_channel(owner, MFC_LSA, 0);
    write_channel(owner, MFC_EAH, 0);
    write_channel(owner, MFC_EAL, 0x10000);
    write_channel(owner, MFC_TagID, 1);
    { int getllar_status = write_channel(owner, MFC_Cmd, MFC_GETLLAR_CMD);
      REQUIRE(getllar_status == 0, "GETLLAR status=%d", getllar_status); }
    /* No event yet: a blocking read pauses. */
    Assembler a = { EVENT_PC };
    emit(owner, &a, enc_il(1, PS3_SPU_EVENT_LOCK_LINE_LOST));
    emit(owner, &a, enc_wrch(1, SPU_WrEventMask));
    emit(owner, &a, enc_rdch(2, SPU_RdEventStat));
    emit(owner, &a, enc_rchcnt(3, SPU_RdEventStat));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, EVENT_PC, 100, &result) == 0, "event run");
    REQUIRE(result.kind == PS3_SPU_INTERP_PAUSED && result.channel == SPU_RdEventStat, "event kind=%d channel=%u",
            result.kind, result.channel);
    REQUIRE(read_channel(owner, SPU_RdEventMask) == PS3_SPU_EVENT_LOCK_LINE_LOST, "event mask=%X", read_channel(owner, SPU_RdEventMask));
    /* The other owner writes the reserved line: the reservation is lost. */
    /* A plain PUT back into the line the first owner reserved: its reservation is lost. MFC transfers need their
     * size staged as well as the addresses. */
    write_channel(writer, MFC_LSA, 0x80);
    write_channel(writer, MFC_EAH, 0);
    write_channel(writer, MFC_EAL, 0x10000);
    write_channel(writer, MFC_Size, 16);
    write_channel(writer, MFC_TagID, 1);
    { int put_status = write_channel(writer, MFC_Cmd, MFC_PUT_CMD);
      REQUIRE(put_status == 0, "competing PUT status=%d", put_status); }
    REQUIRE(ps3_spu_executor_run_interp(owner, result.pc, 100, &result) == 0, "event resume run");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED, "event resume kind=%d", result.kind);
    REQUIRE(owner->core.gpr[2]._u32[0] == PS3_SPU_EVENT_LOCK_LINE_LOST, "event status r2=%08X", owner->core.gpr[2]._u32[0]);
    /* Root fix (C1b): hardware semantics. RdEventStat reports pending & mask and clears nothing; WrEventAck clears
     * the given bits; acking a bit that is not pending is a no-op (the SPURS kernel does exactly that). */
    REQUIRE(owner->core.event_status == PS3_SPU_EVENT_LOCK_LINE_LOST, "RdEventStat left LR pending status=%X",
            owner->core.event_status);
    REQUIRE(write_channel(owner, SPU_WrEventAck, PS3_SPU_EVENT_LOCK_LINE_LOST) == 0, "WrEventAck");
    REQUIRE(owner->core.event_status == 0, "ack cleared LR status=%X", owner->core.event_status);
    /* Nothing pending now: running the program again from its own rdch PAUSES there, never a fabricated event. */
    REQUIRE(ps3_spu_executor_run_interp(owner, EVENT_PC + 8, 100, &result) == 0, "second event run");
    REQUIRE(result.kind == PS3_SPU_INTERP_PAUSED && result.channel == SPU_RdEventStat,
            "second event kind=%d channel=%u", result.kind, result.channel);
    REQUIRE(result.pc == EVENT_PC + 8, "second event pc=%05X (left at the rdch)", result.pc);
    REQUIRE(owner->core.gpr[2]._u32[0] == PS3_SPU_EVENT_LOCK_LINE_LOST,
            "the paused rdch did not overwrite r2=%08X", owner->core.gpr[2]._u32[0]);
    /* Acking a non-pending bit is accepted and changes nothing. */
    REQUIRE(write_channel(owner, SPU_WrEventAck, PS3_SPU_EVENT_SN) == 0, "WrEventAck of a non-pending bit is a no-op");
    REQUIRE(owner->core.event_status == 0, "non-pending ack left status=%X", owner->core.event_status);
    interpreter_release(owner); interpreter_release(writer); fixture_close(&f);
    puts("CHANNEL_EVENTS RdEventStat=0 WrEventMask=1 WrEventAck=2 RdEventMask=11 lr_from_reservation_loss=1 blocking_read_pauses=1");
}

/* WrDec/RdDec with a latched reload, and the reload == 0 case. */
static void check_decrementer(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 4);
    interpreter_start(owner);
    REQUIRE(write_channel(owner, SPU_WrDec, 1000) == 0, "WrDec");
    REQUIRE(owner->decrementer_latched == 1 && owner->decrementer_reload == 1000, "WrDec latched=%u reload=%u",
            owner->decrementer_latched, owner->decrementer_reload);
    REQUIRE(read_channel(owner, SPU_RdDec) == 1000u, "RdDec after WrDec=%u", read_channel(owner, SPU_RdDec));
    REQUIRE(write_channel(owner, SPU_WrDec, 0) == 0, "WrDec 0");
    REQUIRE(read_channel(owner, SPU_RdDec) == 0u, "RdDec with a zero reload=%u", read_channel(owner, SPU_RdDec));
    Assembler a = { 0x3A00 };
    emit(owner, &a, enc_rdch(1, SPU_RdDec));
    emit(owner, &a, enc_rchcnt(2, SPU_RdDec));
    emit(owner, &a, enc_il(3, 0x44));
    emit(owner, &a, enc_wrch(3, SPU_WrDec));
    emit(owner, &a, enc_rdch(4, SPU_RdDec));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, 0x3A00, 100, &result) == 0, "WrDec/RdDec program");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED, "WrDec kind=%d", result.kind);
    REQUIRE(owner->core.gpr[1]._u32[0] == 0u && owner->core.gpr[2]._u32[0] == 1u, "RdDec in program r1=%08X r2=%08X",
            owner->core.gpr[1]._u32[0], owner->core.gpr[2]._u32[0]);
    REQUIRE(owner->core.gpr[4]._u32[0] == 0x44u, "WrDec 0x44 then RdDec=%08X", owner->core.gpr[4]._u32[0]);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_DECREMENTER WrDec=7 RdDec=8 latched_reload=1 zero_reload_returns_zero=1 rchcnt_ready=1");
}

/* SRR0 plain storage. */
static void check_srr0(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 5);
    interpreter_start(owner);
    Assembler a = { 0x3C00 };
    emit(owner, &a, enc_il(1, 0x1230));
    emit(owner, &a, enc_wrch(1, SPU_WrSRR0));
    emit(owner, &a, enc_rdch(2, SPU_RdSRR0));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, 0x3C00, 100, &result) == 0, "SRR0 program");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED && owner->core.gpr[2]._u32[0] == 0x1230u, "SRR0 in program r2=%08X",
            owner->core.gpr[2]._u32[0]);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_SRR0 WrSRR0=14 RdSRR0=15 plain_storage=1");
}

/* SPU_RdMachStat: bit 0 is the interrupt-enable state, which stays 0 because the interpreter never enables
 * interrupts (no E/D decoding, no interrupt delivery); the other bits and words are zero and the count is 1. */
enum { MACHSTAT_PC = 0x3E00 };
static void check_machstat(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 8);
    interpreter_start(owner);
    u128 status; memset(&status, 0xCC, sizeof status);
    REQUIRE(ps3_spu_executor_read_channel(owner, SPU_RdMachStat, &status) == 0, "machstat read");
    REQUIRE(status._u32[0] == 0u && status._u32[1] == 0u && status._u32[2] == 0u && status._u32[3] == 0u,
            "machstat=%08X %08X %08X %08X", status._u32[0], status._u32[1], status._u32[2], status._u32[3]);
    uint32_t count = 0;
    REQUIRE(ps3_spu_executor_count_channel(owner, SPU_RdMachStat, &count) == 0 && count == 1u, "machstat count=%u", count);
    Assembler a = { MACHSTAT_PC };
    emit(owner, &a, enc_rdch(1, SPU_RdMachStat));
    emit(owner, &a, enc_rchcnt(2, SPU_RdMachStat));
    emit(owner, &a, enc_stop(0x3FFF));
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, MACHSTAT_PC, 100, &result) == 0, "machstat program");
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED, "machstat kind=%d (rdch 13 must not pause)", result.kind);
    REQUIRE(owner->core.gpr[1]._u32[0] == 0u && owner->core.gpr[2]._u32[0] == 1u,
            "machstat in program r1=%08X r2=%08X", owner->core.gpr[1]._u32[0], owner->core.gpr[2]._u32[0]);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_MACHSTAT rdch=13 interrupt_enable=0 rchcnt=1 stops=1");
}

/* An unsupported channel names the channel number in the terminal. */
static void check_unsupported_channel(void) {
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 6);
    interpreter_start(owner);
    Assembler a = { 0x3D00 };
    emit(owner, &a, enc_rdch(1, SPU_RdSigNotify1));   /* channel 3 has no native behaviour */
    ps3_spu_interp_result result;
    REQUIRE(ps3_spu_executor_run_interp(owner, 0x3D00, 100, &result) == 0, "unsupported run");
    REQUIRE(result.kind == PS3_SPU_INTERP_FAULT, "unsupported kind=%d", result.kind);
    REQUIRE(result.code == PS3_SPU_EXEC_UNSUPPORTED, "unsupported code=%d", result.code);
    REQUIRE(result.channel == SPU_RdSigNotify1, "unsupported channel=%u", result.channel);
    interpreter_release(owner); fixture_close(&f);
    puts("CHANNEL_UNSUPPORTED channel=3 terminal_channel=3 code=UNSUPPORTED");
}

/* =============================================================================== 4. the real libsre SPURS kernel */
enum { SPURS_FIXTURE_THREADS = 1, SPURS_ELF_OFFSET = 0x20380, SPURS_ENTRY = 0x818, SPURS_INSTRUCTION_BUDGET = 50000000,
       CELL_SPURS_FIXTURE_EA = 0x200000, CELL_SPURS_FIXTURE_BYTES = 0x10000 };

static unsigned char *read_whole_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "KERNEL_IMAGE_OPEN_FAILED path=%s\n", path); exit(1); }
    if (fseek(file, 0, SEEK_END)) { fclose(file); fprintf(stderr, "KERNEL_IMAGE_SEEK_FAILED\n"); exit(1); }
    const long size = ftell(file);
    rewind(file);
    unsigned char *bytes = calloc(1, (size_t)size ? (size_t)size : 1);
    if (!bytes || size <= 0 || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); fprintf(stderr, "KERNEL_IMAGE_READ_FAILED\n"); exit(1);
    }
    fclose(file); *length = (size_t)size; return bytes;
}

static const char *kind_name(ps3_spu_interp_kind kind) {
    switch (kind) {
    case PS3_SPU_INTERP_STOPPED: return "Stopped";
    case PS3_SPU_INTERP_PAUSED: return "Paused";
    case PS3_SPU_INTERP_BUDGET: return "Budget";
    case PS3_SPU_INTERP_FAULT: return "Fault";
    }
    return "?";
}

static void run_spurs_kernel(const char *image_path) {
    size_t image_length = 0;
    unsigned char *image = read_whole_file(image_path, &image_length);
    if (image_length <= SPURS_ELF_OFFSET) { fprintf(stderr, "KERNEL_IMAGE_TOO_SMALL length=%zu\n", image_length); exit(1); }
    Fixture f; fixture_open(&f);
    ps3_lv2_spu_images images = { 0 };
    if (ps3_lv2_spu_images_init(&images, ps3_pthread_guard_port(&f.guard))) { fprintf(stderr, "IMAGES_INIT_FAILED\n"); exit(1); }
    ps3_lv2_spu_image parsed = { 0 };
    unsigned char segments[PS3_LV2_SPU_IMAGE_SEGMENTS_MAX * PS3_LV2_SPU_SEGMENT_BYTES];
    uint32_t segment_count = 0;
    const unsigned char *spurs_elf = image + SPURS_ELF_OFFSET;
    const size_t spurs_elf_length = image_length - SPURS_ELF_OFFSET;
    const uint32_t parse_status = ps3_lv2_spu_image_parse(&images, spurs_elf, spurs_elf_length,
                                                          &parsed, segments, sizeof segments, &segment_count);
    if (parse_status) { fprintf(stderr, "KERNEL_ELF_PARSE_FAILED status=%08X\n", parse_status); exit(1); }
    printf("SPU_KERNEL_IMAGE elf_offset=0x%05X entry=0x%03X parse_entry=0x%03X segments=%u bytes=%zu\n",
           SPURS_ELF_OFFSET, SPURS_ENTRY, parsed.entry_point, segment_count, image_length);

    unsigned char *local_stores = calloc(PS3_LV2_SPU_GROUP_CAP * PS3_LV2_SPU_THREADS_MAX, PS3_LV2_SPU_LS_BYTES);
    if (!local_stores) { fprintf(stderr, "KERNEL_LS_ALLOC_FAILED\n"); exit(1); }
    ps3_lv2_spu_groups groups = { 0 };
    if (ps3_lv2_spu_groups_init(&groups, ps3_pthread_guard_port(&f.guard), local_stores, 6)) {
        fprintf(stderr, "KERNEL_GROUPS_INIT_FAILED\n"); exit(1);
    }
    uint32_t group_id = 0, thread_id = 0;
    /* group_start requires EVERY thread slot initialised, and C1a runs exactly one thread: this fixture creates a
     * one-thread group holding the SPURS kernel. The game's five-thread group is C1b's business. */
    if (ps3_lv2_spu_group_create(&groups, &group_id, SPURS_FIXTURE_THREADS, 128, 0)) {
        fprintf(stderr, "KERNEL_GROUP_CREATE_FAILED\n"); exit(1); }
    /* A CellSpurs-sized zeroed region in guest memory for the kernel's DMAs. */
    if (ps3_atomic_zero(&f.domain, CELL_SPURS_FIXTURE_EA, CELL_SPURS_FIXTURE_BYTES)) {
        fprintf(stderr, "KERNEL_FIXTURE_ZERO_FAILED\n"); exit(1);
    }
    const uint64_t argument[4] = { 0, 0, 0, 0 };   /* C1b supplies the real CellSpurs argument */
    /* A COPY record's source offset is relative to the ELF it was parsed from (lv2_spu.c records ph.offset), so the
     * source buffer is the SPU ELF itself, not the whole libsre image. */
    if (ps3_lv2_spu_thread_initialize(&groups, &thread_id, group_id, 0, &parsed, segments, segment_count,
                                      spurs_elf, spurs_elf_length, argument)) {
        fprintf(stderr, "KERNEL_THREAD_INITIALIZE_FAILED\n"); exit(1);
    }
    if (ps3_lv2_spu_group_start(&groups, group_id)) { fprintf(stderr, "KERNEL_GROUP_START_FAILED\n"); exit(1); }

    ps3_spu_executor *owner = interpreter_owner(&f, 0);
    interpreter_start(owner);
    unsigned char *thread_ls = NULL;
    {
        ps3_lv2_spu_start_snapshot snapshot = { 0 };
        if (ps3_lv2_spu_group_snapshot(&groups, &snapshot) || !snapshot.valid) {
            fprintf(stderr, "KERNEL_SNAPSHOT_FAILED\n"); exit(1);
        }
        thread_ls = local_stores;   /* group 0, thread 0 */
        printf("SPU_KERNEL_SNAPSHOT group=0x%08X num=%u entry[0]=0x%03X arg1[0]=%llu ls_hash[0]=%016llX\n",
               snapshot.group, snapshot.num, snapshot.entry[0], (unsigned long long)snapshot.arg1[0],
               (unsigned long long)snapshot.ls_hash[0]);
    }
    memcpy(owner->core.ls, thread_ls, SPU_LS_SIZE);
    memset(owner->core.gpr, 0, sizeof owner->core.gpr);
    ps3_spu_interp_result result;
    const int status = ps3_spu_executor_run_interp(owner, SPURS_ENTRY, SPURS_INSTRUCTION_BUDGET, &result);
    printf("SPU_KERNEL_FIRST_STOP kind=%s pc=0x%05X code=0x%X channel=%u insns=%llu opcode=0x%08X stop=0x%X\n",
           kind_name(result.kind), result.pc, result.code, result.channel, (unsigned long long)result.instructions,
           result.opcode, result.stop_code);
    if (status) { fprintf(stderr, "KERNEL_RUN_REFUSED status=%d\n", status); }
    REQUIRE(status == 0, "kernel run status=%d", status);
    REQUIRE(result.kind == PS3_SPU_INTERP_STOPPED || result.kind == PS3_SPU_INTERP_PAUSED ||
            result.kind == PS3_SPU_INTERP_FAULT, "kernel kind=%d (a Budget stop means no stop was reached)", result.kind);
    printf("SPU_KERNEL_REGISTERS r1=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X stop_code=0x%X fault_kind=%s\n",
           owner->core.gpr[1]._u32[0], owner->core.gpr[3]._u32[0], owner->core.gpr[4]._u32[0],
           owner->core.gpr[5]._u32[0], owner->core.gpr[6]._u32[0], owner->core.stop_code,
           result.kind == PS3_SPU_INTERP_FAULT ? "fault" : "none");
    printf("SPU_KERNEL_STOP_WORD ls[0x%05X]=0x%08X r0=0x%08X r2=0x%08X r7=0x%08X r8=0x%08X\n", result.pc,
           spu_ls_read32(&owner->core, result.pc), owner->core.gpr[0]._u32[0], owner->core.gpr[2]._u32[0],
           owner->core.gpr[7]._u32[0], owner->core.gpr[8]._u32[0]);
    for (unsigned index = 0; index < FIXTURE_LOG_LIMIT; ++index)
        printf("SPU_KERNEL_OP %02u %s\n", index, index < f.logged ? f.operations[index] : "(none: the kernel stopped first)");
    interpreter_release(owner);
    free(local_stores); free(image);
    fixture_close(&f);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* progress lines must survive a hang */
    if (argc != 2) { fprintf(stderr, "usage: %s <libsre image.bin>\n", argv[0]); return 2; }
    run_case("arithmetic", ARITHMETIC_PC, 0x3FFF, build_arithmetic, check_arithmetic, 0, 1000);
    run_case("shifts", SHIFT_PC, 0x3FFF, build_shifts, check_shifts, 0, 1000);
    run_case("loads_stores", LOADSTORE_PC, 0x3FFF, build_loads_stores, check_loads_stores, 0, 1000);
    run_case("branches", BRANCH_PC, 0x3FFF, build_branches, check_branches, 0, 1000);
    run_case("vector", VECTOR_PC, 0x3FFF, build_vector, check_vector, 0, 1000);
    run_case("float", FLOAT_PC, 0x3FFF, build_float, check_float, 0, 1000);
    /* The resumable-stop case reaches stop 0x110 first and finishes at the second stop; check_stop verifies both. */
    run_case("stop", STOP_PC, 0x110, build_stop, check_stop, 0, 1000);
    check_halt_and_unsupported();
    check_budget();
    check_lifted_owner_refused();
    check_out_mailbox_pause();
    check_in_mailbox_pause();
    check_events();
    check_decrementer();
    check_srr0();
    check_machstat();
    check_unsupported_channel();
    run_spurs_kernel(argv[1]);
    if (failures) { printf("overall=FAIL failures=%d\n", failures); return 1; }
    puts("overall=PASS");
    return 0;
}
