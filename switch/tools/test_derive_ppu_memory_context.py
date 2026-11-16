import json
from pathlib import Path
import tempfile
import unittest
import derive_ppu_memory_context as derive

FIXTURES = Path(__file__).parent / "fixtures/ppu_memory_context"
HEADER = """#include <stdint.h>
typedef struct ppu_context { uint64_t gpr[32]; } ppu_context;
/* Memory access helpers (provided by runtime) */
extern unsigned char *vm_base;
#define vm_read32(a) old_inline(a)
/* Syscall handler */
void lv2_syscall(ppu_context* ctx);
"""

class DerivationTests(unittest.TestCase):
    def test_independent_golden(self):
        source = (FIXTURES / "input.cpp").read_text()
        output, edits, counts = derive.derive_unit(source, "fixture.cpp")
        self.assertEqual(output, (FIXTURES / "expected.cpp").read_text())
        derive.verify_edits(source.encode(), output.encode(), edits)
        self.assertEqual(counts["ll_reserve_value"], 2)
        self.assertEqual(counts["sc32"], 1)
        self.assertEqual(counts["sc64"], 1)

    def test_spliced_comment_rejected(self):
        source = "void func_F0000000(ppu_context* ctx) {\n // comment " + chr(92) + "\n ctx->gpr[0] = vm_read32(4);\n}\n"
        with self.assertRaises(derive.DerivationError):
            derive.derive_unit(source, "spliced.cpp")

    def test_manifest_explicit_function_coverage(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'fixture.cpp'
            source.write_bytes((FIXTURES / 'input.cpp').read_bytes())
            header = root / 'header.h'
            header.write_text(HEADER)
            manifest = derive.derive_fixture(source, header, root / 'out')
            coverage = manifest['outputs']['fixture.cpp']['function_coverage']
            self.assertGreater(coverage['count'], 0)
            self.assertEqual(coverage['count'], len(coverage['names']))
            self.assertEqual(coverage['original_signature_sha256'], coverage['derived_signature_sha256'])

    def test_element_and_left_load_staging_golden(self):
        source = """void func_F0000000(ppu_context* ctx) {
 { uint64_t ea = ctx->gpr[1]; ea &= ~3ULL; memset(&ctx->vr[2], 0, 16); memcpy((uint8_t*)&ctx->vr[2] + (ea & 15), vm_base + (uint32_t)ea, 4); }
 { uint64_t ea = ctx->gpr[1]; uint32_t sh = (uint32_t)(ea & 0xF); uint8_t* m = vm_base + (uint32_t)(ea & ~0xFULL); uint8_t* d = (uint8_t*)&ctx->vr[2]; for (int i = 0; i < 16; i++) d[i] = (sh + (uint32_t)i < 16) ? m[sh + i] : 0; }
}
"""
        expected = """void func_F0000000(ppu_context* ctx) {
 { uint64_t ea = ctx->gpr[1]; ea &= ~3ULL; unsigned char _ppu_memory_bytes[16] = {0}; ppu_native_read_bytes(ctx, (uint32_t)ea, _ppu_memory_bytes + (ea & 15), 4); memcpy(&ctx->vr[2], _ppu_memory_bytes, 16); }
 { uint64_t ea = ctx->gpr[1]; uint32_t sh = (uint32_t)(ea & 0xF); unsigned char _ppu_memory_bytes[16] = {0}; ppu_native_read_bytes(ctx, (uint32_t)ea, _ppu_memory_bytes, 16 - sh); memcpy(&ctx->vr[2], _ppu_memory_bytes, 16); }
}
"""
        output, _, _ = derive.derive_unit(source, "lanes.cpp")
        self.assertEqual(output, expected)
        for malformed in (source.replace("ea &= ~3ULL", "ea &= ~7ULL"), source.replace("i < 16", "i <= 16")):
            with self.assertRaises(derive.DerivationError):
                derive.derive_unit(malformed, "bad.cpp")

    def test_header_cpu_and_preamble(self):
        output, edits, before, after = derive.derive_header(HEADER)
        self.assertEqual(before, after)
        self.assertIn('#include "native_memory_abi.h"', output)
        self.assertNotIn("old_inline", output)
        self.assertTrue(output.endswith("void lv2_syscall(ppu_context* ctx);\n"))
        derive.verify_edits(HEADER.encode(), output.encode(), edits)

    def test_unknown_and_contextless_rejected(self):
        cases = [
            'static inline int bad() { return vm_read32(4); }\n',
            'void func_F0000000(ppu_context* ctx) {\n auto p = vm_base + 4;\n}\n',
            'void func_F0000000(ppu_context* ctx) {\n auto f = vm_write32;\n}\n',
            '#define BAD(a) vm_write32(a,0)\n',
            'void func_F0000000(ppu_context* ctx) {\n ctx->reserve_addr = 3;\n}\n',
            'void func_F0000000(ppu_context* ctx) {\n ppu_context another;\n}\n',
        ]
        for source in cases:
            with self.subTest(source=source), self.assertRaises(derive.DerivationError):
                derive.derive_unit(source, "bad.cpp")

    def test_comment_string_and_braces_do_not_gain_context(self):
        source = 'const char *s="vm_write32(1,2) } {";\n/* vm_base { */\nvoid func_F0000000(ppu_context* ctx) {\n ctx->gpr[0]=vm_read8(1);\n}\n'
        output, _, _ = derive.derive_unit(source,"fixture.cpp")
        self.assertIn('"vm_write32(1,2) } {"',output)
        self.assertIn('ppu_native_read8(ctx, 1)',output)

    def test_manifest_determinism_and_tamper_detection(self):
        source=(FIXTURES/"input.cpp").read_text()
        a=derive.derive_unit(source,"fixture.cpp")
        b=derive.derive_unit(source,"fixture.cpp")
        self.assertEqual(a,b)
        with self.assertRaises(derive.DerivationError):
            derive.verify_edits(source.encode(),a[0].replace("preamble(int x)","preamble(int y)").encode(),a[1])
        with self.assertRaises(derive.DerivationError):
            derive.verify_edits(source.encode(),a[0].encode(),a[1]+a[1][:1])

    def test_pointer_context_and_already_native_bypasses_rejected(self):
        bodies = [
            'uint8_t* p = (uint8_t*)(uintptr_t)ctx->gpr[4]; *p = 1;',
            'ctx = other; vm_write32(4, 1);',
            'func_F0000001(other);',
            'ctx->gpr[0] = ppu_native_read32(ctx, 4);',
        ]
        for body in bodies:
            with self.subTest(body=body), self.assertRaises(derive.DerivationError):
                derive.derive_unit('void func_F0000000(ppu_context* ctx) {\n '+body+'\n}\n', 'bad.cpp')

    def test_fixture_manifests_are_deterministic(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            source=root/'fixture.cpp'; source.write_bytes((FIXTURES/'input.cpp').read_bytes())
            header=root/'header.h'; header.write_text(HEADER)
            derive.derive_fixture(source,header,root/'a')
            derive.derive_fixture(source,header,root/'b')
            self.assertEqual((root/'a/manifest.json').read_bytes(),(root/'b/manifest.json').read_bytes())

    def test_atomic_publication_does_not_replace_existing_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); stage=root/'stage'; output=root/'output'
            stage.mkdir(); output.mkdir(); (output/'sentinel').write_text('preserve')
            with self.assertRaises(derive.DerivationError): derive.atomic_publish(stage,output)
            self.assertEqual((output/'sentinel').read_text(),'preserve')
            self.assertTrue(stage.is_dir())

    def test_pin_mismatch_leaves_no_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); source=root/derive.SOURCE_PREFIX; source.mkdir(parents=True)
            for name in derive.PINS: (source/name).write_text('not the approved input')
            with self.assertRaises(derive.DerivationError): derive.derive_project(root,root/'output')
            self.assertFalse((root/'output').exists())

    def test_no_partial_fixture_publication(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/"input.cpp").write_text('void func_F0000000(ppu_context* ctx) {\n auto p=vm_base;\n}\n')
            (root/"header.h").write_text(HEADER)
            output=root/"output"
            with self.assertRaises(derive.DerivationError):
                derive.derive_fixture(root/"input.cpp",root/"header.h",output)
            self.assertFalse(output.exists())
            self.assertEqual(sorted(p.name for p in root.iterdir()),["header.h","input.cpp"])

if __name__ == "__main__":
    unittest.main()
