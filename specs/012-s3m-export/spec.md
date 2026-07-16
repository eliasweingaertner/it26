# Feature Specification: S3M Export (SaveFormat 1)

**Feature Branch**: `012-s3m-export`

**Created**: 2026-07-11

**Status**: Draft

**Input**: User description: "Port the original's S3M module writer
(D_SaveS3M, IT_D_WM.INC 753) so SaveFormat 1 works: the F10 save screen's
format buttons (IT214/S3M/IT2xx/IT215), the .S3M extension handling, the
save itself with all of the original's format-limit warnings, and Ctrl-S
extension replacement."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Save the song as a Scream Tracker 3 module (Priority: P1)

The user selects the S3M format on the F10 save screen and saves. The port
writes a byte-faithful `D_SaveS3M` file: ST3 header (name, 0x1A/16 marker,
counts, flags, Cwt 0x3217, Ffi 2, "SCRM", GV/speed/tempo/MV+stereo,
dp=252), order list with the >=100 filter and 0xFF terminator, S3M channel
settings (alternating L/R types, 0xFF for muted), the 32-byte default-pan
block, 16-byte-aligned 0x50-byte sample headers ("SCRS", 24-bit memseg
patched after the data pass), 16-byte-aligned translated 64-row patterns
(mask/note/ins/vol/effect per the original's translation incl. octave
shift, S91 -> XA4, V/X halving, C decimal encoding), and unsigned-converted
sample data. Loading the file back (the port's own S3M importer) plays.

**Why this priority**: The whole feature; the last missing save format.

**Independent Test**: Selftest: save a module as S3M, verify header bytes,
re-import it, assert engine-visible fields and sample-data byte equality.

**Acceptance Scenarios**:

1. **Given** itdemo.it, **When** saved with SaveFormat 1, **Then** the file
   carries "SCRM"/"SCRS" magics, the clamped counts, and the port's S3M
   importer loads it back with matching tempo/speed/global volume, orders,
   the first 16 channels' pattern data and byte-identical sample data.
2. **Given** content S3M cannot express (patterns not 64 rows, >100
   patterns, notes outside C-1..B-8, data on channels 17+, vol-column
   effects/pans, sustain/bidi loops, sample GvL != 64, sample vibrato,
   channel volumes != 64, linear slides, instrument mode), **Then** the
   original's exact warning messages appear on the save screen (same rows)
   and the save proceeds with the original's lossy behaviour (cells
   dropped, values clamped) — never a hard failure.
3. **Given** a save error (disk full), **Then** the partial file is
   removed and "Unable to save file" reports, as the .IT path does.

---

### User Story 2 - Choose the format on the save screen (Priority: P2)

The F10 save requester shows the original's four format radio buttons at
(69,12)..(77,23) — " IT214" (0), "  S3M" (1), " IT2xx" (2), " IT215" (3) —
reflecting and setting `SaveFormat`. A typed filename without a dot gets
".IT" or ".S3M" per the selected format (D_SaveModule); Ctrl-S quick-save
replaces the extension with "IT"/"S3M" per format (D_SaveSong).

**Acceptance Scenarios**:

1. **Given** the F10 screen, **When** the user clicks/selects "  S3M",
   **Then** SaveFormat becomes 1 and the button renders selected.
2. **Given** SaveFormat 1 and filename "FOO", **Then** the file saved is
   FOO.S3M; with SaveFormat 3 it is FOO.IT.
3. **Given** Ctrl-S with a loaded "SONG.IT" and SaveFormat 1, **Then** the
   quick-save writes SONG.S3M.

---

### Edge Cases

- Empty/absent patterns inside the range: translated as 64 empty rows
  (Music_GetPattern's empty-pattern fallback).
- All-0xFF order list: OrdNum 2, orders FF FF (original arithmetic).
- The >100-pattern header-length quirk (BP includes the unclamped 2 bytes
  per pattern) is kept 1:1.
- Warnings must each appear once per save (sticky screen rows) and the
  save waits for a key when any warning fired (original behaviour).
- Sample memseg for data-less samples patches 0.

## Requirements *(mandatory)*

- **FR-001**: `Save_S3MModule(path)` in it_save.c MUST transliterate
  D_SaveS3M 1:1 (header layout, counts arithmetic, channel settings/pan
  conversion, sample-header block, pattern translation incl. the
  cell-drop/clamp quirks, unsigned sample conversion, final header +
  memseg patch passes), reusing the existing save_block/error plumbing;
  partial files are deleted on write error.
- **FR-002**: All eleven original warning conditions MUST fire the exact
  message texts (IT_DISK.ASM 611..621) on their original rows via an
  editor hook, and the save MUST wait for a keypress when any fired.
- **FR-003**: The F10 requester MUST gain the four format radio buttons
  (IT_OBJ1.ASM coordinates/labels/styles), keyboard- and mouse-operable,
  bound to `SaveFormat`.
- **FR-004**: Extension handling MUST follow D_SaveModule (append .IT or
  .S3M when the typed name has no dot) and D_SaveSong (Ctrl-S replaces
  the extension by format).
- **FR-005**: SaveFormat 1 MUST dispatch to the S3M writer everywhere the
  .IT writer is reachable with a format choice (F10; Ctrl-S).
- **FR-006**: The selftest MUST gain an S3M block: save, verify magic and
  counts, re-import through the existing S3M importer, assert
  tempo/speed/GV/orders/pattern-cell/sample-byte fidelity, and clean up;
  determinism/roundtrip gates stay green (engine untouched).
- **FR-007**: Engine files untouched; docs updated (README save section +
  fidelity notes, HANDOFF §2/§6).

## Success Criteria

- **SC-001**: Selftest S3M block passes headless on Windows and Linux.
- **SC-002**: All existing gates stay green.
- **SC-003**: A saved .S3M loads in the port and matches the original
  writer's layout byte-for-byte for representable content (verified by
  field assertions; full byte-diff against a DOS-written file is not
  possible without DOSBox — recorded as a deferred check).

## Assumptions

- The port's S3M importer (feature 007) is the read-side reference; a
  save->import round-trip exercises both.
- `Song.Header.Reserved` + `Save_LoadTime` feed the same obfuscated timer
  dword as the .IT writer (identical formula in D_SaveS3M).
