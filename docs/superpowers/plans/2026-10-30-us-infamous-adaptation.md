# US inFAMOUS Disc Adaptation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a fail-closed local fork of inFAMOUSreborn that accepts the user's exact US `BCUS98119` dump, rebases the existing EU port patches onto its generated PPU/SPU code, and produces a Switch-authoring-ready Windows build for testing through Autorun.

**Architecture:** Keep the upstream installer and generated-code model. Add a pure patch-rebase layer that maps each EU lock entry onto a fresh US generated tree, reuse a patch only when its full fixed context applies, and promote a new US lock only after every entry succeeds. The installer remains the composition point for game ingestion, pipeline execution, patching, and supported compilation.

**Tech Stack:** Rust 2021 installer, Python 3.10+ ps3recomp tools, PS3 PPU/SPU static recompilation, RPCS3 retail-SELF decryption, CMake/C++20, Windows clang-cl and Direct3D 12, Nintendo Switch Atmosphère/Autorun.

**Spec:** `docs/superpowers/specs/2026-10-30-us-disc-adaptation-design.md`

## Global Constraints

- Source game: legally owned `inFamous (USA) (En,Fr,Es) (v02.00)`, title ID `BCUS98119`.
- Source ISO: `/run/media/user/Tardis/ROMs/ps3/inFamous (USA) (En,Fr,Es) (v02.00).iso`; treat it as immutable.
- Working repository: `/run/media/user/Tardis/Projects/inFAMOUSreborn-us`.
- Never use `--force` to turn a US mismatched install into a supported install.
- Never commit the ISO, extracted game, `EBOOT.BIN`, `EBOOT.elf`, generated PPU/SPU code, saves, shader caches, screenshots, or runtime logs.
- Do not use fuzzy patch matching. A patch either applies at every recorded context line or the rebase fails.
- Optional trainer, language, and keyboard features are not acceptance gates unless their US probes pass.
- PC build, PC runtime, Autorun runtime, and physical Switch gameplay are separate evidence gates.
- inFamous 2 is out of scope for this plan.

## Review Focus

- A US lift changes line context around an EU patch: abort without touching the promoted lock or patches.
- A US `PARAM.SFO` passes by title ID but the decrypted executable is the wrong revision: reject once its exact SHA is pinned.
- The US PPU layout moves a hardcoded global, TOC, clock, or language address: fail closed or disable only that optional feature.
- Generated private game code enters Git or the published patch set: stop and remove it before committing.
- The PC build works while Autorun fails on memory, x86-64 translation, graphics, audio, or timing: preserve the distinct blocker and do not call the Switch result playable.

---

### Task 1: Add a strict patch-rebase engine

**Files:**
- Create: `installer/src/rebase.rs`
- Modify: `installer/src/lib.rs`
- Test: inline `#[cfg(test)] mod tests` in `installer/src/rebase.rs`

**Interfaces:**
- Consumes: `infamous_installer::lock::{Entry, Lock}`, `infamous_installer::manifest::Manifest`, and `infamous_installer::diff::apply_patch(old: &[u8], patch: &[u8]) -> Result<Vec<u8>, String>`.
- Produces: `pub struct RebaseReport { pub lock: Lock, pub unchanged: usize, pub patched: usize, pub created: usize, pub removed: usize }`.
- Produces: `pub fn rebase_lock(manifest: &Manifest, fresh: &Path, installer_dir: &Path, source: &Lock, eboot_sha: &str) -> Result<RebaseReport>`.

- [ ] **Step 1: Write the failing rebase tests**

Add tests with these exact names and assertions:

