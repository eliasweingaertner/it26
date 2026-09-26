# Phase 0 Research: Authentic Load Sample Screen

**Feature**: 015-load-sample-screen
**Date**: 2026-09-26

ASM decode contract. Line references are into `fable5\impulsetracker`.

---

## R1 — Screen definition (`O1_LoadSampleList`, `IT_OBJ1.ASM:952`)

Object list, default focus object 15 (the list). Idle function
`SampleNameLoader` (`D_LoadSampleNames`), key list `LoadSampleKeyList`.

| Object | Kind | Geometry / data | Line |
|---|---|---|---|
| LoadSampleHeader | header text | "Load Sample" | 1119 |
| LoadSampleBox | box style 27 | (5,12)-(44,48) | 1125 |
| DriveSampleBox | box 27 | (45,12)-(54,23) | 1129 |
| LSInfoBox | box 27 | (63,12)-(77,23) | 1133 |
| LSInfoText | text 20h at (55,13) | Filename…Length, 10 lines, **no divider line** (unlike F3) | 1137 |
| LSWaveFormBox | box 27 | (45,24)-(77,29) | 1152 |
| LSParametersBox | box 9 | (45,30)-(77,42) | 1156 |
| LSParametersText | text 20h at (48,33) | Default Volume / Global Volume / (2 blank) / Vibrato Speed / Depth / Rate | 1160 |
| LSParametersVolBox | box 25 | (62,32)-(72,35) | 1172 |
| LSParametersVibBox | box 25 | (62,36)-(72,40) | 1176 |
| LSFileInfoBox | box 27 | (52,43)-(77,48) | 1180 |
| LSFileInfoText | text 20h at (46,44) | Format / Size / Date / Time | 1184 |
| LoadSampleWindow | custom (15) | D_Draw/Pre/PostLoadSampleWindow | 1193 |
| LSDriveWindow | custom (16) | D_LSDraw/Pre/PostDriveWindow | 1203 |
| LSFileNameInput | string (16) | (64,13), 13 chars, rec +4 | 1025 |
| LSSpeedInput | 5num (18) | (64,14), rec +3Ch | 1032 |
| LSLoopToggle | toggle (17) | (64,15), rec +12h bit 16 | 1038 |
| LSLoopBegin/EndInput | 5num | (64,16)/(64,17), +34h/+38h, `D_LSCheckLoopValues` | 1044 |
| LSSusLoopToggle | toggle | (64,18), +12h bit 32 | 1056 |
| LSSusLoopBegin/EndInput | 5num | (64,19)/(64,20), +40h/+44h, `D_LSCheckSusLoopValues` | 1062 |
| LSDefaultVolumeInput | thumb (9) | (63,33), 0..64, +13h | 1074 |
| LSGlobalVolumeInput | thumb | (63,34), 0..64, +11h | 1081 |
| LSVibratoSpeedInput | thumb | (63,37), 0..64, +4Ch | 1088 |
| LSVibratoDepthInput | scalable thumb (14) | (63,38), 0..32, +4Dh | 1095 |
| LSVibratoRateInput | scalable thumb | (63,39), 0..255, +4Eh | 1103 |

Note: Quality is at row **21** and Length at row **22** on this screen
(`D_DrawLoadSampleWindow`, `(21*80+64)`), one row higher than F3, because
there is no divider line.

**Decision**: transliterate this table into a static object list, same
approach as the F3/F4 ports (features 002/005).

## R2 — The list record (96 bytes, `D_LoadSampleFiles`, `IT_DISK.ASM`)

