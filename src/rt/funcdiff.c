/* Per-function differential harness for the statically-recompiled game.
 *
 * Verifies one recompiled function against the PPSSPP reference trace without needing HLE. It
 * replays the reference trace from "# init", applying every recorded register and memory write
 * to the CpuState and to guest memory, until execution first reaches <target> at <entry-step>
 * (a function entry). At that point the guest state -- registers, VFPU lanes, and all memory
 * the game has written so far -- matches the reference exactly. The recompiled function is then
 * run with tracing on; it stops when it returns (C return) or reaches an import (longjmp).
 *
 * The emitted trace (steps renumbered from 0) is compared by a separate script against the
 * reference slice starting at <entry-step>. A function that reads memory the PPSSPP HLE wrote
 * (untraced) would diverge; pure-compute functions reproduce bit-for-bit.
 *
 * Usage: funcdiff <image.bin> <base-hex> <ref-trace> <target-pc-hex> <entry-step> <out-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sr_register_all(void);

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f); *out_len = n; return d;
}

/* Apply one "name=value" token (a register or memory write) to the guest state. */
static void apply_tok(CpuState *s, char *tok) {
    char *eq = strchr(tok, '=');
    if (!eq) return;
    *eq = 0;
    const char *name = tok, *vs = eq + 1;
    if (name[0] == 'm') {
        /* m<size>[<addr>]=<val>, e.g. m32[0x09fffe84]=0xdeadbeef */
        int size = atoi(name + 1);
        char *lb = strchr(name, '[');
        if (!lb) return;
        uint32_t addr = (uint32_t)strtoul(lb + 1, NULL, 16);
        uint32_t val = (uint32_t)strtoul(vs, NULL, 16);
        if (size == 8) MEM_W8(addr, val);
        else if (size == 16) MEM_W16(addr, val);
        else MEM_W32(addr, val);
        return;
    }
    uint32_t val = (uint32_t)strtoul(vs, NULL, 16);
    if (name[0] == 'r' && name[1] >= '0' && name[1] <= '9') s->r[atoi(name + 1)] = val;
    else if (name[0] == 'f' && name[1] >= '0' && name[1] <= '9') s->fi[atoi(name + 1)] = val;
    else if (name[0] == 'v' && name[1] >= '0' && name[1] <= '9') s->vi[atoi(name + 1)] = val;
    else if (!strcmp(name, "hi")) s->hi = val;
    else if (!strcmp(name, "lo")) s->lo = val;
    else if (!strcmp(name, "fcr31")) s->fcr31 = val;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: funcdiff <image.bin> <base-hex> <ref-trace> <target-pc-hex> <entry-step> <out-trace>\n");
        return 2;
    }
    long len;
    uint8_t *img = read_file(argv[1], &len);
    uint32_t base = (uint32_t)strtoul(argv[2], NULL, 16);
    const char *ref_trace = argv[3];
    uint32_t target = (uint32_t)strtoul(argv[4], NULL, 16);
    unsigned long long entry_step = strtoull(argv[5], NULL, 10);
    const char *out = argv[6];

    sr_mem_init();
    sr_load_segment(base, img, (uint32_t)len);
    sr_register_all();

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4; s.vfpuCtrl[1] = 0xe4; s.vfpuCtrl[2] = 0;

    FILE *f = fopen(ref_trace, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", ref_trace); return 2; }
    char *line = (char *)malloc(1 << 16);
    int reached = 0;

    while (fgets(line, 1 << 16, f)) {
        if (line[0] == '#') {
            if (!strncmp(line, "# init", 6)) {
                char *tok = strtok(line + 6, " \t\n");
                while (tok) { apply_tok(&s, tok); tok = strtok(NULL, " \t\n"); }
            }
            continue;
        }
        char *step_s = strtok(line, " \t\n");
        char *pc_s = strtok(NULL, " \t\n");
        char *op_s = strtok(NULL, " \t\n");
        if (!step_s || !pc_s || !op_s) continue;
        unsigned long long st = strtoull(step_s, NULL, 10);
        uint32_t pc = (uint32_t)strtoul(pc_s + 3, NULL, 16);
        if (st == entry_step && pc == target) { reached = 1; s.pc = pc; break; }
        /* Not yet at entry: apply this step's writes so guest state tracks the reference. */
        char *tok;
        while ((tok = strtok(NULL, " \t\n")) != NULL) apply_tok(&s, tok);
    }
    fclose(f);
    if (!reached) { fprintf(stderr, "did not reach target 0x%08x at step %llu\n", target, entry_step); return 2; }

    RecompFn fn = sr_lookup(target);
    if (!fn) { fprintf(stderr, "no recompiled function at 0x%08x\n", target); return 2; }
    if (sr_trace_open(out, "funcdiff", target) != 0) { fprintf(stderr, "cannot open %s\n", out); return 2; }
    if (setjmp(g_hle_jmp) == 0) fn(&s);
    sr_trace_close();
    fprintf(stderr, "ran 0x%08x from step %llu (hit_hle=%d)\n", target, entry_step, sr_hit_hle);
    return 0;
}
