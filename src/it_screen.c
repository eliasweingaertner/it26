/*
 * it_screen.c
 * -----------
 * 80x50 text-cell framebuffer with the original IT display data
 * (Camouflage palette, custom glyphs, box styles -- it_vgadata.c) and
 * two presentation backends: a VT/ANSI truecolor terminal (here) and a
 * Win32 pixel window (it_screen_win32.c). See it_screen.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "it_screen.h"
#include "it_vgadata.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

static screen_cell_t Back[SCREEN_H][SCREEN_W];
static const screen_backend_t *Backend;
static int Inited = 0;

/* 6-bit VGA DAC -> 8-bit, exactly as a VGA card displays it. */
static uint8_t PalR(int i) { return (uint8_t)((IT_PaletteDefs[i*3+0]*255)/63); }
static uint8_t PalG(int i) { return (uint8_t)((IT_PaletteDefs[i*3+1]*255)/63); }
static uint8_t PalB(int i) { return (uint8_t)((IT_PaletteDefs[i*3+2]*255)/63); }

/* ---- cell-buffer drawing (backend independent) ----------------------- */

void Screen_Clear(uint8_t attr)
{
    int y, x;
    for (y = 0; y < SCREEN_H; y++)
        for (x = 0; x < SCREEN_W; x++) {
            Back[y][x].ch = ' ';
            Back[y][x].attr = attr;
        }
}

void Screen_PutChar(int x, int y, uint8_t ch, uint8_t attr)
{
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) {
        Back[y][x].ch = ch;
        Back[y][x].attr = attr;
    }
}

screen_cell_t Screen_GetCell(int x, int y)
{
    screen_cell_t empty = { 0, 0 };
    if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
        return Back[y][x];
    return empty;
}

void Screen_DrawString(int x, int y, const char *s, uint8_t attr)
{
    while (*s)
        Screen_PutChar(x++, y, (uint8_t)*s++, attr);
}

void Screen_DrawStringCtl(int x, int y, const uint8_t *s, uint8_t attr,
                          const int *nums)
{
    int cx = x;

    while (*s) {
        uint8_t c = *s++;

        if (c == 13) {                  /* next line */
            cx = x;
            y++;
        } else if (c == 0xFF) {         /* repeat: count, char */
            int n = *s++;
            uint8_t rc = *s++;
            while (n-- > 0)
                Screen_PutChar(cx++, y, rc, attr);
        } else if (c == 0xFE) {         /* set attribute */
            attr = *s++;
        } else if (c == 0xFD) {         /* number ('D' decimal supported) */
            char buf[12];
            uint8_t fmt = *s++;
            int v = nums ? *nums++ : 0;
            const char *p;

            snprintf(buf, sizeof(buf), fmt == 'X' ? "%X" : "%d", v);
            for (p = buf; *p; p++)
                Screen_PutChar(cx++, y, (uint8_t)*p, attr);
        } else {
            Screen_PutChar(cx++, y, c, attr);
        }
    }
}

/* S_DrawBox, ported 1:1: 9 (char,attr) pairs per style in the order
 * TL, top, TR, left, fill, right, BL, bottom, BR. */
void Screen_DrawBox(int x0, int y0, int x1, int y1, int style)
{
    const uint8_t *d = IT_BoxDefs[style & 0xFF];
    int nofill = style & IT_BOX_NOFILL;
    int x, y;

    Screen_PutChar(x0, y0, d[0], d[1]);
    for (x = x0 + 1; x < x1; x++)
        Screen_PutChar(x, y0, d[2], d[3]);
    Screen_PutChar(x1, y0, d[4], d[5]);

    for (y = y0 + 1; y < y1; y++) {
        Screen_PutChar(x0, y, d[6], d[7]);
        if (!nofill)
            for (x = x0 + 1; x < x1; x++)
                Screen_PutChar(x, y, d[8], d[9]);
        Screen_PutChar(x1, y, d[10], d[11]);
    }

    Screen_PutChar(x0, y1, d[12], d[13]);
    for (x = x0 + 1; x < x1; x++)
        Screen_PutChar(x, y1, d[14], d[15]);
    Screen_PutChar(x1, y1, d[16], d[17]);
}

/* ---- font bank B (S_GenerateCharacters, the VGA 512-char trick) ------ */

static uint8_t FontB[256][8];

void Screen_GenerateCharacters(int first, int wchars, int hchars,
                               const uint8_t *pix)
{
    int cy, cx, row, b;
    int stride = wchars * 8;

    for (cy = 0; cy < hchars; cy++) {
        for (cx = 0; cx < wchars; cx++) {
            int ch = first + cy * wchars + cx;
            if (ch < 0 || ch > 255)
                continue;
            for (row = 0; row < 8; row++) {
                const uint8_t *src = pix + (cy*8 + row)*stride + cx*8;
                uint8_t bits = 0;
                for (b = 0; b < 8; b++)
                    bits = (uint8_t)((bits << 1) | (src[b] & 1));
                FontB[ch][row] = bits;
            }
        }
    }
}

/* DrawHilightBar-style read-modify-write: Or [ES:DI], bits on the
 * attribute byte (IT_DISPL.ASM hilights the playing row this way). */
void Screen_OrAttr(int x, int y, uint8_t bits)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        Back[y][x].attr |= bits;
}

