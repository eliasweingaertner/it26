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
static int       Running = 1;
static char      FileNameDisp[20] = "";   /* header File Name field */
static time_t    StartTime;

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

/* F_DrawButtonObject look: raised style-8 box, swapped bevel when
 * pressed/selected (style 9), label drawn from the box's left edge. */
static void draw_button(int x0, int y0, int x1, int y1,
                        const char *text, int pressed)
{
    Screen_DrawBox(x0, y0, x1, y1, pressed ? 9 : 8);
    Screen_DrawString(x0 + 1, (y0 + y1) / 2, text, pressed ? 0x23 : 0x20);
}

/* F_DrawThumbBar, ported 1:1: black groove, 6px thumb built from the
 * fractional-bar glyphs 155..167, 3-digit value in attr 21h after. */
static void draw_thumbbar(int x, int y, int min, int max, int val)
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
    Screen_PutChar(x + cell, y, (uint8_t)(155 + sub), 0x02);
    if (155 + sub > 157)
        Screen_PutChar(x + cell + 1, y, (uint8_t)(155 + sub + 5), 0x02);

    draw3num(x + width + 1, y, val, 0x21);
}

/* scalable thumbbar (F_DrawScalableThumbBar approximation): fixed cell
 * width, value range compressed onto it. */
static void draw_thumbbar_scaled(int x, int y, int min, int max, int val,
                                 int width)
{
    int i, v, cell, sub;

    if (val < min) val = min;
    if (val > max) val = max;

    for (i = 0; i < width; i++)
        Screen_PutChar(x + i, y, 0, 0x03);

    v = (val - min) * (width * 8 - 2) / (max - min) + 1;
    cell = v >> 3;
    sub = v & 7;
    Screen_PutChar(x + cell, y, (uint8_t)(155 + sub), 0x02);
    if (155 + sub > 157)
        Screen_PutChar(x + cell + 1, y, (uint8_t)(155 + sub + 5), 0x02);

    draw3num(x + width + 1, y, val, 0x21);
}

