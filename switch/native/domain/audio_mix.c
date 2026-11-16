/* Platform-free downmix of a cellAudio port block to stereo int16. See audio_mix.h for the contract. */
#include "audio_mix.h"

#include <string.h>

uint32_t audio_mix_block_bytes(uint32_t channels)
{
    if (channels != 2u && channels != 6u && channels != 8u) return 0u;
    return AUDIO_MIX_BLOCK_FRAMES * channels * (uint32_t)sizeof(float);
}

uint32_t audio_mix_level_q15(uint32_t level_float_bits)
{
    float level;
    if ((level_float_bits & 0x7f800000u) == 0x7f800000u) return AUDIO_MIX_LEVEL_UNITY;   /* NaN, infinity */
    memcpy(&level, &level_float_bits, sizeof level);
    if (!(level > 0.0f)) return AUDIO_MIX_LEVEL_UNITY;   /* unset (0) or negative: not muted */
    if (level >= 1.0f) return AUDIO_MIX_LEVEL_UNITY;
    return (uint32_t)(level * 32768.0f + 0.5f);
}

int audio_mix_block_is_silent(const uint8_t *block, uint32_t channels)
{
    const uint32_t bytes = audio_mix_block_bytes(channels);
    /* The sign bit of every big-endian float is bit 7 of its first byte; -0.0 is as silent as +0.0. */
    static const uint8_t sign_bytes[8] = {0x80u, 0u, 0u, 0u, 0x80u, 0u, 0u, 0u};
    uint64_t sign_mask, word;
    uint32_t offset;
    if (!bytes) return 1;
    memcpy(&sign_mask, sign_bytes, sizeof sign_mask);
    for (offset = 0; offset < bytes; offset += 8u) {
        memcpy(&word, block + offset, sizeof word);
        if (word & ~sign_mask) return 0;
    }
    return 1;
}

/* One big-endian float32 as a Q15 sample: round(clamp(f, -1, 1) * 32768), so +-1.0 are +-32768. */
static int32_t decode_q15(const uint8_t *p)
{
    uint32_t bits = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    float f;
    if ((bits & 0x7f800000u) == 0x7f800000u) return 0;   /* NaN, infinity */
    memcpy(&f, &bits, sizeof f);
    if (f >= 1.0f) return 32768;
    if (f <= -1.0f) return -32768;
    f *= 32768.0f;   /* exact: a power of two */
    return (int32_t)(f >= 0.0f ? f + 0.5f : f - 0.5f);
}

/* (accumulator in Q30 + half) >> 15, then the level, then the clamp to int16. The shift of a negative
 * value is an arithmetic shift (gcc, clang), so the rounding is the same for both signs: floor(x + 0.5). */
static int16_t finish(int64_t acc, uint32_t level_q15)
{
    int64_t v = (acc + 16384) >> 15;
    if (level_q15 != AUDIO_MIX_LEVEL_UNITY) v = (v * (int64_t)level_q15 + 16384) >> 15;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

int audio_mix_block_to_stereo(const uint8_t *block, uint32_t channels, uint32_t level_q15, int16_t *out)
{
    uint32_t frame;
    if (!audio_mix_block_bytes(channels)) {
        memset(out, 0, AUDIO_MIX_OUT_SAMPLES * sizeof out[0]);
        return -1;
    }
    if (channels == 2u) {
        for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) {
            const uint8_t *s = block + frame * 8u;
            out[frame * 2u] = finish((int64_t)decode_q15(s) * AUDIO_MIX_W_STEREO, level_q15);
            out[frame * 2u + 1u] = finish((int64_t)decode_q15(s + 4) * AUDIO_MIX_W_STEREO, level_q15);
        }
        return 0;
    }
    for (frame = 0; frame < AUDIO_MIX_BLOCK_FRAMES; ++frame) {
        const uint8_t *s = block + frame * channels * 4u;
        const int64_t centre = (int64_t)decode_q15(s + 8) * AUDIO_MIX_W_CENTER;
        const int64_t lfe = (int64_t)decode_q15(s + 12) * AUDIO_MIX_W_LFE;
        int64_t left = (int64_t)decode_q15(s) * AUDIO_MIX_W_FRONT + centre + lfe;
        int64_t right = (int64_t)decode_q15(s + 4) * AUDIO_MIX_W_FRONT + centre + lfe;
        left += (int64_t)decode_q15(s + 16) * AUDIO_MIX_W_REAR;
        right += (int64_t)decode_q15(s + 20) * AUDIO_MIX_W_REAR;
        if (channels == 8u) {
            left += (int64_t)decode_q15(s + 24) * AUDIO_MIX_W_SIDE;
            right += (int64_t)decode_q15(s + 28) * AUDIO_MIX_W_SIDE;
        }
        out[frame * 2u] = finish(left, level_q15);
        out[frame * 2u + 1u] = finish(right, level_q15);
    }
    return 0;
}

