/*
 * ps3recomp - Vulkan backend: textures
 *
 * texture_create -> a 2D array image of `mips` levels and `faces` layers, with
 * the TEXTURE_CONTROL1 crossbar baked into its view's VkComponentMapping;
 * texture_upload -> host rows through the staging slab into one subresource;
 * texture_release -> a retire, with the objects destroyed at the next
 * fence-passed boundary (the same deferred rule as every other slot).
 *
 * The crossbar is a property of the *view*, not of the pixels.
 * Vulkan expresses it as a VkComponentMapping on the VkImageView, and the
 * decode is not repeated here: rsx_texture_component_remap
 * (rsx_texture_layout.h:193-213) is the one implementation, and its output is
 * reordered from its field order (A,R,G,B) into the view's (R,G,B,A). That
 * reorder has been got wrong before -- the regression test_texture_layout.c
 * documents is exactly the identity 0xAAE4 read backwards -- so it is stated
 * once, here, with the two selectors that have no VkComponentSwizzle of their
 * own (force zero, force one) mapped explicitly.
 *
 * Uploads batch. A cube is six uploads and a mip chain is up to ten more, and
 * every one of them would otherwise be its own layout transition and its own
 * "now the shader can read it" barrier. rsx_vk_texture_upload records the copy
 * and leaves the image in TRANSFER_DST_OPTIMAL; the first thing that needs to
 * sample the image finishes the batch (rsx_vk_cmd_upload_done), and a draw
 * finishes every open batch before it records. The fence reset in
 * submit_and_wait closes the batch implicitly: the counter there is the
 * "uploads recorded into the command buffer being recorded now" count.
 */
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* the staging slab */

void* rsx_vk_tex_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment,
                         uint32_t* out_offset)
{
    uint32_t off;
    uint32_t align = alignment ? alignment : 4u;
    if (out_offset)
        *out_offset = 0;
    if (!d->tex_map || !bytes)
        return NULL;
    off = (d->tex_offset + align - 1u) & ~(align - 1u);
    if (off > RSX_VK_TEX_SLAB_BYTES || bytes > RSX_VK_TEX_SLAB_BYTES - off)
        return NULL;
    d->tex_offset = off + bytes;
    if (out_offset)
        *out_offset = off;
    return (uint8_t*)d->tex_map + off;
}

/* ---- the one place the crossbar becomes a VkComponentMapping ----------- */

static void crossbar_mapping(u32 remap, u32 rsx_fmt, VkComponentMapping* out)
{
    uint32_t sw[4];
    rsx_vk_crossbar_swizzle(remap, rsx_fmt, sw);
    out->r = (VkComponentSwizzle)sw[0];
    out->g = (VkComponentSwizzle)sw[1];
    out->b = (VkComponentSwizzle)sw[2];
    out->a = (VkComponentSwizzle)sw[3];
}

/* ---- the null texture --------------------------------------------------- */

/*
 * DESIGN §1.1's "null texture": a 1x1 opaque white R8G8B8A8 image, created
 * once, that every unit a draw does not name is bound to. A descriptor cannot
 * be left without an image, and white is what makes an accidentally unbound
 * unit visible in a frame rather than black-and-invisible.
 */
int rsx_vk_null_image_create(rsx_vk_device* d)
{
    void* dst;
    const u8 white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    rsx_vk_image img;
    rsx_vk_slot slot;

    if (rsx_vk_image_create_ex(d, RSX_BE_FMT_R8G8B8A8, VK_FORMAT_R8G8B8A8_UNORM,
                               1, 1, VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               1, 1, NULL, &img) != 0)
        return -1;
    if (rsx_vk_cmd_upload_rows(d, &img, 0, 0, 1, 1, white, 4, 1) != 0 ||
        rsx_vk_cmd_upload_done(d, &img) != 0)
        return -1;
    /* One command buffer, submitted here: nothing else has recorded yet, so
     * this is also the first frame's begin and cannot disturb a pass. */
    if (rsx_vk_device_submit_and_wait(d, RSX_VK_FLUSH_IDLE) != 0)
        return -1;
    d->null_image = img.image;
    d->null_view = img.view;
    d->null_mem = img.memory;
    memset(&slot, 0, sizeof slot);
    (void)dst;
    rsx_vk_log(d, "null texture: 1x1 opaque white (view 0x%llx)",
               (unsigned long long)d->null_view);
    return 0;
}

