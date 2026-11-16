#ifndef SPURS_LFQUEUE_ATTACH_SDK25_H
#define SPURS_LFQUEUE_ATTACH_SDK25_H
/* Platform-free, instance-free contract for the FOURTH SDK branch of
 *
 *   cellSpursLFQueueAttachLv2EventQueue   NID 0x1656D49F
 *
 * i.e. the arm this entry takes when the EFFECTIVE PROCESS SDK VERSION is above 0x18FFFF.
 *
 * Behaviour taken from the firmware library libsre,
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6; nothing decrypted).
 *
 * WHY THIS UNIT EXISTS. b1/i1 decoded the main loop and G15's root review (decision 25) found it is
 * only the arm the middle SDK range selects, which is reached when `0x17FFFF < sdk <= 0x18FFFF`
 * (the process SDK version is compared unsigned against `0x17FFFF` and then `0x18FFFF`).
 * inFAMOUS's process SDK is **0x00250001**
 * (its own logs: `effective_sdk=0x00250001`), so the final greater-than branch is TAKEN
 * and the whole older loop never runs.
 *
 * WHAT THE SDK-25 ARM DOES, decoded for the game's line:
 *
 *   the line's DIRECTION word (`+0x24`) is read (r29 = the entry's own r3)
 *   the running code is 0x80410909
 *   direction word != 0 -> continue with the shape gates below
 *   direction word == 0 -> return 0x80410909
 *   ...
 *   direction == 3 -> the SPURS shape gate
 *   else the CellSync shape gate: the line's `+0x20` word
 *   != 0xFFFFFFFF -> 0x8041090F
 *   loop index = 0
 *   the record stride = 0
 *   enter the SHARED loop
 *
 * So the SDK-25 branch does NOT own a loop of its own: it OWNS the preconditions (the direction gate
 * and the two shape gates) and then hands the direction word to the
 * same loop body the other branches use, which is why `ps3_spurs_lfqa_plan_build()` is reused
 * for the actions and the publication rather than duplicated. The one word that differs is the one the
 * loop is fed: for direction 3 (`+0x24 == 3`) the trip count is 2 and each iteration runs
 * each iteration runs the queue-create call (0xB244) and the dynamic connect (0xB0D0 -> 0xAF2C), then
 * the SPU status, group-id and start syscalls (134, 136, 137).
 *
 * THE OWNER RESOLUTION RUNS ON THIS ARM TOO (decision 27, correcting G15 §5). The load of the line's `+0x74` word
 * and the whole owner-resolution chain sit BEFORE the SDK compare, so this arm does not
 * skip them: `r28` is the resolved CellSpurs EA (untagged) and `0xAF2C` connects to exactly that. G15's
 * reading -- that the instance came from the line's `+0x60` -- is withdrawn: the predecessor zeroes
 * `queue+0x50..0x6E`, so `+0x60` reads 0 on the game's real queue.
 *
 * THE HARD CONSTRAINT. The game's own check on the result sends a NON-ZERO r3 to an error path,
 * which calls `func_00452290` (an unconditional trap). So on the game's
 * line the result must be 0, and this unit shows it is: direction 3, `+0x20`/`+0x21` = 0xFF (the
 * builder stores both), both iterations succeed, and the SPURS-shape publication arm sets
 * `r31 = 0` before the shared epilogue sign-extends it into r3.
 *
 * Linkage: PS3_SPURS_LFQA_FN (the same macro as the base header), so one implementation and no
 * header/object divergence. Include AFTER spurs_lfqueue_attach.h.
 *
 * No OS calls, no allocation, no globals, no guest-memory access, no libc. */

#include "spurs_lfqueue_attach.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------ */
/* The process-SDK branch table                                                           */
/* ------------------------------------------------------------------------------------ */

/* The two bounds are `0x17FFFF` and `0x18FFFF`. */
#define PS3_SPURS_LFQA_SDK_BRANCH1_MAX 0x17FFFFu
#define PS3_SPURS_LFQA_SDK_BRANCH2_MAX 0x18FFFFu

/* Not a result code: the refusal word before the direction gate runs. */
#define PS3_SPURS_LFQA_DIRECTION_ZERO 0x80410909u

typedef enum spurs_lfqa_sdk_branch {
    /* sdk <= 0x17FFFF: the first bound is NOT passed -> the legacy shape selector and its
     * dump path (the pre-0x180000 arms). No loop on the game's shapes. */
    PS3_SPURS_LFQA_SDK_BRANCH_LEGACY = 0,
    /* 0x17FFFF < sdk <= 0x18FFFF: the middle arm -- the one b1/i1 decoded. */
    PS3_SPURS_LFQA_SDK_BRANCH_180000 = 1,
    /* sdk > 0x18FFFF: the arm EVERY modern title takes, inFAMOUS included. */
    PS3_SPURS_LFQA_SDK_BRANCH_250001 = 2
} spurs_lfqa_sdk_branch;

