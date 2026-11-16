/* Host test of the platform-free sound output units: native/domain/audio_mix.c (downmix, silence test, consume)
 * and native/domain/audio_out_policy.c (ring plan, device queue plan). No libnx, no guest memory.
 *
 *   gcc -std=c11 -Wall -Wextra -Werror -O2 -ffp-contract=off -I native/domain \
 *       native/domain/audio_mix.c native/domain/audio_out_policy.c native/tests/test_audio_mix.c -lm -o test_audio_mix
 */
#include "audio_mix.h"
#include "audio_out_policy.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(condition) do { ++g_checks; if (!(condition)) { ++g_failures; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)
#define CHECK_EQ(actual, expected) do { const long long a_ = (long long)(actual), e_ = (long long)(expected); \
    ++g_checks; if (a_ != e_) { ++g_failures; \
    printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); } } while (0)

static uint8_t g_block[AUDIO_MIX_BLOCK_FRAMES * AUDIO_MIX_MAX_CHANNELS * 4];
static int16_t g_out[AUDIO_MIX_OUT_SAMPLES];

static void put_be(uint8_t *p, uint32_t bits)
{
    p[0] = (uint8_t)(bits >> 24); p[1] = (uint8_t)(bits >> 16); p[2] = (uint8_t)(bits >> 8); p[3] = (uint8_t)bits;
}
static void put_f(uint32_t frame, uint32_t channels, uint32_t channel, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    put_be(g_block + (frame * channels + channel) * 4u, bits);
}
static void clear_block(void) { memset(g_block, 0, sizeof g_block); }
/* the same sample in every frame, so one check of frame 0 and one of the last frame cover the block */
static void fill_channel(uint32_t channels, uint32_t channel, float value)
{
    uint32_t frame;
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) put_f(frame, channels, channel, value);
}
static int all_frames(int16_t left, int16_t right)
{
    uint32_t frame;
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame)
        if (g_out[frame * 2u] != left || g_out[frame * 2u + 1u] != right) return 0;
    return 1;
}
static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void test_sizes(void)
{
    CHECK_EQ(audio_mix_block_bytes(2), 2048);
    CHECK_EQ(audio_mix_block_bytes(6), 6144);
    CHECK_EQ(audio_mix_block_bytes(8), 8192);
    CHECK_EQ(audio_mix_block_bytes(0), 0);
    CHECK_EQ(audio_mix_block_bytes(4), 0);
    CHECK_EQ(audio_mix_block_bytes(7), 0);
    CHECK_EQ(AUDIO_MIX_OUT_SAMPLES, 512);
}

static void test_silence(void)
{
    uint32_t channels[3] = {2, 6, 8}, i;
    for (i = 0; i < 3; ++i) {
        clear_block();
        memset(g_out, 0x55, sizeof g_out);
        CHECK(audio_mix_block_is_silent(g_block, channels[i]));
        CHECK_EQ(audio_mix_block_to_stereo(g_block, channels[i], AUDIO_MIX_LEVEL_UNITY, g_out), 0);
        CHECK(all_frames(0, 0));
        /* consume: silent block -> 0, output zeros, block untouched */
        memset(g_out, 0x55, sizeof g_out);
        CHECK_EQ(audio_mix_consume_block(g_block, channels[i], AUDIO_MIX_LEVEL_UNITY, g_out), 0);
        CHECK(all_frames(0, 0));
    }
    /* -0.0 in every sample is silence too, and its output is exactly zero */
    for (i = 0; i < AUDIO_MIX_BLOCK_FRAMES * 8u; ++i) put_be(g_block + i * 4u, 0x80000000u);
    CHECK(audio_mix_block_is_silent(g_block, 8));
    CHECK_EQ(audio_mix_consume_block(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out), 0);
    CHECK(all_frames(0, 0));
    CHECK_EQ(g_block[0], 0x80);   /* a silent block is left alone */
    /* the smallest non-zero sample anywhere (the last one of the block) is not silence */
    clear_block();
    put_be(g_block + 8192u - 4u, 0x00000001u);
    CHECK(!audio_mix_block_is_silent(g_block, 8));
    /* the first sample, and the other floats of the first 8-byte word */
    clear_block();
    put_be(g_block, 0x3f800000u);
    CHECK(!audio_mix_block_is_silent(g_block, 8));
    clear_block();
    put_be(g_block + 4, 0x00800000u);
    CHECK(!audio_mix_block_is_silent(g_block, 2));
    /* an unsupported channel count */
    CHECK(audio_mix_block_is_silent(g_block, 4));
    memset(g_out, 0x55, sizeof g_out);
    CHECK_EQ(audio_mix_block_to_stereo(g_block, 4, AUDIO_MIX_LEVEL_UNITY, g_out), -1);
    CHECK(all_frames(0, 0));
    memset(g_out, 0x55, sizeof g_out);
    CHECK_EQ(audio_mix_consume_block(g_block, 3, AUDIO_MIX_LEVEL_UNITY, g_out), -1);
    CHECK(all_frames(0, 0));
}

