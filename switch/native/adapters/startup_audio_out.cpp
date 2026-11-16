/* The output thread's pump. See startup_audio_out.h. */
#include "startup_audio_out.h"
#include "audio_out_horizon.h"
#include "../domain/audio_mix.h"
#include "../domain/audio_out_policy.h"
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#ifndef STARTUP_AUDIO_DIAG_SECONDS
#define STARTUP_AUDIO_DIAG_SECONDS 10u
#endif

namespace {
constexpr uint64_t DIAG_PERIOD_US = uint64_t(STARTUP_AUDIO_DIAG_SECONDS) * 1000000u;
constexpr uint32_t INDEX_WORD_BYTES = 8u;

// One port's run as the pump follows it.
struct PortRun {
    bool active = false;
    uint64_t start_us = 0;          // its PortStart time when the run was picked up: a restart, or a close and reopen, changes it
    uint64_t consumed = 0;          // blocks of this run already handed on
    uint32_t level_q15 = AUDIO_MIX_LEVEL_UNITY;
    uint64_t heard = 0;             // blocks of this port that held sound
    uint64_t written_silent = 0;    // blocks the game wrote (the reader's stamp was gone) with zeros
    uint64_t untouched = 0;         // blocks still wholly stamped: nothing wrote them since the reader took them
};

struct Pump {
    StartupAudioProcess *process = nullptr;
    const ps3_guest_memory *memory = nullptr;
    const ps3_clock *clock = nullptr;
    startup_audio_window_fn window = nullptr;
    PortRun runs[AUDIO_PORT_COUNT];
    uint64_t next_diag_us = 0;
};
Pump g_pump;

// The same lock as startup_execution.cpp's AudioProcessGuard (a spin that sleeps after 32 tries, so a lock holder of
// lower priority on this core is never starved).
void acquire(std::atomic_flag &flag)
{
    unsigned tries = 0;
    while (flag.test_and_set(std::memory_order_acquire))
        if (++tries >= 32u) { svcSleepThread(INT64_C(50000)); tries = 0; }
}

uint64_t load_be64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

float float_from_bits(uint32_t bits)
{
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// Non-zero words in [address, address + length) of guest memory, added to `count`; the first one's address goes to
// `first` when it is still unset. A range the window cannot map counts in `unmapped`.
void count_window_words(const Pump &p, uint32_t address, uint32_t length, uint32_t &count, uint32_t &first,
                        uint32_t &unmapped)
{
    if (!length) return;
    const uint8_t *bytes = static_cast<const uint8_t *>(p.window(address, length));
    if (!bytes) { ++unmapped; return; }
    uint32_t offset = 0;
    const uint32_t found = audio_mix_count_nonzero_words(bytes, length, &offset);
    if (found && first == UINT32_MAX) first = address + offset;
    count += found;
}

// ---- Decision 133: the SCREAM probe ----
// Guest addresses of THIS EBOOT (BCUS98119), read statically from the ELF and the recompiled PPU
// (source/infamous-recomp/recompiled/ppu_recomp_002.cpp):
//   0x0077160C  the TOC slot holding the brb driver object (0x009EB000, .bss): func_005525E8 loads it as [[r2-0x6140]-0x7FB8]
//   driver obj  +0x04 fill-callback descriptor (0x00783F18 -> code 0x00551E48), +0x0C callback user = the engine E,
//               +0x50 event port, +0x58 audio port, +0x5C portAddr, +0x60 readIndexAddr, +0x68 lead, +0x6C nBlock
//   0x009EDCB0  a gate word: the fill callback returns at once (writes nothing) while it is 0
//   engine E    +0x2DDC flags (bit 0 and bit 1 must be set or the callback returns at once), +0x2C/+0x30 command buffer base /
//               current, +0x44/+0x48 command indexes, +0x4C records per buffer, +0x2DE0 mix work ring base, +0x2DE4 its size,
//               +0x2DE8 the work block of the current command, 4 voice lists at +0x1580 + n * 0x230 (+0x00 head, nodes linked
//               by +0x04, +0x1C enabled byte, +0x24 count)
//   command     one 0x80-byte record per output block (func_0055B260): +0x0 timestamp, +0x8, +0xC the port block the SPU job
//               writes, +0x10, +0x14 work block, +0x18 flags
// Everything is read through the window (relaxed, no lock): a torn value is possible and harmless for a diagnostic.
constexpr uint32_t SCREAM_OBJECT_SLOT = 0x0077160Cu, SCREAM_OBJECT = 0x009EB000u, SCREAM_GATE = 0x009EDCB0u;
constexpr uint32_t SCREAM_LISTS = 4u, SCREAM_LIST_BASE = 0x1580u, SCREAM_LIST_STRIDE = 0x230u, SCREAM_LIST_WALK_MAX = 256u;
constexpr uint32_t SCREAM_WORK_SCAN_MAX = 0x100000u, SCREAM_COMMAND_BYTES = 0x80u;
constexpr uint32_t SCREAM_UNIT_DUMP_MAX = 8u, SCREAM_UNIT_DUMP_FIRST = 0x40u, SCREAM_UNIT_DUMP_LAST = 0xA0u;

bool guest_be32(const Pump &p, uint32_t address, uint32_t &value)
{
    const uint8_t *b = static_cast<const uint8_t *>(p.window(address, 4u));
    if (!b) return false;
    value = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
    return true;
}
uint32_t guest_word(const Pump &p, uint32_t address)
{
    uint32_t value = 0;
    return guest_be32(p, address, value) ? value : 0xDEADDEADu;
}
uint8_t guest_byte(const Pump &p, uint32_t address)
{
    const uint8_t *b = static_cast<const uint8_t *>(p.window(address, 1u));
    return b ? *b : 0xEEu;
}

void print_scream_probe(const Pump &p, unsigned long long t_ms)
{
    uint32_t object = 0;
    if (!guest_be32(p, SCREAM_OBJECT_SLOT, object) || object != SCREAM_OBJECT) {
        std::printf("STARTUP_SCREAM t_ms=%llu note=object_slot_mismatch slot=0x%08x value=0x%08x\n", t_ms,
            unsigned(SCREAM_OBJECT_SLOT), unsigned(object));
        return;
    }
    const uint32_t descriptor = guest_word(p, object + 0x04u);
    const uint32_t engine = guest_word(p, object + 0x0Cu);
    std::printf("STARTUP_SCREAM t_ms=%llu driver=0x%08x callback_desc=0x%08x callback_code=0x%08x engine=0x%08x event_port=0x%08x "
                "port=%u port_addr=0x%08x index_addr=0x%08x lead=%u nblock=%u gate=0x%08x\n",
        t_ms, unsigned(object), unsigned(descriptor), unsigned(guest_word(p, descriptor)), unsigned(engine),
        unsigned(guest_word(p, object + 0x50u)), unsigned(guest_word(p, object + 0x58u)), unsigned(guest_word(p, object + 0x5Cu)),
        unsigned(guest_word(p, object + 0x60u)), unsigned(guest_word(p, object + 0x68u)), unsigned(guest_word(p, object + 0x6Cu)),
        unsigned(guest_word(p, SCREAM_GATE)));
    if (!engine || engine == 0xDEADDEADu) return;
    const uint32_t work_base = guest_word(p, engine + 0x2DE0u), work_size = guest_word(p, engine + 0x2DE4u);
    std::printf("STARTUP_SCREAM_ENGINE t_ms=%llu flags=0x%08x work_base=0x%08x work_size=0x%x work_cur=0x%08x busy=%u e48=0x%x "
                "e1b4=0x%x e1e4=0x%x e2dc0=0x%08x cmd_base=0x%08x cmd_cur=0x%08x cmd_i44=%u cmd_i48=%u cmd_per=%u cmd_i54=%u e14=0x%08x\n",
        t_ms, unsigned(guest_word(p, engine + 0x2DDCu)), unsigned(work_base), unsigned(work_size),
        unsigned(guest_word(p, engine + 0x2DE8u)), unsigned(guest_byte(p, engine + 0x2DECu)), unsigned(guest_word(p, engine + 0x48u)),
        unsigned(guest_word(p, engine + 0x1B4u)), unsigned(guest_word(p, engine + 0x1E4u)), unsigned(guest_word(p, engine + 0x2DC0u)),
        unsigned(guest_word(p, engine + 0x2Cu)), unsigned(guest_word(p, engine + 0x30u)), unsigned(guest_word(p, engine + 0x44u)),
        unsigned(guest_word(p, engine + 0x48u)), unsigned(guest_word(p, engine + 0x4Cu)), unsigned(guest_word(p, engine + 0x54u)),
        unsigned(guest_word(p, engine + 0x14u)));
    // the mix work ring: does the SPU side produce anything at all, before it reaches the port?
    if (work_base && work_size && work_size <= SCREAM_WORK_SCAN_MAX) {
        const uint8_t *work = static_cast<const uint8_t *>(p.window(work_base, work_size));
        if (work) {
            audio_ring_scan scan{};
            audio_mix_scan_ring(work, 2u, work_size / audio_mix_block_bytes(2u), &scan);   // shape-free: counts samples
            std::printf("STARTUP_SCREAM_WORK t_ms=%llu base=0x%08x bytes=0x%x nonzero=%u nonfinite=%u max_abs=%.6f probe_samples=%u\n",
                t_ms, unsigned(work_base), unsigned(work_size), unsigned(scan.nonzero_samples), unsigned(scan.nonfinite_samples),
                double(float_from_bits(scan.max_abs_bits)), unsigned(scan.probe_samples));
        } else {
            std::printf("STARTUP_SCREAM_WORK t_ms=%llu base=0x%08x bytes=0x%x mapped=0\n", t_ms, unsigned(work_base), unsigned(work_size));
        }
    }
    // the command record the next block will use (one 0x80-byte record per output block)
    const uint32_t command = guest_word(p, engine + 0x30u) + (guest_word(p, engine + 0x44u) << 7);
    std::printf("STARTUP_SCREAM_COMMAND t_ms=%llu at=0x%08x ts_hi=0x%08x ts_lo=0x%08x w8=0x%08x out=0x%08x w10=0x%08x work=0x%08x "
                "flags=0x%08x prev_out=0x%08x\n",
        t_ms, unsigned(command), unsigned(guest_word(p, command)), unsigned(guest_word(p, command + 4u)),
        unsigned(guest_word(p, command + 8u)), unsigned(guest_word(p, command + 0xCu)), unsigned(guest_word(p, command + 0x10u)),
        unsigned(guest_word(p, command + 0x14u)), unsigned(guest_word(p, command + 0x18u)),
        unsigned(guest_word(p, command - SCREAM_COMMAND_BYTES + 0xCu)));
    for (uint32_t n = 0; n < SCREAM_LISTS; ++n) {
        const uint32_t list = engine + SCREAM_LIST_BASE + n * SCREAM_LIST_STRIDE;
        uint32_t node = guest_word(p, list), count = 0;
        uint32_t first[3] = {0, 0, 0};
        while (node && node != 0xDEADDEADu && count < SCREAM_LIST_WALK_MAX) {
            if (count < 3u) first[count] = node;
            ++count;
            node = guest_word(p, node + 0x04u);
        }
        std::printf("STARTUP_SCREAM_LIST t_ms=%llu n=%u at=0x%08x enabled=%u count_field=%u walked=%u", t_ms, unsigned(n),
            unsigned(list), unsigned(guest_byte(p, list + 0x1Cu)), unsigned(guest_word(p, list + 0x24u)), unsigned(count));
        for (uint32_t k = 0; k < 3u && first[k]; ++k)
            std::printf(" v%u=0x%08x[w0=0x%08x w8=0x%08x w4c=0x%08x w5c=0x%08x w80=0x%08x]", unsigned(k), unsigned(first[k]),
                unsigned(guest_word(p, first[k])), unsigned(guest_word(p, first[k] + 8u)), unsigned(guest_word(p, first[k] + 0x4Cu)),
                unsigned(guest_word(p, first[k] + 0x5Cu)), unsigned(guest_word(p, first[k] + 0x80u)));
        std::printf("\n");
        // Decision 134: the rack units themselves. func_005541A0 stamps the tick at +0x5C and writes +0x64 (0 when the unit
        // is skipped, else func_00553AB8 fills it); func_00554768 walks voices while +0x98 < +0x64. Reading: +0x64 is most
        // likely the unit's voice count for the tick (unconfirmed). The words +0x40..+0xA0 are printed raw for the rest.
        node = guest_word(p, list);
        for (uint32_t k = 0; k < SCREAM_UNIT_DUMP_MAX && node && node != 0xDEADDEADu; ++k) {
            std::printf("STARTUP_SCREAM_UNIT t_ms=%llu n=%u k=%u at=0x%08x w64=%u w98=%u w40..a0=", t_ms, unsigned(n), unsigned(k),
                unsigned(node), unsigned(guest_word(p, node + 0x64u)), unsigned(guest_word(p, node + 0x98u)));
            for (uint32_t off = SCREAM_UNIT_DUMP_FIRST; off <= SCREAM_UNIT_DUMP_LAST; off += 4u)
                std::printf("%08x%s", unsigned(guest_word(p, node + off)), off < SCREAM_UNIT_DUMP_LAST ? "," : "\n");
            node = guest_word(p, node + 0x04u);
        }
    }
}

// Decision 132: what the guest left in the audio window, read before the reader takes (and clears) any block.
void print_diagnostics(const Pump &p, uint64_t now_us, uint32_t initialized, const audio_port *ports,
                       const uint64_t *calls, const audio_port_layout &layout)
{
    const unsigned long long t_ms = static_cast<unsigned long long>(now_us / 1000u);
    for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i) {
        const audio_port &port = ports[i];
        if (!port.open) continue;
        const uint32_t block_bytes = audio_mix_block_bytes(port.channels);
        const uint32_t ring_bytes = block_bytes * port.blocks;
        const uint32_t base = layout.buffer_base + i * layout.buffer_stride;
        audio_ring_scan scan{};
        const uint8_t *ring = ring_bytes ? static_cast<const uint8_t *>(p.window(base, ring_bytes)) : nullptr;
        if (ring) audio_mix_scan_ring(ring, port.channels, port.blocks, &scan);
        const uint8_t *index = static_cast<const uint8_t *>(p.window(layout.index_base + INDEX_WORD_BYTES * i, INDEX_WORD_BYTES));
        std::printf("STARTUP_AUDIO_RING t_ms=%llu port=%u running=%u channels=%u blocks=%u counter=%llu index_word=%lld "
                    "addr=0x%08x bytes=0x%x mapped=%d nonzero=%u nonfinite=%u slots=0x%08x max_abs=%.6f stamped_blocks=%u "
                    "heard=%llu written_silent=%llu untouched=%llu level_bits=0x%08x start_us=%llu\n",
            t_ms, unsigned(i), unsigned(port.running), unsigned(port.channels), unsigned(port.blocks),
            static_cast<unsigned long long>(port.counter), index ? static_cast<long long>(load_be64(index)) : -1LL,
            unsigned(base), unsigned(ring_bytes), int(ring != nullptr), unsigned(scan.nonzero_samples),
            unsigned(scan.nonfinite_samples), unsigned(scan.slot_mask), double(float_from_bits(scan.max_abs_bits)),
            unsigned(scan.probe_blocks), static_cast<unsigned long long>(p.runs[i].heard),
            static_cast<unsigned long long>(p.runs[i].written_silent), static_cast<unsigned long long>(p.runs[i].untouched),
            unsigned(port.level_bits),
            static_cast<unsigned long long>(port.start_us));
    }
    uint32_t index_count = 0, index_first = UINT32_MAX, outside_count = 0, outside_first = UINT32_MAX, unmapped = 0;
    const uint32_t index_words_end = layout.index_base + INDEX_WORD_BYTES * AUDIO_PORT_COUNT;
    if (layout.buffer_base > index_words_end)
        count_window_words(p, index_words_end, layout.buffer_base - index_words_end, index_count, index_first, unmapped);
    for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i) {
        const uint32_t base = layout.buffer_base + i * layout.buffer_stride;
        uint32_t ring_bytes = ports[i].open ? audio_mix_block_bytes(ports[i].channels) * ports[i].blocks : 0u;
        if (ring_bytes > layout.buffer_stride) ring_bytes = layout.buffer_stride;
        count_window_words(p, base + ring_bytes, layout.buffer_stride - ring_bytes, outside_count, outside_first, unmapped);
    }
    std::printf("STARTUP_AUDIO_WINDOW t_ms=%llu index_extra_nonzero=%u index_first=0x%08x outside_rings_nonzero=%u "
                "outside_first=0x%08x unmapped_ranges=%u\n",
        t_ms, unsigned(index_count), unsigned(index_first), unsigned(outside_count), unsigned(outside_first), unsigned(unmapped));
    std::printf("STARTUP_AUDIO_CALLS t_ms=%llu initialized=%u availability=%llu devices=%llu device_info=%llu state=%llu "
                "configure=%llu init=%llu quit=%llu open=%llu start=%llu stop=%llu close=%llu config=%llu block_tag=%llu\n",
        t_ms, unsigned(initialized),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_OUT_SOUND_AVAILABILITY]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_OUT_NUMBER_OF_DEVICE]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_OUT_DEVICE_INFO]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_OUT_STATE]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_OUT_CONFIGURE]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_INIT]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_QUIT]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_PORT_OPEN]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_PORT_START]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_PORT_STOP]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_PORT_CLOSE]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_GET_PORT_CONFIG]),
        static_cast<unsigned long long>(calls[AUDIO_IMPORT_GET_PORT_BLOCK_TAG]));
