#include "startup_child_ppu.h"
#include <cstring>

namespace {
// Approved child TLS layout (evidence/opus-native-child-tls-layout-r1/DECISION.md): the TLS region R holds a page of
// header slack, then the image block at R+0x1000 with the 0x30 header bytes directly below it. r13 = block + 0x7000.
constexpr uint64_t PAGE = 0x1000, IMAGE_OFFSET = 0x1000, HEADER_BYTES = 0x30, THREAD_POINTER_BIAS = 0x7000,
    TLS_OVERHEAD = THREAD_POINTER_BIAS + PAGE, STACK_GRANULE = PS3_PPU_STACK_GRANULE, REGION_GRANULE = PS3_PPU_GUEST_REGION_GRANULE,
    ADDRESS_SPACE = UINT64_C(1) << 32;

bool round_up(uint64_t value, uint64_t granule, uint64_t &out)
{
    if (value > UINT64_MAX - (granule - 1)) return false;
    out = (value + granule - 1) / granule * granule;
    return true;
}
// Checked TLS requirement for a template against an image capacity (same formula as the TLS API).
bool tls_required(const Ps3TlsTemplate &t, uint64_t &required)
{
    if (t.file_size > t.memory_size || t.memory_size > ADDRESS_SPACE - TLS_OVERHEAD) return false;
    return round_up(t.memory_size + TLS_OVERHEAD, PAGE, required);
}
bool ports_valid(const StartupChildPpuPorts &p)
{
    return p.threads && p.queues && p.memory && p.thread.start && p.thread.join && p.code && p.code->find &&
        p.clock && p.clock->now_microseconds && p.recorder && p.budget && p.dispatch_stop.stop_requested && p.dispatch_limit > 0;
}
// Everything decidable without touching a resource: layout of the request itself.
bool request_layout_valid(const StartupChildPpuRequest &r)
{
    uint64_t logical = 0, region = 0, tls_region = 0, required = 0;
    if (!r.entry_address || !r.stack_size || !r.tls_capacity) return false;
    if (r.stack_entry_slack < 16 || r.stack_entry_slack % 16) return false;
    if (!round_up(r.stack_size, STACK_GRANULE, logical) || r.stack_entry_slack > logical) return false;
    if (!round_up(logical, REGION_GRANULE, region) || !round_up(r.tls_capacity, REGION_GRANULE, tls_region)) return false;
    if (tls_region <= IMAGE_OFFSET || !tls_required(r.tls_template, required)) return false;
    return required <= tls_region - IMAGE_OFFSET;
}
}

