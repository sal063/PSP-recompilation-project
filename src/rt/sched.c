/* Cooperative thread scheduler for the recompiled game (Phase 6).
 *
 * PSP threads are priority-scheduled and a busy-waiting thread is preempted by its timeslice
 * so a sibling can run. The recompiled code is straight C, so to suspend a thread mid-call-
 * stack we run each guest thread on its own Windows fiber and switch between them. All threads
 * share one CpuState; on a switch its contents are saved into the outgoing thread's control
 * block and the incoming thread's are loaded, so the single register file follows whichever
 * thread is running. SR_YIELD (emitted by codegen at function entry and loop back-edges) burns
 * the timeslice and switches when it reaches zero, giving preemption without a real timer.
 *
 * This is a simplified model: highest-priority ready thread runs; equal priority round-robins;
 * sceKernelDelayThread blocks until enough yields have elapsed. It is enough to interleave the
 * boot threads the way the game's startup expects, not a cycle-accurate kernel.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

int     sr_sched_on = 0;
int32_t sr_timeslice = 0;

#define TIMESLICE 20000          /* yields per thread run before preemption */
#define MAXTHREADS 128

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_WAIT_DELAY, TH_WAIT_OBJ };
enum { PSP_THREAD_RUNNING = 1, PSP_THREAD_READY = 2, PSP_THREAD_WAITING = 4, PSP_THREAD_STOPPED = 16 };
enum { PSP_WAIT_NONE = 0, PSP_WAIT_SLEEP = 1, PSP_WAIT_DELAY = 2, PSP_WAIT_OBJECT = 3 };

typedef struct {
    uint32_t uid;
    int      state;
    int      priority;
    uint32_t entry, arglen, argp;
    void    *fiber;
    int      started;            /* fiber has begun running its body */
    uint64_t wake;               /* scheduler tick to wake at (TH_WAIT_DELAY) */
    uint32_t wait_obj;           /* object uid this thread waits on (TH_WAIT_OBJ) */
    int      wakeups;            /* pending sceKernelWakeupThread count (sleep/wakeup semantics) */
    int      sleeping;           /* 1 while blocked in sceKernelSleepThread[CB] */
    uint64_t vbl_seen;           /* s_vbl_count this thread last consumed (vblank latch) */
    uint32_t sp_init, k0_init;   /* initial sp/k0 (to re-seed registers on a restart) */
    CpuState saved;              /* register file while not running */
} TCB;

static TCB      s_tcb[MAXTHREADS];
static int      s_ntcb = 0;
static int      s_cur = -1;      /* index of running thread, -1 = scheduler */
static void    *s_sched_fiber = NULL;
static CpuState *s_cpu = NULL;
static uint64_t s_tick = 0;
static uint32_t s_gp = 0;        /* module global pointer, inherited by created threads */
static uint32_t s_stack_top = 0x09f00000;  /* sibling thread stacks grow down from here */

static TCB *tcb_by_uid(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].uid == uid) return &s_tcb[i];
    return NULL;
}

static uint32_t resolve_thread_uid(uint32_t uid) {
    return uid ? uid : (s_cur >= 0 ? s_tcb[s_cur].uid : 0);
}

void sched_init(CpuState *cpu) {
    s_cpu = cpu;
    s_gp = cpu->r[28];           /* the driver seeded gp from the module's # init */
    s_sched_fiber = ConvertThreadToFiber(NULL);
    sr_sched_on = 1;
    sr_timeslice = TIMESLICE;
}

uint32_t sched_current_uid(void) { return s_cur >= 0 ? s_tcb[s_cur].uid : 0; }

/* Diagnostic: dump every thread's state, entry, saved PC, and what it waits on. Reveals a thread
 * blocked on a sema/event that is never signalled (a likely scene-transition gate). */
void sched_dump_threads(void) {
    static const char *st[] = { "DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ" };
    fprintf(stderr, "--- threads (%d) cur=%d tick=%llu ---\n", s_ntcb, s_cur, (unsigned long long)s_tick);
    for (int i = 0; i < s_ntcb; i++) {
        TCB *t = &s_tcb[i];
        fprintf(stderr, "  uid=0x%x entry=0x%08x pc=0x%08x %-10s pri=%d wait_obj=0x%x wake=%llu\n",
                t->uid, t->entry, t->saved.pc, st[t->state < 5 ? t->state : 0], t->priority,
                t->wait_obj, (unsigned long long)t->wake);
    }
}

