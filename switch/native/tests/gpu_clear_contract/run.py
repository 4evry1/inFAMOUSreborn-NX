#!/usr/bin/env python3
"""Compile and execute the real adapter on CPU against a narrow deko C ABI spy."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
NATIVE = HERE.parents[1]
HEADER_SHA256 = "0388f36a5891e526066f89df7c055da1368a502d096e3fa15cb91f0d81aac54e"
assert hashlib.sha256((HERE / "include/deko3d.h").read_bytes()).hexdigest() == HEADER_SHA256
with tempfile.TemporaryDirectory(prefix="gpu-clear-contract-") as directory:
    executable = Path(directory) / "contract"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I", str(HERE / "include"), "-I", str(NATIVE / "adapters"),
        str(HERE / "contract.cpp"), str(NATIVE / "adapters/gpu_clear_deko.cpp"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
