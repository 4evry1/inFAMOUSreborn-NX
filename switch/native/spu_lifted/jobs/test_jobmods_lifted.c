/* Host differential test: the lifted inFAMOUS job modules (dynamic hybrid, spu_lifted_hybrid.c ps3_spu_lifted_run_slice_dyn) against the
 * native INTERPRETER, on the same ps3_spu_executor, from the same local store. Decision 104, 2026-11-13.
 *
 * usage: test_jobmods_lifted <spu dir (source/infamous-recomp/spu)> [options]
 *   --module NAME      test one module (prefix of its name accepted; default: every module compiled into the table)
 *   --fuzz N           trials per module: each makes the "policy module" stub call a genuine lifted function (a link-setting branch into the
 *                      job window, the exact way pm_wwsjob enters a job) with random registers and a randomised data area, $lr at a stop
 *                      sentinel. The interpreter runs first (instruction cap); only a run that ends at a stop is replayed through the
 *                      dynamic glue; the two end states (registers, local store, mailboxes, channel operations, DMA) must be identical.
 *   --trial ENTRY:SEED replay one fuzz trial of --module (as printed by FUZZ_MISMATCH)
 *   --fuzz-seed S      --fuzz-cap I (default 400000)   --fuzz-replay S (stable timing of the heavy trials, ~S seconds per module)
 *   --edge             the dispatch edge cases: a corrupted module is NOT lifted, a DMA GET over the code disarms, an interrupt pending at
 *                      the job call is delivered by the interpreter before any lifted code, a DMA over lifted code is a named terminal
 *   --irq-resume       an event arrives at the n-th channel operation of a run; the lifted run takes the interrupt (stop 0x77 vector), returns to srr0,
 *                      finishes, and must end exactly where an uninterrupted interpreter run ends (--fuzz N trials per module)
 *   --irq              smoke: with every event enabled and interrupts on, the lifted run still ends where the interpreter ends (stop class)
 *   --fpmode rz        run under MXCSR truncate + FTZ/DAZ (the SPU single-precision mode) instead of the host default
 *   --trace            with --trial: needs the module built with -DPS3_JOBMOD_TRACE (verify script: JOBMODS_TRACE=1): the first lifted transfer whose
 *                      register file differs from the interpreter's state at that pc
 *   --verbose
 * Inputs read in place: <spu dir>/jobmods/<name>.bin (the module bytes the policy module streams to LS 0x4000), jobmods_manifest.json
 * is not needed (the descriptors are compiled in). What this does NOT have: real job inputs (no capture of a running game exists on this
 * machine): the data a job reads is random, so what is proven is "same instruction semantics on the same state", not "same picture". */
#include "../../adapters/spu_executor.h"
#include "../../adapters/guard_pthread.h"
#include "../../adapters/spu_image_account.h"
#include "../../tests/guest_space_host.h"
#include "jobmods_table.h"
#ifdef PS3_SPU_NATIVE_INFLATE
#include "native_inflate.h"
#include <zlib.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <xmmintrin.h>

enum { SPACE_BYTES = 16 * 1024 * 1024, LOG_MAX = 1u << 18, STUB = 0x1000, SENTINEL = 0x3FFE0, STACK = 0x3F800, DATA_END = 0x3E000,
       STOP_AFTER_CALL = 0x3FFE, STOP_SENTINEL = 0x3FFF, STOP_IRQ = 0x77 };
