/*
 * rsx_render_bridge - the live game's RSX methods into the draw engine, and its flips onto the screen.
 * Root, 2026-11-12, Decision 67. Switch-only (built when the NRO is made with LLE_RSX_RENDER=1).
 *
 * The live command-buffer consumer (native/domain/rsx_live_fifo.c) hands every method and every flip to the
 * composition; with this bridge the composition passes them on to ps3recomp's rsx_draw_engine, which draws through
 * the Vulkan backend (native/adapters/rsx_vulkan) on the console's NVK driver and presents through
 * rsx_vk_present.c. Guest memory is read through the flat guest window (guest_space_flat_horizon.h): the engine
 * gets real pointers to textures, vertex buffers and programs.
 *
 * Everything here runs on ONE thread, the one that pumps the consumer. There is no shader compiler in the NRO:
 * a shader is drawn only when its SPIR-V is in <app dir>/spv-cache (filled on the laptop by replaying captures).
 *
 * Never run anywhere when written: Eden cannot run NVK draws, so the first evidence is a hardware run.
 */
#include <sys/stat.h>
#include "rsx_render_bridge.h"

#include <stdio.h>
#include <string.h>
/* <switch.h> is NOT included: libnx and ps3recomp's ps3types.h both define u128/s128. The one libnx call needed is
 * declared here (NWindow* nwindowGetDefault(void)). */
extern void* nwindowGetDefault(void);
extern void native_status_begin(void);
extern void native_status_release(void);
extern void native_status_row(int row, const char* text);

#include "guest_space_flat_horizon.h"
#include "rsx_draw_engine.h"
#include "rsx_vulkan/rsx_vk.h"
#include "rsx_vulkan/rsx_vk_present.h"

extern const rsx_draw_backend* rsx_vk_backend(void);
extern rsx_vk_device* rsx_vk_backend_device(void);

/* The engine's default guest-memory path names these; the bridge installs its own reader, so they stay empty. */
u8* vm_base = NULL;
u32 ppu_vm_size = 0;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; (void)offset; return 0; }
u32 cellGcmResolveIO(u32 offset) { (void)offset; return 0; }

static struct {
    int state;                      /* 0 not tried, 1 running, -1 failed (stays off) */
    rsx_render_bridge_config config;
    rsx_render_bridge_stats stats;
} b;

static const u8* guest_reader(void* user, u32 location, u32 offset, u32 min_bytes)
{
    uint32_t address;
    const void* pointer;
    (void)user;
    if (location == RSX_LOCATION_LOCAL) {
        if (offset >= b.config.local_size || min_bytes > b.config.local_size - offset) { b.stats.reads_refused++; return NULL; }
        address = b.config.local_base + offset;
    } else {
        /* The init mapping only (IO offset 0 = io_ea). cellGcmMapMainMemory is not served yet. */
        if (offset >= b.config.io_size || min_bytes > b.config.io_size - offset) { b.stats.reads_refused++; return NULL; }
        address = b.config.io_ea + offset;
    }
    pointer = ps3_flat_guest_space_pointer(g_ps3_flat_guest_space, address, min_bytes);
    if (!pointer) b.stats.reads_uncommitted++;
    return (const u8*)pointer;
}

static void report_sink(void* user, int severity, const char* message)
{
    (void)user;
    if (b.stats.reports++ < 200u) fprintf(stderr, "[rsx_vk report %d] %s\n", severity, message);
}

int rsx_render_bridge_start(const rsx_render_bridge_config* config)
{
    const rsx_draw_backend* backend;
    rsx_vk_device* device;
    char path[320];
    if (b.state) return b.state > 0 ? 0 : -1;
    b.state = -1;
    if (!config || !config->app_dir || !g_ps3_flat_guest_space) { printf("RSX_RENDER start refused: no flat guest space\n"); return -1; }
    b.config = *config;
    /* Root (2026-11-12): no separate render.log any more; stderr is the main log, where the backend's own lines go.
     * The status console holds the window the swapchain needs: it is released here and taken back on failure. */
    native_status_row(5, "renderer: starting Vulkan (NVK)...");
    native_status_release();
    snprintf(path, sizeof path, "%s/spv-cache", config->app_dir);
    rsx_vk_set_spv_cache_dir(path, 0);
    {   /* Decision 92: where the backend saves the source of shaders that are not in the cache yet */
        char missing[sizeof path + 16];
        snprintf(missing, sizeof missing, "%s-missing", path);
        (void)mkdir(missing, 0777);
    }
    backend = rsx_vk_backend();
    if (!backend) { printf("RSX_RENDER start failed: no Vulkan backend\n"); native_status_begin(); native_status_row(5, "renderer: OFF (no Vulkan backend). The game runs without a picture."); return -1; }
    rsx_draw_engine_set_backend(backend);
    rsx_draw_engine_set_guest_memory(guest_reader, NULL);
    if (rsx_draw_engine_init(1280u, 720u) != 0) {
        printf("RSX_RENDER start failed: rsx_draw_engine_init (the [rsx_vk] lines above say why)\n");
        rsx_draw_engine_set_backend(NULL);
        native_status_begin();
        native_status_row(5, "renderer: OFF (Vulkan did not start, see log). The game runs without a picture.");
        return -1;
    }
    device = rsx_vk_backend_device();
    if (device) rsx_vk_device_set_report(device, report_sink, NULL);
    b.stats.present_ready = device && rsx_vk_present_init(device, nwindowGetDefault()) == 0;
    if (!b.stats.present_ready) {   /* drawing without a way to show it: keep the status console instead */
        printf("RSX_RENDER swapchain not ready: frames are drawn but cannot be shown\n");
        native_status_begin();
        native_status_row(5, "renderer: Vulkan started, but the screen output did not (see log).");
    }
    b.state = 1;
    printf("RSX_RENDER started present_ready=%d io=0x%08x+0x%x local=0x%08x+0x%x cache=%s\n", (int)b.stats.present_ready,
           config->io_ea, config->io_size, config->local_base, config->local_size, path);
    fflush(stdout);
    return 0;
}

void rsx_render_bridge_display(uint32_t buffer, uint32_t offset, uint32_t pitch, uint32_t width, uint32_t height)
{
    if (b.state != 1) return;
    rsx_draw_engine_set_display_buffer(buffer, RSX_LOCATION_LOCAL, offset, pitch, width, height);
}

/* Root (2026-11-12, Decision 96): time spent inside the renderer, in counter ticks (the caller converts). Tells
 * "the game is slow to produce a frame" from "we are slow to draw it". */
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
void rsx_render_bridge_times(uint64_t* method_ticks, uint64_t* flip_ticks)
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
    b.stats.presented = rsx_vk_present_count();
}

void rsx_render_bridge_stop(void)
{
    rsx_vk_device* device;
    if (b.state != 1) return;
    device = rsx_vk_backend_device();
    if (device) rsx_vk_present_shutdown(device);
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
    b.state = -1;
}

rsx_render_bridge_stats rsx_render_bridge_get_stats(void)
{
    rsx_render_bridge_stats stats = b.stats;
    stats.state = b.state;
    return stats;
}
