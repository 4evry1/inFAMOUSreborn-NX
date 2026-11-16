#ifndef PS3_NATIVE_SPU_IMAGE_ACCOUNT_H
#define PS3_NATIVE_SPU_IMAGE_ACCOUNT_H
/* Per-SPU-image accounting (Decision 104, 2026-11-13). Which program does each SPU thread spend its time in?
 *
 * "Image" here is what the thread executes, named by CONTENT, not by address or thread number:
 *   image   the thread's loaded image (the SPURS kernel of libsre for the five job threads and the sound thread; the Bink decoder),
 *           key = hash of the local store the lv2 loader produced (ps3_spu_lifted_hash64w over [0, last non-zero byte)), per SPURS group;
 *   policy  the policy module streamed in at LS 0xA00 (WWS pm_wwsjob): key = hash of its first 128 bytes, per SPURS group;
 *   job     the job/task module streamed into the job window at LS 0x4000: key = hash of the 48 bytes there (SpuGUID + JobCheckData);
 *   other   anything else (the interrupt stub at the top of the local store, ...).
 * The interpreter attributes instructions at region changes only (taken transfers; spu_interp_native.c), the worker converts them
 * to an estimated time per slice (slice wall time minus the time lifted code ran, split by instruction share), and lifted code is
 * timed per entry. Nothing here runs per instruction; the cost is a few adds per region change and one pass over the slots per
 * slice. All global counters are updated with relaxed atomics by the worker threads; the report reads them without stopping anyone. */
#include "spu_executor.h"
#include "../spu_lifted/spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { PS3_SPU_ACCT_IMAGE = 1, PS3_SPU_ACCT_POLICY = 2, PS3_SPU_ACCT_JOB = 3, PS3_SPU_ACCT_OTHER = 4, PS3_SPU_ACCT_STATIC = 5 };

/* Hash of a freshly loaded local store: [0, end of the last non-zero 16-byte block). *size gets that end. */
uint64_t ps3_spu_acct_image_key(const uint8_t *ls, uint32_t *size);
/* Finds or creates the slot for (kind, key). `name` is copied on creation (may be NULL); entry/size are informative.
 * Returns the slot id (>= 1) or 0 when the table is full (the caller then attributes to slot 0 = "unattributed"). */
unsigned ps3_spu_acct_slot(unsigned kind, uint64_t key, uint32_t entry, uint32_t size, const char *name);
/* A lifted module recognised for a slot (its name replaces the generic one in the report). */
void ps3_spu_acct_name(unsigned slot, const char *name);
/* Arms `o` for region accounting: standard local-store map, the four region slots. Slot ids from ps3_spu_acct_slot. */
void ps3_spu_acct_arm(ps3_spu_executor *o, unsigned kernel_slot, unsigned policy_slot, unsigned job_slot, unsigned other_slot);
/* The job-call hook of ps3_spu_dyn: picks the job's and the policy module's slot from the local store now in place
 * (context = the group id as uintptr_t, so two SPURS instances keep apart). `matched` is the lifted module or NULL. */
void ps3_spu_acct_job_call(void *context, ps3_spu_executor *o, const ps3_spu_lifted_module *matched);
/* After a slice: moves o->acct_instr[] into the global table. slice_ns = wall time of the slice, lifted_ns = the part of it spent in
 * lifted code; lifted_ns_by_slot / enters_by_slot (may be NULL) are the lifted time and entries per slot, zeroed here. */
void ps3_spu_acct_flush(ps3_spu_executor *o, uint64_t slice_ns, uint64_t lifted_ns, uint64_t *lifted_ns_by_slot, uint64_t *enters_by_slot);
/* Round 2 (PS3_SPU_ESCAPE_DIAG): folds the escape tables of `dyn` (escapes by reason and the heaviest target pcs per job slot) into
 * the global slots and zeroes them. Cheap when nothing escaped since the last call. */
void ps3_spu_acct_flush_escapes(ps3_spu_dyn *dyn);
/* Whole-slice accounting for a thread without a region map (a static lifted module such as the Bink decoder). */
void ps3_spu_acct_flat(unsigned slot, uint64_t instructions, uint64_t interp_ns, uint64_t lifted_ns, uint64_t enters);
/* Decision 134: the on_job_end hook of ps3_spu_dyn: one job of `slot` ran `ns` (returned = 1) or ended unseen (returned = 0, no time). */
void ps3_spu_acct_job_ran(unsigned slot, uint64_t ns, int returned);
/* Prints "STARTUP_SPU_IMAGES ..." lines: one header and the top 12 slots by total time. */
void ps3_spu_images_report(void (*print)(const char *line), unsigned seconds);
#ifdef __cplusplus
}
#endif
#endif
