/* Desktop-interpreter oracle for native/tests/test_spu_interp.c (Decision 33 / C1a).
 *
 * spu_interp.c is the desktop runtime's interpreter, so it references a handful of desktop-only externals the host
 * test does not otherwise build: the guest VM base pointer, the float-policy switch, the lifted-function registry it
 * rejoins, the profile/A-B switches, and the taskset/job-API trace hooks. This unit supplies exactly those, and
 * nothing else.
 *
 * Every one of them is inert here: the guest VM base is never dereferenced (the oracle programs stop before any DMA),
 * the float policy is off and frozen at build time (SPU_XFLOAT=0 semantics, which is what the native side has), the
 * registry is empty (so every pc is interpreted), and the probe switches stay 0 (so the trace hooks are never
 * called). spu_interp.c defines g_spu_xfloat / g_spu_xflim itself.
 */
/* Poisons g_spu_xfloat/g_spu_xflim. */
#include "../../native/adapters/spu_linked_image.h"
#include "../../../ps3recomp/runtime/spu/spu_interp.h"
#include "../../../ps3recomp/runtime/spu/spu_laneprof.h"

uint8_t *vm_base = NULL;

spu_lifted_fn spu_lifted_lookup(const spu_context *ctx, uint32_t lsa) { (void)ctx; (void)lsa; return NULL; }
uint8_t g_spu_fn_bitmap[0x40000 / 32];
volatile uint32_t g_spu_fn_reg_gen = 1;
volatile int g_ab_mask = 0;
volatile int g_abx = 0;

SPU_THREAD_LOCAL spu_laneprof_tls g_spu_lp;

typedef void (*spu_plug_fn_t)(spu_context *);
spu_plug_fn_t spu_plug_lookup(spu_context *ctx, uint32_t pc) { (void)ctx; (void)pc; return NULL; }

void spu_spurs_taskset_syscall(spu_context *ctx) { (void)ctx; }
void spu_wws_jobapi_trace(spu_context *ctx) { (void)ctx; }
