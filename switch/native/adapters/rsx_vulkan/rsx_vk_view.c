/*
 * ps3recomp - Vulkan backend: colour targets, surface views, depth targets and
 * depth snapshots (DESIGN §1.2, §1.5, §1.6, §3.1, §3.2, §4.4; slice S3)
 *
 * The five target-side members of the vtable that S1 left as a stub:
 *
 *   color_target_create   every rsx_be_format of DESIGN §3.1, plus the seed
 *                         upload the engine already hands over for an
 *                         R8G8B8A8 surface a title CPU-initialised
 *                         (rsx_draw_engine.c:479-510, :530-545);
 *   surface_view          a second VkImageView over a target's image wearing
 *                         the texture unit's crossbar, cached per
 *                         (surface, remap, format);
 *   depth_target_create   D32_SFLOAT_S8_UINT, chosen at init from the device's
 *                         format properties (Decision 28, DESIGN R10);
 *   depth_snapshot        R32F rendered from the depth image by a full-screen
 *                         triangle -- Vulkan has no depth-to-colour blit and
 *                         DESIGN §4.4 says not to try one;
 *   clear_depth_stencil   folded into the next pass's depth load op, or
 *                         recorded directly when the zeta is not being drawn.
 *
 * Why a clear is a load op here and a direct vkCmdClearColorImage for colour:
 * a depth clear and the depth test that follows it are ordered by the pass
 * and a vkCmdClearDepthStencilImage would need the image out of
 * the depth-attachment layout, which is one more transition per clear on the
 * device this is for. The colour clear keeps S1's form because the engine's
 * own clear_color is the visible-pixels path that S1 proved.
 */
#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* The image a handle names, or NULL. A surface view resolves to its parent's
 * colour target, which is what a caller asking "what format is this?" wants. */
rsx_vk_image* rsx_vk_handle_image(rsx_vk_device* d, u32 handle)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, handle);
    if (!slot)
        return NULL;
    switch (slot->kind) {
    case RSX_VK_SLOT_TEXTURE:
        return &slot->u.texture.image;
    case RSX_VK_SLOT_DEPTH:
        return &slot->u.depth.image;
    case RSX_VK_SLOT_COLOR_TARGET:
        return &slot->u.color;
    case RSX_VK_SLOT_SURFACE_VIEW:
        /* A surface view owns no image: it is a second VkImageView over its
         * parent colour target's VkImage. Layout is a property of the image,
         * not of the view, so the parent is the thing a caller asking "where is
         * this image now?" has to be answered with -- otherwise sampling a
         * rendered surface went through a NULL here and the transition (item 6)
         * never happened. */
        {
            rsx_vk_slot* parent =
                rsx_vk_handle_get(d->handles, slot->u.color.view_of);
            if (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
                return &parent->u.color;
        }
        return NULL;
    default:
        return NULL;
    }
}

/*
 * The VkImageView and VkFormat a descriptor write wants. A surface view
 * carries its own swizzled view; every other sampleable handle carries the
 * view its image was created with. A surface view's parent is named by
 * `view_of`, which is why the view resolves through two slots.
 */
int rsx_vk_handle_view(rsx_vk_device* d, u32 handle, uint64_t* out_view,
                       uint32_t* out_format)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, handle);
    if (!slot)
        return 0;
    switch (slot->kind) {
    case RSX_VK_SLOT_TEXTURE:
        if (!slot->u.texture.image.view)
            return 0;
        *out_view = slot->u.texture.image.view;
        *out_format = slot->u.texture.image.format;
        return 1;
    case RSX_VK_SLOT_DEPTH:
        /* The DEPTH-only view (item 12), not the DEPTH|STENCIL attachment view
         * (item N2). A depth target bound to a texture unit is sampled for its
         * depth, and the descriptor declares the layout the snapshot barrier
         * puts the image in; handing back the attachment view made the sampled
         * view cover stencil too, so the descriptor's aspect and the barrier's
         * aspect disagreed for one and the same image. */
        if (!slot->u.depth.depth_view)
            return 0;
        *out_view = slot->u.depth.depth_view;
        *out_format = slot->u.depth.image.format;
        return 1;
    case RSX_VK_SLOT_COLOR_TARGET:
        if (!slot->u.color.view)
            return 0;
        *out_view = slot->u.color.view;
        *out_format = slot->u.color.format;
        return 1;
    case RSX_VK_SLOT_SURFACE_VIEW:
        if (!slot->u.color.view)
            return 0;
        *out_view = slot->u.color.view;
        *out_format = slot->u.color.format;
        return 1;
    default:
        return 0;
    }
}

/* Every aspect the image behind a handle has. A surface view answers with its
 * parent target's aspects: it owns no image, and the image is what a barrier
 * and a descriptor have to agree about. */
