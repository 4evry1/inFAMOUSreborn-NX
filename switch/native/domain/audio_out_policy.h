#ifndef AUDIO_OUT_POLICY_H
#define AUDIO_OUT_POLICY_H
/* Platform-free decisions of the sound output (native/adapters/audio_out_horizon.h), kept apart from
 * libnx so a host test can drive them with a made-up clock.
 *
 * 1. Which ring blocks to hand on (audio_ring_plan_blocks): the audio service counts blocks played
 *    since the port started; every time the counter passes block j, block j % nBlock has been
 *    "played" and is due to be mixed and cleared. The output thread polls, so it may find several due.
 * 2. What to do with one finished block when it arrives at the device queue (audio_out_policy_plan_submit):
 *    pad with silence when the stream starts or after the device ran dry, drop the block when the queue is
 *    already full. The guest's audio clock never waits on the device: a late stream is silence, an early
 *    one is dropped blocks.
 * No globals, no allocation, no OS calls; time arrives as an argument. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. ring blocks ---- */

/* At most this many blocks are handed on in one poll, whatever nBlock is: older ones are skipped. A
 * ring of 8 blocks holds 42 ms; blocks older than 6 behind the counter are the ones the game is about
 * to overwrite. */
#define AUDIO_RING_MAX_CATCHUP 6u

typedef struct audio_ring_plan {
    uint64_t first;     /* counter value (block number since the port start) of the first block to hand on */
    uint32_t count;     /* blocks to hand on, first .. first+count-1; block n is ring slot n % nBlock */
    uint64_t skipped;   /* due blocks given up because the poll came too late (not part of first..) */
} audio_ring_plan;

/* `consumed` = blocks already handed on since the port start, `counter` = the service's block counter,
 * `n_block` = the port's ring size (8, 16, 32). A counter at or behind `consumed` plans nothing. */
void audio_ring_plan_blocks(uint64_t consumed, uint64_t counter, uint32_t n_block, audio_ring_plan *plan);

/* ---- 2. device queue ---- */

typedef struct audio_out_policy {
    uint32_t prefill;           /* silent blocks queued ahead of the first block and after an underrun */
    uint32_t max_depth;         /* queued blocks at which a new block is dropped */
    uint64_t idle_ns;           /* no block for this long = the stream stopped; the next block restarts it */
    int primed;                 /* a stream is running (set by the first plan after init or an idle gap) */
    uint64_t last_submit_ns;
} audio_out_policy;

typedef struct audio_out_submit_plan {
    uint32_t silence_before;    /* silent blocks to queue before the block */
    int underrun;               /* the device had run dry while the stream was running */
    int drop;                   /* do not queue the block: the queue is full */
} audio_out_submit_plan;

void audio_out_policy_init(audio_out_policy *policy, uint32_t prefill, uint32_t max_depth, uint64_t idle_ns);

/* Plans one block arriving at `now_ns` while `in_flight` blocks (silence included) are queued at the device.
 *   - first block, or the previous one is older than idle_ns: `prefill` silent blocks first (a restart,
 *     not an underrun);
 *   - a running stream and the queue is empty: underrun, `prefill` silent blocks first;
 *   - otherwise nothing before it;
 *   - and in every case `drop` when in_flight (plus the silence planned) has reached max_depth. */
void audio_out_policy_plan_submit(audio_out_policy *policy, uint32_t in_flight, uint64_t now_ns,
                                  audio_out_submit_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_OUT_POLICY_H */
