#!/usr/bin/env bash
# Host verification for the POSIX game-data storage adapter
# (native/adapters/gamedata_storage_posix.c) over a REAL temporary directory tree:
# free space, directory create/stat, atomic write/read, size rounding, path-escape refusal and an
# end-to-end gamedata_build_statget + gamedata_apply_result + gamedata_param_sfo_decode round trip.
# CPU semantics and the host filesystem only: no SD card, no GPU, no game, no network.
# Serial, one compiler job, core 0. Must run inside a capped transient systemd unit; the
# script asserts the effective cgroup limits and refuses otherwise: memory.max=2147483648,
# memory.swap.max=0, pids.max=128, and cpu.max "QUOTA PERIOD" numeric with 0 < QUOTA <=
# PERIOD (at most 1 CPU; the launcher uses exactly "100000 100000").
# Usage: verify_gamedata_storage_posix.sh <fresh build dir>
set -u -o pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-}
[ -n "$OUT" ] || { echo "usage: verify_gamedata_storage_posix.sh <fresh build dir>" >&2; exit 2; }
mkdir -p "$OUT"
if [ -e "$OUT/derived" ]; then echo "REFUSED: $OUT is not fresh" >&2; exit 3; fi

DOMAIN="$ROOT/native/domain"
ADAPTERS="$ROOT/native/adapters"
TEST="$ROOT/native/tests/test_gamedata_storage_posix.c"
STORAGE_C="$ADAPTERS/gamedata_storage_posix.c"
SERVICE_C="$DOMAIN/gamedata_service.c"

# cpu.max validation: exactly two decimal fields, 0 < quota <= period (so quota/period <= 1 CPU). "max" fails.
cpu_quota_ok() {
  local q p extra
  read -r q p extra <<<"$1"
  [ -z "$extra" ] || return 1
  [[ $q =~ ^[0-9]{1,15}$ && $p =~ ^[0-9]{1,15}$ ]] || return 1
  [ $((10#$q)) -gt 0 ] && [ $((10#$p)) -gt 0 ] && [ $((10#$q)) -le $((10#$p)) ]
}

LIM="timeout 600 nice -n 10 taskset -c 0"
INC="-I$DOMAIN -I$ADAPTERS"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

cg=$(sed -n 's/^0:://p' /proc/self/cgroup); cgd=/sys/fs/cgroup/$cg
{ echo "cgroup=$cg"; for n in memory.max memory.swap.max cpu.max pids.max; do echo "$n=$(cat "$cgd/$n")"; done; } \
  >"$OUT/effective-resources.txt"
{ [ "$(cat "$cgd/memory.max")" = 2147483648 ] && [ "$(cat "$cgd/memory.swap.max")" = 0 ] \
  && [ "$(cat "$cgd/pids.max")" = 128 ] && cpu_quota_ok "$(cat "$cgd/cpu.max")"; } \
  || { echo "cgroup limits not as required" >&2; exit 90; }

fail=0
step() { local name=$1; shift; { echo "\$ $*"; "$@"; } >"$OUT/$name.log" 2>&1; local rc=$?
  echo "$name exit=$rc" | tee -a "$OUT/summary.txt"; [ $rc -eq 0 ] || fail=1; return $rc; }

: >"$OUT/summary.txt"
sha256sum "$STORAGE_C" "$ADAPTERS/gamedata_storage_posix.h" "$SERVICE_C" "$DOMAIN/gamedata_service.h" "$TEST" >"$OUT/inputs.sha256"

for O in O0 O2; do
  step gamedata-storage-$O-build $LIM gcc -std=c11 -$O -g -Wall -Wextra -Werror $SAN $INC \
    "$TEST" "$STORAGE_C" "$SERVICE_C" -o "$OUT/gamedata-storage-$O" \
    && step gamedata-storage-$O-run $LIM "$OUT/gamedata-storage-$O" "$OUT"
  grep -q 'GAMEDATA_STORAGE_POSIX checks=[1-9][0-9]* failures=0' "$OUT/gamedata-storage-$O-run.log" 2>/dev/null \
    && echo "GAMEDATA_STORAGE_POSIX PASS variant=O$O sanitizer=address,undefined" || { echo "GAMEDATA_STORAGE_POSIX FAIL variant=O$O"; fail=1; }
  # Symbol closure of the adapter object (no sanitizer instrumentation): only the POSIX/libc calls
  # it is allowed to make may remain undefined.
  step closure-$O $LIM bash -c "
    set -eo pipefail
    gcc -std=c11 -$O -fno-stack-protector -Wall -Wextra -Werror $INC -c \"$STORAGE_C\" -o \"$OUT/closure-gamedata_storage_posix-$O.o\"
    nm -u \"$OUT/closure-gamedata_storage_posix-$O.o\" > \"$OUT/closure-gamedata_storage_posix-$O.undef\"
    cat \"$OUT/closure-gamedata_storage_posix-$O.undef\"
    awk '{print \$2}' \"$OUT/closure-gamedata_storage_posix-$O.undef\" | grep -vxE \
      'memset|memcpy|memcmp|strlen|strcmp|snprintf|__snprintf_chk|mkdir|stat|__xstat|statvfs|opendir|readdir|closedir|fopen|fwrite|fread|ferror|fflush|fclose|rename|unlink|lstat|__lxstat|__errno_location|_GLOBAL_OFFSET_TABLE_' \
      && exit 1 || true
    exit 0"
done

echo "overall=$([ $fail -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $fail
