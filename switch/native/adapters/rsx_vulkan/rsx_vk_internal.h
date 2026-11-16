/*
 * rsx_vk_internal.h - the two halves of the device share the loaded entry
 * points and the memory-type lookup; nothing outside native/adapters/
 * rsx_vulkan/ includes this.
 *
 * Keeping the table in one translation unit and exposing it here is what makes
 * "every proc is checked once at init" true: no file can define its own copy
 * and skip the check.
 */
#ifndef PS3RECOMP_RSX_VK_INTERNAL_H
#define PS3RECOMP_RSX_VK_INTERNAL_H

#include "rsx_vk.h"

#if defined(__GNUC__)
#  define RSX_VK_INTERNAL __attribute__((visibility("hidden")))
#else
#  define RSX_VK_INTERNAL
#endif

/* The loaded entry points, defined in rsx_vk_device.c. */
typedef struct rsx_vk_pfns {
    PFN_vkGetInstanceProcAddr                     GetInstanceProcAddr;
    PFN_vkEnumerateInstanceLayerProperties        EnumerateInstanceLayerProperties;
    PFN_vkCreateInstance                          CreateInstance;
    PFN_vkEnumerateInstanceExtensionProperties    EnumerateInstanceExtensionProperties;
    PFN_vkDestroyInstance                         DestroyInstance;
    PFN_vkEnumeratePhysicalDevices                EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties             GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties  GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceFormatProperties       GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties       GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDebugUtilsMessengerEXT            CreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT           DestroyDebugUtilsMessengerEXT;
    PFN_vkGetDeviceProcAddr                       GetDeviceProcAddr;
    PFN_vkCreateDevice                            CreateDevice;
    PFN_vkDestroyDevice                           DestroyDevice;
    PFN_vkGetDeviceQueue                          GetDeviceQueue;
    PFN_vkDeviceWaitIdle                          DeviceWaitIdle;
    PFN_vkCreateCommandPool                       CreateCommandPool;
    PFN_vkDestroyCommandPool                      DestroyCommandPool;
    PFN_vkAllocateCommandBuffers                  AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers                      FreeCommandBuffers;
    PFN_vkBeginCommandBuffer                      BeginCommandBuffer;
    PFN_vkEndCommandBuffer                        EndCommandBuffer;
    PFN_vkResetCommandBuffer                      ResetCommandBuffer;
    PFN_vkCreateFence                             CreateFence;
    PFN_vkDestroyFence                            DestroyFence;
    PFN_vkResetFences                             ResetFences;
    PFN_vkGetFenceStatus                          GetFenceStatus;
    PFN_vkWaitForFences                           WaitForFences;
    PFN_vkQueueSubmit                             QueueSubmit;
    PFN_vkCreateBuffer                            CreateBuffer;
    PFN_vkDestroyBuffer                           DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements             GetBufferMemoryRequirements;
    PFN_vkAllocateMemory                          AllocateMemory;
    PFN_vkFreeMemory                              FreeMemory;
    PFN_vkBindBufferMemory                        BindBufferMemory;
    PFN_vkMapMemory                               MapMemory;
    PFN_vkUnmapMemory                             UnmapMemory;
    PFN_vkCreateDescriptorSetLayout               CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout              DestroyDescriptorSetLayout;
    PFN_vkCreateImage                             CreateImage;
    PFN_vkDestroyImage                            DestroyImage;
    PFN_vkCreateImageView                         CreateImageView;
    PFN_vkDestroyImageView                        DestroyImageView;
    PFN_vkGetImageMemoryRequirements              GetImageMemoryRequirements;
    PFN_vkBindImageMemory                         BindImageMemory;
    PFN_vkGetPhysicalDeviceFeatures               GetPhysicalDeviceFeatures;
    PFN_vkCmdPipelineBarrier                      CmdPipelineBarrier;
    PFN_vkCmdClearColorImage                      CmdClearColorImage;
    PFN_vkCmdCopyImageToBuffer                    CmdCopyImageToBuffer;

    /* ---- slice S2: shader modules, pipelines, the draw ------------------ */
    PFN_vkCreateShaderModule                      CreateShaderModule;
    PFN_vkDestroyShaderModule                     DestroyShaderModule;
    PFN_vkCreatePipelineLayout                    CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout                   DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines                 CreateGraphicsPipelines;
    PFN_vkDestroyPipeline                         DestroyPipeline;
    PFN_vkCreatePipelineCache                     CreatePipelineCache;
    PFN_vkDestroyPipelineCache                    DestroyPipelineCache;
    PFN_vkCreateDescriptorPool                    CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool                   DestroyDescriptorPool;
    PFN_vkResetDescriptorPool                     ResetDescriptorPool;
    PFN_vkAllocateDescriptorSets                  AllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets                    UpdateDescriptorSets;
    PFN_vkCmdBindPipeline                         CmdBindPipeline;
    PFN_vkCmdBindDescriptorSets                   CmdBindDescriptorSets;
    PFN_vkCmdBeginRenderingKHR                    CmdBeginRenderingKHR;
    PFN_vkCmdEndRenderingKHR                      CmdEndRenderingKHR;
    PFN_vkCmdSetViewport                          CmdSetViewport;
    PFN_vkCmdSetScissor                           CmdSetScissor;
    PFN_vkCmdSetStencilReference                  CmdSetStencilReference;
    PFN_vkCmdBindVertexBuffers                    CmdBindVertexBuffers;
    PFN_vkCmdBindIndexBuffer                      CmdBindIndexBuffer;
    PFN_vkCmdDraw                                 CmdDraw;
    PFN_vkCmdDrawIndexed                          CmdDrawIndexed;

    /* ---- slice S3: textures, targets, depth, samplers ------------------ */
    PFN_vkCreateSampler                           CreateSampler;
    PFN_vkDestroySampler                          DestroySampler;
    PFN_vkGetPhysicalDeviceFormatProperties2      GetPhysicalDeviceFormatProperties2;
    PFN_vkCreateBufferView                        CreateBufferView;
    PFN_vkDestroyBufferView                       DestroyBufferView;
    PFN_vkCmdCopyBufferToImage                    CmdCopyBufferToImage;
    PFN_vkCmdClearDepthStencilImage               CmdClearDepthStencilImage;
    PFN_vkCmdCopyImage                            CmdCopyImage;
} rsx_vk_pfns;

