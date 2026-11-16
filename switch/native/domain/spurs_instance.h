#ifndef NATIVE_SPURS_INSTANCE_H
#define NATIVE_SPURS_INSTANCE_H
#include <stddef.h>
#include <stdint.h>
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#include "lv2_objects.h"
#include "ppu_threads.h"
#include "spu_resource_policy.h"
#include "spurs_kernel1_service.h"
#include "spurs_workload_registration.h"
#include "spurs_idle_request.h"
#include "spurs_exception_handler.h"
#include "spurs_ready_count.h"
#include "spurs_workload_info.h"
#include "service_thread.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Native cellSpursInitializeWithAttribute (aa6269a8), v1 extent, S0-S16 of the approved N12
 * scope (CONTRACT-CLOSURE W0-W36, KERNEL-IDLE-CONTRACT). Everything is a real owned object:
 * lv2 semaphores/queue/port, SPU group/threads, SERVICE PPU threads for the event helper and
 * the handler, and the kernel-1 idle service executing on the group's atomic owners over the
 * real coherent instance. OK is returned only after the helper really blocked in its queue
 * receive, the handler really blocked in the group join after every context executed to its L0
 * reservation wait, and every W-table write was issued. Reference-intent reconstruction
 * (I1/I3), not a binary oracle; anything outside the supported subset STOPs, never a fake rc.
 *
 * Ownership: the runtime and its instance records are caller-owned, zero-filled storage,
 * constructed once, and stay stable after every call (service threads and the kernel worker keep
 * pointing into them). No globals. Lifetime: ps3_spurs_runtime_shutdown must succeed (return 0)
 * before the guest memory, domain, lv2/policy/ppu objects, notify port or LS buffers are
 * released. Priority 250/249 (main/services) is native scheduling policy [R-PRIO]. One shared
 * notify port serves domain, lv2, policy, ppu and the kernel service. Instance bytes are never
 * restored on failure (reference behavior); resources are returned in reverse order. */
enum {
    PS3_SPURS_MAX_INSTANCES = 2, PS3_SPURS_LS_SLOTS = 8, PS3_SPURS_SEMAPHORES = 17,
    PS3_SPURS_EXTENT = 0x1000, PS3_SPURS_ATTRIBUTE_BYTES = 512
};
enum {   /* CELL_SPURS_CORE_ERROR_* */
    PS3_SPURS_RC_OK = 0, PS3_SPURS_RC_AGAIN = 0x80410701u, PS3_SPURS_RC_INVAL = 0x80410702u,
    PS3_SPURS_RC_PERM = 0x80410709u, PS3_SPURS_RC_BUSY = 0x8041070Au, PS3_SPURS_RC_STAT = 0x8041070Fu, PS3_SPURS_RC_ALIGN = 0x80410710u,
    PS3_SPURS_RC_NULL = 0x80410711u
};
typedef enum { PS3_SPURS_THREAD_HELPER = 0, PS3_SPURS_THREAD_HANDLER, PS3_SPURS_THREAD_KERNEL } ps3_spurs_thread_role;
/* lwmutex/lwcond creation at guest addresses (adapters over the sync registries). Return an lv2
 * rc (0 = OK); the adapter writes the 24/8-byte guest images itself. */
typedef struct ps3_spurs_sync_port {
    void *context;
    uint32_t (*lwmutex_create)(void *context, uint32_t address, uint32_t protocol, uint32_t recursive);
    uint32_t (*lwmutex_destroy)(void *context, uint32_t address);
    uint32_t (*lwcond_create)(void *context, uint32_t address, uint32_t mutex_address);
    uint32_t (*lwcond_destroy)(void *context, uint32_t address);
} ps3_spurs_sync_port;
/* Optional guest-write sink for PPU-side writes (the bridge traces them). NULL write = direct
 * coherent ps3_atomic_write. Nonzero return = the write was not performed -> STOP. */
