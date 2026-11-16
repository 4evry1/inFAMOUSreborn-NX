#!/usr/bin/env bash
# Decision 33 / C1a: the native SPU interpreter on the existing SPU executor.
#
# usage: verify_spu_interp.sh <fresh out dir>      (the directory must not exist)
#
# Steps:
#   1. lift the user's own libsre.prx into <out>/lift (evidence/lle-sre-a1/lift_libsre.sh): the kernel's SPU ELF
#   2. build the test three times -- O0 and O2 under ASan+UBSan, O1 under TSan -- and run each
#
# The test covers hand-assembled programs, a differential oracle against the desktop interpreter
# (source/ps3recomp/runtime/spu/spu_interp.c), the C1a channels, and the real SPURS kernel from LS 0x818.
# Everything derived from the user's firmware lives only under <out>.
# Caller-bounded: run inside evidence/startup-event-receive-r2/run_bounded.sh. No network, no sudo.
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?usage: verify_spu_interp.sh <fresh out dir>}"
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"
echo "scope: host behaviour test only; no game boot, no Switch build (root owns that), no network"
echo "out: $out"

echo "=== step 1: lift libsre.prx (the SPURS kernel image) ==="
bash "$root/evidence/lle-sre-a1/lift_libsre.sh" "$out/lift" > "$out/lift.log" 2>&1 || {
    tail -20 "$out/lift.log" >&2; echo "LIFT_FAILED" >&2; exit 1; }
image="$out/lift/prx/libsre_image.bin"
[ -s "$image" ] || { echo "LIFT_IMAGE_MISSING $image" >&2; exit 1; }
echo "image: $image ($(stat -c%s "$image") bytes)"

# The test includes spu_interp.h; the desktop interpreter is built from the same source tree with its
# vector/LS-probe paths off, so the oracle compares instruction semantics and nothing else.
includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory"
           -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/include"
           -I"$root/../ps3recomp/libs/video" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain" )
native_units=( "$root/native/adapters/spu_executor.c" "$root/native/adapters/spu_interp_native.c"
               "$root/native/adapters/fp_control_x86.c" "$root/native/adapters/spu_memory.c"
               "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c"
               "$root/native/adapters/spu_decrementer.c" "$root/../ps3recomp/runtime/time/timebase.c"
               "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
               "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c" )
desktop_units=( "$root/../ps3recomp/runtime/spu/spu_interp.c" "$root/native/tests/spu_interp_oracle_host.c" )

overall=0
run_variant() {
    local optimization="$1" sanitizer="$2" label="$3"
    local dir="$out/$label"; mkdir -p "$dir"
    # -Wno-misleading-indentation: spu_helpers.h and the desktop spu_interp.c both trip it (pre-existing, unrelated to
    # C1a, and not ours to reformat here).
    local flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -Wno-misleading-indentation -fno-omit-frame-pointer
                  "-fsanitize=$sanitizer" )
    if [[ "$sanitizer" == thread ]]; then flags+=( -Wno-error=tsan ); fi
    local objects=()
    local source
    for source in "${native_units[@]}"; do
        local object="$dir/$(basename "${source%.c}").o"
        "${CC:-cc}" -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL \
            "${includes[@]}" -c "$source" -o "$object"
        objects+=( "$object" )
    done
    for source in "${desktop_units[@]}"; do
        local object="$dir/$(basename "${source%.c}").o"
        # Desktop-only units: no PS3_NATIVE_SPU_EXECUTOR (they are the desktop runtime's own code).
        "${CC:-cc}" -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DSPU_INTERP_NO_VEC -DSPU_XFLOAT_RUNTIME \
            "${includes[@]}" -c "$source" -o "$object"
        objects+=( "$object" )
    done
    "${CC:-cc}" -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL \
        "${includes[@]}" "$root/native/tests/test_spu_interp.c" \
        "${objects[@]}" -lm -o "$dir/spu-interp"
    nm "$dir/spu-interp" > "$dir/symbols.txt"
    # The oracle unit necessarily defines the desktop float switch; nothing else may.
    if [ "$(grep -cE ' (g_spu_xfloat|g_spu_xflim)$' "$dir/symbols.txt")" -ne 2 ]; then
        echo "UNEXPECTED_FLOAT_POLICY_SYMBOLS" >&2; grep -E ' (g_spu_xfloat|g_spu_xflim)$' "$dir/symbols.txt" >&2; return 5; fi
    echo "== variant O$optimization $sanitizer =="
    set +e
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    TSAN_OPTIONS=halt_on_error=1 timeout 900s "$dir/spu-interp" "$image" 2>&1 | tee "$dir/run.log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ]; then echo "SPU_INTERP_RUN_FAILED variant=O$optimization/$sanitizer exit=$rc" >&2; return 1; fi
    grep -q '^SPU_KERNEL_FIRST_STOP ' "$dir/run.log" || { echo "SPU_KERNEL_FIRST_STOP_MISSING" >&2; return 1; }
    grep -q '^overall=PASS$' "$dir/run.log" || { echo "SPU_INTERP_OVERALL_FAILED" >&2; return 1; }
    echo "variant=O$optimization/$sanitizer PASS"
}

IFS=";" read -ra variants <<<"${PS3_SPU_INTERP_VARIANTS:-0 address,undefined;2 address,undefined;1 thread}"
for variant in "${variants[@]}"; do
    read -r optimization sanitizer <<<"$variant"
    if ! run_variant "$optimization" "$sanitizer" "v$optimization"; then overall=1; fi
done

if [ "$overall" -eq 0 ]; then
    echo "--- SPU_KERNEL_FIRST_STOP (O2/ASan+UBSan) ---"
    grep '^SPU_KERNEL_FIRST_STOP ' "$out/v2/run.log" || true
fi
echo "overall=$([ "$overall" -eq 0 ] && echo PASS || echo FAIL)"
exit "$overall"
