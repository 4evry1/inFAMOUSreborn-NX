#ifndef AUDIO_OUT_HORIZON_H
#define AUDIO_OUT_HORIZON_H
/* The console's sound output: libnx audout, 48 kHz, 2 channels, signed 16 bit, one 256-frame buffer per
 * cellAudio block. Used only with PS3_NATIVE_AUDIO_OUT.
 *
 * One thread (core 2, priority 0x2b: above the guest threads, which run at 0x3B, and the SPU workers)
 * runs this loop every 2 ms:
 *     reclaim          audout buffers that finished playing go back to the free list
 *     pump(context)    the composition's callback: it advances the guest audio clock, takes every block
 *                      the "hardware" has finished playing from the guest ring, and hands each one to
 *                      audio_out_horizon_submit()
 *     status           the STARTUP_AUDIO_OUT line every 10 s
 * The pump runs on this thread, not on the 1 ms ticker thread: the ticker also drives the renderer, and a
 * long frame there must neither freeze the guest's audio clock nor starve the device.
 *
 * Never blocks the guest and never gates its clock: every block is either queued at the device, or counted
 * as dropped (the queue is full), or replaced by silence (the device ran dry). If audout cannot be
 * started, audio_out_horizon_start() logs once, starts no thread and returns the error: the game then
 * runs exactly as without PS3_NATIVE_AUDIO_OUT.
 *
 * No libnx type appears here, so any translation unit may include this header. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tunables (defaults; the build may override with -D). All in 256-frame blocks (5.33 ms each). */
#ifndef AUDIO_OUT_PREFILL_BLOCKS
#define AUDIO_OUT_PREFILL_BLOCKS 3u     /* silent blocks ahead of the first one and after an underrun: the jitter margin */
#endif
#ifndef AUDIO_OUT_MAX_DEPTH
#define AUDIO_OUT_MAX_DEPTH 8u          /* queued blocks at which a new block is dropped (43 ms) */
#endif
#ifndef AUDIO_OUT_THREAD_PRIORITY
#define AUDIO_OUT_THREAD_PRIORITY 0x2b
#endif
#ifndef AUDIO_OUT_THREAD_CORE
#define AUDIO_OUT_THREAD_CORE 2
#endif
#ifndef AUDIO_OUT_STATUS_SECONDS
#define AUDIO_OUT_STATUS_SECONDS 10u
#endif

typedef void (*audio_out_horizon_pump_fn)(void *context);

typedef struct audio_out_horizon_stats {
    int32_t init_result;        /* libnx Result of the start: 0 = playing */
    uint32_t active;            /* 1 while the output thread runs */
    uint32_t sample_rate;
    uint32_t in_flight;         /* blocks queued at the device now */
    uint64_t blocks;            /* game blocks queued at the device */
    uint64_t empty;             /* of those, all-zero when consumed (the game had not written them) */
    uint64_t silence;           /* silent filler blocks queued (start, underrun) */
    uint64_t underruns;         /* the device ran dry while blocks were still arriving */
    uint64_t dropped;           /* blocks not queued: queue full, no free buffer or audout refused */
    uint64_t skipped;           /* due ring blocks given up because a poll came too late */
    uint64_t append_failures;
    uint64_t pumps;
} audio_out_horizon_stats;

/* Starts audout and the output thread. Returns 0 when playing, else the libnx Result (no thread then).
 * Call once, before the game runs. */
int32_t audio_out_horizon_start(audio_out_horizon_pump_fn pump, void *context);

/* Stops and joins the thread (bounded: 2 s), then stops audout. Safe when start failed or was not called. */
void audio_out_horizon_stop(void);

/* 1 once audio_out_horizon_start() returned 0 and until stop. */
int audio_out_horizon_active(void);

/* From the pump callback only (it runs on the output thread): one finished block as 256 frames of
 * stereo int16 (AUDIO_MIX_OUT_SAMPLES values). `silent` says the guest block was all zero. */
void audio_out_horizon_submit(const int16_t *stereo, int silent);

/* From the pump callback: ring blocks that were due but given up. */
void audio_out_horizon_note_skipped(uint64_t blocks);

void audio_out_horizon_get_stats(audio_out_horizon_stats *out);

/* "STARTUP_AUDIO_OUT init=0x0 blocks=N underruns=N dropped=N empty=N empty_pct=P.T ..." into `buffer`
 * (no trailing newline). For a heartbeat that wants the line without waiting for the 10 s one. */
void audio_out_horizon_format_status(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_OUT_HORIZON_H */
