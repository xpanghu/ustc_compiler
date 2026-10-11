#!/usr/bin/env python3
"""CLI and native LLVM/LoongArch differential checks; no timing claims."""

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
PROGRAMS = {
    "branch": ("""int main(void) {
        int x; int y; x = input(); y = input(); output(x + y);
        if (x < y) { output(x); } else { output(y); } return 0;
    }""", [(b"4\n7\n", b"11\n4\n"), (b"-3\n-8\n", b"-11\n-8\n")]),
    "loop_swap": ("""int main(void) {
        int a; int b; int t; int i; int n;
        a = 10; b = 20; i = 0; n = input();
        while (i < n) { t = a; a = b; b = t; i = i + 1; }
        output(a); output(b); return 0;
    }""", [(b"0\n", b"10\n20\n"), (b"1\n", b"20\n10\n"),
             (b"7\n", b"20\n10\n")]),
    "recursive": ("""int fib(int n) {
        if (n < 2) { return n; } else { return fib(n - 1) + fib(n - 2); }
    } int main(void) { output(fib(input())); return 0; }""",
                  [(b"0\n", b"0\n"), (b"1\n", b"1\n"), (b"9\n", b"34\n")]),
    "mixed_calls": ("""float mix(int x, float y, int z, float w) {
        return x + y + z + w;
    } int main(void) {
        outputFloat(mix(3, mix(1, 1.5, 2, 2.5), 4, 0.5)); return 0;
    }""", [(b"", b"14.500000\n")]),
    "array": ("""int data[6];
    int sum(int a[], int n) { int s; int i; s = 0; i = 0;
        while (i < n) { s = s + a[i]; i = i + 1; } return s; }
    int main(void) { int a[6]; int i; i = 0;
        while (i < 6) { a[i] = i * 3 - 8; data[i] = a[i]; i = i + 1; }
        output(sum(a, 6)); output(data[4]); return 0;
    }""", [(b"", b"-3\n4\n")]),
    "float_predicates": ("""int main(void) { float a; float b;
        a = 0.0 - 1.5; b = 0.0;
        output(a < b); output(a <= b); output(a > b); output(a >= b);
        output(a == b); output(a != b); outputFloat(a / 2.0); return 0;
    }""", [(b"", b"1\n1\n0\n0\n0\n1\n-0.750000\n")]),
    "float_branch": ("""int main(void) { float x; x = input();
        if (x < 0.0) { output(1); } else { output(0); } return 0;
    }""", [(b"-3\n", b"1\n"), (b"0\n", b"0\n"), (b"5\n", b"0\n")]),
    "immediates": ("""int main(void) { int x; x = input();
        output(x + 2047); output((0 - 2048) + x); output(x + 2048);
        output(x - 2048); output(x - (0 - 2048)); output(x - 2049); return 0;
    }""", [(b"-5\n", b"2042\n-2053\n2043\n-2053\n2043\n-2054\n")]),
    "large_frame": ("""int main(void) { int a[1024];
        a[0] = input(); a[1023] = a[0] + 17;
        output(a[0]); output(a[1023]); return 0;
    }""", [(b"-9\n", b"-9\n8\n")]),
    "void_main": ("void main(void) { output(42); }", [(b"", b"42\n")]),
}


def run(command, directory, data=None, checked=True):
    return subprocess.run([str(x) for x in command], cwd=directory, input=data,
                          capture_output=True, timeout=60, check=checked)


def require_tool(name):
    result = shutil.which(str(name))
    if result is None:
        raise ValueError(f"required tool is missing: {name}")
    return result


def host_entry(text):
    if "define void @main()" in text:
        text = text.replace("@main(", "@__backend_void_main(")
        text += "\ndefine i32 @main() {\n call void @__backend_void_main()\n ret i32 0\n}\n"
    return text


def metrics(stderr):
    values = {}
    for field in ("Allocate Size (bytes)", "Execute Cost"):
        match = re.search(re.escape(field) + r":\s*(\d+)", stderr.decode())
        if match is None:
            raise ValueError(f"missing metrics field {field!r}")
        values[field] = int(match.group(1))
    return values


