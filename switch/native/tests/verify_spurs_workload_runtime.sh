#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/spurs-workload-runtime.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
read -r -a lanes <<<"${PS3_SPURS_RUNTIME_LANES:-0 2 1}"
for optimization in "${lanes[@]}"; do
    extra=()
    case "$optimization" in
        0|2) sanitizer=address,undefined ;;
        1) sanitizer=thread; extra=(-Wno-error=tsan -fno-pie -no-pie) ;;
        *) exit 2 ;;
    esac
    sources=(
        "${PS3_SPURS_RUNTIME_TEST_C:-$root/native/tests/test_spurs_instance.c}"
        "$root/native/tests/guest_space_host.c"
        "$root/../ps3recomp/runtime/memory/atomic_domain.c"
        "$root/../ps3recomp/runtime/memory/user_memory.c"
        "$root/native/adapters/guard_pthread.c"
        "$root/native/adapters/wait_port_pthread.c"
        "$root/native/adapters/service_thread_pthread.c"
        "$root/native/domain/lv2_objects.c"
        "$root/native/domain/ppu_threads.c"
        "$root/native/domain/spu_resource_policy.c"
        "$root/native/domain/spurs_kernel1_service.c"
        "${PS3_SPURS_RUNTIME_INSTANCE_C:-$root/native/domain/spurs_instance.c}"
        "$root/native/domain/spurs_workload_registration.c"
        "$root/native/domain/spurs_idle_request.c"
        "$root/native/domain/spurs_exception_handler.c"
        "$root/native/domain/spurs_ready_count.c" "$root/native/domain/spurs_workload_info.c"
    )
    "${CC:-cc}" -std=c11 -pthread "-O$optimization" -g -Wall -Wextra -Werror \
        -fno-omit-frame-pointer "-fsanitize=$sanitizer" "${extra[@]}" \
        -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/sync" \
        -I"$root/native/domain" -I"$root/native/tests" "${sources[@]}" -o "$build/runtime-$optimization"
    echo "SPURS_WORKLOAD_RUNTIME_LANE O$optimization $sanitizer"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
        TSAN_OPTIONS=halt_on_error=1 timeout 120s "$build/runtime-$optimization"
done
echo SPURS_WORKLOAD_RUNTIME_VERIFY_OK
