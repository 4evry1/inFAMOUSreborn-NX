/* Platform-free output decisions. See audio_out_policy.h for the contract. */
#include "audio_out_policy.h"

#include <string.h>

void audio_ring_plan_blocks(uint64_t consumed, uint64_t counter, uint32_t n_block, audio_ring_plan *plan)
{
    uint64_t behind;
    uint64_t keep = AUDIO_RING_MAX_CATCHUP;
    plan->first = consumed;
    plan->count = 0;
    plan->skipped = 0;
    if (counter <= consumed) return;
    behind = counter - consumed;
    /* never reach back into the slots the game is about to overwrite: the last two slots of the ring are the
     * ones it fills next */
    if (n_block > 2u && (uint64_t)(n_block - 2u) < keep) keep = n_block - 2u;
    if (behind > keep) {
        plan->skipped = behind - keep;
        plan->first = counter - keep;
        plan->count = (uint32_t)keep;
    } else {
        plan->count = (uint32_t)behind;
    }
}

void audio_out_policy_init(audio_out_policy *policy, uint32_t prefill, uint32_t max_depth, uint64_t idle_ns)
{
    memset(policy, 0, sizeof *policy);
    policy->prefill = prefill;
    policy->max_depth = max_depth;
    policy->idle_ns = idle_ns;
}

void audio_out_policy_plan_submit(audio_out_policy *policy, uint32_t in_flight, uint64_t now_ns,
                                  audio_out_submit_plan *plan)
{
    const int idle = !policy->primed || (now_ns > policy->last_submit_ns && now_ns - policy->last_submit_ns > policy->idle_ns);
    plan->silence_before = 0;
    plan->underrun = 0;
    plan->drop = 0;
    if (idle) {
        plan->silence_before = policy->prefill;
        policy->primed = 1;
    } else if (in_flight == 0u) {
        plan->underrun = 1;
        plan->silence_before = policy->prefill;
    }
    policy->last_submit_ns = now_ns;
    if ((uint64_t)in_flight + plan->silence_before >= policy->max_depth) {
        plan->silence_before = 0;
        plan->drop = 1;
        plan->underrun = 0;
    }
}