uint8_t Screen_GetAttr(int x, int y)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        return Back[y][x].attr;
    return 0;
}

void Screen_SetAttr(int x, int y, uint8_t attr)
{
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
        Back[y][x].attr = attr;
}

/* S_DefineSmallNumbers (IT_S.ASM): build the info page's small-number
 * charsets from HexNumeralDefinitions.  Font bank B char 0xXY shows hex
 * digits X and Y in its left and right 4 pixels; font A chars 226..245
 * become G0..G9,H0..H9 (letter left, digit right).  The font A override
 * is permanent once defined, exactly as in the original (nothing ever
 * restores 226..245). */

static uint8_t FontASmall[20][8];
static int SmallNumbersOn;

/* S_DefineHiASCII (IT_S.ASM 1589; feature 013): load font bank B with
 * the plain ROM font so attr-bit-3 text (the message editor's colour
 * 12) renders real CP437 high-ASCII. Called on message-editor entry
 * (Glbl_Shift_F9); bank B is reclaimed by the next
 * GenerateCharacters/DefineSmallNumbers, as in the original. */
void Screen_DefineHiASCII(void)
{
    memcpy(FontB, IT_FontROM, sizeof(FontB));
}

void Screen_DefineSmallNumbers(void)
{
    int c, row, l, d;

    for (c = 0; c < 256; c++)
        for (row = 0; row < 8; row++)
            FontB[c][row] =
                (uint8_t)((IT_HexNumerals[(c >> 4)*8 + row] << 4)
                          | IT_HexNumerals[(c & 15)*8 + row]);

    for (l = 0; l < 2; l++)
        for (d = 0; d < 10; d++)
            for (row = 0; row < 8; row++)
                FontASmall[l*10 + d][row] =
                    (uint8_t)((IT_HexNumerals[(16 + l)*8 + row] << 4)
                              | IT_HexNumerals[d*8 + row]);
    SmallNumbersOn = 1;
}

/* ---- rasterizer: cells -> 640x400 RGB (the authentic output) --------- */

static uint8_t FontAInvert[8];          /* font-A char 246 (S_InvertCursor) */
static int InvertOn;

static const uint8_t *GlyphBitmap(uint8_t ch)
{
    if (InvertOn && ch == 246)
        return FontAInvert;
    if (SmallNumbersOn && ch >= 226 && ch <= 245)
        return FontASmall[ch - 226];
    if (ch >= IT_CHARDEF_FIRST && ch < IT_CHARDEF_FIRST + IT_CHARDEF_COUNT)
        return IT_CharDefs[ch - IT_CHARDEF_FIRST];
    return IT_FontROM[ch];
}

/* S_InvertCursor (IT_S.ASM 1633): rebuild font-A char 246 as the glyph
 * currently shown at (x,y) with the pixel columns selected by `mask`
 * inverted, then display char 246 attr 30h there. Used by the pattern
 * editor's cursor on packed (font-B / G0..H9 / char-184) cells: the
 * unmasked half renders in cursor colours, the masked half keeps its
 * normal look. */
void Screen_InvertCursor(int x, int y, uint8_t mask)
{
    screen_cell_t c = Screen_GetCell(x, y);
    const uint8_t *g = (c.attr & 0x08) ? FontB[c.ch] : GlyphBitmap(c.ch);
    int row;

    for (row = 0; row < 8; row++)
        FontAInvert[row] = (uint8_t)(g[row] ^ mask);
    InvertOn = 1;
    Screen_PutChar(x, y, 246, 0x30);
}

/* full-resolution 8-bit overlay (feature 013: the Alt-F12 spectrum
 * analyser replaces the text screen with a VESA graphics mode in the
 * original; the port presents a 640x400 palettized framebuffer through
 * the same rasterizer instead). NULL pixels = overlay off. */
static const uint8_t *OverlayPix;
static uint32_t OverlayPal[256];

void Screen_SetOverlay(const uint8_t *pix, const uint8_t *pal6)
{
    OverlayPix = pix;
    if (pal6) {
        int i;
        for (i = 0; i < 256; i++)
            OverlayPal[i] =
                ((uint32_t)(pal6[i * 3]     << 2) << 16) |
                ((uint32_t)(pal6[i * 3 + 1] << 2) << 8)  |
                 (uint32_t)(pal6[i * 3 + 2] << 2);
    }
}

/* "2026 AI port" corner ribbon (port marking, not in the original):
 * a diagonal strip across the top-right corner of the pixel output
 * only, with the text at half the cell font size -- hand-made 3x5
 * glyphs in the ROM font's style (the ROM font itself is unreadable
 * below ~6px), drawn upright and stepping down the 45-degree
 * baseline.  Cell contents (ITED_DUMP hashes) and the terminal
 * backend are untouched; ITED_NOBANNER=1 hides it for
 * reference-screenshot comparisons. */
