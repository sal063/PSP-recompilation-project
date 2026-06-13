/* In-engine asset viewer (SR_VIEWER=1).
 *
 * View Ace Combat X's assets by driving the game's OWN engine. The recompiled game boots
 * normally; once the engine subsystems (resource registry, GE, CDI) are up, this module takes
 * over the per-frame present hook (h_DisplaySetFrameBuf, on the render thread) and draws a
 * viewer instead of the game's menu.
 *
 * Resource registry (confirmed from sub_1C3ECC / sub_1C41C8 / sub_1B41B0 / sub_1C2EE4):
 *   g_Data = &unk_25CAE0 = guest 0x08A60AE0   (file 0x25CAE0 + base 0x08804000)
 *   g_Data+12 -> m_pInfo; m_pInfo+4 = int16 entry count
 *   entries at g_Data+16, stride 24: { char name[16]; u32 a; u32 b }
 *     - normal asset (.PRM/.MES/...):  a = load address, b = size
 *     - texture (.PDW/.IMG):           a = engine texture id, b = load address
 *
 * Engine functions (file IDB addr + guest = file + 0x08804000):
 *   GetAddrName   sub_1C420C  guest 0x089C820C   (a0=&g_Data, a1=name) -> entry ptr
 *   ModelLookup   sub_1B41B0  guest 0x089B81B0   (a0=name) -> model/texture handle
 *   LoadFileAsync sub_1C4DBC  guest 0x089C8DBC   (a0=&g_Data, a1=name, a2=wait)
 *
 * Phase: texture browser (.PDW). Decode uses the engine's own ge.c sampler via
 * ge_decode_tex_rgba(); PDW layout per scratch/asset_survey.md + FORMATS_RE.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"
#include "ge_shared.h"

/* ---- guest addresses ------------------------------------------------------------------- */
#define VW_G_DATA          0x08A60AE0u   /* &unk_25CAE0 */
#define FN_GetAddrName     0x089C820Cu
#define FN_ModelLookup     0x089B81B0u
#define FN_LoadFileAsync   0x089C8DBCu

#define PDW_MAGIC          0x2E574450u   /* "PDW." little-endian */

/* PSP sceCtrl button mask (matches gui.c read_pad / hle.c). */
#define BTN_UP     0x0010u
#define BTN_RIGHT  0x0020u
#define BTN_DOWN   0x0040u
#define BTN_LEFT   0x0080u
#define BTN_LTRIG  0x0100u
#define BTN_RTRIG  0x0200u
#define BTN_TRI    0x1000u
#define BTN_CIRCLE 0x2000u
#define BTN_CROSS  0x4000u
#define BTN_SQUARE 0x8000u

#define VW_W 480
#define VW_H 272

/* ---- state ----------------------------------------------------------------------------- */
static int      s_enabled = -1;
static unsigned s_frames  = 0;
static int      s_ready   = 0;
static int      s_logged  = 0;
static uint32_t s_prev_btn = 0;

static int      s_pdw_list[512];   /* registry indices of resident .PDW assets */
static int      s_pdw_count = 0;
static int      s_sel = 0;         /* index into s_pdw_list */
static int      s_sub = 0;         /* sub-texture within the selected PDW */

static uint32_t s_decode[1024 * 1024];   /* RGBA decode scratch (<=1024x1024) */
static uint32_t s_screen[VW_W * VW_H];

int viewer_active(void) {
    if (s_enabled < 0) s_enabled = getenv("SR_VIEWER") ? 1 : 0;
    return s_enabled;
}

/* ---- registry helpers ------------------------------------------------------------------ */
static uint32_t reg_count(void) {
    uint32_t hdr = MEM_R32(VW_G_DATA + 12);
    if (!hdr) return 0;
    uint32_t c = (uint32_t)(uint16_t)MEM_R16(hdr + 4);
    return c > 4096 ? 0 : c;
}
static uint32_t reg_entry(uint32_t i) { return VW_G_DATA + 16 + 24 * i; }
static void reg_name(uint32_t i, char out[17]) {
    uint32_t e = reg_entry(i);
    for (int k = 0; k < 16; ++k) out[k] = (char)MEM_R8(e + (uint32_t)k);
    out[16] = 0;
}
static int name_ends(const char *nm, const char *suffix) {
    size_t n = strlen(nm), s = strlen(suffix);
    return n >= s && _stricmp(nm + (n - s), suffix) == 0;
}

