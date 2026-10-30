# US title-dependent runtime address audit — Task 6 evidence

Date: 2026-10-30

Branch: `us-version-adaptation`. Audit base (before Task 6 changes): `b3ff1d9`.
Task 6 commits: round 1 `c8b2807` (constant corrections), round 2 (helper + test gate;
final hash in `task-6-report.md`).

## Identity

- US EBOOT.elf: `.installer/us/game/PS3_GAME/USRDIR/EBOOT.elf`
- SHA-256: `4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4`
  (matches the pin in `installer/infamous.toml`; Task 3 identity evidence).

## VA → file-offset mapping (PT_LOAD)

The decrypted ELF has two load-bearing `PT_LOAD` segments:

| segment | p_offset | p_vaddr | p_filesz | flags |
| --- | --- | --- | --- | --- |
| 0 | `0x00000000` | `0x00010000` | `0x00708748` | R E |
| 1 | `0x00710000` | `0x00720000` | `0x00098280` | RW |

For every address audited here, the file offset is `VA - 0x10000` (segment 0:
`offset = VA - 0x10000`; segment 1: `0x710000 + (VA - 0x720000) = VA - 0x10000`).
`0x7BC650` is inside `.bss` (`NOBITS`, section 38, `[0x7b8280, 0x9eed60)`), so it has no
file bytes: it is zero-filled at load.

## Hardcoded addresses and invariants

| constant | file | expected invariant | US ELF read | verdict |
| --- | --- | --- | --- | --- |
| `LISTS = 0x00720868` | `infamous_lang.h` | 7 x `{u32 count, u32 list}` with counts `{3,5,1,3,3,8,9}` | counts match; lists `0x565728/34/48/4C/58/64/84` | MATCH |
| `PM_TOC = 0x0078BFC0` | `stubs.cpp` | pause-menu OPD TOC field | every OPD `toc = 0x0078BF78` | CHANGED → `0x0078BF78` |
| `base = 0x7BC650` | `infamous_clock_probe.h` | clock struct in `.bss` | `0x7BC650` in `[0x7b8280, 0x9eed60)` | MATCH |
| `word(0x7840B8) == 0x76959C` | `infamous_clock_probe.h` | TOC entry at `TOC - 0x7F08` | `word(0x784070) == 0x769554` | CHANGED |
| `word(0x7615A4) == 0x7BC650` | `infamous_clock_probe.h` | pointer slot at `0x76959C - 0x7FF8` | `word(0x76155C) == 0x7BC650` | CHANGED |

### Language-list table contents (read from the US ELF)

| release | count | list | languages |
| --- | --- | --- | --- |
| 0 (US, BCUS98119) | 3 | `0x00565728` | 1, 2, 3 |
| 1 (BCES00220) | 5 | `0x00565734` | 2, 5, 4, 3, 6 |
| 2 (BCES00591) | 1 | `0x00565748` | 16 |
| 3 (BCES00609) | 3 | `0x0056574C` | 17, 7, 8 |
| 4 (BCAS20074) | 3 | `0x00565758` | 1, 9, 11 |
| 5 (BCET00010) | 8 | `0x00565764` | 17, 2, 5, 4, 3, 6, 7, 8 |
| 6 (BCET00220) | 9 | `0x00565784` | 17, 2, 5, 4, 3, 6, 16, 7, 8 |

`LISTS` and `SHIPPED_COUNT` are identical in the US and EU builds; no change.

### Clock probe chain (read from the US ELF)

- US module TOC = `0x0078BF78` (all 9360 OPDs in `[0x771af8, 0x783f78)` have
  `toc = 0x0078BF78`; `EBOOT.loader.json` reports `module_toc = "0x78bf78"`).
- `word(0x784070) = 0x00769554` (`TOC - 0x7F08`, EU analogue `0x7840B8 -> 0x76959C`).
- `word(0x76155C) = 0x007BC650` (`0x769554 - 0x7FF8`, EU analogue `0x7615A4 -> 0x7BC650`).
- `base = 0x7BC650` is unchanged (the `.bss` clock object sits at the same address).

The EU→US shift for this chain is a uniform `-0x48` of the TOC base and the `.data`
pointer values; the clock base itself does not move.

## Changes

- `infamous-recomp/stubs.cpp`: `PM_TOC = 0x0078BFC0u` → `0x0078BF78u`.
- `infamous-recomp/infamous_clock_probe.h`: extracted a pure, `constexpr`
  `clock_layout_matches(toc_entry_word, clock_ptr_word)` predicate; the `init()` guard
  fails closed via `if (!clock_layout_matches(word(0x784070), word(0x76155C)))`.
- `installer/src/title_probe.rs` (new) + `installer/src/lib.rs`: a `cargo test` gate
  that mirrors the predicate (truth table: match + both mismatch cases) and pins the
  exact C++ source so an inverted comparison, a removed fail-closed `return;`, or a
  stale EU `PM_TOC` literal fails the standard gate. Runs without the ELF.
- `infamous-recomp/tools/verify_us_title_addrs.py`: now ELF-ground-truth only (local
  acceptance test when the dump is present); source pinning moved to the Rust gate.

The round-2 helper/gate was written test-first: the three source-pinning tests failed
against the round-1 C++ (no helper, no `!clock_layout_matches(...)` guard, and a stale
`0x0078BFC0` reference in the `PM_TOC` comment), then passed after the helper and the
comment were corrected.

## Fail-closed status

- Language probe (`infamous_lang.h`): unchanged; the table matches, so no fallback.
- Clock probe (`infamous_clock_probe.h`): the `init()` guard still returns early on any
  layout mismatch, disabling only the optional SIM-CLOCK/FPS-cap features. The core
  boot/runtime path is untouched.
- Pause menu (`stubs.cpp`): `pm_match` still fails closed if the script anchor or the
  word-level expectations differ from retail bytes; nothing is written on a mismatch.

## Remaining Windows-runtime probes (cannot be executed in this Linux session)

- The once-per-frame clock-update function (EU `func_00078AA0`) is referenced only in
  comments; its US counterpart is not yet pinned (candidate `func_00078A70`, start
  `0x78a70` in `EBOOT.functions.json`). The runtime check depends only on the two data
  words above, which are verified. Confirm `INFAMOUS_FPS_CAP` / `INFAMOUS_SIM_TRACE=1`
  (F8 dump) on the Windows build.
- `infamous-recomp/trainer.cpp` `kTitleToc = 0x0078BFC0u` is the same guest TOC and is
  likewise `-0x48` shifted in US (`0x0078BF78`), but it is outside Task 6's file scope
  and its six TOC-relative offsets (`-0x7C0C`, `-0x79E4`, ...) must be re-verified
  against the US TOC before that constant is moved.
- `stubs.cpp` pause-menu anchor words (`0x0077C170`, script address `0x4222AA84`) are
  hand-patch anchors, not the title-dependent runtime probes in scope for Task 6.

## Test evidence

- `cargo test --manifest-path installer/Cargo.toml --no-default-features` → 76 passed
  (65 lib — including the 6 `title_probe` tests — + 11 bin), 0 failed.
- `python3 infamous-recomp/tools/verify_us_title_addrs.py` → PASS (exit 0; ELF ground
  truth only, requires the local dump).