typedef struct ps3_spurs_write_port {
    void *context;
    int (*write)(void *context, uint32_t address, const void *bytes, size_t length);
} ps3_spurs_write_port;
typedef enum {
    PS3_SPURS_INSTANCE_FREE = 0, PS3_SPURS_INSTANCE_BUILDING, PS3_SPURS_INSTANCE_ACTIVE,
    PS3_SPURS_INSTANCE_RETAINED   /* terminal STOP: resources retained until shutdown */
} ps3_spurs_instance_state;
enum { PS3_SPURS_HANDLER_NONE = 0, PS3_SPURS_HANDLER_KERNEL_STOPPED, PS3_SPURS_HANDLER_KERNEL_CANCELLED,
       PS3_SPURS_HANDLER_START_FAILED, PS3_SPURS_HANDLER_BLOCKED_JOIN, PS3_SPURS_HANDLER_JOINED, PS3_SPURS_HANDLER_JOIN_CANCELLED };
struct ps3_spurs_runtime;
typedef struct ps3_spurs_instance {
    struct ps3_spurs_runtime *runtime;
    uint32_t index, state;
    uint32_t spurs_ea, nspus, prefix_size, ppu_priority;
    int32_t spu_priority;
    uint32_t semaphores[PS3_SPURS_SEMAPHORES]; unsigned semaphore_count;
    uint32_t group, spu_threads[PS3_SPURS_K1_MAX_SPUS]; unsigned group_created, threads_initialized;
    ps3_atomic_owner owners[PS3_SPURS_K1_MAX_SPUS]; unsigned owners_acquired;
    unsigned char *ls[PS3_SPURS_K1_MAX_SPUS]; uint8_t ls_slot[PS3_SPURS_K1_MAX_SPUS];
    unsigned lwmutex_created, lwcond_created;
    uint32_t queue, port, spup;
    uint64_t helper_stop_source;  /* canonical owned port source, captured before helper start */
    unsigned queue_created, attached, port_created, port_connected, queue_bound, exception_connected;
    ps3_ppu_ref helper, handler; unsigned helper_live, handler_live;
    ps3_notify_cancel helper_cancel, handler_cancel;
    ps3_spurs_k1_service kernel; unsigned kernel_initialized;
    ps3_spurs_k1_stop kernel_stop;      /* valid once handler_status == KERNEL_STOPPED */
    uint64_t handler_status;            /* PS3_SPURS_HANDLER_*, atomic */
    unsigned unwinding_failed;
    /* Ledger of guest (cellSpursAttachLv2EventQueue) bindings this instance made on its group: one borrowed queue id per
     * SPU port, bit n of user_port_bits set while port n is recorded. Recorded immediately after the connect succeeds
     * and cleared only after an exact detach is CONFIRMED; the queue itself is never owned or destroyed here. */
    uint64_t user_port_bits;
    uint32_t user_port_queue[PS3_SPU_PORTS];
} ps3_spurs_instance;
typedef struct ps3_spurs_runtime_config {
    ps3_atomic_domain *domain;
    ps3_notify_port notify;             /* the ONE port shared by domain/lv2/policy/ppu */
    ps3_lv2_objects *lv2;
    ps3_spu_resource_policy *policy;    /* attached to the same user memory and lv2 */
    ps3_ppu_threads *ppu;
    ps3_spurs_sync_port sync;
    ps3_spurs_write_port writer;
    ps3_service_thread_port threads[PS3_SPURS_MAX_INSTANCES][3];   /* helper, handler, kernel worker */
    unsigned char *local_stores[PS3_SPURS_LS_SLOTS];               /* caller-owned, PS3_SPURS_K1_LS_BYTES each */
    uint32_t creator_priority;          /* 250 */
} ps3_spurs_runtime_config;
/* One terminal helper observation, immutable after release-publication of ready.
 * event is valid only for HELPER_EVENT; receive failure records no dequeued event.
 * Retained through shutdown, reset only before a FREE record's next construction. */
typedef enum { PS3_SPURS_STOP_CATEGORY_KERNEL = 0, PS3_SPURS_STOP_CATEGORY_HELPER_EVENT,
    PS3_SPURS_STOP_CATEGORY_HELPER_RECEIVE_FAILURE } ps3_spurs_stop_category;
