#ifndef RSX_FIFO_WALKER_H
#define RSX_FIFO_WALKER_H
/* Platform-free RSX FIFO walker (instance-owned, bounded, resumable).
 *
 * Addresses are RSX IO offsets. The reader port copies 4 raw guest bytes; the
 * walker decodes them big-endian. PUT is a published stop position compared
 * only for equality before each word; branch targets are validated through the
 * reader, never by numeric comparison with PUT. Partial packets survive every
 * return (budget, cancel, NEED_INPUT) and a payload word is delivered exactly
 * once. Any failure is latched: no later word is ever delivered.
 * Branch targets (JUMP, CALL, RET return offset) are probed before the cursor moves.
 * A branch whose target equals the published PUT is not probed (the target will
 * not execute before a new PUT; it is fetched normally afterwards). A failed
 * read keeps its provenance (fetch vs branch probe, UNMAPPED vs FAULT, raw value).
 * The effect of a refused word is reported by the sink, separately from the
 * unadvanced cursor.
 * See evidence/opus-native-rsx-command-path-r1/REVISED-DESIGN.md section 2 and
 * evidence/opus-native-rsx-command-path-r2/AMENDMENT-DESIGN.md and
 * evidence/opus-native-rsx-review-fixes-r1/DESIGN-AMENDMENT.md. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum rsx_fifo_read_result {
    RSX_FIFO_IO_OK = 0,
    RSX_FIFO_IO_UNMAPPED,
    RSX_FIFO_IO_FAULT
} rsx_fifo_read_result;

typedef struct rsx_fifo_reader {
    void *user;
    rsx_fifo_read_result (*read_word)(void *user, uint32_t io_offset, uint8_t out_be[4]);
} rsx_fifo_reader;

/* Effect of a refused word, independent of the (unadvanced) cursor.
 * NONE: guaranteed no effect. PARTIAL: known partial application, performer
 * reported the work not performed. UNKNOWN: cannot be vouched for. */
typedef enum rsx_fifo_effect {
    RSX_FIFO_EFFECT_NONE = 0,
    RSX_FIFO_EFFECT_PARTIAL,
    RSX_FIFO_EFFECT_UNKNOWN
} rsx_fifo_effect;

/* Returns 0 when the method was executed (the walker then commits the word);
 * any nonzero value is a refusal recorded verbatim as the failure detail.
 * *effect is preset to RSX_FIFO_EFFECT_UNKNOWN; on refusal the sink must set it
 * truthfully (NONE only if it guarantees no effect). Ignored on success. */
typedef struct rsx_fifo_sink {
    void *user;
    int (*method)(void *user, uint32_t subchannel, uint32_t method, uint32_t data,
                  uint32_t io_offset, rsx_fifo_effect *effect);
} rsx_fifo_sink;

/* Optional: polled before each word; nonzero stops the pump (resumable). */
typedef struct rsx_fifo_cancel {
    void *user;
    int (*should_stop)(void *user);
} rsx_fifo_cancel;

typedef enum rsx_fifo_status {
    /* Non-terminal (resumable). */
    RSX_FIFO_REACHED_PUT = 0,     /* get == put at a packet boundary */
    RSX_FIFO_NEED_INPUT,          /* get == put inside a packet's payload */
    RSX_FIFO_PARKED,              /* word at get is a JUMP to itself; not consumed */
    RSX_FIFO_BUDGET_EXHAUSTED,
    RSX_FIFO_CANCELLED,
    /* API misuse: returned without any state change. */
    RSX_FIFO_INVALID_ARGUMENT,
    RSX_FIFO_REENTRANT,
    /* Terminal (latched). */
    RSX_FIFO_INVALID_HEADER,
    RSX_FIFO_UNMAPPED_FETCH,
    RSX_FIFO_READ_FAULT,
    RSX_FIFO_UNMAPPED_BRANCH,     /* branch target probe: reader said UNMAPPED */
    RSX_FIFO_BRANCH_READ_FAULT,   /* branch target probe: reader said FAULT (or out-of-range) */
    RSX_FIFO_CALL_DEPTH,
    RSX_FIFO_RET_WITHOUT_CALL,
    RSX_FIFO_OFFSET_OVERFLOW,
    RSX_FIFO_UNSUPPORTED_CONTROL, /* 0xFEADxxxx driver flip word: no flip service here */
    RSX_FIFO_SINK_FAILED
} rsx_fifo_status;