static void RasterizeBanner(uint32_t *px)
{
    /* "2026 AI port"; bits 0x80/0x40/0x20 = glyph columns, col 3 =
     * inter-character spacing (always clear) */
    static const uint8_t tiny[][5] = {
        {0xE0,0x20,0xE0,0x80,0xE0},  /* 2 */
        {0xE0,0xA0,0xA0,0xA0,0xE0},  /* 0 */
        {0xE0,0x80,0xE0,0xA0,0xE0},  /* 6 */
        {0x00,0x00,0x00,0x00,0x00},  /* space */
        {0xE0,0xA0,0xE0,0xA0,0xA0},  /* A */
        {0xE0,0x40,0x40,0x40,0xE0},  /* I */
        {0x00,0xC0,0xA0,0xC0,0x80},  /* p */
        {0x00,0xE0,0xA0,0xA0,0xE0},  /* o */
        {0x00,0xE0,0x80,0x80,0x80},  /* r */
        {0x40,0xE0,0x40,0x40,0x60},  /* t */
    };
    static const uint8_t map[12] = {0,1,0,2,3,4,5,3,6,7,8,9};
    static int state;                    /* 0 unknown, 1 on, 2 off */
    int x, y, i, r, c;

    if (!state)
        state = getenv("ITED_NOBANNER") ? 2 : 1;
    if (state == 2)
        return;

    /* the band: diagonal strip x-y in [578,590], darker edge lines */
    for (y = 0; y <= 61; y++) {
        int xhi = 590 + y < 639 ? 590 + y : 639;
        for (x = 578 + y; x <= xhi; x++) {
            int v = x - y;
            px[y*640 + x] = (v == 578 || v == 590) ? 0x004000 : 0x007000;
        }
    }

    /* the text: upright glyphs stepping down the 45-degree baseline
     * (rotated strokes alias into dots at this size) */
    for (i = 0; i < 12; i++)
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (tiny[map[i]][r] & (0x80 >> c))
                    px[(4 + i*4 + r)*640 + 589 + i*4 + c] = 0xFFFFFF;
}

void Screen_Rasterize(uint32_t *px)
{
    int cy, cx, row;

    if (OverlayPix) {
        int i;
        for (i = 0; i < 640 * 400; i++)
            px[i] = OverlayPal[OverlayPix[i]];
        return;
    }

    for (cy = 0; cy < SCREEN_H; cy++) {
        for (cx = 0; cx < SCREEN_W; cx++) {
            screen_cell_t c = Back[cy][cx];
            const uint8_t *g = (c.attr & 0x08) ? FontB[c.ch]
                                               : GlyphBitmap(c.ch);
            int fi = c.attr & 15, bi = (c.attr >> 4) & 15;
            uint32_t fg = ((uint32_t)PalR(fi) << 16) |
                          ((uint32_t)PalG(fi) << 8) | PalB(fi);
            uint32_t bg = ((uint32_t)PalR(bi) << 16) |
                          ((uint32_t)PalG(bi) << 8) | PalB(bi);

            for (row = 0; row < 8; row++) {
                uint32_t *dst = px + (cy*8 + row)*640 + cx*8;
                uint8_t bits = g[row];
                int b;
                for (b = 0; b < 8; b++)
                    dst[b] = (bits & (0x80 >> b)) ? fg : bg;
            }
        }
    }

    RasterizeBanner(px);
}

int Screen_WriteBMP(const char *path)
{
    static uint32_t px[640 * 400];
    uint8_t hdr[54];
    uint32_t imgsize = 640 * 400 * 3;
    FILE *fp = fopen(path, "wb");
    int y, x;

    if (!fp)
        return 0;

    Screen_Rasterize(px);

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2)  = 54 + imgsize;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = 640;
    *(int32_t  *)(hdr + 22) = 400;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = imgsize;
    fwrite(hdr, 1, 54, fp);

    for (y = 399; y >= 0; y--) {        /* BMP is bottom-up */
        uint8_t line[640 * 3];
        for (x = 0; x < 640; x++) {
            uint32_t p = px[y*640 + x];
            line[x*3 + 0] = (uint8_t)(p & 0xFF);
            line[x*3 + 1] = (uint8_t)((p >> 8) & 0xFF);
            line[x*3 + 2] = (uint8_t)((p >> 16) & 0xFF);
        }
        fwrite(line, 1, sizeof(line), fp);
    }
    fclose(fp);
    return 1;
}

/* ---- plain-ASCII dump (self-test / layout iteration) ----------------- */

static char PlainChar(uint8_t c)
{
    if (c >= 0x20 && c < 0x7F)
        return (char)c;
    switch (c) {
    case 128: case 130: case 133: case 135:
    case 142: case 144: case 147: case 149: return '+';
    case 129: case 134: case 143: case 148: case 154: return '-';
    case 131: case 132: case 145: case 146: case 168: return '|';
    case 136: case 137: case 138: case 139:
    case 150: case 151: case 152: case 153: return '.';
    case 140: case 141: return '\\';
    case 173: return '.';
    case 0xB3: return '|'; case 0xC4: return '-';
    case 0xDA: case 0xBF: case 0xC0: case 0xD9: return '+';
    case 205: return '=';
    default:  return (c >= 155 && c <= 167) ? '#' : '.';
    }
}

void Screen_DumpPlain(void *vfp)
{
    FILE *fp = (FILE *)vfp;
    int y, x;

    for (y = 0; y < SCREEN_H; y++) {
        char line[SCREEN_W + 1];
        for (x = 0; x < SCREEN_W; x++)
            line[x] = PlainChar(Back[y][x].ch);
        line[SCREEN_W] = 0;
        fprintf(fp, "%s\n", line);
    }
}