StartupChildPpuPrepareResult StartupChildPpu::prepare(const StartupChildPpuPorts &ports, const StartupChildPpuRequest &request)
{
    StartupChildPpuPrepareResult result;
    if (phase_ != StartupChildPpuPhase::Empty) { result.status = StartupChildPpuPrepareStatus::NotEmpty; result.owns_resources = phase_ != StartupChildPpuPhase::Released; return result; }
    if (!ports_valid(ports)) { result.status = StartupChildPpuPrepareStatus::InvalidArgument; return result; }
    if (!request_layout_valid(request)) { result.status = StartupChildPpuPrepareStatus::InvalidLayout; return result; }
    ports_ = ports;
    request_ = request;

    ps3_ppu_ref ref{};
    // A run-to-exit child is fenced: start() waits until the body leaves RUNNING and a body that blocks instead is a failure
    // (the composition cancels/joins it), so a lower-priority child never runs concurrently with its creator.
    const uint32_t reserved = request_.allow_thread_exit || request_.background
        ? ps3_ppu_service_reserve_guest_fenced(ports_.threads, request_.kind, request_.priority, request_.creator_priority,
            request_.stack_size, request_.tls_capacity, ports_.thread, &StartupChildPpu::service_body, this, &ref)
        : ps3_ppu_service_reserve_guest(ports_.threads, request_.kind, request_.priority, request_.creator_priority,
            request_.stack_size, request_.tls_capacity, ports_.thread, &StartupChildPpu::service_body, this, &ref);
    if (reserved != PS3_PPU_OK) { result.status = StartupChildPpuPrepareStatus::ReserveFailed; result.native_rc = reserved; return result; }
    // From here the reservation is owned and retained until reap().
    ref_ = ref;
    phase_ = StartupChildPpuPhase::Reserved;
    result.owns_resources = true;

    uint32_t rc = ps3_ppu_service_guest_storage(ports_.threads, ref_, &storage_);
    if (rc != PS3_PPU_OK) { result.status = StartupChildPpuPrepareStatus::StorageFailed; result.native_rc = rc; return result; }
    uint64_t required = 0;
    if (storage_.stack_logical_bytes < request_.stack_entry_slack || storage_.tls_capacity_bytes <= IMAGE_OFFSET ||
        !tls_required(request_.tls_template, required) || required > storage_.tls_capacity_bytes - IMAGE_OFFSET ||
        uint64_t(storage_.tls_address) + storage_.tls_capacity_bytes > ADDRESS_SPACE) {
        result.status = StartupChildPpuPrepareStatus::StorageFailed;   // allocator layout cannot hold the approved layout
        return result;
    }

    // TLS: the API validates layout/template/range before any byte is written; only then is the 0x30 header cleared.
    const ps3_guest_memory &memory = ports_.memory->coherent_memory;
    const uint32_t block = uint32_t(storage_.tls_address + IMAGE_OFFSET);
    result.tls = ps3_tls_initialize_allocated(&memory, block, storage_.tls_capacity_bytes - IMAGE_OFFSET, &request_.tls_template,
        tls_scratch_, sizeof tls_scratch_, &tls_);
    if (result.tls.status != PS3_TLS_OK) { result.status = StartupChildPpuPrepareStatus::TlsFailed; return result; }
    if (tls_.block != block || uint64_t(tls_.thread_pointer) != uint64_t(block) + THREAD_POINTER_BIAS) {
        result.tls.status = PS3_TLS_ARGUMENT; result.status = StartupChildPpuPrepareStatus::TlsFailed; return result;
    }
    const ps3_guest_memory_status zeroed = memory.zero(memory.context, block - uint32_t(HEADER_BYTES), HEADER_BYTES);
    if (zeroed != PS3_GUEST_MEMORY_OK) {
        result.tls.status = PS3_TLS_MEMORY_FAILURE; result.tls.memory_status = zeroed;
        result.status = StartupChildPpuPrepareStatus::TlsFailed; return result;
    }

    // Per-child execution: own control, services, receive port, cancel token; borrowed shared immutable capabilities.
    services_ = Ps3PpuExecutionServices{};
    services_.memory = &memory;
    services_.clock = ports_.clock;
    services_.compiled_code = ports_.code;
    // PROCESS-WIDE services (decision 14): the same guarded user-memory + lwmutex instance the primary uses, so a child syscall
    // (sys_memory_allocate sc348, the lwmutex imports) reaches the process's real service instead of stopping MissingServices. Only
    // the capability FUNCTIONS and their context are copied; the child keeps its OWN memory binding above, its clock and its
    // compiled code, and initialize_tls stays null (the child uses its own TLS path). A null process_services keeps all of them null.
    if (ports_.process_services) {
        const Ps3PpuExecutionServices &process = *ports_.process_services;
        services_.context = process.context;
        services_.create_mutex = process.create_mutex;
        services_.lock_mutex = process.lock_mutex;
        services_.try_mutex = process.try_mutex;
        services_.unlock_mutex = process.unlock_mutex;
        services_.destroy_mutex = process.destroy_mutex;
        services_.allocate_memory = process.allocate_memory;
        services_.free_memory = process.free_memory;
        services_.query_memory = process.query_memory;
    }
    execution_.fixed_timezone = ports_.fixed_timezone;
    receive_port_ = StartupEventReceivePort{};
    receive_port_.context = this;
    receive_port_.receive = &StartupChildPpu::receive;
    control_.entry = StartupEntry::IndirectCall;
    control_.terminal_notification = {&StartupChildPpu::terminal, this};
    execution_.control = &control_;
    execution_.boot_content = request_.boot_content;
    execution_.system_parameters = request_.system_parameters;
    execution_.process_sdk = request_.process_sdk;
    execution_.system_profile = request_.system_profile;
    execution_.presentation_topology = request_.presentation_topology;
    execution_.presentation_mode = request_.presentation_mode;
    execution_.disc_change_callbacks = request_.disc_change_callbacks;
    execution_.gamedata = request_.gamedata;
    execution_.child_thread_exit_allowed = request_.allow_thread_exit;
    execution_.guest_priority = int32_t(request_.priority);
    execution_.has_guest_priority = true;
    execution_.dispatch = {0, ports_.dispatch_limit};
    execution_.dispatch_stop = ports_.dispatch_stop;
    execution_.event_queues = ports_.queues;
    execution_.event_receive = &receive_port_;
    /* Root (B2b Eden): the process-wide lv2 sync + SPU services (Decision 33) belong to the process, like
     * user memory: every thread of it -- including libsre's own SPURS threads, which issue
     * sys_spu_thread_group_start -- must reach the SAME registries. Null keeps the previous behaviour. */
    if (ports_.process_lv2) {
        execution_.lv2_sleep = ports_.process_lv2->lv2_sleep;
        execution_.lv2_sync_cancel = ports_.process_lv2->lv2_sync_cancel;
        execution_.spu_images = ports_.process_lv2->spu_images;
        execution_.spu_groups = ports_.process_lv2->spu_groups;
        /* Root (C1b, 2026-11-11): lwcond objects belong to the process too. The game's priority-99 thread (entry
         * 0x00442790) locks an lwmutex and calls sys_lwcond_wait (0x2A6D9D51 at 0x442AF0); without the process's
         * condition registry that stopped MissingServices. Same registry instance the primary uses. */
        execution_.conditions = ports_.process_lv2->conditions;
        /* The console (lv2 sc403 sys_tty_write) is the process's as well: a game worker thread (entry 0x00442790)
         * writes to it from 0x447274 and stopped MissingServices. */
        execution_.tty = ports_.process_lv2->tty;
        /* The file system is the process's too (lv2 sc 801.. and the cellFs imports): same service, same guard. */
        execution_.fs = ports_.process_lv2->fs;
        execution_.ppu_threads = ports_.process_lv2->ppu_threads;
        /* The wall clock is the process's (lv2 sc145 sys_time_get_current_time): a game file-I/O worker calls it. The
         * calendar clock port is immutable and its sample function is safe to call from any thread. */
        execution_.calendar_clock = ports_.process_lv2->calendar_clock;
        /* C1b: every blocking lv2 wait of this child publishes into its service activity word (see lv2_blocked_word). */
        execution_.lv2_blocked_word = &ports_.threads->services[ref_.slot].activity;
        execution_.pad_process = ports_.process_lv2->pad_process;
        execution_.audio_process = ports_.process_lv2->audio_process;
        execution_.timer_process = ports_.process_lv2->timer_process;
        execution_.mutex_process = ports_.process_lv2->mutex_process;
        /* Root (2026-11-12, Decision 68): thread creation is the process's too. A child had neither the create
         * port nor the TLS template, so the first child that created a thread (the movie reader, creating Bink's
         * worker) was stopped MissingServices in dispatch_ppu_thread_create. The template is the snapshot the
         * primary took at start-up; it is complete long before any child runs. */
        execution_.ppu_create = ports_.process_lv2->ppu_create;
        if (ports_.process_lv2->control && ports_.process_lv2->control->initialized_tls_template_valid) {
            control_.initialized_tls_template = ports_.process_lv2->control->initialized_tls_template;
            control_.initialized_tls_template_valid = true;
        }
        execution_.rsx_process = ports_.process_lv2->rsx_process;
        execution_.dialog_process = ports_.process_lv2->dialog_process;
    }
    ppu_context &cpu = execution_.ppu.cpu;
    cpu.ctr = request_.entry_address;
    cpu.cia = request_.entry_address;
    cpu.gpr[1] = uint64_t(storage_.stack_address) + storage_.stack_logical_bytes - request_.stack_entry_slack;
    cpu.gpr[2] = request_.toc;
    cpu.gpr[3] = request_.argument;
    cpu.gpr[4] = 0;
    cpu.gpr[13] = tls_.thread_pointer;
    cpu.lr = request_.return_lr;
    cpu.thread_id = ref_.id;
    if (!native_startup_admit_budgeted(execution_, *ports_.memory, services_, *ports_.recorder, *ports_.budget)) {
        result.status = StartupChildPpuPrepareStatus::AdmissionFailed; return result;
    }
    result.status = StartupChildPpuPrepareStatus::Ok;
    return result;
}