def check_cli(args, directory):
    source = directory / "cli.cminus"
    source.write_text("int main(void) { return 0; }\n")
    bad = [
        ["-S", "-backend=unknown"], ["-S", "-backend=mir", "-isel=unknown"],
        ["-emit-llvm", "-backend=mir"], ["-emit-ast", "-backend=mir"],
        ["-S", "-backend=legacy", "-isel=baseline"], ["-S", "-dump-mir"],
        ["-S", "-lab4-metrics"], ["-emit-llvm", "-backend=legacy"],
        ["-S", "-emit-llvm", "-backend=mir"],
    ]
    for index, flags in enumerate(bad):
        output = directory / f"bad{index}.s"
        result = run([args.compiler, *flags, "-o", output, source], directory, checked=False)
        if result.returncode == 0 or list(directory.glob(f"bad{index}.*")):
            raise ValueError(f"CLI accepted invalid flags or left outputs: {flags}")
    output = directory / "valid.s"
    run([args.compiler, "-S", "-backend=mir", "-isel=baseline", "-dump-mir",
         "-o", output, source], directory)
    for suffix in (".s", ".selected.mir", ".prera.mir", ".final.mir"):
        if not output.with_suffix(suffix).is_file():
            raise ValueError(f"missing backend artifact: {suffix}")
    selected = output.with_suffix(".selected.mir").read_text()
    final = output.with_suffix(".final.mir").read_text()
    if selected == final:
        raise ValueError("stage dumps were captured after mutation")
    for mode, flags in (("default", []), ("legacy", ["-backend=legacy"]),
                        ("mir_default", ["-backend=mir"])):
        run([args.compiler, "-S", *flags, "-o", directory / f"{mode}.s", source], directory)
    if (directory / "default.s").read_text() != (directory / "legacy.s").read_text():
        raise ValueError("default backend behavior changed")
    if (directory / "mir_default.s").read_text() != output.read_text():
        raise ValueError("MIR default selection is not baseline")
    if pathlib.Path("/dev/full").exists():
        full = directory / "full.s"
        full.symlink_to("/dev/full")
        result = run([args.compiler, "-S", "-backend=mir", "-o", full, source], directory, checked=False)
        if result.returncode == 0:
            raise ValueError("buffered output failure returned success")
    source.write_text("int ninth(int a,int b,int c,int d,int e,int f,int g,int h,int i) { return i; }\n"
                      "int main(void) { return ninth(1,2,3,4,5,6,7,8,9); }\n")
    output = directory / "unsupported.s"
    result = run([args.compiler, "-S", "-backend=mir", "-dump-mir", "-o", output, source],
                 directory, checked=False)
    if result.returncode == 0 or list(directory.glob("unsupported.*")):
        raise ValueError("unsupported ABI did not fail before artifact creation")
    print("PASS CLI: valid stages and 10 rejection cases", flush=True)


def build_case(args, directory, source, flags):
    ir = directory / "reference.ll"
    run([args.compiler, "-emit-llvm", *flags, "-o", ir, source], directory)
    run([args.opt, "-passes=verify", "-disable-output", ir], directory)
    ir.write_text(host_entry(ir.read_text()))
    native = directory / "native"
    run([args.clang, "-O0", "-Wno-override-module", ir, REPO / "src/io/io.c",
         "-o", native], directory)
    binaries = {"native": [native]}
    counts = {}
    modes = [("legacy", ["-backend=legacy"]),
             ("baseline", ["-backend=mir", "-isel=baseline", "-dump-mir"])]
    if not args.baseline_only:
        modes.append(("greedy", ["-backend=mir", "-isel=greedy", "-dump-mir"]))
    for mode, options in modes:
        asm = directory / f"{mode}.s"
        run([args.compiler, "-S", *flags, *options, "-o", asm, source], directory)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, REPO / "src/io/io.c", "-o", exe], directory)
        binaries[mode] = [args.qemu, exe]
        if mode != "legacy":
            counts[mode] = {
                stage: sum(2 if stage == "final" and line.startswith("  LA_LOCAL ") else 1
                           for line in asm.with_suffix(f".{stage}.mir").read_text().splitlines()
                           if re.match(r"  [A-Z][A-Z0-9_]*(?: |$)", line))
                for stage in ("selected", "final")
            }
    return binaries, counts