/* Run the game's VBLANK interrupt handler (a guest function) on a dedicated interrupt context.
 * It usually calls sceKernelWakeupThread, readying the game thread. Called by the scheduler
 * when no thread is runnable (i.e. once per simulated frame). */
uint32_t sr_vblank_handler(void);
uint32_t sr_vblank_arg(void);
/* Threads blocked in sceDisplayWaitVblankStart wait on this object; deliver_vblank readies
 * them, so the render loop draws exactly once per delivered vblank instead of spinning. */
#define VBLANK_WAIT_OBJ 0x56424c4bu   /* "VBLK" */
static uint64_t s_vbl_count = 0;     /* vblanks delivered so far (latch reference) */

/* The vblank is an interrupt: it can fire WHILE a thread runs (from the yield path, or while a
 * host call like a vsynced swapchain present blocks). A thread that then calls
 * sceDisplayWaitVblankStart must not sleep a whole extra period for the NEXT one -- that
 * hard-quantizes any frame whose work+present crosses the period to 30/20 fps. Latch it
 * instead: if a vblank was delivered since this thread last consumed one, return immediately
 * (consume the pending vblank); only block when none is pending. */
void sched_wait_vblank(void) {
    if (s_cur >= 0) {
        TCB *t = &s_tcb[s_cur];
        if (t->vbl_seen != s_vbl_count) { t->vbl_seen = s_vbl_count; return; }
        sched_block_on(VBLANK_WAIT_OBJ);
        t->vbl_seen = s_vbl_count;
        return;
    }
    sched_block_on(VBLANK_WAIT_OBJ);
}

/* ---- virtual time ------------------------------------------------------------------------
 * The scheduler keeps a microsecond clock (s_vtime_us) that all timed waits compare against.
 * With vblank pacing ON (default) it tracks the real QPC clock, so sceKernelDelayThread and
 * timed sema/event waits elapse in REAL time -- the same timebase the paced vblanks run on.
 * (They used to be counted in scheduler "ticks" -- one tick per yield -- an elastic unit
 * that passed in microseconds while threads were busy and was jumped over when idle. Game
 * speed then depended on incidental scheduling: menus pacing via DelayThread ran 2x, and
 * mission logic threads ran a random number of iterations per frame.)
 * With SR_NOVBPACE=1 (turbo) it advances 1/59.94 s per delivered vblank and jumps over idle
 * delay waits, so everything runs as fast as the host allows. */
static uint64_t s_vtime_us = 0;
static int s_pace_on = -1;
static LARGE_INTEGER s_qfreq;
static LONGLONG s_qpc0;
static double s_vbl_next;            /* QPC counts: when the next vblank is due */
static uint64_t s_vbl_next_us = 0;   /* same deadline on the s_vtime_us scale (cheap compare) */

static void vbl_next_us_sync(void) {
    s_vbl_next_us = (uint64_t)((s_vbl_next - (double)s_qpc0) * 1000000.0 / (double)s_qfreq.QuadPart);
}

static void pace_setup(void) {
    if (s_pace_on >= 0) return;
    s_pace_on = getenv("SR_NOVBPACE") ? 0 : 1;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&s_qfreq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    s_qpc0 = c.QuadPart;
    s_vbl_next = (double)c.QuadPart;
    vbl_next_us_sync();
}

/* True when the scheduler paces vblanks to real time (the default). gui_present uses this to
 * skip its own legacy 60 Hz sleep: two independent pacers stack and push frames past the
 * vblank period (the second one then costs a whole extra frame). */
int sched_vbl_paced(void) { pace_setup(); return s_pace_on > 0; }

static uint64_t qpc_us(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)(c.QuadPart - s_qpc0) * 1000000.0 / (double)s_qfreq.QuadPart);
}

/* Advance the virtual clock to "now" (real-time mode only; monotonic). */
static void vtime_refresh(void) {
    pace_setup();
    if (s_pace_on) {
        uint64_t t = qpc_us();
        if (t > s_vtime_us) s_vtime_us = t;
    }
}

