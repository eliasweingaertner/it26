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
 *   - an SDL2 pixel backend (it_screen_sdl.c) giving the same authentic
 *     window on POSIX (Linux/macOS); compiled when the build defines
 *     HAVE_SDL (CMake option ITED_SDL + a found SDL2);
 *   - a VT/ANSI truecolor terminal backend with damage tracking and
 *     Unicode approximations of the custom glyphs (the no-deps fallback,
 *     and the only backend when neither pixel backend is built/available).
 *
 * Backend selection in Screen_Init: Win32 builds open the Win32 window
 * unless ITED_TERM=1. POSIX builds open the SDL window when HAVE_SDL is
 * defined, ITED_TERM is unset, and a display is available; otherwise the
 * terminal backend is used.
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

/* S_GenerateCharacters: the VGA 512-character trick. IT pixel-draws its
 * envelope / waveform / oscilloscope canvases by rendering into a pixel
 * "generation table" and regenerating font bank B from it; cells whose
 * foreground palette index has bit 3 set (attr & 0x08) display their
 * character from bank B instead of the normal font. `pix` is a
 * (wchars*8) x (hchars*8) byte array, one byte per pixel (bit 0 used),
 * row-major; characters fill left-to-right then top-to-bottom starting
 * at `first`. Shared by the rasterizer (pixel backends, BMP shots) and
 * approximated with quadrant blocks by the terminal backend. */
void Screen_GenerateCharacters(int first, int wchars, int hchars,
                               const uint8_t *pix);

/* S_DefineSmallNumbers: loads the info page's small-number charsets.
 * Font bank B char 0xXY becomes the hex pair X,Y (4 pixels each); font
 * A chars 226..245 become G0..G9,H0..H9. Called on entering pages that
 * use them (Glbl_F5/Glbl_F2 in the original); the font A part persists,
 * font bank B is reclaimed by the next Screen_GenerateCharacters. */
void Screen_DefineSmallNumbers(void);

/* OR bits into a cell's attribute byte (the original hilights the
 * playing row on the info page by Or-ing 0E0h over the drawn cells). */
void Screen_OrAttr(int x, int y, uint8_t bits);

/* read-modify-write helpers for cursor rules like the message
 * editor's `attr = (attr & 8) | 30h` (Msg_PreMessage). */
uint8_t Screen_GetAttr(int x, int y);
void Screen_SetAttr(int x, int y, uint8_t attr);

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
    ITK_SHIFT_F9,           /* message editor (Glbl_Shift_F9) */
    /* Alt/Ctrl modifier combos (Win32 backend; the terminal backend
     * doesn't produce these). ITK_ALT_A..Z and ITK_ALT_0..9 are
     * contiguous. */
    ITK_ALT_A = 0x200,      /* .. ITK_ALT_A + 25 = Alt-Z */
    ITK_ALT_0 = 0x220,      /* .. ITK_ALT_0 + 9  = Alt-9 */
    ITK_ALT_INS = 0x230, ITK_ALT_DEL, ITK_ALT_UP, ITK_ALT_DOWN,
    ITK_ALT_PLUS, ITK_ALT_MINUS, ITK_CTRL_PLUS, ITK_CTRL_MINUS,
    ITK_QUIT = 0x300,       /* window closed (pixel backend) */
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

#ifdef HAVE_SDL
extern const screen_backend_t Screen_BackendSDL;
#endif

#endif /* IT_SCREEN_H */
