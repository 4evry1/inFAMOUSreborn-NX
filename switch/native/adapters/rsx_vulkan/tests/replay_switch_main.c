/*
 * replay_switch_main - the Switch front end of replay_capture.c (root, 2026-11-12).
 *
 * Replays one captured RSX frame through ps3recomp's draw engine and the Vulkan backend on the console's own
 * driver (NXVK / NVK, linked statically), with every shader taken from the on-disk SPIR-V cache: this build has
 * no shader compiler. The picture is written to the SD card and then shown on the screen for about ten seconds
 * through the backend's own swapchain present (rsx_vk_present.c); if that is not available, through libnx's
 * software framebuffer after the device is gone.
 *
 * Files, all under sdmc:/switch/infamous-replay/ :
 *   capture/methods.bin, capture/memory.bin, capture/state.txt   the capture (from an emulator run)
 *   spv-cache/<vs|ps>-<hash>.spv                                 shaders translated on the laptop
 *   replay.log, target-<n>.ppm                                   written by this program
 *
 * This is a renderer test, not the game. Needs the full-memory homebrew menu (the guest image is 383 MiB).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

u32    __nx_applet_type = AppletType_Application;
size_t __nx_heap_size   = 0;

int replay_run(const char* capture_dir, const char* output_dir, unsigned char** keep_pixels);

/* The backend's present module (rsx_vk_present.c). Declared here by hand: this file sees libnx's headers only. */
struct rsx_vk_device;
int rsx_vk_present_init(struct rsx_vk_device* d, void* native_window);
int rsx_vk_present_surface(struct rsx_vk_device* d, unsigned surface);
void rsx_vk_present_shutdown(struct rsx_vk_device* d);
extern void (*g_replay_present_hook)(struct rsx_vk_device* device, unsigned target);
static int g_presented_frames = -1;   /* -1: the swapchain path was not available */

/* Present the finished target through the Vulkan swapchain for about 10 s (PLUS ends it early). */
static void present_hook(struct rsx_vk_device* device, unsigned target)
{
    PadState pad;
    int frame, failures = 0;
    if (rsx_vk_present_init(device, nwindowGetDefault()) != 0) { printf("PRESENT init failed: falling back to the software framebuffer\n"); return; }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    g_presented_frames = 0;
    for (frame = 0; frame < 60 * 10 && appletMainLoop(); frame++) {
        if (rsx_vk_present_surface(device, target) == 0) g_presented_frames++;
        else if (++failures > 30) break;
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    }
    printf("PRESENT swapchain frames=%d failures=%d\n", g_presented_frames, failures);
    rsx_vk_present_shutdown(device);
}
void rsx_vk_set_spv_cache_dir(const char* dir, int write);

#define REPLAY_ROOT "sdmc:/switch/infamous-replay"

static void show(const unsigned char* pixels, int frames)
{
    Framebuffer fb;
    PadState pad;
    NWindow* window = nwindowGetDefault();
    int frame;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    if (R_FAILED(framebufferCreate(&fb, window, 1280, 720, PIXEL_FORMAT_RGBA_8888, 2))) { printf("SHOW framebufferCreate failed\n"); return; }
    if (R_FAILED(framebufferMakeLinear(&fb))) { printf("SHOW framebufferMakeLinear failed\n"); framebufferClose(&fb); return; }
    printf("SHOW begin frames=%d\n", frames);
    for (frame = 0; frame < frames && appletMainLoop(); frame++) {
        u32 stride = 0;
        u8* out = (u8*)framebufferBegin(&fb, &stride);
        int row;
        if (!out) break;
        for (row = 0; row < 720; row++) memcpy(out + (size_t)row * stride, pixels + (size_t)row * 1280u * 4u, 1280u * 4u);
        framebufferEnd(&fb);
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    }
    printf("SHOW end frame=%d\n", frame);
    framebufferClose(&fb);
}

int main(void)
{
    unsigned char* pixels = NULL;
    int rc;
    FILE* log = freopen(REPLAY_ROOT "/replay.log", "w", stdout);
    if (log) { setvbuf(stdout, NULL, _IONBF, 0); }
    if (freopen(REPLAY_ROOT "/replay-driver.log", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    printf("REPLAY_SWITCH version=%s capture=%s\n", REPLAY_VERSION, REPLAY_ROOT "/capture");
    rsx_vk_set_spv_cache_dir(REPLAY_ROOT "/spv-cache", 0);
    g_replay_present_hook = present_hook;
    rc = replay_run(REPLAY_ROOT "/capture", REPLAY_ROOT, &pixels);
    printf("REPLAY_SWITCH replay rc=%d pixels=%d\n", rc, pixels != NULL);
    if (pixels) {
        size_t lit = 0, i;
        for (i = 0; i < 1280u * 720u; i++) if (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) lit++;
        for (i = 0; i < 1280u * 720u; i++) pixels[i * 4 + 3] = 0xFF;   /* opaque on the screen */
        printf("REPLAY_SWITCH lit_pixels=%zu (laptop reference for capture r36: 3129)\n", lit);
        if (g_presented_frames <= 0) show(pixels, 60 * 15);   /* only when the swapchain showed nothing */
        free(pixels);
    }
    printf("REPLAY_SWITCH done\n");
    fflush(stdout); fflush(stderr);
    return 0;
}
