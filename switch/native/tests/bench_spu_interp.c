/* Root (2026-11-12): speed of the native SPU interpreter on the host, for profiling (Decision 80).
 * Not a correctness test: test_spu_interp.c is, and this file reuses its fixture by including it.
 * Build: native/tests/bench_spu_interp.sh <fresh out dir>. Prints BENCH_SPU instructions, seconds, MIPS. */
#define main spu_interp_test_main
#include "test_spu_interp.c"
#undef main
#include <time.h>

static double seconds_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

enum { BENCH_PC = 0x1000, BENCH_DATA = 0x20000 };

/* A loop of the kinds of instruction a codec's inner loops are made of: integer adds, a load and a store of a
 * quadword, logic, shifts, a byte shuffle, a select and the loop branch. 12 instructions an iteration. */
static uint32_t build_loop(ps3_spu_executor *owner, uint32_t iterations) {
    Assembler a = { BENCH_PC };
    emit(owner, &a, enc_ilhu(1, iterations >> 16));
    emit(owner, &a, enc_iohl(1, iterations & 0xFFFF));
    emit(owner, &a, enc_ilhu(5, BENCH_DATA >> 16));
    emit(owner, &a, enc_iohl(5, BENCH_DATA & 0xFFFF));
    const uint32_t loop = a.pc;
    emit(owner, &a, enc_ai(2, 2, 1));
    emit(owner, &a, enc_a(3, 3, 2));
    emit(owner, &a, enc_lqd(4, 5, 0));
    emit(owner, &a, enc_xor(6, 3, 4));
    emit(owner, &a, enc_andi(7, 6, 0xFF));
    emit(owner, &a, enc_shli(8, 7, 2));
    emit(owner, &a, enc_roti(9, 8, 3));
    emit(owner, &a, enc_shufb(10, 6, 9, 4));
    emit(owner, &a, enc_selb(11, 10, 6, 7));
    emit(owner, &a, enc_stqd(11, 5, 16));
    emit(owner, &a, enc_ai(1, 1, -1));
    emit(owner, &a, enc_br_relative(0x042, 1, a.pc, loop));   /* brnz r1, loop */
    emit(owner, &a, enc_stop(0x3FFF));
    return BENCH_PC;
}

int main(int argc, char **argv) {
    const uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 20000000u;
    Fixture f; fixture_open(&f);
    ps3_spu_executor *owner = interpreter_owner(&f, 0);
    ps3_spu_executor_set_channel_observer(owner, NULL, NULL);
    interpreter_start(owner);
    const uint32_t entry = build_loop(owner, iterations);
    ps3_spu_interp_result result;
    uint64_t total = 0;
    uint32_t pc = entry;
    const double start = seconds_now();
    for (;;) {   /* the runner's own slice length, so the per-slice cost is in the measurement */
        if (ps3_spu_executor_run_interp(owner, pc, 200000, &result) != 0) { fprintf(stderr, "BENCH_RUN_FAILED\n"); return 1; }
        total += result.instructions;
        pc = result.pc;
        if (result.kind != PS3_SPU_INTERP_BUDGET) break;
    }
    const double elapsed = seconds_now() - start;
    printf("BENCH_SPU kind=%d instructions=%llu seconds=%.3f mips=%.1f r3=%08X\n", (int)result.kind,
           (unsigned long long)total, elapsed, (double)total / elapsed / 1e6, owner->core.gpr[3]._u32[0]);
    interpreter_release(owner); fixture_close(&f);
    return result.kind == PS3_SPU_INTERP_STOPPED ? 0 : 1;
}
