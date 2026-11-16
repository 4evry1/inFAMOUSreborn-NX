#include "startup_child_composition.h"
/* Root diagnostic (racy on purpose): the stage of the most recent guest thread create. Global scope, C++ linkage. */
volatile uint32_t g_startup_create_stage = 0;
/* Root (2026-11-15, Decision 119): see prepare(). Global scope, C++ linkage. */
void (*g_startup_child_priority_hook)(void *thread_context, int32_t guest_priority, const char *name, uint32_t entry) = nullptr;
#ifdef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
namespace {
StartupPpuCreatePrepareStatus startup_child_map_prepare(StartupChildPpuPrepareStatus status)
{
    switch (status) {
    case StartupChildPpuPrepareStatus::Ok: return StartupPpuCreatePrepareStatus::Prepared;
    case StartupChildPpuPrepareStatus::NotEmpty: return StartupPpuCreatePrepareStatus::Unsupported;   // a second child is outside the policy
    case StartupChildPpuPrepareStatus::InvalidArgument:
    case StartupChildPpuPrepareStatus::InvalidLayout:
    case StartupChildPpuPrepareStatus::ReserveFailed:
    case StartupChildPpuPrepareStatus::StorageFailed:
    case StartupChildPpuPrepareStatus::TlsFailed:
    case StartupChildPpuPrepareStatus::AdmissionFailed: break;
    }
    return StartupPpuCreatePrepareStatus::ProviderFailure;   // never a guessed guest errno; out-of-range values fail closed
}
StartupPpuCreateStartStatus startup_child_map_start(StartupChildPpuStartStatus status)
{
    switch (status) {
    case StartupChildPpuStartStatus::Blocked: return StartupPpuCreateStartStatus::Started;   // the ONLY accepted first blocking state
    case StartupChildPpuStartStatus::Background: return StartupPpuCreateStartStatus::Started;
    case StartupChildPpuStartStatus::Returned:
    case StartupChildPpuStartStatus::Terminal: return StartupPpuCreateStartStatus::Terminal;
    case StartupChildPpuStartStatus::Running:   // never invented as Started
    case StartupChildPpuStartStatus::NotReserved:
    case StartupChildPpuStartStatus::Cancelled:
    case StartupChildPpuStartStatus::StartFailed:
    case StartupChildPpuStartStatus::OwnedFailure:
    case StartupChildPpuStartStatus::Failed:
    case StartupChildPpuStartStatus::Exited:   // only a run_to_exit slot accepts this, through its own bounded wait
    break;
    }
    return StartupPpuCreateStartStatus::ProviderFailure;
}
}   // namespace
const char *startup_child_teardown_name(StartupChildCompositionTeardown status)
{
    switch (status) {
    case StartupChildCompositionTeardown::NeverPrepared: return "never-prepared";
    case StartupChildCompositionTeardown::Released: return "released";
    case StartupChildCompositionTeardown::StillRunning: return "still-running";
    case StartupChildCompositionTeardown::ServiceRetained: return "service-retained";
    case StartupChildCompositionTeardown::AdmissionRetained: return "admission-retained";
    }
    return "unknown";
}

