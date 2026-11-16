/* Host differential test of the lifted SPURS kernel (Decision 133): the dynamic hybrid runner (ps3_spu_lifted_run_slice_dyn) with the kernel module of the user's own
 * libsre against the native INTERPRETER, on the same ps3_spu_executor, from the same local store and the same guest memory.
 *
 * usage: test_kernel_lifted <libsre_image.bin> <toy_pm.bin> [--trials N] [--seed S] [--verbose]
 *
 * Each trial builds a SPURS thread as the lv2 loader leaves it: the kernel's segment at LS 0x100, pc at the entry 0x818, r4 = the CellSpurs address. The CellSpurs
 * area in guest memory is random (so the kernel's workload selection takes a different path each time) except that every workload info entry (0xB00 + 32 * id and the
 * system service entry 0xD00) names the toy policy module (make_toy_pm.py) with a random argument and unique id. The toy module counts its calls, optionally calls the
 * kernel's select entry, and leaves through the kernel's exit entry until its call count passes a random limit, then stops. The interpreter runs the whole program; the
 * lifted side runs it under every arrangement of the dynamic layer that matters:
 *   lifted-kernel+pm   the kernel and the toy module lifted: composite image, ps3_lifted_pm_resolve arms the module, nested lifted select call, KERNEL_EXIT re-entries;
 *   lifted-kernel      the kernel lifted, the module unknown: every policy call escapes to the interpreter, the module's exit re-enters lifted kernel code;
 *   corrupt-kernel     one byte of the kernel's (never executed) code differs: the kernel must NOT be armed and the run is the interpreter's;
 *   pm-only            no kernel module in the set (the build before Decision 133): the module is armed at its entry by the interpreter.
 * End states compared: result (kind, pc, code), all registers, the whole local store, the whole guest memory, the DMA log, the mailboxes and the event state.
 * The counters the report lines print are checked against what the program did (calls, exits, arming). */
#include "../../adapters/spu_executor.h"
#include "../../adapters/guard_pthread.h"
#include "../../adapters/spu_image_account.h"
#include "../../tests/guest_space_host.h"
#include "spurs_kernel.h"
#include "wws_restore_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern const ps3_spu_lifted_module jobmod_toypm_eA00_module;

enum { SPACE_BYTES = 16 * 1024 * 1024, LOG_MAX = 1u << 16, KERNEL_ELF_OFFSET = 0x20380, KERNEL_VADDR = 0x100, KERNEL_BYTES = 0x780, KERNEL_ENTRY = 0x818,
       SPURS_EA = 0x100000, TOY_PM_EA = 0x200000, TOY_PM_LS = 0xA00, INFO_FIRST = 0xB00, INFO_SYSTEM = 0xD00, INFO_BYTES = 32, INFO_WORKLOADS = 16,
       COUNTER = 0x3F000, CALL_SELECT_FLAG = 0x3F020, LIMIT = 0x3F030, RELOAD_FLAG = 0x3F040, RESTORE_FLAG = 0x3F050, STUB_TAIL = 0x3F100, STUB_PC = 0x3FEC0, SAVE_AREA = 0x3F9C0, SAVE_AREA_END = 0x3FE80, STUB_WORDS = 80, STOP_DONE = 0x3FFF, CORRUPT_AT = 0x281, INTERP_CAP = 50000000, MAX_LIMIT = 40 };
typedef struct { uint32_t lsa, bytes, command; uint64_t ea; } DmaOp;
typedef struct {
    const char *label; int lifted;
    ElfTestHostSpace *space; ps3_guest_memory backing; ps3_pthread_guard guard; ps3_atomic_domain domain; ps3_atomic_owner_record records[8];
    ps3_spu_executor *owner;
    DmaOp *dma_log; unsigned dma_count; uint64_t clock_reads;
    ps3_spu_lifted_services services; ps3_spu_lifted_stats stats; ps3_spu_dyn dyn; ps3_spu_interp_result last;
    const ps3_spu_lifted_module *const *set; unsigned set_count;
} Run;
static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static int service_checkpoint(void *v, unsigned phase) { (void)v; (void)phase; return 0; }
static int service_access(void *v, uint64_t address, size_t bytes, int write) {
    (void)v; (void)write; return address < SPACE_BYTES && bytes <= SPACE_BYTES - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS; }
