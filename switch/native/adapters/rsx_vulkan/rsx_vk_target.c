/*
 * ps3recomp - Vulkan backend: formats, images, colour targets, clears and
 * readback (DESIGN §3.1 and §1.2; S1 created the first of these, S3 the rest)
 *
 * Three things live here because they are all "the image and what it means":
 *
 *  - the 12-entry format table of DESIGN §3.1, asked of the physical device at
 *    init and logged choice by choice, with the four fallbacks of that section
 *    applied per entry (rsx_vk_formats_init);
 *  - one image constructor (rsx_vk_image_create) that every target, texture
 *    and snapshot result goes through, so the subresource range, the
 *    cube-compatibility flag and the component mapping cannot drift between
 *    the four callers;
 *  - the v1 clears and readback, unchanged in meaning: S1 records a colour
 *    clear as a direct vkCmdClearColorImage and the pass that follows loads
 *    rather than clears, which is what keeps the two from undoing each other.
 *
 * clear_color is recorded as a direct vkCmdClearColorImage, not as a
 * dynamic-rendering pass with LOAD_OP_CLEAR. Both were allowed by the S1 task;
 * the direct clear is the one that needs no render pass, no attachment info
 * and no pipeline at all, which is what made S1 a device test rather than a
 * draw test. When pass folding arrives (S4) a clear that precedes a draw on
 * the same target becomes a load op and this becomes its own pass
 * records the choice.
 *
 * The vkCmd* entry points live in rsx_vk_device.c's private table, so the
 * commands below go through the rsx_vk_cmd_* helpers that file exports.
 */
#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* the format table */

/*
 * One row per rsx_be_format: the VkFormat of §3.1, and the two halves of its
 * column pair. `attachable` is the table's T/A column read back as a check
 * rather than a promise: a format the guest can never bind as a surface is
 * listed 0, and asking for one is a logged refusal (fallback 1), which cannot
 * arise from the engine because eng_surface_format never returns BC.
 */
typedef struct rsx_vk_fmt_row {
    rsx_be_format be;
    uint32_t      fmt;
    int           attachable;
    const char*   name;
} rsx_vk_fmt_row;

static const rsx_vk_fmt_row k_format_rows[] = {
    { RSX_BE_FMT_R8,             VK_FORMAT_R8_UNORM,               1, "R8"             },
    { RSX_BE_FMT_R8G8,           VK_FORMAT_R8G8_UNORM,             1, "R8G8"           },
    { RSX_BE_FMT_R8G8B8A8,       VK_FORMAT_R8G8B8A8_UNORM,         1, "R8G8B8A8"       },
    { RSX_BE_FMT_BC1,            VK_FORMAT_BC1_RGBA_UNORM_BLOCK,   0, "BC1"            },
    { RSX_BE_FMT_BC2,            VK_FORMAT_BC2_UNORM_BLOCK,        0, "BC2"            },
    { RSX_BE_FMT_BC3,            VK_FORMAT_BC3_UNORM_BLOCK,        0, "BC3"            },
    { RSX_BE_FMT_R16,            VK_FORMAT_R16_UNORM,              1, "R16"            },
    { RSX_BE_FMT_R16G16,         VK_FORMAT_R16G16_UNORM,           1, "R16G16"         },
    { RSX_BE_FMT_R16G16F,        VK_FORMAT_R16G16_SFLOAT,          1, "R16G16F"        },
    { RSX_BE_FMT_R16G16B16A16F,  VK_FORMAT_R16G16B16A16_SFLOAT,    1, "R16G16B16A16F"  },
    { RSX_BE_FMT_R32F,           VK_FORMAT_R32_SFLOAT,             1, "R32F"           },
    { RSX_BE_FMT_R32G32B32A32F,  VK_FORMAT_R32G32B32A32_SFLOAT,    1, "R32G32B32A32F"  },
};

uint32_t rsx_vk_format(rsx_vk_device* d, rsx_be_format fmt)
{
    if (d && fmt > RSX_BE_FMT_NONE && fmt < RSX_BE_FMT_NONE + RSX_VK_FMT_COUNT &&
        d->format_table[fmt])
        return d->format_table[fmt];
    /* A format with no usable VkFormat is a logged 0, never a silent
     * substitution: a wrong format here is a wrong-coloured game later. */
    if (d && fmt != RSX_BE_FMT_NONE)
        rsx_vk_log(d, "format %d has no usable VkFormat on this device",
                   (int)fmt);
    return 0;
}

