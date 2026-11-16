#define SPU_CHANNELS_IMPL 1
#include "spu_dma.h"
#include "spu_execution_port.h"
/* The owner type: the channel TU is included by a verifier that may not put native/adapters on the include
 * path, so this is spelled relative to this file. */
#include "../../../switch/native/adapters/spu_executor.h"
#include "../time/timebase.h"
_Static_assert(PS3_SPU_TAG_UPDATE_IMMEDIATE==MFC_TAG_UPDATE_IMMEDIATE && PS3_SPU_TAG_UPDATE_ANY==MFC_TAG_UPDATE_ANY &&
               PS3_SPU_TAG_UPDATE_ALL==MFC_TAG_UPDATE_ALL,"tag update modes");
_Static_assert(PS3_SPU_EVENT_LOCK_LINE_LOST==0x400u,"SPU event bit LR (CBE layout, spu_coherency.h SPU_EVENT_LR)");
static void native_checkpoint(spu_context *ctx,unsigned phase) {
    int status=ps3_spu_execution_for(ctx)->checkpoint(ctx,phase);
    if (status) ps3_spu_execution_fail(ctx,status);
}
static ps3_spu_executor *native_owner(spu_context *ctx) { return (ps3_spu_executor *)ctx; }
/* A mailbox the interpreter cannot use right now is a BLOCK, not a failure: the owner stays AT the channel
 * instruction (ctx->pc is untouched by this TU) and the caller retries once the PPU has acted (spu_executor.h
 * ps3_spu_interp_result PAUSED). pausing() is the interpreter's own scope only; the PPU-side channel scopes keep
 * their existing refusal for the same situation, so their behaviour is unchanged. */
static int native_pausable(spu_context *ctx) { return native_owner(ctx)->mode==PS3_SPU_EXECUTION_MODE_INTERPRETER; }
static void native_pause(spu_context *ctx) {
    native_owner(ctx)->interp_paused=1; native_owner(ctx)->interp_paused_pc=ctx->pc;
    ps3_spu_executor_pause(ctx,PS3_SPU_EXEC_UNSUPPORTED);
}
/* SPU_WrEventMask (pinned RPCS3 ad468a13 SPUThread.cpp 6548-6575): collect LR/TM for value|old mask, store the
 * mask, set the RdEventStat count when a pending event is selected, then take an interrupt if count&&enabled.
 * Native per-owner state is event_mask/event_status/int_enable; the count bit is not modeled. Accept only the
 * subset where the reference effect is exactly mask=value: SN and LR are the only events with a native producer,
 * nothing to collect, no selected pending event, interrupts disabled. Everything else is refused before any effect. */
static void native_write_event_mask(spu_context *ctx,uint32_t v) {
    /* Root fix (C1b): LR is collected only for interpreter owners (C1a); a lifted owner keeps its pre-C1a contract,
     * SN only (test_spu_executor.c event_mask_rejections_have_no_effect). Masked by C1a's wrong LR value 0x04. */
    const uint32_t produced=PS3_SPU_EVENT_SN|(native_pausable(ctx)?PS3_SPU_EVENT_LOCK_LINE_LOST:0u);
    /* Root (2026-11-11): an INTERPRETER owner may select an already pending event and may have interrupts enabled:
     * the mask is stored and the interpreter takes the interrupt at the next instruction (hardware order: the count
     * is raised by the write, the interrupt follows). A lifted owner keeps both refusals. */
    /* Root (2026-11-12): an INTERPRETER owner may also select events that have no native producer (inFAMOUS's movie
     * decoder writes such a mask in its first 56 instructions and was ended for it). Those bits are stored and never
     * raised; a program that then WAITS for one of them pauses on RdEventStat, which the thread report shows. */
    if ((!native_pausable(ctx) && ((v|ctx->event_mask)&~produced)) ||
        (!native_pausable(ctx) && ((ctx->event_status&v) || ctx->int_enable)))
        ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    ctx->event_mask=v;  /* pending events and interrupt enable untouched: nothing created or discarded */
    ps3_spu_execution_for(ctx)->effect_committed=1;  /* a POST_CHANNEL stop reports the committed mask */
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
}
/* Decision 33 / C1a event channels. Delivery model, per owner: event_status = pending latched events that the SPU
 * has not acknowledged; event_ack_mask = events the SPU acknowledged until each producer's next occurrence (the
 * hardware "edge already taken" latch), so a producer that keeps signalling a still-visible condition cannot spin
 * the SPU. Producers are (a) the lock-line reservation loss (LR), latched here from
 * ps3_spu_memory_take_reservation_loss and (b) the native stall-and-notify path (SN), which latches event_status
 * itself. RdEventStat returns the events selected by the mask -- status & mask & ~ack -- and, without `modify`,
 * consumes them (status <- status & ~returned). WrEventAck sets the ack latch for its bits. */
