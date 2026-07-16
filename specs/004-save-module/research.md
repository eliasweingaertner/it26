# Phase 0 Research: Save Module & Message Editor

## R1. D_SaveIT (IT_D_WM.INC 53) — file layout & header fixups

Order of operations (all offsets = on-disk `.IT` layout):

1. `PE_SaveCurrentPattern` (commit the edit grid), `D_UpdateFileName`.
2. Create file; on failure → "Unable to save" dialog.
3. Header: first 0xC0 bytes copied from song memory, then fixups in the
   disk buffer only:
   - OrdNum: scan orders backward from index 255; with k trailing 0xFF
     entries, OrdNum = 257−k (one 0xFF terminator included; a final
     0xFF is always written).
   - InsNum/SmpNum from the engine counts; PatNum = max used pattern+1.
   - Special = 4 (row hilight valid) | 2 (timer history, only when
     timer data exists) | 8 (MIDI config, when header flag bit 7) | 1
     (message, when length > 1). PHiligt ← pattern editor's RowHilight.
   - Cwt = 0x0217 (TRACKERVERSION). Cmwt: SaveFormat 0 → 0x214;
     SaveFormat 3 → 0x215; SaveFormat 1/2 → 0x200 (0x100 if no
     instruments); **0x216 if any instrument's pitch envelope has flag
     0x80 (filter envelope)** — checked while emitting instrument
     offsets.
   - Reserved dword [3Ch]: edit-time obfuscation
     `EAX = ticks + old; XOR 'JTHL'; ROR 4; NEG; ROL 7; XOR 'ITRK'`
     (ticks = 18.2 Hz timer since load).
   - MsgLgth [36h] = strlen+1 (only when > 1), MsgOffset [38h] = file
     offset after MIDI config, [3Ah] = 0.
4. Offset tables: instrument offsets (554 apart), sample header
   offsets (80 apart; the *position of this table* is remembered),
   pattern offsets (empty pattern → 0; else advance by DataLength+8).
5. Blocks written in order: header+orders+offset tables · timer history
   (count word +1, then 8-byte entries — N/A in the port, no timer
   data) · MIDI config (4896 bytes) when flag bit 7 · message ·
   instrument headers (554×n) · provisional sample headers (80×n, from
   a scratch copy with Flags/Cvt/OffsetInFile patched: Flags |= 8
   compressed unless SaveFormat 2; Cvt = 1 | (4 when SaveFormat 3 =
   double-delta); provisional offsets assume uncompressed data) ·
   patterns (8-byte header + packed data; empty skipped) · sample data
   (per sample: real file position patched into the scratch header
   first — `lseek(cur)` → OffsetInFile) · **rewrite the sample-header
   block** at its remembered position with the patched offsets
   (`WriteITSampleBlock`, called once before data and once after).
6. Close; on any error `D_DeleteIfError` unlinks the file; else
   PEResetModified. Progress strings drawn at (4,17..23) during save.

## R2. The IT 2.14/2.15 sample compressor (IT_DISK.ASM 3406)

Per 32KB block of raw bytes (16-bit samples: 16384 samples per block):

1. Delta pass over the block (byte-wise for 8-bit, word-wise for
   16-bit), starting from 0 each block. SaveFormat 3 runs the delta
   pass **twice** (IT215 double-delta).
2. Bit table: per delta, bits-needed via LUTs built once in D_SaveIT
   (`BitLUT` runs for 8-bit at DiskDataArea:10240; two-level 16-bit
   lookup: `t41[hi]` → low 3 bits pick aux table for the low byte,
   high 5 bits are the base).
3. Minimise pass (widths 1..8, 16-bit 1..16): for each run of equal
   width DL bounded by neighbours, widen the run to the neighbours' min
   width when the width-change escape overhead exceeds the saving
   (exact cost rule ported literally: run ≤ 17 (33), boundary cost
   `minbits (+3/+4 per side when ≤ 5)`, force when run length 1;
   sentinel width 9/17 before the first element).
4. Bit-stream: start width 9 (17); per delta, when the table width
   differs emit the width-change escape of the current width's regime —
   width ≤ 6: `1<<(width−1)` then new code in 3 (4) bits, code =
   new−1−(new>cur); width ≤ 8 (16): `(1<<(width−1)) − 5(−9) + code` in
   width bits; width 9 (17): `0x100 | (new−1)` in 9 bits (17-bit
   analogue) — then the delta in the current width. LSB-first bit
   packing (`WriteBits` flushes low bytes).
