/*
 * it_editor.c
 * -----------
 * Cross-platform terminal front-end reproducing the Impulse Tracker
 * editor's screens and workflow on top of the ported engine. Runs on
 * Windows (cmd/PowerShell/Terminal) and POSIX shells (bash/zsh) via the
 * it_screen layer.
 *
 * Layout follows the IT 2.x conventions: an 80x50 text screen, the
 * F-key screen switches (F1 help, F2 pattern, F3 samples, F4
 * instruments, F5 play, F8 stop, F9 load, F11 order list, F12 song
 * variables), the IT piano keyboard for note entry, octave/edit-step
 * controls, and live playback through the same WAV/hiqual driver as the
 * player. Pattern edits are serialised against the audio thread.
 *
 * The screen *glyphs* are rendered with terminal box characters rather
 * than IT's custom VGA font; everything else (channel grid, note/effect
 * formatting, edit masks, octave/step semantics) matches the original.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

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

/* ---- attributes: ATTR(fg,bg), palette indices = IT Camouflage ----
 * 0 black  1 shadow  2 face  3 highlight  5 yellow  6 green
 * 10 teal  11 near-white  12 grey  14 dk-grey  15 near-black        */
#define ATTR(fg, bg) ((uint8_t)(((fg) & 15) | (((bg) & 15) << 4)))

#define A_DESK     ATTR(3, 0)    /* light text on black desktop   */
#define A_PANEL    ATTR(3, 2)    /* text on tan panel face        */
#define A_PANELHI  ATTR(3, 2)    /* panel highlight edge          */
#define A_PANELLO  ATTR(1, 2)    /* panel shadow edge             */
#define A_TITLE    ATTR(11, 2)   /* bright title on panel         */
#define A_DIM      ATTR(1, 2)    /* dim text on panel             */
#define A_BRIGHT   ATTR(11, 0)
#define A_STATUS   ATTR(5, 1)    /* yellow on dark                */
#define A_SEL      ATTR(0, 3)    /* black on highlight (list sel) */

/* pattern element foregrounds (on the row-tier background) */
#define C_NOTE     3
#define C_EMPTY    14
#define C_INST     10
#define C_VOL      6
#define C_FX       5
#define C_SEP      1
/* row-tier backgrounds */
#define B_NORMAL   0
#define B_BEAT     15
#define B_MEASURE  14
#define B_CURROW   1
#define A_CURSOR   ATTR(0, 11)   /* cursor field: black on near-white */

static int RowHilight1 = 4;      /* beat   (IT default) */
static int RowHilight2 = 16;     /* measure (IT default) */

enum { SCR_HELP, SCR_PATTERN, SCR_SAMPLES, SCR_INSTRUMENTS,
       SCR_ORDER, SCR_VARS };

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
static int       NumChannelsShown = 8;    /* song's used channels (display) */
static int       Running = 1;
static int       Dirty = 1;
static char      StatusMsg[80] = "";

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

/* Bevelled IT-style panel: tan face, light top/left edge, dark
 * bottom/right edge. If `interior` >= 0 the inside is filled with that
 * background (the pattern view uses a black interior). */
static void panel(int x0, int y0, int x1, int y1, int interior,
                  const char *title)
{
    int x, y;
    uint8_t face = A_PANEL, hi = A_PANELHI, lo = A_PANELLO;

    for (y = y0 + 1; y < y1; y++) {
        uint8_t a = (interior >= 0) ? ATTR(C_NOTE, interior) : face;
        fill(x0 + 1, y, x1 - x0 - 1, ' ', a);
    }
    fill(x0, y0, x1 - x0 + 1, 0xC4, hi);     /* top edge (light)   */
    for (y = y0 + 1; y < y1; y++) {
        Screen_PutChar(x0, y, 0xB3, hi);     /* left edge (light)  */
        Screen_PutChar(x1, y, 0xB3, lo);     /* right edge (dark)  */
    }
    fill(x0, y1, x1 - x0 + 1, 0xC4, lo);     /* bottom edge (dark) */
    Screen_PutChar(x0, y0, 0xDA, hi);
    Screen_PutChar(x1, y0, 0xBF, hi);
    Screen_PutChar(x0, y1, 0xC0, hi);
    Screen_PutChar(x1, y1, 0xD9, lo);
    (void)x;
    if (title)
        drawf(x0 + 2, y0, A_TITLE, " %s ", title);
}

