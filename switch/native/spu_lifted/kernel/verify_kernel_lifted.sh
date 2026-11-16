#!/usr/bin/env bash
# Host differential check of the lifted SPURS kernel against the native SPU interpreter (Decision 133, 2026-11-16).
#
# usage: verify_kernel_lifted.sh <fresh out dir> [test options...]        (default options: --trials 200)
#   env: KERNEL_LIBSRE_IMAGE  the libsre image of a build's own lift (default: the newest build/*/lift/prx/libsre_image.bin)
#        KERNEL_OPT           optimisation level of the lifted translation units (default 2)
#        KERNEL_CC            compiler (default cc). KERNEL_CC=<a64cc> KERNEL_RUN=<a64run> builds and runs the test for AArch64 (fp_control_arm64.c; KERNEL_STATIC=1 links it static)
#        KERNEL_RUN           a wrapper the test binary runs under (default: none)
# Steps: (1) generate the kernel module from the libsre image into <out>/gen (gen_kernel_lifted.py: nothing is written outside <out>) and the toy
# policy module into <out>/toy (make_toy_pm.py); (2) compile the runtime, the glue, the accounting unit, both modules and the test; (3) run it.
# No network; nothing is staged anywhere.
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="${1:?usage: verify_kernel_lifted.sh <fresh out dir> [test options...]}"; shift || true
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"; out="$(cd -- "$out" && pwd)"
here="$root/native/spu_lifted/kernel"
image="${KERNEL_LIBSRE_IMAGE:-$(python3 -I -c "
import glob, os
c = [p for p in glob.glob('$root/build/*/lift/prx/libsre_image.bin')]
print(max(c, key=os.path.getmtime) if c else '')")}"
[ -f "$image" ] || { echo "NO_LIBSRE_IMAGE (set KERNEL_LIBSRE_IMAGE)" >&2; exit 2; }
opt="${KERNEL_OPT:-2}"; cc="${KERNEL_CC:-cc}"
[ "$#" -gt 0 ] || set -- --trials 200
echo "scope: host behaviour test only; no game boot, no Switch build, no network"
echo "out: $out  libsre image: $image  compiler: $cc  lifted -O$opt  test options: $*"

echo "=== step 1: generate ==="
python3 -I "$here/gen_kernel_lifted.py" --libsre-image "$image" --out "$out/gen"
python3 -I "$here/make_toy_pm.py" --out "$out/toy"

echo "=== step 2: compile ==="
case "$($cc -dumpmachine)" in
    aarch64*) fp="$root/native/adapters/fp_control_arm64.c"; arch=() ;;
    *)        fp="$root/native/adapters/fp_control_x86.c"; arch=( -msse4.1 ) ;;
esac
includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/time"
           -I"$root/../ps3recomp/include" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain"
           -I"$root/native/spu_lifted" -I"$root/native/spu_lifted/jobs" -I"$here" )
defs=( -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL )
common=( -std=gnu11 -pthread -g -DNDEBUG -fno-omit-frame-pointer "${arch[@]}" -Wno-misleading-indentation -ffp-contract=off -fno-strict-aliasing )
runtime_units=( "$root/native/adapters/spu_executor.c" "$root/native/adapters/spu_interp_native.c" "$fp" "$root/native/adapters/spu_memory.c"
                "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c" "$root/native/adapters/spu_decrementer.c"
                "$root/../ps3recomp/runtime/time/timebase.c" "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
                "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c" "$root/native/spu_lifted/spu_lifted_hybrid.c"
                "$root/native/adapters/spu_image_account.c" )
objects=()
for source in "${runtime_units[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    "$cc" "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -c "$source" -o "$object"
    objects+=( "$object" )
done
for unit in "$out/gen/jobmod_spurs_kernel_e818_rc.c" "$out/toy/jobmod_toypm_eA00_rc.c"; do
    "$cc" "${common[@]}" "-O$opt" -frounding-math "${defs[@]}" "${includes[@]}" -Wall -Wextra -Werror -Wno-unused-variable -Wno-unused-but-set-variable \
        -Wno-unused-parameter -Wno-unused-label -DPS3_LIFTED_TEST_RCHCNT_SPLAT -c "$unit" -o "$out/$(basename "${unit%.c}").o"
    objects+=( "$out/$(basename "${unit%.c}").o" )
done
for unit in "$out/gen/jobmod_spurs_kernel_e818_module.c" "$out/toy/jobmod_toypm_eA00_module.c" "$here/wws_restore_stub.c" "$here/test_kernel_lifted.c"; do
    "$cc" "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -Wall -Wextra -c "$unit" -o "$out/$(basename "${unit%.c}").o"
    objects+=( "$out/$(basename "${unit%.c}").o" )
done
"$cc" "${common[@]}" -O2 "${objects[@]}" -lm ${KERNEL_STATIC:+-static} -o "$out/test-kernel-lifted"

echo "=== step 3: run ==="
set +e
timeout 3600s ${KERNEL_RUN:-} "$out/test-kernel-lifted" "$image" "$out/toy/toy_pm.bin" "$@" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
[ "$rc" -eq 0 ] || { echo "KERNEL_LIFTED_RUN_FAILED exit=$rc" >&2; exit 1; }
python3 -I -c "import sys; sys.exit(0 if 'overall=PASS' in open('$out/run.log').read().split('\n') else 1)" || { echo "KERNEL_LIFTED_OVERALL_FAILED" >&2; exit 1; }
echo "overall=PASS"