StartupChildPpuStartResult StartupChildPpu::start()
{
    StartupChildPpuStartResult result;
    if (phase_ != StartupChildPpuPhase::Reserved || !execution_.ppu.admitted) return result;   // NotReserved
    // A serialized prepare -> cancel -> start never starts the host thread or runs the body: it stays Reserved for reap() (abandon).
    if (cancel_requested_.load(std::memory_order_acquire)) { result.status = StartupChildPpuStartStatus::Cancelled; return result; }
    const uint32_t rc = request_.background
        ? ps3_ppu_service_start_background(ports_.threads, ref_)
        : ps3_ppu_service_start(ports_.threads, ref_);
    result.native_rc = rc;
    if (rc == PS3_PPU_EAGAIN) { result.status = StartupChildPpuStartStatus::StartFailed; return result; }   // stays Reserved: no host thread
    if (rc == PS3_PPU_OWNED_FAILURE) {
        host_started_ = true; phase_ = StartupChildPpuPhase::Started;
        result.status = StartupChildPpuStartStatus::OwnedFailure; return result;
    }
    if (rc != PS3_PPU_OK) { result.status = StartupChildPpuStartStatus::Failed; return result; }
    host_started_ = true;
    phase_ = StartupChildPpuPhase::Started;
    if (request_.background) { result.status = StartupChildPpuStartStatus::Background; return result; }
    ps3_ppu_service &service = ports_.threads->services[ref_.slot];
    const uint64_t activity = __atomic_load_n(&service.activity, __ATOMIC_SEQ_CST);
    if (activity == PS3_PPU_ACTIVITY_EXITED) {
        result.stop = control_.stop;
        if (result.stop == StartupStop::GuestThreadExit && request_.allow_thread_exit) result.status = StartupChildPpuStartStatus::Exited;
        else result.status = result.stop == StartupStop::Returned ? StartupChildPpuStartStatus::Returned : StartupChildPpuStartStatus::Terminal;
    } else if (ps3_ppu_service_is_blocked(ports_.threads, ref_) && receive_calls_.load(std::memory_order_acquire) > 0) {
        result.status = StartupChildPpuStartStatus::Blocked;
    } else if (execution_.lv2_blocked_word) {
        // C1b (process lv2 services): ps3_ppu_service_start returned OK and the child has not exited, so its prefix wait
        // ended because the child really published BLOCKED from an lv2 wait (nothing else writes the activity word). It
        // may already have been woken again by another thread; that is still a child that started and blocked once.
        result.status = StartupChildPpuStartStatus::Blocked;
    } else {
        result.status = StartupChildPpuStartStatus::Running;
    }
    return result;
}