typedef struct { uint32_t kind, channel; } ChannelOp;
typedef struct { uint32_t lsa, bytes, command; uint64_t ea; } DmaOp;
typedef struct Run {
    const char *label; int lifted;
    ElfTestHostSpace *space; ps3_guest_memory backing; ps3_pthread_guard guard; ps3_atomic_domain domain; ps3_atomic_owner_record records[8];
    ps3_spu_executor *owner;
    ChannelOp *channel_log; unsigned channel_count; DmaOp *dma_log; unsigned dma_count;
    unsigned checkpoints; uint64_t clock_reads, interp_instructions; unsigned idle_waits;
    int refuse_writes, fail_waits, guest_dirty;   /* guest_dirty: an MFC command ran since the guest window was last scanned (policy-module trials let DMA write it) */
    unsigned inject_after;                 /* --irq-resume: set an event (event_status bit 0) when the n-th channel operation begins */
    ps3_spu_lifted_services services; ps3_spu_lifted_stats stats; ps3_spu_dyn dyn; ps3_spu_interp_result last;
    const ps3_spu_lifted_module *const *set; unsigned set_count;
    uint64_t pm_pauses_seen;               /* policy-module runs: pm_pause_escapes already matched with the interpreter's retry of that channel operation (observe_channel) */
    uint64_t diag_seen[PS3_ESC_REASONS + 1];   /* PS3_SPU_ESCAPE_DIAG checks: stats already reconciled with the escape tables (by reason, then total) */
} Run;
static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static int service_checkpoint(void *v, unsigned phase) { (void)phase; ++((Run *)v)->checkpoints; return 0; }
static int service_access(void *v, uint64_t address, size_t bytes, int write) {
    if (write && ((Run *)v)->refuse_writes) return PS3_SPU_EXEC_ACCESS;
    return address < SPACE_BYTES && bytes <= SPACE_BYTES - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
static int service_reserve(void *v, const ps3_spu_transfer *t) {
    Run *r = v; if (r->dma_count < LOG_MAX) r->dma_log[r->dma_count] = (DmaOp){ t->lsa, t->bytes, t->command, t->ea }; ++r->dma_count; return 0; }
static void service_finish(void *v, const ps3_spu_transfer *t, ps3_guest_memory_status s, int c, const void *b) { (void)v; (void)t; (void)s; (void)c; (void)b; }
static void observe_channel(void *v, unsigned kind, uint32_t channel) {
    Run *r = v;
    /* Lifted policy code paused at a channel read and the interpreter retried that same instruction: the lifted channel code logged it already. */
    if (r->lifted && r->stats.pm_pause_escapes != r->pm_pauses_seen) { r->pm_pauses_seen = r->stats.pm_pause_escapes; return; }
    if (kind == PS3_SPU_OBS_WRITE_CHANNEL && channel == MFC_Cmd) r->guest_dirty = 1;
    if (r->channel_count < LOG_MAX) r->channel_log[r->channel_count] = (ChannelOp){ kind, channel }; ++r->channel_count;
    if (r->inject_after && r->channel_count == r->inject_after) r->owner->core.event_status |= 1u; }
static ps3_clock_status clock_now(void *context, uint64_t *out) { Run *r = context; *out = 1000u + 7u * r->clock_reads++; return PS3_CLOCK_OK; }
static uint64_t xorshift(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
static double seconds_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

static void owner_start(Run *r) {
    ps3_spu_executor_services services = { r, service_access, service_checkpoint, service_reserve, service_finish };
    memset(r->owner, 0, sizeof *r->owner);
    r->inject_after = 0;
    r->clock_reads = 0; r->checkpoints = 0; r->channel_count = r->dma_count = 0; r->idle_waits = 0;
    memset(&r->dyn, 0, sizeof r->dyn);
    if (ps3_spu_executor_init(r->owner, &r->domain, 0, services)) { fprintf(stderr, "OWNER_INIT_FAILED\n"); exit(1); }
    ps3_spu_executor_set_channel_observer(r->owner, observe_channel, r);
    ps3_clock clock = { r, clock_now };
    if (r->lifted) {
        if (ps3_spu_lifted_dyn_start(r->owner, clock)) { fprintf(stderr, "DYN_START_FAILED\n"); exit(1); }
        r->dyn.modules = r->set; r->dyn.module_count = r->set_count; r->dyn.window_lo = 0x4000u;
        ps3_spu_acct_arm(r->owner, 1, 2, 3, 4);        /* the region map the job call watch needs; slot numbers are arbitrary here */
    } else {
        ps3_spu_executor_set_interpreter(r->owner);
        ps3_spu_executor_start_config config = { 0 };
        config.image_id = -1; config.clock = clock; config.thread_option = PS3_SPU_THREAD_OPTION_NONE; config.hop_budget = 1;
        if (ps3_spu_executor_start(r->owner, &config)) { fprintf(stderr, "INTERP_START_FAILED\n"); exit(1); }
    }
}
static void owner_restart(Run *r) {
    (void)ps3_spu_executor_take_continuation(r->owner);
    if (r->owner->initialized && ps3_spu_executor_release(r->owner)) { fprintf(stderr, "OWNER_RELEASE_FAILED\n"); exit(1); }
    memset(&r->stats, 0, sizeof r->stats); memset(r->diag_seen, 0, sizeof r->diag_seen); r->pm_pauses_seen = 0;
    owner_start(r);
}
static void run_open(Run *r, const char *label, int lifted, const ps3_spu_lifted_module *const *set, unsigned set_count) {
    memset(r, 0, sizeof *r);
    r->label = label; r->lifted = lifted; r->set = set; r->set_count = set_count;
    r->channel_log = calloc(LOG_MAX, sizeof *r->channel_log); r->dma_log = calloc(LOG_MAX, sizeof *r->dma_log);
    r->owner = calloc(1, sizeof *r->owner); r->space = elf_test_host_space_create(SPACE_BYTES);
    if (!r->channel_log || !r->dma_log || !r->owner || !r->space || ps3_pthread_guard_init(&r->guard)) { fprintf(stderr, "OPEN_FAILED\n"); exit(1); }
    r->backing = elf_test_host_space_port(r->space);
    if (ps3_atomic_domain_init(&r->domain, r->backing, ps3_pthread_guard_port(&r->guard), r->records, 8, NULL, 0) ||
        ps3_atomic_commit(&r->domain, 0, SPACE_BYTES)) { fprintf(stderr, "DOMAIN_FAILED\n"); exit(1); }
    /* Decision 135: the lifted side's plain DMA copies directly where the domain allows it, the interpreter's stays guarded (JOBMODS_GUARDED_DMA=1: both guarded) */
    if (lifted && !getenv("JOBMODS_GUARDED_DMA")) ps3_atomic_domain_set_direct(&r->domain, elf_test_host_space_direct, r->space);
    uint64_t s = 0x1234567ull; static unsigned char chunk[65536];       /* the guest window is the same random bytes for every run */
    for (uint32_t base = 0; base < SPACE_BYTES; base += sizeof chunk) {
        for (size_t i = 0; i < sizeof chunk; i += 8) { uint64_t v = xorshift(&s); memcpy(chunk + i, &v, 8); }
        if (r->backing.write(r->backing.context, base, chunk, sizeof chunk)) { fprintf(stderr, "FILL_FAILED\n"); exit(1); }
    }
    owner_start(r);
}
static void run_close(Run *r) {
    (void)ps3_spu_executor_release(r->owner); free(r->owner); free(r->channel_log); free(r->dma_log);
    ps3_pthread_guard_destroy(&r->guard); elf_test_host_space_destroy(r->space);
}
static int lifted_wait(void *context, spu_context *ctx, uint32_t channel) { Run *r = context; (void)ctx; (void)channel; (void)r; return 1; }   /* nobody serves: end the owner */
static void lifted_intr_written(void *context, spu_context *ctx) { (void)context; (void)ctx; }

#ifdef PS3_SPU_ESCAPE_DIAG
/* ---- --escape diagnostics (round 2): the escape tables the dynamic runner keeps per job slot (reasons, heavy-hitter target pcs) are checked
 * against ground truth: the stats counters (every escape is counted once, by reason) and the exact per-pc histogram of
 * PS3_LIFTED_ESCAPE_HIST. Then the accounting report line is parsed and compared with the run totals. Needs both defines. */
#ifndef PS3_LIFTED_ESCAPE_HIST
#error PS3_SPU_ESCAPE_DIAG tests need PS3_LIFTED_ESCAPE_HIST (the exact histogram)
#endif
extern uint64_t g_ps3_lifted_escape_hist[];
static uint64_t g_diag_before[SPU_LS_SIZE / 4], g_diag_true[SPU_LS_SIZE / 4], g_diag_expected[PS3_ESC_REASONS];
static uint64_t g_diag_runs, g_diag_escapes, g_diag_hh_checks;
static void diag_before_run(void) { memcpy(g_diag_before, g_ps3_lifted_escape_hist, sizeof g_diag_before); }
static void diag_after_run(Run *r) {
    const uint64_t now[PS3_ESC_REASONS + 1] = { r->stats.esc_stub, r->stats.esc_norow, r->stats.esc_out, r->stats.irq_escapes, r->stats.esc_hybrid, r->stats.escapes };
    uint64_t local[PS3_ESC_REASONS] = { 0 }, total = 0, hist_total = 0;
    for (unsigned slot = 0; slot < PS3_SPU_ACCT_SLOTS; ++slot) {
        uint64_t slot_sum = 0;
        for (unsigned reason = 0; reason < PS3_ESC_REASONS; ++reason) { local[reason] += r->dyn.esc_n[slot][reason]; slot_sum += r->dyn.esc_n[slot][reason]; }
        CHECK(slot == 3 || !slot_sum, "%s: escapes filed under accounting slot %u (the job region's slot is 3)", r->label, slot);
    }
    for (unsigned reason = 0; reason < PS3_ESC_REASONS; ++reason) {
        CHECK(local[reason] == now[reason] - r->diag_seen[reason], "%s: reason %u: tables %llu, stats %llu", r->label, reason, (unsigned long long)local[reason],
              (unsigned long long)(now[reason] - r->diag_seen[reason]));
        total += local[reason]; g_diag_expected[reason] += local[reason]; r->diag_seen[reason] = now[reason];
    }
    CHECK(total == now[PS3_ESC_REASONS] - r->diag_seen[PS3_ESC_REASONS], "%s: escapes: tables %llu, stats %llu", r->label, (unsigned long long)total,
          (unsigned long long)(now[PS3_ESC_REASONS] - r->diag_seen[PS3_ESC_REASONS]));
    r->diag_seen[PS3_ESC_REASONS] = now[PS3_ESC_REASONS];
    CHECK(r->dyn.esc_dirty == (total != 0), "%s: esc_dirty %u with %llu escapes", r->label, r->dyn.esc_dirty, (unsigned long long)total);
    /* Misra-Gries against the exact histogram of THIS run: a table entry never exceeds the truth, and a pc with more than 2/7 of the escapes (two
     * reasons may split one pc) is present with an error of at most 2/7 of them */
    const uint64_t bound = 2u * total / (PS3_ESC_TOP + 1u);
    for (unsigned i = 0; i < SPU_LS_SIZE / 4; ++i) {
        const uint64_t f = g_ps3_lifted_escape_hist[i] - g_diag_before[i];
        if (!f) continue;
        hist_total += f; g_diag_true[i] += f;
        uint64_t held = 0;
        for (unsigned k = 0; k < PS3_ESC_TOP; ++k) if (r->dyn.esc_hits[3][k] && (r->dyn.esc_key[3][k] & 0xFFFFFFu) == i * 4u) held += r->dyn.esc_hits[3][k];
        CHECK(held <= f, "%s: table holds %llu hits for pc 0x%05X, truth %llu", r->label, (unsigned long long)held, i * 4u, (unsigned long long)f);
        if (f > bound) { ++g_diag_hh_checks; CHECK(held && f - held <= bound, "%s: heavy pc 0x%05X (%llu of %llu): table holds %llu", r->label, i * 4u, (unsigned long long)f,
                                                  (unsigned long long)total, (unsigned long long)held); }
    }
    CHECK(hist_total == total, "%s: histogram %llu escapes, tables %llu", r->label, (unsigned long long)hist_total, (unsigned long long)total);
    ps3_spu_acct_flush_escapes(&r->dyn);
    CHECK(!r->dyn.esc_dirty, "%s: esc_dirty after flush", r->label);
    ++g_diag_runs; g_diag_escapes += total;
}
static char g_diag_line[1024];
static void diag_capture(const char *line) { if (strstr(line, "kind=job") && strstr(line, "key=")) snprintf(g_diag_line, sizeof g_diag_line, "%s", line); }
static void diag_verify(const ps3_spu_lifted_module *const *modules, unsigned count) {
    /* the classifier against an independent linear definition, for every pc, for every module */
    for (unsigned mi = 0; mi < count; ++mi) {
        const ps3_spu_lifted_module *m = modules[mi]; unsigned stubs = 0, bad = 0;
        for (uint32_t pc = 0; pc < SPU_LS_SIZE; pc += 4) {
            unsigned want = PS3_ESC_OUT;
            if (pc >= m->code_begin && pc < m->code_end) {
                want = PS3_ESC_NOROW;
                for (unsigned i = 0; i < m->row_count; ++i) if (m->rows[i].pc == pc && !(m->rows[i].flags & PS3_LIFTED_ROW_GENUINE)) want = PS3_ESC_STUB;
            }
            stubs += want == PS3_ESC_STUB;
            if (ps3_spu_lifted_classify_escape(m, pc) != want) ++bad;
        }
        CHECK(!bad, "%s: %u pcs classified differently from the linear definition", m->name, bad);
        printf("ESC_CLASSIFY %-28s stub_rows=%u mismatched=%u\n", m->name, stubs, bad);
    }
    g_diag_line[0] = 0;
    ps3_spu_images_report(diag_capture, 0);
    printf("ESC_REPORT %s\n", g_diag_line[0] ? g_diag_line : "(no job line)");
    CHECK(g_diag_runs == 0 || g_diag_line[0], "no job line in the accounting report although %llu runs filed escapes", (unsigned long long)g_diag_runs);
    if (!g_diag_line[0]) return;
    unsigned long long tot = 0, c[PS3_ESC_REASONS] = { 0 };
    const char *p = strstr(g_diag_line, " esc=");
    if (!p) { CHECK(g_diag_escapes == 0, "report has no esc= although %llu escapes were filed", (unsigned long long)g_diag_escapes); return; }
    CHECK(sscanf(p, " esc=%llu(stub:%llu,norow:%llu,out:%llu,irq:%llu,hybrid:%llu)", &tot, &c[0], &c[1], &c[2], &c[3], &c[4]) == 6, "cannot parse the esc= field: %s", p);
    CHECK(tot == g_diag_escapes, "report esc=%llu, filed %llu", tot, (unsigned long long)g_diag_escapes);
    for (unsigned reason = 0; reason < PS3_ESC_REASONS; ++reason) CHECK(c[reason] == g_diag_expected[reason], "report reason %u: %llu, filed %llu", reason, c[reason], (unsigned long long)g_diag_expected[reason]);
    p = strstr(g_diag_line, "esc_top=");
    unsigned shown = 0; uint64_t previous = UINT64_MAX;
    for (p = p ? p + 8 : NULL; p && *p && shown < 4; ++shown) {
        unsigned pc, hits; char name[16]; int used = 0;
        if (sscanf(p, "0x%x/%15[a-z]:%u%n", &pc, name, &hits, &used) != 3) break;
        CHECK(pc < SPU_LS_SIZE && hits <= g_diag_true[pc >> 2], "report top pc 0x%05X hits %u, truth %llu", pc, hits, (unsigned long long)g_diag_true[pc >> 2]);
        CHECK(hits <= previous, "report top list not sorted"); previous = hits;
        p += used; if (*p == ',') ++p; else break;
    }
    CHECK(shown >= 1 && shown <= 3, "report esc_top has %u entries", shown);
    printf("ESC_DIAG runs=%llu escapes=%llu heavy_pc_checks=%llu report_top_entries=%u\n", (unsigned long long)g_diag_runs, (unsigned long long)g_diag_escapes, (unsigned long long)g_diag_hh_checks, shown);
}
#endif

/* ----------------------------------------------------------------------------------------------------- the PPU-less fixture */
static uint32_t put32(unsigned char *ls, uint32_t at, uint32_t word) { ls[at] = (unsigned char)(word >> 24); ls[at + 1] = (unsigned char)(word >> 16); ls[at + 2] = (unsigned char)(word >> 8); ls[at + 3] = (unsigned char)word; return at + 4; }
/* `bisl $0,$3` (RR form, op 0x1A9) with the E bit when enable_irq; then `stop 0x3FFE`. The job returns to the stop through $0. */
static void put_stub(unsigned char *ls, int enable_irq) {
    put32(ls, STUB, 0x35200180u | (enable_irq ? 0x00040000u : 0u));
    put32(ls, STUB + 4, STOP_AFTER_CALL);
    put32(ls, SENTINEL, STOP_SENTINEL);
}
/* A fresh local store: zeros, the module at its load address (LS 0x4000 for a job, 0x3000 for the Edge zlib task image), the stub. */
static void make_pristine(unsigned char *ls, const unsigned char *image, size_t image_bytes, uint32_t load) {
    memset(ls, 0, SPU_LS_SIZE);
    memcpy(ls + load, image, image_bytes);
    put_stub(ls, 0);
}
static void setup_trial(Run *r, const unsigned char *pristine, uint32_t entry, uint64_t seed, uint32_t data_begin, int irq_mode) {
    owner_restart(r);
    r->refuse_writes = 1; r->fail_waits = 1;
    spu_context *c = &r->owner->core;
    memcpy(c->ls, pristine, SPU_LS_SIZE);
    uint64_t s = seed * 0x9E3779B97F4A7C15ull | 1u;
    for (uint32_t a = data_begin; a < DATA_END; a += 8) { uint64_t v = xorshift(&s); memcpy(c->ls + a, &v, 8); }
    for (unsigned reg = 0; reg < 128; ++reg)
        for (unsigned w = 0; w < 4; ++w) {
            const uint64_t v = xorshift(&s); uint32_t word;
            switch (v & 7u) {
            case 0: word = (uint32_t)(v >> 8) & 0xFFu; break;
            case 1: case 2: word = data_begin + (((uint32_t)(v >> 8) % (DATA_END - data_begin - 0x1000u)) & ~0xFu); break;   /* LS pointer */
            case 3: word = (((uint32_t)(v >> 8)) % (SPACE_BYTES - 0x20000u)) & ~0xFu; break;                                   /* EA in the window */
            case 4: word = (uint32_t)(v >> 8); break;
            default: word = 0; break;
            }
            c->gpr[reg]._u32[w] = word;
        }
    c->gpr[0] = (u128){ 0 }; c->gpr[0]._u32[0] = SENTINEL;
    c->gpr[1] = (u128){ 0 }; c->gpr[1]._u32[0] = STACK;
    c->gpr[3] = (u128){ 0 }; c->gpr[3]._u32[0] = entry;                       /* the branch target of the stub */
    for (unsigned k = 0; k < 4; ++k) spu_channel_write(&c->ch_in_mbox, (uint32_t)xorshift(&s));
    if (irq_mode) {
        put32(c->ls, 0, STOP_IRQ);                                             /* the interrupt vector: stop 0x77 */
        put_stub(c->ls, 1);
        c->event_mask = 1u; c->event_status = 0;          /* only the event the test injects (bit 0) may interrupt: a natural DMA event would arrive at a different pc on each side */
    }
    c->pc = STUB; c->status = SPU_STATUS_RUNNING; c->stop_code = 0;
}
/* ----------------------------------------------------------------------------------------------------------- running */
static int run_interp_to_end(Run *r, uint64_t cap, uint64_t *instr, int *kind) {
    *instr = 0; *kind = -1;
    for (;;) {
        ps3_spu_interp_result res;
        if (ps3_spu_executor_run_interp(r->owner, r->owner->core.pc, 100000, &res)) { *kind = 9; return 1; }
        *instr += res.instructions; r->last = res;
        if (res.kind == PS3_SPU_INTERP_STOPPED) { *kind = 0; return 0; }
        if (res.kind == PS3_SPU_INTERP_FAULT) { *kind = 1; return 0; }
        if (res.kind == PS3_SPU_INTERP_PAUSED) { *kind = 2; return 0; }
        if (*instr >= cap) { *kind = 3; return 0; }
    }
}
static int run_lifted_to_end(Run *r) {
#ifdef PS3_SPU_ESCAPE_DIAG
    diag_before_run();
#endif
    for (;;) {
        ps3_spu_interp_result res;
        r->services = (ps3_spu_lifted_services){ .context = r, .wait = lifted_wait, .intr_written = lifted_intr_written };
        int status = ps3_spu_lifted_run_slice_dyn(r->owner, &r->dyn, &r->services, 200000, &res, &r->stats);
        if (status != PS3_SPU_EXEC_OK) { fprintf(stderr, "%s: slice refused status=%d\n", r->label, status); return status ? status : 1; }
        r->last = res;
        if (res.kind == PS3_SPU_INTERP_BUDGET) continue;
#ifdef PS3_SPU_ESCAPE_DIAG
        diag_after_run(r);
#endif
        return 0;     /* STOPPED, PAUSED (a wait nobody can serve) or FAULT: all end the trial */
    }
}
enum { CMP_QUIET = 4 };
static unsigned compare_runs(Run *a, Run *b, const char *stage, int flags, int with_cpu) {
    unsigned diffs = 0; const spu_context *ca = &a->owner->core, *cb = &b->owner->core;
    if (a->last.kind != b->last.kind || a->last.pc != b->last.pc || a->last.code != b->last.code ||
        (a->last.kind == PS3_SPU_INTERP_FAULT && (a->last.channel != b->last.channel || a->last.stop_code != b->last.stop_code || a->last.opcode != b->last.opcode))) {
        printf("DIFF %s result: lifted kind=%d pc=0x%05X code=0x%X channel=%u | interp kind=%d pc=0x%05X code=0x%X channel=%u\n", stage,
               a->last.kind, a->last.pc, a->last.code, a->last.channel, b->last.kind, b->last.pc, b->last.code, b->last.channel); ++diffs; }
    if (with_cpu) {
        if ((ca->pc & SPU_LS_MASK) != (cb->pc & SPU_LS_MASK)) { printf("DIFF %s pc: lifted 0x%05X interp 0x%05X\n", stage, ca->pc, cb->pc); ++diffs; }
        unsigned shown = 0, reg_diffs = 0;
        for (unsigned reg = 0; reg < 128; ++reg) for (unsigned w = 0; w < 4; ++w)
            if (ca->gpr[reg]._u32[w] != cb->gpr[reg]._u32[w]) { ++reg_diffs; if (shown++ < 12) printf("DIFF %s r%u.w%u: lifted 0x%08X interp 0x%08X\n", stage, reg, w, ca->gpr[reg]._u32[w], cb->gpr[reg]._u32[w]); }
        if (reg_diffs) { printf("DIFF %s registers: %u words differ\n", stage, reg_diffs); ++diffs; }
        size_t ls_diff = 0; unsigned ranges = 0;
        for (size_t i = 0; i < SPU_LS_SIZE;) {
            if (ca->ls[i] == cb->ls[i]) { ++i; continue; }
            size_t start = i; while (i < SPU_LS_SIZE && ca->ls[i] != cb->ls[i]) ++i;
            ls_diff += i - start; if (ranges++ < 8) printf("DIFF %s local store [0x%05zX,0x%05zX) %zu bytes\n", stage, start, i, i - start); }
        if (ls_diff) { printf("DIFF %s local store: %zu bytes in %u ranges differ\n", stage, ls_diff, ranges); ++diffs; }
        if (ca->ch_in_mbox.count != cb->ch_in_mbox.count || ca->ch_out_mbox.count != cb->ch_out_mbox.count || ca->ch_out_intr_mbox.count != cb->ch_out_intr_mbox.count) {
            printf("DIFF %s mailboxes\n", stage); ++diffs; }
        if (ca->event_mask != cb->event_mask || ca->event_status != cb->event_status || ca->int_enable != cb->int_enable || ca->srr0 != cb->srr0) {
            printf("DIFF %s events: mask %X/%X status %X/%X int_enable %u/%u srr0 %X/%X\n", stage, ca->event_mask, cb->event_mask, ca->event_status, cb->event_status, ca->int_enable, cb->int_enable, ca->srr0, cb->srr0); ++diffs; }
    }
    if (!(flags & 8)) {
        if (a->channel_count != b->channel_count) { printf("DIFF %s channel ops: lifted %u interp %u\n", stage, a->channel_count, b->channel_count); ++diffs; }
        for (unsigned i = 0; i < a->channel_count && i < b->channel_count && i < LOG_MAX; ++i)
            if (a->channel_log[i].kind != b->channel_log[i].kind || a->channel_log[i].channel != b->channel_log[i].channel) { printf("DIFF %s channel op #%u\n", stage, i); ++diffs; break; }
    }
    if (a->dma_count != b->dma_count) { printf("DIFF %s DMA count: lifted %u interp %u\n", stage, a->dma_count, b->dma_count); ++diffs; }
    for (unsigned i = 0; i < a->dma_count && i < b->dma_count && i < LOG_MAX; ++i)
        if (memcmp(&a->dma_log[i], &b->dma_log[i], sizeof(DmaOp))) { printf("DIFF %s DMA #%u\n", stage, i); ++diffs; break; }
    return diffs;
}

typedef struct { uint64_t trials, compared, mismatched, skip_budget, skip_fault, skip_paused, skip_error, skip_selfmod, skip_pm_ls0, known_ls0, known_drain,
                 instr_total, ended_sentinel, ended_call_stop, ended_other, entries_compared, armed_trials, heavy, irq_escapes, escapes, enters; double t_lifted, t_interp; } Stats;
typedef struct { uint32_t entry; uint64_t seed, instr; } Heavy;
/* ---- --trace: where do the lifted run and the interpreter first disagree? The lifted code (built with -DPS3_JOBMOD_TRACE) reports the
 * full register file at every direct transfer; the interpreter is single-stepped from the same state. Each lifted event must match the
 * interpreter's state when it next reaches that pc. */
typedef struct { uint32_t pc; u128 gpr[128]; } TraceEvent;
static TraceEvent *g_trace; static unsigned g_trace_n, g_trace_cap; static int g_trace_on;
static uint32_t g_cov[SPU_LS_SIZE / 4];      /* function entries reached by lifted code (PS3_JOBMOD_TRACE builds): the coverage of the policy-module fuzz */
void ps3_jobmod_trace_entry(spu_context *ctx, uint32_t pc) {
    ++g_cov[(pc & SPU_LS_MASK) >> 2];
    if (!g_trace_on) return;
    if (g_trace_n >= g_trace_cap + 20000u) ps3_spu_execution_fail(ctx, PS3_SPU_EXEC_CANCELLED);     /* a lifted run that never ends: enough events were recorded, stop it (the comparison below uses the first g_trace_cap) */
    if (g_trace_n < g_trace_cap) { g_trace[g_trace_n].pc = pc; memcpy(g_trace[g_trace_n].gpr, ctx->gpr, sizeof ctx->gpr); }
    ++g_trace_n;
}
static void (*g_trace_setup)(Run *);          /* the policy-module replay sets its own fixture */
static void trace_trial(Run *L, Run *I, const unsigned char *pristine, uint32_t entry, uint64_t seed, uint32_t data_begin, unsigned steps) {
    const unsigned cap = 40000; g_trace = malloc(sizeof(TraceEvent) * cap); g_trace_cap = cap; g_trace_n = 0;
    typedef struct { uint32_t pc; u128 gpr[128]; } Step;
    Step *ref = malloc(sizeof(Step) * steps); unsigned n = 0;
    if (g_trace_setup) g_trace_setup(I); else setup_trial(I, pristine, entry, seed, data_begin, 0);
    while (n < steps) {
        ref[n].pc = I->owner->core.pc & SPU_LS_MASK; memcpy(ref[n].gpr, I->owner->core.gpr, sizeof ref[n].gpr); ++n;
        ps3_spu_interp_result r; if (ps3_spu_executor_run_interp(I->owner, I->owner->core.pc, 1, &r) || r.kind != PS3_SPU_INTERP_BUDGET) break;
    }
    printf("TRACE interpreter steps=%u\n", n);
    if (g_trace_setup) g_trace_setup(L); else setup_trial(L, pristine, entry, seed, data_begin, 0);
    g_trace_on = 1; (void)run_lifted_to_end(L); g_trace_on = 0;
    printf("TRACE lifted events=%u (cap %u)\n", g_trace_n, cap);
    unsigned cursor = 0, bad = 0;
    for (unsigned e = 0; e < g_trace_n && e < cap && !bad; ++e) {
        unsigned j = cursor; while (j < n && ref[j].pc != g_trace[e].pc) ++j;
        if (j >= n) { printf("TRACE event %u: lifted transferred to 0x%05X, which the interpreter never reached after step %u (previous event 0x%05X)\n", e, g_trace[e].pc, cursor, e ? g_trace[e - 1].pc : 0u); bad = 1; break; }
        unsigned shown = 0;
        for (unsigned reg = 0; reg < 128; ++reg) for (unsigned w = 0; w < 4; ++w)
            if (g_trace[e].gpr[reg]._u32[w] != ref[j].gpr[reg]._u32[w]) {
                if (!shown++) printf("TRACE event %u at 0x%05X (interpreter step %u, previous lifted event 0x%05X) registers differ:\n", e, g_trace[e].pc, j, e ? g_trace[e - 1].pc : 0u);
                if (shown <= 16) printf("   r%u.w%u lifted 0x%08X interp 0x%08X\n", reg, w, g_trace[e].gpr[reg]._u32[w], ref[j].gpr[reg]._u32[w]);
                bad = 1; }
        cursor = j + 1;
    }
    if (!bad) printf("TRACE no register difference at any of the %u lifted transfer events\n", g_trace_n);
    free(g_trace); g_trace = NULL; free(ref);
}
static char g_current[160];
static uint32_t g_replay_entry; static uint64_t g_replay_seed;
static void on_alarm(int sig) { (void)sig; static const char p[] = "\nJOBMOD_FUZZ_HANG a lifted run did not finish in time: "; (void)!write(1, p, sizeof p - 1); (void)!write(1, g_current, strlen(g_current)); (void)!write(1, "\n", 1); _exit(3); }

/* One module: fuzz trials. Returns the mismatch count. */
static int fuzz_module(Run *L, Run *I, const ps3_spu_lifted_module *m, const unsigned char *pristine, uint32_t data_begin, uint64_t trials,
                       uint64_t seed, uint64_t cap, int irq_mode, int verbose, double replay_seconds, Stats *total) {
    Stats st; memset(&st, 0, sizeof st);
    unsigned *genuine = malloc(sizeof(unsigned) * m->row_count); unsigned genuine_count = 0;
    for (unsigned i = 0; i < m->row_count; ++i)   /* entries inside the verified code range only: beyond it the module holds data the lifter decoded as code */
        if (m->rows[i].flags == PS3_LIFTED_ROW_GENUINE && m->rows[i].pc >= m->code_begin + 0x30u && m->rows[i].pc >= 0x4000u && m->rows[i].pc < m->code_end) genuine[genuine_count++] = i;
    unsigned char *seen = calloc(genuine_count ? genuine_count : 1, 1);
    enum { HEAVY_KEEP = 24, HEAVY_MIN = 20000 }; Heavy heavy[HEAVY_KEEP]; unsigned heavy_count = 0;
    for (uint64_t trial = 0; trial < trials && st.mismatched < 3 && genuine_count; ++trial) {
        unsigned gi = (unsigned)(trial % genuine_count);
        uint32_t entry = m->rows[genuine[gi]].pc;
        uint64_t trial_seed = seed + trial * 0x1000193u + 1u;
        if (g_replay_entry) {          /* --trial ENTRY:SEED: exactly that trial, once */
            if (trial) break;
            entry = g_replay_entry; trial_seed = g_replay_seed; gi = 0;
            for (unsigned k = 0; k < genuine_count; ++k) if (m->rows[genuine[k]].pc == entry) gi = k;
        }
        ++st.trials;
        setup_trial(I, pristine, entry, trial_seed, data_begin, irq_mode);
        const double ti0 = seconds_now(); uint64_t instr; int kind;
        run_interp_to_end(I, cap, &instr, &kind);
        const double ti = seconds_now() - ti0;
        if (kind == 0) {
            if (memcmp(I->owner->core.ls + m->code_begin, pristine + m->code_begin, m->code_end - m->code_begin)) { ++st.skip_selfmod; continue; }
        }
        if (kind != 0) { if (kind == 1) ++st.skip_fault; else if (kind == 2) ++st.skip_paused; else if (kind == 3) ++st.skip_budget; else ++st.skip_error; continue; }
        setup_trial(L, pristine, entry, trial_seed, data_begin, irq_mode);
        snprintf(g_current, sizeof g_current, "%s entry 0x%05X seed %llu interp_instructions %llu end pc=0x%05X stop=0x%X", m->name, entry, (unsigned long long)trial_seed, (unsigned long long)instr, I->last.pc, I->last.code);
        signal(SIGALRM, on_alarm); alarm(60);
        const double tl0 = seconds_now(); const int rc = run_lifted_to_end(L); const double tl = seconds_now() - tl0; alarm(0);
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED &&
            (L->last.channel == PS3_SPU_EXEC_OP_INDIRECT_BRANCH || L->last.channel == PS3_SPU_EXEC_OP_CONTINUATION) &&
            ((L->owner->core.pc & 3u) || !(L->owner->core.pc & SPU_LS_MASK))) { ++st.known_ls0; continue; }
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED && L->last.channel == PS3_SPU_EXEC_OP_DRAIN_CALL) { ++st.known_drain; continue; }
        { /* accounting arithmetic: every interpreted instruction of the lifted run was attributed to exactly one region slot */
            uint64_t sum = 0; for (unsigned k = 0; k < PS3_SPU_ACCT_SLOTS; ++k) sum += L->owner->acct_instr[k];
            if (!rc && sum != L->stats.interp_instructions) { printf("DIFF %s accounting: attributed %llu interpreted %llu\n", m->name, (unsigned long long)sum, (unsigned long long)L->stats.interp_instructions); ++st.mismatched; continue; }
        }
        char label[120]; snprintf(label, sizeof label, "%s fuzz(entry 0x%05X seed %llu)", m->name, entry, (unsigned long long)trial_seed);
        unsigned diffs;
        if (irq_mode) {   /* only the stop class: interrupt delivery points differ between a block boundary and an instruction */
            diffs = (L->last.kind == I->last.kind && L->last.code == I->last.code) ? 0u : 1u;
            if (diffs) printf("DIFF %s irq outcome: lifted kind=%d code=0x%X pc=0x%05X | interp kind=%d code=0x%X pc=0x%05X\n", label, L->last.kind, L->last.code, L->last.pc, I->last.kind, I->last.code, I->last.pc);
        } else diffs = rc ? 1u : compare_runs(L, I, label, CMP_QUIET, 1);
        if (rc) printf("DIFF %s lifted run failed rc=%d\n", label, rc);
        ++st.compared; st.instr_total += instr; st.t_lifted += tl; st.t_interp += ti;
        if (L->stats.job_armed) ++st.armed_trials;
        st.irq_escapes += L->stats.irq_escapes; st.escapes += L->stats.escapes; st.enters += L->stats.enters;
        if (I->last.code == STOP_SENTINEL) ++st.ended_sentinel; else if (I->last.code == STOP_AFTER_CALL) ++st.ended_call_stop; else ++st.ended_other;
        if (!seen[gi]) { seen[gi] = 1; ++st.entries_compared; }
        if (instr >= HEAVY_MIN && !diffs && heavy_count < HEAVY_KEEP && !irq_mode) heavy[heavy_count++] = (Heavy){ entry, trial_seed, instr };
        if (instr >= HEAVY_MIN) ++st.heavy;
        if (diffs) { ++st.mismatched; printf("FUZZ_MISMATCH %s entry=0x%05X seed=%llu interp_instructions=%llu\n", m->name, entry, (unsigned long long)trial_seed, (unsigned long long)instr); }
        else if (verbose) printf("FUZZ_OK %s entry=0x%05X instr=%llu end=0x%X\n", m->name, entry, (unsigned long long)instr, I->last.code);
    }
    printf("FUZZ %-28s trials=%llu compared=%llu mismatched=%llu armed=%llu skipped(budget=%llu fault=%llu paused=%llu error=%llu code_written=%llu) known_divergence(ls0=%llu drain=%llu) "
           "entries=%llu/%u ended(sentinel=%llu call_stop=%llu other=%llu) instr=%llu speed=%.2fx lifted(enters=%llu escapes=%llu irq_escapes=%llu)\n", m->name, (unsigned long long)st.trials, (unsigned long long)st.compared,
           (unsigned long long)st.mismatched, (unsigned long long)st.armed_trials, (unsigned long long)st.skip_budget, (unsigned long long)st.skip_fault, (unsigned long long)st.skip_paused,
           (unsigned long long)st.skip_error, (unsigned long long)st.skip_selfmod, (unsigned long long)st.known_ls0, (unsigned long long)st.known_drain,
           (unsigned long long)st.entries_compared, genuine_count, (unsigned long long)st.ended_sentinel, (unsigned long long)st.ended_call_stop, (unsigned long long)st.ended_other,
           (unsigned long long)st.instr_total, st.t_lifted > 0 ? st.t_interp / st.t_lifted : 0.0,
           (unsigned long long)st.enters, (unsigned long long)st.escapes, (unsigned long long)st.irq_escapes);
    if (replay_seconds > 0 && heavy_count && !st.mismatched) {
        double tl = 0, ti = 0; uint64_t instr_sum = 0, enters = 0, escapes = 0, interp_in_lifted = 0, lifted_ns = 0, irq_esc = 0;
        for (unsigned h = 0; h < heavy_count; ++h) {
            const double budget = replay_seconds / heavy_count; double tli = 0, tii = 0; unsigned reps = 0;
            while (tii < budget || reps < 3) {
                setup_trial(I, pristine, heavy[h].entry, heavy[h].seed, data_begin, 0);
                const double a0 = seconds_now(); uint64_t n; int k; run_interp_to_end(I, 100000000ull, &n, &k); tii += seconds_now() - a0;
                setup_trial(L, pristine, heavy[h].entry, heavy[h].seed, data_begin, 0);
                const double b0 = seconds_now(); (void)run_lifted_to_end(L); tli += seconds_now() - b0;
                enters += L->stats.enters; escapes += L->stats.escapes; interp_in_lifted += L->stats.interp_instructions; lifted_ns += L->stats.lifted_ns; irq_esc += L->stats.irq_escapes;
                ++reps; instr_sum += heavy[h].instr; if (reps > 100000) break;
            }
            tl += tli; ti += tii;
        }
        printf("SPEED %-28s heavy_trials=%u instructions=%llu lifted_s=%.3f interp_s=%.3f speedup=%.2fx lifted_equiv_mips=%.0f interp_mips=%.0f | lifted run: enters=%llu escapes=%llu interpreted_instructions=%llu (%.1f%%) time_in_lifted_code=%.3fs\n", m->name, heavy_count,
               (unsigned long long)instr_sum, tl, ti, ti / tl, (double)instr_sum / tl / 1e6, (double)instr_sum / ti / 1e6, (unsigned long long)enters, (unsigned long long)escapes,
               (unsigned long long)interp_in_lifted, instr_sum ? 100.0 * (double)interp_in_lifted / (double)instr_sum : 0.0, (double)lifted_ns / 1e9);
        (void)irq_esc;
#ifdef PS3_LIFTED_ESCAPE_HIST
        { extern uint64_t g_ps3_lifted_escape_hist[]; uint64_t total = 0;
          for (unsigned i = 0; i < SPU_LS_SIZE / 4; ++i) total += g_ps3_lifted_escape_hist[i];
          for (unsigned rank = 0; rank < 8; ++rank) {
              unsigned best = 0; for (unsigned i = 1; i < SPU_LS_SIZE / 4; ++i) if (g_ps3_lifted_escape_hist[i] > g_ps3_lifted_escape_hist[best]) best = i;
              if (!g_ps3_lifted_escape_hist[best]) break;
              printf("ESCAPE_HIST %s #%u pc=0x%05X count=%llu (%.1f%% of %llu)\n", m->name, rank + 1, best * 4u, (unsigned long long)g_ps3_lifted_escape_hist[best],
                     100.0 * (double)g_ps3_lifted_escape_hist[best] / (double)total, (unsigned long long)total);
              g_ps3_lifted_escape_hist[best] = 0; }
          memset(g_ps3_lifted_escape_hist, 0, sizeof(uint64_t) * (SPU_LS_SIZE / 4)); }
#endif
    }
    total->trials += st.trials; total->compared += st.compared; total->mismatched += st.mismatched; total->entries_compared += st.entries_compared;
    free(genuine); free(seen);
    return st.mismatched ? 1 : 0;
}