typedef struct ps3_spurs_helper_stop_capture {
    uint64_t ready;
    uint32_t spurs_ea, queue;
    ps3_spurs_stop_category category;
    uint32_t receive_status;
    ps3_lv2_event event;
} ps3_spurs_helper_stop_capture;
typedef struct ps3_spurs_runtime {
    ps3_spurs_runtime_config config;
    unsigned ls_used[PS3_SPURS_LS_SLOTS];
    ps3_spurs_instance instances[PS3_SPURS_MAX_INSTANCES];
    unsigned initialized;
    ps3_instance_guard workload_guards[PS3_SPURS_MAX_INSTANCES];
    ps3_spurs_policy_execution_port workload_execution[PS3_SPURS_MAX_INSTANCES];
    unsigned workload_services_configured;
    ps3_spurs_helper_stop_capture helper_stops[PS3_SPURS_MAX_INSTANCES];
} ps3_spurs_runtime;
/* One-shot, before any instance is BUILDING/ACTIVE/RETAINED. Copies ports/guards;
 * caller retains their contexts until successful runtime shutdown. Config ABI is unchanged. */
int ps3_spurs_runtime_set_workload_services(ps3_spurs_runtime *,
    const ps3_instance_guard guards[PS3_SPURS_MAX_INSTANCES],
    const ps3_spurs_policy_execution_port ports[PS3_SPURS_MAX_INSTANCES]);
/* Borrowed PPU owner must remain live and serialized by caller. No services or no
 * ACTIVE owned instance stops before claim. Successful effects on native stop are
 * retained in the ACTIVE graph; caller must stop/join through runtime shutdown. */
void ps3_spurs_runtime_register_workload(ps3_spurs_runtime *, const ps3_spurs_registration_request *,
    ps3_atomic_owner borrowed_ppu_owner, unsigned max_attempts,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_registration_event *),
    ps3_spurs_registration_result *);
typedef enum { PS3_SPURS_OUTCOME_OK = 0, PS3_SPURS_OUTCOME_FAILED, PS3_SPURS_OUTCOME_STOP } ps3_spurs_outcome;
typedef enum {
    PS3_SPURS_STOP_NONE = 0, PS3_SPURS_STOP_UNKNOWN_REGION, PS3_SPURS_STOP_UNSUPPORTED_ATTRIBUTE,
    PS3_SPURS_STOP_INSTANCE_CAPACITY, PS3_SPURS_STOP_UNSUPPORTED_LV2, PS3_SPURS_STOP_GROUP_REJECTED,
    PS3_SPURS_STOP_WRITE_FAILED, PS3_SPURS_STOP_ATTACH_FAILED, PS3_SPURS_STOP_SERVICE_ANOMALY,
    PS3_SPURS_STOP_KERNEL, PS3_SPURS_STOP_EXCEPTION_CONNECT_FAILED, PS3_SPURS_STOP_PROVIDER_FAILURE,
    PS3_SPURS_STOP_ROLLBACK_FAILED, PS3_SPURS_STOP_INVALID_REQUEST,
    PS3_SPURS_STOP_READ_FAILED,  /* attach memory port: a guest read was not performed */
    /* appended for cellSpursGetInfo (existing values unchanged) */
    PS3_SPURS_STOP_INSTANCE_NOT_ACTIVE,   /* get_info: owned instance is BUILDING/RETAINED; detail = state */
    PS3_SPURS_STOP_OUTPUT_OVERLAP,        /* get_info: [info,info+0x118) intersects [spurs,spurs+0x1000); detail = info_ea */
    PS3_SPURS_STOP_GUEST_STATE,           /* get_info: unsupported guest value (trace ptr high32 / prefix size > 15); detail = low 32 bits */
    PS3_SPURS_STOP_PREFLIGHT_REFUSED,     /* get_info: info range wraps (detail 0) or preflight callback nonzero (detail = its code) */
    PS3_SPURS_STOP_UNEXPECTED_HELPER_EVENT, PS3_SPURS_STOP_HELPER_RECEIVE_FAILURE
} ps3_spurs_stop_reason;
typedef struct ps3_spurs_result {
    ps3_spurs_outcome outcome;
    uint32_t rc;                        /* r3 for OK/FAILED */
    uint32_t reason, step, instance;    /* STOP: ps3_spurs_stop_reason, S-step, instance record */
    uint32_t detail;                    /* lv2/ppu rc or kernel stop reason */
    uint32_t kernel_spu, kernel_step;
} ps3_spurs_result;
/* attribute: the 512 attribute bytes read by the caller, or NULL when attr_ea is NULL/misaligned
 * (steps S0.1/S0.2 answer first and the bytes are never consulted). */
