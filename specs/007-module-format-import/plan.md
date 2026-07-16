# Implementation Plan: Module Format Import

**Branch**: `007-module-format-import` | **Date**: 2026-07-03 | **Spec**: `specs/007-module-format-import/spec.md`

## Summary

Port IT's own whole-module importers (`IT_D_RM.INC`: D_LoadS3M / XM /
MOD / MTM / 669 and the `PE_Translate*Pattern` converters from
`PE_TRANS.INC`) into a new `src/it_import.c`. `Import_LoadModule(path)`
sniffs the format (signatures + .MOD fallback) and converts into the
IT in-memory song; the F9 requester lists and opens all five formats;
convert-and-save falls out of feature 004. Verification: synthetic
minimal modules generated per format + render smoke in the selftest;
the IT determinism/roundtrip gates stay untouched.

## Technical Context

**Language/Version**: C11. **Dependencies**: none new; pattern packing
via the existing exact codec (`Pattern_Pack`). **Storage**: reads the
five formats; writes nothing. **Testing**: gates ×8 unchanged +
`IMPORT OK` selftest over generated test modules
(`tools/gen_import_tests.py`). **Constraints**: conversion rules
transliterated from the ASM (incl. authentic quirks: MOD 127-order
scan, XM SmpNum off-by-one, S3M Dxy dead code, XM note-0 → B-0).
**Scale**: `it_import.c` ~1100 lines, requester wiring ~30.

## Constitution Check

- **Engine fidelity (I)/Determinism (III)**: engine untouched;
  `it_load.c` only gains an exported default-MIDI-macros helper (moved,
  not changed). Gates re-run.
- **Authentic data (II)**: FineTuneTable, MODPeriodTable,
  PanningPositions, XMEffectG copied byte-exact from the ASM.
- **One real engine (IV)**: importers fill `Song` directly and pack
  patterns through `Pattern_Pack`.
- **Portability (V)**: stdio only.

## Project Structure

```text
ittrack/src/it_import.c + it_import.h   # NEW: sniffing + 5 loaders
ittrack/src/it_load.c                   # export default MIDI macros
ittrack/src/it_editor.c                 # requester filter + dispatch
ittrack/tools/gen_import_tests.py       # NEW: synthetic test modules
ittrack/testdata/import_*.{s3m,mod,xm,mtm,669}
```

## Complexity Tracking

No violations.