static void test_stereo_values(void)
{
    uint32_t frame;
    /* big-endian decode: 3F 00 00 00 = 0.5 */
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) {
        g_block[frame * 8u + 0] = 0x3f; g_block[frame * 8u + 4] = 0xbf;   /* L = 0.5, R = -0.5 */
    }
    CHECK_EQ(audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out), 0);
    CHECK(all_frames(16384, -16384));
    /* full scale: +1.0 is held at 32767, -1.0 is -32768 */
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) { put_f(frame, 2, 0, 1.0f); put_f(frame, 2, 1, -1.0f); }
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK(all_frames(32767, -32768));
    /* beyond full scale: held, not wrapped */
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) { put_f(frame, 2, 0, 3.5f); put_f(frame, 2, 1, -1000.0f); }
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK(all_frames(32767, -32768));
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) { put_f(frame, 2, 0, 3.0e38f); put_f(frame, 2, 1, -3.0e38f); }
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK(all_frames(32767, -32768));
    /* NaN and both infinities are zero, in every position; the neighbours are not disturbed */
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) { put_f(frame, 2, 0, 0.5f); put_f(frame, 2, 1, 0.25f); }
    put_be(g_block + 0, 0x7fc00000u);    /* frame 0 L: quiet NaN */
    put_be(g_block + 4, 0x7f800000u);    /* frame 0 R: +inf */
    put_be(g_block + 8, 0xff800000u);    /* frame 1 L: -inf */
    put_be(g_block + 12, 0xffffffffu);   /* frame 1 R: NaN with the sign set */
    put_be(g_block + 16, 0x7f800001u);   /* frame 2 L: signalling NaN */
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK_EQ(g_out[0], 0); CHECK_EQ(g_out[1], 0);
    CHECK_EQ(g_out[2], 0); CHECK_EQ(g_out[3], 0);
    CHECK_EQ(g_out[4], 0); CHECK_EQ(g_out[5], 8192);
    CHECK_EQ(g_out[6], 16384); CHECK_EQ(g_out[7], 8192);
    /* rounding: half a step goes away from zero */
    clear_block();
    put_f(0, 2, 0, 1.5f / 32768.0f); put_f(0, 2, 1, -1.5f / 32768.0f);   /* 1.5 -> 2 */
    put_f(1, 2, 0, 0.4f / 32768.0f); put_f(1, 2, 1, -0.4f / 32768.0f);   /* 0.4 -> 0 */
    put_f(2, 2, 0, 0.5f / 32768.0f); put_f(2, 2, 1, -0.5f / 32768.0f);   /* 0.5 -> 1 */
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK_EQ(g_out[0], 2); CHECK_EQ(g_out[1], -2);
    CHECK_EQ(g_out[2], 0); CHECK_EQ(g_out[3], 0);
    CHECK_EQ(g_out[4], 1); CHECK_EQ(g_out[5], -1);
    /* frames stay in order: frame f carries f/512 on the left and -f/512 on the right */
    clear_block();
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) {
        put_f(frame, 2, 0, (float)frame / 512.0f);
        put_f(frame, 2, 1, -(float)frame / 512.0f);
    }
    audio_mix_block_to_stereo(g_block, 2, AUDIO_MIX_LEVEL_UNITY, g_out);
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) {
        CHECK_EQ(g_out[frame * 2u], frame * 64u);
        CHECK_EQ(g_out[frame * 2u + 1u], -(long long)(frame * 64u));
    }
}

