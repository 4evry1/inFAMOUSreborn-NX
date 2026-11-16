#ifndef PS3_NATIVE_SPU_EXECUTOR_H
#define PS3_NATIVE_SPU_EXECUTOR_H
#ifndef PS3_NATIVE_SPU_EXECUTOR
#error "The owned executor requires PS3_NATIVE_SPU_EXECUTOR"
#endif
#include "spu_memory.h"
#include "spu_dma.h"
#include "spu_execution_port.h"
#include "spu_decrementer.h"
#include "spu_linked_image.h"
#include <setjmp.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;
    int (*access)(void *,uint64_t,size_t,int);
    int (*checkpoint)(void *,unsigned);
    int (*reserve_trace)(void *,const ps3_spu_transfer *);
    void (*finish_trace)(void *,const ps3_spu_transfer *,ps3_guest_memory_status,int,const void *);
} ps3_spu_executor_services;
/* channel names the stopping operation (an SPU channel or a PS3_SPU_EXEC_OP_* id). request is the last transfer
 * request begun on this owner: for a transfer/command stop it is that stop's request, but a non-transfer stop
 * (other channels, drain, restore, stop, entry return) does not rewrite it, so it may describe an EARLIER transfer;
 * read it together with channel and the checkpoint phase. effect_committed is the stopping operation's commit flag:
 * for a transfer, whether its bytes were committed; for a channel operation, whether its state effect (mask/parameter
 * write, tag/atomic slot or stall-mask consumption) was committed before a POST_CHANNEL stop (RdDec has no state
 * effect: 0); 0 for every refusal before an effect, PRE_* stops, and boundary/continuation stops. progress is the owner's last COMMITTED list cursor (published after each list
 * element's transfer succeeds); it is never reset per command, so it may belong to an earlier list command and says
 * nothing about the stopping request itself. */
typedef struct {
    int code; ps3_guest_memory_status provider_status; uint32_t channel;
    ps3_spu_transfer request; ps3_spu_list_progress progress; unsigned effect_committed;
    ps3_clock_status clock_status;  /* provider status of the stopping decrementer read (PS3_SPU_EXEC_CLOCK), else OK */
} ps3_spu_executor_terminal;
typedef void (*ps3_spu_lifted_entry)(spu_context *);
/* Native interpreter outcome (spu_interp_native.c). pc is always the owner's core.pc at the terminal:
 *   STOPPED: a `stop`/`stopd` executed. code is the 14-bit stop immediate (the interpreter does NOT interpret its
 *     meaning -- C1b does). pc is the instruction after the stop, as hardware leaves it. Resumable at pc.
 *   PAUSED: a channel the interpreter cannot satisfy without host work (an empty in-mailbox, a full out mailbox, a
 *     blocking RdEventStat). pc is left AT the channel instruction, so re-running retries it after the PPU acts.
 *   BUDGET: the instruction budget ran out; pc is the next unexecuted instruction. Resumable at pc.
 *   FAULT: a terminal stop through the executor's existing fail path (unsupported channel/opcode, memory failure,
 *     checkpoint cancellation, clock failure, a conditional halt). code is that executor code; channel names the
 *     offending channel or PS3_SPU_EXEC_OP_* id. stop_code carries the architectural halt value: 0x1000 for a decoded
 *     heq/hgt/hlgt assertion halt, 0 for any other fault. opcode carries the raw instruction word that halted (so an
 *     opcode the decoder tables do not name is reported by its own number), 0 when the fault was not a halt.
 * instructions is the count executed by that call (a stopped instruction is not counted). */
typedef enum { PS3_SPU_INTERP_STOPPED = 0, PS3_SPU_INTERP_PAUSED = 1, PS3_SPU_INTERP_BUDGET = 2, PS3_SPU_INTERP_FAULT = 3 } ps3_spu_interp_kind;
typedef struct {
    ps3_spu_interp_kind kind;
    uint32_t pc, code, channel, stop_code, opcode;
    uint64_t instructions;
} ps3_spu_interp_result;
/* Per-owner SPU event bits (spu_context event_status/event_mask/event_ack_mask). Only the bits with a native producer
 * or an unambiguous meaning are named: LR (lock-line reservation lost, RPCS3 SPU_EVENT_LR) and SN (MFC
 * stall-and-notify, spu_dma.h PS3_SPU_EVENT_SN -- the same bit, so it is named once here and aliased below). */
