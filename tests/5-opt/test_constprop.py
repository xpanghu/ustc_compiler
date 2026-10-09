"""ConstPropagation differential execution and IR verification (standard library only)."""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", type=pathlib.Path, default=REPO / "build/cminusfc")
parser.add_argument("--target", choices=("native", "loongarch"), default="native")
parser.add_argument("--clang", default="clang")
parser.add_argument("--opt", default="opt")
OPTIONS, remaining = parser.parse_known_args()
OPTIONS.compiler = OPTIONS.compiler.resolve()


class ConstPropagationExecution(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jianmu constprop ")
        self.addCleanup(self.tmp.cleanup)
        self.directory = pathlib.Path(self.tmp.name)

    def command(self, command, data=None):
        result = subprocess.run([str(arg) for arg in command], cwd=self.directory, input=data,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, f"{command}\n{result.stderr.decode(errors='replace')}")
        return result.stdout

    def compare(self, source, runs, absent=(), present=(), mem2reg=True):
        source_path = self.directory / "program.cminus"
        source_path.write_text(source)
        for mode, flags in (("raw", []), ("optimized", (["-mem2reg"] if mem2reg else []) + ["-const-prop"])):
            suffix = ".ll" if OPTIONS.target == "native" else ".s"
            output = self.directory / (mode + suffix)
            emit = "-emit-llvm" if OPTIONS.target == "native" else "-S"
            self.command([OPTIONS.compiler, emit, *flags, "-o", output, source_path])
            self.assertTrue(output.is_file(), "compiler did not produce an artifact")
            ir = output if OPTIONS.target == "native" else output.with_suffix(".ll")
            self.command([OPTIONS.opt, "-passes=verify", "-disable-output", ir])
            if mode == "optimized":
                text = ir.read_text()
                for pattern in absent:
                    self.assertNotIn(pattern, text)
                for pattern in present:
                    self.assertIn(pattern, text)
            executable = self.directory / mode
            if OPTIONS.target == "native":
                self.command([OPTIONS.clang, "-O0", "-Wno-override-module", output,
                              OPTIONS.compiler.parent / "libcminus_io.a", "-o", executable])
                launch = [executable]
            else:
                self.command(["loongarch64-unknown-linux-gnu-gcc", "-static", output,
                              REPO / "src/io/io.c", "-o", executable])
                launch = ["qemu-loongarch64", executable]
            for data, expected in runs:
                with self.subTest(mode=mode, input=data):
                    self.assertEqual(self.command(launch, data), expected)

    def test_scalar_arithmetic_comparison_conversions(self):
        self.compare("""
int main(void) {
    int i; float f;
    i = (2 + 3) * 4;
    f = (i + (3 < 4)) / 2.0;
    i = f;
    output(i);
    outputFloat((9.0 - 1.0) * 2.0 / 4.0);
    return 0;
}
""", [(b"", b"10\n4.000000\n")],
                     absent=(" = add ", " = mul ", " = icmp ", " = fadd ", " = fsub ",
                             " = fmul ", " = fdiv ", " = fptosi ", " = sitofp ", " = zext "))

    def test_constant_branch_phi_and_fixedpoint(self):
        self.compare("""
int main(void) {
    int a;
    if (2 < 3) { a = 5; } else { a = 99; }
    if (a == 5) { output(a * 4); } else { output(0); }
    return 0;
}
""", [(b"", b"20\n")], absent=("br i1", " = phi ", " = icmp ", " = mul "))

    def test_dead_loop_and_nested_control_flow(self):
        self.compare("""
int main(void) {
    if (0) { while (1) { output(0); } }
    else { if (1) { output(99); } }
    return 0;
}
""", [(b"", b"99\n")], absent=("br i1", "call void @output(i32 0)"))

    def test_global_forwarding_and_call_invalidation(self):
        self.compare("""
int g;
int change(void) { g = 7; return 0; }
int main(void) {
    int a;
    g = 5;
    a = g;
    change();
    output(a + g);
    return 0;
}
""", [(b"", b"12\n")], present=("load i32", "store i32 5", "call i32 @change"))

    def test_runtime_argument_and_loop_phi(self):
        self.compare("""
int sum(int n) {
    int i; int s;
    i = 0; s = 0;
    while (i < n) { s = s + i; i = i + 1; }
    return s;
}
int main(void) { output(sum(input())); return 0; }
""", [(b"0\n", b"0\n"), (b"1\n", b"0\n"), (b"5\n", b"10\n")],
                     present=("br i1", "phi i32"))

    def test_unknown_condition_phi(self):
        self.compare("""
int main(void) {
    int a;
    if (input()) { a = 10; } else { a = 20; }
    output(a);
    return 0;
}
""", [(b"0\n", b"20\n"), (b"1\n", b"10\n")], present=("br i1", "phi i32"))

    def test_no_cross_block_or_global_initializer_assumption(self):
        self.compare("""
int g;
int update(int flag) {
    if (flag) { g = 10; } else { g = 20; }
    return g;
}
int main(void) { output(update(1)); output(update(0)); return 0; }
""", [(b"", b"10\n20\n")], present=("load i32", "br i1"))

    def test_signed_zero_is_preserved(self):
        self.compare("""
int main(void) {
    float x;
    x = 0.0 * (0.0 - 1.0);
    outputFloat(1.0 / x);
    return 0;
}
""", [(b"", b"-inf\n")], absent=(" = fmul ", " = fdiv ", " = fsub "))

    def test_nan_comparisons_follow_lightir(self):
        self.compare("""
int main(void) {
    float x;
    x = 0.0 / 0.0;
    output(x < 1.0);
    output(x <= 1.0);
    output(x > 1.0);
    output(x >= 1.0);
    output(x == 1.0);
    output(x != 1.0);
    output(1.0 < x);
    output(1.0 <= x);
    output(1.0 > x);
    output(1.0 >= x);
    output(1.0 == x);
    output(1.0 != x);
    return 0;
}
""", [(b"", b"1\n" * 12)], absent=(" = fcmp ", " = fdiv "))

    def test_finite_runtime_float_comparisons(self):
        self.compare("""
int main(void) {
    float x;
    x = input();
    output(x < 2.0);
    output(x <= 2.0);
    output(x > 2.0);
    output(x >= 2.0);
    output(x == 2.0);
    output(x != 2.0);
    return 0;
}
""", [(b"-9\n", b"1\n1\n0\n0\n0\n1\n"),
       (b"2\n", b"0\n1\n0\n1\n1\n0\n"),
       (b"3\n", b"0\n0\n1\n1\n0\n1\n")], present=(" = fcmp ",))

    def test_without_mem2reg(self):
        self.compare("int main(void) { int a; a = (2 + 3) * 4; output(a); return 0; }",
                     [(b"", b"20\n")], absent=(" = add ", " = mul "), mem2reg=False)


if __name__ == "__main__":
    for command in ([OPTIONS.clang, OPTIONS.opt] if OPTIONS.target == "native"
                    else ["loongarch64-unknown-linux-gnu-gcc", "qemu-loongarch64", OPTIONS.opt]):
        if not shutil.which(command):
            parser.error(f"required tool not found: {command}")
    unittest.main(argv=[sys.argv[0], *remaining])
