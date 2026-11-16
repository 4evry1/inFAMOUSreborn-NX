#!/usr/bin/env bash
# Host verification for the platform-free file-system service (native/domain/fs_service.*)
# and its POSIX adapter (native/adapters/fs_storage_posix.*): the 52-byte CellFsStat record,
# the compact dirent's 1/2 type tag, every path-confinement rule, a 3-level tree walk with
# nread == 0 exactly at the end, the recursive delete confined to the root with a sibling
# sentinel outside it, EMFILE at capacity and double close.
#
# CPU semantics and a private scratch tree only: no SD card, no GPU, no game, no network.
# The scratch tree is `<out_dir>/fs-service-world-XXXXXX` and is removed by the test itself.
#
# Caller-bounded by PROOF: before any temp dir, compiler or test this script reads the ACTUAL
# cgroup v2 limits of its own process and refuses (exit 3, compiler never invoked) unless
# cpu <= 1 CPU, memory.max numeric nonzero <= 2147483648, memory.swap.max 0, pids.max numeric
# nonzero <= 128 and NoNewPrivs=1. No systemd unit is started here.
#
# Usage: verify_fs_service.sh <fresh out-dir>
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

[ $# -eq 1 ] && [ -n "$1" ] || { echo "usage: verify_fs_service.sh <fresh out-dir>" >&2; exit 2; }
OUT=$1
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
mkdir -p "$OUT"
[[ -z "$(ls -A "$OUT" 2>/dev/null)" ]] || { echo "REFUSED (exit 4): $OUT is not a fresh out-dir" >&2; exit 4; }

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DOMAIN="$ROOT/native/domain"
ADAPTERS="$ROOT/native/adapters"
TEST="$ROOT/native/tests/test_fs_service.c"
SERVICE_H="$DOMAIN/fs_service.h"
SERVICE_C="$DOMAIN/fs_service.c"
STORAGE_H="$ADAPTERS/fs_storage_posix.h"
STORAGE_C="$ADAPTERS/fs_storage_posix.c"

for f in "$TEST" "$SERVICE_H" "$SERVICE_C" "$STORAGE_H" "$STORAGE_C"; do
  [[ -f "$f" ]] || { echo "MISSING (exit 5): $f" >&2; exit 5; }
done

CC=${CC:-gcc}
LIM="timeout 600 nice -n 10 taskset -c 0"
INC="-I$DOMAIN -I$ADAPTERS"
WARN="-std=c11 -Wall -Wextra -Werror"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

: >"$OUT/summary.txt"
sha256sum "$SERVICE_H" "$SERVICE_C" "$STORAGE_H" "$STORAGE_C" "$TEST" >"$OUT/inputs.sha256"
echo "cc=$("$CC" --version | head -n1)" >"$OUT/compiler.txt"
cat "$OUT/compiler.txt" "$OUT/inputs.sha256"

fail=0
step() { local name=$1; shift; { echo "\$ $*"; "$@"; } >"$OUT/$name.log" 2>&1; local rc=$?
  echo "$name exit=$rc" | tee -a "$OUT/summary.txt"; [ $rc -eq 0 ] || fail=1; return $rc; }

for O in O0 O2; do
  step fs-service-$O-build $LIM "$CC" $WARN -"$O" -g $SAN $INC \
    "$TEST" "$SERVICE_C" "$STORAGE_C" -o "$OUT/fs-service-$O" \
    && step fs-service-$O-run $LIM "$OUT/fs-service-$O" "$OUT"
  if grep -q '^FS_SERVICE checks=[1-9][0-9]* failures=0$' "$OUT/fs-service-$O-run.log" 2>/dev/null; then
    echo "FS_SERVICE PASS variant=$O sanitizer=address,undefined standard=c11 warnings=error"
  else
    echo "FS_SERVICE FAIL variant=$O"; fail=1
  fi

  # Symbol closure of the domain object: no OS calls at all may remain undefined.
  step closure-domain-$O $LIM bash -c "
    set -eo pipefail
    '$CC' $WARN -$O -fno-stack-protector $INC -c '$SERVICE_C' -o '$OUT/closure-fs_service-$O.o'
    nm -u '$OUT/closure-fs_service-$O.o' > '$OUT/closure-fs_service-$O.undef'
    cat '$OUT/closure-fs_service-$O.undef'
    awk '{print \$2}' '$OUT/closure-fs_service-$O.undef' | grep -vxE \
      'memchr|memcmp|memcpy|__memcpy_chk|memset|__memset_chk|strlen|strncmp|__stack_chk_fail|_GLOBAL_OFFSET_TABLE_' && exit 1 || true
    exit 0"

  # Symbol closure of the adapter object: only the POSIX calls it is allowed to make.
  step closure-adapter-$O $LIM bash -c "
    set -eo pipefail
    '$CC' $WARN -$O -fno-stack-protector $INC -c '$STORAGE_C' -o '$OUT/closure-fs_storage_posix-$O.o'
    nm -u '$OUT/closure-fs_storage_posix-$O.o' > '$OUT/closure-fs_storage_posix-$O.undef'
    cat '$OUT/closure-fs_storage_posix-$O.undef'
    awk '{print \$2}' '$OUT/closure-fs_storage_posix-$O.undef' | grep -vxE \
      'calloc|free|closedir|opendir|readdir|rewinddir|rmdir|unlink|lstat|snprintf|__snprintf_chk|__errno_location|open|close|read|write|lseek|fstat|mkdir|fsync|rename|memchr|memcpy|__memcpy_chk|memset|__memset_chk|strchr|strcmp|strlen|strncmp|__stack_chk_fail|_GLOBAL_OFFSET_TABLE_' \
      && exit 1 || true
    exit 0"
done

# The scratch world must be gone: the test removes it, and nothing outside it was touched.
leftover=$(find "$OUT" -maxdepth 1 -name 'fs-service-world-*' -print 2>/dev/null | head -n1 || true)
if [[ -n "$leftover" ]]; then
  echo "FS_SERVICE FAIL leftover-scratch=$leftover"; fail=1
else
  echo "scratch: removed by the test, nothing left in $OUT"
fi

echo "overall=$([ $fail -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $fail
