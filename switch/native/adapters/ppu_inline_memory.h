#ifndef PS3_PPU_NATIVE_MEMORY_ABI_H
#define PS3_PPU_NATIVE_MEMORY_ABI_H
/* ppu-speed-20261113 item 2: faster loads for the generated game code.
 *
 * This header REPLACES artifacts/ppu-timebase-context-stage26-r6/native_memory_abi.h for one build of the five lifted game units:
 * evidence/ppu-speed-20261113/build_ppu_inline.sh force-includes it (-include) before the unit's own #include "ppu_recomp.h", and
 * it claims the original's include guard, so the original is skipped. Every declaration of the original follows unchanged; what
 * is added is, for the load helpers only, a faster path behind the same names (function-like macros after the declarations).
 * Stores, lwarx/stwcx (ppu_native_load_reserve*, ppu_native_store_conditional*), add16 and zero128 are NOT touched: they stay
 * out-of-line calls with their reservation, sticky-line and conflict logic.
 *
 *   -DPPU_INLINE_LOADS=0   the plain ABI (the control build: identical code to the old objects)
 *   -DPPU_INLINE_LOADS=1   (default) the fast path INLINE at every load site, about 25 AArch64 instructions on the hit path, one
 *                          call to ppu_native_readNN_slow on a miss. Fastest per load; the generated code grows by about 100 bytes
 *                          per load site.
 *   -DPPU_INLINE_LOADS=2   the same fast path as ONE small leaf function per width (ppu_native_readNN_fast, defined in
 *                          native/adapters/ppu_memory_context.cpp): the call site stays a plain call (no code growth), the callee
 *                          drops the 176-byte frame, the thread-local counter (a call on the console's soft-TLS ABI) and the
 *                          binding checks of the out-of-line path.
 *
 * What the fast path does, in the order of native/adapters/ppu_memory_context.cpp's lean path (Decisions 110/111):
 *   1. g_ppu_inline_window is null until the composition has switched the lean path on after the first frame
 *      (ppu_inline_memory_set(true)); null -> the original helper, which does all its checks.
 *   2. a per-context counter (PpuMemoryExecution::inline_tick); every 256th access of a context takes the slow route, which first
 *      runs the full path's binding, cancel and budget checks, so a thread that makes no calls and no stores still sees a stop.
 *      (The old lean path counted per thread in thread-local storage: a function call per access on the soft-TLS ABI.)
 *   3. an access that could cross a page, or touch a page that is not committed in the flat window's commit bitmap, takes the slow
 *      route (the original helper: it handles the cross-page and unmapped cases exactly as before).
 *   4. otherwise one volatile load from window base + address and a byte swap. volatile because the call it replaces was opaque:
 *      the host compiler may neither merge two loads of one guest word nor hoist a polling load out of a guest spin loop.
 * Byte transfers (lvx etc.) of the constants 16 and 4 follow the same rules and keep the guest byte order.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ppu_inline_layout.h"
#ifdef __cplusplus
extern "C" {
#endif
struct ppu_context;
enum { PPU_NATIVE_BULK_MAX_BYTES = 16 };
uint8_t ppu_native_read8(struct ppu_context *ctx, uint64_t ea);
uint16_t ppu_native_read16(struct ppu_context *ctx, uint64_t ea);
uint32_t ppu_native_read32(struct ppu_context *ctx, uint64_t ea);
uint64_t ppu_native_read64(struct ppu_context *ctx, uint64_t ea);
void ppu_native_write8(struct ppu_context *ctx, uint64_t ea, uint8_t value);
void ppu_native_write16(struct ppu_context *ctx, uint64_t ea, uint16_t value);
void ppu_native_write32(struct ppu_context *ctx, uint64_t ea, uint32_t value);
void ppu_native_write64(struct ppu_context *ctx, uint64_t ea, uint64_t value);
void ppu_native_read_bytes(struct ppu_context *ctx, uint64_t ea, void *destination, size_t length);
void ppu_native_write_bytes(struct ppu_context *ctx, uint64_t ea, const void *source, size_t length);
uint32_t ppu_native_load_reserve32(struct ppu_context *ctx, uint64_t ea);
uint64_t ppu_native_load_reserve64(struct ppu_context *ctx, uint64_t ea);
bool ppu_native_store_conditional32(struct ppu_context *ctx, uint64_t ea, uint32_t value);
bool ppu_native_store_conditional64(struct ppu_context *ctx, uint64_t ea, uint64_t value);
uint64_t ppu_native_atomic_add16(struct ppu_context *ctx, uint64_t ea, int32_t increment);
void ppu_native_zero128(struct ppu_context *ctx, uint64_t ea);
/* ppu-speed-20261113 additions, defined in native/adapters/ppu_memory_context.cpp. ppu_inline_window is in ppu_inline_layout.h.
 * g_ppu_inline_window: null = off; published once with a release store (ppu_inline_memory_set), readers load the pointer and then the
 * two fields through it, so the address dependency orders them on the console's weakly ordered cores. */