/* --irq-resume: an interrupt that arrives while lifted code runs is delivered by the interpreter at an exact state. Proof: take it (the
 * vector at LS 0 is `stop 0x77`), return to srr0 with the event acknowledged, finish the job, and compare with a run that was never interrupted.
 * If the lifted code had handed over an imprecise state (a register not written back, a stale pc), the end states would differ. */
static int irq_resume_module(Run *L, Run *I, const ps3_spu_lifted_module *m, const unsigned char *pristine, uint32_t data_begin, uint64_t trials,
                             uint64_t seed, uint64_t cap, unsigned *taken_out, unsigned *lifted_irq_out) {
    unsigned *genuine = malloc(sizeof(unsigned) * m->row_count), count = 0, bad = 0, taken = 0, via_escape = 0, compared = 0;
    for (unsigned i = 0; i < m->row_count; ++i)
        if (m->rows[i].flags == PS3_LIFTED_ROW_GENUINE && m->rows[i].pc >= m->code_begin + 0x30u && m->rows[i].pc >= 0x4000u && m->rows[i].pc < m->code_end) genuine[count++] = i;
    for (uint64_t trial = 0; trial < trials && count && bad < 3; ++trial) {
        const uint32_t entry = m->rows[genuine[trial % count]].pc; const uint64_t trial_seed = seed + trial * 0x1000193u + 1u;
        setup_trial(I, pristine, entry, trial_seed, data_begin, 1);
        uint64_t instr; int kind; run_interp_to_end(I, cap, &instr, &kind);
        if (kind != 0 || I->last.code == STOP_IRQ || I->channel_count == 0) continue;
        if (memcmp(I->owner->core.ls + m->code_begin, pristine + m->code_begin, m->code_end - m->code_begin)) continue;
        if (I->last.pc == 4 && I->last.code == 0) continue;
        { const unsigned char vector[4] = { 0, 0, 0, STOP_IRQ };   /* a job that stored over the interrupt vector (random pointers do) makes the interrupt run garbage on either side */
          if (memcmp(I->owner->core.ls, vector, 4)) continue; }
        const unsigned total_ops = I->channel_count;
        setup_trial(L, pristine, entry, trial_seed, data_begin, 1);
        L->inject_after = 1u + (unsigned)(trial_seed % total_ops);
        signal(SIGALRM, on_alarm); snprintf(g_current, sizeof g_current, "irq-resume %s entry 0x%05X seed %llu", m->name, entry, (unsigned long long)trial_seed); alarm(60);
        int rc = run_lifted_to_end(L);
        spu_context *c = &L->owner->core;
        if (!rc && L->last.kind == PS3_SPU_INTERP_STOPPED && L->last.code == STOP_IRQ) {
            ++taken; if (L->stats.irq_escapes) ++via_escape;
            c->event_status = 0; c->int_enable = 1; c->pc = c->srr0; L->inject_after = 0;     /* iret: acknowledged, back to the interrupted pc */
            rc = run_lifted_to_end(L);
        }
        alarm(0);
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED &&
            (L->last.channel == PS3_SPU_EXEC_OP_INDIRECT_BRANCH || L->last.channel == PS3_SPU_EXEC_OP_CONTINUATION || L->last.channel == PS3_SPU_EXEC_OP_DRAIN_CALL)) continue;
        c->srr0 = I->owner->core.srr0; c->event_status = I->owner->core.event_status; c->int_enable = I->owner->core.int_enable;
        char label[140]; snprintf(label, sizeof label, "%s irq-resume(entry 0x%05X seed %llu after op %u/%u)", m->name, entry, (unsigned long long)trial_seed, L->inject_after, total_ops);
        const unsigned d = rc ? 1u : compare_runs(L, I, label, CMP_QUIET, 1);
        if (rc) printf("DIFF %s lifted run failed rc=%d\n", label, rc);
        ++compared;
        if (d) { ++bad; printf("IRQ_RESUME_MISMATCH %s\n", label); }
    }
    printf("IRQ_RESUME %-28s compared=%u mismatched=%u interrupts_taken=%u (of which lifted code escaped for it: %u)\n", m->name, compared, bad, taken, via_escape);
    if (taken_out) *taken_out += taken; if (lifted_irq_out) *lifted_irq_out += via_escape;
    free(genuine);
    return (int)bad;
}


#ifdef PS3_SPU_NATIVE_INFLATE
/* ----------------------------------------------------------------------------------------------- native inflate (round 2)
 * --inflate [CORPUS] [--inflate-synthetic N]: the Edge zlib task's raw-inflate function at 0x3750 (native_inflate.h) against the lifted function and the
 * interpreter. Streams: the records of CORPUS (make_inflate_corpus.py: real zlib streams of the game's PSARC files, header and trailer removed) and N
 * synthetic ones (zlib at every level and strategy over several kinds of data), each in five variants: as is, bits flipped, truncated, trailing garbage,
 * wrong expected size (the fallback paths). Per case three runs from the same local store: the interpreter, the lifted function with the native path
 * off, and with it on. The first two must be IDENTICAL (everything: registers, local store, mailboxes, DMA); the third must equal the second in r0, r1,
 * r3 and r80..r127, in the output bytes and in the rest of the local store except the dead areas named below (stack frames below the caller's sp, the
 * static inflate state of the task's bss), and must take the native path exactly for the cases that inflate cleanly to the expected size. */
