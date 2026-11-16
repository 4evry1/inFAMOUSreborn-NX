#ifndef PS3_NATIVE_SPURS_KERNEL_LIFTED_H
#define PS3_NATIVE_SPURS_KERNEL_LIFTED_H
/* The SPURS kernel of the user's own libsre.prx as a lifted module (Decision 133). Its translation units are generated at build time from the libsre image of the
 * build's own lift (gen_kernel_lifted.py) into <build>/spu_kernel and exist only there; -DPS3_SPU_LIFTED_KERNEL says they are linked. */
#include "../spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
extern const ps3_spu_lifted_module ps3_spurs_kernel_module;
#ifdef __cplusplus
}
#endif
#endif
