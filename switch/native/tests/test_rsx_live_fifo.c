/* Host test for native/domain/rsx_live_fifo.{h,c}: a fake guest memory with a control register, an IO
 * mapping and a label area. */
#include "../domain/rsx_live_fifo.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { CONTROL = 0x1000, IO = 0x10000, IO_SIZE = 0x4000, LABEL = 0x20000, LABEL_SIZE = 0x2000, MEMORY = 0x30000 };
static uint8_t g_memory[MEMORY];
static unsigned g_flips, g_last_flip, g_forwarded, g_last_method, g_last_data, g_fail_writes;

static int read32(void *user, uint32_t address, uint32_t *value)
{
    (void)user;
    if (address > MEMORY - 4u) return 1;
    *value = ((uint32_t)g_memory[address] << 24) | ((uint32_t)g_memory[address + 1] << 16) |
             ((uint32_t)g_memory[address + 2] << 8) | g_memory[address + 3];
    return 0;
}
static unsigned g_control_get_writes;                    /* writes the consumer made to the control register's `get` */
static int write32(void *user, uint32_t address, uint32_t value)
{
    (void)user;
    if (g_fail_writes || address > MEMORY - 4u) return 1;
    if (address == CONTROL + 4u) ++g_control_get_writes;
    g_memory[address] = (uint8_t)(value >> 24); g_memory[address + 1] = (uint8_t)(value >> 16);
    g_memory[address + 2] = (uint8_t)(value >> 8); g_memory[address + 3] = (uint8_t)value;
    return 0;
}
static void on_method(void *user, uint32_t subchannel, uint32_t method, uint32_t data)
{
    (void)user; (void)subchannel; ++g_forwarded; g_last_method = method; g_last_data = data;
}
static void on_flip(void *user, uint32_t buffer) { (void)user; ++g_flips; g_last_flip = buffer; }
/* Root (2026-11-15): the in-place pointer port. g_hole_lo/g_hole_hi (IO offsets) is a range it refuses, g_pointer_calls counts
 * its calls, g_misalign makes it answer one byte off. Outside the IO mapping it answers NULL. */
static uint32_t g_hole_lo, g_hole_hi, g_pointer_calls, g_misalign;
static const uint8_t *pointer_port(void *user, uint32_t address, uint32_t bytes)
{
    (void)user; ++g_pointer_calls;
    if (address < IO || (uint64_t)address + bytes > (uint64_t)IO + IO_SIZE) return NULL;
    if (g_hole_hi > g_hole_lo && (uint64_t)address - IO < g_hole_hi && (uint64_t)address - IO + bytes > g_hole_lo) return NULL;
    return g_memory + address + (g_misalign ? 1u : 0u);
}
static const rsx_live_ports PORTS = {NULL, read32, write32, on_method, on_flip, NULL};
static const rsx_live_ports PORTS_DIRECT = {NULL, read32, write32, on_method, on_flip, pointer_port};
static const rsx_live_layout LAYOUT = {CONTROL, IO, IO_SIZE, LABEL, LABEL_SIZE, 0x1400};

static uint32_t g_cursor;
static void emit(uint32_t word) { write32(NULL, IO + g_cursor, word); g_cursor += 4; }
static void packet1(uint32_t subchannel, uint32_t method, uint32_t data)
{
    emit((1u << 18) | (subchannel << 13) | method); emit(data);
}
static void set_put(uint32_t put) { write32(NULL, CONTROL, put); }
static uint32_t word_at(uint32_t address) { uint32_t v = 0; read32(NULL, address, &v); return v; }
static void reset(rsx_live_fifo *fifo, uint32_t start)
{
    memset(g_memory, 0, sizeof g_memory);
    g_flips = g_last_flip = g_forwarded = g_last_method = g_last_data = g_fail_writes = 0;
    g_cursor = start;
    write32(NULL, CONTROL, start); write32(NULL, CONTROL + 4, start); write32(NULL, CONTROL + 8, 0xFFFFFFFFu);
    rsx_live_init(fifo, &LAYOUT);
}

