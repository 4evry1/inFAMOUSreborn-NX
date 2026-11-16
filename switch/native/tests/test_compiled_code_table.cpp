#include "compiled_code_table.h"
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <thread>
#include <type_traits>

static unsigned checks;
#define CHECK(expression) do { ++checks; if (!(expression)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); return 1; } } while (0)
static void first(ppu_context *cpu) { cpu->gpr[3] += 1; }
static void middle(ppu_context *cpu) { cpu->gpr[3] += 10; }
static void observed(ppu_context *cpu) { cpu->gpr[3] += 100; }
static void last(ppu_context *cpu) { cpu->gpr[3] += 1000; }

int main()
{
    const func_entry entries[] = {
        {0x100, first, "first"}, {0x00462380, observed, "observed"},
        {0x200, middle, "middle"}, {0xfffffffcu, last, "last"}
    };
    const uint32_t order[] = {0, 2, 1, 3};
    PpuCompiledCodeTable table;
    ps3_compiled_function result = last;
    CHECK(table.lookup(0x100, &result) == PS3_COMPILED_CODE_NOT_ADMITTED);
    CHECK(result == last);
    CHECK(table.admit(entries, 4, order, 4) == PS3_COMPILED_CODE_OK);
    CHECK(table.admit(entries, 4, order, 4) == PS3_COMPILED_CODE_ALREADY_ADMITTED);
    for (unsigned i = 0; i < 4; ++i) {
        const auto &entry = entries[order[i]];
        CHECK(table.lookup(static_cast<uint32_t>(entry.addr), &result) == PS3_COMPILED_CODE_OK);
        CHECK(result == entry.func);
    }
    const auto port = table.port();
    CHECK(port.find(port.context, 0x00462380, &result) == PS3_COMPILED_CODE_OK);
    CHECK(result == observed);
    ppu_context cpu{};
    cpu.lr = 0x1234; cpu.gpr[2] = 0x5678; cpu.gpr[3] = 3;
    result(&cpu);
    CHECK(cpu.gpr[3] == 103 && cpu.lr == 0x1234 && cpu.gpr[2] == 0x5678);
    for (uint32_t address : {0u, 1u, 0x102u, 0x300u}) {
        result = last;
        CHECK(table.lookup(address, &result) != PS3_COMPILED_CODE_OK);
        CHECK(result == last);
    }
    CHECK(table.lookup(0x100, nullptr) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    const uint32_t identity[] = {0, 1, 2, 3};
    const uint32_t duplicate[] = {0, 2, 2, 3};
    const uint32_t outside[] = {0, 2, 1, 4};
    for (const uint32_t *bad : {identity, duplicate, outside}) {
        PpuCompiledCodeTable invalid;
        CHECK(invalid.admit(entries, 4, bad, 4) == PS3_COMPILED_CODE_INVALID_TABLE);
        CHECK(invalid.lookup(0x100, &result) == PS3_COMPILED_CODE_NOT_ADMITTED);
    }
    for (uint64_t address : {0ull, 0x102ull, 0x100000000ull, 0x200ull}) {
        func_entry malformed[4] = {entries[0], entries[1], entries[2], entries[3]};
        malformed[0].addr = address;
        PpuCompiledCodeTable invalid;
        CHECK(invalid.admit(malformed, 4, order, 4) == PS3_COMPILED_CODE_INVALID_TABLE);
    }
    func_entry null_function[] = {entries[0]};
    null_function[0].func = nullptr;
    const uint32_t singleton[] = {0};
    PpuCompiledCodeTable invalid;
    CHECK(invalid.admit(null_function, 1, singleton, 1) == PS3_COMPILED_CODE_INVALID_TABLE);
    CHECK(invalid.admit(entries, 4, order, 3) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    CHECK(invalid.admit(nullptr, 4, order, 4) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    CHECK(invalid.admit(entries, 0, order, 0) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    CHECK(invalid.admit(entries, 4, nullptr, 4) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    CHECK(invalid.admit(entries, SIZE_MAX, order, SIZE_MAX) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    result = first;
    const auto unadmitted = invalid.port();
    CHECK(unadmitted.find(nullptr, 0x100, &result) == PS3_COMPILED_CODE_INVALID_ARGUMENT);
    CHECK(result == first);
    CHECK(invalid.admit(entries, 4, order, 4) == PS3_COMPILED_CODE_OK);
    PpuCompiledCodeTable other;
    const func_entry alternate[] = {{0x100, last, "alternate"}};
    CHECK(other.admit(alternate, 1, singleton, 1) == PS3_COMPILED_CODE_OK);
    ppu_context one{}, two{};
    std::thread a([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            ps3_compiled_function target = nullptr;
            if (table.lookup(0x100, &target) != PS3_COMPILED_CODE_OK) {
                std::terminate();
            }
            target(&one);
        }
    });
    std::thread b([&] {
        for (unsigned i = 0; i < 10000; ++i) {
            ps3_compiled_function target = nullptr;
            if (other.lookup(0x100, &target) != PS3_COMPILED_CODE_OK) {
                std::terminate();
            }
            target(&two);
        }
    });
    a.join(); b.join();
    CHECK(one.gpr[3] == 10000 && two.gpr[3] == 10000000);
    static_assert(!std::is_copy_constructible<PpuCompiledCodeTable>::value, "admitted identity cannot copy");
    std::printf("COMPILED_CODE_TABLE PASS checks=%u\n", checks);
    return 0;
}
