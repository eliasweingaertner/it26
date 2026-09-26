---

description: "Task list for 015-load-sample-screen"
---

# Tasks: Authentic Load Sample Screen

**Input**: Design documents from `/specs/015-load-sample-screen/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/screen-contract.md](contracts/screen-contract.md)

**Tests**: included — FR-014 requires a selftest block (`LSS`) and FR-015 the
standing gates.

## Format: `[ID] [P?] [Story] Description`

Repository root for paths: `C:\Users\elias\fable5\ittrack`. Almost all work is
in `src/it_editor.c` (one 11k-line file), so `[P]` is used only where a task
touches a different file.

---

## Phase 1: Setup

- [X] T001 Record the gate baseline (determinism x4 hashes, roundtrip 12/12, selftest block list incl. INS) at ittrack HEAD `278cb25` into `specs/015-load-sample-screen/baseline.md`
- [X] T002 Create the selftest fixture generator: extend `tools/gen_import_tests.py` to emit `testdata/ls_fixture/` with two subdirectories (`ACOUSTIC/`, `BASS/`), one 8-bit WAV, one ITS, one junk file `README.TXT`, and one small module

---

## Phase 2: Foundational (list records + listing builder)

**Purpose**: the data every story draws from (research R2/R3). Blocks all stories.

- [X] T003 [P] Extend `slibent_t` in `src/it_ris.h` with `Date`, `Time` (DOS-packed, record +54h/+56h) and `SortPri` (+5Ah), keeping existing fields and layout comments
- [X] T004 Add `int RIS_ListDirectory(const char *dir, slibent_t *ents, int max)` to `src/it_ris.c`/`src/it_ris.h`: directories first (the lone `.` entry → `\`, `..` kept) with `hdr.SampleName` = `DirectoryMsg` (8×char 154 + "Directory" + 8×154), `Format` 1, `SortPri` 0; then every file (no extension filter), cap 620; size and host mtime → DOS date/time
- [X] T005 In `RIS_ListDirectory` identify each file through the existing `RIS_ScanModule` path: single-sample sources → their header with `Format` = sample type and `SortPri` 2; modules → `Format` 20h+module type, `SortPri` 1 and module name in `hdr.SampleName`; unreadable → `Format` 4, `SortPri` 3 (in `src/it_ris.c`)
- [X] T006 Sort per `D_SlowSampleSort` (research R3): `\` then `..` pinned, the rest by `SortPri` then filename bytes (13) in `src/it_ris.c`
- [X] T007 Add record-level assertions to a new `LSS` selftest block in `src/it_editor.c` over `testdata/ls_fixture/`: order (`\`, `..`, ACOUSTIC, BASS, module, samples, README.TXT), dotted directory names, junk = unknown

**Checkpoint**: listing reproduces the original's records and order.

---

## Phase 3: User Story 1 — Browse on the original's screen (P1) 🎯 MVP

**Goal**: Load Sample shows the `O1_LoadSampleList` screen with single-list navigation and drives.

**Independent test**: SC-001 — the fixture folder and a folder with ≥5 subdirectories list row-for-row as in IT 2.14 (reference `load-sample.png`).

- [X] T008 [US1] Add the Load Sample screen state (entries, `cur`, `top`, `dir`, `in_module`, drive list, focus) and a `load_sample_screen_run()` loop in `src/it_editor.c`, replacing `file_requester_run(0)` inside `sample_library_requester()`; F9 path untouched (FR-016)
- [X] T009 [US1] Draw the static objects from research R1 in `src/it_editor.c`: full-screen chrome with header "Load Sample", LoadSampleBox (5,12)-(44,48) st.27, DriveSampleBox (45,12)-(54,23), LSInfoBox (63,12)-(77,23), LSInfoText at (55,13), LSWaveFormBox (45,24)-(77,29), LSParametersBox (45,30)-(77,42) st.9 + text at (48,33), vol/vib boxes st.25, LSFileInfoBox (52,43)-(77,48) + text at (46,44)
- [X] T010 [US1] Port `D_DrawLoadSampleWindow`'s list part in `src/it_editor.c`: "No files." at (6,13) attr 5 when empty; 35-row window with scroll clamp; 3-digit row numbers at col 2 attr 20h; divider char A8h attr 2 at col 31; name col 6 (25 chars, bytes ≥226 → space); filename col 32 (12); row attr by `Format` (0→6, 1→5, 4→2, else 3)
- [X] T011 [US1] Port `D_PreLoadSampleWindow` cursor: 38 cells from col 6 attr 30h, divider cell 32h (`src/it_editor.c`)
- [X] T012 [US1] Port `D_LSDrawDriveWindow`/`D_LSPreDriveWindow`: 10 rows "Drive X:" from (46,13) attr 5, cursor 30h over 8 cells; drive list from `GetLogicalDrives` on Windows, a single `/` on POSIX (research R10), reusing `req_scan`'s drive code in `src/it_editor.c`
- [X] T013 [US1] Keys in `src/it_editor.c`: Up/Down/PgUp/PgDn/Home/End on the list; Right/Tab → drive box and back; Enter on dir → re-list, cursor 0; Enter on `\` → root; Enter on a drive → that drive's cwd; Esc leaves
- [X] T014 [US1] Enter on a sample loads through the existing `lib_load_sample_entry()` (no overwrite prompt, host-instrument prompt unchanged, stereo choice unchanged) in `src/it_editor.c`
- [X] T015 [US1] Delete key: `O1_ConfirmDelete2` confirmation "Delete file?" default Cancel, remove the file, re-list (FR-018) in `src/it_editor.c`
- [X] T016 [US1] Capture aid: `ITED_DUMP=13` / `ITED_SHOT_SCREEN=13` render the screen for `ITED_SHOT_DIR` (default `testdata`) in `src/it_editor.c`
- [X] T017 [US1] Extend `LSS` with screen assertions via `Screen_GetCell`: dotted "Directory" row text/attr 5 at row 13, divider glyph at (31,13), row number "001" at (2,13), drive box text at (46,13)
- [X] T018 [US1] Gate: determinism x4, roundtrip 12/12, full selftest on Windows and WSL; visual compare of `ITED_SHOT` vs `load-sample.png`

**Checkpoint**: the reported screen disparity is gone.

---

## Phase 4: User Story 2 — See the sample before loading (P2)

**Goal**: info box, volumes, vibrato, file info, waveform, module browsing, Ctrl-F3.

**Independent test**: SC-002 — preview values for the fixture WAV/ITS equal F3's values after loading them.

- [X] T019 [US2] Info box values in `src/it_editor.c`: Filename (64,13), Speed/loop fields rows 14..20 via the F3 7-digit colour-2 renderer and On/Off + Forwards/Ping Pong toggles; Quality at (64,21) with the `IT_DISK.ASM` strings "8 Bit"/"16 Bit"/"8 Bit Stereo"/"16 Bit Stereo" (stereo = Cvt bit 32); Length (64,22)
- [X] T020 [US2] Thumbbars (display): Default Volume (63,33) 0..64 ← +13h, Global Volume (63,34) ← +11h, Vibrato Speed (63,37) 0..64, Depth (63,38) 0..32, Rate (63,39) 0..255 from the highlighted entry (`src/it_editor.c`)
- [X] T021 [US2] File info in `src/it_editor.c`: Format name at (53,44) attr 5 from `RIS_FormatName`/module names ("Directory" for dirs); Size (53,45) 9-digit zero-padded when < 655360000; Date (53,46) `MonthName D, YYYY`; Time (53,47) 12-hour `h:mmam`/`pm`, attr 5
- [X] T022 [US2] Audition: note keys load the highlighted entry into the check slot with the stereo prompt suppressed and play it (port of `D_PostLoadSampleWindow`, reusing `lib_preview_key`); remember the auditioned index (`SampleInMemory`) in `src/it_editor.c`
- [X] T023 [US2] Waveform: only when the highlighted index is the auditioned one, generate font-B glyphs 0..123 for a 31×4 cell block at (46,25) attr 0Dh from the check-slot data, reusing the F3 waveform rasteriser (`src/it_editor.c`)
- [X] T024 [US2] Enter on a module lists its samples in the same screen (first row = library exit, `LibraryMsg` dotted), Enter on the exit row returns to the directory listing (re-read) in `src/it_editor.c`
- [X] T025 [US2] Ctrl-F3 Sample Library: add `ITK_CTRL_F3` (explicit value in a free gap, collision-checked) to `src/it_screen.h` and both pixel backends; open the same screen with header "Sample Library (Ctrl-F3)" and `LSViewWindowKeys` (Enter navigates only); capture aid 14; retire `draw_lib_browser` for samples in `src/it_editor.c`
- [X] T026 [US2] Extend `LSS`: fixture WAV/ITS preview fields equal the F3 values after `lib_load_sample_entry`; file-info Size/Date formats; module entry lists its samples
- [X] T027 [US2] Gate: determinism x4, roundtrip 12/12, full selftest on Windows and WSL

---

## Phase 5: User Story 3 — Adjust before loading (P3)

**Goal**: editable preview fields, written into the entry (research R8).

**Independent test**: edit speed and loop begin, move away and back (still edited), load, F3 shows the edits.

- [X] T028 [US3] Make the info-box fields focusable/editable (digit entry as F3's 5num fields, toggles cycling as F3) writing into `ents[cur].hdr`, with focus navigation matching the objects' up/down links (research R1) in `src/it_editor.c`
- [X] T029 [US3] Loop correction on edit (`D_LSCheckLoopValues` / `D_LSCheckSusLoopValues`) using the same clamp as F3's `smp_check_both` logic in `src/it_editor.c`
- [X] T030 [US3] Thumbbars editable (left/right, as other thumbbars), writing +13h/+11h/+4Ch/+4Dh/+4Eh of the entry in `src/it_editor.c`
- [X] T031 [US3] Space renames the entry's sample name (`LSWindow_Space`) in `src/it_editor.c`
- [X] T032 [US3] Ensure load applies the edited header: `RIS_LoadSample` keeps the entry's header fields over the file's; restore Cvt/DfP from the pre-audition snapshot (`CheckDataArea`) in `src/it_ris.c` / `src/it_editor.c`
- [X] T033 [US3] Extend `LSS`: edit loop begin + speed, move highlight away and back (edits kept), load, assert F3 values; re-list drops edits
- [X] T034 [US3] Gate: determinism x4, roundtrip 12/12, full selftest on Windows and WSL

---

## Phase 6: Polish

- [X] T035 README fidelity notes: the three deviations (always sorted, no CACHE.ITS, POSIX drive box) in `README.md`
- [X] T036 HANDOFF status paragraph, `LSS` in the gate list, capture aids 13/14 in `docs/HANDOFF.md`
- [X] T037 Final gates on Windows, WSL and CI; compare with `baseline.md` -- Windows + WSL identical to baseline; CI runs on the next push
- [X] T038 Copy `specs/015-load-sample-screen` into the ittrack `specs/` snapshot
- [X] T039 Committed as one commit (phases interleave in it_editor.c) with `git commit -F`

---

## Dependencies

```text
Setup → Foundational (T003-T007) → US1 (T008-T018) → US2 (T019-T027) → US3 (T028-T034) → Polish
```

US2 draws into the screen US1 builds; US3 edits the fields US2 displays, so the
stories are sequential. Each still ends at a shippable, gate-green checkpoint.

## Parallel opportunities

- T003 (`it_ris.h`) alongside T002 (`tools/`).
- T004–T006 (`it_ris.c`) can proceed while T008–T009 scaffold the screen in `it_editor.c`, once T003 lands.
- Everything else is serial in `it_editor.c`.

## Implementation strategy

**MVP = Setup + Foundational + US1 (T001–T018)**: the original's screen and
navigation, loading exactly as today. That alone closes the visible gap in
issue #2's screenshots. US2 adds the preview information, US3 the editing.