/* Same ACTIVE graph and immutable service lifetime as registration; no owner is
 * borrowed because this operation performs only coherent reads and one byte write. */
void ps3_spurs_runtime_request_idle_spu(ps3_spurs_runtime *, const ps3_spurs_idle_request *,
    ps3_spurs_idle_memory_scope,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_idle_event *),
    ps3_spurs_idle_result *);
/* Primary lifecycle calls remain caller-serialized. No whole-operation guard:
 * borrowed owner retains authentic reservation/unset interleavings. Runtime and
 * caller polls fence before memory accesses and final SDK publication; committed
 * effects remain after a native stop. Runtime owns only the SPURS1 extent. */
void ps3_spurs_runtime_exception_handler(ps3_spurs_runtime *, const ps3_spurs_exception_request *,
    ps3_spurs_exception_memory_scope, ps3_atomic_owner borrowed_ppu_owner, size_t max_claim_attempts,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_exception_event *),
    ps3_spurs_exception_result *);
/* Same owned SPURS1 lifecycle/scoped checkpoints as exception registration.
 * One ordinary coherent byte publication; no retry, owner acquisition or
 * scheduler wake hook. A committed store remains after a native stop. */
void ps3_spurs_runtime_ready_count_store(ps3_spurs_runtime *, const ps3_spurs_ready_count_request *,
    ps3_spurs_ready_count_memory_scope, ps3_atomic_owner borrowed_ppu_owner,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_ready_count_event *),
    ps3_spurs_ready_count_result *);
/* Read/query over a stable ACTIVE owned graph; no policy-execution service prerequisite.
 * Output may be any scoped RAM, including aliases. Each confirmed effect is retained
 * on a later caller/kernel/helper fence; no whole-call guard or snapshot. */
void ps3_spurs_runtime_get_workload_info(ps3_spurs_runtime *, const ps3_spurs_workload_info_request *,
    ps3_spurs_workload_info_memory_scope, ps3_atomic_owner borrowed_ppu_owner,
    void *poll_context, int (*may_continue)(void *),
    void *observer_context, int (*observe)(void *, const ps3_spurs_workload_info_event *),
    ps3_spurs_workload_info_result *);

typedef struct ps3_spurs_request { uint32_t attribute_ea, spurs_ea; const uint8_t *attribute; } ps3_spurs_request;
/* Zero-filled storage, once. Returns 0 on success, nonzero for a bad config (including a ppu registry that has
 * not adopted the real main thread id, L5) or a second init. */
int ps3_spurs_runtime_init(ps3_spurs_runtime *, const ps3_spurs_runtime_config *);
ps3_spurs_result ps3_spurs_runtime_initialize(ps3_spurs_runtime *, const ps3_spurs_request *);
/* No callback/delivery claim. Kernel precedence within each ascending instance.
 * reason/spu/step retain kernel meanings only for category KERNEL. */
