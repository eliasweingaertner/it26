/*
 * it_screen.c
 * -----------
 * 80x50 text-cell framebuffer with VT/ANSI terminal backend and
 * damage tracking. See it_screen.h.
 */

#include <stdio.h>
#include <string.h>
#include "it_screen.h"

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

typedef struct cell_t {
    uint8_t ch;
    uint8_t attr;
} cell_t;

static cell_t Front[SCREEN_H][SCREEN_W];
static cell_t Back[SCREEN_H][SCREEN_W];
static int Inited = 0;

/* Impulse Tracker's default "Camouflage" palette (IT_S.ASM PaletteDefs),
 * the 16 VGA colours in 6-bit (0..63) converted to 8-bit RGB. Emitted as
 * 24-bit truecolor so the editor matches IT's exact appearance:
 *   0 black bg     1 panel shadow  2 panel face   3 panel highlight
 *   4 magenta      5 yellow        6 green        7 dark red
 *   8 dk green     9 green         10 teal        11 near-white
 *   12 grey        13 dk magenta   14 dk grey     15 near-black     */
static const uint8_t PaletteRGB[16][3] = {
    {  0,  0,  0}, {125, 89, 69}, {182,150,121}, {235,235,202},
    {178,  0, 85}, {255,255, 85}, { 69,154, 73}, { 77, 12, 24},
    { 32, 85,  0}, { 24,117, 45}, { 57,158,117}, {223,235,227},
    {162,162,162}, {142, 20, 85}, { 89, 65, 61}, { 53, 49, 45},
};

int Screen_Init(void)
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

    memset(Front, 0xFF, sizeof(Front)); /* force full redraw */
    memset(Back, 0, sizeof(Back));
    Inited = 1;
    return 1;
}

void Screen_UnInit(void)
{
    if (!Inited)
        return;
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
    Inited = 0;
}

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

void Screen_DrawBox(int x0, int y0, int x1, int y1, int style)
{
    /* style 0: single line (IT uses several bevel styles; the chrome
     * can be refined when the editor screens are ported). */
    int x, y;
    (void)style;

    for (x = x0 + 1; x < x1; x++) {
        Screen_PutChar(x, y0, 0xC4, 0x07);
        Screen_PutChar(x, y1, 0xC4, 0x07);
    }
    for (y = y0 + 1; y < y1; y++) {
        Screen_PutChar(x0, y, 0xB3, 0x07);
        Screen_PutChar(x1, y, 0xB3, 0x07);
    }
    Screen_PutChar(x0, y0, 0xDA, 0x07);
    Screen_PutChar(x1, y0, 0xBF, 0x07);
    Screen_PutChar(x0, y1, 0xC0, 0x07);
    Screen_PutChar(x1, y1, 0xD9, 0x07);
}

/* CP437 -> UTF-8 for the box/blocks range the IT UI actually uses. */
static const char *CP437ToUTF8(uint8_t c)
{
    static char buf[8];
    static const struct { uint8_t c; const char *u; } map[] = {
        {0xC4, "\xE2\x94\x80"}, {0xB3, "\xE2\x94\x82"}, /* - |   */
        {0xDA, "\xE2\x94\x8C"}, {0xBF, "\xE2\x94\x90"}, /* corners */
        {0xC0, "\xE2\x94\x94"}, {0xD9, "\xE2\x94\x98"},
        {0xC2, "\xE2\x94\xAC"}, {0xC1, "\xE2\x94\xB4"}, /* T tees  */
        {0xC3, "\xE2\x94\x9C"}, {0xB4, "\xE2\x94\xA4"},
        {0xC5, "\xE2\x94\xBC"},
        {0xDB, "\xE2\x96\x88"}, {0xDC, "\xE2\x96\x84"}, /* full/low block */
        {0xDF, "\xE2\x96\x80"}, {0xDD, "\xE2\x96\x8C"}, /* up/left half */
        {0xDE, "\xE2\x96\x90"},                         /* right half */
        {0xB0, "\xE2\x96\x91"}, {0xB1, "\xE2\x96\x92"}, /* shades */
        {0xB2, "\xE2\x96\x93"},
        {0xFA, "\xC2\xB7"}, {0xF9, "\xE2\x97\x8F"},     /* dots */
        {0x07, "\xE2\x97\x8F"},                         /* bullet */
        {0x10, "\xE2\x96\xB6"}, {0x11, "\xE2\x97\x80"}, /* play arrows */
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].c == c)
            return map[i].u;
    if (c < 0x20 || c >= 0x7F) {
        buf[0] = '.';
        buf[1] = 0;
        return buf;
    }
    buf[0] = (char)c;
    buf[1] = 0;
    return buf;
}

void Screen_Update(void)
{
    char out[SCREEN_W * 32 + 64];
    int y, x;

    for (y = 0; y < SCREEN_H; y++) {
        int dirty = memcmp(Front[y], Back[y], sizeof(Front[y])) != 0;
        size_t n = 0;
        int lastattr = -1;

        if (!dirty)
            continue;

        n += (size_t)snprintf(out + n, sizeof(out) - n,
                              "\x1b[%d;1H", y + 1);
        for (x = 0; x < SCREEN_W && n < sizeof(out) - 40; x++) {
            cell_t c = Back[y][x];

            if (c.attr != lastattr) {
                const uint8_t *fg = PaletteRGB[c.attr & 15];
                const uint8_t *bg = PaletteRGB[(c.attr >> 4) & 15];
                n += (size_t)snprintf(out + n, sizeof(out) - n,
                        "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm",
                        fg[0], fg[1], fg[2], bg[0], bg[1], bg[2]);
                lastattr = c.attr;
            }
            n += (size_t)snprintf(out + n, sizeof(out) - n, "%s",
                                  CP437ToUTF8(c.ch));
        }
        fwrite(out, 1, n, stdout);
        memcpy(Front[y], Back[y], sizeof(Front[y]));
    }
    fflush(stdout);
}

void Screen_DumpPlain(void *fpv)
{
    FILE *fp = (FILE *)fpv;
    int y, x;
    for (y = 0; y < SCREEN_H; y++) {
        for (x = 0; x < SCREEN_W; x++) {
            uint8_t c = Back[y][x].ch;
            char o;
            switch (c) {
            case 0xC4: o = '-'; break;
            case 0xB3: o = '|'; break;
            case 0xDA: case 0xBF: case 0xC0: case 0xD9:
            case 0xC3: case 0xB4: case 0xC2: case 0xC1: case 0xC5:
                o = '+'; break;
            case 0xFA: o = '.'; break;
            case 0x10: o = '>'; break;
            case 0x11: o = '<'; break;
            default:
                o = (c >= 32 && c < 127) ? (char)c : ' ';
                break;
            }
            fputc(o, fp);
        }
        fputc('\n', fp);
    }
}

/* ---- keyboard ------------------------------------------------------- */

int Key_Get(void)
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
            case '5': read(STDIN_FILENO, seq + 2, 1); return ITK_PGUP;
            case '6': read(STDIN_FILENO, seq + 2, 1); return ITK_PGDN;
            case '2': read(STDIN_FILENO, seq + 2, 1); return ITK_INS;
            case '3': read(STDIN_FILENO, seq + 2, 1); return ITK_DEL;
            default: return ITK_NONE;
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
