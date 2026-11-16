#include "ppu_recomp.h"

// vm_write32(ignored, ignored); braces } { stay comments
static inline int preamble(int x) { return x + 1; }
void func_F0000000(ppu_context* ctx) {
    ctx->gpr[3] = ppu_native_read32(ctx, ctx->gpr[4] + 4);
    ppu_native_write64(ctx, ctx->gpr[4], ctx->gpr[3]);
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[5] = ppu_native_load_reserve32(ctx, ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[5]; }
    { uint64_t ea = ctx->gpr[4]; int _sc = ppu_native_store_conditional32(ctx, ea, (uint32_t)ctx->gpr[5]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[6] = ppu_native_load_reserve64(ctx, ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[6]; }
    { uint64_t ea = ctx->gpr[4]; int _sc = ppu_native_store_conditional64(ctx, ea, ctx->gpr[6]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; ppu_native_read_bytes(ctx, (uint32_t)ea, &ctx->vr[2], 16); }
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; ppu_native_write_bytes(ctx, (uint32_t)ea, &ctx->vr[2], 16); }
    { uint64_t ea = ctx->gpr[4]; uint32_t raw; ppu_native_read_bytes(ctx, (uint32_t)ea, &raw, 4); ctx->gpr[7] = raw; }
    ppu_native_write_bytes(ctx, (uint32_t)ctx->gpr[4], &ctx->vr[2], 16);
    ppu_native_zero128(ctx, (uint32_t)ctx->gpr[4]);
    ctx->gpr[8] = ppu_native_atomic_add16(ctx, (uint32_t)ctx->gpr[4], (int32_t)1);
}
