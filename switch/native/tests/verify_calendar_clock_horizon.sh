#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/infamous-calendar-adapter.XXXXXXXX")
trap 'rm -rf -- "$build"' EXIT
implementation=${CALENDAR_ADAPTER_IMPLEMENTATION:-$root/native/adapters/calendar_clock_horizon.c}
for optimization in 0 2; do
 flags=(-std=c11 "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer)
 if [[ ${CALENDAR_SANITIZERS:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined); fi
 includes=(-I"$root/native/adapters" -I"$root/../ps3recomp/runtime/time")
 set -x
 "${CC:-cc}" "${flags[@]}" "${includes[@]}" -Dclock_gettime=calendar_clock_test_gettime -c "$implementation" -o "$build/adapter.o"
 "${CC:-cc}" "${flags[@]}" "${includes[@]}" -DCALENDAR_CLOCK_TEST_SHIM "$root/native/tests/test_calendar_clock_horizon.c" "$root/../ps3recomp/runtime/time/calendar_clock.c" "$build/adapter.o" -o "$build/shim"
 "$build/shim"
 "${CC:-cc}" "${flags[@]}" "${includes[@]}" "$root/native/tests/test_calendar_clock_horizon.c" "$root/../ps3recomp/runtime/time/calendar_clock.c" "$implementation" -o "$build/host-real"
 "$build/host-real"
 set +x
done
