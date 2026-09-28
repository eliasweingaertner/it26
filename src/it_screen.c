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
#include "it_cornerart.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
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

/* "2026 AI PORT" corner art (port marking, not in the original): a
 * 76x72 badge bitmap (art/corner.bmp, embedded as it_cornerart.c)
 * blitted into the top-right corner of the pixel output only.  It
 * holds for 10 seconds after the first rasterized frame -- or until
 * the mouse touches it -- then slides out diagonally towards the
 * top-right (40 px/s) and stays gone.  Cell contents (ITED_DUMP
 * hashes) and the terminal backend are untouched; ITED_NOBANNER=1
 * hides it for reference-screenshot comparisons. */
#define CORNERART_HOLD_MS   10000
#define CORNERART_MS_PER_PX 25           /* slide speed: 40 px/s */

/* last mouse position seen by Screen_GetMouse (logical pixels).  The
 * rasterizer reads this instead of polling the backend itself: the
 * Win32 mouse poll pumps the message queue, which must not happen
 * mid-present.  The editor polls every tick, so it stays fresh. */
static int CornerMousePX = -1, CornerMousePY = -1;

static uint64_t CornerArtMS(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}

static void RasterizeCornerArt(uint32_t *px)
{
    static int state;                    /* 0 unknown, 1 on, 2 off */
    static uint64_t t0;
    uint64_t now, elapsed;
    int off = 0, sx, sy;

    if (!state)
        state = getenv("ITED_NOBANNER") ? 2 : 1;
    if (state == 2)
        return;

    now = CornerArtMS();
    if (!t0)
        t0 = now;
    if (now - t0 < CORNERART_HOLD_MS &&  /* mouse touch: slide out now */
        CornerMousePX >= 640 - IT_CORNERART_W &&
        CornerMousePY <  IT_CORNERART_H)
        t0 = now - CORNERART_HOLD_MS;
    elapsed = now - t0;
    if (elapsed > CORNERART_HOLD_MS) {
        off = (int)((elapsed - CORNERART_HOLD_MS) / CORNERART_MS_PER_PX);
        if (off >= IT_CORNERART_W || off >= IT_CORNERART_H) {
            state = 2;                   /* fully slid out: stay gone */
            return;
        }
    }

    /* blit shifted `off` pixels right and up, clipped to the screen:
     * the right columns leave past x=639, the top rows past y=0 */
    for (sy = off; sy < IT_CORNERART_H; sy++)
        for (sx = 0; sx < IT_CORNERART_W - off; sx++)
            px[(sy - off)*640 + (640 - IT_CORNERART_W + off + sx)] =
                IT_CornerArtPal[IT_CornerArt[sy][sx]];
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

    RasterizeCornerArt(px);
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
    else if (c == 0x0D)            Term_PushKey(ITK_ALT_ENTER);
    else if (c == 0x7F || c == 0x08) Term_PushKey(ITK_ALT_BACKSPACE);
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
    if (x < 1) x = 1;
    if (x > SCREEN_W) x = SCREEN_W;
    if (y < 1) y = 1;
    if (y > SCREEN_H) y = SCREEN_H;
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
            if (f == 'C') Term_PushKey(ITK_ALT_RIGHT);
            if (f == 'D') Term_PushKey(ITK_ALT_LEFT);
        } else if (mod <= 1)
            Term_PushKey(plain[i]);
        return;
    }
    case 'H':
        Term_PushKey(mod == 2 ? ITK_SHIFT_HOME : mod == 3 ? ITK_ALT_HOME :
                     mod == 5 ? ITK_CTRL_HOME : ITK_HOME);
        return;
    case 'F':
        Term_PushKey(mod == 2 ? ITK_SHIFT_END : mod == 3 ? ITK_ALT_END :
                     mod == 5 ? ITK_CTRL_END : ITK_END);
        return;
    case 'Z': Term_PushKey(ITK_SHIFT_TAB); return;
    case 'P': case 'Q': case 'R': case 'S':     /* (modified) F1..F4 */
        if (mod == 5 && f == 'Q')
            Term_PushKey(ITK_CTRL_F2);
        else if (mod == 5 && f == 'S')
            Term_PushKey(ITK_CTRL_F4);
        else if (mod == 3)                      /* Alt-F1..F4 */
            Term_PushKey(ITK_ALT_F1 + (f - 'P'));
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
            if (mod == 3)      Term_PushKey(ITK_ALT_F1 + 4);
            else if (mod == 5) Term_PushKey(ITK_CTRL_F5);
            else if (mod == 2) Term_PushKey(ITK_SHIFT_F5);
            else if (mod <= 1) Term_PushKey(ITK_F5);
            return;
        case 17: case 18: case 19: case 20: case 21: {
            int fk = ITK_F6 + (p0 - 17);        /* F6..F10 */
            if (mod == 2 && p0 == 20)      Term_PushKey(ITK_SHIFT_F9);
            else if (mod == 3 && p0 == 20) Term_PushKey(ITK_ALT_F9);
            else if (mod == 3 && p0 == 21) Term_PushKey(ITK_ALT_F10);
            else if (mod == 5 && p0 == 18) Term_PushKey(ITK_CTRL_F7);
            else if (mod == 2 && p0 == 17) Term_PushKey(ITK_SHIFT_F6);
            else if (mod == 5 && p0 == 17) Term_PushKey(ITK_CTRL_F6);
            else if (mod == 3 && p0 <= 19)      /* Alt-F6..F8 */
                Term_PushKey(ITK_ALT_F1 + 5 + (p0 - 17));
            else if (mod <= 1)             Term_PushKey(fk);
            return;
        }
        case 23:
            if (mod == 3)      Term_PushKey(ITK_ALT_F11);
            else if (mod <= 1) Term_PushKey(ITK_F11);
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
    NULL,                       /* console: pixel backend has the mouse */