static int service_reserve(void *v, const ps3_spu_transfer *t) {
    Run *r = v; if (r->dma_count < LOG_MAX) r->dma_log[r->dma_count] = (DmaOp){ t->lsa, t->bytes, t->command, t->ea }; ++r->dma_count; return 0; }
static void service_finish(void *v, const ps3_spu_transfer *t, ps3_guest_memory_status s, int c, const void *b) { (void)v; (void)t; (void)s; (void)c; (void)b; }
static ps3_clock_status clock_now(void *context, uint64_t *out) { Run *r = context; *out = 1000u + 7u * r->clock_reads++; return PS3_CLOCK_OK; }
static uint64_t xorshift(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
static int lifted_wait(void *context, spu_context *ctx, uint32_t channel) { (void)context; (void)ctx; (void)channel; return 1; }   /* nobody serves a mailbox here */

static void owner_start(Run *r) {
    ps3_spu_executor_services services = { r, service_access, service_checkpoint, service_reserve, service_finish };
    memset(r->owner, 0, sizeof *r->owner);
    r->clock_reads = 0; r->dma_count = 0;
    memset(&r->dyn, 0, sizeof r->dyn); memset(&r->stats, 0, sizeof r->stats);
    if (ps3_spu_executor_init(r->owner, &r->domain, 0, services)) { fprintf(stderr, "OWNER_INIT_FAILED\n"); exit(1); }
    ps3_clock clock = { r, clock_now };
    if (r->lifted) {
        if (ps3_spu_lifted_dyn_start(r->owner, clock)) { fprintf(stderr, "DYN_START_FAILED\n"); exit(1); }
        r->dyn.modules = r->set; r->dyn.module_count = r->set_count; r->dyn.window_lo = 0x4000u;
        ps3_spu_acct_arm(r->owner, 1, 2, 3, 4);
    } else {
        ps3_spu_executor_set_interpreter(r->owner);
        ps3_spu_executor_start_config config = { 0 };
        config.image_id = -1; config.clock = clock; config.thread_option = PS3_SPU_THREAD_OPTION_NONE; config.hop_budget = 1;
        if (ps3_spu_executor_start(r->owner, &config)) { fprintf(stderr, "INTERP_START_FAILED\n"); exit(1); }
    }
}
static void run_open(Run *r, const char *label, int lifted, const ps3_spu_lifted_module *const *set, unsigned set_count) {
    memset(r, 0, sizeof *r);
    r->label = label; r->lifted = lifted; r->set = set; r->set_count = set_count;
    r->dma_log = calloc(LOG_MAX, sizeof *r->dma_log); r->owner = calloc(1, sizeof *r->owner); r->space = elf_test_host_space_create(SPACE_BYTES);
    if (!r->dma_log || !r->owner || !r->space || ps3_pthread_guard_init(&r->guard)) { fprintf(stderr, "OPEN_FAILED\n"); exit(1); }
    r->backing = elf_test_host_space_port(r->space);
    if (ps3_atomic_domain_init(&r->domain, r->backing, ps3_pthread_guard_port(&r->guard), r->records, 8, NULL, 0) ||
        ps3_atomic_commit(&r->domain, 0, SPACE_BYTES)) { fprintf(stderr, "DOMAIN_FAILED\n"); exit(1); }
    /* Decision 135: the lifted side's plain DMA copies directly where the domain allows it, the interpreter's stays guarded: the comparison then
     * also checks the direct DMA path against the guarded one (KERNEL_TEST_GUARDED_DMA=1: both guarded) */
    if (lifted && !getenv("KERNEL_TEST_GUARDED_DMA")) ps3_atomic_domain_set_direct(&r->domain, elf_test_host_space_direct, r->space);
    owner_start(r);
}
static void run_restart(Run *r) {
    (void)ps3_spu_executor_take_continuation(r->owner);
    if (r->owner->initialized && ps3_spu_executor_release(r->owner)) { fprintf(stderr, "OWNER_RELEASE_FAILED\n"); exit(1); }
    owner_start(r);
}

/* ------------------------------------------------------------------------------------------------------------ the fixture */
static unsigned char g_kernel[KERNEL_BYTES], g_toy_pm[0x100];
static size_t g_toy_pm_bytes;
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void put_be64(unsigned char *p, uint64_t v) { put_be32(p, (uint32_t)(v >> 32)); put_be32(p + 4, (uint32_t)v); }
static int load_inputs(const char *libsre_path, const char *toy_path) {
    FILE *f = fopen(libsre_path, "rb");
    if (!f) return 0;
    static unsigned char image[0x40000];
    const size_t n = fread(image, 1, sizeof image, f); fclose(f);
    const unsigned char *elf = image + KERNEL_ELF_OFFSET;
    if (n < KERNEL_ELF_OFFSET + 0x1000u || memcmp(elf, "\x7f" "ELF", 4) || be32(elf + 24) != KERNEL_ENTRY) return 0;
    const unsigned char *ph = elf + be32(elf + 28);           /* the first program header is the kernel's one PT_LOAD (gen_kernel_lifted.py checks the layout) */
    if (be32(ph) != 1u || be32(ph + 8) != KERNEL_VADDR || be32(ph + 16) != KERNEL_BYTES) return 0;
    memcpy(g_kernel, elf + be32(ph + 4), KERNEL_BYTES);
    f = fopen(toy_path, "rb");
    if (!f) return 0;
    g_toy_pm_bytes = fread(g_toy_pm, 1, sizeof g_toy_pm, f); fclose(f);
    return g_toy_pm_bytes >= 16 && !(g_toy_pm_bytes & 15u);
}
typedef struct { uint64_t seed; unsigned limit, call_select, reload, restore, corrupt; } Trial;   /* restore: 0 none, 1 the restore sequence intact, 2 its tail left broken */
/* The same guest memory and thread state on every run of a trial. */
static void setup_trial(Run *r, const Trial *trial) {
    run_restart(r);
    uint64_t s = trial->seed * 0x9E3779B97F4A7C15ull | 1u;
    static unsigned char chunk[65536];
    for (uint32_t base = 0; base < SPACE_BYTES; base += sizeof chunk) {
        if (base == 0 || (base >= SPURS_EA - sizeof chunk && base <= TOY_PM_EA)) {
            for (size_t i = 0; i < sizeof chunk; i += 8) { uint64_t v = xorshift(&s); memcpy(chunk + i, &v, 8); }
        } else memset(chunk, 0x5A, sizeof chunk);                    /* (only the windows the kernel can reach need fresh random bytes) */
        if (base <= SPURS_EA && SPURS_EA < base + sizeof chunk) {
            unsigned char *spurs = chunk + (SPURS_EA - base);
            for (unsigned id = 0; id <= INFO_WORKLOADS; ++id) {
                unsigned char *info = spurs + (id < INFO_WORKLOADS ? INFO_FIRST + id * INFO_BYTES : INFO_SYSTEM);
                put_be64(info, TOY_PM_EA);                           /* the policy module's address */
                put_be32(info + 16, (uint32_t)g_toy_pm_bytes);       /* its size (the DMA size of the kernel's GET) */
            }
        }
        if (base <= TOY_PM_EA && TOY_PM_EA < base + sizeof chunk) memcpy(chunk + (TOY_PM_EA - base), g_toy_pm, g_toy_pm_bytes);
        if (r->backing.write(r->backing.context, base, chunk, sizeof chunk)) { fprintf(stderr, "FILL_FAILED\n"); exit(1); }
    }
    spu_context *c = &r->owner->core;
    memset(c->ls, 0, SPU_LS_SIZE);
    memcpy(c->ls + KERNEL_VADDR, g_kernel, KERNEL_BYTES);
    if (trial->corrupt) c->ls[CORRUPT_AT] ^= 0x40u;
    put_be32(c->ls + CALL_SELECT_FLAG, trial->call_select);
    put_be32(c->ls + LIMIT, trial->limit);
    put_be32(c->ls + RELOAD_FLAG, trial->reload);
    if (trial->restore) {      /* Decision 134: pm_wwsjob's register restore sequence at 0x3FEC0 (built from its pattern), a random save area, the tail the module writes back */
        put_be32(c->ls + RESTORE_FLAG, 1u);
        for (unsigned i = 0; i < STUB_WORDS; ++i) put_be32(c->ls + STUB_PC + 4u * i, ps3_wws_restore_stub_word(i < 77u ? i : 76u));
        uint64_t r = trial->seed ^ 0x5EEDull;
        for (uint32_t a = SAVE_AREA; a < SAVE_AREA_END; a += 8) { const uint64_t v = xorshift(&r); memcpy(c->ls + a, &v, 8); }
        if (trial->restore == 1) memcpy(c->ls + STUB_TAIL, c->ls + 0x3FFB0, 80);   /* what the module writes back over the kernel's stack frame and workload info */
    }
    for (unsigned reg = 0; reg < 128; ++reg) c->gpr[reg] = (u128){ 0 };
    c->gpr[3]._u32[0] = 0;                                           /* the SPU's index in its group */
    c->gpr[4]._u32[0] = 0; c->gpr[4]._u32[1] = SPURS_EA;             /* the CellSpurs address, a u64 in the preferred doubleword */
    c->pc = KERNEL_ENTRY; c->status = SPU_STATUS_RUNNING; c->stop_code = 0;
}

/* ------------------------------------------------------------------------------------------------------------ running */
static int run_interp(Run *r, uint64_t *instructions) {
    *instructions = 0;
    for (;;) {
        ps3_spu_interp_result res;
        if (ps3_spu_executor_run_interp(r->owner, r->owner->core.pc, 100000, &res)) return -1;
        *instructions += res.instructions; r->last = res;
        if (res.kind != PS3_SPU_INTERP_BUDGET) return 0;
        if (*instructions >= INTERP_CAP) return 1;
    }
}
static int run_lifted(Run *r) {
    for (;;) {
        ps3_spu_interp_result res;
        r->services = (ps3_spu_lifted_services){ .context = r, .wait = lifted_wait };
        const int status = ps3_spu_lifted_run_slice_dyn(r->owner, &r->dyn, &r->services, 200000, &res, &r->stats);
        if (status != PS3_SPU_EXEC_OK) { fprintf(stderr, "%s: slice refused status=%d\n", r->label, status); return -1; }
        r->last = res;
        if (res.kind != PS3_SPU_INTERP_BUDGET) return 0;
    }
}
static unsigned compare_runs(Run *a, Run *b, const char *stage) {
    unsigned diffs = 0; const spu_context *ca = &a->owner->core, *cb = &b->owner->core;
    if (a->last.kind != b->last.kind || a->last.pc != b->last.pc || a->last.code != b->last.code) {
        printf("DIFF %s result: lifted kind=%d pc=0x%05X code=0x%X channel=%u | interp kind=%d pc=0x%05X code=0x%X channel=%u\n", stage,
               a->last.kind, a->last.pc, a->last.code, a->last.channel, b->last.kind, b->last.pc, b->last.code, b->last.channel); ++diffs; }
    unsigned reg_diffs = 0, shown = 0;
    for (unsigned reg = 0; reg < 128; ++reg) for (unsigned w = 0; w < 4; ++w)
        if (ca->gpr[reg]._u32[w] != cb->gpr[reg]._u32[w]) { ++reg_diffs; if (shown++ < 8) printf("DIFF %s r%u.w%u: lifted 0x%08X interp 0x%08X\n", stage, reg, w, ca->gpr[reg]._u32[w], cb->gpr[reg]._u32[w]); }
    if (reg_diffs) { printf("DIFF %s registers: %u words\n", stage, reg_diffs); ++diffs; }
    for (size_t i = 0; i < SPU_LS_SIZE; ++i)
        if (ca->ls[i] != cb->ls[i]) { printf("DIFF %s local store first at 0x%05zX\n", stage, i); ++diffs; break; }
    static unsigned char ga[65536], gb[65536];
    for (uint32_t base = 0; base < SPACE_BYTES; base += sizeof ga) {
        if (a->backing.read(a->backing.context, base, ga, sizeof ga) || b->backing.read(b->backing.context, base, gb, sizeof gb)) { printf("DIFF %s guest read\n", stage); ++diffs; break; }
        if (memcmp(ga, gb, sizeof ga)) { printf("DIFF %s guest memory in [0x%X,+64K)\n", stage, base); ++diffs; break; }
    }
    if (a->dma_count != b->dma_count) { printf("DIFF %s DMA count: lifted %u interp %u\n", stage, a->dma_count, b->dma_count); ++diffs; }
    else for (unsigned i = 0; i < a->dma_count && i < LOG_MAX; ++i)
        if (memcmp(&a->dma_log[i], &b->dma_log[i], sizeof a->dma_log[i])) { printf("DIFF %s DMA #%u\n", stage, i); ++diffs; break; }
    if (ca->ch_in_mbox.count != cb->ch_in_mbox.count || ca->ch_out_mbox.count != cb->ch_out_mbox.count || ca->ch_out_intr_mbox.count != cb->ch_out_intr_mbox.count ||
        ca->event_mask != cb->event_mask || ca->event_status != cb->event_status || ca->int_enable != cb->int_enable) { printf("DIFF %s mailboxes/events\n", stage); ++diffs; }
    return diffs;
}

typedef struct { const char *label; const ps3_spu_lifted_module *const *set; unsigned count; unsigned corrupt; } Arrangement;
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <libsre_image.bin> <toy_pm.bin> [--trials N] [--seed S] [--verbose]\n", argv[0]); return 2; }
    unsigned trials = 200, verbose = 0; uint64_t seed = 1;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!load_inputs(argv[1], argv[2])) { fprintf(stderr, "INPUTS_UNREADABLE (libsre image with the kernel ELF at 0x%X, toy module)\n", KERNEL_ELF_OFFSET); return 2; }
    alarm(1800);
    static const ps3_spu_lifted_module *const kernel_and_pm[] = { &ps3_spurs_kernel_module, &ps3_wws_restore_stub_module, &jobmod_toypm_eA00_module };
    static const ps3_spu_lifted_module *const kernel_only[] = { &ps3_spurs_kernel_module, &ps3_wws_restore_stub_module };
    static const ps3_spu_lifted_module *const pm_only[] = { &jobmod_toypm_eA00_module };
    const Arrangement arrangements[] = {
        { "lifted-kernel+pm", kernel_and_pm, 3, 0 }, { "lifted-kernel", kernel_only, 2, 0 }, { "corrupt-kernel", kernel_and_pm, 3, 1 }, { "pm-only", pm_only, 1, 0 } };
    enum { ARRANGEMENTS = sizeof arrangements / sizeof arrangements[0] };
    Run interp, lifted;
    run_open(&interp, "interp", 0, NULL, 0);
    run_open(&lifted, "lifted", 1, NULL, 0);
    unsigned compared[ARRANGEMENTS] = { 0 }, skipped = 0, bad = 0;
    uint64_t calls_total = 0, exits_total = 0, rearms_total = 0, interp_instr_total = 0, lifted_interp_total[ARRANGEMENTS] = { 0 };
    uint64_t s = seed;
    for (unsigned t = 0; t < trials; ++t) {
        Trial trial = { xorshift(&s), (unsigned)(xorshift(&s) % (MAX_LIMIT + 1)), (unsigned)(xorshift(&s) & 1u), (unsigned)(xorshift(&s) & 1u), (unsigned)(xorshift(&s) % 3u), 0 };
        for (unsigned a = 0; a < ARRANGEMENTS; ++a) {
            const Arrangement *arr = &arrangements[a];
            trial.corrupt = arr->corrupt;
            interp.set = NULL; setup_trial(&interp, &trial);
            uint64_t instructions = 0;
            const int capped = run_interp(&interp, &instructions);
            if (capped) {
                ++skipped;
                if (verbose) printf("SKIP trial %u (%s) limit=%u select=%u: interpreter %s at pc 0x%05X after %u policy calls\n", t, arr->label, trial.limit, trial.call_select,
                                    capped < 0 ? "refused" : "cap", interp.owner->core.pc, be32(interp.owner->core.ls + COUNTER));
                continue;
            }
            lifted.set = arr->set; lifted.set_count = arr->count; setup_trial(&lifted, &trial);
            if (run_lifted(&lifted)) { ++bad; printf("LIFTED_REFUSED trial %u %s\n", t, arr->label); continue; }
            char stage[96]; snprintf(stage, sizeof stage, "trial %u seed 0x%llx %s", t, (unsigned long long)trial.seed, arr->label);
            const unsigned diffs = compare_runs(&lifted, &interp, stage);
            const uint32_t calls = be32(interp.owner->core.ls + COUNTER);
            const ps3_spu_lifted_stats *st = &lifted.stats;
            ++compared[a]; interp_instr_total += instructions; lifted_interp_total[a] += st->interp_instructions;
            if (diffs) ++bad;
            /* what the counters must say about this program */
            CHECK(trial.restore == 2 || (interp.last.kind == PS3_SPU_INTERP_STOPPED && interp.last.code == STOP_DONE && calls == trial.limit + 1u),
                  "%s: the program did not end at its own stop (kind %d code 0x%X calls %u limit %u)", stage, interp.last.kind, interp.last.code, calls, trial.limit);
            if (trial.restore == 2) {
                /* the restore sequence's tail was left broken: it must stay on the interpreter (no expectation on the rest: the program ran into the bytes after it) */
            } else if (arr->corrupt) {
                CHECK(!st->kernel_armed && st->kernel_mismatch == 1 && !lifted.dyn.resident_kernel, "%s: a changed kernel was armed", stage);
                CHECK(!st->kernel_pm_calls && !st->kernel_exits, "%s: lifted kernel code ran", stage);
            } else if (arr->count == 1 && arr->set[0] == &jobmod_toypm_eA00_module) {
                CHECK(!st->kernel_armed && !st->kernel_mismatch && !st->kernel_exits, "%s: kernel counters without a kernel module", stage);
                CHECK(st->pm_armed >= 1, "%s: the policy module was never armed at its entry", stage);
            } else {
                CHECK(st->kernel_armed == 1 && lifted.dyn.resident_kernel == &ps3_spurs_kernel_module, "%s: the kernel was not armed", stage);
                CHECK(st->kernel_pm_calls == calls, "%s: %llu policy calls by lifted kernel code, the module counted %u", stage, (unsigned long long)st->kernel_pm_calls, calls);
                if (arr->count == 3 && trial.restore != 2) {
                    CHECK(st->kernel_exits == calls - 1u, "%s: %llu kernel exits, expected %u", stage, (unsigned long long)st->kernel_exits, calls - 1u);
                    CHECK(st->kernel_pm_armed >= 1 && st->escapes == 0 && st->interp_instructions == 0,
                          "%s: expected an all-lifted run (pm armed %llu, escapes %llu, interpreted %llu)", stage, (unsigned long long)st->kernel_pm_armed,
                          (unsigned long long)st->escapes, (unsigned long long)st->interp_instructions);
                    CHECK(!trial.reload || st->kernel_pm_armed == calls, "%s: the module was DMA'd again at every dispatch, armed %llu times for %u calls", stage,
                          (unsigned long long)st->kernel_pm_armed, calls);
                    calls_total += calls; exits_total += st->kernel_exits; rearms_total += st->kernel_pm_armed;
                } else if (arr->count == 2) {
                    CHECK(!st->kernel_pm_armed && st->kernel_exits == 0 && st->escapes >= calls, "%s: unknown module: pm armed %llu exits %llu escapes %llu (calls %u)", stage,
                          (unsigned long long)st->kernel_pm_armed, (unsigned long long)st->kernel_exits, (unsigned long long)st->escapes, calls);
                }
            }
            if (verbose || diffs) printf("TRIAL %u %-16s limit=%u select=%u reload=%u restore=%u calls=%u interp_instr=%llu lifted: enters=%llu escapes=%llu interp=%llu exits=%llu pm_calls=%llu diffs=%u\n",
                                         t, arr->label, trial.limit, trial.call_select, trial.reload, trial.restore, calls, (unsigned long long)instructions, (unsigned long long)st->enters,
                                         (unsigned long long)st->escapes, (unsigned long long)st->interp_instructions, (unsigned long long)st->kernel_exits,
                                         (unsigned long long)st->kernel_pm_calls, diffs);
        }
    }
    uint64_t totals[PS3_LIFTED_KERNEL_TOTALS];
    ps3_spu_lifted_kernel_totals(totals);
    printf("KERNEL_TEST trials=%u compared=%u/%u/%u/%u (lifted-kernel+pm, lifted-kernel, corrupt-kernel, pm-only) skipped=%u mismatched=%u failures=%d\n",
           trials, compared[0], compared[1], compared[2], compared[3], skipped, bad, failures);
    printf("KERNEL_TEST interp_instructions=%llu lifted_side_interp_instructions=%llu/%llu/%llu/%llu policy_calls_all_lifted=%llu kernel_exits=%llu pm_armed_by_kernel=%llu\n",
           (unsigned long long)interp_instr_total, (unsigned long long)lifted_interp_total[0], (unsigned long long)lifted_interp_total[1],
           (unsigned long long)lifted_interp_total[2], (unsigned long long)lifted_interp_total[3], (unsigned long long)calls_total, (unsigned long long)exits_total, (unsigned long long)rearms_total);
    printf("KERNEL_TEST totals armed=%llu mismatch=%llu pm_calls=%llu pm_armed=%llu exits=%llu disarms=%llu\n", (unsigned long long)totals[0], (unsigned long long)totals[1],
           (unsigned long long)totals[2], (unsigned long long)totals[3], (unsigned long long)totals[4], (unsigned long long)totals[5]);
    const int pass = !bad && !failures && compared[0] && compared[1] && compared[2] && compared[3];
    printf("overall=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
