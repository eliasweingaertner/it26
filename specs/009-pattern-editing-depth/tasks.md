# Tasks: Pattern Editing Depth

**Input**: Design documents from `/specs/009-pattern-editing-depth/`

**Prerequisites**: plan.md, spec.md, research.md (the decode contract —
every task's ASM line references live there), data-model.md,
contracts/pe-depth.md, quickstart.md

**Tests**: FR-009 mandates selftest + regression gates → per-story
selftest tasks are included.

**Organization**: Staged per plan.md: Foundational key/cursor plumbing
first, then user stories. Code paths relative to
`C:\Users\elias\fable5\ittrack`. ASM references are to
`impulse-tracker-jthlim/IT_PE.ASM` unless noted.

## Format: `[ID] [P?] [Story] Description`

## Phase 1: Setup (state scaffolding)

- [X] T001 Add the 009 editor-state scaffolding to `src/it_editor.c`
      (compiles unused): mark rect + anchor vars, clipboard
      (`editcell_t*` + dims), `undoslot_t` ring[10], pattern scratch,
      `EditMask`/`MaskChange[9]`, `MultiChannelInfo[64]`, `Template`,
      `Amplification=100`/FastVolume, play-mark vars, `LastKeys[3]`
      last-key history helper (push on every F2 key; double-press
      queries per research.md R1). Mirror names/initial values from
      IT_PE.ASM 213..345.

---

## Phase 2: Foundational (blocking prerequisites)

**⚠️ Blocks all stories**: new keys, shift events, keyjazz reduction,
9-column cursor, table dispatch.

- [X] T002 Extend the `ITK_*` enum in `src/it_screen.h` (append-only):
      Ctrl-arrows, Ctrl-Home/End, Ctrl/Shift-PgUp/PgDn, Ctrl-Ins/Del,
      Ctrl-Backspace, Scroll Lock, Ctrl-0..5, Ctrl-Shift-1..4, numpad
      4/8, `ITK_SHIFT_DOWN`/`ITK_SHIFT_UP` (data-model.md "Key layer").
- [X] T003 [P] Translate the new combinations in `src/it_screen_win32.c`
      (WM_KEYDOWN/UP + modifier state; Shift press/release events from
      VK_SHIFT transitions; numpad distinction via extended-key bit).
- [X] T004 [P] Same for `src/it_screen_sdl.c` (SDL keysym + mod state;
      keypad SDLK_KP_4/8). Terminal backend (`src/it_screen.c`): no new
      sequences — add the roadmap-#7 leftover comment.
- [X] T005 Reduce `key_to_note` in `src/it_editor.c` to the original
      `KeyBoardTable` (IT_PE.ASM 254..261: Z-row 12 + Q-row 17; drop
      `;` `,` `.` `l` `/`). Verify the F4 note window, lists and
      selftest scripts still work (they use Z/Q-row keys — research R6).
- [X] T006 Widen the F2 cursor to the original 9 columns in
      `src/it_editor.c`: `CurCol` 0..8 per `PE_PatternCursorPos0..8`
      (3885..5541) — drawing (cursor cell per column), Left/Right/Tab
      movement, mouse hit-testing, and split digit entry (ins
      tens/units, vol tens/units, cmd, param hi/lo) with the original
      per-column semantics.
- [X] T007 Refactor `handle_pattern_key` into an ordered
      (modifier,key)→handler table mirroring IT_PE.ASM 380..860
      (contracts/pe-depth.md), existing behaviour preserved; the ported
      keys keep working (instrument cycling, octave, edit step, pattern
      ±1). Build + full existing selftest green before stories start.

**Checkpoint**: plumbing done — stories can proceed.

---

## Phase 3: User Story 1 - Mark a block and operate on it (P1) 🎯 MVP

**Goal**: full marking + the Alt-key block operation set, snapshotting
into the undo ring (picker UI comes in US3).

**Independent Test**: quickstart.md US1 battery + selftest mark/op
assertions.

- [X] T008 [US1] Mark model + rendering in `src/it_editor.c`:
      `BlockMark/Left/Top/Right/Bottom` with Alt-B/E normalise-by-swap
      semantics (5555/5603), Alt-D bar-mark + repeat-doubles (5651),
      Alt-L track + repeat-widens (5994), Alt-U (7093); mark colour in
      `draw_pattern` per `PE_SelectColour` (8856).
- [X] T009 [US1] Shift-marking: anchor on `ITK_SHIFT_DOWN` (3493),
      extension in the movement handlers, release semantics w/
      `NoteEntered` (3510); Shift-PgUp/PgDn/Home/End variants from the
      key table.
- [X] T010 [US1] Undo ring core in `src/it_editor.c`:
      `snapshot_undo(type)` per `PE_AddToUndoBuffer` (11319 — drop
      oldest, shift, store; Modified flags except type 22); wire into
      every mutating op below per the R3 type column. (Requester UI =
      T022.)
- [X] T011 [US1] Clipboard + Alt-C copy (`BlockCopy` 6580) and the
      shared error messages (`NoBlockMarked/OutOfMemory/NoBlockData`
      6663..6707) on the status line.
- [X] T012 [US1] Paste family: Alt-P (`BlockPaste` 6782), Alt-O
      (`BlockOverWrite` 6707), Alt-M + repeat-precedence
      (`BlockMix`/`SecondBlockMix` 7007/6898) — field rules 1:1.
- [X] T013 [P] [US1] Alt-Z cut (`WipeBlock` 6038), Alt-Y swap
      (`BlockSwap` 6430), Alt-F/G double/halve (`BlockDouble`/`Halve`
      6326/6240).
- [X] T014 [P] [US1] Alt-Q/A transpose (`SemiUp/Down` 7119/7193, clamp
      0..119, NONOTE/special notes untouched).
- [X] T015 [P] [US1] Volume/effect ops: Alt-J amplify + prompt +
      `Amplification` memory (`VolumeAmp` 7418) with Ctrl-J fast mode
      (`ToggleFastVolume` 11622); Alt-K slide / 2×Alt-K wipe (`AltK`
      5805 + `PEGetVolume` 5761); Alt-V set (`BlockVolume` 8417); Alt-W
      wipe excess (`WipeExcessVolumes` 8474); Alt-X effect slide /
      2×Alt-X wipe (`SlideCommand`/`WipeCommands` 7267/7355); Alt-S set
      sample/instrument (`AltS` 5700).
- [X] T016 [US1] Selftest `PE OK` block, US1 battery: script mark →
      each op → pack/unpack → assert cells against precomputed
      expectations; block leaves song unmodified at exit
      (contracts/pe-depth.md). Build + selftest green.

**Checkpoint**: block editing shippable (MVP).

---

## Phase 4: User Story 2 - Edit mask, multichannel, template (P2)

**Independent Test**: quickstart.md US2 + selftest entry assertions.

- [X] T017 [US2] `EditMask`/`MaskChange` consultation in note entry:
      port `PE_NewNote` (4650) semantics — write only mask-enabled
      fields, preserve others; `,` = `PEFunction_SetMask` (3752) XOR of
      the cursor field's bit; original default mask value.
- [X] T018 [US2] Multichannel: Alt-N toggle + 2×Alt-N
      `O1_SelectMultiChannel` dialog (3765, IT_OBJ1.ASM object list);
      `PE_GotoNextInput` (4087) advancement and Backspace honouring
      (3786).
- [X] T019 [US2] Template modes: Alt-I cycle (`ToggleTemplate` 8660,
      status msgs 343..345), `:` off (8682); stamping via `PE_Template`
      (4555) + the four variants (4246/4306/4389/4472), clipboard
      transposed to the entered note.
- [X] T020 [US2] Selftest US2 battery: mask combinations, multichannel
      advancement, template stamp assertions. Build + selftest green.

---

## Phase 5: User Story 3 - Row verbs and the undo requester (P2)

**Independent Test**: quickstart.md US3 + revert byte-equality.

- [X] T021 [US3] Row verbs 1:1: track-local Ins/Del
      (`Insert`/`Delete` 4805/4733 — replace the port's simplified
      versions), Alt-Ins/Del all channels (`RowInsert/Delete`
      4937/4882, undo types 19/20), Ctrl-Ins/Del roll
      (`RollDown/Up` 6172/6104), Backspace step-back (3786) with
      `SkipValue`.
- [X] T022 [US3] Undo requester: Ctrl-Backspace opens the
      `O1_UndoList` modal (11461, layout/attrs from IT_OBJ1.ASM;
      Draw/Pre/PostUndo 11476..11620) listing slots by type string
      (300..322); revert restores the snapshot and pushes Redo (21);
      "Pattern N" caption entries (22).
- [X] T023 [US3] Selftest US3 battery: each verb vs expected cells;
      undo revert byte-equality through pack/unpack; redo. Build +
      selftest green.

---

## Phase 6: User Story 4 - Navigation depth & conveniences (P3)

**Independent Test**: quickstart.md US4 key-by-key.

- [X] T024 [US4] Movement handlers from the R5 registry:
      Ctrl-PgUp/PgDn (3335/3346), Alt-Home/End (3531/3600),
      Home/End cascades (3658/3684), Alt-arrows (10376..10461),
      Ctrl-Left/Right view scroll (10321/10348), Ctrl-Home/End
      (10410/10424), PgUp/PgDn parity vs `RowHiLight` semantics
      (3535/3604) incl. `PE_CentraliseCursor` (3583).
- [X] T025 [P] [US4] Pattern navigation: ±4 (`Next4/Last4Patterns`
      8273/8300), order-follow Ctrl-+/- (`Next/LastOrderPattern`
      8364/8326), trace-aware ±1 alignment (8202/8238) with the
      existing -/= keys.
- [~] T026 [P] [US4] View schemes — **DEFERRED** (leftover). Ctrl-0..5 /
      Ctrl-Shift-1..4 / Alt-T track view need the original's
      multi-view-method pattern renderer, which the port does not have
      (its grid is a single fixed 13-column view). Documented as a
      deviation in README + HANDOFF §6 #5.
- [~] T027 [P] [US4] Toggles + dialogs — **PARTIAL**. Done: Centralise
      (Ctrl-C, pins the cursor mid-view), Trace (Scroll Lock).
      Deferred: Tracking/RowHilight/Division visible toggles,
      `PE_SetPatternLength` resize dialog, `ToggleDefaultVolume` —
      HANDOFF §6 #5 leftovers.
- [~] T028 [P] [US4] Scratch & play aids — **PARTIAL**. Done: Ctrl-F7
      play mark, Enter PickUp (full field alignment). Deferred: Alt-0
      store/restore scratch (Alt-0 is bound to cursor-step-0 as in the
      original; the Alt-Backspace/Alt-Enter scratch pair is a leftover),
      numpad 4/8 play note/row, mute/solo keys — HANDOFF §6 #5.
- [X] T029 [US4] Selftest US4 spot-checks: the `PE OK` block exercises
      cursor/mark/pattern transitions; view-scheme/scratch equality
      checks fold into the deferred items above.

---

## Phase 7: User Story 5 - Thumbbar numeric entry (P3)

- [X] T030 [US5] Digit entry on thumbbars in the widget framework
      (`src/it_editor.c`): '0'..'9' opens the accumulator, Enter
      commits clamped, ESC cancels, per `F_PostThumbBar` (IT_F.ASM
      2125..); works on F3/F4/F12 bars and the pattern-length dialog;
      selftest: scripted digits on a F12 bar assert the value.

---

## Phase 8: Polish & Cross-Cutting

- [X] T031 [P] Docs: `README.md` fidelity notes (keyjazz reduction,
      10-slot undo, any US4 view-scheme deviations);
      `docs/HANDOFF.md` §2 feature block + §6 roadmap #5 done with
      leftovers (terminal combos → #7, MIDI-in excluded).
- [X] T032 [P] Linux/SDL verification: full selftest + manual combo
      spot-check (Ctrl-arrows, Ctrl-Backspace, Shift-marking) per
      quickstart.md; terminal backend graceful-absence check.
- [X] T033 Final gates: determinism ×4 `IDENTICAL`,
      `test_pattern --roundtrip` on itdemo, full selftest (all blocks
      incl. `PE OK`), warning-free MSVC build; DOSBox spot-check of a
      block-op battery against the real IT 2.17 (quickstart US1).

---

## Dependencies

- Phase 2 (T002..T007) blocks everything; T003/T004 [P] after T002;
  T005..T007 sequential in `it_editor.c`.
- US1: T008→T009 (marking before shift-marking), T010 before
  T012..T015 (ops snapshot), T011 before T012/T013 (clipboard); T013/
  T014/T015 [P]-independent op groups after T010/T011; T016 last.
- US2 after US1's T010 (mask entry doesn't need blocks; template needs
  clipboard T011). US3's T022 needs T010; T021 independent of US2.
  US4/US5 independent of US2/US3 except shared file ordering.
- Phase 8 last; T031/T032 [P].

## Parallel Opportunities

- T003/T004 (different backend files). Within US1: T013/T014/T015 are
  disjoint handler groups. US4: T025..T028 mostly disjoint. T031/T032.
- Single-file reality: most tasks touch `it_editor.c` — "parallel"
  means order-free, not simultaneous edits.

## Implementation Strategy

Foundational (T001..T007) → **MVP = US1** (T008..T016, shippable block
editing) → US2/US3 (entry pipeline + undo UI) → US4/US5 → polish.
Commit per phase (matching the repo's one-feature-per-commit style, a
commit per checkpoint here); run the full selftest at every checkpoint
and the determinism gates at least at T033 (and after any engine
touch). Total: 33 tasks (US1: 9, US2: 4, US3: 3, US4: 6, US5: 1,
setup/foundation: 8, polish: 3).
