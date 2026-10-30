# Evidence — US PC build (Task 7), 2026-10-30

- Tree: inFAMOUSreborn-us @ `680a8beaec5ee612b5e98f2ebe92fa18aac47042` (branch `us-version-adaptation`)
- Host: Kubuntu Linux, 16 cores, 62 GB RAM, build output on Tardis

## Step 1 — supported Windows toolchain check

| tool | expected | found |
| --- | --- | --- |
| cmake | 3.20+ | 4.2.3 |
| ninja | any | 1.13.2 |
| clang-cl (VS 2022 Build Tools) | required | not invocable from this Linux session |
| python | 3.10+ | 3.14.4 |
| Wine | (not required, would help) | not installed |

The documented Windows 10/11 clang-cl + VS Build Tools + Ninja + D3D12 build cannot be
invoked from this Linux session, so the allowed **experimental LLVM-MinGW cross-build** was
performed. This is a diagnostic build, clearly distinguished from the supported playable
build; the supported route still needs a Windows boot (Task 8 owns the Windows run).

## Toolchain

- LLVM-MinGW 20250114 (LLVM 19.1.7, UCRT, `x86_64-w64-windows-gnu`), self-contained
  Linux-hosted release, extracted to
  `/run/media/user/Tardis/tools/llvm-mingw-20250114-ucrt-ubuntu-20.04-x86_64`
  (toolchain file `/run/media/user/Tardis/tools/inFamous-llvm-mingw.cmake`, not committed).
- CMake 4.2.3, Ninja 1.13.2, Python 3.14.4 (host).
- PGO: `installer/pgo/infamous.profdata` (written by LLVM 19) is readable by clang 19.1.7,
  so `PORT_PGO=USE` was applied, matching the documented build configuration.

## Commands

Configure (from `infamous-recomp/`):

```
cmake -S . -B build-win64-mingw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/run/media/user/Tardis/tools/inFamous-llvm-mingw.cmake \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPORT_PPU_REGCACHE=ON -DPORT_PPU_MEMLEAN=ALL \
  -DPORT_PGO=USE -DPORT_PGO_PROFILE=/run/media/user/Tardis/Projects/inFAMOUSreborn-us/installer/pgo/infamous.profdata \
  -DPORT_REQUIRE_SDL3=ON -DPORT_SCRUB_PRIVATE=ON \
  -DPS3RECOMP_DIR=/run/media/user/Tardis/Projects/inFAMOUSreborn-us/ps3recomp \
  -DPython3_EXECUTABLE=/usr/bin/python3
```

Build:

```
cmake --build build-win64-mingw --parallel 4
```

Configured features: PGO USE, SDL3 pads ON, DLSS/FSR off (no SDKs), lifted PPU register
cache ON, PPU memlean ALL, 5 lifted PPU TUs, 62 lifted SPU images (+31 register-cached twins).

## Artifact

- `infamous-recomp/build-win64-mingw/inFamousRecomp.exe`
- 111,191,552 bytes; PE32+ console executable for MS Windows, x86-64
- `IMAGE_FILE_MACHINE_AMD64`, 13 sections, `IMAGE_FILE_LARGE_ADDRESS_AWARE`
- TimeDateStamp 2026-10-30 18:59:19 UTC
- SHA-256: `d1fe32ed14b2bd6ae9729804b6bc31480c7411d747e063b4a3bf842c78a65836`
- Link map: `inFamousRecomp.map` (10,371,119 bytes) in the same directory
- Build duration: 5 min 23 s for the full compile at 4 jobs (plus short relinks after each
  integration fix). Build log: `build-win64-mingw/build.log` (0 errors).

Imported DLLs: ADVAPI32, D3DCOMPILER_47, KERNEL32, USER32, WINMM, WS2_32, XINPUT1_3,
bcrypt, d3d12, dbghelp, dxgi, ole32, UCRT `api-ms-win-crt-*`, `libc++.dll`,
`libunwind.dll`. SDL3.dll is loaded at run time (not in the import table).

The MinGW toolchain links libc++/libunwind dynamically, so Task 8 must place
`libc++.dll` and `libunwind.dll` (from the toolchain `bin/`) next to the exe, together
with `sdl3/lib/x64/SDL3.dll`, before the first Windows run.

## Build-integration fixes (authored, committed)

All fixes are narrow and do not change generated gameplay semantics:

