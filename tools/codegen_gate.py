#!/usr/bin/env python3
# Phase 3 codegen gate: generate C for a module, compile it with the runtime + driver, run
# it with tracing, and require the trace to match the PPSSPP oracle up to the first HLE call
# (the oracle is truncated there, where the generated code stops via sr_hle_call).
#
# Usage: codegen_gate.py <elf> <oracle.trace> <workdir>

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    return r.returncode


def first_syscall_step(oracle):
    with open(oracle) as f:
        for line in f:
            if not line or line[0] == "#":
                continue
            p = line.split()
            if len(p) < 3 or not p[2].startswith("op="):
                continue
            op = int(p[2][3:], 16)
            if (op >> 26) == 0 and (op & 0x3F) == 0x0C:
                return int(p[0])
    return None


def truncate(oracle, out, count):
    with open(oracle) as src, open(out, "w", newline="\n") as dst:
        for line in src:
            if line and line[0] == "#":
                dst.write(line)
                continue
            p = line.split()
            if not p:
                continue
            if int(p[0]) >= count:
                break
            dst.write(line)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: codegen_gate.py <elf> <oracle.trace> <workdir>\n")
        return 2
    elf, oracle, workdir = (os.path.abspath(a) for a in argv[1:])
    os.makedirs(workdir, exist_ok=True)
    gen = os.path.join(workdir, "gen.c")
    drv = os.path.join(workdir, "driver.exe")
    mine = os.path.join(workdir, "my.trace")
    trunc = os.path.join(workdir, "oracle_pre_hle.trace")

    env = dict(os.environ)
    tmp = os.path.join(ROOT, ".tmp")
    os.makedirs(tmp, exist_ok=True)
    env["TMPDIR"] = env["TMP"] = env["TEMP"] = tmp.replace("\\", "/")

    if run([sys.executable, os.path.join(ROOT, "tools", "codegen.py"), elf, gen]):
        return 1
    rt = os.path.join(ROOT, "src", "rt")
    if run(["clang", "-std=c11", "-O1", "-D_CRT_SECURE_NO_WARNINGS", "-I", rt,
            "-o", drv, gen, os.path.join(rt, "recomp.c"), os.path.join(rt, "driver.c")], env=env):
        return 1
    if run([drv, elf, oracle, mine]):
        return 1

    s = first_syscall_step(oracle)
    if s is None:
        sys.stderr.write("no syscall in oracle trace\n")
        return 2
    truncate(oracle, trunc, s)
    print(f"comparing the {s} pre-HLE instructions")
    rc = run([sys.executable, os.path.join(ROOT, "tools", "tracediff.py"), trunc, mine])
    if rc == 0:
        print("codegen gate OK: generated C matches PPSSPP")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