/* ====================================================================
 * VT/ANSI truecolor terminal backend (fallback; primary on POSIX)
 *
 * Input (feature 011, roadmap #7): the POSIX side decodes the xterm
 * encodings -- ESC-prefix Alt keys, modified-CSI combos, modifyOther-
 * Keys/CSI-u for Ctrl-digits, and SGR mouse reporting -- through the
 * incremental parser below; the Windows console side decodes conio
 * scan codes (no mouse there; the Win32 window is the primary
 * backend). Remaining limitations: no ITK_SHIFT_PRESS/RELEASE (a tty
 * has no key-up events, so F2 chord entry stays pixel-only) and no
 * keypad-`/` distinction (terminals send plain '/').
 * ==================================================================== */

static screen_cell_t TermFront[SCREEN_H][SCREEN_W];

static int Term_Init(void)
{
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
    printf("\x1b[?1049h\x1b[?25l");     /* alt screen, hide cursor */
#ifndef _WIN32
    /* feature 011: SGR mouse (button-event tracking) + modifyOtherKeys
     * level 1 for the otherwise-unencodable Ctrl-digit combos.
     * Terminals without support ignore these. */
    printf("\x1b[?1002h\x1b[?1006h\x1b[>4;1m");
#endif
    fflush(stdout);
    memset(TermFront, 0xFF, sizeof(TermFront));
    return 1;
}

