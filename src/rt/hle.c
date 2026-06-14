/* High-level emulation (Phase 6): NID -> handler dispatch for the recompiled game's imports.
 *
 * The PRX import table maps each .sceStub.text stub to a (library, NID); tools/imports.py
 * resolves it and the codegen emits sr_syscall(s, NID) in each stub. A handler reads its
 * arguments from $a0-$a3 (and the stack for further args) and returns the $v0 value.
 *
 * The kernel object identifiers (thread/block UIDs) and allocation addresses PPSSPP returns
 * come from its own boot-time allocators, so they are not reproducible here without simulating
 * PPSSPP's whole kernel. This HLE is instead internally consistent: it hands out its own UIDs
 * and allocates from its own bump pointer, and the game uses those values uniformly. Functional
 * equivalence is checked by the sequence of import calls (by NID), not by UID values.
 *
 * After each handler, the caller-saved temp registers are poisoned to 0xDEADBEEF exactly as
 * PPSSPP's SetDeadbeefRegs does, so traces stay aligned on the registers the game actually
 * keeps (it never relies on caller-saved registers surviving a call).
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "iso.h"
#include "pgf.h"
#include "nid_names.h"   /* sr_nid_name(): names unknown NIDs in the trap below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>

uint32_t sr_last_nid = 0;

/* ---- handler table ---- */

typedef struct { uint32_t nid; const char *name; HleFn fn; } HleEntry;
#define HLE_CAP 512
static HleEntry s_hle[HLE_CAP];
static int s_hle_n = 0;

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    if (s_hle_n < HLE_CAP) { s_hle[s_hle_n].nid = nid; s_hle[s_hle_n].name = name; s_hle[s_hle_n].fn = fn; s_hle_n++; }
}

static HleEntry *hle_find(uint32_t nid) {
    for (int i = 0; i < s_hle_n; i++) if (s_hle[i].nid == nid) return &s_hle[i];
    return NULL;
}

/* ---- argument / return helpers ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
/* 5th+ integer arguments are passed on the caller's stack at sp+16, sp+20, ... */
/* Argument 5+idx of an HLE call. PSP userland is MIPS EABI: arguments 1..8 arrive in
 * a0-a3,t0-t3 (r4..r11) and only 9+ go to the stack (sp+0) -- same as PPSSPP's PARAM(n).
 * This used to read the o32 stack home slots (sp+16+), which EABI callers never write:
 * every >4-argument HLE call received stack garbage (found via sceMpegRingbufferConstruct
 * getting its fill callback from there -- the movie player jumped into the ring buffer). */
static uint32_t stack_arg(CpuState *s, int idx) {
    return idx < 4 ? s->r[8 + idx] : MEM_R32(s->r[29] + (uint32_t)(idx - 4) * 4);
}

/* ---- kernel object UID + user-memory bump allocator ---- */

/* Single shared UID pool matching PPSSPP: uid = 0x110, 0x111, ... incrementing by 1. */
static uint32_t s_uid = 0x110;
uint32_t sr_alloc_uid(void) { return s_uid++; }

/* User-partition bump allocator. PPSSPP places the first sceKernelAllocPartitionMemory
 * Low block immediately after the loaded module (ACX's PRX BSS ends ~0x08b9xxxx), not at a
 * round boundary. Starting at the module end reproduces PPSSPP's address layout so the
 * game's internal sub-allocator yields the same buffer addresses as the reference run (the read
 * buffer and the decoded index table stay distinct). Calibrated: at 0x08c00000 the index
 * table landed at 0x08f32200 vs the reference run's 0x08ed3200 (0x5f000 high). */
static uint32_t s_heap = 0x08ba1000;     /* bump pointer in user RAM, just above ACX's module */
/* The kernel hands the game one user partition. PPSSPP's loader reports it as UserSbrk
 * 0x08ba1000..0x09e22800 for this module; s_heap starts at that base, so the free size the game
 * queries is (top - bump pointer) and shrinks as it allocates -- not a fixed fake. The game reads
 * this to choose its asset working-set / layout, so a constant 16 MB made it diverge. */
#define USER_PARTITION_TOP 0x09e22800u

typedef struct { uint32_t uid, addr, size; } Block;
static Block s_blocks[256];
static int s_nblocks = 0;

static uint32_t alloc_block(uint32_t size) {
    uint32_t addr = (s_heap + 0xFFu) & ~0xFFu;   /* 256-byte align */
    s_heap = addr + size;
    uint32_t uid = sr_alloc_uid();
    if (s_nblocks < 256) { s_blocks[s_nblocks].uid = uid; s_blocks[s_nblocks].addr = addr; s_blocks[s_nblocks].size = size; s_nblocks++; }
    return uid;
}
static uint32_t block_addr(uint32_t uid) {
    for (int i = 0; i < s_nblocks; i++) if (s_blocks[i].uid == uid) return s_blocks[i].addr;
    return 0;
}

/* ---- handlers ---- */

static uint32_t g_sdk_version = 0;

static uint32_t h_GetCompiledSdkVersion(CpuState *s) { (void)s; return g_sdk_version; }
static uint32_t h_SetCompiledSdkVersion(CpuState *s) { g_sdk_version = A0; return 0; }

/* sceUtilityGetSystemParamInt(id, int *out): write the system setting and return 0. PPSSPP's
 * defaults (Core/HLE/sceUtility.cpp registry): English (1), Western button order, 24h clock.
 * A no-op that leaves *out untouched makes the game read garbage for the language and load the
 * wrong region assets. IDs follow PSP_SYSTEMPARAM_ID_INT_*. */
static uint32_t h_GetSystemParamInt(CpuState *s) {
    uint32_t id = A0, out = A1, v;
    switch (id) {
        case 2:  v = 1;  break;   /* ADHOC_CHANNEL: automatic */
        case 3:  v = 0;  break;   /* WLAN_POWERSAVE: off */
        case 4:  v = 1;  break;   /* DATE_FORMAT: MMDDYYYY */
        case 5:  v = 0;  break;   /* TIME_FORMAT: 24h */
        case 6:  v = 0;  break;   /* TIMEZONE offset (minutes) */
        case 7:  v = 0;  break;   /* DAYLIGHTSAVINGS: off */
        case 8:  v = 1;  break;   /* LANGUAGE: English */
        case 9:  v = 1;  break;   /* BUTTON_PREFERENCE: cross = enter (Western) */
        default: v = 0;  break;
    }
    if (out) MEM_W32(out, v);
    if (getenv("SR_LANGLOG")) fprintf(stderr, "GetSystemParamInt id=%u -> %u\n", id, v);
    return 0;
}
/* sceUtilityGetSystemParamString(id, char *out, int len): nickname etc. Write a short ASCII name. */
/* sceCtrlGetIdleCancelThreshold(int *idlereset, int *idleback): both thresholds "disabled". */
static uint32_t h_CtrlGetIdleCancelThreshold(CpuState *s) {
    if (A0) MEM_W32(A0, 0xFFFFFFFFu);   /* -1 = idle cancel disabled (PPSSPP default) */
    if (A1) MEM_W32(A1, 0xFFFFFFFFu);
    return 0;
}

/* SysMemUserForUser */
static uint32_t h_AllocPartitionMemory(CpuState *s) {
    /* a0=partition, a1=name, a2=type, a3=size, [sp+16]=addr. Returns a block UID. */
    uint32_t size = A3;
    return alloc_block(size ? size : 16);
}
static uint32_t h_GetBlockHeadAddr(CpuState *s) { return block_addr(A0); }
static uint32_t h_FreePartitionMemory(CpuState *s) { (void)s; return 0; }
static uint32_t partition_free(void) {
    return s_heap < USER_PARTITION_TOP ? USER_PARTITION_TOP - s_heap : 0u;
}
/* A small accounting tail (the kernel keeps block headers per allocation) so the figure is not
 * the exact arithmetic free; PPSSPP reports e.g. 0x1a0b00 with several blocks live. */
static uint32_t h_TotalFreeMemSize(CpuState *s) { (void)s; uint32_t f = partition_free(); return f > (uint32_t)s_nblocks*0x100u ? f - (uint32_t)s_nblocks*0x100u : f; }
static uint32_t h_MaxFreeMemSize(CpuState *s) { (void)s; return partition_free(); }

/* ThreadManForUser, backed by the fiber scheduler (src/rt/sched.c). */
static uint32_t h_CreateThread(CpuState *s) {
    /* a0=name, a1=entry, a2=priority, a3=stackSize. Returns a UID bound to the entry. */
    return sched_create_thread(A1, (int)A2, A3);
}
static uint32_t h_StartThread(CpuState *s) {
    /* a0=thid, a1=arglen, a2=argp. Mark ready, then preempt if it is higher priority -- PSP
     * runs a higher-priority started thread immediately. */
    sched_start_thread(A0, A1, A2);
    sched_preempt();
    return 0;
}
static uint32_t h_ExitThread(CpuState *s) { (void)s; sched_exit_current(); return 0; }
static uint32_t h_DelayThread(CpuState *s) { sched_delay_current(A0); return 0; }
static uint32_t h_ChangeThreadPriority(CpuState *s) { sched_set_priority(A0, (int)A1); return 0; }
static uint32_t h_TerminateDeleteThread(CpuState *s) { sched_terminate_thread(A0); return 0; }
static uint32_t h_GetThreadIdSched(CpuState *s) { (void)s; return sched_current_uid(); }
static uint32_t h_GetThreadPriority(CpuState *s) { (void)s; return (uint32_t)sched_current_priority(); }
static uint32_t h_SleepThread(CpuState *s) { (void)s;
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "SLEEP uid=0x%x\n", sched_current_uid()); }
    sched_thread_sleep(); return 0; }
static uint32_t h_WakeupThread(CpuState *s) {
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "WAKEUP target=0x%x (from 0x%x)\n", A0, sched_current_uid()); }
    sched_thread_wakeup(A0); sched_preempt(); return 0; }
static uint32_t h_CancelWakeupThread(CpuState *s) {
    int old = sched_thread_cancel_wakeup(A0);
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "CANCEL_WAKE target=0x%x old=%d (from 0x%x)\n", A0, old, sched_current_uid()); }
    return old < 0 ? 0x800201a0u : (uint32_t)old;
}
static uint32_t h_WaitThreadEnd(CpuState *s) {
    uint32_t uid = A0;
    while (!sched_is_dormant(uid)) sched_block_on(uid);
    return 0;
}
static uint32_t h_ReferThreadRunStatus(CpuState *s) {
    uint32_t out = A1;
    if (out) {
        SrThreadRunStatus rs;
        int rc = sched_thread_run_status(A0, &rs);
        if (rc < 0) return 0x800201a0u;
        for (uint32_t i = 0; i < 0x2c; i++) MEM_W8(out + i, 0);
        MEM_W32(out + 0x00, rs.size);
        MEM_W32(out + 0x04, rs.status);
        MEM_W32(out + 0x08, rs.currentPriority);
        MEM_W32(out + 0x0c, rs.waitType);
        MEM_W32(out + 0x10, rs.waitId);
        MEM_W32(out + 0x14, rs.wakeupCount);
        MEM_W32(out + 0x18, rs.runClocksLow);
        MEM_W32(out + 0x1c, rs.runClocksHigh);
        MEM_W32(out + 0x20, rs.intrPreemptCount);
        MEM_W32(out + 0x24, rs.threadPreemptCount);
        MEM_W32(out + 0x28, rs.releaseCount);
        if (getenv("SR_WAKELOG")) { static int n=0; if (n++<12) fprintf(stderr,
            "REFER_RUN target=0x%x status=%u pri=%u waitType=%u waitId=0x%x wakeups=%u\n",
            A0, rs.status, rs.currentPriority, rs.waitType, rs.waitId, rs.wakeupCount); }
    }
    return 0;
}
static uint32_t h_ok(CpuState *s) { (void)s; return 0; }

/* ModuleMgrForUser */
static uint32_t h_module_uid(CpuState *s) { (void)s; return sr_alloc_uid(); }
static uint32_t h_GetModuleId(CpuState *s) { (void)s; return 0x112; }   /* main module's id (stable) */

/* Kernel_Library: interrupt suspend/resume. Suspend returns the prior state (use 1); resume
 * is void. */
static uint32_t h_CpuSuspendIntr(CpuState *s) { (void)s; return 1; }

/* Boot-path setup calls. These return success (or the value the reference run returns) so the boot
 * proceeds down the same branch. They do not yet model the subsystem behind them; the import
 * sequence vs the reference run is what validates that the returned value drives the same path. */
static uint32_t h_UmdCheckMedium(CpuState *s) { (void)s; return 1; }      /* medium present */
static uint32_t h_DmacMemcpy(CpuState *s) {
    /* a0=dst, a1=src, a2=size. A real DMA copy in guest memory. */
    uint32_t dst = A0, src = A1, n = A2;
    for (uint32_t i = 0; i < n; i++) MEM_W8(dst + i, MEM_R8(src + i));
    extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
    sr_gpu_vram_dirty(dst, n);   /* DMA into a GPU-cached framebuffer must invalidate it */
    return 0;
}
/* sceLibFont. The PSP firmware fonts (flash0 font PGFs) are not on the game ISO, so we load the
 * real PGF fonts bundled with the PPSSPP reference assets and rasterise glyphs with a port of
 * PPSSPP's PGF reader (src/rt/pgf.c). ltn0.pgf covers Latin + punctuation/symbols (the menus);
 * jpn0.pgf is used as a fallback for any CJK code points. If neither file is found we fall back to
 * a tiny synthetic 5x7 font so text still appears. */
static PGF *s_pgf_ltn = NULL, *s_pgf_jpn = NULL;
static int s_pgf_tried = 0;
static void font_load(void) {
    if (s_pgf_tried) return;
    s_pgf_tried = 1;
    char path[600];
    const char *dir = getenv("SR_FONTDIR");
    if (dir) { snprintf(path, sizeof path, "%s/ltn0.pgf", dir); s_pgf_ltn = pgf_open(path); }
    if (!s_pgf_ltn) s_pgf_ltn = pgf_open("third_party/ppsspp/assets/flash0/font/ltn0.pgf");
    if (!s_pgf_ltn) s_pgf_ltn = pgf_open("build/acx/ltn0.pgf");
    if (dir) { snprintf(path, sizeof path, "%s/jpn0.pgf", dir); s_pgf_jpn = pgf_open(path); }
    if (!s_pgf_jpn) s_pgf_jpn = pgf_open("third_party/ppsspp/assets/flash0/font/jpn0.pgf");
    if (!s_pgf_jpn) s_pgf_jpn = pgf_open("build/acx/jpn0.pgf");
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "font_load: ltn0=%s jpn0=%s\n", s_pgf_ltn ? "ok" : "MISSING", s_pgf_jpn ? "ok" : "MISSING");
}
/* Pick the font that actually has a glyph for `cc` (Latin first, then CJK fallback). */
static const PGF *font_for(int cc) {
    font_load();
    if (s_pgf_ltn && pgf_has_char(s_pgf_ltn, cc)) return s_pgf_ltn;
    if (s_pgf_jpn && pgf_has_char(s_pgf_jpn, cc)) return s_pgf_jpn;
    return NULL;
}

static uint32_t h_FontNewLib(CpuState *s) {
    if (A1) MEM_W32(A1, 0);            /* *errorCode = 0 */
    return 0x0f000001;                 /* non-NULL library handle */
}
static uint32_t h_FontOpen(CpuState *s) {
    /* (lib, index, mode, errorCode*) */
    if (A3) MEM_W32(A3, 0);
    return 0x0f000101;                 /* non-NULL font handle */
}
static uint32_t h_FontGetFontInfo(CpuState *s) {
    font_load();
    const PGF *p = s_pgf_ltn ? s_pgf_ltn : s_pgf_jpn;
    if (A1) { for (int i = 0; i < 0x108; i++) MEM_W8(A1 + (uint32_t)i, 0); if (p) pgf_get_font_info(p, A1); }
    return 0;
}
static uint32_t h_FontGetCharInfo(CpuState *s) {
    uint32_t ci = A2; if (!ci) return 0;
    uint32_t cc = A1 & 0xffff;
    const PGF *p = font_for((int)cc);
    if (p) { pgf_get_char_info(p, (int)cc, 0, ci); return 0; }
    /* No PGF available: synthetic 8x11 metrics fallback. */
    for (int i = 0; i < 0x3c; i++) MEM_W8(ci + (uint32_t)i, 0);
    int draw = (cc > 32);                      /* space and controls: empty */
    MEM_W32(ci + 0, draw ? 8 : 0);             /* bitmapWidth */
    MEM_W32(ci + 4, draw ? 11 : 0);            /* bitmapHeight */
    MEM_W32(ci + 8, 0);                        /* bitmapLeft */
    MEM_W32(ci + 12, 0);                       /* bitmapTop */
    MEM_W32(ci + 16, 8 << 6);                  /* sfp26Width */
    MEM_W32(ci + 20, 11 << 6);                 /* sfp26Height */
    MEM_W32(ci + 24, 0);                       /* ascender */
    MEM_W32(ci + 28, (uint32_t)(-(11 << 6)));  /* descender */
    MEM_W32(ci + 32, 0);                       /* bearingHX */
    MEM_W32(ci + 36, 0);                       /* bearingHY */
    MEM_W32(ci + 48, 8 << 6);                  /* sfp26AdvanceH */
    MEM_W32(ci + 52, 12 << 6);                 /* sfp26AdvanceV */
    return 0;
}

