#!/usr/bin/env bash
# Host differential check of the lifted inFAMOUS job modules against the native SPU interpreter (Decision 104, 2026-11-13).
#
# usage: verify_jobmods_lifted.sh <fresh out dir> [test options...]
#   env: JOBMODS         comma separated module name prefixes to build (default: all 21)
#        JOBMODS_OPT     optimisation level of the lifted translation units (default 2)
#        JOBMODS_JOBS    parallel compiles (default 3)
#        JOBMODS_SKIP_REGEN=1  do not regenerate-and-compare the generated sources
#        JOBMODS_OBJDIR  an earlier out dir: a module object whose source+prelude+opt stamp matches is reused, not recompiled
#        JOBMODS_NATIVE_INFLATE=1  build the native inflate (native_inflate.c, -lz, -DPS3_SPU_NATIVE_INFLATE) and allow --inflate [CORPUS] [--inflate-synthetic N]
#        JOBMODS_ESCDIAG=1  build the runtime and the test with PS3_SPU_ESCAPE_DIAG (+ the exact escape histogram) and check the escape tables
#        JOBMODS_ELF     the user's own EBOOT.elf (read in place; its embedded SPU ELFs give the module data sections)
# Steps: (1) regenerate native/spu_lifted/jobs/generated from the lifter output and compare with the checked-in copy; (2) compile the
# runtime, the glue, the accounting unit, the modules (test build: PS3_LIFTED_TEST_RCHCNT_SPLAT, the interpreter's channel-count splat)
# and the test; (3) run it with the given options (default: --edge --fuzz 400).
# Heavy compiles run under systemd-run with an 8 GiB memory cap, at most JOBMODS_JOBS at once. No network.
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)"
out="${1:?usage: verify_jobmods_lifted.sh <fresh out dir> [test options...]}"; shift || true
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"; out="$(cd -- "$out" && pwd)"
here="$root/native/spu_lifted/jobs"
spu="$root/../infamous-recomp/spu"
elf="${JOBMODS_ELF:-$root/build/emulator-stage11-context-0150-r1-final/data/eden/sdmc/switch/infamous-native-probe/EBOOT.elf}"
opt="${JOBMODS_OPT:-2}"; jobs="${JOBMODS_JOBS:-3}"
[ "$#" -gt 0 ] || set -- --edge --fuzz 400
echo "scope: host behaviour test only; no game boot, no Switch build, no network"
echo "out: $out  lifted -O$opt  modules: ${JOBMODS:-all}  test options: $*"

