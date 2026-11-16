#include "gpu_clear_deko.h"
#include "deko_queue_owner.h"
#include <deko3d.h>
#include <switch/arm/counter.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t command_bytes = 64 * 1024;
constexpr int64_t completion_timeout_ns = INT64_C(5000000000);
constexpr uint8_t unread_sentinel = 0xa5;
constexpr uint32_t maximum_dimension = 256;
constexpr uint32_t dimension_granularity = 4;
constexpr uint32_t all_color_channels = 0xf;

struct Resources {
    DekoQueueOwner gpu;
    DkMemBlock image_memory{};
    DkMemBlock readback_memory{};
    ~Resources() {
        // Every submitted path reaches a completed, flushed fence before here.
        gpu.require_completed();
        gpu.release_commands();
        if (readback_memory) dkMemBlockDestroy(readback_memory);
        if (image_memory) dkMemBlockDestroy(image_memory);
        gpu.release_queue();
    }
};

DkMemBlock allocate_block(DkDevice device, uint32_t size, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, size);
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}
uint32_t align_memory(uint32_t bytes) {
    return (bytes + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
}

void wait_for_stage(DekoQueueOwner& owner, const char* stage) {
    const DekoWaitClock clock{armGetSystemTick, armGetSystemTickFreq};
    if (owner.wait(stage, "GPU_CLEAR", completion_timeout_ns, clock) != DekoCompletion::completed) {
        // The GPU may still own storage. Process termination avoids releasing it
        // while completion is uncertain; Horizon tears down the resource owner.
        std::fputs("GPU_CLEAR_COMPLETION_FAILED_NO_READBACK\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
}
}

gpu_clear_status gpu_clear_readback(const gpu_clear_request& request,
                                    uint8_t* output, size_t output_bytes) {
    if (!output || request.width < dimension_granularity || request.height < dimension_granularity ||
        request.width > maximum_dimension || request.height > maximum_dimension ||
        request.width % dimension_granularity || request.height % dimension_granularity ||
        output_bytes != size_t(request.width) * request.height * 4 ||
        request.rectangle_x >= request.width || request.rectangle_y >= request.height ||
        !request.rectangle_width || !request.rectangle_height ||
        request.rectangle_width > request.width - request.rectangle_x ||
        request.rectangle_height > request.height - request.rectangle_y)
        return gpu_clear_status::invalid_request;
    for (unsigned channel = 0; channel != 4; ++channel) {
        if (!std::isfinite(request.background_rgba[channel]) || !std::isfinite(request.rectangle_rgba[channel]) ||
            request.background_rgba[channel] < 0 || request.background_rgba[channel] > 1 ||
            request.rectangle_rgba[channel] < 0 || request.rectangle_rgba[channel] > 1)
            return gpu_clear_status::invalid_request;
    }
    Resources owned;
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    owned.gpu.device = dkDeviceCreate(&device_maker);
    if (!owned.gpu.device) return gpu_clear_status::device_failed;
    std::puts("GPU_CLEAR_DEVICE_CREATED");

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, owned.gpu.device);
    owned.gpu.queue = dkQueueCreate(&queue_maker);
    if (!owned.gpu.queue) return gpu_clear_status::queue_failed;
    std::puts("GPU_CLEAR_QUEUE_CREATED");
    wait_for_stage(owned.gpu, "queue-init");

    DkImageLayoutMaker image_maker;
    dkImageLayoutMakerDefaults(&image_maker, owned.gpu.device);
    image_maker.flags = DkImageFlags_UsageRender;
    image_maker.format = DkImageFormat_RGBA8_Unorm;
    image_maker.dimensions[0] = request.width;
    image_maker.dimensions[1] = request.height;
    image_maker.dimensions[2] = 1;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &image_maker);
    const uint64_t image_bytes = dkImageLayoutGetSize(&layout);
    if (!image_bytes || image_bytes > UINT32_MAX - DK_MEMBLOCK_ALIGNMENT)
        return gpu_clear_status::allocation_failed;
    owned.image_memory = allocate_block(owned.gpu.device, align_memory(uint32_t(image_bytes)),
                                        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    owned.readback_memory = allocate_block(owned.gpu.device, align_memory(uint32_t(output_bytes)),
                                           DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    owned.gpu.command_memory = allocate_block(owned.gpu.device, command_bytes,
                                          DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    if (!owned.image_memory || !owned.readback_memory || !owned.gpu.command_memory)
        return gpu_clear_status::allocation_failed;
    void* readback = dkMemBlockGetCpuAddr(owned.readback_memory);
    if (!readback) return gpu_clear_status::allocation_failed;
    std::memset(readback, unread_sentinel, output_bytes);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    DkImage image;
    dkImageInitialize(&image, &layout, owned.image_memory, 0);
    DkImageView view;
    dkImageViewDefaults(&view, &image);
    DkCmdBufMaker command_maker;
    dkCmdBufMakerDefaults(&command_maker, owned.gpu.device);
    owned.gpu.commands = dkCmdBufCreate(&command_maker);
    if (!owned.gpu.commands) return gpu_clear_status::command_failed;
    dkCmdBufAddMemory(owned.gpu.commands, owned.gpu.command_memory, 0, command_bytes);
    const DkImageView* targets[] = {&view};
    dkCmdBufBindRenderTargets(owned.gpu.commands, targets, 1, nullptr);
    const DkScissor full{0, 0, request.width, request.height};
    const DkScissor rectangle{request.rectangle_x, request.rectangle_y, request.rectangle_width, request.rectangle_height};
    dkCmdBufSetScissors(owned.gpu.commands, 0, &full, 1);
    dkCmdBufClearColor(owned.gpu.commands, 0, all_color_channels, request.background_rgba);
    dkCmdBufSetScissors(owned.gpu.commands, 0, &rectangle, 1);
    dkCmdBufClearColor(owned.gpu.commands, 0, all_color_channels, request.rectangle_rgba);
    DkCmdList clear_list = dkCmdBufFinishList(owned.gpu.commands);
    if (!clear_list) return gpu_clear_status::command_failed;
    if (!owned.gpu.submit(clear_list)) return gpu_clear_status::command_failed;
    wait_for_stage(owned.gpu, "clear");
    if (!owned.gpu.reset_commands()) return gpu_clear_status::command_failed;
    dkCmdBufBarrier(owned.gpu.commands, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    const DkImageRect region{0, 0, 0, request.width, request.height, 1};
    // Zero strides request the tightly packed byte pitch from deko3d. Width
    // and height here would be interpreted as byte pitch and image height.
    const DkCopyBuf destination{dkMemBlockGetGpuAddr(owned.readback_memory), 0, 0};
    dkCmdBufCopyImageToBuffer(owned.gpu.commands, &view, &region, &destination, 0);
    DkCmdList list = dkCmdBufFinishList(owned.gpu.commands);
    if (!list) return gpu_clear_status::command_failed;
    if (!owned.gpu.submit(list)) return gpu_clear_status::command_failed;
    wait_for_stage(owned.gpu, "copy");
    std::puts("GPU_CLEAR_COMPLETED_FLUSHED_FENCE");
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::memcpy(output, readback, output_bytes);
    std::printf("GPU_CLEAR_READBACK copied=1 bytes=%zu\n", output_bytes);
    return gpu_clear_status::success;
}
