#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/infamous-ppu-budget.XXXXXXXX")
trap 'rm -rf -- "$build"' EXIT
python3 "$root/tools/derive_ppu_memory_context.py" --fixture "$root/native/tests/memory_context_fixture_original.cpp" --header "$root/../infamous-recomp/recompiled/ppu_recomp.h" --output "$build/derived"
for optimization in 0 2; do
 flags=("-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer -ffunction-sections -fdata-sections)
 if [[ ${DISCOVERY_BUDGET_SANITIZERS:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined); fi
 includes=(-I"$build/derived" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/../ps3recomp/runtime" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/ppu" -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/include")
 defines=(-DPPU_NO_INLINE_LOADS -DPPU_NO_INLINE_STORES -DPPU_NO_INLINE_STVX)
 set -x
 "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c "$root/native/tests/guest_space_host.c" -o "$build/host.o"
 "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$build/domain.o"
 "${CXX:-c++}" -std=c++17 "${flags[@]}" "${includes[@]}" "${defines[@]}" "$root/native/tests/test_ppu_memory_budget.cpp" "$root/native/adapters/ppu_memory_context.cpp" "$root/native/adapters/ppu_memory_trace.cpp" "$root/native/adapters/discovery_budget_binding.cpp" "$root/native/diagnostics/discovery_budget.cpp" "$build/host.o" "$build/domain.o" -pthread -Wl,--gc-sections -o "$build/test-O$optimization"
 "$build/test-O$optimization"
 set +x
done