uint32_t rsx_vk_handle_aspect_mask(rsx_vk_device* d, u32 handle)
{
    rsx_vk_slot* slot;
    if (!d)
        return 0;
    slot = rsx_vk_handle_get(d->handles, handle);
    if (!slot)
        return 0;
    switch (slot->kind) {
    case RSX_VK_SLOT_TEXTURE:
        return slot->u.texture.image.aspect_mask;
    case RSX_VK_SLOT_DEPTH:
        return slot->u.depth.image.aspect_mask;
    case RSX_VK_SLOT_COLOR_TARGET:
        return slot->u.color.aspect_mask;
    case RSX_VK_SLOT_SURFACE_VIEW: {
        rsx_vk_slot* parent =
            rsx_vk_handle_get(d->handles, slot->u.color.view_of);
        if (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
            return parent->u.color.aspect_mask;
        return slot->u.color.aspect_mask;
    }
    default:
        return 0;
    }
}

/* The format a handle's own view is a view of. A surface view's slot carries a
 * copy of its parent's format, but the parent is the authority: a view over a
 * BC image would otherwise answer with the BC format even when the descriptor
 * is written for the target, and that is what filterability (DESIGN 3.1
 * fallback 2) decides on. */
uint32_t rsx_vk_handle_format(rsx_vk_device* d, u32 handle)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, handle);
    if (!slot)
        return 0;
    if (slot->kind == RSX_VK_SLOT_SURFACE_VIEW) {
        rsx_vk_slot* parent = rsx_vk_handle_get(d->handles, slot->u.color.view_of);
        if (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
            return parent->u.color.format;
    }
    return slot->u.color.format;
}

/*
 * Move an image to SHADER_READ_ONLY_OPTIMAL for a sampled read.
 *
 * The descriptor write declares SHADER_READ_ONLY_OPTIMAL for every sampled
 * image (rsx_vk_pipeline.c's write_descriptor_set), but a colour target is in
 * COLOR_ATTACHMENT_OPTIMAL after the pass that rendered into it, and nothing
 * transitioned it back. Sampling such a target -- render-to-texture, which the
 * engine does the moment a title reads its own framebuffer -- was therefore an
 * image-layout mismatch: undefined reads on a lenient driver and a validation
 * error on a strict one.
 *
 * Recorded in stream order from wherever the tracker says the image is, so it
 * only emits a barrier when one is needed (DESIGN 4.3 rule 4), and
 * to_color_attachment does the return leg when the image is next an
 * attachment.
 */
int rsx_vk_cmd_to_sampled_read(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    const int32_t want = (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    int32_t from;

    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) == want)
        return 0;
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    from = rsx_vk_image_layout(img);
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    /* The source stage has to cover every way the image can have been written:
     * a colour pass, a transfer (an upload), or an earlier shader read. */
    b.srcAccessMask = (from < 0)
                          ? 0u
                          : (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = rsx_vk_aspect_mask(img->format);
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = img->mips ? img->mips : 1u;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = img->faces ? img->faces : 1u;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

/*
 * A DEPTH image named by a texture unit, transitioned to the layout its
 * descriptor will declare (item M2).
 *
 * Every other sampled image goes to SHADER_READ_ONLY_OPTIMAL, but a depth image
 * may not be read in that layout at all: Vulkan only allows a depth sampler to
 * read an image in DEPTH_STENCIL_READ_ONLY_OPTIMAL (or GENERAL). Until this
 * existed, begin_sampled_read skipped depth handles and bind_sampled_read
 * declared SHADER_READ_ONLY for their view anyway, so the descriptor named a
 * layout the image was never in and the driver was reading a depth image out of
 * a layout no transition ever produced.
 *
 * Both aspects move, because both aspects are what the image's layout is
 * (`img->aspect_mask`, latched once from the format): moving DEPTH alone would
 * leave STENCIL in the attachment layout while the tracker claims the whole
 * image is readable, which is item N1 all over again.
 *
 * The source access mask covers the ways a zeta can have been written (a depth
 * pass) and the way it can have been read before (the snapshot's own sampler),
 * and the source stage is ALL_COMMANDS for the same reason
 * rsx_vk_cmd_to_sampled_read's is: the previous writer may have been any stage.
 */
int rsx_vk_cmd_to_depth_sampled_read(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    const int32_t want =
        (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    int32_t from;

    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) == want)
        return 0;
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    from = rsx_vk_image_layout(img);
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = (from < 0)
                          ? 0u
                          : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_MEMORY_WRITE_BIT);
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = img->aspect_mask;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = img->mips ? img->mips : 1u;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = img->faces ? img->faces : 1u;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

/*
 * The depth-only form for depth_snapshot.
 *
 * A D32_SFLOAT_S8_UINT image has two aspects, and the snapshot's shader reads
 * only depth. Transitioning DEPTH|STENCIL to SHADER_READ_ONLY while the
 * descriptor's view is DEPTH-only makes the two disagree about the aspect; the
 * snapshot's own view (built by rsx_vk_depth_snapshot) covers DEPTH alone, so
 * the barrier and the view must both say DEPTH.
 */