StartupChildPpuStartResult StartupChildPpu::poll_exit()
{
    // Owner method, NO side effects: it observes exactly what start() reads and nothing else (no cancel, no reap, no host call).
    StartupChildPpuStartResult result;
    if (phase_ != StartupChildPpuPhase::Started) return result;   // NotReserved
    const ps3_ppu_service &service = ports_.threads->services[ref_.slot];
    const uint64_t activity = __atomic_load_n(&service.activity, __ATOMIC_SEQ_CST);   // the same load start() performs
    if (activity == PS3_PPU_ACTIVITY_EXITED) {
        result.stop = control_.stop;
        if (result.stop == StartupStop::GuestThreadExit && request_.allow_thread_exit) result.status = StartupChildPpuStartStatus::Exited;
        else result.status = result.stop == StartupStop::Returned ? StartupChildPpuStartStatus::Returned : StartupChildPpuStartStatus::Terminal;
    } else if (ps3_ppu_service_is_blocked(ports_.threads, ref_) && receive_calls_.load(std::memory_order_acquire) > 0) {
        result.status = StartupChildPpuStartStatus::Blocked;
    } else {
        result.status = StartupChildPpuStartStatus::Running;
    }
    return result;
}

void StartupChildPpu::cancel()
{
    // Owner method (serialized with prepare/start/reap/status; see the header). Cancel while Empty/Released is a documented no-op.
    if (phase_ == StartupChildPpuPhase::Empty || phase_ == StartupChildPpuPhase::Released) return;
    cancel_requested_.store(true, std::memory_order_release);
    control_.cancelled.store(true, std::memory_order_release);
    ps3_notify_cancel_request(&token_, &ports_.queues->port);
}