/* each channel alone at full scale; the expected pair is the Q15 weight itself */
static void test_routing(void)
{
    static const struct { uint32_t channel; int left, right; } eight[8] = {
        {0, AUDIO_MIX_W_FRONT, 0}, {1, 0, AUDIO_MIX_W_FRONT},
        {2, AUDIO_MIX_W_CENTER, AUDIO_MIX_W_CENTER}, {3, AUDIO_MIX_W_LFE, AUDIO_MIX_W_LFE},
        {4, AUDIO_MIX_W_REAR, 0}, {5, 0, AUDIO_MIX_W_REAR},
        {6, AUDIO_MIX_W_SIDE, 0}, {7, 0, AUDIO_MIX_W_SIDE}};
    uint32_t i;
    /* the constants themselves: 0.75, 0.75*0.7071, 0.75*0.5, 0.75*0.5, 0.75*0.3 in Q15 */
    CHECK_EQ(AUDIO_MIX_W_FRONT, 24576); CHECK_EQ(AUDIO_MIX_W_CENTER, 17378); CHECK_EQ(AUDIO_MIX_W_LFE, 12288);
    CHECK_EQ(AUDIO_MIX_W_REAR, 12288); CHECK_EQ(AUDIO_MIX_W_SIDE, 7373); CHECK_EQ(AUDIO_MIX_W_STEREO, 32768);
    for (i = 0; i < 8; ++i) {
        fill_channel(8, eight[i].channel, 1.0f);
        audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
        CHECK(all_frames((int16_t)eight[i].left, (int16_t)eight[i].right));
        fill_channel(8, eight[i].channel, -1.0f);
        audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
        CHECK(all_frames((int16_t)-eight[i].left, (int16_t)-eight[i].right));
        /* half scale is half the weight */
        fill_channel(8, eight[i].channel, 0.5f);
        audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
        CHECK(all_frames((int16_t)((eight[i].left + 1) / 2), (int16_t)((eight[i].right + 1) / 2)));
    }
    /* six channels: the first six behave the same (the 8 channel pair 6,7 does not exist) */
    for (i = 0; i < 6; ++i) {
        fill_channel(6, eight[i].channel, 1.0f);
        audio_mix_block_to_stereo(g_block, 6, AUDIO_MIX_LEVEL_UNITY, g_out);
        CHECK(all_frames((int16_t)eight[i].left, (int16_t)eight[i].right));
    }
    /* the sample positions inside a frame depend on the channel count: 6 channels put C at byte 8 of each 24-byte frame */
    clear_block();
    put_f(1, 6, 2, 1.0f);   /* frame 1, centre */
    audio_mix_block_to_stereo(g_block, 6, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK_EQ(g_out[0], 0); CHECK_EQ(g_out[1], 0);
    CHECK_EQ(g_out[2], AUDIO_MIX_W_CENTER); CHECK_EQ(g_out[3], AUDIO_MIX_W_CENTER);
    CHECK_EQ(g_out[4], 0); CHECK_EQ(g_out[5], 0);
    /* two channels at once add: L and C together */
    clear_block();
    put_f(0, 8, 0, 0.5f); put_f(0, 8, 2, 0.5f); put_f(0, 8, 7, -0.5f);
    audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK_EQ(g_out[0], 12288 + 8689);          /* 0.5*24576 + 0.5*17378 = 12288 + 8689 */
    CHECK_EQ(g_out[1], 5003);                  /* 0.5*17378 - 0.5*7373 = 8689 - 3686.5 = 5002.5, rounded up to 5003 (floor(x + 0.5)) */
    /* all eight channels at full scale: the sum is far past 16 bits and is held, never wrapped */
    for (i = 0; i < AUDIO_MIX_BLOCK_FRAMES * 8u; ++i) put_be(g_block + i * 4u, 0x3f800000u);
    audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK(all_frames(32767, 32767));
    for (i = 0; i < AUDIO_MIX_BLOCK_FRAMES * 8u; ++i) put_be(g_block + i * 4u, 0xbf800000u);
    audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK(all_frames(-32768, -32768));
    /* NaN in one surround channel is zero and the other channels still play */
    fill_channel(8, 0, 1.0f);
    put_be(g_block + 6 * 4, 0x7fc00000u);   /* frame 0, channel 6 */
    audio_mix_block_to_stereo(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out);
    CHECK_EQ(g_out[0], AUDIO_MIX_W_FRONT); CHECK_EQ(g_out[1], 0);
}

static void test_level(void)
{
    CHECK_EQ(audio_mix_level_q15(float_bits(1.0f)), 32768);
    CHECK_EQ(audio_mix_level_q15(float_bits(0.5f)), 16384);
    CHECK_EQ(audio_mix_level_q15(float_bits(0.25f)), 8192);
    CHECK_EQ(audio_mix_level_q15(float_bits(2.0f)), 32768);       /* above unity: unity */
    CHECK_EQ(audio_mix_level_q15(float_bits(0.0f)), 32768);       /* unset: not muted */
    CHECK_EQ(audio_mix_level_q15(float_bits(-0.0f)), 32768);
    CHECK_EQ(audio_mix_level_q15(float_bits(-0.5f)), 32768);
    CHECK_EQ(audio_mix_level_q15(0x7fc00000u), 32768);            /* NaN */
    CHECK_EQ(audio_mix_level_q15(0x7f800000u), 32768);            /* +inf */
    CHECK_EQ(audio_mix_level_q15(0xff800000u), 32768);            /* -inf */
    CHECK_EQ(audio_mix_level_q15(float_bits(1.0f / 65536.0f)), 1);
    /* the level scales the mix: stereo full scale at 0.5 */
    fill_channel(2, 0, 1.0f);
    audio_mix_block_to_stereo(g_block, 2, 16384, g_out);
    CHECK(all_frames(16384, 0));
    fill_channel(2, 1, -1.0f);
    audio_mix_block_to_stereo(g_block, 2, 16384, g_out);
    CHECK(all_frames(0, -16384));
    fill_channel(8, 2, 1.0f);
    audio_mix_block_to_stereo(g_block, 8, 16384, g_out);
    CHECK(all_frames(8689, 8689));
    /* a level of zero would mute: the output is zero (the caller maps an unset level to unity first) */
    audio_mix_block_to_stereo(g_block, 8, 0, g_out);
    CHECK(all_frames(0, 0));
}

static void test_consume(void)
{
    uint32_t i;
    fill_channel(8, 2, 0.5f);
    CHECK(!audio_mix_block_is_silent(g_block, 8));
    CHECK_EQ(audio_mix_consume_block(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out), 1);
    CHECK(all_frames(8689, 8689));
    for (i = 0; i < 8192u; ++i) if (g_block[i]) break;
    CHECK_EQ(i, 8192);                              /* every byte of the block was cleared */
    /* the second visit finds a silent block: nothing to play, and the output is zeros */
    CHECK_EQ(audio_mix_consume_block(g_block, 8, AUDIO_MIX_LEVEL_UNITY, g_out), 0);
    CHECK(all_frames(0, 0));
    /* only the block's own bytes are cleared: a guard pattern after it stays */
    {
        static uint8_t buffer[2048 + 16];
        memset(buffer, 0, sizeof buffer);
        put_be(buffer + 100, 0x3f000000u);
        memset(buffer + 2048, 0xa5, 16);
        CHECK_EQ(audio_mix_consume_block(buffer, 2, AUDIO_MIX_LEVEL_UNITY, g_out), 1);
        CHECK_EQ(buffer[100], 0);
        CHECK_EQ(buffer[2048], 0xa5); CHECK_EQ(buffer[2048 + 15], 0xa5);
    }
}

/* random blocks against a real-valued reference of the documented formula */
static void test_against_reference(void)
{
    static const double weight[8][2] = {{1, 0}, {0, 1}, {0.7071, 0.7071}, {0.5, 0.5}, {0.5, 0}, {0, 0.5}, {0.3, 0}, {0, 0.3}};
    unsigned seed = 12345u, run, frame, channel, side;
    int worst = 0;
    for (run = 0; run < 40; ++run) {
        const uint32_t channels = run % 3u == 0 ? 8u : run % 3u == 1 ? 6u : 2u;
        clear_block();
        for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame)
            for (channel = 0; channel < channels; ++channel) {
                seed = seed * 1664525u + 1013904223u;
                put_f(frame, channels, channel, ((float)(seed >> 8) / 8388608.0f - 1.0f) * (run % 5u == 4u ? 1.5f : 1.0f));
            }
        audio_mix_block_to_stereo(g_block, channels, AUDIO_MIX_LEVEL_UNITY, g_out);
        for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame)
            for (side = 0; side < 2; ++side) {
                double sum = 0.0;
                long expected;
                int diff;
                for (channel = 0; channel < channels; ++channel) {
                    uint32_t bits = ((uint32_t)g_block[(frame * channels + channel) * 4u] << 24) |
                                    ((uint32_t)g_block[(frame * channels + channel) * 4u + 1u] << 16) |
                                    ((uint32_t)g_block[(frame * channels + channel) * 4u + 2u] << 8) |
                                    (uint32_t)g_block[(frame * channels + channel) * 4u + 3u];
                    float f;
                    double w = channels == 2u ? (channel == side ? 1.0 : 0.0) : 0.75 * weight[channel][side];
                    memcpy(&f, &bits, sizeof f);
                    if (f > 1.0f) f = 1.0f;
                    if (f < -1.0f) f = -1.0f;
                    sum += w * (double)f;
                }
                expected = lround(sum * 32768.0);
                if (expected > 32767) expected = 32767;
                if (expected < -32768) expected = -32768;
                diff = abs((int)g_out[frame * 2u + side] - (int)expected);
                if (diff > worst) worst = diff;
            }
    }
    printf("reference: worst difference %d LSB over 40 random blocks\n", worst);
    CHECK(worst <= 4);
}

