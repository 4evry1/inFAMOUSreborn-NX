/* The console's sound output on libnx audout. See audio_out_horizon.h for the design.
 *
 * Includes <switch.h> and no SPU header (the SPU headers' u128 typedef clashes with libnx's). */
#include "audio_out_horizon.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "../domain/audio_mix.h"
#include "../domain/audio_out_policy.h"

enum {
    POOL_BUFFERS = 16,                                              /* >= max depth + prefill + the block in hand */
    BLOCK_BYTES = AUDIO_MIX_OUT_SAMPLES * 2,                        /* 256 frames * 2 ch * s16 = 1024 */
    BUFFER_BYTES = 0x1000                                           /* audout wants 0x1000 alignment and size */
};

static struct {
    AudioOutBuffer buffers[POOL_BUFFERS];
    int16_t *memory[POOL_BUFFERS];
    uint8_t free_list[POOL_BUFFERS];
    uint32_t free_count;
    uint8_t fifo[POOL_BUFFERS];                                     /* queued at the device, in append order */
    uint32_t fifo_head, fifo_count;
    audio_out_policy policy;
    Thread thread;
    int thread_made;
    int audout_up;
    uint32_t stop;
    audio_out_horizon_pump_fn pump;
    void *pump_context;
    uint64_t status_due_ns;
    uint32_t first_block_logged;
    /* statistics: written by the output thread, read from anywhere */
    int32_t init_result;
    uint32_t active, sample_rate, in_flight;
    uint64_t blocks, empty, silence, underruns, dropped, skipped, append_failures, pumps;
} g;

#define STAT_ADD(field, n) __atomic_fetch_add(&g.field, (n), __ATOMIC_RELAXED)
#define STAT_GET(field) __atomic_load_n(&g.field, __ATOMIC_RELAXED)

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* ---- the buffer pool and the device queue ---- */

static void fifo_push(uint8_t index)
{
    g.fifo[(g.fifo_head + g.fifo_count) % POOL_BUFFERS] = index;
    ++g.fifo_count;
    __atomic_store_n(&g.in_flight, g.fifo_count, __ATOMIC_RELAXED);
}

/* Takes the buffer at FIFO position `position` out of the queue and frees it. */
static void fifo_release_at(uint32_t position)
{
    const uint8_t index = g.fifo[(g.fifo_head + position) % POOL_BUFFERS];
    uint32_t i;
    for (i = position; i > 0; --i)   /* close the gap: shift the older entries up */
        g.fifo[(g.fifo_head + i) % POOL_BUFFERS] = g.fifo[(g.fifo_head + i - 1) % POOL_BUFFERS];
    g.fifo_head = (g.fifo_head + 1) % POOL_BUFFERS;
    --g.fifo_count;
    g.free_list[g.free_count++] = index;
    __atomic_store_n(&g.in_flight, g.fifo_count, __ATOMIC_RELAXED);
}

/* audout hands back finished buffers one call at a time, oldest first. Anything else it reports
 * (an unknown pointer, a count above one) is resolved by position, so the free list never loses a buffer. */
static void reclaim(void)
{
    uint32_t calls;
    if (!g.audout_up || !g.fifo_count) return;
    for (calls = 0; calls <= POOL_BUFFERS && g.fifo_count; ++calls) {
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        uint32_t position = 0, extra;
        if (R_FAILED(audoutGetReleasedAudioOutBuffer(&released, &count)) || count == 0) break;
        if (released) {
            for (position = 0; position < g.fifo_count; ++position)
                if (&g.buffers[g.fifo[(g.fifo_head + position) % POOL_BUFFERS]] == released) break;
            if (position >= g.fifo_count) position = 0;   /* not ours: the oldest one */
        }
        fifo_release_at(position);
        for (extra = 1; extra < count && g.fifo_count; ++extra) fifo_release_at(0);
    }
}

/* Queues one block (`stereo` NULL = silence). Returns 1 when the device took it. */
static int queue_block(const int16_t *stereo)
{
    uint8_t index;
    if (!g.free_count) return 0;
    index = g.free_list[g.free_count - 1u];
    if (stereo) memcpy(g.memory[index], stereo, BLOCK_BYTES);
    else memset(g.memory[index], 0, BLOCK_BYTES);
    g.buffers[index].next = NULL;
    g.buffers[index].buffer = g.memory[index];
    g.buffers[index].buffer_size = BUFFER_BYTES;
    g.buffers[index].data_size = BLOCK_BYTES;
    g.buffers[index].data_offset = 0;
    if (R_FAILED(audoutAppendAudioOutBuffer(&g.buffers[index]))) {
        STAT_ADD(append_failures, 1);
        return 0;
    }
    --g.free_count;
    fifo_push(index);
    return 1;
}

