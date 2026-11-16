/*
 * ps3recomp - Vulkan backend: HLSL -> SPIR-V, and the decoration verifier
 * (slice S2 of the Vulkan backend design)
 *
 * This is the only place in the backend that touches glslang, and it is the
 * only C++ file in it. ps3recomp's own HLSL -> SPIR-V step lives inside
 * rsx_shader_msl.cpp:61-101 and hands its vector straight to spirv-cross, so
 * it cannot be reached without a `.cpp` include. That
 * function's logic is therefore copied here member for member and is the
 * whole of rsx_vk_hlsl_to_spirv:
 *
 *   EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl
 *   setEnvInput(HLSL, lang, Vulkan, 100)
 *   setEnvClient(Vulkan, EShTargetVulkan_1_1)
 *   setEnvTarget(Spv, EShTargetSpv_1_3)
 *   setEntryPoint("main"), setAutoMapBindings(true), setAutoMapLocations(true)
 *   parse(GetDefaultResources(), 100, false, msgs)
 *   link(msgs), mapIO()
 *   SpvOptions{ disableOptimizer = false }   -- the HLSL legalization passes
 *   GlslangToSpv(...)
 *
 * Two additions, both verification rather than translation:
 *
 *  - rsx_vk_spv_verify walks OpDecorate's Binding / DescriptorSet pairs and
 *    the OpVariable storage classes and checks them against the descriptor
 *    set layouts the backend actually builds. A mismatch is a
 *    hard, loud failure on the test machine rather than a wrong frame on the
 *    device.
 *  - rsx_vk_spv_io_locations reports a stage's interface locations so the
 *    test can prove the VS's outputs and the PS's inputs agree (DESIGN R5:
 *    the two decompilers emit their structs independently and rely on mapIO
 *    numbering them identically).
 *
 * Nothing here creates a VkDevice object, so the verifier half runs on a host
 * without a GPU.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <string>
#include <vector>

/* glslang 16.6.0's install tree puts every public header under
 * include/glslang/ (its SPIRV/ and Public/ included), so the include root is
 * GLSLANG_PREFIX/include and these are the paths the installed tree has. */
/* Root (2026-11-12): -DRSX_VK_NO_GLSLANG builds this unit without the compiler (the Switch). The two translate
 * functions then fail with a clear reason, and every shader has to come from the on-disk SPIR-V cache
 * (rsx_vk_set_spv_cache_dir). The blob path and the verifier below need no compiler. */
#ifndef RSX_VK_NO_GLSLANG
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

/* The SPIR-V enumerators the verifier reads. glslang's install tree ships no
 * spirv/unified1/spirv.h (only spirv-tools' own C++ headers), so the handful
 * of numbers are named here. They are fixed by the SPIR-V core grammar for
 * good -- these values have not changed since 1.0 -- which is exactly why
 * DESIGN §2.2 says a ~120-line walk is enough and a full validator is not. */
enum {
    SPV_OP_NAME          = 5,
    SPV_OP_ENTRY_POINT   = 15,
    SPV_OP_VARIABLE      = 59,
    SPV_OP_DECORATE      = 71,
    SPV_DECO_LOCATION    = 30,
    SPV_DECO_BINDING     = 33,
    SPV_DECO_SET         = 34,
    SPV_DECO_BUILTIN     = 11,
    SPV_STORAGE_INPUT    = 1,
    SPV_STORAGE_UNIFORM_CONSTANT = 0,
    SPV_STORAGE_UNIFORM  = 2,
    SPV_STORAGE_OUTPUT   = 3,
    SPV_STORAGE_PRIVATE  = 6,
    SPV_STORAGE_FUNCTION = 7
};

#include "rsx_vk.h"

/* ---- HLSL -> SPIR-V (the copy of rsx_shader_msl.cpp:61-101) -------------- */

/*
 * glslang's process-wide state is initialised once. rsx_shader_msl.cpp keeps
 * the same latch (:44-58) because EShInitialize is not reentrant and a second
 * call leaks the default resources.
 *
 * EShMessages does not exist in glslang, so the flag word is rebuilt from
 * EShMessages' own enumerators, which are the four bits the MSL wrapper ORs
 * together (rsx_shader_msl.cpp:64).
 */
