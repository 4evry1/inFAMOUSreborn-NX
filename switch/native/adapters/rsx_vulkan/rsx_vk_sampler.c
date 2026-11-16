/*
 * ps3recomp - Vulkan backend: the sampler cache
 *
 * One VkSampler per distinct rsx_be_sampler_desc, in the fixed-size cache
 * Metal uses for the same job (MTL_SAMP_CACHE, rsx_metal_backend.m:277): 64
 * entries, keyed by the desc's own bytes, least-recently-used eviction. The
 * engine can change a unit's sampler every draw and a title has a handful of
 * real combinations, so the cache is what keeps vkCreateSampler out of the
 * per-draw path.
 *
 * The mapping table is DESIGN §3.5 verbatim, including the guest wrap codes
 * 1..8, which are the codes both backends translate (the identical table to
 * gcm_wrap_to_metal at rsx_metal_backend.m:1101-1113). Two of the eight are
 * extensions rather than core ticks:
 *
 *   - 4 (clamp to border) needs VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, which
 *     is core 1.0, and VK_EXT_custom_border_color only if a guest border colour
 *     were ever honoured. It is not: transparent black is what Metal
 *     hard-codes too (:2638), so the extension is not asked for.
 *   - 6,7,8 (mirror clamp to edge) are VK_KHR_sampler_mirror_clamp_to_edge,
 *     advertised by NXVK and core 1.2. There is no device-side check here
 *     because the wrap is chosen per draw, not per device; a device without it
 *     would reject the sampler, which vkCreateSampler reports and this logs.
 *
 * DESIGN §3.1 fallback 2 is the `filterable` argument: an R16_UNORM or
 * R16G16_UNORM the device cannot filter linearly is sampled with NEAREST and
 * one log line, rather than silently returning garbage. Fallback 3 (a format
 * with no COLOR_ATTACHMENT_BLEND) is a pipeline property and lives in
 * rsx_vk_pipeline.c.
 */
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

static uint64_t fnv1a(const void* data, size_t n, uint64_t h)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* The guest's wrap code -> a VkSamplerAddressMode. 1 REPEAT, 2 MIRRORED_REPEAT,
 * 4 CLAMP_TO_BORDER, 6/7/8 MIRROR_CLAMP_TO_EDGE, anything else CLAMP_TO_EDGE
 */
