/*
 * it_ris.h
 * --------
 * Sample / instrument library ("rip from other modules"), ported from
 * the original editor's IT_D_RIS.INC ("Read Instrument Sample") and
 * IT_D_RI.INC ("Read Instrument") plus the record/transfer machinery
 * in IT_DISK.ASM (LSWindow_Enter / LIWindow_Enter / LoadSample).
 * See specs/006-cross-module-sample-load/research.md.
 */

#ifndef IT_RIS_H
#define IT_RIS_H

#include "it_music.h"

/* ---- sample library ----------------------------------------------
 * One entry mirrors a 96-byte DiskDataArea record: an ITS header
 * (hdr; hdr.OffsetInFile = offset of the data inside SrcFile) plus
 * file size, format code and the source path. Format codes follow
 * SampleFormatNames: 2=IT, 3=S3M, 8=XM, 9=PTM, 10=MTM, 11=669,
 * 12=FAR, 14=MOD, 15=KRZ, 16=PAT. */
typedef struct slibent_t {
    sample_t hdr;
    uint32_t FileSize;                  /* record +50h */
    uint8_t  Format;                    /* record +58h */
    char     SrcFile[264];
} slibent_t;

/* Scan a module / .KRZ / .PAT / .ITS file into sample records.
 * Returns the number of entries, or -1 if the file is unreadable or
 * not a supported sample source. */
int RIS_ScanModule(const char *path, slibent_t *ents, int max);

/* LoadSample port: read + convert the entry's data (Load_SampleData)
 * and place header + data into *dst (a Song.Smp slot; the caller
 * holds the engine lock and has stopped playback). Frees dst->Data.
 * Returns 1 on success. */
int RIS_LoadSample(const slibent_t *e, sample_t *dst);

const char *RIS_FormatName(uint8_t fmt);

/* extension filter for the requester (modules + KRZ/PAT/ITS) */
int RIS_KnownExt(const char *name);

/* ---- instrument library ------------------------------------------
 * 48-byte record equivalent. Formats (InstrumentLoaderTable codes):
 * 3=.ITI file, 4=.XI file, 5=instrument in .IT, 6=instrument in .XM */
typedef struct ilibent_t {
    uint8_t  Format;
    char     Name[27];
    uint16_t NumSamples;
    uint32_t Offset;                    /* record +44 */
    char     SrcFile[264];
} ilibent_t;

int RI_ScanModule(const char *path, ilibent_t *ents, int max);

/* 99 - (sample slots with data), as D_InitLoadInstruments computes
 * when the requester opens. */
int RI_UnusedSamples(void);

/* LIWindow_Enter transfer: release samples exclusive to the target
 * instrument, load the instrument + its samples into free slots with
 * the note table remapped. Caller checks RI_UnusedSamples() against
 * e->NumSamples first (as the original does), stops playback and
 * holds the engine lock. */
#define RI_OK        0
#define RI_ERR_OPEN  (-1)
#define RI_ERR_SLOTS (-2)
int RI_LoadInstrument(const ilibent_t *e, int target /* 0-based */);

const char *RI_FormatName(uint8_t fmt);
int RI_KnownExt(const char *name);

/* ---- disk saves (D_SaveSample / SaveITI, deferred from 005) ------ */
int RIS_SaveITS(const sample_t *s, const char *path);
int RIS_SaveWAV(const sample_t *s, const char *path);
int RIS_SaveST(const sample_t *s, const char *path);
int RI_SaveITI(const instrument_t *in, int insnum1 /*1-based*/,
               const char *path);

#endif /* IT_RIS_H */
