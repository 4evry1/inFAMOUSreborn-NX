/*
 * ps3recomp - Vulkan backend: device, submission, staging
 *
 * Every Vulkan entry point is loaded into this file's own table by
 * RSX_VK_LOAD and checked once, so a missing proc is a logged init failure and
 * not a link error or a null call later. The header's PFN typedefs are the
 * contract; no prototype is ever linked.
 *
 * S1 scope, and nothing beyond it:
 *   - instance (+ the optional validation layer), physical device, device,
 *     one graphics+transfer queue, one command pool and fence per in-flight
 *     frame;
 *   - the UBO and readback staging slabs of DESIGN §4.2, host-visible,
 *     host-coherent and persistently mapped;
 *   - the four descriptor set layouts of DESIGN §2.2, built here so S2 finds
 *     them instead of inventing them;
 *   - submit_and_wait, which is fully synchronous because the engine recycles
 *     its staging the moment it returns (rsx_draw_engine.h:112-114).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* ---- the table (declared in rsx_vk_internal.h) -------------------------- */

rsx_vk_pfns g_vk;
static int g_vk_loaded;

/* The descriptor set layouts of DESIGN §2.2. They are built by init and
 * destroyed by shutdown; S2 binds against them. */
rsx_vk_layouts g_layouts;

/* ---- logging ----------------------------------------------------------- */

