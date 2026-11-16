#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:?isolated output required}
mkdir -p "$output"
implementation=()
if test -n "${PS3_READY_COUNT_IMPLEMENTATION:-}"; then
 implementation+=("$PS3_READY_COUNT_IMPLEMENTATION")
elif test -f "$root/native/domain/spurs_ready_count.c"; then
 implementation+=("$root/native/domain/spurs_ready_count.c")
fi
for optimization in 0 2; do
 cc -std=c11 -O"$optimization" -g -Wall -Wextra -Werror -pthread -fno-pie -no-pie -I"$root/native/domain" \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/native/tests/test_spurs_ready_count.c" "${implementation[@]}" \
  "$root/native/adapters/guard_pthread.c" "$root/native/adapters/wait_port_pthread.c" \
  "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$output/spurs-ready-count-O$optimization"
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spurs-ready-count-O$optimization"
done
if test "${PS3_READY_COUNT_TSAN:-0}" = 1; then
 cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-error=tsan -pthread -fno-pie -no-pie -fsanitize=thread -I"$root/native/domain" \
  "$root/native/tests/test_spurs_ready_count.c" "${implementation[@]}" \
  "$root/native/adapters/guard_pthread.c" "$root/native/adapters/wait_port_pthread.c" \
  "$root/../ps3recomp/runtime/memory/atomic_domain.c" -o "$output/spurs-ready-count-tsan"
 TSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spurs-ready-count-tsan"
fi
