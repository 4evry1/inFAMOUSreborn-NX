#!/usr/bin/env bash
# Pure native workload-attribute domain verifier (native/domain/spurs_attributes.c only; no runtime/memory/SPURS collaborators).
# Lanes: O0 ASan+UBSan, O2 ASan+UBSan, O1 TSan. Run through evidence/startup-event-receive-r2/run_bounded.sh.
# Selectors (all optional; PS3_SPURS_* is forwarded by run_bounded.sh):
#   PS3_SPURS_WORKLOAD_VARIANTS="0 2 1"   optimisation lanes
#   PS3_SPURS_WORKLOAD_ATTRIBUTES_C       replacement spurs_attributes.c (mutant / red stand-in stub)
#   PS3_SPURS_WORKLOAD_TEST_C             replacement test source
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/spurs-workload-attr.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
attributes_c="${PS3_SPURS_WORKLOAD_ATTRIBUTES_C:-$root/native/domain/spurs_attributes.c}"
test_c="${PS3_SPURS_WORKLOAD_TEST_C:-$root/native/tests/test_spurs_workload_attributes.c}"
read -r -a levels <<<"${PS3_SPURS_WORKLOAD_VARIANTS:-0 2 1}"
for optimization in "${levels[@]}"; do
 case "$optimization" in 1) sanitizer=thread ;; 0|2) sanitizer=address,undefined ;; *) echo "bad variant $optimization" >&2; exit 2 ;; esac
 flags=( -std=c11 -pthread "-O$optimization" -g -Wall -Wextra -Werror -fno-omit-frame-pointer "-fsanitize=$sanitizer" -I"$root/native/domain" )
 "${CC:-cc}" "${flags[@]}" "$test_c" "$attributes_c" -o "$build/workload-$optimization"
 echo "== variant O$optimization $sanitizer =="
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 timeout 120s "$build/workload-$optimization"
done
echo "VERIFY OK"
