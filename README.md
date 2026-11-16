# inFAMOUS Reborn NX: inFAMOUS for Nintendo Switch

**inFAMOUS (PlayStation 3, 2009) recompiled into a native Nintendo Switch homebrew program. No emulator on the console.**

This is the Nintendo Switch fork of [whompay/inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn), the native Windows port.
The fork adds a Switch target in [`switch/`](switch/) next to the PC port. PC downloads, the PC installer and PC bug reports belong to the upstream repository.

> **Status: experimental, not a finished port.** On a Switch the game boots through the menus into gameplay, and saving and loading work.
> It runs at about 4 frames per second in game (measured on hardware with development build rv71s). Movie (Bink) sound plays; in-game and menu
> sound are silent. Loading is slow and the opening can stay black for a while because the audio engine runs slowly. Performance and sound are
> being worked on. Details: [switch/README.md](switch/README.md).

**This fork adds no game files and no ready-made builds.** You build the application yourself, on your own PC, from your own copy of the
game. The upstream `ps3recomp/fw_spu/` directory carries firmware modules from upstream, unchanged; the Switch port adds none. Only the US
release (`BCUS98119`, v02.00) is supported.

## Build it from your own disc image

```sh
export GAME_ISO=/path/to/inFAMOUS.iso GAME_DKEY=/path/to/inFAMOUS.dkey RPCS3=/path/to/rpcs3
export LIBSRE_SPRX=/path/to/dev_flash/sys/external/libsre.sprx NXVK_DIR=/path/to/nxvk GLSLANG_DIR=/path/to/glslang-switch
switch/build.sh local 0.1.0        # the .nro lands in switch/build/local-nro-<time>/
```

**Known gap: one input cannot be generated yet.** The build needs the GCM init image (`switch/native/domain/gcm_init_image.c` and `.h`), a table
derived from a trace of the firmware's `libgcm_sys`. That derivation is not reproducible from your disc image and firmware alone, and the data is
firmware-derived, so it is not published. Until you provide those two files the build stops in step 60 with a message naming them. Details in
[switch/README.md](switch/README.md).

The same steps one at a time (`switch/scripts/10-extract-disc.sh` to `60-build-nro.sh`), what each reads and writes, and how to put the result
on an SD card are in [switch/README.md](switch/README.md).

## What you need

