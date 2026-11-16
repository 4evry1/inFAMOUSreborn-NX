#include <switch.h>
#include "adapters/guest_space_horizon.h"
#ifdef PS3_NATIVE_HEAP_BACKEND
#include "adapters/heap_guest_space.h"
#ifdef PS3_NATIVE_FLAT_GUEST
#include "adapters/guest_space_flat_horizon.h"
#endif
#include <stdlib.h>
#endif
#ifndef PS3_NATIVE_PROBE_VERSION
#define PS3_NATIVE_PROBE_VERSION "0.7.0"
#endif
#ifdef PS3_NATIVE_LWMUTEX
int native_test_lwmutex_horizon(void);
#endif
#ifdef PS3_NATIVE_USER_MEMORY
int native_test_user_memory_horizon(void);
#endif
#ifdef PS3_NATIVE_GPU_CLEAR
int native_test_gpu_clear(int corrupt_expected);
#endif
#if defined(PS3_NATIVE_NRO_HEAP_BYTES)
#if defined(PS3_NATIVE_HEAP_BACKEND) && defined(PS3_NATIVE_LLE_PROFILE)
/* Root (2026-11-11): with 36 guest threads the 256 MiB heap ran out (213 MiB guest quota + 1 MiB host stack per
 * thread): the 37th thread start returned EAGAIN. The LLE profile may ask for up to 1 GiB; the console is run with
 * the full-memory homebrew menu (LAUNCH_MEMORY actual_allocator_heap_bytes was 3.28 GiB on hardware). */
/* Root (2026-11-12, Decision 90): 1536 MiB. The game needs its whole video memory (249 MiB) besides main memory;
 * hardware run rv21 stopped an SPU job at the old 128 MiB video-memory cap. */
enum { NATIVE_EXPLICIT_HEAP_LIMIT_BYTES = 1536u * 1024u * 1024u };
#elif defined(PS3_NATIVE_HEAP_BACKEND)
enum { NATIVE_EXPLICIT_HEAP_LIMIT_BYTES = 256u * 1024u * 1024u };
#else
enum { NATIVE_EXPLICIT_HEAP_LIMIT_BYTES = 64u * 1024u * 1024u };
#endif
_Static_assert(PS3_NATIVE_NRO_HEAP_BYTES > 0 &&
               PS3_NATIVE_NRO_HEAP_BYTES <= NATIVE_EXPLICIT_HEAP_LIMIT_BYTES &&
               PS3_NATIVE_NRO_HEAP_BYTES % (2u * 1024u * 1024u) == 0,
               "Explicit heap must be a positive 2MiB multiple within the backend limit");
#define PS3_NATIVE_REQUESTED_HEAP_BYTES PS3_NATIVE_NRO_HEAP_BYTES
size_t __nx_heap_size = PS3_NATIVE_NRO_HEAP_BYTES;
#elif defined(PS3_NATIVE_BOUNDED_HEAP)
#define PS3_NATIVE_REQUESTED_HEAP_BYTES (64u * 1024u * 1024u)
/* Diagnostic composition only: leave process memory available for guest pages. */
size_t __nx_heap_size = PS3_NATIVE_REQUESTED_HEAP_BYTES;
#else
#define PS3_NATIVE_REQUESTED_HEAP_BYTES 0
#endif
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int native_spu_helper_tests(void);
int native_spu_numeric_tests(void);
int native_test_double_mode(void);
int native_test_fp_state(void);
int native_test_memory_layout(bool test_sparse_pages);
int native_test_xfk(void);
int native_test_physical_pages(void);
int native_test_guest_space(void);
#if defined(PS3_NATIVE_INDIRECT_DISPATCH)
int native_test_startup_indirect(const ps3_guest_memory *memory, const char *path);
#elif defined(PS3_NATIVE_CONTEXT_MEMORY)
int native_test_startup_context(const ps3_guest_memory *memory, const char *path);
#elif defined(PS3_NATIVE_STARTUP_CLOCK)
int native_test_startup_clock(const ps3_guest_memory *memory, uintptr_t base, const char *path);
#elif defined(PS3_NATIVE_STARTUP_MEMORY)
int native_test_startup_memory(const ps3_guest_memory *memory, uintptr_t base, const char *path);
#elif defined(PS3_NATIVE_STARTUP_LWMUTEX)
int native_test_startup_lwmutex(const ps3_guest_memory *memory, uintptr_t base, const char *path);
#elif defined(PS3_NATIVE_STARTUP_RESUME)
int native_test_startup_resume(const ps3_guest_memory *memory, uintptr_t base, const char *path);
#elif !defined(PS3_NATIVE_ENTRY_CHECKPOINT)
int native_test_generated_ppu(const ps3_guest_memory *memory, uintptr_t base);
#else
int native_test_entry_checkpoint(const ps3_guest_memory *memory, uintptr_t base, const char *path);
#endif
int native_load_real_elf(const ps3_guest_memory *memory, const char *path, void *scratch, size_t scratch_size);

