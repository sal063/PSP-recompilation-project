#!/usr/bin/env python3
# Compare a funcdiff output trace (steps renumbered from 0) against the oracle slice that
# starts at <entry-step>. Step numbers are ignored; pc/op and the set of register/memory
# writes must match. Reports the first divergence, or confirms N matching steps.
#
# Usage: funcdiff_cmp.py <oracle-trace> <my-trace> <entry-step>
import sys

def norm(line):
    p = line.split()
    if len(p) < 3:
        return None
    pc, op = p[1], p[2]
    writes = frozenset(p[3:])
    return pc, op, writes

def main(argv):
    oracle, mine, entry = argv[1], argv[2], int(argv[3])
    my = [norm(l) for l in open(mine) if l[0] != '#']
    my = [m for m in my if m]
    f = open(oracle)
    # advance oracle to entry-step
    cur = []
    for line in f:
        if line[0] == '#':
            continue
        p = line.split()
        if len(p) < 3:
            continue
        st = int(p[0])
        if st < entry:
            continue
        cur.append(norm(line))
        if len(cur) >= len(my):
            break
    n = min(len(my), len(cur))
    for i in range(n):
        if my[i] != cur[i]:
            print(f"DIVERGENCE at my step {i} (oracle step {entry+i}):")
            print(f"  oracle: pc={cur[i][0]} op={cur[i][1]} writes={sorted(cur[i][2])}")
            print(f"  recomp: pc={my[i][0]} op={my[i][1]} writes={sorted(my[i][2])}")
            return 1
    print(f"MATCH: {n} steps identical (recomp ran {len(my)} steps from oracle step {entry})")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