static const uint8_t *font5x7(uint32_t cc) {
    if (cc >= 'a' && cc <= 'z') cc -= 32;
    switch (cc) {
        case 'A': { static const uint8_t r[7] = {14,17,17,31,17,17,17}; return r; }
        case 'B': { static const uint8_t r[7] = {30,17,17,30,17,17,30}; return r; }
        case 'C': { static const uint8_t r[7] = {14,17,16,16,16,17,14}; return r; }
        case 'D': { static const uint8_t r[7] = {30,17,17,17,17,17,30}; return r; }
        case 'E': { static const uint8_t r[7] = {31,16,16,30,16,16,31}; return r; }
        case 'F': { static const uint8_t r[7] = {31,16,16,30,16,16,16}; return r; }
        case 'G': { static const uint8_t r[7] = {14,17,16,23,17,17,15}; return r; }
        case 'H': { static const uint8_t r[7] = {17,17,17,31,17,17,17}; return r; }
        case 'I': { static const uint8_t r[7] = {14,4,4,4,4,4,14}; return r; }
        case 'J': { static const uint8_t r[7] = {7,2,2,2,18,18,12}; return r; }
        case 'K': { static const uint8_t r[7] = {17,18,20,24,20,18,17}; return r; }
        case 'L': { static const uint8_t r[7] = {16,16,16,16,16,16,31}; return r; }
        case 'M': { static const uint8_t r[7] = {17,27,21,21,17,17,17}; return r; }
        case 'N': { static const uint8_t r[7] = {17,25,21,19,17,17,17}; return r; }
        case 'O': { static const uint8_t r[7] = {14,17,17,17,17,17,14}; return r; }
        case 'P': { static const uint8_t r[7] = {30,17,17,30,16,16,16}; return r; }
        case 'Q': { static const uint8_t r[7] = {14,17,17,17,21,18,13}; return r; }
        case 'R': { static const uint8_t r[7] = {30,17,17,30,20,18,17}; return r; }
        case 'S': { static const uint8_t r[7] = {15,16,16,14,1,1,30}; return r; }
        case 'T': { static const uint8_t r[7] = {31,4,4,4,4,4,4}; return r; }
        case 'U': { static const uint8_t r[7] = {17,17,17,17,17,17,14}; return r; }
        case 'V': { static const uint8_t r[7] = {17,17,17,17,17,10,4}; return r; }
        case 'W': { static const uint8_t r[7] = {17,17,17,21,21,21,10}; return r; }
        case 'X': { static const uint8_t r[7] = {17,17,10,4,10,17,17}; return r; }
        case 'Y': { static const uint8_t r[7] = {17,17,10,4,4,4,4}; return r; }
        case 'Z': { static const uint8_t r[7] = {31,1,2,4,8,16,31}; return r; }
        case '0': { static const uint8_t r[7] = {14,17,19,21,25,17,14}; return r; }
        case '1': { static const uint8_t r[7] = {4,12,4,4,4,4,14}; return r; }
        case '2': { static const uint8_t r[7] = {14,17,1,2,4,8,31}; return r; }
        case '3': { static const uint8_t r[7] = {30,1,1,14,1,1,30}; return r; }
        case '4': { static const uint8_t r[7] = {2,6,10,18,31,2,2}; return r; }
        case '5': { static const uint8_t r[7] = {31,16,16,30,1,1,30}; return r; }
        case '6': { static const uint8_t r[7] = {6,8,16,30,17,17,14}; return r; }
        case '7': { static const uint8_t r[7] = {31,1,2,4,8,8,8}; return r; }
        case '8': { static const uint8_t r[7] = {14,17,17,14,17,17,14}; return r; }
        case '9': { static const uint8_t r[7] = {14,17,17,15,1,2,12}; return r; }
        case '.': { static const uint8_t r[7] = {0,0,0,0,0,12,12}; return r; }
        case ',': { static const uint8_t r[7] = {0,0,0,0,0,12,8}; return r; }
        case ':': { static const uint8_t r[7] = {0,12,12,0,12,12,0}; return r; }
        case '/': { static const uint8_t r[7] = {1,1,2,4,8,16,16}; return r; }
        case '-': { static const uint8_t r[7] = {0,0,0,31,0,0,0}; return r; }
        case '+': { static const uint8_t r[7] = {0,4,4,31,4,4,0}; return r; }
        case '!': { static const uint8_t r[7] = {4,4,4,4,4,0,4}; return r; }
        case '?': { static const uint8_t r[7] = {14,17,1,2,4,0,4}; return r; }
        case '(' : { static const uint8_t r[7] = {2,4,8,8,8,4,2}; return r; }
        case ')' : { static const uint8_t r[7] = {8,4,2,2,2,4,8}; return r; }
        case '\'' : { static const uint8_t r[7] = {4,4,8,0,0,0,0}; return r; }
        default: return NULL;
    }
}

static void font_write_pixel(uint32_t base, uint32_t fmt, int bpl, int bufW, int bufH, int px, int py, uint8_t val) {
    static const int pxBytes[5] = { 0, 0, 1, 3, 4 };
    if (fmt > 4 || px < 0 || px >= bufW || py < 0 || py >= bufH) return;
    int pb = pxBytes[fmt];
    uint32_t a = base + (uint32_t)(py * bpl) + (uint32_t)(pb == 0 ? px / 2 : px * pb);
    switch (fmt) {
        case 0: case 1: {
            uint8_t old = MEM_R8(a);
            uint8_t pix = val >> 4;
            if ((px & 1) != (int)fmt) MEM_W8(a, (uint8_t)((pix << 4) | (old & 0x0F)));
            else MEM_W8(a, (uint8_t)((old & 0xF0) | pix));
            break;
        }
        case 2: MEM_W8(a, val); break;
        case 3: MEM_W8(a, val); MEM_W8(a + 1, val); MEM_W8(a + 2, val); break;
        case 4: MEM_W32(a, (uint32_t)val | ((uint32_t)val << 8) | ((uint32_t)val << 16) | ((uint32_t)val << 24)); break;
    }
}

static uint32_t h_FontGetCharGlyphImage(CpuState *s) {
    uint32_t cc = A1 & 0xffff, gi = A2;
    if (!gi || cc <= 32) return 0;
    const PGF *p = font_for((int)cc);
    if (p) { pgf_draw_glyph(p, (int)cc, 0, gi); return 0; }
    /* No PGF available: synthetic 5x7 fallback below. */
    uint32_t fmt = MEM_R32(gi + 0);
    int x = (int)MEM_R32(gi + 4) >> 6, y = (int)MEM_R32(gi + 8) >> 6;
    int bufW = (int)(MEM_R16(gi + 12)), bufH = (int)(MEM_R16(gi + 14));
    int bpl = (int)(MEM_R16(gi + 16));
    uint32_t base = MEM_R32(gi + 20);
    if (getenv("SR_FONTLOG")) { static int n = 0; if (n++ < 12)
        fprintf(stderr, "glyph cc=0x%04x fmt=%u xy=(%d,%d) buf=0x%08x %dx%d bpl=%d\n", cc, fmt, x, y, base, bufW, bufH, bpl); }
    const uint8_t *rows = font5x7(cc);
    if (!rows && getenv("SR_FONTLOG")) { static unsigned char seen[65536]; if (!seen[cc]) { seen[cc] = 1;
        fprintf(stderr, "MISSING glyph cc=0x%04x '%c'\n", cc, (cc >= 32 && cc < 127) ? (char)cc : '?'); } }
    for (int yy = 0; yy < 11; yy++) for (int xx = 0; xx < 8; xx++) {
        int gx = xx - 1, gy = yy - 2;
        int on = rows && gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (rows[gy] & (1u << (4 - gx)));
        font_write_pixel(base, fmt, bpl, bufW, bufH, x + xx, y + yy, on ? 0xFF : 0x00);
    }
    return 0;
}
static uint32_t h_FontFindOptimumFont(CpuState *s) {
    if (A2) MEM_W32(A2, 0);            /* *errorCode */
    return 0;                          /* font index 0 */
}
/* ---- sceMpeg (PSMF video): faithful port in src/rt/mpeg.c (from PPSSPP) ----
 * These thin wrappers marshal MIPS args to the ported mpeg_* functions. The port implements the
 * real PSMF analysis, ring-buffer accounting, handle/context creation, stream registration, and
 * AU getters with timestamp progression + end-of-stream, so the movie playback loop runs and
 * completes exactly as on PPSSPP. The SDL3 build decodes AVC video through Windows Media
 * Foundation (h264_mf.c); ATRAC movie audio is still modelled as silence. */
uint32_t mpeg_init(void);
uint32_t mpeg_finish(void);
uint32_t mpeg_query_mem_size(uint32_t outAddr);
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets);
uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size, uint32_t cbAddr, uint32_t cbArg);
uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr, uint32_t frameWidth, uint32_t mode, uint32_t ddrTop);
uint32_t mpeg_delete(uint32_t mpegAddr);
uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr);
uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr);
uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum);
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid);
uint32_t mpeg_ringbuffer_available_size(uint32_t ring);
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available);
uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr);
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init);
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr);
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr);
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf);
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr);
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);

static uint32_t h_MpegInit(CpuState *s) { (void)s; return mpeg_init(); }
static uint32_t h_MpegMallocAvcEsBuf(CpuState *s) { return mpeg_malloc_avc_es_buf(A0); }
static uint32_t h_MpegFreeAvcEsBuf(CpuState *s) { return mpeg_free_avc_es_buf(A0, A1); }
static uint32_t h_MpegInitAu(CpuState *s) { return mpeg_init_au(A0, A1, A2); }
static uint32_t h_MpegQueryAtracEsSize(CpuState *s) { return mpeg_query_atrac_es_size(A0, A1, A2); }
static uint32_t h_MpegFinish(CpuState *s) { (void)s; return mpeg_finish(); }
/* sceMpegQueryMemSize() takes no args and RETURNS the context size in v0 (PPSSPP MpegRequiredMem:
 * 0x10000 for lib version >= 0x0105, which ACX uses). The earlier wrapper wrote it to a pointer and
 * returned 0, so the game allocated a 0-byte mpeg buffer and sceMpegCreate failed with NO_MEMORY. */
static uint32_t h_MpegQueryMemSize(CpuState *s) { (void)s; return 0x10000u; }
static uint32_t h_MpegCreate(CpuState *s) {
    uint32_t r = mpeg_create(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1), stack_arg(s, 2));
    if (getenv("SR_MPEGLOG")) fprintf(stderr, "MpegCreate mpegAddr=0x%x data=0x%x size=0x%x ring=0x%x fw=%u -> 0x%x\n",
        A0, A1, A2, A3, stack_arg(s,0), r);
    return r;
}
static uint32_t h_MpegDelete(CpuState *s) { return mpeg_delete(A0); }
static uint32_t h_MpegRingbufferQueryMemSize(CpuState *s) { return mpeg_ringbuffer_query_mem_size(A0); }
static uint32_t h_MpegRingbufferConstruct(CpuState *s) { return mpeg_ringbuffer_construct(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1)); }
static uint32_t h_MpegRingbufferAvailable(CpuState *s) { return mpeg_ringbuffer_available_size(A0); }
static uint32_t h_MpegRingbufferPut(CpuState *s) { return mpeg_ringbuffer_put(s, A0, A1, A2); }
static uint32_t h_MpegRegistStream(CpuState *s) { return mpeg_regist_stream(A0, A1, A2); }
static uint32_t h_MpegUnRegistStream(CpuState *s) { return mpeg_unregist_stream(A0, A1); }
static uint32_t h_MpegQueryStreamOffset(CpuState *s) { return mpeg_query_stream_offset(A0, A1, A2); }
static uint32_t h_MpegQueryStreamSize(CpuState *s) { return mpeg_query_stream_size(A0, A1); }
/* PPSSPP returns these via hleDelayResult so the playback thread yields (it does not busy-poll the
 * ring). Mirror that: delay the calling thread a frame-ish, longer when there is no data yet, so the
 * feeder/display threads run and the movie paces instead of hanging the scheduler. */
