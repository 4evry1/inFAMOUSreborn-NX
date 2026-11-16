/*
 * ps3recomp - Vulkan backend: presenting a colour target on the Switch screen (root, 2026-11-12, Decision 66)
 *
 * The backend renders offscreen. This module owns the only window-system objects: a VI surface on the default
 * window, a FIFO swapchain, and one command buffer that blits a colour target into the acquired image. The
 * sequence is the one the nvk_vi_swapchain smoke program showed working on the user's console
 * (third_party/nxvk-c3abea1/switch/smoke/nvk_vi_swapchain.c): VK_KHR_surface + VK_NN_vi_surface on the instance,
 * VK_KHR_swapchain on the device, acquire -> record -> submit -> present.
 *
 * It is synchronous on purpose: present() first submits and waits for everything the backend has recorded, so the
 * blit reads a finished picture and the layout tracker is exact; then it submits its own buffer and waits for it.
 * That costs frame time and is the first thing to change when speed matters.
 *
 * Desktop builds have no window: rsx_vk_present_init answers "unavailable" and present() does nothing.
 * Never run on hardware when written.
 */
#include <stdio.h>
#include <string.h>

#if defined(__SWITCH__)
#  define VK_USE_PLATFORM_VI_NN 1
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include "rsx_vk.h"
#include "rsx_vk_present.h"

#if defined(__SWITCH__)

static struct {
    int ready;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkImage images[8];
    uint32_t image_count;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, rendered;
    VkFence fence;
    uint64_t presented, failed;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
} p;

#define INST(d, name) ((PFN_vk##name)(uintptr_t)rsx_vk_platform_instance_proc(&(d)->procs, (d)->instance, "vk" #name))
#define DEV(d, name)  ((PFN_vk##name)(uintptr_t)rsx_vk_platform_device_proc(&(d)->procs, (d)->device, "vk" #name))

