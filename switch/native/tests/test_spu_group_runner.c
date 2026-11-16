/* Decision 33 / C1b: the SPU group runner.
 *
 * usage: test_spu_group_runner
 *
 * Real host threads (the pthread service-thread port), a real atomic domain and a real ps3_lv2_objects queue -- the
 * same objects the composition wires. Five cases:
 *   1. a 2-thread group of hand-assembled SPU programs: each sends an event through WrOutMbox + WrOutIntrMbox on
 *      spup 3 (bound to a queue with 251's table), reads the CELL_OK reply from RdInMbox, then stops with 0x102 and
 *      its own exit status;
 *   2. reservation loss: GETLLAR, then a blocking RdEventStat with LR in the mask, woken by a PPU-side store;
 *   3. cancellation: a thread spinning in a branch loop is stopped by the cancel latch, and join does not hang;
 *   4. an unknown stop code is a named terminal, and join reports it;
 *   5. ps3_lv2_queue_send: wake, EBUSY when full, FENCED for a service-bound queue.
 */
#include "../adapters/spu_group_runner.h"
#include "../adapters/guard_pthread.h"
#include "../adapters/wait_port_pthread.h"
#include "../adapters/service_thread_pthread.h"
#include "guest_space_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------ SPU instruction encoding */
static uint32_t enc_ri10(uint32_t op8, uint32_t rt, uint32_t ra, uint32_t imm10) {
    return (op8 << 24) | ((imm10 & 0x3FFu) << 14) | ((ra & 0x7Fu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_ri16(uint32_t op9, uint32_t rt, uint32_t imm16) {
    return (op9 << 23) | ((imm16 & 0xFFFFu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_rr(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t rb) {
    return (op11 << 21) | ((rb & 0x7Fu) << 14) | ((ra & 0x7Fu) << 7) | (rt & 0x7Fu);
}
static uint32_t enc_stop(uint32_t code14) { return code14 & 0x3FFFu; }
static uint32_t enc_il(uint32_t rt, int32_t imm16) { return enc_ri16(0x81, rt, (uint32_t)imm16); }
static uint32_t enc_ilhu(uint32_t rt, uint32_t imm16) { return enc_ri16(0x82, rt, imm16); }
static uint32_t enc_iohl(uint32_t rt, uint32_t imm16) { return enc_ri16(0xC1, rt, imm16); }
static uint32_t enc_ai(uint32_t rt, uint32_t ra, int32_t imm) { return enc_ri10(0x1C, rt, ra, (uint32_t)imm); }
static uint32_t enc_rdch(uint32_t rt, uint32_t channel) { return enc_rr(0x00D, rt, channel, 0); }
static uint32_t enc_wrch(uint32_t rt, uint32_t channel) { return enc_rr(0x10D, rt, channel, 0); }
static uint32_t enc_br(uint32_t rt, uint32_t pc, uint32_t target) {
    return enc_ri16(0x64, rt, (uint32_t)(int32_t)(((int32_t)(target - pc)) / 4));
}

/* ---------------------------------------------------------------------------------- fixture: domain, memory, objects */
typedef struct {
    ElfTestHostSpace *space;
    ps3_guest_memory backing;
    ps3_pthread_guard guard;
    ps3_atomic_domain domain;
    ps3_atomic_owner_record records[8];
    ps3_pthread_wait_port wait_port;
    ps3_lv2_objects objects;
    ps3_lv2_spu_groups groups;
    unsigned char *local_stores;
} Host;

enum { HOST_SPACE_BYTES = 16 * 1024 * 1024 };

static int host_access(void *v, uint64_t address, size_t bytes, int write) {
    (void)v; (void)write;
    return address < HOST_SPACE_BYTES && bytes <= HOST_SPACE_BYTES - address ? PS3_SPU_EXEC_OK : PS3_SPU_EXEC_ACCESS;
}
static int host_checkpoint(void *v, unsigned phase) { (void)v; (void)phase; return PS3_SPU_EXEC_OK; }
/* Set PS3_C1B_TRACE=1 to see the runner's own start/pause/stop/event lines. */
static void host_trace(void *context, const char *line) {
    (void)context;
    if (!getenv("PS3_C1B_TRACE")) return;
    fprintf(stderr, "[c1b] %s\n", line);
}
static int host_reserve(void *v, const ps3_spu_transfer *request) { (void)v; (void)request; return PS3_SPU_EXEC_OK; }
static void host_finish(void *v, const ps3_spu_transfer *request, ps3_guest_memory_status status, int committed,
                        const void *bytes) { (void)v; (void)request; (void)status; (void)committed; (void)bytes; }
/* A controlled, monotonic clock: the runner never needs real time, only a non-decreasing source. */
static ps3_clock_status host_now(void *context, uint64_t *out) {
    static uint64_t ticks;
    (void)context;
    *out = 1000u + __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED) / 64u;
    return PS3_CLOCK_OK;
}

static int failures;
#define REQUIRE(condition, ...) do { if (!(condition)) {                    \
    fprintf(stderr, "FAIL %s:%d %s: ", __FILE__, __LINE__, #condition);     \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static void host_open(Host *host) {
    memset(host, 0, sizeof *host);
    host->space = elf_test_host_space_create(HOST_SPACE_BYTES);
    if (!host->space) { fprintf(stderr, "HOST_SPACE_FAILED\n"); exit(1); }
    if (ps3_pthread_guard_init(&host->guard)) { fprintf(stderr, "HOST_GUARD_FAILED\n"); exit(1); }
    if (ps3_pthread_wait_port_init(&host->wait_port)) { fprintf(stderr, "HOST_WAIT_PORT_FAILED\n"); exit(1); }
    host->backing = elf_test_host_space_port(host->space);
    ps3_guest_memory memory = host->backing;
    if (ps3_atomic_domain_init(&host->domain, memory, ps3_pthread_guard_port(&host->guard), host->records, 8, NULL, 0)) {
        fprintf(stderr, "HOST_DOMAIN_FAILED\n"); exit(1);
    }
    if (ps3_atomic_domain_attach_wait_port(&host->domain, ps3_pthread_wait_port_port(&host->wait_port))) {
        fprintf(stderr, "HOST_NOTIFY_FAILED\n"); exit(1);
    }
    if (ps3_atomic_commit(&host->domain, 0, HOST_SPACE_BYTES)) { fprintf(stderr, "HOST_COMMIT_FAILED\n"); exit(1); }
    if (ps3_lv2_objects_init(&host->objects, ps3_pthread_guard_port(&host->guard),
                             ps3_pthread_wait_port_port(&host->wait_port))) {
        fprintf(stderr, "HOST_OBJECTS_FAILED\n"); exit(1);
    }
    host->local_stores = calloc(PS3_LV2_SPU_GROUP_CAP * PS3_LV2_SPU_THREADS_MAX, PS3_LV2_SPU_LS_BYTES);
    if (!host->local_stores) { fprintf(stderr, "HOST_LS_FAILED\n"); exit(1); }
    if (ps3_lv2_spu_groups_init(&host->groups, ps3_pthread_guard_port(&host->guard), host->local_stores, 6)) {
        fprintf(stderr, "HOST_GROUPS_FAILED\n"); exit(1);
    }
}
static void host_close(Host *host) {
    free(host->local_stores);
    ps3_pthread_wait_port_destroy(&host->wait_port);
    ps3_pthread_guard_destroy(&host->guard);
    elf_test_host_space_destroy(host->space);
}
static ps3_spu_executor_services host_services(void) {
    return (ps3_spu_executor_services){ NULL, host_access, host_checkpoint, host_reserve, host_finish };
}

/* ------------------------------------------------------------------------------------------ group construction helpers */
enum { TEST_THREAD_COUNT = 2, TEST_SPUP = 3, TEST_ENTRY = 0x100, TEST_QUEUE_SUPPORTED = 0x1000 };

/* One thread's host port plus the runner that drives it. The ports are the composition's per-thread ports. */
typedef struct {
    ps3_pthread_service_thread storage[PS3_LV2_SPU_THREADS_MAX];
    ps3_service_thread_port ports[PS3_LV2_SPU_THREADS_MAX];
} Ports;

static void ports_init(Ports *ports) {
    for (unsigned index = 0; index < PS3_LV2_SPU_THREADS_MAX; ++index) {
        memset(&ports->storage[index], 0, sizeof ports->storage[index]);
        ports->ports[index] = ps3_pthread_service_thread_port(&ports->storage[index]);
    }
}

/* A minimal SPU ELF whose single PT_LOAD covers `bytes` of code at LS 0x100, so ps3_lv2_spu_thread_initialize can
 * build the local store through the domain exactly as the PPU's syscall does. */
static size_t build_elf(unsigned char *elf, size_t capacity, const uint32_t *code, size_t words) {
    const size_t header = 0x34, ph = 0x20, lib_off = 0x80;
    const size_t code_bytes = words * 4;
    if (capacity < lib_off + code_bytes) { fprintf(stderr, "ELF_CAPACITY\n"); exit(1); }
    memset(elf, 0, lib_off + code_bytes);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 1; elf[5] = 2; elf[6] = 1;                    /* ELFCLASS32, ELFDATA2MSB, EV_CURRENT */
    elf[0x12] = 0; elf[0x13] = 0x17;                       /* e_machine = EM_SPU */
    const uint32_t entry = TEST_ENTRY;
    elf[0x18] = (unsigned char)(entry >> 24); elf[0x19] = (unsigned char)(entry >> 16);
    elf[0x1A] = (unsigned char)(entry >> 8);  elf[0x1B] = (unsigned char)entry;
    const uint32_t phoff = (uint32_t)header;
    elf[0x1C] = (unsigned char)(phoff >> 24); elf[0x1D] = (unsigned char)(phoff >> 16);
    elf[0x1E] = (unsigned char)(phoff >> 8);  elf[0x1F] = (unsigned char)phoff;
    elf[0x2A] = 0; elf[0x2B] = (unsigned char)ph;          /* e_phentsize */
    elf[0x2C] = 0; elf[0x2D] = 1;                          /* e_phnum = 1 */
    unsigned char *program_header = elf + phoff;
    const uint32_t fields[5] = { 1u, (uint32_t)lib_off, entry, (uint32_t)code_bytes, (uint32_t)code_bytes };
    const size_t at[5] = { 0x00, 0x04, 0x08, 0x10, 0x14 };
    for (int index = 0; index < 5; ++index) {
        const uint32_t value = fields[index];
        for (int byte = 0; byte < 4; ++byte)
            program_header[at[index] + byte] = (unsigned char)(value >> (24 - byte * 8));
    }
    for (size_t index = 0; index < words; ++index) {
        const uint32_t word = code[index];
        for (int byte = 0; byte < 4; ++byte)
            elf[lib_off + index * 4 + byte] = (unsigned char)(word >> (24 - byte * 8));
    }
    return lib_off + code_bytes;
}

/* Creates a group of `threads` threads running `code`, and binds spup TEST_SPUP to `queue` on each, as 251 does. */
static uint32_t make_group_args(Host *host, uint32_t threads, const uint32_t *code, size_t words, uint32_t queue,
                                const uint64_t *arg1_per_thread, uint32_t *group_id) {
    static unsigned char elf[8192];
    const size_t elf_bytes = build_elf(elf, sizeof elf, code, words);
    ps3_lv2_spu_images images = { 0 };
    if (ps3_lv2_spu_images_init(&images, ps3_pthread_guard_port(&host->guard))) return 0xFFFFFFFFu;
    ps3_lv2_spu_image image = { 0 };
    unsigned char segments[PS3_LV2_SPU_IMAGE_SEGMENTS_MAX * PS3_LV2_SPU_SEGMENT_BYTES];
    uint32_t segment_count = 0;
    uint32_t status = ps3_lv2_spu_image_parse(&images, elf, elf_bytes, &image, segments, sizeof segments, &segment_count);
    if (status) { fprintf(stderr, "IMAGE_PARSE_FAILED 0x%08X\n", status); return status; }
    if ((status = ps3_lv2_spu_group_create(&host->groups, group_id, threads, 128, 0))) return status;
    for (uint32_t index = 0; index < threads; ++index) {
        const uint64_t args[4] = { arg1_per_thread ? arg1_per_thread[index] : 0u, 0, 0, 0 };
        uint32_t thread_id = 0;
        if ((status = ps3_lv2_spu_thread_initialize(&host->groups, &thread_id, *group_id, index, &image, segments,
                                                    segment_count, elf, elf_bytes, args)))
            return status;
        ps3_lv2_spu_group group = { 0 };
        (void)group;
        /* 251: bind spup to the queue on this thread. */
        ps3_lv2_spu_groups *groups = &host->groups;
        for (size_t gi = 0; gi < PS3_LV2_SPU_GROUP_CAP; ++gi) {
            if (!groups->groups[gi].active || groups->groups[gi].id != *group_id) continue;
            ps3_lv2_spu_thread *thread = &groups->groups[gi].threads[index];
            thread->spup_bound |= UINT64_C(1) << TEST_SPUP;
            thread->spup_queue[TEST_SPUP] = queue;
        }
    }
    return ps3_lv2_spu_group_start(&host->groups, *group_id);
}
static uint32_t make_group(Host *host, uint32_t threads, const uint32_t *code, size_t words, uint32_t queue,
                           uint32_t *group_id) {
    return make_group_args(host, threads, code, words, queue, NULL, group_id);
}

/* --- the SPU programs -------------------------------------------------------------------------------------------- */

/* Program 1: send an event on spup 3, read the reply, exit with a status derived from arg1 (r3).
 *
 * Thread `i` gets arg1 = 0x7 + i, so its exit status is 0x7 + i -- distinct per thread, and read back through 165.
 *   0x100  r6 <- r3            keep arg1 (the exit status base)
 *   0x104  il   r2, 0x1234     data0
 *   0x108  wrch MFC? no: SPU_WrOutMbox, r2
 *   0x10C  ilhu r3, spup<<8
 *   0x110  iohl r3, 0x00AA     data1
 *   0x114  wrch SPU_WrOutIntrMbox, r3   (the event; the runner delivers it and writes the reply)
 *   0x118  rdch r4, SPU_RdInMbox        (the CELL result the runner wrote)
 *   0x11C  a    r3, r6, r4              r3 = arg1 + the reply (CELL_OK = 0, so the status is arg1)
 *   0x120  stop 0x102
 */
enum { SEND_DATA0 = 0x1234, SEND_DATA1 = 0x00AA };
static size_t build_send_event_program(uint32_t *code, uint32_t spup) {
    size_t at = 0;
    /* arg1 is a u64 in r3's preferred doubleword: its LOW word (word 1 of the register). shli by 32 then shli by 32
     * would not help; instead the program adds word 1 of r3 explicitly by rotating: simplest is a word shift into
     * slot 0. `rotqbyi` moves bytes; use a 4-byte rotate so the low word lands in word 0. */
    /* Root fix (C1b): rotqbyi r6, r3, 4 moves word 1 (arg1's low word) into the preferred slot. The worker's
     * `roti r6, r3, 32` rotates each word by 32 bits (a no-op) and only passed with the runner's old swapped-word
     * argument layout. */
    code[at++] = enc_rr(0x1FC, 6, 3, 4);                /* rotqbyi r6, r3, 4 (RI7: imm in the rb field) */
    code[at++] = enc_il(2, SEND_DATA0);
    code[at++] = enc_wrch(2, SPU_WrOutMbox);            /* data0 */
    code[at++] = enc_ilhu(3, spup << 8);                /* r3 = spup << 24 */
    code[at++] = enc_iohl(3, SEND_DATA1);               /* r3 = (spup << 24) | data1 */
    code[at++] = enc_wrch(3, SPU_WrOutIntrMbox);        /* the event */
    code[at++] = enc_rdch(4, SPU_RdInMbox);             /* the CELL result the runner writes */
    code[at++] = enc_rr(0xC0, 3, 6, 4);                 /* r3 = arg1 + result (CELL_OK = 0) */
    code[at++] = enc_stop(0x102);
    return at;
}

/* ---------------------------------------------------------------------------------------------- shared test harness */

/* Runs one group to completion: build, start, join, and hand back the runner's record. The runner storage is static so
 * a test function can inspect it after the join. */
typedef struct {
    ps3_spu_group_runner runner;
    Ports ports;
    unsigned cancel_flag;
    uint32_t group_id;
    unsigned spurious_us;     /* services.spurious_lr_us (0 = off) */
} GroupRun;

static void group_run_open(GroupRun *run) {
    memset(run, 0, sizeof *run);
    ports_init(&run->ports);
}

static int group_run_start(GroupRun *run, Host *host) {
    ps3_spu_group_runner_services services = { 0 };
    services.context = host;
#ifdef PS3_TEST_RUNNER_DYN
    services.job_accounting = 1;   /* Decision 104: every thread runs through the dynamic job layer (region accounting, no modules) */
#endif
    services.threads = run->ports.ports;
    services.thread_port_count = PS3_LV2_SPU_THREADS_MAX;
    services.domain = &host->domain;
    services.services = host_services();
    services.services.context = host;
    services.objects = &host->objects;
    services.groups = &host->groups;
    services.clock = (ps3_clock){ host, host_now };
    services.spurious_lr_us = run->spurious_us;
    services.slice_instructions = 20000;
    services.pause_sleep_microseconds = 100;
    services.trace = host_trace;
    services.cancel_flag = &run->cancel_flag;
    const int init = ps3_spu_group_runner_init(&run->runner, &services, run->group_id);
    if (init) return init;
    return ps3_spu_group_runner_start(&run->runner);
}

/* ---- 1. a 2-thread group: user events, replies, joins and exit statuses ---- */
static void case_two_thread_events(void) {
    Host host; host_open(&host);
    ps3_lv2_queue_attr queue_attr = { PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0 };
    uint32_t queue_id = 0;
    REQUIRE(ps3_lv2_queue_create(&host.objects, &queue_id, &queue_attr, 0, 8) == PS3_LV2_OK, "queue create");
    uint32_t code[32];
    const size_t words = build_send_event_program(code, TEST_SPUP);
    GroupRun run; group_run_open(&run);
    /* Distinct exit statuses: thread i is given arg1 = 7 + i, and the runner puts arg1 in r3 (the SPU ABI). */
    const uint64_t arg1[TEST_THREAD_COUNT] = { 7, 8 };
    const uint32_t start_status = make_group_args(&host, TEST_THREAD_COUNT, code, words, queue_id, arg1, &run.group_id);
    REQUIRE(start_status == PS3_LV2_SPU_OK, "group start 0x%08X", start_status);
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    /* Both events must arrive: sources and data words as lv2 sends them. */
    uint32_t events = 0;
    for (; events < TEST_THREAD_COUNT; ++events) {
        ps3_lv2_event event = { 0 };
        const uint32_t receive = ps3_lv2_queue_receive(&host.objects, queue_id, &event, NULL, NULL);
        REQUIRE(receive == PS3_LV2_OK, "receive %u status 0x%08X", events, receive);
        if (receive) break;
        REQUIRE(event.source == UINT64_C(0xFFFFFFFF53505501), "source 0x%016llX", (unsigned long long)event.source);
        REQUIRE((event.data2 >> 32) == TEST_SPUP, "spup %llu", (unsigned long long)(event.data2 >> 32));
        REQUIRE((event.data2 & 0xFFFFFFu) == SEND_DATA1, "data1 0x%06llX", (unsigned long long)(event.data2 & 0xFFFFFFu));
        REQUIRE(event.data3 == SEND_DATA0, "data0 0x%06llX", (unsigned long long)event.data3);
        REQUIRE((uint32_t)event.data1 >= PS3_LV2_SPU_THREAD_BASE &&
                    (uint32_t)event.data1 < PS3_LV2_SPU_THREAD_BASE + 0x1000000u, "thread id 0x%08llX",
                    (unsigned long long)event.data1);
    }
    REQUIRE(events == TEST_THREAD_COUNT, "events received %u of %u", events, TEST_THREAD_COUNT);
    uint32_t cause = 0, status = 0;
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_OK, "join");
    REQUIRE(cause == PS3_SPU_GROUP_JOIN_ALL_THREADS_EXIT, "cause 0x%X", cause);
    for (uint32_t index = 0; index < TEST_THREAD_COUNT; ++index) {
        const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, index);
        REQUIRE(thread && __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_EXITED,
                "thread %u state", index);
        if (!thread) continue;
        REQUIRE(thread->exit_status == 7u + index, "thread %u exit %u", index, thread->exit_status);
        REQUIRE(thread->events_sent == 1, "thread %u events %u", index, thread->events_sent);
        uint32_t read_back = 0;
        REQUIRE(ps3_lv2_spu_thread_get_exit_status(&host.groups, thread->thread->id, &read_back) == PS3_LV2_SPU_OK,
                "165 for thread %u", index);
        REQUIRE(read_back == 7u + index, "165 read back %u expected %u", read_back, 7u + index);
    }
    printf("GROUP_EVENTS threads=%u events=%u cause=0x%X statuses=%u,%u\n", TEST_THREAD_COUNT, events, cause,
           run.runner.runs[0].exit_status, run.runner.runs[1].exit_status);
    host_close(&host);
}

/* ---- 2. reservation loss: GETLLAR, a blocking RdEventStat, woken by a PPU-side store ---- */
enum { LR_LINE_EA = 0x10000, LR_BUFFER_LSA = 0x200 };
static size_t build_reservation_program(uint32_t *code) {
    size_t at = 0;
    code[at++] = enc_il(1, 0xFFFFFFC8);            /* r1 = 0xFFFFFFC8: the MFC staging channel base, unused here */
    code[at++] = enc_il(2, LR_LINE_EA & 0xFFFF);   /* placeholder, replaced by the explicit stages below */
    at = 0;
    /* Stage GETLLAR: LSA, EAH, EAL, Size(irrelevant), TagID, then MFC_Cmd = 0xD0. */
    code[at++] = enc_il(2, LR_BUFFER_LSA);         /* r2 = LSA */
    code[at++] = enc_wrch(2, MFC_LSA);
    code[at++] = enc_il(3, 0);                     /* r3 = EAH */
    code[at++] = enc_wrch(3, MFC_EAH);
    code[at++] = enc_ilhu(3, LR_LINE_EA >> 16);    /* r3 = EAL = 0x10000 (root fix: EAL is the full low 32 bits; il 0 reserved EA 0) */
    code[at++] = enc_wrch(3, MFC_EAL);
    code[at++] = enc_il(3, 128);                   /* the reservation granule */
    code[at++] = enc_wrch(3, MFC_Size);
    code[at++] = enc_il(3, 1);                     /* tag 1 */
    code[at++] = enc_wrch(3, MFC_TagID);
    code[at++] = enc_il(3, 0xD0);                  /* MFC_GETLLAR_CMD */
    code[at++] = enc_wrch(3, MFC_Cmd);
    /* Enable LR and block on it. */
    code[at++] = enc_il(3, PS3_SPU_EVENT_LOCK_LINE_LOST);   /* LR, 0x400 */
    code[at++] = enc_wrch(3, SPU_WrEventMask);
    code[at++] = enc_rdch(4, SPU_RdEventStat);     /* blocks until the PPU's store loses the line */
    code[at++] = enc_ai(3, 4, 0);                  /* r3 = the event word (LR) */
    code[at++] = enc_stop(0x102);
    return at;
}

/* A PPU-side store straight through the coherent memory: ps3_atomic_write is the domain the executors share. */
static void ppu_store_line(Host *host) {
    unsigned char bytes[128];
    memset(bytes, 0xA5, sizeof bytes);
    const ps3_guest_memory_status status = ps3_atomic_write(&host->domain, LR_LINE_EA, bytes, sizeof bytes);
    REQUIRE(status == PS3_GUEST_MEMORY_OK, "PPU store status %d", (int)status);
}

static void case_reservation_loss(void) {
    Host host; host_open(&host);
    uint32_t code[64];
    const size_t words = build_reservation_program(code);
    GroupRun run; group_run_open(&run);
    const uint32_t start_status = make_group(&host, 1, code, words, 0, &run.group_id);
    REQUIRE(start_status == PS3_LV2_SPU_OK, "group start 0x%08X", start_status);
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    /* Wait, bounded, for the SPU to park on RdEventStat. */
    int paused = 0;
    for (unsigned attempt = 0; attempt < 2000 && !paused; ++attempt) {
        const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
        if (!thread) break;
        paused = __atomic_load_n(&thread->pauses, __ATOMIC_ACQUIRE) > 0 &&
                 __atomic_load_n(&thread->last_channel, __ATOMIC_ACQUIRE) == SPU_RdEventStat;
        if (!paused) {
            struct timespec brief = { 0, 1000000 };
            nanosleep(&brief, NULL);
        }
    }
    REQUIRE(paused, "the SPU parked on RdEventStat (pauses=%u)",
            ps3_spu_group_runner_thread(&run.runner, 0) ? ps3_spu_group_runner_thread(&run.runner, 0)->pauses : 0);
    ppu_store_line(&host);
    uint32_t cause = 0, status = 0;
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_OK, "join");
    const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
    REQUIRE(thread && __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_EXITED, "thread exited");
    if (thread) REQUIRE(thread->exit_status == PS3_SPU_EVENT_LOCK_LINE_LOST, "exit status 0x%X (the LR event word)", thread->exit_status);
    printf("GROUP_RESERVATION_LOSS paused=1 ppu_store_woke=1 exit=0x%X\n", thread ? thread->exit_status : 0u);
    host_close(&host);
}

/* ---- 3. cancellation: a branch loop is stopped at a slice boundary ---- */
static size_t build_spin_program(uint32_t *code) {
    size_t at = 0;
    code[at] = enc_br(0, TEST_ENTRY, TEST_ENTRY);   /* br to itself: never stops */
    return 1;
}

static void case_cancellation(void) {
    Host host; host_open(&host);
    uint32_t code[4];
    const size_t words = build_spin_program(code);
    GroupRun run; group_run_open(&run);
    const uint32_t start_status = make_group(&host, 1, code, words, 0, &run.group_id);
    REQUIRE(start_status == PS3_LV2_SPU_OK, "group start 0x%08X", start_status);
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    /* Let it run, then cancel through the latch the composition polls at every slice boundary. */
    struct timespec brief = { 0, 20000000 };
    nanosleep(&brief, NULL);
    const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
    REQUIRE(thread && __atomic_load_n(&thread->instructions, __ATOMIC_RELAXED) > 0, "the loop executed instructions");
    __atomic_store_n(&run.cancel_flag, 1u, __ATOMIC_RELEASE);
    uint32_t cause = 0, status = 0;
    /* root (C1b): a join ended by the cancellation latch reports CANCELLED (named), never a group exit */
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_CANCELLED, "join (never hangs, cancelled)");
    REQUIRE(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_CANCELLED, "cancelled state 0x%X",
            __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE));
    printf("GROUP_CANCELLATION state=%s insns=%llu\n", ps3_spu_thread_state_name(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE)),
           (unsigned long long)thread->instructions);
    host_close(&host);
}

