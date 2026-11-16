#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?output directory required}
mkdir -p "$out"
implementation=()
if [[ -f "$root/native/adapters/system_language_horizon.cpp" ]]; then
  implementation+=("$root/native/adapters/system_language_horizon.cpp")
fi
for optimization in O0 O2; do
  c++ -std=c++17 "-$optimization" -g -Wall -Wextra -Werror \
    -fno-pie -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer \
    "$root/native/tests/test_system_language_horizon.cpp" "${implementation[@]}" \
    -o "$out/system-language-$optimization"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30 "$out/system-language-$optimization"
done
