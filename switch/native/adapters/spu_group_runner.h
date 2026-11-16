#ifndef PS3_NATIVE_SPU_GROUP_RUNNER_H
#define PS3_NATIVE_SPU_GROUP_RUNNER_H
#include "spu_executor.h"
#include "../domain/lv2_objects.h"
#include "../domain/lv2_spu.h"
#include "../../../ps3recomp/runtime/time/clock.h"
#include "../domain/service_thread.h"
#include "../spu_lifted/spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Decision 33 / C1b: run one lv2 SPU thread group on the C1a interpreter.
 *
 * One ps3_spu_group_runner owns an interpreter owner per initialized thread of one started group. Each owner
 * borrows that thread's 256 KiB local store from lv2_spu (the composition's buffer, which outlives the run) and the
 * caller's atomic domain, so its DMA and atomics see exactly the memory the PPU sees. Each thread runs on its own
 * host thread through the composition's ps3_service_thread_port.
 *
 * Nothing here invents an SPU result: a thread ends either by an lv2 stop the SHIM below serves (0x102 exit, 0x101
 * group exit, 0x110 receive-event) or by a named terminal, and both are recorded so the PPU side can see them.
 *
 * Threading contract: start/join/terminate/cancel are called from the owning (PPU) thread only. `state` fields are
 * written by the worker and read by the owner under __atomic, so a reader never needs the worker to be stopped. */

/* What a single SPU thread ended with. */
typedef enum {
    PS3_SPU_THREAD_IDLE = 0,      /* never ran (not initialized at start, or the host thread failed to start) */
    PS3_SPU_THREAD_RUNNING,       /* the worker is executing instructions */
    PS3_SPU_THREAD_EXITED,        /* stop 0x102: exit_status is valid */
    PS3_SPU_THREAD_GROUP_EXIT,    /* stop 0x101: the thread asked the whole group to end */
    PS3_SPU_THREAD_FAULT,         /* a named terminal: `code` is the executor code, `channel` names the operation */
    PS3_SPU_THREAD_CANCELLED      /* the owner asked it to stop (177/193/cancel latch); no invented result */
} ps3_spu_thread_state;

typedef struct ps3_spu_thread_run ps3_spu_thread_run;
struct ps3_spu_thread_run {
    ps3_spu_executor executor;
    ps3_lv2_spu_thread *thread;   /* borrowed domain thread (LS, entry, args, spup bindings) */
    unsigned initialized;
    /* Worker bookkeeping, written only by the worker. */
    uint32_t pc, last_channel;
    uint64_t instructions;
    unsigned pauses, events_sent, events_received, receive_wait;
    unsigned events_busy, events_notconn;      /* Decision 112: user events refused (queue full) / not delivered (no queue bound to the port) */
    uint32_t last_event_queue, last_event_spup, last_event_data0;
    uint32_t exit_status;
    uint32_t terminal_code;       /* the executor code (or lv2 stop) that ended a FAULT/CANCELLED thread */
    ps3_atomic_owner owner;       /* the borrowed atomic owner, released after the join */
    unsigned owner_acquired;
    /* Cross-thread: the owner polls `state`; the worker publishes it with a release store after every field above. */
    unsigned state;
    unsigned stop_requested;
    unsigned traced_channel;      /* root: last channel a pause was traced for (worker only) */
    unsigned host_started;        /* root (C1b): set only when the service-thread port started this worker */
    /* Hybrid lifted execution (native/spu_lifted/spu_lifted_hybrid.h): non-NULL when this thread's freshly loaded local
     * store matched a known lifted module by content hash; every other thread runs on the interpreter exactly as before.
     * The services and the statistics are touched only by the worker. */
    const ps3_spu_lifted_module *lifted;
    ps3_spu_lifted_services lifted_services;
    ps3_spu_lifted_stats lifted_stats;
    /* Decision 104: a thread whose image is not a static lifted module runs through the dynamic job layer (ps3_spu_lifted_run_slice_dyn:
     * region accounting, and lifted job modules when the runner was given a module set). dyn_on != 0 selects it. The accounting
     * slot of a static module's thread (the Bink decoder) is acct_static_slot. */
    unsigned dyn_on;
    ps3_spu_dyn dyn;
    unsigned acct_static_slot;
    uint64_t acct_enters_seen;
    /* Round 2 (freeze diagnostics, console run rv39s): where is this worker right now? Written by the worker with relaxed atomics, read
     * by the monitor thread (ps3_spu_run_state_report / ps3_spu_run_watch). Times are CLOCK_MONOTONIC nanoseconds, 0 = none. */
    uint64_t slice_begin_ns;                 /* the slice in progress began here; 0 between slices (a slice that never ends = a host-level block or a lifted spin) */
    uint64_t slice_count;
    uint64_t wait_since_ns, wait_last_ns;    /* the current stall streak (same channel, retried at most 50 ms apart): began / last retry */
    uint32_t wait_channel;
    unsigned wait_reported, terminal_reported;       /* monitor only: the immediate line for this streak / this terminal was printed */
    uint64_t slice_reported_ns;              /* monitor only: slice_begin_ns the in-slice line was printed for */
    /* Spurious lock-line-reservation-lost events (services.spurious_lr_us; worker only, read by the monitor): delivered, how many were followed by more than
     * SPURIOUS_WORK_INSTRUCTIONS instructions before the thread parked again (= it found work), the instructions all of them cost, the state of the last one. */
    uint64_t spurious_lr, spurious_useful, spurious_instr_total, spurious_last_ns, spurious_mark;
    unsigned spurious_outstanding;
};