```rust
#[test]
fn unchanged_entry_uses_us_base_hash() {
    let r = run_case("generated/a.txt", "eu-a\n", None, "us-a\n");
    assert_eq!(r.unchanged, 1);
    assert_eq!(r.lock.files[0].base, r.lock.files[0].fin);
}

#[test]
fn applies_existing_patch_to_new_us_base() {
    let r = run_case("generated/b.txt", "us: x\n", Some(("eu: x\n", "eu: y\n")), "us: x\n");
    assert_eq!(r.patched, 1);
    assert_eq!(r.lock.files[0].fin.as_deref(), Some(hash("us: y\n").as_str()));
}

#[test]
fn preserves_a_patch_created_file() {
    let r = run_case("generated/c.txt", "", Some((None, "created\n")), "");
    assert_eq!(r.created, 1);
    assert_eq!(r.lock.files[0].base, None);
    assert_eq!(r.lock.files[0].fin.as_deref(), Some(hash("created\n").as_str()));
}

#[test]
fn preserves_a_deleted_generated_file() {
    let r = run_case("generated/d.txt", "us-d\n", Some(("eu-d\n", None)), "us-d\n");
    assert_eq!(r.removed, 1);
    assert_eq!(r.lock.files[0].fin, None);
}

#[test]
fn context_mismatch_names_the_file_and_does_not_return_a_lock() {
    let e = run_bad_case("generated/e.txt", "us-context\n", ("eu-context\n", "eu-result\n"));
    assert!(e.contains("generated/e.txt"));
    assert!(e.contains("context mismatch"));
}

#[test]
fn unexpected_generated_file_aborts_the_rebase() {
    let e = run_unexpected_file_case("generated/extra.cpp");
    assert!(e.contains("unexpected generated file generated/extra.cpp"));
}
```

Each test creates an isolated directory under `std::env::temp_dir()`, writes the source `generated.lock`, a fresh base file, and any patch generated with `crate::diff::make_patch`.

- [ ] **Step 2: Run the tests to verify the module does not exist**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features rebase::tests -v`

Expected: build failure because `installer/src/rebase.rs` and `rebase_entries` do not exist.

- [ ] **Step 3: Implement `rebase_lock`**

For each source lock entry:

1. Resolve `fresh/<entry.path>` and `installer_dir/patches/<entry.path>.patch`.
2. When `entry.fin == None`, produce a new entry with the US base hash if present and `fin: None`.
3. When the source base and final hashes match and no patch exists, use the US file hash for both base and final.
4. When `entry.base == None`, apply the patch to an empty byte slice and use `base: None`.
5. Otherwise, read the US base, apply the patch strictly, and hash the result for `fin`.
6. Compare `util::select_files(fresh, &manifest.gen_include, &manifest.gen_exclude, pipeline::SKIP_DIRS)` with the source lock paths. Exclude `manifest.is_loose(path)` files; return an error naming every other unexpected generated path.
7. Return an error containing the relative path whenever the patch is missing, the context mismatches, or the file count cannot be reconciled.

Do not write the lock or mutate the fresh tree in this function.

- [ ] **Step 4: Run the focused tests**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features rebase::tests -v`

Expected: all rebase tests pass; the mismatch test reports `context mismatch`.

- [ ] **Step 5: Run all dependency-free installer tests**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features -v`

Expected: all installer library, diff, lock, manifest, ISO, and rebase tests pass.

- [ ] **Step 6: Commit**

```bash
git add installer/src/lib.rs installer/src/rebase.rs
git commit -m "feat: add strict patch rebase engine"
```

---

### Task 2: Add the atomic `rebase-patches` command

**Files:**
- Modify: `installer/src/main.rs`
- Modify: `installer/src/lib.rs` only if a module export was not completed in Task 1
- Test: inline `#[cfg(test)] mod tests` in `installer/src/main.rs`

**Interfaces:**
- Consumes: `rebase::rebase_lock(manifest: &Manifest, fresh: &Path, installer_dir: &Path, source: &Lock, eboot_sha: &str) -> Result<RebaseReport>`.
- Consumes: `pipeline::export_tree`, `pipeline::link_game`, `pipeline::run_steps`, `pipeline::apply_lock`, and `pipeline::lock_satisfied`.
- Produces: CLI command `infamous-installer rebase-patches --game DIR --rpcs3 EXE`.
- Produces candidate lock at `installer/generated.lock.us-new` and report at `.installer/rebase/report.txt`; neither is promoted on failure.

- [ ] **Step 1: Write failing command-parser tests**

Refactor input parsing into:

```rust
fn parse_args_from<I>(args: I) -> Result<Opts>
where
    I: IntoIterator<Item = String>,
```

Add:

```rust
#[test]
fn recognizes_rebase_patches_command() {
    let o = parse_args_from(vec!["rebase-patches".into()]).unwrap();
    assert_eq!(o.cmd, "rebase-patches");
}

#[test]
fn rebase_patches_does_not_silently_default_a_game() {
    let o = parse_args_from(vec!["rebase-patches".into()]).unwrap();
    assert!(o.game.is_none());
}
```

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features main::tests -v`

Expected: failure because `rebase-patches` is not recognized.

- [ ] **Step 2: Add command parsing and usage text**

Add `rebase-patches` to the command allowlist and help text. Do not add it to update/package dispatch paths.

- [ ] **Step 3: Implement the staged command**

`cmd_rebase_patches` performs:

1. Locate the game and verify `TITLE_ID == BCUS98119`.
2. Decrypt/classify `EBOOT.elf` and compute its SHA-256.
3. Export a fresh clean tree and run every manifest pipeline step.
4. Load the current `installer/generated.lock` and call `rebase_lock`.
5. Save the returned lock only to `installer/generated.lock.us-new`.
6. Apply that candidate lock to the fresh tree and require `pipeline::lock_satisfied(&fresh, &candidate)`.
7. Write a plain-text report with entry counts and every path hash.
8. Only after all checks pass, atomically rename the candidate to `installer/generated.lock`.

On any error, leave the current committed lock and patch directory unchanged.

- [ ] **Step 4: Run parser and dependency-free tests**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features -v`

Expected: all tests pass and `rebase-patches` is listed by `cargo run --no-default-features -- --help`.

- [ ] **Step 5: Commit**

```bash
git add installer/src/main.rs
git commit -m "feat: add staged US patch rebase command"
```

---

### Task 3: Capture and pin the exact US executable identity

**Files:**
- Modify: `installer/infamous.toml`
- Modify: `installer/src/game.rs`
- Create: `docs/superpowers/evidence/2026-10-30-us-infamous-identity.md`
- Runtime output, not committed: `.installer/us/game/`, `.installer/us/rpcs3/`, evidence JSON/text

- [ ] **Step 1: Add a US SFO parser test before changing the fixture**

Extend `installer/src/game.rs` tests:

```rust
#[test]
fn parses_the_us_param_sfo_identity() {
    let d = make_sfo(&[("TITLE", "inFamous"), ("TITLE_ID", "BCUS98119"), ("APP_VER", "02.00")]);
    let m = parse_sfo(&d).unwrap();
    assert_eq!(m["TITLE_ID"], "BCUS98119");
    assert_eq!(m["APP_VER"], "02.00");
}
```

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features game::tests::parses_the_us_param_sfo_identity -v`

Expected: FAIL until the fixture includes and asserts `APP_VER`.

- [ ] **Step 2: Run the test and update only the parser fixture/helper**

Run after the minimal test update:

`cargo test --manifest-path installer/Cargo.toml --no-default-features game::tests::parses_the_us_param_sfo_identity -v`

Expected: PASS with `TITLE_ID=BCUS98119` and `APP_VER=02.00`.

- [ ] **Step 3: Download and pin RPCS3**

Use the current official Linux AppImage from `https://rpcs3.net/latest-appimage`. Record:

- downloaded filename;
- SHA-256;
- RPCS3 version/build identifier printed by the AppImage;
- source URL and retrieval date.

Store the executable only under `.installer/us/rpcs3/`, not under a tracked `tools/` directory.

- [ ] **Step 4: Extract and decrypt the user's ISO**

Run:

```bash
cargo run --manifest-path installer/Cargo.toml --no-default-features -- \
  extract-iso \
  --iso "/run/media/user/Tardis/ROMs/ps3/inFamous (USA) (En,Fr,Es) (v02.00).iso" \
  --out ".installer/us/game"
```

Then run the pinned RPCS3 with `--decrypt` against:

`.installer/us/game/PS3_GAME/USRDIR/EBOOT.BIN`

- [ ] **Step 5: Verify and record the executable**

Run:

```bash
file ".installer/us/game/PS3_GAME/USRDIR/EBOOT.elf"
sha256sum ".installer/us/game/PS3_GAME/USRDIR/EBOOT.elf"
```

Expected: a big-endian 64-bit PowerPC ELF accepted by `game::classify(Path) == Kind::Elf`.

