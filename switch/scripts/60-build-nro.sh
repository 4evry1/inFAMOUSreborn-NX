#!/usr/bin/env bash
# Step 6: build the Switch application (.nro) from the sources and the generated inputs of steps 2-5.
#
#   switch/scripts/60-build-nro.sh [tag] [version]        e.g.  60-build-nro.sh local 0.1.0
#
# Output: $SWITCH_WORK/<tag>-nro-<timestamp>/infamous-native-probe.nro (and its .elf), plus build.log in the same folder.
# The result contains code derived from your game and firmware: keep it to yourself; it is git-ignored and must not be shared.
#
# Needs (see switch/README.md): the devkitA64 toolchain with libnx, zlib and expat (local devkitPro, or SWITCH_IMAGE in Docker),
# NXVK_DIR (nxvk built for the Switch) and GLSLANG_DIR (glslang built for the Switch).
#   LLE_HEAP_BYTES    guest heap of the NRO                          default 1610612736 (1.5 GiB)
#   LLE_BUILD_MEMORY  memory limit of the build container            default 4g
#   LLE_LEAN=1        no logs / profiler (speed measurement build)
#   LLE_EMU=1         build for the Eglantine emulator (32 MiB flat-guest slabs, the emulator refuses many mappings); not for a console
#   EXTRA_DEFINES     extra preprocessor defines, e.g. -DPS3_NATIVE_NXLINK   default -DSTARTUP_DEADLINE_SECONDS=31536000 (no run deadline)
#   MAKE_ARGS         more make arguments (e.g. LLE_SPU_KERNEL=0)
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

tag="${1:-local}"
version="${2:-0.1.0-$tag}"
extra="${EXTRA_DEFINES--DSTARTUP_DEADLINE_SECONDS=31536000}"

for n in 000 001 002 003 004; do need_file "$PPU_OBJECTS/ppu_recomp_$n.o" "PPU object $n (run 30-ppu-objects.sh)"; done
need_file "$GEN/compiled-code-index/compiled_code_index.cpp" "compiled-code index (run 30-ppu-objects.sh)"
need_file "$GEN/wws_linked_image.c" "WWS image table (run 30-ppu-objects.sh)"
need_file "$LIBSRE_GLUE/lle_sre_generated.cpp" "libsre glue (run 40-lift-firmware.sh)"
need_dir "$LIBSRE_DIR/lift" "lifted libsre (run 40-lift-firmware.sh)"
need_dir "$SWITCH_DIR/native/spu_lifted/jobs/generated" "lifted job modules (run 50-generate-spu.sh)"
need_dir "$SWITCH_DIR/native/spu_lifted/bink/generated" "lifted Bink decoder (run 50-generate-spu.sh)"
need_dir "$NXVK_DIR/switch/build/cross" "nxvk build for the Switch (NXVK_DIR=$NXVK_DIR; see switch/README.md)"
need_file "$GLSLANG_DIR/lib/libglslang.a" "glslang for the Switch (GLSLANG_DIR=$GLSLANG_DIR)"
[ -f "$SWITCH_DIR/native/domain/gcm_init_image.c" ] || die "native/domain/gcm_init_image.c is missing: it is derived from the firmware's libgcm_sys and is not published; see switch/README.md ('GCM init image')"

out="$SWITCH_WORK/$tag-nro-$(date -u +%Y%m%dT%H%M%S)"
[ ! -e "$out" ] || die "$out exists"
mkdir -p "$out"
if [ -n "${LLE_LEAN:-}" ]; then title="inFAMOUS LEAN (no logs)"; else title="inFAMOUS LOGS (debug)"; fi
say "building into $out"
cd "$SWITCH_DIR/native"
set +e
tc -m "${LLE_BUILD_MEMORY:-4g}" -c "${BUILD_CPUS:-1}" -- make -f Makefile -f ../make/lle.mk -f ../make/sibling.mk \
    -f ../make/restore_objects.mk -f ../make/audio_out.mk -j1 \
    BUILD="$out" GEN="$GEN" REPO="$REPO_ROOT" DEVKITPRO="$DEVKITPRO" \
    ENABLE_HEAP_THREAD_ID=1 ENABLE_N12_STARTUP=1 ENABLE_SPURS_INITIALIZE=1 \
    NRO_HEAP_BYTES="${LLE_HEAP_BYTES:-1610612736}" PROBE_VERSION="$version" "PROBE_TITLE=$title" \
    N12_REUSED= N12_PPU_DIR="$PPU_OBJECTS" LLE_LIFT="$LIBSRE_DIR/lift" LLE_GLUE="$LIBSRE_GLUE" \
    NXVK="$NXVK_DIR" GLSLANG_DIR="$GLSLANG_DIR" LLE_SIBLING_RENDER=1 "LLE_LIVE_TRACE=$extra" \
    ${LLE_EMU:+LLE_EMU=$LLE_EMU} ${LLE_LEAN:+LLE_LEAN=$LLE_LEAN LLE_PROFILER=0 LLE_SPU_ESCAPE_DIAG=0} ${MAKE_ARGS:-} \
    all lle-nro > "$out/build.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    grep -E "error|Error|undefined reference|multiple definition" "$out/build.log" | head -40 >&2 || true
    die "build failed (rc=$rc); see $out/build.log"
fi
nro="$out/infamous-native-probe.nro"
need_file "$nro" "NRO"
say "NRO: $nro  sha256 $(sha256 "$nro")"
