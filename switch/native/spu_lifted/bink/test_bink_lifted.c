/* Host differential test: the inFAMOUS Bink movie decoder SPU program, LIFTED (hybrid glue + the Windows port's lifted
 * code) against the native INTERPRETER, on the same ps3_spu_executor, from the same loaded local store.
 *
 * usage: test_bink_lifted <EBOOT.elf> <spu elf offset> [options]
 *   --mem-seed N      fill the guest memory window with a deterministic pattern (0 = zeros, default)
 *   --event A,B,C     data1,data2,data3 of the synthetic sys_spu_thread_receive_event event (default 0x002001A6,0,0)
 *   --events K        how many events to deliver after the first stop (default 1)
 *   --interp-step N   instructions per interpreter call in the differential (default 1: deliveries happen at the
 *                     instruction a lifted run would deliver them, so the channel logs are comparable)
 *   --bench K         speed phase: K events, lifted then interpreter, timed, no per-step comparison
 *   --no-bench-compare skip the final state comparison after the bench phase
 *   --verbose         print every DMA and the channel operations of each stage
 *   --fuzz N          function-level differential: N trials, each enters a genuine lifted function with random state
 *   --fuzz-seed S     base seed of the fuzz trials (default 1)
 *   --fuzz-cap I      interpreter instruction cap per trial (default 400000)
 *   --fuzz-replay S   after the fuzz, replay up to 48 heavy trials (>= 20000 instructions) from identical state until the
 *                     interpreter has used ~S seconds in total (the lifted side runs the same number of repetitions)
 * The SPU ELF is parsed IN PLACE from the user's own EBOOT.elf (nothing is written out of it) and loaded into a local
 * store by the lv2 SPU domain code, exactly as sys_spu_thread_initialize does. Both runs start from that local store.
 *
 * Output: one line per stage (BINK_STAGE ...), precise diffs on mismatch, and `overall=PASS` / `overall=FAIL`. */
#include "../../adapters/spu_executor.h"
#include "../../adapters/guard_pthread.h"
#include "../../tests/guest_space_host.h"
#include "../../domain/lv2_spu.h"
#include "bink_lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

enum { SPACE_BYTES = 16 * 1024 * 1024, LOG_MAX = 1u << 20, EVENT_MAX = 4096, CELL_ENOTCONN = 0x80010016u, CELL_EINVAL = 0x80010002u };

typedef struct { uint32_t kind, channel; } ChannelOp;
typedef struct { uint32_t lsa, bytes, command; uint64_t ea; } DmaOp;
typedef struct { uint32_t value, data0; } UserEvent;

typedef struct Run {
    const char *label;
    int lifted;
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[8];
    ps3_spu_executor *owner;
    ChannelOp *channel_log; unsigned channel_count;
    DmaOp *dma_log; unsigned dma_count;
    UserEvent events[EVENT_MAX]; unsigned event_count;
    unsigned checkpoints;
    uint64_t clock_reads;
    uint64_t interp_instructions;          /* instructions the interpreter executed (all of them for the interpreter run) */
    unsigned idle_waits;
    int refuse_writes, fail_waits;         /* fuzz mode: DMA PUTs are refused; a wait ends the owner at once */
    ps3_spu_lifted_services services;
    ps3_spu_lifted_stats stats;
    ps3_spu_interp_result last;
} Run;

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