static uint32_t h_MpegGetAvcAu(CpuState *s) {
    uint32_t r = mpeg_get_avc_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
static uint32_t h_MpegGetAtracAu(CpuState *s) {
    uint32_t r = mpeg_get_atrac_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
/* PPSSPP charges real decode latency (sceMpeg.cpp: avcDecodeDelayMs=5400, atracDecodeDelayMs=3000,
 * passed to hleDelayResult in microseconds). Besides pacing, the delay is a guaranteed yield. */
static uint32_t h_MpegAvcDecode(CpuState *s) {
    uint32_t r = mpeg_avc_decode(A0, A1, A2, A3, stack_arg(s, 0));
    sched_delay_current(5400);
    return r;
}
static uint32_t h_MpegAtracDecode(CpuState *s) {
    uint32_t r = mpeg_atrac_decode(A0, A1, A2, A3);
    sched_delay_current(3000);
    return r;
}
static uint32_t h_MpegAvcDecodeStop(CpuState *s) { return mpeg_avc_decode_stop(A0, A1, A2, A3); }

/* sceAtrac3plus: control-flow model (no real ATRAC3 decode -- output is silence). Enough for the
 * audio thread to run an ATRAC clip (e.g. the title BGM) to its loop/end without trapping or
 * spinning. SetDataAndGetID parses the RIFF/"fact" sample count; DecodeData hands back a frame of
 * silence and advances; GetRemainFrame reports PSP_ATRAC_ALLDATA_IS_ON_MEMORY (-1) so the game does
 * not wait for streamed data. ATRAC3plus frame = 2048 samples. Ported semantics from PPSSPP
 * Core/HLE/sceAtrac.cpp (decode bookkeeping), minus the media engine. */
#define ATRAC_SAMPLES_PER_FRAME 2048
typedef struct { int used; uint32_t buf, size; int endSample, posSample, loopNum; } Atrac;
static Atrac s_atrac[8];
static int atrac_riff_samples(uint32_t buf, uint32_t size) {
    /* Scan the RIFF for the "fact" chunk; its first u32 is the total sample count. */
    if (size < 44 || MEM_R32(buf) != 0x46464952u /* 'RIFF' */) return 0;
    uint32_t p = buf + 12, end = buf + (size < 0x100000u ? size : 0x100000u);
    while (p + 8 <= end) {
        uint32_t id = MEM_R32(p), sz = MEM_R32(p + 4);
        if (id == 0x74636166u /* 'fact' */) return (int)MEM_R32(p + 8);
        p += 8 + ((sz + 1u) & ~1u);
    }
    return 0;
}
static uint32_t h_AtracSetDataAndGetID(CpuState *s) {
    int id = -1; for (int i = 0; i < 8; i++) if (!s_atrac[i].used) { id = i; break; }
    if (id < 0) return 0xFFFFFFFFu;
    Atrac *a = &s_atrac[id];
    a->used = 1; a->buf = A0; a->size = A1; a->posSample = 0; a->loopNum = 0;
    a->endSample = atrac_riff_samples(A0, A1);
    if (a->endSample <= 0) a->endSample = ATRAC_SAMPLES_PER_FRAME * 1024;   /* unknown: long clip */
    return (uint32_t)id;
}
static Atrac *atrac_of(uint32_t id) { return id < 8 && s_atrac[id].used ? &s_atrac[id] : 0; }
static uint32_t h_AtracReleaseAtracID(CpuState *s) { Atrac *a = atrac_of(A0); if (a) a->used = 0; return 0; }
static uint32_t h_AtracDecodeData(CpuState *s) {
    /* a0=id, a1=outSamples, a2=*decodedSamples, a3=*finishFlag, sp+16=*remainFrames. */
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;   /* bad ID */
    uint32_t out = A1, decAddr = A2, finAddr = A3, remAddr = stack_arg(s, 0);
    int n = ATRAC_SAMPLES_PER_FRAME;
    if (out) for (int i = 0; i < n * 2; i++) MEM_W16(out + (uint32_t)i * 2, 0);  /* stereo silence */
    a->posSample += n;
    int finished = 0;
    if (a->posSample >= a->endSample) {
        if (a->loopNum == 0) finished = 1;
        else { a->posSample = 0; if (a->loopNum > 0) a->loopNum--; }
    }
    if (decAddr) MEM_W32(decAddr, (uint32_t)n);
    if (finAddr) MEM_W32(finAddr, (uint32_t)finished);
    if (remAddr) MEM_W32(remAddr, 0xFFFFFFFFu);            /* ALLDATA_IS_ON_MEMORY */
    return 0;
}
static uint32_t h_AtracGetRemainFrame(CpuState *s) { if (A1) MEM_W32(A1, 0xFFFFFFFFu); return 0; }
static uint32_t h_AtracGetStreamDataInfo(CpuState *s) {
    /* a1=*writePointer, a2=*writableBytes, a3=*readOffset. No streaming needed: nothing writable. */
    if (A1) MEM_W32(A1, atrac_of(A0) ? s_atrac[A0].buf : 0);
    if (A2) MEM_W32(A2, 0); if (A3) MEM_W32(A3, 0);
    return 0;
}
static uint32_t h_AtracAddStreamData(CpuState *s) { (void)s; return 0; }
static uint32_t h_AtracGetNextDecodePosition(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;
    if (a->posSample >= a->endSample) return 0x80630022u;   /* ALLDATA_DECODED */
    if (A1) MEM_W32(A1, (uint32_t)a->posSample);
    return 0;
}
static uint32_t h_AtracGetSoundSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;
    if (A1) MEM_W32(A1, (uint32_t)a->endSample);          /* end sample */
    if (A2) MEM_W32(A2, 0xFFFFFFFFu);                     /* loop start (-1 = none) */
    if (A3) MEM_W32(A3, 0xFFFFFFFFu);                     /* loop end */
    return 0;
}
static uint32_t h_AtracGetLoopStatus(CpuState *s) {
    if (A1) MEM_W32(A1, atrac_of(A0) ? (uint32_t)s_atrac[A0].loopNum : 0);
    if (A2) MEM_W32(A2, 0);
    return 0;
}
static uint32_t h_AtracSetLoopNum(CpuState *s) { Atrac *a = atrac_of(A0); if (a) a->loopNum = (int)A1; return 0; }
static uint32_t h_AtracResetPlayPosition(CpuState *s) { Atrac *a = atrac_of(A0); if (a) a->posSample = (int)A1; return 0; }

/* sceUtility dialogs (savedata/msg/osk). Faithful to PPSSPP's PSPDialog status machine
 * (Core/Dialog/PSPDialog.cpp): status enum NONE=0, INITIALIZE=1, RUNNING=2, FINISHED=3,
 * SHUTDOWN=4. InitStart -> INITIALIZE; GetStatus returns the current status and then auto-advances
 * INITIALIZE->RUNNING and SHUTDOWN->NONE; the (real-hardware) utility thread completes the
 * autoload, modelled here by RUNNING->FINISHED after a few polls; ShutdownStart -> SHUTDOWN. The
 * earlier guess jumped straight to RUNNING and to NONE, skipping INITIALIZE(1) and SHUTDOWN(4),
 * which a game that waits to observe those states would hang on. result is the common-header
 * field at param+0x1c. */
static void guest_cstr(uint32_t addr, char *out, int max);

/* sceUtilitySavedata: real persistence on a virtual memory stick (src/rt/savedata.c). */
uint32_t sr_savedata_execute(uint32_t param);

static int s_dlg_status = 0, s_dlg_tick = 0; static uint32_t s_dlg_param = 0, s_dlg_result = 0;
static int s_osk_current_clear(void);   /* fwd: savedata/msg dialogs take the slot from the OSK */
static uint32_t h_SavedataInitStart(CpuState *s) {
    s_dlg_param = A0; s_dlg_status = 1; s_dlg_tick = 0;   /* INITIALIZE */
    s_osk_current_clear();
    s_dlg_result = sr_savedata_execute(A0);               /* fill result structs now (PPSSPP IO action) */
    if (getenv("SR_DLGLOG")) { static int n=0; fprintf(stderr, "** SavedataInitStart #%d **\n", ++n);
        uint32_t p = A0;
        char gn[16], sn[24]; guest_cstr(p+0x3c, gn, sizeof(gn)); guest_cstr(p+0x4c, sn, sizeof(sn));
        fprintf(stderr, "SavedataInitStart param=0x%08x size=%u mode=%d gameName='%s' saveName='%s' result=0x%08x\n",
            p, MEM_R32(p+0), (int)MEM_R32(p+0x30), gn, sn, s_dlg_result);
    }
    return 0;
}
static uint32_t h_MsgDialogInitStart(CpuState *s) {
    s_dlg_param = A0; s_dlg_status = 1; s_dlg_tick = 0; s_dlg_result = 0;
    s_osk_current_clear();
    return 0;
}
static uint32_t h_DlgGetStatus(CpuState *s) {
    (void)s;
    int ret = s_dlg_status;
    if (s_dlg_status == 1) {                              /* INITIALIZE -> RUNNING */
        s_dlg_status = 2; s_dlg_tick = 0;
    } else if (s_dlg_status == 2) {                       /* utility-thread completes the op */
        if (++s_dlg_tick > 3) { s_dlg_status = 3; if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result); }
    } else if (s_dlg_status == 4) {                       /* SHUTDOWN -> NONE */
        s_dlg_status = 0;
    }
    if (getenv("SR_DLGLOG")) {                            /* log transitions only (unbounded) */
        static int last = -1;
        if (s_dlg_status != last) {
            fprintf(stderr, "DlgGetStatus: ret=%d -> status=%d (result=0x%08x)\n", ret, s_dlg_status, s_dlg_result);
            last = s_dlg_status;
        }
    }
    return (uint32_t)ret;
}
static uint32_t h_SavedataUpdate(CpuState *s) {
    (void)s;
    if (s_dlg_status == 2) {
        s_dlg_status = 3;
        if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result);
        if (getenv("SR_DLGLOG")) fprintf(stderr, "SavedataUpdate: FINISHED result=0x%08x written to 0x%08x\n",
                                         s_dlg_result, s_dlg_param + 0x1c);
    }
    return 0;
}
static uint32_t h_DlgShutdown(CpuState *s) {
    (void)s;
    s_dlg_status = 4;
    if (getenv("SR_DLGLOG")) fprintf(stderr, "DlgShutdownStart\n");
    return 0;
}

/* ---- sceUtilityOsk: the on-screen keyboard, backed by a native input box (osk_win.c).
 * Same PSPDialog status machine as the other utilities. When the dialog reaches RUNNING the
 * native modal input box collects the text (the game is parked polling OskGetStatus, exactly
 * as it would be while the real OSK overlay is up), then the result is written back into each
 * SceUtilityOskData field as UTF-16 and the status advances to FINISHED. */
int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap);
static int s_osk_status = 0;
static uint32_t s_osk_param = 0;
/* PPSSPP keeps a "current dialog type": OskGetStatus is WRONG_TYPE only while a DIFFERENT
 * utility dialog owns the slot. After an OSK shuts down it stays the current dialog and
 * GetStatus returns NONE(0) — a game spinning "while (OskGetStatus() != 0)" after name entry
 * hangs forever if we keep returning WRONG_TYPE there. */
static int s_osk_current = 0;
static int s_osk_current_clear(void) { s_osk_current = 0; return 0; }

static void osk_read_utf16(uint32_t addr, wchar_t *out, int max) {
    int i = 0;
    if (addr) for (; i < max - 1; i++) {
        uint16_t c = MEM_R16(addr + (uint32_t)i * 2);
        if (!c) break;
        out[i] = (wchar_t)c;
    }
    out[i] = 0;
}

static void osk_run(void) {
    uint32_t p = s_osk_param;
    if (!p) return;
    int nf = (int)MEM_R32(p + 0x30);                       /* fieldCount */
    uint32_t fields = MEM_R32(p + 0x34);                   /* SceUtilityOskData[] */
    if (nf < 1 || nf > 8 || !fields) return;
    for (int i = 0; i < nf; i++) {
        uint32_t f = fields + (uint32_t)i * 0x34;
        uint32_t descA = MEM_R32(f + 0x1c), inA = MEM_R32(f + 0x20);
        uint32_t outLen = MEM_R32(f + 0x24), outA = MEM_R32(f + 0x28);
        uint32_t outLimit = MEM_R32(f + 0x30);
        wchar_t desc[128], intext[256], out[256];
        osk_read_utf16(descA, desc, 128);
        osk_read_utf16(inA, intext, 256);
        int cap = outLen ? (int)outLen : 256;              /* u16 units incl. terminator */
        if (outLimit && (int)outLimit + 1 < cap) cap = (int)outLimit + 1;
        if (cap > 256) cap = 256;
        wcscpy(out, intext);
        int ok = sr_osk_input(desc, intext, out, cap);
        if (outA) {
            const wchar_t *w = ok ? out : intext;
            int j = 0;
            for (; w[j] && j < cap - 1; j++) MEM_W16(outA + (uint32_t)j * 2, (uint16_t)w[j]);
            MEM_W16(outA + (uint32_t)j * 2, 0);
        }
        MEM_W32(f + 0x2c, ok ? 2u : 1u);                   /* result: CHANGED / CANCELLED */
        if (getenv("SR_DLGLOG"))
            fprintf(stderr, "osk: field %d desc='%ls' in='%ls' -> %s '%ls'\n",
                    i, desc, intext, ok ? "ok" : "cancel", ok ? out : intext);
    }
    MEM_W32(p + 0x1c, 0);                                  /* common result */
}

static uint32_t h_OskInitStart(CpuState *s) {
    s_osk_param = A0;
    s_osk_status = 1;                                      /* INITIALIZE */
    s_osk_current = 1;                                     /* OSK owns the dialog slot */
    return 0;
}
/* sceUtilityOskGetStatus: with no OSK ever started (e.g. polled every boot frame while a
 * savedata dialog is up) PPSSPP returns SCE_ERROR_UTILITY_WRONG_TYPE (0x80110005). Once an
 * OSK ran, NONE(0) is a real status games wait for after shutdown. */
static uint32_t h_OskGetStatus(CpuState *s) {
    (void)s;
    if (!s_osk_current) return 0x80110005u;
    int ret = s_osk_status;
    if (s_osk_status == 1) s_osk_status = 2;               /* INITIALIZE -> RUNNING */
    else if (s_osk_status == 2) { osk_run(); s_osk_status = 3; }   /* RUNNING -> FINISHED */
    else if (s_osk_status == 4) { s_osk_status = 0; s_osk_param = 0; }
    return (uint32_t)ret;
}
static uint32_t h_OskUpdate(CpuState *s) { (void)s; return 0; }
static uint32_t h_OskShutdown(CpuState *s) { (void)s; if (s_osk_status) s_osk_status = 4; return 0; }

/* sceWlanGetEtherAddr: 6-byte MAC out through a0. Fixed value so save/profile stamps stay
 * stable across runs. */
static uint32_t h_WlanGetEtherAddr(CpuState *s) {
    static const uint8_t mac[6] = { 0x00, 0x13, 0x37, 0xAC, 0xC5, 0x10 };
    if (!A0) return 0x80000103u;              /* SCE_KERNEL_ERROR_ILLEGAL_ADDR */
    for (int i = 0; i < 6; i++) MEM_W8(A0 + (uint32_t)i, mac[i]);
    return 0;
}
static uint32_t h_WlanOn(CpuState *s) { (void)s; return 1; }   /* powered on / switch up */

static uint32_t h_VolatileMemLock(CpuState *s) {
    /* sceKernelVolatileMemLock(type, void **paddr, int *psize): hand the app the 4MB volatile
     * partition. PPSSPP returns base 0x08400000, size 0x00400000; the game uses it as the
     * destination scratch buffer for decompressing/copying loaded assets, so the out-params
     * must be filled or the copy targets NULL. */
    if (A1) MEM_W32(A1, 0x08400000u);
    if (A2) MEM_W32(A2, 0x00400000u);
    return 0;
}
static uint32_t h_UmdDriveStat(CpuState *s) { (void)s; return 0x32; }     /* PRESENT|READY|READABLE (matches PPSSPP reference) */

/* VBLANK sub-interrupt handler the game registers; delivered once per frame by the scheduler
 * (it typically wakes the sleeping game thread). PSP_VBLANK_INT = 30. */
static uint32_t g_vbl_handler = 0, g_vbl_arg = 0; static int g_vbl_on = 0;
static uint32_t h_RegisterSubIntr(CpuState *s) {
    /* a0=intno, a1=no, a2=handler, a3=arg. */
    if (A0 == 30) { g_vbl_handler = A2; g_vbl_arg = A3; }
    return 0;
}
static uint32_t h_EnableSubIntr(CpuState *s) { if (A0 == 30) g_vbl_on = 1; return 0; }
uint32_t sr_vblank_handler(void) { return g_vbl_on ? g_vbl_handler : 0; }
uint32_t sr_vblank_arg(void) { return g_vbl_arg; }

static uint32_t s_vcount_fwd;  /* mirror of s_vcount for clock/input timing (set in sr_vblank_tick) */
/* Virtual microsecond clock. It is anchored to the frame (VBLANK) counter so it advances at the
 * real PSP rate of one frame per 1/60 s (16667 us). A small per-call increment keeps it strictly
 * monotonic within a frame. Earlier this advanced only +100us per call, so time barely moved while
 * the game ran hundreds of frames -- any "wait N seconds of system time" (logo/loading screens)
 * never elapsed. s_vcount_fwd mirrors the frame counter (set in sr_vblank_tick). */
static uint64_t g_time_usec = 0;
static uint64_t now_usec(void) {
    uint64_t base = (uint64_t)s_vcount_fwd * 16667ull;
    if (base > g_time_usec) g_time_usec = base;
    else g_time_usec += 1;
    return g_time_usec;
}
static uint32_t h_GetSystemTimeLow(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
static uint32_t h_GetSystemTimeWide(CpuState *s) { uint64_t t = now_usec(); s->r[3] = (uint32_t)(t >> 32); return (uint32_t)t; }
static uint32_t h_LibcTime(CpuState *s) {
    uint32_t t = (uint32_t)(now_usec() / 1000000) + 0x40000000u;   /* seconds since epoch-ish */
    if (A0) MEM_W32(A0, t);
    return t;
}
static uint32_t h_LibcClock(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
static uint32_t h_RtcGetCurrentTick(CpuState *s) { uint64_t t = now_usec(); if (A0) { MEM_W32(A0, (uint32_t)t); MEM_W32(A0 + 4, (uint32_t)(t >> 32)); } return 0; }
/* sceRtcGetWin32FileTime(pspTime *in, u64 *out): out gets 100ns FILETIME units; only deltas
 * matter to the game, so the virtual clock scaled by 10 suffices. The input struct is const. */
static uint32_t h_RtcGetWin32FileTime(CpuState *s) {
    uint64_t ft = now_usec() * 10u;
    if (A1) { MEM_W32(A1, (uint32_t)ft); MEM_W32(A1 + 4, (uint32_t)(ft >> 32)); }
    return 0;
}
static uint32_t h_RtcGetTickResolution(CpuState *s) { (void)s; return 1000000; }
/* Fill a struct timeval {sec, usec} from the virtual clock (the old h_ok left the output
 * struct unwritten -- garbage timestamps). */
static uint32_t h_LibcGettimeofday(CpuState *s) {
    uint64_t t = now_usec();
    if (A0) { MEM_W32(A0, (uint32_t)(t / 1000000u) + 0x40000000u); MEM_W32(A0 + 4, (uint32_t)(t % 1000000u)); }
    if (A1) { MEM_W32(A1, 0); MEM_W32(A1 + 4, 0); }   /* timezone: UTC */
    return 0;
}
/* Fill a pspTime {u16 year,month,day,hour,min,sec; u32 usec} from the virtual clock. */
static uint32_t h_RtcGetCurrentClock(CpuState *s) {
    uint32_t out = A0;
    uint64_t t = now_usec();
    if (out) {
        uint64_t secs = t / 1000000u;
        MEM_W16(out + 0, 2007);                          /* year (any sane value) */
        MEM_W16(out + 2, 1 + (uint16_t)((secs / 2592000u) % 12));
        MEM_W16(out + 4, 1 + (uint16_t)((secs / 86400u) % 28));
        MEM_W16(out + 6, (uint16_t)((secs / 3600u) % 24));
        MEM_W16(out + 8, (uint16_t)((secs / 60u) % 60));
        MEM_W16(out + 10, (uint16_t)(secs % 60));
        MEM_W32(out + 12, (uint32_t)(t % 1000000u));
    }
    return 0;
}
static uint32_t h_StdFd(CpuState *s) { (void)s; return 1; }   /* a std file descriptor */
static uint32_t h_F919F628(CpuState *s) { (void)s; return 0x001a0b00; }   /* matches PPSSPP reference */

/* ---- IoFileMgrForUser: file IO from the game ISO (src/rt/iso.c) ---- */

static void guest_cstr(uint32_t addr, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { uint8_t c = MEM_R8(addr + (uint32_t)i); if (!c) break; out[i] = (char)c; }
    out[i] = 0;
}

/* sceKernelLoadModule(path, flags, opt): the module's code is NOT loaded or run (static
 * recompilation covers only the main EBOOT). Log the path loudly so any feature that
 * depends on a runtime-loaded module (e.g. a cutscene player) is identifiable. */
static uint32_t h_LoadModule(CpuState *s) {
    char path[256]; guest_cstr(A0, path, sizeof(path));
    fprintf(stderr, "sceKernelLoadModule(\"%s\") -> fake uid (module code not loaded)\n", path);
    return sr_alloc_uid();
}

/* sceKernelPrintf: surface the game's own debug output (format string only; the engine's
 * messages are mostly plain text and any %-args print as-is, which is still informative). */
static uint32_t h_KernelPrintf(CpuState *s) {
    char msg[512]; guest_cstr(A0, msg, sizeof(msg));
    fprintf(stderr, "GAMELOG %s%s", msg, (msg[0] && msg[strlen(msg)-1] == '\n') ? "" : "\n");
    return 0;
}

typedef struct { int used; uint32_t lba, size, off; int64_t async_res; FILE *host; } Fd;
static Fd s_fds[64];

/* Map a guest path to a host scratch file under build/acx/fs (writable storage that the read-only
 * ISO cannot provide -- ms0: saves, the game's registry file, etc.). The path is flattened so
 * subdirectories collapse into one filename. */
static void host_path(const char *guest, char *out, int max) {
    int n = snprintf(out, (size_t)max, "build/acx/fs/");
    for (int i = 0; guest[i] && n < max - 1; i++) {
        char c = guest[i];
        out[n++] = (char)((c=='/'||c==':'||c=='\\'||c==' ') ? '_' : c);
    }
    out[n] = 0;
}

/* Optional debug override for the in-game text language. The game reads the global system language
 * via sceUtilityGetSystemParamInt (PSP_SYSTEMPARAM_ID_INT_LANGUAGE), which we serve as English (see
 * h_GetSystemParamInt), so by default nothing is forced here. SR_LANG=<n> pokes the resolved value
 * into the game's config object (unk_27CAF4+328) for testing (0=JP,1=EN,2=FR,3=ES,4=DE,5=IT...). */
static void sr_force_language(void) {
    const char *lg = getenv("SR_LANG");
    if (lg) MEM_W8(0x08A80C3C, (uint8_t)atoi(lg));
}

static uint32_t h_IoOpen(CpuState *s) {
    /* a0=path, a1=flags, a2=mode. Returns an fd (>=0) or a negative error.
     * PSP flags: WRONLY=2, RDWR=3, APPEND=0x100, CREAT=0x200, TRUNC=0x400. */
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    uint32_t flags = A1;
    sr_force_language();
    if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) flags=0x%x\n", path, flags);
    if (getenv("SR_PATHHEX")) {
        int bad = 0; for (int i = 0; path[i]; i++) if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] >= 0x7f) bad = 1;
        if (bad || path[0] == 0) {
            fprintf(stderr, "Open BAD path ptr=0x%08x bytes:", A0);
            for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", MEM_R8(A0 + (uint32_t)i));
            fprintf(stderr, "\n");
        }
    }
    int slot = -1;
    for (int i = 1; i < 64; i++) if (!s_fds[i].used) { slot = i; break; }
    if (slot < 0) return 0x80010018;  /* too many open files */

    int writing = (flags & 0x0002) != 0;        /* WRONLY or RDWR */
    int creating = (flags & 0x0200) != 0;
    uint32_t lba, size;
    int in_iso = (iso_lookup(path, &lba, &size) == 0);

    if (writing || creating || !in_iso) {
        /* Host-backed file (writable storage). */
        char hp[512]; host_path(path, hp, sizeof(hp));
        const char *mode;
        if (flags & 0x0400) mode = "w+b";            /* TRUNC */
        else if (flags & 0x0100) mode = "a+b";       /* APPEND */
        else if (writing || creating) mode = "r+b";  /* update; fall back to create below */
        else mode = "rb";                            /* read-only host file (e.g. a prior save) */
        FILE *fp = fopen(hp, mode);
        if (!fp && (writing || creating)) fp = fopen(hp, "w+b");  /* create if missing */
        if (!fp) {
            if (in_iso) goto from_iso;               /* read-only and only on disc: serve the ISO */
            fprintf(stderr, "sceIoOpen: not found: %s\n", path);
            return 0x80010002;
        }
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        s_fds[slot].used = 1; s_fds[slot].host = fp; s_fds[slot].lba = 0;
        s_fds[slot].size = (uint32_t)(sz < 0 ? 0 : sz); s_fds[slot].off = 0;
        return (uint32_t)slot;
    }
