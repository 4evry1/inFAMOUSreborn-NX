#!/usr/bin/env bash
# Build the Nintendo Switch port from your own disc image and firmware.
#
#   GAME_ISO=/path/to/inFAMOUS.iso [GAME_DKEY=/path/to/inFAMOUS.dkey] \
#   RPCS3=/path/to/rpcs3 \
#   LIBSRE_SPRX=/path/to/dev_flash/sys/external/libsre.sprx   (or LIBSRE_PRX=<already decrypted>) \
#   NXVK_DIR=/path/to/nxvk GLSLANG_DIR=/path/to/glslang-switch \
#       switch/build.sh [tag] [version]
#
# Runs switch/scripts/10..60 in order. STEPS="40 50 60" runs a subset. Re-running is safe: 10 skips an extracted folder, 20 resumes
# through the installer, 30 and 40 skip finished outputs (FORCE=1 redoes them), 50 regenerates (seconds), 60 builds into a new folder.
# See switch/README.md for what each step reads and writes and for the prerequisites.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
steps="${STEPS:-10 20 30 40 50 60}"
for s in $steps; do
    case "$s" in
        10) "$here/scripts/10-extract-disc.sh" ;;
        20) "$here/scripts/20-generate-sources.sh" ;;
        30) "$here/scripts/30-ppu-objects.sh" ;;
        40) "$here/scripts/40-lift-firmware.sh" ;;
        50) "$here/scripts/50-generate-spu.sh" ;;
        60) "$here/scripts/60-build-nro.sh" "$@" ;;
        *) echo "unknown step $s" >&2; exit 2 ;;
    esac
done
