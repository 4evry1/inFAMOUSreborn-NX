#!/usr/bin/env python3
"""Verify launch-lab profiles are filename-selected and never force RF_ASYNC off."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PORT_MAIN = ROOT.parent / "infamous-recomp" / "main.cpp"
SPURS = ROOT / "libs" / "spurs" / "cellSpurs.c"


def main() -> None:
    port = PORT_MAIN.read_text()
    assert 'program_name_has(program_path, "-lab-audiooff")' in port
    assert 'program_name_has(program_path, "-lab-noaudio")' in port
    assert 'program_name_has(program_path, "-lab-silent")' in port
    assert 'program_name_has(program_path, "-lab-demote")' in port
    assert 'set_default_env("RF_ASYNC", "0");' in port
    assert 'set_default_env("PS3_NO_AUDIO", "1");' in port
    assert 'set_default_env("AUDIO_THREAD_BOOST", "0");' in port
    assert 'set_default_env("INFAMOUS_SILENT_AUDIO_PRIORITY", "1");' in port

    spurs = SPURS.read_text()
    assert 'getenv("SPURS_KICK_LOG")' in spurs
    assert "if (s_kick_log && kick_slot == 0)" in spurs

    print("launch labs: audio and renderer profiles selected by executable name")


if __name__ == "__main__":
    main()