5. Block output: u16 compressed-length + bitstream (+pad byte when the
   bit count isn't byte-aligned). This is the exact inverse of the
   loader's decompressor already in `it_load.c`.

`D_SaveSampleData` (SaveFormat 2) writes raw bytes in 32KB chunks.

## R3. Save UI flow (IT_DISK.ASM 4566, 4085)

- `Glbl_F10` → mode 10, the file window with `O1_SaveModuleList` (file
  list + editable filename field). Two commit paths: Enter on the
  filename field → `D_SaveModule` (splits any directory part into the
  song directory, applies `.IT` when no '.' present, rejects
  wildcards); Enter on a listed file → its name is copied.
- `D_CheckOverWrite`: if the target exists, a confirm dialog; cancel →
  no write. Then `D_SaveFileITModule` → `D_SaveIT`.
- Errors during write: "Unable to save" dialog once, subsequent writes
  suppressed, partial file deleted.

**Port mapping**: reuse the F9 requester (`req_scan`/draw) as a save
variant: filename input field primed with the current file name,
Enter → overwrite confirm (small modal like the existing menus) → save;
Ctrl-S / menu "Save Current" saves to the loaded path with confirm
skipped only if it's the same file already loaded (original always
confirms on existing files — keep the confirm).

## R4. Message editor (IT_MSG.ASM)

- Buffer: 8000 bytes, NUL-terminated, lines separated by CR (13).
  `Msg_GetMessageLength` = strlen+1 (1 = empty → not saved).
- View mode (entry): Up/Down/PgUp/PgDn scroll TopLine (clamp 7970/±35),
  Enter → edit mode, Ctrl-T toggles colour 12 ↔ 6.
- Edit mode: cursor = byte offset `CurrentPosition`; line/column are
  recomputed by scanning each frame; TopLine follows the cursor over 35
  visible lines. Keys: arrows (Left clamps 0, Right stops at NUL),
  Home/End (line), Up/Down (column-preserving via line scan), PgUp/PgDn
  = 35× up/down, Ins (insert space), Del, Backspace, Ctrl-Y delete
  line, Alt-C clear (confirm dialog) — port as Ctrl-C-free choice:
  Ctrl-Y kept, clear via confirm on Ctrl-L (documented stand-in),
  Tab = 8 spaces, Esc → view mode, printable ≥ 32 inserts, Enter
  inserts CR, buffer-full → "message too long" dialog.
- Word wrap `CheckWordWrap`: current line only; if length (incl. CR)
  > 75: replace the last space before column 75 with CR, else insert a
  CR at column 75.
- Draw: rows 13..47 (35 lines) at x=2, text attr = CharacterColour
  (12 default), **spaces drawn attr 3**; edit mode shows CR as char 20
  attr 1 and end-of-text as char 20 attr 2 (view mode: blank attr 3 /
  4); cursor cell attr = `(attr & 8) | 30h`.
- Loader: `D_LoadIT` reads the message into the buffer; the port's
  `it_load.c` currently skips Special bit 0 — add the read (offset
  [38h], length [36h], clamp 7999, NUL-terminate) and clear the buffer
  when absent (`Msg_ResetMessage` on every load path).

## R5. SaveFormat & defaults

`SWITCH.INC`: `DEFAULTFORMAT = 3` → IT215 double-delta compression is
the authentic 2.17 default. S3M save (SaveFormat 1, `D_SaveS3M`) is out
of scope per the spec. The port implements SaveFormat 3 (default), 0
(IT214 single-delta) and 2 (uncompressed) — all three exercised by the
round-trip test; only 3 is exposed in the UI (as in stock 2.17 the
format is a config choice, not a per-save prompt).

## R6. Round-trip verification

`tests/test_pattern.c` gains a `--roundtrip` mode: load module, render
hash A, `Save_ITModule(tmp)`, `Music_FreeIT` + reload tmp, render hash
B; pass iff A == B and the four canonical hashes are unchanged. This
covers compressed sample round-trip through the existing 2.14/2.15
decompressor, pattern re-pack, message, and header equivalence.

## R7. Deviations (documented)

- Timer/edit-history blocks (Special bit 2) are not written (the port
  tracks no timer data); the original omits them too when absent. The
  Reserved-dword edit-time obfuscation is fed from wall-clock seconds
  ×18.2 since module load.
- Alt-C (clear message) has no Alt in the key layer → stand-in key,
  documented in README.
- The original's save progress strings (rows 17..23 of the mode-10
  screen) are shown, but saving is fast enough that they appear once.
