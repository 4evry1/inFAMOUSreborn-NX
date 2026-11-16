/*
 * test_vk_shader - the slice S2 acceptance test (DESIGN §7 S2, R5, §6.3
 * cases 2 and 3)
 *
 * Three groups of checks, all on lavapipe (forcing the ICD is the caller's
 * job; see the Makefile's header):
 *
 *   1. Shaders. For each program pair built from rsx_test_programs.h (VP MOV /
 *      FP MOV, FP TEX, FP CUBE, FP MRT, and the TXL vertex-texture VP), the
 *      microcode is decompiled the way the engine does it, translated by our
 *      own glslang wrapper (rsx_vk_shader.cpp), and asserted to parse, to have
 *      an entry point named `main`, to declare only descriptor bindings that
 *      DESIGN §2.2 gives the backend a layout for, and -- R5 -- to carry the
 *      same interface locations on the VS's outputs as on the PS's inputs.
 *
 *   2. Pipelines. One pipeline per program pair through the vtable's
 *      pipeline_create, asserting a non-zero handle; rsx_vk_pipeline_create
 *      returns 0 and logs the VkResult on any VK_ERROR_*, so a handle is the
 *      "no VK_ERROR_*" assertion.
 *
 *   3. Draws through rsx_draw_engine_method, pushing NV4097 methods exactly as
 *      tests/test_rsx_draw_engine.c does, with a readback of the centre pixel
 *      compared to the colour the program passes through:
 *        case 2 - the guest programs are resident in the register file, so the
 *                 decompile -> translate -> compile path produces the pixels;
 *        case 3 - the same draw with both modules taken from committed .spv
 *                 blobs under tests/assets/, the offline-cache path of §2.5.
 *
 * Exit 0 only when every check passed.
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
static int g_validation_errors;
static int g_validation_warnings;
static int g_checks;

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
    else if (severity == 1)
        g_validation_warnings++;
}

/* ---- the programs ------------------------------------------------------- */

typedef struct program {
    const char* name;
    char vs[65536];
    char ps[65536];
    int  built;               /* vs/ps hold HLSL */
    int  want_targets;        /* SV_TARGETn count the PS declares */
    /* guest microcode, for the draw cases */
    u8   vp_ucode[64];
    u32  vp_words;
    u8   fp_ucode[64];
    u32  fp_bytes;
    u32  fp_ctrl;
    u32  cube_mask;
    char vs_blob[256];        /* file name under tests/assets */
    char ps_blob[256];
} program;

static char g_hlsl[65536];

