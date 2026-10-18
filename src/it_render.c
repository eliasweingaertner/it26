/*
 * it_render.c
 * -----------
 * Feature 018: offline renders after Esa Ruoho's IT 2.354 fork. The engine
 * and the WAV driver are used exactly as itplay -w uses them (main.c):
 * Music_Play* with StopEndOfPlaySection, then WAVDriver_RenderAny until the
 * engine stops. Nothing here changes how the engine plays.
 *
 * A pattern render is one pass of the pattern (Music_ToggleWAVRender,
 * IT_MUSIC.ASM): StopEndOfPlaySection makes the engine call Music_Stop on
 * the tick after the last row, and that tick is left out, so the file is
 * exactly the pattern's playing time. A song render runs order 0 to the
 * song end the same way, with a 30-minute safety cap.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "it_music.h"
#include "it_pattern.h"
#include "it_screen.h"                  /* CP437 <-> Unicode */
#include "it_render.h"

void     WAVDriver_RenderAny(void *dst, uint32_t frames);
void     WAVDriver_SetMixSpeed(uint32_t hz);
uint32_t WAVDriver_GetMixSpeed(void);
void     WAVDriver_SetOutputFormat(int fmt);
void     WAVDriver_SetOutputChannels(int ch);
void     WAVDriver_SetForceMono(int on);
void     WAVDriver_SetSBFilter(int mode);
void     WAVDriver_SetSBFeedback(int mode);
void     WAVDriver_SetStartRamp(int on);
uint32_t WAVDriver_Pending(void);

/* ---- WAV writer ------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t *h, const render_wav_t *w)
{
    uint32_t bps = w->is_float ? 4 : 2;

    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + w->bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = (uint8_t)(w->is_float ? 3 : 1);     /* IEEE float / PCM */
    h[22] = (uint8_t)w->channels;
    put32(h + 24, w->rate);
    put32(h + 28, w->rate * bps * (uint32_t)w->channels);
    h[32] = (uint8_t)(bps * (uint32_t)w->channels);
    h[34] = (uint8_t)(bps * 8);
    memcpy(h + 36, "data", 4);
    put32(h + 40, w->bytes);
}

int Render_WavOpen(render_wav_t *w, const char *path, int is_float,
                   int channels, uint32_t rate)
{
    uint8_t h[44];

    memset(w, 0, sizeof(*w));
    w->is_float = is_float;
    w->channels = channels;
    w->rate = rate;
    w->fp = Render_Fopen(path, "wb");
    if (!w->fp)
        return 0;
    wav_header(h, w);                   /* sizes patched on close */
    return fwrite(h, 1, 44, w->fp) == 44;
}

int Render_WavWrite(render_wav_t *w, const void *data, uint32_t bytes)
{
    if (!w->fp || fwrite(data, 1, bytes, w->fp) != bytes)
        return 0;
    w->bytes += bytes;
    return 1;
}

int Render_WavClose(render_wav_t *w)
{
    uint8_t h[44];
    int ok;

    if (!w->fp)
        return 0;
    wav_header(h, w);
    ok = fseek(w->fp, 0, SEEK_SET) == 0 && fwrite(h, 1, 44, w->fp) == 44;
    ok = (fclose(w->fp) == 0) && ok;
    w->fp = NULL;
    return ok;
}


/* ---- paths: UTF-8 inside, the platform's own calls outside ------------
 * File names now carry the song and sample names (2026-10-18), so they
 * can hold any character IT's CP437 has. Windows needs the wide API for
 * that; the folder names the rest of it26 keeps (DirSample, the
 * Quicksave folder) are in the ANSI code page there. */
#ifdef _WIN32
static int utf8_to_wide(const char *s, wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0;
}
#endif

FILE *Render_Fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!utf8_to_wide(path, wp, 1024) || !utf8_to_wide(mode, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

int Render_Exists(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    return utf8_to_wide(path, wp, 1024) &&
           GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

int Render_MkDir(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    return utf8_to_wide(path, wp, 1024) && CreateDirectoryW(wp, NULL);
#else
    return mkdir(path, 0777) == 0;
#endif
}

void Render_DirFromNative(char *out, size_t n, const char *native)
{
#ifdef _WIN32
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_ACP, 0, native, -1, w, 1024) ||
        !WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL))
        snprintf(out, n, "%s", native);
#else
    snprintf(out, n, "%s", native);
#endif
}

void Render_PathJoin(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    int sep = l && (dir[l - 1] == '/' || dir[l - 1] == '\\');
    snprintf(out, n, "%s%s%s", dir, sep ? "" : "/", name);
}