int rsx_vk_present_init(rsx_vk_device* d, void* native_window)
{
    PFN_vkCreateViSurfaceNN CreateViSurfaceNN;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetCaps;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetFormats;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetSupport;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkCreateFence CreateFence;
    VkViSurfaceCreateInfoNN surface_info;
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[16], chosen;
    VkSwapchainCreateInfoKHR swapchain_info;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo allocate_info;
    VkSemaphoreCreateInfo semaphore_info;
    VkFenceCreateInfo fence_info;
    VkBool32 supported = VK_FALSE;
    uint32_t format_count = 0, i;
    VkResult result;

    if (p.ready) return 0;
    if (!d || !d->device || !native_window) return -1;
    memset(&p, 0, sizeof p);
    CreateViSurfaceNN = INST(d, CreateViSurfaceNN);
    GetCaps = INST(d, GetPhysicalDeviceSurfaceCapabilitiesKHR);
    GetFormats = INST(d, GetPhysicalDeviceSurfaceFormatsKHR);
    GetSupport = INST(d, GetPhysicalDeviceSurfaceSupportKHR);
    p.DestroySurfaceKHR = INST(d, DestroySurfaceKHR);
    CreateSwapchainKHR = DEV(d, CreateSwapchainKHR);
    GetSwapchainImagesKHR = DEV(d, GetSwapchainImagesKHR);
    p.DestroySwapchainKHR = DEV(d, DestroySwapchainKHR);
    p.AcquireNextImageKHR = DEV(d, AcquireNextImageKHR);
    p.QueuePresentKHR = DEV(d, QueuePresentKHR);
    CreateCommandPool = DEV(d, CreateCommandPool);
    p.DestroyCommandPool = DEV(d, DestroyCommandPool);
    AllocateCommandBuffers = DEV(d, AllocateCommandBuffers);
    p.BeginCommandBuffer = DEV(d, BeginCommandBuffer);
    p.EndCommandBuffer = DEV(d, EndCommandBuffer);
    p.ResetCommandBuffer = DEV(d, ResetCommandBuffer);
    p.CmdPipelineBarrier = DEV(d, CmdPipelineBarrier);
    p.CmdBlitImage = DEV(d, CmdBlitImage);
    p.QueueSubmit = DEV(d, QueueSubmit);
    p.WaitForFences = DEV(d, WaitForFences);
    p.ResetFences = DEV(d, ResetFences);
    CreateSemaphore = DEV(d, CreateSemaphore);
    p.DestroySemaphore = DEV(d, DestroySemaphore);
    CreateFence = DEV(d, CreateFence);
    p.DestroyFence = DEV(d, DestroyFence);
    p.DeviceWaitIdle = DEV(d, DeviceWaitIdle);
    if (!CreateViSurfaceNN || !GetCaps || !GetFormats || !CreateSwapchainKHR || !GetSwapchainImagesKHR ||
        !p.AcquireNextImageKHR || !p.QueuePresentKHR || !CreateCommandPool || !AllocateCommandBuffers ||
        !p.BeginCommandBuffer || !p.EndCommandBuffer || !p.ResetCommandBuffer || !p.CmdPipelineBarrier ||
        !p.CmdBlitImage || !p.QueueSubmit || !p.WaitForFences || !p.ResetFences || !CreateSemaphore || !CreateFence) {
        rsx_vk_log(d, "present: a window-system entry point is missing (surface %d swapchain %d): were the "
                   "extensions enabled?", CreateViSurfaceNN != NULL, CreateSwapchainKHR != NULL);
        return -1;
    }

    memset(&surface_info, 0, sizeof surface_info);
    surface_info.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    surface_info.window = native_window;
    result = CreateViSurfaceNN((VkInstance)(uintptr_t)d->instance, &surface_info, NULL, &p.surface);
    rsx_vk_log(d, "present: vkCreateViSurfaceNN -> %d", (int)result);
    if (result != VK_SUCCESS) return -1;
    if (GetSupport) GetSupport((VkPhysicalDevice)(uintptr_t)d->physical, d->queue_family, p.surface, &supported);
    if (GetCaps((VkPhysicalDevice)(uintptr_t)d->physical, p.surface, &caps) != VK_SUCCESS) {
        rsx_vk_log(d, "present: surface capabilities failed");
        return -1;
    }
    p.extent = caps.currentExtent;
    if (p.extent.width == 0xFFFFFFFFu) { p.extent.width = 1280; p.extent.height = 720; }
    GetFormats((VkPhysicalDevice)(uintptr_t)d->physical, p.surface, &format_count, NULL);
    if (format_count > 16) format_count = 16;
    if (!format_count || GetFormats((VkPhysicalDevice)(uintptr_t)d->physical, p.surface, &format_count, formats) < 0) {
        rsx_vk_log(d, "present: no surface format");
        return -1;
    }
    chosen = formats[0];
    for (i = 0; i < format_count; i++)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = formats[i]; break; }
    p.format = chosen.format;
    rsx_vk_log(d, "present: surface %ux%u format %d colour space %d (%u formats), queue family %u support %d, min images %u",
               p.extent.width, p.extent.height, (int)chosen.format, (int)chosen.colorSpace, format_count,
               d->queue_family, (int)supported, caps.minImageCount);

    memset(&swapchain_info, 0, sizeof swapchain_info);
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = p.surface;
    swapchain_info.minImageCount = caps.minImageCount;
    swapchain_info.imageFormat = chosen.format;
    swapchain_info.imageColorSpace = chosen.colorSpace;
    swapchain_info.imageExtent = p.extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    result = CreateSwapchainKHR((VkDevice)(uintptr_t)d->device, &swapchain_info, NULL, &p.swapchain);
    rsx_vk_log(d, "present: vkCreateSwapchainKHR -> %d", (int)result);
    if (result != VK_SUCCESS) return -1;
    GetSwapchainImagesKHR((VkDevice)(uintptr_t)d->device, p.swapchain, &p.image_count, NULL);
    if (p.image_count > 8) p.image_count = 8;
    if (!p.image_count || GetSwapchainImagesKHR((VkDevice)(uintptr_t)d->device, p.swapchain, &p.image_count, p.images) < 0) {
        rsx_vk_log(d, "present: no swapchain images");
        return -1;
    }

    memset(&pool_info, 0, sizeof pool_info);
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = d->queue_family;
    if (CreateCommandPool((VkDevice)(uintptr_t)d->device, &pool_info, NULL, &p.pool) != VK_SUCCESS) return -1;
    memset(&allocate_info, 0, sizeof allocate_info);
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = p.pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (AllocateCommandBuffers((VkDevice)(uintptr_t)d->device, &allocate_info, &p.cmd) != VK_SUCCESS) return -1;
    memset(&semaphore_info, 0, sizeof semaphore_info);
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    memset(&fence_info, 0, sizeof fence_info);
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (CreateSemaphore((VkDevice)(uintptr_t)d->device, &semaphore_info, NULL, &p.acquired) != VK_SUCCESS ||
        CreateSemaphore((VkDevice)(uintptr_t)d->device, &semaphore_info, NULL, &p.rendered) != VK_SUCCESS ||
        CreateFence((VkDevice)(uintptr_t)d->device, &fence_info, NULL, &p.fence) != VK_SUCCESS)
        return -1;
    p.ready = 1;
    rsx_vk_log(d, "present: ready, %u swapchain images", p.image_count);
    return 0;
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access,
                    VkAccessFlags destination_access)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = source_access;
    b.dstAccessMask = destination_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    p.CmdPipelineBarrier(p.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

int rsx_vk_present_surface(rsx_vk_device* d, u32 surface)
{
    rsx_vk_slot* slot;
    rsx_vk_image* source;
    VkCommandBufferBeginInfo begin_info;
    VkImageBlit blit;
    VkSubmitInfo submit;
    VkPresentInfoKHR present;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkImageLayout source_layout;
    uint32_t index = 0;
    VkResult result;

    if (!p.ready || !d) return -1;
    slot = rsx_vk_handle_get(d->handles, surface);
    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET) { p.failed++; return -1; }
    source = &slot->u.color;
    /* Everything recorded so far must be on the device before the blit reads the target. */
    if (rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE) != 0) { p.failed++; return -1; }
    /* Root (2026-11-12, Decision 94): the fence has passed, so everything retired during this frame is destroyed
     * here. Only the backend's own submit hook did that, and the live path presents through this function instead:
     * console runs ended with "vkAllocateMemory 886, vkFreeMemory 16". Every movie frame's three images stayed
     * allocated (about 0.5 GiB by the end of the logos) and the first real scene then got
     * "vkAllocateMemory for the image failed" 36015 times (run rv24). */
    rsx_vk_handle_collect(d);
    source_layout = (VkImageLayout)rsx_vk_image_layout(source);

    result = p.AcquireNextImageKHR((VkDevice)(uintptr_t)d->device, p.swapchain, UINT64_MAX, p.acquired, VK_NULL_HANDLE, &index);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        if (p.failed++ < 4) rsx_vk_log(d, "present: acquire -> %d", (int)result);
        return -1;
    }
    p.ResetCommandBuffer(p.cmd, 0);
    memset(&begin_info, 0, sizeof begin_info);
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    p.BeginCommandBuffer(p.cmd, &begin_info);
    if (source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        barrier((VkImage)(uintptr_t)source->image, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    barrier(p.images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)slot->u.color.width;
    blit.srcOffsets[1].y = (int32_t)slot->u.color.height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[1].x = (int32_t)p.extent.width;
    blit.dstOffsets[1].y = (int32_t)p.extent.height;
    blit.dstOffsets[1].z = 1;
    p.CmdBlitImage(p.cmd, (VkImage)(uintptr_t)source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.images[index],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    /* The target goes back to the layout the backend's tracker believes it is in. */
    if (source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && source_layout != VK_IMAGE_LAYOUT_UNDEFINED)
        barrier((VkImage)(uintptr_t)source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    else if (source_layout == VK_IMAGE_LAYOUT_UNDEFINED)
        rsx_vk_image_set_layout(source, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(p.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    p.EndCommandBuffer(p.cmd);

    memset(&submit, 0, sizeof submit);
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &p.acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &p.cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &p.rendered;
    p.ResetFences((VkDevice)(uintptr_t)d->device, 1, &p.fence);
    result = p.QueueSubmit((VkQueue)(uintptr_t)d->queue, 1, &submit, p.fence);
    if (result != VK_SUCCESS) {
        if (p.failed++ < 4) rsx_vk_log(d, "present: submit -> %d", (int)result);
        return -1;
    }
    memset(&present, 0, sizeof present);
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &p.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &p.swapchain;
    present.pImageIndices = &index;
    result = p.QueuePresentKHR((VkQueue)(uintptr_t)d->queue, &present);
    p.WaitForFences((VkDevice)(uintptr_t)d->device, 1, &p.fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        if (p.failed++ < 4) rsx_vk_log(d, "present: vkQueuePresentKHR -> %d", (int)result);
        return -1;
    }
    if (p.presented++ == 0) rsx_vk_log(d, "present: first frame on the screen (surface %u, %ux%u)", surface,
                                       slot->u.color.width, slot->u.color.height);
    return 0;
}

void rsx_vk_present_shutdown(rsx_vk_device* d)
{
    if (!p.ready || !d || !d->device) return;
    if (p.DeviceWaitIdle) p.DeviceWaitIdle((VkDevice)(uintptr_t)d->device);
    rsx_vk_log(d, "present: shutdown after %llu frames, %llu failures", (unsigned long long)p.presented,
               (unsigned long long)p.failed);
    if (p.DestroyFence) p.DestroyFence((VkDevice)(uintptr_t)d->device, p.fence, NULL);
    if (p.DestroySemaphore) {
        p.DestroySemaphore((VkDevice)(uintptr_t)d->device, p.acquired, NULL);
        p.DestroySemaphore((VkDevice)(uintptr_t)d->device, p.rendered, NULL);
    }
    if (p.DestroyCommandPool) p.DestroyCommandPool((VkDevice)(uintptr_t)d->device, p.pool, NULL);
    if (p.DestroySwapchainKHR) p.DestroySwapchainKHR((VkDevice)(uintptr_t)d->device, p.swapchain, NULL);
    if (p.DestroySurfaceKHR) p.DestroySurfaceKHR((VkInstance)(uintptr_t)d->instance, p.surface, NULL);
    memset(&p, 0, sizeof p);
}

uint64_t rsx_vk_present_count(void) { return p.presented; }

#else   /* desktop: no window */

int rsx_vk_present_init(rsx_vk_device* d, void* native_window) { (void)d; (void)native_window; return -1; }
int rsx_vk_present_surface(rsx_vk_device* d, u32 surface) { (void)d; (void)surface; return -1; }
void rsx_vk_present_shutdown(rsx_vk_device* d) { (void)d; }
uint64_t rsx_vk_present_count(void) { return 0; }

#endif