static const char NoteNameChars[] = "C-C#D-D#E-F-F#G-G#A-A#B-";

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

    Screen_DrawBox(36, 12, 53, 18, 9);          /* Default Volume */
    Screen_DrawString(38, 14, "Default Volume", 0x20);
    Screen_DrawBox(37, 15, 47, 17, 9);
    draw_thumbbar(38, 16, 0, 64, s->Vol);

    Screen_DrawBox(36, 19, 53, 25, 9);          /* Global Volume */
    Screen_DrawString(38, 21, "Global Volume", 0x20);
    Screen_DrawBox(37, 22, 47, 24, 9);
    draw_thumbbar(38, 23, 0, 64, s->GvL);

    Screen_DrawBox(36, 26, 53, 33, 9);          /* Default Pan */
    Screen_DrawString(39, 28, "Default Pan", 0x20);
    Screen_DrawBox(37, 29, 47, 32, 25);
    Screen_DrawString(38, 30, (s->DfP & 0x80) ? "On " : "Off", 0x20);
    draw_thumbbar(38, 31, 0, 64, s->DfP & 0x7F);

    Screen_DrawBox(36, 35, 53, 41, 9);          /* Vibrato Speed */
    Screen_DrawString(38, 37, "Vibrato Speed", 0x20);
    Screen_DrawBox(37, 38, 47, 40, 9);
    draw_thumbbar(38, 39, 0, 64, s->ViS);

    Screen_DrawBox(36, 42, 53, 48, 9);          /* Vibrato Depth */
    Screen_DrawString(38, 44, "Vibrato Depth", 0x20);
    Screen_DrawBox(37, 45, 47, 47, 9);
    draw_thumbbar_scaled(38, 46, 0, 32, s->ViD, 8);

    Screen_DrawBox(54, 42, 77, 48, 9);          /* Vibrato Rate */
    Screen_DrawString(60, 44, "Vibrato Rate", 0x20);
    Screen_DrawBox(55, 45, 72, 47, 9);
    draw_thumbbar_scaled(56, 46, 0, 255, s->ViR, 15);

    Screen_DrawBox(54, 25, 77, 30, 9);          /* waveform display */
    Screen_DrawBox(54, 31, 77, 41, 9);          /* Vibrato Waveform */
    Screen_DrawString(58, 33, "Vibrato Waveform", 0x20);
    {
        static const uint8_t sine[]   = { ' ',' ',' ',185,186,0 };
        static const uint8_t ramp[]   = { ' ',' ',' ',189,190,0 };
        static const uint8_t square[] = { ' ',' ',' ',187,188,0 };
        int vt = s->ViT & 3;
        Screen_DrawBox(56, 35, 65, 37, vt == 0 ? 9 : 8);
        Screen_DrawStringCtl(57, 36, sine,   vt == 0 ? 0x23 : 0x20, NULL);
        Screen_DrawBox(66, 35, 75, 37, vt == 1 ? 9 : 8);
        Screen_DrawStringCtl(67, 36, ramp,   vt == 1 ? 0x23 : 0x20, NULL);
        Screen_DrawBox(56, 38, 65, 40, vt == 2 ? 9 : 8);
        Screen_DrawStringCtl(57, 39, square, vt == 2 ? 0x23 : 0x20, NULL);
        Screen_DrawBox(66, 38, 75, 40, vt == 3 ? 9 : 8);
        Screen_DrawString(67, 39, " Random", vt == 3 ? 0x23 : 0x20);
    }

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

    draw_button(37, 12, 46, 14, " General", 1);
    draw_button(47, 12, 56, 14, " Volume", 0);
    draw_button(57, 12, 67, 14, " Panning", 0);
    draw_button(68, 12, 76, 14, " Pitch", 0);

    Screen_DrawString(53, 17, "New Note Action", 0x20);
    for (i = 0; i < 4; i++)
        draw_button(50, 19 + i*3, 67, 21 + i*3, NNANames[i],
                    (ins->NNA & 3) == i);

    Screen_DrawString(46, 32, "Duplicate Check Type & Action", 0x20);
    for (i = 0; i < 4; i++)
        draw_button(40, 34 + i*3, 56, 36 + i*3, DCTNames[i],
                    (ins->DCT & 3) == i);
    for (i = 0; i < 3; i++)
        draw_button(58, 34 + i*3, 74, 36 + i*3, DCANames[i],
                    (ins->DCA & 3) == i && (ins->DCT & 3) != 0);

    Screen_DrawString(45, 46, "Filename", 0x20);
    Screen_PutChar(54, 46, 132, 0x21);
    drawf(55, 46, 0x05, "%-12.12s", ins->DOSFileName);
    Screen_PutChar(67, 46, 131, 0x23);
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

    if (n <= 0) n = 1;
    if (ListSel < 0) ListSel = 0;
    if (ListSel >= n) ListSel = n - 1;

    /* order list, type-12 object at (2,15), 32 entries */
    {
        int top = ListSel - 16;
        if (top > n - 32) top = n - 32;
        if (top < 0) top = 0;

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
        int x = (i < 32) ? 20 : 54;
        int bx = 31;
        int c = i;
        uint8_t pan;

        drawf(20, 15 + i, 0x20, "Channel %02d", i + 1);
        drawf(54, 15 + i, 0x20, "Channel %02d", i + 33);

        for (c = 0; c < 2; c++) {
            int chan = i + c * 32;
            pan = Song.Header.ChnlPan[chan];
            bx = c ? 65 : 31;
            x = bx;
            if (pan & 0x80) {
                Screen_DrawString(x + 1, 15 + i, "Muted", 0x02);
            } else if ((pan & 0x7F) == 100) {
                Screen_DrawString(x, 15 + i, "Surround", 0x02);
            } else {
                int pos = (pan & 0x7F) * 8 / 64;
                Screen_PutChar(x + pos, 15 + i, 254, 0x02);
            }
        }
    }
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
    int f = Song.Header.Flags;

    Screen_DrawString(33, 13, "Song Variables", 0x23);
    Screen_DrawBox(16, 15, 43, 17, 25);             /* SongNameBox */
    Screen_DrawBox(16, 18, 50, 21, 9);              /* InitialSpeedBox */
    Screen_DrawBox(16, 22, 34, 28, 25);             /* VolumeBox */
    Screen_DrawStringCtl(2, 16, SongVarLabels, 0x20, NULL);

    draw_itname(17, 16, Song.Header.SongName, 26, 0x05);

    draw_thumbbar_scaled(17, 19, 31, 255, Song.Header.IT, 28);
    draw_thumbbar_scaled(17, 20, 1, 255, Song.Header.IS, 28);

    draw_thumbbar(17, 23, 0, 128, Song.Header.GV);
    draw_thumbbar(17, 24, 0, 128, Song.Header.MV);
    draw_thumbbar(17, 25, 0, 128, Song.Header.Sep);
    Screen_DrawString(17, 26, (f & ITF_OLD_EFFECTS) ? "On " : "Off", 0x05);
    Screen_DrawString(17, 27, (f & ITF_LINK_G_TO_EF) ? "On " : "Off", 0x05);

    draw_button(16, 29, 30, 31, " Instruments",  (f & ITF_INSTRUMENTS) != 0);
    draw_button(31, 29, 45, 31, " Samples",     !(f & ITF_INSTRUMENTS));
    draw_button(16, 32, 30, 34, " Stereo",       (f & ITF_STEREO) != 0);
    draw_button(31, 32, 45, 34, " Mono",        !(f & ITF_STEREO));
    draw_button(16, 35, 30, 37, " Linear",       (f & ITF_LINEAR_SLIDES) != 0);
    draw_button(31, 35, 45, 37, " Amiga",       !(f & ITF_LINEAR_SLIDES));

    Screen_DrawStringCtl(1, 39, (const uint8_t *)"\377\116\201", 0x21, NULL);
    Screen_DrawString(34, 40, "Directories", 0x23);
    Screen_DrawBox(12, 41, 78, 45, 27);             /* DirectoryInputBox */
    Screen_DrawStringCtl(2, 42, DirLabels, 0x20, NULL);
    {
        char cwd[256] = "";
        if (getcwd(cwd, sizeof(cwd)))
            ;
        drawf(13, 42, 0x05, "%-64.64s", cwd);
        drawf(13, 43, 0x05, "%-64.64s", cwd);
        drawf(13, 44, 0x05, "%-64.64s", cwd);
    }
    draw_button(27, 46, 52, 48, "  Save all Preferences", 0);
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
      "  Playback: F5 play song, F6 play pattern, F8 stop.",
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
    draw_button(36, 46, 44, 48, "  Done", 0);
}

