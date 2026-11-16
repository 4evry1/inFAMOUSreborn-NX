#!/usr/bin/env python3
"""Derive immutable sorted offsets over the exact original generated table."""
import argparse
import ctypes
import errno
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "infamous-recomp/recompiled/ppu_recomp_004.cpp"
HEADER = ROOT / "infamous-recomp/recompiled/ppu_recomp.h"
SOURCE_SHA256 = "df572c7644023d622e261f5aa571c43443dc566742e674bd5e9d21324b592564"
HEADER_SHA256 = "e4be2060d1df2bced2215669a21f4929c89af9e32cced271ae8bb0a1d87556f0"
EXPECTED_COUNT = 27312
BEGIN = "/* Function table */\nconst func_entry function_table[] = {\n"
ENTRY = re.compile(r'    \{ 0x([0-9A-Fa-f]{8})ULL, (func_([0-9A-Fa-f]{8})), "(func_[0-9A-Fa-f]{8})" \},')
END = re.compile(r"};\nconst uint64_t function_table_count = ([1-9][0-9]*);\n?\Z")

class IndexError(ValueError):
    pass

def sha(data):
    return hashlib.sha256(data).hexdigest()

def parse_table(source):
    if source.count(BEGIN) != 1:
        raise IndexError("table declaration missing or duplicated")
    table = source.split(BEGIN, 1)[1]
    sentinel = "    { 0, NULL, NULL }\n"
    if table.count(sentinel) != 1:
        raise IndexError("exact single sentinel required")
    body, ending = table.split(sentinel)
    count = END.fullmatch(ending)
    if count is None:
        raise IndexError("table count or trailing grammar changed")
    entries = []
    addresses = set()
    symbols = set()
    for line in body.splitlines():
        match = ENTRY.fullmatch(line)
        if match is None:
            raise IndexError("unrecognized table entry grammar")
        address = int(match[1], 16)
        symbol = match[2]
        if address == 0 or address > 0xffffffff or address & 3:
            raise IndexError("invalid code address")
        if int(match[3], 16) != address or match[4] != symbol:
            raise IndexError("address, symbol and name disagree")
        if address in addresses or symbol in symbols:
            raise IndexError("duplicate address or symbol")
        addresses.add(address)
        symbols.add(symbol)
        entries.append((address, symbol))
    if not entries or len(entries) != int(count[1]):
        raise IndexError("entry count mismatch")
    offsets = sorted(range(len(entries)), key=lambda index: entries[index][0])
    return entries, offsets

def render_index(entries, offsets):
    lines = ['#include "compiled_code_index.h"', '',
        'const uint32_t ppu_compiled_code_sorted_offsets[PPU_COMPILED_CODE_INDEX_COUNT] = {']
    for start in range(0, len(offsets), 12):
        lines.append('    ' + ', '.join(str(index) + 'u' for index in offsets[start:start + 12]) + ',')
    lines.extend(['};', ''])
    return '\n'.join(lines)

def publish(stage, output):
    library = ctypes.CDLL(None, use_errno=True)
    operation = getattr(library, "renameat2", None)
    if operation is None:
        raise IndexError("atomic no-replace publication unavailable")
    operation.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    operation.restype = ctypes.c_int
    if operation(-100, os.fsencode(stage), -100, os.fsencode(output), 1):
        status = ctypes.get_errno()
        if status == errno.EEXIST:
            raise IndexError("output exists")
        raise IndexError("publication failed errno=" + str(status))

def generate(source, header, output):
    source, header, output = Path(source), Path(header), Path(output)
    if output.exists():
        raise IndexError("output exists")
    original, public_header = source.read_bytes(), header.read_bytes()
    if sha(original) != SOURCE_SHA256 or sha(public_header) != HEADER_SHA256:
        raise IndexError("original source/header pin mismatch")
    entries, offsets = parse_table(original.decode('utf-8'))
    if len(entries) != EXPECTED_COUNT:
        raise IndexError("pinned table count changed")
    generated_header = """#ifndef NATIVE_COMPILED_CODE_INDEX_H
#define NATIVE_COMPILED_CODE_INDEX_H
#include <stdint.h>
#define PPU_COMPILED_CODE_INDEX_COUNT 27312u
#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t ppu_compiled_code_sorted_offsets[PPU_COMPILED_CODE_INDEX_COUNT];
#ifdef __cplusplus
}
#endif
#endif
"""
    contents = {'compiled_code_index.h':generated_header.encode(),
        'compiled_code_index.cpp':render_index(entries, offsets).encode()}
    manifest = {'schema':1, 'scope':'immutable offsets over original borrowed func_entry table',
        'source_sha256':SOURCE_SHA256, 'public_header_sha256':HEADER_SHA256,
        'tool_sha256':sha(Path(__file__).read_bytes()), 'count':len(entries),
        'runtime_offset_bytes':len(offsets) * 4,
        'source_order_inversions':sum(entries[index - 1][0] >= entries[index][0] for index in range(1, len(entries))),
        'entries':[{'address':address, 'symbol':symbol, 'original_offset':index}
            for index, (address, symbol) in enumerate(entries)],
        'sorted_offsets':offsets,
        'outputs':{name:{'sha256':sha(data), 'bytes':len(data)} for name, data in contents.items()}}
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.' + output.name + '.pending-', dir=output.parent))
    try:
        for name, data in contents.items():
            (stage / name).write_bytes(data)
        (stage / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True, separators=(',', ':')) + '\n')
        if sha(source.read_bytes()) != SOURCE_SHA256 or sha(header.read_bytes()) != HEADER_SHA256:
            raise IndexError("input changed before publication")
        publish(stage, output)
        return manifest
    finally:
        if stage.exists():
            shutil.rmtree(stage)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        import resource
        import signal
        resource.setrlimit(resource.RLIMIT_AS, (1024**3, 1024**3))
        resource.setrlimit(resource.RLIMIT_CPU, (60, 60))
        os.sched_setaffinity(0, sorted(os.sched_getaffinity(0))[:2])
        signal.alarm(60)
        manifest = generate(SOURCE, HEADER, args.output)
    except (IndexError, OSError, UnicodeError, MemoryError) as error:
        parser.exit(1, 'COMPILED_CODE_INDEX_FAIL ' + str(error) + '\n')
    print('COMPILED_CODE_INDEX_PASS count=' + str(manifest['count']) +
        ' inversions=' + str(manifest['source_order_inversions']))

if __name__ == '__main__':
    main()
