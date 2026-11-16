#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/infamous-startup-observation.XXXXXXXX")
trap 'rm -rf -- "$build"' EXIT
ulimit -c 0
compiler=${CXX:-c++}
implementation=${STARTUP_OBSERVATION_IMPLEMENTATION:-$root/native/diagnostics/startup_observation.cpp}
for optimization in 0 2; do
 flags=(-std=c++17 "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer)
 if [[ ${STARTUP_OBSERVATION_SANITIZERS:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined); fi
 command=("$compiler" "${flags[@]}" -I"$root/native/diagnostics" "$root/native/tests/test_startup_observation.cpp" "$implementation" -o "$build/test-O$optimization")
 printf 'COMMAND '; printf '%q ' "${command[@]}"; printf '\n'
 "${command[@]}"
 timeout --kill-after=2s 30s "$build/test-O$optimization"
done