/* ------------------------------------------------------------------------------------------------ run fixture */
static int service_checkpoint(void *v, unsigned phase) { (void)phase; ++((Run *)v)->checkpoints; return 0; }
static int service_access(void *v, uint64_t address, size_t bytes, int write) {
    if (write && ((Run *)v)->refuse_writes) return PS3_SPU_EXEC_ACCESS;
    return address < SPACE_BYTES && bytes <= SPACE_BYTES - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
static int service_reserve(void *v, const ps3_spu_transfer *t) {
    Run *r = v;
    if (r->dma_count < LOG_MAX) r->dma_log[r->dma_count] = (DmaOp){ t->lsa, t->bytes, t->command, t->ea };
    ++r->dma_count;
    return 0;
}
static void service_finish(void *v, const ps3_spu_transfer *t, ps3_guest_memory_status s, int c, const void *b) {
    (void)v; (void)t; (void)s; (void)c; (void)b;
}
static void observe_channel(void *v, unsigned kind, uint32_t channel) {
    Run *r = v;
    if (r->channel_count < LOG_MAX) r->channel_log[r->channel_count] = (ChannelOp){ kind, channel };
    ++r->channel_count;
}
/* Deterministic clock: the n-th read returns 1000 + 7n microseconds, so both runs see the same decrementer values as long
 * as they read the clock the same number of times (they do: it is read by RdDec and once at start). */
static ps3_clock_status clock_now(void *context, uint64_t *out) {
    Run *r = context;
    *out = 1000u + 7u * r->clock_reads++;
    return PS3_CLOCK_OK;
}

static uint64_t xorshift(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

/* Creates, observes and starts the owner of `r` (fresh state). The guest space and domain are kept. */
static void owner_start(Run *r) {
    ps3_spu_executor_services services = { r, service_access, service_checkpoint, service_reserve, service_finish };
    memset(r->owner, 0, sizeof *r->owner);
    r->clock_reads = 0; r->checkpoints = 0; r->channel_count = r->dma_count = r->event_count = 0; r->idle_waits = 0;
    if (ps3_spu_executor_init(r->owner, &r->domain, 0, services)) { fprintf(stderr, "OWNER_INIT_FAILED\n"); exit(1); }
    ps3_spu_executor_set_channel_observer(r->owner, observe_channel, r);
    ps3_clock clock = { r, clock_now };
    if (r->lifted) {
        int status = ps3_spu_lifted_start(r->owner, &ps3_bink_lifted_module, clock);
        if (status) { fprintf(stderr, "LIFTED_START_FAILED status=%d\n", status); exit(1); }
    } else {
        ps3_spu_executor_set_interpreter(r->owner);
        ps3_spu_executor_start_config config = { 0 };
        config.image_id = -1; config.clock = clock; config.thread_option = PS3_SPU_THREAD_OPTION_NONE; config.hop_budget = 1;
        int status = ps3_spu_executor_start(r->owner, &config);
        if (status) { fprintf(stderr, "INTERP_START_FAILED status=%d\n", status); exit(1); }
    }
}
/* Releases the owner (a fault leaves it stopped; a pending continuation is dropped first) and starts a fresh one. */
static void owner_restart(Run *r) {
    (void)ps3_spu_executor_take_continuation(r->owner);
    if (r->owner->initialized && ps3_spu_executor_release(r->owner)) { fprintf(stderr, "OWNER_RELEASE_FAILED\n"); exit(1); }
    owner_start(r);
}
/* The group runner's register setup (spu_group_runner.c set_argument): a doubleword in words 0 and 1. */
static void owner_load(Run *r, const unsigned char *ls, uint32_t entry, const uint64_t args[4]) {
    memcpy(r->owner->core.ls, ls, SPU_LS_SIZE);
    for (unsigned a = 0; a < 4; ++a) {
        r->owner->core.gpr[3 + a] = (u128){ 0 };
        r->owner->core.gpr[3 + a]._u32[0] = (uint32_t)(args[a] >> 32);
        r->owner->core.gpr[3 + a]._u32[1] = (uint32_t)args[a];
    }
    r->owner->core.pc = entry & (SPU_LS_SIZE - 1u);
    r->owner->core.status = SPU_STATUS_RUNNING;
}

static void run_open(Run *r, const char *label, int lifted, const unsigned char *ls, uint32_t entry, const uint64_t args[4],
                     uint64_t mem_seed) {
    memset(r, 0, sizeof *r);
    r->label = label; r->lifted = lifted;
    r->channel_log = calloc(LOG_MAX, sizeof *r->channel_log);
    r->dma_log = calloc(LOG_MAX, sizeof *r->dma_log);
    r->owner = calloc(1, sizeof *r->owner);
    r->space = elf_test_host_space_create(SPACE_BYTES);
    if (!r->channel_log || !r->dma_log || !r->owner || !r->space || ps3_pthread_guard_init(&r->guard)) { fprintf(stderr, "OPEN_FAILED\n"); exit(1); }
    r->backing = elf_test_host_space_port(r->space);
    if (ps3_atomic_domain_init(&r->domain, r->backing, ps3_pthread_guard_port(&r->guard), r->records, 8, NULL, 0) ||
        ps3_atomic_commit(&r->domain, 0, SPACE_BYTES)) { fprintf(stderr, "DOMAIN_FAILED\n"); exit(1); }
    if (mem_seed) {
        uint64_t s = mem_seed * 0x9E3779B97F4A7C15ull | 1u;
        static unsigned char chunk[65536];
        for (uint32_t base = 0; base < SPACE_BYTES; base += sizeof chunk) {
            for (size_t i = 0; i < sizeof chunk; i += 8) { uint64_t v = xorshift(&s); memcpy(chunk + i, &v, 8); }
            if (r->backing.write(r->backing.context, base, chunk, sizeof chunk)) { fprintf(stderr, "FILL_FAILED\n"); exit(1); }
        }
    }
    owner_start(r);
    owner_load(r, ls, entry, args);
}

static void run_close(Run *r) {
    (void)ps3_spu_executor_release(r->owner);
    free(r->owner); free(r->channel_log); free(r->dma_log);
    ps3_pthread_guard_destroy(&r->guard);
    elf_test_host_space_destroy(r->space);
}

/* ----------------------------------------------------------------------------------------------- the PPU model */
/* The runner's serve_out_interrupt_mailbox with no queue bound anywhere (every send answers ENOTCONN). */
static int serve_intr(Run *r) {
    uint32_t pending = 0;
    if (ps3_spu_executor_mbox_pending(r->owner, PS3_SPU_MAILBOX_OUT_INTR, &pending) != PS3_SPU_EXEC_OK || !pending) return 0;
    uint32_t value = 0, data0 = 0;
    if (ps3_spu_executor_mbox_receive(r->owner, PS3_SPU_MAILBOX_OUT_INTR, &value) != PS3_SPU_EXEC_OK) return 0;
    const uint32_t code = value >> 24;
    if (code < 128u) {
        (void)ps3_spu_executor_mbox_receive(r->owner, PS3_SPU_MAILBOX_OUT, &data0);
        if (r->event_count < EVENT_MAX) r->events[r->event_count] = (UserEvent){ value, data0 };
        ++r->event_count;
        if (code < 64u) (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, CELL_ENOTCONN);
    } else if (code == 128u || code == 192u) {
        (void)ps3_spu_executor_mbox_receive(r->owner, PS3_SPU_MAILBOX_OUT, &data0);
        if (code == 128u) (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, CELL_EINVAL);
    } else {
        (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, CELL_EINVAL);
    }
    return 1;
}
static int lifted_wait(void *context, spu_context *ctx, uint32_t channel) {
    Run *r = context; (void)ctx; (void)channel;
    if (serve_intr(r)) { r->idle_waits = 0; return 0; }
    if (r->fail_waits) return 1;
    if (++r->idle_waits > 5000u) return 1;      /* nobody can satisfy it: end the owner (reported as CANCELLED) */
    struct timespec t = { 0, 50000 }; nanosleep(&t, NULL);
    return 0;
}
static void lifted_intr_written(void *context, spu_context *ctx) { (void)ctx; (void)serve_intr(context); }

/* sys_spu_thread_receive_event as the group runner serves it: pop the queue number, answer OK + three data words. */
static int serve_receive_event(Run *r, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *spuq) {
    uint32_t q = 0;
    const int popped = ps3_spu_executor_mbox_receive(r->owner, PS3_SPU_MAILBOX_OUT, &q);
    if (spuq) *spuq = q;
    (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, 0);
    (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, d1);
    (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, d2);
    (void)ps3_spu_executor_mbox_send(r->owner, PS3_SPU_MAILBOX_IN, d3);
    return popped;
}

/* Runs `r` until it stops (any stop code), faults, or makes no progress. Returns 0 and fills r->last. */
static int run_to_stop(Run *r, uint64_t interp_step) {
    unsigned stalls = 0;
    for (;;) {
        ps3_spu_interp_result res;
        int status;
        if (r->lifted) {
            r->services = (ps3_spu_lifted_services){ .context = r, .wait = lifted_wait, .intr_written = lifted_intr_written };
            status = ps3_spu_lifted_run_slice(r->owner, &ps3_bink_lifted_module, &r->services, interp_step > 1 ? interp_step : 200000,
                                              &res, &r->stats);
        } else {
            status = ps3_spu_executor_run_interp(r->owner, r->owner->core.pc, interp_step, &res);
            if (status == PS3_SPU_EXEC_OK) r->interp_instructions += res.instructions;
        }
        if (status != PS3_SPU_EXEC_OK) { fprintf(stderr, "%s: slice refused status=%d\n", r->label, status); return status ? status : 1; }
        r->last = res;
        switch (res.kind) {
        case PS3_SPU_INTERP_BUDGET:
            (void)serve_intr(r); stalls = 0;
            break;
        case PS3_SPU_INTERP_PAUSED:
            if (serve_intr(r)) stalls = 0;
            else {
                if (++stalls > 20000u) { fprintf(stderr, "%s: paused on channel %u with nobody to serve it\n", r->label, res.channel); return 1; }
                struct timespec t = { 0, 20000 }; nanosleep(&t, NULL);
            }
            break;
        case PS3_SPU_INTERP_STOPPED:
        case PS3_SPU_INTERP_FAULT:
            (void)serve_intr(r);
            return 0;
        }
    }
}

/* -------------------------------------------------------------------------------------------------- comparison */
static uint64_t fnv(const unsigned char *p, size_t n) { return ps3_spu_lifted_fnv1a64(p, n); }

enum { CMP_CPU = 1, CMP_GUEST = 2, CMP_QUIET = 4, CMP_NO_CHANNEL_LOG = 8 };   /* the interpreter re-logs a channel op it retries after a PAUSE */
static unsigned compare_runs(Run *a, Run *b, const char *stage, int flags) {
    const int compare_cpu_state = flags & CMP_CPU;
    unsigned diffs = 0;
    const spu_context *ca = &a->owner->core, *cb = &b->owner->core;
    if (a->last.kind != b->last.kind || a->last.pc != b->last.pc || a->last.code != b->last.code ||
        (a->last.kind == PS3_SPU_INTERP_FAULT && (a->last.channel != b->last.channel || a->last.stop_code != b->last.stop_code ||
                                                  a->last.opcode != b->last.opcode))) {
        printf("DIFF %s result: lifted kind=%d pc=0x%05X code=0x%X channel=%u stop=0x%X opcode=0x%08X | interp kind=%d pc=0x%05X code=0x%X channel=%u stop=0x%X opcode=0x%08X\n",
               stage, a->last.kind, a->last.pc, a->last.code, a->last.channel, a->last.stop_code, a->last.opcode,
               b->last.kind, b->last.pc, b->last.code, b->last.channel, b->last.stop_code, b->last.opcode);
        ++diffs;
    }
    if (compare_cpu_state) {
        if ((ca->pc & SPU_LS_MASK) != (cb->pc & SPU_LS_MASK)) { printf("DIFF %s pc: lifted 0x%05X interp 0x%05X\n", stage, ca->pc, cb->pc); ++diffs; }
        unsigned shown = 0, reg_diffs = 0;
        for (unsigned r = 0; r < 128; ++r)
            for (unsigned w = 0; w < 4; ++w)
                if (ca->gpr[r]._u32[w] != cb->gpr[r]._u32[w]) {
                    ++reg_diffs;
                    if (shown++ < 24) printf("DIFF %s r%u.w%u: lifted 0x%08X interp 0x%08X\n", stage, r, w, ca->gpr[r]._u32[w], cb->gpr[r]._u32[w]);
                }
        if (reg_diffs) { printf("DIFF %s registers: %u words differ\n", stage, reg_diffs); ++diffs; }
        size_t ls_diff_bytes = 0; unsigned ranges = 0;
        for (size_t i = 0; i < SPU_LS_SIZE;) {
            if (ca->ls[i] == cb->ls[i]) { ++i; continue; }
            size_t start = i; while (i < SPU_LS_SIZE && ca->ls[i] != cb->ls[i]) ++i;
            ls_diff_bytes += i - start;
            if (ranges++ < 12) printf("DIFF %s local store [0x%05zX,0x%05zX) %zu bytes (lifted %02X.. interp %02X..)\n", stage, start, i, i - start, ca->ls[start], cb->ls[start]);
        }
        if (ls_diff_bytes) { printf("DIFF %s local store: %zu bytes in %u ranges differ\n", stage, ls_diff_bytes, ranges); ++diffs; }
        if (ca->ch_in_mbox.count != cb->ch_in_mbox.count || ca->ch_out_mbox.count != cb->ch_out_mbox.count ||
            ca->ch_out_intr_mbox.count != cb->ch_out_intr_mbox.count) {
            printf("DIFF %s mailboxes: in %u/%u out %u/%u intr %u/%u (lifted/interp)\n", stage, ca->ch_in_mbox.count, cb->ch_in_mbox.count,
                   ca->ch_out_mbox.count, cb->ch_out_mbox.count, ca->ch_out_intr_mbox.count, cb->ch_out_intr_mbox.count);
            ++diffs;
        } else {
            for (unsigned i = 0; i < ca->ch_out_mbox.count; ++i)
                if (ca->ch_out_mbox.q[(ca->ch_out_mbox.head + i) % SPU_CHANNEL_CAP] != cb->ch_out_mbox.q[(cb->ch_out_mbox.head + i) % SPU_CHANNEL_CAP]) {
                    printf("DIFF %s out mailbox word %u differs\n", stage, i); ++diffs; }
        }
        if (ca->event_mask != cb->event_mask || ca->event_status != cb->event_status || ca->int_enable != cb->int_enable || ca->srr0 != cb->srr0) {
            printf("DIFF %s events: mask %X/%X status %X/%X int_enable %u/%u srr0 %X/%X\n", stage, ca->event_mask, cb->event_mask, ca->event_status,
                   cb->event_status, ca->int_enable, cb->int_enable, ca->srr0, cb->srr0);
            ++diffs;
        }
    }
    /* Channel operations, DMA, user events: the same, in the same order. */
    if (!(flags & CMP_NO_CHANNEL_LOG) && a->channel_count != b->channel_count) { printf("DIFF %s channel ops: lifted %u interp %u\n", stage, a->channel_count, b->channel_count); ++diffs; }
    for (unsigned i = 0; !(flags & CMP_NO_CHANNEL_LOG) && i < a->channel_count && i < b->channel_count && i < LOG_MAX; ++i)
        if (a->channel_log[i].kind != b->channel_log[i].kind || a->channel_log[i].channel != b->channel_log[i].channel) {
            printf("DIFF %s channel op #%u: lifted (%u,%u) interp (%u,%u)\n", stage, i, a->channel_log[i].kind, a->channel_log[i].channel,
                   b->channel_log[i].kind, b->channel_log[i].channel); ++diffs; break; }
    if (a->dma_count != b->dma_count) { printf("DIFF %s DMA count: lifted %u interp %u\n", stage, a->dma_count, b->dma_count); ++diffs; }
    for (unsigned i = 0; i < a->dma_count && i < b->dma_count && i < LOG_MAX; ++i)
        if (memcmp(&a->dma_log[i], &b->dma_log[i], sizeof(DmaOp))) {
            printf("DIFF %s DMA #%u: lifted lsa=0x%05X ea=0x%llX bytes=%u cmd=0x%X | interp lsa=0x%05X ea=0x%llX bytes=%u cmd=0x%X\n", stage, i,
                   a->dma_log[i].lsa, (unsigned long long)a->dma_log[i].ea, a->dma_log[i].bytes, a->dma_log[i].command,
                   b->dma_log[i].lsa, (unsigned long long)b->dma_log[i].ea, b->dma_log[i].bytes, b->dma_log[i].command); ++diffs; break; }
    if (a->event_count != b->event_count) { printf("DIFF %s user events: lifted %u interp %u\n", stage, a->event_count, b->event_count); ++diffs; }
    for (unsigned i = 0; i < a->event_count && i < b->event_count && i < EVENT_MAX; ++i)
        if (memcmp(&a->events[i], &b->events[i], sizeof(UserEvent))) { printf("DIFF %s user event #%u differs\n", stage, i); ++diffs; break; }
    /* Guest memory (the window both runs may have written with PUTs). */
    static unsigned char ma[1 << 20], mb[1 << 20];
    for (uint32_t base = 0; (flags & CMP_GUEST) && base < SPACE_BYTES; base += sizeof ma) {
        if (a->backing.read(a->backing.context, base, ma, sizeof ma) || b->backing.read(b->backing.context, base, mb, sizeof mb)) { ++diffs; break; }
        if (memcmp(ma, mb, sizeof ma)) {
            size_t k = 0; while (ma[k] == mb[k]) ++k;
            printf("DIFF %s guest memory at EA 0x%llX\n", stage, (unsigned long long)base + k); ++diffs; break;
        }
    }
    if (!(diffs == 0 && (flags & CMP_QUIET)))
    printf("BINK_STAGE %s lifted: kind=%d pc=0x%05X code=0x%X dma=%u chops=%u events=%u | interp: kind=%d pc=0x%05X code=0x%X dma=%u chops=%u events=%u | diffs=%u\n",
           stage, a->last.kind, a->last.pc, a->last.code, a->dma_count, a->channel_count, a->event_count,
           b->last.kind, b->last.pc, b->last.code, b->dma_count, b->channel_count, b->event_count, diffs);
    (void)fnv;
    return diffs;
}

/* ---------------------------------------------------------------------------------------------------- loading */
static unsigned char *read_range(const char *path, size_t offset, size_t length) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "OPEN_FAILED %s\n", path); exit(1); }
    unsigned char *buffer = calloc(1, length);
    if (!buffer || fseek(f, (long)offset, SEEK_SET) || fread(buffer, 1, length, f) != length) { fprintf(stderr, "READ_FAILED\n"); exit(1); }
    fclose(f);
    return buffer;
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const unsigned char *p) { return (uint32_t)p[0] << 8 | p[1]; }

