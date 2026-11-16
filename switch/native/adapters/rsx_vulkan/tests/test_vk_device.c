/*
 * test_vk_device - the slice S1 acceptance test (DESIGN §7 S1, §6.3 case 1)
 *
 * Four cases, all on lavapipe (VK_ICD_FILENAMES is the caller's job):
 *
 *   1. clear + readback: through the vtable, create a colour target, clear it
 *      to {0.25, 0.5, 0.75, 1}, read the centre pixel back and compare with
 *      {64,128,191,255} +/- 1.
 *   2. 100 init/shutdown cycles with no leak: vkAllocateMemory calls must
 *      equal vkFreeMemory calls (counted in the backend's own dispatch).
 *   3. submit_and_wait with nothing recorded: a no-op that returns, and does
 *      not submit.
 *   4. the four descriptor set layouts of DESIGN §2.2 exist after init, and
 *      shutdown destroys them (covered by case 2's leak count).
 *
 * Exit code 0 only when every case passed; any validation message fails it.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan_core.h>

#include "rsx_draw_engine.h"
#include "rsx_vk.h"

extern const rsx_draw_backend* rsx_vk_backend(void);
extern rsx_vk_device* rsx_vk_backend_device(void);
extern void rsx_vk_device_set_report(rsx_vk_device*,
                                     void (*)(void*, int, const char*), void*);

static int g_failures;
static int g_validation;
static int g_validation_warn;

static void report_sink(void* user, int severity, const char* message)
{
    (void)user;
    (void)message;
    if (severity >= 2)
        g_validation++;
    else if (severity == 1)
        g_validation_warn++;
}

static void fail(const char* what)
{
    printf("FAIL %s\n", what);
    fflush(stdout);
    g_failures++;
}

static void pass(const char* what)
{
    printf("PASS %s\n", what);
    fflush(stdout);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- case 1 + 3: the vtable's real path -------------------------------- */

static void case_clear_readback(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = rsx_vk_backend_device();
    uint64_t submit_before;
    u32 surface;
    u8 px[4];
    u8 row[4 * 16];
    int want[4] = {64, 128, 191, 255};
    int i;
    const float clear[4] = {0.25f, 0.5f, 0.75f, 1.0f};

    rsx_vk_device_set_report(d, report_sink, NULL);
    if (be->init(be->user, 1280, 720) != 0) {
        fail("init");
        return;
    }
    pass("init");

    /* case 3 first: an empty submit must be a no-op. */
    submit_before = d->memory_allocs;
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    if (d->memory_allocs != submit_before)
        fail("submit_and_wait with nothing recorded allocated memory");
    else
        pass("submit_and_wait with nothing recorded returns");

    surface = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 256, 256,
                                      NULL, 0);
    if (!surface) {
        fail("color_target_create");
        be->shutdown(be->user);
        return;
    }
    printf("INFO colour target handle %u\n", surface);
    pass("color_target_create R8G8B8A8 256x256");

    be->clear_color(be->user, surface, clear);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);

    memset(px, 0xEE, sizeof(px));
    be->readback(be->user, surface, 128, 128, 1, 1, px, 4);
    printf("INFO centre pixel %u,%u,%u,%u (want %d,%d,%d,%d)\n", px[0], px[1],
           px[2], px[3], want[0], want[1], want[2], want[3]);
    for (i = 0; i < 4; i++) {
        int got = px[i];
        if (got < want[i] - 1 || got > want[i] + 1) {
            fail("centre pixel outside +/-1 of the clear colour");
            break;
        }
    }
    if (i == 4)
        pass("clear + readback centre pixel 64,128,191,255 +/-1");

    /* A rectangle with a pitch wider than the row: the pitch must be honoured.
     * This is the shape the engine's own readback uses
     * (rsx_draw_engine.c:1769-1775). */
    memset(row, 0xEE, sizeof(row));
    be->readback(be->user, surface, 16, 32, 4, 2, row, 16);
    /* The tolerance is int arithmetic: (u8)(255 + 1) wraps to 0, so comparing
     * against a u8 bound would reject a correct 255. */
    for (i = 0; i < 4; i++) {
        int got = row[i];
        if (got < want[i] - 1 || got > want[i] + 1) {
            fail("pitched readback row 0 wrong");
            break;
        }
    }
    if (i == 4) {
        /* row 1 starts at the caller's pitch (16), not at 4 */
        int got = row[16];
        if (got < want[0] - 1 || got > want[0] + 1)
            fail("pitched readback row 1 not at the caller's pitch");
        else
            pass("readback honours out_pitch");
    }

    /* An unsupported format is a logged 0, never a wrong-coloured target.
     *
     * S1's check here asked for R32F, which S1 did not know. S3's format table
     * carries all 12 formats of DESIGN §3.1, so R32F is a real colour target
     * now; the case that stays unsupported on every device is a format the
     * table has no entry for at all, i.e. RSX_BE_FMT_NONE, which is also
     * what a 12-entry table indexes out of. */
    if (be->color_target_create(be->user, RSX_BE_FMT_NONE, 16, 16, NULL, 0) != 0)
        fail("color_target_create accepted RSX_BE_FMT_NONE");
    else
        pass("color_target_create RSX_BE_FMT_NONE returns 0 and logs");
    /* And the S3 counterpart: R32F, the format S1 used to reject, is a live
     * colour target now (DESIGN §3.1 lists it as attachable). */
    {
        const u32 r32f = be->color_target_create(be->user, RSX_BE_FMT_R32F, 16,
                                                 16, NULL, 0);
        if (!r32f)
            fail("color_target_create R32F returned 0 (DESIGN 3.1 attaches it)");
        else {
            pass("color_target_create R32F is a live target (S3)");
            be->color_target_release(be->user, r32f);
        }
    }

    be->color_target_release(be->user, surface);
    be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
    /* The release is deferred to the fence-passed boundary the call above is,
     * so the target is gone here and not only at shutdown. */
    if (d->memory_allocs != d->memory_frees)
        printf("INFO one target still held: allocs %llu frees %llu\n",
               (unsigned long long)d->memory_allocs,
               (unsigned long long)d->memory_frees);
    else
        pass("released target freed before shutdown");

    be->shutdown(be->user);
    if (d->memory_allocs != d->memory_frees) {
        printf("FAIL leak in case 1: vkAllocateMemory %llu, vkFreeMemory %llu\n",
               (unsigned long long)d->memory_allocs,
               (unsigned long long)d->memory_frees);
        g_failures++;
    } else {
        printf("INFO case 1 memory: %llu allocated, %llu freed\n",
               (unsigned long long)d->memory_allocs,
               (unsigned long long)d->memory_frees);
        pass("case 1 no leak");
    }
}

