# Implementation Plan: Standalone WAV Sample Loading

**Branch**: `008-wav-sample-loading` | **Date**: 2026-07-05 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/008-wav-sample-loading/spec.md`

## Summary

Close the feature-006 leftover for standalone `.WAV` samples: the F3 Enter
sample-load requester lists WAV files, identifies them per the original
`D_GetSampleInfo8` ("WAV Identification", `IT_D_INF.INC` 856..1000), shows
them with the original format names ("8 Bit WAV Format" / "16 Bit WAV
Format", codes 5/7), and loads them through the existing feature-006 rip
path. The load-side conversion ports the WAV branches of `D_LoadSampleData`
(`IT_DISK.ASM` 2867..3230): unsigned→signed for 8-bit, signed 16-bit as-is,
and the stereo channel-compaction pass — with the Left/Right prompt replaced
by a silent "Left" (documented deviation). Test WAVs join the generated
`testdata/` set and the selftest gains a WAV scan/rip/verify block.

## Technical Context

**Language/Version**: C11 (MSVC `/std:c11`, gcc/clang `-std=c11`)

**Primary Dependencies**: none new — extends `src/it_ris.c` (library
scanners/loaders), `src/it_load.c` (`Load_SampleData`), `src/it_editor.c`
(requester glue), `tools/gen_import_tests.py` (test asset generator)

**Storage**: reads standalone RIFF WAVE files; no new persisted state

**Testing**: `ITED_SELFTEST=1 ited` (new WAV block inside the `LIB OK`
gate), `tests/test_pattern.c` determinism regression (all four modules
`IDENTICAL`), `ITED_SHOT_SCREEN=10 ITED_SHOT_LIB=<wav>` capture

**Target Platform**: Windows / Linux / macOS, all existing backends —
feature is backend-independent (requester + loader code only)

**Project Type**: native desktop application (tracker editor over the
1:1-ported engine)

**Performance Goals**: n/a beyond existing requester behaviour — scanning a
WAV reads the header once; loads are bounded by the original's 4,177,910-byte
data cap

**Constraints**: behaviour MUST match IT 2.17's WAV path byte-for-byte in
its decisions (identification rules, bounded chunk walk incl. the 16-bit
chunk-size arithmetic, length cap, 16-bit C5Speed field), except the single
approved deviation (no stereo Left/Right prompt — left channel taken)

**Scale/Scope**: one new scanner (~80 lines), one extended converter path in
`Load_SampleData` (~30 lines), 3 one-line editor/filter touches, generator +
selftest + docs updates

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Gates derived from the constitution (`.specify/memory/constitution.md` v1.0.0):

- **Engine fidelity (I) + Determinism (III)**: `src/it_load.c` is touched —
  `Load_SampleData` gains the stereo (Cvt bit 5) branch of the original
  `D_LoadSampleData` it was ported from. This is *adding a missed branch of
  the same original routine*, not divergence; flag semantics stay exactly the
  ASM's (BP bit 64 = stereo, bit 128 = right — the port fixes "left", i.e.
  bit 128 never set, per the approved deviation). No other engine file is
  touched; the module-load path is unaffected because no module loader ever
  sets Cvt bit 5. The §4 regression (4 modules `IDENTICAL`) MUST be re-run
  and green before the feature is done. **PASS** (with mandatory regression
  re-run).
- **Authentic data (II)**: No UI layout/colour changes. The only new strings
  are the original's own `WAV8BitFormat`/`WAV16BitFormat` texts, copied
  verbatim from `IT_DISK.ASM` 556..557. The identification/record rules come
  from `IT_D_INF.INC`, not invention. The omitted stereo prompt is flagged as
  a deviation in README fidelity notes + HANDOFF (FR-006). **PASS**.
- **One real engine (IV)**: Loads go through the existing feature-006 path
  (`RIS_LoadSample` → `Load_SampleData` → slot install under
  `Engine_Lock`/`Unlock`, playback stopped). No new audio machinery. **PASS**.
- **Portability (V)**: Pure C11 file parsing; no platform code, no backend
  changes, `external/miniaudio.h` untouched. **PASS**.

*Post-design re-check (after Phase 1)*: unchanged — no violations; the
Complexity Tracking table stays empty.

## Project Structure

### Documentation (this feature)

```text
specs/008-wav-sample-loading/
├── plan.md              # This file
├── research.md          # Phase 0: the decoded original WAV contract
├── data-model.md        # Phase 1: WAV record → slibent_t/sample_t mapping
├── quickstart.md        # Phase 1: build & validation walkthrough
├── contracts/
│   └── ris-wav.md       # Phase 1: C API surface touched by this feature
└── tasks.md             # Phase 2 (/speckit-tasks — not created here)
```

### Source Code (repository root: `ittrack/`)

```text
ittrack/
├── src/
│   ├── it_ris.c         # + scan_wav() (D_GetSampleInfo8 port), RIS_ScanModule
│   │                    #   dispatch branch, RIS_KnownExt + ".WAV",
│   │                    #   RIS_FormatName cases 5/7
│   ├── it_load.c        # Load_SampleData: + stereo pass (Cvt bit 5, left
│   │                    #   channel), the missed branch of D_LoadSampleData
│   ├── it_editor.c      # lib_open_source: formats 5/7 load directly (like
│   │                    #   .ITS); selftest: WAV scan/rip/verify block
│   └── it_ris.h         # comment updates only (format-code list)
├── tools/
│   └── gen_import_tests.py  # + generate testdata/lib_test.{wav8,wav16,wavst}.wav
├── testdata/            # generated WAV fixtures (mono 8-bit, mono 16-bit,
│                        #   stereo 16-bit)
├── docs/HANDOFF.md      # status + leftover list update
└── README.md            # fidelity note: stereo prompt deviation
```

**Structure Decision**: Existing single-project layout; the feature is a new
source-format branch inside the feature-006 library machinery plus the
matching branch of the already-ported `D_LoadSampleData`. No new files, no
new subsystems.

## Complexity Tracking

No constitution violations — table intentionally empty.
