/* Standalone instantiation of the cellSpursLFQueueAttachLv2EventQueue contract.
 *
 * The functions live in spurs_lfqueue_attach.h with `static inline` linkage
 * (macro PS3_SPURS_LFQA_FN), so a translation unit that includes the header -- notably
 * native/adapters/startup_execution.cpp -- needs no extra object at link time. That matters
 * because several pinned host verifiers link startup_execution.cpp against their own explicit
 * object list. This file redefines the macro so the SAME code is also emitted as ordinary external
 * symbols, which is what the host test (native/tests/test_spurs_lfqueue_attach.c) and the Switch
 * compile check link against. There is exactly one implementation and it cannot diverge between the
 * header and the object.
 *
 * Behaviour taken from the firmware library libsre
 * (SHA256 9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6):
 *   cellSpursLFQueueAttachLv2EventQueue NID 0x1656D49F
 *
 * G15 adds the process-SDK branch table and the `0x17778` arm,
 * instantiated through the same macro so it is emitted once here as well:
 *   sdk > 0x18FFFF  -> ps3_spurs_lfqa_plan_build_sdk25()   (inFAMOUS's 0x00250001)
 *
 * Platform-free: no OS calls, no allocation, no globals, no guest-memory access, no libc.
 */
#define PS3_SPURS_LFQA_FN
#include "spurs_lfqueue_attach.h"
#include "spurs_lfqueue_attach_sdk25.h"
