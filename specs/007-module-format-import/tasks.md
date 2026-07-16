# Tasks: Module Format Import

**Input**: Design documents from `/specs/007-module-format-import/`

## Phase 1: Setup

- [X] T001 Baseline gates (determinism ×4 + roundtrip ×4).

## Phase 2: Foundational

- [X] T002 `ittrack/src/it_import.c/h`: reader, format sniffing/dispatch (research R1), shared sample-data reader (unsigned/delta/16-bit conversion per Cvt), tables (FineTuneTable, MODPeriodTable, PanningPositions, XMEffectG), pattern-grid → `Pattern_Pack` plumbing; export the loader's default-MIDI-macros init from `it_load.c`.
- [X] T003 Requester + `do_load_named` accept the five formats (extension filter + Import_LoadModule).

## Phase 3: US1 — S3M (P1) 🎯 MVP

- [X] T004 [US1] D_LoadS3M port per research R2 (+ PE_TranslateS3MPattern incl. the authentic Dxy dead code).
- [X] T005 [US1] `tools/gen_import_tests.py` S3M output + selftest render smoke.

## Phase 4: US2 — MOD, MTM, 669, XM (P2)

- [X] T006 [US2] D_LoadMOD + TranslateMODCommand + PE_TranslateMODPattern per R3 (variants, 127-order quirk).
- [X] T007 [US2] D_LoadMTM + track assembly + comment→message per R4.
- [X] T008 [US2] D_Load669 + repeat-effect memory per R5.
- [X] T009 [US2] D_LoadXM per R6: header/orders, pattern packing + >200-row split + D_InsertOrder, note/ins/vol/effect translators incl. the column swap, instrument conversion (fadeout, note table with sample base, vol/pan envelope emulation rules), sample headers (C5 from PitchTable×FineTune, Cvt 5 delta) + data.
- [X] T010 [US2] gen_import_tests.py MOD/MTM/669/XM outputs; selftest loads each and requires non-silent renders (`IMPORT OK`).

## Phase 5: US3 — Convert-and-save (P3)

- [X] T011 [US3] Verify import → F10 save → reload (via the 004 saver) in the selftest (hash of the re-loaded IT render is stable).

## Phase 6: Polish

- [X] T012 Gates ×8 + selftest; HANDOFF §2/§6 + README notes (quirks kept; import list).

## Dependencies

T002/T003 → all; T004 → T005; T009 largest.
