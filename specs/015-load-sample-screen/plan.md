# Implementation Plan: Authentic Load Sample Screen

**Branch**: `015-load-sample-screen` | **Date**: 2026-09-26 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/015-load-sample-screen/spec.md`

## Summary

Replace the F9-requester layout used for Load Sample with a transliteration of
`O1_LoadSampleList` (`IT_OBJ1.ASM:952`) and its custom draws
(`D_DrawLoadSampleWindow`, `D_LSDrawDriveWindow`, `IT_DISK.ASM`), and use the
same screen for the in-module library and the Ctrl-F3 Sample Library
(`O1_ViewSampleLibrary`). The data layer mostly exists: `slibent_t` already
mirrors the original's 96-byte list record and `RIS_ScanModule`/`RIS_LoadSample`
are the ports of `D_GetSampleInfo`/`LoadSample`. New work is the unified
directory+file listing with the original's sort, the screen itself, edits
written into the entry (research R8), audition-driven waveform, and the keys.

## Technical Context

**Language/Version**: C11.

**Primary Dependencies**: none new. Host directory enumeration via the existing
requester code (`req_scan`); `GetLogicalDrives` on Windows for the drive box.

**Storage**: none. `CACHE.ITS` deliberately not written (research R4).

**Testing**: `test_pattern` gates (unchanged, no engine code touched);
`ITED_SELFTEST` suite + new `LSS` block over a generated fixture folder;
`ITED_DUMP=13/14` capture aids.

**Target Platform**: Windows (Win32 + console), Linux/macOS (SDL2 + terminal).

**Project Type**: desktop application.

**Performance Goals**: listing a folder of 620 entries (the original's cap)
including header identification in well under a second on local disks.

**Constraints**: layout/colours only from the ASM (constitution II); the F9
requester must not change (FR-016); no new files written to user folders.

**Scale/Scope**: one screen (30 objects), ~600–900 lines in `it_editor.c`, a
small `slibent_t` extension in `it_ris.h`/`it_ris.c`.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)**: **PASS.** No engine file is
  touched; `it_ris.c` is editor-side sample I/O. Gates re-run regardless.
- **Authentic data (II)**: **PASS with recorded deviations.** Every coordinate,
  box style, attribute and string comes from research R1/R5/R6. Three
  deviations, each flagged in README: always-sorted list (R3), no `CACHE.ITS`
  (R4), POSIX drive box (R10).
- **One real engine (IV)**: **PASS.** Loading still goes through
  `RIS_LoadSample` + the existing `lib_load_sample_entry` under the engine lock;
  audition uses the existing check slot and `Music_PlaySample`.
- **Portability (V)**: **PASS.** Drive enumeration is the only platform branch,
  kept beside the existing requester's drive code.

Post-Phase-1 re-check: unchanged.

## Project Structure

### Documentation (this feature)

```text
specs/015-load-sample-screen/
├── plan.md, spec.md, research.md, data-model.md, quickstart.md
├── contracts/screen-contract.md
├── checklists/requirements.md
└── tasks.md            # /speckit-tasks
```

### Source Code

```text
src/it_ris.h / it_ris.c   slibent_t + Date/Time/SortPri; directory listing
                          builder (dirs first, "\" pin, all files, identify,
                          D_SlowSampleSort order)
src/it_editor.c           the screen: object list, list/drive/waveform draws,
                          keys, edits-into-entry, audition, Ctrl-F3 binding,
                          capture aids 13/14, LSS selftest block;
                          sample_library_requester + draw_lib_browser retired
README.md, docs/HANDOFF.md  deviations + status
```

**Structure Decision**: flat `src/` as before. The listing builder lives in
`it_ris.c` next to `RIS_ScanModule`, because it produces `slibent_t` records;
the screen lives in `it_editor.c` with the other screens.

## Implementation Phases

**A — records and listing.** Extend `slibent_t`; add `RIS_ListDirectory`
(dirs first with `DirectoryMsg`, `.`→`\`, all files, identify via the existing
`scan_*`, priority, sort per R3). Unit-level assertions in the selftest.

**B — the screen, read-only.** Object list, list draw with row numbers,
divider, type colours, cursor attrs; drive box; info box; file info
(size/date/time formats); replaces the F9 layout for Load Sample. Capture aid
13. Compare against `load-sample.png`.

**C — navigation and load.** Enter on dir/module/sample, Esc, drive focus,
module listing in the same screen; load path unchanged (no overwrite prompt,
host-instrument prompt kept).

**D — edits, audition, waveform.** Input objects write into the entry; loop
correction; note keys audition into the check slot; waveform only when the
auditioned entry is highlighted.

**E — Space rename, Delete, Ctrl-F3.** Library header variant, capture aid 14,
retire `draw_lib_browser`.

## Complexity Tracking

> No constitution violations requiring justification; deviations are listed
> under Constitution Check and in research R3/R4/R10.