int rsx_fifo_status_is_terminal(rsx_fifo_status status);

typedef struct rsx_fifo_packet {
    int active;
    uint32_t header, header_offset, subchannel, increment;
    uint32_t next_method, remaining, index;
} rsx_fifo_packet;

typedef struct rsx_fifo_failure {
    rsx_fifo_status status;    /* RSX_FIFO_REACHED_PUT when no failure */
    uint32_t offset;           /* IO offset of the failed word (== get) */
    uint32_t word;             /* the word read there (0 if unreadable) */
    uint32_t detail;           /* branch target / sink code / fetch offset */
    uint32_t io_offset;        /* offset passed to the failing read (fetch or probe) */
    uint32_t io_result;        /* raw reader return value of that read (0 if none failed) */
    rsx_fifo_effect effect;    /* effect of the failed word; NONE unless the sink said otherwise */
    rsx_fifo_packet packet;    /* continuation state at failure */
} rsx_fifo_failure;

typedef struct rsx_fifo_walker {
    uint32_t magic;
    rsx_fifo_reader reader;
    rsx_fifo_sink sink;
    uint32_t get, put;
    int in_call;
    uint32_t return_offset;
    rsx_fifo_packet packet;
    rsx_fifo_failure failure;
    int terminal;
    int busy;
    uint64_t words_committed, methods_committed;
} rsx_fifo_walker;

typedef struct rsx_fifo_pump_report {
    rsx_fifo_status status;
    uint32_t words_processed;   /* words committed by this pump */
    uint32_t get, put;
} rsx_fifo_pump_report;

/* Caller owns storage. Lifecycle contract (r3, evidence/opus-native-rsx-review-fixes-r1/
 * DESIGN-AMENDMENT.md L1): *walker MUST be zero storage ({0}, memset, static, calloc) or a
 * walker previously passed to this function; init reads magic/busy before writing.
 * Not constructed or idle: construct/reset (clears a latched failure).
 * Busy (called from this walker's own reader/sink/cancel callback): RSX_FIFO_REENTRANT,
 * nothing written (cursor, pending packet, call state preserved; the pump continues).
 * Otherwise null walker, null reader/sink function or misaligned start_get ->
 * RSX_FIFO_INVALID_ARGUMENT with *walker untouched. put starts equal to start_get. */
rsx_fifo_status rsx_fifo_walker_init(rsx_fifo_walker *walker, rsx_fifo_reader reader,
                                     rsx_fifo_sink sink, uint32_t start_get);
/* Publish a new stop position (4-aligned). Allowed in any non-busy state; a
 * terminal walker accepts it but stays terminal. */
rsx_fifo_status rsx_fifo_walker_publish_put(rsx_fifo_walker *walker, uint32_t put);
/* Process up to word_budget words. With budget 0 nothing is read: get == put returns
 * REACHED_PUT / NEED_INPUT, a terminal walker returns its latched status, otherwise
 * BUDGET_EXHAUSTED. cancel may be NULL. report may be NULL; when given, report->status
 * always equals the return value (INVALID_ARGUMENT: get/put 0; REENTRANT: live get/put). */
rsx_fifo_status rsx_fifo_walker_pump(rsx_fifo_walker *walker, uint32_t word_budget,
                                     const rsx_fifo_cancel *cancel,
                                     rsx_fifo_pump_report *report);
const rsx_fifo_failure *rsx_fifo_walker_failure(const rsx_fifo_walker *walker);

#ifdef __cplusplus
}
#endif
#endif
