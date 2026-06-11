/*
 * it_screen.h
 * -----------
 * Editor groundwork: a cross-platform replacement for IT_DISPL.ASM's
 * output layer. Impulse Tracker draws into an 80x50 text screen (VGA
 * 8x8 font text mode) of (char, attribute) cells; everything in
 * IT_OBJ1.ASM/IT_PE.ASM renders through S_DrawString/S_DrawBox-style
 * calls into that buffer.
 *
 * This module provides the same model: an 80x50 cell framebuffer with
 * the IT palette, rendered to a VT/ANSI terminal with damage tracking.
 * The editor screens can be ported on top of this without caring about
 * the platform (a pixel-accurate SDL backend with the original font can
 * be added behind the same interface later).
 */

#ifndef IT_SCREEN_H
#define IT_SCREEN_H

#include <stdint.h>

#define SCREEN_W 80
#define SCREEN_H 50

int  Screen_Init(void);     /* enters alt screen, hides cursor          */
void Screen_UnInit(void);
void Screen_Clear(uint8_t attr);

/* S_Draw* equivalents: x in [0,79], y in [0,49], attr = VGA-style
 * foreground|background<<4 using the IT palette indices. */
void Screen_PutChar(int x, int y, uint8_t ch, uint8_t attr);
void Screen_DrawString(int x, int y, const char *s, uint8_t attr);
void Screen_DrawBox(int x0, int y0, int x1, int y1, int style);

/* flush damaged cells to the terminal (S_UpdateScreen) */
void Screen_Update(void);

/* debug: write the current cell buffer as 50 lines of plain ASCII
 * (box glyphs approximated) to `fp`. Used by the editor self-test. */
void Screen_DumpPlain(void *fp);

/* ---- keyboard (IT_K.ASM replacement groundwork) ---- */

/* returns 0 if no key pending; otherwise an itkey below or an ASCII
 * character. Extended keys are reported with ITK_* codes. */
enum {
    ITK_NONE = 0,
    ITK_UP = 0x100, ITK_DOWN, ITK_LEFT, ITK_RIGHT,
    ITK_PGUP, ITK_PGDN, ITK_HOME, ITK_END, ITK_INS, ITK_DEL,
    ITK_F1, ITK_F2, ITK_F3, ITK_F4, ITK_F5, ITK_F6,
    ITK_F7, ITK_F8, ITK_F9, ITK_F10, ITK_F11, ITK_F12,
    ITK_ESC, ITK_ENTER, ITK_BACKSPACE, ITK_TAB,
};
int Key_Get(void);          /* non-blocking, K_GetKey-style             */

#endif /* IT_SCREEN_H */
