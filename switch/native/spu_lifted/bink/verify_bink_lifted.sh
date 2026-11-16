#!/usr/bin/env bash
# Host differential check of the lifted inFAMOUS Bink movie decoder against the native SPU interpreter.
#
# usage: verify_bink_lifted.sh <fresh out dir> [test options...]
#   env: BINK_ELF      the user's own EBOOT.elf (default: the stage-11 probe copy inside the workspace; read in place)
#        BINK_ELF_OFF  offset of the decoder's SPU ELF inside it (default 0x673E80)
#        BINK_OPT      optimisation level of the lifted translation unit (default 2; use 1 if -O2 is too slow/large)
#        BINK_SAN      extra sanitizer flags for every unit, e.g. "-fsanitize=address,undefined" (default none)
#        BINK_SKIP_REGEN=1  do not regenerate-and-compare the generated sources
# Steps: (1) regenerate native/spu_lifted/bink/generated from the lifter output into <out>/regen and compare with the
# checked-in copy; (2) build the runtime, the glue and the adapted lifted TU twice (test build with the rchcnt-splat
# emulation, production build) ; (3) check the production object for non-tail transfers; (4) run the test.
# Heavy compiles run under systemd-run with an 8 GiB memory cap, at most two compile jobs at once. No network.
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="${1:?usage: verify_bink_lifted.sh <fresh out dir> [test options...]}"; shift || true
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"
out="$(cd -- "$out" && pwd)"
elf="${BINK_ELF:-$root/build/emulator-stage11-context-0150-r1-final/data/eden/sdmc/switch/infamous-native-probe/EBOOT.elf}"
elf_off="${BINK_ELF_OFF:-0x673E80}"
opt="${BINK_OPT:-2}"
san=( ${BINK_SAN:-} )
here="$root/native/spu_lifted/bink"
echo "scope: host behaviour test only; no game boot, no Switch build, no network"
echo "out: $out  elf: $elf @ $elf_off  lifted -O$opt  sanitizers: ${san[*]:-none}"
[ -r "$elf" ] || { echo "MISSING_ELF $elf" >&2; exit 1; }

if [ -z "${BINK_SKIP_REGEN:-}" ]; then
    echo "=== step 1: regenerate the adapted lifted sources and compare with the checked-in copy ==="
    python3 -I "$here/gen_bink_lifted.py" --rc "$root/../infamous-recomp/spu/spu_0001_at_00673E80/spu_recomp_rc.c" \
        --elf "$elf" --elf-offset "$elf_off" --out "$out/regen" | tee "$out/regen.log"
    for f in bink_lifted_rc.c bink_lifted_rows.c bink_lifted_meta.h; do
        cmp "$out/regen/$f" "$here/generated/$f" || { echo "GENERATED_SOURCE_STALE $f" >&2; exit 1; }
    done
    echo "generated sources identical to a fresh regeneration"
fi

includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory"
           -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/include"
           -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain"
           -I"$root/native/spu_lifted" -I"$here" )
defs=( -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL )
common=( -std=gnu11 -pthread -g -DNDEBUG -fno-omit-frame-pointer -msse4.1 -Wno-misleading-indentation -ffp-contract=off "${san[@]}" )
cap=( systemd-run --user --scope --quiet -p MemoryMax=8G -p MemorySwapMax=0 )

# ---- the lifted translation units (the slow compiles): test build (splat emulation) and production build, in parallel
build_lifted() {
    local label="$1"; shift
    ( cd "$out" && "${cap[@]}" cc "${common[@]}" "-O$opt" -frounding-math "${defs[@]}" "${includes[@]}" \
        -Wno-unused-variable -Wno-unused-but-set-variable -Wall -Wextra "$@" \
        -c "$here/generated/bink_lifted_rc.c" -o "$out/lifted_$label.o" ) > "$out/compile_lifted_$label.log" 2>&1
}
t0=$(date +%s)
build_lifted test -DPS3_LIFTED_TEST_RCHCNT_SPLAT & job_test=$!
build_lifted production & job_prod=$!

echo "=== step 2: runtime, glue, test ==="
runtime_units=( "$root/native/adapters/spu_executor.c" "$root/native/adapters/spu_interp_native.c"
                "$root/native/adapters/fp_control_x86.c" "$root/native/adapters/spu_memory.c"
                "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c"
                "$root/native/adapters/spu_decrementer.c" "$root/../ps3recomp/runtime/time/timebase.c"
                "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
                "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c"
                "$root/native/spu_lifted/spu_lifted_hybrid.c" "$here/bink_module.c" "$here/generated/bink_lifted_rows.c" )
objects=()
for source in "${runtime_units[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -c "$source" -o "$object"
    objects+=( "$object" )
done
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -c "$here/test_bink_lifted.c" -o "$out/test_bink_lifted.o"

wait $job_test || { echo "LIFTED_COMPILE_FAILED (test build)"; tail -30 "$out/compile_lifted_test.log"; exit 1; }
wait $job_prod || { echo "LIFTED_COMPILE_FAILED (production build)"; tail -30 "$out/compile_lifted_production.log"; exit 1; }
t1=$(date +%s)
echo "lifted translation units compiled at -O$opt in $((t1 - t0)) s (both builds, in parallel)"
for label in test production; do
    echo "warnings ($label): $(grep -c 'warning:' "$out/compile_lifted_$label.log" || true)"
done
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" "$out/test_bink_lifted.o" "$out/lifted_test.o" "${objects[@]}" -lm -o "$out/test-bink-lifted"
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" "$out/test_bink_lifted.o" "$out/lifted_production.o" "${objects[@]}" -lm -o "$out/test-bink-lifted-production"
nm "$out/test-bink-lifted" > "$out/symbols.txt"
nm "$out/lifted_production.o" | grep ' U ' > "$out/lifted_undefined.txt" || true
# the lifted object must not reference the desktop registry or the fast-tail TLS flag
if grep -E ' U (spu_register_function|spu_rc_add_pairs|g_spu_fast_tail|g_spu_trampoline_fn)$' "$out/lifted_undefined.txt"; then
    echo "UNEXPECTED_DESKTOP_SYMBOL_REFERENCES" >&2; exit 1; fi
echo "lifted object references $(wc -l < "$out/lifted_undefined.txt") undefined symbols (list: lifted_undefined.txt)"

echo "=== step 3: transfer sites compiled as jumps (production object, x86-64) ==="
python3 -I "$here/check_tail_transfers.py" "$out/lifted_production.o" | tee "$out/tail_check.log"

echo "=== step 4: run (differential, then the optional bench) ==="
set +e
timeout 1800s "$out/test-bink-lifted" "$elf" "$elf_off" "$@" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then echo "BINK_LIFTED_RUN_FAILED exit=$rc" >&2; exit 1; fi
grep -q '^overall=PASS$' "$out/run.log" || { echo "BINK_LIFTED_OVERALL_FAILED" >&2; exit 1; }
echo "overall=PASS"
