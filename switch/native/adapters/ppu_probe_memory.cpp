#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "ppu_recomp.h"
// Implement the out-of-line ABI referenced by the existing O0 guest object.
#undef vm_read32

namespace {
struct ReadBinding {
    uintptr_t base;
    uint32_t first;
    uint32_t second;
    uint64_t reads;
    bool active;
};
// Guest address data only, scoped to one synchronous call, never provider callbacks.
thread_local ReadBinding binding{};
bool contains_word(uint64_t address, uint32_t start) {
    return address >= start && address - start <= 8;
}
}

extern "C" uint32_t vm_read32(uint64_t address) {
    if (!binding.active || address > UINT32_MAX - 3 ||
        (!contains_word(address, binding.first) && !contains_word(address, binding.second))) {
        std::fputs("PPU_PROBE_MEMORY_INVALID_READ\n", stderr);
        std::abort();
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(binding.base + address);
    ++binding.reads;
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

extern "C" int ps3_ppu_probe_execute(uintptr_t base, uint32_t first, uint32_t second,
                                       ppu_context* context, uint64_t* reads) {
    if (!base || !context || !reads || binding.active ||
        first > UINT32_MAX - 11 || second > UINT32_MAX - 11 || base > UINTPTR_MAX - UINT32_MAX)
        return 1;
    // Caller has committed both ranges and holds their lifetime until this returns.
    const ReadBinding previous = binding;
    binding = {base, first, second, 0, true};
    func_00011E08(context);
    *reads = binding.reads;
    binding = previous;
    return 0;
}
