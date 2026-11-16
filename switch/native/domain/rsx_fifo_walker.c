/* Platform-free RSX FIFO walker. Contract: rsx_fifo_walker.h and
 * evidence/opus-native-rsx-command-path-r1/REVISED-DESIGN.md section 2.
 * Control-word semantics derived from the desktop walker
 * (cellGcmSys.c gcm_rsx_process_fifo_unlocked) without its resync heuristics:
 * every unexpected condition is a typed stop at the offending word. */
#include "rsx_fifo_walker.h"
#include <string.h>

#define WALKER_MAGIC 0x52464957u   /* 'RFIW' */
#define LAST_SAFE_OFFSET 0xFFFFFFF8u   /* get + 4 must not wrap */

/* Internal decode outcome: keep walking, or stop with a status. */
#define CONTINUE ((rsx_fifo_status)-1)

int rsx_fifo_status_is_terminal(rsx_fifo_status status)
{
    return status >= RSX_FIFO_INVALID_HEADER && status <= RSX_FIFO_SINK_FAILED;
}

static uint32_t be32(const uint8_t b[4])
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static int usable(const rsx_fifo_walker *w)
{
    return w && w->magic == WALKER_MAGIC && w->reader.read_word && w->sink.method;
}

/* Latch the first failure. Effect defaults to NONE (no sink call for the failed
 * word); a sink refusal overwrites it afterwards with the sink's report. */
static rsx_fifo_status fail(rsx_fifo_walker *w, rsx_fifo_status status, uint32_t offset,
                            uint32_t word, uint32_t detail)
{
    w->terminal = 1;
    w->failure.status = status;
    w->failure.offset = offset;
    w->failure.word = word;
    w->failure.detail = detail;
    w->failure.io_offset = 0;
    w->failure.io_result = 0;
    w->failure.effect = RSX_FIFO_EFFECT_NONE;
    w->failure.packet = w->packet;
    return status;
}

/* A failed read keeps its exact provenance: which offset, raw reader value, and
 * whether it was the fetch of the word at get or a branch-target probe. */
static rsx_fifo_status fail_read(rsx_fifo_walker *w, int probe, uint32_t at, uint32_t word,
                                 uint32_t read_offset, rsx_fifo_read_result r)
{
    const int unmapped = r == RSX_FIFO_IO_UNMAPPED;   /* anything else non-OK is a fault */
    rsx_fifo_status st = probe ? (unmapped ? RSX_FIFO_UNMAPPED_BRANCH : RSX_FIFO_BRANCH_READ_FAULT)
                               : (unmapped ? RSX_FIFO_UNMAPPED_FETCH : RSX_FIFO_READ_FAULT);
    fail(w, st, at, word, read_offset);
    w->failure.io_offset = read_offset;
    w->failure.io_result = (uint32_t)r;
    return st;
}

/* Lifecycle (r3 L1): *w must be zero storage or a walker previously passed here.
 * A walker busy inside its own pump (init called from a reader/sink/cancel
 * callback) is refused untouched: cursor, pending packet and call state survive. */
rsx_fifo_status rsx_fifo_walker_init(rsx_fifo_walker *w, rsx_fifo_reader reader,
                                     rsx_fifo_sink sink, uint32_t start_get)
{
    if (!w) return RSX_FIFO_INVALID_ARGUMENT;
    if (w->magic == WALKER_MAGIC && w->busy) return RSX_FIFO_REENTRANT;
    if (!reader.read_word || !sink.method || (start_get & 3u))
        return RSX_FIFO_INVALID_ARGUMENT;
    memset(w, 0, sizeof *w);
    w->reader = reader;
    w->sink = sink;
    w->get = start_get;
    w->put = start_get;
    w->failure.status = RSX_FIFO_REACHED_PUT;
    w->magic = WALKER_MAGIC;
    return RSX_FIFO_REACHED_PUT;
}

rsx_fifo_status rsx_fifo_walker_publish_put(rsx_fifo_walker *w, uint32_t put)
{
    if (!usable(w) || (put & 3u)) return RSX_FIFO_INVALID_ARGUMENT;
    if (w->busy) return RSX_FIFO_REENTRANT;
    w->put = put;
    return w->terminal ? w->failure.status : RSX_FIFO_REACHED_PUT;
}

const rsx_fifo_failure *rsx_fifo_walker_failure(const rsx_fifo_walker *w)
{
    return w ? &w->failure : NULL;
}

/* Branch targets (JUMP, CALL and, since r3, RET's return offset) that will
 * execute must be readable before the cursor moves there; the check is the
 * reader's answer, never an ordering comparison against PUT. A target equal to the published PUT will not execute before a new PUT
 * (the loop stops on get == put before fetching), so it is not probed: it may
 * legitimately lie one past readable memory. It is fetched normally later. */
static rsx_fifo_status probe_target(rsx_fifo_walker *w, uint32_t at, uint32_t word, uint32_t target)
{
    if (target == w->put) return CONTINUE;
    uint8_t scratch[4];
    rsx_fifo_read_result r = w->reader.read_word(w->reader.user, target, scratch);
    if (r == RSX_FIFO_IO_OK) return CONTINUE;
    return fail_read(w, 1, at, word, target, r);
}

