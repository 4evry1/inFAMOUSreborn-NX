#!/usr/bin/env python3
"""Checks on a lifted libsre image.

usage: check_lift.py <prx dir> <lift dir>

Verifies:
  - the lifted function count;
  - every export OPD's code address is a lifted function;
  - the named cellSpurs entry points resolve NID -> OPD -> code -> symbol;
  - all 32 import stubs lift to ps3_hle_call(nid);
  - no lifted function reaches outside the text extent.
NID names come from tools/nid_database.py, never from a hardcoded table.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "ps3recomp", "tools"))
import nid_database  # noqa: E402

REQUIRED_SYMBOLS = [
    ("cellSpurs", "cellSpursInitialize"),
    ("cellSpurs", "cellSpursCreateTaskset"),
    ("cellSpurs", "cellSpursCreateTasksetWithAttribute"),
    ("cellSpurs", "cellSpursCreateTask"),
    ("cellSpurs", "_cellSpursLFQueueInitialize"),
    ("cellSpurs", "cellSpursLFQueueAttachLv2EventQueue"),
]
EXPECTED_IMPORT_STUBS = 32
TEXT_END = 0x30030000


def lifted_source(lift_dir):
    chunks = sorted(n for n in os.listdir(lift_dir) if re.match(r"ppu_recomp_\d+\.cpp$", n))
    return "\n".join(open(os.path.join(lift_dir, n), errors="replace").read() for n in chunks)


def main():
    prx_dir, lift_dir = sys.argv[1], sys.argv[2]
    exports = json.load(open(os.path.join(prx_dir, "libsre_exports.json")))
    source = lifted_source(lift_dir)
    database = nid_database.get_default_db()

    defined = set(int(a, 16) for a in
                  re.findall(r"^void libsre_func_([0-9A-F]{8})\(", source, re.M))
    failures = 0
    print(f"lifted function bodies: {len(defined)}")

    opd_to_code = exports["opd_to_code"]
    export_pairs = [(lib, nid, opd) for lib, entries in exports["exports"].items()
                    for nid, opd in entries.items()]
    unresolved = [(lib, nid, opd) for lib, nid, opd in export_pairs
                  if int(opd_to_code[opd], 16) not in defined]
    print(f"export OPD code addresses that are lifted functions: "
          f"{len(export_pairs) - len(unresolved)} / {len(export_pairs)}")
    for lib, nid, opd in unresolved:
        print(f"  MISSING {lib} nid={nid} opd={opd} code={opd_to_code[opd]}")
    failures += len(unresolved)

    by_nid = {}
    for lib, nid, opd in export_pairs:
        by_nid.setdefault((lib, int(nid, 16)), opd)

    for library, symbol in REQUIRED_SYMBOLS:
        nid_number = database.lookup_name(symbol)
        nid = f"0x{nid_number:08X}"
        opd = by_nid.get((library, nid_number))
        if opd is None:
            print(f"  UNRESOLVED {symbol}: nid={nid} not exported by {library}")
            failures += 1
            continue
        code = opd_to_code[opd]
        is_lifted = int(code, 16) in defined
        print(f"  {symbol}: nid={nid} -> opd={opd} -> code={code} -> "
              f"libsre_func_{int(code, 16):08X} {'lifted' if is_lifted else 'NOT LIFTED'}")
        if not is_lifted:
            failures += 1

    stub_pattern = r"ps3_hle_call\(0x[0-9A-F]{8}u, ctx\); return;  /\* import stub \*/"
    n_stubs = len(re.findall(stub_pattern, source))
    print(f"import stubs lifted to ps3_hle_call: {n_stubs} / {EXPECTED_IMPORT_STUBS}")
    if n_stubs != EXPECTED_IMPORT_STUBS:
        failures += 1

    outside = sorted(a for a in defined if a >= TEXT_END)
    print(f"lifted functions at or above code-end 0x{TEXT_END:08X}: {len(outside)}")
    failures += len(outside)

    print("RESULT:", "PASS" if failures == 0 else f"FAIL ({failures})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