/* Root fix (C1b): LR is 0x400 (CBE SPU event layout; source/ps3recomp/runtime/spu/spu_coherency.h:37 SPU_EVENT_LR).
 * C1a used 0x04, which self-consistent tests could not catch; the real SPURS kernel enables 0x400. */
enum { PS3_SPU_EVENT_NONE = 0u, PS3_SPU_EVENT_LOCK_LINE_LOST = 0x400u };
/* PS3_SPU_EVENT_STALL_NOTIFY is PS3_SPU_EVENT_SN (spu_dma.h, reached through spu_execution_port.h). */
typedef struct {
    void *context;
    int (*select)(void *,uint32_t,unsigned,uint64_t *);
    uint32_t select_pc, exit_pc;
} ps3_spu_policy_callbacks;
/* Which code an owner runs. LIFTED (the default, and what every existing composition gets): the linked image, entered
 * at a lifted function (enter/slice). INTERPRETER (Decision 33 / C1a): arbitrary SPU code decoded from the owner's own
 * local store by ps3_spu_executor_run_interp; the owner needs no linked image.
 *
 * This is a property of the OWNER, not of one start call, so it is set with a call (ps3_spu_executor_set_interpreter
 * below) rather than a new start-config field: adding a field there would silently change what every existing
 * brace-initialised ps3_spu_executor_start_config means. It must be set BEFORE start, and start refuses an owner whose
 * linked image does not match its mode. */
