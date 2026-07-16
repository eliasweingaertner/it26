# Tasks: F4 Instrument Editor — Object-Exact Pane & Tabs

**Input**: Design documents from `/specs/002-f4-instrument-editor/`
**Prerequisites**: plan.md, research.md, data-model.md, contracts/f4-objects.md

**Organization**: Tasks grouped by user story. All code work is in
`ittrack/src/it_editor.c` (per plan), so most tasks are sequential; [P] marks
the few genuinely independent ones.

## Phase 1: Setup

- [X] T001 Verify baseline: build `ited` + `test_pattern` (HANDOFF §3) and run the determinism gate — all four modules `IDENTICAL` — before touching code, so any later failure is attributable. Files: `ittrack/` build only.

## Phase 2: Foundational (blocking prerequisites)

- [X] T002 Extend the widget framework in `ittrack/src/it_editor.c` with a type-13 equivalent: a 3-digit numeric byte field (`WT_NUM3`) bound to a `uint8_t*` with min/max callback (max = env node count − 1), drawn/focused per `F_Draw3Num`/`F_Pre3Num` (value attr 02h, focus cursor 30h), digits typed to edit, clamp on commit per `F_Post3Num`.
- [X] T003 Extend the widget framework in `ittrack/src/it_editor.c` with a custom-draw widget (`WT_CUSTOM`): draw/pre/post callbacks mirroring the type-15 far-function triple, focusable, receives keys and mouse when focused (needed by the note window, envelope display, and PitchPanCenter field).
- [X] T004 Add F4 tab state to `ittrack/src/it_editor.c`: `InsTab` (0..3), the four tab buttons at the contract C1 coordinates switching the built widget table (equivalent of `I_GetInstrumentScreen`/`I_SelectScreen`), active tab indicated as the original does; keep the existing left list (`InstrumentNameBox` (4,12)-(30,48) + `InstrumentWindow`) shared by all tabs.

**Checkpoint**: Framework ready — user story phases can begin.

## Phase 3: User Story 1 — Authentic F4 layout (P1) 🎯 MVP

**Goal**: Right pane byte-sourced from IT_OBJ1.ASM; eyeballed layout gone.

**Independent Test**: `ITED_SHOT` per tab vs reference screenshots; near-pixel.

