/*
 * it_screen_remote.c -- headless remote-control backend (ITED_REMOTE=1).
 *
 * Not part of the original. A test harness (headless-dos/hdos) drives the
 * editor through stdin with the same key scripts it plays into the real
 * IT 2.14 under QEMU, and captures the cell buffer for a cell-by-cell
 * comparison. No window, no terminal: the editor's own loops run
 * unchanged, only the backend is replaced.
 *
 * Protocol, one command per line on stdin; replies on stdout start '@':
 *   ev <scan> <flags> <ch> <code>   queue one it_key_t (decimal or 0x hex);
 *                                   the harness does the key translation
 *   sync                            -> "@ok" once every earlier event has
 *                                      been taken and the editor has gone
 *                                      back to idle-polling (so its redraw
 *                                      after the last key has been presented)
 *   dump <file.json>                sync, then write the cell buffer as
 *                                   {"cols","rows","cursor","chars","attrs"}
 *                                   (hex, CP437) -> "@ok"
 *   shot <file.bmp>                 sync, then Screen_WriteBMP -> "@ok"
 *   quit                            deliver ITK_QUIT
 * Errors reply "@err <text>".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "it_screen.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef CRITICAL_SECTION rm_mutex_t;
#  define rm_lock(m)    EnterCriticalSection(m)
#  define rm_unlock(m)  LeaveCriticalSection(m)
#else
#  include <pthread.h>
typedef pthread_mutex_t rm_mutex_t;
#  define rm_lock(m)    pthread_mutex_lock(m)
#  define rm_unlock(m)  pthread_mutex_unlock(m)
#endif

#define RM_COLS   80
#define RM_ROWS   50
#define RM_LINES  1024
#define RM_LINE   512

/* the editor needs this many consecutive empty polls before a sync
 * counts as idle: the first may come from a drain loop inside a key
 * handler, the second is the outer loop after its Screen_Update */
#define RM_IDLE_POLLS 2

static screen_cell_t Cells[RM_ROWS * RM_COLS];
static rm_mutex_t    Mutex;
static char          Lines[RM_LINES][RM_LINE];
static int           LineHead, LineTail;    /* ring, guarded by Mutex */
static int           InputEOF;
static int           IdlePolls;

static void reply(const char *s)
{
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

#ifdef _WIN32
static DWORD WINAPI reader(LPVOID arg)
#else
static void *reader(void *arg)
#endif
{
    char buf[RM_LINE];
    (void)arg;
    while (fgets(buf, sizeof(buf), stdin)) {
        size_t n = strcspn(buf, "\r\n");
        buf[n] = 0;
        for (;;) {
            int next;
            rm_lock(&Mutex);
            next = (LineTail + 1) % RM_LINES;
            if (next != LineHead) {
                memcpy(Lines[LineTail], buf, n + 1);
                LineTail = next;
                rm_unlock(&Mutex);
                break;
            }
            rm_unlock(&Mutex);
#ifdef _WIN32
            Sleep(5);
#else
            {
                struct timespec ts = { 0, 5000000 };
                nanosleep(&ts, NULL);
            }
#endif
        }
    }
    rm_lock(&Mutex);
    InputEOF = 1;
    rm_unlock(&Mutex);
    return 0;
}

static int RM_Init(void)
{
#ifdef _WIN32
    HANDLE t;
    InitializeCriticalSection(&Mutex);
    t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    if (!t)
        return 0;
    CloseHandle(t);
#else
    pthread_t t;
    pthread_mutex_init(&Mutex, NULL);
    if (pthread_create(&t, NULL, reader, NULL) != 0)
        return 0;
    pthread_detach(t);
#endif
    reply("@ready");
    return 1;
}

static void RM_UnInit(void)
{
}

static void RM_Present(const screen_cell_t *cells)
{
    memcpy(Cells, cells, sizeof(Cells));
}

static int RM_Key(void)
{
    return ITK_NONE;                    /* key_event is always used */
}

static int write_dump(const char *path)
{
    static const char hex[] = "0123456789abcdef";
    FILE *fp = fopen(path, "w");
    int i;
    if (!fp)
        return 0;
    fprintf(fp, "{\"cols\": %d, \"rows\": %d, \"cursor\": [-1, -1], "
                "\"chars\": \"", RM_COLS, RM_ROWS);
    for (i = 0; i < RM_ROWS * RM_COLS; i++) {
        fputc(hex[Cells[i].ch >> 4], fp);
        fputc(hex[Cells[i].ch & 15], fp);
    }
    fputs("\", \"attrs\": \"", fp);
    for (i = 0; i < RM_ROWS * RM_COLS; i++) {
        fputc(hex[Cells[i].attr >> 4], fp);
        fputc(hex[Cells[i].attr & 15], fp);
    }
    fputs("\"}\n", fp);
    fclose(fp);
    return 1;
}

/* Called from the editor's key loops (main thread). Returns 1 with an
 * event, or 0 = nothing pending, exactly like the pixel backends. */
static int RM_KeyEvent(it_key_t *k)
{
    char line[RM_LINE];

    for (;;) {
        rm_lock(&Mutex);
        if (LineHead == LineTail) {
            int eof = InputEOF;
            rm_unlock(&Mutex);
            if (eof) {                  /* harness went away: quit */
                k->code = ITK_QUIT;
                k->flags = ITKF_PRESSED;
                return 1;
            }
            IdlePolls++;
            return 0;
        }
        memcpy(line, Lines[LineHead], RM_LINE);
        rm_unlock(&Mutex);

        if (!strncmp(line, "ev ", 3)) {
            unsigned long v[4];
            char *p = line + 3, *end;
            int i;
            for (i = 0; i < 4; i++) {
                v[i] = strtoul(p, &end, 0);
                if (end == p)
                    break;
                p = end;
            }
            rm_lock(&Mutex);
            LineHead = (LineHead + 1) % RM_LINES;
            rm_unlock(&Mutex);
            if (i < 4) {
                reply("@err ev needs: scan flags ch code");
                continue;
            }
            k->scan  = (uint8_t)v[0];
            k->flags = (uint8_t)v[1];
            k->ch    = (uint16_t)v[2];
            k->code  = (int)v[3];
            IdlePolls = 0;
            return 1;
        }

        if (!strcmp(line, "sync") || !strncmp(line, "dump ", 5) ||
            !strncmp(line, "shot ", 5)) {
            if (IdlePolls < RM_IDLE_POLLS) {    /* not idle yet: come back */
                IdlePolls++;
                return 0;
            }
            rm_lock(&Mutex);
            LineHead = (LineHead + 1) % RM_LINES;
            rm_unlock(&Mutex);
            if (line[0] == 's' && line[1] == 'y')
                reply("@ok");
            else if (line[0] == 'd')
                reply(write_dump(line + 5) ? "@ok" : "@err cannot write dump");
            else
                reply(Screen_WriteBMP(line + 5) ? "@ok" : "@err cannot write bmp");
            continue;
        }

        rm_lock(&Mutex);
        LineHead = (LineHead + 1) % RM_LINES;
        rm_unlock(&Mutex);
        if (!strcmp(line, "quit")) {
            k->code = ITK_QUIT;
            k->flags = ITKF_PRESSED;
            return 1;
        }
        if (line[0])
            reply("@err unknown command");
    }
}

const screen_backend_t Screen_BackendRemote = {
    RM_Init, RM_UnInit, RM_Present, RM_Key, NULL, RM_KeyEvent
};