StartupChildPpuReapResult StartupChildPpu::reap()
{
    StartupChildPpuReapResult result;
    if (phase_ == StartupChildPpuPhase::Empty || phase_ == StartupChildPpuPhase::Released) return result;   // NothingOwned
    if (phase_ == StartupChildPpuPhase::Reserved) {
        result.service_rc = ps3_ppu_service_abandon(ports_.threads, ref_);
        if (result.service_rc != PS3_PPU_OK) { result.status = StartupChildPpuReapStatus::ServiceRetained; return result; }
        phase_ = StartupChildPpuPhase::ServiceReleased;
    } else if (phase_ == StartupChildPpuPhase::Started) {
        const ps3_ppu_service &service = ports_.threads->services[ref_.slot];
        // Never join before the real activity is EXITED, cancelled or not: cancel() only wakes a receive wait and running compiled
        // code reaches no cancellation point on demand. EXITED is final, so a retry after a failed storage cleanup (the join already
        // completed and its result was recorded by the service domain) passes this gate again and ps3_ppu_service_join resumes the
        // cleanup stage without a second host join.
        if (__atomic_load_n(&service.activity, __ATOMIC_SEQ_CST) != PS3_PPU_ACTIVITY_EXITED) {
            result.status = StartupChildPpuReapStatus::StillRunning; return result;
        }
        uint32_t code = 0;
        result.service_rc = ps3_ppu_service_join(ports_.threads, ref_, &code);
        if (result.service_rc != PS3_PPU_OK) { result.status = StartupChildPpuReapStatus::ServiceRetained; return result; }
        exit_code_ = code;
        service_.store(nullptr, std::memory_order_release);
        phase_ = StartupChildPpuPhase::ServiceReleased;
    }
    // Service storage is freed (join/abandon); release ONLY this child's admission owner. Never the shared teardown.
    if (execution_.ppu.admitted) {
        result.memory_status = ppu_memory_release_after_join(execution_.ppu);
        if (result.memory_status != PS3_GUEST_MEMORY_OK) { result.status = StartupChildPpuReapStatus::AdmissionRetained; return result; }
    }
    phase_ = StartupChildPpuPhase::Released;
    result.status = StartupChildPpuReapStatus::Released;
    return result;
}

StartupChildPpuStatus StartupChildPpu::status() const
{
    StartupChildPpuStatus s;
    s.phase = phase_;
    s.service_owned = phase_ == StartupChildPpuPhase::Reserved || phase_ == StartupChildPpuPhase::Started;
    s.admitted = execution_.ppu.admitted;
    s.has_resources = s.service_owned || s.admitted;
    s.host_started = host_started_;
    s.cancel_requested = cancel_requested_.load(std::memory_order_acquire);
    s.ref = ref_; s.storage = storage_; s.tls = tls_;
    s.terminal_calls = terminal_calls_.load(std::memory_order_acquire);
    if (s.terminal_calls) s.stop = control_.stop;
    s.consumed_events = consumed_count_.load(std::memory_order_acquire);
    if (s.consumed_events) s.first_consumed = first_consumed_;   // write-once before the 0 -> 1 release store: immutable
    s.exit_code = exit_code_;
    s.receive_calls = receive_calls_.load(std::memory_order_acquire);
    s.last_queue = last_queue_.load(std::memory_order_relaxed);
    s.last_pc = last_pc_.load(std::memory_order_relaxed);
    s.last_native_rc = last_native_rc_.load(std::memory_order_relaxed);
    s.fault = fault_.load(std::memory_order_relaxed);
    s.discarded_event = discarded_event_.load(std::memory_order_acquire);
    if (s.discarded_event) s.discarded = discarded_;
    return s;
}

void StartupChildPpu::publish_exit_once(StartupControl *control)
{
    if (exit_published_.exchange(true, std::memory_order_acq_rel)) return;
    ps3_ppu_service *service = service_.load(std::memory_order_acquire);
    if (service) ps3_ppu_service_exit(service, uint32_t(control->stop));
}

void StartupChildPpu::service_body(ps3_ppu_service *self, void *argument)
{
    StartupChildPpu *child = static_cast<StartupChildPpu *>(argument);
    child->service_.store(self, std::memory_order_release);
    native_startup_worker(&child->execution_);   // every real stop publishes terminal() then leaves via threadExit
    child->publish_exit_once(&child->control_);   // only reached if the worker returned without a terminal stop
}

