#!/usr/bin/env bash
# Root (2026-11-12): build and run the SPU interpreter speed benchmark on the host (no sanitizers, -O2).
# usage: bench_spu_interp.sh <fresh out dir> [iterations]
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?usage: bench_spu_interp.sh <fresh out dir> [iterations]}"
[ ! -e "$out" ] || { echo "REFUSED: $out already exists" >&2; exit 2; }
mkdir -p "$out"
includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory"
           -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/include"
           -I"$root/../ps3recomp/libs/video" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain" )
units=( "$root/native/adapters/spu_executor.c" "$root/native/adapters/spu_interp_native.c"
        "$root/native/adapters/fp_control_x86.c" "$root/native/adapters/spu_memory.c"
        "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c"
        "$root/native/adapters/spu_decrementer.c" "$root/../ps3recomp/runtime/time/timebase.c"
        "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
        "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c" )
desktop=( "$root/../ps3recomp/runtime/spu/spu_interp.c" "$root/native/tests/spu_interp_oracle_host.c" )
flags=( -O2 -g -DNDEBUG -fno-omit-frame-pointer -Wno-misleading-indentation )
objects=()
for source in "${units[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    cc -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL "${includes[@]}" -c "$source" -o "$object"
    objects+=( "$object" )
done
for source in "${desktop[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    cc -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DSPU_INTERP_NO_VEC -DSPU_XFLOAT_RUNTIME "${includes[@]}" -c "$source" -o "$object"
    objects+=( "$object" )
done
cc -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL "${includes[@]}" \
    "$root/native/tests/bench_spu_interp.c" "${objects[@]}" -lm -o "$out/bench-spu-interp"
"$out/bench-spu-interp" "${2:-20000000}"
