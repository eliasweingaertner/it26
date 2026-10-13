---

description: "Task list for feature 017: Pattern Editor Mouse Selection, Context Menu and Scale-Aware Note Interpolation"
---

# Tasks: Pattern Editor Mouse Selection, Context Menu and Scale-Aware Note Interpolation

**Input**: `specs/017-pattern-editor-mouse-menu/` (spec.md, plan.md)

**Tests**: `ITED_SELFTEST` (DLG block covers the F1 help lines); port-only harness scripts in `build-compare/_work` (ctx_test*.py) drive the remote backend.

## Phase 1: Input

- [X] T001 Add `ITK_MOUSE_RIGHT` in src/it_screen.h
- [X] T002 [P] Right click in src/it_screen_win32.c (`WM_RBUTTONDOWN`)
- [X] T003 [P] Right click in src/it_screen_sdl.c (`SDL_BUTTON_RIGHT`)
- [X] T004 [P] Right click in src/it_screen.c (SGR button 2 press)
- [X] T005 [P] Remote `mouse x y b`, bit 1 = right click, in src/it_screen_remote.c

## Phase 2: US1 Drag selection (P1)

- [X] T006 [US1] `pattern_cell_at`, `pattern_click`, `pattern_drag`, `PeDrag` in src/it_editor.c (commit 5092a01)
- [X] T007 [US1] README fidelity note: mouse in the pattern editor

## Phase 3: US2 Context menu (P1)

- [X] T008 [US2] `PeContextItems`, `pe_context_menu` (no heading, clamped at the pointer, cursor moves on a click outside the block)
- [X] T009 [US2] IT-key items: Copy, Paste, Transpose up/down, Interpolate volume/effect
- [X] T010 [US2] Clear (`ctx_clear`) with undo type 23
- [X] T011 [US2] Transpose by... (`transpose_dialog`, `ctx_transpose_n`) with undo type 25
- [X] T012 [US2] `UndoTypeName[26]` entries 23–25

## Phase 4: US3 Interpolate notes (P1)

- [X] T013 [US3] Scale / root tables, Step field and `interp_dialog`, `interp_draw`
- [X] T014 [US3] `ctx_interp_notes`: run building, notes on the edit step, endpoints kept, instrument copy, undo type 24
- [X] T015 [US3] Ctrl-K (0x0B) in `handle_pattern_key`; menu label "(Ctrl-K)"
- [X] T016 [US3] Verify SC-001/SC-002 (ctx_test5.py)

## Phase 5: US4 Key detection (P2)

- [X] T017 [US4] `interp_histograms` (order list, live Grid, muted channels skipped)
- [X] T018 [US4] `interp_key_of` (Krumhansl-Schmuckler, ≥ 8 notes, correlation ≥ 0.5)
- [X] T019 [US4] `interp_detect_key` fallbacks + `InterpSource` shown by the root
- [X] T020 [US4] Verify SC-003 (ctx_test4.py)

## Phase 6: US5 Help and polish (P3)

- [X] T021 [US5] F1 help lines for context 1 in `help_lines`; selftest DLG block updated
- [X] T022 Selftest 20/20
- [X] T023 README fidelity notes: right-click menu, Ctrl-K, scale-aware interpolation, Transpose by..., Clear
- [X] T024 Commit (with the user's go-ahead)
