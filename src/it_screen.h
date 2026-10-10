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

#include <stddef.h>
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

/* feature 013 (Alt-F12 analyser): while `pix` (640x400 8-bit indices)
 * is set, the rasterizer presents it through `pal6` (256 x 3 6-bit
 * VGA DAC values) instead of the cell buffer -- the port's stand-in
 * for the original's VESA mode switch. Screen_SetOverlay(NULL, NULL)
 * restores the text screen. Pixel backends + BMP shots only; the
 * terminal backend keeps showing the cell buffer. */
void Screen_SetOverlay(const uint8_t *pix, const uint8_t *pal6);

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
    /* feature 014: the original's Ctrl-F1 keypress table, the screen
     * Keyboard/DE.ASM's header points at for reading key codes.
     * Pixel backends only -- it is the backends that have real
     * scancodes to show. */
    ITK_CTRL_F3 = 0x259,    /* feature 015: Sample Library (explicit:
                             * the gap 0x259..0x25F is free) */
    ITK_CTRL_F1 = 0x258,    /* explicit: the implicit successor of
                             * ITK_CTRL_F2 (0x24F) would be 0x250,
                             * which is ITK_SHIFT_UP */
    /* feature 010: Alt-'\' (UnmuteAll, 12Bh) and the keypad slash
     * (keypad '/', E0 35 -> 1B5h = DecreaseOctave in the global key
     * list; MuteNext is the MAIN-ROW '/' position 135h, IT_PE.ASM:801);
     * feature 013: Alt-F12 (spectrum analyser, scan 158h) */
    ITK_ALT_BACKSLASH = 0x238, ITK_KP_DIVIDE, ITK_ALT_F12,
    ITK_SHIFT_UP = 0x250, ITK_SHIFT_DOWN, ITK_SHIFT_LEFT,
    ITK_SHIFT_RIGHT, ITK_SHIFT_PGUP, ITK_SHIFT_PGDN,
    ITK_SHIFT_HOME, ITK_SHIFT_END,
    ITK_CTRL_0 = 0x260,     /* .. ITK_CTRL_0 + 5 = Ctrl-5 */
    ITK_CTRL_SHIFT_1 = 0x268, /* .. +3 = Ctrl-Shift-4 */
    /* plain Shift press/release events (IT_PE.ASM scan 2Ah/36h
     * handlers; drive F2 shift-marking). Pixel backends only -- a
     * terminal has no key-up events (feature 011 limitation). */
    ITK_SHIFT_PRESS = 0x270, ITK_SHIFT_RELEASE,
    /* hotkey audit (2026-09): the rest of the original's key tables --
     * Glbl_Alt_F1..F8 (channel toggles), Alt-F11 (order lock),
     * Shift-F6, Ctrl-F4/F5/F6, the pattern editor's Alt-Left/Right/
     * Home/End/Enter/Backspace, Shift-grey +/- (14Eh/14Ah), keypad '*'
     * (137h, IncreaseOctave) and Right-Ctrl+Enter (111Ch). */
    ITK_ALT_F1 = 0x280,     /* .. ITK_ALT_F1 + 7 = Alt-F8 */
    ITK_ALT_F11 = 0x288, ITK_SHIFT_F6, ITK_CTRL_F4, ITK_CTRL_F5,
    ITK_CTRL_F6, ITK_ALT_LEFT, ITK_ALT_RIGHT, ITK_ALT_HOME, ITK_ALT_END,
    ITK_ALT_ENTER, ITK_ALT_BACKSPACE, ITK_KP_MULTIPLY, ITK_SHIFT_PLUS,
    ITK_SHIFT_MINUS, ITK_RCTRL_ENTER,
    ITK_SHIFT_F5,           /* Glbl_DriverScreen: the Miniaudio Driver
                               screen (extension) */
    /* feature 016 (issue #26): system file dialogs -- extensions. IT 2.14
     * binds neither key (type-0 F9/F10 match only without modifiers, and
     * no Ctrl-F9/F10 entry exists for Ctrl-Shift to fall back to). */
    ITK_CTRL_SHIFT_F9 = 0x298,  /* open module (system dialog) */
    ITK_CTRL_SHIFT_F10,         /* save module as (system dialog) */
    ITK_QUIT = 0x300,       /* window closed (pixel backend) */
    ITK_MOUSE,              /* left button pressed; see Screen_GetMouse */
};
int Key_Get(void);          /* non-blocking, K_GetKey-style             */

