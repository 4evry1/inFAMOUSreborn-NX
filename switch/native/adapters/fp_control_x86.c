#include "spu_fp_control.h"
#include <xmmintrin.h>

enum { MXCSR_ROUNDING_AND_FLUSH = 0xE040u };

spu_fp_saved_state spu_fp_begin_double(void)
{
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(saved & ~MXCSR_ROUNDING_AND_FLUSH);
    return saved;
}

void spu_fp_end_double(spu_fp_saved_state saved)
{
    _mm_setcsr((unsigned)saved);
}