static void native_latch_reservation_loss(ps3_spu_executor *o) {
    int lost=0;
    if (ps3_spu_memory_take_reservation_loss(&o->memory,&lost)!=PS3_GUEST_MEMORY_OK) return;
    if (lost) o->core.event_status|=PS3_SPU_EVENT_LOCK_LINE_LOST;
}
/* Root fix (C1b Eden): hardware semantics (CBE / RPCS3 SPUThread get_events + set_events ack). RdEventStat REPORTS
 * pending & mask and clears nothing; WrEventAck clears exactly the given bits, and acking a bit that is not pending is
 * a no-op, not a fault. The SPURS kernel's wait loop is GETLLAR -> RdEventStat -> WrEventAck(LR) -> repeat
 * (Eden: it faulted on that ack at LS 0x11AC). C1a's consume-on-read + ack latch + refusal broke it. event_ack_mask is
 * no longer used (kept zero). */
static uint32_t native_selected_events(spu_context *ctx) {
    return ctx->event_status&ctx->event_mask;
}
static void native_write_event_ack(spu_context *ctx,uint32_t v) {
    /* Lifted owners keep the pre-C1a contract: WrEventAck is UNSUPPORTED (test_spu_executor.c:504/544). */
    if (!native_pausable(ctx)) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    ctx->event_status&=~v;
    ps3_spu_execution_for(ctx)->effect_committed=1;
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
}
static void native_write_event_mask_channel(spu_context *ctx,uint32_t v) { native_write_event_mask(ctx,v); }
/* SPU_WrDec (pinned RPCS3 ad468a13 SPUThread.cpp write_dec 5380-5387): a running reload muxed with the free-running
 * clock. Written after the most recent clock sample (the owner's start sample, or the last RdDec) the decrementer
 * counts down from the reload value; written before it, the free-running value is already below the reload and the
 * counters wins. The reload is sampled against the owner's own start sample, so the result is deterministic for a
 * caller-provided clock. */
static void native_write_decrementer(spu_context *ctx,uint32_t value) {
    ps3_spu_executor *o=native_owner(ctx);
    if (!o->started) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    uint64_t now=0;
    const ps3_clock_status status=o->start.clock.now_microseconds(o->start.clock.context,&now);
    if (status!=PS3_CLOCK_OK || now<o->decrementer.start_microseconds) {
        o->clock_status=status!=PS3_CLOCK_OK ? status : PS3_CLOCK_RANGE;
        ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_CLOCK);
    }
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    o->decrementer_reload=value; o->decrementer_latch_microseconds=now; o->decrementer_latched=1;
    ps3_spu_execution_for(ctx)->effect_committed=1;
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
}
/* RdDec with a latched reload: the reload minus time already counted since the reload was written, so a written
 * zero returns zero (RPCS3 `cnt ? cnt - (now-us) : 0`). No reload (or an owner that is not latched):
 * the existing clock-source read, unchanged. */
