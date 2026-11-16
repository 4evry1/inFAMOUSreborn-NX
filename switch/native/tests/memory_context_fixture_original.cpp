#include "ppu_recomp.h"
extern "C" uint8_t* vm_base;
void func_F0000000(ppu_context* ctx) {
    ctx->gpr[3] = vm_read32(ctx->gpr[4]);
}
void func_F0000001(ppu_context* ctx) {
    vm_write32(ctx->gpr[4], (uint32_t)ctx->gpr[3]);
}
void func_F0000002(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[5] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[5]; }
}
void func_F0000003(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; int _sc = (ctx->reserve_addr == (uint32_t)ea) && ppu_stwcx32(ea, (uint32_t)ctx->reserve_value, (uint32_t)ctx->gpr[5]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
}
void func_F0000004(ppu_context* ctx) {
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; memcpy(&ctx->vr[2], vm_base + (uint32_t)ea, 16); }
}
void func_F0000005(ppu_context* ctx) {
    { uint64_t ea = (ctx->gpr[4]) & ~0xFULL; memcpy(vm_base + (uint32_t)ea, &ctx->vr[2], 16); }
}
void func_F0000006(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; uint32_t raw; memcpy(&raw, vm_base + (uint32_t)ea, 4); ctx->gpr[7] = raw; }
}
void func_F0000007(ppu_context* ctx) {
    ppu_stvx16((uint32_t)ctx->gpr[4], &ctx->vr[2]);
}
void func_F0000008(ppu_context* ctx) {
    ppu_dcbz128((uint32_t)ctx->gpr[4]);
}
void func_F0000009(ppu_context* ctx) {
    ctx->gpr[8] = ppu_atomic_add16((uint32_t)ctx->gpr[4], (int32_t)1);
}
void func_F000000A(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; ctx->gpr[6] = vm_read64(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[6]; }
}
void func_F000000B(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; int _sc = (ctx->reserve_addr == (uint32_t)ea) && ppu_stdcx64(ea, ctx->reserve_value, ctx->gpr[6]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (_sc ? (2u << 28) : 0u); ctx->reserve_addr = 0; }
}
void func_F000000C(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; ea &= ~3ULL; memset(&ctx->vr[2], 0, 16); memcpy((uint8_t*)&ctx->vr[2] + (ea & 15), vm_base + (uint32_t)ea, 4); }
}
void func_F000000D(ppu_context* ctx) {
    { uint64_t ea = ctx->gpr[4]; uint32_t sh = (uint32_t)(ea & 0xF); uint8_t* m = vm_base + (uint32_t)(ea & ~0xFULL); uint8_t* d = (uint8_t*)&ctx->vr[2]; for (int i = 0; i < 16; i++) d[i] = (sh + (uint32_t)i < 16) ? m[sh + i] : 0; }
}
