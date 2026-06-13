#!/usr/bin/env python3
# Extract the sequence of HLE imports (by NID) a trace executes, mapping each import-stub jr
# line (pc in .sceStub.text) to its NID via the import map. With two traces it reports how far
# the recompiled run's import sequence agrees with the oracle's -- the functional-equivalence
# metric for HLE, which tolerates UID/address value differences the trace-diff cannot.
#
# Usage: nidseq.py <imports.toml> <trace> [<oracle-trace>]

import sys
import tomllib

S0, S1 = 0x08a246ac, 0x08a24dd4 + 8  # .sceStub.text range (ACX)


def load_imports(path):
    m = {}
    for e in tomllib.load(open(path, "rb"))["import"]:
        m[e["stub"]] = (e["lib"], e["nid"])
    return m


def nid_seq(trace, imp, limit=100000):
    seq = []
    for line in open(trace):
        if line[0] == "#":
            continue
        p = line.split()
        if len(p) < 3:
            continue
        pc = int(p[1][3:], 16)
        if S0 <= pc < S1 and p[2] == "op=0x03e00008":  # the stub's jr $ra line
            seq.append((pc, imp.get(pc, ("?", 0))))
            if len(seq) >= limit:
                break
    return seq


def main(argv):
    imp = load_imports(argv[1])
    mine = nid_seq(argv[2], imp)
    print(f"{argv[2]}: {len(mine)} imports")
    for i, (pc, (lib, nid)) in enumerate(mine[:40]):
        print(f"  {i:3} {lib}.0x{nid:08x}")
    if len(argv) > 3:
        orac = nid_seq(argv[3], imp, limit=len(mine) + 5)
        n = min(len(mine), len(orac))
        agree = 0
        for i in range(n):
            if mine[i][0] != orac[i][0]:
                print(f"DIVERGE at import {i}: recomp={imp.get(mine[i][0])} oracle={imp.get(orac[i][0])}")
                break
            agree += 1
        else:
            print(f"import sequence agrees for all {n} compared")
        print(f"AGREE {agree} imports")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
