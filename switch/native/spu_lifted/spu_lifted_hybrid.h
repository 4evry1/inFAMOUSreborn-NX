#ifndef PS3_NATIVE_SPU_LIFTED_HYBRID_H
#define PS3_NATIVE_SPU_LIFTED_HYBRID_H
/* Hybrid lifted/interpreted execution of ONE known SPU program on a ps3_spu_executor owner.
 *
 * Why hybrid: the Windows port's lifted code (the "rc" register-cached twins) covers the basic blocks the lifter could
 * reach from the ELF. It cannot resume after a `stop` (the lifter ends the block there and emits nothing after it),
 * it has stubs for branch targets it never lifted, and an indirect branch may land anywhere. All three are handled the
 * way the Windows runtime handles them: INTERPRET from that pc until the next genuinely lifted entry, then re-enter
 * lifted code. The owner therefore runs in INTERPRETER mode (that mode serves the channels the program needs: in/out
 * mailboxes, event mask, decrementer write) AND has the module's linked image bound, because ps3_spu_executor_enter
 * checks only the bound image id, never the mode, and ps3_spu_executor_start stores the config in either mode.
 *
 * Contract with the executor (see spu_executor.h, spu_lifted_boundary.c):
 *   - the bound image is an EXHAUSTIVE linked image: every 4-byte aligned pc in [4, SPU_LS_SIZE) has a row. Genuine
 *     lifted functions map to themselves, everything else maps to ps3_lifted_escape. A hop (the owned continuation
 *     machinery, used for indirect branches and returns whose C frame is gone) can therefore never be refused for
 *     "no lifted function here"; it lands in the escape, which hands the pc to the interpreter.
 *   - indirect-branch hops need o->policy.select != NULL in spu_lifted_boundary.c; a never-matching dummy policy
 *     (select_pc == exit_pc == 0xFFFFFFFF) is installed. It is never called.
 *   - `stop` and escapes UNWIND the C frames with ps3_spu_executor_pause (a recoverable longjmp to the enter scope)
 *     after resetting host_depth to 0. The guest call stack lives in the local store (link registers, stack
 *     pointer), so nothing guest-visible is lost; returns whose C frame is gone are published as the indirect-branch
 *     marker at host_depth 0 and resolved by the hop machinery above.
 *   - channel waits the interpreter would turn into PAUSED are performed IN PLACE here (poll + sleep), because the
 *     lifted code is not resumable at an arbitrary instruction.
 * Threading: everything here is per host thread (the thread that calls ps3_spu_lifted_run_slice); the glue keeps its
 * thread-local state only for the duration of that call. */
