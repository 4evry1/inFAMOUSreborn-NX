#!/usr/bin/env python3
"""Checked second pass over the immutable accepted context-memory derivation."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
PARSER_PATH = Path(__file__).resolve().parent / 'derive_ppu_memory_context.py'
spec = importlib.util.spec_from_file_location('timebase_memory_parser', PARSER_PATH)
parser = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = parser
spec.loader.exec_module(parser)
DerivationError = parser.DerivationError
OLD_NAME = 'ppu_timebase_now'
NEW_NAME = 'ppu_native_timebase_now'
DECL = '''/* The guest timebase (mftb/mftbu): one global monotonic clock scaled to the
 * PS3's 79.8 MHz, provided by the runtime (runtime/syscalls/sys_timer.c). */
#ifdef __cplusplus
extern "C" uint64_t ppu_timebase_now(void);
#else
extern uint64_t ppu_timebase_now(void);
#endif'''
REPLACEMENT_DECL = '/* Timebase capability is recovered from the explicit CPU owner. */\n#include "native_timebase_abi.h"'
CONTEXT_MANIFEST_SHA256 = 'bc92e211a641ee7c98ed25a0d038e7854779c74278ed1d04a7d9770685d393aa'
EXPECTED_CALLS = {'000': 16, '001': 17, '002': 39, '003': 14, '004': 6}
VALUE_CONTEXT_CALLS = {
    'ps3_hle_call', 'lv2_syscall', 'ps3_indirect_call', 'g_trampoline_fn', 'DRAIN_TRAMPOLINE',
    'ppu_native_read8', 'ppu_native_read16', 'ppu_native_read32', 'ppu_native_read64',
    'ppu_native_write8', 'ppu_native_write16', 'ppu_native_write32', 'ppu_native_write64',
    'ppu_native_read_bytes', 'ppu_native_write_bytes', 'ppu_native_load_reserve32',
    'ppu_native_load_reserve64', 'ppu_native_store_conditional32', 'ppu_native_store_conditional64',
    'ppu_native_atomic_add16', 'ppu_native_zero128', NEW_NAME,
}
DRAIN_BLOCK = r'''#define DRAIN_TRAMPOLINE(ctx) do { \
    while (g_trampoline_fn) { \
        void(*_tf)(void*) = g_trampoline_fn; \
        g_trampoline_fn = 0; \
        _tf((void*)(ctx)); \
    } \
} while(0)'''

def validate_context_use(tokens, index, value_consumers, declared_consumers):
    if index + 1 < len(tokens) and tokens[index + 1].text == '->':
        return
    if (index == 0 or index + 1 >= len(tokens) or tokens[index - 1].text not in ('(', ',') or
            tokens[index + 1].text not in (')', ',')):
        raise DerivationError('CPU pointer declaration, substitution or escape')
    nesting, opening = 0, None
    for previous in range(index - 1, -1, -1):
        token = tokens[previous]
        if token.literal:
            continue
        if token.text == ')':
            nesting += 1
        elif token.text == '(':
            if nesting:
                nesting -= 1
            else:
                opening = previous
                break
    if opening is None or opening == 0:
        raise DerivationError('CPU pointer is not a direct value argument')
    callee = tokens[opening - 1].text
    if opening > 1 and tokens[opening - 2].text in ('.', '->', '::'):
        raise DerivationError('qualified CPU pointer consumer')
    if callee not in value_consumers and not re.fullmatch(r'func_[0-9a-fA-F]{8}', callee):
        raise DerivationError('unreviewed CPU pointer consumer: ' + callee)
    if callee in declared_consumers:
        args, _ = parser.call_arguments(tokens, opening - 1)
        arity, position = declared_consumers[callee]
        if (len(args) != arity or [token.text for token in args[position]] != ['ctx'] or
                args[position][0].start != tokens[index].start):
            raise DerivationError('declared context consumer argument position/arity')

def sha(data):
    return hashlib.sha256(data).hexdigest()

def scan(source, output=False):
    lexer = parser.Lexer()
    depth, function_depth, function, byte_offset = 0, None, None, 0
    changes, functions = [], []
    declaration_tokens, active_includes = 0, 0
    value_consumers = set(VALUE_CONTEXT_CALLS)
    declared_consumers, macro_names = {}, set()
    continuation, drain_end, drain_seen = False, -1, False
    declaration_start = source.find(DECL)
    declaration_end = declaration_start + len(DECL)
    character_offset = 0
    for line_number, line in enumerate(source.splitlines(keepends=True), 1):
        tokens, directive = lexer.line(line)
        preprocessor = directive or continuation
        continuation = preprocessor and line.rstrip('\r\n').endswith('\\')
        spelling = [token.text for token in tokens]
        if directive and len(tokens) >= 3 and spelling[:2] in (['#', 'define'], ['#', 'undef']):
            macro = spelling[2]
            macro_names.add(macro)
            if macro == 'DRAIN_TRAMPOLINE':
                end = character_offset + len(DRAIN_BLOCK)
                if (drain_seen or function is not None or spelling[1] != 'define' or
                        not source[character_offset:].startswith(DRAIN_BLOCK) or
                        (end < len(source) and source[end] not in '\r\n')):
                    raise DerivationError('unreviewed trampoline macro definition')
                drain_seen, drain_end = True, character_offset + len(DRAIN_BLOCK)
            elif macro in value_consumers or macro in ('ctx', OLD_NAME, NEW_NAME) or re.fullmatch(r'func_[0-9a-fA-F]{8}', macro):
                raise DerivationError('macro collision with context/consumer identity')
        if preprocessor and character_offset >= drain_end and any(not t.literal and t.text == 'ctx' for t in tokens):
            raise DerivationError('context in unreviewed preprocessor body')
        if (line.strip() == '#include "native_timebase_abi.h"' and directive and
                [t.text for t in tokens] == ['#', 'include', '"native_timebase_abi.h"']):
            active_includes += 1
        opening = len(tokens) == 8 and tokens[0].text == 'void' and tokens[1].text.startswith('func_')
        if opening:
            parser.require(tokens[2:], '(ppu_context* ctx) {')
            if not re.fullmatch(r'func_[0-9a-fA-F]{8}', tokens[1].text) or function is not None:
                raise DerivationError('invalid generated function scope')
            function = tokens[1].text
            function_depth = depth + 1
            functions.append(function)
        if function is None and not preprocessor:
            start = 0
            for end, token in enumerate(tokens):
                if token.text != ';':
                    continue
                declaration = [t.text for t in tokens[start:end + 1]]
                start = end + 1
                if (len(declaration) >= 9 and declaration[:2] == ['extern', '"C"'] and
                        re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', declaration[3]) and
                        declaration[4] == '(' and declaration[-2:] == [')', ';']):
                    arguments, _ = parser.call_arguments(tokens[start - len(declaration):start], 3)
                    positions = []
                    for position, argument in enumerate(arguments):
                        argument = [item.text for item in argument]
                        if 'ppu_context' not in argument:
                            continue
                        if (argument[:2] != ['ppu_context', '*'] or len(argument) not in (2, 3) or
                                (len(argument) == 3 and not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', argument[2]))):
                            raise DerivationError('context consumer declaration is not pointer-by-value')
                        positions.append(position)
                    if not positions:
                        continue
                    if len(positions) != 1 or declaration[3] in macro_names:
                        raise DerivationError('declared value consumer was replaced by macro')
                    value_consumers.add(declaration[3])
                    declared_consumers[declaration[3]] = (len(arguments), positions[0])
        if function and not opening and any(not t.literal and t.text == 'ppu_context' for t in tokens):
            raise DerivationError('standalone CPU inside generated function')
        if function and not opening and not preprocessor:
            for index, token in enumerate(tokens):
                if not token.literal and token.text == 'ctx':
                    validate_context_use(tokens, index, value_consumers, declared_consumers)
                    if 'DRAIN_TRAMPOLINE' in spelling and not drain_seen:
                        raise DerivationError('trampoline consumer lacks the reviewed macro body')
        for index, token in enumerate(tokens):
            if token.literal or token.text not in (OLD_NAME, NEW_NAME):
                continue
            inside_declaration = declaration_start <= character_offset + token.start < declaration_end
            if not output and inside_declaration and token.text == OLD_NAME:
                declaration_tokens += 1
                continue
            if preprocessor or function is None:
                raise DerivationError(f'helper outside generated function at {line_number}')
            if token.text != (NEW_NAME if output else OLD_NAME):
                raise DerivationError('mixed or residual timebase ABI')
            if index and tokens[index - 1].text in ('::', '.', '->', '&'):
                raise DerivationError('unsupported qualified/helper address use')
            expected = ['(', 'ctx', ')'] if output else ['(', ')']
            actual = [t.text for t in tokens[index + 1:index + 1 + len(expected)]]
            if actual != expected:
                raise DerivationError('timebase helper signature/arity')
            if not output:
                end = tokens[index + 2].end
                start_byte = byte_offset + len(line[:token.start].encode())
                end_byte = byte_offset + len(line[:end].encode())
                changes.append((start_byte, end_byte, NEW_NAME + '(ctx)', 'explicit_timebase_call'))
        if not preprocessor:
            for token in tokens:
                if token.literal:
                    continue
                if token.text == '{':
                    depth += 1
                elif token.text == '}':
                    depth -= 1
                    if depth < 0:
                        raise DerivationError('unmatched closing brace')
                    if function_depth is not None and depth < function_depth:
                        function = function_depth = None
        byte_offset += len(line.encode())
        character_offset += len(line)
    lexer.finish()
    if depth or function:
        raise DerivationError('unclosed generated scope')
    if len(functions) != len(set(functions)):
        raise DerivationError('duplicate generated function')
    if output and active_includes != 1:
        raise DerivationError('native timebase include is not an active directive')
    return changes, functions, declaration_tokens

def audit_output(source):
    if source.count('#include "native_timebase_abi.h"') != 1:
        raise DerivationError('timebase ABI include identity')
    scan(source, output=True)

def derive_unit(source, name):
    if source.count(DECL) != 1:
        raise DerivationError(name + ': exact legacy declaration block required')
    changes, functions, declarations = scan(source)
    if declarations != 2:
        raise DerivationError('legacy declaration block is not active lexical source')
    start = source.index(DECL)
    changes.append((len(source[:start].encode()), len(source[:start + len(DECL)].encode()), REPLACEMENT_DECL, 'timebase_declaration'))
    output, records = parser.apply_edits(source, changes)
    parser.verify_edits(source.encode(), output.encode(), records)
    audit_output(output)
    _, output_functions, _ = scan(output, output=True)
    if functions != output_functions:
        raise DerivationError('generated function identities changed')
    return {'output': output, 'edits': records, 'calls': len(changes) - 1, 'functions': functions}

def derive_project(input_directory, abi, expected_abi, output):
    manifest_path = input_directory / 'manifest.json'
    if sha(manifest_path.read_bytes()) != CONTEXT_MANIFEST_SHA256:
        raise DerivationError('unaccepted context derivation manifest')
    manifest = json.loads(manifest_path.read_text())
    if sha(abi.read_bytes()) != expected_abi:
        raise DerivationError('native timebase ABI identity')
    if output.exists():
        raise DerivationError('output already exists; no overwrite/rerun')
    output.mkdir(parents=True)
    report = {'schema': 1, 'scope': 'context-memory plus explicit CPU timebase', 'context_manifest_sha256': CONTEXT_MANIFEST_SHA256,
              'tool_sha256': sha(Path(__file__).read_bytes()), 'parser_sha256': sha(PARSER_PATH.read_bytes()),
              'abi_sha256': expected_abi, 'inputs': {}, 'outputs': {}, 'expected_calls': EXPECTED_CALLS,
              'original_inputs': manifest['original_inputs'], 'cpu_sha256': manifest['cpu_type_derived_sha256']}
    for name in ('ppu_recomp.h', 'native_memory_abi.h'):
        data = (input_directory / name).read_bytes()
        if sha(data) != manifest['outputs'][name]['sha256']:
            raise DerivationError('context input identity: ' + name)
        (output / name).write_bytes(data)
        report['inputs'][name] = sha(data)
        report['outputs'][name] = {'sha256': sha(data), 'bytes': len(data)}
    shutil.copy2(abi, output / 'native_timebase_abi.h')
    report['outputs']['native_timebase_abi.h'] = {'sha256': expected_abi, 'bytes': abi.stat().st_size}
    for unit, expected in EXPECTED_CALLS.items():
        name = f'ppu_recomp_{unit}.cpp'
        data = (input_directory / name).read_bytes()
        if sha(data) != manifest['outputs'][name]['sha256']:
            raise DerivationError('context input identity: ' + name)
        print('DERIVE_BEGIN', name, flush=True)
        result = derive_unit(data.decode(), name)
        if result['calls'] != expected:
            raise DerivationError(f'call census {name}: {result["calls"]} != {expected}')
        encoded = result.pop('output').encode()
        (output / name).write_bytes(encoded)
        report['inputs'][name] = sha(data)
        report['outputs'][name] = {'sha256': sha(encoded), 'bytes': len(encoded), **result}
        print('DERIVE_PASS', name, 'calls', expected, 'functions', len(result['functions']), flush=True)
    for name, digest in report['inputs'].items():
        if sha((input_directory / name).read_bytes()) != digest:
            raise DerivationError('input changed during derivation')
    if sha(abi.read_bytes()) != expected_abi:
        raise DerivationError('ABI changed during derivation')
    (output / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print('DERIVATION_PASS calls=92 original_inputs_unchanged=true', flush=True)

def main():
    argument = argparse.ArgumentParser()
    argument.add_argument('--input', type=Path, required=True)
    argument.add_argument('--abi', type=Path, required=True)
    argument.add_argument('--abi-sha256', required=True)
    argument.add_argument('--output', type=Path, required=True)
    args = argument.parse_args()
    parser.apply_process_limits(1200, 4096, 2)
    derive_project(args.input, args.abi, args.abi_sha256, args.output)

if __name__ == '__main__':
    main()
