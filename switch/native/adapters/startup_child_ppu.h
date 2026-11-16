#ifndef NATIVE_STARTUP_CHILD_PPU_H
#define NATIVE_STARTUP_CHILD_PPU_H
// Child (secondary) PPU thread owner: one explicit, composition-owned object that runs ONE supplied compiled-code
// entry on its own mapped stack/TLS through the existing native_startup_worker / IndirectCall path.
//
// Requires PS3_NATIVE_INDIRECT_DISPATCH, PS3_NATIVE_STARTUP_SPURS_INITIALIZE (event queues + sc130 receive port) and
// PS3_NATIVE_CONTEXT_MEMORY. This is platform independent: every OS fact arrives through the injected ports.
//
// Lifetime contract (read before use):
//  - The object is large (it embeds a StartupControl): composition allocates it once (e.g. std::unique_ptr) and
//    never copies or moves it. It is single-use: prepare() succeeds at most once; after a completed reap() it stays
//    Released and is not reusable.
//  - There is deliberately NO destructor that frees live resources. Once prepare() has reserved storage the object
//    OWNS a PPU service record + mapped allocation and (after admission) a memory-admission owner. The caller must
//    keep the object, every injected port and every domain object alive until reap() returns Released.
//  - Compose order: cancel() -> reap() (retry only the reported stage) -> only then tear down the shared
//    ps3_ppu_threads / ps3_lv2_objects / PpuMemoryBinding / recorder / budget.
//  - prepare() does NOT read an OPD and does NOT write the guest thread-id output: the caller MUST write the
//    big-endian u64 thread id (id()) to its guest output BEFORE start().
//  - Never call native_startup_release_after_join() for a child: that is the shared SPURS/queue teardown.
//  - This object publishes only what real collaborators produce: it never synthesizes a guest errno or success.
//
// Threading contract (serialized-owner model; NOT a thread-safe object):
//  - prepare(), start(), cancel(), reap() and status() are OWNER methods. The owner (one thread at a time, or any
//    number of threads under one external lock/happens-before chain) must serialize ALL of them with each other.
//    They are never invoked concurrently, and a method is never invoked from the child's own thread (compiled
//    code, receive port, terminal notification, injected ports). The phase and the retained ports/ids are plain
//    members protected only by that serialization; they are deliberately NOT atomic ("atomic phase while the ports
//    still race" would be a half fix).
//  - A background watchdog / shutdown path MUST NOT call these methods before the owner has been joined or is
//    otherwise quiescent. It uses SEPARATE, immutable, composition-owned capabilities created before the owner runs:
//    the per-child ps3_compiled_dispatch_stop_port (backed by a composition atomic) is the asynchronous stop that
//    reaches running compiled code, and a composition-level cancel latch makes the owner skip start(). A child
//    BLOCKED in the sc130 receive is woken only by the owner calling cancel() (the wake needs the child's own token);
//    the standard composition therefore joins the parent first and then runs cancel() -> reap() serialized.
//  - cancel() while Empty (before prepare() succeeded, or after Released) is a documented NO-OP that is not remembered:
//    a later prepare()+start() runs uncancelled. Cancel while Reserved is remembered: start() then returns
//    StartupChildPpuStartStatus::Cancelled WITHOUT a host start (the compiled body never runs) and the object stays
//    Reserved so that reap() can abandon the reservation.
//  - Values the child thread publishes (terminal/receive counters, last_* diagnostics, the consumed-event record,
//    the discarded-event record) are atomics or write-once records ordered by a release store; status() reads
//    them with acquire loads and is therefore safe while the child runs, but only from the serialized owner.
//    status().stop is valid only when terminal_calls != 0; status().exit_code only after a successful join.
//  - reap() never calls the service join before the real PPU activity is EXITED, even after cancel(): a child still
//    running compiled code yields StillRunning (poll again with a finite deadline). A retry after a
//    failed storage cleanup (join already completed, cleanup pending) is allowed and never joins the host thread a
//    second time.
//  - poll_exit() is an OWNER method under the SAME serialization as prepare()/start()/cancel()/reap()/status(): it is never
//    invoked concurrently with any of them and never from the child's own thread. It has NO side effects (no host start, no
//    cancel, no resource change) and is valid only in phase Started; every other phase answers NotReserved. This is the
//    bounded observation a composition uses to wait for a run-to-exit child to end itself through sys_ppu_thread_exit.
#ifndef PS3_NATIVE_INDIRECT_DISPATCH
#error "startup_child_ppu requires PS3_NATIVE_INDIRECT_DISPATCH"
#endif
#ifndef PS3_NATIVE_STARTUP_SPURS_INITIALIZE
#error "startup_child_ppu requires PS3_NATIVE_STARTUP_SPURS_INITIALIZE (sc130 receive port)"
#endif
#include "startup_execution.h"
#include "../domain/ppu_threads.h"
#include "../domain/lv2_objects.h"
#include <atomic>
#include <cstdint>