void rsx_vk_null_image_destroy(rsx_vk_device* d)
{
    VkDevice dev;
    if (!d || !d->device)
        return;
    dev = (VkDevice)d->device;
    if (d->null_view)
        ((PFN_vkDestroyImageView)g_vk.DestroyImageView)(dev, (VkImageView)d->null_view, NULL);
    if (d->null_image)
        ((PFN_vkDestroyImage)g_vk.DestroyImage)(dev, (VkImage)d->null_image, NULL);
    if (d->null_mem) {
        ((PFN_vkFreeMemory)g_vk.FreeMemory)(dev, (VkDeviceMemory)d->null_mem, NULL);
        d->memory_frees++;
    }
    d->null_view = d->null_image = d->null_mem = 0;
}

/* ---- texture_create / release ----------------------------------------- */

u32 rsx_vk_texture_create(rsx_vk_device* d, rsx_be_format fmt, u32 w, u32 h,
                          u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    rsx_vk_slot* slot = NULL;
    rsx_vk_texture* t;
    VkComponentMapping map;
    uint32_t vfmt;
    u32 id;

    if (!d->device) {
        rsx_vk_log(d, "texture_create before init");
        return 0;
    }
    vfmt = rsx_vk_format(d, fmt);
    if (!vfmt) {
        /* DESIGN §3.1 fallback 4: an unsupported format leaves the unit
         * unbound and the draw runs with the null texture, which is the
         * engine's documented behaviour for a failed upload
         * (rsx_draw_engine.c:757-767). */
        rsx_vk_log(d, "texture_create: format %d (rsx_fmt 0x%x) has no usable "
                   "VkFormat; no texture", (int)fmt, rsx_fmt);
        return 0;
    }
    if (!w || !h || w > d->max_color_w || h > d->max_color_h) {
        rsx_vk_log(d, "texture_create: %ux%u is outside the device's limits", w, h);
        return 0;
    }
    if (!mips)
        mips = 1;
    if (faces != 1 && faces != 6) {
        rsx_vk_log(d, "texture_create: %u faces is neither 2D nor a cube", faces);
        return 0;
    }

    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        rsx_vk_log(d, "texture_create: handle table full");
        return 0;
    }
    slot->kind = RSX_VK_SLOT_TEXTURE;
    t = &slot->u.texture;
    t->remap = remap;
    t->rsx_fmt = rsx_fmt;

    crossbar_mapping(remap, rsx_fmt, &map);
    if (rsx_vk_image_create_ex(d, fmt, vfmt, w, h,
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               mips, faces, &map, &t->image) != 0) {
        rsx_vk_handle_retire(d->handles, id);
        return 0;
    }
    d->textures_created++;
    rsx_vk_log(d, "texture_create: handle %u, %ux%u fmt %d (0x%x, rsx_fmt 0x%x), "
               "%u level(s), %u face(s), crossbar 0x%04x -> swizzle %u,%u,%u,%u",
               id, w, h, (int)fmt, vfmt, rsx_fmt, mips, faces, remap & 0xFFFFu,
               (unsigned)map.r, (unsigned)map.g, (unsigned)map.b, (unsigned)map.a);
    return id;
}

void rsx_vk_texture_release(rsx_vk_device* d, u32 texture)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, texture);
    if (!slot || slot->kind != RSX_VK_SLOT_TEXTURE) {
        rsx_vk_log(d, "texture_release: %u is not a live texture", texture);
        return;
    }
    /* Deferred: a command buffer being recorded may still name the image. */
    rsx_vk_handle_retire(d->handles, texture);
}

/* ---- uploads ----------------------------------------------------------- */

/*
 * The copy rows of one subresource through the slab, leaving the image in
 * TRANSFER_DST_OPTIMAL. The slab is refilled by the caller before each call,
 * so a source larger than the slab is an error rather than a wrap.
 *
 * Row length and row count are in the units the *format* uses, which is what
 * the engine's own upload passes:
 *
 *   - uncompressed: `row_bytes` is the source row stride in bytes, `rows` the
 *     texel rows, and VkBufferImageCopy's bufferRowLength is in texels, so it
 *     is row_bytes / bytes-per-texel.
 *   - block-compressed: ps3recomp sends the *compressed blocks*, not decoded
 *     RGBA (rsx_draw_engine.c:769-773 uploads tl->dst_row_bytes per row, and
 *     for BC that is ((w+3)/4)*block_bytes wide and (h+3)/4 rows tall --
 *     rsx_texture_layout.c:98-106). VkBufferImageCopy counts a BC row length in
 *     *blocks*, so it is row_bytes / block_bytes.
 *
 * The previous code left bufferRowLength at 0 ("tightly packed"), which is
 * only correct when the slab copy has no padding. It always does not -- the
 * slab is byte-packed -- but a caller whose row_bytes was wider than the
 * extent then read the wrong texels, and the intent was invisible. Setting it
 * from row_bytes makes the image extent and the row stride agree explicitly.
 *
 * Any other row_bytes that is not an exact whole number of texels (or blocks)
 * is rejected with a log rather than silently reinterpreted.
 */
