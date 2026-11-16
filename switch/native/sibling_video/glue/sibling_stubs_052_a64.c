/*
 * sibling_stubs_052_a64 - what the v0.5.2 video library needs from the rest of its runtime that v0.5.0's did not,
 * as no-ops, for the aarch64/Horizon NRO. Compiled with the other glue (sibling.mk SIB_GLUE); the vendored sources
 * that call it are rsx_vulkan_backend.c (this list is from the cross nm of the 22 objects, evidence/sibling-video-052-20261113/).
 *
 * Root 2026-11-13 (v0.5.0 -> v0.5.2 move). One symbol:
 *
 *   vk_overlay_lowend_auto   vk_init_all (rsx_vulkan_backend.c): "a card with little video memory, or integrated graphics,
 *                            starts with the menu's Low-end PC values for the settings the player has not chosen"; it lives
 *                            in vk_overlay.cpp (Dear ImGui menu), which is not built here (PS3_OVERLAY=0, glue/sibling_stubs_a64.c
 *                            has the other overlay entry points). Called when the backend runs windowed -- on the console it
 *                            does (PS3RECOMP_VK_WINDOW=1) -- with the GPU's memory (NVK reports the shared system memory),
 *                            integrated = 1 on the Tegra X1 (VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU).
 *                            0 = it changed no setting: the settings stay what rsx_render_bridge_sibling.c and sibling-env.txt
 *                            say. Do NOT make this apply the low-end set: it would override the bridge's defaults.
 */
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include "vk_overlay.h"        /* the library's own prototype (so a changed signature fails here) */

int vk_overlay_lowend_auto(unsigned vram_mb, int integrated, const char* gpu_name)
{
    (void)vram_mb; (void)integrated; (void)gpu_name;
    return 0;
}