static char g_state_lines[16][800];
static unsigned g_state_count;
static void state_capture(const char *line) { if (g_state_count < 16) snprintf(g_state_lines[g_state_count++], sizeof g_state_lines[0], "%s", line); }
static unsigned long long state_field(const char *line, const char *name) {
    const char *at = strstr(line, name);
    return at ? strtoull(at + strlen(name), NULL, 0) : ~0ull;
}
/* ---- 4. an unknown stop code is a named terminal ---- */
enum { UNKNOWN_STOP_CODE = 0x0BAD };
static size_t build_unknown_stop_program(uint32_t *code) { code[0] = enc_stop(UNKNOWN_STOP_CODE); return 1; }

static void case_unknown_stop(void) {
    Host host; host_open(&host);
    uint32_t code[4];
    const size_t words = build_unknown_stop_program(code);
    GroupRun run; group_run_open(&run);
    REQUIRE(make_group(&host, 1, code, words, 0, &run.group_id) == PS3_LV2_SPU_OK, "group start");
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    uint32_t cause = 0, status = 0;
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_OK, "join");
    const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
    REQUIRE(thread && __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_FAULT,
            "unknown stop is a fault, not a fabricated exit");
    REQUIRE(thread && thread->terminal_code == PS3_SPU_EXEC_STOP, "terminal code 0x%X", thread ? thread->terminal_code : 0u);
    /* No exit status was invented: 165 refuses. */
    uint32_t read_back = 0xDEADBEEF;
    REQUIRE(thread && ps3_lv2_spu_thread_get_exit_status(&host.groups, thread->thread->id, &read_back) != PS3_LV2_SPU_OK,
            "165 refuses a thread that did not exit through 0x102");
    g_state_count = 0;
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 1u, 5000000000ull) == 1 && !strncmp(g_state_lines[0], "STARTUP_SPU_TERMINAL ", 21) &&
            strstr(g_state_lines[0], "state=fault") != NULL, "a faulted thread is reported by the watch: %s", g_state_count ? g_state_lines[0] : "(none)");
    printf("GROUP_UNKNOWN_STOP state=%s terminal=0x%X\n",
           ps3_spu_thread_state_name(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE)),
           thread->terminal_code);
    host_close(&host);
}