extern const ppu_inline_window *g_ppu_inline_window;
/* the slow route of PPU_INLINE_LOADS=1: the full path's checks when this context's counter is due, then the original helper */
uint8_t ppu_native_read8_slow(struct ppu_context *ctx, uint64_t ea);
uint16_t ppu_native_read16_slow(struct ppu_context *ctx, uint64_t ea);
uint32_t ppu_native_read32_slow(struct ppu_context *ctx, uint64_t ea);
uint64_t ppu_native_read64_slow(struct ppu_context *ctx, uint64_t ea);
void ppu_native_read_bytes_slow(struct ppu_context *ctx, uint64_t ea, void *destination, size_t length);
/* PPU_INLINE_LOADS=2: the fast path as leaf functions */
uint8_t ppu_native_read8_fast(struct ppu_context *ctx, uint64_t ea);
uint16_t ppu_native_read16_fast(struct ppu_context *ctx, uint64_t ea);
uint32_t ppu_native_read32_fast(struct ppu_context *ctx, uint64_t ea);
uint64_t ppu_native_read64_fast(struct ppu_context *ctx, uint64_t ea);
void ppu_native_read_bytes16_fast(struct ppu_context *ctx, uint64_t ea, void *destination);
void ppu_native_read_bytes4_fast(struct ppu_context *ctx, uint64_t ea, void *destination);
#ifdef __cplusplus
}
#endif

#ifndef PPU_INLINE_LOADS
#define PPU_INLINE_LOADS 1
#endif
/* little-endian hosts only (byte swap); PPU_INLINE_LOADS_ANY_ARCH lets the x86 laptop compile it for the micro-benchmark */
#if PPU_INLINE_LOADS && defined(__cplusplus) && (defined(__aarch64__) || defined(PPU_INLINE_LOADS_ANY_ARCH))
#if PPU_INLINE_LOADS == 1
typedef uint16_t __attribute__((may_alias, aligned(1))) ppu_inline_u16;
typedef uint32_t __attribute__((may_alias, aligned(1))) ppu_inline_u32;
typedef uint64_t __attribute__((may_alias, aligned(1))) ppu_inline_u64;
/* true and *host set when the inline path may serve [ea, ea + size), size <= 16; false: take the slow route (which also runs the
 * counter-due checks, because this function has already counted the access). */
