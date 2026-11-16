#!/usr/bin/env python3
"""Keep synchronous filesystem diagnostics off the normal cellFsRead path."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "runtime" / "ppu" / "ppu_fs.cpp"


def main() -> None:
    source = SOURCE.read_text()
    start = source.index("static void cellFsRead(ppu_context* ctx)")
    end = source.index("static void cellFsWrite(ppu_context* ctx)", start)
    function = source[start:end]

    count_at = function.index("read_count++;")
    gate_at = function.index("const bool read_log = getenv(\"FS_READ_LOG\")")
    assert count_at < gate_at, (
        "the read counter must advance on every read, even when diagnostics are off"
    )

    summary_at = function.index("[fs] read summary after")
    summary_gate_at = function.rfind("if (read_log)", 0, summary_at)
    assert summary_gate_at >= 0, (
        "the periodic read summary must not run during normal file loading"
    )
    assert "read_count % 2000" in function[summary_gate_at:summary_at]

    print("cellFsRead logging: diagnostics are gated")


if __name__ == "__main__":
    main()