#ifndef STARTUP_AUDIO_NO_SCREAM_PROBE
    print_scream_probe(p, t_ms);
#endif
    std::fflush(stdout);
}

// Picks up a new run of `port`: PortStart resets the block counter and stamps a new start time (PortClose clears the
// whole port, so a call count would repeat). The counter never goes back within a run, so a smaller one is a new run
// too. A run found late (the counter already past the catch-up limit) is joined at "now", not replayed.
bool follow_run(PortRun &run, uint32_t number, const audio_port &port)
{
    if (run.active && port.start_us == run.start_us && port.counter >= run.consumed) return false;
    run.active = true;
    run.start_us = port.start_us;
    run.consumed = port.counter > AUDIO_RING_MAX_CATCHUP ? port.counter : 0;
    run.level_q15 = audio_mix_level_q15(port.level_bits);
    std::printf("STARTUP_AUDIO_OUT_PORT port=%u channels=%u blocks=%u level_q15=%u start_us=%llu\n", unsigned(number),
        unsigned(port.channels), unsigned(port.blocks), unsigned(run.level_q15), static_cast<unsigned long long>(port.start_us));
    std::fflush(stdout);
    return true;
}

void pump(void *opaque)
{
    Pump &p = *static_cast<Pump *>(opaque);
    uint64_t now_us = 0;
    if (p.clock->now_microseconds(p.clock->context, &now_us) != PS3_CLOCK_OK) return;
    startup_audio_tick(*p.process, *p.memory, now_us);

    audio_port ports[AUDIO_PORT_COUNT];
    uint64_t calls[AUDIO_IMPORT_COUNT];
    uint32_t initialized;
    acquire(p.process->lock);
    initialized = p.process->service.initialized;
    std::memcpy(ports, p.process->service.ports, sizeof ports);
    std::memcpy(calls, p.process->service.calls, sizeof calls);
    p.process->lock.clear(std::memory_order_release);
    const audio_port_layout layout = p.process->service.layout;   // set once at init, never changes

    bool new_run = false;
    int master = -1;
    for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i) {
        PortRun &run = p.runs[i];
        if (!initialized || !ports[i].open || !ports[i].running || !audio_mix_block_bytes(ports[i].channels) ||
            !ports[i].blocks) {
            run.active = false;
            continue;
        }
        if (follow_run(run, i, ports[i])) new_run = true;
        if (master < 0) master = int(i);
    }
    if (initialized && (new_run || now_us >= p.next_diag_us)) {
        print_diagnostics(p, now_us, initialized, ports, calls, layout);
        p.next_diag_us = now_us + DIAG_PERIOD_US;
    }
    if (master < 0) return;

    audio_ring_plan plans[AUDIO_PORT_COUNT]{};
    for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i) {
        if (!p.runs[i].active) continue;
        audio_ring_plan_blocks(p.runs[i].consumed, ports[i].counter, ports[i].blocks, &plans[i]);
        if (plans[i].skipped) audio_out_horizon_note_skipped(plans[i].skipped);
    }
    const uint32_t out_blocks = plans[master].count;
    for (uint32_t k = 0; k < out_blocks; ++k) {
        int16_t sum[AUDIO_MIX_OUT_SAMPLES];
        std::memset(sum, 0, sizeof sum);
        bool heard_any = false;
        for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i) {
            if (!p.runs[i].active || k >= plans[i].count) continue;
            const audio_port &port = ports[i];
            const uint32_t block_bytes = audio_mix_block_bytes(port.channels);
            const uint32_t slot = uint32_t((plans[i].first + k) % port.blocks);
            uint8_t *block = static_cast<uint8_t *>(
                p.window(layout.buffer_base + i * layout.buffer_stride + slot * block_bytes, block_bytes));
            if (!block) continue;
            PortRun &run = p.runs[i];
            if (audio_mix_block_is_stamped(block, port.channels, AUDIO_MIX_PROBE_WORD)) { ++run.untouched; continue; }
            int16_t stereo[AUDIO_MIX_OUT_SAMPLES];
            // a block the game overwrote only in part with zeros still holds stamp words: silence, not sound
            const int held_sound = audio_mix_block_is_silent_or_stamped(block, port.channels, AUDIO_MIX_PROBE_WORD)
                ? 0 : audio_mix_consume_block(block, port.channels, run.level_q15, stereo);
            audio_mix_stamp_block(block, port.channels, AUDIO_MIX_PROBE_WORD);
            if (held_sound != 1) { ++run.written_silent; continue; }
            audio_mix_add_stereo(sum, stereo);
            heard_any = true;
            ++run.heard;
        }
        audio_out_horizon_submit(sum, !heard_any);
    }
    for (uint32_t i = 0; i < AUDIO_PORT_COUNT; ++i)
        if (p.runs[i].active)
            p.runs[i].consumed = plans[i].first + (plans[i].count < out_blocks ? plans[i].count : out_blocks);
}
}   // namespace

int32_t startup_audio_out_start(StartupAudioProcess &process, const ps3_guest_memory &memory, const ps3_clock &clock,
                                startup_audio_window_fn window)
{
    g_pump = Pump();
    g_pump.process = &process;
    g_pump.memory = &memory;
    g_pump.clock = &clock;
    g_pump.window = window;
    return audio_out_horizon_start(pump, &g_pump);
}

void startup_audio_out_stop(void)
{
    audio_out_horizon_stop();
}
