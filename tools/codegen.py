#!/usr/bin/env python3
# Allegrex -> C codegen (Phase 3). Reads an ELF and a function list (TOML from analyze.py,
# or all discovered functions) and emits one C function per guest function:
#   void f_<hexaddr>(CpuState *s);
# per ARCHITECTURE.md section 6. Each instruction becomes one C statement, with branch
# delay-slot reordering, intra-function branches as labels + goto, direct calls for known
# targets, jr $ra as return, and computed transfers through dispatch(). Every emitted
# instruction is wrapped in sr_begin/sr_end trace hooks (in PPSSPP's branch-then-delay
# order) so the generated code can be diffed against the reference trace.

import struct
import sys

from analyze import Elf, exec_ranges, in_ranges, analyze

R = lambda i: "0u" if i == 0 else f"s->r[{i}]"           # read GPR (r0 is constant 0)
F = lambda i: f"s->f[{i}]"
FI = lambda i: f"s->fi[{i}]"


def rs(w): return (w >> 21) & 0x1F
def rt(w): return (w >> 16) & 0x1F
def rd(w): return (w >> 11) & 0x1F
def sa(w): return (w >> 6) & 0x1F
def funct(w): return w & 0x3F
def simm(w): return f"0x{((w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF) & 0xFFFFFFFF:08x}u"
def zimm(w): return f"0x{w & 0xFFFF:x}u"
def s16(w): return (w & 0xFFFF) - 0x10000 if w & 0x8000 else w & 0xFFFF


def wr(i, expr):
    # Assignment to GPR i; writes to r0 are dropped (ARCHITECTURE section 4).
    return "(void)0;" if i == 0 else f"s->r[{i}] = {expr};"


def vreg_indices(reg, size):
    # Physical v[] indices for a VFPU vector register, matching PPSSPP's voffset-integrated
    # addressing (MIPSVFPUUtils.cpp). size is the number of lanes (1=single..4=quad).
    mtx = (reg >> 2) & 7
    col = reg & 3
    transpose = (reg >> 5) & 1
    if size == 1:
        transpose = 0; row = (reg >> 5) & 3; length = 1
    elif size == 2:
        row = (reg >> 5) & 2; length = 2
    elif size == 3:
        row = (reg >> 6) & 1; length = 3
    else:
        row = (reg >> 5) & 2; length = 4
    out = []
    for i in range(length):
        if transpose:
            out.append(mtx * 16 + ((row + i) & 3) * 4 + col)
        else:
            out.append(mtx * 16 + col * 4 + ((row + i) & 3))
    return out


def vec_size(w):  # number of lanes from the VFPU size bits
    return (((w >> 7) & 1) | ((w >> 14) & 2)) + 1


def mreg_index(reg, side, j, i):
    # Physical v[] index of a matrix element (column j, row i), matching PPSSPP ReadMatrix.
    mtx = (reg >> 2) & 7
    col = reg & 3
    transpose = (reg >> 5) & 1
    if side == 1:
        transpose = 0; row = (reg >> 5) & 3
    elif side == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    if transpose:
        return mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3)
    return mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3)


class Unsupported(Exception):
    pass


