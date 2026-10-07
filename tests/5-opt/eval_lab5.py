#!/usr/bin/env python3
"""Lab5 correctness/IR checks, not a timing benchmark or a claim of pass completion."""

import argparse
from collections import Counter
import json
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
SUITES = {
    "const-prop": "const-prop",
    "licm": "loop-inv-hoist",
    "gvn-functional": "gvn/functional",
    "gvn-performance": "gvn/performance",
}


def checked_run(command, directory, input_bytes=None):
    return subprocess.run(command, cwd=directory, input=input_bytes, capture_output=True,
                          timeout=60, check=True)


def find_cases(root, suite):
    cases = sorted((root / SUITES[suite]).glob("*.cminus"))
    if not cases:
        raise ValueError(f"no testcases for {suite} under {root}")
    return cases


def check_output(actual, expected):
    if actual != expected:
        raise ValueError(f"output mismatch: expected {expected!r}, got {actual!r}")


def host_entry(text):
    # The teaching language allows void main. Native process entry must return
    # i32, otherwise an arbitrary exit status can hide behind correct stdout.
    # Only the generated artifact is adapted; course sources/baselines stay intact.
    if "define void @main()" not in text:
        return text
    text = text.replace("@main(", "@__lab5_void_main(")
    return text + "\ndefine i32 @main() {\nentry:\n  call void @__lab5_void_main()\n  ret i32 0\n}\n"


def read_partition(path):
    text = path.read_text()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # The original GVN serializer writes JSON5 trailing commas. This optional
        # dependency is needed only when actually grading a completed GVN pass.
        try:
            import json5
        except ImportError as error:
            raise ValueError("GVN dump is JSON5; explicitly install json5 in your virtual environment") from error
        return json5.loads(text)


def check_partitions(actual, expected):
    def canonical(functions):
        return {f["function"]: {bb: Counter(frozenset(cc) for cc in p) for bb, p in f["pout"].items()}
                for f in functions}
    if canonical(actual) != canonical(expected):
        raise ValueError("GVN partitions differ from the course reference (block/value names are version-dependent)")


def evaluate_case(args, suite, source):
    parent = args.output_dir / suite / args.mode
    parent.mkdir(parents=True, exist_ok=True)
    directory = pathlib.Path(tempfile.mkdtemp(prefix=source.stem + "-", dir=parent))
    ir = directory / "program.ll"
    flags = [] if args.mode == "raw" else ["-mem2reg"]
    if args.mode == "optimized":
        flags += {"const-prop": ["-const-prop"], "licm": ["-licm"],
                  "gvn-functional": ["-gvn", "-dump-json"], "gvn-performance": ["-gvn"]}[suite]
    checked_run([str(args.compiler), "-emit-llvm", *flags, "-o", str(ir), str(source)], directory)
    if not ir.is_file():
        raise ValueError("compiler returned success but did not generate IR")

    if suite == "gvn-functional":
        # These cases supply partitions, not runtime answers. Compilation validates
        # the IR in raw/mem2reg mode; no GVN correctness claim follows from that.
        checked_run([args.clang, "-Wno-override-module", "-c", str(ir), "-o", str(directory / "program.o")], directory)
        if args.mode == "optimized":
            check_partitions(read_partition(directory / "gvn.json"), read_partition(source.with_suffix(".json")))
            return "GVN partitions match (name-dependent reference)"
        return "IR compiled only; GVN algorithm NOT tested"

    input_file = source.with_suffix(".in")
    data = input_file.read_bytes() if input_file.exists() else b""
    expected = source.with_suffix(".out").read_bytes()
    ir.write_text(host_entry(ir.read_text()))
    exe = directory / "program"
    checked_run([args.clang, "-O0", "-Wno-override-module", str(ir), str(args.runtime), "-o", str(exe)], directory)
    check_output(checked_run([str(exe)], directory, data).stdout, expected)

    baseline = source.parent / "baseline" / source.with_suffix(".ll").name
    if not baseline.exists():
        raise ValueError(f"missing course baseline: {baseline}")
    baseline_ir = directory / "baseline.ll"
    baseline_ir.write_text(host_entry(baseline.read_text()))
    baseline_exe = directory / "baseline"
    compat = pathlib.Path(__file__).with_name("baseline_compat.c")
    checked_run([args.clang, "-O0", "-Wno-override-module", str(baseline_ir), str(compat), str(args.runtime), "-o", str(baseline_exe)], directory)
    check_output(checked_run([str(baseline_exe)], directory, data).stdout, expected)
    return "program + course baseline outputs match"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=[*SUITES, "all"], default="all")
    parser.add_argument("--mode", choices=["raw", "mem2reg", "optimized"], default="mem2reg")
    parser.add_argument("--compiler", type=pathlib.Path, default=REPO / "build/cminusfc")
    parser.add_argument("--runtime", type=pathlib.Path, default=REPO / "build/libcminus_io.a")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent / "output")
    parser.add_argument("--test-root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent / "testcases")
    args = parser.parse_args(argv)
    for name in ("compiler", "runtime", "output_dir", "test_root"):
        setattr(args, name, getattr(args, name).resolve())
    if args.output_dir == args.test_root or args.test_root in args.output_dir.parents:
        parser.error("output-dir cannot be inside the course testcases directory")

    suites = list(SUITES) if args.suite == "all" else [args.suite]
    try:
        jobs = [(suite, source) for suite in suites for source in find_cases(args.test_root, suite)]
    except ValueError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    failures = 0
    for suite, source in jobs:
        label = f"{suite}/{source.name} [{args.mode}]"
        try:
            detail = evaluate_case(args, suite, source)
            print(f"PASS {label}: {detail}", flush=True)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            failures += 1
            print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
            if isinstance(error, subprocess.CalledProcessError):
                print(error.stderr.decode(errors="replace")[-2000:], file=sys.stderr)
    print(f"{len(jobs) - failures}/{len(jobs)} cases passed; correctness/IR checks only, no timing score.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
