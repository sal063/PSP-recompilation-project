#!/usr/bin/env python3
# Per-opcode differential gate for a CRT-free test module.
#
# The module runs a block of self-contained test instructions then calls sceKernelExitGame.
# Everything before that exit syscall is pure CPU execution with no HLE, so the reference
# interpreter must reproduce the PPSSPP oracle trace exactly up to that point. This script:
#   1. finds the first syscall step in the oracle trace (opcode 0, funct 0x0c),
#   2. truncates the oracle to the steps strictly before it,
#   3. runs the reference interpreter for that many steps,
#   4. requires the two traces to be byte-identical (zero divergences).
#
# Usage: microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>
# Exit 0 only when every pre-syscall instruction matches PPSSPP.

import subprocess
import sys
import os


def first_syscall_step(oracle_path):
    with open(oracle_path, "r", encoding="ascii", newline="\n") as handle:
        for line in handle:
            if not line or line[0] == "#":
                continue
            parts = line.split()
            if len(parts) < 3 or not parts[2].startswith("op="):
                continue
            op = int(parts[2][3:], 16)
            if (op >> 26) == 0 and (op & 0x3F) == 0x0C:
                return int(parts[0], 10)
    return None


def write_truncated(oracle_path, out_path, count):
    with open(oracle_path, "r", encoding="ascii", newline="\n") as src, \
         open(out_path, "w", encoding="ascii", newline="\n") as dst:
        for line in src:
            if line and line[0] == "#":
                dst.write(line)
                continue
            parts = line.split()
            if not parts:
                continue
            if int(parts[0], 10) >= count:
                break
            dst.write(line)


def main(argv):
    if len(argv) != 5:
        sys.stderr.write("usage: microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>\n")
        return 2
    run_elf, module, oracle, workdir = argv[1:]
    run_elf = os.path.abspath(run_elf)
    module = os.path.abspath(module)
    oracle = os.path.abspath(oracle)
    os.makedirs(workdir, exist_ok=True)

    syscall_step = first_syscall_step(oracle)
    if syscall_step is None:
        sys.stderr.write("no syscall (exit) found in oracle trace; module did not reach its exit\n")
        return 2
    print(f"first exit syscall at oracle step {syscall_step}; comparing the {syscall_step} preceding instructions")

    trunc = os.path.join(workdir, "oracle_pre_exit.trace")
    write_truncated(oracle, trunc, syscall_step)

    mine = os.path.join(workdir, "ref.trace")
    subprocess.run([run_elf, module, oracle, mine, str(syscall_step)], check=True)

    here = os.path.dirname(os.path.abspath(__file__))
    result = subprocess.run([sys.executable, os.path.join(here, "tracediff.py"), trunc, mine])
    if result.returncode == 0:
        print("microtest gate OK: all pre-exit instructions match PPSSPP")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