static double seconds_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }


/* ------------------------------------------------------------------------------------------------ function fuzz */
/* Function-level differential: every genuine lifted function is entered with random registers and a randomised
 * scratch area, with $lr pointing at a `stop 0x3FFF` sentinel. The interpreter runs first (instruction cap); only a
 * run that ends at a stop is replayed lifted, and the two end states (registers, local store, mailboxes, channel
 * operations, DMA) must be identical. DMA PUTs are refused (guest memory stays the same for every trial) and an
 * unsatisfiable wait ends the trial. A trial the interpreter does not finish is skipped, never guessed. */
enum { FUZZ_DATA_BEGIN = 0x18400, FUZZ_DATA_END = 0x3E000, FUZZ_SENTINEL = 0x3FFE0, FUZZ_STACK = 0x3F800 };
typedef struct { uint32_t entry; uint64_t seed, instr; } HeavyTrial;
enum { HEAVY_KEEP = 48, HEAVY_MIN_INSTR = 20000 };
typedef struct {
    uint64_t trials, compared, mismatched, skip_budget, skip_fault, skip_paused, skip_error, skip_selfmod, skip_ls0, known_ls0, known_drain, instr_total, heavy_trials, heavy_instr;
    uint64_t stopped_sentinel, stopped_other, entries_compared;
    double t_lifted, t_interp, t_lifted_heavy, t_interp_heavy;
    uint64_t lifted_enters, lifted_escapes, lifted_interp_instr, lifted_stops;
} FuzzStats;

