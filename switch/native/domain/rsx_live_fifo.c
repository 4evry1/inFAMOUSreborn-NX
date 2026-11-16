/* Live RSX command-buffer consumer. See rsx_live_fifo.h for the contract and its provenance. */
#include "rsx_live_fifo.h"

#include <string.h>

void rsx_live_init(rsx_live_fifo *fifo, const rsx_live_layout *layout)
{
    if (fifo == NULL) return;
    memset(fifo, 0, sizeof(*fifo));
    if (layout != NULL) fifo->layout = *layout;
    fifo->semaphore_dma = RSX_LIVE_DMA_LABEL_RW;
}

static rsx_live_status fault(rsx_live_fifo *fifo, uint32_t reason, uint32_t offset, uint32_t word)
{
    if (!fifo->fault) {
        fifo->fault = 1u;
        fifo->fault_reason = reason;
        fifo->fault_offset = offset;
        fifo->fault_word = word;
    }
    return RSX_LIVE_FAULT;
}

static int offset_ok(const rsx_live_fifo *fifo, uint32_t offset)
{
    return (offset & 3u) == 0u && offset < fifo->layout.io_size && fifo->layout.io_size - offset >= 4u;
}

static int label_ok(const rsx_live_fifo *fifo, uint32_t offset, uint32_t bytes)
{
    return offset <= fifo->layout.label_size && fifo->layout.label_size - offset >= bytes;
}

static int is_label_dma(uint32_t handle)
{
    return handle == RSX_LIVE_DMA_LABEL_RW || handle == RSX_LIVE_DMA_LABEL_R;
}

void rsx_live_flip_now(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t buffer_id)
{
    if (fifo == NULL) return;
    ++fifo->flips;
    fifo->last_flip_buffer = buffer_id;
    if (ports != NULL && ports->flip != NULL) ports->flip(ports->user, buffer_id);
}

/* Executes one method. Returns 0 to continue, 1 when a semaphore acquire must wait (nothing consumed),
 * 2 on a latched fault. */
