/*
 * rsx_render_bridge_sibling - the live game's RSX methods into the inFAMOUS 2 port's draw engine and Vulkan backend
 * (native/sibling_video, vendored from infamous2-switch-spike), and its flips onto the screen.
 * Root experiment 2026-11-13. Switch-only; built instead of rsx_render_bridge.c by
 * evidence/sibling-video-switch-20261113/sibling.mk (LLE_SIBLING_RENDER=1). Same public API as rsx_render_bridge.h and
 * the same exported symbols as rsx_render_bridge.c (including rsx_render_bridge_times, which the composition reads
 * through a weak declaration), so native/tests/test_startup_indirect.cpp is unchanged.
 *
 * What differs from rsx_render_bridge.c (the bridge for OUR draw engine + native/adapters/rsx_vulkan backend):
 *   - the engine and the backend are the sibling's. rsx_vulkan_backend_init() loads Vulkan (through hz_misc.c's
 *     dlopen/dlsym shim, which hands out vk_icdGetInstanceProcAddr of the linked NXVK driver), creates the device, registers
 *     the engine backend, calls rsx_draw_engine_set_default() and rsx_draw_engine_init(); so the bridge no longer calls
 *     set_backend/init itself;
 *   - the screen: PS3RECOMP_VK_WINDOW=1 makes the backend open a "window" (SDL_CreateWindow, answered by hz_window.c:
 *     the default libnx NWindow, 1280x720) and a swapchain on VK_NN_vi_surface; hz_screen_handover() (glue) releases
 *     the status console at that moment and takes it back when the window is dropped. rsx_vk_present_* is not used;
 *   - shaders: the guest's vertex/fragment programs are translated HLSL -> SPIR-V on the console (glslang, ~3 MB of text),
 *     cached under <app dir>/shader_cache (PS3RECOMP_VK_SHADER_CACHE). There is no spv-cache / spv-cache-missing here;
 *   - everything the backend reads from the environment is set below (defaults, then <app dir>/sibling-env.txt);
 *   - log lines: the backend and the engine print to stderr (the backend through its own writer thread); stderr is the
 *     log device in the LLE profile (switch_main.c: devoptab STD_ERR = the tee), so they land in the same log as stdout.
 *
 * Everything here runs on ONE thread, the one that pumps the consumer (the audio ticker), as before.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rsx_draw_engine.h"     /* the sibling's (engine/libs/video), found first by the include path order */
#include "rsx_render_bridge.h"   /* ours (native/adapters) */

extern void native_status_begin(void);
extern void native_status_row(int row, const char *text);

/* sibling entry points without a public header declaration we need */
extern int rsx_vulkan_backend_init(u32 width, u32 height, const char *title);
extern void rsx_vulkan_backend_shutdown(void);
extern int rsx_draw_engine_ready(void);
/* glue/sdl_horizon.c */
extern volatile int g_sibling_surface_state;
/* guest_space_flat_horizon.h, declared here because that header pulls in the SDK's guest_space.h (type clashes with the
 * sibling's ps3types.h) */
typedef struct ps3_flat_guest_space ps3_flat_guest_space;
extern ps3_flat_guest_space *g_ps3_flat_guest_space;
extern const void *ps3_flat_guest_space_pointer(ps3_flat_guest_space *space, uint32_t address, uint64_t length);
/* Root (2026-11-15, renderer CPU work): the same without the space's mutex, any length (guest_space_flat_horizon.c). Weak so a link
 * without that file keeps the locked pointer above. */
extern const void *ps3_flat_guest_fast_range(uint32_t address, uint64_t length) __attribute__((weak));
/* The RSX consumer's switches (startup_execution.cpp, defined in LLE builds), published from the environment below. Weak for the same reason. */
extern volatile int g_ps3_rsx_fifo_direct __attribute__((weak));
extern volatile uint32_t g_ps3_rsx_get_batch __attribute__((weak));
extern volatile uint32_t g_ps3_rsx_pump_chunk __attribute__((weak));
/* PS3RECOMP_BRIDGE_FAST_READER (default 1): guest_reader asks for its pointers without taking the flat space's mutex. 0 = the locked call. */
static int g_fast_reader = 1;