/* ---- 4b. freeze diagnostics (round 2, A1): STARTUP_SPU_STATE / STARTUP_SPU_BLOCKED / STARTUP_SPU_TERMINAL ---- */
static void case_state_report(void) {
    Host host; host_open(&host);
    uint32_t code[4];
    code[0] = enc_rdch(4, SPU_RdInMbox);     /* blocks: the in-mailbox is empty */
    code[1] = enc_stop(0x102);
    GroupRun run; group_run_open(&run);
    REQUIRE(make_group(&host, 1, code, 2, 0, &run.group_id) == PS3_LV2_SPU_OK, "group start");
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    struct timespec brief = { 0, 300000000 };
    nanosleep(&brief, NULL);
    g_state_count = 0;
    ps3_spu_group_runner_state_report(&run.runner, state_capture, 42u);
    REQUIRE(g_state_count == 1, "one STATE line per thread, got %u", g_state_count);
    REQUIRE(!strncmp(g_state_lines[0], "STARTUP_SPU_STATE seconds=42 ", 29), "STATE line: %s", g_state_lines[0]);
    REQUIRE(strstr(g_state_lines[0], "state=running") != NULL, "state=running: %s", g_state_lines[0]);
    char want[40]; snprintf(want, sizeof want, "blocked=yes ch=%u ", (unsigned)SPU_RdInMbox);
    REQUIRE(strstr(g_state_lines[0], want) != NULL, "blocked on SPU_RdInMbox: %s", g_state_lines[0]);
    const unsigned long long since = state_field(g_state_lines[0], "since_ms=");
    REQUIRE(since >= 150 && since < 5000, "stalled about 300 ms, since_ms=%llu", since);
    REQUIRE(strstr(g_state_lines[0], " win4000=") != NULL && strstr(g_state_lines[0], " pm12A0=") != NULL, "job window and pm state words: %s", g_state_lines[0]);
    /* the immediate line: once per streak, only past the threshold */
    g_state_count = 0;
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 43u, 5000000000ull) == 0, "no BLOCKED line before the threshold");
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 44u, 100000000ull) == 1, "one BLOCKED line past the threshold");
    REQUIRE(g_state_count == 1 && !strncmp(g_state_lines[0], "STARTUP_SPU_BLOCKED ", 20) && strstr(g_state_lines[0], " kind=channel channel=") != NULL, "BLOCKED line: %s", g_state_lines[0]);
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 45u, 100000000ull) == 0, "the BLOCKED line is printed once per wait");
    /* cancel it: the worker ends and the watch reports the terminal once */
    __atomic_store_n(&run.cancel_flag, 1u, __ATOMIC_RELEASE);
    uint32_t cause = 0, status = 0;
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_CANCELLED, "join (cancelled)");
    g_state_count = 0;
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 46u, 100000000ull) == 1, "one TERMINAL line");
    REQUIRE(!strncmp(g_state_lines[0], "STARTUP_SPU_TERMINAL ", 21) && strstr(g_state_lines[0], "state=cancelled") != NULL, "TERMINAL line: %s", g_state_lines[0]);
    REQUIRE(ps3_spu_group_runner_watch(&run.runner, state_capture, 47u, 100000000ull) == 0, "the TERMINAL line is printed once");
    printf("GROUP_STATE_REPORT blocked_since_ms=%llu lines=state,blocked,terminal\n", since);
    host_close(&host);
}

