#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/ppu-trace-policy.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
includes=( -I"$root/native/adapters" -I"$root/native/tests" -I"$root/../ps3recomp/runtime/memory" )
for optimization in 0 2; do
    flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer )
    if [[ ${TRACE_POLICY_SANITIZERS:-1} == 1 ]]; then flags+=( -fsanitize=address,undefined ); fi
    "${CXX:-c++}" -std=c++17 "${flags[@]}" "${includes[@]}" \
        "$root/native/tests/test_ppu_memory_trace.cpp" "$root/native/adapters/ppu_memory_trace.cpp" -o "$build/legacy"
    "$build/legacy"
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c \
        "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$build/domain.o"
    "${CC:-cc}" -std=c11 "${flags[@]}" "${includes[@]}" -c \
        "$root/native/tests/guest_space_host.c" -o "$build/space.o"
    "${CXX:-c++}" -std=c++17 -pthread "${flags[@]}" "${includes[@]}" \
        "$root/native/tests/test_ppu_memory_trace_policy.cpp" "$root/native/adapters/ppu_memory_trace.cpp" \
        "$build/domain.o" "$build/space.o" -o "$build/policy"
    "$build/policy"
    printf 'PPU trace policy O%s sanitizers=%s PASS\n' "$optimization" "${TRACE_POLICY_SANITIZERS:-1}"
done