/* ===================================================================
 * Rendering dispatch
 * =================================================================== */
static void redraw(void)
{
    static const char *titles[] = {
        "Help (F1)", "Pattern Editor (F2)", "Sample List (F3)",
        "Instrument List (F4)", "Order List and Panning (F11)",
        "Song Variables & Directory Configuration (F12)",
    };

    Screen_Clear(0x20);
    draw_chrome(titles[Screen]);
    switch (Screen) {
    case SCR_PATTERN:     draw_pattern(); break;
    case SCR_SAMPLES:     draw_samples(); break;
    case SCR_INSTRUMENTS: draw_instruments(); break;
    case SCR_ORDER:       draw_order(); break;
    case SCR_VARS:        draw_vars(); break;
    case SCR_HELP:        draw_help(); break;
    }
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
 * File requester (F9) -- IT_F.ASM "Load Module (F9)" screen layout
 * =================================================================== */
#define REQ_MAXFILES 512
typedef struct reqfile_t {
    char name[64];
    char songname[27];
    long size;
    int  isdir;
} reqfile_t;

static reqfile_t ReqFiles[REQ_MAXFILES];
static int ReqNumFiles, ReqNumDirs;

static int req_name_cmp(const void *a, const void *b)
{
    const reqfile_t *fa = a, *fb = b;
    if (fa->isdir != fb->isdir)
        return fb->isdir - fa->isdir;
    return strcmp(fa->name, fb->name);
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
    ReqNumFiles = ReqNumDirs = 0;

#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA("*", &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                reqfile_t *f;
                if (ReqNumFiles >= REQ_MAXFILES)
                    break;
                if (!strcmp(fd.cFileName, "."))
                    continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    f = &ReqFiles[ReqNumFiles++];
                    snprintf(f->name, sizeof(f->name), "%s", fd.cFileName);
                    f->isdir = 1; f->size = 0; f->songname[0] = 0;
                } else if (has_it_ext(fd.cFileName)) {
                    f = &ReqFiles[ReqNumFiles++];
                    snprintf(f->name, sizeof(f->name), "%s", fd.cFileName);
                    f->isdir = 0;
                    f->size = (long)fd.nFileSizeLow;
                    req_read_songname(f);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR *d = opendir(".");
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) && ReqNumFiles < REQ_MAXFILES) {
                struct stat st;
                if (!strcmp(e->d_name, "."))
                    continue;
                if (stat(e->d_name, &st))
                    continue;
                if (S_ISDIR(st.st_mode)) {
                    reqfile_t *f = &ReqFiles[ReqNumFiles++];
                    snprintf(f->name, sizeof(f->name), "%s", e->d_name);
                    f->isdir = 1; f->size = 0; f->songname[0] = 0;
                } else if (has_it_ext(e->d_name)) {
                    reqfile_t *f = &ReqFiles[ReqNumFiles++];
                    snprintf(f->name, sizeof(f->name), "%s", e->d_name);
                    f->isdir = 0; f->size = (long)st.st_size;
                    req_read_songname(f);
                }
            }
            closedir(d);
        }
    }