static void test_ring_plan(void)
{
    audio_ring_plan plan;
    audio_ring_plan_blocks(0, 0, 8, &plan);
    CHECK_EQ(plan.count, 0); CHECK_EQ(plan.skipped, 0);
    audio_ring_plan_blocks(0, 1, 8, &plan);
    CHECK_EQ(plan.first, 0); CHECK_EQ(plan.count, 1); CHECK_EQ(plan.skipped, 0);
    audio_ring_plan_blocks(5, 8, 8, &plan);
    CHECK_EQ(plan.first, 5); CHECK_EQ(plan.count, 3); CHECK_EQ(plan.skipped, 0);
    audio_ring_plan_blocks(8, 8, 8, &plan);
    CHECK_EQ(plan.count, 0);
    audio_ring_plan_blocks(9, 8, 8, &plan);          /* a counter behind what was handed on: nothing */
    CHECK_EQ(plan.count, 0); CHECK_EQ(plan.skipped, 0);
    audio_ring_plan_blocks(0, 6, 8, &plan);          /* exactly the catch-up limit */
    CHECK_EQ(plan.first, 0); CHECK_EQ(plan.count, 6); CHECK_EQ(plan.skipped, 0);
    audio_ring_plan_blocks(0, 7, 8, &plan);          /* one too many: the oldest is given up */
    CHECK_EQ(plan.first, 1); CHECK_EQ(plan.count, 6); CHECK_EQ(plan.skipped, 1);
    audio_ring_plan_blocks(10, 100, 8, &plan);
    CHECK_EQ(plan.first, 94); CHECK_EQ(plan.count, 6); CHECK_EQ(plan.skipped, 84);
    audio_ring_plan_blocks(10, 100, 32, &plan);      /* a bigger ring does not raise the limit */
    CHECK_EQ(plan.first, 94); CHECK_EQ(plan.count, 6); CHECK_EQ(plan.skipped, 84);
    audio_ring_plan_blocks(0, 4, 4, &plan);          /* a ring of 4 keeps at most 2 */
    CHECK_EQ(plan.first, 2); CHECK_EQ(plan.count, 2); CHECK_EQ(plan.skipped, 2);
    /* the plan's end is the next visit's start: no block is handed on twice or lost */
    {
        uint64_t consumed = 0, handed = 0;
        uint64_t counter;
        for (counter = 0; counter < 1000; counter += 1 + counter % 3u) {
            audio_ring_plan_blocks(consumed, counter, 8, &plan);
            CHECK(plan.first >= consumed);
            handed += plan.count;
            consumed = plan.first + plan.count;
            CHECK(consumed <= counter || counter == 0);
        }
        CHECK(handed + 0 <= 1000u);
    }
}

