#include "ps3_profile_core.h"
#include <stdlib.h>
#include <string.h>

enum { PC_BITS = 15, LR_BITS = 14, STACK_BITS = 14, OTHER_BITS = 15, PROBES = 24 };
typedef struct { uint64_t key; uint32_t count; uint32_t used; } pc_entry;
typedef struct { uint64_t pc, ret[PS3_PROFILE_FRAMES]; uint32_t count, used; } stack_entry;
typedef struct { uint64_t pc; uint32_t slot, count; } other_entry;
static pc_entry g_pc[1u << PC_BITS], g_lr[1u << LR_BITS];
static stack_entry g_stack[1u << STACK_BITS];
static other_entry g_other[1u << OTHER_BITS];
static uint64_t g_main, g_others, g_chain, g_dropped;

static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= UINT64_C(0xFF51AFD7ED558CCD); x ^= x >> 33; x *= UINT64_C(0xC4CEB9FE1A85EC53); x ^= x >> 33; return x; }

void ps3_profile_core_reset(void) {
    memset(g_pc, 0, sizeof g_pc); memset(g_lr, 0, sizeof g_lr); memset(g_stack, 0, sizeof g_stack); memset(g_other, 0, sizeof g_other);
    g_main = g_others = g_chain = g_dropped = 0;
}
static void count_pc(pc_entry *table, unsigned bits, uint64_t key) {
    const uint64_t mask = (UINT64_C(1) << bits) - 1u;
    uint64_t at = mix(key) & mask;
    for (unsigned probe = 0; probe < PROBES; ++probe, at = (at + 1u) & mask) {
        if (!table[at].used) { table[at].used = 1; table[at].key = key; table[at].count = 1; return; }
        if (table[at].key == key) { ++table[at].count; return; }
    }
    ++g_dropped;
}
void ps3_profile_core_add_main(const ps3_profile_sample *s) {
    if (!s) return;
    ++g_main;
    count_pc(g_pc, PC_BITS, s->pc & ~UINT64_C(3));
    count_pc(g_lr, LR_BITS, s->lr);
    {
        stack_entry probe_key; memset(&probe_key, 0, sizeof probe_key);
        probe_key.pc = s->pc & ~UINT64_C(3);
        for (unsigned i = 0; i < s->frames && i < PS3_PROFILE_FRAMES; ++i) probe_key.ret[i] = s->ret[i];
        if (s->frames) ++g_chain;
        uint64_t h = mix(probe_key.pc); for (unsigned i = 0; i < PS3_PROFILE_FRAMES; ++i) h = mix(h ^ probe_key.ret[i]);
        const uint64_t mask = (UINT64_C(1) << STACK_BITS) - 1u;
        uint64_t at = h & mask; unsigned probe;
        for (probe = 0; probe < PROBES; ++probe, at = (at + 1u) & mask) {
            stack_entry *e = &g_stack[at];
            if (!e->used) { *e = probe_key; e->used = 1; e->count = 1; break; }
            if (e->pc == probe_key.pc && !memcmp(e->ret, probe_key.ret, sizeof e->ret)) { ++e->count; break; }
        }
        if (probe == PROBES) ++g_dropped;
    }
}
void ps3_profile_core_add_other(unsigned slot, uint64_t pc) {
    ++g_others;
    pc &= ~UINT64_C(3);
    const uint64_t mask = (UINT64_C(1) << OTHER_BITS) - 1u;
    uint64_t at = mix(pc ^ ((uint64_t)slot << 48)) & mask;
    for (unsigned probe = 0; probe < PROBES; ++probe, at = (at + 1u) & mask) {
        other_entry *e = &g_other[at];
        if (!e->count) { e->pc = pc; e->slot = slot; e->count = 1; return; }
        if (e->pc == pc && e->slot == slot) { ++e->count; return; }
    }
    ++g_dropped;
}
void ps3_profile_core_totals(uint64_t *main_samples, uint64_t *other_samples, uint64_t *with_chain, uint64_t *dropped) {
    if (main_samples) *main_samples = g_main;
    if (other_samples) *other_samples = g_others;
    if (with_chain) *with_chain = g_chain;
    if (dropped) *dropped = g_dropped;
}

