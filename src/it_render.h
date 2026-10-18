/*
 * it_render.h
 * -----------
 * Offline renders of a pattern or the whole song through the ported engine
 * and WAV driver (feature 018, after Esa Ruoho's IT 2.354 fork): the render
 * loop, the WAV writer, and Esa's file names. No UI here -- the editor and
 * the batch options call it.
 */
#ifndef IT_RENDER_H
#define IT_RENDER_H

#include <stdint.h>
#include <stdio.h>

enum { RENDER_PATTERN = 0, RENDER_SONG = 1 };

typedef struct render_job_t {
    /* in */
    int      kind;          /* RENDER_PATTERN / RENDER_SONG              */
    int      pattern;       /* 0..199 (pattern jobs)                     */
    int      rows;          /* header rows; 0 -> 64, > 200 -> 200        */
    int      to_memory;     /* 1: 44.1 kHz 16-bit into mem (import);
                               0: WAV file, 32-bit float unclamped       */
    uint32_t rate;          /* file jobs: the mixing rate                */
    uint32_t mem_cap_bytes; /* memory jobs: data cap (1 MB for imports)  */
    int    (*abort_cb)(void); /* nonzero = stop (Esc, a second gesture)  */
    /* out */
    int      channels;      /* files: 2 when the song plays stereo, else
                               1; memory: always 1 (IT samples are mono) */
    uint32_t out_rate;      /* rate actually mixed at                    */
    uint32_t frames;        /* frames written                            */
    int      aborted;       /* stopped by abort_cb                       */
    int      truncated;     /* memory job hit mem_cap_bytes              */
    int16_t *mem;           /* memory jobs: mono frames (malloc)         */
} render_job_t;

/* Render `job` into the WAV file `path` (to_memory = 0) or into job->mem
 * (to_memory = 1, path NULL). The caller has silenced the live output and
 * restores the driver settings afterwards. Returns 1 on success, 0 when
 * the file cannot be written or memory runs out. */
int Render_Run(render_job_t *job, const char *path);

/* Esa's names (IT_MUSIC.ASM / IT_DISK.ASM, see research R7); `out` gets
 * at least 16 bytes. */
void Render_NameTimestamp(char *out);                 /* LL163422.WAV  */
void Render_NameCounter(char *out, const char *songname); /* TWI0001.WAV */
void Render_NameSampleExport(char *out, const char *hhmmss, int slot1);
void Render_NameUser(char *out, const char *typed);   /* song.wav -> SONG.WAV */
void Render_NameChannel(char *out, const char *typed, int chan1); /* SONG03.WAV */
void Render_ClockHHMMSS(char *out7);                  /* "163422"      */
void Render_ResetCounter(void);                       /* selftest      */

/* minimal WAV writer: 16-bit PCM (fmt 1) or 32-bit float (fmt 3) */
typedef struct render_wav_t {
    FILE    *fp;
    int      is_float, channels;
    uint32_t rate, bytes;
} render_wav_t;
int  Render_WavOpen(render_wav_t *w, const char *path, int is_float,
                    int channels, uint32_t rate);
int  Render_WavWrite(render_wav_t *w, const void *data, uint32_t bytes);
int  Render_WavClose(render_wav_t *w);

#endif
