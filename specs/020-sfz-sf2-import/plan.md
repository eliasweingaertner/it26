# Implementation Plan: SFZ and SoundFont 2 Instrument Import

**Branch**: master (local commits, no push) | **Date**: 2026-11-13 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/020-sfz-sf2-import/spec.md`

## Summary

The F4 Load Instrument screen learns two new sources: `.sfz` files
(instrument rows) and `.sf2` files (library rows that open in place and
list presets). A new editor-side module `src/it_sfimport.c` parses both
formats into one intermediate region list, picks one region per IT note
(loudest velocity layer, first round robin, default controllers / key
switch), deduplicates the regions into IT samples (root key and tuning
baked into C5Speed), thins the set to the free sample slots when needed,
decodes the audio (SF2 sample block directly; WAV/AIFF/FLAC through
miniaudio's built-in dr_wav/dr_flac), and writes an ordinary IT
instrument (note table, volume envelope from the amp EG, filter from the
low-pass) plus samples. `it_ris.c` dispatches to it; the engine is not
touched.

## Technical Context

**Language/Version**: C11 (MSVC `/std:c11`, gcc, clang)
**Primary Dependencies**: vendored `external/miniaudio.h` 0.11.25 (unchanged; its dr_wav and dr_flac decoders are enabled by dropping `MA_NO_DECODING` in `it_editor.c`, `MA_NO_MP3` added)
**Storage**: files (.sfz text + sample files; .sf2 RIFF)
**Testing**: `ITED_SELFTEST=1 ited` new block **SFI** over generated fixtures (`tools/gen_import_tests.py` → `testdata/sfi_fixture/`); new batch mode `ited --sfi-test <path>` for the user's real collections (SC-001/002); `test_pattern` ×4 for SC-005
**Target Platform**: Windows, Linux, macOS
**Project Type**: desktop application (tracker), editor-side extension
**Performance Goals**: SF2 library listing < 1 s for any file size (only the `pdta` chunk is read); preset/SFZ load < 3 s for ≤ 50 MB of audio
**Constraints**: engine 1:1 untouched; vendored header unedited; 99 sample slots; IT envelope max 25 nodes, tick ≤ 9999; fadeout ≤ 256 per tick
**Scale/Scope**: 1387 SFZ + 1189 SF2 files in the user's library as the field test

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)**: no engine file changes. New code lives in `src/it_sfimport.c` (editor sources); `it_ris.c` and `it_editor.c` are editor files. `test_pattern` ×4 re-run anyway (SC-005). ✅
- **Authentic data (II)**: no new screen layout; the new rows reuse the Load Instrument screen's record drawing (format name column gets new strings, library rows reuse the "Library" marker). The new format names are flagged as it26 additions in the README. ✅
- **One real engine (IV)**: the converter writes real `instrument_t` / `sample_t` records via the same slot allocation as `RI_LoadInstrument`, under `ed_lock()`. ✅
- **Portability (V)**: plain C11 stdio; directory case-insensitive fallback uses the existing dirent / FindFirstFile pattern; miniaudio header untouched (only configuration macros at the implementation site). ✅

Post-design re-check: unchanged, all gates pass.

## Project Structure

### Documentation (this feature)

```text
specs/020-sfz-sf2-import/
├── plan.md              # this file
├── research.md          # Phase 0: format facts + conversion decisions
├── data-model.md        # Phase 1: intermediate region model
├── quickstart.md        # Phase 1: validation guide
├── contracts/
│   └── sfimport-api.md  # it_sfimport.h contract + Load Instrument record formats
├── checklists/requirements.md
└── tasks.md             # /speckit-tasks
```

### Source Code (repository root)

```text
src/
├── it_sfimport.h        # NEW: SFI_* API (identify, scan SF2, count SFZ, load)
├── it_sfimport.c        # NEW: SFZ parser, SF2 parser, region selection,
│                        #      dedupe/reduce, audio decode, IT conversion
├── it_ris.c             # identify .sf2/.sfz, scan, load dispatch, names
├── it_ris.h             # RI_FMT_* codes, RI_IsLibrary()
├── it_editor.c          # miniaudio decoders on; li_* uses RI_IsLibrary;
│                        # load entry status; dialog filters; SFI selftest;
│                        # --sfi-test batch mode
CMakeLists.txt, Makefile # add src/it_sfimport.c to the editor sources
tools/gen_import_tests.py # gen_sfi_fixture(): SF2, SFZ, WAV, AIFF, FLAC
testdata/sfi_fixture/    # generated fixture (committed, small)
tools/gen_help.py / help # F1 text
README.md, CHANGELOG.md, docs/HANDOFF.md
```

**Structure Decision**: one new editor-side translation unit for both
formats (they share the whole conversion back half); `it_ris.c` keeps
being the single entry point the editor talks to.

## Complexity Tracking

No constitution violations.
