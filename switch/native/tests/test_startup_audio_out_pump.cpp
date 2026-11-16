/* Host test of the output pump (native/adapters/startup_audio_out.cpp) against the REAL audio service, ring plan and mixer.
 * The pump source is included textually; its two collaborators that need the console are replaced here:
 *   - startup_execution.h  -> audio_out_host/stub_startup_execution.h (StartupAudioProcess and the real clock / guest-memory types),
 *     <switch.h>           -> audio_out_host/switch.h
 *   - audio_out_horizon.c  -> the recorder below (what the pump hands to the device, and when).
 * Guest memory is a byte vector with the real window layout (read indexes at +0, port buffers at +0x10000, stride 0x40000).
 *
 *   see native/tests/run_audio_host_tests.sh
 */
#include "audio_out_host/stub_startup_execution.h"
#include "../adapters/startup_audio_out.cpp"
#include "../domain/audio_mix.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

static unsigned g_checks, g_failures;
#define CHECK(condition) do { ++g_checks; if (!(condition)) { ++g_failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_EQ(actual, expected) do { const long long a_ = (long long)(actual), e_ = (long long)(expected); \
    ++g_checks; if (a_ != e_) { ++g_failures; \
    std::printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); } } while (0)

// ---- the recorder that stands in for the device ----
struct Submit { int16_t left, right; int silent; };
static std::vector<Submit> g_submits;
static uint64_t g_skipped;
static audio_out_horizon_pump_fn g_pump_fn;
static void *g_pump_ctx;
extern "C" {
int32_t audio_out_horizon_start(audio_out_horizon_pump_fn pump, void *context) { g_pump_fn = pump; g_pump_ctx = context; return 0; }
void audio_out_horizon_stop(void) {}
void audio_out_horizon_submit(const int16_t *stereo, int silent) { g_submits.push_back({stereo[0], stereo[1], silent}); }
void audio_out_horizon_note_skipped(uint64_t blocks) { g_skipped += blocks; }
}

// ---- guest memory with the composition's window layout ----
static const uint32_t INDEX_BASE = 0x0000, BUFFER_BASE = 0x10000, STRIDE = 0x40000;
// big enough for the SCREAM probe's fixed guest addresses (driver object 0x009EB000, its TOC slot, the engine)
static std::vector<uint8_t> g_memory(0x00A00000u);
static bool g_window_fails;
static ps3_guest_memory_status mem_read(void *, uint32_t a, void *out, size_t n)
{
    if (uint64_t(a) + n > g_memory.size()) return PS3_GUEST_MEMORY_INVALID_RANGE;
    std::memcpy(out, &g_memory[a], n);
    return PS3_GUEST_MEMORY_OK;
}
static ps3_guest_memory_status mem_write(void *, uint32_t a, const void *in, size_t n)
{
    if (uint64_t(a) + n > g_memory.size()) return PS3_GUEST_MEMORY_INVALID_RANGE;
    std::memcpy(&g_memory[a], in, n);
    return PS3_GUEST_MEMORY_OK;
}
static void *window(uint32_t address, uint32_t length)
{
    if (g_window_fails || uint64_t(address) + length > g_memory.size()) return nullptr;
    return &g_memory[address];
}
static const ps3_guest_memory g_guest{nullptr, nullptr, nullptr, mem_read, mem_write, nullptr};

// ---- the tick the ticker thread makes: the real service's block clock and its read-index stores ----
static unsigned g_tick_calls;
void startup_audio_tick(StartupAudioProcess &process, const ps3_guest_memory &memory, uint64_t now_us)
{
    while (process.lock.test_and_set(std::memory_order_acquire)) {}
    uint64_t indexes[AUDIO_PORT_COUNT]{};
    const uint32_t changed = audio_tick(&process.service, now_us, indexes, AUDIO_PORT_COUNT);
    for (uint32_t port = 0; port < AUDIO_PORT_COUNT; ++port)
        if (changed & (1u << port)) {
            unsigned char bytes[8];
            for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(indexes[port] >> (56 - 8 * i));
            memory.write(memory.context, process.service.layout.index_base + 8u * port, bytes, 8);
        }
    process.lock.clear(std::memory_order_release);
    ++g_tick_calls;
}