/* Sleep (host) until the virtual clock reaches target_us. Real-time mode only. */
static void sleep_until_us(uint64_t target_us) {
    for (;;) {
        uint64_t now = qpc_us();
        if (now >= target_us) break;
        uint64_t d = target_us - now;
        if (d > 2000) Sleep((DWORD)((d - 1000) / 1000));
        else if (d > 300) Sleep(1);
        /* else spin out the remainder via the loop */
    }
    vtime_refresh();
}

/* Pace vblank delivery to the PSP's real ~59.94 Hz. Without this, vblanks fire whenever
 * the scheduler goes idle, so game speed becomes "however fast the GE renders": apps that
 * flip once per vblank were rescued by gui_present's 60 Hz sleep, but apps that wait 2
 * vblanks per flip (30 fps games, some menus) ran at double speed once the GPU rasterizer
 * made the GE fast. Pacing the vblank itself makes every wait ratio correct.
 * SR_NOVBPACE=1 disables (turbo / old behaviour). */
static void vblank_pace(void) {
    pace_setup();
    if (!s_pace_on) { s_vtime_us += 16667; return; }   /* turbo: virtual frame per vblank */
    double period = (double)s_qfreq.QuadPart / 59.94;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (s_vbl_next > (double)now.QuadPart) {
        double ms = (s_vbl_next - (double)now.QuadPart) * 1000.0 / (double)s_qfreq.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.0));
        do { QueryPerformanceCounter(&now); } while ((double)now.QuadPart < s_vbl_next);
    }
    s_vbl_next += period;
    /* fell behind by more than a frame (slow scene, breakpoint): resync, don't fast-forward */
    if ((double)now.QuadPart > s_vbl_next) s_vbl_next = (double)now.QuadPart;
    vbl_next_us_sync();
    vtime_refresh();
}

/* Microseconds of virtual time until the next vblank is due (0 when overdue). */
static uint64_t vblank_due_us(void) {
    pace_setup();
    if (!s_pace_on) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((double)now.QuadPart >= s_vbl_next) return 0;
    return (uint64_t)((s_vbl_next - (double)now.QuadPart) * 1000000.0 / (double)s_qfreq.QuadPart);
}

static void deliver_vblank(void) {
    vblank_pace();
    s_vbl_count++;
    uint32_t h = sr_vblank_handler();
    static unsigned long long vb = 0;
    if (getenv("SR_VBLOG") && (++vb % 1) == 0) fprintf(stderr, "vblank #%llu (handler=0x%x)\n", vb, h);
    sched_wake(VBLANK_WAIT_OBJ);
    if (!h) return;
    if (getenv("SR_PCSAMPLE")) {
        static unsigned long n = 0;
        if ((n++ % 30) == 0) fprintf(stderr, "PCSAMPLE frame=%lu interrupted_pc=0x%08x ra=0x%08x\n",
                                     n, s_cpu->pc, s_cpu->r[31]);
    }
    CpuState save;
    memcpy(&save, s_cpu, sizeof(CpuState));
    memset(s_cpu, 0, sizeof(CpuState));
    s_cpu->r[29] = 0x09df0000;          /* dedicated interrupt stack (below thread stacks) */
    s_cpu->r[28] = s_gp;
    s_cpu->r[4] = sr_vblank_arg();       /* a0 = registered arg */
    s_cpu->r[31] = 0;
    s_cpu->vfpuCtrl[0] = 0xe4; s_cpu->vfpuCtrl[1] = 0xe4;
    int save_cur = s_cur; s_cur = -1;    /* interrupt context: SR_YIELD must not switch */
    dispatch(s_cpu, h);
    s_cur = save_cur;
    memcpy(s_cpu, &save, sizeof(CpuState));
    extern void sr_vblank_tick(void);
    sr_vblank_tick();
}

static void CALLBACK fiber_proc(void *param) {
    TCB *t = (TCB *)param;
    for (;;) {
        /* Entry into the thread body: set up args and the standard return address (0). */
        s_cpu->r[4] = t->arglen;
        s_cpu->r[5] = t->argp;
        s_cpu->r[31] = 0;
        dispatch(s_cpu, t->entry);    /* runs until the thread returns or exits */
        /* Returned without calling sceKernelExitThread: treat as exit. */
        t->state = TH_DORMANT;
        SwitchToFiber(s_sched_fiber); /* back to scheduler; fiber may be reused if restarted */
    }
}

