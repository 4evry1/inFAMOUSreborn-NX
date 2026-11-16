/*
 * ps3recomp - Vulkan backend: pipelines and the draw (slices S2 and S3)
 *
 * DESIGN §1.3 (pipeline_create / pipeline_release) and §1.4 (bind_targets,
 * bind_pipeline, bind_vs_constants, bind_ps_constants, bind_textures,
 * bind_vertex_textures, set_viewport, set_scissor, set_stencil_ref, draw),
 * with §2.2's descriptor layout and §2.4's vertex-input rule.
 *
 * S3 grows the pass: a depth attachment with its clear
 * folding into the load op, the depth-on twin of a pipeline that was created
 * before any zeta was bound, and the texture descriptor sets the PS and the
 * VS both read. The pass scope stays smallest-useful -- one dynamic-rendering
 * pass around one draw, ended by the next set_viewport or by whatever needs
 * the target's layout back. Pass folding and the velocity tracker are S4, so
 * the transitions here are conservative: "to COLOR_ATTACHMENT_OPTIMAL (or
 * DEPTH_STENCIL_ATTACHMENT_OPTIMAL) before the pass, to
 * TRANSFER_SRC_OPTIMAL before a readback" and nothing cleverer.
 *
 * Descriptor strategy: one set per layout per draw from a per-frame pool. The
 * layouts are the ones rsx_vk_device.c built:
 *
 *   set 0  b0  UNIFORM_BUFFER_DYNAMIC  VS  range RSX_VK_VP_CB_BYTES
 *          b1  UNIFORM_BUFFER_DYNAMIC  PS
 *   set 1  t0..t15  SAMPLED_IMAGE      PS
 *          t16..t19 SAMPLED_IMAGE      VS
 *   set 2  s0..s15  SAMPLER            PS
 *          s0..s3   SAMPLER            VS
 *
 * Both stages' image layouts declare the whole unit range, and a descriptor
 * set carries one stage mask, so the VS pair is bound as its own set 1/set 2
 * at the vertex stage (rsx_vk_draw). Every binding of a declared layout is
 * written on every draw: an unwritten descriptor is undefined behaviour in
 * Vulkan, not "unused", so an unbound unit gets the null image and the null
 * sampler rather than nothing.
 *
 * vkCmdPushDescriptorSetKHR is what DESIGN §2.3 prefers, but it needs
 * VK_KHR_push_descriptor to be enabled and is not the whole of the per-draw
 * cost here anyway; the backend takes the descriptor-set route (a pool, one
 * set per layout per draw, reset when the frame's fence passes), which is the
 * documented fallback and is what the 40-descriptor budget in §2.3 allows.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* ---- shader module cache (DESIGN §1.3: modules are shared) ------------- */

static uint64_t fnv1a(const void* data, size_t n, uint64_t h)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static int module_find(rsx_vk_device* d, uint64_t hash, int stage)
{
    uint32_t i;
    for (i = 0; i < d->module_count; i++)
        if (d->modules[i].hash == hash && d->modules[i].stage == (uint32_t)stage)
            return (int)i;
    return -1;
}

/* The oldest entry, so the cache cannot grow without bound on a long run. */
static uint32_t module_victim(rsx_vk_device* d)
{
    uint32_t i, oldest = 0;
    for (i = 1; i < d->module_count; i++)
        if (d->modules[i].last_use < d->modules[oldest].last_use)
            oldest = i;
    return oldest;
}

/* ---- the on-disk SPIR-V cache ------------------------------------------------------------------------ */
static char g_spv_cache_dir[512];
static int g_spv_cache_write;
static int g_spv_cache_env_read;

/* `dir` NULL or empty turns the cache off. `write` lets a miss be stored. The host tools set it from the
 * environment instead: RSX_VK_SPV_CACHE=<dir> (read and write). */
void rsx_vk_set_spv_cache_dir(const char* dir, int write)
{
    g_spv_cache_env_read = 1;
    snprintf(g_spv_cache_dir, sizeof g_spv_cache_dir, "%s", dir ? dir : "");
    g_spv_cache_write = write;
}

static const char* spv_cache_dir(void)
{
    if (!g_spv_cache_env_read) {
        const char* env = getenv("RSX_VK_SPV_CACHE");
        g_spv_cache_env_read = 1;
        if (env && *env) { snprintf(g_spv_cache_dir, sizeof g_spv_cache_dir, "%s", env); g_spv_cache_write = 1; }
    }
    return g_spv_cache_dir[0] ? g_spv_cache_dir : NULL;
}

static void spv_cache_path(char* path, size_t size, const char* dir, uint64_t hash, int stage)
{
    snprintf(path, size, "%s/%s-%016llx.spv", dir, stage == RSX_VK_STAGE_VERTEX ? "vs" : "ps",
             (unsigned long long)hash);
}

static int spv_cache_load(rsx_vk_device* d, uint64_t hash, int stage, rsx_vk_spv* out)
{
    const char* dir = spv_cache_dir();
    char path[640], log[256];
    FILE* f;
    long size;
    void* bytes;
    int ok = 0;
    if (!dir) return 0;
    spv_cache_path(path, sizeof path, dir, hash, stage);
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && size <= (8L << 20) && fseek(f, 0, SEEK_SET) == 0 &&
        (bytes = malloc((size_t)size)) != NULL) {
        if (fread(bytes, 1, (size_t)size, f) == (size_t)size &&
            rsx_vk_spirv_from_blob(bytes, (size_t)size, out, log, sizeof log) == 0)
            ok = 1;
        else
            rsx_vk_log(d, "SPIR-V cache: %s is not usable", path);
        free(bytes);
    }
    fclose(f);
    if (ok) d->spv_cache_hits++;
    return ok;
}

static void spv_cache_store(rsx_vk_device* d, uint64_t hash, int stage, const rsx_vk_spv* spv)
{
    const char* dir = spv_cache_dir();
    char path[640];
    FILE* f;
    if (!dir || !g_spv_cache_write || !spv->words || !spv->word_count) return;
    spv_cache_path(path, sizeof path, dir, hash, stage);
    f = fopen(path, "wb");
    if (!f) return;
    if (fwrite(spv->words, 4u, spv->word_count, f) == spv->word_count) d->spv_cache_stores++;
    fclose(f);
}

enum { RSX_VK_FAILED_SHADERS = 4096 };
static uint64_t g_failed_hash[RSX_VK_FAILED_SHADERS];
static int g_failed_stage[RSX_VK_FAILED_SHADERS];
static uint32_t g_failed_count;

uint64_t rsx_vk_module_get(rsx_vk_device* d, const char* hlsl, int stage,
                           int* out_colour_targets)
{
    char log[1024];
    rsx_vk_spv spv;
    VkShaderModuleCreateInfo ci;
    VkShaderModule mod = VK_NULL_HANDLE;
    uint64_t hash;
    int slot, checked, targets = 0, from_cache = 0;

    if (out_colour_targets)
        *out_colour_targets = 0;
    if (!hlsl || !d->device)
        return 0;

    hash = fnv1a(hlsl, strlen(hlsl), 1469598103934665603ull);
    hash = fnv1a(&stage, sizeof stage, hash);
    slot = module_find(d, hash, stage);
    if (slot >= 0) {
        d->modules[slot].last_use = ++d->module_clock;
        if (out_colour_targets)
            *out_colour_targets = d->modules[slot].colour_targets;
        return d->modules[slot].module;
    }

    {   /* Root diagnostic (2026-11-12): RSX_VK_DUMP_HLSL=<dir> writes every new shader's HLSL there. */
        const char* dump = getenv("RSX_VK_DUMP_HLSL");
        if (dump) {
            char path[512];
            FILE* f;
            snprintf(path, sizeof path, "%s/%s-%016llx.hlsl", dump, stage == RSX_VK_STAGE_VERTEX ? "vs" : "ps",
                     (unsigned long long)hash);
            f = fopen(path, "w");
            if (f) { fputs(hlsl, f); fclose(f); }
        }
    }
    /* Root (2026-11-12): the on-disk SPIR-V cache, keyed by the same hash as the module table (the HLSL text
     * and the stage). A hit skips the HLSL translation, so a build without glslang (the Switch) can run every
     * shader a host run has already translated; a miss is translated and, when the directory is writable,
     * stored. The cached words pass the same verification as fresh ones. */
    {   /* Root (2026-11-12, Decision 92): a shader that already failed fails again without work or log lines.
         * Console run rv23 logged 337 failures of 156 shaders, each with its whole source, every time it was asked. */
        uint32_t known;
        for (known = 0; known < g_failed_count; known++)
            if (g_failed_hash[known] == hash && g_failed_stage[known] == stage) return 0;
    }
    from_cache = spv_cache_load(d, hash, stage, &spv);
    if (!from_cache && rsx_vk_hlsl_to_spirv(hlsl, stage, &spv, log, sizeof log) != 0) {
        const char* dir = spv_cache_dir();
        int saved = 0;
        if (g_failed_count < RSX_VK_FAILED_SHADERS) {
            g_failed_hash[g_failed_count] = hash; g_failed_stage[g_failed_count] = stage; g_failed_count++;
        }
        /* The source goes to <cache dir>-missing/<vs|ps>-<hash>.hlsl, named by the cache key, so a host can
         * translate it and put the result in the cache under the same name. */
        if (dir) {
            char path[700];
            FILE* f;
            snprintf(path, sizeof path, "%s-missing/%s-%016llx.hlsl", dir, stage == RSX_VK_STAGE_VERTEX ? "vs" : "ps",
                     (unsigned long long)hash);
            f = fopen(path, "rb");
            if (f) { fclose(f); saved = 2; }
            else if ((f = fopen(path, "wb")) != NULL) { saved = fputs(hlsl, f) >= 0; fclose(f); }
        }
        rsx_vk_log(d, "RSX_VK_SHADER_FAILED (%s) %016llx: %s (source %s)",
                   stage == RSX_VK_STAGE_VERTEX ? "VS" : "PS", (unsigned long long)hash, log,
                   saved == 2 ? "already saved" : saved ? "saved for translation" : "not saved");
        if (!saved)   /* no place to save it: the text itself, once */
            rsx_vk_log(d, "---- HLSL (failing %s) ----\n%s",
                       stage == RSX_VK_STAGE_VERTEX ? "VS" : "PS", hlsl);
        return 0;
    }

    if (!from_cache)
        spv_cache_store(d, hash, stage, &spv);
    checked = rsx_vk_spv_verify(&spv, stage, log, sizeof log);
    if (checked < 0) {
        rsx_vk_log(d, "RSX_VK_SPV_REJECTED: %s", log);
        d->spv_rejected++;
        rsx_vk_spirv_free(&spv);
        return 0;
    }
    d->spv_verified++;
    targets = rsx_vk_spv_colour_targets(&spv);

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)spv.word_count * 4u;
    ci.pCode = spv.words;
    if (g_vk.CreateShaderModule((VkDevice)d->device, &ci, NULL, &mod) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateShaderModule failed (%s)",
                   stage == RSX_VK_STAGE_VERTEX ? "VS" : "PS");
        rsx_vk_spirv_free(&spv);
        return 0;
    }
    rsx_vk_log(d, "shader module %s: %lub, %d bindings checked, %d colour "
               "targets%s",
               stage == RSX_VK_STAGE_VERTEX ? "VS" : "PS",
               (unsigned long)((uint64_t)spv.word_count * 4u), checked,
               targets, from_cache ? " (from the SPIR-V cache)" : "");
    rsx_vk_spirv_free(&spv);
    d->modules_created++;

    if (d->module_count >= RSX_VK_MAX_MODULES) {
        const uint32_t victim = module_victim(d);
        g_vk.DestroyShaderModule((VkDevice)d->device,
                                 (VkShaderModule)d->modules[victim].module, NULL);
        slot = (int)victim;
    } else {
        slot = (int)d->module_count++;
    }
    d->modules[slot].hash = hash;
    d->modules[slot].module = (uint64_t)(uintptr_t)mod;
    d->modules[slot].stage = (uint32_t)stage;
    d->modules[slot].last_use = ++d->module_clock;
    d->modules[slot].colour_targets = targets;
    if (out_colour_targets)
        *out_colour_targets = targets;
    return (uint64_t)(uintptr_t)mod;
}

