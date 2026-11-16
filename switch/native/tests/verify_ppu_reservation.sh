#!/usr/bin/env bash
# Pure-C PPU service reserve/start/abandon verifier; serial; ASan+UBSan at O0/O2 and TSan at O1.
# Run inside a caller-bounded unit; PS3_VERIFY_CALLER_BOUNDED is accepted and no nested unit is ever started.
# Arguments are passed to the test binary (a case name runs one new case; default: original checks + every case).
# PS3_PPU_RESERVATION_VARIANTS overrides the variant list, e.g. "0 address,undefined".
set -euo pipefail
ulimit -c 0
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/ppu-reservation.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
includes=( -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain"
 -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/sync" -I"$root/../ps3recomp/runtime" )
if [[ -n "${PS3_PPU_RESERVATION_VARIANTS:-}" ]]; then variants=( "$PS3_PPU_RESERVATION_VARIANTS" )
else variants=( "0 address,undefined" "2 address,undefined" "1 thread" ); fi
for variant in "${variants[@]}"; do
 read -r optimization sanitizer <<<"$variant"
 flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer "-fsanitize=$sanitizer" )
 objs=()
 for source in memory/atomic_domain.c memory/user_memory.c; do
  o="$build/$(basename "$source").o"; "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/../ps3recomp/runtime/$source" -o "$o"; objs+=( "$o" )
 done
 for source in tests/guest_space_host.c adapters/guard_pthread.c adapters/wait_port_pthread.c adapters/service_thread_pthread.c \
   domain/lv2_objects.c domain/ppu_threads.c; do
  o="$build/$(basename "$source").o"; "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/native/$source" -o "$o"; objs+=( "$o" )
 done
 "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" "$root/native/tests/test_ppu_reservation.c" "${objs[@]}" -o "$build/ppu-reservation-$optimization"
 echo "== variant O$optimization $sanitizer =="
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 \
  timeout 240s "$build/ppu-reservation-$optimization" "$@"
done
