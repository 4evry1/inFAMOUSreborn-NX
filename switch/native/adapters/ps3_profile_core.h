#ifndef PS3_NATIVE_PROFILE_CORE_H
#define PS3_NATIVE_PROFILE_CORE_H
/* Sampling profiler of the console build, the part that runs anywhere (2026-11-13, coordinator request after run rv52s: the frame limiter is the guest main
 * thread, 100 % busy, 62 % in generated code: WHICH game functions?). The Horizon side (ps3_profile_switch.c) pauses a thread, reads its pc, lr and the frame chain
 * of its host stack, resumes it, and hands the sample to this file; this file only counts, in fixed tables (no allocation while samples arrive), and writes the
 * dump. tools/ps3_profile_report.py turns a dump and the NRO's ELF into function names.
 *
 * Tables, one writer (the sampler thread):
 *   main thread  pc      the host pc of every sample (4-byte exact)
 *                lr      the link register (the caller of a leaf)
 *                stack   pc plus up to PS3_PROFILE_FRAMES return addresses of the host frame chain (the lifted game functions are C functions, so this IS the guest call stack)
 *   other threads (slot = order of creation) pc per thread slot
 * A table whose probe sequence is exhausted drops the sample (counted in `dropped`). */
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { PS3_PROFILE_FRAMES = 6, PS3_PROFILE_MAX_THREADS = 160 };

typedef struct {
    uint64_t pc, lr;
    uint64_t ret[PS3_PROFILE_FRAMES];     /* return addresses of the frame chain, innermost first; ret[frames..] unused */
    unsigned frames;
} ps3_profile_sample;

typedef struct {
    uint64_t handle, entry;               /* the thread's kernel handle (diagnostic) and the address of its entry function (names it in the report) */
    uint64_t samples;                     /* samples taken of it in this interval (the main thread: see main_samples) */
    unsigned slot, is_main;
} ps3_profile_thread_info;

/* Forget every count (a new interval). */
void ps3_profile_core_reset(void);
/* A sample of the main thread. */
void ps3_profile_core_add_main(const ps3_profile_sample *sample);
/* A sample of another thread (slot < PS3_PROFILE_MAX_THREADS). */
void ps3_profile_core_add_other(unsigned slot, uint64_t pc);
/* Totals of the interval. */
void ps3_profile_core_totals(uint64_t *main_samples, uint64_t *other_samples, uint64_t *with_chain, uint64_t *dropped);
/* Writes the text dump of the interval and returns the number of lines, or -1 when `f` is NULL.
 *   PS3PROFILE 1 seconds=<uptime> interval=<s> hz=<main rate> main_samples=.. other_samples=.. with_chain=.. dropped=..
 *   REF <name> 0x<run-time address>            (known functions: the report derives the load base from them)
 *   THREAD <slot> main=<0|1> handle=0x.. entry=0x.. samples=..
 *   PC <count> 0x<pc>                          main thread, most frequent first, at most `limit` per table
 *   LR <count> 0x<lr>
 *   STACK <count> 0x<pc> 0x<ret1> .. 0x<ret6>  (0 = no frame)
 *   OPC <slot> <count> 0x<pc>                  other threads
 *   END */
int ps3_profile_core_write(FILE *f, unsigned seconds, unsigned interval_seconds, unsigned hz, const char *const *ref_names, const uint64_t *ref_addresses, unsigned ref_count,
                           const ps3_profile_thread_info *threads, unsigned thread_count, unsigned limit);

#ifdef __cplusplus
}
#endif
#endif
