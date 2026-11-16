#include "spu_fp_control.h"
#include <stdint.h>
#include <stdio.h>

#if defined(__aarch64__)
static uint64_t read_state(void)
{
    uint64_t control, status;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control) :: "memory");
    __asm__ volatile("mrs %0, fpsr" : "=r"(status) :: "memory");
    return control | (status << 32);
}
static void write_state(uint64_t state)
{
    uint64_t control = (uint32_t)state, status = state >> 32;
    __asm__ volatile("msr fpcr, %0" :: "r"(control) : "memory");
    __asm__ volatile("msr fpsr, %0" :: "r"(status) : "memory");
}
static const uint64_t ROUND_AND_FLUSH = (3ull << 22) | (1ull << 24);
static const uint64_t EXCEPTION_STATUS = 1ull << 32;
#elif defined(__x86_64__)
#include <xmmintrin.h>
static uint64_t read_state(void) { return _mm_getcsr(); }
static void write_state(uint64_t state) { _mm_setcsr((unsigned)state); }
static const uint64_t ROUND_AND_FLUSH = 0xE040;
static const uint64_t EXCEPTION_STATUS = 1;
#else
#error No test adapter for this CPU
#endif

int native_test_fp_state(void)
{
    const uint64_t original = read_state();
    const uint64_t caller = original | ROUND_AND_FLUSH | EXCEPTION_STATUS;
    write_state(caller);
    spu_fp_saved_state outer = spu_fp_begin_double();
    const uint64_t double_state = read_state();
    int failures = (double_state & ROUND_AND_FLUSH) != 0;
    failures += (double_state & EXCEPTION_STATUS) != EXCEPTION_STATUS;
    spu_fp_saved_state inner = spu_fp_begin_double();
    write_state(read_state() & ~EXCEPTION_STATUS);
    spu_fp_end_double(inner);
    failures += read_state() != double_state;
    spu_fp_end_double(outer);
    failures += read_state() != caller;
    write_state(original);
    printf("SPU FP scope state: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