#define MS 1000000ull
static void test_policy(void)
{
    audio_out_policy policy;
    audio_out_submit_plan plan;
    audio_out_policy_init(&policy, 3, 8, 250 * MS);
    /* the first block starts the stream: three blocks of silence ahead of it, not an underrun */
    audio_out_policy_plan_submit(&policy, 0, 1000 * MS, &plan);
    CHECK_EQ(plan.silence_before, 3); CHECK_EQ(plan.underrun, 0); CHECK_EQ(plan.drop, 0);
    /* steady: blocks keep arriving and the queue holds some */
    audio_out_policy_plan_submit(&policy, 3, 1005 * MS, &plan);
    CHECK_EQ(plan.silence_before, 0); CHECK_EQ(plan.underrun, 0); CHECK_EQ(plan.drop, 0);
    audio_out_policy_plan_submit(&policy, 1, 1011 * MS, &plan);   /* low but not dry */
    CHECK_EQ(plan.silence_before, 0); CHECK_EQ(plan.underrun, 0); CHECK_EQ(plan.drop, 0);
    /* the device ran dry while the stream was running: an underrun, silence first, never a wait */
    audio_out_policy_plan_submit(&policy, 0, 1016 * MS, &plan);
    CHECK_EQ(plan.silence_before, 3); CHECK_EQ(plan.underrun, 1); CHECK_EQ(plan.drop, 0);
    /* full: the block is dropped (the guest clock does not wait) */
    audio_out_policy_plan_submit(&policy, 7, 1021 * MS, &plan);
    CHECK_EQ(plan.drop, 0);
    audio_out_policy_plan_submit(&policy, 8, 1026 * MS, &plan);
    CHECK_EQ(plan.drop, 1); CHECK_EQ(plan.silence_before, 0); CHECK_EQ(plan.underrun, 0);
    audio_out_policy_plan_submit(&policy, 12, 1031 * MS, &plan);
    CHECK_EQ(plan.drop, 1);
    /* a silent gap longer than idle_ns ends the stream: the next block restarts it, without an underrun */
    audio_out_policy_plan_submit(&policy, 0, 1031 * MS + 251 * MS, &plan);
    CHECK_EQ(plan.silence_before, 3); CHECK_EQ(plan.underrun, 0); CHECK_EQ(plan.drop, 0);
    /* a gap of exactly idle_ns is still the same stream */
    audio_out_policy_plan_submit(&policy, 0, 1282 * MS + 250 * MS, &plan);
    CHECK_EQ(plan.underrun, 1); CHECK_EQ(plan.silence_before, 3);
    /* a restart onto a queue that still holds blocks (a device that stopped releasing): drop, no silence */
    audio_out_policy_plan_submit(&policy, 6, 1532 * MS + 400 * MS, &plan);
    CHECK_EQ(plan.drop, 1); CHECK_EQ(plan.silence_before, 0);
    /* a clock that steps back is not an idle gap */
    audio_out_policy_plan_submit(&policy, 4, 1000 * MS, &plan);
    CHECK_EQ(plan.silence_before, 0); CHECK_EQ(plan.underrun, 0); CHECK_EQ(plan.drop, 0);
    /* the exact fill level that still takes a block: depth 7 of 8; a restart with 5 queued does not fit its silence */
    audio_out_policy_init(&policy, 3, 8, 250 * MS);
    audio_out_policy_plan_submit(&policy, 5, 0, &plan);
    CHECK_EQ(plan.drop, 1); CHECK_EQ(plan.silence_before, 0);
    audio_out_policy_init(&policy, 3, 8, 250 * MS);
    audio_out_policy_plan_submit(&policy, 4, 0, &plan);
    CHECK_EQ(plan.drop, 0); CHECK_EQ(plan.silence_before, 3);
}