int rsx_vk_format_filterable(const rsx_vk_device* d, rsx_be_format be)
{
    if (!d || be <= RSX_BE_FMT_NONE || be >= RSX_BE_FMT_NONE + RSX_VK_FMT_COUNT)
        return 0;
    return (d->format_features[be] & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
               ? 1
               : 0;
}

int rsx_vk_format_is_block(const rsx_vk_device* d, rsx_be_format be)
{
    uint32_t f;
    if (!d || be <= RSX_BE_FMT_NONE || be >= RSX_BE_FMT_NONE + RSX_VK_FMT_COUNT)
        return 0;
    f = d->format_table[be];
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
        return 1;
    default:
        return 0;
    }
}

int rsx_vk_format_is_depth(uint32_t vk_format)
{
    switch (vk_format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

uint32_t rsx_vk_aspect_mask(uint32_t vk_format)
{
    switch (vk_format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/*
 * DESIGN §3.2 / R10 / Decision 28: one depth format for every zeta.
 *
 * The guest zeta is Z24S8, which D24_UNORM_S8_UINT matches exactly, but R10 is
 * explicit that support for it as a *sampled* depth image is not established
 * for Maxwell/NVK by anything read. The decision taken there is D32 by
 * default; the choice is still made from the format properties rather than at
 * compile time, and logged once, because the same code runs on lavapipe, on a
 * desktop GPU and on the Switch.
 */
static int depth_format_choose(rsx_vk_device* d)
{
    static const VkFormat candidates[2] = { VK_FORMAT_D32_SFLOAT_S8_UINT,
                                            VK_FORMAT_D24_UNORM_S8_UINT };
    const VkFormatFeatureFlags need =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkPhysicalDevice pdev = (VkPhysicalDevice)d->physical;
    uint32_t i;

    for (i = 0; i < 2; i++) {
        VkFormatProperties fp;
        memset(&fp, 0, sizeof fp);
        g_vk.GetPhysicalDeviceFormatProperties(pdev, candidates[i], &fp);
        rsx_vk_log(d, "depth candidate 0x%x: optimal 0x%08x, need 0x%08x",
                   (unsigned)candidates[i], fp.optimalTilingFeatures, need);
        if ((fp.optimalTilingFeatures & need) == need) {
            d->depth_format = (uint32_t)candidates[i];
            d->depth_stencil_format = d->depth_format;
            rsx_vk_log(d, "depth format: 0x%x (Decision 28 default; sampled depth "
                       "supported)", (unsigned)d->depth_format);
            return 0;
        }
    }
    rsx_vk_log(d, "STOP: no depth/stencil format with DEPTH_STENCIL_ATTACHMENT "
               "and SAMPLED; R10 lists no further fallback");
    return -1;
}

int rsx_vk_formats_init(rsx_vk_device* d)
{
    const uint32_t n = sizeof(k_format_rows) / sizeof(k_format_rows[0]);
    VkPhysicalDevice pdev = (VkPhysicalDevice)d->physical;
    void* f2 = (void*)g_vk.GetPhysicalDeviceFormatProperties2;
    uint32_t i;

    memset(d->format_table, 0, sizeof d->format_table);
    memset(d->format_features, 0, sizeof d->format_features);

    for (i = 0; i < n; i++) {
        const rsx_vk_fmt_row* r = &k_format_rows[i];
        VkFormatFeatureFlags f;
        if (f2) {
            VkFormatProperties2 p2;
            memset(&p2, 0, sizeof p2);
            p2.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
            g_vk.GetPhysicalDeviceFormatProperties2(pdev, (VkFormat)r->fmt, &p2);
            f = p2.formatProperties.optimalTilingFeatures;
        } else {
            VkFormatProperties fp;
            memset(&fp, 0, sizeof fp);
            g_vk.GetPhysicalDeviceFormatProperties(pdev, (VkFormat)r->fmt, &fp);
            f = fp.optimalTilingFeatures;
        }
        if (f == 0) {
            /* The device does not know the format at all: 0 says so, and a
             * consumer falls back to the null texture (DESIGN §3.1
             * fallback 4). */
            rsx_vk_log(d, "format %-14s 0x%-3x UNSUPPORTED (no features); the "
                       "table entry stays 0", r->name, (unsigned)r->fmt);
            d->format_fallbacks++;
            continue;
        }
        d->format_table[r->be] = r->fmt;
        d->format_features[r->be] = (uint32_t)f;
        rsx_vk_log(d, "format %-14s -> 0x%-3x features 0x%08x%s",
                   r->name, (unsigned)r->fmt, f,
                   (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                       ? ""
                       : " (not linearly filterable: samplers clamp to NEAREST)");
        if (r->attachable &&
            !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            rsx_vk_log(d, "format %-14s is not a colour attachment here", r->name);
            d->format_fallbacks++;
        }
        if (r->attachable &&
            !(f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
            rsx_vk_log(d, "format %-14s has no COLOR_ATTACHMENT_BLEND: blending "
                       "is forced off for it (DESIGN 3.1 fallback 3)", r->name);
    }

    /* The minimum set: an R8G8B8A8 colour target (the engine's own surface
     * format) and one sampled depth format. */
    if (!d->format_table[RSX_BE_FMT_R8G8B8A8] ||
        !(d->format_features[RSX_BE_FMT_R8G8B8A8] &
          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        rsx_vk_log(d, "STOP: R8G8B8A8_UNORM is not a colour attachment here");
        return -1;
    }
    if (depth_format_choose(d) != 0)
        return -1;
    return 0;
}

/* ---- images ------------------------------------------------------------ */

/*
 * Every image the backend creates. `mips`/`faces` are the subresource shape a
 * texture needs; a colour target and a depth target are 1x1 there. `mapping`
 * is the crossbar a texture's view wears; NULL means identity.
 *
 * The view's subresource range covers every mip and layer of the image rather
 * than level 0 / layer 0 of it: a 2x2 texture with one level is the same
 * image either way, so there is no second code path for the one-mip case.
 */
int rsx_vk_image_create(rsx_vk_device* d, rsx_be_format be, uint32_t fmt,
                        uint32_t w, uint32_t h, uint32_t usage,
                        rsx_vk_image* out)
{
    return rsx_vk_image_create_ex(d, be, fmt, w, h, usage, 1, 1, NULL, out);
}

int rsx_vk_image_create_ex(rsx_vk_device* d, rsx_be_format be, uint32_t fmt,
                           uint32_t w, uint32_t h, uint32_t usage, uint32_t mips,
                           uint32_t faces, const void* component_mapping,
                           rsx_vk_image* out)
{
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    VkImageViewCreateInfo vci;
    VkDevice dev = (VkDevice)d->device;
    const VkComponentMapping* mapping = (const VkComponentMapping*)component_mapping;
    uint32_t type;

    memset(out, 0, sizeof(*out));
    out->layout = -1;
    if (!w || !h || !fmt || !mips || !faces)
        return -1;

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = (VkFormat)fmt;
    ici.extent.width = w;
    ici.extent.height = h;
    ici.extent.depth = 1;
    ici.mipLevels = mips;
    ici.arrayLayers = faces;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = (VkImageUsageFlags)usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* DESIGN §3.4: a cube view must come from a cube-compatible 2D array, not
     * from VK_IMAGE_TYPE_CUBE (which Vulkan does not have). */
    if (faces == 6)
        ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (g_vk.CreateImage(dev, &ici, NULL, (VkImage*)&out->image) != VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateImage %ux%u fmt 0x%x mips %u faces %u failed", w,
                   h, fmt, mips, faces);
        return -1;
    }

    g_vk.GetImageMemoryRequirements(dev, (VkImage)out->image, &req);
    type = rsx_vk_memory_type_find(d, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
        rsx_vk_log(d, "no device-local memory type for the image");
        g_vk.DestroyImage(dev, (VkImage)out->image, NULL);
        out->image = 0;
        return -1;
    }
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (g_vk.AllocateMemory(dev, &ai, NULL, (VkDeviceMemory*)&out->memory) != VK_SUCCESS) {
        rsx_vk_log(d, "vkAllocateMemory for the image failed");
        g_vk.DestroyImage(dev, (VkImage)out->image, NULL);
        out->image = 0;
        return -1;
    }
    d->memory_allocs++;
    if (g_vk.BindImageMemory(dev, (VkImage)out->image, (VkDeviceMemory)out->memory, 0) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "vkBindImageMemory failed");
        rsx_vk_image_destroy(d, out);
        return -1;
    }

    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = (VkImage)out->image;
    if (faces == 6)
        vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    else if (faces > 1)
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    else
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)fmt;
    /* A NULL mapping means the view wears identity, which is what a colour or
     * depth target wants; only a texture's crossbar ever differs. */
    if (mapping && !(mapping->r == VK_COMPONENT_SWIZZLE_IDENTITY &&
                     mapping->g == VK_COMPONENT_SWIZZLE_IDENTITY &&
                     mapping->b == VK_COMPONENT_SWIZZLE_IDENTITY &&
                     mapping->a == VK_COMPONENT_SWIZZLE_IDENTITY)) {
        vci.components = *mapping;
    } else {
        vci.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY,
                                              VK_COMPONENT_SWIZZLE_IDENTITY};
    }
    vci.subresourceRange.aspectMask = rsx_vk_aspect_mask(fmt);
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = faces;
    if (g_vk.CreateImageView(dev, &vci, NULL, (VkImageView*)&out->view) != VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateImageView failed");
        rsx_vk_image_destroy(d, out);
        return -1;
    }

    out->format = fmt;
    out->width = w;
    out->height = h;
    out->be_format = be;
    out->mips = mips;
    out->faces = faces;
    /* The aspects this image actually has, latched once so no barrier has to
     * re-derive them from the VkFormat (item N1). */
    out->aspect_mask = rsx_vk_aspect_mask(fmt);
    out->swizzle[0] = mapping ? (uint32_t)mapping->r : (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY;
    out->swizzle[1] = mapping ? (uint32_t)mapping->g : (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY;
    out->swizzle[2] = mapping ? (uint32_t)mapping->b : (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY;
    out->swizzle[3] = mapping ? (uint32_t)mapping->a : (uint32_t)VK_COMPONENT_SWIZZLE_IDENTITY;
    out->identity = (mapping && (vci.components.r != VK_COMPONENT_SWIZZLE_IDENTITY ||
                                 vci.components.g != VK_COMPONENT_SWIZZLE_IDENTITY ||
                                 vci.components.b != VK_COMPONENT_SWIZZLE_IDENTITY ||
                                 vci.components.a != VK_COMPONENT_SWIZZLE_IDENTITY))
                        ? 0u
                        : 1u;
    return 0;
}

void rsx_vk_image_destroy(rsx_vk_device* d, rsx_vk_image* img)
{
    VkDevice dev = (VkDevice)d->device;
    if (!img || !dev)
        return;
    if (img->view) {
        g_vk.DestroyImageView(dev, (VkImageView)img->view, NULL);
        img->view = 0;
    }
    if (img->image) {
        g_vk.DestroyImage(dev, (VkImage)img->image, NULL);
        img->image = 0;
    }
    if (img->memory) {
        g_vk.FreeMemory(dev, (VkDeviceMemory)img->memory, NULL);
        d->memory_frees++;
        img->memory = 0;
    }
    img->layout = -1;
}

int32_t rsx_vk_image_layout(const rsx_vk_image* img)
{
    return img ? img->layout : -1;
}

void rsx_vk_image_set_layout(rsx_vk_image* img, int32_t layout)
{
    if (img)
        img->layout = layout;
}

/* The tracked layout of whatever image a handle names, and whether the handle
 * is a depth target. A surface view answers with its parent's image (it has
 * none of its own), which is the image its descriptor is actually written
 * from. -1 for a handle that owns no image. */
int32_t rsx_vk_handle_layout(rsx_vk_device* d, u32 handle, int* out_is_depth)
{
    rsx_vk_slot* slot;
    if (out_is_depth)
        *out_is_depth = 0;
    if (!d)
        return -1;
    slot = rsx_vk_handle_get(d->handles, handle);
    if (!slot)
        return -1;
    switch (slot->kind) {
    case RSX_VK_SLOT_DEPTH:
        if (out_is_depth)
            *out_is_depth = 1;
        return rsx_vk_image_layout(&slot->u.depth.image);
    case RSX_VK_SLOT_TEXTURE:
        return rsx_vk_image_layout(&slot->u.texture.image);
    case RSX_VK_SLOT_COLOR_TARGET:
        return rsx_vk_image_layout(&slot->u.color);
    case RSX_VK_SLOT_SURFACE_VIEW: {
        rsx_vk_slot* parent =
            rsx_vk_handle_get(d->handles, slot->u.color.view_of);
        return (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
                   ? rsx_vk_image_layout(&parent->u.color)
                   : rsx_vk_image_layout(&slot->u.color);
    }
    default:
        return -1;
    }
}

/* ---- the handle table's destroy path ----------------------------------- */

/* Destroy whatever a slot names. The handle table calls this at the
 * fence-passed boundary and at shutdown (rsx_vk_handle.c). */
void rsx_vk_slot_destroy(rsx_vk_device* d, rsx_vk_slot* slot)
{
    if (!d || !slot)
        return;
    switch (slot->kind) {
    case RSX_VK_SLOT_COLOR_TARGET:
        rsx_vk_image_destroy(d, &slot->u.color);
        break;
    case RSX_VK_SLOT_TEXTURE:
        rsx_vk_image_destroy(d, &slot->u.texture.image);
        break;
    case RSX_VK_SLOT_DEPTH:
        /* The snapshot published from this zeta is its own texture slot and
         * outlives it: the engine can still be sampling it when the zeta is
         * reallocated, and both die at the same fence-passed boundary. */
        if (slot->u.depth.depth_view && d->device) {
            g_vk.DestroyImageView((VkDevice)d->device,
                                  (VkImageView)slot->u.depth.depth_view, NULL);
            slot->u.depth.depth_view = 0;
        }
        rsx_vk_image_destroy(d, &slot->u.depth.image);
        break;
    case RSX_VK_SLOT_SURFACE_VIEW:
        /* A view owns no image and no memory, only its VkImageView. */
        if (slot->u.color.view && d->device) {
            g_vk.DestroyImageView((VkDevice)d->device,
                                  (VkImageView)slot->u.color.view, NULL);
        }
        slot->u.color.view = 0;
        break;
    case RSX_VK_SLOT_PIPELINE:
        /* The S2 pipeline and the layout it owns; the shader modules belong to
         * the backend-wide cache and outlive every pipeline. */
        rsx_vk_pipeline_destroy(d, &slot->u.pipeline);
        break;
    case RSX_VK_SLOT_NONE:
    default:
        break;
    }
    memset(slot, 0, sizeof(*slot));
    slot->u.color.layout = -1;
}

/* ---- clear + readback through the vtable -------------------------------- */

int rsx_vk_target_clear(rsx_vk_device* d, u32 surface, const float rgba[4])
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, surface);
    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "clear_color: surface %u is not a colour target", surface);
        return -1;
    }
    if (d->rendering && d->rendering_target == (uint64_t)surface)
        rsx_vk_pass_end(d);
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    return rsx_vk_cmd_clear_color(d, &slot->u.color, rgba);
}

/*
 * Read a rectangle of a colour target back to the host.
 *
 * The row width is the only thing S3 adds here: a target is no longer always
 * four bytes a texel. The engine's own readback is always R8G8B8A8, but a test
 * that clears an R16G16B16A16F HDR target and reads it back is the S3
 * acceptance for the wide formats, so the texel size comes from the format.
 */
static uint32_t texel_bytes_for(uint32_t vk_format)
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

int rsx_vk_target_readback(rsx_vk_device* d, u32 surface, u32 x, u32 y, u32 w,
                           u32 h, void* out, u32 out_pitch)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, surface);
    uint32_t row_bytes;
    const uint8_t* src;
    uint8_t* dst;
    u32 row;

    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "readback: surface %u is not a colour target", surface);
        return -1;
    }
    if (!out || !w || !h) {
        rsx_vk_log(d, "readback: empty rect or null destination");
        return -1;
    }
    if (x + w > slot->u.color.width || y + h > slot->u.color.height) {
        rsx_vk_log(d, "readback: %ux%u at %u,%u is outside a %ux%u target", w, h,
                   x, y, slot->u.color.width, slot->u.color.height);
        return -1;
    }
    /* The engine's copy-out uses this pitch; honouring it is the contract at
     * rsx_draw_engine.h:258-262. Smaller than a row would truncate the row. */
    row_bytes = w * texel_bytes_for(slot->u.color.format);
    if (out_pitch && out_pitch < row_bytes) {
        rsx_vk_log(d, "readback: out_pitch %u is smaller than a %u-byte row",
                   out_pitch, row_bytes);
        return -1;
    }

    /* A pass still open on this target would see its layout changed under it;
     * S3's pass scope is one draw, so there is at most one to close. */
    if (d->rendering && d->rendering_target == (uint64_t)surface)
        rsx_vk_pass_end(d);
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    if (rsx_vk_cmd_copy_to_readback(d, &slot->u.color, x, y, w, h, row_bytes) !=
        0)
        return -1;
    if (rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE) != 0)
        return -1;

    src = (const uint8_t*)rsx_vk_readback_slab(d, row_bytes * h);
    dst = (uint8_t*)out;
    if (!src) {
        rsx_vk_log(d, "readback: %u bytes do not fit the readback slab",
                   row_bytes * h);
        return -1;
    }
    /* Tightly packed rows out of vkCmdCopyImageToBuffer, copied
     * row by row so the caller's pitch is respected. */
    for (row = 0; row < h; row++)
        memcpy(dst + (size_t)row * (out_pitch ? out_pitch : row_bytes),
               src + (size_t)row * row_bytes, row_bytes);
    rsx_vk_log(d, "readback: surface %u, %ux%u at %u,%u, pitch %u", surface, w, h,
               x, y, out_pitch ? out_pitch : row_bytes);
    return 0;
}