static void prog_vp_mov(program* p)
{
    rsx_test_vp_mov_out(p->vp_ucode + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(p->vp_ucode + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);
    p->vp_words = 8;
}

static void prog_fp_mov(program* p)
{
    p->fp_bytes = 16;
    rsx_test_fp_mov_r0_col0(p->fp_ucode, RSX_TEST_FP_SWZ_IDENT);
}

static void prog_fp_tex(program* p)
{
    p->fp_bytes = 16;
    rsx_test_fp_tex_r0_tc0(p->fp_ucode, 0);
}

static void prog_fp_mrt(program* p)
{
    p->fp_bytes = 32;
    rsx_test_fp_mrt_col0(p->fp_ucode, RSX_TEST_FP_SWZ_IDENT,
                         RSX_TEST_FP_SWZ(2, 1, 0, 3));
}

/* Decompile with the same entry points the engine uses (eng_pipeline_get,
 * rsx_draw_engine.c:1013-1099): compact VP inputs from the layout mask and the
 * buffered FP form. */
static int prog_build(program* p)
{
    rsx_vertex_layout_plan layout;
    rsx_vp_input_analysis analysis;
    int vi, fi;
    u32 nconst = 0;

    rsx_vertex_layout_plan_init(&layout, 0xFFFFu);
    analysis.input_mask = 0xFFFFu;
    analysis.exact = 0;
    if (p->vp_words &&
        rsx_vp_analyze_inputs(p->vp_ucode, p->vp_words * 16u, &analysis) ==
            (int)p->vp_words &&
        analysis.exact && analysis.input_mask)
        rsx_vertex_layout_plan_init(&layout, analysis.input_mask);

    vi = rsx_vp_decompile_compact_ex(p->vp_ucode, p->vp_words * 16u, 0,
                                     layout.mask, g_hlsl, sizeof g_hlsl);
    if (vi <= 0) {
        bad("%s: the VP does not decompile", p->name);
        return -1;
    }
    memcpy(p->vs, g_hlsl, sizeof p->vs);

    fi = rsx_fp_decompile_buffered_ex(p->fp_ucode, p->fp_bytes, p->fp_ctrl,
                                      p->cube_mask, g_hlsl, sizeof g_hlsl,
                                      &nconst);
    if (fi <= 0) {
        bad("%s: the FP does not decompile", p->name);
        return -1;
    }
    memcpy(p->ps, g_hlsl, sizeof p->ps);
    p->built = 1;
    return 0;
}

/* ---- SPIR-V translation + verification --------------------------------- */

static int translate(const char* what, const char* hlsl, int stage,
                     rsx_vk_spv* out)
{
    char log[2048];
    if (rsx_vk_hlsl_to_spirv(hlsl, stage, out, log, sizeof log) != 0) {
        bad("%s: HLSL -> SPIR-V failed: %s", what, log);
        return -1;
    }
    if (!rsx_vk_spv_has_main(out)) {
        bad("%s: no entry point named `main`", what);
        return -1;
    }
    return 0;
}

static void verify_bindings(const char* what, const rsx_vk_spv* spv, int stage)
{
    char log[2048];
    const int n = rsx_vk_spv_verify(spv, stage, log, sizeof log);
    if (n < 0)
        bad("%s: %s", what, log);
    else
        ok("%s: %d descriptor binding(s) match DESIGN 2.2", what, n);
}

/* R5, precisely as the decompilers make it:
 *
 * The VP decompiler emits every varying (POSITION, COLOR0, COLOR1, FOG,
 * TEXCOORD0..7), and the FP decompiler emits the same struct but only keeps the
 * members its program reads -- glslang's HLSL legalization then drops the
 * unused PS inputs, which is why a PS has one input where the VS has eleven.
 *
 * What has to hold for Vulkan, and what fails here rather than as a wrong
 * frame, is that every location the PS *does* consume is one the VS produces,
 * and that the pairing is semantic: the PS member named "col0" must be the VS
 * output at the location the VS gave COLOR0. Both sides number by declaration
 * order through mapIO, so a divergence between the two structs is exactly the
 * failure this check is for.
 */
static void check_varying_match(const char* what, const rsx_vk_spv* vs,
                                const rsx_vk_spv* ps)
{
    int32_t vo[32], po[32];
    const int vn = rsx_vk_spv_io_locations(vs, RSX_VK_STAGE_VERTEX, vo, 32);
    const int pn = rsx_vk_spv_io_locations(ps, RSX_VK_STAGE_FRAGMENT, po, 32);
    int i, j, missing = -1;
    for (i = 0; i < pn && missing < 0; i++) {
        int found = 0;
        for (j = 0; j < vn; j++)
            if (vo[j] == po[i])
                found = 1;
        if (!found)
            missing = po[i];
    }
    if (missing >= 0) {
        bad("%s: the PS consumes location %d, which the VS does not produce "
            "(DESIGN R5)", what, missing);
        printf("       VS out (%d):", vn);
        for (i = 0; i < vn; i++) printf(" %d", vo[i]);
        printf("\n       PS in  (%d):", pn);
        for (i = 0; i < pn; i++) printf(" %d", po[i]);
        printf("\n");
    } else {
        ok("%s: every PS input location is one the VS produces (PS %d of "
           "VS %d varyings)", what, pn, vn);
    }
}

/* The PS's declared SV_TARGETn count, checked against DESIGN R6. */
static void module_check(rsx_vk_device* d, const char* what, program* p)
{
    int targets = 0;
    if (!rsx_vk_module_get(d, p->vs, RSX_VK_STAGE_VERTEX, NULL))
        bad("%s: VS module from HLSL", what);
    else
        ok("%s: VS module from HLSL", what);
    if (!rsx_vk_module_get(d, p->ps, RSX_VK_STAGE_FRAGMENT, &targets)) {
        bad("%s: PS module from HLSL", what);
        return;
    }
    ok("%s: PS module from HLSL", what);
    if (targets != p->want_targets)
        bad("%s: PS declares %d colour target(s), expected %d", what, targets,
            p->want_targets);
}

static u32 pipeline_for(rsx_vk_device* d, program* p)
{
    rsx_be_render_state rs;
    rsx_vertex_layout_plan layout;
    memset(&rs, 0, sizeof rs);
    rs.depth_func = 0x207u;   /* CELL_GCM ALWAYS */
    rs.color_mask = 0xFFFFFFFFu;
    rsx_vertex_layout_plan_init(&layout, 0xFFFFu);
    return rsx_vk_pipeline_create(d, p->vs, p->ps, &rs, &layout, layout.stride,
                                  RSX_BE_FMT_R8G8B8A8,
                                  (u32)p->want_targets);
}

/* ---- driving the engine ------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_A  0x0194
#define M_COLOR_A_OFFSET       0x0210
#define M_SURFACE_CLIP_H       0x0200
#define M_SURFACE_FORMAT       0x0208   /* SET_SURFACE_FORMAT       */
#define M_COLOR0_PITCH         0x020C   /* SET_SURFACE_PITCH        */
#define M_SURFACE_CLIP_V       0x0204
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
#define FP_UCODE_OFFSET        0x00020000u

#define DMA_LOCAL  0xFEED0000u
#define VTX_OFFSET 0x00010000u
#define LAYER_W    64u
#define LAYER_H    64u

/* The colour the FP passes through is the vertex colour ATTR3 (COLOR0),
 * which the MOV VP writes to o1. Written as exact RGBA8 so the centre-pixel
 * comparison is exact, not a tolerance. */
static const u8 k_vertex_colour[4] = { 0x20, 0xA0, 0xE0, 0xFF };

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

/* CELL_GCM_VERTEX_F is float32 in guest (big-endian) memory
 * (rsx_vertex_compact.c:97-99), so the stream is written byte-swapped the way
 * tests/test_rsx_draw_engine.c's main writes its three vertices. */
static void put_f32_be(u8* p, float f)
{
    u32 w;
    memcpy(&w, &f, 4);
    p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
}

static void write_vertices(void)
{
    /* One full-screen triangle already in NDC: the MOV VP only copies ATTR0
     * to o0, and the engine's clip-space epilogue is the identity for these
     * constants (see upload_identity_transform), so the triangle covers the
     * whole 64x64 layer and the centre pixel is inside it. */
    static const float tri[3][4] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        {  3.0f, -1.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, 1.0f },
    };
    /* The CPU fetch hands the backend a *compact* vertex, not 16 float4s: the
     * layout plan keeps only the attributes the vertex program reads, in
     * ascending register order (rsx_vertex_compact.h:22-28), so slot i holds
     * attrs[i] at offset i*16 and the stride is count*16. For this VP that is
     * ATTR0 at 0 and ATTR3 at 16, stride 32 -- DESIGN §2.4's trap.
     *
     * The guest stream is 16 float4s per vertex at the declared stride; the
     * fetch reads the two it needs out of it. */
    /* The guest stream is what the surface's arrays declare: VTXFMT gives each
     * attribute stride 64 (4 float4s), and ATTR3 starts at byte 48 of it, so a
     * vertex is 64 bytes and ATTR0/ATTR3 are the first and fourth float4. */
    const size_t src_stride = 64u;
    u8* base = vm_base + VTX_OFFSET;
    int v, a;
    for (v = 0; v < 3; v++) {
        for (a = 0; a < 16; a++) {
            float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            if (a == 0) {
                memcpy(f, tri[v], sizeof f);
            } else if (a == 3) {
                f[0] = k_vertex_colour[0] / 255.0f;
                f[1] = k_vertex_colour[1] / 255.0f;
                f[2] = k_vertex_colour[2] / 255.0f;
                f[3] = k_vertex_colour[3] / 255.0f;
            }
            if (a == 0 || a == 3) {
                u8* dst = base + (size_t)v * src_stride + (size_t)a * 16u;
                int c;
                for (c = 0; c < 4; c++)
                    put_f32_be(dst + c * 4u, f[c]);
            }
        }
    }
}