uint32_t sched_create_thread(uint32_t entry, int priority, uint32_t stack_size) {
    if (s_ntcb >= MAXTHREADS) {
        fprintf(stderr, "sched_create_thread: MAXTHREADS(%d) exhausted (entry=0x%08x)\n", MAXTHREADS, entry);
        return 0;
    }
    if (getenv("SR_THLOG")) fprintf(stderr, "create thread #%d entry=0x%08x pri=%d stack=%u\n", s_ntcb, entry, priority, stack_size);
    TCB *t = &s_tcb[s_ntcb++];
    memset(t, 0, sizeof(*t));
    t->uid = sr_alloc_uid();
    t->state = TH_DORMANT;
    t->priority = priority;
    t->entry = entry;
    t->started = 0;
    t->fiber = NULL;
    /* Give the thread a stack, the module gp, and a per-thread k0 (r26) area. The PSP kernel
     * sets k0 to a small per-thread control region near the top of the thread stack, and the
     * game's thread bodies use it as a base pointer (e.g. sw r21,4(k0)); leaving it 0 faults.
     * (The entry thread's saved state is overwritten with the driver's seed in sched_run.) */
    uint32_t sz = stack_size ? stack_size : 0x40000;
    s_stack_top -= (sz + 0xFFu) & ~0xFFu;
    uint32_t top = (s_stack_top + sz) & ~0xFu;
    uint32_t k0 = (top - 0x800) & ~0xFu;           /* reserved k0/TLS region below the top */
    t->k0_init = k0;
    t->sp_init = (k0 - 0x10) & ~0xFu;              /* sp grows down below the k0 region */
    t->saved.r[26] = t->k0_init;
    t->saved.r[29] = t->sp_init;
    t->saved.r[28] = s_gp;
    t->saved.vfpuCtrl[0] = 0xe4; t->saved.vfpuCtrl[1] = 0xe4; t->saved.vfpuCtrl[2] = 0;
    return t->uid;
}

void sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) {
    TCB *t = tcb_by_uid(uid);
    if (!t) return;
    if (getenv("SR_THLOG")) fprintf(stderr, "start thread uid=0x%x entry=0x%08x pri=%d arglen=%u%s\n",
                                    t->uid, t->entry, t->priority, arglen, t->started ? " (restart)" : "");
    /* PSP semantics: starting a DORMANT thread that ran before restarts it from its entry.
     * The old fiber is parked wherever the thread last gave up the CPU -- for a thread that
     * exited via sceKernelExitThread that is deep inside the exit syscall, so resuming it
     * would fall through past the exit into garbage (this stranded the BGM streamer thread
     * and with it the mission scene-switch fade). Throw the old fiber away and re-seed the
     * register file so the scheduler re-enters the body fresh. */
    if (t->started && t->state == TH_DORMANT) {
        if (t->fiber) { DeleteFiber(t->fiber); t->fiber = NULL; }
        t->started = 0;
        t->wakeups = 0; t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
        memset(&t->saved, 0, sizeof(t->saved));
        t->saved.r[26] = t->k0_init;
        t->saved.r[29] = t->sp_init;
        t->saved.r[28] = s_gp;
        t->saved.vfpuCtrl[0] = 0xe4; t->saved.vfpuCtrl[1] = 0xe4; t->saved.vfpuCtrl[2] = 0;
        t->saved.pc = t->entry;
    }
    t->arglen = arglen; t->argp = argp;
    t->state = TH_READY;
}

/* Pick the highest-priority runnable thread (lowest priority number). Wakes delayed threads
 * whose deadline has passed. Returns an index or -1 if nothing is runnable. */
static int pick_next(void) {
    for (int i = 0; i < s_ntcb; i++)
        if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
            s_vtime_us >= s_tcb[i].wake)
            s_tcb[i].state = TH_READY;   /* delay expired, or a timed wait timed out */
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (s_tcb[i].state != TH_READY) continue;
        if (best < 0 || s_tcb[i].priority < s_tcb[best].priority) best = i;
    }
    return best;
}

/* Save the running thread's registers, return to the scheduler, which selects and resumes the
 * next thread. Called from a thread fiber. */
static void switch_to_scheduler(void) {
    SwitchToFiber(s_sched_fiber);
}