#else
    Term_Mouse,
#endif
    NULL                        /* no physical positions from a terminal:
                                 * Key_GetEvent() infers scan via the
                                 * reverse map (feature 014, research R8) */
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

/* ===================================================================
 * Feature 014: the two-layer key event (IT_K.ASM K_GetKey, CX/DX)
 * =================================================================== */

/* CP437 -> Unicode for the 128 high codes; the low half is ASCII.
 * Module files carry these bytes directly and the editor draws them
 * from the hi-ASCII font bank installed by Screen_DefineHiASCII
 * (feature 013), so this table IS the tracker's character repertoire.
 * Anything a host layout produces that is not in here cannot be stored
 * and is rejected at the field-input layer rather than transliterated. */
static const uint16_t CP437High[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7, /* 80 */
    0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5, /* 88 */
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9, /* 90 */
    0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192, /* 98 */
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA, /* A0 */
    0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB, /* A8 */
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556, /* B0 */
    0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510, /* B8 */
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F, /* C0 */
    0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567, /* C8 */
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B, /* D0 */
    0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580, /* D8 */
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4, /* E0 */
    0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229, /* E8 */
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248, /* F0 */
    0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0  /* F8 */
};

/* Unicode -> CP437. Returns 0 when the character has no CP437 code,
 * which the caller must treat as "reject the keystroke". */
uint16_t Screen_UnicodeToCP437(uint32_t u)
{
    int i;
    if (u == 0 || u > 0xFFFF)
        return 0;
    if (u < 0x80)
        return (uint16_t)u;
    for (i = 0; i < 128; i++)
        if (CP437High[i] == (uint16_t)u)
            return (uint16_t)(0x80 + i);
    return 0;
}

/* US set-1 scancode -> the characters that position produces. Used in
 * reverse: a backend that reports characters but no physical position
 * (the terminal; ANSI has no scancodes) gets its `scan` inferred from
 * this. That keeps the terminal backend behaving exactly as it did
 * before this feature, and is the default the original assumed. */
