# Research: Native File Dialogs and Platform-Aware Help

Feature: [spec.md](./spec.md) · Plan: [plan.md](./plan.md)

All Technical Context unknowns are resolved below. Each item: Decision /
Rationale / Alternatives considered.

## R1. Where the dialog code lives

**Decision**: A new optional member of the backend vtable,
`screen_backend_t.file_dialog`, plus a platform-neutral wrapper
`Screen_FileDialog()` in `it_screen.c`. Each pixel backend fills the member
from a platform file: `src/it_dialog_win32.c` (Win32 backend),
`src/it_dialog_mac.c` and `src/it_dialog_posix.c` (SDL backend on macOS /
Linux). The terminal backend leaves it `NULL` (keys inert, FR-006).

**Rationale**: Constitution V puts platform specifics behind the vtable and
keeps the shared core free of platform `#ifdef`s. The editor only sees
"ask the host for a path".

**Alternatives considered**: tinyfiledialogs (one vendored file; macOS via
AppleScript, less native, and a second vendored dependency);
nativefiledialog-extended (GTK3/portal dependency for the AppImage); SDL3's
`SDL_ShowOpenFileDialog` (needs an SDL3 migration, and Windows does not use
SDL at all).

## R2. Windows dialog

**Decision**: `IFileOpenDialog` / `IFileSaveDialog` from C through the COM
`lpVtbl`. Folder picking uses `FOS_PICKFOLDERS`; save uses
`SetFileTypes` with "Impulse Tracker (*.it)" and "Scream Tracker 3
(*.s3m)", `SetFileTypeIndex`, `SetDefaultExtension("it")`, and reads the
chosen type back with `GetFileTypeIndex`. `FOS_OVERWRITEPROMPT` is
**cleared** so the tracker's own overwrite prompt (as on F10) is the single
confirmation (spec Story 2, scenario 3). The owner is the tracker window.
`CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)` once on the UI thread;
`RPC_E_CHANGED_MODE` is tolerated. Links `ole32`, `shell32`, `uuid`.

**Rationale**: The modern Windows dialog (Vista+), with recent places and
search; usable from C11 without C++.

**Alternatives considered**: `GetOpenFileNameA` / `SHBrowseForFolder`
(older look, separate folder dialog, ANSI paths only).

## R3. macOS dialog

**Decision**: `NSOpenPanel` / `NSSavePanel` driven from **C** through the
Objective-C runtime (`objc_getClass`, `sel_registerName`, typed
`objc_msgSend` casts), the same technique already used for
`ApplePressAndHoldEnabled` in `it_screen_sdl.c`. `runModal`, then
`URL.path.UTF8String`. Save: `setAllowedFileTypes:@[it, s3m]` +
`setAllowsOtherFileTypes:YES`, `setNameFieldStringValue:` with the current
song name; no type pop-up (the typed extension decides, per the spec
assumption for dialogs without a type list). The tracker window is made
key again afterwards.

