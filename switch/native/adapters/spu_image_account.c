/* Per-SPU-image accounting, see spu_image_account.h. Decision 104 (2026-11-13). */
#include <sys/stat.h>
#include <stdio.h>
#include "spu_image_account.h"
#include "../spu_lifted/spu_lifted_hybrid.h"
#ifdef PS3_SPU_NATIVE_INFLATE
#include "../spu_lifted/jobs/native_inflate.h"
#endif
#include <stdio.h>
#include <string.h>

enum { SLOTS = PS3_SPU_ACCT_SLOTS, NAME_BYTES = 40 };
void ps3_spu_run_profile_report(void (*print)(const char *line), unsigned seconds) __attribute__((weak));   /* spu_group_runner.c, absent in host tests */
typedef struct {
    unsigned kind;                 /* 0 = free; written once, last (release) after key/name/entry/size */
    uint64_t key;
    uint32_t entry, size;
    char name[NAME_BYTES];
    uint64_t instr, interp_ns, lifted_ns, enters, calls;
    /* round 2: escapes of lifted code by reason (PS3_ESC_*), the heaviest escape targets (pc | reason << 24, hits; Misra-Gries), and
     * up to two policy-module slots this job was called under */
    uint64_t esc[PS3_ESC_REASONS];
    uint32_t esc_key[PS3_ESC_TOP], esc_hits[PS3_ESC_TOP];
    uint16_t pol[2];
    uint64_t run_n, run_ns, run_max_ns, run_unseen;   /* Decision 134: job run time (call to return to the policy module), and jobs whose end was not seen */
} acct_slot;
static acct_slot g_slots[SLOTS];   /* slot 0 stays free: "unattributed" */
static unsigned g_used = 1;

/* Standard local-store map (256-byte pages): kernel < 0xA00 <= policy < 0x4000 <= job < 0x3E000 <= other. */
static uint8_t g_region_map[SPU_LS_SIZE >> 8];
static unsigned g_region_ready;
static void region_map_init(void) {
    if (__atomic_load_n(&g_region_ready, __ATOMIC_ACQUIRE)) return;
    for (unsigned page = 0; page < (SPU_LS_SIZE >> 8); ++page) {
        const uint32_t address = page << 8;
        g_region_map[page] = address < 0xA00u ? PS3_SPU_REGION_KERNEL : address < 0x4000u ? PS3_SPU_REGION_POLICY
                           : address < 0x3E000u ? PS3_SPU_REGION_JOB : PS3_SPU_REGION_OTHER;
    }
    __atomic_store_n(&g_region_ready, 1u, __ATOMIC_RELEASE);   /* benign race: every thread computes the same bytes */
}

uint64_t ps3_spu_acct_image_key(const uint8_t *ls, uint32_t *size) {
    uint32_t end = SPU_LS_SIZE;
    while (end >= 16u) {
        uint64_t a, b; memcpy(&a, ls + end - 16u, 8); memcpy(&b, ls + end - 8u, 8);
        if (a | b) break;
        end -= 16u;
    }
    if (size) *size = end;
    return ps3_spu_lifted_hash64w(ls, end);
}