static const char *NoteNames[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-",
    "F#", "G-", "G#", "A-", "A#", "B-"
};

/* format a 3-char note string into dst (>=4 bytes) */
static void fmt_note(char *dst, uint8_t gnote)
{
    if (gnote == GNOTE_EMPTY) { memcpy(dst, "...", 4); return; }
    if (gnote == GNOTE_OFF)   { memcpy(dst, "===", 4); return; }
    if (gnote == GNOTE_CUT)   { memcpy(dst, "^^^", 4); return; }
    if (gnote == GNOTE_FADE)  { memcpy(dst, "~~~", 4); return; }
    {
        int n = gnote - 1;      /* 0..119 */
        snprintf(dst, 4, "%s%d", NoteNames[n % 12], n / 12);
    }
}

/* IT volume-column letter codes for the special ranges */
static void fmt_vol(char *dst, uint8_t vol)
{
    if (vol == GVOL_EMPTY) { memcpy(dst, "..", 3); return; }
    if (vol <= 64)        { snprintf(dst, 3, "%02d", vol); return; }
    {
        static const char letters[] = "abcdef"; /* a=finevolup..f=pitchup */
        if (vol >= 65 && vol <= 124) {
            int idx = (vol - 65) / 10;
            int val = (vol - 65) % 10;
            if (idx < 6) { dst[0] = letters[idx]; dst[1] = (char)('0'+val);
                           dst[2] = 0; return; }
        }
        if (vol >= 128 && vol <= 192) { snprintf(dst, 3, "p%d",(vol-128)/7);
                                        return; }
        if (vol >= 193 && vol <= 202) { snprintf(dst, 3, "g%d", vol-193);
                                        return; }
        memcpy(dst, "..", 3);
    }
}

/* ===================================================================
 * Top panels (common to all screens), IT-style bevelled boxes.
 * Rows 0..2 song-name + logo panels, rows 3..5 the transport panel.
 * =================================================================== */
#define TOP_BOTTOM 5      /* last row used by the top panels */

static void draw_topbar(void)
{
    int playing;

    /* desktop title strip */
    drawf(0, 0, A_DESK, " Impulse Tracker 2.17 engine port");
    drawf(60, 0, ATTR(1, 0), "F1 for help / quit");

    /* song-name panel */
    panel(0, 1, 52, 3, -1, NULL);
    drawf(2, 2, A_TITLE, "%-48.48s",
          Song.Header.SongName[0] ? Song.Header.SongName : "(untitled)");

    /* transport / status panel */
    panel(53, 1, 79, 5, -1, NULL);
    ed_lock();
    playing = (PlayMode != 0);
    drawf(55, 2, A_PANEL, "Order  %02X/%02X   Row %02X",
          CurrentOrder, Song.Header.OrdNum ? Song.Header.OrdNum - 1 : 0,
          CurRow);
    drawf(55, 3, A_PANEL, "Pattrn %02X      Spd %02d", CurPattern,
          CurrentSpeed);
    drawf(55, 4, A_PANEL, "Tempo %03d  GV %03d %s", Tempo, GlobalVolume,
          playing ? "\x10" : " ");
    ed_unlock();

    /* edit-state panel (rows 4..6) */
    panel(0, 4, 52, 6, -1, NULL);
    drawf(2, 5, A_PANEL,
          "Octave %d   Step %d   %s %02d   %s slides   %s fx",
          BaseOctave, EditStep,
          (Song.Header.Flags & ITF_INSTRUMENTS) ? "Ins" : "Smp", CurInstr,
          (Song.Header.Flags & ITF_LINEAR_SLIDES) ? "linear" : "amiga",
          (Song.Header.Flags & ITF_OLD_EFFECTS) ? "old" : "IT");

    /* tab strip on the desktop line above the working area */
    {
        static const char *tabs[] = { "F1 Help","F2 Pattern","F3 Samp",
                                      "F4 Instr","F11 Order","F12 Vars" };
        int active[] = { SCR_HELP, SCR_PATTERN, SCR_SAMPLES,
                         SCR_INSTRUMENTS, SCR_ORDER, SCR_VARS };
        int x = 1, i;
        for (i = 0; i < 6; i++) {
            uint8_t a = (Screen == active[i]) ? A_SEL : A_DESK;
            drawf(x, 7, a, " %s ", tabs[i]);
            x += (int)strlen(tabs[i]) + 3;
        }
        drawf(58, 7, A_DESK, "F5 Play  F8 Stop  F9 Load");
    }
}