- [X] T005 [US1] Rebuild the General tab in `ittrack/src/it_editor.c` from `O1_InstrumentListGeneral` per contract C2: translate box (31,15)-(42,48) style 27, dividers (char 134/154 runs at (44,15)/(44,30)/(44,45) attr 20h), NNA/DCT/DCA headline texts, NNA/DCT/DCA radio buttons at the exact C2 coordinates bound to `NNA/DCT/DCA`, "Filename" text, filename box (55,46)-(73,48) + string input (56,47) → `DOSFileName`.
- [X] T006 [US1] Implement the note-translation window (`I_DrawNoteWindow`, IT_I.ASM 5374) as a `WT_CUSTOM` in `ittrack/src/it_editor.c`: 33 visible rows of the 120-entry `NoteSampleTable` ("note | sample-note" per the proc's format/colours), scroll + cursor state (`NoteWinTop/NoteWinSel`), Up/Down/PgUp/PgDn navigation; display-only editing keys deferred to US2.
- [X] T007 [US1] Build the three envelope tabs' static frame in `ittrack/src/it_editor.c` per contract C3: envelope box (31,17)-(77,26), VE/VEL/VESL boxes, the per-tab headline/loop/susloop texts at their exact x positions (Volume 38/40, Panning 37, Pitch 35) with 0xFF skip codes, plus tab-specific extras: Volume C4 (GlobalVolume box/text, Volume2 bar, FadeOut2 bar, RandomVol box/bar), Panning C5 (DefaultPan box/text/toggle/value, filler row char 9Ah, PPS bar, PanSwing bar, PitchPanCenter custom showing PPC as note name per `I_DrawPitchPanCenter`), Pitch C6 (MIDI box/text, Cutoff/Resonance/Channel/Program/Bank scalable bars, FILTERENVELOPES=1 rows).
- [X] T008 [US1] Implement the envelope display draw (`I_DrawEnvelope`, IT_I.ASM 6678) as `WT_CUSTOM` draw in `ittrack/src/it_editor.c`: render the active tab's `env_t` onto the (32,18)-(76,25) cell grid exactly as the proc does (axis, node markers, loop/susloop indicators, value scaling; read the proc — do not invent glyphs); static display only in this story.
- [X] T009 [US1] Wire all three envelope-tab toggle/numeric widgets read-only-correct in `ittrack/src/it_editor.c`: On/Carry/Loop/SusLoop toggles → env `Flags` bits 1/8/2/4, Loop/SusLoop Begin/End `WT_NUM3` → `LpB/LpE/SLB/SLE` (editing enabled naturally by the framework; bounds per data-model).
- [X] T010 [US1] Visual verification: capture `ITED_SHOT` BMPs of all four tabs (drive `InsTab` via `ITED_SHOT_SCREEN`/selftest hook as needed), convert to PNG, compare against `../screenshots/` references; fix discrepancies (must trace back to the ASM data, not eyeballing). Remove the "F4 right pane approximated" caveat only when matching.

**Checkpoint**: F4 renders object-exact on all four tabs; existing list/NNA/DCT/DCA still work.

## Phase 4: User Story 2 — Operable tabs (P2)

**Goal**: Every control on each tab editable via keyboard and mouse.

**Independent Test**: change every control; values persist and audition reflects them.

- [X] T011 [US2] Wire the Volume tab value widgets in `ittrack/src/it_editor.c`: `GbV` (0..128), `FadeOut` (0..256, scalable width 16), `RV` (0..100) with `Engine_Lock` around mutations; confirm focus/nav order follows the object tables.
- [X] T012 [US2] Wire the Panning tab value widgets in `ittrack/src/it_editor.c`: `DfP` bit 0x80 toggle + 0..64 value bar, `PPS` signed −32..32, `RP` 0..64, PPC editable via the custom field (note entry via piano keys per `I_PostPitchPanCenter`).
- [X] T013 [US2] Wire the Pitch tab value widgets in `ittrack/src/it_editor.c`: `IFC`/`IFR` 0..127, `MCh` 0..17, `MPr`/Bank lo/hi −1..127 (0xFF sentinel display per the original), pitch-env filter mode bit 0x80 handling per IT_I.ASM.
- [X] T014 [US2] Make the note-translation window editable in `ittrack/src/it_editor.c` per `I_PostNoteWindow` keys: piano note keys set the entry's note, digits/sample keys set sample, Enter picks current sample, arrows move; bounds 0..119, sample 0..99.
- [X] T015 [US2] Session persistence + audition check: switch instruments/tabs and confirm values stick (they live in `Song.Ins[]`); audition edited NNA/envelope/fadeout and confirm audible effect through the engine (quickstart Scenario 2).

**Checkpoint**: All three tabs + General fully operable.

## Phase 5: User Story 3 — Envelope graph editing (P3)

**Goal**: Node add/move/delete + loop markers on the envelope display.

**Independent Test**: quickstart Scenario 3.

- [X] T016 [US3] Port `I_PreEnvelope`/`I_PostEnvelope` (IT_I.ASM 6766/6781) node editing into the envelope `WT_CUSTOM` in `ittrack/src/it_editor.c`: current-node selection (left/right), node value/tick moves (the proc's key set), insert/delete node with the proc's bounds (max 25 nodes, tick ordering, node 0 tick 0), loop/susloop clamping when nodes change; all mutations under `Engine_Lock`.
- [X] T017 [US3] Envelope presets if trivially portable (`ENABLEPRESETENVELOPES=1`, Alt-digit in `I_PostEnvelope`): port or flag "not ported yet" on the info line — do not invent behaviour. File: `ittrack/src/it_editor.c`.
- [X] T018 [US3] Mouse editing on the envelope display in `ittrack/src/it_editor.c` if the original supports it (check the 8010h mouse path in IT_I.ASM); otherwise keyboard-only, matching the original exactly.

## Phase 6: Polish & Cross-Cutting

- [X] T019 Run the determinism gate: all four modules `IDENTICAL` with known-good hashes (quickstart Scenario 4).
- [X] T020 [P] Run `ITED_SELFTEST=1` and fix any regressions; extend the selftest script to walk the four tabs and poke one widget per type.
- [X] T021 [P] Update `ittrack/docs/HANDOFF.md` (§2: F4 now object-exact, remove the eyeballed exception; §6: strike roadmap #2) and `ittrack/README.md` fidelity notes if any deviation remains.

## Dependencies

- Phase 2 → everything; T005–T009 in order (same file, shared scaffolding); T010 after T005–T009.
- US2 needs US1 layout; US3 needs US1's envelope display (T008).
- T019–T021 last.

## Implementation Strategy

MVP = Phase 3 (authentic layout). Then US2 (operability), US3 (node editing).
Single-file feature: expect sequential execution; commit after each checkpoint.

