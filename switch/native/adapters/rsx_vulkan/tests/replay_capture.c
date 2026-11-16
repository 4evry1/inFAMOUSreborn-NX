/*
 * replay_capture - replay an RSX frame capture through ps3recomp's draw engine and the Vulkan backend.
 *
 * Root tool (2026-11-12). Input is a directory written by the console composition's frame capture
 * (native/tests/test_startup_indirect.cpp, StartupRsxCapture): methods.bin, memory.bin, state.txt.
 * Every NV4097 method of every captured frame is given to rsx_draw_engine_method so the register file
 * ends up as the game left it; in the frames before the last one the methods that DRAW (begin/end,
 * draw arrays, inline arrays, clear) are withheld, because the memory snapshot is from the last frame.
 * The colour targets that exist afterwards are read back and written as PPM files.
 *
 *   replay_capture <capture dir> <output dir>
 *
 * Run on lavapipe (VK_ICD_FILENAMES / VK_DRIVER_FILES = lvp_icd.json), like the other tests here.
 * Nothing in this file is evidence about the Switch.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_draw_engine.h"
#include "rsx_vk.h"

extern const rsx_draw_backend* rsx_vk_backend(void);
extern rsx_vk_device* rsx_vk_backend_device(void);
extern void rsx_vk_device_set_report(rsx_vk_device*, void (*)(void*, int, const char*), void*);

/* ---- guest memory: one flat image, main IO mapping then local memory -------------------------------- */
u8* vm_base;
u32 ppu_vm_size;
static u32 g_io_ea, g_io_size, g_local_base, g_local_size;
enum { MAIN_BASE = 0x00100000u };
static u32 local_image_base(void) { return MAIN_BASE + ((g_io_size + 0xFFFFFu) & ~0xFFFFFu); }

u32 cellGcmResolveLocated(int local, u32 offset)
{
    if (local) return offset < g_local_size ? local_image_base() + offset : 0;
    return offset < g_io_size ? MAIN_BASE + offset : 0;
}
u32 cellGcmResolveIO(u32 offset) { return offset < g_io_size ? MAIN_BASE + offset : 0; }

static unsigned g_reports[4];
static void report_sink(void* user, int severity, const char* message)
{
    (void)user;
    if (severity >= 0 && severity < 4) g_reports[severity]++;
    if (g_reports[0] + g_reports[1] + g_reports[2] + g_reports[3] <= 60u)
        printf("REPORT severity=%d %s\n", severity, message);
}

static unsigned char* read_file(const char* dir, const char* name, size_t* size)
{
    char path[1024];
    FILE* file;
    unsigned char* data;
    long length;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    file = fopen(path, "rb");
    if (!file) { printf("FAIL cannot open %s\n", path); return NULL; }
    fseek(file, 0, SEEK_END); length = ftell(file); fseek(file, 0, SEEK_SET);
    data = (unsigned char*)malloc((size_t)length + 1u);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) { fclose(file); free(data); return NULL; }
    data[length] = 0;
    fclose(file);
    *size = (size_t)length;
    return data;
}

static int draws_something(u32 method)
{
    return method == 0x1808u /* SET_BEGIN_END */ || method == 0x1814u /* DRAW_ARRAYS */ ||
           method == 0x1818u /* INLINE_ARRAY */ || method == 0x1824u /* DRAW_INDEX_ARRAY */ ||
           method == 0x1D94u /* CLEAR_SURFACE */;
}

/* Optional: called with the device and the first colour target after the picture is written and before the
 * engine is shut down. The Switch front end presents the target on the screen from here. */
void (*g_replay_present_hook)(rsx_vk_device* device, u32 target) = NULL;