**Rationale**: Stays C11 (Constitution V), needs no `.m` file and no CMake
`OBJC` language. AppKit is already loaded in the process by SDL, so no new
framework link; `CoreFoundation` + `objc` are already linked (feature
015/#15).

**Alternatives considered**: an Objective-C `.m` file with an accessory
type pop-up (cleaner API, but breaks "all code is C11" and adds a CMake
language); AppleScript `choose file` via `osascript` (slow, foreign look).

## R4. Linux dialog

**Decision**: Spawn `zenity` (first) or `kdialog` (second) with
`posix_spawnp` and a pipe; no shell, so paths need no quoting. While the
child runs, the backend keeps pumping SDL events and presenting every
~20 ms (`waitpid(WNOHANG)`), so the window manager does not flag the
tracker as "not responding". Commands:

| Kind | zenity | kdialog |
|---|---|---|
| open | `--file-selection --filename=DIR/ --file-filter=…` | `--getopenfilename DIR FILTER` |
| save | `--file-selection --save --filename=DIR/NAME --file-filter=…` (no `--confirm-overwrite`) | `--getsavefilename DIR/NAME FILTER` |
| folder | `--file-selection --directory --filename=DIR/` | `--getexistingdirectory DIR` |

Exit status 0 + one line on stdout = chosen; non-zero = cancelled. If
neither binary is found on `PATH`: result "unavailable" (FR-017). If the
tracker window is fullscreen it leaves fullscreen for the dialog and
returns afterwards (the helper is a separate top-level window that a
fullscreen window would cover).

**Rationale**: The two helpers cover GNOME/KDE and most desktops; no link
dependency, nothing bundled in the AppImage.

**Alternatives considered**: xdg-desktop-portal over D-Bus (most native,
but a D-Bus client in C is a large addition); GTK3 directly (new build and
AppImage dependency).

## R5. Paths with characters the screen cannot show (Clarification Q1)

**Decision**: The dialog result carries **two** strings: `path`, usable
with the existing `fopen`/`chdir` code unchanged, and `display`, CP437 for
the screen with `?` per unrepresentable character.

- macOS/Linux: `path` = the UTF-8 bytes as returned (the C library takes
  them as is). `display` = UTF-8 decoded, each code point through
  `Screen_UnicodeToCP437`, 0 → `?`.
- Windows: the dialog returns UTF-16. `path` = conversion to the ANSI code
  page with `WC_NO_BEST_FIT_CHARS`; if that is lossy, the 8.3 short path
  from `GetShortPathNameW` (pure ASCII). For a *new* file in "Save As" with
  a lossy name, the file is first created with `CreateFileW(OPEN_ALWAYS)`
  so a short name exists, then the short path is used; if the volume has
  8.3 names disabled (short == long), the result is rejected with a status
  message. `display` from the UTF-16 through `Screen_UnicodeToCP437`.
- F12 folder fields: rejected when `display` contains a substituted `?`
  (the field is typed text and must round-trip).

**Rationale**: Keeps every existing file call untouched (they take `char*`
in the C library's encoding) and meets FR-013 exactly.

**Alternatives considered**: switching the Windows process to the UTF-8
code page by manifest (changes behaviour of all existing ANSI calls and
the `RegisterClassA` window's `WM_CHAR`); converting the whole port to
wide-character file APIs (large, out of scope).

## R6. "Current directory" semantics (Clarification Q2)

**Decision**: Mirror what the original screens already do in the port:

| Dialog | Directory updated | How the port's screen does it today |
|---|---|---|
| Open module / Save module | process working directory | F9/F10 requester `chdir()` (`req_enter_dir`) |
| Sample | `LsDir` + `DirSample` | `ls_set_dir()` |
| Instrument | the Load Instrument screen's directory + `DirInstr` | same pattern as `ls_set_dir()` |

Modules: `chdir(folder)` then load/save the base name, so relative names
and later F9/F10 listings agree. Only after success (FR-011a).

**Rationale**: One behaviour for screen and dialog, no new state.

## R7. Save format and naming (Clarification Q3)

**Decision**: Type index or extension (`.it` / `.s3m`, case-insensitive)
selects `SaveFormat` 0/1 **for this save**, written through
`save_module_dispatch()`. Missing/unknown extension → IT and `.it` is
appended. The persistent F10 `SaveFormat` setting is restored afterwards.
Overwrite: the tracker's `confirm_overwrite()` as in `req_do_save`. The
header `FileNameDisp` shows `display`'s base name, upper-cased like F10
does, cut to the header field width (display only; the stored path is not
cut, FR-013).

**Rationale**: Byte-identical output with F10 (FR-008/SC-002) by reusing
the same writer; a one-off format does not silently change the F10
default.

## R8. Loading a sample or instrument from one chosen file

**Decision**: Add `RIS_IdentifyFile(const char *path, slibent_t *e)` and
`RI_IdentifyFile(const char *path, ilibent_t *e)` to `it_ris.c`
(editor-side library code, not engine): they run the existing per-file
identification (`ls_identify` / the instrument scanner) for exactly one
file. Then the existing `lib_load_sample_entry()` /
`lib_load_instrument_entry()` do the load, prompts included ("Create host
instrument?", "Replace instrument N?", out-of-samples check). If the chosen
file is a **module**, the existing Load Sample / Load Instrument screen
opens inside that module (`ls_enter_module` / `lib_browse_run`), exactly
as entering a module on that screen does, so the user picks the sample or
instrument there.

**Rationale**: Identical results to the original screens (FR-009, SC-003)
by sharing every step after identification.

**Alternatives considered**: `RIS_ListDirectory()` of the whole folder and
picking the entry (identifies every file; slow in large folders).

## R9. Keys

**Decision**: Two new codes `ITK_CTRL_SHIFT_F9` (0x298) and
`ITK_CTRL_SHIFT_F10` (0x299), after `ITK_SHIFT_F5` (0x297). Both pixel
backends test Ctrl+Shift+F9/F10 **before** the existing Shift-F9 test.
Ctrl-O already arrives as code `0x0F` on all backends (Win32 `WM_CHAR`, SDL
Ctrl-letter branch, terminal); the editor binds it on SCR_SAMPLES,
SCR_INSTRUMENTS and SCR_VARS (F12) only. Auto-repeat: the dialog is modal,
so a held key cannot reopen it while open; after closing, the input flush
(R10) drops queued repeats.

**Verification**: IT 2.14 key tables (`IT_M.ASM` `M_FunctionDivider`):
type-0 entries compare the full key word incl. modifier bits; Ctrl/Alt/Shift
types strip modifiers after testing their own; Ctrl-O (`0Fh`),
Ctrl-Shift-F9, Ctrl-Shift-F10 appear in no key list.

## R10. Input state after a dialog (FR-015)

**Decision**: The backend's `file_dialog` ends with a flush: drop queued
key events (`KeyHead = KeyTail`), drop pending OS key/text events
(Win32: `PeekMessage(WM_KEYFIRST..WM_KEYLAST, PM_REMOVE)`; SDL:
`SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT)`), reset the modifier state
(SDL: `SDL_ResetKeyboard()` where SDL ≥ 2.24, else
`SDL_SetModState(KMOD_NONE)`), and queue one `ITK_SHIFT_RELEASE` so the
pattern editor's chord/marking state (`ShiftHeld`) closes cleanly.

## R11. Modality and audio (FR-014, FR-016, FR-018)

**Decision**: The dialog call blocks the UI thread only; the audio device
callback runs on miniaudio's thread and takes `Engine_Lock` itself, and the
editor holds no lock while the dialog is open, so playback continues. The
global key handler runs only from the main loop; the tracker's own modal
prompts run their own key loops and do not dispatch these keys, so the
keys are inert there without extra state.

## R12. Platform-aware help (FR-019/020)

**Decision**: The generated `it_help.inc` stays untouched (Constitution
II). A small editor-side table of **port lines** in the same encoding
(start column + text, `0FFh` repeat) provides:
- an override for `HLP_helpcontext1_181` when the backend reports a preview
  key label (new vtable data member `preview_key`, set to
  `"Right Option"` by the SDL backend on macOS): `"Right Option+Key"` +
  `0FFh,1,' '` + `" Preview "` + dictionary word `0A5h`, so "Preview" stays
  in the same column (13+4 = 16+1);
- addition blocks appended to the global, sample list (2), instrument list
  (7) and configuration (5) contexts when `file_dialog` is available,
  introduced by a heading line "it26 additions" so they are visibly not
  original help.
`draw_help`/`help_key` read the line list through one accessor that
returns the original list or a built-once merged copy.

## R13. Testing without a GUI

**Decision**: `Screen_FileDialog()` honours a test hook
`ITED_DIALOG_FAKE` (`<path>` = chosen, `!cancel`, `!unavailable`) before
asking the backend, so the headless selftest (terminal backend) can drive
all four flows. New selftest block **DLG** checks: open = same song as
`do_load_named`; Save As = byte-identical to an F10 save in IT and S3M;
sample/instrument = identical slot contents to the library path; F12 field
set; cancel = no change; Windows lossy names cannot be faked portably, so
the `display` substitution is unit-checked through the UTF-8 path; overlong
path rejected; Shift-F9 still opens the message editor. Real dialogs are
checked by hand per [quickstart.md](./quickstart.md).
