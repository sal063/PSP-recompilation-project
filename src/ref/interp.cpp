// Reference Allegrex interpreter implementation. See interp.h.

#include "interp.h"

#include <cmath>
#include <cstring>

namespace ref {

// ---- Trace sink ----------------------------------------------------------------------

void TraceSink::Header(const char *target, uint32_t start_pc) {
	std::fprintf(out_, "# psp-recomp trace v1 oracle=interp target=%s start_pc=0x%08x\n",
		target ? target : "unknown", start_pc);
}

void TraceSink::BeginStep(const CpuState *s, uint32_t pc, uint32_t op) {
	std::memcpy(r_, s->r, sizeof(r_));
	std::memcpy(fi_, s->fi, sizeof(fi_));
	hi_ = s->hi;
	lo_ = s->lo;
	fcr31_ = s->fcr31;
	pc_ = pc;
	op_ = op;
}

void TraceSink::EndStep(const CpuState *s, uint32_t mem_addr, int mem_size, const Memory *mem) {
	char line[4096];
	int n = std::snprintf(line, sizeof(line), "%llu pc=0x%08x op=0x%08x", step_, pc_, op_);
	for (int i = 1; i < 32; i++)
		if (s->r[i] != r_[i])
			n += std::snprintf(line + n, sizeof(line) - n, " r%d=0x%08x", i, s->r[i]);
	if (s->hi != hi_)
		n += std::snprintf(line + n, sizeof(line) - n, " hi=0x%08x", s->hi);
	if (s->lo != lo_)
		n += std::snprintf(line + n, sizeof(line) - n, " lo=0x%08x", s->lo);
	for (int i = 0; i < 32; i++)
		if (s->fi[i] != fi_[i])
			n += std::snprintf(line + n, sizeof(line) - n, " f%d=0x%08x", i, s->fi[i]);
	if (s->fcr31 != fcr31_)
		n += std::snprintf(line + n, sizeof(line) - n, " fcr31=0x%08x", s->fcr31);
	if (mem_size == 1)
		n += std::snprintf(line + n, sizeof(line) - n, " m8[0x%08x]=0x%02x", mem_addr, mem->Read8(mem_addr));
	else if (mem_size == 2)
		n += std::snprintf(line + n, sizeof(line) - n, " m16[0x%08x]=0x%04x", mem_addr, mem->Read16(mem_addr));
	else if (mem_size == 4)
		n += std::snprintf(line + n, sizeof(line) - n, " m32[0x%08x]=0x%08x", mem_addr, mem->Read32(mem_addr));
	line[n] = '\n';
	std::fwrite(line, 1, n + 1, out_);
	step_++;
}

// ---- Instruction field helpers -------------------------------------------------------

namespace {

inline uint32_t Rs(uint32_t op) { return (op >> 21) & 0x1F; }
inline uint32_t Rt(uint32_t op) { return (op >> 16) & 0x1F; }
inline uint32_t Rd(uint32_t op) { return (op >> 11) & 0x1F; }
inline uint32_t Sa(uint32_t op) { return (op >> 6) & 0x1F; }
inline uint32_t Funct(uint32_t op) { return op & 0x3F; }
inline int32_t SImm(uint32_t op) { return (int32_t)(int16_t)(op & 0xFFFF); }
inline uint32_t ZImm(uint32_t op) { return op & 0xFFFF; }

inline void SetR(CpuState *s, uint32_t idx, uint32_t val) {
	if (idx != 0) s->r[idx] = val;
}

// Single-precision float to 32-bit integer, mirroring PPSSPP's Int_FPU2op exactly. NaN and
// +inf give INT_MAX, -inf gives INT_MIN. Finite values use the per-op rounding; the (int)
// cast of an out-of-range finite value yields 0x80000000 on x86 (same as PPSSPP's host),
// which is the intended overflow result. funct selects round/trunc/ceil/floor; cvt.w.s
// (funct 0x24) uses round-to-nearest-even (fcr31 rounding mode 0, the default).
inline uint32_t FpConvertToW(float x, uint32_t funct) {
	if (std::isnan(x) || std::isinf(x))
		return (std::isinf(x) && x < 0.0f) ? 0x80000000u : 0x7FFFFFFFu;
	int32_t r;
	switch (funct) {
		case 0x0C: r = (int32_t)std::floor(x + 0.5f); break;  // round.w.s: round half up
		case 0x0D:                                            // trunc.w.s
			if (x >= 0.0f) {
				r = (int32_t)std::floor(x);
				if (r == (int32_t)0x80000000) r = 0x7FFFFFFF;  // positive overflow guard
			} else {
				r = (int32_t)std::ceil(x);
			}
			break;
		case 0x0E: r = (int32_t)std::ceil(x); break;          // ceil.w.s
		case 0x0F: r = (int32_t)std::floor(x); break;         // floor.w.s
		default:   r = (int32_t)std::nearbyint(x); break;     // cvt.w.s: round-to-nearest-even
	}
	return (uint32_t)r;
}

// Compute a store's address and access size from the opcode, or size 0 if not a store.
// Mirrors how the trace records memory writes (TRACE_FORMAT.md), so it matches the oracle.
void StoreInfo(const CpuState *s, uint32_t op, uint32_t *addr, int *size) {
	*size = 0;
	uint32_t opcode = op >> 26;
	switch (opcode) {
		case 0x28: *size = 1; break;   // sb
		case 0x29: *size = 2; break;   // sh
		case 0x2B: *size = 4; break;   // sw
		case 0x2A: *size = 4; break;   // swl (records the touched word)
		case 0x2E: *size = 4; break;   // swr
		case 0x39: *size = 4; break;   // swc1
		default: return;
	}
	*addr = s->r[Rs(op)] + SImm(op);
}

}  // namespace

// ---- Execute one instruction ---------------------------------------------------------

// Advances s->pc and sets branch bookkeeping. Returns kRunning on success, or a stop reason
// for syscall / unimplemented / break. Memory faults are detected by the caller via the
// Memory fault flag after load/store.
static StopReason Execute(CpuState *s, Memory *mem, uint32_t op) {
	const uint32_t opcode = op >> 26;
	const uint32_t branch_pc = s->pc;
	s->pc += 4;  // default sequential advance; branches override via next_pc/in_delay_slot

	auto take_branch = [&](bool cond) {
		if (cond) {
			s->next_pc = branch_pc + 4 + (SImm(op) << 2);
			s->in_delay_slot = true;
		}
	};
	auto skip_likely = [&](bool cond) {
		// Branch-likely: when taken behaves like a normal branch; when not taken the delay
		// slot is annulled (skipped).
		if (cond) {
			s->next_pc = branch_pc + 4 + (SImm(op) << 2);
			s->in_delay_slot = true;
		} else {
			s->pc += 4;  // skip the delay-slot instruction
		}
	};

	switch (opcode) {
		case 0x00: {  // SPECIAL
			const uint32_t funct = Funct(op);
			switch (funct) {
				case 0x00: SetR(s, Rd(op), s->r[Rt(op)] << Sa(op)); return StopReason::kRunning;          // sll
				case 0x02: SetR(s, Rd(op), s->r[Rt(op)] >> Sa(op)); return StopReason::kRunning;          // srl
				case 0x03: SetR(s, Rd(op), (uint32_t)((int32_t)s->r[Rt(op)] >> Sa(op))); return StopReason::kRunning;  // sra
				case 0x04: SetR(s, Rd(op), s->r[Rt(op)] << (s->r[Rs(op)] & 31)); return StopReason::kRunning;          // sllv
				case 0x06: SetR(s, Rd(op), s->r[Rt(op)] >> (s->r[Rs(op)] & 31)); return StopReason::kRunning;          // srlv
				case 0x07: SetR(s, Rd(op), (uint32_t)((int32_t)s->r[Rt(op)] >> (s->r[Rs(op)] & 31))); return StopReason::kRunning;  // srav
				case 0x08: {  // jr
					s->next_pc = s->r[Rs(op)];
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x09: {  // jalr
					SetR(s, Rd(op), branch_pc + 8);
					s->next_pc = s->r[Rs(op)];
					s->in_delay_slot = true;
					return StopReason::kRunning;
				}
				case 0x0A: if ((int)Rt(op) >= 0) { if (s->r[Rt(op)] == 0) SetR(s, Rd(op), s->r[Rs(op)]); } return StopReason::kRunning;  // movz
				case 0x0B: if (s->r[Rt(op)] != 0) SetR(s, Rd(op), s->r[Rs(op)]); return StopReason::kRunning;  // movn
				case 0x0C: return StopReason::kSyscall;   // syscall
				// break: PPSSPP's Int_Break advances PC and continues (it does not halt in
				// headless). The compiler emits break after div as a divide-by-zero guard
				// that only executes on a zero divisor, so matching the oracle means
				// continuing past it rather than stopping.
				case 0x0D: return StopReason::kRunning;   // break
				case 0x10: SetR(s, Rd(op), s->hi); return StopReason::kRunning;  // mfhi
				case 0x11: s->hi = s->r[Rs(op)]; return StopReason::kRunning;    // mthi
				case 0x12: SetR(s, Rd(op), s->lo); return StopReason::kRunning;  // mflo
				case 0x13: s->lo = s->r[Rs(op)]; return StopReason::kRunning;    // mtlo
				case 0x18: {  // mult
					int64_t prod = (int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x19: {  // multu
					uint64_t prod = (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)prod; s->hi = (uint32_t)(prod >> 32); return StopReason::kRunning;
				}
				case 0x1A: {  // div (semantics match PPSSPP Int_MulDivType exactly)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					if (a == (int32_t)0x80000000 && b == -1) { s->lo = 0x80000000; s->hi = 0xFFFFFFFF; }
					else if (b != 0) { s->lo = (uint32_t)(a / b); s->hi = (uint32_t)(a % b); }
					else { s->lo = a < 0 ? 1u : 0xFFFFFFFFu; s->hi = (uint32_t)a; }
					return StopReason::kRunning;
				}
				case 0x1B: {  // divu
					uint32_t a = s->r[Rs(op)], b = s->r[Rt(op)];
					if (b != 0) { s->lo = a / b; s->hi = a % b; }
					else { s->lo = a <= 0xFFFF ? 0xFFFFu : 0xFFFFFFFFu; s->hi = a; }
					return StopReason::kRunning;
				}
				case 0x20:    // add (PSP does not trap on overflow in practice for our targets)
				case 0x21: SetR(s, Rd(op), s->r[Rs(op)] + s->r[Rt(op)]); return StopReason::kRunning;  // addu
				case 0x22:    // sub
				case 0x23: SetR(s, Rd(op), s->r[Rs(op)] - s->r[Rt(op)]); return StopReason::kRunning;  // subu
				case 0x24: SetR(s, Rd(op), s->r[Rs(op)] & s->r[Rt(op)]); return StopReason::kRunning;  // and
				case 0x25: SetR(s, Rd(op), s->r[Rs(op)] | s->r[Rt(op)]); return StopReason::kRunning;  // or
				case 0x26: SetR(s, Rd(op), s->r[Rs(op)] ^ s->r[Rt(op)]); return StopReason::kRunning;  // xor
				case 0x27: SetR(s, Rd(op), ~(s->r[Rs(op)] | s->r[Rt(op)])); return StopReason::kRunning;  // nor
				case 0x2A: SetR(s, Rd(op), (int32_t)s->r[Rs(op)] < (int32_t)s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // slt
				case 0x2B: SetR(s, Rd(op), s->r[Rs(op)] < s->r[Rt(op)] ? 1 : 0); return StopReason::kRunning;  // sltu
				// Allegrex places these in SPECIAL (not in a separate SPECIAL2 page like
				// generic MIPS32): clz/clo at 0x16/0x17, madd/maddu at 0x1C/0x1D, max/min at
				// 0x2C/0x2D, msub/msubu at 0x2E/0x2F. Encodings verified against PPSSPP's
				// MIPSTables.cpp SPECIAL table.
				case 0x16: {  // clz rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0 ? 32 : (uint32_t)__builtin_clz(v)); return StopReason::kRunning;
				}
				case 0x17: {  // clo rd, rs
					uint32_t v = s->r[Rs(op)];
					SetR(s, Rd(op), v == 0xFFFFFFFF ? 32 : (uint32_t)__builtin_clz(~v)); return StopReason::kRunning;
				}
				case 0x1C: {  // madd
					int64_t acc = ((int64_t)(int32_t)s->hi << 32) | (uint32_t)s->lo;
					acc += (int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x1D: {  // maddu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc += (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2C: {  // max rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a > b ? a : b)); return StopReason::kRunning;
				}
				case 0x2D: {  // min rd, rs, rt (signed)
					int32_t a = (int32_t)s->r[Rs(op)], b = (int32_t)s->r[Rt(op)];
					SetR(s, Rd(op), (uint32_t)(a < b ? a : b)); return StopReason::kRunning;
				}
				case 0x2E: {  // msub
					int64_t acc = ((int64_t)(int32_t)s->hi << 32) | (uint32_t)s->lo;
					acc -= (int64_t)(int32_t)s->r[Rs(op)] * (int64_t)(int32_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				case 0x2F: {  // msubu
					uint64_t acc = ((uint64_t)s->hi << 32) | s->lo;
					acc -= (uint64_t)s->r[Rs(op)] * (uint64_t)s->r[Rt(op)];
					s->lo = (uint32_t)acc; s->hi = (uint32_t)(acc >> 32); return StopReason::kRunning;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x01: {  // REGIMM
			const uint32_t rt = Rt(op);
			int32_t v = (int32_t)s->r[Rs(op)];
			switch (rt) {
				case 0x00: take_branch(v < 0); return StopReason::kRunning;   // bltz
				case 0x01: take_branch(v >= 0); return StopReason::kRunning;  // bgez
				case 0x02: skip_likely(v < 0); return StopReason::kRunning;   // bltzl
				case 0x03: skip_likely(v >= 0); return StopReason::kRunning;  // bgezl
				case 0x10: SetR(s, 31, branch_pc + 8); take_branch(v < 0); return StopReason::kRunning;   // bltzal
				case 0x11: SetR(s, 31, branch_pc + 8); take_branch(v >= 0); return StopReason::kRunning;  // bgezal
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x02: s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // j
		case 0x03: SetR(s, 31, branch_pc + 8); s->next_pc = (branch_pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2); s->in_delay_slot = true; return StopReason::kRunning;  // jal
		case 0x04: take_branch(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beq
		case 0x05: take_branch(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bne
		case 0x06: take_branch((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;    // blez
		case 0x07: take_branch((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;     // bgtz
		case 0x08:    // addi
		case 0x09: SetR(s, Rt(op), s->r[Rs(op)] + (uint32_t)SImm(op)); return StopReason::kRunning;  // addiu
		case 0x0A: SetR(s, Rt(op), (int32_t)s->r[Rs(op)] < SImm(op) ? 1 : 0); return StopReason::kRunning;  // slti
		case 0x0B: SetR(s, Rt(op), s->r[Rs(op)] < (uint32_t)SImm(op) ? 1 : 0); return StopReason::kRunning;  // sltiu
		case 0x0C: SetR(s, Rt(op), s->r[Rs(op)] & ZImm(op)); return StopReason::kRunning;  // andi
		case 0x0D: SetR(s, Rt(op), s->r[Rs(op)] | ZImm(op)); return StopReason::kRunning;  // ori
		case 0x0E: SetR(s, Rt(op), s->r[Rs(op)] ^ ZImm(op)); return StopReason::kRunning;  // xori
		case 0x0F: SetR(s, Rt(op), ZImm(op) << 16); return StopReason::kRunning;           // lui
		case 0x14: skip_likely(s->r[Rs(op)] == s->r[Rt(op)]); return StopReason::kRunning;  // beql
		case 0x15: skip_likely(s->r[Rs(op)] != s->r[Rt(op)]); return StopReason::kRunning;  // bnel
		case 0x16: skip_likely((int32_t)s->r[Rs(op)] <= 0); return StopReason::kRunning;    // blezl
		case 0x17: skip_likely((int32_t)s->r[Rs(op)] > 0); return StopReason::kRunning;     // bgtzl
		case 0x20: SetR(s, Rt(op), (uint32_t)(int32_t)(int8_t)mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lb
		case 0x21: SetR(s, Rt(op), (uint32_t)(int32_t)(int16_t)mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning; // lh
		case 0x23: SetR(s, Rt(op), mem->Read32(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lw
		case 0x24: SetR(s, Rt(op), mem->Read8(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;   // lbu
		case 0x25: SetR(s, Rt(op), mem->Read16(s->r[Rs(op)] + SImm(op))); return StopReason::kRunning;  // lhu
		case 0x28: mem->Write8(s->r[Rs(op)] + SImm(op), (uint8_t)s->r[Rt(op)]); return StopReason::kRunning;   // sb
		case 0x29: mem->Write16(s->r[Rs(op)] + SImm(op), (uint16_t)s->r[Rt(op)]); return StopReason::kRunning; // sh
		case 0x2B: mem->Write32(s->r[Rs(op)] + SImm(op), s->r[Rt(op)]); return StopReason::kRunning;  // sw
		case 0x11: {  // COP1 (single-precision FPU)
			const uint32_t fmt = Rs(op);  // sub-op selector in the rs field
			const uint32_t ft = Rt(op);
			const uint32_t fs = Rd(op);   // bits 15:11
			const uint32_t fd = Sa(op);   // bits 10:6
			switch (fmt) {
				case 0x00: SetR(s, ft, s->fi[fs]); return StopReason::kRunning;   // mfc1
				case 0x02:  // cfc1
					SetR(s, ft, fs == 31 ? s->fcr31 : (fs == 0 ? 0x00003351u : 0u));
					return StopReason::kRunning;
				case 0x04: s->fi[fs] = s->r[ft]; return StopReason::kRunning;     // mtc1
				case 0x06: if (fs == 31) s->fcr31 = s->r[ft]; return StopReason::kRunning;  // ctc1
				case 0x08: {  // bc1f / bc1t / bc1fl / bc1tl
					bool tf = (op >> 16) & 1;
					bool likely = (op >> 17) & 1;
					bool cond = s->fpcond != 0;
					if (likely) skip_likely(cond == tf); else take_branch(cond == tf);
					return StopReason::kRunning;
				}
				case 0x10: {  // fmt = S
					const uint32_t funct = Funct(op);
					switch (funct) {
						case 0x00: s->f[fd] = s->f[fs] + s->f[ft]; return StopReason::kRunning;  // add.s
						case 0x01: s->f[fd] = s->f[fs] - s->f[ft]; return StopReason::kRunning;  // sub.s
						case 0x02: {  // mul.s; PPSSPP forces inf*0 to the positive canonical NaN
							float a = s->f[fs], b = s->f[ft];
							if ((std::isinf(a) && b == 0.0f) || (std::isinf(b) && a == 0.0f))
								s->fi[fd] = 0x7fc00000;
							else
								s->f[fd] = a * b;
							return StopReason::kRunning;
						}
						case 0x03: s->f[fd] = s->f[fs] / s->f[ft]; return StopReason::kRunning;  // div.s
						case 0x04: s->f[fd] = std::sqrt(s->f[fs]); return StopReason::kRunning;  // sqrt.s
						case 0x05: s->f[fd] = std::fabs(s->f[fs]); return StopReason::kRunning;  // abs.s
						case 0x06: s->f[fd] = s->f[fs]; return StopReason::kRunning;             // mov.s
						case 0x07: s->f[fd] = -s->f[fs]; return StopReason::kRunning;            // neg.s
						case 0x0C: s->fi[fd] = FpConvertToW(s->f[fs], 0x0C); return StopReason::kRunning;  // round.w.s
						case 0x0D: s->fi[fd] = FpConvertToW(s->f[fs], 0x0D); return StopReason::kRunning;  // trunc.w.s
						case 0x0E: s->fi[fd] = FpConvertToW(s->f[fs], 0x0E); return StopReason::kRunning;  // ceil.w.s
						case 0x0F: s->fi[fd] = FpConvertToW(s->f[fs], 0x0F); return StopReason::kRunning;  // floor.w.s
						case 0x24: s->fi[fd] = FpConvertToW(s->f[fs], 0x24); return StopReason::kRunning;  // cvt.w.s
						default:
							if (funct >= 0x30) {  // c.cond.s
								uint32_t cond = funct & 0xF;
								float a = s->f[fs], b = s->f[ft];
								bool unordered = std::isnan(a) || std::isnan(b);
								bool less = !unordered && a < b;
								bool equal = !unordered && a == b;
								bool result = (unordered && (cond & 1)) || (equal && (cond & 2)) || (less && (cond & 4));
								s->fpcond = result ? 1 : 0;  // PPSSPP stores the condition here, not in fcr31
								return StopReason::kRunning;
							}
							return StopReason::kUnimplemented;
					}
				}
				case 0x14: {  // fmt = W
					if (Funct(op) == 0x20) { s->f[fd] = (float)(int32_t)s->fi[fs]; return StopReason::kRunning; }  // cvt.s.w
					return StopReason::kUnimplemented;
				}
				default: return StopReason::kUnimplemented;
			}
		}
		case 0x1F: {  // SPECIAL3
			const uint32_t funct = Funct(op);
			if (funct == 0x00) {  // ext rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t size = ((op >> 11) & 0x1F) + 1;
				uint32_t mask = size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1);
				SetR(s, Rt(op), (s->r[Rs(op)] >> pos) & mask);
				return StopReason::kRunning;
			}
			if (funct == 0x04) {  // ins rt, rs, pos, size
				uint32_t pos = Sa(op);
				uint32_t msb = (op >> 11) & 0x1F;
				uint32_t size = msb - pos + 1;
				uint32_t mask = (size >= 32 ? 0xFFFFFFFFu : ((1u << size) - 1)) << pos;
				SetR(s, Rt(op), (s->r[Rt(op)] & ~mask) | ((s->r[Rs(op)] << pos) & mask));
				return StopReason::kRunning;
			}
			if (funct == 0x20) {  // BSHFL: sub-op in sa field
				uint32_t sub = Sa(op);
				uint32_t v = s->r[Rt(op)];
				if (sub == 0x02) { SetR(s, Rd(op), ((v & 0x00FF00FF) << 8) | ((v >> 8) & 0x00FF00FF)); return StopReason::kRunning; }  // wsbh
				if (sub == 0x10) { SetR(s, Rd(op), (uint32_t)(int32_t)(int8_t)v); return StopReason::kRunning; }   // seb
				if (sub == 0x18) { SetR(s, Rd(op), (uint32_t)(int32_t)(int16_t)v); return StopReason::kRunning; }  // seh
				return StopReason::kUnimplemented;
			}
			return StopReason::kUnimplemented;
		}
		case 0x31: s->fi[Rt(op)] = mem->Read32(s->r[Rs(op)] + SImm(op)); return StopReason::kRunning; // lwc1
		case 0x39: mem->Write32(s->r[Rs(op)] + SImm(op), s->fi[Rt(op)]); return StopReason::kRunning; // swc1
		default: return StopReason::kUnimplemented;
	}
}

StepResult Run(CpuState *s, Memory *mem, unsigned long long max_steps, TraceSink *sink) {
	for (unsigned long long i = 0; i < max_steps; i++) {
		uint32_t pc = s->pc;
		uint32_t op = mem->Read32(pc);
		if (mem->last_fault())
			return {StopReason::kMemoryFault, pc, op};

		uint32_t store_addr = 0;
		int store_size = 0;
		StoreInfo(s, op, &store_addr, &store_size);

		if (sink) sink->BeginStep(s, pc, op);
		bool was_in_delay_slot = s->in_delay_slot;
		StopReason reason = Execute(s, mem, op);
		if (s->in_delay_slot && was_in_delay_slot) {
			s->pc = s->next_pc;
			s->in_delay_slot = false;
		}
		if (sink) sink->EndStep(s, store_addr, store_size, mem);

		if (mem->last_fault())
			return {StopReason::kMemoryFault, pc, op};
		if (reason != StopReason::kRunning)
			return {reason, pc, op};
	}
	return {StopReason::kStepLimit, s->pc, 0};
}

}  // namespace ref
