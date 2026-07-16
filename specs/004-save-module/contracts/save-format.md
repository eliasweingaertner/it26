# Contract: .IT save output & editor surfaces

## C1. File layout (D_SaveIT order)

`[header 0xC0 + orders OrdNum + ins offsets ×4 + smp offsets ×4 + pat
offsets ×4] [MIDI config 4896 if header flag bit7] [message if
Special&1] [instruments 554×n] [sample headers 80×n] [patterns
(8-byte header + packed data, empty ⇒ offset 0)] [sample data blocks]`
then the sample-header block is rewritten in place with the real data
offsets. OrdNum = 257−k (k trailing 0xFF), always ends with 0xFF.
Cwt = 0x0217; Cmwt = 0x214/0x215/0x200/0x100 per SaveFormat (+0x216
override when a filter envelope exists). Special: bit0 message, bit2
row-hilight-valid (always set: value 4 base), bit3 MIDI config.
PHiligt from the pattern editor's row hilight config.

## C2. Compressed sample blocks (SaveFormat 3 default / 0)

Per 32KB (bytes) block: u16 length + LSB-first bitstream; deltas (×2
for IT215), bit-width table minimised per R2's exact cost rules, width
changes escaped per regime (≤6 / ≤8(16) / 9(17)). Must decompress to
the input bytes via the loader's existing 2.14/2.15 decompressor —
verified by round-trip. Sample header on disk: Flags bit3 set (unless
SaveFormat 2), Cvt = 1 (|4 for IT215).

## C3. Save requester (F10)

F9-style screen titled for saving with the same file/dir/drive lists,
plus an editable filename field primed from the loaded module's name.
Enter (field or file list) → if the target exists, modal Yes/No
overwrite confirm defaulting to No; save runs with progress lines and
ends with a status ("Saved." / "Unable to save <name>"). A name
without '.' gets `.IT`. Esc cancels. Ctrl-S (menu "Save Current")
saves to the loaded filename through the same confirm-on-existing
rule. After a successful save the header File Name field shows the
saved name.

## C4. Message editor screen

Rows 13..47, x=2, 35 lines, 8000-byte CR-separated buffer. View mode:
Up/Down/PgUp/PgDn scroll, Enter → edit, Ctrl-T colour 12↔6. Edit mode:
free cursor per R4 keys (Ctrl-Y delete line, Tab = 8 spaces, Ins/Del/
Backspace, Home/End, Esc → view), word wrap replaces the last space
before column 75 (or inserts CR). Spaces render attr 3; CR shows as
char 20 attr 1 while editing; cursor cell attr `(a&8)|0x30`.
Buffer-full shows a "message too long" status. Message round-trips
through save/load byte-exactly.

## C5. Gates

- Determinism ×4 unchanged (`IDENTICAL`, known hashes).
- Round-trip: save each testdata module (SaveFormat 3, plus 0 and 2 in
  the harness), reload, re-render: hash equal to the original's.
- `ITED_SELFTEST` extended: type into the message editor, save to a
  temp file, reload, verify message + hash; report `SAVE OK`.