void sr_yield(CpuState *s) {
    sr_timeslice = TIMESLICE;
    if (s_cur < 0) return;                 /* not in a thread */
    s_tick++;
    vtime_refresh();                       /* so a busy thread sees siblings' delays expire */
    /* The PSP vblank is an INTERRUPT: it fires even while threads compute. Only delivering it
     * from the scheduler's idle loop deadlocks any code that polls vblank-derived state
     * (sceDisplayGetVcount spin-pacing, two threads ping-ponging on each other) -- the vcount
     * can never advance because nobody ever blocks. Deliver an overdue vblank from the yield
     * path too; deliver_vblank saves/restores the live register file and runs the handler with
     * s_cur = -1, so it is interrupt-safe here. */
    if (s_pace_on > 0 && s_vtime_us >= s_vbl_next_us)
        deliver_vblank();
    TCB *t = &s_tcb[s_cur];
    /* Only switch if someone else could run; otherwise keep going (avoids pointless churn). */
    int other = 0;
    for (int i = 0; i < s_ntcb; i++)
        if (i != s_cur && (s_tcb[i].state == TH_READY ||
            ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
             s_vtime_us >= s_tcb[i].wake))) { other = 1; break; }
    if (!other) {
        /* Diagnostic: if one thread spins with nobody else runnable for a long time, it is a
         * deadlock (a producer thread exited or is blocked). Dump the thread table with each
         * thread's saved PC and wait object; repeat occasionally so later phases are visible. */
        static unsigned long long spun = 0;
        static int dumps = 0;
        if (++spun % 50000000ull == 2001 && dumps < 6) {
            dumps++;
            static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
            int ready = 0, delay = 0, dormant = 0, run = 0;
            for (int i = 0; i < s_ntcb; i++) {
                if (s_tcb[i].state == TH_READY) ready++;
                else if (s_tcb[i].state == TH_WAIT_DELAY) delay++;
                else if (s_tcb[i].state == TH_DORMANT) dormant++;
                else run++;
            }
            fprintf(stderr, "sched: spin on uid 0x%x at pc=0x%08x ra=0x%08x; threads=%d ready=%d delay=%d dormant=%d running=%d\n",
                    t->uid, s->pc, s->r[31], s_ntcb, ready, delay, dormant, run);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %-10s prio %d pc=0x%08x ra=0x%08x wait_obj=0x%x wakeups=%d\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state < 5 ? s_tcb[i].state : 0],
                        s_tcb[i].priority, s_tcb[i].saved.pc, s_tcb[i].saved.r[31],
                        s_tcb[i].wait_obj, s_tcb[i].wakeups);
        }
        return;
    }
    memcpy(&t->saved, s, sizeof(CpuState));
    if (t->state == TH_RUNNING) t->state = TH_READY;
    switch_to_scheduler();
    /* resumed later: our registers were restored into *s by the scheduler before SwitchToFiber */
}

/* PSP scheduling is strict-priority preemptive: the moment a higher-priority thread becomes
 * ready (e.g. sceKernelStartThread starts one), it runs instead of the current thread. Without
 * this, a low-priority boot thread that starts a high-priority worker and busy-waits on its
 * output would never let the worker run. Call after any op that readies a thread. */
void sched_preempt(void) {
    if (s_cur < 0) return;
    TCB *cur = &s_tcb[s_cur];
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (i == s_cur) continue;
        if (s_tcb[i].state == TH_READY && (best < 0 || s_tcb[i].priority < s_tcb[best].priority))
            best = i;
    }
    if (best >= 0 && s_tcb[best].priority < cur->priority) {   /* strictly higher priority ready */
        memcpy(&cur->saved, s_cpu, sizeof(CpuState));
        cur->state = TH_READY;
        switch_to_scheduler();
    }
}

void sched_delay_current(uint32_t usec) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (usec > 2000000u && getenv("SR_DELAYLOG"))   /* > 2s: catch a bogus huge delay */
        fprintf(stderr, "BIG DELAY uid=0x%x entry=0x%08x usec=%u (%.1fs)\n", t->uid, t->entry, usec, usec / 1e6);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_DELAY;
    /* Real microseconds of virtual time; any positive delay yields at least once. */
    vtime_refresh();
    t->wake = s_vtime_us + (usec ? usec : 1);
    switch_to_scheduler();
}

