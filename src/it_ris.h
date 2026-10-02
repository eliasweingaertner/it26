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
 * SampleFormatNames: 2=IT, 3=S3M, 5=WAV 8-bit, 7=WAV 16-bit, 8=XM,
 * 9=PTM, 10=MTM, 11=669, 12=FAR, 13=TX Wave, 14=MOD, 15=KRZ, 16=PAT,
 * 17=AIFF (IFF 8SVX/16SV; feature 013). */
typedef struct slibent_t {
    sample_t hdr;
    uint32_t FileSize;                  /* record +50h */
    uint16_t Date, Time;                /* record +54h/+56h, DOS-packed
                                           (feature 015) */
    uint8_t  Format;                    /* record +58h: 0 unchecked, 1 dir,
                                           4 unknown, >= 20h module */
    uint8_t  SortPri;                   /* record +5Ah: 0 dir, 1 library,
                                           2 recognised, 3 unknown */
    char     SrcFile[264];
} slibent_t;

/* D_LoadSampleFiles + D_GetSampleInfo + D_SlowSampleSort (feature 015):
 * list `dir` as the Load Sample screen shows it -- directories first as
 * dotted "Directory" rows (the lone "." becomes "\", the root), then
 * every file, identified (single samples get their header, modules
 * become dotted "Library" rows with type 20h+), then sorted with "\"
 * and ".." pinned, by SortPri, then by filename bytes. Returns the
 * number of entries (the original caps at 620), -1 if unreadable. */
int RIS_ListDirectory(const char *dir, slibent_t *ents, int max);

/* Scan a module / .KRZ / .PAT / .ITS / .WAV file into sample records.
 * Returns the number of entries, or -1 if the file is unreadable or
 * not a supported sample source. */
int RIS_ScanModule(const char *path, slibent_t *ents, int max);

/* LoadSample port: read + convert the entry's data (Load_SampleData)
 * and place header + data into *dst (a Song.Smp slot; the caller
 * holds the engine lock and has stopped playback). Frees dst->Data.
 * Returns 1 on success. */
int RIS_LoadSample(const slibent_t *e, sample_t *dst);

const char *RIS_FormatName(uint8_t fmt);

/* extension filter for the requester (modules + KRZ/PAT/ITS/WAV) */
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
    /* #27: the Load Instrument list's columns (record +1 file name,
     * +42 file size in k) */
    char     FileName[13];
    uint16_t SizeK;
} ilibent_t;

int RI_ScanModule(const char *path, ilibent_t *ents, int max);

/* #27: D_LoadInstrumentFiles + D_GetInstrumentInfo + D_SlowInstrumentSort
 * for the Load Instrument screen. Formats as the original's record byte
 * 0: 1 = directory ("\" for the root entry, ".."), 3 = .ITI, 4 = .XI,
 * 8 = .IT module in instrument mode, 9 = .XM module; anything else is
 * not listed (the original drops unrecognised files). Order: "\" and
 * ".." first, then directories, modules, instrument files, each by file
 * name. Returns the count, -1 if the directory can't be read. */
int RI_ListDirectory(const char *dir, ilibent_t *ents, int max);
/* one file as RI_ListDirectory would list it; returns its format, 0 if
 * it is not an instrument source */
int RI_IdentifyFile(const char *path, ilibent_t *e);

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
