#!/bin/sh
# Host-only decrementer contract test (controlled clock), O0/O2 ASan/UBSan. Usage: <output dir>
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:-$(mktemp -d)}
mkdir -p "$output"
for optimization in 0 2; do
    cc -std=gnu11 -O"$optimization" -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
        "$root/native/tests/test_spu_decrementer.c" "$root/native/adapters/spu_decrementer.c" \
        "$root/../ps3recomp/runtime/time/timebase.c" -o "$output/spu-decrementer-O$optimization"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spu-decrementer-O$optimization"
done