static void draw_statusline(void)
{
    fill(0, 49, 80, ' ', A_DESK);
    if (StatusMsg[0])
        drawf(1, 49, A_STATUS, " %s ", StatusMsg);
}

/* ===================================================================
 * Pattern editor (F2)
 * =================================================================== */
#define PE_TOP   10         /* first grid data row on screen           */
#define PE_HDR   9          /* channel-header row                       */
#define PE_LEFT  5          /* first grid column (after row-number col) */
#define PE_CHANW 14         /* width of one channel column incl separator */
#define PE_BOTTOM 48        /* last grid data row                       */

static int channels_visible(void)
{
    return (78 - PE_LEFT) / PE_CHANW;   /* ~5 channels */
}

/* row-tier background for a given pattern row */
static int row_bg(int row)
{
    if (RowHilight2 && (row % RowHilight2) == 0) return B_MEASURE;
    if (RowHilight1 && (row % RowHilight1) == 0) return B_BEAT;
    return B_NORMAL;
}

/* draw one element field at (x,y); present? value via cell; uses tier bg */
static void draw_cell(int x, int y, const editcell_t *cell, int bg,
                      int cursorcol)
{
    char ns[4], vs[3];
    int has_n = cell->mask & CM_NOTE, has_i = cell->mask & CM_INS;
    int has_v = cell->mask & CM_VOL,  has_c = cell->mask & CM_CMD;

    fmt_note(ns, has_n ? cell->note : GNOTE_EMPTY);
    fmt_vol(vs, has_v ? cell->vol : GVOL_EMPTY);

    drawf(x,     y, ATTR(has_n ? C_NOTE : C_EMPTY, bg), "%s", ns);
    Screen_PutChar(x + 3, y, ' ', ATTR(C_EMPTY, bg));
    if (has_i) drawf(x + 4, y, ATTR(C_INST, bg), "%02d", cell->ins);
    else       drawf(x + 4, y, ATTR(C_EMPTY, bg), "\xFA\xFA");
    Screen_PutChar(x + 6, y, ' ', ATTR(C_EMPTY, bg));
    drawf(x + 7, y, ATTR(has_v ? C_VOL : C_EMPTY, bg), "%s", vs);
    Screen_PutChar(x + 9, y, ' ', ATTR(C_EMPTY, bg));
    if (has_c) drawf(x + 10, y, ATTR(C_FX, bg), "%c%02X",
                     'A' + cell->cmd - 1, cell->cmdval);
    else       drawf(x + 10, y, ATTR(C_EMPTY, bg), "\xFA\xFA\xFA");

    if (cursorcol >= 0) {
        if (cursorcol == 0)      drawf(x,      y, A_CURSOR, "%s", ns);
        else if (cursorcol == 1) drawf(x + 4,  y, A_CURSOR,
                                       has_i ? "%02d" : "\xFA\xFA", cell->ins);
        else if (cursorcol == 2) drawf(x + 7,  y, A_CURSOR, "%s", vs);
        else if (has_c)          drawf(x + 10, y, A_CURSOR, "%c%02X",
                                       'A' + cell->cmd - 1, cell->cmdval);
        else                     drawf(x + 10, y, A_CURSOR, "\xFA\xFA\xFA");
    }
}