// A waiter on the slot-table lock must let any thread run (Decision 64: no pure spinning on Horizon).
#if defined(__SWITCH__)
extern "C" void svcSleepThread(int64_t nano);
static inline void startup_child_table_relax() { svcSleepThread(INT64_C(50000)); }
#else
#include <thread>
static inline void startup_child_table_relax() { std::this_thread::yield(); }
#endif
namespace {
// An effect-free slot "free" means never prepared; the policy match is exactly (priority, creator priority, stack size).
bool startup_child_policy_matches(const StartupChildCompositionPolicy &policy, const StartupPpuCreateRequest &request)
{
#ifdef PS3_NATIVE_LLE_SRE
    /* Root (B2b Eden): Sony's lifted libsre creates its SPURS handler PPU thread with stack 0x8000
     * (ppu_recomp_000.cpp:14049-14056); in libsre the handler thread itself issues
     * sys_spu_thread_group_start. A slot's stack_size is the capacity it reserves; the child is built
     * with the REQUESTED size (prepare: child.stack_size = request->stack_size), so any request that
     * fits is honoured exactly. Priority and creator priority still match exactly. */
    /* Root (C1b hardware, 2026-11-11): the game creates a thread with priority 99, stack 0x8000 (entry 0x00442790,
     * from 0x4432B4). The slot's priority was a record of the first requests seen, not a requirement: an ordinary
     * slot takes any child with a valid priority that fits. A child that outranks its creator keeps the prefix wait;
     * an equal or lower one is started in the background. The child is still built with the REQUESTED priority and
     * stack. A run-to-exit slot keeps its exact priority (it is the fenced, lower-priority case). */
    /* Root (2026-11-12, Decision 68): the creator's priority is not a requirement either. The slots were recorded
     * with the main thread's priority because only it created threads; the game's movie reader (a child) creates
     * Bink's worker thread. Any creator with a valid priority is accepted. */
    if (request.creator_priority < 0 || request.creator_priority > 3071 || request.stack_size == 0 ||
        request.stack_size > policy.stack_size) return false;
    if (policy.run_to_exit) return uint32_t(request.priority) == policy.priority;
    return request.priority >= 0 && request.priority <= 3071;
#else
    return uint32_t(request.priority) == policy.priority && uint32_t(request.creator_priority) == policy.creator_priority &&
           request.stack_size == policy.stack_size;
#endif
}
bool startup_child_policy_valid(const StartupChildCompositionPolicy &policy)
{
    if (policy.run_to_exit && policy.max_exit_polls == 0) return false;   // a run-to-exit wait without a bound is refused
    return policy.priority < 3072 && policy.creator_priority < 3072 && policy.stack_size != 0 && policy.tls_capacity != 0 &&
           policy.return_lr != 0 && policy.stack_entry_slack != 0;
}
}   // namespace

StartupChildCompositionConfigure StartupChildComposition::configure(const StartupChildPpuPorts &shared, const StartupChildSlotBinding *slots, unsigned count)
{
    if (configured_) return StartupChildCompositionConfigure::AlreadyConfigured;
    if (!slots || count == 0 || count > STARTUP_CHILD_COMPOSITION_MAX_SLOTS) return StartupChildCompositionConfigure::InvalidArgument;
    // shared capabilities only: every per-child capability arrives through a binding (and the dispatch-stop is installed here)
    if (!shared.threads || !shared.queues || !shared.memory || !shared.code || !shared.clock || shared.dispatch_limit == 0 ||
        shared.thread.context || shared.thread.start || shared.thread.join || shared.recorder || shared.budget ||
        shared.dispatch_stop.context || shared.dispatch_stop.stop_requested)
        return StartupChildCompositionConfigure::InvalidArgument;
    for (unsigned i = 0; i < count; ++i) {
        const StartupChildSlotBinding &b = slots[i];
        if (!b.thread.start || !b.thread.join || !b.recorder || !b.budget || !startup_child_policy_valid(b.policy) ||
            (count > 1 && !b.thread.context))   // a null context could alias a sibling: with one slot it is the legacy shape
            return StartupChildCompositionConfigure::InvalidArgument;
        for (unsigned j = 0; j < i; ++j)
            if ((b.thread.context && b.thread.context == slots[j].thread.context) || b.recorder == slots[j].recorder || b.budget == slots[j].budget)
                return StartupChildCompositionConfigure::InvalidArgument;   // independent stable thread/recorder/budget per slot
    }
    // validated: install (copy) everything; nothing above changed any state
    for (unsigned i = 0; i < count; ++i) {
        Slot &slot = slots_[i];
        slot.ports = shared;
        slot.ports.thread = slots[i].thread;
        slot.ports.recorder = slots[i].recorder;
        slot.ports.budget = slots[i].budget;
        slot.ports.dispatch_stop = {&slot, &StartupChildComposition::stop_thunk};   // the per-child, prepublished asynchronous stop
        slot.policy = slots[i].policy;
        slot.report.configured = true;
    }
    slot_count_ = count;
    totals_.slot_count = count;
    port_ = {this, &StartupChildComposition::prepare_thunk, &StartupChildComposition::start_thunk, &StartupChildComposition::join_thunk};
    configured_ = true;
    return StartupChildCompositionConfigure::Ok;
}

