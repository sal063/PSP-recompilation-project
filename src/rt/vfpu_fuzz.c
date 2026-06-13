/* Differential fuzzer: codegen-emitted VFPU C (build/acx/vfpu_fuzz_cases.h, produced by
 * tools/vfpu_fuzz_gen.py from every distinct VFPU compute word in the game ELF) versus the
 * trace-validated single-instruction interpreter sr_vfpu_interp. For each word it runs many
 * trials from identical randomized CPU states (v[], prefixes, VFPU_CC) and compares the full
 * v[] register file and vfpuCtrl bitwise. Any divergence is a codegen bug of the vcmov class.
 *
 * Usage: vfpu_fuzz [trials_per_case] [seed]
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../build/acx/vfpu_fuzz_cases.h"

static uint32_t s_rng = 0x12345678u;
static uint32_t rng(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static float rand_float(void) {
    switch (rng() % 8) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return 0.5f;
    default: {
        /* modest finite values; both sides share the float kernels, so exotic inputs
         * (inf/NaN) would only test code both paths inherit from the same helpers */
        int32_t m = (int32_t)(rng() % 4000) - 2000;
        return (float)m / 16.0f;
    }
    }
}

static uint32_t rand_sprefix(void) {
    if (rng() % 2) return 0xe4;                 /* identity half the time */
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) p |= (rng() & 3u) << (i * 2);   /* swizzle */
    p |= (rng() & 0xFu) << 8;                   /* abs bits */
    if (rng() % 4 == 0) p |= (rng() & 0xFu) << 12;  /* constant bits */
    p |= (rng() & 0xFu) << 16;                  /* negate bits */
    return p;
}

static uint32_t rand_dprefix(void) {
    if (rng() % 2) return 0;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sat = rng() & 3u;
        if (sat == 2) sat = 0;                  /* 2 is reserved */
        p |= sat << (i * 2);
    }
    p |= (rng() & 0xFu) << 8;                   /* write mask */
    return p;
}

int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 200;
    if (argc > 2) s_rng = (uint32_t)strtoul(argv[2], NULL, 0);

    int tested = 0, skipped = 0, bad_cases = 0;
    unsigned long long mismatches = 0, total = 0;

    for (int c = 0; c < FUZZ_NCASES; c++) {
        uint32_t w = fuzz_cases[c].w;
        int case_bad = 0, interp_other = 0;
        for (int t = 0; t < trials; t++) {
            CpuState s0;
            memset(&s0, 0, sizeof(s0));
            for (int i = 0; i < 128; i++) s0.v[i] = rand_float();
            s0.vfpuCtrl[0] = rand_sprefix();
            s0.vfpuCtrl[1] = rand_sprefix();
            s0.vfpuCtrl[2] = rand_dprefix();
            s0.vfpuCtrl[3] = rng() & 0x3Fu;     /* VFPU_CC */

            /* vcrsp/vqmul and vrot have hardware-quirky prefix interactions that neither
             * side models (the game never prefixes them) — fuzz those identity-prefix only */
            uint32_t top = w >> 26, sub3 = (w >> 23) & 7;
            if (top == 0x3c && (sub3 == 5 || (sub3 == 7 && ((w >> 21) & 0x1F) == 29))) {
                s0.vfpuCtrl[0] = 0xe4; s0.vfpuCtrl[1] = 0xe4; s0.vfpuCtrl[2] = 0;
            }

            CpuState s1 = s0, s2 = s0;
            int kind = sr_vfpu_interp(&s2, w);
            if (kind == SR_VFPU_OTHER) { interp_other = 1; break; }
            /* SR_VFPU_STATE (vcmp/vpfx) still mutates vfpuCtrl — compare it like the rest */
            fuzz_run_codegen(&s1, c);
            total++;

            int bad = 0;
            for (int i = 0; i < 128; i++) {
                if (s1.vi[i] != s2.vi[i]) {
                    if (!bad && mismatches < 40)
                        fprintf(stderr,
                                "MISMATCH op=0x%08x (sample @0x%08x) trial %d: v%d codegen=0x%08x (%g) interp=0x%08x (%g) [pfx s=%05x t=%05x d=%03x cc=%02x]\n",
                                w, fuzz_cases[c].addr, t, i, s1.vi[i], s1.v[i], s2.vi[i], s2.v[i],
                                s0.vfpuCtrl[0], s0.vfpuCtrl[1], s0.vfpuCtrl[2], s0.vfpuCtrl[3]);
                    bad = 1;
                }
            }
            for (int i = 0; i < 4; i++) {
                if (s1.vfpuCtrl[i] != s2.vfpuCtrl[i]) {
                    if (!bad && mismatches < 40)
                        fprintf(stderr,
                                "MISMATCH op=0x%08x (sample @0x%08x) trial %d: vfpuCtrl[%d] codegen=0x%08x interp=0x%08x\n",
                                w, fuzz_cases[c].addr, t, i, s1.vfpuCtrl[i], s2.vfpuCtrl[i]);
                    bad = 1;
                }
            }
            if (bad) { mismatches++; case_bad = 1; }
        }
        if (interp_other) {
            skipped++;
            fprintf(stderr, "UNCOVERED op=0x%08x (sample @0x%08x): interp has no oracle for it\n",
                    w, fuzz_cases[c].addr);
            continue;
        }
        tested++;
        if (case_bad) bad_cases++;
    }

    printf("vfpu_fuzz: %d/%d distinct words tested (%d not covered by interp oracle), "
           "%d words diverge, %llu/%llu trials mismatched\n",
           tested, FUZZ_NCASES, skipped, bad_cases, mismatches, total);
    return bad_cases ? 1 : 0;
}