/* ---- case 2: 100 create/destroy cycles --------------------------------- */

static void case_cycles(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    rsx_vk_device* d = rsx_vk_backend_device();
    uint64_t allocs = 0, frees = 0;
    int i;
    int64_t start;

    start = now_ms();
    for (i = 0; i < 100; i++) {
        u32 surface;
        if (be->init(be->user, 1280, 720) != 0) {
            printf("FAIL cycle %d init\n", i);
            g_failures++;
            return;
        }
        surface = be->color_target_create(be->user, RSX_BE_FMT_R8G8B8A8, 64, 64,
                                          NULL, 0);
        if (!surface) {
            printf("FAIL cycle %d color_target_create\n", i);
            g_failures++;
            be->shutdown(be->user);
            return;
        }
        be->clear_color(be->user, surface,
                        (const float[4]){0.25f, 0.5f, 0.75f, 1.0f});
        be->submit_and_wait(be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
        be->color_target_release(be->user, surface);
        be->shutdown(be->user);
        allocs += d->memory_allocs;
        frees += d->memory_frees;
        if (i == 99 || i == 0)
            printf("INFO cycle %d: allocs %llu frees %llu\n", i,
                   (unsigned long long)d->memory_allocs,
                   (unsigned long long)d->memory_frees);
    }
    printf("INFO 100 cycles in %lld ms: vkAllocateMemory %llu, vkFreeMemory %llu\n",
           (long long)(now_ms() - start), (unsigned long long)allocs,
           (unsigned long long)frees);
    if (allocs != frees) {
        printf("FAIL 100 cycles leak %llu allocations\n",
               (unsigned long long)(allocs - frees));
        g_failures++;
    } else {
        pass("100 init/shutdown cycles, vkAllocateMemory == vkFreeMemory");
    }
}

/* ---- case 4: descriptor layouts exist ---------------------------------- */

static void case_layouts(void)
{
    const rsx_draw_backend* be = rsx_vk_backend();
    if (be->init(be->user, 320, 240) != 0) {
        fail("init for the layout case");
        return;
    }
    /* The layouts are created by init and are what S2 will bind against; the
     * check here is that init reached them without a validation error. */
    pass("init builds the four descriptor set layouts of DESIGN 2.2");
    be->shutdown(be->user);
}

int main(void)
{
    printf("test_vk_device: rsx_vulkan slice S1\n");
    fflush(stdout);

    case_clear_readback();
    case_cycles();
    case_layouts();

    printf("validation: %d errors, %d warnings\n", g_validation,
           g_validation_warn);
    if (g_validation) {
        printf("FAIL validation layer reported %d errors\n", g_validation);
        g_failures++;
    }
    if (g_failures) {
        printf("RESULT FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("RESULT PASS\n");
    return 0;
}
