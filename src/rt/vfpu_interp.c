/* Single-instruction VFPU interpreter. Same semantics as the codegen (tools/codegen.py),
 * in callable form, used to verify the VFPU against reference trace data and as the
 * interpreter fallback (ARCHITECTURE.md section 3). Reuses the runtime prefix/transcendental
 * kernels in recomp.c.
 *
 * sr_vfpu_interp executes one VFPU compute or prefix instruction against the CpuState and
 * returns SR_VFPU_COMPUTE (a value-producing op, compare v[]/f[] against the oracle),
 * SR_VFPU_STATE (a prefix/control op, nothing to compare), or SR_VFPU_OTHER (not handled
 * here, e.g. a VFPU load/store that needs memory). */

#include "recomp.h"

#include <math.h>
#include <string.h>

#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2

static int vreg_idx(int reg, int size, uint8_t *out) {
    int mtx = (reg >> 2) & 7, col = reg & 3, transpose = (reg >> 5) & 1, row, length;
    if (size == 1) { transpose = 0; row = (reg >> 5) & 3; length = 1; }
    else if (size == 2) { row = (reg >> 5) & 2; length = 2; }
    else if (size == 3) { row = (reg >> 6) & 1; length = 3; }
    else { row = (reg >> 5) & 2; length = 4; }
    for (int i = 0; i < length; i++)
        out[i] = transpose ? (uint8_t)(mtx * 16 + ((row + i) & 3) * 4 + col)
                           : (uint8_t)(mtx * 16 + col * 4 + ((row + i) & 3));
    return length;
}

static int mreg_idx(int reg, int side, int j, int i) {
    int mtx = (reg >> 2) & 7, col = reg & 3, transpose = (reg >> 5) & 1, row;
    if (side == 1) { transpose = 0; row = (reg >> 5) & 3; }
    else if (side == 3) row = (reg >> 6) & 1;
    else row = (reg >> 5) & 2;
    return transpose ? mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3)
                     : mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3);
}

static int vsize(uint32_t w) { return (((w >> 7) & 1) | ((w >> 14) & 2)) + 1; }

static void eat_prefix(CpuState *s) {
    s->vfpuCtrl[0] = 0xe4u;
    s->vfpuCtrl[1] = 0xe4u;
    s->vfpuCtrl[2] = 0u;
}

/* vcst constant table, computed with the same float expressions as PPSSPP (MIPS.cpp). */
static float sr_cst[32];
static int sr_cst_init = 0;
static void sr_cst_load(void) {
    if (sr_cst_init) return;
    const double PI = 3.14159265358979323846, E = 2.71828182845904523536;
    const double LOG2E = 1.44269504088896340736, LOG10E = 0.43429448190325182765;
    const double LN2 = 0.69314718055994530942, LN10 = 2.30258509299404568402;
    sr_cst[0] = 0.0f;
    sr_cst[1] = 3.4028234663852886e38f;  /* FLT_MAX */
    sr_cst[2] = sqrtf(2.0f);
    sr_cst[3] = sqrtf(0.5f);
    sr_cst[4] = 2.0f / sqrtf((float)PI);
    sr_cst[5] = 2.0f / (float)PI;
    sr_cst[6] = 1.0f / (float)PI;
    sr_cst[7] = (float)PI / 4;
    sr_cst[8] = (float)PI / 2;
    sr_cst[9] = (float)PI;
    sr_cst[10] = (float)E;
    sr_cst[11] = (float)LOG2E;
    sr_cst[12] = (float)LOG10E;
    sr_cst[13] = (float)LN2;
    sr_cst[14] = (float)LN10;
    sr_cst[15] = 2 * (float)PI;
    sr_cst[16] = (float)PI / 6;
    sr_cst[17] = log10f(2.0f);
    sr_cst[18] = logf(10.0f) / logf(2.0f);
    sr_cst[19] = sqrtf(3.0f) / 2.0f;
    for (int i = 20; i < 32; i++) sr_cst[i] = 0.0f;
    sr_cst_init = 1;
}

