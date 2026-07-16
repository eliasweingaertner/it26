# Implementation Plan: Pattern Editor Completion

**Branch**: `010-pattern-editor-completion` | **Date**: 2026-07-10 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/010-pattern-editor-completion/spec.md`

## Summary

Port the remaining F2 surface deferred from feature 009: the Ctrl-F2
set-pattern-length dialog (`PE_SetPatternLength` + `O1_SetPatternLength`),
the in-F2 mute/solo key family (`PEFunction_Alt_F9/F10`, MuteNext/Previous,
SoloGotoNext, UnmuteAll), and the multi-scheme view system (`ViewChannels`
table, five view methods, Ctrl-0..5 / Ctrl-Shift-1..4 / Alt-T / Alt-R /
Alt-H, and the Ctrl-T/Ctrl-H toggles). All behaviour decoded 1:1 from
IT_PE.ASM / IT_OBJ1.ASM into [research.md](research.md); implementation is
editor-side only in `src/it_editor.c` behind the existing widget, undo-ring
and selftest infrastructure.

## Technical Context

**Language/Version**: C11 (MSVC `/std:c11`, gcc/clang `-std=c11`)

**Primary Dependencies**: none new — Win32/SDL2/terminal backends already
behind `screen_backend_t`; engine API `Music_ToggleChannel`,
`Music_SoloChannel`, `Music_UnmuteAll` already exported

**Storage**: `ited.cfg` gains the view/toggle prefs (ViewDivision,
CentraliseCursor bits, ViewChannelTracking) alongside existing entries

**Testing**: HANDOFF §4 gates — determinism ×4 `IDENTICAL`,
`test_pattern --roundtrip`, `ITED_SELFTEST=1` (PE block extended),
`ITED_SHOT` visual comparison vs original screenshots

**Target Platform**: Windows / Linux / macOS

**Project Type**: desktop app (faithful DOS-tracker port)

**Performance Goals**: full-frame redraw at interactive rates (unchanged;
renderer stays cell-buffer based)

**Constraints**: engine files untouched; pattern mutation only via
`Pattern_Unpack`/`Pattern_Pack` under `Engine_Lock`/`Unlock`; layout/colors
only from ASM tables

**Scale/Scope**: ~1 file (`it_editor.c`) + selftest additions; est. +900 LOC

## Constitution Check

- **Engine fidelity (I) + Determinism (III)**: PASS — no engine/pattern-format
  file is modified. `Music_*` mute/solo entry points already exist (ported for
  F5). Determinism gate re-run anyway (renderer rework touches nothing under
  it, but the gate is cheap insurance).
- **Authentic data (II)**: PASS — dialog coordinates/bounds, key codes, view
  widths, captions, cursor tables and status strings are decoded byte-exact in
  research.md from IT_PE.ASM/IT_OBJ1.ASM. Two documented original quirks kept
  (persistent PatternSetLength; single-pattern undo for range resize).
- **One real engine (IV)**: PASS — resize path re-uses the editcell grid +
  exact codec under the engine lock; mute/solo call the real engine state.
- **Portability (V)**: PASS — no backend work needed; keys already exist as
  ITK_* combo codes on Win32/SDL (terminal Alt keys are feature 011).

Post-design re-check: no violations introduced. Complexity Tracking: empty.

## Project Structure

### Documentation (this feature)

```text
specs/010-pattern-editor-completion/
├── spec.md
├── plan.md              # this file
├── research.md          # ASM decode (Phase 0)
├── data-model.md        # Phase 1
├── quickstart.md        # Phase 1 (validation guide)
├── checklists/requirements.md
└── tasks.md             # /speckit-tasks output
```

### Source Code (ittrack repo, `C:\Users\elias\fable5\ittrack`)

```text
src/
├── it_editor.c          # all feature work: dialog, key handlers, view-scheme
│                        #   renderer, cursor geometry, toggles, cfg persistence
└── (no other src changes; it_pattern.c / it_music.c used as-is)

tests/  (selftest lives inside it_editor.c's ITED_SELFTEST block)
```

**Structure Decision**: single-file editor change, consistent with features
002–009; new static data mirrors the ASM tables verbatim with source-line
comments.

## Implementation Phases (for /speckit-tasks)

1. **US1 — length dialog**: O1_SetPatternLength object list on the existing
   modal-widget machinery (type-9 thumbbars bound to statics), Ctrl-F2 key,
   resize loop (undo snapshot → per-pattern length set under lock), selftest.
2. **US2 — mute/solo**: six key-table entries calling the existing engine
   functions + PEFunction_Tab; header attr already state-driven; selftest
   cross-checks F2/F5/F11 state.
3. **US3a — view data model + keys**: ViewChannels table, PE_CheckWidth,
   FastView/QuickViewSetup/ViewTrack/ClearViews/ToggleDivision + toggles,
   cfg persistence.
4. **US3b — renderer**: scheme-driven channel iteration in the grid drawer,
   the four new view procs (widths 10/7/3/2, font-B reuse), caption variants,
   division rendering, cursor geometry from CursorPositions, tracking.
5. **Polish**: ITED_SHOT comparisons, README fidelity notes, HANDOFF update,
   full gates.
