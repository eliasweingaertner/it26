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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>

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
#include "it_screen.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include "../external/miniaudio.h"

extern const sounddriver_t WAVDriver;
void WAVDriver_Render(int16_t *dst, uint32_t frames);
void WAVDriver_SetMixSpeed(uint32_t hz);
uint32_t WAVDriver_GetMixSpeed(void);
int  Music_LoadIT(const char *path);
void Music_FreeIT(void);

enum { SCR_HELP, SCR_PATTERN, SCR_SAMPLES, SCR_INSTRUMENTS,
       SCR_ORDER, SCR_VARS, SCR_INFO, SCR_MESSAGE, SCR_COUNT };

/* ---- editor state ---- */
static int      Screen = SCR_PATTERN;
static editcell_t Grid[MAX_PATROWS * 64];
static uint16_t  CurPattern = 0;
static uint16_t  CurRows = 64;
static int       CurRow = 0, CurChan = 0, CurCol = 0; /* col 0..3 */
static int       TopRow = 0, LeftChan = 0;
static int       BaseOctave = 4;
static int       EditStep = 1;
static int       CurInstr = 1;
static int       ListSel = 0;             /* sample/instrument/order index */
static int       Running = 1;
static char      FileNameDisp[20] = "";   /* header File Name field */
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

/* scalable thumbbar (F_DrawScalableThumbBar approximation): fixed cell
 * width, value range compressed onto it. */