typedef enum { PS3_SPU_EXECUTION_MODE_LIFTED = 0, PS3_SPU_EXECUTION_MODE_INTERPRETER = 1 } ps3_spu_execution_mode;
typedef struct {
    /* In LIFTED mode all fields are mandatory (no fallback clock):
     *   image/image_id: the lifted image actually linked into this composition and the id the owner executes it as;
     *     enter requires core.image_id==image_id, restores compare against image_id, continuations must be image functions.
     *     INTERPRETER mode ignores image and image_id (NULL / any id is accepted, nothing is linked).
     *   clock: caller-owned composition clock. Intended: the same ps3_clock the native PPU timebase uses (one guest
     *     timebase). NOT INTEGRATED: no native startup path calls start yet; the diagnostic passes the Horizon tick clock.
     *   thread_option: SPU thread attribute option bits affecting the decrementer (NONE or DEC_SYNC_TB_ENABLE).
     *   hop_budget: maximum continuation hops for this owner's lifetime (nonzero in LIFTED mode; INTERPRETER never
     *     takes a hop, so 0 is accepted there). */
    const ps3_spu_linked_image *image; int32_t image_id;
    ps3_clock clock; uint32_t thread_option; uint64_t hop_budget;
} ps3_spu_executor_start_config;
typedef struct ps3_spu_executor {
    spu_context core;
    mfc_engine engine;
    ps3_spu_memory memory;
    ps3_spu_execution_port binding;
    ps3_spu_executor_services services;
    ps3_spu_list_progress progress;
    ps3_spu_executor_terminal terminal;
    jmp_buf *escape;
    ps3_guest_memory_status provider_status;
    unsigned initialized, stopped, trace_reserved;
    /* Diagnostic counts of generated-bracket calls that reached the native out-of-line boundary (incremented on
     * entry to spu_drain_call / spu_img_restore, before their guards). Not guest state. */
    unsigned boundary_drains, boundary_restores;
    /* Owned continuation: the single pending lifted transfer published by this owner's generated code through
     * ps3_spu_native_continuation_slot(ctx) (no thread-local slot natively). Taken only by this owner's drain/runner
     * after validation, or by the caller via ps3_spu_executor_take_continuation after a terminal. */
    ps3_spu_lifted_entry continuation;
    ps3_spu_executor_start_config start;
    ps3_spu_decrementer decrementer;
    ps3_clock_status clock_status;
    unsigned started;
    /* Decision 33 / C1a: admitted execution mode and interpreter request state. interp_paused is the channel
     * instruction a PAUSED run left to retry (only meaningful while SAME pc and interp_paused is set). */
    unsigned mode;
    /* Optional per-operation channel observer (ps3_spu_executor_set_channel_observer); NULL in composition. */
    void (*observe_channel)(void *,unsigned,uint32_t);
    void *observe_context;
    uint32_t interp_paused_pc;
    unsigned interp_paused;
    /* Interpreter channel effects that do not belong to spu_context: the SPU_WrDec reload and the running latched
     * decrementer (zero = the RdDec clock source is in use, no reload latched). */
    uint32_t decrementer_reload;
    uint64_t decrementer_latch_microseconds;
    unsigned decrementer_latched;
    unsigned borrowed_owner, slice_mode;
    uint64_t slice_end;
    ps3_spu_policy_callbacks policy;
    uint64_t hops;                       /* continuation hops taken (diagnostic) */
    unsigned decrementer_reads;          /* successful RdDec clock samples, including one dropped by a POST_CHANNEL cancel (diagnostic) */
    uint64_t decrementer_last_raw;       /* full 64-bit read_dec result of the last successful RdDec (diagnostic) */
    /* ---- Decision 104 (2026-11-13): image accounting and the lifted-job watch. Every field is touched only by the owner's
     * worker thread (the interpreter, the DMA hook and the hybrid glue all run on it); zero/NULL = off, which is what an
     * owner has unless the group runner arms it. */
    const uint8_t *region_map;           /* local-store page (256 bytes) -> PS3_SPU_REGION_*; NULL = no region work in the interpreter */
    uint8_t acct_region;                 /* region of the last interpreted instruction */
    uint16_t acct_slot[4];               /* accounting slot of each region (kernel/image, policy module, job, other) */
    uint64_t acct_mark;                  /* instructions of the current interpreter call already attributed */
    uint64_t acct_instr[96];             /* interpreted instructions per accounting slot since the last flush (PS3_SPU_ACCT_SLOTS) */
    const uint8_t *watch_entry_bits;     /* armed: bit ((pc>>2)&7) of byte (pc>>5) = a genuine lifted entry of the resident job module */
    uint32_t watch_code_lo, watch_code_hi; /* armed: the resident module's verified code range; a DMA GET overlapping it disarms */
    unsigned watch_hit;                  /* set by the interpreter when it ended its run on a watch point (PS3_SPU_WATCH_*) */
    unsigned watch_in_lifted;            /* set by the hybrid glue while lifted code runs: a DMA into the code range is then fatal */
    uint64_t watch_disarms;              /* diagnostic: DMA GETs that overlapped the armed code range */
    /* Round 2: a resident POLICY module (pm_wwsjob, LS 0xA00..) beside the job module. Its code is a few ranges (its data lives between them, and changes); a DMA GET over one
     * disarms the policy module only. finish_transfer sets watch_dirty (bit 0: the job range was hit, bit 1: a policy-module range) and the glue consumes it.
     * A lifted run is a named terminal only when the hit module's own code is the one running (core.pc inside it): the policy module loading the next job over the
     * old one is not. With no policy module resident (watch_pm_count 0) every hit during a lifted run is fatal, as before. */
    uint32_t watch_pm_lo[4], watch_pm_hi[4];
    unsigned watch_pm_count, watch_dirty;
    void (*hybrid_escape)(spu_context *); /* dynamic job mode: called where the boundary would refuse a transfer the interpreter can simply execute (a branch to LS 0 or to a misaligned pc, a callee that returned elsewhere than its call expects); it unwinds to the interpreter at ctx->pc. NULL: refuse, as before */
    void (*irq_escape)(spu_context *);   /* dynamic job mode: called where lifted code would need an interrupt delivered (spu_lifted_boundary.c); it unwinds to the interpreter. NULL: refuse, as before */
} ps3_spu_executor;
/* Local-store regions the interpreter tells apart when region_map is set (taken branches only, never per instruction):
 * the SPURS kernel (and any non-SPURS image), the policy module (LS 0xA00, WWS pm_wwsjob), the job code window (LS 0x4000, every
 * job/task module streams there), everything else. */
enum { PS3_SPU_REGION_KERNEL = 0, PS3_SPU_REGION_POLICY = 1, PS3_SPU_REGION_JOB = 2, PS3_SPU_REGION_OTHER = 3 };
enum { PS3_SPU_ACCT_SLOTS = 96 };
/* Why the interpreter ended a run with kind BUDGET before the budget was used up (ps3_spu_executor.watch_hit). */
enum { PS3_SPU_WATCH_NONE = 0, PS3_SPU_WATCH_JOB_CALL = 1, PS3_SPU_WATCH_LIFTED_ENTRY = 2 };
/* Terminal.channel of a FAULT the lifted glue itself raised (above every PS3_SPU_EXEC_OP_*): lifted code ran over local-store
 * bytes that a DMA replaced. */
