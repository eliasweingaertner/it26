# Implementation Plan: Editor Polish Batch

**Branch**: `013-polish-batch` | **Date**: 2026-07-12 | **Spec**: spec.md

**Repo**: `C:\Users\elias\fable5\ittrack`

Three staged commits:

- **Stage A (US1)**: it_editor.c — Alt-U (pe_update_instruments +
  F4 key), F3/F4 name-edit cursor (SamplePos/InstrumentPos state,
  key/mouse plumbing, cursor draw), stereo-prompt hook + modal;
  it_load.c — `Load_StereoChoice` hook in the stereo branch;
  it_screen.c/h — `Screen_DefineHiASCII` (IT_FontROM -> FontB) +
  Shift-F9 call. Selftest: UPD block (Alt-U remap, name edit,
  hi-ASCII bank, stereo default). Docs note for the preview
  resolution.
- **Stage B (US2)**: it_ris.c — IFF + TX Wave scan records;
  it_load.c — TX 12-bit branch; fixtures via tools/gen_import_tests.py
  (8SVX + TX blobs); selftest LIB additions.
- **Stage C (US3)**: it_driver.c waveform tap; it_screen.c overlay
  (Screen_SetOverlay + rasterizer hook + palette); it_editor.c
  fourier_view() modal (FFT transliteration, spectrogram + bars,
  keys), Alt-F12 on F5; ITED_SHOT aid; selftest FFT sanity block.

Gates (determinism/roundtrip/selftest, Windows + WSL) after each
stage; engine files untouched (driver tap is driver-side, feeding no
audio path).