from_iso:
    s_fds[slot].used = 1; s_fds[slot].host = NULL;
    s_fds[slot].lba = lba; s_fds[slot].size = size; s_fds[slot].off = 0;
    return (uint32_t)slot;
}
static uint32_t h_IoWrite(CpuState *s) {
    /* a0=fd, a1=src, a2=count. Returns bytes written. */
    uint32_t fd = A0, src = A1, count = A2;
    if (fd < 2) return count;                        /* stdout/stderr: pretend-consumed */
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    if (!f->host) return 0x80010013;                 /* read-only (ISO) fd: not writable */
    fseek(f->host, (long)f->off, SEEK_SET);
    uint8_t tmp[4096]; uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        for (uint32_t k = 0; k < n; k++) tmp[k] = (uint8_t)MEM_R8(src + done + k);
        done += (uint32_t)fwrite(tmp, 1, n, f->host);
        if (done < count) break;
    }
    fflush(f->host);
    f->off += done; if (f->off > f->size) f->size = f->off;
    return done;
}
static uint32_t h_IoRead(CpuState *s) {
    /* a0=fd, a1=dst, a2=count. Returns bytes read. */
    uint32_t fd = A0, dst = A1, count = A2;
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    if (f->off + count > f->size) count = f->size - f->off;
    /* Read in chunks straight into guest memory, from the host file or the ISO. */
    uint8_t tmp[4096];
    uint32_t done = 0;
    if (f->host) fseek(f->host, (long)f->off, SEEK_SET);
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
        else iso_read(f->lba, f->off + done, tmp, n);
        for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, tmp[k]);
        done += n;
        if (n == 0) break;
    }
    f->off += done;
    if (getenv("SR_IOLOG")) {
        static int n = 0;
        if (n++ < 4000)
            fprintf(stderr, "Read fd=%u off=%u size=%u -> %u\n", fd, f->off - done, count, done);
    }
    return done;
}
static uint32_t h_IoLseek32(CpuState *s) {
    /* a0=fd, a1=offset, a2=whence. Returns new position (32-bit). */
    uint32_t fd = A0; int32_t off = (int32_t)A1; uint32_t whence = A2;
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
    int64_t np = base + off; if (np < 0) np = 0; if (np > f->size) np = f->size;
    f->off = (uint32_t)np;
    return f->off;
}
static uint32_t h_IoLseek(CpuState *s) {
    /* a0=fd, [a2:a3]=64-bit offset, [sp+16]=whence. Returns 64-bit pos in v0:v1. */
    uint32_t fd = A0;
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    uint32_t whence = stack_arg(s, 0);
    if (fd >= 64 || !s_fds[fd].used) { s->r[3] = 0xFFFFFFFF; return 0x80010009; }
    Fd *f = &s_fds[fd];
    int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
    int64_t np = base + off; if (np < 0) np = 0; if (np > f->size) np = f->size;
    f->off = (uint32_t)np;
    s->r[3] = (uint32_t)((uint64_t)np >> 32);
    return (uint32_t)np;
}
static uint32_t h_IoClose(CpuState *s) {
    uint32_t fd = A0;
    if (fd < 64 && s_fds[fd].used) { if (s_fds[fd].host) fclose(s_fds[fd].host); s_fds[fd].host = NULL; s_fds[fd].used = 0; }
    return 0;
}
/* Async IO: the operation completes synchronously and its result is stashed per-fd for the
 * matching sceIoWaitAsync/PollAsync to return (the game streams data this way). */
static uint32_t h_IoOpenAsync(CpuState *s) {
    uint32_t fd = h_IoOpen(s);
    if (fd < 64) s_fds[fd].async_res = (int64_t)(int32_t)fd;   /* open result */
    return fd;
}
static uint32_t h_IoReadAsync(CpuState *s) {
    uint32_t fd = A0;
    uint32_t off = (fd < 64) ? s_fds[fd].off : 0;
    uint32_t n = h_IoRead(s);
    if (getenv("SR_IOLOG")) fprintf(stderr, "ReadAsync fd=%u dst=0x%x size=%u (file off was %u) -> %u; first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
        fd, A1, A2, off, n, MEM_R8(A1), MEM_R8(A1+1), MEM_R8(A1+2), MEM_R8(A1+3), MEM_R8(A1+4), MEM_R8(A1+5), MEM_R8(A1+6), MEM_R8(A1+7));
    if (fd < 64 && s_fds[fd].used) s_fds[fd].async_res = (int64_t)(uint64_t)n;
    return 0;
}
static uint32_t h_IoLseekAsync(CpuState *s) {
    uint32_t fd = A0;
    uint32_t pos = h_IoLseek32(s);
    if (fd < 64 && s_fds[fd].used) s_fds[fd].async_res = (int64_t)(uint64_t)pos;
    return 0;
}
/* Result of the most recent async close per fd slot, so the customary
 * sceIoCloseAsync -> sceIoWaitAsync sequence reads 0 (success), not -1, after the slot is freed. */
static int64_t s_closed_res[64];
static uint32_t h_IoWaitAsync(CpuState *s) {
    uint32_t fd = A0, resp = A1;
    int64_t r = -1;
    if (fd < 64) r = s_fds[fd].used ? s_fds[fd].async_res : s_closed_res[fd];
    if (resp) { MEM_W32(resp, (uint32_t)r); MEM_W32(resp + 4, (uint32_t)((uint64_t)r >> 32)); }
    return 0;   /* completed */
}
static uint32_t h_IoCloseAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd < 64) {
        if (s_fds[fd].host) { fclose(s_fds[fd].host); s_fds[fd].host = NULL; }
        s_fds[fd].async_res = 0;
        s_fds[fd].used = 0;
        s_closed_res[fd] = 0;
    }
    return 0;
}
static uint32_t h_IoGetstat(CpuState *s) {
    /* a0=path, a1=SceIoStat*. SceIoStat layout: mode(+0), attr(+4), size(+8,64-bit),
     * ctime(+0x10), atime(+0x20), mtime(+0x30), st_private[6](+0x40). For UMD files PPSSPP
     * fills st_private[0] (+0x40) with the file's starting LBN; the game reads that to build
     * a raw "sce_lbn0x<LBN>" path. Omitting it made the game read garbage and fetch the wrong
     * sector (e.g. REGFILE.CDI at LBN 0x5f20 was read as 0x80). */
    char path[256]; guest_cstr(A0, path, sizeof(path));
    uint32_t lba, size, st = A1;
    if (iso_lookup(path, &lba, &size) != 0) {
        if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> NOT FOUND\n", path);
        return 0x80010002;
    }
    if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> lba=0x%x size=0x%x\n", path, lba, size);
    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
    MEM_W32(st + 0, 0x2000 | 0x0124);          /* mode: regular file, r-x */
    MEM_W32(st + 4, 0x0004 | 0x0001);          /* attr: file */
    MEM_W32(st + 8, size);                      /* size low (SceOff is 64-bit at +8) */
    MEM_W32(st + 0x40, lba);                    /* st_private[0]: UMD start sector (LBN) */
    return 0;
}

/* ---- audio / control / display / GE / SAS: functional stubs ----
 * These return success and neutral data so the boot reaches and runs its main loop without an
 * actual audio device or GPU. Real rendering (sceGe display lists) and audio are later work;
 * here the calls must not block forever and must hand back valid-shaped results. */

/* sceAudio: a small channel table. The *Blocking output calls must block until the buffer is
 * consumed by the (virtual) audio hardware; that block is what lets the rest of the game run,
 * so a no-op return makes the audio thread monopolise the CPU. Here it yields the thread for
 * the buffer's duration AND forwards the samples to the host waveOut backend (audio.c). */
extern void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR);
static int s_audio_ch[8], s_audio_fmt[8];   /* fmt: 0=stereo, 1=mono */
static uint32_t s_audio_len[8];
static uint32_t h_AudioChReserve(CpuState *s) {
    int ch = (int)A0;
    if (ch < 0) { for (int i = 0; i < 8; i++) if (!s_audio_ch[i]) { ch = i; break; } }
    if (ch < 0 || ch >= 8) return 0xFFFFFFFF;
    s_audio_ch[ch] = 1; s_audio_len[ch] = A1; s_audio_fmt[ch] = (int)A2 & 1;
    return (uint32_t)ch;
}
static uint32_t h_AudioChRelease(CpuState *s) { if (A0 < 8) s_audio_ch[A0] = 0; return 0; }
static uint32_t h_AudioSetChannelDataLen(CpuState *s) {
    if (A0 < 8 && A1 > 0 && A1 <= 65536) s_audio_len[A0] = A1;
    return 0;
}
/* Read a guest sample buffer, expand mono to stereo, hand to the backend, then block until
 * the host queue is back down to ~one buffer of lead (real sceAudio blocking semantics).
 * Pacing against the queue self-corrects: a late thread returns immediately and catches up,
 * an early one sleeps the difference. The old open-loop sleep of the buffer's duration ran
 * slightly slower than the device every iteration (sleep + decode time), so streams drifted
 * behind (voice lagging subtitles) and underran (crackle). */
extern int sr_audio_queued(int ch);
static uint32_t audio_output(CpuState *s, uint32_t ch, uint32_t buf, int voll, int volr) {
    uint32_t n = ch < 8 ? s_audio_len[ch] : 1024;
    if (buf && n > 0 && n <= 65536) {
        static int16_t lr[65536 * 2];
        int mono = ch < 8 ? s_audio_fmt[ch] : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (mono) { int16_t v = (int16_t)MEM_R16(buf + i * 2); lr[i*2] = v; lr[i*2+1] = v; }
            else { lr[i*2] = (int16_t)MEM_R16(buf + i * 4); lr[i*2+1] = (int16_t)MEM_R16(buf + i * 4 + 2); }
        }
        sr_audio_push((int)ch, lr, (int)n, voll, volr);
    }
    int q = sr_audio_queued((int)ch);
    if (q < 0 || n == 0) {                 /* no host audio: open-loop pacing as before */
        sched_delay_current(n ? (n * 1000000u / 44100u) : 1000u);
        return n;
    }
    while ((q = sr_audio_queued((int)ch)) > (int)n)
        sched_delay_current((uint32_t)(q - (int)n) * 1000000u / 44100u);
    return n;
}
static uint32_t h_AudioOutputBlocking(CpuState *s) {
    /* sceAudioOutputBlocking(ch, vol, buf) */
    return audio_output(s, A0, A2, (int)(A1 & 0xFFFF), (int)(A1 & 0xFFFF));
}
static uint32_t h_AudioOutputPannedBlocking(CpuState *s) {
    /* sceAudioOutputPannedBlocking(ch, leftvol, rightvol, buf) */
    return audio_output(s, A0, A3, (int)(A1 & 0xFFFF), (int)(A2 & 0xFFFF));
}
static uint32_t h_AudioRestLen(CpuState *s) { (void)s; return 0; }         /* never backed up */

/* SR_CALLCOUNT instrumentation: per-NID call tallies, dumped at the capture point. */
static struct { uint32_t nid; const char *nm; unsigned long n; } g_cc[512];
static int g_ncc = 0, g_callcount = 0;
static void sr_dump_calls(void) {
    if (!g_callcount) return;
    for (int a = 0; a < g_ncc; a++) for (int b = a + 1; b < g_ncc; b++)
        if (g_cc[b].n > g_cc[a].n) { __typeof__(g_cc[0]) t = g_cc[a]; g_cc[a] = g_cc[b]; g_cc[b] = t; }
    fprintf(stderr, "--- top HLE calls ---\n");
    for (int a = 0; a < g_ncc && a < 18; a++) fprintf(stderr, "  %-32s 0x%08x  %lu\n", g_cc[a].nm, g_cc[a].nid, g_cc[a].n);
}
/* PSP controller ring buffer, modelled on PPSSPP (Core/HLE/sceCtrl.cpp): one sample is latched
 * per VBLANK into a 64-entry ring; sceCtrlReadBufferPositive returns the samples accumulated
 * since the last read (real per-frame history), not a flat copy of the current state. Games do
 * edge detection across these samples, so a flat fill makes a button look permanently held and a
 * press is never seen -- which is why the title/attract loop never advanced on START. */
#define CTRL_RING 64
typedef struct { uint32_t btn; uint8_t lx, ly; } CtrlSample;
static CtrlSample s_ctrl_ring[CTRL_RING] = { [0 ... CTRL_RING-1] = { 0, 128, 128 } };
static int s_ctrl_w = 1, s_ctrl_r = 0;   /* start with one sample available */
#define CTRL_WAIT_OBJ 0xC471D000u

/* sceCtrl: sticks centred. To drive past the skippable intro movie and confirmation prompts
 * without a human, pulse START/CROSS/CIRCLE for a few frames on a periodic cadence (edge presses,
 * so the game sees press+release). Disable with SR_NOINPUT for a truly neutral pad. */
