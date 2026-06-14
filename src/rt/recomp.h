/* Runtime the recompiled C links against (ARCHITECTURE.md sections 4, 5, 7).
 *
 * The codegen emits one C function per guest function with signature void f_<hexaddr>(CpuState*).
 * Those functions read and write this CpuState and access guest memory through the macros
 * below. Computed transfers go through dispatch(). When tracing is enabled (a trace file is
 * set), each translated instruction reports itself so the output can be diffed against the
 * PPSSPP reference trace (tools/TRACE_FORMAT.md).
 */

#ifndef PSP_RECOMP_RT_H
#define PSP_RECOMP_RT_H

#include <stdint.h>

typedef struct CpuState {
    uint32_t r[32];     /* r[0] reads 0; the codegen never emits a write to r[0]. */
    uint32_t hi, lo;
    uint32_t pc;        /* kept accurate at call sites / interpreter boundaries (section 7) */
    union {
        float f[32];
        uint32_t fi[32];
    };
    uint32_t fcr31;
    uint32_t fpcond;    /* FP compare result, separate from fcr31 to match PPSSPP */
    union {
        float v[128];   /* VFPU register file; physical order matches PPSSPP's v[128] */
        uint32_t vi[128];
    };
    uint32_t vfpuCtrl[16];  /* VFPU control: prefixes (S/T/D), cc, etc. */
} CpuState;

/* Guest memory: a single host region. g_mem points at guest 0x08000000, and the underlying
 * allocation also extends 0x04000000 bytes *below* g_mem so the same arena covers VRAM/eDRAM
 * (0x04000000..0x041fffff). SR_HOST maps a VRAM address to g_mem minus an offset that still
 * lands inside the allocation. Keeping SR_RAM_BASE at 0x08000000 means the recompiled code's
 * MEM_* macros are unchanged (no rebuild of the big object). */
extern uint8_t *g_mem;
#define SR_RAM_BASE 0x08000000u
#define SR_PHYS(a)  ((a) & 0x1FFFFFFFu)
/* Signed offset so addresses below SR_RAM_BASE (VRAM/eDRAM at 0x04000000) map below g_mem,
 * into the part of the arena reserved for them, instead of wrapping to a huge positive offset. */
#define SR_HOST(a)  (g_mem + (int32_t)(SR_PHYS(a) - SR_RAM_BASE))

/* The arena covers guest physical [0x04000000, 0x0c000000): VRAM/eDRAM at 0x04000000 and user
 * RAM at 0x08000000. A guest pointer outside that window is wild (would fault on real hardware
 * too). During bring-up some not-yet-faithful subsystems compute such pointers; rather than
 * segfault the host, out-of-range reads return 0 and writes are dropped. Valid accesses are
 * unaffected, so differential/bit-exact checks still hold. */
static inline int sr_inrange(uint32_t a) {
    return (uint32_t)(SR_PHYS(a) - 0x04000000u) < 0x08000000u;
}
extern void sr_oor(uint32_t a, uint32_t v, int store);   /* records out-of-range access (diag) */
static inline uint8_t  sr_r8 (uint32_t a) { if (sr_inrange(a)) return *(uint8_t  *)SR_HOST(a); sr_oor(a,0,0); return 0u; }
static inline uint16_t sr_r16(uint32_t a) { if (sr_inrange(a)) return *(uint16_t *)SR_HOST(a); sr_oor(a,0,0); return 0u; }
static inline uint32_t sr_r32(uint32_t a) { if (sr_inrange(a)) return *(uint32_t *)SR_HOST(a); sr_oor(a,0,0); return 0u; }
static inline void sr_w8 (uint32_t a, uint8_t  v) { if (sr_inrange(a)) *(uint8_t  *)SR_HOST(a) = v; else sr_oor(a,v,1); }
static inline void sr_w16(uint32_t a, uint16_t v) { if (sr_inrange(a)) *(uint16_t *)SR_HOST(a) = v; else sr_oor(a,v,1); }
static inline void sr_w32(uint32_t a, uint32_t v) { if (sr_inrange(a)) *(uint32_t *)SR_HOST(a) = v; else sr_oor(a,v,1); }

#define MEM_R8(a)   sr_r8(a)
#define MEM_R16(a)  sr_r16(a)
#define MEM_R32(a)  sr_r32(a)
#define MEM_W8(a,v)  sr_w8((a), (uint8_t)(v))
#define MEM_W16(a,v) sr_w16((a), (uint16_t)(v))
#define MEM_W32(a,v) sr_w32((a), (uint32_t)(v))