Write the exact SHA and ELF metadata into
`docs/superpowers/evidence/2026-10-30-us-infamous-identity.md`.

- [ ] **Step 6: Pin the measured values in the manifest**

Change `installer/infamous.toml`:

- `title_ids = ["BCUS98119"]`
- `eboot_sha256 = ["<the exact lowercase SHA-256 measured in Step 5>"]`
- `link_name = "Infamous (USA) (En,Fr,Es) (v02.00)"`

Do not retain the EU hash as an accepted alternative.

- [ ] **Step 7: Verify title/hash behavior**

Run the dependency-free tests, then run `infamous-installer doctor --game .installer/us/game`.

Expected: the US game passes title and decrypted-ELF classification; an EU title ID or different ELF hash fails without `--force`.

- [ ] **Step 8: Commit**

```bash
git add installer/infamous.toml installer/src/game.rs \
  docs/superpowers/evidence/2026-10-30-us-infamous-identity.md
git commit -m "feat: pin US inFamous executable identity"
```

---

### Task 4: Rename the fork and remove misleading EU identity

**Files:**
- Modify: `.recomp.json`
- Modify: `README.md`
- Modify: `installer/Cargo.toml`
- Modify: `installer/README.md`
- Modify: `installer/src/main.rs`
- Modify: `installer/src/bin/infamous-setup.rs`
- Modify: `infamous-recomp/CMakeLists.txt`
- Modify: `infamous-recomp/main.cpp`
- Modify: `infamous-recomp/README.md`
- Modify: `infamous-recomp/infamous_lang.h`
- Modify: `infamous-recomp/infamous_clock_probe.h`
- Modify: `infamous-recomp/trainer.cpp`
- Test: `installer/src/main.rs` and `installer/src/manifest.rs` inline tests

- [ ] **Step 1: Add metadata consistency tests**

Add a manifest test asserting:

```rust
assert_eq!(m.title_ids, vec!["BCUS98119"]);
assert_eq!(m.eboot_sha256.len(), 1);
assert_eq!(m.link_name.as_deref(), Some("Infamous (USA) (En,Fr,Es) (v02.00)"));
```

Add a command/help test asserting no supported-path help text says `BCES00220`.

- [ ] **Step 2: Run the metadata tests to verify failure**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features -v`

Expected: failure on remaining EU metadata/user-facing strings.

- [ ] **Step 3: Update fork identity and user-facing text**

Replace EU release claims with US claims in the listed files. Setup fallbacks must use:

```rust
title_ids: vec!["BCUS98119".into()],
link_name: "Infamous (USA) (En,Fr,Es) (v02.00)".into(),
```

Keep comments about the code's EU provenance where they document reverse engineering, but clearly state that this fork targets the US executable and that address-specific behavior is pending Task 6 verification.

- [ ] **Step 4: Run metadata tests and a repository text audit**

Run:

```bash
cargo test --manifest-path installer/Cargo.toml --no-default-features -v
git grep -n -I -e 'BCES00220' -e 'European release' -e 'Infamous (Europe)'
```

Expected: tests pass. Remaining matches must be historical provenance or explicit Task 6 address-audit notes, not supported-install claims.

- [ ] **Step 5: Commit**

```bash
git add .recomp.json README.md installer/Cargo.toml installer/README.md \
  installer/src/main.rs installer/src/bin/infamous-setup.rs \
  installer/src/manifest.rs infamous-recomp/CMakeLists.txt \
  infamous-recomp/main.cpp infamous-recomp/README.md \
  infamous-recomp/infamous_lang.h infamous-recomp/infamous_clock_probe.h \
  infamous-recomp/trainer.cpp
