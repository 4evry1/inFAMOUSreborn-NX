#!/usr/bin/env bash
# Host verification for native/domain/gcm_address_space (CPU only; no GPU/SD/game).
# Serial, one compiler job, core 0. Must run inside a capped transient systemd unit; the script asserts the
# effective cgroup limits and refuses otherwise: memory.max=2147483648, memory.swap.max=0, pids.max=128, and
# cpu.max "QUOTA PERIOD" numeric with 0 < QUOTA <= PERIOD (at most 1 CPU; the launcher uses exactly "100000 100000").
# Usage: verify_gcm_address_space.sh OUT_DIR
# Override (mutation runs): GCM_AS_C=path.  Verifier self-test (bounded; checks the verifier's own failure
# detection, not the core): GCM_AS_SELFTEST=1 verify_gcm_address_space.sh OUT_DIR
# Other override: GCM_AS_NM=command used for `nm -u` (self-test uses a failing command).
set -u -o pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?OUT_DIR}; mkdir -p "$OUT"
SRC=${GCM_AS_C:-$ROOT/native/domain/gcm_address_space.c}
NM=${GCM_AS_NM:-nm}

# cpu.max validation: exactly two decimal fields, 0 < quota <= period (so quota/period <= 1 CPU). "max" fails.
cpu_quota_ok() {
  local q p extra
  read -r q p extra <<<"$1"
  [ -z "$extra" ] || return 1
  [[ $q =~ ^[0-9]{1,15}$ && $p =~ ^[0-9]{1,15}$ ]] || return 1
  [ $((10#$q)) -gt 0 ] && [ $((10#$p)) -gt 0 ] && [ $((10#$q)) -le $((10#$p)) ]
}

LIM="timeout 600 nice -n 10 taskset -c 0"
INC="-I$ROOT/native/domain"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
fail=0
: >"$OUT/summary.txt"
step() { local name=$1; shift; { echo "\$ $*"; "$@"; } >"$OUT/$name.log" 2>&1; local rc=$?
  echo "$name exit=$rc" | tee -a "$OUT/summary.txt"; [ $rc -eq 0 ] || fail=1; return $rc; }

# Core-only closure: compile the core alone and require every undefined symbol to be memset/memcpy. No pipes feed the
# decision: gcc, nm, awk each must succeed (set -e + pipefail), and grep status 1 (= no unexpected symbol) is required
# explicitly, so a failing nm/awk/grep can never look like an empty (clean) closure.
closure_step() { # name opt nm_cmd
  step "$1" $LIM bash -c '
    set -eo pipefail
    gcc -std=gnu11 "-$1" -fno-stack-protector -Wall -Wextra -Werror "$5" -c "$2" -o "$3.o"
    $4 -u "$3.o" >"$3.undef"
    awk "{print \$2}" "$3.undef" >"$3.names"
    rc=0; grep -vxE "memset|memcpy" "$3.names" >"$3.unexpected" || rc=$?
    cat "$3.undef"
    [ "$rc" -eq 1 ]' _ "$2" "$SRC" "$OUT/$1" "$3" "$INC"
}

cg=$(sed -n 's/^0:://p' /proc/self/cgroup); cgd=/sys/fs/cgroup/$cg
{ echo "cgroup=$cg"; for n in memory.max memory.swap.max cpu.max pids.max; do echo "$n=$(cat "$cgd/$n")"; done; } >"$OUT/effective-resources.txt"
{ [ "$(cat "$cgd/memory.max")" = 2147483648 ] && [ "$(cat "$cgd/memory.swap.max")" = 0 ] && [ "$(cat "$cgd/pids.max")" = 128 ] \
  && cpu_quota_ok "$(cat "$cgd/cpu.max")"; } || { echo "cgroup limits not as required" >&2; exit 90; }

if [ "${GCM_AS_SELFTEST:-0}" = 1 ]; then
  st=0
  for v in "100000 100000" "50000 100000" "1 1000" "010000 100000"; do
    cpu_quota_ok "$v" && echo "quota ok   [$v]" || { echo "SELFTEST FAIL: valid cpu.max rejected [$v]"; st=1; }
  done
  for v in "max 100000" "max" "" "0 100000" "100000 0" "200000 100000" "100001 100000" "100000" "100000 100000 1" "-1 100000" \
           "1e5 100000" "100000 100000x" "99999999999999999999 100000"; do
    cpu_quota_ok "$v" && { echo "SELFTEST FAIL: invalid cpu.max accepted [$v]"; st=1; } || echo "quota refused [$v]"
  done
  # nm failing must make the closure step fail (not look like an empty closure); a real nm must pass.
  fail=0; closure_step selftest-closure-nmfail O0 false >/dev/null 2>&1 && { echo "SELFTEST FAIL: failing nm passed"; st=1; } || echo "closure with failing nm: refused"
  fail=0; closure_step selftest-closure-nmok O0 "$NM" >/dev/null 2>&1 && echo "closure with real nm: passes" || { echo "SELFTEST FAIL: real nm closure failed"; st=1; }
  echo "selftest=$([ $st -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
  exit $st
fi

sha256sum "$SRC" "$ROOT/native/domain/gcm_address_space.h" "$ROOT/native/tests/test_gcm_address_space.c" \
  "$ROOT/native/domain/rsx_fifo_walker.c" "$ROOT/native/domain/rsx_fifo_walker.h" >"$OUT/inputs.sha256"
for O in O0 O2; do
  step gcm-as-$O-build $LIM gcc -std=gnu11 -$O -g -Wall -Wextra -Wconversion -Wno-sign-conversion -Werror $SAN $INC \
    "$ROOT/native/tests/test_gcm_address_space.c" "$SRC" "$ROOT/native/domain/rsx_fifo_walker.c" -o "$OUT/gcm-as-$O" \
    && step gcm-as-$O-run $LIM "$OUT/gcm-as-$O"
  closure_step closure-$O $O "$NM"
done
echo "overall=$([ $fail -eq 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $fail
