/*
 * ps3recomp - Vulkan backend for the RSX draw engine (slices S1-S3)
 *
 * One source set, two targets: the desktop Vulkan loader (development and CI
 * on lavapipe) and NXVK on the Switch (slice S5). Everything Vulkan-shaped is
 * compiled with VK_NO_PROTOTYPES and reached through the function table in
 * rsx_vk_procs, which rsx_vk_platform.c fills in differently per target.
 *
 * Nothing here includes the Vulkan headers: the whole backend talks to the
 * header-declared PFN types instead, so a target without a loader still
 * compiles, and no prototype can accidentally be linked against a driver.
 *
 * slices in its §7;
 * Decision 28 in evidence/root-gcm-next-boundary-prep-r1/DECISIONS.md.
 */
#ifndef PS3RECOMP_RSX_VK_H
#define PS3RECOMP_RSX_VK_H

#include <stddef.h>
#include <stdint.h>

#include "rsx_draw_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sizes the vtable and DESIGN §2.2 / §4.2 fix ------------------------ */

/* VS constants: 512 vec4 transform constants + vp_posscale + vp_posoffset.
 * This is ENG_VP_CB_BYTES in rsx_draw_engine.c:54, repeated here because the
 * engine does not export it (ps3recomp is not edited). */
#define RSX_VK_VP_CB_BYTES  ((512u + 2u) * 16u)   /* 8224 */
/* PS constants: fp_constants[N] + fp_alpha. The engine sizes it per draw and
 * never exceeds the VS block (rsx_draw_engine.c:1461-1477). */
#define RSX_VK_PS_CB_BYTES  RSX_VK_VP_CB_BYTES

/* Textures/samplers the PS can name: t0..t15 and s0..s15. */
#define RSX_VK_MAX_TEX_UNITS  RSX_DSP_NUM_TEXTURES        /* 16 */
#define RSX_VK_MAX_VTEX_UNITS RSX_DSP_NUM_VERTEX_TEXTURES /* 4  */

/* The 12 RSX_BE_FMT_* values of DESIGN §3.1 (RSX_BE_FMT_NONE, 0, is not one of
 * them), and the sampler cache depth of DESIGN §2.2 (64, as Metal's
 * MTL_SAMP_CACHE at rsx_metal_backend.m:277). */
#define RSX_VK_FMT_COUNT     ((uint32_t)RSX_BE_FMT_R32G32B32A32F + 1u)
#define RSX_VK_MAX_SAMPLERS  64

/* The binding a unit's sampler lands at, offset from its `t` number: HLSL's
 * t and s registers are separate spaces and glslang maps both to the same
 * binding unless told otherwise, so the samplers are shifted clear of the
 * images (rsx_vk_shader.cpp's setShiftSamplerBinding call and
 * rsx_vk_device.c's layouts_create). */
#define RSX_VK_SAMPLER_BINDING_SHIFT 32
/* The same for the `t` registers, whose 0/1 would otherwise collide with the
 * two cbuffers (glslang keeps a block variable's bN as its binding even when
 * the samplers are shifted). Images are 0..19 in the layout, so a shift of 4
 * clears b0/b1 with room to spare. */
#define RSX_VK_IMAGE_BINDING_SHIFT 4

/* The sampler shift the *vertex* stage's translated program needs in place of
 * RSX_VK_SAMPLER_BINDING_SHIFT.
 *
 * HLSL numbers `s` registers per stage from zero, and glslang adds the sampler
 * shift to that number. The guest's VP declares `SamplerState rsx_vsamp0..3 :
 * register(s0..s3)` (rsx_vp_decompiler.c:619-620), so at the PS shift of 32
 * they land on bindings 32..35 -- *exactly* PS s0..s3, which the draw's
 * descriptor write also fills. Measured with spirv-dis; REPORT.md records the
 * lines.
 *
 * Shifted to 48 the VS's samplers sit at 48..51, clear of the PS's 32..47.
 *
 * The images need no separate shift: glslang keeps `t` register numbers across
 * the stage (the guest's vsamp0 pairs with `register(t16)`) and adds the image
 * shift to them, so `t16..t19` already lands at 20..23. */
#define RSX_VK_VS_SAMPLER_BINDING_SHIFT 48
/* Where the VS's images land at the plain image shift: t register 16 + 4. Named
 * so the layout, the descriptor write and rsx_vk_spv_verify agree by
 * construction rather than by three copies of the number 20. */
#define RSX_VK_VS_IMAGE_BINDING_SHIFT   20

/* Texture staging slab (DESIGN §4.2: 16 MiB). */
/* Root (2026-11-12, Decision 92): the first real scene on the console (run rv23) created about 500 textures in
 * one frame and overran the per-frame sizes below. Old values: 16 MiB, 2 MiB, 8 MiB, 256 modules, 256 sets, 512 handles. */
#define RSX_VK_TEX_SLAB_BYTES (128u * 1024u * 1024u)

/* Frames the ring is staged for by default. On the Switch this is raised to
 * max(3, caps.minImageCount) by rsx_vk_platform_open (DESIGN §0 fact 1). */
#define RSX_VK_MAX_IN_FLIGHT 4

/* Staging rings, sized as DESIGN §4.2. The UBO slab holds both constant
 * blocks; the geometry slab holds a draw's vertices and indices; the readback
 * slab holds one readback rectangle. */
#define RSX_VK_UBO_SLAB_BYTES      (32u * 1024u * 1024u)
#define RSX_VK_GEOM_SLAB_BYTES     (64u * 1024u * 1024u)
#define RSX_VK_READBACK_SLAB_BYTES (1024u * 1024u)

/* Shader modules the backend keeps before it starts reusing the oldest (the
 * cache is keyed by HLSL hash, so 256 programs is more than S2 can fill). */
#define RSX_VK_MAX_MODULES 2048
/* Descriptor sets per in-flight frame that a draw may allocate before the
 * pool is reset. */
#define RSX_VK_SETS_PER_FRAME 4096

/* Slots in the backend's handle table, which is also the bound every loop that
 * walks the live slots must use (rsx_vk_handle_capacity, item 11).
 *
 * Deliberately not 256: at 256 the table's size and the `id < 256` literal the
 * S3b loops spelled were the same number, so those loops were correct by
 * coincidence and the defect could not be observed. 512 is a size a busy frame
 * plausibly needs (the engine caches up to 1024 textures on its own) and puts
 * the capacity clear of any constant the old code carried. */
