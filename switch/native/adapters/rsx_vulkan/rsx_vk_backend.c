/*
 * ps3recomp - Vulkan backend: the rsx_draw_backend instance (slice S1)
 *
 * S1 made seven members real -- init, shutdown, submit_and_wait,
 * color_target_create, color_target_release, clear_color and readback. S2 adds
 * the eight that make a draw reach the GPU: pipeline_create,
 * pipeline_release, bind_targets, bind_pipeline, bind_vs_constants,
 * bind_ps_constants, bind_textures, bind_vertex_textures, set_viewport,
 * set_scissor, set_stencil_ref and draw. S3 adds the target side -- texture_*
 * (rsx_vk_texture.c), surface_view / depth_target_* / depth_snapshot /
 * clear_depth_stencil (rsx_vk_view.c) -- and the sampler cache
 * (rsx_vk_sampler.c).
 *
 * present stays a logged stub until S4/S5: the desktop path is headless and
 * DESIGN §1.6 gives the Switch's swapchain to S5. A stub is a no-op returning
 * 0, which the engine already treats as "this step produced nothing".
 *
 * REPORT.md carries the file:line of every member.
 */
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_vk.h"
#include "rsx_vk_present.h"

/* rsx_vk_target.c */
int rsx_vk_target_clear(rsx_vk_device* d, u32 surface, const float rgba[4]);
int rsx_vk_target_readback(rsx_vk_device* d, u32 surface, u32 x, u32 y, u32 w,
                           u32 h, void* out, u32 out_pitch);

static rsx_vk_device g_device;
static int g_active;

static rsx_vk_device* dev(void* user)
{
    return user ? (rsx_vk_device*)user : &g_device;
}

/* ---- real members ------------------------------------------------------ */

static int vk_init(void* user, u32 width, u32 height)
{
    rsx_vk_device* d = dev(user);
    if (d->initialised) {
        rsx_vk_log(d, "init called twice, ignoring");
        return 0;
    }
    if (rsx_vk_device_init(d, width, height) != 0) {
        rsx_vk_log(d, "init failed");
        return -1;
    }
    g_active = 1;
    return 0;
}

static void vk_shutdown(void* user)
{
    rsx_vk_device* d = dev(user);
    rsx_vk_device_shutdown(d);
    g_active = 0;
}

static void vk_submit_and_wait(void* user, u32 reason)
{
    rsx_vk_device* d = dev(user);
    if (!d->device)
        return;
    /* Fully synchronous, including the fence wait: the engine recycles its
     * staging the moment this returns (rsx_draw_engine.h:112-114). */
    (void)rsx_vk_device_submit_and_wait(d, (int)reason);
    /* The fence passed, so anything retired during this frame can go now. */
    rsx_vk_handle_collect(d);
}

static u32 vk_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                  const void* seed, u32 seed_row_bytes)
{
    return rsx_vk_color_target_create(dev(user), fmt, w, h, seed,
                                      seed_row_bytes);
}

static void vk_color_target_release(void* user, u32 surface)
{
    rsx_vk_color_target_release(dev(user), surface);
}

static void vk_clear_color(void* user, u32 surface, const float rgba[4])
{
    rsx_vk_device* d = dev(user);
    (void)rsx_vk_target_clear(d, surface, rgba);
}

static void vk_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                        void* out, u32 out_pitch)
{
    rsx_vk_device* d = dev(user);
    (void)rsx_vk_target_readback(d, surface, x, y, w, h, out, out_pitch);
}

/* ---- S2: pipelines, per-draw binding, the draw -------------------------- */

static u32 vk_pipeline_create(void* user, const char* vs, const char* ps,
                              const rsx_be_render_state* rs,
                              const rsx_vertex_layout_plan* layout,
                              u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{
    rsx_vk_device* d = dev(user);
    return rsx_vk_pipeline_create(d, vs, ps, rs, layout, stride, rt_fmt,
                                  rt_count);
}

static void vk_pipeline_release(void* user, u32 pipeline)
{
    rsx_vk_pipeline_release(dev(user), pipeline);
}

static void vk_bind_targets(void* user, const u32* surfaces, u32 count,
                            u32 depth)
{
    rsx_vk_bind_targets(dev(user), surfaces, count, depth);
}

static void vk_bind_pipeline(void* user, u32 pipeline)
{
    rsx_vk_bind_pipeline(dev(user), pipeline);
}

static void vk_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    rsx_vk_bind_vs_constants(dev(user), data, bytes);
}

static void vk_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    rsx_vk_bind_ps_constants(dev(user), data, bytes);
}

static void vk_bind_textures(void* user, const u32* textures,
                             const rsx_be_sampler_desc* samplers, u32 mask)
{
    rsx_vk_bind_textures(dev(user), textures, samplers, mask);
}