void sched_block_on(uint32_t obj) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = (uint64_t)-1;     /* infinite: only sched_wake releases it */
    switch_to_scheduler();
}

/* Block on obj, but also wake after usec of virtual time (a timed sema/event wait). Returns 1
 * if it timed out (the deadline passed), 0 if it was woken by a signal. */
int sched_block_on_timeout(uint32_t obj, uint32_t usec) {
    if (s_cur < 0) return 1;
    TCB *t = &s_tcb[s_cur];
    vtime_refresh();
    uint64_t deadline = s_vtime_us + (usec ? usec : 1);
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = deadline;
    switch_to_scheduler();
    vtime_refresh();
    return s_vtime_us >= deadline;   /* resumed: timed out if the deadline has passed */
}

void sched_wake(uint32_t obj) {
    for (int i = 0; i < s_ntcb; i++)
        if (s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == obj)
            s_tcb[i].state = TH_READY;
}

/* sceKernelSleepThread[CB]: PSP wakeup-count semantics. If a wakeup is already pending, consume it
 * and return without blocking; otherwise block until sceKernelWakeupThread targets this thread.
 * This is distinct from sceKernelDelayThread (a timed sleep) -- conflating the two left the main
 * thread sleeping ~forever on a poisoned (0xDEADBEEF) delay argument. */
void sched_thread_sleep(void) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (t->wakeups > 0) { t->wakeups--; return; }   /* pending wakeup: don't block */
    t->sleeping = 1;
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = t->uid;       /* sleep marker: woken only by sched_thread_wakeup(uid) */
    t->wake = (uint64_t)-1;
    switch_to_scheduler();
}

/* sceKernelWakeupThread(uid): wake a sleeping thread, or bank a pending wakeup if it is not
 * currently asleep (so a wakeup issued before the sleep is not lost). */
void sched_thread_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].uid == uid) {
        if (s_tcb[i].sleeping && s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == uid) {
            s_tcb[i].sleeping = 0;
            s_tcb[i].state = TH_READY;
        } else {
            s_tcb[i].wakeups++;
        }
        return;
    }
}

/* sceKernelCancelWakeupThread(uid): returns the number of pending wakeups and clears them.
 * Passing uid 0 targets the current thread; ACX does this once per frame before sleeping. */
int sched_thread_cancel_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return -1;
    int old = t->wakeups;
    t->wakeups = 0;
    return old;
}

static uint32_t psp_thread_status(const TCB *t) {
    switch (t->state) {
        case TH_RUNNING: return PSP_THREAD_RUNNING;
        case TH_READY: return PSP_THREAD_READY;
        case TH_WAIT_DELAY:
        case TH_WAIT_OBJ: return PSP_THREAD_WAITING;
        case TH_DORMANT:
        default: return PSP_THREAD_STOPPED;
    }
}

static uint32_t psp_wait_type(const TCB *t) {
    if (t->state == TH_WAIT_DELAY) return PSP_WAIT_DELAY;
    if (t->state == TH_WAIT_OBJ && t->sleeping && t->wait_obj == t->uid) return PSP_WAIT_SLEEP;
    if (t->state == TH_WAIT_OBJ) return PSP_WAIT_OBJECT;
    return PSP_WAIT_NONE;
}

int sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->size = 0x2c;
    out->status = psp_thread_status(t);
    out->currentPriority = (uint32_t)t->priority;
    out->waitType = psp_wait_type(t);
    out->waitId = (t->state == TH_WAIT_DELAY) ? uid : t->wait_obj;
    out->wakeupCount = (uint32_t)t->wakeups;
    out->runClocksLow = (uint32_t)s_tick;
    out->runClocksHigh = (uint32_t)(s_tick >> 32);
    return 0;
}

void sched_exit_current(void) {
    if (s_cur < 0) return;
    uint32_t uid = s_tcb[s_cur].uid;
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x EXIT (entry 0x%08x)\n", uid, s_tcb[s_cur].entry);
    s_tcb[s_cur].state = TH_DORMANT;
    sched_wake(uid);             /* release threads in sceKernelWaitThreadEnd on this thread */
    switch_to_scheduler();
}