#define RSX_VK_HANDLE_CAPACITY 4096u

/* How many depth_snapshots one frame may record, which is exactly the number
 * of descriptor sets the snapshot pool holds between two fences.
 *
 * The pool used to be a literal 32 in rsx_vk_view.c with nothing checking the
 * count before it started recording, so a title resolving more zetas than that
 * between two fences got an AllocateDescriptorSets failure after the source
 * image had already been transitioned and the destination created (item N5).
 * The cap is now named, checked as a precondition of the call, and the pool is
 * sized from it; rsx_vk_device.c clears snapshots_this_frame at the same fence
 * that resets the pool. */
#define RSX_VK_SNAPSHOTS_PER_FRAME 32u

/* Spare device/instance extensions, one per line in the layers env. */
#define RSX_VK_MAX_EXT 8

/* Handles the backend returns are small integers, 0 means none
 * (rsx_draw_engine.h:109-208). */
typedef struct rsx_vk_handle rsx_vk_handle;

/* the dispatch table */

/*
 * Filled by rsx_vk_platform_procs(), which is the only place the two targets
 * differ. `instance` may be VK_NULL_HANDLE: the instance-level entry points
 * are then resolved on the loader's own handle.
 */
typedef struct rsx_vk_procs {
    void* loader;   /* dlopen handle on the desktop; NULL on the Switch */
    /* The live instance, for the one device-level lookup that goes through
     * the instance (vkGetDeviceProcAddr). Set by rsx_vk_device_init. */
    uint64_t instance_for_device_procs;
} rsx_vk_procs;

/* ---- objects owned by the device --------------------------------------- */

/* One colour target, colour-target view, or depth target: an image, its
 * memory and its default view. */
typedef struct rsx_vk_image {
    uint64_t image;      /* VkImage      */
    uint64_t memory;     /* VkDeviceMemory */
    uint64_t view;       /* VkImageView  */
    uint32_t format;     /* VkFormat     */
    uint32_t width, height;
    rsx_be_format be_format;
    int32_t layout;      /* current VkImageLayout, -1 = UNDEFINED */
    /* The colour target a surface view was built from, or 0 for an owner.
     * No flag-conditional member hides behind this: the field is always
     * present and simply describes the slot. */
    uint32_t view_of;

    /* ---- slice S3 ---------------------------------------------------- */
    uint32_t mips;       /* >= 1                                          */
    uint32_t faces;      /* 1, or 6 for a cube map                        */
    /* The crossbar this image's default view wears, in VkComponentSwizzle
     * order (R,G,B,A), as rsx_texture_component_remap produced it. Kept so
     * surface_view can tell "same swizzle" from "needs another view". */
    uint32_t swizzle[4];
    /* 1 leaves the view's components IDENTITY; 0 means swizzle[] is set. */
    uint32_t identity;
    /* Every aspect the image has, from its VkFormat (rsx_vk_aspect_mask). A
     * depth/stencil image has two, and a barrier that moves "the image" to
     * another layout has to name both or the tracker and the driver disagree
     * about the one it left behind (item N1). Kept on the image rather than
     * recomputed at each barrier so the transition, the descriptor's
     * imageLayout and depth_snapshot's own view can be checked against one
     * number. */
    uint32_t aspect_mask;
} rsx_vk_image;

/* A depth target: the image, plus what a snapshot published from it needs. */
typedef struct rsx_vk_depth {
    rsx_vk_image image;         /* D32_SFLOAT_S8_UINT (Decision 28)        */
    /* A DEPTH-only view of the same image, for depth_snapshot.
     *
     * The image's own view covers DEPTH|STENCIL, which is what the depth test
     * attaches; a sampler over it would present stencil in .g and make the
     * shader's DEPTH-only read a mismatch between the view and the aspect the
     * barrier transitions. The snapshot samples through this view instead. */
    uint64_t depth_view;
    /* The last snapshot published for this target, so a title that samples
     * one zeta twice does not re-resolve it every frame. */
    uint32_t snapshot;
    /* The pending clear this pass has to carry: clears that
     * arrived while no pass was open become the load op of the next one.
     *
     * Per aspect, because a clear of one aspect must not touch the other: a
     * stencil-only clear used to fold into a load-op CLEAR of the whole
     * attachment, which also reset depth to clear_depth and destroyed a depth
     * still being tested against. `clear_pending` stays as "any aspect is
     * pending" so the pass code reads the same as before. */
    int   clear_pending;
    float clear_depth;
    uint32_t clear_stencil;
    uint32_t clear_flags;
    int   depth_clear_pending;
    int   stencil_clear_pending;
    /* True once a snapshot has observed a depth write since the last clear;
     * a clear-only zeta is not a usable depth map (rsx_draw_engine.c:665-674
     * draws the same distinction). */
    uint32_t had_write;
} rsx_vk_depth;

/* A texture: a 2D array image of `mips` levels and `faces`
 * layers, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT when faces == 6, and the
 * crossbar baked into its view's VkComponentMapping. */
typedef struct rsx_vk_texture {
    rsx_vk_image image;
    u32  remap;          /* the raw TEXTURE_CONTROL1 crossbar             */
    u32  rsx_fmt;        /* the SET_TEXTURE_FORMAT byte it decodes against */
    /* Mip/face uploads recorded since the last fence: a texture is
     * TRANSFER_DST_OPTIMAL until the batch ends, and a second upload that
     * arrives after the fence has to open its own batch. */
    u32  uploads_recorded;
} rsx_vk_texture;

/* One graphics pipeline and the two modules it was built from. The modules
 * are owned by the shader cache (keyed by HLSL hash), not by the pipeline, so
 * a pipeline release never destroys a module another pipeline still names
 */
