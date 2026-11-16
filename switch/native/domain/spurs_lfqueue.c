/* Standalone instantiation of the _cellSpursLFQueueInitialize builders.
 *
 * The builders live in spurs_lfqueue.h with `static inline` linkage (macro PS3_SPURS_LFQ_FN), so a
 * translation unit that includes the header -- notably native/adapters/startup_execution.cpp --
 * needs no extra object at link time. That matters because several pinned host verifiers link
 * startup_execution.cpp against their own explicit object list. This file redefines the macro so
 * the SAME code is also emitted as ordinary external symbols, which is what the host test
 * (native/tests/test_spurs_taskset_init.c) and the Switch compile check link against. There is
 * exactly one implementation and it cannot diverge between the header and the object.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6):
 *   _cellSpursLFQueueInitialize NID 0x011EE38B
 *
 * Platform-free: no OS calls, no allocation, no globals, no guest-memory access.
 */
#define PS3_SPURS_LFQ_FN
#include "spurs_lfqueue.h"
