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
 *    verbatim; F4 and the main menu approximate the originals from
 *    reference screenshots.
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
       SCR_ORDER, SCR_VARS, SCR_INFO, SCR_COUNT };

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

/* PE_ConvAX2Num: 3-digit zero-padded decimal */
static void draw3num(int x, int y, int v, uint8_t attr)
{
    drawf(x, y, attr, "%03d", v > 999 ? 999 : (v < 0 ? 0 : v));
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

static void draw_button(int x0, int y0, int x1, int y1,
                        const char *text, int pressed)
{
    draw_button_style(x0, y0, x1, y1, 8, text, pressed, 0);
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
enum { WT_BUTTON = 1, WT_THUMB, WT_TOGGLE, WT_TEXT, WT_LIST };

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
    void    (*action)(void);       /* press / value-change hook */
    /* thumbbar (value lives in *v8, masked by vmask if nonzero) */
    int barx, bary, min, max, dw;  /* dw = scaled display width, 0 = classic */
    /* string input */
    char *text;
    int   tmax;                    /* max characters excl. terminator */
    /* list (selection/scrolling handled by the screen) */
    int (*lkey)(int key);          /* returns nonzero if consumed */
    void (*lclick)(int row, int mx, int mpx);
    int  listy0;                   /* screen row of the first list line */
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

static int thumb_get(const widget_t *w)
{
    return w->vmask ? (*w->v8 & w->vmask) : *w->v8;
}

static void thumb_set(widget_t *w, int v)
{
    if (v < w->min) v = w->min;
    if (v > w->max) v = w->max;
    ed_lock();
    if (w->vmask)
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
        Screen_DrawString(w->x0, w->y0, toggle_get(w) ? "On " : "Off",
                          focused ? 0x30 : 0x02);
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
        case WT_LIST:
            if (w->lclick)
                w->lclick(m.y - w->listy0, m.x, m.px);
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
static void act_tab_not_ported(void);
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
    Screen_DrawBox(54, 31, 77, 41, 9);          /* Vibrato Waveform */
    Screen_DrawString(58, 33, "Vibrato Waveform", 0x20);
    wradio8(56, 35, 65, 37, "   \271\272", &s->ViT, 3, 0);    /* sine   */
    wradio8(66, 35, 75, 37, "   \275\276", &s->ViT, 3, 1);    /* ramp   */
    wradio8(56, 38, 65, 40, "   \273\274", &s->ViT, 3, 2);    /* square */
    wradio8(66, 38, 75, 40, " Random",     &s->ViT, 3, 3);

    Screen_DrawBox(63, 12, 77, 24, 27);         /* InstParamBox */
    Screen_DrawStringCtl(55, 13, InstParamText, 0x20, NULL);
    drawf(64, 13, 0x03, "%-12.12s", s->DOSFileName);
    drawf(64, 14, 0x03, "%6u", s->C5Speed);
    drawf(64, 15, 0x03, (s->Flags & 0x10) ? "On" : "Off");
    drawf(64, 16, 0x03, "%6u", s->LoopBeg);
    drawf(64, 17, 0x03, "%6u", s->LoopEnd);
    drawf(64, 18, 0x03, (s->Flags & 0x20) ? "On" : "Off");
    drawf(64, 19, 0x03, "%6u", s->SusLoopBeg);
    drawf(64, 20, 0x03, "%6u", s->SusLoopEnd);
    drawf(64, 22, 0x03, "%d bits", (s->Flags & 2) ? 16 : 8);
    drawf(64, 23, 0x03, "%u", s->Length);

    widgets_draw();
}

/* ===================================================================
 * Instrument list (F4) -- approximated from the IT 2.14 screen
 * =================================================================== */
static const char *NNANames[4] = { "Note Cut", "Continue",
                                   "Note Off", "Note Fade" };
static const char *DCTNames[4] = { "Disabled", "Note", "Sample",
                                   "Instrument" };
static const char *DCANames[3] = { "Note Cut", "Note Off", "Note Fade" };

static void draw_instruments(void)
{
    int i, n = Song.Header.InsNum ? Song.Header.InsNum : 1;
    int rows = 35, top;
    instrument_t *ins;

    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;
    top = ListSel - rows / 2;
    if (top > n - rows) top = n - rows;
    if (top < 0) top = 0;
    InsListTop = top;

    Screen_DrawBox(4, 12, 35, 48, 27);
    for (i = 0; i < rows; i++) {
        int idx = top + i;
        uint8_t a = (idx == ListSel) ? 0x30 : 0x06;
        if (idx < 0 || idx >= n)
            continue;
        drawf(5, 13 + i, a, "%02d:", idx + 1);
        draw_itname(8, 13 + i, Song.Ins[idx].InstrumentName, 26, a);
    }

    ins = &Song.Ins[ListSel];

    NW = 0;
    wlist(5, 13, 34, 47, 13, instr_list_lkey, instr_list_lclick);

    draw_button(37, 12, 46, 14, " General", 1);
    wbutton(47, 12, 56, 14, " Volume",  act_tab_not_ported);
    wbutton(57, 12, 67, 14, " Panning", act_tab_not_ported);
    wbutton(68, 12, 76, 14, " Pitch",   act_tab_not_ported);

    Screen_DrawString(53, 17, "New Note Action", 0x20);
    for (i = 0; i < 4; i++)
        wradio8(50, 19 + i*3, 67, 21 + i*3, NNANames[i],
                &ins->NNA, 3, (uint8_t)i);

    Screen_DrawString(46, 32, "Duplicate Check Type & Action", 0x20);
    for (i = 0; i < 4; i++)
        wradio8(40, 34 + i*3, 56, 36 + i*3, DCTNames[i],
                &ins->DCT, 3, (uint8_t)i);
    for (i = 0; i < 3; i++)
        wradio8(58, 34 + i*3, 74, 36 + i*3, DCANames[i],
                &ins->DCA, 3, (uint8_t)i);

    Screen_DrawString(45, 46, "Filename", 0x20);
    Screen_PutChar(54, 46, 132, 0x21);
    drawf(55, 46, 0x05, "%-12.12s", ins->DOSFileName);
    Screen_PutChar(67, 46, 131, 0x23);

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
 * Info page (F5) -- live per-channel view, after IT_DISPL.ASM's
 * Display_HostChannel default ("track") view: a channel-number gutter,
 * a sample/instrument-number + name column, and per-channel panning.
 * Where the original scans the sample data to draw an oscilloscope,
 * this first pass shows a final-volume VU bar (the oscilloscope and the
 * other view modes -- note dots, multi-channel, scopes -- are the
 * documented follow-up). Reads live host/slave channel state under the
 * audio lock.
 * =================================================================== */
static int InfoTop;                 /* first channel row shown */
#define INFO_ROWS 35

/* IT pan labels are 9 chars wide, drawn at the box's left edge. */
static const char *InfoLeftMsg = "Left     ";
static const char *InfoRightMsg = "    Right";
static const char *InfoSurrMsg = "Surround ";

static void draw_info(void)
{
    int stereo   = (Song.Header.Flags & ITF_STEREO) != 0;
    int instmode = (Song.Header.Flags & ITF_INSTRUMENTS) != 0;
    int r;

    if (InfoTop > 64 - INFO_ROWS) InfoTop = 64 - INFO_ROWS;
    if (InfoTop < 0) InfoTop = 0;

    Screen_DrawBox(4, 12, 29, 48, 27);          /* VU / scope box    */
    Screen_DrawBox(30, 12, 62, 48, 27);         /* sample + name box */
    if (stereo)
        Screen_DrawBox(63, 12, 73, 48, 27);     /* panning box       */

    ed_lock();
    for (r = 0; r < INFO_ROWS; r++) {
        int c = InfoTop + r;
        int y = 13 + r;
        int muted, on;
        slavechn_t *sc;
        uint8_t cattr, a;

        if (c >= 64) break;

        muted = (Song.Header.ChnlPan[c] & 0x80) != 0;
        on    = (HChn[c].Flags & HF_CHAN_ON) &&
                HChn[c].SCOffst < MAXSLAVECHANNELS;

        /* channel number gutter, coloured as GetChannelColour for a
         * single window (10h = the dim "other window" colour is unused):
         * current 13h, current+muted 16h, muted 11h, otherwise 12h. */
        if (muted)
            cattr = (c == CurChan) ? 0x16 : 0x11;
        else
            cattr = (c == CurChan) ? 0x13 : 0x12;
        drawf(1, y, cattr, "%02d", c + 1);

        if (!on)
            continue;
        sc = &SChn[HChn[c].SCOffst];
        if (!(sc->Flags & SF_CHAN_ON))
            continue;

        /* sample (+ instrument) number, ":" and the name (box 2).
         * attr 6 normal, 7 when note-off, 4 when silent (FV 0). */
        a = (sc->Flags & SF_NOTE_OFF) ? 0x07 : 0x06;
        if (sc->FV == 0)
            a = 0x04;
        drawf(31, y, 0x06, "%02d", (sc->Smp + 1) % 100);
        if (instmode && sc->Ins != 0xFF) {
            drawf(33, y, 0x06, "/%02d", (sc->Ins + 1) % 100);
            Screen_PutChar(36, y, ':', a);
            if (sc->Ins < MAX_INSTRUMENTS)
                draw_itname(37, y, Song.Ins[sc->Ins].InstrumentName, 25, a);
        } else {
            Screen_PutChar(33, y, ':', a);
            if (sc->Smp < MAX_SAMPLES)
                draw_itname(34, y, Song.Smp[sc->Smp].SampleName, 25, a);
        }

        /* final-volume VU bar (box 1, cols 5..28 = 24 cells) */
        {
            int w = sc->FV * 24 / 128, i;
            for (i = 0; i < w && i < 24; i++)
                Screen_PutChar(5 + i, y, 219, 0x06);
        }

        /* panning (box 3, stereo only): Left/Right/Surround or a thumb */
        if (stereo) {
            uint8_t pan = sc->Pan;
            if (pan == 100)
                Screen_DrawString(64, y, InfoSurrMsg, 0x02);
            else if (pan == 0)
                Screen_DrawString(64, y, InfoLeftMsg, 0x02);
            else if (pan == 64)
                Screen_DrawString(64, y, InfoRightMsg, 0x02);
            else if (pan < 128) {
                int v = pan + 1;
                Screen_PutChar(64 + (v >> 3), y,
                               (uint8_t)(155 + (v & 7)), 0x02);
            }
        }
    }
    ed_unlock();
}

static void handle_info_key(int key)
{
    int maxtop = 64 - INFO_ROWS;
    switch (key) {
    case ITK_UP:   if (InfoTop > 0) InfoTop--; break;
    case ITK_DOWN: if (InfoTop < maxtop) InfoTop++; break;
    case ITK_PGUP: InfoTop -= 16; if (InfoTop < 0) InfoTop = 0; break;
    case ITK_PGDN: InfoTop += 16; if (InfoTop > maxtop) InfoTop = maxtop;
                   break;
    case ITK_HOME: InfoTop = 0; break;
    case ITK_END:  InfoTop = maxtop; break;
    default: break;
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
        "Information (F5)",
    };

    Screen_Clear(0x20);
    draw_chrome(titles[Screen]);
    switch (Screen) {
    case SCR_PATTERN:     NW = 0; draw_pattern(); break;
    case SCR_SAMPLES:     draw_samples(); break;
    case SCR_INSTRUMENTS: draw_instruments(); break;
    case SCR_ORDER:       draw_order(); break;
    case SCR_VARS:        draw_vars(); break;
    case SCR_HELP:        draw_help(); break;
    case SCR_INFO:        NW = 0; draw_info(); break;
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

static void act_tab_not_ported(void)
{
    status("Not ported yet -- see the docs/HANDOFF.md roadmap.");
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

static void draw_file_requester(void)
{
    int i;

    Screen_Clear(0x20);
    draw_chrome("Load Module (F9)");

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

static void req_activate_file(int *done)
{
    if (ReqNF && FSel < ReqNF) {
        if (do_load_named(ReqFiles[FSel].name))
            *done = 1;
        else
            status("Can't load %s.", ReqFiles[FSel].name);
    }
}

static void req_enter_dir(const char *name)
{
    if (!chdir(name))
        req_scan();
    else
        status("Can't change to %s.", name);
}

static void file_requester(void)
{
    int done = 0;

    req_scan();
    ReqFocus = 0;

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
                if (strchr(ReqName, '*') || strchr(ReqName, '?')) {
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
{ status("Message editor not ported yet."); return 1; }

static int act_file_load(void)  { file_requester(); return 1; }
static int act_file_new(void)   { new_song(); status("New song."); return 1; }
static int act_file_save(void)
{ status("Saving is not ported yet (HANDOFF roadmap)."); return 1; }
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
    { " Save As...       (F10)", act_file_save },
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
    case ITK_ESC:  main_menu(); return;
    case ITK_F1:  Screen = SCR_HELP; return;
    case ITK_F2:  if (Screen != SCR_PATTERN) Screen = SCR_PATTERN; return;
    case ITK_F3:  Screen = SCR_SAMPLES; ListSel = CurInstr-1; return;
    case ITK_F4:  Screen = SCR_INSTRUMENTS; ListSel = CurInstr-1; return;
    case ITK_F11: Screen = SCR_ORDER; ListSel = 0; return;
    case ITK_F12: Screen = SCR_VARS; return;
    case ITK_F5:  commit_current_pattern(); play_song();
                  Screen = SCR_INFO; return;   /* play + show info page */
    case ITK_F6:  commit_current_pattern(); play_pattern(); return;
    case ITK_F7:  commit_current_pattern(); play_pattern(); return;
    case ITK_F8:  stop_song(); return;
    case ITK_F9:  file_requester(); return;
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
    else
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
        int gv_wired = -1;
        for (k = 0; k < sizeof(script) / sizeof(script[0]); k++) {
            handle_global(script[k]);
            redraw();
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
                "GV=%u GlobalVolume=%u MV=%u [%s]\n",
                sizeof(script) / sizeof(script[0]),
                CurPattern, CurRows, CurRow, CurChan, CurCol,
                Song.Header.IT, Song.Header.IS, Song.Header.ChnlPan[1],
                Song.Header.GV, GlobalVolume, Song.Header.MV,
                gv_wired ? "GV WIRED OK" : "GV MISMATCH");
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
            if (!m.b || DragIdx >= NW || W[DragIdx].type != WT_THUMB)
                DragIdx = -1;
            else
                thumb_from_px(&W[DragIdx], m.px);
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