int rsx_vk_cmd_to_sampled_read_depth(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    const int32_t want = (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    int32_t from;

    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) == want)
        return 0;
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    from = rsx_vk_image_layout(img);
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = (from < 0)
                          ? 0u
                          : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

/* colour targets */

u32 rsx_vk_color_target_create(rsx_vk_device* d, rsx_be_format fmt, u32 w,
                               u32 h, const void* seed, u32 seed_row_bytes)
{
    rsx_vk_slot* slot = NULL;
    rsx_vk_image* img;
    uint32_t vfmt;
    u32 id;

    if (!d->device) {
        rsx_vk_log(d, "color_target_create before init");
        return 0;
    }
    vfmt = rsx_vk_format(d, fmt);
    if (!vfmt) {
        rsx_vk_log(d, "color_target_create: format %d has no usable VkFormat",
                   (int)fmt);
        return 0;
    }
    /* DESIGN §3.1 fallback 1: a sampled-only format (BC1-3) as an attachment
     * cannot arise from the engine (eng_surface_format never returns BC), and
     * is refused rather than created and then rejected by the driver. */
    if (rsx_vk_format_is_block(d, fmt)) {
        rsx_vk_log(d, "color_target_create: format %d is sampled-only (DESIGN "
                   "3.1 fallback 1); refusing the attachment", (int)fmt);
        return 0;
    }
    if (!(d->format_features[fmt] & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
        rsx_vk_log(d, "color_target_create: format %d is not a colour "
                   "attachment on this device", (int)fmt);
        return 0;
    }
    if (!w || !h || w > d->max_color_w || h > d->max_color_h) {
        rsx_vk_log(d, "color_target_create: %ux%u is outside the device's "
                   "limits", w, h);
        return 0;
    }

    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        rsx_vk_log(d, "color_target_create: handle table full");
        return 0;
    }
    slot->kind = RSX_VK_SLOT_COLOR_TARGET;
    img = &slot->u.color;
    if (rsx_vk_image_create_ex(d, fmt, vfmt, w, h,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               1, 1, NULL, img) != 0) {
        rsx_vk_handle_retire(d->handles, id);
        return 0;
    }
    rsx_vk_log(d, "color_target_create: handle %u, %ux%u, fmt %d (0x%x)", id, w,
               h, (int)fmt, vfmt);

    if (seed && seed_row_bytes) {
        /* The engine's own comment says a title CPU-initialises its render
         * targets and samples them before anything draws into them
         * (rsx_draw_engine.c:530-545), so the seed rows go up in the same
         * command buffer as the target's first use. The rows are already
         * decoded R,G,B,A by rsx_texture_decode.
         *
         * `seed_row_bytes / 4u` assumes four bytes a texel, which is true for
         * R8G8B8A8 and for nothing else in the table: seeding an R16_UNORM
         * (two bytes) over-runs the row and seeding R16G16B16A16F (eight) reads
         * only half of each texel. Rather than convert silently and hand back a
         * wrong-coloured surface, the seed is applied only to the format the
         * path handles and refused with one log line otherwise (item 13).
         * rsx_draw_engine.c:530-545 only ever seeds an R8G8B8A8 surface -- it
         * checks the format first -- so the refusal is the guard, not the
         * common path. */
        const int seed_ok = (vfmt == VK_FORMAT_R8G8B8A8_UNORM);
        u32 row_texels = seed_row_bytes / 4u;
        u32 rows = h;
        if (!seed_ok) {
            rsx_vk_log(d, "color_target_create: handle %u is format %d (0x%x); "
                       "the seed path only handles R8G8B8A8 (4 bytes a texel), "
                       "so the seed is refused", id, (int)fmt, vfmt);
        } else {
            if (row_texels < w)
                rsx_vk_log(d, "color_target_create: a %u-byte seed row is "
                           "narrower than the %u-texel target",
                           seed_row_bytes, w);
            if (rsx_vk_cmd_upload_rows(d, img, 0, 0, w, rows < h ? rows : h,
                                       seed, seed_row_bytes, rows) == 0) {
                (void)rsx_vk_cmd_upload_done(d, img);
                rsx_vk_log(d, "color_target_create: handle %u seeded, %u rows of "
                           "%u bytes", id, rows, seed_row_bytes);
            }
        }
    }
    return id;
}

void rsx_vk_color_target_release(rsx_vk_device* d, u32 surface)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, surface);
    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "color_target_release: %u is not a live colour target",
                   surface);
        return;
    }
    /* Deferred: a command buffer being recorded may still name the image. It
     * dies at the next fence-passed boundary. */
    rsx_vk_handle_retire(d->handles, surface);
}

/* surface views */

/*
 * A texture unit naming a live colour target gets a *view* of it wearing that
 * unit's crossbar. This is an image view, not a format change: the image's
 * format never changes, only the component mapping, so no mutable-format
 * extension is needed and the same VkImage serves both roles in one frame.
 *
 * Views are cached per (surface, remap, rsx_fmt) because the engine asks once
 * per texture unit per draw, and a title with four units reading the same
 * surface would otherwise create four identical views every frame.
 */