int sr_vfpu_interp(CpuState *s, uint32_t w) {
    uint32_t op = w >> 26;

    if (op == 0x37) {  /* (w>>24)&3: 0/1/2 = vpfxs/vpfxt/vpfxd, 3 = viim/vfim */
        int rn = (w >> 24) & 3;
        if (rn == 3) {  /* viim: signed 16-bit int immediate; vfim: float16 immediate */
            int vt = (w >> 16) & 0x7F;
            uint32_t imm = w & 0xFFFF;
            float d[1];
            if ((w >> 23) & 1) {  /* vfim: IEEE half -> float32 */
                uint32_t sgn = (imm >> 15) & 1, e = (imm >> 10) & 0x1F, m = imm & 0x3FF;
                uint32_t bits;
                if (e == 0) {
                    if (m == 0) bits = sgn << 31;
                    else {
                        uint32_t e2 = 127 - 15 + 1;
                        while (!(m & 0x400)) { m <<= 1; e2--; }
                        bits = (sgn << 31) | (e2 << 23) | ((m & 0x3FF) << 13);
                    }
                } else if (e == 31) {
                    bits = (sgn << 31) | (0xFFu << 23) | (m << 13);
                } else {
                    bits = (sgn << 31) | ((e - 15 + 127) << 23) | (m << 13);
                }
                memcpy(d, &bits, 4);
            } else {
                d[0] = (float)(int16_t)imm;
            }
            uint8_t i0[1];
            vreg_idx(vt, 1, i0);
            sr_vwrite(s, i0, d, 1, s->vfpuCtrl[2]);
            eat_prefix(s);
            return SR_VFPU_COMPUTE;
        }
        uint32_t data = w & 0xFFFFF;
        if (rn == 2) data &= 0xFFF;
        s->vfpuCtrl[rn] = data;
        return SR_VFPU_STATE;
    }

    int n = vsize(w);
    int vd = w & 0x7F, vs = (w >> 8) & 0x7F, vt = (w >> 16) & 0x7F;
    uint8_t di[4], si[4], ti[4];

    if (op == 0x1b) {  /* VFPU3: vcmp (CC) / vmin / vmax */
        int s3 = (w >> 23) & 7;
        if (s3 == 0) {  /* vcmp: set the CC register, no v[] output */
            int cond = w & 0xF;
            float a[4], b[4];
            vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
            sr_vread(a, s, si, n, s->vfpuCtrl[0]);
            sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
            int cc = 0, orv = 0, andv = 1, affected = (1 << 4) | (1 << 5);
            for (int i = 0; i < n; i++) {
                float x = a[i], y = b[i];
                int c;
                switch (cond) {
                    case 0: c = 0; break;            case 1: c = x == y; break;
                    case 2: c = x < y; break;        case 3: c = x <= y; break;
                    case 4: c = 1; break;            case 5: c = x != y; break;
                    case 6: c = x >= y; break;       case 7: c = x > y; break;
                    case 8: c = x == 0.0f; break;    case 9: c = isnan(x); break;
                    case 10: c = isinf(x); break;    case 11: c = isnan(x) || isinf(x); break;
                    case 12: c = x != 0.0f; break;   case 13: c = !isnan(x); break;
                    case 14: c = !isinf(x); break;   default: c = !(isnan(x) || isinf(x)); break;
                }
                cc |= (c << i); orv |= c; andv &= c; affected |= 1 << i;
            }
            s->vfpuCtrl[3] = (s->vfpuCtrl[3] & ~affected) | ((cc | (orv << 4) | (andv << 5)) & affected);
            eat_prefix(s); return SR_VFPU_STATE;
        }
        if (s3 == 2 || s3 == 3) {  /* vmin / vmax (PPSSPP Int_Vminmax, with NaN/inf int compare) */
            float a[4], b[4], d[4];
            vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
            sr_vread(a, s, si, n, s->vfpuCtrl[0]);
            sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
            for (int i = 0; i < n; i++) {
                int an = isnan(a[i]) || isinf(a[i]), bn = isnan(b[i]) || isinf(b[i]);
                if (an || bn) {
                    int32_t ai, bi;
                    memcpy(&ai, &a[i], 4); memcpy(&bi, &b[i], 4);
                    int32_t r;
                    if (s3 == 2) r = (ai < 0 && bi < 0) ? (bi < ai ? ai : bi) : (ai < bi ? ai : bi);
                    else r = (ai < 0 && bi < 0) ? (ai < bi ? ai : bi) : (bi < ai ? ai : bi);
                    memcpy(&d[i], &r, 4);
                } else {
                    d[i] = s3 == 2 ? (a[i] < b[i] ? a[i] : b[i]) : (b[i] < a[i] ? a[i] : b[i]);
                }
            }
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        return SR_VFPU_OTHER;
    }

    if (op == 0x34) {
        /* Opcode 0x34 sub-dispatch by bits [25:21]: 3 = vcst, 0 = VV2Op, 16-23 = vf2i/vi2f
         * conversions (not handled here). */
        int sub21 = (w >> 21) & 0x1F;
        if (sub21 == 3) {  /* vcst: broadcast a constant */
            sr_cst_load();
            vreg_idx(vd, n, di);
            float d[4], c = sr_cst[(w >> 16) & 0x1F];
            for (int i = 0; i < n; i++) d[i] = c;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 == 21) {  /* vcmov: conditional move on the VFPU CC register */
            int tf = (w >> 19) & 1, imm3 = (w >> 16) & 7;
            float sv[4], d[4];
            vreg_idx(vs, n, si); vreg_idx(vd, n, di);
            sr_vread(sv, s, si, n, s->vfpuCtrl[0]);
            sr_vread(d, s, di, n, s->vfpuCtrl[1]);  /* vd is read as T */
            uint32_t cc = s->vfpuCtrl[3];
            if (imm3 < 6) {
                if ((int)((cc >> imm3) & 1) == !tf)
                    for (int i = 0; i < n; i++) d[i] = sv[i];
            } else if (imm3 == 6) {
                for (int i = 0; i < n; i++)
                    if ((int)((cc >> i) & 1) == !tf) d[i] = sv[i];
            }
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 == 2) {  /* VFPU9 group */
            int op9 = (w >> 16) & 0x1F;
            if (op9 == 4) {  /* vocp: d = 1 - s, computed as forced prefixes per PPSSPP:
                              * S gains negate-all, T is forced to constant ONE (user T abs
                              * may swap the constant, negate still applies). NaN stays
                              * positive. */
                float sv[4], tv[4], d[4];
                vreg_idx(vs, n, si);
                sr_vread(sv, s, si, n, s->vfpuCtrl[0] | (0xFu << 16));
                sr_vread(tv, s, si, n, (s->vfpuCtrl[1] & ~0xFFu) | 0x55u | (0xFu << 12));
                for (int i = 0; i < n; i++)
                    d[i] = isnan(sv[i]) ? fabsf(sv[i]) : tv[i] + sv[i];
                vreg_idx(vd, n, di);
                sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            return SR_VFPU_OTHER;
        }
        if (sub21 != 0)
            return SR_VFPU_OTHER;
        int optype = (w >> 16) & 0x1F;
        vreg_idx(vd, n, di);
        float d[4];
        if (optype == 3) {  /* vidt */
            int offmask = n >= 3 ? 3 : 1, off = vd & offmask;
            for (int i = 0; i < n; i++) d[i] = (i == off) ? 1.0f : 0.0f;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (optype == 6 || optype == 7) {  /* vzero / vone */
            for (int i = 0; i < n; i++) d[i] = optype == 6 ? 0.0f : 1.0f;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        float v[4];
        vreg_idx(vs, n, si);
        sr_vread(v, s, si, n, s->vfpuCtrl[0]);
        for (int i = 0; i < n; i++) {
            switch (optype) {
                case 0: d[i] = v[i]; break;
                case 1: d[i] = fabsf(v[i]); break;
                case 2: d[i] = -v[i]; break;
                case 4: d[i] = v[i] <= 0.0f ? 0.0f : (v[i] > 1.0f ? 1.0f : v[i]); break;
                case 5: d[i] = v[i] < -1.0f ? -1.0f : (v[i] > 1.0f ? 1.0f : v[i]); break;
                case 16: d[i] = sr_vfpu_rcp(v[i]); break;
                case 17: d[i] = sr_vfpu_rsqrt(v[i]); break;
                case 18: d[i] = sr_vfpu_sin(v[i]); break;
                case 19: d[i] = sr_vfpu_cos(v[i]); break;
                case 20: d[i] = sr_vfpu_exp2(v[i]); break;
                case 22: d[i] = sr_vfpu_sqrt(v[i]); break;
                case 24: d[i] = -sr_vfpu_rcp(v[i]); break;
                case 26: d[i] = -sr_vfpu_sin(v[i]); break;
                default: return SR_VFPU_OTHER;
            }
        }
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    int sub = (w >> 23) & 7;

    if ((op == 0x18 && (sub == 0 || sub == 1 || sub == 7)) || (op == 0x19 && sub == 0)) {
        float a[4], b[4], d[4];
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        for (int i = 0; i < n; i++)
            d[i] = op == 0x19 ? a[i] * b[i]
                  : sub == 0 ? a[i] + b[i] : sub == 1 ? a[i] - b[i] : a[i] / b[i];
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 1) {  /* vdot */
        float a[4], b[4], dd[1];
        vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        uint8_t dst[1]; vreg_idx(vd, 1, dst);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        float acc = 0.0f;
        for (int i = 0; i < n; i++) acc += a[i] * b[i];
        dd[0] = acc;
        sr_vwrite(s, dst, dd, 1, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 4) {  /* vhdp */
        float a[4], b[4], dd[1];
        vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        uint8_t dst[1]; vreg_idx(vd, 1, dst);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        float acc = 0.0f;
        for (int i = 0; i < n - 1; i++) acc += a[i] * b[i];
        acc += 1.0f * b[n - 1];
        dd[0] = isnan(acc) ? fabsf(acc) : acc;
        sr_vwrite(s, dst, dd, 1, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 5) {  /* vcrs */
        static const int ss[4] = {1, 2, 0, 3}, ts[4] = {2, 0, 1, 3};
        float a[4], b[4], d[4];
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        for (int i = 0; i < n; i++) d[i] = a[ss[i]] * b[ts[i]];
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 2) {  /* vscl */
        float a[4], d[4];
        uint8_t sc[1]; vreg_idx(vt, 1, sc);
        vreg_idx(vd, n, di); vreg_idx(vs, n, si);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        float scalar = s->v[sc[0]];
        for (int i = 0; i < n; i++) d[i] = a[i] * scalar;
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    if (op == 0x3c && sub == 0) {  /* vmmul */
        int side = n;
        float r[16];
        for (int a = 0; a < side; a++)
            for (int b = 0; b < side; b++) {
                float sum = 0.0f;
                for (int c = 0; c < side; c++)
                    sum += s->v[mreg_idx(vs, side, b, c)] * s->v[mreg_idx(vt, side, a, c)];
                r[a * 4 + b] = sum;
            }
        for (int a = 0; a < side; a++)
            for (int b = 0; b < side; b++)
                s->v[mreg_idx(vd, side, a, b)] = r[a * 4 + b];
        eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x3c && (sub == 1 || sub == 2 || sub == 3)) {  /* vtfm */
        int ins = sub, side = ins + 1, tn = n < ins + 1 ? n : ins + 1;
        vreg_idx(vt, side, ti); vreg_idx(vd, side, di);
        float r[4];
        for (int i = 0; i < side; i++) {
            float sum = 0.0f;
            for (int k = 0; k < tn; k++) sum += s->v[mreg_idx(vs, side, i, k)] * s->v[ti[k]];
            if (ins >= n) sum += s->v[mreg_idx(vs, side, i, ins)];
            r[i] = sum;
        }
        for (int i = 0; i < side; i++) s->v[di[i]] = r[i];
        eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x3c && sub == 7) {
        int idx = (w >> 21) & 0x1F;
        if (idx == 28) {  /* VFPUMatrix1: vmidt (3) / vmzero (6) / vmone (7) */
            int which = (w >> 16) & 0xF;
            if (which != 3 && which != 6 && which != 7) return SR_VFPU_OTHER;
            int side = n;
            for (int j = 0; j < side; j++)
                for (int i = 0; i < side; i++) {
                    float val = which == 3 ? (i == j ? 1.0f : 0.0f) : (which == 6 ? 0.0f : 1.0f);
                    s->v[mreg_idx(vd, side, j, i)] = val;
                }
            eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (idx == 29) {  /* vrot (PPSSPP Int_Vrot). Identity S/T prefixes assumed (ACX never
                           * prefixes it); includes the vd/vs same-register overlap quirk where
                           * the cosine is recomputed from the already-written sine lane. */
            int imm = (w >> 16) & 0x1F;
            int sl = (imm >> 2) & 3, cl = imm & 3;
            uint8_t ai[1]; vreg_idx(vs, 1, ai);
            float ang = s->v[ai[0]];
            float sine = sr_vfpu_sin(ang), cosine = sr_vfpu_cos(ang);
            if (imm & 0x10) sine = -sine;
            float d[4] = {0, 0, 0, 0};
            if (sl == cl) { for (int i = 0; i < n; i++) d[i] = sine; }
            else d[sl] = sine;
            d[cl] = cosine;
            if (((vd >> 2) & 7) == ((vs >> 2) & 7)) {
                /* dest overlaps the source matrix: if the angle register is one of the dest
                 * registers, hardware reads it back after the sine write (reg numbers, not
                 * physical indices, per PPSSPP GetVectorRegs comparison) */
                uint8_t dn[4];
                int mtx = (vd >> 2) & 7, col = vd & 3, row;
                if (n == 2) row = (vd >> 5) & 2;
                else if (n == 3) row = (vd >> 6) & 1;
                else row = (vd >> 5) & 2;
                int transpose = (vd >> 5) & 1;
                for (int i = 0; i < n; i++) {
                    int r = (mtx << 2);
                    if (transpose) r += (((row + i) & 3) << 5) | col;
                    else r += col | (((row + i) & 3) << 5);
                    dn[i] = (uint8_t)r;
                }
                for (int i = 0; i < n; i++)
                    if (vs == dn[i]) { d[cl] = sr_vfpu_cos(d[i]); break; }
            }
            vreg_idx(vd, n, di);
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        return SR_VFPU_OTHER;
    }
    if (op == 0x3c && sub == 5) {  /* vcrsp.t / vqmul.q (PPSSPP Int_CrossQuat, identity-prefix) */
        float a[4], b[4], d[4];
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        if (n == 3) {
            d[0] = a[1] * b[2] - a[2] * b[1];
            d[1] = a[2] * b[0] - a[0] * b[2];
            d[2] = a[0] * b[1] - a[1] * b[0];
        } else if (n == 4) {
            d[0] =  a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
            d[1] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
            d[2] =  a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
            d[3] = -a[0] * b[0] - a[1] * b[1] - a[2] * b[2] + a[3] * b[3];
        } else {
            return SR_VFPU_OTHER;
        }
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    return SR_VFPU_OTHER;
}
