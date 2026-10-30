# inFAMOUSreborn US-disc adaptation design

Date: 2026-10-30
Status: proposed
Base revision: `2e9405c72408d28f505b6636a2f9cb6b6125aa85`
Work root: `/run/media/user/Tardis/Projects/inFAMOUSreborn-us`

## Intent

Adapt the public inFAMOUSreborn Windows port to the user's legally owned US
release, `inFamous (USA) (En,Fr,Es) (v02.00)`, title ID `BCUS98119`, then use
that build to test the game on the hacked Nintendo Switch through Autorun.

The result is a local fork. It must not ship or expose the game, its decrypted
executable, extracted assets, or generated game code.

## Evidence and constraints

- The source ISO is a PS3 disc image containing `PS3_GAME`, `USRDIR/EBOOT.BIN`,
  and the retail SELF executable.
- The current upstream port supports only European `BCES00220`, with decrypted
  executable SHA-256
  `095d851432f3b55179cd725b46838ffedb3e5eb9f73889e52b6f11f3d3f494fd`.
- The installer enforces both the title ID and the executable hash.
- The port carries 114 patches totaling approximately 42 MB. They cover lifted
  PPU and SPU source, and the patch application is intentionally strict about
  fixed line contexts and base hashes.
- The US executable has not yet been decrypted or fingerprinted.
- PowerShell/Windows compilation is the supported production path. The Linux
  build can help validate generated code, but it does not provide the playable
  Direct3D 12 renderer.
- Autorun runs Windows x86-64 programs by translating them to ARM64. The PC
  port is already CPU-heavy, so Switch playability is not assumed.
- Tardis has enough working space for the repository, extracted game, generated
  sources, and build artifacts. It remains the primary work root.

## Goals

1. Accept only the exact measured `BCUS98119` executable supported by this fork.
2. Regenerate the PPU, SPU, job-module, asset, and registration sources from the
   user's US copy.
3. Rebase the existing port modifications onto those US sources.
4. Build a Windows x86-64 executable using a supported compiler.
5. Stage the build and its required game files for an Autorun run on the Switch.
6. Preserve logs and fingerprints that distinguish source generation, PC build,
   PC runtime, Autorun runtime, and physical Switch acceptance.

## Non-goals

- Circumventing DRM, ownership checks, or encryption.
- Publishing or committing game data, decrypted code, generated code, or saves.
- Supporting inFamous 2 or other inFamous regional releases.
- Building a native Horizon/Vulkan port of ps3recomp.
- Promising useful Switch frame rates before hardware measurement.

### Later candidate: inFamous 2

The user also owns a copy of inFamous 2 and wants to investigate it if the first
adaptation succeeds. That is a separate project: it may reuse ps3recomp runtime
work, but it needs its own executable/SPU lift, generated-code patches, address
audit, compatibility fixes, and acceptance run. No inFamous 2 work is included
in this design or in the first fork's success criteria.

## Anti-goals

- Changing only the title-ID check and calling the US build supported.
- Using `--force` to bypass executable or generated-file hash mismatches.
- Accepting a partially rebased patch set.
- Copying EU generated code into the US build.
- Calling a compile or launch successful when the game has not reached a
  repeatable playable state on the Switch.

## Approaches considered

### 1. Relabel the EU port as US

Rejected. It would stop at the visible title-ID rejection while leaving the
executable hash, lifted addresses, SPU images, job modules, patches, and tests
EU-specific.

### 2. Rebase the existing patch set onto US generated output

Selected. Generate US sources with the existing pipeline, then map every entry
in the EU hash lock to the corresponding US generated file. An existing patch
may be reused only when its full context applies to the US file and the resulting
file is recorded in a complete US lock. This preserves the port's maintained
behavior without inventing a new final source tree.

Trade-off: it requires the relevant US-generated code to retain enough structure
for the EU hunks to apply. Any context failure is a hard stop followed by
file-level diagnostics.

### 3. Maintain a separate US final generated tree

This is the fallback if patch rebasing cannot cover the US output. It requires
maintainer-level semantic reconciliation of every divergent PPU/SPU area and is
materially larger than a rebase. It should not be attempted blindly inside this
design.

