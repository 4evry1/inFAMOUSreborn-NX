#!/usr/bin/env python3
"""Task 6 regression test: pin the US (BCUS98119) title-dependent runtime addresses.

Dependency-free (Python stdlib only). Verifies, against the decrypted US EBOOT.elf,
the exact invariants the runtime probes depend on, and asserts the in-tree C++
constants still match. Run from anywhere:

    python3 infamous-recomp/tools/verify_us_title_addrs.py [--elf path/to/EBOOT.elf]

Exits non-zero if any invariant is wrong (e.g. a EU constant is reintroduced).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys


REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_ELF = os.path.join(REPO_ROOT, ".installer", "us", "game", "PS3_GAME", "USRDIR", "EBOOT.elf")

# Pinned in installer/infamous.toml (Task 3 identity evidence).
US_EBOOT_SHA256 = "4f2db3fb7c76fbb4f80cbc8445cf0ea1570a664996a7787e690efcc1a39befb4"

# Language-list table (infamous_lang.h). LISTS = 0x00720868, 7 x { u32 count, u32 list }.
LISTS = 0x00720868
SHIPPED_COUNT = [3, 5, 1, 3, 3, 8, 9]
SHIPPED_LIST = [0x00565728, 0x00565734, 0x00565748, 0x0056574C, 0x00565758, 0x00565764, 0x00565784]
SHIPPED_LANGS = [
    [1, 2, 3],
    [2, 5, 4, 3, 6],
    [16],
    [17, 7, 8],
    [1, 9, 11],
    [17, 2, 5, 4, 3, 6, 7, 8],
    [17, 2, 5, 4, 3, 6, 16, 7, 8],
]

# Pause-menu OPD TOC (stubs.cpp) and clock probe (infamous_clock_probe.h).
US_TOC = 0x0078BF78  # every OPD's toc field in the US lift
OPD_BASE = 0x00771AF8
OPD_SIZE = 0x12480
CLOCK_TOC_ENTRY = 0x00784070  # TOC - 0x7F08
CLOCK_TOC_VALUE = 0x00769554
CLOCK_PTR_SLOT = 0x0076155C  # CLOCK_TOC_VALUE - 0x7FF8
CLOCK_BASE = 0x007BC650
BSS_LO = 0x007B8280  # .bss (NOBITS) section 38
BSS_HI = 0x009EED60


def load_segments(data: bytes):
    """Parse ELF64 big-endian PT_LOAD segments (p_type == 1)."""
    e_phoff = struct.unpack_from(">Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from(">H", data, 0x36)[0]
    e_phnum = struct.unpack_from(">H", data, 0x38)[0]
    segs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
            ">IIQQQQQQ", data, off)
        if p_type == 1:
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def read32(data: bytes, segs, va: int):
    for vaddr, offset, filesz in segs:
        if vaddr <= va < vaddr + filesz:
            return struct.unpack_from(">I", data, offset + (va - vaddr))[0]
    return None


def check_elf(elf_path: str) -> list[str]:
    errs: list[str] = []
    if not os.path.exists(elf_path):
        return [f"ELF not found: {elf_path}"]
    with open(elf_path, "rb") as f:
        data = f.read()

    sha = hashlib.sha256(data).hexdigest()
    if sha != US_EBOOT_SHA256:
        errs.append(f"ELF SHA-256 {sha} != pinned {US_EBOOT_SHA256}")

    segs = load_segments(data)
    if not segs:
        return errs + ["no PT_LOAD segments parsed"]

    # Language-list table.
    for r in range(7):
        cnt = read32(data, segs, LISTS + 8 * r)
        lst = read32(data, segs, LISTS + 8 * r + 4)
        if cnt != SHIPPED_COUNT[r]:
            errs.append(f"release {r}: count {cnt} != {SHIPPED_COUNT[r]}")
        if lst != SHIPPED_LIST[r]:
            errs.append(f"release {r}: list 0x{lst:08X} != 0x{SHIPPED_LIST[r]:08X}")
        if lst and cnt is not None:
            langs = [read32(data, segs, lst + 4 * i) for i in range(cnt)]
            if langs != SHIPPED_LANGS[r]:
                errs.append(f"release {r}: languages {langs} != {SHIPPED_LANGS[r]}")

    # Module TOC: every OPD's toc field.
    n = OPD_SIZE // 8
    for i in range(n):
        toc = read32(data, segs, OPD_BASE + 8 * i + 4)
        if toc != US_TOC:
            errs.append(f"OPD {i} (0x{OPD_BASE + 8*i:08X}) toc 0x{toc:08X} != 0x{US_TOC:08X}")
            break

    # Clock probe chain.
    v = read32(data, segs, CLOCK_TOC_ENTRY)
    if v != CLOCK_TOC_VALUE:
        errs.append(f"word(0x{CLOCK_TOC_ENTRY:08X}) = 0x{v:08X} != 0x{CLOCK_TOC_VALUE:08X}")
    p = read32(data, segs, CLOCK_PTR_SLOT)
    if p != CLOCK_BASE:
        errs.append(f"word(0x{CLOCK_PTR_SLOT:08X}) = 0x{p:08X} != 0x{CLOCK_BASE:08X}")
    if not (BSS_LO <= CLOCK_BASE < BSS_HI):
        errs.append(f"clock base 0x{CLOCK_BASE:08X} outside .bss [{BSS_LO:08X},{BSS_HI:08X})")

    return errs


def expect(needle: str, hay: str, what: str, errs: list[str]):
    if needle not in hay:
        errs.append(f"{what}: missing {needle!r}")


def forbid(needle: str, hay: str, what: str, errs: list[str]):
    if needle in hay:
        errs.append(f"{what}: stale EU value {needle!r} still present")


def check_sources() -> list[str]:
    errs: list[str] = []
    clock = open(os.path.join(REPO_ROOT, "infamous-recomp", "infamous_clock_probe.h")).read()
    stubs = open(os.path.join(REPO_ROOT, "infamous-recomp", "stubs.cpp")).read()
    lang = open(os.path.join(REPO_ROOT, "infamous-recomp", "infamous_lang.h")).read()

    expect("word(0x784070)", clock, "clock probe TOC-entry check", errs)
    expect("0x769554", clock, "clock probe TOC value", errs)
    expect("word(0x76155C)", clock, "clock probe pointer check", errs)
    expect("0x7BC650", clock, "clock probe base", errs)
    forbid("0x7840B8", clock, "clock probe", errs)
    forbid("0x7615A4", clock, "clock probe", errs)
    forbid("0x76959C", clock, "clock probe", errs)

    expect("PM_TOC = 0x0078BF78u", stubs, "stubs.cpp PM_TOC", errs)
    forbid("PM_TOC = 0x0078BFC0u", stubs, "stubs.cpp PM_TOC", errs)

    expect("LISTS = 0x00720868u", lang, "infamous_lang.h LISTS", errs)
    expect("{ 3, 5, 1, 3, 3, 8, 9 }", lang, "infamous_lang.h SHIPPED_COUNT", errs)
    return errs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    args = ap.parse_args()

    errs = check_elf(args.elf) + check_sources()
    if errs:
        print(f"FAIL ({len(errs)}):")
        for e in errs:
            print("  -", e)
        return 1
    print("PASS: US title-dependent runtime addresses match the US EBOOT.elf and the in-tree constants.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
