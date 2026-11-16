#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:?isolated output required}
# optional second argument: a directory whose spu_dma.h is searched FIRST (the original, to show the test fails before the fix)
before=${2:-}
mkdir -p "$output"
for optimization in 0 2; do
 cc -std=gnu11 -O"$optimization" -g -Wall -Wextra -Werror -msse4.1 -pthread \
  -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL \
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie -ffunction-sections -fdata-sections \
  ${before:+-I"$before"} -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory" -I"$root/native/tests" \
  "$root/native/tests/test_spu_list_dma_empty.c" "$root/native/adapters/spu_executor.c" "$root/native/adapters/fp_control_x86.c" \
  "$root/native/adapters/spu_memory.c" "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c" \
  "$root/native/adapters/spu_decrementer.c" "$root/../ps3recomp/runtime/time/timebase.c" \
  "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c" \
  "$root/../ps3recomp/runtime/spu/spu_channels.c" -Wl,--gc-sections -lm -o "$output/spu-list-dma-empty-O$optimization"
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 30 "$output/spu-list-dma-empty-O$optimization"
done