u32 rsx_vk_surface_view(rsx_vk_device* d, u32 surface, u32 remap, u32 fmt)
{
    rsx_vk_slot* parent = rsx_vk_handle_get(d->handles, surface);
    rsx_vk_slot* slot = NULL;
    VkImageViewCreateInfo vci;
    VkImageView view = VK_NULL_HANDLE;
    u32 swz[4], id;

    if (!parent || parent->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "surface_view: %u is not a live colour target", surface);
        return 0;
    }
    /* The crossbar in the view's own order, exactly as texture_create does it:
     * rsx_texture_component_remap writes A,R,G,B and a VkComponentMapping
     * names R,G,B,A. */
    rsx_vk_crossbar_swizzle(remap, fmt, swz);

    /* The cache: a live view of this surface wearing this swizzle. Views are
     * cheap to destroy and expensive to churn, and the engine asks once per
     * texture unit per draw, so a title with four units reading one surface
     * would otherwise make four identical views a frame. */
    for (id = 1; id < rsx_vk_handle_capacity(d->handles); id++) {
        rsx_vk_slot* s = rsx_vk_handle_get(d->handles, id);
        if (!s || s->kind != RSX_VK_SLOT_SURFACE_VIEW)
            continue;
        if (s->u.color.view_of != surface)
            continue;
        if (s->u.color.swizzle[0] != swz[0] || s->u.color.swizzle[1] != swz[1] ||
            s->u.color.swizzle[2] != swz[2] || s->u.color.swizzle[3] != swz[3])
            continue;
        return id;
    }
    memset(&vci, 0, sizeof(vci));
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = (VkImage)parent->u.color.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = (VkFormat)parent->u.color.format;
    vci.components.r = (VkComponentSwizzle)swz[0];
    vci.components.g = (VkComponentSwizzle)swz[1];
    vci.components.b = (VkComponentSwizzle)swz[2];
    vci.components.a = (VkComponentSwizzle)swz[3];
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;
    if (((PFN_vkCreateImageView)g_vk.CreateImageView)((VkDevice)d->device, &vci, NULL, &view) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "surface_view: vkCreateImageView failed for %u", surface);
        return 0;
    }

    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        ((PFN_vkDestroyImageView)g_vk.DestroyImageView)((VkDevice)d->device, view,
                                                        NULL);
        rsx_vk_log(d, "surface_view: handle table full");
        return 0;
    }
    slot->kind = RSX_VK_SLOT_SURFACE_VIEW;
    /* A view owns no image and no memory; its slot names the parent's image
     * only so the format is answerable, and rsx_vk_slot_destroy destroys
     * exactly the VkImageView. */
    slot->u.color = parent->u.color;
    slot->u.color.view = (uint64_t)(uintptr_t)view;
    slot->u.color.identity = 0;
    slot->u.color.layout = -1;
    slot->u.color.view_of = surface;
    slot->u.color.swizzle[0] = swz[0];
    slot->u.color.swizzle[1] = swz[1];
    slot->u.color.swizzle[2] = swz[2];
    slot->u.color.swizzle[3] = swz[3];
    d->surface_views_created++;
    rsx_vk_log(d, "surface_view: surface %u crossbar 0x%04x -> view %u, swizzle "
               "%u,%u,%u,%u", surface, remap & 0xFFFFu, id, swz[0], swz[1],
               swz[2], swz[3]);
    return id;
}

/* depth targets */

u32 rsx_vk_depth_target_create(rsx_vk_device* d, u32 w, u32 h)
{
    rsx_vk_slot* slot = NULL;
    rsx_vk_depth* z;
    u32 id;

    if (!d->device || !d->depth_format) {
        rsx_vk_log(d, "depth_target_create: no depth format was chosen at init");
        return 0;
    }
    if (!w || !h || w > d->max_color_w || h > d->max_color_h) {
        rsx_vk_log(d, "depth_target_create: %ux%u is outside the device's "
                   "limits", w, h);
        return 0;
    }
    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        rsx_vk_log(d, "depth_target_create: handle table full");
        return 0;
    }
    slot->kind = RSX_VK_SLOT_DEPTH;
    z = &slot->u.depth;
    if (rsx_vk_image_create_ex(d, RSX_BE_FMT_NONE, d->depth_stencil_format, w, h,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               1, 1, NULL, &z->image) != 0) {
        rsx_vk_handle_retire(d->handles, id);
        return 0;
    }
    /* The DEPTH-only view depth_snapshot samples through (item 12): the image's
     * own view is DEPTH|STENCIL for the depth test, and a sampler over it would
     * present stencil in .g. */
    {
        VkImageViewCreateInfo vci;
        VkImageView dv = VK_NULL_HANDLE;
        memset(&vci, 0, sizeof vci);
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = (VkImage)z->image.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = (VkFormat)z->image.format;
        vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        if (((PFN_vkCreateImageView)g_vk.CreateImageView)((VkDevice)d->device,
                                                          &vci, NULL, &dv) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "depth_target_create: the DEPTH-only view failed");
            rsx_vk_image_destroy(d, &z->image);
            rsx_vk_handle_retire(d->handles, id);
            return 0;
        }
        z->depth_view = (uint64_t)(uintptr_t)dv;
    }
    /* A fresh zeta loads garbage, so the first pass that binds it clears it.
     * That is the engine's own rule too (rsx_draw_engine.c:1474-1482: the
     * depth attachment is cleared the first time it is bound). */
    z->clear_pending = 1;
    z->clear_depth = 1.0f;
    z->clear_stencil = 0;
    z->clear_flags = RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL;
    z->depth_clear_pending = 1;
    z->stencil_clear_pending = 1;
    d->depth_targets_created++;
    rsx_vk_log(d, "depth_target_create: handle %u, %ux%u, fmt 0x%x", id, w, h,
               d->depth_stencil_format);
    return id;
}