void StartupChildPpu::terminal(void *context, StartupControl *control)
{
    StartupChildPpu *child = static_cast<StartupChildPpu *>(context);
    child->terminal_calls_.fetch_add(1, std::memory_order_release);
    child->publish_exit_once(control);
}

StartupEventReceiveResult StartupChildPpu::receive(void *context, uint32_t queue_id, uint64_t timeout_us)
{
    return static_cast<StartupChildPpu *>(context)->receive_impl(queue_id, timeout_us);
}

StartupEventReceiveResult StartupChildPpu::fail(StartupEventReceiveOutcome outcome, StartupChildPpuReceiveFault why, uint32_t rc)
{
    fault_.store(why, std::memory_order_relaxed);
    last_native_rc_.store(rc, std::memory_order_relaxed);
    StartupEventReceiveResult result;
    result.outcome = outcome;
    return result;
}

StartupEventReceiveResult StartupChildPpu::receive_impl(uint32_t queue_id, uint64_t timeout_us)
{
    using Outcome = StartupEventReceiveOutcome;
    using Fault = StartupChildPpuReceiveFault;
    last_queue_.store(queue_id, std::memory_order_relaxed);
    last_pc_.store(execution_.ppu.cpu.cia, std::memory_order_relaxed);
    receive_calls_.fetch_add(1, std::memory_order_release);
    const auto cancelled = [this] {
        return control_.cancelled.load(std::memory_order_acquire) || __atomic_load_n(&token_.requested, __ATOMIC_ACQUIRE) != 0;
    };
    ps3_ppu_service *const service = service_.load(std::memory_order_acquire);
    if (!service) return fail(Outcome::ProviderFailure, Fault::Native, 0);
    if (timeout_us != 0) return fail(Outcome::Unsupported, Fault::Timeout, 0);   // only the non-timed receive is supported
    if (cancelled()) return fail(Outcome::Cancelled, Fault::Cancelled, 0);
    ps3_lv2_queue snapshot;
    const uint32_t looked_up = ps3_lv2_queue_snapshot(ports_.queues, queue_id, &snapshot);
    if (looked_up != PS3_LV2_OK) return fail(Outcome::Unsupported, Fault::UnknownQueue, looked_up);
    if (snapshot.type != PS3_PPU_QUEUE) return fail(Outcome::Unsupported, Fault::WrongQueueType, 0);
    if (snapshot.service_bound) return fail(Outcome::Unsupported, Fault::ServiceBoundQueue, 0);
    ps3_lv2_event event{};
    // The real primitive publishes BLOCKED into service->activity under the queue guard and wakes the creator.
    const uint32_t rc = ps3_lv2_queue_receive(ports_.queues, queue_id, &event, &token_, &service->activity);
    last_native_rc_.store(rc, std::memory_order_relaxed);
    if (rc == PS3_LV2_OK) {
        // The dequeue is a fact independent of what follows (cancel here, or the bridge stopping on cancel/budget after this
        // callback returns): record it BEFORE any later decision. Delivery to the guest registers is never inferred from this.
        if (consumed_count_.load(std::memory_order_relaxed) == 0) first_consumed_ = event;   // single writer (the child thread)
        consumed_count_.fetch_add(1, std::memory_order_release);
        if (cancelled()) {   // the core dequeues BEFORE it looks at cancellation: the event is consumed and discarded at teardown
            discarded_ = event;
            discarded_event_.store(true, std::memory_order_release);
            return fail(Outcome::Cancelled, Fault::Cancelled, rc);
        }
        StartupEventReceiveResult result;
        result.outcome = Outcome::Completed;
        result.result = 0;
        result.source = event.source; result.data1 = event.data1; result.data2 = event.data2; result.data3 = event.data3;
        return result;
    }
    if (rc == PS3_LV2_CANCELLED || cancelled()) return fail(Outcome::Cancelled, Fault::Cancelled, rc);
    if (rc == PS3_LV2_UNSUPPORTED) return fail(Outcome::Unsupported, Fault::Native, rc);
    return fail(Outcome::ProviderFailure, Fault::Native, rc);
}
