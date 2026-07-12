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

/* S_DefineHiASCII: load font bank B with the plain CP437 ROM font so
 * attr-bit-3 text shows real high-ASCII (message editor; feature 013).
 * The next Screen_GenerateCharacters / Screen_DefineSmallNumbers
 * reclaims bank B, as in the original. */
void Screen_DefineHiASCII(void);

/* S_InvertCursor: redefine font-A char 246 as the glyph at (x,y) with
 * the masked pixel columns inverted and show it there in attr 30h (the
 * packed-cell pattern cursor; feature 010). */
void Screen_InvertCursor(int x, int y, uint8_t mask);

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
    /* Alt/Ctrl modifier combos (all backends since feature 011; the
     * POSIX terminal decodes the xterm ESC-prefix/CSI encodings, the
     * Windows console its conio scan codes). ITK_ALT_A..Z and
     * ITK_ALT_0..9 are contiguous. */
    ITK_ALT_A = 0x200,      /* .. ITK_ALT_A + 25 = Alt-Z */
    ITK_ALT_0 = 0x220,      /* .. ITK_ALT_0 + 9  = Alt-9 */
    ITK_ALT_INS = 0x230, ITK_ALT_DEL, ITK_ALT_UP, ITK_ALT_DOWN,
    ITK_ALT_PLUS, ITK_ALT_MINUS, ITK_CTRL_PLUS, ITK_CTRL_MINUS,
    /* feature 009 (pattern editing depth) */
    ITK_CTRL_UP = 0x240, ITK_CTRL_DOWN, ITK_CTRL_LEFT, ITK_CTRL_RIGHT,
    ITK_CTRL_HOME, ITK_CTRL_END, ITK_CTRL_PGUP, ITK_CTRL_PGDN,
    ITK_CTRL_INS, ITK_CTRL_DEL, ITK_CTRL_BACKSPACE, ITK_SCROLL_LOCK,
    ITK_ALT_F9, ITK_ALT_F10, ITK_CTRL_F7, ITK_CTRL_F2,
    /* feature 010: Alt-'\' (UnmuteAll, 12Bh) and the keypad slash
     * (MuteNext, scan 135h -- distinct from the free main-row '/') */
    ITK_ALT_BACKSLASH = 0x238, ITK_KP_DIVIDE,
    ITK_SHIFT_UP = 0x250, ITK_SHIFT_DOWN, ITK_SHIFT_LEFT,
    ITK_SHIFT_RIGHT, ITK_SHIFT_PGUP, ITK_SHIFT_PGDN,
    ITK_SHIFT_HOME, ITK_SHIFT_END,
    ITK_CTRL_0 = 0x260,     /* .. ITK_CTRL_0 + 5 = Ctrl-5 */
    ITK_CTRL_SHIFT_1 = 0x268, /* .. +3 = Ctrl-Shift-4 */
    /* plain Shift press/release events (IT_PE.ASM scan 2Ah/36h
     * handlers; drive F2 shift-marking). Pixel backends only -- a
     * terminal has no key-up events (feature 011 limitation). */
    ITK_SHIFT_PRESS = 0x270, ITK_SHIFT_RELEASE,
    ITK_QUIT = 0x300,       /* window closed (pixel backend) */
    ITK_MOUSE,              /* left button pressed; see Screen_GetMouse */
};
int Key_Get(void);          /* non-blocking, K_GetKey-style             */

/* ---- mouse (pixel backends + POSIX terminal via SGR reporting;
 * the Windows console path reports none) ----
 * x,y are cell coordinates (0..79, 0..49); px,py logical pixels
 * (0..639, 0..399) for the thumbbars' pixel-precise positioning, as in
 * the original's 8010h mouse events (the terminal approximates px/py
 * to the cell centre); b is bit 0 = left button held. */
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

/* read back one cell of the draw buffer (PE_HilightCursor-style
 * attribute rewrites; feature 009) */
screen_cell_t Screen_GetCell(int x, int y);

/* test hooks for the terminal input parser (feature 011; selftest):
 * feed raw bytes, optionally resolve a pending lone ESC, pop one
 * decoded key per call (ITK_NONE when drained); read the parser's
 * mouse mirror. Platform-neutral -- works without any tty. */
int  Screen_TermFeedTest(const uint8_t *buf, int n, int flush);
void Screen_TermMouseTest(it_mouse_t *m);

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