static uint32_t h_CtrlButtons(void) {
    /* Live keyboard (windowed mode) is OR'd with the auto-input pulse below -- in this headless
     * window environment no key is ever pressed, so without the pulse the intro movie never gets
     * its START and loops forever. SR_NOINPUT disables the pulse for a neutral pad. */
    uint32_t keys = gui_on() ? gui_buttons() : 0;
    /* SR_PADSCRIPT=<file>: lines of "frame hexmask width" -- press mask at frame for width
     * frames. Lets an unattended run navigate menus deterministically; replaces the default
     * START pulse entirely when present. */
    {
        static int scr_n = -1;
        static struct { uint32_t f, mask; uint32_t w; } scr[256];
        if (scr_n < 0) {
            scr_n = 0;
            const char *sp = getenv("SR_PADSCRIPT");
            if (sp) {
                FILE *fp = fopen(sp, "r");
                if (fp) {
                    while (scr_n < 256 &&
                           fscanf(fp, "%u %x %u", &scr[scr_n].f, &scr[scr_n].mask, &scr[scr_n].w) == 3)
                        scr_n++;
                    fclose(fp);
                }
            }
        }
        if (scr_n > 0) {
            for (int i = 0; i < scr_n; i++)
                if (s_vcount_fwd >= scr[i].f && s_vcount_fwd < scr[i].f + scr[i].w) keys |= scr[i].mask;
            return keys;
        }
    }
    /* The auto-START pulse below only exists to advance the intro/attract in headless or no-input
     * runs. When a real controller is connected the player drives input themselves, so suppress the
     * pulse (otherwise a phantom START every few seconds would keep opening the pause menu). */
    if (getenv("SR_NOINPUT") || gui_pad_present()) return keys;
    /* Pulse START only, briefly, on a slow cadence to skip the (minutes-long) intro movie and the
     * "press start" prompt. Pressing CROSS/CIRCLE as well drove the menus into bad states (it
     * confirmed things the game was not ready for); START alone advances the intro without that.
     * SR_PAD=<hex> overrides the mask; SR_PADPERIOD/SR_PADWIDTH tune the cadence. */
    const char *m = getenv("SR_PAD");
    uint32_t mask = m ? (uint32_t)strtoul(m, NULL, 16) : 0x0008u;   /* START */
    const char *pe = getenv("SR_PADPERIOD"); int period = pe ? atoi(pe) : 240;
    const char *pw = getenv("SR_PADWIDTH");  int width  = pw ? atoi(pw) : 4;
    const char *ps = getenv("SR_PADSTART");  int startf = ps ? atoi(ps) : 0;  /* hold input until frame */
    if (period <= 0) period = 240;
    if ((int)s_vcount_fwd < startf) return keys;
    if ((int)(s_vcount_fwd % (uint32_t)period) < width) return keys | mask;
    return keys;
}
/* sceCtrlReadBuffer*: fill SceCtrlData[count]. Positive reports pressed buttons as set bits;
 * Negative reports them inverted (set = not pressed), so it must write ~buttons -- writing the
 * positive mask there makes the game see almost every button held and run wild (it jumped through
 * an uninitialised menu handler). */
/* Latch one controller sample per frame into the ring (called from sr_vblank_tick). */
void sr_ctrl_sample(void) {
    uint8_t lx = 128, ly = 128;
    if (gui_on()) gui_analog(&lx, &ly);
    s_ctrl_ring[s_ctrl_w].btn = h_CtrlButtons();
    s_ctrl_ring[s_ctrl_w].lx = lx;
    s_ctrl_ring[s_ctrl_w].ly = ly;
    s_ctrl_w = (s_ctrl_w + 1) % CTRL_RING;
    if (s_ctrl_w == s_ctrl_r) s_ctrl_r = (s_ctrl_r + 1) % CTRL_RING;  /* drop oldest on overflow */
    sched_wake(CTRL_WAIT_OBJ);
}

/* Fill SceCtrlData[n] from the ring. Returns the number of new samples since the last read (the
 * PSP semantics), giving the game genuine per-frame history for edge/latch detection. Negative
 * reports inverted buttons. peek does not consume or block. */
static uint32_t ctrl_fill_n(uint32_t buf, uint32_t nbufs, int negate, int peek) {
    if (nbufs == 0) nbufs = 1;
    if (nbufs > CTRL_RING) nbufs = CTRL_RING;
    int avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
    /* Blocking ReadBuffer waits for at least one fresh sample (delivered each VBLANK). */
    if (!peek) {
        int guard = 0;
        while (avail == 0 && sr_sched_on && guard++ < 4) {
            sched_block_on(CTRL_WAIT_OBJ);
            avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
        }
    }
    if (avail < 1) avail = 1;                      /* always give at least the latest */
    if (avail > (int)nbufs) avail = (int)nbufs;
    int start = peek ? (s_ctrl_w - avail + CTRL_RING) % CTRL_RING
                     : (s_ctrl_w - avail + CTRL_RING) % CTRL_RING;
    for (int i = 0; i < avail; i++) {
        CtrlSample smp = s_ctrl_ring[(start + i) % CTRL_RING];
        uint32_t field = negate ? ~smp.btn : smp.btn;   /* negative mode inverts buttons only */
        uint32_t e = buf + (uint32_t)i * 16;
        MEM_W32(e + 0, s_vcount_fwd);  /* timestamp (monotonic) */
        MEM_W32(e + 4, field);
        MEM_W8(e + 8, smp.lx); MEM_W8(e + 9, smp.ly); MEM_W8(e + 10, 128); MEM_W8(e + 11, 128);
    }
    if (!peek) s_ctrl_r = s_ctrl_w;                /* consume */
    if (getenv("SR_INLOG")) {
        static unsigned long calls = 0;
        CtrlSample latest = s_ctrl_ring[(s_ctrl_w - 1 + CTRL_RING) % CTRL_RING];
        if ((++calls % 200) == 0 || (latest.btn & 0x8))
            fprintf(stderr, "ctrl_fill #%lu vc=%u avail=%d latest=0x%x lx=%u ly=%u buf=0x%08x neg=%d peek=%d\n",
                    calls, s_vcount_fwd, avail, latest.btn, latest.lx, latest.ly, buf, negate, peek);
    }
    return (uint32_t)avail;
}
static uint32_t ctrl_fill(uint32_t buf, uint32_t count, int negate) {
    return ctrl_fill_n(buf, count, negate, 0);
}
static uint32_t h_CtrlReadBuffer(CpuState *s) { return ctrl_fill(A0, A1, 0); }

/* sceDisplay: remember the framebuffer; vblank waits block until the next delivered vblank. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride);
static uint32_t s_framebuf = 0, s_vcount = 0;
static uint32_t s_last_flip_vcount = 0;   /* hang watchdog (see sr_vblank_tick) */
static uint32_t h_DisplaySetFrameBuf(CpuState *s) {
    s_framebuf = A0;
    s_last_flip_vcount = s_vcount;
    /* SR_GEWATCH: interleave presents with GELIST lines to expose draw-vs-present ordering. */
    {
        static int gw = -1, gwa = 0;
        if (gw < 0) { gw = getenv("SR_GEWATCH") ? 1 : 0;
                      const char *p = getenv("SR_GEWATCH_AFTER"); gwa = p ? atoi(p) : 0; }
        if (gw && s_vcount >= (uint32_t)gwa)
            fprintf(stderr, "PRESENT f=%u buf=0x%08x fmt=%u stride=%u\n", s_vcount, A0, A2, A1);
    }
    /* Asset viewer (SR_VIEWER): drive the game's own engine to load/render assets. Runs here
     * because this is once-per-frame on the render thread with a live CpuState. When it presents
     * its own frame it returns 1, so the game's frame is suppressed. */
    { extern int viewer_on_present(CpuState *); if (viewer_on_present(s)) return 0; }
    /* Interactive window: show this finished frame and sample the keyboard (A1=bufferwidth,
     * A2=pixelformat 0=5650/1=5551/2=4444/3=8888). */
    if (gui_on() && s_framebuf) gui_present(s_framebuf, (int)A2, A1);
    if (getenv("SR_SCENETRACE") && (s_vcount % 30) == 0) {
        /* Director state (f_089e16d0 dispatches the handler at [0x08a6b640+0x40]). In PPSSPP this
         * handler drives the boot sequence and eventually starts the plaympeg movie thread; if it
         * stays null/stuck here, the sequence never advances. Dump the director object. */
        /* Boot state machine (f_0880b398 dispatches table[0x08a3ed58][state]); index 19 = enter
         * intro movie. If the index never reaches 19, the movie never starts. */
        /* Boot sequence state (the gate to the intro movie). f_0880b398 dispatches the handler
         * table at 0x08a3ed58 indexed by the state at 0x08a83c74; index 19 enters the intro movie
         * (installs the director handler at 0x08a6b680). The boot stalls at state 0 (a menu whose
         * scripted/demo input object is never set up -- see VERIFICATION.md Phase 8). */
        fprintf(stderr, "frame %3u: state=%d counter=%d  inTbl[0x08a6b34c]: +000=0x%08x +200=0x%08x +204=0x%08x +208=0x%08x +20c=0x%08x +218=0x%08x\n",
            s_vcount, (int)MEM_R32(0x08a83c74), (int)MEM_R32(0x08a83c78),
            MEM_R32(0x08a6b34c), MEM_R32(0x08a6b54c), MEM_R32(0x08a6b550),
            MEM_R32(0x08a6b554), MEM_R32(0x08a6b558), MEM_R32(0x08a6b564));
    }
    /* SR_BGMSTAT: trace the streamed-BGM player and global fader that gate scene switches.
     * The scene manager (EBOOT sub_F3A0) won't proceed past its fade-to-black until the BGM
     * stream state (dword_39C184[0], guest 0x08BA0184) returns to 0 (idle); the streamer thread
     * (entry 0x08A0063C) must see its stop request ([7], +0x1C) and wind down. The full-screen
     * fade quad alpha comes from the fader object at 0x08A2BC30 {progress,f step,f flags,b}. */
    {
        static int bs = -1;
        if (bs < 0) bs = getenv("SR_BGMSTAT") ? 1 : 0;
        if (bs && (s_vcount % 60) == 0) {
            uint32_t b = 0x08BA0184u;            /* BGM stream channel 0 control block */
            float prog, step;
            uint32_t pw = MEM_R32(0x08A2BC30), sw = MEM_R32(0x08A2BC34);
            memcpy(&prog, &pw, 4); memcpy(&step, &sw, 4);
            fprintf(stderr, "BGMSTAT f=%u state=%d stop=%d pause=%d hs12=%d atrac=%d fd=%d pos=%u "
                            "fade={prog=%g step=%g flags=0x%02x}\n",
                    s_vcount, (int)MEM_R32(b), (int)MEM_R32(b+0x1C), (int)MEM_R32(b+0x28),
                    (int)MEM_R32(b+0x30), (int)MEM_R32(b+0x10), (int)MEM_R32(b+0x14), MEM_R32(b+0x24),
                    prog, step, MEM_R8(0x08A2BC38));
            if ((s_vcount % 600) == 0) { extern void sched_dump_threads(void); sched_dump_threads(); }
        }
    }
    /* SR_REGSTAT: dump the engine's resource-name registry (EBOOT unk_261260, guest 0x08A65260):
     * a directory of {char name[16]; u32 value; u32 extra} entries, count in the header pointed
     * to by +12 (s16 at +4). The hangar aircraft load (fig%02dh.PMD via sub_1B35F8/sub_1C2F64)
     * fails when its names never appear here -- this shows whether registration happens. */
    {
        static int rs = -1;
        static uint32_t rs_last = 0;
        if (rs < 0) rs = getenv("SR_REGSTAT") ? 1 : 0;
        if (rs && s_vcount - rs_last >= 300) {
            rs_last = s_vcount;
            uint32_t reg = 0x08A65260u;
            uint32_t hdr = MEM_R32(reg + 12);
            int cnt = hdr ? (int16_t)MEM_R16(hdr + 4) : -1;
            fprintf(stderr, "REGSTAT f=%u hdr=0x%08x count=%d", s_vcount, hdr, cnt);
            fprintf(stderr, " raw=[");
            for (int k = 0; k < 32; k += 4) fprintf(stderr, "%08x ", MEM_R32(reg + (uint32_t)k));
            fprintf(stderr, "] hdrraw=[");
            if (hdr) for (int k = 0; k < 32; k += 4) fprintf(stderr, "%08x ", MEM_R32(hdr + (uint32_t)k));
            fprintf(stderr, "]");
            /* Init seeds the count at 32, so slots 0..31 are reserved blanks; registrations
             * (sub_1D1FDC) land at base+16+24*count and bump the count. Scan all slots up to
             * count (cap 1024 = array capacity) and print only populated ones. */
            int total = cnt > 1024 ? 1024 : cnt;
            int nonempty = 0, shown = 0;
            for (int i = 0; i < total; i++) {
                uint32_t e = reg + 16 + (uint32_t)i * 24;
                if (MEM_R8(e) == 0) continue;
                nonempty++;
                char nm[17];
                int k;
                for (k = 0; k < 16; k++) { uint8_t c = MEM_R8(e + (uint32_t)k); if (!c || c < 32 || c >= 127) break; nm[k] = (char)c; }
                nm[k] = 0;
                /* the hangar plane lookup wants fig%02dh.* names: always show those */
                int isfig = (nm[0]|32) == 'f' && (nm[1]|32) == 'i' && (nm[2]|32) == 'g';
                if (shown >= 48 && !isfig) continue;
                shown++;
                fprintf(stderr, " [%d:%s:0x%x]", i, nm, MEM_R32(e + 16));
            }
            fprintf(stderr, " nonempty=%d\n", nonempty);
        }
    }
    /* SR_FBSNAP=<N>: dump the presented framebuffer every N frames into a rotating set of
     * PPMs so an unattended run can be observed after the fact (8 most recent kept). */
    {
        static int fs = -2; static uint32_t fs_last = 0;
        if (fs == -2) { const char *e = getenv("SR_FBSNAP"); fs = e ? atoi(e) : 0; }
        if (fs > 0 && s_vcount - fs_last >= (uint32_t)fs) {
            fs_last = s_vcount;
            char p[64];
            snprintf(p, sizeof p, "build/acx/snap_%u.ppm", (s_vcount / (uint32_t)fs) % 8u);
            dump_fb_fmt(p, A0, (int)A2, A1 ? A1 : 512);
            fprintf(stderr, "FBSNAP f=%u -> %s\n", s_vcount, p);
        }
    }
    /* SR_SCENESTAT=1: periodic scene-manager probe (gate/scene-object/boot state words from
     * the FBDUMP one-shot dump, but live) -- shows what a stuck transition is waiting on. */
    {
        static int ss = -1; static uint32_t ss_last = 0;
        if (ss < 0) ss = getenv("SR_SCENESTAT") ? 1 : 0;
        if (ss && s_vcount - ss_last >= 300) {
            ss_last = s_vcount;
            uint32_t scobj = MEM_R32(0x08b9658c);
            fprintf(stderr, "SCENE f=%u gate=0x%08x framectr=%u scobj=0x%08x audio=0x%08x state=%d counter=%d\n",
                    s_vcount, MEM_R32(0x08b98ac0), MEM_R32(0x08b992d0), scobj, MEM_R32(0x08b9af80),
                    (int)MEM_R32(0x08a83c74), (int)MEM_R32(0x08a83c78));
            if (scobj && scobj >= 0x08800000u && scobj < 0x0c000000u)
                for (uint32_t a = scobj; a < scobj + 0x30; a += 16)
                    fprintf(stderr, "  sc+%02x: %08x %08x %08x %08x\n", a - scobj,
                            MEM_R32(a), MEM_R32(a+4), MEM_R32(a+8), MEM_R32(a+12));
        }
    }
    /* The buffer handed to SetFrameBuf is a freshly-completed frame. With SR_FBDUMP=<N>, once N
     * frames have elapsed, snapshot exactly this presented buffer (the right instant -- before the
     * game recycles it as the next back buffer) and stop. */
    const char *fd = getenv("SR_FBDUMP");
    if (fd && s_framebuf && s_vcount >= (uint32_t)atoi(fd)) {
        extern unsigned long g_ge_pixels;
        dump_fb_fmt("build/acx/fb_present.ppm", s_framebuf, (int)A2, A1 ? A1 : 512);
        /* Also snapshot the whole 2MB eDRAM so any rendered region can be found regardless of which
         * buffer/stride/format the game settled on. */
        FILE *raw = fopen("build/acx/edram.bin", "wb");
        if (raw) { for (uint32_t a = 0x04000000; a < 0x04200000; a += 4) { uint32_t w = MEM_R32(a); fwrite(&w, 4, 1, raw); } fclose(raw); }
        sr_trace_close();
        if (getenv("SR_LANGLOG")) fprintf(stderr, "LANG byte @0x08A80C3C = %u\n", MEM_R8(0x08A80C3C));
        extern unsigned long g_tex_samples, g_tex_nonzero;
        fprintf(stderr, "presented frame %u: buf=0x%08x fmt=%u stride=%u ge_pixels=%lu tex_samples=%lu tex_nonzero=%lu\n",
                s_vcount, s_framebuf, A2, A1 ? A1 : 512, g_ge_pixels, g_tex_samples, g_tex_nonzero);
        { extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
          fprintf(stderr, "mpeg: ringPut=%lu getAvcAu=%lu avcDecode=%lu noData=%lu\n",
                  g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata); }
        sr_dump_calls();
        extern void sched_dump_threads(void); sched_dump_threads();
        /* Scene-manager state. f_089fc75c uses base 0x08ba0000 + 0xffff8ac0 -> gate 0x08b98ac0,
         * scene array 0x08b98ac8, framectr 0x08b992d0. */
        fprintf(stderr, "--- scene mgr ---\n");
        fprintf(stderr, "  [0x08b98ac0] gate     = 0x%08x\n", MEM_R32(0x08b98ac0));
        fprintf(stderr, "  [0x08b992d0] framectr = 0x%08x\n", MEM_R32(0x08b992d0));
        uint32_t scobj = MEM_R32(0x08b9658c);
        fprintf(stderr, "  [0x08b9658c] scene-obj = 0x%08x  audio-flag=0x%08x\n", scobj, MEM_R32(0x08b9af80));
        fprintf(stderr, "  --- active scene @0x%08x ---\n", scobj);
        for (uint32_t a = scobj; a < scobj + 0x60; a += 4)
            fprintf(stderr, "    [+0x%02x] = 0x%08x\n", a - scobj, MEM_R32(a));
        uint32_t rroot = 0x08a80ac8u;  /* IDA unk_27CAC8: render object list root */
        uint32_t headp = MEM_R32(rroot + 0x14);
        uint32_t first = headp ? MEM_R32(headp) : 0;
        fprintf(stderr, "--- render list @0x%08x headp=0x%08x first=0x%08x cb=0x%08x ---\n",
                rroot, headp, first, MEM_R32(rroot + 0x20));
        uint32_t node = first;
        for (int i = 0; i < 16 && node; i++) {
            fprintf(stderr,
                "  node[%02d] 0x%08x next=0x%08x prev=0x%08x fn=0x%08x flags=0x%08x b18=%02x b19=%02x b202=%02x b203=%02x payload=0x%08x\n",
                i, node, MEM_R32(node + 4), MEM_R32(node + 8), MEM_R32(node + 12),
                MEM_R32(node + 136), MEM_R8(node + 24), MEM_R8(node + 25),
                MEM_R8(node + 202), MEM_R8(node + 203), MEM_R32(node + 16));
            node = MEM_R32(node + 4);
        }
        _Exit(0);
    }
    return 0;
}
static uint32_t h_DisplayGetFrameBuf(CpuState *s) {
    if (A0) MEM_W32(A0, s_framebuf);
    return 0;
}

