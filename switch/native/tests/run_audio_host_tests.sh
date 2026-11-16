#!/usr/bin/env bash
# Host tests of the sound output (no console, no libnx): the platform-free units and the output pump against the real audio service.
# usage: native/tests/run_audio_host_tests.sh <scratch dir>      (the directory is created; objects and test binaries go there)
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?usage: run_audio_host_tests.sh <scratch dir>}"
mkdir -p "$out"
cflags="-Wall -Wextra -Werror -O2 -ffp-contract=off"
echo "== test_audio_mix (mixer, ring plan, queue policy)"
gcc -std=c11 $cflags -I "$root/native/domain" "$root/native/domain/audio_mix.c" "$root/native/domain/audio_out_policy.c" \
    "$root/native/tests/test_audio_mix.c" -lm -o "$out/test_audio_mix"
"$out/test_audio_mix"
echo "== test_startup_audio_out_pump (the pump, real audio service)"
for unit in audio_service audio_mix audio_out_policy; do
    gcc -std=c11 $cflags -c "$root/native/domain/$unit.c" -o "$out/$unit.o"
done
g++ -std=c++17 $cflags -I "$root/native/tests/audio_out_host" -I "$root/native/domain" -I "$root/../ps3recomp/runtime/time" \
    -I "$root/../ps3recomp/runtime/memory" "$root/native/tests/test_startup_audio_out_pump.cpp" \
    "$out/audio_service.o" "$out/audio_mix.o" "$out/audio_out_policy.o" -o "$out/test_startup_audio_out_pump"
"$out/test_startup_audio_out_pump"