static void engine_setup(void)
{
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, LAYER_W << 16);
    m(M_SURFACE_CLIP_V, LAYER_H << 16);
    /* SET_SURFACE_FORMAT 0 = A8R8G8B8 (rsx_dispatch.c:451,
     * rsx_draw_engine.c's eng_surface_format), and a 4-byte pitch. The engine
     * decodes the surface from these, so a draw without them has no target. */
    m(M_SURFACE_FORMAT, 0x0u);
    m(M_COLOR0_PITCH, LAYER_W * 4u);
    m(M_COLOR_A_OFFSET, 0x40000u);
    /* Each RSX attribute has its own format and its own base offset
     * (rsx_dsp_get_vertex_attr, rsx_dispatch.c:497-510), so the two this
     * vertex program reads are declared separately: ATTR0 at the array base
     * and ATTR3 16 bytes into the same 64-byte vertex. bit 31 of the offset is
     * the location, and 1 is main memory. */
    m(M_VTXFMT + 0x0 * 4u, 2u | (4u << 4) | (64u << 8));   /* f32x4, stride 64 */
    m(M_VTXBUF_OFFSET + 0x0 * 4u, 0x80000000u | (VTX_OFFSET + 0u));
    m(M_VTXFMT + 0x3 * 4u, 2u | (4u << 4) | (64u << 8));
    m(M_VTXBUF_OFFSET + 0x3 * 4u, 0x80000000u | (VTX_OFFSET + 48u));
    m(M_SHADER_CONTROL, 0x40u);   /* 32-bit exports: r0 is target A */
    write_vertices();
}