void audio_out_horizon_submit(const int16_t *stereo, int silent)
{
    audio_out_submit_plan plan;
    uint32_t i;
    if (!g.audout_up) return;
    audio_out_policy_plan_submit(&g.policy, g.fifo_count, now_ns(), &plan);
    if (plan.underrun) STAT_ADD(underruns, 1);
    for (i = 0; i < plan.silence_before; ++i)
        if (queue_block(NULL)) STAT_ADD(silence, 1);
    if (plan.drop || !queue_block(stereo)) {
        STAT_ADD(dropped, 1);
        return;
    }
    STAT_ADD(blocks, 1);
    if (silent) STAT_ADD(empty, 1);
}

void audio_out_horizon_note_skipped(uint64_t blocks) { STAT_ADD(skipped, blocks); }

/* ---- statistics ---- */

void audio_out_horizon_get_stats(audio_out_horizon_stats *out)
{
    memset(out, 0, sizeof *out);
    out->init_result = (int32_t)STAT_GET(init_result);
    out->active = STAT_GET(active);
    out->sample_rate = STAT_GET(sample_rate);
    out->in_flight = STAT_GET(in_flight);
    out->blocks = STAT_GET(blocks);
    out->empty = STAT_GET(empty);
    out->silence = STAT_GET(silence);
    out->underruns = STAT_GET(underruns);
    out->dropped = STAT_GET(dropped);
    out->skipped = STAT_GET(skipped);
    out->append_failures = STAT_GET(append_failures);
    out->pumps = STAT_GET(pumps);
}

void audio_out_horizon_format_status(char *buffer, size_t size)
{
    audio_out_horizon_stats s;
    unsigned long long tenths;
    audio_out_horizon_get_stats(&s);
    tenths = s.blocks ? (unsigned long long)s.empty * 1000ull / s.blocks : 0ull;
    snprintf(buffer, size,
        "STARTUP_AUDIO_OUT init=0x%x blocks=%llu underruns=%llu dropped=%llu empty=%llu empty_pct=%llu.%llu "
        "silence=%llu depth=%u skipped=%llu append_failures=%llu pumps=%llu rate=%u",
        (unsigned)s.init_result, (unsigned long long)s.blocks, (unsigned long long)s.underruns,
        (unsigned long long)s.dropped, (unsigned long long)s.empty, tenths / 10ull, tenths % 10ull,
        (unsigned long long)s.silence, (unsigned)s.in_flight, (unsigned long long)s.skipped,
        (unsigned long long)s.append_failures, (unsigned long long)s.pumps, (unsigned)s.sample_rate);
}

static void print_status(void)
{
    char line[320];
    audio_out_horizon_format_status(line, sizeof line);
    printf("%s\n", line);
    fflush(stdout);
}

/* ---- the output thread ---- */

static void output_thread(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&g.stop, __ATOMIC_ACQUIRE)) {
        reclaim();   /* once per pass, before the pump: the queue depth the pump's blocks are judged by is then at most a few microseconds old */
        if (g.pump) g.pump(g.pump_context);
        STAT_ADD(pumps, 1);
        /* The status goes out right after blocks were queued, so a slow stdout is covered by the queue. */
        if (!g.first_block_logged && STAT_GET(blocks)) {
            g.first_block_logged = 1;
            print_status();
        } else if (now_ns() >= g.status_due_ns) {
            g.status_due_ns = now_ns() + (uint64_t)AUDIO_OUT_STATUS_SECONDS * 1000000000ull;
            print_status();
        }
        svcSleepThread(INT64_C(2000000));
    }
}

/* ---- start and stop ---- */

static void free_pool(void)
{
    int i;
    for (i = 0; i < POOL_BUFFERS; ++i) {
        free(g.memory[i]);
        g.memory[i] = NULL;
    }
}

int audio_out_horizon_active(void) { return STAT_GET(active) != 0; }