1. `ps3recomp/CMakeLists.txt` — for MinGW only: link `synchronization`
   (WaitOnAddress / WakeByAddress* live in `libsynchronization.a`; the capital-S
   `Synchronization.lib` pragma does not resolve on case-sensitive Linux-hosted lld), and
   define `_WIN32_WINNT=0x0A00` (matches the Windows SDK default clang-cl builds against;
   mingw-w64 defaults lower and hides the Win10 CPU-set APIs the D3D12 backend uses).
2. `ps3recomp/runtime/ppu/ppu_loader.cpp` — declare `ppu_resv_break` with the same
   internal linkage as its definition; MSVC/clang-cl tolerate the mismatch, GNU-mode
   clang rejects it.
3. `ps3recomp/runtime/ppu/ppu_sysprx.cpp` — keep the `Synchronization.lib` pragma
   MSVC-only; MinGW resolves through `libsynchronization.a` (see 1).
4. `infamous-recomp/CMakeLists.txt` — MinGW only: `-fms-extensions` (SEH `__try` support,
   implicit under clang-cl); MSVC/MinGW link-map spelling (`/MAP:` vs `-Wl,-Map`).
5. `infamous-recomp/CMakeLists.txt` + `infamous-recomp/stubs.cpp` — the trainer calls EU
   guest functions by symbol (`func_002AF108` and ~25 others) that have no verified US
   counterparts (Task 6 deferred this). It is compiled out for the US fork
   (`INFAMOUS_TRAINER_US_UNVERIFIED`, default ON) and stubs.cpp supplies inert
   `infamous_trainer_install` / `infamous_trainer_anim_hook` / `infamous_trainer_move_pre` /
   `infamous_trainer_move_set` shims (the last two are called by lifted PPU code
   unconditionally; no-ops equal the unpatched guest flow). The install shim logs once:
   `[TRAINER] disabled: EU guest addresses not yet re-verified for the US EBOOT`.

No generated file was modified.

## Failures encountered (all resolved)

1. `GetSystemCpuSetInformation` / `SetThreadSelectedCpuSets` undeclared (MinGW
   `_WIN32_WINNT` default) — fixed by `_WIN32_WINNT=0x0A00` (fix 1).
2. `__try` undeclared in `ppu_loader.cpp` (SEH needs MS extensions under MinGW) —
   fixed by `-fms-extensions` (fix 4).
3. `static declaration of 'ppu_resv_break' follows non-static declaration` (linkage
   mismatch rejected by GNU-mode clang) — fixed by matching declaration (fix 2).
4. Trainer EU guest symbols (`func_002AF108`, `func_00203008`, ...) missing from the US
   lift — compile error proves the stale EU addresses; trainer compiled out until the US
   mapping is verified (fix 5).
5. Link: `/MAP:...` passed to the MinGW driver as a file — per-driver map spelling (fix 4).
6. Link: `libSynchronization.a` / then `WaitOnAddress` family undefined — MinGW
   `synchronization` import lib (fixes 1 and 3).

## Tests

- `cargo test --manifest-path installer/Cargo.toml --no-default-features` → 76 passed,
  0 failed (65 lib + 11 bin; the Task 6 US title/clock gate).
- `python3 infamous-recomp/tools/verify_us_title_addrs.py` → PASS (US EBOOT.elf matches the
  title-dependent runtime address ground truth).
- `cargo fmt --manifest-path installer/Cargo.toml --check` → 4 pre-existing diffs, all in
  `installer/src/title_probe.rs` (Task 6 file, not touched in Task 7; left unchanged).

## What this is NOT / first runtime gate

- The PE cannot run on this Linux host (no Wine); no PC runtime claim is made.
- The Linux null-backend development build was not attempted: it needs `libsdl2-dev` and
  non-interactive sudo is unavailable.
- Task 8 owns: booting Windows, staging the exe with `libc++.dll`, `libunwind.dll`,
  `SDL3.dll` (and the game folder), then the runtime probes from Task 6 (startup/loader,
  language list, clock, SPU registration, graphics device, first rendered frame), and the
  physical SD staging. Nothing was copied to the SD card in Task 7.
- In-tree follow-up: re-verify the trainer's EU guest addresses for the US EBOOT and remove
  the `INFAMOUS_TRAINER_US_UNVERIFIED` guard and stubs.cpp shims.