static int execute(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t subchannel, uint32_t method,
                   uint32_t data, uint32_t word_offset)
{
    const rsx_live_layout *layout = &fifo->layout;
    if (method < 0x100u) {
        switch (method) {
        case RSX_LIVE_NV406E_SET_REFERENCE:
            if (ports->write32(ports->user, layout->control_address + 8u, data) != 0) {
                fault(fifo, RSX_LIVE_FAULT_WRITE, word_offset, data);
                return 2;
            }
            fifo->last_reference = data;
            ++fifo->references;
            return 0;
        case RSX_LIVE_NV406E_SET_CONTEXT_DMA_SEMAPHORE: fifo->semaphore_dma = data; return 0;
        case RSX_LIVE_NV406E_SEMAPHORE_OFFSET: fifo->semaphore_offset = data; return 0;
        case RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE: {
            uint32_t current = 0;
            if (!is_label_dma(fifo->semaphore_dma)) { ++fifo->device_semaphore_ops; return 0; }
            if (!label_ok(fifo, fifo->semaphore_offset, 4u)) {
                fault(fifo, RSX_LIVE_FAULT_LABEL, word_offset, fifo->semaphore_offset);
                return 2;
            }
            if (ports->read32(ports->user, layout->label_address + fifo->semaphore_offset, &current) != 0) {
                fault(fifo, RSX_LIVE_FAULT_READ, word_offset, data);
                return 2;
            }
            if (current != data) { ++fifo->acquire_waits; return 1; }
            ++fifo->acquires;
            return 0;
        }
        case RSX_LIVE_NV406E_SEMAPHORE_RELEASE:
            if (!is_label_dma(fifo->semaphore_dma)) { ++fifo->device_semaphore_ops; return 0; }
            if (!label_ok(fifo, fifo->semaphore_offset, 4u)) {
                fault(fifo, RSX_LIVE_FAULT_LABEL, word_offset, fifo->semaphore_offset);
                return 2;
            }
            if (ports->write32(ports->user, layout->label_address + fifo->semaphore_offset, data) != 0) {
                fault(fifo, RSX_LIVE_FAULT_WRITE, word_offset, data);
                return 2;
            }
            ++fifo->label_writes;
            return 0;
        default:
            ++fifo->unknown_control; /* SET_OBJECT and the like: nothing the PPU can observe */
            return 0;
        }
    }
    if (subchannel == RSX_LIVE_FLIP_SUBCHANNEL && method == RSX_LIVE_FLIP_METHOD) {
        rsx_live_flip_now(fifo, ports, data);
        return 0;
    }
    switch (method) {
    case RSX_LIVE_NV4097_SET_SEMAPHORE_OFFSET: fifo->semaphore_offset_4097 = data; return 0;
    case RSX_LIVE_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE:
    case RSX_LIVE_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE: {
        /* The back-end release swaps the R and B bytes of the value (ARGB written as ABGR). */
        const uint32_t value = method == RSX_LIVE_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE
            ? ((data & 0xFF00FF00u) | ((data & 0xFFu) << 16) | ((data >> 16) & 0xFFu)) : data;
        if (!label_ok(fifo, fifo->semaphore_offset_4097, 4u)) {
            fault(fifo, RSX_LIVE_FAULT_LABEL, word_offset, fifo->semaphore_offset_4097);
            return 2;
        }
        if (ports->write32(ports->user, layout->label_address + fifo->semaphore_offset_4097, value) != 0) {
            fault(fifo, RSX_LIVE_FAULT_WRITE, word_offset, data);
            return 2;
        }
        ++fifo->label_writes;
        return 0;
    }
    case RSX_LIVE_NV4097_GET_REPORT: {
        /* data = (type << 24) | offset. The report is { be64 timer, be32 value, be32 zero }; with no
         * renderer every query counts zero samples. */
        const uint32_t offset = layout->report_offset + (data & 0x00FFFFFFu);
        if (!label_ok(fifo, offset, 16u)) {
            fault(fifo, RSX_LIVE_FAULT_LABEL, word_offset, offset);
            return 2;
        }
        if (ports->write32(ports->user, layout->label_address + offset, 0u) != 0 ||
            ports->write32(ports->user, layout->label_address + offset + 4u, (uint32_t)fifo->reports) != 0 ||
            ports->write32(ports->user, layout->label_address + offset + 8u, 0u) != 0 ||
            ports->write32(ports->user, layout->label_address + offset + 12u, 0u) != 0) {
            fault(fifo, RSX_LIVE_FAULT_WRITE, word_offset, data);
            return 2;
        }
        ++fifo->reports;
        return 0;
    }
    case RSX_LIVE_NV4097_SET_BEGIN_END: if (data != 0u) ++fifo->draws; break;
    case RSX_LIVE_NV4097_CLEAR_SURFACE: ++fifo->clears; break;
    default: break;
    }
    if (ports->method != NULL) ports->method(ports->user, subchannel, method, data);
    return 0;
}

/* ---- Root (2026-11-15): the FIFO words straight from guest memory ----------------------------------------------------
 * Console run rv58s: 98 % of the tick thread's time was inside the pump, and only 22 % of that was the renderer (STARTUP_
 * RENDER_TIME); the rest was this consumer reading every word through the atomic domain (one guard lock, a 71-owner scan and
 * three fences per word, a second guard in the demand window, the flat space's mutex), against a guard five other threads
 * were queueing on. rsx_live_ports::pointer gives the consumer the memory itself; see the header for the contract. */
enum { WINDOW_BYTES = 0x10000u, PAGE_BYTES = 0x1000u };
typedef struct pump_window {
    const uint8_t *base;      /* guest IO offset `lo` as a host pointer; NULL = this range is read through read32 */
    uint32_t lo, hi;          /* [lo, hi) in IO offsets that `base` (or the NULL answer) covers */
} pump_window;

static uint32_t be32_at(const uint8_t *p)
{
    const uint32_t raw = __atomic_load_n((const uint32_t *)(const void *)p, __ATOMIC_RELAXED);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return raw;
#else
    return __builtin_bswap32(raw);
#endif
}

