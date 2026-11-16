#ifndef NATIVE_STARTUP_CHILD_COMPOSITION_H
#define NATIVE_STARTUP_CHILD_COMPOSITION_H
// Composition glue between the generic sys_ppu_thread_create bridge (StartupPpuCreatePort) and up to STARTUP_CHILD_COMPOSITION_MAX_SLOTS
// (= 3) StartupChildPpu owners. Platform independent: every OS fact (host thread port, clock, sleep/poll) arrives through injected
// ports. This file adds POLICY only: explicit immutable per-slot request policy, request/status mapping, asynchronous latches, and
// bounded post-join teardown. StartupChildPpu owns each child's execution/lifecycle; this class owns only the STABLE FIXED slot array.
//
// Lifecycle (heap-allocate once, never copy/move; no destructor frees live resources; slots never relocate and are never reused):
//   1. configure(shared, bindings, count)   BEFORE the parent starts; count 1..STARTUP_CHILD_COMPOSITION_MAX_SLOTS (= 3). `shared`
//      carries only the shared domain/immutable ports; every per-child identity (service thread port, recorder, budget) is an explicit
//      StartupChildSlotBinding that is COPIED into the owned slot (the caller's array need not outlive the call; the pointed-to objects
//      must stay stable through teardown). Each slot gets its own prepublished dispatch-stop latch. configure(ports, policy) is the
//      legacy one-slot form (count = 1).
//   2. parent.ppu_create = create_port()    borrowed immutable; prepare/start/join then run on the PARENT host thread only.
//   3. while the parent runs: request_stop() is the ONLY entry point other threads may use. It publishes fixed atomics only (the
//      create-skip latch, the watchdog flag and EVERY slot's dispatch-stop latch, whether or not a slot is configured), so it never
//      reads a plain member and cannot race prepare/start/join. A child blocked in sc130 is NOT woken by it (that needs the owner cancel);
//      it is released by teardown() after the parent has been joined.
//   4. parent joined (join gives the happens-before for every plain member below) -> teardown(): assert every latch, capture each
//      slot's diagnostic status, cancel() EVERY slot that holds resources BEFORE the first reap, then one bounded reap poll/retry per
//      slot. A retained/still-running slot never blocks its siblings' release. released() is true only when EVERY slot is released;
//      until then the caller owns the whole shared graph (registry/LV2/memory/recorders/budgets) and must keep it alive, exiting the
//      process retaining everything if it never becomes true. Re-calling teardown() touches only unreleased slots (no second
//      cancel/reap/join of a released one).
//
// Provider policy (this is a DIAGNOSTIC provider for the first guest children):
//   Prepared  : the LOWEST free slot (never prepared before, even unsuccessfully) whose policy equals the guest request
//               (priority, creator priority, stack size). prepare_attempted is the reservation: it is set before any provider effect,
//               never cleared, and a slot is therefore never reused, not even after a failed or released child.
//   Unsupported: no free slot (totals().refused_no_slot) or free slots exist but none match (totals().policy_mismatch); both are
//               effect-free and recorded with the last requested priority/creator/stack/argument/output. Never invents a guest
//               errno (GuestError unused).
//   start     : routed by the EXACT prepared id. Zero/unknown ids touch no child (totals().unmatched_start). Started for an actual
//               Blocked child (a blocking slot), or - for a run_to_exit slot ONLY - when the child really ended itself through
//               sys_ppu_thread_exit (Exited, observed_exited). Running is NOT accepted (never invented). Returned/Terminal ->
//               Terminal (the parent stops ProviderFailure); everything else -> ProviderFailure. Failure retains the child.
//   run_to_exit (per-slot policy flag, NOT part of the policy match): the slot's child gets child_thread_exit_allowed, so a guest
//               sys_ppu_thread_exit ends it with StartupStop::GuestThreadExit instead of stopping UnsupportedSyscall. start() then runs
//               the child to its own exit within a bounded, INJECTED wait (exit_wait_poll / max_exit_polls) and reports Started only
//               for a final Exited. A child that blocks, or is still running after the bound, is a ProviderFailure that stays retained
//               for teardown().
//   join      : routed by the EXACT prepared id, like start(). A zero/unknown id is NotFound (totals().join_unmatched); a slot whose
//               observed_exited is false is NotExited (a blocked child is never joined); a slot already joined is AlreadyJoined.
//               Otherwise the slot's read-only child control() is consulted: has_guest_exit false is a ProviderFailure, true sets
//               report.joined and answers {Joined, guest_exit_value}. join() never cancels or reaps, and teardown() stays the only
//               release path.
#ifndef PS3_NATIVE_INDIRECT_DISPATCH
#error "startup_child_composition requires PS3_NATIVE_INDIRECT_DISPATCH"
#endif
#ifndef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
#error "startup_child_composition requires PS3_NATIVE_STARTUP_SPURS_INITIALIZE"
#endif
#include "startup_child_ppu.h"
#include <atomic>
#include <cstdint>