typedef struct rsx_vk_pipeline {
    uint64_t pipeline;
    uint64_t layout;            /* VkPipelineLayout, owned by this slot */
    uint32_t rt_count;
    uint32_t rt_format;
    /* The depth format this pipeline was created against, or 0 for none.
     * A pass supplies exactly the format its pipeline declares or
     * vkCmdBeginRendering is invalid, so rsx_vk_draw compares, never assumes
     */
    uint32_t depth_format;
    uint64_t vs_module;
    uint64_t ps_module;
    /* Everything the pipeline was built from, so the depth-on twin can be
     * built from the same inputs without the engine's help. */
    rsx_be_render_state rs;
    rsx_vertex_layout_plan vlayout;
    /* The depth-on twin, created on first need and reused: the engine's
     * pipeline handle is created before any zeta is bound in that frame, so a
     * pipeline built without a depth attachment must be rebuildable against
     * one. */
    uint32_t depth_variant;
    /* For the twin's own slot: the handle of the pipeline it was built for, so
     * a draw that arrives naming the twin can still find its base. */
    uint32_t variant_of;
    /* The vertex-attribute table pipeline_build actually handed to
     * vkCreateGraphicsPipelines, flattened as {location, binding, offset,
     * VkFormat} per entry. Kept so the reflection assertion compares the
     * compiled SPIR-V against this pipeline's own table rather than against
     * the loop index that produced it. */
    uint32_t vattr[RSX_DSP_NUM_VERTEX_ATTR][4];
    uint32_t vattr_count;
} rsx_vk_pipeline;

/* What a handle slot can name. */
typedef enum {
    RSX_VK_SLOT_NONE = 0,
    RSX_VK_SLOT_COLOR_TARGET,
    RSX_VK_SLOT_PIPELINE,
    RSX_VK_SLOT_TEXTURE,        /* an uploaded texture       */
    RSX_VK_SLOT_SURFACE_VIEW,   /* a swizzled view of a colour target     */
    RSX_VK_SLOT_DEPTH           /* a zeta             */
} rsx_vk_slot_kind;

typedef struct rsx_vk_slot {
    rsx_vk_slot_kind kind;
    union {
        rsx_vk_image color;
        rsx_vk_depth depth;
        rsx_vk_pipeline pipeline;
        rsx_vk_texture texture;
    } u;
    /* A surface view shares its parent's VkImage and VkDeviceMemory; the
     * slot owns only the VkImageView, and `u.color.view_of` names the colour
     * target it was built from. */
} rsx_vk_slot;

/* A lifecycle callback so the optional validation layer can count messages
 * without the device owning a reporter thread. */
typedef void (*rsx_vk_report_fn)(void* user, int severity, const char* message);

/* ---- the device -------------------------------------------------------- */

