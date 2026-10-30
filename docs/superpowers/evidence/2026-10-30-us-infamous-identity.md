# US inFamous executable identity — 2026-10-30

Branch `us-version-adaptation` @ `a9b3618`. Task 3: capture and pin the exact US executable identity.

## Source disc

- Image: `inFamous (USA) (En,Fr,Es) (v02.00).iso`
- Path: `/run/media/user/Tardis/ROMs/ps3/inFamous (USA) (En,Fr,Es) (v02.00).iso`
- Size: 7,654,277,120 bytes
- SHA-256: `482f2246e1fdf19059dd4ea57bc4a7570f11d27ee60b78a64797a88f515d67b5`
- Layout: plain (decrypted) ISO 9660 rip; `extract-iso` found no encrypted disc regions, so no disc
  key was needed. The only encrypted artefact is the retail SELF `EBOOT.BIN`, decrypted by RPCS3.

## PARAM.SFO (`PS3_GAME/PARAM.SFO`, extracted)

- TITLE: `inFamous`
- TITLE_ID: `BCUS98119`
- APP_VER: `01.00` (the `(v02.00)` suffix in the image name is the disc-revision label, not APP_VER)
- PS3_SYSTEM_VER: `04.2100`

## RPCS3 (SELF decrypter)

- Source URL: `https://rpcs3.net/latest-appimage`. That short URL is behind a Cloudflare browser
  challenge from this host, so the artifact was fetched directly from the canonical official
  GitHub release asset the short URL serves:
- Asset URL: `https://github.com/RPCS3/rpcs3-binaries-linux/releases/download/build-9508444d31338e89a4f4cee1d0cee1800c96d2c1/rpcs3-v0.0.43-20511-9508444d_linux64.AppImage`
- Filename: `rpcs3-v0.0.43-20511-9508444d_linux64.AppImage`
- Size: 83,223,350 bytes
- SHA-256: `b286c5fc6056355d2e991be03f540448c0a82d104a8a6c398367839a4c707f67`
- Build reported by `--version`: `RPCS3 0.0.43-20511-9508444d Alpha`
- Retrieval date: 2026-10-30 (UTC)
- Stored only under gitignored `.installer/us/rpcs3/`.

## Decryption

`RPCS3 --headless --no-gui --decrypt PS3_GAME/USRDIR/EBOOT.BIN` wrote `EBOOT.elf` next to the
encrypted `EBOOT.BIN` (retail SELF, 8,041,848 bytes, `game::classify` → `Kind::RetailSelf`).

## Decrypted executable (`PS3_GAME/USRDIR/EBOOT.elf`)

- `file`: `ELF 64-bit MSB executable, 64-bit PowerPC or cisco 7500, Unspecified or Power ELF V1
  ABI, version 1 (Cell LV2), statically linked, stripped`
- Size: 8,039,416 bytes
- SHA-256: `4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4`
- Classification: `game::classify` → `Kind::Elf`. ELF header bytes `7f 45 4c 46 02 02 01` with
  `e_machine` `0x0015` (= 21, `EM_PPC64`), matching the classifier's big-endian 64-bit PPC64 check.

This SHA is the exact value pinned in `installer/infamous.toml`.
