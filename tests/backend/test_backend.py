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


def run(command, directory, data=None, checked=True, *, timeout=60):
    return subprocess.run([str(x) for x in command], cwd=directory, input=data,
                          capture_output=True, timeout=timeout, check=checked)


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
    # Exercise the new positive CLI contract before checking the other artifacts.
    linear = directory / "linear.s"
    run([args.compiler, "-S", "-backend=mir", "-regalloc=linear", "-dump-mir",
         "-o", linear, source], directory, timeout=args.timeout)
    bad = [
        ["-S", "-backend=unknown"], ["-S", "-backend=mir", "-isel=unknown"],
        ["-emit-llvm", "-backend=mir"], ["-emit-ast", "-backend=mir"],
        ["-S", "-backend=legacy", "-isel=baseline"], ["-S", "-dump-mir"],
        ["-S", "-lab4-metrics"], ["-emit-llvm", "-backend=legacy"],
        ["-S", "-emit-llvm", "-backend=mir"],
        ["-S", "-backend=mir", "-regalloc=unknown"],
        ["-S", "-backend=mir", "-regalloc="],
        ["-S", "-backend=legacy", "-regalloc=linear"],
        ["-S", "-regalloc=stack"],
        ["-emit-llvm", "-backend=mir", "-regalloc=linear"],
        ["-emit-ast", "-backend=mir", "-regalloc=stack"],
        ["-S", "-emit-ast", "-backend=mir", "-regalloc=linear"],
        ["-regalloc=unknown", "-S", "-backend=mir"],
    ]
    for index, flags in enumerate(bad):
        output = directory / f"bad{index}.s"
        result = run([args.compiler, *flags, "-o", output, source], directory, checked=False, timeout=args.timeout)
        if result.returncode == 0 or list(directory.glob(f"bad{index}.*")):
            raise ValueError(f"CLI accepted invalid flags or left outputs: {flags}")
    output = directory / "valid.s"
    run([args.compiler, "-S", "-backend=mir", "-isel=baseline", "-dump-mir",
         "-o", output, source], directory, timeout=args.timeout)
    for suffix in (".s", ".ll", ".selected.mir", ".prera.mir", ".allocated.mir", ".final.mir"):
        if not output.with_suffix(suffix).is_file():
            raise ValueError(f"missing backend artifact: {suffix}")
    if output.with_suffix(".regalloc.txt").exists():
        raise ValueError("stack mode emitted a linear allocation report")
    selected = output.with_suffix(".selected.mir").read_text()
    final = output.with_suffix(".final.mir").read_text()
    if selected == final:
        raise ValueError("stage dumps were captured after mutation")
    for mode, flags in (("default", []), ("legacy", ["-backend=legacy"]),
                        ("mir_default", ["-backend=mir"]),
                        ("mir_stack", ["-backend=mir", "-regalloc=stack"])):
        run([args.compiler, "-S", *flags, "-o", directory / f"{mode}.s", source], directory, timeout=args.timeout)
    if (directory / "default.s").read_text() != (directory / "legacy.s").read_text():
        raise ValueError("default backend behavior changed")
    if (directory / "mir_default.s").read_text() != output.read_text():
        raise ValueError("MIR default selection is not baseline")
    if (directory / "mir_stack.s").read_text() != output.read_text():
        raise ValueError("explicit stack differs from MIR default")
    source.write_text("int zfirst(int x) { while (x < 4) { x = x + 1; } return x; }\n"
                      "int main(void) { int x; x = input(); output(zfirst(x)); return x + 2; }\n")
    for isel in ("baseline", "greedy"):
        snapshots = []
        for stem in ("first", "second"):
            repeated = directory / f"{isel}_{stem}.s"
            run([args.compiler, "-S", "-backend=mir", f"-isel={isel}", "-regalloc=linear",
                 "-dump-mir", "-o", repeated, source], directory, timeout=args.timeout)
            snapshots.append([repeated.with_suffix(suffix).read_text() for suffix in
                              (".s", ".ll", ".selected.mir", ".prera.mir", ".allocated.mir",
                               ".final.mir", ".regalloc.txt")])
        if snapshots[0] != snapshots[1]:
            raise ValueError("linear snapshots/report depend on output stem")
        report = snapshots[0][-1]
        if "stats retained=" not in report or "frame_size=" not in report or "0x" in report:
            raise ValueError("missing allocation statistics or unstable address in report")
        allocated = snapshots[0][-3]
        if ("frame=0" not in allocated or allocated == snapshots[0][-2] or
                re.search(r"^  (?!fi)[^\n]+ fi[0-9]+", snapshots[0][-2], re.MULTILINE)):
            raise ValueError("allocated snapshot was captured after frame lowering")
    no_dump = directory / "no_dump.s"
    run([args.compiler, "-S", "-backend=mir", "-regalloc=linear", "-o", no_dump, source], directory, timeout=args.timeout)
    if any(no_dump.with_suffix(suffix).exists() for suffix in
           (".selected.mir", ".prera.mir", ".allocated.mir", ".final.mir", ".regalloc.txt")):
        raise ValueError("linear debug artifacts emitted without dump-mir")
    if pathlib.Path("/dev/full").exists():
        full = directory / "full.s"
        full.symlink_to("/dev/full")
        result = run([args.compiler, "-S", "-backend=mir", "-o", full, source], directory, checked=False, timeout=args.timeout)
        if result.returncode == 0:
            raise ValueError("buffered output failure returned success")
    source.write_text("int ninth(int a,int b,int c,int d,int e,int f,int g,int h,int i) { return i; }\n"
                      "int main(void) { return ninth(1,2,3,4,5,6,7,8,9); }\n")
    for mode in ("stack", "linear"):
        output = directory / f"unsupported_{mode}.s"
        result = run([args.compiler, "-S", "-backend=mir", f"-regalloc={mode}", "-dump-mir",
                      "-o", output, source], directory, checked=False, timeout=args.timeout)
        if result.returncode == 0 or list(directory.glob(f"unsupported_{mode}.*")):
            raise ValueError("unsupported ABI did not fail before artifact creation")
    print(f"PASS CLI: reproducible stages/reports and {len(bad) + 2} rejection cases", flush=True)