extern rsx_vk_pfns g_vk;

/* Pick a memory type index for `type_bits` satisfying `want_flags`;
 * UINT32_MAX when none. */
uint32_t rsx_vk_memory_type_find(rsx_vk_device* d, uint32_t type_bits,
                                 uint32_t want_flags);

/* The two commands S1 records, on the current frame's command buffer. */
int rsx_vk_cmd_clear_color(rsx_vk_device* d, rsx_vk_image* img,
                           const float rgba[4]);
int rsx_vk_cmd_copy_to_readback(rsx_vk_device* d, rsx_vk_image* img, u32 x,
                                u32 y, u32 w, u32 h, uint32_t row_bytes);

/* ---- slice S2 ------------------------------------------------------------------
 *
 * Device-side state the pipeline, shader and draw code shares. Declared here
 * rather than in rsx_vk.h because none of it is part of the backend's vtable:
 * S3's textures and S4's pass folding grow it too.
 */

/* Descriptor set layouts built by rsx_vk_device.c's layouts_create.
 * set0 carries b0+b1 as UNIFORM_BUFFER_DYNAMIC; the image and sampler layouts
 * are separate per stage because HLSL's t and s register spaces are. */
/*
 * The descriptor set layouts, in the two sets glslang 16.6.0's HLSL front end
 * actually produces (see layouts_create for the spirv-dis evidence):
 *
 *   set 0  t0..t19  COMBINED_IMAGE_SAMPLER  the t-register images, with the
 *                                           s-register sampler of the same
 *                                           unit folded in
 *   set 1  b0, b1   UNIFORM_BUFFER_DYNAMIC  the two constant blocks
 *
 * DESIGN §2.2 predicted set 1 = images and set 2 = samplers; glslang puts a
 * unit's image and its sampler at the same `DescriptorSet 0, Binding N`, which
 * only a combined image sampler can express. REPORT.md records the
 * disagreement and the spirv-dis output that settles it.
 */
typedef struct rsx_vk_layouts {
    uint64_t set0_textures;    /* b0,b1 + t0..t19 at 4..23 + s0..s19 at 32..51 */
    uint64_t set1_constants;   /* the same layout; kept because 1-set is what
                                * glslang emits, and both names must not be
                                * destroyed twice (layouts_destroy says so) */
    /* The vertex stage's images and samplers sit at their own shifts
     * (RSX_VK_VS_IMAGE_BINDING_SHIFT / _SAMPLER_) so they cannot alias PS unit
     * 0; the layout names them explicitly so the pipeline layout, the
     * descriptor writes and rsx_vk_spv_verify all agree. */
} rsx_vk_layouts;

