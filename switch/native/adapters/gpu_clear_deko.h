#pragma once
#include <stddef.h>
#include <stdint.h>

struct gpu_clear_request {
    uint32_t width;
    uint32_t height;
    float background_rgba[4];
    uint32_t rectangle_x;
    uint32_t rectangle_y;
    uint32_t rectangle_width;
    uint32_t rectangle_height;
    float rectangle_rgba[4];
};

enum class gpu_clear_status {
    success,
    invalid_request,
    device_failed,
    queue_failed,
    allocation_failed,
    command_failed,
    queue_error,
};

// Synchronous: output is copied only after GPU completion and cache flush.
gpu_clear_status gpu_clear_readback(const gpu_clear_request& request,
                                    uint8_t* output, size_t output_bytes);