typedef struct ps3_spurs_stop_info {
    uint32_t instance, reason, spu, step;
    ps3_spurs_stop_category category;
    uint32_t spurs_ea, queue;
    ps3_lv2_event event;
    uint32_t receive_status;
    unsigned has_event; /* event is an actual dequeued event only when true */
} ps3_spurs_stop_info;
int ps3_spurs_runtime_poll_stop(ps3_spurs_runtime *, ps3_spurs_stop_info *);
/* Native cellSpursAttachLv2EventQueue (attach_lv2_eq, RPCS3 63996a47 cellSpurs.cpp:530-585) for a STATIC attach by the
 * guest to an instance this runtime owns. Every field of the request is explicit; nothing is looked up or defaulted:
 *   spurs_ea, port_ea  guest addresses (already canonical; 0 -> NULL, spurs_ea % 128 -> ALIGN, in that order, before any
 *                      ownership lookup); the port byte is read from port_ea and rewritten with the bound SPU port
 *   queue              lv2 event queue id, BORROWED from its owner (never destroyed, drained, sent to or woken here)
 *   is_dynamic         STATIC mode (0): the port byte at port_ea is READ as the requested port and rewritten with the bound
 *                      one. DYNAMIC mode (nonzero, the firmware's `0xAF2C` requested-port-branch NOT taken: libsre passes
 *                      r6 = 1 from 0xB2B4): port_ea is an OUTPUT ONLY. Nothing is read from it; the request mask is the
 *                      fixed PS3_SPURS_DYNAMIC_PORT_MASK (0xFFFFFFFFFFFF0000, SPU-thread ports 16..63) and the lv2 connect
 *                      picks the lowest free port in it, exactly as syscall 251 does for the firmware. The chosen port is
 *                      written to port_ea. A
 *                      dynamic attach performs NO guest DA8 write: the firmware's keyed path (`0xB0D0` -> `r7 = 1`) skips
 *                      the 0xDA8 bit update at 0xB074, so the guest port bitmap is left exactly as it was.
 *   port_ea           STATIC: readable before the connect. DYNAMIC: must be non-NULL (the firmware's gate 2 answers NULL with
 *                      0x80410711) and writable. Either mode: the guest address holding the bound SPU port.
 *   sdk_version        the EFFECTIVE PROCESS sdk (reference get_sdk_version: s32 g_ps3_process_info.sdk_ver, raw -1 mapped to
 *                      0x485000 by the ADAPTER). It is NEVER the CellSpurs.sdkVersion at +0xDA4 (guest-writable attribute data) and the
 *                      adapter must not read it from guest memory. Kept as uint32; valid range 0..INT32_MAX, where >= 0x180000 limits the
 *                      port to 0..15 (PERM). An unmapped negative (> INT32_MAX, e.g. raw -1 = 0xFFFFFFFF) is a host-contract error: STOP
 *                      INVALID_REQUEST at step REQUEST before any callback (like max_cas_attempts == 0). No full negative-SDK model.
 *   max_cas_attempts   bound on the strong-CAS loop that ORs DA8; zero is rejected before any effect
 * Guest memory is reached only through `memory` (the same coherent-domain contract as the atomic domain: values are
 * host-order, bytes are big-endian; the existing runtime writer/config is not involved). All three callbacks are
 * mandatory. A nonzero read/write return means NOTHING was performed. compare_exchange64 mirrors
 * ps3_atomic_compare_exchange64: it returns the observed value and whether it exchanged; a nonzero return with
 * exchanged == 0 means nothing was performed, while exchanged == 1 means the OR is committed whatever the return (e.g. a
 * trace recorder failing after the commit). That is NOT success: the commit is never undone, but the attach STOPs
 * (WRITE_FAILED, step COMMITTED, detail = the callback code), with or without info. OK only when the committed callback returned 0.
 * Results:
 *   FAILED rc  guest-visible: NULL / ALIGN / STAT (exception != 0) / INVAL (port > 63, static only) / PERM (static only) /
 *              BUSY (EISCONN) / ESRCH (absent queue, step QUEUE) / any other lv2 connect code UNCHANGED (e.g. ESTAT). No
 *              effect of any kind.
 *   STOP       unsupported or native fault; pre-effect stops (bad request/callbacks/cap, unowned or not ACTIVE instance,
 *              a service-bound/internal/non-PPU queue, memory-port failure before the connect) leave the instance untouched.
 *              After the connect, any failure rolls the binding back with the exact detach (record cleared only when that
 *              is confirmed; otherwise ROLLBACK_FAILED and the record stays) and the instance becomes RETAINED; guest
 *              residue (the port byte) is reported in info. The ledger/rollback index is the validated requested port in
 *              STATIC mode and the CHOSEN port in DYNAMIC mode; a policy answering OK with a different spup is rolled back
 *              on the requested port and STOPs PROVIDER_FAILURE (step CONNECT, detail = the bad spup) -- static only: in
 *              DYNAMIC mode the policy's answer IS the requested port, so a mismatch cannot arise.
 *              An ABSENT queue (snapshot ESRCH) is FAILED ESRCH at step QUEUE before the DA8 read and the connect (instance
 *              stays ACTIVE); any other unexpected snapshot status STOPs PROVIDER_FAILURE at step QUEUE.
 *              A committed CAS whose callback then failed is NOT rolled back: STOP WRITE_FAILED at step COMMITTED (new,
 *              appended after DONE); DA8 bit, port byte, ledger and every SPU binding stay, the instance is RETAINED and
 *              shutdown detaches the bindings exactly (the DA8 residue is not cleared by SPURS shutdown).
 *   OK         attached: every thread binds the queue, *port == port, DA8 |= 1 << port, record kept (committed CAS callback
 *              returned 0). DYNAMIC mode: every thread binds the queue, *port == the chosen port, DA8 unchanged, record
 *              kept; info.cas_attempts == 0 and prior_port_bits/prior_bit_set/commit_status stay 0.
 * info is optional and is filled even on failure. Runs on the primary guest thread, like initialize. */
