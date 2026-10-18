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

#include "it_music.h"
#include "it_pattern.h"
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
    w->fp = fopen(path, "wb");
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