#ifndef RSX_VK_NO_GLSLANG
static bool g_inited = false;

static bool init_glslang(void)
{
    if (!g_inited) {
        glslang::InitializeProcess();
        g_inited = true;
    }
    return true;
}

#endif

static void put_log(char* log, size_t log_size, const char* what,
                    const char* detail)
{
    if (!log || !log_size)
        return;
    log[0] = '\0';
    snprintf(log, log_size, "%s: %s", what, detail ? detail : "(no detail)");
}

#ifndef RSX_VK_NO_GLSLANG
int rsx_vk_hlsl_to_spirv(const char* hlsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size)
{
    if (!out)
        return -1;
    out->words = NULL;
    out->word_count = 0;
    if (!hlsl) {
        put_log(log, log_size, "HLSL", "null source");
        return -1;
    }
    if (!init_glslang()) {
        put_log(log, log_size, "glslang", "EShInitialize failed");
        return -1;
    }

    const EShLanguage lang =
        (stage == RSX_VK_STAGE_VERTEX) ? EShLangVertex : EShLangFragment;
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules |
                                           EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan,
                       100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    /* HLSL's `t` and `s` registers are separate spaces, but glslang maps both
     * to the same (DescriptorSet 0, Binding N), which Vulkan cannot express:
     * a unit's image and its sampler would need two descriptor types at one
     * binding. glslang's own remedy is the sampler shift, which moves every
     * `s` register out of the `t` range; the backend then binds PS images at
     * 4..19 and PS samplers at 32..47 (rsx_vk_device.c's layouts_create).
     *
     * The *vertex* stage needs a different SAMPLER shift, and only a sampler
     * shift. glslang keeps a texture's `t` register number and adds the image
     * shift to it, so the VS's `register(t16)` already lands at 16+4 = 20, clear
     * of the PS's 4..19. Its `register(s0..s3)` lands at 0..3 + shift, which at
     * the PS shift of 32 is *exactly* PS s0..s3. Measured with spirv-dis;
     * REPORT.md records both. So the VS is given
     * RSX_VK_VS_SAMPLER_BINDING_SHIFT for its samplers (s registers -> 48..51),
     * and the same image shift as the PS for its textures (t registers -> 20..23,
     * which the layout names as RSX_VK_VS_IMAGE_BINDING_SHIFT). */
    if (stage == RSX_VK_STAGE_VERTEX) {
        shader.setShiftBinding(glslang::EResSampler,
                               RSX_VK_VS_SAMPLER_BINDING_SHIFT);
    } else {
        shader.setShiftBinding(glslang::EResSampler,
                               RSX_VK_SAMPLER_BINDING_SHIFT);
    }
    shader.setShiftBinding(glslang::EResTexture, RSX_VK_IMAGE_BINDING_SHIFT);
    shader.setAutoMapLocations(true);
    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return -1;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* the HLSL legalization passes */
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger,
                          &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation",
                logger.getAllMessages().c_str());
        return -1;
    }

    out->word_count = (uint32_t)spirv.size();
    out->words = (uint32_t*)malloc(spirv.size() * sizeof(unsigned int));
    if (!out->words) {
        out->word_count = 0;
        put_log(log, log_size, "SPIR-V", "out of memory");
        return -1;
    }
    memcpy(out->words, spirv.data(), spirv.size() * sizeof(unsigned int));
    if (log && log_size)
        log[0] = '\0';
    return 0;
}

/* ---- GLSL -> SPIR-V, for the backend's own fixed programs ---------------- */

/*
 * One program in the backend is not a guest program: depth_snapshot's
 * full-screen triangle. It is written in GLSL because it is
 * ours end to end -- no HLSL legalization passes belong in the path of a depth
 * read -- and glslang compiles both languages through the same interface. The
 * environment is the same Vulkan 1.1 / SPIR-V 1.3 floor as the HLSL path, so
 * the bytes are valid on every target this runs on.
 */
