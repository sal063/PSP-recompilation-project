// Self-test for the reference interpreter. Builds small Allegrex programs with hand-computed
// expected results and asserts the interpreter reproduces them. This is a real test: every
// expected value below is computed by hand from the MIPS semantics, not copied from the
// interpreter's own output. Exit code 0 means all checks passed.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "interp.h"

using namespace ref;

namespace {

int g_failures = 0;

void Check(const char *what, uint32_t got, uint32_t want) {
	if (got != want) {
		std::printf("FAIL %-28s got=0x%08x want=0x%08x\n", what, got, want);
		g_failures++;
	} else {
		std::printf("ok   %-28s 0x%08x\n", what, got);
	}
}

// Register indices.
enum { ZERO = 0, T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15, SP = 29 };

uint32_t R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct) {
	return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
uint32_t I(uint32_t opcode, uint32_t rs, uint32_t rt, uint16_t imm) {
	return (opcode << 26) | (rs << 21) | (rt << 16) | imm;
}
const uint32_t BREAK = 0x0000000D;

// Load a program at base and run to completion (a break instruction), returning final state.
CpuState RunProgram(const std::vector<uint32_t> &prog, uint32_t base) {
	Memory mem;
	for (size_t i = 0; i < prog.size(); i++)
		mem.Write32(base + (uint32_t)i * 4, prog[i]);
	CpuState s;
	s.pc = base;
	s.r[SP] = 0x08A00000;  // a valid RAM stack pointer
	StepResult res = Run(&s, &mem, 1000, nullptr);
	if (res.reason != StopReason::kBreak && res.reason != StopReason::kStepLimit) {
		std::printf("FAIL program stopped: reason=%d pc=0x%08x op=0x%08x\n",
			(int)res.reason, res.pc, res.op);
		g_failures++;
	}
	return s;
}

void TestArithmetic() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0x1234),       // lui   t0, 0x1234        -> 0x12340000
		I(0x0D, T0, T0, 0x5678),      // ori   t0, t0, 0x5678    -> 0x12345678
		I(0x09, ZERO, T1, 100),       // addiu t1, zero, 100      -> 100
		R(T0, T1, T2, 0, 0x21),       // addu  t2, t0, t1         -> 0x123456DC
		R(ZERO, T1, T3, 4, 0x00),     // sll   t3, t1, 4          -> 1600 (0x640)
		R(T2, T1, T4, 0, 0x23),       // subu  t4, t2, t1         -> 0x12345678
		R(T0, T1, T5, 0, 0x24),       // and   t5, t0, t1         -> 0x12345678 & 100 = 0x60
		R(T0, T1, T6, 0, 0x2A),       // slt   t6, t0, t1         -> (0x12345678 < 100) signed = 0
		I(0x0A, T1, T7, 0xFFFF),      // slti  t7, t1, -1         -> (100 < -1) = 0
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("lui+ori t0", s.r[T0], 0x12345678);
	Check("addiu t1", s.r[T1], 100);
	Check("addu t2", s.r[T2], 0x123456DC);
	Check("sll t3", s.r[T3], 0x640);
	Check("subu t4", s.r[T4], 0x12345678);
	Check("and t5", s.r[T5], 0x60);
	Check("slt t6", s.r[T6], 0);
	Check("slti t7", s.r[T7], 0);
}

void TestMulDiv() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 7),         // addiu t0, zero, 7
		I(0x09, ZERO, T1, 6),         // addiu t1, zero, 6
		R(T0, T1, 0, 0, 0x18),        // mult  t0, t1            -> lo=42, hi=0
		R(0, 0, T2, 0, 0x12),         // mflo  t2                -> 42
		R(0, 0, T3, 0, 0x10),         // mfhi  t3                -> 0
		I(0x09, ZERO, T4, 100),       // addiu t4, zero, 100
		I(0x09, ZERO, T5, 7),         // addiu t5, zero, 7
		R(T4, T5, 0, 0, 0x1B),        // divu  t4, t5            -> lo=14, hi=2
		R(0, 0, T6, 0, 0x12),         // mflo  t6                -> 14
		R(0, 0, T7, 0, 0x10),         // mfhi  t7                -> 2
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("mult lo (mflo)", s.r[T2], 42);
	Check("mult hi (mfhi)", s.r[T3], 0);
	Check("divu lo (mflo)", s.r[T6], 14);
	Check("divu hi (mfhi)", s.r[T7], 2);
}

void TestMemory() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x0F, 0, T0, 0xDEAD),       // lui   t0, 0xDEAD
		I(0x0D, T0, T0, 0xBEEF),      // ori   t0, t0, 0xBEEF    -> 0xDEADBEEF
		I(0x2B, SP, T0, 0x10),        // sw    t0, 16(sp)
		I(0x23, SP, T1, 0x10),        // lw    t1, 16(sp)        -> 0xDEADBEEF
		I(0x28, SP, T0, 0x20),        // sb    t0, 32(sp)        (stores 0xEF)
		I(0x24, SP, T2, 0x20),        // lbu   t2, 32(sp)        -> 0xEF
		I(0x20, SP, T3, 0x20),        // lb    t3, 32(sp)        -> sign-extended 0xFFFFFFEF
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("lw roundtrip", s.r[T1], 0xDEADBEEF);
	Check("lbu byte", s.r[T2], 0xEF);
	Check("lb sign-extend", s.r[T3], 0xFFFFFFEF);
}

