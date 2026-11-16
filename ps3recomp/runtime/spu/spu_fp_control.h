#ifndef PS3_SPU_FP_CONTROL_H
#define PS3_SPU_FP_CONTROL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-thread opaque state; scopes must be restored in reverse entry order.
 * Double operations use nearest rounding and preserve subnormal operands and
 * results. Restoration includes the caller's exception status. */
typedef uint64_t spu_fp_saved_state;
spu_fp_saved_state spu_fp_begin_double(void);
void spu_fp_end_double(spu_fp_saved_state saved);

#ifdef __cplusplus
}
#endif
#endif