int rsx_vk_cmd_upload_rows(rsx_vk_device* d, rsx_vk_image* img, u32 face,
                           u32 mip, u32 w, u32 h, const void* src,
                           u32 row_bytes, u32 rows)
{
    VkBufferImageCopy copy;
    VkImageMemoryBarrier b;
    VkCommandBuffer cmd;
    void* dst;
    uint32_t off = 0;
    uint32_t need;
    uint32_t block;         /* 4 for a BC format's block extent         */
    int32_t from;

    if (!img || !img->image || !src)
        return -1;
    if (face >= img->faces || mip >= img->mips) {
        rsx_vk_log(d, "upload: face %u / level %u is outside a %u-face, %u-level "
                   "texture", face, mip, img->faces, img->mips);
        return -1;
    }
    if (rsx_vk_device_begin(d) != 0)
        return -1;

    /* First use of this subresource: TRANSFER_DST_OPTIMAL, from UNDEFINED
     * (the whole image is transitioned once, on the first upload). */
    from = rsx_vk_image_layout(img);
    if (from != (int32_t)VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        memset(&b, 0, sizeof(b));
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = (from < 0) ? 0u : VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = (VkImage)img->image;
        b.subresourceRange.aspectMask = rsx_vk_aspect_mask(img->format);
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = img->mips;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = img->faces;
        cmd = (VkCommandBuffer)rsx_vk_device_cmd(d);
        g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                NULL, 1, &b);
        rsx_vk_image_set_layout(img, (int32_t)VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    need = row_bytes * rows;
    dst = rsx_vk_tex_reserve(d, need, d->copy_align ? d->copy_align : 4u, &off);
    if (!dst) {
        /* The slab is per frame and freed by submit_and_wait; a caller that
         * overruns it is asking for more than 16 MiB of decoded rows in one
         * frame, which is a logged drop rather than a corrupt copy. */
        rsx_vk_log(d, "upload: %u bytes do not fit the %u-byte texture slab",
                   need, (unsigned)RSX_VK_TEX_SLAB_BYTES);
        return -1;
    }
    memcpy(dst, src, need);

    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = off;
    /* The row stride in the units VkBufferImageCopy counts it in: texels for an
     * uncompressed format, blocks for a BC one. A stride that is not a whole
     * number of those is a caller error and is refused rather than rounded. */
    if (rsx_vk_format_is_block(d, img->be_format)) {
        uint32_t block_bytes = 16u;
        block = 4u;
        /* BC1's blocks are 8 bytes; BC2/3's are 16. The engine passes the
         * block row's own byte width as row_bytes
         * (((w+3)/4) * block_bytes), so the block count in a row is
         * row_bytes / block_bytes. */
        if (img->format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
            img->format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
            block_bytes = 8u;
        if (row_bytes % block_bytes != 0u) {
            rsx_vk_log(d, "upload: %u-byte row is not a whole number of "
                       "%u-byte blocks", row_bytes, block_bytes);
            return -1;
        }
        /* Root (2026-11-12, capture rv14): VkBufferImageCopy counts a block format in TEXELS as well, not in
         * blocks. The block counts that stood here copied only the top-left quarter-by-quarter corner of every
         * BC texture, from a row length a quarter of the real one, and left the rest of the image empty: the
         * game's loading-screen logo (DXT5, lower part of a 512x256 atlas) sampled as transparent black. */
        copy.bufferRowLength = (row_bytes / block_bytes) * block;   /* in texels, a whole number of blocks */
        copy.imageExtent.width = w;
        copy.imageExtent.height = h;
    } else {
        uint32_t texel = 4u;
        switch (img->format) {
        case VK_FORMAT_R8_UNORM:            texel = 1u;  break;
        case VK_FORMAT_R8G8_UNORM:          texel = 2u;  break;
        case VK_FORMAT_R16_UNORM:           texel = 2u;  break;
        case VK_FORMAT_R8G8B8A8_UNORM:      texel = 4u;  break;
        case VK_FORMAT_R16G16_UNORM:        texel = 4u;  break;
        case VK_FORMAT_R16G16_SFLOAT:       texel = 4u;  break;
        case VK_FORMAT_R32_SFLOAT:          texel = 4u;  break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: texel = 8u;  break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: texel = 16u; break;
        default:                            texel = 4u;  break;
        }
        if (row_bytes % texel != 0u) {
            rsx_vk_log(d, "upload: %u-byte row is not a whole number of %u-byte "
                       "texels", row_bytes, texel);
            return -1;
        }
        copy.bufferRowLength = row_bytes / texel;   /* in texels */
        copy.imageExtent.width = w;
        copy.imageExtent.height = h;
    }
    copy.bufferImageHeight = 0;
    copy.imageOffset.x = 0;
    copy.imageOffset.y = 0;
    copy.imageOffset.z = 0;
    copy.imageExtent.depth = 1;
    copy.imageSubresource.aspectMask = rsx_vk_aspect_mask(img->format);
    copy.imageSubresource.mipLevel = mip;
    copy.imageSubresource.baseArrayLayer = face;
    copy.imageSubresource.layerCount = 1;
    cmd = (VkCommandBuffer)rsx_vk_device_cmd(d);
    g_vk.CmdCopyBufferToImage(cmd, (VkBuffer)d->tex_buf, (VkImage)img->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    d->texture_uploads++;
    return 0;
}

int rsx_vk_cmd_upload_done(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    uint32_t aspect;

    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) != (int32_t)VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        return 0;
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    aspect = rsx_vk_aspect_mask(img->format);
    /* A depth or stencil image is read by the depth test, everything else by
     * a sampler (DESIGN §4.3 shape 1). */
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = (aspect == VK_IMAGE_ASPECT_COLOR_BIT)
                          ? VK_ACCESS_SHADER_READ_BIT
                          : (VK_ACCESS_SHADER_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = img->mips;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = img->faces;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 0;
}

void rsx_vk_texture_upload(rsx_vk_device* d, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, texture);
    if (!slot || slot->kind != RSX_VK_SLOT_TEXTURE) {
        rsx_vk_log(d, "texture_upload: %u is not a live texture", texture);
        return;
    }
    if (!src || !w || !h || !row_bytes) {
        rsx_vk_log(d, "texture_upload: empty upload to %u", texture);
        return;
    }
    {   /* Root diagnostic (2026-11-12): RSX_VK_TRACE=1 summarises what is uploaded. */
        static int trace = -1;
        if (trace < 0) trace = getenv("RSX_VK_TRACE") != NULL;
        if (trace) {
            const unsigned char* bytes = (const unsigned char*)src;
            unsigned long long nonzero = 0, total = (unsigned long long)row_bytes * rows, i, per[4] = {0, 0, 0, 0};
            for (i = 0; i < total; i++) if (bytes[i]) { nonzero++; per[i & 3u]++; }
            rsx_vk_log(d, "texture_upload: %u face %u mip %u %ux%u row_bytes %u rows %u nonzero %llu of %llu (by byte lane %llu %llu %llu %llu)",
                       texture, face, mip, w, h, row_bytes, rows, nonzero, total, per[0], per[1], per[2], per[3]);
        }
    }
    if (rsx_vk_cmd_upload_rows(d, &slot->u.texture.image, face, mip, w, h, src,
                               row_bytes, rows) != 0)
    {
        rsx_vk_log(d, "texture_upload: recording the upload to %u failed", texture);
        return;
    }
    slot->u.texture.uploads_recorded++;
}

/* Finish every texture's open upload batch. Called before a draw records, and
 * once per frame by submit_and_wait's fence reset, so a texture uploaded in
 * frame N is in SHADER_READ_ONLY before anything samples it in frame N. */
void rsx_vk_textures_end_uploads(rsx_vk_device* d)
{
    uint32_t id;
    if (!d || !d->handles)
        return;
    for (id = 1; id < rsx_vk_handle_capacity(d->handles); id++) {
        rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, id);
        if (!slot || slot->kind != RSX_VK_SLOT_TEXTURE)
            continue;
        if (slot->u.texture.uploads_recorded) {
            (void)rsx_vk_cmd_upload_done(d, &slot->u.texture.image);
            slot->u.texture.uploads_recorded = 0;
            d->upload_batches++;
        }
    }
}