enum { PS3_LIFTED_OP_CODE_CHANGED_DMA = 0x10101u };
/* Optional observation hook (NULL unless installed). Called once per SPU channel OPERATION the interpreter performs,
 * before its effect, as (kind, channel). It must not re-enter the owner and cannot fail: it exists so a test can
 * record what a kernel does with its channels and DMA. Installed separately from ps3_spu_executor_services (whose
 * shape therefore stays unchanged) and cleared by passing NULL. */
enum { PS3_SPU_OBS_WRITE_CHANNEL = 1, PS3_SPU_OBS_READ_CHANNEL, PS3_SPU_OBS_READ_CHANNEL_COUNT };
void ps3_spu_executor_set_channel_observer(ps3_spu_executor *,void (*observe)(void *,unsigned,uint32_t),void *);
/* Admits this owner for interpreted execution before start (see ps3_spu_execution_mode above). */
void ps3_spu_executor_set_interpreter(ps3_spu_executor *);
/* Fresh zeroed owner; retain core/LS/domain/ports until users join. No copying,
 * concurrent release or arbitrary context conversion. Services are mandatory,
 * nonthrowing and non-reentrant; finish_trace always closes its reserved credit,
 * cannot fail/escape, and receives bytes only for a real committed transfer.
 * These channel scopes contain C frames only. Genuine entries run through ps3_spu_executor_enter with owned,
 * validated continuation hops (spu_lifted_boundary.h). */
ps3_guest_memory_status ps3_spu_executor_init(ps3_spu_executor *,ps3_atomic_domain *,uint32_t,ps3_spu_executor_services);
/* Borrows a live semantic owner and scheduler LS. Release never unregisters it;
 * caller retains exclusive serialization and owner/domain/LS lifetime. */
ps3_guest_memory_status ps3_spu_executor_init_borrowed(ps3_spu_executor *,ps3_atomic_domain *,ps3_atomic_owner,void *,size_t,uint32_t,ps3_spu_executor_services);
/* Release: BUSY inside an entry, with a trace credit held, or while a continuation is still pending (take it first;
 * pending work is never silently dropped). */
ps3_guest_memory_status ps3_spu_executor_release(ps3_spu_executor *);
/* Runs up to `budget_instructions` SPU instructions of the owner's own local store starting at `pc`, on a STARTED
 * owner in INTERPRETER mode, inside the C-only escape scope (so every channel/DMA stop is a normal return, never an
 * unwind). Reuses the executor's checkpoint, DMA, atomic, decrementer and channel services unchanged; see
 * ps3_spu_interp_result for the four outcomes. INVALID (nothing executed, *out untouched) for a NULL/misaligned pc,
 * zero budget, a non-INTERPRETER owner, an owner that is not initialized/started/unstopped, a live escape, or a
 * pending continuation; the caller's floating-point environment is restored on every exit. A FAULT is permanent for
 * the owner (o->stopped), exactly like the lifted fail path. */
int ps3_spu_executor_run_interp(ps3_spu_executor *,uint32_t pc,uint64_t budget_instructions,ps3_spu_interp_result *out);
/* The interpreter itself (spu_interp_native.c): runs up to `budget` instructions from ctx->pc, writing the outcome
 * into *out. It does not return on a FAULT -- it leaves through ps3_spu_execution_fail, which is why the only caller
 * is ps3_spu_executor_run_interp's escape scope. Not part of the composition API. */
void ps3_spu_interp_run(spu_context *,uint64_t budget,ps3_spu_interp_result *out);
/* Starts an initialized, not yet started, unstopped owner outside any entry (samples cfg->clock once for the
 * decrementer origin). INVALID for a bad owner state or config (NULL clock function, zero budget, unsupported thread
 * option, or a bad image SHAPE: NULL image/table, count 0 or > SPU_LS_SIZE/4, pcs not strictly increasing, a pc not a
 * 4-byte-aligned LS address, a NULL function; LS pc 0 is a legal entry), refused before the clock is sampled and
 * with the owner byte-for-byte unchanged; CLOCK for a failed sample (status in o->clock_status, the only change).
 * Every failure leaves the owner unstarted. */