#ifdef PS3_NATIVE_HEAP_BACKEND
static void *heap_guest_allocate(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes);
}
static void heap_guest_deallocate(void *context, void *allocation)
{
    (void)context;
    free(allocation);
}
static void heap_guest_lock(void *context) { mutexLock(context); }
static void heap_guest_unlock(void *context) { mutexUnlock(context); }
static const uint64_t HEAP_GUEST_QUOTA_PAGES = UINT64_C(196608);   /* Decision 90: 768 MiB of guest pages (was 512 MiB) */
#endif

#if defined(PS3_NATIVE_NXLINK) || defined(PS3_NATIVE_LLE_PROFILE)   /* the log device; nxlink itself only with PS3_NATIVE_NXLINK */
/* Decision 33 / C1b (2026-11-11): live output on the real console. A hardware run used to be readable only after
 * the SD log was fetched, and svcOutputDebugString (the live trace on Eden) goes nowhere on a retail unit.
 * With this flag stdout/stderr are a TEE: every write still goes to the SD log file first (the evidence record is
 * unchanged), then to the PC that sent the NRO with `nxlink -s` (homebrew menu, Y = netloader). No PC, no Wi-Fi or
 * a refused connection leaves socket = -1 and the run is the plain file run. A send error closes the socket and
 * the run goes on. Diagnostic transport only: no game data leaves the console, only this program's own log text. */
#include <errno.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
static int g_tee_file = -1, g_tee_socket = -1;
static Mutex g_tee_mutex;
void native_nxlink_line(const char *text, size_t length)
{
    mutexLock(&g_tee_mutex);
    while (g_tee_socket >= 0 && length) {
        const ssize_t sent = send(g_tee_socket, text, length, 0);
        if (sent <= 0) { close(g_tee_socket); g_tee_socket = -1; break; }
        text += sent; length -= (size_t)sent;
    }
    mutexUnlock(&g_tee_mutex);
}
/* Root (2026-11-12, Decision 93): the log is collected in memory and written to the SD card in blocks. stdout is
 * unbuffered, so every printf used to be one file-system call of its own, several per frame from the renderer,
 * on the thread that printed. native_log_flush() is called by the heartbeat thread every 2 s, when the buffer is
 * three quarters full, by fsync and at exit; a crash can lose at most the last 2 s of text. */
