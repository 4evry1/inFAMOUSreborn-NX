/* Standalone instantiation of the cellSpursCreateTasksetWithAttribute builder.
 *
 * The builders live in spurs_taskset_create.h with `static inline` linkage (macro
 * PS3_SPURS_TSC), so a translation unit that includes the header -- notably
 * native/adapters/startup_execution.cpp -- needs no extra object at link time. That matters
 * because the pinned host verifiers link startup_execution.cpp against their own explicit
 * object list, which this slice must not have to change. This file redefines the macro so the
 * SAME code is also emitted as ordinary external symbols, which is what the host test
 * (native/tests/test_spurs_taskset_init.c) and the Switch compile check link against. There is
 * exactly one implementation and it cannot diverge between the header and the object.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6):
 *   cellSpursCreateTasksetWithAttribute NID 0xC10931CB
 *
 * Platform-free: no OS calls, no allocation, no globals, no guest-memory access.
 */
#define PS3_SPURS_TSC
#include "spurs_taskset_create.h"