StartupChildCompositionConfigure StartupChildComposition::configure(const StartupChildPpuPorts &ports, const StartupChildCompositionPolicy &policy)
{
    if (configured_) return StartupChildCompositionConfigure::AlreadyConfigured;
    if (ports.dispatch_stop.context || ports.dispatch_stop.stop_requested) return StartupChildCompositionConfigure::InvalidArgument;
    StartupChildPpuPorts shared = ports;
    shared.thread = {}; shared.recorder = nullptr; shared.budget = nullptr;
    const StartupChildSlotBinding binding{ports.thread, ports.recorder, ports.budget, policy};
    return configure(shared, &binding, 1);
}

ps3_compiled_dispatch_stop_reason StartupChildComposition::stop_thunk(void *context)
{
    return static_cast<Slot *>(context)->dispatch_stop.load(std::memory_order_acquire)
        ? PS3_COMPILED_DISPATCH_CANCELLED : PS3_COMPILED_DISPATCH_CONTINUE;
}
StartupPpuCreatePrepareResult StartupChildComposition::prepare_thunk(void *context, const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls)
{
    return static_cast<StartupChildComposition *>(context)->prepare(request, tls);
}
StartupPpuCreateStartStatus StartupChildComposition::start_thunk(void *context, uint32_t thread_id)
{
    return static_cast<StartupChildComposition *>(context)->start(thread_id);
}
StartupPpuJoinResult StartupChildComposition::join_thunk(void *context, uint32_t thread_id)
{
    return static_cast<StartupChildComposition *>(context)->join(thread_id);
}

StartupPpuCreatePrepareResult StartupChildComposition::prepare(const StartupPpuCreateRequest *request, const Ps3TlsTemplate *tls)
{
    ++totals_.prepare_calls;
    StartupPpuCreatePrepareResult out;
    // A call that no slot was reserved for is slot 0's in a ONE-slot composition (the legacy report contract) and totals-only otherwise.
    auto refuse = [&](StartupPpuCreatePrepareStatus status) {
        if (slot_count_ == 1) { ++slots_[0].report.prepare_calls; slots_[0].report.mapped_prepare = status; }
        out.status = status;
        return out;
    };
    if (!request || !tls) { ++totals_.refused_invalid; return refuse(StartupPpuCreatePrepareStatus::ProviderFailure); }
    if (create_cancel_.load(std::memory_order_acquire)) { ++totals_.refused_stopped; return refuse(StartupPpuCreatePrepareStatus::ProviderFailure); }   // before any reservation
    // reserve the LOWEST free slot (never prepared, not even unsuccessfully) whose policy matches
    while (table_lock_.test_and_set(std::memory_order_acquire)) startup_child_table_relax();
    struct TableUnlock { std::atomic_flag &flag; bool held; ~TableUnlock() { if (held) flag.clear(std::memory_order_release); } } table_unlock{table_lock_, true};
    Slot *run_to_exit_match = nullptr, *ordinary_match = nullptr;
    unsigned free_slots = 0;
    for (unsigned i = 0; i < slot_count_; ++i) {
        Slot &slot = slots_[i];
        if (slot.report.prepare_attempted) continue;
        ++free_slots;
        if (!startup_child_policy_matches(slot.policy, *request)) continue;
        if (slot.policy.run_to_exit) { if (!run_to_exit_match) run_to_exit_match = &slot; }
        else if (!ordinary_match) ordinary_match = &slot;
    }
    Slot *chosen = run_to_exit_match ? run_to_exit_match : ordinary_match;
    if (!chosen) {   // effect-free: occupancy and policy are told apart, with the request that was refused
        const bool no_slot = free_slots == 0;
        ++(no_slot ? totals_.refused_no_slot : totals_.policy_mismatch);
        StartupChildCompositionRefusal &last = totals_.last_refusal;
        last.reason = no_slot ? StartupChildRefusalReason::NoSlot : StartupChildRefusalReason::PolicyMismatch;
        last.free_slots = free_slots;
        last.priority = request->priority; last.creator_priority = request->creator_priority; last.stack_size = request->stack_size;
        last.output = request->output; last.entry = request->entry; last.argument = request->argument;
        return refuse(StartupPpuCreatePrepareStatus::Unsupported);
    }
    Slot &slot = *chosen;
    ++slot.report.prepare_calls;
    slot.report.prepare_attempted = true;   // THE reservation: before any provider effect, never cleared, so the slot is never reused
    table_lock_.clear(std::memory_order_release);   // the slot is this caller's from here on
    table_unlock.held = false;
    StartupChildPpuRequest child{};
    child.kind = slot.policy.kind;
    child.priority = uint32_t(request->priority);
    child.creator_priority = uint32_t(request->creator_priority);
    child.stack_size = request->stack_size;
    child.tls_capacity = slot.policy.tls_capacity;
    child.entry_address = request->entry;   // compiled-code address resolved by the bridge from the guest OPD
    child.toc = request->toc;
    child.argument = request->argument;
    child.return_lr = slot.policy.return_lr;
    child.stack_entry_slack = slot.policy.stack_entry_slack;
    child.tls_template = *tls;              // by value: the bridge's pointer is valid only during this call
    child.boot_content = request->boot_content;
    child.system_parameters = request->system_parameters;
    child.process_sdk = request->process_sdk;
    child.system_profile = request->system_profile;
    child.presentation_topology = request->presentation_topology;
    child.presentation_mode = request->presentation_mode;
    child.disc_change_callbacks = request->disc_change_callbacks;
    child.gamedata = request->gamedata;
    child.allow_thread_exit = slot.policy.run_to_exit;
#ifdef PS3_NATIVE_LLE_SRE
    if (!slot.policy.run_to_exit && uint32_t(request->priority) >= uint32_t(request->creator_priority)) {
        child.background = true;
        child.allow_thread_exit = true;
    }
#endif
    g_startup_create_stage = 1;   // root diagnostic (racy on purpose), read only by the unjoined report
    // Decision 119: the composition may pick this child's host priority from its guest name/priority (before its host
    // thread exists). Null hook: unchanged.
    if (g_startup_child_priority_hook)
        g_startup_child_priority_hook(slot.ports.thread.context, request->priority, request->name, request->entry);
    slot.report.prepare = slot.child.prepare(slot.ports, child);
    g_startup_create_stage = 2;
    slot.report.mapped_prepare = startup_child_map_prepare(slot.report.prepare.status);
    out.status = slot.report.mapped_prepare;
    if (out.status == StartupPpuCreatePrepareStatus::Prepared) {
        slot.report.prepared_id = slot.child.id();
        out.thread_id = slot.report.prepared_id;
        if (out.thread_id == 0) out.status = slot.report.mapped_prepare = StartupPpuCreatePrepareStatus::ProviderFailure;
    }
    return out;   // a failure after the reservation keeps the child's resources for teardown()
}

