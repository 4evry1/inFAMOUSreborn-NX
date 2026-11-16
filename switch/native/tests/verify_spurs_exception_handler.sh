#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:?isolated output required}
mkdir -p "$output"
implementation=()
test ! -f "$root/native/domain/spurs_exception_handler.c" || implementation+=("$root/native/domain/spurs_exception_handler.c")
for optimization in 0 2; do
 cc -std=c11 -O"$optimization" -g -Wall -Wextra -Werror -pthread -fno-pie -no-pie -I"$root/../ps3recomp/runtime/memory" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/native/tests/test_spurs_exception_handler.c" "${implementation[@]}" \
  "$root/native/adapters/guard_pthread.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c" \
  -o "$output/spurs-exception-O$optimization"
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spurs-exception-O$optimization"
done
if test "${PS3_EXCEPTION_TSAN:-0}" = 1; then
 cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-error=tsan -pthread -fno-pie -no-pie -fsanitize=thread -I"$root/../ps3recomp/runtime/memory" \
  "$root/native/tests/test_spurs_exception_handler.c" "${implementation[@]}" \
  "$root/native/adapters/guard_pthread.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c" \
  -o "$output/spurs-exception-tsan"
 TSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spurs-exception-tsan"
fi