/* The engine's default guest-memory path names these; the bridge installs its own reader, so they stay empty
 * (same as rsx_render_bridge.c). */
u8 *vm_base = NULL;
u32 ppu_vm_size = 0;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; (void)offset; return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; return 0; }

static struct {
    int state;                      /* 0 not tried, 1 running, -1 failed (stays off) */
    rsx_render_bridge_config config;
    rsx_render_bridge_stats stats;
} b;

static const u8 *guest_reader(void *user, u32 location, u32 offset, u32 min_bytes)
{
    uint32_t address;
    const void *pointer;
    (void)user;
    if (location == RSX_LOCATION_LOCAL) {
        if (offset >= b.config.local_size || min_bytes > b.config.local_size - offset) { b.stats.reads_refused++; return NULL; }
        address = b.config.local_base + offset;
    } else {
        /* The init mapping only (IO offset 0 = io_ea). cellGcmMapMainMemory is not served yet. */
        if (offset >= b.config.io_size || min_bytes > b.config.io_size - offset) { b.stats.reads_refused++; return NULL; }
        address = b.config.io_ea + offset;
    }
    /* Root (2026-11-15): the engine asks here for every vertex slot, index run, texture, program and constant block of every draw
     * (tens of thousands of calls a frame); the locked call took the flat space's mutex each time, the one the guarded memory path of
     * the whole guest shares. The renderer reads memory the game handed to the RSX and does not unmap while it is in use, which is
     * all the lock-free bitmap read relies on. */
    pointer = g_fast_reader && ps3_flat_guest_fast_range ? ps3_flat_guest_fast_range(address, min_bytes)
                                                          : ps3_flat_guest_space_pointer(g_ps3_flat_guest_space, address, min_bytes);
    if (!pointer) b.stats.reads_uncommitted++;
    return (const u8 *)pointer;
}

/* The backend reads these when it initialises (and some lazily). The first block follows the sibling's own console
 * defaults (horizon/src/hz_entry.c default_environment, "console runs" 4 and 5 in its comments); the second is bring-up.
 * Dropped from that list on purpose: PS3_VFS_ROOT/HDD0/DEV_FLASH/GAMEDATA/TITLE and GCM_* (the sibling's runtime, not the
 * video library); PS3RECOMP_ENG_GPU_LIGHT / _GPU_SSAO (buffer addresses of inFAMOUS 2); PS3RECOMP_PLAYLOG; AUDIO_PEAK;
 * PS3RECOMP_FPS_CAP=30 (its sleep would run on the audio ticker thread). */
