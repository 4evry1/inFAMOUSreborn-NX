/* Hybrid lifted/interpreted execution on a ps3_spu_executor owner. See spu_lifted_hybrid.h for the contract. */
#include "spu_lifted_hybrid.h"
#include "../adapters/spu_lifted_boundary.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A private pause code (ps3_spu_executor_pause records whatever code it is given). Distinct from every PS3_SPU_EXEC_*. */
enum { PS3_LIFTED_PAUSE_ESCAPE = 0x7E01 };
enum { PS3_LIFTED_ROWS = SPU_LS_SIZE / 4 - 1 };   /* pcs 4, 8, ..., SPU_LS_SIZE - 4 */

static ps3_spu_executor *owner_of(spu_context *ctx) { return (ps3_spu_executor *)ctx; }   /* closed first-subobject contract */

/* ----------------------------------------------------------------------------------------- per-thread run state */
/* lifted_region / lifted_mark_ns (dynamic mode): the local-store region lifted time is charged to now and when it was last charged (Decision 133: a lifted run that starts in
 * the kernel goes on into the policy module through ps3_lifted_pm_resolve, which moves the charge from the kernel's accounting slot to the policy module's). */
typedef struct { const ps3_spu_lifted_services *services; ps3_spu_lifted_stats *stats; uint32_t halt_opcode; ps3_spu_dyn *dyn; unsigned channel_pc_exact;
                 unsigned lifted_region; uint64_t lifted_mark_ns; } run_state;
/* Decision 134: the job in progress on this host thread (its accounting slot, link address and host depth at its call, start time). Thread-local and kept across
 * slices: one owner per host thread. */
typedef struct { unsigned open, slot; uint32_t return_pc; uint32_t depth; uint64_t start_ns; void (*end)(unsigned, uint64_t, int); } job_clock;
static __thread job_clock g_job;
static __thread run_state g_run;

/* -------------------------------------------------------------------------------------------------------- hashing */
uint64_t ps3_spu_lifted_fnv1a64(const uint8_t *bytes, size_t length) {
    uint64_t h = UINT64_C(0xCBF29CE484222325);
    for (size_t i = 0; i < length; ++i) { h ^= bytes[i]; h *= UINT64_C(0x100000001B3); }
    return h;
}