unsigned ps3_spu_acct_slot(unsigned kind, uint64_t key, uint32_t entry, uint32_t size, const char *name) {
    const unsigned used = __atomic_load_n(&g_used, __ATOMIC_ACQUIRE);
    for (unsigned i = 1; i < used; ++i)
        if (__atomic_load_n(&g_slots[i].kind, __ATOMIC_ACQUIRE) == kind && g_slots[i].key == key) return i;
    for (;;) {
        unsigned count = __atomic_load_n(&g_used, __ATOMIC_ACQUIRE);
        if (count >= SLOTS) return 0;
        if (!__atomic_compare_exchange_n(&g_used, &count, count + 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        acct_slot *s = &g_slots[count];
        s->key = key; s->entry = entry; s->size = size;
        if (name) snprintf(s->name, NAME_BYTES, "%s", name);
        __atomic_store_n(&s->kind, kind, __ATOMIC_RELEASE);
        /* two workers can create the same (kind, key) at once: the later slot stays unused (and unprinted: no time is added to it) */
        for (unsigned i = 1; i < count; ++i)
            if (__atomic_load_n(&g_slots[i].kind, __ATOMIC_ACQUIRE) == kind && g_slots[i].key == key) { s->kind = 0; return i; }
        return count;
    }
}

void ps3_spu_acct_name(unsigned slot, const char *name) {
    if (!slot || slot >= SLOTS || !name) return;
    snprintf(g_slots[slot].name, NAME_BYTES, "%s", name);   /* names are written by one worker at a time in practice; a torn read only mangles a log label */
}

void ps3_spu_acct_arm(ps3_spu_executor *o, unsigned kernel_slot, unsigned policy_slot, unsigned job_slot, unsigned other_slot) {
    region_map_init();
    o->acct_slot[PS3_SPU_REGION_KERNEL] = (uint16_t)kernel_slot;
    o->acct_slot[PS3_SPU_REGION_POLICY] = (uint16_t)policy_slot;
    o->acct_slot[PS3_SPU_REGION_JOB] = (uint16_t)job_slot;
    o->acct_slot[PS3_SPU_REGION_OTHER] = (uint16_t)other_slot;
    o->acct_region = PS3_SPU_REGION_KERNEL; o->acct_mark = 0;
    memset(o->acct_instr, 0, sizeof o->acct_instr);
    o->region_map = g_region_map;
}

void ps3_spu_acct_job_call(void *context, ps3_spu_executor *o, const ps3_spu_lifted_module *matched) {
    const uint64_t group = (uint64_t)(uintptr_t)context;
    const uint8_t *ls = o->core.ls;
    char name[NAME_BYTES];
    /* the job: 48 bytes at the window (SpuGUID + JobCheckData), the same for every run of the same module */
    const uint64_t job_key = ps3_spu_lifted_hash64w(ls + 0x4000u, 48u);
    unsigned job = ps3_spu_acct_slot(PS3_SPU_ACCT_JOB, job_key, 0, 0, NULL);
    if (job) {
        acct_slot *s = &g_slots[job];
        if (matched) ps3_spu_acct_name(job, matched->name);
        else if (!s->name[0]) { snprintf(name, sizeof name, "job:%08x", (unsigned)(job_key >> 32)); ps3_spu_acct_name(job, name); }
        if (matched && !s->entry) s->entry = matched->entry_pc;
        const uint32_t earlier_calls = __atomic_fetch_add(&s->calls, 1u, __ATOMIC_RELAXED);
#ifdef __SWITCH__
        /* Root (2026-11-13, Decision 108): a job module with no lifted counterpart ("job:d97a9a91" ran 13.8 billion
         * interpreted instructions during loading in run rv38s). The first time one is seen its whole local store is
         * saved to <app dir>/spu-unknown/job-<key>.bin (at most 8 files), so it can be lifted on the laptop. */
        if (!matched && earlier_calls == 0u) {
            static unsigned saved;
            if (__atomic_fetch_add(&saved, 1u, __ATOMIC_RELAXED) < 8u) {
                char path[160];
                FILE *file;
                mkdir("sdmc:/switch/infamous-native-spu-stage31/spu-unknown", 0777);
                snprintf(path, sizeof path, "sdmc:/switch/infamous-native-spu-stage31/spu-unknown/job-%016llx.bin", (unsigned long long)job_key);
                file = fopen(path, "wb");
                if (file) { fwrite(ls, 1, 0x40000u, file); fclose(file); }
                printf("STARTUP_SPU_UNKNOWN_JOB key=%016llx saved=%d\n", (unsigned long long)job_key, file != NULL);
            }
        }
#else
        (void)earlier_calls;
#endif
    }
    o->acct_slot[PS3_SPU_REGION_JOB] = (uint16_t)job;
    /* the policy module in place now (WWS pm_wwsjob at LS 0xA00): its first 128 bytes, per SPURS instance */
    const uint64_t pm_hash = ps3_spu_lifted_hash64w(ls + 0xA00u, 128u);
    const uint64_t pm_key = pm_hash ^ (group * UINT64_C(0x9E3779B97F4A7C15));
    unsigned policy = ps3_spu_acct_slot(PS3_SPU_ACCT_POLICY, pm_key, 0xA00u, 0, NULL);
    if (policy && !g_slots[policy].name[0]) {
        /* the two policy modules of inFAMOUS 1, told apart by the hash of their first 128 bytes (round 2): the WWS job policy module
         * (pm_wwsjob, EBOOT file offset 0x68C880, 0x2D80 bytes) and the libsre task-set policy module (body at libsre offset 0x23680) */
        const char *kind = pm_hash == UINT64_C(0x6600294344601E95) ? "pm_wwsjob" : pm_hash == UINT64_C(0xE8A62532F773F187) ? "pm_taskset" : "policy";
        snprintf(name, sizeof name, "%s:%08x g%llx", kind, (unsigned)(pm_key >> 32), (unsigned long long)group);
        ps3_spu_acct_name(policy, name);
    }
#ifdef __SWITCH__
    /* Decision 133: the "other (stubs, top of LS)" bucket (552 M interpreted instructions in run rv62s) is code pm_wwsjob writes above LS 0x3E000 at run time (its
     * register save/restore stub at 0x3FEC0, the helpers jobs call there); no local-store dump of a pm_wwsjob thread exists to lift it from. One whole local store of a
     * pm_wwsjob thread is saved once, at the 4096th job call under pm_wwsjob (its stubs are written by then), to <app dir>/spu-unknown/wws-ls-<key>.bin. */
    if (pm_hash == UINT64_C(0x6600294344601E95)) {
        static unsigned wws_calls;
        if (__atomic_add_fetch(&wws_calls, 1u, __ATOMIC_RELAXED) == 4096u) {
            char path[160];
            mkdir("sdmc:/switch/infamous-native-spu-stage31/spu-unknown", 0777);
            snprintf(path, sizeof path, "sdmc:/switch/infamous-native-spu-stage31/spu-unknown/wws-ls-%016llx.bin", (unsigned long long)pm_key);
            FILE *file = fopen(path, "wb");
            if (file) { fwrite(ls, 1, 0x40000u, file); fclose(file); }
            printf("STARTUP_SPU_WWS_LS_SAVED key=%016llx saved=%d top_3fec0=%08x\n", (unsigned long long)pm_key, file != NULL,
                   (unsigned)ls[0x3FEC0] << 24 | (unsigned)ls[0x3FEC1] << 16 | (unsigned)ls[0x3FEC2] << 8 | ls[0x3FEC3]);
        }
    }
#endif
    if (policy) o->acct_slot[PS3_SPU_REGION_POLICY] = (uint16_t)policy;
    if (job && policy) {
        uint16_t *pol = g_slots[job].pol;
        if (pol[0] != policy && pol[1] != policy) { if (!pol[0]) pol[0] = (uint16_t)policy; else if (!pol[1]) pol[1] = (uint16_t)policy; }
    }
}

void ps3_spu_acct_job_ran(unsigned slot, uint64_t ns, int returned) {
    if (!slot || slot >= SLOTS) return;
    acct_slot *s = &g_slots[slot];
    if (!returned) { __atomic_fetch_add(&s->run_unseen, 1u, __ATOMIC_RELAXED); return; }
    __atomic_fetch_add(&s->run_n, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->run_ns, ns, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&s->run_max_ns, __ATOMIC_RELAXED);
    while (ns > seen && !__atomic_compare_exchange_n(&s->run_max_ns, &seen, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}

/* Folds the escape tables of one runner thread into the global slots. Misra-Gries summaries merge by adding the counts of equal
 * keys and, when more than PS3_ESC_TOP keys remain, subtracting the (TOP+1)-th largest count from all of them. */
static unsigned g_esc_lock;
void ps3_spu_acct_flush_escapes(ps3_spu_dyn *dyn) {
    if (!dyn || !dyn->esc_dirty) return;
    dyn->esc_dirty = 0;
    while (__atomic_exchange_n(&g_esc_lock, 1u, __ATOMIC_ACQUIRE)) { }   /* a few dozen instructions under the lock; once per slice per thread */
    for (unsigned slot = 0; slot < SLOTS; ++slot) {
        uint32_t any = 0;
        for (unsigned r = 0; r < PS3_ESC_REASONS; ++r) any |= dyn->esc_n[slot][r];
        if (!any) continue;
        acct_slot *s = &g_slots[slot];
        for (unsigned r = 0; r < PS3_ESC_REASONS; ++r) {
            __atomic_fetch_add(&s->esc[r], (uint64_t)dyn->esc_n[slot][r], __ATOMIC_RELAXED);
            dyn->esc_n[slot][r] = 0;
        }
        uint32_t key[2 * PS3_ESC_TOP], hits[2 * PS3_ESC_TOP];
        unsigned n = 0;
        for (unsigned i = 0; i < PS3_ESC_TOP; ++i) if (s->esc_hits[i]) { key[n] = s->esc_key[i]; hits[n] = s->esc_hits[i]; ++n; }
        for (unsigned i = 0; i < PS3_ESC_TOP; ++i) {
            if (!dyn->esc_hits[slot][i]) continue;
            unsigned j = 0;
            while (j < n && key[j] != dyn->esc_key[slot][i]) ++j;
            if (j < n) hits[j] += dyn->esc_hits[slot][i];
            else { key[n] = dyn->esc_key[slot][i]; hits[n] = dyn->esc_hits[slot][i]; ++n; }
        }
        for (unsigned a = 1; a < n; ++a) {   /* sort by hits, descending */
            const uint32_t k = key[a], h = hits[a]; unsigned b = a;
            while (b > 0 && hits[b - 1] < h) { key[b] = key[b - 1]; hits[b] = hits[b - 1]; --b; }
            key[b] = k; hits[b] = h;
        }
        const uint32_t cut = n > PS3_ESC_TOP ? hits[PS3_ESC_TOP] : 0u;
        for (unsigned i = 0; i < PS3_ESC_TOP; ++i) {
            s->esc_key[i] = i < n ? key[i] : 0u;
            s->esc_hits[i] = i < n && hits[i] > cut ? hits[i] - cut : 0u;
        }
        memset(dyn->esc_key[slot], 0, sizeof dyn->esc_key[slot]);
        memset(dyn->esc_hits[slot], 0, sizeof dyn->esc_hits[slot]);
    }
    __atomic_store_n(&g_esc_lock, 0u, __ATOMIC_RELEASE);
}

void ps3_spu_acct_flush(ps3_spu_executor *o, uint64_t slice_ns, uint64_t lifted_ns, uint64_t *lifted_ns_by_slot, uint64_t *enters_by_slot) {
    uint64_t total = 0;
    for (unsigned i = 0; i < SLOTS; ++i) total += o->acct_instr[i];
    const uint64_t interp_ns = slice_ns > lifted_ns ? slice_ns - lifted_ns : 0;
    for (unsigned i = 0; i < SLOTS; ++i) {
        const uint64_t v = o->acct_instr[i];
        if (v) {
            __atomic_fetch_add(&g_slots[i].instr, v, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_slots[i].interp_ns, (uint64_t)((double)interp_ns * (double)v / (double)total), __ATOMIC_RELAXED);
            o->acct_instr[i] = 0;
        }
        if (lifted_ns_by_slot && (lifted_ns_by_slot[i] || enters_by_slot[i])) {
            __atomic_fetch_add(&g_slots[i].lifted_ns, lifted_ns_by_slot[i], __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_slots[i].enters, enters_by_slot[i], __ATOMIC_RELAXED);
            lifted_ns_by_slot[i] = 0; enters_by_slot[i] = 0;
        }
    }
}

void ps3_spu_acct_flat(unsigned slot, uint64_t instructions, uint64_t interp_ns, uint64_t lifted_ns, uint64_t enters) {
    if (!slot || slot >= SLOTS) return;
    __atomic_fetch_add(&g_slots[slot].instr, instructions, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_slots[slot].interp_ns, interp_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_slots[slot].lifted_ns, lifted_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_slots[slot].enters, enters, __ATOMIC_RELAXED);
}

static const char *kind_name(unsigned kind) {
    switch (kind) {
    case PS3_SPU_ACCT_IMAGE: return "image"; case PS3_SPU_ACCT_POLICY: return "policy"; case PS3_SPU_ACCT_JOB: return "job";
    case PS3_SPU_ACCT_STATIC: return "static"; default: return "other";
    }
}

void ps3_spu_images_report(void (*print)(const char *line), unsigned seconds) {
    if (!print) return;
    unsigned order[SLOTS], count = 0;
    uint64_t total_instr = 0, total_ns = 0, total_lifted = 0;
    const unsigned used = __atomic_load_n(&g_used, __ATOMIC_ACQUIRE);
    for (unsigned i = 1; i < used && i < SLOTS; ++i) {
        if (!__atomic_load_n(&g_slots[i].kind, __ATOMIC_ACQUIRE)) continue;
        const acct_slot *s = &g_slots[i];
        const uint64_t ns = s->interp_ns + s->lifted_ns;
        if (!ns && !s->instr && !s->calls) continue;
        order[count++] = i;
        total_instr += s->instr; total_ns += s->interp_ns; total_lifted += s->lifted_ns;
    }
    for (unsigned a = 1; a < count; ++a) {   /* insertion sort, by total time, then instructions */
        const unsigned v = order[a]; unsigned b = a;
        while (b > 0 && (g_slots[order[b - 1]].interp_ns + g_slots[order[b - 1]].lifted_ns < g_slots[v].interp_ns + g_slots[v].lifted_ns)) { order[b] = order[b - 1]; --b; }
        order[b] = v;
    }
    char line[900];
    snprintf(line, sizeof line, "STARTUP_SPU_IMAGES seconds=%u images=%u interp_instr=%lluM interp_ms=%llu lifted_ms=%llu (interp time is the slice time split by instruction share; the slot table holds %u of %u)",
             seconds, count, (unsigned long long)(total_instr / 1000000u), (unsigned long long)(total_ns / 1000000u),
             (unsigned long long)(total_lifted / 1000000u), used - 1u, (unsigned)SLOTS - 1u);
    print(line);
    const uint64_t grand = total_ns + total_lifted ? total_ns + total_lifted : 1u;
    for (unsigned rank = 0; rank < count && rank < 20u; ++rank) {   /* Decision 134: 20 rows (the job rows carry the run times) */
        const acct_slot *s = &g_slots[order[rank]];
        const uint64_t ns = s->interp_ns + s->lifted_ns;
        snprintf(line, sizeof line, "STARTUP_SPU_IMAGES seconds=%u rank=%u kind=%s key=%016llx name=%s entry=0x%X size=0x%X interp_instr=%lluM interp_ms=%llu lifted_ms=%llu lifted_enters=%llu calls=%llu share=%u.%u%%",
                 seconds, rank + 1u, kind_name(s->kind), (unsigned long long)s->key, s->name[0] ? s->name : "-", s->entry, s->size,
                 (unsigned long long)(s->instr / 1000000u), (unsigned long long)(s->interp_ns / 1000000u),
                 (unsigned long long)(s->lifted_ns / 1000000u), (unsigned long long)s->enters, (unsigned long long)s->calls,
                 (unsigned)(ns * 100u / grand), (unsigned)(ns * 1000u / grand % 10u));
        /* round 2: why lifted code left lifted mode (counts by reason, the three heaviest target pcs with approximate hit counts) and
         * the policy module(s) the job was called under */
        size_t used_bytes = strlen(line);
        const uint64_t esc_total = s->esc[0] + s->esc[1] + s->esc[2] + s->esc[3] + s->esc[4];
        if (esc_total && used_bytes < sizeof line) {
            static const char *const reason_name[PS3_ESC_REASONS] = { "stub", "norow", "out", "irq", "hybrid" };
            used_bytes += (size_t)snprintf(line + used_bytes, sizeof line - used_bytes, " esc=%llu(stub:%llu,norow:%llu,out:%llu,irq:%llu,hybrid:%llu) esc_top=",
                                           (unsigned long long)esc_total, (unsigned long long)s->esc[0], (unsigned long long)s->esc[1],
                                           (unsigned long long)s->esc[2], (unsigned long long)s->esc[3], (unsigned long long)s->esc[4]);
            unsigned taken[3] = { PS3_ESC_TOP, PS3_ESC_TOP, PS3_ESC_TOP };
            for (unsigned pick = 0; pick < 3u && used_bytes < sizeof line; ++pick) {
                unsigned best = PS3_ESC_TOP;
                for (unsigned i = 0; i < PS3_ESC_TOP; ++i) {
                    if (!s->esc_hits[i] || i == taken[0] || i == taken[1]) continue;
                    if (best == PS3_ESC_TOP || s->esc_hits[i] > s->esc_hits[best]) best = i;
                }
                if (best == PS3_ESC_TOP) break;
                taken[pick] = best;
                const unsigned reason = (s->esc_key[best] >> 24) % PS3_ESC_REASONS;
                used_bytes += (size_t)snprintf(line + used_bytes, sizeof line - used_bytes, "%s0x%05X/%s:%u", pick ? "," : "",
                                               (unsigned)(s->esc_key[best] & 0xFFFFFFu), reason_name[reason], (unsigned)s->esc_hits[best]);
            }
        }
        if ((s->run_n || s->run_unseen) && used_bytes < sizeof line)   /* Decision 134: job run time, call to return */
            used_bytes += (size_t)snprintf(line + used_bytes, sizeof line - used_bytes, " run=%llu/%llums avg_us=%llu max_us=%llu unseen=%llu",
                                           (unsigned long long)s->run_n, (unsigned long long)(s->run_ns / 1000000u),
                                           (unsigned long long)(s->run_n ? s->run_ns / s->run_n / 1000u : 0u), (unsigned long long)(s->run_max_ns / 1000u),
                                           (unsigned long long)s->run_unseen);
        for (unsigned k = 0; k < 2u && used_bytes < sizeof line; ++k) {
            const unsigned p = s->pol[k];
            if (p && p < SLOTS && g_slots[p].name[0])
                used_bytes += (size_t)snprintf(line + used_bytes, sizeof line - used_bytes, "%s%s", k ? "," : " pm=", g_slots[p].name);
        }
        print(line);
    }
    {   /* round 2: the lifted policy module (pm_wwsjob): identifications tried / armed / content mismatches, job calls resolved by lifted policy code, composite images bound, channel stalls handed to the interpreter */
        uint64_t t[6];
        ps3_spu_lifted_pm_totals(t);
        if (t[0] | t[1] | t[2] | t[3] | t[4] | t[5]) {
            snprintf(line, sizeof line, "STARTUP_SPU_PM seconds=%u tries=%llu armed=%llu mismatch=%llu job_hooks=%llu composites=%llu pause_escapes=%llu", seconds,
                     (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2], (unsigned long long)t[3], (unsigned long long)t[4], (unsigned long long)t[5]);
            print(line);
        }
    }
    {   /* Decision 133: the lifted SPURS kernel: threads armed at start, content mismatches, policy-module calls made by lifted kernel code, policy modules armed there,
         * kernel exits re-entered lifted, disarms (a DMA replaced its code). The kernel's own rows above ("kind=image") then show lifted_ms. */
        uint64_t k[PS3_LIFTED_KERNEL_TOTALS];
        ps3_spu_lifted_kernel_totals(k);
        if (k[0] | k[1]) {
            snprintf(line, sizeof line, "STARTUP_SPU_KERNEL seconds=%u armed=%llu mismatch=%llu pm_calls=%llu pm_armed=%llu exits=%llu disarms=%llu", seconds,
                     (unsigned long long)k[0], (unsigned long long)k[1], (unsigned long long)k[2], (unsigned long long)k[3], (unsigned long long)k[4], (unsigned long long)k[5]);
            print(line);
        }
    }
    if (ps3_spu_run_profile_report) ps3_spu_run_profile_report(print, seconds);   /* Decision 134: STARTUP_SPU_PROFILE / _CONCURRENCY / _DOMAIN_LOCK (the runner's) */
#ifdef PS3_SPU_NATIVE_INFLATE
    {   /* round 2: the Edge zlib task's raw inflate as host zlib (native_inflate.h) */
        ps3_native_inflate_stats n;
        ps3_native_inflate_get_stats(&n);
        snprintf(line, sizeof line, "STARTUP_SPU_NATIVE_INFLATE seconds=%u calls=%llu bytes_in=%llu bytes_out=%llu native_ms=%llu fallbacks=%llu (args=%llu overlap=%llu zlib=%llu size=%llu) disabled=%llu",
                 seconds, (unsigned long long)n.calls, (unsigned long long)n.bytes_in, (unsigned long long)n.bytes_out, (unsigned long long)(n.ns / 1000000u),
                 (unsigned long long)(n.fallback_args + n.fallback_overlap + n.fallback_zlib + n.fallback_size), (unsigned long long)n.fallback_args,
                 (unsigned long long)n.fallback_overlap, (unsigned long long)n.fallback_zlib, (unsigned long long)n.fallback_size, (unsigned long long)n.fallback_disabled);
        print(line);
    }
#endif
}