static void set_environment(const char *app_dir, char *cache_dir, size_t cache_size)
{
    static const char *const defaults[][2] = {
        /* Root (2026-11-13, run rv36s: "[RSX vulkan] no Vulkan physical devices"): NVK lists the Tegra X1 GPU only with
         * this set, before vkCreateInstance (rsx_vk_platform.c:169 does the same for our own backend). */
        {"NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1"},
        {"PS3_VERBOSE", "0"}, {"PS3_OVERLAY", "0"}, {"HEADLESS", "1"},
        {"PS3RECOMP_RENDER_SCALE", "1.0"}, {"PS3RECOMP_AA", "off"}, {"PS3RECOMP_SHARPEN", "0"},
        {"PS3RECOMP_SOFT_SHADOWS", "0"}, {"PS3RECOMP_SSAO", "0"},
        /* Root (2026-11-15, renderer CPU work): frames in flight 2 (was 1, "from bring-up (memory)" in the sibling's own notes). With 1 the present
         * submitted the frame and waited for the GPU (30-32 ms busy a frame in rv58s; flips_ms 53 ms a frame); with 2 the present submits and
         * returns and the next frame records into the other slot. The sibling runs it on the same NVK driver since its run 26 (the RSX
         * thread's GPU wait fell from 52 % to 9 %). Costs a second set of the per-frame arenas (the rv58s "[limits] at exit" line: vertex 34 of 64
         * MB, index 9 of 16, uniform 30 of 64, staging 45 of 64 in use, so +118 MB in use and up to +208 MB at the caps).
         * PS3RECOMP_VK_FIF=1 in sibling-env.txt is the old behaviour. */
        {"PS3RECOMP_VK_FIF", "2"},
        /* texture cache: 512 entries evicted live textures thousands of times (sibling console run 4) */
        /* Root (2026-11-13, Decision 109, run rv39s): ARENA_GROW was "0" (the sibling's console value). This game's
         * frame has about 10,200 draws and the log shows "index arena full mid-frame" (cap 8 MB): with growth off
         * every frame pays a submit-and-wait when an arena fills (rsx_vulkan_backend.c:1384). Growth on: an arena that
         * ran out doubles after that flush; a refused growth keeps the old size (same file, :1433). */
        {"PS3RECOMP_VK_ARENA_GROW", "1"}, {"PS3RECOMP_ENG_TEXCACHE", "2048"},
        /* the picture: the backend's "window" is the Switch screen (hz_window.c). "immediate": with "fifo"
         * vkQueuePresentKHR took 20-110 ms a frame on the sibling's console (run 5) */
        {"PS3RECOMP_VK_WINDOW", "1"}, {"PS3RECOMP_VK_PRESENT", "immediate"},
        /* the renderer runs the guest's own programs through glslang; that also makes the draw engine the default path */
        {"PS3RECOMP_VK_GUEST_PROGRAMS", "1"}, {"PS3RECOMP_RSX_ENGINE", "dispatch"},
        /* bring-up: pipelines and their translations are built on the calling thread, in order, and a draw never waits for
         * a worker (the replay on the laptop runs the same way) */
        /* Root (Decision 109): measurement for the renderer half of the frame (0.42 s per frame in run rv39s).
         * BATCH_STATS: every 400 flushes, which source line forced a submit-and-wait and how many draws each held.
         * GPU_PROF: GPU timestamps per pass kind and target size (turns itself off if the driver has no timestamp
         * queries). Both print through stderr into the log. */
        /* Decision 130: a lean build (-DPS3_LEAN_BUILD, make LLE_LEAN=1) defaults both off; sibling-env.txt can still turn them on. */
#ifdef PS3_LEAN_BUILD
        {"PS3RECOMP_VK_BATCH_STATS", "0"}, {"PS3RECOMP_VK_GPU_PROF", "0"},
#else
        {"PS3RECOMP_VK_BATCH_STATS", "400"}, {"PS3RECOMP_VK_GPU_PROF", "1"},
#endif
        {"PS3RECOMP_VK_PIPE_ASYNC", "0"}, {"PS3RECOMP_VK_PIPE_PREWARM", "0"}, {"PS3RECOMP_VK_PIPE_RECIPES", "0"},
        /* Root (2026-11-15, renderer CPU work): the RSX consumer and the guest reader (startup_execution.cpp, guest_space_flat_horizon.c).
         * Read again after sibling-env.txt below, so each can be put back to the old path (0) there. */
        {"PS3RECOMP_RSX_FIFO_DIRECT", "1"}, {"PS3RECOMP_RSX_GET_BATCH", "256"}, {"PS3RECOMP_RSX_PUMP_CHUNK", "4096"},
        {"PS3RECOMP_BRIDGE_FAST_READER", "1"},
    };
    char line[512];
    size_t index;
    unsigned from_file = 0;
    FILE *file;
    for (index = 0; index < sizeof defaults / sizeof defaults[0]; ++index) setenv(defaults[index][0], defaults[index][1], 1);
    snprintf(cache_dir, cache_size, "%s/shader_cache", app_dir);
    (void)mkdir(cache_dir, 0777);   /* the backend creates missing parents itself; this makes the line in the log true */
    setenv("PS3RECOMP_VK_SHADER_CACHE", cache_dir, 1);
    /* <app dir>/sibling-env.txt: KEY=VALUE per line, # comments; overrides the above (as the sibling's env.txt does) */
    snprintf(line, sizeof line, "%s/sibling-env.txt", app_dir);
    file = fopen(line, "r");
    if (file) {
        while (fgets(line, sizeof line, file)) {
            char *equals, *end = line + strlen(line);
            while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
            if (!line[0] || line[0] == '#' || !(equals = strchr(line, '='))) continue;
            *equals = 0;
            if (setenv(line, equals + 1, 1) == 0) ++from_file;
        }
        fclose(file);
    }
    /* PS3RECOMP_VK_GPU_PROF: the backend treats ANY value, "0" included, as on (getenv() != NULL). "0" or "off" in sibling-env.txt means off here. */
    { const char *prof = getenv("PS3RECOMP_VK_GPU_PROF"); if (prof && (!strcmp(prof, "0") || !strcmp(prof, "off") || !prof[0])) unsetenv("PS3RECOMP_VK_GPU_PROF"); }
    /* the consumer's and the reader's switches (a number; anything else keeps the default) */
    {
        const char *text;
        unsigned long value;
        char *end;
        if ((text = getenv("PS3RECOMP_RSX_FIFO_DIRECT")) && (value = strtoul(text, &end, 0), end != text) && &g_ps3_rsx_fifo_direct) g_ps3_rsx_fifo_direct = value != 0;
        if ((text = getenv("PS3RECOMP_RSX_GET_BATCH")) && (value = strtoul(text, &end, 0), end != text) && &g_ps3_rsx_get_batch) g_ps3_rsx_get_batch = (uint32_t)value;
        if ((text = getenv("PS3RECOMP_RSX_PUMP_CHUNK")) && (value = strtoul(text, &end, 0), end != text) && &g_ps3_rsx_pump_chunk) g_ps3_rsx_pump_chunk = (uint32_t)value;
        if ((text = getenv("PS3RECOMP_BRIDGE_FAST_READER")) && (value = strtoul(text, &end, 0), end != text)) g_fast_reader = value != 0;
    }
    printf("RSX_RENDER sibling environment: %u defaults, %u from sibling-env.txt, shader cache %s\n",
           (unsigned)(sizeof defaults / sizeof defaults[0]), from_file, cache_dir);
    printf("RSX_RENDER switches: fif=%s gpu_prof=%s fifo_direct=%d get_batch=%u pump_chunk=%u fast_reader=%d\n", getenv("PS3RECOMP_VK_FIF"),
           getenv("PS3RECOMP_VK_GPU_PROF") ? "on" : "off", &g_ps3_rsx_fifo_direct ? (int)g_ps3_rsx_fifo_direct : -1,
           &g_ps3_rsx_get_batch ? (unsigned)g_ps3_rsx_get_batch : 0u, &g_ps3_rsx_pump_chunk ? (unsigned)g_ps3_rsx_pump_chunk : 0u, g_fast_reader);
}