/* decode one UTF-8 character; returns its length (1 on a bad byte) */
static int utf8_next(const unsigned char *s, uint32_t *u)
{
    if (s[0] < 0x80) { *u = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *u = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        *u = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *u = 0x10000;                   /* outside CP437 anyway */
        return 4;
    }
    *u = '?';
    return 1;
}

/* UTF-8 -> CP437 for the status line ('?' where CP437 has nothing) */
void Render_ToDisplay(char *out, size_t n, const char *utf8)
{
    const unsigned char *s = (const unsigned char *)utf8;
    size_t o = 0;

    while (*s && o + 1 < n) {
        uint32_t u;
        uint16_t c;
        s += utf8_next(s, &u);
        c = u >= 32 ? Screen_UnicodeToCP437(u) : 0;
        out[o++] = (char)(c ? c : '?');
    }
    out[o] = 0;
}

static size_t put_utf8(char *o, uint32_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) {
        o[0] = (char)(0xC0 | (u >> 6));
        o[1] = (char)(0x80 | (u & 0x3F));
        return 2;
    }
    o[0] = (char)(0xE0 | (u >> 12));
    o[1] = (char)(0x80 | ((u >> 6) & 0x3F));
    o[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

/* A name for the file system from IT text (CP437, up to `len` bytes, 0
 * ends it): characters as Unicode in UTF-8, \ / : * ? " < > | and
 * controls as '_', runs of spaces kept, leading/trailing spaces and
 * trailing dots dropped, at most RENDER_NAME_MAX bytes on a character
 * boundary. Empty when nothing is left. */
void Render_CleanName(char *out, size_t n, const char *cp437, size_t len)
{
    size_t o = 0, i;
    size_t cap = n - 1 < RENDER_NAME_MAX ? n - 1 : RENDER_NAME_MAX;

    for (i = 0; i < len && cp437[i]; i++) {
        uint8_t c = (uint8_t)cp437[i];
        uint32_t u = Screen_CP437ToUnicode(c);
        char tmp[4];
        size_t k;
        if (c < 32 || u < 32 || strchr("\\/:*?\"<>|", (int)c) ||
            (u >= 0x2500 && u <= 0x25FF))   /* box drawing / blocks */
            u = '_';
        if (o == 0 && u == ' ')
            continue;
        k = put_utf8(tmp, u);
        if (o + k > cap)
            break;
        memcpy(out + o, tmp, k);
        o += k;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.'))
        o--;
    out[o] = 0;
}

/* "2026-10-18 15.39.28" (no ':' -- Windows forbids it in names) */
void Render_Stamp(char *out20)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    if (lt)
        strftime(out20, 20, "%Y-%m-%d %H.%M.%S", lt);
    else
        snprintf(out20, 20, "0000-00-00 00.00.00");
}

/* `dir`/`base``ext` if free, else `base (2)``ext`, (3) ... */
void Render_Unique(char *out, size_t n, const char *dir, const char *base,
                   const char *ext)
{
    char name[RENDER_NAME_MAX + 32];
    int k;

    snprintf(name, sizeof(name), "%s%s", base, ext);
    Render_PathJoin(out, n, dir, name);
    for (k = 2; Render_Exists(out) && k < 1000; k++) {
        snprintf(name, sizeof(name), "%s (%d)%s", base, k, ext);
        Render_PathJoin(out, n, dir, name);
    }
}

/* delete a folder with everything in it (the selftest's clean-up) */
int Render_RemoveTree(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024], pat[1040];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!utf8_to_wide(path, wp, 1024))
        return 0;
    swprintf(pat, 1040, L"%ls\\*", wp);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t child[1100];
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            swprintf(child, 1100, L"%ls\\%ls", wp, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char c8[1100];
                if (WideCharToMultiByte(CP_UTF8, 0, child, -1, c8, sizeof(c8),
                                        NULL, NULL))
                    Render_RemoveTree(c8);
            } else {
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(wp) != 0;
#else
    DIR *d = opendir(path);
    struct dirent *e;
    if (d) {
        while ((e = readdir(d))) {
            char child[1100];
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode))
                Render_RemoveTree(child);
            else
                remove(child);
        }
        closedir(d);
    }
    return rmdir(path) == 0;
#endif
}

/* the sample as a WAV: mono, 8-bit unsigned or 16-bit signed (as
 * RIS_SaveWAV), the header at `rate` */