int rsx_vk_glsl_to_spirv(const char* glsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size)
{
    if (!out)
        return -1;
    out->words = NULL;
    out->word_count = 0;
    if (!glsl) {
        put_log(log, log_size, "GLSL", "null source");
        return -1;
    }
    if (!init_glslang()) {
        put_log(log, log_size, "glslang", "EShInitialize failed");
        return -1;
    }

    const EShLanguage lang =
        (stage == RSX_VK_STAGE_VERTEX) ? EShLangVertex : EShLangFragment;
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    glslang::TShader shader(lang);
    const char* strings[1] = { glsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan,
                       100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    shader.setEntryPoint("main");
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
        put_log(log, log_size, "GLSL parse", shader.getInfoLog());
        return -1;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "GLSL link", program.getInfoLog());
        return -1;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation",
                logger.getAllMessages().c_str());
        return -1;
    }
    out->word_count = (uint32_t)spirv.size();
    out->words = (uint32_t*)malloc(spirv.size() * sizeof(unsigned int));
    if (!out->words) {
        out->word_count = 0;
        put_log(log, log_size, "SPIR-V", "out of memory");
        return -1;
    }
    memcpy(out->words, spirv.data(), spirv.size() * sizeof(unsigned int));
    if (log && log_size)
        log[0] = '\0';
    return 0;
}

#else   /* RSX_VK_NO_GLSLANG */
int rsx_vk_hlsl_to_spirv(const char* hlsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size)
{
    (void)hlsl; (void)stage;
    if (out) { out->words = NULL; out->word_count = 0; }
    put_log(log, log_size, "no shader compiler in this build", "the shader is not in the SPIR-V cache");
    return -1;
}

int rsx_vk_glsl_to_spirv(const char* glsl, int stage, rsx_vk_spv* out,
                         char* log, size_t log_size)
{
    (void)glsl; (void)stage;
    if (out) { out->words = NULL; out->word_count = 0; }
    put_log(log, log_size, "no shader compiler in this build", "GLSL program not available");
    return -1;
}
#endif

/* ---- the blob path (DESIGN §2.5, the offline cache) --------------------- */

int rsx_vk_spirv_from_blob(const void* bytes, size_t byte_count,
                           rsx_vk_spv* out, char* log, size_t log_size)
{
    const uint32_t* words = (const uint32_t*)bytes;
    if (!out)
        return -1;
    out->words = NULL;
    out->word_count = 0;
    if (!bytes || byte_count < 20 || (byte_count % 4u) != 0) {
        put_log(log, log_size, "SPIR-V blob",
                "not a whole number of words, or shorter than a header");
        return -1;
    }
    if (words[0] != 0x07230203u) {
        put_log(log, log_size, "SPIR-V blob", "magic number is not 0x07230203");
        return -1;
    }
    out->word_count = (uint32_t)(byte_count / 4u);
    out->words = (uint32_t*)malloc(byte_count);
    if (!out->words) {
        out->word_count = 0;
        put_log(log, log_size, "SPIR-V blob", "out of memory");
        return -1;
    }
    memcpy(out->words, bytes, byte_count);
    if (log && log_size)
        log[0] = '\0';
    return 0;
}

void rsx_vk_spirv_free(rsx_vk_spv* spv)
{
    if (!spv)
        return;
    free(spv->words);
    spv->words = NULL;
    spv->word_count = 0;
}

/* ---- the SPIR-V walk shared by verify / io_locations / main ------------- *
 *
 * Deliberately the "~120-line SPIR-V header walk" of DESIGN §2.2 and nothing
 * more: no spirv-tools, no validator. What it has to know is the word layout
 * of a handful of opcodes, which the SPIR-V core grammar fixes for good.
 */

