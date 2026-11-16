#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "guest_space.h"
#include "ppu_recomp.h"

extern "C" int ps3_ppu_probe_execute(uintptr_t, uint32_t, uint32_t, ppu_context*, uint64_t*);
static_assert(offsetof(ppu_context, gpr) == 0, "generated GPR ABI");
static_assert(offsetof(ppu_context, cr) == 1024, "generated CR ABI");
static_assert(offsetof(ppu_context, lr) == 1032, "generated LR ABI");

namespace {
constexpr uint32_t low_page = 0x10010000;
constexpr uint32_t stack_page = 0xD00FF000;
constexpr uint32_t first = low_page + 0x20;
constexpr uint32_t second = stack_page + 0x80;
constexpr size_t page_bytes = 4096;
struct Case {
    const char* name;
    uint32_t a[3];
    uint32_t b[3];
    uint64_t result;
    uint32_t cr;
    unsigned compared_word;
    uint64_t reads;
};
const Case cases[] = {
    {"equal", {1,2,3}, {1,2,3}, 0, 2, 2, 6},
    {"first-negative", {1,99,99}, {2,0,0}, UINT64_MAX, 8, 0, 2},
    {"second-positive", {7,11,99}, {7,5,0}, 6, 4, 1, 4},
    {"third-negative-cr-stays-equal", {7,5,1}, {7,5,2}, UINT64_MAX, 2, 2, 6},
    {"signed-wrap", {0,0,0}, {0x80000000u,0,0}, UINT64_C(0xffffffff80000000), 8, 0, 2},
    {"big-endian-high-word", {0xffffffffu,0,0}, {0,0,0}, UINT64_MAX, 8, 0, 2},
};
void encode_words(const uint32_t words[3], uint8_t out[12]) {
    for (unsigned i = 0; i != 3; ++i)
        for (unsigned byte = 0; byte != 4; ++byte)
            out[i*4+byte] = uint8_t(words[i] >> (24-byte*8));
}
}

extern "C" int native_test_generated_ppu(const ps3_guest_memory* memory, uintptr_t base) {
    if (!memory || !memory->commit || !memory->decommit || !memory->read || !memory->write || !base)
        return 1;
    bool low_committed = false, stack_committed = false;
    int failures = 0;
    if (memory->commit(memory->context, low_page, page_bytes) != PS3_GUEST_MEMORY_OK) {
        std::puts("GENERATED_PPU_COMMIT_LOW_FAIL"); return 1;
    }
    low_committed = true;
    if (memory->commit(memory->context, stack_page, page_bytes) != PS3_GUEST_MEMORY_OK) {
        std::puts("GENERATED_PPU_COMMIT_STACK_FAIL"); failures = 1;
    } else {
        stack_committed = true;
        for (const Case& test : cases) {
            uint8_t a[12], b[12], before_low[page_bytes], before_stack[page_bytes], after[page_bytes];
            encode_words(test.a, a); encode_words(test.b, b);
            if (memory->write(memory->context, first, a, sizeof a) != PS3_GUEST_MEMORY_OK ||
                memory->write(memory->context, second, b, sizeof b) != PS3_GUEST_MEMORY_OK ||
                memory->read(memory->context, low_page, before_low, page_bytes) != PS3_GUEST_MEMORY_OK ||
                memory->read(memory->context, stack_page, before_stack, page_bytes) != PS3_GUEST_MEMORY_OK) {
                std::puts("GENERATED_PPU_PORT_SETUP_FAIL"); ++failures; break;
            }
            ppu_context context;
            std::memset(&context, 0xa5, sizeof context);
            for (unsigned i = 0; i != 32; ++i) context.gpr[i] = UINT64_C(0x1122334400000000) + i;
            context.gpr[1] = stack_page + 0x800;
            context.gpr[3] = first; context.gpr[4] = second;
            context.cr = 0xabcdeff0;
            ppu_context expected;
            std::memcpy(&expected, &context, sizeof context);
            expected.gpr[3] = test.result;
            expected.gpr[0] = uint64_t(test.a[test.compared_word]) - uint64_t(test.b[test.compared_word]);
            expected.gpr[9] = test.compared_word == 2 ? test.a[2] : test.b[test.compared_word];
            expected.cr = (expected.cr & ~0xfu) | test.cr;
            uint64_t reads = 0;
            bool passed = ps3_ppu_probe_execute(base, first, second, &context, &reads) == 0 &&
                          reads == test.reads && std::memcmp(&context, &expected, sizeof context) == 0;
            if (memory->read(memory->context, low_page, after, page_bytes) != PS3_GUEST_MEMORY_OK ||
                std::memcmp(before_low, after, page_bytes) != 0) passed = false;
            if (memory->read(memory->context, stack_page, after, page_bytes) != PS3_GUEST_MEMORY_OK ||
                std::memcmp(before_stack, after, page_bytes) != 0) passed = false;
            std::printf("GENERATED_PPU_CASE %s %s reads=%llu\n", test.name, passed ? "PASS" : "FAIL", (unsigned long long)reads);
            if (!passed) ++failures;
        }
    }
    if (stack_committed && memory->decommit(memory->context, stack_page, page_bytes) != PS3_GUEST_MEMORY_OK) {
        std::puts("GENERATED_PPU_DECOMMIT_STACK_FAIL"); ++failures;
    }
    if (low_committed && memory->decommit(memory->context, low_page, page_bytes) != PS3_GUEST_MEMORY_OK) {
        std::puts("GENERATED_PPU_DECOMMIT_LOW_FAIL"); ++failures;
    }
    std::printf("GENERATED_PPU_LEAF_%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
