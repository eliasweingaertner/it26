---

description: "Task list for feature 016: Native File Dialogs and Platform-Aware Help"
---

# Tasks: Native File Dialogs and Platform-Aware Help

**Input**: Design documents from `specs/016-native-file-dialogs/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/keys-and-help.md, contracts/backend-dialog.md, quickstart.md

**Tests**: The spec's success criteria (SC-002 byte-identical saves, SC-003 identical slot contents, SC-004 unchanged keys) require automated checks, so each story adds checks to the existing `ITED_SELFTEST` harness as a **DLG** block (via the `ITED_DIALOG_FAKE` hook). No separate test framework.

**Organization**: Tasks are grouped by user story. All source paths are relative to `it26/`; spec paths relative to the workspace root.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1–US5 as in spec.md

## Reference facts (verified while planning)

- Module directory = process working directory; the F9/F10 requester changes it with `chdir()` (`req_enter_dir`, `src/it_editor.c` ~10295). Loading: `do_load_named(path)` (~11436). Saving: `save_module_dispatch(name)` (~9873) with `SaveFormat` 0 = IT, 1 = S3M; overwrite prompt `confirm_overwrite(draw_fn)`; `req_do_save` (~9893) is the F10 reference flow.
- Sample loading: `lib_load_sample_entry(const slibent_t *)` (~9983) into `Song.Smp[ListSel]`; instrument loading: `lib_load_instrument_entry(const ilibent_t *)` (~10023). Library browsing inside a module: `lib_browse_run(int inslib, const char *srcname)` (~10167); Load Sample screen: `load_sample_screen_run(view)` (~11216), `ls_set_dir()` (~10597) updates `LsDir` and `DirSample`, `ls_enter_module()` (~10564).
- F12 directory fields: `wtext(13, 42|43|44, DirModule|DirSample|DirInstr, 64)` (~4333); widgets in `W[]`, focus `FocusIdx[Screen]`, type `WT_TEXT`.
- Help: `src/it_help.inc` (generated, do not edit); `HelpContextPtrs[15]`; every context list ends with the global key block; contexts 3,5,6,8,10,11,13,14 share `HLPL_nohelpcontext`; `help_context_of()` maps SCR_VARS (F12) and SCR_DRIVER to 5. Preview line `HLP_helpcontext1_181` = col 5, "Caps Lock+Key", `FF 04 20`, " Preview ", word `A5`. `draw_help()` ~4441, `help_key()` ~4489, `help_open()` ~4461, `HelpReturnScreen`.
- Keys: last ITK code `ITK_SHIFT_F5` = 0x297 (`src/it_screen.h`); Shift-F9 tests: `src/it_screen_sdl.c` (`kc == SDLK_F9 && (mod & KMOD_SHIFT)`), `src/it_screen_win32.c` (`wp == VK_F9 && GetKeyState(VK_SHIFT)`). Ctrl-O arrives as code 0x0F on every backend.
- `status(fmt, ...)` prints to the status line; `ed_lock()`/`ed_unlock()` wrap engine access; `stop_song()`.

---

## Phase 1: Setup

**Purpose**: Build wiring for the new platform files.

- [X] T001 Add `src/it_dialog_win32.c` to the Win32 `ited` sources and link `ole32 shell32 uuid` in `CMakeLists.txt`; add `src/it_dialog_mac.c` (APPLE) or `src/it_dialog_posix.c` (non-Apple) to the SDL `ited` sources in `CMakeLists.txt`
- [X] T002 [P] Create stub files `src/it_dialog_win32.c`, `src/it_dialog_mac.c`, `src/it_dialog_posix.c`, each with a header comment and one entry function returning "unavailable", guarded so each compiles only on its platform (`_WIN32` / `__APPLE__ && HAVE_SDL` / POSIX non-Apple `HAVE_SDL`) *(done: full implementations written directly instead of stubs)*
- [X] T003 [P] Update the direct-build command lines (MSVC `cl` list, gcc list) in `docs/HANDOFF.md` §3 to include the new dialog file for each platform and the extra Windows libraries

---

## Phase 2: Foundational (blocks all stories)

**Purpose**: Dialog types, the backend member, the screen-layer wrapper with test hook, new key codes, input flush, and the editor's dialog helper.

- [X] T004 Define `it_dialog_req_t`, `it_dialog_res_t`, the kind enum (`IT_DLG_OPEN_MODULE`, `IT_DLG_SAVE_MODULE`, `IT_DLG_OPEN_SAMPLE`, `IT_DLG_OPEN_INSTRUMENT`, `IT_DLG_PICK_FOLDER`), the status enum (`IT_DLG_CHOSEN`, `IT_DLG_CANCELLED`, `IT_DLG_UNAVAILABLE`, `IT_DLG_REJECTED`), `IT_DLG_PATH_MAX` 1024, and the prototype `int Screen_FileDialog(const it_dialog_req_t *, it_dialog_res_t *)` in `src/it_screen.h` per `specs/016-native-file-dialogs/data-model.md`
- [X] T005 Add members `int (*file_dialog)(const it_dialog_req_t *, it_dialog_res_t *)` and `const char *preview_key` to `screen_backend_t` in `src/it_screen.h`; initialise both to `NULL` in the terminal backend in `src/it_screen.c` and in `Screen_BackendWin32` (`src/it_screen_win32.c`) and `Screen_BackendSDL` (`src/it_screen_sdl.c`) for now
- [X] T006 Add `ITK_CTRL_SHIFT_F9 = 0x298` and `ITK_CTRL_SHIFT_F10` (0x299) after `ITK_SHIFT_F5` in the key enum in `src/it_screen.h`, with a comment naming feature 016 / issue #26
- [X] T007 Implement in `src/it_screen.c`: a shared helper `Screen_Utf8ToCP437Display(const char *utf8, char *out, size_t cap)` (decode UTF-8, map each code point with `Screen_UnicodeToCP437`, 0 → `?`, returns 1 if any substitution) and a helper deriving `save_format` from an extension (`.it`/`.s3m`, case-insensitive; else IT)
- [X] T008 Implement `Screen_FileDialog()` in `src/it_screen.c` per `contracts/backend-dialog.md`: `ITED_DIALOG_FAKE` hook (`!cancel`, `!unavailable`, else path → CHOSEN with display/lossy/save_format filled via T007 helpers), `NULL` backend member → UNAVAILABLE with reason "No file dialog available", else call the backend; then shared validation (length vs `IT_DLG_PATH_MAX` → REJECTED "Path too long"; `IT_DLG_PICK_FOLDER` with `lossy` → REJECTED "Folder name has characters it26 cannot show"; folder longer than 64 → REJECTED "Path too long for this field")
- [X] T009 [P] In `src/it_screen_win32.c` `WndProc` `WM_KEYDOWN`/`WM_SYSKEYDOWN`: before the `VK_F9 && Shift` test, push `ITK_CTRL_SHIFT_F9` / `ITK_CTRL_SHIFT_F10` when `VK_F9`/`VK_F10` arrive with both Ctrl and Shift held (and not Alt)
- [X] T010 [P] In `src/it_screen_sdl.c` `PumpEvents` `SDL_KEYDOWN`: before the `SDLK_F9 && KMOD_SHIFT` test, push `ITK_CTRL_SHIFT_F9` / `ITK_CTRL_SHIFT_F10` for F9/F10 with `KMOD_CTRL` and `KMOD_SHIFT` held (not `KMOD_ALT`)
- [X] T011 [P] Add a static `InputFlushAfterDialog()` to `src/it_screen_win32.c` (drop `KeyQueue`, `PeekMessage(WM_KEYFIRST..WM_KEYLAST, PM_REMOVE)` loop, `PushKey(ITK_SHIFT_RELEASE)`) per research R10
- [X] T012 [P] Add a static `InputFlushAfterDialog()` to `src/it_screen_sdl.c` (drop `KeyQueue`, `SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT)`, `SDL_ResetKeyboard()` under `SDL_VERSION_ATLEAST(2,24,0)` else `SDL_SetModState(KMOD_NONE)`, clear `SkipText`, `PushKey(ITK_SHIFT_RELEASE)`) per research R10
- [X] T013 Add editor helper `static int ed_dialog(int kind, const char *start_dir, const char *suggest, it_dialog_res_t *res)` in `src/it_editor.c`: fills the request, calls `Screen_FileDialog`, and on UNAVAILABLE/REJECTED shows `res->reason` via `status()`; returns 1 only for CHOSEN; plus `static void split_path(const char *path, char *dir, size_t dcap, const char **base)` handling both `/` and `\`
- [X] T014 Add the **DLG** selftest block skeleton in `src/it_editor.c` next to the existing selftest blocks (search for `[LSS OK]`): sets/clears `ITED_DIALOG_FAKE` per step (`_putenv_s` on Windows, `setenv`/`unsetenv` elsewhere), prints `[DLG OK]` only if all its checks pass; first check: with `!cancel`, a dialog request changes nothing

**Checkpoint**: builds on Windows, Linux, macOS; selftest still passes with the empty DLG block.

---

## Phase 3: User Story 1 - Open a module with the OS dialog (Priority: P1) 🎯 MVP

**Goal**: Ctrl-Shift-F9 from any screen opens the native open dialog; the module loads exactly as from F9; the folder becomes the working directory.

**Independent Test**: Ctrl-Shift-F9 from the pattern editor, pick a module in another folder; it loads and plays as via F9, and F9 then lists that folder (quickstart rows 1–3, 12–14, 21).

- [X] T015 [US1] Implement `act_dialog_open_module()` in `src/it_editor.c`: `ed_dialog(IT_DLG_OPEN_MODULE, cwd, NULL, &r)`; on CHOSEN split the path, `chdir(dir)` (status "Can't change to …" and abort on failure), then run the same steps the F9 requester uses on a chosen file (`do_load_named(base)`, its success/failure status and screen handling); keep the original working directory if the load fails
- [X] T016 [US1] Bind `ITK_CTRL_SHIFT_F9` in the global key handler `handle_global()` in `src/it_editor.c` to `act_dialog_open_module()`, ensuring it is not reached from the F1 help screen's own handling and that Shift-F9 (`ITK_SHIFT_F9`) is unchanged
- [X] T017 [US1] Show the loaded name in the header from `r.display` (base name, upper-cased like `do_load_named` does) instead of the raw path in `src/it_editor.c`, so non-CP437 names show `?`
- [X] T018 [P] [US1] Implement the open-file and pick-folder parts of `src/it_dialog_win32.c`: `CoInitializeEx` (tolerate `S_FALSE`/`RPC_E_CHANGED_MODE`), `CoCreateInstance(CLSID_FileOpenDialog)`, owner = tracker `HWND` (pass in via a setter from `src/it_screen_win32.c`), `SetFolder` from `start_dir` via `SHCreateItemFromParsingName`, filters per kind (data-model table), `FOS_PICKFOLDERS` for folders, `Show`, `GetResult`→`GetDisplayName(SIGDN_FILESYSPATH)`; convert per research R5 (ANSI with `WC_NO_BEST_FIT_CHARS`, lossy → `GetShortPathNameW`), `display` via `Screen_UnicodeToCP437` per UTF-16 unit, then `InputFlushAfterDialog`
- [X] T019 [P] [US1] Implement the open-file and pick-folder parts of `src/it_dialog_mac.c` via the Objective-C runtime from C: `NSOpenPanel openPanel`, `setCanChooseFiles:`/`setCanChooseDirectories:`/`setAllowsMultipleSelection:NO`, `setAllowedFileTypes:` (NSArray built with CFArray toll-free) for open kinds, `setDirectoryURL:` from `start_dir`, `runModal` == 1 → `URL.path.UTF8String`; `display` via `Screen_Utf8ToCP437Display`; afterwards make the SDL window key again (`SDL_RaiseWindow`); then the SDL input flush
- [X] T020 [P] [US1] Implement the open-file and pick-folder parts of `src/it_dialog_posix.c`: locate `zenity` then `kdialog` on `PATH`; build argv per research R4 table; `posix_spawnp` with stdout pipe; loop `waitpid(WNOHANG)` while calling `SDL_PumpEvents()` and presenting every ~20 ms; exit 0 + first stdout line → path (strip newline); leave fullscreen before and restore after; `display` via `Screen_Utf8ToCP437Display`; neither helper → UNAVAILABLE "No file dialog available"; then the SDL input flush
- [X] T021 [US1] Wire `file_dialog` in `Screen_BackendWin32` (`src/it_screen_win32.c`) to the Win32 entry, and in `Screen_BackendSDL` (`src/it_screen_sdl.c`) to the mac entry under `__APPLE__`, else the posix entry; make the internal `InputFlushAfterDialog` callable by the dialog files (non-static wrapper declared in a small internal header comment block or `extern` in each file)
- [X] T022 [US1] Add DLG checks in `src/it_editor.c`: fake `testdata/itdemo.it` via Ctrl-Shift-F9 (feed `ITK_CTRL_SHIFT_F9` through the key path) → song equals a `do_load_named("testdata/itdemo.it")` load (compare song name, order count, pattern 0 packed bytes) and working directory is `testdata`; restore working directory; `!cancel` → song unchanged; `!unavailable` → status contains "No file dialog available"; a 1100-character fake path → REJECTED, song unchanged; feed `ITK_SHIFT_F9` → message editor screen opens

**Checkpoint**: US1 complete and testable alone (MVP).

---

## Phase 4: User Story 2 - Save a module with "Save As" (Priority: P2)

**Goal**: Ctrl-Shift-F10 opens the native save dialog; file written through the F10 writer, format from type/extension.

**Independent Test**: Save via Ctrl-Shift-F10 and via F10 under the same name/format; files byte-identical (quickstart rows 4–6).

- [X] T023 [US2] Implement `act_dialog_save_module()` in `src/it_editor.c`: suggest `FileNameDisp` or `UNTITLED.IT`, pre-select `SaveFormat`; on CHOSEN split path, apply extension rule (missing/unknown → append `.it`, format IT; `.s3m` → S3M), `chdir(dir)`, existing-file check + `confirm_overwrite(draw_screen)`, `commit_current_pattern()`, temporarily set `SaveFormat` to the chosen format, `save_module_dispatch(base)`, restore `SaveFormat`, update `FileNameDisp` from the display base name as `req_do_save` does, status "Saved." / "Unable to save file"; on failure restore the previous working directory
- [X] T024 [US2] Bind `ITK_CTRL_SHIFT_F10` in `handle_global()` in `src/it_editor.c` to `act_dialog_save_module()`; confirm F10 and Ctrl-S paths are untouched
- [X] T025 [P] [US2] Add the save part to `src/it_dialog_win32.c`: `CLSID_FileSaveDialog`, `SetFileTypes` (IT, S3M), `SetFileTypeIndex` from `save_format`, `SetDefaultExtension(L"it")`, `SetFileName` from `suggest_name`, clear `FOS_OVERWRITEPROMPT`, read `GetFileTypeIndex` → `save_format`; lossy new names: `CreateFileW(OPEN_ALWAYS)` then `GetShortPathNameW`, short == long → REJECTED "Can't save under this name on this drive" (delete the just-created empty file in that case)
- [X] T026 [P] [US2] Add the save part to `src/it_dialog_mac.c`: `NSSavePanel savePanel`, `setAllowedFileTypes:` (it, s3m), `setAllowsOtherFileTypes:YES`, `setNameFieldStringValue:`, `setDirectoryURL:`, `runModal`, path; `save_format` from the extension
- [X] T027 [P] [US2] Add the save part to `src/it_dialog_posix.c`: zenity `--save` without `--confirm-overwrite` / kdialog `--getsavefilename` with `DIR/NAME` and the IT/S3M filters; `save_format` from the extension
- [X] T028 [US2] Add DLG checks in `src/it_editor.c`: with `testdata/itdemo.it` loaded, fake-save to a temp folder as `dlg_a` (no extension) → `dlg_a.it` exists and is byte-identical to a `save_module_dispatch("dlg_b.IT")` write with `SaveFormat` 0; fake-save `dlg_c.s3m` → byte-identical to an S3M dispatch write; `SaveFormat` afterwards equals its value before; `!cancel` writes nothing; clean up temp files and restore the working directory

**Checkpoint**: US1 + US2 work independently.

---

## Phase 5: User Story 5 - Platform-aware F1 help (Priority: P2)

**Goal**: macOS help names Right Option for preview; all builds with dialogs list the new keys as marked additions.

**Independent Test**: F1 in the pattern editor on macOS shows "Right Option+Key"; on Windows/Linux the original line; additions present on global/F3/F4/F12 help (quickstart rows 15–17).

- [X] T029 [US5] Set `preview_key = "Right Option"` in `Screen_BackendSDL` under `__APPLE__` in `src/it_screen_sdl.c`; add `const char *Screen_PreviewKeyLabel(void)` and `int Screen_HasFileDialog(void)` accessors in `src/it_screen.c` / `src/it_screen.h` (both false/NULL when no backend is active)
- [X] T030 [US5] Add the port help table in `src/it_editor.c` near `draw_help()`: `PortHelpPreview[]` = `{0x05, "Right Option+Key", 0xFF, 0x01, 0x20, " Preview ", 0xA5, 0x00}` (byte array; "Preview" lands in the original column), a heading line `"it26 additions:"`, global lines (Ctrl-Shift-F9 / Ctrl-Shift-F10), sample-list line, instrument-list line, configuration line, each starting at column 5 with the description in the column the context's original lines use (per `contracts/keys-and-help.md`)
- [X] T031 [US5] Add `static const uint8_t *const *help_lines(int ctx)` in `src/it_editor.c`: if neither preview override nor dialogs apply, return `HelpContextPtrs[ctx]`; else build once (per context, and for context 5 per "opened from F12" vs not) a heap copy of the original list with `HLP_helpcontext1_181` replaced by `PortHelpPreview` when `Screen_PreviewKeyLabel()` is set, then appended (only when `Screen_HasFileDialog()`): `HLP_newline`, heading, global lines, plus the context lines for ctx 2 (sample list), 7 (instrument list), and the configuration line only when `HelpReturnScreen == SCR_VARS`; NULL-terminated
- [X] T032 [US5] Replace the direct `HelpContextPtrs[HelpContext]` uses in `draw_help()` and `help_key()` in `src/it_editor.c` with `help_lines(HelpContext)`; verify scrolling limits still use the (longer) merged list
- [X] T033 [US5] Add DLG checks in `src/it_editor.c`: with the fake hook active (`Screen_HasFileDialog()` true under `ITED_DIALOG_FAKE`), the pattern-editor help list ends with the heading and the two global lines; with no backend dialog and no fake, the list equals `HelpContextPtrs[1]` exactly (pointer-by-pointer); the preview override is unit-checked by expanding `PortHelpPreview` with `help_expand` and asserting "Preview" starts at the same column as in the expanded `HLP_helpcontext1_181`

**Checkpoint**: help is correct per platform independently of US3/US4.

---

## Phase 6: User Story 3 - Load a sample or instrument with the OS dialog (Priority: P3)

**Goal**: Ctrl-O on F3/F4 loads the chosen file into the current slot through the original load code; a module opens the Load Sample/Instrument screen inside it.

**Independent Test**: F3 slot 05 → Ctrl-O → WAV; identical to loading via the Load Sample screen; same for F4 with an `.iti` (quickstart rows 7–9).

- [X] T034 [P] [US3] Add `int RIS_IdentifyFile(const char *path, slibent_t *e)` to `src/it_ris.c` / `src/it_ris.h`: runs the existing single-file identification used by `ls_add`/`ls_identify` for one file; returns the entry type (sample / module / unknown) so callers can branch *(superseded: `lib_open_source()` in `src/it_editor.c` already identifies a single file and loads it or opens the module as a library; no new `it_ris.c` function needed)*
- [X] T035 [P] [US3] Add `int RI_IdentifyFile(const char *path, ilibent_t *e)` to `src/it_ris.c` / `src/it_ris.h` using the instrument scanner's per-file path (`RI_ScanModule` logic for single-instrument files such as ITI/XI), returning instrument / module / unknown *(superseded: see T034)*
- [X] T036 [US3] Implement `act_dialog_load_sample()` in `src/it_editor.c`: `ed_dialog(IT_DLG_OPEN_SAMPLE, LsDir or DirSample, …)`; split path; reject folder longer than `sizeof(LsDir)-1` ("Path too long"); `RIS_IdentifyFile`: sample → `lib_load_sample_entry(&e)` then `ls_set_dir(dir)` on success; module → `ls_set_dir(dir)` and open the Load Sample screen inside that module (`load_sample_screen_run` + `ls_enter_module` path used when entering a module there); unknown → status "Unable to load sample." *(module picked → sample library view inside it via `lib_open_source`, not the Load Sample screen)*
- [X] T037 [US3] Implement `act_dialog_load_instrument()` in `src/it_editor.c` analogously with `RI_IdentifyFile`, `lib_load_instrument_entry(&e)` (keeps "Replace instrument N?" and out-of-samples checks), updating the Load Instrument directory and `DirInstr` on success; module → open the instrument library browse inside it (`lib_browse_run(1, path)`)
- [X] T038 [US3] Bind char `0x0F` (Ctrl-O) on SCR_SAMPLES and SCR_INSTRUMENTS key handling in `src/it_editor.c` to the two actions (only when no text field/edit mode of that screen is active)
- [X] T039 [US3] Add DLG checks in `src/it_editor.c`: on F3 with `ListSel = 4`, fake a WAV from `testdata/` → `Song.Smp[4]` equals the result of loading the same file through `RIS_IdentifyFile` + `lib_load_sample_entry` into a scratch copy (name, length, loop begin/end, C5 speed, data bytes); `!cancel` leaves slot 5 unchanged; on F4 fake an instrument file from `testdata/` (add a small `.iti` fixture under `testdata/` if none exists) → slot matches the library path load *(sample check uses `testdata/lib_test8.wav`, instrument check `testdata/lib_test.xi`; no new fixture)*

**Checkpoint**: US3 works independently of US2/US4.

---

## Phase 7: User Story 4 - Pick F12 directories with a folder picker (Priority: P3)

**Goal**: Ctrl-O on a focused F12 path field writes the chosen folder into that field.

**Independent Test**: F12, focus Sample path, Ctrl-O, choose folder → field shows it; Load Sample opens there (quickstart rows 10–11).

- [X] T040 [US4] Implement `act_dialog_pick_folder()` in `src/it_editor.c`: only when `Screen == SCR_VARS` and the focused widget is the `WT_TEXT` whose `text` is `DirModule`, `DirSample` or `DirInstr`; start in that field's folder (fallback cwd); on CHOSEN (validation already rejected lossy and >64) copy `r.path` into the field buffer
- [X] T041 [US4] Bind char `0x0F` on SCR_VARS in `src/it_editor.c` to `act_dialog_pick_folder()`; Ctrl-O on any other F12 control does nothing
- [X] T042 [US4] Add DLG checks in `src/it_editor.c`: focus the Sample path field, fake `testdata` → `DirSample` == `testdata`; fake a folder name containing a UTF-8 emoji → field unchanged and status contains "cannot show"; fake a 70-character folder → field unchanged, status contains "too long"; focus a non-path control → Ctrl-O changes nothing

**Checkpoint**: all five stories functional.

---

## Phase 8: Polish & Cross-Cutting

- [X] T043 [P] Add a "Deliberate deviations" entry to `README.md` (Fidelity notes): Ctrl-Shift-F9 / Ctrl-Shift-F10 / Ctrl-O system dialogs (where they work, Linux helper requirement, terminal has none), `?` display for unrepresentable names, macOS help wording for Right Option, "it26 additions" help lines
- [X] T044 [P] Add an Unreleased entry to `CHANGELOG.md` (issue #26, credit esaruoho)
- [X] T045 [P] Add the deviations to `docs/HANDOFF.md` §6 (new keys, port help table, dialog files per platform, `ITED_DIALOG_FAKE` hook)
- [X] T046 Build on Windows (MSVC, CMake and direct `cl`) and Linux (gcc, SDL and terminal), syntax-check `src/it_dialog_mac.c` against SDL headers with `-D__APPLE__` stubs where possible; fix warnings at `/W3` / `-Wall`
- [X] T047 Run `ITED_SELFTEST=1 ITED_TERM=1 ./ited testdata/itdemo.it` on Windows and Linux: all previous blocks plus `[DLG OK]`
- [X] T048 Run the determinism gate `test_pattern` (all four testdata modules IDENTICAL)
- [ ] T049 Manual checklist `specs/016-native-file-dialogs/quickstart.md` §3 on Windows (rows 1–14, 16–17, 20); hand the macOS (15, 19) and Linux (18, 19) rows to the owner for testing on those machines *(partly: Windows rows 1, 4, 17 done with the real dialog by simulated input; rest + macOS/Linux rows are the owner's hand test)*

---

## Dependencies & Execution Order

### Phase dependencies

- Phase 1 (Setup) → Phase 2 (Foundational) → user stories.
- US1 (Phase 3) delivers the platform dialog implementations' open/folder parts that US3 and US4 reuse; US2 adds the save parts.
- US5 (Phase 5) depends only on Phase 2 (T005 `preview_key`, `Screen_HasFileDialog`), not on US1–US4; its additions are visible once any `file_dialog` is wired (T021) or under the fake hook.
- US3 and US4 depend on US1's T018–T021 (open/folder dialogs) but not on US2.
- Polish after the desired stories.

### Story completion order

```
Setup → Foundational → US1 (MVP) ─┬─ US2
                                  ├─ US3
                                  └─ US4
          Foundational ─────────── US5