static void draw_pattern(void)
{
    int rowsondisplay = PE_BOTTOM - PE_TOP + 1;
    int visch = channels_visible();
    int screeny, ch;
    int center = rowsondisplay / 2;

    /* scroll so cursor row is centred */
    TopRow = CurRow - center;
    if (TopRow > (int)CurRows - rowsondisplay)
        TopRow = CurRows - rowsondisplay;
    if (TopRow < 0)
        TopRow = 0;

    if (CurChan < LeftChan) LeftChan = CurChan;
    if (CurChan >= LeftChan + visch) LeftChan = CurChan - visch + 1;
    if (LeftChan < 0) LeftChan = 0;

    /* framing panel around the whole pattern view, black interior */
    panel(0, PE_HDR - 1, 79, 49, B_NORMAL, NULL);

    /* channel header bar */
    fill(1, PE_HDR, 78, ' ', ATTR(3, 2));
    drawf(1, PE_HDR, ATTR(1, 2), "Row");
    for (ch = 0; ch < visch; ch++) {
        int c = LeftChan + ch;
        int x = PE_LEFT + ch * PE_CHANW;
        uint8_t a = (c == CurChan) ? ATTR(11, 2) : ATTR(3, 2);
        drawf(x, PE_HDR, a, "  Channel %02d ", c + 1);
    }

    for (screeny = 0; screeny < rowsondisplay; screeny++) {
        int row = TopRow + screeny;
        int y = PE_TOP + screeny;
        int bg, rownumbg;

        if (row >= (int)CurRows) {
            fill(1, y, 78, ' ', ATTR(C_EMPTY, B_NORMAL));
            continue;
        }

        bg = (row == CurRow) ? B_CURROW : row_bg(row);
        rownumbg = bg;
        fill(1, y, 78, ' ', ATTR(C_EMPTY, bg));

        /* row number */
        drawf(1, y, ATTR((row == CurRow) ? 11
                         : (RowHilight2 && row % RowHilight2 == 0) ? 5
                         : (RowHilight1 && row % RowHilight1 == 0) ? 3 : 12,
                         rownumbg), "%3d", row);

        for (ch = 0; ch < visch; ch++) {
            int c = LeftChan + ch;
            int x = PE_LEFT + ch * PE_CHANW;
            const editcell_t *cell = &Grid[row * 64 + c];
            int iscur = (row == CurRow && c == CurChan);

            Screen_PutChar(x - 1, y, 0xB3, ATTR(C_SEP, bg)); /* separator */
            draw_cell(x, y, cell, bg, iscur ? CurCol : -1);
        }
        /* trailing separator */
        Screen_PutChar(PE_LEFT + visch * PE_CHANW - 1, y, 0xB3,
                       ATTR(C_SEP, bg));
    }
}

/* working-area panel covering the screen below the top panels */
static void work_panel(const char *title)
{
    panel(0, PE_HDR - 1, 79, 49, -1, title);
}

/* ===================================================================
 * Sample / Instrument lists (F3 / F4)
 * =================================================================== */