static uint32_t native_latched_decrementer(ps3_spu_executor *o,uint32_t *ok) {
    *ok=1;
    if (!o->decrementer_latched) return 0;
    uint64_t now=0;
    const ps3_clock_status status=o->start.clock.now_microseconds(o->start.clock.context,&now);
    if (status!=PS3_CLOCK_OK || now<o->decrementer_latch_microseconds) {
        o->clock_status=status!=PS3_CLOCK_OK ? status : PS3_CLOCK_RANGE; *ok=0; return 0;
    }
    const uint64_t elapsed=ps3_timebase_from_microseconds(now)-ps3_timebase_from_microseconds(o->decrementer_latch_microseconds);
    if (!o->decrementer_reload) return 0;
    const uint64_t raw=(uint64_t)o->decrementer_reload-elapsed;
    return (uint32_t)raw;
}
void spu_wrch(spu_context *ctx,uint32_t channel,u128 value) {
    ps3_spu_execution_port *port=ps3_spu_execution_for(ctx);
    port->channel=channel; port->effect_committed=0;
    uint32_t v=value._u32[0];
    if (channel==MFC_Cmd) {
        if (v==MFC_GETLLAR_CMD || v==MFC_PUTLLC_CMD || v==MFC_PUTLLUC_CMD) {
            int status=port->begin(ctx,v,ctx->mfc_tag,ctx->mfc_lsa,
                                  ((uint64_t)ctx->mfc_eah<<32)|ctx->mfc_eal,sizeof(ctx->resv_line),0);
            if (status) ps3_spu_execution_fail(ctx,status);
            if (ctx->list_stall_mask) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
            unsigned atomic_status;
            status=port->atomic(ctx,v,&atomic_status);
            if (status) ps3_spu_execution_fail(ctx,status);
            ctx->atomic_stat=atomic_status; port->status.atomic_available=1;
            native_checkpoint(ctx,PS3_SPU_POST_COMMAND); return;
        }
        /* Root (2026-11-11): record the command being attempted so a refusal inside the engine (before any
         * transfer is preflighted) still names it in the terminal record. A real transfer overwrites this. */
        port->request=(ps3_spu_transfer){((uint64_t)ctx->mfc_eah<<32)|ctx->mfc_eal,ctx->mfc_lsa,ctx->mfc_size,v,0};
        mfc_channel_write(port->engine,ctx,channel,v); return;
    }
    if (channel==MFC_WrListStallAck) { mfc_channel_write(port->engine,ctx,channel,v); return; }
    if (channel==SPU_WrEventMask) { native_write_event_mask_channel(ctx,v); return; }
    if (channel==SPU_WrEventAck) { native_write_event_ack(ctx,v); return; }
    if (channel==SPU_WrDec) {
        /* Lifted owners keep the pre-C1a refusal (TM collection is not modelled for them); an INTERPRETER owner gets
         * the running reload the SPURS kernel expects. */
        if (!native_pausable(ctx)) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
        native_write_decrementer(ctx,v); return;
    }
    if ((channel==SPU_WrOutMbox || channel==SPU_WrOutIntrMbox) && native_pausable(ctx)) {
        spu_channel *out=channel==SPU_WrOutMbox ? &ctx->ch_out_mbox : &ctx->ch_out_intr_mbox;
        if (out->count>=SPU_CHANNEL_CAP) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
        native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
        if (out->count>=SPU_MBOX_DEPTH) {
            /* At hardware depth the SPU blocks until the PPU reads. Nothing was committed. */
            if (native_pausable(ctx)) native_pause(ctx);
            ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
        }
        spu_channel_write(out,v);
        port->effect_committed=1;
        native_checkpoint(ctx,PS3_SPU_POST_CHANNEL); return;
    }
    switch (channel) {
    case SPU_WrSRR0: break;
    case MFC_LSA: case MFC_EAH: case MFC_EAL: case MFC_Size: case MFC_TagID: case MFC_WrTagMask: break;
    case MFC_WrTagUpdate: if (v>MFC_TAG_UPDATE_ALL) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED); break;
    default: ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    }
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    if (channel==SPU_WrSRR0) {
        ctx->srr0=v&SPU_LS_MASK;
    } else if (channel==MFC_WrTagUpdate) {
        /* Unsatisfied ANY/ALL records its mode and preserves any unread slot. */
        uint32_t completed=port->engine->tag_completed&ctx->mfc_tag_mask;
        if (v==MFC_TAG_UPDATE_IMMEDIATE || (v==MFC_TAG_UPDATE_ANY && completed) ||
            (v==MFC_TAG_UPDATE_ALL && completed==ctx->mfc_tag_mask))
            ps3_spu_status_publish_tag(ctx,completed);
        else port->status.pending_tag_update=v;
    } else {
        mfc_channel_write(port->engine,ctx,channel,v);
        if (channel==MFC_WrTagMask) ps3_spu_status_reevaluate_tag(ctx,port->engine->tag_completed);
    }
    port->effect_committed=1;  /* the channel write is committed before POST_CHANNEL */
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
}
uint32_t spu_rchcnt(spu_context *ctx,uint32_t channel) {
    ps3_spu_execution_port *port=ps3_spu_execution_for(ctx);
    port->channel=channel; port->effect_committed=0;
    if (channel!=MFC_WrTagUpdate && channel!=MFC_RdTagStat && channel!=MFC_RdAtomicStat &&
        channel!=SPU_RdDec &&
        /* The C1a channels below are served for an INTERPRETER owner only, so a lifted composition's channel
         * surface is exactly what it was before this change. */
        !(native_pausable(ctx) && (channel==SPU_RdInMbox || channel==SPU_RdEventStat ||
                                   channel==SPU_RdEventMask || channel==SPU_RdSRR0 ||
                                   channel==SPU_RdMachStat || channel==MFC_RdListStallStat)))
        ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    /* Pinned RPCS3 ad468a13 SPUThread.cpp:5522 exposes write capacity 1,
     * independently of pending updates and the unread tag-status slot. */
    if (channel==MFC_WrTagUpdate) return 1;
    /* Read-side capacity is what a nonblocking read would find: the in-mailbox depth SPU-side, one latched event
     * pair for RdEventStat (its count bit is not modeled), the written mask, and a plain SRR0/RdDec slot. */
    if (channel==SPU_RdInMbox) return ctx->ch_in_mbox.count<SPU_IN_MBOX_HW_DEPTH ? ctx->ch_in_mbox.count : SPU_IN_MBOX_HW_DEPTH;
    if (channel==SPU_RdEventStat) return native_selected_events(ctx) ? 1u : 0u;
    if (channel==SPU_RdEventMask) return 1;
    if (channel==SPU_RdSRR0 || channel==SPU_RdDec) return 1;
    if (channel==SPU_RdMachStat) return 1;   /* always readable: the read never blocks and changes nothing */
    /* Root (2026-11-11): the stall-and-notify handler asks whether a stalled list is waiting before it reads. */
    if (channel==MFC_RdListStallStat) return ctx->list_stall_stat ? 1u : 0u;
    return channel==MFC_RdTagStat ? port->status.tag_available : port->status.atomic_available;
}
/* SPU_RdDec (pinned RPCS3 ad468a13 SPUThread.cpp 5697-5709): read_dec().first, no event collection, no state effect.
 * Order: readiness (owner started; refused before any checkpoint), PRE_CHANNEL (a cancel takes no sample), one clock
 * sample through the owner's decrementer (failure stops CLOCK), POST_CHANNEL (a cancel drops the value; nothing was
 * committed), then the preferred-slot value. */
