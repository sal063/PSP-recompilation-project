// Reference Allegrex interpreter: integer and single-precision FPU semantics.
// See cpu.h. Emits the trace format in tools/TRACE_FORMAT.md when a TraceSink is provided.

#pragma once

#include <cstdio>

#include "cpu.h"

namespace ref {

// Reason an instruction could not be executed. The interpreter never silently ignores an
// unimplemented or faulting instruction (plan section 1.3): it stops and reports.
enum class StopReason {
	kRunning,
	kSyscall,          // hit a syscall; the HLE layer (later phases) must handle it
	kUnimplemented,    // decoded but no semantics implemented yet -> trap, do not fake
	kMemoryFault,      // load/store outside modeled memory
	kBreak,            // break instruction
	kStepLimit,        // reached the caller's instruction budget
};

struct StepResult {
	StopReason reason = StopReason::kRunning;
	uint32_t pc = 0;       // pc of the instruction involved when stopping
	uint32_t op = 0;       // the instruction word
};

// Writes one trace line per executed instruction in the TRACE_FORMAT.md shape. The sink owns
// snapshotting: BeginStep records the pre-state, EndStep diffs and emits.
class TraceSink {
public:
	explicit TraceSink(std::FILE *out) : out_(out) {}
	void Header(const char *target, uint32_t start_pc);
	void BeginStep(const CpuState *s, uint32_t pc, uint32_t op);
	void EndStep(const CpuState *s, uint32_t mem_addr, int mem_size, const Memory *mem);

private:
	std::FILE *out_;
	uint32_t r_[32], fi_[32], hi_, lo_, fcr31_, pc_, op_;
	unsigned long long step_ = 0;
};

// Execute up to max_steps instructions starting from s->pc. Stops on the first non-running
// condition (syscall, unimplemented op, fault, break) or when the budget is exhausted.
// When sink is non-null, every executed instruction is traced.
StepResult Run(CpuState *s, Memory *mem, unsigned long long max_steps, TraceSink *sink);

}  // namespace ref