static void draw_list(int instruments)
{
    int n = instruments ? Song.Header.InsNum : Song.Header.SmpNum;
    int rowsondisplay = 49 - PE_HDR - 1;
    int top, i;

    if (n <= 0) n = 1;
    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;

    top = ListSel - rowsondisplay / 2;
    if (top > n - rowsondisplay) top = n - rowsondisplay;
    if (top < 0) top = 0;

    work_panel(instruments ? "Instrument List (piano keys audition)"
                           : "Sample List (piano keys audition)");

    for (i = 0; i < rowsondisplay; i++) {
        int idx = top + i;
        int y = PE_HDR + 1 + i;
        uint8_t a = (idx == ListSel) ? A_SEL : A_PANEL;
        char name[27];

        if (idx < 0 || idx >= n)
            continue;
        if (idx == ListSel)
            fill(1, y, 78, ' ', A_SEL);

        if (instruments) memcpy(name, Song.Ins[idx].InstrumentName, 26);
        else             memcpy(name, Song.Smp[idx].SampleName, 26);
        name[26] = 0;
        { int k; for (k = 0; k < 26; k++)
              if ((unsigned char)name[k] < 32) name[k] = ' '; }

        if (!instruments) {
            sample_t *s = &Song.Smp[idx];
            drawf(2, y, a, "%02X: %-26.26s  %6u smp  %5u Hz  %s%s",
                  idx + 1, name, s->Length, s->C5Speed,
                  (s->Flags & 2) ? "16bit" : "8bit",
                  (s->Flags & 0x10) ? "  loop" : "");
        } else {
            drawf(2, y, a, "%02X: %-26.26s  NNA %d   fade %u",
                  idx + 1, name, Song.Ins[idx].NNA, Song.Ins[idx].FadeOut);
        }
    }
}

/* ===================================================================
 * Order list (F11)
 * =================================================================== */
static void draw_order(void)
{
    int rowsondisplay = 49 - PE_HDR - 1;
    int n = Song.Header.OrdNum, top, i;

    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n ? n - 1 : 0;
    top = ListSel - rowsondisplay / 2;
    if (top > n - rowsondisplay) top = n - rowsondisplay;
    if (top < 0) top = 0;

    work_panel("Order List  (Enter edit  +/- change pattern)");

    for (i = 0; i < rowsondisplay; i++) {
        int idx = top + i;
        int y = PE_HDR + 1 + i;
        uint8_t a = (idx == ListSel) ? A_SEL : A_PANEL;
        if (idx < 0 || idx >= n)
            continue;
        if (idx == ListSel)
            fill(1, y, 78, ' ', A_SEL);
        {
            uint8_t v = Song.Orders[idx];
            if (v == 255)      drawf(2, y, a, "%03d:   --- (end of song)",
                                     idx);
            else if (v == 254) drawf(2, y, a, "%03d:   +++ (skip)", idx);
            else               drawf(2, y, a, "%03d:   Pattern %02X", idx, v);
        }
    }
}

/* ===================================================================
 * Song variables (F12) + Help (F1)
 * =================================================================== */
static void draw_vars(void)
{
    int y = PE_HDR + 2;
    work_panel("Song Variables");
    drawf(4, y++, A_PANEL, "Song name .... %.26s", Song.Header.SongName);
    drawf(4, y++, A_PANEL, "Orders ....... %d", Song.Header.OrdNum);
    drawf(4, y++, A_PANEL, "Patterns ..... %d", Song.Header.PatNum);
    drawf(4, y++, A_PANEL, "Instruments .. %d", Song.Header.InsNum);
    drawf(4, y++, A_PANEL, "Samples ...... %d", Song.Header.SmpNum);
    y++;
    drawf(4, y++, A_PANEL, "Initial speed  %d", Song.Header.IS);
    drawf(4, y++, A_PANEL, "Initial tempo  %d", Song.Header.IT);
    drawf(4, y++, A_PANEL, "Global volume  %d", Song.Header.GV);
    drawf(4, y++, A_PANEL, "Mix volume     %d", Song.Header.MV);
    drawf(4, y++, A_PANEL, "Sep / PWD      %d / %d",
          Song.Header.Sep, Song.Header.PWD);
    y++;
    drawf(4, y++, A_PANEL, "Mode ......... %s, %s, %s effects",
          (Song.Header.Flags & ITF_INSTRUMENTS) ? "instruments":"samples",
          (Song.Header.Flags & ITF_LINEAR_SLIDES)? "linear":"amiga",
          (Song.Header.Flags & ITF_OLD_EFFECTS) ? "old":"IT");
    drawf(4, y++, A_PANEL, "Stereo ....... %s",
          (Song.Header.Flags & ITF_STEREO) ? "yes" : "no");
}

