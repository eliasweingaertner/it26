# Quickstart: Standalone WAV Sample Loading (008)

Validation walkthrough — run from `C:\Users\elias\fable5\ittrack`.

## Prerequisites

- Build per `docs/HANDOFF.md` §3 (MSVC vcvars64 one-liner or CMake). The
  feature touches `it_ris.c`, `it_load.c`, `it_editor.c` — all already in
  the `ited` source list; `test_pattern` links `it_load.c` too.
- Regenerate test fixtures: `python tools/gen_import_tests.py testdata`
  (now also emits the `lib_test*.wav` files).

## Gate 1 — selftest (headless)

```
ITED_SELFTEST=1 ./ited testdata/itdemo.it
```

Expect the existing gates plus the WAV block to report `LIB OK`:
scan (formats 5/7, frame counts, C5Speed), rip (byte-exact vs generator,
stereo → left channel), save→load WAV round trip, and the two negative
fixtures refused.

## Gate 2 — determinism regression (engine file touched)

```
./test_pattern testdata/beyond_network.it
./test_pattern testdata/itdemo.it
./test_pattern testdata/quests_end.it
./test_pattern testdata/synthscape_filters.it
```

All four MUST print `IDENTICAL` with the known-good hashes (HANDOFF §4).

## Manual check — the user-reported flow

1. `./ited testdata/itdemo.it`, press **F3**, select a slot, press
   **Enter** → sample-load requester.
2. Navigate to a folder of `.WAV` files (e.g. `testdata/`) — the WAVs are
   **listed** (previously the pane was empty).
3. Highlight one — info line shows `8 Bit WAV Format` / `16 Bit WAV Format`.
4. Press a note key — the WAV previews through check slot 100.
5. Press Enter — occupied slots ask `Replace sample N?`; the sample lands in
   the slot with filename-derived name, correct length, and the file's rate
   as C5 speed; piano keys audition it at the right pitch.
6. Load `testdata/lib_testst.wav` (stereo) — result is mono (left channel),
   half the interleaved frame count.

## Screenshot aid

```
ITED_SHOT=wavlib.bmp ITED_SHOT_SCREEN=10 ITED_SHOT_LIB=testdata/lib_test16.wav ./ited
```

renders the library browser with the WAV record (format name + length).

## Docs to verify updated

- `README.md` fidelity notes: stereo Left/Right prompt omitted (left channel
  taken) — deviation recorded.
- `docs/HANDOFF.md`: WAV moved out of the feature-006 leftovers; AIFF/
  TXWave/stereo-prompt remain listed.
