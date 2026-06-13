#!/usr/bin/env python3
# Function-boundary analyzer for PSP ELF/PRX modules (Phase 2).
#
# Discovers function entry points without using the symbol table, by combining:
#   - the ELF entry point and the module's start/stop exports,
#   - direct-call targets (jal) found by sweeping the executable sections,
#   - constructor/destructor pointer arrays (.ctors/.dtors),
#   - function-pointer tables in read-only/data sections (pointers into code),
#   - recursive descent from each entry to bound the function and find more calls.
#
# When the module has a symbol table (the homebrew does), it reports recall against the
# STT_FUNC symbols. It also emits a TOML function inventory.
#
# Usage: analyze.py <elf> [--toml out.toml] [--quiet]

import struct
import sys


class Elf:
    def __init__(self, path, base=None):
        self.data = open(path, "rb").read()
        d = self.data
        self.reloc = None
        assert d[:4] == b"\x7fELF", "not an ELF"
        self.entry, self.phoff, self.shoff = struct.unpack("<III", d[24:36])
        self.phentsize, self.phnum = struct.unpack("<HH", d[42:46])
        self.shentsize, self.shnum, self.shstrndx = struct.unpack("<HHH", d[46:52])
        self.sections = []
        for i in range(self.shnum):
            o = self.shoff + i * self.shentsize
            name, typ, flags, addr, off, size, link, info, align, entsz = struct.unpack("<10I", d[o:o + 40])
            self.sections.append(dict(name=name, typ=typ, flags=flags, addr=addr,
                                      off=off, size=size, link=link, info=info, entsz=entsz))
        shstr = self.sections[self.shstrndx]
        for s in self.sections:
            e = shstr["off"] + s["name"]
            s["nm"] = d[e:d.find(b"\x00", e)].decode("ascii", "replace")
        self.segments = []
        for i in range(self.phnum):
            o = self.phoff + i * self.phentsize
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack("<8I", d[o:o + 32])
            self.segments.append(dict(type=p_type, off=p_off, vaddr=p_vaddr,
                                      filesz=p_filesz, memsz=p_memsz, flags=p_flags))

        # PRX (ET_SCE_PRX = 0xFFA0): rebase to `base` and apply type-A relocations, so the
        # code has concrete addresses. After this, read_at_vaddr serves the relocated image
        # and section/segment addresses are in the rebased space.
        e_type = struct.unpack("<H", d[16:18])[0]
        if base is not None and e_type == 0xFFA0:
            from prxload import Prx
            prx = Prx(path, base)
            prx.relocate()
            self.reloc = prx
            self.entry += base
            for s in self.sections:
                s["addr"] += base
            for s in self.segments:
                s["vaddr"] += base

    def sec(self, name):
        return next((s for s in self.sections if s["nm"] == name), None)

    def read_at_vaddr(self, vaddr, n):
        # For a relocated PRX, serve the rebased+relocated image directly.
        if self.reloc is not None:
            if self.reloc.lo <= vaddr < self.reloc.lo + len(self.reloc.mem):
                o = vaddr - self.reloc.lo
                return bytes(self.reloc.mem[o:o + n])
            return None
        # Otherwise read from the loaded image via PT_LOAD program headers. Sections are not
        # used here because non-code allocated sections (.reginfo, .MIPS.abiflags) can share a
        # vaddr with .text and would otherwise shadow the real instruction bytes.
        for seg in self.segments:
            if seg["type"] == 1 and seg["vaddr"] <= vaddr < seg["vaddr"] + seg["filesz"]:
                off = seg["off"] + (vaddr - seg["vaddr"])
                return self.data[off:off + n]
        return None

    def func_symbols(self):
        symtab, strtab = self.sec(".symtab"), self.sec(".strtab")
        if not symtab or not strtab:
            return None
        d = self.data
        out = set()
        for i in range(symtab["size"] // symtab["entsz"]):
            o = symtab["off"] + i * symtab["entsz"]
            st_name, st_value, st_size, st_info, st_other, st_shndx = struct.unpack("<IIIBBH", d[o:o + 16])
            if (st_info & 0xF) == 2 and st_value != 0:  # STT_FUNC
                out.add(st_value)
        return out


EXEC_SECTIONS = (".text", ".init", ".fini", ".sceStub.text")


def section_bytes(elf, s):
    # For a relocated PRX, read the section from the rebased+relocated image so that any
    # R_MIPS_32 function pointers it holds are already concrete. Otherwise read the raw file.
    if elf.reloc is not None:
        b = elf.read_at_vaddr(s["addr"], s["size"])
        return b if b is not None else b""
    return elf.data[s["off"]:s["off"] + s["size"]]


def exec_ranges(elf):
    ranges = []
    for name in EXEC_SECTIONS:
        s = elf.sec(name)
        if s and s["size"]:
            ranges.append((s["addr"], s["addr"] + s["size"]))
    return ranges


def in_ranges(addr, ranges):
    return any(lo <= addr < hi for lo, hi in ranges)


def trace_function(elf, start, ranges, covered, calls):
    # Recursive descent over one function's intra-procedural control flow from `start`.
    # Adds every instruction address reached to `covered`, and every direct-call (jal) target
    # to `calls`. Calls return, so execution continues after the delay slot; jr and j and an
    # unconditional b end a path (no fall-through). Conditional branches fork: follow the
    # target and continue past the delay slot.
    stack = [start]
    local = set()
    while stack:
        pc = stack.pop()
        while in_ranges(pc, ranges) and pc not in local:
            local.add(pc)
            covered.add(pc)
            wb = elf.read_at_vaddr(pc, 4)
            if wb is None or len(wb) < 4:
                break
            word = struct.unpack("<I", wb)[0]
            op = word >> 26
            funct = word & 0x3F
            if op == 3:  # jal: direct call, returns -> continue past delay slot
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    calls.add(target)
                covered.add(pc + 4)
                pc += 8
                continue
            if op == 2:  # j: almost always a tail call -> target is a function entry, end path
                target = (pc & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                covered.add(pc + 4)
                if in_ranges(target, ranges):
                    calls.add(target)
                break
            if op == 0 and funct == 0x08:  # jr (return / computed): end this path
                covered.add(pc + 4)
                break
            # Branches: REGIMM (1), beq/bne/blez/bgtz (4-7) and their likely forms (20-23),
            # and FPU bc1 (cop1 with rs=8). Target is intra-function; fork and continue.
            is_branch = op in (1, 4, 5, 6, 7, 20, 21, 22, 23) or (op == 0x11 and ((word >> 21) & 0x1F) == 8)
            if is_branch:
                off = word & 0xFFFF
                off = off - 0x10000 if off & 0x8000 else off
                target = pc + 4 + (off << 2)
                if in_ranges(target, ranges):
                    stack.append(target)
                covered.add(pc + 4)
                # Unconditional b (beq $zero,$zero): no fall-through after the delay slot.
                if op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0:
                    break
                pc += 8
                continue
            pc += 4


def analyze(elf):
    ranges = exec_ranges(elf)
    text = elf.sec(".text")

    def in_text(a):
        return text and text["addr"] <= a < text["addr"] + text["size"] and (a & 3) == 0

    # High-confidence function starts: addresses that are genuinely entered as a function,
    # not internal blocks. These seed the extent tracing below.
    hc = set()
    if in_ranges(elf.entry, ranges):
        hc.add(elf.entry)

    # Module export pointers and constructor/destructor arrays point at real functions.
    for nm in (".rodata.sceModuleInfo", ".lib.ent", ".rodata.sceResident",
               ".ctors", ".dtors", ".init_array", ".fini_array"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = struct.unpack("<I", blob[o:o + 4])[0]
            if in_ranges(val, ranges):
                hc.add(val)

    # Function-pointer tables in read-only/data sections (callbacks reached via jalr).
    for nm in (".rodata", ".data", ".sdata"):
        s = elf.sec(nm)
        if not s or s["typ"] == 8:
            continue
        blob = section_bytes(elf, s)
        for o in range(0, len(blob) - 3, 4):
            val = struct.unpack("<I", blob[o:o + 4])[0]
            if in_text(val):
                hc.add(val)

    # Sweep executable sections: jal targets are calls (high-confidence functions); la-style
    # address materialization into code is an indirect-call target. j targets, prologues, and
    # the instruction after an unconditional terminator are weaker "block boundary" signals
    # kept separately and used only to fill gaps the call graph does not cover.
    noisy = set()
    for lo, hi in ranges:
        blob = elf.read_at_vaddr(lo, hi - lo)
        if blob is None:
            continue
        hireg = {}
        for off in range(0, len(blob) - 3, 4):
            word = struct.unpack("<I", blob[off:off + 4])[0]
            addr = lo + off
            op = word >> 26
            if op == 3:  # jal: direct call
                target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    hc.add(target)
            elif op == 2:  # j: tail call or intra-function goto -> weak
                target = (addr & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if in_ranges(target, ranges):
                    noisy.add(target)
            if (word >> 16) == 0x27BD and (word & 0x8000):  # addiu $sp,$sp,-N prologue
                noisy.add(addr)
            is_uncond = (op == 2 or (op == 0 and (word & 0x3F) == 0x08)
                         or (op == 4 and ((word >> 21) & 0x1F) == 0 and ((word >> 16) & 0x1F) == 0))
            if is_uncond and in_ranges(addr + 8, ranges):
                noisy.add(addr + 8)
            if op == 0x0F:  # lui rt, imm
                hireg[(word >> 16) & 0x1F] = (word & 0xFFFF) << 16
            elif op == 0x09:  # addiu rt, rs, simm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg:
                    val = (hireg[rs] + ((word & 0xFFFF) - (0x10000 if word & 0x8000 else 0))) & 0xFFFFFFFF
                    if in_text(val):
                        hc.add(val)
            elif op == 0x0D:  # ori rt, rs, imm  (la low half)
                rs = (word >> 21) & 0x1F
                if rs in hireg:
                    val = hireg[rs] | (word & 0xFFFF)
                    if in_text(val):
                        hc.add(val)

    # Trace each function's extent from the high-confidence seeds, following discovered calls.
    # `covered` ends up holding every instruction that belongs to some known function, so the
    # weak signals that land inside a function body (internal blocks) can be discarded.
    covered = set()
    calls = set()
    functions = set(hc)
    work = list(hc)
    while work:
        s = work.pop()
        if in_ranges(s, ranges):
            trace_function(elf, s, ranges, covered, calls)
        for t in list(calls):
            if t not in functions and in_ranges(t, ranges):
                functions.add(t)
                work.append(t)

    # Gap fill: a weak-signal address that no known function covers is an indirect-only
    # function (reached through a register the call graph could not resolve). Add it and trace
    # it, which may reveal further calls. Iterate until stable.
    changed = True
    while changed:
        changed = False
        for c in sorted(noisy):
            if in_ranges(c, ranges) and c not in covered and c not in functions:
                functions.add(c)
                trace_function(elf, c, ranges, covered, calls)
                for t in list(calls):
                    if t not in functions and in_ranges(t, ranges):
                        functions.add(t)
                        trace_function(elf, t, ranges, covered, calls)
                changed = True

    return functions, ranges


# ---- TOML model: a function inventory the codegen reads and a human can correct ----------

def build_model(elf, starts):
    text = elf.sec(".text")
    stub = elf.sec(".sceStub.text")

    def kind(a):
        if stub and stub["addr"] <= a < stub["addr"] + stub["size"]:
            return "import_stub"
        return "function"

    return {
        "module": (elf.sec(".rodata.sceModuleInfo") is not None) and "prx" or "elf",
        "entry": elf.entry,
        "functions": [{"addr": a, "kind": kind(a)} for a in sorted(starts)],
    }


# Per-function fields are emitted in this order; unknown extra fields are appended so a
# hand-corrected TOML round-trips without loss.
_FUNC_ORDER = ["addr", "kind", "name", "skip", "note"]


def _fmt_value(key, val):
    if key == "addr" or (key == "entry"):
        return f"0x{val:08x}"
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, int):
        return str(val)
    return '"' + str(val).replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit_toml(model, path):
    lines = [
        "# Function inventory emitted by tools/analyze.py. Machine-generated, human-editable.",
        "# Edit names/skip/note or add functions; the loader round-trips edits without loss.",
        f'module = {_fmt_value("module", model["module"])}',
        f'entry = {_fmt_value("entry", model["entry"])}',
        "",
    ]
    for fn in model["functions"]:
        lines.append("[[function]]")
        keys = [k for k in _FUNC_ORDER if k in fn] + [k for k in fn if k not in _FUNC_ORDER]
        for k in keys:
            lines.append(f"{k} = {_fmt_value(k, fn[k])}")
        lines.append("")
    import os
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))


def load_toml(path):
    import tomllib
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    return {
        "module": doc.get("module", "elf"),
        "entry": doc.get("entry", 0),
        "functions": doc.get("function", []),
    }


def roundtrip_check(model, workdir):
    # Emit the model, simulate a hand correction (rename one function, skip another, add a
    # note), then load -> re-emit -> load and require the corrections and the full function
    # set to survive byte-for-byte.
    import os
    os.makedirs(workdir, exist_ok=True)
    t1 = os.path.join(workdir, "functions.toml")
    emit_toml(model, t1)

    corrected = load_toml(t1)
    if len(corrected["functions"]) >= 2:
        corrected["functions"][0]["name"] = "module_entry"
        corrected["functions"][1]["skip"] = True
        corrected["functions"][1]["note"] = "hand-corrected: handled by HLE"
    tc = os.path.join(workdir, "functions.corrected.toml")
    emit_toml(corrected, tc)

    # Idempotence: loading and re-emitting the corrected TOML must be byte-identical.
    reloaded = load_toml(tc)
    tc2 = os.path.join(workdir, "functions.corrected.2.toml")
    emit_toml(reloaded, tc2)
    a = open(tc, "rb").read()
    b = open(tc2, "rb").read()
    ok = a == b
    # And the corrections must still be present after the round-trip.
    fn0, fn1 = reloaded["functions"][0], reloaded["functions"][1]
    preserved = fn0.get("name") == "module_entry" and fn1.get("skip") is True and "note" in fn1
    same_count = len(reloaded["functions"]) == len(model["functions"])
    return ok and preserved and same_count, t1


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = [a for a in argv[1:] if a.startswith("--")]
    if not args:
        sys.stderr.write("usage: analyze.py <elf> [--base=HEX] [--toml=out.toml] [--check=workdir] [--quiet]\n")
        return 2
    base = None
    for o in opts:
        if o.startswith("--base="):
            base = int(o.split("=", 1)[1], 16)
    elf = Elf(args[0], base=base)
    starts, ranges = analyze(elf)
    model = build_model(elf, starts)

    quiet = "--quiet" in opts
    rc = 0
    truth = elf.func_symbols()
    if truth is not None:
        truth_in = set(a for a in truth if in_ranges(a, ranges))
        found = truth_in & starts
        recall = len(found) / len(truth_in) if truth_in else 1.0
        missed = sorted(truth_in - starts)
        if not quiet:
            print(f"ground-truth functions (in exec ranges): {len(truth_in)}")
            print(f"discovered entry points: {len(starts)}")
            print(f"recovered: {len(found)}  recall: {recall*100:.2f}%")
            if missed:
                print(f"missed {len(missed)} (first 20): " + ", ".join("0x%08x" % a for a in missed[:20]))
        print(f"RECALL {recall*100:.2f}% ({len(found)}/{len(truth_in)})")
        if recall < 0.95:
            rc = 1

    for o in opts:
        if o.startswith("--toml="):
            emit_toml(model, o.split("=", 1)[1])
            print("wrote TOML:", o.split("=", 1)[1])
        if o.startswith("--check="):
            ok, t1 = roundtrip_check(model, o.split("=", 1)[1])
            print(f"TOML round-trip (hand-corrected, lossless): {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
