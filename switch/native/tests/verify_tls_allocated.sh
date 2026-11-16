#!/usr/bin/env bash
# Pure-C TLS-in-allocated-storage verifier + unchanged primary TLS regression; serial; ASan+UBSan at O0 and O2.
# Run inside a caller-bounded unit; PS3_VERIFY_CALLER_BOUNDED is accepted and no nested unit is ever started.
# No RLIMIT_AS (incompatible with ASan). PS3_TLS_ALLOCATED_VARIANTS overrides the list, e.g. "0".
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/tls-allocated.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
ppu="$root/../ps3recomp/runtime/ppu"
includes=( -I"$root/native/tests" -I"$ppu" -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/sync" -I"$root/../ps3recomp/runtime" )
if [[ -n "${PS3_TLS_ALLOCATED_VARIANTS:-}" ]]; then read -r -a variants <<<"$PS3_TLS_ALLOCATED_VARIANTS"; else variants=( 0 2 ); fi
for optimization in "${variants[@]}"; do
 flags=( "-O$optimization" -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined )
 cc="${CC:-cc}"
 for unit in ppu/tls_initialize.c memory/user_memory.c memory/atomic_domain.c; do
  "$cc" -std=c11 "${flags[@]}" "${includes[@]}" -c "$root/../ps3recomp/runtime/$unit" -o "$build/$(basename "$unit").o"
 done
 "$cc" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/native/tests/guest_space_host.c" -o "$build/host.o"
 "$cc" -std=c11 -pthread "${flags[@]}" "${includes[@]}" "$root/native/tests/test_tls_allocated.c" "$build"/{tls_initialize.c,user_memory.c,atomic_domain.c,host}.o -o "$build/tls-allocated-$optimization"
 "$cc" -std=c11 "${flags[@]}" "${includes[@]}" "$root/native/tests/test_tls_initialize.c" "$build/tls_initialize.c.o" -o "$build/tls-initialize-$optimization"
 echo "== variant O$optimization address,undefined =="
 export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
 timeout 240s "$build/tls-allocated-$optimization"
 timeout 240s "$build/tls-initialize-$optimization"
done