/* Dump a PSP framebuffer to a binary PPM. fmt: 0=5650, 1=5551, 2=4444, 3=8888. */
static void dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (!stride) stride = 512;
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            unsigned char rgb[3];
            if (fmt == 3) {
                uint32_t p = MEM_R32(fbaddr + (uint32_t)(y * (int)stride + x) * 4);
                rgb[0] = p & 0xFF; rgb[1] = (p >> 8) & 0xFF; rgb[2] = (p >> 16) & 0xFF;
            } else {
                uint16_t p = MEM_R16(fbaddr + (uint32_t)(y * (int)stride + x) * 2);
                if (fmt == 1) {
                    rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
                    rgb[1] = (unsigned char)(((p >> 5) & 0x1F) * 255 / 31);
                    rgb[2] = (unsigned char)(((p >> 10) & 0x1F) * 255 / 31);
                } else if (fmt == 2) {
                    rgb[0] = (unsigned char)(((p) & 0xF) * 17);
                    rgb[1] = (unsigned char)(((p >> 4) & 0xF) * 17);
                    rgb[2] = (unsigned char)(((p >> 8) & 0xF) * 17);
                } else {
                    rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
                    rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
                    rgb[2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
                }
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    fprintf(stderr, "dumped framebuffer 0x%08x fmt=%d stride=%u -> %s\n", fbaddr, fmt, stride, path);
}
static void dump_fb(const char *path, uint32_t fbaddr) { dump_fb_fmt(path, fbaddr, 3, 512); }

/* Block until the scheduler delivers the next vblank. The old behaviour (delay one tick) let the
 * render loop wake while worker threads were still runnable and redraw the same frame dozens of
 * times per vblank -- the loading screen burned ~50s/60 frames in the rasterizer that way. */
static uint32_t h_DisplayWaitVblank(CpuState *s) { (void)s; sched_wait_vblank(); return 0; }

/* Called once per delivered VBLANK by the scheduler. Counts frames and, with SR_FBDUMP=<n>,
 * snapshots the framebuffer the game has drawn (the display buffer, else the GE target, else
 * the VRAM candidates) and stops, so we can see the rendered frame. */
void sr_ctrl_sample(void);
void ge_set_frame(uint32_t frame);
void sr_vblank_tick(void) {
    s_vcount++; s_vcount_fwd = s_vcount;
    /* TEMP (ACX save debug): watch the RECORDS-SAVE state machine (sub_3717C/sub_37794) that
     * registers a pilot into a numbered profile slot, plus the active profile the main-menu
     * banner reads. ACX ULUS10176, image base 0x08804000:
     *   0x08A80E28 byte_27CE28  slot-occupancy bitmask (bit i = slot i registered)
     *   0x08A80E08 save+0x14    active callsign field (OSK-typed name lands here)
     *   0x08A80CD4 dword_27CCCC+8  active profile name (banner source; copied to/from slots)
     *   0x08A80E29 byte_27CE29  current slot index
     *   0x08A84874 dword_280874 records-save handler state (==1 is the register frame)
     *   0x08A84890 dword_280890 target slot for registration
     *   0x08A84894 dword_280894 register gate (0 = register, !=0 = skip as "existing")
     *   0x08A8A6C4 save+0x18D0  profile-slot 0 name
     * Logs on bitmask change, on the records-save entering/leaving its init frame, and every
     * 2s, so the one-frame registration window can't be missed. */
    if (getenv("SR_DLGLOG") && s_dlg_param) {
        #define ACX_RDSTR(buf, addr) do { \
            uint32_t _a = (addr); \
            for (int _i = 0; _i < 12; _i++) { uint8_t _c = MEM_R8(_a + (uint32_t)_i); \
                (buf)[_i] = (_c >= 32 && _c < 127) ? (char)_c : '.'; } \
            (buf)[12] = 0; } while (0)
        static uint8_t s_acx_last_bm = 0xFF;
        static int s_acx_last_sv = -999;
        uint8_t bm = MEM_R8(0x08A80E28u);
        int sv = (int)MEM_R32(0x08A84874u);
        int edge = (bm != s_acx_last_bm) || (sv != s_acx_last_sv && (sv == 1 || s_acx_last_sv == 1));
        if (edge || (s_vcount % 120) == 0) {
            char act[13], prof[13], slot0[13];
            ACX_RDSTR(act, 0x08A80E08u);
            ACX_RDSTR(prof, 0x08A80CD4u);
            ACX_RDSTR(slot0, 0x08A8A6C4u);
            fprintf(stderr, "ACXDBG vc=%u mode=%d bitmask=0x%02x curslot=%d svstate=%d "
                    "tgtslot=%d gate=0x%08x act='%s' prof='%s' slot0='%s' dlg=%d\n",
                    s_vcount, (int)MEM_R32(0x08A803B0u), bm, (int)MEM_R8(0x08A80E29u), sv,
                    (int)MEM_R32(0x08A84890u), MEM_R32(0x08A84894u),
                    act, prof, slot0, s_dlg_status);
        }
        s_acx_last_bm = bm; s_acx_last_sv = sv;
        #undef ACX_RDSTR
    }
    sr_force_language();
    ge_set_frame(s_vcount);
    sr_ctrl_sample();   /* latch one controller sample per frame (PPSSPP ring semantics) */
    /* Hang watchdog: vblanks keep being delivered even when every game thread is blocked,
     * so a stretch with no sceDisplaySetFrameBuf means the render loop is stuck (e.g. an
     * infinite wait). Dump the thread table + mpeg counters so the hang is diagnosable. */
    if (s_last_flip_vcount && s_vcount - s_last_flip_vcount > 0 &&
        (s_vcount - s_last_flip_vcount) % 600 == 0) {
        extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
        fprintf(stderr, "WATCHDOG: no frame presented for %u vblanks (~%us); "
                "mpeg put=%lu getavc=%lu avcdec=%lu nodata=%lu\n",
                s_vcount - s_last_flip_vcount, (s_vcount - s_last_flip_vcount) / 60,
                g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata);
        { extern void sched_dump_threads(void); sched_dump_threads(); }
        fflush(stderr);
    }
    /* Frame capture happens at sceDisplaySetFrameBuf (the instant a finished frame is presented),
     * which is the correct moment -- snapshotting here at an arbitrary vblank catches a buffer the
     * game has already begun clearing for the next frame. */
}
static uint32_t h_DisplayGetVcount(CpuState *s) { (void)s; return s_vcount; }
/* The PSP scans hCountPerVblank=286 lines per frame (PPSSPP Core/HW/Display.cpp). The current
 * scanline is the intra-frame position [0,285]; the accumulated count is vcount*286 + that, and
 * it is what games poll to time intervals far finer than a frame (e.g. logo/intro durations).
 * Returning the frame counter here (286x too small) makes those waits never elapse. */
static uint32_t s_hcount_vc = 0xffffffff, s_hcount_intra = 0;
static uint32_t hcount_intra(void) {
    if (s_vcount != s_hcount_vc) { s_hcount_vc = s_vcount; s_hcount_intra = 0; }
    else if (s_hcount_intra < 285) s_hcount_intra++;
    return s_hcount_intra;
}
static uint32_t h_DisplayGetCurrentHcount(CpuState *s) { (void)s; return hcount_intra(); }
static uint32_t h_DisplayGetAccumulatedHcount(CpuState *s) { (void)s; return s_vcount * 286u + hcount_intra(); }

/* sceGe_user: pretend the GE finishes immediately (DrawSync returns done). eDRAM at 0x04000000. */
typedef struct {
    int used;
    uint32_t signal_func, signal_arg;
    uint32_t finish_func, finish_arg;
} GeCallback;
static GeCallback s_ge_cb[16];
static uint32_t s_ge_list_next = 0;

static void ge_call_guest(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!fn) return;
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = sr_timeslice;
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = 0x09df8000u;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    sr_timeslice = 20000;
    dispatch(s, fn);
    memcpy(s, &save, sizeof(CpuState));
    sr_timeslice = save_slice;
}

static void ge_finish_callback(CpuState *s, uint32_t cbid, uint32_t list_id, uint32_t user_arg) {
    if (cbid >= (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0]))) return;
    GeCallback *cb = &s_ge_cb[cbid];
    if (!cb->used || !cb->finish_func) return;
    if (getenv("SR_GELOG")) fprintf(stderr, "sceGe finish cbid=%u list=0x%08x fn=0x%08x arg=0x%08x\n",
                                    cbid, list_id, cb->finish_func, cb->finish_arg);
    ge_call_guest(s, cb->finish_func, list_id, cb->finish_arg ? cb->finish_arg : user_arg, cbid);
}

static uint32_t h_GeListEnQueue(CpuState *s) {
    /* a0=list ptr, a1=stall, a2=cbid, a3=arg. With SR_GEDUMP set, log the first list's commands
     * once (bring-up aid for the GE display-list interpreter). */
    uint32_t list_id = 0x35000000u | (s_ge_list_next++ & 0x00ffffffu);
    static int dumped = 0;
    if (!dumped && getenv("SR_GEDUMP")) {
        dumped = 1;
        uint32_t list = A0;
        fprintf(stderr, "sceGeListEnQueue list=0x%08x stall=0x%08x cbid=0x%x\n", A0, A1, A2);
        for (int i = 0; i < 48; i++) {
            uint32_t w = MEM_R32(list + (uint32_t)i * 4);
            fprintf(stderr, "  [%02d] cmd=0x%02x data=0x%06x\n", i, w >> 24, w & 0xFFFFFF);
        }
    }
    if (getenv("SR_GEDUMP2")) {
        static int dumped = 0;
        if (!dumped && s_vcount >= 120) {
            dumped = 1; uint32_t list = A0;
            fprintf(stderr, "=== GE list dump at frame %u (list=0x%08x) ===\n", s_vcount, list);
            for (int i = 0; i < 200; i++) {
                uint32_t w = MEM_R32(list + (uint32_t)i*4); uint32_t cmd = w>>24;
                fprintf(stderr, "  [%02d] cmd=0x%02x data=0x%06x\n", i, cmd, w & 0xFFFFFF);
                if (cmd == 0x0C || cmd == 0x0F) break;   /* END/FINISH */
            }
        }
    }
    ge_run_list(A0);   /* process the list now (sets GE state, rasterises any PRIM) */
    ge_finish_callback(s, A2, list_id, A3);
    if (getenv("SR_GESIG")) {
        extern unsigned long g_ge_list_sig, g_ge_prim_count;
        static unsigned long last_sig = 0; static int call = 0;
        if (g_ge_list_sig != last_sig) {
            fprintf(stderr, "GEsig call=%d frame=%u sig=0x%lx prims=%lu\n", call, s_vcount, g_ge_list_sig, g_ge_prim_count);
            last_sig = g_ge_list_sig;
        }
        call++;
    }
    return list_id;
}
static uint32_t h_GeDrawSync(CpuState *s) { (void)s; return 0; }
static uint32_t h_GeEdramGetAddr(CpuState *s) { (void)s; return 0x04000000; }
static uint32_t h_GeSetCallback(CpuState *s) {
    uint32_t info = A0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(s_ge_cb) / sizeof(s_ge_cb[0])); i++) {
        if (!s_ge_cb[i].used) {
            s_ge_cb[i].used = 1;
            s_ge_cb[i].signal_func = info ? MEM_R32(info + 0) : 0;
            s_ge_cb[i].signal_arg  = info ? MEM_R32(info + 4) : 0;
            s_ge_cb[i].finish_func = info ? MEM_R32(info + 8) : 0;
            s_ge_cb[i].finish_arg  = info ? MEM_R32(info + 12) : 0;
            if (getenv("SR_GELOG")) fprintf(stderr, "sceGeSetCallback cbid=%u sig=0x%08x/0x%08x fin=0x%08x/0x%08x\n",
                                            i, s_ge_cb[i].signal_func, s_ge_cb[i].signal_arg,
                                            s_ge_cb[i].finish_func, s_ge_cb[i].finish_arg);
            return i;
        }
    }
    return 0xffffffffu;
}

/* ---- sceSasCore: real voice mixing (VAG ADPCM), pitch resampling, per-voice volumes ----
 * Mixes into the guest buffer that the game then submits via sceAudioOutput*Blocking (which
 * forwards to the host waveOut backend in audio.c). The envelope is simplified to a gate:
 * KeyOn plays the VAG stream at SetVolume level until its end block or KeyOff -- ACX drives
 * SFX with SetSimpleADSR where attack/release are short next to the 256-sample grain. */
#define SAS_VOICES 32
typedef struct {
    int on;                       /* keyed on and stream not exhausted */
    uint32_t vag, vag_size;       /* VAG stream base and byte size */
    uint32_t pos;                 /* byte offset of the next 16-byte block */
    int loop_start;               /* block offset to loop to (-1 = none) */
    int hist1, hist2;             /* ADPCM filter state */
    int pitch;                    /* 0x1000 = native 44.1 kHz */
    int voll, volr;               /* 0..0x1000 */
    int16_t buf[28]; int bufn, bufi;
    uint32_t frac;                /* 12-bit fixed-point resample remainder */
} SasVoice;
static SasVoice s_sasv[SAS_VOICES];
static int s_sas_grain = 256;

static const int vag_f0[5] = { 0, 60, 115,  98, 122 };
static const int vag_f1[5] = { 0,  0, -52, -55, -60 };

/* Decode the next 16-byte VAG block into v->buf. Returns 0 when the stream ends. */
static int sas_vag_block(SasVoice *v) {
    if (v->pos + 16 > v->vag_size) return 0;
    uint32_t a = v->vag + v->pos;
    int hdr = MEM_R8(a), flags = MEM_R8(a + 1);
    if (flags == 7) return 0;                            /* end marker block */
    int pred = (hdr >> 4) & 0xF, shift = hdr & 0xF;
    if (pred > 4) pred = 0;
    if (flags == 6) v->loop_start = (int)v->pos;         /* loop start block */
    for (int i = 0; i < 28; i++) {
        int byte = MEM_R8(a + 2 + (i >> 1));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int samp = (int)((int16_t)((uint16_t)nib << 12)) >> shift;
        samp += (v->hist1 * vag_f0[pred] + v->hist2 * vag_f1[pred]) >> 6;
        if (samp > 32767) samp = 32767; if (samp < -32768) samp = -32768;
        v->buf[i] = (int16_t)samp;
        v->hist2 = v->hist1; v->hist1 = samp;
    }
    v->pos += 16;
    if (flags == 3 && v->loop_start >= 0) v->pos = (uint32_t)v->loop_start;  /* loop end */
    v->bufn = 28; v->bufi = 0;
    return 1;
}

/* Mix one grain into out (s16 interleaved stereo in guest memory). add=0 overwrites. */
static void sas_mix(uint32_t out, int add) {
    if (!out) return;
    int32_t mixl[1024], mixr[1024];
    int n = s_sas_grain > 1024 ? 1024 : s_sas_grain;
    for (int i = 0; i < n; i++) { mixl[i] = 0; mixr[i] = 0; }
    for (int vi = 0; vi < SAS_VOICES; vi++) {
        SasVoice *v = &s_sasv[vi];
        if (!v->on) continue;
        for (int i = 0; i < n; i++) {
            if (v->bufi >= v->bufn && !sas_vag_block(v)) { v->on = 0; break; }
            int samp = v->buf[v->bufi];
            mixl[i] += (samp * v->voll) >> 12;
            mixr[i] += (samp * v->volr) >> 12;
            v->frac += (uint32_t)(v->pitch > 0 ? v->pitch : 0x1000);
            while (v->frac >= 0x1000) { v->frac -= 0x1000; v->bufi++; }
        }
    }
    for (int i = 0; i < n; i++) {
        int l = mixl[i], r = mixr[i];
        if (add) {
            l += (int16_t)MEM_R16(out + (uint32_t)i * 4);
            r += (int16_t)MEM_R16(out + (uint32_t)i * 4 + 2);
        }
        if (l > 32767) l = 32767; if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; if (r < -32768) r = -32768;
        MEM_W16(out + (uint32_t)i * 4,     (uint16_t)(int16_t)l);
        MEM_W16(out + (uint32_t)i * 4 + 2, (uint16_t)(int16_t)r);
    }
}