extern rsx_vk_layouts g_layouts;

/* Reserve `bytes` at `alignment` in the vertex/index staging slab and return
 * a host pointer plus the device offset it will be bound at. The S2 draw
 * copies its vertices and indices here. */
void* rsx_vk_stage_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment,
                           uint32_t* out_offset);

/* vkCmdPipelineBarrier to COLOR_ATTACHMENT_OPTIMAL, recorded only when the
 * image is not already there (DESIGN §4.3 rule 4). */
int rsx_vk_cmd_to_color_attachment(rsx_vk_device* d, rsx_vk_image* img);

/* The same for TRANSFER_SRC_OPTIMAL, for a readback that must see the pass. */
int rsx_vk_cmd_to_transfer_src(rsx_vk_device* d, rsx_vk_image* img);

/* Reserve `bytes` at `alignment` in the texture staging slab and
 * return a host pointer, or NULL when the slab cannot take it. */
void* rsx_vk_tex_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment,
                         uint32_t* out_offset);

/* The aspect mask a format's barriers and copies use: depth, depth+stencil,
 * or colour. */
uint32_t rsx_vk_aspect_mask(uint32_t vk_format);

/* True when the format is a depth (or depth/stencil) format. */
int rsx_vk_format_is_depth(uint32_t vk_format);

/* True when this device's chosen format for `be` is filterable linearly
 * (DESIGN §3.1 fallback 2): the sampler clamps to NEAREST when it is not. */
int rsx_vk_format_filterable(const rsx_vk_device* d, rsx_be_format be);

/* Record a copy of host rows into a texture subresource through the staging
 * slab. Leaves the image in TRANSFER_DST_OPTIMAL: the caller records every
 * row set of a frame and then one `to shader read` barrier, so a 6-face cube
 * or a 10-level mip chain is one transition rather than one per level. */
int rsx_vk_cmd_upload_rows(rsx_vk_device* d, rsx_vk_image* img, u32 face,
                           u32 mip, u32 w, u32 h, const void* src,
                           u32 row_bytes, u32 rows);

/* The barrier that ends an upload batch: TRANSFER_WRITE -> the source a
 * sampler reads (SHADER_READ / DEPTH_STENCIL_READ), recorded only when the
 * tracker says the image is not already there (DESIGN §4.3 rule 4). */
int rsx_vk_cmd_upload_done(rsx_vk_device* d, rsx_vk_image* img);

/* True when `be` is a block-compressed format: view dimensions, row lengths
 * and offsets are in blocks for those, in texels for everything else. */
int rsx_vk_format_is_block(const rsx_vk_device* d, rsx_be_format be);

/* The format a handle's own VkImageView is a view of, with a surface view
 * resolved to its parent colour target's format. This is what decides
 * filterability: a surface view's slot carries a copy of its parent's format,
 * so a view built over a BC image would answer with the BC format even though
 * the descriptor is written for the target -- the parent is the authority and
 * this resolves through it. */
uint32_t rsx_vk_handle_format(rsx_vk_device* d, u32 handle);

/* The last recorded value for each component, in the resource's own R,G,B,A
 * order. A one-texel VkClearColorValue is enough: it is applied to every
 * texel of the range, so only the meaning of each component matters. */
void rsx_vk_clear_values(uint32_t vk_format, VkClearColorValue* c);

/* A texture's crossbar as the four VkComponentSwizzle values of its view, in
 * R,G,B,A order. The one translation both texture_create and surface_view go
 * through, so the two cannot disagree about what 0xAA1B means (the regression
 * test_texture_layout.c documents at rsx_texture_layout.h:205-212 is exactly
 * that disagreement). */
void rsx_vk_crossbar_swizzle(u32 remap, u32 rsx_fmt, uint32_t out[4]);

/* ---- the draw's pass (rsx_vk_pipeline.c) --------------------------------- */

/* True when a pass is open on this command buffer. */
int rsx_vk_is_rendering(const rsx_vk_device* d);

#endif /* PS3RECOMP_RSX_VK_INTERNAL_H */