static void draw_help(void)
{
    static const char *lines[] = {
      "Impulse Tracker engine port - editor key reference",
      "",
      " Screens:  F1 help   F2 pattern   F3 samples   F4 instruments",
      "           F11 order list        F12 song variables",
      "",
      " Playback: F5 play song   F6 play current pattern   F8 stop",
      "",
      " Pattern editor:",
      "   Arrows / PgUp / PgDn / Home / End   move cursor",
      "   Tab / Shift-Tab                     next / prev channel",
      "   [ ]                                 octave down / up",
      "   { }  (Shift-[ ])                    edit step down / up",
      "   - = (or PgUp/Dn on order)           previous / next pattern",
      "   Piano keys                          enter note (see below)",
      "   1                                   note cut    (^^^)",
      "   `                                   note off    (===)",
      "   Del                                 clear cell / pull rows up",
      "   Ins                                 push rows down",
      "   In the instrument/volume/effect column, type digits/letters",
      "",
      " Piano (lower octave Z..M = C..B, upper Q..U = C..B):",
      "   Z X C V B N M , L . ; /   = C C# D D# E F F# G G# A A# B ...",
      "   Q 2 W 3 E R 5 T 6 Y 7 U   = C C# D D# E F F# G G# A A# B (+1)",
      "",
      " Samples / Instruments: arrows to select, piano key to audition.",
      " Esc: stop note jam.   Ctrl-C / Q on this screen: quit.",
    };
    int i, n = (int)(sizeof(lines) / sizeof(lines[0]));
    work_panel("Help");
    for (i = 0; i < n; i++)
        drawf(2, PE_HDR + 1 + i, i == 0 ? A_TITLE : A_PANEL, "%s", lines[i]);
}

/* ===================================================================
 * Rendering dispatch
 * =================================================================== */
static void redraw(void)
{
    Screen_Clear(A_DESK);
    draw_topbar();
    switch (Screen) {
    case SCR_PATTERN:     draw_pattern(); break;
    case SCR_SAMPLES:     draw_list(0); break;
    case SCR_INSTRUMENTS: draw_list(1); break;
    case SCR_ORDER:       draw_order(); break;
    case SCR_VARS:        draw_vars(); break;
    case SCR_HELP:        draw_help(); break;
    }
    draw_statusline();
    Screen_Update();
}

/* ===================================================================
 * Note entry: IT piano keyboard -> grid note (or -1)
 * =================================================================== */
