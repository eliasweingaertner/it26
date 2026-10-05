# Implementation Plan: F4 Instrument Editor — Object-Exact Pane & Tabs

**Branch**: `002-f4-instrument-editor` | **Date**: 2026-07-02 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/002-f4-instrument-editor/spec.md`

## Summary

Replace the screenshot-eyeballed F4 right pane with the original object data and
make the Volume/Panning/Pitch tabs real. The original defines the whole page as
four object lists in `IT_OBJ1.ASM` — `O1_InstrumentListGeneral` (line 5629),
`O1_InstrumentListVolume` (5896), `O1_InstrumentListPanning` (6107),
`O1_InstrumentListPitch` (6251) — sharing `InstrumentNameBox`/`InstrumentWindow`
(left list), the four tab buttons, and per-tab boxes/labels/toggles/thumbbars
whose value fields are *instrument-struct offsets* (0x130/0x182/0x1D4 envelope
blocks, 0x11 NNA…0x16 PPS, 0x3A IFC…0x3F MIDI bank hi). Custom-draw objects come
from `IT_I.ASM`: `I_DrawInstrumentWindow` (4653), `I_DrawNoteWindow` (5374, the
General tab's note-translation window), `I_DrawEnvelope`/`I_PreEnvelope`/
`I_PostEnvelope` (6678+, the envelope display and its node editing keys), and
`I_DrawPitchPanCenter` (8799). All of this is ported into the existing widget
framework in `it26/src/it_editor.c`; the engine is untouched. The 2.17 build
uses `FILTERENVELOPES=1` (`SWITCH.INC`), so the Pitch tab includes Default
Cutoff/Resonance and the shifted MIDI rows.

## Technical Context

**Language/Version**: C11 (project-wide; `_Static_assert`).

**Primary Dependencies**: None new. Extends `it_editor.c`'s widget/object
framework; reads/writes the engine's real `instrument_t` (pinned in
`it_structs.h`: `VEnvelope` @0x130, `PEnvelope` @0x182, `PtEnvelope` @0x1D4 —
exactly the offsets the ASM object tables poke).

**Storage**: N/A (session edits only; disk persistence is feature 004).

**Testing**: `tests/test_pattern.c` determinism gate (must stay `IDENTICAL` ×4);
`ITED_SHOT` BMP of each F4 tab vs the IT 2.14 reference screenshots in
`../screenshots/`; `ITED_SELFTEST` smoke; interactive audition of envelope edits.

**Target Platform**: All three backends (Win32/SDL/terminal) — this is
backend-neutral cell-buffer UI code.

**Project Type**: Native desktop application (tracker editor) over the ported
engine.

**Performance Goals**: Rebuild+draw the F4 widget table each frame at
interactive rates, as every other screen does.

**Constraints**: Layout/colors/geometry byte-sourced from `IT_OBJ1.ASM`; envelope
behaviour from `IT_I.ASM`; edits act on the real `instrument_t` under
`Engine_Lock` where the mixer may read them; no engine file is modified.

**Scale/Scope**: One file materially changed (`src/it_editor.c`, roughly
+700–900 lines): 4 tab layouts, note-translation window, envelope
display/editor, ~30 new widgets wired to instrument fields. Possibly small
additions to the widget framework (numeric byte fields with min/max, custom-draw
widget hook) reused by later features.

## Constitution Check

- **Engine fidelity (I) + Determinism (III)** — ✅ PASS. No engine/pattern file
  is touched. Envelope edits mutate song data (allowed; that is the editor's
  purpose) under `Engine_Lock`; the regression modules are not edited by tests,
  so the gate stays `IDENTICAL` ×4.
- **Authentic data (II)** — ✅ PASS (this feature *closes* the standing
  violation): every coordinate/box style/attr/label comes from
  `IT_OBJ1.ASM` 5629–6560 and `IT_I.ASM`; the eyeballed-F4 exception in
  HANDOFF §2 is removed.
- **One real engine (IV)** — ✅ PASS. Widgets bind directly to `instrument_t`
  fields; auditioning uses the existing engine playback path.
- **Portability (V)** — ✅ PASS. Cell-buffer drawing only; no backend code.

**Result**: All gates pass. Complexity Tracking is empty.

## Project Structure

### Documentation (this feature)

```text
specs/002-f4-instrument-editor/
├── plan.md              # This file
├── research.md          # Phase 0 (object-table decode, env offsets, FILTERENVELOPES)
├── data-model.md        # Phase 1 (instrument/env bindings, widget mappings)
├── quickstart.md        # Phase 1 (build + verify tabs/envelopes)
├── contracts/
│   └── f4-objects.md    # Phase 1 (object table → widget contract, per tab)
├── checklists/requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks)
```

### Source Code (repository root: `it26/`)

```text
it26/
└── src/
    ├── it_editor.c   # EDIT: replace draw_instruments' right pane with the four
    │                 #   object-exact tab layouts; add note-translation window,
    │                 #   envelope display + node editor, new widget bindings
    ├── it_screen.[ch] # UNCHANGED (S_DrawBox/DrawStringCtl already 1:1)
    └── it_structs.h  # UNCHANGED (env_t/instrument_t already pinned)
```

**Structure Decision**: Single existing project; all work in `it_editor.c`
following the established per-screen build/draw pattern.

## Complexity Tracking

> No constitution violations. Section intentionally empty.