static const struct { uint8_t scan; char plain, shifted; } USSet1[] = {
    {0x02,'1','!'}, {0x03,'2','@'}, {0x04,'3','#'}, {0x05,'4','$'},
    {0x06,'5','%'}, {0x07,'6','^'}, {0x08,'7','&'}, {0x09,'8','*'},
    {0x0A,'9','('}, {0x0B,'0',')'}, {0x0C,'-','_'}, {0x0D,'=','+'},
    {0x10,'q','Q'}, {0x11,'w','W'}, {0x12,'e','E'}, {0x13,'r','R'},
    {0x14,'t','T'}, {0x15,'y','Y'}, {0x16,'u','U'}, {0x17,'i','I'},
    {0x18,'o','O'}, {0x19,'p','P'}, {0x1A,'[','{'}, {0x1B,']','}'},
    {0x1E,'a','A'}, {0x1F,'s','S'}, {0x20,'d','D'}, {0x21,'f','F'},
    {0x22,'g','G'}, {0x23,'h','H'}, {0x24,'j','J'}, {0x25,'k','K'},
    {0x26,'l','L'}, {0x27,';',':'}, {0x28,'\'','"'},{0x29,'`','~'},
    {0x2B,'\\','|'},{0x2C,'z','Z'}, {0x2D,'x','X'}, {0x2E,'c','C'},
    {0x2F,'v','V'}, {0x30,'b','B'}, {0x31,'n','N'}, {0x32,'m','M'},
    {0x33,',','<'}, {0x34,'.','>'}, {0x35,'/','?'}, {0x39,' ',' '}
};

uint8_t Key_ReverseScan(uint16_t ch)
{
    size_t i;
    if (ch == 0 || ch > 0x7F)
        return 0;
    for (i = 0; i < sizeof(USSet1) / sizeof(USSet1[0]); i++)
        if (USSet1[i].plain == (char)ch || USSet1[i].shifted == (char)ch)
            return USSet1[i].scan;
    return 0;
}

/* ---- optional KEYBOARD.CFG layout override -------------------------
 * The original's replaceable translation table (IT_K.ASM:96), loaded
 * from a file in the format documented in Keyboard/DE.ASM's header and
 * parsed at IT_K.ASM:1274. The files Impulse Tracker shipped are DOS
 * .COM images: `FileLength DW` at ORG 100h, then repeated
 *   keycode:u8  { condition:u8  value:u16 } ... 0FFh
 * Conditions: 0 none, 1 shift^caps, 2 shift^!caps, 3 shift, 4 ctrl,
 * 5 either alt, 6 lalt, 7 ralt(AltGr), 8 numlock, 9 !numlock, FFh end.
 *
 * This affects the CHARACTER half only. Note entry never consults it
 * (FR-002) -- that is the whole point of the two-layer split. */
static uint8_t *KbdTable;
static size_t   KbdTableLen;
static char     KbdTableName[260];

/* 0 = loaded, else a static reason string */
const char *Key_LoadLayout(const char *path)
{
    FILE *fp;
    long  fsize;
    uint8_t *buf;
    size_t got;
    uint16_t declared;

    free(KbdTable);
    KbdTable = NULL;
    KbdTableLen = 0;
    KbdTableName[0] = 0;
    if (!path || !*path)
        return NULL;                    /* no override: host layout */

    fp = fopen(path, "rb");
    if (!fp)
        return "cannot open";
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return "not seekable"; }
    fsize = ftell(fp);
    rewind(fp);
    if (fsize < 4 || fsize > (1L << 20)) { fclose(fp); return "bad size"; }
    buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fclose(fp); return "out of memory"; }
    got = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    if (got != (size_t)fsize) { free(buf); return "short read"; }

    /* FileLength counts the table only; it must fit inside the file */
    declared = (uint16_t)(buf[0] | (buf[1] << 8));
    if (declared == 0 || (size_t)declared + 2 > got) {
        free(buf);
        return "bad FileLength";
    }
    /* walk it once: every list must terminate inside the declared span */
    {
        size_t i = 2, end = (size_t)declared + 2;
        while (i < end) {
            i++;                                /* keycode */
            for (;;) {
                if (i >= end)      { free(buf); return "unterminated"; }
                if (buf[i] == 0xFF) { i++; break; }
                if (i + 3 > end)   { free(buf); return "truncated entry"; }
                i += 3;                         /* condition + word */
            }
        }
    }
    KbdTable = buf;
    KbdTableLen = (size_t)declared + 2;
    snprintf(KbdTableName, sizeof(KbdTableName), "%s", path);
    return NULL;
}

const char *Key_LayoutName(void)
{
    return KbdTable ? KbdTableName : "";
}

/* Does `flags` satisfy the original's condition code? Caps/Num Lock
 * state is not tracked by the host backends, so conditions 1/2 collapse
 * to "shift or not" and 8/9 to "numlock-independent", which is what a
 * modern host's own layout already resolved for us. */
