#!/usr/bin/env python3
# Compare two CPU-state traces (tools/TRACE_FORMAT.md) and report the first divergence.
# Exit 0 when the two traces are identical step-for-step, 1 when they diverge, 2 on a
# malformed input. The first divergence is reported with the guest PC where it happened,
# which is the located bug for differential testing against the PPSSPP oracle.

import sys


def parse_step_line(line, lineno, path):
    # "<step> pc=<hpc> op=<hword> <tokens...>" -> (step, pc, op, {name: value})
    parts = line.split()
    if len(parts) < 3:
        fail(f"{path}:{lineno}: step line has fewer than 3 fields: {line!r}")
    try:
        step = int(parts[0], 10)
    except ValueError:
        fail(f"{path}:{lineno}: step index is not a decimal integer: {parts[0]!r}")
    if not parts[1].startswith("pc=") or not parts[2].startswith("op="):
        fail(f"{path}:{lineno}: expected 'pc=' then 'op=', got {parts[1]!r} {parts[2]!r}")
    pc = parts[1][3:]
    op = parts[2][3:]
    writes = {}
    for tok in parts[3:]:
        name, eq, value = tok.partition("=")
        if eq != "=" or not name or not value:
            fail(f"{path}:{lineno}: malformed write token: {tok!r}")
        if name in writes:
            fail(f"{path}:{lineno}: duplicate write token for {name!r}")
        writes[name] = value
    return step, pc, op, writes


def fail(msg):
    sys.stderr.write(msg + "\n")
    sys.exit(2)


def load(path):
    steps = []
    header = None
    with open(path, "r", encoding="ascii", newline="\n") as handle:
        for lineno, raw in enumerate(handle, start=1):
            line = raw.rstrip("\n")
            if line == "":
                continue
            if line.startswith("#"):
                if header is None:
                    header = line
                continue
            steps.append(parse_step_line(line, lineno, path))
    if header is None:
        fail(f"{path}: missing required '# psp-recomp trace' header line")
    return header, steps


def describe_writes(writes):
    return " ".join(f"{name}={writes[name]}" for name in sorted(writes))


def diff(path_a, path_b):
    _, a = load(path_a)
    _, b = load(path_b)
    n = min(len(a), len(b))
    for i in range(n):
        step_a, pc_a, op_a, w_a = a[i]
        step_b, pc_b, op_b, w_b = b[i]
        if step_a != step_b:
            return (i, pc_a, f"step index {step_a} vs {step_b}")
        if pc_a != pc_b:
            return (i, pc_a, f"pc {pc_a} vs {pc_b}")
        if op_a != op_b:
            return (i, pc_a, f"op {op_a} vs {op_b}")
        if w_a != w_b:
            only_a = {k: v for k, v in w_a.items() if w_b.get(k) != v}
            only_b = {k: v for k, v in w_b.items() if w_a.get(k) != v}
            detail = f"writes differ: A[{describe_writes(only_a)}] B[{describe_writes(only_b)}]"
            return (i, pc_a, detail)
    if len(a) != len(b):
        shorter, longer = (path_a, path_b) if len(a) < len(b) else (path_b, path_a)
        at = a[n - 1] if n else None
        pc = at[1] if at else "0x????????"
        return (n, pc, f"trace length differs: {len(a)} vs {len(b)} ({shorter} ends first)")
    return None


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: tracediff.py <trace-a> <trace-b>\n")
        return 2
    result = diff(argv[1], argv[2])
    if result is None:
        _, a = load(argv[1])
        print(f"OK: traces identical, {len(a)} steps, zero divergences")
        return 0
    step, pc, detail = result
    print(f"DIVERGENCE at step {step}, pc {pc}: {detail}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