static rsx_fifo_status decode_header(rsx_fifo_walker *w, uint32_t at, uint32_t word)
{
    if ((word & 0xFFFF0000u) == 0xFEAD0000u)          /* lv1 driver flip word */
        return fail(w, RSX_FIFO_UNSUPPORTED_CONTROL, at, word, 0);

    if ((word & 0xE0000003u) == 0x20000000u) {        /* JUMP */
        uint32_t target = word & 0x1FFFFFFCu;
        if (target == at) return RSX_FIFO_PARKED;     /* not consumed; re-read next pump */
        rsx_fifo_status s = probe_target(w, at, word, target);
        if (s != CONTINUE) return s;
        w->get = target;
        return CONTINUE;
    }
    if ((word & 0xE0000003u) == 0x00000002u) {        /* CALL */
        uint32_t target = word & 0x1FFFFFFCu;
        if (w->in_call) return fail(w, RSX_FIFO_CALL_DEPTH, at, word, target);
        if (at > LAST_SAFE_OFFSET) return fail(w, RSX_FIFO_OFFSET_OVERFLOW, at, word, at);
        rsx_fifo_status s = probe_target(w, at, word, target);
        if (s != CONTINUE) return s;
        w->return_offset = at + 4u;
        w->in_call = 1;
        w->get = target;
        return CONTINUE;
    }
    if (word == 0x00020000u) {                        /* RET */
        if (!w->in_call) return fail(w, RSX_FIFO_RET_WITHOUT_CALL, at, word, 0);
        /* r3 (F5): the return target is validated like any branch target (== put exempt);
         * a failure is attributed to the RET word and nothing is committed. */
        rsx_fifo_status s = probe_target(w, at, word, w->return_offset);
        if (s != CONTINUE) return s;
        w->get = w->return_offset;
        w->in_call = 0;
        w->return_offset = 0;
        return CONTINUE;
    }
    if ((word & 0xA0030003u) == 0) {                  /* method header: incrementing / NI */
        uint32_t count = (word >> 18) & 0x7FFu;
        uint32_t method = word & 0x1FFCu;
        uint32_t increment = (word & 0x40000000u) ? 0u : 1u;
        if (increment && count && method + 4u * (count - 1u) > 0x1FFCu)
            return fail(w, RSX_FIFO_INVALID_HEADER, at, word, count);
        if (at > LAST_SAFE_OFFSET) return fail(w, RSX_FIFO_OFFSET_OVERFLOW, at, word, at);
        w->get = at + 4u;
        if (count) {
            w->packet.active = 1;
            w->packet.header = word;
            w->packet.header_offset = at;
            w->packet.subchannel = (word >> 13) & 7u;
            w->packet.increment = increment;
            w->packet.next_method = method;
            w->packet.remaining = count;
            w->packet.index = 0;
        }
        return CONTINUE;
    }
    return fail(w, RSX_FIFO_INVALID_HEADER, at, word, 0);
}

rsx_fifo_status rsx_fifo_walker_pump(rsx_fifo_walker *w, uint32_t word_budget,
                                     const rsx_fifo_cancel *cancel,
                                     rsx_fifo_pump_report *report)
{
    if (report) memset(report, 0, sizeof *report);
    /* Misuse: no state change; the report mirrors the returned status (r3 F3). */
    if (!usable(w)) {
        if (report) report->status = RSX_FIFO_INVALID_ARGUMENT;
        return RSX_FIFO_INVALID_ARGUMENT;
    }
    if (w->busy) {
        if (report) { report->status = RSX_FIFO_REENTRANT; report->get = w->get; report->put = w->put; }
        return RSX_FIFO_REENTRANT;
    }
    uint32_t done = 0;
    rsx_fifo_status status;
    if (w->terminal) {
        status = w->failure.status;
        goto out;
    }
    w->busy = 1;
    for (;;) {
        if (w->get == w->put) {
            status = w->packet.active ? RSX_FIFO_NEED_INPUT : RSX_FIFO_REACHED_PUT;
            break;
        }
        if (done == word_budget) { status = RSX_FIFO_BUDGET_EXHAUSTED; break; }
        if (cancel && cancel->should_stop && cancel->should_stop(cancel->user)) {
            status = RSX_FIFO_CANCELLED;
            break;
        }
        const uint32_t at = w->get;
        uint8_t bytes[4];
        rsx_fifo_read_result r = w->reader.read_word(w->reader.user, at, bytes);
        if (r != RSX_FIFO_IO_OK) { status = fail_read(w, 0, at, 0, at, r); break; }
        const uint32_t word = be32(bytes);
        if (w->packet.active) {
            /* Payload word: deliver once; commit only after the sink accepted it. */
            if (at > LAST_SAFE_OFFSET) { status = fail(w, RSX_FIFO_OFFSET_OVERFLOW, at, word, at); break; }
            /* Preset UNKNOWN: a sink must positively claim NONE. The unadvanced cursor
             * names the failed word; it says nothing about that word's effect. */
            rsx_fifo_effect eff = RSX_FIFO_EFFECT_UNKNOWN;
            int rc = w->sink.method(w->sink.user, w->packet.subchannel, w->packet.next_method, word, at, &eff);
            if (rc != 0) {
                status = fail(w, RSX_FIFO_SINK_FAILED, at, word, (uint32_t)rc);
                w->failure.effect = (eff == RSX_FIFO_EFFECT_NONE || eff == RSX_FIFO_EFFECT_PARTIAL)
                                        ? eff : RSX_FIFO_EFFECT_UNKNOWN;
                break;
            }
            w->get = at + 4u;
            w->packet.index++;
            if (w->packet.increment) w->packet.next_method += 4u;
            if (--w->packet.remaining == 0) memset(&w->packet, 0, sizeof w->packet);
            w->methods_committed++;
        } else {
            status = decode_header(w, at, word);
            if (status != CONTINUE) break;
        }
        w->words_committed++;
        done++;
    }
    w->busy = 0;
out:
    if (report) {
        report->status = status;
        report->words_processed = done;
        report->get = w->get;
        report->put = w->put;
    }
    return status;
}
