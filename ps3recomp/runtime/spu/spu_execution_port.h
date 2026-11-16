#ifndef PS3_SPU_EXECUTION_PORT_H
#define PS3_SPU_EXECUTION_PORT_H
#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct mfc_engine;
u128 spu_rdch(spu_context *,uint32_t);
uint32_t spu_rchcnt(spu_context *,uint32_t);
/* CLOCK: a decrementer read's clock sample failed, or was earlier than the owner's START sample (RANGE). Only that
 * comparison is made: a sample earlier than a previous read but not earlier than the start is accepted (no
 * between-read monotonicity check; the ps3_clock contract of real elapsed microseconds is relied on). BUDGET: the
 * owner's continuation hop budget is exhausted. Both stop before any effect of the refused operation. */
enum { PS3_SPU_EXEC_OK, PS3_SPU_EXEC_INVALID, PS3_SPU_EXEC_UNSUPPORTED,
       PS3_SPU_EXEC_ACCESS, PS3_SPU_EXEC_MEMORY, PS3_SPU_EXEC_TRACE, PS3_SPU_EXEC_CANCELLED,
       PS3_SPU_EXEC_CLOCK, PS3_SPU_EXEC_BUDGET,
       PS3_SPU_EXEC_SLICE, PS3_SPU_EXEC_KERNEL_EXIT, PS3_SPU_EXEC_STOP };
/* PRE_HOP: before an owned lifted continuation is taken and run (per-hop cancellation/scheduling point). */
enum { PS3_SPU_PRE_COMMAND=1, PS3_SPU_PRE_TRANSFER, PS3_SPU_POST_TRANSFER,
       PS3_SPU_POST_COMMAND, PS3_SPU_PRE_CHANNEL, PS3_SPU_POST_CHANNEL, PS3_SPU_PRE_HOP };
typedef struct { uint64_t ea; uint32_t lsa, bytes, command; unsigned write; } ps3_spu_transfer;
typedef struct { uint32_t next_element, remaining, destination, tag; } ps3_spu_list_progress;
/* Per-owner single-slot availability for MFC_RdTagStat/MFC_RdAtomicStat. Values
 * stay canonical in spu_context mfc_tag_status/atomic_stat; zero-initialized
 * means both slots empty and tag update mode immediate. Serialized owner only. */
enum { PS3_SPU_TAG_UPDATE_IMMEDIATE, PS3_SPU_TAG_UPDATE_ANY, PS3_SPU_TAG_UPDATE_ALL };
typedef struct { unsigned tag_available, atomic_available; uint32_t pending_tag_update; } ps3_spu_status_slots;
typedef struct ps3_spu_execution_port {
    struct mfc_engine *engine;
    int (*begin)(spu_context *,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,int);
    int (*transfer)(spu_context *,uint32_t,uint64_t,uint32_t,uint32_t);
    int (*atomic)(spu_context *,uint32_t,unsigned *);
    void (*progress)(spu_context *,ps3_spu_list_progress);
    int (*checkpoint)(spu_context *,unsigned);
    /* SPU_RdDec value from the owner's started decrementer and clock; returns an executor code (OK, UNSUPPORTED when
     * unstarted, CLOCK on provider failure) and writes *value only on OK. value==NULL is a readiness query only
     * (UNSUPPORTED when unstarted, else OK) and never samples the clock. */
    int (*read_decrementer)(spu_context *,uint32_t *);
    unsigned effect_committed;
    uint32_t channel;
    ps3_spu_transfer request;
    ps3_spu_status_slots status;
} ps3_spu_execution_port;
/* Native-only closed caller contract: ctx is the first subobject of an admitted
 * typed executor. Never call these helpers with arbitrary/desktop contexts.
 * The concrete owner is selected in composition, not a global/TLS registry. */
ps3_spu_execution_port *ps3_spu_execution_for(spu_context *);
/* Root (2026-11-11): 1 when the owner delivers SPU interrupts and events itself (the native interpreter:
 * int_enable follows the E/D bits and a selected pending event vectors to LS 0); 0 for a lifted owner, which
 * keeps every pre-existing refusal. */
int ps3_spu_execution_delivers_interrupts(spu_context *);
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void ps3_spu_execution_fail(spu_context *,int);
static inline void ps3_spu_status_publish_tag(spu_context *ctx,uint32_t value) {
    ps3_spu_status_slots *slots=&ps3_spu_execution_for(ctx)->status;
    ctx->mfc_tag_status=value; slots->tag_available=1;
    slots->pending_tag_update=PS3_SPU_TAG_UPDATE_IMMEDIATE;
}
/* Reevaluate only an existing pending ANY/ALL request against the current mask
 * and actual engine completion; never publishes without a pending request and
 * never touches an older unread slot when still unsatisfied. */
static inline void ps3_spu_status_reevaluate_tag(spu_context *ctx,uint32_t engine_completed) {
    uint32_t pending=ps3_spu_execution_for(ctx)->status.pending_tag_update;
    uint32_t completed=engine_completed&ctx->mfc_tag_mask;
    if ((pending==PS3_SPU_TAG_UPDATE_ANY && completed) ||
        (pending==PS3_SPU_TAG_UPDATE_ALL && completed==ctx->mfc_tag_mask))
        ps3_spu_status_publish_tag(ctx,completed);
}
#ifdef __cplusplus
}
#endif
#endif