static void test_reference_and_get(void)
{
    rsx_live_fifo f;
    reset(&f, 0x100);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE);
    CHECK(f.started == 1 && f.get == 0x100);
    packet1(0, RSX_LIVE_NV406E_SET_REFERENCE, 0x1234);
    emit((2u << 18) | 0x0304u); emit(0xAAAA); emit(0xBBBB);            /* two incrementing NV4097 methods */
    emit(RSX_LIVE_NON_INCREMENT | (2u << 18) | 0x1810u); emit(1); emit(2); /* two words to one method */
    emit(0);                                                            /* a NOP header (count 0) */
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE);
    CHECK(word_at(CONTROL + 8) == 0x1234 && f.references == 1 && f.last_reference == 0x1234);
    CHECK(word_at(CONTROL + 4) == g_cursor && f.get == g_cursor);
    CHECK(g_forwarded == 4 && g_last_method == 0x1810 && g_last_data == 2);
    CHECK(f.methods == 5 && f.packets == 4 && f.words == 9 && f.methods_by_subchannel[0] == 5);
}

static void test_budget_and_need_input(void)
{
    rsx_live_fifo f;
    reset(&f, 0);
    packet1(0, 0x0304, 1); packet1(0, 0x0308, 2); packet1(0, 0x030C, 3);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 2) == RSX_LIVE_BUDGET && f.get == 8);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE && f.get == 24);
    emit((3u << 18) | 0x0400u); emit(1); emit(2);       /* put published inside the packet */
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_NEED_INPUT && f.get == 24 && g_forwarded == 3);
    emit(3); set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE && g_forwarded == 6 && g_last_method == 0x0408);
}

static void test_jump_call_return(void)
{
    rsx_live_fifo f;
    reset(&f, 0);
    emit(RSX_LIVE_JUMP_OLD | 0x100u);                   /* old-form jump to 0x100 */
    g_cursor = 0x100;
    emit(0x200u | RSX_LIVE_CALL);                       /* call 0x200 */
    packet1(0, 0x0304, 7);                              /* after the return */
    emit(0x300u | RSX_LIVE_JUMP_NEW);                   /* new-form jump to 0x300 */
    g_cursor = 0x200; packet1(0, 0x0308, 8); emit(RSX_LIVE_RETURN);
    g_cursor = 0x300; packet1(0, 0x030C, 9);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE);
    CHECK(f.jumps == 2 && f.calls == 1 && f.returns == 1 && g_forwarded == 3 && g_last_data == 9 && f.get == 0x308);
    /* a jump to itself parks; a return without a call and a nested call are faults */
    reset(&f, 0x40); emit(RSX_LIVE_JUMP_OLD | 0x40u); set_put(0x80);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_PARKED && f.fault == 0);
    reset(&f, 0); emit(RSX_LIVE_RETURN); set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_RETURN);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT);                    /* latched */
    reset(&f, 0); emit(0x100u | RSX_LIVE_CALL); g_cursor = 0x100; emit(0x200u | RSX_LIVE_CALL); set_put(0x400);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_CALL_DEPTH);
    reset(&f, 0); emit(RSX_LIVE_JUMP_OLD | (IO_SIZE + 0x100u)); set_put(4);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_OFFSET);
}

