# Feature Specification: Editor Polish Batch

**Feature Branch**: `013-polish-batch`

**Created**: 2026-07-12

**Status**: Draft

**Input**: User description: "The remaining §6 leftovers as one batch:
(1) F4 Alt-U update-pattern-data, (2) F3/F4 in-list name editing,
(3) the WAV stereo Left/Right prompt, (4) hi-ASCII message-editor
charset (S_DefineHIASCII), (5) AIFF (IFF 8SVX/16SV) + TX Wave
standalone sample loading, (6) the Alt-F12 Fourier spectrum analyser
(IT_FOUR.ASM, SPECTRUMANALYSER=1 in the released SWITCH.INC), and
(7) resolve the 'instrument-record preview' leftover."

## User Stories

### US1 - Small F3/F4/loader completions (P1)

1. **Alt-U** on the F4 instrument list runs `I_UpdateInstrument` /
   `PE_UpdateInstruments`: for every pattern, every cell whose (note,
   instrument) pair matches an entry of the current instrument's
   note-sample table is rewritten to (table index as note, current
   instrument) — converting sample references into instrument
   references after table edits.
2. **In-list name editing**: the F3/F4 lists carry a horizontal cursor
   (`SamplePos` 0..25 / `InstrumentPos`): inside the name, printable
   keys insert (shifting right within the 25-char region), Backspace
   deletes left, Delete deletes at the cursor; Left/Right/Home/End move
   it; at the right stop the keys keyjazz as today; mouse clicks place
   it. The cursor cell renders attr 30h on the selected row.
3. **WAV stereo prompt**: loading a stereo WAV pops the original
   "Loading Stereo Sample" requester (box (26,22)-(54,29) style 3,
   Left/Right style-8 buttons, keys L/R) and loads the chosen channel;
   headless paths keep the silent-left default.
4. **hi-ASCII**: entering the message editor (Shift-F9) loads font
   bank B with the full CP437 ROM font (S_DefineHIASCII), so
   colour-12 text (attr bit 3) shows real high-ASCII characters; the
   editor accepts bytes 128..255 where the backend delivers them.
5. **Instrument-record preview**: verified against the ASM — the
   original's Load Instrument screen has NO note preview (only the
   sample library previews). The port already matches; the stale
   "leftover" is removed from the docs.

### US2 - AIFF + TX Wave standalone samples (P2)

`D_GetSampleInfo`'s IFF ('FORM'+'8SVX'/'16SV', format code 17, name/
VHDR/BODY chunk walk, big-endian fields halved for 16-bit) and TX Wave
("LM8953" ident, format code 13, attack/loop 17-bit lengths, rate by
the +17h byte: <2→33000, =2→50000, else→16000, data at offset 32,
Cvt = signed|TX-12-bit) records join the F3 library scanner, and
`Load_SampleData` gains the original's 12-bit TX unpack branch
(3 bytes → two 16-bit samples, exact bit ops).

### US3 - Alt-F12 Fourier spectrum analyser (P3)

Alt-F12 on the info page (F5) enters the analyser: the 2048-point
radix-2 FFT from IT_FOUR.ASM (bit-reversal table, FSinCos twiddle
recurrence, magnitude sqrt scaled by 1/128, >>6 clamp 255) over the
driver's last 2048 output samples, drawn as the original's scrolling
spectrogram (one column per frame, bins bottom-up) above the 64-row
thresholded bar spectrum, with the two switchable gradient palettes
('p'), +/- volume keys, playback keys and ESC exit. The port renders
into a full-resolution 640x400 8-bit overlay presented by the pixel
backends (the original leaves text mode for VESA); the terminal
backend reports the analyser needs a pixel backend (documented).

## Requirements

- FR-001 Alt-U per `PE_UpdateInstruments` semantics (match on the
  current instrument's table, first-match index, all patterns,
  commit per pattern; not undoable, as the original).
- FR-002 name-edit cursor per `I_PostSampleList`/`I_SelectInstrument`
  (insert/backspace/delete/arrows/home/end/mouse, 25-char region,
  attr-30h cursor cell).
- FR-003 stereo prompt per `O1_StereoSampleList` via a loader hook
  (NULL hook = left, keeping library/selftest paths headless).
- FR-004 `Screen_DefineHiASCII` fills font bank B from `IT_FontROM`;
  called on message-editor entry (Glbl_Shift_F9 parity).
- FR-005 IFF/TX records + the TX 12-bit loader branch byte-exact per
  the D_GetSampleInfo/D_LoadSampleData decode; library UI shows the
  original type names ("AIFF Sample", "TX Wave Sample").
- FR-006 FFT numerics transliterated (float precision, same
  operation order); overlay + palettes byte-derived from
  Fourier_SetPalette; keys per FourierKeyList.
- FR-007 engine sequencer untouched; the WAV driver gains only a
  passive waveform tap (DriverFlags bit 2 semantics).
- FR-008 selftest: Alt-U remap assertions, name-edit insert/delete
  round trip, TX/IFF fixture scan+load byte checks, FFT sanity
  (impulse/sine bins), stereo-hook default; gates stay green.
- FR-009 docs: README + HANDOFF (incl. the preview resolution).

## Success Criteria

- SC-001 selftest additions pass headless on Windows and Linux.
- SC-002 all existing gates stay green on both platforms.
- SC-003 ITED_SHOT captures the analyser overlay (BMP path renders
  the overlay), verifying the display headless.
