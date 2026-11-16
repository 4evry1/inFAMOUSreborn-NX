#include "spu_executor.h"
#include "spu_lifted_boundary.h"
#include "../../../ps3recomp/runtime/time/timebase.h"
#include <fenv.h>
#include <stddef.h>
#include <string.h>
_Static_assert(offsetof(ps3_spu_executor,core)==0,"lifted context must be first");
/* Caller FP environment for the entry scope. devkitA64 newlib fegetenv/fesetenv keep only FPCR&0x1f00 and the
 * FPSR flags (evidence/opus-native-event-entry-r1/target-r1), which would reset the caller's RMode/FZ/DN/AHP;
 * on AArch64 save/restore the full registers. glibc fenv_t covers x87 CW/SW and MXCSR on the host. */
#if defined(__aarch64__)
typedef struct { uint64_t fpcr, fpsr; } caller_fp_env;
static int save_caller_fp(caller_fp_env *e) {
    __asm__ volatile("mrs %0, fpcr" : "=r"(e->fpcr) :: "memory");
    __asm__ volatile("mrs %0, fpsr" : "=r"(e->fpsr) :: "memory");
    return 0;
}
static void restore_caller_fp(const caller_fp_env *e) {
    __asm__ volatile("msr fpsr, %0" :: "r"(e->fpsr) : "memory");
    __asm__ volatile("msr fpcr, %0" :: "r"(e->fpcr) : "memory");
}
#else
typedef fenv_t caller_fp_env;
static int save_caller_fp(caller_fp_env *e) { return fegetenv(e); }
static void restore_caller_fp(const caller_fp_env *e) { (void)fesetenv(e); }
#endif
enum { SPU_ATOMIC_CONDITIONAL_FAILURE=1 };
static ps3_spu_executor *owner(spu_context *ctx) { return (ps3_spu_executor *)ctx; }
ps3_spu_execution_port *ps3_spu_execution_for(spu_context *ctx) { return &owner(ctx)->binding; }
int ps3_spu_execution_delivers_interrupts(spu_context *ctx) { return owner(ctx)->mode==PS3_SPU_EXECUTION_MODE_INTERPRETER; }
/* A pause is recoverable: the owner is NOT stopped and its pc is left where the caller put it. The two escapes must be
 * distinguishable at the setjmp site, so pause jumps with 2 and fail with 1 (nothing else jumps with a nonzero value). */
