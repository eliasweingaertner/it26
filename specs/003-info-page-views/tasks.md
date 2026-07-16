# Tasks: Info Page (F5) — View Methods, Split Windows, Solo

**Input**: Design documents from `/specs/003-info-page-views/`
**Prerequisites**: plan.md, research.md, data-model.md, contracts/info-page.md

Note: spec US1 "oscilloscope" is delivered as IT 2.17's authentic
velocity bars (research R1); sample dots are excluded (commented out of
the 2.17 build). Both recorded as spec deviations in the fidelity notes.

## Phase 1: Setup

- [X] T001 Baseline: build `ited` + `test_pattern`, run the determinism gate (all four `IDENTICAL`) before changes. Files: `ittrack/` build only.

## Phase 2: Foundational

- [X] T002 Port `Music_SoloChannel`, `Music_ToggleReverse`, `Music_GetLastChannel` (and `Music_NextOrder`/`Music_LastOrder` if compact) 1:1 from `IT_MUSIC.ASM` into `ittrack/src/it_music.c` + prototypes in `ittrack/src/it_music.h`; re-run the determinism gate (engine touched).
- [X] T003 Replace the first-pass F5 state in `ittrack/src/it_editor.c` with the `DisplayWindows` model (data-model): `dispwin_t[5]`, `NumWindows=3` with the 2.17 defaults, `CurrentWindow/ProcessWindow`, `InfoCurrentChannel`, `InfoVelocity/InfoInstrumentNames/InfoFullScreen`; `draw_info` walks windows per `DrawDisplayData` incl. the one-row-taller quirk (contract C1); method dispatch table with stubs falling back to the current track view.

## Phase 3: User Story 1 — Velocity bars in the track view (P1) 🎯 MVP

- [X] T004 [US1] Rework the track view in `ittrack/src/it_editor.c` to `Display_HostChannel` exactly (contract C2): window-parameterised boxes, `DrawChannelNumbers` colours, sample-number "--"≥100, instrument-mode "/ii", ':' attr 7/6/4 states, 25-char names honouring the 'I' toggle, pan column per the proc.
- [X] T005 [US1] Implement the velocity bar in `ittrack/src/it_editor.c`: loop-aware span from `OldSampleOffset`→`SampleOffset` (double for 16-bit, high-byte scan), min/max scan of real sample memory under `Engine_Lock`, value = (max−min)·FV>>8 rounded; chars 176/179/182 + 173+n tip, attr 05h/01h; `InfoVelocity` toggle ('V') switches to plain FV bars.
- [X] T006 [US1] Verify live: play itdemo, bars move with audio and flatten on silence; 'V' toggles (quickstart Scenario 1).

## Phase 4: User Story 2 — The other view methods (P2)

- [X] T007 [US2] Implement `Display_Variables` (method 8) in `ittrack/src/it_editor.c` per contract C4 (active/virtual counts under lock, attr 20h/23h).
- [X] T008 [US2] Implement `Display_NoteDots` (method 9) in `ittrack/src/it_editor.c` per contract C5 (73-column dot grid, glyphs 193..201, colour rules, disowned-overwrite rule).
- [X] T009 [US2] Implement the pattern-view decode helper in `ittrack/src/it_editor.c`: unpack the playing pattern's row window via the existing exact decoder under lock (research R6) with the `DataDecode` semantics.
- [X] T010 [US2] Implement `Display_5Channel`/`8`/`10` in `ittrack/src/it_editor.c` per their procs (headers " Channel xx "/"  xx  ", row centring, per-cell note/ins/vol/efx columns, current-row hilight, GetChannelColour).
- [X] T011 [US2] Implement `Display_18Channel`/`24`/`36`/`64` in `ittrack/src/it_editor.c` per their procs (denser columns).
- [X] T012 [US2] Implement `Display_Details` (method 10) in `ittrack/src/it_editor.c` per the proc + `DetailsMsg` header (NNA/freq/position/volume columns).
- [X] T013 [US2] Wire method cycling PgUp/PgDn (mod 11) + the 'I' instrument/sample names toggle + '+'/'-' order skip (if T002 ported them, else info-line flash). File: `ittrack/src/it_editor.c`.

## Phase 5: User Story 3 — Split windows & solo (P3)

- [X] T014 [US3] Implement window keys in `ittrack/src/it_editor.c` per contract C1: Tab/Shift-Tab focus, Ins split (cap 5, len>6, halve), Del merge (min 1, recombine rule), Ctrl-U/Ctrl-D resize (Alt-Up/Down stand-in, len≥3), Ctrl-F fullscreen toggle.
- [X] T015 [US3] Wire channel selection & engine actions in `ittrack/src/it_editor.c`: Up/Down/Home/End channel select with per-window topchan clamping, shift-'Q' toggle channel, shift-'S' solo (T002), 'G' goto pattern + Space per `Display_SpaceBar`/`Display_GotoPattern`, Alt-only leftovers flash "needs Alt" (documented).
- [X] T016 [US3] Verify solo/unsolo restores prior mute state; muted colours track `GetChannelColour` across all views (quickstart Scenario 4).

## Phase 6: Polish & Cross-Cutting

- [X] T017 Determinism gate ×4 (`IDENTICAL`, known hashes).
- [X] T018 Extend `ITED_SELFTEST` in `ittrack/src/it_editor.c` to cycle all 11 methods, split/resize/merge windows, toggle 'V'/'I', and report `F5 OK`.
- [X] T019 [P] Update `ittrack/docs/HANDOFF.md` (§2 F5 entry, §6 strike roadmap #3) and `ittrack/README.md` fidelity notes (velocity-bars-not-oscilloscope, sample dots excluded, Ctrl-U/D resize stand-in).

## Dependencies

- T002/T003 before all stories; US1 (T004-T006) → MVP; US2 needs T003 dispatch; US3 needs T002 (solo) + T003 (windows).
- T017-T019 last.

## Implementation Strategy

MVP = velocity bars on the reworked track view. Then methods, then
window management + solo. Single file except the small engine addition.