int Render_SaveSampleWav(const sample_t *s, const char *path, uint32_t rate)
{
    uint8_t h[44];
    int is16 = (s->Flags & 2) != 0;
    uint32_t bytes = s->Length << (is16 ? 1 : 0), i;
    FILE *f;
    int ok;

    memset(h, 0, 44);
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1;
    h[22] = 1;
    put32(h + 24, rate);
    put32(h + 28, rate * (is16 ? 2u : 1u));
    h[32] = (uint8_t)(is16 ? 2 : 1);
    h[34] = (uint8_t)(is16 ? 16 : 8);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);

    f = Render_Fopen(path, "wb");
    if (!f)
        return 0;
    ok = fwrite(h, 1, 44, f) == 44;
    if (ok && bytes && s->Data) {
        if (is16) {
            ok = fwrite(s->Data, 1, bytes, f) == bytes;
        } else {                        /* signed -> unsigned, a copy */
            const int8_t *d = (const int8_t *)s->Data;
            uint8_t buf[4096];
            for (i = 0; ok && i < bytes; ) {
                uint32_t k, m = bytes - i < 4096 ? bytes - i : 4096;
                for (k = 0; k < m; k++)
                    buf[k] = (uint8_t)(d[i + k] + 128);
                ok = fwrite(buf, 1, m, f) == m;
                i += m;
            }
        }
    }
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* ---- render loop ----------------------------------------------------- */

#define RENDER_CHUNK 4096

static void engine_lock(void)   { if (Engine_Lock) Engine_Lock(); }
static void engine_unlock(void) { if (Engine_Unlock) Engine_Unlock(); }

/* append `n` frames (stereo from the driver) to the job's output */
static int render_emit(render_job_t *job, render_wav_t *w,
                       const void *buf, uint32_t n, uint32_t *mem_cap)
{
    uint32_t i;

    if (job->to_memory) {
        const int16_t *s = (const int16_t *)buf;
        int16_t *d;
        if (job->frames + n > *mem_cap) {
            n = *mem_cap - job->frames;
            job->truncated = 1;
        }
        d = job->mem + job->frames;     /* IT samples are mono: the
                                           stereo mix is mixed down, as
                                           IT's "Both" for stereo WAVs */
        for (i = 0; i < n; i++)
            d[i] = (int16_t)(((int32_t)s[i * 2] + s[i * 2 + 1]) >> 1);
    } else if (job->channels == 2) {
        if (!Render_WavWrite(w, buf, n * 8))
            return 0;
    } else {
        static float mono[RENDER_CHUNK];
        const float *s = (const float *)buf;
        for (i = 0; i < n; i++)
            mono[i] = s[i * 2];
        if (!Render_WavWrite(w, mono, n * 4))
            return 0;
    }
    job->frames += n;
    return 1;
}

int Render_Run(render_job_t *job, const char *path)
{
    static uint8_t buf[RENDER_CHUNK * 8];   /* stereo float or int16 */
    render_wav_t w;
    uint32_t mem_cap = 0, cap, rate;
    int rows = job->rows, ok = 1;

    job->frames = 0;
    job->aborted = 0;
    job->truncated = 0;
    job->mem = NULL;
    job->channels = job->to_memory ? 1
                  : (Song.Header.Flags & ITF_STEREO) ? 2 : 1;

    engine_lock();
    Music_Stop();
    WAVDriver_SetMixSpeed(job->to_memory ? 44100 : job->rate);
    rate = WAVDriver_GetMixSpeed();
    WAVDriver_SetOutputFormat(job->to_memory ? 0 : 5);
    WAVDriver_SetOutputChannels(2);
    WAVDriver_SetForceMono(0);
    WAVDriver_SetSBFilter(0);
    WAVDriver_SetSBFeedback(0);
    WAVDriver_SetStartRamp(1);
    Driver->InitSound();
    Music_ResetRNG();
    Music_InitStereo();
    Music_InitTempo();
    engine_unlock();
    job->out_rate = rate;

    if (job->to_memory) {
        mem_cap = job->mem_cap_bytes / (uint32_t)(2 * job->channels);
        job->mem = (int16_t *)malloc((size_t)mem_cap * 2u *
                                     (size_t)job->channels + 2);
        if (!job->mem)
            return 0;
    } else if (!Render_WavOpen(&w, path, 1, job->channels, rate)) {
        return 0;
    }

    if (rows <= 0)
        rows = 64;
    if (rows > 200)
        rows = 200;
    cap = rate * 60u * 30u;                 /* itplay -w's runaway cap */

    engine_lock();
    StopEndOfPlaySection = 1;
    if (job->kind == RENDER_SONG)
        Music_PlaySong(0);
    else
        Music_PlayPattern((uint16_t)job->pattern, (uint16_t)rows, 0);
    engine_unlock();

    for (;;) {
        uint32_t pend;

        if (job->abort_cb && job->abort_cb()) {
            job->aborted = 1;
            break;
        }
        /* one frame forces the next tick; a tick in which the engine
         * stopped (the tick after the last row) is not part of it */
        WAVDriver_RenderAny(buf, 1);
        if (PlayMode == 0)
            break;
        if (!render_emit(job, &w, buf, 1, &mem_cap)) { ok = 0; break; }
        pend = WAVDriver_Pending();
        while (pend) {
            uint32_t n = pend > RENDER_CHUNK ? RENDER_CHUNK : pend;
            WAVDriver_RenderAny(buf, n);
            if (!render_emit(job, &w, buf, n, &mem_cap)) { ok = 0; break; }
            pend -= n;
        }
        if (!ok || job->truncated || job->frames >= cap)
            break;
    }

    engine_lock();
    StopEndOfPlaySection = 0;
    Music_Stop();
    engine_unlock();

    if (!job->to_memory && !Render_WavClose(&w))
        ok = 0;
    return ok;
}

