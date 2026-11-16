#!/usr/bin/env bash
# Decision 33 / B2b: the SPU image and SPU thread group domain (native/domain/lv2_spu.h), serial.
#
# usage: verify_lv2_spu.sh [OUT_DIR]
#
# The domain is CPU-free and guest-memory-free, so this is a pure host test: real guard, a
# caller-provided local-store pool, and a hand-built SPU ELF. O0 and O2 under ASan+UBSan, O1 under
# TSan.
#
# PS3_LV2_SPU_VARIANTS='0 address,undefined;2 address,undefined;1 thread' overrides the list.
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-$root/build/b2b-spu-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$out"
echo "out: $out"

includes=( -I"$root/native/domain" -I"$root/native/adapters" -I"$root/native/tests"
    -I"$root/../ps3recomp/runtime/sync" -I"$root/../ps3recomp/runtime/memory"
    -I"$root/../ps3recomp/include" )

overall=0
IFS=";" read -ra variants <<<"${PS3_LV2_SPU_VARIANTS:-0 address,undefined;2 address,undefined;1 thread}"
for variant in "${variants[@]}"; do
    read -r optimization sanitizer <<<"$variant"
    flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer "-fsanitize=$sanitizer" )
    if [[ "$sanitizer" == thread ]]; then flags+=( -Wno-error=tsan ); fi
    dir="$out/v$optimization"; mkdir -p "$dir"; objs=()
    for source in domain/lv2_spu.c adapters/guard_pthread.c; do
        o="$dir/$(basename "$source").o"
        "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/native/$source" -o "$o"
        objs+=( "$o" )
    done
    "${CXX:-c++}" -std=c++17 -pthread "${flags[@]}" "${includes[@]}" \
        "$root/native/tests/test_lv2_spu.cpp" "${objs[@]}" -o "$dir/lv2-spu"
    echo "== variant O$optimization $sanitizer =="
    set +e
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    TSAN_OPTIONS=halt_on_error=1 timeout 300s "$dir/lv2-spu"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        echo "LV2_SPU_RUN_FAILED variant=O$optimization/$sanitizer exit=$rc" >&2
        overall=1
    fi
done

echo "overall=$([ "$overall" -eq 0 ] && echo PASS || echo FAIL)"
exit "$overall"
