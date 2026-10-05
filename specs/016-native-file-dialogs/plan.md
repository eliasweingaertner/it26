# Implementation Plan: Native File Dialogs and Platform-Aware Help

**Branch**: `016-native-file-dialogs` | **Date**: 2026-10-02 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/016-native-file-dialogs/spec.md`

## Summary

Add the operating system's own file and folder dialogs as optional
shortcuts next to Impulse Tracker's original file screens (issue #26):
Ctrl-Shift-F9 (open module) and Ctrl-Shift-F10 (save module as) from any
screen, Ctrl-O on F3/F4 (load sample/instrument into the current slot) and
on F12 path fields (folder picker). Every result goes through the code the
original screens already use, so results are identical. The dialogs sit
behind the backend vtable: Win32 `IFileDialog` from C, macOS
`NSOpenPanel`/`NSSavePanel` via the Objective-C runtime from C, Linux
`zenity`/`kdialog` as child processes; the terminal backend has none.
Paths are carried twice, OS-usable and CP437 display (`?` substitution).
The F1 help gets a macOS override for the Caps Lock preview line (Right
Option) and marked "it26 additions" lines for the new keys, without
touching the generated help data.

## Technical Context

**Language/Version**: C11 (MSVC `/std:c11`, gcc, clang)

**Primary Dependencies**: existing: SDL2 API (sdl2-compat on macOS),
Win32/GDI, miniaudio (untouched). New system libraries only: Windows
`ole32`, `shell32`, `uuid` (COM file dialogs); macOS AppKit classes reached
through the already-linked `objc` + `CoreFoundation`; Linux runtime
helpers `zenity` or `kdialog` (optional, not linked, not bundled)

**Storage**: files on disk (modules, samples, instruments); `ited.cfg`
directories (unchanged format)

**Testing**: `ITED_SELFTEST` (new DLG block via the `ITED_DIALOG_FAKE`
hook), `tests/test_pattern.c` determinism gate, manual checklist in
[quickstart.md](./quickstart.md) on Windows, macOS, Linux

**Target Platform**: Windows 10+ (Win32 pixel backend), macOS 11+ (SDL
backend, arm64), Linux desktop (SDL backend, X11/Wayland)

**Project Type**: desktop application (tracker), single C project

**Performance Goals**: dialog appears without noticeable delay
(< 0.5 s after the key); audio never drops while a dialog is open

**Constraints**: no change to engine or generated data; IT 2.14 keys keep
their behaviour; no SDL3 migration; no new vendored dependency; C11 only
(no Objective-C or C++ sources)

**Scale/Scope**: 5 dialog entry points, 3 platform dialog files, ~2 new
key codes, 1 help override + 4 help addition blocks, 1 selftest block

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)**: PASS. No engine or
  pattern-format file is touched. `test_pattern` is re-run anyway as the
  release gate (quickstart §2).
- **Authentic data (II)**: PASS with recorded deviations. No layout,
  colour or coordinate changes; the generated `it_help.inc` is not edited
  and `tools/gen_help.py` is not changed. The macOS preview wording and
  the "it26 additions" help lines are port-owned text, kept in a
  separate editor table, visibly marked, and listed in the README fidelity
  notes and `docs/HANDOFF.md` §6.
- **One real engine (IV)**: PASS. Loads and saves call the existing paths
  (`do_load_named`, `save_module_dispatch`, `lib_load_sample_entry`,
  `lib_load_instrument_entry`); sample transfers keep their
  `ed_lock`/`ed_unlock`.
- **Portability (V)**: PASS. Dialogs are a new optional vtable member
  (`file_dialog`) plus a data member (`preview_key`); platform code lives
  in three backend-side files; the shared editor has no platform
  `#ifdef`. All sources are C11 (macOS uses the Objective-C runtime from
  C). `external/miniaudio.h` untouched.

*Post-design re-check (after Phase 1)*: PASS, unchanged. The design adds
no engine code, no hand-edited generated data, and keeps platform code
behind the vtable.

## Project Structure

### Documentation (this feature)

```text
specs/016-native-file-dialogs/
├── plan.md              # This file
├── research.md          # Phase 0: decisions R1–R13
├── data-model.md        # Phase 1: request/result types, backend members, help lists
├── quickstart.md        # Phase 1: build, automated + manual validation
├── contracts/
│   ├── keys-and-help.md     # user-facing keys, messages, help lines
│   └── backend-dialog.md    # Screen_FileDialog + backend obligations
├── checklists/
│   └── requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks)
```

### Source Code (`it26/`)

```text
src/
├── it_screen.h          # + it_dialog_req_t / it_dialog_res_t, ITK_CTRL_SHIFT_F9/F10,
│                        #   screen_backend_t.file_dialog / .preview_key, Screen_FileDialog()
├── it_screen.c          # + Screen_FileDialog (fake hook, validation), UTF-8→CP437 display helper
├── it_screen_win32.c    # + Ctrl-Shift-F9/F10 before Shift-F9; file_dialog hookup; input flush
├── it_screen_sdl.c      # + Ctrl-Shift-F9/F10 before Shift-F9; file_dialog hookup (mac/posix);
│                        #   input flush; preview_key = "Right Option" on macOS
├── it_dialog_win32.c    # NEW: IFileOpenDialog / IFileSaveDialog (COM from C), ANSI/8.3 paths
├── it_dialog_mac.c      # NEW: NSOpenPanel / NSSavePanel via objc runtime
├── it_dialog_posix.c    # NEW: zenity / kdialog via posix_spawnp, event pumping
├── it_ris.c / it_ris.h  # + RIS_IdentifyFile, RI_IdentifyFile (single-file identification)
└── it_editor.c          # + dialog actions (open/save module, sample, instrument, F12 folder),
                         #   key bindings, help_lines() accessor + port help table, DLG selftest
CMakeLists.txt           # + new sources per platform; Windows ole32/shell32/uuid
README.md                # + fidelity note (keys, macOS help wording)
CHANGELOG.md             # + entry
docs/HANDOFF.md          # + §6 deviation entries; direct-build command lists new files/libs
```

**Structure Decision**: Single existing C project. Platform dialog code is
split into one file per platform and compiled only for its backend
(`it_dialog_win32.c` with the Win32 backend; `it_dialog_mac.c` /
`it_dialog_posix.c` with the SDL backend on Apple / non-Apple). The editor
reaches them only through `Screen_FileDialog()`.

## Complexity Tracking

No constitution violations to justify. Recorded deviations from the DOS
binary (README fidelity notes, HANDOFF §6): the three new keys; the macOS
help wording; the "it26 additions" help lines.
