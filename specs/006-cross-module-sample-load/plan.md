# Implementation Plan: Load Samples & Instruments from Other Modules

**Branch**: `006-cross-module-sample-load` | **Spec**: spec.md | **Research**: research.md

## Summary

Port IT 2.17's sample/instrument library: browsing into a module (or
.KRZ/.PAT/.ITI/.XI file) from the F3/F4 lists, listing its
samples/instruments, previewing them through the check slot, and
loading a selected sample or a full instrument (with sample-slot
allocation and note-table remapping) into the current song. Source of
truth: `IT_D_RIS.INC`, `IT_D_RI.INC`, and the record/dispatch/transfer
machinery in `IT_DISK.ASM` — fully decoded in research.md. Also picks
up the 005 leftovers: Alt-O/T/W sample disk saves and the .ITI
instrument save.

## Technical Context

**Language**: C11, same toolchain as the repo (MSVC cl, CMake for
Linux/macOS). **New files**: `src/it_ris.c`, `src/it_ris.h` (named
after the source includes). **Touched**: `src/it_load.c/h` (export
`Load_SampleData`), `src/it_editor.c` (two requesters, F3/F4 Enter
wiring, Alt-O/T/W, selftest), `CMakeLists.txt`, docs.
**Testing**: determinism gate ×4 + `--roundtrip` ×4 unchanged
(FR-009/SC-005); `ITED_SELFTEST` gains a `LIB OK` block:
rip-vs-full-load byte-compare against testdata modules (including a
compressed IT215 save), scans of all five 007-generated test modules,
plus generated minimal .XI/.ITI/FAR/PTM round-trip checks where
practical. **Performance**: scans read the whole source file once into
memory (files are MB-scale; matches it_import.c).

## Constitution Check

* I (1:1 engine fidelity): all scanners/loaders are instruction-level
  ports of the ASM incl. quirks (127-order MOD scan, FAR hardcoded 256
  patterns, PAT length=loop-end field, MTM/669 record-0 pointer
  chaining, out-of-slots check before the exclusive release, Cvt/DfP
  word overwrite on instrument-sample transfer). Deviations (spec
  FR-004 overwrite warning; bounds-clamped reads instead of fixed
  2000-byte DOS buffers; no ULT/TXWave/WAV/AIFF, per 2.17 itself)
  are documented in README fidelity notes.
* II (UI from ASM data): requester reuses the ported F9 screen chrome;
  the library list mirrors the original's list layout (number/name
  columns, the `····Directory····` exit row, format names from
  `SampleFormatNames` strings).
* III (bit-deterministic audio): no engine changes; gates must stay
  IDENTICAL.
* IV (one engine): loaders write straight into `Song` under the
  engine lock, reusing `sample_t`/`instrument_t`.
* V (screen backend abstraction): UI goes through the existing widget
  layer only.

## Project Structure (delta)

```
it26/src/it_ris.c        # NEW: R2 scanners, R3 loader, R5 instrument side, R6 saves
it26/src/it_ris.h        # NEW: slibent_t/ilibent_t + API
it26/src/it_load.c/h     # Load_SampleData export (R3 converter superset)
it26/src/it_editor.c     # sample/instrument library requesters, wiring, selftest
it26/tools/gen_import_tests.py  # + minimal .XI (and .FAR/.PTM if feasible)
```

## Complexity Tracking

No new abstractions; one new translation unit mirroring one original
include pair. The requesters reuse the existing modal-loop pattern
(file_requester_run) rather than generalising it.
