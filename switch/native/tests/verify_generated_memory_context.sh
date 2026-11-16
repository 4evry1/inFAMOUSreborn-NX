#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/ppu-memory-contract.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
python3 "$root/tools/derive_ppu_memory_context.py" \
    --fixture "$root/native/tests/memory_context_fixture_original.cpp" \
    --header "$root/../infamous-recomp/recompiled/ppu_recomp.h" --output "$build/derived"
includes=( -I"$build/derived" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/../infamous-recomp/recompiled"
    -I"$root/../ps3recomp/runtime/ppu" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/time"
    -I"$root/../ps3recomp/include" )
defines=( -DPPU_NO_INLINE_LOADS -DPPU_NO_INLINE_STORES -DPPU_NO_INLINE_STVX )
for optimization in 0 2; do
    flags=( "-O$optimization" -g -fno-omit-frame-pointer -fsanitize=address,undefined )
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c \
        "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$build/domain.o"
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c \
        "$root/native/tests/guest_space_host.c" -o "$build/space.o"
    "${CXX:-c++}" -std=c++17 -pthread "${flags[@]}" "${includes[@]}" "${defines[@]}" \
        "$root/native/tests/test_generated_memory_context.cpp" \
        "$build/derived/memory_context_fixture_original.cpp" \
        "$root/native/adapters/ppu_memory_context.cpp" "$root/native/adapters/ppu_memory_trace.cpp" "$root/native/adapters/discovery_budget_binding.cpp" "$root/native/diagnostics/discovery_budget.cpp" "$build/domain.o" "$build/space.o" -o "$build/contracts"
    "$build/contracts"
    printf 'PPU memory host contracts O%s ASan/UBSan PASS\n' "$optimization"
done
