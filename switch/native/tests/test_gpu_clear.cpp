#include "gpu_clear_deko.h"
#include <cstdint>
#include <cstdio>

extern "C" int native_test_gpu_clear(int corrupt_expected_pattern) {
    constexpr uint32_t width = 64, height = 32;
    uint8_t actual[width * height * 4]{};
    const gpu_clear_request request{width, height, {1.0f, 0.0f, 0.0f, 1.0f},
                                   width / 4, height / 4, width / 2, height / 2,
                                   {0.0f, 1.0f, 0.0f, 1.0f}};
    std::printf("GPU_CLEAR_BEGIN negative_corrupt_expected=%u width=%u height=%u\n",
                corrupt_expected_pattern ? 1u : 0u, width, height);
    const gpu_clear_status status = gpu_clear_readback(request, actual, sizeof actual);
    if (status != gpu_clear_status::success) {
        std::printf("GPU_CLEAR_FAIL adapter_status=%u\n", unsigned(status));
        return 1;
    }
    size_t mismatches = 0;
    uint32_t checksum = 2166136261u;
    for (uint32_t y = 0; y != height; ++y) {
        for (uint32_t x = 0; x != width; ++x) {
            const bool center = x >= width / 4 && x < 3 * width / 4 &&
                                y >= height / 4 && y < 3 * height / 4;
            uint8_t expected[] = {uint8_t(center ? 0 : 255), uint8_t(center ? 255 : 0), 0, 255};
            if (corrupt_expected_pattern) expected[0] ^= 1;
            for (unsigned channel = 0; channel != 4; ++channel) {
                const size_t offset = (size_t(y) * width + x) * 4 + channel;
                checksum = (checksum ^ actual[offset]) * 16777619u;
                if (actual[offset] != expected[channel]) {
                    if (!mismatches)
                        std::printf("GPU_CLEAR_FIRST_MISMATCH x=%u y=%u channel=%u actual=%u expected=%u\n",
                                    x, y, channel, unsigned(actual[offset]), unsigned(expected[channel]));
                    ++mismatches;
                }
            }
        }
    }
    if (corrupt_expected_pattern && !mismatches)
        std::puts("GPU_CLEAR_NEGATIVE_UNEXPECTED_MATCH");
    std::printf("GPU_CLEAR_%s mismatches=%zu bytes=%zu checksum=%08x negative=%u\n",
                mismatches ? "FAIL" : "PASS", mismatches, sizeof actual, checksum,
                corrupt_expected_pattern ? 1u : 0u);
    return mismatches ? 1 : 0;
}
