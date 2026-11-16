#!/usr/bin/env bash
# Step 1: from your own disc image to a game folder with a decrypted EBOOT.elf.
#
#   GAME_ISO=/path/to/inFAMOUS.iso [GAME_DKEY=/path/to/inFAMOUS.dkey] RPCS3=/path/to/rpcs3 \
#       switch/scripts/10-extract-disc.sh [output game folder]
#
# - GAME_ISO   your disc image of the US release (BCUS98119). A 1:1 encrypted dump needs its .dkey (GAME_DKEY, default
#              <image>.dkey next to it) or the key itself in GAME_KEY (32 hex digits).
# - skip the image step by pointing GAME_DIR at an already extracted folder (the one that holds PS3_GAME).
# - RPCS3      only needed when PS3_GAME/USRDIR has EBOOT.BIN but no decrypted EBOOT.elf; `rpcs3 --decrypt` writes it.
#
# Nothing is copied anywhere but into the output folder, which is git-ignored by default (switch/build/game).
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"

[ $# -ge 1 ] && GAME_DIR="$1" && EBOOT_ELF="$GAME_DIR/PS3_GAME/USRDIR/EBOOT.elf"

if [ -n "${GAME_ISO:-}" ] && [ -d "$GAME_DIR/PS3_GAME" ]; then
    say "$GAME_DIR already holds PS3_GAME: not extracting again (remove the folder to start over)"
elif [ -n "${GAME_ISO:-}" ]; then
    need_file "$GAME_ISO" "disc image (GAME_ISO)"
    ensure_installer
    key_args=()
    if [ -n "${GAME_DKEY:-}" ]; then key_args=(--dkey "$GAME_DKEY")
    elif [ -n "${GAME_KEY:-}" ]; then key_args=(--key "$GAME_KEY"); fi
    say "extracting $GAME_ISO"
    mkdir -p "$GAME_DIR"
    "$INSTALLER" extract-iso --iso "$GAME_ISO" --out "$GAME_DIR" "${key_args[@]}"
fi

need_dir "$GAME_DIR/PS3_GAME/USRDIR" "game folder (GAME_DIR must contain PS3_GAME/USRDIR; set GAME_ISO to extract an image)"

if [ ! -f "$EBOOT_ELF" ]; then
    [ -n "${RPCS3:-}" ] || die "no decrypted EBOOT.elf in $GAME_DIR/PS3_GAME/USRDIR: set RPCS3 to an RPCS3 executable, or decrypt EBOOT.BIN yourself (RPCS3: Utilities > Decrypt PS3 Binaries)"
    need_file "$GAME_DIR/PS3_GAME/USRDIR/EBOOT.BIN" "EBOOT.BIN"
    say "decrypting EBOOT.BIN with RPCS3"
    "$RPCS3" --decrypt "$GAME_DIR/PS3_GAME/USRDIR/EBOOT.BIN"
fi
need_file "$EBOOT_ELF" "EBOOT.elf"

want="$(expected_eboot_sha256)"
have="$(sha256 "$EBOOT_ELF")"
[ "$have" = "$want" ] || die "EBOOT.elf sha256 $have does not match the supported US disc ($want); only BCUS98119 v02.00 is supported"
say "game folder ready: $GAME_DIR (EBOOT.elf sha256 verified)"