#include "../adapters/spu_executor.h"
#include "../adapters/spu_linked_image.h"
#ifdef __cplusplus
extern "C" {
#endif

/* One row of a module's sorted lifted-function list. flags: 0 = lifter stub (an unresolved branch target; an escape to
 * the interpreter), PS3_LIFTED_ROW_GENUINE = lifted code, PS3_LIFTED_ROW_LOSSY = additionally contains a word the lifter
 * could not translate (data decoded as code in a constant pool): the program never runs it, and it is NOT equivalent to
 * the interpreter if it did. */
enum { PS3_LIFTED_ROW_GENUINE = 1, PS3_LIFTED_ROW_LOSSY = 2 };
typedef struct { uint32_t pc; void (*function)(spu_context *); uint32_t flags; } ps3_spu_lifted_row;

typedef struct ps3_spu_lifted_module {
    const char *name;
    int32_t image_id;                       /* the id the owner executes the image as (spu_context.image_id) */
    uint32_t entry_pc;                      /* the SPU ELF entry the module was lifted from */
    uint32_t code_begin, code_end;          /* LS range whose loaded content defines the module */
    uint64_t code_hash;                     /* FNV-1a 64 over LS[code_begin, code_end): the identity check, once per thread start */
    uint64_t code_hash_fast;                /* ps3_spu_lifted_hash64w over the same range: the cheap re-check at every lifted stop */
    const ps3_spu_lifted_row *rows;         /* sorted by pc, unique, 4-byte aligned, 0 < pc < SPU_LS_SIZE */
    unsigned row_count;
    /* Dynamic (job / overlay) modules only; 0/0 for a static module. The first 16 bytes at code_begin (the job's SpuGUID) as two
     * little-endian u64: the cheap test before the hash (Decision 104, native/spu_lifted/jobs/). */
    uint64_t sig_lo, sig_hi;
    /* Round 2: a POLICY module (kind PS3_LIFTED_KIND_POLICY: pm_wwsjob, LS 0xA00..) is identified by the signature at code_begin and the hash of its CODE ranges
     * only (ps3_spu_lifted_module_hash: its data lives between them and changes while it runs): `ranges` is range_count pairs {lo, hi} (at most 4). A job module has
     * range_count 0 and is hashed over [code_begin, code_end). code_hash_fast of a policy module is that module hash. */
    const uint32_t *ranges;
    unsigned range_count;
    unsigned kind;
    /* Decision 133: a KERNEL module (the SPURS kernel of libsre, LS 0x100..0x880, native/spu_lifted/kernel) is the thread's own resident program: identified once,
     * at the first slice, by the hash of its code ranges, and laid into every image the owner is bound to. exit_pc is its exit entry (0x808): an indirect transfer
     * there abandons every guest frame (it resets the stack), so lifted code leaves its C frames with PS3_SPU_EXEC_KERNEL_EXIT and is entered again at exit_pc.
     * 0 for every other module. */
    uint32_t exit_pc;
} ps3_spu_lifted_module;
/* PS3_LIFTED_KIND_GUARDED (Decision 134): code the guest WRITES at run time (pm_wwsjob's register restore sequence at LS 0x3FEC0) whose lifted functions check
 * their own bytes every time they run and escape when they differ; laid into every image while the kernel is armed. */
enum { PS3_LIFTED_KIND_JOB = 0, PS3_LIFTED_KIND_POLICY = 1, PS3_LIFTED_KIND_KERNEL = 2, PS3_LIFTED_KIND_GUARDED = 3 };
/* The identity hash of `m` over the local store `ls`: hash64w of [code_begin, code_end) for a job module, the fold of hash64w over its ranges for a policy module. */
uint64_t ps3_spu_lifted_module_hash(const uint8_t *ls, const ps3_spu_lifted_module *m);

/* FNV-1a 64 over a byte range (the hash the module identity uses). */
uint64_t ps3_spu_lifted_fnv1a64(const uint8_t *bytes, size_t length);
/* Four-lane 64-bit-word multiply/xor-shift hash (~25x faster than FNV: about 5 us for the 98 KB Bink code range). Words are
 * read little-endian; a final partial word is zero padded; the length is mixed in. gen_bink_lifted.py implements the same. */
uint64_t ps3_spu_lifted_hash64w(const uint8_t *bytes, size_t length);
/* The module among modules[0..count) whose entry matches and whose code range hashes to its code_hash in `ls` (a freshly
 * loaded 256 KiB local store), or NULL: the caller then stays on the interpreter. */
const ps3_spu_lifted_module *ps3_spu_lifted_find(const uint8_t *ls, uint32_t entry_pc,
                                                 const ps3_spu_lifted_module *const *modules, unsigned count);
/* The exhaustive linked image of a module (built once per module, thread safe). NULL if the rows are malformed. */
const ps3_spu_linked_image *ps3_spu_lifted_image(const ps3_spu_lifted_module *module);
/* 1 when `pc` starts a genuine lifted function of the module. */
int ps3_spu_lifted_is_genuine(const ps3_spu_lifted_module *module, uint32_t pc);

/* Admits `o` (initialized, not started) for hybrid execution of `module` and starts it. Sets INTERPRETER mode, binds the
 * module's image with image_id, installs the dummy policy. The caller then loads the local store and the registers and
 * sets core.pc exactly as for an interpreter owner. Returns an executor code (PS3_SPU_EXEC_OK on success). */
int ps3_spu_lifted_start(ps3_spu_executor *o, const ps3_spu_lifted_module *module, ps3_clock clock);

typedef struct ps3_spu_lifted_services {
    void *context;
    /* A channel operation needs a condition only the PPU (or a peer SPU) can provide: SPU_RdInMbox with no word,
     * SPU_WrOutMbox / SPU_WrOutIntrMbox with the mailbox full. Called repeatedly until the condition holds; it should
     * serve what it can (e.g. the out-interrupt mailbox) and sleep a bounded moment. Return nonzero to end the owner
     * (the glue then fails it with PS3_SPU_EXEC_CANCELLED). NULL: poll the executor's checkpoint and sleep 100 us. */
    int (*wait)(void *context, spu_context *ctx, uint32_t channel);
    /* SPU_WrOutIntrMbox was just written by lifted code. The interpreter path delivers the interrupt mailbox at the next
     * slice boundary; lifted code runs many blocks per slice, so the runner may deliver here instead. NULL: nothing. */
    void (*intr_written)(void *context, spu_context *ctx);
    /* Lifted code is the static program. At every lifted stop (about once per decoder job) the glue re-hashes the module's
     * code range in the local store (~100 us) and ends the owner with PS3_LIFTED_OP_CODE_CHANGED if a DMA or a store has
     * altered it. Nonzero skips that check. */
    unsigned skip_code_check;
} ps3_spu_lifted_services;
/* terminal.channel of a FAULT the glue itself raised (above every PS3_SPU_EXEC_OP_*). */
enum { PS3_LIFTED_OP_CODE_CHANGED = 0x10100u };

typedef struct ps3_spu_lifted_stats {
    uint64_t enters, escapes, stops, interp_instructions, interp_calls, waits, hops_at_exit;
    /* dynamic job modules (ps3_spu_lifted_run_slice_dyn); all zero for a static module */
    uint64_t job_calls, job_armed, job_unknown, job_mismatch, lifted_ns, dma_disarms, irq_escapes, hash_ns;
    /* escapes by reason (PS3_ESC_*, round 2; counted only when PS3_SPU_ESCAPE_DIAG is defined) */
    uint64_t esc_stub, esc_norow, esc_out, esc_hybrid;
    /* policy module (pm_wwsjob) lifted beside the jobs: identifications tried / armed / content mismatches, job calls resolved by lifted policy code, composite images built,
     * channel stalls of lifted policy code handed to the interpreter at the channel instruction */
    uint64_t pm_tries, pm_armed, pm_mismatch, pm_job_hooks, pm_composites, pm_pause_escapes;
    /* Decision 133, the lifted SPURS kernel: armed at thread start (0/1), content mismatch at thread start, policy-module calls made by lifted kernel code
     * (ps3_lifted_pm_resolve), policy modules armed there, kernel exits (PS3_SPU_EXEC_KERNEL_EXIT re-entries), disarms because a DMA replaced its code */
    uint64_t kernel_armed, kernel_mismatch, kernel_pm_calls, kernel_pm_armed, kernel_exits, kernel_disarms;
} ps3_spu_lifted_stats;

/* Why lifted job code handed control to the interpreter (an "escape"), classified by the pc it handed over:
 *   STUB    a function the lifter could not translate exactly (a stub row: lossy / plain stub / unmatched stop): the module's own gap
 *   NOROW   a pc inside the module's code range that starts no lifted function (an indirect branch into the middle of a function, data)
 *   OUT     a pc outside the module's code range: the policy module or kernel (a return to the PM, a call of a PM helper)
 *   IRQ     an enabled interrupt was pending at a block transfer / call return (irq_escape)
 *   HYBRID  a transfer the boundary would refuse but the interpreter can execute (LS 0, misaligned pc, unmatched return, deep recursion)
 * PS3_ESC_AUTO asks ps3_lifted_escape to classify by pc (what the generated code's stub rows call). */
enum { PS3_ESC_STUB = 0, PS3_ESC_NOROW = 1, PS3_ESC_OUT = 2, PS3_ESC_IRQ = 3, PS3_ESC_HYBRID = 4, PS3_ESC_REASONS = 5, PS3_ESC_AUTO = 5 };
/* The PS3_ESC_STUB / NOROW / OUT class of an escape to `pc` while module `m` is armed (the classification ps3_lifted_escape applies; defined
 * when PS3_SPU_ESCAPE_DIAG is). */
unsigned ps3_spu_lifted_classify_escape(const ps3_spu_lifted_module *m, uint32_t pc);
enum { PS3_ESC_TOP = 6 };   /* heavy-hitter counters per accounting slot (Misra-Gries: any pc with more than 1/7 of the escapes is kept) */

/* One slice of the hybrid owner, result-compatible with ps3_spu_executor_run_interp's ps3_spu_interp_result:
 *   STOPPED  lifted code executed `stop` (pc = the instruction after it, code = the 14-bit stop code) or the
 *            interpreter did. Resumable: call again.
 *   PAUSED   the interpreter paused on a channel (pc at the channel instruction). Resumable: call again.
 *   BUDGET   the interpreter ran `interp_budget` instructions without reaching a genuine lifted entry. Resumable.
 *   FAULT    a terminal (the owner is stopped): code/channel/stop_code/opcode as for the interpreter.
 * Lifted code has no instruction budget: it runs until a stop, an escape, a fault, or a wait that returns nonzero.
 * `interp_budget` bounds only interpreted stretches (so cancellation stays reachable). The function returns an
 * executor code like ps3_spu_executor_run_interp (OK when *out is valid). `stats` may be NULL. */
int ps3_spu_lifted_run_slice(ps3_spu_executor *o, const ps3_spu_lifted_module *module, const ps3_spu_lifted_services *services,
                             uint64_t interp_budget, ps3_spu_interp_result *out, ps3_spu_lifted_stats *stats);

/* ================================================================================ dynamic (job) modules, Decision 104
 * A SPURS thread runs the (interpreted) kernel and policy module; jobs stream into the job window (LS 0x4000) one at a time and are
 * entered by a link-setting branch from the policy module. The interpreter reports that branch (PS3_SPU_WATCH_JOB_CALL); the glue
 * then
 *   1. reads the 16-byte signature at the window and, for every module with that signature, hashes the module's code range as it is
 *      in the local store NOW (ps3_spu_lifted_hash64w against code_hash_fast); no match: the job stays interpreted;
 *   2. on a match ARMS the owner: the module's genuine-entry bitmap goes to owner->watch_entry_bits, the module's image is bound
 *      (start.image / image ids), and every later taken transfer to a genuine entry (a return from a policy-module call, an
 *      interrupt handler's iret, a loop back edge of interpreted code) ends the interpreter run so lifted code takes over at once;
 *   3. a DMA GET over the module's code range disarms (executor finish_transfer); lifted code running at that moment is stopped
 *      with PS3_LIFTED_OP_CODE_CHANGED_DMA; a `stop` re-hashes the code like the static module does.
 * Anything else is an escape, as for the static module: unknown targets, lossy functions, a pending enabled interrupt (the
 * lifted job's block transfers and call returns hand the exact state to the interpreter, which delivers it), channels the glue
 * does not wait for. */
enum { PS3_SPU_LS_BITMAP_BYTES = SPU_LS_SIZE / 4 / 8 };   /* one bit per instruction word of the local store */
typedef struct ps3_spu_dyn {
    const ps3_spu_lifted_module *const *modules;   /* the set, may be empty (accounting only) */
    unsigned module_count;
    uint32_t window_lo;                            /* the job code window base (0x4000) */
    const ps3_spu_lifted_module *resident;         /* armed job module or NULL (worker only) */
    const ps3_spu_lifted_module *resident_pm;      /* armed policy module or NULL (worker only); with `resident` the bound image is their composite */
    /* Called at every job call (before identification of lifted code is acted on) with the matched module or NULL; the group runner
     * uses it to pick the accounting slot of the job and of the policy module. NULL: nothing. */
    void (*on_job_call)(void *context, ps3_spu_executor *o, const ps3_spu_lifted_module *matched);
    void *hook_context;
    /* Decision 134: a job's run time, measured from its call (on_job_call) to its return to the policy module (the drain of the policy module's lifted call
     * returning at the job's link address, spu_lifted_boundary.c): slot = the job's accounting slot. A job that does not return through a lifted call (it ran
     * interpreted, or escaped) is reported with returned = 0 and no time when the next job is called. NULL: nothing measured. */
    void (*on_job_end)(unsigned slot, uint64_t ns, int returned);
    /* Time lifted code ran since the runner last flushed, in total and by accounting slot (the slot of the job region at the time
     * of the entry, owner->acct_slot[PS3_SPU_REGION_JOB]); the runner folds and zeroes them every slice. */
    uint64_t slice_lifted_ns;
    uint64_t lifted_ns[PS3_SPU_ACCT_SLOTS], lifted_enters[PS3_SPU_ACCT_SLOTS];
    /* Escape diagnostics (PS3_SPU_ESCAPE_DIAG), per accounting slot of the job region, folded and zeroed by the runner every slice
     * (ps3_spu_acct_flush_escapes): escapes by reason, and the heaviest target pcs as (pc | reason << 24, hits). */
    unsigned esc_dirty;
    uint32_t esc_n[PS3_SPU_ACCT_SLOTS][PS3_ESC_REASONS];
    uint32_t esc_key[PS3_SPU_ACCT_SLOTS][PS3_ESC_TOP], esc_hits[PS3_SPU_ACCT_SLOTS][PS3_ESC_TOP];
    /* Decision 133: the SPURS kernel module when its code is in the local store (identified at the first slice; NULL: the kernel runs interpreted). With it armed every
     * bound image carries its rows (composites keyed by kernel, policy module and job module), its exit entry is the owner's policy.exit_pc, and the image id is its id.
     * kernel_state: 0 not looked for yet, 1 looked for (once per thread start). */
    const ps3_spu_lifted_module *resident_kernel;
    unsigned kernel_state;
    /* Policy modules are armed when the SPURS kernel branches to one's entry (a transfer to a bit of pm_entry_bits is a watch hit while none is armed), not only at the
     * first job call. Built lazily from `modules` (pm_entry_state: 0 not built, 1 built); union_bits = the armed job's genuine entries plus the policy entries, for `union_for`. */
    unsigned pm_entry_state, pm_entry_count;
    const uint8_t *union_src;                      /* the genuine bitmap union_bits was made from */
    uint8_t pm_entry_bits[PS3_SPU_LS_BITMAP_BYTES], union_bits[PS3_SPU_LS_BITMAP_BYTES];
} ps3_spu_dyn;
/* Admits `o` (initialized, not started) for dynamic job execution: INTERPRETER mode with the all-escape placeholder image bound,
 * the dummy policy of ps3_spu_lifted_start. The caller sets o->region_map (and its accounting slots) itself. */
int ps3_spu_lifted_dyn_start(ps3_spu_executor *o, ps3_clock clock);
/* One slice, result-compatible with ps3_spu_lifted_run_slice. A job-call watch hit is handled inside (identify, account, go on);
 * a lifted entry hit enters lifted code. `interp_budget` bounds interpreted instructions only. */
int ps3_spu_lifted_run_slice_dyn(ps3_spu_executor *o, ps3_spu_dyn *dyn, const ps3_spu_lifted_services *services,
                                 uint64_t interp_budget, ps3_spu_interp_result *out, ps3_spu_lifted_stats *stats);

/* Called by lifted POLICY-module code at the instruction that calls a job (the generator puts it before the indirect call of function 0x31C8, after ctx->pc holds the
 * target): identifies the job module now in the window, arms it beside the policy module (or forgets the previous one) and runs the accounting hook. No-op unless a
 * policy module is armed. */
void ps3_lifted_job_resolve(spu_context *ctx);
/* Process-wide policy-module counters: tries, armed, mismatches, job hooks, composites bound, pause escapes (accounting report). */
void ps3_spu_lifted_pm_totals(uint64_t out[6]);
/* Called by lifted KERNEL code at its call of the policy module (function 0x7D8; the generator puts it after ctx->pc holds the target): the kernel
 * has just DMA'd the workload's policy module to LS 0xA00, so a replaced module is forgotten and the module now there is identified and armed beside the kernel before the
 * lifted call looks the target up in the bound image. No-op unless the kernel is armed. */
void ps3_lifted_pm_resolve(spu_context *ctx);
/* Process-wide lifted-kernel counters (Decision 133): threads armed, content mismatches, policy-module calls, policy modules armed by them, kernel exits, disarms. */
enum { PS3_LIFTED_KERNEL_TOTALS = 6 };
void ps3_spu_lifted_kernel_totals(uint64_t out[PS3_LIFTED_KERNEL_TOTALS]);
/* Channel operations of lifted policy code carry the address of their instruction: a read that cannot complete now (no selected event, no tag status, no stall status)
 * leaves lifted code HERE and the interpreter executes the instruction, which blocks and takes interrupts as it always did. */
u128 ps3_lifted_rdch_at(spu_context *ctx, uint32_t channel, uint32_t instruction_address);
void ps3_lifted_wrch_at(spu_context *ctx, uint32_t channel, u128 value, uint32_t instruction_address);

/* ---- entry points the adapted lifted translation unit calls (bink_lifted_prelude.h renames the runtime's) ---- */
u128 ps3_lifted_rdch(spu_context *ctx, uint32_t channel);
void ps3_lifted_wrch(spu_context *ctx, uint32_t channel, u128 value);
uint32_t ps3_lifted_rchcnt(spu_context *ctx, uint32_t channel);
enum { PS3_LIFTED_NO_STOP_WORD = 0xFFFFFFFFu };
#ifdef __cplusplus
[[noreturn]] void ps3_lifted_stop_at(spu_context *ctx, uint32_t stop_address);
[[noreturn]] void ps3_lifted_halt_at(spu_context *ctx, uint32_t instruction_address);
[[noreturn]] void ps3_lifted_escape(spu_context *ctx);
#else
_Noreturn void ps3_lifted_stop_at(spu_context *ctx, uint32_t stop_address);
_Noreturn void ps3_lifted_halt_at(spu_context *ctx, uint32_t instruction_address);
_Noreturn void ps3_lifted_escape(spu_context *ctx);
#endif
/* Dynamic job modules: an enabled interrupt is pending at a lifted block transfer (jobs' SPU_FAST_XFER). Counts, then escapes. */
#ifdef __cplusplus
[[noreturn]] void ps3_lifted_irq_escape(spu_context *ctx);
#else
_Noreturn void ps3_lifted_irq_escape(spu_context *ctx);
#endif
#ifdef __cplusplus
}
#endif
#endif
