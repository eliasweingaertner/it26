# Implementation Plan: Save Module (F10) & Message Editor

**Branch**: `004-save-module` | **Date**: 2026-07-02 | **Spec**: `specs/004-save-module/spec.md`

**Input**: Feature specification from `/specs/004-save-module/spec.md`

## Summary

Port the `.IT` save path from `IT_D_WM.INC` (`D_SaveIT`) + `IT_DISK.ASM`
(`D_SaveSampleDataCompressed`, `WriteBits`, the bit-width LUTs,
`D_SaveBlock`, `D_CheckOverWrite`, `D_SaveModule` filename handling) and
the song message editor from `IT_MSG.ASM` (8000-byte CR-separated
buffer, view/edit modes, word wrap). New editor screen for F10 (save
requester with editable filename + overwrite confirm) and a message
editor screen; loader extended to read the embedded song message
(Special bit 0), which it currently skips. Round-trip gate: load → save
→ reload → byte-identical rendered audio for all four testdata modules.

## Technical Context

**Language/Version**: C11 (MSVC + POSIX), same toolchain as the rest of
`ittrack/`.

**Primary Dependencies**: none new. `src/it_save.c` (new) uses stdio;
editor screens in `src/it_editor.c`.

**Storage**: writes standard `.IT` files (Cwt 0x0217, Cmwt per
SaveFormat; default SaveFormat 3 = IT215 double-delta compression, per
`SWITCH.INC DEFAULTFORMAT = 3`).

**Testing**: `tests/test_pattern.c` determinism gate (unchanged hashes)
plus a new round-trip mode: save each testdata module and re-render
from the saved file — audio hash must equal the original's.

**Target Platform**: Windows/Linux/macOS, backend-agnostic (file I/O +
cell drawing only).

**Project Type**: existing single-project port.

**Performance Goals**: saving a large module completes in well under a
second (32KB-block compressor is O(n) with two minimise passes over
≤32KB tables).

**Constraints**: saving must not mutate the in-memory song (the
original patches sample headers only in its disk buffer, never in song
memory) and must hold no engine state hostage — snapshot under
`Engine_Lock` only around reads of live data.

**Scale/Scope**: one new translation unit (`it_save.c` ~700 lines), a
message module (~300 lines in `it_editor.c` or `it_msg.c`), save
requester + message screen wiring, loader message read (~10 lines).

## Constitution Check

- **Engine fidelity (I) + Determinism (III)**: `it_load.c` gains the
  message read (Special bit 0) — a faithful port of the corresponding
  `D_LoadIT` read that was previously skipped; no playback-affecting
  code changes. `it_music.c`/`it_effects.c`/`it_driver.c` untouched.
  Gate re-run required (all four `IDENTICAL`) plus the new round-trip
  check. The save path itself is new editor-side code transliterated
  from `IT_D_WM.INC`/`IT_DISK.ASM` (which are editor/disk modules in
  the original, not engine modules).
- **Authentic data (II)**: the compressor's bit-width LUTs are built by
  code transliterated from the `D_SaveIT` table-setup block (`BitLUT`,
  `BitLUT16` + run fills), not hand-derived. Save-screen/message-screen
  layouts come from the ASM coordinates (message area x=2, rows 13..47,
  35 lines, colour 12/6 via Ctrl-T, cursor attr rule `(a&8)|30h`).
- **One real engine (IV)**: the saver serialises `Song` (the engine's
  own storage) and packs patterns already in engine format; the current
  pattern is committed via the existing `Pattern_Pack` before saving
  (`PE_SaveCurrentPattern` equivalent).
- **Portability (V)**: stdio only; no backend code.

## Project Structure

### Documentation (this feature)

```text
specs/004-save-module/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── save-format.md
└── tasks.md
```

### Source Code (repository root: `ittrack/`)

```text
ittrack/
├── src/
│   ├── it_save.c        # NEW: D_SaveIT + compressed/plain sample writers
│   ├── it_save.h        # NEW: Save_ITModule(path), SaveFormat
│   ├── it_load.c        # + song message read (Special bit 0)
│   ├── it_editor.c      # + save requester screen, message editor screen,
│   │                    #   F10 / menu wiring, Msg_* buffer + keys
│   └── it_music.h       # (only if a shared decl is needed)
├── tests/
│   └── test_pattern.c   # + save→reload round-trip verification mode
└── docs/HANDOFF.md      # §2/§6 updates
```

**Structure Decision**: the writer is a separate `it_save.c` (mirrors
the original's IT_DISK/IT_D_WM module split and keeps `it_editor.c`
from growing further); the message buffer/editor lives with the editor
(IT_MSG.ASM is a UI module).

## Complexity Tracking

No constitution violations. Deviations recorded in research.md (R7).