typedef struct rsx_vk_device {
    rsx_vk_procs procs;

    /* instance */
    uint64_t instance;          /* VkInstance         */
    uint64_t debug_messenger;   /* VkDebugUtilsMessengerEXT, 0 if not enabled */
    int validation_enabled;
    uint32_t api_version;       /* what vkCreateInstance was given */

    /* physical device + capability facts the backend branches on */
    uint64_t physical;          /* VkPhysicalDevice   */
    uint64_t device;            /* VkDevice           */
    uint64_t queue;             /* VkQueue            */
    uint32_t queue_family;
    char device_name[256];
    uint32_t max_color_w, max_color_h;
    uint32_t max_memory_allocs;
    uint32_t memory_type_count;

    /* frame scoping: one command pool and one fence per in-flight frame */
    uint32_t in_flight;
    uint64_t pool[RSX_VK_MAX_IN_FLIGHT];
    uint64_t fence[RSX_VK_MAX_IN_FLIGHT];
    uint64_t cmd[RSX_VK_MAX_IN_FLIGHT];
    uint32_t frame;             /* the frame currently recording */
    int recording;              /* cmd[frame] is in the recording state */

    /* staging ring: one host-visible, persistently mapped buffer */
    uint64_t ubo_buf, ubo_mem;
    void*    ubo_map;
    uint32_t ubo_offset;       /* the bump pointer                       */
    uint32_t ubo_last_offset;  /* where the most recent reservation began */
    uint32_t ubo_align;

    /* geometry slab: one draw's vertices and indices */
    uint64_t geom_buf, geom_mem;
    void*    geom_map;
    uint32_t geom_offset;

    uint64_t rb_buf, rb_mem;
    void*    rb_map;

    /* ---- slice S2: pipelines, shader modules, the draw ------------------ */

    /* The device extensions S2 asks for at init when the physical device
     * offers them, logged either way. */
    int has_dynamic_rendering;
    int has_push_descriptor;
    int has_synchronization2;
    uint32_t api_version_actual;

    uint64_t pipeline_cache;    /* VkPipelineCache */
    uint64_t descriptor_pool;   /* vkAllocateDescriptorSets source */

    /* Shader module cache, keyed by the hash of the HLSL text. Modules are
     * shared by every pipeline built from the same program. */
    struct rsx_vk_module_entry {
        uint64_t hash;
        uint64_t module;
        uint32_t stage;
        uint64_t last_use;
        int      colour_targets;
    } modules[RSX_VK_MAX_MODULES];
    uint32_t module_count;
    uint64_t module_clock;

    /* ---- the pending draw (DESIGN §1.4: accumulate, flush at draw) ------ */
    u32 pending_target;
    /* The pipeline handle the engine bound, so the draw can ask for its
     * depth-on twin when a zeta arrives after the pipeline was created.
     *
     * This is always the engine's own handle: never the twin's. A draw with no
     * zeta bound has to run the engine's pipeline, and one with a zeta bound
     * has to run the depth-on twin, so the choice is made per draw from
     * `pending_depth` and the twin is only ever named locally. */
    u32 pending_pipeline_id;
    uint64_t pending_pipeline;    /* VkPipeline of the pipeline last bound */
    uint64_t pending_layout;      /* VkPipelineLayout of the pipeline last bound */
    uint32_t pending_pipeline_rt_count;
    uint32_t pending_pipeline_rt_format;
    uint32_t pending_ubo_vs;
    uint32_t pending_ubo_ps;
    uint32_t pending_ubo_vs_bytes;
    uint32_t pending_ubo_ps_bytes;

    /* The dynamic state the engine latched for this draw. Recorded by
     * rsx_vk_draw inside the pass rather than when the setter was called,
     * because a command buffer's dynamic state is per-recording and the
     * engine's setter order is not the command order (DESIGN §1.4 lists it,
     * but bind_pipeline starts the recording before the pass opens). */
    float pending_viewport[4];
    int   pending_viewport_set;
    uint32_t pending_scissor[4];
    int   pending_scissor_set;
    uint32_t pending_stencil_ref;
    int   pending_stencil_ref_set;

    /* frame scoping for the pending pass */
    int rendering;
    uint64_t rendering_target;
    uint64_t rendering_depth;

    /* counters a test can read */
    uint32_t pipelines_created;
    uint32_t pipelines_failed;
    uint32_t draws_recorded;
    uint32_t modules_created;
    uint32_t passes_begun;
    uint32_t spv_verified;
    uint32_t spv_rejected;
    uint32_t spv_cache_hits;      /* modules whose SPIR-V came from the on-disk cache */
    uint32_t spv_cache_stores;    /* translations written to it */

    /* handle table with a retired list */
    rsx_vk_handle* handles;

    /* ---- slice S3: formats, textures, targets, depth ------------------ */

    /* The format table of DESIGN §3.1 as this device reports it: indexed by
     * rsx_be_format, the VkFormat chosen (0 = unsupported) and the feature
     * bits that restricted the choice. Filled and logged at init. */
    uint32_t format_table[RSX_VK_FMT_COUNT];
    uint32_t format_features[RSX_VK_FMT_COUNT];
    /* DESIGN R10 / Decision 28: one depth format for every zeta. */
    uint32_t depth_format;
    uint32_t depth_stencil_format;  /* == depth_format when it has stencil */

    /* The texture staging slab. */
    uint64_t tex_buf, tex_mem;
    void*    tex_map;
    uint32_t tex_offset;
    uint32_t copy_align;      /* optimalBufferCopyOffsetAlignment      */

    /* Sampler cache, keyed like Metal's eng_sampler_slot. */
    /* Null texture: a 1x1 opaque white image for the units a
     * draw does not name, because a descriptor has no "no image" value. */
    uint64_t null_image;
    uint64_t null_view;
    uint64_t null_mem;
    struct rsx_vk_sampler_entry {
        uint64_t hash;
        uint64_t sampler;
        uint64_t last_use;
        int      used;
        /* The desc this sampler was built from, with the unfilterable clamp
         * already applied. Kept so a test can assert that DESIGN §3.1
         * fallback 2 really chose NEAREST for an unfilterable format rather
         * than trusting that "a sampler exists" (item L2). */
        rsx_be_sampler_desc desc;
    } samplers[RSX_VK_MAX_SAMPLERS];
    uint32_t sampler_count;
    uint64_t sampler_clock;
    /* One shared sampler for the descriptors a pass binds and a draw does not
     * name, so every image descriptor always has a sampler to pair with.
     *
     * It is deliberately NOT one of the 64 cache entries: it lives for the
     * device's lifetime, and a cache that could evict it would leave this
     * field naming a destroyed VkSampler after 64 distinct samplers. Created
     * by rsx_vk_null_sampler outside the cache; destroyed at shutdown. */
    uint64_t null_sampler;
    /* Samplers evicted from the cache and not yet safe to destroy: they are
     * retired through the same fence-passed boundary as images and pipelines
     * (rsx_vk_samplers_collect), because a recorded or in-flight command
     * buffer may still name one. */
    struct rsx_vk_sampler_retired {
        uint64_t sampler;
        uint32_t frame_retired;
    } samplers_retired[RSX_VK_MAX_SAMPLERS * 4];
    uint32_t samplers_retired_count;
    /* Evictions the retirement list could not take (it is only dropped when
     * 4x the cache is in flight, which cannot happen); counted so a leak is
     * visible rather than silent. */
    uint32_t samplers_retired_dropped;
    /* How many sampler requests were answered with the NEAREST fallback because
     * the format could not filter linearly (DESIGN §3.1 fallback 2). A counter
     * rather than a claim: a guest that already asked for NEAREST and an
     * unfilterable format that had to be forced look the same in the desc, so
     * the fallback is counted where it is decided. */
    uint32_t samplers_nearest_forced;

    /* What bind_textures / bind_vertex_textures latched for this draw. */
    u32 pending_tex[RSX_VK_MAX_TEX_UNITS];
    u32 pending_tex_mask;
    uint64_t pending_sampler[RSX_VK_MAX_TEX_UNITS];
    u32 pending_vtex[RSX_VK_MAX_VTEX_UNITS];
    u32 pending_vtex_mask;
    uint64_t pending_vsampler[RSX_VK_MAX_VTEX_UNITS];

    /* The depth attachment bind_targets named (0 = none). */
    u32 pending_depth;

    /* DESIGN R6: the pass's colour attachments must equal the pipeline's. A
     * pass is begun by rsx_vk_draw after bind_targets, so this is what the
     * draw compares. */
    u32 pending_rt_count;
    /* Root (2026-11-13, Decision 104): every colour target of the bound set, target A first; entries from
     * pending_rt_count on are 0. mrt_passes counts passes begun with two or more attachments. */
    u32 pending_targets[RSX_BE_MAX_COLOR_TARGETS];
    u32 mrt_logged;
    uint64_t mrt_passes;

    /* DESIGN §3.1 fallback 3: a format with no COLOR_ATTACHMENT_BLEND has its
     * blending forced off, with one log line per format. */
    uint32_t blend_forced_off_mask;

    /* Counters a test can read (S3). */
    uint32_t textures_created;
    uint32_t texture_uploads;
    uint32_t depth_targets_created;
    uint32_t depth_snapshots;
    /* Snapshots this frame has taken, and how many the cap refused. Cleared at
     * every fence with the snapshot pool's reset (item N5). */
    uint32_t snapshots_this_frame;
    uint32_t snapshots_refused;
    uint32_t surface_views_created;
    uint32_t samplers_created;
    uint32_t format_fallbacks;
    uint32_t upload_batches;
    uint32_t depth_clears;
    /* Clears folded into a pass load op, i.e. actually applied to the image.
     * A test compares this against depth_clears to see that a per-aspect
     * clear was carried rather than dropped. */
    uint32_t depth_clears_applied;

    /* The depth_snapshot program: a fixed full-screen triangle
     * that samples the zeta and writes R32F. Built on first use. */
    uint64_t snap_layout;
    uint64_t snap_set_layout;
    uint64_t snap_pipeline;
    /* The snapshot descriptor pool. Kept at field offset parity with the rest
     * of the struct; see snap_pool_created below for the one that is created
     * once and reused. */
    uint64_t snap_pool;
    int      snap_built;
    /* The snapshot descriptor pool is created once and reused with a reset at
     * each fence-passed boundary, so N snapshots are one pool, not N. */
    uint64_t snap_pool_created;
    int      snap_pool_in_use;

    /* Descriptor pools this device has created and not yet destroyed, so the
     * "the pool count stays at 1" depth_snapshot test is a number. */
    uint32_t descriptor_pools_created;
    uint32_t descriptor_pools_alive;

    /* Item N3: sampled-read layout transitions must be recorded before
     * pass_begin, and a draw may not sample an image it is also attaching.
     *
     * feedback_loops counts the draws that named one of their own attachments
     * as a sampled image; feedback_logged is how many of those have had their
     * log line, so the line appears once per draw and not once per unit.
     * sampled_read_transitions counts the barriers this path recorded, which is
     * what lets a test assert the transitions still happen at all after they
     * moved out of write_descriptor_set. */
    uint32_t sampled_read_transitions;
    uint32_t feedback_loops;
    uint32_t feedback_logged;
    /* Item M3: how many command buffers have been recorded and submitted, and
     * how many submit_and_wait calls were the no-op path (nothing recorded).
     * A test that takes 32 snapshots and submits can assert from these that the
     * barriers really were submitted and that the snapshot counter came back
     * to zero at that same boundary. */
    uint32_t submits;
    uint32_t submit_noops;

    /* Item M3: the per-frame resets (UBO/geom/tex ring offsets, the snapshot
     * counter and the snapshot pool when it is in use) are one function, run at
     * every frame boundary -- the fence when a batch was submitted, and the
     * no-op path when none was -- so a frame that records nothing cannot carry
     * the previous frame's snapshot allowance or staging offsets into the next
     * one. frame_resets counts the resets that actually ran. */
    uint32_t frame_resets;

    /* logging: each RSX_VK_UNIMPLEMENTED member is reported once */
    uint64_t unimplemented_mask;

    /* allocation accounting: vkAllocateMemory minus vkFreeMemory, so a leak
     * is a number and not an opinion (DESIGN §7 S1 acceptance). */
    uint64_t memory_allocs;
    uint64_t memory_frees;

    /* optional validation-layer sink */
    rsx_vk_report_fn report;
    void* report_user;
    uint32_t validation_messages;

    u32 width, height;          /* init()'s arguments: logged, nothing more */
    int initialised;
} rsx_vk_device;

