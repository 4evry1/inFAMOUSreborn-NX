#ifndef PS3_RUNTIME_PPU_NATIVE_TIMEBASE_ABI_H
#define PS3_RUNTIME_PPU_NATIVE_TIMEBASE_ABI_H
#include <stdint.h>
struct ppu_context;
#ifdef __cplusplus
extern "C" {
#endif
/* Requires the admitted full native startup owner and its active worker.
 * Failure transfers to the owner's nonreturning stop path; no timestamp is
 * returned and the generated destination assignment must not execute. */
uint64_t ppu_native_timebase_now(struct ppu_context *cpu);
#ifdef __cplusplus
}
#endif
#endif