int audio_mix_consume_block(uint8_t *block, uint32_t channels, uint32_t level_q15, int16_t *out)
{
    const uint32_t bytes = audio_mix_block_bytes(channels);
    if (!bytes) {
        memset(out, 0, AUDIO_MIX_OUT_SAMPLES * sizeof out[0]);
        return -1;
    }
    if (audio_mix_block_is_silent(block, channels)) {
        memset(out, 0, AUDIO_MIX_OUT_SAMPLES * sizeof out[0]);
        return 0;
    }
    audio_mix_block_to_stereo(block, channels, level_q15, out);
    memset(block, 0, bytes);
    return 1;
}

void audio_mix_add_stereo(int16_t *acc, const int16_t *in)
{
    uint32_t i;
    for (i = 0; i < AUDIO_MIX_OUT_SAMPLES; ++i) {
        int32_t v = (int32_t)acc[i] + (int32_t)in[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        acc[i] = (int16_t)v;
    }
}

void audio_mix_scan_ring(const uint8_t *ring, uint32_t channels, uint32_t blocks, audio_ring_scan *out)
{
    const uint32_t block_bytes = audio_mix_block_bytes(channels);
    uint32_t block, offset;
    memset(out, 0, sizeof *out);
    if (!block_bytes) return;
    for (block = 0; block < blocks; ++block) {
        const uint8_t *b = ring + (size_t)block * block_bytes;
        uint32_t stamped = 0;
        for (offset = 0; offset < block_bytes; offset += 4u) {
            const uint32_t bits = ((uint32_t)b[offset] << 24) | ((uint32_t)b[offset + 1u] << 16) |
                                  ((uint32_t)b[offset + 2u] << 8) | (uint32_t)b[offset + 3u];
            const uint32_t magnitude = bits & 0x7fffffffu;
            if (bits == AUDIO_MIX_PROBE_WORD) { ++stamped; continue; }
            if (!magnitude) continue;
            ++out->nonzero_samples;
            if (block < AUDIO_MIX_SCAN_MAX_BLOCKS) out->slot_mask |= 1u << block;
            if (magnitude >= 0x7f800000u) ++out->nonfinite_samples;
            /* a non-negative IEEE float orders like its bit pattern */
            else if (magnitude > out->max_abs_bits) out->max_abs_bits = magnitude;
        }
        out->probe_samples += stamped;
        if (stamped == block_bytes / 4u) ++out->probe_blocks;
    }
}

uint32_t audio_mix_count_nonzero_words(const uint8_t *bytes, uint32_t length, uint32_t *first_offset)
{
    uint32_t offset, count = 0, word;
    *first_offset = UINT32_MAX;
    for (offset = 0; offset + 4u <= length; offset += 4u) {
        memcpy(&word, bytes + offset, sizeof word);
        if (!word) continue;
        if (!count) *first_offset = offset;
        ++count;
    }
    return count;
}

void audio_mix_stamp_block(uint8_t *block, uint32_t channels, uint32_t word)
{
    const uint32_t bytes = audio_mix_block_bytes(channels);
    uint32_t offset;
    for (offset = 0; offset < bytes; offset += 4u) {
        block[offset] = (uint8_t)(word >> 24);
        block[offset + 1u] = (uint8_t)(word >> 16);
        block[offset + 2u] = (uint8_t)(word >> 8);
        block[offset + 3u] = (uint8_t)word;
    }
}

int audio_mix_block_is_stamped(const uint8_t *block, uint32_t channels, uint32_t word)
{
    const uint32_t bytes = audio_mix_block_bytes(channels);
    uint32_t offset;
    if (!bytes) return 0;
    for (offset = 0; offset < bytes; offset += 4u) {
        const uint32_t v = ((uint32_t)block[offset] << 24) | ((uint32_t)block[offset + 1u] << 16) |
                           ((uint32_t)block[offset + 2u] << 8) | (uint32_t)block[offset + 3u];
        if (v != word) return 0;
    }
    return 1;
}

int audio_mix_block_is_silent_or_stamped(const uint8_t *block, uint32_t channels, uint32_t word)
{
    const uint32_t bytes = audio_mix_block_bytes(channels);
    uint32_t offset;
    for (offset = 0; offset < bytes; offset += 4u) {
        const uint32_t v = ((uint32_t)block[offset] << 24) | ((uint32_t)block[offset + 1u] << 16) |
                           ((uint32_t)block[offset + 2u] << 8) | (uint32_t)block[offset + 3u];
        if ((v & 0x7fffffffu) != 0u && v != word) return 0;
    }
    return 1;
}