/* ---- the crossbar ------------------------------------------------------- */

void rsx_vk_crossbar_swizzle(u32 remap, u32 rsx_fmt, uint32_t out[4])
{
    u8 c[4];
    uint32_t sw[4];
    int i;

    /* rsx_texture_component_remap writes A,R,G,B -- the crossbar's own field
     * order, LSB first (rsx_texture_layout.h:196-198) -- and its selectors are
     * 0..3 for the resource's own components plus the two constants
     * RSX_REMAP_ZERO/RSX_REMAP_ONE. A VkComponentMapping names R,G,B,A, and
     * VK_COMPONENT_SWIZZLE_R.._A are 0..3, so a selector of n is
     * SWIZZLE_(R+n): rsx_texture_decode lays every format out R,G,B,A, which
     * is what makes that true rather than a coincidence
     * (rsx_texture_layout.h:184-188). */
    rsx_texture_component_remap(remap, rsx_fmt, c);
    for (i = 0; i < 4; i++) {
        if (c[i] == RSX_REMAP_ZERO)
            sw[i] = (uint32_t)VK_COMPONENT_SWIZZLE_ZERO;
        else if (c[i] == RSX_REMAP_ONE)
            sw[i] = (uint32_t)VK_COMPONENT_SWIZZLE_ONE;
        else
            sw[i] = (uint32_t)VK_COMPONENT_SWIZZLE_R + (uint32_t)(c[i] & 3u);
    }
    /* Field order A,R,G,B -> view order R,G,B,A. */
    out[0] = sw[1];
    out[1] = sw[2];
    out[2] = sw[3];
    out[3] = sw[0];
}

/* ---- clear values ------------------------------------------------------- */

/*
 * vkCmdClearColorImage takes a VkClearColorValue applied to every texel of the
 * range, so what matters is which component of the resource each field names.
 * The VkClearColorValue is a union of three arrays in R,G,B,A order, and the
 * float32 member is the one every wide format reads, so a colour target of any
 * of the 12 formats clears from the same three channels.
 */
void rsx_vk_clear_values(uint32_t vk_format, VkClearColorValue* c)
{
    switch (vk_format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R32_SFLOAT:
        /* One real channel: a 2D image with no alpha. Everything else is 0. */
        c->float32[0] = 1.0f;
        c->float32[1] = 0.0f;
        c->float32[2] = 0.0f;
        c->float32[3] = 1.0f;
        break;
    default:
        c->float32[0] = 0.0f;
        c->float32[1] = 0.0f;
        c->float32[2] = 0.0f;
        c->float32[3] = 1.0f;
        break;
    }
}