/* Alt-Enter is a host key (fullscreen) everywhere except the pattern
 * editor, where IT binds it to PEFunction_StoreCurrentPattern. The
 * editor sets this while the pattern editor is on screen; the pixel
 * backends then deliver ITK_ALT_ENTER instead of toggling fullscreen. */
extern int Screen_AltEnterIsKey;

/* macOS: set by the editor while the pattern editor is shown; the SDL
 * backend then treats Right Option (alone) as the held note-preview key
 * (IT's Caps Lock) instead of Alt. Ignored on other platforms. */
extern int Screen_RightOptPreview;

/* ---- the two-layer key event (feature 014) ----
 * K_GetKey returns CX/DX = input/translated (IT_K.ASM:1108): the raw
 * physical key in CX, the layout-translated character in DX. Consumers
 * pick one. Note entry reads the position only (IT_I.ASM:1344 compares
 * `BL, CL` -- the scancode low byte), so the tracker rows sit at fixed
 * physical places on every keyboard layout; text fields read the
 * character only, which is where national characters come from.
 *
 * `scan` is the original's CL: a PC set-1 scancode, +80h for the
 * E0-extended variant (so right Ctrl = 9Dh, right Alt = B8h, exactly
 * as IT_K.ASM:1216..1250 probes them). 0 = the source cannot supply a
 * position (terminal backend; see Key_ReverseScan).
 * `flags` is the original's CH, same bit assignment.
 * `ch` is CP437, never Unicode -- conversion happens in the backend.
 * `code` is the legacy value Key_Get() has always returned. */
enum {
    ITKF_PRESSED = 1,       /* clear = key release                      */
    ITKF_LSHIFT  = 2,
    ITKF_RSHIFT  = 4,
    ITKF_LCTRL   = 8,
    ITKF_RCTRL   = 16,
    ITKF_LALT    = 32,
    ITKF_RALT    = 64,      /* AltGr                                    */
    ITKF_CAPSDOWN = 128,    /* Caps Lock key held (K_IsKeyDown 3Ah: the
                               pattern editor's note preview); pixel
                               backends only */
    ITKF_SHIFT   = ITKF_LSHIFT | ITKF_RSHIFT,
    ITKF_CTRL    = ITKF_LCTRL  | ITKF_RCTRL,
    ITKF_ALT     = ITKF_LALT   | ITKF_RALT
};

typedef struct it_key_t {
    uint8_t  scan;          /* PC set-1 scancode, +80h = E0-extended    */
    uint8_t  flags;         /* ITKF_* -- the original's CH              */
    uint16_t ch;            /* CP437 character, 0 = none                */
    int      code;          /* legacy: ASCII char or ITK_* value        */
} it_key_t;

/* Non-blocking. Returns 0 when nothing is pending, else 1 and fills *k.
 * Key_Get() is exactly this, returning .code. */
int Key_GetEvent(it_key_t *k);

/* Unicode -> CP437; 0 = the character has no CP437 code and the
 * keystroke must be rejected (the module format cannot carry it). */
uint16_t Screen_UnicodeToCP437(uint32_t u);

/* Character -> US set-1 scancode; 0 = unknown. For backends that report
 * characters but no physical position (the terminal). */
uint8_t Key_ReverseScan(uint16_t ch);

/* Optional layout override in the original's KEYBOARD.CFG format (the
 * files IT shipped load unchanged). Affects the CHARACTER half only --
 * note entry never consults it. NULL/empty path clears any override and
 * returns to the host layout. Returns NULL on success, else a static
 * reason string; on failure the host layout stays in effect and the
 * caller is expected to surface the reason rather than fail startup. */
const char *Key_LoadLayout(const char *path);
const char *Key_LayoutName(void);   /* "" when no override is loaded */

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

/* #28: IT's MouseToggle (Ctrl-M) and Refresh (Ctrl-E) */
void Screen_SetMouseVisible(int show);
int  Screen_MouseVisible(void);
void Screen_Refresh(void);      /* next Screen_Update repaints everything */

/* ---- system file dialogs (feature 016, issue #26) ----
 * An extension next to IT's own file screens: the host's open / save /
 * folder dialog. The result carries the path twice -- `path` for the C
 * library's file calls (never cut or altered), `display` for the screen
 * (CP437, '?' for each character CP437 cannot show). */