static u128 native_read_decrementer(spu_context *ctx,ps3_spu_execution_port *port) {
    ps3_spu_executor *o=native_owner(ctx);
    uint32_t value=0;
    int status=port->read_decrementer(ctx,NULL);
    if (status) ps3_spu_execution_fail(ctx,status);
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    if (o->decrementer_latched) {
        uint32_t ok=0; value=native_latched_decrementer(o,&ok);
        if (!ok) ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_CLOCK);
        ++o->decrementer_reads;
    } else {
        status=port->read_decrementer(ctx,&value);
        if (status) ps3_spu_execution_fail(ctx,status);
    }
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
    return spu_make_preferred_u32(value);
}
/* SPU_RdEventStat returns the pending events the mask selects and CONSUMES them (status <- status & ~returned);
 * with none pending it blocks. A PAUSE for the interpreter, the existing refusal for the PPU-side scope. */
static u128 native_read_event_stat(spu_context *ctx,ps3_spu_execution_port *port) {
    native_latch_reservation_loss(native_owner(ctx));
    const uint32_t selected=native_selected_events(ctx);
    if (!selected) {
        if (native_pausable(ctx)) native_pause(ctx);
        ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    }
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    port->effect_committed=1;   /* hardware: reading the status does not clear it (root fix C1b) */
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
    return spu_make_preferred_u32(selected);
}
/* SPU_RdMachStat (CBE channel 13, the machine status word; RPCS3 SPUThread read_machstat): bit 0 is the SPU's
 * interrupt-enable state and bit 1 is isolation mode, which this runtime never uses (coordinator's CBE knowledge and
 * RPCS3's handling, not extracted from firmware here); every other bit and the other three words are zero. The
 * native interpreter does not decode the E/D bits of bi/bisl/bisled/biz/iret into ctx->int_enable and interrupt
 * delivery is not modelled at all (native/adapters/spu_executor.c ASYNC_INTR refusal), so interrupts are never enabled
 * here and the word is always 0. Readable at any time: no pause, no state change. */
