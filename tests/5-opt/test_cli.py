"""Compiler CLI regression for the Lab5 scaffold (no third-party packages)."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

COMPILER = pathlib.Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2] / "build/cminusfc"


class Lab5CLI(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jianmu lab5 ")
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.source = self.root / "small program.cminus"
        self.source.write_text("int main(void) { return (2 + 3) * 4; }\n")
        self.output = self.root / "result.ll"

    def compile(self, *flags):
        return subprocess.run([str(COMPILER), "-emit-llvm", *flags, "-o", str(self.output), str(self.source)], capture_output=True, timeout=10, cwd=self.root)

    def test_help_lists_lab5_options(self):
        result = subprocess.run([str(COMPILER), "--help"], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        for flag in (b"-const-prop", b"-gvn", b"-dump-json"):
            self.assertIn(flag, result.stdout)

    def test_provided_integer_folding_baseline(self):
        raw = self.compile()
        self.assertEqual(raw.returncode, 0, raw.stderr)
        self.assertIn("mul i32", self.output.read_text())
        result = self.compile("-mem2reg", "-const-prop")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ret i32 20", self.output.read_text())
        self.assertNotIn("mul i32", self.output.read_text())

    def test_gvn_succeeds_with_stable_strict_json(self):
        self.source.write_text("int main(void) { float x; x = 1.5; outputFloat(x); return (2 + 3) * 4; }\n")
        result = self.compile("-mem2reg", "-gvn", "-dump-json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ret i32 20", self.output.read_text())
        dump = self.root / "gvn.json"
        first = dump.read_text()
        self.assertTrue(json.loads(first))
        result = self.compile("-mem2reg", "-gvn", "-dump-json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(first, dump.read_text())

    def test_unwritable_json_target_fails_without_output(self):
        (self.root / "gvn.json").mkdir()
        result = self.compile("-mem2reg", "-gvn", "-dump-json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"gvn.json", result.stderr)
        self.assertFalse(self.output.exists())

    def test_json_requires_gvn(self):
        result = self.compile("-dump-json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"dump-json must be used with gvn", result.stdout + result.stderr)

    def test_gvn_requires_mem2reg(self):
        result = self.compile("-gvn")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"gvn must be used with mem2reg", result.stdout + result.stderr)

    def test_ast_does_not_silently_ignore_gvn(self):
        result = self.compile("-emit-ast", "-mem2reg", "-gvn")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"cannot be used with emit-ast", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