def mir_modes(args):
    selectors = ("baseline",) if args.baseline_only else ("baseline", "greedy")
    allocators = ("stack", "linear") if args.regalloc == "both" else (args.regalloc,)
    return [(f"{isel}_{allocator}",
             ["-backend=mir", f"-isel={isel}", f"-regalloc={allocator}", "-dump-mir"])
            for isel in selectors for allocator in allocators]


def allocation_counts(asm):
    stages = {stage: asm.with_suffix(f".{stage}.mir").read_text()
              for stage in ("selected", "prera", "allocated", "final")}
    counts = {stage: sum(2 if stage == "final" and line.startswith("  LA_LOCAL ") else 1
                        for line in text.splitlines()
                        if re.match(r"  [A-Z][A-Z0-9_]*(?: |$)", line))
              for stage, text in stages.items() if stage in ("selected", "final")}
    functions = {}
    for stage in ("prera", "allocated", "final"):
        for match in re.finditer(r"^function @(\S+) frame=(\d+)\n(.*?)(?=^function @|\Z)",
                                 stages[stage], re.MULTILINE | re.DOTALL):
            name, frame, body = match.groups()
            row = functions.setdefault(name, {})
            objects = len(re.findall(r"^  fi\d+ size=", body, re.MULTILINE))
            if stage == "prera":
                row["alloca_objects"] = objects
            elif stage == "allocated":
                row["spilled"] = objects - row["alloca_objects"]
                row["reloads"] = row["stores"] = 0
                for opcode, slot in re.findall(r"^  (LD_W|LD_D|FLD_S|ST_W|ST_D|FST_S) .* fi(\d+)",
                                              body, re.MULTILINE):
                    if int(slot) >= row["alloca_objects"]:
                        row["reloads" if opcode in ("LD_W", "LD_D", "FLD_S") else "stores"] += 1
            else:
                row["frame_size"] = int(frame)
                row["callee_saved"] = objects - row["alloca_objects"] - row["spilled"] - 2
                # The unified entry/exit always saves/restores ra and fp as well.
                row["abi_saves"] = row["abi_restores"] = 2 + row["callee_saved"]
    counts["functions"] = functions
    counts["allocation"] = {field: sum(row[field] for row in functions.values())
                            for field in ("spilled", "reloads", "stores", "frame_size",
                                          "callee_saved", "abi_saves", "abi_restores")}
    return counts