/* Upload a transform program the way the FIFO does: VP_UPLOAD_FROM_ID names
 * the first slot, then 0x0B80 + lane*4 delivers word lane of a vec4 and the
 * hardware's own load pointer advances after the fourth (rsx_dispatch.c:328). */
static void upload_vp(const u8* ucode, u32 words)
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

/* Transform constants 0..3 as an identity matrix, so the built-in VP (which
 * reads them as its rows) passes the position through and the vertex program's
 * own output reaches the rasteriser unchanged. */
static void upload_identity_transform(void)
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

/* Put a guest fragment program in guest memory and point SHADER_PROGRAM at it,
 * so the engine builds the guest pair rather than its built-in one: it only
 * takes the guest path when BOTH programs are resident
 * (rsx_draw_engine.c:935, `return vp_instrs && fp_size`).
 *
 * SHADER_PROGRAM's low two bits are the location and 2 means RSX_LOCATION_MAIN
 * (rsx_dispatch.c:576-582), while M_FP_ACTIVE_PROGRAM is masked to
 * `v & ~3u` for the offset -- so passing 0 here would be "no program", which is
 * exactly the distinction the engine's comment at :916 draws. */
static void upload_fp(const u8* ucode, u32 bytes)
{
    memcpy(vm_base + FP_UCODE_OFFSET, ucode, bytes);
    m(M_FP_ACTIVE_PROGRAM, (FP_UCODE_OFFSET & ~3u) | 2u);
}

static void draw_triangle(void)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

/* The engine's surface table is private, so the handle it created is found in
 * the backend's own slot table -- the same object the engine holds. */
static u32 first_colour_target(rsx_vk_device* d)
{
    u32 id;
    for (id = 1; id < 256; id++) {
        rsx_vk_slot* slot = rsx_vk_handle_get(d->handles, id);
        if (slot && slot->kind == RSX_VK_SLOT_COLOR_TARGET)
            return id;
    }
    return 0;
}

static int read_centre(rsx_vk_device* d, const rsx_draw_backend* be, u32 handle,
                       u8 out[4])
{
    (void)d;
    memset(out, 0xEE, 4);
    be->readback(be->user, handle, LAYER_W / 2u, LAYER_H / 2u, 1, 1, out, 4);
    return 0;
}

static int pixel_matches(const u8 px[4], const u8 want[4])
{
    int i;
    for (i = 0; i < 4; i++) {
        const int got = px[i];
        const int ref = want[i];
        if (got < ref - 2 || got > ref + 2)
            return 0;
    }
    return 1;
}

/* ---- the fixtures ------------------------------------------------------- */