/* ---- 4c. empty DMA lists (round 2, console run rv41s): `putl` / `getl` with a list size of 0 (or 5: no whole element) complete their tag; the thread
 * goes on and exits with the tag status. Before the fix the command ended the thread (state fault, terminal 0x2). The interpreter path of
 * test_spu_list_dma_empty.c (which drives the channel code directly). ---- */
static size_t build_empty_list_program(uint32_t *code, uint32_t command, uint32_t list_size) {
    size_t at = 0;
    code[at++] = enc_il(2, 0x300); code[at++] = enc_wrch(2, MFC_LSA);          /* destination */
    code[at++] = enc_il(3, 0);     code[at++] = enc_wrch(3, MFC_EAH);
    code[at++] = enc_il(4, 0x400); code[at++] = enc_wrch(4, MFC_EAL);          /* the list's address in the local store */
    code[at++] = enc_il(5, (int32_t)list_size); code[at++] = enc_wrch(5, MFC_Size);
    code[at++] = enc_il(6, 4);     code[at++] = enc_wrch(6, MFC_TagID);
    code[at++] = enc_il(7, (int32_t)command); code[at++] = enc_wrch(7, MFC_Cmd);
    code[at++] = enc_il(8, 16);    code[at++] = enc_wrch(8, MFC_WrTagMask);    /* tag 4 */
    code[at++] = enc_il(9, 2);     code[at++] = enc_wrch(9, MFC_WrTagUpdate);  /* wait for ALL */
    code[at++] = enc_rdch(10, MFC_RdTagStat);
    code[at++] = enc_rr(0x41, 3, 10, 10);                                      /* r3 = the tag status */
    code[at++] = enc_stop(0x102);
    return at;
}
static void case_empty_list(void) {
    static const struct { uint32_t command, size; } variants[] = { { 0x24, 0 }, { 0x44, 0 }, { 0x24, 5 }, { 0x44, 7 } };
    for (unsigned v = 0; v < sizeof variants / sizeof variants[0]; ++v) {
        Host host; host_open(&host);
        uint32_t code[32];
        const size_t words = build_empty_list_program(code, variants[v].command, variants[v].size);
        GroupRun run; group_run_open(&run);
        REQUIRE(make_group(&host, 1, code, words, 0, &run.group_id) == PS3_LV2_SPU_OK, "group start");
        REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
        uint32_t cause = 0, status = 0;
        REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_OK, "join");
        const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
        REQUIRE(thread && __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_EXITED, "cmd 0x%X size %u: the thread did not exit (state %s, terminal 0x%X)",
                variants[v].command, variants[v].size, thread ? ps3_spu_thread_state_name(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE)) : "?", thread ? thread->terminal_code : 0u);
        REQUIRE(thread && thread->exit_status == 16u, "tag status %u (want 16) for cmd 0x%X size %u", thread ? thread->exit_status : 0u, variants[v].command, variants[v].size);
        host_close(&host);
    }
    printf("GROUP_EMPTY_LIST putl/getl with list size 0, 5 and 7 complete tag 4 and the thread exits\n");
}

