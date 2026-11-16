/*
 * sibling_stubs_a64 - the runtime symbols the inFAMOUS 2 port's video library (the C files of engine/libs/video) expect
 * from the rest of ITS runtime, as no-ops, for the aarch64/Horizon NRO.
 *
 * Root experiment 2026-11-13 (sibling video library on Switch, step 2). Derived from
 * evidence/sibling-video-replay-20261113/sibling_stubs.c (the Linux replay tool's stubs, which also compile for aarch64:
 * checked in build/sibling-video-a64-092802/extra), minus vm_write32: the NRO already defines it
 * (the composition's guest memory), and a second definition would collide.
 * Nothing here is game code and nothing is copied from the sibling: every function is a no-op or a zero, written from the
 * call sites in the sibling's sources.
 * Guest memory (vm_base, ppu_vm_size, cellGcmResolve*) is NOT here: rsx_render_bridge_sibling.c owns it.
 *
 * Grouped by who asks for them:
 *   overlay      vk_overlay.cpp (Dear ImGui HUD and menu): not built here; PS3_OVERLAY=0
 *   pad          cellPad.c window events and mouse capture: the Switch screen has no window events (hz_window.c's
 *                SDL_PollEvent answers 0), so nothing ever calls them
 *   gcm / clocks cellGcmSys.c pieces the vtable path (rsx_commands.c) and the engine name
 *   diagnostics  play log, hitch catcher, limits, achievements
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include "vk_overlay.h"        /* sibling header: prototypes of the overlay entry points */

/* ---- vk_overlay.cpp ----------------------------------------------------------------------------- */
volatile int g_vk_overlay_active;
volatile int g_vk_overlay_on_worker;
void vk_overlay_attach(const vk_overlay_attach_info* info) { (void)info; }
void vk_overlay_swapchain_changed(VkFormat format, int color_ok) { (void)format; (void)color_ok; }
int  vk_overlay_draw(VkCommandBuffer cmd, uint32_t index, VkImage image, VkExtent2D extent,
                     VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access)
{ (void)cmd; (void)index; (void)image; (void)extent; (void)layout; (void)stage; (void)access; return 0; }
void vk_overlay_poll(void) {}
int  vk_overlay_event(const void* sdl_event) { (void)sdl_event; return 0; }
void vk_overlay_notice(const char* text) { (void)text; }
int  vk_overlay_open_menu(void) { return 0; }
int  vk_overlay_menu_open(void) { return 0; }
void vk_overlay_shutdown(void) {}

/* ---- cellPad host hooks (window events, mouse capture) ------------------------------------------ */
volatile uint32_t g_pad_host_keys[4];
volatile uint32_t g_pad_mouse_buttons;
volatile int32_t  g_pad_mouse_dx, g_pad_mouse_dy, g_pad_mouse_wheel;
int  cellPad_host_window_event(int ev) { (void)ev; return 0; }
void cellPad_host_window_focus(int focused) { (void)focused; }
int  cellPad_host_capture_wanted_now(void) { return 0; }
void cellPad_host_set_mouse_captured(int on) { (void)on; }
int  cellPad_host_overlay_open(void) { return 0; }
int  ps3_pad_kbm_on(void) { return 0; }
int  ps3_pad_kbm_test(void) { return 0; }

/* ---- cellGcmSys.c / vm.h pieces the vtable path (rsx_commands.c) and the engine name ------------- */
uint32_t ppu_hle_inject_base = 0x00F00000u;        /* only used to place semaphore labels (vtable path) */
volatile uint32_t g_gcm_xlate_gen;                  /* bumped when the IO map changes: never, here */
void rsx_snap_event(int which, uint32_t offset) { (void)which; (void)offset; }
void cellGcm_hattr_take(unsigned long long* drain_ns, unsigned long long* sema_ns, unsigned long long* gaveup)
{ if (drain_ns) *drain_ns = 0; if (sema_ns) *sema_ns = 0; if (gaveup) *gaveup = 0; }
void spu_edge_vp_set(float sx, float s) { (void)sx; (void)s; }   /* SPU Edge vertex culling: no SPUs here */

/* ---- runtime clocks ----------------------------------------------------------------------------- */
unsigned long long ps3_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
void ps3_ms(const char* key) { (void)key; }

/* ---- diagnostics -------------------------------------------------------------------------------- */
void ps3_hc_session(const char* playlog_path, double t0) { (void)playlog_path; (void)t0; }
void ps3_hc_frame(double now) { (void)now; }
void ps3_rl_ach_frame(void) {}
int  ps3_limits_format(char* buf, int len, int size) { (void)buf; (void)size; return len; }
int  ps3_limits_take_frame(char* buf, int len, int size) { (void)buf; (void)size; return len; }
int  ps3_limits_pending(void) { return 0; }