/* Root (2026-11-13): reads back every live colour target at its own size into <out_dir>/target-<tag><id>.ppm. */
static unsigned dump_targets(rsx_vk_device* device, const rsx_draw_backend* be, const char* out_dir, const char* tag, u8** keep)
{
    unsigned targets = 0;
    u32 id;
        /* Root (2026-11-13): every live colour target at its own size (was: ids below 256 at 1280x720). */
        for (id = 1; id < rsx_vk_handle_capacity(device->handles); id++) {
            rsx_vk_slot* slot = rsx_vk_handle_get(device->handles, id);
            if (slot && slot->kind == RSX_VK_SLOT_COLOR_TARGET && slot->u.color.width && slot->u.color.width <= 2048u) {
                const u32 width = slot->u.color.width, height = slot->u.color.height;
                u8* pixels = (u8*)malloc((size_t)width * height * 4u);
                char path[1024];
                FILE* out;
                size_t pixel, nonzero = 0;
                if (!pixels) continue;
                memset(pixels, 0, (size_t)width * height * 4u);
                {   /* the backend's readback slab is 1 MiB: read the target in 120-row strips */
                    u32 row;
                    for (row = 0; row < height; row += 120u)
                        be->readback(be->user, id, 0, row, width, height - row < 120u ? height - row : 120u,
                                     pixels + (size_t)row * width * 4u, width * 4u /* row pitch */);
                }
                for (pixel = 0; pixel < (size_t)width * height; ++pixel)
                    if (pixels[pixel * 4u] | pixels[pixel * 4u + 1u] | pixels[pixel * 4u + 2u]) ++nonzero;
                snprintf(path, sizeof path, "%s/target-%s%u.ppm", out_dir, tag, id);
                out = fopen(path, "wb");
                if (out) {
                    fprintf(out, "P6\n%u %u\n255\n", width, height);
                    for (pixel = 0; pixel < (size_t)width * height; ++pixel) fwrite(pixels + pixel * 4u, 1, 3, out);
                    fclose(out);
                }
                printf("TARGET %shandle=%u non_black_pixels=%zu of %u file=%s\n", tag, id, nonzero, width * height, path);
                if (keep && !*keep) *keep = pixels;
                else free(pixels);
                ++targets;
            }
        }
    return targets;
}

/* The replay itself. `keep_pixels`, when not NULL, receives a malloc'd 1280x720 RGBA copy of the first colour
 * target (the caller frees it): the Switch front end shows it on the screen after the device is gone. */
