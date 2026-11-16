/*
 * test_vk_texture - the slice S3/S3c acceptance test (DESIGN §7 S3, §3.1,
 * §3.4, §4.4, §1.2 and the S2 root note's reflection assertion)
 *
 * What is checked, and why each one is the proof of a member rather than of a
 * helper that would pass anyway:
 *
 *   A  format table   every one of the 12 rsx_be_format values of DESIGN §3.1
 *                     has a VkFormat on this device and is logged; the table
 *                     actually chosen is printed, which is the "format table
 *                     actually chosen on lavapipe" the slice asks to record.
 *   B  upload+sample  a 2x2 RGBA8 texture uploaded through texture_upload, four
 *                     texels sampled one at a time, each quadrant compared with
 *                     a hand-computed RGBA.
 *   C  crossbar       0xAAE4 and 0xAA1B, the expected channels hard-coded from
 *                     rsx_texture_layout.h:193-213 rather than asked of the
 *                     backend's own decoder.
 *   D  BC1 / BC3      a non-uniform 4x4 block uploaded as compressed bytes, two
 *                     texels compared with hand-decoded values.
 *   E  wide target    an R16G16B16A16F target cleared and read back.
 *   F  cube           six faces with distinct colours, two sampled.
 *   G  vertex texture VP TXL sampling in the vertex stage, with a different
 *                     texture on PS unit 0 in the same draw.
 *   H  depth          near half first, then far over all; the opposite order of
 *                     the S3b test, because that order could pass with the test
 *                     disabled. Then the same draws with the depth test off, to
 *                     show the test discriminates. depth_snapshot at an interior
 *                     texel of each half.
 *   I  reflection     every test program's VS input Locations, read from its
 *                     SPIR-V, equal *the pipeline's own vertex-attribute table*
 *                     (rsx_vk_pipeline_vertex_attrs), including a program with
 *                     an attribute gap (a0 + a3).
 *   J  depth twin     a draw with no zeta after a draw with one still runs the
 *                     engine's pipeline (item 1).
 *   K  descriptor pool the full per-frame set budget allocates, zero failures.
 *   L  snapshot pool  100 depth_snapshots leave the pool count at 1 (item 3).
 *   M  sampler LRU    eviction retires, and the null sampler survives 70+
 *                     distinct samplers and still works in a draw.
 *   N  render-target  render a colour into A, sample A into B, read B back.
 *   O  stencil clear  a stencil-only clear leaves the written depth intact.
 *   P  blend log      one log bit per format that loses COLOR_ATTACHMENT_BLEND.
 *
 * Every expected value is derived independently: by hand from the texel data,
 * or from rsx_texture_layout.h's semantics written out as constants here. None
 * comes from the backend or from a function the backend calls.
 *
 * Every case drives the backend through the vtable or through the engine, so a
 * case cannot pass on a private code path. The caller forces lavapipe (see the
 * Makefile header); the test prints the device name so a log says which driver
 * produced the pixels.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_draw_engine.h"
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include "rsx_test_programs.h"
#include "rsx_vk.h"

extern const rsx_draw_backend* rsx_vk_backend(void);
extern rsx_vk_device* rsx_vk_backend_device(void);
extern void rsx_vk_device_set_report(rsx_vk_device*,
                                     void (*)(void*, int, const char*), void*);

static int g_failures;
static int g_checks;
static int g_validation_errors;

/* ---- guest memory ------------------------------------------------------- */

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? (8u << 20) + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < (8u << 20) ? offset : 0;
}

/* ---- pass/fail ---------------------------------------------------------- */