static void test_add_stereo(void)
{
    int16_t acc[AUDIO_MIX_OUT_SAMPLES], in[AUDIO_MIX_OUT_SAMPLES];
    uint32_t i;
    for (i = 0; i < AUDIO_MIX_OUT_SAMPLES; ++i) { acc[i] = 1000; in[i] = -300; }
    acc[0] = 30000; in[0] = 5000;            /* held at the top */
    acc[1] = -30000; in[1] = -5000;          /* held at the bottom */
    acc[2] = 32767; in[2] = -32768;          /* opposite extremes: -1 */
    audio_mix_add_stereo(acc, in);
    CHECK_EQ(acc[0], 32767);
    CHECK_EQ(acc[1], -32768);
    CHECK_EQ(acc[2], -1);
    CHECK_EQ(acc[3], 700);
    CHECK_EQ(acc[AUDIO_MIX_OUT_SAMPLES - 1u], 700);
    CHECK_EQ(in[3], -300);                   /* the input is not changed */
}

static void test_scan_ring(void)
{
    static uint8_t ring[8u * 8192u];
    audio_ring_scan scan;
    uint32_t first = 0;
    memset(ring, 0, sizeof ring);
    memset(&scan, 0x55, sizeof scan);
    audio_mix_scan_ring(ring, 8, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 0); CHECK_EQ(scan.slot_mask, 0); CHECK_EQ(scan.max_abs_bits, 0); CHECK_EQ(scan.nonfinite_samples, 0);
    /* -0.0 is silence */
    put_be(ring + 4u, 0x80000000u);
    audio_mix_scan_ring(ring, 8, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 0);
    /* block 0: one -0.5; block 5: +0.25 and a NaN; block 7, last sample: -0.75 (the largest finite magnitude) */
    put_be(ring + 8u, float_bits(-0.5f));
    put_be(ring + 5u * 8192u + 100u * 4u, float_bits(0.25f));
    put_be(ring + 5u * 8192u + 101u * 4u, 0x7fc00000u);
    put_be(ring + 8u * 8192u - 4u, float_bits(-0.75f));
    audio_mix_scan_ring(ring, 8, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 4);
    CHECK_EQ(scan.nonfinite_samples, 1);
    CHECK_EQ(scan.slot_mask, 0xa1);
    CHECK_EQ(scan.max_abs_bits, float_bits(0.75f));
    /* the same bytes as a 2-channel ring of 8 blocks only reach the first 16 KiB: block 0 only */
    audio_mix_scan_ring(ring, 2, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 1); CHECK_EQ(scan.slot_mask, 0x01);
    /* an unsupported shape scans nothing */
    audio_mix_scan_ring(ring, 4, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 0);
    audio_mix_scan_ring(ring, 8, 0, &scan);
    CHECK_EQ(scan.nonzero_samples, 0);
    /* word counter: any non-zero bit pattern counts (the -0.0 at offset 4 too), the first hit's offset is given,
     * a tail shorter than a word is ignored */
    CHECK_EQ(audio_mix_count_nonzero_words(ring, 16, &first), 2);
    CHECK_EQ(first, 4);
    CHECK_EQ(audio_mix_count_nonzero_words(ring, 4, &first), 0);
    CHECK_EQ(first, UINT32_MAX);
    CHECK_EQ(audio_mix_count_nonzero_words(ring + 8u, 3, &first), 0);
    CHECK_EQ(audio_mix_count_nonzero_words(ring, sizeof ring, &first), 5);
    CHECK_EQ(first, 4);
    /* the reader's stamp is counted apart, never as sound: block 2 stamped whole, block 3 stamped but for one sample */
    audio_mix_stamp_block(ring + 2u * 8192u, 8, AUDIO_MIX_PROBE_WORD);
    audio_mix_stamp_block(ring + 3u * 8192u, 8, AUDIO_MIX_PROBE_WORD);
    put_be(ring + 3u * 8192u + 40u, 0u);
    audio_mix_scan_ring(ring, 8, 8, &scan);
    CHECK_EQ(scan.nonzero_samples, 4);
    CHECK_EQ(scan.slot_mask, 0xa1);
    CHECK_EQ(scan.probe_samples, 2u * 2048u - 1u);
    CHECK_EQ(scan.probe_blocks, 1);
}