## Proposed implementation

### Variant identity

The fork changes its manifest, metadata, launch text, and diagnostics to identify
the US release. The executable SHA recorded in the manifest and generated lock
must come from the user's decrypted `EBOOT.elf`, not from a guessed or shared
value.

The source ISO is treated as immutable. Extraction, RPCS3 decryption, generated
files, and builds use dedicated directories under the Tardis work root.

### Ingest and fingerprinting

1. Extract the ISO into a private working directory.
2. Use a recorded, pinned RPCS3 release to decrypt the retail SELF.
3. Verify the result is a big-endian PPC64 PS3 ELF.
4. Record its SHA-256, size, entry point, TOC, and image manifest.
5. Compare those values with the EU baseline. A failed comparison starts the
   patch-rebase path; it does not trigger a bypass.

### Patch rebasing

Add a maintainer command or equivalent reproducible tool that performs this
transaction:

1. Run the normal manifest pipeline against the US executable in an isolated
   work tree.
2. Load the existing EU lock and patch directory.
3. For every lock entry:
   - retain an identical generated file as both US base and final;
   - retain a patch-created file after verifying its final hash;
   - retain a deleted file;
   - apply the existing patch to the US base only if its complete fixed context
     and line-ending conversion succeed;
   - otherwise abort with file, line, and expected/actual context diagnostics.
4. Write a candidate US lock containing only hashes derived from the US run.
5. Validate that every required generated file is represented exactly once and
   that no unexplained file remains.
6. Promote the candidate lock atomically only after all entries succeed.

Patch files are reused rather than duplicated when the same hunk applies to both
regions. They are not fuzzy-matched or silently shifted.

The installer remains fail-closed. `--force` is not part of the supported US
path.

### Code and metadata changes

- Update the installer manifest and metadata to `BCUS98119`.
- Replace hardcoded EU executable hashes after measuring the legitimate US file.
- Make user-facing title/hash errors region-neutral or US-specific.
- Update the launcher/title metadata and setup defaults.
- Audit title-specific hardcoded addresses and fail-closed probes, especially
  the clock, language, TOC, display, and SPU paths.
- Preserve upstream behavior where an address or data table is verified
  identical. Disable or reject an optional feature when its probe proves the US
  layout differs.

### Build and verification

Verification has separate gates:

1. Rust installer tests and Python tool tests pass.
2. The US game is extracted and decrypted successfully.
3. The full generation pipeline completes.
4. All required patches rebase successfully and the US lock validates.
5. A clean Linux compile catches generated-code and patch-integration errors.
6. A documented Windows clang-cl build produces the playable executable.
7. The executable starts on Windows and reaches a repeatable in-game state.
8. Autorun starts that executable on the Switch and leaves complete logs.
9. Hardware acceptance records menu access, control/audio behavior, frame rate,
   memory pressure, and the first run-ending blocker.

No earlier gate is presented as proof of a later one.

### Privacy and repository hygiene

- The ISO, extracted game, decrypted `EBOOT.elf`, generated sources, shader
  caches, saves, screenshots, and runtime logs stay out of Git.
- No fork is published and no upstream PR is opened without a separate approval.
- Existing user files and Switch configuration are backed up before any
  on-device staging.

## Failure behavior

- Patch context mismatch: stop, preserve the generated output, and report the
  exact uncovered files. Do not weaken the patch checker.
- US executable hash mismatch: reject unknown dumps and ask for the exact
  supported revision.
- Build failure: preserve the complete build log and generated tree.
- PC runtime failure: retain the Windows crash/log evidence before attempting
  Autorun.
- Switch failure: retain Autorun, stdout/stderr, and progress logs; distinguish
  launch, menu, gameplay, performance, and memory failures.

## Deferred decisions

- Pin the tested RPCS3 version after checking its release digest.
- Choose the Windows build execution path once the Linux-side adaptation is
  ready; a diagnostic cross-build does not replace the documented compiler test.
- Choose the final Switch storage location after checking free space and
  filesystem compatibility.

