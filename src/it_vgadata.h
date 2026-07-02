/*
 * it_vgadata.h
 * ------------
 * Byte-exact display data from the original Impulse Tracker source
 * (IT_S.ASM) plus the IBM VGA ROM 8x8 font. Generated into
 * it_vgadata.c by tools/gen_vgadata.py.
 */

#ifndef IT_VGADATA_H
#define IT_VGADATA_H

#include <stdint.h>

/* Default "Camouflage" palette: 16 x (r,g,b), 6-bit VGA DAC (0..63). */
extern const uint8_t IT_PaletteDefs[48];

/* Custom UI glyphs for characters 128..201 (8x8, MSB = left pixel),
 * defined by S_InitScreen via S_RedefineCharacters. */
#define IT_CHARDEF_FIRST 128
#define IT_CHARDEF_COUNT 74
extern const uint8_t IT_CharDefs[IT_CHARDEF_COUNT][8];

/* Box styles (S_DrawBox): 9 x (char, attr) in the order
 * TL, top, TR, left, fill, right, BL, bottom, BR. */
#define IT_BOXSTYLE_COUNT 30
extern const uint8_t IT_BoxDefs[IT_BOXSTYLE_COUNT][18];

/* HexNumeralDefinitions: 4-pixel-wide numerals 0-9 A-F G H used by
 * S_DefineSmallNumbers to build the info page's small-number charsets
 * (font-B hex pairs, font-A chars 226..245 = G0..G9,H0..H9).  The
 * original '0' row has only 7 bytes so glyphs 1.. are shifted one byte
 * early and 'H' reads BoxDefinitions[0]=128 as its final row; the raw
 * layout is preserved (glyph n rows at [n*8-1..n*8+6] for n>=1). */
extern const uint8_t IT_HexNumerals[144];

/* The full CP437 8x8 font (chars 0..255) IT runs on top of. */
extern const uint8_t IT_FontROM[256][8];

#endif /* IT_VGADATA_H */