enum { INF_TRAMP = 0x20000, INF_IN = 0x21000, INF_IN_MAX = 0x12000, INF_OUT = 0x0C000, INF_OUT_MAX = 0x14000, INF_FN = 0x3750 };   /* all inside the task's bss, below its static inflate state */
typedef struct { unsigned char *data; uint32_t len, out_size; const char *kind; } InflateCase;
static void put_inflate_stub(unsigned char *ls) {
    put32(ls, STUB, 0x35200100u);             /* bisl $0,$2 : the trampoline (r2) with the return address in r0 */
    put32(ls, STUB + 4, STOP_AFTER_CALL);
    put32(ls, SENTINEL, STOP_SENTINEL);
    put32(ls, INF_TRAMP, 0x0400007Fu);        /* ori   $127,$0,0   keep the stub's return address */
    put32(ls, INF_TRAMP + 4, 0x31000000u | (((INF_FN >> 2) & 0xFFFFu) << 7));   /* brasl $0,0x3750 */
    put32(ls, INF_TRAMP + 8, 0x35003F80u);    /* bi    $127        back to the stub */
}
static void setup_inflate(Run *r, const unsigned char *pristine, const InflateCase *c) {
    owner_restart(r);
    r->refuse_writes = 1; r->fail_waits = 1;
    spu_context *cx = &r->owner->core;
    memcpy(cx->ls, pristine, SPU_LS_SIZE);
    put_inflate_stub(cx->ls);
    memset(cx->ls + INF_OUT, 0xA5, INF_OUT_MAX);
    memcpy(cx->ls + INF_IN, c->data, c->len);
    for (unsigned reg = 0; reg < 128; ++reg) cx->gpr[reg] = (u128){ 0 };
    cx->gpr[0]._u32[0] = SENTINEL; cx->gpr[1]._u32[0] = STACK; cx->gpr[1]._u32[1] = 0x2000; /* word 1 of the stack pointer register is the stack space left: the function's prologue halts when it goes negative */ cx->gpr[2]._u32[0] = INF_TRAMP;
    cx->gpr[3]._u32[0] = INF_OUT; cx->gpr[4]._u32[0] = c->out_size; cx->gpr[5]._u32[0] = INF_IN; cx->gpr[6]._u32[0] = c->len;
    cx->pc = STUB; cx->status = SPU_STATUS_RUNNING; cx->stop_code = 0;
}
static unsigned inflate_compare_native(Run *N, Run *L, const char *label, unsigned *ls_ranges_out) {
    unsigned diffs = 0; const spu_context *cn = &N->owner->core, *cl = &L->owner->core;
    if (N->last.kind != L->last.kind || N->last.pc != L->last.pc || N->last.code != L->last.code) { printf("DIFF %s result: native kind=%d pc=0x%05X code=0x%X | lifted kind=%d pc=0x%05X code=0x%X\n", label, N->last.kind, N->last.pc, N->last.code, L->last.kind, L->last.pc, L->last.code); ++diffs; }
    for (unsigned reg = 0; reg < 128; ++reg) {
        if (reg >= 2 && reg < 80 && reg != 3) continue;           /* caller-saved by the SPU ABI */
        for (unsigned w = 0; w < 4; ++w)
            if (cn->gpr[reg]._u32[w] != cl->gpr[reg]._u32[w]) { if (diffs < 6) printf("DIFF %s r%u.w%u: native 0x%08X lifted 0x%08X\n", label, reg, w, cn->gpr[reg]._u32[w], cl->gpr[reg]._u32[w]); ++diffs; }
    }
    if (N->channel_count != L->channel_count || N->dma_count != L->dma_count) { printf("DIFF %s channel/DMA operations: native %u/%u lifted %u/%u\n", label, N->channel_count, N->dma_count, L->channel_count, L->dma_count); ++diffs; }
    /* local store: everything but the stack frames the callee left below the caller's sp and the task's static inflate state */
    const uint32_t dead_lo = STACK - 0x1000u, dead_hi = STACK + 0x20u, state_lo = 0x33E00u, state_hi = 0x36000u;   /* the static inflate_state and its tables; the 16-byte unaligned-copy scratch at 0xBC90 is named below */   /* sp + 0x10 is the callee's link-register save slot: the lifted function writes it, the native path does not need to */
    for (uint32_t i = 0; i < SPU_LS_SIZE;) {
        if (cn->ls[i] == cl->ls[i]) { ++i; continue; }
        if ((i >= dead_lo && i < dead_hi) || (i >= state_lo && i < state_hi) || (i >= 0xBC90u && i < 0xBCA0u)) { if (ls_ranges_out) ++*ls_ranges_out; ++i; continue; }
        const uint32_t start = i;
        while (i < SPU_LS_SIZE && cn->ls[i] != cl->ls[i] && !((i >= dead_lo && i < dead_hi) || (i >= state_lo && i < state_hi) || (i >= 0xBC90u && i < 0xBCA0u))) ++i;
        if (diffs < 6) printf("DIFF %s ls[0x%05X,0x%05X): %u bytes differ, first native 0x%02X lifted 0x%02X\n", label, start, i, i - start, cn->ls[start], cl->ls[start]);
        ++diffs;
    }
    return diffs;
}
static void inflate_add(InflateCase **cases, unsigned *count, unsigned *cap, unsigned char *data, uint32_t len, uint32_t out_size, const char *kind) {
    if (len > INF_IN_MAX || out_size > INF_OUT_MAX || !len) { free(data); return; }
    if (*count == *cap) { *cap = *cap ? *cap * 2 : 256; *cases = realloc(*cases, *cap * sizeof **cases); }
    (*cases)[(*count)++] = (InflateCase){ data, len, out_size, kind };
}
static unsigned char *make_data(uint64_t *rng, uint32_t size, unsigned kind) {
    unsigned char *d = malloc(size ? size : 1);
    switch (kind % 5) {
    case 0: for (uint32_t i = 0; i < size; ++i) d[i] = (unsigned char)xorshift(rng); break;                                          /* incompressible */
    case 1: memset(d, 0, size); break;                                                                                                 /* zeros */
    case 2: for (uint32_t i = 0; i < size; ++i) d[i] = (unsigned char)("the quick brown fox jumps over the lazy dog 0123456789 "[ (i + (xorshift(rng) & 3)) % 52 ]); break;   /* text-like */
    case 3: for (uint32_t i = 0; i < size; ++i) d[i] = (unsigned char)((i * 7 + (xorshift(rng) & 15)) >> 2); break;                  /* smooth binary */
    default: { uint32_t i = 0; while (i < size) { const uint32_t run = 1 + (uint32_t)(xorshift(rng) % 40), v = (uint32_t)xorshift(rng); for (uint32_t k = 0; k < run && i < size; ++k, ++i) d[i] = (unsigned char)(v >> (k & 7)); } } break;   /* runs */
    }
    return d;
}
static int inflate_test(Run *L, Run *N, Run *I, const ps3_spu_lifted_module *m, const unsigned char *pristine, const char *corpus, unsigned synthetic) {
    InflateCase *cases = NULL; unsigned count = 0, cap = 0;
    uint64_t rng = 0xC0FFEE123ull;
    unsigned real = 0;
    if (corpus) {
        FILE *f = fopen(corpus, "rb");
        if (!f) { fprintf(stderr, "OPEN_FAILED corpus %s\n", corpus); return 1; }
        uint32_t header[3];
        while (fread(header, 4, 3, f) == 3) {
            unsigned char *payload = malloc(header[0]);
            if (!payload || fread(payload, 1, header[0], f) != header[0]) { free(payload); break; }
            inflate_add(&cases, &count, &cap, payload, header[0], header[1], "real");
            ++real;
        }
        fclose(f);
    }
    const unsigned real_cases = count;
    for (unsigned k = 0; k < synthetic; ++k) {
        const uint32_t size = (k % 11 == 0) ? (uint32_t)(xorshift(&rng) % 40) : (uint32_t)(1 + xorshift(&rng) % (k % 7 == 0 ? INF_OUT_MAX : 20000));
        unsigned char *data = make_data(&rng, size, k);
        z_stream zs; memset(&zs, 0, sizeof zs);
        static const int strategies[5] = { Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED };
        if (deflateInit2(&zs, (int)(xorshift(&rng) % 10), Z_DEFLATED, -15, 1 + (int)(xorshift(&rng) % 9), strategies[xorshift(&rng) % 5]) != Z_OK) { free(data); continue; }
        unsigned char *packed = malloc(deflateBound(&zs, size) + 16);
        zs.next_in = data; zs.avail_in = size; zs.next_out = packed; zs.avail_out = (uInt)deflateBound(&zs, size) + 16;
        const int rc = deflate(&zs, Z_FINISH);
        const uint32_t packed_len = (uint32_t)zs.total_out;
        deflateEnd(&zs);
        free(data);
        if (rc != Z_STREAM_END) { free(packed); continue; }
        inflate_add(&cases, &count, &cap, packed, packed_len, size, "synthetic");
    }
    /* variants of every base case */
    const unsigned base = count;
    for (unsigned k = 0; k < base; ++k) {
        const InflateCase c = cases[k];   /* by value: inflate_add may move the array */
        unsigned char *copy = malloc(c.len + 1);
        memcpy(copy, c.data, c.len);
        for (unsigned f = 0, flips = 1 + (unsigned)(xorshift(&rng) % 3); f < flips; ++f) copy[xorshift(&rng) % c.len] ^= (unsigned char)(1u << (xorshift(&rng) & 7));
        inflate_add(&cases, &count, &cap, copy, c.len, c.out_size, "flipped");
        if (c.len > 2) { unsigned char *cut = malloc(c.len); memcpy(cut, c.data, c.len); inflate_add(&cases, &count, &cap, cut, c.len - 1 - (uint32_t)(xorshift(&rng) % (c.len > 8 ? 4 : 1)), c.out_size, "truncated"); }
        { unsigned char *ext = malloc(c.len + 64); memcpy(ext, c.data, c.len); for (unsigned g = 0; g < 17; ++g) ext[c.len + g] = (unsigned char)xorshift(&rng); inflate_add(&cases, &count, &cap, ext, c.len + 17, c.out_size, "trailing"); }
        { unsigned char *same = malloc(c.len); memcpy(same, c.data, c.len); inflate_add(&cases, &count, &cap, same, c.len, c.out_size + 1 + (uint32_t)(xorshift(&rng) % 3), "size+"); }
        if (c.out_size > 1) { unsigned char *same = malloc(c.len); memcpy(same, c.data, c.len); inflate_add(&cases, &count, &cap, same, c.len, c.out_size - 1, "size-"); }
    }
    printf("INFLATE_TEST %s cases=%u (real streams %u, synthetic %u, variants %u)\n", m->name, count, real_cases, count > base ? base - real_cases : 0, count - base);
    ps3_native_inflate_reset_stats();
    unsigned bad = 0, ran = 0, expect_native = 0, took_native = 0, dead_bytes = 0, interp_ok = 0, paused_cases = 0;
    uint64_t in_bytes = 0, out_bytes = 0;
    double t_lifted = 0, t_native = 0, t_interp = 0, t_lifted_clean = 0, t_native_clean = 0;
    for (unsigned k = 0; k < count && bad < 4; ++k) {
        const InflateCase *c = &cases[k];
        char label[160]; snprintf(label, sizeof label, "%s inflate case %u (%s, %u bytes -> %u)", m->name, k, c->kind, c->len, c->out_size);
        /* ground truth: what zlib says about this stream and size */
        z_stream zs; memset(&zs, 0, sizeof zs); inflateInit2(&zs, -15);
        unsigned char *host_out = malloc(c->out_size + 1);
        zs.next_in = c->data; zs.avail_in = c->len; zs.next_out = host_out; zs.avail_out = c->out_size;
        const int zr = inflate(&zs, Z_FINISH); const int clean = zr == Z_STREAM_END && zs.total_out == c->out_size && c->out_size && c->len;   /* (the native path leaves a zero-length call to the lifted function) */ inflateEnd(&zs); free(host_out);
        g_ps3_native_inflate_enabled = 0;
        int interp_paused = 0;
        setup_inflate(I, pristine, c); { const double a = seconds_now(); uint64_t n; int kind; run_interp_to_end(I, 400000000ull, &n, &kind); t_interp += seconds_now() - a; interp_paused = kind == 2; if (kind != 0 && kind != 2) { ++bad; printf("DIFF %s interpreter did not end (kind %d: result kind=%d pc=0x%05X code=0x%X channel=%u stop=0x%X opcode=0x%08X)\n", label, kind, I->last.kind, I->last.pc, I->last.code, I->last.channel, I->last.stop_code, I->last.opcode); continue; } }
        setup_inflate(L, pristine, c); { const double a = seconds_now(); (void)run_lifted_to_end(L); const double e = seconds_now() - a; t_lifted += e; if (clean) t_lifted_clean += e; }
        /* an error path prints through spu_printf and waits for the PPU's reply: the interpreter PAUSES there (the lifted run is ended by the harness's wait service), so only
         * the two lifted runs are compared; every other case must equal the interpreter exactly */
        unsigned d = interp_paused ? 0u : compare_runs(L, I, label, CMP_QUIET, 1);
        paused_cases += (unsigned)interp_paused;
        g_ps3_native_inflate_enabled = 1;
        ps3_native_inflate_stats before; ps3_native_inflate_get_stats(&before);
        setup_inflate(N, pristine, c); { const double a = seconds_now(); (void)run_lifted_to_end(N); const double e = seconds_now() - a; t_native += e; if (clean) t_native_clean += e; }
        ps3_native_inflate_stats after; ps3_native_inflate_get_stats(&after);
        const unsigned native_used = after.calls - before.calls == 1;
        d += inflate_compare_native(N, L, label, &dead_bytes);
        if (N->stats.job_armed != 1) { printf("DIFF %s the module was not armed (job_armed=%llu)\n", label, (unsigned long long)N->stats.job_armed); ++d; }
        if (native_used != (unsigned)clean) { printf("DIFF %s native path %s but the stream is %s for zlib (ret %d)\n", label, native_used ? "taken" : "not taken", clean ? "clean" : "not clean", zr); ++d; }
        ++ran; expect_native += (unsigned)clean; took_native += native_used; if (L->last.kind == PS3_SPU_INTERP_STOPPED) ++interp_ok;
        if (native_used) { in_bytes += c->len; out_bytes += c->out_size; }
        if (d) { ++bad; printf("INFLATE_MISMATCH %s\n", label); }
    }
    ps3_native_inflate_stats st; ps3_native_inflate_get_stats(&st);
    printf("INFLATE %-28s cases=%u ran=%u (interpreter paused in printf: %u) mismatched=%u clean(native expected)=%u native_taken=%u | fallbacks: args=%llu overlap=%llu zlib=%llu size=%llu | dead-area byte diffs ignored=%u | bytes in=%llu out=%llu\n",
           m->name, count, ran, paused_cases, bad, expect_native, took_native, (unsigned long long)st.fallback_args, (unsigned long long)st.fallback_overlap, (unsigned long long)st.fallback_zlib,
           (unsigned long long)st.fallback_size, dead_bytes, (unsigned long long)in_bytes, (unsigned long long)out_bytes);
    printf("INFLATE_TIME interpreter=%.3fs lifted(native off)=%.3fs lifted(native on)=%.3fs (all cases, fallbacks included; run setup included)\n", t_interp, t_lifted, t_native);
    printf("INFLATE_SPEED clean streams only: lifted %.3fs, native %.3fs (zlib itself %.3fs for %llu bytes out) -> %.1fx per call including the run setup both sides pay\n", t_lifted_clean, t_native_clean, (double)st.ns / 1e9,
           (unsigned long long)out_bytes, t_native_clean > 0 ? t_lifted_clean / t_native_clean : 0.0);
    for (unsigned k = 0; k < count; ++k) free(cases[k].data);
    free(cases);
    return bad ? 1 : 0;
}
#endif