enum { PS3_SPU_GROUP_RUNNER_MAX_THREADS = PS3_LV2_SPU_THREADS_MAX };
/* One per host thread: the service-thread port hands this pointer back on join, so it outlives the worker. */
typedef struct ps3_spu_worker_argument { struct ps3_spu_group_runner *runner; struct ps3_spu_thread_run *run; } ps3_spu_worker_argument;

typedef struct {
    void *context;
    /* One service-thread port per SPU thread (a ps3_service_thread_port owns exactly one thread: start once, join
     * once). `count` entries must be valid, i.e. at least the group's thread count. */
    const ps3_service_thread_port *threads;
    unsigned thread_port_count;
    /* The composition's atomic domain and the memory services its executors use. */
    ps3_atomic_domain *domain;
    ps3_spu_executor_services services;
    /* The lv2 objects the SPU-to-PPU events go to, and this group's threads' spup -> queue bindings (251/191). */
    ps3_lv2_objects *objects;
    /* The lv2 SPU domain that owns the threads (LS, entry, args, spup_queue). */
    ps3_lv2_spu_groups *groups;
    /* The owner's clock: the same one the rest of the composition uses, so RdDec advances on guest time. */
    ps3_clock clock;
    /* Slicing: instructions per run_interp call, and how long a paused thread sleeps before retrying. */
    uint64_t slice_instructions;
    uint32_t pause_sleep_microseconds;
    /* Cancellation latch (the process discovery deadline / stop request). Polled at every slice boundary. */
    const unsigned *cancel_flag;
    /* Optional diagnostics sink: one line per start/pause/stop/event. NULL in composition. */
    void (*trace)(void *context, const char *line);
    /* Lifted modules a thread may run instead of the interpreter (NULL / 0: interpreter only, the pre-existing behaviour). */
    const ps3_spu_lifted_module *const *lifted_modules;
    unsigned lifted_module_count;
    /* Dynamic job modules (Decision 104): the set ps3_spu_lifted_run_slice_dyn may arm in the job window. count 0 with
     * job_accounting set = accounting only. Without either, a thread that is not a static module is a plain interpreter owner. */
    const ps3_spu_lifted_module *const *job_modules;
    unsigned job_module_count;
    unsigned job_accounting;
    /* Round 2 safety net (console run rv47s: loading stalled for good with all five instance-0 SPURS threads parked in the kernel's event wait). A thread that has
     * been parked in SPU_RdEventStat for this many microseconds with the lock-line-reservation-lost bit (0x400) in its event mask and nothing pending is given a
     * reservation-lost event, exactly as a real loss latches it (and its reservation is dropped, as after a real loss). On hardware a reservation can be lost for
     * reasons the program cannot see, so SPURS re-reads its line and re-checks for work after every one: a spurious loss is safe and bounds any lost wake-up.
     * 0 = off (the default of every host test); the composition sets PS3_SPU_SPURIOUS_LR_US (lle.mk LLE_SPU_SPURIOUS_LR). */
    unsigned spurious_lr_us;
} ps3_spu_group_runner_services;

