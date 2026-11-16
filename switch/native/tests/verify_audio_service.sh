#!/usr/bin/env bash
# Host verification for the platform-free audio service (native/domain/audio_service.*):
# cellAudioOut* answers, cellAudio port lifecycle, the 48 kHz / 256-sample block clock and block tags.
# CPU only: no guest memory, no threads, no OS audio, no network.
#
# Usage: verify_audio_service.sh <fresh out-dir>     (run it under evidence/startup-event-receive-r2/run_bounded.sh)
set -euo pipefail
ulimit -c 0
[[ $# -eq 1 ]] || { echo "usage: $0 <fresh out-dir>" >&2; exit 2; }
OUT="$1"
[[ ! -e "$OUT" ]] || { echo "REFUSED: $OUT exists; give a fresh directory" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
mkdir -p "$OUT"
CC_BIN="${CC:-gcc}"
"$CC_BIN" --version | sed -n 1p > "$OUT/compiler.txt"
sha256sum "$ROOT/native/domain/audio_service.h" "$ROOT/native/domain/audio_service.c" \
    "$ROOT/native/tests/test_audio_service.c" > "$OUT/inputs.sha256"
for O in O0 O2; do
    echo "== variant $O =="
    "$CC_BIN" -std=c11 -"$O" -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        "$ROOT/native/tests/test_audio_service.c" "$ROOT/native/domain/audio_service.c" \
        -o "$OUT/audio-$O" > "$OUT/build-$O.log" 2>&1 || { cat "$OUT/build-$O.log"; echo "overall=FAIL build $O"; exit 1; }
    "$OUT/audio-$O" > "$OUT/run-$O.log" 2>&1 || { cat "$OUT/run-$O.log"; echo "overall=FAIL run $O"; exit 1; }
    grep -E '^AUDIO_SERVICE checks=[1-9][0-9]* failures=0$' "$OUT/run-$O.log" || { cat "$OUT/run-$O.log"; echo "overall=FAIL line $O"; exit 1; }
    # Closure: the domain object may import nothing but memset/memcpy and sanitizer runtime symbols.
    "$CC_BIN" -std=c11 -"$O" -Wall -Wextra -Werror -c "$ROOT/native/domain/audio_service.c" -o "$OUT/closure-$O.o"
    if nm -u "$OUT/closure-$O.o" | awk '{print $2}' | grep -vxE 'memset|memcpy|_GLOBAL_OFFSET_TABLE_' ; then
        echo "overall=FAIL closure $O"; exit 1
    fi
done
echo "overall=PASS"
