/* Live RSX command-buffer consumer ("null renderer" stage).
 *
 * The game writes NV40 method packets into its command buffer, advances `put` in the control register
 * and then waits for the RSX: for `get` to catch up, for `ref` to take a SET_REFERENCE value, or for a
 * label (semaphore) to be written. Nothing consumed the buffer after cellGcmInit, so the game's main
 * loop slept for ever. This core walks the buffer from `get` to `put`, executes the methods that are
 * visible to the PPU (reference, semaphores/labels, reports, the port's own flip marker) and COUNTS
 * every other method: there is no drawing here. A renderer is attached later through `method`.
 *
 * Platform-free and instance-owned: no globals, no threads, no clock. Guest memory is reached only
 * through the injected ports; every guest word is big-endian. One caller at a time.
 *
 * Provenance: the FIFO word layout, the method numbers and the label byte order are root's
 * recollection of the NV40/RSX conventions as RPCS3 and the public GCM headers describe them, not
 * extracted from firmware. The flip marker (subchannel 7, method 0x944) is this port's own word:
 * producer (_cellGcmSetFlipCommand arm) and consumer are both native.
 */
#ifndef RSX_LIVE_FIFO_H
#define RSX_LIVE_FIFO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FIFO control words. */
#define RSX_LIVE_JUMP_OLD_MASK 0xE0000003u
#define RSX_LIVE_JUMP_OLD 0x20000000u /* target = word & 0x1FFFFFFC */
#define RSX_LIVE_JUMP_NEW_MASK 0x00000003u
#define RSX_LIVE_JUMP_NEW 0x00000001u /* target = word & 0xFFFFFFFC */
#define RSX_LIVE_CALL 0x00000002u     /* target = word & 0xFFFFFFFC */
#define RSX_LIVE_RETURN 0x00020000u
#define RSX_LIVE_NON_INCREMENT 0x40000000u

/* Channel methods (valid on every subchannel). */
#define RSX_LIVE_NV406E_SET_REFERENCE 0x0050u
#define RSX_LIVE_NV406E_SET_CONTEXT_DMA_SEMAPHORE 0x0060u
#define RSX_LIVE_NV406E_SEMAPHORE_OFFSET 0x0064u
#define RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE 0x0068u
#define RSX_LIVE_NV406E_SEMAPHORE_RELEASE 0x006Cu
/* NV4097 methods the PPU can observe. */
#define RSX_LIVE_NV4097_GET_REPORT 0x1800u
#define RSX_LIVE_NV4097_SET_BEGIN_END 0x1808u
#define RSX_LIVE_NV4097_SET_SEMAPHORE_OFFSET 0x1D6Cu
#define RSX_LIVE_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70u
#define RSX_LIVE_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE 0x1D74u
#define RSX_LIVE_NV4097_CLEAR_SURFACE 0x1D94u
/* This port's flip marker: header (1 << 18) | (7 << 13) | 0x944, one data word = display buffer id. */
#define RSX_LIVE_FLIP_SUBCHANNEL 7u
#define RSX_LIVE_FLIP_METHOD 0x0944u
#define RSX_LIVE_FLIP_HEADER ((1u << 18) | (RSX_LIVE_FLIP_SUBCHANNEL << 13) | RSX_LIVE_FLIP_METHOD)

/* Semaphore DMA handles: the label area (RW / R) and the driver's device area. */
#define RSX_LIVE_DMA_LABEL_RW 0x66606660u
#define RSX_LIVE_DMA_LABEL_R 0x66616661u

typedef struct rsx_live_ports {
    void *user;
    /* Both return 0 on success. Addresses are guest effective addresses. */
    int (*read32)(void *user, uint32_t address, uint32_t *value);
    int (*write32)(void *user, uint32_t address, uint32_t value);
    /* Optional: every method that was not executed here (the future renderer). */
    void (*method)(void *user, uint32_t subchannel, uint32_t method, uint32_t data);
    /* Optional: a flip of display buffer `buffer_id` was reached in the stream. */
    void (*flip)(void *user, uint32_t buffer_id);
    /* Optional (Root 2026-11-15, renderer CPU work): a host pointer to the guest bytes [address, address + bytes) when
     * every one of them can be read in place (committed flat guest memory), else NULL. When it is set, the FIFO words
     * (command headers, method data, jump / call words; never the control register, labels or reports) are read from
     * the returned memory with aligned relaxed 32-bit loads instead of one read32 per word, and read32 stays the
     * answer for every range the pointer port refuses. The words are written by the PPU without any lock, so the
     * guarded read32 never ordered anything against that writer either; `put` is still read through read32 (its
     * fences order the loads that follow) and a branch word is followed by an acquire fence. The pointer is only
     * used until the pump returns. */
    const uint8_t *(*pointer)(void *user, uint32_t address, uint32_t bytes);
} rsx_live_ports;
/* Feature macro: this header has the pointer port, rsx_live_fifo.get_batch_words and the counters below. */
#define RSX_LIVE_HAS_DIRECT 1