/* One FIFO word at IO offset `offset` (the caller checked offset_ok / that the packet lies inside the mapping). */
static int fetch(rsx_live_fifo *fifo, const rsx_live_ports *ports, pump_window *window, uint32_t offset, uint32_t *value)
{
    if (ports->pointer != NULL) {
        if (offset < window->lo || offset >= window->hi) {
            uint32_t lo = offset & ~(uint32_t)(WINDOW_BYTES - 1u);
            uint32_t length = fifo->layout.io_size - lo < WINDOW_BYTES ? fifo->layout.io_size - lo : WINDOW_BYTES;
            const uint8_t *pointer = ports->pointer(ports->user, fifo->layout.io_address + lo, length);
            if (pointer == NULL || ((uintptr_t)pointer & 3u) != 0u) {   /* not all of it: just the page of this word */
                lo = offset & ~(uint32_t)(PAGE_BYTES - 1u);
                length = fifo->layout.io_size - lo < PAGE_BYTES ? fifo->layout.io_size - lo : PAGE_BYTES;
                pointer = ports->pointer(ports->user, fifo->layout.io_address + lo, length);
                if (pointer != NULL && ((uintptr_t)pointer & 3u) != 0u) pointer = NULL;
            }
            window->base = pointer;   /* NULL is remembered for the page: its words go through read32 without asking again */
            window->lo = lo;
            window->hi = lo + length;
        }
        if (window->base != NULL) {
            *value = be32_at(window->base + (offset - window->lo));
            ++fifo->direct_words;
            return 0;
        }
    }
    ++fifo->port_words;
    return ports->read32(ports->user, fifo->layout.io_address + offset, value);
}

static int flush_get(rsx_live_fifo *fifo, const rsx_live_ports *ports)
{
    if (ports->write32(ports->user, fifo->layout.control_address + 4u, fifo->get) != 0) return 1;
    fifo->get_dirty = 0u;
    fifo->flushed_words = fifo->words;
    ++fifo->get_writes;
    return 0;
}

