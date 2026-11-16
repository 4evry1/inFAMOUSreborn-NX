#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/generated-recorder.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
python3 "$root/tools/derive_ppu_memory_context.py" --fixture "$root/native/tests/memory_context_fixture_original.cpp" \
    --header "$root/../infamous-recomp/recompiled/ppu_recomp.h" --output "$build/derived"
includes=( -I"$build/derived" -I"$root/native/adapters" -I"$root/native/tests" \
    -I"$root/../ps3recomp/runtime/ppu" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/include" )
defines=( -DPPU_NO_INLINE_LOADS -DPPU_NO_INLINE_STORES -DPPU_NO_INLINE_STVX )
for optimization in 0 2; do
    flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer )
    if [[ ${GENERATED_RECORDER_SANITIZERS:-1} == 1 ]]; then flags+=( -fsanitize=address,undefined ); fi
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$build/domain.o"
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c "$root/native/tests/guest_space_host.c" -o "$build/space.o"
    "${CXX:-c++}" -std=c++17 -pthread "${flags[@]}" "${includes[@]}" "${defines[@]}" \
        "$root/native/tests/test_generated_recorder.cpp" "$build/derived/memory_context_fixture_original.cpp" \
        "$root/native/adapters/ppu_memory_context.cpp" "$root/native/adapters/ppu_memory_trace.cpp" \
        "$build/domain.o" "$build/space.o" -o "$build/contracts"
    "$build/contracts"
    printf 'Generated recorder O%s sanitizers=%s PASS\n' "$optimization" "${GENERATED_RECORDER_SANITIZERS:-1}"
done