StartupPpuCreateStartStatus StartupChildComposition::start(uint32_t thread_id)
{
    ++totals_.start_calls;
    Slot *routed = nullptr;   // the slot whose child was prepared with EXACTLY this id
    if (thread_id != 0) {
        while (table_lock_.test_and_set(std::memory_order_acquire)) startup_child_table_relax();
        for (unsigned i = 0; i < slot_count_ && !routed; ++i) {
            const StartupChildCompositionReport &r = slots_[i].report;
            if (r.prepare_attempted && r.prepare.status == StartupChildPpuPrepareStatus::Ok && r.prepared_id == thread_id) routed = &slots_[i];
        }
        table_lock_.clear(std::memory_order_release);
    }
    if (!routed) {   // zero, unknown or never-prepared id: no child is touched
        ++totals_.unmatched_start;
        if (slot_count_ == 1) { ++slots_[0].report.start_calls; slots_[0].report.mapped_start = StartupPpuCreateStartStatus::ProviderFailure; }
        return StartupPpuCreateStartStatus::ProviderFailure;
    }
    StartupChildCompositionReport &report = routed->report;
    ++report.start_calls;
    if (report.start_attempted || report.start_skipped_by_cancel_latch)   // a repeated id: nothing changes
        return report.mapped_start = StartupPpuCreateStartStatus::ProviderFailure;
    if (create_cancel_.load(std::memory_order_acquire)) {   // stop requested: no host start, the reservation stays for teardown
        report.start_skipped_by_cancel_latch = true;
        return report.mapped_start = StartupPpuCreateStartStatus::ProviderFailure;
    }
    report.start_attempted = true;
    g_startup_create_stage = 3;
    report.start = routed->child.start();
    g_startup_create_stage = 4;
    report.final_start = report.start;
    if (routed->policy.run_to_exit) {
        // run-to-exit: the child must END ITSELF (sys_ppu_thread_exit). Wait with the slot's own injected, finite bound; only a
        // final Exited is Started. A child that blocks in the receive, or is still running after the bound, is a ProviderFailure
        // that stays retained for teardown(). Nothing is reaped here and nothing is invented.
        unsigned polls = 0;
#ifdef PS3_NATIVE_LLE_PROFILE
        // Root (2026-11-11): the creator's prefix wait is bounded under the LLE profile (domain/ppu_threads.c), so start()
        // can come back while the child is still running and report it as "blocked once". The exit wait below needs the
        // child's real state, so it is observed again here (poll_exit has no side effects).
        if (report.final_start.status == StartupChildPpuStartStatus::Blocked) report.final_start = routed->child.poll_exit();
#endif
        while (report.final_start.status == StartupChildPpuStartStatus::Running && polls < routed->policy.max_exit_polls) {
            if (routed->policy.exit_wait_poll) routed->policy.exit_wait_poll(routed->policy.exit_wait_context);
            report.final_start = routed->child.poll_exit();
            ++polls;
        }
        report.exit_polls = polls;
        report.observed_exited = report.final_start.status == StartupChildPpuStartStatus::Exited;
        switch (report.final_start.status) {
        case StartupChildPpuStartStatus::Exited: return report.mapped_start = StartupPpuCreateStartStatus::Started;
        case StartupChildPpuStartStatus::Returned:
        case StartupChildPpuStartStatus::Terminal: return report.mapped_start = StartupPpuCreateStartStatus::Terminal;
        default: return report.mapped_start = StartupPpuCreateStartStatus::ProviderFailure;   // Blocked, Running, and every failure
        }
    }
    report.observed_background = report.start.status == StartupChildPpuStartStatus::Background;
    report.observed_blocked = report.start.status == StartupChildPpuStartStatus::Blocked;
    return report.mapped_start = startup_child_map_start(report.start.status);
}