void TestBranchDelaySlot() {
	const uint32_t base = 0x08900000;
	// beq taken: the delay slot runs, the instruction after it is skipped.
	std::vector<uint32_t> p = {
		I(0x09, ZERO, T0, 5),         // 0x..00 addiu t0, zero, 5
		I(0x09, ZERO, T1, 5),         // 0x..04 addiu t1, zero, 5
		I(0x04, T0, T1, 2),           // 0x..08 beq t0,t1,+2 -> target 0x..08+4+8 = 0x..14
		I(0x09, ZERO, T2, 0x111),     // 0x..0c delay slot: addiu t2, zero, 0x111 (runs)
		I(0x09, ZERO, T3, 0x222),     // 0x..10 skipped (branch target is 0x..14)
		I(0x09, ZERO, T4, 0x333),     // 0x..14 addiu t4, zero, 0x333 (runs)
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("delay slot executed", s.r[T2], 0x111);
	Check("skipped after branch", s.r[T3], 0);     // must remain 0
	Check("branch target ran", s.r[T4], 0x333);
}

uint32_t MTC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x04u << 21) | (rt << 16) | (fs << 11); }
uint32_t MFC1(uint32_t rt, uint32_t fs) { return (0x11u << 26) | (0x00u << 21) | (rt << 16) | (fs << 11); }
uint32_t FPS(uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x10u << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
uint32_t FPW(uint32_t fs, uint32_t fd, uint32_t funct) {
	return (0x11u << 26) | (0x14u << 21) | (fs << 11) | (fd << 6) | funct;
}

void TestFpu() {
	const uint32_t base = 0x08900000;
	// f0=2.0 (0x40000000), f1=3.0 (0x40400000) loaded via GPR + mtc1.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0f
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0f
		FPS(1, 0, 2, 0x00), MFC1(T2, 2),          // add.s f2,f0,f1 -> 5.0f; t2 = bits
		FPS(1, 0, 3, 0x02), MFC1(T3, 3),          // mul.s f3,f0,f1 -> 6.0f; t3 = bits
		FPS(1, 0, 4, 0x01), MFC1(T4, 4),          // sub.s f4,f0,f1 -> -1.0f; t4 = bits
		I(0x09, ZERO, T5, 7), MTC1(T5, 5),        // f5 = int 7 (bits)
		FPW(5, 6, 0x20), MFC1(T6, 6),             // cvt.s.w f6,f5 -> 7.0f; t6 = bits
		I(0x0F, ZERO, T7, 0x40F0), MTC1(T7, 7),   // f7 = 7.5f (0x40F00000)
		FPS(0, 7, 8, 0x24), MFC1(T0, 8),          // cvt.w.s f8,f7 -> 8 (round-half-to-even); t0 = 8
		FPS(0, 7, 9, 0x0D), MFC1(T1, 9),          // trunc.w.s f9,f7 -> 7; t1 = 7
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("add.s 2+3=5.0", s.r[T2], 0x40A00000);
	Check("mul.s 2*3=6.0", s.r[T3], 0x40C00000);
	Check("sub.s 2-3=-1.0", s.r[T4], 0xBF800000);
	Check("cvt.s.w 7=7.0", s.r[T6], 0x40E00000);
	Check("cvt.w.s 7.5=8", s.r[T0], 8);
	Check("trunc.w.s 7.5=7", s.r[T1], 7);
}

uint32_t BC1(uint32_t tf, uint32_t likely, uint16_t off) {
	return (0x11u << 26) | (0x08u << 21) | ((likely & 1) << 17) | ((tf & 1) << 16) | off;
}

void TestFpuCompareBranch() {
	const uint32_t base = 0x08900000;
	// c.lt.s f0,f1 with f0=2.0 < f1=3.0 sets the FPU condition; bc1t must be taken and skip
	// the instruction after its delay slot.
	std::vector<uint32_t> p = {
		I(0x0F, ZERO, T0, 0x4000), MTC1(T0, 0),   // f0 = 2.0
		I(0x0F, ZERO, T1, 0x4040), MTC1(T1, 1),   // f1 = 3.0
		FPS(1, 0, 0, 0x3C),                       // c.lt.s f0,f1 -> condition true
		BC1(1, 0, 2),                             // bc1t +2 (taken)
		0,                                        // delay slot: nop
		I(0x09, ZERO, T2, 0xBAD),                 // skipped on taken branch
		I(0x09, ZERO, T2, 0x111),                 // branch target
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("c.lt.s+bc1t taken", s.r[T2], 0x111);
}

void TestR0IsZero() {
	const uint32_t base = 0x08900000;
	std::vector<uint32_t> p = {
		I(0x09, ZERO, ZERO, 1234),    // addiu zero, zero, 1234  -> write to r0 discarded
		R(ZERO, ZERO, T0, 0, 0x21),   // addu  t0, zero, zero     -> 0
		BREAK,
	};
	CpuState s = RunProgram(p, base);
	Check("r0 stays zero", s.r[ZERO], 0);
	Check("addu from zero", s.r[T0], 0);
}

}  // namespace

int main() {
	TestArithmetic();
	TestMulDiv();
	TestMemory();
	TestBranchDelaySlot();
	TestFpu();
	TestFpuCompareBranch();
	TestR0IsZero();
	std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
	return g_failures == 0 ? 0 : 1;
}
