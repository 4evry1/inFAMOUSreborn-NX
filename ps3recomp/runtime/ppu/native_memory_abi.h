#ifndef PS3_PPU_NATIVE_MEMORY_ABI_H
#define PS3_PPU_NATIVE_MEMORY_ABI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ppu_context;
enum { PPU_NATIVE_BULK_MAX_BYTES = 16 };
/* Native derived-code ABI. Every ctx is the CPU-first subobject of a live,
 * standard-layout execution owner admitted by composition. Helpers obtain the
 * same explicit domain used by that owner's HLE services; no TLS/global lookup.
 * EA is normalized to low32; nonempty ranges crossing4GiB fail before touching
 * memory. Scalar values are big-endian; byte transfers preserve raw byte order.
 * Read destinations/register values publish only after complete successful
 * reads. A provider error stops execution only after provider guards unwind;
 * helpers never manufacture zero reads, successful stores or guest CELL codes.
 * Byte transfers support at most16 bytes. Larger lengths are unsupported.
 * Zero-length read_bytes/write_bytes are no-ops: no provider call, pointer
 * dereference or destination publication, including when the buffer is NULL.
 * Host buffers must not overlap domain/owner records or shared guest backing.
 * LR reads+arms atomically. SC consumes every attempt, even mismatch/failure,
 * and uses the explicit domain-owner record, never legacy CPU reserve mirrors.
 * add16 returns the new wrapped BE16 value zero-extended to uint64_t.
 * zero128 aligns the low32 EA down to128 bytes. Composition owns all lifetimes
 * until workers join and reservation owners/pins are explicitly reclaimed. */
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
#ifdef __cplusplus
}
#endif
#endif