static inline __attribute__((always_inline)) bool ppu_inline_host(struct ppu_context *ctx, uint64_t ea, uint32_t size, uintptr_t *host)
{
    const uint32_t address = (uint32_t)ea;
    const ppu_inline_window *const window = g_ppu_inline_window;
    uint32_t *const tick = (uint32_t *)(void *)((unsigned char *)ctx + PPU_INLINE_TICK_OFFSET);
    const uint32_t count = *tick + 1u;
    *tick = count;
    if (__builtin_expect(window == 0, 0) || __builtin_expect((count & 255u) == 0u, 0)) return false;
    if (size > 1u && __builtin_expect((address & 4095u) > 4096u - size, 0)) return false;   /* may cross a page */
    const uint32_t page = address >> 12;
    if (__builtin_expect(((window->bitmap[page >> 3] >> (page & 7u)) & 1u) == 0u, 0)) return false;
    *host = window->base + address;
    return true;
}
#define PPU_INLINE_READ(NAME, TYPE, ALIASED, SWAP, SIZE) \
static inline __attribute__((always_inline)) TYPE ppu_inline_##NAME(struct ppu_context *ctx, uint64_t ea) \
{ \
    uintptr_t host; \
    if (__builtin_expect(ppu_inline_host(ctx, ea, SIZE, &host), 1)) return SWAP(*(const volatile ALIASED *)host); \
    return ppu_native_##NAME##_slow(ctx, ea); \
}
PPU_INLINE_READ(read8, uint8_t, unsigned char, (uint8_t), 1u)
PPU_INLINE_READ(read16, uint16_t, ppu_inline_u16, __builtin_bswap16, 2u)
PPU_INLINE_READ(read32, uint32_t, ppu_inline_u32, __builtin_bswap32, 4u)
PPU_INLINE_READ(read64, uint64_t, ppu_inline_u64, __builtin_bswap64, 8u)
/* Byte transfers of 16 (lvx, ...) and 4 bytes: raw guest order, destination written only after the whole read. Other lengths: original. */
static inline __attribute__((always_inline)) void ppu_inline_read_bytes(struct ppu_context *ctx, uint64_t ea, void *destination, size_t length)
{
    if (__builtin_constant_p(length) && (length == 16u || length == 4u)) {
        uintptr_t host;
        if (__builtin_expect(ppu_inline_host(ctx, ea, (uint32_t)length, &host), 1)) {
            if (length == 16u) {
                const uint64_t a = *(const volatile ppu_inline_u64 *)host, b = *(const volatile ppu_inline_u64 *)(host + 8);
                ((ppu_inline_u64 *)destination)[0] = a; ((ppu_inline_u64 *)destination)[1] = b;
            } else {
                const uint32_t a = *(const volatile ppu_inline_u32 *)host;
                *(ppu_inline_u32 *)destination = a;
            }
            return;
        }
        ppu_native_read_bytes_slow(ctx, ea, destination, length);
        return;
    }
    ppu_native_read_bytes(ctx, ea, destination, length);
}
#define ppu_native_read8(ctx, ea) ppu_inline_read8((ctx), (ea))
#define ppu_native_read16(ctx, ea) ppu_inline_read16((ctx), (ea))
#define ppu_native_read32(ctx, ea) ppu_inline_read32((ctx), (ea))
#define ppu_native_read64(ctx, ea) ppu_inline_read64((ctx), (ea))
#define ppu_native_read_bytes(ctx, ea, destination, length) ppu_inline_read_bytes((ctx), (ea), (destination), (length))
#else   /* PPU_INLINE_LOADS == 2 */
static inline __attribute__((always_inline)) void ppu_fast_read_bytes(struct ppu_context *ctx, uint64_t ea, void *destination, size_t length)
{
    if (__builtin_constant_p(length) && length == 16u) ppu_native_read_bytes16_fast(ctx, ea, destination);
    else if (__builtin_constant_p(length) && length == 4u) ppu_native_read_bytes4_fast(ctx, ea, destination);
    else ppu_native_read_bytes(ctx, ea, destination, length);
}
#define ppu_native_read8(ctx, ea) ppu_native_read8_fast((ctx), (ea))
#define ppu_native_read16(ctx, ea) ppu_native_read16_fast((ctx), (ea))
#define ppu_native_read32(ctx, ea) ppu_native_read32_fast((ctx), (ea))
#define ppu_native_read64(ctx, ea) ppu_native_read64_fast((ctx), (ea))
#define ppu_native_read_bytes(ctx, ea, destination, length) ppu_fast_read_bytes((ctx), (ea), (destination), (length))
#endif
#endif
#endif