typedef struct ps3_spurs_attach_memory {
    void *context;
    int (*read)(void *context, uint32_t address, void *bytes, size_t length);
    int (*write)(void *context, uint32_t address, const void *bytes, size_t length);
    int (*compare_exchange64)(void *context, uint32_t address, uint64_t expected, uint64_t desired, ps3_atomic_compare_result *result);
} ps3_spurs_attach_memory;
typedef struct ps3_spurs_attach_request {
    uint32_t spurs_ea, queue, port_ea, is_dynamic, sdk_version, max_cas_attempts;
} ps3_spurs_attach_request;
/* The dynamic-mode request mask: SPU-thread ports 16..63, every bit set (libsre 0xAFDC..0xB038 builds exactly this). */
#define PS3_SPURS_DYNAMIC_PORT_MASK UINT64_C(0xFFFFFFFFFFFF0000)
typedef struct ps3_spurs_attach_info {
    uint8_t spup;                 /* the port bound (valid once the connect succeeded) */
    unsigned port_written;        /* the guest port byte was written: residue if a later step stopped */
    uint32_t cas_attempts;        /* compare_exchange64 calls made */
    uint64_t prior_port_bits;     /* DA8 value the committing CAS replaced (OK, or STOP at step COMMITTED) */
    unsigned prior_bit_set;       /* bit `spup` was already set in prior_port_bits */
    int commit_status;            /* the committed CAS's callback return: nonzero = STOP at step COMMITTED (the commit stands) */
    uint32_t cause;               /* rollback only: the ps3_spurs_stop_reason that triggered it (0 for STEP_COMMITTED: no rollback) */
} ps3_spurs_attach_info;
enum {   /* result.step of the attach (initialize uses 0..16) */
    PS3_SPURS_ATTACH_STEP_REQUEST = 20, PS3_SPURS_ATTACH_STEP_OWNER, PS3_SPURS_ATTACH_STEP_EXCEPTION, PS3_SPURS_ATTACH_STEP_DYNAMIC,
    PS3_SPURS_ATTACH_STEP_PORT_READ, PS3_SPURS_ATTACH_STEP_PORT_RANGE, PS3_SPURS_ATTACH_STEP_QUEUE, PS3_SPURS_ATTACH_STEP_CONNECT,
    PS3_SPURS_ATTACH_STEP_PORT_WRITE, PS3_SPURS_ATTACH_STEP_BITS_READ, PS3_SPURS_ATTACH_STEP_BITS_CAS, PS3_SPURS_ATTACH_STEP_DONE,
    PS3_SPURS_ATTACH_STEP_COMMITTED   /* appended: existing values unchanged */
};
/* ps3_spurs_attach_info.spup in dynamic mode is always the CHOSEN port (16..63). No value of `step` is appended for the
 * dynamic mode: it reuses DYNAMIC (host-contract ordering only, no longer a refusal), QUEUE, CONNECT, PORT_WRITE, DONE and
 * COMMITTED. PS3_SPURS_ATTACH_STEP_DYNAMIC is therefore only reached as a host-contract STOP for an out-of-range
 * is_dynamic value, which no caller has a reason to send. */