static uint32_t h_SasInit(CpuState *s) {
    /* __sceSasInit(core, grain, maxVoices, outMode, sampleRate) */
    s_sas_grain = (int)A1 > 0 && (int)A1 <= 1024 ? (int)A1 : 256;
    memset(s_sasv, 0, sizeof(s_sasv));
    for (int i = 0; i < SAS_VOICES; i++) { s_sasv[i].pitch = 0x1000; s_sasv[i].loop_start = -1; }
    return 0;
}
static uint32_t h_SasSetVoice(CpuState *s) {
    /* __sceSasSetVoice(core, voice, vagAddr, size, loopmode) */
    SasVoice *v = &s_sasv[A1 & 31u];
    v->vag = A2; v->vag_size = A3;
    v->pos = 0; v->loop_start = stack_arg(s, 0) ? 0 : -1;
    v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    return 0;
}
static uint32_t h_SasSetPitch(CpuState *s) { s_sasv[A1 & 31u].pitch = (int)A2; return 0; }
static uint32_t h_SasSetVolume(CpuState *s) {
    /* __sceSasSetVolume(core, voice, l, r, effectL, effectR); volumes 0..0x1000 */
    SasVoice *v = &s_sasv[A1 & 31u];
    v->voll = (int)A2 & 0x1FFF; v->volr = (int)A3 & 0x1FFF;
    return 0;
}
static uint32_t h_SasGetEndFlag(CpuState *s) {
    (void)s;
    uint32_t m = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!s_sasv[i].on) m |= 1u << i;
    return m;
}
static uint32_t h_SasSetKeyOn(CpuState *s) {
    SasVoice *v = &s_sasv[A1 & 31u];
    v->pos = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    if (!v->voll && !v->volr) { v->voll = 0x1000; v->volr = 0x1000; }  /* keyed before SetVolume */
    v->on = v->vag && v->vag_size >= 16;
    return 0;
}
static uint32_t h_SasSetKeyOff(CpuState *s) { s_sasv[A1 & 31u].on = 0; return 0; }
static uint32_t h_SasGetEnvelopeHeight(CpuState *s) { return s_sasv[A1 & 31u].on ? 0x40000000u : 0; }
static uint32_t h_SasCore(CpuState *s)        { sas_mix(A1, 0); return 0; }
static uint32_t h_SasCoreWithMix(CpuState *s) { sas_mix(A1, 1); return 0; }

/* ---- semaphores and event flags, backed by the scheduler's block/wake-on-object ---- */

typedef struct { int used; uint32_t uid; int count, maxc; uint32_t pattern; } Sync;
static Sync s_sync[128];
static Sync *sync_find(uint32_t uid) {
    for (int i = 0; i < 128; i++) if (s_sync[i].used && s_sync[i].uid == uid) return &s_sync[i];
    return NULL;
}
static Sync *sync_new(void) {
    for (int i = 0; i < 128; i++) if (!s_sync[i].used) { s_sync[i].used = 1; s_sync[i].uid = sr_alloc_uid(); return &s_sync[i]; }
    return NULL;
}

static uint32_t h_CreateSema(CpuState *s) {
    /* a0=name, a1=attr, a2=initCount, a3=maxCount. */
    Sync *m = sync_new(); if (!m) return 0x80020000;
    m->count = (int)A2; m->maxc = (int)A3;
    if (getenv("SR_SEMALOG")) fprintf(stderr, "CreateSema uid=0x%x init=%d max=%d\n", m->uid, (int)A2, (int)A3);
    return m->uid;
}
static uint32_t h_DeleteSema(CpuState *s) { Sync *m = sync_find(A0); if (m) m->used = 0; return 0; }
static uint32_t h_WaitSema(CpuState *s) {
    /* a0=semaid, a1=signal, a2=timeout ptr (0=infinite, else *a2 = microseconds). */
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (getenv("SR_SEMALOG")) fprintf(stderr, "WaitSema uid=0x%x need=%d count=%d timeout_ptr=0x%x\n", uid, need, m ? m->count : -999, toptr);
    while (m->count < need) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) return 0x800201A8;  /* WAIT_TIMEOUT */
        } else {
            sched_block_on(uid);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    m->count -= need;
    return 0;
}
static uint32_t h_SignalSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->count += (int)A1;
    sched_wake(A0);
    sched_preempt();    /* a woken higher-priority waiter runs immediately */
    return 0;
}
static uint32_t h_PollSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    int need = (int)A1;
    if (m->count < need) return 0x80020001;   /* would block */
    m->count -= need;
    return 0;
}

static uint32_t h_CreateEventFlag(CpuState *s) {
    /* a0=name, a1=attr, a2=initPattern, a3=opt. */
    Sync *m = sync_new(); if (!m) return 0x80020000;
    m->pattern = A2;
    return m->uid;
}
static uint32_t h_DeleteEventFlag(CpuState *s) { Sync *m = sync_find(A0); if (m) m->used = 0; return 0; }
static uint32_t h_SetEventFlag(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern |= A1; sched_wake(A0); sched_preempt(); return 0;
}
static uint32_t h_ClearEventFlag(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern &= A1; return 0;
}
static int evf_match(Sync *m, uint32_t bits, uint32_t mode) {
    return (mode & 1) ? ((m->pattern & bits) != 0) : ((m->pattern & bits) == bits);
}
static uint32_t h_WaitEventFlag(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    while (!evf_match(m, bits, mode)) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) { if (outp) MEM_W32(outp, m->pattern); return 0x800201A8; }
        } else {
            sched_block_on(uid);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    if (outp) MEM_W32(outp, m->pattern);
    if (mode & 0x20) m->pattern &= ~bits;       /* PSP_EVENT_WAITCLEAR */
    else if (mode & 0x10) m->pattern = 0;        /* PSP_EVENT_WAITCLEARALL */
    return 0;
}
static uint32_t h_PollEventFlag(CpuState *s) {
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (!evf_match(m, bits, mode)) { if (outp) MEM_W32(outp, m->pattern); return 0x80020021; }
    if (outp) MEM_W32(outp, m->pattern);
    return 0;
}
/* sceKernelReferEventFlagStatus(uid, SceKernelEventFlagInfo *info): size(0), name[32](4),
 * attr(36), initPattern(40), currentPattern(44), numWaitThreads(48). Size stays as the caller
 * wrote it; we don't track init pattern or waiters separately. */
static uint32_t h_ReferEventFlagStatus(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    uint32_t info = A1; if (!info) return 0x80020000;
    for (int i = 0; i < 32; i++) MEM_W8(info + 4 + (uint32_t)i, 0);
    MEM_W32(info + 36, 0x200);          /* PSP_EVENT_WAITMULTIPLE */
    MEM_W32(info + 40, m->pattern);
    MEM_W32(info + 44, m->pattern);
    MEM_W32(info + 48, 0);
    return 0;
}

