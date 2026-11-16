/* End-to-end host test: the inFAMOUS Bink decoder as an lv2 SPU thread under the REAL group runner (spu_group_runner.c),
 * with the PPU side played by the test: the thread is bound to a receive queue (193) and to a user-event queue for spup 53
 * (251), the test sends receive events (what the game's PPU does per movie job) and collects the user events the decoder
 * throws back. Built twice by verify_bink_runner.sh from a scratch copy of the tree with RUNNER_PATCH.diff applied:
 * once with -DPS3_SPU_LIFTED_BINK (hybrid lifted) and once without (interpreter, the pre-existing runner behaviour);
 * the script compares the two runs' event streams and thread records.
 *
 * usage: test_bink_runner <EBOOT.elf> <spu elf offset> <events> <label>
 * Output: BINKRUN lines; the last is `BINKRUN_RESULT label=... events=... digest=... state=... seconds=... events_per_second=...`. */
#include "../../adapters/spu_group_runner.h"
#include "../../adapters/guard_pthread.h"
#include "../../adapters/wait_port_pthread.h"
#include "../../adapters/service_thread_pthread.h"
#include "../../tests/guest_space_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef PS3_SPU_LIFTED_BINK
#include "bink_lifted.h"
#endif

enum { SPACE_BYTES = 16 * 1024 * 1024, DECODER_SPUP = 53, RECEIVE_SOURCE = 0x72616435 };

static int host_access(void *v, uint64_t address, size_t bytes, int write) {
    (void)v; (void)write;
    return address < SPACE_BYTES && bytes <= SPACE_BYTES - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
static int host_checkpoint(void *v, unsigned phase) { (void)v; (void)phase; return PS3_SPU_EXEC_OK; }
static int host_reserve(void *v, const ps3_spu_transfer *request) { (void)v; (void)request; return PS3_SPU_EXEC_OK; }
static void host_finish(void *v, const ps3_spu_transfer *request, ps3_guest_memory_status status, int committed, const void *bytes) {
    (void)v; (void)request; (void)status; (void)committed; (void)bytes;
}
static void host_trace(void *context, const char *line) { (void)context; if (getenv("BINKRUN_TRACE")) fprintf(stderr, "[runner] %s\n", line); }
static ps3_clock_status host_now(void *context, uint64_t *out) {
    static uint64_t ticks;
    (void)context;
    *out = 1000u + __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED) / 64u;
    return PS3_CLOCK_OK;
}
static double seconds_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }

static unsigned char *read_range(const char *path, size_t offset, size_t length) {
    FILE *f = fopen(path, "rb");
    unsigned char *buffer = calloc(1, length);
    if (!f || !buffer || fseek(f, (long)offset, SEEK_SET) || fread(buffer, 1, length, f) != length) { fprintf(stderr, "READ_FAILED %s\n", path); exit(1); }
    fclose(f);
    return buffer;
}
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const unsigned char *p) { return (uint32_t)p[0] << 8 | p[1]; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 5) { fprintf(stderr, "usage: %s <EBOOT.elf> <spu elf offset> <events> <label>\n", argv[0]); return 2; }
    const char *path = argv[1];
    const size_t offset = (size_t)strtoull(argv[2], NULL, 0);
    const unsigned events = (unsigned)strtoul(argv[3], NULL, 0);
    const char *label = argv[4];
    int failures = 0;