/* ---- 4d. spurious reservation-lost wake (round 2, console run rv47s) ---- */
/* A thread parked in SPU_RdEventStat with the LR bit in its mask is given a reservation-lost event after services.spurious_lr_us, as a real loss latches it;
 * a thread waiting on another event mask is not; with the bound 0 (the default of these tests) nobody is. The toy kernel here is the SPURS idle loop in miniature:
 * GETLLAR, wait for LR, acknowledge it, three times, then exit with the number of events it saw. */
static size_t build_idle_loop_program(uint32_t *code, uint32_t event_mask, uint32_t loops) {
    size_t at = 0;
    code[at++] = enc_il(2, LR_BUFFER_LSA); code[at++] = enc_wrch(2, MFC_LSA);
    code[at++] = enc_il(3, 0);             code[at++] = enc_wrch(3, MFC_EAH);
    code[at++] = enc_ilhu(3, LR_LINE_EA >> 16); code[at++] = enc_wrch(3, MFC_EAL);
    code[at++] = enc_il(3, 128);           code[at++] = enc_wrch(3, MFC_Size);
    code[at++] = enc_il(3, 1);             code[at++] = enc_wrch(3, MFC_TagID);
    code[at++] = enc_il(3, (int32_t)event_mask); code[at++] = enc_wrch(3, SPU_WrEventMask);
    code[at++] = enc_il(6, (int32_t)loops);                  /* r6 = loops left */
    code[at++] = enc_il(7, 0);                               /* r7 = events seen */
    const size_t top = at;
    code[at++] = enc_il(3, 0xD0);          code[at++] = enc_wrch(3, MFC_Cmd);     /* GETLLAR: reserve the line again */
    code[at++] = enc_rdch(4, SPU_RdEventStat);                                    /* parks here until LR */
    code[at++] = enc_wrch(4, SPU_WrEventAck);                                     /* acknowledge what was seen */
    code[at++] = enc_ai(7, 7, 1);
    code[at++] = enc_ai(6, 6, -1);
    /* brnz r6, top : RI16 op 0x42 */
    code[at] = enc_ri16(0x42, 6, (uint32_t)(int32_t)(((int32_t)(top - at)) & 0xFFFF)); ++at;
    code[at++] = enc_rr(0x41, 3, 7, 7);                                           /* r3 = events seen */
    code[at++] = enc_stop(0x102);
    return at;
}
static void case_spurious_lr(void) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        /* 0: bound 2 ms, mask LR -> the loop completes on spurious events alone; 1: bound 2 ms, mask SN (0x2) -> never woken; 2: bound off, mask LR -> never woken */
        Host host; host_open(&host);
        uint32_t code[48];
        const size_t words = build_idle_loop_program(code, variant == 1 ? 0x2u : PS3_SPU_EVENT_LOCK_LINE_LOST, 3);
        GroupRun run; group_run_open(&run); run.spurious_us = variant == 2 ? 0u : 2000u;
        REQUIRE(make_group(&host, 1, code, words, 0, &run.group_id) == PS3_LV2_SPU_OK, "group start");
        REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
        const ps3_spu_thread_run *thread = ps3_spu_group_runner_thread(&run.runner, 0);
        struct timespec brief = { 0, variant == 0 ? 500000000 : 120000000 };    /* up to 0.5 s for the three events (2 ms each), 0.12 s of quiet otherwise */
        for (unsigned waited = 0; waited < 50; ++waited) {
            if (variant == 0 && __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_EXITED) break;
            struct timespec tick = { 0, 10000000 }; nanosleep(&tick, NULL);
            if (variant != 0 && waited == 11) break;
        }
        (void)brief;
        if (variant == 0) {
            REQUIRE(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_EXITED, "spurious LR: the idle loop did not finish (state %s, spurious %llu)",
                    ps3_spu_thread_state_name(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE)), (unsigned long long)thread->spurious_lr);
            REQUIRE(thread->exit_status == 3u, "events seen %u", thread->exit_status);
            REQUIRE(thread->spurious_lr == 3u, "spurious events delivered %llu (want 3)", (unsigned long long)thread->spurious_lr);
            { uint32_t cause = 0, status = 0; REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_OK, "join (the worker still records the end after publishing the state)"); }
            printf("GROUP_SPURIOUS_LR delivered=%llu useful=%llu avg_instructions=%llu (toy idle loop)\n", (unsigned long long)thread->spurious_lr, (unsigned long long)thread->spurious_useful,
                   (unsigned long long)(thread->spurious_lr ? thread->spurious_instr_total / thread->spurious_lr : 0));
            REQUIRE(thread->spurious_useful == 0, "the toy idle loop found no work, useful=%llu", (unsigned long long)thread->spurious_useful);
        } else {
            REQUIRE(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == PS3_SPU_THREAD_RUNNING, "variant %u: the thread must stay parked (state %s)", variant,
                    ps3_spu_thread_state_name(__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE)));
            REQUIRE(thread->spurious_lr == 0, "variant %u: no spurious event may be delivered, got %llu", variant, (unsigned long long)thread->spurious_lr);
            __atomic_store_n(&run.cancel_flag, 1u, __ATOMIC_RELEASE);
            uint32_t cause = 0, status = 0;
            (void)ps3_spu_group_runner_join(&run.runner, &cause, &status);
        }
        host_close(&host);
    }
    printf("GROUP_SPURIOUS_LR_GATES mask 0x2 and bound 0 are never woken\n");
}

