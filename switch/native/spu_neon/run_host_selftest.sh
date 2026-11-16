#!/usr/bin/env bash
# Host run of the SPU fast-helper self-test (native/spu_neon/spu_neon_selftest.c).
# usage: run_host_selftest.sh <fresh out dir>
#
# Builds and runs the SAME self-test source four ways on the x86-64 host:
#   sse41     -O2 -msse4.1            the SSE bodies of spu_helpers.h / spu_context.h against the scalar _ref bodies
#   avx2      -O2 -mavx2 -mfma        the same with the FMA float path
#   neonshim  -O2, SSE macros undefined, -D__ARM_NEON -DSPU_NEON_SHIM, include native/spu_neon/neon_shim:
#             the NEON bodies compiled against a portable emulation of <arm_neon.h> (checks the lane logic of every
#             NEON body; it does not check the real instructions -- that is the console run)
#   neonshim0 the same at -O0 (no inlining / folding)
#   scalar    -O2, SSE macros undefined, no NEON: the portable scalar branch against its own _ref copies
# Every variant must print NEON_SELFTEST_TOTAL ... mismatching_ops=0. Exit status is the number of failing variants.
set -uo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?usage: run_host_selftest.sh <fresh out dir>}"
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"
here="$root/native/spu_neon"
inc=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/include" )
defs=( -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL -DSPU_NEON_SELFTEST_MAIN )
base=( -std=gnu11 -g -Wall -Wextra -ffp-contract=off -Wno-misleading-indentation )
nosse=( -U__SSE2__ -U__SSE__ -U__SSE3__ -U__SSSE3__ -U__SSE4_1__ -U__SSE4_2__ -U__AVX__ -U__AVX2__ -U__FMA__ )
fail=0
run_variant() {
    local name="$1"; shift
    echo "=== variant $name"
    if ! cc "${base[@]}" "$@" "${defs[@]}" "${inc[@]}" "$here/spu_neon_selftest.c" -lm -o "$out/selftest-$name" > "$out/compile-$name.log" 2>&1; then
        echo "COMPILE_FAILED $name (log: $out/compile-$name.log)"; fail=$((fail + 1)); return
    fi
    echo "warnings: $(grep -c 'warning:' "$out/compile-$name.log" || true)"
    "$out/selftest-$name" > "$out/report-$name.txt" 2>&1; local rc=$?
    grep -E '^NEON_SELFTEST_(INFO|TOTAL)|mismatches=[1-9]' "$out/report-$name.txt"
    if [ "$rc" -ne 0 ]; then echo "VARIANT_FAILED $name rc=$rc"; fail=$((fail + 1)); fi
}
run_variant sse41    -O2 -msse4.1
run_variant avx2     -O2 -msse4.1 -mavx2 -mfma
run_variant neonshim -O2 "${nosse[@]}" -D__ARM_NEON=1 -DSPU_NEON_SHIM -I"$here/neon_shim"
run_variant neonshim0 -O0 "${nosse[@]}" -D__ARM_NEON=1 -DSPU_NEON_SHIM -I"$here/neon_shim"
run_variant scalar   -O2 "${nosse[@]}"
echo "failing variants: $fail"
exit "$fail"
