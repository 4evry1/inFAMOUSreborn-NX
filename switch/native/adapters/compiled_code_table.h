#ifndef NATIVE_COMPILED_CODE_TABLE_H
#define NATIVE_COMPILED_CODE_TABLE_H
#include "compiled_code.h"
#include "ppu_recomp.h"
#include <cstddef>

/* Borrow the public generated type directly. Composition retains both arrays
 * unchanged until all users join. Admission is exclusive on a fresh/quiescent
 * instance; after success only const lookup is permitted. Array storage must
 * contain the stated counts, and target outputs must not alias that storage.
 * No sentinel is read: generated function_table_count excludes it. */
class PpuCompiledCodeTable {
public:
    PpuCompiledCodeTable() = default;
    PpuCompiledCodeTable(const PpuCompiledCodeTable &) = delete;
    PpuCompiledCodeTable &operator=(const PpuCompiledCodeTable &) = delete;
    PpuCompiledCodeTable(PpuCompiledCodeTable &&) = delete;
    PpuCompiledCodeTable &operator=(PpuCompiledCodeTable &&) = delete;
    ps3_compiled_code_status admit(const func_entry *entries, size_t entry_count,
        const uint32_t *sorted_offsets, size_t offset_count);
    ps3_compiled_code_status lookup(uint32_t address,
        ps3_compiled_function *target) const;
    ps3_compiled_code_lookup port() const;
private:
    static ps3_compiled_code_status find(const void *context, uint32_t address,
        ps3_compiled_function *target);
    const func_entry *entries_ = nullptr;
    const uint32_t *offsets_ = nullptr;
    size_t count_ = 0;
};
#endif