enum {
    IT_DLG_OPEN_MODULE, IT_DLG_SAVE_MODULE, IT_DLG_OPEN_SAMPLE,
    IT_DLG_OPEN_INSTRUMENT, IT_DLG_PICK_FOLDER
};
enum { IT_DLG_CHOSEN, IT_DLG_CANCELLED, IT_DLG_UNAVAILABLE, IT_DLG_REJECTED };
#define IT_DLG_PATH_MAX 1024

typedef struct it_dialog_req_t {
    int         kind;           /* IT_DLG_OPEN_MODULE ...                   */
    const char *start_dir;      /* NULL/"" or missing = working directory   */
    const char *suggest_name;   /* save: pre-filled file name               */
    int         save_format;    /* save: 0 = IT, 1 = S3M pre-selected       */
} it_dialog_req_t;

typedef struct it_dialog_res_t {
    int         status;         /* IT_DLG_CHOSEN ...                        */
    char        path[IT_DLG_PATH_MAX];
    char        display[IT_DLG_PATH_MAX];
    int         lossy;          /* display has a '?' substitution           */
    int         save_format;    /* save: 0 = IT, 1 = S3M (type/extension)   */
    const char *reason;         /* status-line text for REJECTED/UNAVAILABLE */
} it_dialog_res_t;

/* Blocks while the dialog is open (audio keeps running on its own
 * thread). Test hook: ITED_DIALOG_FAKE = "!cancel" | "!unavailable" |
 * <path> answers without any UI. Returns res->status. */
int  Screen_FileDialog(const it_dialog_req_t *req, it_dialog_res_t *res);
int  Screen_HasFileDialog(void);        /* backend dialog or fake hook   */
/* label of the held note-preview key where it is not Caps Lock (macOS:
 * "Right Option"); NULL = Caps Lock, IT's own help text applies */
const char *Screen_PreviewKeyLabel(void);
/* "Cmd" where the host has its own shorter dialog keys (macOS: Cmd-F9,
 * Cmd-F10, Cmd-O next to Ctrl-Shift-F9/F10, Ctrl-O); NULL elsewhere */
const char *Screen_DialogModLabel(void);
/* UTF-8 -> CP437 for display, '?' per unmappable character; returns 1
 * if anything was substituted */
int  Screen_Utf8ToCP437Display(const char *utf8, char *out, size_t cap);
/* 1 = S3M for a ".s3m" name (any case), else 0 = IT */
int  Screen_SaveFormatFromName(const char *name);
/* ';'-separated extensions (no dots, lower case) a dialog kind offers --
 * the lists the loaders accept (Import_KnownExt, RIS_KnownExt,
 * RI_KnownExt); "" for the folder picker */
const char *Screen_DialogExts(int kind);
const char *Screen_DialogTitle(int kind);

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

/* test hook for the key event path (feature 014; selftest): queue
 * synthetic it_key_t events so US/German layout behaviour can be
 * exercised on any host, with no German keyboard attached. */
int  Screen_KeyFeedTest(const it_key_t *k, int n);

typedef struct screen_backend_t {
    int  (*init)(void);
    void (*uninit)(void);
    /* present the full cell buffer; backend does its own damage tracking */
    void (*present)(const screen_cell_t *cells);
    int  (*key)(void);
    void (*mouse)(it_mouse_t *m);   /* NULL = no mouse support */
    /* feature 014: full two-layer event. NULL = this backend cannot
     * report physical positions; Key_GetEvent() then synthesizes one
     * from key() and fills scan from the configured reverse map. */
    int  (*key_event)(it_key_t *k);
    /* feature 016: the host's file dialog; NULL = none (terminal). Must
     * fill path/display/lossy (and save_format for saves), not ask about
     * overwriting (the editor does, as F10), and flush input before it
     * returns (no key typed into the dialog, no modifier left held). */
    int  (*file_dialog)(const it_dialog_req_t *req, it_dialog_res_t *res);
    const char *preview_key;    /* see Screen_PreviewKeyLabel            */
    const char *dialog_mod;     /* see Screen_DialogModLabel             */
    /* #28: Ctrl-M (MouseToggle) -- show/hide the pointer over the
     * window; NULL = no pointer to hide (terminal) */
    void (*mouse_visible)(int show);
} screen_backend_t;

#ifdef _WIN32
extern const screen_backend_t Screen_BackendWin32;
#endif

/* headless, driven over stdin by a test harness (ITED_REMOTE=1;
 * it_screen_remote.c) */
extern const screen_backend_t Screen_BackendRemote;

#ifdef HAVE_SDL
extern const screen_backend_t Screen_BackendSDL;
#endif

#endif /* IT_SCREEN_H */
