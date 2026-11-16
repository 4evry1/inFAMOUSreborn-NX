#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/startup-current-time-flags.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
python3 "$root/tools/derive_ppu_memory_context.py" --fixture "$root/native/tests/memory_context_fixture_original.cpp" --header "$root/../infamous-recomp/recompiled/ppu_recomp.h" --output "$build/derived"
printf '#include "startup_execution.h"\nstatic_assert(sizeof(ppu_context) == 1104);\n' > "$build/contract.cpp"
includes=( -I"$build/derived" -I"$root/native/adapters" -I"$root/native/diagnostics"
 -I"$root/../ps3recomp/runtime/ppu" -I"$root/../ps3recomp/runtime/memory"
 -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/include" )
base=( -DPPU_NO_INLINE_LOADS -DPPU_NO_INLINE_STORES -DPPU_NO_INLINE_STVX -DPS3_NATIVE_CONTEXT_MEMORY )
required=( PS3_NATIVE_STARTUP_LWMUTEX PS3_NATIVE_STARTUP_MEMORY PS3_NATIVE_STARTUP_CLOCK PS3_NATIVE_STARTUP_TIMEBASE )
all=( -DPS3_NATIVE_STARTUP_CURRENT_TIME )
for flag in "${required[@]}"; do all+=( "-D$flag" ); done
compile() {
 "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsyntax-only "${includes[@]}" "${base[@]}" "$@" "$build/contract.cpp"
}
compile "${all[@]}" > "$build/all-on.log" 2>&1
printf 'Current-time feature guard all-on PASS\n'
compile > "$build/default-off.log" 2>&1
printf 'Current-time feature guard default-off PASS\n'
diagnostic='Current-time variant requires STARTUP_LWMUTEX, STARTUP_MEMORY, STARTUP_CLOCK and STARTUP_TIMEBASE'
for missing in "${required[@]}"; do
 selected=( -DPS3_NATIVE_STARTUP_CURRENT_TIME )
 for flag in "${required[@]}"; do
  if [[ $flag != "$missing" ]]; then selected+=( "-D$flag" ); fi
 done
 if compile "${selected[@]}" > "$build/$missing.log" 2>&1; then
  printf 'Current-time feature guard unexpectedly accepted missing %s\n' "$missing" >&2
  exit 1
 fi
 if ! grep -Fq "$diagnostic" "$build/$missing.log"; then
  cat "$build/$missing.log" >&2
  printf 'Current-time feature guard missing intended diagnostic for %s\n' "$missing" >&2
  exit 1
 fi
 cat "$build/$missing.log"
 printf 'Current-time feature guard missing=%s intended-diagnostic PASS\n' "$missing"
done