/* Root (2026-11-11): the interpreter now keeps int_enable from the E/D bits, so bit 0 is that flag. */
static uint32_t native_machine_status(spu_context *ctx) { return ctx->int_enable ? 1u : 0u; }
static u128 native_read_in_mailbox(spu_context *ctx,ps3_spu_execution_port *port) {
    if (!spu_channel_has_data(&ctx->ch_in_mbox)) {
        if (native_pausable(ctx)) native_pause(ctx);
        ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
    }
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    const uint32_t value=spu_channel_read(&ctx->ch_in_mbox);
    port->effect_committed=1;
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
    return spu_make_preferred_u32(value);
}
u128 spu_rdch(spu_context *ctx,uint32_t channel) {
    ps3_spu_execution_port *port=ps3_spu_execution_for(ctx);
    port->channel=channel; port->effect_committed=0;
    if (channel==SPU_RdDec) return native_read_decrementer(ctx,port);
    /* The C1a channels serve an INTERPRETER owner only; a lifted composition keeps its pre-C1a refusals. */
    if (native_pausable(ctx)) {
        if (channel==SPU_RdInMbox) return native_read_in_mailbox(ctx,port);
        if (channel==SPU_RdEventStat) return native_read_event_stat(ctx,port);
    }
    native_checkpoint(ctx,PS3_SPU_PRE_CHANNEL);
    uint32_t value;
    if (channel==SPU_RdEventMask && native_pausable(ctx)) {
        value=ctx->event_mask;
    } else if (channel==SPU_RdSRR0 && native_pausable(ctx)) {
        value=ctx->srr0;
    } else if (channel==SPU_RdMachStat && native_pausable(ctx)) {
        value=native_machine_status(ctx);
    } else if (channel==MFC_RdTagStat || channel==MFC_RdAtomicStat) {
        int tag=channel==MFC_RdTagStat;
        unsigned *available=tag ? &port->status.tag_available : &port->status.atomic_available;
        /* Empty reads and TagStat with unmodeled pending MFC work stay terminal. */
        if (!*available || (tag && (port->engine->queue_count || ctx->list_stall_mask)))
            ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
        uint32_t *slot=tag ? &ctx->mfc_tag_status : &ctx->atomic_stat;
        value=*slot; *slot=0; *available=0;
        port->effect_committed=1;  /* the slot is consumed before POST_CHANNEL */
    } else {
        /* Root (2026-11-11): with no stalled list the read blocks on hardware; the interpreter owner pauses at the
         * instruction (and is retried), a lifted owner keeps the refusal. */
        if (channel==MFC_RdListStallStat && !ctx->list_stall_stat && native_pausable(ctx)) native_pause(ctx);
        if (channel!=MFC_RdTagMask && !(channel==MFC_RdListStallStat && ctx->list_stall_stat))
            ps3_spu_execution_fail(ctx,PS3_SPU_EXEC_UNSUPPORTED);
        value=mfc_channel_read(port->engine,ctx,channel);
        port->effect_committed=channel==MFC_RdListStallStat;  /* the read clears the stall mask; RdTagMask has no effect */
    }
    native_checkpoint(ctx,PS3_SPU_POST_CHANNEL);
    return spu_make_preferred_u32(value);
}
