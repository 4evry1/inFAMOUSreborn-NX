#!/usr/bin/env bash
# Step 4: lift the PS3 system library libsre (SPURS / cellSync) from YOUR OWN firmware and generate the glue unit for it.
#
#   LIBSRE_PRX=/path/to/libsre.prx                          switch/scripts/40-lift-firmware.sh
#   LIBSRE_SPRX=/path/to/libsre.sprx RPCS3=/path/to/rpcs3   switch/scripts/40-lift-firmware.sh     (decrypts it first)
#
# libsre.sprx is /dev_flash/sys/external/libsre.sprx of the PS3 system software. Install the firmware in RPCS3 (File >
# Install Firmware, with PS3UPDAT.PUP from Sony) and take it from RPCS3's dev_flash folder. Firmware is Sony's and is neither
# shipped nor published here; the lifted code and the glue are derived from it and stay under $SWITCH_GEN.
#
# The build is made for one version of the module (the base address, TOC and extent below belong to it); another firmware
# version is refused unless you set LIBSRE_ALLOW_ANY=1, which is unsupported and will most likely fail at the checks.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

expected_sha256=9e7ee6edc99b1d372686cf927b883b18f19ed4d32d1faf35e214cca4ae0737b6   # decrypted libsre.prx the port was made against
BASE=0x30000000
TOC=0x30039AB0
TEXT_END=0x30030000
IMG_SIZE=$((0x32340))

# a finished lift needs no firmware input at all
if [ -d "$LIBSRE_DIR" ] && [ -f "$LIBSRE_GLUE/lle_sre_generated.h" ]; then
    if [ -z "${FORCE:-}" ]; then say "libsre already lifted: $LIBSRE_DIR (FORCE=1 lifts again)"; exit 0; fi
fi

if [ -z "${LIBSRE_PRX:-}" ]; then
    [ -n "${LIBSRE_SPRX:-}" ] || die "set LIBSRE_PRX (decrypted libsre.prx) or LIBSRE_SPRX (encrypted) and RPCS3"
    [ -n "${RPCS3:-}" ] || die "decrypting LIBSRE_SPRX needs RPCS3 (path to the executable)"
    need_file "$LIBSRE_SPRX" "libsre.sprx"
    mkdir -p "$GEN/firmware"
    cp -- "$LIBSRE_SPRX" "$GEN/firmware/libsre.sprx"
    say "decrypting libsre.sprx with RPCS3"
    "$RPCS3" --decrypt "$GEN/firmware/libsre.sprx"
    LIBSRE_PRX="$GEN/firmware/libsre.prx"
fi
need_file "$LIBSRE_PRX" "decrypted libsre.prx"
have="$(sha256 "$LIBSRE_PRX")"
if [ "$have" != "$expected_sha256" ] && [ -z "${LIBSRE_ALLOW_ANY:-}" ]; then
    die "libsre.prx sha256 $have is not the supported module ($expected_sha256); see the header of this script"
fi

# work in a partial folder and move it into place at the end: an interrupted lift never leaves a half-written $LIBSRE_DIR
rm -rf "$LIBSRE_DIR" "$LIBSRE_GLUE" "$LIBSRE_DIR.partial"
TOOLS="$REPO_ROOT/ps3recomp/tools"
work="$LIBSRE_DIR.partial"
prx="$work/prx"; lift="$work/lift"
mkdir -p "$prx" "$lift" "$LIBSRE_GLUE"

say "relocate (lift_prx.py)"
"$PYTHON" "$TOOLS/lift_prx.py" "$LIBSRE_PRX" --base "$BASE" --output "$prx/"
image="$prx/libsre_image.bin"
[ "$(stat -c%s "$image")" = "$IMG_SIZE" ] || die "relocated image has the wrong size (expected $IMG_SIZE)"

say "HLE stub map"
"$PYTHON" "$SWITCH_DIR/tools/lle/make_hle_stubs.py" "$prx/libsre_imports.json" "$prx/libsre_hle_stubs.json"

say "lift (ppu_lifter.py)"
"$PYTHON" "$TOOLS/ppu_lifter.py" "$image" --raw --base "$BASE" --toc "$TOC" \
    --functions "$prx/libsre_functions.json" --hle-stubs "$prx/libsre_hle_stubs.json" \
    --symbol-prefix libsre_ --code-end "$TEXT_END" --output "$lift/" -j 1
"$PYTHON" -I "$SWITCH_DIR/tools/lle/patch_import_stubs.py" "$lift"
"$PYTHON" "$SWITCH_DIR/tools/lle/check_lift.py" "$prx" "$lift"

mv "$work" "$LIBSRE_DIR"
prx="$LIBSRE_DIR/prx"; image="$prx/libsre_image.bin"
say "glue unit"
"$PYTHON" "$SWITCH_DIR/tools/lle/gen_lle_sre_glue.py" "$LIBSRE_DIR" "$LIBSRE_GLUE/lle_sre_generated.cpp"
cat > "$LIBSRE_GLUE/lle_sre_generated.h" <<HEADER
#ifndef LLE_SRE_GENERATED_H
#define LLE_SRE_GENERATED_H
#include "lle_sre.h"
constexpr unsigned kLibsreImageSize = $(stat -c%s "$image")u;
#endif
HEADER
say "libsre lifted: $LIBSRE_DIR  glue: $LIBSRE_GLUE"
