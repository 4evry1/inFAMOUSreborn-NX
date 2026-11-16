#!/usr/bin/env bash
# Host verification for the absent-device import answers (native/adapters/startup_absent_imports.h):
# the 23 NIDs the coordinator's hardware run r5 stopped on (cellKbInit first), the two record sizes
# (CellKbInfo 140, CellMouseInfo 648), the netctl state store, the NULL out pointer -> INVALID_PARAMETER
# path, a refused pointer -> BadPointer, and two NIDs that must stay NotHandled.
#
# CPU semantics only: a fake 64 KiB guest memory, no Switch header, no game, no GPU, no network.
#
# Usage: verify_startup_absent_imports.sh <fresh out-dir>
set -euo pipefail
ulimit -c 0

refuse() { echo "REFUSED (exit 3): $* -- no compiler or test was run" >&2; exit 3; }

[ $# -eq 1 ] && [ -n "$1" ] || { echo "usage: verify_startup_absent_imports.sh <fresh out-dir>" >&2; exit 2; }
OUT=$1
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
[ ! -e "$OUT" ] || { echo "REFUSED (exit 3): $OUT already exists -- give a fresh out-dir" >&2; exit 3; }
mkdir -p "$OUT"
[[ -z "$(ls -A "$OUT" 2>/dev/null)" ]] || { echo "REFUSED (exit 3): $OUT is not a fresh out-dir" >&2; exit 3; }

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ADAPTERS="$ROOT/native/adapters"
TEST="$ROOT/native/tests/test_startup_absent_imports.cpp"
HEADER="$ADAPTERS/startup_absent_imports.h"

for f in "$TEST" "$HEADER"; do
  [[ -f "$f" ]] || { echo "MISSING (exit 4): $f" >&2; exit 4; }
done

CXX_BIN=${CXX:-c++}
WARN="-std=c++17 -Wall -Wextra -Werror"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

sha256sum "$HEADER" "$TEST" > "$OUT/inputs.sha256"
echo "cxx=$("$CXX_BIN" --version | head -n1)" | tee "$OUT/compiler.txt"
cat "$OUT/inputs.sha256"

fail=0
for O in O0 O2; do
  binary="$OUT/absent-imports-$O"
  echo "== variant $O =="
  if ! "$CXX_BIN" $WARN -"$O" -g $SAN -I"$ADAPTERS" "$TEST" -o "$binary" > "$OUT/build-$O.log" 2>&1; then
    cat "$OUT/build-$O.log" >&2
    echo "BUILD_FAILED variant=$O" >&2
    fail=1
    continue
  fi
  set +e
  timeout 300s "$binary" > "$OUT/run-$O.log" 2>&1
  rc=$?
  set -e
  cat "$OUT/run-$O.log"
  if [ "$rc" -ne 0 ]; then
    echo "RUN_FAILED variant=$O exit=$rc (see $OUT/run-$O.log)" >&2
    fail=1
  fi
  if ! grep -qxE 'STARTUP_ABSENT_IMPORTS_PASS cases=[1-9][0-9]*' "$OUT/run-$O.log"; then
    echo "NO_PASS_LINE variant=$O" >&2
    fail=1
  fi
done

echo "overall=$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
exit "$fail"