if [ -z "${JOBMODS_SKIP_REGEN:-}" ]; then
    echo "=== step 1: regenerate the adapted lifted sources and compare with the checked-in copy ==="
    python3 -I "$here/gen_jobmods_lifted.py" --spu "$spu" --out "$out/regen" --elf "$elf" > "$out/regen.log"
    for f in "$here"/generated/*; do
        cmp "$out/regen/$(basename "$f")" "$f" || { echo "GENERATED_SOURCE_STALE $(basename "$f")" >&2; exit 1; }
    done
    echo "generated sources identical to a fresh regeneration"
fi

includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/time"
           -I"$root/../ps3recomp/include" -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain"
           -I"$root/native/spu_lifted" -I"$here" )
defs=( -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL ${JOBMODS_NATIVE_INFLATE:+-DPS3_SPU_NATIVE_INFLATE} )
common=( -std=gnu11 -pthread -g -DNDEBUG -fno-omit-frame-pointer -msse4.1 -Wno-misleading-indentation -ffp-contract=off -fno-strict-aliasing )
cap=( systemd-run --user --scope --quiet -p MemoryMax=8G -p MemorySwapMax=0 )

# ---- the modules to build
names=()
for f in "$here"/generated/*_module.c; do
    n="$(basename "$f" _module.c)"
    if [ -n "${JOBMODS:-}" ]; then
        keep=0; IFS=',' read -ra want <<< "$JOBMODS"
        for w in "${want[@]}"; do case "$n" in *"$w"*) keep=1;; esac; done
        [ "$keep" = 1 ] || continue
    fi
    names+=( "$n" )
done
echo "modules: ${#names[@]}"
modflags=()
for n in "${names[@]}"; do modflags+=( "-DPS3_JOBMOD_$n" ); done

compile_module() {
    local n="$1"
    local stamp; stamp="$(sha256sum "$here/generated/${n}_rc.c" "$here/jobmod_lifted_prelude.h" | cut -d' ' -f1 | tr -d '\n') O$opt T${JOBMODS_TRACE:-0} N${JOBMODS_NATIVE_INFLATE:-0}"
    if [ -n "${JOBMODS_OBJDIR:-}" ] && [ -f "$JOBMODS_OBJDIR/${n}_rc.o" ] && [ "$(cat "$JOBMODS_OBJDIR/${n}_rc.stamp" 2>/dev/null)" = "$stamp" ]; then
        cp "$JOBMODS_OBJDIR/${n}_rc.o" "$out/${n}_rc.o"; cp "$JOBMODS_OBJDIR/${n}_rc.stamp" "$out/${n}_rc.stamp"
        cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -c "$here/generated/${n}_module.c" -o "$out/${n}_module.o"
        return 0
    fi
    ( cd "$out" && "${cap[@]}" cc "${common[@]}" "-O$opt" -frounding-math "${defs[@]}" "${includes[@]}" -Wall -Wextra \
        -Wno-unused-variable -Wno-unused-but-set-variable -DPS3_LIFTED_TEST_RCHCNT_SPLAT ${JOBMODS_TRACE:+-DPS3_JOBMOD_TRACE} \
        -c "$here/generated/${n}_rc.c" -o "$out/${n}_rc.o" ) > "$out/compile_${n}.log" 2>&1 \
        || { echo "LIFTED_COMPILE_FAILED $n"; tail -30 "$out/compile_${n}.log"; return 1; }
    printf '%s' "$stamp" > "$out/${n}_rc.stamp"
    cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" -c "$here/generated/${n}_module.c" -o "$out/${n}_module.o"
}
t0=$(date +%s)
echo "=== step 2: compile modules ($jobs parallel), runtime, glue, test ==="
pids=()
run_pool() {
    local running=0
    for n in "${names[@]}"; do
        compile_module "$n" &
        pids+=( $! ); running=$((running + 1))
        if [ "$running" -ge "$jobs" ]; then wait -n || { echo "a module compile failed" >&2; exit 1; }; running=$((running - 1)); fi
    done
    wait || { echo "a module compile failed" >&2; exit 1; }
}
run_pool &
pool=$!

runtime_units=( "$root/native/adapters/spu_executor.c" "$root/native/adapters/spu_interp_native.c"
                "$root/native/adapters/fp_control_x86.c" "$root/native/adapters/spu_memory.c"
                "$root/native/adapters/guard_pthread.c" "$root/native/adapters/spu_lifted_boundary.c"
                "$root/native/adapters/spu_decrementer.c" "$root/../ps3recomp/runtime/time/timebase.c"
                "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
                "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c"
                "$root/native/spu_lifted/spu_lifted_hybrid.c" "$root/native/adapters/spu_image_account.c" ${JOBMODS_NATIVE_INFLATE:+"$here/native_inflate.c"} )
objects=()
for source in "${runtime_units[@]}"; do
    object="$out/$(basename "${source%.c}").o"
    cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" ${JOBMODS_ESCHIST:+-DPS3_LIFTED_ESCAPE_HIST} ${JOBMODS_ESCDIAG:+-DPS3_LIFTED_ESCAPE_HIST -DPS3_SPU_ESCAPE_DIAG} -c "$source" -o "$object"
    objects+=( "$object" )
done
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" "${modflags[@]}" -c "$here/generated/jobmods_table.c" -o "$out/jobmods_table.o"
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" ${JOBMODS_ESCHIST:+-DPS3_LIFTED_ESCAPE_HIST} ${JOBMODS_ESCDIAG:+-DPS3_LIFTED_ESCAPE_HIST -DPS3_SPU_ESCAPE_DIAG} -c "$here/test_jobmods_lifted.c" -o "$out/test_jobmods_lifted.o"
wait $pool || { echo "MODULE_COMPILE_FAILED" >&2; exit 1; }
t1=$(date +%s)
echo "modules compiled at -O$opt in $((t1 - t0)) s"
modobjs=()
for n in "${names[@]}"; do modobjs+=( "$out/${n}_rc.o" "$out/${n}_module.o" ); done
cc "${common[@]}" -O2 "${defs[@]}" "${includes[@]}" "$out/test_jobmods_lifted.o" "$out/jobmods_table.o" "${modobjs[@]}" "${objects[@]}" -lm ${JOBMODS_NATIVE_INFLATE:+-lz} -o "$out/test-jobmods-lifted"
ls -l "$out"/*_rc.o | awk '{print $5, $9}' > "$out/object_sizes.txt"
nm "$out/test-jobmods-lifted" | grep -E ' U (spu_register_function|spu_rc_add_pairs|g_spu_fast_tail|g_spu_trampoline_fn|spu_tlc|spu_lookup)$' && { echo "UNEXPECTED_DESKTOP_SYMBOL_REFERENCES" >&2; exit 1; } || true

echo "=== step 3: run ==="
set +e
timeout 7200s "$out/test-jobmods-lifted" "$spu" --extra "$here/extra" "$@" 2>&1 | tee "$out/run.log"
rc=${PIPESTATUS[0]}
set -e
if [ "$rc" -ne 0 ]; then echo "JOBMODS_LIFTED_RUN_FAILED exit=$rc" >&2; exit 1; fi
grep -q '^overall=PASS$' "$out/run.log" || { echo "JOBMODS_LIFTED_OVERALL_FAILED" >&2; exit 1; }
echo "overall=PASS"