static void Term_UnInit(void)
{
#ifndef _WIN32
    printf("\x1b[>4;0m\x1b[?1006l\x1b[?1002l");
#endif
    printf("\x1b[0m\x1b[?25h\x1b[?1049l");
    fflush(stdout);
#ifndef _WIN32
    {
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        t.c_lflag |= ICANON | ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
#endif
}

/* CP437/custom glyph -> UTF-8 approximation for the terminal fallback.
 * The custom IT glyphs (128..201) are bevel lines, slider bars and
 * icons; they map onto nearby Unicode box/block drawing characters. */
static const char *TermGlyph(uint8_t c)
{
    static char buf[2];

    switch (c) {
    /* thin bevel set (1px lines) */
    case 128: return "\xE2\x94\x8C";            /* ┌ */
    case 129: return "\xE2\x94\x80";            /* ─ */
    case 130: return "\xE2\x94\x90";            /* ┐ */
    case 131: return "\xE2\x94\x82";            /* │ */
    case 132: return "\xE2\x94\x82";
    case 133: return "\xE2\x94\x94";            /* └ */
    case 134: return "\xE2\x94\x80";
    case 135: return "\xE2\x94\x98";            /* ┘ */
    case 136: return "\xE2\x96\x98";            /* ▘ */
    case 137: return "\xE2\x96\x9D";            /* ▝ */
    case 138: return "\xE2\x96\x96";            /* ▖ */
    case 139: return "\xE2\x96\x97";            /* ▗ */
    case 140: return "\xE2\x97\xA3";            /* ◣ */
    case 141: return "\xE2\x97\xA5";            /* ◥ */
    /* thick bevel set (2px lines) */
    case 142: return "\xE2\x94\x8F";            /* ┏ */
    case 143: return "\xE2\x94\x81";            /* ━ */
    case 144: return "\xE2\x94\x93";            /* ┓ */
    case 145: return "\xE2\x94\x83";            /* ┃ */
    case 146: return "\xE2\x94\x83";
    case 147: return "\xE2\x94\x97";            /* ┗ */
    case 148: return "\xE2\x94\x81";
    case 149: return "\xE2\x94\x9B";            /* ┛ */
    case 150: case 151: case 152: case 153: return "\xC2\xB7";
    case 154: return "\xE2\x95\x8C";            /* ╌ */
    case 168: return "\xE2\x95\x91";            /* ║ track divider */
    case 169: case 170: case 171: return "\xE2\x96\xB4"; /* ▴ markers */
    case 172: return "M";
    case 173: return "\xC2\xB7";                /* · empty field dot */
    case 183: return "\xE2\x97\x86";            /* ◆ */
    case 185: case 186: return "~";             /* sine icon */
    case 187: case 188: return "\xE2\x8E\x8D";  /* ⎍ square icon */
    case 189: case 190: return "\\";            /* ramp icon */
    case 191: return "[";
    case 192: return "]";
    case 0xB3: return "\xE2\x94\x82";
    case 0xD9: return "\xE2\x94\x98";
    case 0xDB: return "\xE2\x96\x88";
    case 0xDC: return "\xE2\x96\x84";
    case 0xDF: return "\xE2\x96\x80";
    case 205:  return "\xE2\x95\x90";           /* ═ note off */
    default:
        if (c >= 155 && c <= 167)
            return "\xE2\x96\x8A";              /* ▊ slider thumb */
        if (c >= 193 && c <= 201)
            return "\xE2\x80\xA2";              /* • note dots */
        if (SmallNumbersOn && c >= 226 && c <= 245) {
            buf[0] = (char)('0' + (c - 226) % 10);  /* G0..H9: show digit */
            buf[1] = 0;
            return buf;
        }
        buf[0] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        buf[1] = 0;
        return buf;
    }
}

/* font bank B cell -> Unicode quadrant block (terminal approximation of
 * the generated canvas glyphs; index = UL|UR<<1|LL<<2|LR<<3) */
static const char *TermGlyphB(uint8_t ch)
{
    static const char *quad[16] = {
        " ",            "\xE2\x96\x98", "\xE2\x96\x9D", "\xE2\x96\x80",
        "\xE2\x96\x96", "\xE2\x96\x8C", "\xE2\x96\x9E", "\xE2\x96\x9B",
        "\xE2\x96\x97", "\xE2\x96\x9A", "\xE2\x96\x90", "\xE2\x96\x9C",
        "\xE2\x96\x84", "\xE2\x96\x99", "\xE2\x96\x9F", "\xE2\x96\x88",
    };
    const uint8_t *g = FontB[ch];
    int q = 0;

    if ((g[0] | g[1] | g[2] | g[3]) & 0xF0) q |= 1;
    if ((g[0] | g[1] | g[2] | g[3]) & 0x0F) q |= 2;
    if ((g[4] | g[5] | g[6] | g[7]) & 0xF0) q |= 4;
    if ((g[4] | g[5] | g[6] | g[7]) & 0x0F) q |= 8;
    return quad[q];
}

static void Term_Present(const screen_cell_t *cells)
{
    char out[SCREEN_W * 48 + 64];
    int y, x;

    for (y = 0; y < SCREEN_H; y++) {
        const screen_cell_t *row = cells + y * SCREEN_W;
        size_t n = 0;
        int lastattr = -1;

        if (!memcmp(TermFront[y], row, sizeof(TermFront[y])))
            continue;

        n += (size_t)snprintf(out + n, sizeof(out) - n, "\x1b[%d;1H", y + 1);
        for (x = 0; x < SCREEN_W && n < sizeof(out) - 48; x++) {
            screen_cell_t c = row[x];

            if (c.attr != lastattr) {
                int fi = c.attr & 15, bi = (c.attr >> 4) & 15;
                n += (size_t)snprintf(out + n, sizeof(out) - n,
                                      "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm",
                                      PalR(fi), PalG(fi), PalB(fi),
                                      PalR(bi), PalG(bi), PalB(bi));
                lastattr = c.attr;
            }
            n += (size_t)snprintf(out + n, sizeof(out) - n, "%s",
                                  (c.attr & 0x08) ? TermGlyphB(c.ch)
                                                  : TermGlyph(c.ch));
        }
        fwrite(out, 1, n, stdout);
        memcpy(TermFront[y], row, sizeof(TermFront[y]));
    }
    fflush(stdout);
}

/* ====================================================================
 * Terminal input parser (feature 011): an incremental byte state
 * machine shared by the key and mouse polls, decoding the xterm
 * families -- Alt prefix (ESC x), modified CSI (CSI 1;m X / CSI n;m ~ /
 * CSI 1;m P..S), modifyOtherKeys (CSI 27;m;c~) + CSI-u (CSI c;m u),
 * and SGR mouse (CSI < b;x;y M/m) -- into the ITK_* codes + an
 * it_mouse_t mirror. Platform-neutral and compiled everywhere so the
 * selftest can drive it headless (Screen_TermFeedTest); only the read
 * pump in Term_Key/Term_Mouse is POSIX.
 * ==================================================================== */

static int TermKeyQueue[64];
static int TermKeyHead, TermKeyTail;
static int TermMousePX = 320, TermMousePY = 200, TermMouseB;

enum { TP_GROUND, TP_ESC, TP_CSI, TP_SS3 };
static int TP_State;
static int TP_Param[4], TP_NParam;      /* CSI numeric parameters */
static int TP_Priv;                     /* CSI private marker (< > ?) */
static int TP_EscPending;               /* lone ESC seen at pump end */

static void Term_PushKey(int k)
{
    int next = (TermKeyTail + 1) % 64;
    if (next != TermKeyHead) {
        TermKeyQueue[TermKeyTail] = k;
        TermKeyTail = next;
    }
}

static int Term_PopKey(void)
{
    int k;
    if (TermKeyHead == TermKeyTail)
        return ITK_NONE;
    k = TermKeyQueue[TermKeyHead];
    TermKeyHead = (TermKeyHead + 1) % 64;
    return k;
}

/* ESC x -> Alt-x */
static void Term_AltByte(uint8_t c)
{
    if (c >= 'a' && c <= 'z')      Term_PushKey(ITK_ALT_A + (c - 'a'));
    else if (c >= 'A' && c <= 'Z') Term_PushKey(ITK_ALT_A + (c - 'A'));
    else if (c >= '0' && c <= '9') Term_PushKey(ITK_ALT_0 + (c - '0'));
    else if (c == '\\')            Term_PushKey(ITK_ALT_BACKSLASH);
    else if (c == '+' || c == '=') Term_PushKey(ITK_ALT_PLUS);
    else if (c == '-')             Term_PushKey(ITK_ALT_MINUS);
    /* unmapped Alt combos are consumed silently */
}

/* modifyOtherKeys / CSI-u payload: unshifted (or shifted) codepoint +
 * xterm modifier value (1 + 1 shift / 2 alt / 4 ctrl) */
static void Term_ModOther(int code, int mod)
{
    int bits = (mod > 0) ? mod - 1 : 0;

    if (bits == 4) {                            /* Ctrl */
        if (code >= '0' && code <= '5')
            Term_PushKey(ITK_CTRL_0 + (code - '0'));
        else if (code == '+' || code == '=')
            Term_PushKey(ITK_CTRL_PLUS);
        else if (code == '-')
            Term_PushKey(ITK_CTRL_MINUS);
        else if (code == 8 || code == 127)
            Term_PushKey(ITK_CTRL_BACKSPACE);
        else if (code >= 'a' && code <= 'z')    /* some terminals CSI-u
                                                   encode Ctrl-letters */
            Term_PushKey(code - 'a' + 1);
    } else if (bits == 5) {                     /* Ctrl+Shift */
        if (code >= '1' && code <= '4')
            Term_PushKey(ITK_CTRL_SHIFT_1 + (code - '1'));
        else if (code == '!') Term_PushKey(ITK_CTRL_SHIFT_1);
        else if (code == '@') Term_PushKey(ITK_CTRL_SHIFT_1 + 1);
        else if (code == '#') Term_PushKey(ITK_CTRL_SHIFT_1 + 2);
        else if (code == '$') Term_PushKey(ITK_CTRL_SHIFT_1 + 3);
    } else if (bits == 2) {                     /* Alt via CSI-u */
        if (code < 256)
            Term_AltByte((uint8_t)code);
    }
    /* other combinations: consumed */
}

/* CSI < b;x;y M/m -- SGR mouse. Left button only (pixel-backend
 * parity); px/py approximate to the cell centre. */
static void Term_MouseReport(int b, int x, int y, int press)
{
    int motion = b & 32;

    if (b & 64)                                 /* wheel: consumed */
        return;
    if (x < 1) x = 1; if (x > SCREEN_W) x = SCREEN_W;
    if (y < 1) y = 1; if (y > SCREEN_H) y = SCREEN_H;
    TermMousePX = (x - 1) * 8 + 4;
    TermMousePY = (y - 1) * 8 + 4;
    if ((b & 3) != 0)                           /* not the left button */
        return;
    if (!press)
        TermMouseB = 0;
    else if (!motion) {
        TermMouseB = 1;
        Term_PushKey(ITK_MOUSE);
    }
    /* motion with button held: position update only, b stays */
}

/* CSI final byte: dispatch on collected parameters */
static void Term_CsiFinal(uint8_t f)
{
    int p0  = TP_NParam > 0 ? TP_Param[0] : 0;
    int mod = TP_NParam > 1 ? TP_Param[1] : 1;

    if (TP_Priv == '<') {                       /* SGR mouse */
        if (f == 'M' || f == 'm')
            Term_MouseReport(TP_Param[0],
                             TP_NParam > 1 ? TP_Param[1] : 0,
                             TP_NParam > 2 ? TP_Param[2] : 0,
                             f == 'M');
        return;
    }
    if (TP_Priv)                                /* ? / > replies etc. */
        return;

    switch (f) {
    case 'A': case 'B': case 'C': case 'D': {   /* arrows */
        static const int plain[4] = { ITK_UP, ITK_DOWN, ITK_RIGHT,
                                      ITK_LEFT };
        static const int shift[4] = { ITK_SHIFT_UP, ITK_SHIFT_DOWN,
                                      ITK_SHIFT_RIGHT, ITK_SHIFT_LEFT };
        static const int ctrl[4]  = { ITK_CTRL_UP, ITK_CTRL_DOWN,
                                      ITK_CTRL_RIGHT, ITK_CTRL_LEFT };
        int i = (f == 'A') ? 0 : (f == 'B') ? 1 : (f == 'C') ? 2 : 3;
        if (mod == 2)      Term_PushKey(shift[i]);
        else if (mod == 5) Term_PushKey(ctrl[i]);
        else if (mod == 3) {
            if (f == 'A') Term_PushKey(ITK_ALT_UP);
            if (f == 'B') Term_PushKey(ITK_ALT_DOWN);
        } else if (mod <= 1)
            Term_PushKey(plain[i]);
        return;
    }
    case 'H':
        Term_PushKey(mod == 2 ? ITK_SHIFT_HOME :
                     mod == 5 ? ITK_CTRL_HOME : ITK_HOME);
        return;
    case 'F':
        Term_PushKey(mod == 2 ? ITK_SHIFT_END :
                     mod == 5 ? ITK_CTRL_END : ITK_END);
        return;
    case 'Z': Term_PushKey(ITK_SHIFT_TAB); return;
    case 'P': case 'Q': case 'R': case 'S':     /* (modified) F1..F4 */
        if (mod == 5 && f == 'Q')
            Term_PushKey(ITK_CTRL_F2);
        else if (mod <= 1)
            Term_PushKey(ITK_F1 + (f - 'P'));
        return;
    case 'u':                                   /* CSI-u */
        Term_ModOther(p0, mod);
        return;
    case '~':
        switch (p0) {
        case 1: case 7:
            Term_PushKey(mod == 2 ? ITK_SHIFT_HOME :
                         mod == 5 ? ITK_CTRL_HOME : ITK_HOME);
            return;
        case 4: case 8:
            Term_PushKey(mod == 2 ? ITK_SHIFT_END :
                         mod == 5 ? ITK_CTRL_END : ITK_END);
            return;
        case 2:
            Term_PushKey(mod == 3 ? ITK_ALT_INS :
                         mod == 5 ? ITK_CTRL_INS : ITK_INS);
            return;
        case 3:
            Term_PushKey(mod == 3 ? ITK_ALT_DEL :
                         mod == 5 ? ITK_CTRL_DEL : ITK_DEL);
            return;
        case 5:
            Term_PushKey(mod == 2 ? ITK_SHIFT_PGUP :
                         mod == 5 ? ITK_CTRL_PGUP : ITK_PGUP);
            return;
        case 6:
            Term_PushKey(mod == 2 ? ITK_SHIFT_PGDN :
                         mod == 5 ? ITK_CTRL_PGDN : ITK_PGDN);
            return;
        case 11: case 12: case 13: case 14:     /* F1..F4 (old xterm) */
            if (mod <= 1) Term_PushKey(ITK_F1 + (p0 - 11));
            return;
        case 15:
            if (mod <= 1) Term_PushKey(ITK_F5);
            return;
        case 17: case 18: case 19: case 20: case 21: {
            int fk = ITK_F6 + (p0 - 17);        /* F6..F10 */
            if (mod == 2 && p0 == 20)      Term_PushKey(ITK_SHIFT_F9);
            else if (mod == 3 && p0 == 20) Term_PushKey(ITK_ALT_F9);
            else if (mod == 3 && p0 == 21) Term_PushKey(ITK_ALT_F10);
            else if (mod == 5 && p0 == 18) Term_PushKey(ITK_CTRL_F7);
            else if (mod <= 1)             Term_PushKey(fk);
            return;
        }
        case 23:
            if (mod <= 1) Term_PushKey(ITK_F11);
            return;
        case 24:
            if (mod == 3)      Term_PushKey(ITK_ALT_F12);
            else if (mod <= 1) Term_PushKey(ITK_F12);
            return;
        case 27:                                /* modifyOtherKeys */
            Term_ModOther(TP_NParam > 2 ? TP_Param[2] : 0, mod);
            return;
        default:
            return;
        }
    default:                                    /* unknown final: eat */
        return;
    }
}

/* feed one raw byte through the state machine */
static void Term_FeedByte(uint8_t c)
{
    switch (TP_State) {
    case TP_GROUND:
        if (c == 0x1B)      { TP_State = TP_ESC; return; }
        if (c == '\r' || c == '\n') { Term_PushKey(ITK_ENTER); return; }
        if (c == 0x7F)      { Term_PushKey(ITK_BACKSPACE); return; }
        if (c == '\t')      { Term_PushKey(ITK_TAB); return; }
        if (c >= 0x80)      return;             /* UTF-8 tails: eat */
        if (c)              Term_PushKey(c);    /* incl. Ctrl-letters
                                                   1..26 (8 = Ctrl-H) */
        return;
    case TP_ESC:
        if (c == '[') {
            TP_State = TP_CSI;
            TP_NParam = 0;
            TP_Param[0] = TP_Param[1] = TP_Param[2] = TP_Param[3] = 0;
            TP_Priv = 0;
            return;
        }
        if (c == 'O') { TP_State = TP_SS3; return; }
        if (c == 0x1B) { Term_PushKey(ITK_ESC); return; } /* stay */
        TP_State = TP_GROUND;
        Term_AltByte(c);
        return;
    case TP_SS3:
        TP_State = TP_GROUND;
        if (c >= 'P' && c <= 'S')
            Term_PushKey(ITK_F1 + (c - 'P'));
        /* SS3 keypad codes etc.: consumed */
        return;
    case TP_CSI:
        if (c == '<' || c == '?' || c == '>') { TP_Priv = c; return; }
        if (c >= '0' && c <= '9') {
            if (TP_NParam == 0)
                TP_NParam = 1;
            if (TP_NParam <= 4)
                TP_Param[TP_NParam - 1] =
                    TP_Param[TP_NParam - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (TP_NParam < 4)
                TP_NParam++;
            if (TP_NParam == 1)                 /* leading ';' */
                TP_NParam = 2;
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {
            TP_State = TP_GROUND;
            Term_CsiFinal(c);
            return;
        }
        return;                                 /* intermediates: eat */
    }
}

/* resolve a dangling ESC (no continuation arrived) as a real ESC key */
static void Term_FlushEsc(void)
{
    if (TP_State == TP_ESC) {
        TP_State = TP_GROUND;
        Term_PushKey(ITK_ESC);
    }
    TP_EscPending = 0;
}

/* test hook (selftest, FR-007): feed `n` bytes, optionally flush a
 * pending lone ESC, then pop one decoded key (ITK_NONE when drained).
 * Call repeatedly with n=0 to drain the queue. */
int Screen_TermFeedTest(const uint8_t *buf, int n, int flush)
{
    int i;
    for (i = 0; i < n; i++)
        Term_FeedByte(buf[i]);
    if (flush)
        Term_FlushEsc();
    return Term_PopKey();
}

void Screen_TermMouseTest(it_mouse_t *m)
{
    m->px = TermMousePX;
    m->py = TermMousePY;
    m->x = m->px / 8;
    m->y = m->py / 8;
    m->b = TermMouseB;
}

#ifndef _WIN32
/* read everything the tty has buffered into the parser; called from
 * both the key and the mouse poll. A lone ESC only becomes ITK_ESC
 * after it has survived one full empty poll (~15ms editor tick) --
 * terminals transmit multi-byte sequences atomically. */
static void Term_Pump(void)
{
    unsigned char buf[256];
    ssize_t n;
    int got = 0;

    for (;;) {
        ssize_t i;

        n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (i = 0; i < n; i++)
            Term_FeedByte(buf[i]);
        got = 1;
        if (n < (ssize_t)sizeof(buf))
            break;
    }

    if (TP_State == TP_ESC) {
        if (!got && TP_EscPending)
            Term_FlushEsc();
        else
            TP_EscPending = 1;
    } else {
        TP_EscPending = 0;
    }
}
#endif

static int Term_Key(void)
{
#ifdef _WIN32
    /* Windows console (`ITED_TERM=1`): conio scan codes. 0x00/0xE0
     * prefixes carry the Alt/Ctrl/Shift combos (feature 011). */
    if (!_kbhit())
        return ITK_NONE;
    {
        int c = _getch();

        if (c == 0 || c == 0xE0) {
            /* scan -> Alt-letter (PC scan code rows) */
            static const char altrow[] =
                "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
            int e = _getch();

            if (e >= 16 && e <= 50 && altrow[e - 16])
                return ITK_ALT_A + (altrow[e - 16] - 'a');
            switch (e) {
            case 72: return ITK_UP;
            case 80: return ITK_DOWN;
            case 75: return ITK_LEFT;
            case 77: return ITK_RIGHT;
            case 73: return ITK_PGUP;
            case 81: return ITK_PGDN;
            case 71: return ITK_HOME;
            case 79: return ITK_END;
            case 82: return ITK_INS;
            case 83: return ITK_DEL;
            case 15: return ITK_SHIFT_TAB;
            case 59: case 60: case 61: case 62: case 63:
            case 64: case 65: case 66: case 67: case 68:
                return ITK_F1 + (e - 59);
            case 133: return ITK_F11;
            case 134: return ITK_F12;
            /* feature 011: modifier combos the console reports */
            case 115: return ITK_CTRL_LEFT;
            case 116: return ITK_CTRL_RIGHT;
            case 141: return ITK_CTRL_UP;
            case 145: return ITK_CTRL_DOWN;
            case 119: return ITK_CTRL_HOME;
            case 117: return ITK_CTRL_END;
            case 132: return ITK_CTRL_PGUP;
            case 118: return ITK_CTRL_PGDN;
            case 146: return ITK_CTRL_INS;
            case 147: return ITK_CTRL_DEL;
            case 162: return ITK_ALT_INS;
            case 163: return ITK_ALT_DEL;
            case 152: return ITK_ALT_UP;
            case 160: return ITK_ALT_DOWN;
            case 130: return ITK_ALT_MINUS;
            case 131: return ITK_ALT_PLUS;
            case 92:  return ITK_SHIFT_F9;      /* Shift-F1..F10=84..93 */
            case 95:  return ITK_CTRL_F2;       /* Ctrl-F1..F10=94..103 */
            case 100: return ITK_CTRL_F7;
            case 112: return ITK_ALT_F9;        /* Alt-F1..F10=104..113 */
            case 113: return ITK_ALT_F10;
            default:
                if (e >= 120 && e <= 128)       /* Alt-1..9 */
                    return ITK_ALT_0 + (e - 119);
                if (e == 129)                   /* Alt-0 */
                    return ITK_ALT_0;
                return ITK_NONE;
            }
        }
        if (c == 27)   return ITK_ESC;
        if (c == 13)   return ITK_ENTER;
        if (c == 8)    return ITK_BACKSPACE;    /* BS and Ctrl-H collide
                                                   on the console */
        if (c == 0x7F) return ITK_CTRL_BACKSPACE;
        if (c == 9)    return ITK_TAB;
        return c;
    }
#else
    Term_Pump();
    return Term_PopKey();
#endif
}

#ifndef _WIN32
static void Term_Mouse(it_mouse_t *m)
{
    Term_Pump();
    Screen_TermMouseTest(m);
}
#endif

static const screen_backend_t Screen_BackendTerm = {
    Term_Init, Term_UnInit, Term_Present, Term_Key,
#ifdef _WIN32
    NULL                        /* console: pixel backend has the mouse */
#else
    Term_Mouse
#endif
};

/* ---- public init/update/key dispatch --------------------------------- */

int Screen_Init(void)
{
    const char *term = getenv("ITED_TERM");
    int force_term = (term && *term && *term != '0');

#ifdef _WIN32
    Backend = force_term ? &Screen_BackendTerm : &Screen_BackendWin32;
#else
#  ifdef HAVE_SDL
    /* POSIX: prefer the authentic SDL window when not forced to the
     * terminal and a display is available. SDL's own init is the probe;
     * if it fails (headless), fall back to the terminal. */
    if (!force_term && Screen_BackendSDL.init()) {
        Backend = &Screen_BackendSDL;       /* already initialised */
        Inited = 1;
        return 1;
    }
    if (!force_term)
        fprintf(stderr, "ited: no display; using terminal backend\n");
#  endif
    Backend = &Screen_BackendTerm;
#endif

    if (!Backend->init())
        return 0;
    Inited = 1;
    return 1;
}

void Screen_UnInit(void)
{
    if (!Inited)
        return;
    Backend->uninit();
    Inited = 0;
}

void Screen_Update(void)
{
    if (Backend)
        Backend->present(&Back[0][0]);
}

int Key_Get(void)
{
    return Backend ? Backend->key() : ITK_NONE;
}

void Screen_GetMouse(it_mouse_t *m)
{
    memset(m, 0, sizeof(*m));
    if (Backend && Backend->mouse)
        Backend->mouse(m);
}