static u8* read_file(const char* path, size_t* out_size)
{
    FILE* f = fopen(path, "rb");
    long n;
    u8* buf;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (u8*)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int progs_init(program* progs)
{
    int np = 0;
    {
        program* p = &progs[np++];
        p->name = "VP MOV / FP MOV";
        p->want_targets = 1;
        prog_vp_mov(p);
        prog_fp_mov(p);
        p->fp_ctrl = 0x40u;
        strcpy(p->vs_blob, "tri.vert.spv");
        strcpy(p->ps_blob, "tri.frag.spv");
    }
    {
        program* p = &progs[np++];
        p->name = "FP TEX";
        p->want_targets = 1;
        prog_vp_mov(p);
        prog_fp_tex(p);
        p->fp_ctrl = 0x40u;
        strcpy(p->vs_blob, "tex.vert.spv");
        strcpy(p->ps_blob, "tex.frag.spv");
    }
    {
        program* p = &progs[np++];
        p->name = "FP CUBE";
        p->want_targets = 1;
        prog_vp_mov(p);
        prog_fp_tex(p);
        p->cube_mask = 1u;    /* unit 0 is a cube map */
        p->fp_ctrl = 0x40u;
        strcpy(p->vs_blob, "cube.vert.spv");
        strcpy(p->ps_blob, "cube.frag.spv");
    }
    {
        program* p = &progs[np++];
        p->name = "VP TXL";
        p->want_targets = 1;
        rsx_test_vp_mov_out(p->vp_ucode + 0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_txl_out(p->vp_ucode + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 1);
        p->vp_words = 8;
        prog_fp_mov(p);
        p->fp_ctrl = 0x40u;
        strcpy(p->vs_blob, "txl.vert.spv");
        strcpy(p->ps_blob, "txl.frag.spv");
    }
    {
        program* p = &progs[np++];
        p->name = "FP MRT";
        p->want_targets = 2;
        prog_vp_mov(p);
        prog_fp_mrt(p);
        p->fp_ctrl = 0x40u;
        strcpy(p->vs_blob, "mrt.vert.spv");
        strcpy(p->ps_blob, "mrt.frag.spv");
    }

    return np;
}

/* ---- the bake mode: write the committed .spv fixtures ------------------ *
 *
 * The task asks for the case-3 blobs to be "generated by our wrapper" and the
 * commands recorded in REPORT.md. So the test itself can bake them: with
 * --bake <dir> it decompiles each program, translates it with the same
 * rsx_vk_hlsl_to_spirv a pipeline uses, and writes <dir>/<name>.{vert,frag}.spv.
 * That is one binary, one wrapper, and no second tool to drift.
 */
static int bake_fixtures(program* progs, int np, const char* dir)
{
    int i;
    for (i = 0; i < np; i++) {
        program* p = &progs[i];
        rsx_vk_spv vs, ps;
        char path[1024];
        FILE* f;

        if (prog_build(p) != 0)
            return -1;
        if (rsx_vk_hlsl_to_spirv(p->vs, RSX_VK_STAGE_VERTEX, &vs, NULL, 0) != 0) {
            printf("FAIL bake %s VS\n", p->name);
            return -1;
        }
        if (rsx_vk_hlsl_to_spirv(p->ps, RSX_VK_STAGE_FRAGMENT, &ps, NULL, 0) != 0) {
            printf("FAIL bake %s PS\n", p->name);
            return -1;
        }
        snprintf(path, sizeof path, "%s/%s", dir, p->vs_blob);
        if (!(f = fopen(path, "wb")) ||
            fwrite(vs.words, 4, vs.word_count, f) != vs.word_count) {
            printf("FAIL write %s\n", path);
            return -1;
        }
        fclose(f);
        printf("baked %s (%u bytes)\n", path, vs.word_count * 4u);
        snprintf(path, sizeof path, "%s/%s", dir, p->ps_blob);
        if (!(f = fopen(path, "wb")) ||
            fwrite(ps.words, 4, ps.word_count, f) != ps.word_count) {
            printf("FAIL write %s\n", path);
            return -1;
        }
        fclose(f);
        printf("baked %s (%u bytes)\n", path, ps.word_count * 4u);
        rsx_vk_spirv_free(&vs);
        rsx_vk_spirv_free(&ps);
    }
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char** argv)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = rsx_vk_backend_device();
    const char* asset_dir = (argc > 1) ? argv[1]
                                       : "native/adapters/rsx_vulkan/tests/assets";
    program progs[5];
    int np = 0, i;

    printf("test_vk_shader: rsx_vulkan slice S2\n");
    fflush(stdout);

    vm_base = (u8*)calloc(1, 16u << 20);
    if (!vm_base) {
        printf("RESULT FAIL (guest memory)\n");
        return 1;
    }
    ppu_vm_size = 16u << 20;

    memset(progs, 0, sizeof progs);
    np = progs_init(progs);

    if (argc > 1 && strcmp(argv[1], "--bake") == 0) {
        const char* dir = (argc > 2) ? argv[2] : "native/adapters/rsx_vulkan/tests/assets";
        const int rc = bake_fixtures(progs, np, dir);
        free(vm_base);
        return rc == 0 ? 0 : 1;
    }

    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(LAYER_W, LAYER_H) != 0) {
        printf("RESULT FAIL (engine init)\n");
        return 1;
    }
    rsx_vk_device_set_report(d, report_sink, NULL);
    info("device: %s (API %u.%u)", d->device_name,
         (d->api_version >> 22) & 0x7Fu, (d->api_version >> 12) & 0x3FFu);

    /* ---- group 1: decompile, translate, verify ------------------------- */
    for (i = 0; i < np; i++) {
        program* p = &progs[i];
        rsx_vk_spv vs, ps;
        if (prog_build(p) != 0)
            continue;
        if (translate(p->name, p->vs, RSX_VK_STAGE_VERTEX, &vs) != 0)
            continue;
        if (translate(p->name, p->ps, RSX_VK_STAGE_FRAGMENT, &ps) != 0) {
            rsx_vk_spirv_free(&vs);
            continue;
        }
        ok("%s: SPIR-V parses, `main` present (VS %u words, PS %u words)",
           p->name, vs.word_count, ps.word_count);
        verify_bindings(p->name, &vs, RSX_VK_STAGE_VERTEX);
        verify_bindings(p->name, &ps, RSX_VK_STAGE_FRAGMENT);
        check_varying_match(p->name, &vs, &ps);
        module_check(d, p->name, p);
        rsx_vk_spirv_free(&vs);
        rsx_vk_spirv_free(&ps);
    }

    /* ---- group 2: one pipeline per program ---------------------------- */
    for (i = 0; i < np; i++) {
        program* p = &progs[i];
        u32 handle;
        if (!p->built)
            continue;
        handle = pipeline_for(d, p);
        if (!handle)
            bad("%s: pipeline_create returned 0", p->name);
        else
            ok("%s: pipeline created (handle %u, %d colour target(s))",
               p->name, handle, p->want_targets);
    }

    /* ---- case 2: a real draw through the vtable ---------------------- */
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(LAYER_W, LAYER_H) != 0) {
        printf("RESULT FAIL (engine re-init)\n");
        return 1;
    }
    {
        program* p = &progs[0];
        u8 px[4];
        u32 handle;

        engine_setup();
        upload_vp(p->vp_ucode, p->vp_words);
        upload_fp(p->fp_ucode, p->fp_bytes);
        upload_identity_transform();
        /* Black first, so a draw that never reached the target is a distinct
         * wrong pixel rather than the image's undefined contents. */
        m(M_CLEAR_COLOR_VALUE, 0x00000000u);
        m(M_CLEAR_BUFFERS, 0xF0u);
        draw_triangle();
        rsx_draw_engine_flush();
        info("case 2: guest draws in the last frame: %u",
             rsx_draw_engine_guest_draws());

        handle = first_colour_target(d);
        if (!handle) {
            bad("case 2: the engine created no colour target");
        } else {
            read_centre(d, be, handle, px);
            info("case 2: centre pixel %u,%u,%u,%u (want %u,%u,%u,%u)", px[0],
                 px[1], px[2], px[3], k_vertex_colour[0], k_vertex_colour[1],
                 k_vertex_colour[2], k_vertex_colour[3]);
            if (pixel_matches(px, k_vertex_colour))
                ok("case 2: draw through the vtable reads back the program's "
                   "colour");
            else
                bad("case 2: centre pixel %u,%u,%u,%u != %u,%u,%u,%u", px[0],
                    px[1], px[2], px[3], k_vertex_colour[0],
                    k_vertex_colour[1], k_vertex_colour[2],
                    k_vertex_colour[3]);
        }
    }

    /* ---- case 3: the same draw from committed .spv blobs ------------- */
    {
        char path[1024];
        size_t vsn = 0, psn = 0;
        u8* vsb;
        u8* psb;

        snprintf(path, sizeof path, "%s/tri.vert.spv", asset_dir);
        vsb = read_file(path, &vsn);
        snprintf(path, sizeof path, "%s/tri.frag.spv", asset_dir);
        psb = read_file(path, &psn);
        if (!vsb || !psb) {
            bad("case 3: %s/tri.{vert,frag}.spv missing -- run the bake step in "
                "REPORT.md", asset_dir);
        } else {
            rsx_vk_spv vs, ps;
            char log[1024];
            if (rsx_vk_spirv_from_blob(vsb, vsn, &vs, log, sizeof log) != 0 ||
                rsx_vk_spirv_from_blob(psb, psn, &ps, log, sizeof log) != 0) {
                bad("case 3: %s", log);
            } else {
                ok("case 3: blobs parse (VS %u words, PS %u words)",
                   vs.word_count, ps.word_count);
                if (!rsx_vk_spv_has_main(&vs))
                    bad("case 3: the VS blob has no `main`");
                if (!rsx_vk_spv_has_main(&ps))
                    bad("case 3: the PS blob has no `main`");
                verify_bindings("case 3 VS blob", &vs, RSX_VK_STAGE_VERTEX);
                verify_bindings("case 3 PS blob", &ps, RSX_VK_STAGE_FRAGMENT);
                check_varying_match("case 3 blobs", &vs, &ps);
                rsx_vk_spirv_free(&vs);
                rsx_vk_spirv_free(&ps);
            }
            {
                const u32 handle = first_colour_target(d);
                u8 px[4];
                if (!handle) {
                    bad("case 3: the engine created no colour target");
                } else if (rsx_vk_spv_blob_draw(vsb, vsn, psb, psn, handle, NULL,
                                                0, 0, NULL, 0) != 0) {
                    bad("case 3: the blob draw could not be recorded");
                } else {
                    ok("case 3: draw recorded from committed .spv blobs");
                    read_centre(d, be, handle, px);
                    info("case 3: centre pixel %u,%u,%u,%u (want %u,%u,%u,%u)",
                         px[0], px[1], px[2], px[3], k_vertex_colour[0],
                         k_vertex_colour[1], k_vertex_colour[2],
                         k_vertex_colour[3]);
                    if (pixel_matches(px, k_vertex_colour))
                        ok("case 3: blob draw reads back the same colour");
                    else
                        bad("case 3: centre pixel %u,%u,%u,%u != %u,%u,%u,%u",
                            px[0], px[1], px[2], px[3], k_vertex_colour[0],
                            k_vertex_colour[1], k_vertex_colour[2],
                            k_vertex_colour[3]);
                }
            }
            free(vsb);
            free(psb);
        }
    }

    /* ---- summary ----------------------------------------------------- */
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);

    printf("validation: %d errors, %d warnings\n", g_validation_errors,
           g_validation_warnings);
    if (d->validation_enabled && (g_validation_errors || g_validation_warnings)) {
        printf("FAIL the validation layer reported messages\n");
        g_failures++;
    }
    printf("counters: %u pipelines created, %u failed, %u draws, %u modules, "
           "%u passes, %u SPIR-V verified, %u rejected\n",
           d->pipelines_created, d->pipelines_failed, d->draws_recorded,
           d->modules_created, d->passes_begun, d->spv_verified,
           d->spv_rejected);
    free(vm_base);
    if (g_failures) {
        printf("RESULT FAIL (%d of %d checks)\n", g_failures, g_checks);
        return 1;
    }
    printf("RESULT PASS (%d checks)\n", g_checks);
    return 0;
}