#endif

    qsort(ReqFiles, (size_t)ReqNumFiles, sizeof(ReqFiles[0]), req_name_cmp);
    {
        int i;
        ReqNumDirs = 0;
        for (i = 0; i < ReqNumFiles; i++)
            if (ReqFiles[i].isdir)
                ReqNumDirs++;
    }
}

static const uint8_t SearchText[] =
    "Search\015\015\015Format\015  Size\015  Date\015  Time";
static const uint8_t FileText[] = " Filename\015Directory";

static int do_load_named(const char *path);

static void draw_file_requester(int sel, int *ptop)
{
    int i;
    int top = *ptop;
    int nfileonly = ReqNumFiles - ReqNumDirs;

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

        if (sel < top) top = sel;
        if (sel >= top + 31) top = sel - 30;
        if (top < 0) top = 0;

        /* files (left box): name + song name columns */
        {
            int row = 0;
            for (i = 0; i < ReqNumFiles && row < 31; i++) {
                uint8_t a;
                if (ReqFiles[i].isdir)
                    continue;
                if (i < top) { continue; }
                if (ReqFiles[i].isdir == 0 && i >= top && row < 31) {
                    a = (i == sel) ? 0x30 : 0x03;
                    drawf(3, 13 + row, a, "%-13.13s ", ReqFiles[i].name);
                    drawf(17, 13 + row, a, "%-23.23s",
                          ReqFiles[i].songname);
                    row++;
                }
            }
            if (nfileonly == 0)
                Screen_DrawString(3, 13, "(no .it modules here)", 0x03);
        }

        /* directories (middle box) */
        {
            int row = 0;
            for (i = 0; i < ReqNumFiles && row < 21; i++) {
                if (!ReqFiles[i].isdir)
                    continue;
                drawf(44, 13 + row, (i == sel) ? 0x30 : 0x03,
                      "%-12.12s", ReqFiles[i].name);
                row++;
            }
        }

#ifdef _WIN32
        {
            DWORD drives = GetLogicalDrives();
            int row = 0;
            for (i = 0; i < 26 && row < 21; i++)
                if (drives & (1u << i))
                    drawf(59, 13 + row++, 0x05, "Drive %c:", 'A' + i);
        }
#endif

        /* file info */
        if (sel < ReqNumFiles && !ReqFiles[sel].isdir) {
            drawf(58, 40, 0x05, "Impulse Tracker");
            drawf(58, 41, 0x05, "%09ld", ReqFiles[sel].size);
        }

    drawf(13, 46, 0x05, "%-25.25s", "*.IT");
    {
        char cwd[256] = "";
        if (getcwd(cwd, sizeof(cwd)))
            ;
        drawf(13, 47, 0x05, "%-64.64s", cwd);
    }
    *ptop = top;
}