/* The branch selection itself: the SDK version is compared against 0x17FFFF, then
 * against 0x18FFFF. Both compares are UNSIGNED. */
PS3_SPURS_LFQA_FN spurs_lfqa_sdk_branch ps3_spurs_lfqa_sdk_branch_of(uint32_t sdk_version)
{
    if (sdk_version > PS3_SPURS_LFQA_SDK_BRANCH2_MAX) return PS3_SPURS_LFQA_SDK_BRANCH_250001;
    if (sdk_version > PS3_SPURS_LFQA_SDK_BRANCH1_MAX) return PS3_SPURS_LFQA_SDK_BRANCH_180000;
    return PS3_SPURS_LFQA_SDK_BRANCH_LEGACY;
}

/* ------------------------------------------------------------------------------------ */
/* The SDK-25 precondition chain                                               */
/* ------------------------------------------------------------------------------------ */

/* Runs, in the firmware's order, exactly what the SDK-25 arm does before it rejoins the shared loop:
 *
 *   0. the owner resolution, which the firmware performs BEFORE the SDK compare and
 *      therefore before everything below: a word 0 or a resolved 0 returns
 *      0x80410902 without reading `+0x24` at all;
 *   1. read the line's `+0x24` word and compare it with 0:
 *      a line whose direction word is ZERO returns PS3_SPURS_LFQA_DIRECTION_ZERO and performs
 *      nothing at all (it goes straight to the epilogue with r31 = 0x80410909);
 *   2. compare the word with 3: the SPURS arm, or the CellSync arm,
 *      which is `ps3_spurs_lfqa_shape_gate()` verbatim (the SPURS arm tests `+0x20` then `+0x21`, the
 *      CellSync arm tests the `+0x20` word against -1);
 *   3. on success the plan's shape, trip count and actions are the base unit's.
 *
 * `tag` is the word at `owner+0x64` when the owner is untagged (the adapter's single guest read), and is
 * unused for a tagged owner. `plan->group` is the resolved CellSpurs EA `0xAF2C` connects to, exactly as
 * ps3_spurs_lfqa_plan_build() computes it for the other arms.
 *
 * On STATUS_REFUSED `*r3` holds the firmware's code and nothing was planned. */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_plan_build_sdk25(const spurs_lfqa_request *request,
    uint32_t tag, uint32_t port2, spurs_lfqa_plan *plan)
{
    if (!plan) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    for (size_t index = 0; index < sizeof(*plan); ++index) ((unsigned char *)plan)[index] = 0;
    plan->status = PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
    if (!request) return PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;

    uint32_t r3 = 0;
    if (ps3_spurs_lfqa_validate(request, &r3) != PS3_SPURS_LFQA_STATUS_OK) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = r3;
        plan->owner_phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }

    /* The owner resolution. It sits BETWEEN the two entry gates above and the SDK
     * compare, so on THIS arm it also precedes the direction gate and both shape gates
     * (the zero test and the shape tests), which the firmware only reaches after it. `r28` is the
     * CellSpurs EA 0xAF2C connects to, on this arm as on the others. Its refusal (word 0, or a
     * resolved 0) therefore wins over 0x80410909 and 0x8041090F, and -- like every refusal here -- it
     * precedes the firmware's first store, so it is answered as an effect-free
     * refusal with the resolution's own code. */
    {
        uint32_t owner_ea = 0;
        spurs_lfqa_owner_phase phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        uint32_t owner_code = 0;
        if (ps3_spurs_lfqa_owner_tagged(request->owner, tag, &owner_ea, &phase, &owner_code) !=
            PS3_SPURS_LFQA_STATUS_OK) {
            plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
            plan->r3 = owner_code;
            plan->owner_phase = phase;
            plan->actions_count = 0;
            return PS3_SPURS_LFQA_STATUS_REFUSED;
        }
        plan->owner_phase = phase;
        plan->group = owner_ea;
    }

    /* 0x17780 / 0x17784 / 0x17788. The code word 0x80410909 is assembled BEFORE the test, exactly as
     * 0x1777C/0x17784 do it, so the zero-direction return is that word verbatim. */
    if (request->version == 0u) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = PS3_SPURS_LFQA_DIRECTION_ZERO;
        plan->owner_phase = PS3_SPURS_LFQA_OWNER_PHASE_A;
        plan->actions_count = 0;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }

    /* the same two gates, in the same order, with the same codes. */
    if (ps3_spurs_lfqa_shape_gate(request, port2, &r3) != PS3_SPURS_LFQA_STATUS_OK) {
        plan->status = PS3_SPURS_LFQA_STATUS_REFUSED;
        plan->r3 = r3;
        plan->actions_count = 0;
        return PS3_SPURS_LFQA_STATUS_REFUSED;
    }

    plan->spurs_shape = ps3_spurs_lfqa_is_spurs_shape(request->version);
    plan->iterations = ps3_spurs_lfqa_iterations(request->version);

    /* 0xAF2C's requested-port bound is not reachable here: the SDK-25 branch is the `r6 = 1`
     * (dynamic) arm, where the mask is built from the constants 63 and 16 and
     * the requested-port byte is never read. The bound is on that byte,
     * so it cannot fire. Stated, not skipped silently: the plan still carries the mask so the
     * adapter can assert it equals PS3_SPURS_LFQA_EVENT_MASK. */
    plan->actions_count = plan->iterations;
    for (uint32_t index = 0; index < plan->iterations && index < PS3_SPURS_LFQA_MAX_ACTIONS; ++index) {
        plan->actions[index].iteration = index;
        plan->actions[index].kind = plan->spurs_shape ? PS3_SPURS_LFQA_ACTION_CREATE_AND_ATTACH
                                                      : PS3_SPURS_LFQA_ACTION_ATTACH_ONLY;
        plan->actions[index].queue = plan->spurs_shape ? 0u : request->recorded_queue;
        plan->actions[index].mask = ps3_spurs_lfqa_event_mask(PS3_SPURS_LFQA_EVENT_PORT_WILDCARD);
    }
    plan->status = PS3_SPURS_LFQA_STATUS_OK;
    plan->r3 = PS3_SPURS_LFQA_OK;
    return PS3_SPURS_LFQA_STATUS_OK;
}

