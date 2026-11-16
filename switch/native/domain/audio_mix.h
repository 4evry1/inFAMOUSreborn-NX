#ifndef AUDIO_MIX_H
#define AUDIO_MIX_H
/* Platform-free mixing of one cellAudio port block down to stereo 16-bit PCM.
 *
 * A port block is AUDIO_MIX_BLOCK_FRAMES frames of `channels` big-endian float32 samples, interleaved
 * (frame 0: ch0..chN-1, frame 1: ...), 2, 6 or 8 channels. The output is AUDIO_MIX_BLOCK_FRAMES frames
 * of left/right int16 in the host's byte order.
 *
 * Channel order of a 6 or 8 channel port: 0 L, 1 R, 2 C, 3 LFE, 4 Ls, 5 Rs, 6 Lb, 7 Rb (an 8 channel
 * port adds the last pair). The even channel of each pair feeds the left output, the odd one the right
 * output, so the order inside a surround pair cannot swap the sides.
 *
 * Downmix (integer, Q15 weights; the real-valued weights are in the comments):
 *   2 ch : L = L                            R = R
 *   6/8ch: L = 0.75 * (L + 0.7071 C + 0.5 LFE + 0.5 Ls + 0.3 Lb)      R likewise with R, Rs, Rb
 * The 0.75 is the headroom of the multi-channel layouts: a stereo-only mix passes at -2.5 dB, and a
 * mix with energy in several channels clips less often. The sum is clamped, never wrapped.
 * The weights follow the inFAMOUS 2 port's working downmix (C 0.707, LFE 0.5, rear 0.5, side 0.3).
 *
 * Every step is integer after the sample decode, so a host build and an aarch64 build produce the
 * same bytes. No globals, no allocation, no OS calls. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIX_BLOCK_FRAMES 256u
#define AUDIO_MIX_MAX_CHANNELS 8u
#define AUDIO_MIX_OUT_SAMPLES (AUDIO_MIX_BLOCK_FRAMES * 2u)   /* int16 values in one stereo block */
#define AUDIO_MIX_LEVEL_UNITY 32768u                          /* Q15: 1.0 */

/* Q15 weights (value / 32768). */
#define AUDIO_MIX_W_STEREO 32768   /* 1.0     2 ch: front pair, unscaled */
#define AUDIO_MIX_W_FRONT 24576    /* 0.75    6/8 ch: front pair */
#define AUDIO_MIX_W_CENTER 17378   /* 0.75 * 0.7071 */
#define AUDIO_MIX_W_LFE 12288      /* 0.75 * 0.5 */
#define AUDIO_MIX_W_REAR 12288     /* 0.75 * 0.5   channels 4 and 5 */
#define AUDIO_MIX_W_SIDE 7373      /* 0.75 * 0.3   channels 6 and 7 */

/* Bytes of one port block: frames * channels * 4. 0 for a channel count other than 2, 6, 8. */
uint32_t audio_mix_block_bytes(uint32_t channels);

/* The port's level (the guest's float, passed as its IEEE bit pattern in host order) as a Q15 factor.
 * A level that is NaN, infinite, zero or negative counts as 1.0 (a game that leaves it unset is not
 * muted), above 1.0 is 1.0, otherwise round(level * 32768). */
uint32_t audio_mix_level_q15(uint32_t level_float_bits);

/* 1 when every sample of the block is +0.0 or -0.0 (the block is silent, or the producer has not
 * refilled it), else 0. `block` holds block_bytes(channels) bytes; an unsupported channel count
 * returns 1. */
int audio_mix_block_is_silent(const uint8_t *block, uint32_t channels);

/* Mixes one block to stereo.
 *   block      : block_bytes(channels) bytes of big-endian float32, not modified
 *   level_q15  : the port level (audio_mix_level_q15), 32768 = unity
 *   out        : AUDIO_MIX_OUT_SAMPLES int16 values, L R L R ...
 * A sample that is NaN or infinite counts as 0; a finite sample beyond [-1, 1] is held at +-1 before
 * the mix. Returns 0, or -1 (and fills `out` with zeros) for an unsupported channel count. */
