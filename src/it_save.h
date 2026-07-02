/*
 * it_save.h
 * ---------
 * .IT module writer, ported from IT_D_WM.INC (D_SaveIT) and IT_DISK.ASM
 * (WriteBits, D_SaveSampleData / D_SaveSampleDataCompressed, the
 * bit-width LUT setup from D_SaveIT, D_SaveBlock, D_DeleteIfError).
 */

#ifndef IT_SAVE_H
#define IT_SAVE_H

#include <stdint.h>
#include <time.h>

/* set on module load / new song; feeds the header's obfuscated
 * cumulative-edit-time counter on save (18.2 Hz ticks). */
extern time_t Save_LoadTime;

/* SaveFormat (IT_DISK.ASM 420, default SWITCH.INC DEFAULTFORMAT = 3):
 *   0 = IT 2.14 (compressed),  2 = IT 2.xx (uncompressed),
 *   3 = IT 2.15 (compressed, double delta).  1 = S3M (not ported). */
extern uint8_t SaveFormat;

/* Song message (IT_MSG.ASM MessageData): 8000 bytes, CR-separated
 * lines, NUL-terminated. Owned here so the loader and the writer can
 * see it without linking the editor. */
#define IT_MESSAGELENGTH 8000
extern char IT_MessageData[IT_MESSAGELENGTH];

/* Msg_GetMessageLength: strlen + 1 (1 = empty, not saved) */
uint16_t Msg_GetMessageLength(void);

/* Optional progress hook (the original draws "File Header",
 * "Instrument Headers", ... at rows 17..23 of the save screen).
 * stage: 0 header, 1 instrument headers, 2 sample headers,
 * 3 pattern n, 4 sample n, 5 done. */
extern void (*Save_Progress)(int stage, int param);

/* D_SaveIT: write the current Song (+ message, + MIDI config) to
 * `path`. Returns 1 on success; on any write error the partial file is
 * deleted and 0 is returned. Never modifies the in-memory song. */
int Save_ITModule(const char *path);

#endif /* IT_SAVE_H */
