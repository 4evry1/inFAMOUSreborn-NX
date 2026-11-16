#ifndef PS3_NATIVE_INFLATE_H
#define PS3_NATIVE_INFLATE_H
/* Native replacement for ONE function of the lifted Edge zlib inflate task (jobmod_422c6e0242f5_e3090, LS 0x3000): the stateless
 * raw-deflate inflater `edgeZlibInflateRawData` at entry 0x3750 (round 2, 2026-11-13).
 *
 * What the SPU function does (read from the lifted code, source/infamous-recomp/spu/spu_0000_at_005F6000):
 *     int inflate_raw(uint8_t *out [r3], uint32_t out_size [r4], const uint8_t *in [r5], uint32_t in_size [r6])
 *   asserts out and in lie in the local store (<= 0x3FFFF), runs inflateInit2_(strm, -15, "1.2.3.0-PS3-SPU-EDGE", 56) and inflate(strm, Z_FINISH) on a
 *   z_stream built on its own stack, and returns 0 only when inflate ends the stream (Z_STREAM_END), inflateEnd succeeds and total_out == out_size.
 *   Every other outcome prints through spu_printf (a PPU round trip) and returns an error code.
 *
 * ps3_native_inflate_edge_raw() does the same work with the host's zlib when -- and only when -- the call is a plain success: valid arguments, no
 * overlap, the stream decodes to exactly out_size bytes. It then writes the output into the local store, sets r3 = 0 and returns 1: the caller
 * returns from the function like the lifted epilogue would (SPU_RET). In every other case it changes NOTHING and returns 0, and the lifted code
 * runs, so errors, truncated or oversized streams, printing and return codes are the lifted function's own.
 *
 * What the native path does NOT reproduce (dead by the SPU ABI, checked in the host test): the function's stack frame below the caller's sp, the
 * caller-saved registers r2..r79 other than r3, and the static inflate state in the task's bss (LS 0x33E80..), which inflateInit2_ rewrites at the next
 * call. Callee-saved registers, r0, r1, the output bytes and r3 are exactly the lifted function's.
 *
 * Needs zlib; built only with -DPS3_SPU_NATIVE_INFLATE (lle.mk: LLE_SPU_NATIVE_INFLATE=1). */
#include "spu_context.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int ps3_native_inflate_edge_raw(spu_context *ctx);

/* Counters (relaxed atomics, all threads): calls taken natively, bytes of compressed input and of output, nanoseconds spent, and the fallbacks by reason. */
typedef struct {
    uint64_t calls, bytes_in, bytes_out, ns;
    uint64_t fallback_args, fallback_overlap, fallback_zlib, fallback_size, fallback_disabled;
} ps3_native_inflate_stats;
void ps3_native_inflate_get_stats(ps3_native_inflate_stats *out);
void ps3_native_inflate_reset_stats(void);
/* Runtime switch (default 1): tests turn the native path off to get the lifted function's own result for comparison. */
extern volatile int g_ps3_native_inflate_enabled;
#ifdef __cplusplus
}
#endif
#endif