int audio_mix_block_to_stereo(const uint8_t *block, uint32_t channels, uint32_t level_q15, int16_t *out);

/* What the firmware's reader does with one block: mix it and then clear it (zero bytes), so a block the
 * producer has not refilled reads as silence instead of replaying. A silent block is not touched.
 * Returns 1 when the block held sound (mixed into `out`, then cleared), 0 when it was silent (`out`
 * is zeros, the block is as it was), -1 for an unsupported channel count (zeros in `out`). */
int audio_mix_consume_block(uint8_t *block, uint32_t channels, uint32_t level_q15, int16_t *out);

/* Adds `in` to `acc` sample by sample (AUDIO_MIX_OUT_SAMPLES values each), held at the int16 range: the sum of
 * two ports that are played at once (the game's mix and a movie's sound). */
void audio_mix_add_stereo(int16_t *acc, const int16_t *in);

/* What a whole port ring holds, for the diagnostics (Decision 132): read only, nothing is cleared. */
#define AUDIO_MIX_SCAN_MAX_BLOCKS 32u
typedef struct audio_ring_scan {
    uint32_t nonzero_samples;   /* samples other than +0.0 / -0.0 */
    uint32_t nonfinite_samples; /* NaN or infinity (counted in nonzero_samples too) */
    uint32_t slot_mask;         /* bit n: block n holds a non-zero sample (blocks beyond 32 are not in the mask) */
    uint32_t max_abs_bits;      /* IEEE bits of the largest finite |sample|, host order; 0 when none */
    uint32_t probe_samples;     /* samples equal to AUDIO_MIX_PROBE_WORD: the reader's stamp, not counted above */
    uint32_t probe_blocks;      /* blocks wholly stamped (nothing has written them since the reader took them) */
} audio_ring_scan;

/* Scans `blocks` blocks of `channels`-channel big-endian float32 starting at `ring`. An unsupported channel count
 * or zero blocks gives an all-zero result. */
void audio_mix_scan_ring(const uint8_t *ring, uint32_t channels, uint32_t blocks, audio_ring_scan *out);

/* Counts the 4-byte words of `bytes` bytes (a multiple of 4; a tail is ignored) that are not zero, and gives the
 * byte offset of the first one in `*first_offset` (UINT32_MAX when none). */
uint32_t audio_mix_count_nonzero_words(const uint8_t *bytes, uint32_t length, uint32_t *first_offset);

/* The write probe (Decision 132): after the reader has taken a block it stamps every sample with this value,
 * 1e-30f (0x0da24260), instead of leaving zeros. It is inaudible (it rounds to 0 in Q15) and a producer that ADDS
 * into the block, relying on the firmware having zeroed it, loses nothing (x + 1e-30 is x). When the reader comes
 * round again, a stamp still whole says nothing wrote the block; a stamp that is gone says the game wrote it, with
 * zeros or with sound. */
#define AUDIO_MIX_PROBE_WORD 0x0da24260u

/* Writes `word` (big-endian) into every sample of the block. Nothing for an unsupported channel count. */
void audio_mix_stamp_block(uint8_t *block, uint32_t channels, uint32_t word);

/* 1 when every sample of the block is `word` (big-endian), else 0 (also for an unsupported channel count). */
int audio_mix_block_is_stamped(const uint8_t *block, uint32_t channels, uint32_t word);

/* 1 when every sample is +0.0, -0.0 or `word` (big-endian): nothing audible, also when a producer overwrote only part
 * of a stamped block with zeros. Unsupported channel count: 1. */
int audio_mix_block_is_silent_or_stamped(const uint8_t *block, uint32_t channels, uint32_t word);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_MIX_H */
