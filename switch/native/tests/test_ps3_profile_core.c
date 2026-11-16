/* Host test of the profiler's counting and dump (native/adapters/ps3_profile_core.c). usage:
 *   test_ps3_profile_core <dump path> <shift> <func_A> <func_B> <memory_helper> <ref_function> <other_function>
 * The addresses are those of functions of a small ELF the verifier builds (verify_ps3_profile.sh); <shift> is added to all of them (the NRO's load base). The test feeds a known
 * mix of samples, checks the totals, the overflow accounting and the reset, and writes the dump that tools/ps3_profile_report.py is then run on. */
#include "../adapters/ps3_profile_core.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #c); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

int main(int argc, char **argv) {
    if (argc < 8) { fprintf(stderr, "usage: %s dump shift A B memory ref other\n", argv[0]); return 2; }
    const char *path = argv[1];
    const uint64_t shift = strtoull(argv[2], NULL, 0), a = strtoull(argv[3], NULL, 0) + shift, b = strtoull(argv[4], NULL, 0) + shift, helper = strtoull(argv[5], NULL, 0) + shift,
                   ref = strtoull(argv[6], NULL, 0) + shift, other = strtoull(argv[7], NULL, 0) + shift;
    uint64_t m, o, chain, dropped;

    /* 1. a run of known samples: A is in the generated code (600), the memory helper called from A (300, chain: A, B), a stray (100) */
    ps3_profile_core_reset();
    for (unsigned i = 0; i < 600; ++i) { ps3_profile_sample s = { a + 0x10 + 4u * (i % 4u), b + 8u, { b + 8u }, 1 }; s.ret[0] = b + 8u; ps3_profile_core_add_main(&s); }
    for (unsigned i = 0; i < 300; ++i) { ps3_profile_sample s = { helper + 8u, a + 0x20, { a + 0x20, b + 8u }, 2 }; ps3_profile_core_add_main(&s); }
    for (unsigned i = 0; i < 100; ++i) { ps3_profile_sample s = { other + 4u, 0, { 0 }, 0 }; ps3_profile_core_add_main(&s); }
    for (unsigned i = 0; i < 40; ++i) ps3_profile_core_add_other(3, other + 4u * (i % 2u));
    for (unsigned i = 0; i < 10; ++i) ps3_profile_core_add_other(7, a + 0x30);
    ps3_profile_core_totals(&m, &o, &chain, &dropped);
    CHECK(m == 1000 && o == 50 && chain == 900 && dropped == 0, "totals main %llu other %llu chain %llu dropped %llu", (unsigned long long)m, (unsigned long long)o, (unsigned long long)chain, (unsigned long long)dropped);

    const char *names[3] = { "ps3_profile_start", "ps3_profile_note_thread", "__wrap_threadCreate" };
    const uint64_t addresses[3] = { ref, 0, 0 };
    ps3_profile_thread_info threads[3] = { { 0x1111, other, 100, 0, 1 }, { 0x2222, other, 40, 3, 0 }, { 0x3333, a, 10, 7, 0 } };
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "open %s", path);
    const int lines = ps3_profile_core_write(f, 60, 60, 1000, names, addresses, 1, threads, 3, 3000);
    if (f) fclose(f);
    CHECK(lines > 8, "dump lines %d", lines);
    CHECK(ps3_profile_core_write(NULL, 0, 0, 0, NULL, NULL, 0, NULL, 0, 0) == -1, "NULL file");

    /* 2. the tables are fixed: far more distinct pcs than a table holds are counted as dropped, never crash, never lose the totals */
    ps3_profile_core_reset();
    for (uint64_t i = 0; i < 400000; ++i) { ps3_profile_sample s = { 0x1000000u + 4u * i, 0x2000000u + 4u * i, { 0x3000000u + 4u * i }, 1 }; ps3_profile_core_add_main(&s); }
    ps3_profile_core_totals(&m, &o, &chain, &dropped);
    CHECK(m == 400000 && dropped > 0, "overflow: main %llu dropped %llu", (unsigned long long)m, (unsigned long long)dropped);

    /* 3. reset forgets everything */
    ps3_profile_core_reset();
    ps3_profile_core_totals(&m, &o, &chain, &dropped);
    CHECK(!m && !o && !chain && !dropped, "reset");
    { FILE *g = tmpfile(); CHECK(ps3_profile_core_write(g, 1, 1, 1000, names, addresses, 1, threads, 0, 10) == 3, "an empty interval writes header, REF and END"); if (g) fclose(g); }

    printf("PROFILE_CORE %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