static void test_semaphores_reports_flip(void)
{
    rsx_live_fifo f;
    reset(&f, 0);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_OFFSET, 0x30);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_RELEASE, 0xCAFE0001);
    packet1(0, RSX_LIVE_NV4097_SET_SEMAPHORE_OFFSET, 0x40);
    packet1(0, RSX_LIVE_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, 0x11223344);
    packet1(0, RSX_LIVE_NV4097_SET_SEMAPHORE_OFFSET, 0x50);
    packet1(0, RSX_LIVE_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE, 0x55667788);
    packet1(0, RSX_LIVE_NV4097_GET_REPORT, (1u << 24) | 0x20u);
    packet1(RSX_LIVE_FLIP_SUBCHANNEL, RSX_LIVE_FLIP_METHOD, 1);
    packet1(0, RSX_LIVE_NV4097_SET_BEGIN_END, 5); packet1(0, RSX_LIVE_NV4097_SET_BEGIN_END, 0);
    packet1(0, RSX_LIVE_NV4097_CLEAR_SURFACE, 0xF3);
    set_put(g_cursor);
    write32(NULL, LABEL + 0x1400 + 0x20 + 8, 0x77777777);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE);
    CHECK(word_at(LABEL + 0x30) == 0xCAFE0001);
    CHECK(word_at(LABEL + 0x40) == 0x11443322);          /* R and B swapped */
    CHECK(word_at(LABEL + 0x50) == 0x55667788);
    CHECK(word_at(LABEL + 0x1400 + 0x20 + 8) == 0 && f.reports == 1 && f.label_writes == 3);
    CHECK(g_flips == 1 && g_last_flip == 1 && f.flips == 1 && f.draws == 1 && f.clears == 1);
    CHECK(g_forwarded == 3);                              /* begin, end, clear reach the renderer port */

    /* acquire waits until the label holds the value, without consuming the packet */
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_OFFSET, 0x60);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE, 9);
    packet1(0, RSX_LIVE_NV406E_SET_REFERENCE, 0x99);
    set_put(g_cursor);
    {
        const uint32_t before = g_cursor - 16;
        CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_WAIT_SEMAPHORE && f.get == before && f.acquire_waits == 1);
        CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_WAIT_SEMAPHORE && word_at(CONTROL + 8) != 0x99);
        write32(NULL, LABEL + 0x60, 9);
        CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE && f.acquires == 1 && word_at(CONTROL + 8) == 0x99);
    }
    /* the device semaphore area is not modelled: acquire passes, release writes nothing */
    packet1(0, RSX_LIVE_NV406E_SET_CONTEXT_DMA_SEMAPHORE, 0x56616661);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_OFFSET, 0x70);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE, 3);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_RELEASE, 4);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_IDLE && f.device_semaphore_ops == 2 && word_at(LABEL + 0x70) == 0);
    /* a label offset outside the area is a fault, and so is a failing write */
    packet1(0, RSX_LIVE_NV406E_SET_CONTEXT_DMA_SEMAPHORE, RSX_LIVE_DMA_LABEL_RW);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_OFFSET, LABEL_SIZE);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_RELEASE, 1);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_LABEL);
    reset(&f, 0); packet1(0, RSX_LIVE_NV406E_SET_REFERENCE, 1); set_put(g_cursor); g_fail_writes = 1;
    CHECK(rsx_live_pump(&f, &PORTS, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_WRITE);
    g_fail_writes = 0;
    rsx_live_flip_now(&f, &PORTS, 0);
    CHECK(f.flips == 1 && g_flips == 1 && g_last_flip == 0);
    CHECK(rsx_live_pump(NULL, &PORTS, 1) == RSX_LIVE_FAULT);
}

/* A FIFO of many packets (incrementing, non-incrementing, a NOP, a jump and a call / return, a flip, a reference) pumped with every
 * combination of port set and get batching must end in the same state: same forwarded methods (checksummed in order), same
 * counters, same control register, same `get`. */
