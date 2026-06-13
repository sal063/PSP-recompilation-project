/* VFPU verifier driven by a real Ace Combat X oracle trace.
 *
 * Replays the trace maintaining the full CPU state (r, f, v, hi, lo, fcr31, and the VFPU
 * prefix vfpuCtrl, which is tracked from vpfx + the per-op eat since it is not traced). For
 * every VFPU compute instruction it recomputes the result from the tracked state with
 * sr_vfpu_interp and checks it against the value the oracle recorded. Loads/stores and
 * non-VFPU ops are taken from the trace (no memory is modelled). Reports the first divergence
 * with its pc/op, and a summary of how many VFPU ops were verified.
 *
 * Usage: acx_verify <oracle-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply_token(CpuState *s, const char *name, uint32_t val) {
    if (name[0] == 'r' && name[1] >= '0' && name[1] <= '9') s->r[atoi(name + 1)] = val;
    else if (name[0] == 'f' && name[1] >= '0' && name[1] <= '9') s->fi[atoi(name + 1)] = val;
    else if (name[0] == 'v' && name[1] >= '0' && name[1] <= '9') s->vi[atoi(name + 1)] = val;
    else if (!strcmp(name, "hi")) s->hi = val;
    else if (!strcmp(name, "lo")) s->lo = val;
    else if (!strcmp(name, "fcr31")) s->fcr31 = val;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: acx_verify <oracle-trace>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4; s.vfpuCtrl[1] = 0xe4; s.vfpuCtrl[2] = 0;

    char *line = (char *)malloc(1 << 16);
    unsigned long long verified = 0, vfpu_other = 0, mismatches = 0, steps = 0;

    while (fgets(line, 1 << 16, f)) {
        if (line[0] == '#') {
            if (!strncmp(line, "# init", 6)) {
                char *tok = strtok(line + 6, " \t\n");
                while (tok) {
                    char *eq = strchr(tok, '=');
                    if (eq) { *eq = 0; apply_token(&s, tok, (uint32_t)strtoul(eq + 1, NULL, 16)); }
                    tok = strtok(NULL, " \t\n");
                }
            }
            continue;
        }
        char *p = line;
        char *step_s = strtok(p, " \t\n");
        char *pc_s = strtok(NULL, " \t\n");
        char *op_s = strtok(NULL, " \t\n");
        if (!step_s || !pc_s || !op_s || strncmp(pc_s, "pc=", 3) || strncmp(op_s, "op=", 3))
            continue;
        steps++;
        uint32_t pc = (uint32_t)strtoul(pc_s + 3, NULL, 16);
        uint32_t op = (uint32_t)strtoul(op_s + 3, NULL, 16);

        /* Collect the recorded register changes for this step. */
        char names[64][16];
        uint32_t vals[64];
        int nch = 0;
        char *tok;
        while ((tok = strtok(NULL, " \t\n")) != NULL) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            if (tok[0] == 'm') continue;  /* memory write: not modelled */
            *eq = 0;
            if (nch < 64) { strncpy(names[nch], tok, 15); names[nch][15] = 0; vals[nch] = (uint32_t)strtoul(eq + 1, NULL, 16); nch++; }
        }

        /* Try to interpret as a VFPU op against the current (pre-step) state. */
        uint32_t backup_v[128], backup_f[32];
        memcpy(backup_v, s.vi, sizeof(backup_v));
        memcpy(backup_f, s.fi, sizeof(backup_f));
        int kind = sr_vfpu_interp(&s, op);

        if (kind == SR_VFPU_COMPUTE) {
            /* Build the oracle's expected v[] from the pre-step state + recorded changes. */
            uint32_t expect_v[128];
            memcpy(expect_v, backup_v, sizeof(expect_v));
            for (int i = 0; i < nch; i++)
                if (names[i][0] == 'v') expect_v[atoi(names[i] + 1)] = vals[i];
            int bad = 0;
            for (int i = 0; i < 128; i++) {
                if (s.vi[i] != expect_v[i]) {
                    if (!bad && mismatches < 10) {
                        fprintf(stderr, "MISMATCH step %s pc=0x%08x op=0x%08x: v%d got=0x%08x want=0x%08x\n",
                                step_s, pc, op, i, s.vi[i], expect_v[i]);
                    }
                    bad = 1;
                }
            }
            if (bad) mismatches++; else verified++;
            /* Restore and re-apply the oracle's exact changes to advance in lockstep. */
            memcpy(s.vi, backup_v, sizeof(backup_v));
            memcpy(s.fi, backup_f, sizeof(backup_f));
        } else if (kind == SR_VFPU_OTHER) {
            uint32_t mj = op >> 26;
            if (mj == 0x36 || mj == 0x3e) {
                vfpu_other++;  /* VFPU loads/stores: taken from the trace */
            } else if (mj == 0x18 || mj == 0x19 || mj == 0x1b ||
                       mj == 0x34 || mj == 0x3c || mj == 0x3d) {
                /* 0x1a is PPSSPP's internal emuhack (function replacement), not a real op. */
                /* An unhandled VFPU compute op. Record one sample word per opcode for triage. */
                static uint32_t seen[256];
                static int nseen = 0;
                int known = 0;
                for (int k = 0; k < nseen; k++) if (seen[k] == op) { known = 1; break; }
                if (!known && nseen < 256) {
                    seen[nseen++] = op;
                    fprintf(stderr, "UNHANDLED VFPU op=0x%08x (opcode 0x%02x sub %u (op>>16)&1f=0x%02x) first at step %s pc=0x%08x\n",
                            op, mj, (op >> 23) & 7, (op >> 16) & 0x1f, step_s, pc);
                }
            }
        }
        /* For STATE ops, keep the vfpuCtrl change. For all ops, apply the recorded changes. */
        for (int i = 0; i < nch; i++)
            apply_token(&s, names[i], vals[i]);
    }
    fclose(f);
    printf("steps=%llu  VFPU compute verified=%llu  mismatches=%llu  vfpu_loadstore=%llu\n",
           steps, verified, mismatches, vfpu_other);
    return mismatches ? 1 : 0;
}
