# Implementation Plan: Pattern Editing Depth

**Branch**: `009-pattern-editing-depth` | **Date**: 2026-07-05 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/009-pattern-editing-depth/spec.md`

## Summary

Port the remaining IT 2.17 F2 pattern-editor surface from the `IT_PE.ASM`
key table and `PEFunction_*` handlers: block marking + the full Alt-key
operation set, the edit mask / multichannel / template entry pipeline
(`PE_NewNote`), the original row verbs plus the 10-slot typed undo history
with its Ctrl-Backspace picker, navigation depth, F2 conveniences and view
schemes, thumbbar digit entry, and the key-layer groundwork (new `ITK_*`
codes incl. shift press/release events; keyjazz map reduced to the
original `KeyBoardTable`). The decode contract is research.md; three draft
assumptions were corrected there (R0): block semantics live in IT_PE.ASM
(not PE_TRANS.INC), undo is a 10-entry typed history with a requester, and
`,` toggles the cursor-field mask bit.

## Technical Context

**Language/Version**: C11 (MSVC `/std:c11`, gcc/clang `-std=c11`)

**Primary Dependencies**: none new — `src/it_editor.c` (grid editing,
widget framework, selftest), `src/it_screen.h` + the three backends (new
key codes / shift events), `src/it_pattern.c` (unchanged codec, used by
snapshots), `src/it_music.c` only via existing entry points (play from
mark, mute/solo — `Music_*` functions already ported for F5/F11)

**Storage**: none new; session-local clipboard/undo/mask state; `ited.cfg`
gains only what `IT.CFG` persists (verified during implementation, R9.6)

**Testing**: new selftest `PE OK` block (scripted key batteries: mark →
op → assert cells; undo revert byte-equality; mask/template entry); the
standing gates: determinism ×4 `IDENTICAL`, `--roundtrip`, full selftest
headless on Win32 + Linux/SDL

**Target Platform**: Windows / Linux / macOS; pixel backends get the full
key set, terminal backend's missing modifier combos stay roadmap-#7
leftovers (documented)

**Project Type**: native desktop application (tracker editor over the
1:1-ported engine)

**Performance Goals**: editing operations are O(pattern) on 64×200 cells —
imperceptible; undo memory bounded at 10 pattern snapshots (original's own
bound)

**Constraints**: handler behaviour 1:1 from the cited ASM lines, quirks
included; the explicit cell-mask model stays authoritative; every mutation
under `Engine_Lock`; pattern data round-trips through the exact
`Pattern_Pack/Unpack` codec only at commit boundaries

**Scale/Scope**: ~60 handlers across 5 story groups (research.md R2..R8
registry); the largest editor feature since the stage-4 rework — staged
delivery by user story (R9.1)

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Gates derived from the constitution (`.specify/memory/constitution.md` v1.0.0):

- **Engine fidelity (I) + Determinism (III)**: No engine files are
  planned to change. Play-from-mark / mute-solo use already-ported
  `Music_*` entry points. If implementation discovers a missing engine
  entry point, it is ported 1:1 and the §4 regression re-runs — the
  determinism ×4 gate is mandatory in the final task regardless.
  **PASS.**
- **Authentic data (II)**: All key bindings, mark colours, the undo
  requester layout (`O1_UndoList`), the multichannel dialog
  (`O1_SelectMultiChannel`) and status strings come from the ASM data
  (IT_PE.ASM 300..345, IT_OBJ1.ASM object lists) — no invented UI. The
  keyjazz reduction *removes* an invented extension. **PASS.**
- **One real engine (IV)**: All edits happen in the unpacked grid and
  round-trip through `it_pattern.c`'s exact codec under
  `Engine_Lock`/`Unlock`; clipboard/undo snapshots use the same
  `editcell_t` model with its authoritative mask bits. **PASS.**
- **Portability (V)**: New key codes go into the shared `ITK_*` enum
  behind the `screen_backend_t` vtable; per-backend translation only in
  the backend files; terminal gaps documented, no platform `#ifdef`s in
  shared code. **PASS.**

*Post-design re-check*: unchanged — no violations; Complexity Tracking
stays empty.

## Project Structure

### Documentation (this feature)

```text
specs/009-pattern-editing-depth/
├── plan.md              # This file
├── research.md          # Phase 0: decoded IT_PE.ASM contract (R0 corrections!)
├── data-model.md        # Phase 1: editor state model additions
├── quickstart.md        # Phase 1: build & validation walkthrough
├── contracts/
│   └── pe-depth.md      # Phase 1: internal interfaces touched
└── tasks.md             # Phase 2 (/speckit-tasks)
```

### Source Code (repository root: `it26/`)

```text
it26/
├── src/
│   ├── it_editor.c        # the bulk: 9-column cursor model, mark state +
│   │                      #   rendering, block ops, PE_NewNote pipeline
│   │                      #   (mask/multichannel/template), row verbs,
│   │                      #   undo ring + requester, navigation, view
│   │                      #   schemes, conveniences, selftest PE block
│   ├── it_screen.h        # new ITK_* codes + shift press/release events
│   ├── it_screen_win32.c  # VK translation for the new combinations
│   ├── it_screen_sdl.c    # SDL translation for the new combinations
│   ├── it_screen.c        # terminal backend: document non-delivered combos
│   └── it_pattern.c       # UNCHANGED (codec reused by snapshots)
├── docs/HANDOFF.md        # status + §6 roadmap updates per stage
└── README.md              # fidelity notes (keyjazz reduction note)
```

**Structure Decision**: single-project layout; everything lands in the
existing editor + screen layer. Staged by user story (R9.1): key plumbing
(US5 groundwork) → US1 marking/block ops → US2 entry pipeline → US3 row
verbs/undo → US4 navigation/conveniences, each stage ending with its
selftest battery + the standing gates.

## Complexity Tracking

No constitution violations — table intentionally empty.