/* ---- Esa's names ----------------------------------------------------- */

static uint16_t RenderCounter = 0;          /* WAV_RenderCounter */

void Render_ResetCounter(void) { RenderCounter = 0; }

void Render_ClockHHMMSS(char *out7)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    snprintf(out7, 7, "%02d%02d%02d", lt ? lt->tm_hour : 0,
             lt ? lt->tm_min : 0, lt ? lt->tm_sec : 0);
}

/* WAV_BuildTimestampBasename: static "LL" + HHMMSS */
void Render_NameTimestamp(char *out)
{
    char hms[7];
    Render_ClockHHMMSS(hms);
    snprintf(out, 16, "LL%s.WAV", hms);
}

/* WAV_BuildCounterName: first 3 chars of the song name upper-cased,
 * non-alphanumeric -> 'X', a 0 / space before 3 chars pads with 'X',
 * an empty name (byte 0 is 0 or space) -> "PTN"; then the counter,
 * incremented before use, as 4 digits */
void Render_NameCounter(char *out, const char *songname)
{
    char pfx[4] = "PTN";
    int i;

    if (songname && songname[0] && songname[0] != ' ') {
        for (i = 0; i < 3; i++) {
            char c = songname[i];
            if (c == 0 || c == ' ') {
                for (; i < 3; i++)
                    pfx[i] = 'X';
                break;
            }
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')))
                c = 'X';
            pfx[i] = c;
        }
    }
    RenderCounter++;
    snprintf(out, 16, "%s%04u.WAV", pfx, (unsigned)(RenderCounter % 10000));
}

/* D_DumpBuildRunPrefix + D_DumpBuildName: HHMMSS + 2-digit slot */
void Render_NameSampleExport(char *out, const char *hhmmss, int slot1)
{
    snprintf(out, 16, "%.6s%02d.WAV", hhmmss, slot1 % 100);
}

/* split a typed 8.3 name into upper-cased base (<= 8) and extension */
static void split83(const char *typed, char *base, char *ext)
{
    const char *p = typed, *dot;
    int i;

    for (dot = typed; *dot; dot++)          /* strip a path */
        if (*dot == '/' || *dot == '\\' || *dot == ':')
            p = dot + 1;
    dot = strrchr(p, '.');
    for (i = 0; p[i] && p + i != dot && i < 8; i++)
        base[i] = (char)toupper((unsigned char)p[i]);
    base[i] = 0;
    ext[0] = 0;
    if (dot && dot[1]) {
        for (i = 0; dot[1 + i] && i < 3; i++)
            ext[i] = (char)toupper((unsigned char)dot[1 + i]);
        ext[i] = 0;
    } else {
        strcpy(ext, "WAV");
    }
}

/* F10 WAV: the typed name wins ("song.wav" -> SONG.WAV) */
void Render_NameUser(char *out, const char *typed)
{
    char base[9], ext[4];
    split83(typed, base, ext);
    snprintf(out, 16, "%s.%s", base[0] ? base : "SONG", ext);
}

/* WAV_BuildChannelFilename: base cut to 6 + 2-digit channel (1-based) */
void Render_NameChannel(char *out, const char *typed, int chan1)
{
    char base[9], ext[4];
    split83(typed, base, ext);
    base[6] = 0;
    snprintf(out, 16, "%s%02d.%s", base[0] ? base : "SONG", chan1 % 100,
             ext);
}
