#!/usr/bin/env bash
# Step 3: turn the regenerated PPU sources into the AArch64 objects the NRO links.
#
#   switch/scripts/30-ppu-objects.sh            (after 20-generate-sources.sh)
#
#   1. derive_ppu_memory_context.py     the five units rewritten to go through the runtime's memory owner (hash-pinned input)
#   2. derive_ppu_timebase_context.py   the explicit CPU timebase on top of that
#   3. derive_compiled_code_index.py    the sorted function index used for indirect calls
#   4. gen_wws_image.py                 the linked-image table of the SPURS job-manager policy module
#   5. compile units 000..004 with devkitA64 (generated-code loads through the leaf helper, PPU_INLINE_LOADS=2)
#
# Outputs under $SWITCH_GEN (default switch/build/generated): ppu-memory-context, ppu-timebase-context, compiled-code-index,
# wws_linked_image.c, ppu-objects/ppu_recomp_00N.o. They are derived from your game and are never committed.
#   PPU_JOBS   compilers at once (default 2, each can use 4 GB)       PPU_MEMORY   per-compiler memory (default 4g)
#   FORCE=1    redo steps whose output already exists
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

for n in 000 001 002 003 004; do need_file "$REPO_ROOT/infamous-recomp/recompiled/ppu_recomp_$n.cpp" "recompiled unit $n (run 20-generate-sources.sh)"; done
need_file "$REPO_ROOT/infamous-recomp/spu/pm_wwsjob/spu_recomp.c" "regenerated pm_wwsjob (run 20-generate-sources.sh)"
TOOLS="$SWITCH_DIR/tools"
mkdir -p "$GEN"
step() { # step <output path> <label> <command...>: run unless the output exists
    local out="$1" label="$2"; shift 2
    if [ -e "$out" ] && [ -z "${FORCE:-}" ]; then say "$label: have $(basename "$out")"; return; fi
    [ -z "${FORCE:-}" ] || rm -rf "$out"
    say "$label"
    "$@"
}

step "$GEN/ppu-memory-context" "memory-context derivation" \
    "$PYTHON" "$TOOLS/derive_ppu_memory_context.py" --max-seconds "${DERIVE_MAX_SECONDS:-3600}" --output "$GEN/ppu-memory-context"
step "$GEN/ppu-timebase-context" "timebase derivation" \
    "$PYTHON" "$TOOLS/derive_ppu_timebase_context.py" --input "$GEN/ppu-memory-context" \
        --abi "$REPO_ROOT/ps3recomp/runtime/ppu/native_timebase_abi.h" \
        --abi-sha256 8183e8830f1d2999064b01d4492958d67b9aa4a3e1d12037b79b249d6241d9a5 --output "$GEN/ppu-timebase-context"
step "$GEN/compiled-code-index" "compiled-code index" \
    "$PYTHON" "$TOOLS/derive_compiled_code_index.py" --output "$GEN/compiled-code-index"
step "$GEN/wws_linked_image.c" "SPURS policy-module image table" \
    "$PYTHON" "$TOOLS/gen_wws_image.py" "$GEN"

DERIVED="$GEN/ppu-timebase-context"
mkdir -p "$PPU_OBJECTS"
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
compile_unit() {
    local n="$1"
    [ ! -f "$PPU_OBJECTS/ppu_recomp_$n.o" ] || [ -n "${FORCE:-}" ] || { say "unit $n: have object"; return 0; }
    say "compiling unit $n"
    tc -m "${PPU_MEMORY:-4g}" -c 2 -- "$CXX" -std=gnu++17 -O2 -fno-strict-aliasing -march=armv8-a+crc+crypto -mtune=cortex-a57 \
        -mtp=soft -fPIE -ffunction-sections -fdata-sections -frounding-math -ffp-contract=off -D__SWITCH__ -fmax-errors=8 \
        -DPPU_NO_INLINE_LOADS=1 -DPPU_NO_INLINE_STORES=1 -DPPU_NO_INLINE_STVX=1 -DPPU_INLINE_LOADS=2 \
        -include "$SWITCH_DIR/native/adapters/ppu_inline_memory.h" \
        -I"$DERIVED" -I"$DEVKITPRO/libnx/include" -I"$REPO_ROOT/ps3recomp/include" -I"$REPO_ROOT/ps3recomp/runtime" \
        -I"$REPO_ROOT/ps3recomp/runtime/ppu" -I"$REPO_ROOT/ps3recomp/runtime/spu" -I"$REPO_ROOT/ps3recomp" \
        -I"$REPO_ROOT/infamous-recomp/recompiled" -I"$REPO_ROOT/sdl3/include" \
        -c -MD -MF "$PPU_OBJECTS/ppu_recomp_$n.d" "$DERIVED/ppu_recomp_$n.cpp" -o "$PPU_OBJECTS/ppu_recomp_$n.o" \
        > "$PPU_OBJECTS/ppu_recomp_$n.log" 2>&1 || { tail -20 "$PPU_OBJECTS/ppu_recomp_$n.log" >&2; rm -f "$PPU_OBJECTS/ppu_recomp_$n.o"; die "unit $n failed (log: $PPU_OBJECTS/ppu_recomp_$n.log)"; }
}
jobs_max="${PPU_JOBS:-2}"
running=0
pids=()
set -m   # every background compile gets its own process group, so a failure can stop the others, compilers and containers included
abort() { local p; trap - INT TERM; for p in "${pids[@]}"; do kill -- "-$p" 2>/dev/null || true; done; wait 2>/dev/null || true; die "$1"; }
trap 'abort "interrupted"' INT TERM
for n in 000 001 002 003 004; do
    compile_unit "$n" &
    pids+=("$!")
    running=$((running + 1))
    if [ "$running" -ge "$jobs_max" ]; then
        wait -n || abort "a PPU unit failed to compile"
        running=$((running - 1))
    fi
done
wait || true
for n in 000 001 002 003 004; do need_file "$PPU_OBJECTS/ppu_recomp_$n.o" "object for unit $n"; done
say "PPU objects ready: $PPU_OBJECTS"
