#!/usr/bin/env python3
"""Host-only timebase arithmetic and C/C++ ABI checks."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
TIME = ROOT / "runtime/time"

def run(command):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)

with tempfile.TemporaryDirectory(prefix="timebase-contract-") as scratch:
    out = pathlib.Path(scratch)
    for optimization in ("-O0", "-O2"):
        for sanitized in (False, True):
            flags = ["-Wall", "-Wextra", "-Werror", optimization, "-I", TIME]
            if sanitized:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie"]
            link = ["-no-pie"] if sanitized else []
            obj = out / "timebase.o"
            run(["cc", "-std=c11", *flags, "-c", TIME / "timebase.c", "-o", obj])
            run(["cc", "-std=c11", *flags, HERE / "contract.c", obj, *link, "-o", out / "contract"])
            run([out / "contract"])
            run(["c++", "-std=c++17", *flags, HERE / "linkage.cpp", obj, *link, "-o", out / "linkage"])
            run([out / "linkage"])
print("TIMEBASE_MATRIX_PASS")