// Borrowed through reap(). Shared (domain/immutable) and per-child (observation) capabilities are explicit.
struct StartupChildPpuPorts {
    ps3_ppu_threads *threads = nullptr;                 // shared pure-domain registry (main already adopted)
    ps3_lv2_objects *queues = nullptr;                  // shared pure-domain LV2 (guest queues + its notify port)
    PpuMemoryBinding *memory = nullptr;                 // shared coherent guest memory binding
    ps3_service_thread_port thread{};                   // dedicated host thread port (one per child)
    const ps3_compiled_code_lookup *code = nullptr;     // immutable shared lookup
    const ps3_clock *clock = nullptr;                   // immutable shared clock capability (never a primary counter)
    PpuMemoryTraceRecorder *recorder = nullptr;         // PER CHILD, stable through join
    DiscoveryBudgetBinding *budget = nullptr;           // PER CHILD, stable through join
    ps3_compiled_dispatch_stop_port dispatch_stop{};    // PER CHILD
    uint32_t dispatch_limit = 0;                        // indirect-call depth limit, > 0
    // PROCESS-WIDE services (decision 14), shared and borrowed through reap(); NOT per child. On a PS3 user memory, lwmutexes and
    // the timezone belong to the process, so every thread of it uses the same instances. Null keeps the previous behaviour exactly
    // (the child's syscalls then stop MissingServices instead of borrowing the process's).
    const Ps3PpuExecutionServices *process_services = nullptr;  // borrowed through reap: the process's guarded user-memory + lwmutex
                                                                // service instance (the SAME instance the primary uses)
    const StartupFixedTimezone *fixed_timezone = nullptr;       // borrowed through reap: the process's fixed timezone
    // Root (B2b): the primary execution whose lv2 sync/SPU service pointers every child borrows (Decision 33).
    const StartupExecution *process_lv2 = nullptr;
};
// Every value is an explicit caller policy; the adapter owns no game address.
struct StartupChildPpuRequest {
    uint32_t kind = 0, priority = 0, creator_priority = 0;
    uint64_t stack_size = 0, tls_capacity = 0;
    uint32_t entry_address = 0;                         // low 32 bits of CTR/CIA (compiled-code address, not an OPD)
    uint64_t toc = 0, argument = 0, return_lr = 0;
    uint32_t stack_entry_slack = 0x70;                  // bytes below the logical stack top; 16-aligned, 16..stack
    Ps3TlsTemplate tls_template{};                      // copied by value at prepare time
    BootContentMetadata boot_content{};                // creator's semantic launch metadata, copied at prepare
    SystemParameters system_parameters{};              // creator's immutable settings snapshot, copied at prepare
    StartupProcessSdk process_sdk{};                   // same admitted process SDK authority, copied at prepare
    SystemProfile system_profile{};                    // native application display profile, copied at prepare
    PresentationTopology presentation_topology=PresentationTopology::Unconfigured;
    NativePresentationMode presentation_mode=NativePresentationMode::Unconfigured;
    ps3_disc_change_registry *disc_change_callbacks=nullptr; // borrowed process registry/guard through join
    const StartupGameDataPort *gamedata=nullptr;        // borrowed composition service (null = the import stops MissingServices)
    bool allow_thread_exit=false;                       // true: sys_ppu_thread_exit ends the child (StartupStop::GuestThreadExit)
    bool background=false;                              // true: the child does not outrank its creator; start() returns without waiting for it
};
enum class StartupChildPpuPhase { Empty, Reserved, Started, ServiceReleased, Released };
enum class StartupChildPpuPrepareStatus { Ok, InvalidArgument, InvalidLayout, NotEmpty, ReserveFailed, StorageFailed, TlsFailed, AdmissionFailed };
struct StartupChildPpuPrepareResult {
    StartupChildPpuPrepareStatus status = StartupChildPpuPrepareStatus::InvalidArgument;
    uint32_t native_rc = 0;                             // ps3_ppu_* rc when the failing step returned one
    Ps3TlsReport tls{};                                 // valid for TlsFailed
    bool owns_resources = false;                        // true after the reservation: reap() is mandatory
};
enum class StartupChildPpuStartStatus { Blocked, Returned, Terminal, Running, NotReserved, Cancelled, StartFailed, OwnedFailure, Failed,
    Exited, Background };   // Cancelled: cancel() preceded start(); no host start, still Reserved. Exited: the child ended itself through
                            // sys_ppu_thread_exit with allow_thread_exit (the ONLY status that means a guest-initiated termination)
                            // Background: a background child was started; it may be running, blocked or already exited