# Effect of a non-control instruction -> (c_statement, store_addr_expr_or_None, store_size).
def effect(addr, w):
    op = w >> 26
    if op == 0:
        fn = funct(w)
        a, b, d, sh = rs(w), rt(w), rd(w), sa(w)
        if fn == 0x00: return wr(d, f"({R(b)} << {sh})"), None, 0           # sll
        if fn == 0x02: return wr(d, f"({R(b)} >> {sh})"), None, 0           # srl
        if fn == 0x03: return wr(d, f"((uint32_t)((int32_t){R(b)} >> {sh}))"), None, 0  # sra
        if fn == 0x04: return wr(d, f"({R(b)} << ({R(a)} & 31))"), None, 0  # sllv
        if fn == 0x06: return wr(d, f"({R(b)} >> ({R(a)} & 31))"), None, 0  # srlv
        if fn == 0x07: return wr(d, f"((uint32_t)((int32_t){R(b)} >> ({R(a)} & 31)))"), None, 0  # srav
        if fn == 0x0A: return f"if ({R(b)} == 0) {wr(d, R(a))}", None, 0    # movz
        if fn == 0x0B: return f"if ({R(b)} != 0) {wr(d, R(a))}", None, 0    # movn
        if fn == 0x0D: return "/* break: continue (matches PPSSPP headless) */ (void)0;", None, 0
        if fn == 0x10: return wr(d, "s->hi"), None, 0                       # mfhi
        if fn == 0x11: return f"s->hi = {R(a)};", None, 0                   # mthi
        if fn == 0x12: return wr(d, "s->lo"), None, 0                       # mflo
        if fn == 0x13: return f"s->lo = {R(a)};", None, 0                   # mtlo
        if fn == 0x16: return wr(d, f"({R(a)} == 0 ? 32u : (uint32_t)__builtin_clz({R(a)}))"), None, 0  # clz
        if fn == 0x17: return wr(d, f"({R(a)} == 0xFFFFFFFFu ? 32u : (uint32_t)__builtin_clz(~{R(a)}))"), None, 0  # clo
        if fn == 0x18: return f"{{ int64_t _p = (int64_t)(int32_t){R(a)} * (int64_t)(int32_t){R(b)}; s->lo = (uint32_t)_p; s->hi = (uint32_t)(_p >> 32); }}", None, 0  # mult
        if fn == 0x19: return f"{{ uint64_t _p = (uint64_t){R(a)} * (uint64_t){R(b)}; s->lo = (uint32_t)_p; s->hi = (uint32_t)(_p >> 32); }}", None, 0  # multu
        if fn == 0x1A: return ("{ int32_t _a=(int32_t)%s, _b=(int32_t)%s; if (_a==(int32_t)0x80000000 && _b==-1){s->lo=0x80000000u;s->hi=0xFFFFFFFFu;} "
                               "else if (_b!=0){s->lo=(uint32_t)(_a/_b);s->hi=(uint32_t)(_a%%_b);} else {s->lo=_a<0?1u:0xFFFFFFFFu;s->hi=(uint32_t)_a;} }") % (R(a), R(b)), None, 0  # div
        if fn == 0x1B: return ("{ uint32_t _a=%s,_b=%s; if(_b!=0){s->lo=_a/_b;s->hi=_a%%_b;} else {s->lo=_a<=0xFFFFu?0xFFFFu:0xFFFFFFFFu;s->hi=_a;} }") % (R(a), R(b)), None, 0  # divu
        if fn == 0x1C: return f"{{ int64_t _acc=((int64_t)(int32_t)s->hi<<32)|(uint32_t)s->lo; _acc+=(int64_t)(int32_t){R(a)}*(int64_t)(int32_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # madd
        if fn == 0x1D: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; _acc+=(uint64_t){R(a)}*(uint64_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # maddu
        if fn == 0x20 or fn == 0x21: return wr(d, f"({R(a)} + {R(b)})"), None, 0  # add/addu
        if fn == 0x22 or fn == 0x23: return wr(d, f"({R(a)} - {R(b)})"), None, 0  # sub/subu
        if fn == 0x24: return wr(d, f"({R(a)} & {R(b)})"), None, 0          # and
        if fn == 0x25: return wr(d, f"({R(a)} | {R(b)})"), None, 0          # or
        if fn == 0x26: return wr(d, f"({R(a)} ^ {R(b)})"), None, 0          # xor
        if fn == 0x27: return wr(d, f"(~({R(a)} | {R(b)}))"), None, 0       # nor
        if fn == 0x2A: return wr(d, f"((int32_t){R(a)} < (int32_t){R(b)} ? 1u : 0u)"), None, 0  # slt
        if fn == 0x2B: return wr(d, f"({R(a)} < {R(b)} ? 1u : 0u)"), None, 0  # sltu
        if fn == 0x2C: return wr(d, f"({{int32_t _x=(int32_t){R(a)},_y=(int32_t){R(b)}; (uint32_t)(_x>_y?_x:_y);}})"), None, 0  # max
        if fn == 0x2D: return wr(d, f"({{int32_t _x=(int32_t){R(a)},_y=(int32_t){R(b)}; (uint32_t)(_x<_y?_x:_y);}})"), None, 0  # min
        if fn == 0x2E: return f"{{ int64_t _acc=((int64_t)(int32_t)s->hi<<32)|(uint32_t)s->lo; _acc-=(int64_t)(int32_t){R(a)}*(int64_t)(int32_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # msub
        if fn == 0x2F: return f"{{ uint64_t _acc=((uint64_t)s->hi<<32)|s->lo; _acc-=(uint64_t){R(a)}*(uint64_t){R(b)}; s->lo=(uint32_t)_acc; s->hi=(uint32_t)(_acc>>32);}}", None, 0  # msubu
        raise Unsupported(f"SPECIAL funct 0x{fn:02x} at 0x{addr:08x}")
    if op == 0x08 or op == 0x09: return wr(rt(w), f"({R(rs(w))} + {simm(w)})"), None, 0  # addi/addiu
    if op == 0x0A: return wr(rt(w), f"((int32_t){R(rs(w))} < (int32_t){simm(w)} ? 1u : 0u)"), None, 0  # slti
    if op == 0x0B: return wr(rt(w), f"({R(rs(w))} < {simm(w)} ? 1u : 0u)"), None, 0  # sltiu
    if op == 0x0C: return wr(rt(w), f"({R(rs(w))} & {zimm(w)})"), None, 0  # andi
    if op == 0x0D: return wr(rt(w), f"({R(rs(w))} | {zimm(w)})"), None, 0  # ori
    if op == 0x0E: return wr(rt(w), f"({R(rs(w))} ^ {zimm(w)})"), None, 0  # xori
    if op == 0x0F: return wr(rt(w), f"({zimm(w)} << 16)"), None, 0          # lui
    if op == 0x1F:  # SPECIAL3
        fn = funct(w)
        if fn == 0x00:  # ext
            pos, size = sa(w), ((w >> 11) & 0x1F) + 1
            mask = 0xFFFFFFFF if size >= 32 else ((1 << size) - 1)
            return wr(rt(w), f"(({R(rs(w))} >> {pos}) & 0x{mask:x}u)"), None, 0
        if fn == 0x04:  # ins
            pos, msb = sa(w), (w >> 11) & 0x1F
            size = msb - pos + 1
            mask = ((0xFFFFFFFF if size >= 32 else ((1 << size) - 1)) << pos) & 0xFFFFFFFF
            return wr(rt(w), f"(({R(rt(w))} & ~0x{mask:x}u) | (({R(rs(w))} << {pos}) & 0x{mask:x}u))"), None, 0
        if fn == 0x20:
            sub = sa(w)
            if sub == 0x02: return wr(rd(w), f"((({R(rt(w))} & 0x00FF00FFu) << 8) | (({R(rt(w))} >> 8) & 0x00FF00FFu))"), None, 0  # wsbh
            if sub == 0x10: return wr(rd(w), f"((uint32_t)(int32_t)(int8_t){R(rt(w))})"), None, 0   # seb
            if sub == 0x18: return wr(rd(w), f"((uint32_t)(int32_t)(int16_t){R(rt(w))})"), None, 0  # seh
        raise Unsupported(f"SPECIAL3 funct 0x{fn:02x} at 0x{addr:08x}")
    # loads
    if op == 0x20: return wr(rt(w), f"((uint32_t)(int32_t)(int8_t)MEM_R8({R(rs(w))} + {simm(w)}))"), None, 0   # lb
    if op == 0x21: return wr(rt(w), f"((uint32_t)(int32_t)(int16_t)MEM_R16({R(rs(w))} + {simm(w)}))"), None, 0  # lh
    if op == 0x23: return wr(rt(w), f"MEM_R32({R(rs(w))} + {simm(w)})"), None, 0   # lw
    if op == 0x24: return wr(rt(w), f"MEM_R8({R(rs(w))} + {simm(w)})"), None, 0    # lbu
    if op == 0x25: return wr(rt(w), f"MEM_R16({R(rs(w))} + {simm(w)})"), None, 0   # lhu
    # stores: the address expression is recomputable (the base reg is not changed by the
    # store), so the trace hook can read the stored bytes back from the same address.
    if op == 0x28: return f"MEM_W8({R(rs(w))} + {simm(w)}, {R(rt(w))});", f"({R(rs(w))} + {simm(w)})", 1   # sb
    if op == 0x29: return f"MEM_W16({R(rs(w))} + {simm(w)}, {R(rt(w))});", f"({R(rs(w))} + {simm(w)})", 2  # sh
    if op == 0x2B: return f"MEM_W32({R(rs(w))} + {simm(w)}, {R(rt(w))});", f"({R(rs(w))} + {simm(w)})", 4  # sw
    # Unaligned word access (lwl/lwr/swl/swr). Loads merge with the current rt; stores
    # read-modify-write the aligned word, so the trace hook reads back the aligned word (4B).
    if op == 0x22: return wr(rt(w), f"sr_lwl({R(rt(w))}, {R(rs(w))} + {simm(w)})"), None, 0   # lwl
    if op == 0x26: return wr(rt(w), f"sr_lwr({R(rt(w))}, {R(rs(w))} + {simm(w)})"), None, 0   # lwr
    if op == 0x2A: return f"sr_swl({R(rs(w))} + {simm(w)}, {R(rt(w))});", f"(({R(rs(w))} + {simm(w)}) & ~3u)", 4  # swl
    if op == 0x2E: return f"sr_swr({R(rs(w))} + {simm(w)}, {R(rt(w))});", f"(({R(rs(w))} + {simm(w)}) & ~3u)", 4  # swr
    if op == 0x31: return f"s->fi[{rt(w)}] = MEM_R32({R(rs(w))} + {simm(w)});", None, 0  # lwc1
    if op == 0x39: return f"MEM_W32({R(rs(w))} + {simm(w)}, s->fi[{rt(w)}]);", f"({R(rs(w))} + {simm(w)})", 4  # swc1
    if op == 0x11: return fpu_effect(addr, w)
    if op in (0x36, 0x3e, 0x32, 0x3a, 0x12, 0x18, 0x19, 0x1b, 0x37, 0x34, 0x3c): return vfpu_effect(addr, w)
    raise Unsupported(f"opcode 0x{op:02x} at 0x{addr:08x}")


def _arr(idx):
    return "(const uint8_t[]){" + ",".join(str(x) for x in idx) + "}"


_EAT = " s->vfpuCtrl[0]=0xe4u; s->vfpuCtrl[1]=0xe4u; s->vfpuCtrl[2]=0u;"


def _half_to_f32_bits(h):
    # float16 -> float32 bit pattern (IEEE half, like PPSSPP Float16ToFloat32)
    s = (h >> 15) & 1
    e = (h >> 10) & 0x1F
    m = h & 0x3FF
    if e == 0:
        if m == 0:
            return s << 31
        e2 = 127 - 15 + 1
        while not (m & 0x400):
            m <<= 1
            e2 -= 1
        return (s << 31) | (e2 << 23) | ((m & 0x3FF) << 13)
    if e == 31:
        return (s << 31) | (0xFF << 23) | (m << 13)
    return (s << 31) | ((e - 15 + 127) << 23) | (m << 13)


def _f32(x):  # round a Python double to float32, like a C float intermediate
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _vfpu_cst():
    # vcst constant table; same float32 expressions as the interpreter (vfpu_interp.c)
    # and PPSSPP (MIPS.cpp), with float32 rounding applied at each intermediate step.
    import math
    PI, E = math.pi, math.e
    c = [0.0] * 32
    c[1] = _f32(3.4028234663852886e38)               # FLT_MAX
    c[2] = _f32(math.sqrt(2.0))                      # sqrtf(2.0f)
    c[3] = _f32(math.sqrt(0.5))                      # sqrtf(0.5f)
    c[4] = _f32(2.0 / _f32(math.sqrt(_f32(PI))))     # 2.0f / sqrtf((float)PI)
    c[5] = _f32(2.0 / _f32(PI))
    c[6] = _f32(1.0 / _f32(PI))
    c[7] = _f32(_f32(PI) / 4)
    c[8] = _f32(_f32(PI) / 2)
    c[9] = _f32(PI)
    c[10] = _f32(E)
    c[11] = _f32(1.44269504088896340736)             # LOG2E
    c[12] = _f32(0.43429448190325182765)             # LOG10E
    c[13] = _f32(0.69314718055994530942)             # LN2
    c[14] = _f32(2.30258509299404568402)             # LN10
    c[15] = _f32(2 * _f32(PI))
    c[16] = _f32(_f32(PI) / 6)
    c[17] = _f32(math.log10(2.0))                    # log10f(2.0f)
    c[18] = _f32(_f32(math.log(10.0)) / _f32(math.log(2.0)))  # logf(10)/logf(2)
    c[19] = _f32(_f32(math.sqrt(3.0)) / 2.0)         # sqrtf(3.0f) / 2.0f
    return c


_VFPU_CST = _vfpu_cst()


def _flit(v):  # C float literal that round-trips the float32 value exactly
    s = f"{v:.9g}"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def vfpu_effect(addr, w):
    op = w >> 26
    # Opcode 0x37 splits on (w>>24)&3: 0/1/2 = vpfxs/vpfxt/vpfxd (set the prefix register),
    # 3 = viim/vfim (load an immediate into a single VFPU register — NOT a prefix write;
    # decoding these as a vfpuCtrl[3] store silently clobbered VFPU_CC and dropped the
    # immediate, which zeroed the -1 in gum perspective matrices).
    if op == 0x37:
        regnum = (w >> 24) & 3
        if regnum == 3:  # viim (bit23=0): signed 16-bit int; vfim (bit23=1): float16
            vt = (w >> 16) & 0x7F
            imm = w & 0xFFFF
            if (w >> 23) & 1:
                bits = _half_to_f32_bits(imm)
            else:
                iv = imm - 0x10000 if imm & 0x8000 else imm
                bits = struct.unpack("<I", struct.pack("<f", float(iv)))[0]
            i0 = vreg_indices(vt, 1)[0]
            body = (f"uint32_t _bits=0x{bits:08x}u; float _d[1]; memcpy(_d,&_bits,4); "
                    f"sr_vwrite(s,{_arr([i0])},_d,1,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        data = w & 0xFFFFF
        if regnum == 2:
            data &= 0xFFF
        return f"s->vfpuCtrl[{regnum}] = 0x{data:x}u;", None, 0
    # VFPU4 jump group (0x34): bits 21-25 select the sub-table (PPSSPP tableVFPU4Jump).
    # Index 0 is the VV2Op single-source group; index 21 is vcmov. Anything else is
    # unsupported and must trap loudly rather than silently mis-decode as vmov.
    if op == 0x34:
        vd, vs = w & 0x7F, (w >> 8) & 0x7F
        n = vec_size(w)
        jump = (w >> 21) & 0x1F
        di = vreg_indices(vd, n)
        if jump == 0x15:
            # vcmov: conditional move on VFPU_CC (vfpuCtrl[3]). tf=0 -> vcmovt (move when
            # the CC bit is set), tf=1 -> vcmovf. D is also read as T, so the T prefix
            # applies to it (matches PPSSPP Int_Vcmov).
            tf = (w >> 19) & 1
            imm3 = (w >> 16) & 7
            si = vreg_indices(vs, n)
            rd_st = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                     f"sr_vread(_d,s,{_arr(di)},{n},s->vfpuCtrl[1]); ")
            if imm3 < 6:
                move = (f"if (((s->vfpuCtrl[3] >> {imm3}) & 1u) == {1 - tf}u) "
                        f"{{ for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]; }} ")
            elif imm3 == 6:
                move = (f"for(int _i=0;_i<{n};_i++) "
                        f"if (((s->vfpuCtrl[3] >> _i) & 1u) == {1 - tf}u) _d[_i]=_s[_i]; ")
            else:
                raise Unsupported(f"vcmov imm3 {imm3} at 0x{addr:08x}")
            body = rd_st + move + f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}"
            return "{ " + body + " }", None, 0
        if jump == 0x02:  # VFPU9 group, sub-indexed by bits 16-20
            op9 = (w >> 16) & 0x1F
            if op9 == 4:  # vocp: d = 1.0 - s. Hardware computes it as t + s with forced
                          # prefixes (S gains negate-all, T forced to constant ONE); NaN
                          # stays positive (matches PPSSPP Int_Vocp).
                si = vreg_indices(vs, n)
                body = (f"float _s[4],_t[4],_d[4]; "
                        f"sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]|0xF0000u); "
                        f"sr_vread(_t,s,{_arr(si)},{n},(s->vfpuCtrl[1]&~0xFFu)|0x55u|0xF000u); "
                        f"for(int _i=0;_i<{n};_i++) _d[_i]=isnan(_s[_i])?fabsf(_s[_i]):_t[_i]+_s[_i]; "
                        f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
                return "{ " + body + " }", None, 0
            raise Unsupported(f"VFPU9 op 0x{op9:02x} at 0x{addr:08x}")
        if jump == 0x03:  # vcst: broadcast a VFPU constant (resolved at codegen time)
            val = _flit(_VFPU_CST[(w >> 16) & 0x1F])
            body = (f"float _d[4]; for(int _i=0;_i<{n};_i++) _d[_i]={val}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if jump != 0:
            raise Unsupported(f"VFPU4 jump 0x{jump:02x} at 0x{addr:08x}")
        optype = (w >> 16) & 0x1F
        if optype in (0, 1, 2, 4, 5):
            si = vreg_indices(vs, n)
            per = {0: "_s[_i]", 1: "fabsf(_s[_i])", 2: "-_s[_i]",
                   4: "(_s[_i]<=0.0f?0.0f:(_s[_i]>1.0f?1.0f:_s[_i]))",
                   5: "(_s[_i]<-1.0f?-1.0f:(_s[_i]>1.0f?1.0f:_s[_i]))"}[optype]
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++) _d[_i]={per}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        _TRANS = {16: "sr_vfpu_rcp(_s[_i])", 17: "sr_vfpu_rsqrt(_s[_i])",
                  18: "sr_vfpu_sin(_s[_i])", 19: "sr_vfpu_cos(_s[_i])",
                  20: "sr_vfpu_exp2(_s[_i])",
                  22: "sr_vfpu_sqrt(_s[_i])",
                  24: "-sr_vfpu_rcp(_s[_i])", 26: "-sr_vfpu_sin(_s[_i])"}
        if optype in _TRANS:
            si = vreg_indices(vs, n)
            body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"for(int _i=0;_i<{n};_i++) _d[_i]={_TRANS[optype]}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if optype in (6, 7):  # vzero / vone
            val = "0.0f" if optype == 6 else "1.0f"
            body = (f"float _d[4]; for(int _i=0;_i<{n};_i++) _d[_i]={val}; "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if optype == 3:  # vidt: identity column, 1.0 at the diagonal offset, else 0.0
            offmask = 3 if n >= 3 else 1
            off = vd & offmask
            vals = ",".join("1.0f" if i == off else "0.0f" for i in range(n))
            body = (f"float _d[4]={{{vals}}}; sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        raise Unsupported(f"VV2Op optype {optype} at 0x{addr:08x}")
    # lv.q / sv.q: quad load/store. vt uses bit 0 (not bits 0-1) for the high register bit.
    if op == 0x36 or op == 0x3e:
        vt = ((w >> 16) & 0x1F) | ((w & 1) << 5)
        idx = vreg_indices(vt, 4)
        base = f"({R(rs(w))} + {simm(w)})"
        if op == 0x36:  # lv.q
            parts = " ".join(f"s->vi[{idx[i]}] = MEM_R32(_a + {i*4});" for i in range(4))
            return f"{{ uint32_t _a = {base}; {parts} }}", None, 0
        parts = " ".join(f"MEM_W32(_a + {i*4}, s->vi[{idx[i]}]);" for i in range(4))
        return f"{{ uint32_t _a = {base}; {parts} }}", base, 16  # sv.q
    # lv.s / sv.s: single-lane load/store. vt = (rt<<0) | (low 2 bits << 5); offset is the
    # 16-bit imm with its low 2 bits cleared (they carry the high register bits).
    if op == 0x32 or op == 0x3a:
        vt = ((w >> 16) & 0x1F) | ((w & 3) << 5)
        i0 = vreg_indices(vt, 1)[0]
        off = (w & 0xFFFC)
        off = off - 0x10000 if off & 0x8000 else off
        addr_e = f"({R(rs(w))} + {off})"
        if op == 0x32:  # lv.s
            return f"s->vi[{i0}] = MEM_R32({addr_e});", None, 0
        return f"MEM_W32({addr_e}, s->vi[{i0}]);", addr_e, 4  # sv.s
    # COP2 mfc2/mtc2 (and the imm>=128 control-register forms cfc2/ctc2). imm selects a
    # single VFPU register (voffset-mapped) or a vfpuCtrl entry; rs picks the direction.
    if op == 0x12:
        sub = (w >> 21) & 0x1F
        imm = w & 0xFF
        if imm < 128:
            vidx = vreg_indices(imm, 1)[0]
            src, dst = f"s->vi[{vidx}]", f"s->vi[{vidx}]"
        else:
            src = dst = f"s->vfpuCtrl[{imm - 128}]"
        if sub == 3:    # mfv/mfvc: GPR <- VFPU (PSP encoding)
            return wr(rt(w), src), None, 0
        if sub == 7:    # mtv/mtvc: VFPU <- GPR
            return f"{dst} = {R(rt(w))};", None, 0
        raise Unsupported(f"cop2 sub {sub} at 0x{addr:08x}")
    # VFPU0 (0x18): vadd/vsub/vdiv ; VFPU1 (0x19): vmul/vscl. Sources read into temps first
    # so an in-place destination (vd overlapping vs/vt) is handled correctly.
    # Vector ALU with the source/dest prefix system. Sources are read (and prefixed) into
    # locals, the op runs, the destination prefix (saturate + write mask) is applied on
    # write, then the S/T/D prefixes are reset to default (eaten), matching PPSSPP.
    vd, vs, vt = w & 0x7F, (w >> 8) & 0x7F, (w >> 16) & 0x7F
    n = vec_size(w)
    sub = (w >> 23) & 7
    di, si = vreg_indices(vd, n), vreg_indices(vs, n)
    # VFPU3 (0x1b): vcmp sets the CC register; vmin/vmax are per-lane (int compare on NaN/inf).
    # Mirrors src/rt/vfpu_interp.c, which is verified against the ACX reference trace.
    if op == 0x1b:
        ti = vreg_indices(vt, n)
        if sub == 0:  # vcmp: condition is fixed at codegen time, so emit the lane predicate.
            cond = w & 0xF
            _C = {0: "0", 1: "_x==_y", 2: "_x<_y", 3: "_x<=_y", 4: "1", 5: "_x!=_y",
                  6: "_x>=_y", 7: "_x>_y", 8: "_x==0.0f", 9: "isnan(_x)", 10: "isinf(_x)",
                  11: "(isnan(_x)||isinf(_x))", 12: "_x!=0.0f", 13: "!isnan(_x)",
                  14: "!isinf(_x)", 15: "!(isnan(_x)||isinf(_x))"}[cond]
            body = (f"float _a[4],_b[4]; sr_vread(_a,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"sr_vread(_b,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                    f"int _cc=0,_or=0,_and=1,_aff=(1<<4)|(1<<5); "
                    f"for(int _i=0;_i<{n};_i++){{ float _x=_a[_i],_y=_b[_i]; int _c=({_C}); "
                    f"_cc|=(_c<<_i);_or|=_c;_and&=_c;_aff|=1<<_i; }} "
                    f"s->vfpuCtrl[3]=(s->vfpuCtrl[3]&~_aff)|((_cc|(_or<<4)|(_and<<5))&_aff);{_EAT}")
            return "{ " + body + " }", None, 0
        if sub in (2, 3):  # vmin / vmax
            ismin = "1" if sub == 2 else "0"
            body = (f"float _a[4],_b[4],_d[4]; sr_vread(_a,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                    f"sr_vread(_b,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                    f"for(int _i=0;_i<{n};_i++){{ int _an=isnan(_a[_i])||isinf(_a[_i]),_bn=isnan(_b[_i])||isinf(_b[_i]); "
                    f"if(_an||_bn){{ int32_t _ai,_bi,_r; memcpy(&_ai,&_a[_i],4); memcpy(&_bi,&_b[_i],4); "
                    f"if({ismin}) _r=(_ai<0&&_bi<0)?(_bi<_ai?_ai:_bi):(_ai<_bi?_ai:_bi); "
                    f"else _r=(_ai<0&&_bi<0)?(_ai<_bi?_ai:_bi):(_bi<_ai?_ai:_bi); memcpy(&_d[_i],&_r,4); }} "
                    f"else _d[_i]={ismin}?(_a[_i]<_b[_i]?_a[_i]:_b[_i]):(_b[_i]<_a[_i]?_a[_i]:_b[_i]); }} "
                    f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        raise Unsupported(f"VFPU3 sub {sub} at 0x{addr:08x}")
    # VFPUMatrix1 (0x3c, bits[25:21]==28): vmidt(3)/vmzero(6)/vmone(7) NxN matrix init.
    if op == 0x3c and sub == 7 and ((w >> 21) & 0x1F) == 28:
        which = (w >> 16) & 0xF
        if which in (3, 6, 7):
            side = n
            writes = []
            for j in range(side):
                for i in range(side):
                    if which == 3:    val = "1.0f" if i == j else "0.0f"  # vmidt: identity
                    elif which == 6:  val = "0.0f"                        # vmzero
                    else:             val = "1.0f"                        # vmone
                    writes.append(f"s->v[{mreg_index(vd, side, j, i)}]={val};")
            return "{ " + " ".join(writes) + _EAT + " }", None, 0
        raise Unsupported(f"VFPUMatrix1 which {which} at 0x{addr:08x}")
    # vcrsp.t (triple) / vqmul.q (quad): cross product / quaternion multiply (PPSSPP CrossQuat).
    if op == 0x3c and sub == 5:
        si, ti, di = vreg_indices(vs, n), vreg_indices(vt, n), vreg_indices(vd, n)
        if n == 4:
            body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},4,s->vfpuCtrl[0]); "
                    f"sr_vread(_t,s,{_arr(ti)},4,s->vfpuCtrl[1]); "
                    f"_d[0]=_s[0]*_t[3]+_s[1]*_t[2]-_s[2]*_t[1]+_s[3]*_t[0]; "
                    f"_d[1]=-_s[0]*_t[2]+_s[1]*_t[3]+_s[2]*_t[0]+_s[3]*_t[1]; "
                    f"_d[2]=_s[0]*_t[1]-_s[1]*_t[0]+_s[2]*_t[3]+_s[3]*_t[2]; "
                    f"_d[3]=-_s[0]*_t[0]-_s[1]*_t[1]-_s[2]*_t[2]+_s[3]*_t[3]; "
                    f"sr_vwrite(s,{_arr(di)},_d,4,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        if n == 3:
            body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},3,s->vfpuCtrl[0]); "
                    f"sr_vread(_t,s,{_arr(ti)},3,s->vfpuCtrl[1]); "
                    f"_d[0]=_s[1]*_t[2]-_s[2]*_t[1]; _d[1]=_s[2]*_t[0]-_s[0]*_t[2]; _d[2]=_s[0]*_t[1]-_s[1]*_t[0]; "
                    f"sr_vwrite(s,{_arr(di)},_d,3,s->vfpuCtrl[2]);{_EAT}")
            return "{ " + body + " }", None, 0
        raise Unsupported(f"vcrsp/vqmul size {n} at 0x{addr:08x}")
    # vrot: build a vector of sin/cos of an angle (PPSSPP Int_Vrot, common non-overlap case).
    if op == 0x3c and sub == 7 and ((w >> 21) & 0x1F) == 29:
        di = vreg_indices(vd, n)
        ang = vreg_indices(vs, 1)[0]
        imm = (w >> 16) & 0x1F
        neg = "-" if (imm & 0x10) else ""
        sine_lane, cos_lane = (imm >> 2) & 3, imm & 3
        lines = [f"float _a=s->v[{ang}]; float _si={neg}sr_vfpu_sin(_a),_co=sr_vfpu_cos(_a); float _d[4]={{0,0,0,0}};"]
        if sine_lane == cos_lane:
            for i in range(n):
                lines.append(f"_d[{i}]=_si;")
            lines.append(f"_d[{cos_lane}]=_co;")
        else:
            lines.append(f"_d[{sine_lane}]=_si; _d[{cos_lane}]=_co;")
        lines.append(f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + " ".join(lines) + " }", None, 0
    if (op == 0x18 and sub in (0, 1, 7)) or (op == 0x19 and sub == 0):
        ti = vreg_indices(vt, n)
        oper = {(0x18, 0): "+", (0x18, 1): "-", (0x18, 7): "/", (0x19, 0): "*"}[(op, sub)]
        body = (f"float _s[4],_t[4],_d[4]; "
                f"sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]{oper}_t[_i]; "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 1:  # vdot: single = sum_i s[i]*t[i] (4-wide accumulate)
        ti = vreg_indices(vt, n)
        dst = vreg_indices(vd, 1)[0]
        body = (f"float _s[4],_t[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"float _d=0.0f; for(int _i=0;_i<{n};_i++) _d+=_s[_i]*_t[_i]; "
                f"float _dd[1]={{_d}}; sr_vwrite(s,{_arr([dst])},_dd,1,s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 4:  # vhdp: homogeneous dot (last source lane forced to 1.0)
        ti = vreg_indices(vt, n)
        dst = vreg_indices(vd, 1)[0]
        terms = "+".join(f"_s[{i}]*_t[{i}]" for i in range(n - 1)) + f"+1.0f*_t[{n - 1}]"
        body = (f"float _s[4],_t[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); "
                f"float _d={terms}; _d=isnan(_d)?fabsf(_d):_d; "
                f"float _dd[1]={{_d}}; sr_vwrite(s,{_arr([dst])},_dd,1,s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x19 and sub == 5:  # vcrs: half cross product with forced swizzles
        ti = vreg_indices(vt, n)
        di = vreg_indices(vd, n)
        ss, ts = (1, 2, 0, 3), (2, 0, 1, 3)
        muls = " ".join(f"_d[{i}]=_s[{ss[i]}]*_t[{ts[i]}];" for i in range(n))
        body = (f"float _s[4],_t[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"sr_vread(_t,s,{_arr(ti)},{n},s->vfpuCtrl[1]); {muls} "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    if op == 0x3c and sub == 0:  # vmmul: d[a][b] = sum_c vs(b,c)*vt(a,c). No prefix (identity).
        side = vec_size(w)
        lines = []
        for a in range(side):
            for b in range(side):
                terms = "+".join(f"s->v[{mreg_index(vs, side, b, c)}]*s->v[{mreg_index(vt, side, a, c)}]"
                                 for c in range(side))
                # leading +0.0f matches PPSSPP's "sum = 0.0f; sum += ..." (-0 + -0 stays -0,
                # but 0 + -0 is +0, and the game can observe the sign bit)
                lines.append(f"float _m{a}_{b}=0.0f+{terms};")
        writes = " ".join(f"s->v[{mreg_index(vd, side, a, b)}]=_m{a}_{b};"
                          for a in range(side) for b in range(side))
        return "{ " + " ".join(lines) + " " + writes + _EAT + " }", None, 0
    if op == 0x3c and sub in (1, 2, 3):  # vtfm: d[i] = sum_k M(col=i,row=k)*t[k] (+ homog.)
        ins = sub
        side = ins + 1
        tn = min(n, ins + 1)
        ti = vreg_indices(vt, side)
        di = vreg_indices(vd, side)
        lines = []
        for i in range(side):
            terms = [f"s->v[{mreg_index(vs, side, i, k)}]*s->v[{ti[k]}]" for k in range(tn)]
            if ins >= n:  # homogeneous transform: implicit 1.0 in the last lane
                terms.append(f"s->v[{mreg_index(vs, side, i, ins)}]")
            lines.append(f"float _v{i}=0.0f+{'+'.join(terms)};")
        writes = " ".join(f"s->v[{di[i]}]=_v{i};" for i in range(side))
        return "{ " + " ".join(lines) + " " + writes + _EAT + " }", None, 0
    if op == 0x19 and sub == 2:  # vscl: vd[i] = vs[i] * V(vt) (scalar)
        scalar = vreg_indices(vt, 1)[0]
        body = (f"float _s[4],_d[4]; sr_vread(_s,s,{_arr(si)},{n},s->vfpuCtrl[0]); "
                f"float _sc=s->v[{scalar}]; "
                f"for(int _i=0;_i<{n};_i++) _d[_i]=_s[_i]*_sc; "
                f"sr_vwrite(s,{_arr(di)},_d,{n},s->vfpuCtrl[2]);{_EAT}")
        return "{ " + body + " }", None, 0
    raise Unsupported(f"VFPU opcode 0x{op:02x} sub 0x{sub:x} at 0x{addr:08x}")


def fpu_effect(addr, w):
    fmt = rs(w); ft = rt(w); fs = rd(w); fdv = sa(w)
    if fmt == 0x00: return wr(rt(w), f"s->fi[{fs}]"), None, 0          # mfc1
    if fmt == 0x02: return wr(rt(w), f"({fs} == 31 ? s->fcr31 : ({fs} == 0 ? 0x00003351u : 0u))"), None, 0  # cfc1
    if fmt == 0x04: return f"s->fi[{fs}] = {R(rt(w))};", None, 0       # mtc1
    if fmt == 0x06: return (f"if ({fs} == 31) s->fcr31 = {R(rt(w))};"), None, 0  # ctc1
    if fmt == 0x10:
        fn = funct(w)
        if fn == 0x00: return f"{F(fdv)} = {F(fs)} + {F(ft)};", None, 0
        if fn == 0x01: return f"{F(fdv)} = {F(fs)} - {F(ft)};", None, 0
        if fn == 0x02: return f"{{ float _a={F(fs)},_b={F(ft)}; if((isinf(_a)&&_b==0.0f)||(isinf(_b)&&_a==0.0f)) s->fi[{fdv}]=0x7fc00000u; else {F(fdv)}=_a*_b; }}", None, 0
        if fn == 0x03: return f"{F(fdv)} = {F(fs)} / {F(ft)};", None, 0
        if fn == 0x04: return f"{F(fdv)} = sqrtf({F(fs)});", None, 0
        if fn == 0x05: return f"{F(fdv)} = fabsf({F(fs)});", None, 0
        if fn == 0x06: return f"{F(fdv)} = {F(fs)};", None, 0
        if fn == 0x07: return f"{F(fdv)} = -{F(fs)};", None, 0
        if fn in (0x0C, 0x0D, 0x0E, 0x0F, 0x24):
            return f"s->fi[{fdv}] = sr_to_w({F(fs)}, 0x{fn:02x});", None, 0
        if fn >= 0x30:
            cond = fn & 0xF
            return (f"{{ float _a={F(fs)},_b={F(ft)}; int _u=isnan(_a)||isnan(_b); int _l=!_u&&_a<_b; int _e=!_u&&_a==_b; "
                    f"s->fpcond = ((_u&&({cond}&1))||(_e&&({cond}&2))||(_l&&({cond}&4)))?1u:0u; }}"), None, 0
    if fmt == 0x14 and funct(w) == 0x20: return f"{F(fdv)} = (float)(int32_t)s->fi[{fs}];", None, 0  # cvt.s.w
    raise Unsupported(f"COP1 fmt 0x{fmt:02x} funct 0x{funct(w):02x} at 0x{addr:08x}")


def is_cond_branch(w):
    op = w >> 26
    return op in (4, 5, 6, 7, 20, 21, 22, 23) or op == 1 or (op == 0x11 and rs(w) == 8)


def cond_expr(w):
    op = w >> 26
    if op == 4: return f"({R(rs(w))} == {R(rt(w))})"          # beq
    if op == 5: return f"({R(rs(w))} != {R(rt(w))})"          # bne
    if op == 6: return f"((int32_t){R(rs(w))} <= 0)"          # blez
    if op == 7: return f"((int32_t){R(rs(w))} > 0)"           # bgtz
    if op == 20: return f"({R(rs(w))} == {R(rt(w))})"         # beql
    if op == 21: return f"({R(rs(w))} != {R(rt(w))})"         # bnel
    if op == 22: return f"((int32_t){R(rs(w))} <= 0)"         # blezl
    if op == 23: return f"((int32_t){R(rs(w))} > 0)"          # bgtzl
    if op == 1:
        sub = rt(w)
        if sub in (0, 2, 0x10): return f"((int32_t){R(rs(w))} < 0)"   # bltz/bltzl/bltzal
        if sub in (1, 3, 0x11): return f"((int32_t){R(rs(w))} >= 0)"  # bgez/bgezl/bgezal
    if op == 0x11 and rs(w) == 8:
        tf = (w >> 16) & 1
        return f"(s->fpcond {'!=' if tf else '=='} 0)"        # bc1t/bc1f
    raise Unsupported(f"branch op 0x{op:02x}")


def is_likely(w):
    op = w >> 26
    return op in (20, 21, 22, 23) or (op == 1 and rt(w) in (2, 3)) or (op == 0x11 and rs(w) == 8 and ((w >> 17) & 1))


def is_link(w):  # branch that also writes $ra
    return (w >> 26) == 1 and rt(w) in (0x10, 0x11)


def branch_target(addr, w):
    return (addr + 4 + (s16(w) << 2)) & 0xFFFFFFFF


def jump_target(addr, w):
    return ((addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) & 0xFFFFFFFF


def read32(elf, addr):
    b = elf.read_at_vaddr(addr, 4)
    return struct.unpack("<I", b)[0] if b and len(b) >= 4 else None


def is_control(w):
    op = w >> 26
    fn = w & 0x3F
    return op in (2, 3) or (op == 0 and fn in (0x08, 0x09)) or is_cond_branch(w)


def function_flow(elf, start, ranges, known):
    # Recursive descent over one function: collect instruction addresses and intra-function
    # branch/jump labels. Calls return; jr/j/uncond-b end a straight run; j to a known
    # function is a tail call (not followed here).
    insns, labels, seen = set(), set(), set()
    stack = [start]
    while stack:
        pc = stack.pop()
        while in_ranges(pc, ranges) and pc not in seen:
            seen.add(pc)
            insns.add(pc)
            w = read32(elf, pc)
            if w is None:
                break
            op, fn = w >> 26, w & 0x3F
            if op == 3 or (op == 0 and fn == 0x09):  # jal / jalr: call, returns
                insns.add(pc + 4)
                seen.add(pc + 4)
                pc += 8
                continue
            if op == 2:  # j
                t = jump_target(pc, w)
                insns.add(pc + 4)
                seen.add(pc + 4)
                if t not in known and in_ranges(t, ranges):  # intra goto
                    labels.add(t)
                    stack.append(t)
                break
            if op == 0 and fn == 0x08:  # jr: return / computed / tail
                insns.add(pc + 4)
                seen.add(pc + 4)
                break
            if op == 0 and fn == 0x0C:  # syscall: HLE boundary
                break
            if is_cond_branch(w):
                t = branch_target(pc, w)
                labels.add(t)
                if in_ranges(t, ranges):
                    stack.append(t)
                insns.add(pc + 4)
                seen.add(pc + 4)
                pc += 8
                continue
            pc += 4
    return insns, labels


def normal_line(addr, w):
    eff, saddr, ssize = effect(addr, w)
    return f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {eff} sr_end(s, {saddr if saddr else '0u'}, {ssize});"


def emit_function(elf, start, ranges, known):
    insns, labels = function_flow(elf, start, ranges, known)
    out = []
    out.append(f"void f_{start:08x}(CpuState *s) {{")
    out.append("    SR_YIELD(s);")   # preemption point (no-op unless the scheduler is active)
    # Instructions are emitted in address order, but execution must begin at the entry. When the
    # entry is not the lowest address (the function contains code reached via a back-edge to a
    # lower address), jump to the entry's label first; otherwise control would start mid-body.
    if insns and start != min(insns):
        labels = set(labels)
        labels.add(start)
        out.append(f"    goto L_{start:08x};")
    consumed = set()
    for addr in sorted(insns):
        if addr in consumed:
            continue
        if addr in labels:
            out.append(f"  L_{addr:08x}: ;")
        w = read32(elf, addr)
        if w is None:
            continue
        op, fn = w >> 26, w & 0x3F

        if not is_control(w):
            out.append(normal_line(addr, w))
            continue

        # Control transfer: emit its branch/jump line, then the delay slot (consumed), then
        # the transfer, matching PPSSPP's branch-then-delay-slot trace order.
        ds = addr + 4
        dsw = read32(elf, ds)
        consumed.add(ds)
        ds_is_syscall = dsw is not None and (dsw >> 26) == 0 and (dsw & 0x3F) == 0x0C

        if op == 3:  # jal
            target = jump_target(addr, w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); s->r[31] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            if target in known:
                out.append(f"    f_{target:08x}(s);")
            else:
                out.append(f"    dispatch(s, 0x{target:08x}u);")
            continue
        if op == 0 and fn == 0x09:  # jalr rd, rs
            d, a = rd(w), rs(w)
            link = f"s->r[{d}] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; " if d != 0 else ""
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {link}sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            out.append(f"    {{ uint32_t _t = {R(a)}; dispatch(s, _t); }}")
            continue
        if op == 0 and fn == 0x08:  # jr rs
            a = rs(w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); sr_end(s, 0u, 0);")
            if ds_is_syscall:
                out.append(f"    sr_hle_call(s, 0x{(dsw >> 6) & 0xFFFFF:x}u); return;")
            else:
                out.append(normal_line(ds, dsw))
                if a == 31:
                    out.append("    return;")
                else:
                    out.append(f"    {{ uint32_t _t = {R(a)}; dispatch(s, _t); return; }}")
            continue
        if op == 2:  # j
            target = jump_target(addr, w)
            out.append(f"    sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); sr_end(s, 0u, 0);")
            out.append(normal_line(ds, dsw))
            if target in known:
                out.append(f"    f_{target:08x}(s); return;")
            else:
                y = "SR_YIELD(s); " if target <= addr else ""   # backward j: loop edge
                out.append(f"    {y}goto L_{target:08x};")
            continue
        # conditional branch. A backward target is a loop edge: yield when it is taken.
        target = branch_target(addr, w)
        y = "SR_YIELD(s); " if target <= addr else ""
        link = f"s->r[31] = 0x{(addr + 8) & 0xFFFFFFFF:08x}u; " if is_link(w) else ""
        out.append(f"    {{ uint32_t _c = {cond_expr(w)};")
        out.append(f"      sr_begin(s, 0x{addr:08x}u, 0x{w:08x}u); {link}sr_end(s, 0u, 0);")
        if is_likely(w):
            out.append(f"      if (_c) {{ {normal_line(ds, dsw).strip()} {y}goto L_{target:08x}; }} }}")
        else:
            out.append(f"   {normal_line(ds, dsw)}")
            out.append(f"      if (_c) {{ {y}goto L_{target:08x}; }} }}")
    out.append("}")
    out.append("")
    return out


SR_TO_W = """static uint32_t sr_to_w(float x, uint32_t fn) {
    if (isnan(x) || isinf(x)) return (isinf(x) && x < 0.0f) ? 0x80000000u : 0x7FFFFFFFu;
    int32_t r;
    switch (fn) {
        case 0x0C: r = (int32_t)floorf(x + 0.5f); break;
        case 0x0D: if (x >= 0.0f) { r = (int32_t)floorf(x); if (r == (int32_t)0x80000000) r = 0x7FFFFFFF; } else r = (int32_t)ceilf(x); break;
        case 0x0E: r = (int32_t)ceilf(x); break;
        case 0x0F: r = (int32_t)floorf(x); break;
        default:   r = (int32_t)nearbyintf(x); break;
    }
    return (uint32_t)r;
}"""


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    if len(args) < 2:
        sys.stderr.write("usage: codegen.py <elf> <out.c> [--base=HEX]\n")
        return 2
    base = None
    for o in opts:
        if o.startswith("--base="):
            base = int(o.split("=", 1)[1], 16)
    elf = Elf(args[0], base=base)
    ranges = exec_ranges(elf)
    known, _ = analyze(elf)
    known = set(a for a in known if in_ranges(a, ranges))

    # Import stubs live in .sceStub.text. In the file each is "jr $ra; <placeholder>"; the
    # syscall is written into the delay slot by the loader at run time, so we cannot read it
    # from the file. The codegen emits the jr line (which is recorded in the trace) and then an HLE
    # boundary call, stopping the traced run exactly where the reference trace reaches its syscall.
    stub = elf.sec(".sceStub.text")
    def is_stub(a):
        return stub is not None and stub["addr"] <= a < stub["addr"] + stub["size"]

    # Resolve each import stub to its NID from the PRX import table (only meaningful for a
    # relocated PRX). The stub then dispatches to the HLE handler for that NID.
    impmap = {}
    if elf.reloc is not None:
        try:
            from imports import parse_imports
            impmap = parse_imports(elf)
        except Exception as e:
            sys.stderr.write(f"warning: import table parse failed: {e}\n")

    out = [
        "/* Generated by tools/codegen.py. Do not edit by hand. */",
        '#include "recomp.h"',
        "#include <math.h>",
        "#include <string.h>",
        "",
        SR_TO_W,
        "",
    ]
    for a in sorted(known):
        out.append(f"void f_{a:08x}(CpuState *s);")
    out.append("")
    emitted = []
    stubbed = []
    for a in sorted(known):
        if is_stub(a):
            w0 = read32(elf, a) or 0x03E00008          # jr $ra
            w1 = read32(elf, a + 4) or 0               # delay slot (PPSSPP patches a syscall)
            lib_nid = impmap.get(a)
            if lib_nid is not None:
                lib, nid = lib_nid
                out.append(f"void f_{a:08x}(CpuState *s) {{  /* import: {lib} nid 0x{nid:08x} */")
                out.append(f"    sr_begin(s, 0x{a:08x}u, 0x{w0:08x}u); sr_end(s, 0u, 0);")
                out.append(f"    sr_begin(s, 0x{a + 4:08x}u, 0x{w1:08x}u);")
                out.append(f"    sr_syscall(s, 0x{nid:08x}u);")
                out.append(f"    sr_end(s, 0u, 0);")
                out.append("}")
            else:
                # No import-table entry (or non-PRX): keep the old HLE-boundary behavior.
                out.append(f"void f_{a:08x}(CpuState *s) {{  /* import stub -> HLE boundary */")
                out.append(f"    sr_begin(s, 0x{a:08x}u, 0x{w0:08x}u); sr_end(s, 0u, 0);")
                out.append(f"    sr_hle_call(s, 0u);")
                out.append("}")
            out.append("")
            emitted.append(a)
            continue
        try:
            out.extend(emit_function(elf, a, ranges, known))
            emitted.append(a)
        except Unsupported as e:
            # Emit a loud-trapping stub so the dispatch table stays complete and reaching this
            # function at run time aborts with its address and reason (recorded in STUBS.md),
            # rather than silently dropping it.
            reason = str(e).replace('"', "'")
            out.append(f"void f_{a:08x}(CpuState *s) {{  /* untranslatable: {reason} */")
            out.append(f'    sr_unimplemented(0x{a:08x}u, "{reason}");')
            out.append("}")
            out.append("")
            emitted.append(a)
            stubbed.append((a, reason))
            sys.stderr.write(f"skip 0x{a:08x}: {e}\n")
    out.append("void sr_register_all(void) {")
    for a in emitted:
        out.append(f"    sr_register(0x{a:08x}u, f_{a:08x});")
    out.append("}")
    out.append("")
    with open(args[1], "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out))
    print(f"wrote {args[1]}: {len(emitted)}/{len(known)} functions ({len(stubbed)} trapping stubs)")
    if stubbed:
        import os
        sp = os.path.join(os.path.dirname(args[1]) or ".", "stubs.txt")
        with open(sp, "w", encoding="ascii", newline="\n") as f:
            for a, r in stubbed:
                f.write(f"0x{a:08x} {r}\n")
        print(f"wrote stub list: {sp}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