```

### Within each story

Platform files ([P]) can be written in parallel; editor actions, then key binding, then DLG checks.

## Parallel Execution Examples

- **Phase 2**: T009 (Win32 keys) ∥ T010 (SDL keys) ∥ T011 (Win32 flush) ∥ T012 (SDL flush), after T004–T006.
- **US1**: T018 (Windows) ∥ T019 (macOS) ∥ T020 (Linux) while T015–T017 are written in `it_editor.c`.
- **US2**: T025 ∥ T026 ∥ T027.
- **US3**: T034 ∥ T035 (both `it_ris.c`, different functions; merge carefully or do sequentially if editing conflicts).
- **Polish**: T043 ∥ T044 ∥ T045.

## Implementation Strategy

### MVP first

1. Phases 1–2.
2. Phase 3 (US1) → validate open-module on Windows; owner validates on macOS/Linux.
3. Stop and demo: Ctrl-Shift-F9 works everywhere.

### Incremental delivery

US1 → US2 (save) → US5 (help, small) → US3 (samples/instruments) → US4 (F12 folders, the original request #26) → Polish. Each step keeps the selftest green and the determinism gate IDENTICAL.

## Notes

- Never edit `src/it_help.inc` or `tools/gen_help.py`; never touch engine files or `external/miniaudio.h`.
- The macOS and Linux dialog code cannot run on this Windows machine; their correctness is confirmed by the owner's manual tests.
- Commit only when the owner asks; reference issue #26 with `Refs:` and credit `Reported-by: esaruoho`.
