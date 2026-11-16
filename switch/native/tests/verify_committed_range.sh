#!/usr/bin/env bash
# Host verification for native/adapters/committed_range.h, the lock-free "every page committed?" check behind ps3_flat_guest_fast_range
# (the RSX consumer's FIFO reads and the renderer's guest reader). ASan+UBSan, O0 and O2. CPU only, no network.
#
# Usage: verify_committed_range.sh <fresh out-dir>
set -euo pipefail
ulimit -c 0
[[ $# -eq 1 ]] || { echo "usage: $0 <fresh out-dir>" >&2; exit 2; }
OUT="$1"
[[ ! -e "$OUT" ]] || { echo "REFUSED: $OUT exists; give a fresh directory" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$OUT"
sha256sum "$ROOT/native/adapters/committed_range.h" "$ROOT/native/tests/test_committed_range.c" > "$OUT/inputs.sha256"
for O in O0 O2; do
    echo "== variant $O =="
    "${CC:-gcc}" -std=c11 -"$O" -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        "$ROOT/native/tests/test_committed_range.c" -o "$OUT/committed-range-$O"
    "$OUT/committed-range-$O" > "$OUT/run-$O.log" 2>&1 || { cat "$OUT/run-$O.log"; echo "overall=FAIL run $O"; exit 1; }
    grep -E '^COMMITTED_RANGE checks=[1-9][0-9]* failures=0$' "$OUT/run-$O.log" || { cat "$OUT/run-$O.log"; echo "overall=FAIL line $O"; exit 1; }
done
echo "overall=PASS"
