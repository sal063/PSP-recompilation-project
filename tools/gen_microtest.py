#!/usr/bin/env python3
# Generate a CRT-free Allegrex test module that exercises the integer and single-precision
# FPU ISA with edge-case and seeded-random operands. Each test sets up its operands inline
# (no memory, no HLE) and executes one instruction; the result lands in a register and is
# captured by the PPSSPP oracle trace. The reference interpreter must reproduce every
# instruction exactly (tools/microtest_gate.py). Deterministic: a fixed seed.
#
# Usage: gen_microtest.py <out.c> [random_per_op]

import random
import sys

# Edge-case 32-bit operands that tend to expose sign/overflow/boundary bugs.
EDGE = [0x00000000, 0x00000001, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x00000002,
        0x55555555, 0xAAAAAAAA, 0x0000FFFF, 0xFFFF0000, 0x00000100, 0x000000FF]

# Edge-case single-precision bit patterns: 0, -0, 1, -1, 2, 0.5, big, small-normal,
# denormal, +inf, -inf, qNaN, max-finite.
FEDGE = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x3F000000,
         0x49742400, 0x00800000, 0x00000001, 0x7F800000, 0xFF800000, 0x7FC00000,
         0x7F7FFFFF, 0xC0490FDB, 0x40490FDB]


def li(reg, val):
    # Load a 32-bit immediate with lui+ori (both traced, both compared).
    return [f"lui ${reg}, 0x{(val >> 16) & 0xFFFF:04x}",
            f"ori ${reg}, ${reg}, 0x{val & 0xFFFF:04x}"]


def operands32(extra):
    vals = list(EDGE)
    for _ in range(extra):
        vals.append(random.randint(0, 0xFFFFFFFF))
    return vals


def fpairs(extra):
    vals = list(FEDGE)
    for _ in range(extra):
        vals.append(random.randint(0, 0xFFFFFFFF))
    return vals


def gen(extra):
    lines = []

    def emit(*asm):
        lines.extend(asm)

    # Integer R-type: dest = f(t0, t1).
    rtype = ["addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "max", "min",
             "sllv", "srlv", "srav"]
    ops = operands32(extra)
    for op in rtype:
        for a in ops:
            for b in EDGE:  # second operand from the edge set keeps the module bounded
                emit(*li("t0", a), *li("t1", b), f"{op} $t2, $t0, $t1")

    # Shift-immediate: dest = f(t0, sa) for several shift amounts.
    for op in ["sll", "srl", "sra"]:
        for a in ops:
            for sa in (0, 1, 7, 15, 16, 31):
                emit(*li("t0", a), f"{op} $t2, $t0, {sa}")

    # Multiply/divide: results in hi/lo (captured directly). Includes divide-by-zero and the
    # INT_MIN / -1 overflow case so the reference interpreter's edge handling is checked.
    for op in ["mult", "multu", "div", "divu", "madd", "maddu", "msub", "msubu"]:
        for a in EDGE:
            for b in EDGE:
                # Seed hi/lo for madd/msub via a prior multiply so accumulation is exercised.
                emit(*li("t0", a), *li("t1", b), "multu $t0, $t1", f"{op} $t0, $t1",
                     "mfhi $t2", "mflo $t3")

    # clz/clo.
    for op in ["clz", "clo"]:
        for a in ops:
            emit(*li("t0", a), f"{op} $t2, $t0")

    # ext/ins with varied position and size.
    for a in ops:
        for pos, size in ((0, 8), (4, 8), (8, 16), (0, 32), (16, 16), (3, 5)):
            emit(*li("t0", a), f"ext $t2, $t0, {pos}, {size}")
    for a in EDGE:
        for b in EDGE:
            for pos, size in ((0, 8), (8, 8), (4, 12), (0, 1)):
                emit(*li("t2", a), *li("t1", b), f"ins $t2, $t1, {pos}, {size}")

    # bit/byte ops.
    for a in ops:
        emit(*li("t0", a), "wsbh $t2, $t0", "seb $t3, $t0", "seh $t4, $t0")

    # Immediate ALU.
    for a in ops:
        for imm in (0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x1234):
            emit(*li("t0", a),
                 f"addiu $t2, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                 f"slti  $t3, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                 f"sltiu $t4, $t0, {imm - 0x10000 if imm >= 0x8000 else imm}",
                 f"andi  $t5, $t0, 0x{imm:04x}",
                 f"ori   $t6, $t0, 0x{imm:04x}",
                 f"xori  $t7, $t0, 0x{imm:04x}")

    # FPU two-operand arithmetic and compares, plus one-operand transforms/conversions.
    fvals = fpairs(extra)
    for a in fvals:
        for b in FEDGE:
            emit(*li("t0", a), "mtc1 $t0, $f0", *li("t1", b), "mtc1 $t1, $f1",
                 "add.s $f2, $f0, $f1", "mfc1 $t2, $f2",
                 "sub.s $f3, $f0, $f1", "mfc1 $t3, $f3",
                 "mul.s $f4, $f0, $f1", "mfc1 $t4, $f4",
                 "div.s $f5, $f0, $f1", "mfc1 $t5, $f5",
                 "c.eq.s $f0, $f1", "c.lt.s $f0, $f1", "c.le.s $f0, $f1",
                 "c.ult.s $f0, $f1", "c.un.s $f0, $f1")
    for a in fvals:
        emit(*li("t0", a), "mtc1 $t0, $f0",
             "abs.s $f1, $f0", "mfc1 $t1, $f1",
             "neg.s $f2, $f0", "mfc1 $t2, $f2",
             "sqrt.s $f3, $f0", "mfc1 $t3, $f3",
             "cvt.w.s $f4, $f0", "mfc1 $t4, $f4",
             "trunc.w.s $f5, $f0", "mfc1 $t5, $f5",
             "round.w.s $f6, $f0", "mfc1 $t6, $f6",
             "ceil.w.s $f7, $f0", "mfc1 $t7, $f7",
             "floor.w.s $f8, $f0", "mfc1 $t8, $f8",
             "cvt.s.w $f9, $f4", "mfc1 $t9, $f9")

    return lines


def write_c(path, lines):
    body = "\n".join('\t\t"%s\\n"' % ln for ln in lines)
    clobbers = ", ".join('"%s"' % r for r in
                         ["t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9",
                          "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9",
                          "hi", "lo", "memory"])
    with open(path, "w", encoding="ascii", newline="\n") as out:
        out.write(
            "// Generated by tools/gen_microtest.py. Do not edit by hand.\n"
            "// CRT-free Allegrex integer + FPU differential test module.\n\n"
            "#include <pspkernel.h>\n\n"
            'PSP_MODULE_INFO("microtest_gen", 0, 1, 0);\n'
            "PSP_MAIN_THREAD_ATTR(0);\n\n"
            "void _start(void) {\n"
            "\t__asm__ volatile(\n"
            + body + "\n"
            "\t\t::: " + clobbers + "\n"
            "\t);\n"
            "\tsceKernelExitGame();\n"
            "\tfor (;;) {}\n"
            "}\n")


def main(argv):
    if len(argv) < 2:
        sys.stderr.write("usage: gen_microtest.py <out.c> [random_per_op]\n")
        return 2
    random.seed(0xA11E)
    extra = int(argv[2]) if len(argv) > 2 else 6
    lines = gen(extra)
    write_c(argv[1], lines)
    print(f"wrote {argv[1]}: {len(lines)} instructions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