void rsx_vk_modules_destroy_all(rsx_vk_device* d)
{
    uint32_t i;
    if (!d || !d->device)
        return;
    for (i = 0; i < d->module_count; i++) {
        if (d->modules[i].module)
            g_vk.DestroyShaderModule((VkDevice)d->device,
                                     (VkShaderModule)d->modules[i].module, NULL);
        memset(&d->modules[i], 0, sizeof(d->modules[i]));
    }
    d->module_count = 0;
}

/* ---- state translation tables ------------------------------------------ */

/* RSX blend factor -> VkBlendFactor. The guest enum is the 4-bit NV40 one. */
/* Root (2026-11-12): RSX blend factors, equations and stencil operations are GL enums (0x0302 SRC_ALPHA,
 * 0x8006 FUNC_ADD, 0x1E01 REPLACE ...), the same tables as rsx_d3d12_backend.c's gl_blend_factor_d3d,
 * gl_blend_op_d3d and st_op. The earlier tables here read the low bits as a 0..15 index, which turned
 * SRC_ALPHA / ONE_MINUS_SRC_ALPHA into SRC_COLOR / ONE_MINUS_SRC_COLOR: inFAMOUS's install-screen text came
 * out as solid white quads (replay of capture r34). The SIGNED equations 0xF005..0xF007 are drawn as their
 * unsigned forms. */
static VkBlendFactor vk_blend_factor(u32 f, int is_alpha)
{
    switch (f & 0xFFFFu) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return is_alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return is_alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return is_alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return is_alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp vk_blend_op(u32 e)
{
    switch (e & 0xFFFFu) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: case 0xF005: case 0xF007: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD, 0xF006 FUNC_ADD_SIGNED, unset */
    }
}

static VkCompareOp vk_compare(u32 f)
{
    /* CELL_GCM_COMPARE 0x200..0x207, the same table the alpha test uses. */
    switch (f & 0x7u) {
    case 0x1: return VK_COMPARE_OP_LESS;
    case 0x2: return VK_COMPARE_OP_EQUAL;
    case 0x3: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 0x4: return VK_COMPARE_OP_GREATER;
    case 0x5: return VK_COMPARE_OP_NOT_EQUAL;
    case 0x6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 0x7: return VK_COMPARE_OP_ALWAYS;
    default:  return VK_COMPARE_OP_NEVER;
    }
}

static VkStencilOp vk_stencil_op(u32 o)
{
    switch (o & 0xFFFFu) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;   /* 0x1E00 KEEP, unset */
    }
}

/* rsx_topology (rsx_primitives.h:31-38) -> VkPrimitiveTopology. The same 1:1
 * map as rsx_metal_backend.m's topo_to_metal (:557-566); the engine has
 * already expanded everything a host API does not have. */