int32_t audio_out_horizon_start(audio_out_horizon_pump_fn pump, void *context)
{
    Result rc;
    int i;
    memset(&g, 0, sizeof g);
    g.pump = pump;
    g.pump_context = context;
    audio_out_policy_init(&g.policy, AUDIO_OUT_PREFILL_BLOCKS, AUDIO_OUT_MAX_DEPTH, 250000000ull);

    rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) {
        g.audout_up = 1;
        rc = audoutStartAudioOut();
    }
    if (R_SUCCEEDED(rc)) {
        g.sample_rate = audoutGetSampleRate();
        /* the blocks are mixed for 48 kHz, 2 channels, int16: anything else would play at the wrong pitch */
        if (g.sample_rate != 48000u || audoutGetChannelCount() != 2u || audoutGetPcmFormat() != PcmFormat_Int16)
            rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    for (i = 0; R_SUCCEEDED(rc) && i < POOL_BUFFERS; ++i) {
        g.memory[i] = (int16_t *)memalign(BUFFER_BYTES, BUFFER_BYTES);
        if (!g.memory[i]) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break; }
        memset(g.memory[i], 0, BUFFER_BYTES);
        g.free_list[g.free_count++] = (uint8_t)i;
    }
    if (R_SUCCEEDED(rc)) {
        rc = threadCreate(&g.thread, output_thread, NULL, NULL, 0x10000, AUDIO_OUT_THREAD_PRIORITY, AUDIO_OUT_THREAD_CORE);
        if (R_SUCCEEDED(rc)) {
            g.thread_made = 1;
            g.status_due_ns = now_ns() + (uint64_t)AUDIO_OUT_STATUS_SECONDS * 1000000000ull;
            __atomic_store_n(&g.active, 1u, __ATOMIC_RELEASE);   /* before the thread can queue anything */
            rc = threadStart(&g.thread);
            if (R_FAILED(rc)) {
                __atomic_store_n(&g.active, 0u, __ATOMIC_RELEASE);
                threadClose(&g.thread);
                g.thread_made = 0;
            }
        }
    }
    if (R_FAILED(rc)) {
        /* Silent: nothing was started that the game depends on. */
        if (g.audout_up) {
            audoutStopAudioOut();
            audoutExit();
            g.audout_up = 0;
        }
        free_pool();
        g.free_count = 0;
        __atomic_store_n(&g.init_result, (int32_t)rc, __ATOMIC_RELAXED);
        printf("STARTUP_AUDIO_OUT init=0x%x blocks=0 underruns=0 dropped=0 mode=silent (audout unavailable: the game runs without sound output)\n",
            (unsigned)rc);
        fflush(stdout);
        return (int32_t)rc;
    }
    __atomic_store_n(&g.init_result, 0, __ATOMIC_RELAXED);
    printf("STARTUP_AUDIO_OUT init=0x0 blocks=0 underruns=0 dropped=0 mode=playing rate=%u buffers=%d frames=%u "
           "prefill=%u max_depth=%u thread_core=%d priority=0x%x\n",
        (unsigned)g.sample_rate, (int)POOL_BUFFERS, (unsigned)AUDIO_MIX_BLOCK_FRAMES, (unsigned)AUDIO_OUT_PREFILL_BLOCKS,
        (unsigned)AUDIO_OUT_MAX_DEPTH, (int)AUDIO_OUT_THREAD_CORE, (unsigned)AUDIO_OUT_THREAD_PRIORITY);
    fflush(stdout);
    return 0;
}

void audio_out_horizon_stop(void)
{
    Result waited;
    if (!g.thread_made) return;
    __atomic_store_n(&g.stop, 1u, __ATOMIC_RELEASE);
    waited = svcWaitSynchronizationSingle(g.thread.handle, INT64_C(2000000000));
    if (R_FAILED(waited)) {
        /* The thread is still running (a blocked IPC or write): leave audout and the buffers to it. */
        printf("STARTUP_AUDIO_OUT stop_timeout result=0x%x\n", (unsigned)waited);
        fflush(stdout);
        return;
    }
    threadWaitForExit(&g.thread);
    threadClose(&g.thread);
    g.thread_made = 0;
    __atomic_store_n(&g.active, 0u, __ATOMIC_RELEASE);
    audoutStopAudioOut();
    audoutExit();
    g.audout_up = 0;
    print_status();
    free_pool();
    g.free_count = 0;
    g.fifo_count = 0;
}