static int KbdCondMatch(uint8_t cond, uint8_t flags)
{
    int shift = (flags & ITKF_SHIFT) != 0;
    int ctrl  = (flags & ITKF_CTRL)  != 0;
    int alt   = (flags & ITKF_ALT)   != 0;
    switch (cond) {
    case 0: return !shift && !ctrl && !alt;
    case 1: return  shift && !ctrl && !alt;
    case 2: return !shift && !ctrl && !alt;
    case 3: return  shift;
    case 4: return  ctrl;
    case 5: return  alt;
    case 6: return (flags & ITKF_LALT) != 0;
    case 7: return (flags & ITKF_RALT) != 0;
    case 8: case 9: return !ctrl && !alt;
    default: return 0;
    }
}

/* Apply the loaded table to one event. Returns 1 when it produced a
 * value (which may legitimately be an Alt remap in the high byte). */
static int KbdTranslate(it_key_t *k)
{
    size_t i = 2, end = KbdTableLen;

    if (!KbdTable || k->scan == 0 || !(k->flags & ITKF_PRESSED))
        return 0;
    while (i < end) {
        uint8_t keycode = KbdTable[i++];
        int mine = (keycode == k->scan);
        for (;;) {
            uint8_t cond;
            uint16_t val;
            if (i >= end || KbdTable[i] == 0xFF) { i++; break; }
            cond = KbdTable[i];
            val  = (uint16_t)(KbdTable[i + 1] | (KbdTable[i + 2] << 8));
            i += 3;
            if (mine && KbdCondMatch(cond, k->flags)) {
                if ((val & 0xFF) == 0 && (val >> 8) != 0) {
                    /* Alt remap: scancode in the high byte. Keeps the
                     * shortcut on the printed keycap (research R5). */
                    k->ch = 0;
                } else {
                    k->ch = (uint16_t)(val & 0xFF);
                    k->code = (int)(val & 0xFF);
                }
                return 1;
            }
        }
    }
    return 0;
}

/* synthetic event queue for the selftest (Screen_KeyFeedTest) */
#define KEYFEED_MAX 64
static it_key_t KeyFeed[KEYFEED_MAX];
static int      KeyFeedHead, KeyFeedTail;

int Screen_KeyFeedTest(const it_key_t *k, int n)
{
    int i, pushed = 0;
    for (i = 0; i < n; i++) {
        int nx = (KeyFeedTail + 1) % KEYFEED_MAX;
        if (nx == KeyFeedHead)
            break;                      /* full; caller drains and retries */
        KeyFeed[KeyFeedTail] = k[i];
        KeyFeedTail = nx;
        pushed++;
    }
    return pushed;
}

static int KeyFeed_Pop(it_key_t *k)
{
    if (KeyFeedHead == KeyFeedTail)
        return 0;
    *k = KeyFeed[KeyFeedHead];
    KeyFeedHead = (KeyFeedHead + 1) % KEYFEED_MAX;
    return 1;
}

int Key_GetEvent(it_key_t *k)
{
    int c;

    memset(k, 0, sizeof(*k));
    if (KeyFeed_Pop(k))
        return 1;
    if (!Backend)
        return 0;
    if (Backend->key_event) {
        if (!Backend->key_event(k))
            return 0;
        KbdTranslate(k);        /* no-op unless a layout file is loaded */
        return 1;
    }

    /* Backend reports the legacy code only (terminal). Rebuild the
     * event around it: the character is the code when it is printable,
     * and the position comes from the US reverse map. */
    c = Backend->key();
    if (c == ITK_NONE)
        return 0;
    k->code  = c;
    k->flags = ITKF_PRESSED;
    if (c > 0 && c < 0x100)
        k->ch = (uint16_t)c;
    k->scan = Key_ReverseScan(k->ch);
    return 1;
}

int Screen_AltEnterIsKey = 0;

int Key_Get(void)
{
    it_key_t k;
    return Key_GetEvent(&k) ? k.code : ITK_NONE;
}

void Screen_GetMouse(it_mouse_t *m)
{
    memset(m, 0, sizeof(*m));
    if (Backend && Backend->mouse) {
        Backend->mouse(m);
        CornerMousePX = m->px;           /* corner-art hover test */
        CornerMousePY = m->py;
    }
}