static char g_fuzz_current[128];
static void fuzz_on_alarm(int sig) {
    (void)sig;
    static const char prefix[] = "\nBINK_FUZZ_HANG a lifted run did not finish in time: ";
    (void)!write(1, prefix, sizeof prefix - 1);
    (void)!write(1, g_fuzz_current, strlen(g_fuzz_current));
    (void)!write(1, "\n", 1);
    _exit(3);
}

static void fuzz_setup(Run *r, const unsigned char *pristine, uint32_t entry, uint64_t seed) {
    owner_restart(r);
    r->refuse_writes = 1; r->fail_waits = 1;
    spu_context *c = &r->owner->core;
    memcpy(c->ls, pristine, SPU_LS_SIZE);
    uint64_t s = seed * 0x9E3779B97F4A7C15ull | 1u;
    for (uint32_t a = FUZZ_DATA_BEGIN; a < FUZZ_DATA_END; a += 8) { uint64_t v = xorshift(&s); memcpy(c->ls + a, &v, 8); }
    memset(c->ls + FUZZ_DATA_END, 0, SPU_LS_SIZE - FUZZ_DATA_END);
    c->ls[FUZZ_SENTINEL + 2] = 0x3F; c->ls[FUZZ_SENTINEL + 3] = 0xFF;      /* stop 0x3FFF */
    for (unsigned reg = 0; reg < 128; ++reg)
        for (unsigned w = 0; w < 4; ++w) {
            const uint64_t v = xorshift(&s);
            uint32_t word;
            switch (v & 3u) {
            case 0: word = (uint32_t)(v >> 8) & 0xFFu; break;                                                  /* small */
            case 1: word = FUZZ_DATA_BEGIN + (((uint32_t)(v >> 8) % (FUZZ_DATA_END - FUZZ_DATA_BEGIN - 0x1000u)) & ~0xFu); break;  /* LS pointer */
            case 2: word = (uint32_t)(v >> 8); break;                                                          /* anything */
            default: word = 0; break;
            }
            c->gpr[reg]._u32[w] = word;
        }
    c->gpr[0] = (u128){ 0 }; c->gpr[0]._u32[0] = FUZZ_SENTINEL;
    c->gpr[1] = (u128){ 0 }; c->gpr[1]._u32[0] = FUZZ_STACK;
    for (unsigned k = 0; k < 4; ++k) spu_channel_write(&c->ch_in_mbox, (uint32_t)xorshift(&s));
    c->pc = entry; c->status = SPU_STATUS_RUNNING; c->stop_code = 0;
}