def build_case(args, directory, source, flags):
    ir = directory / "reference.ll"
    run([args.compiler, "-emit-llvm", *flags, "-o", ir, source], directory, timeout=args.timeout)
    run([args.opt, "-passes=verify", "-disable-output", ir], directory, timeout=args.timeout)
    ir.write_text(host_entry(ir.read_text()))
    native = directory / "native"
    run([args.clang, "-O0", "-Wno-override-module", ir, REPO / "src/io/io.c",
         "-o", native], directory, timeout=args.timeout)
    binaries = {"native": [native]}
    counts = {}
    modes = [("legacy", ["-backend=legacy"])] + mir_modes(args)
    for mode, options in modes:
        asm = directory / f"{mode}.s"
        run([args.compiler, "-S", *flags, *options, "-o", asm, source], directory, timeout=args.timeout)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, REPO / "src/io/io.c", "-o", exe], directory, timeout=args.timeout)
        binaries[mode] = [args.qemu, exe]
        if mode != "legacy":
            counts[mode] = allocation_counts(asm)
    return binaries, counts


def evaluate(args, directory, name, source_text, inputs, flags):
    directory.mkdir()
    source = directory / f"{name}.cminus"
    source.write_text(source_text)
    binaries, counts = build_case(args, directory, source, flags)
    completed = []
    for data, expected in inputs:
        reference = None
        for mode, command in binaries.items():
            result = run(command, directory, data, checked=False, timeout=args.timeout)
            actual = (result.stdout.replace(b"\r\n", b"\n"), result.returncode)
            if mode == "native":
                reference = actual
                if expected is not None:
                    matches = actual == expected if isinstance(expected, tuple) else actual == (expected, 0)
                    if not matches:
                        raise ValueError(f"{name}: reference result {actual!r} != {expected!r}")
            elif actual != reference:
                raise ValueError(f"{name} [{mode}]: {actual!r} != native {reference!r}")
        completed.append(list(binaries))
    print(f"PASS {name}: {len(inputs)} inputs, {len(binaries)} execution paths", flush=True)
    return {"case": name, "inputs": len(inputs), "paths": completed, "counts": counts}


def evaluate_metrics(args, directory):
    directory.mkdir()
    source = directory / "metrics.cminus"
    source.write_text("int main(void) { int a[4]; int x; x = input(); a[2] = x * 3; "
                      "output(a[2] / 2); return 0; }\n")
    observations = {}
    modes = [("legacy", ["-backend=legacy"])] + [
        (name, options + ["-lab4-metrics"]) for name, options in mir_modes(args)]
    for mode, flags in modes:
        asm = directory / f"{mode}.s"
        run([args.compiler, "-S", "-mem2reg", *flags, "-o", asm, source], directory, timeout=args.timeout)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, REPO / "src/io/io.c", "-o", exe], directory, timeout=args.timeout)
        result = run([args.qemu, exe], directory, b"8\n", timeout=args.timeout)
        if result.stdout != b"12\n":
            raise ValueError("metrics changed program semantics")
        observations[mode] = metrics(result.stderr)
    if any(value != observations["legacy"] for value in observations.values()):
        raise ValueError(f"metrics differ: {observations}")
    print(f"PASS metrics: {len(observations)} target paths, {observations}", flush=True)
    return observations


