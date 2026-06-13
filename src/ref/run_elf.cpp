// Drive the reference interpreter over a statically-linked PSP ELF and emit a trace, seeded
// with the exact entry register state captured by the PPSSPP oracle (the "# init" line in
// the oracle trace). The emitted trace is then diffed against the oracle with
// tools/tracediff.py to locate the first divergence in the reference interpreter.
//
// Usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps]
//
// This is a bring-up driver, not the loader of record (that is Phase 2). It handles only
// PT_LOAD of a non-relocated ELF, which is what the golden homebrew is.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "interp.h"

using namespace ref;

namespace {

std::vector<uint8_t> ReadFile(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data(n);
	if (n > 0 && fread(data.data(), 1, n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
	fclose(f);
	return data;
}

uint32_t Rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t Rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

// Load PT_LOAD segments into guest memory. Returns the ELF entry point.
uint32_t LoadElf(const std::vector<uint8_t> &elf, Memory *mem) {
	if (elf.size() < 52 || memcmp(elf.data(), "\x7f""ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); exit(2); }
	uint32_t e_entry = Rd32(&elf[24]);
	uint32_t e_phoff = Rd32(&elf[28]);
	uint16_t e_phentsize = Rd16(&elf[42]);
	uint16_t e_phnum = Rd16(&elf[44]);
	for (uint16_t i = 0; i < e_phnum; i++) {
		const uint8_t *ph = &elf[e_phoff + (uint32_t)i * e_phentsize];
		uint32_t p_type = Rd32(ph + 0);
		uint32_t p_offset = Rd32(ph + 4);
		uint32_t p_vaddr = Rd32(ph + 8);
		uint32_t p_filesz = Rd32(ph + 16);
		uint32_t p_memsz = Rd32(ph + 20);
		if (p_type != 1)  // PT_LOAD
			continue;
		for (uint32_t b = 0; b < p_filesz; b++)
			mem->Write8(p_vaddr + b, elf[p_offset + b]);
		for (uint32_t b = p_filesz; b < p_memsz; b++)  // .bss
			mem->Write8(p_vaddr + b, 0);
	}
	return e_entry;
}

// Parse the "# init r0=.. ... fcr31=.. f0=.. .." line from the oracle trace into a CpuState.
bool SeedFromInit(const char *trace_path, CpuState *s) {
	FILE *f = fopen(trace_path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
	char buf[8192];
	bool found = false;
	while (fgets(buf, sizeof(buf), f)) {
		if (strncmp(buf, "# init", 6) != 0)
			continue;
		found = true;
		char *tok = strtok(buf + 6, " \t\n");
		while (tok) {
			char *eq = strchr(tok, '=');
			if (eq) {
				*eq = 0;
				uint32_t val = (uint32_t)strtoul(eq + 1, nullptr, 16);
				const char *name = tok;
				if (name[0] == 'r') s->r[atoi(name + 1)] = val;
				else if (name[0] == 'f' && name[1] >= '0' && name[1] <= '9') s->fi[atoi(name + 1)] = val;
				else if (strcmp(name, "hi") == 0) s->hi = val;
				else if (strcmp(name, "lo") == 0) s->lo = val;
				else if (strcmp(name, "fcr31") == 0) s->fcr31 = val;
			}
			tok = strtok(nullptr, " \t\n");
		}
		break;
	}
	fclose(f);
	return found;
}

const char *ReasonName(StopReason r) {
	switch (r) {
		case StopReason::kRunning: return "running";
		case StopReason::kSyscall: return "syscall";
		case StopReason::kUnimplemented: return "unimplemented";
		case StopReason::kMemoryFault: return "memory-fault";
		case StopReason::kBreak: return "break";
		case StopReason::kStepLimit: return "step-limit";
	}
	return "?";
}

}  // namespace

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: run_elf <elf> <oracle-trace> <out-trace> [max_steps]\n");
		return 2;
	}
	const char *elf_path = argv[1];
	const char *oracle_trace = argv[2];
	const char *out_path = argv[3];
	unsigned long long max_steps = argc > 4 ? strtoull(argv[4], nullptr, 10) : 2000000ULL;

	Memory mem;
	std::vector<uint8_t> elf = ReadFile(elf_path);
	uint32_t entry = LoadElf(elf, &mem);

	CpuState s;
	if (!SeedFromInit(oracle_trace, &s)) {
		fprintf(stderr, "no '# init' line in %s; rebuild oracle trace with init dump\n", oracle_trace);
		return 2;
	}
	s.pc = entry;

	FILE *out = fopen(out_path, "wb");
	if (!out) { fprintf(stderr, "cannot open %s for write\n", out_path); return 2; }
	TraceSink sink(out);
	sink.Header("hello", entry);

	StepResult res = Run(&s, &mem, max_steps, &sink);
	fclose(out);

	fprintf(stderr, "stopped: reason=%s pc=0x%08x op=0x%08x\n", ReasonName(res.reason), res.pc, res.op);
	return 0;
}