/* ------------------------------------------------------------------------------------ */
/* The CellSpurs the connect targets (decision 27)                                        */
/* ------------------------------------------------------------------------------------ */

/* The SDK-25 arm does NOT read a CellSpurs EA from the line. G15 believed it came from the object's
 * `+0x60`; decision 27 withdrew that: the entry's own owner resolution
 * runs BEFORE the SDK compare, so every arm -- this
 * one included -- connects to the same `r28`:
 *
 *   r0  = owner & 0xF
 *   r28 = owner & ~0xF                     (tagged owner)
 *   nibble != 0 -> r28 is the CellSpurs EA
 *   nibble == 0 and word == 0 -> refusal, code 0x80410902
 *   untagged: r28 = the taskset's own CellSpurs, low word of its u64
 *   r28 == 0 -> refusal, code 0x80410902
 *
 * Its tag meaning is the INIT's: a tagged (`|1`) owner is a CellSpurs and an untagged one is a
 * taskset -- the opposite of the G14 labels. The adapter performs that resolution and hands the result to
 * 0xAF2C; a resolved value this runtime does not own is the honest ProviderFailure. Nothing is fabricated. */
#define PS3_SPURS_LFQA_CELLSPURS_EA 0x60u   /* NOT used by this entry; see above (decision 27) */

/* ------------------------------------------------------------------------------------ */
/* The SDK-25 entry point                                                                 */
/* ------------------------------------------------------------------------------------ */

/* The whole attach entry with the branch table folded in, so one call serves every SDK:
 *
 *   sdk > 0x18FFFF     ps3_spurs_lfqa_plan_build_sdk25()
 *   otherwise          ps3_spurs_lfqa_plan_build()            (b1's decode)
 *
 * The gates that precede the branch (alignment, NULL) are executed by whichever builder
 * runs, in the branch's own order. The owner resolution runs before the SDK compare
 * on EVERY arm and is therefore the ADAPTER's step, not this table's (decision 27): this ARM never reads
 * or writes the resolved connect target. */
PS3_SPURS_LFQA_FN spurs_lfqa_status ps3_spurs_lfqa_plan_build_for_sdk(const spurs_lfqa_request *request,
    uint32_t tag, uint32_t port2, spurs_lfqa_plan *plan)
{
    if (ps3_spurs_lfqa_sdk_branch_of(request ? request->sdk_version : 0u) ==
        PS3_SPURS_LFQA_SDK_BRANCH_250001)
        return ps3_spurs_lfqa_plan_build_sdk25(request, tag, port2, plan);
    ps3_spurs_lfqa_plan_build(request, tag, port2, plan);
    return plan ? plan->status : PS3_SPURS_LFQA_STATUS_BAD_ARGUMENT;
}

#ifdef __cplusplus
}
#endif
#endif