void rsx_vk_log(rsx_vk_device* d, const char* fmt, ...)
{
    va_list ap;
    (void)d;
    fputs("[rsx_vk] ", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void rsx_vk_unimplemented(rsx_vk_device* d, const char* member)
{
    printf("RSX_VK_UNIMPLEMENTED %s\n", member);
    fflush(stdout);
    (void)d;
}

int rsx_vk_check(rsx_vk_device* d, int vk_result, const char* what)
{
    if (vk_result == (int)VK_SUCCESS)
        return 0;
    rsx_vk_log(d, "%s failed: VkResult %d", what, vk_result);
    return -1;
}

/* ---- loading ----------------------------------------------------------- */

#define RSX_VK_LOAD_I(field, name)                                            \
    do {                                                                      \
        g_vk.field = (PFN_##name)(uintptr_t)rsx_vk_platform_instance_proc(     \
            &d->procs, d->instance, #name);                                    \
        if (!g_vk.field) {                                                     \
            rsx_vk_log(d, "missing entry point %s", #name);                     \
            return -1;                                                         \
        }                                                                      \
    } while (0)

#define RSX_VK_LOAD_D(field, name)                                            \
    do {                                                                      \
        g_vk.field = (PFN_##name)(uintptr_t)rsx_vk_platform_device_proc(       \
            &d->procs, d->device, #name);                                      \
        if (!g_vk.field) {                                                     \
            rsx_vk_log(d, "missing device entry point %s", #name);              \
            return -1;                                                         \
        }                                                                      \
    } while (0)

/*
 * Two passes, because Vulkan's instance entry points fall in two groups: the
 * three that must be reachable with no instance at all (vkCreateInstance,
 * vkEnumerateInstanceLayerProperties, vkEnumerateInstanceExtensionProperties)
 * and everything else, which only answers once the instance exists. Resolving
 * the whole set against a null instance returns NULL for the second group even
 * on a loader that would answer it -- so the second pass is separate.
 */
#if defined(__SWITCH__)
extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rsx_vk_switch_icd_gipa(VkInstance instance, const char* name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}
#endif
static int load_global_procs(rsx_vk_device* d)
{
    uint64_t gipa = rsx_vk_platform_instance_proc(&d->procs, 0,
                                                  "vkGetInstanceProcAddr");
    g_vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gipa;
    g_vk.EnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)
            rsx_vk_platform_instance_proc(&d->procs, 0,
                                          "vkEnumerateInstanceLayerProperties");
    g_vk.CreateInstance = (PFN_vkCreateInstance)
        rsx_vk_platform_instance_proc(&d->procs, 0, "vkCreateInstance");
    g_vk.EnumerateInstanceExtensionProperties =
        (PFN_vkEnumerateInstanceExtensionProperties)
            rsx_vk_platform_instance_proc(&d->procs, 0,
                                          "vkEnumerateInstanceExtensionProperties");
    /* Root (2026-11-12, first console run of the live renderer): the statically linked NXVK driver is an ICD, not a
     * loader. It did not answer every one of these four names and the renderer gave up before vkCreateInstance.
     * Only vkCreateInstance is needed without exception. The layer list is a loader service (absent = no layers);
     * vkGetInstanceProcAddr falls back to the ICD's own entry on the console; the extension list is only used when
     * present. The log names what was missing. */
    rsx_vk_log(d, "global entry points: GetInstanceProcAddr=%d EnumerateInstanceLayerProperties=%d CreateInstance=%d "
                  "EnumerateInstanceExtensionProperties=%d", g_vk.GetInstanceProcAddr != NULL,
               g_vk.EnumerateInstanceLayerProperties != NULL, g_vk.CreateInstance != NULL,
               g_vk.EnumerateInstanceExtensionProperties != NULL);
#if defined(__SWITCH__)
    if (!g_vk.GetInstanceProcAddr) g_vk.GetInstanceProcAddr = rsx_vk_switch_icd_gipa;
#endif
    if (!g_vk.GetInstanceProcAddr || !g_vk.CreateInstance) {
        rsx_vk_log(d, "the Vulkan loader does not answer the global entry points");
        return -1;
    }
    return 0;
}

static int load_instance_procs(rsx_vk_device* d)
{
    RSX_VK_LOAD_I(DestroyInstance, vkDestroyInstance);
    RSX_VK_LOAD_I(EnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    RSX_VK_LOAD_I(GetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    RSX_VK_LOAD_I(GetPhysicalDeviceQueueFamilyProperties,
                  vkGetPhysicalDeviceQueueFamilyProperties);
    RSX_VK_LOAD_I(GetPhysicalDeviceFormatProperties,
                  vkGetPhysicalDeviceFormatProperties);
    RSX_VK_LOAD_I(GetPhysicalDeviceFeatures, vkGetPhysicalDeviceFeatures);
    RSX_VK_LOAD_I(GetPhysicalDeviceMemoryProperties,
                  vkGetPhysicalDeviceMemoryProperties);
    return 0;
}

static int load_device_procs(rsx_vk_device* d)
{
    /* vkGetDeviceProcAddr is the bootstrap for everything below and is
     * itself reachable only through the instance, so it is loaded with the
     * instance entry points rather than through RSX_VK_LOAD_D. */
    RSX_VK_LOAD_I(GetDeviceProcAddr, vkGetDeviceProcAddr);
    RSX_VK_LOAD_D(DestroyDevice, vkDestroyDevice);
    RSX_VK_LOAD_D(DeviceWaitIdle, vkDeviceWaitIdle);
    RSX_VK_LOAD_D(CreateCommandPool, vkCreateCommandPool);
    RSX_VK_LOAD_D(DestroyCommandPool, vkDestroyCommandPool);
    RSX_VK_LOAD_D(AllocateCommandBuffers, vkAllocateCommandBuffers);
    RSX_VK_LOAD_D(FreeCommandBuffers, vkFreeCommandBuffers);
    RSX_VK_LOAD_D(BeginCommandBuffer, vkBeginCommandBuffer);
    RSX_VK_LOAD_D(EndCommandBuffer, vkEndCommandBuffer);
    RSX_VK_LOAD_D(ResetCommandBuffer, vkResetCommandBuffer);
    RSX_VK_LOAD_D(CreateFence, vkCreateFence);
    RSX_VK_LOAD_D(DestroyFence, vkDestroyFence);
    RSX_VK_LOAD_D(ResetFences, vkResetFences);
    RSX_VK_LOAD_D(GetFenceStatus, vkGetFenceStatus);
    RSX_VK_LOAD_D(WaitForFences, vkWaitForFences);
    RSX_VK_LOAD_D(QueueSubmit, vkQueueSubmit);
    RSX_VK_LOAD_D(CreateBuffer, vkCreateBuffer);
    RSX_VK_LOAD_D(DestroyBuffer, vkDestroyBuffer);
    RSX_VK_LOAD_D(GetBufferMemoryRequirements, vkGetBufferMemoryRequirements);
    RSX_VK_LOAD_D(AllocateMemory, vkAllocateMemory);
    RSX_VK_LOAD_D(FreeMemory, vkFreeMemory);
    RSX_VK_LOAD_D(BindBufferMemory, vkBindBufferMemory);
    RSX_VK_LOAD_D(MapMemory, vkMapMemory);
    RSX_VK_LOAD_D(UnmapMemory, vkUnmapMemory);
    RSX_VK_LOAD_D(CreateDescriptorSetLayout, vkCreateDescriptorSetLayout);
    RSX_VK_LOAD_D(DestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
    RSX_VK_LOAD_D(CreateImage, vkCreateImage);
    RSX_VK_LOAD_D(DestroyImage, vkDestroyImage);
    RSX_VK_LOAD_D(CreateImageView, vkCreateImageView);
    RSX_VK_LOAD_D(DestroyImageView, vkDestroyImageView);
    RSX_VK_LOAD_D(GetImageMemoryRequirements, vkGetImageMemoryRequirements);
    RSX_VK_LOAD_D(BindImageMemory, vkBindImageMemory);
    RSX_VK_LOAD_D(CmdPipelineBarrier, vkCmdPipelineBarrier);
    RSX_VK_LOAD_D(CmdClearColorImage, vkCmdClearColorImage);
    RSX_VK_LOAD_D(CmdCopyImageToBuffer, vkCmdCopyImageToBuffer);

    /* Slice S2. The dynamic-rendering pair are KHR entry points on a 1.1
     * instance, so they are named by their KHR spelling even when the driver
     * promotes them; the loader answers either way. */
    RSX_VK_LOAD_D(CreateShaderModule, vkCreateShaderModule);
    RSX_VK_LOAD_D(DestroyShaderModule, vkDestroyShaderModule);
    RSX_VK_LOAD_D(CreatePipelineLayout, vkCreatePipelineLayout);
    RSX_VK_LOAD_D(DestroyPipelineLayout, vkDestroyPipelineLayout);
    RSX_VK_LOAD_D(CreateGraphicsPipelines, vkCreateGraphicsPipelines);
    RSX_VK_LOAD_D(DestroyPipeline, vkDestroyPipeline);
    RSX_VK_LOAD_D(CreatePipelineCache, vkCreatePipelineCache);
    RSX_VK_LOAD_D(DestroyPipelineCache, vkDestroyPipelineCache);
    RSX_VK_LOAD_D(CreateDescriptorPool, vkCreateDescriptorPool);
    RSX_VK_LOAD_D(DestroyDescriptorPool, vkDestroyDescriptorPool);
    RSX_VK_LOAD_D(ResetDescriptorPool, vkResetDescriptorPool);
    RSX_VK_LOAD_D(AllocateDescriptorSets, vkAllocateDescriptorSets);
    RSX_VK_LOAD_D(UpdateDescriptorSets, vkUpdateDescriptorSets);
    RSX_VK_LOAD_D(CmdBindPipeline, vkCmdBindPipeline);
    RSX_VK_LOAD_D(CmdBindDescriptorSets, vkCmdBindDescriptorSets);
    RSX_VK_LOAD_D(CmdBeginRenderingKHR, vkCmdBeginRenderingKHR);
    RSX_VK_LOAD_D(CmdEndRenderingKHR, vkCmdEndRenderingKHR);
    RSX_VK_LOAD_D(CmdSetViewport, vkCmdSetViewport);
    RSX_VK_LOAD_D(CmdSetScissor, vkCmdSetScissor);
    RSX_VK_LOAD_D(CmdSetStencilReference, vkCmdSetStencilReference);
    RSX_VK_LOAD_D(CmdBindVertexBuffers, vkCmdBindVertexBuffers);
    RSX_VK_LOAD_D(CmdBindIndexBuffer, vkCmdBindIndexBuffer);
    RSX_VK_LOAD_D(CmdDraw, vkCmdDraw);
    RSX_VK_LOAD_D(CmdDrawIndexed, vkCmdDrawIndexed);

    /* Slice S3. The two vkCmdBindDescriptorSetsKHR spellings are only needed
     * if a backend path ever pushes descriptors, so they are
     * optional: a driver that does not answer them is not a failure. */
    RSX_VK_LOAD_D(CreateSampler, vkCreateSampler);
    RSX_VK_LOAD_D(DestroySampler, vkDestroySampler);
    RSX_VK_LOAD_D(CmdCopyBufferToImage, vkCmdCopyBufferToImage);
    RSX_VK_LOAD_D(CmdClearDepthStencilImage, vkCmdClearDepthStencilImage);
    RSX_VK_LOAD_D(CmdCopyImage, vkCmdCopyImage);
    g_vk.GetPhysicalDeviceFormatProperties2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)(uintptr_t)
            rsx_vk_platform_instance_proc(&d->procs, d->instance,
                                          "vkGetPhysicalDeviceFormatProperties2");
    if (!g_vk.GetPhysicalDeviceFormatProperties2)
        rsx_vk_log(d, "vkGetPhysicalDeviceFormatProperties2 absent; the format "
                   "table falls back to vkGetPhysicalDeviceFormatProperties");
    return 0;
}

/* ---- validation -------------------------------------------------------- */

static VKAPI_ATTR VkBool32 VKAPI_CALL rsx_vk_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    rsx_vk_device* d = (rsx_vk_device*)user;
    int level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                    ? 2
                    : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                          ? 1
                          : 0;
    (void)types;
    printf("VALIDATION %s: %s\n",
           level == 2 ? "ERROR" : level == 1 ? "WARNING" : "INFO",
           data && data->pMessage ? data->pMessage : "(no message)");
    fflush(stdout);
    if (d)
        d->validation_messages++;
    if (d && d->report)
        d->report(d->report_user, level, data && data->pMessage ? data->pMessage
                                                               : "");
    return VK_FALSE;
}

void rsx_vk_device_set_report(rsx_vk_device* d, rsx_vk_report_fn fn, void* user)
{
    d->report = fn;
    d->report_user = user;
}

/* The layer list, once, so "absent" is a fact in the log rather than a guess. */
static int layer_present(const char* wanted)
{
    uint32_t count = 0, i;
    VkLayerProperties props[64];
    if (!g_vk.EnumerateInstanceLayerProperties)
        return 0;
    if (g_vk.EnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS)
        return 0;
    if (count > 64)
        count = 64;
    if (g_vk.EnumerateInstanceLayerProperties(&count, props) != VK_SUCCESS)
        return 0;
    for (i = 0; i < count; i++)
        if (strcmp(props[i].layerName, wanted) == 0)
            return 1;
    return 0;
}

/* ---- memory helpers ---------------------------------------------------- */

static uint32_t find_memory_type(rsx_vk_device* d, uint32_t type_bits,
                                 VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i;
    g_vk.GetPhysicalDeviceMemoryProperties((VkPhysicalDevice)d->physical, &mp);
    for (i = 0; i < mp.memoryTypeCount && i < 32; i++) {
        if (!(type_bits & (1u << i)))
            continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

/* The same lookup for rsx_vk_target.c, which creates images rather than
 * buffers. Exported because the memory-property query is in this file's
 * private table. */
uint32_t rsx_vk_memory_type_find(rsx_vk_device* d, uint32_t type_bits,
                                 uint32_t want_flags)
{
    return find_memory_type(d, type_bits, (VkMemoryPropertyFlags)want_flags);
}

/* staging ring */

static int ring_create(rsx_vk_device* d, uint64_t* buf, uint64_t* mem,
                       void** map, uint32_t bytes, const char* what)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkDevice dev = (VkDevice)d->device;
    uint32_t type;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_vk.CreateBuffer(dev, &bi, NULL, (VkBuffer*)buf) != VK_SUCCESS) {
        rsx_vk_log(d, "%s: vkCreateBuffer failed", what);
        return -1;
    }
    g_vk.GetBufferMemoryRequirements(dev, (VkBuffer)*buf, &req);
    type = find_memory_type(d, req.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        rsx_vk_log(d, "%s: no host-visible coherent memory type", what);
        return -1;
    }
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (g_vk.AllocateMemory(dev, &ai, NULL, (VkDeviceMemory*)mem) != VK_SUCCESS) {
        rsx_vk_log(d, "%s: vkAllocateMemory failed", what);
        return -1;
    }
    d->memory_allocs++;
    if (g_vk.BindBufferMemory(dev, (VkBuffer)*buf, (VkDeviceMemory)*mem, 0) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "%s: vkBindBufferMemory failed", what);
        return -1;
    }
    if (g_vk.MapMemory(dev, (VkDeviceMemory)*mem, 0, VK_WHOLE_SIZE, 0, map) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "%s: vkMapMemory failed", what);
        return -1;
    }
    rsx_vk_log(d, "%s: %u bytes, memory type %u, persistently mapped", what,
               bytes, type);
    return 0;
}

static void ring_destroy(rsx_vk_device* d, uint64_t* buf, uint64_t* mem,
                         void** map)
{
    VkDevice dev = (VkDevice)d->device;
    if (*map && *mem) {
        g_vk.UnmapMemory(dev, (VkDeviceMemory)*mem);
        *map = NULL;
    }
    if (*buf) {
        g_vk.DestroyBuffer(dev, (VkBuffer)*buf, NULL);
        *buf = 0;
    }
    if (*mem) {
        g_vk.FreeMemory(dev, (VkDeviceMemory)*mem, NULL);
        d->memory_frees++;
        *mem = 0;
    }
}

void* rsx_vk_ubo_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment)
{
    uint32_t align = alignment ? alignment : 4u;
    uint32_t aligned;
    void* p;
    if (align > d->ubo_align)
        align = d->ubo_align;
    aligned = (d->ubo_offset + align - 1u) & ~(align - 1u);
    if (aligned > RSX_VK_UBO_SLAB_BYTES || bytes > RSX_VK_UBO_SLAB_BYTES - aligned)
        return NULL;
    p = (void*)((uint8_t*)d->ubo_map + aligned);
    /* The offset *of this reservation*, not the new bump pointer: the caller
     * needs it for the descriptor and rsx_vk_ubo_last_offset reports it. S1
     * left this reporting the advanced pointer, which pointed every descriptor
     * one block past its data. */
    d->ubo_last_offset = aligned;
    d->ubo_offset = aligned + bytes;
    return p;
}

uint32_t rsx_vk_ubo_last_offset(const rsx_vk_device* d)
{
    return d->ubo_last_offset;
}

void* rsx_vk_readback_slab(rsx_vk_device* d, uint32_t bytes)
{
    if (bytes > RSX_VK_READBACK_SLAB_BYTES)
        return NULL;
    return d->rb_map;
}

/* descriptor set layouts */


/*
 * The geometry slab: one draw's vertices and one draw's indices, at the
 * alignment the buffer copy wants. A slab that cannot take the
 * request flushes the same way the UBO arena does -- submit_and_wait is
 * synchronous by contract, so the caller can retry immediately.
 */
void* rsx_vk_stage_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment,
                           uint32_t* out_offset)
{
    uint32_t off;
    uint32_t align = alignment ? alignment : 4u;
    if (out_offset)
        *out_offset = 0;
    if (!d->geom_map || !bytes)
        return NULL;
    off = (d->geom_offset + align - 1u) & ~(align - 1u);
    if (off > RSX_VK_GEOM_SLAB_BYTES || bytes > RSX_VK_GEOM_SLAB_BYTES - off)
        return NULL;
    d->geom_offset = off + bytes;
    if (out_offset)
        *out_offset = off;
    return (uint8_t*)d->geom_map + off;
}

/* COLOR_ATTACHMENT_OPTIMAL, recorded only when the image is not there: the
 * layout tracker of DESIGN §4.3 rule 4 in its S2 form. */
int rsx_vk_cmd_to_color_attachment(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    VkCommandBuffer cmd;
    const int32_t want = (int32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    int32_t from = rsx_vk_image_layout(img);
    if (!img || !img->image)
        return -1;
    if (from == want)
        return 0;
    cmd = (VkCommandBuffer)d->cmd[d->frame];
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                            NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

static uint64_t make_layout(rsx_vk_device* d, const VkDescriptorSetLayoutBinding* b,
                            uint32_t n, const char* what)
{
    VkDescriptorSetLayoutCreateInfo ci;
    uint64_t layout = 0;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = n;
    ci.pBindings = b;
    if (g_vk.CreateDescriptorSetLayout((VkDevice)d->device, &ci, NULL,
                                       (VkDescriptorSetLayout*)&layout) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "%s: vkCreateDescriptorSetLayout failed", what);
        return 0;
    }
    rsx_vk_log(d, "%s: %u bindings", what, n);
    return layout;
}

static int layouts_create(rsx_vk_device* d)
{
    /*
     * ONE layout, ONE set, because that is what glslang 16.6.0's HLSL front
     * end emits: every resource variable of a stage is `DescriptorSet 0` and
     * takes its `register()` number as the Binding, with the samplers shifted
     * clear of the images by RSX_VK_SAMPLER_BINDING_SHIFT (rsx_vk_shader.cpp's
     * setShiftBinding call). spirv-dis of the S3 test programs:
     *
     *   PS: rsx_tex[16] Binding 0, rsx_samp[16] Binding 32, PSConstants
     *       (b1) Binding 1    -- all DescriptorSet 0
     *   VS: rsx_vtex0 Binding 16, rsx_vsamp0 Binding 32, VPConst (b0)
     *       Binding 0         -- all DescriptorSet 0
     *
     * So the bindings are:
     *
     *   set 0  b0       UNIFORM_BUFFER_DYNAMIC  VS+PS   (VPConst)
     *   set 0  b1       UNIFORM_BUFFER_DYNAMIC  VS+PS   (PSConstants)
     *   set 0  binding 4..23    SAMPLED_IMAGE  VS+PS  (the t registers, shifted)
     *   set 0  binding 32..51   SAMPLER        VS+PS  (the s registers, shifted)
     *
     * The images and samplers keep the *separate* descriptor types DESIGN
     * §2.2 describes (a `Texture2D` and a `SamplerState` are separate SPIR-V
     * variables combined by OpSampledImage at the sample), so a sampler
     * change never rewrites an image descriptor.
     *
     * DESIGN §2.2 predicted images in set 1 and samplers in set 2. The set
     * numbers are the compiler's, not ours, and one set with the shifted
     * binding numbers is what it actually produces; REPORT.md records the
     * disagreements and the spirv-dis evidence for each.
     *
     * A binding number can only carry one descriptor type, which is why the
     * sampler shift is required rather than optional: without it t0 and s0
     * both land on binding 0 and no layout can satisfy both.
     */
    /* b0, b1, then one binding per t register and one per s register; the two
     * unit ranges are each RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS. */
    VkDescriptorSetLayoutBinding b[2 + 2 * (RSX_VK_MAX_TEX_UNITS +
                                            RSX_VK_MAX_VTEX_UNITS)];
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT |
                                    VK_SHADER_STAGE_FRAGMENT_BIT;
    uint32_t n = 0, i;
    memset(b, 0, sizeof b);
    b[n].binding = 0;
    b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1;
    b[n].stageFlags = both;
    n++;
    b[n].binding = 1;
    b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    b[n].descriptorCount = 1;
    b[n].stageFlags = both;
    n++;
    /* t0..t19: the PS's sixteen units and the VS's four vertex textures, in
     * one t register space, shifted clear of b0/b1. The VS's four sit at
     * their own shift (RSX_VK_VS_IMAGE_BINDING_SHIFT) so a VS unit can never
     * alias PS unit 0; the PS's sit at RSX_VK_IMAGE_BINDING_SHIFT + unit. */
    for (i = 0; i < RSX_VK_MAX_TEX_UNITS; i++, n++) {
        b[n].binding = RSX_VK_IMAGE_BINDING_SHIFT + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[n].descriptorCount = 1;
        b[n].stageFlags = both;
    }
    for (i = 0; i < RSX_VK_MAX_VTEX_UNITS; i++, n++) {
        b[n].binding = RSX_VK_VS_IMAGE_BINDING_SHIFT + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    /* s0..s15 for the PS and s0..s3 of the VS's own register space, at the
     * same split. */
    for (i = 0; i < RSX_VK_MAX_TEX_UNITS; i++, n++) {
        b[n].binding = RSX_VK_SAMPLER_BINDING_SHIFT + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = both;
    }
    for (i = 0; i < RSX_VK_MAX_VTEX_UNITS; i++, n++) {
        b[n].binding = RSX_VK_VS_SAMPLER_BINDING_SHIFT + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    g_layouts.set0_textures = make_layout(d, b, n, "layout set0 (b0,b1,PS t0..t15,"
                                          "VS t16..t19,PS s0..s15,VS s0..s3)");
    if (!g_layouts.set0_textures)
        return -1;
    /* One set: the images, the samplers and both constant blocks. */
    g_layouts.set1_constants = g_layouts.set0_textures;
    return 0;
}

static void layouts_destroy(rsx_vk_device* d)
{
    VkDevice dev = (VkDevice)d->device;
    /* One layout now: set1_constants aliases set0_textures, so it must not be
     * destroyed twice. */
    if (g_layouts.set0_textures)
        g_vk.DestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout)g_layouts.set0_textures, NULL);
    memset(&g_layouts, 0, sizeof(g_layouts));
}

/* ---- recording --------------------------------------------------------- */

int rsx_vk_device_begin(rsx_vk_device* d)
{
    VkCommandBufferBeginInfo bi;
    VkCommandBuffer cmd;
    if (d->recording)
        return 0;
    cmd = (VkCommandBuffer)d->cmd[d->frame];
    if (g_vk.ResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        rsx_vk_log(d, "vkResetCommandBuffer failed");
        return -1;
    }
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_vk.BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        rsx_vk_log(d, "vkBeginCommandBuffer failed");
        return -1;
    }
    /* recording == 1 means "this frame's command buffer has commands in it that
     * a submit would have to execute" (item M3). It is set here and nowhere
     * else, and every path that records into the command buffer reaches this
     * call first: depth_snapshot (rsx_vk_view.c:rsx_vk_depth_snapshot), the
     * colour clear and the readback (rsx_vk_target.c:rsx_vk_target_clear,
     * :rsx_vk_target_readback), the texture upload batch
     * (rsx_vk_texture.c:rsx_vk_cmd_upload_rows, rsx_vk_textures_end_uploads),
     * the sampled-read and depth-read transitions
     * (rsx_vk_view.c:rsx_vk_cmd_to_sampled_read, _depth_sampled_read,
     * rsx_vk_pipeline.c:to_color_attachment/to_depth_attachment) and the pass
     * plus the draw (rsx_vk_pipeline.c:rsx_vk_pass_begin). So no path can
     * record without being submitted. */
    d->recording = 1;
    return 0;
}

uint64_t rsx_vk_device_cmd(rsx_vk_device* d)
{
    return d->cmd[d->frame];
}

/* ---- the two commands S1 records --------------------------------------- */

/*
 * A colour clear as a direct vkCmdClearColorImage (see rsx_vk_target.c for why
 * S1 chose this over a dynamic-rendering load op). The layout tracker of
 * DESIGN §4.3 is the only state carried across commands: an image in GENERAL
 * needs no transition, anything else does.
 */
int rsx_vk_cmd_clear_color(rsx_vk_device* d, rsx_vk_image* img,
                           const float rgba[4])
{
    VkImageMemoryBarrier b;
    VkClearColorValue c;
    VkImageSubresourceRange range;
    VkCommandBuffer cmd = (VkCommandBuffer)d->cmd[d->frame];
    int32_t from = rsx_vk_image_layout(img);

    if (from != (int32_t)VK_IMAGE_LAYOUT_GENERAL) {
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = (VkImage)img->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                NULL, 1, &b);
        rsx_vk_image_set_layout(img, (int32_t)VK_IMAGE_LAYOUT_GENERAL);
    }

    memset(&c, 0, sizeof(c));
    c.float32[0] = rgba[0];
    c.float32[1] = rgba[1];
    c.float32[2] = rgba[2];
    c.float32[3] = rgba[3];
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;
    g_vk.CmdClearColorImage(cmd, (VkImage)img->image,
                            VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    return 0;
}

/*
 * Copy a rectangle of `img` into the readback slab. The copy's row length is
 * fixed by the caller, and the buffer offset is zero because the slab is
 * reused per call and the fence wait below makes that safe.
 */
/* Bytes per texel of the image's format, as the copy's row length needs it:
 * `bufferRowLength` is in *texels*, not bytes. */
static uint32_t copy_texel_bytes(uint32_t vk_format)
{
    switch (vk_format) {
    case VK_FORMAT_R8_UNORM:            return 1u;
    case VK_FORMAT_R8G8_UNORM:          return 2u;
    case VK_FORMAT_R8G8B8A8_UNORM:      return 4u;
    case VK_FORMAT_R16_UNORM:           return 2u;
    case VK_FORMAT_R16G16_UNORM:        return 4u;
    case VK_FORMAT_R16G16_SFLOAT:       return 4u;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8u;
    case VK_FORMAT_R32_SFLOAT:          return 4u;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16u;
    default:                            return 4u;
    }
}

int rsx_vk_cmd_copy_to_readback(rsx_vk_device* d, rsx_vk_image* img, u32 x,
                                u32 y, u32 w, u32 h, uint32_t row_bytes)
{
    VkImageMemoryBarrier b;
    VkBufferImageCopy copy;
    VkCommandBuffer cmd = (VkCommandBuffer)d->cmd[d->frame];
    int32_t from = rsx_vk_image_layout(img);

    if (from != (int32_t)VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = (VkImage)img->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                NULL, 1, &b);
        rsx_vk_image_set_layout(img, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = 0;
    copy.bufferRowLength = row_bytes / copy_texel_bytes(img->format);
    copy.bufferImageHeight = 0;              /* tightly packed rows */
    copy.imageOffset.x = (int32_t)x;
    copy.imageOffset.y = (int32_t)y;
    copy.imageOffset.z = 0;
    copy.imageExtent.width = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth = 1;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    g_vk.CmdCopyImageToBuffer(cmd, (VkImage)img->image,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              (VkBuffer)d->rb_buf, 1, &copy);
    /* The copy's result is read on the host after the fence, so the buffer is
     * handed to the host domain here rather than in a second barrier. */
    {
        VkBufferMemoryBarrier bb;
        memset(&bb, 0, sizeof(bb));
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = (VkBuffer)d->rb_buf;
        bb.offset = 0;
        bb.size = VK_WHOLE_SIZE;
        g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb,
                                0, NULL);
    }
    return 0;
}

/* ---- submit_and_wait --------------------------------------------------- */

/*
 * Everything that has to happen when a frame boundary is reached, whether or
 * not that boundary carried a submit (item M3).
 *
 * The three staging-ring offsets are rewound: the engine recycles its upload
 * staging the moment submit_and_wait returns, so the slab is the
 * caller's to reuse again. The pass state is cleared because a pass cannot
 * survive the frame it was begun in. The snapshot allowance comes back, and the
 * snapshot pool with it -- the cap and the pool describe the same sets, so they
 * are reset together and can never disagree.
 *
 * Before this existed, the no-op path (nothing recorded) reset only the three
 * ring offsets and returned; the snapshot counter kept the refused frame's
 * count and the pool kept its sets, so a frame that submitted nothing after a
 * frame that had used its whole snapshot allowance could not snapshot at all
 * until some later frame happened to record something. One function for both
 * paths is what makes "every frame boundary resets the same state" true by
 * construction rather than by two copies staying in step.
 */
static void frame_boundary_reset(rsx_vk_device* d)
{
    VkDevice dev = (VkDevice)d->device;

    d->ubo_offset = 0;
    d->geom_offset = 0;
    d->tex_offset = 0;
    d->rendering = 0;
    d->rendering_target = 0;
    d->rendering_depth = 0;
    if (d->descriptor_pool)
        g_vk.ResetDescriptorPool(dev, (VkDescriptorPool)d->descriptor_pool, 0);
    /* The snapshot's own pool is reset at the same fence, which is what lets it
     * be one pool for the device's lifetime instead of one per snapshot. It is
     * only reset when it has allocated something, so a device that never takes
     * a snapshot never touches it. */
    if (d->snap_pool_created && d->snap_pool_in_use) {
        g_vk.ResetDescriptorPool(dev, (VkDescriptorPool)d->snap_pool_created, 0);
        d->snap_pool_in_use = 0;
    }
    /* ...and the per-frame snapshot allowance comes back with it (item N5):
     * the cap and the pool describe the same sets, so they are cleared at the
     * same boundary and can never disagree. */
    d->snapshots_this_frame = 0;
    d->frame_resets++;
}

int rsx_vk_device_submit_and_wait(rsx_vk_device* d, int reason)
{
    VkSubmitInfo si;
    VkDevice dev = (VkDevice)d->device;
    VkFence fence;

    if (!d->device)
        return -1;

    if (!d->recording) {
        /* Nothing recorded: the contract says "returns", not "submits an
         * empty batch". An empty vkQueueSubmit is legal but pointless, and a
         * test asserts this path does not submit at all.
         *
         * It is still a frame boundary (item M3), so the same reset the fence
         * does has to run here. The vkResetDescriptorPool arguments are the
         * already-loaded entry points, so this allocates nothing and submits
         * nothing: the case that asserts "nothing recorded allocates no
         * memory" still holds. */
        rsx_vk_log(d, "submit_and_wait reason=%d: nothing recorded, no-op",
                   reason);
        d->submit_noops++;
        frame_boundary_reset(d);
        return 0;
    }

    if (g_vk.EndCommandBuffer((VkCommandBuffer)d->cmd[d->frame]) != VK_SUCCESS) {
        rsx_vk_log(d, "vkEndCommandBuffer failed");
        d->recording = 0;
        return -1;
    }
    d->recording = 0;

    fence = (VkFence)d->fence[d->frame];
    /* The frame's fence starts signalled (see frames_create) so a frame that
     * is never submitted cannot block a wait. It must be unsignalled before
     * the submit, or the wait below returns while the GPU is still recording:
     * the host would read the readback buffer before the copy landed. */
    if (g_vk.ResetFences(dev, 1, &fence) != VK_SUCCESS) {
        rsx_vk_log(d, "vkResetFences before submit failed");
        return -1;
    }
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = (VkCommandBuffer*)&d->cmd[d->frame];
    {   /* Root (2026-11-12, Decision 99): the result code, and at most 16 lines of it. Console run rv30 logged
         * "vkQueueSubmit failed" 12484 times without saying why. */
        const VkResult submitted = g_vk.QueueSubmit((VkQueue)d->queue, 1, &si, fence);
        if (submitted != VK_SUCCESS) {
            static unsigned failures;
            if (failures++ < 16u)
                rsx_vk_log(d, "vkQueueSubmit failed: VkResult %d (reason %d, submits so far %llu, failure %u)",
                           (int)submitted, reason, (unsigned long long)d->submits, failures);
            /* The rings must still come back, or every later draw overflows them for ever. */
            frame_boundary_reset(d);
            return -1;
        }
    }
    d->submits++;

    /* RSX_BE_FLUSH_SHUTDOWN does not wait: shutdown waits on the device
     * anyway, and a caller that has already lost the device should not block
     * here. Every other reason waits, because the engine
     * recycles its staging the moment this returns. */
    if (reason != RSX_BE_FLUSH_SHUTDOWN) {
        if (g_vk.WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkWaitForFences failed");
            return -1;
        }
    }
    /* The ring slots are free the moment the fence passes, and
     * so is every descriptor set this frame's command buffer wrote; the
     * snapshot allowance and its pool come back here too. One function, the
     * same one the no-op path runs (item M3). */
    frame_boundary_reset(d);

    /* Fence passed: samplers evicted during the frame just submitted can now
     * be destroyed; nothing recorded can still name them (item 4). */
    rsx_vk_samplers_collect(d);
    return 0;
}

/* ---- init / shutdown --------------------------------------------------- */

static int pick_physical(rsx_vk_device* d)
{
    VkPhysicalDevice devs[16];
    uint32_t count = 16, i;
    int chosen = -1;

    if (g_vk.EnumeratePhysicalDevices((VkInstance)d->instance, &count, devs) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "vkEnumeratePhysicalDevices failed");
        return -1;
    }
    if (count == 0) {
        rsx_vk_log(d, "no Vulkan physical device");
        return -1;
    }
    for (i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        g_vk.GetPhysicalDeviceProperties(devs[i], &props);
        rsx_vk_log(d, "physical device %u: %s (api %u.%u.%u)", i, props.deviceName,
                   VK_API_VERSION_MAJOR(props.apiVersion),
                   VK_API_VERSION_MINOR(props.apiVersion),
                   VK_API_VERSION_PATCH(props.apiVersion));
        if (chosen < 0)
            chosen = (int)i; /* the first device is the one the ICD named */
    }
    if (chosen < 0)
        return -1;
    d->physical = (uint64_t)(uintptr_t)devs[chosen];
    return 0;
}

static int fill_properties(rsx_vk_device* d)
{
    VkPhysicalDeviceProperties p;
    VkPhysicalDeviceMemoryProperties mp;
    g_vk.GetPhysicalDeviceProperties((VkPhysicalDevice)d->physical, &p);
    snprintf(d->device_name, sizeof(d->device_name), "%s", p.deviceName);
    d->max_color_w = p.limits.maxImageDimension2D;
    d->max_color_h = p.limits.maxImageDimension2D;
    d->max_memory_allocs = p.limits.maxMemoryAllocationCount;
    g_vk.GetPhysicalDeviceMemoryProperties((VkPhysicalDevice)d->physical, &mp);
    d->memory_type_count = mp.memoryTypeCount;
    d->ubo_align = p.limits.minUniformBufferOffsetAlignment
                       ? (uint32_t)p.limits.minUniformBufferOffsetAlignment
                       : 4u;
    if (d->ubo_align > 256u)
        d->ubo_align = 256u;
    rsx_vk_log(d, "device: %s, maxImageDimension2D %u, maxMemoryAllocationCount %u",
               d->device_name, d->max_color_w, d->max_memory_allocs);
    rsx_vk_log(d, "limits: minUniformBufferOffsetAlignment %u (ring alignment)",
               d->ubo_align);
    return 0;
}

/*
 * Which of the three extensions S2 needs this physical device actually
 * advertises. DESIGN §1.3/§1.7 and R1 both insist on enumerating rather than
 * assuming, and the log line is what a hardware run is read from.
 *
 * vkEnumerateDeviceExtensionProperties is reached through the loaded instance
 * table, so it is resolved here and kept local: S2 is the only user.
 */
static int device_extension_present(rsx_vk_device* d, const char* wanted)
{
    PFN_vkEnumerateDeviceExtensionProperties fn =
        (PFN_vkEnumerateDeviceExtensionProperties)(uintptr_t)
            rsx_vk_platform_instance_proc(&d->procs, d->instance,
                                          "vkEnumerateDeviceExtensionProperties");
    VkExtensionProperties props[256];
    uint32_t count = 256, i;
    if (!fn)
        return 0;
    if (fn((VkPhysicalDevice)d->physical, NULL, &count, NULL) != VK_SUCCESS)
        return 0;
    if (count > 256)
        count = 256;
    if (fn((VkPhysicalDevice)d->physical, NULL, &count, props) != VK_SUCCESS)
        return 0;
    for (i = 0; i < count; i++)
        if (strcmp(props[i].extensionName, wanted) == 0)
            return 1;
    return 0;
}

static int create_device(rsx_vk_device* d)
{
    VkQueueFamilyProperties qf[8];
    uint32_t qcount = 8, i;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    VkDeviceCreateInfo ci;
    VkDevice dev;

    g_vk.GetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)d->physical,
                                                &qcount, qf);
    d->queue_family = UINT32_MAX;
    for (i = 0; i < qcount; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            d->queue_family = i;
            break;
        }
    }
    if (d->queue_family == UINT32_MAX) {
        rsx_vk_log(d, "no graphics queue family");
        return -1;
    }
    rsx_vk_log(d, "queue family %u: graphics%s", d->queue_family,
               (qf[d->queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)
                   ? "+compute"
                   : "");

    /* vkCreateDevice is the one device entry point that must be reachable
     * before a device exists, so it is resolved here rather than in
     * load_device_procs. */
    g_vk.CreateDevice = (PFN_vkCreateDevice)(uintptr_t)
        rsx_vk_platform_instance_proc(&d->procs, d->instance, "vkCreateDevice");
    if (!g_vk.CreateDevice) {
        rsx_vk_log(d, "missing entry point vkCreateDevice");
        return -1;
    }

    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    /* The extensions S2 records passes with. Dynamic rendering is what lets
     * the backend exist without VkRenderPass at all; it is
     * required, so a device without it is a hard failure rather than a silent
     * fall back to render passes this backend does not implement. */
    {
        const char* exts[4];
        uint32_t n = 0;
        d->has_dynamic_rendering = device_extension_present(
            d, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        d->has_synchronization2 = device_extension_present(
            d, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        d->has_push_descriptor = device_extension_present(
            d, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        if (!d->has_dynamic_rendering) {
            rsx_vk_log(d, "VK_KHR_dynamic_rendering is absent: S2 needs it");
            return -1;
        }
        exts[n++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
        if (d->has_synchronization2)
            exts[n++] = VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME;
        if (d->has_push_descriptor)
            exts[n++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
#if defined(__SWITCH__)
        /* Decision 66: the swapchain rsx_vk_present.c creates. Only on the console, where a window exists. */
        if (device_extension_present(d, "VK_KHR_swapchain"))
            exts[n++] = "VK_KHR_swapchain";
        else
            rsx_vk_log(d, "VK_KHR_swapchain is absent: nothing can be presented");
#endif
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = 1;
        ci.pQueueCreateInfos = &qci;
        ci.enabledExtensionCount = n;
        ci.ppEnabledExtensionNames = exts;
        if (g_vk.CreateDevice((VkPhysicalDevice)d->physical, &ci, NULL, &dev) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkCreateDevice failed");
            return -1;
        }
        rsx_vk_log(d, "device extensions: dynamic_rendering=1 synchronization2=%d "
                   "push_descriptor=%d", d->has_synchronization2,
                   d->has_push_descriptor);
    }
    d->device = (uint64_t)(uintptr_t)dev;
    /* Same reason as CreateDevice: reachable only through the instance. */
    g_vk.GetDeviceQueue = (PFN_vkGetDeviceQueue)(uintptr_t)
        rsx_vk_platform_instance_proc(&d->procs, d->instance, "vkGetDeviceQueue");
    if (!g_vk.GetDeviceQueue) {
        rsx_vk_log(d, "missing entry point vkGetDeviceQueue");
        return -1;
    }
    g_vk.GetDeviceQueue(dev, d->queue_family, 0, (VkQueue*)&d->queue);
    return 0;
}

static int frames_create(rsx_vk_device* d)
{
    uint32_t i;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo ai;
    VkFenceCreateInfo fi;
    VkDevice dev = (VkDevice)d->device;

    for (i = 0; i < d->in_flight; i++) {
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = d->queue_family;
        if (g_vk.CreateCommandPool(dev, &pci, NULL, (VkCommandPool*)&d->pool[i]) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkCreateCommandPool %u failed", i);
            return -1;
        }
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = (VkCommandPool)d->pool[i];
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (g_vk.AllocateCommandBuffers(dev, &ai, (VkCommandBuffer*)&d->cmd[i]) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkAllocateCommandBuffers %u failed", i);
            return -1;
        }
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        /* Signalled, so a frame that is never submitted does not block the
         * first wait. A reset happens right before each submit. */
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (g_vk.CreateFence(dev, &fi, NULL, (VkFence*)&d->fence[i]) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkCreateFence %u failed", i);
            return -1;
        }
    }
    rsx_vk_log(d, "%u in-flight frames: one command pool and fence each",
               d->in_flight);
    return 0;
}

static void frames_destroy(rsx_vk_device* d)
{
    uint32_t i;
    VkDevice dev = (VkDevice)d->device;
    if (!dev)
        return;
    for (i = 0; i < RSX_VK_MAX_IN_FLIGHT; i++) {
        if (d->pool[i]) {
            g_vk.DestroyCommandPool(dev, (VkCommandPool)d->pool[i], NULL);
            d->pool[i] = 0;
            d->cmd[i] = 0;
        }
        if (d->fence[i]) {
            g_vk.DestroyFence(dev, (VkFence)d->fence[i], NULL);
            d->fence[i] = 0;
        }
    }
}

int rsx_vk_device_init(rsx_vk_device* d, u32 width, u32 height)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    const char* exts[RSX_VK_MAX_EXT];
    const char* layers[1];
    uint32_t next = 0, nlayers = 0;
    int have_debug = 0;

    memset(d, 0, sizeof(*d));
    d->width = width;
    d->height = height;

    if (rsx_vk_platform_open(d) != RSX_VK_PLATFORM_OK) {
        rsx_vk_log(d, "platform open failed (%s)", rsx_vk_platform_driver());
        return -1;
    }
    if (load_global_procs(d) != 0)
        return -1;

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "inFAMOUS native rsx_vulkan";
    app.applicationVersion = 1;
    app.pEngineName = "ps3recomp rsx_draw_engine";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_1; /* DESIGN §2.1 floor */

    /* Headless: the platform contributes its own instance extensions (none on
     * the desktop, the vi surface in S5). */
    next = rsx_vk_platform_instance_extensions(exts, RSX_VK_MAX_EXT);

    /* Validation. If the layer is there, enable it and let the callback count
     * everything; if not, say so in the log and run. */
    if (layer_present("VK_LAYER_KHRONOS_validation")) {
        if (next < RSX_VK_MAX_EXT) {
            exts[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
            have_debug = 1;
        }
    } else {
        rsx_vk_log(d, "VALIDATION_LAYER absent");
    }

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = exts;
    ci.enabledLayerCount = nlayers;
    ci.ppEnabledLayerNames = layers;
    if (g_vk.CreateInstance(&ci, NULL, (VkInstance*)&d->instance) != VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateInstance failed");
        return -1;
    }
    d->validation_enabled = have_debug;
    d->procs.instance_for_device_procs = d->instance;
    /* The instance now exists, so the rest of the table can be resolved. */
    if (load_instance_procs(d) != 0)
        return -1;
    g_vk_loaded = 1;

    if (have_debug) {
        VkDebugUtilsMessengerCreateInfoEXT dci;
        g_vk.CreateDebugUtilsMessengerEXT =
            (PFN_vkCreateDebugUtilsMessengerEXT)(uintptr_t)
                rsx_vk_platform_instance_proc(&d->procs, d->instance,
                                              "vkCreateDebugUtilsMessengerEXT");
        g_vk.DestroyDebugUtilsMessengerEXT =
            (PFN_vkDestroyDebugUtilsMessengerEXT)(uintptr_t)
                rsx_vk_platform_instance_proc(&d->procs, d->instance,
                                              "vkDestroyDebugUtilsMessengerEXT");
        if (!g_vk.CreateDebugUtilsMessengerEXT ||
            !g_vk.DestroyDebugUtilsMessengerEXT) {
            rsx_vk_log(d, "VALIDATION_LAYER: debug utils entry point missing");
            d->validation_enabled = 0;
        } else {
            memset(&dci, 0, sizeof(dci));
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = rsx_vk_debug_cb;
            dci.pUserData = d;
            if (g_vk.CreateDebugUtilsMessengerEXT(
                    (VkInstance)d->instance, &dci, NULL,
                    (VkDebugUtilsMessengerEXT*)&d->debug_messenger) != VK_SUCCESS)
                d->validation_enabled = 0;
        }
        rsx_vk_log(d, "VALIDATION_LAYER present: enabled, any message fails a test");
    }

    if (pick_physical(d) != 0)
        return -1;
    if (fill_properties(d) != 0)
        return -1;
    if (create_device(d) != 0)
        return -1;
    if (load_device_procs(d) != 0)
        return -1;
    if (frames_create(d) != 0)
        return -1;

    if (ring_create(d, &d->ubo_buf, &d->ubo_mem, &d->ubo_map,
                    RSX_VK_UBO_SLAB_BYTES, "ubo slab") != 0)
        return -1;
    if (ring_create(d, &d->geom_buf, &d->geom_mem, &d->geom_map,
                    RSX_VK_GEOM_SLAB_BYTES, "geometry slab") != 0)
        return -1;
    if (ring_create(d, &d->rb_buf, &d->rb_mem, &d->rb_map,
                    RSX_VK_READBACK_SLAB_BYTES, "readback slab") != 0)
        return -1;

    if (layouts_create(d) != 0)
        return -1;

    /* DESIGN §3.1: the 12-entry format table, asked of the device and logged
     * entry by entry, before anything creates an image. R10's depth format is
     * chosen here too, because a depth target needs it and a pipeline needs
     * to know whether it exists. */
    if (rsx_vk_formats_init(d) != 0)
        return -1;

    /* DESIGN §4.2: the texture staging slab. Its own buffer, because the
     * geometry slab is sized for one draw's vertices and a mip chain does not
     * fit that shape. */
    if (ring_create(d, &d->tex_buf, &d->tex_mem, &d->tex_map,
                    RSX_VK_TEX_SLAB_BYTES, "texture slab") != 0)
        return -1;
    {
        VkPhysicalDeviceProperties p;
        g_vk.GetPhysicalDeviceProperties((VkPhysicalDevice)d->physical, &p);
        d->copy_align = p.limits.optimalBufferCopyOffsetAlignment
                            ? (uint32_t)p.limits.optimalBufferCopyOffsetAlignment
                            : 4u;
        rsx_vk_log(d, "limits: optimalBufferCopyOffsetAlignment %u (texture slab)",
                   d->copy_align);
    }
    /* DESIGN §1.1's null texture, before the handle table: it belongs to the
     * device rather than to a slot, and every draw's texture set names it. */
    if (rsx_vk_null_image_create(d) != 0)
        return -1;

    /* DESIGN §1.3: one VkPipelineCache for the device's lifetime (S4 persists
     * it with VK_KHR_pipeline_binary; S2 only has to pass a valid one). */
    {
        VkPipelineCacheCreateInfo pci;
        VkPipelineCache pc = VK_NULL_HANDLE;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        if (g_vk.CreatePipelineCache((VkDevice)d->device, &pci, NULL, &pc) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkCreatePipelineCache failed");
            return -1;
        }
        d->pipeline_cache = (uint64_t)(uintptr_t)pc;
    }
    /* One descriptor pool per device; a set per draw is freed by the frame's
     * fence, and the pool is reset when the device goes idle.
     *
     * The sizes have to cover what write_descriptor_set actually writes into
     * one set, or vkAllocateDescriptorSets reports
     * VK_ERROR_OUT_OF_POOL_MEMORY at some draw in some frame and the draw is
     * dropped with no pixel to show why. Per set:
     *
     *   2   UNIFORM_BUFFER_DYNAMIC   b0, b1
     *   20  SAMPLED_IMAGE            PS t0..t15 + VS t16..t19
     *   20  SAMPLER                  PS s0..s15 + VS s0..s3
     *   0   COMBINED_IMAGE_SAMPLER   the per-draw set writes none of these
     *
     * The old sizes were RSX_VK_MAX_TEX_UNITS (16) for the images and the
     * samplers, four short of the writes, and 20 combined image samplers per
     * set that the draw never uses. The combined-image-sampler size is now
     * zero for the draw's pool: depth_snapshot has its own pool
     * (snap_pool_created) precisely because its set is a different layout. */
    {
        VkDescriptorPoolSize sizes[4];
        VkDescriptorPoolCreateInfo dci;
        VkDescriptorPool dp = VK_NULL_HANDLE;
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[0].descriptorCount = 2u * RSX_VK_SETS_PER_FRAME;
        sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        sizes[1].descriptorCount =
            (RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS) *
            RSX_VK_SETS_PER_FRAME;
        sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        sizes[2].descriptorCount =
            (RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS) *
            RSX_VK_SETS_PER_FRAME;
        sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        /* The draw's own set writes no combined image samplers; the depth
         * snapshot's does, and it has a pool of its own. */
        sizes[3].descriptorCount = 0;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dci.maxSets = RSX_VK_SETS_PER_FRAME;
        dci.poolSizeCount = 4;
        dci.pPoolSizes = sizes;
        if (g_vk.CreateDescriptorPool((VkDevice)d->device, &dci, NULL, &dp) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "vkCreateDescriptorPool failed");
            return -1;
        }
        d->descriptor_pool = (uint64_t)(uintptr_t)dp;
        d->descriptor_pools_created++;
        d->descriptor_pools_alive++;
        rsx_vk_log(d, "descriptor pool: %u sets, %u images + %u samplers each",
                   (unsigned)RSX_VK_SETS_PER_FRAME,
                   (unsigned)(RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS),
                   (unsigned)(RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS));
    }

    /* The one format S1 knows. Checking it here means the
     * test's failure is a format problem, not a mystery at clear time. */
    {
        VkFormatProperties fp;
        g_vk.GetPhysicalDeviceFormatProperties((VkPhysicalDevice)d->physical,
                                               VK_FORMAT_R8G8B8A8_UNORM, &fp);
        rsx_vk_log(d, "R8G8B8A8_UNORM: optimal 0x%08x", fp.optimalTilingFeatures);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
            !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            rsx_vk_log(d, "R8G8B8A8_UNORM lacks COLOR_ATTACHMENT or TRANSFER_SRC");
            return -1;
        }
    }

    /* The handle table. Its size is the walk bound every "iterate the live
     * slots" loop uses (`rsx_vk_handle_capacity`), which is why the loops must
     * not spell a literal instead (item 11).
     *
     * It is 512 rather than the 256 S3b used for a testable reason as well as a
     * practical one: at exactly 256 the capacity and the old `id < 256` literal
     * were the same number, so the S3b defect -- the loops stopping at a
     * constant rather than at the table's size -- could not be observed on any
     * device, and a fix that only changed `256` to
     * `rsx_vk_handle_capacity(d->handles)` would have been untestable. A title
     * with a few hundred live textures, surfaces and views in one frame is
     * ordinary (1024 textures are cached by the engine alone,
     * rsx_draw_engine.h RSX_DRAW_ENGINE_TEXTURE_CACHE), so a table that can
     * only hold 256 is the smaller problem. 512 keeps the table's arrays small
     * and puts the capacity clear of any literal the old code could have
     * matched by accident. */
    d->handles = rsx_vk_handles_create(RSX_VK_HANDLE_CAPACITY);
    if (!d->handles) {
        rsx_vk_log(d, "handle table allocation failed");
        return -1;
    }

    d->initialised = 1;
    rsx_vk_log(d, "init ok: %ux%u (logged only), %s", width, height,
               rsx_vk_platform_driver());
    return 0;
}

void rsx_vk_device_shutdown(rsx_vk_device* d)
{
    VkDevice dev = (VkDevice)d->device;
    VkInstance inst = (VkInstance)d->instance;

    if (!d->initialised && !d->instance && !d->device)
        return;

    if (dev) {
        rsx_vk_handle_destroy_all(d);
        g_vk.DeviceWaitIdle(dev);
    }
    if (d->handles) {
        rsx_vk_handles_destroy(d->handles);
        d->handles = NULL;
    }
    if (dev) {
        layouts_destroy(d);
        /* The sampler cache and the depth_snapshot program are the device's,
         * not any slot's. */
        rsx_vk_samplers_destroy_all(d);
        rsx_vk_null_image_destroy(d);
        if (d->snap_pipeline)
            g_vk.DestroyPipeline(dev, (VkPipeline)d->snap_pipeline, NULL);
        if (d->snap_layout)
            g_vk.DestroyPipelineLayout(dev, (VkPipelineLayout)d->snap_layout,
                                       NULL);
        if (d->snap_pool_created) {
            g_vk.DestroyDescriptorPool(dev, (VkDescriptorPool)d->snap_pool_created,
                                       NULL);
            d->snap_pool_created = 0;
            d->snap_pool_in_use = 0;
            d->descriptor_pools_alive--;
        }
        /* The legacy alias: earlier code named the per-call pool snap_pool. */
        if (d->snap_pool) {
            g_vk.DestroyDescriptorPool(dev, (VkDescriptorPool)d->snap_pool, NULL);
            d->snap_pool = 0;
        }
        if (d->snap_set_layout)
            g_vk.DestroyDescriptorSetLayout(
                dev, (VkDescriptorSetLayout)d->snap_set_layout, NULL);
        d->snap_pipeline = d->snap_layout = d->snap_set_layout = 0;
        d->snap_built = 0;
        rsx_vk_modules_destroy_all(d);
        if (d->descriptor_pool) {
            g_vk.DestroyDescriptorPool(dev, (VkDescriptorPool)d->descriptor_pool,
                                       NULL);
            d->descriptor_pools_alive--;
        }
        if (d->pipeline_cache)
            g_vk.DestroyPipelineCache(dev, (VkPipelineCache)d->pipeline_cache, NULL);
        ring_destroy(d, &d->tex_buf, &d->tex_mem, &d->tex_map);
        ring_destroy(d, &d->rb_buf, &d->rb_mem, &d->rb_map);
        ring_destroy(d, &d->geom_buf, &d->geom_mem, &d->geom_map);
        ring_destroy(d, &d->ubo_buf, &d->ubo_mem, &d->ubo_map);
        frames_destroy(d);
        g_vk.DestroyDevice(dev, NULL);
        d->device = 0;
        d->queue = 0;
    }
    if (inst) {
        if (d->validation_enabled && d->debug_messenger &&
            g_vk.DestroyDebugUtilsMessengerEXT)
            g_vk.DestroyDebugUtilsMessengerEXT(
                inst, (VkDebugUtilsMessengerEXT)d->debug_messenger, NULL);
        d->debug_messenger = 0;
        if (g_vk.DestroyInstance)
            g_vk.DestroyInstance(inst, NULL);
        d->instance = 0;
    }
    rsx_vk_platform_close(d);
    rsx_vk_log(d, "shutdown: vkAllocateMemory %llu, vkFreeMemory %llu",
               (unsigned long long)d->memory_allocs,
               (unsigned long long)d->memory_frees);
    d->initialised = 0;
}