enum { PS3_SPU_ESCAPE_FAIL = 1, PS3_SPU_ESCAPE_PAUSE = 2 };
_Noreturn void ps3_spu_executor_pause(spu_context *ctx,int code) {
    ps3_spu_executor *o=owner(ctx);
    o->terminal=(ps3_spu_executor_terminal){code,o->provider_status,o->binding.channel,o->binding.request,o->progress,
                                          o->binding.effect_committed,PS3_CLOCK_OK};
    if (o->escape) longjmp(*o->escape,PS3_SPU_ESCAPE_PAUSE);
    __builtin_trap();
}
_Noreturn void ps3_spu_execution_fail(spu_context *ctx,int code) {
    ps3_spu_executor *o=owner(ctx);
    if (!o->stopped) {
        o->stopped=1;
        o->terminal=(ps3_spu_executor_terminal){code,o->provider_status,o->binding.channel,o->binding.request,o->progress,
                                                o->binding.effect_committed,code==PS3_SPU_EXEC_CLOCK ? o->clock_status : PS3_CLOCK_OK};
    }
    if (o->escape) longjmp(*o->escape,PS3_SPU_ESCAPE_FAIL);
    __builtin_trap();
}
static int checkpoint(spu_context *ctx,unsigned phase) {
    ps3_spu_executor *o=owner(ctx);
    return o->services.checkpoint(o->services.context,phase);
}
static int begin(spu_context *ctx,uint32_t cmd,uint32_t tag,uint32_t lsa,uint64_t ea,uint32_t size,int ack) {
    ps3_spu_executor *o=owner(ctx); (void)tag; (void)ack;
    o->binding.request=(ps3_spu_transfer){ea,lsa,size,cmd,mfc_is_put(cmd)};
    o->binding.effect_committed=0; o->provider_status=PS3_GUEST_MEMORY_OK;
    return checkpoint(ctx,PS3_SPU_PRE_COMMAND);
}
static int preflight(ps3_spu_executor *o,ps3_spu_transfer request) {
    o->binding.request=request; o->binding.effect_committed=0; o->provider_status=PS3_GUEST_MEMORY_OK;
    int status=checkpoint(&o->core,PS3_SPU_PRE_TRANSFER); if (status) return status;
    if (request.ea>UINT32_MAX || request.bytes>PS3_GUEST_ADDRESS_BYTES-request.ea) return PS3_SPU_EXEC_ACCESS;
    status=o->services.access(o->services.context,request.ea,request.bytes,request.write); if (status) return status;
    status=o->services.reserve_trace(o->services.context,&request);
    if (!status) o->trace_reserved=1;
    return status;
}
static int finish_transfer(ps3_spu_executor *o,ps3_guest_memory_status status,int committed) {
    o->provider_status=status; o->binding.effect_committed=committed;
    o->trace_reserved=0;
    o->services.finish_trace(o->services.context,&o->binding.request,status,committed,
                            committed ? o->core.ls+o->binding.request.lsa : NULL);
    /* Decision 104: a transfer INTO the local store (a GET family command or GETLLAR) over the armed job module's code disarms
     * the lifted entry bits. Lifted code that is running when it happens is stale from the next instruction on and cannot be
     * resumed in the interpreter from inside a C frame: a named terminal, never a silent run on the wrong program. */
    if (committed && !o->binding.request.write && (o->watch_code_hi || o->watch_pm_count)) {
        const uint32_t lsa=o->binding.request.lsa; const uint64_t end=(uint64_t)lsa+o->binding.request.bytes;
        unsigned hit=0;
        if (o->watch_code_hi && lsa<o->watch_code_hi && end>o->watch_code_lo) hit|=1u;
        for (unsigned i=0;i<o->watch_pm_count;++i) if (lsa<o->watch_pm_hi[i] && end>o->watch_pm_lo[i]) hit|=2u;
        if (hit) {
            int fatal=0;
            if (o->watch_in_lifted) {
                const uint32_t pc=o->core.pc & SPU_LS_MASK;
                fatal=!o->watch_pm_count;     /* no policy module resident: any hit while lifted code runs is stale code */
                if ((hit&1u) && pc>=o->watch_code_lo && pc<o->watch_code_hi) fatal=1;
                if (hit&2u) for (unsigned i=0;i<o->watch_pm_count;++i) if (pc>=o->watch_pm_lo[i] && pc<o->watch_pm_hi[i]) fatal=1;
            }
            o->watch_dirty|=hit;
            if (hit&1u) o->watch_code_lo=o->watch_code_hi=0;
            if (hit&2u) o->watch_pm_count=0;
            o->watch_entry_bits=NULL; ++o->watch_disarms;
            if (fatal) {
                o->binding.channel=PS3_LIFTED_OP_CODE_CHANGED_DMA; o->binding.effect_committed=1;
                ps3_spu_execution_fail(&o->core,PS3_SPU_EXEC_UNSUPPORTED);
            }
        }
    }
    return status==PS3_GUEST_MEMORY_OK ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_MEMORY;
}
static int transfer(spu_context *ctx,uint32_t lsa,uint64_t ea,uint32_t bytes,uint32_t cmd) {
    ps3_spu_executor *o=owner(ctx); lsa&=SPU_LS_MASK;
    ps3_spu_transfer request={ea,lsa,bytes,cmd,mfc_is_put(cmd)};
    int status=preflight(o,request); if (status) return status;
    ps3_guest_memory_status memory_status=request.write ? ps3_spu_memory_put(&o->memory,ea,lsa,bytes) : ps3_spu_memory_get(&o->memory,ea,lsa,bytes);
    return finish_transfer(o,memory_status,memory_status==PS3_GUEST_MEMORY_OK);
}
static int atomic(spu_context *ctx,uint32_t cmd,unsigned *atomic_status) {
    ps3_spu_executor *o=owner(ctx);
    if (cmd!=MFC_GETLLAR_CMD && cmd!=MFC_PUTLLC_CMD && cmd!=MFC_PUTLLUC_CMD)
        return PS3_SPU_EXEC_UNSUPPORTED;
    uint32_t lsa=ctx->mfc_lsa&(SPU_LS_MASK&~(PS3_RESERVATION_GRANULE_BYTES-1u));
    uint64_t ea=(((uint64_t)ctx->mfc_eah<<32)|ctx->mfc_eal)&~(uint64_t)(PS3_RESERVATION_GRANULE_BYTES-1u);
    ps3_spu_transfer request={ea,lsa,PS3_RESERVATION_GRANULE_BYTES,cmd,cmd!=MFC_GETLLAR_CMD};
    int status=preflight(o,request); if (status) return status;
    ps3_guest_memory_status memory_status;
    if (cmd==MFC_GETLLAR_CMD) memory_status=ps3_spu_memory_getllar(&o->memory,ea,lsa,atomic_status);
    else if (cmd==MFC_PUTLLC_CMD) memory_status=ps3_spu_memory_putllc(&o->memory,ea,lsa,atomic_status);
    else if (cmd==MFC_PUTLLUC_CMD) memory_status=ps3_spu_memory_putlluc(&o->memory,ea,lsa,atomic_status);
    else return PS3_SPU_EXEC_UNSUPPORTED;
    int committed=memory_status==PS3_GUEST_MEMORY_OK && !(cmd==MFC_PUTLLC_CMD && *atomic_status==SPU_ATOMIC_CONDITIONAL_FAILURE);
    return finish_transfer(o,memory_status,committed);
}
static void progress(spu_context *ctx,ps3_spu_list_progress value) { owner(ctx)->progress=value; }
/* SPU_RdDec (pinned RPCS3 ad468a13 SPUThread.cpp 5697-5709: read_dec().first, no event side effect). */
static int read_decrementer(spu_context *ctx,uint32_t *value) {
    ps3_spu_executor *o=owner(ctx);
    if (!o->started) return PS3_SPU_EXEC_UNSUPPORTED;
    if (!value) return PS3_SPU_EXEC_OK;  /* readiness query: never samples */
    uint64_t raw=0;
    const ps3_clock_status status=ps3_spu_decrementer_read(&o->decrementer,o->start.clock,&raw);
    if (status!=PS3_CLOCK_OK) { o->clock_status=status; return PS3_SPU_EXEC_CLOCK; }
    ++o->decrementer_reads; o->decrementer_last_raw=raw;
    *value=(uint32_t)raw; return PS3_SPU_EXEC_OK;
}
ps3_guest_memory_status ps3_spu_executor_init(ps3_spu_executor *o,ps3_atomic_domain *domain,uint32_t id,ps3_spu_executor_services services) {
    if (!o || !domain || !services.access || !services.checkpoint || !services.reserve_trace || !services.finish_trace)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (o->initialized) return PS3_GUEST_MEMORY_BUSY;
    spu_context_init(&o->core,id); mfc_engine_init(&o->engine);
    ps3_guest_memory_status status=ps3_spu_memory_init(&o->memory,domain,o->core.ls,SPU_LS_SIZE);
    if (status) return status;
    o->services=services;
    o->binding=(ps3_spu_execution_port){.engine=&o->engine,.begin=begin,.transfer=transfer,.atomic=atomic,.progress=progress,
                                        .checkpoint=checkpoint,.read_decrementer=read_decrementer};
    o->initialized=1; return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory_status ps3_spu_executor_init_borrowed(ps3_spu_executor *o,ps3_atomic_domain *domain,ps3_atomic_owner token,void *ls,size_t bytes,
                                                     uint32_t id,ps3_spu_executor_services services) {
    if (!o || !domain || token.domain!=domain || !ls || bytes!=SPU_LS_SIZE || ((uintptr_t)ls&15u) ||
        !services.access || !services.checkpoint || !services.reserve_trace || !services.finish_trace)
        return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (o->initialized) return PS3_GUEST_MEMORY_BUSY;
    /* A zero-byte read validates identity/generation under the shared guard,
     * without touching backing memory or the owner's reservation/loss state. */
    ps3_guest_memory_status status=ps3_atomic_owner_read(domain,token,0,NULL,0);
    if (status) return status;
    spu_context_init(&o->core,id); o->core.ls=ls; mfc_engine_init(&o->engine);
    o->memory=(ps3_spu_memory){domain,token,ls}; o->services=services;
    o->binding=(ps3_spu_execution_port){.engine=&o->engine,.begin=begin,.transfer=transfer,.atomic=atomic,.progress=progress,
                                      .checkpoint=checkpoint,.read_decrementer=read_decrementer};
    o->borrowed_owner=1; o->initialized=1;
    return PS3_GUEST_MEMORY_OK;
}
ps3_guest_memory_status ps3_spu_executor_release(ps3_spu_executor *o) {
    if (!o || !o->initialized) return PS3_GUEST_MEMORY_INVALID_RANGE;
    if (o->escape || o->trace_reserved || o->continuation) return PS3_GUEST_MEMORY_BUSY;
    ps3_guest_memory_status status=o->borrowed_owner ? PS3_GUEST_MEMORY_OK : ps3_spu_memory_release(&o->memory);
    if (!status) {
        if (o->borrowed_owner) o->memory=(ps3_spu_memory){0};
        o->initialized=0; o->binding=(ps3_spu_execution_port){0};
        o->started=0; o->start=(ps3_spu_executor_start_config){0}; o->decrementer=(ps3_spu_decrementer){0};
        o->borrowed_owner=0; o->slice_mode=0; o->policy=(ps3_spu_policy_callbacks){0};
        o->mode=PS3_SPU_EXECUTION_MODE_LIFTED; o->interp_paused=0; o->interp_paused_pc=0;
        o->observe_channel=NULL; o->observe_context=NULL;
        o->decrementer_reload=0; o->decrementer_latched=0; o->decrementer_latch_microseconds=0;
    }
    return status;
}
/* Bound-image shape, checked once at start (O(count), reads only the table): 1..SPU_LS_SIZE/4 entries (the count bound
 * is checked before any entry is read), pcs strictly increasing (sorted and unique: the lookup is a binary search),
 * each pc a 4-byte-aligned LS address below SPU_LS_SIZE, each function non-NULL. LS pc 0 is a legal entry, as the
 * generator allows; only a HOP to pc 0 is refused (hop: desktop LS-0 job end). */
static int image_shape_valid(const ps3_spu_linked_image *image) {
    if (!image || !image->functions || !image->count || image->count>SPU_LS_SIZE/4u) return 0;
    for (size_t i=0;i<image->count;++i) {
        const ps3_spu_lifted_function *f=&image->functions[i];
        if (!f->function || f->pc>=SPU_LS_SIZE || (f->pc&3u) || (i && f->pc<=image->functions[i-1].pc)) return 0;
    }
    return 1;
}
/* Admit an owner that runs interpreted code from its own local store. Must be called before start; the start config
 * then needs no linked image (and ignores image/image_id). */
void ps3_spu_executor_set_interpreter(ps3_spu_executor *o) {
    if (!o) return;
    o->mode=PS3_SPU_EXECUTION_MODE_INTERPRETER;
}
int ps3_spu_executor_start(ps3_spu_executor *o,const ps3_spu_executor_start_config *cfg) {
    if (!o || !o->initialized || o->started || o->stopped || o->escape || !cfg ||
        (o->mode==PS3_SPU_EXECUTION_MODE_LIFTED && !image_shape_valid(cfg->image)) ||
        !cfg->clock.now_microseconds || !cfg->hop_budget)
        return PS3_SPU_EXEC_INVALID;
    if (cfg->thread_option!=PS3_SPU_THREAD_OPTION_NONE && cfg->thread_option!=PS3_SPU_THREAD_OPTION_DEC_SYNC_TB_ENABLE)
        return PS3_SPU_EXEC_INVALID;  /* e.g. ASYNC_INTR: interrupt delivery is not modelled natively */
    ps3_spu_decrementer decrementer={0};
    ps3_clock_status status=ps3_spu_decrementer_start(&decrementer,cfg->clock,cfg->thread_option);
    if (status) { o->clock_status=status; return PS3_SPU_EXEC_CLOCK; }
    o->start=*cfg; o->decrementer=decrementer; o->clock_status=PS3_CLOCK_OK; o->started=1;
    return PS3_SPU_EXEC_OK;
}
ps3_spu_lifted_entry ps3_spu_executor_take_continuation(ps3_spu_executor *o) {
    if (!o || o->escape) return NULL;
    ps3_spu_lifted_entry pending=o->continuation; o->continuation=NULL; return pending;
}
int ps3_spu_executor_write_channel(ps3_spu_executor *o,uint32_t channel,u128 value) {
    if (!o || !o->initialized) return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    jmp_buf escape; jmp_buf *previous=o->escape; o->escape=&escape;
    if (!setjmp(escape)) spu_wrch(&o->core,channel,value);
    o->escape=previous; return o->stopped ? o->terminal.code : PS3_SPU_EXEC_OK;
}
int ps3_spu_executor_read_channel(ps3_spu_executor *o,uint32_t channel,u128 *value) {
    if (!o || !o->initialized || !value) return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    jmp_buf escape; jmp_buf *previous=o->escape; o->escape=&escape;
    if (!setjmp(escape)) { u128 result=spu_rdch(&o->core,channel); *value=result; }
    o->escape=previous; return o->stopped ? o->terminal.code : PS3_SPU_EXEC_OK;
}
int ps3_spu_executor_count_channel(ps3_spu_executor *o,uint32_t channel,uint32_t *count) {
    if (!o || !o->initialized || !count) return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    jmp_buf escape; jmp_buf *previous=o->escape; o->escape=&escape;
    if (!setjmp(escape)) { uint32_t result=spu_rchcnt(&o->core,channel); *count=result; }
    o->escape=previous; return o->stopped ? o->terminal.code : PS3_SPU_EXEC_OK;
}
/* Depth of each mailbox as the SPU sees it: the PPU->SPU inbound mailbox is four on hardware, the SPU->PPU outbound
 * and interrupt mailboxes are one. A depth of N means the SPU may hold N words SPU-side; the queue itself buffers
 * more (spu_context.h SPU_CHANNEL_CAP) so a PPU sender that does not poll cannot lose a word. */
static spu_channel *mailbox_channel(ps3_spu_executor *o,ps3_spu_mailbox mailbox) {
    switch (mailbox) {
    case PS3_SPU_MAILBOX_IN: return &o->core.ch_in_mbox;
    case PS3_SPU_MAILBOX_OUT: return &o->core.ch_out_mbox;
    case PS3_SPU_MAILBOX_OUT_INTR: return &o->core.ch_out_intr_mbox;
    }
    return NULL;
}
static uint32_t mailbox_depth(ps3_spu_mailbox mailbox) {
    return mailbox==PS3_SPU_MAILBOX_IN ? SPU_IN_MBOX_HW_DEPTH : SPU_MBOX_DEPTH;
}
int ps3_spu_executor_mbox_send(ps3_spu_executor *o,ps3_spu_mailbox mailbox,uint32_t value) {
    if (!o || !o->initialized) return PS3_SPU_EXEC_INVALID;
    spu_channel *channel=mailbox_channel(o,mailbox);
    if (!channel) return PS3_SPU_EXEC_INVALID;
    if (channel->count>=mailbox_depth(mailbox)) return PS3_SPU_EXEC_UNSUPPORTED;
    spu_channel_write(channel,value);
    return PS3_SPU_EXEC_OK;
}
int ps3_spu_executor_mbox_receive(ps3_spu_executor *o,ps3_spu_mailbox mailbox,uint32_t *value) {
    if (!o || !o->initialized || !value) return PS3_SPU_EXEC_INVALID;
    spu_channel *channel=mailbox_channel(o,mailbox);
    if (!channel) return PS3_SPU_EXEC_INVALID;
    if (!spu_channel_has_data(channel)) return PS3_SPU_EXEC_UNSUPPORTED;
    *value=spu_channel_read(channel);
    return PS3_SPU_EXEC_OK;
}
int ps3_spu_executor_mbox_pending(ps3_spu_executor *o,ps3_spu_mailbox mailbox,uint32_t *pending) {
    if (!o || !o->initialized || !pending) return PS3_SPU_EXEC_INVALID;
    spu_channel *channel=mailbox_channel(o,mailbox);
    if (!channel) return PS3_SPU_EXEC_INVALID;
    *pending=channel->count;
    return PS3_SPU_EXEC_OK;
}
void ps3_spu_executor_set_channel_observer(ps3_spu_executor *o,void (*observe)(void *,unsigned,uint32_t),void *context) {
    if (!o) return;
    o->observe_channel=observe; o->observe_context=observe ? context : NULL;
}
int ps3_spu_executor_run_interp(ps3_spu_executor *o,uint32_t pc,uint64_t budget,ps3_spu_interp_result *out) {
    if (!o || !out || !budget || (pc&3u) || pc>=SPU_LS_SIZE) return PS3_SPU_EXEC_INVALID;
    if (!o->initialized || !o->started || o->mode!=PS3_SPU_EXECUTION_MODE_INTERPRETER || o->escape)
        return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    /* An interpreter run has no continuation to hand back: a pending lifted one means another entry owns the owner. */
    if (o->continuation) return PS3_SPU_EXEC_INVALID;
    caller_fp_env caller_fp; if (save_caller_fp(&caller_fp)) return PS3_SPU_EXEC_INVALID;
    ps3_spu_interp_result result={PS3_SPU_INTERP_FAULT,pc,PS3_SPU_EXEC_OK,0,0,0,0};
    jmp_buf escape; o->escape=&escape;
    const int escape_reason=setjmp(escape);
    if (!escape_reason) {   /* the run finished on its own: budget, stop, or no host escape at all */
        o->core.pc=pc;
        o->core.stop_code=0;   /* a fresh run starts with no halt value; a heq/hgt/hlgt step sets its own */
        ps3_spu_interp_run(&o->core,budget,&result);
    } else if (escape_reason==PS3_SPU_ESCAPE_PAUSE) {
        /* A channel the interpreter cannot satisfy: recoverable. The owner is not stopped and the channel
         * instruction is still at the path's pc (a channel TU never moves pc), so re-running retries it. */
        result.kind=PS3_SPU_INTERP_PAUSED; result.code=(uint32_t)o->terminal.code;
        result.channel=o->terminal.channel; result.stop_code=0; result.opcode=0;
        o->interp_paused=1; o->interp_paused_pc=o->core.pc;
    } else {
        /* spu_halt already wrote result.opcode before it failed; stop_code is the architectural halt value the
         * stepping instruction left in the core (0x1000 for a decoded assertion halt, 0 otherwise). */
        result.kind=PS3_SPU_INTERP_FAULT; result.code=(uint32_t)o->terminal.code;
        result.channel=o->terminal.channel; result.stop_code=o->core.stop_code;
    }
    o->escape=NULL; restore_caller_fp(&caller_fp);
    result.pc=o->core.pc;
    if (o->region_map && result.instructions>o->acct_mark) {   /* what ran since the last region change belongs to the current region */
        o->acct_instr[o->acct_slot[o->acct_region]]+=result.instructions-o->acct_mark;
        o->acct_mark=result.instructions;
    }
    *out=result;
    return PS3_SPU_EXEC_OK;
}
int ps3_spu_executor_enter(ps3_spu_executor *o,ps3_spu_lifted_entry entry) {    if (!o || !o->initialized || !entry || o->escape) return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    if (!o->started || o->continuation || o->core.image_id!=o->start.image_id) return PS3_SPU_EXEC_INVALID;
    caller_fp_env caller_fp; if (save_caller_fp(&caller_fp)) return PS3_SPU_EXEC_INVALID;
    jmp_buf escape; o->escape=&escape;
    if (!setjmp(escape)) {
        entry(&o->core);
        ps3_spu_lifted_boundary_run_continuations(&o->core);  /* never returns: every top-level outcome is terminal */
    }
    o->escape=NULL; restore_caller_fp(&caller_fp);
    return o->terminal.code;
}
static int run_slice(ps3_spu_executor *o,ps3_spu_lifted_entry entry,uint64_t budget,int continuing) {
    if (!o || !o->initialized || !o->started || o->escape || !budget || o->core.host_depth ||
        o->core.image_id!=o->start.image_id || (continuing ? !o->continuation : (!entry || o->continuation)))
        return PS3_SPU_EXEC_INVALID;
    if (o->stopped) return o->terminal.code;
    if (!continuing && ps3_spu_linked_image_lookup(o->start.image,o->core.pc)!=entry) return PS3_SPU_EXEC_INVALID;
    caller_fp_env caller_fp; if (save_caller_fp(&caller_fp)) return PS3_SPU_EXEC_INVALID;
    o->slice_mode=1; o->slice_end=budget>UINT64_MAX-o->hops ? UINT64_MAX : o->hops+budget;
    jmp_buf escape; o->escape=&escape;
    if (!setjmp(escape)) {
        if (entry) entry(&o->core);
        ps3_spu_lifted_boundary_run_continuations(&o->core);
    }
    o->escape=NULL; o->slice_mode=0; restore_caller_fp(&caller_fp);
    return o->terminal.code;
}
int ps3_spu_executor_enter_slice(ps3_spu_executor *o,ps3_spu_lifted_entry entry,uint64_t budget) {
    return run_slice(o,entry,budget,0);
}
int ps3_spu_executor_continue_slice(ps3_spu_executor *o,uint64_t budget) {
    return run_slice(o,NULL,budget,1);
}