int ps3_spu_executor_start(ps3_spu_executor *,const ps3_spu_executor_start_config *);
/* Returns and clears the owner's pending continuation (NULL when none); only outside an entry. */
ps3_spu_lifted_entry ps3_spu_executor_take_continuation(ps3_spu_executor *);
int ps3_spu_executor_write_channel(ps3_spu_executor *,uint32_t,u128);
int ps3_spu_executor_read_channel(ps3_spu_executor *,uint32_t,u128 *);
/* Nonblocking RCHCNT scope; *count is written only on success. */
int ps3_spu_executor_count_channel(ps3_spu_executor *,uint32_t,uint32_t *);
/* Decision 33 / C1a: the PPU side of this owner's mailboxes, for the channels the interpreter added (SPU_RdInMbox 29,
 * SPU_WrOutMbox 28, SPU_WrOutIntrMbox 30). Each mailbox is the owner's spu_channel (ch_in_mbox / ch_out_mbox /
 * ch_out_intr_mbox) with hardware depth (in 4, out 1, out-intr 1): the SPU blocks on an empty in-mailbox and on a
 * full out mailbox, and the SPU-side depth is what rchcnt reports. These are the only writers the PPU needs; they do
 * not checkpoint and do not touch the executor's escape scope (a mailbox is state, not a stop).
 *   send: returns PS3_SPU_EXEC_UNSUPPORTED when the mailbox is at hardware depth (the caller polls free slots),
 *     PS3_SPU_EXEC_INVALID for a NULL/unknown owner or an unknown mailbox.
 *   receive: *value is written only when a word was taken; returns PS3_SPU_EXEC_UNSUPPORTED when empty.
 *   count: *free_slots / *pending written only on success. */
typedef enum { PS3_SPU_MAILBOX_IN = 0, PS3_SPU_MAILBOX_OUT = 1, PS3_SPU_MAILBOX_OUT_INTR = 2 } ps3_spu_mailbox;
int ps3_spu_executor_mbox_send(ps3_spu_executor *,ps3_spu_mailbox,uint32_t value);
int ps3_spu_executor_mbox_receive(ps3_spu_executor *,ps3_spu_mailbox,uint32_t *value);
int ps3_spu_executor_mbox_pending(ps3_spu_executor *,ps3_spu_mailbox,uint32_t *pending);
/* Non-channel terminal operations recorded in terminal.channel; above every SPU channel number.
 * OP_CONTINUATION: an owned continuation was refused (not this image's function at pc, LS 0, publish while
 * pending) or the hop budget is exhausted; the pending work stays in the owner. */
enum { PS3_SPU_EXEC_OP_ENTRY_RETURNED=0x10000u, PS3_SPU_EXEC_OP_DRAIN_CALL, PS3_SPU_EXEC_OP_IMG_RESTORE,
       PS3_SPU_EXEC_OP_INDIRECT_BRANCH, PS3_SPU_EXEC_OP_STOP, PS3_SPU_EXEC_OP_CONTINUATION };
/* Runs one actual lifted entry on a STARTED owner's core inside the C-only escape scope, then (top level) the owned
 * continuations it publishes, one validated hop at a time (spu_lifted_boundary.h). The caller's floating-point
 * environment (full FPCR/FPSR on AArch64; fenv_t elsewhere) is restored on every exit. A normal return with nothing
 * pending is terminal OP_ENTRY_RETURNED, never success. INVALID (no effect) for an unstarted owner, a pending
 * continuation, or core.image_id != the bound image id. Always returns the terminal code (never OK) otherwise. */
int ps3_spu_executor_enter(ps3_spu_executor *,ps3_spu_lifted_entry);
/* Opt-in slices yield only before a top-level continuation, with no live C
 * frames. Nested work is never unwound and replayed to simulate resumption. */
int ps3_spu_executor_enter_slice(ps3_spu_executor *,ps3_spu_lifted_entry,uint64_t);
int ps3_spu_executor_continue_slice(ps3_spu_executor *,uint64_t);
/* Recoverable boundary escape; legal only for a top-level slice or a real
 * policy exit that abandons its guest call stack. */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void ps3_spu_executor_pause(spu_context *,int);
#ifdef __cplusplus
}
#endif
#endif
