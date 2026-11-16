#!/usr/bin/env bash
# Pure-C PPU mapped guest-storage verifier (host test, NOT game execution); serial; ASan+UBSan at O0/O2 and TSan at O1.
# Caller-bounded by PROOF, not by claim: before any temp dir, compiler or test this script reads the ACTUAL cgroup v2 limits of
# its own process and refuses (exit 3, compiler never invoked) unless cpu<=1 CPU, memory.max numeric nonzero <=2147483648,
# memory.swap.max 0, pids.max numeric nonzero <=128 and NoNewPrivs=1. PS3_VERIFY_CALLER_BOUNDED is NOT consulted (an environment
# variable proves nothing). No systemd unit is ever started here; run it inside an existing bounded unit.
# Arguments are passed to the test binary (a case name runs one case; default: every case).
# PS3_PPU_GUEST_STORAGE_VARIANTS overrides the variant list, e.g. "0 address,undefined".
set -euo pipefail
ulimit -c 0
refuse() { echo "REFUSED (exit 3): $* -- not running under verified caller caps; no compiler or test was run" >&2; exit 3; }
uint() { [[ "$1" =~ ^[0-9]{1,18}$ ]]; }
verify_caller_caps() {
 local path mnt_root mnt rel cg cpu quota period mem swp pids nnp
 path="$(sed -n 's/^0:://p' /proc/self/cgroup 2>/dev/null | head -n1)"
 [[ "$path" == /* && "$path" != *..* ]] || refuse "no cgroup v2 entry in /proc/self/cgroup"
 read -r mnt_root mnt < <(awk '{for(i=7;i<=NF;i++) if($i=="-"){ if($(i+1)=="cgroup2"){print $4, $5; exit}; break }}' /proc/self/mountinfo 2>/dev/null) || true
 [[ -n "${mnt:-}" && "$mnt" == /* && "$mnt" != *\\* ]] || refuse "no cgroup2 mount in /proc/self/mountinfo"
 rel="$path"
 if [[ "$mnt_root" != "/" ]]; then
  [[ "$path" == "$mnt_root" || "$path" == "$mnt_root"/* ]] || refuse "cgroup path $path is outside the cgroup2 mount root $mnt_root"
  rel="${path#"$mnt_root"}"
 fi
 cg="$mnt$rel"; [[ -d "$cg" ]] || refuse "cgroup directory $cg is not readable"
 cpu="$(cat "$cg/cpu.max" 2>/dev/null)" || refuse "cpu.max unreadable in $cg"
 mem="$(cat "$cg/memory.max" 2>/dev/null)" || refuse "memory.max unreadable in $cg"
 swp="$(cat "$cg/memory.swap.max" 2>/dev/null)" || refuse "memory.swap.max unreadable in $cg"
 pids="$(cat "$cg/pids.max" 2>/dev/null)" || refuse "pids.max unreadable in $cg"
 nnp="$(awk '/^NoNewPrivs:/{print $2}' /proc/self/status 2>/dev/null)"
 read -r quota period <<<"$cpu"
 echo "caps: cgroup=$cg cpu.max='$cpu' memory.max=$mem memory.swap.max=$swp pids.max=$pids NoNewPrivs=$nnp"
 uint "${quota:-}" && uint "${period:-}" && (( 10#$quota > 0 && 10#$period > 0 && 10#$quota <= 10#$period )) || refuse "cpu.max '$cpu' is not a numeric quota <= 1 CPU (max/unlimited rejected)"
 uint "$mem" && (( 10#$mem > 0 && 10#$mem <= 2147483648 )) || refuse "memory.max '$mem' is not numeric in 1..2147483648"
 uint "$swp" && (( 10#$swp == 0 )) || refuse "memory.swap.max '$swp' is not 0"
 uint "$pids" && (( 10#$pids > 0 && 10#$pids <= 128 )) || refuse "pids.max '$pids' is not numeric in 1..128"
 [[ "$nnp" == 1 ]] || refuse "NoNewPrivs '$nnp' is not 1"
}
verify_caller_caps
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$(mktemp -d "${TMPDIR:-/tmp}/ppu-guest-storage.XXXXXXXX")"
trap 'rm -rf -- "$build"' EXIT
includes=( -I"$root/native/adapters" -I"$root/native/tests" -I"$root/native/domain"
 -I"$root/../ps3recomp/runtime/memory" -I"$root/../ps3recomp/runtime/sync" -I"$root/../ps3recomp/runtime" )
if [[ -n "${PS3_PPU_GUEST_STORAGE_VARIANTS:-}" ]]; then variants=( "$PS3_PPU_GUEST_STORAGE_VARIANTS" )
else variants=( "0 address,undefined" "2 address,undefined" "1 thread" ); fi
for variant in "${variants[@]}"; do
 read -r optimization sanitizer <<<"$variant"
 flags=( "-O$optimization" -g -DNDEBUG -Wall -Wextra -Werror -fno-omit-frame-pointer "-fsanitize=$sanitizer" )
 objs=()
 for source in memory/atomic_domain.c memory/user_memory.c; do
  o="$build/$(basename "$source").o"; "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/../ps3recomp/runtime/$source" -o "$o"; objs+=( "$o" )
 done
 for source in tests/guest_space_host.c adapters/guard_pthread.c adapters/wait_port_pthread.c adapters/service_thread_pthread.c domain/ppu_threads.c; do
  o="$build/$(basename "$source").o"; "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" -c "$root/native/$source" -o "$o"; objs+=( "$o" )
 done
 "${CC:-cc}" -std=c11 -pthread "${flags[@]}" "${includes[@]}" "$root/native/tests/test_ppu_guest_storage.c" "${objs[@]}" -o "$build/ppu-guest-storage-$optimization"
 echo "== variant O$optimization $sanitizer =="
 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 TSAN_OPTIONS=halt_on_error=1 \
  timeout 240s "$build/ppu-guest-storage-$optimization" "$@"
done