static int by_pc_count(const void *a, const void *b) { const pc_entry *x = a, *y = b; return x->count < y->count ? 1 : x->count > y->count ? -1 : (x->key < y->key ? -1 : x->key > y->key); }
static int by_stack_count(const void *a, const void *b) { const stack_entry *x = a, *y = b; return x->count < y->count ? 1 : x->count > y->count ? -1 : (x->pc < y->pc ? -1 : x->pc > y->pc); }
static int by_other_count(const void *a, const void *b) { const other_entry *x = a, *y = b; return x->count < y->count ? 1 : x->count > y->count ? -1 : (x->pc < y->pc ? -1 : x->pc > y->pc); }

static int write_pc_table(FILE *f, const char *tag, const pc_entry *table, unsigned bits, unsigned limit) {
    const size_t n = (size_t)1 << bits; size_t used = 0; int lines = 0;
    pc_entry *copy = malloc(n * sizeof *copy);
    if (!copy) return 0;
    for (size_t i = 0; i < n; ++i) if (table[i].used) copy[used++] = table[i];
    qsort(copy, used, sizeof *copy, by_pc_count);
    for (size_t i = 0; i < used && i < limit; ++i, ++lines) fprintf(f, "%s %u 0x%llx\n", tag, copy[i].count, (unsigned long long)copy[i].key);
    free(copy);
    return lines;
}

int ps3_profile_core_write(FILE *f, unsigned seconds, unsigned interval_seconds, unsigned hz, const char *const *ref_names, const uint64_t *ref_addresses, unsigned ref_count,
                           const ps3_profile_thread_info *threads, unsigned thread_count, unsigned limit) {
    int lines = 0;
    if (!f) return -1;
    if (!limit) limit = 3000;
    fprintf(f, "PS3PROFILE 1 seconds=%u interval=%u hz=%u main_samples=%llu other_samples=%llu with_chain=%llu dropped=%llu\n", seconds, interval_seconds, hz,
            (unsigned long long)g_main, (unsigned long long)g_others, (unsigned long long)g_chain, (unsigned long long)g_dropped); ++lines;
    for (unsigned i = 0; i < ref_count; ++i, ++lines) fprintf(f, "REF %s 0x%llx\n", ref_names[i], (unsigned long long)ref_addresses[i]);
    for (unsigned i = 0; i < thread_count; ++i, ++lines)
        fprintf(f, "THREAD %u main=%u handle=0x%llx entry=0x%llx samples=%llu\n", threads[i].slot, threads[i].is_main, (unsigned long long)threads[i].handle,
                (unsigned long long)threads[i].entry, (unsigned long long)threads[i].samples);
    lines += write_pc_table(f, "PC", g_pc, PC_BITS, limit);
    lines += write_pc_table(f, "LR", g_lr, LR_BITS, limit);
    {
        const size_t n = (size_t)1 << STACK_BITS; size_t used = 0;
        stack_entry *copy = malloc(n * sizeof *copy);
        if (copy) {
            for (size_t i = 0; i < n; ++i) if (g_stack[i].used) copy[used++] = g_stack[i];
            qsort(copy, used, sizeof *copy, by_stack_count);
            for (size_t i = 0; i < used && i < limit; ++i, ++lines) {
                fprintf(f, "STACK %u 0x%llx", copy[i].count, (unsigned long long)copy[i].pc);
                for (unsigned k = 0; k < PS3_PROFILE_FRAMES; ++k) fprintf(f, " 0x%llx", (unsigned long long)copy[i].ret[k]);
                fputc('\n', f);
            }
            free(copy);
        }
    }
    {
        const size_t n = (size_t)1 << OTHER_BITS; size_t used = 0;
        other_entry *copy = malloc(n * sizeof *copy);
        if (copy) {
            for (size_t i = 0; i < n; ++i) if (g_other[i].count) copy[used++] = g_other[i];
            qsort(copy, used, sizeof *copy, by_other_count);
            for (size_t i = 0; i < used && i < limit; ++i, ++lines) fprintf(f, "OPC %u %u 0x%llx\n", copy[i].slot, copy[i].count, (unsigned long long)copy[i].pc);
            free(copy);
        }
    }
    fprintf(f, "END\n"); ++lines;
    return lines;
}
