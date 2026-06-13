/* Bring-up driver for the recompiled C (Phase 3 differential).
 *
 * Loads a statically-linked PSP ELF into guest memory, seeds the CpuState from the entry
 * register state the PPSSPP reference trace recorded ("# init" line in the trace), registers
 * every generated function, and runs the entry function with tracing on. Execution stops
 * cleanly at the first HLE call (sr_hle_call longjmps), exactly where the reference trace is
 * truncated for comparison.
 *
 * Usage: driver <elf> <ref-trace> <out-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#ifdef SR_VULKAN
#include "gpu_vk/gpu_bridge.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void sr_register_all(void);

/* Last-resort crash reporter: a silent host fault (access violation etc.) otherwise kills
 * the process with nothing in the log. Maps a faulting address inside the guest arena back
 * to its guest address. */
static LONG WINAPI sr_crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "HOST CRASH: exception 0x%08lx at host %p\n",
            er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uint8_t *fa = (const uint8_t *)er->ExceptionInformation[1];
        fprintf(stderr, "  %s of host %p", er->ExceptionInformation[0] ? "write" : "read", fa);
        if (g_mem && fa >= g_mem - 0x04000000 && fa < g_mem + 0x04000000)
            fprintf(stderr, " (guest 0x%08x)", (uint32_t)(fa - g_mem) + 0x08000000u);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   /* still crash (after reporting) */
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *out_len = n;
    return d;
}

static uint32_t load_elf(const uint8_t *elf) {
    if (memcmp(elf, "\x7f""ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); exit(2); }
    uint32_t e_entry = rd32(elf + 24);
    uint32_t e_phoff = rd32(elf + 28);
    uint16_t phentsize = rd16(elf + 42);
    uint16_t phnum = rd16(elf + 44);
    sr_mem_init();
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * phentsize;
        uint32_t p_type = rd32(ph + 0), p_off = rd32(ph + 4), p_vaddr = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16);
        if (p_type == 1 && p_filesz)  /* PT_LOAD */
            sr_load_segment(p_vaddr, elf + p_off, p_filesz);
    }
    return e_entry;
}

static void seed_from_init(const char *trace_path, CpuState *s) {
    FILE *f = fopen(trace_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
    char buf[8192];
    int found = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "# init", 6) != 0)
            continue;
        found = 1;
        char *tok = strtok(buf + 6, " \t\n");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                uint32_t val = (uint32_t)strtoul(eq + 1, NULL, 16);
                if (tok[0] == 'r') s->r[atoi(tok + 1)] = val;
                else if (tok[0] == 'f' && tok[1] >= '0' && tok[1] <= '9') s->fi[atoi(tok + 1)] = val;
                else if (strcmp(tok, "hi") == 0) s->hi = val;
                else if (strcmp(tok, "lo") == 0) s->lo = val;
                else if (strcmp(tok, "fcr31") == 0) s->fcr31 = val;
            }
            tok = strtok(NULL, " \t\n");
        }
        break;
    }
    fclose(f);
    if (!found) { fprintf(stderr, "no '# init' in %s\n", trace_path); exit(2); }
}

int main(int argc, char **argv) {
    uint32_t entry;
    const char *ref_trace, *out;

    SetUnhandledExceptionFilter(sr_crash_filter);

#ifdef SR_VULKAN
    /* In GUI mode, PPSSPP's GPU owns guest memory (it needs the mirror-mapped arena). Adopt its
     * base as g_mem before any segment load, so the game image and the GPU share one arena. */
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--gui") == 0) {
        void *base = acx_gpu_mem_init();
        if (base) sr_mem_use_external(base);
        else fprintf(stderr, "acx_gpu_mem_init failed; falling back to local arena\n");
        break;
    }
#endif

    /* Image mode: a pre-relocated flat image (e.g. a rebased PRX from tools/prxload.py) is
     * loaded at <base> and run from <entry>. Used for Ace Combat X, whose PRX must be
     * rebased + relocated before it has concrete addresses.
     *   driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>  */
    if (argc >= 7 && strcmp(argv[1], "--image") == 0) {
        long len;
        uint8_t *img = read_file(argv[2], &len);
        uint32_t base = (uint32_t)strtoul(argv[3], NULL, 16);
        entry = (uint32_t)strtoul(argv[4], NULL, 16);
        ref_trace = argv[5];
        out = argv[6];
        sr_mem_init();
        sr_load_segment(base, img, (uint32_t)len);
        goto have_image;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: driver <elf> <ref-trace> <out-trace>\n"
                        "       driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>\n");
        return 2;
    }
    {
        long len;
        uint8_t *elf = read_file(argv[1], &len);
        entry = load_elf(elf);
    }
    ref_trace = argv[2];
    out = argv[3];
have_image:;

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4;  /* SPREFIX = identity */
    s.vfpuCtrl[1] = 0xe4;  /* TPREFIX = identity */
    s.vfpuCtrl[2] = 0;     /* DPREFIX = none */
    seed_from_init(ref_trace, &s);
    s.pc = entry;

    sr_register_all();
    /* "none" as the out path disables tracing -- much faster for HLE bring-up, where the goal
     * is to reach the next unimplemented import rather than diff a trace. */
    if (strcmp(out, "none") != 0 && sr_trace_open(out, "cgtest", entry) != 0) {
        fprintf(stderr, "cannot open out trace %s\n", out);
        return 2;
    }
    RecompFn fn = sr_lookup(entry);
    if (!fn) { fprintf(stderr, "no recompiled function at entry 0x%08x\n", entry); return 2; }

    int use_sched = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--sched") == 0) use_sched = 1;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--gui") == 0) { gui_init("Ace Combat X: Skies of Deception"); use_sched = 1; }

    if (use_sched) {
        /* Run with the cooperative scheduler so the game's threads interleave (the boot busy-
         * waits on a sibling). sched_run uses s as the live register file for whichever thread
         * runs; it returns when no thread is runnable, or the process exits at an unimplemented
         * import inside a thread fiber. */
        sched_init(&s);
        sched_run(entry, s.r[4], s.r[5]);
    } else if (setjmp(g_hle_jmp) == 0) {
        fn(&s);
    }
    sr_trace_close();
    fprintf(stderr, "done (hit_hle=%d)\n", sr_hit_hle);
    return 0;
}
