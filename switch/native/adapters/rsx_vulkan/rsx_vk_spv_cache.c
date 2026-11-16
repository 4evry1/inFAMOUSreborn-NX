/*
 * ps3recomp - Vulkan backend: the offline SPIR-V cache path
 *
 * Split out of rsx_vk_pipeline.c so the S1 device test, which links the same
 * backend objects, does not drag rsx_draw_engine.c's guest-memory hooks into
 * its link line. The only thing that needs the engine here is
 * rsx_vertex_layout_plan_init, for the default attribute table; the pipeline
 * itself is the same code path as the HLSL one, which is the point.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_internal.h"

/* ---- the blob path used by the S2 test's case 3 ------------------------ *
 *
 * DESIGN §2.5's offline cache: the same pipeline and the same draw, but the
 * two VkShaderModules come from committed SPIR-V bytes instead of glslang.
 * Kept here rather than in the test so the cache path and the HLSL path share
 * one pipeline/draw implementation, which is the only way "the blob works"
 * can mean "the draw works".
 */
uint32_t rsx_vk_spv_blob_pipeline(rsx_vk_device* d, const void* vs_spv,
                                  size_t vs_bytes, const void* ps_spv,
                                  size_t ps_bytes, const rsx_be_render_state* rs,
                                  const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt,
                                  u32 rt_count)
{
    VkShaderModuleCreateInfo mci;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    char log[1024];
    rsx_vk_spv vsv, psv;
    rsx_vk_slot* slot;
    u32 id;

    if (rsx_vk_spirv_from_blob(vs_spv, vs_bytes, &vsv, log, sizeof log) != 0 ||
        rsx_vk_spirv_from_blob(ps_spv, ps_bytes, &psv, log, sizeof log) != 0)
        return 0;
    memset(&mci, 0, sizeof(mci));
    mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mci.codeSize = (size_t)vsv.word_count * 4u;
    mci.pCode = vsv.words;
    if (g_vk.CreateShaderModule((VkDevice)d->device, &mci, NULL, &vs) != VK_SUCCESS)
        vs = VK_NULL_HANDLE;
    mci.codeSize = (size_t)psv.word_count * 4u;
    mci.pCode = psv.words;
    if (g_vk.CreateShaderModule((VkDevice)d->device, &mci, NULL, &ps) != VK_SUCCESS)
        ps = VK_NULL_HANDLE;
    rsx_vk_spirv_free(&vsv);
    rsx_vk_spirv_free(&psv);
    if (!vs || !ps) {
        rsx_vk_log(d, "blob pipeline: vkCreateShaderModule failed");
        return 0;
    }

    id = rsx_vk_handle_alloc(d->handles, &slot);
    if (!id) {
        g_vk.DestroyShaderModule((VkDevice)d->device, vs, NULL);
        g_vk.DestroyShaderModule((VkDevice)d->device, ps, NULL);
        return 0;
    }
    slot->kind = RSX_VK_SLOT_PIPELINE;
    {
        VkGraphicsPipelineCreateInfo ci;
        VkPipelineShaderStageCreateInfo st[2];
        VkVertexInputBindingDescription bind;
        VkVertexInputAttributeDescription at[RSX_DSP_NUM_VERTEX_ATTR];
        VkPipelineVertexInputStateCreateInfo vin;
        VkPipelineInputAssemblyStateCreateInfo ia;
        VkPipelineViewportStateCreateInfo vp;
        VkPipelineRasterizationStateCreateInfo rast;
        VkPipelineMultisampleStateCreateInfo ms;
        VkPipelineDepthStencilStateCreateInfo ds;
        VkPipelineColorBlendAttachmentState ba[RSX_BE_MAX_COLOR_TARGETS];
        VkPipelineColorBlendStateCreateInfo cb;
        VkDynamicState dyn[3];
        VkPipelineDynamicStateCreateInfo dynci;
        VkPipelineRenderingCreateInfo render;
        VkFormat formats[RSX_BE_MAX_COLOR_TARGETS];
        VkDescriptorSetLayout sets[1];
        VkPipelineLayoutCreateInfo lci;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipeline vkp = VK_NULL_HANDLE;
        uint32_t vfmt, i;
        const u32 rt = rt_count ? (rt_count > RSX_BE_MAX_COLOR_TARGETS
                                       ? RSX_BE_MAX_COLOR_TARGETS
                                       : rt_count)
                                : 1u;

        vfmt = rsx_vk_format(d, rt_fmt);
        if (!vfmt)
            return 0;
        memset(st, 0, sizeof st);
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].pName = "main";
        st[0].module = vs;
        st[1] = st[0];
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = ps;

        memset(&bind, 0, sizeof bind);
        bind.binding = 0;
        bind.stride = vertex_stride;
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        memset(at, 0, sizeof at);
        for (i = 0; i < layout->count && i < RSX_DSP_NUM_VERTEX_ATTR; i++) {
            at[i].location = i;
            at[i].binding = 0;
            at[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            at[i].offset = i * 16u;
        }
        memset(&vin, 0, sizeof vin);
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vin.vertexBindingDescriptionCount = layout->count ? 1u : 0u;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = layout->count;
        vin.pVertexAttributeDescriptions = at;
        memset(&ia, 0, sizeof ia);
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        memset(&vp, 0, sizeof vp);
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        memset(&rast, 0, sizeof rast);
        rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rast.polygonMode = VK_POLYGON_MODE_FILL;
        rast.lineWidth = 1.0f;
        rast.cullMode = VK_CULL_MODE_NONE;
        rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        memset(&ms, 0, sizeof ms);
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        memset(&ds, 0, sizeof ds);
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        memset(ba, 0, sizeof ba);
        for (i = 0; i < rt; i++) {
            ba[i].blendEnable = VK_FALSE;
            ba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba[i].colorBlendOp = VK_BLEND_OP_ADD;
            ba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba[i].alphaBlendOp = VK_BLEND_OP_ADD;
            ba[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                   VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT |
                                   VK_COLOR_COMPONENT_A_BIT;
        }
        memset(&cb, 0, sizeof cb);
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = rt;
        cb.pAttachments = ba;
        dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
        dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
        dyn[2] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
        memset(&dynci, 0, sizeof dynci);
        dynci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynci.dynamicStateCount = 3;
        dynci.pDynamicStates = dyn;
        for (i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
            formats[i] = (VkFormat)vfmt;
        memset(&render, 0, sizeof render);
        render.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        render.colorAttachmentCount = rt;
        render.pColorAttachmentFormats = formats;
        render.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
        render.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        /* One set layout, matching the compiled blobs and the draw's own
         * layout. The previous cut passed the same handle twice as two sets,
         * so the pipeline layout declared set 0 and set 1 as the same layout
         * while every module declares its resources in set 0 alone. It worked
         * by accident on lavapipe (the extra set is simply never written) but
         * it is wrong, and item 15 of the S3c task names it. */
        sets[0] = (VkDescriptorSetLayout)g_layouts.set0_textures;
        memset(&lci, 0, sizeof lci);
        lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lci.setLayoutCount = 1;
        lci.pSetLayouts = sets;
        if (g_vk.CreatePipelineLayout((VkDevice)d->device, &lci, NULL, &pl) !=
            VK_SUCCESS) {
            rsx_vk_log(d, "blob pipeline: vkCreatePipelineLayout failed");
            return 0;
        }
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &render;
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vin;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rast;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dynci;
        ci.layout = pl;
        if (g_vk.CreateGraphicsPipelines((VkDevice)d->device,
                                         (VkPipelineCache)d->pipeline_cache, 1,
                                         &ci, NULL, &vkp) != VK_SUCCESS) {
            g_vk.DestroyPipelineLayout((VkDevice)d->device, pl, NULL);
            rsx_vk_log(d, "blob pipeline: vkCreateGraphicsPipelines failed");
            return 0;
        }
        slot->u.pipeline.pipeline = (uint64_t)(uintptr_t)vkp;
        slot->u.pipeline.layout = (uint64_t)(uintptr_t)pl;
        slot->u.pipeline.rt_count = rt;
        slot->u.pipeline.rt_format = vfmt;
        slot->u.pipeline.depth_format = -1;
        slot->u.pipeline.vs_module = (uint64_t)(uintptr_t)vs;
        slot->u.pipeline.ps_module = (uint64_t)(uintptr_t)ps;
    }
    (void)rs;
    d->pipelines_created++;
    rsx_vk_log(d, "blob pipeline %u from committed SPIR-V (%zu + %zu bytes)", id,
               vs_bytes, ps_bytes);
    return id;
}

uint32_t rsx_vk_spv_blob_draw(const void* vs_spv, size_t vs_bytes,
                              const void* ps_spv, size_t ps_bytes, u32 surface,
                              const void* vertices, u32 vertex_count,
                              u32 stride, const u32* indices, u32 index_count)
{
    /* The device the vtable is driving; a blob draw is the same device, the
     * same pass and the same draw as the HLSL case, which is the point. */
    extern rsx_vk_device* rsx_vk_backend_device(void);
    rsx_vk_device* d = rsx_vk_backend_device();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_vk_slot* slot;
    u32 pipeline;

    if (!d || !d->device)
        return 1;
    slot = rsx_vk_handle_get(d->handles, surface);
    if (!slot || slot->kind != RSX_VK_SLOT_COLOR_TARGET)
        return 3;

    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;                       /* ALWAYS */
    rs.color_mask = 0xFFFFFFFFu;
    /* The vertex input table is derived from the VS's own input locations,
     * not assumed: the compact decompile keeps only the registers the program
     * reads and numbers their locations by declaration order, so
     * the count is whatever the module declares. */
    {
        rsx_vk_spv vs_check;
        int32_t locs[RSX_DSP_NUM_VERTEX_ATTR];
        int n = 0, k;
        (void)locs;
        if (rsx_vk_spirv_from_blob(vs_spv, vs_bytes, &vs_check, NULL, 0) == 0) {
            n = rsx_vk_spv_input_locations(&vs_check, locs,
                                           RSX_DSP_NUM_VERTEX_ATTR);
            rsx_vk_spirv_free(&vs_check);
        }
        memset(&layout, 0, sizeof layout);
        if (n <= 0)
            n = 1;
        if (n > (int)RSX_DSP_NUM_VERTEX_ATTR)
            n = (int)RSX_DSP_NUM_VERTEX_ATTR;
        layout.count = (u8)n;
        layout.stride = (u32)n * 16u;
        for (k = 0; k < n; k++)
            layout.attrs[k] = (u8)k;
    }

    pipeline = rsx_vk_spv_blob_pipeline(d, vs_spv, vs_bytes, ps_spv, ps_bytes,
                                        &rs, &layout, layout.stride,
                                        RSX_BE_FMT_R8G8B8A8, 1);
    if (!pipeline)
        return 2;

    if (!vertices) {
        /* The same full-screen triangle, and the same ATTR3 colour, that the
         * HLSL case draws, so the two readbacks are comparable. */
        static const float tri[3][4] = {
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  3.0f, -1.0f, 0.0f, 1.0f },
            { -1.0f,  3.0f, 0.0f, 1.0f },
        };
        /* The *compact* layout the decompiled VP declares: ATTR0 at slot 0 and
         * ATTR3 at slot 1, stride 32. The compact-input analysis
         * keeps only the two registers this program reads, in ascending order,
         * so glslang numbers their locations by declaration order -- slot 0
         * and slot 1, not 0 and 3. */
        static u8 verts[3 * 32];
        /* The compact VP writes COLOR0 from o1, which the MOV VP fills from
         * ATTR3 -- so the colour must be at slot 1. */
        const float col[4] = { 0x20 / 255.0f, 0xA0 / 255.0f, 0xE0 / 255.0f, 1.0f };
        int v;
        for (v = 0; v < 3; v++) {
            memcpy(verts + (size_t)v * 32u + 0u, tri[v], 16);
            memcpy(verts + (size_t)v * 32u + 16u, col, 16);
        }
        vertices = verts;
        vertex_count = 3;
        stride = 32u;
    }

    /* bind_targets/bind_pipeline record into *this* command buffer, and the
     * dynamic state below belongs to it too; rsx_vk_draw opens the pass after
     * them, which is the order the engine uses (rsx_draw_engine.c:1487-1522). */
    /* The VP's epilogue reads vp_posscale/vp_posoffset out of b0, so the
     * constant block has to exist even for this synthetic draw: 512 identity
     * transform rows and an RSX viewport transform of (+1,+1,+1) / (0,0,0),
     * which is the engine's own default when the guest never programmed a
     * viewport (rsx_draw_engine.c:1444, `xf[8] = {1,1,1,0,0,0,0,0}`). */
    {
        static uint8_t vs_cb[RSX_VK_VP_CB_BYTES];
        static int filled;
        if (!filled) {
            float* f = (float*)vs_cb;
            int r;
            for (r = 0; r < 512; r++) {
                f[r * 4 + 0] = (r == 0 || r == 5 || r == 10 || r == 15) ? 1.0f : 0.0f;
                f[r * 4 + 1] = 0.0f;
                f[r * 4 + 2] = 0.0f;
                f[r * 4 + 3] = 0.0f;
            }
            f[512 * 4 + 0] = 1.0f;   /* vp_posscale = (1,1,1,0) */
            f[512 * 4 + 1] = 1.0f;
            f[512 * 4 + 2] = 1.0f;
            f[512 * 4 + 3] = 0.0f;
            f[513 * 4 + 0] = 0.0f;   /* vp_posoffset = (0,0,0,0) */
            f[513 * 4 + 1] = 0.0f;
            f[513 * 4 + 2] = 0.0f;
            f[513 * 4 + 3] = 0.0f;
            filled = 1;
        }
        rsx_vk_bind_vs_constants(d, vs_cb, sizeof vs_cb);
        rsx_vk_bind_ps_constants(d, vs_cb, 32u);   /* one slot + fp_alpha */
    }

    rsx_vk_bind_targets(d, &surface, 1, 0);
    rsx_vk_bind_pipeline(d, pipeline);
    rsx_vk_set_viewport(d, 0.0f, 0.0f, (float)slot->u.color.width,
                        (float)slot->u.color.height);
    rsx_vk_set_scissor(d, 0, 0, 0, 0);
    rsx_vk_set_stencil_ref(d, 0);
    rsx_vk_draw(d, RSX_TOPOLOGY_TRIANGLES, vertices, vertex_count, stride,
                indices, index_count);
    if (rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE) != 0)
        return 4;
    /* The pipeline was built for this one draw and is retired with it. */
    rsx_vk_pipeline_release(d, pipeline);
    return 0;
}