static int key_to_note(int key)
{
    /* returns grid note for octave BaseOctave (+offset), or -1 */
    static const struct { char k; int semitone, oct; } map[] = {
        /* lower octave row */
        {'z',0,0},{'s',1,0},{'x',2,0},{'d',3,0},{'c',4,0},{'v',5,0},
        {'g',6,0},{'b',7,0},{'h',8,0},{'n',9,0},{'j',10,0},{'m',11,0},
        {',',12,0},{'l',13,0},{'.',14,0},{';',15,0},{'/',16,0},
        /* upper octave row */
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
            return note + 1;    /* grid encoding */
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
        return;                 /* don't fight the sequencer */
    n[0] = (gnote >= 1 && gnote <= 120) ? (uint8_t)(gnote - 1)
                                        : (uint8_t)gnote;
    n[1] = (uint8_t)CurInstr;
    n[2] = 0xFF;                 /* no volume */
    n[3] = 0; n[4] = 0;
    ed_lock();
    Music_PlayNote((uint16_t)chan, n, 128); /* central pan, full vol */
    ed_unlock();
}

/* ===================================================================
 * Pattern editor key handling
 * =================================================================== */
static void cell_clear(editcell_t *c)
{
    memset(c, 0, sizeof(*c));   /* mask = 0 -> all fields absent */
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
        /* note column */
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
        /* instrument: 2 decimal digits */
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
        /* volume: 2 decimal digits 00..64 */
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
        /* effect: letter then 2 hex */
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
 * List screens key handling
 * =================================================================== */
static void handle_list_key(int key, int instruments)
{
    int n = instruments ? Song.Header.InsNum : Song.Header.SmpNum;
    switch (key) {
    case ITK_UP:   if (ListSel > 0) ListSel--; return;
    case ITK_DOWN: if (ListSel < n - 1) ListSel++; return;
    case ITK_PGUP: ListSel -= 16; if (ListSel < 0) ListSel = 0; return;
    case ITK_PGDN: ListSel += 16; if (ListSel >= n) ListSel = n-1; return;
    case ITK_HOME: ListSel = 0; return;
    case ITK_END:  ListSel = n - 1; return;
    default: break;
    }
    {
        int gn = key_to_note(key);
        if (gn > 0) {
            CurInstr = ListSel + 1;
            /* audition on a high channel to avoid clobbering edit chans */
            jam_note(gn, 40);
        }
    }
}

static void handle_order_key(int key)
{
    int n = Song.Header.OrdNum;
    switch (key) {
    case ITK_UP:   if (ListSel > 0) ListSel--; return;
    case ITK_DOWN: if (ListSel < n - 1) ListSel++; return;
    case '=': case '+':
        ed_lock();
        if (Song.Orders[ListSel] < 199) Song.Orders[ListSel]++;
        ed_unlock(); return;
    case '-':
        ed_lock();
        if (Song.Orders[ListSel] > 0 && Song.Orders[ListSel] < 200)
            Song.Orders[ListSel]--;
        ed_unlock(); return;
    case ITK_ENTER:
        if (Song.Orders[ListSel] < 200) {
            commit_current_pattern();
            load_pattern(Song.Orders[ListSel]);
            Screen = SCR_PATTERN;
        }
        return;
    default: break;
    }
}

/* ===================================================================
 * Minimal one-line file path input (F9 load)
 * =================================================================== */
static int prompt_line(const char *prompt, char *buf, int bufsz)
{
    int len = 0;
    buf[0] = 0;
    for (;;) {
        int k;
        fill(0, 49, 80, ' ', A_SEL);
        drawf(1, 49, A_SEL, "%s%s_", prompt, buf);
        Screen_Update();

        k = Key_Get();
        if (k == ITK_NONE) { ma_sleep(15); continue; }
        if (k == ITK_ENTER) return len > 0;
        if (k == ITK_ESC)   return 0;
        if (k == ITK_BACKSPACE) { if (len > 0) buf[--len] = 0; continue; }
        if (k >= 32 && k < 127 && len < bufsz - 1) {
            buf[len++] = (char)k; buf[len] = 0;
        }
    }
}

static void do_load(void)
{
    char path[256];
    if (!prompt_line("Load module: ", path, sizeof(path))) {
        StatusMsg[0] = 0;
        return;
    }
    stop_song();
    ed_lock();
    if (Music_LoadIT(path)) {
        Driver->InitSound();
        Music_InitMusic();
        Music_InitStereo();
        Music_InitMixTable();
        Music_InitTempo();
        ed_unlock();
        CurPattern = 0; CurRow = CurChan = CurCol = 0; ListSel = 0;
        load_pattern(0);
        snprintf(StatusMsg, sizeof(StatusMsg), "Loaded: %.40s", path);
    } else {
        ed_unlock();
        snprintf(StatusMsg, sizeof(StatusMsg), "Failed to load: %.40s",
                 path);
    }
}

/* ===================================================================
 * Global key dispatch
 * =================================================================== */
static void handle_global(int key)
{
    switch (key) {
    case ITK_F1:  Screen = SCR_HELP; return;
    case ITK_F2:  if (Screen != SCR_PATTERN) Screen = SCR_PATTERN; return;
    case ITK_F3:  Screen = SCR_SAMPLES; ListSel = CurInstr-1; return;
    case ITK_F4:  Screen = SCR_INSTRUMENTS; ListSel = CurInstr-1; return;
    case ITK_F11: Screen = SCR_ORDER; return;
    case ITK_F12: Screen = SCR_VARS; return;
    case ITK_F5:  commit_current_pattern(); play_song(); return;
    case ITK_F6:  commit_current_pattern(); play_pattern(); return;
    case ITK_F7:  commit_current_pattern(); play_pattern(); return;
    case ITK_F8:  stop_song(); return;
    case ITK_F9:  do_load(); return;
    default: break;
    }

    switch (Screen) {
    case SCR_PATTERN:     handle_pattern_key(key); break;
    case SCR_SAMPLES:     handle_list_key(key, 0); break;
    case SCR_INSTRUMENTS: handle_list_key(key, 1); break;
    case SCR_ORDER:       handle_order_key(key); break;
    case SCR_HELP: case SCR_VARS:
        if (key == 'q' || key == 'Q') Running = 0;
        break;
    }
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

    WAVDriver_SetMixSpeed(mixspeed);
    mixspeed = WAVDriver_GetMixSpeed();
    Driver = &WAVDriver;
    Driver->InitSound();

    if (startmod) {
        if (!Music_LoadIT(startmod)) {
            fprintf(stderr, "failed to load %s\n", startmod);
            return 1;
        }
    } else {
        /* empty session: one blank pattern, minimal header */
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

    /* Non-interactive smoke test: exercise every screen and the pattern
     * editing/entry paths without a real terminal or audio device, then
     * exit. Used by the build's regression check. */
    if (getenv("ITED_SELFTEST")) {
        static const int script[] = {
            ITK_F1, ITK_F12, ITK_F11, ITK_DOWN, ITK_DOWN, '+', '-',
            ITK_F3, ITK_DOWN, 'q', ITK_F4, ITK_UP,
            ITK_F2,
            'z','s','x','d','c', ITK_DOWN, '1', ITK_DOWN, '`',
            ITK_RIGHT, '0','5', ITK_RIGHT, '4','0',
            ITK_RIGHT, 'a','0','4',
            ITK_TAB, 'q','w','e','r','t',
            ']','[','}','{',
            ITK_INS, ITK_DEL, ITK_PGDN, ITK_PGUP, ITK_HOME, ITK_END,
            '=', '-',
            ITK_F6, ITK_F8, ITK_F5, ITK_F8,
        };
        size_t k;
        for (k = 0; k < sizeof(script) / sizeof(script[0]); k++) {
            handle_global(script[k]);
            redraw();
        }
        commit_current_pattern();
        if (getenv("ITED_DUMP")) {
            const char *which = getenv("ITED_DUMP");
            FILE *df = fopen("screen_dump.txt", "w");
            Screen = atoi(which); /* 0=help..5=vars; default pattern */
            if (Screen < SCR_HELP || Screen > SCR_VARS) Screen = SCR_PATTERN;
            Screen_Clear(A_DESK);
            draw_topbar();
            switch (Screen) {
            case SCR_PATTERN: draw_pattern(); break;
            case SCR_SAMPLES: draw_list(0); break;
            case SCR_INSTRUMENTS: draw_list(1); break;
            case SCR_ORDER: draw_order(); break;
            case SCR_VARS: draw_vars(); break;
            default: draw_help(); break;
            }
            draw_statusline();
            if (df) { Screen_DumpPlain(df); fclose(df); }
        }
        fprintf(stderr, "ITED selftest: completed %zu actions, "
                "pattern %u, %u rows, cursor r%d c%d col%d\n",
                sizeof(script) / sizeof(script[0]),
                CurPattern, CurRows, CurRow, CurChan, CurCol);
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
    snprintf(StatusMsg, sizeof(StatusMsg),
             "Ready. F1 = help.  %u Hz.  %s",
             mixspeed, DeviceUp ? "audio on" : "no audio device");

    while (Running && !g_sig) {
        int key = Key_Get();
        if (key != ITK_NONE) {
            if (key == 0x11 /* Ctrl-Q */) break;
            handle_global(key);
            Dirty = 1;
        }
        redraw();           /* cheap due to damage tracking */
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