// Root (2026-11-15, Decision 119): called by prepare() with the slot's host thread-port context, BEFORE the child's host thread
// is created, with the guest's priority, thread name and entry. The composition (test_startup_indirect.cpp, Horizon) uses it to
// give guest audio threads a host priority above the other guest threads. Null (the default): no change.
extern void (*g_startup_child_priority_hook)(void *thread_context, int32_t guest_priority, const char *name, uint32_t entry);

// Every value is an explicit caller policy (no game address). The guest request must MATCH priority/creator/stack.
struct StartupChildCompositionPolicy {
    uint32_t kind = 0;                  // ps3_ppu_threads kind for the child service (must not collide with SPURS services)
    uint32_t priority = 0;              // expected guest priority of the child (first child: 249)
    uint32_t creator_priority = 0;      // expected creator (parent main) priority (first child: 250)
    uint32_t stack_size = 0;            // expected guest stack size (first child: 0x10000)
    uint64_t tls_capacity = 0;          // child TLS region bytes (first child: 0x10000)
    uint64_t return_lr = 0;             // explicit unreachable LR sentinel; NO exact ABI claim
    uint32_t stack_entry_slack = 0x70;  // bytes below the logical stack top
    // Behaviour appended AFTER the policy-match fields: these are NOT part of the match, which stays exactly
    // (priority, creator priority, stack size).
    bool run_to_exit = false;           // true: the slot's child may end itself (sys_ppu_thread_exit) and start() waits for it
    void *exit_wait_context = nullptr;  // INJECTED wait for run_to_exit: called between polls, never an unbounded block
    void (*exit_wait_poll)(void *) = nullptr;   // null = no wait (the polls then run back-to-back)
    unsigned max_exit_polls = 0;        // run_to_exit needs a finite nonzero bound (configure refuses run_to_exit with 0)
};
#ifdef PS3_NATIVE_LLE_PROFILE   // build-wide (see startup_capacity_policy.h); equals STARTUP_CHILD_SLOT_CAPACITY there
constexpr unsigned STARTUP_CHILD_COMPOSITION_MAX_SLOTS = 64;
#else
constexpr unsigned STARTUP_CHILD_COMPOSITION_MAX_SLOTS = 3;
#endif
// Explicit per-slot identity: a dedicated host thread port, a per-child recorder and budget, and the slot's request policy.
// Distinct slots must not alias a thread context, recorder or budget (configure refuses). The binding is copied by configure().
struct StartupChildSlotBinding {
    ps3_service_thread_port thread{};
    PpuMemoryTraceRecorder *recorder = nullptr;
    DiscoveryBudgetBinding *budget = nullptr;
    StartupChildCompositionPolicy policy{};
};
enum class StartupChildCompositionConfigure { Ok, InvalidArgument, AlreadyConfigured };
enum class StartupChildCompositionTeardown { NeverPrepared, Released, StillRunning, ServiceRetained, AdmissionRetained };
// Finite teardown bounds. poll(context) is the INJECTED sleep (Horizon svcSleepThread / host usleep); null = no wait.
// The bounds apply to EACH slot's own reap loop, so one teardown() call may wait up to as long in total as (slots) x these bounds;
// a re-called teardown() gets the bounds again for every slot it still has to finish.
struct StartupChildCompositionTeardownPolicy {
    void *context = nullptr;
    void (*poll)(void *context) = nullptr;
    unsigned max_reap_polls = 0;        // StillRunning answers tolerated before giving up
    unsigned max_stage_retries = 0;     // *Retained answers retried
};
// Everything below is valid only AFTER the parent was joined (plain members, ordered by that join).
struct StartupChildCompositionReport {
    bool configured = false;
    // Calls attributed to THIS slot: the prepare that reserved it and the start routed to its exact id. A call no slot was reserved for is
    // counted here only by a ONE-slot composition (every call is then slot 0's: the legacy contract); with several slots see totals().
    unsigned prepare_calls = 0, start_calls = 0;
    bool prepare_attempted = false;                         // StartupChildPpu::prepare reached
    StartupChildPpuPrepareResult prepare{};
    StartupPpuCreatePrepareStatus mapped_prepare = StartupPpuCreatePrepareStatus::ProviderFailure;
    uint32_t prepared_id = 0;
    bool start_attempted = false;                           // StartupChildPpu::start reached
    bool start_skipped_by_cancel_latch = false;
    StartupChildPpuStartResult start{};
    StartupPpuCreateStartStatus mapped_start = StartupPpuCreateStartStatus::ProviderFailure;
    bool observed_blocked = false;                          // start.status == Blocked (the ONLY accepted first state)
    bool observed_background = false;                       // start.status == Background: the child was started without the prefix wait
    bool stop_requested_by_watchdog = false;                // request_stop() was called
    bool latches_asserted_by_teardown = false;
    bool teardown_ran = false;
    StartupChildPpuStatus before{};                         // captured after the parent join, BEFORE cancel
    StartupChildPpuStatus after{};                          // captured after the last reap attempt
    StartupChildPpuReapResult reap{};
    unsigned reap_calls = 0, still_running_polls = 0, stage_retries = 0;
    StartupChildCompositionTeardown teardown = StartupChildCompositionTeardown::NeverPrepared;
    // Teardown sequencing proof (one counter shared by all slots, never reset): the sequence number at which this slot's FIRST cancel()
    // was issued and at which its first reap() was issued (0 = never). Both are set once and kept: a re-called teardown() still re-issues
    // cancel() on a retained / still-running slot, but does not renumber it. Every first cancel precedes every first reap iff
    // max(cancel_sequence) < min(first_reap_sequence) over the slots that hold resources.
    unsigned cancel_sequence = 0, first_reap_sequence = 0;
    // run_to_exit evidence (appended; zeroed for a blocking slot). start stays the FIRST start() result; final_start is the last
    // result start()/poll_exit() returned, i.e. the state this composition acted on. exit_polls counts poll_exit() calls only.
    bool observed_exited = false;                           // the child really ended through sys_ppu_thread_exit
    unsigned exit_polls = 0;
    StartupChildPpuStartResult final_start{};
    // lv2 syscall 44 (sys_ppu_thread_join) evidence (appended). join_calls counts the joins this composition routed to this slot;
    // joined is set once and kept, so a second join of the same id is AlreadyJoined rather than a second success.
    unsigned join_calls = 0;
    bool joined = false;
};
enum class StartupChildRefusalReason { None, NoSlot, PolicyMismatch };
// The LAST effect-free refusal (no free slot, or free slots but a policy mismatch) and the request that caused it.
struct StartupChildCompositionRefusal {
    StartupChildRefusalReason reason = StartupChildRefusalReason::None;
    unsigned free_slots = 0;                                // configured slots never prepared at refusal time
    int32_t priority = 0, creator_priority = 0;
    uint32_t stack_size = 0, output = 0, entry = 0;
    uint64_t argument = 0;
};
// Composition-wide counters (valid after the parent join, like report()). Every field is something this class counted itself.
struct StartupChildCompositionTotals {
    unsigned slot_count = 0;
    unsigned prepare_calls = 0, start_calls = 0;            // every call that reached the port, attributed or not
    unsigned refused_no_slot = 0, policy_mismatch = 0;      // effect-free Unsupported refusals
    unsigned refused_stopped = 0, refused_invalid = 0;      // ProviderFailure before any slot was reserved (latch set / null request or TLS)
    unsigned unmatched_start = 0;                           // start(id) with a zero, unknown or never-prepared id: no child touched
    // lv2 syscall 44 (sys_ppu_thread_join): every join that reached the port, and the ones that named no prepared slot.
    unsigned join_calls = 0, join_unmatched = 0;
    StartupChildCompositionRefusal last_refusal{};
};
// Public diagnostic name of a teardown outcome.
const char *startup_child_teardown_name(StartupChildCompositionTeardown status);

