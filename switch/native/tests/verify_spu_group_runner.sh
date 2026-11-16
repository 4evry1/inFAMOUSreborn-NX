#!/usr/bin/env bash
# Decision 33 / C1b: the SPU group runner on the C1a interpreter.
#
# usage: verify_spu_group_runner.sh <fresh out dir>
#
# Real host threads, a real atomic domain and a real lv2 event queue -- the same objects the composition wires.
# Every case is bounded: the runner's waits are sliced and cancellable, and the test's own waits have attempt limits,
# so a broken runner fails the case instead of hanging the verifier.
#
# Variants: O0 and O2 under ASan+UBSan, O1 under TSan (the runner is genuinely multi-threaded).
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?usage: verify_spu_group_runner.sh <fresh out dir>}"
[ ! -e "$out" ] || { echo "REFUSED: $out already exists -- give a fresh directory" >&2; exit 2; }
mkdir -p "$out"
echo "scope: host behaviour test only; no game boot, no Switch build (root owns that), no network"
echo "out: $out"

includes=( -I"$root/../ps3recomp/runtime/spu" -I"$root/../ps3recomp/runtime/memory"
           -I"$root/../ps3recomp/runtime/time" -I"$root/../ps3recomp/runtime/sync"
           -I"$root/../ps3recomp/include" -I"$root/native/adapters" -I"$root/native/tests"
           -I"$root/native/domain" )
units_c=( "$root/native/adapters/spu_group_runner.c" "$root/native/adapters/spu_executor.c"
          "$root/native/adapters/spu_interp_native.c" "$root/native/adapters/fp_control_x86.c"
          "$root/native/adapters/spu_memory.c" "$root/native/adapters/guard_pthread.c"
          "$root/native/adapters/wait_port_pthread.c" "$root/native/adapters/service_thread_pthread.c"
          "$root/native/adapters/spu_lifted_boundary.c" "$root/native/adapters/spu_decrementer.c"
          "$root/native/tests/guest_space_host.c" "$root/../ps3recomp/runtime/memory/atomic_domain.c"
          "$root/../ps3recomp/runtime/spu/spu_channels.c" "$root/native/domain/lv2_spu.c"
          "$root/native/domain/lv2_objects.c" "$root/../ps3recomp/runtime/time/timebase.c" )
units_c=( "${units_c[@]}" "$root/native/tests/test_spu_group_runner.c" )

overall=0
run_variant() {
    local optimization="$1" sanitizer="$2" label="$3"
    local dir="$out/$label"; mkdir -p "$dir"
    # -Wno-misleading-indentation: pre-existing spu_helpers.h (lines 570/572), not C1a/C1b code.
    local flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -Wno-misleading-indentation
                  -fno-omit-frame-pointer "-fsanitize=$sanitizer" )
    if [[ "$sanitizer" == thread ]]; then flags+=( -Wno-error=tsan ); fi
    local objects=()
    local source
    for source in "${units_c[@]}"; do
        local object="$dir/$(basename "${source%.c}").o"
        "${CC:-cc}" -std=gnu11 -pthread "${flags[@]}" -msse4.1 -DPS3_NATIVE_SPU_EXECUTOR -DPS3_SPU_EXTERNAL_FP_CONTROL \
            "${includes[@]}" -c "$source" -o "$object"
        objects+=( "$object" )
    done
    "${CC:-cc}" -std=gnu11 -pthread "${flags[@]}" "${objects[@]}" -lm -o "$dir/spu-group-runner"
    echo "== variant O$optimization $sanitizer =="
    set +e
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    TSAN_OPTIONS=halt_on_error=1 timeout 300s "$dir/spu-group-runner" 2>&1 | tee "$dir/run.log"
    local rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ]; then echo "SPU_GROUP_RUNNER_FAILED variant=O$optimization/$sanitizer exit=$rc" >&2; return 1; fi
    grep -q '^overall=PASS$' "$dir/run.log" || { echo "SPU_GROUP_RUNNER_OVERALL_FAILED" >&2; return 1; }
    echo "variant=O$optimization/$sanitizer PASS"
}

IFS=";" read -ra variants <<<"${PS3_SPU_GROUP_RUNNER_VARIANTS:-0 address,undefined;2 address,undefined;1 thread}"
for variant in "${variants[@]}"; do
    read -r optimization sanitizer <<<"$variant"
    if ! run_variant "$optimization" "$sanitizer" "v$optimization"; then overall=1; fi
done

if [ "$overall" -eq 0 ]; then
    echo "--- summary (O2/ASan+UBSan) ---"
    grep -E '^(GROUP_|QUEUE_)' "$out/v2/run.log" || true
fi
echo "overall=$([ "$overall" -eq 0 ] && echo PASS || echo FAIL)"
exit "$overall"
