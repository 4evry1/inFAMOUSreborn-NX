# inFAMOUS for Nintendo Switch (native port, experimental)

This folder adds a Nintendo Switch target next to the Windows port in the rest of the repository. The Windows port is
untouched: it keeps its own installer, runtime and renderer. The Switch port reuses the same recompiled game code and the same
`ps3recomp` toolkit, and replaces the host side: a native AArch64 runtime for Horizon (memory, threads, the PS3 kernel objects,
SPURS, file system, audio, input) and a Vulkan renderer on top of [nxvk](https://github.com/PalindromicBreadLoaf/nxvk).

**Nothing in `switch/` is game code, game data, firmware or a console build.** The upstream `ps3recomp/fw_spu/` directory carries firmware modules from upstream, unchanged; this port adds none. You build the application
yourself, on your own PC, from your own disc image and your own PS3 system software. The result contains code derived from them
and must not be shared.

## State

Work in progress, not a finished port. On a Switch the game boots through the menus into gameplay, and saving and loading work. It runs at
about 4 frames per second in game (measured on hardware with development build rv71s). Movie (Bink) sound plays; in-game and menu sound
are silent. Loading is slow and the opening can stay black for a while because the audio engine runs slowly. Performance and sound are being
worked on.

How it is built: the PPU code of the game is recompiled to AArch64 and runs natively on Horizon; the SPU programs run on a native interpreter with the
hot ones lifted to C (Bink video decoder, the SPURS kernel and policy modules, the job modules); Sony's SPURS library (`libsre`) is lifted from your
firmware and runs as native code. Only the US release (`BCUS98119`, v02.00) is supported.

## Layout

| Path | Contents |
|---|---|
| `native/` | the native runtime: `domain/` (platform-free services), `adapters/` (Horizon, libnx, POSIX and renderer adapters), `diagnostics/`, `tests/` (host tests, and the composition root `test_startup_indirect.cpp` that is linked into the NRO), `spu_lifted/` (SPU lifters' glue and generators), `spu_neon/`, `sibling_video/` (vendored renderer, see below), `switch_main.c`, `Makefile` |
| `make/` | make overlays: `lle.mk` (the real build), `wws.mk`, `sibling.mk`, `restore_objects.mk`, `audio_out.mk` |
| `scripts/` | the build pipeline, steps 10 to 60, and `env.sh` |
| `tools/` | derivation tools (`derive_*.py`), the libsre glue generator, helpers |
| `patches/` | the small delta applied to one regenerated PPU unit |
| `build.sh` | runs the steps in order |

Shared code lives in the existing folders: `../ps3recomp/runtime/` (loader, memory, time, sync, process, platform), `../ps3recomp/libs/`.
Comments and some test scripts mention `evidence/...` and `artifacts/...` folders: those were the development workspace's
receipts and are not published. Host test scripts that need them are left out.

## What you need

- A PC with Linux (the scripts are bash; developed on Ubuntu-like systems) with `python3` (3.10+), `patch`, `cargo` (Rust, once, to build
  `../installer`), about 40 GB of free disk and 16 GB of RAM.
- The Switch toolchain: [devkitPro](https://devkitpro.org) `devkitA64` with `libnx` and the portlibs `switch-zlib`, `switch-expat`
  and `switch-sdl2` (headers only), **or** Docker with an image that has them (`SWITCH_IMAGE`, default `devkitpro/devkita64`;
  install the portlibs into a derived image). The build also runs `python3` inside that environment.
- Your disc image of the **US release** of inFAMOUS (`BCUS98119`, v02.00): an `.iso` (with its `.dkey` if it is an encrypted dump) or an
  extracted folder that contains `PS3_GAME`.
- [RPCS3](https://rpcs3.net), only to decrypt `EBOOT.BIN` (if there is no `EBOOT.elf` yet) and the firmware module.
- **Your PS3 system software**: `libsre.sprx` from `dev_flash/sys/external/` after installing the firmware in RPCS3 (or the already decrypted
  `libsre.prx`). The lift is pinned to one version of the module (SHA-256 `9e7ee6ed...37b6` of the decrypted file).
- nxvk built for the Switch: clone `https://github.com/PalindromicBreadLoaf/nxvk` at commit `c3abea1`, follow its own Switch build
  instructions, and point `NXVK_DIR` at the checkout (the build uses the archives under `switch/build/cross`, listed in `make/lle.mk` and `make/sibling.mk`).
- glslang built for the Switch (`GLSLANG_DIR`, with `lib/libglslang.a` and `lib/libglslang-default-resource-limits.a`). The vendored
  renderer needs it to translate shaders. The libraries used so far are glslang 15.1.0 with HLSL enabled, the optimizer off, built with
  devkitA64 and `-fno-exceptions -fno-rtti`; a configuration along the lines of
  `cmake -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake -DENABLE_HLSL=ON -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-fno-exceptions -fno-rtti"`
  gives them (not re-run for this publication).

Every path is an environment variable or an argument; nothing is tied to one machine. `scripts/env.sh` lists them all.

## Build from your own ISO

```sh
# from the repository root
export GAME_ISO=/path/to/inFAMOUS.iso          # your disc image
export GAME_DKEY=/path/to/inFAMOUS.dkey        # only for an encrypted dump
export RPCS3=/path/to/rpcs3                    # decrypts EBOOT.BIN and the firmware module
export LIBSRE_SPRX=/path/to/dev_flash/sys/external/libsre.sprx    # or LIBSRE_PRX=<decrypted libsre.prx>
export NXVK_DIR=/path/to/nxvk                  # nxvk, built for the Switch
export GLSLANG_DIR=/path/to/glslang-switch     # glslang, built for the Switch

switch/build.sh local 0.1.0                    # all steps; the NRO lands in switch/build/local-nro-<time>/
```

Or step by step (`GAME_DIR` defaults to `switch/build/game`, generated files go to `switch/build/generated`):

```sh
switch/scripts/10-extract-disc.sh        # disc image -> game folder, decrypted EBOOT.elf (checked against installer/infamous.toml)
switch/scripts/20-generate-sources.sh    # the Windows port's pipeline: PPU lift, SPU images, job modules, patches; then the Switch delta
switch/scripts/30-ppu-objects.sh         # memory/timebase derivation of the five PPU units, compiled-code index, AArch64 objects
switch/scripts/40-lift-firmware.sh       # libsre from your firmware: relocate, lift, glue
switch/scripts/50-generate-spu.sh        # lifted Bink decoder, libsre policy modules, WWS job modules
switch/scripts/60-build-nro.sh local 0.1.0
```

The long steps are 20 (it lifts about 170 MB of C++) and 30 (compiling it at `-O2`, 4 GB per compiler). Running a step again is safe: 10 does not
extract an existing folder again, 20 resumes through the installer (`SKIP_INSTALLER=1` only applies the Switch delta), 30 and 40 skip outputs that
exist (`FORCE=1` redoes them), 50 regenerates (seconds), and 60 always builds into a new folder.

## What each step reads and writes

| Step | Reads | Writes (all git-ignored) |
|---|---|---|
| 10 | your disc image | `build/game/PS3_GAME/...`, `EBOOT.elf` |
| 20 | `EBOOT.elf` | `../infamous-recomp/recompiled/`, `../infamous-recomp/spu/`, `../infamous-recomp/spu-images/` |
| 30 | the files above | `build/generated/{ppu-memory-context,ppu-timebase-context,compiled-code-index,wws_linked_image.c,ppu-objects}` |
| 40 | your firmware's `libsre` | `build/generated/{libsre,libsre-glue}` |
| 50 | `EBOOT.elf`, step 20 and 40 outputs | `native/spu_lifted/{bink,jobs}/generated/`, `native/spu_lifted/jobs/extra/lift_input/` |
| 60 | everything above, NXVK, glslang | `build/<tag>-nro-<time>/infamous-native-probe.{nro,elf}`, `build.log` |

The SPURS kernel is lifted during step 60 from the libsre image of step 40.

## Required input that is not generated (GCM init image)

`native/domain/gcm_init_image.c` and `.h` (the RSX command image that the firmware's `cellGcmInit` writes, 844 words) are derived
from an offline trace of the firmware module `libgcm_sys`. They are firmware-derived data and are not published, and the trace's inputs are
not yet scripted for public use, so a build from scratch stops in step 60 with a message until you provide both files (they are in
`.gitignore`). Their interface is `native/domain/gcm_init_service.h`; two matching test vectors (`tests/gcm_init_expected_methods.h`,
`tests/gcm_offset_table_expected.h`) are likewise not published. This is the one step of the pipeline that is not reproducible from the
disc image and system software alone.

## Running it on a console

The application is a homebrew NRO. It reads its data from `sdmc:/switch/infamous-native-spu-stage31/` (the folder name is historical):

- `EBOOT.elf`: your decrypted executable (from step 10);
- `heap-thread-id.flag`: an empty file that selects the start-up composition this build is made for;
- `bdvd/`, `hdd0/`, `app_home/`: the guest's `/dev_bdvd`, `/dev_hdd0` and `/app_home` roots. Put the disc folder contents (`PS3_GAME/...`) under
  `bdvd/`. Save data is written under `hdd0/`.

The NRO reserves a large guest heap (`LLE_HEAP_BYTES`, 1.5 GiB by default); launch it in a way that gives homebrew the full application memory
(a title-override launch rather than applet mode). Logs and status files are written next to the data. Add `-DPS3_NATIVE_NXLINK` through
`EXTRA_DEFINES` to mirror stdout to `nxlink`.

## Third-party code

- `native/sibling_video/` is a vendored copy of the video library of the [inFAMOUS 2 PC port](https://github.com/SamiEnazi/infamous2-port) (MIT, see
  `native/sibling_video/LICENSE` and `VENDORED_FROM.txt`), with the post-processing shaders (FidelityFX CAS and FSR 1, FXAA, SMAA) and `stb` headers
  it ships, each with its own licence notice.
- `native/spu_neon/neon_shim/arm_neon.h` is a small host-test stand-in written for this port.
- nxvk, glslang, devkitPro, libnx and RPCS3 are used, not included.

## Not supported

`native/Makefile` still carries the older start-up compositions from the development history (the feature blocks that are not selected by
`ENABLE_HEAP_THREAD_ID=1 ENABLE_N12_STARTUP=1`). They refer to pre-built objects and to check tools that are not published, and they are not
supported here. `scripts/60-build-nro.sh` builds the composition that has been run on a console.

## Left out on purpose

Game-, firmware- and capture-derived material is not part of this tree: lifted SPU modules and generated tables, the `libsre` image and lift,
the GCM init image (above), shader binaries emitted from captured game shaders, capture/replay experiments, console builds and logs. Build output
and everything generated lives in git-ignored folders. Pre-built objects, `.nro`/`.nso`/`.elf` files and run logs must not be committed.