typedef struct ps3_spu_group_runner ps3_spu_group_runner;
struct ps3_spu_group_runner {
    ps3_spu_group_runner_services services;
    ps3_lv2_spu_group *group;                    /* borrowed */
    uint32_t group_id, num;
    ps3_spu_thread_run runs[PS3_SPU_GROUP_RUNNER_MAX_THREADS];
    ps3_spu_worker_argument worker_arguments[PS3_SPU_GROUP_RUNNER_MAX_THREADS];
    unsigned started, joined, prepared, threads_started;
    /* The group exit cause recorded for sys_spu_thread_group_join (0 until every thread has ended). */
    uint32_t join_cause, join_status;
};

/* Zeroes *runner and binds it to `group_id` of `services.groups`. Every field of `services` except `trace` is
 * mandatory. INVALID for a NULL/bad argument or an unstarted group; the runner is then still safe to release. */
int ps3_spu_group_runner_init(ps3_spu_group_runner *, const ps3_spu_group_runner_services *, uint32_t group_id);
/* Starts one host thread per initialized thread of the group (pc = entry, r3..r6 = arg1..arg4). Returns OK, or the
 * executor code of the first thread that could not be prepared (every thread prepared so far stays stopped and
 * flagged IDLE, nothing is left running). Idempotent: a second call on a started runner returns OK. */
int ps3_spu_group_runner_start(ps3_spu_group_runner *);
/* Asks every worker to stop at its next slice boundary and waits for all host threads. Safe on a never-started
 * runner. `cancel_flag` is set too, so a worker inside a long slice converges. */
int ps3_spu_group_runner_terminate(ps3_spu_group_runner *);
/* Bounded, cancellable wait for every thread to end. Writes the lv2 join cause and status. The cause is
 * PS3_SPU_GROUP_JOIN_ALL_THREADS_EXIT, or GROUP_EXIT when any thread ended with stop 0x101. */
enum { PS3_SPU_GROUP_JOIN_ALL_THREADS_EXIT = 0x2, PS3_SPU_GROUP_JOIN_GROUP_EXIT = 0x1 };
int ps3_spu_group_runner_join(ps3_spu_group_runner *, uint32_t *cause, uint32_t *status);
/* True when every initialized thread has a terminal. */
int ps3_spu_group_runner_finished(const ps3_spu_group_runner *);
/* The exit status stop 0x102 recorded for one SPU thread (by lv2 thread id), or ESTAT when it has not exited. */
int ps3_spu_group_runner_thread_exit_status(const ps3_spu_group_runner *, uint32_t thread_id, uint32_t *status);
/* The per-thread state record (for the result printer). NULL for an index the group does not have. */
const ps3_spu_thread_run *ps3_spu_group_runner_thread(const ps3_spu_group_runner *, uint32_t index);
const char *ps3_spu_thread_state_name(unsigned state);
/* Round 2 (freeze diagnostics): STARTUP_SPU_STATE lines for every prepared thread of one runner, and the once-each immediate lines (STARTUP_SPU_TERMINAL for a thread
 * that ended, STARTUP_SPU_BLOCKED kind=channel / kind=in_slice for one stalled on a channel or inside one slice for longer than blocked_ns). Safe while the workers run. */
void ps3_spu_group_runner_state_report(const ps3_spu_group_runner *, void (*print)(const char *line), unsigned seconds);
unsigned ps3_spu_group_runner_watch(ps3_spu_group_runner *, void (*print)(const char *line), unsigned seconds, uint64_t blocked_ns);
#ifdef __cplusplus
}
#endif
#endif