static void draw_thumbbar_scaled(int x, int y, int min, int max, int val,
                                 int width, uint8_t tattr)
{
    int i, v, cell, sub;

    if (val < min) val = min;
    if (val > max) val = max;

    for (i = 0; i < width; i++)
        Screen_PutChar(x + i, y, 0, 0x03);

    v = (val - min) * (width * 8 - 2) / (max - min) + 1;
    cell = v >> 3;
    sub = v & 7;
    Screen_PutChar(x + cell, y, (uint8_t)(155 + sub), tattr);
    if (155 + sub > 157)
        Screen_PutChar(x + cell + 1, y, (uint8_t)(155 + sub + 5), tattr);

    draw3num(x + width + 1, y, val, 0x21);
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
    int width = dw ? dw : ((max - min + 15) >> 3);
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
        /* F_DrawToggle: "On"/"Off" attr 02h; focus (F_PreToggle)
         * hilights just the word (2 or 3 cells) in 30h */
        Screen_DrawString(w->x0, w->y0, toggle_get(w) ? "On " : "Off", 0x02);
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
        int i, len = 0;
        while (len < w->tmax && w->text[len])
            len++;
        for (i = 0; i < w->tmax; i++) {
            uint8_t c = (i < len) ? (uint8_t)w->text[i] : ' ';
            if (c < 32)
                c = ' ';
            Screen_PutChar(w->x0 + i, w->y0, c, 0x02);
        }
        if (focused) {
            int cx = (len < w->tmax) ? len : w->tmax - 1;
            uint8_t c = (cx < len) ? (uint8_t)w->text[cx] : ' ';
            if (c < 32)
                c = ' ';
            Screen_PutChar(w->x0 + cx, w->y0, c, 0x30);
        }
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
        switch (dir) {
        case 0: if (dy >= 0) continue; score = -dy + abs(dx) * 4; break;
        case 1: if (dy <= 0) continue; score =  dy + abs(dx) * 4; break;
        case 2: if (dx >= 0) continue; score = -dx + abs(dy) * 4; break;
        default:if (dx <= 0) continue; score =  dx + abs(dy) * 4; break;
        }
        if (score < bestscore) {
            bestscore = score;
            best = i;
        }
    }
    if (best >= 0)
        FocusIdx[Screen] = best;
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
        case ITK_HOME:  thumb_set(w, w->min);           return 1;
        case ITK_END:   thumb_set(w, w->max);           return 1;
        default: break;
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
        if (key >= 32 && key < 127) {
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
static void act_stereo_changed(void);
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
    Screen_DrawStringCtl(38, 3,
        (Song.Header.Flags & ITF_INSTRUMENTS) ? HeaderMsg2 : HeaderMsg3,
        0x20, NULL);
    Screen_DrawStringCtl(2, 4, HeaderMsg4, 0x20, nums);

    /* ---- live values, PE_FillHeader positions, attr 5 ---- */
    draw_itname(12, 3, Song.Header.SongName, 25, 0x05);
    drawf(12, 4, 0x05, "%-18.18s", FileNameDisp);

    ed_lock();
    draw3num(12, 5, (PlayMode == 2) ? CurrentOrder : 0, 0x05);
    draw3num(16, 5, Song.Header.OrdNum ? Song.Header.OrdNum - 1 : 0, 0x05);
    draw3num(12, 6, CurPattern, 0x05);
    draw3num(16, 6, Song.Header.PatNum ? Song.Header.PatNum - 1 : 0, 0x05);
    draw3num(12, 7, CurRow, 0x05);
    draw3num(16, 7, CurRows ? CurRows - 1 : 0, 0x05);
    draw3num(50, 4, CurrentSpeed, 0x05);
    draw3num(54, 4, Tempo, 0x05);
    ed_unlock();

    Screen_PutChar(50, 5, (uint8_t)('0' + BaseOctave), 0x05);

    /* instrument/sample number + name */
    if (CurInstr <= 0) {
        drawf(50, 3, 0x05, "..");
        fill(53, 3, 25, '.', 0x05);
    } else {
        const char *name = "";
        drawf(50, 3, 0x05, "%02d", CurInstr % 100);
        if (Song.Header.Flags & ITF_INSTRUMENTS) {
            if (CurInstr <= MAX_INSTRUMENTS)
                name = Song.Ins[CurInstr - 1].InstrumentName;
        } else {
            if (CurInstr <= MAX_SAMPLES)
                name = Song.Smp[CurInstr - 1].SampleName;
        }
        draw_itname(53, 3, name, 25, 0x05);
    }

    /* ---- info line (row 9) + time, IT_L.ASM ---- */
    fill(2, 9, 59, ' ', 0x20);
    ed_lock();
    if (PlayMode == 2) {
        int nums9[6];
        nums9[0] = CurrentOrder;
        nums9[1] = Song.Header.OrdNum ? Song.Header.OrdNum - 1 : 0;
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

    /* ---- dotted title line (row 11), F_DrawInfoLine ---- */
    {
        int len = (int)strlen(title);
        int n1 = (78 - len) / 2 - 1;
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
#define PE_CHANNELS 5                   /* NumChannelsEdit default view */

static uint8_t row_hilight_1(void)      /* beat */
{
    uint8_t h = (uint8_t)(Song.Header.PHiligt & 0xFF);
    return h ? h : 4;
}

static uint8_t row_hilight_2(void)      /* measure */
{
    uint8_t h = (uint8_t)(Song.Header.PHiligt >> 8);
    return h ? h : 16;
}

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

/* volume column display incl. effect letters / pan colour (fg 2) */
static void draw_volume(int x, int y, const editcell_t *c, uint8_t attr)
{
    uint8_t v;

    if (!(c->mask & CM_VOL)) {
        fill(x, y, 2, 173, attr);
        return;
    }
    v = c->vol;
    if (v >= 65) {
        int eff = (v & 0x7F) - 65 + ((v & 0x80) ? 60 : 0);
        if (eff >= 0) {
            drawf(x, y, attr, "%c%d", 'A' + eff / 10, eff % 10);
            return;
        }
        attr = (uint8_t)((attr & 0xF0) | 2);    /* panning colour */
        v &= 0x7F;
    }
    drawf(x, y, attr, "%02d", v % 100);
}

static void draw_pattern(void)
{
    int ch, i, screeny;
    int maxrow = (int)CurRows - 1;

    /* TopRow window (PE_DrawPatternEditNormal) */
    if (TopRow > CurRow) TopRow = CurRow;
    if (TopRow + 32 <= CurRow) TopRow = CurRow - 31;
    if (TopRow > maxrow - 31) TopRow = maxrow - 31;
    if (TopRow < 0) TopRow = 0;

    if (CurChan < LeftChan) LeftChan = CurChan;
    if (CurChan >= LeftChan + PE_CHANNELS) LeftChan = CurChan-PE_CHANNELS+1;
    if (LeftChan > 64 - PE_CHANNELS) LeftChan = 64 - PE_CHANNELS;
    if (LeftChan < 0) LeftChan = 0;

    Screen_DrawBox(4, 14, 4 + 14*PE_CHANNELS, 47, 27);

    /* channel headers, attr 13h / 10h muted */
    for (ch = 0; ch < PE_CHANNELS; ch++) {
        int c = LeftChan + ch;
        uint8_t a = (Song.Header.ChnlPan[c] & 0x80) ? 0x10 : 0x13;
        char hdr[13];
        snprintf(hdr, sizeof(hdr), " Channel %02d ", c + 1);
        Screen_DrawString(5 + 14*ch, 14, hdr, a);
    }

    for (screeny = 0; screeny < 32; screeny++) {
        int row = TopRow + screeny;
        int y = 15 + screeny;
        uint8_t rowattr;

        if (row > maxrow)
            break;

        rowattr = 0x06;
        if (row % row_hilight_2() == 0)      rowattr = 0xE6;
        else if (row % row_hilight_1() == 0) rowattr = 0xF6;

        draw3num(1, y, row, 0x20);          /* row number gutter */

        for (ch = 0; ch < PE_CHANNELS; ch++) {
            int c = LeftChan + ch;
            int x = 5 + 14*ch;
            const editcell_t *cell = &Grid[row * 64 + c];

            draw_note(x, y, cell, rowattr);
            Screen_PutChar(x + 3, y, ' ', rowattr);
            if (cell->mask & CM_INS)
                drawf(x + 4, y, rowattr, "%02d", cell->ins % 100);
            else
                fill(x + 4, y, 2, 173, rowattr);
            Screen_PutChar(x + 6, y, ' ', rowattr);
            draw_volume(x + 7, y, cell, rowattr);
            Screen_PutChar(x + 9, y, ' ', rowattr);
            if (cell->mask & CM_CMD)
                Screen_PutChar(x + 10, y,
                               (uint8_t)('A' + cell->cmd - 1), rowattr);
            else
                Screen_PutChar(x + 10, y, '.', rowattr);
            drawf(x + 11, y, rowattr, "%02X",
                  (cell->mask & CM_CMD) ? cell->cmdval : 0);

            if (ch < PE_CHANNELS - 1)       /* track divider, char 168 */
                Screen_PutChar(x + 13, y, 168,
                               (uint8_t)((rowattr & 0xF0) | 2));
        }
    }

    /* cursor: attr 30h on the active field (PE_PrePatternEdit) */
    if (CurRow >= TopRow && CurRow < TopRow + 32 &&
        CurChan >= LeftChan && CurChan < LeftChan + PE_CHANNELS) {
        static const int fieldoff[4] = { 0, 4, 7, 10 };
        static const int fieldw[4]   = { 3, 2, 2, 3 };
        int x = 5 + 14*(CurChan - LeftChan) + fieldoff[CurCol];
        int y = 15 + (CurRow - TopRow);
        const editcell_t *cell = &Grid[CurRow * 64 + CurChan];

        for (i = 0; i < fieldw[CurCol]; i++)
            Screen_PutChar(x + i, y, 0, 0x30);
        /* re-draw the field content in cursor colours */
        if (CurCol == 0) {
            draw_note(x, y, cell, 0x30);
        } else if (CurCol == 1) {
            if (cell->mask & CM_INS)
                drawf(x, y, 0x30, "%02d", cell->ins % 100);
            else
                fill(x, y, 2, 173, 0x30);
        } else if (CurCol == 2) {
            draw_volume(x, y, cell, 0x30);
        } else {
            if (cell->mask & CM_CMD)
                Screen_PutChar(x, y, (uint8_t)('A' + cell->cmd - 1), 0x30);
            else
                Screen_PutChar(x, y, '.', 0x30);
            drawf(x + 1, y, 0x30, "%02X",
                  (cell->mask & CM_CMD) ? cell->cmdval : 0);
        }
    }

    /* channel notch at the bottom edge under the cursor channel */
    {
        int x = 5 + 14*(CurChan - LeftChan);
        for (i = 0; i < 3; i++)
            Screen_PutChar(x + i, 47, 0xA9, 0x23);
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
static int PanSel;                              /* selected pan channel */

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

static int ed_mem_nonzero(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        if (b[i])
            return 1;
    return 0;
}

/* generic Yes/No confirm (O1_Confirm*List), default No */
static int confirm_box(const char *text)
{
    int sel = 1;

    for (;;) {
        int key, tx;

        draw_screen();
        Screen_DrawBox(24, 22, 55, 27, 27);
        tx = 40 - (int)strlen(text) / 2;
        Screen_DrawString(tx, 23, text, 0x20);
        draw_button_style(30, 24, 36, 26, 3, " Yes", 0, sel == 0);
        draw_button_style(43, 24, 48, 26, 3, " No", 0, sel == 1);
        Screen_Update();

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 0;
        case ITK_LEFT: case ITK_RIGHT: case ITK_TAB: case ITK_SHIFT_TAB:
            sel ^= 1; break;
        case 'y': case 'Y': return 1;
        case 'n': case 'N': case ITK_ESC: return 0;
        case ITK_ENTER: return sel == 0;
        default: break;
        }
    }
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
        Screen_DrawBox(24, 22, 55, 27, 27);
        Screen_DrawString(26, 23, title, 0x20);
        drawf(26, 25, 0x30, "%-10.10s", buf);
        Screen_Update();

        key = Key_Get();
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
        Screen_DrawBox(18, 21, 61, 28, 27);
        drawf(24, 22, 0x20, "Convert sample to %d bit?", to16 ? 16 : 8);
        draw_button_style(21, 24, 34, 26, 3, " Convert data", 0, sel == 0);
        draw_button_style(36, 24, 49, 26, 3, " Adjust  end", 0, sel == 1);
        draw_button_style(51, 24, 59, 26, 3, " Cancel", 0, sel == 2);
        Screen_Update();

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 0;
        case ITK_ESC:  return 0;
        case ITK_LEFT:  sel = (sel + 2) % 3; break;
        case ITK_RIGHT: case ITK_TAB: sel = (sel + 1) % 3; break;
        case ITK_ENTER: return sel == 0 ? 1 : sel == 1 ? 2 : 0;
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
    amp = prompt_number("Amplification % (100 = no change)", sug, 400);
    if (amp <= 0)
        return;
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
    memset(s, 0, sizeof(*s));           /* Music_ReleaseSample + name */
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

    if (ed_mem_nonzero(&Song.Smp[98], 80) || cur >= 98)
        return;
    stop_song();
    ed_lock();
    for (i = 98; i > cur; i--)
        Song.Smp[i] = Song.Smp[i - 1];
    memset(&Song.Smp[cur], 0, sizeof(sample_t));
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
    memset(&Song.Smp[98], 0, sizeof(sample_t));
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
        uint8_t a = (focused && i == SmpFieldSel) ? 0x30 : 0x03;
        switch (f->kind) {
        case 0:
            drawf(64, f->y, a, "%7u",
                  (unsigned)*((const uint32_t *)smp_field_ptr(f->which)));
            break;
        case 1:
            drawf(64, f->y, a, "%-9.9s",
                  !(s->Flags & 0x10) ? "Off"
                  : (s->Flags & 0x40) ? "Ping Pong" : "On");
            break;
        case 2:
            drawf(64, f->y, a, "%-9.9s",
                  !(s->Flags & 0x20) ? "Off"
                  : (s->Flags & 0x80) ? "Ping Pong" : "On");
            break;
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

/* Alt-key dispatch for the F3 sample list; returns 1 when consumed */
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
    case ITK_ALT_A + ('O'-'A'):
    case ITK_ALT_A + ('T'-'A'):
    case ITK_ALT_A + ('W'-'A'):
        status("Sample disk ops arrive with the sample library (006)");
        break;
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

static void draw_samples(void)
{
    int i, n = Song.Header.SmpNum ? Song.Header.SmpNum : 1;
    int rows = 35, top;
    sample_t *s;

    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;
    top = ListSel - rows / 2;
    if (top > n - rows) top = n - rows;
    if (top < 0) top = 0;
    SmpListTop = top;

    Screen_DrawBox(4, 12, 35, 48, 27);          /* SampleListBox */
    for (i = 0; i < rows; i++) {
        int idx = top + i;
        uint8_t a = (idx == ListSel) ? 0x30 : 0x06;
        if (idx < 0 || idx >= n)
            continue;
        drawf(5, 13 + i, a, "%02d:", idx + 1);
        draw_itname(8, 13 + i, Song.Smp[idx].SampleName, 26, a);
    }

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
    drawf(64, 22, 0x03, "%d bits", (s->Flags & 2) ? 16 : 8);
    drawf(64, 23, 0x03, "%u", s->Length);

    widgets_draw();
}

/* ===================================================================
 * Instrument list (F4) -- object-exact port of the four object lists
 * O1_InstrumentListGeneral/Volume/Panning/Pitch (IT_OBJ1.ASM 5629..)
 * with the custom-draw objects from IT_I.ASM: I_DrawInstrumentWindow,
 * I_DrawNoteWindow, I_DrawEnvelope (+node editing), and
 * I_DrawPitchPanCenter. FILTERENVELOPES=1 layout (the 2.17 build).
 * =================================================================== */
static int key_to_note(int key);
static void jam_note(int gnote, int chan);

static uint8_t InsTab = 0;         /* InstrumentScreen: 0=General 1=Vol
                                    * 2=Pan 3=Pitch                     */
static int CurrentNode = 0;        /* envelope node cursor              */
static int NodeHeld = 0;           /* Enter "grabs" the node            */
static int NoteWinTop = 0;         /* note-translation window scroll    */
static int NoteWinSel = 0;         /* CurrentNote 0..119                */
static int NotePos = 0;            /* cursor column 0..3                */
static uint8_t NoteSampleNumber = 1; /* SampleNumber (IT_I.ASM)         */
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

static void ins_op_delete(void)         /* Alt-D: I_DeleteInstrument */
{
    if (!confirm_box("Delete instrument?"))
        return;
    stop_song();
    ed_lock();
    memset(cur_ins(), 0, sizeof(instrument_t));
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

static void ins_op_insert_slot(void)    /* Alt-Ins: I_InsertInstrument */
{
    int cur = ListSel, i;

    if (ed_mem_nonzero(&Song.Ins[98], 554) || cur >= 98)
        return;
    stop_song();
    ed_lock();
    for (i = 98; i > cur; i--)
        Song.Ins[i] = Song.Ins[i - 1];
    memset(&Song.Ins[cur], 0, sizeof(instrument_t));
    ed_unlock();
    if (Song.Header.Flags & ITF_INSTRUMENTS)
        pattern_remap_ins(0, cur + 1, 0);
}

static void ins_op_remove_slot(void)    /* Alt-Del: I_RemoveInstrument */
{
    int cur = ListSel, i;

    stop_song();
    ed_lock();
    for (i = cur; i < 98; i++)
        Song.Ins[i] = Song.Ins[i + 1];
    memset(&Song.Ins[98], 0, sizeof(instrument_t));
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
    case ITK_ALT_A + ('J'-'A'): ins_op_scale_volumes(); break;
    case ITK_ALT_A + ('P'-'A'): ins_op_copy();          break;
    case ITK_ALT_A + ('R'-'A'): ins_op_replace();       break;
    case ITK_ALT_A + ('S'-'A'): ins_op_swap();          break;
    case ITK_ALT_A + ('X'-'A'): ins_op_exchange();      break;
    case ITK_ALT_A + ('U'-'A'):
        status("Update pattern data (Alt-U) not ported yet");
        break;
    case ITK_ALT_A + ('O'-'A'):
        status("Instrument disk ops arrive with the library (006)");
        break;
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

        if (note == NoteWinSel)
            a |= 0xE0;                     /* row hilight (+0E0h) */

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

static void notewin_play_current(void)
{
    CurInstr = ListSel + 1;
    jam_note(NoteWinSel + 1, 40);
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
        int gn = key_to_note(key);
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
    int i, n = Song.Header.InsNum ? Song.Header.InsNum : 1;
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
        drawf(2, 13 + i, 0x20, "%02d", (idx + 1) % 100);
        draw_itname(5, 13 + i, Song.Ins[idx].InstrumentName, 25, a);
    }

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
    int i, n = Song.Header.OrdNum;
    int focusw = FocusIdx[SCR_ORDER];

    if (n <= 0) n = 1;
    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;
    if (PanSel < 0) PanSel = 0;
    if (PanSel > 63) PanSel = 63;

    NW = 0;
    wlist(6, 15, 9, 46, 15, order_list_lkey, order_list_lclick);
    wlist(20, 15, 39, 46, 15, pan_left_lkey, pan_left_lclick);
    wlist(54, 15, 73, 46, 15, pan_right_lkey, pan_right_lclick);

    /* order list, type-12 object at (2,15), 32 entries */
    {
        int top = ListSel - 16;
        if (top > n - 32) top = n - 32;
        if (top < 0) top = 0;
        OrdListTop = top;

        Screen_DrawBox(5, 14, 10, 47, 27);
        for (i = 0; i < 32; i++) {
            int idx = top + i;
            uint8_t a;
            if (idx >= n || idx >= MAX_ORDERS)
                break;
            a = (idx == ListSel) ? 0x30 : 0x03;
            draw3num(1, 15 + i, idx, 0x20);
            if (Song.Orders[idx] == 255)
                Screen_DrawString(6, 15 + i, "---", a);
            else if (Song.Orders[idx] == 254)
                Screen_DrawString(6, 15 + i, "+++", a);
            else
                draw3num(6, 15 + i, Song.Orders[idx], a);
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
            uint8_t a = sel ? 0x03 : 0x02;

            drawf(bx - 11, 15 + i, sel ? 0x30 : 0x20,
                  "Channel %02d", chan + 1);
            if (pan & 0x80) {
                Screen_DrawString(bx + 1, 15 + i, "Muted", a);
            } else if ((pan & 0x7F) == 100) {
                Screen_DrawString(bx, 15 + i, "Surround", a);
            } else {
                int pos = (pan & 0x7F) * 8 / 64;
                Screen_PutChar(bx + pos, 15 + i, 254, a);
            }
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
        w = wthumb(17, 19, 31, 255, &Song.Header.IT, 0, 28);
        w->action = act_tempo_changed;
        w = wthumb(17, 20, 1, 255, &Song.Header.IS, 0, 28);
        w->action = act_speed_changed;
    }
    wthumb(17, 23, 0, 128, &Song.Header.GV, 0, 0)->action = act_gv_changed;
    wthumb(17, 24, 0, 128, &Song.Header.MV, 0, 0)->action = act_mv_changed;
    wthumb(17, 25, 0, 128, &Song.Header.Sep, 0, 0)->action = act_stereo_changed;
    wtogglef(17, 26, &Song.Header.Flags, ITF_OLD_EFFECTS);
    wtogglef(17, 27, &Song.Header.Flags, ITF_LINK_G_TO_EF);

    wradiof(16, 29, 30, 31, " Instruments", &Song.Header.Flags,
            ITF_INSTRUMENTS, 0);
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
static void draw_help(void)
{
    static const char *lines[] = {
      "",
      "  Summary of keys.",
      "",
      "  Screens:  F1 help, F2 pattern, F3 samples, F4 instruments,",
      "            F11 order list, F12 song variables, ESC main menu.",
      "",
      "  Playback: F5 play song + info page, F6 play pattern, F8 stop.",
      "",
      "  Pattern editor:",
      "    Arrows/PgUp/PgDn/Home/End      move cursor",
      "    Tab / Shift-Tab                next / previous channel",
      "    [ ]  { }                       octave / edit step down,up",
      "    - =                            previous / next pattern",
      "    Piano keys                     enter note",
      "    1  `                           note cut (^^^), note off (==)",
      "    Del / Ins                      clear cell, pull / push rows",
      "    . in any column                clear field",
      "",
      "  Piano:  Z X C V B N M , . /  =  C D E F G A B C D E (low)",
      "          Q W E R T Y U I O P  =  C D E F G A B C D E (high)",
      "          (with sharps on the row above each)",
      "",
      "  Samples/Instruments: arrows select, piano key auditions.",
      "",
      "  Ctrl-Q quits.",
    };
    int i;

    Screen_DrawBox(1, 12, 78, 48, 27);
    for (i = 0; i < (int)(sizeof(lines)/sizeof(lines[0])); i++)
        Screen_DrawString(3, 13 + i, lines[i], 0x06);
    NW = 0;
    wbutton(36, 46, 44, 48, "  Done", act_help_done);
    widgets_draw();
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
            else {
                int v = p + 1;
                Screen_PutChar(64 + (v >> 3), y,
                               (uint8_t)(155 + (v & 7)), 2);
                if (155 + (v & 7) > 157)
                    Screen_PutChar(64 + (v >> 3) + 1, y,
                                   (uint8_t)(155 + (v & 7) + 5), 2);
            }
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
static void handle_info_key(int key)
{
    dispwin_t *w = &InfoWin[InfoCurWindow];

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
    case 0x15: {                        /* Ctrl-U = DisplayAltUp */
        int idx    = (InfoCurWindow == 0) ? 1 : InfoCurWindow + 1;
        int minlen = (InfoCurWindow == 0) ? 4 : 3;
        if (idx < InfoNumWindows && InfoWin[idx - 1].length > minlen) {
            InfoWin[idx].length++;
            InfoWin[idx - 1].length--;
            InfoWin[idx].topline--;
        }
        return;
    }
    case 0x04: {                        /* Ctrl-D = DisplayAltDown */
        int idx = InfoCurWindow + 1;
        if (idx < InfoNumWindows && InfoWin[idx].length > 3) {
            InfoWin[idx - 1].length++;
            InfoWin[idx].length--;
            InfoWin[idx].topline++;
        }
        return;
    }
    case 'Q':                           /* DisplayToggleChannel */
        ed_lock();
        Music_ToggleChannel((uint16_t)InfoCurChannel);
        ed_unlock();
        return;
    case 'S':                           /* DisplaySoloChannel */
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
    case 'r':                           /* DisplayToggleReverse (Alt-R) */
        ed_lock(); Music_ToggleReverse(); ed_unlock();
        /* the original engine SetInfoLines this itself (same text in
         * both directions); the port keeps the engine UI-free */
        status("Left/right outputs reversed");
        return;
    case 's':                           /* DisplayToggleStereo (Alt-S) */
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
    case 0x0C:                          /* Ctrl-L: clear (Alt-C stand-in) */
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
static void draw_screen(void)
{
    static const char *titles[] = {
        "Help (F1)", "Pattern Editor (F2)", "Sample List (F3)",
        "Instrument List (F4)", "Order List and Panning (F11)",
        "Song Variables & Directory Configuration (F12)",
        "Information (F5)", "Message Editor (Shift-F9)",
    };

    Screen_Clear(0x20);
    if (Screen == SCR_INFO && InfoFullScreen) {
        /* Display_FullScreen mode 200: no standard chrome, only the
         * display data over rows 1..49 */
        NW = 0;
        draw_info();
        return;
    }
    draw_chrome(titles[Screen]);
    switch (Screen) {
    case SCR_PATTERN:     NW = 0; draw_pattern(); break;
    case SCR_SAMPLES:     draw_samples(); break;
    case SCR_INSTRUMENTS: draw_instruments(); break;
    case SCR_ORDER:       draw_order(); break;
    case SCR_VARS:        draw_vars(); break;
    case SCR_HELP:        draw_help(); break;
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
static int key_to_note(int key)
{
    static const struct { char k; int semitone, oct; } map[] = {
        {'z',0,0},{'s',1,0},{'x',2,0},{'d',3,0},{'c',4,0},{'v',5,0},
        {'g',6,0},{'b',7,0},{'h',8,0},{'n',9,0},{'j',10,0},{'m',11,0},
        {',',12,0},{'l',13,0},{'.',14,0},{';',15,0},{'/',16,0},
        {'q',0,1},{'2',1,1},{'w',2,1},{'3',3,1},{'e',4,1},{'r',5,1},
        {'5',6,1},{'t',7,1},{'6',8,1},{'y',9,1},{'7',10,1},{'u',11,1},
        {'i',12,1},{'9',13,1},{'o',14,1},{'0',15,1},{'p',16,1},
    };
    size_t i;
    int lk = (key >= 'A' && key <= 'Z') ? key - 'A' + 'a' : key;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].k == lk) {
            int note = (BaseOctave + map[i].oct) * 12 + map[i].semitone;
            if (note < 0 || note > 119)
                return -1;
            return note + 1;
        }
    }
    return -1;
}

/* ===================================================================
 * Engine control helpers (locked)
 * =================================================================== */
static void commit_current_pattern(void)
{
    Pattern_Pack(CurPattern, Grid, CurRows);
}

static void load_pattern(uint16_t pat)
{
    CurPattern = pat;
    CurRows = Pattern_EnsureExists(pat, 64);
    CurRows = Pattern_Unpack(pat, Grid);
    if (CurRow >= (int)CurRows) CurRow = CurRows - 1;
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

static void rows_shift_up(int chan, int fromrow)   /* Del */
{
    int r;
    for (r = fromrow; r < (int)CurRows - 1; r++)
        Grid[r * 64 + chan] = Grid[(r + 1) * 64 + chan];
    cell_clear(&Grid[((int)CurRows - 1) * 64 + chan]);
}

static void rows_shift_down(int chan, int fromrow) /* Ins */
{
    int r;
    for (r = (int)CurRows - 1; r > fromrow; r--)
        Grid[r * 64 + chan] = Grid[(r - 1) * 64 + chan];
    cell_clear(&Grid[fromrow * 64 + chan]);
}

static int hexval(int k)
{
    if (k >= '0' && k <= '9') return k - '0';
    if (k >= 'a' && k <= 'f') return k - 'a' + 10;
    if (k >= 'A' && k <= 'F') return k - 'A' + 10;
    return -1;
}

static void advance_row(void)
{
    CurRow += EditStep;
    if (CurRow >= (int)CurRows) CurRow = CurRows - 1;
}

static void handle_pattern_key(int key)
{
    editcell_t *cell = &Grid[CurRow * 64 + CurChan];
    int changed = 0;

    switch (key) {
    case ITK_UP:    if (CurRow > 0) CurRow--; return;
    case ITK_DOWN:  if (CurRow < (int)CurRows - 1) CurRow++; return;
    case ITK_LEFT:
        if (CurCol > 0) CurCol--;
        else if (CurChan > 0) { CurChan--; CurCol = 3; }
        return;
    case ITK_RIGHT:
        if (CurCol < 3) CurCol++;
        else if (CurChan < 63) { CurChan++; CurCol = 0; }
        return;
    case ITK_TAB:   if (CurChan < 63) CurChan++; CurCol = 0; return;
    case ITK_SHIFT_TAB: if (CurChan > 0) CurChan--; CurCol = 0; return;
    case ITK_PGUP:  CurRow -= 16; if (CurRow < 0) CurRow = 0; return;
    case ITK_PGDN:  CurRow += 16; if (CurRow >= (int)CurRows)
                        CurRow = CurRows - 1; return;
    case ITK_HOME:  CurRow = 0; return;
    case ITK_END:   CurRow = CurRows - 1; return;
    case ITK_INS:   rows_shift_down(CurChan, CurRow); changed = 1; break;
    case ITK_DEL:   rows_shift_up(CurChan, CurRow); changed = 1; break;
    case '[':       if (BaseOctave > 0) BaseOctave--; return;
    case ']':       if (BaseOctave < 8) BaseOctave++; return;
    case '{':       if (EditStep > 0) EditStep--; return;
    case '}':       if (EditStep < 16) EditStep++; return;
    case '-':
        if (CurPattern > 0) { commit_current_pattern();
            load_pattern(CurPattern - 1); } return;
    case '=':
        commit_current_pattern();
        load_pattern(CurPattern + 1 < MAX_PATTERNS ? CurPattern+1
                                                   : CurPattern);
        return;
    default: break;
    }

    if (!changed && CurCol == 0) {
        if (key == '1') {
            cell->note = GNOTE_CUT; cell->mask |= CM_NOTE;
            if (CurInstr) { cell->ins = (uint8_t)CurInstr;
                            cell->mask |= CM_INS; }
            changed = 1; advance_row();
        } else if (key == '`') {
            cell->note = GNOTE_OFF; cell->mask |= CM_NOTE;
            changed = 1; advance_row();
        } else if (key == '.') {
            cell->mask &= (uint8_t)~CM_NOTE; changed = 1;
        } else {
            int gn = key_to_note(key);
            if (gn > 0) {
                cell->note = (uint8_t)gn; cell->mask |= CM_NOTE;
                cell->ins = (uint8_t)CurInstr; cell->mask |= CM_INS;
                jam_note(gn, CurChan);
                changed = 1;
                advance_row();
            }
        }
    } else if (!changed && CurCol == 1) {
        if (key >= '0' && key <= '9') {
            uint8_t cur = (cell->mask & CM_INS) ? cell->ins : 0;
            cur = (uint8_t)(((cur * 10) + (key - '0')) % 100);
            cell->ins = cur; cell->mask |= CM_INS;
            CurInstr = cur ? cur : CurInstr;
            changed = 1; advance_row();
        } else if (key == '.') {
            cell->mask &= (uint8_t)~CM_INS; changed = 1;
        }
    } else if (!changed && CurCol == 2) {
        if (key >= '0' && key <= '9') {
            int v = (cell->mask & CM_VOL) ? cell->vol : 0;
            v = (v * 10 + (key - '0')) % 100;
            if (v > 64) v = 64;
            cell->vol = (uint8_t)v; cell->mask |= CM_VOL;
            changed = 1; advance_row();
        } else if (key == '.') {
            cell->mask &= (uint8_t)~CM_VOL; changed = 1;
        }
    } else if (!changed && CurCol == 3) {
        int lk = (key >= 'a' && key <= 'z') ? key - 32 : key;
        if (lk >= 'A' && lk <= 'Z') {
            cell->cmd = (uint8_t)(lk - 'A' + 1); cell->mask |= CM_CMD;
            changed = 1;
        } else {
            int h = hexval(key);
            if (h >= 0) {
                cell->cmdval = (uint8_t)((cell->cmdval << 4) | h);
                cell->mask |= CM_CMD;
                changed = 1; advance_row();
            } else if (key == '.') {
                cell->mask &= (uint8_t)~CM_CMD; cell->cmdval = 0;
                changed = 1;
            }
        }
    }

    if (changed)
        commit_current_pattern();
}

/* ===================================================================
 * List widgets: key/click handlers and widget action callbacks
 * =================================================================== */
static int generic_list_lkey(int key, int n)
{
    switch (key) {
    case ITK_UP:   if (ListSel > 0) ListSel--; return 1;
    case ITK_DOWN: if (ListSel < n - 1) ListSel++; return 1;
    case ITK_PGUP: ListSel -= 16; if (ListSel < 0) ListSel = 0; return 1;
    case ITK_PGDN: ListSel += 16; if (ListSel >= n) ListSel = n-1; return 1;
    case ITK_HOME: ListSel = 0; return 1;
    case ITK_END:  ListSel = n - 1; return 1;
    default: break;
    }
    {
        int gn = key_to_note(key);
        if (gn > 0) {
            CurInstr = ListSel + 1;
            jam_note(gn, 40);
            return 1;
        }
    }
    return 0;
}

static int sample_list_lkey(int key)
{
    return generic_list_lkey(key, Song.Header.SmpNum
                                  ? Song.Header.SmpNum : 1);
}

static int instr_list_lkey(int key)
{
    return generic_list_lkey(key, Song.Header.InsNum
                                  ? Song.Header.InsNum : 1);
}

static void sample_list_lclick(int row, int mx, int mpx)
{
    int n = Song.Header.SmpNum ? Song.Header.SmpNum : 1;
    (void)mx; (void)mpx;
    if (row >= 0 && SmpListTop + row < n)
        ListSel = SmpListTop + row;
}

static void instr_list_lclick(int row, int mx, int mpx)
{
    int n = Song.Header.InsNum ? Song.Header.InsNum : 1;
    (void)mx; (void)mpx;
    if (row >= 0 && InsListTop + row < n)
        ListSel = InsListTop + row;
}

static int order_list_lkey(int key)
{
    int n = Song.Header.OrdNum ? Song.Header.OrdNum : 1;
    switch (key) {
    case ITK_UP:   if (ListSel > 0) ListSel--; return 1;
    case ITK_DOWN: if (ListSel < n - 1) ListSel++; return 1;
    case ITK_PGUP: ListSel -= 16; if (ListSel < 0) ListSel = 0; return 1;
    case ITK_PGDN: ListSel += 16; if (ListSel >= n) ListSel = n-1; return 1;
    case ITK_HOME: ListSel = 0; return 1;
    case ITK_END:  ListSel = n - 1; return 1;
    case '=': case '+':
        ed_lock();
        if (Song.Orders[ListSel] < 199) Song.Orders[ListSel]++;
        ed_unlock(); return 1;
    case '-':
        ed_lock();
        if (Song.Orders[ListSel] > 0 && Song.Orders[ListSel] < 200)
            Song.Orders[ListSel]--;
        ed_unlock(); return 1;
    case ITK_ENTER:
        if (Song.Orders[ListSel] < 200) {
            commit_current_pattern();
            load_pattern(Song.Orders[ListSel]);
            Screen = SCR_PATTERN;
        }
        return 1;
    default: break;
    }
    return 0;
}

static void order_list_lclick(int row, int mx, int mpx)
{
    int n = Song.Header.OrdNum ? Song.Header.OrdNum : 1;
    (void)mx; (void)mpx;
    if (row >= 0 && OrdListTop + row < n)
        ListSel = OrdListTop + row;
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
    case ITK_PGDN: PanSel += 8; if (PanSel > base + 31)
                       PanSel = base + 31; return 1;
    case ITK_HOME: PanSel = base; return 1;
    case ITK_END:  PanSel = base + 31; return 1;
    case ITK_LEFT:  pan_adjust(PanSel, -1); return 1;
    case ITK_RIGHT: pan_adjust(PanSel,  1); return 1;
    case 'l': case 'L': pan_set(PanSel, 0);   return 1;
    case 'm': case 'M': pan_set(PanSel, 32);  return 1;
    case 'r': case 'R': pan_set(PanSel, 64);  return 1;
    case 's': case 'S': pan_set(PanSel, 100); return 1;
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

static void pan_col_lclick(int row, int mx, int mpx, int base, int bx)
{
    if (row < 0 || row > 31)
        return;
    PanSel = base + row;
    if (mx >= bx && mx <= bx + 8) {         /* inside the slider */
        int p = (mpx - bx * 8) * 65 / 72;
        if (p < 0) p = 0;
        if (p > 64) p = 64;
        pan_set(PanSel, p);
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

static void act_help_done(void)
{
    Screen = SCR_PATTERN;
}

static void act_save_prefs(void)
{
    FILE *fp = fopen("ited.cfg", "w");
    if (!fp) {
        status("Can't write ited.cfg here.");
        return;
    }
    fprintf(fp, "moduledir=%s\nsampledir=%s\ninstrdir=%s\n"
            "octave=%d\nstep=%d\n",
            DirModule, DirSample, DirInstr, BaseOctave, EditStep);
    fclose(fp);
    status("Preferences saved to ited.cfg.");
}

/* ===================================================================
 * File requester (F9) -- IT_F.ASM "Load Module (F9)" screen layout
 * =================================================================== */
#define REQ_MAXFILES 1024
#define REQ_MAXDIRS  256
typedef struct reqfile_t {
    char name[64];
    char songname[27];
    long size;
} reqfile_t;

static reqfile_t ReqFiles[REQ_MAXFILES];
static char      ReqDirs[REQ_MAXDIRS][64];
static char      ReqDrives[26];
static int       ReqNF, ReqND, ReqNDrv;
static int       ReqFocus;          /* 0 files, 1 dirs, 2 drives, 3 name */
static int       FSel, FTop, DSel, DTop, VSel;
static char      ReqName[26] = "*.IT";

static int req_file_cmp(const void *a, const void *b)
{
    return strcmp(((const reqfile_t *)a)->name,
                  ((const reqfile_t *)b)->name);
}

static int req_dir_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int has_it_ext(const char *name)
{
    size_t l = strlen(name);
    return l > 3 && name[l-3] == '.' &&
           (name[l-2] == 'i' || name[l-2] == 'I') &&
           (name[l-1] == 't' || name[l-1] == 'T');
}

static void req_read_songname(reqfile_t *f)
{
    FILE *fp = fopen(f->name, "rb");
    char hdr[30];

    f->songname[0] = 0;
    if (!fp)
        return;
    if (fread(hdr, 1, 30, fp) == 30 && !memcmp(hdr, "IMPM", 4)) {
        memcpy(f->songname, hdr + 4, 26);
        f->songname[26] = 0;
    }
    fclose(fp);
}

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
                    snprintf(f->name, sizeof(f->name), "%s", fd.cFileName);
                    f->size = (long)fd.nFileSizeLow;
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
                                 "%s", e->d_name);
                } else if (has_it_ext(e->d_name) && ReqNF < REQ_MAXFILES) {
                    reqfile_t *f = &ReqFiles[ReqNF++];
                    snprintf(f->name, sizeof(f->name), "%s", e->d_name);
                    f->size = (long)st.st_size;
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

static const uint8_t SearchText[] =
    "Search\015\015\015Format\015  Size\015  Date\015  Time";
static const uint8_t FileText[] = " Filename\015Directory";

static int do_load_named(const char *path);

static int ReqSave;                     /* 0 = load (F9), 1 = save (F10) */

static void draw_file_requester(void)
{
    int i;

    Screen_Clear(0x20);
    draw_chrome(ReqSave ? "Save Module (F10)" : "Load Module (F9)");

    Screen_DrawBox(2, 12, 41, 44, 27);          /* FileBox */
    Screen_DrawBox(43, 12, 56, 34, 27);         /* DirBox */
    Screen_DrawBox(58, 12, 67, 34, 27);         /* DriveBox */
    Screen_DrawBox(50, 36, 77, 38, 27);         /* SearchBox */
    Screen_DrawBox(50, 39, 77, 44, 27);         /* FileInfoBox */
    Screen_DrawBox(12, 45, 77, 48, 27);         /* FileNameBox */
    Screen_DrawStringCtl(44, 37, SearchText, 0x20, NULL);
    Screen_DrawStringCtl(3, 46, FileText, 0x20, NULL);

    /* files (left box, 31 rows): name + song name columns */
    if (FSel < FTop) FTop = FSel;
    if (FSel >= FTop + 31) FTop = FSel - 30;
    if (FTop < 0) FTop = 0;
    for (i = 0; i < 31 && FTop + i < ReqNF; i++) {
        int idx = FTop + i;
        uint8_t a = (idx == FSel) ? (ReqFocus == 0 ? 0x30 : 0x20) : 0x03;
        drawf(3, 13 + i, a, "%-13.13s ", ReqFiles[idx].name);
        drawf(17, 13 + i, a, "%-23.23s", ReqFiles[idx].songname);
    }
    if (ReqNF == 0)
        Screen_DrawString(3, 13, "(no .it modules here)", 0x03);

    /* directories (middle box, 21 rows) */
    if (DSel < DTop) DTop = DSel;
    if (DSel >= DTop + 21) DTop = DSel - 20;
    if (DTop < 0) DTop = 0;
    for (i = 0; i < 21 && DTop + i < ReqND; i++) {
        int idx = DTop + i;
        uint8_t a = (idx == DSel) ? (ReqFocus == 1 ? 0x30 : 0x20) : 0x03;
        drawf(44, 13 + i, a, "%-12.12s", ReqDirs[idx]);
    }

    /* drives (right box) */
    if (VSel >= ReqNDrv) VSel = ReqNDrv ? ReqNDrv - 1 : 0;
    for (i = 0; i < 21 && i < ReqNDrv; i++) {
        uint8_t a = (i == VSel) ? (ReqFocus == 2 ? 0x30 : 0x20) : 0x05;
        drawf(59, 13 + i, a, "Drive %c:", ReqDrives[i]);
    }

    /* file info */
    if (ReqNF && FSel < ReqNF) {
        drawf(58, 40, 0x05, "Impulse Tracker");
        drawf(58, 41, 0x05, "%09ld", ReqFiles[FSel].size);
    }

    /* filename input + current directory */
    {
        int len = (int)strlen(ReqName);
        drawf(13, 46, 0x02, "%-25.25s", ReqName);
        if (ReqFocus == 3)
            Screen_PutChar(13 + (len < 25 ? len : 24), 46,
                           (uint8_t)(len < 25 ? ' ' : ReqName[24]), 0x30);
    }
    {
        char cwd[256] = "";
        if (getcwd(cwd, sizeof(cwd)))
            ;
        drawf(13, 47, 0x05, "%-64.64s", cwd);
    }
}

/* D_CheckOverWrite's confirm dialog (O1_ConfirmOverWriteList):
 * "Overwrite file?" with Yes/No, default No. `bg` redraws the screen
 * beneath the modal. */
static int confirm_overwrite(void (*bg)(void))
{
    int sel = 1;                        /* 0 = Yes, 1 = No */

    for (;;) {
        int key;

        bg();
        Screen_DrawBox(24, 22, 55, 27, 27);
        Screen_DrawString(32, 23, "Overwrite file?", 0x20);
        draw_button_style(30, 24, 36, 26, 3, " Yes", 0, sel == 0);
        draw_button_style(43, 24, 48, 26, 3, " No", 0, sel == 1);
        Screen_Update();

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }
        switch (key) {
        case ITK_QUIT: Running = 0; return 0;
        case ITK_LEFT: case ITK_RIGHT: case ITK_TAB:
        case ITK_SHIFT_TAB:
            sel ^= 1;
            break;
        case 'y': case 'Y':
            return 1;
        case 'n': case 'N': case ITK_ESC:
            return 0;
        case ITK_ENTER:
            return sel == 0;
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

/* D_SaveModule tail + D_PostFileSaveWindow2: apply .IT when no '.',
 * confirm overwrite, run the writer, report. */
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
    if (!strchr(name, '.') && strlen(name) < sizeof(name) - 4)
        strcat(name, ".IT");

    f = fopen(name, "rb");
    if (f) {
        fclose(f);
        if (!confirm_overwrite(draw_file_requester))
            return;
    }

    commit_current_pattern();           /* PE_SaveCurrentPattern */
    Save_Progress = save_progress_draw;
    if (Save_ITModule(name)) {
        char *q;
        Save_Progress = NULL;
        snprintf(FileNameDisp, sizeof(FileNameDisp), "%s", name);
        for (q = FileNameDisp; *q; q++)
            if (*q >= 'a' && *q <= 'z')
                *q = (char)(*q - 32);
        status("Saved.");
        *done = 1;
    } else {
        Save_Progress = NULL;
        status("Unable to save file");  /* O1_UnableToSaveList */
    }
}

static void req_activate_file(int *done)
{
    if (ReqNF && FSel < ReqNF) {
        if (ReqSave) {
            snprintf(ReqName, sizeof(ReqName), "%s",
                     ReqFiles[FSel].name);
            req_do_save(done);
        } else if (do_load_named(ReqFiles[FSel].name)) {
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

static void file_requester_run(int save)
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

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }

        switch (key) {
        case ITK_QUIT: Running = 0; done = 1; continue;
        case ITK_ESC:  done = 1; continue;
        case ITK_TAB:       ReqFocus = (ReqFocus + 1) % 4; continue;
        case ITK_SHIFT_TAB: ReqFocus = (ReqFocus + 3) % 4; continue;
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
            }
            continue; }
        default: break;
        }

        if (ReqFocus == 0) {
            switch (key) {
            case ITK_UP:   if (FSel > 0) FSel--; break;
            case ITK_DOWN: if (FSel < ReqNF - 1) FSel++; break;
            case ITK_PGUP: FSel -= 30; if (FSel < 0) FSel = 0; break;
            case ITK_PGDN: FSel += 30; if (FSel >= ReqNF)
                               FSel = ReqNF ? ReqNF - 1 : 0; break;
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
            case ITK_PGDN: DSel += 20; if (DSel >= ReqND)
                               DSel = ReqND ? ReqND - 1 : 0; break;
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
                } else if (do_load_named(ReqName)) {
                    done = 1;
                } else {
                    status("Can't load %s.", ReqName);
                }
            } else if (key >= 32 && key < 127 && len < 25) {
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
    char keep[26];

    memcpy(keep, ReqName, sizeof(keep));
    file_requester_run(1);
    memcpy(ReqName, keep, sizeof(keep));
    ReqSave = 0;
}

/* "Save Current" (Ctrl-S): save to the loaded filename without the
 * requester; the original always goes through D_CheckOverWrite. */
static void quick_save(void)
{
    char name[40];
    FILE *f;

    if (!FileNameDisp[0]) {
        save_requester();
        return;
    }
    snprintf(name, sizeof(name), "%s", FileNameDisp);
    if (!strchr(name, '.') && strlen(name) < sizeof(name) - 4)
        strcat(name, ".IT");
    f = fopen(name, "rb");
    if (f) {
        fclose(f);
        if (!confirm_overwrite(draw_screen))
            return;
    }
    commit_current_pattern();
    if (Save_ITModule(name))
        status("Saved.");
    else
        status("Unable to save file");
}

static int do_load_named(const char *path)
{
    stop_song();
    ed_lock();
    if (Music_LoadIT(path)) {
        const char *base = path, *p;
        char *q;
        Driver->InitSound();
        Music_InitMusic();
        Music_InitStereo();
        Music_InitMixTable();
        Music_InitTempo();
        ed_unlock();
        CurPattern = 0; CurRow = CurChan = CurCol = 0; ListSel = 0;
        load_pattern(0);
        for (p = path; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        snprintf(FileNameDisp, sizeof(FileNameDisp), "%s", base);
        for (q = FileNameDisp; *q; q++)
            if (*q >= 'a' && *q <= 'z')
                *q = (char)(*q - 32);
        return 1;
    }
    ed_unlock();
    return 0;
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
    Song.Header.Flags = ITF_STEREO | ITF_INSTRUMENTS;
    memset(Song.Orders, 255, sizeof(Song.Orders));
    Song.Orders[0] = 0;
    for (i = 0; i < 64; i++) {
        Song.Header.ChnlPan[i] = (i & 1) ? 48 : 16;
        Song.Header.ChnlVol[i] = 64;
    }
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
}

/* ited.cfg: directories + octave/edit step, written by the F12 "Save
 * all Preferences" button */
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
            snprintf(DirModule, sizeof(DirModule), "%s", line + 10);
        else if (!strncmp(line, "sampledir=", 10))
            snprintf(DirSample, sizeof(DirSample), "%s", line + 10);
        else if (!strncmp(line, "instrdir=", 9))
            snprintf(DirInstr, sizeof(DirInstr), "%s", line + 9);
        else if (!strncmp(line, "octave=", 7))
            BaseOctave = atoi(line + 7);
        else if (!strncmp(line, "step=", 5))
            EditStep = atoi(line + 5);
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

        key = Key_Get();
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
static int act_view_patterns(void) { Screen = SCR_PATTERN; return 1; }
static int act_view_orders(void)   { Screen = SCR_ORDER;   return 1; }
static int act_view_vars(void)     { Screen = SCR_VARS;    return 1; }
static int act_help(void)          { Screen = SCR_HELP;    return 1; }
static int act_message_editor(void)
{ Screen = SCR_MESSAGE; return 1; }

static int act_file_load(void)  { file_requester(); return 1; }
static int act_file_new(void)   { new_song(); status("New song."); return 1; }
static int act_file_save(void)     { quick_save(); return 1; }
static int act_file_save_as(void)  { save_requester(); return 1; }
static int act_file_shell(void) { status("No DOS to shell to."); return 1; }
static int act_file_quit(void)  { Running = 0; return 1; }

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
static int act_pb_reinit(void)
{
    ed_lock();
    Driver->InitSound();
    ed_unlock();
    status("Sound driver reinitialised.");
    return 1;
}
static int act_pb_driver(void)
{ status("Driver screen not ported yet."); return 1; }
static int act_pb_length(void)
{ status("Calculate Length not ported yet."); return 1; }

static int act_smp_list(void)
{ Screen = SCR_SAMPLES; ListSel = CurInstr - 1; return 1; }
static int act_smp_lib(void)
{ status("Sample library not ported yet."); return 1; }
static int act_ins_list(void)
{ Screen = SCR_INSTRUMENTS; ListSel = CurInstr - 1; return 1; }
static int act_ins_lib(void)
{ status("Instrument library not ported yet."); return 1; }

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
    if (m.y >= 15 && m.y <= 46 && m.x >= 5 &&
        m.x < 5 + 14 * PE_CHANNELS) {
        int row = TopRow + (m.y - 15);
        int ch  = LeftChan + (m.x - 5) / 14;
        int off = (m.x - 5) % 14;

        if (row < (int)CurRows && ch < 64) {
            CurRow = row;
            CurChan = ch;
            CurCol = (off <= 3) ? 0 : (off <= 6) ? 1 : (off <= 9) ? 2 : 3;
        }
    }
}

static void handle_global(int key)
{
    switch (key) {
    case ITK_QUIT: Running = 0; return;
    case ITK_ESC:
        if (Screen == SCR_MESSAGE && MsgEdit) {
            handle_message_key(ITK_ESC);    /* edit -> view mode */
            return;
        }
        main_menu();
        return;
    case ITK_SHIFT_F9: Screen = SCR_MESSAGE; return;
    case ITK_F1:  Screen = SCR_HELP; return;
    case ITK_F2:  if (Screen != SCR_PATTERN) Screen = SCR_PATTERN; return;
    case ITK_F3:  Screen = SCR_SAMPLES; ListSel = CurInstr-1; return;
    case ITK_F4:  Screen = SCR_INSTRUMENTS; ListSel = CurInstr-1; return;
    case ITK_F11: Screen = SCR_ORDER; ListSel = 0; return;
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
    case ITK_F6:  commit_current_pattern(); play_pattern(); return;
    case ITK_F7:  commit_current_pattern(); play_pattern(); return;
    case ITK_F8:  stop_song(); return;
    case ITK_F9:  file_requester(); return;
    case ITK_F10: save_requester(); return;
    case 0x13:    quick_save(); return;     /* Ctrl-S */
    case ITK_MOUSE:
        if (Screen == SCR_PATTERN)
            pattern_click();
        else
            widgets_mouse();
        return;
    default: break;
    }

    if (Screen == SCR_PATTERN)
        handle_pattern_key(key);
    else if (Screen == SCR_INFO)
        handle_info_key(key);
    else if (Screen == SCR_MESSAGE)
        handle_message_key(key);
    else if (Screen == SCR_SAMPLES) {
        if (!widgets_key(key))          /* Alt ops after the widgets */
            handle_sample_altkey(key);
    } else if (Screen == SCR_INSTRUMENTS) {
        if (!widgets_key(key))          /* note window gets Alt first */
            handle_instrument_altkey(key);
    } else
        widgets_key(key);
}

/* ===================================================================
 * Audio + main loop
 * =================================================================== */
static void audio_cb(ma_device *d, void *out, const void *in, ma_uint32 fr)
{
    (void)d; (void)in;
    WAVDriver_Render((int16_t *)out, fr);
}

static volatile int g_sig = 0;
static void on_sig(int s) { (void)s; g_sig = 1; }

int main(int argc, char **argv)
{
    const char *startmod = NULL;
    uint32_t mixspeed = 44100;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
            mixspeed = (uint32_t)atoi(argv[++i]);
        else if (argv[i][0] != '-')
            startmod = argv[i];
    }

    StartTime = time(NULL);

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

    WAVDriver_SetMixSpeed(mixspeed);
    mixspeed = WAVDriver_GetMixSpeed();
    Driver = &WAVDriver;
    Driver->InitSound();

    if (startmod) {
        if (!Music_LoadIT(startmod)) {
            fprintf(stderr, "failed to load %s\n", startmod);
            return 1;
        }
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
            if (scr >= 0 && scr <= SCR_INFO)
                Screen = scr;
            else if (scr == 9)              /* message editor */
                Screen = SCR_MESSAGE;
            if (getenv("ITED_SHOT_TAB"))   /* F4 tab 0..3 for captures */
                InsTab = (uint8_t)(atoi(getenv("ITED_SHOT_TAB")) & 3);
            if (getenv("ITED_SHOT_SAMPLE"))    /* F3 list selection */
                ListSel = atoi(getenv("ITED_SHOT_SAMPLE")) - 1;
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
            }
            Screen_Clear(0x20);
            if (scr == 7) {                 /* main menu overlay */
                draw_screen();
                menu_draw(&MainMenuDef, 2);
            } else if (scr == 8) {          /* file requester */
                req_scan();
                draw_file_requester();
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
            ITK_RIGHT, '0','5', ITK_RIGHT, '4','0',
            ITK_RIGHT, 'a','0','4',
            ITK_TAB, 'q','w','e','r','t',
            ITK_SHIFT_TAB,
            ']','[','}','{',
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

    /* audio device */
    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = 2;
        cfg.sampleRate = mixspeed;
        cfg.dataCallback = audio_cb;
        if (ma_device_init(NULL, &cfg, &Device) == MA_SUCCESS) {
            ma_device_start(&Device);
            DeviceUp = 1;
        }
    }

    if (!Screen_Init()) {
        fprintf(stderr, "screen init failed\n");
        return 1;
    }
    signal(SIGINT, on_sig);

    while (Running && !g_sig) {
        int key = Key_Get();
        if (key != ITK_NONE) {
            if (key == 0x11 /* Ctrl-Q */) break;
            handle_global(key);
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