/* PDW base address for a registry texture entry: a=+16 (texid), b=+20 (load addr); but be
 * robust and accept whichever field actually points at a "PDW." header. */
static uint32_t pdw_addr_of(uint32_t e) {
    uint32_t a = MEM_R32(e + 16), b = MEM_R32(e + 20);
    if (MEM_R32(b) == PDW_MAGIC) return b;
    if (MEM_R32(a) == PDW_MAGIC) return a;
    return 0;
}

static void rebuild_pdw_list(void) {
    s_pdw_count = 0;
    uint32_t n = reg_count();
    for (uint32_t i = 0; i < n && s_pdw_count < (int)(sizeof(s_pdw_list)/sizeof(s_pdw_list[0])); ++i) {
        char nm[17]; reg_name(i, nm);
        if (nm[0] && name_ends(nm, ".PDW") && pdw_addr_of(reg_entry(i)))
            s_pdw_list[s_pdw_count++] = (int)i;
    }
}

/* ---- PDW decode ------------------------------------------------------------------------ */
/* Decode sub-texture `sub` of the PDW at guest `pdw` into s_decode; returns 1 and sets w,h. */
static int decode_pdw_sub(uint32_t pdw, int sub, int *out_w, int *out_h) {
    if (MEM_R32(pdw) != PDW_MAGIC) return 0;
    uint32_t count = MEM_R32(pdw + 8);
    if (sub < 0 || (uint32_t)sub >= count) return 0;
    uint32_t desc = pdw + MEM_R32(pdw + 0x10 + 4u * (uint32_t)sub);

    uint32_t texel_size = MEM_R32(desc + 0);
    int w   = (int)(uint16_t)MEM_R16(desc + 4);
    int h   = (int)(uint16_t)MEM_R16(desc + 6);
    int fmt = MEM_R8(desc + 8);          /* 0 = T4 (4bpp), 1 = T8 (8bpp) */
    int swz = MEM_R8(desc + 9);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    uint32_t texel_addr = desc + 0x10;
    uint32_t clut_addr  = desc + 0x10 + texel_size;   /* RGBA8888 CLUT follows the texels */

    GeState *ge = ge_state_ptr();
    ge->tex_w = w; ge->tex_h = h; ge->tex_bufw = (uint32_t)w;
    ge->tex_fmt = (fmt == 0) ? 4u : 5u;               /* PSM_T4 / PSM_T8 */
    ge->tex_swizzle = swz ? 1 : 0;
    ge->tex_addr = texel_addr;
    ge->tex_wrap = 0;
    /* clut_fmt: entry PSM 3 (8888), shift 0, mask 0xFF, start 0 -> direct index. */
    ge->clut_fmt = 0x0000FF03u;
    int nent = (fmt == 0) ? 16 : 256;
    for (int i = 0; i < nent; ++i) {
        uint32_t c = MEM_R32(clut_addr + 4u * (uint32_t)i);
        memcpy(&ge->clutram[(i * 4) & 2047], &c, 4);
    }
    ge_decode_tex_rgba(s_decode);
    *out_w = w; *out_h = h;
    return 1;
}

