/*
 * it_editor.c
 * -----------
 * Cross-platform front-end reproducing the Impulse Tracker editor's
 * screens and workflow on top of the ported engine, drawn with the
 * original IT 2.14 screen layouts:
 *
 *  - the common chrome (full-screen bevel, header rows 1-8, info line,
 *    dotted title line) is rendered from the *original control-coded
 *    strings* (HeaderMsg1-4 from IT_F.ASM, SongPlayMsg/TimeMsg from
 *    IT_L.ASM) through Screen_DrawStringCtl, with the live values
 *    filled at the exact PE_FillHeader positions;
 *  - the pattern editor follows IT_PE.ASM (sunken style-27 box at
 *    (4,14)-(74,47), " Channel xx " headers in attr 13h, 32 rows,
 *    attr 06h cells with E6h/F6h row hilights, char-168 track
 *    dividers, char-173 empty fields, cursor attr 30h);
 *  - F3/F12/F9/F11 use the object coordinates from IT_OBJ1.ASM
 *    verbatim; F4 is the object-exact port of the four instrument-page
 *    object lists (General/Volume/Panning/Pitch, IT_OBJ1.ASM 5629..)
 *    with the IT_I.ASM custom objects: instrument window, note
 *    translation window, envelope display/editor (font bank B canvas,
 *    I_MapEnvelope), pitch-pan center.
 *
 * Keys: F1 help, F2 pattern, F3 samples, F4 instruments, F5/F6 play,
 * F8 stop, F9 load (file requester), F11 orders, F12 song variables,
 * ESC main menu, IT piano note entry, octave/edit-step, Ctrl-Q quit.
 * Pattern edits are serialised against the audio thread.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <math.h>                       /* Fourier analyser (013) */

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define getcwd _getcwd
#define chdir _chdir
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

#include "it_music.h"
#include "it_pattern.h"
#include "it_save.h"
#include "it_import.h"
#include "it_ris.h"
#include "it_screen.h"
#include "it_vgadata.h"                 /* IT_FontROM (selftest checks) */

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "../external/miniaudio.h"

extern const sounddriver_t WAVDriver;
void WAVDriver_Render(int16_t *dst, uint32_t frames);
void WAVDriver_SetMixSpeed(uint32_t hz);
uint32_t WAVDriver_GetMixSpeed(void);
void WAVDriver_GetWaveForm(int16_t *out2048);   /* feature 013 tap */
/* Shift-F5 extensions (it_driver.c) */
void WAVDriver_RenderAny(void *dst, uint32_t frames);
void WAVDriver_SetOutputFormat(int fmt);
int  WAVDriver_GetOutputFormat(void);
void WAVDriver_SetSBFilter(int mode);
void WAVDriver_SetSBFeedback(int mode);
void WAVDriver_SetStartRamp(int on);
void WAVDriver_SetOutputChannels(int ch);
void WAVDriver_SetForceMono(int on);
int  Music_LoadIT(const char *path);
void Music_FreeIT(void);

enum { SCR_HELP, SCR_PATTERN, SCR_SAMPLES, SCR_INSTRUMENTS,
       SCR_ORDER, SCR_VARS, SCR_INFO, SCR_MESSAGE, SCR_KEYS,
       SCR_DRIVER, SCR_EMPTY, SCR_COUNT };

/* ---- editor state ---- */
static int      Screen = SCR_PATTERN;
static int      FileMode = 0;            /* CurrentMode of a modal file
                                           screen (9/10/13/15), 0 = none;
                                           Glbl_GetHeaderMode, #36 */
static editcell_t Grid[MAX_PATROWS * 64];
static uint16_t  CurPattern = 0;
static uint16_t  CurRows = 64;
static int       CurRow = 0, CurChan = 0;
/* cursor column, the original's 9 positions (PE_PatternCursorPos0..8):
 * 0 note, 1 octave digit, 2/3 ins tens/units, 4/5 vol tens/units,
 * 6 command, 7/8 param hi/lo */
static int       CurCol = 0;

/* ==== feature 009: pattern editing depth (IT_PE.ASM 213..345,
 * 862..957) -- names/initial values mirror the ASM ==== */
static uint8_t   EditMask = 3;          /* bit0 ins, bit1 vol, bit2 cmd */
static const uint8_t MaskChange[9] = { 0, 0, 1, 1, 2, 2, 4, 4, 4 };
static uint8_t   MultiChannelInfo[64];
static int       BlockMark = 0;         /* 1 = block marked */
static int       BlockLeft, BlockTop, BlockRight, BlockBottom;
static int       BlockAnchorChan, BlockAnchorRow;
static int       BlockReset = 0, NoteEntered = 0, ShiftHeld = 0;
static editcell_t *ClipData = NULL;     /* BlockDataArea (0 = empty) */
static int       ClipChans = 0, ClipRows = 0;
static int       Template = 0;          /* 0 off, 1 overwrite,
                                         * 2 mix-pattern, 3 mix-clip,
                                         * 4 notes only */
static int       Amplification = 100;   /* Alt-J prompt memory */
static int       FastVolumeAmp = 67;    /* FastVolumeAmplification */
static uint8_t   PEConfig = 0xE8;       /* CentraliseCursor byte: bit0
                                         * centralise, bit1 hilight row,
                                         * bit2 fast volume changes */
static uint8_t   VolumePan = 0;         /* 0x80 = entering pannings */
static uint8_t   CommandToValue = 0;    /* cursor moves onto values */
/* Last* entry memory (ASM note values: 0..119, 253 = none) */
static uint8_t   LastNote = 60;
static uint8_t   LastVolume = 0xFF;
static uint8_t   LastCommand = 0, LastCommandValue = 0;
static int       PlayMarkPattern = 0, PlayMarkRow = 0, PlayMarkOn = 0;
static int       LastKeys[3];           /* LastKeyBoard1..3 history */

/* ---- feature 014: the live key event -------------------------------
 * The original keeps both halves of a keypress in scope at once
 * (K_GetKey returns CX/DX = input/translated, IT_K.ASM:1108). Note
 * entry needs the physical half -- IT_I.ASM:1344 matches the scancode
 * and never looks at the character -- which no `int key` argument can
 * carry. So the whole event lives here, filled by ed_get_key() in every
 * key loop and synthesized by ed_sync_key() when a key is injected
 * directly (the selftest scripts, and the internal handle_global()
 * calls that stand in for real presses). */
static it_key_t  CurKey;
static char      KeyboardCfg[260];      /* ited.cfg keyboard_cfg= */

static void ed_sync_key(int key)
{
    if (CurKey.code == key && (CurKey.flags & ITKF_PRESSED))
        return;                         /* already the live event */
    memset(&CurKey, 0, sizeof(CurKey));
    CurKey.code  = key;
    CurKey.flags = ITKF_PRESSED;
    if (key > 0 && key < 0x100)
        CurKey.ch = (uint16_t)key;
    CurKey.scan = Key_ReverseScan(CurKey.ch);
}

/* Ctrl-F1 keypress table (feature 014): a rolling log of what the
 * tracker actually received. This is the screen Keyboard/DE.ASM's header
 * comment points at ("the value in the keypress table in IT on Ctrl-F1")
 * as the way to read key codes when writing a layout file. */
#define KEYLOG_N 12
static it_key_t KeyLog[KEYLOG_N];
static int      KeyLogCount;

static int ed_get_key(void)
{
    if (!Key_GetEvent(&CurKey)) {
        memset(&CurKey, 0, sizeof(CurKey));
        return ITK_NONE;
    }
    memmove(&KeyLog[1], &KeyLog[0], sizeof(KeyLog) - sizeof(KeyLog[0]));
    KeyLog[0] = CurKey;
    if (KeyLogCount < KEYLOG_N)
        KeyLogCount++;
    return CurKey.code;
}
typedef struct undoslot_t {             /* UndoBuffer: 10 snapshots */
    editcell_t *cells;
    uint16_t    rows;
    uint16_t    pattern;
    uint8_t     type;                   /* 0..22, UndoBufferTypes */
} undoslot_t;
static undoslot_t UndoRing[10];
static IT_MAYBE_UNUSED editcell_t *ScratchData = NULL;  /* Alt-0 store/restore */
static IT_MAYBE_UNUSED int       ScratchRows = 0;
static int       TracePlayback = 0;
static int       ViewTracking = 0;      /* ViewChannelTracking (891) */
static int       TopRow = 0, LeftChan = 0;
/* ---- feature 010: pattern-length dialog + multi-scheme views ---- */
static uint16_t  PatternSetLength = 64; /* IT_PE.ASM 349; persists across
                                         * dialog invocations (the original
                                         * re-prime is commented out) */
static uint16_t  PatternLengthStart = 0, PatternLengthEnd = 0;  /* 350/351 */
static uint16_t  ViewChannels[100];     /* 890: (method<<8)|channel entries,
                                         * 0xFFFF-terminated; all-0xFFFF =
                                         * default full view */
static uint8_t   ViewDivision = 1;      /* 888: char-168 divider column */
static int       ViewWidth = 0;         /* 889 (derived by pe_check_width) */
static int       NumChansEdit = 5;      /* NumChannelsEdit (880) */
/* ViewMethodInfo (910..924): per-method cell widths */
static const int ViewMethodWidth[5] = { 13, 10, 7, 3, 2 };
/* CursorPositions (855..860): per-method cursor-column x offsets
 * (methods 0..2), then the ViewNote/ViewTiny rows consumed by the
 * hilight path (rows 3..4; first byte = cursor attr base) */
static const uint8_t CursorPositions[5][9] = {
    { 0, 2, 4, 5, 7, 8, 10, 11, 12 },
    { 0, 2, 3, 4, 5, 6, 7, 8, 9 },
    { 0, 2, 3, 3, 4, 4, 5, 6, 6 },
    { 0x20, 2, 1, 2, 1, 2, 0, 1, 2 },
    { 0x10, 1, 0, 1, 0, 1, 0, 1, 1 },
};
static int       BaseOctave = 4;
static int       EditStep = 1;
static int       CurInstr = 1;
static int       ListSel = 0;             /* sample/instrument/order index */
static uint8_t   NoteSampleNumber = 1;    /* SampleNumber (IT_I.ASM)       */
static int       Running = 1;
static char      FileNameDisp[20] = "";   /* header File Name field */
/* feature 016: the real file name Ctrl-S saves to after a system-dialog
 * load/save, when it differs from the display (case kept, characters
 * CP437 cannot show kept); "" = save to FileNameDisp as before. The
 * original screens clear it whenever they set FileNameDisp. */
static char      FileSaveName[260] = "";
static time_t    StartTime;
static char      DirModule[256], DirSample[256], DirInstr[256];

static ma_device Device;
static ma_mutex  Mutex;
static int       DeviceUp = 0;

static void ed_lock(void)   { ma_mutex_lock(&Mutex); }
static void ed_unlock(void) { ma_mutex_unlock(&Mutex); }

/* ---- small drawing helpers ---- */
static void fill(int x, int y, int w, uint8_t ch, uint8_t attr)
{
    int i;
    for (i = 0; i < w; i++)
        Screen_PutChar(x + i, y, ch, attr);
}

static void drawf(int x, int y, uint8_t attr, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Screen_DrawString(x, y, buf, attr);
}

/* PE_ConvAX2Num: 3-digit zero-padded decimal (signed values, as the
 * F4 Pitch-Pan Separation / MIDI Program fields need, print as %3d) */
static void draw3num(int x, int y, int v, uint8_t attr)
{
    if (v < 0)
        drawf(x, y, attr, "%3d", v < -99 ? -99 : v);
    else
        drawf(x, y, attr, "%03d", v > 999 ? 999 : v);
}

/* 26-char IT name field: control chars become spaces (PE_FillHeader) */
static void draw_itname(int x, int y, const char *name, int w, uint8_t attr)
{
    int i;
    for (i = 0; i < w; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c < 32)
            c = ' ';
        Screen_PutChar(x + i, y, c, attr);
    }
}

/* F_DrawButtonObject look: base style box, +1 (swapped bevel) when
 * pressed/down; label attr 20h, or 23h when the button has keyboard
 * focus (F_PreButtonObject). */
static void draw_button_style(int x0, int y0, int x1, int y1, int style,
                              const char *text, int pressed, int focused)
{
    Screen_DrawBox(x0, y0, x1, y1, style + (pressed ? 1 : 0));
    Screen_DrawString(x0 + 1, (y0 + y1) / 2, text, focused ? 0x23 : 0x20);
}

/* F_DrawThumbBar, ported 1:1: black groove, 6px thumb built from the
 * fractional-bar glyphs 155..167, 3-digit value in attr 21h after.
 * `tattr` is 02h normally, 03h (white thumb) when the bar has focus --
 * exactly F_DrawThumbBar vs F_PreThumbBar. */
static void draw_thumbbar(int x, int y, int min, int max, int val,
                          uint8_t tattr)
{
    int width = (max - min + 15) >> 3;
    int i, v, cell, sub;

    if (val < min) val = min;
    if (val > max) val = max;

    for (i = 0; i < width; i++)
        Screen_PutChar(x + i, y, 0, 0x03);

    v = val - min + 1;
    cell = v >> 3;
    sub = v & 7;
    Screen_PutChar(x + cell, y, (uint8_t)(155 + sub), tattr);
    if (155 + sub > 157)
        Screen_PutChar(x + cell + 1, y, (uint8_t)(155 + sub + 5), tattr);

    draw3num(x + width + 1, y, val, 0x21);
}

/* F_DrawScalableThumbBar (IT_F.ASM 1738), ported 1:1: `width` is the
 * object's display length L; the groove is L+1 cells, the thumb sits at
 * pixel (val-min)*L*8/(max-min)+1 and the 3-digit value at x+L+2 --
 * outside the object's box (issue #6: F12 Initial Tempo/Speed). */
static void draw_thumbbar_scaled(int x, int y, int min, int max, int val,
                                 int width, uint8_t tattr)
{
    int i, v, cell, sub;

    if (val < min) val = min;
    if (val > max) val = max;

    for (i = 0; i <= width; i++)
        Screen_PutChar(x + i, y, 0, 0x03);

    v = (val - min) * width * 8 / (max - min) + 1;
    cell = v >> 3;
    sub = v & 7;
    Screen_PutChar(x + cell, y, (uint8_t)(155 + sub), tattr);
    if (155 + sub > 157)
        Screen_PutChar(x + cell + 1, y, (uint8_t)(155 + sub + 5), tattr);

    draw3num(x + width + 2, y, val, 0x21);
}

static const char NoteNameChars[] = "C-C#D-D#E-F-F#G-G#A-A#B-";

/* ===================================================================
 * Transient status message on the info line (row 9), like the
 * original's "Saved.", "Function not implemented" etc. messages.
 * =================================================================== */
static char   StatusMsg[64];
static time_t StatusUntil;

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(StatusMsg, sizeof(StatusMsg), fmt, ap);
    va_end(ap);
    StatusUntil = time(NULL) + 2;
}

/* ===================================================================
 * Object/widget framework -- C equivalent of the IT_F.ASM object
 * handlers. Focus indication follows the original exactly:
 *   buttons   F_PreButtonObject:  label attr 23h instead of 20h
 *   thumbbars F_PreThumbBar:      thumb attr 03h instead of 02h
 *   toggles   F_PreToggle:        On/Off word hilighted in 30h
 *   text      F_PreStringInput:   30h cursor cell after the text
 * Every interactive screen rebuilds its widget table each frame (the
 * value pointers track the current list selection); a per-screen focus
 * index survives across frames and screen switches.
 * =================================================================== */
enum { WT_BUTTON = 1, WT_THUMB, WT_TOGGLE, WT_TEXT, WT_LIST,
       WT_NUM3, WT_CUSTOM };

typedef struct widget_t {
    uint8_t type;
    int x0, y0, x1, y1;            /* hit box, inclusive cell coords */
    /* button */
    const char *label;
    int style;                     /* box style up; down = style+1 */
    /* value binding: byte radio/thumb value, or 16-bit flag word */
    uint8_t  *v8;
    uint8_t   vmask, vval;         /* radio: down when (*v8&vmask)==vval */
    uint16_t *v16;
    uint16_t  bit;                 /* flag bit (toggle / flag-button)  */
    uint8_t   neg;                 /* flag-button: down when bit clear */
    uint8_t   sgn;                 /* thumb/num: value byte is signed  */
    void    (*action)(void);       /* press / value-change hook */
    /* thumbbar (value in *v8 masked by vmask, or 16-bit in *v16) */
    int barx, bary, min, max, dw;  /* dw = scaled display width, 0 = classic */
    /* string input */
    char *text;
    int   tmax;                    /* max characters excl. terminator */
    /* list (selection/scrolling handled by the screen) */
    int (*lkey)(int key);          /* returns nonzero if consumed */
    void (*lclick)(int row, int mx, int mpx);
    int  listy0;                   /* screen row of the first list line */
    /* custom object (type-15 far-function triple: draw/pre + post) */
    void (*cdraw)(int focused);    /* draw + pre (focus) */
    int  (*ckey)(int key);         /* returns nonzero if consumed */
    void (*cclick)(const it_mouse_t *m);
    void (*cdrag)(const it_mouse_t *m); /* NULL = no drag tracking */
} widget_t;

#define MAX_WIDGETS 48
static widget_t W[MAX_WIDGETS];
static int      NW;
static int      FocusIdx[SCR_COUNT];
static int      DragIdx = -1;      /* thumbbar being mouse-dragged */

static widget_t *wadd(int type, int x0, int y0, int x1, int y1)
{
    widget_t *w = &W[NW];
    memset(w, 0, sizeof(*w));
    w->type = type;
    w->x0 = x0; w->y0 = y0; w->x1 = x1; w->y1 = y1;
    if (NW < MAX_WIDGETS - 1)
        NW++;
    return w;
}

static widget_t *wbutton(int x0, int y0, int x1, int y1, const char *label,
                         void (*action)(void))
{
    widget_t *w = wadd(WT_BUTTON, x0, y0, x1, y1);
    w->label = label;
    w->style = 8;
    w->action = action;
    return w;
}

/* radio button over a byte field: down when (*v&mask)==val, pressing
 * stores val (the ButtonEffectType-5 "set variable" buttons) */
static widget_t *wradio8(int x0, int y0, int x1, int y1, const char *label,
                         uint8_t *v, uint8_t mask, uint8_t val)
{
    widget_t *w = wbutton(x0, y0, x1, y1, label, NULL);
    w->v8 = v; w->vmask = mask; w->vval = val;
    return w;
}

/* radio button over a song-flags bit: `neg` button clears the bit */
static widget_t *wradiof(int x0, int y0, int x1, int y1, const char *label,
                         uint16_t *flags, uint16_t bit, uint8_t neg)
{
    widget_t *w = wbutton(x0, y0, x1, y1, label, NULL);
    w->v16 = flags; w->bit = bit; w->neg = neg;
    return w;
}

static widget_t *wthumb(int x, int y, int min, int max, uint8_t *v,
                        uint8_t vmask, int dw)
{
    int width = dw ? dw + 1 : ((max - min + 15) >> 3);    /* L+1 cells */
    widget_t *w = wadd(WT_THUMB, x, y, x + width - 1, y);
    w->barx = x; w->bary = y;
    w->min = min; w->max = max; w->dw = dw;
    w->v8 = v; w->vmask = vmask;
    return w;
}

static widget_t *wtoggle8(int x, int y, uint8_t *v, uint8_t bit)
{
    widget_t *w = wadd(WT_TOGGLE, x, y, x + 2, y);
    w->v8 = v; w->bit = bit;
    return w;
}

static widget_t *wtogglef(int x, int y, uint16_t *flags, uint16_t bit)
{
    widget_t *w = wadd(WT_TOGGLE, x, y, x + 2, y);
    w->v16 = flags; w->bit = bit;
    return w;
}

static widget_t *wtext(int x, int y, char *text, int tmax)
{
    widget_t *w = wadd(WT_TEXT, x, y, x + tmax, y);
    w->text = text; w->tmax = tmax;
    return w;
}

static widget_t *wlist(int x0, int y0, int x1, int y1, int listy0,
                       int (*lkey)(int),
                       void (*lclick)(int, int, int))
{
    widget_t *w = wadd(WT_LIST, x0, y0, x1, y1);
    w->lkey = lkey; w->lclick = lclick; w->listy0 = listy0;
    return w;
}

/* F_Draw3Num: 3-digit numeric byte field (envelope loop node numbers) */
static widget_t *wnum3(int x, int y, uint8_t *v, int min, int max,
                       void (*action)(void))
{
    widget_t *w = wadd(WT_NUM3, x, y, x + 2, y);
    w->v8 = v; w->min = min; w->max = max; w->action = action;
    return w;
}

/* type-15 custom object: draw/pre + post callbacks */
static widget_t *wcustom(int x0, int y0, int x1, int y1,
                         void (*cdraw)(int), int (*ckey)(int),
                         void (*cclick)(const it_mouse_t *))
{
    widget_t *w = wadd(WT_CUSTOM, x0, y0, x1, y1);
    w->cdraw = cdraw; w->ckey = ckey; w->cclick = cclick;
    return w;
}

static int thumb_get(const widget_t *w)
{
    if (w->v16)
        return *w->v16;
    if (w->sgn)
        return (int8_t)*w->v8;
    return w->vmask ? (*w->v8 & w->vmask) : *w->v8;
}

static void thumb_set(widget_t *w, int v)
{
    if (v < w->min) v = w->min;
    if (v > w->max) v = w->max;
    ed_lock();
    if (w->v16)
        *w->v16 = (uint16_t)v;
    else if (w->vmask && !w->sgn)
        *w->v8 = (uint8_t)((*w->v8 & ~w->vmask) | v);
    else
        *w->v8 = (uint8_t)v;
    ed_unlock();
    if (w->action)
        w->action();
}

/* mouse -> value, pixel-precise as the original 8010h mouse events:
 * classic bars map 1 pixel = 1 unit from barx*8+4; scalable bars
 * compress the range onto dw*8 pixels with rounding. */
static void thumb_from_px(widget_t *w, int mpx)
{
    int rel = mpx - (w->barx * 8 + 4);
    int v;

    if (rel < 0)
        rel = 0;
    if (w->dw) {
        int np = w->dw * 8;
        if (rel > np)
            rel = np;
        v = w->min + ((w->max - w->min) * rel + np / 2) / np;
    } else {
        v = w->min + rel;
    }
    thumb_set(w, v);
}

static int toggle_get(const widget_t *w)
{
    if (w->v8)
        return (*w->v8 & (uint8_t)w->bit) != 0;
    return (*w->v16 & w->bit) != 0;
}

static void toggle_flip(widget_t *w)
{
    ed_lock();
    if (w->v8)
        *w->v8 ^= (uint8_t)w->bit;
    else
        *w->v16 ^= w->bit;
    ed_unlock();
    if (w->action)
        w->action();
}

static int button_down(const widget_t *w)
{
    if (w->v8)
        return (*w->v8 & w->vmask) == w->vval;
    if (w->v16)
        return ((((*w->v16 & w->bit) != 0) ? 1 : 0) ^ w->neg) & 1;
    return 0;
}

static void button_press(widget_t *w)
{
    if (w->v8) {
        ed_lock();
        *w->v8 = (uint8_t)((*w->v8 & ~w->vmask) | w->vval);
        ed_unlock();
    } else if (w->v16) {
        ed_lock();
        if (w->neg)
            *w->v16 &= (uint16_t)~w->bit;
        else
            *w->v16 |= w->bit;
        ed_unlock();
    }
    if (w->action)
        w->action();
}

static int Num3Pos = 0;            /* TripleNumberPos: shared digit cursor */

static void widget_draw(const widget_t *w, int focused)
{
    switch (w->type) {
    case WT_BUTTON:
        draw_button_style(w->x0, w->y0, w->x1, w->y1, w->style,
                          w->label, button_down(w), focused);
        break;
    case WT_THUMB:
        if (w->dw)
            draw_thumbbar_scaled(w->barx, w->bary, w->min, w->max,
                                 thumb_get(w), w->dw,
                                 focused ? 0x03 : 0x02);
        else
            draw_thumbbar(w->barx, w->bary, w->min, w->max,
                          thumb_get(w), focused ? 0x03 : 0x02);
        break;
    case WT_TOGGLE:
        /* F_DrawToggle: "On"/"Off" attr 02h, two or three cells (the
         * box colour stays after "On"); focus (F_PreToggle) hilights
         * just the word in 30h */
        Screen_DrawString(w->x0, w->y0, toggle_get(w) ? "On" : "Off", 0x02);
        if (focused)
            Screen_DrawString(w->x0, w->y0,
                              toggle_get(w) ? "On" : "Off", 0x30);
        break;
    case WT_NUM3: {
        int v = w->sgn ? (int8_t)*w->v8 : *w->v8;
        draw3num(w->x0, w->y0, v, 0x02);
        if (focused) {          /* F_Pre3Num: digit cursor cell in 30h */
            char d[2];
            d[0] = (char)('0' + (v / (Num3Pos == 0 ? 100 :
                                      Num3Pos == 1 ? 10 : 1)) % 10);
            d[1] = 0;
            Screen_DrawString(w->x0 + Num3Pos, w->y0, d, 0x30);
        }
        break; }
    case WT_CUSTOM:
        if (w->cdraw)
            w->cdraw(focused);
        break;
    case WT_TEXT: {
        /* F_DrawStringInput (IT_F.ASM 2977): the text up to its end in
         * colour 2, characters from 226 up as spaces; the rest of the
         * field keeps the box colour (#35 #37 #39). F_PreStringInput:
         * 30h on the cell after the text. */
        int i, len = 0;
        while (len < w->tmax && w->text[len])
            len++;
        for (i = 0; i < len; i++) {
            uint8_t c = (uint8_t)w->text[i];
            Screen_PutChar(w->x0 + i, w->y0, c >= 226 ? ' ' : c, 0x02);
        }
        if (focused)
            Screen_SetAttr(w->x0 + len, w->y0, 0x30);
        break; }
    case WT_LIST:
        break;                     /* the screen draws its own lists */
    }
}

static void widgets_draw(void)
{
    int i;
    if (FocusIdx[Screen] >= NW)
        FocusIdx[Screen] = 0;
    for (i = 0; i < NW; i++)
        widget_draw(&W[i], i == FocusIdx[Screen]);
}

/* directional focus movement; approximates the original per-object
 * Up/Down/Left/Right association links geometrically */
static void nav_move(int dir)              /* 0=up 1=down 2=left 3=right */
{
    const widget_t *c = &W[FocusIdx[Screen]];
    int cx = (c->x0 + c->x1) / 2, cy = (c->y0 + c->y1) / 2;
    int best = -1, bestscore = 0x7FFFFFFF, i;

    for (i = 0; i < NW; i++) {
        int x, y, dx, dy, score;
        if (i == FocusIdx[Screen])
            continue;
        x = (W[i].x0 + W[i].x1) / 2;
        y = (W[i].y0 + W[i].y1) / 2;
        dx = x - cx; dy = y - cy;
        /* Up/Down prefer a widget whose columns overlap the current one
         * (the original's object links mostly point straight down); only
         * without one does the nearest-centre fallback apply. Centre
         * distance alone jumped from the long F12 Tempo/Speed bars to
         * the Control buttons and past the path fields (issue #19). */
        switch (dir) {
        case 0: if (dy >= 0) continue; score = -dy + abs(dx) * 4; break;
        case 1: if (dy <= 0) continue; score =  dy + abs(dx) * 4; break;
        case 2: if (dx >= 0) continue; score = -dx + abs(dy) * 4; break;
        default:if (dx <= 0) continue; score =  dx + abs(dy) * 4; break;
        }
        if (dir < 2) {
            if (W[i].x1 >= c->x0 && W[i].x0 <= c->x1)
                score = abs(dy) * 8 + abs(dx);      /* overlapping */
            else
                score += 100000;                    /* fallback only */
        }
        if (score < bestscore) {
            bestscore = score;
            best = i;
        }
    }
    if (best >= 0)
        FocusIdx[Screen] = best;
}

/* F_PostThumbBar30 + O1_ThumbStringList (IT_F.ASM / IT_OBJ1.ASM): typing
 * a digit on a thumbbar opens "Enter Value" -- box (29,24)-(50,28) style
 * 3, text at (32,26) attr 23h, a 4-char input at (44,26) in a style-27
 * box (43,25)-(48,27) -- seeded with that digit. Enter parses it; a
 * non-digit, an empty entry or a value outside min..max changes nothing
 * (the original rejects, it does not clamp). Esc cancels. `bg` redraws
 * the screen underneath (the original's S_SaveScreen/S_RestoreScreen).
 * Returns 1 and sets *out when a value was accepted (issue #9). */
static void draw_screen(void);

static int thumb_value_dialog(int first, int min, int max, int *out,
                              void (*bg)(void))
{
    char buf[5];
    int len = 1;

    buf[0] = (char)first;
    buf[1] = 0;
    while (Running) {
        int key, x;

        bg();
        Screen_DrawBox(29, 24, 50, 28, 3);          /* ThumbBox */
        Screen_DrawString(32, 26, "Enter Value", 0x23);
        Screen_DrawBox(43, 25, 48, 27, 27);         /* ThumbInputBox */
        for (x = 0; x < 4; x++)
            Screen_PutChar(44 + x, 26, x < len ? (uint8_t)buf[x] : ' ',
                           x == len ? 0x30 : 0x02);  /* cursor cell */
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return 0; }
        if (key == ITK_ESC)
            return 0;
        if (key == ITK_BACKSPACE) {
            if (len > 0)
                buf[--len] = 0;
            continue;
        }
        if (key == ITK_ENTER) {
            int v = 0, i;
            if (len == 0)
                return 0;
            for (i = 0; i < len; i++) {
                if (buf[i] < '0' || buf[i] > '9')
                    return 0;
                v = v * 10 + (buf[i] - '0');
            }
            if (v < min || v > max)
                return 0;
            *out = v;
            return 1;
        }
        if (key >= 32 && key < 127 && len < 4) {
            buf[len++] = (char)key;
            buf[len] = 0;
        }
    }
    return 0;
}

static int widgets_key(int key)
{
    widget_t *w;
    int f = FocusIdx[Screen];

    if (NW == 0)
        return 0;
    if (f >= NW)
        f = FocusIdx[Screen] = 0;
    w = &W[f];

    if (w->type == WT_LIST && w->lkey && w->lkey(key))
        return 1;
    if (w->type == WT_CUSTOM && w->ckey && w->ckey(key))
        return 1;

    if (w->type == WT_NUM3) {   /* F_Post3Num: digit cursor + entry */
        switch (key) {
        case ITK_LEFT: case ITK_BACKSPACE:
            if (Num3Pos > 0) Num3Pos--;
            return 1;
        case ITK_RIGHT:
            if (Num3Pos < 2) Num3Pos++;
            return 1;
        case '+': thumb_set(w, thumb_get(w) + 1); return 1;
        case '-': thumb_set(w, thumb_get(w) - 1); return 1;
        default: break;
        }
        if (key >= '0' && key <= '9') {
            int v = *w->v8, dig[3];
            dig[0] = v / 100 % 10; dig[1] = v / 10 % 10; dig[2] = v % 10;
            dig[Num3Pos] = key - '0';
            thumb_set(w, dig[0] * 100 + dig[1] * 10 + dig[2]);
            if (Num3Pos < 2) Num3Pos++;
            return 1;
        }
    }

    switch (key) {
    case ITK_TAB:       FocusIdx[Screen] = (f + 1) % NW;      return 1;
    case ITK_SHIFT_TAB: FocusIdx[Screen] = (f + NW - 1) % NW; return 1;
    case ITK_UP:        nav_move(0); return 1;
    case ITK_DOWN:      nav_move(1); return 1;
    default: break;
    }

    if (w->type == WT_THUMB) {
        switch (key) {
        case ITK_LEFT:  thumb_set(w, thumb_get(w) - 1); return 1;
        case ITK_RIGHT: thumb_set(w, thumb_get(w) + 1); return 1;
        /* F_PostThumbBar16/17: Shift = 4 steps, Ctrl = 2 (issue #7) */
        case ITK_SHIFT_LEFT:  thumb_set(w, thumb_get(w) - 4); return 1;
        case ITK_SHIFT_RIGHT: thumb_set(w, thumb_get(w) + 4); return 1;
        case ITK_CTRL_LEFT:   thumb_set(w, thumb_get(w) - 2); return 1;
        case ITK_CTRL_RIGHT:  thumb_set(w, thumb_get(w) + 2); return 1;
        case ITK_HOME:  thumb_set(w, w->min);           return 1;
        case ITK_END:   thumb_set(w, w->max);           return 1;
        default: break;
        }
        /* a typed digit opens "Enter Value" (F_PostThumbBar30) */
        if (key >= '0' && key <= '9') {
            int v;
            if (thumb_value_dialog(key, w->min, w->max, &v, draw_screen))
                thumb_set(w, v);
            return 1;
        }
    } else {
        if (key == ITK_LEFT)  { nav_move(2); return 1; }
        if (key == ITK_RIGHT) { nav_move(3); return 1; }
    }

    if (w->type == WT_TEXT) {
        int len = 0;
        while (len < w->tmax && w->text[len])
            len++;
        if (key == ITK_BACKSPACE) {
            if (len > 0)
                w->text[len - 1] = 0;
            return 1;
        }
        if (key >= 32 && key < 256) {   /* CP437, incl. national chars
                                           (feature 014); anything the
                                           host layout produces that
                                           CP437 cannot carry never
                                           reaches us at all */
            if (len < w->tmax) {
                w->text[len] = (char)key;
                if (len + 1 <= w->tmax)
                    w->text[len + 1] = 0;
            }
            return 1;
        }
    }

    if (key == ITK_ENTER || key == ' ') {
        if (w->type == WT_BUTTON) { button_press(w); return 1; }
        if (w->type == WT_TOGGLE) { toggle_flip(w);  return 1; }
    }
    return 0;
}

static void widgets_mouse(void)
{
    it_mouse_t m;
    int i;

    Screen_GetMouse(&m);
    for (i = 0; i < NW; i++) {
        widget_t *w = &W[i];
        if (m.x < w->x0 || m.x > w->x1 || m.y < w->y0 || m.y > w->y1)
            continue;
        FocusIdx[Screen] = i;
        switch (w->type) {
        case WT_BUTTON: button_press(w); break;
        case WT_TOGGLE: toggle_flip(w); break;
        case WT_THUMB:  thumb_from_px(w, m.px); DragIdx = i; break;
        case WT_NUM3:   thumb_set(w, thumb_get(w) - 1); break;
        case WT_LIST:
            if (w->lclick)
                w->lclick(m.y - w->listy0, m.x, m.px);
            break;
        case WT_CUSTOM:
            if (w->cclick)
                w->cclick(&m);
            if (w->cdrag)
                DragIdx = i;
            break;
        default: break;
        }
        return;
    }
}

/* per-screen list handlers (definitions follow the key-handling code) */
static int  sample_list_lkey(int key);
static int  instr_list_lkey(int key);
static int  order_list_lkey(int key);
static int  pan_left_lkey(int key);
static int  pan_right_lkey(int key);
static void sample_list_lclick(int row, int mx, int mpx);
static void instr_list_lclick(int row, int mx, int mpx);
static void order_list_lclick(int row, int mx, int mpx);
static void pan_left_lclick(int row, int mx, int mpx);
static void pan_right_lclick(int row, int mx, int mpx);
static int  max_order(void);
static void act_stereo_changed(void);
static void act_enable_instruments(void);
static void act_tempo_changed(void);
static void act_speed_changed(void);
static void act_gv_changed(void);
static void act_mv_changed(void);
static void act_help_done(void);
static void act_save_prefs(void);

/* ===================================================================
 * Common chrome: header (HeaderMsg1-4 from IT_F.ASM), info line,
 * dotted title line. All strings byte-exact from the original.
 * =================================================================== */

/* octal: \377=FF repeat, \376=FE attr, \375=FD number, \015=CR
 * glyphs: \200..\215 = custom chars 128..141                      */
static const uint8_t HeaderMsg1[] =
    "\377\011 Impulse Tracker v2.14 Copyright (C) 1995-2000 Jeffrey Lim\015"
    "\377\011 \376\041\213\377\031\206\212\377\013 \213\377\034\206\212\015"
    "\376\040Song Name\376\041\204\376\005\377\031 \376\043\203\376\040";

static const uint8_t HeaderMsg2[] =      /* instrument mode */
    " Instrument\376\041\204\376\007  :\377\031 \376\043\203";

static const uint8_t HeaderMsg3[] =      /* sample mode */
    "\377\005 Sample\376\041\204\376\007  :\377\031 \376\043\203";

static const uint8_t HeaderMsg4[] =
    "\376\040File Name\376\041\204\376\005\377\022 \376\043\200\377\006\201"
    "\210\376\040Speed/Tempo\376\041\204\376\005   \376\001/\376\005   "
    "\376\043\200\377\024\201\210\015"
    "\376\040\377\004 Order\376\041\204\376\005   \376\001/\376\005   "
    "\376\043\200\377\012\201\210\376\040\377\014 Octave\376\041\204\376\005 "
    "\376\043\200\377\005\201\210\015"
    "\376\040  Pattern\376\041\204\376\005   \376\001/\376\005   \376\043\203"
    "\376\040 F1...Help       F9.....Load \376\043\211\201\210\376\040"
    "\377\013 FreeMem \375Dk \015"
    "\376\040\377\006 Row\376\041\204\376\005   \376\001/\376\005   "
    "\376\043\203\376\040 ESC..Main Menu  F5/F8..Play / Stop"
    "\377\010 FreeEMS \375Dk\015"
    "\376\043\377\011 \211\377\007\201\210";

static const uint8_t SongPlayMsg[] =
    "Playing, Order: \376\043\375D\376\040/\376\043\375D\376\040, Pattern: "
    "\376\043\375D\376\040, Row: \376\043\375D\376\040/\376\043\375D\376\040"
    ", \376\043\375D\376\040 Channels\377\012 ";

static const uint8_t PatternPlayMsg[] =
    "Playing, Pattern: \376\043\375D\376\040, Row: \376\043\375D\376\040/"
    "\376\043\375D\376\040, \376\043\375D\376\040 Channels\377\012 ";

static int free_mem_k(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        unsigned long long k = ms.ullAvailPhys / 1024;
        return k > 999999 ? 999999 : (int)k;
    }
#endif
    return 65536;
}

static int count_active_channels(void)
{
    int i, n = 0;
    /* CountChannels in IT_L.ASM: flag bit 0 = channel on, skip when the
     * high-byte bit 3 (0x800, note-off/disowned) is set. */
    for (i = 0; i < MAXSLAVECHANNELS; i++)
        if ((SChn[i].Flags & 1) && !(SChn[i].Flags & 0x800))
            n++;
    return n;
}

static void draw_chrome(const char *title)
{
    int nums[2];

    Screen_DrawBox(0, 0, 79, 49, 4);     /* FullScreenBox */

    nums[0] = free_mem_k();
    nums[1] = 0;                          /* FreeEMS: none in this port */
    Screen_DrawStringCtl(2, 1, HeaderMsg1, 0x20, NULL);
    /* Glbl_GetHeaderMode (IT_G.ASM 645): the sample list and the Load
     * Sample screen (modes 3 / 13) show "Sample" with LastInstrument, the
     * instrument list (mode 4) "Sample" with SampleNumber, every other
     * mode follows the song's instrument mode (#36) */
    int hmode = FileMode ? FileMode
              : (Screen == SCR_SAMPLES) ? 3
              : (Screen == SCR_INSTRUMENTS) ? 4 : 0;
    int hnum = (hmode == 4) ? NoteSampleNumber : CurInstr;
    int hins = (hmode == 3 || hmode == 13 || hmode == 4) ? 0
             : (Song.Header.Flags & ITF_INSTRUMENTS) != 0;
    Screen_DrawStringCtl(38, 3, hins ? HeaderMsg2 : HeaderMsg3, 0x20, NULL);
    Screen_DrawStringCtl(2, 4, HeaderMsg4, 0x20, nums);

    /* ---- live values, PE_FillHeader positions, attr 5 ---- */
    draw_itname(12, 3, Song.Header.SongName, 25, 0x05);
    drawf(12, 4, 0x05, "%-18.18s", FileNameDisp);

    ed_lock();
    draw3num(12, 5, (PlayMode == 2) ? CurrentOrder : 0, 0x05);
    draw3num(16, 5, max_order(), 0x05);         /* PE_GetMaxOrder */
    draw3num(12, 6, CurPattern, 0x05);
    draw3num(16, 6, Song.Header.PatNum ? Song.Header.PatNum - 1 : 0, 0x05);
    draw3num(12, 7, CurRow, 0x05);
    draw3num(16, 7, CurRows ? CurRows - 1 : 0, 0x05);
    draw3num(50, 4, CurrentSpeed, 0x05);
    draw3num(54, 4, Tempo, 0x05);
    ed_unlock();

    Screen_PutChar(50, 5, (uint8_t)('0' + BaseOctave), 0x05);

    /* instrument/sample number + name */
    if (hnum <= 0) {
        drawf(50, 3, 0x05, "..");
        fill(53, 3, 25, '.', 0x05);
    } else {
        const char *name = "";
        drawf(50, 3, 0x05, "%02d", hnum % 100);
        if (hins) {
            if (hnum <= MAX_INSTRUMENTS)
                name = Song.Ins[hnum - 1].InstrumentName;
        } else {
            if (hnum <= MAX_SAMPLES)
                name = Song.Smp[hnum - 1].SampleName;
        }
        draw_itname(53, 3, name, 25, 0x05);
    }

    /* ---- info line (row 9) + time, IT_L.ASM ---- */
    fill(2, 9, 59, ' ', 0x20);
    ed_lock();
    if (PlayMode == 2) {
        int nums9[6];
        nums9[0] = CurrentOrder;
        nums9[1] = max_order();
        nums9[2] = CurrentPattern;
        nums9[3] = CurrentRow;
        nums9[4] = NumberOfRows;
        nums9[5] = count_active_channels();
        Screen_DrawStringCtl(2, 9, SongPlayMsg, 0x20, nums9);
    } else if (PlayMode == 1) {
        int nums9[4];
        nums9[0] = CurrentPattern;
        nums9[1] = CurrentRow;
        nums9[2] = NumberOfRows;
        nums9[3] = count_active_channels();
        Screen_DrawStringCtl(2, 9, PatternPlayMsg, 0x20, nums9);
    }
    ed_unlock();
    if (time(NULL) < StatusUntil)
        drawf(2, 9, 0x23, "%-59.59s", StatusMsg);
    {
        long secs = (long)(time(NULL) - StartTime);
        drawf(62, 9, 0x20, " Time    %ld:%02ld:%02ld",
              secs / 3600, (secs / 60) % 60, secs % 60);
    }

    /* ---- dotted title line (row 11), F_DrawInfoLine (IT_F.ASM 2443):
     * (78 - len) / 2 dots, space, title, space, the rest dots; an empty
     * title is 78 dots (#33: the port had one dot fewer in front) ---- */
    if (!title[0]) {
        int i;
        for (i = 0; i < 78; i++)
            Screen_PutChar(1 + i, 11, 154, 0x21);
    } else {
        int len = (int)strlen(title);
        int n1 = (78 - len) / 2;
        int n2 = 78 - n1 - len - 2;
        int x = 1, i;

        for (i = 0; i < n1; i++)
            Screen_PutChar(x++, 11, 154, 0x21);
        Screen_PutChar(x++, 11, ' ', 0x20);
        Screen_DrawString(x, 11, title, 0x20);
        x += len;
        Screen_PutChar(x++, 11, ' ', 0x20);
        for (i = 0; i < n2; i++)
            Screen_PutChar(x++, 11, 154, 0x21);
    }
}

/* ===================================================================
 * Pattern editor (F2) -- layout/colours from PE_DrawPatternEdit
 * =================================================================== */
/* RowHiLight1/RowHiLight2 (IT_PE.ASM 882): the pattern editor's own row
 * hilight setting, 4/16 by default, edited in Pattern Editor Options.
 * 0 switches that hilight off (PE_DrawPatternEdit skips a zero divisor).
 * Port: seeded from a loaded module's header PHiligt when it carries one,
 * and written back to it on save (the original's save takes the PE
 * config too -- see it_save.c). */
static uint8_t RowHiLight1 = 4, RowHiLight2 = 16;

static void row_hilight_from_song(void)
{
    RowHiLight1 = 4;
    RowHiLight2 = 16;
    if (Song.Header.PHiligt) {
        RowHiLight1 = (uint8_t)(Song.Header.PHiligt & 0xFF);
        RowHiLight2 = (uint8_t)(Song.Header.PHiligt >> 8);
    }
}

/* paging / block sizes want a non-zero step even when hilights are off */
static uint8_t row_hilight_2(void) { return RowHiLight2 ? RowHiLight2 : 16; }

/* Draw_3Note */
static void draw_note(int x, int y, const editcell_t *c, uint8_t attr)
{
    uint8_t n = (c->mask & CM_NOTE) ? c->note : GNOTE_EMPTY;

    if (n == GNOTE_EMPTY) {
        fill(x, y, 3, 173, attr);
    } else if (n == GNOTE_CUT) {
        fill(x, y, 3, '^', attr);
    } else if (n == GNOTE_OFF) {
        fill(x, y, 3, 205, attr);
    } else if (n == GNOTE_FADE) {
        fill(x, y, 3, '~', attr);
    } else {
        int v = n - 1;
        Screen_PutChar(x,     y, (uint8_t)NoteNameChars[(v%12)*2],   attr);
        Screen_PutChar(x + 1, y, (uint8_t)NoteNameChars[(v%12)*2+1], attr);
        Screen_PutChar(x + 2, y, (uint8_t)('0' + v / 12), attr);
    }
}

/* volume column display incl. effect letters / pan colour (fg 2).
 * The effect test mirrors the ASM exactly: (v & 7Fh) - 65 must be
 * non-negative BEFORE the bit-7 +60 offset is applied (pannings are
 * 128..192 and must fall through to the pan path). */
static void draw_volume(int x, int y, const editcell_t *c, uint8_t attr)
{
    uint8_t v;

    if (!(c->mask & CM_VOL)) {
        fill(x, y, 2, 173, attr);
        return;
    }
    v = c->vol;
    if (v >= 65) {
        int eff = (v & 0x7F) - 65;
        if (eff >= 0) {
            if (v & 0x80)
                eff += 60;
            drawf(x, y, attr, "%c%d", 'A' + eff / 10, eff % 10);
            return;
        }
        attr = (uint8_t)((attr & 0xF0) | 2);    /* panning colour */
        v &= 0x7F;
    }
    drawf(x, y, attr, "%02d", v % 100);
}

/* ViewNote/ViewTiny volume cell: effect letters keep the base attr,
 * plain volume dims to attr-4, pannings to attr-5 (IT_PE.ASM
 * ViewNoteNoVEffect / ViewTinyNoVEffect). */
static void draw_volume_small(int x, int y, uint8_t v, uint8_t a)
{
    if (v >= 65) {
        int eff = (v & 0x7F) - 65;
        if (eff >= 0) {
            if (v & 0x80)
                eff += 60;
            drawf(x, y, a, "%c%d", 'A' + eff / 10, eff % 10);
            return;
        }
        a = (uint8_t)(a - 5);                   /* panning */
        v &= 0x7F;
    } else {
        a = (uint8_t)(a - 4);
    }
    drawf(x, y, a, "%02d", v % 100);
}

/* Draw_2Note (IT_PE_V.INC 59): two-cell note -- lowercase letter for
 * naturals, uppercase for sharps, then the octave digit. */
static void draw_note2(int x, int y, const editcell_t *c, uint8_t attr)
{
    uint8_t n = (c->mask & CM_NOTE) ? c->note : GNOTE_EMPTY;

    if (n == GNOTE_EMPTY) {
        fill(x, y, 2, 173, attr);
    } else if (n == GNOTE_CUT) {
        fill(x, y, 2, '^', attr);
    } else if (n == GNOTE_OFF) {
        fill(x, y, 2, 205, attr);
    } else if (n == GNOTE_FADE) {
        fill(x, y, 2, '~', attr);
    } else {
        int v = n - 1;
        uint8_t l = (uint8_t)NoteNameChars[(v % 12) * 2];
        if (NoteNameChars[(v % 12) * 2 + 1] == '-')
            l = (uint8_t)(l + 'a' - 'A');
        Screen_PutChar(x,     y, l, attr);
        Screen_PutChar(x + 1, y, (uint8_t)('0' + v / 12), attr);
    }
}

/* PE_SelectColour (IT_PE.ASM 8856) + the inline default-channel path
 * (2585..2647): base row colour, block-mark override (view columns get
 * 96h/86h, the default channels 93h/83h), Ctrl-H cursor-row hilight. */
static uint8_t pe_select_colour(int row, int chan, int viewproc)
{
    uint8_t a = 0x06;

    if (RowHiLight2 && row % RowHiLight2 == 0)      a = 0xE6;
    else if (RowHiLight1 && row % RowHiLight1 == 0) a = 0xF6;

    if (BlockMark &&
        chan >= BlockLeft && chan <= BlockRight &&
        row >= BlockTop && row <= BlockBottom) {
        if (viewproc)
            return (uint8_t)((a & 0x80) ? 0x96 : 0x86);
        return (uint8_t)((a & 0x80) ? 0x93 : 0x83);
    }
    if (row == CurRow && (PEConfig & 2))
        return 0x16;
    return a;
}

/* one 13-wide cell: ViewFull (IT_PE.ASM 9074) == the default-channel
 * body in PE_DrawPatternEdit */
static int PEDefaultVolume = 0;     /* the pattern editor's Flags bit 0,
                                       Ctrl-V (PE_ToggleDefaultVolume) */

/* PE_DrawPattern (IT_PE.ASM 2669): with default volumes on, an empty
 * volume field under a note + instrument shows the sample's default
 * volume between chars 191/192 (".." when the instrument maps the note
 * to no sample). Returns 1 when it drew x+6..x+9. */
static int draw_default_volume(int x, int y, const editcell_t *cell,
                               uint8_t a)
{
    int smp;

    if (!PEDefaultVolume || (cell->mask & CM_VOL))
        return 0;
    if (!(cell->mask & CM_NOTE) || cell->note < 1 || cell->note > 120)
        return 0;
    if (!(cell->mask & CM_INS) || cell->ins == 0)
        return 0;
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        smp = Song.Ins[(cell->ins - 1) % 99]
                  .NoteSampleTable[(cell->note - 1) * 2 + 1];
    else
        smp = cell->ins;
    Screen_PutChar(x + 6, y, 191, a);
    if (smp >= 1 && smp <= 99)
        drawf(x + 7, y, a, "%02d", Song.Smp[smp - 1].Vol % 100);
    else
        fill(x + 7, y, 2, 173, a);
    Screen_PutChar(x + 9, y, 192, a);
    return 1;
}

static void draw_cell_full(int x, int y, const editcell_t *cell, uint8_t a)
{
    draw_note(x, y, cell, a);
    Screen_PutChar(x + 3, y, ' ', a);
    if (cell->mask & CM_INS)
        drawf(x + 4, y, a, "%02d", cell->ins % 100);
    else
        fill(x + 4, y, 2, 173, a);
    if (!draw_default_volume(x, y, cell, a)) {
        Screen_PutChar(x + 6, y, ' ', a);
        draw_volume(x + 7, y, cell, a);
        Screen_PutChar(x + 9, y, ' ', a);
    }
    if (cell->mask & CM_CMD)
        Screen_PutChar(x + 10, y, (uint8_t)('A' + cell->cmd - 1), a);
    else
        Screen_PutChar(x + 10, y, '.', a);
    drawf(x + 11, y, a, "%02X", (cell->mask & CM_CMD) ? cell->cmdval : 0);
}

/* ViewCompress (9209), 10 wide: full fields, no spaces; instrument,
 * command and value dim to attr-4 */
static void draw_cell_compress(int x, int y, const editcell_t *cell,
                               uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    draw_note(x, y, cell, a);
    if (cell->mask & CM_INS)
        drawf(x + 3, y, a4, "%02d", cell->ins % 100);
    else
        fill(x + 3, y, 2, 173, a4);
    draw_volume(x + 5, y, cell, a);
    if (cell->mask & CM_CMD)
        Screen_PutChar(x + 7, y, (uint8_t)('A' + cell->cmd - 1), a4);
    else
        Screen_PutChar(x + 7, y, '.', a4);
    drawf(x + 8, y, a4, "%02X", (cell->mask & CM_CMD) ? cell->cmdval : 0);
}

/* ViewAllSmall (9342), 7 wide: all fields, the numeric ones packed
 * into single cells via font bank B (attr bit 3; char = tens<<4|ones)
 * and the G/H volume effects via the small font-A glyphs 226..245 */
static void draw_cell_allsmall(int x, int y, const editcell_t *cell,
                               uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    draw_note(x, y, cell, a);

    if (cell->mask & CM_INS) {
        int v = cell->ins % 100;
        Screen_PutChar(x + 3, y, (uint8_t)(((v / 10) << 4) | (v % 10)),
                       (uint8_t)(a + 4));
    } else {
        Screen_PutChar(x + 3, y, 184, a4);
    }

    if (!(cell->mask & CM_VOL)) {
        Screen_PutChar(x + 4, y, 184, a);
    } else {
        uint8_t v = cell->vol;
        int eff = (v & 0x7F) - 65;
        if (eff >= 0) {
            if (v & 0x80)               /* Gx/Hx -> small glyph */
                Screen_PutChar(x + 4, y, (uint8_t)(226 + eff), a);
            else                        /* Ax..Fx packed: 0Ah+tens|ones */
                Screen_PutChar(x + 4, y,
                    (uint8_t)(((0x0A + eff / 10) << 4) | (eff % 10)),
                    (uint8_t)(a + 6));
        } else {
            uint8_t va = (uint8_t)(a + ((v & 0x80) ? 4 : 6));
            v &= 0x7F;
            Screen_PutChar(x + 4, y,
                (uint8_t)(((v / 10) << 4) | (v % 10)), va);
        }
    }

    if (cell->mask & CM_CMD)
        Screen_PutChar(x + 5, y, (uint8_t)('A' + cell->cmd - 1), a4);
    else
        Screen_PutChar(x + 5, y, '.', a4);
    /* raw value byte: font bank B renders it as its hex pair */
    Screen_PutChar(x + 6, y, (cell->mask & CM_CMD) ? cell->cmdval : 0,
                   (uint8_t)(a + 4));
}

/* ViewNote (9479), 3 wide: one field by priority note/ins/vol/cmd */
static void draw_cell_note(int x, int y, const editcell_t *cell, uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    if (cell->mask & CM_NOTE) {
        draw_note(x, y, cell, a);
    } else if (cell->mask & CM_INS) {
        Screen_PutChar(x, y, ' ', a);
        drawf(x + 1, y, a, "%02d", cell->ins % 100);
    } else if (cell->mask & CM_VOL) {
        Screen_PutChar(x, y, ' ', a4);
        draw_volume_small(x + 1, y, cell->vol, a);
    } else if (cell->mask & CM_CMD) {
        Screen_PutChar(x, y,
            cell->cmd ? (uint8_t)('A' + cell->cmd - 1) : '.', a4);
        drawf(x + 1, y, a4, "%02X", cell->cmdval);
    } else {
        fill(x, y, 3, 173, a);
    }
}

/* ViewNote cursor-row override (ViewNote12): the cursor column picks
 * which field the row shows regardless of priority */
static void draw_cell_note_cur(int x, int y, const editcell_t *cell,
                               uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    if (CurCol <= 1) {                  /* note field */
        if (!(cell->mask & CM_NOTE))
            fill(x, y, 3, 173, a);      /* else the normal pass drew it */
    } else if (CurCol <= 3) {           /* instrument */
        Screen_PutChar(x, y, ' ', a);
        if (cell->mask & CM_INS)
            drawf(x + 1, y, a, "%02d", cell->ins % 100);
        else
            fill(x + 1, y, 2, 173, a);
    } else if (CurCol <= 5) {           /* volume */
        Screen_PutChar(x, y, ' ', a4);
        if (cell->mask & CM_VOL)
            draw_volume_small(x + 1, y, cell->vol, a);
        else
            fill(x + 1, y, 2, 173, a4);
    } else {                            /* command + value */
        Screen_PutChar(x, y,
            (cell->mask & CM_CMD) && cell->cmd
                ? (uint8_t)('A' + cell->cmd - 1) : '.', a4);
        drawf(x + 1, y, a4, "%02X",
              (cell->mask & CM_CMD) ? cell->cmdval : 0);
    }
}

/* ViewTiny (9818), 2 wide */
static void draw_cell_tiny(int x, int y, const editcell_t *cell, uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    if (cell->mask & CM_NOTE) {
        draw_note2(x, y, cell, a);
    } else if (cell->mask & CM_INS) {
        drawf(x, y, a, "%02d", cell->ins % 100);
    } else if (cell->mask & CM_VOL) {
        draw_volume_small(x, y, cell->vol, a);
    } else if (cell->mask & CM_CMD) {
        Screen_PutChar(x, y,
            cell->cmd ? (uint8_t)('A' + cell->cmd - 1) : '.', a4);
        Screen_PutChar(x + 1, y, cell->cmdval, (uint8_t)(a + 4));
    } else {
        fill(x, y, 2, 173, a);
    }
}

/* ViewTiny cursor-row override (ViewTiny14) */
static void draw_cell_tiny_cur(int x, int y, const editcell_t *cell,
                               uint8_t a)
{
    uint8_t a4 = (uint8_t)(a - 4);

    if (CurCol <= 1) {
        if (!(cell->mask & CM_NOTE))
            fill(x, y, 2, 173, a);
    } else if (CurCol <= 3) {
        if (cell->mask & CM_INS)
            drawf(x, y, a, "%02d", cell->ins % 100);
        else
            fill(x, y, 2, 173, a);
    } else if (CurCol <= 5) {
        if (cell->mask & CM_VOL)
            draw_volume_small(x, y, cell->vol, a);
        else
            fill(x, y, 2, 173, a4);
    } else {
        Screen_PutChar(x, y,
            (cell->mask & CM_CMD) && cell->cmd
                ? (uint8_t)('A' + cell->cmd - 1) : '.', a4);
        Screen_PutChar(x + 1, y,
            (cell->mask & CM_CMD) ? cell->cmdval : 0, (uint8_t)(a + 4));
    }
}

/* PE_HilightView (8931): draw the cursor inside a view column whose
 * left edge is at screen x. Packed cells (font-bank-B attr, char 184,
 * or the small glyphs 226..245) get the half-cell invert via font-A
 * char 246; plain cells get attr 30h over the column's cell span
 * (CursorPositions high nibble = extra cells, low nibble = x offset). */
static void pe_hilight_view(int m, int x, int chan)
{
    int span = 1, y, cl, cx;
    screen_cell_t sc;

    if (Template && !ShiftHeld && CurCol == 0 && ClipData)
        span = ClipChans;
    if (chan < CurChan || chan >= CurChan + span)
        return;
    if (CurRow < TopRow || CurRow >= TopRow + 32)
        return;

    y  = 15 + (CurRow - TopRow);
    cl = CursorPositions[m][CurCol];
    cx = x + (cl & 0x0F);
    sc = Screen_GetCell(cx, y);
    if ((sc.attr & 8) || sc.ch == 184 ||
        (sc.ch >= 226 && sc.ch < 246)) {
        uint8_t mask = 0xF0;            /* right/units half */
        if (CurCol < 8 &&
            CursorPositions[m][CurCol] == CursorPositions[m][CurCol + 1])
            mask = 0x0F;                /* shares the cell with the next
                                         * column: left/tens half */
        Screen_InvertCursor(cx, y, mask);
    } else {
        int n;
        for (n = (cl >> 4) + 1; n > 0; n--, cx++)
            Screen_SetAttr(cx, y, 0x30);
    }
}

/* width-matched captions (ChannelMsg/2/7/4/5, IT_PE.ASM 863..869) */
static const char *const ViewCaptionFmt[5] = {
    " Channel %02d ", "Channel %02d", "Chnl %02d", " %02d", "%02d"
};

/* one ViewChannels entry: caption, 32 rows, cursor (the View* procs) */
static void draw_view_column(int m, int x, int chan)
{
    int screeny;
    int maxrow = (int)CurRows - 1;
    uint8_t ca = (Song.Header.ChnlPan[chan] & 0x80) ? 0x10 : 0x13;

    drawf(x, 14, ca, ViewCaptionFmt[m], chan + 1);

    for (screeny = 0; screeny < 32; screeny++) {
        int row = TopRow + screeny;
        const editcell_t *cell;
        uint8_t a;

        if (row > maxrow)
            break;
        cell = &Grid[row * 64 + chan];
        a = pe_select_colour(row, chan, 1);
        switch (m) {
        case 0:  draw_cell_full(x, 15 + screeny, cell, a);     break;
        case 1:  draw_cell_compress(x, 15 + screeny, cell, a); break;
        case 2:  draw_cell_allsmall(x, 15 + screeny, cell, a); break;
        case 3:  draw_cell_note(x, 15 + screeny, cell, a);     break;
        default: draw_cell_tiny(x, 15 + screeny, cell, a);     break;
        }
    }

    if ((m == 3 || m == 4) && chan == CurChan &&
        CurRow >= TopRow && CurRow < TopRow + 32) {
        const editcell_t *cell = &Grid[CurRow * 64 + chan];
        uint8_t a = pe_select_colour(CurRow, chan, 1);
        if (m == 3)
            draw_cell_note_cur(x, 15 + (CurRow - TopRow), cell, a);
        else
            draw_cell_tiny_cur(x, 15 + (CurRow - TopRow), cell, a);
    }

    pe_hilight_view(m, x, chan);
}

static void draw_pattern(void)
{
    int ch, i, screeny;
    int maxrow = (int)CurRows - 1;
    int gutterx, defx0;

    /* row/block clamps (PE_DrawPatternEdit 2189..2207) */
    if (CurRow > maxrow) CurRow = maxrow;
    if (BlockTop > maxrow) BlockTop = maxrow;
    if (BlockBottom > maxrow) BlockBottom = maxrow;

    /* ViewChannelTracking scroll (2322..2398): when the cursor channel
     * is outside the view list, shift every entry towards it (clamped
     * to 0..63) so the cursor channel scrolls into a view column */
    if (ViewTracking && (ViewChannels[0] & 0xFF) != 0xFF) {
        int minc = ViewChannels[0] & 0xFF, maxc = minc, found = 0;

        for (i = 0; i < 100 && (ViewChannels[i] & 0xFF) != 0xFF; i++) {
            int c = ViewChannels[i] & 0xFF;
            if (c == CurChan) { found = 1; break; }
            if (c < minc) minc = c;
            if (c > maxc) maxc = c;
        }
        if (!found) {
            int delta = (maxc <= CurChan) ? CurChan - maxc
                                          : CurChan - minc;
            for (i = 0; i < 100 && (ViewChannels[i] & 0xFF) != 0xFF; i++) {
                int c = (ViewChannels[i] & 0xFF) + delta;
                if (c < 0)  c = 0;
                if (c > 63) c = 63;
                ViewChannels[i] =
                    (uint16_t)((ViewChannels[i] & 0xFF00) | c);
            }
        }
    }

    /* TopRow window (PE_DrawPatternEditNormal); centralise mode
     * (PEConfig bit 0) pins the cursor to the middle */
    if (PEConfig & 1) {
        TopRow = CurRow - 16;
    } else {
        if (TopRow > CurRow) TopRow = CurRow;
        if (TopRow + 32 <= CurRow) TopRow = CurRow - 31;
    }
    if (TopRow > maxrow - 31) TopRow = maxrow - 31;
    if (TopRow < 0) TopRow = 0;

    if (NumChansEdit > 0) {
        if (LeftChan > CurChan)
            LeftChan = CurChan;
        if (LeftChan + NumChansEdit <= CurChan)
            LeftChan = CurChan - NumChansEdit + 1;
    }

    gutterx = NumChansEdit ? 1 + ViewWidth : 1;
    defx0   = 5 + ViewWidth;

    /* boxes (2209..2248) */
    if (ViewWidth) {
        int cx = NumChansEdit ? 0 : 3;
        Screen_DrawBox(1 + cx, 14, ViewWidth + cx, 47, 27);
    }
    if (NumChansEdit) {
        Screen_DrawBox(4 + ViewWidth, 14,
                       4 + ViewWidth + 14 * NumChansEdit, 47, 27);

        /* default-channel headers, attr 13h / 10h muted */
        for (ch = 0; ch < NumChansEdit; ch++) {
            int c = LeftChan + ch;
            uint8_t a = (Song.Header.ChnlPan[c] & 0x80) ? 0x10 : 0x13;
            char hdr[24];               /* worst-case %02d width; c+1 <= 64 */
            snprintf(hdr, sizeof(hdr), " Channel %02d ", c + 1);
            Screen_DrawString(defx0 + 14 * ch, 14, hdr, a);
        }
    }

    /* row-number gutter; PlayMark row in attr B0h (2437..2472) */
    for (screeny = 0; screeny < 32; screeny++) {
        int row = TopRow + screeny;
        uint8_t a = 0x20;

        if (row > maxrow)
            break;
        if (PlayMarkOn && PlayMarkPattern == (int)CurPattern &&
            row == PlayMarkRow)
            a = 0xB0;
        draw3num(gutterx, 15 + screeny, row, a);
    }

    /* view columns left to right (2478..2554); char-168 divider
     * columns between entries when ViewDivision is on */
    {
        int x = NumChansEdit ? 2 : 5;

        for (i = 0; i < 100 && (ViewChannels[i] & 0xFF) != 0xFF; i++) {
            int m = (ViewChannels[i] >> 8) & 7;

            draw_view_column(m, x, ViewChannels[i] & 0xFF);
            x += ViewMethodWidth[m];
            if (i + 1 < 100 && (ViewChannels[i + 1] & 0xFF) != 0xFF &&
                ViewDivision) {
                for (screeny = 0; screeny < 32 &&
                                  TopRow + screeny <= maxrow; screeny++)
                    Screen_PutChar(x, 15 + screeny, 168, 0x02);
                x++;
            }
        }
    }

    /* default channels (2564..2842) */
    if (NumChansEdit > 0) {
        for (screeny = 0; screeny < 32; screeny++) {
            int row = TopRow + screeny;
            int y = 15 + screeny;

            if (row > maxrow)
                break;

            for (ch = 0; ch < NumChansEdit; ch++) {
                int c = LeftChan + ch;
                int x = defx0 + 14 * ch;
                uint8_t a = pe_select_colour(row, c, 0);

                draw_cell_full(x, y, &Grid[row * 64 + c], a);

                if (ch < NumChansEdit - 1) {    /* divider, char 168:
                     * background kept except over marked blocks
                     * (attr byte 80h..9Fh drops to plain 02h) */
                    uint8_t da = (uint8_t)((a & 0xF0) | 2);
                    if (da >= 0x80 && da < 0xA0)
                        da = 0x02;
                    Screen_PutChar(x + 13, y, 168, da);
                }
            }
        }
    }

    /* playing-row hilight on the gutter digits (2844..2884) */
    if (PlayMode != 0 && CurrentPattern == CurPattern) {
        int r = (int)CurrentRow - TopRow;
        if (r >= 0 && r < 32 && TopRow + r <= maxrow) {
            for (i = 0; i < 3; i++) {
                uint8_t at = Screen_GetAttr(gutterx + i, 15 + r);
                Screen_SetAttr(gutterx + i, 15 + r,
                               (uint8_t)((at & 0xF0) | 3));
            }
        }
    }

    /* default-channel cursor (PE_PrePatternEdit 2903..2924): ONE cell,
     * attr := 30h at the CursorPositions[0] offset */
    if (NumChansEdit > 0 &&
        CurChan >= LeftChan && CurChan < LeftChan + NumChansEdit &&
        CurRow >= TopRow && CurRow < TopRow + 32) {
        Screen_SetAttr(defx0 + 14 * (CurChan - LeftChan) +
                           CursorPositions[0][CurCol],
                       15 + (CurRow - TopRow), 0x30);
    }

    /* the bottom edge, row 47 (PE_PrePatternEdit 2926..3106, #34) */
    if (NumChansEdit > 0) {
        int ch, dx = 1, cl = CurCol;
        /* multichannel 'M's (char 172) over the visible channels */
        for (ch = 0; ch < NumChansEdit && LeftChan + ch < 64; ch++)
            if (MultiChannelInfo[LeftChan + ch])
                Screen_PutChar(defx0 + 3 + 14 * ch, 47, 172,
                               Screen_GetAttr(defx0 + 3 + 14 * ch, 47));
        /* the edit mask under the cursor channel -- or under every
         * channel the block covers while a template is on */
        if (Template && ClipData && ClipChans > 0) {
            dx = NumChansEdit + LeftChan - CurChan;
            if (dx > ClipChans)
                dx = ClipChans;
        }
        if (CurChan >= LeftChan && CurChan < LeftChan + NumChansEdit) {
            int x = defx0 + 14 * (CurChan - LeftChan);
            for (ch = 0; ch < dx; ch++, x += 14) {
                /* note group A9h (AAh when the cursor is elsewhere), then
                 * instrument / volume / command groups per EditMask */
                uint8_t g = cl ? 0xAA : 0xA9;
                for (i = 0; i < 3; i++) Screen_PutChar(x + i, 47, g, 0x23);
                if (EditMask & 1)
                    for (i = 4; i < 6; i++) Screen_PutChar(x + i, 47, g, 0x23);
                if (EditMask & 2)
                    for (i = 7; i < 9; i++) Screen_PutChar(x + i, 47, g, 0x23);
                if (EditMask & 4)
                    for (i = 10; i < 13; i++) Screen_PutChar(x + i, 47, g, 0x23);
                if (cl == 0) {                  /* cursor on the note */
                    for (i = 0; i < 3; i++)
                        Screen_PutChar(x + i, 47, 0xAB, 0x23);
                    continue;
                }
                /* the cursor's own cells: ABh where the mask is on (the
                 * octave always), A9h otherwise; first channel only */
                if (cl == 1)
                    Screen_PutChar(x + 2, 47, 0xAB, 0x23);
                else if (cl <= 3) {
                    g = (EditMask & 1) ? 0xAB : 0xA9;
                    Screen_PutChar(x + 4, 47, g, 0x23);
                    Screen_PutChar(x + 5, 47, g, 0x23);
                } else if (cl <= 5) {
                    g = (EditMask & 2) ? 0xAB : 0xA9;
                    Screen_PutChar(x + 7, 47, g, 0x23);
                    Screen_PutChar(x + 8, 47, g, 0x23);
                } else if (cl == 6) {
                    Screen_PutChar(x + 10, 47, (EditMask & 4) ? 0xAB : 0xA9,
                                   0x23);
                } else {
                    g = (EditMask & 4) ? 0xAB : 0xA9;
                    Screen_PutChar(x + 11, 47, g, 0x23);
                    Screen_PutChar(x + 12, 47, g, 0x23);
                }
                break;
            }
        }
    }
}

/* ===================================================================
 * Sample list (F3) -- object coordinates from IT_OBJ1.ASM
 * =================================================================== */
static const uint8_t InstParamText[] =
    "Filename\015   Speed\015    Loop\015 LoopBeg\015 LoopEnd\015"
    " SusLoop\015 SusLBeg\015 SusLEnd\015"
    "\377\010 \376\041\222\376\003\377\015\232\376\040\015"
    " Quality\015  Length";

static int SmpListTop, InsListTop, OrdListTop;
static int OrderCursor;                 /* F11 digit cursor, 0..2 */

/* PE_GetMaxOrder (IT_PE.ASM 1109): index of the first 0FFh terminator
 * minus one, floored at 0 (255 if the list is full) */
static int max_order(void)
{
    int i;

    for (i = 0; i < 256; i++)
        if (Song.Orders[i] == 0xFF)
            break;
    return i > 0 ? i - 1 : 0;
}
static int PanSel;                              /* selected pan channel */
/* in-list name editing (feature 013; IT_I.ASM I_PostSampleList /
 * I_PostInstrumentWindow): F3 cursor position within the sample name
 * (25 = right stop = keyjazz mode); F4 uses a Spacebar-toggled edit
 * mode with its own position. */
static int SamplePos = 25;                      /* IT_I.ASM 290 */
static int InstrumentPos = 0;                   /* 242 */
static int InstrumentEdit = 0;                  /* 273 */

/* ===================================================================
 * Sample editor core (IT_I.ASM): the F3 waveform view, loop editing
 * and the Alt-key destructive operations. IT 2.17 has no freehand
 * draw / selection / zoom -- the authentic surface is exactly this
 * (README fidelity notes).
 * =================================================================== */
static void draw_screen(void);
static void stop_song(void);
static void commit_current_pattern(void);
static void load_pattern(uint16_t pat);

#define SMP_MAXBYTES 4177920u           /* I_ReMix size cap */
#define SMP_PAD      32                 /* interpolator padding, as loader */

static sample_t *cur_smp(void) { return &Song.Smp[ListSel]; }

static int smp_has_data(const sample_t *s)
{
    return (s->Flags & 1) && s->Data && s->Length;
}

/* generic Yes/No confirm (O1_Confirm*List), default No */
/* Mouse for the port's modal dialogs. They run their own key loops, so
 * without this they ignored the mouse entirely -- the original's
 * M_Object1List gives every object in every dialog click (buttons) and
 * click+drag (thumbbars). */
static int mouse_in(const it_mouse_t *m, int x0, int y0, int x1, int y1)
{
    return m->x >= x0 && m->x <= x1 && m->y >= y0 && m->y <= y1;
}

/* a click on a plain thumbbar at (barx,bary): 1 when it hit the bar,
 * *v = the pointer's value (thumb_from_px mapping: one logical pixel per
 * step from 4px into the bar) */
static int thumb_hit(const it_mouse_t *m, int barx, int bary, int min,
                     int max, int *v)
{
    int width = (max - min + 15) >> 3;
    int r;

    if (m->y != bary || m->x < barx || m->x >= barx + width)
        return 0;
    r = min + (m->px - (barx * 8 + 4));
    *v = r < min ? min : r > max ? max : r;
    return 1;
}

static int confirm_box_bg(const char *text, int default_yes,
                          void (*bg)(void))
{
    int sel = default_yes ? 0 : 1;

    for (;;) {
        int key, tx;

        bg();
        /* ConfirmOverWriteBox (26,25)-(54,32) style 3, the text on row 27
         * (O1_ConfirmQuit & friends), and the two raised style-8 buttons
         * ConfirmOverWriteOKButton (30,29)-(39,31) "   OK" and
         * ConfirmOverWriteCancelButton (41,29)-(50,31) " Cancel"
         * (IT_OBJ1.ASM; issue #5). Texts too long for that box get
         * ConfirmNosaveBox's wider (20,25)-(60,32). */
        if ((int)strlen(text) > 27)
            Screen_DrawBox(20, 25, 60, 32, 3);
        else
            Screen_DrawBox(26, 25, 54, 32, 3);
        tx = 40 - (int)strlen(text) / 2;
        Screen_DrawString(tx, 27, text, 0x20);
        draw_button_style(30, 29, 39, 31, 8, "   OK", 0, sel == 0);
        draw_button_style(41, 29, 50, 31, 8, " Cancel", 0, sel == 1);
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 0;
        case ITK_LEFT: case ITK_RIGHT: case ITK_TAB: case ITK_SHIFT_TAB:
            sel ^= 1; break;
        case 'y': case 'Y': case 'o': case 'O': return 1;   /* OKCancelList */
        case 'n': case 'N': case 'c': case 'C': case ITK_ESC: return 0;
        case ITK_ENTER: return sel == 0;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 30, 29, 39, 31)) return 1;     /* OK */
            if (mouse_in(&m, 41, 29, 50, 31)) return 0;     /* Cancel */
            break; }
        default: break;
        }
    }
}

static int confirm_box_def(const char *text, int default_yes)
{
    return confirm_box_bg(text, default_yes, draw_screen);
}

static int confirm_box(const char *text)
{
    return confirm_box_def(text, 0);
}

/* numeric prompt (GetNumberInput / O1_*List): returns -1 on cancel */
static long prompt_number(const char *title, unsigned long def,
                          unsigned long maxv)
{
    char buf[12];
    int len;

    snprintf(buf, sizeof(buf), "%lu", def);
    len = (int)strlen(buf);

    for (;;) {
        int key;

        draw_screen();
        Screen_DrawBox(24, 22, 55, 27, 3);      /* numeric prompt: tan panel */
        Screen_DrawString(26, 23, title, 0x20);
        drawf(26, 25, 0x30, "%-10.10s", buf);
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return -1;
        case ITK_ESC:  return -1;
        case ITK_BACKSPACE:
            if (len > 0)
                buf[--len] = 0;
            break;
        case ITK_ENTER: {
            unsigned long v = strtoul(buf, NULL, 10);
            if (v > maxv)
                v = maxv;
            return (long)v;
        }
        default:
            if (key >= '0' && key <= '9' && len < 9) {
                buf[len++] = (char)key;
                buf[len] = 0;
            }
            break;
        }
    }
}

/* O1_ConfirmConvert2List: 0 cancel, 1 convert data, 2 adjust fields */
static int quality_dialog(int to16)
{
    int sel = 0;

    for (;;) {
        int key;

        draw_screen();
        Screen_DrawBox(18, 21, 61, 28, 3);      /* ConfirmConvert: tan panel */
        drawf(24, 22, 0x20, "Convert sample to %d bit?", to16 ? 16 : 8);
        draw_button_style(21, 24, 34, 26, 3, " Convert data", 0, sel == 0);
        draw_button_style(36, 24, 49, 26, 3, " Adjust  end", 0, sel == 1);
        draw_button_style(51, 24, 59, 26, 3, " Cancel", 0, sel == 2);
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 0;
        case ITK_ESC:  return 0;
        case ITK_LEFT:  sel = (sel + 2) % 3; break;
        case ITK_RIGHT: case ITK_TAB: sel = (sel + 1) % 3; break;
        case ITK_ENTER: return sel == 0 ? 1 : sel == 1 ? 2 : 0;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 21, 24, 34, 26)) return 1;     /* Convert */
            if (mouse_in(&m, 36, 24, 49, 26)) return 2;     /* Adjust */
            if (mouse_in(&m, 51, 24, 59, 26)) return 0;     /* Cancel */
            break; }
        default: break;
        }
    }
}

/* ---- waveform view: I_DrawWaveForm (IT_I.ASM 1846) ----------------- */

static uint8_t WavePix[176 * 32];

static void wave_marker(uint32_t beg, uint32_t end, uint32_t len,
                        int twopix)
{
    uint32_t cb = (uint32_t)(((uint64_t)175 * beg + len / 2) / len);
    uint32_t ce = (uint32_t)(((uint64_t)175 * end + len / 2) / len);
    int r;

    for (r = 0; r < 32; r++) {
        uint8_t v = twopix ? (uint8_t)(((r + 1) >> 1) & 1)
                           : (uint8_t)(1 - (r & 1));
        if (cb < 176)
            WavePix[r * 176 + cb] = v;
        if (ce < 176)
            WavePix[r * 176 + ce] = v;
    }
}

static void draw_waveform(void)
{
    const sample_t *s = cur_smp();

    memset(WavePix, 0, sizeof(WavePix));

    if (smp_has_data(s)) {
        int is16 = (s->Flags & 2) != 0;
        int step = is16 ? 2 : 1;
        const uint8_t *base = (const uint8_t *)s->Data + (is16 ? 1 : 0);
        uint32_t len = s->Length;
        uint64_t stepfx = ((uint64_t)len << 16) / 176;
        uint64_t posfx = 0;
        int prevmin = -128, prevmax = 127;  /* LastWaveformValues 7F80h */
        int col;

        for (col = 0; col < 176; col++) {
            uint64_t nextfx = posfx + stepfx;
            uint32_t i0 = (uint32_t)(posfx >> 16);
            uint32_t i1 = (uint32_t)(nextfx >> 16);
            int8_t mn, mx;
            int lo, hi, cnt, row;
            uint32_t i;

            if (i0 >= len)
                i0 = len - 1;
            mn = mx = (int8_t)base[(size_t)i0 * step];
            for (i = i0; i < i1 && i < len; i++) {
                int8_t v = (int8_t)base[(size_t)i * step];
                if (v < mn)
                    mn = v;
                else if (v > mx)
                    mx = v;
            }
            /* join with the previous column (XChg LastWaveformValues) */
            lo = mn; hi = mx;
            if (lo > prevmax) lo = prevmax;
            if (hi < prevmin) hi = prevmin;
            prevmin = mn; prevmax = mx;

            /* row map: byte-exact SAR/Add AX,202h/SAR (incl. the AL->AH
             * carry when min>>1 is -1 or -2 -- authentic) */
            {
                uint8_t al = (uint8_t)((int8_t)lo >> 1);
                uint8_t ah = (uint8_t)((int8_t)hi >> 1);
                unsigned ax = (unsigned)((ah << 8) | al) + 0x202;
                int rlo, rhi;
                ah = (uint8_t)((int8_t)(ax >> 8) >> 2);
                al = (uint8_t)((int8_t)(ax & 0xFF) >> 2);
                rhi = (int8_t)ah;
                rlo = (int8_t)al;
                cnt = rhi - rlo + 1;
                row = 16 - rhi;
                if (row == 32)
                    row = 31;
            }
            for (; cnt > 0; cnt--, row++)
                if (row >= 0 && row < 32)
                    WavePix[row * 176 + col] = 1;

            posfx = nextfx;
        }

        if (s->Flags & 0x10)
            wave_marker(s->LoopBeg, s->LoopEnd, len, 1);
        if (s->Flags & 0x20)
            wave_marker(s->SusLoopBeg, s->SusLoopEnd, len, 0);
    }

    Screen_GenerateCharacters(1, 22, 4, WavePix);
}

/* ---- loop clamps: I_CheckLoopValues / I_CheckSusLoopValues --------- */

static void smp_refresh_loops(void)     /* Music_RegetLoopInformation */
{
    int i;
    ed_lock();
    for (i = 0; i < MAXSLAVECHANNELS; i++)
        if (SChn[i].Flags & SF_CHAN_ON)
            GetLoopInformation(&SChn[i]);
    ed_unlock();
}

static void smp_check_loop(void)
{
    sample_t *s = cur_smp();
    uint32_t lim = s->Length ? s->Length - 1 : 0;

    if (s->LoopBeg > lim)
        s->LoopBeg = lim;
    if (s->LoopEnd > lim + 1)
        s->LoopEnd = lim + 1;
    if (s->LoopEnd <= s->LoopBeg)
        s->Flags &= (uint8_t)~0x10;
    smp_refresh_loops();
}

static void smp_check_susloop(void)
{
    sample_t *s = cur_smp();
    uint32_t lim = s->Length ? s->Length - 1 : 0;

    if (s->SusLoopBeg > lim)
        s->SusLoopBeg = lim;
    if (s->SusLoopEnd > lim + 1)
        s->SusLoopEnd = lim + 1;
    if (s->SusLoopEnd <= s->SusLoopBeg)
        s->Flags &= (uint8_t)~0x20;
    smp_refresh_loops();
}

static void smp_check_both(void)
{
    smp_check_loop();
    smp_check_susloop();
}

/* ---- sample memory (Music_AllocateSample / ReleaseSample) ---------- */

static void *smp_alloc(uint32_t bytes)
{
    void *p;
    if (bytes > SMP_MAXBYTES)
        bytes = SMP_MAXBYTES;
    p = malloc((size_t)bytes + SMP_PAD);
    if (p)
        memset((uint8_t *)p + bytes, 0, SMP_PAD);
    return p;
}

static void smp_free_data(sample_t *s)
{
    ed_lock();
    free(s->Data);
    s->Data = NULL;
    s->Flags &= (uint8_t)~1;
    ed_unlock();
}

/* ---- destructive ops (research R3, transliterated) ----------------- */

static void smp_op_convert(void)        /* Alt-A: I_ConvertSample */
{
    sample_t *s = cur_smp();
    uint32_t i, n;
    uint8_t *p;

    if (!smp_has_data(s))
        return;
    if (!confirm_box("Convert between signed/unsigned?"))
        return;
    stop_song();
    ed_lock();
    n = s->Length;
    p = (uint8_t *)s->Data + ((s->Flags & 2) ? 1 : 0);
    for (i = 0; i < n; i++)             /* 16-bit: high bytes only */
        p[(size_t)i * ((s->Flags & 2) ? 2 : 1)] ^= 0x80;
    ed_unlock();
}

static void smp_op_invert(void)         /* Alt-I: I_InvertSample */
{
    sample_t *s = cur_smp();
    uint32_t i, n;

    if (!smp_has_data(s))
        return;
    stop_song();
    ed_lock();
    n = s->Length;
    if (s->Flags & 2) {
        int16_t *p = (int16_t *)s->Data;
        for (i = 0; i < n; i++)
            p[i] = (int16_t)-p[i];
    } else {
        int8_t *p = (int8_t *)s->Data;
        for (i = 0; i < n; i++)
            p[i] = (int8_t)-p[i];
    }
    ed_unlock();
}

static void smp_op_centre(void)         /* Alt-H: I_CenterSample */
{
    sample_t *s = cur_smp();
    uint32_t i, n;
    long off;
    char msg[48];

    if (!smp_has_data(s))
        return;
    n = s->Length;
    if (s->Flags & 2) {
        const int16_t *p = (const int16_t *)s->Data;
        int16_t mn = p[0], mx = p[0];
        for (i = 0; i < n; i++) {
            if (p[i] < mn) mn = p[i];
            else if (p[i] > mx) mx = p[i];
        }
        off = -((long)mn + mx) >> 1;
    } else {
        const int8_t *p = (const int8_t *)s->Data;
        int8_t mn = p[0], mx = p[0];
        for (i = 0; i < n; i++) {
            if (p[i] < mn) mn = p[i];
            else if (p[i] > mx) mx = p[i];
        }
        off = -((long)mn + mx) >> 1;
    }
    snprintf(msg, sizeof(msg), "Centre sample (DC offset %ld)?", off);
    if (!confirm_box(msg))
        return;
    stop_song();
    ed_lock();
    if (s->Flags & 2) {
        int16_t *p = (int16_t *)s->Data;
        for (i = 0; i < n; i++)
            p[i] = (int16_t)(p[i] + off);
    } else {
        int8_t *p = (int8_t *)s->Data;
        for (i = 0; i < n; i++)
            p[i] = (int8_t)(p[i] + off);
    }
    ed_unlock();
}

static int sample_amp_dialog(int *val);   /* O1_SampleAmplificationList */

static void smp_op_amplify(void)        /* Alt-M: I_AmplifySample */
{
    sample_t *s = cur_smp();
    uint32_t i, n;
    unsigned dev = 0, sug;
    long amp;
    uint32_t mult;

    if (!smp_has_data(s))
        return;
    n = s->Length;
    if (s->Flags & 2) {
        const int16_t *p = (const int16_t *)s->Data;
        for (i = 0; i < n; i++) {
            int v = p[i] < 0 ? -p[i] : p[i];
            if ((unsigned)v > dev)
                dev = (unsigned)v;
        }
    } else {
        const int8_t *p = (const int8_t *)s->Data;
        for (i = 0; i < n; i++) {
            int v = p[i] < 0 ? -p[i] : p[i];
            if ((unsigned)v > dev)
                dev = (unsigned)v;
        }
        dev <<= 8;                      /* BH-position, as the original */
    }
    sug = (dev > 0x32) ? (unsigned)(0x320000u / dev) : 400;
    if (sug >= 400)
        sug = 400;
    {                                   /* O1_SampleAmplificationList */
        int v = (int)sug;
        if (!sample_amp_dialog(&v))
            return;
        amp = v;
    }
    stop_song();
    mult = (uint32_t)(((uint64_t)(unsigned long)amp << 16) / 100);
    ed_lock();
    if (s->Flags & 2) {
        int16_t *p = (int16_t *)s->Data;
        for (i = 0; i < n; i++) {
            int64_t v = ((int64_t)p[i] * (int64_t)mult + 0x8000) >> 16;
            if (v > 0x7FFF) v = 0x7FFF;
            if (v < -0x8000) v = -0x8000;
            p[i] = (int16_t)v;
        }
    } else {
        int8_t *p = (int8_t *)s->Data;
        for (i = 0; i < n; i++) {
            int32_t v = (int32_t)(((int64_t)p[i] * (int64_t)mult
                                   + 0x8000) >> 16);
            if (v > 0x7F) v = 0x7F;
            if (v < -0x80) v = -0x80;
            p[i] = (int8_t)v;
        }
    }
    ed_unlock();
}

static void smp_op_reverse(void)        /* Alt-G: I_ReverseSample */
{
    sample_t *s = cur_smp();
    uint32_t n, t;

    if (!smp_has_data(s))
        return;
    stop_song();
    ed_lock();
    n = s->Length;
    if (s->Flags & 2) {
        int16_t *p = (int16_t *)s->Data;
        uint32_t a = 0, b = n - 1;
        while (a < b) {
            int16_t x = p[a]; p[a] = p[b]; p[b] = x;
            a++; b--;
        }
    } else {
        int8_t *p = (int8_t *)s->Data;
        uint32_t a = 0, b = n - 1;
        while (a < b) {
            int8_t x = p[a]; p[a] = p[b]; p[b] = x;
            a++; b--;
        }
    }
    t = s->LoopBeg;                     /* mirror both loops */
    s->LoopBeg = n - s->LoopEnd;
    s->LoopEnd = n - t;
    t = s->SusLoopBeg;
    s->SusLoopBeg = n - s->SusLoopEnd;
    s->SusLoopEnd = n - t;
    ed_unlock();
    smp_refresh_loops();
}

static void smp_op_cut_before(void)     /* Alt-B: I_CutSampleBeforeLoop */
{
    sample_t *s = cur_smp();
    uint32_t cut, bytes;

    if (!smp_has_data(s) || s->LoopBeg == 0)
        return;
    if (!confirm_box("Cut sample before loop?"))
        return;
    stop_song();
    ed_lock();
    cut = s->LoopBeg;
    if ((s->Flags & 0x20) && s->SusLoopBeg < cut)
        cut = s->SusLoopBeg;
    s->LoopBeg    = (s->LoopBeg    > cut) ? s->LoopBeg    - cut : 0;
    s->LoopEnd    = (s->LoopEnd    > cut) ? s->LoopEnd    - cut : 0;
    s->SusLoopBeg = (s->SusLoopBeg > cut) ? s->SusLoopBeg - cut : 0;
    s->SusLoopEnd = (s->SusLoopEnd > cut) ? s->SusLoopEnd - cut : 0;
    s->Length -= cut;
    bytes = s->Length << ((s->Flags & 2) ? 1 : 0);
    memmove(s->Data,
            (uint8_t *)s->Data + ((size_t)cut << ((s->Flags & 2) ? 1 : 0)),
            bytes);
    memset((uint8_t *)s->Data + bytes, 0, SMP_PAD);
    ed_unlock();
    smp_check_both();
}

static void smp_op_cut_after(void)      /* Alt-L: I_CutSample */
{
    sample_t *s = cur_smp();
    uint32_t end;

    if (!smp_has_data(s) || s->LoopEnd == 0)
        return;
    if (!confirm_box("Cut sample after loop?"))
        return;
    stop_song();
    ed_lock();
    end = s->LoopEnd;
    if (s->SusLoopEnd > end)
        end = s->SusLoopEnd;
    s->Length = end;
    ed_unlock();
    smp_check_both();
}

/* Alt-E / Alt-F: I_ResizeSample(NoInt) via I_ReMix */
static void smp_op_resize(int interpolate)
{
    sample_t *s = cur_smp();
    long nl;
    uint32_t newlen, oldlen;
    int is16;
    void *nd;

    if (!smp_has_data(s))
        return;
    nl = prompt_number(interpolate ? "Resize sample to (interpolated)"
                                   : "Resize sample to (no interpolation)",
                       s->Length, 9999999);
    if (nl <= 0)
        return;
    is16 = (s->Flags & 2) != 0;
    newlen = (uint32_t)nl;
    if ((newlen << is16) > SMP_MAXBYTES)
        newlen = SMP_MAXBYTES >> is16;
    oldlen = s->Length;

    nd = smp_alloc(newlen << is16);
    if (!nd) {
        status("Out of memory");
        return;
    }
    stop_song();
    ed_lock();
    {
        /* 16.16 source step = old/new (BP:BX in the original) */
        uint64_t stepfx = (((uint64_t)oldlen << 16) / newlen);
        uint64_t pos = 0;
        uint32_t i;

        if (is16) {
            const int16_t *src = (const int16_t *)s->Data;
            int16_t *dst = (int16_t *)nd;
            for (i = 0; i < newlen; i++) {
                uint32_t si = (uint32_t)(pos >> 16);
                if (si >= oldlen)
                    si = oldlen - 1;
                if (interpolate) {
                    uint32_t f = ((uint32_t)pos >> 8) & 0xFF;
                    int32_t s0 = src[si];
                    int32_t s1 = src[si + 1 < oldlen ? si + 1 : si];
                    dst[i] = (int16_t)((s0 * (int32_t)(256 - f)
                                        + s1 * (int32_t)f + 0x80) >> 8);
                } else
                    dst[i] = src[si];
                pos += stepfx;
            }
        } else {
            const int8_t *src = (const int8_t *)s->Data;
            int8_t *dst = (int8_t *)nd;
            for (i = 0; i < newlen; i++) {
                uint32_t si = (uint32_t)(pos >> 16);
                if (si >= oldlen)
                    si = oldlen - 1;
                if (interpolate) {
                    uint32_t f = ((uint32_t)pos >> 8) & 0xFF;
                    int32_t s0 = src[si];
                    int32_t s1 = src[si + 1 < oldlen ? si + 1 : si];
                    dst[i] = (int8_t)((s0 * (int32_t)(256 - f)
                                       + s1 * (int32_t)f + 0x80) >> 8);
                } else
                    dst[i] = src[si];
                pos += stepfx;
            }
        }
        free(s->Data);
        s->Data = nd;
        s->Length = newlen;
        /* scale the loop points by new/old, cap 9999999 (I_ReMix5) */
        {
            uint32_t *pts[4];
            int k;
            pts[0] = &s->LoopBeg;    pts[1] = &s->LoopEnd;
            pts[2] = &s->SusLoopBeg; pts[3] = &s->SusLoopEnd;
            for (k = 0; k < 4; k++) {
                uint64_t v = (uint64_t)*pts[k] * newlen / oldlen;
                *pts[k] = (v > 9999999u) ? 9999999u : (uint32_t)v;
            }
            /* C5 speed scaled too (offset 3Ch is among the 5 dwords
             * from +34h in the original: 34,38,3C,40,44) */
            {
                uint64_t v = (uint64_t)s->C5Speed * newlen / oldlen;
                s->C5Speed = (v > 9999999u) ? 9999999u : (uint32_t)v;
            }
        }
    }
    ed_unlock();
    smp_check_both();
}

static void smp_op_quality(void)        /* Alt-Q: I_ToggleSampleQuality */
{
    sample_t *s = cur_smp();
    int mode, is16;
    uint32_t i, n;

    if (!smp_has_data(s))
        return;
    is16 = (s->Flags & 2) != 0;
    mode = quality_dialog(!is16);
    if (!mode)
        return;
    stop_song();
    ed_lock();
    n = s->Length;
    if (mode == 2) {                    /* adjust fields (reinterpret) */
        if (is16) {
            s->Flags &= (uint8_t)~2;
            s->Length <<= 1; s->LoopBeg <<= 1; s->LoopEnd <<= 1;
            s->SusLoopBeg <<= 1; s->SusLoopEnd <<= 1;
        } else {
            s->Flags |= 2;
            s->Length >>= 1; s->LoopBeg >>= 1; s->LoopEnd >>= 1;
            s->SusLoopBeg >>= 1; s->SusLoopEnd >>= 1;
        }
    } else if (is16) {                  /* convert 16 -> 8: high bytes */
        void *nd = smp_alloc(n);
        if (nd) {
            const int16_t *src = (const int16_t *)s->Data;
            int8_t *dst = (int8_t *)nd;
            for (i = 0; i < n; i++)
                dst[i] = (int8_t)(src[i] >> 8);
            free(s->Data);
            s->Data = nd;
            s->Flags &= (uint8_t)~2;
        } else
            status("Out of memory");
    } else {                            /* convert 8 -> 16: v << 8 */
        void *nd = smp_alloc(n * 2);
        if (nd) {
            const int8_t *src = (const int8_t *)s->Data;
            int16_t *dst = (int16_t *)nd;
            for (i = 0; i < n; i++)
                dst[i] = (int16_t)(src[i] << 8);
            free(s->Data);
            s->Data = nd;
            s->Flags |= 2;
        } else
            status("Out of memory");
    }
    ed_unlock();
    smp_check_both();
}

static void smp_op_delete(void)         /* Alt-D: I_DeleteSample */
{
    sample_t *s = cur_smp();

    if (!confirm_box("Delete sample?"))
        return;
    stop_song();
    smp_free_data(s);
    ed_lock();
    Music_InitSample(s);                /* Music_ReleaseSample + name */
    ed_unlock();
}

static void smp_op_speed(int which)     /* Alt/Ctrl +/-: speed ops */
{
    sample_t *s = cur_smp();
    uint32_t c5 = s->C5Speed;

    switch (which) {
    case 0:                             /* Alt-+: double, cap 9999999 */
        if (c5 * 2 <= 9999999u && c5 <= 0x7FFFFFFFu / 2)
            s->C5Speed = c5 * 2;
        break;
    case 1:                             /* Alt--: halve */
        s->C5Speed = c5 >> 1;
        break;
    case 2: {                           /* Ctrl-+: semitone up */
        uint64_t add = ((uint64_t)c5 * 255392045u) >> 32;
        uint64_t v = (uint64_t)c5 + add;
        if (v <= 0xFFFFFFFFu)           /* JC keeps the old value */
            s->C5Speed = (uint32_t)v;
        break;
    }
    case 3:                             /* Ctrl--: semitone down */
        s->C5Speed = (uint32_t)(((uint64_t)c5 * 4053909306u) >> 32);
        break;
    }
}

static void smp_op_clear_name(void)     /* Alt-C: I_ClearSampleName */
{
    sample_t *s = cur_smp();
    ed_lock();
    memset(s->DOSFileName, 0, sizeof(s->DOSFileName));
    memset(s->SampleName, 0, sizeof(s->SampleName));
    ed_unlock();
}

static void smp_op_scale_volumes(void)  /* Alt-J: I_ScaleSampleVolumes */
{
    long amp = prompt_number("Scale all sample volumes by %", 100, 400);
    int i;

    if (amp <= 0)
        return;
    ed_lock();
    for (i = 0; i < 99; i++) {
        unsigned v = (unsigned)Song.Smp[i].GvL * (unsigned)amp / 100;
        Song.Smp[i].GvL = (uint8_t)(v >= 64 ? 64 : v);
    }
    ed_unlock();
}

/* pattern instrument-byte remap (PE_InsertInstrument /
 * PE_DeleteInstrument / PE_SwapInstruments) via the exact codec.
 * op: 0 = insert at n (bytes >= n incremented, cap 99),
 *     1 = delete n (bytes >= n decremented),
 *     2 = swap a <-> b,  3 = replace a -> b. */
static editcell_t OpGrid[MAX_PATROWS * 64];

static void pattern_remap_ins(int op, int a, int b)
{
    int p, i;

    commit_current_pattern();
    for (p = 0; p < MAX_PATTERNS; p++) {
        uint16_t rows;
        int changed = 0;
        if (!Song.Patterns[p].PackedData)
            continue;
        rows = Pattern_Unpack((uint16_t)p, OpGrid);
        for (i = 0; i < (int)rows * 64; i++) {
            editcell_t *e = &OpGrid[i];
            int v;
            if (!(e->mask & CM_INS))
                continue;
            v = e->ins;
            switch (op) {
            case 0: if (v >= a && v < 99) { e->ins = (uint8_t)(v + 1);
                                            changed = 1; } break;
            case 1: if (v >= a) { e->ins = (uint8_t)(v - 1);
                                  changed = 1; } break;
            case 2: if (v == a) { e->ins = (uint8_t)b; changed = 1; }
                    else if (v == b) { e->ins = (uint8_t)a; changed = 1; }
                    break;
            case 3: if (v == a) { e->ins = (uint8_t)b; changed = 1; }
                    break;
            }
        }
        if (changed)
            Pattern_Pack((uint16_t)p, OpGrid, rows);
    }
    load_pattern(CurPattern);
}

/* Alt-U on the F4 list (I_UpdateInstrument, IT_I.ASM 6575 ->
 * PE_UpdateInstruments, IT_PE.ASM 10840; feature 013): rewrite every
 * pattern cell whose (note, instrument) pair matches an entry of the
 * selected instrument's note-sample table into (table-index note,
 * selected instrument). All patterns; not undoable (original). */
static void pattern_update_instruments(void)
{
    int ins = ListSel + 1;              /* PE_GetLastInstrument */
    const uint8_t *tab = Song.Ins[ins - 1].NoteSampleTable;
    int p, i, dx;

    commit_current_pattern();
    for (p = 0; p < MAX_PATTERNS; p++) {
        uint16_t rows;
        int changed = 0;
        if (!Song.Patterns[p].PackedData)
            continue;
        rows = Pattern_Unpack((uint16_t)p, OpGrid);
        for (i = 0; i < (int)rows * 64; i++) {
            editcell_t *e = &OpGrid[i];
            if (!(e->mask & CM_NOTE) || e->note < 1 || e->note > 120)
                continue;               /* real notes only */
            if (!(e->mask & CM_INS) || e->ins == 0)
                continue;
            for (dx = 0; dx < 120; dx++)
                if (tab[dx * 2] == e->note - 1 &&
                    tab[dx * 2 + 1] == e->ins)
                    break;
            if (dx < 120) {
                e->note = (uint8_t)(dx + 1);
                e->ins = (uint8_t)ins;
                changed = 1;
            }
        }
        if (changed)
            Pattern_Pack((uint16_t)p, OpGrid, rows);
    }
    load_pattern(CurPattern);
}

/* NoteSampleTable remap over all instruments (sample slot ops in
 * instrument mode) */
static void nst_remap(int op, int a, int b)
{
    int i, n;

    ed_lock();
    for (i = 0; i < MAX_INSTRUMENTS; i++) {
        uint8_t *t = Song.Ins[i].NoteSampleTable;
        for (n = 0; n < 120; n++) {
            int v = t[n * 2 + 1];
            switch (op) {
            case 0: if (v >= a && v < 99) t[n * 2 + 1] = (uint8_t)(v + 1);
                    break;
            case 1: if (v >= a) t[n * 2 + 1] = (uint8_t)(v - 1); break;
            case 2: if (v == a) t[n * 2 + 1] = (uint8_t)b;
                    else if (v == b) t[n * 2 + 1] = (uint8_t)a;
                    break;
            case 3: if (v == a) t[n * 2 + 1] = (uint8_t)b; break;
            }
        }
    }
    ed_unlock();
}

static void smp_op_insert_slot(void)    /* Alt-Ins: I_InsertSample */
{
    int cur = ListSel, i;

    /* the last slot must be free -- blank means the SampleHeader
     * template (or all-zero), not all-zero alone, since empty slots are
     * template-stamped (278cb25; fix from PR #3 by esaruoho) */
    if (!Music_SampleIsBlank(&Song.Smp[98]) || cur >= 98)
        return;
    stop_song();
    ed_lock();
    for (i = 98; i > cur; i--)
        Song.Smp[i] = Song.Smp[i - 1];
    Music_InitSample(&Song.Smp[cur]);
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        nst_remap(0, cur + 1, 0);
    else
        pattern_remap_ins(0, cur + 1, 0);
}

static void smp_op_remove_slot(void)    /* Alt-Del: I_RemoveSample */
{
    int cur = ListSel, i;
    sample_t *s = cur_smp();

    if (s->Flags & 1)                   /* only when slot has no data */
        return;
    stop_song();
    ed_lock();
    for (i = cur; i < 98; i++)
        Song.Smp[i] = Song.Smp[i + 1];
    Music_InitSample(&Song.Smp[98]);
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        nst_remap(1, cur + 1, 0);
    else
        pattern_remap_ins(1, cur + 1, 0);
}

static void smp_op_swap(void)           /* Alt-S: I_SwapSamples */
{
    long n = prompt_number("Swap current sample with", 0, 99);
    sample_t t;

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    stop_song();
    ed_lock();
    t = Song.Smp[n - 1];
    Song.Smp[n - 1] = Song.Smp[ListSel];
    Song.Smp[ListSel] = t;
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        nst_remap(2, ListSel + 1, (int)n);
    else
        pattern_remap_ins(2, ListSel + 1, (int)n);
}

static void smp_op_exchange(void)       /* Alt-X: I_ExchangeSamples */
{
    long n = prompt_number("Exchange current sample with", 0, 99);
    sample_t t;

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    stop_song();
    ed_lock();
    t = Song.Smp[n - 1];
    Song.Smp[n - 1] = Song.Smp[ListSel];
    Song.Smp[ListSel] = t;
    ed_unlock();
}

static void smp_op_replace(void)        /* Alt-R: I_ReplaceSample */
{
    long n = prompt_number("Replace all uses of current with", 0, 99);

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        nst_remap(3, ListSel + 1, (int)n);
    else
        pattern_remap_ins(3, ListSel + 1, (int)n);
}

/* ---- F3 editable loop/speed fields (O1_SampleList objects 30..36).
 * Numbers edit through the numeric prompt (Enter on the field) --
 * a documented deviation from the original's inline digit entry. --- */

typedef struct smpfield_t {
    const char *title;
    int y;                              /* row in the InstParamBox */
    int kind;                           /* 0 num, 1 loop tri, 2 sus tri */
    int which;                          /* num: 0 C5,1 LB,2 LE,3 SB,4 SE */
} smpfield_t;

static const smpfield_t SmpFields[] = {
    { "C5 speed",           14, 0, 0 },
    { "Loop",               15, 1, 0 },
    { "Loop beginning",     16, 0, 1 },
    { "Loop end",           17, 0, 2 },
    { "Sustain loop",       18, 2, 0 },
    { "SusLoop beginning",  19, 0, 3 },
    { "SusLoop end",        20, 0, 4 },
};
static int SmpFieldSel;                 /* focused field for cdraw */

static uint32_t *smp_field_ptr(int which)
{
    sample_t *s = cur_smp();
    switch (which) {
    case 0:  return &s->C5Speed;
    case 1:  return &s->LoopBeg;
    case 2:  return &s->LoopEnd;
    case 3:  return &s->SusLoopBeg;
    default: return &s->SusLoopEnd;
    }
}

static void smp_field_commit(int idx)
{
    const smpfield_t *f = &SmpFields[idx];
    sample_t *s = cur_smp();

    if (f->kind == 0) {
        long v = prompt_number(f->title, *smp_field_ptr(f->which),
                               9999999);
        if (v >= 0) {
            ed_lock();
            *smp_field_ptr(f->which) = (uint32_t)v;
            ed_unlock();
        }
    } else if (f->kind == 1) {          /* Off -> On -> Ping Pong */
        ed_lock();
        if (!(s->Flags & 0x10))
            s->Flags |= 0x10, s->Flags &= (uint8_t)~0x40;
        else if (!(s->Flags & 0x40))
            s->Flags |= 0x40;
        else
            s->Flags &= (uint8_t)~(0x10 | 0x40);
        ed_unlock();
    } else {
        ed_lock();
        if (!(s->Flags & 0x20))
            s->Flags |= 0x20, s->Flags &= (uint8_t)~0x80;
        else if (!(s->Flags & 0x80))
            s->Flags |= 0x80;
        else
            s->Flags &= (uint8_t)~(0x20 | 0x80);
        ed_unlock();
    }
    smp_check_both();
}

static void smpfield_cdraw(int focused)
{
    const sample_t *s = cur_smp();
    int i;

    for (i = 0; i < (int)(sizeof(SmpFields) / sizeof(SmpFields[0])); i++) {
        const smpfield_t *f = &SmpFields[i];
        uint8_t a = (focused && i == SmpFieldSel) ? 0x30 : 0x02;
        switch (f->kind) {
        case 0:
            /* F_Draw5Num -> F_ConvEAX2Num (IT_F.ASM 4311): always seven
             * digits, zero-padded, colour 2 -- "0008363" */
            drawf(64, f->y, a, "%07u",
                  (unsigned)(*((const uint32_t *)smp_field_ptr(f->which))
                             % 10000000u));
            break;
        case 1:
        case 2: {
            /* F_DrawToggle writes "On"/"Off"; GetSampleToggle (IT_F.ASM
             * 3230) then adds "Forwards" or "Ping Pong" three cells to
             * the right when the loop is on, colour 2 */
            uint8_t on = f->kind == 1 ? 0x10 : 0x20;
            uint8_t pp = f->kind == 1 ? 0x40 : 0x80;
            /* no padding: the box colour stays after the words (#35) */
            if (!(s->Flags & on)) {
                drawf(64, f->y, a, "Off");
            } else {
                drawf(64, f->y, a, "On");
                drawf(67, f->y, 0x02, "%s",
                      (s->Flags & pp) ? "Ping Pong" : "Forwards");
            }
            break;
        }
        }
    }
}

static int smpfield_ckey(int key)
{
    int nf = (int)(sizeof(SmpFields) / sizeof(SmpFields[0]));

    switch (key) {
    case ITK_UP:
        if (SmpFieldSel > 0) { SmpFieldSel--; return 1; }
        return 0;
    case ITK_DOWN:
        if (SmpFieldSel < nf - 1) { SmpFieldSel++; return 1; }
        return 0;
    case ITK_ENTER: case ' ':
        smp_field_commit(SmpFieldSel);
        return 1;
    default:
        return 0;
    }
}

static void smpfield_cclick(const it_mouse_t *m)
{
    int i;
    for (i = 0; i < (int)(sizeof(SmpFields) / sizeof(SmpFields[0])); i++)
        if (m->y == SmpFields[i].y) {
            SmpFieldSel = i;
            smp_field_commit(i);
            return;
        }
}

static instrument_t *cur_ins(void);

/* D_SaveSample / D_SaveST3Sample / D_SaveRawSample: Alt-O/T/W write
 * the current sample under its DOS filename (error when empty). */
static void smp_save_disk(int fmt)
{
    sample_t *s = cur_smp();
    char name[16];
    int ok;

    if (!s->DOSFileName[0]) {
        status("Error: Sample %d NOT saved! (No Filename?)", ListSel + 1);
        return;
    }
    snprintf(name, sizeof(name), "%.12s", s->DOSFileName);
    switch (fmt) {
    default: ok = RIS_SaveITS(s, name); break;
    case 1:  ok = RIS_SaveST(s, name);  break;
    case 2:  ok = RIS_SaveWAV(s, name); break;
    }
    if (!ok) {
        status("Error: Sample %d NOT saved! (No Filename?)", ListSel + 1);
        return;
    }
    status(fmt == 0 ? "Impulse Tracker sample saved (sample %d)"
         : fmt == 1 ? "Scream Tracker sample saved (sample %d)"
                    : "WAV Sample saved (sample %d)", ListSel + 1);
}

/* D_SaveInstrument: F4 Alt-O writes the current instrument as .ITI */
static void ins_save_disk(void)
{
    instrument_t *in = cur_ins();
    char name[16];

    if (!in->DOSFileName[0]) {
        status("Error: Instrument %d NOT saved! (No Filename?)",
               ListSel + 1);
        return;
    }
    snprintf(name, sizeof(name), "%.12s", in->DOSFileName);
    if (RI_SaveITI(in, ListSel + 1, name))
        status("Instrument saved (instrument %d)", ListSel + 1);
    else
        status("Error: Instrument %d NOT saved! (No Filename?)",
               ListSel + 1);
}

/* Alt-key dispatch for the F3 sample list; returns 1 when consumed */
static void list_toggle_multichannel(void);   /* Alt-N (F3/F4) */

static int handle_sample_altkey(int key)
{
    switch (key) {
    case ITK_ALT_A + ('A'-'A'): smp_op_convert();      break;
    case ITK_ALT_A + ('B'-'A'): smp_op_cut_before();   break;
    case ITK_ALT_A + ('C'-'A'): smp_op_clear_name();   break;
    case ITK_ALT_A + ('D'-'A'): smp_op_delete();       break;
    case ITK_ALT_A + ('E'-'A'): smp_op_resize(1);      break;
    case ITK_ALT_A + ('F'-'A'): smp_op_resize(0);      break;
    case ITK_ALT_A + ('G'-'A'): smp_op_reverse();      break;
    case ITK_ALT_A + ('H'-'A'): smp_op_centre();       break;
    case ITK_ALT_A + ('I'-'A'): smp_op_invert();       break;
    case ITK_ALT_A + ('J'-'A'): smp_op_scale_volumes();break;
    case ITK_ALT_A + ('L'-'A'): smp_op_cut_after();    break;
    case ITK_ALT_A + ('M'-'A'): smp_op_amplify();      break;
    case ITK_ALT_A + ('Q'-'A'): smp_op_quality();      break;
    case ITK_ALT_A + ('R'-'A'): smp_op_replace();      break;
    case ITK_ALT_A + ('S'-'A'): smp_op_swap();         break;
    case ITK_ALT_A + ('X'-'A'): smp_op_exchange();     break;
    case ITK_ALT_A + ('Y'-'A'): /* I_CalculateC5Speed: 2.17 stub */
        break;
    case ITK_ALT_A + ('N'-'A'): list_toggle_multichannel(); break;
    case ITK_ALT_A + ('O'-'A'): smp_save_disk(0);      break;
    case ITK_ALT_A + ('T'-'A'): smp_save_disk(1);      break;
    case ITK_ALT_A + ('W'-'A'): smp_save_disk(2);      break;
    case ITK_ALT_PLUS:  smp_op_speed(0); break;
    case ITK_ALT_MINUS: smp_op_speed(1); break;
    case ITK_CTRL_PLUS: smp_op_speed(2); break;
    case ITK_CTRL_MINUS:smp_op_speed(3); break;
    case ITK_ALT_INS:   smp_op_insert_slot(); break;
    case ITK_ALT_DEL:   smp_op_remove_slot(); break;
    default:
        return 0;
    }
    return 1;
}

/* I_ShowSamplePlay / I_ShowInstrumentPlay (IT_I.ASM 8443): refresh the
 * live bits of a play table from the slave channels (1 = playing and not
 * muted, 2 = just started), then draw a column of dots at x=1 for the
 * 35 list rows: none, a dark small dot (173, attr 21h) for "has been
 * played", a bright small dot (23h) while it plays, a large bright dot
 * (183) when it has just been triggered. `first` is the table index of
 * the top row. */
static void draw_play_dots(uint8_t *table, int first, int instrument)
{
    int i;

    for (i = 0; i < 100; i++)
        table[i] &= (uint8_t)~3;
    for (i = 0; i < MAXSLAVECHANNELS; i++) {
        const slavechn_t *sc = &SChn[i];
        int k = instrument ? sc->Ins : sc->Smp;
        if (!(sc->Flags & SF_CHAN_ON) || (sc->Flags & SF_CHN_MUTED))
            continue;
        if (k >= 100)
            continue;
        table[k] |= 1;
        if (sc->OldSampleOffset == 0)
            table[k] |= 2;
    }
    for (i = 0; i < 35; i++) {
        int k = first + i;
        uint8_t d = k >= 0 && k < 128 ? table[k] : 0;
        uint8_t ch = 0, a = 0x21;
        if (d != 0) {
            ch = 173;
            if (d != 4) {
                a = 0x23;
                if (d & 2)
                    ch = 183;
            }
        }
        Screen_PutChar(1, 13 + i, ch, a);
    }
}

static void draw_samples(void)
{
    int i, n = 99;              /* IT lists all 99 slots */
    int rows = 35, top;
    sample_t *s;

    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;
    top = SmpListTop;                   /* TopSample: scroll only when */
    if (ListSel < top) top = ListSel;   /* the selection leaves view   */
    if (ListSel > top + rows - 1) top = ListSel - (rows - 1);
    if (top > n - rows) top = n - rows;
    if (top < 0) top = 0;
    SmpListTop = top;

    Screen_DrawBox(4, 12, 35, 48, 27);          /* SampleListBox */
    /* I_DrawSampleList (IT_I.ASM 998): number at (2,y) attr 20h, the
     * 25-char name at (5,y) attr 06h, divider 168 at x=30 attr 02h,
     * "Play" at x=31 (attr 06h with a sample, 07h without); the current
     * row's 30 cells take bg E. Focused (I_PreSampleList): the name
     * cursor cell, or with the cursor on the Play column (SamplePos 25)
     * its four cells, turn 30h (60h for an empty slot). */
    for (i = 0; i < rows; i++) {
        int idx = top + i, x;
        uint8_t pa;
        if (idx < 0 || idx >= n)
            continue;
        drawf(2, 13 + i, 0x20, "%02d", (idx + 1) % 100);
        draw_itname(5, 13 + i, Song.Smp[idx].SampleName, 25, 0x06);
        Screen_PutChar(30, 13 + i, 168, 0x02);
        pa = (Song.Smp[idx].Flags & 1) ? 0x06 : 0x07;
        Screen_DrawString(31, 13 + i, "Play", pa);
        if (idx != ListSel)
            continue;
        for (x = 5; x < 35; x++)
            Screen_SetAttr(x, 13 + i,
                           (uint8_t)((Screen_GetAttr(x, 13 + i) & 0x0F) | 0xE0));
        if (FocusIdx[SCR_SAMPLES] == 0) {       /* the list widget */
            if (SamplePos < 25)
                Screen_SetAttr(5 + SamplePos, 13 + i, 0x30);
            else
                for (x = 31; x < 35; x++)
                    Screen_SetAttr(x, 13 + i, pa == 0x06 ? 0x30 : 0x60);
        }
    }
    draw_play_dots(SamplePlayTable, top, 0);

    s = &Song.Smp[ListSel];

    NW = 0;
    wlist(5, 13, 34, 47, 13, sample_list_lkey, sample_list_lclick);

    Screen_DrawBox(36, 12, 53, 18, 9);          /* Default Volume */
    Screen_DrawString(38, 14, "Default Volume", 0x20);
    Screen_DrawBox(37, 15, 47, 17, 9);
    wthumb(38, 16, 0, 64, &s->Vol, 0, 0);

    Screen_DrawBox(36, 19, 53, 25, 9);          /* Global Volume */
    Screen_DrawString(38, 21, "Global Volume", 0x20);
    Screen_DrawBox(37, 22, 47, 24, 9);
    wthumb(38, 23, 0, 64, &s->GvL, 0, 0);

    Screen_DrawBox(36, 26, 53, 33, 9);          /* Default Pan */
    Screen_DrawString(39, 28, "Default Pan", 0x20);
    Screen_DrawBox(37, 29, 47, 32, 25);
    wtoggle8(38, 30, &s->DfP, 0x80);
    wthumb(38, 31, 0, 64, &s->DfP, 0x7F, 0);

    Screen_DrawBox(36, 35, 53, 41, 9);          /* Vibrato Speed */
    Screen_DrawString(38, 37, "Vibrato Speed", 0x20);
    Screen_DrawBox(37, 38, 47, 40, 9);
    wthumb(38, 39, 0, 64, &s->ViS, 0, 0);

    Screen_DrawBox(36, 42, 53, 48, 9);          /* Vibrato Depth */
    Screen_DrawString(38, 44, "Vibrato Depth", 0x20);
    Screen_DrawBox(37, 45, 47, 47, 9);
    wthumb(38, 46, 0, 32, &s->ViD, 0, 8);

    Screen_DrawBox(54, 42, 77, 48, 9);          /* Vibrato Rate */
    Screen_DrawString(60, 44, "Vibrato Rate", 0x20);
    Screen_DrawBox(55, 45, 72, 47, 9);
    wthumb(56, 46, 0, 255, &s->ViR, 0, 15);

    Screen_DrawBox(54, 25, 77, 30, 9);          /* waveform display */
    draw_waveform();                            /* I_DrawWaveForm   */
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 22; j++)                /* InstWaveFormText */
            Screen_PutChar(55 + j, 26 + i,
                           (uint8_t)(1 + i * 22 + j), 0x0D);
    }
    Screen_DrawBox(54, 31, 77, 41, 9);          /* Vibrato Waveform */
    Screen_DrawString(58, 33, "Vibrato Waveform", 0x20);
    wradio8(56, 35, 65, 37, "   \271\272", &s->ViT, 3, 0);    /* sine   */
    wradio8(66, 35, 75, 37, "   \275\276", &s->ViT, 3, 1);    /* ramp   */
    wradio8(56, 38, 65, 40, "   \273\274", &s->ViT, 3, 2);    /* square */
    wradio8(66, 38, 75, 40, " Random",     &s->ViT, 3, 3);

    Screen_DrawBox(63, 12, 77, 24, 27);         /* InstParamBox */
    Screen_DrawStringCtl(55, 13, InstParamText, 0x20, NULL);
    wtext(64, 13, s->DOSFileName, 12);          /* InstFileName     */
    wcustom(64, 14, 77, 20, smpfield_cdraw, smpfield_ckey,
            smpfield_cclick);                   /* speed/loop fields */
    /* I_ShowSampleInfo (IT_I.ASM 1686): colour 2; no sample flag ->
     * "No sample", else the bit depth */
    Screen_DrawString(64, 22, !(s->Flags & 1) ? "No sample"
                             : (s->Flags & 2) ? "16 bits" : "8 bits", 0x02);
    drawf(64, 23, 0x02, "%u", s->Length);

    widgets_draw();
}

/* ===================================================================
 * Instrument list (F4) -- object-exact port of the four object lists
 * O1_InstrumentListGeneral/Volume/Panning/Pitch (IT_OBJ1.ASM 5629..)
 * with the custom-draw objects from IT_I.ASM: I_DrawInstrumentWindow,
 * I_DrawNoteWindow, I_DrawEnvelope (+node editing), and
 * I_DrawPitchPanCenter. FILTERENVELOPES=1 layout (the 2.17 build).
 * =================================================================== */
static int key_to_note(const it_key_t *k);
static int key_to_note_plain(void);
static void jam_note(int gnote, int chan);

static uint8_t InsTab = 0;         /* InstrumentScreen: 0=General 1=Vol
                                    * 2=Pan 3=Pitch                     */
static int CurrentNode = 0;        /* envelope node cursor              */
static int NodeHeld = 0;           /* Enter "grabs" the node            */
static int NoteWinTop = 0;         /* note-translation window scroll    */
static int NoteWinSel = 0;         /* CurrentNote 0..119                */
static int NotePos = 0;            /* cursor column 0..3                */
static int IdxTabBtn, IdxEnvOn, IdxNNACut, IdxLeftList; /* focus links  */

#define ENVELOPEGRANULARITY 50
#define MAXENVELOPETICK     9999

static uint16_t EnvUpperLimit = ENVELOPEGRANULARITY;

static instrument_t *cur_ins(void)
{
    return &Song.Ins[ListSel];
}

static env_t *cur_env(void)
{
    instrument_t *ins = cur_ins();
    switch (InsTab) {
    case 2:  return &ins->PEnvelope;
    case 3:  return &ins->PtEnvelope;
    default: return &ins->VEnvelope;
    }
}

/* AmplitudeCompensate + axis row, per I_MapEnvelope: filter envelopes
 * and the volume envelope have the axis at the bottom (row 62); pan and
 * pitch centre it (row 31). Compensate shifts signed values to 0..64. */
static int env_compensate(const env_t *e)
{
    if (e->Flags & 0x80) return 32;
    return (InsTab == 1) ? 0 : 32;
}

static int env_axis_row(const env_t *e)
{
    if (e->Flags & 0x80) return 62;
    return (InsTab == 1) ? 62 : 31;
}

/* ---- instrument list Alt ops (InstrumentGlobalKeyList) ------------- */

/* Alt-D: I_DeleteInstrument (IT_I.ASM 2335). Deletes the instrument
 * AND every sample its note table plays: for each of the 120 entries
 * the sample byte (+41h) is released (Music_ReleaseSample) and its
 * header cleared (Music_ClearSampleName), then I_InstrumentClear stamps
 * the template. Samples shared with other instruments go too -- that
 * is the original's behaviour. */
static void ins_op_delete(void)
{
    instrument_t *in = cur_ins();
    int n;

    if (!confirm_box("Delete instrument?"))     /* default Cancel (CX=4) */
        return;
    stop_song();
    for (n = 0; n < 120; n++) {
        int smp = in->NoteSampleTable[n * 2 + 1];
        sample_t *s;
        if (smp < 1 || smp > 99)
            continue;
        s = &Song.Smp[smp - 1];
        smp_free_data(s);
        ed_lock();
        Music_InitSample(s);            /* Music_ClearSampleName */
        ed_unlock();
    }
    ed_lock();
    Music_InitInstrument(in);
    ed_unlock();
}

/* Alt-W: I_InstrumentClear (IT_I.ASM 5357) -- reset the instrument to
 * the InstrumentHeader template, samples untouched. */
static void ins_op_wipe(void)
{
    ed_lock();
    Music_InitInstrument(cur_ins());
    ed_unlock();
}

static void ins_op_swap(void)           /* Alt-S: I_SwapInstruments */
{
    long n = prompt_number("Swap current instrument with", 0, 99);
    instrument_t t;

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    stop_song();
    ed_lock();
    t = Song.Ins[n - 1];
    Song.Ins[n - 1] = Song.Ins[ListSel];
    Song.Ins[ListSel] = t;
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        pattern_remap_ins(2, ListSel + 1, (int)n);
}

static void ins_op_exchange(void)       /* Alt-X: I_ExchangeInstruments */
{
    long n = prompt_number("Exchange current instrument with", 0, 99);
    instrument_t t;

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    stop_song();
    ed_lock();
    t = Song.Ins[n - 1];
    Song.Ins[n - 1] = Song.Ins[ListSel];
    Song.Ins[ListSel] = t;
    ed_unlock();
}

static void ins_op_replace(void)        /* Alt-R: I_ReplaceInstrument */
{
    long n = prompt_number("Replace all uses of current with", 0, 99);

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        pattern_remap_ins(3, ListSel + 1, (int)n);
}

static void ins_op_copy(void)           /* Alt-P: I_CopyInstrument */
{
    long n = prompt_number("Copy instrument from", 0, 99);

    if (n <= 0 || (int)n - 1 == ListSel)
        return;
    ed_lock();
    Song.Ins[ListSel] = Song.Ins[n - 1];
    ed_unlock();
}

static void ins_op_scale_volumes(void)  /* Alt-J */
{
    long amp = prompt_number("Scale all instrument volumes by %",
                             100, 400);
    int i;

    if (amp <= 0)
        return;
    ed_lock();
    for (i = 0; i < 99; i++) {
        unsigned v = (unsigned)Song.Ins[i].GbV * (unsigned)amp / 100;
        Song.Ins[i].GbV = (uint8_t)(v >= 128 ? 128 : v);
    }
    ed_unlock();
}

static void ins_op_clear_name(void)     /* Alt-C: I_InstrumentNameClear */
{
    instrument_t *ins = cur_ins();
    ed_lock();
    memset(ins->DOSFileName, 0, sizeof(ins->DOSFileName));
    memset(ins->InstrumentName, 0, sizeof(ins->InstrumentName));
    ed_unlock();
}

/* Alt-Ins: I_InsertInstrument. No-op on instrument 99; otherwise the
 * slots from here on move down one (instrument 99 falls off the end --
 * the original does not guard it), patterns are remapped in instrument
 * mode (PE_InsertInstrument), and the freed slot gets the template. */
static void ins_op_insert_slot(void)
{
    int cur = ListSel, i;

    if (cur >= 98)
        return;
    stop_song();
    ed_lock();
    for (i = 98; i > cur; i--)
        Song.Ins[i] = Song.Ins[i - 1];
    Music_InitInstrument(&Song.Ins[cur]);
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        pattern_remap_ins(0, cur + 1, 0);
}

/* Alt-Del: I_RemoveInstrument. The following slots move up one,
 * patterns are remapped in instrument mode (PE_DeleteInstrument), and
 * instrument 99 is reset with Music_ClearInstrument. */
static void ins_op_remove_slot(void)
{
    int cur = ListSel, i;

    stop_song();
    ed_lock();
    for (i = cur; i < 98; i++)
        Song.Ins[i] = Song.Ins[i + 1];
    Music_InitInstrument(&Song.Ins[98]);
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        pattern_remap_ins(1, cur + 1, 0);
}

/* returns 1 when consumed (F4 instrument list screen) */
static int handle_instrument_altkey(int key)
{
    switch (key) {
    case ITK_ALT_A + ('C'-'A'): ins_op_clear_name();    break;
    case ITK_ALT_A + ('D'-'A'): ins_op_delete();        break;
    case ITK_ALT_A + ('W'-'A'): ins_op_wipe();          break;
    case ITK_ALT_INS:           ins_op_insert_slot();   break;
    case ITK_ALT_DEL:           ins_op_remove_slot();   break;
    case ITK_ALT_A + ('J'-'A'): ins_op_scale_volumes(); break;
    case ITK_ALT_A + ('P'-'A'): ins_op_copy();          break;
    case ITK_ALT_A + ('R'-'A'): ins_op_replace();       break;
    case ITK_ALT_A + ('S'-'A'): ins_op_swap();          break;
    case ITK_ALT_A + ('X'-'A'): ins_op_exchange();      break;
    case ITK_ALT_A + ('U'-'A'):         /* I_UpdateInstrument */
        pattern_update_instruments();
        break;
    case ITK_ALT_A + ('O'-'A'): ins_save_disk();        break;
    case ITK_ALT_A + ('N'-'A'): list_toggle_multichannel(); break;
    default:
        return 0;
    }
    return 1;
}

/* SetInstrument3Num tail: after a loop-node edit, push each pair's end
 * up to its begin across all three envelopes. */
static void env_fix_loop_pairs(void)
{
    instrument_t *ins = cur_ins();
    env_t *es[3];
    int i;

    es[0] = &ins->VEnvelope; es[1] = &ins->PEnvelope;
    es[2] = &ins->PtEnvelope;
    ed_lock();
    for (i = 0; i < 3; i++) {
        if (es[i]->LpB > es[i]->LpE) es[i]->LpE = es[i]->LpB;
        if (es[i]->SLB > es[i]->SLE) es[i]->SLE = es[i]->SLB;
    }
    ed_unlock();
}

/* I_MapEnvelope: render the active envelope into the 256x64 pixel
 * generation table and regenerate font bank B (chars 0..255). */
static void env_map(void)
{
    static uint8_t pix[256 * 64];
    env_t *e = cur_env();
    int comp = env_compensate(e);
    int num, r, x, i;
    int lastamp = 0, lastx = 0;

    memset(pix, 0, sizeof(pix));

    for (r = 0; r < 64; r += 2)            /* dotted Y axis, column 3 */
        pix[r * 256 + 3] = 1;
    r = env_axis_row(e);                   /* dotted X axis */
    for (x = 1; x < 256; x += 2)
        pix[r * 256 + x] = 1;

    num = e->Num;
    if (CurrentNode >= num)
        CurrentNode = num ? num - 1 : 0;

    if (num) {
        int lasttick = e->NodePoints[num - 1].Tick;
        EnvUpperLimit = (uint16_t)((lasttick / ENVELOPEGRANULARITY + 1)
                                   * ENVELOPEGRANULARITY);

        for (i = 0; i < num; i++) {
            /* amplitude in 8.8 (row = high byte), tick -> 0..249 */
            uint8_t a8 = (uint8_t)(64 - comp - e->NodePoints[i].Magnitude);
            int amp = a8 * 244;
            int tick = e->NodePoints[i].Tick;
            int row, px, py, dr;

            if (tick >= EnvUpperLimit)
                break;                     /* I_MapEnvelopeError */
            x = tick * 250 / EnvUpperLimit;
            row = amp >> 8;
            px = x + 3; py = row + 1;

            for (dr = -1; dr <= 1; dr++) { /* 3x3 node marker */
                pix[(py + dr) * 256 + px - 1] = 1;
                pix[(py + dr) * 256 + px]     = 1;
                pix[(py + dr) * 256 + px + 1] = 1;
                if (i == CurrentNode) {    /* wide marks at +/-3 */
                    pix[(py + dr) * 256 + px + 2] = 0;
                    pix[(py + dr) * 256 + px + 3] = 1;
                    pix[(py + dr) * 256 + px - 3] = 1;
                    pix[(py + dr) * 256 + px - 2] = 0;
                }
            }

            if ((e->Flags & 2) && (i == e->LpB || i == e->LpE)) {
                for (r = 0; r < 64; r++)   /* dashed loop marker */
                    pix[r * 256 + px] = (uint8_t)(((r + 1) >> 1) & 1);
            } else if ((e->Flags & 4) && (i == e->SLB || i == e->SLE)) {
                for (r = 0; r < 64; r++)   /* dotted susloop marker */
                    pix[r * 256 + px] = (uint8_t)((r & 1) ^ 1);
            }

            if (i > 0) {                   /* line segment from last node */
                int dcols = x - lastx;
                if (dcols == 0) {          /* vertical */
                    int r0 = lastamp >> 8, r1 = amp >> 8;
                    int d = (r1 >= r0) ? 1 : -1, n = (r1 - r0) * d, rr = r0;
                    while (n--) {
                        pix[(rr + 1) * 256 + lastx + 3] = 1;
                        rr += d;
                    }
                } else if (dcols > 0) {    /* diagonal */
                    int damp = amp - lastamp;
                    int step = (damp >= 0) ? -1 : 1;
                    int sd = (damp >= 0) ? damp / dcols
                                         : -((-damp) / dcols);
                    int acc = lastamp, col = lastx + 3, c;
                    for (c = 0; c < dcols; c++) {
                        int prev = acc >> 8, run, rr, k;
                        acc += sd;
                        run = prev - (acc >> 8);
                        if (run < 0) run = -run;
                        if (run == 0) run = 1;
                        rr = acc >> 8;
                        for (k = 0; k < run; k++) {
                            if (rr >= 0 && rr < 63)
                                pix[(rr + 1) * 256 + col] = 1;
                            rr += step;
                        }
                        col++;
                    }
                }
            }
            lastamp = amp; lastx = x;
        }

        /* live playback cursors: a full-height line per slave channel
         * playing this instrument with this envelope running */
        ed_lock();
        for (i = 0; i < MAXSLAVECHANNELS; i++) {
            slavechn_t *sc = &SChn[i];
            uint16_t envbit = (uint16_t)(SF_VOLENV_ON << (InsTab - 1));
            uint16_t pos;

            if (!(sc->Flags & SF_CHAN_ON) || sc->Ins != ListSel)
                continue;
            pos = (InsTab == 1) ? sc->VEnv.Pos :
                  (InsTab == 2) ? sc->PEnv.Pos : sc->PtEnv.Pos;
            if (pos == 0 && !(sc->Flags & envbit))
                continue;
            if (pos >= EnvUpperLimit)
                continue;
            x = pos * 250 / EnvUpperLimit + 3;
            for (r = 0; r < 64; r++)
                pix[r * 256 + x] = 1;
        }
        ed_unlock();
    }

    Screen_GenerateCharacters(0, 32, 8, pix);
}

/* I_EnvelopeSelected: grabbing a node turns the envelope on (and on the
 * pitch tab defaults a fresh envelope to filter mode, FILTERENVELOPES) */
static void env_selected(env_t *e)
{
    ed_lock();
    if (InsTab == 3 && !(e->Flags & 1))
        e->Flags |= 0x80;
    e->Flags |= 1;
    ed_unlock();
}

static void env_insert_node(void)          /* I_VolumeEnvelopeInsert */
{
    env_t *e = cur_env();
    int cur = CurrentNode, num = e->Num, k;

    if (cur + 1 == num || num >= 25 || num == 0)
        return;
    if (e->NodePoints[cur + 1].Tick - e->NodePoints[cur].Tick < 2)
        return;

    ed_lock();
    e->Num++;
    for (k = num; k > cur + 1; k--)
        e->NodePoints[k] = e->NodePoints[k - 1];
    e->NodePoints[cur + 1].Tick =
        (uint16_t)((e->NodePoints[cur].Tick +
                    e->NodePoints[cur + 2].Tick) >> 1);
    {
        int comp = env_compensate(e);
        int mid = ((e->NodePoints[cur].Magnitude + comp) +
                   (e->NodePoints[cur + 2].Magnitude + comp)) / 2 - comp;
        e->NodePoints[cur + 1].Magnitude = (int8_t)mid;
    }
    if (cur < e->LpB) e->LpB++;
    if (cur < e->LpE) e->LpE++;
    if (cur < e->SLB) e->SLB++;
    if (cur < e->SLE) e->SLE++;
    ed_unlock();
}

static void env_delete_node(void)          /* I_VolumeEnvelopeDelete */
{
    env_t *e = cur_env();
    int cur = CurrentNode, num = e->Num, k, last;

    if (cur == 0 || num <= 2)
        return;

    ed_lock();
    e->Num--;
    for (k = cur; k < num - 1; k++)
        e->NodePoints[k] = e->NodePoints[k + 1];
    e->NodePoints[num - 1].Tick = 0;
    e->NodePoints[num - 1].Magnitude = 0;
    last = e->Num - 1;
    if (cur < e->LpB) e->LpB--;
    if (cur < e->LpE) e->LpE--;
    if (cur < e->SLB) e->SLB--;
    if (cur < e->SLE) e->SLE--;
    if (last < e->LpB) e->LpB = (uint8_t)last;
    if (last < e->LpE) e->LpE = (uint8_t)last;
    if (last < e->SLB) e->SLB = (uint8_t)last;
    if (last < e->SLE) e->SLE = (uint8_t)last;
    ed_unlock();
}

/* node moves (I_VolumeEnvelopeHeld*): value clamped so value+comp stays
 * in 0..64; ticks stay strictly between the neighbours, node 0 fixed */
static void env_node_value(int delta)
{
    env_t *e = cur_env();
    int comp = env_compensate(e);
    int v;

    if (!e->Num)
        return;
    v = e->NodePoints[CurrentNode].Magnitude + comp + delta;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    ed_lock();
    e->NodePoints[CurrentNode].Magnitude = (int8_t)(v - comp);
    ed_unlock();
}

static void env_node_tick(int delta, int home_end)
{
    env_t *e = cur_env();
    int cur = CurrentNode, t;

    if (cur == 0 || !e->Num)
        return;
    t = e->NodePoints[cur].Tick;
    if (home_end < 0)                      /* Home: prev+1 */
        t = e->NodePoints[cur - 1].Tick + 1;
    else if (home_end > 0)                 /* End: next-1 / max */
        t = (cur + 1 == e->Num) ? MAXENVELOPETICK
                                : e->NodePoints[cur + 1].Tick - 1;
    else
        t += delta;
    if (t < e->NodePoints[cur - 1].Tick + 1)
        t = e->NodePoints[cur - 1].Tick + 1;
    if (cur + 1 < e->Num && t > e->NodePoints[cur + 1].Tick - 1)
        t = e->NodePoints[cur + 1].Tick - 1;
    if (t > MAXENVELOPETICK)
        t = MAXENVELOPETICK;
    ed_lock();
    e->NodePoints[cur].Tick = (uint16_t)t;
    ed_unlock();
}

/* PresetEnvelopes (IT_I.ASM 367): ten stored envelopes, all defaulting
 * to the flat 2-node 32-amplitude shape. Digit keys load a preset
 * (Magnitude -= compensate on the pan/pitch tabs), Alt-digit stores
 * the current envelope (Flags & 7Fh, Magnitude += compensate). */
typedef struct presetenv_t {
    uint8_t   Flags, Num, LpB, LpE, SLB, SLE;
    envnode_t Nodes[25];
} presetenv_t;
static presetenv_t PresetEnv[10];
static int PresetEnvInit;

static void preset_init(void)
{
    int i;
    for (i = 0; i < 10; i++) {
        memset(&PresetEnv[i], 0, sizeof(PresetEnv[i]));
        PresetEnv[i].Num = 2;
        PresetEnv[i].Nodes[0].Magnitude = 32;
        PresetEnv[i].Nodes[1].Magnitude = 32;
        PresetEnv[i].Nodes[1].Tick = 100;
    }
    PresetEnvInit = 1;
}

static void preset_load(int n)
{
    env_t *e = cur_env();
    int comp = env_compensate(e), i;
    const presetenv_t *p = &PresetEnv[n];

    if (!PresetEnvInit)
        preset_init();
    p = &PresetEnv[n];
    ed_lock();
    e->Flags = p->Flags;
    e->Num = p->Num; e->LpB = p->LpB; e->LpE = p->LpE;
    e->SLB = p->SLB; e->SLE = p->SLE;
    for (i = 0; i < 25; i++) {
        e->NodePoints[i].Magnitude =
            (int8_t)(p->Nodes[i].Magnitude - comp);
        e->NodePoints[i].Tick = p->Nodes[i].Tick;
    }
    ed_unlock();
    if (CurrentNode >= e->Num)
        CurrentNode = 0;
}

static void preset_save(int n)
{
    env_t *e = cur_env();
    int comp = env_compensate(e), i;
    presetenv_t *p;

    if (!PresetEnvInit)
        preset_init();
    p = &PresetEnv[n];
    p->Flags = (uint8_t)(e->Flags & 0x7F);
    p->Num = e->Num; p->LpB = e->LpB; p->LpE = e->LpE;
    p->SLB = e->SLB; p->SLE = e->SLE;
    for (i = 0; i < 25; i++) {
        p->Nodes[i].Magnitude =
            (int8_t)(e->NodePoints[i].Magnitude + comp);
        p->Nodes[i].Tick = e->NodePoints[i].Tick;
    }
    status("Envelope preset %d set", n);
}

static int env_ckey(int key)               /* I_PostEnvelope key model */
{
    env_t *e = cur_env();

    if (NodeHeld) {
        switch (key) {
        case ITK_ENTER:     NodeHeld = 0; return 1;
        case ITK_UP:        env_node_value(1);  return 1;
        case ITK_DOWN:      env_node_value(-1); return 1;
        case ITK_PGUP:      env_node_value(8);  return 1;
        case ITK_PGDN:      env_node_value(-8); return 1;
        case ITK_LEFT:      env_node_tick(-1, 0);  return 1;
        case ITK_RIGHT:     env_node_tick(1, 0);   return 1;
        case ITK_TAB:       env_node_tick(16, 0);  return 1;
        case ITK_SHIFT_TAB: env_node_tick(-16, 0); return 1;
        case ITK_HOME:      env_node_tick(0, -1);  return 1;
        case ITK_END:       env_node_tick(0, 1);   return 1;
        case ITK_INS:       env_insert_node(); return 1;
        case ITK_DEL:       env_delete_node(); return 1;
        default: break;
        }
    } else {
        switch (key) {
        case ITK_LEFT:
            if (CurrentNode > 0) CurrentNode--;
            return 1;
        case ITK_RIGHT:
            if (CurrentNode + 1 < e->Num) CurrentNode++;
            return 1;
        case ITK_ENTER:
            NodeHeld = 1;
            env_selected(e);
            return 1;
        case ITK_UP:   FocusIdx[Screen] = IdxTabBtn; return 1;
        case ITK_DOWN: FocusIdx[Screen] = IdxEnvOn;  return 1;
        case ITK_INS:  env_insert_node(); return 1;
        case ITK_DEL:  env_delete_node(); return 1;
        default: break;
        }
        if (key >= '0' && key <= '9') {    /* load preset */
            preset_load(key - '0');
            return 1;
        }
        if (key >= ITK_ALT_0 && key <= ITK_ALT_0 + 9) {
            preset_save(key - ITK_ALT_0);  /* store preset */
            return 1;
        }
    }
    return 0;
}

/* mouse on the envelope canvas: select/drag the node under the pointer
 * (I_MouseEnvelopePress / I_MouseEnvelopeDrag, pixel-precise) */
static int env_node_at(const it_mouse_t *m)
{
    env_t *e = cur_env();
    int comp = env_compensate(e), i;

    for (i = 0; i < e->Num; i++) {
        int xp = 32*8 + e->NodePoints[i].Tick * 250 / EnvUpperLimit + 3;
        int yp = 18*8 + 61 * (64 - comp - e->NodePoints[i].Magnitude)
                        / 64 + 1;
        if (m->px >= xp - 3 && m->px <= xp + 3 &&
            m->py >= yp - 3 && m->py <= yp + 3)
            return i;
    }
    return -1;
}

static void env_cdrag(const it_mouse_t *m)
{
    env_t *e = cur_env();
    int comp = env_compensate(e);
    int cur = CurrentNode, t, v;

    if (!e->Num)
        return;
    v = 64 - comp - (m->py - 18*8 - 1) * 64 / 61;
    if (v + comp < 0)  v = -comp;
    if (v + comp > 64) v = 64 - comp;
    ed_lock();
    e->NodePoints[cur].Magnitude = (int8_t)v;
    ed_unlock();
    if (cur > 0) {
        t = (m->px - 32*8 - 3) * EnvUpperLimit / 250;
        if (t < e->NodePoints[cur - 1].Tick + 1)
            t = e->NodePoints[cur - 1].Tick + 1;
        if (cur + 1 < e->Num && t > e->NodePoints[cur + 1].Tick - 1)
            t = e->NodePoints[cur + 1].Tick - 1;
        if (t > MAXENVELOPETICK)
            t = MAXENVELOPETICK;
        ed_lock();
        e->NodePoints[cur].Tick = (uint16_t)t;
        ed_unlock();
    }
}

static void env_cclick(const it_mouse_t *m)
{
    int i = env_node_at(m);
    if (i >= 0) {
        CurrentNode = i;
        env_selected(cur_env());
    }
}

static void env_cdraw(int focused)         /* I_DrawEnvelope */
{
    static const char *hdr[3][2] = {
        { "Volume Envelope",    "Volume Envelope (Edit)"    },
        { "Panning Envelope",   "Panning Envelope (Edit)"   },
        { "Frequency Envelope", "Frequency Envelope (Edit)" },
    };
    env_t *e = cur_env();
    int cx, cy, val, disp;

    env_map();

    drawf(33, 16, focused ? 0x23 : 0x20, "%-22s",
          hdr[InsTab - 1][NodeHeld ? 1 : 0]);

    for (cy = 0; cy < 8; cy++)             /* the canvas cell grid */
        for (cx = 0; cx < 32; cx++)
            Screen_PutChar(32 + cx, 18 + cy,
                           (uint8_t)(cy * 32 + cx), 0x0C);

    val = e->Num ? e->NodePoints[CurrentNode].Magnitude : 0;
    disp = (e->Flags & 0x80) ? val + 32 : val;
    drawf(66, 19, 0x02, "Node %d/%d", CurrentNode, e->Num);
    drawf(66, 21, 0x02, "Tick %d",
          e->Num ? e->NodePoints[CurrentNode].Tick : 0);
    drawf(66, 23, 0x02, "Value %d", disp);
}

/* ---- note-translation window (I_DrawNoteWindow, IT_I.ASM 5374) ---- */
static const int NotePosTable[4] = { 4, 6, 8, 9 };

static void notewin_cdraw(int focused)
{
    instrument_t *ins = cur_ins();
    int i;

    if (NoteWinSel < NoteWinTop)
        NoteWinTop = NoteWinSel;
    if (NoteWinSel > NoteWinTop + 31)
        NoteWinTop = NoteWinSel - 31;

    for (i = 0; i < 32; i++) {
        int note = NoteWinTop + i, y = 16 + i;
        uint8_t nt = ins->NoteSampleTable[note * 2];
        uint8_t sm = ins->NoteSampleTable[note * 2 + 1];
        uint8_t a = 0x02;

        if (focused && note == NoteWinSel)
            a |= 0xE0;                     /* row hilight (+0E0h): from
                                              I_PreNoteWindow, so only
                                              while focused (#37) */

        Screen_PutChar(32, y, (uint8_t)NoteNameChars[(note%12)*2],   a);
        Screen_PutChar(33, y, (uint8_t)NoteNameChars[(note%12)*2+1], a);
        Screen_PutChar(34, y, (uint8_t)('0' + note / 12), a);
        Screen_PutChar(35, y, 0xA8, a);
        Screen_PutChar(36, y, (uint8_t)NoteNameChars[(nt%12)*2],   a);
        Screen_PutChar(37, y, (uint8_t)NoteNameChars[(nt%12)*2+1], a);
        Screen_PutChar(38, y, (uint8_t)('0' + nt / 12), a);
        Screen_PutChar(39, y, ' ', a);
        if (sm == 0) {
            Screen_PutChar(40, y, 173, a);
            Screen_PutChar(41, y, 173, a);
        } else {
            Screen_PutChar(40, y, (uint8_t)('0' + sm / 10), a);
            Screen_PutChar(41, y, (uint8_t)('0' + sm % 10), a);
        }
        if (focused && note == NoteWinSel) {
            int cxp = 32 + NotePosTable[NotePos];
            /* cursor cell attr 30h (I_PreNoteWindow) */
            uint8_t ch;
            switch (NotePos) {
            case 0:  ch = (uint8_t)NoteNameChars[(nt%12)*2]; break;
            case 1:  ch = (uint8_t)('0' + nt / 12); break;
            case 2:  ch = sm ? (uint8_t)('0' + sm / 10) : 173; break;
            default: ch = sm ? (uint8_t)('0' + sm % 10) : 173; break;
            }
            Screen_PutChar(cxp, y, ch, 0x30);
        }
    }
}

static int list_play_channel(void);         /* Alt-N multichannel */

static void notewin_play_current(void)
{
    CurInstr = ListSel + 1;
    jam_note(NoteWinSel + 1, list_play_channel());
}

static int notewin_ckey(int key)           /* NoteListKeys+PostNoteWindow */
{
    instrument_t *ins = cur_ins();
    uint8_t *entry = &ins->NoteSampleTable[NoteWinSel * 2];

    switch (key) {
    case ITK_UP:
        if (NoteWinSel > 0) NoteWinSel--;
        else FocusIdx[Screen] = IdxTabBtn;
        return 1;
    case ITK_DOWN:
        if (NoteWinSel < 119) NoteWinSel++;
        return 1;
    case ITK_PGUP:
        NoteWinSel -= 12; if (NoteWinSel < 0) NoteWinSel = 0;
        return 1;
    case ITK_PGDN:
        NoteWinSel += 12; if (NoteWinSel > 119) NoteWinSel = 119;
        return 1;
    case ITK_HOME: NoteWinSel = 0;   return 1;
    case ITK_END:  NoteWinSel = 119; return 1;
    case ITK_LEFT:  if (NotePos > 0) NotePos--; return 1;
    case ITK_RIGHT: if (NotePos < 3) NotePos++; return 1;
    case ITK_TAB:       FocusIdx[Screen] = IdxNNACut;   return 1;
    case ITK_SHIFT_TAB: FocusIdx[Screen] = IdxLeftList; return 1;
    case '>': case '\'':
        if (NoteSampleNumber < 99) NoteSampleNumber++;
        return 1;
    case '<': case ';':
        if (NoteSampleNumber > 0) NoteSampleNumber--;
        return 1;
    case ITK_ENTER:                        /* I_NoteSamplePickUp */
        NoteSampleNumber = entry[1];
        return 1;
    case ITK_ALT_A + ('A'-'A'): {          /* I_NoteAll: identity + smp */
        int n;
        ed_lock();
        for (n = 0; n < 120; n++) {
            ins->NoteSampleTable[n * 2] = (uint8_t)n;
            ins->NoteSampleTable[n * 2 + 1] = NoteSampleNumber;
        }
        ed_unlock();
        return 1;
    }
    case ITK_ALT_A + ('N'-'A'):            /* I_NoteNext */
        if (NoteWinSel > 0) {
            uint8_t *prev = entry - 2;
            if (prev[0] < 119) {
                ed_lock();
                entry[0] = (uint8_t)(prev[0] + 1);
                entry[1] = prev[1];
                ed_unlock();
            }
        }
        return 1;
    case ITK_ALT_A + ('P'-'A'):            /* I_NotePrevious */
        if (NoteWinSel < 119) {
            uint8_t *next = entry + 2;
            if (next[0] != 0) {
                ed_lock();
                entry[0] = (uint8_t)(next[0] - 1);
                entry[1] = next[1];
                ed_unlock();
            }
        }
        return 1;
    case ITK_ALT_UP: {                     /* I_NoteTransposeUp */
        int n;
        ed_lock();
        for (n = 0; n < 120; n++) {
            uint8_t v = ins->NoteSampleTable[n * 2];
            ins->NoteSampleTable[n * 2] =
                (uint8_t)(v + 1 > 119 ? 119 : v + 1);
        }
        ed_unlock();
        return 1;
    }
    case ITK_ALT_DOWN: {                   /* I_NoteTransposeDown */
        int n;
        ed_lock();
        for (n = 0; n < 120; n++) {
            uint8_t v = ins->NoteSampleTable[n * 2];
            ins->NoteSampleTable[n * 2] = (uint8_t)(v ? v - 1 : 0);
        }
        ed_unlock();
        return 1;
    }
    case ITK_ALT_INS: {                    /* I_NoteInsert: shift down */
        ed_lock();
        memmove(ins->NoteSampleTable + 2, ins->NoteSampleTable,
                119 * 2);
        ins->NoteSampleTable[0] = 0;
        ins->NoteSampleTable[1] = 0;
        ed_unlock();
        return 1;
    }
    case ITK_ALT_DEL: {                    /* I_NoteDelete: shift up */
        ed_lock();
        memmove(ins->NoteSampleTable, ins->NoteSampleTable + 2,
                119 * 2);
        ins->NoteSampleTable[119 * 2] = 0;
        ins->NoteSampleTable[119 * 2 + 1] = 0;
        ed_unlock();
        return 1;
    }
    case ' ':                              /* I_NoteSpace */
        if (NotePos >= 2) {
            ed_lock();
            entry[1] = NoteSampleNumber;
            ed_unlock();
            if (NoteWinSel < 119) NoteWinSel++;
            return 1;
        }
        return 0;
    default:
        break;
    }

    if (NotePos == 0) {                    /* piano keys set note+sample */
        int gn = key_to_note_plain();
        if (gn > 0 && gn <= 120) {
            ed_lock();
            entry[0] = (uint8_t)(gn - 1);
            entry[1] = NoteSampleNumber;
            ed_unlock();
            notewin_play_current();
            if (NoteWinSel < 119) NoteWinSel++;
            return 1;
        }
    } else if (key >= '0' && key <= '9') {
        int d = key - '0';
        ed_lock();
        if (NotePos == 1) {                /* octave digit */
            int nn = (entry[0] % 12) + d * 12;
            if (nn <= 119)
                entry[0] = (uint8_t)nn;
        } else if (NotePos == 2) {         /* sample tens */
            entry[1] = (uint8_t)(d * 10 + entry[1] % 10);
            NoteSampleNumber = entry[1];
        } else {                           /* sample units */
            entry[1] = (uint8_t)((entry[1] / 10) * 10 + d);
            NoteSampleNumber = entry[1];
            NotePos = 2;
        }
        ed_unlock();
        if (NoteWinSel < 119) NoteWinSel++;
        return 1;
    } else if (key == '.' && NotePos >= 2) {
        ed_lock();
        entry[1] = 0;
        ed_unlock();
        if (NoteWinSel < 119) NoteWinSel++;
        return 1;
    }
    return 0;
}

static void notewin_cclick(const it_mouse_t *m)
{
    int note = NoteWinTop + (m->y - 16);
    if (note < 0) note = 0;
    if (note > 119) note = 119;
    NoteWinSel = note;
}

/* ---- Pitch-Pan Center (I_DrawPitchPanCenter, IT_I.ASM 8799) ---- */
static void ppc_cdraw(int focused)
{
    instrument_t *ins = cur_ins();
    int n = ins->PPC % 120;
    uint8_t a = focused ? 0x03 : 0x02;

    Screen_PutChar(54, 45, (uint8_t)NoteNameChars[(n%12)*2],   a);
    Screen_PutChar(55, 45, (uint8_t)NoteNameChars[(n%12)*2+1], a);
    Screen_PutChar(56, 45, (uint8_t)('0' + n / 12), a);
}

static int ppc_ckey(int key)
{
    instrument_t *ins = cur_ins();

    switch (key) {
    case ITK_RIGHT: case '+':
        if (ins->PPC < 119) { ed_lock(); ins->PPC++; ed_unlock(); }
        return 1;
    case ITK_LEFT: case '-':
        if (ins->PPC > 0) { ed_lock(); ins->PPC--; ed_unlock(); }
        return 1;
    default:
        return 0;
    }
}

/* ---- the four object lists ---- */
static void act_ins_changed(void)
{
    NodeHeld = 0;                          /* selection/tab change */
}

static void draw_instruments(void)
{
    int i, n = 99;              /* IT lists all 99 slots */
    int top, focusw;
    instrument_t *ins;
    widget_t *w;

    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;
    top = InsListTop;
    if (ListSel < top) top = ListSel;
    if (ListSel > top + 34) top = ListSel - 34;
    if (top > n - 35) top = n - 35;
    if (top < 0) top = 0;
    InsListTop = top;

    ins = cur_ins();
    NW = 0;
    focusw = FocusIdx[Screen];

    /* InstrumentNameBox + I_DrawInstrumentWindow: numbers at (2,y)
     * attr 20h, 25-char names at (5,y) attr 06h, current row E6h,
     * keyboard focus 30h (I_PreInstrumentWindow) */
    IdxLeftList = NW;
    wlist(2, 13, 29, 47, 13, instr_list_lkey, instr_list_lclick);
    Screen_DrawBox(4, 12, 30, 48, 27);
    for (i = 0; i < 35; i++) {
        int idx = top + i;
        uint8_t a;
        if (idx >= n)
            break;
        a = (idx == ListSel) ? ((focusw == IdxLeftList) ? 0x30 : 0xE6)
                             : 0x06;
        if (idx == ListSel && InstrumentEdit)
            a = 0x06;           /* edit mode: single cursor cell
                                   (I_PreInstrumentWindow2) */
        drawf(2, 13 + i, 0x20, "%02d", (idx + 1) % 100);
        draw_itname(5, 13 + i, Song.Ins[idx].InstrumentName, 25, a);
        if (idx == ListSel && InstrumentEdit)
            Screen_SetAttr(5 + InstrumentPos, 13 + i, 0x30);
    }
    if (Song.Header.Flags & ITF_INSTRUMENTS)    /* I_ShowInstrumentPlay */
        draw_play_dots(InstrumentPlayTable, top + 1, 1);

    /* tab buttons (G/V-InstrumentGeneral/Volume/Panning/PitchButton) */
    IdxTabBtn = NW + InsTab;
    wradio8(31, 12, 41, 14, " General", &InsTab, 3, 0)->action
        = act_ins_changed;
    wradio8(43, 12, 53, 14, " Volume",  &InsTab, 3, 1)->action
        = act_ins_changed;
    wradio8(55, 12, 65, 14, " Panning", &InsTab, 3, 2)->action
        = act_ins_changed;
    wradio8(67, 12, 77, 14, "  Pitch",  &InsTab, 3, 3)->action
        = act_ins_changed;

    if (InsTab == 0) {
        /* ---- General (O1_InstrumentListGeneral) ---- */
        Screen_DrawBox(31, 15, 42, 48, 27);        /* TranslateBox */
        wcustom(32, 16, 41, 47, notewin_cdraw, notewin_ckey,
                notewin_cclick);

        fill(44, 15, 35, 134, 0x20);               /* NNADivision */
        fill(44, 30, 35, 134, 0x20);               /* DCTDivision */
        fill(44, 45, 35, 154, 0x20);               /* FileDivision */
        Screen_DrawString(54, 17, "New Note Action", 0x20);
        Screen_DrawString(47, 32, "Duplicate Check Type & Action", 0x20);
        Screen_DrawString(47, 47, "Filename", 0x20);

        IdxNNACut = NW;
        wradio8(45, 18, 77, 20, "  Note Cut",  &ins->NNA, 3, 0);
        wradio8(45, 21, 77, 23, "  Continue",  &ins->NNA, 3, 1);
        wradio8(45, 24, 77, 26, "  Note Off",  &ins->NNA, 3, 2);
        wradio8(45, 27, 77, 29, "  Note Fade", &ins->NNA, 3, 3);

        wradio8(45, 33, 60, 35, "  Disabled",   &ins->DCT, 3, 0);
        wradio8(45, 36, 60, 38, "  Note",       &ins->DCT, 3, 1);
        wradio8(45, 39, 60, 41, "  Sample",     &ins->DCT, 3, 2);
        wradio8(45, 42, 60, 44, "  Instrument", &ins->DCT, 3, 3);

        wradio8(61, 33, 77, 35, "  Note Cut",  &ins->DCA, 3, 0);
        wradio8(61, 36, 77, 38, "  Note Off",  &ins->DCA, 3, 1);
        wradio8(61, 39, 77, 41, "  Note Fade", &ins->DCA, 3, 2);

        Screen_DrawBox(55, 46, 73, 48, 27);        /* FilenameBox */
        wtext(56, 47, ins->DOSFileName, 12);
    } else {
        /* ---- shared envelope frame (Volume/Panning/Pitch tabs) ---- */
        static const uint8_t VELText[] =
            "Envelope Loop\015   Loop Begin\015\377\005 Loop End";
        static const uint8_t VESLText[] =
            " Sustain Loop\015SusLoop Begin\015  SusLoop End";
        env_t *e = cur_env();
        int nodemax = e->Num ? e->Num - 1 : 0;

        Screen_DrawBox(31, 17, 77, 26, 27);        /* EnvelopeBox */
        Screen_DrawBox(53, 27, 63, 30, 27);        /* VEBox */
        Screen_DrawBox(53, 31, 63, 35, 27);        /* VELBox */
        Screen_DrawBox(53, 36, 63, 40, 27);        /* VESLBox */

        if (InsTab == 1)
            Screen_DrawStringCtl(38, 28, (const uint8_t *)
                "Volume Envelope\015          Carry", 0x20, NULL);
        else if (InsTab == 2)
            Screen_DrawStringCtl(37, 28, (const uint8_t *)
                "Panning Envelope\015           Carry", 0x20, NULL);
        else
            Screen_DrawStringCtl(35, 28, (const uint8_t *)
                "Frequency Envelope\015             Carry", 0x20, NULL);
        Screen_DrawStringCtl(40, 32, VELText, 0x20, NULL);
        Screen_DrawStringCtl(40, 37, VESLText, 0x20, NULL);

        wcustom(32, 18, 63, 25, env_cdraw, env_ckey, env_cclick)->cdrag
            = env_cdrag;

        IdxEnvOn = NW;
        wtoggle8(54, 28, &e->Flags, 1);            /* Envelope On/Off */
        wtoggle8(54, 29, &e->Flags, 8);            /* Carry */
        wtoggle8(54, 32, &e->Flags, 2);            /* Envelope Loop */
        wnum3(54, 33, &e->LpB, 0, nodemax, env_fix_loop_pairs);
        wnum3(54, 34, &e->LpE, 0, nodemax, env_fix_loop_pairs);
        wtoggle8(54, 37, &e->Flags, 4);            /* Sustain Loop */
        wnum3(54, 38, &e->SLB, 0, nodemax, env_fix_loop_pairs);
        wnum3(54, 39, &e->SLE, 0, nodemax, env_fix_loop_pairs);

        if (InsTab == 1) {
            /* ---- Volume tab extras ---- */
            Screen_DrawBox(53, 41, 71, 44, 27);    /* GlobalVolumeBox */
            Screen_DrawStringCtl(39, 42, (const uint8_t *)
                " Global Volume\015\377\007 Fadeout\015\015\015"
                "Volume Swing %", 0x20, NULL);
            wthumb(54, 42, 0, 128, &ins->GbV, 0, 0);
            w = wthumb(54, 43, 0, 256, NULL, 0, 16);
            w->v16 = &ins->FadeOut;
            Screen_DrawBox(53, 45, 71, 47, 27);    /* RandomVolBox */
            wthumb(54, 46, 0, 100, &ins->RV, 0, 16);
        } else if (InsTab == 2) {
            /* ---- Panning tab extras ---- */
            Screen_DrawBox(53, 41, 63, 48, 27);    /* DefaultPanBox */
            Screen_DrawStringCtl(33, 42, (const uint8_t *)
                "\377\011 Default Pan\015\377\013 Pan Value\015\015"
                "\377\004 Pitch-Pan Center\015Pitch-Pan Separation\015"
                "\377\013 Pan swing", 0x20, NULL);
            wtoggle8(54, 42, &ins->DfP, 0x80);
            wthumb(54, 43, 0, 64, &ins->DfP, 0x7F, 0);
            fill(54, 44, 9, 0x9A, 0x02);           /* PanBoxFiller */
            wcustom(54, 45, 56, 45, ppc_cdraw, ppc_ckey, NULL);
            wthumb(54, 46, -32, 32, &ins->PPS, 0, 0)->sgn = 1;
            wthumb(54, 47, 0, 64, &ins->RP, 0, 0);
        } else {
            /* ---- Pitch tab extras (FILTERENVELOPES=1 layout) ---- */
            uint8_t *bnk = (uint8_t *)&ins->MIDIBnk;
            Screen_DrawBox(53, 41, 71, 48, 27);    /* MIDIBox1 */
            Screen_DrawStringCtl(36, 42, (const uint8_t *)
                "Default Cutoff\015Default Resonance\015MIDI Channel\015"
                "MIDI Program\015MIDI Bank Low\015MIDI Bank High",
                0x20, NULL);
            wthumb(54, 42, 0, 127, &ins->IFC, 0, 16);
            wthumb(54, 43, 0, 127, &ins->IFR, 0, 16);
            wthumb(54, 44, 0, 17,  &ins->MCh, 0, 16);
            wthumb(54, 45, -1, 127, &ins->MPr, 0, 16)->sgn = 1;
            wthumb(54, 46, -1, 127, &bnk[0], 0, 16)->sgn = 1;
            wthumb(54, 47, -1, 127, &bnk[1], 0, 16)->sgn = 1;
        }
    }

    widgets_draw();
}

/* ===================================================================
 * Order list and panning (F11) -- PanBox/F_ShowChannels coordinates
 * =================================================================== */
static const uint8_t PanHeaderText[] =
    { 146, 0xFE, 0x30, 'L', ' ', ' ', ' ', 'M', ' ', ' ', ' ', 'R',
      0xFE, 0x23, 145, 0 };

static void draw_order(void)
{
    int i;
    int focusw = FocusIdx[SCR_ORDER];

    if (ListSel < 0) ListSel = 0;
    if (ListSel > 255) ListSel = 255;
    if (PanSel < 0) PanSel = 0;
    if (PanSel > 63) PanSel = 63;

    NW = 0;
    wlist(6, 15, 8, 46, 15, order_list_lkey, order_list_lclick);
    wlist(20, 15, 39, 46, 15, pan_left_lkey, pan_left_lclick);
    wlist(54, 15, 73, 46, 15, pan_right_lkey, pan_right_lclick);

    /* order list, type-12 object at (2,15), 32 entries: all 256 slots
     * are always navigable (PE_DrawOrderList scroll-into-view; values
     * attr 02h, "---"/"+++" for 255/254, playing order 23h row number,
     * the PE_PreOrderList single-digit 30h cursor when focused) */
    {
        int top = OrdListTop;

        if (top > ListSel) top = ListSel;
        if (top + 32 <= ListSel) top = ListSel - 31;
        if (top > 256 - 32) top = 256 - 32;
        if (top < 0) top = 0;
        OrdListTop = top;

        Screen_DrawBox(5, 14, 9, 47, 27);
        for (i = 0; i < 32; i++) {
            int idx = top + i;
            uint8_t o = Song.Orders[idx];
            uint8_t na = 0x20;
            char val[8];

            if (PlayMode == 2 && idx == (int)CurrentOrder)
                na = 0x23;                      /* PE_ShowOrder */
            draw3num(2, 15 + i, idx, na);   /* the object's x (#38) */
            if (o == 255)
                memcpy(val, "---", 4);
            else if (o == 254)
                memcpy(val, "+++", 4);
            else
                snprintf(val, sizeof(val), "%03d", o);
            Screen_DrawString(6, 15 + i, val, 0x02);
            if (idx == ListSel && focusw == 0) {
                char c[2];
                c[0] = val[OrderCursor];
                c[1] = 0;
                Screen_DrawString(6 + OrderCursor, 15 + i, c, 0x30);
            }
        }
    }

    /* channel panning columns */
    Screen_DrawBox(30, 14, 40, 47, 15);
    Screen_DrawBox(64, 14, 74, 47, 15);
    Screen_DrawStringCtl(30, 14, PanHeaderText, 0x23, NULL);
    Screen_DrawStringCtl(64, 14, PanHeaderText, 0x23, NULL);

    for (i = 0; i < 32; i++) {
        int c;

        for (c = 0; c < 2; c++) {
            int chan = i + c * 32;
            int sel = (PanSel == chan) && (focusw == 1 + c);
            int bx = c ? 65 : 31;
            uint8_t pan = Song.Header.ChnlPan[chan];

            drawf(bx - 11, 15 + i, sel ? 0x30 : 0x20,
                  "Channel %02d", chan + 1);
            /* Channel1..64 are type-9 objects (IT_OBJ1.ASM): an ordinary
             * F_DrawThumbBar over 0..64 -- the fine fractional thumb
             * glyphs and the 3-digit value in attr 21h, as on every other
             * slider (issue #10). Values outside 0..64 skip the bar; its
             * draw hook DrawPanning (IT_F.ASM 2690) then writes
             * "  Muted" (bit 7) or "Surround" (100) at the bar's x in
             * attr 5, and nothing for any other out-of-range value. */
            if (pan <= 64)
                draw_thumbbar(bx, 15 + i, 0, 64, pan, sel ? 0x03 : 0x02);
            else if (pan & 0x80)
                Screen_DrawString(bx, 15 + i, "  Muted", 0x05);
            else if (pan == 100)
                Screen_DrawString(bx, 15 + i, "Surround", 0x05);
        }
    }

    widgets_draw();
}

/* ===================================================================
 * Song variables (F12) -- object coordinates verbatim
 * =================================================================== */
static const uint8_t SongVarLabels[] =
    "\377\005 Song Name\015\015\015"
    " Initial Tempo\015 Initial Speed\015\015\015"
    " Global Volume\015 Mixing Volume\015"
    "\377\004 Separation\015   Old Effects\015Compatible Gxx\015\015\015"
    "\377\007 Control\015\015\015"
    "\377\006 Playback\015\015\015"
    "  Pitch Slides";

static const uint8_t DirLabels[] =
    "\377\004 Module\015\377\004 Sample\015Instrument";

static void draw_vars(void)
{
    Screen_DrawString(33, 13, "Song Variables", 0x23);
    Screen_DrawBox(16, 15, 43, 17, 25);             /* SongNameBox */
    Screen_DrawBox(16, 18, 50, 21, 9);              /* InitialSpeedBox */
    Screen_DrawBox(16, 22, 34, 28, 25);             /* VolumeBox */
    Screen_DrawStringCtl(2, 16, SongVarLabels, 0x20, NULL);

    NW = 0;
    wtext(17, 16, Song.Header.SongName, 25);

    {
        widget_t *w;
        w = wthumb(17, 19, 31, 255, &Song.Header.IT, 0, 32);
        w->action = act_tempo_changed;
        w = wthumb(17, 20, 1, 255, &Song.Header.IS, 0, 32);
        w->action = act_speed_changed;
    }
    wthumb(17, 23, 0, 128, &Song.Header.GV, 0, 0)->action = act_gv_changed;
    wthumb(17, 24, 0, 128, &Song.Header.MV, 0, 0)->action = act_mv_changed;
    wthumb(17, 25, 0, 128, &Song.Header.Sep, 0, 0)->action = act_stereo_changed;
    wtogglef(17, 26, &Song.Header.Flags, ITF_OLD_EFFECTS);
    wtogglef(17, 27, &Song.Header.Flags, ITF_LINK_G_TO_EF);

    wradiof(16, 29, 30, 31, " Instruments", &Song.Header.Flags,
            ITF_INSTRUMENTS, 0)->action = act_enable_instruments;
    wradiof(31, 29, 45, 31, " Samples", &Song.Header.Flags,
            ITF_INSTRUMENTS, 1);
    wradiof(16, 32, 30, 34, " Stereo", &Song.Header.Flags,
            ITF_STEREO, 0)->action = act_stereo_changed;
    wradiof(31, 32, 45, 34, " Mono", &Song.Header.Flags,
            ITF_STEREO, 1)->action = act_stereo_changed;
    wradiof(16, 35, 30, 37, " Linear", &Song.Header.Flags,
            ITF_LINEAR_SLIDES, 0);
    wradiof(31, 35, 45, 37, " Amiga", &Song.Header.Flags,
            ITF_LINEAR_SLIDES, 1);

    Screen_DrawStringCtl(1, 39, (const uint8_t *)"\377\116\201", 0x21, NULL);
    Screen_DrawString(34, 40, "Directories", 0x23);
    Screen_DrawBox(12, 41, 78, 45, 27);             /* DirectoryInputBox */
    Screen_DrawStringCtl(2, 42, DirLabels, 0x20, NULL);
    wtext(13, 42, DirModule, 64);
    wtext(13, 43, DirSample, 64);
    wtext(13, 44, DirInstr, 64);

    wbutton(27, 46, 52, 48, "  Save all Preferences", act_save_prefs);

    widgets_draw();
}

/* ===================================================================
 * Help (F1)
 * =================================================================== */
/* Ctrl-F1 keypress table. Shows both halves of the last few key events
 * side by side: the physical position the tracker matched note entry
 * against, and the character the active layout produced from it. On a
 * German keyboard the key printed Z reads scan 15h / char 'z' -- which
 * is exactly the evidence needed to hand-write a KEYBOARD.CFG. */
static void draw_keys(void)
{
    static const char *modname[7] = { "LSh", "RSh", "LCt", "RCt",
                                      "LAl", "RAl", NULL };
    const char *layout = Key_LayoutName();
    char line[80];
    int i, b, x;

    Screen_Clear(0x11);
    Screen_DrawBox(1, 1, 78, 46, 1);
    Screen_DrawString(3, 2, "Key press table", 0x0F);
    Screen_DrawString(3, 4,
        "Positions come from the hardware, characters from the layout.",
        0x0A);
    snprintf(line, sizeof(line), "Layout: %s",
             layout[0] ? layout : "host keyboard layout (no override)");
    Screen_DrawString(3, 5, line, 0x0A);

    Screen_DrawString(3, 7, "scan  char  code   modifiers", 0x0F);
    Screen_DrawString(3, 8,
        "----  ----  ----   ---------------------------------", 0x09);

    for (i = 0; i < KeyLogCount; i++) {
        const it_key_t *k = &KeyLog[i];
        uint8_t attr = i == 0 ? 0x0F : 0x09;

        snprintf(line, sizeof(line), "%02X    ", k->scan);
        Screen_DrawString(3, 9 + i, line, attr);
        if (k->ch >= 32)
            Screen_PutChar(9, 9 + i, (uint8_t)k->ch, attr);
        else
            Screen_PutChar(9, 9 + i, '-', attr);
        snprintf(line, sizeof(line), "%02X   %04X", k->ch, (unsigned)k->code);
        Screen_DrawString(11, 9 + i, line, attr);

        x = 26;
        if (!(k->flags & ITKF_PRESSED)) {
            Screen_DrawString(x, 9 + i, "release", attr);
        } else {
            for (b = 0; b < 6; b++)
                if (k->flags & (2 << b)) {
                    Screen_DrawString(x, 9 + i, modname[b], attr);
                    x += 4;
                }
        }
    }

    Screen_DrawString(3, 44, "Press any key to log it; ESC or F2 returns "
                             "to the pattern editor.", 0x0A);
}

/* ===================================================================
 * F1 help -- IT_H.ASM ported 1:1. The text tables come from the
 * original's data via tools/gen_help.py (src/it_help.inc); H_DrawHelp
 * expands each line's dictionary words and draws 32 lines from TopLine
 * in attr 06h inside HelpBox (1,12)-(78,45) style 27. Each context
 * remembers its scroll position (Positions); Done/Esc return to the
 * screen help was called from (Glbl_RestoreMode).
 * Contexts: 0 order list & panning, 1 pattern editor, 2 sample list,
 * 3 load module, 4 order list & volume, 5 configuration, 6 load sample,
 * 7 instrument list, 8 keyboard, 9 info page, 10 palette, 11 load
 * instrument, 12 message editor, 13/14 MIDI (only 0/1/2/4/7/9/12 carry
 * their own page; the rest show the global keys).
 * =================================================================== */
#include "it_help.inc"

static int HelpContext = 1, HelpTop = 0, HelpPositions[15];
static int HelpReturnScreen = SCR_PATTERN;

/* H_DrawHelp's decoder: bytes 80h..FDh insert a dictionary word (which
 * may itself contain words), 0FEh passes with the attribute byte after
 * it, 0FFh passes with its count and character */
static void help_expand(const uint8_t *src, uint8_t *out, int *n, int cap)
{
    uint8_t b;

    while ((b = *src++) != 0) {
        if (b < 0x80) {
            if (*n < cap) out[(*n)++] = b;
        } else if (b < 0xFE) {
            help_expand(HelpDecodeWords[b - 0x80], out, n, cap);
        } else {
            if (*n < cap) out[(*n)++] = b;
            if (b == 0xFF) {
                if (*n < cap) out[(*n)++] = *src++;
                if (*n < cap) out[(*n)++] = *src++;
            }
        }
    }
}

/* ---- port-owned help lines (feature 016) ----
 * Not IT's text: kept apart from the generated it_help.inc, in the same
 * line encoding (start column, text, 0FFh repeat, 80h.. dictionary). On
 * macOS the note preview is Right Option (issue #20), so that one line
 * names it, with "Preview" in the original column 17 (13 + 4 spaces =
 * 16 + 1). Where the system file dialogs exist, the keys are listed
 * after the original lines under a heading that marks them as additions. */
#define HL(s) ((const uint8_t *)(s))
static const char PortHelpPreview[] =
    "\x05" "Right Option+Key" "\xFF\x01\x20" "Preview " "\xA5";
static const char PortHelpHead[] =
    "\x03" "it26 additions (not in Impulse Tracker).";
static const char PortHelpOpen[] =
    "\x05" "Ctrl-Shift-F9     Open module (system dialog)";
static const char PortHelpSave[] =
    "\x05" "Ctrl-Shift-F10    Save module as (system dialog)";
static const char PortHelpSmp[] =
    "\x05" "Ctrl-O            Load sample into this slot (system dialog)";
static const char PortHelpIns[] =
    "\x05" "Ctrl-O            Load instrument into this slot (system dialog)";
static const char PortHelpDir[] =
    "\x05" "Ctrl-O            Choose folder for a path field (system dialog)";
/* macOS: the shorter Cmd twins (F-keys need Fn there) */
static const char PortHelpOpenMac[] =
    "\x05" "Cmd-F9            Open module (system dialog)";
static const char PortHelpSaveMac[] =
    "\x05" "Cmd-F10           Save module as (system dialog)";
static const char PortHelpSmpMac[] =
    "\x05" "Cmd-O             Load sample into this slot (system dialog)";
static const char PortHelpInsMac[] =
    "\x05" "Cmd-O             Load instrument into this slot (system dialog)";
static const char PortHelpDirMac[] =
    "\x05" "Cmd-O             Choose folder for a path field (system dialog)";

/* the line list a context shows: a copy of IT's own with the port lines
 * applied (rebuilt per call -- a few hundred pointers) */
static const uint8_t *const *help_lines(int ctx)
{
    static const uint8_t *buf[400];
    const uint8_t *const *src = HelpContextPtrs[ctx];
    const char *pk = Screen_PreviewKeyLabel();
    int dlg = Screen_HasFileDialog(), n = 0;

    for (; *src && n < 380; src++) {
        if (*src == HLP_helpglobal_16)  /* "Ctrl-D  DOS Shell": no DOS to
                                           shell to (#28; parked for a
                                           later re-addition) */
            continue;
        buf[n++] = (pk && *src == HLP_helpcontext1_181) ? HL(PortHelpPreview)
                                                         : *src;
    }
    if (dlg) {
        int mac = Screen_DialogModLabel() != NULL;
        buf[n++] = HLP_newline;
        buf[n++] = HL(PortHelpHead);
        buf[n++] = HL(mac ? PortHelpOpenMac : PortHelpOpen);
        buf[n++] = HL(mac ? PortHelpSaveMac : PortHelpSave);
        if (ctx == 2)
            buf[n++] = HL(mac ? PortHelpSmpMac : PortHelpSmp);
        else if (ctx == 7)
            buf[n++] = HL(mac ? PortHelpInsMac : PortHelpIns);
        else if (ctx == 5 && HelpReturnScreen == SCR_VARS)  /* shared ctx */
            buf[n++] = HL(mac ? PortHelpDirMac : PortHelpDir);
    }
    buf[n] = NULL;
    return buf;
}

static void draw_help(void)
{
    const uint8_t *const *ln = help_lines(HelpContext) + HelpTop;
    int i;

    Screen_DrawBox(1, 12, 78, 45, 27);                  /* HelpBox */
    for (i = 0; i < 32 && ln[i]; i++) {                 /* H_DrawHelp */
        uint8_t buf[160];
        int n = 0;
        help_expand(ln[i] + 1, buf, &n, (int)sizeof(buf) - 1);
        buf[n] = 0;
        Screen_DrawStringCtl(ln[i][0], 13 + i, buf, 0x06, NULL);
    }
    NW = 0;
    wbutton(34, 46, 45, 48, "   Done", act_help_done);  /* HelpDoneButton */
    widgets_draw();
}

/* H_Help: remember where help was called from and show that screen's
 * context at its saved position */
static void help_open(int context)
{
    if (Screen != SCR_HELP)
        HelpReturnScreen = Screen;
    HelpContext = context;
    HelpTop = HelpPositions[context];
    Screen = SCR_HELP;
}

static int help_context_of(int scr)
{
    switch (scr) {
    case SCR_ORDER:       return 0;
    case SCR_PATTERN:     return 1;
    case SCR_SAMPLES:     return 2;
    case SCR_VARS:        return 5;
    case SCR_INSTRUMENTS: return 7;
    case SCR_KEYS:        return 8;
    case SCR_DRIVER:      return 5;
    case SCR_INFO:        return 9;
    case SCR_MESSAGE:     return 12;
    default:              return 1;
    }
}

/* HelpKeyList: Up/Down/PgUp/PgDn scroll (H_HelpDown stops once the
 * last line is on screen), Esc leaves; everything else falls through to
 * the global keys */
static int help_key(int key)
{
    const uint8_t *const *ln = help_lines(HelpContext);
    int i, rows = 0;

    while (ln[rows])
        rows++;
    switch (key) {
    case ITK_UP:   if (HelpTop > 0) HelpTop--; return 1;
    case ITK_DOWN: if (HelpTop + 32 < rows) HelpTop++; return 1;
    case ITK_PGUP: HelpTop = HelpTop > 32 ? HelpTop - 32 : 0; return 1;
    case ITK_PGDN:
        for (i = 0; i < 32; i++)
            if (HelpTop + 32 < rows) HelpTop++;
        return 1;
    default: break;
    }
    return 0;
}

/* ===================================================================
 * Info page (F5) -- IT_DISPL.ASM ported 1:1: the DisplayWindows split-
 * window engine (up to 5 windows stacked over rows 12..49) with all 11
 * view methods of the 2.17 build. Display_SampleDots is commented out
 * of the original's DisplayDataModes table and is excluded here too.
 *
 * The original decodes packed pattern data row by row with its own
 * repeat-bit decoder (LoadNextData / GotoRow over a DataDecode memory
 * table); this port realises identical semantics with Pattern_Unpack
 * (the player-exact decoder) into a scratch grid, plus the pattern
 * editor's own grid for its current pattern (the PE_GetCurrentPattern
 * "PatternArrayNumber" fast path of the original).
 * =================================================================== */

static void commit_current_pattern(void);
static void load_pattern(uint16_t pat);

typedef struct dispwin_t {          /* DisplayWindows entry             */
    uint16_t method;                /* +0 index into DisplayDataModes   */
    uint8_t  topchan;               /* +2 first channel shown           */
    uint8_t  topline;               /* +3 first screen row              */
    uint16_t length;                /* +4 rows (+6 offset is derived)   */
} dispwin_t;

/* 2.17 defaults: track view 12..31, Variables 32..34, 24-chn 35..49 */
static dispwin_t InfoWin[5] = {
    { 0, 0, 12, 20 }, { 8, 0, 32, 3 }, { 5, 0, 35, 15 },
    { 0, 0, 0, 0 }, { 0, 0, 0, 0 },
};
static int     InfoNumWindows = 3;  /* NumWindows                       */
static int     InfoCurWindow  = 0;  /* CurrentWindow                    */
static int     InfoProcWindow = 0;  /* ProcessWindow                    */
static int     InfoCurChannel = 0;  /* CurrentChannel                   */
static uint8_t InfoVelocity   = 0;  /* Velocity: 0 = sample-scan bars   */
static uint8_t InfoInstNames  = 0;  /* Instrument: 1 = instrument names */
static uint8_t InfoFullScreen = 0;  /* FullScreen                       */

/* local play-state copies (DrawDisplayData refreshes them per frame) */
static uint16_t IPlayMode, ICurRow, ICurPattern, ICurOrder;
static uint16_t InfoPEMaxRow;       /* PatternMaxRow                    */

/* row decode state (DataArray + the LoadNextData walk) */
static uint8_t    InfoRow[64][5];   /* per channel: note,ins,vol,cmd,val */
static editcell_t InfoGrid[MAX_PATROWS * 64];
static uint16_t   InfoGridPat = 0xFFFF;
static uint16_t   IDecodePattern, IDecodeRow, IDecodeMaxRow;
static int        INumbered;        /* span has row numbers (real pat)  */

static const char InfoNNAMsg[4][4] = { "Cut", "Con", "Off", "Fde" };
static const char Note2Table[13]   = "cCdDefFgGaAb";

static uint16_t info_pattern_rows(uint16_t pat)
{
    if (pat == CurPattern)
        return InfoPEMaxRow;
    if (pat < MAX_PATTERNS && Song.Patterns[pat].PackedData)
        return Song.Patterns[pat].Rows;
    return 64;
}

static const editcell_t *info_cells(uint16_t pat)
{
    if (pat == CurPattern)
        return Grid;
    if (InfoGridPat != pat) {
        Pattern_Unpack(pat, InfoGrid);
        InfoGridPat = pat;
    }
    return InfoGrid;
}

/* LoadNextData: fetch the next row of IDecodePattern into InfoRow.
 * Empty cells become {0FDh,0,0FFh,0,0} exactly as the original's
 * DataArray init. Note that engine note 253 ("fade") shares 0FDh with
 * the empty marker -- authentic collision, fades display as empty. */
static void info_load_row(void)
{
    const editcell_t *g;
    int c;

    if (IDecodePattern == 0xFFFF || IDecodeRow >= MAX_PATROWS) {
        for (c = 0; c < 64; c++) {
            InfoRow[c][0] = 0xFD; InfoRow[c][1] = 0;
            InfoRow[c][2] = 0xFF; InfoRow[c][3] = 0; InfoRow[c][4] = 0;
        }
        IDecodeRow++;
        return;
    }
    g = info_cells(IDecodePattern) + (size_t)IDecodeRow * 64;
    for (c = 0; c < 64; c++) {
        const editcell_t *e = &g[c];
        if ((e->mask & CM_NOTE) && e->note)
            InfoRow[c][0] = (e->note <= 120) ? (uint8_t)(e->note - 1)
                                             : e->note;
        else
            InfoRow[c][0] = 0xFD;
        InfoRow[c][1] = (e->mask & CM_INS) ? e->ins : 0;
        InfoRow[c][2] = (e->mask & CM_VOL) ? e->vol : 0xFF;
        InfoRow[c][3] = (e->mask & CM_CMD) ? e->cmd : 0;
        InfoRow[c][4] = (e->mask & CM_CMD) ? e->cmdval : 0;
    }
    IDecodeRow++;
}

/* GetBeforeRows: rows above the current pattern's span -- pattern mode
 * wraps to the same pattern's end, song mode shows the previous order's
 * pattern (blank rows at order 0 / end markers). */
static int info_before_rows(const dispwin_t *w)
{
    int cnt = (((int)w->length - 4) >> 1) - (int)ICurRow;
    uint16_t pat;

    INumbered = 0;
    if (cnt <= 0)
        return 0;
    if (IPlayMode != 1) {
        if (ICurOrder == 0 || (pat = Song.Orders[ICurOrder - 1]) > 199) {
            IDecodePattern = 0xFFFF;
            return cnt;
        }
    } else
        pat = ICurPattern;
    IDecodePattern = pat;
    {
        int start = (int)info_pattern_rows(pat) - cnt;
        if (start < 0)
            start = 0;              /* port safety (degenerate window) */
        IDecodeRow = (uint16_t)start;
    }
    INumbered = 1;
    return cnt;
}

/* GetCurrentPatternRows */
static int info_current_rows(const dispwin_t *w)
{
    int half  = ((int)w->length - 4) >> 1;
    int final = ((int)w->length - 3) - half + (int)ICurRow; /* exclusive */
    int start = (int)ICurRow - half;
    int cnt;

    if (start < 0)
        start = 0;
    IDecodePattern = ICurPattern;
    IDecodeMaxRow  = info_pattern_rows(ICurPattern);
    if (final > (int)IDecodeMaxRow)
        final = (int)IDecodeMaxRow;
    IDecodeRow = (uint16_t)start;
    cnt = final - start;
    if (cnt < 0)
        cnt = 0;
    INumbered = 1;
    return cnt;
}

/* GetAfterRows */
static int info_after_rows(const dispwin_t *w)
{
    int dx = ((int)w->length - 3) - (((int)w->length - 4) >> 1)
             + (int)ICurRow;
    int cnt = dx - (int)IDecodeMaxRow;
    uint16_t pat;

    INumbered = 0;
    if (cnt <= 0)
        return 0;
    if (IPlayMode != 1) {
        if (ICurOrder >= 255 || (pat = Song.Orders[ICurOrder + 1]) > 199) {
            IDecodePattern = 0xFFFF;
            return cnt;
        }
    } else
        pat = ICurPattern;
    IDecodePattern = pat;
    IDecodeRow = 0;
    INumbered = 1;
    return cnt;
}

/* DisplayTrackData: walk the before/current/after row spans through
 * `show`. Returns 0 when not playing (the original pops the caller's
 * return address, skipping the view's hilight bar as well). */
static int info_track_data(const dispwin_t *w,
                           void (*show)(const dispwin_t *, int))
{
    int y = w->topline + 2, pass, i, cnt;
    int budget = (int)w->length - 3;    /* port safety: the original
                                           draws past the window bottom
                                           for degenerate len<4 windows */

    if (IPlayMode == 0)
        return 0;

    for (pass = 0; pass < 3; pass++) {
        cnt = (pass == 0) ? info_before_rows(w)
            : (pass == 1) ? info_current_rows(w)
                          : info_after_rows(w);
        if (cnt > budget)
            cnt = budget;
        budget -= cnt;
        for (i = 0; i < cnt; i++, y++) {
            if (INumbered)              /* PE_ConvAX2Num, attr 20h */
                drawf(1, y, 0x20, "%03d", IDecodeRow % 1000);
            info_load_row();
            show(w, y);
        }
    }
    return 1;
}

/* DrawHilightBar: hilight the centre (playing) row */
static void info_hilight(const dispwin_t *w, int cells)
{
    int y = w->topline + ((int)w->length >> 1), i;
    for (i = 0; i < cells; i++)
        Screen_OrAttr(5 + i, y, 0xE0);
}

/* GetChannelColour */
static uint8_t info_chan_colour(int chan)
{
    if (Song.Header.ChnlPan[chan] & 0x80)
        return (chan == InfoCurChannel) ? 0x16 : 0x11;
    if (chan == InfoCurChannel)
        return 0x13;
    return (InfoProcWindow == InfoCurWindow) ? 0x12 : 0x10;
}

/* DrawChannelNumbers: 2-digit gutter at x=2. Current channel 23h (26h
 * when muted); other muted channels get NO number at all; otherwise
 * 21h (20h when this window isn't the focused one). */
static void info_channel_numbers(const dispwin_t *w, int rows, int ytop)
{
    int r;
    for (r = 0; r < rows; r++) {
        int c = w->topchan + r;
        int muted;
        uint8_t a;
        if (c > 63)
            break;                      /* port safety */
        muted = Song.Header.ChnlPan[c] & 0x80;
        if (c == InfoCurChannel)
            a = muted ? 0x26 : 0x23;
        else if (muted)
            continue;
        else
            a = (InfoProcWindow == InfoCurWindow) ? 0x21 : 0x20;
        drawf(2, ytop + r, a, "%02d", c + 1);
    }
}

/* one 3-char note cell: "C-5", 255 -> three 205 (note off fill),
 * 254 -> "^^^" (cut), anything else >119 -> three dots (char 173) */
static void info_note3(int x, int y, uint8_t note)
{
    if (note > 119) {
        uint8_t ch = (note == 0xFF) ? 205 : (note == 0xFE) ? '^' : 173;
        Screen_PutChar(x,     y, ch, 6);
        Screen_PutChar(x + 1, y, ch, 6);
        Screen_PutChar(x + 2, y, ch, 6);
    } else {
        int sem = note % 12, oct = note / 12;
        Screen_PutChar(x,     y, (uint8_t)NoteNameChars[sem * 2],     6);
        Screen_PutChar(x + 1, y, (uint8_t)NoteNameChars[sem * 2 + 1], 6);
        Screen_PutChar(x + 2, y, (uint8_t)('0' + oct), 6);
    }
}

/* one 2-char volume-column cell. va/pa = attrs for plain volume / pan
 * values, ea = attr for effect letter+digit (A0.., pan flag +60). */
static void info_vol2(int x, int y, uint8_t v, uint8_t va, uint8_t pa,
                      uint8_t ea)
{
    uint8_t t = (uint8_t)(v & 0x7F);

    if (t >= 65) {                      /* volume-column effect */
        t = (uint8_t)(t - 65);
        if (v & 0x80)
            t = (uint8_t)(t + 60);
        Screen_PutChar(x,     y, (uint8_t)('A' + t / 10), ea);
        Screen_PutChar(x + 1, y, (uint8_t)('0' + t % 10), ea);
    } else {
        uint8_t a = (v & 0x80) ? pa : va;
        Screen_PutChar(x,     y, (uint8_t)('0' + t / 10), a);
        Screen_PutChar(x + 1, y, (uint8_t)('0' + t % 10), a);
    }
}

/* Draw2Num / Draw3Num / Draw10Num (Details columns, attr 2). The tens/
 * hundreds characters can exceed '9' for large values, as the original
 * simply adds '0' to each digit register. */
static void info_draw2(int x, int y, uint8_t v)
{
    Screen_PutChar(x,     y, (uint8_t)('0' + v / 10), 2);
    Screen_PutChar(x + 1, y, (uint8_t)('0' + v % 10), 2);
}

static void info_draw3(int x, int y, uint16_t v)
{
    if (v >= 2560)
        v = 0;                          /* Draw3Num: AH >= 10 -> 0 */
    Screen_PutChar(x,     y, (uint8_t)('0' + v / 100),      2);
    Screen_PutChar(x + 1, y, (uint8_t)('0' + (v / 10) % 10), 2);
    Screen_PutChar(x + 2, y, (uint8_t)('0' + v % 10),       2);
}

static void info_draw10(int xend, int y, uint32_t v)
{
    int x = xend;
    do {
        Screen_PutChar(x--, y, (uint8_t)('0' + v % 10), 2);
        v /= 10;
    } while (v);
}

/* ---- method 0: Display_HostChannel (track view) ------------------- */

/* velocity-bar amplitude: min/max scan over the sample span mixed since
 * the last frame (OldSampleOffset -> SampleOffset, loop-aware; 16-bit
 * samples scan the high bytes). Returns max-min (0..255). */
static int info_scan_amp(const slavechn_t *sc)
{
    int32_t beg = (int32_t)sc->OldSampleOffset;
    int32_t end = sc->SampleOffset;
    int32_t cnt, i;
    const sample_t *s;
    const uint8_t *p;
    int step;
    int8_t mn, mx;

    if (beg < 0) beg = 0;
    if (end < 0) end = 0;
    if (sc->LpM >= 8) {
        if (sc->LpM == 8) {             /* forwards loop: wrap to end  */
            if (end < beg)
                end = sc->LoopEnd;
        } else {                        /* ping pong: |span|           */
            if (end <= beg) { int32_t t = beg; beg = end; end = t; }
        }
    }
    cnt = end - beg;
    if (cnt <= 0)
        return 0;
    if (sc->Smp >= MAX_SAMPLES)
        return 0;                       /* port safety */
    s = &Song.Smp[sc->Smp];
    if (!s->Data || beg >= (int32_t)s->Length)
        return 0;                       /* port safety: the original
                                           scans raw DOS memory here */
    if (cnt > (int32_t)s->Length - beg)
        cnt = (int32_t)s->Length - beg;

    if (sc->Bit & 2) {                  /* 16 bit: high bytes (Or ESI,1) */
        p = (const uint8_t *)s->Data + (size_t)beg * 2 + 1;
        step = 2;
    } else {
        p = (const uint8_t *)s->Data + beg;
        step = 1;
    }
    mn = mx = (int8_t)p[0];
    for (i = 0; i < cnt; i++, p += step) {
        int8_t v = (int8_t)*p;
        if (v > mx)                     /* Cmp DH,AL / JL  */
            mx = v;
        else if (v < mn)                /* Cmp DL,AL / JG  */
            mn = v;
    }
    return (uint8_t)(mx - mn);
}

static void view_hostchannel(dispwin_t *w)
{
    int stereo  = (Song.Header.Flags & ITF_STEREO) != 0;
    int insmode = (Song.Header.Flags & ITF_INSTRUMENTS) != 0;
    int top = w->topline, bot = top + (int)w->length - 1;
    int tc, r, i;

    Screen_DrawBox(4, top, 29, bot, 27);
    Screen_DrawBox(30, top, 62, bot, 27);
    if (stereo)
        Screen_DrawBox(63, top, 73, bot, 27);

    tc = w->topchan;                    /* Display_HostChannel1/2/22 */
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel + 3 - (int)w->length)
        tc = InfoCurChannel + 3 - (int)w->length;
    if (tc > 66 - (int)w->length)
        tc = 65 - (int)w->length;
    w->topchan = (uint8_t)tc;

    info_channel_numbers(w, (int)w->length - 2, top + 1);

    for (r = 0; r < (int)w->length - 2; r++) {
        int c = tc + r, y = top + 1 + r, x;
        hostchn_t  *hc;
        slavechn_t *sc;
        uint8_t a;

        if (c > 63)
            break;                      /* port safety */
        hc = &HChn[c];
        if (!(hc->Flags & HF_CHAN_ON))
            continue;
        sc = &SChn[hc->SCOffst];

        /* sample number ("--" from 100 up), "/ii" in instrument mode */
        x = 31;
        if (sc->Smp + 1 <= 99) {
            Screen_PutChar(x,     y, (uint8_t)('0' + (sc->Smp + 1) / 10), 6);
            Screen_PutChar(x + 1, y, (uint8_t)('0' + (sc->Smp + 1) % 10), 6);
        } else {
            Screen_PutChar(x,     y, '-', 6);
            Screen_PutChar(x + 1, y, '-', 6);
        }
        x += 2;
        if (insmode && sc->Ins != 0xFF) {
            Screen_PutChar(x,     y, '/', 6);
            Screen_PutChar(x + 1, y, (uint8_t)('0' + sc->Ins / 10), 6);
            Screen_PutChar(x + 2, y, (uint8_t)('0' + sc->Ins % 10), 6);
            x += 3;
        }
        /* ':' attr: 6 normally, 7 after note-off, 4 when silent */
        a = (sc->Flags & SF_NOTE_OFF) ? 7 : 6;
        if (sc->FV == 0)
            a = 4;
        Screen_PutChar(x, y, ':', a);
        x++;
        /* 25-char sample/instrument name; chars >= 226 blanked (they
         * are the redefined small-number glyphs -- "AvoidMouse") */
        {
            const char *nm = (sc->Smp < MAX_SAMPLES)
                             ? Song.Smp[sc->Smp].SampleName : "";
            if (insmode && InfoInstNames &&
                sc->InsOffs >= 1 && sc->InsOffs <= MAX_INSTRUMENTS)
                nm = Song.Ins[sc->InsOffs - 1].InstrumentName;
            for (i = 0; i < 25; i++) {
                uint8_t ch = (uint8_t)nm[i];
                if (ch >= 226)
                    ch = ' ';
                Screen_PutChar(x + i, y, ch, 6);
                if (!nm[i]) {           /* port safety: stop at NUL,   */
                    for (i++; i < 25; i++)      /* pad with real blanks */
                        Screen_PutChar(x + i, y, 0, 6);
                    break;
                }
            }
        }

        /* panning (stereo box): words or the 2-cell fractional thumb */
        if (stereo) {
            uint8_t p = sc->FP;
            if (p == 100)
                Screen_DrawString(64, y, "Surround ", 2);
            else if (p == 0)
                Screen_DrawString(64, y, "Left     ", 2);
            else if (p == 64)
                Screen_DrawString(64, y, "    Right", 2);
            else if (p < 64) {          /* thumb; 1..63 keeps it in box */
                int v = p + 1;
                Screen_PutChar(64 + (v >> 3), y,
                               (uint8_t)(155 + (v & 7)), 2);
                if (155 + (v & 7) > 157)
                    Screen_PutChar(64 + (v >> 3) + 1, y,
                                   (uint8_t)(155 + (v & 7) + 5), 2);
            }
            /* port safety: out-of-range pan (>64, not surround) draws
             * nothing rather than a thumb painted outside the box. The
             * engine guarantees FP in {0..64,100}; a value outside that
             * is corrupt data, which the original would mis-render. */
        }

        /* velocity / volume bar at x=5.. (24 cells max):
         * value = (amp * FV) >> 9 rounded, amp = 255 in volume-bar mode
         * (and for MIDI sample 100); bar = groups of 176/179/182 plus a
         * 173+n fractional tip. attr 5, 1 when the channel is muted. */
        {
            int amp = 255;
            int v, bar, full, rem, bx;
            uint8_t ba = (sc->Flags & SF_CHN_MUTED) ? 1 : 5;

            if (InfoVelocity == 0 && sc->Smp != 100)
                amp = info_scan_amp(sc);
            v    = amp * sc->FV;
            bar  = (v >> 9) + ((v >> 8) & 1);
            full = bar >> 3;
            rem  = bar & 7;
            bx   = 5;
            for (i = 0; i < full; i++) {
                Screen_PutChar(bx++, y, 176, ba);
                Screen_PutChar(bx++, y, 179, ba);
                Screen_PutChar(bx++, y, 182, ba);
            }
            if (rem) {
                if (rem > 3)
                    Screen_PutChar(bx++, y, 176, ba);
                if (rem > 5) {
                    Screen_PutChar(bx++, y, 179, ba);
                    rem++;
                }
                Screen_PutChar(bx, y, (uint8_t)(173 + rem), ba);
            }
        }
    }
}

/* ---- methods 1..7: the pattern views ------------------------------ */

static void show_5channel(const dispwin_t *w, int y)
{
    int i, x = 5;
    for (i = 0; i < 5; i++, x += 14) {
        const uint8_t *d = InfoRow[w->topchan + i];
        info_note3(x, y, d[0]);
        if (d[1]) {
            Screen_PutChar(x + 4, y, (uint8_t)('0' + d[1] / 10), 6);
            Screen_PutChar(x + 5, y, (uint8_t)('0' + d[1] % 10), 6);
        } else {
            Screen_PutChar(x + 4, y, 173, 6);
            Screen_PutChar(x + 5, y, 173, 6);
        }
        if (d[2] != 0xFF)
            info_vol2(x + 7, y, d[2], 6, 2, 6);
        else {
            Screen_PutChar(x + 7, y, 173, 6);
            Screen_PutChar(x + 8, y, 173, 6);
        }
        Screen_PutChar(x + 10, y, d[3] ? (uint8_t)('@' + d[3]) : '.', 6);
        drawf(x + 11, y, 6, "%02X", d[4]);
        if (i < 4)
            Screen_PutChar(x + 13, y, 168, 2);
    }
}

static void view_5channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 74, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 4) tc = InfoCurChannel - 4;
    if (tc >= 59) tc = 59;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 5; i++)
        drawf(5 + 14 * i, top + 1, info_chan_colour(tc + i),
              " Channel %02d ", tc + i + 1);
    if (info_track_data(w, show_5channel))
        info_hilight(w, 69);
}

static void show_8channel(const dispwin_t *w, int y)
{
    int i, x = 5;
    for (i = 0; i < 8; i++, x += 9) {
        const uint8_t *d = InfoRow[w->topchan + i];
        info_note3(x, y, d[0]);
        if (d[2] != 0xFF)
            info_vol2(x + 3, y, d[2], 2, 1, 2);
        Screen_PutChar(x + 5, y, d[3] ? (uint8_t)('@' + d[3]) : '.', 6);
        drawf(x + 6, y, 6, "%02X", d[4]);
        if (i < 7)
            Screen_PutChar(x + 8, y, 168, 2);
    }
}

static void view_8channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 76, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 7) tc = InfoCurChannel - 7;
    if (tc >= 56) tc = 56;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 8; i++)
        drawf(6 + 9 * i, top + 1, info_chan_colour(tc + i),
              "  %02d  ", tc + i + 1);
    if (info_track_data(w, show_8channel))
        info_hilight(w, 71);
}

static void show_10channel(const dispwin_t *w, int y)
{
    int i, x = 5;
    for (i = 0; i < 10; i++, x += 7) {
        const uint8_t *d = InfoRow[w->topchan + i];
        info_note3(x, y, d[0]);
        if (d[1])                       /* instrument as font-B pair    */
            Screen_PutChar(x + 3, y,
                           (uint8_t)((d[1] / 10) << 4 | (d[1] % 10)), 0x0A);
        else
            Screen_PutChar(x + 3, y, 184, 0x02);
        if (d[2] != 0xFF) {
            uint8_t t = (uint8_t)(d[2] & 0x7F);
            if (t >= 65) {
                t = (uint8_t)(t - 65);
                if (d[2] & 0x80)        /* pan effect: G0..H9 glyph     */
                    Screen_PutChar(x + 4, y, (uint8_t)(226 + t), 6);
                else                    /* vol effect: hex-letter pair  */
                    Screen_PutChar(x + 4, y,
                                   (uint8_t)((10 + t / 10) << 4 | (t % 10)),
                                   0x0C);
            } else
                Screen_PutChar(x + 4, y,
                               (uint8_t)((t / 10) << 4 | (t % 10)),
                               (d[2] & 0x80) ? 0x09 : 0x0C);
        } else
            Screen_PutChar(x + 4, y, 184, 0x06);
        Screen_PutChar(x + 5, y, d[3] ? (uint8_t)('@' + d[3]) : '.', 2);
        Screen_PutChar(x + 6, y, d[4], 0x0A);   /* value: hex pair     */
    }
}

static void view_10channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 75, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 9) tc = InfoCurChannel - 9;
    if (tc >= 54) tc = 54;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 10; i++)
        drawf(5 + 7 * i, top + 1, info_chan_colour(tc + i),
              "  %02d  ", tc + i + 1);
    if (info_track_data(w, show_10channel))
        info_hilight(w, 70);
}

/* Process3CharacterRow: one 3-cell cell for the 18/24-channel views;
 * priority note > instrument > volume > command. */
static void info_cell3(int x, int y, const uint8_t *d)
{
    if (d[0] != 0xFD || d[1]) {
        if (d[0] != 0xFD)
            info_note3(x, y, d[0]);
        else {                          /* instrument, right 2 cells    */
            Screen_PutChar(x + 1, y, (uint8_t)('0' + d[1] / 10), 6);
            Screen_PutChar(x + 2, y, (uint8_t)('0' + d[1] % 10), 6);
        }
    } else if (d[2] != 0xFF)
        info_vol2(x + 1, y, d[2], 2, 1, 2);
    else if (d[3]) {
        Screen_PutChar(x, y, (uint8_t)('@' + d[3]), 2);
        drawf(x + 1, y, 2, "%02X", d[4]);
    } else {
        Screen_PutChar(x,     y, 173, 6);
        Screen_PutChar(x + 1, y, 173, 6);
        Screen_PutChar(x + 2, y, 173, 6);
    }
}

static void show_18channel(const dispwin_t *w, int y)
{
    int i, x = 5;
    for (i = 0; i < 18; i++, x += 4) {
        info_cell3(x, y, InfoRow[w->topchan + i]);
        if (i < 17)
            Screen_PutChar(x + 3, y, 168, 2);
    }
}

static void view_18channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 76, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 17) tc = InfoCurChannel - 17;
    if (tc >= 46) tc = 46;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 18; i++)
        drawf(6 + 4 * i, top + 1, info_chan_colour(tc + i),
              "%02d", tc + i + 1);
    if (info_track_data(w, show_18channel))
        info_hilight(w, 71);
}

static void show_24channel(const dispwin_t *w, int y)
{
    int i;
    for (i = 0; i < 24; i++)
        info_cell3(5 + 3 * i, y, InfoRow[w->topchan + i]);
}

static void view_24channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 77, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 23) tc = InfoCurChannel - 23;
    if (tc >= 40) tc = 40;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 24; i++)
        drawf(6 + 3 * i, top + 1, info_chan_colour(tc + i),
              "%02d", tc + i + 1);
    if (info_track_data(w, show_24channel))
        info_hilight(w, 72);
}

static void show_36channel(const dispwin_t *w, int y)
{
    int i, x = 5;
    for (i = 0; i < 36; i++, x += 2) {
        const uint8_t *d = InfoRow[w->topchan + i];
        if (d[0] != 0xFD || d[1]) {
            if (d[0] != 0xFD) {
                if (d[0] > 119) {
                    uint8_t ch = (d[0] == 0xFF) ? 205
                               : (d[0] == 0xFE) ? '^' : 173;
                    Screen_PutChar(x,     y, ch, 6);
                    Screen_PutChar(x + 1, y, ch, 6);
                } else {
                    Screen_PutChar(x, y,
                                   (uint8_t)Note2Table[d[0] % 12], 6);
                    Screen_PutChar(x + 1, y,
                                   (uint8_t)('0' + d[0] / 12), 6);
                }
            } else {
                Screen_PutChar(x,     y, (uint8_t)('0' + d[1] / 10), 6);
                Screen_PutChar(x + 1, y, (uint8_t)('0' + d[1] % 10), 6);
            }
        } else if (d[2] != 0xFF)
            info_vol2(x, y, d[2], 2, 1, 2);
        else if (d[3]) {
            Screen_PutChar(x,     y, (uint8_t)('@' + d[3]), 2);
            Screen_PutChar(x + 1, y, d[4], 0x0A);   /* value hex pair  */
        } else {
            Screen_PutChar(x,     y, 173, 6);
            Screen_PutChar(x + 1, y, 173, 6);
        }
    }
}

static void view_36channel(dispwin_t *w)
{
    int top = w->topline, tc, i;

    Screen_DrawBox(4, top + 1, 77, top + (int)w->length - 1, 27);
    tc = w->topchan;
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel - 35) tc = InfoCurChannel - 35;
    if (tc >= 28) tc = 28;
    w->topchan = (uint8_t)tc;
    for (i = 0; i < 36; i++)
        drawf(5 + 2 * i, top + 1, info_chan_colour(tc + i),
              "%02d", tc + i + 1);
    if (info_track_data(w, show_36channel))
        info_hilight(w, 72);
}

static void show_64channel(const dispwin_t *w, int y)
{
    int i;
    (void)w;
    for (i = 0; i < 64; i++) {
        const uint8_t *d = InfoRow[i];
        int x = 5 + i;
        if (d[0] != 0xFD || d[1]) {
            if (d[0] != 0xFD) {
                if (d[0] > 119)
                    Screen_PutChar(x, y, (d[0] == 0xFF) ? 205
                                       : (d[0] == 0xFE) ? '^' : 173, 6);
                else
                    Screen_PutChar(x, y,
                                   (uint8_t)Note2Table[d[0] % 12], 6);
            } else
                Screen_PutChar(x, y,
                               (uint8_t)((d[1] / 10) << 4 | (d[1] % 10)),
                               0x0A);
        } else if (d[2] != 0xFF && (d[2] & 0x7F) <= 64) {
            uint8_t t = (uint8_t)(d[2] & 0x7F);
            Screen_PutChar(x, y, (uint8_t)((t / 10) << 4 | (t % 10)),
                           (d[2] & 0x80) ? 0x09 : 0x0C);
        } else
            Screen_PutChar(x, y, 173, 6);
    }
    for (i = 0; i < 9; i++)             /* 9-cell filler to x=77 */
        Screen_PutChar(69 + i, y, 173, 6);
}

static void view_64channel(dispwin_t *w)
{
    int top = w->topline, i;

    Screen_DrawBox(4, top + 1, 78, top + (int)w->length - 1, 27);
    /* channel numbers as font-B packed decimal pairs, colour | 8 */
    for (i = 0; i < 64; i++)
        Screen_PutChar(5 + i, top + 1,
                       (uint8_t)(((i + 1) / 10) << 4 | ((i + 1) % 10)),
                       (uint8_t)(info_chan_colour(i) | 8));
    for (i = 0; i < 9; i++)
        Screen_PutChar(69 + i, top + 1, 0, 0x10);
    w->topchan = 0;
    if (info_track_data(w, show_64channel))
        info_hilight(w, 73);
}

/* ---- method 8: Display_Variables ----------------------------------- */

static void view_variables(dispwin_t *w)
{
    int i, act = 0, used = 0;
    uint8_t a = (InfoProcWindow == InfoCurWindow) ? 0x23 : 0x20;

    for (i = 0; i < MAXSLAVECHANNELS; i++) {
        act += SChn[i].Flags & 1;
        if (SChn[i].HCOffst != 0xFFFF)  /* original: word [SI+38h] != 0 */
            used++;
    }
    drawf(2, w->topline + 1, a, "Active Channels: %d (%d)", act, used);
    drawf(2, w->topline + 2, a, "  Global Volume: %d", GlobalVolume);
}

/* ---- method 9: Display_NoteDots ------------------------------------ */

static void view_notedots(dispwin_t *w)
{
    int top = w->topline, tc, r, i;

    Screen_DrawBox(4, top, 78, top + (int)w->length - 1, 27);

    tc = w->topchan;                    /* Display_Dots1/2/3 clamp */
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel + 3 - (int)w->length)
        tc = InfoCurChannel + 3 - (int)w->length;
    if (tc > 66 - (int)w->length)
        tc = 65 - (int)w->length;
    w->topchan = (uint8_t)tc;

    info_channel_numbers(w, (int)w->length - 2, top + 1);

    for (r = 0; r < (int)w->length - 2; r++) {
        int c = tc + r, y = top + 1 + r;
        uint8_t row[73][2];

        if (c > 63)
            break;                      /* port safety */
        for (i = 0; i < 73; i++) {
            row[i][0] = 193;
            row[i][1] = 6;
        }
        for (i = 0; i < MAXSLAVECHANNELS; i++) {
            const slavechn_t *sc = &SChn[i];
            int col, v;
            uint8_t ch, a;
            if (!(sc->Flags & SF_CHAN_ON))
                continue;
            if ((sc->HCN & 0x7F) != c)
                continue;
            col = (int)sc->Nte - 30;    /* columns = notes 30..102 */
            if (col < 0 || col >= 73)
                continue;
            v  = sc->FV + 7;            /* dot size, rounded */
            ch = (uint8_t)(193 + (v >> 4) + ((v >> 3) & 1));
            a  = (uint8_t)(2 + ((sc->Smp == 100 ? sc->Ins
                                                : sc->Smp) & 3));
            if (sc->Flags & SF_CHN_MUTED)
                a = 1;
            if ((sc->HCN & 0x80) && row[col][0] > ch)
                continue;               /* disowned: keep larger dots */
            row[col][0] = ch;
            row[col][1] = a;
        }
        for (i = 0; i < 73; i++)
            Screen_PutChar(5 + i, y, row[i][0], row[i][1]);
    }
}

/* ---- method 10: Display_Details ------------------------------------ */

static void view_details(dispwin_t *w)
{
    static const int div9[9] = { 15, 26, 35, 38, 41, 44, 47, 51, 54 };
    int insmode = (Song.Header.Flags & ITF_INSTRUMENTS) != 0;
    int top = w->topline, bot = top + (int)w->length - 1;
    int tc, r, i;

    Screen_DrawBox(4,  top + 1, 30, bot, 27);
    Screen_DrawBox(31, top + 1, 57, bot, 27);
    if (insmode) {
        Screen_DrawBox(58, top + 1, 66, bot, 27);
        Screen_DrawString(59, top + 1, "NNA", 0x12);    /* VirtualMsg */
        Screen_PutChar(62, top + 1, 148, 0x21);
        Screen_DrawString(63, top + 1, "Tot", 0x12);
    }
    /* DetailsMsg header: text 12h, divider glyphs 148/152/153 21h */
    Screen_DrawString(6, top + 1, "Frequency", 0x12);
    Screen_PutChar(15, top + 1, 148, 0x21);
    Screen_PutChar(16, top + 1, 148, 0x21);
    Screen_DrawString(17, top + 1, "Position", 0x12);
    Screen_PutChar(25, top + 1, 148, 0x21);
    Screen_PutChar(26, top + 1, 148, 0x21);
    Screen_DrawString(27, top + 1, "Smp", 0x12);
    Screen_PutChar(30, top + 1, 152, 0x21);
    Screen_PutChar(31, top + 1, 153, 0x21);
    Screen_DrawString(32, top + 1, "FVl", 0x12);
    Screen_PutChar(35, top + 1, 148, 0x21);
    Screen_DrawString(36, top + 1, "Vl", 0x12);
    Screen_PutChar(38, top + 1, 148, 0x21);
    Screen_DrawString(39, top + 1, "CV", 0x12);
    Screen_PutChar(41, top + 1, 148, 0x21);
    Screen_DrawString(42, top + 1, "SV", 0x12);
    Screen_PutChar(44, top + 1, 148, 0x21);
    Screen_DrawString(45, top + 1, "VE", 0x12);
    Screen_PutChar(47, top + 1, 148, 0x21);
    Screen_DrawString(48, top + 1, "Fde", 0x12);
    Screen_PutChar(51, top + 1, 148, 0x21);
    Screen_DrawString(52, top + 1, "Pn", 0x12);
    Screen_PutChar(54, top + 1, 148, 0x21);
    Screen_DrawString(55, top + 1, "PE", 0x12);

    tc = w->topchan;                    /* Display_Details1/2/22 clamp */
    if (InfoCurChannel < tc) tc = InfoCurChannel;
    if (tc < InfoCurChannel + 4 - (int)w->length)
        tc = InfoCurChannel + 4 - (int)w->length;
    if (tc >= 67 - (int)w->length)
        tc = 67 - (int)w->length;
    w->topchan = (uint8_t)tc;

    info_channel_numbers(w, (int)w->length - 3, top + 2);

    for (r = 0; r < (int)w->length - 3; r++) {
        int c = tc + r, y = top + 2 + r;
        hostchn_t  *hc;
        slavechn_t *sc = NULL;

        if (c < 0 || c > 63)
            break;                      /* port safety */
        hc = &HChn[c];
        for (i = 0; i < 9; i++)
            Screen_PutChar(div9[i], y, 168, 2);

        if (hc->Flags & HF_CHAN_ON) {
            sc = &SChn[hc->SCOffst];
            info_draw10(14, y, (uint32_t)sc->Frequency);
            info_draw10(25, y, (uint32_t)sc->SampleOffset);
            info_draw3(27, y, (uint16_t)(sc->Smp + 1));
            info_draw3(32, y, sc->FV);
            info_draw2(36, y, sc->Vol);
            info_draw2(39, y, sc->CVl);
            info_draw2(42, y, (uint8_t)(sc->SVl >> 1));
            info_draw2(45, y, (uint8_t)(sc->VEnv.Value >> 16));
            info_draw3(48, y, (uint16_t)(sc->FadeOut >> 1));
            if (sc->FP == 100) {
                Screen_PutChar(52, y, 'S', 2);
                Screen_PutChar(53, y, 'u', 2);
            } else
                info_draw2(52, y, sc->FP);
            info_draw2(55, y, (uint8_t)((sc->PEnv.Value >> 16) + 32));
        }
        if (insmode) {
            Screen_PutChar(62, y, 168, 2);
            if (!(hc->Flags & HF_CHAN_ON)) {
                Screen_PutChar(59, y, '-', 2);
                Screen_PutChar(60, y, '-', 2);
                Screen_PutChar(61, y, '-', 2);
            } else
                Screen_DrawString(59, y, InfoNNAMsg[sc->NNA & 3], 2);
            {
                int n = 0;              /* virtual channels of this host */
                for (i = 0; i < MAXSLAVECHANNELS; i++)
                    if ((SChn[i].Flags & 1) &&
                        (SChn[i].HCN & 0x7F) == hc->HCN)
                        n++;
                info_draw3(63, y, (uint16_t)n);
            }
        }
    }
}

/* ---- DrawDisplayData: walk the windows ------------------------------ */

typedef void (*info_view_fn)(dispwin_t *);
static info_view_fn const InfoViews[11] = {
    view_hostchannel, view_5channel,  view_8channel,  view_10channel,
    view_18channel,   view_24channel, view_36channel, view_64channel,
    view_variables,   view_notedots,  view_details,
};

static void draw_info(void)
{
    int i;

    ed_lock();
    IPlayMode    = PlayMode;            /* Music_GetPlayMode snapshot */
    ICurRow      = CurrentRow;
    ICurPattern  = CurrentPattern;
    ICurOrder    = CurrentOrder;
    InfoPEMaxRow = CurRows;             /* PE_GetCurrentPattern        */
    if (ICurPattern == CurPattern)
        InfoPEMaxRow = NumberOfRows;    /* Music_GetPatternLength      */
    InfoGridPat = 0xFFFF;               /* patterns may have changed   */

    for (i = 0; i < InfoNumWindows; i++) {
        dispwin_t adj = InfoWin[i];
        InfoProcWindow = i;
        /* DrawDisplayData quirk: windows after the first (or any window
         * in fullscreen) grow one row upward -- unless the method draws
         * its own full frame (track view 0 / note dots 9) -- so their
         * box top border replaces the previous window's bottom one. */
        if ((InfoFullScreen || i > 0) &&
            adj.method != 0 && adj.method != 9) {
            adj.topline--;
            adj.length++;
        }
        InfoViews[adj.method](&adj);
        InfoWin[i].topchan = adj.topchan;   /* clamped value persists */
    }
    ed_unlock();
}

/* PostDisplayData / DisplayListKeys. Portable stand-ins for Alt-only
 * combos (no Alt modifier in the key layer yet, HANDOFF roadmap #5):
 * Ctrl-U/Ctrl-D = Alt-Up/Alt-Down (resize), 'r' = Alt-R (reverse),
 * 's' = Alt-S (stereo). Documented in the README fidelity notes. */
/* ===================================================================
 * Alt-F12 spectrum analyser (IT_FOUR.ASM, SPECTRUMANALYSER=1 in the
 * released SWITCH.INC; feature 013). Fourier_Transform is
 * transliterated (2048-point radix-2, float arithmetic -- the
 * original loads FPU control word 003Fh = 24-bit precision for it);
 * the display is the original's scrolling spectrogram over the 64-row
 * thresholded bar spectrum with the two switchable gradient palettes.
 * Deviation (README): rendered into the 640x400 overlay instead of a
 * VESA mode switch; terminal backend excluded.
 * =================================================================== */
#define FOUR_W 640
#define FOUR_H 400

static float FourRe[2048], FourIm[2048];
static uint8_t FourPix[FOUR_W * FOUR_H];
static uint8_t FourMag[1024];

static void play_song(void);
static void play_pattern(void);

/* Fourier_CreateTable: 11-bit bit reversal */
static uint16_t four_reloc(uint16_t c)
{
    uint16_t r = 0;
    int b;

    for (b = 0; b < 11; b++) {
        r = (uint16_t)((r << 1) | (c & 1));
        c >>= 1;
    }
    return r;
}

/* Fourier_Transform + the magnitude pass: mag8[k] =
 * min(255, lrint(|X[k]| / 128) >> 6) for bins 0..1023 */
static void fourier_fft(const int16_t *wave, uint8_t *mag8)
{
    int i, d, k;

    for (i = 0; i < 2048; i++) {
        FourRe[four_reloc((uint16_t)i)] = (float)wave[i];
        FourIm[four_reloc((uint16_t)i)] = 0.0f;
    }
    for (i = 1; i < 2048; i <<= 1) {
        float dr = (float)cos(-3.14159265358979323846 / i);
        float di = (float)sin(-3.14159265358979323846 / i);
        float cr = 1.0f, ci = 0.0f;

        for (d = 0; d < i; d++) {
            for (k = d; k < 2048; k += 2 * i) {
                float sr = FourRe[k + i], si = FourIm[k + i];
                float tr = cr * sr - ci * si;   /* temp = s * phase */
                float ti = cr * si + ci * sr;
                FourRe[k + i] = FourRe[k] - tr;
                FourIm[k + i] = FourIm[k] - ti;
                FourRe[k] += tr;
                FourIm[k] += ti;
            }
            {                           /* phase *= deltaphase */
                float nr = cr * dr - ci * di;
                float ni = cr * di + ci * dr;
                cr = nr;
                ci = ni;
            }
        }
    }
    for (k = 0; k < 1024; k++) {
        float m = sqrtf(FourRe[k] * FourRe[k] + FourIm[k] * FourIm[k])
                  * 0.0078125f;         /* Const1_2048 = 1/128 */
        long v = lrintf(m) >> 6;
        mag8[k] = (uint8_t)(v > 255 ? 255 : v);
    }
}

/* Fourier_SetPalette: the two gradient palettes, 6-bit DAC values */
static void fourier_palette(uint8_t *p, int sel)
{
    int i;

    if (sel) {                          /* palette A */
        for (i = 0; i < 64; i++) {
            p[i * 3] = 0; p[i * 3 + 1] = 0;
            p[i * 3 + 2] = (uint8_t)(i >> 1);
        }
        for (i = 0; i < 64; i++) {
            p[(64 + i) * 3] = 0;
            p[(64 + i) * 3 + 1] = (uint8_t)(i >> 1);
            p[(64 + i) * 3 + 2] = (uint8_t)((i >> 1) + 32);
        }
        for (i = 0; i < 128; i++) {
            p[(128 + i) * 3]     = (uint8_t)(i >> 1);
            p[(128 + i) * 3 + 1] = (uint8_t)((i >> 2) + 32);
            p[(128 + i) * 3 + 2] = 63;
        }
    } else {                            /* palette B (default) */
        for (i = 0; i < 32; i++) {
            p[i * 3] = 0; p[i * 3 + 1] = 0;
            p[i * 3 + 2] = (uint8_t)(i * 2);
        }
        for (i = 0; i < 32; i++) {
            p[(32 + i) * 3] = (uint8_t)(i * 2);
            p[(32 + i) * 3 + 1] = 0;
            p[(32 + i) * 3 + 2] = 63;
        }
        for (i = 0; i < 32; i++) {
            p[(64 + i) * 3] = 63;
            p[(64 + i) * 3 + 1] = 0;
            p[(64 + i) * 3 + 2] = (uint8_t)(63 - i * 2);
        }
        for (i = 0; i < 32; i++) {
            p[(96 + i) * 3] = 63;
            p[(96 + i) * 3 + 1] = (uint8_t)(i * 2);
            p[(96 + i) * 3 + 2] = 0;
        }
        for (i = 0; i < 128; i++) {
            p[(128 + i) * 3] = 63;
            p[(128 + i) * 3 + 1] = 63;
            p[(128 + i) * 3 + 2] = (uint8_t)(i >> 1);
        }
    }
}

/* Fourier_DrawScreen: one frame -- FFT the driver tap, plot one
 * spectrogram column at *xoff (bins H-64..1, low frequencies at the
 * bottom), redraw the 64-row bar spectrum (bins 1..W, lit where
 * magnitude > (63-row)*4). */
static void fourier_frame(int *xoff)
{
    static int16_t wave[2048];
    int r, i;

    WAVDriver_GetWaveForm(wave);
    fourier_fft(wave, FourMag);

    for (r = 0; r < FOUR_H - 64; r++)
        FourPix[r * FOUR_W + *xoff] = FourMag[FOUR_H - 64 - r];
    *xoff = (*xoff + 1) % FOUR_W;

    for (r = 0; r < 64; r++) {
        uint8_t bl = (uint8_t)((63 - r) << 2);
        uint8_t *row = FourPix + (FOUR_H - 64 + r) * FOUR_W;
        for (i = 0; i < FOUR_W; i++)
            row[i] = (FourMag[i + 1] > bl) ? 255 : 0;
    }
}

/* Fourier_Start / O1_FourierDisplay: the modal analyser view.
 * Keys per FourierKeyList: 'p' palette, +/- the F5 order keys,
 * F5/F6/F8 playback, ESC exits. */
static void fourier_view(void)
{
    static uint8_t fpal[768];
    int xoff = 0, palsel = 0;

    memset(FourPix, 0, sizeof(FourPix));
    fourier_palette(fpal, palsel);
    Screen_SetOverlay(FourPix, fpal);

    for (;;) {
        int key;

        fourier_frame(&xoff);
        Screen_Update();

        key = ed_get_key();
        switch (key) {
        case ITK_QUIT:
            Running = 0;
            key = ITK_ESC;
            break;
        case 'p':
            palsel ^= 1;
            fourier_palette(fpal, palsel);
            Screen_SetOverlay(FourPix, fpal);
            break;
        case '+':
            ed_lock(); Music_NextOrder(); ed_unlock();
            break;
        case '-':
            ed_lock(); Music_LastOrder(); ed_unlock();
            break;
        case ITK_F5: commit_current_pattern(); play_song();    break;
        case ITK_F6: commit_current_pattern(); play_pattern(); break;
        case ITK_F8: stop_song(); break;
        default:
            break;
        }
        if (key == ITK_ESC)
            break;
        ma_sleep(15);
    }

    Screen_SetOverlay(NULL, NULL);      /* Fourier_End */
    Screen_DefineSmallNumbers();
}

static void handle_info_key(int key)
{
    dispwin_t *w = &InfoWin[InfoCurWindow];

    /* Q/S/G/V/I are type-5 entries in DisplayListKeys: either case */
    if (key == 'q' || key == 's' || key == 'g' || key == 'v' || key == 'i')
        key -= 'a' - 'A';
    switch (key) {
    case ITK_UP: case ITK_LEFT:         /* DisplayUp */
        if (InfoCurChannel > 0) InfoCurChannel--;
        return;
    case ITK_DOWN: case ITK_RIGHT:      /* DisplayDown */
        if (InfoCurChannel < 63) InfoCurChannel++;
        return;
    case ITK_HOME:                      /* DisplayHome */
        InfoCurChannel = 0;
        return;
    case ITK_END:                       /* DisplayEnd */
        ed_lock();
        InfoCurChannel = Music_GetLastChannel();
        ed_unlock();
        return;
    case '+':                           /* DisplayPlus */
        ed_lock(); Music_NextOrder(); ed_unlock();
        return;
    case '-':                           /* DisplayMinus */
        ed_lock(); Music_LastOrder(); ed_unlock();
        return;
    case ITK_ALT_F12:                   /* Display_FourierStart */
        fourier_view();
        return;
    case ITK_PGUP:                      /* DisplayPageUp: method-1 mod 11 */
        w->method = (uint16_t)((w->method + 10) % 11);
        return;
    case ITK_PGDN:                      /* DisplayPageDown */
        w->method = (uint16_t)((w->method + 1) % 11);
        return;
    case ITK_TAB:                       /* DisplayNext (wraps) */
        InfoCurWindow = (InfoCurWindow + 1) % InfoNumWindows;
        return;
    case ITK_SHIFT_TAB:                 /* DisplayPrevious (saturates) */
        if (InfoCurWindow > 0) InfoCurWindow--;
        return;
    case ITK_INS: {                     /* DisplayInsert: split */
        int n, half;
        if (InfoNumWindows >= 5 || w->length <= 6)
            return;
        for (n = InfoNumWindows; n > InfoCurWindow; n--)
            InfoWin[n] = InfoWin[n - 1];
        half = ((int)w->length >> 1) + ((int)w->length & 1);
        InfoWin[InfoCurWindow + 1].length =
            (uint16_t)((int)w->length - half);
        InfoWin[InfoCurWindow + 1].topline =
            (uint8_t)(w->topline + half);
        w->length = (uint16_t)half;
        InfoNumWindows++;
        return;
    }
    case ITK_DEL: {                     /* DisplayDelete: merge */
        int n;
        uint16_t blen;
        if (InfoNumWindows <= 1)
            return;
        blen = w->length;
        for (n = InfoCurWindow; n < InfoNumWindows - 1; n++)
            InfoWin[n] = InfoWin[n + 1];
        InfoNumWindows--;
        if (InfoCurWindow >= InfoNumWindows) {
            InfoCurWindow--;            /* deleted last: grow previous */
            InfoWin[InfoCurWindow].length =
                (uint16_t)(InfoWin[InfoCurWindow].length + blen);
        } else {                        /* next window grows upward */
            InfoWin[InfoCurWindow].length =
                (uint16_t)(InfoWin[InfoCurWindow].length + blen);
            InfoWin[InfoCurWindow].topline =
                (uint8_t)(InfoWin[InfoCurWindow].topline - blen);
        }
        return;
    }
    case ITK_ALT_UP: {                  /* DisplayAltUp */
        int idx    = (InfoCurWindow == 0) ? 1 : InfoCurWindow + 1;
        int minlen = (InfoCurWindow == 0) ? 4 : 3;
        if (idx < InfoNumWindows && InfoWin[idx - 1].length > minlen) {
            InfoWin[idx].length++;
            InfoWin[idx - 1].length--;
            InfoWin[idx].topline--;
        }
        return;
    }
    case ITK_ALT_DOWN: {                /* DisplayAltDown */
        int idx = InfoCurWindow + 1;
        if (idx < InfoNumWindows && InfoWin[idx].length > 3) {
            InfoWin[idx - 1].length++;
            InfoWin[idx].length--;
            InfoWin[idx].topline++;
        }
        return;
    }
    case ITK_ALT_F9:                    /* DisplayToggleChannel */
    case 'Q':
        ed_lock();
        Music_ToggleChannel((uint16_t)InfoCurChannel);
        ed_unlock();
        return;
    case ITK_ALT_F10:                   /* DisplaySoloChannel */
    case 'S':
        ed_lock();
        Music_SoloChannel((uint16_t)InfoCurChannel);
        ed_unlock();
        return;
    case ' ':                           /* Display_SpaceBar: toggle+down */
        ed_lock();
        Music_ToggleChannel((uint16_t)InfoCurChannel);
        ed_unlock();
        if (InfoCurChannel < 63) InfoCurChannel++;
        return;
    case ITK_ALT_A + ('R' - 'A'):       /* DisplayToggleReverse */
        ed_lock(); Music_ToggleReverse(); ed_unlock();
        /* the original engine SetInfoLines this itself (same text in
         * both directions); the port keeps the engine UI-free */
        status("Left/right outputs reversed");
        return;
    case ITK_ALT_A + ('S' - 'A'):       /* DisplayToggleStereo */
        ed_lock();
        Song.Header.Flags ^= ITF_STEREO;
        Music_InitStereo();
        ed_unlock();
        status((Song.Header.Flags & ITF_STEREO) ? "Stereo Enabled"
                                                : "Stereo Disabled");
        return;
    case 'V':                           /* DisplayToggleVelocity */
        InfoVelocity ^= 1;
        status(InfoVelocity ? "Using velocity bars" : "Using volume bars");
        return;
    case 'I':                           /* DisplayToggleInstrument */
        InfoInstNames ^= 1;
        status(InfoInstNames ? "Using Instrument names"
                             : "Using Sample names");
        return;
    case 'G': {                         /* Display_GotoPattern */
        uint16_t pat, row;
        int playing;
        ed_lock();
        playing = (PlayMode != 0);
        pat = CurrentPattern;
        row = CurrentRow;
        ed_unlock();
        if (!playing)
            return;
        commit_current_pattern();
        load_pattern(pat);
        CurRow  = (row < CurRows) ? row : 0;
        CurChan = InfoCurChannel;
        Screen  = SCR_PATTERN;
        return;
    }
    case 0x06:                          /* Ctrl-F = Display_FullScreen */
        if (InfoNumWindows != 1)
            return;                     /* only a single window */
        InfoFullScreen ^= 1;
        if (InfoFullScreen) {
            InfoWin[0].topline = 1;
            InfoWin[0].length  = 49;
        } else {
            InfoWin[0].topline = 12;
            InfoWin[0].length  = 38;
        }
        return;
    default:
        break;
    }
}

/* ===================================================================
 * Message editor (Shift-F9) -- IT_MSG.ASM ported 1:1. 8000-byte
 * CR-separated NUL-terminated buffer (IT_MessageData, owned by
 * it_save.c so the loader/writer see it), view + edit modes, 35
 * visible lines at (2,13..47) inside box (1,12)-(78,48), word wrap at
 * column 75. Ctrl-T toggles the character colour 12 <-> 6 (the
 * original also swaps in the hi-ASCII charset via S_DefineHIASCII;
 * only the colour is ported -- README fidelity note).
 * =================================================================== */
static int MsgTopLine;                  /* TopLine                     */
static int MsgPos;                      /* CurrentPosition             */
static int MsgEdit;                     /* Edit                        */
static int MsgHoriz, MsgLine;           /* HorizontalPosition/Line     */
static uint8_t MsgColour = 12;          /* CharacterColour             */

static void msg_reset(void)             /* Msg_ResetMessage */
{
    memset(IT_MessageData, 0, IT_MESSAGELENGTH);
    MsgEdit = 0;
    MsgTopLine = 0;
    MsgPos = 0;
}

static int msg_find_start(int si)       /* FindStart */
{
    while (--si >= 0)
        if (IT_MessageData[si] == 13)
            break;
    return si + 1;
}

static int msg_insert(int si, int len)  /* InsertData; 0 = buffer full */
{
    if (IT_MessageData[IT_MESSAGELENGTH - 2 - len] != 0) {
        status("Message too long!");    /* O1_LongMessageList */
        return 0;
    }
    memmove(IT_MessageData + si + len, IT_MessageData + si,
            (size_t)(IT_MESSAGELENGTH - 1 - si - len));
    return 1;
}

static void msg_delete(int si, int len) /* DeleteData */
{
    if (IT_MESSAGELENGTH - si - len < 0)
        return;
    memmove(IT_MessageData + si, IT_MessageData + si + len,
            (size_t)(IT_MESSAGELENGTH - si - len));
    memset(IT_MessageData + IT_MESSAGELENGTH - len, 0, (size_t)len);
}

static void msg_wordwrap(void)          /* CheckWordWrap */
{
    int si = msg_find_start(MsgPos);
    int bx = 0;

    for (;;) {
        char c = IT_MessageData[si + bx];
        bx++;
        if (c == 0 || c == 13)
            break;
    }
    if (bx <= 75)
        return;
    for (bx = 75 - 1; bx > 0; bx--)     /* replace the last space */
        if (IT_MessageData[si + bx] == 32) {
            IT_MessageData[si + bx] = 13;
            return;
        }
    if (msg_insert(si + 75, 1))         /* no space: insert a CR */
        IT_MessageData[si + 75] = 13;
}

static void msg_left(void)
{
    if (MsgPos > 0)
        MsgPos--;
}

static void msg_right(void)
{
    if (MsgPos < IT_MESSAGELENGTH - 2 &&
        (IT_MessageData[MsgPos] || IT_MessageData[MsgPos + 1]))
        MsgPos++;
}

static void msg_up(void)                /* Msg_EditMsgUp */
{
    int si = msg_find_start(MsgPos);
    int cx;

    if (si == 0)
        return;
    si = msg_find_start(si - 1);
    for (cx = MsgHoriz; cx > 0; cx--) {
        if (IT_MessageData[si] == 13)
            break;
        si++;
    }
    MsgPos = si;
}

static void msg_down(void)              /* Msg_EditMsgDown */
{
    int si = MsgPos, cx;

    for (;;) {
        char c = IT_MessageData[si];
        si++;
        if (c == 0)
            return;
        if (c == 13)
            break;
    }
    for (cx = MsgHoriz; cx > 0; cx--) {
        char c = IT_MessageData[si];
        if (c == 0 || c == 13)
            break;
        si++;
    }
    if (si < IT_MESSAGELENGTH - 2)
        MsgPos = si;
}

static void msg_delete_key(void)
{
    msg_delete(MsgPos, 1);
    msg_wordwrap();
}

static void msg_insert_char(int ch)     /* Msg_PostMessage4 */
{
    if (!msg_insert(MsgPos, 1))
        return;
    IT_MessageData[MsgPos] = (char)ch;
    msg_wordwrap();
    msg_right();
}

static void draw_message(void)          /* Msg_DrawMessage + box */
{
    const char *m = IT_MessageData;
    int si = 0, i, y;

    Screen_DrawBox(1, 12, 78, 48, 27);

    if (MsgEdit) {
        int line = 0, col = 0;
        for (i = 0; i < MsgPos && m[i]; i++) {
            col++;
            if (m[i] == 13) {
                line++;
                col = 0;
            }
        }
        MsgLine = line;
        MsgHoriz = col;
        if (MsgTopLine > line)
            MsgTopLine = line;
        if (MsgTopLine + 34 < line)
            MsgTopLine = line - 34;
        if (MsgTopLine < 0)
            MsgTopLine = 0;
    }

    for (i = MsgTopLine; i > 0; ) {     /* skip TopLine lines */
        char c = m[si];
        if (c == 0)
            break;
        si++;
        if (c == 13)
            i--;
    }

    for (y = 0; y < 35; y++) {
        int x = 2;
        for (;;) {
            char c = m[si];
            if (c == 0) {               /* end marker, attr + 1 */
                Screen_PutChar(x, 13 + y,
                               MsgEdit ? 20 : 0, MsgEdit ? 2 : 4);
                goto done;
            }
            si++;
            if (c == 13) {              /* line end marker */
                Screen_PutChar(x, 13 + y,
                               MsgEdit ? 20 : 0, MsgEdit ? 1 : 3);
                break;
            }
            if (x <= 77) {              /* port safety: clip long lines */
                if (c == ' ')
                    Screen_PutChar(x, 13 + y, ' ', 3);
                else
                    Screen_PutChar(x, 13 + y, (uint8_t)c, MsgColour);
            }
            x++;
        }
    }
done:
    if (MsgEdit) {                      /* Msg_PreMessage cursor:
                                           attr = (attr & 8) | 30h    */
        int cy = 13 + MsgLine - MsgTopLine;
        int cx = 2 + MsgHoriz;
        if (cy >= 13 && cy <= 47 && cx <= 77)
            Screen_SetAttr(cx, cy,
                           (uint8_t)((Screen_GetAttr(cx, cy) & 8)
                                     | 0x30));
    }
}

static void handle_message_key(int key)
{
    if (!MsgEdit) {                     /* NoEditKeys */
        switch (key) {
        case ITK_UP:
            if (MsgTopLine > 0) MsgTopLine--;
            break;
        case ITK_DOWN:
            if (++MsgTopLine > 7970) MsgTopLine = 7970;
            break;
        case ITK_PGUP:
            MsgTopLine -= 35;
            if (MsgTopLine < 0) MsgTopLine = 0;
            break;
        case ITK_PGDN:
            MsgTopLine += 35;
            if (MsgTopLine > 7970) MsgTopLine = 7970;
            break;
        case ITK_ENTER:                 /* Msg_ViewMsgEdit */
            MsgTopLine = 0;
            MsgLine = 0;
            MsgPos = 0;
            MsgEdit = 1;
            break;
        case 0x14:                      /* Ctrl-T: colour toggle */
            MsgColour ^= 6 ^ 12;
            break;
        default:
            break;
        }
        return;
    }
    switch (key) {                      /* EditMsgKeys */
    case ITK_HOME:
        MsgPos = msg_find_start(MsgPos);
        break;
    case ITK_END: {                     /* Msg_EditMsgEnd */
        int si = MsgPos;
        while (IT_MessageData[si] && IT_MessageData[si] != 13)
            si++;
        MsgPos = si;
        break;
    }
    case ITK_LEFT:  msg_left();  break;
    case ITK_RIGHT: msg_right(); break;
    case ITK_UP:    msg_up();    break;
    case ITK_DOWN:  msg_down();  break;
    case ITK_PGUP: {
        int i;
        for (i = 0; i < 35; i++) msg_up();
        break;
    }
    case ITK_PGDN: {
        int i;
        for (i = 0; i < 35; i++) msg_down();
        break;
    }
    case ITK_INS:                       /* insert a space */
        if (msg_insert(MsgPos, 1)) {
            IT_MessageData[MsgPos] = 32;
            msg_wordwrap();
        }
        break;
    case ITK_TAB: {                     /* Msg_Tab: 8 spaces */
        int i;
        for (i = 0; i < 8; i++)
            msg_insert_char(32);
        break;
    }
    case ITK_DEL:
        msg_delete_key();
        break;
    case ITK_ESC:                       /* Msg_EditMsgView */
        MsgEdit = 0;
        break;
    case ITK_BACKSPACE:
        if (MsgPos > 0) {
            msg_left();
            msg_delete_key();
        }
        break;
    case 0x19: {                        /* Ctrl-Y: delete line */
        int si = msg_find_start(MsgPos);
        int bx = 0;
        for (;;) {
            char c = IT_MessageData[si + bx];
            bx++;
            if (c == 0 || c == 13)
                break;
        }
        msg_delete(si, bx);
        MsgPos = si;
        break;
    }
    case ITK_ALT_A + ('C' - 'A'):       /* Msg_ClearMessage (EditMsgKeys) */
        msg_reset();
        MsgEdit = 1;
        status("Message cleared");
        break;
    case 0x14:                          /* Ctrl-T */
        MsgColour ^= 6 ^ 12;
        break;
    case ITK_ENTER:
        msg_insert_char(13);
        break;
    default:
        if (key >= 32 && key < 256)
            msg_insert_char(key);
        break;
    }
}

/* ===================================================================
 * Rendering dispatch. draw_screen fills the cell buffer (and rebuilds
 * the active screen's widget table); redraw additionally presents it.
 * Modal overlays (menus) call draw_screen, draw on top, then present
 * once -- presenting twice per frame is what flickered.
 * =================================================================== */
static void draw_driver(void);              /* Shift-F5 */

static void draw_screen(void)
{
    static const char *titles[] = {
        "Help", "Pattern Editor (F2)", "Sample List (F3)",
        "Instrument List (F4)", "Order List and Panning (F11)",
        "Song Variables & Directory Configuration (F12)",
        "Info Page (F5)", "Message Editor (Shift-F9)",   /* DisplayHeader,
                                                        IT_OBJ1.ASM 6580 */
        "Keyboard (Ctrl-F1)", "Miniaudio Driver (Shift-F5)",
        "",                             /* O1_EmptyList: NoText */
    };

    Screen_Clear(0x20);
    if (Screen == SCR_INFO && InfoFullScreen) {
        /* Display_FullScreen mode 200: no standard chrome, only the
         * display data over rows 1..49 */
        NW = 0;
        draw_info();
        return;
    }
    /* the F3/F4 list cursor is LastInstrument in the original */
    if (!FileMode && (Screen == SCR_SAMPLES || Screen == SCR_INSTRUMENTS)
        && ListSel >= 0)
        CurInstr = ListSel + 1;
    draw_chrome(titles[Screen]);
    switch (Screen) {
    case SCR_PATTERN:     NW = 0; draw_pattern(); break;
    case SCR_SAMPLES:     draw_samples(); break;
    case SCR_INSTRUMENTS: draw_instruments(); break;
    case SCR_ORDER:       draw_order(); break;
    case SCR_VARS:        draw_vars(); break;
    case SCR_HELP:        draw_help(); break;
    case SCR_KEYS:        NW = 0; draw_keys(); break;
    case SCR_DRIVER:      draw_driver(); break;
    case SCR_EMPTY:       NW = 0; break;
    case SCR_INFO:        NW = 0; draw_info(); break;
    case SCR_MESSAGE:     NW = 0; draw_message(); break;
    }
}

static void redraw(void)
{
    draw_screen();
    Screen_Update();
}

/* ===================================================================
 * Note entry: IT piano keyboard -> grid note (or -1)
 * =================================================================== */
static int key_to_note(const it_key_t *k)
{
    /* KeyBoardTable, IT_I.ASM:333 -- transliterated verbatim, including
     * the 0FFFFh terminator. The entries are `DW 12Ch, 0` pairs: the
     * high byte is the press flag (CH bit 0) and the LOW byte is the
     * SCANCODE. The original's lookup at IT_I.ASM:1344 is `Cmp BL, CL`,
     * comparing the scancode alone, so note entry is purely positional:
     * the rows sit at fixed physical places on every keyboard layout.
     * That is the whole point of feature 014 -- on a German QWERTZ board
     * scancode 15h is the key printed Z and 2Ch the key printed Y, and
     * matching characters instead cross-wired exactly those two. */
    static const uint16_t KeyBoardTable[] = {
        0x12C,  0, 0x11F,  1, 0x12D,  2, 0x120,  3, 0x12E,  4,
        0x12F,  5, 0x122,  6, 0x130,  7, 0x123,  8, 0x131,  9,
        0x124, 10, 0x132, 11, 0x110, 12, 0x103, 13, 0x111, 14,
        0x104, 15, 0x112, 16, 0x113, 17, 0x106, 18, 0x114, 19,
        0x107, 20, 0x115, 21, 0x108, 22, 0x116, 23, 0x117, 24,
        0x10A, 25, 0x118, 26, 0x10B, 27, 0x119, 28, 0xFFFF
    };
    const uint16_t *si = KeyBoardTable;

    if (!k || k->scan == 0)
        return -1;
    while (*si != 0xFFFF) {
        uint16_t bx = *si++;
        uint16_t semitone = *si++;
        if ((bx & 0xFF) == k->scan) {           /* Cmp BL, CL */
            int note = BaseOctave * 12 + (int)semitone;
            if (note < 0 || note > 119)
                return -1;
            return note + 1;
        }
    }
    return -1;
}

/* note keys outside the pattern editor (sample/instrument lists, note
 * table, Load Sample preview) count only without Shift/Ctrl/Alt: the
 * original tests `CH, Not 1` first (I_PostSampleList, D_PostLoad-
 * SampleWindow1). Positional matching made Alt-M the piano key M and
 * swallowed the Alt ops on F3/F4. */
static int key_to_note_plain(void)
{
    if (CurKey.flags & (ITKF_SHIFT | ITKF_CTRL | ITKF_ALT))
        return -1;
    return key_to_note(&CurKey);
}

/* ===================================================================
 * Engine control helpers (locked)
 * =================================================================== */
static void commit_current_pattern(void)
{
    Pattern_Pack(CurPattern, Grid, CurRows);
}

/* PEFunction_StorePattern's "stored" copy: the pattern as it was when it
 * was entered or last stored (Alt-Enter). Alt-Backspace (RestoreData)
 * reverts the working grid to it. The port packs the grid into the song
 * eagerly after most edits, so the stored copy is kept separately. */
static editcell_t StoreGrid[MAX_PATROWS * 64];
static int        StorePattern = -1;
static uint16_t   StoreRows;

static void pe_store_point(void)
{
    memcpy(StoreGrid, Grid, sizeof(editcell_t) * (size_t)CurRows * 64);
    StorePattern = CurPattern;
    StoreRows = CurRows;
}

static void load_pattern(uint16_t pat)
{
    CurPattern = pat;
    CurRows = Pattern_EnsureExists(pat, 64);
    CurRows = Pattern_Unpack(pat, Grid);
    if (CurRow >= (int)CurRows) CurRow = CurRows - 1;
    pe_store_point();
}

static void play_song(void)     { ed_lock(); Music_PlaySong(0); ed_unlock(); }
static void play_pattern(void)
{
    ed_lock();
    Music_PlayPattern(CurPattern, CurRows, 0);
    ed_unlock();
}
static void stop_song(void)     { ed_lock(); Music_Stop(); ed_unlock(); }

static void jam_note(int gnote, int chan)
{
    uint8_t n[5];
    if (PlayMode != 0)
        return;
    n[0] = (gnote >= 1 && gnote <= 120) ? (uint8_t)(gnote - 1)
                                        : (uint8_t)gnote;
    n[1] = (uint8_t)CurInstr;
    n[2] = 0xFF;
    n[3] = 0; n[4] = 0;
    ed_lock();
    Music_PlayNote((uint16_t)chan, n, 128);
    ed_unlock();
}

/* ===================================================================
 * Pattern editor key handling
 * =================================================================== */
static void cell_clear(editcell_t *c)
{
    memset(c, 0, sizeof(*c));
}

static int hexval(int k)
{
    if (k >= '0' && k <= '9') return k - '0';
    if (k >= 'a' && k <= 'f') return k - 'a' + 10;
    if (k >= 'A' && k <= 'F') return k - 'A' + 10;
    return -1;
}

/* ==== cell field accessors: map the explicit-mask editcell_t onto
 * the ASM 5-byte cell (note 253 = none, ins 0 = none, vol 255 = none;
 * a command cell is "empty" when cmd and value are both 0) ==== */
static uint8_t cn_get(const editcell_t *c)      /* ASM note value */
{
    if (!(c->mask & CM_NOTE))
        return 253;
    return (c->note >= 1 && c->note <= 120) ? (uint8_t)(c->note - 1)
                                            : c->note;
}
static void cn_set(editcell_t *c, uint8_t v)
{
    if (v == 253) {
        c->mask &= (uint8_t)~CM_NOTE;
        c->note = 0;
    } else {
        c->mask |= CM_NOTE;
        c->note = (v <= 120) ? (uint8_t)(v + 1) : v;
    }
}
static uint8_t ci_get(const editcell_t *c)
{
    return (c->mask & CM_INS) ? c->ins : 0;
}
static void ci_set(editcell_t *c, uint8_t v)
{
    c->ins = v;
    if (v) c->mask |= CM_INS;
    else { c->mask &= (uint8_t)~CM_INS; c->ins = 0; }
}
static uint8_t cv_get(const editcell_t *c)
{
    return (c->mask & CM_VOL) ? c->vol : 0xFF;
}
static void cv_set(editcell_t *c, uint8_t v)
{
    c->vol = v;
    if (v != 0xFF) c->mask |= CM_VOL;
    else { c->mask &= (uint8_t)~CM_VOL; c->vol = 0; }
}
static void cc_fixmask(editcell_t *c)
{
    if (c->cmd || c->cmdval) c->mask |= CM_CMD;
    else                     c->mask &= (uint8_t)~CM_CMD;
}
static void cc_setcmd(editcell_t *c, uint8_t cmd)
{
    if (!(c->mask & CM_CMD)) c->cmdval = 0;
    c->cmd = cmd;
    cc_fixmask(c);
}
static void cc_setval(editcell_t *c, uint8_t val)
{
    if (!(c->mask & CM_CMD)) c->cmd = 0;
    c->cmdval = val;
    cc_fixmask(c);
}
static uint8_t cc_getval(const editcell_t *c)
{
    return (c->mask & CM_CMD) ? c->cmdval : 0;
}
static int cc_empty(const editcell_t *c)        /* word [cell+3] == 0 */
{
    return !(c->mask & CM_CMD) || (c->cmd == 0 && c->cmdval == 0);
}

static editcell_t *cellat(int row, int chan)
{
    return &Grid[row * 64 + chan];
}

/* ==== undo ring (PE_AddToUndoBuffer, IT_PE.ASM 11319): 10 slots,
 * newest first; each is a full snapshot of the current pattern ==== */
static void snapshot_undo(uint8_t type)
{
    undoslot_t s;
    size_t n = (size_t)CurRows * 64;
    int i;

    s.cells = (editcell_t *)malloc(n * sizeof(editcell_t));
    if (!s.cells)
        return;                         /* original flashes + skips */
    memcpy(s.cells, Grid, n * sizeof(editcell_t));
    s.rows = (uint16_t)CurRows;
    s.pattern = CurPattern;
    s.type = type;

    free(UndoRing[9].cells);            /* release oldest */
    for (i = 9; i > 0; i--)
        UndoRing[i] = UndoRing[i - 1];
    UndoRing[0] = s;
}

/* ==== movement (PEFunction_* 3358..3752) ==== */
static int pe_skip(void) { return EditStep ? EditStep : 1; }

static void pe_move_up(void)
{
    int r = CurRow - pe_skip();
    if (r >= 0)
        CurRow = r;                     /* no move if it would go < 0 */
}
static void pe_move_down(void)
{
    int r = CurRow + pe_skip();
    if (r <= (int)CurRows - 1)
        CurRow = r;
}
static void pe_move_left(void)
{
    int col = CurCol - 1;
    if (col < 0) {
        if (CurChan == 0)
            return;
        CurChan--;
        col = 8;
        if (CommandToValue && cc_empty(cellat(CurRow, CurChan)))
            col = 6;
    }
    CurCol = col;
}
static void pe_move_right(void)
{
    int col = CurCol + 1;
    if (col > 6) {
        int wrap = 0;
        if (CommandToValue && cc_empty(cellat(CurRow, CurChan)))
            wrap = 1;
        else if (col >= 9)
            wrap = 1;
        if (wrap) {
            if (CurChan + 1 >= 64)
                return;
            CurChan++;
            col = 0;
        }
    }
    CurCol = col;
}
static void pe_page(int dir, int hilight)   /* PgUp/PgDn chains */
{
    int bl = hilight ? hilight : 16;
    int row = CurRow;
    int maxrow = (int)CurRows - 1;

    if (dir < 0) {
        if (maxrow - pe_skip() < row) { /* near-bottom snap quirk */
            row = ((row - 1) / bl) * bl;
            if (row < 0) row = 0;
        } else {
            row -= bl;
            if (row < 0) row = 0;
        }
    } else {
        row += bl;
        if (row > maxrow) row = maxrow;
    }
    CurRow = row;
}
static void pe_centralise(void)             /* PE_CentraliseCursor */
{
    TopRow = CurRow - 16;
    if (TopRow < 0) TopRow = 0;
}
static void pe_home(void)                   /* cascade */
{
    if (CurCol != 0)        CurCol = 0;
    else if (CurChan != 0)  CurChan = 0;
    else                    CurRow = 0;
}
static void pe_end(void)
{
    int last = Music_GetLastChannel();
    if (CurCol != 8)            CurCol = 8;
    else if (CurChan != last)   CurChan = last;
    else                        CurRow = (int)CurRows - 1;
}
static int PEOrder = 0;                     /* Order cursor (nav) */

static void pe_chan_left(void)              /* PEFunction_AltLeft */
{
    if (CurChan > 0)
        CurChan--;
}
static void pe_chan_right(void)             /* PEFunction_AltRight */
{
    if (CurChan < 63)
        CurChan++;
}
/* PEFunction_ViewLeft / ViewRight (IT_PE.ASM 10330..): move to the
 * previous / next channel in the track-view list (Ctrl-1..5 schemes).
 * The current channel must be in the list; with no track views set up
 * (or at either end) nothing happens. */
static void pe_view_step(int dir)
{
    int i;

    for (i = 0; i < 100 && ViewChannels[i] != 0xFFFF; i++)
        if ((ViewChannels[i] & 0xFF) == CurChan) {
            if (dir < 0 && i > 0)
                CurChan = ViewChannels[i - 1] & 0xFF;
            else if (dir > 0 && i + 1 < 100 && ViewChannels[i + 1] != 0xFFFF)
                CurChan = ViewChannels[i + 1] & 0xFF;
            return;
        }
}
static void pe_alt_up(void)                 /* AltUp view scroll (10438) */
{
    if (TopRow > 0) {
        TopRow--;
        if (CurRow - TopRow >= 32)
            CurRow--;
    }
}
static void pe_alt_down(void)               /* AltDown view scroll (10461) */
{
    int nt = TopRow + 1;
    if ((int)CurRows - 1 - nt >= 31) {
        TopRow = nt;
        if (nt > CurRow)
            CurRow = nt;
    }
}
static void pe_ctrl_home(void)              /* one row up (10410) */
{
    if (CurRow > 0) CurRow--;
}
static void pe_ctrl_end(void)               /* one row down (10424) */
{
    if (CurRow < (int)CurRows - 1) CurRow++;
}
static void pe_goto_pattern(int pat)
{
    if (pat < 0) pat = 0;
    if (pat > 199) pat = 199;
    commit_current_pattern();
    load_pattern((uint16_t)pat);
    if (CurRow > (int)CurRows - 1) CurRow = (int)CurRows - 1;
}
static void pe_next_order(void)             /* NextOrderPattern (8364) */
{
    if (PEOrder < 0xFF && Song.Orders[PEOrder + 1] < 200)
        PEOrder++;
    if (Song.Orders[PEOrder] < 200)
        pe_goto_pattern(Song.Orders[PEOrder]);
}
static void pe_last_order(void)             /* LastOrderPattern (8326) */
{
    if (PEOrder > 0 && Song.Orders[PEOrder - 1] < 200)
        PEOrder--;
    if (Song.Orders[PEOrder] < 200)
        pe_goto_pattern(Song.Orders[PEOrder]);
}
static void pe_set_play_mark(void)          /* Ctrl-F7 (11095) */
{
    /* pressing it again on the marked row clears the mark */
    if (PlayMarkOn && PlayMarkPattern == (int)CurPattern &&
        PlayMarkRow == CurRow) {
        PlayMarkOn = 0;
        return;
    }
    PlayMarkPattern = CurPattern;
    PlayMarkRow = CurRow;
    PlayMarkOn = 1;
}

/* PE_F7 (11128): play from the play mark, or from the cursor when no
 * mark is set. If the order list holds that pattern -- at the current
 * order first, else its first occurrence -- the song plays on from
 * there (Music_PlayPartSong); otherwise the pattern loops alone. */
static void pe_f7(void)
{
    int pat = CurPattern, row = CurRow, ord = -1, i;

    commit_current_pattern();
    pe_store_point();
    if (PlayMarkOn) {
        pat = PlayMarkPattern;
        row = PlayMarkRow;
    }
    if (Song.Orders[PEOrder] == pat)
        ord = PEOrder;
    else
        for (i = 0; i < 256; i++)
            if (Song.Orders[i] == pat) { ord = i; break; }
    ed_lock();
    if (ord >= 0) {
        Music_PlayPartSong((uint16_t)ord, (uint16_t)row);
    } else {
        uint16_t rows = Pattern_EnsureExists((uint16_t)pat, 64);
        if (row >= rows) row = rows - 1;
        Music_PlayPattern((uint16_t)pat, rows, (uint16_t)row);
    }
    ed_unlock();
}

/* PE_PlayCurrentPosition (11075), Ctrl-F6: the pattern from the cursor */
static void pe_play_from_row(void)
{
    commit_current_pattern();
    pe_store_point();
    ed_lock();
    Music_PlayPattern(CurPattern, CurRows, (uint16_t)CurRow);
    ed_unlock();
}

/* Next4Patterns / Last4Patterns (8273/8300), Shift-grey +/- */
static void pe_step4(int dir)
{
    pe_goto_pattern((int)CurPattern + 4 * dir);
}
static void pe_shift_pgup_mv(void)          /* ShiftPgUp (3634) */
{
    pe_page(-1, row_hilight_2());
    pe_centralise();
}
static void pe_shift_pgdn_mv(void)
{
    pe_page(+1, row_hilight_2());
    pe_centralise();
}
static void pe_backspace(void)
{
    int row = CurRow - EditStep;
    int chan = CurChan;

    if (row >= 0) {
        CurRow = row;
        if (MultiChannelInfo[chan]) {   /* previous enabled channel */
            int start = chan, wrapped = 0;
            do {
                chan--;
                if (chan < 0) {
                    if (row == 0)
                        return;
                    wrapped++;
                    chan &= 63;
                }
                if (chan == start)
                    break;
            } while (!MultiChannelInfo[chan]);
            CurChan = chan;
            if (EditStep == 0 && wrapped && row != 0)
                CurRow = row - 1;
        } else if (EditStep == 0) {
            chan--;
            if (chan < 0) {
                if (row == 0)
                    return;
                row--;
                chan &= 63;
            }
            CurRow = row;
            CurChan = chan;
        }
    }
}

/* PE_GotoNextInput (4087): cursor-column dance + row/channel advance */
static void pe_goto_next_input(int full)
{
    int advance = 1;

    if (full) {
        switch (CurCol) {
        case 0: case 1:                     break;
        case 2: CurCol = 3; advance = 0;    break;
        case 3: CurCol = 2;                 break;
        case 4: CurCol = 5; advance = 0;    break;
        case 5: CurCol = 4;                 break;
        case 6: if (CommandToValue) { CurCol = 7; advance = 0; }
                break;
        case 7: CurCol = 8; advance = 0;    break;
        default:                            /* 8 */
            if (CommandToValue) CurCol = 6;
            else                CurCol = 7;
            break;
        }
        if (!advance)
            return;
    }
    {
        int row = CurRow + EditStep;
        int chan = CurChan;

        if (row > (int)CurRows - 1)
            return;
        CurRow = row;
        if (CurCol == 0 && MultiChannelInfo[chan]) {
            int start = chan, wraps = 0;
            do {
                chan++;
                if (chan > 63)
                    wraps++;
                chan &= 63;
                if (chan == start)
                    break;
            } while (!MultiChannelInfo[chan]);
            CurChan = chan;
            if (EditStep == 0 && wraps)
                pe_move_down();             /* SkipValue 0: wrap descends */
        } else if (EditStep == 0) {
            int c = CurChan + 1, r = CurRow;
            if (c > 63) {
                if (r >= (int)CurRows - 1)
                    return;
                r++;
            }
            c &= 63;
            CurRow = r;
            CurChan = c;
        }
    }
}

/* template width quirk: with template mode on and a 1-row clipboard,
 * the wipe/insert/delete verbs cover the clipboard's width */
static int pe_template_width(void)
{
    if (Template && CurCol == 0 && ClipData && ClipRows == 1)
        return ClipChans;
    return 1;
}

/* WipeNote (4019): write a note value + wipe mask-enabled fields */
static void pe_wipe_note(uint8_t noteval)
{
    int w = pe_template_width();
    int idx = CurRow * 64 + CurChan;
    int lim = (int)CurRows * 64;
    int i;

    for (i = 0; i < w && idx + i < lim; i++) {
        editcell_t *c = &Grid[idx + i];
        cn_set(c, noteval);
        if (EditMask & 1) ci_set(c, 0);
        if (EditMask & 2) cv_set(c, 0xFF);
        if (EditMask & 4) { c->cmd = 0; c->cmdval = 0; cc_fixmask(c); }
    }
    LastNote = noteval;
    commit_current_pattern();
    if (ShiftHeld) {                        /* CHORDENTRY */
        int keep = EditStep;
        EditStep = 0;
        NoteEntered = 1;
        pe_goto_next_input(1);
        EditStep = keep;
    } else
        pe_goto_next_input(1);
}

static void jam_cell(const editcell_t *c, int chan)
{
    uint8_t n[5];
    if (PlayMode != 0)
        return;
    n[0] = cn_get(c);
    n[1] = ci_get(c);
    n[2] = cv_get(c);
    n[3] = (uint8_t)((c->mask & CM_CMD) ? c->cmd : 0);
    n[4] = cc_getval(c);
    ed_lock();
    Music_PlayNote((uint16_t)chan, n, 32);
    ed_unlock();
}

static int pe_template_stamp(uint8_t noteval);  /* feature 009 US2 */

/* PE_NewNote4 (4668): write note + mask-enabled Last* fields, play */
static void pe_new_note(uint8_t noteval)
{
    editcell_t *c = cellat(CurRow, CurChan);

    if (Template && pe_template_stamp(noteval))
        return;

    LastNote = noteval;
    cn_set(c, noteval);
    if (EditMask & 1) ci_set(c, (uint8_t)CurInstr);
    if (EditMask & 2) cv_set(c, LastVolume);
    if (EditMask & 4) {
        c->cmd = LastCommand;
        c->cmdval = LastCommandValue;
        cc_fixmask(c);
    }
    jam_cell(c, CurChan);
    commit_current_pattern();
    if (ShiftHeld) {                        /* CHORDENTRY */
        int keep = EditStep;
        EditStep = 0;
        NoteEntered = 1;
        pe_goto_next_input(1);
        EditStep = keep;
    } else
        pe_goto_next_input(1);
}

static void pe_play_current_note(void);     /* US4 (8538/8575) */
static void pe_play_current_row(void);

/* the 9 cursor-column character handlers (PE_PatternCursorPos0..8) */
static int pe_col_key(int key)
{
    editcell_t *c = cellat(CurRow, CurChan);

    switch (CurCol) {
    case 0: {                               /* note */
        int gn = key_to_note(&CurKey);
        if (gn > 0 && (CurKey.flags & ITKF_CAPSDOWN)) {
            /* PE_PatternCursorPreview (3946): Caps Lock held plays the
             * note on this channel with the current instrument and
             * enters nothing */
            uint8_t n[5] = { 0, 0, 0xFF, 0, 0 };
            if (gn - 1 <= 119) {
                n[0] = (uint8_t)(gn - 1);
                n[1] = (uint8_t)CurInstr;
                ed_lock();
                Music_PlayNote((uint16_t)CurChan, n, 32);
                ed_unlock();
            }
            return 1;
        }
        if (gn > 0) {
            int noteval = gn - 1;
            if (noteval <= 119)
                pe_new_note((uint8_t)noteval);
            return 1;
        }
        switch (key) {
        case '.':           pe_wipe_note(253); return 1;
        case '1': case '!': pe_wipe_note(254); return 1;
        case '`': case '~': pe_wipe_note(255); return 1;
        case ' ':           pe_new_note(LastNote); return 1;
        case '4': case '$': pe_play_current_note(); return 1;
        case '8':           pe_play_current_row(); return 1;
        default:            return 0;
        }
    }
    case 1: {                               /* octave digit */
        if (key >= '0' && key <= '9') {
            uint8_t n = cn_get(c);
            if (n <= 120) {                 /* incl. the 120 quirk */
                cn_set(c, (uint8_t)(n % 12 + (key - '0') * 12));
                commit_current_pattern();
            }
            pe_goto_next_input(1);
            return 1;
        }
        return 0;
    }
    case 2: case 3: {                       /* ins tens / units */
        if (key == ' ') {
            ci_set(c, (uint8_t)CurInstr);
        } else if (key == '.') {
            ci_set(c, 0);
        } else if (key >= '0' && key <= '9') {
            uint8_t old = ci_get(c);
            uint8_t v = (CurCol == 2)
                ? (uint8_t)((key - '0') * 10 + old % 10)
                : (uint8_t)(old / 10 * 10 + (key - '0'));
            CurInstr = v;                   /* LastInstrument */
            ci_set(c, v);
            commit_current_pattern();
            pe_goto_next_input(1);
            return 1;
        } else
            return 0;
        commit_current_pattern();
        pe_goto_next_input(0);
        return 1;
    }
    case 4: case 5: {                       /* vol tens / units */
        if (key == '`') {                   /* PE_VolumePan */
            VolumePan ^= 0x80;
            status(VolumePan ? "Panning control set"
                             : "Volume control set");
            return 1;
        }
        if (key == ' ') {
            cv_set(c, LastVolume);
        } else if (key == '.') {
            cv_set(c, 0xFF);
        } else if (key >= '0' && key <= '9') {
            uint8_t raw = cv_get(c);
            uint8_t old = (uint8_t)(raw == 0xFF ? 0 : raw);
            uint8_t v;
            old &= 0x7F;
            if (old >= 65)
                old = (uint8_t)(old - 65);
            if (CurCol == 4) {
                v = (uint8_t)((key - '0') * 10 + old % 10);
                if (v > 64) v = 64;
                v |= VolumePan;
            } else {
                v = (uint8_t)(old / 10 * 10 + (key - '0'));
                if (raw != 0xFF && (raw & 0x7F) > 64) {
                    /* keep the effect class when editing its digit */
                    v = (uint8_t)(v + 65);
                    if (raw & 0x80)
                        v = (uint8_t)(v + 128);
                } else {
                    if (v > 64) v = 64;
                    v |= VolumePan;
                }
            }
            LastVolume = v;
            cv_set(c, v);
            commit_current_pattern();
            pe_goto_next_input(1);
            return 1;
        } else if (CurCol == 4 &&
                   ((key >= 'a' && key <= 'h') ||
                    (key >= 'A' && key <= 'H'))) {
            /* volume-effect letters on the tens column */
            uint8_t raw = cv_get(c);
            uint8_t old = (uint8_t)(raw == 0xFF ? 0 : raw);
            uint8_t units, v;
            int letter = (key >= 'a') ? key - 'a' : key - 'A';
            old &= 0x7F;
            if (old >= 65)
                old = (uint8_t)(old - 65);
            units = (uint8_t)(old % 10);
            v = (uint8_t)(letter * 10);
            if (v >= 60)
                v = (uint8_t)(v + 128 - 60);
            v = (uint8_t)(v + 65 + units);
            LastVolume = v;
            cv_set(c, v);
            commit_current_pattern();
            pe_goto_next_input(1);
            return 1;
        } else
            return 0;
        LastVolume = cv_get(c);
        commit_current_pattern();
        pe_goto_next_input(0);
        return 1;
    }
    case 6: {                               /* command letter */
        uint8_t cmd;
        if (key == ' ')
            cmd = LastCommand;
        else if (key == '.')
            cmd = 0;
        else {
            int lk = (key >= 'a' && key <= 'z') ? key - 32 : key;
            if (lk < 'A' || lk > 'Z')
                return 0;
            cmd = (uint8_t)(lk - '@');
        }
        LastCommand = cmd;
        cc_setcmd(c, cmd);
        commit_current_pattern();
        pe_goto_next_input(1);
        return 1;
    }
    case 7: case 8: {                       /* param hi / lo nibble */
        uint8_t val;
        if (key == '.') {
            val = 0;
        } else if (key == ' ') {
            val = LastCommandValue;
        } else {
            int h = hexval(key);
            if (h < 0)
                return 0;
            val = (CurCol == 7)
                ? (uint8_t)((cc_getval(c) & 0x0F) | (h << 4))
                : (uint8_t)((cc_getval(c) & 0xF0) | h);
            LastCommandValue = val;
            cc_setval(c, val);
            commit_current_pattern();
            pe_goto_next_input(1);
            return 1;
        }
        LastCommandValue = val;
        cc_setval(c, val);
        commit_current_pattern();
        pe_goto_next_input(0);
        return 1;
    }
    default:
        return 0;
    }
}

/* ==== track / row verbs (PEFunction_Insert/Delete 4805/4733,
 * RowInsert/RowDelete 4937/4882) ==== */
static void pe_track_delete(void)
{
    int w = pe_template_width();
    int i, idx;
    int lim = (int)CurRows * 64;

    for (i = 0; i < w; i++) {
        int base = CurRow * 64 + CurChan + i;
        if (base >= lim)
            break;
        for (idx = base; idx + 64 < lim; idx += 64)
            Grid[idx] = Grid[idx + 64];
        cell_clear(&Grid[idx]);
    }
    commit_current_pattern();
}
static void pe_track_insert(void)
{
    int w = pe_template_width();
    int i, idx;
    int lim = (int)CurRows * 64;

    for (i = 0; i < w; i++) {
        int base = CurRow * 64 + CurChan + i;
        if (base >= lim)
            break;
        for (idx = ((lim - 1 - base) / 64) * 64 + base; idx > base;
             idx -= 64)
            Grid[idx] = Grid[idx - 64];
        cell_clear(&Grid[base]);
    }
    commit_current_pattern();
}
static void pe_row_delete(void)             /* Alt-Del, undo type 20 */
{
    int r;
    if (LastKeys[1] != ITK_ALT_DEL)         /* repeat suppression */
        snapshot_undo(20);
    for (r = CurRow; r < (int)CurRows - 1; r++)
        memcpy(&Grid[r * 64], &Grid[(r + 1) * 64],
               64 * sizeof(editcell_t));
    memset(&Grid[((int)CurRows - 1) * 64], 0, 64 * sizeof(editcell_t));
    commit_current_pattern();
}
static void pe_row_insert(void)             /* Alt-Ins, undo type 19 */
{
    int r;
    if (LastKeys[1] != ITK_ALT_INS)
        snapshot_undo(19);
    for (r = (int)CurRows - 1; r > CurRow; r--)
        memcpy(&Grid[r * 64], &Grid[(r - 1) * 64],
               64 * sizeof(editcell_t));
    memset(&Grid[CurRow * 64], 0, 64 * sizeof(editcell_t));
    commit_current_pattern();
}

/* ==== marking (5555..5700, 5994, 7093) ==== */
static void mark_begin_chain(int chan, int row)
{
    if (!BlockMark) {
        BlockMark = 1;
        BlockLeft = BlockRight = chan;
        BlockTop = BlockBottom = row;
        return;
    }
    if (chan > BlockRight) {
        BlockLeft = BlockRight;
        BlockRight = chan;
    } else
        BlockLeft = chan;
    if (row > BlockBottom) {
        BlockTop = BlockBottom;
        BlockBottom = row;
    } else
        BlockTop = row;
}
static void mark_end_chain(int chan, int row)
{
    if (!BlockMark) {
        BlockMark = 1;
        BlockLeft = BlockRight = chan;
        BlockTop = BlockBottom = row;
        return;
    }
    if (chan < BlockLeft) {
        BlockRight = BlockLeft;
        BlockLeft = chan;
    } else
        BlockRight = chan;
    if (row < BlockTop) {
        BlockBottom = BlockTop;
        BlockTop = row;
    } else
        BlockBottom = row;
}
static void pe_alt_d(void)                  /* mark bar / double it */
{
    if (LastKeys[1] == ITK_ALT_A + ('D' - 'A') && BlockMark) {
        int len = (BlockBottom - BlockTop + 1) * 2;
        int bot = BlockTop + len - 1;
        if (bot > (int)CurRows - 1)
            bot = (int)CurRows - 1;
        BlockBottom = bot;
        return;
    }
    BlockTop = CurRow;
    BlockLeft = BlockRight = CurChan;
    BlockBottom = CurRow + row_hilight_2() - 1;
    if (BlockBottom > (int)CurRows - 1)
        BlockBottom = (int)CurRows - 1;
    BlockMark = 1;
}
static void pe_alt_l(void)                  /* mark track / widen */
{
    if (LastKeys[1] == ITK_ALT_A + ('L' - 'A') && BlockMark) {
        BlockLeft = 0;
        BlockRight = 63;
        return;
    }
    BlockMark = 1;
    BlockLeft = BlockRight = CurChan;
    BlockTop = 0;
    BlockBottom = (int)CurRows - 1;
}

/* ==== block operations (IT_PE.ASM 6038..8538) ====
 * All operate on the unpacked Grid (row*64+chan stride) and commit
 * once at the end, matching the ASM handler bodies 1:1. The
 * no-block-marked / no-clipboard paths flash the original's status
 * strings. Cell fields use the editcell accessors above. */
static int block_marked(void)
{
    if (!BlockMark) {
        status("No block is marked.");
        return 0;
    }
    return 1;
}
static int clip_present(void)
{
    if (!ClipData) {
        status("No block data in memory.");
        return 0;
    }
    return 1;
}

static void pe_block_copy(void)             /* Alt-C (6580) */
{
    int w, h, r, c;

    if (!block_marked())
        return;
    w = BlockRight - BlockLeft + 1;
    h = BlockBottom - BlockTop + 1;
    free(ClipData);
    ClipData = (editcell_t *)malloc((size_t)w * h * sizeof(editcell_t));
    if (!ClipData) {
        ClipChans = ClipRows = 0;
        status("Out of memory for block.");
        return;
    }
    ClipChans = w;
    ClipRows = h;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            ClipData[r * w + c] =
                Grid[(BlockTop + r) * 64 + BlockLeft + c];
}

static void pe_wipe_block(void)             /* Alt-Z (6038), type 18 */
{
    int r, c;

    if (!block_marked())
        return;
    if (LastKeys[1] == ITK_ALT_A + ('Z' - 'A'))
        return;                             /* repeat = no-op */
    snapshot_undo(18);
    if (!Template)                          /* copy to clipboard first */
        pe_block_copy();
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++)
            cell_clear(&Grid[r * 64 + c]);
    commit_current_pattern();
}

static void pe_block_overwrite(void)        /* Alt-O (6707), type 10 */
{
    int r, c;

    if (!clip_present())
        return;
    if (LastKeys[1] != ITK_ALT_A + ('O' - 'A'))
        snapshot_undo(10);
    for (r = 0; r < ClipRows; r++) {
        int drow = CurRow + r;
        if (drow > (int)CurRows - 1)
            break;
        for (c = 0; c < ClipChans; c++) {
            int dch = CurChan + c;
            if (dch > 63)
                break;
            Grid[drow * 64 + dch] = ClipData[r * ClipChans + c];
        }
    }
    commit_current_pattern();
}

static void pe_block_paste(void)            /* Alt-P (6782), type 11 */
{
    int r, c;

    if (!clip_present())
        return;
    if (LastKeys[1] != ITK_ALT_A + ('P' - 'A'))
        snapshot_undo(11);
    /* insert-paste: within each pasted column, shift existing rows
     * down by the clipboard height, then drop the clipboard in */
    for (c = 0; c < ClipChans; c++) {
        int dch = CurChan + c;
        if (dch > 63)
            break;
        for (r = (int)CurRows - 1; r >= CurRow + ClipRows; r--)
            Grid[r * 64 + dch] = Grid[(r - ClipRows) * 64 + dch];
        for (r = 0; r < ClipRows; r++) {
            int drow = CurRow + r;
            if (drow > (int)CurRows - 1)
                break;
            Grid[drow * 64 + dch] = ClipData[r * ClipChans + c];
        }
    }
    commit_current_pattern();
}

static void pe_block_mix(void)              /* Alt-M (7007), type 9 */
{
    int r, c, second;

    if (!clip_present())
        return;
    /* first Alt-M mixes whole empty cells; a repeat switches to the
     * field-by-field SecondBlockMix (6898) */
    second = (LastKeys[1] == ITK_ALT_A + ('M' - 'A') &&
              LastKeys[2] != ITK_ALT_A + ('M' - 'A'));
    if (!second)
        snapshot_undo(9);
    for (r = 0; r < ClipRows; r++) {
        int drow = CurRow + r;
        if (drow > (int)CurRows - 1)
            break;
        for (c = 0; c < ClipChans; c++) {
            int dch = CurChan + c;
            editcell_t *d;
            const editcell_t *s;
            if (dch > 63)
                break;
            d = &Grid[drow * 64 + dch];
            s = &ClipData[r * ClipChans + c];
            if (!second) {                  /* mix only fully-empty cells */
                if (d->mask == 0 && d->note == 0)
                    *d = *s;
            } else {                        /* per-field: fill blanks */
                if (!(d->mask & CM_NOTE) && (s->mask & CM_NOTE)) {
                    d->note = s->note; d->mask |= CM_NOTE;
                }
                if (!(d->mask & CM_INS) && (s->mask & CM_INS)) {
                    d->ins = s->ins; d->mask |= CM_INS;
                }
                if (!(d->mask & CM_VOL) && (s->mask & CM_VOL)) {
                    d->vol = s->vol; d->mask |= CM_VOL;
                }
                if (!(d->mask & CM_CMD) && (s->mask & CM_CMD)) {
                    d->cmd = s->cmd; d->cmdval = s->cmdval;
                    d->mask |= CM_CMD;
                }
            }
        }
    }
    commit_current_pattern();
}

static void pe_block_swap(void)             /* Alt-Y (6430), type 17 */
{
    int w, h, r, c;

    if (!block_marked())
        return;
    w = BlockRight - BlockLeft + 1;
    h = BlockBottom - BlockTop + 1;
    /* cursor must be outside the marked block and the swap target
     * must fit the pattern */
    if (CurChan + w - 1 < BlockLeft || CurChan > BlockRight ||
        CurRow + h - 1 < BlockTop || CurRow > BlockBottom) {
        /* no overlap -- ok */
    } else {
        status("Cursor overlaps the marked block.");
        return;
    }
    if (CurChan + w > 64 || CurRow + h > (int)CurRows) {
        status("Swap block is out of range.");
        return;
    }
    snapshot_undo(17);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            editcell_t *a = &Grid[(BlockTop + r) * 64 + BlockLeft + c];
            editcell_t *b = &Grid[(CurRow + r) * 64 + CurChan + c];
            editcell_t t = *a; *a = *b; *b = t;
        }
    commit_current_pattern();
}

/* PEFunction_BlockHalve (6240), Alt-G: the block's rows take every
 * second row starting at its top -- reading on below the block, down
 * to the pattern's end (rows past it give empty cells) */
static void pe_block_halve(void)
{
    int w, h, i, c;

    if (!block_marked())
        return;
    snapshot_undo(5);
    w = BlockRight - BlockLeft + 1;
    h = BlockBottom - BlockTop + 1;
    for (i = 0; i < h; i++) {
        int src = BlockTop + 2 * i, dst = BlockTop + i;
        for (c = 0; c < w; c++) {
            editcell_t *d = &Grid[dst * 64 + BlockLeft + c];
            if (src <= (int)CurRows - 1)
                *d = Grid[src * 64 + BlockLeft + c];
            else
                cell_clear(d);
        }
    }
    commit_current_pattern();
}

/* PEFunction_BlockDouble (6326), Alt-F: the block spreads over twice its
 * length from its top -- row k to row 2k, an empty row after each --
 * overwriting what follows the block, clipped at the pattern's end */
static void pe_block_double(void)
{
    int w, h, k, c, last = (int)CurRows - 1;

    if (!block_marked())
        return;
    snapshot_undo(4);
    w = BlockRight - BlockLeft + 1;
    h = BlockBottom - BlockTop + 1;
    for (k = h - 1; k >= 0; k--) {          /* bottom-up: dst >= src */
        int dst = BlockTop + 2 * k;
        for (c = 0; c < w; c++) {
            if (dst + 1 <= last)
                cell_clear(&Grid[(dst + 1) * 64 + BlockLeft + c]);
            if (dst <= last)
                Grid[dst * 64 + BlockLeft + c] =
                    Grid[(BlockTop + k) * 64 + BlockLeft + c];
        }
    }
    commit_current_pattern();
}

static void pe_roll_up(void)                /* Ctrl-Del (6104) */
{
    int w, r, c;
    editcell_t *tmp;

    if (!block_marked())
        return;
    if (BlockBottom == BlockTop)
        return;
    w = BlockRight - BlockLeft + 1;
    tmp = (editcell_t *)malloc((size_t)w * sizeof(editcell_t));
    if (!tmp)
        return;
    for (c = 0; c < w; c++)
        tmp[c] = Grid[BlockTop * 64 + BlockLeft + c];
    for (r = BlockTop; r < BlockBottom; r++)
        for (c = 0; c < w; c++)
            Grid[r * 64 + BlockLeft + c] =
                Grid[(r + 1) * 64 + BlockLeft + c];
    for (c = 0; c < w; c++)
        Grid[BlockBottom * 64 + BlockLeft + c] = tmp[c];
    free(tmp);
    commit_current_pattern();
}

static void pe_roll_down(void)              /* Ctrl-Ins (6172) */
{
    int w, r, c;
    editcell_t *tmp;

    if (!block_marked())
        return;
    if (BlockBottom == BlockTop)
        return;
    w = BlockRight - BlockLeft + 1;
    tmp = (editcell_t *)malloc((size_t)w * sizeof(editcell_t));
    if (!tmp)
        return;
    for (c = 0; c < w; c++)
        tmp[c] = Grid[BlockBottom * 64 + BlockLeft + c];
    for (r = BlockBottom; r > BlockTop; r--)
        for (c = 0; c < w; c++)
            Grid[r * 64 + BlockLeft + c] =
                Grid[(r - 1) * 64 + BlockLeft + c];
    for (c = 0; c < w; c++)
        Grid[BlockTop * 64 + BlockLeft + c] = tmp[c];
    free(tmp);
    commit_current_pattern();
}

/* transpose one cell's note by +/-1 with the ASM's clamps */
static void transpose_cell(editcell_t *c, int up)
{
    if (!(c->mask & CM_NOTE))
        return;
    if (c->note < 1 || c->note > 120)       /* specials skip */
        return;
    if (up) {
        if (c->note <= 119)                 /* asmnote < 119 */
            c->note++;
    } else {
        if (c->note >= 2)                   /* asmnote > 0 */
            c->note--;
    }
}
static void pe_semi(int up)                 /* Alt-Q/Alt-A (7119/7193) */
{
    int r, c;
    int self = up ? (ITK_ALT_A + ('Q' - 'A'))
                  : (ITK_ALT_A + ('A' - 'A'));

    if (LastKeys[1] != self)
        snapshot_undo(up ? 2 : 3);
    if (!BlockMark) {
        transpose_cell(cellat(CurRow, CurChan), up);
    } else {
        for (r = BlockTop; r <= BlockBottom; r++)
            for (c = BlockLeft; c <= BlockRight; c++)
                transpose_cell(&Grid[r * 64 + c], up);
    }
    commit_current_pattern();
}

/* Alt-X: first press slides effect values across the block
 * (interpolate top..bottom per column), a repeat wipes commands */
static void pe_slide_commands(void)         /* type 15 */
{
    int w, h, r, c;

    if (BlockBottom == BlockTop)
        return;
    snapshot_undo(15);
    w = BlockRight - BlockLeft + 1;
    h = BlockBottom - BlockTop;
    for (c = 0; c < w; c++) {
        editcell_t *top = &Grid[BlockTop * 64 + BlockLeft + c];
        editcell_t *bot = &Grid[BlockBottom * 64 + BlockLeft + c];
        int v0 = cc_getval(top), v1 = cc_getval(bot);
        for (r = 1; r < h; r++) {
            editcell_t *d = &Grid[(BlockTop + r) * 64 + BlockLeft + c];
            int v = v0 + (v1 - v0) * r / h;
            cc_setval(d, (uint8_t)v);
        }
    }
    commit_current_pattern();
}
static void pe_wipe_commands(void)          /* Alt-X (7355), type 16 */
{
    int r, c, wipe;

    if (!block_marked())
        return;
    wipe = (LastKeys[1] == ITK_ALT_A + ('X' - 'A'));
    if (!wipe) {
        pe_slide_commands();
        return;
    }
    if (LastKeys[2] != ITK_ALT_A + ('X' - 'A'))
        snapshot_undo(16);
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++) {
            editcell_t *d = &Grid[r * 64 + c];
            d->cmd = 0; d->cmdval = 0;
            d->mask &= (uint8_t)~CM_CMD;
        }
    commit_current_pattern();
}

/* default volume of a cell's note (PEGetVolume 5761): the sample's
 * default volume, resolved through instrument mode when active */
static int cell_default_volume(const editcell_t *c)
{
    int smp = -1;
    if (!(c->mask & CM_NOTE) || c->note < 1 || c->note > 120)
        return -1;
    if (!(c->mask & CM_INS) || c->ins == 0)
        return -1;
    if (Song.Header.Flags & ITF_INSTRUMENTS) {
        const instrument_t *in = &Song.Ins[c->ins - 1];
        int note = c->note - 1;
        smp = in->NoteSampleTable[note * 2 + 1];
    } else
        smp = c->ins;
    if (smp < 1 || smp > 99)
        return -1;
    return Song.Smp[smp - 1].Vol;
}
/* PEFunction_AltK (5805), Alt-K: slide the volume column of each marked
 * channel from the top row's value to the bottom row's (missing values
 * pull the note's default volume; volumes and pannings are never mixed).
 * Pressed twice it wipes the block's volume column; a third press does
 * nothing. */
static void pe_alt_k(void)
{
    const int altk = ITK_ALT_A + ('K' - 'A');
    int r, c, h;

    if (!block_marked())
        return;
    if (LastKeys[1] == altk) {
        if (LastKeys[2] == altk)
            return;
        snapshot_undo(8);
        for (r = BlockTop; r <= BlockBottom; r++)
            for (c = BlockLeft; c <= BlockRight; c++)
                cv_set(&Grid[r * 64 + c], 0xFF);
        commit_current_pattern();
        return;
    }
    snapshot_undo(7);
    h = BlockBottom - BlockTop;             /* CL = number of rows */
    if (h == 0)
        return;
    for (c = BlockLeft; c <= BlockRight; c++) {
        editcell_t *top = &Grid[BlockTop * 64 + c];
        editcell_t *bot = &Grid[BlockBottom * 64 + c];
        int a = cv_get(bot), b = cv_get(top), chg, step, acc;
        if (a == 0xFF && (a = cell_default_volume(bot)) < 0)
            continue;
        if (b == 0xFF && (b = cell_default_volume(top)) < 0)
            continue;
        if ((a & 0x7F) > 64 || (b & 0x7F) > 64)
            continue;                       /* volume-column effects */
        if ((a & 0x80) != (b & 0x80))
            continue;                       /* one volume, one pan */
        cv_set(bot, (uint8_t)a);
        cv_set(top, (uint8_t)b);
        chg = a - b;                        /* 8.8 step, as the ASM's
                                               two-stage DIV */
        if (chg >= 0) {
            step = ((chg / h) << 8) | (((chg % h) << 8) / h);
            acc = b << 8;
        } else {
            step = -((((-chg) / h) << 8) | ((((-chg) % h) << 8) / h));
            acc = (b << 8) | 0xFF;
        }
        for (r = 0; r < h; r++) {
            cv_set(&Grid[(BlockTop + r) * 64 + c], (uint8_t)(acc >> 8));
            acc = (acc + step) & 0xFFFF;
        }
    }
    commit_current_pattern();
}

static void pe_volume_amp_by(int amp);

static void pe_volume_amp(void)             /* Alt-J (7418), type 6 */
{
    int amp;

    if (!block_marked())
        return;
    if (PEConfig & 4)                       /* fast volume mode */
        amp = FastVolumeAmp;
    else {
        long v;
        if (Amplification > 200)
            Amplification = 200;
        v = prompt_number("Amplify to (%):",
                          (unsigned long)Amplification, 1000);
        if (v < 0)
            return;
        amp = (int)v;
        Amplification = amp;
    }
    pe_volume_amp_by(amp);
}

static void pe_volume_amp_by(int amp)
{
    int r, c;

    snapshot_undo(6);
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++) {
            editcell_t *d = &Grid[r * 64 + c];
            int v = cv_get(d);
            if (v == 0xFF)                  /* pull the default vol */
                v = cell_default_volume(d);
            if (v < 0 || v > 64)            /* skip pans / vol-effects */
                continue;
            v = v * amp / 100;
            if (v > 64) v = 64;
            cv_set(d, (uint8_t)v);
        }
    commit_current_pattern();
}

/* The amplification boxes: a style-3 box, a text line, one type-9
 * thumbbar in a style-25 box, OK (30,32)-(39,34) and Cancel (40,32)-
 * (49,34) (ConfirmOK/CancelButton). Enter confirms, Esc cancels; `alt`
 * is an extra confirming key (AmpExtraKeyList's Alt-J) or 0.
 *  - O1_GetFastAmpList (IT_OBJ1.ASM 869), Ctrl-J: box (22,25)-(57,35),
 *    "   Volume Amplification %" at (27,27), 10..90 at (33,30) in
 *    (32,29)-(44,31).
 *  - O1_SampleAmplificationList (881), F3 Alt-M: box (9,25)-(69,35),
 *    "   Sample Amplification %" at (27,27), 0..400 at (13,30) in
 *    (12,29)-(64,31) (issue #23).
 * Returns 1 on OK with *val set; on Cancel *val is left as it was. */
typedef struct ampbox_t {
    int bx0, by0, bx1, by1;         /* the dialog box */
    const char *text;               /* at (27,27) */
    int tx0, tx1;                   /* thumbbar box columns (rows 29..31) */
    int min, max;                   /* thumbbar at (tx0+1, 30) */
} ampbox_t;

static const ampbox_t *AmpBox;
static int AmpFocus, AmpVal;

static void amp_draw(void)
{
    const ampbox_t *b = AmpBox;

    draw_screen();
    Screen_DrawBox(b->bx0, b->by0, b->bx1, b->by1, 3);
    Screen_DrawString(27, 27, b->text, 0x20);
    Screen_DrawBox(b->tx0, 29, b->tx1, 31, 25);
    draw_thumbbar(b->tx0 + 1, 30, b->min, b->max, AmpVal,
                  AmpFocus == 0 ? 0x03 : 0x02);
    draw_button_style(30, 32, 39, 34, 8, "   OK", 0, AmpFocus == 1);
    draw_button_style(40, 32, 49, 34, 8, " Cancel", 0, AmpFocus == 2);
}

static int amp_dialog(const ampbox_t *b, int *val, int alt)
{
    int drag = 0, lo = b->min, hi = b->max, bar = b->tx0 + 1;

    AmpBox = b;
    AmpVal = *val < lo ? lo : *val > hi ? hi : *val;
    AmpFocus = 0;
    while (Running) {
        int key, v;
        if (drag) {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b) drag = 0;
            else {
                v = lo + (m.px - (bar * 8 + 4));
                AmpVal = v < lo ? lo : v > hi ? hi : v;
            }
        }
        amp_draw();
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return 0; }
        if (key == ITK_ESC) return 0;
        if ((alt && key == alt) ||
            key == ITK_ENTER || (key == ' ' && AmpFocus)) {
            if (AmpFocus == 2) return 0;
            *val = AmpVal;
            return 1;
        }
        if (key == ITK_MOUSE) {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 30, 32, 39, 34)) { *val = AmpVal; return 1; }
            if (mouse_in(&m, 40, 32, 49, 34)) return 0;
            if (thumb_hit(&m, bar, 30, lo, hi, &v)) {
                AmpFocus = 0;
                AmpVal = v;
                drag = 1;
            }
            continue;
        }
        if (key == ITK_TAB || key == ITK_DOWN) {
            AmpFocus = (AmpFocus + 1) % 3; continue;
        }
        if (key == ITK_SHIFT_TAB || key == ITK_UP) {
            AmpFocus = (AmpFocus + 2) % 3; continue;
        }
        if (AmpFocus) {                     /* buttons */
            if (key == ITK_LEFT || key == ITK_RIGHT)
                AmpFocus = AmpFocus == 1 ? 2 : 1;
            continue;
        }
        v = AmpVal;                         /* the thumbbar */
        if (key == ITK_LEFT)             v--;
        else if (key == ITK_RIGHT)       v++;
        else if (key == ITK_SHIFT_LEFT)  v -= 4;
        else if (key == ITK_SHIFT_RIGHT) v += 4;
        else if (key == ITK_CTRL_LEFT)   v -= 2;
        else if (key == ITK_CTRL_RIGHT)  v += 2;
        else if (key == ITK_HOME)        v = lo;
        else if (key == ITK_END)         v = hi;
        else if (key >= '0' && key <= '9') {
            int nv;
            if (!thumb_value_dialog(key, lo, hi, &nv, amp_draw))
                continue;
            v = nv;
        } else
            continue;
        AmpVal = v < lo ? lo : v > hi ? hi : v;
    }
    return 0;
}

static const ampbox_t FastAmpBox = {
    22, 25, 57, 35, "   Volume Amplification %", 32, 44, 10, 90
};
static const ampbox_t SampleAmpBox = {
    9, 25, 69, 35, "   Sample Amplification %", 12, 64, 0, 400
};

static int sample_amp_dialog(int *val)
{
    return amp_dialog(&SampleAmpBox, val, 0);
}

static int fast_amp_dialog(void)
{
    return amp_dialog(&FastAmpBox, &FastVolumeAmp,
                      ITK_ALT_A + ('J' - 'A'));
}

/* ToggleFastVolume (11622), Ctrl-J: Alt-J / Alt-I become one-key
 * attenuate / amplify by FastVolumeAmplification (asked for on enable) */
static void pe_toggle_fast_volume(void)
{
    PEConfig ^= 4;
    if (!(PEConfig & 4)) {
        status("Alt-I / Alt-J fast volume changes disabled");
        return;
    }
    if (fast_amp_dialog()) {
        status("Alt-I / Alt-J fast volume changes enabled");
    } else {
        PEConfig &= (uint8_t)~4;
        status("Alt-I / Alt-J fast volume changes not enabled");
    }
}

/* PEFunction_RestoreData (8610), Alt-Backspace: back to the stored copy
 * (one undo snapshot for a run of presses, as LastKeyBoard2 checks) */
static void pe_restore_data(void)
{
    if (StorePattern != (int)CurPattern)
        return;
    if (LastKeys[1] != ITK_ALT_BACKSPACE)
        snapshot_undo(1);
    memcpy(Grid, StoreGrid, sizeof(editcell_t) * (size_t)StoreRows * 64);
    commit_current_pattern();
}

static void pe_block_volume(void)           /* Alt-V (8417), type 13 */
{
    int r, c;

    if (!block_marked())
        return;
    if (LastKeys[1] != ITK_ALT_A + ('V' - 'A'))
        snapshot_undo(13);
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++)
            cv_set(&Grid[r * 64 + c], LastVolume);
    commit_current_pattern();
}

static void pe_wipe_excess_volumes(void)    /* Alt-W (8474), type 14 */
{
    int r, c;

    if (!block_marked())
        return;
    if (LastKeys[1] == ITK_ALT_A + ('W' - 'A'))
        return;
    snapshot_undo(14);
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++) {
            editcell_t *d = &Grid[r * 64 + c];
            /* wipe volume where the cell has no instrument and no
             * real note (>= NONOTE) */
            if ((d->mask & CM_INS) && d->ins)
                continue;
            if ((d->mask & CM_NOTE) && d->note >= 1 && d->note <= 120)
                continue;
            cv_set(d, 0xFF);
        }
    commit_current_pattern();
}

static void pe_alt_s(void)                  /* Alt-S (5700), type 12 */
{
    int r, c;

    if (!block_marked())
        return;
    if (LastKeys[1] != ITK_ALT_A + ('S' - 'A'))
        snapshot_undo(12);
    for (r = BlockTop; r <= BlockBottom; r++)
        for (c = BlockLeft; c <= BlockRight; c++) {
            editcell_t *d = &Grid[r * 64 + c];
            if ((d->mask & CM_NOTE) && d->note >= 1 && d->note <= 120)
                ci_set(d, (uint8_t)CurInstr);   /* only cells with a note */
        }
    commit_current_pattern();
}

/* shifted movement extends the mark from the shift anchor
 * (PE_PostPatternEditShift, 3237..3296) */
static void pe_shift_move(void (*mover)(void))
{
    int orow = CurRow, ochan = CurChan;

    mover();
    if (orow == CurRow && ochan == CurChan)
        return;
    if (BlockReset) {
        BlockReset = 0;
        BlockMark = 0;
    }
    mark_begin_chain(BlockAnchorChan, BlockAnchorRow);
    mark_end_chain(CurChan, CurRow);
}

/* PE_Template (4555): stamp the clipboard at the cursor, transposing
 * every note so the clipboard's first note becomes the entered note.
 * Overwrite (1) replaces cells; Mix-Pattern (2) keeps existing fields;
 * Mix-Clipboard (3) and Notes-Only (4) prefer clipboard/notes. */
static int pe_template_stamp(uint8_t noteval)
{
    int offset, r, c;
    const editcell_t *first;

    if (!ClipData) {
        status("No block data in memory.");
        return 1;
    }
    first = &ClipData[0];
    if (!(first->mask & CM_NOTE) || first->note < 1 || first->note > 120) {
        status("Template's first note must be a note.");
        return 1;
    }
    offset = (int)noteval - (first->note - 1);

    for (r = 0; r < ClipRows; r++) {
        int drow = CurRow + r;
        if (drow > (int)CurRows - 1)
            break;
        for (c = 0; c < ClipChans; c++) {
            int dch = CurChan + c;
            editcell_t *d, t;
            const editcell_t *s;
            if (dch > 63)
                break;
            d = &Grid[drow * 64 + dch];
            s = &ClipData[r * ClipChans + c];
            t = *s;
            /* transpose real notes, clamp out-of-range to none */
            if ((t.mask & CM_NOTE) && t.note >= 1 && t.note <= 120) {
                int nn = (t.note - 1) + offset;
                if (nn < 0 || nn > 119) { t.mask &= (uint8_t)~CM_NOTE;
                                          t.note = 0; }
                else t.note = (uint8_t)(nn + 1);
            }
            switch (Template) {
            case 1: *d = t; break;              /* overwrite */
            case 4:                             /* notes only */
                if (t.mask & CM_NOTE) { d->note = t.note;
                    d->mask = (uint8_t)((d->mask & ~CM_NOTE) |
                                        (t.mask & CM_NOTE)); }
                break;
            case 2:                             /* mix, pattern wins */
                if (!(d->mask & CM_NOTE) && (t.mask & CM_NOTE))
                    { d->note = t.note; d->mask |= CM_NOTE; }
                if (!(d->mask & CM_INS) && (t.mask & CM_INS))
                    { d->ins = t.ins; d->mask |= CM_INS; }
                if (!(d->mask & CM_VOL) && (t.mask & CM_VOL))
                    { d->vol = t.vol; d->mask |= CM_VOL; }
                if (!(d->mask & CM_CMD) && (t.mask & CM_CMD))
                    { d->cmd = t.cmd; d->cmdval = t.cmdval;
                      d->mask |= CM_CMD; }
                break;
            default:                            /* 3: clipboard wins */
                if (t.mask & CM_NOTE) { d->note = t.note;
                    d->mask = (uint8_t)((d->mask & ~CM_NOTE) | CM_NOTE); }
                if (t.mask & CM_INS)  { d->ins = t.ins; d->mask |= CM_INS; }
                if (t.mask & CM_VOL)  { d->vol = t.vol; d->mask |= CM_VOL; }
                if (t.mask & CM_CMD)  { d->cmd = t.cmd; d->cmdval = t.cmdval;
                                        d->mask |= CM_CMD; }
                break;
            }
        }
    }
    commit_current_pattern();
    if (ClipRows == 1)                          /* play the stamped row */
        jam_cell(cellat(CurRow, CurChan), CurChan);
    if (EditStep)
        pe_goto_next_input(1);
    else {
        CurRow += ClipRows;
        if (CurRow > (int)CurRows - 1)
            CurRow = (int)CurRows - 1;
    }
    return 1;
}

static void pe_play_current_note(void) { }  /* T028 */
static void pe_play_current_row(void)  { }  /* T028 */

/* undo type captions (IT_PE.ASM UndoBufferTypes 300..322) */
static const char *UndoTypeName[23] = {
    "Empty",
    "Undo revert pattern data (Alt-BkSpace)",
    "Undo transposition up (Alt-Q)",
    "Undo transposition down (Alt-A)",
    "Undo block length double (Alt-F)",
    "Undo block length halve (Alt-G)",
    "Undo volume amplification (Alt-J)",
    "Undo volume or panning slide (Alt-K)",
    "Recover volumes/pannings (2*Alt-K)",
    "Replace mixed data (Alt-M)",
    "Replace overwritten data (Alt-O)",
    "Undo paste data (Alt-P)",
    "Undo set sample/instrument (Alt-S)",
    "Undo set volume/panning (Alt-V)",
    "Replace extra volumes/pannings (Alt-W)",
    "Undo effect data slide (Alt-X)",
    "Recover effects/effect data (2*Alt-X)",
    "Undo swap block (Alt-Y)",
    "Undo block cut (Alt-Z)",
    "Remove inserted row(s) (Alt-Insert)",
    "Replace deleted row(s) (Alt-Delete)",
    "Redo (Undo)",
    "Pattern data",
};

/* Ctrl-Backspace: pick a snapshot from the undo ring and revert to it
 * (PEFunction_Undo 11461 + O1_UndoList). Reverting pushes the current
 * state back as a Redo entry (type 21). */
static void pe_undo_requester(void)
{
    int n = 0, i, sel = 0;

    for (i = 0; i < 10; i++)
        if (UndoRing[i].cells)
            n++;
    if (n == 0) {
        status("Nothing to undo.");
        return;
    }
    for (;;) {
        int key, y;

        draw_screen();
        Screen_DrawBox(18, 18, 61, 20 + n + 1, 27);
        Screen_DrawString(20, 19, "Undo:", 0x20);
        for (i = 0; i < n; i++) {
            uint8_t a = (i == sel) ? 0x3A : 0x02;
            y = 20 + i;
            if (i == sel)
                fill(19, y, 41, ' ', a);
            Screen_DrawString(20, y,
                UndoTypeName[UndoRing[i].type <= 22 ? UndoRing[i].type : 0],
                a);
        }
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return; }
        if (key == ITK_UP)   { if (sel > 0) sel--; continue; }
        if (key == ITK_DOWN) { if (sel < n - 1) sel++; continue; }
        if (key == ITK_ESC)  return;
        if (key == ITK_ENTER) break;
    }
    /* revert to slot `sel`: copy it out first (snapshot_undo may free
     * the ring's oldest slot), push current state as Redo, restore */
    if (UndoRing[sel].pattern == CurPattern) {
        uint16_t wantrows = UndoRing[sel].rows;
        size_t nb = (size_t)wantrows * 64 * sizeof(editcell_t);
        editcell_t *want = (editcell_t *)malloc(nb);
        if (!want)
            return;
        memcpy(want, UndoRing[sel].cells, nb);
        snapshot_undo(21);                  /* Redo entry */
        memcpy(Grid, want, nb);
        CurRows = wantrows;
        if (CurRow > (int)CurRows - 1) CurRow = (int)CurRows - 1;
        commit_current_pattern();
        free(want);
    } else {
        status("Undo is for another pattern.");
    }
}

/* ===================================================================
 * Feature 010: pattern-length dialog, mute/solo keys, view schemes
 * =================================================================== */

/* PE_CheckWidth (IT_PE.ASM 8711): sum the view-column widths (+2
 * border, +count-1 dividers); fail if >= 76, else derive ViewWidth and
 * the number of default full-width channels. Returns 0 ok / -1 fail. */
static int pe_check_width(void)
{
    int dx = 0, cx = 0, i;

    for (i = 0; i < 100 && (ViewChannels[i] & 0xFF) != 0xFF; i++) {
        dx += ViewMethodWidth[(ViewChannels[i] >> 8) & 7];
        cx++;
    }
    if (cx) {
        dx += 2;
        if (ViewDivision)
            dx += cx - 1;
    }
    if (dx >= 76)
        return -1;
    ViewWidth = dx;
    NumChansEdit = 0;
    if (dx < 74)
        NumChansEdit = (74 - dx) / 14;
    return 0;
}

static void pe_toggle_tracking(void)    /* Ctrl-T (11189) */
{
    ViewTracking ^= 1;
    status(ViewTracking ? "View-Channel cursor tracking enabled"
                        : "View-Channel cursor tracking disabled");
}

static void pe_toggle_row_hilight(void) /* Ctrl-H (11209) */
{
    PEConfig ^= 2;
    status((PEConfig & 2) ? "Row hilight enabled"
                          : "Row hilight disabled");
}

static void pe_toggle_division(void)    /* Alt-H (10095); silent */
{
    ViewDivision ^= 1;
    if (pe_check_width() < 0)
        ViewDivision ^= 1;              /* revert if too wide */
}

static void pe_clear_views(void)        /* Alt-R (8835) */
{
    memset(ViewChannels, 0xFF, sizeof(ViewChannels));
    ViewWidth = 0;
    NumChansEdit = 5;                   /* StartChannelEdit (873) */
    ViewTracking = 0;
}

/* PE_FastView (10254): n = 0 removes the current channel's view entry
 * (compacting the list), n = 1..6 sets method n-1, appending at the
 * terminator if absent; revert on width failure. */
static void pe_fast_view(int n)
{
    int method = n - 1, i;
    uint16_t old;

    for (i = 0; i < 99; i++) {
        int c = ViewChannels[i] & 0xFF;
        if (c == 0xFF || c == CurChan)
            break;
    }
    if (method < 0) {                   /* Ctrl-0: delete entry */
        if ((ViewChannels[i] & 0xFF) == 0xFF)
            return;
        for (; i < 99; i++) {
            ViewChannels[i] = ViewChannels[i + 1];
            if (ViewChannels[i] == 0xFFFF)
                break;
        }
        ViewChannels[99] = 0xFFFF;
        pe_check_width();
        return;
    }
    old = ViewChannels[i];
    ViewChannels[i] = (uint16_t)((method << 8) | CurChan);
    if (pe_check_width() < 0)
        ViewChannels[i] = old;
}

/* PEFunction_QuickViewSetup (10169): preset = channels 0..n-1 all in
 * one method; count depends on ViewDivision. Enables tracking. */
static void pe_quick_view_setup(int method, int ndiv, int nnodiv)
{
    int n = ViewDivision ? ndiv : nnodiv;
    int fill = ViewDivision ? nnodiv : 100 - n;     /* ASM fill quirk */
    int i, j;

    for (i = 0; i < n; i++)
        ViewChannels[i] = (uint16_t)((method << 8) | i);
    for (j = 0; j < fill && i < 100; j++, i++)
        ViewChannels[i] = 0xFFFF;
    pe_check_width();
    if (!ViewTracking)
        pe_toggle_tracking();
}

/* Alt-T (PEFunction_ViewTrack 8767): cycle the current channel's view
 * method; past the narrowest the entry is removed; on width failure
 * keep narrowing, restoring the old entry when the cycle is spent. */
static void pe_view_track(void)
{
    int i, method;
    uint16_t old;

    for (i = 0; i < 99; i++) {
        int c = ViewChannels[i] & 0xFF;
        if (c == 0xFF || c == CurChan)
            break;
    }
    old = ViewChannels[i];
    method = (((old >> 8) & 0xFF) + 1) & 0xFF;      /* FF+1 -> 0 */
    if (method > 4) {                   /* wrap: remove entry */
        for (; i < 99; i++) {
            ViewChannels[i] = ViewChannels[i + 1];
            if (ViewChannels[i] == 0xFFFF)
                break;
        }
        ViewChannels[99] = 0xFFFF;
        pe_check_width();
        return;
    }
    for (;;) {
        ViewChannels[i] = (uint16_t)((method << 8) | CurChan);
        if (pe_check_width() == 0)
            return;
        method++;
        if (method >= 4) {              /* cycle spent: restore (8825) */
            ViewChannels[i] = old;
            return;
        }
    }
}

/* Ctrl-F2: the Set Pattern Length requester (PE_SetPatternLength
 * 11692; objects O1_SetPatternLength, IT_OBJ1.ASM 659..730). Start/End
 * prime to the current pattern; the length value persists across
 * invocations (the original's re-prime is commented out, 11695).
 * Deviation (README fidelity notes): one undo snapshot of the current
 * pattern is pushed first -- the original's resize is not undoable. */
static void pe_apply_pattern_length(void)
{
    uint16_t p;

    commit_current_pattern();           /* StorePattern (11715) */
    snapshot_undo(22);                  /* type 22 "Pattern data" */
    for (p = PatternLengthStart;
         p <= PatternLengthEnd && p < MAX_PATTERNS; p++) {
        memset(OpGrid, 0,
               sizeof(editcell_t) * (size_t)MAX_PATROWS * 64);
        Pattern_EnsureExists(p, 64);
        Pattern_Unpack(p, OpGrid);
        Pattern_Pack(p, OpGrid, PatternSetLength);
    }
    load_pattern(CurPattern);           /* re-decode (11747) */
    if (CurRow > (int)CurRows - 1)
        CurRow = (int)CurRows - 1;
}

/* The Set Pattern Length box over the pattern editor. Also the background
 * of the "Enter Value" box, which the original draws on top of it. */
static int PslFocus;
static void psl_draw(void)
{
    int focus = PslFocus;
    draw_screen();
    Screen_DrawBox(15, 19, 65, 33, 3);
    Screen_DrawString(31, 21, "Set Pattern Length", 0x20);
    Screen_DrawString(19, 24, "Pattern Length", 0x20);
    Screen_DrawString(19, 27, " Start Pattern", 0x20);
    Screen_DrawString(19, 28, "   End Pattern", 0x20);
    Screen_DrawBox(33, 23, 56, 25, 25);
    Screen_DrawBox(33, 26, 60, 29, 25);
    draw_thumbbar(34, 24, 32, 200, PatternSetLength,
                  focus == 0 ? 0x03 : 0x02);
    draw_thumbbar(34, 27, 0, 199, PatternLengthStart,
                  focus == 1 ? 0x03 : 0x02);
    draw_thumbbar(34, 28, 0, 199, PatternLengthEnd,
                  focus == 2 ? 0x03 : 0x02);
    draw_button_style(35, 30, 44, 32, 8, "   OK", 0, focus == 3);
}

static void pe_set_pattern_length(void)
{
    /* focus: 0 length bar, 1 start bar, 2 end bar, 3 OK */
    int focus = 0, drag = -1;
    static const int bary[3] = { 24, 27, 28 };

    PatternLengthStart = CurPattern;
    PatternLengthEnd = CurPattern;

    for (;;) {
        int key;
        uint16_t *val = focus == 0 ? &PatternSetLength
                      : focus == 1 ? &PatternLengthStart
                      : focus == 2 ? &PatternLengthEnd : NULL;
        int vmin = focus == 0 ? 32 : 0;
        int vmax = focus == 0 ? 200 : 199;

        if (drag >= 0) {                    /* thumbbar drag */
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b) {
                drag = -1;
            } else {
                uint16_t *dv = drag == 0 ? &PatternSetLength
                             : drag == 1 ? &PatternLengthStart
                             : &PatternLengthEnd;
                int lo = drag == 0 ? 32 : 0, hi = drag == 0 ? 200 : 199;
                int r = lo + (m.px - (34 * 8 + 4));
                *dv = (uint16_t)(r < lo ? lo : r > hi ? hi : r);
            }
        }

        PslFocus = focus;
        psl_draw();
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return;
        case ITK_ESC:  return;
        case ITK_TAB: case ITK_DOWN:
            focus = (focus + 1) & 3; continue;
        case ITK_SHIFT_TAB: case ITK_UP:
            focus = (focus + 3) & 3; continue;
        case ITK_ENTER: case ' ':
            if (focus == 3) {
                pe_apply_pattern_length();
                return;
            }
            continue;
        case ITK_MOUSE: {
            it_mouse_t m;
            int i, v;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 35, 30, 44, 32)) {         /* OK */
                pe_apply_pattern_length();
                return;
            }
            for (i = 0; i < 3; i++)
                if (thumb_hit(&m, 34, bary[i], i == 0 ? 32 : 0,
                              i == 0 ? 200 : 199, &v)) {
                    focus = drag = i;
                    *(i == 0 ? &PatternSetLength : i == 1
                      ? &PatternLengthStart : &PatternLengthEnd) =
                        (uint16_t)v;
                }
            continue; }
        default: break;
        }
        if (val) {
            int v = *val;
            if (key == ITK_LEFT)       v--;
            else if (key == ITK_RIGHT) v++;
            else if (key == ITK_SHIFT_LEFT)  v -= 4;    /* issue #7 */
            else if (key == ITK_SHIFT_RIGHT) v += 4;
            else if (key == ITK_CTRL_LEFT)   v -= 2;
            else if (key == ITK_CTRL_RIGHT)  v += 2;
            else if (key == ITK_HOME)  v = vmin;
            else if (key == ITK_END)   v = vmax;
            else if (key >= '0' && key <= '9') {
                int nv;                     /* F_PostThumbBar30 */
                if (!thumb_value_dialog(key, vmin, vmax, &nv, psl_draw))
                    continue;
                v = nv;
            } else
                continue;
            if (v < vmin) v = vmin;
            if (v > vmax) v = vmax;
            *val = (uint16_t)v;
        }
    }
}

/* Glbl_F2_1 + O1_PEConfigList (IT_G.ASM 224, IT_OBJ1.ASM 632): F2 while
 * already in the pattern editor opens "Pattern Editor Options" (issue #4).
 * Box (10,18)-(69,43) style 3; five thumbbars at x=40 in style-9 boxes --
 * Base octave 0..8, Cursor step 0..16, Row hilight minor 0..32 / major
 * 0..128, Number of rows in pattern 32..200 -- then the Command/Value
 * columns Link/Split buttons (set CommandToValue 1/0, drawn pressed when
 * selected) and Done. Values change live; Done, Esc or F2 closes
 * (ESCF2&ReturnList). "Number of rows" starts at the current pattern's
 * length (NumberOfRows = MaxRow+1) and is applied to that pattern on
 * close (MaxRow = NumberOfRows-1), through the same path as Ctrl-F2. */
/* The options box over the pattern editor; also the background of the
 * "Enter Value" box, which the original draws on top of it. */
static int PecFocus, PecRows;
static void pec_draw(void)
{
    int focus = PecFocus;
    draw_screen();
    Screen_DrawBox(10, 18, 69, 43, 3);                      /* PEConfigBox */
    Screen_DrawString(28, 19, "Pattern Editor Options", 0x20);
    Screen_DrawString(28, 23, "Base octave", 0x20);
    Screen_DrawString(28, 26, "Cursor step", 0x20);
    Screen_DrawString(22, 29, "Row hilight minor", 0x20);
    Screen_DrawString(22, 32, "Row hilight major", 0x20);
    Screen_DrawString(14, 35, "Number of rows in pattern", 0x20);
    Screen_DrawString(18, 38, "Command/Value columns", 0x20);
    Screen_DrawBox(39, 22, 42, 24, 9);                      /* PECBox1..5 */
    Screen_DrawBox(39, 25, 43, 27, 9);
    Screen_DrawBox(39, 28, 45, 30, 9);
    Screen_DrawBox(39, 31, 57, 33, 9);
    Screen_DrawBox(39, 34, 62, 36, 9);
    draw_thumbbar(40, 23, 0, 8, BaseOctave, focus == 14 ? 0x03 : 0x02);
    draw_thumbbar(40, 26, 0, 16, EditStep, focus == 15 ? 0x03 : 0x02);
    draw_thumbbar(40, 29, 0, 32, RowHiLight1, focus == 16 ? 0x03 : 0x02);
    draw_thumbbar(40, 32, 0, 128, RowHiLight2, focus == 17 ? 0x03 : 0x02);
    draw_thumbbar(40, 35, 32, 200, PecRows, focus == 18 ? 0x03 : 0x02);
    draw_button_style(39, 37, 50, 39, 8, "   Link", CommandToValue == 1,
                      focus == 19);
    draw_button_style(51, 37, 63, 39, 8, "   Split", CommandToValue == 0,
                      focus == 20);
    draw_button_style(34, 40, 45, 42, 8, "   Done", 0, focus == 13);
}

static void pe_options_dialog(void)
{
    /* focus = object number: 14..18 thumbbars, 19 Link, 20 Split, 13 Done */
    int focus = 14, drag = -1;
    int rows = CurRows;
    static const int tb_y[5]  = { 23, 26, 29, 32, 35 };
    static const int tb_lo[5] = { 0, 0, 0, 0, 32 };
    static const int tb_hi[5] = { 8, 16, 32, 128, 200 };

    while (Running) {
        int key, v;
        int b1 = RowHiLight1, b2 = RowHiLight2;

        if (drag >= 0) {                    /* thumbbar drag (mouse) */
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b) {
                drag = -1;
            } else {
                int r = tb_lo[drag] + (m.px - (40 * 8 + 4));
                r = r < tb_lo[drag] ? tb_lo[drag]
                  : r > tb_hi[drag] ? tb_hi[drag] : r;
                if (drag == 0) BaseOctave = r;
                else if (drag == 1) EditStep = r;
                else if (drag == 2) RowHiLight1 = (uint8_t)r;
                else if (drag == 3) RowHiLight2 = (uint8_t)r;
                else rows = r;
                b1 = RowHiLight1;
                b2 = RowHiLight2;
            }
        }

        PecFocus = focus;
        PecRows = rows;
        pec_draw();
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return; }
        if (key == ITK_ESC || key == ITK_F2)
            break;
        if (key == ITK_MOUSE) {             /* M_Object1List mouse */
            it_mouse_t m;
            int i, hv;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 34, 40, 45, 42))              /* Done */
                break;
            if (mouse_in(&m, 39, 37, 50, 39)) {            /* Link */
                focus = 19; CommandToValue = 1; continue;
            }
            if (mouse_in(&m, 51, 37, 63, 39)) {            /* Split */
                focus = 20; CommandToValue = 0; continue;
            }
            for (i = 0; i < 5; i++)
                if (thumb_hit(&m, 40, tb_y[i], tb_lo[i], tb_hi[i], &hv)) {
                    focus = 14 + i;
                    drag = i;
                    if (i == 0) BaseOctave = hv;
                    else if (i == 1) EditStep = hv;
                    else if (i == 2) RowHiLight1 = (uint8_t)hv;
                    else if (i == 3) RowHiLight2 = (uint8_t)hv;
                    else rows = hv;
                }
            continue;
        }
        switch (key) {                      /* the objects' Up/Down links */
        case ITK_DOWN: case ITK_TAB:
            focus = focus == 13 ? 14 : focus == 20 ? 13
                  : focus == 19 ? 13 : focus + 1;
            continue;
        case ITK_UP: case ITK_SHIFT_TAB:
            focus = focus == 14 ? 13 : focus == 13 ? 19
                  : focus == 20 ? 18 : focus - 1;
            continue;
        default: break;
        }
        if (focus >= 19 || focus == 13) {   /* buttons */
            if ((key == ITK_LEFT || key == ITK_RIGHT) && focus != 13)
                focus = focus == 19 ? 20 : 19;
            else if (key == ITK_ENTER || key == ' ') {
                if (focus == 13)
                    break;                  /* Done */
                CommandToValue = focus == 19 ? 1 : 0;
            }
            continue;
        }
        {                                   /* thumbbars */
            static const int lo[5] = { 0, 0, 0, 0, 32 };
            static const int hi[5] = { 8, 16, 32, 128, 200 };
            int t = focus - 14;
            int *pv[5];
            int b1i = b1, b2i = b2;
            pv[0] = &BaseOctave; pv[1] = &EditStep; pv[2] = &b1i;
            pv[3] = &b2i; pv[4] = &rows;
            v = *pv[t];
            if (key == ITK_LEFT)       v--;
            else if (key == ITK_RIGHT) v++;
            else if (key == ITK_SHIFT_LEFT)  v -= 4;    /* issue #7 */
            else if (key == ITK_SHIFT_RIGHT) v += 4;
            else if (key == ITK_CTRL_LEFT)   v -= 2;
            else if (key == ITK_CTRL_RIGHT)  v += 2;
            else if (key == ITK_HOME)  v = lo[t];
            else if (key == ITK_END)   v = hi[t];
            else if (key >= '0' && key <= '9') {
                int nv;
                if (!thumb_value_dialog(key, lo[t], hi[t], &nv, pec_draw))
                    continue;
                v = nv;
            } else
                continue;
            if (v < lo[t]) v = lo[t];
            if (v > hi[t]) v = hi[t];
            *pv[t] = v;
            RowHiLight1 = (uint8_t)b1i;
            RowHiLight2 = (uint8_t)b2i;
        }
    }

    if (rows != CurRows) {                  /* MaxRow = NumberOfRows - 1 */
        uint16_t keep = PatternSetLength;
        PatternSetLength = (uint16_t)rows;
        PatternLengthStart = PatternLengthEnd = CurPattern;
        pe_apply_pattern_length();
        PatternSetLength = keep;
    }
}

/* PEFunction_MuteNext. IT_PE.ASM:801 binds this with type byte 0 --
 * a raw key code -- to `DW 135h`, i.e. 100h (pressed) | scancode 35h:
 * the MAIN-ROW '/' position, not the keypad (the keypad divide is
 * E0 35, which K_GetKey reports as 1B5h, and it is absent from the
 * table). The very next entry binds MutePrevious with type byte 1 -- a
 * CHARACTER -- to '?', which on a US board is Shift+/ : the same
 * physical key, split by shift. So this is a position binding and its
 * partner is a character binding, in the original's own encoding.
 *
 * The keypad '/' (1B5h) is not in this table: the global key list binds
 * it to DecreaseOctave (and keypad '*' to IncreaseOctave). Until the
 * 2026-09 hotkey audit the port also muted on keypad '/'. */
/* O1_SelectMultiChannel (IT_OBJ1.ASM 4008), 2*Alt-N: box (7,18)-(72,42)
 * style 3, "Multichannel Selection" at (29,19) attr 23h, four style-27
 * boxes at x = 19/35/51/67 holding 16 On/Off toggles each (channel c
 * at (20 + 16*(c/16), 22 + c%16), label "Channel nn" 10 cells to the
 * left, F_DrawSMCChannels), OK (35,39)-(44,41). The toggles link up/
 * down within a column (row 0 up and row 15 down = OK), left/right
 * across columns with wrap; focus starts on the current channel. */
static int SmcFocus;                        /* 0..63 toggles, 64 = OK */
static void smc_draw(void)
{
    int c;

    draw_screen();
    Screen_DrawBox(7, 18, 72, 42, 3);
    Screen_DrawString(29, 19, "Multichannel Selection", 0x23);
    for (c = 0; c < 4; c++)
        Screen_DrawBox(19 + 16 * c, 21, 23 + 16 * c, 38, 27);
    for (c = 0; c < 64; c++) {
        int x = 20 + 16 * (c / 16), y = 22 + c % 16;
        drawf(x - 11, y, 0x20, "Channel %02d", c + 1);
        Screen_DrawString(x, y, MultiChannelInfo[c] ? "On " : "Off", 0x02);
        if (SmcFocus == c)
            Screen_DrawString(x, y, MultiChannelInfo[c] ? "On" : "Off", 0x30);
    }
    draw_button_style(35, 39, 44, 41, 8, "   OK   ", 0, SmcFocus == 64);
}

static void pe_multichannel_menu(void)
{
    SmcFocus = CurChan;
    while (Running) {
        int key, f = SmcFocus;
        smc_draw();
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return; }
        if (key == ITK_ESC) return;
        if (key == ITK_MOUSE) {
            it_mouse_t m;
            int c;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 35, 39, 44, 41)) return;
            for (c = 0; c < 64; c++)
                if (mouse_in(&m, 20 + 16 * (c / 16), 22 + c % 16,
                             22 + 16 * (c / 16), 22 + c % 16)) {
                    SmcFocus = c;
                    MultiChannelInfo[c] ^= 1;
                }
            continue;
        }
        if (f == 64) {                      /* the OK button */
            if (key == ITK_ENTER || key == ' ') return;
            if (key == ITK_UP) SmcFocus = 15;
            else if (key == ITK_DOWN || key == ITK_TAB) SmcFocus = 0;
            continue;
        }
        switch (key) {
        case ITK_UP:    SmcFocus = f % 16 ? f - 1 : 64; break;
        case ITK_DOWN:  SmcFocus = f % 16 == 15 ? 64 : f + 1; break;
        case ITK_RIGHT: case ITK_TAB:   SmcFocus = (f + 16) % 64; break;
        case ITK_LEFT:  case ITK_SHIFT_TAB: SmcFocus = (f + 48) % 64; break;
        case ITK_ENTER: case ' ':       MultiChannelInfo[f] ^= 1; break;
        default: break;
        }
    }
}

static void pe_mute_next(void)
{
    ed_lock(); Music_ToggleChannel((uint16_t)CurChan); ed_unlock();
    if (CurChan < 63) { CurCol = 0; CurChan++; }
}

static void handle_pattern_key(int key)
{
    /* position-dispatched bindings go before the character switch */
    if ((CurKey.flags & ITKF_PRESSED) && CurKey.scan == 0x35 &&
        !(CurKey.flags & ITKF_SHIFT) && key != '?') {
        pe_mute_next();
        return;
    }

    /* LastKeyBoard history (PE_PostPatternEdit 3215..3222) */
    LastKeys[2] = LastKeys[1];
    LastKeys[1] = LastKeys[0];
    LastKeys[0] = key;

    switch (key) {
    /* -- shift press/release: marking anchor + chord entry -- */
    case ITK_SHIFT_PRESS:
        BlockAnchorChan = CurChan;
        BlockAnchorRow = CurRow;
        BlockReset = 1;
        NoteEntered = 0;
        ShiftHeld = 1;
        return;
    case ITK_SHIFT_RELEASE:
        ShiftHeld = 0;
        if (NoteEntered) {                  /* chord entry ends */
            CurChan = BlockAnchorChan;
            CurRow = BlockAnchorRow;
            NoteEntered = 0;
            pe_goto_next_input(1);
        }
        return;
    /* -- movement -- */
    case ITK_UP:    pe_move_up(); return;
    case ITK_DOWN:  pe_move_down(); return;
    case ITK_LEFT:  pe_move_left(); return;
    case ITK_RIGHT: pe_move_right(); return;
    case ITK_PGUP:  pe_page(-1, row_hilight_2()); return;
    case ITK_PGDN:  pe_page(+1, row_hilight_2()); return;
    case ITK_HOME:  pe_home(); return;
    case ITK_END:   pe_end(); return;
    /* -- channel / view movement (Alt/Ctrl arrows) -- */
    case ITK_ALT_UP:    pe_alt_up();    return;
    case ITK_ALT_DOWN:  pe_alt_down();  return;
    case ITK_CTRL_LEFT: pe_view_step(-1); return;  /* ViewLeft */
    case ITK_CTRL_RIGHT:pe_view_step(+1); return;  /* ViewRight */
    case ITK_CTRL_HOME: pe_ctrl_home(); return;
    case ITK_CTRL_END:  pe_ctrl_end();  return;
    /* -- pattern / order navigation -- */
    case ITK_CTRL_PLUS:  pe_next_order(); return;
    case ITK_CTRL_MINUS: pe_last_order(); return;
    case ITK_CTRL_F7:    pe_set_play_mark(); return;
    case ITK_CTRL_F6:    pe_play_from_row(); return;    /* 11075 */
    case ITK_SHIFT_PLUS:  pe_step4(+1); return;         /* Next4Patterns */
    case ITK_SHIFT_MINUS: pe_step4(-1); return;         /* Last4Patterns */
    /* -- hotkey audit (2026-09): the rest of PEFunctions -- */
    case ITK_ALT_LEFT:  pe_chan_left();  return;        /* AltLeft */
    case ITK_ALT_RIGHT: pe_chan_right(); return;        /* AltRight */
    case ITK_CTRL_PGUP: CurRow = 0; return;             /* Ctrl_PgUp */
    case ITK_CTRL_PGDN: CurRow = (int)CurRows - 1; return;
    case ITK_ALT_HOME:          /* PgUp/PgDn by the minor hilight (3531) */
        pe_page(-1, RowHiLight1 ? RowHiLight1 : 16); return;
    case ITK_ALT_END:
        pe_page(+1, RowHiLight1 ? RowHiLight1 : 16); return;
    case ITK_CTRL_UP:                   /* DecreaseInstrument (4999) */
        if (CurInstr > 0) CurInstr--;
        return;
    case ITK_CTRL_DOWN:                 /* IncreaseInstrument */
        if (CurInstr < 99) CurInstr++;
        return;
    case ITK_ALT_ENTER:                 /* StoreCurrentPattern (8138) */
        commit_current_pattern();
        pe_store_point();
        return;
    case ITK_ALT_BACKSPACE: pe_restore_data(); return;
    case ITK_ALT_A + ('K' - 'A'): pe_alt_k(); return;
    case 0x0A: pe_toggle_fast_volume(); return;         /* Ctrl-J */
    case 0x16:                          /* Ctrl-V: PE_ToggleDefaultVolume */
        PEDefaultVolume ^= 1;
        status(PEDefaultVolume ? "Default volumes enabled"
                               : "Default volumes disabled");
        return;
    /* -- view/entry toggles -- */
    case 0x03:                          /* Ctrl-C: centralise cursor */
        PEConfig ^= 1;
        status((PEConfig & 1) ? "Centralise cursor enabled"
                              : "Centralise cursor disabled");
        return;
    case 0x14: pe_toggle_tracking();    return;     /* Ctrl-T */
    case 0x08: pe_toggle_row_hilight(); return;     /* Ctrl-H */
    /* -- feature 010: pattern length, mute/solo, view schemes -- */
    case ITK_CTRL_F2: pe_set_pattern_length(); return;
    case '\\':                          /* PEFunction_Alt_F9 */
    case ITK_ALT_F9:
        ed_lock(); Music_ToggleChannel((uint16_t)CurChan); ed_unlock();
        return;
    case '?':                           /* MutePrevious (clamp at 0) */
        if (CurChan > 0)
            CurChan--;
        ed_lock(); Music_ToggleChannel((uint16_t)CurChan); ed_unlock();
        return;
    case ITK_ALT_F10:                   /* solo */
        ed_lock(); Music_SoloChannel((uint16_t)CurChan); ed_unlock();
        return;
    case '|':                           /* SoloGotoNext: solo + Tab */
        ed_lock(); Music_SoloChannel((uint16_t)CurChan); ed_unlock();
        if (CurChan < 63) { CurCol = 0; CurChan++; }
        return;
    case ITK_ALT_BACKSLASH:             /* UnmuteAll */
        ed_lock(); Music_UnmuteAll(); ed_unlock();
        return;
    case ITK_ALT_A + ('H' - 'A'): pe_toggle_division(); return;
    case ITK_ALT_A + ('R' - 'A'): pe_clear_views();     return;
    case ITK_ALT_A + ('T' - 'A'): pe_view_track();      return;
    case ITK_CTRL_SHIFT_1:     pe_quick_view_setup(1, 6, 7);   return;
    case ITK_CTRL_SHIFT_1 + 1: pe_quick_view_setup(2, 9, 10);  return;
    case ITK_CTRL_SHIFT_1 + 2: pe_quick_view_setup(3, 18, 24); return;
    case ITK_CTRL_SHIFT_1 + 3: pe_quick_view_setup(4, 24, 36); return;
    case ITK_SCROLL_LOCK:               /* ToggleTrace */
        TracePlayback ^= 1;
        status(TracePlayback ? "Playback tracing enabled"
                             : "Playback tracing disabled");
        return;
    case ITK_TAB:
        if (CurChan < 63) { CurCol = 0; CurChan++; }
        return;
    case ITK_SHIFT_TAB:
        if (CurCol == 0) { if (CurChan > 0) CurChan--; }
        CurCol = 0;
        return;
    case ITK_BACKSPACE: pe_backspace(); return;
    /* -- shifted movement extends the mark; Shift-Left/Right move a
     * whole channel (the key table routes them to AltLeft/AltRight),
     * Shift-PgUp/PgDn also centralise (3634/3646) -- */
    case ITK_SHIFT_UP:    pe_shift_move(pe_move_up); return;
    case ITK_SHIFT_DOWN:  pe_shift_move(pe_move_down); return;
    case ITK_SHIFT_LEFT:  pe_shift_move(pe_chan_left); return;
    case ITK_SHIFT_RIGHT: pe_shift_move(pe_chan_right); return;
    case ITK_SHIFT_HOME:  pe_shift_move(pe_home); return;
    case ITK_SHIFT_END:   pe_shift_move(pe_end); return;
    case ITK_SHIFT_PGUP:  pe_shift_move(pe_shift_pgup_mv); return;
    case ITK_SHIFT_PGDN:  pe_shift_move(pe_shift_pgdn_mv); return;
    /* -- marking -- */
    case ITK_ALT_A + ('B' - 'A'):
        mark_begin_chain(CurChan, CurRow); return;
    case ITK_ALT_A + ('E' - 'A'):
        mark_end_chain(CurChan, CurRow); return;
    case ITK_ALT_A + ('D' - 'A'): pe_alt_d(); return;
    case ITK_ALT_A + ('L' - 'A'): pe_alt_l(); return;
    case ITK_ALT_A + ('U' - 'A'): BlockMark = 0; return;
    /* -- block operations -- */
    case ITK_ALT_A + ('C' - 'A'): pe_block_copy(); return;
    case ITK_ALT_A + ('O' - 'A'): pe_block_overwrite(); return;
    case ITK_ALT_A + ('P' - 'A'): pe_block_paste(); return;
    case ITK_ALT_A + ('M' - 'A'): pe_block_mix(); return;
    case ITK_ALT_A + ('Z' - 'A'): pe_wipe_block(); return;
    case ITK_ALT_A + ('Y' - 'A'): pe_block_swap(); return;
    case ITK_ALT_A + ('F' - 'A'): pe_block_double(); return;
    case ITK_ALT_A + ('G' - 'A'): pe_block_halve(); return;
    case ITK_ALT_A + ('Q' - 'A'): pe_semi(1); return;
    case ITK_ALT_A + ('A' - 'A'): pe_semi(0); return;
    case ITK_ALT_A + ('X' - 'A'): pe_wipe_commands(); return;
    case ITK_ALT_A + ('J' - 'A'): pe_volume_amp(); return;
    case ITK_ALT_A + ('V' - 'A'): pe_block_volume(); return;
    case ITK_ALT_A + ('W' - 'A'): pe_wipe_excess_volumes(); return;
    case ITK_ALT_A + ('S' - 'A'): pe_alt_s(); return;
    /* -- entry pipeline toggles -- */
    case ITK_ALT_A + ('N' - 'A'):           /* multichannel toggle */
        MultiChannelInfo[CurChan] ^= 1;
        if (LastKeys[1] == ITK_ALT_A + ('N' - 'A')) {
            pe_multichannel_menu();         /* 2*Alt-N */
            return;
        }
        status(MultiChannelInfo[CurChan]
               ? "Multichannel enabled for this channel"
               : "Multichannel disabled for this channel");
        return;
    case ITK_ALT_A + ('I' - 'A'):           /* ToggleTemplate (8660) */
        if (PEConfig & 4) {                 /* fast volume amplify */
            if (block_marked())
                pe_volume_amp_by(10050 / FastVolumeAmp);
            return;
        }
        if (++Template > 4) Template = 0;
        status(Template == 0 ? "Template mode off"
             : Template == 1 ? "Template, Overwrite"
             : Template == 2 ? "Template, Mix - Pattern data precedence"
             : Template == 3 ? "Template, Mix - Clipboard data precedence"
                             : "Template, Notes only");
        return;
    case ':':                               /* TemplateOff (8682) */
        Template = 0;
        status("Template mode off");
        return;
    /* -- row / track verbs -- */
    case ITK_INS:   pe_track_insert(); return;
    case ITK_DEL:   pe_track_delete(); return;
    case ITK_ALT_INS: pe_row_insert(); return;
    case ITK_ALT_DEL: pe_row_delete(); return;
    case ITK_CTRL_INS: pe_roll_down(); return;
    case ITK_CTRL_DEL: pe_roll_up(); return;
    case ITK_CTRL_BACKSPACE: pe_undo_requester(); return;
    /* -- edit step (Alt-0..9, PE_PostPatternEdit6 + Alt0) -- */
    case ITK_ALT_0:
        EditStep = 0;
        status("Cursor step set to 0");
        return;
    /* -- octave / step / pattern / instrument keys (existing) -- */
    /* { } [ ] are PE_LeftBrace.. = the global speed/volume keys,
     * handled in handle_global (octave is keypad / *, the skip value
     * Alt-0..9, as in the original) */
    case '<': case ';':             /* PEFunction_DecreaseInstrument */
        if (CurInstr > 0) CurInstr--;
        return;
    case '>': case '\'':            /* PEFunction_IncreaseInstrument */
        if (CurInstr < 99) CurInstr++;
        return;
    case ',':                       /* PEFunction_SetMask (3752) */
        EditMask ^= MaskChange[CurCol];
        return;
    case ITK_ENTER: {               /* PEFunction_PickUp (3857) */
        editcell_t *c = cellat(CurRow, CurChan);
        if (Template != 4)
            Template = 0;
        LastNote = cn_get(c);
        if (ci_get(c))
            CurInstr = ci_get(c);
        LastVolume = cv_get(c);
        LastCommand = (uint8_t)((c->mask & CM_CMD) ? c->cmd : 0);
        LastCommandValue = cc_getval(c);
        return;
    }
    case '-': case '+': case '=':
        commit_current_pattern();
        if (key == '-') {
            if (CurPattern > 0)
                load_pattern(CurPattern - 1);
        } else {
            if (CurPattern + 1 < MAX_PATTERNS)
                load_pattern(CurPattern + 1);
        }
        return;
    default: break;
    }

    /* Alt-1..9 set the cursor step (PE_PostPatternEdit6) */
    if (key >= ITK_ALT_0 + 1 && key <= ITK_ALT_0 + 9) {
        EditStep = key - ITK_ALT_0;
        status("Cursor step set to %d", EditStep);
        return;
    }

    /* Ctrl-0..5: view-method assignment (PEFunction_Ctrl0..5) */
    if (key >= ITK_CTRL_0 && key <= ITK_CTRL_0 + 5) {
        pe_fast_view(key - ITK_CTRL_0);
        return;
    }

    /* plain characters go to the cursor-column handler */
    if (key >= 32 && key < 127)
        pe_col_key(key);
}

/* ===================================================================
 * List widgets: key/click handlers and widget action callbacks
 * =================================================================== */
/* I_ToggleMultiChannel / UpdateMultiChannel (IT_I.ASM 8733): F3/F4 notes
 * play on PlayChannel (channel 1 by default; < > , . change it); with
 * multichannel playback on, every note moves on to the next channel, so
 * notes ring on together. */
static int ListMultiChannel = 0, ListPlayChannel = 0;

static int list_play_channel(void)
{
    if (!ListMultiChannel)
        return ListPlayChannel;
    if (++ListPlayChannel >= 64)
        ListPlayChannel = 0;
    status("Using channel %d for playback", ListPlayChannel + 1);
    return ListPlayChannel;
}

static void list_toggle_multichannel(void)
{
    ListMultiChannel ^= 1;
    status(ListMultiChannel ? "Multichannel playback enabled"
                            : "Multichannel playback disabled");
}

static int generic_list_lkey(int key, int n)
{
    switch (key) {
    case ITK_CTRL_PGUP: ListSel = 0; return 1;      /* I_SampleCtrlPgUp */
    case ITK_CTRL_PGDN: ListSel = n - 1; return 1;  /* ... 99 */
    case ITK_UP:   if (ListSel > 0) ListSel--; return 1;
    case ITK_DOWN: if (ListSel < n - 1) ListSel++; return 1;
    case ITK_PGUP: ListSel -= 16; if (ListSel < 0) ListSel = 0; return 1;
    case ITK_PGDN: ListSel += 16; if (ListSel >= n) ListSel = n-1; return 1;
    case ITK_HOME: ListSel = 0; return 1;
    case ITK_END:  ListSel = n - 1; return 1;
    default: break;
    }
    {
        int gn = key_to_note_plain();
        if (gn > 0) {
            CurInstr = ListSel + 1;
            jam_note(gn, list_play_channel());
            return 1;
        }
    }
    return 0;
}

static void sample_library_requester(void);     /* feature 006 */
static void instrument_library_requester(void);

/* insert/delete within the 25-char editable name region (feature 013;
 * I_PostSampleList3/5 -- the 26th byte stays untouched on insert and
 * zero-fills on delete) */
static void name_insert(char *nm, int pos, char c)
{
    int i;
    if (pos < 0 || pos > 24)            /* callers keep pos in [0,24] */
        return;
    for (i = 24; i > pos; i--)
        nm[i] = nm[i - 1];
    nm[pos] = c;
}

static void name_delete(char *nm, int pos)
{
    int i;
    if (pos < 0 || pos > 24)            /* callers keep pos in [0,24] */
        return;
    for (i = pos; i < 24; i++)
        nm[i] = nm[i + 1];
    nm[24] = 0;
}

static int sample_list_lkey(int key)
{
    char *nm = Song.Smp[ListSel].SampleName;

    if (key == ITK_ENTER) {                 /* IT: Enter = load sample */
        sample_library_requester();
        return 1;
    }
    switch (key) {          /* I_SampleLeft/Right/Home/End: the name
                               cursor, not the list */
    case ITK_LEFT:  if (SamplePos > 0)  SamplePos--; return 1;
    case ITK_RIGHT: if (SamplePos < 25) SamplePos++; return 1;
    case ITK_HOME:  SamplePos = 0;  return 1;
    case ITK_END:   SamplePos = 25; return 1;
    default: break;
    }
    if (SamplePos < 25) {                   /* editing inside the name */
        if (key >= 32 && key < 256) {       /* CP437 (feature 014) */
            name_insert(nm, SamplePos, (char)key);
            SamplePos++;
            return 1;
        }
        if (key == ITK_BACKSPACE) {
            if (SamplePos > 0) {
                SamplePos--;
                name_delete(nm, SamplePos);
            }
            return 1;
        }
        if (key == ITK_DEL) {
            name_delete(nm, SamplePos);
            return 1;
        }
    }
    return generic_list_lkey(key, 99);
}

static int instr_list_lkey(int key)
{
    char *nm = Song.Ins[ListSel].InstrumentName;

    if (InstrumentEdit) {       /* I_PostInstrumentWindow edit mode */
        if (key == ITK_ESC || key == ITK_ENTER) {
            InstrumentEdit = 0;
            return 1;
        }
        if (key >= 32 && key < 256) {       /* CP437 (feature 014) */
            name_insert(nm, InstrumentPos, (char)key);
            if (InstrumentPos < 24)         /* I_InstrumentRight */
                InstrumentPos++;
            return 1;
        }
        switch (key) {
        case ITK_BACKSPACE:
            if (InstrumentPos > 0) {
                InstrumentPos--;
                name_delete(nm, InstrumentPos);
            }
            return 1;
        case ITK_DEL:
            name_delete(nm, InstrumentPos);
            return 1;
        case ITK_LEFT:  if (InstrumentPos > 0)  InstrumentPos--; return 1;
        case ITK_RIGHT: if (InstrumentPos < 24) InstrumentPos++; return 1;
        default:
            break;                          /* list nav still works */
        }
    }
    if (key == ITK_ENTER) {                 /* IT: Enter = load instrument */
        instrument_library_requester();
        return 1;
    }
    if (key == ' ') {                       /* Spacebar enters edit mode
                                               (I_PostInstrumentWindow7) */
        InstrumentEdit = 1;
        InstrumentPos = 0;
        return 1;
    }
    return generic_list_lkey(key, 99);
}

static void sample_list_lclick(int row, int mx, int mpx)
{
    (void)mpx;
    if (row >= 0 && SmpListTop + row < 99)
        ListSel = SmpListTop + row;
    /* I_SelectInstrument: click position places the name cursor
     * (left of the name = the note-play stop) */
    SamplePos = (mx >= 5 && mx - 5 < 25) ? mx - 5 : 25;
}

static void instr_list_lclick(int row, int mx, int mpx)
{
    (void)mpx;
    if (row >= 0 && InsListTop + row < 99)
        ListSel = InsListTop + row;
    if (InstrumentEdit) {                   /* I_SelectInstrument2 */
        int p = mx - 5;
        if (p < 0)  p = 0;
        if (p > 24) p = 24;
        InstrumentPos = p;
    }
}

/* PE_PostOrderListSwapPatterns: renumber pattern b as d and vice versa
 * everywhere -- the order list, the pattern data slots, and the pattern
 * currently open in the editor (*cur). Caller holds the engine lock. */
static void order_swap_patterns(int b, int d, int *cur)
{
    pattern_t tmp;
    int i;

    if (*cur == b)
        *cur = d;
    else if (*cur == d)
        *cur = b;
    for (i = 0; i < 256; i++) {
        if (Song.Orders[i] == b)
            Song.Orders[i] = (uint8_t)d;
        else if (Song.Orders[i] == d)
            Song.Orders[i] = (uint8_t)b;
    }
    tmp = Song.Patterns[b];
    Song.Patterns[b] = Song.Patterns[d];
    Song.Patterns[d] = tmp;
}

/* PE_PostOrderListReorder (Alt-R): renumber the patterns so the order
 * list plays 0, 1, 2, ... */
static void order_reorder_patterns(void)
{
    int i, d = 0, cur;

    stop_song();
    commit_current_pattern();
    cur = CurPattern;
    ed_lock();
    for (i = 0; i < 256; i++) {
        int al = Song.Orders[i];

        if (al >= 200 || al < d)
            continue;
        if (al > d)
            order_swap_patterns(al, d, &cur);
        d++;
    }
    ed_unlock();
    load_pattern((uint16_t)cur);
}

/* the OrderListKeys table (IT_PE.ASM 959) over the full 256-slot list */
static int order_list_lkey(int key)
{
    int o = ListSel;

    switch (key) {
    case ITK_UP:                                /* PE_PostOrderList1 */
        if (ListSel > 0) ListSel--;
        return 1;
    case ITK_DOWN:                              /* PE_PostOrderList3 */
        if (ListSel < 255) ListSel++;
        return 1;
    case ITK_PGUP:                              /* PE_PostOrderList4 */
        ListSel -= 16; if (ListSel < 0) ListSel = 0;
        return 1;
    case ITK_PGDN:                              /* PE_PostOrderList6 */
        ListSel += 16; if (ListSel > 255) ListSel = 255;
        return 1;
    case ITK_HOME:
        ListSel = 0;
        return 1;
    case ITK_END: {                             /* first --- terminator */
        int i;
        for (i = 0; i < 255; i++)
            if (Song.Orders[i] == 0xFF)
                break;
        ListSel = i;
        return 1;
    }
    case ITK_LEFT:                              /* PE_PostOrderList7 */
        OrderCursor = OrderCursor ? OrderCursor - 1 : 2;
        return 1;
    case ITK_RIGHT:                             /* PE_PostOrderList9 */
        OrderCursor = OrderCursor < 2 ? OrderCursor + 1 : 0;
        return 1;
    case '-':                                   /* PE_PostOrderList16 */
        ed_lock();
        Song.Orders[o] = 0xFF;
        ed_unlock();
        OrderCursor = 0;
        if (ListSel < 255) ListSel++;
        return 1;
    case '+': case '=':                         /* PE_PostOrderList17 */
        ed_lock();
        Song.Orders[o] = 0xFE;
        ed_unlock();
        OrderCursor = 0;
        if (ListSel < 255) ListSel++;
        return 1;
    case ITK_INS: {                             /* PE_PostOrderList19 */
        int i;
        ed_lock();
        for (i = 255; i > o; i--)
            Song.Orders[i] = Song.Orders[i - 1];
        Song.Orders[o] = 0xFF;
        ed_unlock();
        return 1;
    }
    case ITK_DEL: {                             /* PE_PostOrderList18 */
        int i;
        ed_lock();
        for (i = o; i < 255; i++)
            Song.Orders[i] = Song.Orders[i + 1];
        Song.Orders[255] = 0xFF;
        ed_unlock();
        return 1;
    }
    case 'n': case 'N': {                       /* PE_PostOrderList22 */
        int prev;
        if (o == 0)
            return 0;
        prev = Song.Orders[o - 1];
        if (prev > 198)
            return 0;
        ed_lock();
        Song.Orders[o] = (uint8_t)(prev + 1);
        ed_unlock();
        if (ListSel < 255) ListSel++;
        return 1;
    }
    case ' ': case ITK_CTRL_F7:                 /* PE_PostOrderListNextOrder */
        if (PlayMode == 2) {
            status("Playing order %d next", o);
            ed_lock();
            Music_SetNextOrder((uint16_t)o);
            ed_unlock();
        }
        return 1;
    case ITK_ALT_A + ('R' - 'A'):               /* PE_PostOrderListReorder */
        order_reorder_patterns();
        return 1;
    case 'g': case 'G':                         /* PE_PostOrderList24 */
    case ITK_ENTER:
        PEOrder = o;
        if (Song.Orders[o] < 200) {
            commit_current_pattern();
            load_pattern(Song.Orders[o]);
            Screen = SCR_PATTERN;
        }
        return 1;
    default:
        break;
    }

    /* digit entry (PE_PostOrderList11/15): replace one decimal digit of
     * the pattern number, clamp to 199, then advance the digit cursor
     * (order advances after the units digit) */
    if (key >= '0' && key <= '9') {
        int cur = Song.Orders[o];
        int dig[3], val;

        if (cur > 199)
            cur = 0;
        dig[0] = cur / 100;
        dig[1] = (cur / 10) % 10;
        dig[2] = cur % 10;
        dig[OrderCursor] = key - '0';
        val = dig[0] * 100 + dig[1] * 10 + dig[2];
        if (val > 199)
            val = 199;
        ed_lock();
        Song.Orders[o] = (uint8_t)val;
        ed_unlock();
        if (OrderCursor < 2) {
            OrderCursor++;
        } else if (ListSel < 255) {
            OrderCursor = 0;
            ListSel++;
        }
        return 1;
    }
    return 0;
}

static void order_list_lclick(int row, int mx, int mpx)
{
    (void)mpx;
    if (row >= 0 && OrdListTop + row <= 255) {
        ListSel = OrdListTop + row;
        OrderCursor = (mx >= 6 && mx <= 8) ? mx - 6 : 0;
    }
}

/* F11 pan columns: Up/Down select channel, Left/Right slide the pan,
 * L/M/R hard positions, S surround, Space mute toggle (the original's
 * pan thumbbar keys) */
static void pan_adjust(int chan, int delta)
{
    uint8_t pan = Song.Header.ChnlPan[chan];
    int p = pan & 0x7F;

    if (p > 64)                     /* leaving surround: re-centre */
        p = 32;
    p += delta;
    if (p < 0) p = 0;
    if (p > 64) p = 64;
    ed_lock();
    Song.Header.ChnlPan[chan] = (uint8_t)((pan & 0x80) | p);
    ed_unlock();
}

static void pan_set(int chan, int p)
{
    ed_lock();
    Song.Header.ChnlPan[chan] =
        (uint8_t)((Song.Header.ChnlPan[chan] & 0x80) | p);
    ed_unlock();
}

static int pan_col_lkey(int key, int base)
{
    if (PanSel < base || PanSel >= base + 32)
        PanSel = base;

    switch (key) {
    case ITK_UP:   if (PanSel > base) PanSel--; return 1;
    case ITK_DOWN: if (PanSel < base + 31) PanSel++; return 1;
    case ITK_PGUP: PanSel -= 8; if (PanSel < base) PanSel = base; return 1;
    case ITK_PGDN: PanSel += 8;
                   if (PanSel > base + 31) PanSel = base + 31;
                   return 1;
    case ITK_HOME: PanSel = base; return 1;
    case ITK_END:  PanSel = base + 31; return 1;
    case ITK_LEFT:  pan_adjust(PanSel, -1); return 1;
    case ITK_RIGHT: pan_adjust(PanSel,  1); return 1;
    case ITK_SHIFT_LEFT:  pan_adjust(PanSel, -4); return 1;   /* issue #7 */
    case ITK_SHIFT_RIGHT: pan_adjust(PanSel,  4); return 1;
    case ITK_CTRL_LEFT:   pan_adjust(PanSel, -2); return 1;
    case ITK_CTRL_RIGHT:  pan_adjust(PanSel,  2); return 1;
    case 'l': case 'L': pan_set(PanSel, 0);   return 1;
    case 'm': case 'M': pan_set(PanSel, 32);  return 1;
    case 'r': case 'R': pan_set(PanSel, 64);  return 1;
    case 's': case 'S': pan_set(PanSel, 100); return 1;
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': {
        int v;                      /* the pan bars are type-9 thumbbars */
        if (thumb_value_dialog(key, 0, 64, &v, draw_screen))
            pan_set(PanSel, v);
        return 1; }
    case ' ':
        ed_lock();
        Song.Header.ChnlPan[PanSel] ^= 0x80;
        ed_unlock();
        return 1;
    default: break;
    }
    return 0;
}

static int pan_left_lkey(int key)  { return pan_col_lkey(key, 0); }
static int pan_right_lkey(int key) { return pan_col_lkey(key, 32); }

/* The pan columns are ordinary 0..64 thumbbars (issue #10), so the
 * pointer maps like every other thumbbar (thumb_from_px): one logical
 * pixel per step from 4px into the bar. A click inside a bar also starts
 * a drag that the main loop follows while the button is held (issue #8
 * -- the pan bars are drawn inside list widgets, which the generic
 * thumbbar drag never covered). */
static int PanDragChan = -1, PanDragBx;

static void pan_from_px(int chan, int bx, int mpx)
{
    int p = mpx - (bx * 8 + 4);
    if (p < 0) p = 0;
    if (p > 64) p = 64;
    pan_set(chan, p);
}

static void pan_col_lclick(int row, int mx, int mpx, int base, int bx)
{
    if (row < 0 || row > 31)
        return;
    PanSel = base + row;
    if (mx >= bx && mx <= bx + 8) {         /* inside the slider */
        pan_from_px(PanSel, bx, mpx);
        PanDragChan = PanSel;
        PanDragBx = bx;
    }
}

static void pan_left_lclick(int row, int mx, int mpx)
{
    pan_col_lclick(row, mx, mpx, 0, 31);
}

static void pan_right_lclick(int row, int mx, int mpx)
{
    pan_col_lclick(row, mx, mpx, 32, 65);
}

/* ---- widget action callbacks ---- */
static void act_stereo_changed(void)
{
    ed_lock();
    Music_InitStereo();
    ed_unlock();
}

/* F_SetControlInstrument (IT_F.ASM 4810): the F12 "Instruments" radio.
 * The flag bit is already set by button_press; offer to initialise the
 * instruments from the samples (O1_InitialiseInstrumentList prompt). On
 * yes, reset all instruments to the default template, then for each
 * populated sample copy its name into the matching instrument and map all
 * 120 notes to that sample. Declining leaves instrument mode on with the
 * existing instruments untouched. */
static void act_enable_instruments(void)
{
    int s, n;

    if (!confirm_box("Initialise instruments?"))
        return;

    ed_lock();
    Music_ClearAllInstruments();
    for (s = 0; s < MAX_SAMPLES - 1; s++) {     /* samples 1..99 */
        sample_t     *smp = &Song.Smp[s];
        instrument_t *in  = &Song.Ins[s];

        if (!(smp->Flags & 1))                  /* sample present? */
            continue;
        memcpy(in->InstrumentName, smp->SampleName, sizeof(in->InstrumentName));
        for (n = 0; n < 120; n++)
            in->NoteSampleTable[n * 2 + 1] = (uint8_t)(s + 1);
    }
    ed_unlock();
    status("Instruments initialised from samples.");
}

/* "Initial Tempo" is the song's start tempo (re-seeded into the runtime
 * Tempo by Music_Stop on every play). While stopped, mirror it into the
 * runtime Tempo so the header display stays consistent (as for Speed);
 * while playing, leave the live Txx-driven Tempo alone. */
static void act_tempo_changed(void)
{
    ed_lock();
    if (PlayMode == 0) {
        Tempo = (uint8_t)Song.Header.IT;
        Music_InitTempo();
    }
    ed_unlock();
}

static void act_speed_changed(void)
{
    ed_lock();
    if (PlayMode == 0)
        CurrentSpeed = Song.Header.IS;
    ed_unlock();
}

/* Global Volume is the live mixing scalar (GlobalVolume); push the edit
 * straight into it so it takes effect while playing, not just on next
 * play. Music_SetGlobalVolume also flags all channels for vol recalc. */
static void act_gv_changed(void)
{
    ed_lock();
    Music_SetGlobalVolume((uint8_t)Song.Header.GV);
    ed_unlock();
}

/* Mixing Volume lives in the driver; re-push it (Music_Stop does not). */
static void act_mv_changed(void)
{
    ed_lock();
    Music_InitMixTable();
    ed_unlock();
}

static void act_help_done(void)             /* H_HelpESC */
{
    HelpPositions[HelpContext] = HelpTop;
    Screen = HelpReturnScreen;
}

static void audio_pref_save(FILE *fp);

static void act_save_prefs(void)
{
    FILE *fp = fopen("ited.cfg", "w");
    if (!fp) {
        status("Can't write ited.cfg here.");
        return;
    }
    audio_pref_save(fp);
    fprintf(fp, "moduledir=%s\nsampledir=%s\ninstrdir=%s\n"
            "octave=%d\nstep=%d\n"
            "peconfig=%d\nviewdivision=%d\nviewtracking=%d\n"
            "keyboard_cfg=%s\n",
            DirModule, DirSample, DirInstr, BaseOctave, EditStep,
            PEConfig, ViewDivision, ViewTracking, KeyboardCfg);
    fclose(fp);
    status("Preferences saved to ited.cfg.");
}

/* ===================================================================
 * File requester (F9) -- IT_F.ASM "Load Module (F9)" screen layout
 * =================================================================== */
#define REQ_MAXFILES 1024
#define REQ_MAXDIRS  256
typedef struct reqfile_t {
    char     name[64];
    char     songname[27];
    long     size;
    /* the original's file record (D_LoadModuleFiles / IT_D_INF.INC):
     * +23 type (FormatNames index), +22 MOD channels, +0/+2 DOS time /
     * date of the directory entry */
    int      type;
    uint8_t  chans;
    uint16_t dtime, ddate;
} reqfile_t;

static reqfile_t ReqFiles[REQ_MAXFILES];
static char      ReqDirs[REQ_MAXDIRS][64];
static char      ReqDrives[26];
static int       ReqNF, ReqND, ReqNDrv;
static int       ReqFocus;          /* 0 files, 1 dirs, 2 drives, 3 name */
static int       FSel, FTop, DSel, DTop, VSel;
/* FileSpecifier, 64 characters (FileNamePrompt), FileSpecifierDefault */
static char      ReqName[65] = "*.IT, *.XM, *.S3M, *.MTM, *.669, *.MOD";

static int req_file_cmp(const void *a, const void *b)
{
    return strcmp(((const reqfile_t *)a)->name,
                  ((const reqfile_t *)b)->name);
}

static int req_dir_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* 0 = module load/save (F9/F10), 1 = sample library (F3 Enter),
 * 2 = instrument library (F4 Enter) */
static int ReqLibMode;

static int has_it_ext(const char *name)
{
    if (ReqLibMode == 1)
        return RIS_KnownExt(name);
    if (ReqLibMode == 2)
        return RI_KnownExt(name);
    return Import_KnownExt(name);   /* .IT/.S3M/.XM/.MOD/.MTM/.669 */
}

/* D_GetSongNameModuleType (IT_D_INF.INC 3): the FormatNames type and
 * the song name, from the file's first bytes. Kept 1:1, including the
 * MTM case falling through to type 1 ("Unknown module format") after
 * copying its name -- the original has no Ret there. */
static void req_read_songname(reqfile_t *f)
{
    uint8_t h[1084];
    size_t n;
    FILE *fp = fopen(f->name, "rb");

    memset(f->songname, 0, sizeof(f->songname));
    f->type = 1;
    f->chans = 0;
    if (!fp)
        return;
    memset(h, 0, sizeof(h));                /* D_LoadFileHeader clears */
    n = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    (void)n;

    if (!memcmp(h, "IMPM", 4)) {
        int cmwt = h[0x2A] | (h[0x2B] << 8);
        f->type = cmwt > 0x217 ? 3 : cmwt < 0x214 ? 2 : 7;
        memcpy(f->songname, h + 4, 25);
        return;
    }
    if (!memcmp(h + 44, "SCRM", 4)) {
        f->type = 4;
        memcpy(f->songname, h, 25);
        return;
    }
    if (!memcmp(h, "Extended Module: ", 17)) {
        f->type = 5;
        memcpy(f->songname, h + 17, 20);
        return;
    }
    if (!memcmp(h, "if", 2) || !memcmp(h, "JN", 2)) {
        f->type = 6;
        memcpy(f->songname, h + 2, 25);
        return;
    }
    {
        const uint8_t *m = h + 1080;
        int t = 0;
        if (!memcmp(m, "M.K.", 4))      t = 9;
        else if (!memcmp(m, "M!K!", 4)) t = 10;
        else if (!memcmp(m, "4CHN", 4)) t = 11;
        else if (!memcmp(m, "6CHN", 4)) t = 12;
        else if (!memcmp(m, "8CHN", 4)) t = 13;
        else if (m[2] == 'C' && m[3] == 'H' && m[0] >= '0' && m[0] <= '9' &&
                 m[1] >= '0' && m[1] <= '9') {
            t = 17;
            f->chans = (uint8_t)((m[0] - '0') * 10 + (m[1] - '0'));
        }
        else if (!memcmp(m, "FLT4", 4)) t = 14;
        else if (h[471] == 0x78)        t = 16;
        if (t) {
            f->type = t;
            memcpy(f->songname, h, 20);
            return;
        }
    }
    if (!memcmp(h, "MTM", 3))
        memcpy(f->songname, h + 4, 20);     /* ... then type 1 (sic) */
    f->type = 1;
}

/* FormatNames (IT_DISK.ASM 523) */
static const char *const ReqFormatNames[19] = {
    "Unchecked", "Unknown module format", "Impulse Tracker",
    "Impulse Tracker ?.??", "Scream Tracker 3", "Fast Tracker 2 Module",
    "Composer 669 Module", "Compressed Impulse Tracker", "",
    "Amiga-NewTracker", "Amiga-ProTracker", "4 Channel MOD",
    "6 Channel MOD", "8 Channel MOD", "4 Channel Startrekker",
    "8 Channel Startrekker", "Old Amiga-MOD format ? ", NULL /* %d */,
    "MultiTracker Module"
};

static void req_scan(void)
{
    ReqNF = ReqND = ReqNDrv = 0;

#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("*", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, "."))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (ReqND < REQ_MAXDIRS)
                        snprintf(ReqDirs[ReqND++], sizeof(ReqDirs[0]),
                                 "%s", fd.cFileName);
                } else if (has_it_ext(fd.cFileName) &&
                           ReqNF < REQ_MAXFILES) {
                    reqfile_t *f = &ReqFiles[ReqNF++];
                    FILETIME lt;
                    WORD dd = 0, dt = 0;
                    snprintf(f->name, sizeof(f->name), "%s", fd.cFileName);
                    f->size = (long)fd.nFileSizeLow;
                    if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt))
                        FileTimeToDosDateTime(&lt, &dd, &dt);
                    f->ddate = dd;
                    f->dtime = dt;
                    req_read_songname(f);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    {
        DWORD drives = GetLogicalDrives();
        int i;
        for (i = 0; i < 26; i++)
            if (drives & (1u << i))
                ReqDrives[ReqNDrv++] = (char)('A' + i);
    }
#else
    {
        DIR *d = opendir(".");
        struct dirent *e;
        if (d) {
            while ((e = readdir(d))) {
                struct stat st;
                if (!strcmp(e->d_name, "."))
                    continue;
                if (stat(e->d_name, &st))
                    continue;
                if (S_ISDIR(st.st_mode)) {
                    if (ReqND < REQ_MAXDIRS)
                        snprintf(ReqDirs[ReqND++], sizeof(ReqDirs[0]),
                                 "%.*s", (int)sizeof(ReqDirs[0]) - 1, e->d_name);
                } else if (has_it_ext(e->d_name) && ReqNF < REQ_MAXFILES) {
                    reqfile_t *f = &ReqFiles[ReqNF++];
                    struct tm tmv, *t;
                    snprintf(f->name, sizeof(f->name),
                             "%.*s", (int)sizeof(f->name) - 1, e->d_name);
                    f->size = (long)st.st_size;
                    t = localtime_r(&st.st_mtime, &tmv);
                    f->ddate = t ? (uint16_t)(((t->tm_year + 1900 - 1980) << 9) |
                                              ((t->tm_mon + 1) << 5) | t->tm_mday) : 0;
                    f->dtime = t ? (uint16_t)((t->tm_hour << 11) |
                                              (t->tm_min << 5) | (t->tm_sec / 2)) : 0;
                    req_read_songname(f);
                }
            }
            closedir(d);
        }
    }
#endif

    qsort(ReqFiles, (size_t)ReqNF, sizeof(ReqFiles[0]), req_file_cmp);
    qsort(ReqDirs, (size_t)ReqND, sizeof(ReqDirs[0]), req_dir_cmp);
    FSel = FTop = DSel = DTop = 0;
}

static int has_ext_ci(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext);
    size_t i;

    if (nl < el)
        return 0;
    for (i = 0; i < el; i++) {
        char a = name[nl - el + i], b = ext[i];
        if (a >= 'a' && a <= 'z')
            a = (char)(a - 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* lazy file-format classification for the info box (the original's
 * on-cursor D_LoadFileHeader check; names from FormatNames /
 * SampleFormatNames) */
static int  ReqInfoIdx = -1;
static char ReqInfoFmt[40];

static const char *req_sniff_format(void)
{
    uint8_t h[1084];
    size_t got = 0;
    FILE *f;

    if (ReqInfoIdx == FSel)
        return ReqInfoFmt;
    ReqInfoIdx = FSel;
    snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Unknown format");

    f = fopen(ReqFiles[FSel].name, "rb");
    if (f) {
        memset(h, 0, sizeof(h));
        got = fread(h, 1, sizeof(h), f);
        fclose(f);
    }
    if (got >= 4 && !memcmp(h, "IMPM", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Impulse Tracker");
    else if (got >= 4 && !memcmp(h, "IMPS", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Impulse Tracker Sample");
    else if (got >= 4 && !memcmp(h, "IMPI", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt),
                 "Impulse Tracker Instrument");
    else if (got >= 21 && !memcmp(h, "Extended Instrument: ", 21))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt),
                 "Fast Tracker 2 Instrument");
    else if (got >= 17 && !memcmp(h, "Extended Module: ", 17))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Fast Tracker 2 Module");
    else if (got >= 0x30 && !memcmp(h + 0x2C, "SCRM", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Scream Tracker 3");
    else if (got >= 3 && !memcmp(h, "MTM", 3))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "MultiTracker Module");
    else if (got >= 2 && (!memcmp(h, "if", 2) || !memcmp(h, "JN", 2)))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Composer 669 Module");
    else if (got >= 48 && !memcmp(h + 44, "PTMF", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Poly Tracker Module");
    else if (got >= 4 && !memcmp(h, "FAR\xFE", 4))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Farandole Module");
    else if (got >= 22 && !memcmp(h, "GF1PATCH110\0ID#000002", 22))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt),
                 "Gravis UltraSound Patch");
    else if (has_ext_ci(ReqFiles[FSel].name, ".KRZ"))
        snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Kurzweil Synth File");
    else if (got >= 1084) {
        uint8_t *m = h + 1080;
        if (!memcmp(m, "M.K.", 4) || !memcmp(m, "M!K!", 4))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "Amiga-ProTracker");
        else if (!memcmp(m, "FLT4", 4))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt),
                     "4 Channel Startrekker");
        else if (!memcmp(m, "OCTA", 4) || !memcmp(m, "CD81", 4))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "8 Channel MOD");
        else if (m[0] >= '1' && m[0] <= '9' && !memcmp(m + 1, "CHN", 3))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "%c Channel MOD",
                     m[0]);
        else if (m[0] >= '1' && m[0] <= '9' && m[1] >= '0' &&
                 m[1] <= '9' && !memcmp(m + 2, "CH", 2))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt), "%c%c Channel MOD",
                     m[0], m[1]);
        else if (has_ext_ci(ReqFiles[FSel].name, ".MOD"))
            snprintf(ReqInfoFmt, sizeof(ReqInfoFmt),
                     "Old Amiga-MOD format ? ");
    }
    return ReqInfoFmt;
}

static const uint8_t SearchText[] =
    "Search\015\015\015Format\015  Size\015  Date\015  Time";
static const uint8_t FileText[] = " Filename\015Directory";

static int do_load_named(const char *path);
static int load_module_screen(const char *path);

static int ReqSave;                     /* 0 = load (F9), 1 = save (F10) */

static void req_draw_string(int x, int y, const char *s, int focused)
{
    int i;
    for (i = 0; s[i]; i++) {
        uint8_t c = (uint8_t)s[i];
        Screen_PutChar(x + i, y, c >= 226 ? ' ' : c, 0x02);
    }
    if (focused)
        Screen_SetAttr(x + i, y, 0x30);
}

static void draw_file_requester(void)
{
    int i;

    Screen_Clear(0x20);
    draw_chrome(ReqLibMode == 1 ? "Load Sample" :
                ReqLibMode == 2 ? "Load Instrument" :
                ReqSave ? "Save Module (F10)" : "Load Module (F9)");

    Screen_DrawBox(2, 12, 41, 44, 27);          /* FileBox */
    Screen_DrawBox(43, 12, 56, 34, 27);         /* DirBox */
    Screen_DrawBox(58, 12, 67, 34, 27);         /* DriveBox */
    Screen_DrawBox(50, 36, 77, 38, 27);         /* SearchBox */
    Screen_DrawBox(50, 39, 77, 44, 27);         /* FileInfoBox */
    Screen_DrawBox(12, 45, 77, 48, 27);         /* FileNameBox */
    Screen_DrawStringCtl(44, 37, SearchText, 0x20, NULL);
    Screen_DrawStringCtl(3, 46, FileText, 0x20, NULL);

    /* D_DrawFileWindow (IT_DISK.ASM 1762): 31 rows from (3,13) -- file
     * name (12 cells, colour by type with FileColours on), the 2A8h
     * divider at column 15 on every row, the song name from column 16
     * (25 cells, colour 2 + FileColours) */
    if (FSel < FTop) FTop = FSel;
    if (FSel >= FTop + 31) FTop = FSel - 30;
    if (FTop < 0) FTop = 0;
    for (i = 0; i < 31; i++) {
        int idx = FTop + i, k;
        if (idx < ReqNF) {
            const reqfile_t *f = &ReqFiles[idx];
            int t = f->type;
            uint8_t a = t == 0 ? 6 : t == 1 ? 7 : (t <= 3 || t == 7) ? 3
                      : t <= 8 ? 5 : 2;            /* FileColours = 1 */
            int end = 0;
            for (k = 0; k < 12; k++) {
                uint8_t c = end ? 0 : (uint8_t)f->name[k];
                if (!c)
                    end = 1;
                Screen_PutChar(3 + k, 13 + i, c, a);
            }
        }
        Screen_PutChar(15, 13 + i, 0xA8, 0x02);     /* 2A8h */
        if (idx < ReqNF)
            for (k = 0; k < 25; k++) {
                uint8_t c = (uint8_t)ReqFiles[idx].songname[k];
                Screen_PutChar(16 + k, 13 + i, c >= 226 ? ' ' : c, 0x03);
            }
    }
    if (ReqNF == 0)
        Screen_DrawString(3, 13, "No files.", 0x07);   /* NoFilesMsg */
    for (i = 0; i < 13; i++)                    /* the Search string */
        Screen_PutChar(51 + i, 37, 0, 0x05);
    if (ReqFocus == 0) {                        /* D_PreFileWindow */
        int y = 13 + FSel - FTop;
        if (ReqNF)
            for (i = 0; i < 38; i++)
                Screen_SetAttr(3 + i, y, i == 12 ? 0x32 : 0x30);
        Screen_SetAttr(51, 37, 0x60);           /* CurrentSearchPos 0 */
    }

    /* D_DrawDirectoryWindow: 21 rows from (44,13), colour 5; "No dirs." */
    if (DSel < DTop) DTop = DSel;
    if (DSel >= DTop + 21) DTop = DSel - 20;
    if (DTop < 0) DTop = 0;
    for (i = 0; i < 21 && DTop + i < ReqND; i++)
        Screen_DrawString(44, 13 + i, ReqDirs[DTop + i], 0x05);
    if (ReqND == 0)
        Screen_DrawString(44, 13, "No dirs.", 0x07);
    if (ReqFocus == 1 && ReqND)                 /* D_PreDirectoryWindow */
        for (i = 0; i < 12; i++)
            Screen_SetAttr(44 + i, 13 + DSel - DTop, 0x30);

    /* D_DrawDriveWindow: "Drive X:" from (59,13), colour 5 */
    if (VSel >= ReqNDrv) VSel = ReqNDrv ? ReqNDrv - 1 : 0;
    for (i = 0; i < 21 && i < ReqNDrv; i++)
        drawf(59, 13 + i, 0x05, "Drive %c:", ReqDrives[i]);
    if (ReqFocus == 2 && ReqNDrv)               /* D_PreDriveWindow */
        for (i = 0; i < 8; i++)
            Screen_SetAttr(59 + i, 13 + VSel, 0x30);

    /* the file info box (D_DrawFileWindow12..): format, size, date and
     * time of the current file, all in colour 5 from column 51 */
    if (ReqNF && FSel < ReqNF) {
        const reqfile_t *f = &ReqFiles[FSel];
        if (ReqLibMode) {
            Screen_DrawString(51, 40, req_sniff_format(), 0x05);
        } else if (f->type >= 0 && f->type < 19) {
            if (f->type == 17)                  /* ChannelXX */
                drawf(51, 40, 0x05, "%d Channel MOD", f->chans);
            else
                Screen_DrawString(51, 40, ReqFormatNames[f->type], 0x05);
        }
        if ((unsigned long)f->size / 65536UL < 10000UL)
            drawf(51, 41, 0x05, "%09ld", f->size);
        {
            static const char *const mon[16] = {
                "", "January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November",
                "December", "", "", "" };
            const char *m = mon[(f->ddate >> 5) & 15];
            int x = 51 + (int)strlen(m), h = f->dtime >> 11, pm = 0;
            Screen_DrawString(51, 42, m, 0x05);
            Screen_PutChar(x, 42, 0, 0x05);     /* the string's 0 is stored */
            drawf(x + 1, 42, 0x05, "%d, %d", f->ddate & 31,
                  ((f->ddate >> 9) & 0x7F) + 1980);
            if (h >= 12) { pm = 1; h -= 12; }
            if (h == 0) h = 12;
            drawf(51, 43, 0x05, "%d:%02d%cm", h, (f->dtime >> 5) & 63,
                  pm ? 'p' : 'a');
        }
    }

    /* save-format radio buttons (O1_SaveModuleList objects 17..20:
     * style 8 at (69,12)..(77,23), bound to SaveFormat) */
    if (ReqSave) {
        static const char *const fmtlbl[4] =
            { " IT214", "  S3M", " IT2xx", " IT215" };
        static const uint8_t fmtof[4] = { 0, 1, 2, 3 };
        for (i = 0; i < 4; i++)
            draw_button_style(69, 12 + 3 * i, 77, 14 + 3 * i, 8,
                              fmtlbl[i], SaveFormat == fmtof[i],
                              ReqFocus == 4 && SaveFormat == fmtof[i]);
    }

    /* FileNamePrompt (13,46) / SongDirectoryPrompt (13,47): string
     * inputs, F_DrawStringInput -- the text in colour 2 (characters from
     * 226 up as spaces), the rest of the field keeps the box colour;
     * F_PreStringInput puts the 30h cursor after the text */
    req_draw_string(13, 46, ReqName, ReqFocus == 3);
    {
        char cwd[256] = "";
        if (!getcwd(cwd, sizeof(cwd)))  /* on failure keep the "" fallback */
            cwd[0] = '\0';
        cwd[64] = 0;                    /* SongDirectory: 64 characters */
        req_draw_string(13, 47, cwd, 0);
    }
}

/* D_CheckOverWrite's confirm dialog (O1_ConfirmOverWriteList, default
 * object 4 = Cancel): "Overwrite file?" in the shared confirm box with
 * OK/Cancel -- it had kept the old flat Yes/No look. `bg` redraws the
 * screen beneath the modal. */
static int confirm_overwrite(void (*bg)(void))
{
    return confirm_box_bg("Overwrite file?", 0, bg);
}

/* O1_StereoSampleList (feature 013): the "Loading Stereo Sample"
 * Left/Right requester, drawn over the current screen as the original
 * overlays the loader. Returns 64 (left) or 64+128 (right). */
static int stereo_choice_prompt(void)
{
    int sel = 0;                        /* 0 = Left, 1 = Right */

    for (;;) {
        int key;

        Screen_DrawBox(26, 22, 54, 29, 3);
        Screen_DrawString(30, 24, "Loading Stereo Sample", 0x20);
        draw_button_style(30, 26, 39, 28, 8, "  Left", 0, sel == 0);
        draw_button_style(40, 26, 50, 28, 8, "  Right", 0, sel == 1);
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 64;
        case 'l': case 'L': case ITK_ESC:
            return 64;
        case 'r': case 'R':
            return 64 + 128;
        case ITK_LEFT: case ITK_RIGHT: case ITK_TAB: case ITK_SHIFT_TAB:
            sel ^= 1;
            break;
        case ITK_ENTER: case ' ':
            return sel ? 64 + 128 : 64;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (m.y >= 26 && m.y <= 28) {
                if (m.x >= 30 && m.x <= 39) return 64;
                if (m.x >= 40 && m.x <= 50) return 64 + 128;
            }
            break; }
        default:
            break;
        }
    }
}

/* the original's save progress strings at (4,17..23), attr 5 */
static void save_progress_draw(int stage, int param)
{
    switch (stage) {
    case 0: drawf(4, 17, 5, "File Header"); break;
    case 1: drawf(4, 18, 5, "Instrument Headers"); break;
    case 2: drawf(4, 19, 5, "Sample Headers"); break;
    case 3: drawf(4, 20, 5, "Pattern %d", param); break;
    case 4: drawf(4, 21, 5, "Sample %d", param); break;
    case 5: drawf(4, 23, 5, "Done"); break;
    }
    Screen_Update();
}

/* the original's S3M format-limit warnings, rows 23..33 attr 4 */
static void save_s3m_warning_draw(int row, const char *msg)
{
    drawf(4, row, 4, "%s", msg);
    Screen_Update();
}

/* D_SaveS3M tail: when any format warning fired, hold the screen
 * until a key is pressed (K_ClearKeyboardQueue + K_GetKey wait) */
static int SaveNoKeyWait = 0;           /* selftest only (feature 016) */

static void save_s3m_keywait(void)
{
    if (!Save_S3MWarned || SaveNoKeyWait)
        return;
    while (ed_get_key() != ITK_NONE)
        ;                               /* clear the queue */
    for (;;) {
        int key;
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_QUIT) { Running = 0; return; }
        if (key != ITK_NONE)
            return;
        ma_sleep(15);
    }
}

/* dispatch by SaveFormat (D_PostFileSaveWindow3) */
static int save_module_dispatch(const char *name)
{
    int ok;

    Save_Progress = save_progress_draw;
    if (SaveFormat == 1) {
        Save_S3MWarning = save_s3m_warning_draw;
        ok = Save_S3MModule(name);
        save_s3m_keywait();
        Save_S3MWarning = NULL;
    } else {
        Song.Header.PHiligt = (uint16_t)(RowHiLight1 | (RowHiLight2 << 8));
        ok = Save_ITModule(name);
    }
    Save_Progress = NULL;
    return ok;
}

/* D_SaveModule tail + D_PostFileSaveWindow2: apply .IT/.S3M by format
 * when no '.', confirm overwrite, run the writer, report. */
static void req_do_save(int *done)
{
    char name[40];
    FILE *f;

    snprintf(name, sizeof(name), "%s", ReqName);
    if (!name[0])
        return;
    if (strchr(name, '*') || strchr(name, '?')) {
        req_scan();                     /* wildcard: new file spec */
        return;
    }
    if (!strchr(name, '.') && strlen(name) < sizeof(name) - 5)
        strcat(name, SaveFormat == 1 ? ".S3M" : ".IT");

    f = fopen(name, "rb");
    if (f) {
        fclose(f);
        if (!confirm_overwrite(draw_file_requester))
            return;
    }

    commit_current_pattern();           /* PE_SaveCurrentPattern */
    if (save_module_dispatch(name)) {
        char *q;
        snprintf(FileNameDisp, sizeof(FileNameDisp), "%s", name);
        for (q = FileNameDisp; *q; q++)
            if (*q >= 'a' && *q <= 'z')
                *q = (char)(*q - 32);
        FileSaveName[0] = 0;
        status("Saved.");
        *done = 1;
    } else {
        status("Unable to save file");  /* O1_UnableToSaveList */
    }
}

/* ===================================================================
 * Sample / instrument library (feature 006): LSWindow_Enter /
 * LIWindow_Enter over the requester chrome. Entering a module scans
 * it into records; entry 0 is the ExitLibraryDirectory row.
 * =================================================================== */
#define LIB_MAX     200
#define CHECK_SLOT  99                  /* sample 100: preview slot */

static slibent_t SLib[LIB_MAX];
static ilibent_t ILib[LIB_MAX];
static int LibN;                        /* records in the open library */
static int LibSel, LibTop;
static int LibCheckIdx = -1;            /* record loaded into slot 99  */
static int LibUnused;                   /* UnusedSamples at open (F4)  */

static void lib_release_check(void)
{
    ed_lock();
    free(Song.Smp[CHECK_SLOT].Data);
    memset(&Song.Smp[CHECK_SLOT], 0, sizeof(sample_t));
    ed_unlock();
}

/* note keys audition the selected record through the check slot
 * (D_PostLoadSampleWindow -> LoadSample(99) + Music_PlaySample) */
static void lib_preview_key(const slibent_t *e, int idx, int key)
{
    int gn = key_to_note_plain();
    sample_t tmp;

    if (gn <= 0)
        return;
    if (LibCheckIdx != idx) {
        memset(&tmp, 0, sizeof(tmp));
        if (!RIS_LoadSample(e, &tmp))
            return;
        stop_song();
        ed_lock();
        free(Song.Smp[CHECK_SLOT].Data);
        Song.Smp[CHECK_SLOT] = tmp;
        ed_unlock();
        LibCheckIdx = idx;
    }
    ed_lock();
    Music_PlaySample((uint8_t)(gn - 1), CHECK_SLOT + 1, 40);
    ed_unlock();
}

/* load the selected sample record into the current F3 slot. No
 * overwrite prompt: the original has none (LSWindow_EnterSample,
 * IT_DISK.ASM 7297). The extra "Replace sample?" box that features
 * 006-013 showed defaulted to Cancel, so Enter,Enter aborted the load
 * and users learned to answer Y,Y -- the second Y then created a new
 * host instrument on every replacement. */
static int lib_load_sample_entry(const slibent_t *e)
{
    sample_t *dst = &Song.Smp[ListSel];
    sample_t tmp;
    int mkins = 0;
    /* LSWindow_Enter (IT_DISK.ASM 7297): in instrument mode, offer to
     * host the sample in an instrument -- default Yes when the slot had
     * no sample, No when one is being replaced */
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        mkins = confirm_box_def("Create host instrument?",
                                !(dst->Flags & 1));
    memset(&tmp, 0, sizeof(tmp));
    if (!RIS_LoadSample(e, &tmp)) {
        status("Unable to load sample.");
        return 0;
    }
    stop_song();
    ed_lock();
    free(dst->Data);
    *dst = tmp;
    if (ListSel >= Song.Header.SmpNum)
        Song.Header.SmpNum = (uint16_t)(ListSel + 1);
    ed_unlock();
    if (mkins) {
        int in;
        ed_lock();
        in = Music_AssignSampleToInstrument(ListSel);
        ed_unlock();
        if (in)
            status("Sample assigned to Instrument %d", in);
        else
            status("Error: No available Instruments!");
        return 1;
    }
    status("Sample %d loaded.", ListSel + 1);
    return 1;
}

/* load an instrument record into the current F4 slot (LIWindow_Enter
 * codes 3..6): out-of-slots check first, then the transfer */
static int lib_load_instrument_entry(const ilibent_t *e)
{
    instrument_t *dst = &Song.Ins[ListSel];
    int occupied = dst->InstrumentName[0] != 0;
    int k, r;
    char msg[64];

    for (k = 0; k < 120 && !occupied; k++)
        if (dst->NoteSampleTable[k * 2 + 1])
            occupied = 1;

    if (e->NumSamples > LibUnused) {    /* O1_OutOfSamplesList */
        status("Out of sample space! (%d needed, %d available)",
               e->NumSamples, LibUnused);
        return 0;
    }
    if (occupied) {
        snprintf(msg, sizeof(msg), "Replace instrument %d?", ListSel + 1);
        if (!confirm_box(msg))
            return 0;
    }

    stop_song();
    ed_lock();
    r = RI_LoadInstrument(e, ListSel);
    if (r == RI_OK) {
        int i;
        if (ListSel >= Song.Header.InsNum)
            Song.Header.InsNum = (uint16_t)(ListSel + 1);
        for (i = 99; i > Song.Header.SmpNum; i--) {
            if (Song.Smp[i - 1].Flags & 1) {
                Song.Header.SmpNum = (uint16_t)i;
                break;
            }
        }
    }
    ed_unlock();

    if (r != RI_OK) {
        status("Unable to load instrument.");
        return 0;
    }
    LibUnused = RI_UnusedSamples();

    if (!(Song.Header.Flags & ITF_INSTRUMENTS) &&
        confirm_box("Enable instrument mode?")) {   /* O1_EnableInstrumentMode */
        ed_lock();
        Song.Header.Flags |= ITF_INSTRUMENTS;
        ed_unlock();
    }
    status("Instrument %d loaded (%d samples).", ListSel + 1,
           e->NumSamples);
    return 1;
}

static void draw_lib_browser(int inslib, const char *srcname)
{
    int rows = 35, i;

    const char *base = srcname, *p;

    for (p = srcname; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    Screen_Clear(0x20);
    draw_chrome(inslib ? "Load Instrument" : "Load Sample");
    drawf(2, 10, 0x20, "In %-13.13s", base);
    if (inslib)
        drawf(56, 10, 0x20, "Available Samples: %d", LibUnused);
    Screen_DrawBox(2, 12, 77, 48, 27);

    if (LibSel < LibTop) LibTop = LibSel;
    if (LibSel >= LibTop + rows) LibTop = LibSel - rows + 1;
    if (LibTop < 0) LibTop = 0;

    for (i = 0; i < rows && LibTop + i <= LibN; i++) {
        int idx = LibTop + i;
        int y = 13 + i;
        uint8_t a = (idx == LibSel) ? 0x30 : 0x03;

        drawf(3, y, (idx == LibSel) ? a : 0x02, "%3d", idx + 1);
        if (idx == 0) {                 /* ExitLibraryDirectory row */
            int x;
            for (x = 0; x < 8; x++) {
                Screen_PutChar(8 + x, y, 154, a);
                Screen_PutChar(25 + x, y, 154, a);
            }
            Screen_DrawString(16, y, "Directory", a);
        } else if (!inslib) {
            const slibent_t *e = &SLib[idx - 1];
            draw_itname(8, y, e->hdr.SampleName, 26, a);
            drawf(36, y, a, "%-24.24s", RIS_FormatName(e->Format));
            drawf(62, y, a, "%9u", e->hdr.Length);
        } else {
            const ilibent_t *e = &ILib[idx - 1];
            draw_itname(8, y, e->Name, 26, a);
            drawf(36, y, a, "%-27.27s", RI_FormatName(e->Format));
            if (e->NumSamples == 0)
                drawf(64, y, a, "No Samples");
            else if (e->NumSamples == 1)
                drawf(64, y, a, "1 Sample");
            else
                drawf(64, y, a, "%d Samples", e->NumSamples);
        }
    }
    Screen_Update();
}

/* browse an opened library; returns 1 when something was loaded
 * (leave the requester), 0 = back to the file list */
/* GlobalKeyChain inside the file screens (issue #13): Load Sample, Load/
 * Save Module and the libraries are ordinary object lists in the
 * original, so the global keys work there. Playback keys act in place;
 * screen keys close the modal screen and are handled once the main loop
 * has it back. Returns 0 (not a global key), 1 (handled, carry on) or 2
 * (leave the modal screen). */
static int PendingGlobalKey = 0, PendingHelpContext = 1;
static int FromFileScreen = 0;          /* handle_global runs a key a file
                                           screen handed back (#29) */
static void handle_global(int key);
static void sample_to_instrument(void);
static void help_open(int context);

static void bg_keep(void) { }          /* leave the screen as drawn */

static int modal_global_key(int key, int help_context)
{
    if ((key >= ITK_ALT_F1 && key <= ITK_ALT_F1 + 7) || key == ITK_ALT_F11)
        goto in_place;
    switch (key) {
    case ITK_F6: case ITK_F7: case ITK_F8:
    case ITK_CTRL_F5: case ITK_SHIFT_F6:
    case 0x05: case 0x09: case 0x0D:    /* Ctrl-E / I / M (#28) */
    in_place:
        handle_global(key);
        return 1;
    case ITK_F1:
        PendingHelpContext = help_context;
        /* fall through */
    case ITK_F2: case ITK_F3: case ITK_F4: case ITK_F5:
    case ITK_F9: case ITK_F10: case ITK_F11: case ITK_F12:
    case ITK_CTRL_F1: case ITK_CTRL_F3: case ITK_CTRL_F4: case ITK_SHIFT_F9:
    case ITK_SHIFT_F5:
    case ITK_CTRL_SHIFT_F9: case ITK_CTRL_SHIFT_F10:    /* feature 016 */
        PendingGlobalKey = key;
        return 2;
    case 0x11:
        /* Ctrl-Q: Glbl_Quit is in the global key list, so it works on
         * the file screens too (#27). Asked in place, over the screen as
         * it is drawn; Cancel stays here. */
        if (confirm_box_bg("Exit Impulse Tracker?", 1, bg_keep)) {
            Running = 0;
            return 2;
        }
        return 1;
    default:
        return 0;
    }
}

static int lib_browse_run(int inslib, const char *srcname)
{
    LibSel = 0;                 /* cursor on the Directory row, as IT */
    LibTop = 0;
    LibCheckIdx = -1;

    while (Running) {
        int key;

        draw_lib_browser(inslib, srcname);

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (modal_global_key(key, inslib ? 11 : 6)) {
        case 1: continue;
        case 2: return 1;               /* closes the requester too */
        default: break;
        }

        switch (key) {
        case ITK_QUIT:
            Running = 0;
            return 1;
        case ITK_ESC:
        case ITK_BACKSPACE:
            return 0;
        case ITK_UP:   if (LibSel > 0) LibSel--; break;
        case ITK_DOWN: if (LibSel < LibN) LibSel++; break;
        case ITK_PGUP: LibSel -= 34; if (LibSel < 0) LibSel = 0; break;
        case ITK_PGDN: LibSel += 34; if (LibSel > LibN) LibSel = LibN;
                       break;
        case ITK_HOME: LibSel = 0; break;
        case ITK_END:  LibSel = LibN; break;
        case ITK_ENTER:
            if (LibSel == 0)
                return 0;               /* the Directory row */
            if (!inslib) {
                if (lib_load_sample_entry(&SLib[LibSel - 1]))
                    return 1;
            } else {
                if (lib_load_instrument_entry(&ILib[LibSel - 1]))
                    return 1;
            }
            break;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (m.x >= 3 && m.x <= 76 && m.y >= 13 && m.y <= 47) {
                int idx = LibTop + (m.y - 13);
                if (idx <= LibN) {
                    if (idx == LibSel) {
                        if (idx == 0)
                            return 0;
                        if (!inslib
                            ? lib_load_sample_entry(&SLib[idx - 1])
                            : lib_load_instrument_entry(&ILib[idx - 1]))
                            return 1;
                    } else {
                        LibSel = idx;
                    }
                }
            }
            break; }
        default:
            if (!inslib && LibSel > 0)
                lib_preview_key(&SLib[LibSel - 1], LibSel, key);
            break;
        }
    }
    return 1;
}

/* Enter on a file in library mode: standalone sample/instrument files
 * load directly, containers open as a library (LSWindow_Enter /
 * LIWindow_Enter dispatch by format code). */
static void lib_open_source(const char *path, int *done)
{
    if (ReqLibMode == 1) {
        int n = RIS_ScanModule(path, SLib, LIB_MAX);

        if (n < 0) {
            status("Unknown sample source: %s", path);
            return;
        }
        if (n == 1 && (SLib[0].Format == 5 || SLib[0].Format == 7 ||
                       (SLib[0].Format == 2 && has_ext_ci(path, ".ITS")))) {
            if (lib_load_sample_entry(&SLib[0]))
                *done = 1;
            return;
        }
        LibN = n;
        if (lib_browse_run(0, path))
            *done = 1;
    } else {
        int n = RI_ScanModule(path, ILib, LIB_MAX);

        if (n < 0) {
            status("Unknown instrument source: %s", path);
            return;
        }
        if (n == 1 && (ILib[0].Format == 3 || ILib[0].Format == 4)) {
            if (lib_load_instrument_entry(&ILib[0]))
                *done = 1;
            return;
        }
        LibN = n;
        if (lib_browse_run(1, path))
            *done = 1;
    }
}

static void req_activate_file(int *done)
{
    if (ReqNF && FSel < ReqNF) {
        if (ReqSave) {
            snprintf(ReqName, sizeof(ReqName), "%s",
                     ReqFiles[FSel].name);
            req_do_save(done);
        } else if (ReqLibMode) {
            lib_open_source(ReqFiles[FSel].name, done);
        } else if (load_module_screen(ReqFiles[FSel].name)) {
            *done = 1;
        } else {
            status("Can't load %s.", ReqFiles[FSel].name);
        }
    }
}

static void req_enter_dir(const char *name)
{
    if (!chdir(name))
        req_scan();
    else
        status("Can't change to %s.", name);
}

static void file_requester_run_(int save);

static void file_requester_run(int save)
{
    int keep = FileMode;

    FileMode = save ? 10 : 9;
    file_requester_run_(save);
    FileMode = keep;
}

static void file_requester_run_(int save)
{
    int done = 0;

    ReqSave = save;
    req_scan();
    ReqFocus = save ? 3 : 0;            /* save: filename field first */
    if (save)
        snprintf(ReqName, sizeof(ReqName), "%s",
                 FileNameDisp[0] ? FileNameDisp : "UNTITLED.IT");

    while (!done && Running) {
        int key;

        draw_file_requester();
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (modal_global_key(key, ReqLibMode == 2 ? 11 : 3)) {
        case 1: continue;
        case 2: done = 1; continue;
        default: break;
        }

        switch (key) {
        case ITK_QUIT: Running = 0; done = 1; continue;
        case ITK_ESC:  done = 1; continue;
        case ITK_TAB:
            ReqFocus = (ReqFocus + 1) % (ReqSave ? 5 : 4);
            continue;
        case ITK_SHIFT_TAB:
            ReqFocus = (ReqFocus + (ReqSave ? 4 : 3)) % (ReqSave ? 5 : 4);
            continue;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (m.x >= 3 && m.x <= 40 && m.y >= 13 && m.y <= 43) {
                int idx = FTop + (m.y - 13);
                ReqFocus = 0;
                if (idx < ReqNF) {
                    if (idx == FSel)
                        req_activate_file(&done);
                    else
                        FSel = idx;
                }
            } else if (m.x >= 44 && m.x <= 55 && m.y >= 13 && m.y <= 33) {
                int idx = DTop + (m.y - 13);
                ReqFocus = 1;
                if (idx < ReqND) {
                    if (idx == DSel)
                        req_enter_dir(ReqDirs[DSel]);
                    else
                        DSel = idx;
                }
            } else if (m.x >= 59 && m.x <= 66 && m.y >= 13 && m.y <= 33) {
                int idx = m.y - 13;
                ReqFocus = 2;
                if (idx < ReqNDrv) {
                    if (idx == VSel) {
                        char d[4] = "A:\\";
                        d[0] = ReqDrives[VSel];
                        req_enter_dir(d);
                    } else {
                        VSel = idx;
                    }
                }
            } else if (m.y == 46 && m.x >= 13 && m.x <= 38) {
                ReqFocus = 3;
            } else if (ReqSave && m.x >= 69 && m.x <= 77 &&
                       m.y >= 12 && m.y <= 23 && (m.y - 12) % 3 != 2) {
                ReqFocus = 4;           /* format radio buttons */
                SaveFormat = (uint8_t)((m.y - 12) / 3);
            }
            continue; }
        default: break;
        }

        if (ReqFocus == 0) {
            switch (key) {
            case ITK_UP:   if (FSel > 0) FSel--; break;
            case ITK_DOWN: if (FSel < ReqNF - 1) FSel++; break;
            case ITK_PGUP: FSel -= 30; if (FSel < 0) FSel = 0; break;
            case ITK_PGDN: FSel += 30;
                           if (FSel >= ReqNF) FSel = ReqNF ? ReqNF - 1 : 0;
                           break;
            case ITK_HOME: FSel = 0; break;
            case ITK_END:  FSel = ReqNF ? ReqNF - 1 : 0; break;
            case ITK_ENTER: req_activate_file(&done); break;
            case ITK_BACKSPACE: req_enter_dir(".."); break;
            default: break;
            }
        } else if (ReqFocus == 1) {
            switch (key) {
            case ITK_UP:   if (DSel > 0) DSel--; break;
            case ITK_DOWN: if (DSel < ReqND - 1) DSel++; break;
            case ITK_PGUP: DSel -= 20; if (DSel < 0) DSel = 0; break;
            case ITK_PGDN: DSel += 20;
                           if (DSel >= ReqND) DSel = ReqND ? ReqND - 1 : 0;
                           break;
            case ITK_HOME: DSel = 0; break;
            case ITK_END:  DSel = ReqND ? ReqND - 1 : 0; break;
            case ITK_ENTER:
                if (ReqND)
                    req_enter_dir(ReqDirs[DSel]);
                break;
            case ITK_BACKSPACE: req_enter_dir(".."); break;
            default: break;
            }
        } else if (ReqFocus == 2) {
            switch (key) {
            case ITK_UP:   if (VSel > 0) VSel--; break;
            case ITK_DOWN: if (VSel < ReqNDrv - 1) VSel++; break;
            case ITK_ENTER:
                if (ReqNDrv) {
                    char d[4] = "A:\\";
                    d[0] = ReqDrives[VSel];
                    req_enter_dir(d);
                }
                break;
            case ITK_BACKSPACE: req_enter_dir(".."); break;
            default: break;
            }
        } else if (ReqFocus == 4) {         /* save-format group */
            switch (key) {
            case ITK_UP:
                if (SaveFormat > 0) SaveFormat--;
                break;
            case ITK_DOWN:
                if (SaveFormat < 3) SaveFormat++;
                break;
            default:
                break;
            }
        } else {                            /* filename input */
            int len = (int)strlen(ReqName);
            if (key == ITK_BACKSPACE) {
                if (len > 0)
                    ReqName[len - 1] = 0;
            } else if (key == ITK_ENTER) {
                if (ReqSave) {
                    req_do_save(&done);
                } else if (strchr(ReqName, '*') || strchr(ReqName, '?')) {
                    req_scan();
                } else if (ReqLibMode) {
                    lib_open_source(ReqName, &done);
                } else if (load_module_screen(ReqName)) {
                    done = 1;
                } else {
                    status("Can't load %s.", ReqName);
                }
            } else if (key >= 32 && key < 127 &&
                       len < (int)sizeof(ReqName) - 1) {    /* 64 */
                ReqName[len] = (char)key;
                ReqName[len + 1] = 0;
            }
        }
    }
}

static void file_requester(void)        /* F9 (load) */
{
    file_requester_run(0);
}

static void save_requester(void)        /* F10 (Glbl_F10 / mode 10) */
{
    char keep[sizeof(ReqName)];

    memcpy(keep, ReqName, sizeof(keep));
    file_requester_run(1);
    memcpy(ReqName, keep, sizeof(keep));
    ReqSave = 0;
}

/* ===================================================================
 * Load Sample / Sample Library screen (feature 015) -- O1_LoadSampleList
 * and O1_ViewSampleLibrary (IT_OBJ1.ASM 952), custom draws from
 * IT_DISK.ASM (D_DrawLoadSampleWindow 5260, D_PreLoadSampleWindow,
 * D_LSDrawDriveWindow). Geometry, attributes and strings: see
 * specs/015-load-sample-screen/research.md R1/R5/R6.
 * =================================================================== */
#define LS_MAX   620                    /* D_LoadSampleFiles cap */
#define LS_ROWS  35                     /* rows 13..47 */
#define LS_DROWS 10                     /* drive window rows */

static slibent_t LsEnt[LS_MAX];
static int  LsN, LsCur, LsTop;
static int  LsFocus = 15;               /* object: 15 list, 16 drives */
static int  LsView;                     /* 1 = Sample Library (Ctrl-F3) */
static char LsDir[256];                 /* = sizeof(DirSample) */
static char LsDrives[26];
static int  LsNDrv, LsDrvCur, LsDrvTop;

static const uint8_t LsInfoText[] =         /* LSInfoText (no divider) */
    "Filename\015   Speed\015    Loop\015 LoopBeg\015 LoopEnd\015"
    " SusLoop\015 SusLBeg\015 SusLEnd\015 Quality\015  Length";
static const uint8_t LsParText[] =          /* LSParametersText */
    "Default Volume\015 Global Volume\015\015\015 Vibrato Speed\015"
    " Vibrato Depth\015  Vibrato Rate";
static const uint8_t LsFileInfoText[] =     /* LSFileInfoText */
    "Format\015  Size\015  Date\015  Time";

/* canonical absolute form of a host path, in place */
static void ls_normalize(char *p, size_t cap)
{
#ifdef _WIN32
    char out[MAX_PATH];
    DWORD r = GetFullPathNameA(p, (DWORD)sizeof(out), out, NULL);
    if (r > 0 && r < cap)               /* never truncate a path */
        memcpy(p, out, (size_t)r + 1);
#else
    char out[4096];
    if (realpath(p, out) && strlen(out) < cap)  /* never truncate a path */
        memcpy(p, out, strlen(out) + 1);
#endif
}

/* drive list: logical drives on Windows; a single "/" elsewhere
 * (research R10 -- platform adaptation) */
static void ls_scan_drives(void)
{
    LsNDrv = 0;
    LsDrvCur = 0;
#ifdef _WIN32
    {
        DWORD drives = GetLogicalDrives();
        int i;
        char cur = (char)toupper((unsigned char)LsDir[0]);
        for (i = 0; i < 26; i++)
            if (drives & (1u << i)) {
                if ((char)('A' + i) == cur)
                    LsDrvCur = LsNDrv;
                LsDrives[LsNDrv++] = (char)('A' + i);
            }
    }
#else
    LsDrives[LsNDrv++] = '/';
#endif
}

static int  LsInModule;                 /* SamplesInModule */
static int      LsCheckIdx = -1;        /* SampleCheck */
static sample_t LsCheckHdr;             /* CheckDataArea (80 bytes) */

/* D_InitLoadSamples: (re)read the current sample directory */
static void ls_list(void)
{
    LsN = RIS_ListDirectory(LsDir, LsEnt, LS_MAX);
    if (LsN < 0)
        LsN = 0;
    LsCur = LsTop = 0;
    LsInModule = 0;
    LibCheckIdx = -1;                   /* SampleInMemory = 0FFFFh */
    LsCheckIdx = -1;                    /* SampleCheck = 0FFFFh */
    ls_scan_drives();
}

/* LSWindow_EnterLoadInSampleData: list a module's samples on the same
 * screen. Record 0 is ExitLibraryDirectory (IT_DISK.ASM): a type-1
 * entry named "." with the dotted Directory name and the module's
 * date, so Enter on it re-lists the directory. */
static void ls_enter_module(const slibent_t *m)
{
    slibent_t mod = *m;
    int n, i;

    n = RIS_ScanModule(mod.SrcFile, LsEnt + 1, LS_MAX - 1);
    if (n < 0) {
        status("Unknown sample source: %.12s", mod.hdr.DOSFileName);
        return;
    }
    memset(&LsEnt[0], 0, sizeof(LsEnt[0]));
    memcpy(LsEnt[0].hdr.DOSFileName, ".           ", 12);
    for (i = 0; i < 8; i++) {
        LsEnt[0].hdr.SampleName[i] = (char)154;
        LsEnt[0].hdr.SampleName[17 + i] = (char)154;
    }
    memcpy(LsEnt[0].hdr.SampleName + 8, "Directory", 9);
    LsEnt[0].Format = 1;
    LsEnt[0].Date = mod.Date;
    LsEnt[0].Time = mod.Time;
    memcpy(LsEnt[0].SrcFile, LsDir, sizeof(LsDir));  /* 256 <= 264 */
    for (i = 1; i <= n; i++) {
        LsEnt[i].Date = mod.Date;
        LsEnt[i].Time = mod.Time;
        LsEnt[i].SortPri = 2;
    }
    LsN = n + 1;
    LsCur = LsTop = 0;
    LsInModule = 1;
    LibCheckIdx = -1;
    LsCheckIdx = -1;
}

static void ls_set_dir(const char *path)
{
    if (strlen(path) >= sizeof(LsDir)) {
        status("Path too long.");
        return;
    }
    memcpy(LsDir, path, strlen(path) + 1);
    ls_normalize(LsDir, sizeof(LsDir));
    memcpy(DirSample, LsDir, sizeof(DirSample));    /* SampleDirectory */
    ls_list();
}

/* LSWindow_Enter, type 1: change into a directory ("\" = the root) */
static void ls_enter_dir(const slibent_t *e)
{
    char nd[264];                       /* = sizeof(SrcFile) */

    if (LsInModule && e->hdr.DOSFileName[0] == '.') {
        snprintf(nd, sizeof(nd), "%s", LsDir);  /* ExitLibraryDirectory */
    } else if (!strcmp(e->hdr.DOSFileName, "\\")) {
#ifdef _WIN32
        snprintf(nd, sizeof(nd), "%c:\\", LsDir[0]);
#else
        snprintf(nd, sizeof(nd), "/");
#endif
    } else {
        snprintf(nd, sizeof(nd), "%s", e->SrcFile);
    }
    ls_set_dir(nd);
}

/* LS_DriveWindow_Enter: switch to that drive's current directory */
static void ls_enter_drive(void)
{
#ifdef _WIN32
    char d[MAX_PATH];
    if (_getdcwd(LsDrives[LsDrvCur] - 'A' + 1, d, sizeof(d)))
        ls_set_dir(d);
    else
        status("Can't read drive %c:", LsDrives[LsDrvCur]);
#else
    ls_set_dir("/");
#endif
}

/* D_DeleteSampleFile: not for unchecked entries or directories, and not
 * inside a module; confirm (O1_ConfirmDelete2, default Cancel), delete,
 * drop the row */
static void ls_delete_file(void)
{
    int i;

    if (LsN == 0 || LsInModule || LsEnt[LsCur].Format <= 1)
        return;
    if (!confirm_box("Delete file?"))
        return;
    if (remove(LsEnt[LsCur].SrcFile) != 0) {
        status("Can't delete %s.", LsEnt[LsCur].hdr.DOSFileName);
        return;
    }
    for (i = LsCur; i < LsN - 1; i++)
        LsEnt[i] = LsEnt[i + 1];
    LsN--;
    if (LsCur >= LsN && LsCur > 0)
        LsCur--;
}

/* D_DrawLoadSampleWindow, list part */
static void ls_draw_list(void)
{
    int i, k;

    if (LsN == 0) {
        Screen_DrawString(6, 13, "No files.", 0x05);    /* NoFilesMsg */
        return;
    }
    if (LsTop > LsCur)
        LsTop = LsCur;
    if (LsTop + (LS_ROWS - 1) < LsCur)
        LsTop = LsCur - (LS_ROWS - 1);

    for (i = 0; i < LS_ROWS; i++)                   /* divider, 2A8h */
        Screen_PutChar(31, 13 + i, 0xA8, 0x02);

    for (i = 0; i < LS_ROWS && LsTop + i < LsN; i++) {
        const slibent_t *e = &LsEnt[LsTop + i];
        int y = 13 + i;
        uint8_t a;

        drawf(2, y, 0x20, "%03d", LsTop + i + 1);     /* PE_ConvAX2Num */

        /* colour by type: 0 unchecked 6, dir 5, unknown 2, else 3 */
        a = e->Format == 0 ? 0x06 : e->Format == 1 ? 0x05
          : e->Format == 4 ? 0x02 : 0x03;
        for (k = 0; k < 25; k++) {
            uint8_t c = (uint8_t)e->hdr.SampleName[k];
            Screen_PutChar(6 + k, y, c >= 226 ? ' ' : c, a);
        }
        for (k = 0; k < 12; k++) {                  /* filename, col 32 */
            uint8_t c = (uint8_t)e->hdr.DOSFileName[k];
            Screen_PutChar(32 + k, y, c, a);
            if (!c) {
                for (k++; k < 12; k++)
                    Screen_PutChar(32 + k, y, 0, a);
                break;
            }
        }
    }

    if (LsFocus == 15) {                /* D_PreLoadSampleWindow */
        int y = 13 + LsCur - LsTop;
        for (k = 0; k < 38; k++)
            Screen_SetAttr(6 + k, y, k == 25 ? 0x32 : 0x30);
    }
}

/* D_LSDrawDriveWindow + D_LSPreDriveWindow */
static void ls_draw_drives(void)
{
    int i;

    if (LsDrvTop > LsDrvCur)
        LsDrvTop = LsDrvCur;
    if (LsDrvTop + (LS_DROWS - 1) < LsDrvCur)
        LsDrvTop = LsDrvCur - (LS_DROWS - 1);
    for (i = 0; i < LS_DROWS && LsDrvTop + i < LsNDrv; i++)
        drawf(46, 13 + i, 0x05, "Drive %c:", LsDrives[LsDrvTop + i]);
    if (LsFocus == 16 && LsNDrv) {
        int y = 13 + LsDrvCur - LsDrvTop, k;
        for (k = 0; k < 8; k++)
            Screen_SetAttr(46 + k, y, 0x30);
    }
}

/* LSInfoBox values + thumbbars + LSFileInfo for the highlighted entry
 * (D_DrawLoadSampleWindow, the LS*Input objects). Drawn for every entry
 * type, as the original does: a directory shows Speed 0000000, Loop Off,
 * 8 Bit, 0. */
static void ls_draw_values(void)
{
    static const char *const month[13] = {
        "", "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    const slibent_t *e;
    const sample_t *s;
    char fn[13];
    int y;

    if (LsN == 0)
        return;
    e = &LsEnt[LsCur];
    s = &e->hdr;

    memcpy(fn, s->DOSFileName, 12);
    fn[12] = 0;
    drawf(64, 13, 0x02, "%-13.13s", fn);            /* LSFileNameInput */

    /* F_Draw5Num: seven zero-padded digits, colour 2 */
    drawf(64, 14, 0x02, "%07u", (unsigned)(s->C5Speed % 10000000u));
    drawf(64, 16, 0x02, "%07u", (unsigned)(s->LoopBeg % 10000000u));
    drawf(64, 17, 0x02, "%07u", (unsigned)(s->LoopEnd % 10000000u));
    drawf(64, 19, 0x02, "%07u", (unsigned)(s->SusLoopBeg % 10000000u));
    drawf(64, 20, 0x02, "%07u", (unsigned)(s->SusLoopEnd % 10000000u));
    for (y = 15; y <= 18; y += 3) {     /* F_DrawToggle + GetSampleToggle */
        uint8_t on = y == 15 ? 0x10 : 0x20, pp = y == 15 ? 0x40 : 0x80;
        if (!(s->Flags & on)) {
            Screen_DrawString(64, y, "Off", 0x02);
        } else {
            Screen_DrawString(64, y, "On", 0x02);
            Screen_DrawString(67, y, (s->Flags & pp) ? "Ping Pong"
                                                     : "Forwards", 0x02);
        }
    }
    /* Quality uses IT_DISK.ASM's strings, stereo = Cvt bit 32 */
    Screen_DrawString(64, 21, (s->Cvt & 32)
                      ? ((s->Flags & 2) ? "16 Bit Stereo" : "8 Bit Stereo")
                      : ((s->Flags & 2) ? "16 Bit" : "8 Bit"), 0x02);
    drawf(64, 22, 0x02, "%u", s->Length);

    draw_thumbbar(63, 33, 0, 64, s->Vol, 0x02);     /* LSDefaultVolume */
    draw_thumbbar(63, 34, 0, 64, s->GvL, 0x02);     /* LSGlobalVolume */
    draw_thumbbar(63, 37, 0, 64, s->ViS, 0x02);     /* LSVibratoSpeed */
    draw_thumbbar_scaled(63, 38, 0, 32, s->ViD, 8, 0x02);
    draw_thumbbar_scaled(63, 39, 0, 255, s->ViR, 8, 0x02);

    /* file info, attr 5 */
    Screen_DrawString(53, 44, RIS_FormatName(e->Format), 0x05);
    if (e->FileSize < 655360000u)                   /* DX < 10000 */
        drawf(53, 45, 0x05, "%09u", (unsigned)e->FileSize);
    {
        /* MonthNames entry + its NUL cell, day, ", ", year */
        int mo = (e->Date >> 5) & 15, d = e->Date & 31;
        const char *mn = mo <= 12 ? month[mo] : "";
        int x = 53 + (int)strlen(mn);
        Screen_DrawString(53, 46, mn, 0x05);
        Screen_PutChar(x++, 46, 0, 0x05);
        drawf(x, 46, 0x05, "%d, %d", d, ((e->Date >> 9) & 0x7F) + 1980);
    }
    {
        /* 12-hour, no leading zero, am/pm */
        int h = e->Time >> 11, mi = (e->Time >> 5) & 63, pm = 0;
        if (h >= 12) { pm = 1; h -= 12; }
        if (h == 0) h = 12;
        drawf(53, 47, 0x05, "%d:%02d%cm", h, mi, pm ? 'p' : 'a');
    }
}

/* D_DrawWaveForm (IT_DISK.ASM 8542): 248x32 pixels from the check slot,
 * shown only when the highlighted entry is the one auditioned
 * (CurrentSample == SampleInMemory). Unlike F3's I_DrawWaveForm there is
 * no joining with the previous column; loop markers use the record's
 * (possibly edited) loop points scaled by 247. */
static uint8_t LsWavePix[248 * 32];

static void ls_marker(uint32_t b, uint32_t en, uint32_t len, int sus)
{
    uint32_t cb = (uint32_t)(((uint64_t)247 * b + len / 2) / len);
    uint32_t ce = (uint32_t)(((uint64_t)247 * en + len / 2) / len);
    int r, ah = 1;
    uint8_t al = 1;

    if (cb > 247) cb = 247;
    if (ce > 247) ce = 247;
    for (r = 0; r < 32; r++) {
        uint8_t v = sus ? al : (uint8_t)((ah & 2) ? 1 : 0);
        LsWavePix[r * 248 + cb] = v;
        LsWavePix[r * 248 + ce] = v;
        ah++;
        al ^= 1;
    }
}

static void ls_draw_waveform(void)
{
    const sample_t *d = &Song.Smp[CHECK_SLOT];
    const sample_t *h;
    int x, y;

    if (LsN == 0 || LibCheckIdx != LsCur || !smp_has_data(d))
        return;
    h = &LsEnt[LsCur].hdr;
    memset(LsWavePix, 0, sizeof(LsWavePix));
    {
        int is16 = (d->Flags & 2) != 0, step = is16 ? 2 : 1;
        const uint8_t *base = (const uint8_t *)d->Data + (is16 ? 1 : 0);
        uint64_t total = (uint64_t)d->Length * (uint64_t)step;
        uint64_t per = (total << 16) / 248, pos = 0;
        int col;

        for (col = 0; col < 248; col++) {
            uint64_t start = pos >> 16, end = (pos + per) >> 16;
            size_t i = (size_t)start;
            int8_t mn, mx;
            int cnt, row;

            if (start >= total)
                start = total ? total - (uint64_t)step : 0;
            i = (size_t)start;
            mn = mx = (int8_t)base[i];              /* CL = CH = [SI] */
            for (; i < end && i < total; i += (size_t)step) {
                int8_t v = (int8_t)base[i];
                if (v < mn) mn = v;
                else if (v > mx) mx = v;
            }
            {   /* SAR/Add AX,202h/SAR, AL->AH carry kept (authentic) */
                uint8_t al = (uint8_t)((int8_t)mn >> 1);
                uint8_t ah = (uint8_t)((int8_t)mx >> 1);
                unsigned ax = (unsigned)((ah << 8) | al) + 0x202;
                int rhi, rlo;
                ah = (uint8_t)((int8_t)(ax >> 8) >> 2);
                al = (uint8_t)((int8_t)(ax & 0xFF) >> 2);
                rhi = (int8_t)ah;
                rlo = (int8_t)al;
                cnt = rhi - rlo + 1;
                row = 16 - rhi;
                if (row == 32)
                    row = 31;
            }
            for (; cnt > 0; cnt--, row++)
                if (row >= 0 && row < 32)
                    LsWavePix[row * 248 + col] = 1;
            pos += per;
        }
    }
    if (h->Length) {
        if (h->Flags & 0x10)
            ls_marker(h->LoopBeg, h->LoopEnd, h->Length, 0);
        if (h->Flags & 0x20)
            ls_marker(h->SusLoopBeg, h->SusLoopEnd, h->Length, 1);
    }
    Screen_GenerateCharacters(0, 31, 4, LsWavePix);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 31; x++)
            Screen_PutChar(46 + x, 25 + y, (uint8_t)(y * 31 + x), 0x0D);
}

static void ls_draw(void)
{
    Screen_Clear(0x20);
    draw_chrome(LsView ? "Sample Library (Ctrl-F3)" : "Load Sample");
    Screen_DrawBox(5, 12, 44, 48, 27);          /* LoadSampleBox */
    Screen_DrawBox(45, 12, 54, 23, 27);         /* DriveSampleBox */
    Screen_DrawBox(63, 12, 77, 23, 27);         /* LSInfoBox */
    Screen_DrawStringCtl(55, 13, LsInfoText, 0x20, NULL);
    Screen_DrawBox(45, 24, 77, 29, 27);         /* LSWaveFormBox */
    Screen_DrawBox(45, 30, 77, 42, 9);          /* LSParametersBox */
    Screen_DrawStringCtl(48, 33, LsParText, 0x20, NULL);
    Screen_DrawBox(62, 32, 72, 35, 25);         /* LSParametersVolBox */
    Screen_DrawBox(62, 36, 72, 40, 25);         /* LSParametersVibBox */
    Screen_DrawBox(52, 43, 77, 48, 27);         /* LSFileInfoBox */
    Screen_DrawStringCtl(46, 44, LsFileInfoText, 0x20, NULL);
    ls_draw_list();
    ls_draw_drives();
    ls_draw_values();
    ls_draw_waveform();
}

/* ---- US3: editing on the Load Sample screen ------------------------
 * The LS*Input objects write straight into the list record
 * (SetLoadSample5Num -> D_GetLoadSampleVars, research R8); every cursor
 * move first runs CheckSampleModified (R8b), which offers to save the
 * sample file, else to discard the edit. */

/* LSInfoBox field objects 17..24 by row; 25..29 are the thumbbars */
static int ls_obj_row(int obj) { return obj >= 17 && obj <= 24 ? obj - 4 : 0; }

static void ls_snapshot(void)
{
    if (LsN && LsCur != LsCheckIdx) {
        LsCheckIdx = LsCur;
        LsCheckHdr = LsEnt[LsCur].hdr;
    }
}

/* D_LSCheckLoopValues / D_LSCheckSusLoopValues: caps at Length (not
 * Length-1 as F3's I_CheckLoopValues), then clears the loop bit when
 * end <= begin */
static void ls_check_loops(sample_t *s, int sus)
{
    uint32_t *b = sus ? &s->SusLoopBeg : &s->LoopBeg;
    uint32_t *en = sus ? &s->SusLoopEnd : &s->LoopEnd;

    if (s->Length <= *b) *b = s->Length;
    if (s->Length < *en) *en = s->Length;
    if (*en <= *b)
        s->Flags &= (uint8_t)~(sus ? 0x20 : 0x10);
}

/* the record differs from its snapshot? The original compares header
 * bytes 00h..11h and 13h..4Fh -- byte 12h (Flags) is skipped, so a loop
 * toggle alone does not count as a modification (authentic). */
static int ls_modified(void)
{
    const uint8_t *a, *b;

    if (LsN == 0 || LsInModule || LsCheckIdx != LsCur)
        return 0;
    if (LsEnt[LsCur].Format <= 1 || LsEnt[LsCur].Format >= 0x20)
        return 0;
    a = (const uint8_t *)&LsEnt[LsCur].hdr;
    b = (const uint8_t *)&LsCheckHdr;
    return memcmp(a, b, 0x12) != 0 || memcmp(a + 0x13, b + 0x13, 0x50 - 0x13) != 0;
}

/* replace (or add) the filename's extension */
static void ls_with_ext(char *out, size_t cap, const char *dir,
                        const char *name, const char *ext)
{
    char base[16];
    char *dot;

    snprintf(base, sizeof(base), "%.12s", name);
    if ((dot = strrchr(base, '.')) != NULL)
        *dot = 0;
    snprintf(out, cap, "%s/%s%s", dir, base, ext);
}

/* D_SaveSampleInternal, with the user-chosen deviation "keep format":
 * the original always writes ITS under the record's filename, turning an
 * edited .WAV into ITS data with a .WAV name. Here an ITS stays ITS in
 * place; a WAV stays WAV only when the change is one WAV can carry (the
 * sample rate / filename); anything else is written as ITS under the
 * same name with an .ITS extension, next to the untouched original.
 * Returns 1 when a file was written. */
static int ls_save_entry(int renamed)
{
    slibent_t *e = &LsEnt[LsCur];
    sample_t tmp;
    char name[16], target[300];
    int ok, wav_ok = 0, fallback = 0;

    memset(&tmp, 0, sizeof(tmp));
    if (!RIS_LoadSample(e, &tmp)) {
        status("Unable to load sample.");
        return 0;
    }
    snprintf(name, sizeof(name), "%.12s", e->hdr.DOSFileName);
    while (strlen(name) && name[strlen(name) - 1] == ' ')
        name[strlen(name) - 1] = 0;

    if (e->Format == 5 || e->Format == 7) {
        sample_t a = e->hdr, b = LsCheckHdr;
        a.C5Speed = b.C5Speed = 0;          /* WAV carries the rate */
        memset(a.DOSFileName, 0, 12);       /* and the filename */
        memset(b.DOSFileName, 0, 12);
        a.Flags = b.Flags = 0;              /* not compared (see above) */
        wav_ok = memcmp(&a, &b, 0x50) == 0;
    }

    if (e->Format == 2 || wav_ok) {
        if (renamed)
            snprintf(target, sizeof(target), "%s/%s", LsDir, name);
        else
            snprintf(target, sizeof(target), "%s", e->SrcFile);
        ok = e->Format == 2 ? RIS_SaveITS(&tmp, target)
                            : RIS_SaveWAV(&tmp, target);
        if (ok && renamed && strcmp(target, e->SrcFile))
            remove(e->SrcFile);             /* rename: drop the old file */
    } else {
        FILE *fp;
        ls_with_ext(target, sizeof(target), LsDir, name, ".ITS");
        fallback = 1;
        if ((fp = fopen(target, "rb")) != NULL) {
            fclose(fp);
            if (!confirm_box("Replace existing .ITS?")) {
                free(tmp.Data);
                return 0;
            }
        }
        ok = RIS_SaveITS(&tmp, target);
    }
    free(tmp.Data);
    if (!ok)
        status("Error: sample NOT saved!");
    else if (fallback)
        status("Saved as ITS -- the original file is unchanged.");
    else
        status("Sample saved.");
    return ok;
}

/* CheckSampleModified: 1 = the move may go ahead, 0 = stay */
static int ls_check_modified(void)
{
    int renamed, cur;

    if (!ls_modified())
        return 1;
    renamed = memcmp(LsEnt[LsCur].hdr.DOSFileName, LsCheckHdr.DOSFileName,
                     12) != 0;
    if (confirm_box_def(renamed ? "Save/Rename sample?" : "Save sample?",
                        1)) {
        cur = LsCur;
        ls_save_entry(renamed);
        ls_list();                          /* D_InitLoadSamples */
        LsCur = cur < LsN ? cur : (LsN ? LsN - 1 : 0);
        LsCheckIdx = -1;
        return 1;
    }
    if (confirm_box_def("Discard changes?", 1)) {
        memcpy(&LsEnt[LsCur].hdr, &LsCheckHdr, 0x50);
        return 1;
    }
    return 0;
}

/* LSWindow_Space: O1_EditSampleName -- box (23,25)-(56,31) style 3,
 * "Edit Sample Name" at (32,26) attr 23h, 25-char input at (27,29) in a
 * style-27 box; Enter keeps (trailing spaces trimmed), Esc cancels */
static void ls_edit_name(void)
{
    char buf[26];
    int len;

    if (LsN == 0)
        return;
    memcpy(buf, LsEnt[LsCur].hdr.SampleName, 25);
    buf[25] = 0;
    len = (int)strlen(buf);
    while (len && buf[len - 1] == ' ')
        buf[--len] = 0;

    while (Running) {
        int key;

        ls_draw();
        Screen_DrawBox(23, 25, 56, 31, 3);
        Screen_DrawString(32, 26, "Edit Sample Name", 0x23);
        Screen_DrawBox(26, 28, 53, 30, 27);
        drawf(27, 29, 0x02, "%-25.25s", buf);
        Screen_SetAttr(27 + (len < 25 ? len : 24), 29, 0x30);
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; return; }
        if (key == ITK_ESC)
            return;
        if (key == ITK_ENTER) {
            memset(LsEnt[LsCur].hdr.SampleName, 0, 26);
            memcpy(LsEnt[LsCur].hdr.SampleName, buf, (size_t)len);
            return;
        }
        if (key == ITK_BACKSPACE && len > 0)
            buf[--len] = 0;
        else if (key >= 32 && key < 256 && len < 25) {
            buf[len++] = (char)key;
            buf[len] = 0;
        }
    }
}

/* keys for the field objects 17..29 (the list is 15, drives 16) */
static void ls_field_key(int key)
{
    sample_t *s = &LsEnt[LsCur].hdr;
    int obj = LsFocus;

    if (LsN == 0)
        return;
    switch (key) {                          /* object links, research R1 */
    case ITK_UP:
        if (obj > 17) LsFocus = obj - 1;
        return;
    case ITK_DOWN:
        if (obj < 29) LsFocus = obj + 1;
        return;
    case ITK_TAB:
        LsFocus = obj <= 24 ? 25 : 15;
        return;
    case ITK_SHIFT_TAB:
        LsFocus = obj <= 24 ? 16 : 17;
        return;
    default:
        break;
    }

    if (obj == 17) {                        /* LSFileNameInput */
        int len = 0;
        while (len < 12 && s->DOSFileName[len])
            len++;
        if (key == ITK_BACKSPACE && len > 0)
            s->DOSFileName[len - 1] = 0;
        else if (key > 32 && key < 127 && len < 12)
            s->DOSFileName[len] = (char)key;
        return;
    }
    if (obj == 19 || obj == 22) {           /* LSLoopToggle/SusLoopToggle */
        if (key == ITK_ENTER || key == ' ')
            s->Flags ^= (uint8_t)(obj == 19 ? 0x10 : 0x20);
        return;
    }
    if (obj >= 18 && obj <= 24) {           /* 5num inputs */
        static const char *const title[] = {
            "Speed", "", "Loop Begin", "Loop End", "", "Sus. Loop Begin",
            "Sus. Loop End"
        };
        uint32_t *v = obj == 18 ? &s->C5Speed : obj == 20 ? &s->LoopBeg
                    : obj == 21 ? &s->LoopEnd : obj == 23 ? &s->SusLoopBeg
                    : &s->SusLoopEnd;
        if (key == ITK_ENTER || key == ' ') {
            long n = prompt_number(title[obj - 18], *v, 9999999);
            if (n >= 0) {
                *v = (uint32_t)n;
                if (obj == 20 || obj == 21) ls_check_loops(s, 0);
                if (obj == 23 || obj == 24) ls_check_loops(s, 1);
            }
        }
        return;
    }
    {                                       /* thumbbars 25..29 */
        uint8_t *v = obj == 25 ? &s->Vol : obj == 26 ? &s->GvL
                   : obj == 27 ? &s->ViS : obj == 28 ? &s->ViD : &s->ViR;
        int mx = obj == 28 ? 32 : obj == 29 ? 255 : 64;
        if (key >= '0' && key <= '9') {     /* F_PostThumbBar30 */
            int nv;
            if (thumb_value_dialog(key, 0, mx, &nv, ls_draw))
                *v = (uint8_t)nv;
        }
        else if (key == ITK_LEFT && *v > 0) (*v)--;
        else if (key == ITK_RIGHT && *v < mx) (*v)++;
        else if (key == ITK_SHIFT_LEFT || key == ITK_CTRL_LEFT) {
            int d = key == ITK_SHIFT_LEFT ? 4 : 2;      /* issue #7 */
            *v = (uint8_t)(*v > d ? *v - d : 0);
        } else if (key == ITK_SHIFT_RIGHT || key == ITK_CTRL_RIGHT) {
            int d = key == ITK_SHIFT_RIGHT ? 4 : 2;
            *v = (uint8_t)(*v + d < mx ? *v + d : mx);
        }
        else if (key == ITK_HOME) *v = 0;
        else if (key == ITK_END) *v = (uint8_t)mx;
    }
}

/* the focused field's highlight (the objects' Pre functions) */
static void ls_draw_focus(void)
{
    int y = ls_obj_row(LsFocus), k;
    const sample_t *s;

    if (LsN == 0 || LsFocus < 17)
        return;
    s = &LsEnt[LsCur].hdr;
    if (LsFocus == 17) {
        for (k = 0; k < 13; k++) Screen_SetAttr(64 + k, 13, 0x30);
    } else if (LsFocus == 19 || LsFocus == 22) {
        int on = s->Flags & (LsFocus == 19 ? 0x10 : 0x20);
        for (k = 0; k < (on ? 2 : 3); k++) Screen_SetAttr(64 + k, y, 0x30);
    } else if (LsFocus <= 24) {
        for (k = 0; k < 7; k++) Screen_SetAttr(64 + k, y, 0x30);
    } else {                                /* F_PreThumbBar: white thumb */
        if (LsFocus == 25) draw_thumbbar(63, 33, 0, 64, s->Vol, 0x03);
        if (LsFocus == 26) draw_thumbbar(63, 34, 0, 64, s->GvL, 0x03);
        if (LsFocus == 27) draw_thumbbar(63, 37, 0, 64, s->ViS, 0x03);
        if (LsFocus == 28) draw_thumbbar_scaled(63, 38, 0, 32, s->ViD, 8, 0x03);
        if (LsFocus == 29) draw_thumbbar_scaled(63, 39, 0, 255, s->ViR, 8, 0x03);
    }
}

/* the shared screen loop; view = 1 for the Sample Library (Ctrl-F3) */
static void load_sample_screen_run_(int view);

static void load_sample_screen_run(int view)
{
    int keep = FileMode;

    FileMode = 13;
    load_sample_screen_run_(view);
    FileMode = keep;
}

static void load_sample_screen_run_(int view)
{
    /* thumbbar objects 25..29: x=63, rows, ranges; 28/29 are the scaled
     * 8-cell bars */
    static const int tb_y[5]  = { 33, 34, 37, 38, 39 };
    static const int tb_hi[5] = { 64, 64, 64, 32, 255 };
    int drag = -1;

    LsView = view;
    LsFocus = 15;
    ls_set_dir(DirSample[0] ? DirSample : ".");

    while (Running) {
        int key;

        if (drag >= 0 && LsN) {                 /* thumbbar drag */
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b) {
                drag = -1;
            } else {
                sample_t *s = &LsEnt[LsCur].hdr;
                uint8_t *v = drag == 0 ? &s->Vol : drag == 1 ? &s->GvL
                           : drag == 2 ? &s->ViS : drag == 3 ? &s->ViD
                           : &s->ViR;
                int rel = m.px - (63 * 8 + 4), r;
                if (rel < 0) rel = 0;
                if (drag >= 3) {                /* scaled: 8 cells */
                    if (rel > 64) rel = 64;
                    r = (tb_hi[drag] * rel + 32) / 64;
                } else {
                    r = rel;
                }
                *v = (uint8_t)(r > tb_hi[drag] ? tb_hi[drag] : r);
            }
        }

        ls_snapshot();
        ls_draw();
        ls_draw_focus();
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; break; }
        if (key == ITK_ESC)
            break;
        {
            int g = modal_global_key(key, 6);
            if (g == 1) continue;
            if (g == 2) break;
        }

        if (key == ITK_MOUSE) {                 /* M_Object1List mouse */
            it_mouse_t m;
            int i;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 2, 13, 43, 47)) {          /* the list */
                int idx = LsTop + (m.y - 13);
                if (idx >= LsN)
                    continue;
                if (idx == LsCur && LsFocus == 15) {
                    key = ITK_ENTER;            /* click again = Enter */
                } else {
                    LsFocus = 15;
                    if (idx != LsCur && ls_check_modified())
                        LsCur = idx;
                    continue;
                }
            } else if (mouse_in(&m, 46, 13, 53, 22)) {  /* drives */
                int di = LsDrvTop + (m.y - 13);
                LsFocus = 16;
                if (di < LsNDrv) {
                    LsDrvCur = di;
                    ls_enter_drive();
                }
                continue;
            } else if (LsN && mouse_in(&m, 64, 13, 76, 20)) { /* fields */
                LsFocus = 17 + (m.y - 13);
                if (LsFocus == 19 || LsFocus == 22)     /* toggles */
                    ls_field_key(ITK_ENTER);
                continue;
            } else {
                for (i = 0; i < 5; i++)
                    if (LsN && m.y == tb_y[i] && m.x >= 63 && m.x <= 71) {
                        LsFocus = 25 + i;
                        drag = i;
                    }
                continue;
            }
        }

        if (LsFocus >= 17) {                    /* LS*Input objects */
            ls_field_key(key);
            continue;
        }
        if (LsFocus == 16) {                    /* LSDriveWindowKeys */
            switch (key) {
            case ITK_UP:   if (LsDrvCur > 0) LsDrvCur--; break;
            case ITK_DOWN: if (LsDrvCur < LsNDrv - 1) LsDrvCur++; break;
            case ITK_SHIFT_TAB: case ITK_LEFT:  /* DriveWindow_Tab -> 15 */
                LsFocus = 15; break;
            case ITK_TAB: case ITK_RIGHT:       /* LSDriveWindow_Right */
                LsFocus = 17; break;
            case ITK_ENTER: ls_enter_drive(); LsFocus = 15; break;
            default: break;
            }
            continue;
        }

        switch (key) {                          /* LSWindowKeys */
        case ITK_UP: case ITK_DOWN: case ITK_PGUP: case ITK_PGDN:
        case ITK_HOME: case ITK_END:
            if (!ls_check_modified())           /* CheckSampleModified */
                break;
            switch (key) {
            case ITK_UP:   if (LsCur > 0) LsCur--; break;
            case ITK_DOWN: if (LsCur < LsN - 1) LsCur++; break;
            case ITK_PGUP: LsCur = LsCur >= 35 ? LsCur - 35 : 0; break;
            case ITK_PGDN: LsCur = LsCur + 35 < LsN ? LsCur + 35
                                                    : (LsN ? LsN - 1 : 0);
                           break;
            case ITK_HOME: LsCur = 0; break;
            default:       LsCur = LsN ? LsN - 1 : 0; break;
            }
            break;
        case ' ':                               /* LSWindow_Space */
            ls_edit_name();
            break;
        case ITK_RIGHT: case ITK_TAB:           /* FileWindow_ShiftTab */
            LsFocus = 16; break;
        case ITK_DEL:                           /* D_DeleteSampleFile */
            ls_delete_file();
            break;
        case ITK_ENTER: {
            slibent_t *e;
            if (LsN == 0)
                break;
            e = &LsEnt[LsCur];
            if (e->Format == 1) {               /* directory */
                ls_enter_dir(e);
            } else if (e->Format >= 0x20) {     /* module: its samples */
                ls_enter_module(e);
            } else if (!LsView) {               /* LSWindow_EnterSample */
                if (lib_load_sample_entry(e))
                    goto leave;
            }
            break; }
        default:
            /* D_PostLoadSampleWindow: note keys audition the entry
             * through the check slot, stereo menu suppressed
             * (DisableStereoMenu); this is what makes the waveform
             * appear */
            if (LsN && LsEnt[LsCur].Format >= 2 &&
                LsEnt[LsCur].Format < 0x20) {   /* unknown = raw (#17) */
                int (*keep)(void) = Load_StereoChoice;
                Load_StereoChoice = NULL;
                lib_preview_key(&LsEnt[LsCur], LsCur, key);
                Load_StereoChoice = keep;
            }
            break;
        }
    }
leave:
    lib_release_check();                /* drop the preview sample */
}

/* F3 Enter: the Load Sample screen */
static void sample_library_requester(void)
{
    load_sample_screen_run(0);
}

/* ===================================================================
 * Load Instrument / Instrument Library screen (#27) --
 * O1_LoadInstrumentList / O1_ViewInstrumentLibrary (IT_OBJ1.ASM 8115),
 * custom draws from IT_DISK.ASM: D_DrawLoadInstrument 9221,
 * D_PreLoadInstrument 9464, D_LIDrawDriveWindow 6022 /
 * D_LIPreDriveWindow 6071; keys LoadInstrumentKeys / ViewInstrumentKeys
 * / LIDriveWindowKeys (IT_DISK.ASM 845). Until 2026-10 the port showed
 * the module file requester here. As on the Load Sample screen, every
 * file is identified at once (no CACHE.ITI, no idle name loader), so the
 * list is always sorted.
 * Objects: 5 = the list (LoadInstrumentWindow (5,12)-(62,48)),
 *          7 = the drives (LoadInstrumentDriveWindow (63,15)-(72,48)).
 * =================================================================== */
static const char *path_base(const char *p);

#define LI_MAX   999                    /* D_LoadInstrumentFiles cap */
#define LI_ROWS  35                     /* rows 13..47 */
#define LI_DROWS 32                     /* D_LIDrawDriveWindow: 32 rows */

static ilibent_t LiEnt[LI_MAX];
static int  LiN, LiCur, LiTop;
static int  LiFocus = 5;                /* object 5 list, 7 drives */
static int  LiView;                     /* 1 = Instrument Library (Ctrl-F4) */
static int  LiInModule;                 /* InstrumentsInModule */
static char LiDrives[26];
static int  LiNDrv, LiDrvCur, LiDrvTop;

static void li_scan_drives(void)
{
    LiNDrv = 0;
    LiDrvCur = 0;
#ifdef _WIN32
    {
        DWORD drives = GetLogicalDrives();
        int i;
        char cur = (char)toupper((unsigned char)DirInstr[0]);
        for (i = 0; i < 26; i++)
            if (drives & (1u << i)) {
                if ((char)('A' + i) == cur)
                    LiDrvCur = LiNDrv;
                LiDrives[LiNDrv++] = (char)('A' + i);
            }
    }
#else
    LiDrives[LiNDrv++] = '/';
#endif
}

/* D_InitLoadInstruments: InstrumentDirectory = path, (re)read it */
static void li_set_dir(const char *path)
{
    char d[sizeof(DirInstr)];

    if (strlen(path) >= sizeof(d)) {
        status("Path too long.");
        return;
    }
    memcpy(d, path, strlen(path) + 1);
    ls_normalize(d, sizeof(d));
    memcpy(DirInstr, d, sizeof(d));     /* InstrumentDirectory */
    LiN = RI_ListDirectory(DirInstr, LiEnt, LI_MAX);
    if (LiN < 0)
        LiN = 0;
    LiCur = LiTop = 0;
    LiInModule = 0;
    li_scan_drives();
}

/* LIWindow_InInstrument1 / LIWindow_EnterLoadInInstrumentData: the
 * module's instruments on the same screen, behind the
 * ExitInstrumentLibraryDirectory record ("." + Directory); each row
 * carries the module's file name and size 0 (TransferInstrumentName) */
static void li_enter_module(const ilibent_t *m)
{
    ilibent_t mod = *m;
    int n, i;

    n = RI_ScanModule(mod.SrcFile, LiEnt + 1, LI_MAX - 1);
    if (n < 0) {
        status("Unknown instrument source: %.12s", mod.FileName);
        return;
    }
    memset(&LiEnt[0], 0, sizeof(LiEnt[0]));
    LiEnt[0].Format = 1;
    snprintf(LiEnt[0].FileName, sizeof(LiEnt[0].FileName), ".");
    for (i = 0; i < 8; i++) {
        LiEnt[0].Name[i] = (char)154;
        LiEnt[0].Name[17 + i] = (char)154;
    }
    memcpy(LiEnt[0].Name + 8, "Directory", 9);
    for (i = 1; i <= n; i++) {
        memcpy(LiEnt[i].FileName, mod.FileName, sizeof(mod.FileName));
        LiEnt[i].SizeK = 0;
    }
    LiN = n + 1;
    LiCur = LiTop = 0;
    LiInModule = 1;
}

static void li_enter_dir(const ilibent_t *e)
{
    char nd[264];

    if (LiInModule && e->FileName[0] == '.') {
        snprintf(nd, sizeof(nd), "%s", DirInstr);   /* leave the module */
    } else if (!strcmp(e->FileName, "\\")) {
#ifdef _WIN32
        snprintf(nd, sizeof(nd), "%c:\\", DirInstr[0]);
#else
        snprintf(nd, sizeof(nd), "/");
#endif
    } else {
        snprintf(nd, sizeof(nd), "%s", e->SrcFile);
    }
    li_set_dir(nd);
}

/* LI_DriveWindow_Enter: that drive's current directory, back to the list */
static void li_enter_drive(void)
{
#ifdef _WIN32
    char d[MAX_PATH];
    if (_getdcwd(LiDrives[LiDrvCur] - 'A' + 1, d, sizeof(d)))
        li_set_dir(d);
    else
        status("Can't read drive %c:", LiDrives[LiDrvCur]);
#else
    li_set_dir("/");
#endif
    LiFocus = 5;
}

/* D_DeleteInstrumentFile: only instrument files (formats 2..7), not
 * inside a module; O1_ConfirmDelete3, default Cancel */
static void li_delete_file(void)
{
    int i;

    if (LiN == 0 || LiInModule || LiEnt[LiCur].Format <= 1 ||
        LiEnt[LiCur].Format >= 8)
        return;
    if (!confirm_box("Delete file?"))
        return;
    if (remove(LiEnt[LiCur].SrcFile) != 0) {
        status("Can't delete %s.", LiEnt[LiCur].FileName);
        return;
    }
    for (i = LiCur; i < LiN - 1; i++)
        LiEnt[i] = LiEnt[i + 1];
    LiN--;
    if (LiCur >= LiN && LiCur > 0)
        LiCur--;
}

static void li_divider(int x, int y)
{
    Screen_PutChar(x, y, 0xA8, 0x02);           /* 2A8h */
}

/* D_DrawLoadInstrument + D_PreLoadInstrument */
static void li_draw_list(void)
{
    int i, k;

    if (LiN == 0) {
        Screen_DrawString(6, 13, "No files.", 0x05);    /* NoFilesMsg */
        return;
    }
    if (LiTop > LiCur)
        LiTop = LiCur;
    if (LiTop + (LI_ROWS - 1) < LiCur)
        LiTop = LiCur - (LI_ROWS - 1);

    for (i = 0; i < LI_ROWS; i++) {
        int idx = LiTop + i, y = 13 + i;
        const ilibent_t *e;
        uint8_t a;

        if (idx >= LiN) {                       /* empty rows: dividers */
            li_divider(31, y);
            li_divider(44, y);
            li_divider(55, y);
            continue;
        }
        e = &LiEnt[idx];
        drawf(2, y, 0x20, "%03d", idx + 1);     /* PE_ConvAX2Num */
        /* 0 unchecked 6, directory 5, unrecognised 7, instrument 3 */
        a = e->Format == 0 ? 0x06 : e->Format == 1 ? 0x05
          : e->Format == 2 ? 0x07 : 0x03;
        for (k = 0; k < 25; k++) {              /* instrument name */
            uint8_t c = (uint8_t)e->Name[k];
            Screen_PutChar(6 + k, y, c >= 226 ? ' ' : c, a);
        }
        li_divider(31, y);
        for (k = 0; k < 12; k++) {              /* file name, col 32 */
            uint8_t c = (uint8_t)e->FileName[k];
            Screen_PutChar(32 + k, y, c, a);
            if (!c)
                break;
        }
        li_divider(44, y);
        if (e->Format >= 3) {
            if (e->Format >= 8)
                drawf(45, y, a, "\x9A\x9A" "Module" "\x9A\x9A");
            else if (e->NumSamples == 0)
                drawf(45, y, a, "No Samples");
            else if (e->NumSamples == 1)
                drawf(45, y, a, "1 Sample");
            else if (e->NumSamples == 0xFFFF)
                drawf(45, y, a, "???");
            else
                drawf(45, y, a, "%u Samples", (unsigned)e->NumSamples);
            li_divider(55, y);
            drawf(56, y, a, "%uk", (unsigned)e->SizeK);     /* FileSizeMsg */
        } else {
            li_divider(55, y);
        }
    }

    if (LiFocus == 5) {                 /* D_PreLoadInstrument: 56 cells */
        int y = 13 + LiCur - LiTop;
        for (k = 0; k < 56; k++)
            Screen_SetAttr(6 + k, y,
                           Screen_GetCell(6 + k, y).ch == 0xA8 ? 0x32 : 0x30);
    }
}

/* D_LIDrawDriveWindow + D_LIPreDriveWindow */
static void li_draw_drives(void)
{
    int i;

    if (LiDrvTop > LiDrvCur)
        LiDrvTop = LiDrvCur;
    if (LiDrvTop + (LI_DROWS - 1) < LiDrvCur)
        LiDrvTop = LiDrvCur - (LI_DROWS - 1);
    for (i = 0; i < LI_DROWS && LiDrvTop + i < LiNDrv; i++)
        drawf(64, 16 + i, 0x05, "Drive %c:", LiDrives[LiDrvTop + i]);
    if (LiFocus == 7 && LiNDrv) {
        int y = 16 + LiDrvCur - LiDrvTop, k;
        for (k = 0; k < 8; k++)
            Screen_SetAttr(64 + k, y, 0x30);
    }
}

static void li_draw(void)
{
    Screen_Clear(0x20);
    draw_chrome(LiView ? "Instrument Library (Ctrl-F4)" : "Load Instrument");
    Screen_DrawBox(5, 12, 62, 48, 27);          /* LoadInstrumentWindow */
    Screen_DrawBox(63, 15, 72, 48, 27);         /* LoadInstrumentDriveWindow */
    li_draw_list();
    /* FreeSampleMsg: "Available" 13 "Samples: " 0FDh 'D' at (64,13) */
    Screen_DrawString(64, 13, "Available", 0x20);
    drawf(64, 14, 0x20, "Samples: %d", LibUnused);
    li_draw_drives();
}

/* LIWindow_Enter (load) / LIViewWindow_Enter (view): directories and
 * modules open in place; an instrument loads into the current slot
 * (load mode only). Returns 1 when the screen should close. */
static int li_enter(void)
{
    ilibent_t *e;

    if (LiN == 0)
        return 0;
    e = &LiEnt[LiCur];
    if (e->Format == 1) {
        li_enter_dir(e);
        return 0;
    }
    if (e->Format >= 8) {
        li_enter_module(e);
        return 0;
    }
    if (!LiView && e->Format >= 3)
        return lib_load_instrument_entry(e);
    return 0;
}

static void load_instrument_screen_run_(int view, const char *module);

static void load_instrument_screen_run(int view, const char *module)
{
    int keep = FileMode;

    FileMode = 15;
    load_instrument_screen_run_(view, module);
    FileMode = keep;
}

static void load_instrument_screen_run_(int view, const char *module)
{
    LiView = view;
    LiFocus = 5;
    LibUnused = RI_UnusedSamples();     /* D_InitLoadInstruments */
    li_set_dir(DirInstr[0] ? DirInstr : ".");
    if (module) {                       /* opened on a module (Ctrl-O) */
        ilibent_t m;
        memset(&m, 0, sizeof(m));
        snprintf(m.SrcFile, sizeof(m.SrcFile), "%s", module);
        snprintf(m.FileName, sizeof(m.FileName), "%s", path_base(module));
        li_enter_module(&m);
    }

    while (Running) {
        int key;

        li_draw();
        Screen_Update();
        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        if (key == ITK_QUIT) { Running = 0; break; }
        if (key == ITK_ESC)                     /* Glbl_F4 */
            break;
        if (key == ITK_ALT_A + ('S' - 'A'))     /* D_SlowInstrumentSort: */
            continue;                           /* always sorted already */
        {
            int g = modal_global_key(key, 11);  /* SetHelpContext11 */
            if (g == 1) continue;
            if (g == 2) break;
        }

        if (key == ITK_MOUSE) {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (mouse_in(&m, 2, 13, 61, 47)) {          /* the list */
                int idx = LiTop + (m.y - 13);
                if (idx >= LiN)
                    continue;
                if (idx == LiCur && LiFocus == 5) {
                    key = ITK_ENTER;            /* click again = Enter */
                } else {
                    LiFocus = 5;
                    LiCur = idx;
                    continue;
                }
            } else if (mouse_in(&m, 64, 16, 71, 47)) {  /* drives */
                int di = LiDrvTop + (m.y - 16);
                LiFocus = 7;
                if (di < LiNDrv) {
                    LiDrvCur = di;
                    li_enter_drive();
                }
                continue;
            } else {
                continue;
            }
        }

        if (LiFocus == 7) {                     /* LIDriveWindowKeys */
            switch (key) {
            case ITK_UP:   if (LiDrvCur > 0) LiDrvCur--; break;
            case ITK_DOWN: if (LiDrvCur < LiNDrv - 1) LiDrvCur++; break;
            case ITK_LEFT: case ITK_TAB:        /* LIDriveWindow_Tab */
                LiFocus = 5; break;
            case ITK_ENTER: li_enter_drive(); break;
            default: break;
            }
            continue;
        }

        switch (key) {                          /* ViewInstrumentKeys */
        case ITK_UP:   if (LiCur > 0) LiCur--; break;
        case ITK_DOWN: if (LiCur + 1 < LiN) LiCur++; break;
        case ITK_PGUP: LiCur = LiCur >= 35 ? LiCur - 35 : 0; break;
        case ITK_PGDN: LiCur = LiCur + 35 < LiN ? LiCur + 35
                                                : (LiN ? LiN - 1 : 0);
                       break;
        case ITK_HOME: LiCur = 0; break;
        case ITK_END:  LiCur = LiN ? LiN - 1 : 0; break;
        case ITK_DEL:  li_delete_file(); break;
        case ITK_RIGHT: case ITK_TAB:           /* LIViewWindow_Tab */
            LiFocus = 7; break;
        case ITK_ENTER:
            if (li_enter())
                goto leave;
            break;
        default: break;
        }
    }
leave:
    lib_release_check();
}

/* F4 Enter: Load Instrument */
static void instrument_library_requester(void)
{
    load_instrument_screen_run(0, NULL);
}

/* "Save Current" (Ctrl-S): save to the loaded filename without the
 * requester; the original always goes through D_CheckOverWrite.
 * D_SaveSong replaces the extension with IT/S3M per SaveFormat. */
static void quick_save(void)
{
    char name[sizeof(FileSaveName) + 8], *dot;
    FILE *f;

    if (!FileNameDisp[0]) {
        save_requester();
        return;
    }
    snprintf(name, sizeof(name), "%s",
             FileSaveName[0] ? FileSaveName : FileNameDisp);
    dot = strrchr(name, '.');
    if (dot && (size_t)(dot - name) < sizeof(name) - 5)
        strcpy(dot, SaveFormat == 1 ? ".S3M" : ".IT");
    else if (!dot && strlen(name) < sizeof(name) - 5)
        strcat(name, SaveFormat == 1 ? ".S3M" : ".IT");
    f = fopen(name, "rb");
    if (f) {
        fclose(f);
        if (!confirm_overwrite(draw_screen))
            return;
    }
    commit_current_pattern();
    if (save_module_dispatch(name))
        status("Saved.");
    else
        status("Unable to save file");
}

/* ---- Load Module screen (O1_LoadITList and its siblings, IT_OBJ1.ASM
 * 1423): while loading, the body is a LoadBox with the format line and
 * the loader's progress log (D_LoadIT, IT_D_RM.INC 2360); afterwards
 * F_GotoEmptyList shows O1_EmptyList -- the header over an empty body,
 * no title -- until a global key (#40). ---- */
static void load_progress_draw(int row, int what, int n)
{
    static const char *const msg[] = {      /* IT_DISK.ASM 397 */
        "File Header", "Instrument %d", "Sample Header %d", "Sample %d",
        "Pattern %d",
    };
    drawf(4, row, 5, msg[what], n);
    Screen_Update();
}

static const char *load_format_title(const char *path) /* Load*ModuleText */
{
    uint8_t h[0x30];
    size_t n;
    FILE *fp = fopen(path, "rb");

    if (!fp)
        return NULL;
    memset(h, 0, sizeof(h));
    n = fread(h, 1, sizeof(h), fp);
    fclose(fp);
    (void)n;
    if (!memcmp(h, "IMPM", 4))                  return "Impulse Tracker Module";
    if (!memcmp(h, "Extended Module: ", 17))    return "Fast Tracker II Module";
    if (!memcmp(h + 0x2C, "SCRM", 4))           return "Scream Tracker III Module";
    if (!memcmp(h, "MTM", 3))                   return "MultiTracker Module";
    if ((h[0] == 'i' && h[1] == 'f') || (h[0] == 'J' && h[1] == 'N'))
        return "Composer 669 Module";
    return "MOD Format Module";
}

static int load_module_screen(const char *path)    /* D_PostFileLoadWindow */
{
    const char *fmt = load_format_title(path);
    int ok;

    if (fmt) {
        Screen_Clear(0x20);
        draw_chrome("Load Module (F9)");
        Screen_DrawBox(1, 12, 78, 48, 27);      /* LoadBox */
        Screen_DrawString(3, 14, fmt, 0x02);
        fill(3, 15, (int)strlen(fmt), 129, 0x02);
        Screen_Update();
        Load_Progress = load_progress_draw;
    }
    ok = do_load_named(path);
    Load_Progress = NULL;
    if (ok)
        Screen = SCR_EMPTY;                     /* F_GotoEmptyList */
    return ok;
}

static int do_load_named(const char *path)
{
    /* stop_song leaves all slave channels off, so the mixer touches no
     * Song data while the loader replaces it; the importers take the
     * engine lock themselves via Pattern_Pack (so this must not hold
     * it around the whole load). */
    stop_song();
    if (Import_LoadModule(path)) {
        const char *base = path, *p;
        char *q;
        ed_lock();
        Driver->InitSound();
        Music_InitMusic();
        Music_InitStereo();
        Music_InitMixTable();
        Music_InitTempo();
        ed_unlock();
        CurPattern = 0; CurRow = CurChan = CurCol = 0; ListSel = 0;
        load_pattern(0);
        row_hilight_from_song();
        for (p = path; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        snprintf(FileNameDisp, sizeof(FileNameDisp), "%s", base);
        for (q = FileNameDisp; *q; q++)
            if (*q >= 'a' && *q <= 'z')
                *q = (char)(*q - 32);
        FileSaveName[0] = 0;
        return 1;
    }
    return 0;
}

/* ===================================================================
 * System file dialogs (feature 016, issue #26) -- an extension next to
 * IT's own file screens: Ctrl-Shift-F9 / Ctrl-Shift-F10 (global),
 * Ctrl-O on F3 / F4 / an F12 path field. Every result goes through the
 * code the original screens use, and updates the same directory they
 * would (the working directory for modules and instruments, as the
 * F9/F10 requester's chdir; LsDir/DirSample for samples). The dialog
 * itself lives behind the backend (Screen_FileDialog).
 * =================================================================== */
static int ed_dialog(int kind, const char *start, const char *suggest,
                     it_dialog_res_t *r)
{
    it_dialog_req_t q;

    memset(&q, 0, sizeof(q));
    q.kind = kind;
    q.start_dir = start;
    q.suggest_name = suggest;
    q.save_format = SaveFormat;
    if (Screen_FileDialog(&q, r) == IT_DLG_CHOSEN)
        return 1;
    if (r->reason)
        status("%s", r->reason);
    return 0;
}

/* change to a directory we have been in / were handed; silent on
 * failure (the caller has already reported its own result) */
static void cd_back(const char *dir)
{
    if (dir[0] && chdir(dir) != 0)
        return;
}

static const char *path_base(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

/* the folder part of a chosen path ("" = none); 0 if it does not fit */
static int path_dir(const char *p, char *out, size_t cap)
{
    size_t n = (size_t)(path_base(p) - p);

    if (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\') &&
        !(n == 3 && p[1] == ':'))       /* keep "/" and "C:\" */
        n--;
    if (n >= cap)
        return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

/* header File Name from the display form (upper-cased like the F9/F10
 * screens, '?' already in place); Ctrl-S keeps the real name */
static void dialog_set_names(const it_dialog_res_t *r, const char *realbase)
{
    char *q;

    snprintf(FileNameDisp, sizeof(FileNameDisp), "%.*s",   /* display: cut */
             (int)sizeof(FileNameDisp) - 1, path_base(r->display));
    for (q = FileNameDisp; *q; q++)
        if (*q >= 'a' && *q <= 'z')
            *q = (char)(*q - 32);
    snprintf(FileSaveName, sizeof(FileSaveName), "%s", realbase);
}

static void act_dialog_open_module(void)
{
    it_dialog_res_t r;
    char dir[IT_DLG_PATH_MAX], keep[IT_DLG_PATH_MAX];
    const char *base;

    if (!getcwd(keep, sizeof(keep)))
        keep[0] = 0;
    if (!ed_dialog(IT_DLG_OPEN_MODULE, keep, NULL, &r))
        return;
    base = path_base(r.path);
    if (strlen(base) >= sizeof(FileSaveName) || !path_dir(r.path, dir, sizeof(dir))) {
        status("Path too long");
        return;
    }
    if (dir[0] && chdir(dir)) {
        status("Can't change to %s.", dir);
        return;
    }
    if (load_module_screen(base)) {     /* as the F9 requester's Enter */
        dialog_set_names(&r, base);
    } else {
        status("Can't load %s.", path_base(r.display));
        cd_back(keep);
    }
}

static void act_dialog_save_module(void)
{
    it_dialog_res_t r;
    char dir[IT_DLG_PATH_MAX], keep[IT_DLG_PATH_MAX];
    char name[sizeof(FileSaveName) + 8];
    const char *base, *dot;
    int fmt, keepfmt = SaveFormat, ok;
    FILE *f;

    if (!getcwd(keep, sizeof(keep)))
        keep[0] = 0;
    if (!ed_dialog(IT_DLG_SAVE_MODULE, keep,
                   FileSaveName[0] ? FileSaveName
                   : FileNameDisp[0] ? FileNameDisp : "UNTITLED.IT", &r))
        return;
    base = path_base(r.path);
    if (strlen(base) >= sizeof(FileSaveName) - 4 ||
        !path_dir(r.path, dir, sizeof(dir))) {
        status("Path too long");
        return;
    }
    /* the chosen type / typed extension decides; none or an unknown
     * one saves as IT with ".it" appended */
    snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 5, base);
    dot = strrchr(name, '.');            /* (length checked above) */
    if (dot && (Screen_SaveFormatFromName(name) == 1 ||
                (tolower((unsigned char)dot[1]) == 'i' &&
                 tolower((unsigned char)dot[2]) == 't' && dot[3] == 0))) {
        fmt = Screen_SaveFormatFromName(name);
    } else {
        fmt = 0;
        strcat(name, ".it");
    }
    if (dir[0] && chdir(dir)) {
        status("Can't change to %s.", dir);
        return;
    }
    f = fopen(name, "rb");              /* D_CheckOverWrite, as F10 */
    if (f) {
        fclose(f);
        if (!confirm_overwrite(draw_screen)) {
            cd_back(keep);
            return;
        }
    }
    commit_current_pattern();           /* PE_SaveCurrentPattern */
    SaveFormat = fmt;                   /* this save only */
    ok = save_module_dispatch(name);
    SaveFormat = keepfmt;
    if (ok) {
        it_dialog_res_t shown = r;      /* display gets the final name */
        if (strcmp(name, base)) {
            size_t n = strlen(shown.display);
            snprintf(shown.display + n, sizeof(shown.display) - n, ".it");
        }
        dialog_set_names(&shown, name);
        status("Saved.");
    } else {
        status("Unable to save file");
        cd_back(keep);
    }
}

/* Ctrl-O on F3 / F4. Samples: where the library requester's Enter sends
 * a file (lib_open_source): a standalone sample loads into the current
 * slot, a module opens as a library. Instruments: an .ITI/.XI loads into
 * the current slot (LIWindow_Enter), a module opens the Load Instrument
 * screen inside it (#27). The folder becomes SampleDirectory /
 * InstrumentDirectory. */
static void act_dialog_load_slot(int instruments)
{
    it_dialog_res_t r;
    char dir[IT_DLG_PATH_MAX], cwd[IT_DLG_PATH_MAX];
    const char *home = instruments ? DirInstr : DirSample;
    int done = 0, keepmode = ReqLibMode;

    if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = 0;
    if (!ed_dialog(instruments ? IT_DLG_OPEN_INSTRUMENT : IT_DLG_OPEN_SAMPLE,
                   home[0] ? home : cwd, NULL, &r))
        return;
    if (strlen(r.path) >= sizeof(((slibent_t *)0)->SrcFile) ||
        !path_dir(r.path, dir, sizeof(dir)) ||
        strlen(dir) >= sizeof(DirSample)) {     /* = sizeof(DirInstr) */
        status("Path too long");
        return;
    }
    if (instruments) {
        ilibent_t e;
        int f = RI_IdentifyFile(r.path, &e);
        if (f == 0) {
            status("Unknown instrument source: %.12s", path_base(r.display));
            return;
        }
        memcpy(DirInstr, dir, strlen(dir) + 1);     /* InstrumentDirectory */
        if (f >= 8) {
            load_instrument_screen_run(0, r.path);
        } else {
            LibUnused = RI_UnusedSamples();
            lib_load_instrument_entry(&e);
        }
        return;
    }
    ReqLibMode = 1;
    lib_open_source(r.path, &done);
    ReqLibMode = keepmode;
    lib_release_check();
    if (done)
        memcpy(DirSample, dir, strlen(dir) + 1);    /* SampleDirectory */
}

/* Ctrl-O on F12: the focused Module / Sample / Instrument path field */
static void act_dialog_pick_folder(void)
{
    it_dialog_res_t r;
    widget_t *w;
    char *field;

    if (NW <= 0 || FocusIdx[Screen] >= NW)
        return;
    w = &W[FocusIdx[Screen]];
    field = w->type == WT_TEXT ? w->text : NULL;
    if (field != DirModule && field != DirSample && field != DirInstr)
        return;                         /* not a path field: nothing */
    if (!ed_dialog(IT_DLG_PICK_FOLDER, field, NULL, &r))
        return;
    if (strlen(r.path) > (size_t)w->tmax) {
        status("Path too long for this field");
        return;
    }
    memcpy(field, r.path, strlen(r.path) + 1);
}

/* selftest helpers (feature 016): set/clear the dialog test hook */
static void dlg_fake(const char *v)
{
#ifdef _WIN32
    _putenv_s("ITED_DIALOG_FAKE", v ? v : "");
#else
    if (v)
        setenv("ITED_DIALOG_FAKE", v, 1);
    else
        unsetenv("ITED_DIALOG_FAKE");
#endif
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int ca, cb, same = fa && fb;

    while (same) {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb)
            same = 0;
        if (ca == EOF || cb == EOF)
            break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

/* screen column (from the line's start) where `word` begins once a help
 * line is expanded; -1 if absent. 0FFh n c counts n columns, 0FEh a
 * (attribute) none. */
static int help_col_of(const uint8_t *line, const char *word)
{
    uint8_t buf[160];
    int n = 0, i, col = 0;
    size_t wl = strlen(word);

    help_expand(line, buf, &n, (int)sizeof(buf) - 1);
    buf[n] = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == 0xFF) { col += buf[i + 1]; i += 2; continue; }
        if (buf[i] == 0xFE) { i += 1; continue; }
        if ((size_t)(n - i) >= wl && !memcmp(buf + i, word, wl))
            return col;
        col++;
    }
    return -1;
}

/* default (empty) song header, as IT's F_FileNew sets up */
static void song_defaults(void)
{
    int i;

    memset(&Song, 0, sizeof(Song));
    Song.Header.ID = 0x4D504D49u;
    Song.Header.OrdNum = 1;
    Song.Header.PatNum = 1;
    Song.Header.IS = 6;
    Song.Header.IT = 125;
    Song.Header.GV = 128;
    Song.Header.MV = 48;
    Song.Header.Sep = 128;
    /* pristine SongData values (IT_MDATA.ASM line 9: Flags = 9 --
     * stereo + linear slides, sample mode; pans all 32) */
    Song.Header.Flags = ITF_STEREO | ITF_LINEAR_SLIDES;
    /* SongData's orders are 256 x 0FFh: a new song's order list is
     * empty ("---" everywhere), order 0 included (#38) */
    memset(Song.Orders, 255, sizeof(Song.Orders));
    for (i = 0; i < 64; i++) {
        Song.Header.ChnlPan[i] = 32;
        Song.Header.ChnlVol[i] = 64;
    }
    /* blank slots hold the pristine Instrument/SampleHeader templates,
     * as the original's song data area always does */
    Music_ClearAllInstruments();
    for (i = 0; i < MAX_SAMPLES - 1; i++)
        Music_InitSample(&Song.Smp[i]);
}

static void new_song(void)
{
    stop_song();
    ed_lock();
    Music_FreeIT();
    song_defaults();
    msg_reset();                        /* Msg_ResetMessage */
    Save_LoadTime = time(NULL);
    Music_InitMusic();
    Music_InitStereo();
    Music_InitMixTable();
    Music_InitTempo();
    ed_unlock();
    CurPattern = 0;
    CurRow = CurChan = CurCol = 0;
    ListSel = 0;
    CurInstr = 1;
    load_pattern(0);
    FileNameDisp[0] = 0;
    FileSaveName[0] = 0;
}

/* ited.cfg: directories + octave/edit step, written by the F12 "Save
 * all Preferences" button */
static void audio_pref_line(const char *line);

static void load_prefs(void)
{
    FILE *fp = fopen("ited.cfg", "r");
    char line[320];

    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (!strncmp(line, "moduledir=", 10))
            snprintf(DirModule, sizeof(DirModule),
                     "%.*s", (int)sizeof(DirModule) - 1, line + 10);
        else if (!strncmp(line, "sampledir=", 10))
            snprintf(DirSample, sizeof(DirSample),
                     "%.*s", (int)sizeof(DirSample) - 1, line + 10);
        else if (!strncmp(line, "instrdir=", 9))
            snprintf(DirInstr, sizeof(DirInstr),
                     "%.*s", (int)sizeof(DirInstr) - 1, line + 9);
        else if (!strncmp(line, "octave=", 7))
            BaseOctave = atoi(line + 7);
        else if (!strncmp(line, "step=", 5))
            EditStep = atoi(line + 5);
        else if (!strncmp(line, "peconfig=", 9))
            PEConfig = (uint8_t)atoi(line + 9);
        else if (!strncmp(line, "viewdivision=", 13))
            ViewDivision = (uint8_t)(atoi(line + 13) & 1);
        else if (!strncmp(line, "viewtracking=", 13))
            ViewTracking = atoi(line + 13) & 1;
        else if (!strncmp(line, "keyboard_cfg=", 13))
            snprintf(KeyboardCfg, sizeof(KeyboardCfg),
                     "%.*s", (int)sizeof(KeyboardCfg) - 1, line + 13);
        else
            audio_pref_line(line);          /* audio_* (Shift-F5) */
    }
    fclose(fp);
    if (BaseOctave < 0) BaseOctave = 0;
    if (BaseOctave > 8) BaseOctave = 8;
    if (EditStep < 0)  EditStep = 0;
    if (EditStep > 16) EditStep = 16;
}

/* ===================================================================
 * Menus (ESC) -- ported from the IT_OBJ1.ASM object lists
 * (O1_MainMenu, O1_FileMenu, O1_PlayBackMenu, O1_SampleMenu,
 * O1_InstrumentMenu): exact box coordinates, box styles (outer 3/1 +
 * inner 0, items style 28) and item texts. The focused item draws its
 * label in attr 23h (F_PreButtonObject); a frame draws the underlying
 * screen, the menu chain, then presents ONCE (the old draw presented
 * the bare screen and then the menu, which flickered).
 * =================================================================== */
typedef struct menuitem_t {
    const char *text;
    int (*act)(void);          /* returns 1 to close the whole chain */
} menuitem_t;

typedef struct menudef_t {
    int x0, y0, x1, y1, style; /* outer box; inner box inset, style 0 */
    int tx, ty;
    const char *title;
    int ix0, ix1, iy;          /* items at (ix0,iy+3i)-(ix1,iy+2+3i) */
    const menuitem_t *items;
    int n;
} menudef_t;

static void menu_draw(const menudef_t *d, int sel)
{
    int i;

    Screen_DrawBox(d->x0, d->y0, d->x1, d->y1, d->style);
    Screen_DrawBox(d->x0 + 1, d->y0 + 1, d->x1 - 1, d->y1 - 1, 0);
    Screen_DrawString(d->tx, d->ty, d->title, 0x23);
    for (i = 0; i < d->n; i++)
        draw_button_style(d->ix0, d->iy + 3*i, d->ix1, d->iy + 2 + 3*i,
                          28, d->items[i].text, 0, i == sel);
}

static int run_menu(const menudef_t *d, void (*under)(void), int *psel)
{
    int sel = psel ? *psel : 0;

    for (;;) {
        int key, res;

        if (!Running)
            return 1;
        if (psel)
            *psel = sel;
        under();
        menu_draw(d, sel);
        Screen_Update();

        key = ed_get_key();
        if (key == ITK_NONE) { ma_sleep(15); continue; }

        switch (key) {
        case ITK_QUIT: Running = 0; return 1;
        case ITK_ESC:  return 0;
        case ITK_UP:   sel = (sel + d->n - 1) % d->n; break;
        case ITK_DOWN: case ITK_TAB:
                       sel = (sel + 1) % d->n; break;
        case ITK_ENTER:
            if (psel) *psel = sel;
            res = d->items[sel].act();
            if (res) return res;
            break;
        case ITK_MOUSE: {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (m.x < d->x0 || m.x > d->x1 || m.y < d->y0 || m.y > d->y1)
                return 0;               /* click outside closes the menu */
            if (m.x >= d->ix0 && m.x <= d->ix1 && m.y >= d->iy) {
                int i = (m.y - d->iy) / 3;
                if (i >= 0 && i < d->n) {
                    sel = i;
                    if (psel) *psel = sel;
                    res = d->items[i].act();
                    if (res) return res;
                }
            }
            break; }
        default: break;
        }
    }
}

static int  MainMenuSel = 2;            /* defaults to View Patterns */
static void menu_under_screen(void) { draw_screen(); }
static void menu_under_main(void);

/* ---- menu item actions ---- */
static int act_view_patterns(void)
{ sample_to_instrument(); Screen = SCR_PATTERN; return 1; }
static int act_view_orders(void)   { Screen = SCR_ORDER;   return 1; }
static int act_view_vars(void)     { Screen = SCR_VARS;    return 1; }
static int act_help(void)          { help_open(help_context_of(Screen)); return 1; }
static int act_message_editor(void)
{ Screen_DefineHiASCII(); Screen = SCR_MESSAGE; return 1; }

static int act_file_load(void)  { file_requester(); return 1; }
static int act_file_new(void)   { new_song(); status("New song."); return 1; }
static int act_file_save(void)     { quick_save(); return 1; }
static int act_file_save_as(void)  { save_requester(); return 1; }
static int act_file_shell(void) { status("No DOS to shell to."); return 1; }
/* Quit (IT.ASM 857): O1_ConfirmQuit, "Exit Impulse Tracker?", default
 * OK (object 3); Cancel returns to the tracker (issue #5). Shared by
 * Ctrl-Q and the menu. */
static int confirm_quit(void)
{
    return confirm_box_def("Exit Impulse Tracker?", 1);
}

static int act_file_quit(void)
{
    if (confirm_quit())
        Running = 0;
    return 1;
}

static int act_pb_info(void)
{ Screen = SCR_INFO; return 1; }
static int act_pb_song(void)
{ commit_current_pattern(); play_song(); return 1; }
static int act_pb_pattern(void)
{ commit_current_pattern(); play_pattern(); return 1; }
static int act_pb_order(void)
{
    commit_current_pattern();
    ed_lock();
    Music_PlaySong((uint16_t)((Screen == SCR_ORDER && ListSel > 0)
                              ? ListSel : 0));
    ed_unlock();
    return 1;
}
static int act_pb_mark(void)
{
    commit_current_pattern();
    ed_lock();
    Music_PlayPattern(CurPattern, CurRows, (uint16_t)CurRow);
    ed_unlock();
    return 1;
}
static int act_pb_stop(void) { stop_song(); return 1; }
/* Music_ReinitSoundCard (Ctrl-I, #28): DriverReinitSound re-initialises
 * the card; here that is reopening the audio device (helps after the
 * device changed or was unplugged) with the mixer reset. Rate and format
 * stay, so the song keeps playing. */
static int audio_open(void);
static int act_pb_reinit(void)
{
    if (DeviceUp) {                     /* no engine lock held: uninit
                                           waits for the callback */
        ma_device_uninit(&Device);
        DeviceUp = 0;
    }
    ed_lock();
    Driver->InitSound();
    ed_unlock();
    if (audio_open())
        status("Sound driver reinitialised.");
    else
        status("Can't open the audio device.");
    return 1;
}
static void drv_open(void);
static int act_pb_driver(void)
{ drv_open(); return 1; }
static int act_pb_length(void)
{ status("Calculate Length not ported yet."); return 1; }

/* Glbl_SampleToInstrument (IT_G.ASM 934): leaving the sample list of an
 * instrument-mode song, LastInstrument becomes the first instrument
 * whose note table uses that sample (#36) */
static void sample_to_instrument(void)
{
    int i, k;

    if (Screen != SCR_SAMPLES || FileMode || FromFileScreen
        || !(Song.Header.Flags & ITF_INSTRUMENTS))
        return;
    for (i = 0; i < 99; i++)
        for (k = 0; k < 120; k++)
            if (Song.Ins[i].NoteSampleTable[k * 2 + 1] == (uint8_t)CurInstr) {
                CurInstr = i + 1;
                return;
            }
}

static void glbl_f4(void)               /* Glbl_F4: init SampleNumber */
{
    sample_to_instrument();
    NoteSampleNumber = (uint8_t)CurInstr;
    if (CurInstr == 0)
        CurInstr = 1;
    Screen = SCR_INSTRUMENTS;
    ListSel = CurInstr - 1;
}

static int act_smp_list(void)
{ Screen = SCR_SAMPLES; ListSel = CurInstr - 1; return 1; }
static int act_smp_lib(void)           /* Sample Library (Ctrl-F3) */
{ Screen = SCR_SAMPLES; load_sample_screen_run(1); return 1; }
static int act_ins_list(void) { glbl_f4(); return 1; }
static int act_ins_lib(void)           /* Instrument Library (Ctrl-F4) */
{ Screen = SCR_INSTRUMENTS; load_instrument_screen_run(1, NULL); return 1; }

/* ---- submenus (coordinates/texts verbatim from IT_OBJ1.ASM) ---- */
static const menuitem_t FileItems[] = {
    { " Load...           (F9)", act_file_load },
    { " New...        (Ctrl-N)", act_file_new },
    { " Save Current  (Ctrl-S)", act_file_save },
    { " Save As...       (F10)", act_file_save_as },
    { " Shell to DOS  (Ctrl-D)", act_file_shell },
    { " Quit          (Ctrl-Q)", act_file_quit },
};
static const menudef_t FileMenuDef = {
    25, 16, 54, 39, 1, 30, 18, "File Menu", 27, 52, 20, FileItems, 6
};

static const menuitem_t PlayBackItems[] = {
    { " Show Infopage          (F5)", act_pb_info },
    { " Play Song         (Ctrl-F5)", act_pb_song },
    { " Play Pattern           (F6)", act_pb_pattern },
    { " Play from Order  (Shift-F6)", act_pb_order },
    { " Play from Mark/Cursor  (F7)", act_pb_mark },
    { " Stop                   (F8)", act_pb_stop },
    { " Reinit Soundcard   (Ctrl-I)", act_pb_reinit },
    { " Driver Screen    (Shift-F5)", act_pb_driver },
    { " Calculate Length   (Ctrl-P)", act_pb_length },
};
static const menudef_t PlayBackMenuDef = {
    25, 16, 59, 48, 1, 31, 18, "Playback Menu", 27, 57, 20,
    PlayBackItems, 9
};

static const menuitem_t SampleItems[] = {
    { " Sample List          (F3)", act_smp_list },
    { " Sample Library  (Ctrl-F3)", act_smp_lib },
    { " Reload Soundcard (Ctrl-G)", act_pb_reinit },
};
static const menudef_t SampleMenuDef = {
    25, 23, 57, 37, 1, 30, 25, "Sample Menu", 27, 55, 27, SampleItems, 3
};

static const menuitem_t InstrumentItems[] = {
    { " Instrument List          (F4)", act_ins_list },
    { " Instrument Library  (Ctrl-F4)", act_ins_lib },
};
static const menudef_t InstrumentMenuDef = {
    20, 23, 56, 34, 1, 25, 25, "Instrument Menu", 22, 54, 27,
    InstrumentItems, 2
};

static int act_file_menu(void)
{ return run_menu(&FileMenuDef, menu_under_main, NULL); }
static int act_playback_menu(void)
{ return run_menu(&PlayBackMenuDef, menu_under_main, NULL); }
static int act_sample_menu(void)
{ return run_menu(&SampleMenuDef, menu_under_main, NULL); }
static int act_instrument_menu(void)
{ return run_menu(&InstrumentMenuDef, menu_under_main, NULL); }

static const menuitem_t MainItems[] = {
    { " File Menu...",               act_file_menu },
    { " Playback Menu...",           act_playback_menu },
    { " View Patterns        (F2)",  act_view_patterns },
    { " Sample Menu...",             act_sample_menu },
    { " Instrument Menu...",         act_instrument_menu },
    { " View Orders/Panning (F11)",  act_view_orders },
    { " View Variables      (F12)",  act_view_vars },
    { " Message Editor (Shift-F9)",  act_message_editor },
    { " Help!                (F1)",  act_help },
};
static const menudef_t MainMenuDef = {
    6, 14, 38, 46, 3, 12, 16, "Main Menu", 8, 36, 18, MainItems, 9
};

static void menu_under_main(void)
{
    draw_screen();
    menu_draw(&MainMenuDef, MainMenuSel);
}

static void main_menu(void)
{
    run_menu(&MainMenuDef, menu_under_screen, &MainMenuSel);
}

/* ===================================================================
 * Global key dispatch
 * =================================================================== */
static void pattern_click(void)
{
    it_mouse_t m;

    Screen_GetMouse(&m);
    if (NumChansEdit > 0 &&
        m.y >= 15 && m.y <= 46 && m.x >= 5 + ViewWidth &&
        m.x < 5 + ViewWidth + 14 * NumChansEdit) {
        int row = TopRow + (m.y - 15);
        int ch  = LeftChan + (m.x - 5 - ViewWidth) / 14;
        int off = (m.x - 5 - ViewWidth) % 14;

        if (row < (int)CurRows && ch < 64) {
            /* 9 cursor columns at cell offsets 0..2 (note), 2
             * (octave), 4, 5, 7, 8, 10, 11, 12 */
            static const int colmap[14] =
                { 0, 0, 1, 2, 2, 3, 4, 4, 5, 6, 6, 7, 8, 8 };
            CurRow = row;
            CurChan = ch;
            CurCol = colmap[off];
        }
    }
}

/* Glbl_LeftBrace/RightBrace (speed) and Glbl_Left/RightSquareBracket
 * (global volume), IT_G.ASM 805..; returns 1 when the key was one */
static int global_brace_key(int key)
{
    int v;

    ed_lock();
    if (key == '{')      v = Music_IncreaseSpeed();     /* fewer frames */
    else if (key == '}') v = Music_DecreaseSpeed();
    else if (key == '[') v = Music_DecreaseVolume();
    else if (key == ']') v = Music_IncreaseVolume();
    else { ed_unlock(); return 0; }
    ed_unlock();
    if (key == '{' || key == '}')
        status("Speed set to %d frames per row", v);
    else
        status("Global Volume set to %d", v);
    return 1;
}

/* SampleGlobalKeyList / InstrumentGlobalKeyList character entries:
 * ` solos the current sample/instrument (Music_ToggleSolo), < > , .
 * pick the playback channel (I_Decrease/IncreasePlayChannel); then the
 * global keys */
static void list_global_key(int key, int instrument)
{
    switch (key) {
    case '`': {
        int on, n = instrument ? CurInstr : CurInstr - 1;
        ed_lock();
        on = Music_ToggleSolo(instrument, (uint8_t)n);
        ed_unlock();
        if (!on)
            status("Solo disabled");
        else
            status(instrument ? "Solo instrument %d" : "Solo sample %d",
                   CurInstr);
        return;
    }
    case '<': case ',':
        if (ListPlayChannel > 0) ListPlayChannel--;
        status("Using channel %d for playback", ListPlayChannel + 1);
        return;
    case '>': case '.':
        if (ListPlayChannel < 63) ListPlayChannel++;
        status("Using channel %d for playback", ListPlayChannel + 1);
        return;
    default:
        global_brace_key(key);
    }
}

static void handle_global(int key)
{
    ed_sync_key(key);
    switch (key) {
    case ITK_QUIT: Running = 0; return;
    case ITK_ESC:
        if (Screen == SCR_MESSAGE && MsgEdit) {
            handle_message_key(ITK_ESC);    /* edit -> view mode */
            return;
        }
        if (Screen == SCR_INSTRUMENTS && InstrumentEdit) {
            InstrumentEdit = 0;             /* leave name editing */
            return;
        }
        if (Screen == SCR_HELP) {           /* H_HelpESC */
            act_help_done();
            return;
        }
        main_menu();
        return;
    case ITK_SHIFT_F9:                  /* Glbl_Shift_F9: hi-ASCII
                                           charset for the message */
        Screen_DefineHiASCII();
        Screen = SCR_MESSAGE;
        return;
    case ITK_F1:                        /* H_Help, context-sensitive */
        if (Screen != SCR_HELP)
            help_open(help_context_of(Screen));
        return;
    case ITK_CTRL_F3:                   /* Sample Library (feature 015) */
        act_smp_lib();
        return;
    case ITK_CTRL_F1:                   /* keypress table (feature 014) */
        Screen = SCR_KEYS;
        return;
    case ITK_F2:                        /* Glbl_F2 loads the packed-cell
                                           charsets for the small views */
        /* CurrentMode == 2 test: a key handed back by a file screen
         * (F9/F10, Load Sample/Instrument, libraries) comes from that
         * screen's mode -- Glbl_F9 sets CurrentMode 9 -- even though the
         * port's modal screen left Screen as it was (#29) */
        sample_to_instrument();
        if (Screen != SCR_PATTERN || FromFileScreen) {
            Screen_DefineSmallNumbers();
            Screen = SCR_PATTERN;
        } else {
            pe_options_dialog();        /* Glbl_F2_1 (issue #4) */
        }
        return;
    case ITK_F3:  Screen = SCR_SAMPLES; ListSel = CurInstr-1; return;
    case ITK_F4:  glbl_f4(); return;
    case ITK_F11:                       /* Glbl_F11: the order cursor is
                                           the pattern editor's Order,
                                           so it is where G left it
                                           (issue #14) */
        Screen = SCR_ORDER; ListSel = PEOrder; return;
    case ITK_F12: Screen = SCR_VARS; return;
    case ITK_F5: {                      /* Glbl_F5: info page; start the
                                           song only when nothing plays */
        int mode0, anyslave = 0, i;
        ed_lock();
        mode0 = (PlayMode == 0);
        if (mode0)
            for (i = 0; i < MAXSLAVECHANNELS; i++)
                if (SChn[i].Flags & SF_CHAN_ON) { anyslave = 1; break; }
        ed_unlock();
        if (Screen == SCR_INFO) {
            if (mode0) { commit_current_pattern(); play_song(); }
        } else {
            if (mode0 && !anyslave) { commit_current_pattern(); play_song(); }
            Screen_DefineSmallNumbers();    /* S_DefineSmallNumbers   */
            Screen = SCR_INFO;
        }
        return;
    }
    case ITK_F6:
        commit_current_pattern(); pe_store_point(); play_pattern(); return;
    case ITK_F7:  pe_f7(); return;          /* PE_F7: mark or cursor */
    case ITK_F8:  stop_song(); return;
    case ITK_F9:  file_requester(); return;
    case ITK_F10: save_requester(); return;
    case 0x13:    quick_save(); return;     /* Ctrl-S */
    /* -- feature 016 (issue #26): system file dialogs, extensions -- */
    case ITK_CTRL_SHIFT_F9:
        if (Screen != SCR_HELP)
            act_dialog_open_module();
        return;
    case ITK_CTRL_SHIFT_F10:
        if (Screen != SCR_HELP)
            act_dialog_save_module();
        return;
    case 0x0F:                              /* Ctrl-O: unbound in IT */
        if (Screen == SCR_SAMPLES) {
            act_dialog_load_slot(0);
            return;
        }
        if (Screen == SCR_INSTRUMENTS && !InstrumentEdit) {
            act_dialog_load_slot(1);
            return;
        }
        if (Screen == SCR_VARS) {
            act_dialog_pick_folder();
            return;
        }
        break;
    /* -- hotkey audit (2026-09): the rest of the global key list -- */
    case 0x0C: case 0x12:                   /* Ctrl-L / Ctrl-R = Glbl_F9 */
        file_requester(); return;
    case 0x17:    save_requester(); return; /* Ctrl-W = Glbl_F10 */
    case 0x0E:    act_file_new(); return;   /* Ctrl-N: F_NewSong */
    /* #28: the rest of the help's Ctrl keys (Ctrl-D DOSShell is not
     * offered -- no DOS to shell to; its help line is hidden) */
    case 0x05:                              /* Ctrl-E: Refresh -- repaint
                                               everything (no cache files
                                               to reset in the port) */
        Screen_Refresh();
        return;
    case 0x09:    act_pb_reinit(); return;  /* Ctrl-I: Music_ReinitSoundCard */
    case 0x0D:                              /* Ctrl-M: MouseToggle */
        Screen_SetMouseVisible(!Screen_MouseVisible());
        return;
    case 0x10:    act_pb_length(); return;  /* Ctrl-P: Music_TimeSong */
    case ITK_CTRL_F4: act_ins_lib(); return;    /* Glbl_Ctrl_F4 */
    case ITK_SHIFT_F5: drv_open(); return;      /* Glbl_DriverScreen */
    case ITK_CTRL_F5:                       /* Glbl_Ctrl_F5: from order 0 */
        commit_current_pattern(); pe_store_point(); play_song(); return;
    case ITK_SHIFT_F6:                      /* Glbl_Shift_F6: from the
                                               pattern editor's order */
        commit_current_pattern(); pe_store_point();
        ed_lock(); Music_PlaySong((uint16_t)PEOrder); ed_unlock();
        return;
    case ITK_ALT_F11:                       /* Music_ToggleOrderUpdate */
        ed_lock(); OrderLockFlag ^= 1; ed_unlock();
        status(OrderLockFlag ? "Order list locked" : "Order list unlocked");
        return;
    case ITK_MOUSE:
        if (Screen == SCR_PATTERN)
            pattern_click();
        else
            widgets_mouse();
        return;
    default: break;
    }
    if (key >= ITK_ALT_F1 && key <= ITK_ALT_F1 + 7) {  /* Glbl_Alt_F1..F8 */
        ed_lock(); Music_ToggleChannel((uint16_t)(key - ITK_ALT_F1));
        ed_unlock();
        return;
    }
    /* keypad / and * (1B5h/137h): Decrease/IncreaseOctave on every
     * screen; a focused text field takes them as characters instead */
    if (key == ITK_KP_DIVIDE || key == ITK_KP_MULTIPLY) {
        if (Screen != SCR_PATTERN && Screen != SCR_INFO && NW > 0 &&
            FocusIdx[Screen] < NW && W[FocusIdx[Screen]].type == WT_TEXT) {
            widgets_key(key == ITK_KP_DIVIDE ? '/' : '*');
            return;
        }
        if (key == ITK_KP_DIVIDE) { if (BaseOctave > 0) BaseOctave--; }
        else if (BaseOctave < 9) BaseOctave++;
        return;
    }
    /* Ctrl-Left/Right = DisplayMinus/Plus (previous/next order) unless
     * the screen uses them: the pattern editor's views, a focused
     * thumbbar (F_PostThumbBar16), the envelope editor */
    if ((key == ITK_CTRL_LEFT || key == ITK_CTRL_RIGHT) &&
        Screen != SCR_PATTERN) {
        if (Screen == SCR_INFO || Screen == SCR_MESSAGE || !widgets_key(key)) {
            ed_lock();
            if (key == ITK_CTRL_LEFT) Music_LastOrder();
            else                      Music_NextOrder();
            ed_unlock();
        }
        return;
    }

    /* { } [ ]: Glbl_LeftBrace.. (speed / global volume) -- the pattern
     * editor, info page and message viewer have no other use for them;
     * on the widget screens they come after the objects, so a text
     * field still types them */
    if ((key == '{' || key == '}' || key == '[' || key == ']') &&
        (Screen == SCR_PATTERN || Screen == SCR_INFO ||
         (Screen == SCR_MESSAGE && !MsgEdit))) {
        global_brace_key(key);
        return;
    }
    if (Screen == SCR_PATTERN)
        handle_pattern_key(key);
    else if (Screen == SCR_INFO)
        handle_info_key(key);
    else if (Screen == SCR_MESSAGE)
        handle_message_key(key);
    else if (Screen == SCR_SAMPLES) {
        if (!widgets_key(key) &&        /* Alt ops after the widgets */
            !handle_sample_altkey(key))
            list_global_key(key, 0);
    } else if (Screen == SCR_INSTRUMENTS) {
        if (!widgets_key(key) &&        /* note window gets Alt first */
            !handle_instrument_altkey(key))
            list_global_key(key, 1);
    } else if (Screen == SCR_HELP) {
        if (!help_key(key) && !widgets_key(key))
            global_brace_key(key);
    } else if (!widgets_key(key))
        global_brace_key(key);
}

/* ===================================================================
 * Audio + main loop
 * =================================================================== */
static void audio_cb(ma_device *d, void *out, const void *in, ma_uint32 fr)
{
    (void)d; (void)in;
    WAVDriver_RenderAny(out, fr);
}

/* ===================================================================
 * Shift-F5: Miniaudio Driver screen (deliberate extension)
 *
 * The original's Shift-F5 shows the loaded sound card driver's own
 * screen (Glbl_DriverScreen). The port plays through miniaudio with the
 * WAV writer's mixer, so this screen configures that path instead:
 * output device, sample rate (only the rates the selected device reports
 * natively), buffer size and output format -- applied with Apply, as
 * they reopen the device -- plus live mixer options: the Sound Blaster 16
 * driver's output filter and feedback modes (SB16DRV.ASM) and the WAV
 * driver's "Ramp volume at start of sample". Saved to ited.cfg (audio_*)
 * with Save Prefs. Defaults are the original's: system device, 44.1 kHz,
 * 16-bit dithered, no filter/feedback, ramp on.
 * =================================================================== */
static ma_context      AudioCtx;
static int             AudioCtxUp = 0;
static ma_device_info *AudDevs = NULL;
static ma_uint32       AudNDev = 0;

static char     AudDevName[256] = "";       /* "" = system default */
static uint32_t AudRate = 44100;
static int      AudFormat = 0;              /* 0 16-bit, 1 24-bit, 2 float */
static uint32_t AudBuffer = 0;              /* frames, 0 = backend's */
static int      AudFilter = 0, AudFeedback = 0, AudRamp = 1;
static int      AudExclusive = 0;           /* WASAPI exclusive mode */
static int      AudMono = 0;                /* one output channel, mixer
                                               forced to mono */
static int      AudRateSwitch = 0;          /* macOS: set the device's
                                               nominal rate */
static int      AudRateFromCmdline = 0;

static char     AudStatus[4][48];           /* the running device */

#define DRV_MAXRATE 12
static uint8_t  DrvSelDev, DrvSelRate, DrvSelFmt, DrvSelBuf;
static uint8_t  DrvSelFilter, DrvSelFeedback, DrvSelRamp, DrvSelExcl;
static uint8_t  DrvSelMono, DrvSelRateSw;
static int      DrvDevTop;
static uint8_t  DrvOpenDev, DrvOpenRate, DrvOpenFmt, DrvOpenBuf,
                DrvOpenExcl, DrvOpenMono,   /* as opened: Apply pending? */
                DrvOpenRateSw;
static uint32_t DrvRates[DRV_MAXRATE];
static int      DrvNRates;
static char     DrvRateLbl[DRV_MAXRATE][14];
static const uint32_t DrvBuffers[6] = { 0, 256, 512, 1024, 2048, 4096 };
static const char *const DrvBufLbl[6] = {
    " Default", "     256", "     512", "    1024", "    2048", "    4096"
};
static const char *const DrvFmtLbl[5] = {
    " 16 Bit, Dither", "     24 Bit", "  32 Bit Float", "     8 Bit",
    "  8 Bit, Dither"
};

static void audio_pref_line(const char *line)
{
    if (!strncmp(line, "audio_device=", 13))
        snprintf(AudDevName, sizeof(AudDevName), "%s", line + 13);
    else if (!strncmp(line, "audio_rate=", 11))
        AudRate = (uint32_t)strtoul(line + 11, NULL, 10);
    else if (!strncmp(line, "audio_format=", 13))
        AudFormat = atoi(line + 13) % 5;
    else if (!strncmp(line, "audio_buffer=", 13))
        AudBuffer = (uint32_t)strtoul(line + 13, NULL, 10);
    else if (!strncmp(line, "audio_filter=", 13))
        AudFilter = atoi(line + 13) % 3;
    else if (!strncmp(line, "audio_feedback=", 15))
        AudFeedback = atoi(line + 15) % 3;
    else if (!strncmp(line, "audio_ramp=", 11))
        AudRamp = atoi(line + 11) ? 1 : 0;
    else if (!strncmp(line, "audio_exclusive=", 16))
        AudExclusive = atoi(line + 16) ? 1 : 0;
    else if (!strncmp(line, "audio_mono=", 11))
        AudMono = atoi(line + 11) ? 1 : 0;
    else if (!strncmp(line, "audio_rateswitch=", 17))
        AudRateSwitch = atoi(line + 17) ? 1 : 0;
}

static void audio_pref_save(FILE *fp)
{
    fprintf(fp, "audio_device=%s\naudio_rate=%u\naudio_format=%d\n"
            "audio_buffer=%u\naudio_filter=%d\naudio_feedback=%d\n"
            "audio_ramp=%d\naudio_exclusive=%d\naudio_mono=%d\n"
            "audio_rateswitch=%d\n",
            AudDevName, (unsigned)AudRate, AudFormat, (unsigned)AudBuffer,
            AudFilter, AudFeedback, AudRamp, AudExclusive, AudMono,
            AudRateSwitch);
}

static void audio_context(void)
{
    if (!AudioCtxUp && ma_context_init(NULL, 0, NULL, &AudioCtx) == MA_SUCCESS)
        AudioCtxUp = 1;
    if (AudioCtxUp &&
        ma_context_get_devices(&AudioCtx, &AudDevs, &AudNDev, NULL, NULL)
            != MA_SUCCESS) {
        AudDevs = NULL;
        AudNDev = 0;
    }
}

/* exclusive mode exists on Windows' WASAPI only (miniaudio refuses it
 * elsewhere) */
static int audio_has_exclusive(void)
{
#if defined(MA_HAS_WASAPI)
    return AudioCtxUp && AudioCtx.backend == ma_backend_wasapi;
#else
    return 0;
#endif
}

/* macOS has no exclusive mode (miniaudio's CoreAudio backend refuses
 * it); its counterpart is the device's nominal sample rate, the one set
 * in Audio MIDI Setup. miniaudio keeps it and resamples unless
 * coreaudio.allowNominalSampleRateChange is set -- then it switches the
 * device, for every app on it, and macOS keeps that rate afterwards. */
static int audio_has_rateswitch(void)
{
    return AudioCtxUp && AudioCtx.backend == ma_backend_coreaudio;
}

#if defined(MA_HAS_WASAPI)
/* The rates the device accepts in exclusive mode. miniaudio's device info
 * carries a single exclusive format, so each standard rate is asked for
 * directly with IAudioClient::IsFormatSupported(EXCLUSIVE), stereo, in
 * float/32/24/16-bit. This only queries the driver -- the device is not
 * opened and other programs keep playing. Returns the count. */
static int wasapi_exclusive_rates(int devsel, const uint32_t *std, int nstd,
                                  uint32_t *out)
{
    ma_IMMDeviceEnumerator *en = NULL;
    ma_IMMDevice *dev = NULL;
    ma_IAudioClient *ac = NULL;
    HRESULT hr;
    int n = 0, i, f;
    static const struct { int bits, valid, flt; } fm[4] = {
        { 32, 32, 1 }, { 32, 32, 0 }, { 24, 24, 0 }, { 16, 16, 0 }
    };

    hr = ma_CoCreateInstance((&AudioCtx), &MA_CLSID_MMDeviceEnumerator, NULL,
                             CLSCTX_ALL, &MA_IID_IMMDeviceEnumerator,
                             (void **)&en);
    if (FAILED(hr))
        return 0;
    if (devsel > 0 && devsel <= (int)AudNDev)
        hr = ma_IMMDeviceEnumerator_GetDevice(en, AudDevs[devsel - 1].id.wasapi,
                                              &dev);
    else
        hr = ma_IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, ma_eRender,
                                                            ma_eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = ma_IMMDevice_Activate(dev, &MA_IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void **)&ac);
    if (SUCCEEDED(hr)) {
        for (i = 0; i < nstd; i++)
            for (f = 0; f < 4; f++) {
                MA_WAVEFORMATEXTENSIBLE wf;
                memset(&wf, 0, sizeof(wf));
                wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                wf.nChannels = 2;
                wf.nSamplesPerSec = std[i];
                wf.wBitsPerSample = (WORD)fm[f].bits;
                wf.nBlockAlign = (WORD)(2 * fm[f].bits / 8);
                wf.nAvgBytesPerSec = wf.nBlockAlign * std[i];
                wf.cbSize = 22;
                wf.Samples.wValidBitsPerSample = (WORD)fm[f].valid;
                wf.dwChannelMask = 0x3;         /* front left + right */
                wf.SubFormat = fm[f].flt ? MA_GUID_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                         : MA_GUID_KSDATAFORMAT_SUBTYPE_PCM;
                if (SUCCEEDED(ma_IAudioClient_IsFormatSupported(ac,
                        MA_AUDCLNT_SHAREMODE_EXCLUSIVE,
                        (MA_WAVEFORMATEX *)&wf, NULL))) {
                    out[n++] = std[i];
                    break;
                }
            }
        ma_IAudioClient_Release(ac);
    }
    if (dev)
        ma_IMMDevice_Release(dev);
    ma_IMMDeviceEnumerator_Release(en);
    return n;
}
#endif

/* the rates a device reports natively; a device that takes any rate
 * (nativeDataFormats sampleRate 0) gets the standard list; in exclusive
 * mode, the rates it accepts exclusively */
static void drv_add_lofi(void);

static void drv_fill_rates(int devsel)
{
    static const uint32_t std[] = { 22050, 32000, 44100, 48000,
                                    88200, 96000, 176400, 192000 };
    ma_device_info info;
    uint32_t got[64];
    int any = 0, ng = 0, n = 0, i, j;

#if defined(MA_HAS_WASAPI)
    if (DrvSelExcl && audio_has_exclusive()) {
        n = wasapi_exclusive_rates(devsel, std, 8, DrvRates);
        DrvNRates = n;
        drv_add_lofi();
        return;
    }
#endif
    memset(&info, 0, sizeof(info));
    if (AudioCtxUp &&
        ma_context_get_device_info(&AudioCtx, ma_device_type_playback,
                                   devsel > 0 && devsel <= (int)AudNDev
                                       ? &AudDevs[devsel - 1].id : NULL,
                                   &info) == MA_SUCCESS) {
        for (i = 0; i < (int)info.nativeDataFormatCount; i++) {
            uint32_t r = info.nativeDataFormats[i].sampleRate;
            if (r == 0)
                any = 1;
            else if (r >= 8000 && r <= 192000 && ng < 64)
                got[ng++] = r;
        }
    } else
        any = 1;
    if (ng == 0)
        any = 1;
    for (i = 0; i < 8 && n < DRV_MAXRATE; i++) {
        int ok = any;
        for (j = 0; j < ng; j++)
            if (got[j] == std[i]) ok = 1;
        if (ok)
            DrvRates[n++] = std[i];
    }
    for (j = 0; j < ng && n < DRV_MAXRATE; j++) {   /* odd native rates */
        int dup = 0;
        for (i = 0; i < n; i++)
            if (DrvRates[i] == got[j]) dup = 1;
        if (!dup)
            DrvRates[n++] = got[j];
    }
    for (i = 1; i < n; i++)                         /* ascending */
        for (j = i; j > 0 && DrvRates[j - 1] > DrvRates[j]; j--) {
            uint32_t t = DrvRates[j];
            DrvRates[j] = DrvRates[j - 1];
            DrvRates[j - 1] = t;
        }
    DrvNRates = n;
    drv_add_lofi();
}

/* the lo-fi rates are always offered: hardly any device plays them, so
 * the system resamples, but the mixer really runs at them -- that is
 * where the character comes from. Rates the device does not play itself
 * get a '*'. */
static void drv_add_lofi(void)
{
    static const uint32_t lofi[] = { 8000, 11025, 16000, 22050 };
    int native = DrvNRates, i, j, k;

    for (k = 0; k < 4 && DrvNRates < DRV_MAXRATE; k++) {
        int dup = 0;
        for (i = 0; i < DrvNRates; i++)
            if (DrvRates[i] == lofi[k]) dup = 1;
        if (!dup)
            DrvRates[DrvNRates++] = lofi[k];
    }
    for (i = 0; i < DrvNRates; i++) {           /* ascending, keep the
                                                   native flag with it */
        uint32_t r = DrvRates[i];
        int nat = i < native;
        snprintf(DrvRateLbl[i], sizeof(DrvRateLbl[i]), "%6u Hz%s",
                 (unsigned)r, nat ? "" : "*");
    }
    for (i = 1; i < DrvNRates; i++)
        for (j = i; j > 0 && DrvRates[j - 1] > DrvRates[j]; j--) {
            uint32_t t = DrvRates[j];
            char lb[14];
            DrvRates[j] = DrvRates[j - 1];
            DrvRates[j - 1] = t;
            memcpy(lb, DrvRateLbl[j], sizeof(lb));
            memcpy(DrvRateLbl[j], DrvRateLbl[j - 1], sizeof(lb));
            memcpy(DrvRateLbl[j - 1], lb, sizeof(lb));
        }
}

static int drv_pick_rate(uint32_t want)
{
    static const uint32_t pref[] = { 0, 48000, 44100 };
    int i, k;

    for (k = 0; k < 3; k++)
        for (i = 0; i < DrvNRates; i++)
            if (DrvRates[i] == (k ? pref[k] : want))
                return i;
    return 0;
}

static int drv_dev_index(const char *name)
{
    ma_uint32 i;

    if (name[0])
        for (i = 0; i < AudNDev; i++)
            if (!strcmp(AudDevs[i].name, name))
                return (int)i + 1;
    return 0;
}

/* the screen shows the current settings each time it is opened */
static void drv_open(void)
{
    int i;

    audio_context();
    DrvSelDev = (uint8_t)drv_dev_index(AudDevName);
    DrvSelExcl = (uint8_t)(AudExclusive && audio_has_exclusive());
    drv_fill_rates(DrvSelDev);
    DrvSelRate = (uint8_t)drv_pick_rate(AudRate);
    DrvSelFmt = (uint8_t)AudFormat;
    DrvSelBuf = 0;
    for (i = 0; i < 6; i++)
        if (DrvBuffers[i] == AudBuffer)
            DrvSelBuf = (uint8_t)i;
    DrvSelFilter = (uint8_t)AudFilter;
    DrvSelFeedback = (uint8_t)AudFeedback;
    DrvSelRamp = (uint8_t)(AudRamp ? 0 : 1);
    DrvSelMono = (uint8_t)AudMono;
    DrvSelRateSw = (uint8_t)(AudRateSwitch && audio_has_rateswitch());
    DrvDevTop = DrvSelDev > 7 ? DrvSelDev - 7 : 0;
    DrvOpenDev = DrvSelDev; DrvOpenRate = DrvSelRate;
    DrvOpenFmt = DrvSelFmt; DrvOpenBuf = DrvSelBuf;
    DrvOpenExcl = DrvSelExcl; DrvOpenMono = DrvSelMono;
    DrvOpenRateSw = DrvSelRateSw;
    Screen = SCR_DRIVER;
}

static void audio_live_apply(void)
{
    ed_lock();
    WAVDriver_SetSBFilter(AudFilter);
    WAVDriver_SetSBFeedback(AudFeedback);
    WAVDriver_SetStartRamp(AudRamp);
    ed_unlock();
}

/* open the output with the audio_* settings; 1 on success */
static int audio_open(void)
{
    static const ma_format fmts[5] = { ma_format_s16, ma_format_s32,
                                       ma_format_f32, ma_format_u8,
                                       ma_format_u8 };
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    int d = drv_dev_index(AudDevName);

    cfg.playback.pDeviceID = d ? &AudDevs[d - 1].id : NULL;
    cfg.playback.format = fmts[AudFormat];
    cfg.playback.channels = AudMono ? 1 : 2;
    cfg.sampleRate = AudRate;
    cfg.periodSizeInFrames = AudBuffer;
    cfg.playback.shareMode = AudExclusive && audio_has_exclusive()
                           ? ma_share_mode_exclusive : ma_share_mode_shared;
    cfg.coreaudio.allowNominalSampleRateChange =
        (ma_bool32)(AudRateSwitch && audio_has_rateswitch());
    cfg.dataCallback = audio_cb;
    if (ma_device_init(AudioCtxUp ? &AudioCtx : NULL, &cfg, &Device)
            != MA_SUCCESS)
        return 0;
    if (ma_device_start(&Device) != MA_SUCCESS) {
        ma_device_uninit(&Device);
        return 0;
    }
    DeviceUp = 1;
    {
        uint32_t per = Device.playback.internalPeriodSizeInFrames;
        uint32_t irate = Device.playback.internalSampleRate;
        static const char *const fn[5] = { "16 Bit", "24 Bit", "32 Bit Float",
                                           "8 Bit", "8 Bit dith." };
        snprintf(AudStatus[0], sizeof(AudStatus[0]), "%u Hz, %s, %s%s",
                 (unsigned)AudRate, fn[AudFormat], AudMono ? "mono" : "stereo",
                 irate && irate != AudRate ? ", resampled" : "");
        snprintf(AudStatus[1], sizeof(AudStatus[1]),
                 "Buffer %u frames (%.1f ms)", (unsigned)per,
                 irate ? per * 1000.0 / irate : 0.0);
        snprintf(AudStatus[2], sizeof(AudStatus[2]), "%.37s",
                 Device.playback.name[0] ? Device.playback.name : "Default");
        snprintf(AudStatus[3], sizeof(AudStatus[3]), "via %s, %s",
                 AudioCtxUp ? ma_get_backend_name(AudioCtx.backend) : "?",
                 Device.playback.shareMode == ma_share_mode_exclusive
                     ? "exclusive"
                     : audio_has_rateswitch()
                         ? (AudRateSwitch ? "device rate switched"
                                          : "device rate kept")
                         : "shared");
    }
    return 1;
}

/* (re)configure the mixer for AudRate/AudFormat and reopen the device;
 * falls back to the defaults when the choice cannot be opened */
static int audio_restart(void)
{
    int ok;

    if (DeviceUp) {                         /* no engine lock held here:
                                               uninit waits for the
                                               callback, which takes it */
        ma_device_uninit(&Device);
        DeviceUp = 0;
    }
    ed_lock();
    Music_Stop();
    WAVDriver_SetMixSpeed(AudRate);
    AudRate = WAVDriver_GetMixSpeed();
    WAVDriver_SetOutputFormat(AudFormat);
    WAVDriver_SetOutputChannels(AudMono ? 1 : 2);
    WAVDriver_SetForceMono(AudMono);
    Driver->InitSound();
    Music_InitTempo();
    ed_unlock();
    audio_live_apply();
    ok = audio_open();
    if (!ok) {
        AudDevName[0] = 0;
        AudRate = 44100;
        AudFormat = 0;
        AudBuffer = 0;
        AudExclusive = 0;
        AudMono = 0;
        AudRateSwitch = 0;
        ed_lock();
        WAVDriver_SetMixSpeed(AudRate);
        WAVDriver_SetOutputFormat(0);
        WAVDriver_SetOutputChannels(2);
        WAVDriver_SetForceMono(0);
        Driver->InitSound();
        Music_InitTempo();
        ed_unlock();
        if (!audio_open())
            snprintf(AudStatus[0], sizeof(AudStatus[0]), "No audio output");
    }
    return ok;
}

static void act_drv_excl(void);

static void act_drv_device(void)            /* device picked: its rates */
{
    uint32_t keep = DrvNRates ? DrvRates[DrvSelRate] : AudRate;
    drv_fill_rates(DrvSelDev);
    DrvSelRate = (uint8_t)drv_pick_rate(keep);
}

static void act_drv_excl(void)              /* shared/exclusive: rates */
{
    act_drv_device();
}

static void act_drv_live(void)              /* filter/feedback/ramp */
{
    AudFilter = DrvSelFilter;
    AudFeedback = DrvSelFeedback;
    AudRamp = DrvSelRamp == 0;
    audio_live_apply();
}

static void act_drv_apply(void)
{
    if (DrvSelDev > 0 && DrvSelDev <= AudNDev)
        snprintf(AudDevName, sizeof(AudDevName), "%s",
                 AudDevs[DrvSelDev - 1].name);
    else
        AudDevName[0] = 0;
    AudRate = DrvNRates ? DrvRates[DrvSelRate] : 44100;
    AudFormat = DrvSelFmt;
    AudBuffer = DrvBuffers[DrvSelBuf];
    AudExclusive = DrvSelExcl;
    AudMono = DrvSelMono;
    AudRateSwitch = DrvSelRateSw;
    if (audio_restart())
        status("Audio output reopened.");
    else
        status("That output could not be opened; back to the defaults.");
    drv_open();                             /* show what is running */
}

static void act_drv_save(void) { act_save_prefs(); }

static int drv_pending(void)
{
    return DrvSelDev != DrvOpenDev || DrvSelRate != DrvOpenRate ||
           DrvSelFmt != DrvOpenFmt || DrvSelBuf != DrvOpenBuf ||
           DrvSelExcl != DrvOpenExcl || DrvSelMono != DrvOpenMono ||
           DrvSelRateSw != DrvOpenRateSw;
}

/* device names come as UTF-8; shown as ASCII (accents folded, anything
 * else '?'), since the screen font has no accented letters. Draws up to
 * column maxx. */
static void draw_utf8(int x, int y, const char *str, int maxx, uint8_t a)
{
    const unsigned char *p = (const unsigned char *)str;

    while (*p && x <= maxx) {
        uint32_t u;
        int extra;
        if (*p < 0x80)                 { u = *p;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0)  { u = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0)  { u = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0)  { u = *p & 0x07; extra = 3; }
        else                           { p++; continue; }
        p++;
        while (extra-- > 0 && (*p & 0xC0) == 0x80)
            u = (u << 6) | (*p++ & 0x3F);
        /* IT's font has graphics, not accented letters, above 7Fh:
         * fold Latin-1 letters to their base letter (o for o-umlaut) */
        if (u >= 0xC0 && u <= 0xFF) {
            static const char fold[65] =
                "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
                "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
            if (u == 0xDF) {                /* sharp s */
                Screen_PutChar(x++, y, 's', a);
                if (x <= maxx) Screen_PutChar(x++, y, 's', a);
                continue;
            }
            u = (uint32_t)(unsigned char)fold[u - 0xC0];
        }
        Screen_PutChar(x++, y, (uint8_t)(u >= 32 && u < 127 ? u : '?'), a);
    }
}

/* the device list: box (2,14)-(38,23), eight rows, "System Default"
 * first; Up/Down/PgUp/PgDn/Home/End pick, the mouse too */
static void drv_list_draw(int focused)
{
    int i, n = (int)AudNDev + 1;

    Screen_DrawBox(2, 14, 38, 23, 27);
    for (i = 0; i < 8 && DrvDevTop + i < n; i++) {
        int e = DrvDevTop + i;
        uint8_t a = e == DrvSelDev ? (focused ? 0x30 : 0x23) : 0x06;
        int k;
        for (k = 3; k <= 37; k++)
            Screen_PutChar(k, 15 + i, ' ', a);
        draw_utf8(3, 15 + i, e == 0 ? "System Default" : AudDevs[e - 1].name,
                  37, a);
    }
}

static void drv_list_sel(int e)
{
    int n = (int)AudNDev + 1;

    if (e < 0) e = 0;
    if (e > n - 1) e = n - 1;
    if (e == DrvSelDev)
        return;
    DrvSelDev = (uint8_t)e;
    if (DrvSelDev < DrvDevTop) DrvDevTop = DrvSelDev;
    if (DrvSelDev > DrvDevTop + 7) DrvDevTop = DrvSelDev - 7;
    act_drv_device();
}

static int drv_list_key(int key)
{
    switch (key) {
    case ITK_UP:   if (DrvSelDev == 0) return 0;
                   drv_list_sel(DrvSelDev - 1); return 1;
    case ITK_DOWN: if (DrvSelDev >= AudNDev) return 0;
                   drv_list_sel(DrvSelDev + 1); return 1;
    case ITK_PGUP: drv_list_sel(DrvSelDev - 8); return 1;
    case ITK_PGDN: drv_list_sel(DrvSelDev + 8); return 1;
    case ITK_HOME: drv_list_sel(0); return 1;
    case ITK_END:  drv_list_sel((int)AudNDev); return 1;
    default: return 0;
    }
}

static void drv_list_click(const it_mouse_t *m)
{
    if (m->y >= 15 && m->y <= 22)
        drv_list_sel(DrvDevTop + (m->y - 15));
}

static void draw_driver(void)
{
    int i;

    NW = 0;
    Screen_DrawString(2, 13, "Output Device", 0x20);
    wcustom(2, 14, 38, 23, drv_list_draw, drv_list_key, drv_list_click);

    Screen_DrawString(2, 25, "Sample Rate   (* = resampled)", 0x20);
    if (DrvNRates == 0)
        Screen_DrawString(3, 27, "(none in this mode)", 0x23);
    for (i = 0; i < DrvNRates; i++) {
        int c = i % 3, r = i / 3;
        wradio8(3 + 12 * c, 26 + 3 * r, 14 + 12 * c, 28 + 3 * r,
                DrvRateLbl[i], &DrvSelRate, 0xFF, (uint8_t)i);
    }

    if (audio_has_rateswitch()) {
        Screen_DrawString(2, 38, "Device Rate", 0x20);
        wradio8(3, 39, 19, 41, "      Keep", &DrvSelRateSw, 0xFF, 0);
        wradio8(21, 39, 37, 41, "     Switch", &DrvSelRateSw, 0xFF, 1);
    }
    if (audio_has_exclusive()) {
        Screen_DrawString(2, 38, "Device Access", 0x20);
        wradio8(3, 39, 19, 41, "     Shared", &DrvSelExcl, 0xFF, 0)
            ->action = act_drv_excl;
        wradio8(21, 39, 37, 41, "   Exclusive", &DrvSelExcl, 0xFF, 1)
            ->action = act_drv_excl;
    }

    Screen_DrawString(2, 42, "Buffer Size (frames)", 0x20);
    for (i = 0; i < 6; i++) {
        int c = i % 3, r = i / 3;
        wradio8(3 + 12 * c, 43 + 3 * r, 13 + 12 * c, 45 + 3 * r,
                DrvBufLbl[i], &DrvSelBuf, 0xFF, (uint8_t)i);
    }

    Screen_DrawString(41, 13, "Output Format", 0x20);
    for (i = 0; i < 5; i++) {
        int c = i % 2, r = i / 2;
        wradio8(42 + 19 * c, 14 + 3 * r, 58 + 19 * c, 16 + 3 * r,
                DrvFmtLbl[i], &DrvSelFmt, 0xFF, (uint8_t)i);
    }

    Screen_DrawString(41, 24, "Filter mode", 0x20);
    Screen_DrawString(60, 24, "Feedback mode", 0x20);
    {
        static const char *const fl[3] = { "  No Filter", "  50% Filter",
                                           "  75% Filter" };
        static const char *const fb[3] = { "  None", "  50% Separated",
                                           "  50% Crossed" };
        for (i = 0; i < 3; i++) {
            wradio8(42, 25 + 3 * i, 58, 27 + 3 * i, fl[i], &DrvSelFilter,
                    0xFF, (uint8_t)i)->action = act_drv_live;
            wradio8(61, 25 + 3 * i, 77, 27 + 3 * i, fb[i], &DrvSelFeedback,
                    0xFF, (uint8_t)i)->action = act_drv_live;
        }
    }

    Screen_DrawString(41, 35, "Sample Start Ramp", 0x20);   /* the WAV
                              driver's "Ramp volume at start of sample" */
    wradio8(42, 36, 49, 38, "  On", &DrvSelRamp, 0xFF, 0)->action =
        act_drv_live;
    wradio8(51, 36, 58, 38, "  Off", &DrvSelRamp, 0xFF, 1)->action =
        act_drv_live;
    Screen_DrawString(60, 35, "Channels", 0x20);
    wradio8(61, 36, 69, 38, " Stereo", &DrvSelMono, 0xFF, 0);
    wradio8(70, 36, 77, 38, "  Mono", &DrvSelMono, 0xFF, 1);

    wbutton(42, 40, 58, 42, "     Apply", act_drv_apply);
    wbutton(61, 40, 77, 42, "  Save Prefs", act_drv_save);
    if (drv_pending())
        Screen_DrawString(42, 43, "Press Apply to use these settings",
                          0x23);

    for (i = 0; i < 4; i++)                         /* what is running */
        draw_utf8(41, 45 + i, AudStatus[i], 79, 0x21);
    widgets_draw();
}

static volatile int g_sig = 0;
static void on_sig(int s) { (void)s; g_sig = 1; }

int main(int argc, char **argv)
{
    const char *startmod = NULL;
    uint32_t mixspeed = 44100;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) {
            mixspeed = (uint32_t)atoi(argv[++i]);
            AudRateFromCmdline = 1;
        }
        else if (argv[i][0] != '-')
            startmod = argv[i];
    }

    StartTime = time(NULL);

    memset(ViewChannels, 0xFF, sizeof(ViewChannels));   /* default view */

    /* directories default to the startup cwd; ited.cfg overrides */
    {
        char cwd[256] = "";
        if (getcwd(cwd, sizeof(cwd))) {
            snprintf(DirModule, sizeof(DirModule), "%s", cwd);
            snprintf(DirSample, sizeof(DirSample), "%s", cwd);
            snprintf(DirInstr, sizeof(DirInstr), "%s", cwd);
        }
        load_prefs();
    }

    /* Optional layout override (feature 014, FR-009/FR-010). A bad file
     * is reported and ignored -- the host layout stays in effect and
     * startup continues, because losing the whole editor over a broken
     * keyboard table would be a far worse outcome than losing the
     * override. */
    if (KeyboardCfg[0]) {
        const char *why = Key_LoadLayout(KeyboardCfg);
        if (why)
            fprintf(stderr, "ited: keyboard layout '%s' not loaded (%s); "
                    "using the host layout\n", KeyboardCfg, why);
        else
            fprintf(stderr, "ited: keyboard layout '%s' loaded\n",
                    KeyboardCfg);
    }

    WAVDriver_SetMixSpeed(mixspeed);
    mixspeed = WAVDriver_GetMixSpeed();
    Driver = &WAVDriver;
    Driver->InitSound();

    if (startmod) {
        if (!Import_LoadModule(startmod)) {
            fprintf(stderr, "failed to load %s\n", startmod);
            return 1;
        }
        row_hilight_from_song();
        {
            const char *base = startmod, *p;
            char *q;
            for (p = startmod; *p; p++)
                if (*p == '/' || *p == '\\')
                    base = p + 1;
            snprintf(FileNameDisp, sizeof(FileNameDisp), "%s", base);
            for (q = FileNameDisp; *q; q++)
                if (*q >= 'a' && *q <= 'z')
                    *q = (char)(*q - 32);
        }
    } else {
        song_defaults();
    }

    Music_InitMusic();
    Music_InitStereo();
    Music_InitMixTable();
    Music_InitTempo();

    if (ma_mutex_init(&Mutex) != MA_SUCCESS) {
        fprintf(stderr, "mutex init failed\n");
        return 1;
    }
    Engine_Lock = ed_lock;
    Engine_Unlock = ed_unlock;

    load_pattern(0);

    /* Non-interactive: ITED_DUMP=<screen#> -> plain-ASCII dump of that
     * screen; ITED_SHOT=<file.bmp> -> pixel-exact BMP. No terminal or
     * window needed (the cell buffer + rasterizer are backend-free). */
    {
        const char *dump = getenv("ITED_DUMP");
        const char *shot = getenv("ITED_SHOT");
        if (dump || shot) {
            int scr = dump ? atoi(dump) : (getenv("ITED_SHOT_SCREEN")
                            ? atoi(getenv("ITED_SHOT_SCREEN")) : SCR_PATTERN);
            /* NB the capture numbers above SCR_INFO are their own
             * namespace of overlay aids (7 main menu, 8 file requester,
             * 9 message editor, 10/11 library browsers), so they do NOT
             * line up with the SCR_ enum past SCR_INFO. The feature-014
             * keypress table takes the next free one, 12. */
            if (scr >= 0 && scr <= SCR_INFO)
                Screen = scr;
            else if (scr == 9)              /* message editor */
                Screen = SCR_MESSAGE;
            else if (scr == 12)             /* keypress table (Ctrl-F1) */
                Screen = SCR_KEYS;
            else if (scr == 17)             /* Miniaudio Driver (Shift-F5) */
                drv_open();
            if (getenv("ITED_SHOT_HELP"))  /* F1 help context 0..14 */
                HelpContext = atoi(getenv("ITED_SHOT_HELP")) % 15;
            if (getenv("ITED_SHOT_TAB"))   /* F4 tab 0..3 for captures */
                InsTab = (uint8_t)(atoi(getenv("ITED_SHOT_TAB")) & 3);
            if (getenv("ITED_SHOT_SAMPLE"))    /* F3 list selection */
                ListSel = atoi(getenv("ITED_SHOT_SAMPLE")) - 1;
            if (scr != SCR_INFO && getenv("ITED_SHOT_PLAY")) {
                /* play + mix n seconds before capturing any other screen
                 * (the F3/F4 play dots, for instance) */
                static int16_t pbuf2[2048 * 2];
                uint32_t left = (uint32_t)(atoi(getenv("ITED_SHOT_PLAY")) > 0
                                           ? atoi(getenv("ITED_SHOT_PLAY")) : 1)
                                * WAVDriver_GetMixSpeed();
                Music_PlaySong(0);
                while (left) {
                    uint32_t n = left > 2048 ? 2048 : left;
                    WAVDriver_Render(pbuf2, n);
                    left -= n;
                }
            }
            if (scr == SCR_INFO) {         /* Glbl_F5 entry side effect */
                Screen_DefineSmallNumbers();
                /* capture aids: view method for window 0, play state */
                if (getenv("ITED_SHOT_METHOD"))
                    InfoWin[0].method =
                        (uint16_t)(atoi(getenv("ITED_SHOT_METHOD")) % 11);
                if (getenv("ITED_SHOT_PLAY")) {
                    /* ITED_SHOT_PLAY=n: play + mix n seconds headless
                     * so the capture shows real mid-song state */
                    static int16_t pbuf[2048 * 2];
                    int secs = atoi(getenv("ITED_SHOT_PLAY"));
                    uint32_t left = (uint32_t)(secs > 0 ? secs : 1)
                                    * WAVDriver_GetMixSpeed();
                    Music_PlaySong(0);
                    while (left) {
                        uint32_t n = left > 2048 ? 2048 : left;
                        WAVDriver_Render(pbuf, n);
                        left -= n;
                    }
                }
                if (getenv("ITED_SHOT_FOURIER")) {
                    /* capture aid (feature 013): render analyser
                     * frames over live playback and leave the overlay
                     * set for the BMP (use with ITED_SHOT_PLAY) */
                    static uint8_t fpal[768];
                    static int16_t pbuf[2048 * 2];
                    int f, xo = 0;
                    memset(FourPix, 0, sizeof(FourPix));
                    fourier_palette(fpal, 0);
                    Screen_SetOverlay(FourPix, fpal);
                    for (f = 0; f < 480; f++) {
                        WAVDriver_Render(pbuf, 2048);
                        fourier_frame(&xo);
                    }
                }
            }
            if (scr == SCR_PATTERN) {      /* Glbl_F2 entry side effect */
                Screen_DefineSmallNumbers();
                /* capture aid: ITED_SHOT_PEVIEW=1..4 -> the four
                 * Ctrl-Shift view presets, 5 -> a mixed Ctrl-1..5
                 * fast-view layout on channels 0..4 */
                if (getenv("ITED_SHOT_PEVIEW")) {
                    switch (atoi(getenv("ITED_SHOT_PEVIEW"))) {
                    case 1: pe_quick_view_setup(1, 6, 7);   break;
                    case 2: pe_quick_view_setup(2, 9, 10);  break;
                    case 3: pe_quick_view_setup(3, 18, 24); break;
                    case 4: pe_quick_view_setup(4, 24, 36); break;
                    case 5:
                        for (i = 0; i < 5; i++) {
                            CurChan = i;
                            pe_fast_view(i + 1);
                        }
                        CurChan = 0;
                        break;
                    default: break;
                    }
                }
            }
            Screen_Clear(0x20);
            if (scr == 7) {                 /* main menu overlay */
                draw_screen();
                menu_draw(&MainMenuDef, 2);
            } else if (scr == 8) {          /* file requester */
                req_scan();
                draw_file_requester();
            } else if (scr == 16) {         /* Pattern Editor Options */
                it_key_t fk;
                memset(&fk, 0, sizeof(fk));
                fk.flags = ITKF_PRESSED;
                fk.code = ITK_ESC;
                Screen_KeyFeedTest(&fk, 1);
                Screen = SCR_PATTERN;
                pe_options_dialog();
            } else if (scr == 15) {         /* the Quit confirm box */
                it_key_t fk;
                memset(&fk, 0, sizeof(fk));
                fk.flags = ITKF_PRESSED;
                fk.code = ITK_ESC;          /* draw it, then Cancel */
                Screen_KeyFeedTest(&fk, 1);
                Screen = SCR_PATTERN;
                confirm_quit();
            } else if (scr == 13 || scr == 14) { /* Load Sample / Sample
                                                    Library (feature 015) */
                const char *d = getenv("ITED_SHOT_DIR");
                LsView = (scr == 14);
                LsFocus = 15;
                ls_set_dir(d ? d : "testdata");
                if (getenv("ITED_SHOT_ROW"))
                    LsCur = atoi(getenv("ITED_SHOT_ROW"));
                if (LsCur >= LsN) LsCur = LsN ? LsN - 1 : 0;
                if (getenv("ITED_SHOT_AUDITION") && LsN) {
                    ed_sync_key('q');       /* audition -> waveform */
                    lib_preview_key(&LsEnt[LsCur], LsCur, 'q');
                }
                ls_draw();
            } else if (scr == 10 || scr == 11) { /* library browser */
                const char *src = getenv("ITED_SHOT_LIB");
                if (!src)
                    src = "testdata/itdemo.it";
                if (scr == 10) {
                    LibN = RIS_ScanModule(src, SLib, LIB_MAX);
                } else {
                    LibN = RI_ScanModule(src, ILib, LIB_MAX);
                    LibUnused = RI_UnusedSamples();
                }
                if (LibN < 0)
                    LibN = 0;
                LibSel = LibN ? 1 : 0;
                LibTop = 0;
                draw_lib_browser(scr == 11, src);
            } else {
                redraw();
            }
            if (dump) {
                FILE *fp = fopen("screen_dump.txt", "w");
                if (fp) {
                    Screen_DumpPlain(fp);
                    fclose(fp);
                    fprintf(stderr, "wrote screen_dump.txt (screen %d)\n",
                            Screen);
                }
            }
            if (shot) {
                if (Screen_WriteBMP(shot))
                    fprintf(stderr, "wrote %s (screen %d)\n", shot, Screen);
            }
            ma_mutex_uninit(&Mutex);
            Engine_Lock = NULL; Engine_Unlock = NULL;
            Music_FreeIT();
            return 0;
        }
    }

    /* Non-interactive smoke test (build regression). */
    if (getenv("ITED_SELFTEST")) {
        static const int script[] = {
            ITK_F1, ITK_F12,
            /* widgets: song name text, Tab to tempo bar, adjust,
             * navigate to speed, adjust, toggle Old Effects */
            '!', ITK_BACKSPACE, ITK_TAB, ITK_RIGHT, ITK_RIGHT, ITK_LEFT,
            ITK_DOWN, ITK_RIGHT, ITK_SHIFT_TAB, ITK_SHIFT_TAB,
            ITK_F11, ITK_DOWN, ITK_DOWN, '+', '-',
            /* pan column: Tab from the order list, move, slide, mute */
            ITK_TAB, ITK_DOWN, ITK_RIGHT, ITK_LEFT, 'm', ' ', ' ',
            ITK_F3, ITK_DOWN, ITK_TAB, ITK_RIGHT, ITK_LEFT,
            ITK_F4, ITK_UP, ITK_TAB, ITK_ENTER,
            ITK_F2,
            'z','s','x','d','c', ITK_DOWN, '1', ITK_DOWN, '`',
            /* 9-column cursor (feature 009): ins tens/units, vol
             * tens/units, command, param nibbles */
            ITK_RIGHT, ITK_RIGHT, '0','5',
            ITK_RIGHT, ITK_RIGHT, '4','0',
            ITK_RIGHT, ITK_RIGHT, 'a', ITK_RIGHT, '0','4',
            ITK_TAB, 'q','w','e','r','t',
            ITK_SHIFT_TAB,
            ']','[','}','{',
            ',', ',',                       /* mask toggle + restore */
            ITK_ALT_A + 1, ITK_DOWN, ITK_ALT_A + 4,     /* Alt-B/E */
            ITK_ALT_A + 20,                             /* Alt-U */
            ITK_ALT_INS, ITK_ALT_DEL,       /* row verbs + undo push */
            ITK_INS, ITK_DEL, ITK_PGDN, ITK_PGUP, ITK_HOME, ITK_END,
            '=', '-',
            ITK_F6, ITK_F8, ITK_F5, ITK_F8,
        };
        size_t k;
        int gv_wired = -1, f4_ok = 1;
        for (k = 0; k < sizeof(script) / sizeof(script[0]); k++) {
            handle_global(script[k]);
            redraw();
        }

        /* F4 tabs (feature 002): walk all four object lists and poke
         * one widget of each new kind, verifying the instrument data
         * moved (envelope flag toggle, loop node field, node edit). */
        {
            instrument_t *ins;
            env_t *e;
            uint8_t f0;
            int i, envw;

            Screen = SCR_INSTRUMENTS;
            ListSel = 0;
            for (i = 0; i < 4; i++) {       /* render every tab */
                InsTab = (uint8_t)i;
                redraw();
            }
            InsTab = 1;                     /* Volume tab checks */
            redraw();
            ins = cur_ins();
            e = &ins->VEnvelope;
            envw = -1;
            for (i = 0; i < NW; i++)
                if (W[i].type == WT_CUSTOM && W[i].ckey == env_ckey)
                    envw = i;
            f0 = e->Flags;
            for (i = 0; i < NW; i++)        /* envelope On toggle */
                if (W[i].type == WT_TOGGLE && W[i].v8 == &e->Flags &&
                    W[i].bit == 1) {
                    FocusIdx[SCR_INSTRUMENTS] = i;
                    handle_global(ITK_ENTER);
                    break;
                }
            if (((e->Flags ^ f0) & 1) == 0)
                f4_ok = 0;
            if (envw >= 0 && e->Num >= 2) { /* grab + move node 1 */
                int t0;
                FocusIdx[SCR_INSTRUMENTS] = envw;
                handle_global(ITK_RIGHT);   /* node 1 */
                t0 = e->NodePoints[CurrentNode].Tick;
                handle_global(ITK_ENTER);   /* hold */
                handle_global(ITK_LEFT);    /* tick-1 */
                handle_global(ITK_ENTER);   /* release */
                if (CurrentNode == 1 &&
                    e->NodePoints[1].Tick != t0 - 1 &&
                    e->NodePoints[1].Tick > 1)
                    f4_ok = 0;
                redraw();
            }
        }

        /* F5 info page (feature 003): start playback, cycle the focused
         * window through all 11 view methods, split / resize / merge
         * windows, toggle the bar and name modes, and fullscreen. */
        {
            int f5_ok = 1, m0, i;

            handle_global(ITK_F5);          /* play + info page */
            redraw();
            if (Screen != SCR_INFO || PlayMode != 2)
                f5_ok = 0;
            m0 = InfoWin[InfoCurWindow].method;
            for (i = 0; i < 11; i++) {      /* all view methods render */
                handle_global(ITK_PGDN);
                redraw();
            }
            if ((int)InfoWin[InfoCurWindow].method != m0)
                f5_ok = 0;
            handle_global(ITK_INS);         /* split: 3 -> 4 windows */
            redraw();
            if (InfoNumWindows != 4)
                f5_ok = 0;
            handle_global(0x15);            /* Ctrl-U / Ctrl-D resize */
            handle_global(0x04);
            redraw();
            handle_global(ITK_DEL);         /* merge back to 3 */
            redraw();
            if (InfoNumWindows != 3)
                f5_ok = 0;
            handle_global(ITK_TAB);         /* focus cycling wraps */
            handle_global(ITK_TAB);
            handle_global(ITK_TAB);
            if (InfoCurWindow != 0)
                f5_ok = 0;
            handle_global('V');             /* velocity <-> volume bars */
            redraw();
            if (!InfoVelocity)
                f5_ok = 0;
            handle_global('V');
            handle_global('I');             /* instrument names */
            redraw();
            handle_global('I');
            handle_global('Q');             /* mute + unmute channel */
            handle_global('Q');
            handle_global('S');             /* solo + unsolo */
            handle_global('S');
            redraw();
            handle_global(ITK_DEL);         /* down to a single window */
            handle_global(ITK_DEL);
            handle_global(0x06);            /* Ctrl-F fullscreen */
            redraw();
            if (!InfoFullScreen || InfoWin[0].length != 49)
                f5_ok = 0;
            handle_global(0x06);
            redraw();
            if (InfoFullScreen)
                f5_ok = 0;
            handle_global(ITK_F8);          /* stop */
            redraw();
            fprintf(stderr, "ITED selftest: [%s]\n",
                    f5_ok ? "F5 OK" : "F5 FAIL");
        }

        /* Module import (feature 007): load each generated test module,
         * render 2 s (must be non-silent), then convert-and-save via
         * the 004 writer and reload the .IT (US3). */
        {
            static const char *mods[] = {
                "testdata/import_test.s3m", "testdata/import_test.mod",
                "testdata/import_test.mtm", "testdata/import_test.669",
                "testdata/import_test.xm",
            };
            int import_ok = 1;
            size_t mi;

            for (mi = 0; mi < sizeof(mods) / sizeof(mods[0]); mi++) {
                FILE *probe = fopen(mods[mi], "rb");
                if (!probe)
                    continue;           /* generated files absent: skip */
                fclose(probe);
                if (!do_load_named(mods[mi])) {
                    fprintf(stderr, "  import failed: %s\n", mods[mi]);
                    import_ok = 0;
                    continue;
                }
                {
                    static int16_t rbuf[1024 * 2];
                    long acc = 0;
                    int fr, k2;
                    ed_lock();
                    Music_PlaySong(0);
                    ed_unlock();
                    for (fr = 0; fr < 80; fr++) {
                        WAVDriver_Render(rbuf, 1024);
                        for (k2 = 0; k2 < 1024 * 2; k2++)
                            acc += rbuf[k2] < 0 ? -rbuf[k2] : rbuf[k2];
                    }
                    stop_song();
                    if (acc == 0) {
                        fprintf(stderr, "  import silent: %s\n",
                                mods[mi]);
                        import_ok = 0;
                    }
                }
                if (mi == 0) {          /* US3: convert-and-save */
                    commit_current_pattern();
                    if (!Save_ITModule("st_import.it") ||
                        !do_load_named("st_import.it"))
                        import_ok = 0;
                    remove("st_import.it");
                }
            }
            /* restore the original module for the following blocks */
            do_load_named("testdata/itdemo.it");
            fprintf(stderr, "ITED selftest: [%s]\n",
                    import_ok ? "IMPORT OK" : "IMPORT FAIL");
        }

        /* F3 sample editor (feature 005): waveform view + destructive
         * ops verified as involutions / no-ops on real sample data. */
        {
            int f3_ok = 1;
            sample_t *s;
            uint32_t h0 = 0, h1, lb, le, i;

            Screen = SCR_SAMPLES;
            ListSel = 2;                    /* itdemo sample 3 has data */
            redraw();
            s = cur_smp();
            if (!smp_has_data(s))
                f3_ok = 0;
            else {
                const uint8_t *d = (const uint8_t *)s->Data;
                uint32_t bytes = s->Length << ((s->Flags & 2) ? 1 : 0);
                for (i = 0; i < bytes; i++)
                    h0 = h0 * 31 + d[i];
                lb = s->LoopBeg; le = s->LoopEnd;

                smp_op_invert(); smp_op_invert();       /* involution */
                smp_op_reverse(); smp_op_reverse();     /* involution */
                redraw();
                h1 = 0;
                d = (const uint8_t *)s->Data;
                for (i = 0; i < bytes; i++)
                    h1 = h1 * 31 + d[i];
                if (h1 != h0 || s->LoopBeg != lb || s->LoopEnd != le)
                    f3_ok = 0;

                /* loop clamp: End <= Beg clears the loop flag */
                s->Flags |= 0x10;
                s->LoopEnd = s->LoopBeg;
                smp_check_loop();
                if (s->Flags & 0x10)
                    f3_ok = 0;
                s->LoopBeg = lb; s->LoopEnd = le;
                s->Flags |= 0x10;
                smp_check_loop();
                redraw();
            }
            fprintf(stderr, "ITED selftest: [%s]\n",
                    f3_ok ? "F3 OK" : "F3 FAIL");
        }

        /* Save + message editor (feature 004): type into the message
         * editor, save the module, reload it, verify the message and
         * that the saved file loads at all. */
        {
            int save_ok = 1;
            const char *tmp = "st_save.it";

            handle_global(ITK_SHIFT_F9);    /* message editor */
            redraw();
            if (Screen != SCR_MESSAGE)
                save_ok = 0;
            handle_global(ITK_ENTER);       /* edit mode */
            handle_global('H');
            handle_global('i');
            handle_global(ITK_ENTER);       /* CR */
            handle_global('y');
            handle_global('o');
            redraw();
            if (strcmp(IT_MessageData, "Hi\015yo") != 0)
                save_ok = 0;
            commit_current_pattern();
            if (!Save_ITModule(tmp))
                save_ok = 0;
            else if (!do_load_named(tmp))
                save_ok = 0;
            else if (strcmp(IT_MessageData, "Hi\015yo") != 0)
                save_ok = 0;
            remove(tmp);
            fprintf(stderr, "ITED selftest: [%s]\n",
                    save_ok ? "SAVE OK" : "SAVE FAIL");
        }

        /* Sample/instrument library (feature 006): rip-vs-full-load
         * byte equality (through the IT215 decompressor), scans of the
         * 007 test modules, instrument transfer with note-table remap,
         * and the .ITS/.ITI disk round-trips. */
        {
            int lib_ok = 1;
            const char *tmp = "st_lib.it";
            uint8_t keep_fmt = SaveFormat;
            static slibent_t ents[LIB_MAX];
            static ilibent_t ients[LIB_MAX];

            do_load_named("testdata/itdemo.it");

            SaveFormat = 3;             /* IT215-compress the samples */
            commit_current_pattern();
            if (!Save_ITModule(tmp))
                lib_ok = 0;
            SaveFormat = keep_fmt;

            /* 1) rip every sample of the compressed save and compare
             *    byte-for-byte with the fully-loaded song */
            if (lib_ok) {
                int n = RIS_ScanModule(tmp, ents, LIB_MAX), i, j = 0;

                if (n <= 0)
                    lib_ok = 0;
                for (i = 0; i < Song.Header.SmpNum && lib_ok; i++) {
                    sample_t *src = &Song.Smp[i];
                    sample_t got;
                    size_t bytes;

                    if (!(src->Flags & 1) || !src->Length)
                        continue;
                    if (j >= n) {
                        lib_ok = 0;
                        break;
                    }
                    memset(&got, 0, sizeof(got));
                    if (!RIS_LoadSample(&ents[j], &got)) {
                        lib_ok = 0;
                        break;
                    }
                    bytes = (size_t)src->Length
                            << ((src->Flags & 2) ? 1 : 0);
                    if (got.Length != src->Length ||
                        got.LoopBeg != src->LoopBeg ||
                        got.LoopEnd != src->LoopEnd ||
                        got.C5Speed != src->C5Speed ||
                        got.Vol != src->Vol ||
                        memcmp(got.SampleName, src->SampleName, 26) ||
                        !got.Data ||
                        memcmp(got.Data, src->Data, bytes)) {
                        fprintf(stderr, "  lib rip mismatch: smp %d\n",
                                i + 1);
                        lib_ok = 0;
                    }
                    free(got.Data);
                    j++;
                }
            }

            /* 2) every supported sample source scans and rips */
            {
                static const char *mods[] = {
                    "testdata/import_test.s3m", "testdata/import_test.mod",
                    "testdata/import_test.mtm", "testdata/import_test.669",
                    "testdata/import_test.xm",  "testdata/lib_test.ptm",
                    "testdata/lib_test.far",    "testdata/lib_test.krz",
                    "testdata/lib_test.pat",
                };
                size_t mi;
                for (mi = 0; mi < sizeof(mods) / sizeof(mods[0]); mi++) {
                    FILE *probe = fopen(mods[mi], "rb");
                    sample_t got;
                    int n;
                    if (!probe)
                        continue;
                    fclose(probe);
                    n = RIS_ScanModule(mods[mi], ents, LIB_MAX);
                    memset(&got, 0, sizeof(got));
                    if (n < 1 || !RIS_LoadSample(&ents[0], &got) ||
                        !got.Data || got.Length == 0) {
                        fprintf(stderr, "  lib scan failed: %s\n",
                                mods[mi]);
                        lib_ok = 0;
                    }
                    free(got.Data);
                }
            }

            /* 3) .ITS round-trip on a sample with data */
            {
                sample_t *src = &Song.Smp[2];   /* itdemo sample 3 */
                sample_t got;
                size_t bytes = (size_t)src->Length
                               << ((src->Flags & 2) ? 1 : 0);
                int n;

                memset(&got, 0, sizeof(got));
                if (!RIS_SaveITS(src, "st_lib.its") ||
                    (n = RIS_ScanModule("st_lib.its", ents, LIB_MAX)) != 1
                    || ents[0].Format != 2 ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data ||
                    got.Length != src->Length ||
                    memcmp(got.Data, src->Data, bytes)) {
                    fprintf(stderr, "  its roundtrip failed\n");
                    lib_ok = 0;
                }
                free(got.Data);
                remove("st_lib.its");
            }

            /* 3b) standalone WAV (feature 008): scan fields, rip
             *     byte-exact against the generator's deterministic
             *     ramps (stereo rips the left channel), refuse the
             *     negative fixtures, and a RIS_SaveWAV -> re-scan ->
             *     rip round trip. */
            {
                sample_t got;
                int n, k;

                /* mono 8-bit: (k*7)&0xFF unsigned -> ^0x80 signed */
                n = RIS_ScanModule("testdata/lib_test8.wav", ents,
                                   LIB_MAX);
                memset(&got, 0, sizeof(got));
                if (n != 1 || ents[0].Format != 5 ||
                    ents[0].hdr.Length != 256 ||
                    ents[0].hdr.C5Speed != 22050 ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data) {
                    fprintf(stderr, "  wav8 scan/rip failed\n");
                    lib_ok = 0;
                } else {
                    const int8_t *p = (const int8_t *)got.Data;
                    for (k = 0; k < 256; k++)
                        if (p[k] != (int8_t)(((k * 7) & 0xFF) ^ 0x80)) {
                            fprintf(stderr, "  wav8 data mismatch\n");
                            lib_ok = 0;
                            break;
                        }
                }
                free(got.Data);

                /* mono 16-bit: the (k<<7)-16384 ramp, signed as-is */
                n = RIS_ScanModule("testdata/lib_test16.wav", ents,
                                   LIB_MAX);
                memset(&got, 0, sizeof(got));
                if (n != 1 || ents[0].Format != 7 ||
                    ents[0].hdr.Length != 256 ||
                    ents[0].hdr.C5Speed != 44100 ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data) {
                    fprintf(stderr, "  wav16 scan/rip failed\n");
                    lib_ok = 0;
                } else {
                    const int16_t *p = (const int16_t *)got.Data;
                    for (k = 0; k < 256; k++)
                        if (p[k] != (int16_t)((k << 7) - 16384)) {
                            fprintf(stderr, "  wav16 data mismatch\n");
                            lib_ok = 0;
                            break;
                        }
                }
                free(got.Data);

                /* stereo 16-bit: per-channel frame count, left rip */
                n = RIS_ScanModule("testdata/lib_testst.wav", ents,
                                   LIB_MAX);
                memset(&got, 0, sizeof(got));
                if (n != 1 || ents[0].Format != 7 ||
                    ents[0].hdr.Length != 256 ||
                    (ents[0].hdr.Cvt & 32) == 0 ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data ||
                    got.Length != 256) {
                    fprintf(stderr, "  wavst scan/rip failed\n");
                    lib_ok = 0;
                } else {
                    const int16_t *p = (const int16_t *)got.Data;
                    for (k = 0; k < 256; k++)
                        if (p[k] != (int16_t)((k << 7) - 16384)) {
                            fprintf(stderr,
                                    "  wavst left-channel mismatch\n");
                            lib_ok = 0;
                            break;
                        }
                }
                free(got.Data);

                /* negatives: float tag / 24-bit must be refused */
                if (RIS_ScanModule("testdata/lib_testf.wav", ents,
                                   LIB_MAX) > 0 ||
                    RIS_ScanModule("testdata/lib_test24.wav", ents,
                                   LIB_MAX) > 0) {
                    fprintf(stderr, "  wav negative fixture accepted\n");
                    lib_ok = 0;
                }

                /* IFF 8SVX (feature 013): chunk walk incl. the
                 * original's VHDR field quirk (loop 32..48) */
                n = RIS_ScanModule("testdata/lib_test.iff", ents,
                                   LIB_MAX);
                memset(&got, 0, sizeof(got));
                if (n != 1 || ents[0].Format != 17 ||
                    ents[0].hdr.Length != 256 ||
                    ents[0].hdr.LoopBeg != 32 ||
                    ents[0].hdr.LoopEnd != 48 ||
                    !(ents[0].hdr.Flags & 16) ||
                    ents[0].hdr.C5Speed != 16726 ||
                    memcmp(ents[0].hdr.SampleName, "iff fixture!", 12) ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data) {
                    fprintf(stderr, "  iff scan/rip failed\n");
                    lib_ok = 0;
                } else {
                    const int8_t *p = (const int8_t *)got.Data;
                    for (k = 0; k < 256; k++)
                        if (p[k] != (int8_t)((k * 5) & 0xFF)) {
                            fprintf(stderr, "  iff data mismatch\n");
                            lib_ok = 0;
                            break;
                        }
                }
                free(got.Data);

                /* TX Wave (feature 013): 12-bit unpack to 16-bit */
                n = RIS_ScanModule("testdata/lib_test.txw", ents,
                                   LIB_MAX);
                memset(&got, 0, sizeof(got));
                if (n != 1 || ents[0].Format != 13 ||
                    ents[0].hdr.Length != 80 ||
                    ents[0].hdr.LoopBeg != 48 ||
                    ents[0].hdr.LoopEnd != 80 ||
                    ents[0].hdr.Flags != (1 | 2 | 16) ||
                    ents[0].hdr.C5Speed != 33000 ||
                    !RIS_LoadSample(&ents[0], &got) || !got.Data) {
                    fprintf(stderr, "  txw scan/rip failed\n");
                    lib_ok = 0;
                } else {
                    const int16_t *p = (const int16_t *)got.Data;
                    for (k = 0; k < 80; k++)
                        if (p[k] != (int16_t)((k << 5) & 0xFFF0)) {
                            fprintf(stderr, "  txw data mismatch\n");
                            lib_ok = 0;
                            break;
                        }
                }
                free(got.Data);

                /* save -> re-scan -> rip round trip */
                {
                    sample_t *src = &Song.Smp[2];   /* itdemo sample 3 */
                    size_t bytes = (size_t)src->Length
                                   << ((src->Flags & 2) ? 1 : 0);

                    memset(&got, 0, sizeof(got));
                    if (!RIS_SaveWAV(src, "st_lib.wav") ||
                        RIS_ScanModule("st_lib.wav", ents,
                                       LIB_MAX) != 1 ||
                        ents[0].Format != ((src->Flags & 2) ? 7 : 5) ||
                        !RIS_LoadSample(&ents[0], &got) || !got.Data ||
                        got.Length != src->Length ||
                        memcmp(got.Data, src->Data, bytes)) {
                        fprintf(stderr, "  wav roundtrip failed\n");
                        lib_ok = 0;
                    }
                    free(got.Data);
                    remove("st_lib.wav");
                }
            }

            /* 4) instrument paths: synthesize an instrument over sample
             *    3, save as .ITI, load it back into slot 91 (fresh) and
             *    compare the remapped sample's data; then rip the same
             *    instrument out of a re-saved module (in-IT path) and
             *    the XM instrument out of import_test.xm (XI chain). */
            {
                instrument_t *in = &Song.Ins[0];
                sample_t *src = &Song.Smp[2];
                size_t bytes = (size_t)src->Length
                               << ((src->Flags & 2) ? 1 : 0);
                int k, n, r;

                memcpy(in->DOSFileName, "ST_LIB.ITI\0", 12);
                snprintf(in->InstrumentName,
                         sizeof(in->InstrumentName), "lib test");
                for (k = 0; k < 120; k++) {
                    in->NoteSampleTable[k * 2] = (uint8_t)k;
                    in->NoteSampleTable[k * 2 + 1] = 3;
                }
                if (Song.Header.InsNum < 1)
                    Song.Header.InsNum = 1;

                if (!RI_SaveITI(in, 1, "st_lib.iti"))
                    lib_ok = 0;
                n = RI_ScanModule("st_lib.iti", ients, LIB_MAX);
                if (n != 1 || ients[0].Format != 3 ||
                    ients[0].NumSamples != 1)
                    lib_ok = 0;
                else {
                    ListSel = 90;
                    ed_lock();
                    r = RI_LoadInstrument(&ients[0], 90);
                    ed_unlock();
                    if (r != RI_OK)
                        lib_ok = 0;
                    else {
                        int slot =
                            Song.Ins[90].NoteSampleTable[60 * 2 + 1];
                        sample_t *ns = slot ? &Song.Smp[slot - 1] : NULL;
                        if (!ns || !ns->Data ||
                            ns->Length != src->Length ||
                            memcmp(ns->Data, src->Data, bytes)) {
                            fprintf(stderr,
                                    "  iti transfer mismatch\n");
                            lib_ok = 0;
                        }
                    }
                }
                remove("st_lib.iti");

                /* in-IT instrument rip */
                commit_current_pattern();
                if (!Save_ITModule(tmp))
                    lib_ok = 0;
                n = RI_ScanModule(tmp, ients, LIB_MAX);
                ed_lock();
                r = (n >= 1 && ients[0].Format == 5)
                    ? RI_LoadInstrument(&ients[0], 92) : -9;
                ed_unlock();
                if (r != RI_OK ||
                    !Song.Ins[92].NoteSampleTable[60 * 2 + 1]) {
                    fprintf(stderr, "  in-IT instrument rip failed\n");
                    lib_ok = 0;
                }

                /* .XI instrument file (the same conversion chain) */
                {
                    FILE *probe = fopen("testdata/lib_test.xi", "rb");
                    if (probe) {
                        fclose(probe);
                        n = RI_ScanModule("testdata/lib_test.xi",
                                          ients, LIB_MAX);
                        ed_lock();
                        r = (n == 1 && ients[0].Format == 4 &&
                             ients[0].NumSamples == 1)
                            ? RI_LoadInstrument(&ients[0], 94) : -9;
                        ed_unlock();
                        if (r != RI_OK ||
                            !Song.Ins[94].NoteSampleTable[60 * 2 + 1]) {
                            fprintf(stderr, "  xi load failed\n");
                            lib_ok = 0;
                        } else {
                            int slot = Song.Ins[94]
                                       .NoteSampleTable[60 * 2 + 1];
                            if (!Song.Smp[slot - 1].Data ||
                                Song.Smp[slot - 1].Length == 0)
                                lib_ok = 0;
                        }
                    }
                }

                /* in-XM instrument rip (xi_chain) */
                {
                    FILE *probe = fopen("testdata/import_test.xm", "rb");
                    if (probe) {
                        fclose(probe);
                        n = RI_ScanModule("testdata/import_test.xm",
                                          ients, LIB_MAX);
                        ed_lock();
                        r = (n >= 1 && ients[0].Format == 6)
                            ? RI_LoadInstrument(&ients[0], 93) : -9;
                        ed_unlock();
                        if (r != RI_OK) {
                            fprintf(stderr,
                                    "  in-XM instrument rip failed\n");
                            lib_ok = 0;
                        } else {
                            int slot = Song.Ins[93]
                                       .NoteSampleTable[60 * 2 + 1];
                            if (!slot || !Song.Smp[slot - 1].Data)
                                lib_ok = 0;
                        }
                    }
                }
            }
            remove(tmp);

            /* restore a clean itdemo for the remaining blocks */
            do_load_named("testdata/itdemo.it");
            ListSel = 0;
            fprintf(stderr, "ITED selftest: [%s]\n",
                    lib_ok ? "LIB OK" : "LIB FAIL");
        }

        /* Verify the F12 Global/Mixing Volume wiring actually reaches the
         * engine (the bug fixed here): focus each slider and nudge it, then
         * check the runtime GlobalVolume tracks Song.Header.GV. GV defaults
         * to 128 (= range max), so step LEFT to force a real change. */
        {
            int gi = -1, mi = -1, i;
            Screen = SCR_VARS;
            redraw();                       /* build the F12 widget table */
            for (i = 0; i < NW; i++) {
                if (W[i].v8 == &Song.Header.GV) gi = i;
                if (W[i].v8 == &Song.Header.MV) mi = i;
            }
            if (gi >= 0) { FocusIdx[SCR_VARS] = gi; handle_global(ITK_LEFT); }
            if (mi >= 0) { redraw(); FocusIdx[SCR_VARS] = mi;
                           handle_global(ITK_LEFT); }
            gv_wired = (GlobalVolume == Song.Header.GV);
        }

        /* Pattern editing depth (feature 009): drive marking + the
         * block ops on a scratch pattern and assert cell contents,
         * plus an undo revert byte-equality check. Operates on a fresh
         * high pattern so the loaded song is left untouched. */
        {
            int pe_ok = 1;
            uint16_t keep = CurPattern;
            editcell_t *snap;
            size_t n;

            Screen = SCR_PATTERN;
            commit_current_pattern();
            load_pattern(200);              /* empty scratch pattern */
            n = (size_t)CurRows * 64;
            CurRow = CurChan = CurCol = 0;
            BlockMark = 0; free(ClipData); ClipData = NULL;

            /* enter a C-5 note+instrument at (0,0), copy the 1x1 block,
             * paste-overwrite it at row 4, verify it landed */
            CurInstr = 5;
            pe_new_note(60);                /* writes note, advances */
            CurRow = 0;
            mark_begin_chain(0, 0);
            mark_end_chain(0, 0);
            pe_block_copy();
            if (!ClipData || ClipChans != 1 || ClipRows != 1)
                pe_ok = 0;
            CurRow = 4;
            pe_block_overwrite();
            if (!(Grid[4 * 64].mask & CM_NOTE) ||
                Grid[4 * 64].note != 61)    /* stored note = value+1 */
                pe_ok = 0;

            /* transpose the copied cell up a semitone via Alt-Q on a
             * 1-cell mark, then back down */
            CurRow = 4; CurChan = 0;
            BlockMark = 1; BlockLeft = BlockRight = 0;
            BlockTop = BlockBottom = 4;
            pe_semi(1);
            if (Grid[4 * 64].note != 62)
                pe_ok = 0;
            pe_semi(0);
            if (Grid[4 * 64].note != 61)
                pe_ok = 0;

            /* undo revert: snapshot, wipe the block, revert, compare */
            snap = (editcell_t *)malloc(n * sizeof(editcell_t));
            memcpy(snap, Grid, n * sizeof(editcell_t));
            pe_wipe_block();                /* pushes undo type 18 */
            if (Grid[4 * 64].mask & CM_NOTE) /* really cleared? */
                pe_ok = 0;
            if (UndoRing[0].cells && UndoRing[0].type == 18) {
                memcpy(Grid, UndoRing[0].cells,
                       (size_t)UndoRing[0].rows * 64 * sizeof(editcell_t));
                if (memcmp(Grid, snap, n * sizeof(editcell_t)) != 0)
                    pe_ok = 0;
            } else
                pe_ok = 0;
            free(snap);

            /* row insert/delete round-trip on the whole pattern */
            CurRow = 0;
            cn_set(&Grid[0], 40);
            pe_row_insert();                /* row 0 blanked, note -> 1 */
            if ((Grid[0].mask & CM_NOTE) ||
                !(Grid[64].mask & CM_NOTE))
                pe_ok = 0;
            pe_row_delete();
            if (!(Grid[0].mask & CM_NOTE) || Grid[0].note != 41)
                pe_ok = 0;

            /* mask: clear bit 0 (ins) and confirm note entry skips it */
            EditMask = 3;
            CurRow = 10; CurChan = 0; CurCol = 0;
            EditMask ^= MaskChange[2];      /* toggle ins bit via col 2 */
            /* MaskChange[2] == 1 -> EditMask bit0 cleared */
            pe_new_note(50);
            if (Grid[10 * 64].mask & CM_INS)
                pe_ok = 0;
            EditMask = 3;

            /* template stamp: build a 2-row clip (C-5, D-5), copy it,
             * then template-overwrite anchored on E-5 -> the stamp
             * transposes so the first note becomes E-5 */
            CurRow = 20; CurChan = 0; CurCol = 0; CurInstr = 3;
            pe_new_note(60);                /* row 20: C-5 */
            CurRow = 21;
            pe_new_note(62);                /* row 21: D-5 */
            BlockMark = 1; BlockLeft = BlockRight = 0;
            BlockTop = 20; BlockBottom = 21;
            pe_block_copy();
            Template = 1;                   /* overwrite */
            CurRow = 30; CurChan = 0; CurCol = 0;
            pe_template_stamp(64);          /* anchor E-5 (=64) */
            if (Grid[30 * 64].note != 65 || /* E-5 stored = 64+1 */
                Grid[31 * 64].note != 67)   /* D-5 -> F#-5 (+2) */
                pe_ok = 0;
            Template = 0;

            /* multichannel: enable ch0+ch2, note entry from ch0 lands,
             * cursor advances to the next enabled channel */
            memset(MultiChannelInfo, 0, sizeof(MultiChannelInfo));
            MultiChannelInfo[0] = MultiChannelInfo[2] = 1;
            CurRow = 40; CurChan = 0; CurCol = 0;
            EditStep = 0;
            pe_new_note(48);
            if (CurChan != 2)               /* advanced past ch1 */
                pe_ok = 0;
            EditStep = 1;
            memset(MultiChannelInfo, 0, sizeof(MultiChannelInfo));

            free(ClipData); ClipData = NULL;
            free(UndoRing[0].cells);
            memset(UndoRing, 0, sizeof(UndoRing));
            load_pattern(keep);
            CurRow = CurChan = CurCol = 0;
            fprintf(stderr, "ITED selftest: [%s]\n",
                    pe_ok ? "PE OK" : "PE FAIL");
        }

        /* Pattern editor completion (feature 010): length resize with
         * undo, the mute/solo key family, and the view-scheme tables.
         * Runs on scratch pattern 199 (itdemo uses 0..52); the slot is
         * freed afterwards so song data is left untouched. */
        {
            int pe2_ok = 1;
            uint16_t keep = CurPattern;
            uint8_t mutekeep[64];           /* itdemo mutes ch13..63 in
                                             * its own header */
            int i;

            for (i = 0; i < 64; i++)
                mutekeep[i] = (uint8_t)(Song.Header.ChnlPan[i] & 0x80);

            Screen = SCR_PATTERN;
            commit_current_pattern();
            load_pattern(199);              /* empty scratch: 64 rows */
            CurRow = CurChan = CurCol = 0;
            if (CurRows != 64)
                pe2_ok = 0;

            /* -- US1: shrink 64->32 discards, grow ->128 blank-fills,
             * undo snapshot restores the pre-resize contents -- */
            cn_set(&Grid[40 * 64], 40);     /* note at row 40 */
            commit_current_pattern();
            PatternSetLength = 32;
            PatternLengthStart = PatternLengthEnd = 199;
            pe_apply_pattern_length();
            if (CurRows != 32 || CurRow > 31)
                pe2_ok = 0;
            if (!UndoRing[0].cells || UndoRing[0].rows != 64 ||
                !(UndoRing[0].cells[40 * 64].mask & CM_NOTE))
                pe2_ok = 0;                 /* snapshot kept row 40 */
            PatternSetLength = 128;
            pe_apply_pattern_length();
            if (CurRows != 128)
                pe2_ok = 0;
            for (i = 32; i < 128; i++)      /* grown rows are empty */
                if (Grid[i * 64].mask)
                    pe2_ok = 0;
            /* revert via the ring (as the Ctrl-Backspace requester
             * does): restore the 64-row snapshot byte-exact */
            if (UndoRing[1].cells && UndoRing[1].rows == 64) {
                memcpy(Grid, UndoRing[1].cells,
                       (size_t)64 * 64 * sizeof(editcell_t));
                CurRows = 64;
                commit_current_pattern();
                if (!(Grid[40 * 64].mask & CM_NOTE) ||
                    Grid[40 * 64].note != 41)
                    pe2_ok = 0;
            } else
                pe2_ok = 0;

            /* -- US2: mute/solo keys drive the engine state -- */
            CurChan = 2;
            handle_global('\\');            /* mute ch2 */
            if (!(Song.Header.ChnlPan[2] & 0x80))
                pe2_ok = 0;
            handle_global('\\');            /* unmute */
            if (Song.Header.ChnlPan[2] & 0x80)
                pe2_ok = 0;
            handle_global(ITK_ALT_F10);     /* solo ch2 */
            if ((Song.Header.ChnlPan[2] & 0x80) ||
                !(Song.Header.ChnlPan[0] & 0x80))
                pe2_ok = 0;
            handle_global(ITK_ALT_BACKSLASH);   /* unmute all: restores
                                                 * the module's own mute
                                                 * states, nothing more */
            for (i = 0; i < 64; i++)
                if ((Song.Header.ChnlPan[i] & 0x80) != mutekeep[i])
                    pe2_ok = 0;
            handle_global('|');             /* solo ch2 + advance */
            if (CurChan != 3 || (Song.Header.ChnlPan[2] & 0x80))
                pe2_ok = 0;
            handle_global(ITK_ALT_BACKSLASH);
            pe_mute_next();                 /* main-row '/' (135h):
                                             * mute ch3 + advance */
            if (CurChan != 4 || !(Song.Header.ChnlPan[3] & 0x80))
                pe2_ok = 0;
            handle_global('?');             /* back to ch3 + toggle */
            if (CurChan != 3 || (Song.Header.ChnlPan[3] & 0x80))
                pe2_ok = 0;
            handle_global(ITK_ALT_BACKSLASH);
            {                               /* keypad / and * = octave
                                             * (1B5h/137h, global list) */
                int keepoct = BaseOctave;
                BaseOctave = 4;
                handle_global(ITK_KP_DIVIDE);
                if (BaseOctave != 3 || CurChan != 3) pe2_ok = 0;
                handle_global(ITK_KP_MULTIPLY);
                handle_global(ITK_KP_MULTIPLY);
                if (BaseOctave != 5) pe2_ok = 0;
                BaseOctave = keepoct;
            }

            /* -- US3: view-scheme table mutators -- */
            CurChan = 1;
            handle_global(ITK_CTRL_0 + 3);  /* Ctrl-3: method 2 (7 wide) */
            if (ViewChannels[0] != ((2 << 8) | 1) ||
                ViewChannels[1] != 0xFFFF ||
                ViewWidth != 9 || NumChansEdit != 4)
                pe2_ok = 0;
            handle_global(ITK_CTRL_SHIFT_1 + 1);    /* preset 2 */
            for (i = 0; i < 9; i++)
                if (ViewChannels[i] != ((2 << 8) | i))
                    pe2_ok = 0;
            if (ViewChannels[9] != 0xFFFF || !ViewTracking)
                pe2_ok = 0;
            handle_global(ITK_CTRL_0);      /* Ctrl-0: remove ch1 entry */
            if ((ViewChannels[1] & 0xFF) != 2 ||
                ViewChannels[8] != 0xFFFF)
                pe2_ok = 0;
            /* width overflow: 5 full-width entries fit (71), the 6th
             * (85) must be reverted */
            pe_clear_views();
            for (i = 0; i < 6; i++) {
                CurChan = i;
                handle_global(ITK_CTRL_0 + 1);
            }
            if (ViewChannels[5] != 0xFFFF || ViewWidth != 71)
                pe2_ok = 0;
            /* mixed-scheme render + cursor walk across methods */
            CurChan = 0;
            handle_global(ITK_CTRL_0 + 1);  /* keep ch0 full */
            CurChan = 1;
            handle_global(ITK_CTRL_0 + 2);  /* ch1 compressed */
            for (CurChan = 0; CurChan <= 2; CurChan++)
                for (CurCol = 0; CurCol <= 8; CurCol++)
                    redraw();
            CurChan = 0; CurCol = 0;
            /* toggles flip their state bits */
            i = PEConfig;
            pe_toggle_row_hilight();
            if (((PEConfig ^ i) & 2) == 0)
                pe2_ok = 0;
            pe_toggle_row_hilight();
            pe_toggle_division();
            if (ViewDivision != 0)          /* narrow layout: no revert */
                pe2_ok = 0;
            pe_toggle_division();
            pe_clear_views();
            if (NumChansEdit != 5 || ViewWidth != 0 || ViewTracking)
                pe2_ok = 0;

            for (i = 0; i < 10; i++)
                free(UndoRing[i].cells);
            memset(UndoRing, 0, sizeof(UndoRing));
            PatternSetLength = 64;
            free(Song.Patterns[199].PackedData);    /* drop the scratch */
            Song.Patterns[199].PackedData = NULL;
            Song.Patterns[199].Rows = 0;
            Song.Patterns[199].DataLength = 0;
            load_pattern(keep);
            CurRow = CurChan = CurCol = 0;
            redraw();
            fprintf(stderr, "ITED selftest: [%s]\n",
                    pe2_ok ? "PE2 OK" : "PE2 FAIL");
        }

        /* hotkey audit (2026-09): the key-table entries added in one go.
         * Works on scratch pattern 198 and puts everything back. */
        {
            int hk_ok = 1, keep = CurPattern, keepscr = Screen, i;
            int keepins = CurInstr, keepord = PEOrder;
            uint8_t keeplock = OrderLockFlag;

            Screen = SCR_PATTERN;
            load_pattern(198);
            CurChan = 0; CurCol = 0; CurRow = 10;
            /* Ctrl-PgUp/PgDn: top/bottom; Alt-Home/End: minor-hilight page */
            handle_global(ITK_CTRL_PGDN);
            if (CurRow != (int)CurRows - 1) hk_ok = 0;
            handle_global(ITK_CTRL_PGUP);
            if (CurRow != 0) hk_ok = 0;
            handle_global(ITK_ALT_END);
            if (CurRow != (RowHiLight1 ? RowHiLight1 : 16)) hk_ok = 0;
            handle_global(ITK_ALT_HOME);
            if (CurRow != 0) hk_ok = 0;
            /* Ctrl-Up/Down: instrument; Alt-Left/Right: channel */
            CurInstr = 5;
            handle_global(ITK_CTRL_DOWN);
            handle_global(ITK_CTRL_DOWN);
            handle_global(ITK_CTRL_UP);
            if (CurInstr != 6) hk_ok = 0;
            handle_global(ITK_ALT_RIGHT);
            handle_global(ITK_ALT_RIGHT);
            handle_global(ITK_ALT_LEFT);
            if (CurChan != 1) hk_ok = 0;
            CurChan = 0;
            /* Alt-K slides the volume column 0 -> 64 over rows 0..4 */
            for (i = 0; i < 5; i++) {
                cn_set(&Grid[i * 64], 48);
                ci_set(&Grid[i * 64], 1);
            }
            cv_set(&Grid[0], 0);
            cv_set(&Grid[4 * 64], 64);
            BlockMark = 1;
            BlockLeft = BlockRight = 0; BlockTop = 0; BlockBottom = 4;
            handle_global(ITK_ALT_A + ('K' - 'A'));
            if (cv_get(&Grid[2 * 64]) != 32 || cv_get(&Grid[1 * 64]) != 16)
                hk_ok = 0;
            handle_global(ITK_ALT_A + ('K' - 'A'));   /* twice: wipe */
            if (cv_get(&Grid[0]) != 0xFF || cv_get(&Grid[4 * 64]) != 0xFF)
                hk_ok = 0;
            BlockMark = 0;
            /* Alt-F doubles the block over rows 0..7 (row k -> 2k, blank
             * between), Alt-G halves it back; notes 10..13 in rows 0..3 */
            for (i = 0; i < 8; i++)
                cell_clear(&Grid[i * 64]);
            for (i = 0; i < 4; i++)
                cn_set(&Grid[i * 64], (uint8_t)(10 + i));
            BlockMark = 1;
            BlockLeft = BlockRight = 0; BlockTop = 0; BlockBottom = 3;
            handle_global(ITK_ALT_A + ('F' - 'A'));
            for (i = 0; i < 8; i++)
                if (cn_get(&Grid[i * 64]) !=
                    ((i & 1) ? 253 : (uint8_t)(10 + i / 2)))
                    hk_ok = 0;
            handle_global(ITK_ALT_A + ('G' - 'A'));
            for (i = 0; i < 4; i++)
                if (cn_get(&Grid[i * 64]) != (uint8_t)(10 + i))
                    hk_ok = 0;
            BlockMark = 0;
            /* { } speed, [ ] global volume (Glbl_LeftBrace..) */
            {
                uint16_t sp0, gv0;
                ed_lock(); sp0 = CurrentSpeed; gv0 = GlobalVolume; ed_unlock();
                handle_global('}');
                handle_global(']');
                ed_lock();
                if (CurrentSpeed != (sp0 < 255 ? sp0 + 1 : sp0) ||
                    GlobalVolume != (gv0 < 128 ? gv0 + 1 : gv0))
                    hk_ok = 0;
                ed_unlock();
                handle_global('{');
                handle_global('[');
                ed_lock();
                if (CurrentSpeed != sp0 || GlobalVolume != gv0) hk_ok = 0;
                ed_unlock();
                Song.Header.IS = (uint8_t)sp0;
            }
            /* 2*Alt-N: Multichannel Selection; Down, Space toggles ch 2 */
            {
                static const int seq[] = { ITK_DOWN, ' ', ITK_ESC };
                it_key_t fk;
                int k2;
                uint8_t m1 = MultiChannelInfo[1];
                for (k2 = 0; k2 < 3; k2++) {
                    memset(&fk, 0, sizeof(fk));
                    fk.flags = ITKF_PRESSED; fk.code = seq[k2];
                    fk.ch = (uint16_t)(seq[k2] < 256 ? seq[k2] : 0);
                    Screen_KeyFeedTest(&fk, 1);
                }
                CurChan = 0;
                pe_multichannel_menu();
                if (MultiChannelInfo[1] == m1) hk_ok = 0;
                MultiChannelInfo[1] = m1;
            }
            /* Alt-Enter stores, Alt-Backspace reverts to the stored copy */
            cv_set(&Grid[0], 20);
            handle_global(ITK_ALT_ENTER);
            cv_set(&Grid[0], 40);
            handle_global(ITK_ALT_BACKSPACE);
            if (cv_get(&Grid[0]) != 20) hk_ok = 0;
            /* Ctrl-V: default volumes shown as 191 vv 192 */
            handle_global(0x16);
            if (!PEDefaultVolume) hk_ok = 0;
            handle_global(0x16);
            if (PEDefaultVolume) hk_ok = 0;
            /* Ctrl-F7 toggles the mark; F7 plays from it, Ctrl-F6 from
             * the cursor (pattern 198 is in no order: pattern loop) */
            CurRow = 12;
            handle_global(ITK_CTRL_F7);
            if (!PlayMarkOn || PlayMarkRow != 12) hk_ok = 0;
            CurRow = 3;
            handle_global(ITK_F7);
            ed_lock();
            if (PlayMode != 1 || CurrentPattern != 198 || CurrentRow != 12)
                hk_ok = 0;
            ed_unlock();
            handle_global(ITK_CTRL_F6);
            ed_lock();
            if (PlayMode != 1 || CurrentRow != 3) hk_ok = 0;
            ed_unlock();
            CurRow = 12;
            handle_global(ITK_CTRL_F7);         /* same row: cleared */
            if (PlayMarkOn) hk_ok = 0;
            stop_song();
            /* F7 on a pattern in the order list plays the song on */
            PEOrder = 0;
            load_pattern(Song.Orders[0]);
            CurRow = 2;
            handle_global(ITK_F7);
            ed_lock();
            if (PlayMode != 2 || CurrentOrder != 0 || CurrentRow != 2)
                hk_ok = 0;
            ed_unlock();
            stop_song();
            /* Shift-grey +/-: four patterns */
            load_pattern(10);
            handle_global(ITK_SHIFT_PLUS);
            if (CurPattern != 14) hk_ok = 0;
            handle_global(ITK_SHIFT_MINUS);
            handle_global(ITK_SHIFT_MINUS);
            handle_global(ITK_SHIFT_MINUS);
            if (CurPattern != 2) hk_ok = 0;
            /* global: Alt-F1 toggles channel 1, Alt-F11 the order lock */
            {
                uint8_t m0 = MuteChannelTable[0];
                handle_global(ITK_ALT_F1);
                if (MuteChannelTable[0] == m0) hk_ok = 0;
                handle_global(ITK_ALT_F1);
                if (MuteChannelTable[0] != m0) hk_ok = 0;
            }
            handle_global(ITK_ALT_F11);
            if (OrderLockFlag == keeplock) hk_ok = 0;
            OrderLockFlag = keeplock;
            /* issues #21/#22: F1 opens the calling screen's help page
             * (IT_H.ASM data) and Done/Esc return to that screen */
            Screen = SCR_SAMPLES;
            handle_global(ITK_F1);
            if (Screen != SCR_HELP || HelpContext != 2) hk_ok = 0;
            handle_global(ITK_DOWN);
            if (HelpTop != 1) hk_ok = 0;
            handle_global(ITK_ESC);
            if (Screen != SCR_SAMPLES) hk_ok = 0;
            handle_global(ITK_F1);                  /* position kept */
            if (HelpTop != 1) hk_ok = 0;
            act_help_done();
            HelpPositions[2] = 0;
            /* issue #13: F2 inside the Load Sample screen leaves it and
             * lands on the pattern editor */
            {
                it_key_t fk;
                memset(&fk, 0, sizeof(fk));
                fk.flags = ITKF_PRESSED; fk.code = ITK_F2;
                Screen_KeyFeedTest(&fk, 1);
                Screen = SCR_SAMPLES;
                sample_library_requester();
                if (PendingGlobalKey != ITK_F2) hk_ok = 0;
                PendingGlobalKey = 0;
                handle_global(ITK_F2);
                if (Screen != SCR_PATTERN) hk_ok = 0;
            }
            /* Shift-F5 extensions: 96 kHz, 24-bit and float output, the
             * SB16 filter and feedback render sane audio; the screen
             * builds; everything goes back to 44.1 kHz / 16-bit after */
            {
                static int32_t wide[4096 * 2];
                int fmt, nz = 0, bad = 0;
                ed_lock();
                WAVDriver_SetMixSpeed(96000);
                Driver->InitSound();
                Music_InitTempo();
                Music_PlaySong(0);
                ed_unlock();
                for (fmt = 1; fmt <= 2; fmt++) {
                    int k2;
                    WAVDriver_SetOutputFormat(fmt);
                    WAVDriver_SetSBFilter(fmt);
                    WAVDriver_SetSBFeedback(fmt);
                    for (k2 = 0; k2 < 8; k2++)
                        WAVDriver_RenderAny(wide, 4096);
                    for (k2 = 0; k2 < 4096 * 2; k2++) {
                        if (fmt == 2) {
                            float f;
                            memcpy(&f, &wide[k2], 4);
                            if (!(f >= -1.0f && f <= 1.0f)) bad = 1;
                            if (f != 0.0f) nz = 1;
                        } else {
                            if (wide[k2] & 0xFF) bad = 1;   /* 24 bits */
                            if (wide[k2]) nz = 1;
                        }
                    }
                }
                if (bad || !nz || WAVDriver_GetMixSpeed() != 96000)
                    hk_ok = 0;
                /* lo-fi: 11025 Hz, 8-bit (truncated, then dithered), one
                 * channel with the mixer forced to mono: bytes around the
                 * 80h centre, not all silent */
                {
                    static uint8_t b8[4096];
                    int k2, lo = 255, hi = 0;
                    ed_lock();
                    Music_Stop();
                    WAVDriver_SetMixSpeed(11025);
                    WAVDriver_SetForceMono(1);
                    WAVDriver_SetOutputChannels(1);
                    Driver->InitSound();
                    Music_InitTempo();
                    Music_PlaySong(0);
                    ed_unlock();
                    for (fmt = 3; fmt <= 4; fmt++) {
                        WAVDriver_SetOutputFormat(fmt);
                        for (k2 = 0; k2 < 6; k2++)
                            WAVDriver_RenderAny(b8, 4096);
                        for (k2 = 0; k2 < 4096; k2++) {
                            if (b8[k2] < lo) lo = b8[k2];
                            if (b8[k2] > hi) hi = b8[k2];
                        }
                    }
                    if (hi - lo < 8 || lo > 0x80 || hi < 0x80)
                        hk_ok = 0;
                    WAVDriver_SetForceMono(0);
                    WAVDriver_SetOutputChannels(2);
                }
                ed_lock();
                Music_Stop();
                WAVDriver_SetOutputFormat(0);
                WAVDriver_SetSBFilter(0);
                WAVDriver_SetSBFeedback(0);
                WAVDriver_SetMixSpeed(44100);
                Driver->InitSound();
                Music_InitTempo();
                ed_unlock();
                drv_open();
                redraw();
                if (Screen != SCR_DRIVER || NW < 12 || DrvNRates < 1)
                    hk_ok = 0;
                Screen = SCR_PATTERN;
            }
            /* issue #12: F3 as I_DrawSampleList -- number at x=2, divider
             * 168 at x=30, "Play" at x=31, the played dot at x=1 */
            {
                int keepsel = ListSel;
                Screen = SCR_SAMPLES;
                ListSel = 0;
                SmpListTop = 0;
                memset(SamplePlayTable, 0, sizeof(SamplePlayTable));
                SamplePlayTable[1] = 4;             /* sample 02: played */
                redraw();
                if (Screen_GetCell(2, 13).ch != '0' ||
                    Screen_GetCell(3, 13).ch != '1' ||
                    Screen_GetCell(30, 13).ch != 168 ||
                    Screen_GetCell(31, 13).ch != 'P' ||
                    Screen_GetCell(34, 13).ch != 'y' ||
                    Screen_GetCell(1, 14).ch != 173 ||
                    Screen_GetCell(1, 13).ch == 173)
                    hk_ok = 0;
                memset(SamplePlayTable, 0, sizeof(SamplePlayTable));
                ListSel = keepsel;
                Screen = SCR_PATTERN;
            }
            /* Alt+M on F3/F4 is the Alt op, not the piano key M */
            memset(&CurKey, 0, sizeof(CurKey));
            CurKey.flags = ITKF_PRESSED | ITKF_LALT;
            CurKey.scan = 0x32;
            if (key_to_note_plain() != -1) hk_ok = 0;
            CurKey.flags = ITKF_PRESSED;
            if (key_to_note_plain() <= 0) hk_ok = 0;
            memset(&CurKey, 0, sizeof(CurKey));
            /* issue #7 on the F11 pan bars: Shift 4, Ctrl 2 */
            {
                uint8_t keeppan = Song.Header.ChnlPan[0];
                PanSel = 0;
                pan_set(0, 32);
                pan_col_lkey(ITK_SHIFT_RIGHT, 0);
                pan_col_lkey(ITK_CTRL_LEFT, 0);
                if ((Song.Header.ChnlPan[0] & 0x7F) != 34) hk_ok = 0;
                Song.Header.ChnlPan[0] = keeppan;
            }
            /* issue #14: G from order row 2, then F11 lands on row 2 */
            Screen = SCR_ORDER;
            ListSel = 1;
            FocusIdx[SCR_ORDER] = 0;
            redraw();
            handle_global('G');
            if (Screen != SCR_PATTERN || CurPattern != Song.Orders[1])
                hk_ok = 0;
            handle_global(ITK_F11);
            if (Screen != SCR_ORDER || ListSel != 1) hk_ok = 0;
            Screen = SCR_PATTERN;

            free(Song.Patterns[198].PackedData);
            Song.Patterns[198].PackedData = NULL;
            Song.Patterns[198].Rows = 0;
            Song.Patterns[198].DataLength = 0;
            for (i = 0; i < 10; i++)
                free(UndoRing[i].cells);
            memset(UndoRing, 0, sizeof(UndoRing));
            load_pattern((uint16_t)keep);
            CurRow = CurChan = CurCol = 0;
            CurInstr = keepins;
            PEOrder = keepord;
            Screen = keepscr;
            fprintf(stderr, "ITED selftest: [%s]\n",
                    hk_ok ? "HOT OK" : "HOT FAIL");
        }

        /* Terminal input parser (feature 011): drive the shared byte
         * parser directly -- no tty needed, platform-neutral. */
        {
            int t_ok = 1, got;
            it_mouse_t m;
            static const struct { const char *seq; int key; } tk[] = {
                { "\x1b" "b",       ITK_ALT_A + 1 },        /* Alt-B  */
                { "\x1b" "3",       ITK_ALT_0 + 3 },
                { "\x1b" "\\",      ITK_ALT_BACKSLASH },
                { "\x1b[1;5C",      ITK_CTRL_RIGHT },
                { "\x1b[1;2A",      ITK_SHIFT_UP },
                { "\x1b[1;5H",      ITK_CTRL_HOME },
                { "\x1b[5;5~",      ITK_CTRL_PGUP },
                { "\x1b[6;2~",      ITK_SHIFT_PGDN },
                { "\x1b[3;3~",      ITK_ALT_DEL },
                { "\x1b[2;5~",      ITK_CTRL_INS },
                { "\x1b[20;2~",     ITK_SHIFT_F9 },
                { "\x1b[20;3~",     ITK_ALT_F9 },
                { "\x1b[21;3~",     ITK_ALT_F10 },
                { "\x1b[18;5~",     ITK_CTRL_F7 },
                { "\x1b[1;5Q",      ITK_CTRL_F2 },
                { "\x1b[27;5;51~",  ITK_CTRL_0 + 3 },   /* mOK Ctrl-3 */
                { "\x1b[51;5u",     ITK_CTRL_0 + 3 },   /* CSI-u form */
                { "\x1b[52;6u",     ITK_CTRL_SHIFT_1 + 3 },
                { "\x1b[33;6u",     ITK_CTRL_SHIFT_1 }, /* shifted '!' */
                { "\x1b[27;5;127~", ITK_CTRL_BACKSPACE },
                { "\x1b[A",         ITK_UP },
                { "\x1bOP",         ITK_F1 },
                { "\x1b[15~",       ITK_F5 },
                { "\x1b[24~",       ITK_F12 },
                { "\x1b[Z",         ITK_SHIFT_TAB },
                { "q",              'q' },
                { "\x14",           0x14 },             /* Ctrl-T */
                { "\x7f",           ITK_BACKSPACE },
            };
            size_t ti;

            for (ti = 0; ti < sizeof(tk) / sizeof(tk[0]); ti++) {
                got = Screen_TermFeedTest((const uint8_t *)tk[ti].seq,
                                          (int)strlen(tk[ti].seq), 0);
                if (got != tk[ti].key ||
                    Screen_TermFeedTest(NULL, 0, 0) != ITK_NONE)
                    t_ok = 0;
            }
            /* bare ESC resolves only via the pump's grace flush */
            if (Screen_TermFeedTest((const uint8_t *)"\x1b", 1, 0)
                    != ITK_NONE)
                t_ok = 0;
            if (Screen_TermFeedTest(NULL, 0, 1) != ITK_ESC)
                t_ok = 0;
            /* sequence split across two polls */
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[1;", 4, 0)
                    != ITK_NONE)
                t_ok = 0;
            if (Screen_TermFeedTest((const uint8_t *)"5D", 2, 0)
                    != ITK_CTRL_LEFT)
                t_ok = 0;
            /* junk: unknown CSI final, unmapped Alt, UTF-8 tail byte */
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[9X", 4, 0)
                    != ITK_NONE ||
                Screen_TermFeedTest((const uint8_t *)"\x1b#", 2, 0)
                    != ITK_NONE ||
                Screen_TermFeedTest((const uint8_t *)"\x90", 1, 0)
                    != ITK_NONE)
                t_ok = 0;
            /* SGR mouse: press -> ITK_MOUSE + state, drag moves with
             * the button held, release drops it; wheel is consumed */
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[<0;10;5M",
                                    10, 0) != ITK_MOUSE)
                t_ok = 0;
            Screen_TermMouseTest(&m);
            if (m.x != 9 || m.y != 4 || m.b != 1 || m.px != 76)
                t_ok = 0;
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[<32;12;6M",
                                    11, 0) != ITK_NONE)
                t_ok = 0;
            Screen_TermMouseTest(&m);
            if (m.x != 11 || m.y != 5 || m.b != 1)
                t_ok = 0;
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[<0;12;6m",
                                    10, 0) != ITK_NONE)
                t_ok = 0;
            Screen_TermMouseTest(&m);
            if (m.b != 0)
                t_ok = 0;
            if (Screen_TermFeedTest((const uint8_t *)"\x1b[<64;1;1M",
                                    10, 0) != ITK_NONE)
                t_ok = 0;
            Screen_TermMouseTest(&m);
            if (m.x != 11 || m.y != 5)          /* wheel didn't move it */
                t_ok = 0;
            fprintf(stderr, "ITED selftest: [%s]\n",
                    t_ok ? "TERM OK" : "TERM FAIL");
        }

        /* S3M export (feature 012): D_SaveS3M layout checks + a round
         * trip through the feature-007 S3M importer. */
        {
            int s3m_ok = 1;
            const char *tmp = "st_s3m.s3m";
            uint8_t hd[0x100];
            FILE *fp;
            uint8_t v_gv, v_is, v_it;
            uint8_t smpbytes[16];
            uint32_t smplen = 0;
            int smpidx = 2;             /* itdemo sample 3 has data */
            int i, ordn = 0, patn = 0;

            do_load_named("testdata/itdemo.it");
            v_gv = Song.Header.GV;
            v_is = Song.Header.IS;
            v_it = Song.Header.IT;
            for (i = 255; i > 0; i--)
                if (Song.Orders[i] != 0xFF) { ordn = i; break; }
            ordn += 2;
            for (i = 0; i < MAX_PATTERNS; i++)
                if (Song.Patterns[i].PackedData &&
                    Song.Patterns[i].DataLength)
                    patn = i + 1;
            {
                const sample_t *s = &Song.Smp[smpidx];
                smplen = s->Length << ((s->Flags & 2) ? 1 : 0);
                memcpy(smpbytes, s->Data, 16);
            }

            commit_current_pattern();
            Save_S3MWarned = 0;
            if (!Save_S3MModule(tmp))
                s3m_ok = 0;

            fp = fopen(tmp, "rb");
            if (!fp || fread(hd, 1, sizeof(hd), fp) != sizeof(hd)) {
                s3m_ok = 0;
            } else {
                if (memcmp(hd + 0x2C, "SCRM", 4) != 0 ||
                    hd[28] != 0x1A || hd[29] != 16)
                    s3m_ok = 0;
                if ((hd[0x20] | (hd[0x21] << 8)) != ordn ||
                    (hd[0x24] | (hd[0x25] << 8)) != patn)
                    s3m_ok = 0;
                if (hd[0x30] != (uint8_t)(v_gv >> 1) ||
                    hd[0x31] != v_is || hd[0x32] != v_it)
                    s3m_ok = 0;
                if (hd[0x40] != 0 ||        /* unmuted ch 1/2 -> L1,R1 */
                    hd[0x41] != 8 ||
                    hd[0x4D] != 0xFF)       /* ch 14 muted in itdemo  */
                    s3m_ok = 0;
                if (hd[0x35] != 252)        /* default pans present   */
                    s3m_ok = 0;
            }
            if (fp)
                fclose(fp);

            /* round trip: the importer must read our file back */
            if (s3m_ok && !do_load_named(tmp))
                s3m_ok = 0;
            if (s3m_ok) {
                const sample_t *s = &Song.Smp[smpidx];
                uint32_t bl = s->Length << ((s->Flags & 2) ? 1 : 0);
                if (Song.Header.IS != v_is || Song.Header.IT != v_it)
                    s3m_ok = 0;
                if (Song.Header.GV != (uint8_t)((v_gv >> 1) << 1))
                    s3m_ok = 0;
                if (bl != smplen || !s->Data ||
                    memcmp(s->Data, smpbytes, 16) != 0)
                    s3m_ok = 0;         /* sign-conversion round trip */
            }
            remove(tmp);
            do_load_named("testdata/itdemo.it");
            fprintf(stderr, "ITED selftest: [%s]\n",
                    s3m_ok ? "S3M OK" : "S3M FAIL");
        }

        /* Polish batch (feature 013, US1): Alt-U pattern update,
         * in-list name editing, hi-ASCII bank, stereo-hook default. */
        {
            int u_ok = 1, i;
            uint16_t keep = CurPattern;
            static instrument_t keepins;
            char keepnm[26];

            /* -- Alt-U: table entry (note 60, sample 3) on instrument
             * 5 remaps a (60,3) cell to (10, ins 5) -- */
            commit_current_pattern();
            load_pattern(199);
            CurRow = CurChan = CurCol = 0;
            memset(&Grid[0], 0, sizeof(editcell_t));
            Grid[0].mask = CM_NOTE | CM_INS;
            Grid[0].note = 61;          /* engine note 60 */
            Grid[0].ins  = 3;
            commit_current_pattern();
            keepins = Song.Ins[4];
            memset(Song.Ins[4].NoteSampleTable, 0, 240);
            Song.Ins[4].NoteSampleTable[10 * 2]     = 60;
            Song.Ins[4].NoteSampleTable[10 * 2 + 1] = 3;
            i = ListSel;
            ListSel = 4;                /* instrument 5 */
            pattern_update_instruments();
            ListSel = i;
            if (Grid[0].note != 11 || Grid[0].ins != 5)
                u_ok = 0;               /* engine 10 + instrument 5 */
            Song.Ins[4] = keepins;
            free(Song.Patterns[199].PackedData);
            Song.Patterns[199].PackedData = NULL;
            Song.Patterns[199].Rows = 0;
            Song.Patterns[199].DataLength = 0;
            load_pattern(keep);

            /* -- F3 name editing: insert x2 + backspace x2 round trip */
            i = ListSel; ListSel = 0;
            memcpy(keepnm, Song.Smp[0].SampleName, 26);
            SamplePos = 25;
            sample_list_lkey(ITK_HOME);
            if (SamplePos != 0)
                u_ok = 0;
            sample_list_lkey('H');
            sample_list_lkey('i');
            if (Song.Smp[0].SampleName[0] != 'H' ||
                Song.Smp[0].SampleName[1] != 'i' ||
                Song.Smp[0].SampleName[2] != keepnm[0] || SamplePos != 2)
                u_ok = 0;
            sample_list_lkey(ITK_BACKSPACE);
            sample_list_lkey(ITK_BACKSPACE);
            if (Song.Smp[0].SampleName[0] != keepnm[0] || SamplePos != 0)
                u_ok = 0;
            memcpy(Song.Smp[0].SampleName, keepnm, 26);
            sample_list_lkey(ITK_END);  /* back to note-play mode */

            /* -- F4 edit mode (Spacebar toggles; Enter leaves) -- */
            memcpy(keepnm, Song.Ins[0].InstrumentName, 26);
            instr_list_lkey(' ');
            if (!InstrumentEdit || InstrumentPos != 0)
                u_ok = 0;
            instr_list_lkey('X');
            if (Song.Ins[0].InstrumentName[0] != 'X' ||
                Song.Ins[0].InstrumentName[1] != keepnm[0] ||
                InstrumentPos != 1)
                u_ok = 0;
            instr_list_lkey(ITK_BACKSPACE);
            if (Song.Ins[0].InstrumentName[0] != keepnm[0])
                u_ok = 0;
            instr_list_lkey(ITK_ENTER);
            if (InstrumentEdit)
                u_ok = 0;
            memcpy(Song.Ins[0].InstrumentName, keepnm, 26);
            ListSel = i;

            /* -- hi-ASCII: bank-B char 176 rasterizes as the ROM
             * glyph after Screen_DefineHiASCII -- */
            {
                uint32_t *px = (uint32_t *)malloc(640u * 400u * 4u);
                if (!px) {
                    u_ok = 0;
                } else {
                    uint32_t bg;
                    Screen_Clear(0x00);
                    Screen_PutChar(1, 0, ' ', 0x08);
                    Screen_PutChar(0, 0, 176, 0x08);  /* fg bit 3 ->
                                                         font bank B */
                    Screen_DefineHiASCII();
                    Screen_Rasterize(px);
                    bg = px[8];         /* first pixel of the blank */
                    for (i = 0; i < 8; i++) {
                        int lit = (IT_FontROM[176][0] >> (7 - i)) & 1;
                        if ((px[i] != bg) != lit)
                            u_ok = 0;
                    }
                    free(px);
                }
                Screen_DefineSmallNumbers();    /* restore bank B */
            }

            /* -- headless stereo default stays silent-left -- */
            if (Load_StereoChoice != NULL)
                u_ok = 0;

            redraw();
            fprintf(stderr, "ITED selftest: [%s]\n",
                    u_ok ? "UPD OK" : "UPD FAIL");
        }

        /* Fourier transform sanity (feature 013): a bin-32 sine
         * saturates its bin and leaves the rest near-silent; silence
         * yields all-zero magnitudes. */
        {
            int f_ok = 1, i;
            static int16_t w[2048];

            for (i = 0; i < 2048; i++)
                w[i] = (int16_t)(8192.0 *
                    sin(2.0 * 3.14159265358979323846 * 32.0 * i
                        / 2048.0));
            fourier_fft(w, FourMag);
            if (FourMag[32] != 255)     /* |X| = 8192*1024/128>>6 = 1024 */
                f_ok = 0;
            if (FourMag[30] > 8 || FourMag[34] > 8 || FourMag[200] > 2)
                f_ok = 0;               /* leakage stays tiny */
            memset(w, 0, sizeof(w));
            fourier_fft(w, FourMag);
            for (i = 0; i < 1024; i++)
                if (FourMag[i])
                    f_ok = 0;
            fprintf(stderr, "ITED selftest: [%s]\n",
                    f_ok ? "FFT OK" : "FFT FAIL");
        }

        /* F11 order list (OrderListKeys) + Music_AssignSampleToInstrument:
         * full-range navigation, digit entry, +/-/Ins/Del/N/End, Alt-R
         * reorder, and the sample->host-instrument transfer. All state is
         * restored afterwards. */
        {
            int o_ok = 1, keepsel = ListSel, keepcur = OrderCursor;
            int c0 = Music_GetNumberOfInstruments();
            uint8_t keepord[256];

            memcpy(keepord, Song.Orders, 256);
            memset(Song.Orders, 0xFF, 256);
            Song.Orders[0] = 0;
            ListSel = 0;
            OrderCursor = 0;

            if (max_order() != 0)
                o_ok = 0;
            order_list_lkey(ITK_DOWN);          /* past the terminator */
            if (ListSel != 1)
                o_ok = 0;
            order_list_lkey('0');               /* digit entry: 005 */
            order_list_lkey('0');
            order_list_lkey('5');
            if (Song.Orders[1] != 5 || ListSel != 2 || OrderCursor != 0)
                o_ok = 0;
            order_list_lkey('9');               /* 900 clamps to 199 */
            if (Song.Orders[2] != 199 || OrderCursor != 1)
                o_ok = 0;
            order_list_lkey('+');               /* +++ then down */
            if (Song.Orders[2] != 0xFE || ListSel != 3 || OrderCursor != 0)
                o_ok = 0;
            order_list_lkey('-');               /* --- then down */
            if (Song.Orders[3] != 0xFF || ListSel != 4)
                o_ok = 0;
            ListSel = 1;                        /* Ins/Del row shifting */
            order_list_lkey(ITK_INS);
            if (Song.Orders[1] != 0xFF || Song.Orders[2] != 5)
                o_ok = 0;
            order_list_lkey(ITK_DEL);
            if (Song.Orders[1] != 5 || Song.Orders[2] != 0xFE)
                o_ok = 0;
            ListSel = 2;                        /* N = previous + 1 */
            order_list_lkey('N');
            if (Song.Orders[2] != 6 || ListSel != 3)
                o_ok = 0;
            order_list_lkey(ITK_END);           /* first --- */
            if (ListSel != 3 || max_order() != 2)
                o_ok = 0;

            /* Alt-R: orders 0,5,6 renumber to 0,1,2 (patterns swapped
             * 1<->5, 2<->6; swapped back below) */
            order_list_lkey(ITK_ALT_A + ('R' - 'A'));
            if (Song.Orders[0] != 0 || Song.Orders[1] != 1 ||
                Song.Orders[2] != 2)
                o_ok = 0;
            {
                int cur = (int)CurPattern;
                ed_lock();
                order_swap_patterns(2, 6, &cur);
                order_swap_patterns(1, 5, &cur);
                ed_unlock();
                load_pattern((uint16_t)cur);
            }

            /* host-instrument transfer: blank same-numbered slot gets
             * the sample name + a full 120-note map */
            {
                int n = Music_AssignSampleToInstrument(97);
                if (n != 98 ||
                    Music_InstrumentIsBlank(&Song.Ins[97]) ||
                    Song.Ins[97].NoteSampleTable[1] != 98 ||
                    Song.Ins[97].NoteSampleTable[2 * 119 + 1] != 98 ||
                    memcmp(Song.Ins[97].InstrumentName,
                           Song.Smp[97].SampleName, 26) != 0)
                    o_ok = 0;
                /* occupied slot: falls through to the first blank one */
                n = Music_AssignSampleToInstrument(97);
                if (n == 0 || n == 98 ||
                    Song.Ins[n - 1].NoteSampleTable[1] != 98)
                    o_ok = 0;
                if (n > 0)
                    Music_InitInstrument(&Song.Ins[n - 1]);
                Music_InitInstrument(&Song.Ins[97]);
            }
            if (Music_GetNumberOfInstruments() != c0)
                o_ok = 0;

            memcpy(Song.Orders, keepord, 256);
            ListSel = keepsel;
            OrderCursor = keepcur;
            redraw();
            fprintf(stderr, "ITED selftest: [%s]\n",
                    o_ok ? "ORD OK" : "ORD FAIL");
        }

        /* Feature 014: the two-layer key event. Note entry must depend
         * on the PHYSICAL position only (IT_I.ASM:1344 compares the
         * scancode), so the same scancode sequence must yield the same
         * notes under a US and a German layout even though the
         * characters those positions produce differ. Driven through
         * Screen_KeyFeedTest, so this runs on any host with no German
         * keyboard attached. */
        {
            int k_ok = 1, i;
            /* the 12 lower-row positions, in the original's order:
             * Z S X D C V G B H N J M (US labels) */
            static const uint8_t rowscan[12] = {
                0x2C,0x1F,0x2D,0x20,0x2E,0x2F,
                0x22,0x30,0x23,0x31,0x24,0x32
            };
            /* what those same positions PRINT on each layout: US has
             * z..m, German swaps y/z (Keyboard/DE.ASM keycodes 21/44) */
            static const char us_ch[12] = {
                'z','s','x','d','c','v','g','b','h','n','j','m'
            };
            static const char de_ch[12] = {
                'y','s','x','d','c','v','g','b','h','n','j','m'
            };
            int us_note[12], de_note[12];

            for (i = 0; i < 12; i++) {
                it_key_t k;
                memset(&k, 0, sizeof(k));
                k.scan = rowscan[i];
                k.flags = ITKF_PRESSED;
                k.ch = (uint16_t)us_ch[i];
                k.code = us_ch[i];
                Screen_KeyFeedTest(&k, 1);
                ed_get_key();
                us_note[i] = key_to_note(&CurKey);

                memset(&k, 0, sizeof(k));
                k.scan = rowscan[i];
                k.flags = ITKF_PRESSED;
                k.ch = (uint16_t)de_ch[i];
                k.code = de_ch[i];
                Screen_KeyFeedTest(&k, 1);
                ed_get_key();
                de_note[i] = key_to_note(&CurKey);
            }
            for (i = 0; i < 12; i++) {
                /* identical notes from identical positions ... */
                if (us_note[i] != de_note[i])
                    k_ok = 0;
                /* ... and a real ascending chromatic run */
                if (us_note[i] != BaseOctave * 12 + i + 1)
                    k_ok = 0;
            }
            /* ... while the characters genuinely differ at the swapped
             * key, which is what makes the parity assertion meaningful */
            if (us_ch[0] == de_ch[0])
                k_ok = 0;

            /* the upper row's root (Q, scancode 10h) is one octave up */
            {
                it_key_t k;
                memset(&k, 0, sizeof(k));
                k.scan = 0x10;
                k.flags = ITKF_PRESSED;
                k.ch = 'q';
                k.code = 'q';
                Screen_KeyFeedTest(&k, 1);
                ed_get_key();
                if (key_to_note(&CurKey) != BaseOctave * 12 + 12 + 1)
                    k_ok = 0;
            }

            /* a position with no note binding stays unbound, and an
             * event carrying a character but no position yields none */
            {
                it_key_t k;
                memset(&k, 0, sizeof(k));
                k.scan = 0x1E;              /* A: not in KeyBoardTable */
                k.flags = ITKF_PRESSED;
                k.ch = 'a';
                k.code = 'a';
                Screen_KeyFeedTest(&k, 1);
                ed_get_key();
                if (key_to_note(&CurKey) != -1)
                    k_ok = 0;

                memset(&k, 0, sizeof(k));
                k.flags = ITKF_PRESSED;     /* scan 0 = position unknown */
                k.ch = 'z';
                k.code = 'z';
                Screen_KeyFeedTest(&k, 1);
                ed_get_key();
                if (key_to_note(&CurKey) != -1)
                    k_ok = 0;
            }

            /* the terminal path: characters only, position inferred
             * from the US reverse map, so 'z' still means C */
            if (Key_ReverseScan('z') != 0x2C) k_ok = 0;
            if (Key_ReverseScan('y') != 0x15) k_ok = 0;
            if (Key_ReverseScan('Q') != 0x10) k_ok = 0;

            /* CP437 conversion for the seven German letters (R7) */
            if (Screen_UnicodeToCP437(0x00E4) != 0x84) k_ok = 0;  /* a" */
            if (Screen_UnicodeToCP437(0x00F6) != 0x94) k_ok = 0;  /* o" */
            if (Screen_UnicodeToCP437(0x00FC) != 0x81) k_ok = 0;  /* u" */
            if (Screen_UnicodeToCP437(0x00DF) != 0xE1) k_ok = 0;  /* ss */
            if (Screen_UnicodeToCP437(0x00C4) != 0x8E) k_ok = 0;
            if (Screen_UnicodeToCP437(0x00D6) != 0x99) k_ok = 0;
            if (Screen_UnicodeToCP437(0x00DC) != 0x9A) k_ok = 0;
            if (Screen_UnicodeToCP437('A')    != 'A')  k_ok = 0;
            if (Screen_UnicodeToCP437(0x0142) != 0)    k_ok = 0;  /* l/ */

            /* National characters round-trip through a save/reload
             * (FR-015). The seven German letters as CP437 bytes:
             * a" o" u" ss A" O" U" */
            {
                static const unsigned char umlauts[7] = {
                    0x84, 0x94, 0x81, 0xE1, 0x8E, 0x99, 0x9A
                };
                const char *tmp = "st_kbd.it";
                char keep[26];
                int j;

                memcpy(keep, Song.Smp[0].SampleName, 26);
                memset(Song.Smp[0].SampleName, 0, 26);
                for (j = 0; j < 7; j++)
                    Song.Smp[0].SampleName[j] = (char)umlauts[j];

                commit_current_pattern();
                if (!Save_ITModule(tmp))
                    k_ok = 0;
                else if (!do_load_named(tmp))
                    k_ok = 0;
                else if (memcmp(Song.Smp[0].SampleName, umlauts, 7) != 0)
                    k_ok = 0;               /* bytes must survive intact */
                remove(tmp);
                memcpy(Song.Smp[0].SampleName, keep, 26);
            }

            /* The two layers must stay independent: an Alt shortcut is
             * carried by `code` and must survive whatever position it
             * arrived from (research R5 -- Alt follows the keycap, so
             * the German Z key at scancode 15h still means Alt-Z). */
            {
                it_key_t k;
                memset(&k, 0, sizeof(k));
                k.scan = 0x15;              /* German Z position */
                k.flags = ITKF_PRESSED | ITKF_LALT;
                k.code = ITK_ALT_A + 25;    /* Alt-Z */
                Screen_KeyFeedTest(&k, 1);
                if (ed_get_key() != ITK_ALT_A + 25)
                    k_ok = 0;
                if (CurKey.scan != 0x15)    /* position still reported */
                    k_ok = 0;
            }

            /* A malformed layout override must be refused and must
             * leave the host layout in place, never fail startup
             * (FR-010). */
            {
                const char *tmp = "st_kbd.cfg";
                FILE *fp = fopen(tmp, "wb");
                if (fp) {
                    /* FileLength claims far more than the file holds */
                    static const unsigned char bad[6] =
                        { 0xFF, 0x7F, 0x15, 0x00, 0x7A, 0x00 };
                    fwrite(bad, 1, sizeof(bad), fp);
                    fclose(fp);
                    if (Key_LoadLayout(tmp) == NULL)
                        k_ok = 0;           /* must have been rejected */
                    if (Key_LayoutName()[0])
                        k_ok = 0;           /* must not be active */
                    remove(tmp);
                }
                /* a missing file is a reported failure, not a crash */
                if (Key_LoadLayout("st_kbd_absent.cfg") == NULL)
                    k_ok = 0;
                /* clearing returns to the host layout */
                if (Key_LoadLayout(NULL) != NULL)
                    k_ok = 0;
                if (Key_LayoutName()[0])
                    k_ok = 0;
            }

            memset(&CurKey, 0, sizeof(CurKey));
            fprintf(stderr, "ITED selftest: [%s]\n",
                    k_ok ? "KBD OK" : "KBD FAIL");
        }

        /* F4 instrument slot ops, driven through the real key path
         * (handle_global -> widgets_key -> handle_instrument_altkey):
         * Alt-D deletes the instrument AND its samples (I_DeleteInstrument),
         * Alt-W wipes to the template keeping samples (I_InstrumentClear),
         * Alt-Ins / Alt-Del shift slots (I_Insert/RemoveInstrument).
         * Uses slots 90..92 / sample 96, which itdemo leaves empty. */
        {
            int i_ok = 1, i, lw = -1;
            int keep_scr = Screen, keep_sel = ListSel;
            uint8_t keep_tab = InsTab;
            it_key_t yes;

            for (i = 89; i <= 92; i++)
                Music_InitInstrument(&Song.Ins[i]);
            /* instrument 90 plays sample 96 on every note */
            for (i = 0; i < 120; i++)
                Song.Ins[89].NoteSampleTable[i * 2 + 1] = 96;
            memcpy(Song.Ins[90].InstrumentName, "KEEP", 5);
            memset(&Song.Smp[95], 0, sizeof(sample_t));
            Song.Smp[95].Data = calloc(64, 1);
            Song.Smp[95].Length = 64;
            Song.Smp[95].Flags = 1;
            memcpy(Song.Smp[95].SampleName, "doomed", 7);

            Screen = SCR_INSTRUMENTS;
            InsTab = 0;
            ListSel = 89;
            redraw();
            for (i = 0; i < NW; i++)
                if (W[i].type == WT_LIST && W[i].lkey == instr_list_lkey)
                    lw = i;
            if (lw < 0)
                i_ok = 0;
            else
                FocusIdx[SCR_INSTRUMENTS] = lw;

            /* Alt-D, confirmed: instrument back to template, sample gone */
            memset(&yes, 0, sizeof(yes));
            yes.flags = ITKF_PRESSED; yes.ch = 'y'; yes.code = 'y';
            Screen_KeyFeedTest(&yes, 1);
            handle_global(ITK_ALT_A + ('D' - 'A'));
            if (!Music_InstrumentIsBlank(&Song.Ins[89])) i_ok = 0;
            if (Song.Smp[95].Data || Song.Smp[95].Flags ||
                Song.Smp[95].SampleName[0])
                i_ok = 0;
            if (memcmp(Song.Ins[90].InstrumentName, "KEEP", 5)) i_ok = 0;

            /* Alt-W: template again, but the sample it maps survives */
            memcpy(Song.Ins[89].InstrumentName, "W", 2);
            Song.Ins[89].NoteSampleTable[1] = 1;
            {
                void *d0 = Song.Smp[0].Data;
                handle_global(ITK_ALT_A + ('W' - 'A'));
                if (!Music_InstrumentIsBlank(&Song.Ins[89])) i_ok = 0;
                if (Song.Smp[0].Data != d0) i_ok = 0;
            }

            /* Alt-Ins: 90 becomes a fresh template, old 90 moves to 91 */
            memcpy(Song.Ins[89].InstrumentName, "A", 2);
            handle_global(ITK_ALT_INS);
            if (!Music_InstrumentIsBlank(&Song.Ins[89])) i_ok = 0;
            if (memcmp(Song.Ins[90].InstrumentName, "A", 2)) i_ok = 0;
            if (memcmp(Song.Ins[91].InstrumentName, "KEEP", 5)) i_ok = 0;

            /* Alt-Del: back where we were; 99 is reset to the template */
            handle_global(ITK_ALT_DEL);
            if (memcmp(Song.Ins[89].InstrumentName, "A", 2)) i_ok = 0;
            if (memcmp(Song.Ins[90].InstrumentName, "KEEP", 5)) i_ok = 0;
            if (!Music_InstrumentIsBlank(&Song.Ins[98])) i_ok = 0;

            for (i = 89; i <= 92; i++)
                Music_InitInstrument(&Song.Ins[i]);

            /* F3 Alt-Ins with template-stamped empty slots (fix from PR #3
             * by esaruoho): the last slot is blank, so the insert must run
             * and move slot 91 down to 92 */
            {
                uint16_t keepflags = Song.Header.Flags;
                sample_t keep91 = Song.Smp[90], keep92 = Song.Smp[91];
                Song.Header.Flags &= (uint16_t)~ITF_INSTRUMENTS;
                Music_InitSample(&Song.Smp[90]);
                memcpy(Song.Smp[90].SampleName, "shift me", 9);
                ListSel = 90;
                smp_op_insert_slot();
                if (memcmp(Song.Smp[91].SampleName, "shift me", 9) ||
                    !Music_SampleIsBlank(&Song.Smp[90]))
                    i_ok = 0;
                smp_op_remove_slot();           /* and back */
                if (memcmp(Song.Smp[90].SampleName, "shift me", 9))
                    i_ok = 0;
                Song.Smp[90] = keep91;
                Song.Smp[91] = keep92;
                Song.Header.Flags = keepflags;
            }

            /* issue #9: typing digits on a thumbbar opens "Enter Value"
             * (F_PostThumbBar30). F12 Initial Tempo, "8" "0" Enter -> 80;
             * out of range (999 > 255) is rejected, not clamped; Esc
             * cancels. */
            {
                uint8_t keep_it = Song.Header.IT;
                int keep_scr2 = Screen, wi = -1, k;
                it_key_t fk;
                Screen = SCR_VARS;
                redraw();
                for (k = 0; k < NW; k++)
                    if (W[k].type == WT_THUMB &&
                        W[k].v8 == (uint8_t *)&Song.Header.IT)
                        wi = k;
                if (wi < 0) {
                    i_ok = 0;
                } else {
                    static const int seq1[] = { '0', ITK_ENTER };
                    static const int seq2[] = { '9', '9', ITK_ENTER };
                    static const int seq3[] = { ITK_ESC };
                    FocusIdx[SCR_VARS] = wi;
                    Song.Header.IT = 112;
                    for (k = 0; k < 2; k++) {
                        memset(&fk, 0, sizeof(fk));
                        fk.flags = ITKF_PRESSED; fk.code = seq1[k];
                        fk.ch = (uint16_t)(seq1[k] < 256 ? seq1[k] : 0);
                        Screen_KeyFeedTest(&fk, 1);
                    }
                    handle_global('8');
                    if (Song.Header.IT != 80) i_ok = 0;
                    for (k = 0; k < 3; k++) {
                        memset(&fk, 0, sizeof(fk));
                        fk.flags = ITKF_PRESSED; fk.code = seq2[k];
                        fk.ch = (uint16_t)(seq2[k] < 256 ? seq2[k] : 0);
                        Screen_KeyFeedTest(&fk, 1);
                    }
                    handle_global('9');
                    if (Song.Header.IT != 80) i_ok = 0;
                    memset(&fk, 0, sizeof(fk));
                    fk.flags = ITKF_PRESSED; fk.code = seq3[0];
                    Screen_KeyFeedTest(&fk, 1);
                    handle_global('5');
                    if (Song.Header.IT != 80) i_ok = 0;
                    /* issue #7: Shift-arrows step 4, Ctrl-arrows 2, clamped */
                    handle_global(ITK_SHIFT_RIGHT);
                    if (Song.Header.IT != 84) i_ok = 0;
                    handle_global(ITK_CTRL_LEFT);
                    if (Song.Header.IT != 82) i_ok = 0;
                    Song.Header.IT = 253;
                    handle_global(ITK_SHIFT_RIGHT);
                    if (Song.Header.IT != 255) i_ok = 0;
                }
                /* issue #19: F12 cursor-down walks the left column from
                 * Song Name to Save all Preferences, and Amiga reaches
                 * the Module path */
                {
                    static const int chain[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8,
                                                 10, 12, 14, 15, 16, 17 };
                    FocusIdx[SCR_VARS] = 0;
                    for (k = 1; k < (int)(sizeof(chain) / sizeof(chain[0]));
                         k++) {
                        handle_global(ITK_DOWN);
                        redraw();
                        if (FocusIdx[SCR_VARS] != chain[k]) i_ok = 0;
                    }
                    FocusIdx[SCR_VARS] = 13;            /* Amiga */
                    handle_global(ITK_DOWN);
                    if (FocusIdx[SCR_VARS] != 14) i_ok = 0;
                }
                Song.Header.IT = keep_it;
                Screen = keep_scr2;
            }

            /* issue #4: F2 in the pattern editor opens Pattern Editor
             * Options. Right on Base octave, then Number of rows via
             * "Enter Value" (9 6 Enter), Esc closes and resizes the
             * current pattern; Link/Split set CommandToValue. */
            {
                static const int seq1[] = {
                    ITK_RIGHT, ITK_DOWN, ITK_DOWN, ITK_DOWN, ITK_DOWN,
                    '9', '6', ITK_ENTER, ITK_ESC };
                static const int seq2[] = {
                    ITK_DOWN, ITK_DOWN, ITK_DOWN, ITK_DOWN,
                    '6', '4', ITK_ENTER, ITK_ESC };
                static const int seq3[] = {
                    ITK_DOWN, ITK_DOWN, ITK_DOWN, ITK_DOWN, ITK_DOWN,
                    ITK_RIGHT, ITK_ENTER, ITK_ESC };
                int keep_oct = BaseOctave, keep_ctv = CommandToValue;
                int keep_scr3 = Screen, k;
                it_key_t fk;
#define PE4_FEED(arr) for (k = 0; k < (int)(sizeof(arr)/sizeof(arr[0])); k++) { \
    memset(&fk, 0, sizeof(fk)); fk.flags = ITKF_PRESSED; fk.code = arr[k]; \
    fk.ch = (uint16_t)(arr[k] < 256 ? arr[k] : 0); Screen_KeyFeedTest(&fk, 1); }
                Screen = SCR_PATTERN;
                PE4_FEED(seq1);
                handle_global(ITK_F2);
                if (BaseOctave != keep_oct + 1 || CurRows != 96) i_ok = 0;
                PE4_FEED(seq2);
                handle_global(ITK_F2);
                if (CurRows != 64) i_ok = 0;
                CommandToValue = 1;
                PE4_FEED(seq3);
                handle_global(ITK_F2);
                if (CommandToValue != 0) i_ok = 0;
#undef PE4_FEED
                BaseOctave = keep_oct;
                CommandToValue = (uint8_t)keep_ctv;
                Screen = keep_scr3;
            }

            /* issue #5: Quit asks "Exit Impulse Tracker?" first; Esc
             * (Cancel) keeps running, Enter (default OK) quits */
            {
                it_key_t fk;
                memset(&fk, 0, sizeof(fk));
                fk.flags = ITKF_PRESSED; fk.code = ITK_ESC;
                Screen_KeyFeedTest(&fk, 1);
                act_file_quit();
                if (!Running) i_ok = 0;
                fk.code = ITK_ENTER;
                Screen_KeyFeedTest(&fk, 1);
                act_file_quit();
                if (Running) i_ok = 0;
                Running = 1;                /* carry on with the selftest */
            }

            /* F5 title is the original's DisplayHeader, "Info Page (F5)"
             * (IT_OBJ1.ASM 6580; reported in PR #3 by esaruoho) */
            {
                static const char title[] = "Info Page (F5)";
                int x, j, found = 0;
                Screen = SCR_INFO;
                redraw();
                for (x = 0; x + (int)sizeof(title) - 1 <= SCREEN_W && !found;
                     x++) {
                    for (j = 0; title[j]; j++)
                        if (Screen_GetCell(x + j, 11).ch != (uint8_t)title[j])
                            break;
                    if (!title[j])
                        found = 1;
                }
                if (!found)
                    i_ok = 0;
            }
            Screen = keep_scr;
            ListSel = keep_sel;
            InsTab = keep_tab;
            memset(&CurKey, 0, sizeof(CurKey));
            fprintf(stderr, "ITED selftest: [%s]\n",
                    i_ok ? "INS OK" : "INS FAIL");
        }

        /* Feature 015: the Load Sample screen. Records first
         * (D_LoadSampleFiles + D_GetSampleInfo + D_SlowSampleSort) over
         * testdata/ls_fixture, generated by tools/gen_import_tests.py. */
        {
            static slibent_t le[620];
            int l_ok = 1, n, i;
            static const char *const want[] = {
                "\\", "..", "ACOUSTIC", "BASS", "SONG.S3M",
                "FIXTURE.ITS", "TEST8.WAV", "README.TXT"
            };

            n = RIS_ListDirectory("testdata/ls_fixture", le, 620);
            if (n != 8)
                l_ok = 0;
            for (i = 0; i < 8 && i < n; i++)
                if (strncmp(le[i].hdr.DOSFileName, want[i], 12))
                    l_ok = 0;
            if (n == 8) {
                /* directories: dotted DirectoryMsg, type 1, priority 0 */
                if (le[2].Format != 1 || le[2].SortPri != 0 ||
                    (uint8_t)le[2].hdr.SampleName[0] != 154 ||
                    memcmp(le[2].hdr.SampleName + 8, "Directory", 9))
                    l_ok = 0;
                /* module: dotted LibraryMsg, type 20h (S3M), priority 1 */
                if (le[4].Format != 0x20 || le[4].SortPri != 1 ||
                    memcmp(le[4].hdr.SampleName + 9, "Library", 7))
                    l_ok = 0;
                /* the ITS: its own header, as the generator wrote it */
                if (le[5].Format != 2 || le[5].SortPri != 2 ||
                    le[5].hdr.C5Speed != 11025 || le[5].hdr.Length != 1000 ||
                    le[5].hdr.LoopBeg != 100 || le[5].hdr.LoopEnd != 900 ||
                    !(le[5].hdr.Flags & 0x10) || le[5].hdr.GvL != 48 ||
                    le[5].hdr.Vol != 40 || le[5].hdr.ViS != 3 ||
                    le[5].hdr.ViD != 5 || le[5].hdr.ViR != 7)
                    l_ok = 0;
                /* 8-bit WAV at 22050 Hz */
                if (le[6].Format != 5 || le[6].hdr.C5Speed != 22050)
                    l_ok = 0;
                /* unrecognised files are still listed, as unknown */
                if (le[7].Format != 4 || le[7].SortPri != 3)
                    l_ok = 0;
                /* ... and load as raw 8-bit unsigned data (D_GetSampleInfo2,
                 * issue #17): whole file, C-5 8363, first byte ^ 80h */
                {
                    sample_t raw;
                    FILE *rf = fopen(le[7].SrcFile, "rb");
                    int b0 = rf ? fgetc(rf) : -1;
                    if (rf) fclose(rf);
                    memset(&raw, 0, sizeof(raw));
                    if (!(le[7].hdr.Flags & 1) || le[7].hdr.C5Speed != 8363 ||
                        le[7].hdr.Length != le[7].FileSize ||
                        le[7].hdr.Cvt != 0 || b0 < 0 ||
                        !RIS_LoadSample(&le[7], &raw) || !raw.Data ||
                        ((uint8_t *)raw.Data)[0] != (uint8_t)(b0 ^ 0x80))
                        l_ok = 0;
                    free(raw.Data);
                }
                /* file dates are real (year >= 2020 packs to >= 40<<9) */
                if (le[6].Date < (40u << 9))
                    l_ok = 0;
            }
            if (strcmp(RIS_FormatName(0x28), "Fast Tracker 2 Module"))
                l_ok = 0;                   /* the original's table quirk */

            /* the screen itself (US1): drawn from the object list */
            {
                char keepdir[256];
                screen_cell_t c;

                memcpy(keepdir, DirSample, sizeof(keepdir));
                LsView = 0;
                LsFocus = 15;
                ls_set_dir("testdata/ls_fixture");
                ls_draw();
                c = Screen_GetCell(2, 13);          /* row number 001 */
                if (c.ch != '0' || c.attr != 0x20) l_ok = 0;
                c = Screen_GetCell(4, 13);
                if (c.ch != '1') l_ok = 0;
                c = Screen_GetCell(31, 20);         /* divider A8h attr 2 */
                if (c.ch != 0xA8 || c.attr != 0x02) l_ok = 0;
                c = Screen_GetCell(6, 15);          /* dotted Directory row */
                if (c.ch != 154 || c.attr != 0x05) l_ok = 0;
                c = Screen_GetCell(14, 15);
                if (c.ch != 'D') l_ok = 0;
                c = Screen_GetCell(32, 15);         /* ACOUSTIC, col 32 */
                if (c.ch != 'A') l_ok = 0;
                c = Screen_GetCell(32, 20);         /* README.TXT, unknown */
                if (c.ch != 'R' || c.attr != 0x02) l_ok = 0;
                c = Screen_GetCell(6, 13);          /* cursor bar 30h */
                if (c.attr != 0x30) l_ok = 0;
                c = Screen_GetCell(31, 13);         /* divider cell 32h */
                if (c.attr != 0x32) l_ok = 0;
                c = Screen_GetCell(46, 13);         /* drive box */
                if (c.ch != 'D' || c.attr != 0x05) l_ok = 0;
                c = Screen_GetCell(55, 21);         /* Quality label row */
                if (c.ch != ' ' && c.ch != 'Q') l_ok = 0;
                c = Screen_GetCell(56, 21);
                if (c.ch != 'Q') l_ok = 0;
                /* Enter on a directory row re-lists inside it */
                LsCur = 2;                          /* ACOUSTIC */
                ls_enter_dir(&LsEnt[LsCur]);
                {
                    int found = 0;
                    for (i = 0; i < LsN; i++)
                        if (!strncmp(LsEnt[i].hdr.DOSFileName,
                                     "PIANO.WAV", 12))
                            found = 1;
                    if (!found || LsCur != 0) l_ok = 0;
                }
                /* US2: preview == what F3 holds after the load */
                {
                    uint16_t keepflags = Song.Header.Flags;
                    uint16_t keepnum = Song.Header.SmpNum;
                    int keepsel = ListSel, j, itsrow = -1, wavrow = -1;

                    ls_set_dir("testdata/ls_fixture");
                    for (j = 0; j < LsN; j++) {
                        if (!strncmp(LsEnt[j].hdr.DOSFileName,
                                     "FIXTURE.ITS", 12)) itsrow = j;
                        if (!strncmp(LsEnt[j].hdr.DOSFileName,
                                     "TEST8.WAV", 12)) wavrow = j;
                    }
                    if (itsrow < 0 || wavrow < 0) l_ok = 0;
                    Song.Header.Flags &= (uint16_t)~ITF_INSTRUMENTS;
                    ListSel = 97;               /* scratch slot 98 */
                    for (j = 0; j < 2 && itsrow >= 0 && wavrow >= 0; j++) {
                        const slibent_t *pe = &LsEnt[j ? wavrow : itsrow];
                        sample_t *d = &Song.Smp[97];
                        if (!lib_load_sample_entry(pe)) { l_ok = 0; break; }
                        if (d->C5Speed != pe->hdr.C5Speed ||
                            d->Length != pe->hdr.Length ||
                            d->LoopBeg != pe->hdr.LoopBeg ||
                            d->LoopEnd != pe->hdr.LoopEnd ||
                            ((d->Flags ^ pe->hdr.Flags) & 0x33) ||
                            d->GvL != pe->hdr.GvL || d->Vol != pe->hdr.Vol ||
                            d->ViS != pe->hdr.ViS || d->ViD != pe->hdr.ViD)
                            l_ok = 0;
                    }
                    smp_free_data(&Song.Smp[97]);
                    Music_InitSample(&Song.Smp[97]);
                    Song.Header.SmpNum = keepnum;
                    Song.Header.Flags = keepflags;
                    ListSel = keepsel;

                    /* file info: 9-digit size, month name, 12h time */
                    if (itsrow >= 0) {
                        char row[27];
                        int x;
                        LsCur = itsrow;
                        ls_draw();
                        for (x = 0; x < 9; x++)
                            row[x] = (char)Screen_GetCell(53 + x, 45).ch;
                        row[9] = 0;
                        if (strcmp(row, "000001080")) l_ok = 0;
                        for (x = 0; x < 26; x++) {  /* NUL cell = space */
                            uint8_t ch = Screen_GetCell(53 + x, 46).ch;
                            row[x] = ch ? (char)ch : ' ';
                        }
                        row[26] = 0;
                        if (!strstr(row, ", 20")) l_ok = 0;
                        c = Screen_GetCell(64, 21);     /* "8 Bit" */
                        if (c.ch != '8' || c.attr != 0x02) l_ok = 0;
                        c = Screen_GetCell(67, 15);     /* "On Forwards" */
                        if (c.ch != 'F') l_ok = 0;
                        /* audition -> waveform glyph block appears */
                        ed_sync_key('q');           /* note = scancode */
                        lib_preview_key(&LsEnt[itsrow], itsrow, 'q');
                        ls_draw();
                        c = Screen_GetCell(47, 25);
                        if (c.ch != 1 || c.attr != 0x0D) l_ok = 0;
                        stop_song();
                        lib_release_check();
                    }

                    /* a module lists its samples behind an exit row */
                    for (j = 0; j < LsN; j++)
                        if (LsEnt[j].Format == 0x20) break;
                    if (j < LsN) {
                        ls_enter_module(&LsEnt[j]);
                        if (!LsInModule || LsN < 2 || LsEnt[0].Format != 1 ||
                            LsEnt[0].hdr.DOSFileName[0] != '.' ||
                            LsEnt[1].Format != 3)
                            l_ok = 0;
                        ls_enter_dir(&LsEnt[0]);    /* back out */
                        if (LsInModule || LsN != 8) l_ok = 0;
                    } else {
                        l_ok = 0;
                    }
                }
                /* US3: edits + CheckSampleModified + keep-format saves,
                 * in a scratch copy of the fixture */
                {
                    static const char *const cp[2] = { "FIXTURE.ITS",
                                                       "TEST8.WAV" };
                    const char *wd = "st_ls_work";
                    int j, row;
                    it_key_t fk;
#ifdef _WIN32
                    _mkdir(wd);
#else
                    mkdir(wd, 0777);
#endif
                    for (j = 0; j < 2; j++) {       /* copy the two files */
                        char a[300], b[300];
                        FILE *fi, *fo;
                        int ch;
                        snprintf(a, sizeof(a), "testdata/ls_fixture/%s", cp[j]);
                        snprintf(b, sizeof(b), "%s/%s", wd, cp[j]);
                        fi = fopen(a, "rb");
                        fo = fopen(b, "wb");
                        if (fi && fo)
                            while ((ch = fgetc(fi)) != EOF) fputc(ch, fo);
                        if (fi) fclose(fi);
                        if (fo) fclose(fo);
                    }
#define LS_ROW(nm) do { row = -1; for (j = 0; j < LsN; j++) \
    if (!strncmp(LsEnt[j].hdr.DOSFileName, nm, 12)) row = j; } while (0)
#define LS_FEED(kc) do { memset(&fk, 0, sizeof(fk)); \
    fk.flags = ITKF_PRESSED; fk.code = (kc); \
    fk.ch = (uint16_t)((kc) < 256 ? (kc) : 0); \
    Screen_KeyFeedTest(&fk, 1); } while (0)

                    ls_set_dir(wd);
                    /* 1: ITS edit -> "Save sample?" (Enter = OK) -> the
                     * file itself carries the edit */
                    LS_ROW("FIXTURE.ITS");
                    if (row < 0) l_ok = 0;
                    else {
                        LsCur = row; ls_snapshot();
                        LsEnt[row].hdr.C5Speed = 22050;
                        LsEnt[row].hdr.LoopBeg = 200;
                        if (!ls_modified()) l_ok = 0;
                        LS_FEED(ITK_ENTER);
                        if (!ls_check_modified()) l_ok = 0;
                        LS_ROW("FIXTURE.ITS");
                        if (row < 0 || LsEnt[row].Format != 2 ||
                            LsEnt[row].hdr.C5Speed != 22050 ||
                            LsEnt[row].hdr.LoopBeg != 200)
                            l_ok = 0;
                    }
                    /* 2: a loop toggle alone is not a modification
                     * (byte 12h is skipped, authentic) */
                    LS_ROW("FIXTURE.ITS");
                    if (row >= 0) {
                        LsCur = row; LsCheckIdx = -1; ls_snapshot();
                        LsEnt[row].hdr.Flags ^= 0x10;
                        if (ls_modified()) l_ok = 0;
                        LsEnt[row].hdr.Flags ^= 0x10;
                    }
                    /* 3: "No" then "Discard changes?" OK -> restored */
                    if (row >= 0) {
                        LsEnt[row].hdr.Vol = 5;
                        LS_FEED('n'); LS_FEED(ITK_ENTER);
                        if (!ls_check_modified() || LsEnt[row].hdr.Vol != 40)
                            l_ok = 0;
                    }
                    /* 4: "No" twice -> the move is refused, edit kept */
                    if (row >= 0) {
                        LsEnt[row].hdr.Vol = 5;
                        LS_FEED('n'); LS_FEED('n');
                        if (ls_check_modified() || LsEnt[row].hdr.Vol != 5)
                            l_ok = 0;
                        memcpy(&LsEnt[row].hdr, &LsCheckHdr, 0x50);
                    }
                    /* 5: WAV speed-only edit stays a WAV, rewritten */
                    LS_ROW("TEST8.WAV");
                    if (row < 0) l_ok = 0;
                    else {
                        LsCur = row; LsCheckIdx = -1; ls_snapshot();
                        LsEnt[row].hdr.C5Speed = 11025;
                        LS_FEED(ITK_ENTER);
                        ls_check_modified();
                        LS_ROW("TEST8.WAV");
                        if (row < 0 || LsEnt[row].Format != 5 ||
                            LsEnt[row].hdr.C5Speed != 11025)
                            l_ok = 0;
                    }
                    /* 6: a WAV edit WAV can't hold -> TEST8.ITS beside
                     * the untouched WAV (the keep-format deviation) */
                    if (row >= 0) {
                        LsCur = row; LsCheckIdx = -1; ls_snapshot();
                        LsEnt[row].hdr.Vol = 17;
                        LS_FEED(ITK_ENTER);
                        ls_check_modified();
                        LS_ROW("TEST8.WAV");
                        if (row < 0 || LsEnt[row].Format != 5 ||
                            LsEnt[row].hdr.Vol == 17) l_ok = 0;
                        LS_ROW("TEST8.ITS");
                        if (row < 0 || LsEnt[row].Format != 2 ||
                            LsEnt[row].hdr.Vol != 17) l_ok = 0;
                    }
                    /* 7: the loop clamp caps at Length */
                    {
                        sample_t t;
                        memset(&t, 0, sizeof(t));
                        t.Length = 100; t.LoopBeg = 150; t.LoopEnd = 300;
                        t.Flags = 0x10;
                        ls_check_loops(&t, 0);
                        if (t.LoopBeg != 100 || t.LoopEnd != 100 ||
                            (t.Flags & 0x10)) l_ok = 0;
                    }
#undef LS_ROW
#undef LS_FEED
                    for (j = 0; j < LsN; j++)
                        if (LsEnt[j].Format > 1) remove(LsEnt[j].SrcFile);
#ifdef _WIN32
                    _rmdir(wd);
#else
                    rmdir(wd);
#endif
                }
                memcpy(DirSample, keepdir, sizeof(keepdir));
            }
            fprintf(stderr, "ITED selftest: [%s]\n",
                    l_ok ? "LSS OK" : "LSS FAIL");
        }

        /* ---- feature 016: system file dialogs, driven through the
         * ITED_DIALOG_FAKE hook (no UI; any backend) ---- */
        {
            int d_ok = 1, i, n, dfail_line = 0;
#define DFAIL() do { if (d_ok) dfail_line = __LINE__; d_ok = 0; } while (0)
            char home[IT_DLG_PATH_MAX], cwd[IT_DLG_PATH_MAX];
            char keepdir[sizeof(DirSample)];
            int keepscr = Screen, keepsel = ListSel;
            const uint8_t *const *hl;

            if (!getcwd(home, sizeof(home)))
                home[0] = 0;
            memcpy(keepdir, DirSample, sizeof(keepdir));

            /* reference song */
            if (!do_load_named("testdata/itdemo.it"))
                DFAIL();

            /* cancel / unavailable / overlong: nothing changes */
            dlg_fake("!cancel");
            FileNameDisp[0] = 0;
            handle_global(ITK_CTRL_SHIFT_F9);
            if (FileNameDisp[0] || !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, home))
                DFAIL();
            dlg_fake("!unavailable");
            StatusMsg[0] = 0;
            handle_global(ITK_CTRL_SHIFT_F9);
            if (!strstr(StatusMsg, "No file dialog available") || FileNameDisp[0])
                DFAIL();
            {
                static char longp[1100];
                memset(longp, 'a', sizeof(longp) - 1);
                longp[sizeof(longp) - 1] = 0;
                dlg_fake(longp);
                StatusMsg[0] = 0;
                handle_global(ITK_CTRL_SHIFT_F9);
                if (!strstr(StatusMsg, "too long") || FileNameDisp[0])
                    DFAIL();
            }

            /* US1: open = the F9 load, and the folder becomes current */
            {
                char name0[27];
                uint16_t ord0 = Song.Header.OrdNum, pat0 = Song.Header.PatNum;
                memcpy(name0, Song.Header.SongName, sizeof(name0));
                act_file_new();
                dlg_fake("testdata/itdemo.it");
                handle_global(ITK_CTRL_SHIFT_F9);
                if (memcmp(name0, Song.Header.SongName, sizeof(name0)) ||
                    Song.Header.OrdNum != ord0 || Song.Header.PatNum != pat0 ||
                    strcmp(FileNameDisp, "ITDEMO.IT") ||
                    strcmp(FileSaveName, "itdemo.it"))
                    DFAIL();
                if (!getcwd(cwd, sizeof(cwd)) ||
                    strcmp(path_base(cwd), "testdata"))
                    DFAIL();
                cd_back(home);
            }
            /* Shift-F9 is still the message editor */
            handle_global(ITK_SHIFT_F9);
            if (Screen != SCR_MESSAGE)
                DFAIL();
            Screen = keepscr;

            /* US2: Save As = the F10 writer, byte for byte; the format
             * follows the extension; SaveFormat is left as it was */
            {
                const char *wd = "st_dlg_work";
                static const char *const files[4] = {
                    "dlg_a.it", "dlg_b.IT", "dlg_c.s3m", "dlg_d.S3M" };
                int keepfmt = SaveFormat, pass, j;
#ifdef _WIN32
                _mkdir(wd);
#else
                mkdir(wd, 0777);
#endif
                SaveNoKeyWait = 1;              /* S3M warnings: no wait */
                for (j = 0; j < 4; j++) {       /* fresh: no overwrite prompt */
                    char p[300];
                    snprintf(p, sizeof(p), "%s/%s", wd, files[j]);
                    remove(p);
                }
                for (pass = 0; pass < 2; pass++) {   /* IT, then S3M */
                    int same = 0, tries;
                    for (tries = 0; tries < 2 && !same; tries++) {
                        /* a second boundary between the two writes
                         * changes the stored edit time: one retry, on
                         * fresh files (no overwrite prompt) */
                        char p1[300], p2[300];
                        snprintf(p1, sizeof(p1), "%s/%s", wd, files[pass * 2]);
                        snprintf(p2, sizeof(p2), "%s/%s", wd, files[pass * 2 + 1]);
                        remove(p1);
                        remove(p2);
                        dlg_fake(pass ? "st_dlg_work/dlg_c.s3m"
                                      : "st_dlg_work/dlg_a");
                        handle_global(ITK_CTRL_SHIFT_F10);
                        SaveFormat = pass;
                        save_module_dispatch(files[pass * 2 + 1]);
                        SaveFormat = keepfmt;
                        same = files_equal(files[pass * 2],
                                           files[pass * 2 + 1]);
                        cd_back(home);
                    }
                    if (!same)
                        DFAIL();
                }
                SaveNoKeyWait = 0;
                if (SaveFormat != keepfmt)
                    DFAIL();
                dlg_fake("!cancel");                /* writes nothing */
                handle_global(ITK_CTRL_SHIFT_F10);
                for (j = 0; j < 4; j++) {
                    char p[300];
                    snprintf(p, sizeof(p), "%s/%s", wd, files[j]);
                    remove(p);
                }
#ifdef _WIN32
                _rmdir(wd);
#else
                rmdir(wd);
#endif
                if (!do_load_named("testdata/itdemo.it"))  /* names reset */
                    DFAIL();
            }

            /* US3: Ctrl-O on F3 / F4 = the library requester's load */
            {
                uint16_t keepnum = Song.Header.SmpNum, keepins = Song.Header.InsNum;
                uint16_t keepflags = Song.Header.Flags;
                slibent_t ref[4];
                sample_t t;
                uint8_t had[99];
                instrument_t keepi = Song.Ins[98];

                Song.Header.Flags &= (uint16_t)~ITF_INSTRUMENTS;    /* no
                                                   host-instrument prompt */
                Screen = SCR_SAMPLES;
                ListSel = 97;
                dlg_fake("testdata/lib_test8.wav");
                handle_global(0x0F);
                memset(&t, 0, sizeof(t));
                if (RIS_ScanModule("testdata/lib_test8.wav", ref, 4) != 1 ||
                    !RIS_LoadSample(&ref[0], &t))
                    DFAIL();
                else {
                    sample_t *d = &Song.Smp[97];
                    size_t bytes = (size_t)t.Length << ((t.Flags & 2) ? 1 : 0);
                    if (!(d->Flags & 1) || d->Length != t.Length ||
                        d->LoopBeg != t.LoopBeg || d->LoopEnd != t.LoopEnd ||
                        d->C5Speed != t.C5Speed || !d->Data || !t.Data ||
                        memcmp(d->Data, t.Data, bytes))
                        DFAIL();
                    free(t.Data);
                }
                if (strcmp(DirSample, "testdata"))
                    DFAIL();
                smp_free_data(&Song.Smp[97]);
                Music_InitSample(&Song.Smp[97]);
                dlg_fake("!cancel");
                handle_global(0x0F);
                if (Song.Smp[97].Flags & 1)
                    DFAIL();
                Song.Header.SmpNum = keepnum;

                for (i = 0; i < 99; i++)
                    had[i] = (uint8_t)(Song.Smp[i].Flags & 1);
                Song.Header.Flags = keepflags;
                Screen = SCR_INSTRUMENTS;
                ListSel = 98;
                {
                    char keepins[sizeof(DirInstr)];
                    memcpy(keepins, DirInstr, sizeof(keepins));
                    dlg_fake("testdata/lib_test.xi");
                    handle_global(0x0F);
                    /* loaded; the folder is InstrumentDirectory (#27) */
                    if (!Song.Ins[98].InstrumentName[0] ||
                        strcmp(DirInstr, "testdata"))
                        DFAIL();
                    memcpy(DirInstr, keepins, sizeof(keepins));
                }
                cd_back(home);
                for (i = 0; i < 99; i++)                /* undo the load */
                    if (!had[i] && (Song.Smp[i].Flags & 1)) {
                        smp_free_data(&Song.Smp[i]);
                        Music_InitSample(&Song.Smp[i]);
                    }
                Song.Ins[98] = keepi;
                Song.Header.SmpNum = keepnum;
                Song.Header.InsNum = keepins;
                Song.Header.Flags = keepflags;
            }

            /* US4: Ctrl-O on an F12 path field */
            {
                int fi = -1, other = -1;
                Screen = SCR_VARS;
                draw_screen();                  /* builds the widgets */
                for (i = 0; i < NW; i++) {
                    if (W[i].type == WT_TEXT && W[i].text == DirSample)
                        fi = i;
                    else if (other < 0 && !(W[i].type == WT_TEXT &&
                             (W[i].text == DirModule || W[i].text == DirInstr)))
                        other = i;
                }
                if (fi < 0)
                    DFAIL();
                else {
                    FocusIdx[SCR_VARS] = fi;
                    dlg_fake("testdata");
                    handle_global(0x0F);
                    if (strcmp(DirSample, "testdata"))
                        DFAIL();
                    dlg_fake("testdata/\xF0\x9F\x8E\xB5");  /* emoji */
                    StatusMsg[0] = 0;
                    handle_global(0x0F);
                    if (strcmp(DirSample, "testdata") ||
                        !strstr(StatusMsg, "cannot show"))
                        DFAIL();
                    {
                        char seventy[71];
                        memset(seventy, 'b', 70);
                        seventy[70] = 0;
                        dlg_fake(seventy);
                        StatusMsg[0] = 0;
                        handle_global(0x0F);
                        if (strcmp(DirSample, "testdata") ||
                            !strstr(StatusMsg, "too long"))
                            DFAIL();
                    }
                    if (other >= 0) {
                        FocusIdx[SCR_VARS] = other;
                        dlg_fake("st_never");
                        handle_global(0x0F);
                        if (!strcmp(DirModule, "st_never") ||
                            !strcmp(DirSample, "st_never") ||
                            !strcmp(DirInstr, "st_never"))
                            DFAIL();
                    }
                }
            }

            /* US5: help -- additions only where dialogs exist; the
             * macOS preview line keeps "Preview" in its column */
            dlg_fake("!cancel");
            hl = help_lines(1);
            for (n = 0; hl[n]; n++)
                ;
            if (n < 3 || hl[n - 1] != HL(PortHelpSave) ||
                hl[n - 3] != HL(PortHelpHead))
                DFAIL();
            dlg_fake(NULL);
            if (!Screen_HasFileDialog()) {  /* IT's lines, minus Ctrl-D (#28) */
                const uint8_t *const *o = HelpContextPtrs[1];
                const uint8_t *const *m = help_lines(1);
                int a = 0, b = 0;
                for (; o[a]; a++) {
                    if (o[a] == HLP_helpglobal_16)
                        continue;
                    if (m[b++] != o[a]) { DFAIL(); break; }
                }
                if (m[b])
                    DFAIL();
            }
            if (help_col_of(HL(PortHelpPreview) + 1, "Preview") !=
                help_col_of(HLP_helpcontext1_181 + 1, "Preview") ||
                help_col_of(HL(PortHelpPreview) + 1, "Preview") < 0) {
                fprintf(stderr, "ITED selftest: DLG preview col %d vs %d\n",
                        help_col_of(HL(PortHelpPreview) + 1, "Preview"),
                        help_col_of(HLP_helpcontext1_181 + 1, "Preview"));
                DFAIL();
            }

            dlg_fake(NULL);
            cd_back(home);
            memcpy(DirSample, keepdir, sizeof(keepdir));
            Screen = keepscr;
            ListSel = keepsel;
            if (!d_ok)
                fprintf(stderr, "ITED selftest: DLG first failure at line %d\n", dfail_line);
#undef DFAIL
            fprintf(stderr, "ITED selftest: [%s]\n",
                    d_ok ? "DLG OK" : "DLG FAIL");
        }

        /* ---- #27: Load Instrument screen as IT 2.14 (D_DrawLoadInstrument)
         * and Ctrl-Q on the file screens ---- */
        {
            int li_ok = 1, i, grp = 0, mod = -1, xi = -1, lfail_line = 0;
#define LFAIL() do { if (li_ok) lfail_line = __LINE__; li_ok = 0; } while (0)
            char keepins[sizeof(DirInstr)];
            it_key_t fk;

            memcpy(keepins, DirInstr, sizeof(keepins));
            li_set_dir("testdata");
            /* "." is shown as "\" (to the root), then ".." */
            if (LiN < 4 || strcmp(LiEnt[0].FileName, "\\") ||
                strcmp(LiEnt[1].FileName, ".."))
                LFAIL();
            for (i = 0; i < LiN; i++) {
                const ilibent_t *e = &LiEnt[i];
                int g = e->Format == 1 ? 0 : (e->Format & 8) ? 1 : 2;
                if (e->Format == 2 || e->Format == 0)   /* never listed */
                    LFAIL();
                if (has_ext_ci(e->FileName, ".WAV") ||
                    has_ext_ci(e->FileName, ".MOD"))
                    LFAIL();
                if (g < grp)                            /* group order */
                    LFAIL();
                grp = g;
                if (e->Format == 8 && !strcmp(e->FileName, "itdemo.it"))
                    mod = i;
                if (e->Format == 4 && !strcmp(e->FileName, "lib_test.xi"))
                    xi = i;
            }
            if (mod < 0 || xi < 0)
                LFAIL();
            /* the drawn rows: columns, dividers, colours */
            LiView = 0; LiFocus = 5; LibUnused = 42;
            if (mod >= 0) {
                LiCur = mod; LiTop = 0;
                li_draw();
                i = 13 + mod - LiTop;
                if (Screen_GetCell(31, i).ch != 0xA8 ||
                    Screen_GetCell(44, i).ch != 0xA8 ||
                    Screen_GetCell(55, i).ch != 0xA8 ||
                    Screen_GetCell(47, i).ch != 'M' ||        /* "Module" */
                    Screen_GetCell(6 + 9, i).ch != 'L' ||     /* Library */
                    Screen_GetCell(32, i).ch != 'i' ||        /* itdemo.it */
                    Screen_GetCell(6, i).attr != 0x30 ||      /* cursor bar */
                    Screen_GetCell(31, i).attr != 0x32)
                    LFAIL();
                if (Screen_GetCell(64, 13).ch != 'A' ||       /* Available */
                    Screen_GetCell(73, 14).ch != '4' ||       /* Samples: 42 */
                    Screen_GetCell(64, 16).ch != 'D')         /* Drive */
                    LFAIL();
            }
            if (xi >= 0) {
                LiCur = xi; LiTop = 0;
                li_draw();
                i = 13 + xi - LiTop;
                if (Screen_GetCell(32, i).ch != 'l' ||          /* lib_test.xi */
                    Screen_GetCell(6, i).attr != 0x30 ||
                    Screen_GetCell(45, i).ch < '0' ||          /* "N Sample" */
                    Screen_GetCell(56, i).ch < '0')            /* size "Nk" */
                    LFAIL();
            }
            /* a module opens in place behind the "." exit record */
            if (mod >= 0) {
                LiCur = mod;
                li_enter_module(&LiEnt[mod]);
                if (!LiInModule || LiN < 2 || LiEnt[0].Format != 1 ||
                    strcmp(LiEnt[0].FileName, ".") || LiEnt[1].Format != 5 ||
                    strcmp(LiEnt[1].FileName, "itdemo.it"))
                    LFAIL();
                LiCur = 0;
                li_enter_dir(&LiEnt[0]);                /* back out */
                if (LiInModule || LiN < 3)
                    LFAIL();
            }
            /* loading from the screen into a blank song (instrument mode
             * off): IT module, XM module, standalone .XI -- Enter in
             * load mode transfers the instrument; view mode does not */
            {
                static const char *const srcs[3] = {
                    "beyond_network.it", "import_test.xm", "lib_test.xi" };
                int s;
                for (s = 0; s < 3; s++) {
                    int j, found = -1, r;
                    new_song();
                    ListSel = 0;
                    LibUnused = RI_UnusedSamples();
                    li_set_dir("testdata");
                    for (j = 0; j < LiN; j++)       /* FileName is cut to 12 */
                        if (!strcmp(path_base(LiEnt[j].SrcFile), srcs[s]))
                            found = j;
                    if (found < 0) { LFAIL(); continue; }
                    LiCur = found;
                    LiView = 1;                         /* library: view */
                    if (LiEnt[found].Format >= 8) {
                        li_enter();                     /* opens module */
                        if (!LiInModule || LiN < 2) { LFAIL(); continue; }
                        LiCur = 1;
                    }
                    if (li_enter() || Song.Ins[0].InstrumentName[0])
                        LFAIL();                        /* view: no load */
                    LiView = 0;                         /* Load Instrument */
                    memset(&fk, 0, sizeof(fk));
                    fk.flags = ITKF_PRESSED; fk.code = ITK_ESC;
                    Screen_KeyFeedTest(&fk, 1);         /* "Enable
                                                           instrument mode?" */
                    r = li_enter();
                    if (r != 1 || !(Song.Ins[0].InstrumentName[0] ||
                                    Song.Ins[0].NoteSampleTable[1]))
                        LFAIL();
                    while (ed_get_key() != ITK_NONE)
                        ;
                }
                new_song();
            }
            memcpy(DirInstr, keepins, sizeof(keepins));

            /* #28: Ctrl-E / Ctrl-I / Ctrl-M do something; the help no
             * longer lists Ctrl-D in any context */
            {
                int c, k2;
                handle_global(0x0D);                    /* Ctrl-M: hide */
                if (Screen_MouseVisible()) LFAIL();
                handle_global(0x0D);                    /* and show */
                if (!Screen_MouseVisible()) LFAIL();
                StatusMsg[0] = 0;
                handle_global(0x09);                    /* Ctrl-I */
                if (!strstr(StatusMsg, "reinitialised") &&
                    !strstr(StatusMsg, "audio device"))
                    LFAIL();
                handle_global(0x05);                    /* Ctrl-E: no crash */
                for (c = 0; c < 15; c++) {
                    const uint8_t *const *hl2 = help_lines(c);
                    for (k2 = 0; hl2[k2]; k2++)
                        if (hl2[k2] == HLP_helpglobal_16) { LFAIL(); break; }
                }
            }

            /* Ctrl-Q on a file screen: asks in place; Cancel stays */
            memset(&fk, 0, sizeof(fk));
            fk.flags = ITKF_PRESSED; fk.code = ITK_ESC;
            Screen_KeyFeedTest(&fk, 1);
            if (modal_global_key(0x11, 6) != 1 || !Running)
                LFAIL();
            fk.code = ITK_ENTER;                        /* OK = quit */
            Screen_KeyFeedTest(&fk, 1);
            if (modal_global_key(0x11, 6) != 2 || Running)
                LFAIL();
            Running = 1;
            if (!li_ok)
                fprintf(stderr, "ITED selftest: LI first failure at line %d\n", lfail_line);
#undef LFAIL
            fprintf(stderr, "ITED selftest: [%s]\n",
                    li_ok ? "LI OK" : "LI FAIL");
        }

        commit_current_pattern();
        fprintf(stderr, "ITED selftest: completed %zu actions, "
                "pattern %u, %u rows, cursor r%d c%d col%d, "
                "tempo %u speed %u pan[1] %02X, "
                "GV=%u GlobalVolume=%u MV=%u [%s] [%s]\n",
                sizeof(script) / sizeof(script[0]),
                CurPattern, CurRows, CurRow, CurChan, CurCol,
                Song.Header.IT, Song.Header.IS, Song.Header.ChnlPan[1],
                Song.Header.GV, GlobalVolume, Song.Header.MV,
                gv_wired ? "GV WIRED OK" : "GV MISMATCH",
                f4_ok ? "F4 OK" : "F4 FAIL");
        ma_mutex_uninit(&Mutex);
        Engine_Lock = NULL; Engine_Unlock = NULL;
        Music_FreeIT();
        return 0;
    }

    /* audio device: the Shift-F5 settings from ited.cfg (a -r on the
     * command line wins over the saved rate) */
    audio_context();
    if (AudRateFromCmdline)
        AudRate = mixspeed;
    audio_restart();

    if (!Screen_Init()) {
        fprintf(stderr, "screen init failed\n");
        return 1;
    }
    Screen_DefineSmallNumbers();        /* editor starts on F2; the small
                                           views need the packed charsets */
    signal(SIGINT, on_sig);

    /* interactive only: the stereo Left/Right requester (feature 013);
     * headless paths (selftest, shots) keep the silent-left default */
    Load_StereoChoice = stereo_choice_prompt;

    while (Running && !g_sig) {
        int key;
        /* Alt-Enter: store pattern in the pattern editor, else the
         * backend's fullscreen toggle */
        Screen_AltEnterIsKey = (Screen == SCR_PATTERN);
        Screen_RightOptPreview = (Screen == SCR_PATTERN);   /* macOS */
        key = ed_get_key();
        if (key != ITK_NONE) {
            if (key == 0x11) {              /* Ctrl-Q -> Quit */
                if (confirm_quit())
                    break;
            } else {
                handle_global(key);
            }
            while (PendingGlobalKey) {      /* from a file screen */
                int pk = PendingGlobalKey;
                PendingGlobalKey = 0;
                if (pk == ITK_F1)
                    help_open(PendingHelpContext);
                else {
                    FromFileScreen = 1;
                    handle_global(pk);
                    FromFileScreen = 0;
                }
            }
            if (Screen == SCR_ORDER)        /* one variable in the
                                               original: Order */
                PEOrder = ListSel;
        }
        /* thumbbar mouse drag: follow the pointer while the button is
         * held (the table is rebuilt every frame, so check the slot) */
        if (DragIdx >= 0) {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b || DragIdx >= NW ||
                (W[DragIdx].type != WT_THUMB &&
                 W[DragIdx].type != WT_CUSTOM))
                DragIdx = -1;
            else if (W[DragIdx].type == WT_THUMB)
                thumb_from_px(&W[DragIdx], m.px);
            else if (W[DragIdx].cdrag)
                W[DragIdx].cdrag(&m);
        }
        /* F11 pan bar drag (issue #8) */
        if (PanDragChan >= 0) {
            it_mouse_t m;
            Screen_GetMouse(&m);
            if (!m.b || Screen != SCR_ORDER)
                PanDragChan = -1;
            else
                pan_from_px(PanDragChan, PanDragBx, m.px);
        }
        redraw();
        if (key == ITK_NONE)
            ma_sleep(20);
    }

    Screen_UnInit();
    if (DeviceUp) ma_device_uninit(&Device);
    ma_mutex_uninit(&Mutex);
    Engine_Lock = NULL; Engine_Unlock = NULL;
    Music_FreeIT();
    return 0;
}
