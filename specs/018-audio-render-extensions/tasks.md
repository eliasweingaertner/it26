---

description: "Task list for feature 018: Audio Rendering Extensions (after Esa Ruoho's IT 2.354)"
---

# Tasks: Audio Rendering Extensions (after Esa Ruoho's IT 2.354)

**Input**: Design documents from `specs/018-audio-render-extensions/`

**Prerequisites**: plan.md, spec.md, research.md (R1–R12), data-model.md, contracts/keys-and-screens.md, contracts/cli.md, quickstart.md

**Tests**: The plan requires automated checks: an **RND** block in the existing `ITED_SELFTEST` harness (`src/it_editor.c`, run by `build-compare/_work/selftest.ps1`), the `test_pattern` determinism gate (engine untouched), and port-only harness scripts for the gestures. No new test framework.

**Organization**: Tasks are grouped by user story (US1–US7 as in spec.md). Paths are relative to the repository root (`ittrack/`).

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies on incomplete tasks)
- **[Story]**: US1–US7

## Reference facts (it26, verified while planning)

- Live audio: `audio_cb` in `src/it_editor.c` (~14913) calls `WAVDriver_RenderAny`; `DoTick` in `src/it_driver.c` (~1068) takes `Engine_Lock` only around `Update` + `M32MixHandler`; `WAV_InitSound` (~997) resets FIFO/dither/filters and allocates nothing.
- Output formats: `WAVDriver_SetOutputFormat(0..4)` (0 = 16-bit original path, 1 = 24-bit, 2 = float clamped to ±1.0 at ~1135, 3/4 = 8-bit); `WAVDriver_SetOutputChannels`, `WAVDriver_SetForceMono`, `WAVDriver_SetSBFilter/Feedback`, `WAVDriver_SetStartRamp`, `WAVDriver_SetMixSpeed/GetMixSpeed`.
- Engine: `Music_PlayPattern(pat, rows, row)`, `Music_PlayPartSong(order, row)`, `Music_PlaySong(order)`, `Music_Stop`, `StopEndOfPlaySection` (it_music.c 2258 pattern end, 2340 song end), `PlayMode`, `CurrentOrder/Row/Pattern`, `NumberOfRows`, `StopSong`, `Music_InitStereo`, `Music_InitTempo`, `Music_ResetRNG`, `Music_AssignSampleToInstrument(smp0)`, mutes = `Song.Header.ChnlPan[c] & 0x80` (+ `MuteChannelTable`).
- Offline render reference: `render_to_wav` in `src/main.c` (4096-frame chunks, 30-minute cap).
- Editor: `smp_alloc` (+`SMP_PAD`), `RIS_SaveWAV(const sample_t*, path)` (8-bit unsigned, 16-bit signed, mono header), `stop_song`, `play_song`, `status()`, `CurKey.flags` (`ITKF_SHIFT`, `ITKF_CTRL`, `ITKF_ALT`), `ListSel`, `CurInstr`, `Screen`, `SCR_*`.
- F12: directory box `Screen_DrawBox(12, 41, 78, 45, 27)`, `wtext(13, 42..44, DirModule/DirSample/DirInstr, 64)`, `wbutton(27, 46, 52, 48, "  Save all Preferences", act_save_prefs)`, link table `links[18][6]` (issue #50), `act_save_prefs` / `load_prefs` (`ited.cfg`), `act_dialog_pick_folder` (feature 016).
- F10: format buttons drawn in `draw_file_requester` (`fmtlbl[4]`, rows 12..23), `req_link` objects 17..20, `ReqBtn`, `SaveFormat` 0..3, `save_module_dispatch`, `req_do_save`, `quick_save` (Ctrl-S).
- F11: order list key handler (~10000; `OrderCursor` 0..2, `ITK_RIGHT` wrap, `'g'/'G'` goto); F5: `handle_info_key` (~6294); F3: `sample_list_lkey` (~9828) and `handle_sample_altkey`; F2: `handle_pattern_key`; global keys: `handle_global`.
- Build: `CMakeLists.txt` `EDITOR_SOURCES`; the selftest script compiles an explicit source list in `build-compare/_work/selftest.ps1`.

---

## Phase 1: Setup

- [X] T001 Create `src/it_render.h` with the render API from plan.md (render job struct: kind pattern/song, pattern, rows, solo, destination import/file, channels; sink callbacks; `Render_Run`; naming functions; WAV writer open/write/close)
- [X] T002 Add `src/it_render.c` (empty skeleton including it_music.h / it_render.h) to `EDITOR_SOURCES` in `CMakeLists.txt` and to the cl source list in `build-compare/_work/selftest.ps1`; build `ited` and the selftest exe

---

## Phase 2: Foundational (blocks every story)

- [X] T003 Add output format 5 (float without clamp: mix value × 1/2^29, no ±1.0 clamp, no clip count) to the it26 extension block of `DoTick` and `WAVDriver_RenderAny` in `src/it_driver.c` (bytes per frame 4, accepted by `WAVDriver_SetOutputFormat` 0..5); leave format 0 untouched; not offered on Shift-F5
- [X] T004 Implement the render loop `Render_Run` in `src/it_render.c`: `Music_Stop`; `Driver->InitSound()`; mix speed (44100 for import, the given rate for file jobs); output format (0 = 16-bit stereo for import, 5 for file jobs) and channels 2; SB16 filter/feedback off, force-mono off, start ramp on; `Music_ResetRNG`; `Music_InitStereo`; `Music_InitTempo`; `StopEndOfPlaySection = 1`; `Music_PlayPattern(pat, rows (0→64, >200→200), 0)` or `Music_PlaySong(0)`; `WAVDriver_RenderAny` in 4096-frame chunks to the sink until `PlayMode == 0` (stop the last chunk at the frame where it ended), the abort callback returns nonzero, or the cap (30 min of frames) is hit; mono songs: left channel only; `StopEndOfPlaySection = 0` and `Music_Stop` at the end; returns frames written / aborted flag (R1, R2, R3)
- [X] T005 [P] Implement the WAV file sink in `src/it_render.c`: write a 44-byte placeholder header, append data, on close patch RIFF/data sizes; 32-bit float = fmt tag 3, block align 4×channels; 16-bit = fmt tag 1; `fopen` failure returns an error
- [X] T006 [P] Implement the memory sink in `src/it_render.c` (growing 16-bit buffer, interleaved, with a byte cap parameter and a "truncated" flag)
- [X] T007 [P] Implement Esa's names in `src/it_render.c`: `Render_NameTimestamp` ("LL"+HHMMSS+".WAV", localtime), `Render_NameCounter(songname)` (PFX rule R7 + per-session counter incremented before use, 4 digits, ".WAV"), `Render_NameSampleExport(hhmmss, slot)`, `Render_NameUser(typed)` (upper-case, ".WAV" if no extension, 8.3), `Render_NameChannel(typed, ch1)` (base ≤ 6 + 2 digits + extension)
- [X] T008 Live-audio gate in `src/it_editor.c`: a second `ma_mutex AudioMutex` (init next to `Mutex` in `main`) locked by `audio_cb` around its driver call; `volatile int RenderActive` → `audio_cb` writes silence (zero bytes for the device's frame size) and returns without touching the driver
- [X] T009 `render_begin()` / `render_end(int resume)` in `src/it_editor.c`: begin = `commit_current_pattern`, snapshot PlayMode/CurrentOrder/CurrentRow/CurrentPattern/NumberOfRows and the driver settings (mix speed, output format, channels, SB filter/feedback, start ramp, force mono), set `RenderActive` under `AudioMutex`; end = restore the settings, `Driver->InitSound()`, `Music_InitStereo`, `Music_InitTempo`, clear `RenderActive` under `AudioMutex`, then if `resume` and the snapshot PlayMode was 1 → `Music_PlayPattern(pattern, rows, row)`, 2 → `Music_PlayPartSong(order, row)` (all under `ed_lock`) (R4, R5)
- [X] T010 Render folder in `src/it_editor.c`: `char DirQuicksave[256]`; `quicksavedir=` read in `load_prefs` and written in `act_save_prefs`; `render_dir(char *out)` = Quicksave if set (must exist, else return error), else `DirSample` if set, else "."; `render_path(dir, name)` joins with the platform separator
- [X] T011 Render key poll in `src/it_editor.c`: an abort callback for `Render_Run` that, every chunk, pumps the backend's key queue (no screen redraw beyond the status line) and returns 1 on Esc or on any render gesture (R11), showing "Render halted -- finalizing to Quicksave ..." for file renders
- [X] T012 Selftest RND scaffolding in `src/it_editor.c` (`ITED_SELFTEST`): new block "RND" printing `[RND OK]` / `[RND FAIL]` like the other blocks, with a work folder `st_rnd_work` created and removed; first checks: names from T007 (empty song name → PTN0001, "a-b" → ABX…, timestamp 8.3 form, `Render_NameChannel("song.wav", 3)` = SONG03.WAV, `Render_NameUser("song.wav")` = SONG.WAV)

**Checkpoint**: render core, gate, folder and names exist; no gesture yet.

---

## Phase 3: User Story 1 — Turn the pattern into a sample (P1) 🎯 MVP

**Goal**: Ctrl-O (F2), F11 Ctrl-O / Shift-G / Right on digit 2 render a pattern and import it as a sample.

**Independent Test**: spec US1 — itdemo.it, F2 pattern 0, Ctrl-O → sample "Pattern 000" in slot max(samples, instruments)+1, looped, 44100 Hz, 16-bit, status "Pat 000 -> LL<time>.WAV -> sample NNN".

- [X] T013 [US1] `render_import(int pat, int rows, int resume)` in `src/it_editor.c`: slot = max(highest used sample, highest used instrument)+1 (R6), none → "Import failed: no free sample slot"; `render_begin`; `Render_Run` (import job, memory sink, cap 1 MB = 1048576 bytes); build the `sample_t` (flags 1|0x10|2, mono: a stereo mix is mixed down (L+R)/2, Vol 64, GvL 64, Cvt 1, DfP 32, Length frames, LoopBeg 0, LoopEnd Length, sustain 0, C5Speed 44100, DOSFileName = LL<time>.WAV (12 chars), SampleName "Pattern NNN", data via `smp_alloc`) and store it under `ed_lock`; `Song.Header.SmpNum` raised; instrument mode → `Music_AssignSampleToInstrument` and remember the instrument; `ListSel`/`CurInstr` set so F3 and F4 land on it; `render_end(resume)`; status "Pat NNN -> <file> -> sample NNN" (+ " in inst MMM"), or "Pattern truncated to 1MB; sample N"
- [X] T014 [US1] F2 Ctrl-O in `handle_pattern_key` (`src/it_editor.c`): 0x0F without `ITKF_SHIFT` → `render_import(CurPattern, CurRows, 1)`; show "Rendering pattern to WAV ..." before the render
- [X] T015 [US1] F11 resolve + import gestures in the order list key handler (`src/it_editor.c`): `order_render_pattern()` = playing pattern when `PlayMode != 0` else `Song.Orders[ListSel]` (≥ 200 → no action); Ctrl-O (no Shift) → import; `ITK_RIGHT` with `OrderCursor == 2` → import (else IT's wrap); `'G'` with `ITKF_SHIFT` → import (plain g/G unchanged)
- [X] T016 [US1] RND selftest (`src/it_editor.c`): load `testdata/itdemo.it`, `render_import` of pattern 0 without resume → slot rule, name "Pattern 000", flags (loop, 16-bit, stereo per song flag), C5 44100, Length = the frame count of a second identical render (determinism), LoopEnd = Length; instrument-mode song → host instrument exists; a full song (99 slots used) → "no free sample slot"
- [X] T017 [US1] Port-only harness script `build-compare/_work/rnd_us1.py`: F2 Ctrl-O while the song plays → new sample visible in F3, playback resumes (status line "Playing" at the same order); F11 Right on digit 2, Shift-G

**Checkpoint**: MVP — pattern → sample works and resumes playback.

---

## Phase 4: User Story 2 — Bounce a pattern to the Quicksave folder (P1)

**Goal**: Shift-Ctrl-O and the F11/F5 gestures write LL<HHMMSS>.WAV into the render folder; F12 Quicksave row.

**Independent Test**: spec US2 — Quicksave set, F2 Shift-Ctrl-O → LL<time>.WAV there, sample list unchanged.

- [X] T018 [US2] `render_to_folder(int pat, int rows, int resume)` in `src/it_editor.c`: `render_dir` (invalid → "Render aborted: Quicksave folder invalid (check F12)", nothing touched); name `Render_NameTimestamp`; `render_begin`; `Render_Run` (file job, Shift-F5 rate, format 5, file sink); `render_end(resume)`; status "Saved pat NNN as <file> in <folder>"
- [X] T019 [US2] Gestures in `src/it_editor.c`: F2 Shift-Ctrl-O (0x0F + `ITKF_SHIFT`); F11 Shift-Ctrl-O, `ITK_SHIFT_RIGHT` on `OrderCursor == 2`, Ctrl-G (0x07); F5 in `handle_info_key`: `ITK_SHIFT_RIGHT` and 0x0F → playing pattern, stopped → `CurPattern` (≥ 200 refused), channel selection untouched
- [X] T020 [US2] F12 Quicksave row in the F12 builder (`src/it_editor.c`): `DirLabels` gains " Quicksave" (Esa's `DirectoryText2`: `0FFh,1," Quicksave"`), box to `Screen_DrawBox(12, 41, 78, 46, 27)`, `wtext(13, 45, DirQuicksave, 64)`, "Save all Preferences" to `wbutton(27, 47, 52, 49, …)`, the F12 link table extended for the new input (Up = Instrument, Down/Tab = Save button, Shift-Tab = Instrument), Enter on the row → `act_dialog_pick_folder` targeting `DirQuicksave` where a dialog exists
- [X] T021 [US2] RND selftest (`src/it_editor.c`): Quicksave = `st_rnd_work` → `render_to_folder` writes `LL*.WAV` there; header = fmt tag 3, 32-bit, rate = current mix speed, channels per song flag; data length = import render frames × (rate ratio) when rates equal; a missing Quicksave folder → status contains "invalid", no file anywhere; empty Quicksave → file in `DirSample`
- [X] T022 [US2] RND selftest: unclamped float — render a pattern of testdata that clips (or a synthetic loud pattern built in the selftest) and check at least one sample value > 1.0 in the file (SC-007)
- [X] T023 [US2] Port-only harness script `build-compare/_work/rnd_us2.py`: F12 Quicksave row screenshot (box, label, button position), F5 Shift-Right while playing, F11 Ctrl-G

---

## Phase 5: User Story 3 — Export the samples as WAVs (P2)

**Independent Test**: spec US3 — F5 D → one `HHMMSSnn.WAV` per sample, "Dumped N samples as <HHMMSS>nn.WAV in <folder>", playback continues.

- [X] T024 [US3] Confirm samples in memory are always mono (IT converts stereo WAVs on load; the mixer has no stereo samples) so `RIS_SaveWAV`'s mono header is right; no change to `src/it_ris.c` if so
- [X] T025 [US3] `export_samples(int only_slot)` in `src/it_editor.c`: `render_dir` check; "Starting batch export of samples as .WAV..." painted at once; HHMMSS taken once; each non-empty slot (or `only_slot`) → `RIS_SaveWAV` with `Render_NameSampleExport`; for `only_slot` a header copy with `C5Speed = 44100` and the messages "Saving selected sample as C-4 44.1kHz WAV..." / "Saved C-4 44.1kHz WAV (sample N)" / "Selected sample is empty."; all: "Dumped N samples as <HHMMSS>nn.WAV in <folder>" (Esa's text said SMPnn.WAV, fixed 2026-10-18) or "No samples to dump"; playback not stopped (read-only)
- [X] T026 [US3] Gestures in `src/it_editor.c`: F5 'd'/'D' and Ctrl-Shift-Right (`ITK_SHIFT_RIGHT` with `ITKF_CTRL`, or `ITK_CTRL_RIGHT` with `ITKF_SHIFT`, whichever the backends deliver); F11 Ctrl-Shift-Right; F3 `sample_list_lkey`: `ITK_CTRL_RIGHT` without Shift → selected, with Shift → all (check first that IT 2.14 binds nothing to Ctrl-Right on F3)
- [X] T027 [US3] RND selftest: export itdemo.it's samples → file count = non-empty slots; one 8-bit sample's bytes = signed data + 128; a 16-bit one byte-identical to its data; selected export header rate 44100; the song's sample memory unchanged (checksum before/after)

---

## Phase 6: User Story 4 — Stems: per channel, or the whole song (P2)

**Independent Test**: spec US4 — channels 1, 2, 5 used, 2 muted → Shift-Alt-M writes 2 files; mutes as before.

- [X] T028 [US4] `render_sweep(int song, const char *typed)` in `src/it_editor.c`: channel list = (song: every unmuted channel 0..63 that the song uses (`Song.Header.ChnlPan` not muted); pattern: channels with a note 0..119 in `Grid` and not muted); save `ChnlPan`/`MuteChannelTable`; per channel: mute all others (bit 7), progress "Rendering channel NN ...", `Render_Run` (file job; pattern or song), name = typed ? `Render_NameChannel` : `Render_NameCounter`; Esc → stop after the current channel; restore mutes; no import, no resume; final status with the file count
- [X] T029 [US4] F2 Shift-Alt-M in `handle_pattern_key` (`src/it_editor.c`): Alt-M with `ITKF_SHIFT` → `render_sweep(0, NULL)`; Alt-M without Shift stays block mix
- [X] T030 [US4] F10 buttons in `src/it_editor.c`: `fmtlbl` gains "  WAV" (69,24–77,26) and " MWAV" (69,27–77,29), `ReqBtn` 0..5, `req_link` objects 21/22 (IT215 Down → 21; 21 Up 20 Down 22; 22 Up 21 Down 15; Left/Shift-Tab → 14, Right/Tab → 15), mouse hit rows 12..29; `SaveFormat` 4/5
- [X] T031 [US4] Save path in `src/it_editor.c`: `req_do_save` and `quick_save` with `SaveFormat` 4 → whole song to `Render_NameUser(typed)` in the current folder (overwrite prompt as for modules), 5 → `render_sweep(1, typed)`; no `.IT` extension appended for 4/5; "Rendering song ..." progress, Esc aborts and finishes the file
- [X] T032 [US4] Second-gesture stop: any render key pressed during `Render_Run` (T011) stops it; verify the sweep stops after the current channel and the whole-song file is finished with valid sizes
- [X] T033 [US4] RND selftest: Shift-Alt-M sweep on a pattern built in the selftest (notes in channels 1, 2, 5; channel 2 muted) → 2 files; `ChnlPan` restored; each file's peak shows only its channel (other channels silent → non-zero only where that channel plays); MWAV naming SONG01…; F10 `req_link` table for 20..22

---

## Phase 7: User Story 5 — Quicksave the module (P3)

**Independent Test**: spec US5 — Alt-W writes MYSONG.IT into the Quicksave folder.

- [X] T034 [US5] Audit Alt-W per screen against IT's key tables (pattern editor Alt-W = "wipe" in IT 2.14, F3 Alt-W save sample, F4?) and record which screens get the global Alt-W in `specs/018-audio-render-extensions/research.md` R12
- [X] T035 [US5] Global Alt-W / Shift-Alt-W in `handle_global` (`src/it_editor.c`) after the screen handlers: Alt-W → `DirQuicksave` empty → "Quick-save folder not configured." + `save_requester()`; else save under `FileNameDisp` in `DirQuicksave` via `save_module_dispatch` (SaveFormat 4/5 → render as T031); Shift-Alt-W → `DirQuicksave` = `DirModule` (or the current folder), `act_save_prefs`, "Quick-save folder memorized."
- [X] T036 [US5] RND selftest: Alt-W with an empty folder → status text; with `st_rnd_work` → file exists and loads back; Shift-Alt-W sets the folder and writes `quicksavedir=` to `ited.cfg` (restore the user's `ited.cfg` afterwards — selftest runs in the repo folder)

---

## Phase 8: User Story 6 — Render from the command line (P3)

**Independent Test**: spec US6 — `ited --render-patterns song.it` writes one WAV per pattern with data and exits 0 without a window.

- [X] T037 [US6] Parse `--render-patterns <module>`, `--pattern N`, `--dump-samples <module>` in `main` of `src/it_editor.c`; after `load_prefs`, the module load and engine init (before `Screen_Init` / the audio device) run the batch: patterns 0..199 with data (or N) → `Render_Run` file jobs, counter names, "wrote <path>" per file; dump → `export_samples(0)`; exit codes per contracts/cli.md
- [X] T038 [US6] Port-only check script `build-compare/_work/rnd_cli.ps1`: run both options on `testdata/itdemo.it` with a temp `ited.cfg` Quicksave folder, count files, check exit codes (0; 1 for a missing module and for `--pattern 300`)

---

## Phase 9: User Story 7 — Discoverable (P3)

- [X] T039 [US7] F1 help lines in `help_lines` (`src/it_editor.c`): pattern editor (ctx 1): Ctrl-O, Shift-Ctrl-O, Shift-Alt-M; order list, info page and sample list contexts their gestures; global Alt-W / Shift-Alt-W; update the selftest DLG help expectations
- [X] T040 [P] [US7] README section "Rendering (after Esa Ruoho's IT 2.354)" in `README.md`: gestures table, names, formats (imports 44.1 kHz/16-bit, WAV files 32-bit float unclipped at the Shift-F5 rate), the F5 Ctrl-O difference, the batch options; fidelity notes list the F10/F12 layout from Esa's fork

---

## Phase 10: Polish & Cross-Cutting

- [X] T041 Run `test_pattern` (all four modules `IDENTICAL`) to prove `it_driver.c`'s original path is unchanged
- [X] T042 Run the full selftest (all blocks OK incl. RND) and the existing IT 2.14 comparison scripts that touch F2/F3/F5/F10/F11/F12 (`f9_arrows`, `f9_tab_esc`, `mouse_f3`, `mouse_f12`, `mouse_f11`, `it_screens`) — differences only where the new F10 buttons / F12 row are visible, documented in the README
- [X] T043 [P] Update `docs/HANDOFF.md` (feature 018 status, render module, gate) and `CHANGELOG.md` (unreleased section)
- [X] T044 Mark tasks done here and run `quickstart.md` manual checks 1–10 on Windows; note any not run

---

## Implementation notes (2026-10-18)

- T024: confirmed — IT samples are mono in memory (IT's loader converts stereo WAVs; the mixer has no stereo samples), so no `it_ris.c` change; imports are mixed to mono for the same reason (spec Clarifications).
- T026: F3 single-sample export is **Ctrl-Alt-Right** (user decision; IT's Ctrl-Right = next order kept).
- T017/T023: implemented as one port-only script `build-compare/_work/rnd_gestures.py` (19 gestures, 19 OK); T031/T032 checked with `rnd_f10.py` (WAV 283 s song in 7 s; MWAV Esc finishes the channel in progress).
- T038: run by hand from a scratch folder (53 pattern files from itdemo.it in 3.9 s, exit 0; `--pattern 300` and a missing module exit 1; `--dump-samples` 15 files).
- T041: `test_pattern` IDENTICAL for all four modules with the known hashes.
- T042: F10 captures differ only by the two new buttons, F12 only in rows 45–49 (Quicksave row, box, moved button).
- T044: not run — listening on a real device (only render files and status lines were checked), the F12 Enter / Ctrl-O folder dialog for the Quicksave row (the remote backend has no dialogs), Linux/macOS builds (CI on push).

## Phase 11: Follow-up — descriptive file names (2026-10-18)

- [X] T045 Path layer in `src/it_render.c` / `src/it_render.h`: UTF-8 paths internally; `Render_Fopen`, `Render_Exists`, `Render_MkDir` (Windows: UTF-8 → UTF-16, `_wfopen` / `_wstat` / `CreateDirectoryW`; POSIX: as is), `Render_DirFromNative` (Windows ANSI code page → UTF-8), `Render_ToDisplay` (UTF-8 → CP437 for the status line, `?` for the rest); the WAV writer opens through `Render_Fopen`
- [X] T046 Name building in `src/it_render.c`: `Render_CleanName` (CP437 → UTF-8, forbidden characters → `_`, trim, ≤ 80 bytes on a character boundary), `Render_Stamp` ("YYYY-MM-DD HH.MM.SS"), `Render_Unique` (` (2)` …), `Render_SongTitle` input from the editor
- [X] T047 `Render_SaveSampleWav(const sample_t *, path, rate)` in `src/it_render.c` (mono, 8-bit unsigned / 16-bit signed, as `RIS_SaveWAV`, through `Render_Fopen`)
- [X] T048 Switch every file-writing path in `src/it_editor.c` to the new names: `render_to_folder`, `export_samples` (folder per export; single sample), `render_sweep` (stems folder; F10 MWAV `<typed base> - chNN.wav`), F10 WAV (typed name as typed), `batch_run` (patterns folder; dump as export); status texts show the display form; the import keeps `LL<HHMMSS>.WAV` in the sample field
- [X] T049 RND selftest and port-only scripts updated to the new names (incl. a song name with `?`, `:` and a CP437 umlaut, a clash → ` (2)`); README / CHANGELOG / HANDOFF names updated

## Dependencies & Execution Order

- Setup (T001–T002) → Foundational (T003–T012) → user stories.
- US1 (T013–T017) needs T004, T006, T007, T008, T009. **MVP**.
- US2 (T018–T023) needs T004, T005, T007–T011; independent of US1.
- US3 (T024–T027) needs T007, T010; independent of the render loop.
- US4 (T028–T033) needs US2's file render (T018) and T011.
- US5 (T034–T036) needs T010; T035's WAV/MWAV branch needs T031.
- US6 (T037–T038) needs T004/T005/T007 and T025.
- US7 and Polish last.

## Parallel Opportunities

- T005, T006, T007 (different functions in the new file, no shared state) after T004's signature is fixed.
- US3 (export) can run alongside US1/US2 after Phase 2.
- T040 (README) and T043 (docs) alongside the implementation.

## Implementation Strategy

1. MVP: Phases 1–3 (pattern → sample, with resume), validate with T016/T017.
2. Add US2 (Quicksave files + F12 row) → the DAW workflow.
3. Add US3 and US4 (export, stems, F10) → everything Esa renders.
4. US5–US7 and polish; commit per phase with the user's go-ahead.
