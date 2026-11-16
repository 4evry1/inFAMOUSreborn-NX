#ifndef PS3_NATIVE_WWS_RESTORE_STUB_H
#define PS3_NATIVE_WWS_RESTORE_STUB_H
/* pm_wwsjob's run-time register restore sequence at LS 0x3FEC0 as a self-checking lifted function (Decision 134, wws_restore_stub.c). */
#include "../spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const ps3_spu_lifted_module ps3_wws_restore_stub_module;
/* Word `index` (0..76) of the sequence, big-endian instruction encoding. */
uint32_t ps3_wws_restore_stub_word(unsigned index);
/* 1 when LS [0x3FEC0, 0x3FFF4) holds exactly the sequence. */
int ps3_wws_restore_stub_present(const spu_context *ctx);
void ps3_wws_restore_stub(spu_context *ctx);
#ifdef __cplusplus
}
#endif
#endif
