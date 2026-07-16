# Tasks: Editor Polish Batch

**Repo**: `C:\Users\elias\fable5\ittrack`

- [X] T001 Baseline gates green (post-012 tree).
- [X] T002 [US1] Alt-U: pattern_update_instruments per research R1 +
      F4 key (replaced the "not ported yet" stub).
- [X] T003 [US1] F3/F4 name-edit cursor per R2 (keys, mouse, draw;
      F3 SamplePos, F4 Spacebar edit mode + ESC guard).
- [X] T004 [US1] WAV stereo prompt per R3 (Load_StereoChoice hook +
      modal; right channel supported in the compaction).
- [X] T005 [US1] Screen_DefineHiASCII per R4 + Shift-F9/menu wiring.
- [X] T006 [US1] Selftest UPD block; docs preview resolution (R7).
- [X] T007 [US2] IFF 8SVX/16SV + TX Wave records per R5 (it_ris.c),
      TX 12-bit loader branch (it_load.c), lib_test.{iff,txw}
      fixtures + LIB selftest additions (even-length NAME chunk:
      the original walk has no pad skip).
- [X] T008 [US3] WAV-driver waveform tap (2048-frame mono ring,
      WAVDriver_GetWaveForm).
- [X] T009 [US3] Screen_SetOverlay 640x400 8-bit overlay + palette in
      the shared rasterizer (pixel backends + BMP shots).
- [X] T010 [US3] fourier_view(): FFT transliteration (float, ASM
      operation order), spectrogram + bars, palettes, keys,
      ITK_ALT_F12 on all backends, Alt-F12 on F5,
      ITED_SHOT_FOURIER aid, FFT sanity selftest.
- [X] T011 Docs (README/HANDOFF) + final gates: determinism x4 +
      roundtrip x4x3 IDENTICAL and 11-block selftest green on
      Windows (MSVC) and Linux (WSL); analyser shot visually
      verified; staged commits.
