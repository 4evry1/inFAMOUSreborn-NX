#!/usr/bin/env python3
"""Focused transformation tests; the missing-module RED is labelled separately."""
import importlib.util
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
path = Path(os.environ.get('TIMEBASE_DERIVATION_MODULE', ROOT / 'tools/derive_ppu_timebase_context.py'))
spec = importlib.util.spec_from_file_location('timebase_derivation_under_test', path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
DECL = '''/* The guest timebase (mftb/mftbu): one global monotonic clock scaled to the
 * PS3's 79.8 MHz, provided by the runtime (runtime/syscalls/sys_timer.c). */
#ifdef __cplusplus
extern "C" uint64_t ppu_timebase_now(void);
#else
extern uint64_t ppu_timebase_now(void);
#endif'''

def source(body='    ctx->gpr[0] = ppu_timebase_now();\n'):
    return '#include "ppu_recomp.h"\n' + DECL + '\nvoid func_00000010(ppu_context* ctx) {\n' + body + '}\n'

class DerivationTests(unittest.TestCase):
    def test_real_call_and_nonedit_preservation(self):
        text = source()
        result = module.derive_unit(text, 'fixture')
        self.assertEqual(result['calls'], 1)
        self.assertEqual(result['functions'], ['func_00000010'])
        self.assertEqual(result['output'], text.replace(DECL, module.REPLACEMENT_DECL).replace('= ppu_timebase_now();', '= ppu_native_timebase_now(ctx);'))
        self.assertEqual(len(result['edits']), 2)

    def test_comments_literals_unicode_and_multiple_calls(self):
        text = source('    // ppu_timebase_now(ignored) α\n    const char *s = "ppu_timebase_now()";\n    ctx->gpr[0] = ppu_timebase_now(); ctx->gpr[31] = ppu_timebase_now ( );\n')
        result = module.derive_unit(text, 'fixture')
        self.assertEqual(result['calls'], 2)
        self.assertIn('// ppu_timebase_now(ignored) α', result['output'])
        self.assertIn('"ppu_timebase_now()"', result['output'])

    def test_rejects_unsupported_input(self):
        cases = {
            'argument': source('    ctx->gpr[0] = ppu_timebase_now(ctx);\n'),
            'address': source('    auto f = &ppu_timebase_now;\n'),
            'scope': source() + 'uint64_t x = ppu_timebase_now();\n',
            'directive': source('    #define BAD ppu_timebase_now()\n'),
            'wrong_cpu': source().replace('ppu_context* ctx', 'ppu_context* other'),
            'declaration': source().replace('ppu_timebase_now(void)', 'ppu_timebase_now()'),
            'duplicate_declaration': source() + DECL + '\n',
            'missing_declaration': source().replace(DECL, ''),
            'mixed': source('    ctx->gpr[0] = ppu_native_timebase_now(ctx);\n'),
            'rerun': source().replace(DECL, module.REPLACEMENT_DECL).replace('ppu_timebase_now()', 'ppu_native_timebase_now(ctx)'),
            'nested_cpu': source('    ppu_context spare;\n    ctx->gpr[0] = ppu_timebase_now();\n'),
            'unclosed': source()[:-2],
            'malformed_name': source().replace('func_00000010', 'func_invalid'),
            'commented_declaration': source().replace(DECL, '// ' + DECL.replace('\n', '\n// ')),
            'literal_declaration': source().replace(DECL, 'const char *s = R"tag(\n' + DECL + '\n)tag";'),
        }
        for name, text in cases.items():
            with self.subTest(name=name), self.assertRaises(module.DerivationError):
                module.derive_unit(text, name)

    def test_independent_output_audit(self):
        good = module.derive_unit(source(), 'fixture')['output']
        module.audit_output(good)
        for bad in [good.replace('ppu_native_timebase_now(ctx)', 'ppu_timebase_now()'), good.replace('ppu_native_timebase_now(ctx)', 'ppu_native_timebase_now(other)'), good.replace('ppu_native_timebase_now(ctx)', 'ppu_native_timebase_now()')]:
            with self.assertRaises(module.DerivationError):
                module.audit_output(bad)
        with self.assertRaises(module.DerivationError):
            module.audit_output(good.replace('#include "native_timebase_abi.h"', '/*\n#include "native_timebase_abi.h"\n*/'))

    def test_rejects_context_substitution_or_reference_escape(self):
        for prefix in ['auto ctx = other;', 'ctx = other;', '(ctx) = other;',
                       'auto alias = ctx;', 'auto *alias = &ctx;', 'unknown_mutator(ctx);',
                       'std::swap(ctx, other);', '[](auto ctx){};']:
            with self.subTest(prefix=prefix), self.assertRaises(module.DerivationError):
                module.derive_unit(source('    ' + prefix + '\n    ctx->gpr[0] = ppu_timebase_now();\n'), 'bad_owner')

    def test_accepts_reviewed_pointer_by_value_calls(self):
        text = source('    func_00000020(ctx);\n    DRAIN_TRAMPOLINE(ctx);\n    ppu_native_write64(ctx, 4, 8);\n    ps3_hle_call(5, ctx);\n    ctx->gpr[0] = ppu_timebase_now();\n')
        text = text.replace('void func_00000010', module.DRAIN_BLOCK + '\nvoid func_00000010')
        self.assertEqual(module.derive_unit(text, 'valid_calls')['calls'], 1)
        with self.assertRaises(module.DerivationError):
            module.derive_unit(text.replace('_tf((void*)(ctx));', 'ctx = other;'), 'bad_trampoline')
        with self.assertRaises(module.DerivationError):
            module.derive_unit(text.replace(module.DRAIN_BLOCK, module.DRAIN_BLOCK + '; ctx = other'), 'trampoline_suffix')

    def test_external_declaration_requires_value_pointer_not_reference(self):
        text = source('    extension(ctx);\n    ctx->gpr[0] = ppu_timebase_now();\n')
        good = text.replace('void func_00000010', 'extern "C" int extension(ppu_context* ctx);\nvoid func_00000010')
        self.assertEqual(module.derive_unit(good, 'value_extension')['calls'], 1)
        bad = good.replace('extension(ppu_context* ctx)', 'extension(ppu_context*& ctx)')
        with self.assertRaises(module.DerivationError):
            module.derive_unit(bad, 'reference_extension')

    def test_macro_rebinding_and_wrong_call_arity_are_rejected(self):
        base = source('    extension(ctx);\n    ctx->gpr[0] = ppu_timebase_now();\n').replace('void func_00000010', 'extern "C" int extension(ppu_context* ctx);\nvoid func_00000010')
        variants = [base.replace('void func_00000010', '#define extension(p) ((p) = other)\nvoid func_00000010'),
                    base.replace('void func_00000010', '/* prefix */ #define extension(p) ((p) = other)\nvoid func_00000010'),
                    base.replace('extension(ctx);', 'extension(other, ctx);'),
                    base.replace('void func_00000010', '#define ctx other\nvoid func_00000010'),
                    base.replace('void func_00000010', '#define hidden() ctx = other\nvoid func_00000010').replace('extension(ctx);', 'hidden();'),
                    base.replace('void func_00000010', '#define hidden() \\\n+ ctx = other\nvoid func_00000010').replace('extension(ctx);', 'hidden();')]
        for bad in variants:
            with self.subTest(bad=bad), self.assertRaises(module.DerivationError):
                module.derive_unit(bad, 'macro_or_arity')

    def test_multiple_value_declarations_on_one_line(self):
        text = source('    second(ctx);\n    ctx->gpr[0] = ppu_timebase_now();\n')
        text = text.replace('void func_00000010', 'extern "C" int first(ppu_context* ctx); extern "C" double second(ppu_context* ctx);\nvoid func_00000010')
        self.assertEqual(module.derive_unit(text, 'two_declarations')['calls'], 1)

    def test_explicit_multi_argument_and_unnamed_value_parameters(self):
        text = source('    extension(ctx, 4);\n    unnamed(ctx);\n    ctx->gpr[0] = ppu_timebase_now();\n')
        text = text.replace('void func_00000010', 'extern "C" int extension(ppu_context* ctx, uint32_t value); extern "C" void unnamed(ppu_context*);\nvoid func_00000010')
        self.assertEqual(module.derive_unit(text, 'multi_value')['calls'], 1)
        for bad in [text.replace('extension(ctx, 4)', 'extension(4, ctx)'), text.replace('extension(ctx, 4)', 'extension(ctx, ctx)'), text.replace('extension(ctx, 4)', 'extension(ctx)')]:
            with self.assertRaises(module.DerivationError):
                module.derive_unit(bad, 'multi_bad_argument')

if __name__ == '__main__':
    unittest.main(verbosity=2)
