#include "ppu_recomp.h"
extern "C" uint8_t* vm_base;
// vm_write32(ignored, ignored); braces } { stay comments
static inline int preamble(int x) { return x + 1; }
void func_F0000000(ppu_context* ctx) {
    ctx->gpr[3] = vm_read32(ctx->gpr[4] + 4);
    vm_write64(ctx->gpr[4], ctx->gpr[3]);
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[5] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[5]; }
    { uint64_t ea = ctx->gpr[4]; int _sc = (ctx->reserve_addr == (uint32_t)ea) && ppu_stwcx32(ea, (uint32_t)ctx->reserve_value, (uint32_t)ctx->gpr[5]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[6] = vm_read64(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[6]; }
    { uint64_t ea = ctx->gpr[4]; int _sc = (ctx->reserve_addr == (uint32_t)ea) && ppu_stdcx64(ea, ctx->reserve_value, ctx->gpr[6]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; memcpy(&ctx->vr[2], vm_base + (uint32_t)ea, 16); }
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; memcpy(vm_base + (uint32_t)ea, &ctx->vr[2], 16); }
    { uint64_t ea = ctx->gpr[4]; uint32_t raw; memcpy(&raw, vm_base + (uint32_t)ea, 4); ctx->gpr[7] = raw; }
    ppu_stvx16((uint32_t)ctx->gpr[4], &ctx->vr[2]);
    ppu_dcbz128((uint32_t)ctx->gpr[4]);
    ctx->gpr[8] = ppu_atomic_add16((uint32_t)ctx->gpr[4], (int32_t)1);
}