void rsx_vk_depth_target_release(rsx_vk_device* d, u32 depth)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, depth);
    if (!slot || slot->kind != RSX_VK_SLOT_DEPTH) {
        rsx_vk_log(d, "depth_target_release: %u is not a live zeta", depth);
        return;
    }
    rsx_vk_handle_retire(d->handles, depth);
}

void rsx_vk_clear_depth_stencil(rsx_vk_device* d, u32 depth, u32 flags,
                                float depth_value, u8 stencil)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, depth);
    rsx_vk_depth* z;

    if (!slot || slot->kind != RSX_VK_SLOT_DEPTH) {
        rsx_vk_log(d, "clear_depth_stencil: %u is not a live zeta", depth);
        return;
    }
    z = &slot->u.depth;
    /* The engine clears just before it binds the zeta for the draw that will
     * use it (rsx_draw_engine.c:1474-1482, :1569), so folding the clear into
     * the next pass's load op is the same ordering with one fewer command. If
     * a clear arrives while a pass is already open on this zeta, the pass has
     * to end first: its load op was decided when it began. */
    if (d->rendering && d->rendering_depth == (uint64_t)depth)
        rsx_vk_pass_end(d);
    /* Per aspect (item 8): a clear carries only the aspects its flags name, and
     * the pass uses CLEAR for an aspect only when an aspect was actually asked
     * for. A stencil-only clear of a zeta whose depth was just written must
     * leave that depth intact, which a whole-attachment LOAD_OP_CLEAR cannot
     * do. */
    if (flags & RSX_BE_CLEAR_DEPTH) {
        z->clear_depth = depth_value;
        z->depth_clear_pending = 1;
    }
    if (flags & RSX_BE_CLEAR_STENCIL) {
        z->clear_stencil = stencil;
        z->stencil_clear_pending = 1;
    }
    z->clear_pending = (z->depth_clear_pending || z->stencil_clear_pending);
    z->clear_flags = flags;
    /* A clear invalidates the older published depth image: the next texture
     * consumer resolves the newly written pass exactly once
     * (rsx_draw_engine.c:1573-1578 draws the same line). */
    z->had_write = 0;
    z->snapshot = 0;
    d->depth_clears++;
    rsx_vk_log(d, "clear_depth_stencil: zeta %u flags 0x%x depth %.6f stencil %u",
               depth, flags, depth_value, stencil);
}

/* depth_snapshot */

/*
 * A zeta read back as a sampleable R32F image.
 *
 * This is NOT a blit. Vulkan forbids a depth-to-colour vkCmdBlitImage (both
 * images would need BLIT_SRC for a depth format and a colour-compatible
 * format, and DESIGN §4.4 says not to try it), and there is no depth-resolve
 * promise on NXVK we rely on. So it is what Metal does: the depth image is
 * attached to a pass with no colour target, a full-screen triangle samples it,
 * and the triangle writes .r into a fresh R32F image.
 *
 * The draw helper lives here rather than in the pipeline file because it is a
 * fixed program with a fixed pipeline -- nothing of the guest's enters it --
 * and the source pixels have to be observed in stream order, which recording
 * it in the same command buffer gives for free.
 *
 * The shaders are GLSL rather than HLSL: this is not a guest program, glslang
 * compiles both equally, and a GLSL fragment stage avoids the HLSL
 * legalization passes that would otherwise be in the path of a depth test.
 * The SPIR-V is generated once per device and kept in a module slot.
 */
static const char k_snapshot_vs[] =
    "#version 450\n"
    "layout(location = 0) out vec2 v_uv;\n"
    "void main() {\n"
    "  /* One triangle covering the clip square: (-1,-1), (3,-1), (-1,3). */\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  v_uv = vec2(p.x, 1.0 - p.y);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char k_snapshot_fs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_depth;\n"
    "layout(location = 0) in vec2 v_uv;\n"
    "layout(location = 0) out float o_depth;\n"
    "void main() { o_depth = texture(u_depth, v_uv).r; }\n";