/* ---- 5. ps3_lv2_queue_send unit tests: wake, EBUSY when full, FENCED ---- */
static void case_queue_send(void) {
    Host host; host_open(&host);
    ps3_lv2_queue_attr queue_attr = { PS3_SYNC_FIFO, PS3_PPU_QUEUE, 0 };
    uint32_t queue_id = 0;
    REQUIRE(ps3_lv2_queue_create(&host.objects, &queue_id, &queue_attr, 0, 2) == PS3_LV2_OK, "queue create");
    REQUIRE(ps3_lv2_queue_send(&host.objects, queue_id, 1, 2, 3, 4) == PS3_LV2_OK, "send 1");
    REQUIRE(ps3_lv2_queue_send(&host.objects, queue_id, 5, 6, 7, 8) == PS3_LV2_OK, "send 2");
    REQUIRE(ps3_lv2_queue_send(&host.objects, queue_id, 9, 10, 11, 12) == PS3_LV2_EBUSY, "full queue is EBUSY");
    ps3_lv2_event event = { 0 };
    REQUIRE(ps3_lv2_queue_receive(&host.objects, queue_id, &event, NULL, NULL) == PS3_LV2_OK, "receive 1");
    REQUIRE(event.source == 1 && event.data1 == 2 && event.data2 == 3 && event.data3 == 4, "first event words");
    REQUIRE(ps3_lv2_queue_send(&host.objects, queue_id, 9, 10, 11, 12) == PS3_LV2_OK, "send after a receive");
    REQUIRE(ps3_lv2_queue_send(&host.objects, 0x7FFFFFFFu, 0, 0, 0, 0) == PS3_LV2_ESRCH, "unknown queue is ESRCH");
    /* A service-bound queue accepts only its owned pattern: anything else is FENCED. */
    uint32_t bound_id = 0;
    REQUIRE(ps3_lv2_queue_create(&host.objects, &bound_id, &queue_attr, 0, 4) == PS3_LV2_OK, "service queue create");
    const ps3_lv2_event owned = { 0, 0x11, 0x22, 0x33 };
    REQUIRE(ps3_lv2_queue_bind_service(&host.objects, bound_id, owned) == PS3_LV2_OK, "bind service");
    REQUIRE(ps3_lv2_queue_send(&host.objects, bound_id, 0, 0x11, 0x22, 0x33) == PS3_LV2_OK, "the owned event is accepted");
    REQUIRE(ps3_lv2_queue_send(&host.objects, bound_id, 0, 0x99, 0x22, 0x33) == PS3_LV2_FENCED,
            "a foreign event is FENCED");
    printf("QUEUE_SEND wake=1 ebusy_when_full=1 fenced=1 esrch=1\n");
    host_close(&host);
}

