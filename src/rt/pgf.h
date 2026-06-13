/* PGF firmware font reader/rasteriser (Phase 8 font support).
 *
 * A C port of PPSSPP's Core/Font/PGF.cpp, enough to serve sceLibFont for ACX: parse a PSP .pgf
 * firmware font, report per-character metrics, and rasterise real glyph bitmaps into the guest
 * buffer the game uploads as a GE texture. Replaces the earlier synthetic 5x7 fallback so the
 * full character set (Latin punctuation, symbols, CJK via jpn0.pgf) renders correctly.
 */
#ifndef SR_PGF_H
#define SR_PGF_H

#include <stdint.h>

typedef struct PGF PGF;

/* Load and parse a .pgf file from the host filesystem. Returns NULL on failure. */
PGF *pgf_open(const char *path);

/* True if the font has a real glyph for this Unicode code point. */
int pgf_has_char(const PGF *p, int charCode);

/* Write a PGFFontInfo (PPSSPP layout) to guest address `gfi`. */
void pgf_get_font_info(const PGF *p, uint32_t gfi);

/* Fill a PGFCharInfo (0x3c bytes, PPSSPP layout) at guest address `gci` for `charCode`, falling
 * back to `altCharCode`. Returns 1 if a glyph was found, 0 otherwise (charInfo zeroed). */
int pgf_get_char_info(const PGF *p, int charCode, int altCharCode, uint32_t gci);

/* Read a GlyphImage (PPSSPP layout) from guest address `ggi` and rasterise the glyph for
 * `charCode` (or `altCharCode`) into its buffer. Returns 1 if drawn. */
int pgf_draw_glyph(const PGF *p, int charCode, int altCharCode, uint32_t ggi);

#endif