static VkSamplerAddressMode wrap_mode(u32 guest)
{
    switch (guest) {
    case 1u: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2u: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4u: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6u:
    case 7u:
    case 8u: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

uint64_t rsx_vk_sampler_get(rsx_vk_device* d, const rsx_be_sampler_desc* sd,
                            int filterable)
{
    rsx_be_sampler_desc key;
    uint64_t hash;
    uint32_t i, victim = 0;
    VkSamplerCreateInfo ci;
    VkSampler s = VK_NULL_HANDLE;

    if (!d || !sd)
        return 0;
    memset(&key, 0, sizeof key);
    memcpy(&key, sd, sizeof key);
    /* `filterable` is part of the identity: the same desc paired with an
     * unfilterable format has to produce a different sampler. */
    if (!filterable) {
        key.min_linear = 0;
        key.mag_linear = 0;
        key.mip_linear = 0;
    }
    hash = fnv1a(&key, sizeof key, 1469598103934665603ull);

    for (i = 0; i < RSX_VK_MAX_SAMPLERS; i++) {
        if (d->samplers[i].used && d->samplers[i].hash == hash) {
            d->samplers[i].last_use = ++d->sampler_clock;
            /* A cache hit for an unfilterable request was still a request the
             * fallback answered, so it is counted every time rather than only
             * when the sampler is created: the L2 case asks "did this bind use
             * NEAREST because of the format", not "was a sampler built". */
            if (!filterable &&
                (sd->min_linear || sd->mag_linear || sd->mip_linear))
                d->samplers_nearest_forced++;
            return d->samplers[i].sampler;
        }
    }

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = key.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = key.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    /* DESIGN §3.5: a texture with no mips is NotMipmapped, which is what
     * Metal does (rsx_metal_backend.m:2630), and so is an unfilterable
     * format's clamp. */
    ci.mipmapMode = (key.mip_linear && key.mip_present)
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(key.wrap_s);
    ci.addressModeV = wrap_mode(key.wrap_t);
    ci.addressModeW = wrap_mode(key.wrap_r);
    /* RSX 4.8 fixed in the guest's own units; already decoded
     * (rsx_draw_engine.c:853-869 decodes control0 into these two). */
    ci.minLod = key.min_lod;
    ci.maxLod = key.mip_present ? key.max_lod : 0.0f;
    if (ci.maxLod < 0.0f)
        ci.maxLod = VK_LOD_CLAMP_NONE;
    ci.mipLodBias = 0.0f;
    ci.anisotropyEnable = VK_FALSE;
    ci.compareEnable = VK_FALSE;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.unnormalizedCoordinates = VK_FALSE;

    if (g_vk.CreateSampler((VkDevice)d->device, &ci, NULL, &s) != VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateSampler failed: min %d mag %d mip %d wrap %u/%u/%u",
                   (int)ci.minFilter, (int)ci.magFilter, (int)ci.mipmapMode,
                   key.wrap_s, key.wrap_t, key.wrap_r);
        return 0;
    }

    /* Least recently used wins the eviction. The evicted sampler is NOT
     * destroyed here: a command buffer still recording, or one already
     * submitted and in flight, may name it through a descriptor set that was
     * written with it. It goes on the retirement list and dies at the next
     * fence-passed boundary, exactly like an image or a pipeline
     * (rsx_vk_handle_retire's comment states the same rule). */
    for (i = 0; i < RSX_VK_MAX_SAMPLERS; i++) {
        if (!d->samplers[i].used) {
            victim = i;
            break;
        }
        if (d->samplers[i].last_use < d->samplers[victim].last_use)
            victim = i;
    }
    if (d->samplers[victim].used) {
        rsx_vk_log(d, "sampler cache full (%u): evicting the least recently used",
                   (unsigned)RSX_VK_MAX_SAMPLERS);
        if (d->samplers_retired_count <
            (uint32_t)(sizeof d->samplers_retired / sizeof d->samplers_retired[0])) {
            d->samplers_retired[d->samplers_retired_count].sampler =
                d->samplers[victim].sampler;
            d->samplers_retired[d->samplers_retired_count].frame_retired =
                d->frame;
            d->samplers_retired_count++;
        } else {
            /* Cannot happen while the list is 4x the cache and only one
             * generation is ever in flight, but a dropped lease is a leak and
             * this device counts leaks rather than assuming none. */
            d->samplers_retired_dropped++;
            rsx_vk_log(d, "sampler retirement list full: sampler 0x%llx leaked",
                       (unsigned long long)d->samplers[victim].sampler);
        }
        d->sampler_count--;
    }
    d->samplers[victim].hash = hash;
    d->samplers[victim].sampler = (uint64_t)(uintptr_t)s;
    d->samplers[victim].last_use = ++d->sampler_clock;
    d->samplers[victim].used = 1;
    d->sampler_count++;
    d->samplers_created++;
    if (!filterable &&
        (sd->min_linear || sd->mag_linear || sd->mip_linear)) {
        d->samplers_nearest_forced++;
        rsx_vk_log(d, "sampler: the texture format is not linearly filterable, "
                   "min/mag/mip clamped to NEAREST (DESIGN 3.1 fallback 2)");
    }
    /* The desc the cache entry was actually built for, after the clamp. */
    d->samplers[victim].desc = key;
    return (uint64_t)(uintptr_t)s;
}

/*
 * The sampler every image descriptor pairs with when the draw does not name
 * one. Vulkan needs a sampler bound for every sampled image even when the
 * shader only reads its texels, and the engine binds the units a program
 * actually samples, so this is the default for the rest: point sampling,
 * clamp to edge.
 *
 * It is created OUTSIDE the LRU cache. As a cache entry it was the 65th
 * distinct sampler that destroyed it: `d->null_sampler` then named a freed
 * VkSampler and every unnamed unit of every later draw bound it, which is a
 * use-after-free the validation layer reports and a device crash otherwise.
 * It lives for the device's lifetime and rsx_vk_samplers_destroy_all destroys
 * it.
 */
uint64_t rsx_vk_null_sampler(rsx_vk_device* d)
{
    VkSamplerCreateInfo ci;
    VkSampler s = VK_NULL_HANDLE;

    if (!d)
        return 0;
    if (d->null_sampler)
        return d->null_sampler;
    if (!d->device)
        return 0;
    /* Point sampling, clamp to edge: guest wrap code 2 is MIRRORED_REPEAT but
     * a 1x1 white image is the same under every wrap, and NEAREST is what
     * keeps the null path from being the one that needs linear filtering. */
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = VK_FILTER_NEAREST;
    ci.minFilter = VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.minLod = 0.0f;
    ci.maxLod = 0.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (g_vk.CreateSampler((VkDevice)d->device, &ci, NULL, &s) != VK_SUCCESS) {
        rsx_vk_log(d, "null sampler: vkCreateSampler failed");
        return 0;
    }
    d->null_sampler = (uint64_t)(uintptr_t)s;
    rsx_vk_log(d, "null sampler created outside the cache (0x%llx)",
               (unsigned long long)d->null_sampler);
    return d->null_sampler;
}

/*
 * The fence-passed boundary for samplers. Called from
 * rsx_vk_device_submit_and_wait once the frame's fence has passed, so nothing
 * recorded can still name a retired sampler. A sampler retired during the
 * frame that has just been submitted is collected here; one retired while a
 * later frame is being recorded waits for that frame's boundary because its
 * frame_retired is then the newer frame.
 */
void rsx_vk_samplers_collect(rsx_vk_device* d)
{
    uint32_t i, keep = 0;
    if (!d || !d->device)
        return;
    for (i = 0; i < d->samplers_retired_count; i++) {
        if (d->samplers_retired[i].frame_retired <= d->frame &&
            d->samplers_retired[i].sampler) {
            g_vk.DestroySampler((VkDevice)d->device,
                                (VkSampler)d->samplers_retired[i].sampler, NULL);
        } else {
            d->samplers_retired[keep++] = d->samplers_retired[i];
        }
    }
    d->samplers_retired_count = keep;
}

uint32_t rsx_vk_samplers_pending(const rsx_vk_device* d)
{
    return d ? d->samplers_retired_count : 0;
}

uint32_t rsx_vk_samplers_dropped(const rsx_vk_device* d)
{
    return d ? d->samplers_retired_dropped : 0;
}

/*
 * The nearest-filter question, asked of a handle rather than of a desc.
 *
 * DESIGN §3.1 fallback 2 says a format the device cannot filter linearly is
 * sampled with NEAREST plus one log line instead of returning garbage. But the
 * backend's own observable was only "a sampler was created": the filter comes
 * from a cache key, so a test cannot tell a nearest sampler that was *chosen* for
 * an unfilterable format from a nearest sampler the guest asked for anyway.
 * This answers with the desc the cache actually built, so an L2 test can assert
 * the fallback chose NEAREST for a surface view whose parent is unfilterable.
 */
int rsx_vk_sampler_desc_for(rsx_vk_device* d, uint64_t sampler,
                            rsx_be_sampler_desc* out)
{
    uint32_t i;
    if (!d || !sampler || !out)
        return 0;
    if (sampler == d->null_sampler)
        return 0;   /* outside the cache by design (item 5) */
    for (i = 0; i < RSX_VK_MAX_SAMPLERS; i++) {
        if (d->samplers[i].used && d->samplers[i].sampler == sampler) {
            memcpy(out, &d->samplers[i].desc, sizeof *out);
            return 1;
        }
    }
    return 0;
}

/* Whether the fallback was applied, i.e. the desc was recorded against a
 * format the device cannot filter linearly (filterable == 0). */
uint32_t rsx_vk_samplers_nearest_forced(const rsx_vk_device* d)
{
    return d ? d->samplers_nearest_forced : 0;
}

void rsx_vk_samplers_destroy_all(rsx_vk_device* d)
{
    uint32_t i;
    if (!d || !d->device)
        return;
    for (i = 0; i < RSX_VK_MAX_SAMPLERS; i++) {
        if (d->samplers[i].used && d->samplers[i].sampler) {
            g_vk.DestroySampler((VkDevice)d->device,
                                (VkSampler)d->samplers[i].sampler, NULL);
            d->samplers[i].sampler = 0;
            d->samplers[i].used = 0;
        }
    }
    d->sampler_count = 0;
    /* Every retired sampler dies here too: the device has been waited on, so
     * no command buffer can still name one. */
    for (i = 0; i < d->samplers_retired_count; i++) {
        if (d->samplers_retired[i].sampler) {
            g_vk.DestroySampler((VkDevice)d->device,
                                (VkSampler)d->samplers_retired[i].sampler, NULL);
            d->samplers_retired[i].sampler = 0;
        }
    }
    d->samplers_retired_count = 0;
    /* The null sampler is the device's, not the cache's. */
    if (d->null_sampler) {
        g_vk.DestroySampler((VkDevice)d->device, (VkSampler)d->null_sampler,
                            NULL);
        d->null_sampler = 0;
    }
}