/* The snapshot pipeline, its layout and its descriptor set layout, built on
 * first use and reused for every zeta of the device's lifetime. */
struct snapshot_pipeline {
    uint64_t layout;
    uint64_t set_layout;
    uint64_t pipeline;
    uint64_t out_format;
    int      built;
};

static int snapshot_build(rsx_vk_device* d)
{
    rsx_vk_spv vs_spv, fs_spv;
    VkShaderModuleCreateInfo mci;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding b;
    VkDescriptorSetLayoutCreateInfo sli;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo lci;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vin;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[2];
    VkPipelineDynamicStateCreateInfo dynci;
    VkPipelineRenderingCreateInfo render;
    VkGraphicsPipelineCreateInfo ci;
    VkPipeline p = VK_NULL_HANDLE;
    VkFormat out_fmt = VK_FORMAT_R32_SFLOAT;
    char log[1024];

    if (rsx_vk_glsl_to_spirv(k_snapshot_vs, RSX_VK_STAGE_VERTEX, &vs_spv, log,
                             sizeof log) != 0 ||
        rsx_vk_glsl_to_spirv(k_snapshot_fs, RSX_VK_STAGE_FRAGMENT, &fs_spv, log,
                             sizeof log) != 0) {
        rsx_vk_log(d, "depth_snapshot: %s", log);
        return 0;
    }
    memset(&mci, 0, sizeof mci);
    mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mci.codeSize = (size_t)vs_spv.word_count * 4u;
    mci.pCode = vs_spv.words;
    if (((PFN_vkCreateShaderModule)g_vk.CreateShaderModule)((VkDevice)d->device, &mci, NULL, &vs) != VK_SUCCESS)
        rsx_vk_log(d, "depth_snapshot: vkCreateShaderModule (VS) failed");
    mci.codeSize = (size_t)fs_spv.word_count * 4u;
    mci.pCode = fs_spv.words;
    if (((PFN_vkCreateShaderModule)g_vk.CreateShaderModule)((VkDevice)d->device, &mci, NULL, &fs) != VK_SUCCESS)
        rsx_vk_log(d, "depth_snapshot: vkCreateShaderModule (FS) failed");
    rsx_vk_spirv_free(&vs_spv);
    rsx_vk_spirv_free(&fs_spv);
    if (!vs || !fs)
        return 0;

    memset(&b, 0, sizeof b);
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&sli, 0, sizeof sli);
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 1;
    sli.pBindings = &b;
    if (((PFN_vkCreateDescriptorSetLayout)g_vk.CreateDescriptorSetLayout)((VkDevice)d->device, &sli, NULL,
                                       &set_layout) != VK_SUCCESS) {
        rsx_vk_log(d, "depth_snapshot: vkCreateDescriptorSetLayout failed");
        return 0;
    }
    memset(&lci, 0, sizeof lci);
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &set_layout;
    if (((PFN_vkCreatePipelineLayout)g_vk.CreatePipelineLayout)((VkDevice)d->device, &lci, NULL, &layout) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "depth_snapshot: vkCreatePipelineLayout failed");
        return 0;
    }

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].pName = "main";
    st[0].module = vs;
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].pName = "main";
    st[1].module = fs;
    memset(&vin, 0, sizeof vin);
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    memset(&ba, 0, sizeof ba);
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&dynci, 0, sizeof dynci);
    dynci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynci.dynamicStateCount = 2;
    dynci.pDynamicStates = dyn;
    memset(&render, 0, sizeof render);
    render.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount = 1;
    render.pColorAttachmentFormats = &out_fmt;
    render.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    render.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &render;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dynci;
    ci.layout = layout;
    if (((PFN_vkCreateGraphicsPipelines)g_vk.CreateGraphicsPipelines)((VkDevice)d->device,
                                     (VkPipelineCache)d->pipeline_cache, 1, &ci,
                                     NULL, &p) != VK_SUCCESS) {
        rsx_vk_log(d, "depth_snapshot: vkCreateGraphicsPipelines failed");
        return 0;
    }
    ((PFN_vkDestroyShaderModule)g_vk.DestroyShaderModule)((VkDevice)d->device, vs, NULL);
    ((PFN_vkDestroyShaderModule)g_vk.DestroyShaderModule)((VkDevice)d->device, fs, NULL);
    d->snap_layout = (uint64_t)(uintptr_t)layout;
    d->snap_set_layout = (uint64_t)(uintptr_t)set_layout;
    d->snap_pipeline = (uint64_t)(uintptr_t)p;
    d->snap_built = 1;
    rsx_vk_log(d, "depth_snapshot: pipeline built (R32F out, combined "
               "sampler2D depth in)");
    return 1;
}