int rsx_render_bridge_start(const rsx_render_bridge_config *config)
{
    char cache_dir[320];
    if (b.state) return b.state > 0 ? 0 : -1;
    b.state = -1;
    if (!config || !config->app_dir || !g_ps3_flat_guest_space) { printf("RSX_RENDER start refused: no flat guest space\n"); return -1; }
    b.config = *config;
    set_environment(config->app_dir, cache_dir, sizeof cache_dir);
    rsx_draw_engine_set_guest_memory(guest_reader, NULL);
    /* The status console holds the screen the swapchain needs: hz_screen_handover(1), called by the backend when it opens
     * its window, releases it (and hz_screen_handover(2) gives it back when the window is dropped). */
    native_status_row(5, "renderer: starting Vulkan (sibling backend, NVK)...");
    if (rsx_vulkan_backend_init(1280u, 720u, "inFAMOUS") != 0) {
        printf("RSX_RENDER start failed: rsx_vulkan_backend_init (the [RSX vulkan] lines above say why)\n");
        rsx_draw_engine_set_guest_memory(NULL, NULL);
        native_status_begin();   /* a no-op when the screen was never released */
        native_status_row(5, "renderer: OFF (Vulkan did not start, see log). The game runs without a picture.");
        return -1;
    }
    if (!rsx_draw_engine_ready()) {   /* the backend fell back to the fixed-function vtable path: it cannot draw this game */
        printf("RSX_RENDER start failed: the register-file draw engine is not active (no guest programs: glslang or PS3RECOMP_VK_GUEST_PROGRAMS)\n");
        rsx_vulkan_backend_shutdown();
        rsx_draw_engine_set_guest_memory(NULL, NULL);
        native_status_begin();
        native_status_row(5, "renderer: OFF (draw engine not active, see log). The game runs without a picture.");
        return -1;
    }
    b.stats.present_ready = g_sibling_surface_state > 0;
    if (!b.stats.present_ready) {   /* drawing offscreen without a way to show it: keep the status console */
        printf("RSX_RENDER swapchain not ready: frames are drawn but cannot be shown\n");
        native_status_begin();
        native_status_row(5, "renderer: Vulkan started, but the screen output did not (see log).");
    }
    b.state = 1;
    printf("RSX_RENDER started present_ready=%d io=0x%08x+0x%x local=0x%08x+0x%x cache=%s backend=sibling\n", (int)b.stats.present_ready,
           config->io_ea, config->io_size, config->local_base, config->local_size, cache_dir);
    fflush(stdout);
    return 0;
}