// lv2 syscall 44 (sys_ppu_thread_join). Routing is by the EXACT prepared id, exactly like start(). Nothing is cancelled or reaped here:
// the child's release stays teardown()'s job. The exit value is read through StartupChildPpu::control(), which is valid because this
// slot's start() really observed the child EXITED (observed_exited), and the child's release store orders every field read there.
StartupPpuJoinResult StartupChildComposition::join(uint32_t thread_id)
{
    ++totals_.join_calls;
    StartupPpuJoinResult out;   // ProviderFailure until a rule below says otherwise
    if (thread_id != 0)
        for (unsigned i = 0; i < slot_count_; ++i) {
            StartupChildCompositionReport &report = slots_[i].report;
            if (!report.prepare_attempted || report.prepare.status != StartupChildPpuPrepareStatus::Ok || report.prepared_id != thread_id) continue;
            ++report.join_calls;
            if (!report.observed_exited) {   // a blocked or never-exited child is never joined
                out.status = StartupPpuJoinStatus::NotExited;
                return out;
            }
            if (report.joined) { out.status = StartupPpuJoinStatus::AlreadyJoined; return out; }
            const StartupControl &child = slots_[i].child.control();
            if (!child.has_guest_exit) return out;                                       // no guest exit value was recorded: never invent one
            report.joined = true;
            out.status = StartupPpuJoinStatus::Joined;
            out.exit_value = child.guest_exit_value;
            return out;
        }
    ++totals_.join_unmatched;   // zero, unknown or never-prepared id: no child is touched
    if (slot_count_ == 1) ++slots_[0].report.join_calls;   // legacy one-slot contract: an unmatched join is slot 0's call
    out.status = StartupPpuJoinStatus::NotFound;
    return out;
}

const StartupChildCompositionReport &StartupChildComposition::report()
{
    slots_[0].report.stop_requested_by_watchdog = watchdog_called_.load(std::memory_order_acquire);
    return slots_[0].report;
}
const StartupChildCompositionReport &StartupChildComposition::report(unsigned slot)
{
    static const StartupChildCompositionReport none{};
    if (slot >= STARTUP_CHILD_COMPOSITION_MAX_SLOTS) return none;
    if (slot < slot_count_) slots_[slot].report.stop_requested_by_watchdog = watchdog_called_.load(std::memory_order_acquire);
    return slots_[slot].report;
}