struct StartupChildPpuStartResult {
    StartupChildPpuStartStatus status = StartupChildPpuStartStatus::NotReserved;
    uint32_t native_rc = 0;
    StartupStop stop = StartupStop::None;               // meaningful for Returned/Terminal
};
enum class StartupChildPpuReapStatus { Released, NothingOwned, StillRunning, ServiceRetained, AdmissionRetained };
struct StartupChildPpuReapResult {
    StartupChildPpuReapStatus status = StartupChildPpuReapStatus::NothingOwned;
    uint32_t service_rc = 0;
    ps3_guest_memory_status memory_status = PS3_GUEST_MEMORY_OK;
};
enum class StartupChildPpuReceiveFault { None, Timeout, UnknownQueue, WrongQueueType, ServiceBoundQueue, Native, Cancelled };
struct StartupChildPpuStatus {
    StartupChildPpuPhase phase = StartupChildPpuPhase::Empty;
    bool has_resources = false, service_owned = false, admitted = false, host_started = false, cancel_requested = false;
    ps3_ppu_ref ref{};
    ps3_ppu_guest_storage storage{};
    Ps3TlsImage tls{};
    unsigned terminal_calls = 0;
    StartupStop stop = StartupStop::None;               // valid only when terminal_calls != 0
    uint32_t exit_code = 0;                             // valid after a successful service join
    unsigned receive_calls = 0;
    uint32_t last_queue = 0;
    uint64_t last_pc = 0;
    uint32_t last_native_rc = 0;
    StartupChildPpuReceiveFault fault = StartupChildPpuReceiveFault::None;
    // Every SUCCESSFUL queue dequeue is recorded here, independent of what happened to it afterwards. The adapter never
    // knows whether the event reached guest registers (the bridge may still stop on cancel/budget AFTER the provider
    // returned): delivery is UNCONFIRMED for every consumed event; never infer "not delivered" or "delivered" from
    // fault/stop/discarded_event. consumed_events counts all dequeues; first_consumed is the immutable FIRST one
    // (valid iff consumed_events != 0), later events only increase the count.
    unsigned consumed_events = 0;
    ps3_lv2_event first_consumed{};
    // Narrower fact: the provider saw cancellation right after the dequeue and refused to return the event (it was
    // discarded at teardown). false does NOT mean nothing was consumed: consult consumed_events.
    bool discarded_event = false;
    ps3_lv2_event discarded{};
};

class StartupChildPpu {
public:
    StartupChildPpu() = default;
    StartupChildPpu(const StartupChildPpu &) = delete;
    StartupChildPpu &operator=(const StartupChildPpu &) = delete;
    StartupChildPpu(StartupChildPpu &&) = delete;
    StartupChildPpu &operator=(StartupChildPpu &&) = delete;
    // No destructor releasing live context by design.