def evaluate(args, directory, name, source_text, inputs, flags):
    directory.mkdir()
    source = directory / f"{name}.cminus"
    source.write_text(source_text)
    binaries, counts = build_case(args, directory, source, flags)
    for data, expected in inputs:
        reference = None
        for mode, command in binaries.items():
            result = run(command, directory, data, checked=False)
            actual = (result.stdout, result.returncode)
            if mode == "native":
                reference = actual
                if expected is not None:
                    matches = actual == expected if isinstance(expected, tuple) else result.stdout == expected
                    if not matches:
                        raise ValueError(f"{name}: reference result {actual!r} != {expected!r}")
            elif actual != reference:
                raise ValueError(f"{name} [{mode}]: {actual!r} != native {reference!r}")
    print(f"PASS {name}: {len(inputs)} inputs, {len(binaries)} execution paths", flush=True)
    return {"case": name, "inputs": len(inputs), "counts": counts}


def evaluate_metrics(args, directory):
    directory.mkdir()
    source = directory / "metrics.cminus"
    source.write_text("int main(void) { int a[4]; int x; x = input(); a[2] = x * 3; "
                      "output(a[2] / 2); return 0; }\n")
    observations = {}
    for mode in ("legacy", "baseline", "greedy"):
        if args.baseline_only and mode == "greedy":
            continue
        asm = directory / f"{mode}.s"
        flags = ["-backend=legacy"] if mode == "legacy" else ["-backend=mir", f"-isel={mode}", "-lab4-metrics"]
        run([args.compiler, "-S", "-mem2reg", *flags, "-o", asm, source], directory)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, REPO / "src/io/io.c", "-o", exe], directory)
        result = run([args.qemu, exe], directory, b"8\n")
        if result.stdout != b"12\n":
            raise ValueError("metrics changed program semantics")
        observations[mode] = metrics(result.stderr)
    if any(value != observations["legacy"] for value in observations.values()):
        raise ValueError(f"metrics differ: {observations}")
    print(f"PASS metrics: {observations}", flush=True)
    return observations


def evaluate_c_entry(args, directory):
    directory.mkdir()
    source = directory / "callee.cminus"
    source.write_text("float exported(int x, int a[], float y, int z, float w) { return x + a[1] + y + z + w; }\n"
                      "int pred(float a[], int i, int j) { return (a[i] < a[j]) + 2 * (a[i] <= a[j]) + "
                      "4 * (a[i] > a[j]) + 8 * (a[i] >= a[j]) + 16 * (a[i] == a[j]) + 32 * (a[i] != a[j]); }\n")
    harness = directory / "harness.c"
    harness.write_text('#include <stdio.h>\n#include <math.h>\n'
                       'extern float exported(int, int*, float, int, float);\nextern int pred(float*,int,int);\n'
                       'int main(void) { int a[]={7,-2}; float f[]={NAN,INFINITY,-INFINITY,-0.0f,0.0f,1.0f,-1.0f}; '
                       'printf("%.6f\\n", exported(3,a,1.5,4,0.5)); '
                       'for(int i=0;i<7;++i) for(int j=0;j<7;++j) printf("%d ",pred(f,i,j)); '
                       'puts(""); return 0; }\n')
    ir = directory / "callee.ll"
    # Whole-program DCE roots at main; this translation unit is a C-callable library.
    run([args.compiler, "-emit-llvm", "-o", ir, source], directory)
    run([args.opt, "-passes=verify", "-disable-output", ir], directory)
    native = directory / "native"
    run([args.clang, "-Wno-override-module", ir, harness, REPO / "src/io/io.c", "-o", native], directory)
    reference = run([native], directory).stdout
    rows = reference.splitlines()
    values = rows[1].split() if len(rows) == 2 else []
    if not rows or rows[0] != b"7.000000" or len(values) != 49 or values[:7] != [b"63"] * 7 or values[3*7+4] != b"26":
        raise ValueError(f"C interoperability reference failed: {reference!r}")
    for mode in ("baseline", "greedy"):
        if args.baseline_only and mode == "greedy":
            continue
        asm = directory / f"{mode}.s"
        run([args.compiler, "-S", "-backend=mir", f"-isel={mode}", "-o", asm, source], directory)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, harness, REPO / "src/io/io.c", "-o", exe], directory)
        if run([args.qemu, exe], directory).stdout != reference:
            raise ValueError(f"C caller to MIR callee ABI failed [{mode}]")
    print("PASS C caller → MIR callee: integer/pointer/float parameters", flush=True)