void  sr_mem_init(void);
void  sr_load_segment(uint32_t vaddr, const void *data, uint32_t len);

/* Unaligned word access (MIPS LWL/LWR/SWL/SWR), little-endian, matching PPSSPP's
 * interpreter. The load forms take the current rt and the effective address and return the
 * merged register value; the store forms read-modify-write the aligned word at addr&~3. */
uint32_t sr_lwl(uint32_t rtv, uint32_t addr);
uint32_t sr_lwr(uint32_t rtv, uint32_t addr);
void     sr_swl(uint32_t addr, uint32_t rtv);
void     sr_swr(uint32_t addr, uint32_t rtv);

/* VFPU source/destination prefix application (ARCHITECTURE section 6.4), ported from
 * PPSSPP. sr_vread reads n lanes from physical indices idx[], then applies a source
 * prefix (swizzle/abs/negate/constant). sr_vwrite applies the destination prefix
 * (saturate) and write mask, then stores. The prefix value comes from s->vfpuCtrl. */
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix);
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix);

/* VFPU transcendentals, exact ports of PPSSPP's table-based kernels (the PSP hardware does
 * not compute these with IEEE math). The lookup tables are loaded once from assets/vfpu/
 * (override the directory with PSP_VFPU_TABLES). */
float sr_vfpu_rcp(float x);
float sr_vfpu_rsqrt(float x);
float sr_vfpu_sqrt(float x);
float sr_vfpu_sin(float x);
float sr_vfpu_cos(float x);
float sr_vfpu_exp2(float x);

/* Single-instruction VFPU interpreter (src/rt/vfpu_interp.c). Returns SR_VFPU_COMPUTE for a
 * value-producing op (compare v[]/f[] to the reference trace), SR_VFPU_STATE for a prefix/control op
 * (nothing to compare), or SR_VFPU_OTHER for ops not handled here (e.g. VFPU load/store). */
#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2
int sr_vfpu_interp(CpuState *s, uint32_t op);

/* Dispatch: guest address -> native recompiled function (section 7). Computed jumps/calls go
 * through here; unknown targets would fall to the interpreter once it is linked in. */
typedef void (*RecompFn)(CpuState *);
void     sr_register(uint32_t addr, RecompFn fn);
RecompFn sr_lookup(uint32_t addr);
void     dispatch(CpuState *s, uint32_t target);

/* Tracing. When a trace file is open, the generated code reports each instruction. sr_begin
 * snapshots the register file and records pc/op; sr_end diffs and emits the line, with an
 * optional store (addr/size) read back from guest memory. Emit order follows PPSSPP: a branch
 * reports before its delay slot. */
int  sr_trace_open(const char *path, const char *target, uint32_t start_pc);
void sr_trace_close(void);
void sr_begin(const CpuState *s, uint32_t pc, uint32_t op);
void sr_end(const CpuState *s, uint32_t mem_addr, int mem_size);

/* HLE boundary marker used by the bring-up driver: a call to an unresolved import stop the
 * traced run so the comparison ends exactly where the reference trace reaches its first syscall. */
void sr_hle_call(CpuState *s, uint32_t nid);
extern int sr_hit_hle;

/* HLE syscall dispatch. The recompiled import stub at <stub> calls sr_syscall with the NID
 * resolved from the PRX import table. It dispatches to the registered handler (which reads
 * arguments from $a0-$a3 and returns the $v0 value), then poisons the caller-saved temp
 * registers to 0xDEADBEEF exactly as PPSSPP's kernel does, and writes the return to $v0. A
 * NID with no handler logs and stops at the HLE boundary (longjmp) so bring-up can see which
 * import to implement next. sr_last_nid records the most recent dispatched NID. */
/* Kernel object UID allocator, shared by every object type (threads, memory blocks, callbacks,
 * modules, ...) exactly like PPSSPP's single KernelObjectPool. PPSSPP hands out uid = index +
 * handleOffset(0x100) starting at index initialNextID(0x10), i.e. 0x110, 0x111, ... and games
 * index their own per-object tables by (uid - 0x100), so the base and a single shared counter
 * matter. */
uint32_t sr_alloc_uid(void);