static VkPrimitiveTopology vk_topology(rsx_topology t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case RSX_TOPOLOGY_LINES:         return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case RSX_TOPOLOGY_LINE_STRIP:    return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:                          return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

/* pipeline_create */

/*
 * One pipeline, from the two modules and the state.
 *
 * Everything a pipeline can be built from arrives as arguments, so the same
 * function serves pipeline_create (the engine's HLSL path), the depth-on twin
 * (same modules, a depth attachment added) and, through the same table, the
 * blob path in rsx_vk_spv_cache.c. `depth_format` is 0 for a pass with no
 * depth attachment, which is what S2 used unconditionally.
 */
static uint32_t pipeline_build(rsx_vk_device* d, uint64_t vs_mod,
                               uint64_t ps_mod, const rsx_be_render_state* rs,
                               const rsx_vertex_layout_plan* layout,
                               u32 vertex_stride, uint32_t vfmt, u32 rt_count,
                               uint32_t depth_format, rsx_vk_pipeline* p)
{
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[RSX_DSP_NUM_VERTEX_ATTR];
    VkPipelineVertexInputStateCreateInfo vin;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState blend[RSX_BE_MAX_COLOR_TARGETS];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[3];
    VkPipelineDynamicStateCreateInfo dynci;
    VkPipelineRenderingCreateInfo render;
    VkFormat formats[RSX_BE_MAX_COLOR_TARGETS];
    VkDescriptorSetLayout sets[2];
    VkPipelineLayoutCreateInfo lci;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo ci;
    VkPipeline vkp = VK_NULL_HANDLE;
    u32 i;
    /* DESIGN §3.1 fallback 3: a colour format with no COLOR_ATTACHMENT_BLEND
     * has its blending forced off, with one log line per format. The bit has to
     * come from the format, not from a constant: the previous cut used
     * `1u << 24`, so *one* log line covered every format and a second format
     * that lost its blend went unreported. */
    int blendable = 1;

    if (depth_format && !(depth_format == d->depth_stencil_format))
        rsx_vk_log(d, "pipeline: depth format 0x%x is not the one init chose "
                   "(0x%x)", depth_format, d->depth_stencil_format);
    for (i = 0; i < RSX_VK_FMT_COUNT; i++) {
        if (d->format_table[RSX_BE_FMT_NONE + i] == vfmt) {
            blendable = (d->format_features[RSX_BE_FMT_NONE + i] &
                         VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? 1 : 0;
            if (!blendable) {
                const uint32_t bit = 1u << i;   /* one bit per rsx_be_format */
                if (!(d->blend_forced_off_mask & bit)) {
                    d->blend_forced_off_mask |= bit;
                    rsx_vk_log(d, "pipeline: fmt %d (0x%x) has no "
                               "COLOR_ATTACHMENT_BLEND, blend_enable forced off "
                               "(DESIGN 3.1 fallback 3)",
                               (int)(RSX_BE_FMT_NONE + i), vfmt);
                }
            }
            break;
        }
    }

    memset(stages, 0, sizeof stages);
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].pName = "main";
    stages[0].module = (VkShaderModule)(uintptr_t)vs_mod;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "main";
    stages[1].module = (VkShaderModule)(uintptr_t)ps_mod;

    /* DESIGN §2.4 / §3.3: every attribute is a float4 at slot*16, because the
     * CPU fetch already converted it (rsx_vertex_compact.c:93-110). */
    memset(&binding, 0, sizeof(binding));
    binding.binding = 0;
    binding.stride = vertex_stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    memset(attrs, 0, sizeof attrs);
    for (i = 0; i < layout->count && i < RSX_DSP_NUM_VERTEX_ATTR; i++) {
        attrs[i].location = i;                 /* the layout slot, not the attr */
        attrs[i].binding = 0;
        attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[i].offset = i * 16u;
    }
    memset(&vin, 0, sizeof(vin));
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vin.vertexBindingDescriptionCount = layout->count ? 1u : 0u;
    vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = layout->count;
    vin.pVertexAttributeDescriptions = attrs;

    memset(&ia, 0, sizeof(ia));
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; /* the engine's default */
    ia.primitiveRestartEnable = VK_FALSE;

    memset(&vp, 0, sizeof(vp));
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    memset(&raster, 0, sizeof(raster));
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.depthClampEnable = VK_FALSE;
    raster.rasterizerDiscardEnable = VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = VK_FALSE;
    /* Root (2026-11-12): the engine is written against the D3D convention (clip +y is the TOP of the
     * target), and draw_set_dynamic gives Vulkan a negative-height viewport to match. With that flip a
     * triangle that is counter-clockwise as the viewer sees it is counter-clockwise for Vulkan too, so the
     * guest's GL enums map directly: FRONT_FACE CW 0x900 / CCW 0x901, CULL_FACE FRONT 0x404 / BACK 0x405 /
     * FRONT_AND_BACK 0x408. Before this the viewport was not flipped and BACK was mapped to FRONT, which
     * culled the front faces of inFAMOUS's install-screen progress bar (replay of capture r33). */
    if (rs->cull_enable) {
        raster.cullMode = (rs->cull_face == 0x404u) ? VK_CULL_MODE_FRONT_BIT
                        : (rs->cull_face == 0x408u) ? VK_CULL_MODE_FRONT_AND_BACK
                                                    : VK_CULL_MODE_BACK_BIT;
    } else {
        raster.cullMode = VK_CULL_MODE_NONE;
    }
    raster.frontFace = (rs->front_face == 0x901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                  : VK_FRONT_FACE_CLOCKWISE;

    memset(&ms, 0, sizeof(ms));
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* The depth/stencil state the guest asked for. With no depth attachment
     * the testing bits have nothing to read, so they are off; with one they
     * are exactly the guest's, which is what makes the depth case of S3 work
     * through the engine's own render state. */
    memset(&ds, 0, sizeof(ds));
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (depth_format && rs->depth_test) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (depth_format && rs->depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = vk_compare(rs->depth_func);
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = (depth_format && rs->stencil_enable) ? VK_TRUE : VK_FALSE;
    ds.front.failOp = vk_stencil_op(rs->s_fail);
    ds.front.passOp = vk_stencil_op(rs->s_zpass);
    ds.front.depthFailOp = vk_stencil_op(rs->s_zfail);
    ds.front.compareOp = vk_compare(rs->s_func);
    ds.front.compareMask = rs->s_func_mask ? rs->s_func_mask : 0xFFu;
    ds.front.writeMask = rs->s_write_mask ? rs->s_write_mask : 0xFFu;
    if (rs->stencil_two_sided) {
        ds.back.failOp = vk_stencil_op(rs->bs_fail);
        ds.back.passOp = vk_stencil_op(rs->bs_zpass);
        ds.back.depthFailOp = vk_stencil_op(rs->bs_zfail);
        ds.back.compareOp = vk_compare(rs->bs_func);
        ds.back.compareMask = ds.front.compareMask;
        ds.back.writeMask = ds.front.writeMask;
    } else {
        ds.back = ds.front;
    }

    /* Every attachment takes target A's mask and blend (DESIGN §1.3, the rule
     * at rsx_metal_backend.m:2549-2568): a zero-initialised secondary
     * attachment writes nothing at all. */
    for (i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++) {
        VkPipelineColorBlendAttachmentState* a = &blend[i];
        memset(a, 0, sizeof(*a));
        if (i >= rt_count)
            continue;
        a->blendEnable = (rs->blend_enable && blendable) ? VK_TRUE : VK_FALSE;
        a->srcColorBlendFactor = vk_blend_factor(rs->sf_rgb, 0);
        a->dstColorBlendFactor = vk_blend_factor(rs->df_rgb, 0);
        a->colorBlendOp = vk_blend_op(rs->eq_rgb);
        a->srcAlphaBlendFactor = vk_blend_factor(rs->sf_a, 1);
        a->dstAlphaBlendFactor = vk_blend_factor(rs->df_a, 1);
        a->alphaBlendOp = vk_blend_op(rs->eq_a);
        /* The guest colour mask is A8R8G8B8 bits; Vulkan's is the same
         * four-channel mask in R,G,B,A order. */
        {
            const u32 m = rs->color_mask ? rs->color_mask : 0xFFFFFFFFu;
            a->colorWriteMask = 0;
            if (m & 0x00FF0000u) a->colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
            if (m & 0x0000FF00u) a->colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
            if (m & 0x000000FFu) a->colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
            if (m & 0xFF000000u) a->colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        }
    }
    memset(&cb, 0, sizeof(cb));
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = rt_count;
    cb.pAttachments = blend;
    cb.logicOpEnable = VK_FALSE;

    memset(dyn, 0, sizeof dyn);
    dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    dyn[2] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    memset(&dynci, 0, sizeof(dynci));
    dynci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynci.dynamicStateCount = 3;
    dynci.pDynamicStates = dyn;

    /* Dynamic rendering: no VkRenderPass, no VkFramebuffer, ever (DESIGN
     * §1.3). The formats must match the VkRenderingInfo the pass supplies, so
     * the array lives in this frame rather than in a static. */
    for (i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        formats[i] = (VkFormat)vfmt;
    memset(&render, 0, sizeof(render));
    render.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render.colorAttachmentCount = rt_count;
    render.pColorAttachmentFormats = formats;
    if (depth_format) {
        const int stencil =
            (depth_format == VK_FORMAT_D24_UNORM_S8_UINT ||
             depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
             depth_format == VK_FORMAT_D16_UNORM_S8_UINT);
        render.depthAttachmentFormat = (VkFormat)depth_format;
        render.stencilAttachmentFormat =
            stencil ? (VkFormat)depth_format : VK_FORMAT_UNDEFINED;
    } else {
        render.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        render.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    }

    /* One set: both constant blocks, the images and the shifted samplers
     * (rsx_vk_device.c's layouts_create says why glslang needs them in one). */
    sets[0] = (VkDescriptorSetLayout)g_layouts.set0_textures;
    memset(&lci, 0, sizeof(lci));
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = sets;
    /* DESIGN §2.2's note: the layouts are fixed templates because glslang's
     * auto-mapped bindings are deterministic for a given version, and
     * rsx_vk_spv_verify checks every module against them. The VS image and VS
     * sampler layouts are the same bindings in the vertex stage, so one
     * layout each serves both stages; they are bound separately per stage
     * when a program samples in the vertex stage (S3's TXL case). */
    if (g_vk.CreatePipelineLayout((VkDevice)d->device, &lci, NULL, &pl) !=
        VK_SUCCESS) {
        rsx_vk_log(d, "vkCreatePipelineLayout failed");
        return 0;
    }

    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &render;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dynci;
    ci.layout = pl;

    if (g_vk.CreateGraphicsPipelines((VkDevice)d->device,
                                     (VkPipelineCache)d->pipeline_cache, 1,
                                     &ci, NULL, &vkp) != VK_SUCCESS) {
        rsx_vk_log(d, "vkCreateGraphicsPipelines failed");
        g_vk.DestroyPipelineLayout((VkDevice)d->device, pl, NULL);
        return 0;
    }
    memset(p, 0, sizeof(*p));
    p->pipeline = (uint64_t)(uintptr_t)vkp;
    p->layout = (uint64_t)(uintptr_t)pl;
    p->vs_module = vs_mod;
    p->ps_module = ps_mod;
    p->rt_count = rt_count;
    p->rt_format = vfmt;
    p->depth_format = depth_format;
    p->rs = *rs;
    p->vlayout = *layout;
    /* The table that was actually handed to vkCreateGraphicsPipelines, so the
     * reflection assertion can compare the compiled SPIR-V against it rather
     * than against the loop that produced it. */
    p->vattr_count = (layout->count < RSX_DSP_NUM_VERTEX_ATTR)
                         ? layout->count
                         : RSX_DSP_NUM_VERTEX_ATTR;
    for (i = 0; i < p->vattr_count; i++) {
        p->vattr[i][0] = attrs[i].location;
        p->vattr[i][1] = attrs[i].binding;
        p->vattr[i][2] = attrs[i].offset;
        p->vattr[i][3] = (uint32_t)attrs[i].format;
    }
    return 1;
}

u32 rsx_vk_pipeline_vertex_attrs(rsx_vk_device* d, u32 pipeline,
                                 uint32_t* out, u32 cap)
{
    rsx_vk_slot* slot;
    u32 i;
    if (!d || !d->handles)
        return 0;
    slot = rsx_vk_handle_get(d->handles, pipeline);
    if (!slot || slot->kind != RSX_VK_SLOT_PIPELINE)
        return 0;
    for (i = 0; i < slot->u.pipeline.vattr_count && out && i < cap; i++) {
        out[i * 4 + 0] = slot->u.pipeline.vattr[i][0];
        out[i * 4 + 1] = slot->u.pipeline.vattr[i][1];
        out[i * 4 + 2] = slot->u.pipeline.vattr[i][2];
        out[i * 4 + 3] = slot->u.pipeline.vattr[i][3];
    }
    return slot->u.pipeline.vattr_count;
}

u32 rsx_vk_pipeline_create(rsx_vk_device* d, const char* vs_hlsl,
                           const char* ps_hlsl, const rsx_be_render_state* rs,
                           const rsx_vertex_layout_plan* layout,
                           u32 vertex_stride, rsx_be_format rt_fmt,
                           u32 rt_count)
{
    uint32_t vfmt;
    uint64_t vs_mod, ps_mod;
    int ps_targets = 0;
    u32 id;
    rsx_vk_slot* slot;

    if (!d->device || !vs_hlsl || !ps_hlsl || !rs || !layout) {
        rsx_vk_log(d, "pipeline_create: null argument");
        return 0;
    }
    vfmt = rsx_vk_format(d, rt_fmt);
    if (!vfmt) {
        rsx_vk_log(d, "pipeline_create: render target format %d has no "
                   "VkFormat", (int)rt_fmt);
        return 0;
    }

    /* DESIGN §1.3: rt_count == 0 is clamped to 1, as Metal does. Vulkan
     * requires the pipeline's colourAttachmentCount to match the pass's, and
     * the engine keys on rt_count, so the two agree by construction. */
    if (rt_count == 0)
        rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS)
        rt_count = RSX_BE_MAX_COLOR_TARGETS;

    vs_mod = rsx_vk_module_get(d, vs_hlsl, RSX_VK_STAGE_VERTEX, NULL);
    ps_mod = rsx_vk_module_get(d, ps_hlsl, RSX_VK_STAGE_FRAGMENT, &ps_targets);
    /* Root (2026-11-12, replay of the first captured inFAMOUS frame): the decompiled fragment program returns a
     * PSOutput struct with two SV_TARGETs while the surface has ONE colour target (the RSX drops the second).
     * Leaving the extra output in crashed lavapipe's rasteriser thread, so for a single-target pass the
     * multi-target entry point is renamed and a one-target `main` that returns its t0 is appended. Passes with
     * two or more targets and still more outputs keep the outputs (logged below). */
    if (vs_mod && ps_mod && ps_targets > (int)rt_count && rt_count == 1) {
        static const char entry[] = "PSOutput main(PSInput input)";
        static const char renamed[] = "PSOutput _rsx_mrt_main(PSInput input)";
        static const char tail[] = "\nfloat4 main(PSInput input) : SV_TARGET { return _rsx_mrt_main(input).t0; }\n";
        const char* at = strstr(ps_hlsl, entry);
        if (at && !strstr(at + 1, entry)) {
            const size_t length = strlen(ps_hlsl);
            char* single = (char*)malloc(length + sizeof(renamed) + sizeof(tail));
            if (single) {
                const size_t head = (size_t)(at - ps_hlsl);
                int single_targets = 0;
                uint64_t single_mod;
                memcpy(single, ps_hlsl, head);
                memcpy(single + head, renamed, sizeof(renamed) - 1u);
                memcpy(single + head + sizeof(renamed) - 1u, at + sizeof(entry) - 1u, length - head - (sizeof(entry) - 1u));
                memcpy(single + length + (sizeof(renamed) - sizeof(entry)), tail, sizeof(tail));
                single_mod = rsx_vk_module_get(d, single, RSX_VK_STAGE_FRAGMENT, &single_targets);
                if (single_mod) {
                    rsx_vk_log(d, "pipeline_create: PS with %d colour outputs reduced to its first for a 1-target pass", ps_targets);
                    ps_mod = single_mod;
                    ps_targets = single_targets;
                }
                free(single);
            }
        }
    }
    if (!vs_mod || !ps_mod) {
        d->pipelines_failed++;
        rsx_vk_log(d, "pipeline_create: shader module failed (VS %llu, PS %llu)",
                   (unsigned long long)vs_mod, (unsigned long long)ps_mod);
        return 0;
    }
    /* Root (2026-11-12, replay of the first captured inFAMOUS frame): the game's fragment program writes two
     * colour outputs while the surface has one target; the RSX drops the extra one. Vulkan does the same:
     * a fragment output with no corresponding attachment is discarded, so this is logged and DRAWN.
     * (DESIGN R6 refused it; that dropped every draw of the frame.) */
    if (ps_targets > (int)rt_count)
        rsx_vk_log(d, "pipeline_create: the PS writes %d colour targets, the pass has %u: the extra outputs are discarded",
                   ps_targets, rt_count);

    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        rsx_vk_log(d, "pipeline_create: handle table full");
        return 0;
    }
    slot->kind = RSX_VK_SLOT_PIPELINE;
    if (!pipeline_build(d, vs_mod, ps_mod, rs, layout, vertex_stride, vfmt,
                        rt_count, 0, &slot->u.pipeline)) {
        rsx_vk_handle_retire(d->handles, id);
        d->pipelines_failed++;
        return 0;
    }
    d->pipelines_created++;
    rsx_vk_log(d, "pipeline %u: %u colour target(s), fmt 0x%x, %u attribute(s),"
               " stride %u -> ok (no depth attachment)",
               id, rt_count, vfmt, (unsigned)layout->count, vertex_stride);
    return id;
}

/*
 * The depth-on twin of a pipeline.
 *
 * The engine builds its pipelines before it knows which zeta a draw binds --
 * pipeline_create is called from eng_pipeline_get, and the depth handle is
 * resolved in the same frame but later -- so a draw that arrives with a depth
 * attachment and a pipeline created without one gets a twin, built from the
 * same two modules, render state and vertex layout the original was built
 * from (all kept in the slot), and reused from then on.
 */
static uint32_t pipeline_depth_variant(rsx_vk_device* d, u32 base_id,
                                       rsx_vk_slot* slot)
{
    rsx_vk_pipeline* p = &slot->u.pipeline;
    rsx_vk_slot* twin = NULL;
    uint32_t id;

    if (p->depth_variant)
        return p->depth_variant;
    id = rsx_vk_handle_alloc(d->handles, &twin);
    if (!id) {
        rsx_vk_log(d, "depth variant: handle table full; the draw is dropped");
        return 0;
    }
    twin->kind = RSX_VK_SLOT_PIPELINE;
    if (!pipeline_build(d, p->vs_module, p->ps_module, &p->rs, &p->vlayout,
                        p->vlayout.stride, p->rt_format, p->rt_count,
                        d->depth_stencil_format, &twin->u.pipeline)) {
        rsx_vk_handle_retire(d->handles, id);
        d->pipelines_failed++;
        rsx_vk_log(d, "depth variant: vkCreateGraphicsPipelines failed");
        return 0;
    }
    twin->u.pipeline.depth_variant = id;
    /* The twin points back at its base so a draw that reaches the twin by
     * handle (rather than through the base slot) still finds the base's id. */
    twin->u.pipeline.variant_of = base_id;
    p->depth_variant = id;
    d->pipelines_created++;
    rsx_vk_log(d, "pipeline depth variant %u: depth format 0x%x, %u colour "
               "target(s)", id, d->depth_stencil_format, p->rt_count);
    return id;
}

void rsx_vk_pipeline_destroy(rsx_vk_device* d, rsx_vk_pipeline* p)
{
    if (!d || !p || !d->device)
        return;
    /* Modules belong to the cache, not to the pipeline. The
     * pipeline layout does belong to the slot, and a VkPipeline keeps it alive
     * by reference, so it dies after the pipeline. */
    if (p->pipeline)
        g_vk.DestroyPipeline((VkDevice)d->device, (VkPipeline)p->pipeline, NULL);
    p->pipeline = 0;
    if (p->layout)
        g_vk.DestroyPipelineLayout((VkDevice)d->device, (VkPipelineLayout)p->layout,
                                   NULL);
    p->layout = 0;
}

void rsx_vk_pipeline_release(rsx_vk_device* d, u32 pipeline)
{
    rsx_vk_slot* slot;
    if (!pipeline) {
        rsx_vk_log(d, "pipeline_release: 0 is not a pipeline");
        return;
    }
    slot = rsx_vk_handle_get(d->handles, pipeline);
    if (!slot || slot->kind != RSX_VK_SLOT_PIPELINE) {
        rsx_vk_log(d, "pipeline_release: %u is not live", pipeline);
        return;
    }
    /* Deferred exactly like a target: a command buffer still recording may
     * name the VkPipeline. The depth-on twin, if one was ever built, goes with
     * it: the twin exists only to serve this pipeline's draws and nothing else
     * can reach it, so retiring the base must retire the twin too. */
    if (slot->u.pipeline.depth_variant)
        rsx_vk_handle_retire(d->handles, slot->u.pipeline.depth_variant);
    rsx_vk_handle_retire(d->handles, pipeline);
}

/* bindings */

/*
 * One descriptor set per draw for the constant blocks plus one for the images
 * and one for the samplers. All three come from the device's pool and are
 * written per draw: the UBO offsets and the texture set both differ per draw,
 * so none of them can be shared inside a frame even when nothing changed.
 */
/*
 * The sampled-read side of a draw's textures, recorded BEFORE the pass begins
 * (item N3).
 *
 * It used to run inside write_descriptor_set, which rsx_vk_draw calls after
 * rsx_vk_pass_begin: the layout transitions were therefore recorded inside the
 * dynamic-rendering scope. Vulkan forbids a queue-family-ownership-free image
 * barrier inside a rendering scope (vkCmdBeginRendering .. vkCmdEndRendering
 * may not contain one), so on a driver that enforces it every sampled target
 * transition was an invalid command, and the image the pass attached could be
 * re-transitioned underneath the pass.
 *
 * Two things happen here, in this order, for every named unit of both stages:
 *
 *  1. A feedback loop is detected: the unit names the image whose view this
 *     draw is about to attach. Sampling an attachment of the same draw is
 *     undefined in general and unsupported in S3; the unit is forced to the
 *     null view (and the null sampler) rather than left to a driver's mercy.
 *     One log line per such draw, counted in d->feedback_loops.
 *  2. Otherwise the image is transitioned to the layout its descriptor
 *     declares, if the tracker says it is somewhere else. Exactly what
 *     write_descriptor_set used to do, now at a point where the barrier is
 *     legal.
 *
 * `suppress[unit]` is 1 for a unit step 1 took away from the descriptor write;
 * the caller passes the same array to bind_sampled_read (the writer).
 */
/*
 * The images and views this draw's pass will attach, resolved BEFORE the pass
 * is begun so begin_sampled_read can recognise a feedback loop (items N3, M1).
 * The list is the colour target's image and its view, then -- when a zeta is
 * bound -- that image and the attachment view it is named by on both the depth
 * and the stencil slots (a D*S8 image is attached once and named by both
 * structs).
 *
 * BOTH the image and the view are latched (item M1). Comparing VkImageView
 * handles alone cannot see self-sampling through a surface view: a unit naming
 * this draw's own target through rsx_vk_surface_view carries a *different* view
 * over the *same* image, so the view comparison missed it, the image was
 * transitioned to SHADER_READ_ONLY before the pass, the pass moved it back to
 * COLOR_ATTACHMENT_OPTIMAL, and the descriptor still declared
 * SHADER_READ_ONLY -- a feedback loop bound as if it were legal. The image is
 * what a layout belongs to (rsx_vk_handle_image already resolves a surface view
 * to its parent), so the image is the thing to compare.
 *
 * Returns the count written; 0 when the target is not a live colour target.
 */
static uint32_t resolve_attachment_views(rsx_vk_device* d, uint64_t out_img[RSX_VK_MAX_ATTACHED],
                                         uint64_t out_view[RSX_VK_MAX_ATTACHED])
{
    rsx_vk_slot* target = rsx_vk_handle_get(d->handles, d->pending_target);
    rsx_vk_slot* zslot;
    uint32_t n = 0;

    memset(out_img, 0, sizeof(uint64_t) * RSX_VK_MAX_ATTACHED);
    memset(out_view, 0, sizeof(uint64_t) * RSX_VK_MAX_ATTACHED);
    if (!target || target->kind != RSX_VK_SLOT_COLOR_TARGET)
        return 0;
    out_img[n] = target->u.color.image;
    out_view[n] = target->u.color.view;
    n++;
    {
        /* Root (Decision 104): the further targets of a multi-target set are attachments too. */
        u32 i;
        for (i = 1; i < d->pending_rt_count && i < RSX_BE_MAX_COLOR_TARGETS; i++) {
            rsx_vk_slot* t = rsx_vk_handle_get(d->handles, d->pending_targets[i]);
            if (!t || t->kind != RSX_VK_SLOT_COLOR_TARGET)
                continue;
            out_img[n] = t->u.color.image;
            out_view[n] = t->u.color.view;
            n++;
        }
    }
    if (!d->pending_depth)
        return n;
    zslot = rsx_vk_handle_get(d->handles, d->pending_depth);
    if (zslot && zslot->kind == RSX_VK_SLOT_DEPTH) {
        out_img[n] = zslot->u.depth.image.image;
        out_view[n] = zslot->u.depth.image.view;
        n++;
        out_img[n] = zslot->u.depth.image.image;
        out_view[n] = zslot->u.depth.image.view;
        n++;
    }
    return n;
}

static void begin_sampled_read(rsx_vk_device* d,
                               const uint64_t attached_img[RSX_VK_MAX_ATTACHED],
                               uint32_t attached_count,
                               unsigned char suppress[RSX_VK_MAX_TEX_UNITS +
                                                      RSX_VK_MAX_VTEX_UNITS],
                               int32_t sampled_layout[RSX_VK_MAX_TEX_UNITS +
                                                      RSX_VK_MAX_VTEX_UNITS])
{
    const uint32_t units = RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS;
    uint32_t u;

    memset(suppress, 0, units);
    for (u = 0; u < units; u++) {
        const int is_vs = (u >= RSX_VK_MAX_TEX_UNITS);
        const u32 unit = is_vs ? (u - RSX_VK_MAX_TEX_UNITS) : u;
        const u32 mask = is_vs ? d->pending_vtex_mask : d->pending_tex_mask;
        const u32 handle = is_vs ? d->pending_vtex[unit] : d->pending_tex[unit];
        uint64_t view = 0;
        uint32_t imgfmt = 0;
        rsx_vk_image* simg;
        uint32_t a;

        /* Every unit's declared layout starts as the colour-read one, which is
         * what a unit the draw does not name keeps (its descriptor points at
         * the null view, in SHADER_READ_ONLY). */
        sampled_layout[u] = (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!((mask >> unit) & 1u) || !handle)
            continue;
        if (!rsx_vk_handle_view(d, handle, &view, &imgfmt))
            continue;
        /* The image behind the handle, resolved through a surface view's
         * parent. NULL for a handle that owns no image the tracker knows. */
        simg = rsx_vk_handle_image(d, handle);
        /* Step 1: the pass's own attachments, by IMAGE (item M1). A unit
         * naming this draw's target through a surface view has its own view
         * but the same image, and that is the loop. */
        for (a = 0; a < attached_count; a++) {
            const uint64_t simg_h = simg ? simg->image : 0;
            if (simg_h && simg_h == attached_img[a]) {
                suppress[u] = 1;
                d->feedback_loops++;
                if (d->feedback_logged < d->feedback_loops) {
                    d->feedback_logged = d->feedback_loops;
                    rsx_vk_log(d, "draw: %s unit %u samples this draw's own "
                               "attachment (image 0x%llx, view 0x%llx); "
                               "feedback loops are not supported in S3, so "
                               "the unit binds the null view",
                               is_vs ? "vertex" : "fragment", unit,
                               (unsigned long long)simg_h,
                               (unsigned long long)view);
                }
                break;
            }
        }
        if (suppress[u])
            continue;
        /* Step 2: the layout the descriptor will declare, and the transition
         * to it (items N3, M2).
         *
         * A depth image is sampled through its DEPTH-only view (item N2), so
         * its layout is DEPTH_STENCIL_READ_ONLY_OPTIMAL -- the layout a depth
         * sampler is allowed to read -- and NOT the colour-read one. Declaring
         * SHADER_READ_ONLY for it while nothing transitioned it (the S3b skip
         * this replaces tested `!rsx_vk_format_is_depth` and simply did
         * nothing) is the descriptor/driver disagreement item M2 names.
         *
         * Every other image goes to SHADER_READ_ONLY_OPTIMAL, here, before
         * the pass exists. */
        if (!simg)
            continue;
        if (rsx_vk_format_is_depth(imgfmt)) {
            if (rsx_vk_image_layout(simg) !=
                (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
                if (rsx_vk_cmd_to_depth_sampled_read(d, simg) == 0)
                    d->sampled_read_transitions++;
            }
            sampled_layout[u] =
                (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        } else {
            if (rsx_vk_image_layout(simg) !=
                (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                if (rsx_vk_cmd_to_sampled_read(d, simg) == 0)
                    d->sampled_read_transitions++;
            }
            sampled_layout[u] =
                (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
}

/*
 * The draw's descriptor set: both constant blocks, every image and every
 * sampler of set 0 (rsx_vk_device.c's layouts_create).
 *
 * Vulkan requires every binding a layout declares to have a valid descriptor:
 * a set left partly unwritten is undefined behaviour, not "unused". So all
 * 2+20+20 bindings are written every draw, and a unit the draw does not name
 * is bound to the null texture (a 1x1 opaque white image, DESIGN §1.1) with
 * its null sampler, which is what the engine's own behaviour is for a unit
 * whose upload failed (rsx_draw_engine.c:757-767).
 *
 * Nothing here transitions an image any more: `suppress` tells this writer
 * which units begin_sampled_read took away (a feedback loop), and every other
 * named unit has already been put in SHADER_READ_ONLY_OPTIMAL by that call,
 * before the pass began (item N3).
 */
static uint64_t bind_sampled_read(
    rsx_vk_device* d, u32 vs_offset, u32 vs_bytes, u32 ps_offset, u32 ps_bytes,
    const unsigned char suppress[RSX_VK_MAX_TEX_UNITS +
                                 RSX_VK_MAX_VTEX_UNITS],
    const int32_t sampled_layout[RSX_VK_MAX_TEX_UNITS +
                                 RSX_VK_MAX_VTEX_UNITS])
{
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetLayout l = (VkDescriptorSetLayout)g_layouts.set0_textures;
    VkDescriptorBufferInfo ubo[2];
    VkDescriptorImageInfo img[RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS];
    VkDescriptorImageInfo smp[RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS];
    VkWriteDescriptorSet w[2 + 2 * (RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS)];
    const VkSampler null_smp = (VkSampler)rsx_vk_null_sampler(d);
    const uint32_t units = RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS;
    uint32_t u, n = 0;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = (VkDescriptorPool)d->descriptor_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &l;
    if (((PFN_vkAllocateDescriptorSets)g_vk.AllocateDescriptorSets)(
            (VkDevice)d->device, &ai, &set) != VK_SUCCESS) {
        /* Every draw takes one set, so this only happens when a frame records
         * more draws than the pool was sized for. The pool must not be reset
         * here: a set a recorded vkCmdBindDescriptorSets named has to stay
         * valid until the submit. */
        rsx_vk_log(d, "descriptor pool of %u sets exhausted in frame %u; the "
                   "draw is dropped", (unsigned)RSX_VK_SETS_PER_FRAME, d->frame);
        return 0;
    }

    memset(ubo, 0, sizeof ubo);
    ubo[0].buffer = (VkBuffer)d->ubo_buf;
    ubo[0].offset = vs_offset;
    ubo[0].range = vs_bytes;
    ubo[1].buffer = (VkBuffer)d->ubo_buf;
    ubo[1].offset = ps_offset;
    ubo[1].range = ps_bytes;
    memset(w, 0, sizeof w);
    for (u = 0; u < 2; u++) {
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = u;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[n].pBufferInfo = &ubo[u];
        n++;
    }

    memset(img, 0, sizeof img);
    memset(smp, 0, sizeof smp);
    for (u = 0; u < units; u++) {
        const int is_vs = (u >= RSX_VK_MAX_TEX_UNITS);
        const u32 unit = is_vs ? (u - RSX_VK_MAX_TEX_UNITS) : u;
        const u32 mask = is_vs ? d->pending_vtex_mask : d->pending_tex_mask;
        const u32 handle = is_vs ? d->pending_vtex[unit] : d->pending_tex[unit];
        const uint64_t sampler = is_vs ? d->pending_vsampler[unit]
                                       : d->pending_sampler[unit];
        uint64_t view = 0;
        uint32_t imgfmt = 0;
        const int named = !suppress[u] && ((mask >> unit) & 1u) && handle &&
                          rsx_vk_handle_view(d, handle, &view, &imgfmt);
        /* The binding must be the one the translated SPIR-V declares. The VS
         * gets its own shifts (item 7): glslang renumbers a stage's resources
         * from zero, so a VS written at the PS's bindings put `register(t16)`'s
         * image at binding 4 and `register(s0)`'s sampler at binding 32 --
         * exactly PS unit 0's two slots. rsx_vk_shader.cpp's translation and
         * rsx_vk_device.c's layout use the same constants. */
        const u32 img_binding = is_vs ? (RSX_VK_VS_IMAGE_BINDING_SHIFT + unit)
                                      : (RSX_VK_IMAGE_BINDING_SHIFT + unit);
        const u32 smp_binding = is_vs ? (RSX_VK_VS_SAMPLER_BINDING_SHIFT + unit)
                                      : (RSX_VK_SAMPLER_BINDING_SHIFT + unit);

        img[u].imageView = named ? (VkImageView)view : (VkImageView)d->null_view;
        {   /* Root diagnostic (2026-11-12): RSX_VK_TRACE=1 says what each named unit really binds. */
            static int trace = -1;
            if (trace < 0) trace = getenv("RSX_VK_TRACE") != NULL;
            if (trace && ((mask >> unit) & 1u))
                rsx_vk_log(d, "draw bind: %s unit %u handle %u named %d suppress %d view 0x%llx fmt 0x%x sampler 0x%llx",
                           is_vs ? "VS" : "PS", unit, handle, named, (int)suppress[u],
                           (unsigned long long)view, imgfmt, (unsigned long long)sampler);
        }
        /* The layout begin_sampled_read actually transitioned (and the tracker
         * recorded) for this unit: SHADER_READ_ONLY for a colour image, 
         * DEPTH_STENCIL_READ_ONLY for a depth image (item M2). The descriptor
         * and the image's layout must be one and the same or the sampler reads
         * through a layout the driver was never told about. A unit the draw
         * does not name keeps the colour-read layout the null view is in. */
        img[u].imageLayout = (VkImageLayout)sampled_layout[u];
        img[u].sampler = VK_NULL_HANDLE;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = img_binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[n].pImageInfo = &img[u];
        n++;

        smp[u].imageView = VK_NULL_HANDLE;
        smp[u].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        smp[u].sampler = (named && sampler) ? (VkSampler)(uintptr_t)sampler
                                            : null_smp;
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet = set;
        w[n].dstBinding = smp_binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w[n].pImageInfo = &smp[u];
        n++;
    }
    ((PFN_vkUpdateDescriptorSets)g_vk.UpdateDescriptorSets)((VkDevice)d->device,
                                                            n, w, 0, NULL);
    return (uint64_t)(uintptr_t)set;
}

void rsx_vk_bind_targets(rsx_vk_device* d, const u32* surfaces, u32 count,
                         u32 depth)
{
    if (!surfaces || !count) {
        rsx_vk_log(d, "bind_targets: no surfaces, draw would have nowhere to go");
        d->pending_target = 0;
        d->pending_depth = 0;
        return;
    }
    /* Root (2026-11-13, Decision 104): every target of the set is latched and attached by rsx_vk_pass_begin.
     * Until now only target A was attached while the pipeline was still built with the engine's rt_count, so a
     * two-target draw ran a 2-attachment pipeline inside a 1-attachment pass: the first inFAMOUS scene (run rv34)
     * drew black and the console's driver lost the device (VkResult -4). A handle that is not a live colour
     * target ends the set there, so the attached targets are always a prefix of what the engine named. */
    {
        u32 i, n = 0;
        if (count > RSX_BE_MAX_COLOR_TARGETS)
            count = RSX_BE_MAX_COLOR_TARGETS;
        for (i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
            d->pending_targets[i] = 0;
        for (i = 0; i < count; i++) {
            rsx_vk_slot* t = rsx_vk_handle_get(d->handles, surfaces[i]);
            if (!t || t->kind != RSX_VK_SLOT_COLOR_TARGET)
                break;
            d->pending_targets[n++] = surfaces[i];
        }
        if (n < count && d->mrt_logged < 8) {
            d->mrt_logged++;
            rsx_vk_log(d, "bind_targets: target %u of %u (handle %u) is not a live colour target; %u attached",
                       n, count, surfaces[n], n);
        }
        d->pending_target = surfaces[0];
        d->pending_rt_count = n ? n : 1;
    }
    if (depth) {
        rsx_vk_slot* z = rsx_vk_handle_get(d->handles, depth);
        if (z && z->kind == RSX_VK_SLOT_DEPTH)
            d->pending_depth = depth;
        else {
            rsx_vk_log(d, "bind_targets: depth %u is not a live zeta", depth);
            d->pending_depth = 0;
        }
    } else {
        d->pending_depth = 0;
    }
}

void rsx_vk_bind_pipeline(rsx_vk_device* d, u32 pipeline)
{
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, pipeline);
    if (!slot || slot->kind != RSX_VK_SLOT_PIPELINE) {
        rsx_vk_log(d, "bind_pipeline: %u is not a live pipeline", pipeline);
        return;
    }
    /* Latched, not recorded: the command buffer the pass will be open on does
     * not exist yet (see record_dynamic_state's comment). rsx_vk_draw records
     * the bind inside the pass. */
    d->pending_pipeline_id = pipeline;
    d->pending_pipeline = slot->u.pipeline.pipeline;
    d->pending_layout = slot->u.pipeline.layout;
    d->pending_pipeline_rt_count = slot->u.pipeline.rt_count;
    d->pending_pipeline_rt_format = slot->u.pipeline.rt_format;
}

void rsx_vk_bind_textures(rsx_vk_device* d, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask)
{
    u32 u;
    if (!textures)
        return;
    memset(d->pending_tex, 0, sizeof d->pending_tex);
    d->pending_tex_mask = mask;
    for (u = 0; u < RSX_VK_MAX_TEX_UNITS; u++) {
        if (!((mask >> u) & 1u))
            continue;
        d->pending_tex[u] = textures[u];
        /* A unit bound to nothing still gets a sampler: the descriptor write
         * needs one, and the null image is what it reads. */
        {
            rsx_be_format be = RSX_BE_FMT_R8G8B8A8;
            rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, textures[u]);
            int filterable = 1;
            if (slot && slot->kind == RSX_VK_SLOT_TEXTURE)
                be = slot->u.texture.image.be_format;
            else if (slot && slot->kind == RSX_VK_SLOT_COLOR_TARGET)
                be = slot->u.color.be_format;
            else if (slot && slot->kind == RSX_VK_SLOT_SURFACE_VIEW) {
                /* A surface view is a second view of its parent's target, and
                 * the format that decides filterability is the *parent's*: the
                 * view's own slot carries a copy of it, which is a copy and not
                 * the authority the descriptor is written from (item 10). */
                rsx_vk_slot* parent =
                    rsx_vk_handle_get(d->handles, slot->u.color.view_of);
                if (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
                    be = parent->u.color.be_format;
                else
                    be = slot->u.color.be_format;
            }
            filterable = rsx_vk_format_filterable(d, be);
            d->pending_sampler[u] = rsx_vk_sampler_get(
                d, samplers ? &samplers[u] : &(rsx_be_sampler_desc){0}, filterable);
        }
    }
}

void rsx_vk_bind_vertex_textures(rsx_vk_device* d, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{    u32 u;
    if (!textures)
        return;
    memset(d->pending_vtex, 0, sizeof d->pending_vtex);
    d->pending_vtex_mask = mask;
    for (u = 0; u < RSX_VK_MAX_VTEX_UNITS; u++) {
        if (!((mask >> u) & 1u))
            continue;
        d->pending_vtex[u] = textures[u];
        {
            rsx_be_format be = RSX_BE_FMT_R8G8B8A8;
            rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, textures[u]);
            if (slot && slot->kind == RSX_VK_SLOT_TEXTURE)
                be = slot->u.texture.image.be_format;
            else if (slot && slot->kind == RSX_VK_SLOT_COLOR_TARGET)
                be = slot->u.color.be_format;
            else if (slot && slot->kind == RSX_VK_SLOT_SURFACE_VIEW) {
                /* The parent's format decides filterability (item 10), as in
                 * bind_textures. */
                rsx_vk_slot* parent =
                    rsx_vk_handle_get(d->handles, slot->u.color.view_of);
                if (parent && parent->kind == RSX_VK_SLOT_COLOR_TARGET)
                    be = parent->u.color.be_format;
                else
                    be = slot->u.color.be_format;
            }
            d->pending_vsampler[u] = rsx_vk_sampler_get(
                d, samplers ? &samplers[u] : &(rsx_be_sampler_desc){0},
                rsx_vk_format_filterable(d, be));
        }
    }
}

/* constants and dynamic state */

/*
 * The two constant blocks go into the UBO slab at a 256-byte alignment (the
 * ring's own, taken from minUniformBufferOffsetAlignment) and the descriptor
 * points at this draw's copy, so the dynamic offset is 0 -- which is what S1
 * got wrong once, pointing every descriptor one block past its data.
 */
void rsx_vk_pass_end(rsx_vk_device* d);
/* Root (2026-11-12, Decision 96): the per-frame rings (constants, geometry, descriptor sets) were sized for a
 * frame of a few hundred draws and are only reset when a frame is presented. The first real scene on the console
 * (run rv26) issues more than 4000 draws before a present; from then on every draw was dropped ("do not fit the
 * UBO slab", "descriptor pool ... exhausted"). When a ring is nearly used up, what has been recorded is submitted
 * and waited for, which resets the rings, and recording goes on in a fresh command buffer. This runs at the start
 * of a draw's constant upload, before anything of that draw is in a ring; the engine uploads both constant blocks
 * for every draw, so no draw keeps an offset from before the flush. */
static void flush_if_rings_nearly_full(rsx_vk_device* d, u32 bytes)
{
    static uint64_t seen_resets = ~(uint64_t)0;
    static uint32_t draws_since_reset, flushes;
    if (seen_resets != (uint64_t)d->frame_resets) { seen_resets = (uint64_t)d->frame_resets; draws_since_reset = 0; }
    draws_since_reset++;
    if (draws_since_reset < RSX_VK_SETS_PER_FRAME - 64u &&
        d->ubo_offset + bytes + 65536u <= RSX_VK_UBO_SLAB_BYTES &&
        d->geom_offset + 4u * 1024u * 1024u <= RSX_VK_GEOM_SLAB_BYTES)
        return;
    rsx_vk_pass_end(d);
    (void)rsx_vk_device_submit_and_wait(d, RSX_VK_FLUSH_IDLE);
    rsx_vk_handle_collect(d);   /* the fence passed: objects retired so far in this long frame can go */
    if (flushes++ < 8u || (flushes & 1023u) == 0u)
        rsx_vk_log(d, "mid-frame flush %u after %u draws (rings reset)", flushes, draws_since_reset);
    seen_resets = (uint64_t)d->frame_resets;
    draws_since_reset = 0;
}

void rsx_vk_bind_vs_constants(rsx_vk_device* d, const void* data, u32 bytes)
{
    void* dst;
    if (!data || !bytes)
        return;
    flush_if_rings_nearly_full(d, bytes);
    dst = rsx_vk_ubo_reserve(d, bytes, 256);
    if (!dst) {
        rsx_vk_log(d, "bind_vs_constants: %u bytes do not fit the UBO slab",
                   bytes);
        return;
    }
    memcpy(dst, data, bytes);
    d->pending_ubo_vs = rsx_vk_ubo_last_offset(d);
    d->pending_ubo_vs_bytes = bytes;
}

void rsx_vk_bind_ps_constants(rsx_vk_device* d, const void* data, u32 bytes)
{
    void* dst;
    if (!data || !bytes)
        return;
    dst = rsx_vk_ubo_reserve(d, bytes, 256);
    if (!dst) {
        rsx_vk_log(d, "bind_ps_constants: %u bytes do not fit the UBO slab",
                   bytes);
        return;
    }
    memcpy(dst, data, bytes);
    d->pending_ubo_ps = rsx_vk_ubo_last_offset(d);
    d->pending_ubo_ps_bytes = bytes;
}

/*
 * The dynamic state the engine latched for this draw. An empty scissor means
 * "no scissor" to the engine (rsx_draw_engine.c:1495-1514) but Vulkan needs a
 * non-empty rect, so it is widened to the attachment extent.
 */
static void record_dynamic_state(rsx_vk_device* d)
{
    VkCommandBuffer cmd = (VkCommandBuffer)rsx_vk_device_cmd(d);
    rsx_vk_slot* target = rsx_vk_handle_get(d->handles, d->pending_target);
    VkViewport vp;
    VkRect2D sc;

    if (!target || target->kind != RSX_VK_SLOT_COLOR_TARGET)
        return;

    vp.x = d->pending_viewport[0];
    vp.y = d->pending_viewport[1];
    vp.width = d->pending_viewport[2];
    vp.height = d->pending_viewport[3];
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    if (!d->pending_viewport_set || vp.width <= 0.0f || vp.height <= 0.0f) {
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = (float)target->u.color.width;
        vp.height = (float)target->u.color.height;
    }
    /* D3D orientation: clip +y at the top (negative height, core since Vulkan 1.1). See pipeline_build. */
    vp.y += vp.height;
    vp.height = -vp.height;
    g_vk.CmdSetViewport(cmd, 0, 1, &vp);

    sc.offset.x = (int32_t)d->pending_scissor[0];
    sc.offset.y = (int32_t)d->pending_scissor[1];
    sc.extent.width = d->pending_scissor[2];
    sc.extent.height = d->pending_scissor[3];
    if (!d->pending_scissor_set || !sc.extent.width || !sc.extent.height) {
        sc.offset.x = 0;
        sc.offset.y = 0;
        sc.extent.width = target->u.color.width;
        sc.extent.height = target->u.color.height;
    }
    g_vk.CmdSetScissor(cmd, 0, 1, &sc);
    g_vk.CmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                               d->pending_stencil_ref & 0xFFu);
}

void rsx_vk_set_viewport(rsx_vk_device* d, float x, float y, float w, float h)
{
    d->pending_viewport[0] = x;
    d->pending_viewport[1] = y;
    d->pending_viewport[2] = w;
    d->pending_viewport[3] = h;
    d->pending_viewport_set = 1;
    /* The engine's setter order is bind_targets .. draw, then the next
     * draw's set_viewport; a pass still open belongs to the draw that ended. */
    if (d->rendering)
        rsx_vk_pass_end(d);
}

void rsx_vk_set_scissor(rsx_vk_device* d, u32 x, u32 y, u32 w, u32 h)
{
    d->pending_scissor[0] = x;
    d->pending_scissor[1] = y;
    d->pending_scissor[2] = w;
    d->pending_scissor[3] = h;
    d->pending_scissor_set = 1;
}

/* Both faces: the pipeline decides which one is read. */
void rsx_vk_set_stencil_ref(rsx_vk_device* d, u32 ref)
{
    d->pending_stencil_ref = ref;
    d->pending_stencil_ref_set = 1;
}

/* the pass */

int rsx_vk_is_rendering(const rsx_vk_device* d)
{
    return d ? d->rendering : 0;
}

/* Colour attachment: COLOR_ATTACHMENT_OPTIMAL, from wherever the tracker says
 * it is (DESIGN §4.3 shape 2, rule 4). A target a texture unit sampled since
 * the last pass is in SHADER_READ_ONLY_OPTIMAL; one that was cleared directly
 * is in GENERAL; a fresh one is UNDEFINED. */
static int to_color_attachment(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    const int32_t want = (int32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    int32_t from;
    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) == want)
        return 0;
    from = rsx_vk_image_layout(img);
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = (from < 0)
                          ? 0u
                          : (VK_ACCESS_MEMORY_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT |
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT);
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = img->mips ? img->mips : 1u;
    b.subresourceRange.layerCount = img->faces ? img->faces : 1u;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                            NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

/* Depth attachment: DEPTH_STENCIL_ATTACHMENT_OPTIMAL (DESIGN §4.3 shape 2). */
static int to_depth_attachment(rsx_vk_device* d, rsx_vk_image* img)
{
    VkImageMemoryBarrier b;
    const int32_t want = (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    int32_t from;
    if (!img || !img->image)
        return -1;
    if (rsx_vk_image_layout(img) == want)
        return 0;
    from = rsx_vk_image_layout(img);
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = (from < 0)
                          ? 0u
                          : (VK_ACCESS_MEMORY_WRITE_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT);
    b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = (VkImageLayout)(from < 0 ? VK_IMAGE_LAYOUT_UNDEFINED : from);
    b.newLayout = (VkImageLayout)want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = (VkImage)img->image;
    b.subresourceRange.aspectMask = rsx_vk_aspect_mask(img->format);
    b.subresourceRange.levelCount = img->mips ? img->mips : 1u;
    b.subresourceRange.layerCount = img->faces ? img->faces : 1u;
    g_vk.CmdPipelineBarrier((VkCommandBuffer)rsx_vk_device_cmd(d),
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            0, 0, NULL, 0, NULL, 1, &b);
    rsx_vk_image_set_layout(img, want);
    return 0;
}

/*
 * The pass. S3's scope is still one dynamic-rendering pass per draw: begun by
 * rsx_vk_draw, ended by the next set_viewport after it, or by whatever else
 * needs the target's layout back (a clear, a readback, a submit). Pass folding
 * -- contiguous draws on the same attachments sharing one pass -- is S4; what
 * S3 adds to the pass is the depth attachment, its clear-to-load-op and the
 * layout transitions that a depth target needs.
 *
 * Colour attachments load rather than clear: the engine issues clear_color
 * itself and it is recorded as its own vkCmdClearColorImage, so a pass that
 * cleared here would undo it. A depth clear folds into the load op instead,
 * because there is no vkCmdClearDepthStencilImage in the folded form this pass
 * needs and a load-op clear is exactly what DESIGN §1.5 describes.
 */
int rsx_vk_pass_begin(rsx_vk_device* d, u32 surface)
{
    VkRenderingAttachmentInfo att[RSX_BE_MAX_COLOR_TARGETS];
    rsx_vk_slot* extra[RSX_BE_MAX_COLOR_TARGETS];
    u32 att_count, ai;
    u32 area_w, area_h;
    VkRenderingAttachmentInfo depth_att;
    VkRenderingAttachmentInfo stencil_att;
    VkRenderingInfo ri;
    rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, surface);
    rsx_vk_slot* zslot = NULL;

    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "pass: surface %u is not a live colour target", surface);
        return -1;
    }
    if (d->rendering && d->rendering_target == surface)
        return 0;    /* same pass still open */
    rsx_vk_pass_end(d);
    if (rsx_vk_device_begin(d) != 0)
        return -1;
    if (to_color_attachment(d, &slot->u.color) != 0)
        return -1;
    /* Root (Decision 104): the pass has exactly as many colour attachments as the bound pipeline was built with
     * (Vulkan requires the two counts to agree). Attachment i is the i-th bound target; a position the engine
     * bound no live target for gets a null view, which Vulkan defines as "writes are discarded". */
    att_count = d->pending_pipeline_rt_count ? d->pending_pipeline_rt_count : 1u;
    if (att_count > RSX_BE_MAX_COLOR_TARGETS)
        att_count = RSX_BE_MAX_COLOR_TARGETS;
    area_w = slot->u.color.width;
    area_h = slot->u.color.height;
    extra[0] = slot;
    for (ai = 1; ai < att_count; ai++) {
        extra[ai] = NULL;
        if (ai < d->pending_rt_count && d->pending_targets[ai] && d->pending_targets[0] == surface) {
            rsx_vk_slot* t = rsx_vk_handle_get(d->handles, d->pending_targets[ai]);
            u32 k;
            int repeated = 0;
            if (!t || t->kind != RSX_VK_SLOT_COLOR_TARGET)
                continue;
            for (k = 0; k < ai; k++)
                if (extra[k] && extra[k]->u.color.image == t->u.color.image)
                    repeated = 1;
            if (repeated || t->u.color.format != slot->u.color.format) {
                if (d->mrt_logged < 8) {
                    d->mrt_logged++;
                    rsx_vk_log(d, "pass: target %u (handle %u) %s; it is left out",
                               ai, d->pending_targets[ai],
                               repeated ? "repeats an earlier target" : "has another format than target A");
                }
                continue;
            }
            if (to_color_attachment(d, &t->u.color) != 0)
                continue;
            extra[ai] = t;
            if (t->u.color.width < area_w)
                area_w = t->u.color.width;
            if (t->u.color.height < area_h)
                area_h = t->u.color.height;
        }
    }

    if (d->pending_depth) {
        zslot = rsx_vk_handle_get(d->handles, d->pending_depth);
        if (!zslot || zslot->kind != RSX_VK_SLOT_DEPTH) {
            rsx_vk_log(d, "pass: depth %u is not a live zeta", d->pending_depth);
            return -1;
        }
        if (zslot->u.depth.image.width < slot->u.color.width ||
            zslot->u.depth.image.height < slot->u.color.height) {
            /* The engine sizes every zeta to cover the whole canvas
             * (rsx_draw_engine.c:618-631 says why), so a smaller one is a
             * real mismatch and the pass would be rejected. */
            rsx_vk_log(d, "pass: depth %ux%u is smaller than the %ux%u target",
                       zslot->u.depth.image.width,
                       zslot->u.depth.image.height, slot->u.color.width,
                       slot->u.color.height);
            return -1;
        }
        if (to_depth_attachment(d, &zslot->u.depth.image) != 0)
            return -1;
    }

    memset(att, 0, sizeof(att));
    for (ai = 0; ai < att_count; ai++) {
        att[ai].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att[ai].imageView = extra[ai] ? (VkImageView)extra[ai]->u.color.view : (VkImageView)0;
        att[ai].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[ai].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[ai].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (att_count > 1)
        d->mrt_passes++;

    memset(&ri, 0, sizeof(ri));
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset.x = 0;
    ri.renderArea.offset.y = 0;
    ri.renderArea.extent.width = area_w;
    ri.renderArea.extent.height = area_h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = att_count;
    ri.pColorAttachments = att;
    ri.pDepthAttachment = NULL;
    ri.pStencilAttachment = NULL;

    if (zslot) {
        rsx_vk_depth* z = &zslot->u.depth;
        const int has_stencil = (rsx_vk_aspect_mask(z->image.format) &
                                 VK_IMAGE_ASPECT_STENCIL_BIT) ? 1 : 0;
        VkAttachmentLoadOp stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        memset(&depth_att, 0, sizeof depth_att);
        depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_att.imageView = (VkImageView)z->image.view;
        depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        /* Per aspect (item 8): CLEAR only the aspects a clear actually named.
         * A stencil-only clear must not reset depth; a depth-only clear must
         * not reset stencil. A pass that clears one aspect and loads the other
         * cannot be expressed with one VkRenderingAttachmentInfo loadOp, so
         * the attachment is split: the depth attachment carries the depth
         * aspect's loadOp and the stencil attachment carries the stencil
         * aspect's, which is exactly what Vulkan offers for a D*S8 format.
         *
         * With neither pending the pass loads both, as before. */
        if (z->depth_clear_pending || z->stencil_clear_pending) {
            depth_att.loadOp = z->depth_clear_pending
                                   ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                   : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_att.clearValue.depthStencil.depth = z->clear_depth;
            depth_att.clearValue.depthStencil.stencil =
                (uint32_t)z->clear_stencil;
            stencil_load = z->stencil_clear_pending
                               ? VK_ATTACHMENT_LOAD_OP_CLEAR
                               : VK_ATTACHMENT_LOAD_OP_LOAD;
            z->depth_clear_pending = 0;
            z->stencil_clear_pending = 0;
            z->clear_pending = 0;
            d->depth_clears_applied++;
        } else {
            depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &depth_att;
        /* A stencil-only clear cannot ride on the depth attachment's loadOp
         * without also clearing depth, and a pass that clears one aspect while
         * loading the other has no single VkAttachmentLoadOp to describe it.
         * The required form there is TWO VkRenderingAttachmentInfo over the
         * same image -- one for the depth aspect, one for the stencil aspect --
         * each carrying its own loadOp; Vulkan 1.3's dynamic rendering
         * explicitly provides pStencilAttachment for exactly this and does not
         * forbid it naming the same image as pDepthAttachment. It is not a
         * spec violation.
         *
         * This comment used to claim the opposite -- that two structs over one
         * image were "a spec violation that a lenient driver may accept by
         * silently dropping the depth write" -- and the code then avoided them
         * whenever the two loadOps agreed. That was wrong on both counts (item
         * N4): the split form is what the spec asks for whenever the loadOps
         * differ, and a driver that accepts one struct for both aspects is the
         * normal case, not a lenient exception. So: one struct when both
         * aspects want the same loadOp, two when they differ, which is what the
         * branch below does. */
        if (!has_stencil)
            ri.pStencilAttachment = NULL;
        else if (depth_att.loadOp == stencil_load)
            ri.pStencilAttachment = &depth_att;
        else {
            memset(&stencil_att, 0, sizeof stencil_att);
            stencil_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            stencil_att.imageView = (VkImageView)z->image.view;
            stencil_att.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            stencil_att.loadOp = stencil_load;
            stencil_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            stencil_att.clearValue.depthStencil.stencil =
                (uint32_t)z->clear_stencil;
            ri.pStencilAttachment = &stencil_att;
        }
    }

    g_vk.CmdBeginRenderingKHR((VkCommandBuffer)rsx_vk_device_cmd(d), &ri);
    d->rendering = 1;
    d->rendering_target = (uint64_t)surface;
    d->rendering_depth = (uint64_t)d->pending_depth;
    d->passes_begun++;
    return 0;
}

void rsx_vk_pass_end(rsx_vk_device* d)
{
    if (!d || !d->rendering)
        return;
    g_vk.CmdEndRenderingKHR((VkCommandBuffer)rsx_vk_device_cmd(d));
    d->rendering = 0;
    d->rendering_target = 0;
    d->rendering_depth = 0;
}

/* the draw */

/*
 * The draw. The vertex input comes from the draw's own arguments and the
 * vertex layout plan the pipeline was built from (slot*16, DESIGN §2.4), and
 * the reflection assertion of the S3 test checks that table against the
 * locations each compiled program actually declares.
 */
void rsx_vk_draw(rsx_vk_device* d, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count)
{
    VkDeviceSize offsets[1];
    uint64_t dset = 0;
    VkDescriptorSet dset_vk;
    uint32_t dynamic_offsets[2];
    rsx_vk_slot* target;
    rsx_vk_slot* pslot;
    VkPipeline pipe;
    unsigned char suppress[RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS];
    int32_t sampled_layout[RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS];
    uint64_t attached_img[RSX_VK_MAX_ATTACHED];
    uint64_t attached_view[RSX_VK_MAX_ATTACHED];
    uint32_t attached_count;

    if (!vertices || !vertex_count || !stride) {
        rsx_vk_log(d, "draw: nothing to draw (%u vertices, stride %u)",
                   vertex_count, stride);
        return;
    }
    target = rsx_vk_handle_get(d->handles, d->pending_target);
    if (!target || target->kind != RSX_VK_SLOT_COLOR_TARGET) {
        rsx_vk_log(d, "draw: no live target bound (pending %u)",
                   d->pending_target);
        return;
    }
    pslot = rsx_vk_handle_get(d->handles, d->pending_pipeline_id);
    if (!pslot || pslot->kind != RSX_VK_SLOT_PIPELINE || !pslot->u.pipeline.pipeline) {
        rsx_vk_log(d, "draw: no pipeline bound; the draw is dropped");
        return;
    }

    /* A depth attachment and a pipeline built without one are incompatible:
     * vkCmdBeginRendering rejects a pass whose attachment formats differ from
     * the pipeline's, and a pipeline cannot be patched. The twin is built from
     * the pipeline's own inputs (its slot keeps them) and reused from then on;
     * the engine keeps binding the original, so this check is what finds the
     * twin again next draw.
     *
     * `d->pending_pipeline_id` is deliberately NOT rewritten to the twin's
     * handle. It used to be, and the consequence was that the *next* draw of
     * the frame -- the one with no zeta bound -- ran the depth-on twin inside a
     * pass with no depth attachment, which Vulkan rejects. The twin is chosen
     * per draw from `pending_depth` and named only in this local. */
    if (d->pending_depth && !pslot->u.pipeline.depth_format) {
        const u32 variant = pipeline_depth_variant(d, d->pending_pipeline_id,
                                                   pslot);
        if (!variant)
            return;
        pslot = rsx_vk_handle_get(d->handles, variant);
        if (!pslot || pslot->u.pipeline.depth_format != d->depth_stencil_format) {
            rsx_vk_log(d, "draw: the depth variant of the pipeline is gone");
            return;
        }
    } else if (!d->pending_depth && pslot->u.pipeline.depth_format) {
        /* The engine can only ever bind a pipeline it created, and it creates
         * them without depth (pipeline_create passes no depth format), so this
         * is a defensive branch rather than a path: a depth-on pipeline bound
         * with no zeta cannot be begun. */
        rsx_vk_log(d, "draw: pipeline %u wants depth format 0x%x but the draw "
                   "bound no zeta", d->pending_pipeline_id,
                   pslot->u.pipeline.depth_format);
        return;
    }
    pipe = (VkPipeline)pslot->u.pipeline.pipeline;

    /* Every open texture upload batch ends here: a draw is the point at which
     * the rows have to be visible to a sampler (DESIGN §4.3 shape 1). */
    rsx_vk_textures_end_uploads(d);

    /* The sampled images' layout transitions belong to the command buffer's
     * *outside* scope, so they are recorded before the pass exists (item N3),
     * and a unit naming one of this draw's own attachments is dropped to the
     * null view there rather than bound and left to the driver. The
     * attachment views have to be resolved here, before pass_begin, because
     * that is the whole point of doing this first. */
    attached_count = resolve_attachment_views(d, attached_img, attached_view);
    begin_sampled_read(d, attached_img, attached_count, suppress,
                       sampled_layout);

    /* The pending dynamic state (viewport, scissor, stencil reference) is
     * recorded by rsx_vk_pass_begin once the command buffer exists. */
    if (rsx_vk_pass_begin(d, d->pending_target) != 0)
        return;
    g_vk.CmdBindPipeline((VkCommandBuffer)rsx_vk_device_cmd(d),
                         VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    record_dynamic_state(d);

    /* Vertices (and indices) go into the geometry slab, at a 16-byte align. */
    {
        const u32 vbytes = vertex_count * stride;
        const u32 ibytes = (indices && index_count) ? index_count * 4u : 0u;
        uint32_t voff = 0, ioff = 0;
        void* vdst;
        void* idst;
        vdst = rsx_vk_stage_reserve(d, vbytes, 16, &voff);
        idst = ibytes ? rsx_vk_stage_reserve(d, ibytes, 4, &ioff) : NULL;
        if (!vdst || (ibytes && !idst)) {
            rsx_vk_log(d, "draw: %u + %u bytes do not fit the geometry slab",
                       vbytes, ibytes);
            rsx_vk_pass_end(d);
            return;
        }
        memcpy(vdst, vertices, vbytes);
        if (idst)
            memcpy(idst, indices, ibytes);

        memset(offsets, 0, sizeof offsets);
        offsets[0] = voff;
        g_vk.CmdBindVertexBuffers((VkCommandBuffer)rsx_vk_device_cmd(d), 0, 1,
                                  (VkBuffer*)&d->geom_buf, offsets);
        if (idst)
            g_vk.CmdBindIndexBuffer((VkCommandBuffer)rsx_vk_device_cmd(d),
                                    (VkBuffer)d->geom_buf, ioff,
                                    VK_INDEX_TYPE_UINT32);
    }

    /* S3's pipelines are built for a triangle list, which is what the engine
     * hands over for anything it did not rebuild, and what it rebuilds
     * everything else into (§1.4). The map is still used, so that a topology
     * the pipeline cannot express is a log line rather than a wrong draw. */
    if (vk_topology(topology) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        rsx_vk_log(d, "draw: topology %d is not a triangle list; the pipeline "
                   "was built for a list (DESIGN 1.4)", (int)topology);

    dset = bind_sampled_read(d, d->pending_ubo_vs, d->pending_ubo_vs_bytes,
                             d->pending_ubo_ps, d->pending_ubo_ps_bytes, suppress,
                             sampled_layout);
    if (!dset) {
        rsx_vk_log(d, "draw: descriptor set unavailable; the draw is dropped");
        rsx_vk_pass_end(d);
        return;
    }
    /* The ranges in the constant descriptors are the block sizes, so the
     * dynamic offsets are 0: each descriptor already points at this draw's
     * copy of the block. */
    dset_vk = (VkDescriptorSet)(uintptr_t)dset;
    dynamic_offsets[0] = 0;
    dynamic_offsets[1] = 0;
    ((PFN_vkCmdBindDescriptorSets)g_vk.CmdBindDescriptorSets)(
        (VkCommandBuffer)rsx_vk_device_cmd(d), VK_PIPELINE_BIND_POINT_GRAPHICS,
        (VkPipelineLayout)d->pending_layout, 0, 1, &dset_vk, 2, dynamic_offsets);

    if (indices && index_count)
        g_vk.CmdDrawIndexed((VkCommandBuffer)rsx_vk_device_cmd(d), index_count,
                            1, 0, 0, 0);
    else
        g_vk.CmdDraw((VkCommandBuffer)rsx_vk_device_cmd(d), vertex_count, 1, 0,
                     0);
    d->draws_recorded++;
    if (d->pending_depth) {
        rsx_vk_slot* z = rsx_vk_handle_get(d->handles, d->pending_depth);
        if (z && z->kind == RSX_VK_SLOT_DEPTH)
            z->u.depth.had_write = 1;
    }
    rsx_vk_pass_end(d);
}
