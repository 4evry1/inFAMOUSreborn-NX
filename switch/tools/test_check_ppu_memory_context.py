import json
from pathlib import Path
import tempfile
import unittest
import derive_ppu_memory_context as derive
import check_ppu_memory_context as checker
from test_derive_ppu_memory_context import HEADER,FIXTURES

class CheckerTests(unittest.TestCase):
    def setUp(self):
        self.folder=tempfile.TemporaryDirectory()
        self.root=Path(self.folder.name)
        self.source=self.root/"fixture.cpp"
        self.header=self.root/"header.h"
        self.source.write_bytes((FIXTURES/"input.cpp").read_bytes())
        self.header.write_text(HEADER)
        self.output=self.root/"derived"
        derive.derive_fixture(self.source,self.header,self.output)
    def tearDown(self):
        self.folder.cleanup()
    def check(self,allow=True):
        return checker.check_directory(self.output,allow_fixture=allow,fixture_source=self.source,fixture_header=self.header)
    def test_valid_fixture_and_explicit_fixture_boundary(self):
        self.assertEqual(self.check()["scope"],"fixture_only")
        with self.assertRaises(derive.DerivationError): self.check(False)
    def test_tampered_unchanged_span_is_rejected_even_with_updated_file_hash(self):
        path=self.output/"fixture.cpp"
        path.write_text(path.read_text().replace("preamble(int x)","preamble(int y)"))
        manifest=json.loads((self.output/"manifest.json").read_text())
        manifest["outputs"]["fixture.cpp"]["sha256"]=derive.sha(path.read_bytes())
        (self.output/"manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(derive.DerivationError): self.check()
    def test_forged_function_coverage_rejected(self):
        manifest = json.loads((self.output / "manifest.json").read_text())
        manifest["outputs"]["fixture.cpp"]["function_coverage"]["names"] = []
        (self.output / "manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(derive.DerivationError):
            self.check()

    def test_path_escape_and_unknown_files_rejected(self):
        manifest=json.loads((self.output/"manifest.json").read_text())
        manifest["outputs"]["../escape.cpp"]=manifest["outputs"].pop("fixture.cpp")
        (self.output/"manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(derive.DerivationError): self.check()
    def test_cpu_type_and_source_mutations_rejected(self):
        self.header.write_text(HEADER.replace("gpr[32]","gpr[31]"))
        with self.assertRaises(derive.DerivationError): self.check()
    def test_malicious_broad_edit_cannot_use_class_as_permission(self):
        manifest=json.loads((self.output/"manifest.json").read_text())
        record=manifest["outputs"]["fixture.cpp"]["edits"][1]
        record[4]="base_declaration"
        (self.output/"manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(derive.DerivationError): self.check()

if __name__ == "__main__": unittest.main()
