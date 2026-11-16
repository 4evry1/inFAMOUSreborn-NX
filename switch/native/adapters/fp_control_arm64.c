#include "spu_fp_control.h"

enum {
    FPCR_ROUNDING_MODE = 3u << 22,
    FPCR_FLUSH_TO_ZERO = 1u << 24,
};

spu_fp_saved_state spu_fp_begin_double(void)
{
    uint64_t control, status;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control) :: "memory");
    __asm__ volatile("mrs %0, fpsr" : "=r"(status) :: "memory");
    const uint64_t nearest_with_subnormals =
        control & ~(uint64_t)(FPCR_ROUNDING_MODE | FPCR_FLUSH_TO_ZERO);
    __asm__ volatile("msr fpcr, %0" :: "r"(nearest_with_subnormals) : "memory");
    return (status << 32) | control;
}

void spu_fp_end_double(spu_fp_saved_state saved)
{
    const uint64_t control = (uint32_t)saved;
    const uint64_t status = saved >> 32;
    __asm__ volatile("msr fpsr, %0" :: "r"(status) : "memory");
    __asm__ volatile("msr fpcr, %0" :: "r"(control) : "memory");
}
