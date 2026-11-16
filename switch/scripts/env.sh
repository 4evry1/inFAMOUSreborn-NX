#!/usr/bin/env bash
# Shared settings for the Switch build scripts. Source it; do not run it.
#
# Nothing here is tied to one machine: every path is an environment variable or a script argument.
#
#   REPO_ROOT       the checkout (parent of switch/); found from this file's location
#   SWITCH_WORK     scratch and output folder                     default: <repo>/switch/build   (git-ignored)
#   SWITCH_GEN      what is derived from your game and firmware   default: $SWITCH_WORK/generated
#   GAME_DIR        your extracted disc: the folder that holds PS3_GAME
#   GAME_ISO        your disc image (.iso), for 10-extract-disc.sh;   GAME_DKEY its .dkey, if the image is encrypted
#   RPCS3           path to an RPCS3 executable (only used to decrypt EBOOT.BIN and the firmware module)
#   LIBSRE_PRX      the decrypted libsre.prx of your PS3 system software;  or LIBSRE_SPRX, the encrypted libsre.sprx
#   NXVK_DIR        checkout of nxvk, built for the Switch (see switch/README.md)
#   GLSLANG_DIR     glslang built for the Switch: <dir>/lib/libglslang.a, libglslang-default-resource-limits.a
#   DEVKITPRO       devkitPro install (when you build without Docker)       default: /opt/devkitpro
#   SWITCH_USE_DOCKER   1 = run the Switch toolchain in Docker, 0 = use the local devkitPro, auto (default) = local if present
#   SWITCH_IMAGE    Docker image with devkitA64, libnx, zlib and expat   default: devkitpro/devkita64:latest
#   PYTHON          Python 3.10+                                          default: python3
set -euo pipefail

SWITCH_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd -- "$SWITCH_DIR/.." && pwd)"
SWITCH_WORK="${SWITCH_WORK:-$SWITCH_DIR/build}"
GEN="${SWITCH_GEN:-$SWITCH_WORK/generated}"
PYTHON="${PYTHON:-python3}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
NXVK_DIR="${NXVK_DIR:-$SWITCH_DIR/third_party/nxvk}"
GLSLANG_DIR="${GLSLANG_DIR:-$SWITCH_DIR/native/sibling_video/deps/glslang-install-switch}"
GAME_DIR="${GAME_DIR:-$SWITCH_WORK/game}"
SWITCH_IMAGE="${SWITCH_IMAGE:-devkitpro/devkita64:latest}"
SWITCH_USE_DOCKER="${SWITCH_USE_DOCKER:-auto}"
INSTALLER="${INSTALLER:-$REPO_ROOT/installer/target/release/infamous-installer}"
EBOOT_ELF="${EBOOT_ELF:-$GAME_DIR/PS3_GAME/USRDIR/EBOOT.elf}"
LIBSRE_DIR="$GEN/libsre"            # prx/ (relocated module) and lift/ (lifted C++), from your libsre.prx
LIBSRE_GLUE="$GEN/libsre-glue"
PPU_OBJECTS="$GEN/ppu-objects"

die() { echo "error: $*" >&2; exit 1; }
say() { echo "== $*"; }
need_file() { [ -f "$1" ] || die "$2 not found: $1"; }
need_dir() { [ -d "$1" ] || die "$2 not found: $1"; }
sha256() { sha256sum "$1" | cut -d' ' -f1; }

use_docker() {
    case "$SWITCH_USE_DOCKER" in
        1|yes|true) return 0 ;;
        0|no|false) return 1 ;;
        *) [ ! -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" ] ;;
    esac
}

# tc [-m MEM] [-c CPUS] -- COMMAND...      run COMMAND in the Switch toolchain environment.
# Local mode runs it directly (DEVKITPRO is exported). Docker mode runs it in $SWITCH_IMAGE with the same absolute
# paths: the checkout read-only, the work folder (and NXVK_DIR / GLSLANG_DIR when they are elsewhere) mounted as needed.
tc() {
    local mem=4g cpus=2
    while [ "${1:-}" != "--" ]; do
        case "$1" in -m) mem="$2"; shift 2 ;; -c) cpus="$2"; shift 2 ;; *) die "tc: bad option $1" ;; esac
    done
    shift
    if ! use_docker; then
        DEVKITPRO="$DEVKITPRO" "$@"
        return
    fi
    command -v docker >/dev/null || die "neither a local devkitPro ($DEVKITPRO) nor docker was found"
    mkdir -p "$SWITCH_WORK" "$GEN"
    local mounts=(--mount "type=bind,src=$REPO_ROOT,dst=$REPO_ROOT,readonly" --mount "type=bind,src=$SWITCH_WORK,dst=$SWITCH_WORK")
    case "$GEN/" in "$SWITCH_WORK/"*) ;; *) mounts+=(--mount "type=bind,src=$GEN,dst=$GEN") ;; esac
    local d
    for d in "$NXVK_DIR" "$GLSLANG_DIR"; do
        [ -d "$d" ] || continue
        case "$d/" in "$REPO_ROOT/"*) ;; *) mounts+=(--mount "type=bind,src=$d,dst=$d,readonly") ;; esac
    done
    for d in ${TC_EXTRA_MOUNTS_RO:-}; do mounts+=(--mount "type=bind,src=$d,dst=$d,readonly"); done
    docker run --rm --user "$(id -u):$(id -g)" --network none --read-only --tmpfs /tmp:rw,size=1g -e HOME=/tmp \
        --cap-drop ALL --security-opt no-new-privileges --ulimit core=0:0 \
        --cpus "$cpus" --memory "$mem" --memory-swap "$mem" --pids-limit 512 \
        "${mounts[@]}" --workdir "$PWD" --entrypoint "" "$SWITCH_IMAGE" "$@"
}

# the installer that regenerates the game code from your disc (built once with cargo)
ensure_installer() {
    [ -x "$INSTALLER" ] && return
    command -v cargo >/dev/null || die "cargo (Rust) is needed once to build installer/; or set INSTALLER to a built infamous-installer"
    say "building the installer"
    cargo build --release --manifest-path "$REPO_ROOT/installer/Cargo.toml"
    [ -x "$INSTALLER" ] || die "installer build did not produce $INSTALLER"
}

# the decrypted EBOOT.elf the port was made from (US disc, BCUS98119): sha256 is read from installer/infamous.toml
expected_eboot_sha256() {
    "$PYTHON" -I - "$REPO_ROOT/installer/infamous.toml" <<'PY'
import re, sys
text = open(sys.argv[1], encoding='utf-8').read()
print(re.search(r'eboot_sha256\s*=\s*\[\s*"([0-9a-f]{64})"', text).group(1))
PY
}