def evaluate_c_helpers(args, directory):
    directory.mkdir()
    generator = pathlib.Path(args.compiler).with_name("test_machine_codegen")
    require_tool(generator)
    run([generator, "emit-interop", directory], directory)
    harness = directory / "helpers.c"
    harness.write_text('#include <stdio.h>\n'
                       'int c_mix_i(int x,float y,int*p) { return x+(int)y+p[1]; }\n'
                       'float c_mix_f(int x,float y,int*p) { return (float)x+y+(float)p[0]; }\n'
                       '_Bool c_readbit(unsigned char*p) { return *p & 1; }\n'
                       'extern int mir_call_i(int,float,int*);\n'
                       'extern float mir_call_f(int,float,int*);\n'
                       'extern int mir_readbit(unsigned char*);\n'
                       'extern _Bool mir_identity(_Bool);\nextern int mir_call_bit(unsigned char*);\n'
                       'int main(void) { int p[]={7,-2}; unsigned char bits[]={2,3}; '
                       'printf("%d %.6f %d %d %d %d %d %d\\n", mir_call_i(3,1.5f,p), mir_call_f(3,1.5f,p), '
                       'mir_readbit(bits), mir_readbit(bits+1), mir_identity(0), mir_identity(1), '
                       'mir_call_bit(bits), mir_call_bit(bits+1)); return 0; }\n')
    ir = directory / "interop.ll"
    run([args.opt, "-passes=verify", "-disable-output", ir], directory)
    native = directory / "native"
    run([args.clang, "-Wno-override-module", ir, harness, "-o", native], directory)
    reference = run([native], directory).stdout
    if reference != b"13 13.500000 0 1 0 1 0 1\n":
        raise ValueError(f"external helper reference failed: {reference!r}")
    for mode in ("baseline", "greedy"):
        if args.baseline_only and mode == "greedy":
            continue
        asm = directory / f"interop.{mode}.s"
        exe = directory / mode
        run([args.cross_cc, "-static", asm, harness, "-o", exe], directory)
        if run([args.qemu, exe], directory).stdout != reference:
            raise ValueError(f"MIR caller to C helpers ABI failed [{mode}]")
    print("PASS MIR caller → C helpers: mixed arguments/returns across clobbers", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=pathlib.Path, default=REPO / "build/cminusfc")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--opt", default="opt")
    parser.add_argument("--cross-cc", default="loongarch64-unknown-linux-gnu-gcc")
    parser.add_argument("--qemu", default="qemu-loongarch64")
    parser.add_argument("--cli-only", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--course", choices=("lab3", "lab4"))
    parser.add_argument("--mode", choices=("raw", "mem2reg", "const-prop", "gvn", "licm", "all"), default="mem2reg")
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args(argv)
    args.flags = [] if args.mode == "raw" else ["-mem2reg"]
    if args.mode in ("const-prop", "gvn", "licm"):
        args.flags.append("-" + args.mode)
    elif args.mode == "all":
        args.flags.extend(["-const-prop", "-gvn", "-licm"])
    args.compiler = args.compiler.resolve()
    try:
        args.compiler = require_tool(args.compiler)
        if not args.cli_only:
            for name in ("clang", "opt", "cross_cc", "qemu"):
                setattr(args, name, require_tool(getattr(args, name)))
        with tempfile.TemporaryDirectory(prefix="jianmu backend ") as temp:
            directory = pathlib.Path(temp)
            check_cli(args, directory)
            if args.cli_only:
                return 0
            records = []
            if args.course:
                root = REPO / ("tests/3-codegen/autogen/testcases" if args.course == "lab3" else "tests/4-opt/testcases")
                for source in sorted(root.glob("*.cminus")):
                    data = source.with_suffix(".in").read_bytes() if source.with_suffix(".in").exists() else b""
                    golden = source.with_suffix(".out").read_bytes()
                    # Lab3/4 goldens append the process exit code to stdout.
                    lines = golden.splitlines(keepends=True)
                    expected = (b"".join(lines[:-1]), int(lines[-1]))
                    records.append(evaluate(args, directory / source.stem, source.stem,
                                            source.read_text(), [(data, expected)], args.flags))
            else:
                for name, (source, inputs) in PROGRAMS.items():
                    records.append(evaluate(args, directory / name, name, source, inputs, args.flags))
                evaluate_c_entry(args, directory / "c_entry")
                evaluate_c_helpers(args, directory / "c_helpers")
                observations = evaluate_metrics(args, directory / "metrics")
            report = {"cases": records, "modes": 3 if args.baseline_only else 4}
            if not args.course:
                report["metrics"] = observations
            if args.report:
                args.report.write_text(json.dumps(report, indent=2) + "\n")
            print(f"PASS differential matrix: {len(records)} programs", flush=True)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr.decode(errors="replace")[-4000:], file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