bool StartupChildComposition::slot_done(const Slot &slot) const
{
    return slot.report.teardown_ran && (slot.report.teardown == StartupChildCompositionTeardown::Released ||
                                        slot.report.teardown == StartupChildCompositionTeardown::NeverPrepared);
}
bool StartupChildComposition::released() const
{
    if (!teardown_ran_) return false;
    for (unsigned i = 0; i < (slot_count_ ? slot_count_ : 1u); ++i) if (!slot_done(slots_[i])) return false;
    return true;
}

void StartupChildComposition::reap_slot(Slot &slot, const StartupChildCompositionTeardownPolicy &policy)
{
    StartupChildCompositionReport &report = slot.report;
    unsigned polls = 0, retries = 0;
    if (report.first_reap_sequence == 0) report.first_reap_sequence = ++teardown_sequence_;
    for (;;) {
        const StartupChildPpuReapResult reaped = slot.child.reap();
        ++report.reap_calls;
        report.reap = reaped;
        bool again = false;
        switch (reaped.status) {
        case StartupChildPpuReapStatus::Released:
        case StartupChildPpuReapStatus::NothingOwned: report.teardown = StartupChildCompositionTeardown::Released; break;
        case StartupChildPpuReapStatus::StillRunning:
            report.teardown = StartupChildCompositionTeardown::StillRunning;
            if (polls < policy.max_reap_polls) { ++polls; ++report.still_running_polls; again = true; }
            break;
        case StartupChildPpuReapStatus::ServiceRetained:
            report.teardown = StartupChildCompositionTeardown::ServiceRetained;
            if (retries < policy.max_stage_retries) { ++retries; ++report.stage_retries; again = true; }
            break;
        case StartupChildPpuReapStatus::AdmissionRetained:
            report.teardown = StartupChildCompositionTeardown::AdmissionRetained;
            if (retries < policy.max_stage_retries) { ++retries; ++report.stage_retries; again = true; }
            break;
        }
        if (!again) break;
        if (policy.poll) policy.poll(policy.context);   // injected finite sleep; never an unbounded wait
    }
    report.after = slot.child.status();
}

const StartupChildCompositionReport &StartupChildComposition::teardown(const StartupChildCompositionTeardownPolicy &policy)
{
    teardown_ran_ = true;
    for (Slot &slot : slots_) slot.dispatch_stop.store(true, std::memory_order_release);   // assert every stop latch on the deadline path
    create_cancel_.store(true, std::memory_order_release);
    const unsigned n = slot_count_ ? slot_count_ : 1;   // an unconfigured composition still reports slot 0 (nothing owned)
    bool live[STARTUP_CHILD_COMPOSITION_MAX_SLOTS]{};
    // 1. diagnostics BEFORE any cancel, for every unreleased slot (first capture only is kept in report.before)
    for (unsigned i = 0; i < n; ++i) {
        Slot &slot = slots_[i];
        if (slot_done(slot)) continue;   // released earlier: never touched again
        slot.report.teardown_ran = true;
        slot.report.latches_asserted_by_teardown = true;
        const StartupChildPpuStatus pre = slot.child.status();
        if (!slot.before_captured) { slot.report.before = pre; slot.before_captured = true; }
        if (!pre.has_resources) {   // nothing owned: never prepared (or already released)
            slot.report.teardown = pre.phase == StartupChildPpuPhase::Released ? StartupChildCompositionTeardown::Released : StartupChildCompositionTeardown::NeverPrepared;
            slot.report.after = pre;
        } else live[i] = true;
    }
    // 2. cancel EVERY live/reserved slot BEFORE the first reap: a slot that waits in its own receive is only woken by its own cancel,
    //    so no sibling's bounded reap may delay another slot's cancellation
    //    (cancel() is re-issued on a retry as before; only the report's sequence number is kept from the FIRST cancel)
    for (unsigned i = 0; i < n; ++i) if (live[i]) {
        slots_[i].child.cancel();
        if (slots_[i].report.cancel_sequence == 0) slots_[i].report.cancel_sequence = ++teardown_sequence_;
    }
    // 3. one bounded reap per slot; a retained or still-running slot never prevents its sibling's release
    for (unsigned i = 0; i < n; ++i) if (live[i]) reap_slot(slots_[i], policy);
    return report();
}
#endif
