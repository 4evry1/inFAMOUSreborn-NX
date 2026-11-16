#include "compiled_code_table.h"
#include <cstdint>
#include <limits>
#include <type_traits>

static_assert(std::is_same<decltype(func_entry::func), ps3_compiled_function>::value,
    "Borrowed generated targets must have the inward CPU function signature");

ps3_compiled_code_status PpuCompiledCodeTable::admit(const func_entry *entries,
    size_t entry_count, const uint32_t *sorted_offsets, size_t offset_count)
{
    if (count_ != 0) {
        return PS3_COMPILED_CODE_ALREADY_ADMITTED;
    }
    if (!entries || !sorted_offsets || entry_count == 0 ||
        entry_count != offset_count || entry_count > UINT32_MAX ||
        entry_count > std::numeric_limits<size_t>::max() / sizeof(func_entry) ||
        offset_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t)) {
        return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    }
    uint64_t previous = 0;
    for (size_t position = 0; position < offset_count; ++position) {
        const uint32_t offset = sorted_offsets[position];
        if (offset >= entry_count) {
            return PS3_COMPILED_CODE_INVALID_TABLE;
        }
        const func_entry &entry = entries[offset];
        if (entry.addr == 0 || entry.addr > UINT32_MAX || (entry.addr & 3) != 0 ||
            !entry.func || entry.addr <= previous) {
            return PS3_COMPILED_CODE_INVALID_TABLE;
        }
        previous = entry.addr;
    }
    // Equal counts, in-range offsets and strictly increasing addresses imply
    // every index appears exactly once; no temporary bitmap or table is needed.
    entries_ = entries;
    offsets_ = sorted_offsets;
    count_ = entry_count;
    return PS3_COMPILED_CODE_OK;
}

ps3_compiled_code_status PpuCompiledCodeTable::lookup(uint32_t address,
    ps3_compiled_function *target) const
{
    if (!target) {
        return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    }
    if (count_ == 0) {
        return PS3_COMPILED_CODE_NOT_ADMITTED;
    }
    if (address == 0 || (address & 3) != 0) {
        return PS3_COMPILED_CODE_INVALID_ADDRESS;
    }
    size_t first = 0;
    size_t limit = count_;
    while (first < limit) {
        const size_t middle = first + (limit - first) / 2;
        const func_entry &entry = entries_[offsets_[middle]];
        if (entry.addr < address) {
            first = middle + 1;
        } else if (entry.addr > address) {
            limit = middle;
        } else {
            *target = entry.func;
            return PS3_COMPILED_CODE_OK;
        }
    }
    return PS3_COMPILED_CODE_NOT_FOUND;
}

ps3_compiled_code_status PpuCompiledCodeTable::find(const void *context,
    uint32_t address, ps3_compiled_function *target)
{
    if (!context) {
        return PS3_COMPILED_CODE_INVALID_ARGUMENT;
    }
    return static_cast<const PpuCompiledCodeTable *>(context)->lookup(address, target);
}

ps3_compiled_code_lookup PpuCompiledCodeTable::port() const
{
    return {this, find};
}