| Offset | Content |
|---|---|
| +00h..+4Fh | ITS sample header (80 bytes). For files: filled by identification. |
| +04h | DOS filename, 13 bytes (the header's DOSFileName field reused) |
| +14h | Sample name, 25 shown. For directories: `DirectoryMsg` = 8×char 154, "Directory", 8×154 — the dotted look |
| +50h (80) | file size, dword |
| +54h (84) | DOS date |
| +56h (86) | DOS time |
| +58h (88) | type: 0 unidentified, 1 dir, 2 IT sample, 3 ST3 sample, 4 unknown, 5/7 WAV, … ≥20h module (low byte − 20h = module format) |
| +5Ah (90) | sort priority: 0 dir, 1 library(module), 2 recognised sample, 3 unknown |

**Decision**: the port's `slibent_t` (`it_ris.h`) already mirrors +00..+58h.
Extend it with date/time, type and priority rather than inventing a parallel
record.

## R3 — Listing, identification, sort

- `D_LoadSampleFiles`: FindFirst `*.*` directories first (attribute 10h), with
  the lone `.` entry rewritten to `\` (root); then **all** files, no extension
  filter. Cap 620 entries.
- Files start as type 0 (colour 6, "Unchecked"). `D_LoadSampleNames`, the idle
  function, identifies one entry per idle tick via `D_LoadSampleHeader` +
  `D_GetSampleInfo` (`IT_D_INF.INC:353`), skipping while a key is held.
  The highlighted entry is identified immediately on draw.
- After the last entry is identified, `D_SlowSampleSort` runs **only if the
  cursor is still on row 0**: `\` and `..` stay pinned on top, the rest sorted by
  priority (+5Ah) then filename bytes.

**Decision**: identify all entries synchronously on directory entry (modern
disks make the idle loader's purpose moot), then apply the same sort
unconditionally. **Deviation**: the original's list order is visibly unsorted
for the first moments and stays unsorted if you move before identification
finishes; ours is always sorted. Recorded in README.

**Alternative rejected**: porting the idle loader tick for tick. It would
reproduce a DOS-speed artefact and make the screen's content depend on timing,
which the selftest cannot pin down.

## R4 — CACHE.ITS

`D_InitLoadSamples` reads/writes `CACHE.ITS` in each browsed directory (the
96-byte records plus a count), invalidated by directory date/time, and deletes
ST3's cache file.

**Decision**: not ported. Writing files into users' sample folders is
unwelcome on a modern system and the cache only saved floppy/HDD seek time.
**Deviation**, recorded. Consequence: a `CACHE.ITS`/`CACHE.ITI` left by real IT
shows up as an ordinary unknown file, exactly as it does in IT's own list
(the DOSBox screenshot shows `CACHE.ITI`).

## R5 — Drawing (`D_DrawLoadSampleWindow`, `IT_DISK.ASM:5260`)

- Empty listing: "No files." at (6,13), colour 5.
- Scroll window 35 rows (13..47). Row numbers at column 2 via `PE_ConvAX2Num`
  in colour 20h, 3 digits.
- Divider: char 0A8h colour 2 (`2A8h`) at column 31, rows 13..47.
- Name column at 6, 25 chars; characters ≥ 226 replaced by space. Filename
  column at 32, up to 12 chars.
- Row colour by type: 0 → 6, 1 (dir) → 5, 4 (unknown) → 2, else 3.
- Cursor row (`D_PreLoadSampleWindow`): 38 cells from column 6 set to attr 30h,
  except the 13th-from-last (the divider cell) which gets 32h.
- Quality at (64,21): "8 Bit"/"16 Bit"/"8 Bit Stereo"/"16 Bit Stereo" by flags
  +12h bit 2 and Cvt +2Eh bit 32, colour 2 — note these are the `IT_DISK.ASM`
  strings ("8 Bit", not F3's "8 bits"). Length at (64,22).
- Format at (53,44) colour 5 from `SampleFormatNames` indexed by type.
- Size at (53,45): 9 digits zero-padded, colour 5, only when < 10000×65536.
- Date at (53,46): `MonthName D, YYYY`, colour 5. Time at (53,47):
  12-hour, no leading zero on the hour, `h:mm` + `am`/`pm`.
- Waveform: drawn **only** when `CurrentSample == SampleInMemory`, i.e. after the
  user auditioned that entry; 31×4 cells at (46,25) using generated font-B
  characters 0..123, attr 0Dh.

## R6 — Keys

`LSWindowKeys`/`LSViewWindowKeys` (`IT_DISK.ASM:917`):
Enter → `LSWindow_Enter`; Space → rename (`LSWindow_Space`); Up/Down/PgUp/PgDn/
Home/End; Right or Tab → drive window; Delete → `D_DeleteSampleFile`
(confirmation `O1_ConfirmDelete2`, default Cancel). Any note key
(`D_PostLoadSampleWindow`) loads the entry into slot 99 and plays it — this is
what sets `SampleInMemory` and makes the waveform appear. The stereo prompt is
suppressed during audition (`DisableStereoMenu`).

Drive window: 10 rows from (46,13), "Drive X:" colour 5, cursor attr 30h over 8
cells.

## R7 — Enter

`LSWindow_Enter` (`IT_DISK.ASM:7173`): type 1 → change directory and re-list,
cursor 0; type ≥ 20h → list that module's samples in the same screen
(`LSWindow_EnterLoadInSampleData`), first row the library exit; otherwise
`LSWindow_EnterSample` (already ported, host-instrument prompt, no overwrite
prompt).

## R8 — Edits (resolves the spec's open assumption)

`SetLoadSample5Num` (`IT_F.ASM`) writes through `D_GetLoadSampleVars`
(`IT_DISK.ASM:6347`) = `DiskDataArea + 96·CurrentSample`: **into the listing
record**. `LoadSample` reads the same record. So edits apply on load **and
persist on the entry while the listing lives**. `CheckDataArea` is a snapshot
used only to restore Cvt/DfP (+2Eh) after an audition.

The spec's FR-010 originally said edits were discarded on highlight change; it
has been corrected.

## R8b — Correction: edits make Load Sample a sample-file editor

Found during implementation (T013), in `CheckSampleModified`
(`IT_DISK.ASM`), which every cursor move (`LSWindow_Up/Down/PgUp/PgDn/
Home/End`) calls first. If the highlighted entry is a sample (type 2..1Fh)
whose record differs from the `CheckDataArea` snapshot:

- filename unchanged -> "Save sample?" (`O1_ConfirmResaveList`, default OK):
  load the entry into slot 99 and write it back with `D_SaveSampleInternal`;
- filename changed (LSFileNameInput) -> "Save/Rename sample?"
  (`O1_ConfirmSaveRenameList`): save under the new name, delete the old file;
- declined -> "Discard changes?" (`O1_ConfirmDiscardList`, default OK):
  OK restores the 80-byte header from the snapshot; Cancel aborts the move.

`D_SaveSampleInternal` always writes **ITS** (header + `Cvt=1`, data pointer
80) under the record's filename -- an edited `.WAV` is overwritten with ITS
data, name unchanged. Edits therefore do NOT simply persist on the entry
(the R8 reading and the first FR-010 correction were incomplete). Whether to
port the in-place ITS overwrite of non-ITS files as-is is a user decision,
taken before US3.

PgUp/PgDn step 35 rows; End = last entry. Drive window keys: Up/Down,
Enter (change drive), Shift-Tab/Left -> list (object 15), Tab/Right -> the
filename input (object 17).

## R9 — Sample Library, Ctrl-F3 (`O1_ViewSampleLibrary`)

Same objects, header "Sample Library (Ctrl-F3)", `LSViewWindowKeys` (no Enter
→ load; Enter only navigates). The port's `draw_lib_browser` covers the in-module
part of this today with an eyeballed layout; it will be replaced by the shared
screen.

## R10 — Platform adaptation

- Drives: Windows → available drive letters (`GetLogicalDrives`); POSIX → a
  single `/` entry. Documented adaptation, not a behaviour change.
- Filenames: host names longer than 12 shown truncated; the full path is kept in
  the entry for loading.
- Date/time: host modification time converted to local time.