/* sceGe display-list GPU (src/rt/ge.c): execute a GE command list, rasterising into VRAM. */
void ge_run_list(uint32_t addr);
uint32_t ge_framebuffer(void);

/* Interactive window front-end (src/rt/gui.c, Win32). gui_init opens the window; gui_present is
 * called from sceDisplaySetFrameBuf to show a frame, pump messages, and sample the keyboard;
 * gui_buttons returns the live PSP pad state; gui_on reports whether the window is active. */
void     gui_init(const char *title);
int      gui_on(void);
uint32_t gui_buttons(void);
void     gui_analog(uint8_t *lx, uint8_t *ly);   /* live left-stick (0..255, 128=centre) */
int      gui_pad_present(void);                  /* 1 when a game controller is connected */
void     gui_present(uint32_t fbaddr, int fmt, uint32_t stride);
int      gui_present_rgba(const uint32_t *px);   /* viewer: present a 480x272 XRGB buffer */

typedef uint32_t (*HleFn)(CpuState *s);
void     sr_hle_register(uint32_t nid, const char *name, HleFn fn);
void     sr_hle_init(void);     /* registers all built-in handlers (idempotent) */
void     sr_syscall(CpuState *s, uint32_t nid);
extern uint32_t sr_last_nid;

/* Cooperative scheduler with preemptive yield points (src/rt/sched.c). The recompiled code
 * calls SR_YIELD at function entry and loop back-edges; when the scheduler is active and the
 * thread's timeslice runs out, it switches to another ready thread (Windows fibers). When the
 * scheduler is off (per-function differential, plain driver) SR_YIELD is a cheap no-op that
 * changes nothing, so it does not affect the existing trace verification. */
extern int     sr_sched_on;
extern int32_t sr_timeslice;
void sr_yield(CpuState *s);
#define SR_YIELD(s) do { if (sr_sched_on && --sr_timeslice <= 0) sr_yield(s); } while (0)

/* Scheduler API used by the thread HLE handlers and the driver. */
void     sched_init(CpuState *cpu);                 /* CpuState the running thread reads/writes */
uint32_t sched_create_thread(uint32_t entry, int priority, uint32_t stack_size);
void     sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp);
void     sched_exit_current(void);                  /* current thread is done */
void     sched_delay_current(uint32_t usec);        /* block current thread for usec */
void     sched_preempt(void);                       /* yield now if a higher-priority thread is ready */
void     sched_block_on(uint32_t obj);              /* block current thread until sched_wake(obj) */
void     sched_wait_vblank(void);                   /* block current thread until the next delivered vblank */
int      sched_block_on_timeout(uint32_t obj, uint32_t usec);  /* returns 1 if timed out */
void     sched_wake(uint32_t obj);                  /* ready all threads blocked on obj */
void     sched_thread_sleep(void);                  /* sceKernelSleepThread[CB] (wakeup-count) */
void     sched_thread_wakeup(uint32_t uid);         /* sceKernelWakeupThread (banks if not asleep) */
void     sched_set_priority(uint32_t uid, int priority);   /* sceKernelChangeThreadPriority */
void     sched_terminate_thread(uint32_t uid);      /* sceKernelTerminate[Delete]Thread */
int      sched_thread_cancel_wakeup(uint32_t uid);  /* sceKernelCancelWakeupThread; uid 0=current */
typedef struct SrThreadRunStatus {
    uint32_t size;
    uint32_t status;
    uint32_t currentPriority;
    uint32_t waitType;
    uint32_t waitId;
    uint32_t wakeupCount;
    uint32_t runClocksLow;
    uint32_t runClocksHigh;
    uint32_t intrPreemptCount;
    uint32_t threadPreemptCount;
    uint32_t releaseCount;
} SrThreadRunStatus;
int      sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out);
int      sched_current_priority(void);
int      sched_is_dormant(uint32_t uid);
uint32_t sched_current_uid(void);
void     sched_run(uint32_t entry, uint32_t arglen, uint32_t argp);  /* run from the entry thread */

/* A function the static recompiler could not translate (an unimplemented instruction). The
 * body traps loudly with the guest address and reason rather than silently doing nothing, so
 * reaching it during a run is unmistakable. Every such function is listed in STUBS.md. */
void sr_unimplemented(uint32_t pc, const char *reason);

#include <setjmp.h>
extern jmp_buf g_hle_jmp;  /* sr_hle_call longjmps here so the run stops at the HLE boundary */

#endif
