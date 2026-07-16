# Tasks: Load Samples & Instruments from Other Modules

**Input**: plan.md + research.md (R-sections are the contract)

## Phase 1: Setup

- [X] T001 Baseline gates (determinism ×4 + roundtrip ×4) + point CLAUDE.md at this plan.

## Phase 2: Foundational

- [X] T002 `it_load.c/h`: export `Load_SampleData(filedata, size, sample_t*)` — full R3 converter (16-bit, unsigned, byte-swap, delta, byte-delta, IT214/215 compressed; Flg&=~0x0C, Cvt=1, interpolator pad, loop clamps).
- [X] T003 `src/it_ris.c/h` skeleton: record structs, readers, format sniffing (R1 codes), `RIS_FormatName`; build wiring (CMake + doc build lines).

## Phase 3: US1 — single sample from another module (P1) 🎯 MVP

- [X] T004 [US1] Scanners per R2: IT, S3M, XM, MOD(+15-instr), MTM, 669, FAR, PTM, KRZ, PAT → `RIS_ScanModule`.
- [X] T005 [US1] `RIS_LoadSample` per R3 (header copy semantics of `LoadSample`) + standalone `.ITS`.
- [X] T006 [US1] Editor: sample library requester (F9 chrome; files ↔ library levels, Directory exit row, format names), Enter on the F3 sample list opens it; load into current slot with occupied-slot confirm (FR-004); engine-lock/stop discipline.
- [X] T007 [US1] Selftest `LIB OK` part 1: rip-vs-full-load byte-compare on testdata .IT (incl. an IT215-compressed save→rip) + all five 007 test modules scan/load non-empty.

## Phase 4: US2 — full instrument import (P2)

- [X] T008 [US2] `RI_ScanModule` (IT/XM records per R5) + loaders `LoadITInstrument`(.ITI)/`LoadXIInstrument`(.XI)/in-module IT/XM.
- [X] T009 [US2] `RI_LoadInstrument` transfer per R5: UnusedSamples check, exclusive-sample release, free-slot allocation + note-table remap, data loads, enable-instrument-mode prompt; clean failure (FR-004).
- [X] T010 [US2] Editor: instrument library requester, Enter on the F4 list; selftest part 2 (rip instrument from testdata, verify remap + sample data equality; .XI from gen script).

## Phase 5: US3 — browse & preview (P3)

- [X] T011 [US3] Check-slot (99) preview: note keys audition the selected file/library entry (R4), note-off on release; lazy per-file info in the requester (format name, first sample/instrument name).

## Phase 6: Polish

- [X] T012 R6 disk saves: F3 Alt-O (.ITS) / Alt-T (ST) / Alt-W (WAV), F4 instrument save (.ITI); .ITI/.ITS round-trip in selftest.
- [X] T013 Gates ×8 + full selftest; HANDOFF §2/§4/§6 + README fidelity notes; commit.

## Dependencies

T002/T003 → all; T004→T005→T006→T007; T008→T009→T010; T012 after US1/US2.