void sr_hle_init(void) {
    if (s_hle_n) return;
    g_callcount = getenv("SR_CALLCOUNT") ? 1 : 0;
    /* NID audit 2026-06: every entry below verified against PPSSPP's HLE tables. A handler on
     * the wrong NID is worse than none -- handlers that fill out-params write through whatever
     * the registers happen to hold for the REAL function's signature (the VolatileMemUnlock
     * mixup sprayed two wild words per asset load and zeroed the model-slot counter). */
    sr_hle_register(0x7591c7db, "sceKernelSetCompiledSdkVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0xf77d77cb, "sceKernelSetCompilerVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0x237dbd4f, "sceKernelAllocPartitionMemory", h_AllocPartitionMemory);
    sr_hle_register(0x9d9a5ba1, "sceKernelGetBlockHeadAddr", h_GetBlockHeadAddr);
    sr_hle_register(0xb6d61d02, "sceKernelFreePartitionMemory", h_FreePartitionMemory);
    sr_hle_register(0xf919f628, "sceKernelTotalFreeMemSize", h_TotalFreeMemSize);
    sr_hle_register(0xa291f107, "sceKernelMaxFreeMemSize", h_MaxFreeMemSize);
    sr_hle_register(0x446d8de6, "sceKernelCreateThread", h_CreateThread);
    sr_hle_register(0xf475845d, "sceKernelStartThread", h_StartThread);
    sr_hle_register(0xaa73c935, "sceKernelExitThread", h_ExitThread);
    sr_hle_register(0x809ce29b, "sceKernelExitDeleteThread", h_ExitThread);
    sr_hle_register(0xe81caf8f, "sceKernelCreateCallback", h_module_uid);
    sr_hle_register(0xceadeb47, "sceKernelDelayThread", h_DelayThread);
    sr_hle_register(0x94aa61ee, "sceKernelGetThreadCurrentPriority", h_GetThreadPriority);
    sr_hle_register(0x9ace131e, "sceKernelSleepThread", h_SleepThread);
    sr_hle_register(0xd59ead2f, "sceKernelWakeupThread", h_WakeupThread);
    sr_hle_register(0xfccfad26, "sceKernelCancelWakeupThread", h_CancelWakeupThread);
    sr_hle_register(0x278c0df5, "sceKernelWaitThreadEnd", h_WaitThreadEnd);
    sr_hle_register(0x840e8133, "sceKernelWaitThreadEndCB", h_WaitThreadEnd);
    sr_hle_register(0x71bc9871, "sceKernelChangeThreadPriority", h_ChangeThreadPriority);
    sr_hle_register(0x383f7bcc, "sceKernelTerminateDeleteThread", h_TerminateDeleteThread);
    sr_hle_register(0x9fa03cd3, "sceKernelDeleteThread", h_ok);
    sr_hle_register(0xa66b0120, "sceKernelReferEventFlagStatus", h_ReferEventFlagStatus);
    sr_hle_register(0xffc36a14, "sceKernelReferThreadRunStatus", h_ReferThreadRunStatus);
    sr_hle_register(0xd8b73127, "sceKernelGetModuleIdByAddress", h_GetModuleId);
    sr_hle_register(0x092968f4, "sceKernelCpuSuspendIntr", h_CpuSuspendIntr);
    sr_hle_register(0x5f10d406, "sceKernelCpuResumeIntr", h_ok);
    /* Boot setup batch (return success / reference value). */
    sr_hle_register(0x4ac57943, "sceKernelRegisterExitCallback", h_ok);
    sr_hle_register(0xa5da2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt);
    sr_hle_register(0x36aa6e91, "sceImposeSetLanguageMode", h_ok);
    /* sceUtility dialogs (OSK / savedata / netconf): no dialog active -> status 0, calls ok. */
    sr_hle_register(0xf3f76017, "sceUtilityOskGetStatus", h_OskGetStatus);
    sr_hle_register(0x4b85c861, "sceUtilityOskUpdate", h_OskUpdate);
    sr_hle_register(0x3dfaeba9, "sceUtilityOskShutdownStart", h_OskShutdown);
    sr_hle_register(0x1579a159, "sceUtilityLoadNetModule", h_ok);
    /* 0xf6269b82 is OskInitStart, NOT GetSystemParamString -- the old string handler wrote
     * A2 bytes through A1, both garbage for this signature. */
    sr_hle_register(0xf6269b82, "sceUtilityOskInitStart", h_OskInitStart);
    sr_hle_register(0x50c4cd57, "sceUtilitySavedataInitStart", h_SavedataInitStart);
    sr_hle_register(0x9790b33c, "sceUtilitySavedataShutdownStart", h_DlgShutdown);
    sr_hle_register(0x8874dbe0, "sceUtilitySavedataGetStatus", h_DlgGetStatus);
    sr_hle_register(0xd4b95ffb, "sceUtilitySavedataUpdate", h_SavedataUpdate);
    sr_hle_register(0x64d50c56, "sceUtilityUnloadNetModule", h_ok);
    /* sceWlan: the game stamps saves/profiles with the console's MAC. A stable fake works
     * (PPSSPP behaviour); low 2 bits of byte 0 must be clear (locally-administered/multicast
     * OUI bits confuse some games -- PPSSPP masks them too). */
    sr_hle_register(0x0c622081, "sceWlanGetEtherAddr", h_WlanGetEtherAddr);
    sr_hle_register(0x93440b11, "sceWlanDevIsPowerOn", h_WlanOn);
    sr_hle_register(0xd7763699, "sceWlanGetSwitchState", h_WlanOn);
    sr_hle_register(0x04b7766e, "scePowerRegisterCallback", h_ok);
    sr_hle_register(0x293b45b8, "sceKernelGetThreadId", h_GetThreadIdSched);
    sr_hle_register(0x82826f70, "sceKernelSleepThreadCB", h_SleepThread);
    sr_hle_register(0x46ebb729, "sceUmdCheckMedium", h_UmdCheckMedium);
    sr_hle_register(0xc6183d47, "sceUmdActivate", h_ok);
    sr_hle_register(0x8ef08fce, "sceUmdWaitDriveStat", h_ok);
    sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy);
    /* sceMpeg: faithful port (src/rt/mpeg.c). Drives the PSMF intro to completion. */
    sr_hle_register(0x682a619b, "sceMpegInit", h_MpegInit);
    sr_hle_register(0x874624d6, "sceMpegFinish", h_MpegFinish);
    sr_hle_register(0xc132e22f, "sceMpegQueryMemSize", h_MpegQueryMemSize);
    sr_hle_register(0xd8c5f121, "sceMpegCreate", h_MpegCreate);
    sr_hle_register(0x606a4649, "sceMpegDelete", h_MpegDelete);
    sr_hle_register(0x42560f23, "sceMpegRegistStream", h_MpegRegistStream);
    sr_hle_register(0x591a4aa2, "sceMpegUnRegistStream", h_MpegUnRegistStream);
    sr_hle_register(0x21ff80e4, "sceMpegQueryStreamOffset", h_MpegQueryStreamOffset);
    sr_hle_register(0x611e9e11, "sceMpegQueryStreamSize", h_MpegQueryStreamSize);
    sr_hle_register(0xd7a29f46, "sceMpegRingbufferQueryMemSize", h_MpegRingbufferQueryMemSize);
    sr_hle_register(0x37295ed8, "sceMpegRingbufferConstruct", h_MpegRingbufferConstruct);
    sr_hle_register(0x13407f13, "sceMpegRingbufferDestruct", h_ok);
    sr_hle_register(0xb240a59e, "sceMpegRingbufferPut", h_MpegRingbufferPut);
    sr_hle_register(0xb5f6dc87, "sceMpegRingbufferAvailableSize", h_MpegRingbufferAvailable);
    sr_hle_register(0xfe246728, "sceMpegGetAvcAu", h_MpegGetAvcAu);
    sr_hle_register(0xe1ce83a7, "sceMpegGetAtracAu", h_MpegGetAtracAu);
    sr_hle_register(0x0e3c2e9d, "sceMpegAvcDecode", h_MpegAvcDecode);
    sr_hle_register(0x800c44df, "sceMpegAtracDecode", h_MpegAtracDecode);
    sr_hle_register(0x740fccd1, "sceMpegAvcDecodeStop", h_MpegAvcDecodeStop);
    sr_hle_register(0x4571cc64, "sceMpegAvcDecodeFlush", h_ok);
    sr_hle_register(0xa780cf7e, "sceMpegMallocAvcEsBuf", h_MpegMallocAvcEsBuf);
    sr_hle_register(0xceb870b1, "sceMpegFreeAvcEsBuf", h_MpegFreeAvcEsBuf);
    sr_hle_register(0x167afd9e, "sceMpegInitAu", h_MpegInitAu);
    sr_hle_register(0xf8dcb679, "sceMpegQueryAtracEsSize", h_MpegQueryAtracEsSize);
    /* sceAtrac3plus (control flow only; silence output). */
    sr_hle_register(0x7a20e7af, "sceAtracSetDataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x61eb33f5, "sceAtracReleaseAtracID", h_AtracReleaseAtracID);
    sr_hle_register(0x6a8c3cd5, "sceAtracDecodeData", h_AtracDecodeData);
    sr_hle_register(0x9ae849a7, "sceAtracGetRemainFrame", h_AtracGetRemainFrame);
    sr_hle_register(0x5d268707, "sceAtracGetStreamDataInfo", h_AtracGetStreamDataInfo);
    sr_hle_register(0x7db31251, "sceAtracAddStreamData", h_AtracAddStreamData);
    sr_hle_register(0xe23e3a35, "sceAtracGetNextDecodePosition", h_AtracGetNextDecodePosition);
    sr_hle_register(0xa2bba8be, "sceAtracGetSoundSample", h_AtracGetSoundSample);
    sr_hle_register(0xfaa4f89b, "sceAtracGetLoopStatus", h_AtracGetLoopStatus);
    sr_hle_register(0x868120b5, "sceAtracSetLoopNum", h_AtracSetLoopNum);
    sr_hle_register(0x644e5607, "sceAtracResetPlayPosition", h_AtracResetPlayPosition);
    sr_hle_register(0x707b7629, "sceMpegFlushAllStream", h_ok);
    /* sceLibFont (firmware fonts absent; valid handles + empty glyphs). */
    sr_hle_register(0x67f17ed7, "sceFontNewLib", h_FontNewLib);
    sr_hle_register(0xa834319d, "sceFontOpen", h_FontOpen);
    sr_hle_register(0x574b6fbc, "sceFontDoneLib", h_ok);
    sr_hle_register(0x0da7535e, "sceFontGetFontInfo", h_FontGetFontInfo);
    sr_hle_register(0xdcc80c2f, "sceFontGetCharInfo", h_FontGetCharInfo);
    sr_hle_register(0x980f4895, "sceFontGetCharGlyphImage", h_FontGetCharGlyphImage);
    sr_hle_register(0x099ef33c, "sceFontFindOptimumFont", h_FontFindOptimumFont);
    sr_hle_register(0x3aea8cb6, "sceFontClose", h_ok);
    sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacMemcpy);
    sr_hle_register(0xaee7404d, "sceUmdRegisterUMDCallBack", h_ok);
    sr_hle_register(0x6af9b50a, "sceUmdCancelWaitDriveStat", h_ok);
    sr_hle_register(0x6b4a146c, "sceUmdGetDriveStat", h_UmdDriveStat);
    sr_hle_register(0x20628e6f, "sceUmdGetErrorStat", h_ok);
    sr_hle_register(0x56202973, "sceUmdWaitDriveStatWithTimer", h_ok);
    sr_hle_register(0xc629af26, "sceUtilityLoadAvModule", h_ok);
    sr_hle_register(0x977de386, "sceKernelLoadModule", h_LoadModule);
    sr_hle_register(0x50f0c1ec, "sceKernelStartModule", h_ok);
    sr_hle_register(0xf0a26395, "sceKernelGetModuleId", h_GetModuleId);
    sr_hle_register(0xd1ff982a, "sceKernelStopModule", h_ok);
    sr_hle_register(0x2e0911aa, "sceKernelUnloadModule", h_ok);
    sr_hle_register(0x8f2df740, "sceKernelStopUnloadSelfModuleWithStatus", h_ok);
    sr_hle_register(0xf919f628, "SysMemUserForUser_f919f628", h_F919F628);
    /* IoFileMgrForUser: file IO from the ISO. */
    sr_hle_register(0x109f50bc, "sceIoOpen", h_IoOpen);
    sr_hle_register(0x6a638d83, "sceIoRead", h_IoRead);
    sr_hle_register(0x42ec03ac, "sceIoWrite", h_IoWrite);
    sr_hle_register(0x779103a0, "sceIoRename", h_ok);
    sr_hle_register(0x68963324, "sceIoLseek32", h_IoLseek32);
    sr_hle_register(0x27eb27b8, "sceIoLseek", h_IoLseek);
    sr_hle_register(0x810c4bc3, "sceIoClose", h_IoClose);
    sr_hle_register(0xace946e8, "sceIoGetstat", h_IoGetstat);
    sr_hle_register(0x89aa9906, "sceIoOpenAsync", h_IoOpenAsync);
    sr_hle_register(0xa0b5a7c2, "sceIoReadAsync", h_IoReadAsync);
    sr_hle_register(0x71b19e77, "sceIoLseekAsync", h_IoLseekAsync);
    sr_hle_register(0xe23eec33, "sceIoWaitAsync", h_IoWaitAsync);
    sr_hle_register(0x35dbd746, "sceIoWaitAsyncCB", h_IoWaitAsync);
    sr_hle_register(0x3251ea56, "sceIoPollAsync", h_IoWaitAsync);
    sr_hle_register(0xff5940b6, "sceIoCloseAsync", h_IoCloseAsync);
    sr_hle_register(0x54f5fb11, "sceIoDevctl", h_ok);
    /* sceAudio */
    sr_hle_register(0x5ec81c55, "sceAudioChReserve", h_AudioChReserve);
    sr_hle_register(0x6fc46853, "sceAudioChRelease", h_AudioChRelease);
    sr_hle_register(0x136caf51, "sceAudioOutputBlocking", h_AudioOutputBlocking);
    sr_hle_register(0x13f592bc, "sceAudioOutputPannedBlocking", h_AudioOutputPannedBlocking);
    sr_hle_register(0xe2d56b2d, "sceAudioOutputPanned", h_AudioOutputPannedBlocking);
    sr_hle_register(0x95fd0c2d, "sceAudioChangeChannelConfig", h_ok);
    sr_hle_register(0xb011922f, "sceAudioGetChannelRestLength", h_AudioRestLen);
    sr_hle_register(0xb7e1d8e7, "sceAudioChangeChannelVolume", h_ok);
    sr_hle_register(0xcb2e439e, "sceAudioSetChannelDataLen", h_AudioSetChannelDataLen);
    /* sceCtrl */
    sr_hle_register(0x1f4011e6, "sceCtrlSetSamplingMode", h_ok);
    sr_hle_register(0xa7144800, "sceCtrlSetIdleCancelThreshold", h_ok);
    sr_hle_register(0x1f803938, "sceCtrlReadBufferPositive", h_CtrlReadBuffer);
    /* 0x687660fa is GetIdleCancelThreshold(int*,int*), NOT ReadBufferNegative -- the pad
     * handler used pointer a1 as a buffer count and wrote up to a ring of SceCtrlData
     * through a1's 4-byte int (and could block the caller on the input ring). */
    sr_hle_register(0x687660fa, "sceCtrlGetIdleCancelThreshold", h_CtrlGetIdleCancelThreshold);
    /* sceDisplay */
    sr_hle_register(0x0e20f177, "sceDisplaySetMode", h_ok);
    sr_hle_register(0x289d82fe, "sceDisplaySetFrameBuf", h_DisplaySetFrameBuf);
    sr_hle_register(0xeeda2e54, "sceDisplayGetFrameBuf", h_DisplayGetFrameBuf);
    sr_hle_register(0x984c27e7, "sceDisplayWaitVblankStart", h_DisplayWaitVblank);
    sr_hle_register(0x9c6eaad7, "sceDisplayGetVcount", h_DisplayGetVcount);
    sr_hle_register(0x773dd3a3, "sceDisplayGetCurrentHcount", h_DisplayGetCurrentHcount);
    sr_hle_register(0x210eab3a, "sceDisplayGetAccumulatedHcount", h_DisplayGetAccumulatedHcount);
    sr_hle_register(0x4d4e10ec, "sceDisplayIsVblank", h_ok);
    sr_hle_register(0xdba6c4c4, "sceDisplayGetFramePerSec", h_ok);
    /* sceGe_user */
    sr_hle_register(0xab49e76a, "sceGeListEnQueue", h_GeListEnQueue);
    sr_hle_register(0xb287bd61, "sceGeDrawSync", h_GeDrawSync);
    sr_hle_register(0xe47e40e4, "sceGeEdramGetAddr", h_GeEdramGetAddr);
    sr_hle_register(0xa4fc06a4, "sceGeSetCallback", h_GeSetCallback);
    /* sceSasCore: real VAG voice mixing (see sas_mix above). */
    sr_hle_register(0x68a46b95, "__sceSasGetEndFlag", h_SasGetEndFlag);
    sr_hle_register(0xa3589d81, "__sceSasCore", h_SasCore);
    sr_hle_register(0x50a14dfc, "__sceSasCoreWithMix", h_SasCoreWithMix);
    sr_hle_register(0x76f01aca, "__sceSasSetKeyOn", h_SasSetKeyOn);
    sr_hle_register(0xa0cf2fa4, "__sceSasSetKeyOff", h_SasSetKeyOff);
    sr_hle_register(0x42778a9f, "__sceSasInit", h_SasInit);
    sr_hle_register(0x99944089, "__sceSasSetVoice", h_SasSetVoice);
    sr_hle_register(0xad84d37f, "__sceSasSetPitch", h_SasSetPitch);
    sr_hle_register(0x440ca7d8, "__sceSasSetVolume", h_SasSetVolume);
    sr_hle_register(0x74ae582a, "__sceSasGetEnvelopeHeight", h_SasGetEnvelopeHeight);
    static const uint32_t sas_ok[] = {     /* ADSR/reverb/noise setters: accepted, unmodelled */
        0x019b25eb, 0x267a6dd2, 0x2c8e6ab3, 0x33d4ab37,
        0x5f9529f6, 0x787d04d5, 0x9ec3676a,
        0xb7660a23, 0xcbcd4f79, 0xd5a229c9, 0xf983b186 };
    for (unsigned i = 0; i < sizeof(sas_ok) / sizeof(sas_ok[0]); i++)
        sr_hle_register(sas_ok[i], "__sceSas_ok", h_ok);
    /* time / rtc / libc clock */
    sr_hle_register(0x369ed59d, "sceKernelGetSystemTimeLow", h_GetSystemTimeLow);
    sr_hle_register(0x82bc5777, "sceKernelGetSystemTimeWide", h_GetSystemTimeWide);
    sr_hle_register(0xdb738f35, "sceKernelGetSystemTime", h_RtcGetCurrentTick);
    sr_hle_register(0x6ff40acc, "sceRtcGetTick", h_RtcGetCurrentTick);
    sr_hle_register(0xcf561893, "sceRtcGetWin32FileTime", h_RtcGetWin32FileTime);
    sr_hle_register(0xe7c27d1b, "sceRtcGetCurrentClockLocalTime", h_RtcGetCurrentClock);
    sr_hle_register(0x91e4f6a7, "sceKernelLibcClock", h_LibcClock);
    sr_hle_register(0x27cc57f0, "sceKernelLibcTime", h_LibcTime);
    sr_hle_register(0x71ec4271, "sceKernelLibcGettimeofday", h_LibcGettimeofday);
    /* cache / misc UtilsForUser: no-ops are fine without a real cache. */
    sr_hle_register(0x79d1c3fa, "sceKernelDcacheWritebackAll", h_ok);
    sr_hle_register(0x3ee30821, "sceKernelDcacheWritebackRange", h_ok);
    sr_hle_register(0x6ad345d7, "sceKernelSetGPO", h_ok);
    /* Stdio std handles. */
    sr_hle_register(0x13a5abef, "sceKernelPrintf", h_KernelPrintf);
    sr_hle_register(0x172d316e, "sceKernelStdin", h_StdFd);
    sr_hle_register(0xa6bab2e9, "sceKernelStdout", h_StdFd);
    sr_hle_register(0xf78ba90a, "sceKernelStderr", h_StdFd);
    /* scePower / sceSuspendForUser / LoadExecForUser: locks and registrations succeed. */
    sr_hle_register(0x3aee7261, "sceKernelPowerUnlock", h_ok);
    sr_hle_register(0xeadb1bd7, "sceKernelPowerLock", h_ok);
    sr_hle_register(0x090ccb3f, "sceKernelPowerTick", h_ok);
    sr_hle_register(0x3e0271d3, "sceKernelVolatileMemLock", h_VolatileMemLock);
    sr_hle_register(0xa14f40b2, "sceKernelVolatileMemTryLock", h_VolatileMemLock);
    /* Unlock takes only the type arg -- it must NOT run the Lock handler: writing the
     * out-params through leftover a1/a2 register garbage sprayed two wild 4-byte writes
     * per asset-load unlock (this zeroed the resource registry's model-slot counter,
     * which silently killed every .PMD model lookup, e.g. the hangar aircraft). */
    sr_hle_register(0xa569e425, "sceKernelVolatileMemUnlock", h_ok);
    sr_hle_register(0x05572a5f, "sceKernelExitGame", h_ok);   /* returning lets the app idle on */
    /* InterruptManager: record the VBLANK handler; the scheduler delivers it per frame. */
    sr_hle_register(0xca04a2b9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntr);
    sr_hle_register(0xd61e6961, "sceKernelReleaseSubIntrHandler", h_ok);
    sr_hle_register(0xfb8e22ec, "sceKernelEnableSubIntr", h_EnableSubIntr);
    sr_hle_register(0x8a389411, "sceKernelDisableSubIntr", h_ok);
    /* semaphores */
    sr_hle_register(0xd6da4ba1, "sceKernelCreateSema", h_CreateSema);
    sr_hle_register(0x28b6489c, "sceKernelDeleteSema", h_DeleteSema);
    sr_hle_register(0x4e3a1105, "sceKernelWaitSema", h_WaitSema);
    sr_hle_register(0x6d212bac, "sceKernelWaitSemaCB", h_WaitSema);
    sr_hle_register(0x3f53e640, "sceKernelSignalSema", h_SignalSema);
    sr_hle_register(0x58b1f937, "sceKernelPollSema", h_PollSema);
    /* event flags */
    sr_hle_register(0x55c20a00, "sceKernelCreateEventFlag", h_CreateEventFlag);
    sr_hle_register(0xef9e4c70, "sceKernelDeleteEventFlag", h_DeleteEventFlag);
    sr_hle_register(0x1fb15a32, "sceKernelSetEventFlag", h_SetEventFlag);
    sr_hle_register(0x812346e4, "sceKernelClearEventFlag", h_ClearEventFlag);
    sr_hle_register(0x402fcf22, "sceKernelWaitEventFlag", h_WaitEventFlag);
    sr_hle_register(0x328c546a, "sceKernelWaitEventFlagCB", h_WaitEventFlag);
    sr_hle_register(0x30fd48f0, "sceKernelPollEventFlag", h_PollEventFlag);
}

/* ---- dispatch ---- */

void sr_syscall(CpuState *s, uint32_t nid) {
    sr_hle_init();
    sr_last_nid = nid;
    if (getenv("SR_NIDLOG")) {
        static FILE *nf = NULL; static unsigned long nc = 0;
        if (!nf) nf = fopen("build/acx/nidseq_mine.txt", "w");
        if (nf) { fprintf(nf, "0x%08x\n", nid); if ((++nc & 0x3f) == 0) fflush(nf); }
    }
    HleEntry *e = hle_find(nid);
    if (!e) {
        /* Permissive mode (SR_PERMISSIVE): unimplemented NIDs log once and return 0 instead of
         * trapping the thread. This is for bring-up exploration -- it lets audio/video (sceMpeg,
         * sceAtrac, sceSas) and other not-yet-modelled subsystems no-op so the main render thread
         * can run. It deliberately returns fake success, so it is gated and not the default. */
        if (getenv("SR_PERMISSIVE")) {
            static uint32_t warned[1024]; static int nwarned = 0; int known = 0;
            for (int i = 0; i < nwarned; i++) if (warned[i] == nid) { known = 1; break; }
            if (!known && nwarned < 1024) { warned[nwarned++] = nid;
                const char *nm = sr_nid_name(nid);
                fprintf(stderr, "HLE: permissive no-op nid 0x%08x (%s) (thread uid 0x%x)\n",
                        nid, nm ? nm : "unknown", sched_current_uid()); }
            s->r[1] = 0xDEADBEEFu;
            for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
            s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu; s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
            s->r[2] = 0;
            return;
        }
        {
            const char *nm = sr_nid_name(nid);
            fprintf(stderr, "HLE: unimplemented nid 0x%08x (%s) (thread uid 0x%x)\n"
                            "     -> add a handler in src/rt/hle.c: sr_hle_register(0x%08xu, \"%s\", h_...);\n",
                    nid, nm ? nm : "unknown", sched_current_uid(), nid, nm ? nm : "sceUnknown");
        }
        sr_hit_hle = 1;
        /* Under the fiber scheduler, longjmp across fibers is invalid; stop the process cleanly
         * after flushing the trace. The plain driver (no scheduler) keeps the longjmp boundary. */
        if (sr_sched_on) { sr_trace_close(); fflush(stderr); _Exit(7); }
        longjmp(g_hle_jmp, 1);
    }
    if (getenv("SR_SYSLOG")) {
        /* Log the first time each (thread, nid) pair occurs, to see what each thread does. */
        static struct { uint32_t uid, nid; } seen[4096]; static int nseen = 0;
        uint32_t cur = sched_current_uid(); int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i].uid == cur && seen[i].nid == nid) { known = 1; break; }
        if (!known && nseen < 4096) { seen[nseen].uid = cur; seen[nseen].nid = nid; nseen++;
            fprintf(stderr, "thr 0x%x : %s (0x%08x)\n", cur, e->name, nid); }
    }
    if (g_callcount) {
        int j = 0; for (; j < g_ncc; j++) if (g_cc[j].nid == nid) break;
        if (j == g_ncc && g_ncc < 512) { g_cc[j].nid = nid; g_cc[j].nm = e->name; g_cc[j].n = 0; g_ncc++; }
        if (j < 512) g_cc[j].n++;
    }
    uint32_t ret = e->fn(s);
    /* Poison caller-saved temps exactly like PPSSPP SetDeadbeefRegs: r1, r4-r15, r24, r25,
     * hi, lo. The return value in v0 (and v1) is written afterward and survives. */
    s->r[1] = 0xDEADBEEFu;
    for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
    s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu;
    s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
    s->r[2] = ret;
}