namespace {

struct Decoration {
    uint32_t target;      /* the <id> the decoration applies to */
    uint32_t kind;        /* SpvDecoration* */
    int32_t  value;       /* the single literal operand, when there is one */
};

struct InterfaceVar {
    uint32_t id;
    uint32_t storage;     /* SPV_STORAGE_* */
    int32_t  location;    /* -1 when undecorated */
    int32_t  builtin;     /* -1 when undecorated */
    char     name[48];    /* OpName, for telling SV_TARGETn from COLOR0 */
};

struct Name {
    uint32_t id;
    std::string text;
};

struct Module {
    std::vector<Decoration> decorations;
    std::vector<InterfaceVar> vars;
    std::vector<Name> names;
    bool has_main_name;   /* OpName <id> "main" */
    uint32_t version;
};

/* Name strings are NUL-terminated inside the instruction's trailing words. */
static std::string trailing_string(const uint32_t* ins, uint32_t first_word,
                                   uint32_t word_count)
{
    char buf[256];
    size_t n = 0;
    for (uint32_t w = first_word; w < word_count && n < sizeof(buf) - 1; w++) {
        const char* c = (const char*)&ins[w];
        for (int b = 0; b < 4 && n < sizeof(buf) - 1; b++) {
            buf[n++] = c[b];
            if (c[b] == '\0') {
                w = word_count;
                break;
            }
        }
    }
    buf[n] = '\0';
    return std::string(buf);
}

static bool module_parse(const rsx_vk_spv* spv, Module* m)
{
    if (!spv || !spv->words || spv->word_count < 5)
        return false;
    const uint32_t* w = spv->words;
    m->version = w[1];
    const uint32_t bound = w[3];
    (void)bound;

    uint32_t i = 5;
    while (i < spv->word_count) {
        const uint32_t opcode = w[i] & 0xFFFFu;
        const uint32_t count = w[i] >> 16;
        if (count == 0 || i + count > spv->word_count)
            return false;
        const uint32_t* ins = &w[i];

        switch (opcode) {
        case SPV_OP_DECORATE:
            /* <id> <decoration> [literal] */
            if (count >= 3) {
                Decoration d;
                d.target = ins[1];
                d.kind = ins[2];
                d.value = (count >= 4) ? (int32_t)ins[3] : 0;
                m->decorations.push_back(d);
            }
            break;
        case SPV_OP_VARIABLE:
            /* <result type> <result id> <storage class> [initialiser] */
            if (count >= 4) {
                InterfaceVar v;
                v.id = ins[2];
                v.storage = ins[3];
                v.location = -1;
                v.builtin = -1;
                v.name[0] = '\0';
                m->vars.push_back(v);
            }
            break;
        case SPV_OP_NAME:
            /* <target> "name" */
            if (count >= 3) {
                Name n;
                n.id = ins[1];
                n.text = trailing_string(ins, 2, count);
                if (n.text == "main")
                    m->has_main_name = true;
                m->names.push_back(n);
            }
            break;
        case SPV_OP_ENTRY_POINT:
            /* <execution model> <entry id> "name" */
            if (count >= 4) {
                const std::string name = trailing_string(ins, 3, count);
                if (name == "main")
                    m->has_main_name = true;
            }
            break;
        default:
            break;
        }
        i += count;
    }
    return true;
}

/* Fold the OpDecorate list and the OpName list onto the OpVariable list. */
static void module_apply(Module* m)
{
    for (size_t k = 0; k < m->names.size(); k++) {
        for (size_t v = 0; v < m->vars.size(); v++) {
            if (m->vars[v].id != m->names[k].id)
                continue;
            snprintf(m->vars[v].name, sizeof m->vars[v].name, "%s",
                     m->names[k].text.c_str());
        }
    }
    for (size_t k = 0; k < m->decorations.size(); k++) {
        const Decoration& d = m->decorations[k];
        for (size_t v = 0; v < m->vars.size(); v++) {
            if (m->vars[v].id != d.target)
                continue;
            if (d.kind == SPV_DECO_LOCATION)
                m->vars[v].location = d.value;
            else if (d.kind == SPV_DECO_BUILTIN)
                m->vars[v].builtin = d.value;
        }
    }
}

static int32_t decoration_of(const Module& m, uint32_t target, uint32_t kind)
{
    for (size_t k = 0; k < m.decorations.size(); k++)
        if (m.decorations[k].target == target && m.decorations[k].kind == kind)
            return m.decorations[k].value;
    return -1;
}

} /* namespace */

