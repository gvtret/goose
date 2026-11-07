#!/usr/bin/env python3
"""Test runner for the LLVM-IR backend (M5).

Compiles a .goose file through the IR path:
  goose --emit-ir <fixture>.goose -o <tmp>.ll
  goose llc <tmp>.ll -o <tmp>.exe
  <tmp>.exe
and compares stdout against the expected file.

Usage:
  python3 test/ir_runner.py <goose_binary> <fixture.goose> <expected.out> [extra args...]
"""
import subprocess
import sys
import tempfile
import os


def main() -> int:
    if len(sys.argv) < 4:
        print(f"usage: {sys.argv[0]} <goose> <fixture.goose> <expected.out> [args...]",
              file=sys.stderr)
        return 2

    goose = sys.argv[1]
    fixture = sys.argv[2]
    expected_file = sys.argv[3]
    extra = sys.argv[4:]

    with tempfile.TemporaryDirectory() as d:
        ll = os.path.join(d, "prog.ll")
        exe = os.path.join(d, "prog")

        # 1. Emit IR
        r = subprocess.run([goose, "--emit-ir", fixture, "-o", ll],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[ir_runner] --emit-ir failed:\n{r.stderr}", file=sys.stderr)
            return 1

        # 2. llc + clang via the goose llc driver
        r = subprocess.run([goose, "llc", ll, "-o", exe],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"[ir_runner] goose llc failed:\n{r.stderr}", file=sys.stderr)
            return 1

        # 3. Run and capture stdout
        r = subprocess.run([exe] + extra, capture_output=True, text=True)

    with open(expected_file) as f:
        expected = f.read()

    actual = r.stdout
    if actual != expected:
        print(f"[ir_runner] MISMATCH for {fixture}:", file=sys.stderr)
        print(f"--- expected ---\n{expected}", file=sys.stderr)
        print(f"--- actual ---\n{actual}", file=sys.stderr)
        return 1

    print(f"[ir_runner] {fixture}: ok ({len(actual)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
