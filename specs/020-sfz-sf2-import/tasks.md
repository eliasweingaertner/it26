# Tasks: SFZ and SoundFont 2 Instrument Import

**Input**: `specs/020-sfz-sf2-import/` (spec, plan, research, data-model, contracts)

## Phase 1: Setup

- [x] T001 Add `src/it_sfimport.c` / `src/it_sfimport.h` skeleton (contract API) and list it in `CMakeLists.txt` EDITOR_SOURCES and `Makefile` EDITOR_SRCS
- [x] T002 Enable miniaudio's WAV/FLAC decoders at the implementation site in `src/it_editor.c` (drop `MA_NO_DECODING`, add `MA_NO_MP3`); miniaudio declares dr_wav/dr_flac only inside its implementation, so the decode call is `SFI_DecodeAudio` in `src/it_editor.c` (research R6)
- [x] T003 Add `RI_FMT_SFZ/SF2PRESET/SF2`, `RI_IsLibrary`, `RI_LastReport` to `src/it_ris.h`

## Phase 2: Foundational (shared back half, research R3/R6–R10)

- [x] T004 Region model + per-key selection (R1/R2) in `src/it_sfimport.c`
- [x] T005 Voice dedupe, reduce-to-fit (R7), slot release + allocation mirroring `RI_LoadInstrument`
- [x] T006 Audio decoding: dr_wav (WAV/AIFF, smpl loops), AIFF MARK/INST loop reader, dr_flac; range/loop clipping; stereo layout via `it_sample.h`
- [x] T007 Instrument build: note table, C5Speed (R3), GvL/DfP (R10), envelope (R8), filter (R9), names

## Phase 3: User Story 1 — SFZ (P1) 🎯 MVP

- [x] T008 [US1] SFZ tokenizer: comments, `#define`, `#include`, headers, opcode values with spaces, note names
- [x] T009 [US1] Header inheritance (`<control>` default_path, global/master/group/region) → regions; CC/switch/seq/rand/trigger conditions
- [x] T010 [US1] Sample path resolution (backslashes, default_path, case-insensitive fallback on POSIX)
- [x] T011 [US1] `SFI_CountSFZ`; `it_ris.c`: identify `.sfz`, format name, `RI_KnownExt`, load dispatch

## Phase 4: User Story 2 — SF2 (P1)

- [x] T012 [US2] RIFF walk with fseek, `pdta` reader, SF3 detection
- [x] T013 [US2] Preset → zones: global zones, preset+instrument generator layering, range intersection → regions; linked stereo partner
- [x] T014 [US2] `SFI_ScanSF2` (rows, sorted bank/program), SF2 sample reading (16-bit, seek per sample) in the decoder
- [x] T015 [US2] `it_ris.c` identify (content), `RI_ScanModule`, names; `it_editor.c`: every `>= 8` library test → `RI_IsLibrary`

## Phase 5: User Story 3/4 — fit and shape (P2)

- [x] T016 [US3] `lib_load_instrument_entry`: skip the pre-check for the new formats, status line with reduced/skipped counts, "Out of sample space!" on RI_ERR_SLOTS
- [x] T017 [US4] Verify envelope/filter/pan conversions in the selftest (part of T019)

## Phase 6: Tests

- [x] T018 `tools/gen_import_tests.py`: `gen_sfi_fixture()` — SF2 (2 presets: mono looped key split w/ velocity layers + global zone; stereo linked pair), SFZ (#include, #define, groups, round robin, CC/switch regions, release region, 16-bit WAV with smpl loop, 24-bit stereo WAV, AIFF with loop, FLAC), commit `testdata/sfi_fixture/`
- [x] T019 Selftest block **SFI** in `src/it_editor.c`
- [x] T020 Batch mode `ited --sfi-test` (contract); run against `D:\CommonHome\sound\sfz` and `\sf2`, fix failures until SC-001/002 hold
- [x] T021 Native dialog filters (`*.sfz;*.sf2`) for Open Instrument
- [x] T022 `test_pattern` ×4 IDENTICAL; full selftest green

## Phase 7: Polish (US5)

- [x] T023 F1 help (Load Instrument / it26 additions), README (sources + conversion rules + limits), CHANGELOG (Unreleased), HANDOFF §5h, memory note
- [x] T024 Commit locally (no push)

## Dependencies

T001–T003 → T004–T007 → US1 (T008–T011) and US2 (T012–T015) in either order → T016 → T018–T022 → T023–T024.
