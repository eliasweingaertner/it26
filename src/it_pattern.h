/*
 * it_pattern.h
 * ------------
 * Editor-side unpacked pattern model. The engine reads patterns in the
 * packed on-disk format (see it_load.c / UpdateNoteData); the editor
 * works on an unpacked grid of cells and re-packs into the format the
 * player consumes whenever a pattern is modified.
 *
 * The pack/unpack here is the exact inverse of the player's decoder in
 * it_music.c (mask bits 1/2/4/8 = read note/instrument/volume/command;
 * an empty cell simply emits nothing for that channel, and the player
 * retains the channel's previous state, which is the IT semantics).
 */

#ifndef IT_PATTERN_H
#define IT_PATTERN_H

#include <stdint.h>
#include "it_music.h"

/* Grid note encoding (editor space):
 *   0          empty
 *   1..120     notes C-0..B-9   (engine note value = grid - 1)
 *   253/254/255 fade / cut / off (engine value identical)            */
#define GNOTE_EMPTY 0
#define GNOTE_FADE  253
#define GNOTE_CUT   254
#define GNOTE_OFF   255

#define GVOL_EMPTY  255

/* A cell mirrors IT's pattern model: a `mask` says which of the four
 * fields are present (the player decodes from exactly these bits), and
 * the value bytes are meaningful only when the corresponding mask bit is
 * set. This avoids conflating "field absent" with "field value 0/255". */
#define CM_NOTE 0x01
#define CM_INS  0x02
#define CM_VOL  0x04
#define CM_CMD  0x08

typedef struct editcell_t {
    uint8_t mask;     /* CM_* present-field bits          */
    uint8_t note;     /* grid note encoding above (if CM_NOTE)     */
    uint8_t ins;      /* instrument/sample number (if CM_INS)      */
    uint8_t vol;      /* volume-column byte (if CM_VOL)            */
    uint8_t cmd;      /* 1..26 (A..Z) (if CM_CMD)        */
    uint8_t cmdval;   /* effect parameter                */
} editcell_t;

#define MAX_PATROWS 256

/* Unpack pattern `patnum` into `grid` (rows*64 cells, row-major: cell
 * for (row, channel) at grid[row*64 + channel]). Returns the row count
 * (0 if the pattern is empty/absent, in which case grid is cleared to
 * 64 empty rows). */
int  Pattern_Unpack(uint16_t patnum, editcell_t *grid);

/* Re-pack `grid` (rows*64 cells) into the packed stream stored in
 * Song.Patterns[patnum], replacing any previous data. Allocates the
 * pattern slot if needed. Returns 1 on success. */
int  Pattern_Pack(uint16_t patnum, const editcell_t *grid, uint16_t rows);

/* Ensure pattern `patnum` exists with `rows` rows (creates an empty
 * pattern if absent). Returns the row count. */
uint16_t Pattern_EnsureExists(uint16_t patnum, uint16_t rows);

/* Engine-lock hooks (set by the editor to serialise pattern edits
 * against the audio thread's per-tick Update()). Either may be NULL. */
extern void (*Engine_Lock)(void);
extern void (*Engine_Unlock)(void);

#endif /* IT_PATTERN_H */
