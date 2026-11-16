#!/bin/sh
# Native SPU float-policy contract (review F1). Host-only; the caller bounds resources. Usage: <output dir>
# 1. Negative compiles: natively, writing the desktop switches (g_spu_xfloat, g_spu_xflim) or selecting the desktop
#    runtime-limit form (SPU_XFLOAT_RUNTIME) must fail to compile, naming the reason. Positive control: the same write
#    compiles in a desktop build, so the native failure is the policy and not the fixture.
# 2. Behaviour on the host for the three native branches (portable = the branch AArch64 compiles, forced by undefining
#    the SSE feature macros; sse2; sse41), O0+O2 ASan/UBSan, linked with NO g_spu_xfloat/g_spu_xflim definition, then
#    nm proves neither symbol exists. 3. Exhaustive 2^32 clamp check per branch (O2, no sanitizer).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=$1
mkdir -p "$output"
spu="$root/../ps3recomp/runtime/spu"
# -Wno-misleading-indentation: pre-existing one-line helper style in spu_helpers.h (also warned by the target build).
base="-std=gnu11 -g -Wall -Wextra -Werror -Wno-misleading-indentation -frounding-math -ffp-contract=off -DPS3_SPU_EXTERNAL_FP_CONTROL -I$spu -I$root/../ps3recomp/include"
native="$base -DPS3_NATIVE_SPU_EXECUTOR"
portable="-U__SSE2__ -U__SSE3__ -U__SSSE3__ -U__SSE4_1__ -U__SSE4_2__ -U__AVX__ -U__AVX2__ -U__FMA__"
negative() {  # <name> <expected reason> <source> <flags...>
    name=$1; reason=$2; printf '%b\n' "$3" > "$output/$name.c"; shift 3
    if cc $native "$@" -c "$output/$name.c" -o "$output/$name.o" 2> "$output/$name.err"; then echo "NEGATIVE_COMPILED $name"; exit 6; fi
    grep -q -- "$reason" "$output/$name.err" || { echo "NEGATIVE_WRONG_REASON $name"; cat "$output/$name.err"; exit 6; }
    echo "NEGATIVE_REJECTED $name reason=$reason"
}
write='#include "spu_helpers.h"\nvoid policy_write(void) { g_spu_xfloat = 0; }'
negative write-portable g_spu_xfloat "$write" $portable
negative write-sse41 g_spu_xfloat "$write" -msse4.1
negative extern-write g_spu_xfloat '#include "spu_helpers.h"\nextern int g_spu_xfloat;\nvoid policy_write(void) { g_spu_xfloat = 0; }' $portable
negative xflim-write g_spu_xflim '#include "spu_helpers.h"\nvoid policy_write(void) { g_spu_xflim[0] = 0; }' -msse4.1
negative runtime-limits SPU_XFLOAT_RUNTIME '#include "spu_helpers.h"' -msse4.1 -DSPU_XFLOAT_RUNTIME
printf '%b\n' "$write" > "$output/desktop-control.c"
cc $base -msse4.1 -c "$output/desktop-control.c" -o "$output/desktop-control.o"
echo "DESKTOP_CONTROL_COMPILES write=g_spu_xfloat"
for variant in portable sse2 sse41; do
    case $variant in portable) flags=$portable;; sse2) flags="";; sse41) flags=-msse4.1;; esac
    for optimization in 0 2; do
        bin="$output/float-policy-$variant-O$optimization"
        cc $native $flags -O"$optimization" -fsanitize=address,undefined -fno-omit-frame-pointer \
            "$root/native/tests/test_spu_float_policy.c" -lm -o "$bin"
        nm "$bin" > "$bin.symbols"
        if grep -E ' (g_spu_xfloat|g_spu_xflim)$' "$bin.symbols"; then exit 7; fi
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 60 "$bin" > "$bin.log"
        cat "$bin.log"; grep -q "^FLOAT_POLICY_PASS branch=$variant " "$bin.log"
    done
    bin="$output/float-policy-$variant-exhaustive"
    cc $native $flags -O2 "$root/native/tests/test_spu_float_policy.c" -lm -o "$bin"
    timeout 300 "$bin" exhaustive > "$bin.log"; cat "$bin.log"
    grep -q "^FLOAT_POLICY_EXHAUSTIVE branch=$variant " "$bin.log"
done
echo "SPU_FLOAT_POLICY_PASS negative=5 desktop_control=1 variants=portable,sse2,sse41 symbols_absent=1 exhaustive=3"
