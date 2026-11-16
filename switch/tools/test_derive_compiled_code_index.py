import json
from pathlib import Path
import tempfile
import unittest
import derive_compiled_code_index as derive

SOURCE = """// unrelated generated body must not be transformed
/* Function table */
const func_entry function_table[] = {
    { 0x00000200ULL, func_00000200, "func_00000200" },
    { 0x00000100ULL, func_00000100, "func_00000100" },
    { 0x00462380ULL, func_00462380, "func_00462380" },
    { 0, NULL, NULL }
};
const uint64_t function_table_count = 3;
"""

class IndexTests(unittest.TestCase):
    def test_complete_sorted_offsets(self):
        entries, order = derive.parse_table(SOURCE)
        self.assertEqual(order, [1, 0, 2])
        self.assertEqual([entries[index][0] for index in order], [0x100, 0x200, 0x462380])
        self.assertEqual(sorted(order), list(range(3)))
    def test_malformed_tables_fail(self):
        cases = [
            SOURCE.replace('count = 3', 'count = 2'),
            SOURCE.replace('0x00000200ULL', '0x00000202ULL'),
            SOURCE.replace('0x00000200ULL', '0x100000000ULL'),
            SOURCE.replace('0x00000200ULL', '0x00000000ULL'),
            SOURCE.replace('func_00000200,', 'NULL,'),
            SOURCE.replace('func_00000200,', 'func_00000100,'),
            SOURCE.replace('"func_00000200"', '"different"'),
            SOURCE.replace('    { 0, NULL, NULL }', ''),
            SOURCE.replace('    { 0, NULL, NULL }', '    { 0, NULL, NULL }\n    { 0, NULL, NULL }'),
            SOURCE.replace('0x00000200ULL, func_00000200, "func_00000200"', '0x00000100ULL, func_00000100, "func_00000100"'),
            SOURCE + '// unexpected trailing text',
            SOURCE.replace('    { 0, NULL, NULL }', '    { 0, NULL, NULL },'),
            SOURCE.replace('    { 0x00000200', '    /* hidden */ { 0x00000200'),
        ]
        for source in cases:
            with self.subTest(source=source), self.assertRaises(derive.IndexError):
                derive.parse_table(source)
    def test_determinism_and_complete_manifest(self):
        a = derive.render_index(*derive.parse_table(SOURCE))
        b = derive.render_index(*derive.parse_table(SOURCE))
        self.assertEqual(a, b)
        self.assertIn('1u, 0u, 2u', a)
    def test_pinned_failure_publishes_nothing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / 'source.cpp'; source.write_text(SOURCE)
            header = root / 'header.h'; header.write_text('not pinned')
            with self.assertRaises(derive.IndexError):
                derive.generate(source, header, root / 'output')
            self.assertFalse((root / 'output').exists())
    @unittest.skipUnless(derive.SOURCE.exists(), 'needs the game code regenerated from your own disc (switch/scripts/20-generate-sources.sh)')
    def test_actual_pinned_generation_is_deterministic(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first = derive.generate(derive.SOURCE, derive.HEADER, root / 'first')
            second = derive.generate(derive.SOURCE, derive.HEADER, root / 'second')
            self.assertEqual(first, second)
            self.assertEqual((root / 'first/manifest.json').read_bytes(), (root / 'second/manifest.json').read_bytes())
            self.assertEqual(first['count'], 27312)
            self.assertEqual(sorted(first['sorted_offsets']), list(range(27312)))
            self.assertGreater(first['source_order_inversions'], 0)
            with self.assertRaises(derive.IndexError):
                derive.generate(derive.SOURCE, derive.HEADER, root / 'first')
            self.assertEqual(json.loads((root / 'first/manifest.json').read_text()), first)

    def test_no_replace_publication(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            stage = root / 'stage'; stage.mkdir()
            output = root / 'output'; output.mkdir()
            with self.assertRaises(derive.IndexError):
                derive.publish(stage, output)
            self.assertTrue(stage.is_dir())
            self.assertTrue(output.is_dir())

if __name__ == '__main__':
    unittest.main()
