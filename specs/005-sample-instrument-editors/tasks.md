# Tasks: In-Depth Sample & Instrument Editors

**Input**: Design documents from `/specs/005-sample-instrument-editors/`
**Prerequisites**: plan.md, research.md, data-model.md, contracts/sample-editor.md

Note: spec deviations per research R1 — IT 2.17 has no draw/selection/
zoom; US1/US2 deliver the authentic waveform view + Alt-op set.

## Phase 1: Setup

- [X] T001 Baseline gates (determinism ×4 + roundtrip ×4 `IDENTICAL`).

## Phase 2: Foundational

- [X] T002 Alt-key layer: `ITK_ALT_A..Z/0..9/INS/DEL/UP/DOWN/PLUS/MINUS`, `ITK_CTRL_PLUS/MINUS` in `ittrack/src/it_screen.h`; Win32 WM_SYSKEYDOWN mapping in `it_screen_win32.c`.
- [X] T003 Modal helpers in `ittrack/src/it_editor.c`: `prompt_number()` (title/default/max), parameterised `confirm_box()`, 3-way `quality_dialog()`.

## Phase 3: User Story 1 — Waveform view & loops (P1) 🎯 MVP

- [X] T004 [US1] `I_DrawWaveForm` port: 176×32 canvas per contract C1 into the F3 box; regenerate in draw_samples.
- [X] T005 [US1] Editable F3 fields per contract C2 (filename input, C5 speed, loop/susloop tri-states + begin/end numbers) with `I_CheckLoopValues`/`I_CheckSusLoopValues` clamps + slave loop refresh under lock.

## Phase 4: User Story 2 — Sample ops (P2)

- [X] T006 [US2] Simple in-place ops: Alt-A convert, Alt-I invert, Alt-H centre (scan + confirm + offset), Alt-M amplify (scan + suggested % + 16.16 multiply + clip), Alt-G reverse (+ loop mirroring).
- [X] T007 [US2] Length ops: Alt-B cut-before-loop, Alt-L cut-after-loop, Alt-E/Alt-F resize (I_ReMix incl. interpolation + loop scaling), Alt-Q quality 3-way (convert/reinterpret).
- [X] T008 [US2] Speed & misc: Alt-+/−, Ctrl-+/− (exact multipliers), Alt-Y stub, Alt-C clear name, Alt-D delete (confirm + free + clear).
- [X] T009 [US2] Slot ops: Alt-Ins/Del insert/remove with reference fixups (NoteSampleTable / pattern bytes via unpack-adjust-pack incl. the edit grid), Alt-S swap, Alt-X exchange, Alt-R replace, Alt-J scale sample volumes.

## Phase 5: User Story 3 — Instrument side (P3)

- [X] T010 [US3] Instrument slot ops: Alt-Ins/Del, swap/exchange/replace/copy prompts, Alt-J scale instrument volumes (GbV cap 128), Alt-C clear name.
- [X] T011 [US3] Note-window Alt ops (F4 General tab): Alt-A/N/P, Alt-Up/Down transpose, Alt-Ins/Del rows, Enter pickup, </> sample dec/inc.
- [X] T012 [US3] Envelope presets per contract C5 (digit load, Alt-digit save, compensate handling, Enter-grab auto-enable).

## Phase 6: Polish

- [X] T013 Gates ×8 + extended selftest (`F3 OK` per contract C6).
- [X] T014 [P] HANDOFF §2/§6 + README deviations (no draw/zoom in 2.17, Alt-Y stub, Alt-O/T/W deferred to 006, terminal backend has no Alt keys).

## Dependencies

T002/T003 → everything; T004 → T005; ops independent after T003.

## Implementation Strategy

MVP = waveform + loop editing; ops land in three batches (T006-T009);
instrument side last.