#define REQUIRE(condition, ...) do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

    /* the SPU ELF, in place from the user's EBOOT */
    unsigned char head[64];
    { unsigned char *h = read_range(path, offset, sizeof head); memcpy(head, h, sizeof head); free(h); }
    const uint32_t phoff = be32(head + 28), phnum = be16(head + 44), phentsize = be16(head + 42);
    size_t elf_length = phoff + (size_t)phnum * phentsize;
    { unsigned char *ph = read_range(path, offset + phoff, (size_t)phnum * phentsize);
      for (uint32_t k = 0; k < phnum; ++k) { const unsigned char *e = ph + (size_t)k * phentsize; const size_t end = (size_t)be32(e + 4) + be32(e + 16); if (end > elf_length) elf_length = end; }
      free(ph); }
    unsigned char *elf = read_range(path, offset, elf_length);

    /* host fixture: the same objects the composition wires */
    ElfTestHostSpace *space = elf_test_host_space_create(SPACE_BYTES);
    ps3_pthread_guard guard; ps3_pthread_wait_port wait_port;
    ps3_atomic_domain domain; ps3_atomic_owner_record records[8];
    ps3_lv2_objects *objects = calloc(1, sizeof *objects);
    ps3_lv2_spu_groups *groups = calloc(1, sizeof *groups);
    unsigned char *local_stores = calloc(PS3_LV2_SPU_GROUP_CAP * PS3_LV2_SPU_THREADS_MAX, PS3_LV2_SPU_LS_BYTES);
    if (!space || !objects || !groups || !local_stores || ps3_pthread_guard_init(&guard) || ps3_pthread_wait_port_init(&wait_port)) { fprintf(stderr, "HOST_FAILED\n"); return 1; }
    ps3_guest_memory backing = elf_test_host_space_port(space);
    if (ps3_atomic_domain_init(&domain, backing, ps3_pthread_guard_port(&guard), records, 8, NULL, 0) ||
        ps3_atomic_domain_attach_wait_port(&domain, ps3_pthread_wait_port_port(&wait_port)) || ps3_atomic_commit(&domain, 0, SPACE_BYTES) ||
        ps3_lv2_objects_init(objects, ps3_pthread_guard_port(&guard), ps3_pthread_wait_port_port(&wait_port)) ||
        ps3_lv2_spu_groups_init(groups, ps3_pthread_guard_port(&guard), local_stores, 6)) { fprintf(stderr, "HOST_INIT_FAILED\n"); return 1; }

    ps3_lv2_queue_attr attr = { PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0 };
    uint32_t receive_queue = 0, event_queue = 0;
    REQUIRE(ps3_lv2_queue_create(objects, &receive_queue, &attr, 0, 64) == PS3_LV2_OK, "receive queue");
    REQUIRE(ps3_lv2_queue_create(objects, &event_queue, &attr, 0, 64) == PS3_LV2_OK, "event queue");

    ps3_lv2_spu_images images = { 0 }; ps3_lv2_spu_image parsed = { 0 };
    unsigned char segments[PS3_LV2_SPU_IMAGE_SEGMENTS_MAX * PS3_LV2_SPU_SEGMENT_BYTES]; uint32_t segment_count = 0;
    uint32_t group_id = 0, thread_id = 0;
    const uint64_t args[4] = { UINT64_C(0x7261643500000000), 0, 0, 0 };
    REQUIRE(ps3_lv2_spu_images_init(&images, ps3_pthread_guard_port(&guard)) == 0, "images");
    REQUIRE(ps3_lv2_spu_image_parse(&images, elf, elf_length, &parsed, segments, sizeof segments, &segment_count) == 0, "parse");
    REQUIRE(ps3_lv2_spu_group_create(groups, &group_id, 1, 128, 0) == 0, "group create");
    REQUIRE(ps3_lv2_spu_thread_initialize(groups, &thread_id, group_id, 0, &parsed, segments, segment_count, elf, elf_length, args) == 0, "thread init");
    for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi)
        if (groups->groups[gi].active && groups->groups[gi].id == group_id) {
            ps3_lv2_spu_thread *thread = &groups->groups[gi].threads[0];
            /* 251. The real game's jobs throw on spup 53 (value top byte 117); the empty job of this test throws the code
             * the decoder derives from a zero descriptor, 0x40 = 64, which the runner masks to spup 0. Bind both. */
            thread->spup_bound |= (UINT64_C(1) << DECODER_SPUP) | UINT64_C(1);
            thread->spup_queue[DECODER_SPUP] = event_queue;
            thread->spup_queue[0] = event_queue;
        }
    REQUIRE(ps3_lv2_spu_thread_bind_queue(groups, thread_id, receive_queue) == 0, "193 bind");
    REQUIRE(ps3_lv2_spu_group_start(groups, group_id) == 0, "group start");
    if (failures) return 1;

    ps3_pthread_service_thread storage[PS3_LV2_SPU_THREADS_MAX]; ps3_service_thread_port ports[PS3_LV2_SPU_THREADS_MAX];
    for (unsigned i = 0; i < PS3_LV2_SPU_THREADS_MAX; ++i) { memset(&storage[i], 0, sizeof storage[i]); ports[i] = ps3_pthread_service_thread_port(&storage[i]); }
    unsigned cancel_flag = 0;
    ps3_spu_group_runner *runner = calloc(1, sizeof *runner);
    ps3_spu_group_runner_services services = { 0 };
    services.context = NULL; services.threads = ports; services.thread_port_count = PS3_LV2_SPU_THREADS_MAX;
    services.domain = &domain;
    services.services = (ps3_spu_executor_services){ NULL, host_access, host_checkpoint, host_reserve, host_finish };
    services.objects = objects; services.groups = groups; services.clock = (ps3_clock){ NULL, host_now };
    services.slice_instructions = 200000; services.pause_sleep_microseconds = 50; services.trace = host_trace; services.cancel_flag = &cancel_flag;
