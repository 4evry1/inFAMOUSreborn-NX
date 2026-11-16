"""CPU-only composition guard; never launches the diagnostic."""
from pathlib import Path
import re
import subprocess

native = Path(__file__).resolve().parents[1]
recipe = native / "Makefile.gpu"
assert recipe.is_file(), "Dedicated GPU recipe is missing"
plan = subprocess.check_output(["make", "-B", "-n", "-f", str(recipe)], cwd=native, text=True)
compiled = re.findall(r" -c (\S+) -o ", plan)
assert sorted(compiled) == sorted(["switch_gpu_main.c", "adapters/gpu_clear_deko.cpp", "adapters/deko_queue_owner.cpp", "tests/test_gpu_clear.cpp"]), compiled
main = (native / "switch_gpu_main.c").read_text()
assert "infamous-native-probe" not in main
assert ".flag" not in main
assert re.findall(r"native_test_\w+\(", main) == ["native_test_gpu_clear(", "native_test_gpu_clear("], "Unexpected test entry point"
assert "native_test_gpu_clear(0)" in main
assert "__nx_heap_size" in main and "64u * 1024u * 1024u" in main
assert "-Wl,--gc-sections" in plan
print("PASS dedicated GPU composition: exactly main + GPU adapter + queue owner + GPU test")
