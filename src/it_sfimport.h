/*
 * it_sfimport.h
 * -------------
 * it26 extension (feature 020): SFZ and SoundFont 2 instruments on the
 * Load Instrument screen. Not part of IT 2.14/2.17; editor-side only,
 * the engine is untouched. Conversion rules: specs/020-sfz-sf2-import/
 * research.md.
 */
#ifndef IT_SFIMPORT_H
#define IT_SFIMPORT_H

#include <stddef.h>
#include <stdint.h>
#include "it_structs.h"

struct ilibent_t;

/* what the last load did, for the status line */
typedef struct sfi_report_t {
    int  needed;        /* IT samples the instrument wanted             */
    int  loaded;        /* IT samples created (after reduce-to-fit)     */
    int  skipped;       /* samples whose audio could not be read        */
    char why[80];       /* first reason                                 */
} sfi_report_t;

/* "RIFF" <size> "sfbk" */
int SFI_IsSF2(const uint8_t *head, size_t n);

/* the presets of an .sf2 as Load Instrument rows (format
 * RI_FMT_SF2PRESET, Offset = preset index), sorted by bank/program;
 * -1 if the file is not a readable SoundFont */
int SFI_ScanSF2(const char *path, struct ilibent_t *ents, int max);

/* samples an .sfz needs (>= 0) and its name (file stem); -1 unreadable */
int SFI_CountSFZ(const char *path, char name[27]);

/* convert an SFZ file / SF2 preset into an instrument and at most
 * `maxsmp` samples (sh[] receive loaded data, Flags/C5Speed/loops set;
 * ins->NoteSampleTable references them 1-based). Returns the number of
 * samples (>= 1), or -1 when nothing usable could be loaded (rep->why).
 * Touches no song data. */
int SFI_Build(const struct ilibent_t *e, int maxsmp, instrument_t *ins,
              sample_t *sh, sfi_report_t *rep);

/* decoded sample file (16-bit interleaved) */
typedef struct sfi_pcm_t {
    int16_t *pcm;               /* malloc'd                              */
    uint64_t frames;
    int      ch;
    uint32_t rate;
    int      loopmode;          /* 0 none, 1 forward, 3 ping-pong (smpl) */
    int64_t  ls, le;            /* le exclusive                          */
    int      unity;             /* smpl unity note, -1 unknown           */
    int      aiff;              /* AIFF container (loops: caller)        */
} sfi_pcm_t;

/* WAV / AIFF (dr_wav) or FLAC (dr_flac). Implemented in it_editor.c,
 * the translation unit that holds miniaudio's implementation (its
 * decoders are declared only there). 1 = decoded, 0 = not a format
 * it reads, -1 = empty / too long / out of memory. */
int SFI_DecodeAudio(const char *path, sfi_pcm_t *out, uint64_t maxframes);

/* ited --sfi-test: load every .sfz / .sf2 preset below the paths into
 * an emptied song, one line per item, then a summary (stdout) */
int SFI_BatchTest(int n, const char *const *paths);

#endif /* IT_SFIMPORT_H */
