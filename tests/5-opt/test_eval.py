"""Tests for the reliable Lab5 evaluation entry point."""

import importlib.util
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("eval_lab5.py")


class EvaluatorTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), "Lab5 evaluator entry point is missing")
        spec = importlib.util.spec_from_file_location("eval_lab5", SCRIPT)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def test_nonzero_process_is_a_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(subprocess.CalledProcessError):
                self.module.checked_run([sys.executable, "-c", "print('answer'); raise SystemExit(3)"], pathlib.Path(directory))

    def test_process_handles_spaces_without_shell(self):
        with tempfile.TemporaryDirectory(prefix="lab5 paths ") as directory:
            result = self.module.checked_run([sys.executable, "-c", "import sys; print(sys.argv[1])", "argument with spaces"], pathlib.Path(directory))
            self.assertEqual(result.stdout, b"argument with spaces\n")

    def test_missing_testcases_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                self.module.find_cases(pathlib.Path(directory), "const-prop")

    def test_wrong_output_fails_even_when_exit_is_zero(self):
        with self.assertRaises(ValueError):
            self.module.check_output(b"wrong\n", b"answer\n")

    def test_void_main_gets_defined_host_exit_status(self):
        text = "define void @main() { ret void }\n"
        result = self.module.host_entry(text)
        self.assertIn("define void @__lab5_void_main()", result)
        self.assertIn("define i32 @main()", result)
        self.assertIn("ret i32 0", result)
        integer_main = "define i32 @main() { ret i32 3 }\n"
        self.assertEqual(self.module.host_entry(integer_main), integer_main)

    def test_duplicate_congruence_classes_do_not_pass(self):
        expected = [{"function": "main", "pout": {"entry": [["%a"]]}}]
        actual = [{"function": "main", "pout": {"entry": [["%a"], ["%a"]]}}]
        with self.assertRaises(ValueError):
            self.module.check_partitions(actual, expected)

    def test_boolean_and_integer_members_are_distinct(self):
        expected = [{"function": "main", "pout": {"entry": [[True]]}}]
        actual = [{"function": "main", "pout": {"entry": [[1]]}}]
        with self.assertRaises(ValueError):
            self.module.check_partitions(actual, expected)

    def test_stale_ir_cannot_turn_missing_compiler_output_into_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "case.cminus"
            source.write_text("int main(void) { return 0; }\n")
            old = root / "output/gvn-functional/mem2reg/case"
            old.mkdir(parents=True)
            (old / "program.ll").write_text("define i32 @main() { ret i32 0 }\n")
            # Real POSIX true exits zero without writing compiler artifacts.
            # The check must reject that, even when a previous run left valid IR.
            zero_exit = shutil.which("true")
            args = argparse.Namespace(output_dir=root / "output", mode="mem2reg",
                                      compiler=pathlib.Path(zero_exit), clang=zero_exit)
            with self.assertRaisesRegex(ValueError, "did not generate IR"):
                self.module.evaluate_case(args, "gvn-functional", source)


if __name__ == "__main__":
    unittest.main()