/* the verifier */

/*
 * The descriptor set layout the backend builds (rsx_vk_device.c's
 * layouts_create), expressed as "which (set, binding, storage class) triples
 * may appear". Everything is in set 0, because that is where glslang 16.6.0's
 * HLSL front end puts every resource variable of a stage, with the register
 * number as the binding:
 *
 *     set 0, binding 0, 1                  Uniform          -- b0, b1
 *     set 0, binding 4..19                 UniformConstant  -- PS t0..t15
 *     set 0, binding 32..47                UniformConstant  -- PS s0..s15
 *     set 0, binding 20..23                UniformConstant  -- VS t16..t19
 *     set 0, binding 48..51                UniformConstant  -- VS s0..s3
 *
 * The stage decides which image/sampler ranges are legal, because the two
 * stages are translated with different shifts (rsx_vk_hlsl_to_spirv says why:
 * glslang renumbers a stage's resources from zero, so `register(t16)` in a VP
 * would otherwise be binding 4, the same slot as PS unit 0's image).
 *
 * Both shifts are needed even within a stage: glslang keeps a cbuffer's bN as
 * its binding, so an unshifted t0/t1 would land on the same bindings as b0/b1,
 * and an unshifted s0 would land on t0.
 *
 * DESIGN §2.2 predicted images in set 1 and samplers in set 2; this is the
 * numbering the compiler actually produces, and REPORT.md records both the
 * prediction and the spirv-dis output that settles it. Anything outside these
 * ranges is a hard failure, which is the point of checking rather than
 * trusting.
 */
static bool binding_ok(int32_t set, int32_t binding, uint32_t storage,
                       int stage)
{
    const int is_vs = (stage == RSX_VK_STAGE_VERTEX);
    const int32_t img_lo = is_vs ? RSX_VK_VS_IMAGE_BINDING_SHIFT
                                 : RSX_VK_IMAGE_BINDING_SHIFT;
    const int32_t img_hi = img_lo + (is_vs ? RSX_VK_MAX_VTEX_UNITS
                                          : RSX_VK_MAX_TEX_UNITS);
    const int32_t smp_lo = is_vs ? RSX_VK_VS_SAMPLER_BINDING_SHIFT
                                 : RSX_VK_SAMPLER_BINDING_SHIFT;
    const int32_t smp_hi = smp_lo + (is_vs ? RSX_VK_MAX_VTEX_UNITS
                                          : RSX_VK_MAX_TEX_UNITS);
    if (set != 0 || binding < 0)
        return false;
    if (binding <= 1)
        return storage == SPV_STORAGE_UNIFORM;
    if (binding >= img_lo && binding < img_hi)
        return storage == SPV_STORAGE_UNIFORM_CONSTANT;
    if (binding >= smp_lo && binding < smp_hi)
        return storage == SPV_STORAGE_UNIFORM_CONSTANT;
    return false;
}

int rsx_vk_spv_verify(const rsx_vk_spv* spv, int stage, char* log,
                      size_t log_size)
{
    Module m;
    m.has_main_name = false;
    m.version = 0;
    if (!module_parse(spv, &m)) {
        put_log(log, log_size, "SPIR-V verify", "the module does not parse");
        return -1;
    }
    module_apply(&m);

    if (!m.has_main_name) {
        put_log(log, log_size, "SPIR-V verify", "no entry point named `main`");
        return -1;
    }

    int checked = 0;
    for (size_t v = 0; v < m.vars.size(); v++) {
        const InterfaceVar& var = m.vars[v];
        const int32_t set = decoration_of(m, var.id, SPV_DECO_SET);
        const int32_t binding = decoration_of(m, var.id, SPV_DECO_BINDING);
        if (var.storage == SPV_STORAGE_INPUT ||
            var.storage == SPV_STORAGE_OUTPUT ||
            var.storage == SPV_STORAGE_PRIVATE ||
            var.storage == SPV_STORAGE_FUNCTION)
            continue;
        if (binding < 0 && set < 0)
            continue;   /* a plain uniform the layout does not name */
        if (!binding_ok(set, binding, var.storage, stage)) {
            if (log && log_size)
                snprintf(log, log_size,
                         "SPIR-V verify: %s variable %u declares "
                         "DescriptorSet=%d Binding=%d, which the backend's "
                         "layout (DESIGN 2.2) does not have",
                         stage == RSX_VK_STAGE_VERTEX ? "vertex" : "fragment",
                         var.id, set, binding);
            return -1;
        }
        checked++;
    }
    if (log && log_size)
        log[0] = '\0';
    return checked;
}

