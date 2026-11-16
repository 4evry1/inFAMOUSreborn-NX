#ifndef NATIVE_STARTUP_AUDIO_OUT_H
#define NATIVE_STARTUP_AUDIO_OUT_H
/* Root (2026-11-15): the glue between the process's audio service (StartupAudioProcess, native/domain/audio_service.h)
 * and the console's sound output (audio_out_horizon.h). Used only with PS3_NATIVE_AUDIO_OUT.
 *
 * It is the output thread's pump. Every 2 ms on that thread it
 *   1. advances the service's block clock (startup_audio_tick: the same call the 1 ms ticker makes, so the guest's
 *      read index keeps moving even while the ticker thread is busy drawing a frame),
 *   2. finds every open, running port and the ring blocks the clock has moved past since its last visit,
 *   3. mixes each one to stereo (audio_mix_consume_block), CLEARS it in guest memory the way the firmware's reader
 *      does (a block the game has not refilled then reads as silence instead of replaying), adds the ports together
 *      and queues the sum. "Cleared" is the write probe's stamp (AUDIO_MIX_PROBE_WORD, 1e-30f in every sample: as
 *      silent as zero, and harmless to a producer that adds into the block): a block still stamped when the reader
 *      comes round again was never written (counted "untouched", played as silence); one the game rewrote with
 *      zeros counts "written_silent". The first lap of a run cannot tell zeros the game wrote from the window's own
 *      zeros, so up to one ring of blocks counts as written_silent at the start of every run.
 * Decision 132: every running port is played. The lowest running port sets how many blocks go out per pump; each
 * other port hands on as many of its own due blocks (it runs on the same clock, so it has as many due, give or take
 * one at a phase edge). The game's mix (port 0, 8 channels) and a Bink movie's sound (a second port) are heard
 * together; before, only port 0 was played and a movie was always silent.
 *
 * Diagnostics, printed and flushed at once (a console run ends with HOME, so a summary at exit never appears): when a
 * port run starts and every STARTUP_AUDIO_DIAG_SECONDS, BEFORE the reader takes blocks,
 *   STARTUP_AUDIO_RING   per open port: shape, counter, the read-index word as the guest sees it, how many non-zero
 *                        samples the WHOLE ring holds, which slots, the largest |sample|, blocks heard so far
 *   STARTUP_AUDIO_WINDOW non-zero words anywhere else in the audio window (past the read indexes, outside open rings)
 *   STARTUP_AUDIO_CALLS  the count of each of the 13 audio imports */
#include "startup_execution.h"

/* A host pointer to `length` bytes of guest memory at `address`, or NULL (ps3_flat_guest_fast_pointer). */
typedef void *(*startup_audio_window_fn)(uint32_t address, uint32_t length);

/* Starts audout and the output thread. Returns 0 when sound is playing; otherwise the libnx Result, no thread was
 * started, nothing in guest memory is touched and the game runs as without PS3_NATIVE_AUDIO_OUT. `process`, `memory`
 * and `clock` must outlive startup_audio_out_stop(). */
int32_t startup_audio_out_start(StartupAudioProcess &process, const ps3_guest_memory &memory, const ps3_clock &clock,
                                startup_audio_window_fn window);

/* Joins the output thread (bounded) and stops audout. Call before the guest memory goes away. */
void startup_audio_out_stop(void);

#endif /* NATIVE_STARTUP_AUDIO_OUT_H */
