#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/spurs-registration.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
implementation="${PS3_SPURS_REGISTRATION_C:-$root/native/domain/spurs_workload_registration.c}"
read -r -a lanes <<<"${PS3_SPURS_REGISTRATION_LANES:-0 2 1}"
for optimization in "${lanes[@]}"; do
    case "$optimization" in
        0|2) sanitizer=address,undefined ;;
        1) sanitizer=thread ;;
        *) echo 'Invalid registration lane' >&2; exit 2 ;;
    esac
    "${CC:-cc}" -std=c11 -pthread "-O$optimization" -g -Wall -Wextra -Werror \
        -fno-omit-frame-pointer "-fsanitize=$sanitizer" \
        -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/sync" \
        "$root/native/tests/test_spurs_workload_registration.c" "$implementation" \
        "$root/../ps3recomp/runtime/memory/atomic_domain.c" \
        "$root/native/adapters/guard_pthread.c" -o "$build/registration-$optimization"
    echo "REGISTRATION_LANE O$optimization $sanitizer"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        TSAN_OPTIONS=halt_on_error=1 timeout 120s "$build/registration-$optimization"
done
echo SPURS_REGISTRATION_VERIFY_OK
