#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build=$(mktemp -d "${TMPDIR:-/tmp}/infamous-calendar-port.XXXXXXXX")
trap 'rm -rf -- "$build"' EXIT
implementation=${CALENDAR_PORT_IMPLEMENTATION:-$root/../ps3recomp/runtime/time/calendar_clock.c}
for optimization in 0 2; do
 flags=(-std=c11 "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer)
 if [[ ${CALENDAR_SANITIZERS:-0} == 1 ]]; then flags+=(-fsanitize=address,undefined); fi
 command=("${CC:-cc}" "${flags[@]}" -I"$root/../ps3recomp/runtime/time" "$root/native/tests/test_calendar_clock_port.c" "$implementation" -o "$build/test-O$optimization")
 printf 'COMMAND '; printf '%q ' "${command[@]}"; printf '\n'
 "${command[@]}"; "$build/test-O$optimization"
done