static uint64_t g_stream_hash;
static uint32_t g_end;                                   /* `put` of the mixed stream: after the last packet of the main path */
static void hash_method(void *user, uint32_t subchannel, uint32_t method, uint32_t data)
{
    (void)user; ++g_forwarded;
    g_stream_hash = (g_stream_hash ^ (((uint64_t)subchannel << 56) ^ ((uint64_t)method << 32) ^ data)) * 0x100000001B3ull;
}
static void build_mixed(void)
{
    uint32_t i, jump_site;
    for (i = 0; i < 200; ++i) {
        packet1(0, 0x0304u + (i % 8u) * 4u, 0x1000u + i);
        emit((3u << 18) | 0x0B80u); emit(i); emit(i * 3u); emit(i * 5u);
        emit(RSX_LIVE_NON_INCREMENT | (2u << 18) | 0x1810u); emit(i); emit(i + 1u);
        if (i % 50u == 7u) emit(0);
        if (i % 40u == 9u) packet1(RSX_LIVE_FLIP_SUBCHANNEL, RSX_LIVE_FLIP_METHOD, i & 1u);
        if (i % 60u == 11u) packet1(0, RSX_LIVE_NV406E_SET_REFERENCE, 0x500u + i);
    }
    jump_site = g_cursor; emit(0);                        /* patched below: a jump over a gap */
    g_cursor += 0x80;                                     /* the gap (zero words: NOP headers, never reached) */
    emit(0x2000u | RSX_LIVE_CALL);                        /* call 0x2000 ... */
    packet1(0, 0x0310, 0x77);                             /* ... after the return */
    g_end = g_cursor;
    write32(NULL, IO + jump_site, RSX_LIVE_JUMP_OLD | (jump_site + 4u + 0x80u));
    g_cursor = 0x2000; packet1(0, 0x0314, 0x88); emit(RSX_LIVE_RETURN);
}
static void run_mixed(const rsx_live_ports *ports, uint32_t batch, uint32_t budget, uint32_t *get_out, rsx_live_fifo *out)
{
    rsx_live_fifo f;
    rsx_live_status st;
    uint32_t guard = 0;
    reset(&f, 0);
    build_mixed();
    set_put(g_end);
    f.get_batch_words = batch;
    g_stream_hash = 0xCBF29CE484222325ull;
    g_pointer_calls = 0;
    do { st = rsx_live_pump(&f, ports, budget); } while (st == RSX_LIVE_BUDGET && ++guard < 100000u);
    CHECK(st == RSX_LIVE_IDLE && f.fault == 0);
    *get_out = word_at(CONTROL + 4);
    *out = f;
}
static void test_direct_equals_port(void)
{
    static const uint32_t batches[] = {0u, 1u, 7u, 64u, 100000u};
    static const uint32_t budgets[] = {13u, 100u, 0x20000u};
    uint32_t b, k, ref_get = 0;
    uint64_t ref_hash = 0, ref_ref = 0;
    rsx_live_fifo ref, f;
    rsx_live_ports plain = PORTS, direct = PORTS_DIRECT;
    plain.method = hash_method; direct.method = hash_method;
    run_mixed(&plain, 0u, 0x20000u, &ref_get, &ref);
    ref_hash = g_stream_hash; ref_ref = word_at(CONTROL + 8);
    CHECK(ref_get == g_end && ref.get == g_end && ref.direct_words == 0 && ref.port_words > 0 && ref.get_writes > 0);
    CHECK(ref.jumps == 1 && ref.calls == 1 && ref.returns == 1 && ref.flips == 5 && ref.references == 4);
    for (b = 0; b < sizeof batches / sizeof batches[0]; ++b)
        for (k = 0; k < sizeof budgets / sizeof budgets[0]; ++k) {
            uint32_t get = 0;
            run_mixed(&plain, batches[b], budgets[k], &get, &f);
            CHECK(g_stream_hash == ref_hash && get == ref_get && f.get == ref.get && f.words == ref.words && f.packets == ref.packets &&
                  f.methods == ref.methods && f.flips == ref.flips && word_at(CONTROL + 8) == ref_ref && f.direct_words == 0);
            run_mixed(&direct, batches[b], budgets[k], &get, &f);
            CHECK(g_stream_hash == ref_hash && get == ref_get && f.get == ref.get && f.words == ref.words && f.packets == ref.packets &&
                  f.methods == ref.methods && f.flips == ref.flips && word_at(CONTROL + 8) == ref_ref);
            CHECK(f.direct_words > 0 && f.port_words == 0 && g_pointer_calls > 0);
            /* batching writes `get` less often, never more often than once per packet */
            if (batches[b] >= 64u && budgets[k] == 0x20000u) CHECK(f.get_writes <= f.words / batches[b] + 2u && f.get_writes < ref.get_writes);
        }
}

/* Words in a range the pointer port refuses are read through read32; the stream is unchanged. A pointer that is not 4-byte
 * aligned is refused too (the loads are aligned). */
static void test_direct_fallback(void)
{
    rsx_live_fifo f, base;
    rsx_live_ports plain = PORTS, direct = PORTS_DIRECT;
    uint32_t get = 0, ref_get = 0;
    uint64_t ref_hash;
    plain.method = hash_method; direct.method = hash_method;
    run_mixed(&plain, 0u, 0x20000u, &ref_get, &base);
    ref_hash = g_stream_hash;
    g_hole_lo = 0x300; g_hole_hi = 0x900;                 /* the middle of the stream */
    run_mixed(&direct, 64u, 0x20000u, &get, &f);
    CHECK(g_stream_hash == ref_hash && get == ref_get && f.words == base.words);
    CHECK(f.direct_words > 0 && f.port_words > 0 && f.direct_words + f.port_words == base.port_words);
    g_hole_lo = g_hole_hi = 0;
    g_misalign = 1;
    run_mixed(&direct, 64u, 0x20000u, &get, &f);
    CHECK(g_stream_hash == ref_hash && get == ref_get && f.direct_words == 0 && f.port_words == base.port_words);
    g_misalign = 0;
}

