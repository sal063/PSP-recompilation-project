"""Compare matching PPM snapshots from two directories (software vs GPU GE runs).

Usage: python tools/ppmdiff.py dirA dirB
Prints per-file: pixel count differing at all, differing by >8/255 in any channel,
and the max channel delta. Exit code 0 always (it is a report, not a gate).
"""
import os
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    if parts[0] != b"P6":
        return None, 0, 0
    w, h = map(int, parts[1].split())
    return parts[3][: w * h * 3], w, h


def main():
    da, db = sys.argv[1], sys.argv[2]
    names = sorted(set(os.listdir(da)) & set(os.listdir(db)),
                   key=lambda n: int("".join(c for c in n if c.isdigit()) or 0))
    if not names:
        print("no common files")
        return
    for n in names:
        a, w, h = read_ppm(os.path.join(da, n))
        b, _, _ = read_ppm(os.path.join(db, n))
        if a is None or b is None or len(a) != len(b):
            print(f"{n}: unreadable/size mismatch")
            continue
        npx = w * h
        diff = big = 0
        maxd = 0
        for i in range(0, npx * 3, 3):
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
            if d:
                diff += 1
                if d > 8:
                    big += 1
                if d > maxd:
                    maxd = d
        print(f"{n}: {npx}px diff={diff} ({100.0*diff/npx:.2f}%) big(>8)={big} "
              f"({100.0*big/npx:.2f}%) maxdelta={maxd}")


if __name__ == "__main__":
    main()