git commit -m "refactor: identify the fork as the US release"
```

---

### Task 5: Generate the US lift and require complete patch coverage

**Files:**
- Modify generated output, not committed: `infamous-recomp/recompiled/`, `infamous-recomp/spu/`, `infamous-recomp/spu-images/`, asset outputs
- Modify after success: `installer/generated.lock`
- Reuse unchanged when possible: `installer/patches/**`
- Create evidence: `docs/superpowers/evidence/2026-10-30-us-patch-rebase.md`

- [ ] **Step 1: Run the staged rebase**

Run:

```bash
cargo run --manifest-path installer/Cargo.toml --no-default-features -- \
  rebase-patches \
  --game ".installer/us/game" \
  --rpcs3 ".installer/us/rpcs3/rpcs3.AppImage"
```

Expected success: every generated entry is reconciled, `installer/generated.lock` changes, and
`.installer/rebase/report.txt` records nonzero counts with no mismatch.

Expected failure: the committed lock and patch directory remain unchanged; the command names every failing file and context line.

- [ ] **Step 2: Validate lock satisfaction**

Run:

```bash
cargo run --manifest-path installer/Cargo.toml --no-default-features -- \
  install \
  --game ".installer/us/game" \
  --no-build \
  -y
```

Expected: `All <N> generated files already match the port's patch set.` and no regeneration of unpublished EU files.

- [ ] **Step 3: Verify patch and game-data hygiene**

Run:

```bash
git status --short
git diff --stat
git diff -- installer/generated.lock
git grep -n -I -e 'BCUS98119' -e 'BCES00220' -- installer/generated.lock
```

Expected: only the US lock/evidence changes are tracked; no generated `.cpp`, `.c`, EBOOT, ISO, save, or log files appear.

- [ ] **Step 4: Record the rebase result**

Write to the evidence file:

- exact US EBOOT SHA;
- generated file count;
- patched, created, removed, and unchanged counts;
- patch directory aggregate SHA-256;
- every mismatch, if any, quoted with file and line.

If any mismatch exists, stop this plan and attach that evidence to the next scoped design; do not start semantic reconciliation under this plan.

- [ ] **Step 5: Commit only on complete coverage**

```bash
git add installer/generated.lock \
  docs/superpowers/evidence/2026-10-30-us-patch-rebase.md
git commit -m "feat: add complete US generated-file lock"
```

---

### Task 6: Audit title-dependent runtime addresses

**Files:**
- Inspect: `infamous-recomp/infamous_clock_probe.h`
- Inspect: `infamous-recomp/infamous_lang.h`
- Inspect: `infamous-recomp/stubs.cpp`
- Inspect generated: `infamous-recomp/recompiled/ppu_recomp.h`
- Inspect generated: `.installer/work/ppu/EBOOT.functions.json`
- Inspect generated: `.installer/work/ppu/EBOOT.imports.json`
- Modify only files whose US probe differs

- [ ] **Step 1: Record each hardcoded address and its expected invariant**

Build a table containing at least:

- `LISTS = 0x00720868` and seven shipped-language counts from `infamous_lang.h`;
- `PM_TOC = 0x0078BFC0` from `stubs.cpp`;
- `0x7840B8`, `0x76959C`, `0x7615A4`, and `0x7BC650` from `infamous_clock_probe.h`;
- any TOC or function addresses in the generated loader manifest used by those probes.

- [ ] **Step 2: Check each invariant against the US lift**

Run the existing probes with `CELL_PROBE=1`, `PS3_VERBOSE=1`, and `SIM_CLOCK` diagnostics enabled where applicable.

Expected for each non-critical feature: either the expected values match or the feature logs a clear mismatch and disables itself.

Any crash, silent wrong value, or disabled core boot path is a hard failure.

- [ ] **Step 3: Add a regression test for any changed pure helper**

For each changed testable helper, add a focused inline test or Python test that pins the US value and the mismatch behavior before modifying it.

Run the focused test and verify it fails, implement the minimum mismatch handling, then rerun it until it passes.

- [ ] **Step 4: Run the full dependency-free test suite**

Run: `cargo test --manifest-path installer/Cargo.toml --no-default-features -v`

Expected: all tests pass.

- [ ] **Step 5: Commit if runtime source changed**

```bash
git add infamous-recomp docs/superpowers/evidence
git commit -m "fix: validate US title-specific runtime addresses"
```

If no source changes are needed, record the evidence and do not create an empty commit.

---

### Task 7: Compile and validate the US PC build

**Files:**
- Generated, not committed: `infamous-recomp/build-install/`, `infamous-recomp/build-fast/`, build logs
- Create evidence: `docs/superpowers/evidence/2026-10-30-us-pc-build.md`

- [ ] **Step 1: Confirm the supported Windows build toolchain**

Check:

```bat
cmake --version
ninja --version
clang-cl --version
python --version
```

Expected: CMake 3.20+, Ninja, clang-cl from Visual Studio 2022 Build Tools, and Python 3.10+.

If the Linux session cannot invoke that toolchain, leave the generated Tardis tree intact, record the missing toolchain, and perform this task after booting Windows. Do not substitute an unverified cross-build as the playable build.

- [ ] **Step 2: Build through the project installer**

Run from the Tardis checkout with the supported Windows environment:

```bat
infamous-installer.exe install --game "<US game folder>" --build-jobs 4 -y
```

Expected: generation is skipped or confirms the US lock, and `inFamousRecomp.exe` is produced.

- [ ] **Step 3: Run the executable on Windows**

Run `inFamousRecomp.exe` with logging enabled. Capture:

- startup and loader status;
- language-list probe result;
- clock/display probe result;
- SPU registration and workload status;
- graphics device/shader status;
- first rendered menu or gameplay frame;
- exit code and crash log.

Expected: no US-specific address mismatch fatal; the game reaches a repeatable rendered state.

- [ ] **Step 4: Record the PC result separately**

Write executable SHA-256, build timestamp, compiler versions, test duration, reached state, and every blocker to the evidence file.

Do not describe a successful PC launch as Switch acceptance.

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/evidence/2026-10-30-us-pc-build.md
git commit -m "test: record US PC build evidence"
```

---

### Task 8: Stage through Autorun and run on the Switch

**Files:**
- Not committed: Autorun installation, game folder, logs, screenshots
- Create evidence: `docs/superpowers/evidence/2026-10-30-us-switch-run.md`

- [ ] **Step 1: Verify the target SD and Autorun state**

Before copying:

```bash
findmnt -T <mounted-switch-sd>
lsblk -o NAME,SIZE,FSTYPE,LABEL,UUID,MOUNTPOINTS
```

Record the exact mounted device, free space, Autorun runtime build, and whether the required 39-bit AMD64 forwarder is installed.

Expected: enough free space for the executable, Windows DLLs, game data, and logs.

- [ ] **Step 2: Check available Autorun storage and features**

Confirm the installed Autorun build supports:

- AMD64 execution;
- VKD3D-Proton / Direct3D 12;
- the 39-bit application forwarder;
- the Switch controller/XInput path.

If a required feature is absent, update Autorun from its official release and verify the runtime build in `autorun_runtime.log` before adding the game.

- [ ] **Step 3: Stage the game without altering source data**

Copy only the built install folder and required game data to the selected SD or Autorun-supported USB volume. Keep the original ISO and PC build intact.

Back up any existing per-game Autorun config before replacing it.

- [ ] **Step 4: Run and capture logs**

Launch the game through Autorun. Preserve:

- `switch/wine/logs/autorun_runtime.log`;
- the per-game log;
- `stdout.txt`, `stderr.txt`, and `stdin.txt`;
- the first screen or rendered state;
- free-memory or code-cache progress lines;
- whether input and audio work.

Expected first acceptance result: the game reaches its first controllable gameplay state at any frame rate.

If it does not, record the first reproducible blocker instead of expanding into unrelated porting work.

- [ ] **Step 5: Record the hardware result**

Write the Switch model, Atmosphère/Autorun versions, runtime log build line, reached state, observed frame rate, memory/code-cache pressure, controls, audio, and exact blocker to the evidence file.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/evidence/2026-10-30-us-switch-run.md
git commit -m "test: record US inFamous Switch run"
```

---

## Final Verification

- [ ] `cargo test --manifest-path installer/Cargo.toml --no-default-features -v` passes.
- [ ] `git status --short` shows no game, generated source, build output, save, screenshot, or log artifacts.
- [ ] `installer/generated.lock` contains the measured US EBOOT SHA and no EU hash alternative.
- [ ] Every required patch in the US lock applied without fuzzy matching.
- [ ] A documented Windows clang-cl build exists and its executable hash is recorded.
- [ ] The Switch evidence distinguishes PC success from Autorun/hardware success.
- [ ] The final result states the first playable state or the first reproducible blocker.

