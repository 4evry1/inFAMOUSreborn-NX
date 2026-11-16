/* See native_inflate.h. Round 2 (2026-11-13). */
#include "native_inflate.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

volatile int g_ps3_native_inflate_enabled = 1;
static ps3_native_inflate_stats g_stats;

enum { LS_BYTES = 0x40000 };

static inline uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define COUNT(field, n) __atomic_fetch_add(&g_stats.field, (uint64_t)(n), __ATOMIC_RELAXED)

void ps3_native_inflate_get_stats(ps3_native_inflate_stats *out) {
    if (!out) return;
    out->calls = __atomic_load_n(&g_stats.calls, __ATOMIC_RELAXED);
    out->bytes_in = __atomic_load_n(&g_stats.bytes_in, __ATOMIC_RELAXED);
    out->bytes_out = __atomic_load_n(&g_stats.bytes_out, __ATOMIC_RELAXED);
    out->ns = __atomic_load_n(&g_stats.ns, __ATOMIC_RELAXED);
    out->fallback_args = __atomic_load_n(&g_stats.fallback_args, __ATOMIC_RELAXED);
    out->fallback_overlap = __atomic_load_n(&g_stats.fallback_overlap, __ATOMIC_RELAXED);
    out->fallback_zlib = __atomic_load_n(&g_stats.fallback_zlib, __ATOMIC_RELAXED);
    out->fallback_size = __atomic_load_n(&g_stats.fallback_size, __ATOMIC_RELAXED);
    out->fallback_disabled = __atomic_load_n(&g_stats.fallback_disabled, __ATOMIC_RELAXED);
}
void ps3_native_inflate_reset_stats(void) { memset((void *)&g_stats, 0, sizeof g_stats); }

/* The output of one call is held here until it is known to be a success (an error leaves the local store untouched). One 256 KiB buffer per worker thread,
 * allocated on first use: a call can never have an output larger than the local store. */
static __thread uint8_t *tl_scratch;

int ps3_native_inflate_edge_raw(spu_context *ctx) {
    if (!g_ps3_native_inflate_enabled) { COUNT(fallback_disabled, 1); return 0; }
    const uint32_t out_ptr = ctx->gpr[3]._u32[0], out_size = ctx->gpr[4]._u32[0], in_ptr = ctx->gpr[5]._u32[0], in_size = ctx->gpr[6]._u32[0];
    /* the lifted function asserts out <= 0x3FFFF and in <= 0x3FFFF; zlib's z_stream sizes are 32-bit counts; a zero-length buffer is a corner of
     * inflate's return codes (Z_BUF_ERROR) that the lifted function should answer */
    if (out_ptr > 0x3FFFFu || in_ptr > 0x3FFFFu || !out_size || !in_size || (uint64_t)out_ptr + out_size > LS_BYTES || (uint64_t)in_ptr + in_size > LS_BYTES) {
        COUNT(fallback_args, 1); return 0;
    }
    if ((uint64_t)out_ptr < (uint64_t)in_ptr + in_size && (uint64_t)in_ptr < (uint64_t)out_ptr + out_size) { COUNT(fallback_overlap, 1); return 0; }
    if (!tl_scratch) {
        tl_scratch = malloc(LS_BYTES);
        if (!tl_scratch) { COUNT(fallback_args, 1); return 0; }
    }
    const uint64_t t0 = now_ns();
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    if (inflateInit2(&strm, -15) != Z_OK) { COUNT(fallback_zlib, 1); return 0; }
    strm.next_in = (Bytef *)(ctx->ls + in_ptr); strm.avail_in = in_size;
    strm.next_out = tl_scratch; strm.avail_out = out_size;
    const int ret = inflate(&strm, Z_FINISH);
    const uLong total_out = strm.total_out;
    const int end = inflateEnd(&strm);
    if (ret != Z_STREAM_END || end != Z_OK) { COUNT(fallback_zlib, 1); COUNT(ns, now_ns() - t0); return 0; }
    if (total_out != out_size) { COUNT(fallback_size, 1); COUNT(ns, now_ns() - t0); return 0; }
    memcpy(ctx->ls + out_ptr, tl_scratch, out_size);
    ctx->gpr[3] = (u128){ 0 };      /* status 0: the lifted function returns inflateEnd's result in r3 (the test compares the whole quadword) */
    COUNT(calls, 1); COUNT(bytes_in, in_size); COUNT(bytes_out, out_size); COUNT(ns, now_ns() - t0);
    return 1;
}