static void vk_bind_vertex_textures(void* user, const u32* textures,
                                    const rsx_be_sampler_desc* samplers,
                                    u32 mask)
{
    rsx_vk_bind_vertex_textures(dev(user), textures, samplers, mask);
}

static void vk_set_viewport(void* user, float x, float y, float w, float h)
{
    rsx_vk_set_viewport(dev(user), x, y, w, h);
}

static void vk_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    rsx_vk_set_scissor(dev(user), x, y, w, h);
}

static void vk_set_stencil_ref(void* user, u32 ref)
{
    rsx_vk_set_stencil_ref(dev(user), ref);
}

static void vk_draw(void* user, rsx_topology topology, const void* vertices,
                    u32 vertex_count, u32 stride, const u32* indices,
                    u32 index_count)
{
    rsx_vk_draw(dev(user), topology, vertices, vertex_count, stride, indices,
                index_count);
}

/* ---- S3: textures, views, depth (rsx_vk_texture.c, rsx_vk_view.c) ------ */

static u32 vk_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                             u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    return rsx_vk_texture_create(dev(user), fmt, w, h, mips, faces, remap,
                                 rsx_fmt);
}

static void vk_texture_upload(void* user, u32 texture, u32 face, u32 mip,
                              u32 w, u32 h, const void* src, u32 row_bytes,
                              u32 rows)
{
    rsx_vk_texture_upload(dev(user), texture, face, mip, w, h, src, row_bytes,
                          rows);
}

static void vk_texture_release(void* user, u32 texture)
{
    rsx_vk_texture_release(dev(user), texture);
}

static u32 vk_surface_view(void* user, u32 surface, u32 remap, u32 fmt)
{
    return rsx_vk_surface_view(dev(user), surface, remap, fmt);
}

static u32 vk_depth_target_create(void* user, u32 w, u32 h)
{
    return rsx_vk_depth_target_create(dev(user), w, h);
}

static void vk_depth_target_release(void* user, u32 depth)
{
    rsx_vk_depth_target_release(dev(user), depth);
}

static u32 vk_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    return rsx_vk_depth_snapshot(dev(user), depth, w, h);
}

static void vk_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                   float depth_value, u8 stencil)
{
    rsx_vk_clear_depth_stencil(dev(user), depth, flags, depth_value, stencil);
}

/* ---- logged stub: present ---------------------------------------------- */

static void vk_stub_present(void* user, u32 surface)
{
#if defined(__SWITCH__)
    /* Root (2026-11-12, Decision 66): on the console the flip goes to the screen through rsx_vk_present.c, once
     * the composition has called rsx_vk_present_init. Until then (and on a failure) it is a silent no-op. */
    (void)rsx_vk_present_surface(dev(user), surface);
#else
    (void)surface;
    rsx_vk_unimplemented(dev(user), "present");
#endif
}

/* ---- the vtable -------------------------------------------------------- */

static const rsx_draw_backend g_backend = {
    &g_device,                 /* user */
    vk_init,
    vk_shutdown,
    vk_submit_and_wait,
    vk_texture_create,         /* texture_create       */
    vk_texture_upload,         /* texture_upload       */
    vk_texture_release,        /* texture_release      */
    vk_color_target_create,    /* color_target_create  */
    vk_color_target_release,   /* color_target_release */
    vk_surface_view,           /* surface_view         */
    vk_depth_target_create,    /* depth_target_create  */
    vk_depth_target_release,   /* depth_target_release */
    vk_depth_snapshot,         /* depth_snapshot       */
    vk_pipeline_create,        /* pipeline_create    */
    vk_pipeline_release,       /* pipeline_release   */
    vk_bind_targets,           /* bind_targets       */
    vk_bind_pipeline,          /* bind_pipeline      */
    vk_bind_vs_constants,      /* bind_vs_constants  */
    vk_bind_ps_constants,      /* bind_ps_constants  */
    vk_bind_textures,          /* bind_textures        */
    vk_bind_vertex_textures,   /* bind_vertex_textures */
    vk_set_viewport,           /* set_viewport       */
    vk_set_scissor,            /* set_scissor        */
    vk_set_stencil_ref,        /* set_stencil_ref    */
    vk_draw,                   /* draw               */
    vk_clear_color,            /* clear_color        */
    vk_clear_depth_stencil,    /* clear_depth_stencil  */
    vk_stub_present,
    vk_readback                /* readback           */
};

const rsx_draw_backend* rsx_vk_backend(void)
{
    return &g_backend;
}

rsx_vk_device* rsx_vk_backend_device(void)
{
    return &g_device;
}