def evaluate_c_entry(args, directory):
    directory.mkdir()
    source = directory / "callee.cminus"
    source.write_text("float recur(int n, int a[], float y) { if (n < 1) { return a[0] + y; } "
                      "else { return y + recur(n - 1, a, y + 1.0); } }\n"
                      "float exported(int x, int a[], float y, int z, float w) { "
                      "return x + a[1] + y + z + w + recur(2, a, 1.0); }\n"
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
    run([args.compiler, "-emit-llvm", "-o", ir, source], directory, timeout=args.timeout)
    run([args.opt, "-passes=verify", "-disable-output", ir], directory, timeout=args.timeout)
    native = directory / "native"
    run([args.clang, "-Wno-override-module", ir, harness, REPO / "src/io/io.c", "-o", native], directory, timeout=args.timeout)
    reference = run([native], directory, timeout=args.timeout).stdout
    rows = reference.splitlines()
    values = rows[1].split() if len(rows) == 2 else []
    if not rows or rows[0] != b"20.000000" or len(values) != 49 or values[:7] != [b"63"] * 7 or values[3*7+4] != b"26":
        raise ValueError(f"C interoperability reference failed: {reference!r}")
    completed = ["native"]
    for mode, options in mir_modes(args):
        asm = directory / f"{mode}.s"
        run([args.compiler, "-S", *options, "-o", asm, source], directory, timeout=args.timeout)
        exe = directory / mode
        run([args.cross_cc, "-static", asm, harness, REPO / "src/io/io.c", "-o", exe], directory, timeout=args.timeout)
        if run([args.qemu, exe], directory, timeout=args.timeout).stdout != reference:
            raise ValueError(f"C caller to MIR callee ABI failed [{mode}]")
        completed.append(mode)
    print(f"PASS C caller → MIR callee: {len(completed)} execution paths, "
          "integer/pointer/float/live across recursion", flush=True)
    return completed


def evaluate_c_helpers(args, directory):
    directory.mkdir()
    generator = pathlib.Path(args.compiler).with_name("test_machine_codegen")
    require_tool(generator)
    run([generator, "emit-interop", directory], directory, timeout=args.timeout)
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
    run([args.opt, "-passes=verify", "-disable-output", ir], directory, timeout=args.timeout)
    native = directory / "native"
    run([args.clang, "-Wno-override-module", ir, harness, "-o", native], directory, timeout=args.timeout)
    reference = run([native], directory, timeout=args.timeout).stdout
    if reference != b"13 13.500000 0 1 0 1 0 1\n":
        raise ValueError(f"external helper reference failed: {reference!r}")
    completed = ["native"]
    for mode, _ in mir_modes(args):
        isel, allocation = mode.split("_")
        suffix = "" if allocation == "stack" else ".linear"
        asm = directory / f"interop.{isel}{suffix}.s"
        exe = directory / mode
        run([args.cross_cc, "-static", asm, harness, "-o", exe], directory, timeout=args.timeout)
        if run([args.qemu, exe], directory, timeout=args.timeout).stdout != reference:
            raise ValueError(f"MIR caller to C helpers ABI failed [{mode}]")
        completed.append(mode)
    print(f"PASS MIR caller → C helpers: {len(completed)} execution paths, "
          "mixed arguments/returns across clobbers", flush=True)
    return completed


def evaluate_regalloc_abi(args, directory):
    directory.mkdir()
    generator = pathlib.Path(args.compiler).with_name("test_register_allocation")
    require_tool(generator)
    run([generator, "emit-abi", directory], directory, timeout=args.timeout)
    expected_defs = {f"$r{n}" for n in range(23, 32)} | {f"$f{n}" for n in range(24, 32)}
    fields = (directory / "manifest.txt").read_text().split()
    if fields[:1] != ["body_defs"] or len(fields[1:]) != 17 or set(fields[1:]) != expected_defs:
        raise ValueError(f"ABI allocated body coverage is incomplete: {fields}")
    wrapper = REPO / "tests/backend/regalloc_abi.S"
    completed = []
    for source, expected in (("clobber_saved.s", 0), ("bad_saved.s", 1),
                             ("truncated_saved.s", 1)):
        exe = directory / (source + ".exe")
        run([args.cross_cc, "-static", wrapper, directory / source,
             directory / "main.c", "-o", exe], directory, timeout=args.timeout)
        result = run([args.qemu, exe], directory, checked=False, timeout=args.timeout)
        if result.returncode != expected:
            raise ValueError(f"ABI wrapper {source}: {result.returncode} != {expected}")
        completed.append(source)
    print("PASS full64 saved ABI: 17 body definitions, positive=0, clobber=1, truncation=1", flush=True)
    return {"executions": len(completed), "paths": completed,
            "body_definitions": len(fields[1:]), "allocator": "linear"}


def evaluate_regalloc_pressure(args, directory):
    directory.mkdir()
    generator = pathlib.Path(args.compiler).with_name("test_register_allocation")
    require_tool(generator)
    run([generator, "emit-pressure", directory], directory, timeout=args.timeout)
    rows = [row.split() for row in (directory / "manifest.txt").read_text().splitlines()]
    expected = {(mode, str(pool), shape) for mode in ("stack", "linear")
                for pool in range(3) for shape in ("straight", "loop", "swap", "call")}
    if (len(rows) != 24 or {tuple(row[:3]) for row in rows} != expected or
            any(len(row) != 4 or int(row[3]) <= 0 for row in rows)):
        raise ValueError("pressure manifest must demonstrate spills in all 24 cases")
    completed = []
    allocators = ("stack", "linear") if args.regalloc == "both" else (args.regalloc,)
    for mode in allocators:
        for pool in range(3):
            stem = f"pressure.{mode}.{pool}"
            exe = directory / stem
            run([args.cross_cc, "-static", directory / (stem + ".s"),
                 directory / "main.c", "-o", exe], directory, timeout=args.timeout)
            run([args.qemu, exe], directory, timeout=args.timeout)
            completed.append(stem)
    selected = [row for row in rows if row[0] in allocators]
    print(f"PASS target pressure: {len(selected)} known-value cases, "
          f"{len(completed)} executions, pools 0/1/2, {','.join(allocators)}", flush=True)
    return {"executions": completed, "cases": selected}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=pathlib.Path, default=REPO / "build/cminusfc")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--opt", default="opt")
    parser.add_argument("--cross-cc", default="loongarch64-unknown-linux-gnu-gcc")
    parser.add_argument("--qemu", default="qemu-loongarch64")
    parser.add_argument("--cli-only", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--course", choices=("lab3", "lab4", "lab6-functional", "lab6-performance"))
    parser.add_argument("--regalloc", choices=("stack", "linear", "both"), default="both")
    parser.add_argument("--mode", choices=("raw", "mem2reg", "const-prop", "gvn", "licm", "all"), default="mem2reg")
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if not args.timeout > 0:
        parser.error("--timeout must be positive")
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
                roots = {"lab3": "tests/3-codegen/autogen/testcases", "lab4": "tests/4-opt/testcases",
                         "lab6-functional": "tests/6-register/functional-cases",
                         "lab6-performance": "tests/6-register/performance-cases"}
                root = REPO / roots[args.course]
                sources = sorted(root.glob("*.cminus"))
                if not sources:
                    raise ValueError(f"no .cminus programs in {root}")
                for source in sources:
                    data = source.with_suffix(".in").read_bytes() if source.with_suffix(".in").exists() else b""
                    golden = source.with_suffix(".out").read_bytes()
                    # Course goldens append the process exit code to stdout.
                    lines = golden.replace(b"\r\n", b"\n").splitlines(keepends=True)
                    if not lines:
                        raise ValueError(f"empty golden: {source.with_suffix('.out')}")
                    expected = (b"".join(lines[:-1]), int(lines[-1]))
                    records.append(evaluate(args, directory / source.stem, source.stem,
                                            source.read_text(), [(data, expected)], args.flags))
            else:
                for name, (source, inputs) in PROGRAMS.items():
                    records.append(evaluate(args, directory / name, name, source, inputs, args.flags))
                abi = evaluate_regalloc_abi(args, directory / "regalloc_abi")
                pressure = evaluate_regalloc_pressure(args, directory / "regalloc_pressure")
                c_entry = evaluate_c_entry(args, directory / "c_entry")
                c_helpers = evaluate_c_helpers(args, directory / "c_helpers")
                observations = evaluate_metrics(args, directory / "metrics")
            report = {"cases": records, "modes": len(records[0]["paths"][0]),
                      "executions": sum(len(paths) for record in records for paths in record["paths"]),
                      "middleend": args.mode}
            if not args.course:
                report.update(metrics=observations, abi=abi, pressure=pressure,
                              c_entry=c_entry, c_helpers=c_helpers)
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