/* ---------------------------------------------------------------------------------------------------- dispatch edge cases */
static int edge_cases(Run *L, Run *I, const ps3_spu_lifted_module *m, const unsigned char *pristine, uint32_t data_begin) {
    int bad = 0;
    uint32_t entry = m->entry_pc;
    if (entry < 0x4000u) {
        /* a task image (round 2): its entry is below the job window, so the policy-module stub's call arms nothing; the call must land in the window.
         * Take the first genuine function there whose job ends cleanly at the stub (some seed of 7..39). */
        entry = 0;
        for (unsigned i = 0; i < m->row_count && !entry; ++i) {
            if (m->rows[i].flags != PS3_LIFTED_ROW_GENUINE || m->rows[i].pc < 0x4000u || m->rows[i].pc >= m->code_end) continue;
            for (uint64_t seed = 7; seed < 40; ++seed) {
                setup_trial(I, pristine, m->rows[i].pc, seed, data_begin, 0);
                uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k);
                if (k == 0 && I->last.kind == PS3_SPU_INTERP_STOPPED && (I->last.code == STOP_SENTINEL || I->last.code == STOP_AFTER_CALL)) { entry = m->rows[i].pc; break; }
            }
        }
        CHECK(entry != 0, "%s: no function of the module in the job window ends cleanly under the stub", m->name);
        printf("EDGE %s task image: entry below the window, edge cases use the function at 0x%05X\n", m->name, entry);
    }
    /* 1. the genuine module is armed by the job call and enters lifted code */
    for (uint64_t seed = 7; seed < 40; ++seed) {      /* a seed whose job ends cleanly at the stub's stop, so the owner is still usable */
        setup_trial(L, pristine, entry, seed, data_begin, 0); (void)run_lifted_to_end(L);
        if (L->last.kind == PS3_SPU_INTERP_STOPPED && !L->owner->stopped) break;
    }
    CHECK(L->stats.job_calls == 1 && L->stats.job_armed == 1 && L->stats.job_mismatch == 0 && L->stats.job_unknown == 0, "%s: job call stats calls=%llu armed=%llu mismatch=%llu unknown=%llu", m->name,
          (unsigned long long)L->stats.job_calls, (unsigned long long)L->stats.job_armed, (unsigned long long)L->stats.job_mismatch, (unsigned long long)L->stats.job_unknown);
    CHECK(L->owner->watch_entry_bits != NULL && L->owner->watch_code_hi == m->code_end, "%s: not armed after the job", m->name);
    printf("EDGE %s armed: enters=%llu escapes=%llu\n", m->name, (unsigned long long)L->stats.enters, (unsigned long long)L->stats.escapes);
    /* 2. a DMA GET over the code disarms (executor hook), outside lifted code */
    {
        spu_context *c = &L->owner->core; const uint32_t lsa = 0x4100, bytes = 128;
        (void)ps3_spu_executor_write_channel(L->owner, MFC_LSA, (u128){ ._u32 = { lsa, 0, 0, 0 } });
        (void)ps3_spu_executor_write_channel(L->owner, MFC_EAL, (u128){ ._u32 = { 0x10000, 0, 0, 0 } });
        (void)ps3_spu_executor_write_channel(L->owner, MFC_Size, (u128){ ._u32 = { bytes, 0, 0, 0 } });
        (void)ps3_spu_executor_write_channel(L->owner, MFC_TagID, (u128){ ._u32 = { 1, 0, 0, 0 } });
        const int status = ps3_spu_executor_write_channel(L->owner, MFC_Cmd, (u128){ ._u32 = { 0x40, 0, 0, 0 } });   /* GET */
        (void)c;
        CHECK(status == PS3_SPU_EXEC_OK, "%s: GET over code refused status=%d", m->name, status);
        CHECK(L->owner->watch_entry_bits == NULL && L->owner->watch_disarms == 1, "%s: GET over the code did not disarm (bits=%p disarms=%llu)", m->name, (const void *)L->owner->watch_entry_bits, (unsigned long long)L->owner->watch_disarms);
    }
    /* 3. the changed code is not lifted next time (hash mismatch), and the result equals the interpreter's */
    setup_trial(I, pristine, entry, 7, data_begin, 0);
    setup_trial(L, pristine, entry, 7, data_begin, 0);
    L->owner->core.ls[m->code_begin + (m->code_end - m->code_begin) / 2] ^= 0x10;     /* one flipped bit in the middle of the code */
    I->owner->core.ls[m->code_begin + (m->code_end - m->code_begin) / 2] ^= 0x10;
    { uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k); (void)run_lifted_to_end(L); }
    CHECK(L->stats.job_mismatch == 1 && L->stats.enters == 0 && L->stats.job_armed == 0, "%s: a corrupted module was lifted (mismatch=%llu enters=%llu armed=%llu)", m->name,
          (unsigned long long)L->stats.job_mismatch, (unsigned long long)L->stats.enters, (unsigned long long)L->stats.job_armed);
    if (I->last.kind == PS3_SPU_INTERP_STOPPED || I->last.kind == PS3_SPU_INTERP_FAULT) bad += (int)compare_runs(L, I, "corrupted module", CMP_QUIET, 1);
    /* 4. a module the table does not know (unknown signature) */
    setup_trial(L, pristine, entry, 7, data_begin, 0); L->owner->core.ls[m->code_begin] ^= 0x01;
    (void)run_lifted_to_end(L);
    CHECK(L->stats.job_unknown == 1 && L->stats.enters == 0, "%s: unknown signature handling unknown=%llu enters=%llu", m->name, (unsigned long long)L->stats.job_unknown, (unsigned long long)L->stats.enters);
    /* 5. a pending enabled interrupt at the job call: the interpreter takes it before any lifted code (vector at LS 0 = stop 0x77) */
    setup_trial(I, pristine, entry, 9, data_begin, 1); setup_trial(L, pristine, entry, 9, data_begin, 1);
    I->owner->core.event_status = L->owner->core.event_status = 1;
    { uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k); (void)run_lifted_to_end(L); }
    CHECK(L->last.kind == PS3_SPU_INTERP_STOPPED && L->last.code == STOP_IRQ && L->stats.enters == 0, "%s: interrupt pending at the call: kind=%d code=0x%X enters=%llu", m->name, L->last.kind, L->last.code, (unsigned long long)L->stats.enters);
    bad += (int)compare_runs(L, I, "irq at call", CMP_QUIET, 1);
    /* 6. a DMA GET over lifted code that runs is a named terminal (the hook), never a silent run on stale code */
    for (uint64_t seed = 7; seed < 40; ++seed) {
        setup_trial(L, pristine, entry, seed, data_begin, 0); (void)run_lifted_to_end(L);
        if (L->last.kind == PS3_SPU_INTERP_STOPPED && !L->owner->stopped && L->owner->watch_entry_bits) break;
    }
    L->owner->watch_in_lifted = 1;
    (void)ps3_spu_executor_write_channel(L->owner, MFC_LSA, (u128){ ._u32 = { 0x4100, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(L->owner, MFC_EAL, (u128){ ._u32 = { 0x10000, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(L->owner, MFC_Size, (u128){ ._u32 = { 128, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(L->owner, MFC_TagID, (u128){ ._u32 = { 1, 0, 0, 0 } });
    { const int status = ps3_spu_executor_write_channel(L->owner, MFC_Cmd, (u128){ ._u32 = { 0x40, 0, 0, 0 } });
      CHECK(status == PS3_SPU_EXEC_UNSUPPORTED && L->owner->terminal.channel == PS3_LIFTED_OP_CODE_CHANGED_DMA, "%s: DMA over running lifted code: status=%d channel=0x%X", m->name, status, L->owner->terminal.channel); }
    L->owner->watch_in_lifted = 0;
    printf("EDGE %s ok (bad=%d failures=%d)\n", m->name, bad, failures);
    return bad;
}


/* ================================================================================================= policy module (pm_wwsjob), round 2
 * The module is the blob the SPURS kernel DMAs to LS 0xA00 (extra/<name>.bin). It is lifted beside the jobs: armed by the first job call (the dyn layer looks for it in LS 0xA00
 * when a link-setting branch from outside the window enters the window), its lifted functions call jobs through a hook (function 0x31C8), its channel reads that cannot
 * complete (RdEventStat with nothing selected, RdListStallStat with no stall) leave lifted code AT the instruction and the interpreter takes over.
 * Fixture (no real PPU, no real SPURS state; random data, as for the jobs): the stub at LS 0x400 (kernel region)
 *     bisl $0,$120      r120 = the window target: a `bi $0` at LS 0x4000 (PM only) or after the job image (composite): the job call that arms the policy module (and the job)
 *     bisl $0,$3        r3 = the policy-module function under test (or SENTINEL for the arming checks)
 *     stop 0x3FFE
 * Both runs start from the same pristine local store and the same seed; the interpreter run is the reference. PAUSED ends (a channel read nobody serves) are compared too. */
enum { PM_STUB = 0x400, PM_KERNEL_RET = 0x500, PM_WINDOW = 0x4000, PM_BASE = 0xA00, PM_WWSJOB_ENTRY = 0x2BF0, PM_WWSJOB_JOB_FN = 0x31C8, PM_POKE = 0x3D000, PM_ARENA_LO = 0x100000, PM_ARENA_HI = 0x200000, PM_STAGE1_REG = 120 };
typedef struct { uint64_t trials, compared, mismatched, skip_fault, skip_budget, skip_selfmod, skip_known, ended_stop, ended_sentinel, ended_paused, armed, composites, pause_escapes, job_hooks, enters, escapes, instr;
                 unsigned entries_compared; double t_lifted, t_interp; } PmStats;

/* Policy-module trials let the module's DMA write the guest window (it must: the module PUTs its state and results, a refused PUT faults at once). The window is scanned after each run: the pages that
 * differ from the pristine copy are what the run wrote; they are restored, and the lifted run's must equal the interpreter's (page numbers and contents). */
typedef struct { unsigned count; int overflow; uint32_t page[64]; uint64_t hash[64]; } GuestDirty;
static unsigned char *g_guest_ref, *g_guest_buf;
static void guest_ref_init(Run *r) {            /* (re)takes the pristine copy: the arena words depend on the module's variable range, so every policy module has its own */
    if (!g_guest_ref) { g_guest_ref = malloc(SPACE_BYTES); g_guest_buf = malloc(SPACE_BYTES); }
    if (!g_guest_ref || !g_guest_buf || r->backing.read(r->backing.context, 0, g_guest_ref, SPACE_BYTES)) { fprintf(stderr, "GUEST_REF_FAILED\n"); exit(1); }
}
static void guest_scan_restore(Run *r, GuestDirty *out) {
    memset(out, 0, sizeof *out);
    if (!r->guest_dirty) return;
    r->guest_dirty = 0;
    if (r->backing.read(r->backing.context, 0, g_guest_buf, SPACE_BYTES)) { fprintf(stderr, "GUEST_READ_FAILED\n"); exit(1); }
    for (uint32_t p = 0; p < SPACE_BYTES / 4096u; ++p) {
        if (!memcmp(g_guest_buf + (size_t)p * 4096u, g_guest_ref + (size_t)p * 4096u, 4096u)) continue;
        if (out->count < 64u) { out->page[out->count] = p; out->hash[out->count] = ps3_spu_lifted_hash64w(g_guest_buf + (size_t)p * 4096u, 4096u); } else out->overflow = 1;
        ++out->count;
        if (r->backing.write(r->backing.context, p * 4096u, g_guest_ref + (size_t)p * 4096u, 4096u)) { fprintf(stderr, "GUEST_RESTORE_FAILED\n"); exit(1); }
    }
}
/* debugging aid (PM_TRACE_GUEST): how many pages of the run's guest window differ from the pristine copy right now (should be 0 between trials) */
static unsigned guest_unclean(Run *r) {
    if (r->backing.read(r->backing.context, 0, g_guest_buf, SPACE_BYTES)) return 9999u;
    unsigned n = 0; for (uint32_t p = 0; p < SPACE_BYTES / 4096u; ++p) if (memcmp(g_guest_buf + (size_t)p * 4096u, g_guest_ref + (size_t)p * 4096u, 4096u)) ++n;
    return n;
}
static unsigned guest_compare(const GuestDirty *a, const GuestDirty *b, const char *label) {
    unsigned diff = a->count != b->count;
    for (unsigned i = 0; i < a->count && i < b->count && i < 64u && !diff; ++i) diff = a->page[i] != b->page[i] || a->hash[i] != b->hash[i];
    if (diff) {
        printf("DIFF %s guest memory: lifted wrote %u pages, interpreter %u\n", label, a->count, b->count);
        if (getenv("PM_REPLAY_VERBOSE")) { for (unsigned i = 0; i < a->count && i < 8u && i < 64u; ++i) printf("   lifted page %u (0x%X) hash %016llx\n", a->page[i], a->page[i] * 4096u, (unsigned long long)a->hash[i]);
                                           for (unsigned i = 0; i < b->count && i < 8u && i < 64u; ++i) printf("   interp page %u (0x%X) hash %016llx\n", b->page[i], b->page[i] * 4096u, (unsigned long long)b->hash[i]); }
    }
    return diff;
}
/* What differs between the policy modules: where its variables lie (between its code ranges, or behind the last one) and whether it has a job-call function (pm_wwsjob's 0x31C8; the task-set module
 * has none: a task is entered by an indirect branch). Set by policy_module(). */
static uint32_t g_pm_var_lo, g_pm_var_hi, g_pm_job_fn;
static const char *g_extra_dir;      /* --extra: where the task images (and the policy modules) are */
static void pm_put_stub(unsigned char *ls, int irq) {
    const uint32_t e = irq ? 0x00040000u : 0u;
    put32(ls, PM_STUB, 0x35200000u | (PM_STAGE1_REG << 7) | e);          /* bisl $0,$120 */
    put32(ls, PM_STUB + 4, 0x35200180u | e);      /* bisl $0,$3 */
    put32(ls, PM_STUB + 8, STOP_AFTER_CALL);
    put32(ls, PM_KERNEL_RET, 0x35000000u);       /* bi $0: a harmless first call from the kernel region that does not enter the job window (the policy module is then armed only by the branch to its entry) */
    put32(ls, SENTINEL, STOP_SENTINEL);
}
/* The pristine local store: zeros, the policy module at 0xA00, the job image (when given) at its load address, `bi $0` at *window_call (the job call that arms), the stub. */
static void pm_make_pristine(unsigned char *ls, const unsigned char *pm_image, size_t pm_bytes, const unsigned char *job_image, size_t job_bytes, uint32_t job_load, uint32_t *window_call) {
    memset(ls, 0, SPU_LS_SIZE);
    memcpy(ls + PM_BASE, pm_image, pm_bytes);
    if (job_image) { memcpy(ls + job_load, job_image, job_bytes); *window_call = (job_load + (uint32_t)job_bytes + 0x400u + 15u) & ~15u; }
    else *window_call = PM_WINDOW;
    put32(ls, *window_call, 0x35000000u);        /* bi $0 */
    pm_put_stub(ls, 0);
}
/* Words of the policy-module fixture: mostly what real SPURS data is made of (zero, small counts, aligned pointers into the local store, aligned effective addresses into an arena of the guest window,
 * the module's own variables), a little of anything. Plain random words make the module's first DMA fault (an effective address outside the window) before it reaches anything. */
static uint32_t pm_friendly_word(uint64_t *s) {
    const uint64_t v = xorshift(s); const uint32_t x = (uint32_t)(v >> 8);
    switch (v & 15u) {
    case 0: case 1: case 2: case 3: case 4: case 5: return 0;
    case 6: case 7: return x & 0xFFu;
    case 8: case 9: return PM_ARENA_LO + ((x % (PM_ARENA_HI - PM_ARENA_LO - 0x1000u)) & ~0xFu);
    case 10: case 11: return g_pm_var_lo + ((x % (g_pm_var_hi - g_pm_var_lo)) & ~0xFu);
    case 12: return 0x6000u + ((x % (0x3C000u - 0x6000u)) & ~0xFu);
    case 13: return x & 0xFFFFu;
    default: return x;
    }
}
static void pm_arena_fill(Run *r) {         /* the guest window's arena: friendly words, the same for both runs */
    static unsigned char buf[PM_ARENA_HI - PM_ARENA_LO]; uint64_t s = 0xA11CE5EEDull;
    for (uint32_t a = 0; a < sizeof buf; a += 4) put32(buf, a, pm_friendly_word(&s));
    if (r->backing.write(r->backing.context, PM_ARENA_LO, buf, sizeof buf)) { fprintf(stderr, "ARENA_FILL_FAILED\n"); exit(1); }
}
static uint32_t pm_random_word(uint64_t *s, uint32_t data_begin) {
    const uint64_t v = xorshift(s);
    switch (v & 7u) {
    case 0: return (uint32_t)(v >> 8) & 0xFFu;
    case 1: case 2: return data_begin + (((uint32_t)(v >> 8) % (DATA_END - data_begin - 0x1000u)) & ~0xFu);
    case 3: return (((uint32_t)(v >> 8)) % (SPACE_BYTES - 0x20000u)) & ~0xFu;
    case 4: return (uint32_t)(v >> 8);
    case 5: return g_pm_var_lo + (((uint32_t)(v >> 8) % (g_pm_var_hi - g_pm_var_lo)) & ~0xFu);     /* a pointer into the policy module's own variables */
    default: return 0;
    }
}
static void pm_setup_trial(Run *r, const unsigned char *pristine, uint32_t stage1, uint32_t stage2, uint64_t seed, uint32_t data_begin, int irq_mode) {
    setup_trial(r, pristine, stage2, seed, data_begin, 0);
    spu_context *c = &r->owner->core;
    uint64_t s = (seed + 0x51ED270Bull) * 0x9E3779B97F4A7C15ull | 1u;
    for (uint32_t a = g_pm_var_lo; a < g_pm_var_hi; a += 16)          /* some of the module's variables and tables are not what the image holds */
        if ((xorshift(&s) & 7u) == 0) for (unsigned w = 0; w < 4; ++w) put32(c->ls, a + 4u * w, pm_friendly_word(&s));
    for (uint32_t a = data_begin; a < DATA_END; a += 4) put32(c->ls, a, pm_friendly_word(&s));
    for (unsigned reg = 2; reg < 128; ++reg) for (unsigned w = 0; w < 4; ++w) c->gpr[reg]._u32[w] = pm_friendly_word(&s);
    (void)pm_random_word;
    c->gpr[3] = (u128){ 0 }; c->gpr[3]._u32[0] = stage2;
    c->gpr[PM_STAGE1_REG] = (u128){ 0 }; c->gpr[PM_STAGE1_REG]._u32[0] = stage1;
    r->refuse_writes = 0;
    if (irq_mode) {
        put32(c->ls, 0, STOP_IRQ); pm_put_stub(c->ls, 1);
        c->event_mask = 1u; c->event_status = 0;
    }
    c->pc = PM_STUB; c->status = SPU_STATUS_RUNNING; c->stop_code = 0;
}
/* Function 0x31C8 calls the job at r5 = LS[LS[0x1320].w0 + 0x10].w0 + LS[0x1320].w0: make r5 the given job entry. */
static void pm_poke_job_target(unsigned char *ls, uint32_t target) {
    put32(ls, 0x1320, PM_POKE); put32(ls, PM_POKE + 0x10, target - PM_POKE);
}
static uint32_t g_pm_job_image_end;      /* end of the job image in the local store of the composite being tested */
static int pm_code_unchanged(const Run *r, const unsigned char *pristine, const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job) {
    for (unsigned i = 0; i < pm->range_count; ++i)
        if (memcmp(r->owner->core.ls + pm->ranges[2u * i], pristine + pm->ranges[2u * i], pm->ranges[2u * i + 1u] - pm->ranges[2u * i])) return 0;
    if (!job) return 1;
    /* the job's code, and its data behind the code: the module's table also holds functions lifted from those data bytes (the lifter decodes data as code); a run that wrote its own data and then
     * jumped into it executes the new bytes in the interpreter and the old ones in the lifted table. Real jobs never run their data; such a trial says nothing about the lifted code. */
    return !memcmp(r->owner->core.ls + job->code_begin, pristine + job->code_begin, (g_pm_job_image_end > job->code_end ? g_pm_job_image_end : job->code_end) - job->code_begin);
}
static int pm_known_divergence(const Run *L) {
    if (L->last.kind != PS3_SPU_INTERP_FAULT || L->last.code != PS3_SPU_EXEC_UNSUPPORTED) return 0;
    if ((L->last.channel == PS3_SPU_EXEC_OP_INDIRECT_BRANCH || L->last.channel == PS3_SPU_EXEC_OP_CONTINUATION) && ((L->owner->core.pc & 3u) || !(L->owner->core.pc & SPU_LS_MASK))) return 1;
    return L->last.channel == PS3_SPU_EXEC_OP_DRAIN_CALL;
}
static int pm_run_lifted(Run *r) {          /* run_lifted_to_end without the escape-table diagnostics of the job slot */
    for (;;) {
        ps3_spu_interp_result res;
        r->services = (ps3_spu_lifted_services){ .context = r, .wait = lifted_wait, .intr_written = lifted_intr_written };
        int status = ps3_spu_lifted_run_slice_dyn(r->owner, &r->dyn, &r->services, 200000, &res, &r->stats);
        if (status != PS3_SPU_EXEC_OK) { fprintf(stderr, "%s: slice refused status=%d\n", r->label, status); return status ? status : 1; }
        r->last = res;
        if (res.kind == PS3_SPU_INTERP_BUDGET) continue;
        return 0;
    }
}

/* One batch of trials. `job` NULL: policy module alone; else the composite (the job image sits at its load address). */
static struct { const unsigned char *pristine; uint32_t first, entry, data_begin, target; uint64_t seed; int job_call; } g_pm_trace;
static void pm_trace_setup(Run *r) {
    pm_setup_trial(r, g_pm_trace.pristine, g_pm_trace.first, g_pm_trace.entry, g_pm_trace.seed, g_pm_trace.data_begin, 0);
    if (g_pm_trace.job_call) pm_poke_job_target(r->owner->core.ls, g_pm_trace.target);
}
static int pm_fuzz(Run *L, Run *I, const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job, const unsigned char *pristine, uint32_t window_call, uint32_t data_begin,
                   const uint32_t *pm_entries, unsigned pm_count, const uint32_t *job_entries, unsigned job_count, uint64_t trials, uint64_t seed, uint64_t cap, int irq_mode, int verbose, PmStats *st) {
    unsigned char *seen = calloc(pm_count + job_count + 1u, 1); unsigned bad = 0;
    for (uint64_t trial = 0; trial < trials && bad < 3 && pm_count; ++trial) {
        if (g_replay_entry && trial) break;           /* --trial ENTRY:SEED replays one trial (the job variants use their own job entries) */
        const uint64_t trial_seed = g_replay_entry ? g_replay_seed : seed + trial * 0x1000193u + 1u;
        uint32_t entry; unsigned slot; int job_call = 0, first_call_job = 0, by_entry = 0;
        if (job && job_count && trial % 3u == 0u) {          /* the job-call function with a job target poked into the variables it reads, or (a policy module with no such function) a job function as the first call */
            slot = pm_count + (unsigned)((trial / 3u) % job_count); if (g_pm_job_fn) { entry = g_pm_job_fn; job_call = 1; } else { entry = pm_entries[(unsigned)(trial % pm_count)]; first_call_job = 1; } }
        else { slot = (unsigned)((trial - (job ? trial / 3u + 1u : 0u)) % pm_count); entry = pm_entries[slot]; }
        uint32_t target = (job_call || first_call_job) ? job_entries[slot - pm_count] : 0u;
        if (g_replay_entry) {
            entry = g_replay_entry; for (slot = 0; slot < pm_count && pm_entries[slot] != entry; ++slot) {} if (slot >= pm_count) slot = 0;
            const char *t = getenv("PM_REPLAY_TARGET");      /* the job function of a composite trial (printed by PM_FUZZ_MISMATCH as target=) */
            if (t && job) { target = (uint32_t)strtoul(t, NULL, 16); job_call = g_pm_job_fn != 0; first_call_job = !job_call; if (job_call) entry = g_pm_job_fn; }
            else if (job) { job_call = first_call_job = 0; target = 0; }
        }
        ++st->trials;
        if (!job_call && !first_call_job && trial % 8u == 3u) { entry = pm->entry_pc; by_entry = 1; for (slot = 0; slot < pm_count && pm_entries[slot] != entry; ++slot) {} }   /* the kernel's branch to the module's entry arms it */
        const uint32_t first = first_call_job ? target : by_entry ? (uint32_t)PM_KERNEL_RET : window_call;
        if (g_replay_entry && getenv("PM_TRACE")) {        /* with the modules built under JOBMODS_TRACE=1: where does the lifted run first differ from the interpreter */
            g_pm_trace = (typeof(g_pm_trace)){ pristine, first, entry, data_begin, target, trial_seed, job_call }; g_trace_setup = pm_trace_setup;
            trace_trial(L, I, pristine, entry, trial_seed, data_begin, 40000); g_trace_setup = NULL; break;
        }
        pm_setup_trial(I, pristine, first, entry, trial_seed, data_begin, irq_mode);
        if (getenv("PM_TRACE_GUEST")) { const unsigned ui = guest_unclean(I), ul = guest_unclean(L); if (ui || ul) printf("PM_TRACE_GUEST trial %llu: guest window not pristine before the run: interpreter %u pages, lifted %u pages\n", (unsigned long long)trial, ui, ul); }
        if (job_call) pm_poke_job_target(I->owner->core.ls, target);
        const double ti0 = seconds_now(); uint64_t instr; int kind;
        run_interp_to_end(I, cap, &instr, &kind);
        const double ti = seconds_now() - ti0;
        GuestDirty gi, gl; guest_scan_restore(I, &gi);
        { const char *diag = getenv("PM_DIAG_ENTRY"); if (diag && strtoul(diag, NULL, 16) == entry) printf("PM_DIAG entry 0x%05X seed %llu: interpreter kind=%d (0 stop, 1 fault, 2 paused, 3 budget) pc=0x%05X code=0x%X channel=%u instructions=%llu\n", entry, (unsigned long long)trial_seed, kind, I->last.pc, I->last.code, I->last.channel, (unsigned long long)instr); }
        if (kind == 0 || kind == 2) { if (!pm_code_unchanged(I, pristine, pm, job)) { ++st->skip_selfmod; continue; } }
        if (kind == 1) { ++st->skip_fault; continue; }
        if (kind == 3) { ++st->skip_budget; continue; }
        if (kind != 0 && kind != 2) { ++st->skip_known; continue; }
        pm_setup_trial(L, pristine, first, entry, trial_seed, data_begin, irq_mode);
        if (job_call) pm_poke_job_target(L->owner->core.ls, target);
        snprintf(g_current, sizeof g_current, "%s%s entry 0x%05X target 0x%05X seed %llu interp_instructions %llu end pc=0x%05X stop=0x%X", pm->name, job ? "+job" : "", entry, target, (unsigned long long)trial_seed, (unsigned long long)instr, I->last.pc, I->last.code);
        signal(SIGALRM, on_alarm); alarm(60);
        const double tl0 = seconds_now(); const int rc = pm_run_lifted(L); const double tl = seconds_now() - tl0; alarm(0);
        guest_scan_restore(L, &gl);
        if (getenv("PM_TRACE_GUEST") && (gl.count || gi.count)) printf("PM_TRACE_GUEST trial %llu entry 0x%05X seed %llu: interpreter %u pages, lifted %u pages (kinds: interp %d, lifted %d code 0x%X)\n", (unsigned long long)trial, entry, (unsigned long long)trial_seed, gi.count, gl.count, kind, L->last.kind, L->last.code);
        if (!rc && pm_known_divergence(L)) { ++st->skip_known; continue; }
        { uint64_t sum = 0; for (unsigned k = 0; k < PS3_SPU_ACCT_SLOTS; ++k) sum += L->owner->acct_instr[k];
          if (!rc && sum != L->stats.interp_instructions) { printf("DIFF %s accounting: attributed %llu interpreted %llu\n", pm->name, (unsigned long long)sum, (unsigned long long)L->stats.interp_instructions); ++st->mismatched; ++bad; continue; } }
        char label[160]; snprintf(label, sizeof label, "%s%s pm-fuzz(entry 0x%05X target 0x%05X seed %llu%s)", pm->name, job ? "+job" : "", entry, target, (unsigned long long)trial_seed, irq_mode ? " irq" : "");
        unsigned diffs;
        if (irq_mode) { diffs = (L->last.kind == I->last.kind && L->last.code == I->last.code) ? 0u : 1u;
                        if (diffs) printf("DIFF %s irq outcome: lifted kind=%d code=0x%X pc=0x%05X | interp kind=%d code=0x%X pc=0x%05X\n", label, L->last.kind, L->last.code, L->last.pc, I->last.kind, I->last.code, I->last.pc); }
        else diffs = rc ? 1u : compare_runs(L, I, label, CMP_QUIET, 1) + guest_compare(&gl, &gi, label);
        if (rc) printf("DIFF %s lifted run failed rc=%d\n", label, rc);
        ++st->compared; st->instr += instr; st->t_lifted += tl; st->t_interp += ti;
        st->armed += L->stats.pm_armed; st->composites += L->stats.pm_composites; st->pause_escapes += L->stats.pm_pause_escapes; st->job_hooks += L->stats.pm_job_hooks;
        st->enters += L->stats.enters; st->escapes += L->stats.escapes;
        if (kind == 2) ++st->ended_paused; else if (I->last.code == STOP_SENTINEL) ++st->ended_sentinel; else ++st->ended_stop;
        if (!seen[slot]) { seen[slot] = 1; ++st->entries_compared; }
        if (diffs) { ++st->mismatched; ++bad; printf("PM_FUZZ_MISMATCH %s entry=0x%05X target=0x%05X seed=%llu irq=%d interp_instructions=%llu\n", pm->name, entry, target, (unsigned long long)trial_seed, irq_mode, (unsigned long long)instr); }
        else if (verbose) printf("PM_FUZZ_OK %s entry=0x%05X target=0x%05X instr=%llu end=%d/0x%X\n", pm->name, entry, target, (unsigned long long)instr, kind, I->last.code);
    }
    free(seen);
    return (int)bad;
}

static unsigned pm_collect_entries(const ps3_spu_lifted_module *m, uint32_t lo, uint32_t hi, uint32_t *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < m->row_count && n < max; ++i) {
        const uint32_t pc = m->rows[i].pc;
        if (m->rows[i].flags != PS3_LIFTED_ROW_GENUINE || pc < lo || pc >= hi) continue;
        int in_code = m->range_count == 0;
        for (unsigned k = 0; k < m->range_count; ++k) if (pc >= m->ranges[2u * k] && pc < m->ranges[2u * k + 1u]) in_code = 1;
        if (in_code) out[n++] = pc;
    }
    return n;
}
static int pm_dma(Run *r, uint32_t lsa, uint32_t bytes, int put) {
    r->guest_dirty = 1;
    (void)ps3_spu_executor_write_channel(r->owner, MFC_LSA, (u128){ ._u32 = { lsa, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(r->owner, MFC_EAL, (u128){ ._u32 = { 0x10000, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(r->owner, MFC_Size, (u128){ ._u32 = { bytes, 0, 0, 0 } });
    (void)ps3_spu_executor_write_channel(r->owner, MFC_TagID, (u128){ ._u32 = { 1, 0, 0, 0 } });
    return ps3_spu_executor_write_channel(r->owner, MFC_Cmd, (u128){ ._u32 = { put ? 0x20u : 0x40u, 0, 0, 0 } });
}
/* the arming run: the stub's job call, then a jump to the sentinel stop (no policy-module function runs) */
static int pm_arm_run(Run *r, const unsigned char *pristine, uint32_t window_call, uint32_t data_begin, int irq_mode) {
    pm_setup_trial(r, pristine, window_call, SENTINEL, 7, data_begin, irq_mode);
    return pm_run_lifted(r);
}
static int pm_edge(Run *L, Run *I, const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job, const unsigned char *pristine, uint32_t window_call, uint32_t data_begin) {
    int bad = 0; const char *tag = job ? "composite" : "alone";
    const unsigned last = pm->range_count - 1u;
    const uint32_t last_lo = pm->ranges[2u * last], last_hi = pm->ranges[2u * last + 1u];
    const uint32_t code_flip = last_lo + 0x40u, data_flip = g_pm_var_lo + 0x10u;
    /* 1. armed by the job call */
    (void)pm_arm_run(L, pristine, window_call, data_begin, 0);
    CHECK(L->last.kind == PS3_SPU_INTERP_STOPPED && L->last.code == STOP_SENTINEL, "%s %s: arming run ended kind=%d code=0x%X", pm->name, tag, L->last.kind, L->last.code);
    CHECK(L->stats.job_calls == 1 && L->stats.pm_tries == 1 && L->stats.pm_armed == 1 && L->stats.pm_mismatch == 0, "%s %s: calls=%llu tries=%llu armed=%llu mismatch=%llu", pm->name, tag,
          (unsigned long long)L->stats.job_calls, (unsigned long long)L->stats.pm_tries, (unsigned long long)L->stats.pm_armed, (unsigned long long)L->stats.pm_mismatch);
    CHECK(L->owner->watch_entry_bits != NULL && L->owner->watch_pm_count == pm->range_count && L->owner->watch_pm_lo[0] == pm->ranges[0] && L->owner->watch_pm_hi[last] == last_hi,
          "%s %s: watch not set (bits=%p ranges=%u)", pm->name, tag, (const void *)L->owner->watch_entry_bits, L->owner->watch_pm_count);
    if (job) CHECK(L->stats.job_armed == 1 && L->stats.pm_composites == 1 && L->owner->watch_code_hi == job->code_end, "%s composite: job_armed=%llu composites=%llu code_hi=0x%X", pm->name,
                   (unsigned long long)L->stats.job_armed, (unsigned long long)L->stats.pm_composites, L->owner->watch_code_hi);
    else CHECK(L->stats.job_armed == 0 && L->stats.job_unknown == 1 && L->owner->watch_code_hi == 0, "%s alone: job_armed=%llu unknown=%llu", pm->name, (unsigned long long)L->stats.job_armed, (unsigned long long)L->stats.job_unknown);
    /* 2. a DMA GET over the policy module's VARIABLES does not disarm; one over its code does (and a PUT never does) */
    { const unsigned long long disarms = L->owner->watch_disarms;
      int st = pm_dma(L, g_pm_var_lo, 128, 0);
      CHECK(st == PS3_SPU_EXEC_OK && L->owner->watch_pm_count == pm->range_count && L->owner->watch_disarms == disarms, "%s %s: GET over the variables disarmed (status %d count %u)", pm->name, tag, st, L->owner->watch_pm_count);
      L->refuse_writes = 0;
      st = pm_dma(L, last_lo + 0x100u, 128, 1);
      L->refuse_writes = 1;
      CHECK(st == PS3_SPU_EXEC_OK && L->owner->watch_pm_count == pm->range_count && L->owner->watch_disarms == disarms, "%s %s: PUT over the code disarmed (status %d)", pm->name, tag, st);
      st = pm_dma(L, (last_hi + 15u) & ~15u, 64, 0);
      CHECK(st == PS3_SPU_EXEC_OK && L->owner->watch_pm_count == pm->range_count && L->owner->watch_disarms == disarms, "%s %s: GET just behind the code disarmed", pm->name, tag);
      st = pm_dma(L, last_lo - 0x80u, 0x100, 0);      /* straddles what lies below the last code range and the start of that range */
      CHECK(st == PS3_SPU_EXEC_OK && L->owner->watch_pm_count == 0 && (L->owner->watch_dirty & 2u) && L->owner->watch_disarms == disarms + 1, "%s %s: GET into the code did not disarm (count %u dirty %u)", pm->name, tag, L->owner->watch_pm_count, L->owner->watch_dirty); }
    /* 3. a corrupted policy module (one bit of its code) is not lifted and the run equals the interpreter's; the job (when there is one) still is */
    pm_setup_trial(I, pristine, window_call, SENTINEL, 7, data_begin, 0);
    pm_setup_trial(L, pristine, window_call, SENTINEL, 7, data_begin, 0);
    L->owner->core.ls[code_flip] ^= 0x10; I->owner->core.ls[code_flip] ^= 0x10;
    { uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k); (void)pm_run_lifted(L); }
    CHECK(L->stats.pm_tries == 1 && L->stats.pm_mismatch == 1 && L->stats.pm_armed == 0 && L->owner->watch_pm_count == 0, "%s %s: a corrupted policy module was armed (tries=%llu mismatch=%llu armed=%llu)", pm->name, tag,
          (unsigned long long)L->stats.pm_tries, (unsigned long long)L->stats.pm_mismatch, (unsigned long long)L->stats.pm_armed);
    bad += (int)compare_runs(L, I, "corrupted policy module", CMP_QUIET, 1);
    /* 4. a changed VARIABLE is no change of identity */
    pm_setup_trial(L, pristine, window_call, SENTINEL, 7, data_begin, 0);
    L->owner->core.ls[data_flip] ^= 0x10;
    (void)pm_run_lifted(L);
    CHECK(L->stats.pm_armed == 1 && L->stats.pm_mismatch == 0, "%s %s: a changed variable changed the identity (armed=%llu mismatch=%llu)", pm->name, tag, (unsigned long long)L->stats.pm_armed, (unsigned long long)L->stats.pm_mismatch);
    /* 5. another signature: not a policy module this table knows (not even tried) */
    pm_setup_trial(L, pristine, window_call, SENTINEL, 7, data_begin, 0);
    L->owner->core.ls[PM_BASE] ^= 0x01;
    (void)pm_run_lifted(L);
    CHECK(L->stats.pm_tries == 0 && L->stats.pm_armed == 0 && L->owner->watch_pm_count == 0, "%s %s: another signature was tried (tries=%llu)", pm->name, tag, (unsigned long long)L->stats.pm_tries);
    /* 5b. armed by the branch to its entry (no job call before it); an unknown signature at that entry is not armed and the run equals the interpreter's */
    pm_setup_trial(L, pristine, PM_KERNEL_RET, pm->entry_pc, 7, data_begin, 0);
    (void)pm_run_lifted(L);
    CHECK(L->stats.pm_tries >= 1 && L->stats.pm_armed >= 1 && L->stats.pm_mismatch == 0 && L->stats.enters >= 1, "%s %s: not armed at its entry (tries=%llu armed=%llu mismatch=%llu enters=%llu)", pm->name, tag,
          (unsigned long long)L->stats.pm_tries, (unsigned long long)L->stats.pm_armed, (unsigned long long)L->stats.pm_mismatch, (unsigned long long)L->stats.enters);
    pm_setup_trial(I, pristine, PM_KERNEL_RET, pm->entry_pc, 7, data_begin, 0); pm_setup_trial(L, pristine, PM_KERNEL_RET, pm->entry_pc, 7, data_begin, 0);
    L->owner->core.ls[PM_BASE] ^= 0x01; I->owner->core.ls[PM_BASE] ^= 0x01;
    { uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k); (void)pm_run_lifted(L); }
    CHECK(L->stats.pm_tries == 0 && L->stats.pm_armed == 0 && L->stats.enters == 0, "%s %s: an unknown module was armed at its entry (tries=%llu armed=%llu enters=%llu)", pm->name, tag,
          (unsigned long long)L->stats.pm_tries, (unsigned long long)L->stats.pm_armed, (unsigned long long)L->stats.enters);
    bad += (int)compare_runs(L, I, "unknown module at the entry", CMP_QUIET, 1);
    { GuestDirty tmp; L->guest_dirty = I->guest_dirty = 1; guest_scan_restore(L, &tmp); guest_scan_restore(I, &tmp); }
    /* 6. an enabled interrupt pending at the job call: the interpreter takes it before any lifted code */
    pm_setup_trial(I, pristine, window_call, SENTINEL, 9, data_begin, 1); pm_setup_trial(L, pristine, window_call, SENTINEL, 9, data_begin, 1);
    I->owner->core.event_status = L->owner->core.event_status = 1;
    { uint64_t n; int k; run_interp_to_end(I, 400000, &n, &k); (void)pm_run_lifted(L); }
    CHECK(L->last.kind == PS3_SPU_INTERP_STOPPED && L->last.code == STOP_IRQ && L->stats.enters == 0, "%s %s: interrupt pending at the call: kind=%d code=0x%X enters=%llu", pm->name, tag, L->last.kind, L->last.code, (unsigned long long)L->stats.enters);
    bad += (int)compare_runs(L, I, "irq at the job call", CMP_QUIET, 1);
    /* 7. DMA inside lifted policy code: over the variables it is allowed, over the code it is a named terminal, never a silent run on stale code */
    (void)pm_arm_run(L, pristine, window_call, data_begin, 0);
    L->owner->watch_in_lifted = 1; L->owner->core.pc = last_lo + 0x400u;       /* the DMA instruction is in the policy module's code (the annotated channel operations leave pc exact) */
    { int st = pm_dma(L, g_pm_var_lo, 128, 0);
      CHECK(st == PS3_SPU_EXEC_OK && !L->owner->stopped && L->owner->watch_pm_count == pm->range_count, "%s %s: GET over the variables while lifted code runs: status=%d", pm->name, tag, st);
      if (job) {        /* the policy module loads the next job into the window: allowed, the job is forgotten */
          st = pm_dma(L, job->code_begin, 128, 0);
          CHECK(st == PS3_SPU_EXEC_OK && (L->owner->watch_dirty & 1u) && L->owner->watch_pm_count == pm->range_count, "%s %s: GET over the job from lifted policy code: status=%d dirty=%u", pm->name, tag, st, L->owner->watch_dirty); }
      st = pm_dma(L, last_lo + 0x400u, 128, 0);
      CHECK(st == PS3_SPU_EXEC_UNSUPPORTED && L->owner->terminal.channel == PS3_LIFTED_OP_CODE_CHANGED_DMA, "%s %s: GET over running lifted policy code: status=%d channel=0x%X", pm->name, tag, st, L->owner->terminal.channel); }
    L->owner->watch_in_lifted = 0;
    /* 8. the job window changes between two calls (composite): a GET over the job's code forgets the job, the policy module stays armed */
    if (job) {
        (void)pm_arm_run(L, pristine, window_call, data_begin, 0);
        const int st = pm_dma(L, job->code_begin, 128, 0);
        CHECK(st == PS3_SPU_EXEC_OK && (L->owner->watch_dirty & 1u) && L->owner->watch_code_hi == 0 && L->owner->watch_pm_count == pm->range_count, "%s composite: GET over the job: status=%d dirty=%u code_hi=0x%X pm_count=%u",
              pm->name, st, L->owner->watch_dirty, L->owner->watch_code_hi, L->owner->watch_pm_count);
    }
    { GuestDirty tmp; L->guest_dirty = I->guest_dirty = 1; guest_scan_restore(L, &tmp); guest_scan_restore(I, &tmp); }   /* the edge cases' own DMA must not reach the trials */
    printf("PM_EDGE %s %s ok (bad=%d failures=%d)\n", pm->name, tag, bad, failures);
    return bad;
}
/* --irq-resume for the policy module: as irq_resume_module (an event arrives at the n-th channel operation, the interrupt is taken, the state returns to srr0 and finishes: must equal an uninterrupted interpreter run) */
static int pm_irq_resume(Run *L, Run *I, const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job, const unsigned char *pristine, uint32_t window_call, uint32_t data_begin,
                         const uint32_t *pm_entries, unsigned pm_count, const uint32_t *job_entries, unsigned job_count, uint64_t trials, uint64_t seed, uint64_t cap, unsigned *taken_out) {
    unsigned bad = 0, taken = 0, via_escape = 0, compared = 0;
    for (uint64_t trial = 0; trial < trials && pm_count && bad < 3; ++trial) {
        const uint64_t trial_seed = seed + trial * 0x1000193u + 1u;
        const int job_call = job && job_count && g_pm_job_fn && trial % 3u == 0u;
        const uint32_t entry = job_call ? g_pm_job_fn : pm_entries[trial % pm_count], target = job_call ? job_entries[(trial / 3u) % job_count] : 0u;
        pm_setup_trial(I, pristine, window_call, entry, trial_seed, data_begin, 1);
        if (job_call) pm_poke_job_target(I->owner->core.ls, target);
        uint64_t instr; int kind; run_interp_to_end(I, cap, &instr, &kind);
        GuestDirty gi, gl; guest_scan_restore(I, &gi);
        if (kind != 0 || I->last.code == STOP_IRQ || I->channel_count == 0 || !pm_code_unchanged(I, pristine, pm, job)) continue;
        if (I->last.pc == 4 && I->last.code == 0) continue;
        { const unsigned char vector[4] = { 0, 0, 0, STOP_IRQ }; if (memcmp(I->owner->core.ls, vector, 4)) continue; }
        const unsigned total_ops = I->channel_count;
        pm_setup_trial(L, pristine, window_call, entry, trial_seed, data_begin, 1);
        if (job_call) pm_poke_job_target(L->owner->core.ls, target);
        L->inject_after = 1u + (unsigned)(trial_seed % total_ops);
        signal(SIGALRM, on_alarm); snprintf(g_current, sizeof g_current, "pm irq-resume %s entry 0x%05X target 0x%05X seed %llu", pm->name, entry, target, (unsigned long long)trial_seed); alarm(60);
        int rc = pm_run_lifted(L);
        spu_context *c = &L->owner->core;
        if (!rc && L->last.kind == PS3_SPU_INTERP_STOPPED && L->last.code == STOP_IRQ) {
            ++taken; if (L->stats.irq_escapes) ++via_escape;
            c->event_status = 0; c->int_enable = 1; c->pc = c->srr0; L->inject_after = 0;
            rc = pm_run_lifted(L);
        }
        alarm(0);
        guest_scan_restore(L, &gl);
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED &&
            (L->last.channel == PS3_SPU_EXEC_OP_INDIRECT_BRANCH || L->last.channel == PS3_SPU_EXEC_OP_CONTINUATION || L->last.channel == PS3_SPU_EXEC_OP_DRAIN_CALL)) continue;
        c->srr0 = I->owner->core.srr0; c->event_status = I->owner->core.event_status; c->int_enable = I->owner->core.int_enable;
        char label[160]; snprintf(label, sizeof label, "%s%s pm irq-resume(entry 0x%05X target 0x%05X seed %llu after op %u/%u)", pm->name, job ? "+job" : "", entry, target, (unsigned long long)trial_seed, L->inject_after, total_ops);
        const unsigned d = rc ? 1u : compare_runs(L, I, label, CMP_QUIET, 1) + guest_compare(&gl, &gi, label);
        if (rc) printf("DIFF %s lifted run failed rc=%d\n", label, rc);
        ++compared;
        if (d) { ++bad; printf("PM_IRQ_RESUME_MISMATCH %s\n", label); }
    }
    printf("PM_IRQ_RESUME %-28s%s compared=%u mismatched=%u interrupts_taken=%u (of which lifted code escaped for it: %u)\n", pm->name, job ? "+job" : "", compared, bad, taken, via_escape);
    if (taken_out) *taken_out += taken;
    return (int)bad;
}

/* Everything for one policy module of the table. `images` are the module bytes of the table's job modules (for the composites). */
static int policy_module(const char *spu_dir, const ps3_spu_lifted_module *pm, const unsigned char *pm_image, size_t pm_bytes, int edge, int irq_resume, uint64_t fuzz_trials, uint64_t fuzz_seed,
                         uint64_t fuzz_cap, int irq, int verbose, Stats *total) {
    int bad = 0;
    unsigned char *pristine = malloc(SPU_LS_SIZE);
    uint32_t window_call; pm_make_pristine(pristine, pm_image, pm_bytes, NULL, 0, 0, &window_call);
    CHECK(ps3_spu_lifted_module_hash(pristine, pm) == pm->code_hash_fast, "%s: descriptor hash does not match the module bytes", pm->name);
    { unsigned char *cat = malloc(SPU_LS_SIZE); size_t n = 0; for (unsigned i = 0; i < pm->range_count; ++i) { memcpy(cat + n, pristine + pm->ranges[2u * i], pm->ranges[2u * i + 1u] - pm->ranges[2u * i]); n += pm->ranges[2u * i + 1u] - pm->ranges[2u * i]; }
      CHECK(ps3_spu_lifted_fnv1a64(cat, n) == pm->code_hash, "%s: descriptor FNV hash does not match", pm->name); free(cat); }
    CHECK(pm->kind == PS3_LIFTED_KIND_POLICY && pm->range_count >= 1 && pm->range_count <= 4 && pm->code_begin == PM_BASE, "%s: descriptor shape", pm->name);
    /* where its variables are: between its first two code ranges, else behind the last one up to the end of its image */
    if (pm->range_count >= 2) { g_pm_var_lo = pm->ranges[1]; g_pm_var_hi = pm->ranges[2]; }
    else { g_pm_var_lo = (pm->ranges[1] + 15u) & ~15u; g_pm_var_hi = PM_BASE + (uint32_t)pm_bytes; }
    CHECK(g_pm_var_hi > g_pm_var_lo + 0x80u, "%s: no room for variables behind its code", pm->name);
    g_pm_job_fn = pm->entry_pc == PM_WWSJOB_ENTRY ? PM_WWSJOB_JOB_FN : 0u;
    uint32_t *entries = malloc(sizeof(uint32_t) * pm->row_count); unsigned count = pm_collect_entries(pm, PM_BASE, 0x40000u, entries, pm->row_count);
    printf("PM_MODULE %s size=0x%zX code ranges=%u entries=%u (of %u rows) entry=0x%X\n", pm->name, pm_bytes, pm->range_count, count, pm->row_count, pm->entry_pc);
    const uint32_t data_begin = 0x6000u;
    const ps3_spu_lifted_module *const set_pm[1] = { pm };
    Run L, I; run_open(&L, "lifted", 1, set_pm, 1); run_open(&I, "interp", 0, NULL, 0);
    pm_arena_fill(&L); pm_arena_fill(&I); guest_ref_init(&I);
    if (edge) bad += pm_edge(&L, &I, pm, NULL, pristine, window_call, data_begin);
    PmStats st; memset(&st, 0, sizeof st);
    if (fuzz_trials && !irq_resume) {
        bad += pm_fuzz(&L, &I, pm, NULL, pristine, window_call, data_begin, entries, count, NULL, 0, fuzz_trials, fuzz_seed, fuzz_cap, irq, verbose, &st);
        printf("PM_FUZZ %-28s alone     trials=%llu compared=%llu mismatched=%llu armed=%llu skipped(budget=%llu fault=%llu code_written=%llu known=%llu) entries=%u/%u ended(sentinel=%llu stop=%llu paused=%llu) instr=%llu speed=%.2fx lifted(enters=%llu escapes=%llu pause_escapes=%llu)\n",
               pm->name, (unsigned long long)st.trials, (unsigned long long)st.compared, (unsigned long long)st.mismatched, (unsigned long long)st.armed, (unsigned long long)st.skip_budget, (unsigned long long)st.skip_fault,
               (unsigned long long)st.skip_selfmod, (unsigned long long)st.skip_known, st.entries_compared, count, (unsigned long long)st.ended_sentinel, (unsigned long long)st.ended_stop, (unsigned long long)st.ended_paused,
               (unsigned long long)st.instr, st.t_lifted > 0 ? st.t_interp / st.t_lifted : 0.0, (unsigned long long)st.enters, (unsigned long long)st.escapes, (unsigned long long)st.pause_escapes);
        total->trials += st.trials; total->compared += st.compared; total->mismatched += st.mismatched; total->entries_compared += st.entries_compared;
    }
    if (irq_resume) bad += pm_irq_resume(&L, &I, pm, NULL, pristine, window_call, data_begin, entries, count, NULL, 0, fuzz_trials ? fuzz_trials : 300, fuzz_seed, fuzz_cap, NULL);
    run_close(&L); run_close(&I);
    /* ---- the composites: the policy module beside the three smallest job modules of the table that do not overlap its code, and the Edge zlib task image (below the window) when it does not */
    unsigned picked[4], picked_count = 0;
    for (unsigned round = 0; round < 4; ++round) {
        unsigned best = ~0u; uint32_t best_size = ~0u;
        for (unsigned mi = 0; mi < ps3_jobmods_count; ++mi) {
            const ps3_spu_lifted_module *j = ps3_jobmods_table[mi]; int taken = 0, overlap = 0;
            for (unsigned k = 0; k < picked_count; ++k) if (picked[k] == mi) taken = 1;
            for (unsigned k = 0; k < pm->range_count; ++k) if (j->code_begin < pm->ranges[2u * k + 1u] && pm->ranges[2u * k] < j->code_end) overlap = 1;
            const int task_image = j->code_begin < PM_WINDOW;
            if (taken || overlap || j->kind != PS3_LIFTED_KIND_JOB) continue;
            if (round < 3u ? (task_image || j->entry_pc < PM_WINDOW) : !task_image) continue;     /* rounds 0..2: the smallest ordinary jobs; round 3: the task image */
            if (j->code_end - j->code_begin < best_size) { best = mi; best_size = j->code_end - j->code_begin; }
        }
        if (best == ~0u) continue;
        picked[picked_count++] = best;
    }
    for (unsigned pi = 0; pi < picked_count && (edge || fuzz_trials || irq_resume); ++pi) {
        const ps3_spu_lifted_module *job = ps3_jobmods_table[picked[pi]];
        char path[1024]; snprintf(path, sizeof path, "%s/jobmods/%s.bin", spu_dir, job->name);
        FILE *f = fopen(path, "rb");
        if (!f && g_extra_dir) { snprintf(path, sizeof path, "%s/%s.bin", g_extra_dir, job->name); f = fopen(path, "rb"); }
        if (!f) { fprintf(stderr, "OPEN_FAILED %s\n", path); bad += 1; continue; }
        fseek(f, 0, SEEK_END); const long jsize = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *jimage = malloc((size_t)jsize); if (fread(jimage, 1, (size_t)jsize, f) != (size_t)jsize) { fprintf(stderr, "READ_FAILED\n"); return 1; } fclose(f);
        uint32_t window_call_c; pm_make_pristine(pristine, pm_image, pm_bytes, jimage, (size_t)jsize, job->code_begin, &window_call_c);
        g_pm_job_image_end = job->code_begin + (uint32_t)jsize;
        const uint32_t data_begin_c = (window_call_c + 0x1000u + 15u) & ~15u;
        uint32_t *job_entries = malloc(sizeof(uint32_t) * job->row_count); unsigned job_count = 0;
        for (unsigned i = 0; i < job->row_count; ++i) if (job->rows[i].flags == PS3_LIFTED_ROW_GENUINE && job->rows[i].pc >= job->code_begin + 0x30u && job->rows[i].pc >= PM_WINDOW && job->rows[i].pc < job->code_end) job_entries[job_count++] = job->rows[i].pc;
        const ps3_spu_lifted_module *const set_both[2] = { pm, job };
        Run Lc, Ic; run_open(&Lc, "lifted", 1, set_both, 2); run_open(&Ic, "interp", 0, NULL, 0);
        pm_arena_fill(&Lc); pm_arena_fill(&Ic);
        printf("PM_COMPOSITE %s + %s job entries=%u window_call=0x%X data=0x%X\n", pm->name, job->name, job_count, window_call_c, data_begin_c);
        if (edge) bad += pm_edge(&Lc, &Ic, pm, job, pristine, window_call_c, data_begin_c);
        if (irq_resume) bad += pm_irq_resume(&Lc, &Ic, pm, job, pristine, window_call_c, data_begin_c, entries, count, job_entries, job_count, fuzz_trials ? fuzz_trials : 300, fuzz_seed, fuzz_cap, NULL);
        else if (fuzz_trials) {
            PmStats sc; memset(&sc, 0, sizeof sc);
            bad += pm_fuzz(&Lc, &Ic, pm, job, pristine, window_call_c, data_begin_c, entries, count, job_entries, job_count, fuzz_trials, fuzz_seed + 77u, fuzz_cap, irq, verbose, &sc);
            printf("PM_FUZZ %-28s +%-22s trials=%llu compared=%llu mismatched=%llu armed=%llu composites=%llu job_hooks=%llu skipped(budget=%llu fault=%llu code_written=%llu known=%llu) ended(sentinel=%llu stop=%llu paused=%llu) instr=%llu lifted(enters=%llu escapes=%llu pause_escapes=%llu)\n",
                   pm->name, job->name, (unsigned long long)sc.trials, (unsigned long long)sc.compared, (unsigned long long)sc.mismatched, (unsigned long long)sc.armed, (unsigned long long)sc.composites, (unsigned long long)sc.job_hooks,
                   (unsigned long long)sc.skip_budget, (unsigned long long)sc.skip_fault, (unsigned long long)sc.skip_selfmod, (unsigned long long)sc.skip_known, (unsigned long long)sc.ended_sentinel, (unsigned long long)sc.ended_stop,
                   (unsigned long long)sc.ended_paused, (unsigned long long)sc.instr, (unsigned long long)sc.enters, (unsigned long long)sc.escapes, (unsigned long long)sc.pause_escapes);
            total->trials += sc.trials; total->compared += sc.compared; total->mismatched += sc.mismatched; total->entries_compared += sc.entries_compared;
        }
        run_close(&Lc); run_close(&Ic); free(jimage); free(job_entries); g_pm_job_image_end = 0;
    }
    { unsigned reached = 0, any = 0; char missing[2048]; for (unsigned i = 0; i < SPU_LS_SIZE / 4; ++i) any |= g_cov[i] != 0; size_t at = 0; missing[0] = 0;
      for (unsigned i = 0; i < count; ++i) { if (g_cov[entries[i] >> 2]) ++reached; else if (at + 12 < sizeof missing) at += (size_t)snprintf(missing + at, sizeof missing - at, " %X", entries[i]); }
      if (any) printf("PM_COVERAGE %s functions entered by lifted code: %u of %u; never entered:%s\n", pm->name, reached, count, missing); }
    free(entries); free(pristine);
    (void)verbose;
    return bad;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <spu dir> [options]\n", argv[0]); return 2; }
    const char *spu_dir = argv[1], *only = NULL, *extra_dir = NULL, *inflate_corpus = NULL; int inflate_mode = 0; unsigned inflate_synthetic = 300;
    uint64_t fuzz_trials = 0, fuzz_seed = 1, fuzz_cap = 400000, replay = 0; int verbose = 0, edge = 0, irq = 0, irq_resume = 0, rz = 0, trace = 0, policy_enabled = 1;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--extra") && i + 1 < argc) extra_dir = argv[++i];
        else if (!strcmp(argv[i], "--inflate")) { inflate_mode = 1; if (i + 1 < argc && argv[i + 1][0] != '-') inflate_corpus = argv[++i]; }
        else if (!strcmp(argv[i], "--inflate-synthetic") && i + 1 < argc) inflate_synthetic = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz_trials = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-seed") && i + 1 < argc) fuzz_seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-cap") && i + 1 < argc) fuzz_cap = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-replay") && i + 1 < argc) replay = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--edge")) edge = 1;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--trial") && i + 1 < argc) { char *colon; g_replay_entry = (uint32_t)strtoul(argv[++i], &colon, 0); g_replay_seed = strtoull(colon + 1, NULL, 0); fuzz_trials = 1; }
        else if (!strcmp(argv[i], "--irq")) irq = 1;
        else if (!strcmp(argv[i], "--irq-resume")) irq_resume = 1;
        else if (!strcmp(argv[i], "--fpmode") && i + 1 < argc) rz = !strcmp(argv[++i], "rz");
        else if (!strcmp(argv[i], "--no-policy")) policy_enabled = 0;      /* skip the policy module (default: tested with every option that applies) */
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (rz) _mm_setcsr(_mm_getcsr() | 0x6000u | 0x8000u | 0x0040u);   /* truncate + FTZ + DAZ: the SPU single-precision mode */
    printf("JOBMODS_TEST modules_in_table=%u fpmode=%s\n", ps3_jobmods_count, rz ? "rz+ftz+daz" : "host-default");
    int total_bad = 0; Stats total; memset(&total, 0, sizeof total);
    g_extra_dir = extra_dir;
    unsigned char *pristine = malloc(SPU_LS_SIZE);
    unsigned tested = 0;
#ifdef PS3_SPU_ESCAPE_DIAG
    {   /* the four accounting slots owner_start arms (1..4); the job slot (3) is the one the escape tables are filed under */
        const unsigned s1 = ps3_spu_acct_slot(PS3_SPU_ACCT_IMAGE, 1, 0, 0, "test image"), s2 = ps3_spu_acct_slot(PS3_SPU_ACCT_POLICY, 2, 0, 0, "test policy"),
                       s3 = ps3_spu_acct_slot(PS3_SPU_ACCT_JOB, 3, 0, 0, "test job"), s4 = ps3_spu_acct_slot(PS3_SPU_ACCT_OTHER, 4, 0, 0, "test other");
        CHECK(s1 == 1 && s2 == 2 && s3 == 3 && s4 == 4, "accounting slots %u %u %u %u", s1, s2, s3, s4);
        ps3_spu_acct_flat(3, 1, 1, 1, 1);
    }
#endif
    for (unsigned mi = 0; mi < ps3_jobmods_count; ++mi) {
        const ps3_spu_lifted_module *m = ps3_jobmods_table[mi];
        if (only && strncmp(m->name, only, strlen(only)) && !strstr(m->name, only)) continue;
        char path[1024]; snprintf(path, sizeof path, "%s/jobmods/%s.bin", spu_dir, m->name);
        FILE *f = fopen(path, "rb");
        if (!f && extra_dir) { snprintf(path, sizeof path, "%s/%s.bin", extra_dir, m->name); f = fopen(path, "rb"); }   /* a task image: written by the generator next to tasks.json */
        if (!f) { fprintf(stderr, "OPEN_FAILED %s\n", path); return 1; }
        fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *image = malloc((size_t)size); if (fread(image, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "READ_FAILED\n"); return 1; } fclose(f);
        if (m->kind == PS3_LIFTED_KIND_POLICY) {
            printf("MODULE %s size=0x%lX policy module, ranges=%u rows=%u entry=0x%X\n", m->name, size, m->range_count, m->row_count, m->entry_pc);
            if (policy_enabled) total_bad += policy_module(spu_dir, m, image, (size_t)size, edge, irq_resume, fuzz_trials, fuzz_seed, fuzz_cap, irq, verbose, &total);
            free(image); ++tested;
            continue;
        }
        make_pristine(pristine, image, (size_t)size, m->code_begin);
        const uint32_t image_end = m->code_begin + (uint32_t)size;
        const uint32_t data_begin = (image_end + 0x1800u + 15u) & ~15u;
        CHECK(ps3_spu_lifted_hash64w(pristine + m->code_begin, m->code_end - m->code_begin) == m->code_hash_fast, "%s: descriptor hash does not match the module bytes", m->name);
        CHECK(ps3_spu_lifted_fnv1a64(pristine + m->code_begin, m->code_end - m->code_begin) == m->code_hash, "%s: descriptor FNV hash does not match", m->name);
        printf("MODULE %s size=0x%lX code=[0x%X,0x%X) rows=%u entry=0x%X sig=%016llX%016llX\n", m->name, size, m->code_begin, m->code_end, m->row_count, m->entry_pc,
               (unsigned long long)m->sig_lo, (unsigned long long)m->sig_hi);
        const ps3_spu_lifted_module *const set[1] = { m };
        Run L, I;
        run_open(&L, "lifted", 1, set, 1); run_open(&I, "interp", 0, NULL, 0);
#ifdef PS3_SPU_NATIVE_INFLATE
        if (inflate_mode) {
            if (m->entry_pc == 0x3090u) { Run N; run_open(&N, "native", 1, set, 1); total_bad += inflate_test(&L, &N, &I, m, pristine, inflate_corpus, inflate_synthetic); run_close(&N); }
            run_close(&L); run_close(&I); free(image); ++tested;
            continue;
        }
#else
        (void)inflate_mode; (void)inflate_corpus; (void)inflate_synthetic;
#endif
        if (edge) total_bad += edge_cases(&L, &I, m, pristine, data_begin);
        if (irq_resume) total_bad += irq_resume_module(&L, &I, m, pristine, data_begin, fuzz_trials ? fuzz_trials : 300, fuzz_seed, fuzz_cap, NULL, NULL);
        if (trace && g_replay_entry) trace_trial(&L, &I, pristine, g_replay_entry, g_replay_seed, data_begin, 60000);
        else if (fuzz_trials && !irq_resume) total_bad += fuzz_module(&L, &I, m, pristine, data_begin, fuzz_trials, fuzz_seed, fuzz_cap, irq, verbose, (double)replay, &total);
        run_close(&L); run_close(&I); free(image); ++tested;
    }
    free(pristine);
#ifdef PS3_SPU_ESCAPE_DIAG
    if (tested) diag_verify(ps3_jobmods_table, ps3_jobmods_count);
#endif
    const int pass = !failures && !total_bad && tested;
    printf("JOBMODS_SUMMARY modules=%u trials=%llu compared=%llu mismatched=%llu entries_compared=%llu failures=%d\n", tested, (unsigned long long)total.trials,
           (unsigned long long)total.compared, (unsigned long long)total.mismatched, (unsigned long long)total.entries_compared, failures);
    printf("overall=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