static void file_requester(void)
{
    int sel = 0, top = 0;
    int done = 0;

    req_scan();

    while (!done && Running) {
        int key;

        draw_file_requester(sel, &top);
        Screen_Update();

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }

        switch (key) {
        case ITK_QUIT: Running = 0; done = 1; break;
        case ITK_ESC:  done = 1; break;
        case ITK_UP:   if (sel > 0) sel--; break;
        case ITK_DOWN: if (sel < ReqNumFiles - 1) sel++; break;
        case ITK_PGUP: sel -= 16; if (sel < 0) sel = 0; break;
        case ITK_PGDN: sel += 16; if (sel >= ReqNumFiles)
                           sel = ReqNumFiles ? ReqNumFiles - 1 : 0; break;
        case ITK_ENTER:
            if (sel < ReqNumFiles) {
                if (ReqFiles[sel].isdir) {
                    if (!chdir(ReqFiles[sel].name)) {
                        req_scan();
                        sel = top = 0;
                    }
                } else {
                    if (do_load_named(ReqFiles[sel].name))
                        done = 1;
                }
            }
            break;
        case ITK_BACKSPACE:
            if (!chdir("..")) {
                req_scan();
                sel = top = 0;
            }
            break;
        default: break;
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

/* ===================================================================
 * Main menu (ESC) -- IT 2.14 main menu look
 * =================================================================== */
static const struct { const char *text; int key; } MenuItems[] = {
    { " File Menu...",            0 },
    { " Playback Menu...",        0 },
    { " View Patterns      (F2)", ITK_F2 },
    { " Sample Menu...",          ITK_F3 },
    { " Instrument Menu...",      ITK_F4 },
    { " View Orders/Panning(F11)",ITK_F11 },
    { " View Variables    (F12)", ITK_F12 },
    { " Message Editor(Shift-F9)",0 },
    { " Help!              (F1)", ITK_F1 },
};
enum { MENU_NITEMS = sizeof(MenuItems) / sizeof(MenuItems[0]) };

static void draw_main_menu(int sel)
{
    int i;

    redraw();                           /* current screen behind the menu */
    Screen_DrawBox(22, 11, 58, 12 + MENU_NITEMS*3 + 2, 1);
    Screen_DrawString(25, 12, "Main Menu", 0x23);
    for (i = 0; i < MENU_NITEMS; i++)
        draw_button(24, 13 + i*3, 56, 15 + i*3, MenuItems[i].text, i == sel);
}

static void main_menu(void)
{
    int sel = 2;
    int done = 0;

    while (!done && Running) {
        int key;

        draw_main_menu(sel);
        Screen_Update();

        key = Key_Get();
        if (key == ITK_NONE) { ma_sleep(15); continue; }

        switch (key) {
        case ITK_QUIT: Running = 0; done = 1; break;
        case ITK_ESC:  done = 1; break;
        case ITK_UP:   sel = (sel + MENU_NITEMS - 1) % MENU_NITEMS; break;
        case ITK_DOWN: sel = (sel + 1) % MENU_NITEMS; break;
        case ITK_ENTER:
            if (MenuItems[sel].key) {
                switch (MenuItems[sel].key) {
                case ITK_F1:  Screen = SCR_HELP; break;
                case ITK_F2:  Screen = SCR_PATTERN; break;
                case ITK_F3:  Screen = SCR_SAMPLES; break;
                case ITK_F4:  Screen = SCR_INSTRUMENTS; break;
                case ITK_F11: Screen = SCR_ORDER; break;
                case ITK_F12: Screen = SCR_VARS; break;
                }
                done = 1;
            }
            break;
        default: break;
        }
    }
}

/* ===================================================================
 * Global key dispatch
 * =================================================================== */
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
    case ITK_F5:  commit_current_pattern(); play_song(); return;
    case ITK_F6:  commit_current_pattern(); play_pattern(); return;
    case ITK_F7:  commit_current_pattern(); play_pattern(); return;
    case ITK_F8:  stop_song(); return;
    case ITK_F9:  file_requester(); return;
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

    StartTime = time(NULL);
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

    /* Non-interactive: ITED_DUMP=<screen#> -> plain-ASCII dump of that
     * screen; ITED_SHOT=<file.bmp> -> pixel-exact BMP. No terminal or
     * window needed (the cell buffer + rasterizer are backend-free). */
    {
        const char *dump = getenv("ITED_DUMP");
        const char *shot = getenv("ITED_SHOT");
        if (dump || shot) {
            int scr = dump ? atoi(dump) : (getenv("ITED_SHOT_SCREEN")
                            ? atoi(getenv("ITED_SHOT_SCREEN")) : SCR_PATTERN);
            if (scr >= 0 && scr <= SCR_VARS)
                Screen = scr;
            Screen_Clear(0x20);
            if (scr == 6) {                 /* main menu overlay */
                draw_main_menu(2);
            } else if (scr == 7) {          /* file requester */
                int top = 0;
                req_scan();
                draw_file_requester(0, &top);
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
            ITK_F1, ITK_F12, ITK_F11, ITK_DOWN, ITK_DOWN, '+', '-',
            ITK_F3, ITK_DOWN, ITK_F4, ITK_UP,
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
        for (k = 0; k < sizeof(script) / sizeof(script[0]); k++) {
            handle_global(script[k]);
            redraw();
        }
        commit_current_pattern();
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

    while (Running && !g_sig) {
        int key = Key_Get();
        if (key != ITK_NONE) {
            if (key == 0x11 /* Ctrl-Q */) break;
            handle_global(key);
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