static void test_probe_stamp(void)
{
    static uint8_t block[8192];
    int16_t out[AUDIO_MIX_OUT_SAMPLES];
    float f;
    uint32_t bits = AUDIO_MIX_PROBE_WORD;
    memcpy(&f, &bits, sizeof f);
    CHECK(f > 0.9e-30f && f < 1.1e-30f);                       /* 1e-30: a normal float, inaudible */
    memset(block, 0, sizeof block);
    CHECK_EQ(audio_mix_block_is_stamped(block, 8, AUDIO_MIX_PROBE_WORD), 0);
    audio_mix_stamp_block(block, 8, AUDIO_MIX_PROBE_WORD);
    CHECK_EQ(block[0], 0x0d); CHECK_EQ(block[3], 0x60); CHECK_EQ(block[8191], 0x60);
    CHECK_EQ(audio_mix_block_is_stamped(block, 8, AUDIO_MIX_PROBE_WORD), 1);
    CHECK_EQ(audio_mix_block_is_stamped(block, 2, AUDIO_MIX_PROBE_WORD), 1);   /* the first 2 KiB as a 2 ch block */
    CHECK_EQ(audio_mix_block_is_stamped(block, 5, AUDIO_MIX_PROBE_WORD), 0);   /* unsupported shape */
    /* a stamped block played anyway is silence */
    CHECK_EQ(audio_mix_block_to_stereo(block, 8, AUDIO_MIX_LEVEL_UNITY, out), 0);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[AUDIO_MIX_OUT_SAMPLES - 1u], 0);
    /* one sample changed: no longer stamped */
    put_be(block + 8188u, float_bits(0.5f));
    CHECK_EQ(audio_mix_block_is_stamped(block, 8, AUDIO_MIX_PROBE_WORD), 0);
    /* a 6 ch stamp covers exactly its 6 KiB */
    memset(block, 0, sizeof block);
    audio_mix_stamp_block(block, 6, AUDIO_MIX_PROBE_WORD);
    CHECK_EQ(audio_mix_block_is_stamped(block, 6, AUDIO_MIX_PROBE_WORD), 1);
    CHECK_EQ(block[6144], 0);
    audio_mix_stamp_block(block, 3, AUDIO_MIX_PROBE_WORD);                    /* unsupported: nothing written */
    CHECK_EQ(block[6144], 0);
    /* part stamp, part zeros (and a -0.0): nothing audible; one real sample: audible */
    memset(block, 0, sizeof block);
    audio_mix_stamp_block(block, 2, AUDIO_MIX_PROBE_WORD);
    put_be(block + 8u, 0x80000000u);
    CHECK_EQ(audio_mix_block_is_silent_or_stamped(block, 8, AUDIO_MIX_PROBE_WORD), 1);
    put_be(block + 4096u, float_bits(0.001f));
    CHECK_EQ(audio_mix_block_is_silent_or_stamped(block, 8, AUDIO_MIX_PROBE_WORD), 0);
}

int main(void)
{
    test_sizes();
    test_silence();
    test_stereo_values();
    test_routing();
    test_level();
    test_consume();
    test_against_reference();
    test_ring_plan();
    test_policy();
    test_add_stereo();
    test_scan_ring();
    test_probe_stamp();
    printf("TEST_AUDIO_MIX checks=%u failures=%u\n", g_checks, g_failures);
    if (g_failures) { printf("TEST_AUDIO_MIX_FAIL\n"); return 1; }
    printf("TEST_AUDIO_MIX_ALL_PASS\n");
    return 0;
}