/* get batching: the guest-visible `get` lags inside a pump by less than the batch, and is exact whenever a pump returns, whatever the status. */
static void test_get_batching(void)
{
    rsx_live_fifo f;
    uint32_t i;
    reset(&f, 0);
    f.get_batch_words = 10u;
    for (i = 0; i < 20; ++i) packet1(0, 0x0304, i);      /* 20 packets of 2 words */
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 15) == RSX_LIVE_BUDGET);
    CHECK(word_at(CONTROL + 4) == f.get && f.get > 0 && f.get_dirty == 0);       /* flushed on the way out */
    CHECK(f.get_writes < 8u);                                                     /* not once per packet */
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 0x20000) == RSX_LIVE_IDLE && word_at(CONTROL + 4) == g_cursor);
    /* inside one long pump `get` is published every batch (40 words, batch 10: at 10, 20, 30, 40), not only at the end */
    reset(&f, 0);
    f.get_batch_words = 10u;
    for (i = 0; i < 20; ++i) packet1(0, 0x0304, i);
    set_put(g_cursor);
    g_control_get_writes = 0;
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 0x20000) == RSX_LIVE_IDLE && word_at(CONTROL + 4) == g_cursor);
    CHECK(g_control_get_writes >= 3u && g_control_get_writes <= 5u);
    /* batch 0 = after every packet, as before the change */
    reset(&f, 0);
    for (i = 0; i < 20; ++i) packet1(0, 0x0304, i);
    set_put(g_cursor);
    g_control_get_writes = 0;
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 0x20000) == RSX_LIVE_IDLE && g_control_get_writes == 20u);
    /* an unsatisfied acquire: `get` stops in front of the acquire packet and says so */
    reset(&f, 0);
    f.get_batch_words = 1000u;
    packet1(0, 0x0304, 1); packet1(0, 0x0308, 2);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_OFFSET, 0x60);
    packet1(0, RSX_LIVE_NV406E_SEMAPHORE_ACQUIRE, 9);
    packet1(0, 0x030C, 3);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_WAIT_SEMAPHORE && word_at(CONTROL + 4) == 24u && f.get == 24u);
    write32(NULL, LABEL + 0x60, 9);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_IDLE && word_at(CONTROL + 4) == g_cursor);
    /* a packet that crosses put is waited for; the packets before it are visible */
    reset(&f, 0);
    f.get_batch_words = 1000u;
    packet1(0, 0x0304, 1);
    emit((3u << 18) | 0x0400u); emit(1);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_NEED_INPUT && word_at(CONTROL + 4) == 8u);
    /* a fault keeps the get of the packets that completed, and a failing control-register write is a write fault */
    reset(&f, 0);
    f.get_batch_words = 1000u;
    packet1(0, 0x0304, 1); packet1(0, 0x0308, 2); emit(RSX_LIVE_RETURN);
    set_put(g_cursor);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_RETURN && word_at(CONTROL + 4) == 16u);
    reset(&f, 0);
    f.get_batch_words = 1000u;
    packet1(0, 0x0304, 1);
    set_put(g_cursor);
    g_fail_writes = 1;
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_FAULT && f.fault_reason == RSX_LIVE_FAULT_WRITE);
    g_fail_writes = 0;
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_FAULT);               /* latched */
    /* a parked jump-to-self reports PARKED with `get` exact; patching the word later is followed */
    reset(&f, 0);
    f.get_batch_words = 1000u;
    packet1(0, 0x0304, 1);
    emit(RSX_LIVE_JUMP_OLD | g_cursor);
    set_put(g_cursor + 0x40);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_PARKED && word_at(CONTROL + 4) == 8u);
    write32(NULL, IO + 8, (1u << 18) | 0x0308u); write32(NULL, IO + 12, 5u);
    set_put(16u);
    CHECK(rsx_live_pump(&f, &PORTS_DIRECT, 100) == RSX_LIVE_IDLE && word_at(CONTROL + 4) == 16u && g_last_data == 5u);
}

int main(void)
{
    test_reference_and_get();
    test_budget_and_need_input();
    test_jump_call_return();
    test_semaphores_reports_flip();
    test_direct_equals_port();
    test_direct_fallback();
    test_get_batching();
    printf("RSX_LIVE_FIFO checks=%u failures=%u\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