/* ---- 4d. group-shared diagnostics (round 2, run rv50s): a thread that holds a reservation on a 128-byte line at EA >= 0x10000 (the SPURS object) makes the report dump that
 * line, its workload table and the tasksets the table names; read-only, one report per minute in the composition. ---- */
static void case_shared_dump(void) {
    Host host; host_open(&host);
    unsigned char spurs[0x100];
    memset(spurs, 0, sizeof spurs);
    /* workload 0 of the table at +0xB00: image EA (be64), argument = taskset EA 0x30000 (be64), size, priorities */
    unsigned char entry[0x20]; memset(entry, 0, sizeof entry);
    entry[8] = 0; entry[9] = 0; entry[10] = 0; entry[11] = 0; entry[12] = 0; entry[13] = 3; entry[14] = 0; entry[15] = 0;
    unsigned char marker[16]; for (unsigned i = 0; i < sizeof marker; ++i) marker[i] = (unsigned char)(0xA0 + i);
    spurs[0x70] = 0x12; spurs[0x71] = 0x34;
    REQUIRE(host.backing.write(host.backing.context, 0x20000, spurs, sizeof spurs) == PS3_GUEST_MEMORY_OK, "spurs object");
    REQUIRE(host.backing.write(host.backing.context, 0x20000 + 0xB00, entry, sizeof entry) == PS3_GUEST_MEMORY_OK, "workload table");
    REQUIRE(host.backing.write(host.backing.context, 0x30000, marker, sizeof marker) == PS3_GUEST_MEMORY_OK, "taskset");
    uint32_t code[16]; size_t at = 0;
    code[at++] = enc_il(2, 0x300);   code[at++] = enc_wrch(2, MFC_LSA);
    code[at++] = enc_il(3, 0);       code[at++] = enc_wrch(3, MFC_EAH);
    code[at++] = enc_ilhu(4, 2);     code[at++] = enc_wrch(4, MFC_EAL);       /* EA 0x20000 */
    code[at++] = enc_il(5, 0xD0);    code[at++] = enc_wrch(5, MFC_Cmd);       /* GETLLAR */
    code[at++] = enc_rdch(6, MFC_RdAtomicStat);
    code[at++] = enc_rdch(7, SPU_RdInMbox);                                   /* blocks */
    code[at++] = enc_stop(0x102);
    GroupRun run; group_run_open(&run);
    REQUIRE(make_group(&host, 1, code, at, 0, &run.group_id) == PS3_LV2_SPU_OK, "group start");
    REQUIRE(group_run_start(&run, &host) == PS3_SPU_EXEC_OK, "runner start");
    struct timespec brief = { 0, 300000000 };
    nanosleep(&brief, NULL);
    g_state_count = 0;
    ps3_spu_group_runner_state_report(&run.runner, state_capture, 60u);
    unsigned spurs_lines = 0, workload_lines = 0, signal_lines = 0, taskset_lines = 0, state_lines = 0, marker_seen = 0;
    for (unsigned i = 0; i < g_state_count; ++i) {
        const char *line = g_state_lines[i];
        if (!strncmp(line, "STARTUP_SPU_STATE ", 18)) { ++state_lines; REQUIRE(strstr(line, " ev_sent=0 ev_busy=0 ev_notconn=0 last_ev=q0x00000000/spup0/d0x000000") != NULL, "event fields: %s", line); }
        if (strstr(line, "STARTUP_SPURS_DUMP ") && strstr(line, "what=spurs wid=-1 base=0x00020000 +0x")) ++spurs_lines;
        if (strstr(line, "what=spurs_workloads ")) ++workload_lines;
        if (strstr(line, "what=spurs_signal ")) ++signal_lines;
        if (strstr(line, "what=taskset wid=0 base=0x00030000 +0x000=A0A1A2A3A4A5A6A7A8A9AAABACADAEAF")) ++marker_seen;
        if (strstr(line, "what=taskset wid=0 base=0x00030000 ")) ++taskset_lines;
    }
    REQUIRE(state_lines == 1 && spurs_lines == 4 && workload_lines == 2 && signal_lines == 1 && taskset_lines == 6 && marker_seen == 1,
            "dump lines: state %u spurs %u workloads %u signal %u taskset %u marker %u (of %u lines)", state_lines, spurs_lines, workload_lines, signal_lines, taskset_lines, marker_seen, g_state_count);
    __atomic_store_n(&run.cancel_flag, 1u, __ATOMIC_RELEASE);
    uint32_t cause = 0, status = 0;
    REQUIRE(ps3_spu_group_runner_join(&run.runner, &cause, &status) == PS3_SPU_EXEC_CANCELLED, "join (cancelled)");
    printf("GROUP_SHARED_DUMP spurs=%u workloads=%u signal=%u taskset=%u (reservation line 0x20000, taskset 0x30000)\n", spurs_lines, workload_lines, signal_lines, taskset_lines);
    host_close(&host);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    case_queue_send();
    case_two_thread_events();
    case_reservation_loss();
    case_cancellation();
    case_state_report();
    case_unknown_stop();
    case_empty_list();
    case_spurious_lr();
    case_shared_dump();
    if (failures) { printf("overall=FAIL failures=%d\n", failures); return 1; }
    puts("overall=PASS");
    return 0;
}
