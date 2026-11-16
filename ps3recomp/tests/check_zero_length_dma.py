#!/usr/bin/env python3
"""Guard the architectural zero-byte MFC completion path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DMA = ROOT / "runtime" / "spu" / "spu_dma.h"
TEST = ROOT / "runtime" / "spu" / "tests" / "test_spu_lifted_start.c"


def main() -> None:
    source = DMA.read_text()
    function = source.index("static inline int mfc_do_transfer(")
    zero_guard = source.index("if (size == 0)", function)
    fast_dispatch = source.index("return mfc_do_transfer_fast(", function)
    assert zero_guard < fast_dispatch, (
        "zero-byte MFC completion must be handled before fast/slow dispatch"
    )
    guard = source[zero_guard : source.index(";", zero_guard) + 1]
    assert "& 15u" in guard and "? 0 : -1" in guard, (
        "zero-byte MFC completion must preserve four-bit alignment validation"
    )

    tests = TEST.read_text()
    assert "test_zero_length_dma();" in tests
    assert "zero-byte GET still rejects mismatched four-bit alignment" in tests
    print("zero-byte MFC completion: guarded before dispatch")


if __name__ == "__main__":
    main()