typedef struct rsx_live_layout {
    uint32_t control_address; /* be32 put, get, ref */
    uint32_t io_address;      /* EA of IO offset 0 */
    uint32_t io_size;         /* bytes of the IO mapping */
    uint32_t label_address;   /* EA of label 0; label i is at +16 * i */
    uint32_t label_size;      /* bytes of the label / report area */
    uint32_t report_offset;   /* reports start at label_address + report_offset (0x1400 on the RSX) */
} rsx_live_layout;

typedef enum rsx_live_status {
    RSX_LIVE_IDLE = 0,       /* get == put */
    RSX_LIVE_BUDGET,         /* word budget used up; more is waiting */
    RSX_LIVE_NEED_INPUT,     /* put lies inside a packet's payload */
    RSX_LIVE_WAIT_SEMAPHORE, /* a SEMAPHORE_ACQUIRE is not satisfied yet */
    RSX_LIVE_PARKED,         /* a jump to itself */
    RSX_LIVE_FAULT           /* latched: bad offset, unreadable word, port failure */
} rsx_live_status;

typedef struct rsx_live_fifo {
    rsx_live_layout layout;
    uint32_t started;
    uint32_t get;                /* IO offset of the next word */
    uint32_t call_return, in_call;
    uint32_t semaphore_dma;      /* NV406E_SET_CONTEXT_DMA_SEMAPHORE */
    uint32_t semaphore_offset;   /* NV406E_SEMAPHORE_OFFSET */
    uint32_t semaphore_offset_4097;
    uint32_t fault, fault_offset, fault_word, fault_reason;
    uint32_t last_reference, last_flip_buffer, last_put;
    uint64_t pumps, words, methods, packets, jumps, calls, returns;
    uint64_t references, label_writes, acquires, acquire_waits, device_semaphore_ops;
    uint64_t reports, flips, draws, clears, unknown_control;
    uint64_t methods_by_subchannel[8];
    /* Root (2026-11-15): control-register `get` updates. 0 (the default after rsx_live_init) = after every packet, as
     * before. n > 0 = once at least n words were consumed since the last update, and always before rsx_live_pump
     * returns, so the guest sees `get` lag by at most n words inside one pump and exactly as before between pumps.
     * Set by the caller after rsx_live_init. */
    uint32_t get_batch_words;
    uint32_t get_dirty;          /* `get` moved since the last control-register update */
    uint64_t flushed_words;      /* `words` at the last control-register update */
    /* Counters: FIFO words read in place / through read32, control-register `get` writes. */
    uint64_t direct_words, port_words, get_writes;
} rsx_live_fifo;

/* Fault reasons. */
#define RSX_LIVE_FAULT_NONE 0u
#define RSX_LIVE_FAULT_OFFSET 1u    /* get or a branch target outside the IO mapping, or misaligned */
#define RSX_LIVE_FAULT_READ 2u
#define RSX_LIVE_FAULT_WRITE 3u
#define RSX_LIVE_FAULT_CALL_DEPTH 4u
#define RSX_LIVE_FAULT_RETURN 5u
#define RSX_LIVE_FAULT_LABEL 6u     /* semaphore / report offset outside the label area */

void rsx_live_init(rsx_live_fifo *fifo, const rsx_live_layout *layout);

/* Processes up to `word_budget` FIFO words. The first call adopts the control register's `get`.
 * After every packet the control register's `get` is updated. Never blocks. */
rsx_live_status rsx_live_pump(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t word_budget);

/* A flip requested outside the stream (cellGcmSetFlipImmediate). */
void rsx_live_flip_now(rsx_live_fifo *fifo, const rsx_live_ports *ports, uint32_t buffer_id);

#ifdef __cplusplus
}
#endif

#endif /* RSX_LIVE_FIFO_H */