class StartupChildComposition {
public:
    StartupChildComposition() = default;
    StartupChildComposition(const StartupChildComposition &) = delete;
    StartupChildComposition &operator=(const StartupChildComposition &) = delete;
    // No destructor releasing live context by design.

    // `shared` carries only the shared fields (threads, queues, memory, code, clock, dispatch_limit); its per-child fields (thread,
    // recorder, budget, dispatch_stop) must be EMPTY. count must be 1..STARTUP_CHILD_COMPOSITION_MAX_SLOTS and `slots` must name
    // `count` fully valid bindings (null recorder/budget or thread start/join, an invalid policy, or aliased thread contexts/
    // recorders/budgets are refused; with count > 1 a thread context must also be non-null). Nothing changes on a refusal. Not
    // callable while the parent runs. All pointed-to objects are borrowed through teardown (see StartupChildPpuPorts).
    StartupChildCompositionConfigure configure(const StartupChildPpuPorts &shared, const StartupChildSlotBinding *slots, unsigned count);
    // Legacy one-slot form (existing callers): `ports.dispatch_stop` must be EMPTY, this object installs its own per-child latch.
    StartupChildCompositionConfigure configure(const StartupChildPpuPorts &ports, const StartupChildCompositionPolicy &policy);
    // Immutable borrowed port for StartupExecution::ppu_create; null until configure() succeeded.
    const StartupPpuCreatePort *create_port() const { return configured_ ? &port_ : nullptr; }
    // Thread-safe, any thread, any time: only stores fixed atomics (create-skip, watchdog flag, every slot's dispatch-stop).
    // Never an owner method and never reads a plain member (so it needs no knowledge of the configured slot count).
    void request_stop()
    {
        watchdog_called_.store(true, std::memory_order_release); create_cancel_.store(true, std::memory_order_release);
        for (Slot &slot : slots_) slot.dispatch_stop.store(true, std::memory_order_release);
    }
    // After the parent was joined: single caller. Re-callable to continue a StillRunning/*Retained outcome (counters accumulate);
    // released slots are skipped. Returns slot 0's report (legacy contract); use report(i)/released() for the whole picture.
    const StartupChildCompositionReport &teardown(const StartupChildCompositionTeardownPolicy &policy);
    // True only after a teardown() that left nothing owned in ANY slot (each Released, or NeverPrepared: it never reserved anything).
    bool released() const;
    // Slot 0 detail (the legacy contract; for a one-slot composition it is the only slot). Refreshes the watchdog flag from its atomic.
    // Plain fields are valid only after the parent was joined (or from the parent thread).
    const StartupChildCompositionReport &report();
    // Per-slot detail; an index outside 0..MAX-1 or a slot that was not configured returns an empty, configured=false report.
    const StartupChildCompositionReport &report(unsigned slot);
    const StartupChildCompositionTotals &totals() const { return totals_; }
    unsigned slot_count() const { return slot_count_; }
    // READ-ONLY child access for diagnostics ONLY after the parent join. There is deliberately no mutable access: cancel/reap
    // belong to teardown() so that ordering, retention and reporting stay with this composition. nullptr outside 0..MAX-1.
    const StartupChildPpu &child() const { return slots_[0].child; }
    const StartupChildPpu *child(unsigned slot) const { return slot < STARTUP_CHILD_COMPOSITION_MAX_SLOTS ? &slots_[slot].child : nullptr; }

private:
    struct Slot {
        StartupChildPpu child;
        StartupChildPpuPorts ports{};
        StartupChildCompositionPolicy policy{};
        StartupChildCompositionReport report{};
        bool before_captured = false;
        std::atomic<bool> dispatch_stop{false};
    };
    static StartupPpuCreatePrepareResult prepare_thunk(void *context, const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls);
    static StartupPpuCreateStartStatus start_thunk(void *context, uint32_t thread_id);
    static StartupPpuJoinResult join_thunk(void *context, uint32_t thread_id);
    static ps3_compiled_dispatch_stop_reason stop_thunk(void *context);   // context = the Slot (its own atomic only)
    StartupPpuCreatePrepareResult prepare(const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls);
    StartupPpuCreateStartStatus start(uint32_t thread_id);
    StartupPpuJoinResult join(uint32_t thread_id);
    bool slot_done(const Slot &slot) const;                // teardown ran and left nothing owned
    void reap_slot(Slot &slot, const StartupChildCompositionTeardownPolicy &policy);

    Slot slots_[STARTUP_CHILD_COMPOSITION_MAX_SLOTS];      // stable storage: no vector, no relocation, never reused
    StartupPpuCreatePort port_{};
    StartupChildCompositionTotals totals_{};
    unsigned slot_count_ = 0, teardown_sequence_ = 0;
    bool configured_ = false, teardown_ran_ = false;
    std::atomic<bool> create_cancel_{false}, watchdog_called_{false};
    // Root (2026-11-12, Decision 68): any guest thread may create threads, so the slot reservation in prepare() and
    // the routing in start()/join() are serialized. Held only over the table scan, never over a child's own work.
    std::atomic_flag table_lock_ = ATOMIC_FLAG_INIT;
};
#endif
