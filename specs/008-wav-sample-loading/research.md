# Research: Standalone WAV Sample Loading (008)

Sources read for this document:

- `impulse-tracker-jthlim/IT_D_INF.INC` 856..1000 — `D_GetSampleInfo8`
  ("WAV Identification") + record synthesis.
- `impulse-tracker-jthlim/IT_DISK.ASM` 390 (`WAVEfmtID`), 474..507
  (`SampleFormatNames`), 556..557 (format-name strings), 2867..3239
  (`D_LoadSampleData` incl. the stereo prompt and channel-compaction pass),
  8788..8830 `IT_OBJ1.ASM` (`O1_StereoSampleList` Left/Right menu).
- Port: `it26/src/it_load.c` (`Load_SampleData`), `it26/src/it_ris.c`
  (feature-006 scanners + `RIS_LoadSample`), `it26/src/it_editor.c`
  (`lib_open_source`, requester glue, selftest LIB block).

## R1. Decision: identification = `D_GetSampleInfo8`, ported exactly

**Decision**: A file is a WAV sample iff, per the original:

1. Bytes 8..15 equal `"WAVEfmt "` (i.e. the `WAVE` form type + `fmt ` chunk
   id). **The leading `RIFF` magic at offset 0 is NOT checked** — quirk, keep.
2. Bytes 18..19 equal `00 00` (high word of the `fmt ` chunk size must be 0)
   and bytes 20..21 equal `01 00` (wFormatTag = 1, integer PCM only).
3. The `data` chunk is found by a bounded walk: the first candidate chunk
   header is at offset `20 + fmt_size_low_word` (the ASM sets
   `BP = 0x18 + word[0x10]`, where `BP` addresses the chunk's *size field*
   and `[BP-4]` its id). At most **3** chunks are examined (`Mov CX,3`).
   When skipping, the advance is `low_word(chunk_size) + 8` — **only the low
   16 bits of the size dword are used to skip** (quirk, keep; a 16-bit carry
   in the ASM aborts — in C, walking past end-of-file aborts).
4. `wBitsPerSample` (byte at offset 34) must be exactly 8 or 16, else the
   file is "unknown".
5. `nChannels` (word at offset 22) == 2 → stereo. **Any other value
   (including >2) is treated as mono** — quirk, keep (a 4-channel WAV loads
   garbled in the original too; identification does not reject it).

**Rationale**: Constitution II — behaviour comes from the ASM, quirks
included; these rules are exactly what 2.17 accepted/refused.

**Alternatives considered**: a modern lenient RIFF parser (full 32-bit chunk
sizes, unlimited chunk walk, `RIFF` magic check) — rejected: it would accept
files the original refused and vice versa, breaking the parity requirement
(spec SC-004).

## R2. Decision: record synthesis (library entry fields)

**Decision**: The synthesized record (ITS-shaped, as feature 006's
`slibent_t.hdr`) is filled per `D_GetSampleInfo8`:

- Format code (record +88 in DOS, `slibent_t.Format`): `5` = 8-bit,
  `7` = 16-bit (`(DH & 3) + 4`). Stereo does not change the code.
- `GvL = 64`, `Vol = 64`.
- `Flg = 1 | (16bit ? 2 : 0) | (stereo ? 4 : 0)` (DH byte; bit 2 = stereo in
  the ITS header sense — `Load_SampleData` clears bits 2/3 after load, as
  the port already does).
- Name: the DOS filename (base name), copied up to 13 chars, NUL-padded to
  26 — the port's existing `transfer_filename()` behaviour.
- `Cvt = (16bit ? 1 : 0) | (stereo ? 32 : 0)` — bit 0 = "data is signed"
  (16-bit WAV is signed, 8-bit is unsigned), bit 5 = stereo. This is the
  word the loader shifts left into its BP flags.
- `DfP = 0` (the synthesis stores AH=0; no default pan).
- Length (frames): `min(dword at data-chunk size field, 4177910)`, then
  `>> 1` if 16-bit, then `>> 1` if stereo. Note the length **cap reads the
  full dword** while chunk *skipping* uses only the low word (R1.3) — two
  different widths, both authentic.
- `C5Speed = word at offset 0x18` — **only the low 16 bits of
  nSamplesPerSec** (quirk, keep: a 96 kHz WAV gets C5Speed 30464 in the
  original too). Loop/susloop fields and vibrato all 0.
- Data offset (`OffsetInFile`): the data chunk's payload start = size-field
  offset + 4 (ASM stores `BP + 4`).
- `FileSize`: the capped byte size of the data payload (display column).

**Rationale**: field-for-field parity with the DOS record so the shared
list/preview/load code needs no WAV-specific cases.

## R3. Decision: load path = the WAV branches of `D_LoadSampleData`

**Decision**: `Load_SampleData` (`it_load.c`), the existing port of
`D_LoadSampleData`, gains the one branch it skipped ("not ported" comment):
stereo, keyed on `Cvt & 32` (ASM BP bit 64 after the `<<1`):

- Bytes to read: `len << is16 << stereo` (the ASM doubles EDX once for
  16-bit, once for stereo).
