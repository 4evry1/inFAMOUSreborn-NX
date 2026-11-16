#!/usr/bin/env python3
"""Independent byte-edit/census verifier for the pinned timebase second pass."""
import argparse
import hashlib
import json
from pathlib import Path
import re

if not __debug__:
    raise RuntimeError('Timebase checker requires unoptimized Python; no acceptance artifact emitted')

COUNTS = {'000': 16, '001': 17, '002': 39, '003': 14, '004': 6}
OLD_BLOCK = b'''/* The guest timebase (mftb/mftbu): one global monotonic clock scaled to the
 * PS3's 79.8 MHz, provided by the runtime (runtime/syscalls/sys_timer.c). */
#ifdef __cplusplus
extern "C" uint64_t ppu_timebase_now(void);
#else
extern uint64_t ppu_timebase_now(void);
#endif'''
NEW_BLOCK = b'/* Timebase capability is recovered from the explicit CPU owner. */\n#include "native_timebase_abi.h"'
ASSIGNMENT = re.compile(rb'\bctx->gpr\[[0-9]+\] = ppu_timebase_now\(\);')
FUNCTION = re.compile(rb'^void (func_[0-9a-fA-F]{8})\(ppu_context\* ctx\) \{', re.MULTILINE)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def check_unit(before, after, records, expected):
    assert before.count(OLD_BLOCK) == 1
    calls = [match.start() + match.group().index(b'ppu_timebase_now') for match in ASSIGNMENT.finditer(before)]
    assert len(calls) == expected
    declarations = 0
    actual_calls = []
    cursor, new_cursor = 0, 0
    for record in records:
        start, length, target, target_length, kind, old_hash, new_hash = record
        assert all(type(value) is int and value >= 0 for value in (start, length, target, target_length))
        assert start >= cursor and start + length <= len(before)
        assert target == new_cursor + start - cursor and target + target_length <= len(after)
        assert before[cursor:start] == after[new_cursor:target], 'unclassified byte change'
        original = before[start:start + length]
        replacement = after[target:target + target_length]
        assert sha(original) == old_hash and sha(replacement) == new_hash
        if kind == 'explicit_timebase_call':
            assert original == b'ppu_timebase_now()' and replacement == b'ppu_native_timebase_now(ctx)'
            actual_calls.append(start)
        elif kind == 'timebase_declaration':
            assert original == OLD_BLOCK and replacement == NEW_BLOCK
            declarations += 1
        else:
            raise AssertionError('unknown edit class')
        cursor, new_cursor = start + length, target + target_length
    assert before[cursor:] == after[new_cursor:], 'unclassified tail change'
    assert actual_calls == calls and declarations == 1
    before_functions = FUNCTION.findall(before)
    assert before_functions == FUNCTION.findall(after) and len(set(before_functions)) == len(before_functions)
    assert after.count(b'ppu_native_timebase_now(ctx)') == expected
    return {'calls': expected, 'functions': len(before_functions), 'nonedit_bytes_equal': True}

def check_project(root, input_directory, output_directory):
    manifest = json.loads((output_directory / 'manifest.json').read_text())
    context_bytes = (input_directory / 'manifest.json').read_bytes()
    assert sha(context_bytes) == manifest['context_manifest_sha256'] == 'bc92e211a641ee7c98ed25a0d038e7854779c74278ed1d04a7d9770685d393aa'
    context = json.loads(context_bytes)
    assert manifest['expected_calls'] == COUNTS
    for name, row in context['original_inputs'].items():
        assert sha((root / 'infamous-recomp/recompiled' / name).read_bytes()) == row['sha256']
    results = {}
    for unit, count in COUNTS.items():
        name = f'ppu_recomp_{unit}.cpp'
        before = (input_directory / name).read_bytes()
        after = (output_directory / name).read_bytes()
        row = manifest['outputs'][name]
        assert sha(before) == context['outputs'][name]['sha256'] == manifest['inputs'][name]
        assert sha(after) == row['sha256']
        result = check_unit(before, after, row['edits'], count)
        assert row['calls'] == count and len(row['functions']) == result['functions']
        results[name] = result
    for name in ('ppu_recomp.h', 'native_memory_abi.h'):
        before = (input_directory / name).read_bytes()
        after = (output_directory / name).read_bytes()
        assert before == after and sha(after) == context['outputs'][name]['sha256'] == manifest['outputs'][name]['sha256']
    assert manifest['cpu_sha256'] == context['cpu_type_derived_sha256'] == context['cpu_type_original_sha256']
    assert sha((output_directory / 'native_timebase_abi.h').read_bytes()) == manifest['abi_sha256']
    assert sha((root / 'ps3recomp/runtime/ppu/native_timebase_abi.h').read_bytes()) == manifest['abi_sha256']
    return {'status': 'PASS', 'calls': 92, 'units': results, 'original_inputs_preserved': True,
            'cpu_and_memory_abi_bytes_preserved': True, 'manifest_sha256': sha((output_directory / 'manifest.json').read_bytes()),
            'checker_sha256': sha(Path(__file__).read_bytes())}

def main():
    args = argparse.ArgumentParser()
    args.add_argument('--root', type=Path, required=True)
    args.add_argument('--input', type=Path, required=True)
    args.add_argument('--output', type=Path, required=True)
    args.add_argument('--report', type=Path, required=True)
    option = args.parse_args()
    if option.report.exists():
        raise SystemExit('Report exists; refusing overwrite')
    report = check_project(option.root, option.input, option.output)
    option.report.write_text(json.dumps(report, indent=2) + '\n')
    print('TIMEBASE_DERIVATION_CHECK_PASS calls=92', flush=True)

if __name__ == '__main__':
    main()