int sched_current_priority(void) { return s_cur >= 0 ? s_tcb[s_cur].priority : 32; }

/* sceKernelChangeThreadPriority: uid 0 = current thread. */
void sched_set_priority(uint32_t uid, int priority) {
    if (uid == 0 && s_cur >= 0) uid = s_tcb[s_cur].uid;
    TCB *t = tcb_by_uid(uid);
    if (t) t->priority = priority;
}

/* sceKernelTerminateDeleteThread: force another thread dormant so a later start restarts it from
 * its entry (the restart path resets registers/fiber). Terminating yourself exits. The old h_ok
 * stub left the "killed" thread running. */
void sched_terminate_thread(uint32_t uid) {
    if (s_cur >= 0 && s_tcb[s_cur].uid == uid) { sched_exit_current(); return; }
    TCB *t = tcb_by_uid(uid);
    if (!t || t->state == TH_DORMANT) return;
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x TERMINATED (entry 0x%08x)\n", uid, t->entry);
    t->state = TH_DORMANT;
    t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    sched_wake(uid);             /* release threads in sceKernelWaitThreadEnd on this thread */
}

int sched_is_dormant(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].uid == uid) return s_tcb[i].state == TH_DORMANT;
    return 1;   /* unknown thread: treat as ended */
}

/* The scheduler loop. Runs on the main (converted) fiber. Creates the entry thread, then keeps
 * resuming the highest-priority ready thread until none remain runnable. */
void sched_run(uint32_t entry, uint32_t arglen, uint32_t argp) {
    uint32_t uid = sched_create_thread(entry, 32, 0);
    TCB *t0 = tcb_by_uid(uid);
    /* The entry (module_start) keeps the driver-seeded state -- real sp, gp, and module args
     * -- rather than the synthetic thread stack. */
    memcpy(&t0->saved, s_cpu, sizeof(CpuState));
    arglen = s_cpu->r[4]; argp = s_cpu->r[5];
    sched_start_thread(uid, arglen, argp);

    static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
    unsigned long long iters = 0;
    for (;;) {
        if (getenv("SCHED_DUMP") && (++iters % 400000) == 0) {
            fprintf(stderr, "--- sched dump (tick=%llu) ---\n", (unsigned long long)s_tick);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %s prio %d wait_obj 0x%x\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state], s_tcb[i].priority, s_tcb[i].wait_obj);
        }
        int idx = pick_next();
        if (idx < 0) {
            /* No thread is ready. If a timed wait expires before the next vblank is due,
             * sleep precisely to it (sub-frame delays keep their real duration); otherwise
             * deliver a VBLANK interrupt (real-time paced) -- the per-frame heartbeat that
             * drives the render loop. Vblank delivery can't starve: it runs whenever due. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            vtime_refresh();
            if (s_pace_on && soonest != (uint64_t)-1 && soonest > s_vtime_us &&
                soonest - s_vtime_us < vblank_due_us())
                sleep_until_us(soonest);
            else
                deliver_vblank();
            idx = pick_next();
        }
        if (idx < 0) {
            /* Still nothing. Stop only when nothing is even waiting on a deadline. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            if (soonest == (uint64_t)-1) break;   /* truly nothing runnable (all infinite waits) */
            if (!s_pace_on) {                     /* turbo: jump the clock over the wait */
                if (soonest > s_vtime_us) s_vtime_us = soonest;
                idx = pick_next();
                if (idx < 0) break;
            } else {
                continue;   /* paced: keep delivering vblanks; real time reaches the deadline */
            }
        }
        TCB *t = &s_tcb[idx];
        s_cur = idx;
        t->state = TH_RUNNING;
        memcpy(s_cpu, &t->saved, sizeof(CpuState));   /* load this thread's registers */
        sr_timeslice = TIMESLICE;          /* a fresh slice for this run (the counter is global) */
        if (!t->started) {
            t->started = 1;
            /* Guest calls become native C calls, so a deep guest call chain needs a deep host
             * stack. Reserve a large fiber stack (committed on demand) to match. */
            t->fiber = CreateFiberEx((SIZE_T)1 << 18, (SIZE_T)64 << 20, 0, fiber_proc, t);
        }
        SwitchToFiber(t->fiber);           /* run until it yields/blocks/exits */
        s_cur = -1;
        s_tick++;
    }
}