- Conversion order as the ASM: read → unsigned→signed pass over the **full
  interleaved buffer** when `Cvt` bit 0 is clear (XOR 0x80 / 0x8000) → then
  the stereo compaction pass: in-place, halved count; 8-bit keeps bytes at
  even offsets (left) — right would start at +1; 16-bit keeps words at even
  word offsets — right would `LodsW` first. **The port always takes left**
  (BP bit 128 never set) — the approved deviation (R4).
- After the pass: `Flags &= ~0x0C; Cvt = 1;` — already in the port, now also
  correct for WAV (clears the stereo Flg bit 2).
- The existing 8-bit unsigned conversion (`Cvt` bit 0 clear) and signed
  16-bit passthrough already match the WAV needs — no change.

**Rationale**: Constitution I/IV — this is the same original routine the
function was ported from; adding its missed branch keeps one loader for all
sources. No module loader sets `Cvt` bit 5, so existing paths are untouched
(determinism gate must still be re-run — engine file).

**Alternatives considered**: a WAV-only de-interleave inside `it_ris.c`
before calling `Load_SampleData` — rejected: duplicates conversion logic
outside the routine that owns it in the original, and would diverge the
`Load_SampleData` contract from `D_LoadSampleData`.

## R4. Decision: stereo prompt → silent "Left" (documented deviation)

**Decision**: The original shows `O1_StereoSampleList` — a "Loading Stereo
Sample" box with `Left` (returns 64) / `Right` (returns 192) buttons and
L/R hotkeys — from *inside* `D_LoadSampleData`. The port does not add a UI
callback into the engine-side loader; it hardcodes the Left choice.
Recorded as a deviation in README fidelity notes + `docs/HANDOFF.md`
(replaces the "WAV … not ported" line). The Right/prompt option remains a
listed leftover.

**Rationale**: matches the feature-006 scope decision (research.md there:
"bit6 stereo prompt (WAV only — not ported)"); a mono result identical to
the original's default-focused button. Left = first button in the original
list.

**Alternatives considered**: porting the modal prompt — deferred, not
rejected on principle; it needs a UI hook threaded into `Load_SampleData`
(or a pre-load prompt in `it_ris.c`), which is exactly the kind of coupling
worth its own small task later. Rejecting stereo files — worse than the
original (parity break).

## R5. Decision: requester/editor integration points

**Decision**: three touches, all in existing feature-006 code:

1. `RIS_KnownExt` (`it_ris.c` 747): add `".WAV"` to the extension list —
   this alone makes WAV files *appear* in the requester (the user-reported
   symptom).
2. `RIS_ScanModule` (`it_ris.c` 656): add the WAV identification branch
   (content-keyed per R1, like every other sniff there) producing one
   record via `scan_wav()`.
3. `lib_open_source` (`it_editor.c` 6116): standalone WAVs load directly on
   Enter like `.ITS` — extend the single-record condition to
   `Format == 5 || Format == 7`. (The original dispatches standalone sample
   formats straight to LoadSample rather than opening a library view.)
4. `RIS_FormatName` (`it_ris.c` 730): add `case 5: "8 Bit WAV Format"` and
   `case 7: "16 Bit WAV Format"` — verbatim `IT_DISK.ASM` 556..557. The
   info line and library list pick these up with no further changes.

Preview (note keys → check slot 100) and the "Replace sample N?" confirm
need nothing: they run off `RIS_LoadSample`, which works once the record is
synthesized.

**Rationale**: feature 006 was built for exactly this kind of extension; no
new UI.

## R6. Decision: test assets & gates

**Decision**:

- `tools/gen_import_tests.py` generates three deterministic fixtures in
  `testdata/`: `lib_test8.wav` (mono 8-bit PCM), `lib_test16.wav` (mono
  16-bit PCM), `lib_testst.wav` (stereo 16-bit PCM with distinct L/R
  content so the left-channel pick is verifiable). Rates chosen to exercise
  C5Speed (e.g. 22050/44100).
- Selftest (`ITED_SELFTEST=1`) WAV block inside the existing LIB section:
  scan each fixture (expect 1 record, formats 5/7, expected frame counts and
  C5Speed), rip each (expect signed data matching the generator's known
  bytes; stereo rip must equal the left channel), and a save→load round
  trip: `RIS_SaveWAV` an existing sample, re-scan + rip it, compare data
  byte-for-byte. Failure prints and drops the `LIB OK` line.
- Determinism regression re-run (engine file touched): all four modules
  `IDENTICAL` per HANDOFF §4.
- Identification-parity negatives in the selftest: a `.WAV`-named file with
  a non-PCM tag (e.g. format 3 float) and a 24-bit PCM file must scan as
  unsupported (`RIS_ScanModule` < 0 or unknown), matching R1.

**Rationale**: keeps the feature inside the existing self-contained gates
(`LIB OK`, hashes) with fixtures generated, not committed binaries.

## R7. Note: `RIS_SaveWAV` round-trip caveat

Feature 006's `RIS_SaveWAV` writes a 44-byte canonical header (RIFF size
left 0, as the original does). Under R1 the loader never reads the RIFF
size field, and the fmt/data layout matches the 3-chunk walk, so saved
files re-load — the round trip in R6 is valid. The 8-bit save path writes
unsigned data (`Cvt` 0 on disk semantics), matching the load conversion.