static int fuzz(Run *L, Run *I, const unsigned char *pristine, uint64_t trials, uint64_t seed, uint64_t cap, int verbose, uint64_t replay_seconds) {
    FuzzStats st; memset(&st, 0, sizeof st);
    HeavyTrial heavy[HEAVY_KEEP]; unsigned heavy_count = 0;
    const ps3_spu_lifted_module *m = &ps3_bink_lifted_module;
    unsigned genuine[1024]; unsigned genuine_count = 0;
    for (unsigned i = 0; i < m->row_count && genuine_count < 1024; ++i) if (m->rows[i].flags == PS3_LIFTED_ROW_GENUINE) genuine[genuine_count++] = i;   /* not stubs, not lossy */
    unsigned char *seen = calloc(genuine_count, 1);
    for (uint64_t trial = 0; trial < trials && st.mismatched < 5; ++trial) {
        const unsigned gi = (unsigned)(trial % genuine_count);
        const uint32_t entry = m->rows[genuine[gi]].pc;
        const uint64_t trial_seed = seed + trial * 0x1000193u + 1u;
        ++st.trials;
        fuzz_setup(I, pristine, entry, trial_seed);
        const double ti0 = seconds_now();
        uint64_t instr = 0; int kind = -1;
        for (;;) {
            ps3_spu_interp_result res;
            if (ps3_spu_executor_run_interp(I->owner, I->owner->core.pc, 100000, &res)) { kind = 9; break; }
            instr += res.instructions; I->last = res;
            if (res.kind == PS3_SPU_INTERP_STOPPED) { (void)serve_intr(I); kind = 0; break; }
            if (res.kind == PS3_SPU_INTERP_FAULT) { kind = 1; break; }
            if (res.kind == PS3_SPU_INTERP_PAUSED) { kind = 2; break; }
            if (instr >= cap) { kind = 3; break; }
        }
        const double ti = seconds_now() - ti0;
        if (kind == 0) {
            /* The lifted code is the static program: a trial that stored into the code range (random pointers do) would
             * run different instructions on the two sides. Likewise a jump to LS 0, where the interpreter executes the
             * word there (`stop 0`) and the boundary refuses the hop (spu_lifted_boundary.c hop: pc 0). Both are skipped. */
            const ps3_spu_lifted_module *mod = &ps3_bink_lifted_module;
            if (memcmp(I->owner->core.ls + mod->code_begin, pristine + mod->code_begin, mod->code_end - mod->code_begin)) { ++st.skip_selfmod; continue; }
            if (I->last.pc == 4 && I->last.code == 0) { ++st.skip_ls0; continue; }
        }
        if (kind != 0) { if (kind == 1) ++st.skip_fault; else if (kind == 2) ++st.skip_paused; else if (kind == 3) ++st.skip_budget; else ++st.skip_error; continue; }
        fuzz_setup(L, pristine, entry, trial_seed);
        snprintf(g_fuzz_current, sizeof g_fuzz_current, "entry 0x%05X seed %llu interp_instructions %llu interp_end pc=0x%05X stop=0x%X", entry, (unsigned long long)trial_seed, (unsigned long long)instr, I->last.pc, I->last.code);
        signal(SIGALRM, fuzz_on_alarm);
        alarm(getenv("BINK_FUZZ_ALARM") ? (unsigned)atoi(getenv("BINK_FUZZ_ALARM")) : 60u);
        const double tl0 = seconds_now();
        const int rc = run_to_stop(L, 200000);
        const double tl = seconds_now() - tl0;
        alarm(0);
        /* Known divergence: a branch to LS 0 or to a misaligned address. The interpreter masks the low two bits and executes
         * whatever is there; the boundary refuses a hop to pc 0 (spu_lifted_boundary.c) and an indirect branch to an address
         * with no row, so the lifted owner ends with UNSUPPORTED / OP_INDIRECT_BRANCH. Compiled code never does this. */
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED &&
            (L->last.channel == PS3_SPU_EXEC_OP_INDIRECT_BRANCH || L->last.channel == PS3_SPU_EXEC_OP_CONTINUATION) &&
            ((L->owner->core.pc & 3u) || !(L->owner->core.pc & SPU_LS_MASK))) { ++st.known_ls0; continue; }
        /* Known divergence: a callee returned to a pc other than the one its call bracket expects, with nothing pending
         * (garbage link register). The boundary's spu_drain_call ends the owner (OP_DRAIN_CALL); the interpreter goes on. */
        if (!rc && L->last.kind == PS3_SPU_INTERP_FAULT && L->last.code == PS3_SPU_EXEC_UNSUPPORTED && L->last.channel == PS3_SPU_EXEC_OP_DRAIN_CALL) { ++st.known_drain; continue; }
        char label[96]; snprintf(label, sizeof label, "fuzz(entry 0x%05X seed %llu)", entry, (unsigned long long)trial_seed);
        const unsigned diffs = rc ? 1u : compare_runs(L, I, label, CMP_CPU | CMP_QUIET);
        if (rc) printf("DIFF %s lifted run failed rc=%d\n", label, rc);
        ++st.compared; st.instr_total += instr; st.t_lifted += tl; st.t_interp += ti;
        if (I->last.code == 0x3FFF) ++st.stopped_sentinel; else ++st.stopped_other;
        if (!seen[gi]) { seen[gi] = 1; ++st.entries_compared; }
        if (instr >= HEAVY_MIN_INSTR && !diffs && heavy_count < HEAVY_KEEP) heavy[heavy_count++] = (HeavyTrial){ entry, trial_seed, instr };
        if (instr >= 20000) { ++st.heavy_trials; st.heavy_instr += instr; st.t_lifted_heavy += tl; st.t_interp_heavy += ti; }
        st.lifted_enters += L->stats.enters; st.lifted_escapes += L->stats.escapes; st.lifted_interp_instr += L->stats.interp_instructions; st.lifted_stops += L->stats.stops;
        memset(&L->stats, 0, sizeof L->stats);
        if (diffs) { ++st.mismatched; printf("FUZZ_MISMATCH entry=0x%05X seed=%llu interp_instructions=%llu\n", entry, (unsigned long long)trial_seed, (unsigned long long)instr); }
        else if (verbose) printf("FUZZ_OK entry=0x%05X instr=%llu end=%s/0x%X\n", entry, (unsigned long long)instr, I->last.code == 0x3FFF ? "sentinel" : "stop", I->last.code);
    }
    free(seen);
    printf("FUZZ trials=%llu compared=%llu mismatched=%llu skipped(budget=%llu fault=%llu paused=%llu error=%llu self_modified_code=%llu branch_to_ls0=%llu+%llu_known_divergence unmatched_return_known_divergence=%llu) entries_compared=%llu/%u "
           "ended_sentinel=%llu ended_other_stop=%llu\n", (unsigned long long)st.trials, (unsigned long long)st.compared, (unsigned long long)st.mismatched,
           (unsigned long long)st.skip_budget, (unsigned long long)st.skip_fault, (unsigned long long)st.skip_paused, (unsigned long long)st.skip_error,
           (unsigned long long)st.skip_selfmod, (unsigned long long)st.skip_ls0, (unsigned long long)st.known_ls0, (unsigned long long)st.known_drain,
           (unsigned long long)st.entries_compared, genuine_count, (unsigned long long)st.stopped_sentinel, (unsigned long long)st.stopped_other);
    printf("FUZZ_SPEED compared_instructions=%llu lifted_seconds=%.4f interp_seconds=%.4f speedup=%.2fx lifted_equiv_mips=%.1f interp_mips=%.1f "
           "lifted_enters=%llu escapes=%llu bridge_interp_instructions=%llu\n", (unsigned long long)st.instr_total, st.t_lifted, st.t_interp,
           st.t_lifted > 0 ? st.t_interp / st.t_lifted : 0.0, st.t_lifted > 0 ? (double)st.instr_total / st.t_lifted / 1e6 : 0.0,
           st.t_interp > 0 ? (double)st.instr_total / st.t_interp / 1e6 : 0.0, (unsigned long long)st.lifted_enters, (unsigned long long)st.lifted_escapes,
           (unsigned long long)st.lifted_interp_instr);
    printf("FUZZ_SPEED_HEAVY trials(>=20000 instr)=%llu instructions=%llu lifted_seconds=%.4f interp_seconds=%.4f speedup=%.2fx lifted_equiv_mips=%.1f interp_mips=%.1f\n",
           (unsigned long long)st.heavy_trials, (unsigned long long)st.heavy_instr, st.t_lifted_heavy, st.t_interp_heavy,
           st.t_lifted_heavy > 0 ? st.t_interp_heavy / st.t_lifted_heavy : 0.0,
           st.t_lifted_heavy > 0 ? (double)st.heavy_instr / st.t_lifted_heavy / 1e6 : 0.0,
           st.t_interp_heavy > 0 ? (double)st.heavy_instr / st.t_interp_heavy / 1e6 : 0.0);
    /* Stable timing of the heavy trials: each is replayed from identical state, many times, with only the run timed. */
    if (replay_seconds && heavy_count && !st.mismatched) {
        double tl_sum = 0, ti_sum = 0; uint64_t instr_sum = 0, reps_total = 0;
        printf("FUZZ_REPLAY %u heavy trials (>= %d interpreter instructions), ~%llu s of interpreter time in total\n", heavy_count, HEAVY_MIN_INSTR, (unsigned long long)replay_seconds);
        for (unsigned h = 0; h < heavy_count; ++h) {
            double tl = 0, ti = 0; unsigned reps = 0;
            const double budget = (double)replay_seconds / heavy_count;
            while (ti < budget || reps < 3) {          /* the interpreter is the slower side: it sets the time spent */
                fuzz_setup(I, pristine, heavy[h].entry, heavy[h].seed);
                const double a0 = seconds_now();
                for (;;) {
                    ps3_spu_interp_result res;
                    if (ps3_spu_executor_run_interp(I->owner, I->owner->core.pc, 100000, &res) || res.kind != PS3_SPU_INTERP_BUDGET) break;
                }
                ti += seconds_now() - a0;
                fuzz_setup(L, pristine, heavy[h].entry, heavy[h].seed);
                const double b0 = seconds_now();
                (void)run_to_stop(L, 200000);
                tl += seconds_now() - b0;
                ++reps;
                if (reps > 200000) break;
            }
            printf("FUZZ_REPLAY_TRIAL entry=0x%05X instructions=%llu reps=%u lifted_us=%.2f interp_us=%.2f speedup=%.2fx lifted_equiv_mips=%.0f interp_mips=%.0f\n",
                   heavy[h].entry, (unsigned long long)heavy[h].instr, reps, tl / reps * 1e6, ti / reps * 1e6, ti / tl,
                   (double)heavy[h].instr * reps / tl / 1e6, (double)heavy[h].instr * reps / ti / 1e6);
            tl_sum += tl; ti_sum += ti; instr_sum += heavy[h].instr * reps; reps_total += reps;
        }
        printf("FUZZ_REPLAY_TOTAL instructions=%llu reps=%llu lifted_seconds=%.3f interp_seconds=%.3f speedup=%.2fx lifted_equiv_mips=%.0f interp_mips=%.0f\n",
               (unsigned long long)instr_sum, (unsigned long long)reps_total, tl_sum, ti_sum, ti_sum / tl_sum,
               (double)instr_sum / tl_sum / 1e6, (double)instr_sum / ti_sum / 1e6);
    }
    return st.mismatched ? 1 : 0;
}