/* ---- platform (rsx_vk_platform.c: the only __SWITCH__ file) ------------- */

#define RSX_VK_PLATFORM_OK 0
#define RSX_VK_PLATFORM_ERR_LOADER 1
#define RSX_VK_PLATFORM_ERR_PROC 2

/* True when the Vulkan entry point set for this target is reachable at all:
 * the deskop needs libvulkan.so.1, the Switch needs nothing (NXVK is linked). */
int rsx_vk_platform_available(void);
const char* rsx_vk_platform_driver(void);

/* Instance extensions this target asks for on a headless device. Writes up to
 * `cap` names into `out` and returns how many; 0 is fine (headless). */
uint32_t rsx_vk_platform_instance_extensions(const char** out, uint32_t cap);

/* Look up an instance-level entry point. `instance` may be 0. */
uint64_t rsx_vk_platform_instance_proc(rsx_vk_procs* p, uint64_t instance,
                                       const char* name);
/* Look up a device-level entry point. */
uint64_t rsx_vk_platform_device_proc(rsx_vk_procs* p, uint64_t device,
                                     const char* name);

/* In-flight frames for this target, before a swapchain exists. */
uint32_t rsx_vk_platform_default_in_flight(void);

/* Open the device for this target: the loader, or the linked ICD. Split from
 * rsx_vk_device_init so the __SWITCH__ ordering rules (setenv before
 * vkCreateInstance, DESIGN §5) live in one place. */
int rsx_vk_platform_open(rsx_vk_device* d);
void rsx_vk_platform_close(rsx_vk_device* d);

/* ---- device (rsx_vk_device.c) ------------------------------------------ */

void rsx_vk_device_set_report(rsx_vk_device* d, rsx_vk_report_fn fn, void* user);

int  rsx_vk_device_init(rsx_vk_device* d, u32 width, u32 height);
void rsx_vk_device_shutdown(rsx_vk_device* d);

/* End the recording command buffer if there is one and submit it, waiting on
 * the frame fence. Returns 0 when the device is idle and the staging ring is
 * reusable, which is the contract at rsx_draw_engine.h:112-114.
 *
 * sub-16-bit `reason` values come from rsx_draw_engine.h:88-91; the two extra
 * ones below are the backend's own, for the internal flush the ring uses. */
#define RSX_VK_FLUSH_RING_FULL (-1)
#define RSX_VK_FLUSH_IDLE      (-2)
int  rsx_vk_device_submit_and_wait(rsx_vk_device* d, int reason);

/* Reserve `bytes` at `alignment` in the UBO slab and return a host pointer, or
 * NULL when the slab cannot take it (the caller flushes and retries). */
void* rsx_vk_ubo_reserve(rsx_vk_device* d, uint32_t bytes, uint32_t alignment);
/* The device offset of the most recent reservation, for a dynamic UBO offset. */
uint32_t rsx_vk_ubo_last_offset(const rsx_vk_device* d);

/* Copy `bytes` into the readback slab and return its host pointer, or NULL. */
void* rsx_vk_readback_slab(rsx_vk_device* d, uint32_t bytes);

/* Recording accessors used by the slices that record commands. */
int  rsx_vk_device_begin(rsx_vk_device* d);
uint64_t rsx_vk_device_cmd(rsx_vk_device* d);

/* Small helpers with the VkResult check built in; they log and return 0/-1. */
int rsx_vk_check(rsx_vk_device* d, int vk_result, const char* what);

