# Tasks: Standalone WAV Sample Loading

**Input**: Design documents from `/specs/008-wav-sample-loading/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md,
contracts/ris-wav.md, quickstart.md

**Tests**: The spec's SC-005/FR-009 explicitly require selftest and
regression gates, so test tasks are included.

**Organization**: Tasks grouped by user story. All code paths live in the
port repo `it26/` (paths below are relative to `C:\Users\elias\fable5\ittrack`).

## Format: `[ID] [P?] [Story] Description`

## Phase 1: Setup (test fixtures)

**Purpose**: Deterministic WAV fixtures the selftest and manual checks use.

- [X] T001 Extend `tools/gen_import_tests.py` to emit `testdata/lib_test8.wav`
      (mono 8-bit PCM, 22050 Hz), `testdata/lib_test16.wav` (mono 16-bit PCM,
      44100 Hz), `testdata/lib_testst.wav` (stereo 16-bit PCM, 44100 Hz,
      distinct L/R content), and the two negative fixtures
      `testdata/lib_testf.wav` (wFormatTag=3 float) and
      `testdata/lib_test24.wav` (24-bit PCM). Deterministic byte content
      (fixed waveform, no RNG); regenerate with
      `python tools/gen_import_tests.py testdata` and verify the files appear.

---

## Phase 2: Foundational

**Purpose**: none — feature 006 already provides the requester, record list,
preview, replace-confirm, and engine-lock machinery. No blocking work.

**Checkpoint**: proceed straight to User Story 1.

---

## Phase 3: User Story 1 - Load a mono WAV file as a sample (Priority: P1) 🎯 MVP

**Goal**: `.WAV` files appear in the F3 Enter requester, are identified with
the original's format names, and load correctly (8-bit unsigned→signed,
16-bit signed, filename name, frame count, C5Speed from the fmt chunk).

**Independent Test**: quickstart.md "Manual check" steps 1–5 with the mono
fixtures + selftest scan/rip assertions for `lib_test8.wav`/`lib_test16.wav`.

- [X] T002 [US1] Add `".WAV"` to the extension list in `RIS_KnownExt`
      (`src/it_ris.c` ~747) — makes WAV files appear in the requester pane.
- [X] T003 [US1] Add `case 5: return "8 Bit WAV Format";` and
      `case 7: return "16 Bit WAV Format";` to `RIS_FormatName`
      (`src/it_ris.c` ~730) — strings verbatim from `IT_DISK.ASM` 556..557.
- [X] T004 [US1] Implement `scan_wav()` in `src/it_ris.c` per research.md R1/R2
      and data-model.md: identification (bytes 8..15 `"WAVEfmt "`, bytes
      18..21 `00 00 01 00`, bits 8/16 only, ≤3-chunk `data` walk with
      low-word+8 skip, no `RIFF` check), record synthesis (Format 5/7,
      GvL/Vol 64, Flg `1|2*16bit|4*stereo`, Cvt `(16bit?1:0)|(stereo?32:0)`,
      DfP 0, Length `min(dword size,4177910)>>16bit>>stereo`, C5Speed =
      low word of nSamplesPerSec, OffsetInFile = payload start, name via
      `transfer_filename`). Wire a content-keyed dispatch branch into
      `RIS_ScanModule` (~656) alongside the other sniffs.
- [X] T005 [US1] Extend the single-record direct-load branch in
      `lib_open_source` (`src/it_editor.c` ~6116) so `Format == 5 || 7` loads
      directly on Enter like `.ITS` (standalone sample, no library drill-in).
- [X] T006 [US1] Selftest (mono half): in the LIB section of
      `src/it_editor.c`, scan `testdata/lib_test8.wav` and
      `testdata/lib_test16.wav` (expect 1 record each, Format 5/7, expected
      Length and C5Speed), rip both via `RIS_LoadSample` and compare data
      byte-for-byte against the generator's known waveform; scan the two
      negative fixtures and require refusal; add a `RIS_SaveWAV` → re-scan →
      rip → byte-compare round trip. Failures drop `LIB OK`.
- [X] T007 [US1] Build (`docs/HANDOFF.md` §3) and run
      `ITED_SELFTEST=1 ./ited testdata/itdemo.it` — all gates incl. the new
      WAV assertions green; manual quickstart steps 1–5 verified once
      (requester lists WAVs, info line names, preview, load, audition).

**Checkpoint**: mono WAV loading fully works — MVP deliverable.

---

## Phase 4: User Story 2 - Load a stereo WAV, left channel (Priority: P2)

**Goal**: stereo PCM WAVs load as left-channel mono (per-channel frame
count), via the ported stereo branch of `D_LoadSampleData`; deviation (no
Left/Right prompt) documented.

**Independent Test**: quickstart.md step 6 + selftest stereo assertions for
`lib_testst.wav`.

- [X] T008 [US2] Port the stereo branch of `D_LoadSampleData`
      (`IT_DISK.ASM` 3172..3219) into `Load_SampleData` in `src/it_load.c`
      per research.md R3: on `Cvt & 32`, read `Length << is16 << 1` bytes,
      run the existing unsigned→signed pass over the full interleaved buffer,
      then compact in place keeping the left channel (8-bit: even bytes;
      16-bit: even words; right-channel start offsets stay in the code as
      the ASM has them but are never selected). Update the function's header
      comment (drop "64 = stereo prompt … not ported"; note the left-fixed
      deviation).
- [X] T009 [US2] Selftest (stereo half) in `src/it_editor.c`: scan
      `testdata/lib_testst.wav` (Format 7, Length = per-channel frames), rip
      and byte-compare against the generator's left-channel waveform.
- [X] T010 [US2] **Determinism regression** (engine file touched): rebuild
      `test_pattern` and run all four testdata modules — every run MUST print
      `IDENTICAL` with the HANDOFF §4 hashes.

**Checkpoint**: both stories done; all gates green.

---

## Phase 5: Polish & Cross-Cutting

- [X] T011 [P] Record the deviation and status: `README.md` fidelity notes
      (stereo Left/Right prompt omitted, left channel taken); update
      `docs/HANDOFF.md` §2 feature-006 block + §6 roadmap #9 leftovers (WAV
      done; AIFF, TXWave, stereo-prompt remain), and the `it_ris.h`
      format-code comment (add 5/7).
- [X] T012 [P] Capture `ITED_SHOT_SCREEN=10 ITED_SHOT_LIB=testdata/lib_test16.wav`
      BMP and eyeball the library row (name, "16 Bit WAV Format", length)
      per quickstart.md.
- [X] T013 Final gate sweep per quickstart.md: regenerate fixtures, full
      selftest, 4× determinism, `test_pattern --roundtrip` on one module
      (save path untouched — sanity), Win32 build warning-free.

---

## Dependencies

- Phase 1 (T001) blocks T006/T007/T009 (fixtures needed by selftest) but not
  T002–T005/T008 (code can land first).
- US1 (T002–T007): T002/T003 [P]-independent; T004 depends on nothing else;
  T005 after T004 (needs Format 5/7 records); T006 after T001+T004; T007 last.
- US2 (T008–T010): T008 independent of US1 code (different file); T009 after
  T001+T004+T008; T010 after T008. US2 is testable without US1's UI touches
  (scan+rip level) but the full user flow builds on US1 — implement in order.
- Phase 5 after both stories.

## Parallel Opportunities

- T002, T003 and T004 touch disjoint regions and can be batched; T008 is a
  different file (`it_load.c`) and can proceed in parallel with US1.
- T011 and T12 are [P] once code is done.

## Implementation Strategy

MVP = Phase 1 + US1 (mono WAV end-to-end, the user-reported dead end fixed).
US2 adds the engine-side stereo branch + regression gate. Total: 13 tasks
(US1: 6, US2: 3, setup: 1, polish: 3).