int rsx_vk_spv_has_main(const rsx_vk_spv* spv)
{
    Module m;
    m.has_main_name = false;
    if (!module_parse(spv, &m))
        return 0;
    return m.has_main_name ? 1 : 0;
}

/* The stage's *input* locations: a VS's vertex attributes, or a PS's
 * varyings. Same walk as rsx_vk_spv_io_locations with the other storage
 * class, which is what calls that ask "what does this stage consume" want. */
int rsx_vk_spv_input_locations(const rsx_vk_spv* spv, int32_t* out, int cap)
{
    Module m;
    m.has_main_name = false;
    if (!module_parse(spv, &m))
        return 0;
    module_apply(&m);
    int n = 0;
    for (size_t v = 0; v < m.vars.size(); v++) {
        const InterfaceVar& var = m.vars[v];
        if (var.storage != SPV_STORAGE_INPUT || var.location < 0 ||
            var.builtin >= 0)
            continue;
        if (out && n < cap)
            out[n] = var.location;
        n++;
    }
    return n;
}

int rsx_vk_spv_io_locations(const rsx_vk_spv* spv, int stage,
                            int32_t* out, int cap)
{
    Module m;
    m.has_main_name = false;
    if (!module_parse(spv, &m))
        return 0;
    module_apply(&m);

    /* A PS's varyings arrive as Input; a VS's leave as Output. The builtins
     * (SV_Position, SV_IsFrontFace) carry a BuiltIn decoration and no
     * Location, which is exactly how they drop out here. */
    const uint32_t want = (stage == RSX_VK_STAGE_VERTEX)
                              ? (uint32_t)SPV_STORAGE_OUTPUT
                              : (uint32_t)SPV_STORAGE_INPUT;
    int n = 0;
    for (size_t v = 0; v < m.vars.size(); v++) {
        const InterfaceVar& var = m.vars[v];
        if (var.storage != want || var.location < 0 || var.builtin >= 0)
            continue;
        if (out && n < cap)
            out[n] = var.location;
        n++;
    }
    /* Ascending, so a comparison of two stages is a comparison of the same
     * list in the same order. */
    if (out)
        for (int a = 1; a < (n < cap ? n : cap); a++)
            for (int b = a; b > 0 && out[b - 1] > out[b]; b--) {
                const int32_t t = out[b - 1];
                out[b - 1] = out[b];
                out[b] = t;
            }
    return n;
}

int rsx_vk_spv_colour_targets(const rsx_vk_spv* spv)
{
    Module m;
    m.has_main_name = false;
    if (!module_parse(spv, &m))
        return 0;
    module_apply(&m);
    int count = 0;
    for (size_t v = 0; v < m.vars.size(); v++) {
        const InterfaceVar& var = m.vars[v];
        /* Only meaningful for a fragment module, and the caller only asks for
         * one there: every Location-decorated, non-builtin Output of a PS is an
         * SV_TARGETn (glslang names it after the HLSL variable, `input.col0`
         * or `_out.t0`, so the name cannot be the test). A VS's outputs are
         * COLOR0/TEXCOORDn and are never counted here. */
        if (var.storage == SPV_STORAGE_OUTPUT && var.location >= 0 &&
            var.builtin < 0) {
            if (var.location + 1 > count)
                count = var.location + 1;
        }
    }
    return count;
}