static const char *stop_name(uint32_t code) {
    switch (code) { case 0x100: return "0x100"; case 0x101: return "group-exit"; case 0x102: return "thread-exit"; case 0x110: return "receive-event"; default: return "other"; }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <EBOOT.elf> <spu elf offset> [options]\n", argv[0]); return 2; }
    const char *path = argv[1];
    const size_t offset = (size_t)strtoull(argv[2], NULL, 0);
    uint64_t mem_seed = 0, bench = 0, interp_step = 1;
    uint32_t event[3] = { 0x002001A6u, 0, 0 };
    unsigned events = 1;
    int bench_compare = 1, verbose = 0;
    uint64_t fuzz_trials = 0, fuzz_seed = 1, fuzz_cap = 400000, fuzz_replay = 0;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--mem-seed") && i + 1 < argc) mem_seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--event") && i + 1 < argc) { sscanf(argv[++i], "%i,%i,%i", (int *)&event[0], (int *)&event[1], (int *)&event[2]); }
        else if (!strcmp(argv[i], "--events") && i + 1 < argc) events = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--interp-step") && i + 1 < argc) interp_step = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--no-bench-compare")) bench_compare = 0;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) fuzz_trials = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-seed") && i + 1 < argc) fuzz_seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-cap") && i + 1 < argc) fuzz_cap = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--fuzz-replay") && i + 1 < argc) fuzz_replay = strtoull(argv[++i], NULL, 0);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!interp_step) interp_step = 1;

    /* The SPU ELF's length: the end of its last program-header segment and of the header tables (it is embedded). */
    unsigned char head[64];
    { unsigned char *h = read_range(path, offset, sizeof head); memcpy(head, h, sizeof head); free(h); }
    if (memcmp(head, "\x7f""ELF", 4) || be16(head + 18) != 23) { fprintf(stderr, "NOT_AN_SPU_ELF at 0x%zX\n", offset); return 1; }
    size_t elf_length = 0;
    { const uint32_t phoff = be32(head + 28), phnum = be16(head + 44), phentsize = be16(head + 42);
      unsigned char *ph = read_range(path, offset + phoff, (size_t)phnum * phentsize);
      elf_length = phoff + (size_t)phnum * phentsize;
      for (uint32_t k = 0; k < phnum; ++k) { const unsigned char *e = ph + (size_t)k * phentsize;
          const size_t end = (size_t)be32(e + 4) + be32(e + 16); if (end > elf_length) elf_length = end; }
      free(ph); }
    unsigned char *elf = read_range(path, offset, elf_length);
    printf("BINK_ELF offset=0x%zX bytes=0x%zX entry=0x%X\n", offset, elf_length, be32(head + 24));

    /* lv2: parse the image, initialize one thread of a one-thread group (this is what loads the segments into LS). */
    ps3_pthread_guard guard; if (ps3_pthread_guard_init(&guard)) return 1;
    ps3_lv2_spu_images images = { 0 }; ps3_lv2_spu_image parsed = { 0 };
    unsigned char segments[PS3_LV2_SPU_IMAGE_SEGMENTS_MAX * PS3_LV2_SPU_SEGMENT_BYTES]; uint32_t segment_count = 0;
    if (ps3_lv2_spu_images_init(&images, ps3_pthread_guard_port(&guard)) ||
        ps3_lv2_spu_image_parse(&images, elf, elf_length, &parsed, segments, sizeof segments, &segment_count)) { fprintf(stderr, "PARSE_FAILED\n"); return 1; }
    unsigned char *local_stores = calloc(PS3_LV2_SPU_GROUP_CAP * PS3_LV2_SPU_THREADS_MAX, PS3_LV2_SPU_LS_BYTES);
    ps3_lv2_spu_groups *groups = calloc(1, sizeof *groups);
    uint32_t group_id = 0, thread_id = 0;
    const uint64_t args[4] = { UINT64_C(0x7261643500000000), 0, 0, 0 };   /* SPU_ARG arg1 from the hardware log; arg2..4 unknown (0) */
    if (!local_stores || !groups || ps3_lv2_spu_groups_init(groups, ps3_pthread_guard_port(&guard), local_stores, 6) ||
        ps3_lv2_spu_group_create(groups, &group_id, 1, 128, 0) ||
        ps3_lv2_spu_thread_initialize(groups, &thread_id, group_id, 0, &parsed, segments, segment_count, elf, elf_length, args)) {
        fprintf(stderr, "LV2_INIT_FAILED\n"); return 1; }
    const unsigned char *ls = local_stores;
    printf("BINK_LOAD segments=%u entry=0x%X ls_code_hash=0x%016llX\n", segment_count, parsed.entry_point,
           (unsigned long long)ps3_spu_lifted_fnv1a64(ls + ps3_bink_lifted_module.code_begin, ps3_bink_lifted_module.code_end - ps3_bink_lifted_module.code_begin));
    const ps3_spu_lifted_module *const known[1] = { &ps3_bink_lifted_module };
    const ps3_spu_lifted_module *found = ps3_spu_lifted_find(ls, parsed.entry_point, known, 1);
    CHECK(found == &ps3_bink_lifted_module, "content hash does not match the lifted module (expected 0x%016llX)",
          (unsigned long long)ps3_bink_lifted_module.code_hash);
    if (!found) { printf("overall=FAIL\n"); return 1; }
    printf("BINK_MATCH module=%s hash_ok=1\n", found->name);

    Run lifted, interp;
    run_open(&lifted, "lifted", 1, ls, parsed.entry_point, args, mem_seed);
    run_open(&interp, "interp", 0, ls, parsed.entry_point, args, mem_seed);

    /* ---- stage 1: from the entry to the first stop */
    int rc = run_to_stop(&lifted, interp_step);
    CHECK(rc == 0, "lifted run to first stop failed rc=%d", rc);
    rc = run_to_stop(&interp, interp_step);
    CHECK(rc == 0, "interpreter run to first stop failed rc=%d", rc);
    unsigned total_diffs = compare_runs(&lifted, &interp, "stage1(first stop)", CMP_CPU | CMP_GUEST);
    printf("BINK_LIFTED_STATS enters=%llu escapes=%llu stops=%llu interp_instructions=%llu waits=%llu | interp instructions=%llu\n",
           (unsigned long long)lifted.stats.enters, (unsigned long long)lifted.stats.escapes, (unsigned long long)lifted.stats.stops,
           (unsigned long long)lifted.stats.interp_instructions, (unsigned long long)lifted.stats.waits, (unsigned long long)interp.interp_instructions);
    int stage1_ok = lifted.last.kind == PS3_SPU_INTERP_STOPPED && lifted.last.code == 0x110 && interp.last.kind == PS3_SPU_INTERP_STOPPED && interp.last.code == 0x110;
    CHECK(stage1_ok, "stage 1 did not end at stop 0x110 (lifted kind=%d code=0x%X pc=0x%X, interp kind=%d code=0x%X pc=0x%X)",
          lifted.last.kind, lifted.last.code, lifted.last.pc, interp.last.kind, interp.last.code, interp.last.pc);

    /* ---- stage 2: feed synthetic events, one at a time, compare after each */
    for (unsigned n = 0; stage1_ok && n < events && !failures && !total_diffs; ++n) {
        uint32_t qa = 0, qb = 0;
        int pa = serve_receive_event(&lifted, event[0], event[1], event[2], &qa);
        int pb = serve_receive_event(&interp, event[0], event[1], event[2], &qb);
        CHECK(pa == 0 && pb == 0 && qa == qb, "event %u: out mailbox pop lifted=%d/0x%X interp=%d/0x%X", n, pa, qa, pb, qb);
        const unsigned c0 = lifted.channel_count, d0 = lifted.dma_count; const uint64_t i0 = interp.interp_instructions;
        rc = run_to_stop(&lifted, interp_step); CHECK(rc == 0, "lifted event %u rc=%d", n, rc);
        rc = run_to_stop(&interp, interp_step); CHECK(rc == 0, "interp event %u rc=%d", n, rc);
        char label[64]; snprintf(label, sizeof label, "stage2(event %u)", n);
        if (verbose) {
            for (unsigned k = d0; k < lifted.dma_count && k < LOG_MAX; ++k)
                printf("BINK_DMA event=%u #%u lsa=0x%05X ea=0x%llX bytes=%u cmd=0x%X\n", n, k, lifted.dma_log[k].lsa, (unsigned long long)lifted.dma_log[k].ea, lifted.dma_log[k].bytes, lifted.dma_log[k].command);
            for (unsigned k = c0; k < lifted.channel_count && k < c0 + 64; ++k)
                printf("BINK_CHANNEL event=%u #%u kind=%u channel=%u\n", n, k, lifted.channel_log[k].kind, lifted.channel_log[k].channel);
            for (unsigned k = 0; k < lifted.event_count && k < 8; ++k) printf("BINK_USER_EVENT #%u value=0x%08X data0=0x%08X\n", k, lifted.events[k].value, lifted.events[k].data0);
        }
        total_diffs += compare_runs(&lifted, &interp, label, CMP_CPU | CMP_GUEST);
        printf("BINK_EVENT n=%u spuq=0x%X data=0x%X,0x%X,0x%X -> lifted %s code=0x%X (%s) channel_ops+%u dma+%u | interp instructions+%llu\n", n, qa,
               event[0], event[1], event[2], lifted.last.kind == PS3_SPU_INTERP_STOPPED ? "stop" : lifted.last.kind == PS3_SPU_INTERP_FAULT ? "FAULT" : "other",
               lifted.last.code, stop_name(lifted.last.code), lifted.channel_count - c0, lifted.dma_count - d0, (unsigned long long)(interp.interp_instructions - i0));
        if (lifted.last.kind != PS3_SPU_INTERP_STOPPED || lifted.last.code != 0x110) break;   /* terminal: nothing more to feed */
    }
    printf("BINK_LIFTED_STATS enters=%llu escapes=%llu stops=%llu interp_instructions=%llu waits=%llu | interp instructions=%llu\n",
           (unsigned long long)lifted.stats.enters, (unsigned long long)lifted.stats.escapes, (unsigned long long)lifted.stats.stops,
           (unsigned long long)lifted.stats.interp_instructions, (unsigned long long)lifted.stats.waits, (unsigned long long)interp.interp_instructions);

    /* ---- function-level fuzz (own owners; the stage 1/2 owners are left as they are) */
    if (fuzz_trials && !failures && !total_diffs) {
        Run fl, fi;
        run_open(&fl, "fuzz-lifted", 1, ls, parsed.entry_point, args, 0);
        run_open(&fi, "fuzz-interp", 0, ls, parsed.entry_point, args, 0);
        if (fuzz(&fl, &fi, ls, fuzz_trials, fuzz_seed, fuzz_cap, verbose, fuzz_replay)) total_diffs++;
        run_close(&fl); run_close(&fi);
    }

    /* ---- speed phase: fresh owners, same events, no comparison inside the timed region */
    if (bench && !failures && !total_diffs) {
        Run bl, bi;
        run_open(&bl, "bench-lifted", 1, ls, parsed.entry_point, args, mem_seed);
        run_open(&bi, "bench-interp", 0, ls, parsed.entry_point, args, mem_seed);
        CHECK(run_to_stop(&bl, 200000) == 0 && run_to_stop(&bi, 200000) == 0, "bench warm-up");
        const uint64_t warm_interp = bi.interp_instructions;
        double t_lifted = 0, t_interp = 0;
        uint64_t iter_done = 0;
        for (int pass = 0; pass < 2; ++pass) {
            Run *r = pass == 0 ? &bl : &bi;
            const double t0 = seconds_now();
            for (uint64_t n = 0; n < bench; ++n) {
                serve_receive_event(r, event[0], event[1], event[2], NULL);
                if (run_to_stop(r, 200000)) { fprintf(stderr, "bench run failed\n"); failures++; break; }
                if (r->last.kind != PS3_SPU_INTERP_STOPPED || r->last.code != 0x110) { if (pass == 0) iter_done = n; break; }
                if (pass == 0) iter_done = n + 1;
            }
            const double t = seconds_now() - t0;
            if (pass == 0) t_lifted = t; else t_interp = t;
        }
        const uint64_t instructions = bi.interp_instructions - warm_interp;
        printf("BENCH_BINK events=%llu instructions=%llu (interpreter count) lifted_seconds=%.4f interp_seconds=%.4f "
               "lifted_equiv_mips=%.1f interp_mips=%.1f speedup=%.2fx lifted_interp_instructions=%llu escapes=%llu enters=%llu\n",
               (unsigned long long)bench, (unsigned long long)instructions, t_lifted, t_interp,
               t_lifted > 0 ? (double)instructions / t_lifted / 1e6 : 0.0, t_interp > 0 ? (double)instructions / t_interp / 1e6 : 0.0,
               t_lifted > 0 ? t_interp / t_lifted : 0.0, (unsigned long long)bl.stats.interp_instructions,
               (unsigned long long)bl.stats.escapes, (unsigned long long)bl.stats.enters);
        (void)iter_done;
        if (bench_compare) total_diffs += compare_runs(&bl, &bi, "bench(final)", CMP_CPU | CMP_GUEST | CMP_NO_CHANNEL_LOG);
        run_close(&bl); run_close(&bi);
    }

    run_close(&lifted); run_close(&interp);
    free(groups); free(local_stores); free(elf);
    ps3_pthread_guard_destroy(&guard);
    const int pass = !failures && !total_diffs;
    printf("overall=%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