enum { TEE_BUFFER_BYTES = 256 * 1024 };
static char g_tee_buffer[TEE_BUFFER_BYTES];
static size_t g_tee_used;
static uint32_t g_tee_lock;   /* 0 free, 1 held: short sections, spin with yield */
static void tee_lock(void) { uint32_t expected = 0; while (!__atomic_compare_exchange_n(&g_tee_lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { expected = 0; svcSleepThread(0); } }
static void tee_unlock(void) { __atomic_store_n(&g_tee_lock, 0u, __ATOMIC_RELEASE); }
static void tee_flush_locked(void)
{
    size_t done = 0;
    while (done < g_tee_used) {
        const ssize_t written = write(g_tee_file, g_tee_buffer + done, g_tee_used - done);
        if (written <= 0) break;
        done += (size_t)written;
    }
    g_tee_used = 0;
}
void native_log_flush(void)
{
    if (g_tee_file < 0) return;
    tee_lock(); tee_flush_locked(); tee_unlock();
    /* Root (2026-11-12, Decision 100): a write alone stays in the file system's cache; closing the program with
     * HOME then loses it (run rv32: the log ended at 550 s of a longer run; the inFAMOUS 2 session saw the same).
     * This runs on the heartbeat thread every 2 s, outside the lock, so no printing thread waits for the card. */
    fsync(g_tee_file);
}
/* Root (2026-11-12, user request): "no log" mode, chosen by holding ZL while the program starts. Everything is
 * dropped except the 10-second heartbeat lines and the final RESULT line, so the frame counts of a run with and
 * without logging can be compared. */
static int g_native_log_off;
static ssize_t tee_write(struct _reent *reent, void *handle, const char *text, size_t length)
{
    (void)handle; (void)reent;
    if (g_native_log_off && !(length >= 17 && memcmp(text, "STARTUP_HEARTBEAT", 17) == 0) &&
        !(length >= 6 && memcmp(text, "RESULT", 6) == 0) && !(length >= 11 && memcmp(text, "LOG_MODE ", 9) == 0))
        return (ssize_t)length;
    tee_lock();
    if (length >= TEE_BUFFER_BYTES) {   /* larger than the buffer: straight to the file, in order */
        tee_flush_locked();
        (void)write(g_tee_file, text, length);
    } else {
        if (g_tee_used + length > TEE_BUFFER_BYTES) tee_flush_locked();
        memcpy(g_tee_buffer + g_tee_used, text, length);
        g_tee_used += length;
        if (g_tee_used > TEE_BUFFER_BYTES / 4 * 3) tee_flush_locked();
    }
    tee_unlock();
    if (g_tee_socket >= 0) native_nxlink_line(text, length);
    return (ssize_t)length;
}
static int tee_fsync(struct _reent *reent, void *handle)
{
    (void)reent; (void)handle;
    native_log_flush();
    return fsync(g_tee_file);
}
static const devoptab_t g_tee_device = {
    .name = "tee", .write_r = tee_write, .fsync_r = tee_fsync,
};
#endif

static void debug_line(const char *line)
{
    svcOutputDebugString(line, strlen(line));
#if defined(PS3_NATIVE_NXLINK) || defined(PS3_NATIVE_LLE_PROFILE)   /* the log device; nxlink itself only with PS3_NATIVE_NXLINK */
    native_nxlink_line(line, strlen(line));
#endif
}

static void log_launch_memory(void)
{
    extern char *fake_heap_start, *fake_heap_end;
    const uintptr_t heap_start = (uintptr_t)fake_heap_start;
    const uintptr_t heap_end = (uintptr_t)fake_heap_end;
    u64 heap_base = 0, resource_total = 0, resource_used = 0;
    MemoryInfo heap_info = {0};
    u32 page_info = 0;
    Result heap_result = svcGetInfo(&heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    if (R_SUCCEEDED(heap_result)) heap_result = svcQueryMemory(&heap_info, &page_info, heap_base);
    Result total_result = svcGetInfo(&resource_total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
    Result used_result = svcGetInfo(&resource_used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
    printf("LAUNCH_MEMORY requested_heap_bytes=0x%llx actual_allocator_heap_bytes=0x%llx mapped_heap_bytes=0x%llx heap_result=0x%x nso=%d heap_override=%d applet_type=%d\n",
           (unsigned long long)PS3_NATIVE_REQUESTED_HEAP_BYTES,
           (unsigned long long)(heap_end >= heap_start ? heap_end - heap_start : 0),
           (unsigned long long)heap_info.size, heap_result, envIsNso(), envHasHeapOverride(), appletGetAppletType());
    printf("SYSTEM_RESOURCE total=0x%llx total_result=0x%x used=0x%llx used_result=0x%x\n",
           (unsigned long long)resource_total, total_result,
           (unsigned long long)resource_used, used_result);
}

/* One application-data prefix selects every flag, log and EBOOT path. The N12 startup composition
 * (Makefile ENABLE_N12_STARTUP=1 -> -DPS3_NATIVE_APPDATA_STAGE31 on main.o) uses the accepted Stage33 runner
 * data directory; every other build keeps the generic probe directory. */
#ifdef PS3_NATIVE_APPDATA_STAGE31
#define NATIVE_APPDATA_DIR "sdmc:/switch/infamous-native-spu-stage31"
#else
#define NATIVE_APPDATA_DIR "sdmc:/switch/infamous-native-probe"
#endif

#include <sys/iosupport.h>
#include <sys/reent.h>
/* ---- on-screen status (root, 2026-11-12, user request) ------------------------------------------------------------
 * The user could not tell a working run from a frozen one: every build showed a black screen. libnx's text console
 * now shows what the program is doing. stdout and stderr stay the log file (and nxlink): the console is written
 * through its own device entry, saved right after consoleInit, so nothing printed for the log reaches the screen
 * and nothing shown on screen is missing from the log's own lines.
 * The Vulkan renderer needs the same window: native_status_release() gives it up before the renderer starts, and
 * native_status_begin() takes it back when the renderer could not start. */
static const devoptab_t *g_status_device;
static Mutex g_status_mutex;
static bool g_status_on;
static bool g_status_log_installed;   /* set once stdout/stderr are the log: a later consoleInit must not take them */
void native_status_begin(void)
{
    mutexLock(&g_status_mutex);
    if (!g_status_on) {
        const devoptab_t *saved_out = devoptab_list[STD_OUT], *saved_err = devoptab_list[STD_ERR];
        consoleInit(NULL);
        if (!g_status_device) g_status_device = devoptab_list[STD_ERR];   /* the console's own device (first init) */
        if (g_status_log_installed) { devoptab_list[STD_OUT] = saved_out; devoptab_list[STD_ERR] = saved_err; }
        g_status_on = true;
    }
    mutexUnlock(&g_status_mutex);
}
void native_status_release(void)
{
    mutexLock(&g_status_mutex);
    if (g_status_on) { consoleExit(NULL); g_status_on = false; }
    mutexUnlock(&g_status_mutex);
}
/* Writes `text` at 1-based `row` (clearing the rest of that row) and shows it. A no-op while the console is off. */
void native_status_row(int row, const char *text)
{
    char line[200];
    mutexLock(&g_status_mutex);
    if (g_status_on && g_status_device && g_status_device->write_r) {
        const int length = snprintf(line, sizeof line, "\x1b[%d;1H%.160s\x1b[K", row, text);
        if (length > 0) g_status_device->write_r(__getreent(), NULL, line, (size_t)length);
        consoleUpdate(NULL);
    }
    mutexUnlock(&g_status_mutex);
}

int main(void)
{
    const char *directory = NATIVE_APPDATA_DIR;
    mkdir("sdmc:/switch", 0777);
    mkdir(directory, 0777);
    const bool test_sparse_pages = access(
        NATIVE_APPDATA_DIR "/sparse-memory.flag", F_OK) == 0;
    const bool test_physical_pages = access(
        NATIVE_APPDATA_DIR "/physical-memory.flag", F_OK) == 0;
    const bool test_guest_space = access(
        NATIVE_APPDATA_DIR "/guest-space.flag", F_OK) == 0;
    const bool test_real_elf = access(
        NATIVE_APPDATA_DIR "/real-elf.flag", F_OK) == 0;
#if defined(PS3_NATIVE_STARTUP_RESUME)
    const bool test_generated_ppu = false;
#ifdef PS3_NATIVE_HEAP_THREAD_ID
    const char *startup_flag = NATIVE_APPDATA_DIR "/heap-thread-id.flag";
#elif defined(PS3_NATIVE_HEAP_STARTUP)
    const char *startup_flag = NATIVE_APPDATA_DIR "/heap-startup.flag";
#elif defined(PS3_NATIVE_THREAD_ID_QUERY)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-thread-id.flag";
#elif defined(PS3_NATIVE_STARTUP_TIMEBASE)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-timebase.flag";
#elif defined(PS3_NATIVE_INDIRECT_DISPATCH)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-indirect.flag";
#elif defined(PS3_NATIVE_CONTEXT_MEMORY)
    const char *startup_flag = NATIVE_APPDATA_DIR "/context-memory.flag";
#elif defined(PS3_NATIVE_STARTUP_CLOCK)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-clock.flag";
#elif defined(PS3_NATIVE_STARTUP_MEMORY)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-memory.flag";
#elif defined(PS3_NATIVE_STARTUP_LWMUTEX)
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-lwmutex.flag";
#else
    const char *startup_flag = NATIVE_APPDATA_DIR "/startup-resume.flag";
#endif
    const bool test_startup_resume = access(startup_flag, F_OK) == 0;
#elif defined(PS3_NATIVE_ENTRY_CHECKPOINT)
    const bool test_generated_ppu = false;
    const bool test_entry_checkpoint = access(
        NATIVE_APPDATA_DIR "/entry-checkpoint.flag", F_OK) == 0;
#else
    const bool test_generated_ppu = access(
        NATIVE_APPDATA_DIR "/generated-ppu.flag", F_OK) == 0;
#ifdef PS3_NATIVE_GPU_CLEAR
    const bool test_gpu_clear = access(NATIVE_APPDATA_DIR "/gpu-clear.flag", F_OK) == 0;
    const bool test_gpu_negative = access(NATIVE_APPDATA_DIR "/gpu-clear-negative.flag", F_OK) == 0;
#endif
#endif
    const char *suite = test_generated_ppu ? "generated-ppu" : test_real_elf ? "real-elf" : test_guest_space ? "guest-space" : test_physical_pages ? "physical-memory" :
        test_sparse_pages ? "sparse-memory" : "cpu";
#ifdef PS3_NATIVE_GPU_CLEAR
    if (test_gpu_clear || test_gpu_negative) suite = test_gpu_negative ? "gpu-clear-negative" : "gpu-clear";
#endif
#ifdef PS3_NATIVE_ENTRY_CHECKPOINT
    if (test_entry_checkpoint) suite = "entry-checkpoint";
#endif
#ifdef PS3_NATIVE_STARTUP_RESUME
#ifdef PS3_NATIVE_HEAP_THREAD_ID
    if (test_startup_resume) suite = "heap-thread-id";
#elif defined(PS3_NATIVE_HEAP_STARTUP)
    if (test_startup_resume) suite = "heap-startup";
#elif defined(PS3_NATIVE_THREAD_ID_QUERY)
    if (test_startup_resume) suite = "startup-thread-id";
#elif defined(PS3_NATIVE_STARTUP_TIMEBASE)
    if (test_startup_resume) suite = "startup-timebase";
#elif defined(PS3_NATIVE_INDIRECT_DISPATCH)
    if (test_startup_resume) suite = "startup-indirect";
#elif defined(PS3_NATIVE_CONTEXT_MEMORY)
    if (test_startup_resume) suite = "context-memory";
#elif defined(PS3_NATIVE_STARTUP_CLOCK)
    if (test_startup_resume) suite = "startup-clock";
#elif defined(PS3_NATIVE_STARTUP_MEMORY)
    if (test_startup_resume) suite = "startup-memory";
#elif defined(PS3_NATIVE_STARTUP_LWMUTEX)
    if (test_startup_resume) suite = "startup-lwmutex";
#else
    if (test_startup_resume) suite = "startup-resume";
#endif
#endif
#ifdef PS3_NATIVE_USER_MEMORY
    const bool test_user_memory = access(
        NATIVE_APPDATA_DIR "/user-memory.flag", F_OK) == 0;
    if (test_user_memory) suite = "user-memory";
#endif
#ifdef PS3_NATIVE_LWMUTEX
    const bool test_lwmutex = access(
        NATIVE_APPDATA_DIR "/lwmutex.flag", F_OK) == 0;
    if (test_lwmutex) suite = "lwmutex";
#endif
    mutexInit(&g_status_mutex);
    native_status_begin();
    native_status_row(1, "inFAMOUS native " PS3_NATIVE_PROBE_VERSION "   (test build, not the finished game)");
    native_status_row(2, "It stops by itself. To leave earlier: HOME, then close the software.");
    native_status_row(4, "starting...");
#if defined(PS3_NATIVE_NXLINK) || defined(PS3_NATIVE_LLE_PROFILE)   /* the log device; nxlink itself only with PS3_NATIVE_NXLINK */
    {   /* hold ZL during the first second and a half = no log */
        PadState pad;
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        for (int poll = 0; poll < 30 && !g_native_log_off; ++poll) {
            padUpdate(&pad);
            if (padGetButtons(&pad) & HidNpadButton_ZL) g_native_log_off = 1;
            svcSleepThread(50000000ll);
        }
        native_status_row(3, g_native_log_off ? "LOG: OFF (ZL was held). Only frame counts are saved."
                                              : "LOG: on. Hold ZL while it starts for a run without logging.");
    }
#endif
    char path[160];
    snprintf(path, sizeof(path), "%s/%s-" PS3_NATIVE_PROBE_VERSION "-%llu.log", directory, suite,
             (unsigned long long)armGetSystemTick());
    int descriptor = open(path, O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (descriptor < 0) {
        debug_line("INFAMOUS_NATIVE_PROBE ERROR log-create\n");
        return 2;
    }
#if defined(PS3_NATIVE_NXLINK) || defined(PS3_NATIVE_LLE_PROFILE)   /* the log device; nxlink itself only with PS3_NATIVE_NXLINK */
    mutexInit(&g_tee_mutex);
    g_tee_file = descriptor;   /* stays open for the whole process: it IS stdout's file half */
#ifdef PS3_NATIVE_NXLINK
    const Result socket_result = socketInitializeDefault();
    if (R_SUCCEEDED(socket_result)) {
        g_tee_socket = nxlinkConnectToHost(false, false);
        if (g_tee_socket < 0) socketExit();
    }
#endif
    devoptab_list[STD_OUT] = &g_tee_device;
    devoptab_list[STD_ERR] = &g_tee_device;
#else
    if (dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0) {
        close(descriptor);
        debug_line("INFAMOUS_NATIVE_PROBE ERROR log-redirect\n");
        return 2;
    }
    close(descriptor);
#endif
    g_status_log_installed = true;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#if defined(PS3_NATIVE_NXLINK) || defined(PS3_NATIVE_LLE_PROFILE)   /* the log device; nxlink itself only with PS3_NATIVE_NXLINK */
    printf("LOG_MODE %s\n", g_native_log_off ? "off_zl_held_heartbeat_only" : "full");
#endif
    printf("inFAMOUS native CPU diagnostic " PS3_NATIVE_PROBE_VERSION "\n");
    printf("ARM64 Horizon executable. No game boot or graphics claim.\n");
    printf("REQUESTED_SUITE %s\n", suite);
#ifdef PS3_NATIVE_NXLINK
    printf("NXLINK_TEE socket_init=0x%x connected=%d\n", socket_result, g_tee_socket >= 0);
#endif
    log_launch_memory();
    debug_line("INFAMOUS_NATIVE_PROBE START " PS3_NATIVE_PROBE_VERSION "\n");
    int failures = 0;
    failures += native_spu_helper_tests() != 0;
    failures += native_spu_numeric_tests() != 0;
    failures += native_test_double_mode() != 0;
    failures += native_test_fp_state() != 0;
    failures += native_test_xfk() != 0;
#ifdef PS3_NATIVE_HEAP_BACKEND
    printf("MEMORY_LAYOUT raw_guest_window_gate=not_requested backend=heap-sparse no_virtual_window_reservation=true\n");
    if (test_sparse_pages || test_physical_pages || test_guest_space) {
        printf("HEAP_STARTUP FAIL incompatible_raw_memory_flag\n");
        ++failures;
    }
#else
    failures += native_test_memory_layout(test_sparse_pages && !test_physical_pages) != 0;
    if (test_physical_pages) failures += native_test_physical_pages() != 0;
    if (test_guest_space) failures += native_test_guest_space() != 0;
#endif
    bool test_image_suite = test_real_elf;
#ifdef PS3_NATIVE_ENTRY_CHECKPOINT
    test_image_suite = test_image_suite || test_entry_checkpoint;
#endif
#ifdef PS3_NATIVE_STARTUP_RESUME
    test_image_suite = test_image_suite || test_startup_resume;
#endif
    if (test_image_suite || test_generated_ppu) {
        const char *guest_label = test_real_elf ? "REAL_ELF" : "GENERATED_PPU";
#ifdef PS3_NATIVE_ENTRY_CHECKPOINT
        if (test_entry_checkpoint) guest_label = "ENTRY_CHECKPOINT";
#endif
#ifdef PS3_NATIVE_STARTUP_RESUME
#ifdef PS3_NATIVE_HEAP_THREAD_ID
        if (test_startup_resume) guest_label = "HEAP_THREAD_ID";
#elif defined(PS3_NATIVE_HEAP_STARTUP)
        if (test_startup_resume) guest_label = "HEAP_STARTUP";
#elif defined(PS3_NATIVE_INDIRECT_DISPATCH)
        if (test_startup_resume) guest_label = "STARTUP_INDIRECT";
#elif defined(PS3_NATIVE_CONTEXT_MEMORY)
        if (test_startup_resume) guest_label = "CONTEXT_MEMORY";
#elif defined(PS3_NATIVE_STARTUP_CLOCK)
        if (test_startup_resume) guest_label = "STARTUP_CLOCK";
#elif defined(PS3_NATIVE_STARTUP_MEMORY)
        if (test_startup_resume) guest_label = "STARTUP_MEMORY";
#elif defined(PS3_NATIVE_STARTUP_LWMUTEX)
        if (test_startup_resume) guest_label = "STARTUP_LWMUTEX";
#else
        if (test_startup_resume) guest_label = "STARTUP_RESUME";
#endif
#endif
        u64 heap_base = 0;
        MemoryInfo heap_info = {0}; u32 page_info = 0;
        Result heap_result = svcGetInfo(&heap_base, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
        if (R_SUCCEEDED(heap_result)) heap_result = svcQueryMemory(&heap_info, &page_info, heap_base);
        printf("DIAGNOSTIC_HEAP actual_bytes=0x%llx result=0x%x nso=%d\n",
               (unsigned long long)heap_info.size, heap_result, envIsNso());
#ifdef PS3_NATIVE_HEAP_BACKEND
        Mutex backing_mutex;
        mutexInit(&backing_mutex);
        ps3_heap_guest_allocator allocator = {NULL, heap_guest_allocate, heap_guest_deallocate};
        ps3_instance_guard backing_guard = {&backing_mutex, heap_guest_lock, heap_guest_unlock};
        ps3_heap_guest_space *space = NULL;
        printf("HEAP_BACKEND logical_quota_bytes=%llu logical_quota_pages=%llu no_contiguous_guest_base=true\n",
               (unsigned long long)(HEAP_GUEST_QUOTA_PAGES * PS3_GUEST_PAGE_BYTES),
               (unsigned long long)HEAP_GUEST_QUOTA_PAGES);
        ps3_guest_memory_status status = ps3_heap_guest_space_create(
            &space, allocator, backing_guard, HEAP_GUEST_QUOTA_PAGES);
        if (status != PS3_GUEST_MEMORY_OK) {
            printf("%s FAIL heap-guest-space-create status=%d\n", guest_label, status);
#else
        ps3_horizon_guest_create_diagnostic diagnostic = {0};
        ps3_guest_memory_status invalid_status =
            ps3_horizon_guest_space_create_diagnostic(NULL, &diagnostic);
        if (invalid_status != PS3_GUEST_MEMORY_INVALID_RANGE ||
            diagnostic.stage != PS3_HORIZON_CREATE_ARGUMENT || diagnostic.kernel_result_valid) {
            printf("GUEST_SPACE_CREATE_DIAGNOSTIC FAIL invalid_argument_status=%d stage=%d result_valid=%d\n",
                   invalid_status, diagnostic.stage, diagnostic.kernel_result_valid);
            ++failures;
        }
        ps3_horizon_guest_space *space = NULL;
        ps3_guest_memory_status status = ps3_horizon_guest_space_create_diagnostic(&space, &diagnostic);
        if (status != PS3_GUEST_MEMORY_OK) {
            printf("%s FAIL guest-space-create status=%d stage=%s stage_id=%d kernel_result_valid=%d kernel_result=0x%08x\n",
                   guest_label, status, ps3_horizon_guest_create_stage_name(diagnostic.stage),
                   diagnostic.stage, diagnostic.kernel_result_valid, diagnostic.kernel_result);
#endif
            ++failures;
        } else {
#ifdef PS3_NATIVE_HEAP_BACKEND
            ps3_guest_memory memory = ps3_heap_guest_space_port(space);
#ifdef PS3_NATIVE_FLAT_GUEST
            /* Root (2026-11-12, Decision 57): prefer the flat 4 GiB window (real guest pointers); the heap-block
             * backend stays as the fallback when the platform refuses the mapping sequence. */
            ps3_flat_guest_space *flat_space = NULL;
            ps3_flat_guest_diagnostic flat_diagnostic = {0};
            const ps3_guest_memory_status flat_status =
                ps3_flat_guest_space_create(&flat_space, HEAP_GUEST_QUOTA_PAGES, &flat_diagnostic);
            if (flat_status == PS3_GUEST_MEMORY_OK) {
                memory = ps3_flat_guest_space_port(flat_space);
                g_ps3_flat_guest_base = ps3_flat_guest_space_base(flat_space);
                g_ps3_flat_guest_space = flat_space;
                printf("FLAT_GUEST selected=1 base=0x%llx slab_bytes=0x200000 quota_pages=%llu\n",
                       (unsigned long long)g_ps3_flat_guest_base, (unsigned long long)HEAP_GUEST_QUOTA_PAGES);
            } else {
                printf("FLAT_GUEST selected=0 status=%d stage=%s kernel_result=0x%08x slab_step=%u fallback=heap-blocks\n",
                       flat_status, ps3_flat_guest_stage_name(flat_diagnostic.stage), flat_diagnostic.kernel_result,
                       flat_diagnostic.slab_step);
            }
#endif
#else
            ps3_guest_memory memory = ps3_horizon_guest_space_port(space);
#endif
            if (test_image_suite) {
                unsigned char scratch[16384];
                failures += native_load_real_elf(&memory,
                    NATIVE_APPDATA_DIR "/EBOOT.elf", scratch, sizeof(scratch)) != 0;
#ifdef PS3_NATIVE_ENTRY_CHECKPOINT
                if (test_entry_checkpoint && failures == 0) {
                    failures += native_test_entry_checkpoint(&memory,
                        ps3_horizon_guest_space_base(space),
                        NATIVE_APPDATA_DIR "/EBOOT.elf") != 0;
                }
#endif
#ifdef PS3_NATIVE_STARTUP_RESUME
                if (test_startup_resume && failures == 0) {
#ifdef PS3_NATIVE_INDIRECT_DISPATCH
                    failures += native_test_startup_indirect(&memory,
#elif defined(PS3_NATIVE_CONTEXT_MEMORY)
                    failures += native_test_startup_context(&memory,
#elif defined(PS3_NATIVE_STARTUP_CLOCK)
                    failures += native_test_startup_clock(&memory,
#elif defined(PS3_NATIVE_STARTUP_MEMORY)
                    failures += native_test_startup_memory(&memory,
#elif defined(PS3_NATIVE_STARTUP_LWMUTEX)
                    failures += native_test_startup_lwmutex(&memory,
#else
                    failures += native_test_startup_resume(&memory,
#endif
#if !defined(PS3_NATIVE_CONTEXT_MEMORY)
                        ps3_horizon_guest_space_base(space),
#endif
                        NATIVE_APPDATA_DIR "/EBOOT.elf") != 0;
                }
#endif
            } else {
#if !defined(PS3_NATIVE_ENTRY_CHECKPOINT) && !defined(PS3_NATIVE_STARTUP_RESUME)
                failures += native_test_generated_ppu(&memory,
                    ps3_horizon_guest_space_base(space)) != 0;
#endif
            }
#ifdef PS3_NATIVE_HEAP_BACKEND
            ps3_heap_guest_stats stats = ps3_heap_guest_space_stats(space);
            printf("HEAP_BACKEND committed_pages=%llu logical_committed_bytes=%llu metadata_bytes=%llu backing_bytes_including_headers=%llu backing_blocks=%llu\n",
                   (unsigned long long)stats.committed_pages,
                   (unsigned long long)(stats.committed_pages * PS3_GUEST_PAGE_BYTES),
                   (unsigned long long)stats.metadata_bytes,
                   (unsigned long long)stats.backing_bytes,
                   (unsigned long long)stats.backing_blocks);
#ifdef PS3_NATIVE_FLAT_GUEST
            if (flat_space) {
                const ps3_flat_guest_stats flat_stats = ps3_flat_guest_space_stats(flat_space);
                printf("FLAT_GUEST committed_pages=%llu slabs=%llu slab_acquires=%llu slab_releases=%llu slab_failures=%llu last_kernel_result=0x%08x last_slab_step=%u\n",
                       (unsigned long long)flat_stats.committed_pages, (unsigned long long)flat_stats.slabs,
                       (unsigned long long)flat_stats.slab_acquires, (unsigned long long)flat_stats.slab_releases,
                       (unsigned long long)flat_stats.slab_failures, flat_stats.last_kernel_result, flat_stats.last_slab_step);
                g_ps3_flat_guest_base = 0;
                g_ps3_flat_guest_space = NULL;
                printf("FLAT_GUEST cleanup_status=%d\n", ps3_flat_guest_space_destroy(&flat_space));
            }
#endif
            status = ps3_heap_guest_space_destroy(&space);
#else
            ps3_horizon_guest_stats stats = ps3_horizon_guest_space_stats(space);
            printf("%s committed_pages=%llu ownership_pages=%llu kernel_result=0x%x\n",
                   guest_label,
                   (unsigned long long)stats.committed_pages,
                   (unsigned long long)stats.ownership_pages, stats.last_kernel_result);
            status = ps3_horizon_guest_space_destroy(&space);
#endif
            printf("%s cleanup_status=%d released=%d\n", guest_label, status, space == NULL);
            failures += status != PS3_GUEST_MEMORY_OK;
        }
    }
#ifdef PS3_NATIVE_GPU_CLEAR
    if (test_gpu_clear || test_gpu_negative) failures += native_test_gpu_clear(test_gpu_negative) != 0;
#endif
#ifdef PS3_NATIVE_USER_MEMORY
    if (test_user_memory) failures += native_test_user_memory_horizon() != 0;
#endif
#ifdef PS3_NATIVE_LWMUTEX
    if (test_lwmutex) failures += native_test_lwmutex_horizon() != 0;
#endif
    printf("RESULT %s failed_suites=%d\n", failures ? "FAIL" : "PASS", failures);
    fflush(stdout);
    fsync(STDOUT_FILENO);   /* tee_fsync writes the buffered log out first (Decision 93) */
    debug_line(failures ? "INFAMOUS_NATIVE_PROBE FAIL\n" : "INFAMOUS_NATIVE_PROBE PASS\n");
    native_status_begin();   /* the renderer may still hold the window when it worked; then this shows nothing */
    native_status_row(4, "FINISHED. The log is saved. Returning to the menu in 5 seconds...");
    svcSleepThread(5000000000ll);
    return failures != 0;
}