#ifdef PS3_SPU_LIFTED_BINK
    {   /* what ps3_spu_run_start does under the define; here the runner is driven directly */
        static const ps3_spu_lifted_module *const modules[] = { &ps3_bink_lifted_module };
        services.lifted_modules = modules; services.lifted_module_count = 1;
    }
#endif
    REQUIRE(ps3_spu_group_runner_init(runner, &services, group_id) == PS3_SPU_EXEC_OK, "runner init");
    REQUIRE(ps3_spu_group_runner_start(runner) == PS3_SPU_EXEC_OK, "runner start");
    if (failures) return 1;

    /* the PPU side: one receive event per job, one thrown event expected back per job */
    uint64_t digest = UINT64_C(0xCBF29CE484222325);
    unsigned received = 0;
    const double t0 = seconds_now();
    for (unsigned n = 0; n < events; ++n) {
        const uint64_t data1 = 0x002001A0u + 0x10u * (n & 7u) + 6u;
        const uint32_t sent = ps3_lv2_queue_send(objects, receive_queue, RECEIVE_SOURCE, data1, 0, 0);
        REQUIRE(sent == PS3_LV2_OK, "send %u status 0x%08X", n, sent);
        ps3_lv2_event event = { 0 };
        const uint32_t status = ps3_lv2_queue_receive(objects, event_queue, &event, NULL, NULL);
        REQUIRE(status == PS3_LV2_OK, "receive %u status 0x%08X", n, status);
        if (sent != PS3_LV2_OK || status != PS3_LV2_OK) break;
        ++received;
        const uint64_t words[4] = { event.source, event.data1, event.data2, event.data3 };
        for (unsigned w = 0; w < 4; ++w) for (unsigned b = 0; b < 8; ++b) { digest ^= (words[w] >> (b * 8)) & 0xFF; digest *= UINT64_C(0x100000001B3); }
        if (n < 3) printf("BINKRUN_EVENT %u source=0x%llX thread=0x%llX spup_data=0x%llX data0=0x%llX\n", n, (unsigned long long)event.source,
                          (unsigned long long)event.data1, (unsigned long long)event.data2, (unsigned long long)event.data3);
    }
    const double elapsed = seconds_now() - t0;
    (void)ps3_spu_group_runner_terminate(runner);
    uint32_t cause = 0, status = 0;
    (void)ps3_spu_group_runner_join(runner, &cause, &status);
    const ps3_spu_thread_run *run = ps3_spu_group_runner_thread(runner, 0);
    REQUIRE(run != NULL, "thread record");
    const char *state = run ? ps3_spu_thread_state_name(__atomic_load_n(&run->state, __ATOMIC_ACQUIRE)) : "none";
    printf("BINKRUN_THREAD label=%s lifted=%d state=%s events_received=%u events_sent=%u pauses=%u terminal=0x%X pc=0x%05X\n", label,
           run && run->lifted ? 1 : 0, state, run ? run->events_received : 0, run ? run->events_sent : 0, run ? run->pauses : 0,
           run ? run->terminal_code : 0, run ? run->pc : 0);
    if (run && run->lifted)
        printf("BINKRUN_LIFTED enters=%llu escapes=%llu stops=%llu waits=%llu interp_instructions=%llu\n", (unsigned long long)run->lifted_stats.enters,
               (unsigned long long)run->lifted_stats.escapes, (unsigned long long)run->lifted_stats.stops, (unsigned long long)run->lifted_stats.waits,
               (unsigned long long)run->lifted_stats.interp_instructions);
    REQUIRE(received == events, "received %u of %u thrown events", received, events);
    REQUIRE(run && !strcmp(state, "cancelled"), "thread ended %s, expected cancelled", state);
    printf("BINKRUN_RESULT label=%s events=%u digest=%016llX state=%s seconds=%.3f events_per_second=%.0f\n", label, received,
           (unsigned long long)digest, state, elapsed, elapsed > 0 ? received / elapsed : 0.0);
    free(runner); free(local_stores); free(groups); free(objects); free(elf);
    return failures ? 1 : 0;
}
