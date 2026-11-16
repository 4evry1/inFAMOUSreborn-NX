#ifndef PS3_NATIVE_SPU_RUN_PORT_H
#define PS3_NATIVE_SPU_RUN_PORT_H
/* Decision 33 / C1b (root): the narrow, plain-typed port the PPU-side syscall arms use to run lv2 SPU thread groups.
 *
 * It keeps the SPU executor's build flags (PS3_NATIVE_SPU_EXECUTOR, PS3_SPU_EXTERNAL_FP_CONTROL) out of
 * startup_execution.cpp: everything executor-shaped lives in spu_group_runner.c, which implements this port over a
 * fixed pool of ps3_spu_group_runner records. All results are lv2 CELL codes (0 = CELL_OK, 0x8001xxxx = guest error)
 * or PS3_SPU_RUN_INTERNAL_* (>= 0xFFFF0000), which the caller must turn into a named stop, never into r3. */
#include <stdint.h>
#include "../domain/lv2_objects.h"
#include "../domain/lv2_spu.h"
#include "../domain/service_thread.h"
#include "../../../ps3recomp/runtime/time/clock.h"
#include "../../../ps3recomp/runtime/memory/atomic_domain.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS3_SPU_RUN_MAX_RANGES = 16,
    PS3_SPU_RUN_INTERNAL_NO_RUNNER = 0xFFFF0101u,    /* every runner record is in use */
    PS3_SPU_RUN_INTERNAL_PREPARE = 0xFFFF0102u,      /* no thread could be prepared (owners/executors) */
    PS3_SPU_RUN_INTERNAL_INVALID = 0xFFFF0103u,      /* the port was not configured */
    PS3_SPU_RUN_INTERNAL_JOIN = 0xFFFF0104u,         /* a host join failed */
    PS3_SPU_RUN_INTERNAL_CANCELLED = 0xFFFF0105u     /* the process cancellation ended the join */
};

typedef struct ps3_spu_run_config {
    ps3_atomic_domain *domain;                    /* the PPU's own coherent domain */
    ps3_lv2_objects *objects;                     /* SPU-to-PPU event queues */
    ps3_lv2_spu_groups *groups;                   /* the lv2 SPU registry (LS, entry, args, spup bindings) */
    ps3_clock clock;                              /* guest time for RdDec */
    const ps3_service_thread_port *threads;       /* one port per SPU thread slot */
    unsigned thread_count;
    const unsigned *cancel_flag;                  /* process cancellation latch; polled at slice/pause boundaries */
    /* Guest RAM an SPU DMA may touch (the composition's declared ranges). */
    uint32_t range_base[PS3_SPU_RUN_MAX_RANGES], range_length[PS3_SPU_RUN_MAX_RANGES];
    unsigned range_count;
    uint64_t slice_instructions;                  /* 0 = 200000 */
    uint32_t pause_sleep_microseconds;            /* 0 = 200 */
    void (*trace)(void *context, const char *line);   /* optional */
    void *trace_context;
} ps3_spu_run_config;

/* Configures the process-wide port (one per process). Returns 0, or PS3_SPU_RUN_INTERNAL_INVALID. */
uint32_t ps3_spu_run_configure(const ps3_spu_run_config *);
/* sys_spu_thread_group_start's effect AFTER the lv2 domain accepted the start: runs every initialized thread. */
uint32_t ps3_spu_run_start(uint32_t group);
/* sys_spu_thread_group_join: bounded/cancellable wait for every thread, then the lv2 cause and status. */
uint32_t ps3_spu_run_join(uint32_t group, uint32_t *cause, uint32_t *status);
/* sys_spu_thread_get_exit_status. */
uint32_t ps3_spu_run_exit_status(uint32_t thread, uint32_t *status);
/* sys_spu_thread_group_terminate: cooperative stop of every worker, then join. */
uint32_t ps3_spu_run_terminate(uint32_t group);
/* Process teardown: cancel and join every running group (call before guest memory is released). */
void ps3_spu_run_shutdown(void);
/* One line per SPU thread of every runner, through `print` (the composition's result printer). */
void ps3_spu_run_report(void (*print)(const char *line));
/* Round 2 (freeze diagnostics, console run rv39s). STARTUP_SPU_STATE: one line per SPU thread of every runner, about once a minute: state, pc, lr, the
 * channel it is stalled on and for how long, how long it has been inside one slice, event status/mask, decrementer, MFC state, mailbox counts, the job window and
 * pm_wwsjob's state words. ps3_spu_run_watch (about once a second) prints, once each, STARTUP_SPU_TERMINAL (a thread ended), STARTUP_SPU_BLOCKED kind=channel (more
 * than 5 s on one channel) and STARTUP_SPU_BLOCKED kind=in_slice (more than 5 s inside one slice); it returns the number of lines printed. */
void ps3_spu_run_state_report(void (*print)(const char *line), unsigned seconds);
unsigned ps3_spu_run_watch(void (*print)(const char *line), unsigned seconds);

#ifdef __cplusplus
}
#endif
#endif
