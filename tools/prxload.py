#!/usr/bin/env python3
# PSP PRX loader: rebase a relocatable PRX to a load base and apply its type-A relocations,
# producing a flat memory image with concrete addresses (the form the analyzer/codegen need).
# Ports PPSSPP's ElfReader::LoadRelocations (type-A, section type 0x700000A0).
#
# Usage: prxload.py <prx-elf> <base-hex> [--out image.bin] [--verify pc=word ...]

import struct
import sys

R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_26 = 0, 1, 2, 4
R_MIPS_HI16, R_MIPS_LO16, R_MIPS_GPREL16 = 5, 6, 7
SHT_PRX_RELOC = 0x700000A0


class Prx:
    def __init__(self, path, base):
        self.data = open(path, "rb").read()
        d = self.data
        assert d[:4] == b"\x7fELF", "not an ELF"
        self.entry, self.phoff, self.shoff = struct.unpack("<III", d[24:36])
        self.phentsize, self.phnum = struct.unpack("<HH", d[42:46])
        self.shentsize, self.shnum, self.shstrndx = struct.unpack("<HHH", d[46:52])
        self.base = base

        self.segments = []
        for i in range(self.phnum):
            o = self.phoff + i * self.phentsize
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<8I", d[o:o + 32])
            self.segments.append(dict(type=p_type, off=p_off, vaddr=p_vaddr,
                                      filesz=p_filesz, memsz=p_memsz))
        loads = [s for s in self.segments if s["type"] == 1]
        # segVAddr[i] = where program-header segment i is loaded. PSP relocs index program
        # segments; only PT_LOAD segments carry an image, indexed in header order.
        self.seg_vaddr = [base + s["vaddr"] for s in loads]

        # Flat image covering all loaded segments.
        end = max(base + s["vaddr"] + s["memsz"] for s in loads)
        self.lo = base
        self.mem = bytearray(end - base)
        for s in loads:
            dst = (base + s["vaddr"]) - self.lo
            self.mem[dst:dst + s["filesz"]] = d[s["off"]:s["off"] + s["filesz"]]

        self.sections = []
        for i in range(self.shnum):
            o = self.shoff + i * self.shentsize
            name, typ, flags, addr, off, size, link, info, align, entsz = struct.unpack("<10I", d[o:o + 40])
            self.sections.append(dict(typ=typ, off=off, size=size))

    def r32(self, addr):
        o = addr - self.lo
        return struct.unpack("<I", self.mem[o:o + 4])[0]

    def w32(self, addr, val):
        o = addr - self.lo
        self.mem[o:o + 4] = struct.pack("<I", val & 0xFFFFFFFF)

    def relocate(self):
        d = self.data
        total = 0
        for sec in self.sections:
            if sec["typ"] != SHT_PRX_RELOC:
                continue
            n = sec["size"] // 8
            rels = [struct.unpack("<II", d[sec["off"] + r * 8:sec["off"] + r * 8 + 8]) for r in range(n)]
            self._apply(rels)
            total += n
        return total

    def _apply(self, rels):
        nseg = len(self.seg_vaddr)
        n = len(rels)
        for r in range(n):
            offset, info = rels[r]
            rtype = info & 0xF
            ofs_seg = (info >> 8) & 0xFF
            addr_seg = (info >> 16) & 0xFF
            if ofs_seg >= nseg:
                continue
            addr = offset + self.seg_vaddr[ofs_seg]
            relocate_to = self.seg_vaddr[addr_seg] if addr_seg < nseg else 0
            op = self.r32(addr)

            if rtype == R_MIPS_32:
                op = (op + relocate_to) & 0xFFFFFFFF
            elif rtype == R_MIPS_26:
                op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (relocate_to >> 2)) & 0x03FFFFFF)
            elif rtype == R_MIPS_HI16:
                cur = (op & 0xFFFF) << 16
                lo = 0
                for t in range(r + 1, n):
                    t_type = rels[t][1] & 0xF
                    if t_type == R_MIPS_HI16:
                        continue
                    lo = rels[t][0]  # placeholder; replaced below
                    lo_op = self.r32(rels[t][0] + self.seg_vaddr[(rels[t][1] >> 8) & 0xFF])
                    s16 = (lo_op & 0xFFFF) - 0x10000 if (lo_op & 0x8000) else (lo_op & 0xFFFF)
                    cur = (cur + s16 + relocate_to) & 0xFFFFFFFF
                    hi = ((cur >> 16) + (1 if (cur & 0x8000) else 0)) & 0xFFFF
                    op = (op & 0xFFFF0000) | hi
                    break
            elif rtype == R_MIPS_LO16:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + relocate_to) & 0xFFFF)
            elif rtype == R_MIPS_16:
                op = (op & 0xFFFF0000) | ((((op & 0xFFFF)) + relocate_to) & 0xFFFF)
            # R_MIPS_GPREL16 / R_MIPS_NONE: nothing.
            self.w32(addr, op)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: prxload.py <prx-elf> <base-hex> [--out image.bin] [pc=word ...]\n")
        return 2
    base = int(argv[2], 16)
    prx = Prx(argv[1], base)
    n = prx.relocate()
    print(f"loaded base=0x{base:08x} entry=0x{base + prx.entry:08x} relocations applied={n}")
    rc = 0
    for a in argv[3:]:
        if a.startswith("--out="):
            open(a.split("=", 1)[1], "wb").write(prx.mem)
            print("wrote image:", a.split("=", 1)[1])
        elif "=" in a:
            pc_s, word_s = a.split("=")
            pc, want = int(pc_s, 16), int(word_s, 16)
            got = prx.r32(pc)
            ok = got == want
            print(f"verify 0x{pc:08x}: got 0x{got:08x} want 0x{want:08x} {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
