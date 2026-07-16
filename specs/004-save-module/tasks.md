# Tasks: Save Module (F10) & Message Editor

**Input**: Design documents from `/specs/004-save-module/`
**Prerequisites**: plan.md, research.md, data-model.md, contracts/save-format.md

## Phase 1: Setup

- [X] T001 Baseline: build `ited` + `test_pattern`, run the determinism gate (all four `IDENTICAL`).

## Phase 2: Foundational

- [X] T002 Create `ittrack/src/it_save.h` + `it_save.c`: `SaveFormat` (default 3), `save_block()` with sticky error + delete-on-error, header/orders/offset-table emission per contract C1 (OrdNum backward scan, Cwt/Cmwt rules incl. the 0x216 filter-envelope override, Special bits, Reserved obfuscation), instrument block, provisional + rewritten sample-header block, pattern blocks.
- [X] T003 [P] Message buffer in `ittrack/src/it_editor.c`: `MessageData[8000]` + `Msg_Reset/GetLength` and the externs `it_save.h` declares; `ittrack/src/it_load.c` reads the message when `Special & 1` (clamp 7999) and clears it on every load.

## Phase 3: User Story 1 — Save a module (P1) 🎯 MVP

- [X] T004 [US1] Port `D_SaveSampleData` (raw, SaveFormat 2) and the LUT table-setup + `WriteBits` + `D_SaveSampleDataCompressed` (8-bit and 16-bit paths, IT214/IT215) into `ittrack/src/it_save.c` per research R2.
- [X] T005 [US1] Round-trip harness: `--roundtrip` mode in `ittrack/tests/test_pattern.c` (save each module with SaveFormat 3/0/2, reload, hash equal) — run on all four testdata modules.
- [X] T006 [US1] Wire plain saving: F10-less path `Save_ITModule(current path)` callable from the File menu ("Save Current", Ctrl-S) with confirm-on-existing; "Saved."/"Unable to save" statuses.

## Phase 4: User Story 2 — Save requester (P2)

- [X] T007 [US2] Save requester screen in `ittrack/src/it_editor.c`: F9-style lists + editable filename field primed with the loaded name; Enter applies `.IT` when no '.', Esc cancels (contract C3).
- [X] T008 [US2] Overwrite confirm modal (Yes/No, default No) + F10/menu wiring; header File Name updates after successful save.

## Phase 5: User Story 3 — Message editor (P3)

- [X] T009 [US3] Message screen (view + edit modes) in `ittrack/src/it_editor.c` per contract C4: draw (rows 13..47, attr rules, CR/end markers, cursor), view keys, edit keys (arrows/Home/End/PgUp/PgDn/Ins/Del/Backspace/Ctrl-Y/Tab/Esc/printables/Enter), word wrap, buffer-full status, Ctrl-T colour toggle, clear with confirm.
- [X] T010 [US3] Menu/key entry for the message editor; message persists through save (C1) and reload (T003).

## Phase 6: Polish & Cross-Cutting

- [X] T011 Determinism gate ×4 (`IDENTICAL`, known hashes) + round-trip gate ×4.
- [X] T012 Extend `ITED_SELFTEST`: type into the message editor, save to a temp file, reload, verify message + audio hash; report `SAVE OK`.
- [X] T013 [P] Update `ittrack/docs/HANDOFF.md` (§2 save/message entry, §6 strike roadmap #4) and `ittrack/README.md` fidelity notes (timer blocks omitted, Alt-C stand-in).

## Dependencies

- T002 → everything; T004 → T005/T006; US2 needs T006; US3 needs T003.
- T011-T013 last.

## Implementation Strategy

MVP = a correct writer proven by the round-trip harness (T004/T005),
then the UI surfaces. The compressor is the risk item; the harness
pins it against the existing decompressor immediately.