ps3_spurs_result ps3_spurs_runtime_attach_lv2_event_queue(ps3_spurs_runtime *, const ps3_spurs_attach_request *,
                                                          const ps3_spurs_attach_memory *, ps3_spurs_attach_info *);
/* Native cellSpursGetInfo (NID 0x1f402f8f; RPCS3 63996a47 cellSpurs.cpp:1970-2007, CellSpursInfo cellSpurs.h:308-329).
 * Read-only query of an instance this runtime owns. Output VALUES come from GUEST memory (reference source of truth); the
 * instance record is used only for ownership/state and the diagnostic mirror comparison. Order of checks (nothing is called
 * before the preflight; every failure before step PREFLIGHT has no effect of any kind):
 *   host contract -> NULL(spurs||info) -> ALIGN(spurs%128) -> owner -> ACTIVE -> info range -> overlap -> preflight
 *   -> reference-ordered reads/writes.
 * There is NO info alignment check and NO exception-field check (reference). The instance is never mutated or retained.
 * read/write: scalar lengths 1/4/8 only; bytes are raw guest (big-endian) bytes; nonzero return = nothing performed and the
 * domain never calls a callback again afterwards. preflight: called exactly once, before any read/write; nonzero = nothing
 * performed. Output bytes already written are never restored (the reference has no rollback); reserved bytes (+0E..0F,
 * +14..17, +74..117) and the name tail after the NUL are never written (no blanket zeroing).
 * Results:
 *   FAILED  rc 0x80410711 NULL (spurs_ea==0 || info_ea==0, before ALIGN) / 0x80410710 ALIGN (spurs_ea%128), step REQUEST,
 *           instance 0 (no instance consulted), no callback.
 *   STOP    host contract (rt NULL/uninitialized, rq/mem NULL, any callback NULL): INVALID_REQUEST step REQUEST;
 *           unknown spurs_ea: UNKNOWN_REGION step OWNER detail spurs_ea; owned but not ACTIVE: INSTANCE_NOT_ACTIVE step OWNER
 *           detail state; info_ea > 0xFFFFFFFF-0x117: PREFLIGHT_REFUSED step RANGE detail 0; whole-instance overlap:
 *           OUTPUT_OVERLAP step OVERLAP detail info_ea; preflight nonzero: PREFLIGHT_REFUSED step PREFLIGHT detail code.
 *           For every STOP up to and including step PREFLIGHT no read or write was performed and report.partial == 0.
 *           For unknown/host-contract/NULL/ALIGN results result.instance is 0 (result_of(NULL,..), like attach); owner-found
 *           results carry that instance's index.
 *           Late STOPs (reads/writes performed): READ_FAILED / WRITE_FAILED at the step of the field being accessed, detail =
 *           the callback code; GUEST_STATE at step TRACE_BUFFER (guest traceBuffer raw >> 32 != 0; detail = low 32 bits of the
 *           raw value, the full value is in report.trace_buffer_raw) or step PREFIX (guest prefix size > 15, detail = the size).
 *           Prior writes stay; report.partial is set when writes > 0.
 *   OK      rc 0, step DONE: 18+n reads, 23+n writes (n = guest prefix size <= 15), instance still ACTIVE.
 * Deadline meet/miss (+70, +6C) are the reference TODO zeros: compatibility values, NOT
 * deadline telemetry. Access order is normative (see the spurs_get_info test for the literal log). */
enum { PS3_SPURS_INFO_BYTES = 0x118, PS3_SPURS_INFO_MAX_ACCESSES = 71,
       PS3_SPURS_INFO_READ_SPANS = 4, PS3_SPURS_INFO_WRITE_SPANS = 3 };
