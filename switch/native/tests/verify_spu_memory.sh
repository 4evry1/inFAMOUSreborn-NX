#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:-$(mktemp -d)}
mkdir -p "$output"
for optimization in 0 2; do
    for suite in atomic_domain spu_memory; do
        set -- "$root/native/tests/test_$suite.c" "$root/native/tests/guest_space_host.c" \
            "$root/native/adapters/guard_pthread.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
        if [ "$suite" = spu_memory ]; then set -- "$@" "$root/native/adapters/spu_memory.c"; fi
        cc -std=c11 -O"$optimization" -g -Wall -Wextra -Werror -pthread \
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
            -I"$root/../ps3recomp/runtime/memory" -I"$root/native/tests" \
            "$@" \
            -o "$output/$suite-O$optimization"
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
            timeout 30 "$output/$suite-O$optimization"
    done
done
