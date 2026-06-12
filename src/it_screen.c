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

/* ---- rasterizer: cells -> 640x400 RGB (the authentic output) --------- */

static const uint8_t *GlyphBitmap(uint8_t ch)
{
    if (ch >= IT_CHARDEF_FIRST && ch < IT_CHARDEF_FIRST + IT_CHARDEF_COUNT)
        return IT_CharDefs[ch - IT_CHARDEF_FIRST];
    return IT_FontROM[ch];
}

void Screen_Rasterize(uint32_t *px)
{
    int cy, cx, row;

    for (cy = 0; cy < SCREEN_H; cy++) {
        for (cx = 0; cx < SCREEN_W; cx++) {
            screen_cell_t c = Back[cy][cx];
            const uint8_t *g = GlyphBitmap(c.ch);
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
    fflush(stdout);
    memset(TermFront, 0xFF, sizeof(TermFront));
    return 1;
}

static void Term_UnInit(void)
{
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
        buf[0] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        buf[1] = 0;
        return buf;
    }
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
                                  TermGlyph(c.ch));
        }
        fwrite(out, 1, n, stdout);
        memcpy(TermFront[y], row, sizeof(TermFront[y]));
    }
    fflush(stdout);
}

static int Term_Key(void)
{
#ifdef _WIN32
    if (!_kbhit())
        return ITK_NONE;
    {
        int c = _getch();

        if (c == 0 || c == 0xE0) {
            int e = _getch();
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
            default: return ITK_NONE;
            }
        }
        if (c == 27) return ITK_ESC;
        if (c == 13) return ITK_ENTER;
        if (c == 8)  return ITK_BACKSPACE;
        if (c == 9)  return ITK_TAB;
        return c;
    }
#else
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
        return ITK_NONE;

    if (c == 0x1B) {
        unsigned char seq[4];
        if (read(STDIN_FILENO, seq, 1) != 1)
            return ITK_ESC;
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, seq + 1, 1) != 1)
                return ITK_ESC;
            switch (seq[1]) {
            case 'A': return ITK_UP;
            case 'B': return ITK_DOWN;
            case 'D': return ITK_LEFT;
            case 'C': return ITK_RIGHT;
            case 'H': return ITK_HOME;
            case 'F': return ITK_END;
            case 'Z': return ITK_SHIFT_TAB;
            case '5': read(STDIN_FILENO, seq + 2, 1); return ITK_PGUP;
            case '6': read(STDIN_FILENO, seq + 2, 1); return ITK_PGDN;
            case '2': read(STDIN_FILENO, seq + 2, 1); return ITK_INS;
            case '3': read(STDIN_FILENO, seq + 2, 1); return ITK_DEL;
            case '1': {
                /* xterm F-keys: ESC [ 1 ... ~ */
                unsigned char a = 0, b = 0;
                if (read(STDIN_FILENO, &a, 1) == 1 && a != '~')
                    read(STDIN_FILENO, &b, 1);
                switch (a) {
                case '1': return ITK_F1;
                case '2': return ITK_F2;
                case '3': return ITK_F3;
                case '4': return ITK_F4;
                case '5': return ITK_F5;
                case '7': return ITK_F6;
                case '8': return ITK_F7;
                case '9': return ITK_F8;
                default:  return ITK_NONE;
                }
            }
            default: return ITK_NONE;
            }
        }
        if (seq[0] == 'O') {
            if (read(STDIN_FILENO, seq + 1, 1) != 1)
                return ITK_ESC;
            switch (seq[1]) {
            case 'P': return ITK_F1;
            case 'Q': return ITK_F2;
            case 'R': return ITK_F3;
            case 'S': return ITK_F4;
            default:  return ITK_NONE;
            }
        }
        return ITK_ESC;
    }
    if (c == '\n' || c == '\r') return ITK_ENTER;
    if (c == 0x7F || c == 8)    return ITK_BACKSPACE;
    if (c == '\t')              return ITK_TAB;
    return c;
#endif
}

static const screen_backend_t Screen_BackendTerm = {
    Term_Init, Term_UnInit, Term_Present, Term_Key, NULL
};

/* ---- public init/update/key dispatch --------------------------------- */

int Screen_Init(void)
{
    const char *term = getenv("ITED_TERM");

#ifdef _WIN32
    Backend = (term && *term && *term != '0') ? &Screen_BackendTerm
                                              : &Screen_BackendWin32;
#else
    (void)term;
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