/* Blit decoded RGBA (w x h) into the 480x272 XRGB screen, aspect-fit, on a checkerboard. */
static void blit_fit(int w, int h) {
    for (int y = 0; y < VW_H; ++y)
        for (int x = 0; x < VW_W; ++x) {
            int c = ((x >> 3) ^ (y >> 3)) & 1;
            s_screen[y * VW_W + x] = c ? 0x00303030u : 0x00202020u;
        }
    float sx = (float)VW_W / (float)w, sy = (float)VW_H / (float)h;
    float sc = sx < sy ? sx : sy;
    if (sc > 8.0f) sc = 8.0f;
    int dw = (int)(w * sc), dh = (int)(h * sc);
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    int ox = (VW_W - dw) / 2, oy = (VW_H - dh) / 2;
    for (int dy = 0; dy < dh; ++dy)
        for (int dx = 0; dx < dw; ++dx) {
            int srcx = (int)(dx / sc); if (srcx >= w) srcx = w - 1;
            int srcy = (int)(dy / sc); if (srcy >= h) srcy = h - 1;
            uint32_t p = s_decode[srcy * w + srcx];          /* r|g<<8|b<<16|a<<24 */
            uint32_t xrgb = ((p & 0xFF) << 16) | (((p >> 8) & 0xFF) << 8) | ((p >> 16) & 0xFF);
            s_screen[(oy + dy) * VW_W + (ox + dx)] = xrgb;
        }
}

static void fill_screen(uint32_t col) {
    for (int i = 0; i < VW_W * VW_H; ++i) s_screen[i] = col;
}

/* ---- input ----------------------------------------------------------------------------- */
static void handle_input(void) {
    uint32_t b = gui_buttons();
    uint32_t e = b & ~s_prev_btn;   /* freshly-pressed edges */
    s_prev_btn = b;
    if (s_pdw_count == 0) return;

    if (e & BTN_RIGHT) { s_sel = (s_sel + 1) % s_pdw_count; s_sub = 0; }
    if (e & BTN_LEFT)  { s_sel = (s_sel - 1 + s_pdw_count) % s_pdw_count; s_sub = 0; }
    if (e & (BTN_UP | BTN_DOWN | BTN_RTRIG | BTN_LTRIG)) {
        uint32_t e0 = reg_entry((uint32_t)s_pdw_list[s_sel]);
        uint32_t pdw = pdw_addr_of(e0);
        int n = pdw ? (int)MEM_R32(pdw + 8) : 1;
        if (n < 1) n = 1;
        if (e & (BTN_DOWN | BTN_RTRIG)) s_sub = (s_sub + 1) % n;
        if (e & (BTN_UP   | BTN_LTRIG)) s_sub = (s_sub - 1 + n) % n;
    }
}

/* ---- frame ----------------------------------------------------------------------------- */
static void log_once(void) {
    if (s_logged) return;
    s_logged = 1;
    fprintf(stderr, "[viewer] ready: %u registry entries, %d .PDW textures\n",
            reg_count(), s_pdw_count);
    for (int i = 0; i < s_pdw_count && i < 40; ++i) {
        char nm[17]; reg_name((uint32_t)s_pdw_list[i], nm);
        uint32_t pdw = pdw_addr_of(reg_entry((uint32_t)s_pdw_list[i]));
        fprintf(stderr, "[viewer]   PDW[%d] %-16s @0x%08x subtex=%u\n",
                i, nm, pdw, pdw ? MEM_R32(pdw + 8) : 0);
    }
    fprintf(stderr, "[viewer] controls: LEFT/RIGHT=asset, UP/DOWN=sub-texture\n");
    fflush(stderr);
}

/* Returns 1 if the viewer presented this frame (caller skips the normal present). */
int viewer_on_present(CpuState *s) {
    (void)s;
    if (!viewer_active()) return 0;
    s_frames++;
    if (!s_ready) {
        if (s_frames < 120 || reg_count() == 0) return 0;   /* wait for engine to settle */
        s_ready = 1;
        rebuild_pdw_list();
        log_once();
    }

    rebuild_pdw_list();   /* cheap; keeps up with on-demand loads */
    handle_input();

    if (s_pdw_count == 0) { fill_screen(0x00400000u); return gui_present_rgba(s_screen); }
    if (s_sel >= s_pdw_count) s_sel = 0;

    uint32_t e = reg_entry((uint32_t)s_pdw_list[s_sel]);
    uint32_t pdw = pdw_addr_of(e);
    int w = 0, h = 0;
    if (pdw && decode_pdw_sub(pdw, s_sub, &w, &h)) blit_fit(w, h);
    else fill_screen(0x00000040u);   /* decode failed: dark blue */

    return gui_present_rgba(s_screen);
}
