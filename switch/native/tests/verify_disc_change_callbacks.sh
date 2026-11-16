#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?owned output directory required}
mkdir -p "$out"
implementation=()
if [[ -f "$root/native/domain/disc_change_callbacks.c" ]]; then implementation+=("$root/native/domain/disc_change_callbacks.c"); fi
for variant in '0 address,undefined' '2 address,undefined' '1 thread'; do
  read -r optimization sanitizer <<<"$variant"
  cc -std=c11 -pthread "-O$optimization" -g -Wall -Wextra -Werror \
    -fno-pie -no-pie "-fsanitize=$sanitizer" -fno-omit-frame-pointer \
    "$root/native/tests/test_disc_change_callbacks.c" "$root/native/adapters/guard_pthread.c" \
    "${implementation[@]}" -o "$out/disc-change-O$optimization"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TSAN_OPTIONS=halt_on_error=1 \
    timeout 30 "$out/disc-change-O$optimization"
done