- **Your own disc image** of the US release: an `.iso` (with its `.dkey` for an encrypted dump), or an extracted folder that contains `PS3_GAME`.
- **Your own PS3 system software.** The build lifts one system library, `libsre`, from it: install the firmware (`PS3UPDAT.PUP`) in
  [RPCS3](https://rpcs3.net) and take `libsre.sprx` from its `dev_flash`. RPCS3 also decrypts `EBOOT.BIN` and that library.
- **A Linux PC** with Python 3.10+, `patch`, Rust (`cargo`, once, for the PC port's installer, which regenerates the game code from your disc), about
  40 GB of disk and 16 GB of RAM.
- **The Switch toolchain:** [devkitPro](https://devkitpro.org) (devkitA64, libnx, zlib, expat, SDL2 headers) or Docker with an image that has them.
- **nxvk** ([PalindromicBreadLoaf/nxvk](https://github.com/PalindromicBreadLoaf/nxvk), commit `c3abea1`) and **glslang**, both built for the Switch.
- **A Switch that runs homebrew** with an SD card.

Everything below this line is the upstream PC port's README, kept as it is. Its features, numbers and install steps describe the Windows builds, not the Switch one.

---

# inFamous PC

> **Official repo: [github.com/whompay/inFAMOUSreborn](https://github.com/whompay/inFAMOUSreborn).**
> Only download the Setup from the [Releases page](https://github.com/whompay/inFAMOUSreborn/releases) here.
> Forks are not supported, and bugs from forks won't be fixed.

An unofficial native Windows port of **inFamous** (PlayStation 3, 2009, US
release `BCUS98119`). The game's PowerPC (PPU) and SPU code is statically
recompiled to x86-64 C/C++ with [ps3recomp](https://github.com/sp00nznet/ps3recomp),
so it runs as a normal Windows program rather than under an emulator.

**This repository contains no game code or data.** You build the game yourself,
on your own PC, from your own copy of the game.

## Easy install

Download **inFamous Setup.exe** from the latest release, run it, choose your
`.iso` (an encrypted image works with its `.dkey` next to it) or your game
folder (the one with `PS3_GAME` in it), pick an install location and press
**Install**.

Setup checks for Python and the C++ build tools and offers to install them,
copies the game out of the disc image, regenerates the recompiled code from your
copy, builds it and adds an inFamous shortcut. Running Setup again on the same
folder updates or repairs the install and only redoes what changed.

## Updating

When a new version is published here, players update in place instead of reinstalling:
**Update now** in Setup (it finds the existing install), the **Update inFamous** shortcut in
the Start menu, or `Update inFamous.cmd` in the install folder. The updater compares every file
with this repository, downloads only what changed, regenerates the game code only if the
port's patches changed, and recompiles only the affected parts. Installs made with an older
Setup need one run of the new Setup on the same folder first.

## Requirements

- Your own inFamous disc dump, US release (`BCUS98119`).
- Windows 10 or 11 (64-bit) and a GPU with Direct3D 12 support.
- A CPU with SSE4.1 (2008 or newer). On CPUs without AVX2 (Intel before 2013, Pentium/Celeron
  without AVX, AMD before 2015) Setup builds a slower compatibility version automatically.
  On PCs with 16 or fewer CPU threads the game uses fewer threads and less CPU (tested down to 4 threads).
- [Python 3.10+](https://www.python.org/downloads/) with "Add python.exe to
  PATH" ticked.
- [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/)
  with the **Desktop development with C++** workload and the **C++ Clang
  Compiler for Windows** component.
- [RPCS3](https://rpcs3.net), only if your `PS3_GAME\USRDIR` has `EBOOT.BIN`
  but no decrypted `EBOOT.elf`. Setup uses it to decrypt the executable.
- About 10 GB of free disk space and 16 GB of RAM.

## Building manually

```bat
cd installer
cargo build --release
target\release\infamous-installer --game "D:\Dumps\inFamous"
```

`infamous-installer doctor` checks the requirements only. Finished steps are
skipped when you run it again, so an interrupted install continues where it
stopped. See [installer/README.md](installer/README.md) for every step and the
maintainer commands.

## Playing

Run **inFamous** (the shortcut Setup made). It opens a small options window first
(vsync, frame cap, resolution, language and more, saved to `settings.ini`);
**Play** starts the game. The same settings are in the game's pause menu under
**Settings**. Controllers work as on the console, and keyboard and mouse are supported.

**F10** opens the in-game trainer: God Mode, Infinite Energy, Refill Health,
Max Health x2/x5/x10, Add XP, Karma +/-, Kill Enemies / Civilians, Super Speed
and Player Model (play as Zeke, Trish, Kessler, Alden or John). Set `TRAINER=0`
to turn it off.

## DLSS and FSR (optional)

The game can use NVIDIA DLSS / DLAA or AMD FSR for upscaling, and AMD FSR frame generation.
They are not included in the download because NVIDIA and AMD distribute their SDKs themselves,
but both are free and take a few minutes to add.

- **DLSS** needs an NVIDIA RTX graphics card.
- **FSR** works on any modern GPU (NVIDIA, AMD or Intel). FSR 4 needs a Radeon RX 9000 card;
  other cards get FSR 3.1 automatically.

**1. Download the SDK(s) you want.** You need the **source code**, not a Windows or Linux demo.
On the page, click the green **Code** button, then **Download ZIP**. If you go through the
Releases page instead, pick **Source code (zip)**, not the files with "demo" or "samples" in the name.

| For | Download | Rename the extracted folder to |
|---|---|---|
| DLSS / DLAA | [github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS) | `dlss-sdk` |
| FSR and frame generation | [github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | `fsr-sdk` |

**2. Put the folder(s) in your inFamous install folder**, next to `infamous-recomp` and
`ps3recomp`:

```
inFamous\
  dlss-sdk\        <- contains include\ and lib\
  fsr-sdk\         <- contains Kits\
  infamous-recomp\
  ps3recomp\
  ...
```

Make sure you don't end up with a folder inside a folder (`dlss-sdk\DLSS-main\include`).
`include` (DLSS) or `Kits` (FSR) must be directly inside.

**3. Rebuild the game once.** Open a Command Prompt in the install folder and run:

```bat
type nul > .installer\update.pending
"Update inFamous.cmd"
```

This rebuilds the game with the SDK(s), which takes a while. Any later update also picks them up
automatically, so you only do this once.

**4. Turn it on in game:** pause, then **Settings**, then **Upscaling** (DLAA, DLSS Quality /
Balanced / Performance, FSR Native AA / Quality / Balanced / Performance) and **Frame
Generation**. The same options are in the launcher's options window. Upscaling and frame
generation apply after a restart.

If the **Upscaling** row is missing, the build didn't find the SDK: check the folder names and
layout from step 2, then repeat step 3.

## How it works

| Path | Contents |
|---|---|
| `ps3recomp` | The recompiler toolkit and runtime: PPU and SPU lifters, HLE system libraries, RSX on Direct3D 12, audio and input |
| `infamous-recomp` | The inFamous-specific parts: program entry, game stubs and fixes, asset tools |
| `installer` | Setup and the command-line installer: reads the disc, regenerates the code from your game, applies `installer/patches`, builds |
| `installer/infamous.toml` | Everything about the game and the port: pipeline steps, generated files, build and launch settings |
| `installer/generated.lock` | Hashes of each generated file before and after the port's patches |

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet and
  contributors (see [ps3recomp/CONTRIBUTORS.md](ps3recomp/CONTRIBUTORS.md)).
- [RPCS3](https://rpcs3.net), used to decrypt the game executable.
- Sucker Punch Productions, for the game.
- [Lorenzo Aiello](https://github.com/aiellolorenzo23) and [ibrahimpuri](https://github.com/ibrahimpuri), for their support.
