#ifndef PS3_BINK_LIFTED_H
#define PS3_BINK_LIFTED_H
#include "../spu_lifted_hybrid.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The inFAMOUS Bink movie decoder (the single SPU thread of the third lv2 SPU thread group, entry 0xF0, EBOOT SPU ELF
 * at file offset 0x673E80), lifted by the Windows port as spu_0001_at_00673E80 and adapted by gen_bink_lifted.py.
 * Build bink_module.c, generated/bink_lifted_rc.c and generated/bink_lifted_rows.c into the image (see the host
 * scripts for the flags). ps3_spu_lifted_find() matches it by the content hash of the loaded code segment. */
extern const ps3_spu_lifted_module ps3_bink_lifted_module;
#ifdef __cplusplus
}
#endif
#endif