static uint64_t g_now_us;
static ps3_clock_status g_clock_status = PS3_CLOCK_OK;
static ps3_clock_status clock_now(void *, uint64_t *out)
{
    if (g_clock_status != PS3_CLOCK_OK) return g_clock_status;
    *out = g_now_us;
    return PS3_CLOCK_OK;
}
static const ps3_clock g_clock{nullptr, clock_now};

static StartupAudioProcess g_process;
static uint32_t float_bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

static uint8_t *ring_block(uint32_t port, uint32_t slot, uint32_t channels)
{
    return &g_memory[BUFFER_BASE + port * STRIDE + slot * audio_mix_block_bytes(channels)];
}
static void put_be(uint8_t *p, float v)
{
    const uint32_t b = float_bits(v);
    p[0] = uint8_t(b >> 24); p[1] = uint8_t(b >> 16); p[2] = uint8_t(b >> 8); p[3] = uint8_t(b);
}
/* the game writing one block: `value` in channel `channel` of every frame */
static void game_writes(uint32_t port, uint32_t slot, uint32_t channels, uint32_t channel, float value)
{
    uint8_t *block = ring_block(port, slot, channels);
    for (uint32_t frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) put_be(block + (frame * channels + channel) * 4u, value);
}
static void zero_block(uint32_t port, uint32_t slot, uint32_t channels)
{
    std::memset(ring_block(port, slot, channels), 0, audio_mix_block_bytes(channels));
}
/* taken by the reader: every sample holds the write probe's stamp */
static bool block_is_taken(uint32_t port, uint32_t slot, uint32_t channels)
{
    return audio_mix_block_is_stamped(ring_block(port, slot, channels), channels, AUDIO_MIX_PROBE_WORD) == 1;
}
static bool block_is_zero(uint32_t port, uint32_t slot, uint32_t channels)
{
    const uint8_t *block = ring_block(port, slot, channels);
    for (uint32_t i = 0; i < audio_mix_block_bytes(channels); ++i) if (block[i]) return false;
    return true;
}
/* what the mixer makes of the block as the game left it (before the pump clears it) */
static Submit expected(uint32_t port, uint32_t slot, uint32_t channels, uint32_t level_q15)
{
    int16_t out[AUDIO_MIX_OUT_SAMPLES];
    audio_mix_block_to_stereo(ring_block(port, slot, channels), channels, level_q15, out);
    return {out[0], out[1], 0};
}
static void run_pump(uint64_t now_us)
{
    g_now_us = now_us;
    g_submits.clear();
    g_pump_fn(g_pump_ctx);
}
/* run_pump with stdout captured: the diagnostic lines the pump printed */
static std::string run_pump_captured(uint64_t now_us)
{
    std::fflush(stdout);
    FILE *capture = std::tmpfile();
    if (!capture) return std::string();
    const int saved = dup(1);
    dup2(fileno(capture), 1);
    run_pump(now_us);
    std::fflush(stdout);
    dup2(saved, 1);
    close(saved);
    std::string text;
    std::rewind(capture);
    char buffer[512];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, capture)) > 0) text.append(buffer, n);
    std::fclose(capture);
    return text;
}
static bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }
/* the clock value at which a port started at `start_us` has counter == `blocks` (48000/256 blocks per second) */
static uint64_t at_block(uint64_t start_us, uint64_t blocks) { return start_us + (blocks * 16000u + 2u) / 3u; }

