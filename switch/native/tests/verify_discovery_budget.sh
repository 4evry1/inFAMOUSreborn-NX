#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/infamous-discovery-budget.XXXXXXXX")
trap 'rm -rf -- "$build"' EXIT
compiler=${CXX:-c++}
implementation=${DISCOVERY_BUDGET_IMPLEMENTATION:-$root/native/diagnostics/discovery_budget.cpp}
for optimization in 0 2; do
  flags=(-std=c++17 "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer)
  if [[ ${DISCOVERY_BUDGET_SANITIZERS:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined); fi
  command=("$compiler" "${flags[@]}" -I"$root/native/diagnostics" "$root/native/tests/test_discovery_budget.cpp" "$implementation" -o "$build/test-O$optimization")
  printf 'COMMAND '; printf '%q ' "${command[@]}"; printf '\n'
  "${command[@]}"
  "$build/test-O$optimization"
done