static rsx_live_status pump_run(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t word_budget)
{
    uint32_t put = 0, used = 0;
    pump_window window = {NULL, 0u, 0u};
    ++fifo->pumps;
    if (ports->read32(ports->user, fifo->layout.control_address, &put) != 0)
        return fault(fifo, RSX_LIVE_FAULT_READ, 0u, 0u);
    if (!fifo->started) {
        uint32_t get = 0;
        if (ports->read32(ports->user, fifo->layout.control_address + 4u, &get) != 0)
            return fault(fifo, RSX_LIVE_FAULT_READ, 0u, 0u);
        fifo->get = get;
        fifo->started = 1u;
    }
    fifo->last_put = put;
    if (ports->pointer != NULL) __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* the words below `put` are read after `put` */
    for (;;) {
        uint32_t word = 0, count, method, subchannel, index;
        if (fifo->get == put) return RSX_LIVE_IDLE;
        if (used >= word_budget) return RSX_LIVE_BUDGET;
        if (!offset_ok(fifo, fifo->get)) return fault(fifo, RSX_LIVE_FAULT_OFFSET, fifo->get, 0u);
        if (fetch(fifo, ports, &window, fifo->get, &word) != 0)
            return fault(fifo, RSX_LIVE_FAULT_READ, fifo->get, 0u);
        if (ports->pointer != NULL && ((word & RSX_LIVE_JUMP_OLD_MASK) == RSX_LIVE_JUMP_OLD || (word & RSX_LIVE_JUMP_NEW_MASK) == RSX_LIVE_JUMP_NEW ||
                                       (word & 3u) == RSX_LIVE_CALL))
            __atomic_thread_fence(__ATOMIC_ACQUIRE);   /* what the branch was patched in front of is read after the word itself */
        if ((word & RSX_LIVE_JUMP_OLD_MASK) == RSX_LIVE_JUMP_OLD || (word & RSX_LIVE_JUMP_NEW_MASK) == RSX_LIVE_JUMP_NEW) {
            const uint32_t target = (word & RSX_LIVE_JUMP_OLD_MASK) == RSX_LIVE_JUMP_OLD
                ? (word & 0x1FFFFFFCu) : (word & 0xFFFFFFFCu);
            if (target == fifo->get) return RSX_LIVE_PARKED;
            if (!offset_ok(fifo, target)) return fault(fifo, RSX_LIVE_FAULT_OFFSET, fifo->get, word);
            fifo->get = target;
            ++fifo->jumps; ++fifo->words; ++used;
        } else if ((word & 3u) == RSX_LIVE_CALL) {
            const uint32_t target = word & 0xFFFFFFFCu;
            if (fifo->in_call) return fault(fifo, RSX_LIVE_FAULT_CALL_DEPTH, fifo->get, word);
            if (!offset_ok(fifo, target)) return fault(fifo, RSX_LIVE_FAULT_OFFSET, fifo->get, word);
            fifo->call_return = fifo->get + 4u;
            fifo->in_call = 1u;
            fifo->get = target;
            ++fifo->calls; ++fifo->words; ++used;
        } else if (word == RSX_LIVE_RETURN) {
            if (!fifo->in_call) return fault(fifo, RSX_LIVE_FAULT_RETURN, fifo->get, word);
            fifo->get = fifo->call_return;
            fifo->in_call = 0u;
            ++fifo->returns; ++fifo->words; ++used;
        } else {
            count = (word >> 18) & 0x7FFu;
            method = word & 0x1FFCu;
            subchannel = (word >> 13) & 7u;
            /* The whole packet must lie before put (put never sits inside a packet the game finished)
             * and inside the mapping. A packet that crosses put is waited for. */
            if (count != 0u) {
                const uint32_t bytes = count * 4u;
                if (fifo->layout.io_size - fifo->get - 4u < bytes)
                    return fault(fifo, RSX_LIVE_FAULT_OFFSET, fifo->get, word);
                if (put > fifo->get && put - fifo->get - 4u < bytes) return RSX_LIVE_NEED_INPUT;
            }
            for (index = 0; index < count; ++index) {
                uint32_t data = 0;
                const uint32_t at = fifo->get + 4u + index * 4u;
                const uint32_t this_method = (word & RSX_LIVE_NON_INCREMENT) ? method : method + index * 4u;
                int outcome;
                if (fetch(fifo, ports, &window, at, &data) != 0)
                    return fault(fifo, RSX_LIVE_FAULT_READ, at, word);
                outcome = execute(fifo, ports, subchannel, this_method, data, at);
                if (outcome == 2) return RSX_LIVE_FAULT;
                if (outcome == 1) {
                    /* An unsatisfied acquire: the packet is retried from its header. Only acquire
                     * packets of one word are expected; earlier words of a longer packet would repeat. */
                    return RSX_LIVE_WAIT_SEMAPHORE;
                }
                ++fifo->methods;
                ++fifo->methods_by_subchannel[subchannel];
            }
            fifo->get += 4u + count * 4u;
            ++fifo->packets;
            fifo->words += 1u + count;
            used += 1u + count;
        }
        fifo->get_dirty = 1u;
        if (fifo->get_batch_words == 0u || fifo->words - fifo->flushed_words >= fifo->get_batch_words) {
            if (flush_get(fifo, ports) != 0) return fault(fifo, RSX_LIVE_FAULT_WRITE, fifo->get, 0u);
        }
    }
}

rsx_live_status rsx_live_pump(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t word_budget)
{
    rsx_live_status status;
    if (fifo == NULL || ports == NULL || ports->read32 == NULL || ports->write32 == NULL) return RSX_LIVE_FAULT;
    if (fifo->fault) return RSX_LIVE_FAULT;
    status = pump_run(fifo, ports, word_budget);
    /* every way out of the loop above leaves the guest-visible `get` where the last completed packet ended */
    if (fifo->get_dirty && !(fifo->fault && fifo->fault_reason == RSX_LIVE_FAULT_WRITE) && flush_get(fifo, ports) != 0) {
        fault(fifo, RSX_LIVE_FAULT_WRITE, fifo->get, 0u);
        status = RSX_LIVE_FAULT;
    }
    return status;
}
