#!/usr/bin/env bash
# Host verification for the commit-on-demand guest window decorator
# (native/adapters/demand_window.c) over the heap backing.
# CPU semantics only: no GPU, no SD, no game, no network.
# Serial, one compiler job, core 0. Must run inside a capped transient systemd unit; the
# script asserts the effective cgroup limits and refuses otherwise: memory.max=2147483648,
# memory.swap.max=0, pids.max=128, and cpu.max "QUOTA PERIOD" numeric with 0 < QUOTA <=
# PERIOD (at most 1 CPU; the launcher uses exactly "100000 100000").
# Usage: verify_demand_window.sh [OUT_DIR]     (default build/verify-demand-window-r1)
set -u -o pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/verify-demand-window-r1}
mkdir -p "$OUT"

ADAPTERS="$ROOT/native/adapters"
RUNTIME_MEMORY="$ROOT/../ps3recomp/runtime/memory"
RUNTIME_SYNC="$ROOT/../ps3recomp/runtime/sync"
TEST="$ROOT/native/tests/test_demand_window.c"
NEW_C="$ADAPTERS/demand_window.c"
BASE_C="$ADAPTERS/heap_guest_space.c"

# cpu.max validation: exactly two decimal fields, 0 < quota <= period (so quota/period <= 1 CPU). "max" fails.
cpu_quota_ok() {
  local q p extra
  read -r q p extra <<<"$1"
  [ -z "$extra" ] || return 1
  [[ $q =~ ^[0-9]{1,15}$ && $p =~ ^[0-9]{1,15}$ ]] || return 1
  [ $((10#$q)) -gt 0 ] && [ $((10#$p)) -gt 0 ] && [ $((10#$q)) -le $((10#$p)) ]
}

LIM="timeout 600 nice -n 10 taskset -c 0"
INC="-I$ADAPTERS -I$RUNTIME_MEMORY -I$RUNTIME_SYNC"
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
sha256sum "$NEW_C" "$ADAPTERS/demand_window.h" \
  "$BASE_C" "$ADAPTERS/heap_guest_space.h" \
  "$TEST" >"$OUT/inputs.sha256"

for O in O0 O2; do
  step demand-window-$O-build $LIM gcc -std=c11 -$O -g -Wall -Wextra -Werror $SAN $INC \
    "$TEST" $NEW_C $BASE_C -o "$OUT/demand-window-$O" \
    && step demand-window-$O-run $LIM "$OUT/demand-window-$O"
  # Symbol closure of the new adapter object (no sanitizer instrumentation): only
  # memset/memcpy may remain undefined.
  step closure-$O $LIM bash -c "
    set -eo pipefail
    for f in demand_window; do
      gcc -std=c11 -$O -fno-stack-protector -Wall -Wextra -Werror $INC -c \"$ADAPTERS/\$f.c\" -o \"$OUT/closure-\$f-$O.o\"
      nm -u \"$OUT/closure-\$f-$O.o\" > \"$OUT/closure-\$f-$O.undef\"
      cat \"$OUT/closure-\$f-$O.undef\"
      awk '{print \$2}' \"$OUT/closure-\$f-$O.undef\" | grep -vxE 'memset|memcpy|_GLOBAL_OFFSET_TABLE_' && exit 1 || true
    done
    exit 0"
done

echo "overall=$([ $fail -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $fail
