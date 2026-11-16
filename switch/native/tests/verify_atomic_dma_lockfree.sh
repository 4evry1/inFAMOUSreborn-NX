#!/bin/sh
# Decision 135: plain SPU DMA without the domain guard. Builds and runs native/tests/test_atomic_dma_lockfree.c
#   x86-64: -O0 and -O2 under ASan+UBSan, -O2 under ThreadSanitizer, and -DPS3_ATOMIC_LOCKFREE_DMA=0 (the switch off: every DMA guarded);
#   AArch64 (when A64CC/A64RUN are given, e.g. the rootless arm64-runner): -O2, run under qemu;
# then the existing atomic-domain and SPU memory suites (verify_spu_memory.sh). usage: verify_atomic_dma_lockfree.sh <out dir> [rounds]
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:?usage: verify_atomic_dma_lockfree.sh <out dir> [rounds]}
rounds=${2:-600}
mkdir -p "$output"
sources="$root/native/tests/test_atomic_dma_lockfree.c $root/native/adapters/guard_pthread.c $root/../ps3recomp/runtime/memory/atomic_domain.c"
includes="-I$root/../ps3recomp/runtime/memory -I$root/native/tests"
for optimization in 0 2; do
    cc -std=c11 -O"$optimization" -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer $includes $sources -o "$output/asan-O$optimization"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 600 "$output/asan-O$optimization" "$rounds"
done
# (gcc warns that ThreadSanitizer does not model atomic_thread_fence: the Dekker step of the protocol is invisible to it, so a report naming a fenced access
# would be a false positive; none is expected because every guest byte is a relaxed atomic and the rest is guarded)
cc -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-error=tsan -Wno-tsan -pthread -fsanitize=thread $includes $sources -o "$output/tsan-O2"
TSAN_OPTIONS=halt_on_error=1 timeout 1200 "$output/tsan-O2" "$rounds"
cc -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -DPS3_ATOMIC_LOCKFREE_DMA=0 $includes $sources -o "$output/off-O2"
timeout 600 "$output/off-O2" "$rounds" > "$output/off.log" 2>&1 || true
grep -q ATOMIC_DMA_LOCKFREE_FAIL "$output/off.log" && grep -q "^DMA_TRANSITION rounds=$rounds exact=$rounds " "$output/off.log" && echo "(switch off: only the direct-path accounting assertions fail, as they should; data checks pass)"
if [ -n "${A64CC:-}" ] && [ -n "${A64RUN:-}" ]; then
    "$A64CC" -std=c11 -O2 -Wall -Wextra -Werror -pthread $includes $sources -o "$output/a64-O2"
    timeout 1800 "$A64RUN" "$output/a64-O2" "$rounds"
fi
sh "$root/native/tests/verify_spu_memory.sh" "$output/spu_memory"
echo "ATOMIC_DMA_LOCKFREE_VERIFY_PASS"