/* Log one line. Always prefixed so a run's log can be filtered. */
void rsx_vk_log(rsx_vk_device* d, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Report an unimplemented vtable member once per device. */
void rsx_vk_unimplemented(rsx_vk_device* d, const char* member);

/* ---- colour targets + readback (rsx_vk_target.c) ----------------------- */

/* RSX_BE_FMT -> VkFormat. S3 fills the whole 12-entry table of DESIGN §3.1
 * at init (rsx_vk_formats_init) and this returns the entry it chose; 0 for
 * RSX_BE_FMT_NONE and for any format the device reported as unusable. */
uint32_t rsx_vk_format(rsx_vk_device* d, rsx_be_format fmt);

/* Ask the physical device about every format of DESIGN §3.1, choose one per
 * entry with the four fallbacks of §3.1, and log each choice. Called by
 * rsx_vk_device_init after the device exists and before anything creates an
 * image; returns 0 when the minimum set (R8G8B8A8 as a colour target and a
 * depth format) is present. */
int rsx_vk_formats_init(rsx_vk_device* d);

/* 0 on failure. */
int rsx_vk_image_create(rsx_vk_device* d, rsx_be_format be, uint32_t fmt,
                        uint32_t w, uint32_t h, uint32_t usage,
                        rsx_vk_image* out);

/* The general form: `mips` levels, `faces` layers (6 marks the image
 * cube-compatible and makes the view a CUBE), and the component mapping the
 * view wears (NULL or all-identity for a plain target). */
int rsx_vk_image_create_ex(rsx_vk_device* d, rsx_be_format be, uint32_t fmt,
                           uint32_t w, uint32_t h, uint32_t usage,
                           uint32_t mips, uint32_t faces,
                           const void* component_mapping,
                           rsx_vk_image* out);
void rsx_vk_image_destroy(rsx_vk_device* d, rsx_vk_image* img);

/* The per-image velocity tracker of DESIGN §4.3, reduced to what S1 needs:
 * the layout an image is currently in. -1 means "unknown, use UNDEFINED". */
int32_t rsx_vk_image_layout(const rsx_vk_image* img);
void rsx_vk_image_set_layout(rsx_vk_image* img, int32_t layout);

/* Every aspect an image's VkFormat has (VK_IMAGE_ASPECT_DEPTH_BIT alone for a
 * depth-only format, DEPTH|STENCIL for a D*S8 one, COLOR for everything else).
 * A barrier that moves "the image" to another layout names this, so a
 * depth/stencil image never leaves one aspect behind (item N1). */
uint32_t rsx_vk_aspect_mask(uint32_t vk_format);

/* The same question asked of a handle, resolved through the slot: a surface
 * view answers with its parent target's aspects (it has no image of its own).
 * A colour target, texture or depth target answers with its own. 0 for a
 * handle that is not live. */
uint32_t rsx_vk_handle_aspect_mask(rsx_vk_device* d, u32 handle);

/* A point read of an image's tracked layout, for a test that has to state what
 * the tracker holds rather than infer it from a pixel. Returns -1 when the
 * handle is not a live image-bearing slot. `out_zeta` may be NULL. */
int32_t rsx_vk_handle_layout(rsx_vk_device* d, u32 handle, int* out_is_depth);

/* ---- HLSL -> SPIR-V (rsx_vk_shader.cpp, slice S2) ----------------------- */

/* RSX_SHADER_STAGE_* values, repeated from rsx_shader_msl.h:36 because that
 * header is a C++ one and this backend never includes it. */
#define RSX_VK_STAGE_VERTEX   0
#define RSX_VK_STAGE_FRAGMENT 1

/* One stage's SPIR-V: a malloc'd array of 32-bit words and its length. */
typedef struct rsx_vk_spv {
    uint32_t* words;
    uint32_t  word_count;
} rsx_vk_spv;

/* HLSL -> SPIR-V with the options of rsx_shader_msl.cpp:61-101 (Vulkan 1.1 /
 * SPIR-V 1.3, entry `main`, auto-map bindings and locations, mapIO, the HLSL
 * legalization passes on). Fills `out`; returns 0 on success, -1 with the
 * reason in `log` otherwise. Never touches a VkDevice, so a test can verify
 * the module's declarations without one. */
int rsx_vk_hlsl_to_spirv(const char* hlsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size);

/* The same for GLSL, which the backend's own fixed programs are written in
 * (depth_snapshot's full-screen triangle). Same Vulkan 1.1 / SPIR-V 1.3 floor
 * and the same entry point, without the HLSL legalization passes. */
int rsx_vk_glsl_to_spirv(const char* glsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size);

/* Parse a committed .spv blob (the offline-cache path, DESIGN §2.5). */
int rsx_vk_spirv_from_blob(const void* bytes, size_t byte_count,
                           rsx_vk_spv* out, char* log, size_t log_size);
void rsx_vk_spirv_free(rsx_vk_spv* spv);

/* The decoration walk of DESIGN §2.2: every OpDecorate Binding/DescriptorSet
 * is read back and checked against the layout the backend builds. Returns the
 * number of bindings checked, or -1 with the reason in `log` when a module
 * declares something the layout does not have. `stage` picks which set the
 * image bindings must land in (t0..t15 in the PS, t16..t19 in the VS). */
int rsx_vk_spv_verify(const rsx_vk_spv* spv, int stage, char* log,
                      size_t log_size);

/* What the verifier found, for the test's VS-out/PS-in comparison: the
 * location decorations of a stage's inputs (PS) or outputs (VS), in ascending
 * order. Returns the count written. */
int rsx_vk_spv_io_locations(const rsx_vk_spv* spv, int stage,
                            int32_t* out, int cap);
/* The stage's own input locations: a VS's vertex attributes, or a PS's
 * varyings. Same list as rsx_vk_spv_io_locations with the other storage class;
 * the caller wants the VS's inputs to build its vertex input table from. */
int rsx_vk_spv_input_locations(const rsx_vk_spv* spv, int32_t* out, int cap);

/* True when the storage class OpVariable named `main`'s entry point exists. */
int rsx_vk_spv_has_main(const rsx_vk_spv* spv);
/* Colour targets the PS writes (count of SV_Target locations), 0 for a VS. */
int rsx_vk_spv_colour_targets(const rsx_vk_spv* spv);

/* ---- shader modules and pipelines (rsx_vk_pipeline.c) ------------------ */

/* The VkShaderModule for this HLSL, compiled once per distinct text. `out`
 * receives the module and (when non-NULL) the PS's colour-target count. A
 * compiled module is checked by rsx_vk_spv_verify before it is cached; a
 * mismatched module is a logged failure, never a silent mis-bind. */
/* The on-disk SPIR-V cache (root, 2026-11-12): <dir>/<vs|ps>-<hash of the HLSL text and stage>.spv. NULL or ""
 * turns it off; `write` stores each fresh translation. Host tools may set RSX_VK_SPV_CACHE=<dir> instead. */
void rsx_vk_set_spv_cache_dir(const char* dir, int write);
uint64_t rsx_vk_module_get(rsx_vk_device* d, const char* hlsl, int stage,
                           int* out_colour_targets);

/* Release every module the cache owns (called from rsx_vk_pipeline_destroy_all
 * at shutdown, after every pipeline that named one is gone). */
void rsx_vk_modules_destroy_all(rsx_vk_device* d);

/* vtable members of DESIGN §1.3 and §1.4. */
u32  rsx_vk_pipeline_create(rsx_vk_device* d, const char* vs_hlsl,
                            const char* ps_hlsl, const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* layout,
                            u32 vertex_stride, rsx_be_format rt_fmt,
                            u32 rt_count);
void rsx_vk_pipeline_release(rsx_vk_device* d, u32 pipeline);
void rsx_vk_pipeline_destroy(rsx_vk_device* d, rsx_vk_pipeline* p);

void rsx_vk_bind_targets(rsx_vk_device* d, const u32* surfaces, u32 count,
                         u32 depth);
void rsx_vk_bind_pipeline(rsx_vk_device* d, u32 pipeline);
void rsx_vk_bind_vs_constants(rsx_vk_device* d, const void* data, u32 bytes);
void rsx_vk_bind_ps_constants(rsx_vk_device* d, const void* data, u32 bytes);
void rsx_vk_set_viewport(rsx_vk_device* d, float x, float y, float w, float h);
void rsx_vk_set_scissor(rsx_vk_device* d, u32 x, u32 y, u32 w, u32 h);
void rsx_vk_set_stencil_ref(rsx_vk_device* d, u32 ref);
void rsx_vk_draw(rsx_vk_device* d, rsx_topology topology, const void* vertices,
                 u32 vertex_count, u32 stride, const u32* indices,
                 u32 index_count);

/* The vertex-attribute table a pipeline was actually built with, read back
 * from the slot the pipeline_build populated rather than recomputed: a test
 * that compares the compiled SPIR-V's Locations against the pipeline's own
 * table is making a claim about this table, not about a loop index.
 *
 * Fills up to `cap` entries and returns how many the pipeline has, or -1 when
 * the handle is not a live pipeline. Each entry is {location, binding, offset,
 * VkFormat}. */
u32 rsx_vk_pipeline_vertex_attrs(rsx_vk_device* d, u32 pipeline,
                                 uint32_t* out, u32 cap);

/* Open (or keep) the single dynamic-rendering pass around `surface`; the S2
 * scope is one draw per pass, so a change of target ends the previous one. */
/* Colour targets + depth + stencil a draw can have attached at once. */
#define RSX_VK_MAX_ATTACHED (RSX_BE_MAX_COLOR_TARGETS + 2)
int rsx_vk_pass_begin(rsx_vk_device* d, u32 surface);
void rsx_vk_pass_end(rsx_vk_device* d);

/* ---- textures, samplers, targets and depth (rsx_vk_texture.c, ---------- *
 * rsx_vk_sampler.c, rsx_vk_target.c): DESIGN §1.2 and §3. These are the
 * vtable members of S3, listed here so the backend file is a table of
 * assignments and nothing else.
 */

u32  rsx_vk_texture_create(rsx_vk_device* d, rsx_be_format fmt, u32 w, u32 h,
                           u32 mips, u32 faces, u32 remap, u32 rsx_fmt);
void rsx_vk_texture_upload(rsx_vk_device* d, u32 texture, u32 face, u32 mip,
                           u32 w, u32 h, const void* src, u32 row_bytes,
                           u32 rows);
void rsx_vk_texture_release(rsx_vk_device* d, u32 texture);

u32  rsx_vk_color_target_create(rsx_vk_device* d, rsx_be_format fmt, u32 w,
                                u32 h, const void* seed, u32 seed_row_bytes);
void rsx_vk_color_target_release(rsx_vk_device* d, u32 surface);
/* A texture handle for a colour target a texture unit names, wearing that
 * unit's TEXTURE_CONTROL1 crossbar; cached on (surface, remap, fmt). */
u32  rsx_vk_surface_view(rsx_vk_device* d, u32 surface, u32 remap, u32 fmt);

u32  rsx_vk_depth_target_create(rsx_vk_device* d, u32 w, u32 h);
void rsx_vk_depth_target_release(rsx_vk_device* d, u32 depth);
/* Resolve a zeta into a sampleable R32F texture. Returns
 * the handle of a texture slot, or 0 when there was nothing to resolve. */
u32  rsx_vk_depth_snapshot(rsx_vk_device* d, u32 depth, u32 w, u32 h);

/* The 1x1 opaque white image unbound units sample, created at
 * init after the format table and the staging slab exist. */
int rsx_vk_null_image_create(rsx_vk_device* d);
void rsx_vk_null_image_destroy(rsx_vk_device* d);

/* Finish every texture's open upload batch, so a texture uploaded in this
 * frame is in SHADER_READ_ONLY_OPTIMAL before anything samples it. */
void rsx_vk_textures_end_uploads(rsx_vk_device* d);

void rsx_vk_bind_textures(rsx_vk_device* d, const u32* textures,
                          const rsx_be_sampler_desc* samplers, u32 mask);
void rsx_vk_bind_vertex_textures(rsx_vk_device* d, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask);
void rsx_vk_clear_depth_stencil(rsx_vk_device* d, u32 depth, u32 flags,
                                float depth_value, u8 stencil);

/* ---- samplers (rsx_vk_sampler.c) -------------------------------------- */

/* The VkSampler for one rsx_be_sampler_desc, created once per distinct desc
 * and cached. `filterable` is 0 when the format the sampler is
 * paired with cannot filter linearly, which clamps min/mag/mip to NEAREST and
 * logs once (DESIGN §3.1 fallback 2). */
uint64_t rsx_vk_sampler_get(rsx_vk_device* d, const rsx_be_sampler_desc* sd,
                            int filterable);
void rsx_vk_samplers_destroy_all(rsx_vk_device* d);

/* Destroy every evicted sampler whose retirement predates the frame that is
 * now fenced, i.e. the fence-passed boundary (rsx_vk_device_submit_and_wait).
 * The same rule images and pipelines follow: a recorded or in-flight command
 * buffer may still name a sampler, so destroying one at eviction time was a
 * use-after-free waiting for a busy frame. */
void rsx_vk_samplers_collect(rsx_vk_device* d);

/* How many evicted samplers are waiting for the boundary, and how many had to
 * be dropped because the list was full. A test reads these. */
uint32_t rsx_vk_samplers_pending(const rsx_vk_device* d);
uint32_t rsx_vk_samplers_dropped(const rsx_vk_device* d);
/* The desc the cache entry for `sampler` was built from, with fallback 2's
 * unfilterable clamp already applied. Returns 1 and fills `out` for a live
 * cache entry, 0 for the null sampler or an unknown handle (item L2). */
int rsx_vk_sampler_desc_for(rsx_vk_device* d, uint64_t sampler,
                            rsx_be_sampler_desc* out);

/* The sampler an unnamed texture unit is paired with: point sampling, clamp
 * to edge, created once outside the cache and destroyed at shutdown. A
 * sampled-image descriptor still needs a sampler even when the program only
 * reads texels. */
uint64_t rsx_vk_null_sampler(rsx_vk_device* d);

/* Whether the fallback was applied. */
uint32_t rsx_vk_samplers_nearest_forced(const rsx_vk_device* d);

/* ---- texture helpers the target/view/depth code shares ----------------- */

/* The image a handle names, or NULL. A surface view resolves to its parent's
 * colour target. */
rsx_vk_image* rsx_vk_handle_image(rsx_vk_device* d, u32 handle);

/* The VkImageView and VkFormat a descriptor write wants for a handle, with a
 * surface view resolved to its own swizzled view. Returns 1 and fills both on
 * success, 0 when the handle names nothing sampleable. */
int rsx_vk_handle_view(rsx_vk_device* d, u32 handle, uint64_t* out_view,
                       uint32_t* out_format);

/* The same, plus the rsx_be_format and the rsx_vk_image the handle resolves
 * to. A sampleable handle that is *not* the image its own view belongs to (a
 * surface view) resolves to its parent target, which is what layout tracking
 * has to follow: the view and the target are one VkImage. Returns 1 with all
 * four filled, 0 when the handle names nothing sampleable. */
int rsx_vk_handle_sample(rsx_vk_device* d, u32 handle, uint64_t* out_view,
                         uint32_t* out_format, rsx_be_format* out_be,
                         rsx_vk_image** out_image);

/* True when a sampler may be used to sample an image of this format through
 * the descriptor set: the format must be a VkFormat this device can sample.
 * It takes the VkFormat rather than a handle so a caller that has already
 * resolved a surface view to its parent's target can ask the same question. */
int rsx_vk_format_sampleable(const rsx_vk_device* d, uint32_t vk_format);

/* Move `img` to SHADER_READ_ONLY_OPTIMAL for a sampled read, recording a
 * barrier from wherever the tracker says it is. This is what makes sampling a
 * rendered colour target legal: the descriptor declares SHADER_READ_ONLY
 * (rsx_vk_pipeline.c's write_descriptor_set), so a target left in
 * COLOR_ATTACHMENT_OPTIMAL after its pass has to be transitioned before the
 * draw that samples it, and transitioned back when it is next an attachment
 * (to_color_attachment does that side already). */
int rsx_vk_cmd_to_sampled_read(rsx_vk_device* d, rsx_vk_image* img);

/* The depth side of the same idea: DEPTH-only sampling for depth_snapshot, so
 * a DEPTH|STENCIL image is read through the aspect the sample declares. */
int rsx_vk_cmd_to_sampled_read_depth(rsx_vk_device* d, rsx_vk_image* img);

/* A depth image named by a texture unit of a draw (item M2): both aspects move
 * to DEPTH_STENCIL_READ_ONLY_OPTIMAL, the only layout a depth sampler may read
 * through, and the tracker records it so bind_sampled_read can declare the
 * same layout in the descriptor. */
int rsx_vk_cmd_to_depth_sampled_read(rsx_vk_device* d, rsx_vk_image* img);

/* ---- the offline SPIR-V cache path (DESIGN §2.5, S2 test case 3) ------- *
 *
 * A pipeline built from committed .spv bytes instead of from glslang, and the
 * draw that uses it. Same device, same pass, same draw as the HLSL path --
 * which is the only way "the blob works" can mean "the draw works". The test
 * drives this through the engine's own surface, so it returns 0 only when the
 * draw was recorded and submitted.
 */
uint32_t rsx_vk_spv_blob_pipeline(rsx_vk_device* d, const void* vs_spv,
                                  size_t vs_bytes, const void* ps_spv,
                                  size_t ps_bytes,
                                  const rsx_be_render_state* rs,
                                  const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt,
                                  u32 rt_count);
uint32_t rsx_vk_spv_blob_draw(const void* vs_spv, size_t vs_bytes,
                              const void* ps_spv, size_t ps_bytes, u32 surface,
                              const void* vertices, u32 vertex_count,
                              u32 stride, const u32* indices, u32 index_count);

/* ---- handles (rsx_vk_handle.c) ----------------------------------------- */
rsx_vk_handle* rsx_vk_handles_create(uint32_t capacity);
/* Destroy every live slot's objects, then the table. Called by shutdown. */
void rsx_vk_handle_destroy_all(rsx_vk_device* d);
void rsx_vk_handles_destroy(rsx_vk_handle* h);
/* 0 on failure (table full). */
u32 rsx_vk_handle_alloc(rsx_vk_handle* h, rsx_vk_slot** out);
rsx_vk_slot* rsx_vk_handle_get(rsx_vk_handle* h, u32 id);
/* Move the slot to the retired list; it is reported once more, then dies. */
void rsx_vk_handle_retire(rsx_vk_handle* h, u32 id);
/* Free every retired slot the caller has now destroyed. */
void rsx_vk_handle_collect(struct rsx_vk_device* d);
u32 rsx_vk_handle_live(const rsx_vk_handle* h);
u32 rsx_vk_handle_retired(const rsx_vk_handle* h);
/* The table's slot count. A loop that walks every live slot must end here and
 * not at a literal: the capacity is the device's to choose. */
u32 rsx_vk_handle_capacity(const rsx_vk_handle* h);

/* ---- the backend (rsx_vk_backend.c) ------------------------------------ */
/* The vtable the engine registers. Every member is real or a logged stub. */
const rsx_draw_backend* rsx_vk_backend(void);

/* The device behind the registered vtable, for a host that wants to read the
 * allocation counters or the last validation message. */
rsx_vk_device* rsx_vk_backend_device(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VK_H */
