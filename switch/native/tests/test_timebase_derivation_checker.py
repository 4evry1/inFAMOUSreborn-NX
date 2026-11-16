#!/usr/bin/env python3
"""Adversarial byte-manifest checks independent of the derivation lexer."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import unittest
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('timebase_checker', ROOT / 'tools/check_ppu_timebase_context.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

def sha(value):
    return hashlib.sha256(value).hexdigest()

def fixture():
    prefix = b'#include "ppu_recomp.h"\n'
    middle = b'\nvoid func_00000010(ppu_context* ctx) {\n    ctx->gpr[0] = '
    suffix = b';\n}\n'
    old_call, new_call = b'ppu_timebase_now()', b'ppu_native_timebase_now(ctx)'
    before = prefix + checker.OLD_BLOCK + middle + old_call + suffix
    after = prefix + checker.NEW_BLOCK + middle + new_call + suffix
    first = [len(prefix), len(checker.OLD_BLOCK), len(prefix), len(checker.NEW_BLOCK), 'timebase_declaration', sha(checker.OLD_BLOCK), sha(checker.NEW_BLOCK)]
    second = [len(prefix + checker.OLD_BLOCK + middle), len(old_call), len(prefix + checker.NEW_BLOCK + middle), len(new_call), 'explicit_timebase_call', sha(old_call), sha(new_call)]
    return before, after, [first, second]

class CheckerTests(unittest.TestCase):
    def test_optimized_python_cannot_accept_any_artifact(self):
        script = "import runpy; m=runpy.run_path(%r); print(m['check_unit'](b'', b'', [], 92))" % str(ROOT / 'tools/check_ppu_timebase_context.py')
        result = subprocess.run([sys.executable, '-O', '-c', script], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("'calls': 92", result.stdout)

    def test_accepts_exact_independent_fixture(self):
        before, after, edits = fixture()
        self.assertEqual(checker.check_unit(before, after, edits, 1), {'calls': 1, 'functions': 1, 'nonedit_bytes_equal': True})

    def test_rejects_tampered_output_even_with_updated_edit_hash(self):
        before, after, edits = fixture()
        altered = after.replace(b'ppu_native_timebase_now(ctx)', b'ppu_native_timebase_now(abc)')
        edits[1][-1] = sha(b'ppu_native_timebase_now(abc)')
        with self.assertRaises(AssertionError):
            checker.check_unit(before, altered, edits, 1)

    def test_rejects_nonedit_cpu_function_and_tail_changes(self):
        before, after, edits = fixture()
        for bad in [after.replace(b'func_00000010', b'func_00000020'), after.replace(b'gpr[0]', b'gpr[1]'), after + b'\n', after.replace(b'#include "ppu_recomp.h"', b'#include "bad_header.h"')]:
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                checker.check_unit(before, bad, edits, 1)

    def test_rejects_manifest_count_offsets_missing_and_classes(self):
        before, after, edits = fixture()
        variants = []
        for position, value in [(0, -1), (1, 0), (2, 999), (3, 1), (4, 'unclassified')]:
            changed = copy.deepcopy(edits)
            changed[1][position] = value
            variants.append(changed)
        variants += [edits[:1], list(reversed(edits)), edits + [edits[1]]]
        for bad in variants:
            with self.subTest(bad=bad), self.assertRaises(AssertionError):
                checker.check_unit(before, after, bad, 1)
        with self.assertRaises(AssertionError):
            checker.check_unit(before, after, edits, 2)

if __name__ == '__main__':
    unittest.main(verbosity=2)