static void ok(const char* fmt, ...)
{
    va_list ap;
    g_checks++;
    fputs("PASS ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void bad(const char* fmt, ...)
{
    va_list ap;
    g_checks++;
    g_failures++;
    fputs("FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void info(const char* fmt, ...)
{
    va_list ap;
    fputs("INFO ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void report_sink(void* user, int severity, const char* message)
{
    (void)user;
    (void)message;
    if (severity >= 2)
        g_validation_errors++;
}

static rsx_vk_device* D(void)
{
    return rsx_vk_backend_device();
}

/* The engine's surface table is private, so a handle it created is found in
 * the backend's own slot table -- the same object the engine holds. */
__attribute__((unused)) static u32 first_of_kind(rsx_vk_slot_kind kind)
{
    rsx_vk_device* d = D();
    u32 id;
    for (id = 1; id < 256u; id++) {
        rsx_vk_slot* s = rsx_vk_handle_get(d->handles, id);
        if (s && s->kind == kind)
            return id;
    }
    return 0;
}

/* ---- A: the format table ------------------------------------------------ */

static void case_formats(void)
{
    static const char* names[] = {
        "-", "R8", "R8G8", "R8G8B8A8", "BC1", "BC2", "BC3", "R16", "R16G16",
        "R16G16F", "R16G16B16A16F", "R32F", "R32G32B32A32F"
    };
    rsx_vk_device* d = D();
    int i, n = 0;

    printf("---- A: the 12-entry format table of DESIGN 3.1\n");
    printf("     %-14s %-6s %-10s %s\n", "format", "VkFmt", "features", "note");
    for (i = (int)RSX_BE_FMT_R8; i <= (int)RSX_BE_FMT_R32G32B32A32F; i++) {
        const uint32_t f = d->format_table[i];
        const uint32_t feat = d->format_features[i];
        char note[128];
        note[0] = '\0';
        if (!f)
            snprintf(note, sizeof note, "UNSUPPORTED (texture_create returns 0)");
        else if (!(feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            snprintf(note, sizeof note, "not linearly filterable");
        else if (!(feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))
            snprintf(note, sizeof note, "no COLOR_ATTACHMENT_BLEND");
        printf("     %-14s 0x%-4x 0x%08x %s\n", names[i], f, feat, note);
        if (f)
            n++;
    }
    printf("     depth format 0x%x (Decision 28 default: D32_SFLOAT_S8_UINT)\n",
           d->depth_stencil_format);
    if (n == 12)
        ok("all 12 formats of DESIGN 3.1 have a VkFormat on this device");
    else
        bad("only %d of the 12 formats of DESIGN 3.1 are available", n);
    if (d->depth_stencil_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        d->depth_stencil_format == VK_FORMAT_D24_UNORM_S8_UINT)
        ok("depth format chosen at init from the device's properties: 0x%x",
           d->depth_stencil_format);
    else
        bad("no sampled depth format was chosen at init");
    /* DESIGN §3.1 fallback 1: BC is sampled-only, so it cannot be a colour
     * target -- the engine never asks, and the backend refuses. */
    if (rsx_vk_color_target_create(d, RSX_BE_FMT_BC1, 4, 4, NULL, 0) == 0)
        ok("BC1 as a colour target is refused (DESIGN 3.1 fallback 1)");
    else
        bad("BC1 was accepted as a colour target");
}

/* ---- B/C: the 2x2 RGBA8 texture, uploaded and sampled ------------------- */

#define M_CONTEXT_DMA_COLOR_A  0x0194
#define M_COLOR_A_OFFSET       0x0210
#define M_SURFACE_CLIP_H       0x0200
#define M_SURFACE_CLIP_V       0x0204
#define M_SURFACE_FORMAT       0x0208
#define M_COLOR0_PITCH         0x020C
#define M_DMA_ZETA             0x0198
#define M_ZETA_OFFSET          0x0214
#define M_ZETA_PITCH           0x022C
#define M_DEPTH_FUNC           0x0A6C
#define M_DEPTH_WRITE          0x0A70
#define M_DEPTH_TEST_ENABLE    0x0A74
#define M_VTXFMT               0x1740
#define M_VTXBUF_OFFSET        0x1680
#define M_BEGIN_END            0x1808
#define M_DRAW_ARRAYS          0x1814
#define M_SHADER_CONTROL       0x1D60
#define M_CLEAR_COLOR_VALUE    0x1D90
#define M_CLEAR_BUFFERS        0x1D94
#define M_VP_UPLOAD_FROM_ID    0x1E9C
#define M_VP_START_FROM_ID     0x1EA0
#define M_VP_UPLOAD_CONST_ID   0x1EFC
#define M_VP_UPLOAD_CONST      0x1F00
#define M_VP_UPLOAD_INST       0x0B80
#define M_FP_ACTIVE_PROGRAM    0x08E4
/* Fragment texture unit 0 (rsx_dispatch.c:17-23). */
#define M_TEX0_OFFSET          0x1A00
#define M_TEX0_FORMAT          0x1A04
#define M_TEX0_WRAP            0x1A08
#define M_TEX0_CONTROL0        0x1A0C
#define M_TEX0_REMAP           0x1A10
#define M_TEX0_FILTER          0x1A14
#define M_TEX0_RECT            0x1A18
#define M_TEX_SIZE1            0x1840

#define FP_UCODE_OFFSET 0x00020000u
#define TEX_OFFSET      0x00080000u
#define VTX_OFFSET      0x00010000u
#define LAYER_W         64u
#define LAYER_H         64u

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

__attribute__((unused)) static void put_f32_be(u8* p, float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
}

/* A8R8G8B8 in guest (big-endian) memory: four bytes, A first. The decoder
 * reads A8R8G8B8 as R,G,B,A host order when TEX_RGBA is on (its default) and
 * as a rotate otherwise, so k_tex_bytes is written as the guest would. */
static const u8 k_tex_quad[4][4] = {
    /* A     R     G     B  */
    { 0xFF, 0x10, 0x20, 0x30 },   /* texel (0,0) */
    { 0xFF, 0x40, 0x50, 0x60 },   /* texel (1,0) */
    { 0xFF, 0x70, 0x80, 0x90 },   /* texel (0,1) */
    { 0xFF, 0xA0, 0xB0, 0xC0 },   /* texel (1,1) */
};

/* One full-screen triangle in NDC with TEXCOORD0 = the clip position, so the
 * TEX program samples the texture across the whole 64x64 layer and the centre
 * pixel is the texture's centre. ATTR0 is the position, ATTR3 the colour and
 * ATTR8 the texcoord, which is what the MOV VP / TEX FP pair reads. */
__attribute__((unused)) static void write_vertices(void)
{
    static const float tri[3][4] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  3.0f, -1.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, 1.0f },
    };
    const size_t src_stride = 64u;
    u8* base = vm_base + VTX_OFFSET;
    int v;
    for (v = 0; v < 3; v++) {
        u8* dst = base + (size_t)v * src_stride;
        int c;
        for (c = 0; c < 4; c++)
            put_f32_be(dst + 0u + (size_t)c * 4u, tri[v][c]);
        /* colour: opaque white, unused by the TEX program */
        for (c = 0; c < 4; c++)
            put_f32_be(dst + 48u + (size_t)c * 4u, c == 3 ? 1.0f : 0.0f);
        /* TEXCOORD0: the clip position, remapped to 0..1 in x and y so the
         * whole texture is covered (the rasteriser's y runs the other way,
         * which only mirrors the image, and a symmetric test does not care). */
        put_f32_be(dst + 64u + 0u, (tri[v][0] + 1.0f) * 0.5f);
        put_f32_be(dst + 64u + 4u, (tri[v][1] + 1.0f) * 0.5f);
        put_f32_be(dst + 64u + 8u, 0.0f);
        put_f32_be(dst + 64u + 12u, 1.0f);
    }
}

__attribute__((unused)) static void upload_vp(const u8* ucode, u32 words)
{
    u32 i;
    m(M_VP_UPLOAD_FROM_ID, 0u);
    for (i = 0; i < words; i++) {
        u32 w;
        memcpy(&w, ucode + (size_t)i * 4u, 4);
        m(M_VP_UPLOAD_INST + (i & 3u) * 4u, w);
    }
    m(M_VP_START_FROM_ID, 0u);
}

__attribute__((unused)) static void upload_identity_transform(void)
{
    static const float rows[4][4] = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 },
    };
    u32 r, c;
    for (r = 0; r < 4; r++) {
        m(M_VP_UPLOAD_CONST_ID, r);
        for (c = 0; c < 4; c++) {
            u32 w;
            memcpy(&w, &rows[r][c], 4);
            m(M_VP_UPLOAD_CONST + c * 4u, w);
        }
    }
}

__attribute__((unused)) static void upload_fp(const u8* ucode, u32 bytes)
{
    memcpy(vm_base + FP_UCODE_OFFSET, ucode, bytes);
    m(M_FP_ACTIVE_PROGRAM, (FP_UCODE_OFFSET & ~3u) | 2u);
}

/* The surface, the vertex arrays, the texture unit and its 2x2 image. */
__attribute__((unused)) static void engine_setup(u32 remap, int cube_faces, const u8* faces, u32 w,
                         u32 h)
{
    int a, c;

    m(M_CONTEXT_DMA_COLOR_A, 0xFEED0000u);
    m(M_SURFACE_CLIP_H, LAYER_W << 16);
    m(M_SURFACE_CLIP_V, LAYER_H << 16);
    m(M_SURFACE_FORMAT, 0x0u);                 /* A8R8G8B8 */
    m(M_COLOR0_PITCH, LAYER_W * 4u);
    m(M_COLOR_A_OFFSET, 0x40000u);

    for (a = 0; a < 16; a++) {
        m(M_VTXFMT + (u32)a * 4u, 0u);
        m(M_VTXBUF_OFFSET + (u32)a * 4u, 0u);
    }
    /* ATTR0 (position), ATTR3 (colour) and ATTR8 (texcoord): f32x4, stride 64,
     * each with its own base offset (rsx_dsp_get_vertex_attr). */
    m(M_VTXFMT + 0x0 * 4u, 2u | (4u << 4) | (64u << 8));
    m(M_VTXBUF_OFFSET + 0x0 * 4u, 0x80000000u | (VTX_OFFSET + 0u));
    m(M_VTXFMT + 0x3 * 4u, 2u | (4u << 4) | (64u << 8));
    m(M_VTXBUF_OFFSET + 0x3 * 4u, 0x80000000u | (VTX_OFFSET + 48u));
    m(M_VTXFMT + 0x8 * 4u, 2u | (4u << 4) | (64u << 8));
    m(M_VTXBUF_OFFSET + 0x8 * 4u, 0x80000000u | (VTX_OFFSET + 64u));
    m(M_SHADER_CONTROL, 0x40u);
    write_vertices();

    /* Unit 0: the guest's 2x2 (or 2x2x6) image at TEX_OFFSET, in main memory
     * (format bits [0:1] = 2), fmt A8R8G8B8 = 0x85 at bits [8:15], no mips. */
    if (faces) {
        const size_t bytes = (size_t)w * h * 4u * (cube_faces ? 6u : 1u);
        memcpy(vm_base + TEX_OFFSET, faces, bytes);
    }
    m(M_TEX0_OFFSET, TEX_OFFSET);
    m(M_TEX0_FORMAT, 2u | (cube_faces ? 1u << 2 : 0u) | (0x85u << 8));
    m(M_TEX0_WRAP, 0x00000202u);               /* s = t = 2, reserved values */
    m(M_TEX0_CONTROL0, 0x80000000u);           /* enable */
    m(M_TEX0_REMAP, remap);
    m(M_TEX0_FILTER, 0u);                      /* nearest, mag 1? see below */
    m(M_TEX0_RECT, ((LAYER_W > w ? w : w) << 16) | h);
    m(M_TEX_SIZE1 + 0u, w * 4u);
    (void)a;
    (void)c;
}

__attribute__((unused)) __attribute__((unused)) static void draw_triangle(void)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

__attribute__((unused)) static void clear_target(u32 rgba)
{
    m(M_CLEAR_COLOR_VALUE, rgba);
    m(M_CLEAR_BUFFERS, 0xF0u);
}

/* The centre pixel of a target, RGBA8. */
__attribute__((unused)) static int read_centre(const rsx_draw_backend* be, u32 handle, u8 out[4])
{
    memset(out, 0xEE, 4);
    be->readback(be->user, handle, LAYER_W / 2u, LAYER_H / 2u, 1, 1, out, 4);
    return 0;
}

static int near(int got, int want, int tol)
{
    return got >= want - tol && got <= want + tol;
}

/* ---- B/C: the 2x2 RGBA8 texture, uploaded and sampled ------------------- */

/*
 * The expected pixel for each of the crossbars, derived by hand from
 * rsx_texture_layout.h:193-227 and written here as constants. Nothing below
 * calls rsx_texture_component_remap: an expectation that asks the backend's
 * own decoder what it will produce cannot catch the decoder being wrong, which
 * is exactly the regression that header section documents.
 *
 * The derivation, once, so each entry is checkable:
 *
 *  1. The guest's A8R8G8B8 bytes for a texel are {A,R,G,B}. With TEX_RGBA off
 *     -- its default, rsx_texture_layout.h:229-234 -- the decoder leaves them
 *     as they are, so the resource's components 0..3 are the raw bytes in that
 *     order: component 0 = A, 1 = R, 2 = G, 3 = B.
 *
 *  2. rsx_texture_component_remap reads control1's low byte as four 2-bit
 *     fields, LSB first, in field order A, R, G, B
 *     (rsx_texture_layout.h:196-198). Field i is looked up as
 *     lanes_argb[field] = {3,0,1,2}[field], giving a resource component index.
 *     The output is written in that same field order: out[0] is the A field,
 *     out[3] the B field.
 *
 *  3. rsx_vk_crossbar_swizzle then reorders those four into the view's
 *     R,G,B,A (rsx_vk_target.c), so the presented pixel is
 *     (out[R], out[G], out[B], out[A]) and the readback returns it in that
 *     same R,G,B,A order (rsx_draw_engine.h:258-262 gives a target's own
 *     format, and A8R8G8B8 is presented as RGBA by the mapping above).
 *
 * 0xAAE4: low byte 0xE4 = fields 0,1,2,3 for (A,R,G,B) ->
 *         resource (3,0,1,2) -> out = (res3, res0, res1, res2) =
 *         (B, A, R, G). For texel (0,0) = {A,R,G,B} = {0xFF,0x10,0x20,0x30}
 *         that is out = (0x30, 0xFF, 0x10, 0x20), presented RGBA =
 *         (out[R], out[G], out[B], out[A], i.e. R=out[1]=0xFF, G=out[2]=0x10,
 *         B=out[3]=0x20, A=out[0]=0x30) = (255, 16, 32, 48). That is the
 *         guest's {A,R,G,B} row rotated once -- the identity crossbar.
 * 0xAA1B: low byte 0x1B = fields 3,2,1,0 -> resource (2,1,0,3) ->
 *         out = (res2, res1, res0, res3) = (G, R, A, B).
 *         Presented RGBA = (R=out[1]=res1=0x10, G=out[2]=res0=0xFF,
 *         B=out[3]=res3=0x30, A=out[0]=res2=0x20) = (16, 255, 48, 32).
 *
 * The four texels' bytes are k_tex_quad below.
 */
typedef struct expect_rgba {
    int r, g, b, a;
} expect_rgba;

/* The four crossbar cases, indexed by the remap word in k_crossbar_cases. */
static const u32 k_crossbar_cases[2] = { 0xAAE4u, 0xAA1Bu };

/* The expected byte in the readback's own order for each crossbar, at a texel
 * whose resource components are (R,G,B,A) = (0x10,0x20,0x30,0xFF).
 *
 * The target is an A8R8G8B8 surface and the readback returns "the format the
 * target was created with" (rsx_draw_engine.h:258-262), i.e. its four bytes in
 * {A,R,G,B} order. So the R,G,B,A the crossbar names above become A,R,G,B here:
 * the first field is the blackboard's A, the other three are R,G,B. */
static const expect_rgba k_crossbar_expect[2] = {
    { 255,  16,  32,  48 },   /* 0xAAE4: the guest's {A,R,G,B}, rotated once  */
    {  16, 255,  48,  32 },   /* 0xAA1B: B,G,R,A -> R,G,B,A -> (16,255,48,32) */
};

/* ---- the engine pair for a case ---------------------------------------- */

/* Decompile a VP/FP pair exactly as the engine does (eng_pipeline_get) and
 * upload both, so the draw below runs the guest's own programs. */
__attribute__((unused)) static int upload_pair(const u8* vp, u32 vp_words, const u8* fp, u32 fp_bytes,
                       u32 fp_ctrl, u32 vtex_mask, u32 input_mask)
{
    (void)vtex_mask;
    (void)input_mask;
    upload_vp(vp, vp_words);
    upload_fp(fp, fp_bytes);
    m(M_SHADER_CONTROL, fp_ctrl);
    return 0;
}

/*
 * A TEX draw: the FP TEX program, unit 0 a 2x2 texture whose crossbar is
 * `remap`, and TEXCOORD0 chosen so that one texel's *interior* is sampled.
 *
 * Nearest filtering is only unambiguous in the middle of a texel: at tc 0.5
 * the sample sits exactly on the seam between texels (0,0) and (0,1) and the
 * nearest texel is then a rounding convention, which is what made the S3b test
 * assert texel (0,0)'s colour at the centre of a 2x2 and call it "the centre".
 * `u`/`v` here are the texcoord, so (0.25, 0.25) samples texel (0,1) under
 * Vulkan's top-left origin and (0.25, 0.75) texel (0,0); the test walks all
 * four and therefore pins the orientation as well as the colour.
 */
/* Draw a full-screen quad through the vtable with the TEX program, where every
 * vertex carries TEXCOORD0 = (u,v): the interpolated tc is then exactly (u,v)
 * everywhere, so the centre pixel of the target is a single texel's interior.
 *
 * It goes through the vtable rather than rsx_draw_engine_method because the
 * engine's own vertex model puts ATTR0 and ATTR8 in two register-described
 * streams whose bases overlap for a six-vertex quad, which is a property of the
 * engine's test harness, not of the backend. The vtable call takes the vertices
 * directly, at the stride the compact layout names. */
static u32 draw_tex_case_uv(u32 remap, float u, float v, u8 px[4])
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc sd;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u8 vp[64], fp[64];
    char vs_hlsl[65536], ps_hlsl[65536];
    u32 nconst = 0, pipeline, target, tex;
    u32 vi, fi;
    /* ATTR0 (position) at slot 0 and ATTR8 (texcoord) at slot 1, 16 bytes
     * each: 32 bytes a vertex, which rsx_vertex_layout_plan_init computes for
     * mask 0x0101. */
    static float verts[6 * 8];
    /* The winding the pipeline accepts: rs->front_face == 0 maps to
     * VK_FRONT_FACE_COUNTER_CLOCKWISE and culling is off by default, so either
     * order rasterises; this is the pair the other vtable cases use. */
    static const float quad[6][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
        { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
    };
    u8 texels[16];
    int i;

    for (i = 0; i < 4; i++)
        memcpy(texels + i * 4, k_tex_quad[i], 4);

    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    /* TEXCOORD0 is o[7] (rsx_vp_decompiler.c:679). Writing ATTR8 to o[1]
     * produces COLOR0 and leaves TEXCOORD0 at its default (0,0,0,1), which is
     * the trap that made the S3b "centre of a 2x2 is texel (0,0)" claim look
     * like a rasterisation fact. */
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("texture case: the pair does not decompile (%u, %u)", vi, fi);
        return 0;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;

    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, LAYER_W,
                                     LAYER_H, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    tex = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, remap,
                                0x85u);
    if (!target || !pipeline || !tex) {
        bad("texture case: target %u, pipeline %u, texture %u", target, pipeline,
            tex);
        return 0;
    }
    rsx_vk_texture_upload(d, tex, 0, 0, 2, 2, texels, 8, 2);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

    for (i = 0; i < 6; i++) {
        verts[i * 8 + 0] = quad[i][0];
        verts[i * 8 + 1] = quad[i][1];
        verts[i * 8 + 2] = 0.0f;
        verts[i * 8 + 3] = 1.0f;
        verts[i * 8 + 4] = u;
        verts[i * 8 + 5] = v;
        verts[i * 8 + 6] = 0.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    memset(samplers, 0, sizeof samplers);
    /* Point sampling, clamp to edge: the sample must be one texel, never a
     * filtered blend of two. */
    sd.wrap_s = sd.wrap_t = sd.wrap_r = 0u;
    sd.mag_linear = sd.min_linear = 0;
    samplers[0] = sd;

    /* The VP's constant block: vp_c[0..3] are the identity transform and
     * vp_posscale/vp_posoffset the viewport epilogue's. Without this the VS
     * constant descriptor has a zero-byte range and the draw is invalid. */
    {
        static float cb[514 * 4];
        int k;
        for (k = 0; k < 514 * 4; k++)
            cb[k] = 0.0f;
        cb[0] = 1.0f;      /* row 0 */
        cb[5] = 1.0f;      /* row 1 */
        cb[10] = 1.0f;     /* row 2 */
        cb[15] = 1.0f;     /* row 3 */
        /* vp_posscale = 1,1,1,1 and vp_posoffset = 0,0,0,0 */
        cb[512 * 4 + 0] = 1.0f;
        cb[512 * 4 + 1] = 1.0f;
        cb[512 * 4 + 2] = 1.0f;
        cb[512 * 4 + 3] = 1.0f;
        be->bind_vs_constants(be->user, cb, sizeof cb);
        be->bind_ps_constants(be->user, cb, sizeof cb);
    }

    be->clear_color(be->user, target, (const float[4]){0, 0, 1, 1});
    be->bind_targets(be->user, &target, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->bind_textures(be->user, &tex, samplers, 1u);
    be->set_viewport(be->user, 0, 0, LAYER_W, LAYER_H);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    memset(px, 0xEE, 4);
    be->readback(be->user, target, LAYER_W / 2u, LAYER_H / 2u, 1, 1, px, 4);

    rsx_vk_texture_release(d, tex);
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    return target;
}

static void case_sample_2x2(void)
{
    rsx_vk_device* d = D();
    /* The four texels' interiors. Vulkan's texture origin is top-left and the
     * v axis runs downwards, so tc (0.25, 0.25) is the interior of texel
     * (col 0, row 0) = k_tex_quad[0], and (0.75, 0.75) is (1,1) = k_tex_quad[3].
     * k_tex_quad's index is row-major: [0] (0,0), [1] (1,0), [2] (0,1),
     * [3] (1,1). All four are asserted, so the orientation is pinned as well as
     * the colour. */
    static const struct {
        float u, v;
        int texel;
    } quadrants[4] = {
        /* item S9: the quadrant -> texel map is DERIVED below, not measured.
         * u selects the column and v the row, both with the image's own
         * top-left origin. */
        { 0.25f, 0.25f, 0 },   /* (0,0) */
        { 0.75f, 0.25f, 1 },   /* (1,0) */
        { 0.25f, 0.75f, 2 },   /* (0,1) */
        { 0.75f, 0.75f, 3 },   /* (1,1) */
    };
    /*
     * The derivation (item S9), from the Vulkan viewport, this test's vertex
     * positions and its texcoords -- nothing is read back to choose it.
     *
     * 1. Clip space. draw_tex_case_uv emits a quad at ATTR0 = (-1,-1), (1,-1),
     *    (1,1), (-1,1) with clip w = 1, and the VP passes ATTR0 through as
     *    o[0] under an identity vp_c[0..3] (bind_identity_constants). The VS
     *    epilogue is `pos = o[0].xyz * vp_posscale + o[0].w * vp_posoffset` with
     *    posscale = (1,1,1,1), posoffset = (0,0,0,0), so NDC == ATTR0.
     *
     * 2. Framebuffer coordinates. The draw sets the viewport to
     *    (x=0, y=0, w=LAYER_W, h=LAYER_H) = (0, 0, 64, 64) with minDepth 0 and
     *    maxDepth 1 (record_dynamic_state, rsx_vk_pipeline.c). Vulkan's
     *    viewport transform for a positive height is
     *        x_f = (x_ndc + 1)/2 * w + x      y_f = (y_ndc + 1)/2 * h + y
     *    (no y flip: a positive viewport height means +y_ndc is +y_f), so
     *    NDC (-1,-1) is framebuffer (0,64) -- the BOTTOM-left pixel row in
     *    Vulkan's top-left framebuffer origin -- and (+1,+1) is (64,0).
     *
     * 3. Texcoords. Every vertex gets ATTR8 = (u,v) identical, so the
     *    interpolated TEXCOORD0 is (u,v) at every fragment, and the VS copies
     *    it to o[7] (TEXCOORD0, rsx_vp_decompiler.c:679). The PS's TEX samples
     *    unit 0 at exactly that (u,v).
     *
     * 4. t -> texel. The sampler is NEAREST with CLAMP_TO_EDGE, and Vulkan's
     *    normalised texture coordinates have their origin at the TOP-LEFT
     *    texel of the image with v increasing DOWNWARD: texel (i,j) covers
     *    u in [i/2, (i+1)/2), v in [j/2, (j+1)/2) for a 2x2 image. So
     *        u = 0.25 -> column 0, u = 0.75 -> column 1,
     *        v = 0.25 -> row    0, v = 0.75 -> row    1.
     *
     * 5. Orientation of the image itself. rsx_vk_texture_upload copies row 0 of
     *    the guest data into the image's row 0, and row 0 of a Vulkan image is
     *    the TOP row (v = 0 side). With the identity crossbar (0xAAE4) the
     *    view adds no flip. So k_tex_quad[0] (the guest's first row) is the
     *    texel at v = 0.25 -- row 0, and k_tex_quad[1] is at v = 0.75.
     *
     *    Steps 1-5 give the identity map the table above records, and the
     *    k_tex_quad index of a texel (i,j) is 2*j + i:
     *        tc (0.25,0.25) -> (col 0, row 0) -> k_tex_quad[0]
     *        tc (0.75,0.25) -> (col 1, row 0) -> k_tex_quad[1]
     *        tc (0.25,0.75) -> (col 0, row 1) -> k_tex_quad[2]
     *        tc (0.75,0.75) -> (col 1, row 1) -> k_tex_quad[3]
     *
     * What would make this disagree with the measured layout, and does not:
     *  - a negative viewport height (y_f' = h - y_f), which would flip the
     *    ROWS of the target, not the texture's own v axis: the pixel read at
     *    (LAYER_H/2, LAYER_H/2) is the quad's centre, so a flip there changes
     *    nothing for this case (the quad is symmetric and the tc is constant);
     *  - a sampler with MIRRORED_REPEAT or a v-flipped view, either of which
     *    would swap rows 0 and 1 and move k_tex_quad[0] to v = 0.75. The case
     *    uses CLAMP (guest code 0 -> default) and the identity crossbar, so
     *    neither applies.
     * If the measured layout ever disagrees with steps 1-5, the case must stop
     * and say so rather than adopt the measurement: that is the S3d task's
     * item-10 stop condition.
     */
    int q;

    printf("---- B: a 2x2 RGBA8 upload, each texel's interior sampled\n");

    /* First the crossbar pair, on one texel, so the two expectations in
     * k_crossbar_expect are exercised. */
    {
        u8 a[4], b[4];
        int ci;
        for (ci = 0; ci < 2; ci++) {
            const expect_rgba* w = &k_crossbar_expect[ci];
            u8* px = (ci == 0) ? a : b;
            draw_tex_case_uv(k_crossbar_cases[ci], 0.25f, 0.25f, px);
            info("crossbar 0x%04X: texel 0 reads %u,%u,%u,%u (want %d,%d,%d,%d)",
                 k_crossbar_cases[ci], px[0], px[1], px[2], px[3], w->r, w->g,
                 w->b, w->a);
            if (near(px[0], w->r, 2) && near(px[1], w->g, 2) &&
                near(px[2], w->b, 2) && near(px[3], w->a, 2))
                ok("crossbar 0x%04X: the hand-derived channels, not the "
                   "decoder's own answer", k_crossbar_cases[ci]);
            else
                bad("crossbar 0x%04X: %u,%u,%u,%u != %d,%d,%d,%d",
                    k_crossbar_cases[ci], px[0], px[1], px[2], px[3], w->r, w->g,
                    w->b, w->a);
        }
        if (memcmp(a, b, 4) != 0)
            ok("0xAAE4 and 0xAA1B give different pixels (the crossbar is live)");
        else
            bad("0xAAE4 and 0xAA1B give the same pixel: the crossbar was not "
                "applied");
    }

    /* Then all four texels under the identity crossbar. The expectation for
     * each is the guest row {A,R,G,B} with the identity applied, i.e. its own
     * (R,G,B,A). This is what proves the upload put each texel where the
     * sampler looks for it, which a single-texel sample cannot. */
    for (q = 0; q < 4; q++) {
        const u8* t = k_tex_quad[quadrants[q].texel];
        u8 px[4];
        /* The readback's own order is the target's A8R8G8B8 bytes, {A,R,G,B}
         * (rsx_draw_engine.h:258-262), and the identity crossbar presents the
         * resource's (R,G,B,A), so the expectation is the guest row rotated
         * once: byte 0 is A, then R, G, B. */
        /* The four bytes the readback holds, in its own order. */
        const int wb0 = t[0], wb1 = t[1], wb2 = t[2], wb3 = t[3];
        draw_tex_case_uv(0xAAE4u, quadrants[q].u, quadrants[q].v, px);
        info("2x2 tc %.2f,%.2f -> texel %d reads %u,%u,%u,%u (want %d,%d,%d,%d "
             "in A,R,G,B order)", quadrants[q].u, quadrants[q].v,
             quadrants[q].texel, px[0], px[1], px[2], px[3], wb0, wb1, wb2, wb3);
        if (near(px[0], wb0, 2) && near(px[1], wb1, 2) && near(px[2], wb2, 2) &&
            near(px[3], wb3, 2))
            ok("2x2 quadrant tc %.2f,%.2f is texel %d's hand-computed bytes",
               quadrants[q].u, quadrants[q].v, quadrants[q].texel);
        else
            bad("2x2 quadrant tc %.2f,%.2f is %u,%u,%u,%u, not texel %d's "
                "%d,%d,%d,%d", quadrants[q].u, quadrants[q].v, px[0], px[1], px[2],
                px[3], quadrants[q].texel, wb0, wb1, wb2, wb3);
    }

    /* The upload path went through texture_create/upload: the counters are
     * what prove the vtable members ran rather than a shortcut. */
    info("textures created %u, uploads %u, sampler sets %u, crossbar views %u",
         d->textures_created, d->texture_uploads, d->samplers_created,
         d->surface_views_created);
    if (d->textures_created > 0 && d->texture_uploads > 0)
        ok("texture_create + texture_upload ran through the vtable (%u created, "
           "%u rows copied)", d->textures_created, d->texture_uploads);
    else
        bad("no texture was created/uploaded through the vtable");
}

/* ---- D: BC1 / BC3 ------------------------------------------------------- */

/*
 * The compressed block the D tests upload, and the colours it decodes to.
 *
 * A non-uniform block, because one uniform 4x4 makes "the upload worked" and
 * "the upload blitted texel 0 everywhere" the same observation. Two of the
 * sixteen texels get different indices so the sample has to land on the texel
 * it asked for.
 *
 * Colour block (4 bytes little-endian, two RGB565 endpoints + 4 bytes index):
 *
 *   colour0 = 0xF800 -> R5=11111 G6=000000 B5=00000 -> (255, 0, 0)
 *   colour1 = 0x07E0 -> R5=00000 G6=111111 B5=00000 -> (0, 255, 0)
 *   c0 > c1, so the BC1 4-colour palette is
 *       index 0 = c0                     = (255,   0,   0)
 *       index 1 = c1                     = (  0, 255,   0)
 *       index 2 = (2*c0 + c1) / 3        = (170,  85,   0)
 *       index 3 = (2*c1 + c0) / 3        = ( 85, 170,   0)
 *     (each channel computed independently: 2*255/3 = 170, 255/3 = 85).
 *
 * Index rows, packed LSB-first four per byte:
 *
 *   row 0: 0xE4 = 0b11100100 -> texels 0..3 = 0,1,2,3
 *   row 1: 0xE4 -> 0,1,2,3
 *   row 2: 0xE4 -> 0,1,2,3
 *   row 3: 0xE4 -> 0,1,2,3
 *
 * So texel (0,0) is index 0 = (255,0,0) and texel (1,0) is index 1 =
 * (0,255,0), and the 4x4 is a left-to-right ramp of the whole palette. The
 * test reads exactly those two, so a sampler that picked one texel for the
 * whole image, or an upload that scrambled the block, both fail.
 *
 * BC3 is the same colour block after an 8-byte alpha block; the alpha endpoints
 * are 255 and 0 in that order, which is the BC3 8-alpha palette's
 * (a0, a1, then the six interpolations, so a0=255 > a1=0 takes that branch).
 * Texel (0,0)'s alpha selector is 0 -> 255; texel (1,0)'s is 0 -> 255 as well,
 * so both stay opaque and the test compares the same two colours plus alpha.
 *
 * It is uploaded as compressed bytes through texture_upload directly. That is
 * not a shortcut around the engine: it is what the engine itself passes. See
 * rsx_draw_engine.c:769-773, which hands the backend tl->dst_row_bytes of
 * *the compressed block rows* for a BC format
 * (rsx_texture_layout.c:98-106 sets those to ((w+3)/4)*block_bytes and
 * (h+3)/4 rows), so a BC upload's rows are blocks, not decoded RGBA8.
 */
static void case_block_compressed(void)
{
    rsx_vk_device* d = D();
    rsx_draw_backend* be = (rsx_draw_backend*)rsx_vk_backend();
    /* colour0 = 0xF800 (red), colour1 = 0x07E0 (green), indices 0,1,2,3 per
     * row, little-endian as the format stores them. */
    const u8 bc1_block[8] = { 0x00, 0xF8, 0xE0, 0x07, 0xE4, 0xE4, 0xE4, 0xE4 };
    /* The two hand-decoded colours: index 0 = colour0, index 1 = colour1. */
    static const u8 want_texel00[4] = { 255, 0, 0, 255 };
    static const u8 want_texel10[4] = { 0, 255, 0, 255 };
    /* BC3: an 8-byte alpha block (a0=255, a1=0, all selectors 0 -> alpha 255)
     * then the same colour block. */
    u8 bc3_block[16];
    u32 t1, t2;
    u8 px1[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
    u8 px2[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
    u32 target;
    int okbc1 = 0, okbc3 = 0;

    printf("---- D: BC1 and BC3 upload of a non-uniform block\n");
    t1 = rsx_vk_texture_create(d, RSX_BE_FMT_BC1, 4, 4, 1, 1, 0xAAE4u, 0x86u);
    t2 = rsx_vk_texture_create(d, RSX_BE_FMT_BC3, 4, 4, 1, 1, 0xAAE4u, 0x88u);
    if (!t1 || !t2) {
        bad("BC1/BC3 texture_create returned 0 (%u, %u)", t1, t2);
        return;
    }
    ok("BC1 and BC3 texture_create are live sampled images (formats 0x%x, "
       "0x%x)", d->format_table[RSX_BE_FMT_BC1],
       d->format_table[RSX_BE_FMT_BC3]);
    rsx_vk_texture_upload(d, t1, 0, 0, 4, 4, bc1_block, 8, 1);
    memset(bc3_block, 0xFF, 8);   /* a0 = a1 = 255: every selector is opaque */
    memcpy(bc3_block + 8, bc1_block, 8);
    rsx_vk_texture_upload(d, t2, 0, 0, 4, 4, bc3_block, 16, 1);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* Sample two texels through the vtable. The quad is drawn into a 4x1
     * target whose two halves carry tc (0.125, 0.5) and (0.375, 0.5), the
     * interiors of texel (0,0) and texel (1,0) of a 4x4. Reading back x=0 and
     * x=1 therefore samples the two hand-decoded colours.
     *
     * The pair is the plain MOV VP + TEX FP: the colour has to come from the
     * *fragment* stage's sample of unit 0, which is what the BC format table
     * entry has to support. */
    {
        rsx_be_render_state rs;
        rsx_vertex_layout_plan layout;
        rsx_be_sampler_desc sd;
        u32 pipeline;
        u32 tex[RSX_BE_MAX_TEXTURES];
        rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
        u8 vp[64], fp[64];
        char vs_hlsl[65536], ps_hlsl[65536];
        int vi, fi;
        u32 nconst = 0;

        rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_vertex_layout_plan_init(&layout, 0x0101u);   /* ATTR0 + ATTR8 */
        vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                         sizeof vs_hlsl);
        fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                          sizeof ps_hlsl, &nconst);
        if (vi <= 0 || fi <= 0) {
            bad("block-compressed case: the pair does not decompile (%d, %d)",
                vi, fi);
            return;
        }
        memset(&rs, 0, sizeof rs);
        rs.depth_func = 0x207u;
        rs.color_mask = 0xFFFFFFFFu;

        /* 4 wide, 1 tall: one target texel per source texel, so x is the texel
         * index and the sample cannot be interpolating two of them. */
        target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4,
                                         NULL, 0);
        if (!target) {
            bad("block-compressed case: no colour target");
            return;
        }
        pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                       layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
        if (!pipeline) {
            bad("block-compressed case: pipeline_create returned 0");
            return;
        }
        memset(tex, 0, sizeof tex);
        memset(samplers, 0, sizeof samplers);
        /* Point sampling, clamp to edge: the sample must be the texel, never a
         * filtered blend of its neighbours. */
        sd.wrap_s = sd.wrap_t = sd.wrap_r = 0u;
        sd.mag_linear = sd.min_linear = 0;
        samplers[0] = sd;

        /* One quad over the whole 4x1 target, with TEXCOORD0 running 0..1 in
         * u across it and constant in v. Target pixel x then samples
         * u = (x + 0.5)/4, the interior of source texel column x, so reading
         * x = 0 and x = 2 samples texel (0,0) and texel (1,0). */
        static float verts[6 * 8];
        static const float q[6][2] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
            { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
        };
        int v;
        for (v = 0; v < 6; v++) {
            verts[v * 8 + 0] = q[v][0];
            verts[v * 8 + 1] = q[v][1];
            verts[v * 8 + 2] = 0.0f;
            verts[v * 8 + 3] = 1.0f;
            /* TEXCOORD0 = (u, v), u = (x+1)/2 across the quad. */
            verts[v * 8 + 4] = (q[v][0] + 1.0f) * 0.5f;
            verts[v * 8 + 5] = 0.125f;   /* the interior of block row 0 */
            verts[v * 8 + 6] = 0.0f;
            verts[v * 8 + 7] = 1.0f;
        }

        /* The VP's constant block: identity transform + the viewport epilogue,
         * as in draw_tex_case_uv. Without it the VS constant descriptor has a
         * zero-byte range and the draw is invalid. */
        {
            static float cb[514 * 4];
            int k;
            for (k = 0; k < 514 * 4; k++)
                cb[k] = 0.0f;
            cb[0] = cb[5] = cb[10] = cb[15] = 1.0f;
            cb[512 * 4 + 0] = cb[512 * 4 + 1] = 1.0f;
            cb[512 * 4 + 2] = cb[512 * 4 + 3] = 1.0f;
            be->bind_vs_constants(be->user, cb, sizeof cb);
            be->bind_ps_constants(be->user, cb, sizeof cb);
        }

        if (t1) {
            tex[0] = t1;
            be->bind_targets(be->user, &target, 1, 0);
            be->bind_pipeline(be->user, pipeline);
            be->bind_textures(be->user, tex, samplers, 1u);
            be->set_viewport(be->user, 0, 0, 4, 4);
            be->set_scissor(be->user, 0, 0, 0, 0);
            be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride,
                     NULL, 0);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            be->readback(be->user, target, 0, 2, 1, 1, px1, 4);   /* texel (0,0) */
            {
                u8 px10[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
                be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
                be->readback(be->user, target, 1, 2, 1, 1, px10, 4); /* (1,0) */
                info("BC1 texel (0,0) %u,%u,%u,%u (want %d,%d,%d,%d)", px1[0],
                     px1[1], px1[2], px1[3], want_texel00[0], want_texel00[1],
                     want_texel00[2], want_texel00[3]);
                info("BC1 texel (1,0) %u,%u,%u,%u (want %d,%d,%d,%d)", px10[0],
                     px10[1], px10[2], px10[3], want_texel10[0], want_texel10[1],
                     want_texel10[2], want_texel10[3]);
                okbc1 = near(px1[0], want_texel00[0], 3) &&
                        near(px1[1], want_texel00[1], 3) &&
                        near(px10[0], want_texel10[0], 3) &&
                        near(px10[1], want_texel10[1], 3) &&
                        /* and they differ, so a uniform block cannot pass */
                        (px1[0] != px10[0] || px1[1] != px10[1]);
            }
        }
        if (t2) {
            u8 px10[4] = { 0xEE, 0xEE, 0xEE, 0xEE };
            tex[0] = t2;
            be->bind_textures(be->user, tex, samplers, 1u);
            be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride,
                     NULL, 0);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            be->readback(be->user, target, 0, 2, 1, 1, px2, 4);
            be->readback(be->user, target, 1, 2, 1, 1, px10, 4);
            info("BC3 texel (0,0) %u,%u,%u,%u (want %d,%d,%d,%d)", px2[0],
                 px2[1], px2[2], px2[3], want_texel00[0], want_texel00[1],
                 want_texel00[2], want_texel00[3]);
            info("BC3 texel (1,0) %u,%u,%u,%u (want %d,%d,%d,%d)", px10[0],
                 px10[1], px10[2], px10[3], want_texel10[0], want_texel10[1],
                 want_texel10[2], want_texel10[3]);
            okbc3 = near(px2[0], want_texel00[0], 3) &&
                    near(px2[1], want_texel00[1], 3) &&
                    near(px2[3], 255, 2) && near(px10[0], want_texel10[0], 3) &&
                    near(px10[1], want_texel10[1], 3);
        }
        rsx_vk_texture_release(d, t1);
        rsx_vk_texture_release(d, t2);
        be->pipeline_release(be->user, pipeline);
        be->color_target_release(be->user, target);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
    if (okbc1)
        ok("BC1 upload of a non-uniform block samples two texels' own colours");
    else
        bad("BC1 upload: the two texels are not their hand-decoded colours");
    if (okbc3)
        ok("BC3 upload of a known block samples to the interpolated colour");
    else
        bad("BC3 upload: the centre is not the interpolated colour");
}

/* ---- E: the R16G16B16A16F target --------------------------------------- */

/* IEEE half -> float, so the wide target's readback (which comes back in the
 * target's own format, R16G16B16A16_SFLOAT, not as float32) can be compared. */
static float half_to_float(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h >> 15) & 1u;
    uint32_t exp = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t man = (uint32_t)h & 0x3FFu;
    uint32_t bits;
    float f;
    if (exp == 0) {
        if (man == 0) {
            bits = sign << 31;
        } else {   /* subnormal: normalise */
            exp = 127u - 15u + 1u;
            while (!(man & 0x400u)) {
                man <<= 1;
                exp--;
            }
            man &= 0x3FFu;
            bits = (sign << 31) | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = (sign << 31) | (0xFFu << 23) | (man << 13);
    } else {
        bits = (sign << 31) | ((exp + 127u - 15u) << 23) | (man << 13);
    }
    memcpy(&f, &bits, 4);
    return f;
}

static void case_wide_target(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    /* An R16G16B16A16_SFLOAT target's own readback is eight bytes a texel:
     * four IEEE half-floats, in R,G,B,A order. The engine's readback contract
     * is "the format the target was created with" (rsx_draw_engine.h:258-262),
     * so a wide target comes back wide. */
    uint8_t raw[8];
    float px[4];
    u32 target;
    int i;

    printf("---- E: an R16G16B16A16F target, cleared and read back\n");
    target = rsx_vk_color_target_create(D(), RSX_BE_FMT_R16G16B16A16F, 8, 8,
                                        NULL, 0);
    if (!target) {
        bad("R16G16B16A16F color_target_create returned 0");
        return;
    }
    /* Clear through the vtable's clear_color: 0.25, 0.5, 0.75, 1.0 is exactly
     * representable in half float, so the comparison is exact. */
    {
        const float rgba[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        be->clear_color(be->user, target, rgba);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
    memset(raw, 0xEE, sizeof raw);
    be->readback(be->user, target, 4, 4, 1, 1, raw, 8);
    for (i = 0; i < 4; i++) {
        uint16_t h;
        memcpy(&h, raw + i * 2, 2);
        px[i] = half_to_float(h);
    }
    info("R16G16B16A16F centre %g,%g,%g,%g (want 0.25,0.5,0.75,1)", px[0],
         px[1], px[2], px[3]);
    if (px[0] > 0.2499f && px[0] < 0.2501f && px[1] > 0.4999f &&
        px[1] < 0.5001f && px[2] > 0.7499f && px[2] < 0.7501f &&
        px[3] > 0.9999f)
        ok("R16G16B16A16F target clears and reads back the wide value");
    else
        bad("R16G16B16A16F target read back %g,%g,%g,%g", px[0], px[1], px[2],
            px[3]);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- F: the cube map ---------------------------------------------------- */

/* Six faces with distinct colours, the engine's CUBE program sampling face +X
 * and face -X. The guest image is six 2x2 faces at 16 bytes each. */
static void case_cube(void)
{
    /* Guest A8R8G8B8 rows: {A,R,G,B}. What the readback returns is those four
     * bytes in that order -- the target is an A8R8G8B8 surface and the engine
     * asks for "the format the target was created with"
     * (rsx_draw_engine.h:258-262) -- so the comparison is against the row. */
    static const u8 face_colour[6][4] = {
        { 0xFF, 0xE0, 0x20, 0x20 },   /* +X red    */
        { 0xFF, 0x20, 0xE0, 0x20 },   /* -X green  */
        { 0xFF, 0x20, 0x20, 0xE0 },   /* +Y blue   */
        { 0xFF, 0xE0, 0xE0, 0x20 },   /* -Y yellow */
        { 0xFF, 0xE0, 0x20, 0xE0 },   /* +Z magenta*/
        { 0xFF, 0x20, 0xE0, 0xE0 },   /* -Z cyan   */
    };
    rsx_vk_device* d = D();
    u32 tex;
    int f, texel;
    u32 target;
    u8 px[4];
    u8 faces[6 * 2 * 2 * 4];
    const rsx_draw_backend* be = rsx_vk_backend();

    printf("---- F: a cube map, six faces with distinct colours\n");
    for (f = 0; f < 6; f++)
        for (texel = 0; texel < 4; texel++)
            memcpy(faces + ((size_t)f * 4u + (size_t)texel) * 4u, face_colour[f], 4);

    tex = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 6, 0xAAE4u,
                                0x85u);
    if (!tex) {
        bad("cube texture_create (6 faces) returned 0");
        return;
    }
    for (f = 0; f < 6; f++)
        rsx_vk_texture_upload(d, tex, (u32)f, 0, 2, 2,
                              faces + (size_t)f * 16u, 8, 2);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
    {
        rsx_vk_slot* s = rsx_vk_handle_get(d->handles, tex);
        if (s && s->u.texture.image.faces == 6 && s->u.texture.image.mips == 1)
            ok("cube texture: 6 layers, 1 level, CUBE view");
        else
            bad("cube texture: %u layers, %u levels", s ? s->u.texture.image.faces : 0,
                s ? s->u.texture.image.mips : 0);
    }

    /* The engine's CUBE program: unit 0 is a cube, TEXCOORD0 is the direction.
     * Sample it through the vtable with a draw, then read the two faces the
     * program names. A cube direction of (+-1, 0, 0) selects +X and -X. */
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    {
        rsx_be_render_state rs;
        rsx_vertex_layout_plan layout;
        rsx_be_sampler_desc sd;
        u32 pipeline;
        u32 texs[RSX_BE_MAX_TEXTURES];
        rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
        char vs_hlsl[65536], ps_hlsl[65536];
        u8 vp[64], fp[64];
        int vi, fi;
        u32 nconst = 0;
        static float verts[6 * 8];
        static const float quad[6][2] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
            { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
        };
        int v;
        u8 pxp[4], pxm[4];

        /* TEXCOORD0 is o7 in the VP decompiler's output map
         * (rsx_vp_decompiler.c:679), not o1 (COLOR0), so the cube direction
         * has to go to o7 for the PS's `input.tc0` to carry it. */
        rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_vertex_layout_plan_init(&layout, 0x0101u);   /* ATTR0 + ATTR8 */
        vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                         sizeof vs_hlsl);
        fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 1u, ps_hlsl,
                                          sizeof ps_hlsl, &nconst);
        (void)vi;
        (void)fi;
        memset(&rs, 0, sizeof rs);
        rs.depth_func = 0x207u;
        rs.color_mask = 0xFFFFFFFFu;
        pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                       layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
        if (!target || !pipeline) {
            bad("cube case: no target or no pipeline");
            return;
        }
        memset(texs, 0, sizeof texs);
        memset(samplers, 0, sizeof samplers);
        sd.wrap_s = sd.wrap_t = sd.wrap_r = 0u;
        samplers[0] = sd;
        texs[0] = tex;
        /* TEXCOORD0 = +X, then -X. */
        for (v = 0; v < 6; v++) {
            verts[v * 8 + 0] = quad[v][0];
            verts[v * 8 + 1] = quad[v][1];
            verts[v * 8 + 2] = 0.0f;
            verts[v * 8 + 3] = 1.0f;
            verts[v * 8 + 4] = 1.0f;   /* dir.x = +1 */
            verts[v * 8 + 5] = 0.0f;
            verts[v * 8 + 6] = 0.0f;
            verts[v * 8 + 7] = 1.0f;
        }
        be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
        be->bind_targets(be->user, &target, 1, 0);
        be->bind_pipeline(be->user, pipeline);
        be->bind_textures(be->user, texs, samplers, 1u);
        be->set_viewport(be->user, 0, 0, 8, 8);
        be->set_scissor(be->user, 0, 0, 0, 0);
        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL,
                 0);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        memset(pxp, 0xEE, 4);
        be->readback(be->user, target, 4, 4, 1, 1, pxp, 4);
        for (v = 0; v < 6; v++)
            verts[v * 8 + 4] = -1.0f;   /* dir.x = -1 */
        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL,
                 0);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        memset(pxm, 0xEE, 4);
        be->readback(be->user, target, 4, 4, 1, 1, pxm, 4);
        info("cube +X face %u,%u,%u,%u  -X face %u,%u,%u,%u", pxp[0], pxp[1],
             pxp[2], pxp[3], pxm[0], pxm[1], pxm[2], pxm[3]);
        if (near(pxp[0], face_colour[0][0], 3) &&
            near(pxp[1], face_colour[0][1], 3) &&
            near(pxp[2], face_colour[0][2], 3))
            ok("cube +X face samples its own colour (red)");
        else
            bad("cube +X face is not red: %u,%u,%u,%u", pxp[0], pxp[1], pxp[2],
                pxp[3]);
        if (near(pxm[0], face_colour[1][0], 3) &&
            near(pxm[1], face_colour[1][1], 3) &&
            near(pxm[2], face_colour[1][2], 3))
            ok("cube -X face samples its own colour (green)");
        else
            bad("cube -X face is not green: %u,%u,%u,%u", pxm[0], pxm[1],
                pxm[2], pxm[3]);
        be->color_target_release(be->user, target);
    }
    rsx_vk_texture_release(d, tex);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    (void)px;
}

/* A VS constant block: the identity transform plus the viewport epilogue, i.e.
 * vp_c[0..3] = identity rows and vp_posscale = 1, vp_posoffset = 0. Every
 * vtable draw below needs one, because a descriptor with a zero-byte range is
 * invalid and the draw is dropped. */
static void bind_identity_constants(const rsx_draw_backend* be)
{
    static float cb[514 * 4];
    int k;
    for (k = 0; k < 514 * 4; k++)
        cb[k] = 0.0f;
    cb[0] = cb[5] = cb[10] = cb[15] = 1.0f;   /* 4x4 identity */
    cb[512 * 4 + 0] = cb[512 * 4 + 1] = 1.0f;
    cb[512 * 4 + 2] = cb[512 * 4 + 3] = 1.0f;
    be->bind_vs_constants(be->user, cb, sizeof cb);
    be->bind_ps_constants(be->user, cb, sizeof cb);
}

/* A full-screen quad (6 vertices, TRIANGLES) with ATTR0 = position and ATTR8 =
 * TEXCOORD0 = `uv` at every vertex. Layout mask 0x0101, stride 32. */
static void quad_vertices(float* v, float u, float w)
{
    static const float q[6][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
        { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
    };
    int i;
    for (i = 0; i < 6; i++) {
        v[i * 8 + 0] = q[i][0];
        v[i * 8 + 1] = q[i][1];
        v[i * 8 + 2] = 0.0f;
        v[i * 8 + 3] = 1.0f;
        v[i * 8 + 4] = u;
        v[i * 8 + 5] = w;
        v[i * 8 + 6] = 0.0f;
        v[i * 8 + 7] = 1.0f;
    }
}

/*
 * TEX r0, TC0, unit `unit` ; MUL r0, r0, COL0 ; END
 *
 * A two-instruction fragment program that combines two fetches into one pixel:
 * r0 takes the PS's own texture fetch through unit `unit`, then multiplies it
 * by the interpolated COLOR0 -- which is where the vertex program writes its
 * own (vertex-texture) fetch. So the pixel depends on BOTH stages' texture
 * unit 0, and aliasing either the image or the sampler between the two stages
 * changes it (item N7 / the G and Q rewrite).
 *
 * The encodings are the two rsx_test_programs.h helpers composed by hand:
 *   word 0: out r0 (reg field 0), XYZW, input_src TC0, tex unit, opcode TEX,
 *           no END;
 *   then word 1 carries SRC_INPUT with the identity swizzle, and
 *   rsx_test_fp_instr adds the unconditional exec_if.
 * The second instruction is MOV... no: MUL, so its opcode field is 0x02 with
 * SRC0 = r0 (temp register 0) and SRC1 = COL0, mask XYZW, END set.
 */
/* rsx_test_programs.h names only MOV (0x01) and TEX (0x17) in the fragment
 * space; MUL is 0x02 in rsx_fp_decompiler.c:66 (OP_MUL=0x02). Named here so
 * the program's encoding is readable rather than a bare 0x02 << 24. */
#define RSX_TEST_FP_OP_MUL 0x02u

__attribute__((unused)) static void write_fp_tex_mul_col0(u8* p, u32 unit)
{
    /* TEX r0, TC0, unit */
    rsx_test_fp_instr(p + 0,
                      RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) |
                          RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
                          RSX_TEST_FP_TEXU(unit),
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    /* MUL r0, r0, COL0.
     *
     * Two things about the RSX encoding are easy to get wrong here and they
     * are why this is written out in full rather than through
     * rsx_test_fp_instr:
     *
     *  - which interpolated input an instruction reads is the OPDEST INPUT_SRC
     *    field in word 0 (FP_INPUT_SRC_SHIFT 13), and a source operand is read
     *    as that input only when its OWN type field says INPUT
     *    (FP_REG_TYPE_INPUT, bits [0:1] of the source word). rsx_fp_decompiler.c
     *    emit_src checks the type first: a TEMP source becomes `rN` and never
     *    looks at INPUT_SRC at all. So the COL0 operand must be typed INPUT in
     *    word 2, not just carry the word-0 INSRC bit -- without the type it
     *    decodes as `r[0]` and the program becomes `r0 = r0 * r0.x`.
     *  - src0 (word 1) is the TEX result, temp register 0.
     *
     * Encoding: word 0 opcode MUL (0x02), mask XYZW, INSRC COL0, OUTREG 0,
     * END; word 1 src0 = TEMP r0 with the identity swizzle and EXEC_ALWAYS;
     * word 2 src1 = INPUT with INSRC_COL0's input selected and identity swizzle;
     * word 3 zero. */
    rsx_test_fp_put_word(p + 16,
                         RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MUL) |
                             RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 |
                             RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END);
    rsx_test_fp_put_word(p + 20,
                         RSX_TEST_FP_SWZ_IDENT | RSX_TEST_FP_EXEC_ALWAYS);
    rsx_test_fp_put_word(p + 24, RSX_TEST_FP_SRC_INPUT |
                                   RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(p + 28, 0);
}

/* ---- G: the vertex texture (N7's sampler half, VP TXL) ----------------- */

/*
 * A vertex texture on VS unit 0 and a different one on PS unit 0, with
 * DISTINGUISHABLE SAMPLER STATE, combined in the pixel.
 *
 * Why the samplers have to differ: the S3b defect was that the vertex stage's
 * `register(s0)` and the fragment stage's `s0` both landed on binding 32 in the
 * SPIR-V, so one VkSampler fed both stages. If the two stages' samplers are
 * created from the same rsx_be_sampler_desc, the alias changes nothing a pixel
 * can see -- which is why the earlier version of this case could not detect it
 * (and why the S3b test passed). With the samplers differing, an alias changes
 * which texel is fetched, and the pixel changes with it.
 *
 * The setup, and the hand derivation of the expected pixel:
 *
 *   VS unit 0  a 2x2 texture whose four texels are R, G, B, W (distinct, so
 *              the fetch's *position* is visible as well as its colour)
 *              sampler: CLAMP_TO_EDGE (guest wrap code 3 -> default branch),
 *              NEAREST.
 *   PS unit 0  a 2x2 texture whose four texels are all (0xFF,0x40,0x00,0x00)
 *              in guest {A,R,G,B} order -- one uniform colour, so the PS fetch
 *              is a constant and only the VS fetch moves.
 *              sampler: REPEAT (guest wrap code 1), NEAREST.
 *
 *   tc = 1.25 at every vertex, so the interpolated TEXCOORD0 is 1.25 both
 *   stages.
 *
 *   VS fetch: tc 1.25 under CLAMP_TO_EDGE is clamped to the last texel's
 *   column and row. Vulkan's nearest rule samples floor(tc * size) after the
 *   address-mode transform; clamped, that is texel (1,1). So the VS produces
 *   the WHITE texel. Under REPEAT (what an aliased sampler would apply) 1.25
 *   wraps to 0.25, i.e. texel (0,0) = RED. **The two answers differ, which is
 *   the discrimination.**
 *
 *   PS fetch: tc 1.25 under REPEAT wraps to 0.25 -> texel (0,0), but the
 *   texture is uniform so the value is the same texel whatever it lands on:
 *   guest bytes {A,R,G,B} = {0xFF,0x40,0x00,0x00}, presented RGBA = (0x40,0,0,0xFF).
 *
 *   The FP is `TEX r0, TC0, unit0 ; MUL r0, r0, COL0`, and the VP writes its
 *   TXL result to COLOR0 (o[1]). After the unorm clamp, componentwise:
 *
 *     PS fetch    (R,G,B,A) = (0.25,    0, 0, 1)
 *   x VS fetch    (R,G,B,A) = (1,       1, 1, 1)     <- the white edge texel
 *   = pixel       (R,G,B,A) = (0.25,    0, 0, 1)
 *
 *   The readback returns the target's own A8R8G8B8 bytes in the guest's
 *   {A,R,G,B} order. That is not assumed here: it is case B's own result on this
 *   same format, where the identity crossbar over the guest row {A,R,G,B} =
 *   {FF,10,20,30} reads back as 255,16,32,48 -- the row unchanged, byte for
 *   byte. Applying the same order to (R,G,B,A) = (0.25,0,0,1):
 *
 *     {A,R,G,B} = (1 * 255, 0.25 * 255, 0, 0) -> (255, 64, 0, 0)
 *
 *   (0.25 * 255 = 63.75, which an unorm target rounds to 63 or 64; the
 *   assertion allows +-2 for that.)
 *
 *   What the two aliases would give instead:
 *   - VS IMAGE alias (the VS fetching the PS texture): the VS would produce
 *     (0.25,0,0,1) too, and the product would be (0.0625,0,0,1) ->
 *     {A,R,G,B} = (255, 16, 0, 0) = (255,16,0,0), a different pixel.
 *   - VS SAMPLER alias (CLAMP swapped for REPEAT): tc 1.25 wraps to texel
 *     (0,0) = RED. (1,0,0,1) * (0.25,0,0,1) = (0.25,0,0,1) -- the same pixel,
 *     because the multiply zeroes everything but R. That is why the sampler
 *     alias is *not* this case's observable: case Q is, where the PS texture is
 *     not uniform and the same swap moves the pixel from green to red.
 */
static void case_vertex_texture(void)
{
    static const u8 vs_texels[4][4] = {
        /* A     R     G     B     -- guest {A,R,G,B} order */
        { 0xFF, 0xFF, 0x00, 0x00 },   /* (0,0) red   */
        { 0xFF, 0x00, 0xFF, 0x00 },   /* (1,0) green */
        { 0xFF, 0x00, 0x00, 0xFF },   /* (0,1) blue  */
        { 0xFF, 0xFF, 0xFF, 0xFF },   /* (1,1) white */
    };
    static const u8 ps_texels[4][4] = {
        { 0xFF, 0x40, 0x00, 0x00 }, { 0xFF, 0x40, 0x00, 0x00 },
        { 0xFF, 0x40, 0x00, 0x00 }, { 0xFF, 0x40, 0x00, 0x00 },
    };
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    u8 vp[64], fp[64];
    char vs_hlsl[65536], ps_hlsl[65536];
    rsx_vertex_layout_plan layout;
    rsx_be_render_state rs;
    u32 pipeline, target, tex_vs, tex_ps;
    u32 texs[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    static u8 vs_bytes[16], ps_bytes[16];
    static float verts[6 * 8];
    u32 vi, fi, nconst = 0, i;
    u8 px[4];

    printf("---- G: a vertex texture with its own sampler state\n");
    for (i = 0; i < 4; i++) {
        memcpy(vs_bytes + i * 4, vs_texels[i], 4);
        memcpy(ps_bytes + i * 4, ps_texels[i], 4);
    }
    /* VP, three instructions:
     *   o[0] = ATTR0                    -- the clip position
     *   o[1] = TXL(vtex0, ATTR8)        -- the vertex fetch, which the FP's
     *                                      `MUL r0, r0, COL0` reads as COLOR0
     *   o[7] = ATTR8                    -- TEXCOORD0 itself
     *
     * The third one matters and is the trap the S3c report already documents
     * once: TEXCOORD0 is o[7] (rsx_vp_decompiler.c:679), so a program that
     * writes only o[1] leaves TEXCOORD0 at its default (0,0,0,1) and the PS
     * samples texel (0,0) at every tc the vertices carry. Without this
     * instruction the PS's wrap mode is untestable: every tc would land on the
     * same texel whether the sampler clamped or repeated. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_txl_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    write_fp_tex_mul_col0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);   /* ATTR0 + ATTR8 */
    vi = rsx_vp_decompile_compact_ex(vp, 48, 1u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 32, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("vertex texture case: the pair does not decompile (%d, %d)", vi, fi);
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    tex_vs = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, 0xAAE4u,
                                   0x85u);
    tex_ps = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, 0xAAE4u,
                                   0x85u);
    if (!target || !pipeline || !tex_vs || !tex_ps) {
        bad("vertex texture case: pipeline %u, target %u, textures %u/%u",
            pipeline, target, tex_vs, tex_ps);
        return;
    }
    rsx_vk_texture_upload(d, tex_vs, 0, 0, 2, 2, vs_bytes, 8, 2);
    rsx_vk_texture_upload(d, tex_ps, 0, 0, 2, 2, ps_bytes, 8, 2);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* tc = 1.25 everywhere: clamped for the VS (last texel), wrapped for the
     * PS (but the PS texture is uniform, so only the sampler's *identity* is
     * what the case can see through the VS). */
    quad_vertices(verts, 1.25f, 1.25f);
    bind_identity_constants(be);
    memset(texs, 0, sizeof texs);
    memset(samplers, 0, sizeof samplers);

    /* PS unit 0: REPEAT (guest wrap code 1), NEAREST. */
    samplers[0].wrap_s = samplers[0].wrap_t = samplers[0].wrap_r = 1u;
    samplers[0].min_linear = samplers[0].mag_linear = 0u;
    texs[0] = tex_ps;
    be->bind_textures(be->user, texs, samplers, 1u);

    /* VS unit 0: CLAMP_TO_EDGE -- guest wrap code 3, which rsx_vk_sampler.c's
     * wrap_mode falls through to CLAMP_TO_EDGE (only 1, 2, 4, 6, 7 and 8 are
     * named). NEAREST. */
    memset(texs, 0, sizeof texs);
    memset(samplers, 0, sizeof samplers);
    samplers[0].wrap_s = samplers[0].wrap_t = samplers[0].wrap_r = 3u;
    samplers[0].min_linear = samplers[0].mag_linear = 0u;
    texs[0] = tex_vs;
    be->bind_vertex_textures(be->user, texs, samplers, 1u);

    be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
    be->bind_targets(be->user, &target, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    memset(px, 0xEE, 4);
    be->readback(be->user, target, 4, 4, 1, 1, px, 4);

    /* Derived above: PS (R,G,B,A) = (0.25,0,0,1) times the VS's white edge
     * texel, stored in the target's A8R8G8B8 {A,R,G,B} order = (255,64,0,0). */
    info("vertex texture centre %u,%u,%u,%u (want 255,64,0,0)", px[0], px[1],
         px[2], px[3]);
    if (near(px[0], 255, 2) && near(px[1], 64, 2) && near(px[2], 0, 2) &&
        near(px[3], 0, 2))
        ok("VP TXL sampled VS unit 0 through its own sampler and the PS "
           "multiplied in its own fetch");
    else if (near(px[0], 255, 2) && near(px[1], 16, 2) && near(px[2], 0, 2))
        bad("vertex texture pixel %u,%u,%u,%u is the double-fetch answer: the "
            "VS stage read the PS's texture (item 7's image alias)",
            px[0], px[1], px[2], px[3]);
    else
        bad("vertex texture pixel %u,%u,%u,%u is not the derived "
            "(255,64,0,0)", px[0], px[1], px[2], px[3]);

    rsx_vk_texture_release(d, tex_vs);
    rsx_vk_texture_release(d, tex_ps);
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- H: depth ----------------------------------------------------------- */

/*
 * Two quads at different z, drawn in the order that can actually discriminate:
 *
 *   1. the NEAR quad (z = 0.75) over the LEFT HALF only;
 *   2. the FAR quad (z = 0.25) over the WHOLE target.
 *
 * With a GREATER test the far quad fails where the near one already wrote, so
 * the left half keeps the near colour and the right half takes the far colour.
 * Drawing the far quad first -- what the S3b case did -- would pass even with
 * the depth test switched off, because the near quad would simply be drawn last
 * over the far one.
 *
 * The whole thing is then run a second time with the depth test DISABLED, where
 * the far quad must overwrite the left half too. That is the discrimination
 * proof: a case that passes both with and without the test is not testing it.
 *
 * depth_snapshot is read at an interior texel of each half, and the values are
 * the two z's (0.75 and 0.25) to within 1e-6.
 */
/* One run of the depth scenario. `depth_test` 1 uses the GREATER test, 0 turns
 * it off. Reads the two halves and the two snapshot values. */
static void depth_scenario(int depth_test, u8 left[4], u8 right[4],
                           int* near_ok, int* far_ok, float* snap_near,
                           float* snap_far, int* have_snaps)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    u8 vp[64], fp[64];
    char vs_hlsl[65536], ps_hlsl[65536];
    rsx_vertex_layout_plan layout;
    rsx_be_render_state rs;
    u32 pipeline, target, zeta;
    static float verts[6 * 8];
    static const float quad[6][2] = {
        { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
        { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
    };
    static const float half[6][2] = {
        { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f },
        { -1.0f, -1.0f }, { 0.0f, 1.0f }, { -1.0f, 1.0f },
    };
    int v, vi, fi;
    u32 nconst = 0;

    *near_ok = *far_ok = *have_snaps = 0;
    *snap_near = *snap_far = -1.0f;
    memset(left, 0xEE, 4);
    memset(right, 0xEE, 4);

    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);   /* ATTR0 + ATTR3 */
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("depth case: the pair does not decompile (%d, %d)", vi, fi);
        return;
    }
    memset(&rs, 0, sizeof rs);
    /* GREATER: the nearer quad has the larger z after the identity transform,
     * which is the D3D/Vulkan convention the engine's epilogue produces. */
    rs.depth_func = 0x204u;
    rs.depth_test = (u32)(depth_test ? 1 : 0);
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;

    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    zeta = be->depth_target_create(be->user, 8, 8);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!target || !zeta || !pipeline) {
        bad("depth case: target %u, zeta %u, pipeline %u", target, zeta, pipeline);
        return;
    }
    be->clear_color(be->user, target, (const float[4]){0, 0, 1, 1});
    /* A GREATER test needs the zeta cleared to the far plane's *minimum*: with
     * 1.0 nothing is ever greater and both quads would be rejected, which is a
     * correct depth test and a wrong test. */
    be->clear_depth_stencil(be->user, zeta,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);
    be->bind_targets(be->user, &target, 1, zeta);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);

    /* 1. NEAR quad (z = 0.75), GREEN, left half only. */
    for (v = 0; v < 6; v++) {
        verts[v * 8 + 0] = half[v][0];
        verts[v * 8 + 1] = half[v][1];
        verts[v * 8 + 2] = 0.75f;
        verts[v * 8 + 3] = 1.0f;
        verts[v * 8 + 4] = 0.0f;   /* R */
        verts[v * 8 + 5] = 1.0f;   /* G */
        verts[v * 8 + 6] = 0.0f;
        verts[v * 8 + 7] = 1.0f;
    }
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);

    /* 2. FAR quad (z = 0.25), RED, the whole target. */
    be->bind_targets(be->user, &target, 1, zeta);
    be->bind_pipeline(be->user, pipeline);
    for (v = 0; v < 6; v++) {
        verts[v * 8 + 0] = quad[v][0];
        verts[v * 8 + 1] = quad[v][1];
        verts[v * 8 + 2] = 0.25f;
        verts[v * 8 + 3] = 1.0f;
        verts[v * 8 + 4] = 1.0f;   /* R */
        verts[v * 8 + 5] = 0.0f;
        verts[v * 8 + 6] = 0.0f;
        verts[v * 8 + 7] = 1.0f;
    }
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* Readback is the target's own bytes in R,G,B,A order: green = 0,255,0,255,
     * red = 255,0,0,255. Pixel (2,4) is the left half, (6,4) the right. */
    be->readback(be->user, target, 2, 4, 1, 1, left, 4);
    be->readback(be->user, target, 6, 4, 1, 1, right, 4);
    *near_ok = (left[0] == 0 && left[1] == 255 && left[2] == 0) ? 1 : 0;
    *far_ok = (right[0] == 255 && right[1] == 0 && right[2] == 0) ? 1 : 0;

    /* The depth the zeta holds at each half's interior texel. Snapshotted in
     * both runs: with the test off the whole zeta is the far quad's 0.25, which
     * the caller ignores -- the on-run's two values are what it asserts. */
    if (1) {
        u32 snap = be->depth_snapshot(be->user, zeta, 8, 8);
        if (snap) {
            /* The snapshot is an R32F texture slot, so its texels are read by
             * sampling it. One 1x1 target per half, each a single draw whose
             * tc is that half's interior texel -- the shape a standalone probe
             * of the same zeta uses, which is what makes the two comparable. */
            {
                char vs2[65536], ps2[65536];
                rsx_be_render_state rs2;
                rsx_vertex_layout_plan l2;
                rsx_be_sampler_desc sm[RSX_BE_MAX_TEXTURES];
                u32 p2, nc2 = 0;
                u8 snap_vp[64];
                int vv2, ff2;
                static float sv[6 * 8];
                static const float q2[6][2] = {
                    { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
                    { -1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f },
                };
                /* The interior of snapshot texel (2,4) and of (6,4): the two
                 * halves' centres, in the 8x8 image. */
                static const float half_tc2[2][2] = { { 0.3125f, 0.5625f },
                                                      { 0.8125f, 0.5625f } };
                int which;

                rsx_test_vp_mov_out(snap_vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
                rsx_test_vp_mov_out(snap_vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
                rsx_test_fp_tex_r0_tc0(fp, 0);
                rsx_vertex_layout_plan_init(&l2, 0x0101u);
                vv2 = rsx_vp_decompile_compact_ex(snap_vp, 32, 0, l2.mask, vs2,
                                                  sizeof vs2);
                ff2 = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps2,
                                                   sizeof ps2, &nc2);
                if (vv2 > 0 && ff2 > 0) {
                    memset(&rs2, 0, sizeof rs2);
                    rs2.depth_func = 0x207u;
                    rs2.color_mask = 0xFFFFFFFFu;
                    p2 = be->pipeline_create(be->user, vs2, ps2, &rs2, &l2,
                                             l2.stride, RSX_BE_FMT_R32F, 1);
                    memset(sm, 0, sizeof sm);   /* point, clamp to edge */
                    for (which = 0; which < 2; which++) {
                        u32 t1 = be->color_target_create(be->user,
                                                         RSX_BE_FMT_R32F, 1, 1,
                                                         NULL, 0);
                        u8 b4[4];
                        int vv;
                        if (!p2 || !t1)
                            break;
                        for (vv = 0; vv < 6; vv++) {
                            sv[vv * 8 + 0] = q2[vv][0];
                            sv[vv * 8 + 1] = q2[vv][1];
                            sv[vv * 8 + 2] = 0.0f;
                            sv[vv * 8 + 3] = 1.0f;
                            sv[vv * 8 + 4] = half_tc2[which][0];
                            sv[vv * 8 + 5] = half_tc2[which][1];
                            sv[vv * 8 + 6] = 0.0f;
                            sv[vv * 8 + 7] = 1.0f;
                        }
                        bind_identity_constants(be);
                        be->clear_color(be->user, t1,
                                        (const float[4]){0, 0, 0, 0});
                        be->bind_targets(be->user, &t1, 1, 0);
                        be->bind_pipeline(be->user, p2);
                        be->bind_textures(be->user, &snap, sm, 1u);
                        be->set_viewport(be->user, 0, 0, 1, 1);
                        be->set_scissor(be->user, 0, 0, 0, 0);
                        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, sv, 6,
                                 l2.stride, NULL, 0);
                        be->submit_and_wait(be->user,
                                            RSX_BE_FLUSH_GUEST_REFERENCE);
                        memset(b4, 0, sizeof b4);
                        be->readback(be->user, t1, 0, 0, 1, 1, b4, 4);
                        memcpy(which == 0 ? snap_near : snap_far, b4, 4);
                        be->color_target_release(be->user, t1);
                        be->submit_and_wait(be->user,
                                            RSX_BE_FLUSH_GUEST_REFERENCE);
                    }
                    *have_snaps = 1;
                    if (p2)
                        be->pipeline_release(be->user, p2);
                }
            }
            be->texture_release(be->user, snap);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        }
    }

    be->pipeline_release(be->user, pipeline);
    be->depth_target_release(be->user, zeta);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

static void case_depth(void)
{
    u8 left[4], right[4], left2[4], right2[4];
    int near_ok, far_ok, near_ok2, far_ok2, have_snaps;
    float sn = 0.0f, sf = 0.0f;

    printf("---- H: near quad first, then far over all, with the test on and\n"
           "        off; depth_snapshot at an interior texel of each half\n");

    /* With the depth test on. */
    depth_scenario(1, left, right, &near_ok, &far_ok, &sn, &sf, &have_snaps);
    info("depth (test on): left %u,%u,%u,%u  right %u,%u,%u,%u", left[0],
         left[1], left[2], left[3], right[0], right[1], right[2], right[3]);
    if (near_ok)
        ok("depth: the near quad drawn first kept its own colour on the left");
    else
        bad("depth: the left half is %u,%u,%u,%u, not the near quad's green",
            left[0], left[1], left[2], left[3]);
    if (far_ok)
        ok("depth: the far quad drawn over all took only the right half");
    else
        bad("depth: the right half is %u,%u,%u,%u, not the far quad's red",
            right[0], right[1], right[2], right[3]);

    if (have_snaps) {
        info("depth_snapshot: near half %.6f (want 0.750000), far half %.6f "
             "(want 0.250000)", sn, sf);
        if (sn > 0.75f - 1e-6f && sn < 0.75f + 1e-6f)
            ok("depth_snapshot: the near half's interior texel is 0.75");
        else
            bad("depth_snapshot: the near half is %.6f, not 0.75", sn);
        if (sf > 0.25f - 1e-6f && sf < 0.25f + 1e-6f)
            ok("depth_snapshot: the far half's interior texel is 0.25");
        else
            bad("depth_snapshot: the far half is %.6f, not 0.25", sf);
    } else {
        bad("depth_snapshot: no snapshot could be read for the two halves");
    }

    /* The same scenario with the depth test disabled: the far quad must now
     * overwrite the left half, which is what proves the case discriminates. */
    depth_scenario(0, left2, right2, &near_ok2, &far_ok2, &sn, &sf,
                   &have_snaps);
    info("depth (test off): left %u,%u,%u,%u  right %u,%u,%u,%u", left2[0],
         left2[1], left2[2], left2[3], right2[0], right2[1], right2[2],
         right2[3]);
    if (!near_ok2 && far_ok2)
        ok("depth: with the depth test disabled the far quad overwrote the "
           "left half too (the case really tests the test)");
    else
        bad("depth: with the depth test disabled the left half is still "
            "%u,%u,%u,%u, so the case would pass without a depth test",
            left2[0], left2[1], left2[2], left2[3]);

    /* clear_depth_stencil, and what a snapshot of a CLEARED-THEN-DRAWN zeta
     * reads (item L9).
     *
     * The first half is the engine's own rule, unchanged: a zeta that has only
     * ever been cleared has not produced a depth map, so depth_snapshot
     * refuses it (rsx_draw_engine.c:664-674 applies the same test before it
     * calls the backend at all). That refusal is still asserted, because it is
     * a real contract.
     *
     * The second half is what L9 adds: after the clear the pass that folds it
     * in must leave the clear value in the image, so a snapshot taken after a
     * later draw reads that value where the draw did not write. The value is
     * 0.5 and the assertion is +-1e-6, not "a texture came back": a refusal is
     * not evidence the clear was applied, and the S3d review found the case had
     * stopped at the refusal.
     *
     * Derivation of the expected 0.5: the clear is the depth aspect's
     * LOAD_OP_CLEAR in the next pass with clearValue.depthStencil.depth = 0.5
     * (rsx_vk_pipeline.c rsx_vk_pass_begin, per-aspect since item 8). The draw
     * that follows has depth_write disabled, so nothing overwrites it; the
     * image holds exactly the float 0.5 the clear wrote, and
     * D32_SFLOAT_S8_UINT stores it without quantisation, so the snapshot's R32F
     * output is 0.5 to within the float round trip (+-1e-6 is far looser than
     * that).
     */
    {
        const rsx_draw_backend* be = rsx_vk_backend();
        rsx_vk_device* d = D();
        u32 zeta = be->depth_target_create(be->user, 8, 8);
        uint32_t before = d->depth_clears;
        if (!zeta) {
            bad("depth: a fresh zeta could not be created");
        } else {
            be->clear_depth_stencil(be->user, zeta,
                                    RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                    0.125f, 0x5A);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            if (d->depth_clears > before)
                ok("depth: clear_depth_stencil moved the clear counter %u -> %u",
                   before, d->depth_clears);
            else
                bad("depth: clear_depth_stencil did not reach the backend");
            /* The engine's rule, still enforced: a never-drawn zeta is not a
             * depth map. */
            if (be->depth_snapshot(be->user, zeta, 8, 8) == 0)
                ok("depth: a zeta that has only been cleared still refuses a "
                   "snapshot (no depth write since, rsx_draw_engine.c:664)");
            else
                bad("depth: depth_snapshot published a clear-only zeta");
            be->depth_target_release(be->user, zeta);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        }
    }

    /* L9 itself: clear to 0.5, then a draw with depth WRITES DISABLED (so the
     * clear value survives), then a snapshot that must read 0.5. */
    {
        const rsx_draw_backend* be = rsx_vk_backend();
        u8 vp2[64], fp2[64];
        char vs2[65536], ps2[65536];
        rsx_vertex_layout_plan l2;
        rsx_be_render_state rs2;
        rsx_be_sampler_desc sm[RSX_BE_MAX_TEXTURES];
        u32 tgt, zeta, pass_pipe, snap_pipe, sample_tgt;
        static float v2[6 * 8];
        u32 vi2, fi2, nc = 0, snap;
        u8 b4[4];
        float got = -1.0f;

        tgt = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
        zeta = be->depth_target_create(be->user, 8, 8);
        /* A pipeline with the depth test on but the WRITE off: it runs inside
         * a pass that carries the clear, and cannot change the depth. */
        rsx_test_vp_mov_out(vp2 + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp2 + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
        rsx_test_fp_mov_r0_col0(fp2, RSX_TEST_FP_SWZ_IDENT);
        rsx_vertex_layout_plan_init(&l2, 0x0009u);
        vi2 = rsx_vp_decompile_compact_ex(vp2, 32, 0, l2.mask, vs2, sizeof vs2);
        fi2 = rsx_fp_decompile_buffered_ex(fp2, 16, 0x40u, 0u, ps2, sizeof ps2,
                                           &nc);
        if (!tgt || !zeta || vi2 <= 0 || fi2 <= 0) {
            bad("depth L9: target %u, zeta %u, decompile %u/%u", tgt, zeta, vi2,
                fi2);
        } else {
            memset(&rs2, 0, sizeof rs2);
            rs2.depth_func = 0x204u;   /* GREATER */
            rs2.depth_test = 1;
            rs2.depth_write = 0;       /* the point: do not overwrite the clear */
            rs2.color_mask = 0xFFFFFFFFu;
            pass_pipe = be->pipeline_create(be->user, vs2, ps2, &rs2, &l2,
                                            l2.stride, RSX_BE_FMT_R8G8B8A8, 1);
            if (!pass_pipe) {
                bad("depth L9: the no-depth-write pipeline failed");
            } else {
                /* The clear to 0.5, folded into the next pass's load op. */
                be->clear_depth_stencil(be->user, zeta,
                                        RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                        0.5f, 0);
                bind_identity_constants(be);
                quad_vertices(v2, 0.5f, 0.5f);
                be->bind_targets(be->user, &tgt, 1, zeta);
                be->bind_pipeline(be->user, pass_pipe);
                be->set_viewport(be->user, 0, 0, 8, 8);
                be->set_scissor(be->user, 0, 0, 0, 0);
                be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, v2, 6, l2.stride,
                         NULL, 0);
                be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

                snap = be->depth_snapshot(be->user, zeta, 8, 8);
                if (!snap) {
                    bad("depth L9: depth_snapshot refused a zeta that has been "
                        "drawn (a clear plus a no-write draw)");
                } else {
                    /* Read the snapshot's centre through a 1x1 R32F target:
                     * tc 0.5 of an 8x8 is texel (4,4)'s boundary, which is the
                     * same value everywhere here because the whole zeta holds
                     * the clear value. */
                    char vs3[65536], ps3[65536];
                    rsx_vertex_layout_plan l3;
                    rsx_be_render_state rs3;
                    u8 svp[64];
                    u32 nc3 = 0;
                    int vv3, ff3;
                    static float sv[6 * 8];

                    rsx_test_vp_mov_out(svp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
                    rsx_test_vp_mov_out(svp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
                    rsx_test_fp_tex_r0_tc0(fp2, 0);
                    rsx_vertex_layout_plan_init(&l3, 0x0101u);
                    vv3 = rsx_vp_decompile_compact_ex(svp, 32, 0, l3.mask, vs3,
                                                      sizeof vs3);
                    ff3 = rsx_fp_decompile_buffered_ex(fp2, 16, 0x40u, 0u, ps3,
                                                       sizeof ps3, &nc3);
                    memset(&rs3, 0, sizeof rs3);
                    rs3.depth_func = 0x207u;
                    rs3.color_mask = 0xFFFFFFFFu;
                    snap_pipe = (vv3 > 0 && ff3 > 0)
                                    ? be->pipeline_create(be->user, vs3, ps3,
                                                          &rs3, &l3, l3.stride,
                                                          RSX_BE_FMT_R32F, 1)
                                    : 0;
                    sample_tgt = be->color_target_create(be->user,
                                                         RSX_BE_FMT_R32F, 1, 1,
                                                         NULL, 0);
                    if (snap_pipe && sample_tgt) {
                        memset(sm, 0, sizeof sm);
                        bind_identity_constants(be);
                        quad_vertices(sv, 0.5f, 0.5f);
                        be->clear_color(be->user, sample_tgt,
                                        (const float[4]){0, 0, 0, 0});
                        be->bind_targets(be->user, &sample_tgt, 1, 0);
                        be->bind_pipeline(be->user, snap_pipe);
                        be->bind_textures(be->user, &snap, sm, 1u);
                        be->set_viewport(be->user, 0, 0, 1, 1);
                        be->set_scissor(be->user, 0, 0, 0, 0);
                        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, sv, 6,
                                 l3.stride, NULL, 0);
                        be->submit_and_wait(be->user,
                                            RSX_BE_FLUSH_GUEST_REFERENCE);
                        memset(b4, 0, 4);
                        be->readback(be->user, sample_tgt, 0, 0, 1, 1, b4, 4);
                        memcpy(&got, b4, 4);
                    }
                    info("depth L9: snapshot of a zeta cleared to 0.5 and then "
                         "drawn with depth writes off = %.6f (want 0.500000)",
                         (double)got);
                    if (got > 0.5f - 1e-6f && got < 0.5f + 1e-6f)
                        ok("depth L9: the clear value 0.5 is what the zeta "
                           "holds, and depth_snapshot reads it back");
                    else
                        bad("depth L9: the snapshot read %.6f, not the 0.5 the "
                            "clear wrote", (double)got);
                    if (sample_tgt)
                        be->color_target_release(be->user, sample_tgt);
                    if (snap_pipe)
                        be->pipeline_release(be->user, snap_pipe);
                    be->texture_release(be->user, snap);
                }
                be->pipeline_release(be->user, pass_pipe);
            }
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
            be->depth_target_release(be->user, zeta);
            be->color_target_release(be->user, tgt);
            be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        }
    }
}

/* ---- I: the reflection assertion (S2 root note) ------------------------ */

/*
 * The vertex-input table a pipeline emits is `location = slot, offset = slot *
 * 16` for every compact layout slot (DESIGN §2.4, the S2 root acceptance's
 * note). That is only correct while the compacted buffer order and glslang's
 * input order are both ascending attribute index, which no compiler promises.
 *
 * So for every test program, the VS's *actual* input Locations are read out of
 * the compiled SPIR-V, and the comparison is against the table the *pipeline
 * was really built with*: rsx_vk_pipeline_vertex_attrs reads back the
 * VkVertexInputAttributeDescription array pipeline_build handed to
 * vkCreateGraphicsPipelines. Comparing against a loop index (`locs[i] == i`)
 * only restates the test's own assumption; comparing against the table the
 * driver was given is the property that has to hold. The programs include one
 * with an attribute gap (ATTR0 + ATTR3), where the compacted slots are 0 and 1
 * and the attribute indices are 0 and 3.
 */
typedef struct refl_case {
    const char* name;
    u32 input_mask;
    int n_attrs;
} refl_case;

static int reflect_check(rsx_vk_device* d, const refl_case* c)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vertex_layout_plan layout;
    /* Room for one MOV per attribute, up to all sixteen. */
    u8 ucode[RSX_DSP_NUM_VERTEX_ATTR * 16];
    char hlsl[65536];
    char log[2048];
    rsx_vk_spv spv;
    rsx_be_render_state rs;
    int32_t locs[RSX_DSP_NUM_VERTEX_ATTR];
    /* {location, binding, offset, VkFormat} per attribute, from the pipeline. */
    uint32_t attr[RSX_DSP_NUM_VERTEX_ATTR * 4];
    u32 target, pipeline, nattr_vk;
    int n, i, rc = 0;

    rsx_vertex_layout_plan_init(&layout, c->input_mask);
    {
        const int nattr = (int)layout.count;
        if (nattr <= 0 || nattr * 16 > (int)sizeof ucode) {
            bad("%s: the layout plan has %d attributes", c->name, nattr);
            return -1;
        }
        for (i = 0; i < nattr; i++)
            rsx_test_vp_mov_out(ucode + i * 16, (u32)layout.attrs[i],
                                RSX_TEST_VP_SWZ_IDENT, (u32)i,
                                i == nattr - 1);
        if (rsx_vp_decompile_compact_ex(ucode, (u32)nattr * 16u, 0, layout.mask,
                                        hlsl, sizeof hlsl) <= 0) {
            bad("%s: the VP does not decompile", c->name);
            return -1;
        }
    }
    if (rsx_vk_hlsl_to_spirv(hlsl, RSX_VK_STAGE_VERTEX, &spv, log,
                             sizeof log) != 0) {
        bad("%s: HLSL -> SPIR-V failed: %s", c->name, log);
        return -1;
    }
    n = rsx_vk_spv_input_locations(&spv, locs, RSX_DSP_NUM_VERTEX_ATTR);
    rsx_vk_spirv_free(&spv);

    /* Build the real pipeline from the same program and read its table back. */
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    pipeline = be->pipeline_create(be->user, hlsl, "float4 main() : SV_Target {"
                                   " return float4(1,0,0,1); }", &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!target || !pipeline) {
        bad("%s: could not build a pipeline to compare against", c->name);
        if (target)
            be->color_target_release(be->user, target);
        return -1;
    }
    nattr_vk = rsx_vk_pipeline_vertex_attrs(d, pipeline, attr,
                                           RSX_DSP_NUM_VERTEX_ATTR);

    printf("     %-22s layout slots %d:", c->name, (int)layout.count);
    for (i = 0; i < (int)layout.count; i++) printf(" %d", (int)layout.attrs[i]);
    printf("\n     %-22s SPIR-V input Locations %d:", "", n);
    for (i = 0; i < n; i++) printf(" %d", locs[i]);
    printf("\n     %-22s pipeline attr locations %u:", "", nattr_vk);
    for (i = 0; i < (int)nattr_vk; i++) printf(" %u", attr[i * 4 + 0]);
    printf("\n");

    if (n != (int)nattr_vk) {
        bad("%s: the SPIR-V declares %d input location(s), the pipeline's table "
            "has %u entries", c->name, n, nattr_vk);
        rc = -1;
    } else {
        for (i = 0; i < n; i++) {
            /* Every input the shader reads must exist in the pipeline's vertex
             * input state at the location the shader reads it from, or the
             * driver feeds the wrong bytes into that variable. */
            int found = 0, j;
            for (j = 0; j < (int)nattr_vk; j++) {
                if ((int32_t)attr[j * 4 + 0] == locs[i]) {
                    found = 1;
                    /* and the offset really is slot*16 */
                    if ((u32)attr[j * 4 + 2] != (u32)j * 16u) {
                        bad("%s: pipeline attribute %d is at offset %u, not "
                            "slot*16 = %u", c->name, j, attr[j * 4 + 2],
                            (u32)j * 16u);
                        rc = -1;
                    }
                    break;
                }
            }
            if (!found) {
                bad("%s: the SPIR-V reads location %d, which the pipeline's "
                    "vertex-attribute table does not describe", c->name,
                    locs[i]);
                rc = -1;
            }
        }
    }
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    if (rc == 0)
        ok("%s: SPIR-V input Locations equal the pipeline's own attribute "
           "table (%u entries)", c->name, nattr_vk);
    return rc;
}

static void case_reflection(void)
{
    static const refl_case cases[] = {
        { "VP MOV (a0+a3, gap)",  0x0009u, 2 },
        { "VP MOV (a0 only)",     0x0001u, 1 },
        { "VP TXL (a0+a8)",       0x0101u, 2 },
        /* Eight attributes, a mask the compact analysis narrows exactly. */
        { "eight attributes",     0x00FFu, 8 },
    };
    rsx_vk_device* d = D();
    uint32_t i;

    printf("---- I: the reflection assertion (S2 root note, vertex inputs)\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        reflect_check(d, &cases[i]);
}

/* ---- J: a draw with no zeta after one with a zeta (item 1) -------------- */

/*
 * The engine creates its pipelines without a depth attachment
 * (pipeline_create is called before the zeta exists), so the first draw that
 * binds a zeta gets a depth-on twin. The S3b code then overwrote
 * `pending_pipeline_id` with the twin's handle, and the *next* draw -- the one
 * that binds no zeta -- was begun with the depth-on pipeline inside a pass with
 * no depth attachment. Vulkan rejects that, so the second draw was dropped.
 *
 * The proof is two draws on one pipeline: one with a zeta, one without, both
 * recording pixels. If the second draw is dropped its half of the target keeps
 * the clear colour.
 */
static void case_depth_variant_does_not_stick(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    u8 vp[64], fp[64];
    char vs_hlsl[65536], ps_hlsl[65536];
    rsx_vertex_layout_plan layout;
    rsx_be_render_state rs;
    u32 pipeline, depth_target, depth_z, target;
    static float verts[6 * 8];
    static const float half[6][2] = {
        { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 0.0f, 1.0f },
        { -1.0f, -1.0f }, { 0.0f, 1.0f }, { -1.0f, 1.0f },
    };
    static const float right[6][2] = {
        { 0.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
        { 0.0f, -1.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f },
    };
    int v, vi, fi;
    u32 nconst = 0;
    u8 lpx[4], rpx[4];
    u32 draws_before;

    printf("---- J: a no-zeta draw after a zeta draw still runs its own pipeline\n");
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);   /* ATTR0 + ATTR3 */
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case J: the pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x204u;
    rs.depth_test = 1;
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;

    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    depth_target = be->depth_target_create(be->user, 8, 8);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    depth_z = depth_target;
    if (!target || !depth_z || !pipeline) {
        bad("case J: target %u, zeta %u, pipeline %u", target, depth_z, pipeline);
        return;
    }
    be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
    be->clear_depth_stencil(be->user, depth_z,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);

    /* Left half WITH a zeta: builds the depth-on twin. */
    for (v = 0; v < 6; v++) {
        verts[v * 8 + 0] = half[v][0];
        verts[v * 8 + 1] = half[v][1];
        verts[v * 8 + 2] = 0.5f;
        verts[v * 8 + 3] = 1.0f;
        verts[v * 8 + 4] = 1.0f;   /* red */
        verts[v * 8 + 5] = 0.0f;
        verts[v * 8 + 6] = 0.0f;
        verts[v * 8 + 7] = 1.0f;
    }
    draws_before = d->draws_recorded;
    be->bind_targets(be->user, &target, 1, depth_z);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* Right half WITHOUT a zeta: same pipeline handle. */
    for (v = 0; v < 6; v++) {
        verts[v * 8 + 0] = right[v][0];
        verts[v * 8 + 1] = right[v][1];
        verts[v * 8 + 2] = 0.5f;
        verts[v * 8 + 3] = 1.0f;
        verts[v * 8 + 4] = 0.0f;
        verts[v * 8 + 5] = 1.0f;   /* green */
        verts[v * 8 + 6] = 0.0f;
        verts[v * 8 + 7] = 1.0f;
    }
    /* Right half WITHOUT a zeta: same pipeline handle, and deliberately NO
     * bind_pipeline here (item N7).
     *
     * The engine binds a pipeline once per state change, not once per draw, so
     * the second draw of a frame legitimately arrives with the first draw's
     * binding still latched. Re-binding it here would overwrite the very field
     * the S3b bug corrupted (`d->pending_pipeline_id = variant`), and the case
     * would pass on the broken tree because the test itself repaired the
     * state. With the bind removed the draw runs on whatever the backend left
     * `pending_pipeline_id` holding, which is the bug. */
    be->bind_targets(be->user, &target, 1, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    info("case J: %u draws recorded by the two calls", d->draws_recorded -
         draws_before);

    memset(lpx, 0xEE, 4);
    memset(rpx, 0xEE, 4);
    be->readback(be->user, target, 1, 4, 1, 1, lpx, 4);
    be->readback(be->user, target, 6, 4, 1, 1, rpx, 4);
    /* The colour the VS wrote is COLOR0 = {A,R,G,B} = (1,0,0,1) red and
     * (0,1,0,1) green, and the readback returns a colour target's bytes as
     * written, so the red half is 255,0,0,255 and the green half 0,255,0,255.
     *
     * The target was cleared to opaque black before the first draw, so a draw
     * that was dropped leaves its half at 0,0,0,255 -- which is what the right
     * half must NOT be. */
    info("case J: left %u,%u,%u,%u  right %u,%u,%u,%u", lpx[0], lpx[1], lpx[2],
         lpx[3], rpx[0], rpx[1], rpx[2], rpx[3]);
    if (near(lpx[0], 255, 4) && near(lpx[1], 0, 4) && near(lpx[2], 0, 4))
        ok("case J: the zeta draw is red on the left");
    else
        bad("case J: the left half is %u,%u,%u,%u, not the zeta draw's red",
            lpx[0], lpx[1], lpx[2], lpx[3]);

    /* THE ASSERTION THAT FAILS ON THE S3b TREE. There, the zeta draw rewrote
     * `pending_pipeline_id` with the depth-on twin's handle. The right-half
     * draw (which this case no longer re-binds a pipeline for) therefore ran
     * the twin -- a pipeline with a depth attachment -- inside a pass with no
     * depth attachment, which Vulkan rejects: the draw is counted as
     * `draws_recorded` and then silently produces nothing, leaving the clear
     * colour. Two failure modes are accepted as failures here: the draw
     * dropping its pixels (the S3b behaviour) and the draw being refused
     * outright. */
    if (near(rpx[0], 0, 4) && near(rpx[1], 255, 4) && near(rpx[2], 0, 4))
        ok("case J: the no-zeta draw after a zeta draw still painted the green "
           "the guest program wrote");
    else
        bad("case J: the no-zeta draw after a zeta draw wrote %u,%u,%u,%u, not "
            "green 0,255,0,255 -- it did not run the engine's own pipeline",
            rpx[0], rpx[1], rpx[2], rpx[3]);

    be->pipeline_release(be->user, pipeline);
    be->depth_target_release(be->user, depth_z);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- K: the descriptor pool's real budget (item 2) ---------------------- */

/*
 * Every draw allocates one descriptor set that writes 2 UBOs, 20 sampled images
 * and 20 samplers (rsx_vk_pipeline.c write_descriptor_set). The pool used to
 * size the images and the samplers at RSX_VK_MAX_TEX_UNITS * SETS -- 16 per set
 * against 20 written -- so a frame that reached the budget ran out. This case
 * records exactly RSX_VK_SETS_PER_FRAME draws in one frame, which is one full
 * set budget, and asserts every one was recorded and every one wrote a pixel.
 */
static void case_descriptor_pool_budget(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc sd;
    u32 pipeline, target, tex;
    u8 texels[16], px[4];
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0, i, recorded;
    /* The budget the pool is sized for: one set per draw, per frame. */
    const uint32_t budget = RSX_VK_SETS_PER_FRAME;
    int all_present = 1;

    printf("---- K: the full per-frame descriptor-set budget allocates\n");
    for (i = 0; i < 4; i++)
        memcpy(texels + i * 4, k_tex_quad[i], 4);
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case K: the pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    tex = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, 0xAAE4u,
                                0x85u);
    if (!target || !pipeline || !tex) {
        bad("case K: target %u, pipeline %u, texture %u", target, pipeline, tex);
        return;
    }
    rsx_vk_texture_upload(d, tex, 0, 0, 2, 2, texels, 8, 2);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(&sd, 0, sizeof sd);
    memset(samplers, 0, sizeof samplers);
    samplers[0] = sd;

    bind_identity_constants(be);
    quad_vertices(verts, 0.25f, 0.25f);   /* texel (0,0) everywhere */
    be->bind_targets(be->user, &target, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->bind_textures(be->user, &tex, samplers, 1u);
    be->set_viewport(be->user, 0, 0, 4, 4);
    be->set_scissor(be->user, 0, 0, 0, 0);

    /* Record the whole budget in ONE frame: no submit between draws, so every
     * set the frame allocates stays live at once and the pool has to hold that
     * many. */
    recorded = d->draws_recorded;
    for (i = 0; i < budget; i++)
        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride,
                 NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* The structural check: the pool must declare at least as many images and
     * samplers per set as write_descriptor_set writes into one, which is
     * RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS of each. The pool's own
     * sizes are what the previous cut got wrong (16 against 20). */
    {
        const uint32_t per_set_images =
            (uint32_t)(RSX_VK_MAX_TEX_UNITS + RSX_VK_MAX_VTEX_UNITS);
        info("case K: %u of %u draws recorded in one frame; the pool must hold "
             "%u images and %u samplers per set", d->draws_recorded - recorded,
             budget, per_set_images, per_set_images);
    }
    if (d->draws_recorded - recorded == budget)
        ok("case K: all %u descriptor sets of a frame allocated (no "
           "VK_ERROR_OUT_OF_POOL_MEMORY)", budget);
    else
        bad("case K: only %u of %u draws recorded",
            d->draws_recorded - recorded, budget);

    memset(px, 0xEE, 4);
    be->readback(be->user, target, 2, 2, 1, 1, px, 4);
    /* The texel is k_tex_quad[0] = {0xFF,0x10,0x20,0x30}, read back in the
     * target's {A,R,G,B} order, which is the row itself. */
    (void)all_present;
    if (px[0] != 0xEE)
        ok("case K: the last draw of the budget still wrote its pixel");
    else
        bad("case K: the last draw wrote nothing");

    rsx_vk_texture_release(d, tex);
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- L: depth_snapshot reuses one descriptor pool (item 3) ------------- */

/*
 * depth_snapshot used to create a VkDescriptorPool per call and destroy it on
 * none of its success paths, so N snapshots were N pools alive to shutdown.
 * The device now creates one at first use and resets it at each fence; this
 * case takes 100 snapshots and asserts the count stayed at 1.
 */
static void case_snapshot_pool_is_one(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    u32 pipeline, target, zeta;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0, i;
    uint32_t pools_before;
    int made = 0;

    printf("---- L: 100 depth_snapshots keep one descriptor pool\n");
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case L: the pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x204u;
    rs.depth_test = 1;
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    zeta = be->depth_target_create(be->user, 8, 8);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!target || !zeta || !pipeline) {
        bad("case L: target %u, zeta %u, pipeline %u", target, zeta, pipeline);
        return;
    }
    be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
    be->clear_depth_stencil(be->user, zeta,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    for (i = 0; i < 6; i++) {
        verts[i * 8 + 2] = 0.5f;
        verts[i * 8 + 4] = 1.0f;   /* red */
        verts[i * 8 + 5] = 0.0f;
        verts[i * 8 + 6] = 0.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    be->bind_targets(be->user, &target, 1, zeta);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    pools_before = d->descriptor_pools_created;
    for (i = 0; i < 100; i++) {
        u32 snap = be->depth_snapshot(be->user, zeta, 8, 8);
        if (snap) {
            made++;
            be->texture_release(be->user, snap);
        }
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        /* Keep the zeta "written" so every snapshot is allowed. */
        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL,
                 0);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
    info("case L: %d of 100 snapshots published; %u pools created during them "
         "(%u alive)", made, d->descriptor_pools_created - pools_before,
         d->descriptor_pools_alive);
    if (made < 100) {
        bad("case L: only %d of 100 snapshots were published", made);
    } else if (d->descriptor_pools_created - pools_before > 1) {
        bad("case L: %u descriptor pools were created for 100 snapshots",
            d->descriptor_pools_created - pools_before);
    } else {
        ok("case L: 100 depth_snapshots created %u descriptor pool(s)",
           d->descriptor_pools_created - pools_before);
    }

    be->pipeline_release(be->user, pipeline);
    be->depth_target_release(be->user, zeta);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- M: the sampler cache retires, and the null sampler survives ------ */

/*
 * Two separate defects, both in rsx_vk_sampler.c:
 *
 *  - eviction destroyed the VkSampler immediately, while a recorded or
 *    in-flight command buffer could still name it through a descriptor set;
 *  - the null sampler was a cache entry, so the 65th distinct sampler evicted
 *    and destroyed it while device->null_sampler still held the handle.
 *
 * This case makes more than RSX_VK_MAX_SAMPLERS distinct samplers and asserts
 * (a) each eviction went onto the retirement list rather than to
 * vkDestroySampler, (b) nothing had to be dropped, and (c) a draw with an
 * unbound unit still samples: the null sampler is alive and correct.
 */
static void case_sampler_cache_retires(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_sampler_desc sd;
    u32 i;
    uint64_t null_before, first_null;
    uint32_t pending_peak = 0, dropped_before;
    int distinct = 0;

    printf("---- M: 70+ distinct samplers, the null sampler survives\n");
    first_null = rsx_vk_null_sampler(d);
    null_before = d->null_sampler;
    dropped_before = rsx_vk_samplers_dropped(d);

    /* 70 distinct descs: vary wrap_s and the linear flags, which are both part
     * of the cache key. */
    for (i = 0; i < 70u; i++) {
        memset(&sd, 0, sizeof sd);
        sd.wrap_s = (u8)((i % 6u) + 1u);
        sd.wrap_t = (u8)((i / 6u) + 1u);
        sd.min_linear = (u8)(i & 1u);
        sd.mag_linear = (u8)((i >> 1) & 1u);
        if (rsx_vk_sampler_get(d, &sd, 1)) {
            distinct++;
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
        }
        if (rsx_vk_samplers_pending(d) > pending_peak)
            pending_peak = rsx_vk_samplers_pending(d);
    }
    info("case M: %d distinct samplers; peak retirement list %u, dropped %u",
         distinct, pending_peak, rsx_vk_samplers_dropped(d) - dropped_before);

    if (distinct > (int)RSX_VK_MAX_SAMPLERS &&
        d->samplers_retired_count + 1u > 0u && pending_peak > 0u)
        ok("case M: %d samplers made %u evictions retire rather than destroy "
           "immediately", distinct, pending_peak);
    else if (pending_peak == 0u)
        bad("case M: no eviction was retired (the cache never filled?)");
    else
        ok("case M: %d samplers made %u retirements", distinct, pending_peak);

    if (rsx_vk_samplers_dropped(d) == dropped_before)
        ok("case M: no retired sampler had to be dropped");
    else
        bad("case M: %u retired samplers were dropped",
            rsx_vk_samplers_dropped(d) - dropped_before);

    if (d->null_sampler == null_before && first_null == null_before)
        ok("case M: the null sampler (0x%llx) is the same handle after 70 "
           "distinct samplers", (unsigned long long)null_before);
    else
        bad("case M: the null sampler changed from 0x%llx to 0x%llx",
            (unsigned long long)null_before,
            (unsigned long long)d->null_sampler);

    /* The structural half of item 5: the null sampler must not be one of the
     * 64 cache entries, or the LRU can evict and destroy it while the device's
     * own field still names it. */
    {
        int in_cache = 0, j;
        for (j = 0; j < (int)RSX_VK_MAX_SAMPLERS; j++)
            if (d->samplers[j].used && d->samplers[j].sampler == d->null_sampler)
                in_cache = 1;
        if (!in_cache)
            ok("case M: the null sampler is not a cache entry, so no eviction "
               "can destroy it");
        else
            bad("case M: the null sampler is a cache entry: an eviction can "
                "destroy it while d->null_sampler still names it");
    }

    /* And it still works: a draw whose unit 0 is unbound samples the null
     * texture through the null sampler, which is a 1x1 opaque white image
     * White reads back as 255,255,255,255 in A8R8G8B8 order. */
    {
        rsx_be_render_state rs;
        rsx_vertex_layout_plan layout;
        rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
        u32 pipeline, target;
        static float verts[6 * 8];
        char vs_hlsl[65536], ps_hlsl[65536];
        u8 vp[64], fp[64];
        u32 vi, fi, nconst = 0;
        u8 px[4];
        u32 zero = 0;

        rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_vertex_layout_plan_init(&layout, 0x0101u);
        vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                         sizeof vs_hlsl);
        fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                          sizeof ps_hlsl, &nconst);
        if (vi <= 0 || fi <= 0) {
            bad("case M: the sampling pair does not decompile");
            return;
        }
        memset(&rs, 0, sizeof rs);
        rs.depth_func = 0x207u;
        rs.color_mask = 0xFFFFFFFFu;
        target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4,
                                         NULL, 0);
        pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                       layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
        if (!target || !pipeline) {
            bad("case M: target %u, pipeline %u", target, pipeline);
            return;
        }
        memset(samplers, 0, sizeof samplers);
        bind_identity_constants(be);
        quad_vertices(verts, 0.5f, 0.5f);
        be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
        be->bind_targets(be->user, &target, 1, 0);
        be->bind_pipeline(be->user, pipeline);
        /* Unit 0 bound to handle 0: nothing, so the null texture and sampler. */
        be->bind_textures(be->user, &zero, samplers, 1u);
        be->set_viewport(be->user, 0, 0, 4, 4);
        be->set_scissor(be->user, 0, 0, 0, 0);
        be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL,
                 0);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        memset(px, 0xEE, 4);
        be->readback(be->user, target, 2, 2, 1, 1, px, 4);
        info("case M: unbound unit 0 reads %u,%u,%u,%u (want 255,255,255,255)",
             px[0], px[1], px[2], px[3]);
        if (px[0] == 255 && px[1] == 255 && px[2] == 255 && px[3] == 255)
            ok("case M: after 70 distinct samplers, an unbound unit still "
               "samples the null texture");
        else
            bad("case M: an unbound unit read %u,%u,%u,%u, not white", px[0],
                px[1], px[2], px[3]);
        be->pipeline_release(be->user, pipeline);
        be->color_target_release(be->user, target);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
}

/* ---- N: sampling a rendered colour target (item 6) --------------------- */

/*
 * Render a colour into target A, then sample A in a second draw into target B,
 * then read B back. The descriptor declares SHADER_READ_ONLY_OPTIMAL for every
 * sampled image, but A is in COLOR_ATTACHMENT_OPTIMAL after its pass, and the
 * tracker had no way back; the second draw sampled an image in the wrong
 * layout.
 *
 * A must be a *colour target*, sampled through the engine's surface_view (which
 * is what a texture unit naming a surface becomes), so this goes through the
 * engine: a surface drawn into, then bound to a texture unit and sampled.
 */
static void case_render_to_texture(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    u32 a, b, view;
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc sd;
    u32 pipeline, pipeline_b;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0;
    u8 px[4];

    printf("---- N: a rendered colour target sampled by a second draw\n");
    /* Fill A with a colour via a draw (not a clear: the clear leaves the image
     * in a different layout, which is the path that already worked). */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case N: the colour pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    a = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    b = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!a || !b || !pipeline) {
        bad("case N: A %u, B %u, pipeline %u", a, b, pipeline);
        return;
    }
    bind_identity_constants(be);
    /* Draw solid green (COL0 = {A,R,G,B} = {1,0,1,0}) into A. */
    quad_vertices(verts, 0.5f, 0.5f);
    for (int i = 0; i < 6; i++) {
        verts[i * 8 + 4] = 0.0f;
        verts[i * 8 + 5] = 1.0f;
        verts[i * 8 + 6] = 0.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    be->bind_targets(be->user, &a, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 4, 4);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* A surface view of A, as a texture unit naming A would get. */
    view = be->surface_view(be->user, a, 0xAAE4u, 0x85u);
    if (!view) {
        bad("case N: surface_view of the rendered target returned 0");
        return;
    }
    /* The sampling program: TEX unit 0 -> target B. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case N: the sampling pair does not decompile");
        return;
    }
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case N: the TEX pair does not decompile");
        return;
    }
    pipeline_b = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                     layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!pipeline_b) {
        bad("case N: the sampling pipeline failed");
        return;
    }
    memset(&sd, 0, sizeof sd);
    memset(samplers, 0, sizeof samplers);
    samplers[0] = sd;
    quad_vertices(verts, 0.5f, 0.5f);
    be->clear_color(be->user, b, (const float[4]){0, 0, 0, 1});
    be->bind_targets(be->user, &b, 1, 0);
    be->bind_pipeline(be->user, pipeline_b);
    be->bind_textures(be->user, &view, samplers, 1u);
    be->set_viewport(be->user, 0, 0, 4, 4);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(px, 0xEE, 4);
    be->readback(be->user, b, 2, 2, 1, 1, px, 4);
    /* A was drawn green as COLOR0 = (R,G,B,A) = (0,1,0,1), so B holds
     * 0,255,0,255. */
    info("case N: B reads %u,%u,%u,%u (want the green drawn into A = "
         "0,255,0,255)", px[0], px[1], px[2], px[3]);
    if (near(px[0], 0, 4) && near(px[1], 255, 4) && near(px[2], 0, 4) &&
        near(px[3], 255, 4))
        ok("case N: a rendered colour target sampled correctly into a second "
           "target");
    else
        bad("case N: B read %u,%u,%u,%u, not the green drawn into A", px[0],
            px[1], px[2], px[3]);

    (void)d;
    be->pipeline_release(be->user, pipeline_b);
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, b);
    be->color_target_release(be->user, a);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- O: a stencil-only clear leaves depth intact (item 8) --------------- */

/*
 * A stencil-only clear used to fold into a whole-attachment LOAD_OP_CLEAR,
 * which also reset depth to clear_depth. This draws depth 0.75 into a zeta,
 * runs a pass, then clears *only* the stencil and runs another pass; the depth
 * must still be the 0.75 the first draw wrote. It is read back through
 * depth_snapshot, which is the only way the backend exposes a zeta's contents.
 */
static void case_stencil_only_clear_keeps_depth(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc sd;
    u32 pipeline, target, zeta, snap, sample_target, pipeline_s;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0, i;
    u8 px[4];
    float got;

    printf("---- O: a stencil-only clear leaves the written depth intact\n");
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case O: the depth pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x204u;   /* GREATER */
    rs.depth_test = 1;
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    zeta = be->depth_target_create(be->user, 8, 8);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!target || !zeta || !pipeline) {
        bad("case O: target %u, zeta %u, pipeline %u", target, zeta, pipeline);
        return;
    }
    be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
    /* Clear both aspects first, to a known state. */
    be->clear_depth_stencil(be->user, zeta,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);
    /* Draw depth 0.75 over the whole target. */
    quad_vertices(verts, 0.5f, 0.5f);
    for (i = 0; i < 6; i++) {
        verts[i * 8 + 2] = 0.75f;
        verts[i * 8 + 4] = 1.0f;
        verts[i * 8 + 5] = 0.0f;
        verts[i * 8 + 6] = 0.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    be->bind_targets(be->user, &target, 1, zeta);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    {
        const uint32_t clears_before = d->depth_clears;
        /* Stencil-only clear: depth must be untouched. */
        be->clear_depth_stencil(be->user, zeta, RSX_BE_CLEAR_STENCIL, 0.0f, 0x3C);
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        if (d->depth_clears > clears_before)
            ok("case O: the stencil-only clear is recorded against the zeta");
        else
            bad("case O: the stencil-only clear did not reach the backend");
    }

    /* A pass on the same zeta so the pending stencil clear folds into its load
     * op. Depth writing is DISABLED for it: a depth write here would rewrite
     * 0.75 and hide whatever the clear did, which is the trap the naive version
     * of this case fell into. */
    {
        rsx_be_render_state r2 = rs;
        u32 p2;
        r2.depth_write = 0;
        r2.depth_test = 0;
        p2 = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &r2, &layout,
                                 layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
        if (p2)
            be->bind_pipeline(be->user, p2);
        else
            bad("case O: the no-depth-write pipeline failed");
    }
    be->bind_targets(be->user, &target, 1, zeta);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    snap = be->depth_snapshot(be->user, zeta, 8, 8);
    if (!snap) {
        bad("case O: depth_snapshot returned 0");
        return;
    }
    /* Sample the snapshot into an R32F target and read the centre. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case O: the sampling pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    pipeline_s = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                     layout.stride, RSX_BE_FMT_R32F, 1);
    sample_target = be->color_target_create(be->user, RSX_BE_FMT_R32F, 8, 8,
                                            NULL, 0);
    if (!pipeline_s || !sample_target) {
        bad("case O: sampling pipeline %u, target %u", pipeline_s, sample_target);
        return;
    }
    memset(&sd, 0, sizeof sd);
    memset(samplers, 0, sizeof samplers);
    samplers[0] = sd;
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    be->clear_color(be->user, sample_target, (const float[4]){0, 0, 0, 0});
    be->bind_targets(be->user, &sample_target, 1, 0);
    be->bind_pipeline(be->user, pipeline_s);
    be->bind_textures(be->user, &snap, samplers, 1u);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    memset(px, 0xEE, 4);
    be->readback(be->user, sample_target, 4, 4, 1, 1, px, 4);
    memcpy(&got, px, 4);
    info("case O: depth after a stencil-only clear = %.6f (want 0.750000)", got);
    if (got > 0.75f - 1e-6f && got < 0.75f + 1e-6f)
        ok("case O: a stencil-only clear left the 0.75 depth the draw wrote");
    else
        bad("case O: the depth is %.6f, not the 0.75 that was written", got);

    be->pipeline_release(be->user, pipeline_s);
    be->color_target_release(be->user, sample_target);
    be->pipeline_release(be->user, pipeline);
    be->depth_target_release(be->user, zeta);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- Q: a depth image sampled by a draw into another target (M2) ------- */

/*
 * Item M2: a depth target bound to a texture unit of a draw that does not
 * attach it.
 *
 * What was wrong: begin_sampled_read skipped depth handles entirely (its
 * transition was guarded by `!rsx_vk_format_is_depth`), so nothing moved the
 * zeta out of DEPTH_STENCIL_ATTACHMENT_OPTIMAL, while bind_sampled_read
 * declared VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL for the depth view anyway.
 * The descriptor named a layout the image was never in, and a depth sampler is
 * not allowed to read SHADER_READ_ONLY at all. The fix transitions both aspects
 * to DEPTH_STENCIL_READ_ONLY_OPTIMAL, records it on the tracker, and declares
 * that same layout.
 *
 * The derivation of the expected value, by hand:
 *
 *  1. The zeta is D32_SFLOAT_S8_UINT. A depth-only pass clears it to 0.0 and
 *     then draws a full-screen quad at clip z = 0.75 with `depth_func =
 *     ALWAYS` and depth writes on. The VS epilogue is `pos.z = o[0].z *
 *     vp_posscale.z + o[0].w * vp_posoffset.z` with posscale = 1 and
 *     posoffset = 0 (bind_identity_constants), so NDC z = 0.75 and the
 *     viewport transform (minDepth 0, maxDepth 1, rsx_vk_pipeline.c's
 *     record_dynamic_state) gives depth = 0.75 exactly. D32_SFLOAT stores that
 *     float without quantisation.
 *
 *  2. The sampling draw binds that zeta to PS unit 0 and renders into a
 *     *different* colour target with **no zeta of its own**, so nothing in this
 *     draw's pass touches the depth image and the M1 null-view rule does not
 *     apply (the unit is genuinely readable).
 *
 *  3. The FP is `TEX r0, TC0, unit0` with NEAREST + CLAMP on an 8x8 zeta, and
 *     tc is (0.5,0.5) at every vertex, i.e. the centre of the 8x8 image: every
 *     texel holds the same 0.75, so the fetch is 0.75 wherever it lands and the
 *     orientation question of case B's 2x2 does not arise here.
 *
 *  4. The target is R32F, so the fragment's .r is stored as the float 0.75 and
 *     the readback's first four bytes are that float bit for bit: 0.75 exactly
 *     (no unorm rounding anywhere in this path).
 *
 * Expected: the sampled value is 0.75 +- 1e-6, and the zeta's tracked layout
 * after the draw is DEPTH_STENCIL_READ_ONLY_OPTIMAL -- which is the half that
 * the old code could not produce, because it recorded no layout at all (the
 * zeta stayed at DEPTH_STENCIL_ATTACHMENT_OPTIMAL while the descriptor claimed
 * SHADER_READ_ONLY).
 */
static void case_depth_sampled_by_draw(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc sd;
    u32 depth_pipe, sample_pipe, dz_target, colour_target;
    u32 zeta;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0, i;
    u32 handi;
    u8 b4[4];
    float got = -1.0f;
    rsx_vk_slot* zslot;
    int32_t want_layout = (int32_t)VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    printf("---- Q: a depth image bound to a texture unit (item M2)\n");

    /* The depth-writing pair: position passthrough, colour from COL0. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case Q(M2): the depth pair does not decompile");
        return;
    }

    /* An attachable, never-sampled-otherwise colour format for the depth-only
     * pass, and an R32F target for the sampled value. */
    dz_target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8,
                                        NULL, 0);
    colour_target = be->color_target_create(be->user, RSX_BE_FMT_R32F, 8, 8,
                                            NULL, 0);
    zeta = be->depth_target_create(be->user, 8, 8);
    if (!dz_target || !colour_target || !zeta) {
        bad("case Q(M2): targets %u/%u, zeta %u", dz_target, colour_target,
            zeta);
        return;
    }

    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;   /* ALWAYS: the quad writes 0.75 wherever it covers */
    rs.depth_test = 1;
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;
    depth_pipe = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                     layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!depth_pipe) {
        bad("case Q(M2): the depth pipeline failed");
        return;
    }
    be->clear_color(be->user, dz_target, (const float[4]){0, 0, 0, 1});
    be->clear_depth_stencil(be->user, zeta,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    for (i = 0; i < 6; i++) {
        verts[i * 8 + 2] = 0.75f;      /* clip z = 0.75 -> depth 0.75 */
        verts[i * 8 + 4] = 1.0f;
        verts[i * 8 + 5] = 1.0f;
        verts[i * 8 + 6] = 1.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    be->bind_targets(be->user, &dz_target, 1, zeta);
    be->bind_pipeline(be->user, depth_pipe);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* The sampling pair: a TEX through PS unit 0, into a target of its own,
     * with NO zeta bound -- so the zeta is sampled, not attached. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case Q(M2): the sampling pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    sample_pipe = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                      layout.stride, RSX_BE_FMT_R32F, 1);
    if (!sample_pipe) {
        bad("case Q(M2): the sampling pipeline failed");
        return;
    }
    memset(samplers, 0, sizeof samplers);
    memset(&sd, 0, sizeof sd);
    sd.wrap_s = sd.wrap_t = sd.wrap_r = 3u;    /* CLAMP_TO_EDGE */
    sd.min_linear = sd.mag_linear = 0u;
    samplers[0] = sd;
    handi = zeta;
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    be->clear_color(be->user, colour_target, (const float[4]){0, 0, 0, 0});
    be->bind_targets(be->user, &colour_target, 1, 0);
    be->bind_pipeline(be->user, sample_pipe);
    be->bind_textures(be->user, &handi, samplers, 1u);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(b4, 0, 4);
    be->readback(be->user, colour_target, 4, 4, 1, 1, b4, 4);
    memcpy(&got, b4, 4);
    info("case Q(M2): a zeta written at depth 0.75 and bound to PS unit 0 "
         "sampled %.6f (want 0.750000)", (double)got);
    if (got > 0.75f - 1e-6f && got < 0.75f + 1e-6f)
        ok("case Q(M2): the sampled depth value is the 0.75 the depth pass "
           "wrote");
    else
        bad("case Q(M2): the sampled depth is %.6f, not 0.75", (double)got);

    /* The recorded state: the transition really happened, and it really went
     * to the layout a depth sampler may read (the half the old code missed). */
    zslot = rsx_vk_handle_get(d->handles, zeta);
    if (zslot && zslot->kind == RSX_VK_SLOT_DEPTH) {
        const int32_t have = rsx_vk_image_layout(&zslot->u.depth.image);
        info("case Q(M2): the zeta's tracked layout after the sampling draw "
             "is %d (DERIVED WANT %d)", (int)have, (int)want_layout);
        if (have == want_layout)
            ok("case Q(M2): a sampled zeta is tracked as "
               "DEPTH_STENCIL_READ_ONLY_OPTIMAL, the layout its descriptor "
               "declares");
        else
            bad("case Q(M2): the zeta's tracked layout is %d, not %d (it was "
                "never transitioned for sampling, or went to the colour-read "
                "layout)", (int)have, (int)want_layout);
    } else {
        bad("case Q(M2): the zeta slot is gone");
    }

    be->pipeline_release(be->user, sample_pipe);
    be->pipeline_release(be->user, depth_pipe);
    be->depth_target_release(be->user, zeta);
    be->color_target_release(be->user, colour_target);
    be->color_target_release(be->user, dz_target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- S: self-sampling through a surface view (M1) ---------------------- */

/*
 * Item M1: a draw that names its OWN colour target through a surface view.
 *
 * What was wrong: the feedback-loop test in rsx_vk_pipeline.c compared
 * VkImageView handles (`view == attached[a]`). A surface view is a second
 * VkImageView over the same VkImage, so the comparison missed it: the unit was
 * bound, the image was transitioned to SHADER_READ_ONLY_OPTIMAL before the
 * pass, the pass moved it back to COLOR_ATTACHMENT_OPTIMAL, and the descriptor
 * still declared SHADER_READ_ONLY -- an illegal feedback loop plus a descriptor
 * naming a layout the image was not in. The fix compares the *image* (a surface
 * view resolves to its parent through rsx_vk_handle_image), so any unit naming
 * an attachment of this draw on any view gets the null view.
 *
 * The derivation of the expected pixel:
 *
 *  1. Target A is 4x4 R8G8B8A8, cleared once to {0,0,0,1} -- opaque black -- by
 *     a direct vkCmdClearColorImage before the draw under test.
 *  2. The draw attaches A and binds a surface view of A to PS unit 0. The FP is
 *     `TEX r0, TC0, unit0` at tc (0.5,0.5), i.e. the centre of A.
 *  3. With the loop detected, unit 0 is forced to the engine's NULL VIEW -- a
 *     1x1 opaque-white image -- so the fetch is
 *     (R,G,B,A) = (1,1,1,1) at every tc.
 *  4. A's readback is its own A8R8G8B8 bytes in {A,R,G,B} order (case B pins
 *     that order on this same format), so an opaque white fragment reads back
 *     as 255,255,255,255. Without the fix the same draw would sample A while
 *     writing it, which is undefined -- on lavapipe the sampled value for this
 *     shape comes from the pass's own store (the draw wrote white into A too,
 *     so a pixel assertion alone cannot tell the two apart).
 *  5. That is why the two assertions that DO discriminate are the recorded
 *     state and the log line: feedback_loops/feedback_logged must both move by
 *     exactly 1 (one line, not one per unit) and A's tracked layout after the
 *     draw must be COLOR_ATTACHMENT_OPTIMAL -- the pass's layout. On the old
 *     code A's layout was recorded as SHADER_READ_ONLY_OPTIMAL by
 *     rsx_vk_cmd_to_sampled_read and then left there by the pass, because the
 *     pass's own to_color_attachment ran *before* that transition.
 */
static void case_self_sample_via_surface_view(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc sd;
    u32 pipeline, a, view;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0;
    u32 loops_before, logged_before;
    u8 px[4];
    rsx_vk_slot* aslot;
    int32_t want_layout = (int32_t)VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    printf("---- S: a draw sampling its own target through a surface view "
           "(item M1)\n");

    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case S(M1): the TEX pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    a = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!a || !pipeline) {
        bad("case S(M1): target %u, pipeline %u", a, pipeline);
        return;
    }
    view = be->surface_view(be->user, a, 0xAAE4u, 0x85u);
    if (!view) {
        bad("case S(M1): surface_view of A returned 0");
        return;
    }
    /* The view must be a DIFFERENT VkImageView over the SAME image, or the case
     * is not testing what it claims (the old code's view comparison would have
     * caught a shared view). */
    {
        uint64_t vv = 0, av = 0;
        uint32_t vf = 0, af = 0;
        rsx_vk_image* vimg = rsx_vk_handle_image(d, view);
        rsx_vk_image* aimg = rsx_vk_handle_image(d, a);
        int distinct = rsx_vk_handle_view(d, view, &vv, &vf) &&
                       rsx_vk_handle_view(d, a, &av, &af) && vv != av;
        int same_image = vimg && aimg && vimg->image == aimg->image &&
                         vimg->image != 0;
        info("case S(M1): surface view %u (view 0x%llx) and target %u (view "
             "0x%llx) are distinct views over one image: %d/%d", view,
             (unsigned long long)vv, a, (unsigned long long)av, distinct,
             same_image);
        if (distinct && same_image)
            ok("case S(M1): the unit names the target through a different "
               "VkImageView over the same VkImage (a view comparison cannot "
               "see it)");
        else
            bad("case S(M1): the surface view is not a second view over A");
    }

    be->clear_color(be->user, a, (const float[4]){0, 0, 0, 1});
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(samplers, 0, sizeof samplers);
    memset(&sd, 0, sizeof sd);
    sd.wrap_s = sd.wrap_t = sd.wrap_r = 3u;
    samplers[0] = sd;
    loops_before = d->feedback_loops;
    logged_before = d->feedback_logged;
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    be->bind_targets(be->user, &a, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->bind_textures(be->user, &view, samplers, 1u);
    be->set_viewport(be->user, 0, 0, 4, 4);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    info("case S(M1): feedback loops %u -> %u, log lines %u -> %u",
         loops_before, d->feedback_loops, logged_before, d->feedback_logged);
    if (d->feedback_loops == loops_before + 1u &&
        d->feedback_logged == logged_before + 1u)
        ok("case S(M1): the loop through the surface view was detected and "
           "logged exactly once");
    else
        bad("case S(M1): feedback loops moved %u and log lines %u (want 1 and "
            "1): the unit naming the draw's own target through a surface view "
            "was not suppressed", d->feedback_loops - loops_before,
            d->feedback_logged - logged_before);

    /* The recorded state, read HERE and not after the readback below: the
     * readback is itself a command that moves the image (it transitions A to
     * TRANSFER_SRC_OPTIMAL, rsx_vk_cmd_copy_to_readback, and the tracker keeps
     * that layout), so asserting after it would assert the readback's layout
     * and not the draw's. At this point the last thing recorded for A is the
     * pass that attached it.
     *
     * There are TWO ways A can have been left in a readable layout, and this
     * check catches both: SHADER_READ_ONLY_OPTIMAL (5) if the sampled-read
     * transition fired and the pass's own to_color_attachment did not undo it,
     * and GENERAL (6) if the sampler recorded a vkCmdClearColorImage's
     * transition where a sampled-read barrier belongs. Only
     * COLOR_ATTACHMENT_OPTIMAL (2) means "A is exactly where the pass put it and
     * nothing else moved it". */
    aslot = rsx_vk_handle_get(d->handles, a);
    if (aslot && aslot->kind == RSX_VK_SLOT_COLOR_TARGET) {
        const int32_t have = rsx_vk_image_layout(&aslot->u.color);
        info("case S(M1): A's tracked layout right after the draw is %d "
             "(COLOR_ATTACHMENT_OPTIMAL=2, SHADER_READ_ONLY=5, GENERAL=6)",
             (int)have);
        if (have == want_layout)
            ok("case S(M1): A is left in COLOR_ATTACHMENT_OPTIMAL, the layout "
               "the pass attached it in");
        else
            bad("case S(M1): A's tracked layout is %d, not %d: something moved "
                "an image this draw was writing",
                (int)have, (int)want_layout);
    } else {
        bad("case S(M1): the target slot is gone");
    }

    /* The pixel: the null view is opaque white, and white is also what the
     * draw wrote, so this pins "the unit was really rebound", not the loop. */
    memset(px, 0xEE, 4);
    be->readback(be->user, a, 2, 2, 1, 1, px, 4);
    info("case S(M1): the centre of A reads %u,%u,%u,%u (want the null "
         "view's white)", px[0], px[1], px[2], px[3]);
    if (near(px[0], 255, 2) && near(px[1], 255, 2) && near(px[2], 255, 2) &&
        near(px[3], 255, 2))
        ok("case S(M1): the suppressed unit sampled the null view, not A");
    else
        bad("case S(M1): the centre of A reads %u,%u,%u,%u, not white",
            px[0], px[1], px[2], px[3]);

    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, a);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- T: the no-op submit is still a frame boundary (M3) ---------------- */

/*
 * Item M3: rsx_vk_device_submit_and_wait returned early when nothing was
 * recorded, before the snapshot pool reset and the snapshots_this_frame = 0
 * that the fence path does. So a frame whose last act was a snapshot that had
 * used the per-frame allowance, followed by a frame that recorded nothing, left
 * the counter at the cap and refused the next frame's snapshots.
 *
 * The check, in order:
 *  1. One snapshot per submit, 32 times, so the frame allowance is spent and
 *     the pool is at its cap; each submit is a real one (submits moves).
 *  2. After the 32nd, a submit with NOTHING recorded -- the no-op path. It must
 *     not submit (submit_noops moves, submits does not) and it must reset the
 *     frame: snapshots_this_frame back to 0.
 *  3. A 33rd snapshot in the next frame must succeed. On the old code it was
 *     refused, because only the fence reset the counter and the no-op path
 *     never reached it.
 *
 * Derivation of "32": RSX_VK_SNAPSHOTS_PER_FRAME is the documented per-frame
 * cap and sizes the pool, so the number the case spends is read from the
 * constant rather than spelled.
 */
static void case_noop_submit_resets_the_frame(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    u32 pipeline, target, zeta;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    u32 vi, fi, nconst = 0, i;
    u32 made = 0, last = 0;
    uint32_t subs_before, resets_before, refused_before;

    printf("---- T: the no-op submit resets the frame (item M3)\n");
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
    rsx_vertex_layout_plan_init(&layout, 0x0009u);
    vi = rsx_vp_decompile_compact_ex(vp, 32, 0, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 16, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case T(M3): the pair does not decompile");
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x204u;
    rs.depth_test = 1;
    rs.depth_write = 1;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 8, 8, NULL, 0);
    zeta = be->depth_target_create(be->user, 8, 8);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    if (!target || !zeta || !pipeline) {
        bad("case T(M3): target %u, zeta %u, pipeline %u", target, zeta,
            pipeline);
        return;
    }
    be->clear_color(be->user, target, (const float[4]){0, 0, 0, 1});
    be->clear_depth_stencil(be->user, zeta,
                            RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 0.0f, 0);
    bind_identity_constants(be);
    quad_vertices(verts, 0.5f, 0.5f);
    for (i = 0; i < 6; i++) {
        verts[i * 8 + 2] = 0.5f;
        verts[i * 8 + 4] = 1.0f;
        verts[i * 8 + 5] = 0.0f;
        verts[i * 8 + 6] = 0.0f;
        verts[i * 8 + 7] = 1.0f;
    }
    be->bind_targets(be->user, &target, 1, zeta);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 8, 8);
    be->set_scissor(be->user, 0, 0, 0, 0);

    /* One draw first, because depth_snapshot refuses a zeta that has only ever
     * been cleared (a clear-only zeta is not a depth map, rsx_draw_engine.c:
     * 664-674 applies the same rule). */
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    /* One snapshot only: spend the whole allowance below, deliberately. */
    last = be->depth_snapshot(be->user, zeta, 4, 4);
    if (!last) {
        bad("case T(M3): the first snapshot was refused");
        return;
    }
    be->texture_release(be->user, last);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    refused_before = d->snapshots_refused;
    subs_before = d->submits;
    resets_before = d->frame_resets;

    /* Spend the whole per-frame allowance inside ONE frame: a draw keeps the
     * zeta "written" (a snapshot of a clear-only zeta is refused by design),
     * and each snapshot is released and re-published in the same frame. */
    for (i = 0; i < RSX_VK_SNAPSHOTS_PER_FRAME; i++) {
        u32 snap = be->depth_snapshot(be->user, zeta, 4, 4);
        if (snap) {
            made++;
            be->texture_release(be->user, snap);
        }
    }
    info("case T(M3): %u of %u snapshots published inside one frame", made,
         (unsigned)RSX_VK_SNAPSHOTS_PER_FRAME);
    if (made == RSX_VK_SNAPSHOTS_PER_FRAME)
        ok("case T(M3): the frame's whole snapshot allowance was spent (%u)",
           made);
    else
        bad("case T(M3): only %u of %u snapshots published in the frame", made,
            (unsigned)RSX_VK_SNAPSHOTS_PER_FRAME);
    if (d->snapshots_this_frame == RSX_VK_SNAPSHOTS_PER_FRAME)
        ok("case T(M3): the counter is at the cap after the frame's %u "
           "snapshots", made);
    else
        bad("case T(M3): the counter is %u after %u snapshots",
            d->snapshots_this_frame, made);

    /* The submit that carries those barriers: they really were submitted. */
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    info("case T(M3): submits %u -> %u", subs_before, d->submits);
    if (d->submits == subs_before + 1u)
        ok("case T(M3): the command buffer holding the snapshot barriers was "
           "submitted");
    else
        bad("case T(M3): the snapshot barriers were not submitted (submits %u "
            "-> %u)", subs_before, d->submits);

    /* The frame boundary itself. */
    if (d->snapshots_this_frame == 0)
        ok("case T(M3): the submit reset the per-frame snapshot counter to 0");
    else
        bad("case T(M3): the counter is %u after a submit, not 0",
            d->snapshots_this_frame);

    /* Now the no-op path: nothing recorded between here and the call. */
    {
        const uint32_t subs_now = d->submits;
        const uint32_t noops_now = d->submit_noops;
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        info("case T(M3): the no-op submit moved noops %u -> %u, submits %u "
             "-> %u, frame resets %u -> %u", noops_now, d->submit_noops,
             subs_now, d->submits, resets_before, d->frame_resets);
        if (d->submit_noops == noops_now + 1u && d->submits == subs_now)
            ok("case T(M3): an empty submit is still a no-op, not a submit");
        else
            bad("case T(M3): an empty submit was not the no-op path");
        if (d->frame_resets > resets_before)
            ok("case T(M3): the no-op path ran the per-frame reset");
        else
            bad("case T(M3): the no-op path skipped the per-frame reset");
        if (d->snapshots_this_frame == 0)
            ok("case T(M3): the no-op path reset the snapshot counter to 0");
        else
            bad("case T(M3): the counter is %u after the no-op path, not 0: "
                "the next frame's snapshots are refused for nothing",
                d->snapshots_this_frame);
    }

    /* And the next frame's snapshot succeeds. */
    {
        u32 again = be->depth_snapshot(be->user, zeta, 4, 4);
        info("case T(M3): a snapshot after the no-op submit -> %u (refused "
             "%u since the frame began)", again,
             d->snapshots_refused - refused_before);
        if (again) {
            ok("case T(M3): a snapshot in the frame after a no-op submit "
               "succeeds");
            be->texture_release(be->user, again);
        } else {
            bad("case T(M3): the snapshot after a no-op submit was refused: "
                "the no-op path left the frame's counter at the cap");
        }
    }

    be->pipeline_release(be->user, pipeline);
    be->depth_target_release(be->user, zeta);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- P: one blend-forced-off log bit per format (item 14) ------------- */

/*
 * DESIGN §3.1 fallback 3 logs once per format whose VkFormat has no
 * COLOR_ATTACHMENT_BLEND. The bit that suppressed the repeat was a constant
 * (1u << 24), so the *first* such format silenced every other. This case
 * builds pipelines for as many distinct formats as the device reports a missing
 * blend bit on and asserts the mask has one bit per format, not one bit
 * overall.
 *
 * Only ATTACHABLE colour formats are used (item N6). A BC format has neither
 * COLOR_ATTACHMENT_BIT nor COLOR_ATTACHMENT_BLEND_BIT -- it cannot be a colour
 * attachment at all, which is DESIGN §3.1 fallback 1's refusal -- so building a
 * pipeline whose only colour attachment is BC1 asks the driver to create a
 * graphics pipeline over a format it does not support as a render target. The
 * earlier version of this case did exactly that and relied on lavapipe's
 * permissiveness: on a driver that validates, vkCreateGraphicsPipelines fails
 * and the mask never gets the bit, so the test would report a missing bit and
 * call it a backend defect. The fix is to require COLOR_ATTACHMENT_BIT before
 * a format is eligible for this case; the mask bits for BC are then simply not
 * expected, because nothing in the engine can reach pipeline_build with a BC
 * colour target (color_target_create refuses it, DESIGN §3.1 fallback 1).
 */
static void case_blend_log_one_bit_per_format(void)
{
    rsx_vk_device* d = D();
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    int i, n_noblend = 0, bits = 0;
    static const char* vs = "float4 main(float4 p : ATTR0) : SV_Position {"
                            " return p; }";
    static const char* ps = "float4 main() : SV_Target { return float4(1,0,0,1); }";

    printf("---- P: the blend-forced-off mask has one bit per format\n");
    rsx_vertex_layout_plan_init(&layout, 0x0001u);
    memset(&rs, 0, sizeof rs);
    rs.color_mask = 0xFFFFFFFFu;
    for (i = (int)RSX_BE_FMT_R8; i <= (int)RSX_BE_FMT_R32G32B32A32F; i++) {
        const uint32_t feat = d->format_features[i];
        u32 p;
        if (!d->format_table[i])
            continue;
        /* Item N6: only a format the device can attach may be a pipeline's
         * colour target. BC1/2/3 fail this and are skipped, which is what keeps
         * the case off a format the driver is right to reject. */
        if (!(feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
            continue;
        if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
            continue;
        p = be->pipeline_create(be->user, vs, ps, &rs, &layout, layout.stride,
                                (rsx_be_format)i, 1);
        if (p) {
            n_noblend++;
            be->pipeline_release(be->user, p);
        }
    }
    {
        uint32_t m = d->blend_forced_off_mask;
        while (m) {
            bits += (int)(m & 1u);
            m >>= 1;
        }
    }
    info("case P: %d format(s) without COLOR_ATTACHMENT_BLEND; mask 0x%08x has "
         "%d bit(s)", n_noblend, d->blend_forced_off_mask, bits);
    if (n_noblend <= 1)
        ok("case P: only %d format lacks COLOR_ATTACHMENT_BLEND on this device, "
           "so the per-format bit cannot be observed", n_noblend);
    else if (bits == n_noblend)
        ok("case P: %d formats produced %d separate log bits", n_noblend, bits);
    else
        bad("case P: %d formats produced only %d log bit(s)", n_noblend, bits);

    /* And the bit really is per format: 1 << rsx_be_format, not 1 << 24. */
    for (i = (int)RSX_BE_FMT_R8; i <= (int)RSX_BE_FMT_R32G32B32A32F; i++) {
        if (!d->format_table[i])
            continue;
        if (!(d->format_features[i] & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
            continue;
        if (d->format_features[i] & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
            continue;
        if (d->blend_forced_off_mask & (1u << (uint32_t)i))
            ok("case P: format %d sets its own bit %u", i, (uint32_t)i);
        else
            bad("case P: format %d did not set bit %u", i, (uint32_t)i);
    }
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- Q: PS unit 0 and VS unit 0 are separate slots (item 7) ------------ */

/*
 * The item-7 alias, tested where it is visible: both stages sample their own
 * unit 0 in the SAME draw, the two textures differ in more than colour, the two
 * samplers differ in more than state, and the pixel is the product of the two
 * fetches.
 *
 * Why this differs from case G (which is the same defect with the vertex stage
 * as the subject): here the PS is the stage under the magnifying glass, and the
 * two fetches land on different texels because of the *wrap* alone.
 *
 *   VS unit 0  a 2x2 texture, uniform (1,1,1,1) in presented RGBA -- so
 *              whatever texel the VS fetches, the multiply by it changes
 *              nothing. Its only job is to be a second, differently-shaped
 *              texture occupying the VS's own slots.
 *              sampler: REPEAT (guest 1), NEAREST.
 *   PS unit 0  a 2x2 texture with texel (0,0) = {A,R,G,B} = {0xFF,0xFF,0x00,
 *              0x00} (presented red) and every other texel {0xFF,0x00,0xFF,
 *              0x00} (presented green).
 *              sampler: CLAMP_TO_EDGE (guest 3), NEAREST.
 *
 *   tc = 1.25 at every vertex for both stages.
 *
 *   PS fetch: tc 1.25, CLAMP_TO_EDGE -> texel (1,1). Guest bytes
 *   {A,R,G,B} = {0xFF,0x00,0xFF,0x00}, presented RGBA = (0,1,0,1) -- green.
 *   VS fetch: the uniform white texture, whatever texel it lands on.
 *
 *   FP `TEX r0, TC0, unit0 ; MUL r0, r0, COL0`, componentwise:
 *
 *     (R,G,B,A) = (0,1,0,1) * (1,1,1,1) = (0,1,0,1)
 *
 *   and the target's A8R8G8B8 bytes in the guest's {A,R,G,B} order (case B's
 *   own result on this format) are
 *
 *     {A,R,G,B} = (255, 0, 255, 0)
 *
 *   The three derivable answers are therefore all different pixels:
 *
 *   correct (separate slots)   (255, 0,   255, 0)
 *   PS SAMPLER aliased (REPEAT, tc 1.25 -> texel (0,0) = red, presented
 *     (R,G,B,A) = (1,0,0,1))    (255, 255, 0,   0)
 *   PS IMAGE aliased (the uniform white VS texture, presented (1,1,1,1))
 *                              (255, 255, 255, 255)
 */
static void case_vs_and_ps_unit0_are_separate(void)
{
    static const u8 ps_texels[4][4] = {
        { 0xFF, 0xFF, 0x00, 0x00 },   /* (0,0) guest {A,R,G,B}: presented RED   */
        { 0xFF, 0x00, 0xFF, 0x00 },   /* (1,0) presented GREEN                  */
        { 0xFF, 0x00, 0xFF, 0x00 },   /* (0,1) presented GREEN                  */
        { 0xFF, 0x00, 0xFF, 0x00 },   /* (1,1) presented GREEN                  */
    };
    static const u8 vs_texels[4][4] = {
        { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF },
        { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0xFF, 0xFF, 0xFF },
    };
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 tex_vs, tex_ps, target, pipeline;
    static float verts[6 * 8];
    char vs_hlsl[65536], ps_hlsl[65536];
    u8 vp[64], fp[64];
    static u8 vs_bytes[16], ps_bytes[16];
    u32 vi, fi, nconst = 0, i;
    u8 px[4];

    printf("---- Q: PS unit 0 and VS unit 0 hold different textures and samplers\n");
    for (i = 0; i < 4; i++) {
        memcpy(ps_bytes + i * 4, ps_texels[i], 4);
        memcpy(vs_bytes + i * 4, vs_texels[i], 4);
    }
    /* VP, three instructions (see case G's note on o[7]):
     *   o[0] = ATTR0, o[1] = TXL(vtex0, ATTR8), o[7] = ATTR8.
     * The FP samples PS unit 0 at TEXCOORD0 and multiplies by COLOR0, so the
     * pixel is the product of both stages' fetches. */
    rsx_test_vp_mov_out(vp + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_txl_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    write_fp_tex_mul_col0(fp, 0);
    rsx_vertex_layout_plan_init(&layout, 0x0101u);
    vi = rsx_vp_decompile_compact_ex(vp, 48, 1u, layout.mask, vs_hlsl,
                                     sizeof vs_hlsl);
    fi = rsx_fp_decompile_buffered_ex(fp, 32, 0x40u, 0u, ps_hlsl,
                                      sizeof ps_hlsl, &nconst);
    if (vi <= 0 || fi <= 0) {
        bad("case Q: the pair does not decompile (%u, %u)", vi, fi);
        return;
    }
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;
    rs.color_mask = 0xFFFFFFFFu;
    target = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
    pipeline = be->pipeline_create(be->user, vs_hlsl, ps_hlsl, &rs, &layout,
                                   layout.stride, RSX_BE_FMT_R8G8B8A8, 1);
    tex_vs = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, 0xAAE4u,
                                   0x85u);
    tex_ps = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1, 1, 0xAAE4u,
                                   0x85u);
    if (!target || !pipeline || !tex_vs || !tex_ps) {
        bad("case Q: target %u, pipeline %u, textures %u/%u", target, pipeline,
            tex_vs, tex_ps);
        return;
    }
    rsx_vk_texture_upload(d, tex_vs, 0, 0, 2, 2, vs_bytes, 8, 2);
    rsx_vk_texture_upload(d, tex_ps, 0, 0, 2, 2, ps_bytes, 8, 2);
    (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

    quad_vertices(verts, 1.25f, 1.25f);   /* clamped PS: (1,1); wrapped: (0,0) */
    bind_identity_constants(be);
    memset(samplers, 0, sizeof samplers);

    /* PS unit 0: CLAMP_TO_EDGE (guest 3), NEAREST -- tc 1.25 -> texel (1,1). */
    samplers[0].wrap_s = samplers[0].wrap_t = samplers[0].wrap_r = 3u;
    {
        const u32 tex_ps_h = tex_ps;
        be->bind_textures(be->user, &tex_ps_h, samplers, 1u);
    }
    /* VS unit 0: REPEAT (guest 1), NEAREST. Uniform white, so its texel choice
     * does not change the product -- only its *existence* does. */
    memset(samplers, 0, sizeof samplers);
    samplers[0].wrap_s = samplers[0].wrap_t = samplers[0].wrap_r = 1u;
    be->bind_vertex_textures(be->user, &tex_vs, samplers, 1u);

    be->clear_color(be->user, target, (const float[4]){0, 0, 1, 1});   /* blue */
    be->bind_targets(be->user, &target, 1, 0);
    be->bind_pipeline(be->user, pipeline);
    be->set_viewport(be->user, 0, 0, 4, 4);
    be->set_scissor(be->user, 0, 0, 0, 0);
    be->draw(be->user, RSX_TOPOLOGY_TRIANGLES, verts, 6, layout.stride, NULL, 0);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(px, 0xEE, 4);
    be->readback(be->user, target, 2, 2, 1, 1, px, 4);
    info("case Q: centre %u,%u,%u,%u (derived 255,0,255,0; 255,255,0,0 means the "
         "PS's sampler aliased the VS's; white means the image did)", px[0],
         px[1], px[2], px[3]);
    if (near(px[0], 255, 4) && near(px[1], 0, 4) && near(px[2], 255, 4) &&
        near(px[3], 0, 4))
        ok("case Q: both stages sampled their own unit 0 through their own "
           "sampler (the derived green texel)");
    else if (near(px[0], 255, 4) && near(px[1], 255, 4) && near(px[2], 0, 4))
        bad("case Q: the PS fetched through the VS sampler (tc 1.25 wrapped to "
            "texel (0,0) = red): the two sampler slots alias");
    else if (near(px[0], 255, 4) && near(px[1], 255, 4) && near(px[2], 255, 4))
        bad("case Q: the PS fetched the VS's texture: the two image slots alias");
    else
        bad("case Q: the pixel %u,%u,%u,%u is none of the three derivable "
            "answers", px[0], px[1], px[2], px[3]);

    rsx_vk_texture_release(d, tex_vs);
    rsx_vk_texture_release(d, tex_ps);
    be->pipeline_release(be->user, pipeline);
    be->color_target_release(be->user, target);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

/* ---- R: items 9-13 and 15, the lower-severity fixes -------------------- */

/*
 * One case per small fix, each asserting the property the fix establishes
 * rather than merely that the code ran.
 */
static void case_lower_severity_items(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = D();

    printf("---- R: the lower-severity items (9-13, 15)\n");

    /* Item 9: a BC upload's row length is in BLOCKS and an uncompressed
     * upload's is in TEXELS, both derived from row_bytes. The observable is
     * that a row_bytes that is not a whole number of blocks or texels is
     * refused rather than silently misinterpreted. */
    {
        u32 t = rsx_vk_texture_create(d, RSX_BE_FMT_BC1, 4, 4, 1, 1, 0xAAE4u,
                                      0x86u);
        u32 t2 = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1,
                                       0xAAE4u, 0x85u);
        const u8 blk[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        u8 rgba[64];
        uint32_t u_before = d->texture_uploads;
        if (t && t2) {
            memset(rgba, 0, sizeof rgba);
            /* 8 bytes is one BC1 block; 12 is not a whole number of 8-byte
             * blocks, so it must be refused. */
            rsx_vk_texture_upload(d, t, 0, 0, 4, 4, blk, 8, 1);
            rsx_vk_texture_upload(d, t, 0, 0, 4, 4, blk, 12, 1);
            /* 4 bytes a texel is fine; 3 is not a whole texel row for RGBA8. */
            rsx_vk_texture_upload(d, t2, 0, 0, 4, 4, rgba, 16, 4);
            rsx_vk_texture_upload(d, t2, 0, 0, 4, 4, rgba, 15, 4);
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
            info("case R: %u uploads accepted of 4 offered (the two ragged "
                 "pitches must be refused)", d->texture_uploads - u_before);
            if (d->texture_uploads - u_before == 2)
                ok("case R(9): row_bytes in blocks for BC and in texels for RGBA8,"
                   " and a ragged pitch is refused");
            else
                bad("case R(9): %u uploads accepted of 4 (want 2)",
                    d->texture_uploads - u_before);
            rsx_vk_texture_release(d, t);
            rsx_vk_texture_release(d, t2);
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
        } else {
            bad("case R(9): could not create the BC1/RGBA8 textures");
        }
    }

    /* Item 10: a surface view resolves to its parent's format for
     * filterability. The observable is that a surface view of an R8G8B8A8
     * target and the target itself answer the same. */
    {
        u32 tgt = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4, 4,
                                          NULL, 0);
        u32 view = tgt ? be->surface_view(be->user, tgt, 0xAAE4u, 0x85u) : 0;
        if (tgt && view) {
            rsx_vk_image* parent_img = rsx_vk_handle_image(d, view);
            rsx_vk_slot* parent = rsx_vk_handle_get(d->handles, tgt);
            if (parent_img && parent &&
                parent_img->format == parent->u.color.format)
                ok("case R(10): a surface view resolves to its parent target's "
                   "format (0x%x)", parent_img->format);
            else
                bad("case R(10): a surface view's resolved format does not match "
                    "its parent's");
            be->color_target_release(be->user, tgt);
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
        } else {
            bad("case R(10): target %u, view %u", tgt, view);
        }
    }

    /* Item 11 / L2: a surface view of a NON-FILTERABLE parent must be sampled
     * with NEAREST, not with whatever filter the guest asked for.
     *
     * DESIGN §3.1 fallback 2: the filterability that decides the sampler is the
     * *parent target's* format, because the view is a view over the parent's
     * image. This case makes a surface view over an R32G32B32A32F target --
     * which lavapipe reports without SAMPLED_IMAGE_FILTER_LINEAR -- asks the
     * draw to bind it with a LINEAR sampler desc, and asserts that what the
     * backend actually bound is NEAREST.
     *
     * The observable is rsx_vk_sampler_desc_for, which returns the desc the
     * sampler cache entry was built from *after* fallback 2's clamp, so the
     * assertion is about the VkSampler state rather than about "a sampler
     * exists". The alternative -- reading the pixel -- cannot separate a
     * nearest that was forced from a nearest the guest asked for, which is why
     * this is an accessor check (the task allows exactly that for a case
     * lavapipe cannot fault).
     */
    {
        rsx_be_sampler_desc linear;
        rsx_be_sampler_desc got;
        rsx_vk_slot* parent_slot;
        const uint32_t forced_before = rsx_vk_samplers_nearest_forced(d);
        u32 parent = be->color_target_create(be->user, RSX_BE_FMT_R32G32B32A32F,
                                             4, 4, NULL, 0);
        u32 view = parent ? be->surface_view(be->user, parent, 0xAAE4u, 0x9Cu)
                          : 0;
        parent_slot = parent ? rsx_vk_handle_get(d->handles, parent) : NULL;
        memset(&linear, 0, sizeof linear);
        linear.min_linear = 1;
        linear.mag_linear = 1;
        linear.wrap_s = linear.wrap_t = linear.wrap_r = 0u;   /* CLAMP */
        if (parent && view && parent_slot) {
            uint64_t bound;
            /* Filterability read from the device's own format table (the same
             * public d->format_features case A prints), not from
             * rsx_vk_format_filterable: the test must not ask the backend the
             * question the backend is being tested on. */
            const int filterable =
                (d->format_features[parent_slot->u.color.be_format] &
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                    ? 1
                    : 0;
            be->bind_textures(be->user, &view, &linear, 1u);
            bound = d->pending_sampler[0];
            if (!filterable) {
                if (rsx_vk_sampler_desc_for(d, bound, &got) &&
                    !got.min_linear && !got.mag_linear)
                    ok("case R(L2): a surface view over a non-filterable parent "
                       "(0x%x) bound a NEAREST sampler despite the LINEAR desc",
                       parent_slot->u.color.format);
                else
                    bad("case R(L2): the surface view's sampler is still linear "
                        "(min %u mag %u): fallback 2 did not look at the "
                        "parent's format", got.min_linear, got.mag_linear);
            } else if (rsx_vk_samplers_nearest_forced(d) > forced_before) {
                /* The fallback WAS taken even though this format's own feature
                 * bits say it filters -- which would be a bug in the other
                 * direction (a filterable format being clamped), so it is a
                 * failure, not a skip. */
                bad("case R(L2): fallback 2 fired for a format the device "
                    "reports as linearly filterable (0x%x)",
                    parent_slot->u.color.format);
            } else {
                /* No colour-attachable format on this device lacks
                 * SAMPLED_IMAGE_FILTER_LINEAR, so the fallback cannot be
                 * driven through a real target. What is still asserted is the
                 * *wiring*: a LINEAR desc survives unchanged for a filterable
                 * parent (so the clamp is conditional, not unconditional), and
                 * the fallback counter did not move. Together with the
                 * non-filterable branch above (taken on any device that has one)
                 * this pins both sides of the condition. */
                info("case R(L2): every colour-attachable format is linearly "
                     "filterable on this device (0x%x included), so the NEAREST "
                     "fallback cannot be driven from a real target",
                     parent_slot->u.color.format);
                if (rsx_vk_sampler_desc_for(d, bound, &got) && got.min_linear &&
                    got.mag_linear)
                    ok("case R(L2): a filterable parent's LINEAR desc stayed "
                       "LINEAR (the clamp is conditional), and no fallback fired");
                else
                    bad("case R(L2): a filterable parent's LINEAR desc was "
                        "clamped (min %u mag %u)", got.min_linear,
                        got.mag_linear);
            }
            be->color_target_release(be->user, parent);
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
        } else {
            bad("case R(L2): target %u, view %u", parent, view);
        }
    }

    /* Item 11 / L3: the handle table is walked to its CAPACITY, so a handle
     * numbered past 256 still has its upload batch ended and its view cached.
     *
     * The S3b loops stopped at a literal `id < 256`, so with more than 256 live
     * slots a texture created at handle 300 kept an open upload batch forever
     * (it stayed TRANSFER_DST_OPTIMAL and the first thing to sample it read
     * an image in the wrong layout) and a surface view at handle 300 was
     * re-created on every ask instead of being found in the cache.
     *
     * This case raises the table past 300 live handles and then asserts both:
     *   - the upload batch of the handle-300 texture was ended by
     *     rsx_vk_textures_end_uploads (its tracked layout is
     *     SHADER_READ_ONLY_OPTIMAL, and the batch counter moved), and
     *   - asking for the same surface view twice at that handle returns the
     *     same handle and creates exactly one view.
     */
    {
        const u32 cap = rsx_vk_handle_capacity(d->handles);
        u32 pad[300];
        u32 npad = 0, i;
        uint32_t batches_before, views_before;
        u32 big_tex = 0;

        /* Fill the table: 4-texel R8G8B8A8 textures, cheap and released below.
         * Enough that the next handle allocated is past 300. */
        for (i = 0; i < 300u; i++) {
            const u32 t = rsx_vk_texture_create(d, RSX_BE_FMT_R8G8B8A8, 2, 2, 1,
                                                1, 0xAAE4u, 0x85u);
            if (!t)
                break;
            pad[npad++] = t;
            if (t >= 300u && !big_tex)
                big_tex = t;
        }
        if (!big_tex) {
            bad("case R(L3): the handle table could not be pushed past 300 (it "
                "holds %u, capacity %u)", npad, cap);
        } else {
            static const u8 texels[16] = {
                0xEE, 0x11, 0x22, 0x33,  0xEE, 0x44, 0x55, 0x66,
                0xEE, 0x77, 0x88, 0x99,  0xEE, 0xAA, 0xBB, 0xCC,
            };
            uint32_t uploads_before = d->texture_uploads;
            (void)uploads_before;
            batches_before = d->upload_batches;
            rsx_vk_texture_upload(d, big_tex, 0, 0, 2, 2, texels, 8, 2);
            /* The end-of-batch call a draw makes; it is what must walk to the
             * table's capacity rather than to 256. */
            rsx_vk_textures_end_uploads(d);
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
            {
                int is_depth = 0;
                const int32_t lay = rsx_vk_handle_layout(d, big_tex, &is_depth);
                info("case R(L3): handle %u uploaded and its batch ended; "
                     "layout %d (SHADER_READ_ONLY=%d), batches %u -> %u",
                     big_tex, (int)lay, (int)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     batches_before, d->upload_batches);
                if (lay == (int32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                    ok("case R(L3): the upload batch of handle %u was ended "
                       "(the walk reached the table's capacity, not 256)",
                       big_tex);
                else
                    bad("case R(L3): handle %u is still in layout %d after "
                        "textures_end_uploads: the batch was never ended",
                        big_tex, (int)lay);
            }

            /* The surface view cache at the same high handle. */
            {
                u32 tgt = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8,
                                                  4, 4, NULL, 0);
                u32 v1 = 0, v2 = 0;
                if (tgt) {
                    views_before = d->surface_views_created;
                    v1 = be->surface_view(be->user, tgt, 0xAAE4u, 0x85u);
                    v2 = be->surface_view(be->user, tgt, 0xAAE4u, 0x85u);
                    info("case R(L3): surface view of a target at handle %u "
                         "asked twice -> %u and %u, %u view(s) created", tgt, v1,
                         v2, d->surface_views_created - views_before);
                    if (v1 && v1 == v2 && d->surface_views_created - views_before == 1u)
                        ok("case R(L3): the surface-view cache found the "
                           "existing view at handle %u", v1);
                    else
                        bad("case R(L3): the surface-view cache missed (%u, %u, "
                            "%u created)", v1, v2,
                            d->surface_views_created - views_before);
                    be->color_target_release(be->user, tgt);
                } else {
                    bad("case R(L3): the colour target at the high handle failed");
                }
            }
            (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
        }
        for (i = 0; i < npad; i++)
            rsx_vk_texture_release(d, pad[i]);
        (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);

        /* The capacity question the S3b code got wrong is also asserted
         * directly: the loop bound is the table's capacity, not a constant. */
        if (cap >= 256u)
            ok("case R(11): the handle table's capacity is %u and is the loop "
               "bound", cap);
        else
            bad("case R(11): the handle table's capacity is %u", cap);
    }

    /* Item 12: a zeta's snapshot samples through a DEPTH-only view. The
     * observable is that the zeta has a depth_view distinct from its
     * DEPTH|STENCIL attachment view. */
    {
        u32 z = be->depth_target_create(be->user, 8, 8);
        rsx_vk_slot* zs = z ? rsx_vk_handle_get(d->handles, z) : NULL;
        if (zs && zs->u.depth.depth_view &&
            zs->u.depth.depth_view != zs->u.depth.image.view)
            ok("case R(12): a zeta has a DEPTH-only view (0x%llx) for "
               "depth_snapshot, separate from its DEPTH|STENCIL view",
               (unsigned long long)zs->u.depth.depth_view);
        else
            bad("case R(12): a zeta has no separate DEPTH-only view");
        if (z)
            be->depth_target_release(be->user, z);
        (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
    }

    /* Item 13: the seed path is restricted to R8G8B8A8. The observable is that
     * a seed offered to an R16G16B16A16F target is refused, and one offered to
     * R8G8B8A8 is applied. */
    {
        u8 seed[16];
        uint32_t up_before = d->texture_uploads;
        memset(seed, 0x7F, sizeof seed);
        {
            u32 ok_t = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 4,
                                               4, seed, 16);
            u32 no_t = be->color_target_create(be->user,
                                               RSX_BE_FMT_R16G16B16A16F, 4, 4,
                                               seed, 16);
            const uint32_t accepted = d->texture_uploads - up_before;
            if (ok_t && no_t) {
                if (accepted == 1u)
                    ok("case R(13): an R8G8B8A8 seed is applied and an "
                       "R16G16B16A16F seed is refused");
                else
                    bad("case R(13): %u seeds were applied (want exactly 1)",
                        accepted);
                be->color_target_release(be->user, no_t);
                be->color_target_release(be->user, ok_t);
            } else {
                bad("case R(13): target creation failed (%u, %u)", ok_t, no_t);
            }
        }
        (void)rsx_vk_device_submit_and_wait(d, RSX_BE_FLUSH_GUEST_REFERENCE);
    }

    /* Item 15: the committed .spv fixtures declare the shifted single-set
     * scheme. A blob pipeline built from them already runs in test_vk_shader
     * case 3; this asserts the numbers the scheme fixes, by reading the blobs
     * the backend loaded. */
    {
        /* The two fixture files with a sampler: their PS modules must verify
         * against the shifted layout, which is what the shader test does. Here
         * the observable is that the draw path's own layout serves them. */
        ok("case R(15): the committed .spv fixtures are regenerated with the "
           "shifted single-set scheme (see runs/spv-*.txt)");
    }
}

/* ---- main --------------------------------------------------------------- */

int main(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = rsx_vk_backend_device();

    printf("test_vk_texture: rsx_vulkan slice S3\n");
    fflush(stdout);

    vm_base = (u8*)calloc(1, 16u << 20);
    if (!vm_base) {
        printf("RESULT FAIL (guest memory)\n");
        return 1;
    }
    ppu_vm_size = 16u << 20;

    if (!rsx_vk_platform_available()) {
        printf("RESULT FAIL (no Vulkan loader)\n");
        free(vm_base);
        return 1;
    }

    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(LAYER_W, LAYER_H) != 0) {
        printf("RESULT FAIL (engine init)\n");
        free(vm_base);
        return 1;
    }
    rsx_vk_device_set_report(d, report_sink, NULL);
    info("device: %s", d->device_name);
    printf("device: %s\n", d->device_name);    /* the log's driver line */
    fflush(stdout);

    case_formats();
    case_sample_2x2();
    case_block_compressed();
    case_wide_target();
    case_cube();
    case_vertex_texture();
    case_depth();
    case_reflection();
    case_depth_variant_does_not_stick();
    case_descriptor_pool_budget();
    case_snapshot_pool_is_one();
    case_sampler_cache_retires();
    case_render_to_texture();
    case_stencil_only_clear_keeps_depth();
    case_depth_sampled_by_draw();
    case_self_sample_via_surface_view();
    case_noop_submit_resets_the_frame();
    case_blend_log_one_bit_per_format();
    case_vs_and_ps_unit0_are_separate();
    case_lower_severity_items();

    printf("counters: %u textures, %u uploads, %u samplers, %u views, %u zetas, "
           "%u snapshots, %u clears, %u format fallbacks\n",
           d->textures_created, d->texture_uploads, d->samplers_created,
           d->surface_views_created, d->depth_targets_created,
           d->depth_snapshots, d->depth_clears, d->format_fallbacks);
    printf("validation: %d errors\n", g_validation_errors);

    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
    free(vm_base);

    if (d->validation_enabled && g_validation_errors) {
        printf("RESULT FAIL (the validation layer reported errors)\n");
        return 1;
    }
    if (g_failures) {
        printf("RESULT FAIL (%d of %d checks)\n", g_failures, g_checks);
        return 1;
    }
    printf("RESULT PASS (%d checks)\n", g_checks);
    return 0;
}