typedef struct ps3_spurs_span { uint32_t address, length; } ps3_spurs_span;
typedef struct ps3_spurs_info_memory {
    void *context;
    /* Spans are read {spurs+0x076,1} {spurs+0x900,8} {spurs+0x948,8} {spurs+0xD20,0x7C}; write {info+0x00,0x0E}
     * {info+0x10,4} {info+0x18,0x5C}. max_accesses is the worst case (18+15 reads + 23+15 writes = 71). */
    int (*preflight)(void *context, const ps3_spurs_span *reads, size_t read_count,
                     const ps3_spurs_span *writes, size_t write_count, uint32_t max_accesses);
    int (*read)(void *context, uint32_t address, void *bytes, size_t length);
    int (*write)(void *context, uint32_t address, const void *bytes, size_t length);
} ps3_spurs_info_memory;
typedef struct ps3_spurs_info_request { uint32_t spurs_ea, info_ea; } ps3_spurs_info_request;
typedef struct ps3_spurs_info_report {
    uint32_t reads, writes;           /* callbacks that returned 0 */
    uint32_t last_write_offset;       /* info-relative offset of the last performed write; UINT32_MAX = none */
    unsigned partial;                 /* writes > 0 && outcome != OK (residue left in guest output) */
    uint32_t flags;                   /* captured +D80 */
    uint8_t nspus, prefix_size;       /* GUEST-observed +076, +D9B (never used as a host index bound) */
    uint64_t trace_buffer_raw, trace_size;   /* observed +900, +948 */
    uint32_t mirror_mismatch;         /* diagnostic only, never alters bytes/outcome: bit0 nspus, 1 spu_prio, 2 ppu_prio,
                                         3 group, 4 spu ids (k < in->nspus vs in->spu_threads[k], k >= in->nspus vs 0; bounded
                                         by the NATIVE in->nspus <= PS3_SPURS_K1_MAX_SPUS, never by guest nSpus), 5 handler0
                                         vs in->handler.id, 6 handler1 vs in->helper.id, 7 prefix_size. Only bits whose source
                                         was observed are ever set. */
} ps3_spurs_info_report;
enum {   /* result.step of get_info (attach uses 20..31) */
    PS3_SPURS_INFO_STEP_REQUEST = 40, PS3_SPURS_INFO_STEP_OWNER, PS3_SPURS_INFO_STEP_RANGE, PS3_SPURS_INFO_STEP_OVERLAP,
    PS3_SPURS_INFO_STEP_PREFLIGHT, PS3_SPURS_INFO_STEP_FLAGS, PS3_SPURS_INFO_STEP_NSPUS, PS3_SPURS_INFO_STEP_SPU_PRIORITY,
    PS3_SPURS_INFO_STEP_PPU_PRIORITY, PS3_SPURS_INFO_STEP_EXIT_FLAGS, PS3_SPURS_INFO_STEP_GROUP, PS3_SPURS_INFO_STEP_SPU_IDS,
    PS3_SPURS_INFO_STEP_HANDLER0, PS3_SPURS_INFO_STEP_HANDLER1, PS3_SPURS_INFO_STEP_TRACE_SIZE,
    PS3_SPURS_INFO_STEP_TRACE_BUFFER, PS3_SPURS_INFO_STEP_PREFIX, PS3_SPURS_INFO_STEP_COUNTERS, PS3_SPURS_INFO_STEP_DONE
};
/* report is optional; it is zero-filled first with last_write_offset = UINT32_MAX. */
ps3_spurs_result ps3_spurs_runtime_get_info(ps3_spurs_runtime *, const ps3_spurs_info_request *,
                                            const ps3_spurs_info_memory *, ps3_spurs_info_report *);
/* Cancel, wake and join every service/worker and return resources (reverse order). Returns the
 * number of instances that could NOT be fully torn down (a join failed): memory and objects
 * must stay alive then; the call may be retried. */
unsigned ps3_spurs_runtime_shutdown(ps3_spurs_runtime *);
#ifdef __cplusplus
}
#endif
#endif