u32 rsx_vk_depth_snapshot(rsx_vk_device* d, u32 depth, u32 w, u32 h)
{
    rsx_vk_slot* zslot = rsx_vk_handle_get(d->handles, depth);
    rsx_vk_slot* tslot = NULL;
    rsx_vk_depth* z;
    rsx_vk_image* out;
    u32 id;
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet wr;
    VkRenderingAttachmentInfo att;
    VkRenderingInfo ri;
    VkViewport vp;
    VkRect2D sc;
    VkImageMemoryBarrier b;
    VkCommandBuffer cmd;

    if (!zslot || zslot->kind != RSX_VK_SLOT_DEPTH) {
        rsx_vk_log(d, "depth_snapshot: %u is not a live zeta", depth);
        return 0;
    }
    z = &zslot->u.depth;
    if (!z->had_write) {
        /* A clear-only zeta is not a usable depth map; the engine asks the
         * same question before it calls this (rsx_draw_engine.c:665-674). */
        rsx_vk_log(d, "depth_snapshot: zeta %u has had no depth write since "
                   "its last clear", depth);
        return 0;
    }
    if (!w || !h)
        return 0;
    if (!d->snap_built && !snapshot_build(d))
        return 0;

    /* The per-frame snapshot cap (item N5), checked BEFORE anything is
     * recorded. The snapshot pool holds RSX_VK_SNAPSHOTS_PER_FRAME descriptor
     * sets between two fences; a snapshot that arrives after that has no set
     * to write, and discovering it halfway through would leave the zeta
     * transitioned to a layout nothing samples it in and a half-recorded
     * command buffer behind the refusal. So the cap is a precondition: on
     * overflow this logs once and returns 0 with no barrier, no image and no
     * handle allocated.
     *
     * snapshots_this_frame is cleared at every fence (rsx_vk_device.c
     * submit_and_wait), which is the same boundary the pool reset uses, so the
     * two can never disagree about how many sets the frame has taken. */
    if (d->snapshots_this_frame >= RSX_VK_SNAPSHOTS_PER_FRAME) {
        d->snapshots_refused++;
        rsx_vk_log(d, "depth_snapshot: frame %u already recorded %u snapshots "
                   "(the per-frame cap); the snapshot is refused and nothing "
                   "is recorded", d->frame,
                   (unsigned)RSX_VK_SNAPSHOTS_PER_FRAME);
        return 0;
    }
    d->snapshots_this_frame++;

    /* The result is a texture slot, because that is what the engine binds: a
     * sampleable image with a view, released by texture_release like any
     * other, and sampled by binding it to whichever unit the guest named. */
    id = rsx_vk_handle_alloc(d->handles, &tslot);
    if (!id) {
        rsx_vk_log(d, "depth_snapshot: handle table full");
        return 0;
    }
    tslot->kind = RSX_VK_SLOT_TEXTURE;
    tslot->u.texture.remap = 0xAAE4u;   /* identity: the shader reads .r */
    tslot->u.texture.rsx_fmt = 0u;
    if (rsx_vk_image_create_ex(d, RSX_BE_FMT_R32F, VK_FORMAT_R32_SFLOAT, w, h,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               1, 1, NULL, &tslot->u.texture.image) != 0) {
        rsx_vk_handle_retire(d->handles, id);
        return 0;
    }
    out = &tslot->u.texture.image;

    if (d->rendering)
        rsx_vk_pass_end(d);
    if (rsx_vk_device_begin(d) != 0)
        return 0;
    cmd = (VkCommandBuffer)rsx_vk_device_cmd(d);

    /* The source: wherever the tracker says the zeta is, to a sampled read.
     * Recorded in stream order, so the depth-writing draws before this have
     * executed.
     *
     * BOTH aspects (item N1). The whole image changes layout here -- that is
     * what rsx_vk_image_set_layout records -- so the barrier has to cover the
     * same two aspects, or the stencil aspect stays in
     * DEPTH_STENCIL_ATTACHMENT_OPTIMAL while the tracker claims
     * SHADER_READ_ONLY_OPTIMAL and the next to_depth_attachment records a
     * wrong oldLayout. The snapshot still *views* DEPTH alone (item 12, its
     * descriptor names z->depth_view), which is a view over one aspect of a
     * two-aspect image in a combined layout: legal, and the reason
     * DEPTH_STENCIL_READ_ONLY_OPTIMAL exists as a distinct layout at all. */
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = (VkImageLayout)(z->image.layout < 0
                                      ? VK_IMAGE_LAYOUT_UNDEFINED
                                      : z->image.layout);
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)z->image.image;
    b.subresourceRange.aspectMask = z->image.aspect_mask;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    ((PFN_vkCmdPipelineBarrier)g_vk.CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                            NULL, 1, &b);
    rsx_vk_image_set_layout(&z->image,
                            (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    /* The destination: COLOR_ATTACHMENT_OPTIMAL, from UNDEFINED (a fresh
     * image, so nothing to preserve). */
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)out->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    ((PFN_vkCmdPipelineBarrier)g_vk.CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                            NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(out,
                            (int32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    /* The descriptor set for the depth sampler, from this program's own
     * layout: its binding 0 is a combined image sampler, which the per-draw
     * layouts (separate image and sampler, DESIGN §2.2) are not. Its sampler
     * is point-filtered and clamped, which is what a zeta read texel for
     * texel wants. */
    /* This program's own pool: its layout is not the draw's, and the draw's
     * pool is reset at every fence. Two allocations of the same set from a
     * pool the device resets under it is the lifetime hazard the descriptor
     * pool exists to avoid -- but the *snapshot* pool is created ONCE, at first
     * use, and reset at each fence-passed boundary (submit_and_wait), not per
     * call. Creating one per call leaked a VkDescriptorPool per snapshot, which
     * no counter saw: 100 snapshots were 100 pools alive until shutdown. */
    {
        VkDescriptorPoolSize sz;
        VkDescriptorPoolCreateInfo pci;
        VkDescriptorSetLayout l = (VkDescriptorSetLayout)d->snap_set_layout;
        if (!d->snap_pool_created) {
            sz.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            /* One set per snapshot, and the cap above is how many a frame may
             * take, so the pool is sized from that one named constant. It used
             * to be the literal 32, which was neither the draw pool's budget
             * nor a number anything else knew (item N5). */
            sz.descriptorCount = RSX_VK_SNAPSHOTS_PER_FRAME;
            memset(&pci, 0, sizeof pci);
            pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pci.maxSets = RSX_VK_SNAPSHOTS_PER_FRAME;
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &sz;
            if (((PFN_vkCreateDescriptorPool)g_vk.CreateDescriptorPool)(
                    (VkDevice)d->device, &pci, NULL,
                    (VkDescriptorPool*)&d->snap_pool_created) != VK_SUCCESS) {
                rsx_vk_log(d, "depth_snapshot: vkCreateDescriptorPool failed");
                rsx_vk_log(d, "depth_snapshot: no descriptor set; the snapshot "
                           "is dropped");
                rsx_vk_handle_retire(d->handles, id);
                return 0;
            }
            d->descriptor_pools_created++;
            d->descriptor_pools_alive++;
            rsx_vk_log(d, "depth_snapshot: descriptor pool created once (%u "
                       "sets), reset at each fence",
                       (unsigned)RSX_VK_SNAPSHOTS_PER_FRAME);
        }
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = (VkDescriptorPool)d->snap_pool_created;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &l;
        if (((PFN_vkAllocateDescriptorSets)g_vk.AllocateDescriptorSets)(
                (VkDevice)d->device, &ai, &dset) != VK_SUCCESS) {
            rsx_vk_log(d, "depth_snapshot: the snapshot descriptor set is "
                       "unavailable");
            rsx_vk_handle_retire(d->handles, id);
            return 0;
        }
        d->snap_pool_in_use = 1;
    }
    {
        rsx_be_sampler_desc sd;
        memset(&sd, 0, sizeof sd);
        sd.wrap_s = sd.wrap_t = sd.wrap_r = 2u;   /* guest code: MIRRORED_REPEAT */
        memset(&ii, 0, sizeof ii);
        ii.imageView = (VkImageView)z->depth_view;
        /* The layout the barrier above put the image in, not the colour-read
         * layout: the descriptor and the tracker must agree (N1). */
        ii.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        ii.sampler = (VkSampler)rsx_vk_sampler_get(d, &sd, 1);
    }
    memset(&wr, 0, sizeof wr);
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    ((PFN_vkUpdateDescriptorSets)g_vk.UpdateDescriptorSets)((VkDevice)d->device, 1, &wr, 0, NULL);

    /* The pass: one colour target (the R32F result), no depth. */
    memset(&att, 0, sizeof att);
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = (VkImageView)out->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    ((PFN_vkCmdBeginRenderingKHR)g_vk.CmdBeginRenderingKHR)(cmd, &ri);
    ((PFN_vkCmdBindPipeline)g_vk.CmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         (VkPipeline)d->snap_pipeline);
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    ((PFN_vkCmdSetViewport)g_vk.CmdSetViewport)(cmd, 0, 1, &vp);
    sc.offset.x = 0;
    sc.offset.y = 0;
    sc.extent.width = w;
    sc.extent.height = h;
    ((PFN_vkCmdSetScissor)g_vk.CmdSetScissor)(cmd, 0, 1, &sc);
    ((PFN_vkCmdBindDescriptorSets)g_vk.CmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                               (VkPipelineLayout)d->snap_layout, 0, 1, &dset, 0,
                               NULL);
    ((PFN_vkCmdDraw)g_vk.CmdDraw)(cmd, 3, 1, 0, 0);
    ((PFN_vkCmdEndRenderingKHR)g_vk.CmdEndRenderingKHR)(cmd);

    /* The result has to be samplable by the draws that follow. */
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)out->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    ((PFN_vkCmdPipelineBarrier)g_vk.CmdPipelineBarrier)(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(out,
                            (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    d->depth_snapshots++;
    rsx_vk_log(d, "depth_snapshot: zeta %u -> texture %u (%ux%u R32F)", depth,
               id, w, h);
    return id;
}
