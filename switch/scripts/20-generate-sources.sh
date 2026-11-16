#!/usr/bin/env bash
# Step 2: regenerate the game's recompiled PPU/SPU sources from your own copy (the PC port's own pipeline), then apply the
# Switch delta to the first PPU unit.
#
#   GAME_DIR=<folder with PS3_GAME> [RPCS3=...] [JOBS=n] switch/scripts/20-generate-sources.sh
#
# What happens (all outputs are git-ignored, they are derived from the game):
#   1. installer/ (the PC port's regenerator) runs with --no-build: PPU lift, SPU images, job modules, plugin blobs, patches.
#      It writes infamous-recomp/recompiled/ and infamous-recomp/spu/ in this checkout and checks them against installer/generated.lock.
#   2. switch/patches/ppu_recomp_000.switch.patch is applied to infamous-recomp/recompiled/ppu_recomp_000.cpp (two small
#      call-site changes the Switch runtime provides hooks for) and the result is checked against its pinned hash.
# The lift is the long step (tens of minutes, several GB of RAM). The installer resumes finished steps by itself;
# SKIP_INSTALLER=1 only applies the Switch delta (for a checkout whose sources were regenerated earlier).
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

need_file "$EBOOT_ELF" "EBOOT.elf (run switch/scripts/10-extract-disc.sh first)"
ensure_installer

unit="$REPO_ROOT/infamous-recomp/recompiled/ppu_recomp_000.cpp"
upstream_hash=b5076f4b8e2e3d7f67fe410c8f0854a6075a738932bf7da1b5df94b94747723e    # after installer/patches (generated.lock)
switch_hash=98ff1a9b2b1814c8ab1179eedb2e3e3d7072c16a1151b52e8b7d7da2ad3b5358      # after the Switch delta

args=(--root "$REPO_ROOT" --game "$GAME_DIR" --no-build --yes)
[ -n "${RPCS3:-}" ] && args+=(--rpcs3 "$RPCS3")
[ -n "${JOBS:-}" ] && args+=(--jobs "$JOBS")
if [ -z "${SKIP_INSTALLER:-}" ]; then
    say "regenerating the recompiled sources with the installer (long)"
    "$INSTALLER" "${args[@]}"
fi

need_file "$unit" "regenerated ppu_recomp_000.cpp"
have="$(sha256 "$unit")"
if [ "$have" = "$switch_hash" ]; then
    say "ppu_recomp_000.cpp already carries the Switch delta"
elif [ "$have" = "$upstream_hash" ]; then
    say "applying the Switch delta to ppu_recomp_000.cpp"
    # patch a scratch copy and move it into place only when the result is the pinned one, so a failure never leaves a half-patched unit
    mkdir -p "$SWITCH_WORK"
    scratch="$(mktemp -d -p "$SWITCH_WORK")"
    trap 'rm -rf "$scratch"' EXIT
    mkdir -p "$scratch/infamous-recomp/recompiled"
    cp -- "$unit" "$scratch/infamous-recomp/recompiled/ppu_recomp_000.cpp"
    patch -p1 -d "$scratch" --no-backup-if-mismatch < "$SWITCH_DIR/patches/ppu_recomp_000.switch.patch"
    have="$(sha256 "$scratch/infamous-recomp/recompiled/ppu_recomp_000.cpp")"
    [ "$have" = "$switch_hash" ] || die "ppu_recomp_000.cpp is $have after the delta, expected $switch_hash (the file was left unchanged)"
    cp -- "$scratch/infamous-recomp/recompiled/ppu_recomp_000.cpp" "$unit"
else
    die "ppu_recomp_000.cpp sha256 $have is neither the installer's patched output ($upstream_hash) nor the Switch variant: the installer did not finish, or the game version differs"
fi
say "recompiled sources ready"