    // Validate everything that needs no resource, then reserve -> storage -> TLS -> per-child admission -> seed.
    // A failure BEFORE the reservation leaves the object Empty. Any failure after it RETAINS the reservation
    // (status().has_resources) for an explicit reap(); nothing is rolled back behind the caller's back.
    StartupChildPpuPrepareResult prepare(const StartupChildPpuPorts &ports, const StartupChildPpuRequest &request);
    // The ps3 thread id the caller must store (BE u64) before start(); 0 until reserved.
    uint32_t id() const { return ref_.id; }
    // Real ps3_ppu_service_start. Blocked is reported only when the service is really blocked in the receive callback.
    // Refused with Cancelled (no host start, object stays Reserved for reap()) once cancel() preceded it.
    StartupChildPpuStartResult start();
    // Owner method (see the threading contract). Side-effect free observation of an already started child: Exited/Returned/
    // Terminal once the real activity is EXITED, Blocked while really blocked in the receive, Running otherwise. NotReserved in
    // every phase but Started. Used to wait, with a bounded injected poll, for a run-to-exit child to end itself.
    StartupChildPpuStartResult poll_exit();
    // Owner method (see the threading contract). Idempotent. No-op while Empty/Released. Otherwise latches the cancel request,
    // sets control.cancelled and ps3_notify_cancel_request on the LV2 notify port (real wake of a blocked receive).
    void cancel();
    // One bounded, non-blocking attempt at the next unfinished release stage; call again after a *Retained or StillRunning
    // status. The service join is attempted only after the real activity is EXITED (cancelled or not).
    StartupChildPpuReapResult reap();
    // Owner method. See the threading contract for which fields are ordered by the child's release stores.
    StartupChildPpuStatus status() const;
    // READ-ONLY terminal control of the child, for per-slot diagnostics. Valid only AFTER reap() returned Released, or once the
    // caller has observed the child EXITED (the release store of the terminal record orders every field read here). The
    // composition never uses it for its own logic.
    const StartupControl &control() const { return control_; }

private:
    static void service_body(ps3_ppu_service *self, void *argument);
    static void terminal(void *context, StartupControl *control);
    static StartupEventReceiveResult receive(void *context, uint32_t queue_id, uint64_t timeout_us);
    void publish_exit_once(StartupControl *control);
    StartupEventReceiveResult receive_impl(uint32_t queue_id, uint64_t timeout_us);
    StartupEventReceiveResult fail(StartupEventReceiveOutcome outcome, StartupChildPpuReceiveFault why, uint32_t rc);

    StartupChildPpuPorts ports_{};
    StartupChildPpuRequest request_{};
    StartupChildPpuPhase phase_ = StartupChildPpuPhase::Empty;
    ps3_ppu_ref ref_{};
    ps3_ppu_guest_storage storage_{};
    Ps3TlsImage tls_{};
    Ps3PpuExecutionServices services_{};
    StartupControl control_{};
    StartupExecution execution_{};
    StartupEventReceivePort receive_port_{};
    ps3_notify_cancel token_{};
    unsigned char tls_scratch_[256]{};
    bool host_started_ = false;
    uint32_t exit_code_ = 0;
    std::atomic<ps3_ppu_service *> service_{nullptr};
    std::atomic<bool> cancel_requested_{false}, exit_published_{false}, discarded_event_{false};
    std::atomic<unsigned> terminal_calls_{0}, receive_calls_{0}, consumed_count_{0};
    std::atomic<uint32_t> last_queue_{0}, last_native_rc_{0};
    std::atomic<uint64_t> last_pc_{0};
    std::atomic<StartupChildPpuReceiveFault> fault_{StartupChildPpuReceiveFault::None};
    ps3_lv2_event discarded_{};
    ps3_lv2_event first_consumed_{};   // write-once by the child thread BEFORE consumed_count_ goes 0 -> 1 (release)
};
#endif