static inline uint64_t mix64(uint64_t h, uint64_t w) { h ^= w; h *= UINT64_C(0x9E3779B97F4A7C15); return h ^ (h >> 32); }
static inline uint64_t load_le64(const uint8_t *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 | (uint64_t)p[3] << 24 |
           (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 | (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}
uint64_t ps3_spu_lifted_hash64w(const uint8_t *bytes, size_t length) {
    uint64_t h0 = UINT64_C(0x243F6A8885A308D3), h1 = UINT64_C(0x13198A2E03707344), h2 = UINT64_C(0xA4093822299F31D0), h3 = UINT64_C(0x082EFA98EC4E6C89);
    size_t i = 0;
    for (; i + 32 <= length; i += 32) {
        h0 = mix64(h0, load_le64(bytes + i));      h1 = mix64(h1, load_le64(bytes + i + 8));
        h2 = mix64(h2, load_le64(bytes + i + 16)); h3 = mix64(h3, load_le64(bytes + i + 24));
    }
    for (; i + 8 <= length; i += 8) h0 = mix64(h0, load_le64(bytes + i));
    if (i < length) { uint8_t pad[8] = { 0 }; memcpy(pad, bytes + i, length - i); h0 = mix64(h0, load_le64(pad)); }
    return mix64(mix64(mix64(mix64(h0, h1), h2), h3), (uint64_t)length);
}

const ps3_spu_lifted_module *ps3_spu_lifted_find(const uint8_t *ls, uint32_t entry_pc,
                                                 const ps3_spu_lifted_module *const *modules, unsigned count) {
    if (!ls || !modules) return NULL;
    for (unsigned i = 0; i < count; ++i) {
        const ps3_spu_lifted_module *m = modules[i];
        if (!m || m->entry_pc != entry_pc || m->code_begin >= m->code_end || m->code_end > SPU_LS_SIZE) continue;
        if (ps3_spu_lifted_fnv1a64(ls + m->code_begin, m->code_end - m->code_begin) == m->code_hash) return m;
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------- the exhaustive linked image */
typedef struct {
    const ps3_spu_lifted_module *module;   /* claimed with a CAS */
    unsigned state;                        /* 0 empty, 1 building, 2 ready, 3 malformed */
    ps3_spu_lifted_function *functions;
    uint8_t *genuine;                      /* bit (pc >> 2) */
    ps3_spu_linked_image image;
} image_slot;
enum { IMAGE_SLOTS = 32 };   /* Decision 104: the static module plus up to 21 job modules (each builds its image lazily on first use) */
static image_slot g_slots[IMAGE_SLOTS];

static void sleep_microseconds(uint32_t microseconds) {
    struct timespec request = { (time_t)(microseconds / 1000000u), (long)((microseconds % 1000000u) * 1000u) };
    nanosleep(&request, NULL);
}

static void build_image(image_slot *slot) {
    const ps3_spu_lifted_module *m = slot->module;
    int ok = m->rows && m->row_count;
    for (unsigned i = 0; ok && i < m->row_count; ++i) {
        const ps3_spu_lifted_row *r = &m->rows[i];
        if (!r->function || !r->pc || r->pc >= SPU_LS_SIZE || (r->pc & 3u) || (i && r->pc <= m->rows[i - 1].pc)) ok = 0;
    }
    if (ok) {
        slot->functions = malloc(sizeof(ps3_spu_lifted_function) * PS3_LIFTED_ROWS);
        slot->genuine = calloc(1, SPU_LS_SIZE / 4 / 8);
        if (!slot->functions || !slot->genuine) ok = 0;
    }
    if (!ok) { free(slot->functions); free(slot->genuine); slot->functions = NULL; slot->genuine = NULL;
               __atomic_store_n(&slot->state, 3u, __ATOMIC_RELEASE); return; }
    unsigned next = 0;
    for (uint32_t index = 0; index < PS3_LIFTED_ROWS; ++index) {
        const uint32_t pc = (index + 1u) * 4u;
        if (next < m->row_count && m->rows[next].pc == pc) {
            slot->functions[index] = (ps3_spu_lifted_function){ pc, m->rows[next].function };
            if (m->rows[next].flags & PS3_LIFTED_ROW_GENUINE) slot->genuine[(pc >> 2) >> 3] |= (uint8_t)(1u << ((pc >> 2) & 7u));
            ++next;
        } else {
            slot->functions[index] = (ps3_spu_lifted_function){ pc, ps3_lifted_escape };
        }
    }
    slot->image = (ps3_spu_linked_image){ m->name, slot->functions, PS3_LIFTED_ROWS };
    __atomic_store_n(&slot->state, 2u, __ATOMIC_RELEASE);
}

static image_slot *slot_for(const ps3_spu_lifted_module *module) {
    if (!module) return NULL;
    for (unsigned i = 0; i < IMAGE_SLOTS; ++i) {
        const ps3_spu_lifted_module *seen = __atomic_load_n(&g_slots[i].module, __ATOMIC_ACQUIRE);
        if (!seen) {
            const ps3_spu_lifted_module *expected = NULL;
            if (__atomic_compare_exchange_n(&g_slots[i].module, &expected, module, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&g_slots[i].state, 1u, __ATOMIC_RELEASE);
                build_image(&g_slots[i]);
                return &g_slots[i];
            }
            seen = expected;
        }
        if (seen == module) {
            while (__atomic_load_n(&g_slots[i].state, __ATOMIC_ACQUIRE) < 2u) sleep_microseconds(50);
            return &g_slots[i];
        }
    }
    return NULL;
}

const ps3_spu_linked_image *ps3_spu_lifted_image(const ps3_spu_lifted_module *module) {
    image_slot *slot = slot_for(module);
    return slot && __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == 2u ? &slot->image : NULL;
}

int ps3_spu_lifted_is_genuine(const ps3_spu_lifted_module *module, uint32_t pc) {
    image_slot *slot = slot_for(module);
    pc &= SPU_LS_MASK & ~3u;
    if (!slot || __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) != 2u || !pc) return 0;
    return (slot->genuine[(pc >> 2) >> 3] >> ((pc >> 2) & 7u)) & 1u;
}

/* Identity of a module in the local store (see ps3_spu_lifted_module): for a policy module a fold over its code ranges, so the data between them does not matter. */
uint64_t ps3_spu_lifted_module_hash(const uint8_t *ls, const ps3_spu_lifted_module *m) {
    if (!m->range_count) return ps3_spu_lifted_hash64w(ls + m->code_begin, m->code_end - m->code_begin);
    uint64_t h = UINT64_C(0x9E3779B97F4A7C15);
    for (unsigned i = 0; i < m->range_count && i < 4u; ++i) {
        const uint32_t lo = m->ranges[2u * i], hi = m->ranges[2u * i + 1u];
        h = (h ^ ps3_spu_lifted_hash64w(ls + lo, hi - lo)) * UINT64_C(0xFF51AFD7ED558CCD);
        h ^= h >> 32;
    }
    return h;
}

/* A policy module and a job module armed together run on a COMPOSITE exhaustive image: the job's rows with the policy module's rows laid over them (the two never
 * overlap: the policy module lives at 0xA00..0x3728, a job window module from 0x4000), so a job's call of a policy-module helper and the policy module's call of a job are
 * direct lifted transfers. Decision 133: the SPURS kernel (LS 0x290..0x848) is the third layer, laid over both; with it armed every image the owner is bound to is a
 * composite that holds it. One composite per combination in use (built on first use, 1 MiB, shared by every thread). */
/* Process-wide totals of the policy-module counters (the per-thread stats are folded into one report line by whoever prints them; these are for the accounting report). */
static uint64_t g_pm_totals[6];
#define PM_COUNT(stats_ptr, field, index) do { if (stats_ptr) ++(stats_ptr)->field; __atomic_fetch_add(&g_pm_totals[index], 1u, __ATOMIC_RELAXED); } while (0)
void ps3_spu_lifted_pm_totals(uint64_t out[6]) { for (unsigned i = 0; i < 6; ++i) out[i] = __atomic_load_n(&g_pm_totals[i], __ATOMIC_RELAXED); }
static uint64_t g_kernel_totals[PS3_LIFTED_KERNEL_TOTALS];
enum { KERNEL_TOTAL_ARMED = 0, KERNEL_TOTAL_MISMATCH = 1, KERNEL_TOTAL_PM_CALLS = 2, KERNEL_TOTAL_PM_ARMED = 3, KERNEL_TOTAL_EXITS = 4, KERNEL_TOTAL_DISARMS = 5 };
#define KERNEL_COUNT(stats_ptr, field, index) do { if (stats_ptr) ++(stats_ptr)->field; __atomic_fetch_add(&g_kernel_totals[index], 1u, __ATOMIC_RELAXED); } while (0)
void ps3_spu_lifted_kernel_totals(uint64_t out[PS3_LIFTED_KERNEL_TOTALS]) {
    for (unsigned i = 0; i < PS3_LIFTED_KERNEL_TOTALS; ++i) out[i] = __atomic_load_n(&g_kernel_totals[i], __ATOMIC_RELAXED);
}
/* A build has the SPURS kernel and three policy modules, each with its own jobs: room for every (kernel, policy module, job module) combination of a table of IMAGE_SLOTS modules. */
typedef struct { unsigned claim; const ps3_spu_lifted_module *kernel, *pm, *job; image_slot slot; } composite_entry;   /* claim: 0 free, 1 being claimed, 2 combination published */
static composite_entry g_composite[IMAGE_SLOTS * 3];
/* The self-checking modules of the table (kind PS3_LIFTED_KIND_GUARDED), laid over the kernel in every composite. One process-wide set: every dynamic thread is given the
 * same module table (spu_group_runner.c), published once by the first thread that prepares its entries. state: 0 empty, 1 building, 2 ready. */
enum { GUARDED_MAX = 4 };
static const ps3_spu_lifted_module *g_guarded[GUARDED_MAX];
static unsigned g_guarded_count, g_guarded_state;
static void publish_guarded(const ps3_spu_lifted_module *const *modules, unsigned count) {
    unsigned expected = 0;
    if (__atomic_load_n(&g_guarded_state, __ATOMIC_ACQUIRE) == 2u) return;
    if (!__atomic_compare_exchange_n(&g_guarded_state, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_guarded_state, __ATOMIC_ACQUIRE) != 2u) sleep_microseconds(20);
        return;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < count && n < GUARDED_MAX; ++i)
        if (modules[i] && modules[i]->kind == PS3_LIFTED_KIND_GUARDED && modules[i]->rows && modules[i]->row_count) g_guarded[n++] = modules[i];
    g_guarded_count = n;
    __atomic_store_n(&g_guarded_state, 2u, __ATOMIC_RELEASE);
}
static void overlay_rows(ps3_spu_lifted_function *f, uint8_t *bits, const ps3_spu_lifted_module *m) {
    for (unsigned i = 0; i < m->row_count; ++i) {
        const ps3_spu_lifted_row *r = &m->rows[i];
        const uint32_t index = (r->pc >> 2) - 1u;
        f[index] = (ps3_spu_lifted_function){ r->pc, r->function };
        if (r->flags & PS3_LIFTED_ROW_GENUINE) bits[(r->pc >> 2) >> 3] |= (uint8_t)(1u << ((r->pc >> 2) & 7u));
    }
}
static void build_composite(image_slot *slot, const ps3_spu_lifted_module *kernel, const ps3_spu_lifted_module *pm, const image_slot *job_slot) {
    ps3_spu_lifted_function *f = malloc(sizeof(ps3_spu_lifted_function) * PS3_LIFTED_ROWS);
    uint8_t *bits = calloc(1, SPU_LS_SIZE / 4 / 8);
    if (!f || !bits) { free(f); free(bits); __atomic_store_n(&slot->state, 3u, __ATOMIC_RELEASE); return; }
    if (job_slot) {
        memcpy(f, job_slot->functions, sizeof(ps3_spu_lifted_function) * PS3_LIFTED_ROWS);
        memcpy(bits, job_slot->genuine, SPU_LS_SIZE / 4 / 8);
    } else {
        for (uint32_t index = 0; index < PS3_LIFTED_ROWS; ++index) f[index] = (ps3_spu_lifted_function){ (index + 1u) * 4u, ps3_lifted_escape };
    }
    if (pm) overlay_rows(f, bits, pm);
    if (kernel) overlay_rows(f, bits, kernel);
    if (kernel) for (unsigned i = 0; i < g_guarded_count; ++i) overlay_rows(f, bits, g_guarded[i]);
    slot->functions = f; slot->genuine = bits;
    slot->image = (ps3_spu_linked_image){ pm ? pm->name : kernel ? kernel->name : "ps3-lifted-composite", f, PS3_LIFTED_ROWS };
    __atomic_store_n(&slot->state, 2u, __ATOMIC_RELEASE);
}
static int slot_ready(const ps3_spu_lifted_module *module, image_slot **out) {
    *out = NULL;
    if (!module) return 1;
    *out = slot_for(module);
    return *out && __atomic_load_n(&(*out)->state, __ATOMIC_ACQUIRE) == 2u;
}
/* The composite of the given modules (any two or all three; NULL = absent), or NULL when one of them has no image or the table is full. */
static image_slot *composite_for(const ps3_spu_lifted_module *kernel, const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job) {
    image_slot *kernel_slot, *pm_slot, *job_slot;
    if (!slot_ready(kernel, &kernel_slot) || !slot_ready(pm, &pm_slot) || !slot_ready(job, &job_slot)) return NULL;
    for (unsigned i = 0; i < IMAGE_SLOTS * 3u; ++i) {
        composite_entry *entry = &g_composite[i];
        unsigned claim = __atomic_load_n(&entry->claim, __ATOMIC_ACQUIRE);
        if (!claim) {
            unsigned expected = 0;
            if (__atomic_compare_exchange_n(&entry->claim, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                entry->kernel = kernel; entry->pm = pm; entry->job = job;
                __atomic_store_n(&entry->slot.state, 1u, __ATOMIC_RELEASE);
                __atomic_store_n(&entry->claim, 2u, __ATOMIC_RELEASE);
                build_composite(&entry->slot, kernel, pm, job_slot);
                return __atomic_load_n(&entry->slot.state, __ATOMIC_ACQUIRE) == 2u ? &entry->slot : NULL;
            }
            claim = expected;
        }
        while (claim == 1u) { sleep_microseconds(20); claim = __atomic_load_n(&entry->claim, __ATOMIC_ACQUIRE); }
        if (entry->kernel == kernel && entry->pm == pm && entry->job == job) {
            while (__atomic_load_n(&entry->slot.state, __ATOMIC_ACQUIRE) < 2u) sleep_microseconds(50);
            return __atomic_load_n(&entry->slot.state, __ATOMIC_ACQUIRE) == 2u ? &entry->slot : NULL;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------------------------------------- start */
static int never_select(void *context, uint32_t spu_id, unsigned flags, uint64_t *out) {
    (void)context; (void)spu_id; (void)flags; (void)out;
    return PS3_SPU_EXEC_UNSUPPORTED;
}

int ps3_spu_lifted_start(ps3_spu_executor *o, const ps3_spu_lifted_module *module, ps3_clock clock) {
    if (!o || !module) return PS3_SPU_EXEC_INVALID;
    const ps3_spu_linked_image *image = ps3_spu_lifted_image(module);
    if (!image) return PS3_SPU_EXEC_INVALID;
    ps3_spu_executor_set_interpreter(o);
    ps3_spu_executor_start_config config = { 0 };
    config.image = image; config.image_id = module->image_id; config.clock = clock;
    config.thread_option = PS3_SPU_THREAD_OPTION_NONE; config.hop_budget = UINT64_MAX;
    const int status = ps3_spu_executor_start(o, &config);
    if (status) return status;
    o->core.image_id = module->image_id;
    /* Opt-in for exact bound-image indirect lookup (spu_lifted_boundary.c). Neither pc can ever match: they are masked
     * to the 18-bit local store address before the comparison. */
    o->policy.context = NULL; o->policy.select = never_select;
    o->policy.select_pc = 0xFFFFFFFFu; o->policy.exit_pc = 0xFFFFFFFFu;
    return PS3_SPU_EXEC_OK;
}

/* ------------------------------------------------------------------- entry points called by the lifted code */
static void observe(spu_context *ctx, unsigned kind, uint32_t channel) {
    ps3_spu_executor *o = owner_of(ctx);
    if (o->observe_channel) o->observe_channel(o->observe_context, kind, channel);
}

/* Wait in place for a PPU-side condition. The interpreter would return PAUSED with pc at the channel instruction and
 * be re-run; lifted code is not resumable at an arbitrary instruction, so the wait happens inside the channel call. */
static void wait_for(spu_context *ctx, uint32_t channel) {
    ps3_spu_executor *o = owner_of(ctx);
    const ps3_spu_lifted_services *s = g_run.services;
    if (g_run.stats) ++g_run.stats->waits;
    int cancelled = 0;
    if (s && s->wait) cancelled = s->wait(s->context, ctx, channel) ? PS3_SPU_EXEC_CANCELLED : 0;
    else {
        o->binding.channel = channel; o->binding.effect_committed = 0;
        cancelled = o->binding.checkpoint(ctx, PS3_SPU_PRE_CHANNEL);
        if (!cancelled) sleep_microseconds(100);
    }
    if (cancelled) { o->binding.channel = channel; o->binding.effect_committed = 0; ps3_spu_execution_fail(ctx, cancelled); }
}

static u128 rdch_impl(spu_context *ctx, uint32_t channel) {
    observe(ctx, PS3_SPU_OBS_READ_CHANNEL, channel);
    if (channel == SPU_RdInMbox) while (!spu_channel_has_data(&ctx->ch_in_mbox)) wait_for(ctx, channel);
    return spu_rdch(ctx, channel);
}

static void wrch_impl(spu_context *ctx, uint32_t channel, u128 value) {
    observe(ctx, PS3_SPU_OBS_WRITE_CHANNEL, channel);
    if (channel == SPU_WrOutMbox || channel == SPU_WrOutIntrMbox) {
        const spu_channel *out = channel == SPU_WrOutMbox ? &ctx->ch_out_mbox : &ctx->ch_out_intr_mbox;
        while (out->count >= SPU_MBOX_DEPTH) wait_for(ctx, channel);
        spu_wrch(ctx, channel, value);
        const ps3_spu_lifted_services *s = g_run.services;
        if (channel == SPU_WrOutIntrMbox && s && s->intr_written) s->intr_written(s->context, ctx);
        return;
    }
    spu_wrch(ctx, channel, value);
}

/* channel_pc_exact: the channel operation the lifted code is in carries the address of its instruction (ps3_lifted_*_at), so a pause there may resume at ctx->pc. Every other
 * channel entry point clears it: a job module's channel operations leave ctx->pc stale and a pause inside them is not resumable. */
u128 ps3_lifted_rdch(spu_context *ctx, uint32_t channel) { g_run.channel_pc_exact = 0; return rdch_impl(ctx, channel); }
void ps3_lifted_wrch(spu_context *ctx, uint32_t channel, u128 value) { g_run.channel_pc_exact = 0; wrch_impl(ctx, channel, value); }

uint32_t ps3_lifted_rchcnt(spu_context *ctx, uint32_t channel) {
    observe(ctx, PS3_SPU_OBS_READ_CHANNEL_COUNT, channel);
    return spu_rchcnt(ctx, channel);
}
/* rdch / wrch / rchcnt (11-bit op 0x00D / 0x10D / 0x00F): the only instructions a paused channel operation can be at. */
static int is_channel_word(uint32_t w) { const uint32_t op = w >> 21; return op == 0x00Du || op == 0x10Du || op == 0x00Fu; }
/* Policy-module code (round 2): ctx->pc is the address of the channel instruction before the channel code runs. A read the owner cannot complete now (a blocking
 * RdEventStat / RdTagStat / RdListStallStat) pauses inside the channel code exactly as it does for the interpreter, with ctx->pc as the pc to resume at (the channel code
 * records it); the dynamic layer turns that pause into "continue in the interpreter at this instruction" (run_slice_dyn). The generator proves the address of every
 * channel operation of the module and the register file is written back before a channel operation, so the interpreter resumes with the exact architectural state. */
u128 ps3_lifted_rdch_at(spu_context *ctx, uint32_t channel, uint32_t instruction_address) {
    ctx->pc = instruction_address & SPU_LS_MASK;
    g_run.channel_pc_exact = 1;
    return rdch_impl(ctx, channel);
}
void ps3_lifted_wrch_at(spu_context *ctx, uint32_t channel, u128 value, uint32_t instruction_address) {
    ctx->pc = instruction_address & SPU_LS_MASK;
    g_run.channel_pc_exact = 1;
    wrch_impl(ctx, channel, value);
}

/* Lifted code reached an architectural `stop`. Leave resumable: pc after the stop, no C frame kept. */
void ps3_lifted_stop_at(spu_context *ctx, uint32_t stop_address) {
    ps3_spu_executor *o = owner_of(ctx);
    o->binding.channel = PS3_SPU_EXEC_OP_STOP; o->binding.effect_committed = 0;
    if (stop_address == PS3_LIFTED_NO_STOP_WORD) ps3_spu_execution_fail(ctx, PS3_SPU_EXEC_UNSUPPORTED);
    ctx->pc = (stop_address + 4u) & SPU_LS_MASK;
    ctx->status = SPU_STATUS_RUNNING;
    ctx->host_depth = 0; ctx->drain_ret_pc = 0; o->continuation = NULL;
    if (g_run.stats) ++g_run.stats->stops;
    ps3_spu_executor_pause(ctx, PS3_SPU_EXEC_STOP);
}

/* A conditional halt (heq/hgt/hlgt assertion) fired in lifted code: the interpreter's FAULT form. */
void ps3_lifted_halt_at(spu_context *ctx, uint32_t instruction_address) {
    ctx->pc = instruction_address & SPU_LS_MASK;
    ctx->stop_code = 0x1000;                  /* SPU_HALT_STOP_CODE: the assertion halt status */
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
    g_run.halt_opcode = spu_ls_read32(ctx, ctx->pc);
    ps3_spu_execution_fail(ctx, PS3_SPU_EXEC_UNSUPPORTED);
}

#ifdef PS3_LIFTED_ESCAPE_HIST   /* test builds only: where do escapes happen (host test --escape-hist) */
uint64_t g_ps3_lifted_escape_hist[SPU_LS_SIZE / 4];
#endif

#ifdef PS3_SPU_ESCAPE_DIAG
/* Round 2 (console run rv38s: 86.6 million lifted entries for 288 thousand calls of one task module): which pcs does lifted job code
 * hand to the interpreter, and why? One classification (binary search over the armed module's rows) and a few adds per escape; the
 * runner folds the per-thread tables into the accounting slot of the job once per slice (ps3_spu_acct_flush_escapes). */
unsigned ps3_spu_lifted_classify_escape(const ps3_spu_lifted_module *m, uint32_t pc) {
    if (!m || pc < m->code_begin || pc >= m->code_end) return PS3_ESC_OUT;
    unsigned lo = 0, hi = m->row_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2u;
        if (m->rows[mid].pc < pc) lo = mid + 1u; else hi = mid;
    }
    return lo < m->row_count && m->rows[lo].pc == pc && !(m->rows[lo].flags & PS3_LIFTED_ROW_GENUINE) ? PS3_ESC_STUB : PS3_ESC_NOROW;
}
static void note_escape(spu_context *ctx, unsigned reason) {
    ps3_spu_dyn *dyn = g_run.dyn;
    if (!dyn) return;
    const ps3_spu_executor *o = owner_of(ctx);
    const uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    if (reason >= PS3_ESC_REASONS) reason = ps3_spu_lifted_classify_escape(dyn->resident, pc);
    if (g_run.stats) {
        switch (reason) {
        case PS3_ESC_STUB: ++g_run.stats->esc_stub; break;
        case PS3_ESC_NOROW: ++g_run.stats->esc_norow; break;
        case PS3_ESC_OUT: ++g_run.stats->esc_out; break;
        case PS3_ESC_HYBRID: ++g_run.stats->esc_hybrid; break;
        default: break;   /* IRQ: stats->irq_escapes */
        }
    }
    const unsigned slot = o->acct_slot[PS3_SPU_REGION_JOB] < PS3_SPU_ACCT_SLOTS ? o->acct_slot[PS3_SPU_REGION_JOB] : 0u;
    ++dyn->esc_n[slot][reason];
    dyn->esc_dirty = 1;
    /* Misra-Gries heavy hitters over (reason, pc) */
    const uint32_t key = pc | (reason << 24);
    uint32_t *keys = dyn->esc_key[slot], *hits = dyn->esc_hits[slot];
    unsigned free_index = PS3_ESC_TOP;
    for (unsigned i = 0; i < PS3_ESC_TOP; ++i) {
        if (hits[i] && keys[i] == key) { ++hits[i]; return; }
        if (!hits[i] && free_index == PS3_ESC_TOP) free_index = i;
    }
    if (free_index != PS3_ESC_TOP) { keys[free_index] = key; hits[free_index] = 1u; return; }
    for (unsigned i = 0; i < PS3_ESC_TOP; ++i) --hits[i];
}
#else
static inline void note_escape(spu_context *ctx, unsigned reason) { (void)ctx; (void)reason; }
#endif

/* ctx->pc is a pc the lifted image has no genuine function for: hand it to the interpreter. */
_Noreturn static void escape_as(spu_context *ctx, unsigned reason) {
    ps3_spu_executor *o = owner_of(ctx);
#ifdef PS3_LIFTED_ESCAPE_HIST
    ++g_ps3_lifted_escape_hist[(ctx->pc & SPU_LS_MASK) >> 2];
#endif
    note_escape(ctx, reason);
    o->binding.channel = PS3_SPU_EXEC_OP_INDIRECT_BRANCH; o->binding.effect_committed = 0;
    ctx->host_depth = 0; ctx->drain_ret_pc = 0; o->continuation = NULL;
    if (g_run.stats) ++g_run.stats->escapes;
    ps3_spu_executor_pause(ctx, PS3_LIFTED_PAUSE_ESCAPE);
}
void ps3_lifted_escape(spu_context *ctx) { escape_as(ctx, PS3_ESC_AUTO); }

/* An enabled interrupt is pending where lifted code is at an exact block boundary (or a call return): hand the state to the
 * interpreter, which delivers it before its next instruction. */
void ps3_lifted_irq_escape(spu_context *ctx) {
    if (g_run.stats) ++g_run.stats->irq_escapes;
    escape_as(ctx, PS3_ESC_IRQ);
}
static void boundary_irq_escape(spu_context *ctx) { ps3_lifted_irq_escape(ctx); }
static void boundary_hybrid_escape(spu_context *ctx) { escape_as(ctx, PS3_ESC_HYBRID); }

/* -------------------------------------------------------------------------------------------------------- slice */
static int fault_result(ps3_spu_executor *o, ps3_spu_interp_result *out, int code) {
    memset(out, 0, sizeof *out);
    out->kind = PS3_SPU_INTERP_FAULT; out->pc = o->core.pc & SPU_LS_MASK;
    out->code = (uint32_t)code; out->channel = o->terminal.channel;
    out->stop_code = o->core.stop_code; out->opcode = g_run.halt_opcode;
    return PS3_SPU_EXEC_OK;
}

int ps3_spu_lifted_run_slice(ps3_spu_executor *o, const ps3_spu_lifted_module *module, const ps3_spu_lifted_services *services,
                             uint64_t interp_budget, ps3_spu_interp_result *out, ps3_spu_lifted_stats *stats) {
    if (!o || !module || !out || !interp_budget) return PS3_SPU_EXEC_INVALID;
    image_slot *slot = slot_for(module);
    if (!slot || slot->state != 2u || !o->initialized || !o->started || o->escape || o->start.image != &slot->image)
        return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    g_run.services = services; g_run.stats = stats; g_run.halt_opcode = 0; g_run.dyn = NULL;
    uint64_t interpreted = 0;
    unsigned singles = 0;
    int result = PS3_SPU_EXEC_OK;
    for (;;) {
        const uint32_t pc = o->core.pc & SPU_LS_MASK & ~3u;
        if (ps3_spu_lifted_is_genuine(module, pc)) {
            ps3_spu_lifted_entry entry = ps3_spu_linked_image_lookup(&slot->image, pc);
            (void)ps3_spu_executor_take_continuation(o);
            o->core.pc = pc; o->core.status = SPU_STATUS_RUNNING; o->core.stop_code = 0;
            o->core.host_depth = 0; o->core.drain_ret_pc = 0;
            if (stats) ++stats->enters;
            const int code = ps3_spu_executor_enter(o, entry);
            if (stats) stats->hops_at_exit = o->hops;
            if (o->stopped) { result = fault_result(o, out, o->terminal.code); break; }
            if (code == PS3_SPU_EXEC_STOP) {
                if (!(services && services->skip_code_check) &&
                    ps3_spu_lifted_hash64w(o->core.ls + module->code_begin, module->code_end - module->code_begin) != module->code_hash_fast) {
                    /* The local store no longer holds the program the lifted code was made from. A named terminal. */
                    o->stopped = 1;
                    o->terminal = (ps3_spu_executor_terminal){ PS3_SPU_EXEC_UNSUPPORTED, o->provider_status, PS3_LIFTED_OP_CODE_CHANGED,
                                                               o->binding.request, o->progress, 0, PS3_CLOCK_OK };
                    result = fault_result(o, out, PS3_SPU_EXEC_UNSUPPORTED);
                    break;
                }
                memset(out, 0, sizeof *out);
                out->kind = PS3_SPU_INTERP_STOPPED; out->pc = o->core.pc & SPU_LS_MASK;
                out->code = o->core.stop_code & 0x3FFFu; out->instructions = interpreted;
                break;
            }
            if (code == PS3_LIFTED_PAUSE_ESCAPE) continue;        /* interpret from o->core.pc */
            if (code == PS3_SPU_EXEC_INVALID) { result = code; break; }
            /* Any other recoverable pause from inside lifted code (an interpreter-only blocking channel the glue does
             * not wait for): not resumable at the channel instruction. A named terminal, never a silent replay. */
            o->stopped = 1;
            result = fault_result(o, out, code);
            break;
        }
        /* Interpret: single steps first (the lifted entry is usually a few instructions away), then bigger chunks. */
        uint64_t step = singles < 64u ? 1u : 256u;
        if (step > interp_budget - interpreted) step = interp_budget - interpreted;
        ps3_spu_interp_result r;
        result = ps3_spu_executor_run_interp(o, o->core.pc & SPU_LS_MASK & ~3u, step, &r);
        if (result != PS3_SPU_EXEC_OK) break;
        interpreted += r.instructions; singles += (unsigned)step;
        if (stats) { stats->interp_instructions += r.instructions; ++stats->interp_calls; }
        if (r.kind != PS3_SPU_INTERP_BUDGET) { r.instructions = interpreted; *out = r; break; }
        if (interpreted >= interp_budget && !ps3_spu_lifted_is_genuine(module, o->core.pc)) {
            r.instructions = interpreted; *out = r; break;
        }
    }
    g_run.services = NULL; g_run.stats = NULL;
    return result;
}

/* ================================================================================ dynamic (job) modules, Decision 104 */
static inline uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The placeholder image an owner has bound while no job module is armed: every pc is an escape. Built once. */
static ps3_spu_lifted_function *g_escape_functions;
static ps3_spu_linked_image g_escape_image;
static unsigned g_escape_state;   /* 0 empty, 1 building, 2 ready */
static const ps3_spu_linked_image *escape_image(void) {
    for (;;) {
        const unsigned state = __atomic_load_n(&g_escape_state, __ATOMIC_ACQUIRE);
        if (state == 2u) return &g_escape_image;
        unsigned expected = 0;
        if (state == 0u && __atomic_compare_exchange_n(&g_escape_state, &expected, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            ps3_spu_lifted_function *f = malloc(sizeof(ps3_spu_lifted_function) * PS3_LIFTED_ROWS);
            if (!f) { __atomic_store_n(&g_escape_state, 0u, __ATOMIC_RELEASE); return NULL; }
            for (uint32_t index = 0; index < PS3_LIFTED_ROWS; ++index) f[index] = (ps3_spu_lifted_function){ (index + 1u) * 4u, ps3_lifted_escape };
            g_escape_functions = f;
            g_escape_image = (ps3_spu_linked_image){ "ps3-lifted-escape-only", f, PS3_LIFTED_ROWS };
            __atomic_store_n(&g_escape_state, 2u, __ATOMIC_RELEASE);
            return &g_escape_image;
        }
        sleep_microseconds(50);
    }
}

enum { PS3_DYN_IMAGE_ID = 0x7000 };   /* core.image_id / start.image_id while no module is armed (modules use their own ids) */

static void job_clock_drain_returned(spu_context *ctx, uint32_t return_pc);
int ps3_spu_lifted_dyn_start(ps3_spu_executor *o, ps3_clock clock) {
    if (!o) return PS3_SPU_EXEC_INVALID;
    const ps3_spu_linked_image *image = escape_image();
    if (!image) return PS3_SPU_EXEC_INVALID;
    ps3_spu_executor_set_interpreter(o);
    ps3_spu_executor_start_config config = { 0 };
    config.image = image; config.image_id = PS3_DYN_IMAGE_ID; config.clock = clock;
    config.thread_option = PS3_SPU_THREAD_OPTION_NONE; config.hop_budget = UINT64_MAX;
    const int status = ps3_spu_executor_start(o, &config);
    if (status) return status;
    o->core.image_id = PS3_DYN_IMAGE_ID;
    o->irq_escape = boundary_irq_escape; o->hybrid_escape = boundary_hybrid_escape;
    ps3_lifted_drain_returned_hook = job_clock_drain_returned;     /* Decision 134 (one value for the process) */
    o->policy.context = NULL; o->policy.select = never_select;
    o->policy.select_pc = 0xFFFFFFFFu; o->policy.exit_pc = 0xFFFFFFFFu;
    return PS3_SPU_EXEC_OK;
}

/* What is armed now -> the image the owner is bound to, the genuine-entry bitmap of the interpreter's watch and the watched code ranges of the DMA hook:
 *   nothing                   escape image, no bits
 *   one module                its own image (its id)
 *   two or three modules      their composite. The id is the kernel's when it is armed, else the policy module's: the id the outermost lifted caller saved at its call
 *                             and restores after it (spu_img_restore), so a module armed below it never changes the id.
 * With the kernel armed the owner's policy.exit_pc is the kernel's exit entry (see ps3_spu_lifted_module.exit_pc). */
static void dyn_prepare_entries(ps3_spu_dyn *dyn) {
    if (dyn->pm_entry_state) return;
    dyn->pm_entry_state = 1;
    publish_guarded(dyn->modules, dyn->module_count);
    for (unsigned i = 0; i < dyn->module_count; ++i) {
        const ps3_spu_lifted_module *m = dyn->modules[i];
        if (!m || m->kind != PS3_LIFTED_KIND_POLICY || m->entry_pc < 4u || (m->entry_pc & 3u) || m->entry_pc >= SPU_LS_SIZE) continue;
        dyn->pm_entry_bits[(m->entry_pc >> 2) >> 3] |= (uint8_t)(1u << ((m->entry_pc >> 2) & 7u));
        ++dyn->pm_entry_count;
    }
}
enum { PS3_NO_EXIT_PC = 0xFFFFFFFFu, WATCH_RANGES_MAX = 4 };
static void dyn_apply(ps3_spu_executor *o, ps3_spu_dyn *dyn) {
    dyn_prepare_entries(dyn);
    const ps3_spu_lifted_module *kernel = dyn->resident_kernel, *pm = dyn->resident_pm, *job = dyn->resident;
    const ps3_spu_linked_image *image = NULL;
    const uint8_t *bits = NULL;
    int32_t id = PS3_DYN_IMAGE_ID;
    /* (with the kernel armed and self-checking modules in the table, the kernel alone is a composite too: it carries them) */
    while (!image && ((kernel != NULL) + (pm != NULL) + (job != NULL) >= 2 || (kernel && g_guarded_count))) {
        image_slot *composite = composite_for(kernel, pm, job);
        if (composite) {
            image = &composite->image; bits = composite->genuine; id = kernel ? kernel->image_id : pm->image_id;
            PM_COUNT(g_run.stats, pm_composites, 4);
        }
        else if (job) dyn->resident = job = NULL;            /* no composite (build failed, table full): the job, then the policy module, run interpreted */
        else if (pm) dyn->resident_pm = pm = NULL;
        else break;                                          /* the kernel alone, without its self-checking modules */
    }
    if (!image) {
        const ps3_spu_lifted_module *single = kernel ? kernel : pm ? pm : job;
        image_slot *slot = single ? slot_for(single) : NULL;
        if (slot && __atomic_load_n(&slot->state, __ATOMIC_ACQUIRE) == 2u) { image = &slot->image; bits = slot->genuine; id = single->image_id; }
        else if (single) { dyn->resident_kernel = NULL; dyn->resident_pm = NULL; dyn->resident = NULL; kernel = pm = job = NULL; }
    }
    if (!image) image = escape_image();
    if (!image) { o->watch_entry_bits = NULL; return; }
    if (!pm && dyn->pm_entry_count) {       /* no policy module armed: the entries of the armed code and the policy modules' entries, so a branch to a policy module is seen */
        if (!bits) bits = dyn->pm_entry_bits;
        else {
            if (dyn->union_src != bits) {
                for (unsigned i = 0; i < PS3_SPU_LS_BITMAP_BYTES; ++i) dyn->union_bits[i] = bits[i] | dyn->pm_entry_bits[i];
                dyn->union_src = bits;
            }
            bits = dyn->union_bits;
        }
    }
    o->start.image = image; o->start.image_id = id; o->core.image_id = id;
    o->watch_entry_bits = bits;
    o->watch_code_lo = job ? job->code_begin : 0; o->watch_code_hi = job ? job->code_end : 0;
    o->watch_pm_count = 0;
    if (pm) {            /* (a policy module has 1..4 ranges: dyn_identify_pm refuses any other) */
        for (unsigned i = 0; i < pm->range_count; ++i) { o->watch_pm_lo[i] = pm->ranges[2u * i]; o->watch_pm_hi[i] = pm->ranges[2u * i + 1u]; }
        o->watch_pm_count = pm->range_count;
    }
    /* The kernel's code is NOT a watched range: the executor's DMA hook ends lifted code whose pc lies in ANY watched range when one of them is replaced, and the kernel
     * DMAs every policy module over the previous one from inside its own code. Nothing in SPURS writes the kernel's code; it is hashed again instead wherever control
     * enters lifted kernel code from outside (dyn_kernel_check) and at every policy-module call it makes (ps3_lifted_pm_resolve). */
    o->policy.exit_pc = kernel && kernel->exit_pc ? kernel->exit_pc : PS3_NO_EXIT_PC;
}
/* Is the armed kernel still the kernel in the local store? (its code ranges hashed again: a few hundred nanoseconds) */
static int dyn_kernel_intact(const ps3_spu_executor *o, const ps3_spu_dyn *dyn) {
    return dyn->resident_kernel && ps3_spu_lifted_module_hash(o->core.ls, dyn->resident_kernel) == dyn->resident_kernel->code_hash_fast;
}
/* A DMA GET replaced code of the armed job (bit 0) or of the policy module or kernel (bit 1): forget what was hit (the executor already cleared its watch for it). */
static void dyn_take_dirty(ps3_spu_executor *o, ps3_spu_dyn *dyn) {
    const unsigned dirty = o->watch_dirty;
    if (!dirty) return;
    o->watch_dirty = 0;
    if (dirty & 1u) dyn->resident = NULL;
    if (dirty & 2u) dyn->resident_pm = NULL;
    dyn_apply(o, dyn);
}
/* Before lifted code is entered at a kernel pc: the kernel's code must still be the lifted one (else it is disarmed and runs interpreted). A job armed without a policy
 * module is forgotten here: the kernel is about to DMA the next policy module, and with no policy module armed the executor's DMA hook ends ANY lifted run that replaces
 * watched job code (the Edge zlib task at LS 0x3000 lies under the policy module area); the job is identified again at its next call. Returns 1 when the kernel is armed. */
static int dyn_kernel_check(ps3_spu_executor *o, ps3_spu_dyn *dyn) {
    if (!dyn->resident_kernel) return 0;
    int changed = 0;
    if (!dyn_kernel_intact(o, dyn)) { dyn->resident_kernel = NULL; KERNEL_COUNT(g_run.stats, kernel_disarms, KERNEL_TOTAL_DISARMS); changed = 1; }
    if (!dyn->resident_pm && dyn->resident) { dyn->resident = NULL; changed = 1; }
    if (changed) dyn_apply(o, dyn);
    return dyn->resident_kernel != NULL;
}
/* The watch was cleared from outside: forget the job and policy modules; the kernel stays when its code is unchanged. */
static void dyn_disarm(ps3_spu_executor *o, ps3_spu_dyn *dyn) {
    dyn->resident = NULL; dyn->resident_pm = NULL; o->watch_dirty = 0;
    if (dyn->resident_kernel && !dyn_kernel_intact(o, dyn)) { dyn->resident_kernel = NULL; KERNEL_COUNT(g_run.stats, kernel_disarms, KERNEL_TOTAL_DISARMS); }
    dyn_apply(o, dyn);
}

/* Do two modules' code overlap? (the Edge zlib task below the window runs under the task-set policy module, never under pm_wwsjob) */
static int dyn_overlaps_pm(const ps3_spu_lifted_module *pm, const ps3_spu_lifted_module *job) {
    for (unsigned i = 0; pm && i < pm->range_count && i < 4u; ++i)
        if (job->code_begin < pm->ranges[2u * i + 1u] && pm->ranges[2u * i] < job->code_end) return 1;
    return 0;
}

/* Identify the job now in the window. Returns the verified module or NULL; counts what it saw. */
static const ps3_spu_lifted_module *dyn_identify(ps3_spu_executor *o, ps3_spu_dyn *dyn, ps3_spu_lifted_stats *stats) {
    int signature_seen = 0;
    if (stats) ++stats->job_calls;
    for (unsigned i = 0; i < dyn->module_count; ++i) {
        const ps3_spu_lifted_module *m = dyn->modules[i];
        /* A job module starts AT the window; a task image (round 2: the Edge zlib task, SPU ELF at LS 0x3000) starts below it and runs through it:
         * the job-call watch fires when its code below the window calls into it. Either way the 16-byte signature is read at the module's own start. */
        if (!m || m->kind != PS3_LIFTED_KIND_JOB || m->code_begin > dyn->window_lo || dyn->window_lo >= m->code_end || m->code_end > SPU_LS_SIZE) continue;
        const uint8_t *start = o->core.ls + m->code_begin;
        if (m->sig_lo != load_le64(start) || m->sig_hi != load_le64(start + 8)) continue;
        signature_seen = 1;
        const uint64_t t0 = now_ns();
        const uint64_t hash = ps3_spu_lifted_hash64w(o->core.ls + m->code_begin, m->code_end - m->code_begin);
        if (stats) stats->hash_ns += now_ns() - t0;
        if (hash == m->code_hash_fast) return m;
    }
    if (stats) { if (signature_seen) ++stats->job_mismatch; else ++stats->job_unknown; }
    return NULL;
}

/* Identify the policy module in LS 0xA00 (signature, then the hash of its code ranges). NULL: not a known policy module (or its code differs). */
static const ps3_spu_lifted_module *dyn_identify_pm(ps3_spu_executor *o, ps3_spu_dyn *dyn, ps3_spu_lifted_stats *stats) {
    for (unsigned i = 0; i < dyn->module_count; ++i) {
        const ps3_spu_lifted_module *m = dyn->modules[i];
        if (!m || m->kind != PS3_LIFTED_KIND_POLICY || !m->range_count || m->range_count > 4u || m->code_end > SPU_LS_SIZE) continue;
        const uint8_t *start = o->core.ls + m->code_begin;
        if (m->sig_lo != load_le64(start) || m->sig_hi != load_le64(start + 8)) continue;
        PM_COUNT(stats, pm_tries, 0);
        const uint64_t t0 = now_ns();
        const uint64_t hash = ps3_spu_lifted_module_hash(o->core.ls, m);
        if (stats) stats->hash_ns += now_ns() - t0;
        if (hash == m->code_hash_fast) return m;
        PM_COUNT(stats, pm_mismatch, 2);
    }
    return NULL;
}

/* The interpreter reached the entry of a policy module while none is armed (the kernel branched to it): look at LS 0xA00. 1 = armed (the image was rebound), 0 = not a known module (or its code differs). */
static int dyn_arm_pm_at_entry(ps3_spu_executor *o, ps3_spu_dyn *dyn, ps3_spu_lifted_stats *stats) {
    const ps3_spu_lifted_module *pm = dyn_identify_pm(o, dyn, stats);
    if (!pm) return 0;
    dyn->resident_pm = pm; PM_COUNT(stats, pm_armed, 1);
    dyn_apply(o, dyn);
    return dyn->resident_pm == pm;
}

/* Decision 134: a job is called. The one before it (if any) did not come back through a lifted call: reported without time. */
static void job_clock_start(ps3_spu_executor *o, ps3_spu_dyn *dyn) {
    if (g_job.open && g_job.end) g_job.end(g_job.slot, 0u, 0);
    g_job.open = 0;
    if (!dyn->on_job_end) return;
    g_job.end = dyn->on_job_end;
    g_job.slot = o->acct_slot[PS3_SPU_REGION_JOB];
    g_job.return_pc = o->core.gpr[0]._u32[0] & SPU_LS_MASK;
    g_job.depth = o->core.host_depth;
    g_job.start_ns = now_ns();
    g_job.open = 1;
}
/* The drain of a lifted call ended: the job in progress returned when its link address and call depth match (the call site incremented host_depth before the drain). */
static void job_clock_drain_returned(spu_context *ctx, uint32_t return_pc) {
    if (!g_job.open || (return_pc & SPU_LS_MASK) != g_job.return_pc || ctx->host_depth != g_job.depth + 1u) return;
    g_job.open = 0;
    g_job.end(g_job.slot, now_ns() - g_job.start_ns, 1);
}

/* A job is called: the job module in the window is forgotten, the policy module is looked for when none is armed, the module in the window is identified. */
static void dyn_job_call(ps3_spu_executor *o, ps3_spu_dyn *dyn, ps3_spu_lifted_stats *stats) {
    dyn_take_dirty(o, dyn);
    dyn->resident = NULL;
    if (!dyn->resident_pm) {
        const ps3_spu_lifted_module *pm = dyn_identify_pm(o, dyn, stats);
        if (pm) { dyn->resident_pm = pm; PM_COUNT(stats, pm_armed, 1); }
    }
    const ps3_spu_lifted_module *matched = dyn->module_count ? dyn_identify(o, dyn, stats) : NULL;
    if (matched && dyn_overlaps_pm(dyn->resident_pm, matched)) matched = NULL;
    if (matched) { dyn->resident = matched; if (stats) ++stats->job_armed; }
    dyn_apply(o, dyn);
    if (dyn->on_job_call) dyn->on_job_call(dyn->hook_context, o, dyn->resident);
    job_clock_start(o, dyn);
}

/* Lifted policy code calls a job (see ps3_lifted_job_resolve in the header). The module armed before is still in the window when no DMA GET touched its code since
 * (the executor's hook would have cleared it): then there is nothing to identify again. */
void ps3_lifted_job_resolve(spu_context *ctx) {
    ps3_spu_executor *o = owner_of(ctx);
    ps3_spu_dyn *dyn = g_run.dyn;
    if (!dyn || !dyn->resident_pm) return;
    if (o->watch_dirty & 2u) return;              /* the policy module itself was replaced under us: nothing to resolve (the loop top forgets it) */
    ps3_spu_lifted_stats *stats = g_run.stats;
    PM_COUNT(stats, pm_job_hooks, 3);
    if (dyn->resident && o->watch_code_hi) {      /* the same job module again, its code untouched */
        if (stats) ++stats->job_calls;
        if (dyn->on_job_call) dyn->on_job_call(dyn->hook_context, o, dyn->resident);
        job_clock_start(o, dyn);
        return;
    }
    dyn_job_call(o, dyn, stats);
}

/* Decision 133: the first slice of a thread looks for the SPURS kernel module of the table in the local store the loader produced (the hash of its code ranges) and arms it. */
static void dyn_arm_kernel(ps3_spu_executor *o, ps3_spu_dyn *dyn, ps3_spu_lifted_stats *stats) {
    dyn->kernel_state = 1;
    for (unsigned i = 0; i < dyn->module_count; ++i) {
        const ps3_spu_lifted_module *m = dyn->modules[i];
        if (!m || m->kind != PS3_LIFTED_KIND_KERNEL || !m->range_count || m->range_count > WATCH_RANGES_MAX || m->code_end > SPU_LS_SIZE) continue;
        const uint64_t t0 = now_ns();
        const uint64_t hash = ps3_spu_lifted_module_hash(o->core.ls, m);
        if (stats) stats->hash_ns += now_ns() - t0;
        if (hash != m->code_hash_fast) { KERNEL_COUNT(stats, kernel_mismatch, KERNEL_TOTAL_MISMATCH); continue; }
        dyn->resident_kernel = m;
        KERNEL_COUNT(stats, kernel_armed, KERNEL_TOTAL_ARMED);
        dyn_apply(o, dyn);
        return;
    }
}

/* Lifted time is charged to the accounting slot of a local-store region: the kernel's while a run is in kernel code, the policy module's (or, with none armed, the job's)
 * otherwise, as before the kernel was lifted. */
static unsigned dyn_lifted_region(const ps3_spu_executor *o, const ps3_spu_dyn *dyn, uint32_t pc) {
    if (dyn->resident_kernel && o->region_map && o->region_map[(pc & SPU_LS_MASK) >> 8] == PS3_SPU_REGION_KERNEL) return PS3_SPU_REGION_KERNEL;
    return dyn->resident_pm ? PS3_SPU_REGION_POLICY : PS3_SPU_REGION_JOB;
}
static unsigned dyn_acct_slot(const ps3_spu_executor *o, unsigned region) {
    return o->acct_slot[region] < PS3_SPU_ACCT_SLOTS ? o->acct_slot[region] : 0u;
}
static void dyn_charge_lifted(const ps3_spu_executor *o, ps3_spu_dyn *dyn, uint64_t now) {
    dyn->lifted_ns[dyn_acct_slot(o, g_run.lifted_region)] += now - g_run.lifted_mark_ns;
    g_run.lifted_mark_ns = now;
}

void ps3_lifted_pm_resolve(spu_context *ctx) {
    ps3_spu_executor *o = owner_of(ctx);
    ps3_spu_dyn *dyn = g_run.dyn;
    if (!dyn || !dyn->resident_kernel) return;
    ps3_spu_lifted_stats *stats = g_run.stats;
    KERNEL_COUNT(stats, kernel_pm_calls, KERNEL_TOTAL_PM_CALLS);
    dyn_charge_lifted(o, dyn, now_ns());
    if (!dyn_kernel_intact(o, dyn)) {
        /* the code this lifted kernel was made from is not in the local store any more: a named terminal, never a run of the wrong program */
        o->binding.channel = PS3_LIFTED_OP_CODE_CHANGED; o->binding.effect_committed = 0;
        ps3_spu_execution_fail(ctx, PS3_SPU_EXEC_UNSUPPORTED);
    }
    if (o->watch_dirty) dyn_take_dirty(o, dyn);
    if (dyn->resident_kernel && !dyn->resident_pm) {
        const ps3_spu_lifted_module *pm = dyn_identify_pm(o, dyn, stats);
        if (pm) {
            dyn->resident_pm = pm;
            PM_COUNT(stats, pm_armed, 1);
            KERNEL_COUNT(stats, kernel_pm_armed, KERNEL_TOTAL_PM_ARMED);
            if (dyn->resident && dyn_overlaps_pm(pm, dyn->resident)) dyn->resident = NULL;
            dyn_apply(o, dyn);
        }
    }
    g_run.lifted_region = dyn->resident_pm ? PS3_SPU_REGION_POLICY : PS3_SPU_REGION_JOB;
}

int ps3_spu_lifted_run_slice_dyn(ps3_spu_executor *o, ps3_spu_dyn *dyn, const ps3_spu_lifted_services *services,
                                 uint64_t interp_budget, ps3_spu_interp_result *out, ps3_spu_lifted_stats *stats) {
    if (!o || !dyn || !out || !interp_budget || !o->initialized || !o->started || o->escape) return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    g_run.services = services; g_run.stats = stats; g_run.halt_opcode = 0; g_run.dyn = dyn;
    uint64_t interpreted = 0;
    int result = PS3_SPU_EXEC_OK, skip_enter = 0;
    dyn_prepare_entries(dyn);
    if (!dyn->kernel_state) dyn_arm_kernel(o, dyn, stats);                                         /* Decision 133: once, on the local store the loader produced */
    if (dyn->pm_entry_count && !o->watch_entry_bits && !dyn->resident && !dyn->resident_pm) dyn_apply(o, dyn);     /* from the first slice on the kernel's branch to a policy module is seen */
    for (;;) {
        if (o->watch_dirty) dyn_take_dirty(o, dyn);
        else if (!o->watch_entry_bits && (dyn->resident || dyn->resident_pm || dyn->resident_kernel)) dyn_disarm(o, dyn);     /* the watch was cleared from outside */
        const uint32_t pc = o->core.pc & SPU_LS_MASK & ~3u;
        const int armed = o->watch_entry_bits != NULL;
        const int irq_pending = o->core.int_enable && (o->core.event_status & o->core.event_mask);
        if (armed && !skip_enter && !irq_pending && ((o->watch_entry_bits[pc >> 5] >> ((pc >> 2) & 7u)) & 1u)) {
            if (!dyn->resident_pm && dyn->pm_entry_count && ((dyn->pm_entry_bits[pc >> 5] >> ((pc >> 2) & 7u)) & 1u)) {
                /* the entry of a policy module of the table, none armed: identify the module in LS 0xA00 (signature, then the hash of its code ranges) and arm it */
                if (dyn_arm_pm_at_entry(o, dyn, stats)) continue;
                skip_enter = 1; continue;
            }
            if (dyn_lifted_region(o, dyn, pc) == PS3_SPU_REGION_KERNEL && !dyn_kernel_check(o, dyn)) continue;   /* (rebound: the loop top decides again) */
            ps3_spu_lifted_entry entry = ps3_spu_linked_image_lookup(o->start.image, pc);
            (void)ps3_spu_executor_take_continuation(o);
            o->core.pc = pc; o->core.status = SPU_STATUS_RUNNING; o->core.stop_code = 0;
            o->core.host_depth = 0; o->core.drain_ret_pc = 0;
            if (stats) ++stats->enters;
            o->watch_in_lifted = 1;
            o->interp_paused = 0; g_run.channel_pc_exact = 0;
            const uint64_t t0 = now_ns();
            g_run.lifted_region = dyn_lifted_region(o, dyn, pc); g_run.lifted_mark_ns = t0;
            ++dyn->lifted_enters[dyn_acct_slot(o, g_run.lifted_region)];
            const int code = ps3_spu_executor_enter(o, entry);
            const uint64_t t1 = now_ns(), spent = t1 - t0;
            o->watch_in_lifted = 0;
            dyn->slice_lifted_ns += spent;
            dyn_charge_lifted(o, dyn, t1);
            if (stats) { stats->lifted_ns += spent; stats->hops_at_exit = o->hops; }
            if (o->stopped) { result = fault_result(o, out, o->terminal.code); break; }
            if (code == PS3_SPU_EXEC_STOP) {
                int changed = 0;
                if (!(services && services->skip_code_check) && o->watch_entry_bits) {
                    if (dyn->resident && ps3_spu_lifted_hash64w(o->core.ls + dyn->resident->code_begin, dyn->resident->code_end - dyn->resident->code_begin) != dyn->resident->code_hash_fast) changed = 1;
                    if (dyn->resident_pm && ps3_spu_lifted_module_hash(o->core.ls, dyn->resident_pm) != dyn->resident_pm->code_hash_fast) changed = 1;
                    if (dyn->resident_kernel && !dyn_kernel_intact(o, dyn)) changed = 1;
                }
                if (changed) {
                    o->stopped = 1;
                    o->terminal = (ps3_spu_executor_terminal){ PS3_SPU_EXEC_UNSUPPORTED, o->provider_status, PS3_LIFTED_OP_CODE_CHANGED,
                                                               o->binding.request, o->progress, 0, PS3_CLOCK_OK };
                    result = fault_result(o, out, PS3_SPU_EXEC_UNSUPPORTED);
                    break;
                }
                memset(out, 0, sizeof *out);
                out->kind = PS3_SPU_INTERP_STOPPED; out->pc = o->core.pc & SPU_LS_MASK;
                out->code = o->core.stop_code & 0x3FFFu; out->instructions = interpreted;
                break;
            }
            if (code == PS3_LIFTED_PAUSE_ESCAPE) { skip_enter = 1; continue; }       /* interpret from o->core.pc */
            if (code == PS3_SPU_EXEC_KERNEL_EXIT) {
                /* Lifted code branched to the kernel's exit entry (policy.exit_pc, set only while the kernel is armed): the exit resets the guest stack, so the C frames the
                 * unwind dropped held nothing the guest can return to. The kernel is entered again at the exit entry (o->core.pc). */
                (void)ps3_spu_executor_take_continuation(o);
                o->core.host_depth = 0; o->core.drain_ret_pc = 0;
                KERNEL_COUNT(stats, kernel_exits, KERNEL_TOTAL_EXITS);
                if (!dyn->resident_kernel) skip_enter = 1;
                continue;
            }
            if (code == PS3_SPU_EXEC_UNSUPPORTED && (dyn->resident_pm || dyn->resident_kernel) && g_run.channel_pc_exact && o->interp_paused && o->interp_paused_pc == o->core.pc &&
                !(o->core.pc & 3u) && is_channel_word(spu_ls_read32(&o->core, o->core.pc & SPU_LS_MASK))) {
                /* Lifted policy code reached a channel read it cannot complete (the channel code paused at the instruction the generator named): the interpreter executes that
                 * instruction, waits for the event as it always did, takes interrupts, and lifted code is entered again at the next genuine entry. */
                (void)ps3_spu_executor_take_continuation(o);
                o->core.host_depth = 0; o->core.drain_ret_pc = 0;
                PM_COUNT(stats, pm_pause_escapes, 5);
                skip_enter = 1; continue;
            }
            if (code == PS3_SPU_EXEC_INVALID) { result = code; break; }
            o->stopped = 1;                                                           /* a pause the glue cannot resume: a named terminal */
            result = fault_result(o, out, code);
            break;
        }
        /* Interpret. After an escape the first instruction runs alone when an enabled interrupt is pending: the interpreter
         * delivers it at that instruction, and entering lifted code again at the same pc would just escape again. */
        uint64_t step = interp_budget - interpreted;
        if ((skip_enter || (armed && irq_pending)) && irq_pending) step = 1;
        ps3_spu_interp_result r;
        result = ps3_spu_executor_run_interp(o, pc, step, &r);
        if (result != PS3_SPU_EXEC_OK) break;
        skip_enter = 0;
        interpreted += r.instructions;
        if (stats) { stats->interp_instructions += r.instructions; ++stats->interp_calls; }
        if (r.kind == PS3_SPU_INTERP_BUDGET && o->watch_hit) {
            const unsigned hit = o->watch_hit; o->watch_hit = PS3_SPU_WATCH_NONE;
            if (hit == PS3_SPU_WATCH_JOB_CALL) dyn_job_call(o, dyn, stats);
            if (interpreted < interp_budget) continue;                                 /* the entry (or the next block) is taken at the loop top */
        }
        r.instructions = interpreted; *out = r; break;
    }
    if (o->watch_in_lifted) o->watch_in_lifted = 0;
    if (stats && o->watch_disarms) stats->dma_disarms = o->watch_disarms;
    g_run.services = NULL; g_run.stats = NULL; g_run.dyn = NULL;
    return result;
}