int replay_run(const char* capture_dir, const char* output_dir, unsigned char** keep_pixels)
{
    char* argv[3];
    const int argc = 3;
    const rsx_draw_backend* be;
    rsx_vk_device* device;
    unsigned char *state, *memory, *methods;
    size_t state_size = 0, memory_size = 0, methods_size = 0, position, records, index;
    unsigned display_count = 0, frames = 0, last_frame_start = 0, fed = 0, withheld = 0, targets = 0;
    const char* line;
    const u32* words;
    u32 id;

    argv[0] = (char*)"replay_capture"; argv[1] = (char*)capture_dir; argv[2] = (char*)output_dir;
    (void)argc;
    if (keep_pixels) *keep_pixels = NULL;
    state = read_file(argv[1], "state.txt", &state_size);
    memory = read_file(argv[1], "memory.bin", &memory_size);
    methods = read_file(argv[1], "methods.bin", &methods_size);
    if (!state || !memory || !methods) return 2;
    if (sscanf(strstr((char*)state, "io_ea"), "io_ea 0x%x io_size 0x%x", &g_io_ea, &g_io_size) != 2 ||
        sscanf(strstr((char*)state, "local_base"), "local_base 0x%x local_size 0x%x", &g_local_base, &g_local_size) != 2) {
        printf("FAIL state.txt has no io/local lines\n"); return 2;
    }
    ppu_vm_size = local_image_base() + g_local_size;
    vm_base = (u8*)calloc(1, ppu_vm_size);
    if (!vm_base) { printf("FAIL no memory for a %u byte guest image\n", ppu_vm_size); return 2; }
    for (position = 0; position + 8u <= memory_size;) {
        u32 ea, length;
        memcpy(&ea, memory + position, 4); memcpy(&length, memory + position + 4, 4);
        position += 8u;
        if (position + length > memory_size) { printf("FAIL memory.bin is truncated\n"); return 2; }
        if (ea >= g_io_ea && ea - g_io_ea + length <= g_io_size)
            memcpy(vm_base + MAIN_BASE + (ea - g_io_ea), memory + position, length);
        else if (ea >= g_local_base && ea - g_local_base + length <= g_local_size)
            memcpy(vm_base + local_image_base() + (ea - g_local_base), memory + position, length);
        else printf("INFO memory record 0x%08X+0x%X is outside both ranges\n", ea, length);
        position += length;
    }
    {   /* Debug knob: REPLAY_FILL=<guest address>:<length>:<byte> (hex) overwrites part of the image. */
        const char* fill = getenv("REPLAY_FILL");
        unsigned ea, length, value;
        if (fill && sscanf(fill, "%x:%x:%x", &ea, &length, &value) == 3) {
            if (ea >= g_local_base && ea - g_local_base + length <= g_local_size)
                memset(vm_base + local_image_base() + (ea - g_local_base), (int)value, length);
            else if (ea >= g_io_ea && ea - g_io_ea + length <= g_io_size)
                memset(vm_base + MAIN_BASE + (ea - g_io_ea), (int)value, length);
            printf("INFO REPLAY_FILL 0x%08X+0x%X = 0x%02X\n", ea, length, value);
        }
    }
    printf("INFO guest image %u MiB: main io 0x%08X+0x%X, local 0x%08X+0x%X\n", ppu_vm_size >> 20,
           g_io_ea, g_io_size, g_local_base, g_local_size);

    be = rsx_vk_backend();
    device = rsx_vk_backend_device();
    if (!be) { printf("FAIL no Vulkan backend\n"); return 1; }
    rsx_draw_engine_set_backend(be);
    if (rsx_draw_engine_init(1280u, 720u) != 0) { printf("FAIL rsx_draw_engine_init\n"); return 1; }
    if (device) rsx_vk_device_set_report(device, report_sink, NULL);

    for (line = (const char*)state; (line = strstr(line, "display ")) != NULL; line += 8) {
        unsigned buffer, offset, pitch, width, height;
        if (sscanf(line, "display %u offset 0x%x pitch %u width %u height %u", &buffer, &offset, &pitch, &width, &height) == 5) {
            rsx_draw_engine_set_display_buffer(buffer, 0u /* local */, offset, pitch, width, height);
            ++display_count;
        }
    }

    words = (const u32*)methods;
    records = methods_size / 12u;
    for (index = 0; index < records; ++index)
        if (words[index * 3u] == 0xFFFFFFFFu) { ++frames; if (index + 1u < records) last_frame_start = (unsigned)(index + 1u); }
    /* the last frame is the one that ENDS with the final flip record */
    {
        unsigned seen = 0;
        last_frame_start = 0;
        for (index = 0; index < records; ++index)
            if (words[index * 3u] == 0xFFFFFFFFu && ++seen == frames - 1u) last_frame_start = (unsigned)(index + 1u);
    }
    printf("INFO %zu records, %u frames, last frame starts at record %u, %u display buffers\n",
           records, frames, last_frame_start, display_count);
    {
        /* Debug knobs: REPLAY_MAX_DRAWS=n feeds only the first n begin/end pairs of the last frame;
         * REPLAY_SKIP_DRAWS=n withholds the first n; REPLAY_NO_CLEAR=1 withholds CLEAR_SURFACE. */
        const char* max_text = getenv("REPLAY_MAX_DRAWS");
        const char* skip_text = getenv("REPLAY_SKIP_DRAWS");
        const unsigned max_draws = max_text ? (unsigned)strtoul(max_text, NULL, 0) : 0xFFFFFFFFu;
        const unsigned skip_draws = skip_text ? (unsigned)strtoul(skip_text, NULL, 0) : 0u;
        const int no_clear = getenv("REPLAY_NO_CLEAR") != NULL;
        unsigned draw_index = 0;
        int inside = 0, inside_allowed = 0;
        for (index = 0; index < records; ++index) {
            const u32 subchannel = words[index * 3u], method = words[index * 3u + 1u], data = words[index * 3u + 2u];
            if (subchannel == 0xFFFFFFFFu) continue;
            if (subchannel != 0u) continue;   /* NV4097 is subchannel 0 in this game */
            if (index < last_frame_start && draws_something(method)) { ++withheld; continue; }
            if (index >= last_frame_start) {
                if (method == 0x1D94u && no_clear) { ++withheld; continue; }
                if (method == 0x1808u && data != 0u) {
                    inside = 1;
                    inside_allowed = draw_index >= skip_draws && draw_index < max_draws;
                    ++draw_index;
                    if (!inside_allowed) { ++withheld; continue; }
{   /* Debug knob: REPLAY_DUMP_AT=a,b,c dumps every colour target just before those draws. */
                        const char* at = getenv("REPLAY_DUMP_AT");
                        while (at && *at) {
                            char* end;
                            const unsigned long want = strtoul(at, &end, 0);
                            if (end == at) break;
                            if (want == draw_index - 1u && device) {
                                char tag[32];
                                snprintf(tag, sizeof tag, "d%lu-", want);
                                rsx_draw_engine_flush();
                                dump_targets(device, be, argv[2], tag, NULL);
                            }
                            at = *end ? end + 1 : end;
                        }
                    }
                    printf("INFO draw %u begins (primitive %u)\n", draw_index - 1u, data); fflush(stdout);
                } else if (method == 0x1808u) {
                    const int allowed = inside_allowed;
                    inside = 0;
                    if (!allowed) { ++withheld; continue; }
                } else if (inside && !inside_allowed && draws_something(method)) { ++withheld; continue; }
            }
            {   /* Debug knob: REPLAY_FORCE_W=<constant index> replaces a zero w of that vertex constant by 1.0. */
                static u32 load_index = 0xFFFFFFFFu;
                static long force = -2;
                if (force == -2) { const char* text = getenv("REPLAY_FORCE_W"); force = text ? strtol(text, NULL, 0) : -1; }
                if (method == 0x1EFCu) load_index = data * 4u;
                else if (force >= 0 && method >= 0x1F00u && method < 0x1F80u && load_index != 0xFFFFFFFFu &&
                         load_index + (method - 0x1F00u) / 4u == (u32)force * 4u + 3u && data == 0u) {
                    rsx_draw_engine_method(method, 0x3F800000u);
                    ++fed;
                    continue;
                }
            }
            rsx_draw_engine_method(method, data);
            ++fed;
        }
    }
    rsx_draw_engine_flush();
    printf("INFO fed %u methods, withheld %u draw methods of earlier frames, engine guest draws %u\n",
           fed, withheld, rsx_draw_engine_guest_draws());

    if (device) targets += dump_targets(device, be, argv[2], "", keep_pixels);
    if (device && g_replay_present_hook) {
        for (id = 1; id < 256; id++) {
            rsx_vk_slot* slot = rsx_vk_handle_get(device->handles, id);
            if (slot && slot->kind == RSX_VK_SLOT_COLOR_TARGET) { g_replay_present_hook(device, id); break; }
        }
    }
    printf("REPLAY_DONE targets=%u reports=%u,%u,%u,%u\n", targets, g_reports[0], g_reports[1], g_reports[2], g_reports[3]);
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
    free(vm_base);
    return 0;
}

#if !defined(__SWITCH__)
int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash in the driver thread must not lose the log */
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 3) { printf("usage: %s <capture dir> <output dir>\n", argv[0]); return 2; }
    return replay_run(argv[1], argv[2], NULL);
}
#endif
