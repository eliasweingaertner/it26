/*
 * it_screen.h
 * -----------
 * Cross-platform replacement for IT_DISPL.ASM/IT_S.ASM's output layer.
 * Impulse Tracker draws into an 80x50 text screen (VGA 8x8 font text
 * mode) of (char, attribute) cells; everything in IT_OBJ1.ASM/IT_PE.ASM
 * renders through S_DrawString/S_DrawBox-style calls into that buffer.
 *
 * This module provides the same model: an 80x50 cell framebuffer using
 * the original Camouflage palette, custom UI glyphs and box styles from
 * IT_S.ASM (see it_vgadata.h), presented by one of two backends behind
 * the same interface:
 *
 *   - a pixel backend (Win32 window, 640x400 logical pixels rendered
 *     with the real 8x8 glyph bitmaps, integer-scaled) -- the authentic
 *     look;
 *   - a VT/ANSI truecolor terminal backend with damage tracking and
 *     Unicode approximations of the custom glyphs (no-deps fallback,
 *     and the only backend on POSIX until an SDL backend lands).
 *
 * Backend selection in Screen_Init: Win32 builds open the window unless
 * ITED_TERM=1 is set; everything else uses the terminal.
 */

#ifndef IT_SCREEN_H
#define IT_SCREEN_H

#include <stdint.h>

#define SCREEN_W 80
#define SCREEN_H 50

int  Screen_Init(void);
void Screen_UnInit(void);
void Screen_Clear(uint8_t attr);

/* S_Draw* equivalents: x in [0,79], y in [0,49], attr = VGA-style
 * foreground|background<<4 using the IT palette indices. */
void Screen_PutChar(int x, int y, uint8_t ch, uint8_t attr);
void Screen_DrawString(int x, int y, const char *s, uint8_t attr);

/* S_DrawString with the original control codes (IT_S.ASM):
 *   0    end of string          13   next line (x back to start)
 *   0xFF n c  repeat char c n times
 *   0xFE a    set attribute to a
 *   0xFD 'D'  print the next number from `nums` in decimal
 * `nums` may be NULL if the string contains no 0xFD codes. */
void Screen_DrawStringCtl(int x, int y, const uint8_t *s, uint8_t attr,
                          const int *nums);

/* S_DrawBox: draws box style `style` (IT_S.ASM BoxDefinitions) with
 * corners (x0,y0)-(x1,y1) inclusive. Add IT_BOX_NOFILL to leave the
 * interior untouched (high-byte flag, as in the original). */
#define IT_BOX_NOFILL 0x100
void Screen_DrawBox(int x0, int y0, int x1, int y1, int style);

/* flush damaged cells to the active backend (S_UpdateScreen) */
void Screen_Update(void);

/* render the current cell buffer to `px` as 640x400 0x00RRGGBB pixels
 * using the real glyph bitmaps + palette (shared by the pixel backend,
 * Screen_WriteBMP and any future SDL backend). */
void Screen_Rasterize(uint32_t *px);

/* write the current cell buffer as a 640x400 24-bit BMP (for visual
 * verification without a window/terminal). Returns 1 on success. */
int Screen_WriteBMP(const char *path);

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
    ITK_ESC, ITK_ENTER, ITK_BACKSPACE, ITK_TAB, ITK_SHIFT_TAB,
    ITK_QUIT,               /* window closed (pixel backend) */
    ITK_MOUSE,              /* left button pressed; see Screen_GetMouse */
};
int Key_Get(void);          /* non-blocking, K_GetKey-style             */

/* ---- mouse (pixel backend; terminal backend reports no mouse) ----
 * x,y are cell coordinates (0..79, 0..49); px,py logical pixels
 * (0..639, 0..399) for the thumbbars' pixel-precise positioning, as in
 * the original's 8010h mouse events; b is bit 0 = left button held. */
typedef struct it_mouse_t {
    int x, y;
    int px, py;
    int b;
} it_mouse_t;
void Screen_GetMouse(it_mouse_t *m);

/* ---- internal: backend interface (it_screen.c / it_screen_win32.c) ---- */

typedef struct screen_cell_t {
    uint8_t ch;
    uint8_t attr;
} screen_cell_t;

typedef struct screen_backend_t {
    int  (*init)(void);
    void (*uninit)(void);
    /* present the full cell buffer; backend does its own damage tracking */
    void (*present)(const screen_cell_t *cells);
    int  (*key)(void);
    void (*mouse)(it_mouse_t *m);   /* NULL = no mouse support */
} screen_backend_t;

#ifdef _WIN32
extern const screen_backend_t Screen_BackendWin32;
#endif

#endif /* IT_SCREEN_H */