void rsx_render_bridge_display(uint32_t buffer, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height)
{
    if (b.state != 1) return;
    rsx_draw_engine_set_display_buffer(buffer, RSX_LOCATION_LOCAL, offset, pitch, width, height);
}

/* Time spent inside the renderer, in counter ticks (the caller converts). Tells "the game is slow to produce a frame"
 * from "we are slow to draw it". */
static uint64_t g_bridge_method_ticks, g_bridge_flip_ticks;
static inline uint64_t bridge_ticks_now(void)
{
#ifdef __aarch64__
    /* cntpct_el0, as libnx armGetSystemTick reads it. cntvct_el0 is not readable from a Horizon program: build
     * rv29 used it and died on its first renderer call, three launches in a row. */
    uint64_t value; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value)); return value;
#else
    return 0;
#endif
}
void rsx_render_bridge_times(uint64_t *method_ticks, uint64_t *flip_ticks)
{
    if (method_ticks) *method_ticks = g_bridge_method_ticks;
    if (flip_ticks) *flip_ticks = g_bridge_flip_ticks;
}

void rsx_render_bridge_method(uint32_t subchannel, uint32_t method, uint32_t data)
{
    uint64_t before;
    if (b.state != 1 || subchannel != 0u) return;   /* NV4097 is subchannel 0 in this game */
    before = bridge_ticks_now();
    rsx_draw_engine_method(method, data);
    g_bridge_method_ticks += bridge_ticks_now() - before;
    b.stats.methods++;
}

void rsx_render_bridge_flip(uint32_t buffer)
{
    uint64_t before;
    if (b.state != 1) return;
    before = bridge_ticks_now();
    rsx_draw_engine_present_buffer(buffer);
    g_bridge_flip_ticks += bridge_ticks_now() - before;
    b.stats.flips++;
    /* the backend has no public count of frames it put on the screen: flips handed to a ready swapchain */
    if (b.stats.present_ready) b.stats.presented = b.stats.flips;
}

void rsx_render_bridge_stop(void)
{
    if (b.state != 1) return;
    rsx_vulkan_backend_shutdown();   /* also shuts the engine down, unregisters the backend and drops the window */
    b.state = -1;
}

rsx_render_bridge_stats rsx_render_bridge_get_stats(void)
{
    rsx_render_bridge_stats stats = b.stats;
    stats.state = b.state;
    return stats;
}
