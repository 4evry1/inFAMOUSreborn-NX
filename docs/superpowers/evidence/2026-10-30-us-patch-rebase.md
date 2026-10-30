# US patch rebase — Task 5 evidence

Date: 2026-10-30

Status: COMPLETE — the US lift, SPU relocation, and full PPU reconciliation produce a complete US
generated-file lock that `install --no-build` reproduces on a fresh regeneration.

## Identity and lock

- US EBOOT.elf SHA-256: `4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4`
- Promoted lock: `eboot 4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4`, 156 entries
- Lock SHA-256: `b64745f2f9a543f6c575f78ca0ee2b6ae54664b3f7cbf6b46d13cf05ea981a40`
- Branch / HEAD base: `us-version-adaptation` @ `b3c548d`

## Rebase result

`rebase-patches` completed: `42 unchanged, 93 patched, 21 created, 0 removed`. The candidate lock
applied cleanly to the fresh tree (`84 already right, 51 patched, 21 created, 0 removed`) and
`lock_satisfied` passed before promotion.

## Lock satisfaction (install --no-build)

A fresh `install --no-build -y` regenerated the US lift and applied the lock (`84 already right, 51
patched, 21 created`); a second run reported:

```text
All 156 generated files already match the port's patch set.
```

## SPU offset relocation

The eight embedded SPU programs are byte-identical between EU and US at a −0x500 file-offset shift.
Derived 8 EU→US aliases by `spu_000N` index plus exact image sha256 equality, verified all 32
relocated entries (8 images + 24 lifted sources) byte-identical modulo the group name, and
relocated the committed patch set.

| index | EU group | US group |
| --- | --- | --- |
| 0 | `spu_0000_at_005F6500` | `spu_0000_at_005F6000` |
| 1 | `spu_0001_at_00674380` | `spu_0001_at_00673E80` |
| 2 | `spu_0002_at_0068FE00` | `spu_0002_at_0068F900` |
| 3 | `spu_0003_at_006A3800` | `spu_0003_at_006A3300` |
| 4 | `spu_0004_at_006BF480` | `spu_0004_at_006BEF80` |
| 5 | `spu_0005_at_006D7700` | `spu_0005_at_006D7200` |
| 6 | `spu_0006_at_006F1E00` | `spu_0006_at_006F1900` |
| 7 | `spu_0007_at_00700600` | `spu_0007_at_00700100` |

## PPU reconciliation

`ppu_compat` classified the EU PPU patches and reconciled all six US PPU files:

- 44,546 `memcpy(vm_base + (uint32_t)ea, &ctx->vr[N], 16)` → `ppu_stvx16(...)` global mappings;
- 25 `memset(vm_base + (uint32_t)ea, 0, 128)` → `ppu_dcbz128((uint32_t)ea)` mappings;
- 361 comment-only dash pairs folded (UTF-8 em/en dash, U+FFFD, Windows-1252 `0x97`), no-ops;
- 43 residual hunks resolved by tiers: 11 byte-exact, 12 token-normalized, 8 hex-normalized, 12
  order-consistent interpolated; every new-side `func_`/`loc_` token covered (curated alias
  `loc_001850EC → loc_001855EC` for the movie-pool retry);
- the two EU single-register stores `gpr[20]→vr[3]` and `gpr[23]→vr[5]` are absent from the US lift
  with no same-register raw store, recorded as `not applicable` (no silent coverage).

No U+FFFD is introduced. US-specific PPU patch files were generated with the existing diff writer
from the raw US base to the reconciled final source, so `install` reproduces the final bytes.

## Minimum config fixes (EU-specific offsets)

- `infamous-recomp/tools/extract_ui_font.py`: EU dump path → US link name (committed c3f8404).
- `installer/infamous.toml` `pm-carve`: `0x68CD80` → `0x68C880` (pm_wwsjob shifted −0x500;
  carving at `0x68C880` reproduces the EU base `2fedf78b…` exactly).
- `ps3recomp/tools/plug_lift.py` `pmA2C`: `0x68C380` → `0x68BE80` (pm_wwsjob+0x2C),
  `0x68BE80 + 0xA2C = 0x68C8AC`.

## Hashes

- Patch directory aggregate SHA-256 (93 files): `1e9e5ae6344118745acdf929514191fc54b6f77ca3d403e9f520e34da5723b05`
- Lock SHA-256: `b64745f2f9a543f6c575f78ca0ee2b6ae54664b3f7cbf6b46d13cf05ea981a40`

## Tests

63 tests pass (54 lib + 9 bin), covering mechanical exactness, mechanical not-applicable vs
partial-match rejection, all four residual tiers, new-side token coverage, the curated alias,
CP-1252 folding, conflicting mappings, and CRLF→LF header application.
