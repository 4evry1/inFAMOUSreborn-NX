#!/usr/bin/env python3
"""Convert lift_prx.py's <name>_imports.json into ppu_lifter.py's --hle-stubs list.

lift_prx.py emits {library: {nid_hex: stub_slot_ea_hex}}; ppu_lifter.py wants
[{"library": str, "nid": "0xNNNNNNNN", "stub": "0xADDR"}, ...] so that each stub
address lifts to ps3_hle_call(nid) instead of the literal .lib.stub trampoline.

usage: make_hle_stubs.py <libsre_imports.json> <out.json>
"""

import json
import sys


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    entries = json.load(open(sys.argv[1]))
    stubs = [
        {"library": library, "nid": nid, "stub": stub}
        for library, imports in sorted(entries.items())
        for nid, stub in sorted(imports.items())
    ]
    seen = {}
    for entry in stubs:
        slot = int(entry["stub"], 16)
        if slot in seen:
            raise SystemExit(f"stub slot {entry['stub']} appears twice: "
                             f"{seen[slot]} and {entry['nid']}")
        seen[slot] = entry["nid"]
    with open(sys.argv[2], "w") as out:
        json.dump(stubs, out, indent=1)
    print(f"hle stubs: {len(stubs)} entries from "
          f"{len({e['library'] for e in stubs})} libraries -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
