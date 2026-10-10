"""GVN differential execution, verifier, and optimization evidence."""

import argparse
import pathlib
import re
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


def counts(text):
    return {op: len(re.findall(pattern, text)) for op, pattern in {
        "add": r" = add i32 ", "mul": r" = mul i32 ",
        "fadd": r" = fadd float ", "gep": r" = getelementptr ", "pure": r"call i32 @pure\(",
        "input": r"call i32 @input\(", "read": r"call i32 @read\(",
        "phi": r" = phi ", "load": r" = load ",
    }.items()}


class GVNExecution(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jianmu gvn ")
        self.addCleanup(temporary.cleanup)
        self.directory = pathlib.Path(temporary.name)

    def command(self, command, data=None):
        result = subprocess.run([str(arg) for arg in command], cwd=self.directory,
                                input=data, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0,
                         f"{command}\n{result.stderr.decode(errors='replace')}")
        return result.stdout

    def compare(self, source, runs, reductions=(), preserved=()):
        source_path = self.directory / "program.cminus"
        source_path.write_text(source)
        evidence = {}
        modes = (("raw", []), ("mem2reg", ["-mem2reg"]),
                 ("gvn", ["-mem2reg", "-gvn"]),
                 ("combined", ["-mem2reg", "-const-prop", "-gvn", "-licm"]))
        for mode, flags in modes:
            output = self.directory / (mode + (".ll" if OPTIONS.target == "native" else ".s"))
            self.command([OPTIONS.compiler, "-emit-llvm" if OPTIONS.target == "native" else "-S",
                          *flags, "-o", output, source_path])
            ir = output if OPTIONS.target == "native" else output.with_suffix(".ll")
            self.assertTrue(ir.is_file(), "compiler did not produce IR")
            self.command([OPTIONS.opt, "-passes=verify", "-disable-output", ir])
            evidence[mode] = counts(ir.read_text())
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
        for op in reductions:
            self.assertLess(evidence["gvn"][op], evidence["mem2reg"][op],
                            f"expected {op} elimination: {evidence}")
        for op in preserved:
            self.assertEqual(evidence["gvn"][op], evidence["mem2reg"][op],
                             f"unexpected {op} elimination: {evidence}")
        print(f"{self._testMethodName}: mem2reg={evidence['mem2reg']} gvn={evidence['gvn']}")

    def test_direct_phi_codegen(self):
        self.command([OPTIONS.compiler.with_name("test_gvn"), "emit-phi-codegen"])
        for mode in ("raw", "gvn"):
            with self.subTest(mode=mode):
                ir = self.directory / (mode + ".ll")
                self.command([OPTIONS.opt, "-passes=verify", "-disable-output", ir])
                executable = self.directory / (mode + "-phi")
                if OPTIONS.target == "native":
                    self.command([OPTIONS.clang, "-O0", "-Wno-override-module", ir,
                                  OPTIONS.compiler.parent / "libcminus_io.a", "-o", executable])
                    launch = [executable]
                else:
                    self.command(["loongarch64-unknown-linux-gnu-gcc", "-static",
                                  self.directory / (mode + ".s"), REPO / "src/io/io.c",
                                  "-o", executable])
                    launch = ["qemu-loongarch64", executable]
                self.assertEqual(self.command(launch), b"3\n2\n1\n2.500000\n1.500000\n")

    def test_scalar_chain(self):
        self.compare("""
int main(void) {
 int a; int b; int p; int q; a=input(); b=input();
 p=a+b; q=a+b; output(p*3); output(q*3); return 0;
}
""", [(b"4\n7\n", b"33\n33\n")], reductions=("add", "mul"), preserved=("input",))

    def test_diamond_phi_of_operations(self):
        self.compare("""
int main(void) {
 int x; int y; int a; int b; int c;
 x=input(); y=input();
 if(input()) { a=x; b=y; c=x+y; }
 else { a=y; b=x; c=y+x; }
 output(c); output(a+b); return 0;
}
""", [(b"3\n8\n0\n", b"11\n11\n"), (b"3\n8\n1\n", b"11\n11\n")],
                     reductions=("add",))

    def test_loop_zero_one_many(self):
        self.compare("""
int main(void) {
 int n; int i; int s; int a; int b;
 n=input(); i=0; s=0;
 while(i<n) { a=i+2; b=i+2; s=s+a+b; i=i+1; }
 output(s); return 0;
}
""", [(b"0\n", b"0\n"), (b"1\n", b"4\n"), (b"5\n", b"40\n")],
                     reductions=("add",))

    def test_nested_loop_parallel_swap(self):
        self.compare("""
int main(void) {
 int n; int i; int j; int a; int b; int temp;
 n=input(); i=0; a=1; b=2;
 while(i<n) {
  j=0;
  while(j<3) { temp=a; a=b; b=temp; j=j+1; }
  i=i+1;
 }
 output(a); output(b); return 0;
}
""", [(b"0\n", b"1\n2\n"), (b"1\n", b"2\n1\n"),
       (b"5\n", b"2\n1\n")])

    def test_pure_and_effectful_calls(self):
        self.compare("""
int g;
int pure(int a) { return a*2; }
int read(void) { return g; }
int main(void) {
 int a; int b; a=input(); b=input();
 output(pure(a)); output(pure(a));
 g=a; output(read()); g=b; output(read()); return 0;
}
""", [(b"3\n9\n", b"6\n6\n3\n9\n")], reductions=("pure",), preserved=("read", "input"))

    def test_array_addresses_and_alias_store(self):
        self.compare("""
int change(int a[]) {
 a[0]=17; return a[0];
}
int main(void) {
 int a[4]; int i; i=input(); a[i]=5;
 output(a[i]); a[i]=9; output(a[i]); change(a); output(a[0]); return 0;
}
""", [(b"0\n", b"5\n9\n17\n"), (b"2\n", b"5\n9\n17\n")],
                     reductions=("gep",), preserved=("load",))

    def test_float_boundaries(self):
        self.compare("""
int main(void) {
 float x; float z; x=input(); z=0.0*(0.0-1.0);
 outputFloat(x+1.5); outputFloat(x+1.5); outputFloat(1.0/z);
 x=0.0/0.0; output(x==1.0); output(x!=1.0); return 0;
}
""", [(b"2\n", b"3.500000\n3.500000\n-inf\n1\n1\n")], reductions=("fadd",))


if __name__ == "__main__":
    required = ([OPTIONS.clang, OPTIONS.opt] if OPTIONS.target == "native" else
                ["loongarch64-unknown-linux-gnu-gcc", "qemu-loongarch64", OPTIONS.opt])
    for command in required:
        if not shutil.which(command):
            parser.error(f"required tool not found: {command}")
    for command in required:
        if shutil.which(command):
            version = subprocess.run([command, "--version"], capture_output=True, timeout=10)
            print(version.stdout.decode().splitlines()[0])
    unittest.main(argv=[sys.argv[0], *remaining])