int main()
{
    audio_service *svc = &g_process.service;
    const audio_port_layout layout{INDEX_BASE, BUFFER_BASE, STRIDE};
    audio_service_init(svc, &layout);
    CHECK_EQ(startup_audio_out_start(g_process, g_guest, g_clock, window), 0);
    CHECK(g_pump_fn != nullptr);

    // nothing initialised, nothing open: the pump ticks the service and hands on nothing
    run_pump(1000);
    CHECK_EQ(g_submits.size(), 0);
    CHECK_EQ(g_tick_calls, 1);
    // a clock that fails: no tick, no blocks, no crash
    g_clock_status = PS3_CLOCK_PLATFORM_FAILURE;
    run_pump(2000);
    CHECK_EQ(g_tick_calls, 1);
    g_clock_status = PS3_CLOCK_OK;

    CHECK_EQ(audio_init(svc), 0);
    uint32_t port = 99;
    CHECK_EQ(audio_port_open(svc, 8, 8, 0, float_bits(1.0f), &port), 0);
    CHECK_EQ(port, 0);
    run_pump(3000);
    CHECK_EQ(g_submits.size(), 0);              // open but not started: not playing

    // ---- run 1: 8 channels, 8 blocks, started at t0 ----
    const uint64_t t0 = 1000000;
    CHECK_EQ(audio_port_start(svc, 0, t0), 0);
    for (uint32_t slot = 0; slot < 8; ++slot) game_writes(0, slot, 8, 2, float(slot + 1) / 16.0f);   // centre, a level per slot
    Submit want[8];
    for (uint32_t slot = 0; slot < 8; ++slot) want[slot] = expected(0, slot, 8, AUDIO_MIX_LEVEL_UNITY);
    CHECK(want[0].left != want[1].left && want[0].left != 0);

    // counter 0: no block has finished. A new run: the diagnostics see the whole ring the game wrote, before any read
    const std::string diag = run_pump_captured(t0);
    CHECK_EQ(g_submits.size(), 0);
    CHECK(has(diag, "STARTUP_AUDIO_OUT_PORT port=0 channels=8 blocks=8"));
    CHECK(has(diag, "STARTUP_AUDIO_RING t_ms=1000 port=0 running=1 channels=8 blocks=8 counter=0 index_word=0 addr=0x00010000 "
                    "bytes=0x10000 mapped=1 nonzero=2048 nonfinite=0 slots=0x000000ff max_abs=0.500000 stamped_blocks=0 heard=0 "
                    "written_silent=0 untouched=0"));
    CHECK(has(diag, "STARTUP_AUDIO_WINDOW t_ms=1000 index_extra_nonzero=0 index_first=0xffffffff outside_rings_nonzero=0"));
    CHECK(has(diag, "STARTUP_AUDIO_CALLS t_ms=1000 initialized=1") && has(diag, " init=1 quit=0 open=1 start=1 "));
    // the next diagnostics are 10 s later, not at every pump
    CHECK(!has(run_pump_captured(t0), "STARTUP_AUDIO_RING"));
    run_pump(at_block(t0, 1) - 1);              // a hair before the first block ends
    CHECK_EQ(g_submits.size(), 0);
    run_pump(at_block(t0, 1));                  // block 0 has been played
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) {
        CHECK_EQ(g_submits[0].left, want[0].left); CHECK_EQ(g_submits[0].right, want[0].right); CHECK_EQ(g_submits[0].silent, 0);
    }
    CHECK(block_is_taken(0, 0, 8));             // consumed: cleared (stamped), the way the firmware's reader does
    CHECK(!block_is_zero(0, 1, 8));             // the next one is still the game's
    run_pump(at_block(t0, 1));                  // the same instant again: nothing new
    CHECK_EQ(g_submits.size(), 0);
    run_pump(at_block(t0, 3));                  // blocks 1 and 2, in order
    CHECK_EQ(g_submits.size(), 2);
    if (g_submits.size() == 2) {
        CHECK_EQ(g_submits[0].left, want[1].left); CHECK_EQ(g_submits[1].left, want[2].left);
    }
    CHECK(block_is_taken(0, 1, 8) && block_is_taken(0, 2, 8));
    // the read index the guest sees is counter % blocks, stored by the same tick
    CHECK_EQ(g_memory[INDEX_BASE + 7], 3);
    // the game refills slot 0 for the next pass (block 8), then the clock reaches counter 9: blocks 3..8 = slots 3,4,5,6,7,0
    game_writes(0, 0, 8, 2, 0.75f);
    const Submit refilled = expected(0, 0, 8, AUDIO_MIX_LEVEL_UNITY);
    run_pump(at_block(t0, 9));
    CHECK_EQ(g_submits.size(), 6);
    if (g_submits.size() == 6) {
        for (uint32_t i = 0; i < 5; ++i) CHECK_EQ(g_submits[i].left, want[3 + i].left);
        CHECK_EQ(g_submits[5].left, refilled.left); CHECK_EQ(g_submits[5].silent, 0);
    }
    // blocks 9 and 10 are slots 1 and 2, which nobody refilled: they were cleared, so they arrive as silence
    run_pump(at_block(t0, 11));
    CHECK_EQ(g_submits.size(), 2);
    if (g_submits.size() == 2) {
        CHECK_EQ(g_submits[0].silent, 1); CHECK_EQ(g_submits[0].left, 0); CHECK_EQ(g_submits[0].right, 0); CHECK_EQ(g_submits[1].silent, 1);
    }
    // the write probe: those two still held the reader's stamp, so nothing wrote them
    CHECK_EQ(g_pump.runs[0].untouched, 2); CHECK_EQ(g_pump.runs[0].written_silent, 0); CHECK_EQ(g_pump.runs[0].heard, 9);
    // the game rewrites block 11 (slot 3) with zeros: silence, but counted as written, and stamped again
    zero_block(0, 3, 8);
    run_pump(at_block(t0, 12));
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) CHECK_EQ(g_submits[0].silent, 1);
    CHECK_EQ(g_pump.runs[0].written_silent, 1); CHECK_EQ(g_pump.runs[0].untouched, 2);
    CHECK(block_is_taken(0, 3, 8));
    // a long stall: counter 12 -> 40. 28 blocks are due, 6 are kept (blocks 34..39 = slots 2..7), 22 given up
    for (uint32_t slot = 0; slot < 8; ++slot) game_writes(0, slot, 8, 0, float(slot + 1) / 32.0f);   // front left, a level per slot
    Submit stall[8];
    for (uint32_t slot = 0; slot < 8; ++slot) stall[slot] = expected(0, slot, 8, AUDIO_MIX_LEVEL_UNITY);
    g_skipped = 0;
    run_pump(at_block(t0, 40));
    CHECK_EQ(g_submits.size(), 6);
    CHECK_EQ(g_skipped, 22);
    if (g_submits.size() == 6)
        for (uint32_t i = 0; i < 6; ++i) CHECK_EQ(g_submits[i].left, stall[2 + i].left);
    CHECK(block_is_taken(0, 2, 8) && block_is_taken(0, 7, 8));
    CHECK(!block_is_zero(0, 0, 8) && !block_is_zero(0, 1, 8));   // slots 0 and 1 (blocks 32, 33) were given up, not played, not cleared

    // ---- run 2: stop and start again: a new run counts from block 0 ----
    CHECK_EQ(audio_port_stop(svc, 0), 0);
    run_pump(at_block(t0, 60));
    CHECK_EQ(g_submits.size(), 0);              // stopped
    const uint64_t t1 = at_block(t0, 70);
    CHECK_EQ(audio_port_start(svc, 0, t1), 0);
    run_pump(t1);
    CHECK_EQ(g_submits.size(), 0);
    zero_block(0, 0, 8);                        // slots 0 and 1 still hold what the stall left (given up, never played)
    game_writes(0, 0, 8, 1, 0.5f);              // front right
    run_pump(at_block(t1, 2));
    CHECK_EQ(g_submits.size(), 2);              // blocks 0 and 1 of the new run: slots 0, 1
    if (g_submits.size() == 2) { CHECK_EQ(g_submits[0].left, 0); CHECK(g_submits[0].right != 0); CHECK_EQ(g_submits[0].silent, 0); }

    // ---- run 3: close, reopen as 6 channels, 16 blocks, port level 0.5; the new start time is a new run ----
    CHECK_EQ(audio_port_close(svc, 0), 0);
    run_pump(at_block(t1, 5));
    CHECK_EQ(g_submits.size(), 0);
    CHECK_EQ(audio_port_open(svc, 6, 16, 0, float_bits(0.5f), &port), 0);
    CHECK_EQ(port, 0);
    const uint64_t t2 = at_block(t1, 9);
    CHECK_EQ(audio_port_start(svc, 0, t2), 0);
    game_writes(0, 0, 6, 2, 0.5f);              // centre
    const Submit half = expected(0, 0, 6, 16384);
    run_pump(at_block(t2, 1));
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) { CHECK_EQ(g_submits[0].left, half.left); CHECK_EQ(g_submits[0].right, half.right); CHECK(half.left != 0); }
    CHECK(block_is_taken(0, 0, 6));
    // a ring of 16: block 16 is slot 0 again
    for (uint32_t slot = 1; slot < 16; ++slot) game_writes(0, slot, 6, 2, 0.25f);
    game_writes(0, 0, 6, 2, 0.125f);
    const Submit last = expected(0, 0, 6, 16384);
    run_pump(at_block(t2, 17));                 // 16 blocks behind: 6 kept = blocks 11..16 = slots 11..15, 0
    CHECK_EQ(g_submits.size(), 6);
    if (g_submits.size() == 6) { CHECK_EQ(g_submits[5].left, last.left); CHECK(g_submits[5].left != 0); }

    // ---- an unset level (0.0) is unity, not mute ----
    CHECK_EQ(audio_port_close(svc, 0), 0);
    CHECK_EQ(audio_port_open(svc, 2, 8, 0, float_bits(0.0f), &port), 0);
    const uint64_t t3 = at_block(t2, 40);
    CHECK_EQ(audio_port_start(svc, 0, t3), 0);
    game_writes(0, 0, 2, 0, 1.0f);
    run_pump(at_block(t3, 1));
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) CHECK_EQ(g_submits[0].left, 32767);

    // ---- two running ports (Decision 132): both are played, added together; the lowest sets the pace ----
    uint32_t second = 99;
    CHECK_EQ(audio_port_open(svc, 2, 8, 0, float_bits(1.0f), &second), 0);
    CHECK_EQ(second, 1);
    const uint64_t t4 = at_block(t3, 10);
    CHECK_EQ(audio_port_start(svc, 1, t4), 0);
    game_writes(1, 0, 2, 1, 0.5f);
    game_writes(0, 1, 2, 0, 0.5f);              // port 0's block 1
    run_pump(at_block(t3, 2));                  // port 0: blocks 1 (block 0 went with the previous pump) -> slot 1
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) CHECK_EQ(g_submits[0].left, 16384);
    CHECK(!block_is_zero(1, 0, 2));             // port 1 has not started yet (t4 is later): nothing of it is due
    // at t4 + 1 block, port 0 (counter 11) has blocks 2..10 due, 6 kept (5..10 = slots 5,6,7,0,1,2), and port 1 its
    // block 0: port 1's block is mixed into the first block out. The 2-channel ring shares bytes with the earlier
    // 6- and 8-channel runs, so it is cleared first.
    for (uint32_t slot = 0; slot < 8; ++slot) zero_block(0, slot, 2);
    game_writes(0, 5, 2, 0, 0.25f);             // port 0's block 5 -> slot 5, front left
    g_skipped = 0;
    run_pump(at_block(t4, 1));
    CHECK_EQ(g_submits.size(), 6);
    CHECK_EQ(g_skipped, 3);
    if (g_submits.size() == 6) {
        CHECK_EQ(g_submits[0].left, 8192); CHECK_EQ(g_submits[0].right, 16384); CHECK_EQ(g_submits[0].silent, 0);
        for (size_t i = 1; i < 6; ++i) CHECK_EQ(g_submits[i].silent, 1);
    }
    CHECK(block_is_taken(1, 0, 2) && block_is_taken(0, 5, 2));
    // port 0 blocks 11, 12 (slots 3, 4) with port 1 blocks 1, 2 (slots 1, 2); both at full scale in the same channel
    // of the first pair: the sum is held at the int16 range, not wrapped
    game_writes(0, 3, 2, 0, 1.0f);
    game_writes(1, 1, 2, 0, 1.0f);
    run_pump(at_block(t3, 13));
    CHECK_EQ(g_submits.size(), 2);
    if (g_submits.size() == 2) { CHECK_EQ(g_submits[0].left, 32767); CHECK_EQ(g_submits[0].silent, 0); CHECK_EQ(g_submits[1].silent, 1); }
    CHECK(block_is_taken(0, 3, 2) && block_is_taken(1, 1, 2));
    CHECK_EQ(audio_port_close(svc, 0), 0);
    game_writes(1, 3, 2, 1, 0.5f);
    run_pump(at_block(t4, 4));                  // port 1 alone now: the same run goes on, block 3 = slot 3
    CHECK_EQ(g_submits.size(), 1);
    if (g_submits.size() == 1) { CHECK_EQ(g_submits[0].left, 0); CHECK_EQ(g_submits[0].right, 16384); }
    CHECK(block_is_taken(1, 3, 2));

    // ---- a window that cannot be mapped: the block is silence, the run goes on ----
    g_window_fails = true;
    run_pump(at_block(t4, 6));
    CHECK_EQ(g_submits.size(), 2);
    if (g_submits.size() == 2) { CHECK_EQ(g_submits[0].silent, 1); CHECK_EQ(g_submits[1].silent, 1); }
    g_window_fails = false;

    // ---- a run joined late is joined at "now", not replayed: restart at a time when the counter is already 500 ----
    CHECK_EQ(audio_port_stop(svc, 1), 0);
    run_pump(at_block(t4, 4));
    CHECK_EQ(audio_port_start(svc, 1, at_block(t4, 5)), 0);        // counter 0 at this instant
    // the pump first sees it 500 blocks later (a thread that was not scheduled for 2.7 s)
    game_writes(1, 4, 2, 0, 0.5f);
    g_skipped = 0;
    run_pump(at_block(at_block(t4, 5), 500));
    CHECK_EQ(g_submits.size(), 0);              // joined at counter 500: nothing replayed, nothing due yet
    CHECK_EQ(g_skipped, 0);
    run_pump(at_block(at_block(t4, 5), 501));
    CHECK_EQ(g_submits.size(), 1);              // block 500 = slot 4
    if (g_submits.size() == 1) CHECK_EQ(g_submits[0].left, 16384);

    // ---- close and reopen the SAME port number with no pump in between, the new run already past where the old one got:
    //      only its start time tells them apart (the counter is not smaller, the port number is the same) ----
    uint32_t dummy = 99;
    CHECK_EQ(audio_port_open(svc, 2, 8, 0, float_bits(1.0f), &dummy), 0);   // takes port 0, never started: not played
    CHECK_EQ(dummy, 0);
    CHECK_EQ(audio_port_close(svc, 1), 0);
    uint32_t again = 99;
    CHECK_EQ(audio_port_open(svc, 2, 8, 0, float_bits(1.0f), &again), 0);
    CHECK_EQ(again, 1);
    const uint64_t t6 = at_block(t4, 800);
    CHECK_EQ(audio_port_start(svc, 1, t6), 0);
    run_pump(at_block(t6, 503));                // counter 503 > the 501 handed on by the old run: a continuing run would hand on 2 blocks
    CHECK_EQ(g_submits.size(), 0);              // a new run joined late: joined at "now"
    run_pump(at_block(t6, 504));
    CHECK_EQ(g_submits.size(), 1);
    // restart with the very same start time (a clock value the service saw twice): the counter going back says it is a new run
    CHECK_EQ(audio_port_stop(svc, 1), 0);
    CHECK_EQ(audio_port_start(svc, 1, t6), 0);
    run_pump(at_block(t6, 2));
    CHECK_EQ(g_submits.size(), 2);

    // ---- 10 s on, the diagnostics again: a stray word past the read indexes and one outside every open ring (port 0 is
    //      open as 2 channels x 8 blocks = 0x4000 bytes) are reported with their addresses ----
    std::memset(&g_memory[BUFFER_BASE], 0, g_memory.size() - BUFFER_BASE);
    g_memory[INDEX_BASE + 0x100] = 1;
    g_memory[BUFFER_BASE + 0x10000 + 8] = 1;
    const std::string later = run_pump_captured(at_block(t6, 2) + 20000000u);
    CHECK(has(later, "STARTUP_AUDIO_RING"));
    CHECK(has(later, "index_extra_nonzero=1 index_first=0x00000100 outside_rings_nonzero=1 outside_first=0x00020008 unmapped_ranges=0"));
    g_memory[INDEX_BASE + 0x100] = 0;
    g_memory[BUFFER_BASE + 0x10000 + 8] = 0;

    // ---- the SCREAM probe (Decision 133): the brb driver object, its engine, a voice list, the work ring, a command ----
    {
        auto put32 = [](uint32_t a, uint32_t v) { for (int i = 0; i < 4; ++i) g_memory[a + i] = uint8_t(v >> (24 - 8 * i)); };
        const uint32_t obj = 0x009EB000u, engine = obj - 0x180u, work = 0x00900000u, cmd = 0x00910000u;
        put32(0x0077160Cu, obj);
        put32(obj + 0x04u, 0x00783F18u); put32(0x00783F18u, 0x00551E48u);
        put32(obj + 0x0Cu, engine); put32(obj + 0x58u, 0); put32(obj + 0x5Cu, 0x0e210000u); put32(obj + 0x6Cu, 8);
        put32(0x009EDCB0u, 1);
        put32(engine + 0x2DDCu, 3); put32(engine + 0x2DE0u, work); put32(engine + 0x2DE4u, 0x4000u);
        put32(engine + 0x30u, cmd); put32(engine + 0x44u, 2); put32(cmd + 2u * 0x80u + 0xCu, 0x0e212000u);
        put32(work + 0x100u, float_bits(0.25f));
        const uint32_t list1 = engine + 0x1580u + 0x230u;
        put32(list1, 0x00920000u); put32(0x00920000u + 4u, 0x00920100u); put32(0x00920100u + 4u, 0);
        put32(0x00920000u + 0x80u, 0x2u);
        g_memory[list1 + 0x1Cu] = 1;
        const std::string scream = run_pump_captured(at_block(t6, 2) + 40000000u);
        CHECK(has(scream, "STARTUP_SCREAM t_ms="));
        CHECK(has(scream, "driver=0x009eb000 callback_desc=0x00783f18 callback_code=0x00551e48 engine=0x009eae80"));
        CHECK(has(scream, "gate=0x00000001"));
        CHECK(has(scream, "STARTUP_SCREAM_ENGINE") && has(scream, "flags=0x00000003 work_base=0x00900000 work_size=0x4000"));
        CHECK(has(scream, "STARTUP_SCREAM_WORK") && has(scream, "nonzero=1 nonfinite=0 max_abs=0.250000"));
        CHECK(has(scream, "STARTUP_SCREAM_COMMAND") && has(scream, "at=0x00910100") && has(scream, "out=0x0e212000"));
        CHECK(has(scream, "STARTUP_SCREAM_LIST t_ms=") && has(scream, "n=1 at=0x009ec630 enabled=1 count_field=0 walked=2 v0=0x00920000"));
        CHECK(has(scream, "w80=0x00000002"));
        put32(0x00920000u + 0x64u, 3);
        const std::string units = run_pump_captured(at_block(t6, 2) + 60000000u);
        CHECK(has(units, "STARTUP_SCREAM_UNIT t_ms=") && has(units, "n=1 k=0 at=0x00920000 w64=3 w98=0 w40..a0="));
        CHECK(has(units, "n=1 k=1 at=0x00920100 w64=0"));
        CHECK(!has(units, "n=0 k=0"));
        CHECK(has(scream, "n=0 at=0x009ec400 enabled=0 count_field=0 walked=0\n"));
    }

    // ---- the service torn down (cellAudioQuit): no blocks ----
    CHECK_EQ(audio_quit(svc), 0);
    run_pump(at_block(t4, 700));
    CHECK_EQ(g_submits.size(), 0);

    startup_audio_out_stop();
    std::printf("TEST_AUDIO_OUT_PUMP checks=%u failures=%u\n", g_checks, g_failures);
    if (g_failures) { std::printf("TEST_AUDIO_OUT_PUMP_FAIL\n"); return 1; }
    std::printf("TEST_AUDIO_OUT_PUMP_ALL_PASS\n");
    return 0;
}
