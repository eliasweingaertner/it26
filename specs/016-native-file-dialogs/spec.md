# Feature Specification: Native File Dialogs and Platform-Aware Help

**Feature Branch**: `016-native-file-dialogs`

**Created**: 2026-10-02

**Status**: Draft

**Input**: User description: "Native OS file dialogs and platform-aware help (GitHub issue #26 by esaruoho). A deliberate, documented extension to the 1:1 Impulse Tracker 2.14 port, available in the SDL (macOS/Linux) and Win32 backends; the terminal backend does not offer it. (1) Ctrl-Shift-F9 opens the operating system's native "Open" dialog for modules from any screen, without switching screens; the chosen file loads exactly as if picked on the F9 Load Module screen. (2) Ctrl-Shift-F10 opens the native "Save As" dialog from any screen; the chosen name and format go through the existing module save path (same as F10 Save Module). (3) Ctrl-O on the F3 sample list / F4 instrument list opens a native file picker and loads the chosen sample/instrument into the current slot, as the Load Sample / Load Instrument screens would. (4) Ctrl-O on a focused F12 path field (Module, Sample, Instrument directories) opens a native folder picker and writes the chosen folder into that field. All four key combinations are unused in IT 2.14 (verified against the original key tables). Both backends currently send Shift-F9 (message editor) before checking Ctrl, so Ctrl-Shift-F9 must be recognised first. Cancelling a dialog changes nothing. Paths that the 8-bit CP437 screen cannot show or that exceed field length must be handled explicitly. After a dialog closes, stuck modifier/key state must not leak into the tracker. Native pickers: Windows, macOS, Linux (zenity/kdialog, graceful no-op if neither is installed); no SDL3 migration. Audio keeps playing while a dialog is open. (5) Platform-aware F1 help: on macOS the pattern editor help line "Caps Lock+Key  Preview note" (IT_H.ASM:706) reads "Right Option+Key" instead, matching the macOS Right Option preview from issue #20, without disturbing the original layout; the help also lists the new dialog keys where they apply. README gets a deviation note for the new keys."

## Context

Impulse Tracker's own file screens (F9 Load Module, F10 Save Module, the
Load Sample / Load Instrument screens, the F12 directory fields) are DOS-era:
a drive list, a directory list and typed paths. On a modern desktop,
finding a file in a deep folder tree or on a network share is slow that way,
and typing a long path into the F12 fields is error-prone (issue #26).

This feature adds the operating system's own file and folder dialogs as an
**optional shortcut** next to the original screens. The original screens,
their keys and their behaviour stay exactly as they are. The new keys were
chosen because Impulse Tracker 2.14 binds none of them:

| Key | Where | Opens | Result |
|---|---|---|---|
| Ctrl-Shift-F9 | any screen | OS "Open" dialog (modules) | module loads as from F9 |
| Ctrl-Shift-F10 | any screen | OS "Save As" dialog (modules) | module saves as from F10 |
| Ctrl-O | F3 sample list | OS "Open" dialog (samples) | sample loads into the current slot |
| Ctrl-O | F4 instrument list | OS "Open" dialog (instruments) | instrument loads into the current slot |
| Ctrl-O | F12, focused path field | OS folder picker | folder written into that field |

The second part makes the F1 help match the platform: on macOS, where the
note preview key is Right Option (issue #20) instead of Caps Lock, the help
says so; and the help mentions the new dialog keys.

## Clarifications

### Session 2026-10-02

- Q: How are chosen paths with characters the tracker's 8-bit screen cannot display handled? → A: Open, save and load use the real path unchanged; only its on-screen display is approximate (`?` for each unrepresentable character). F12 path fields still reject such folders. Proper Unicode display of file names (an embedded 8×8 Unicode bitmap font across all file screens) is recorded as a separate follow-up feature, out of scope here.
- Q: Should a dialog change the tracker's current directories? → A: Yes. After a successful open, save or load, the folder of the chosen file becomes the current module / sample / instrument directory, exactly as browsing there on the corresponding original screen would; the next original screen and the next dialog open there.
- Q: How does "Save As" decide the file format? → A: The dialog offers "Impulse Tracker (*.it)" and "Scream Tracker 3 (*.s3m)"; the chosen type or the typed extension decides the format; a missing or unknown extension saves as IT and appends `.it`.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Open a module with the OS dialog (Priority: P1)

A musician working in the pattern editor wants to open a module that lives
somewhere deep in their music folder. They press Ctrl-Shift-F9, the
operating system's familiar "Open" dialog appears (with its recent folders,
favourites and search), they pick the file, and the module is loaded exactly
as if they had found it on the F9 Load Module screen. They never leave the
screen they were on, other than whatever loading a module normally does.

**Why this priority**: Loading modules is the most frequent file task and
the one where the DOS-style browser hurts most. It is also the smallest
slice that proves the whole mechanism (dialog, key handling, hand-off to the
existing load path) on all three platforms.

**Independent Test**: On each platform, press Ctrl-Shift-F9 from the pattern
editor, pick a module in a folder that is not the current module directory,
and confirm the module loads and plays identically to loading it via F9.

**Acceptance Scenarios**:

1. **Given** any tracker screen, **When** the user presses Ctrl-Shift-F9,
   **Then** the operating system's native "Open" dialog appears, starting in
   the configured module directory.
2. **Given** the dialog is open, **When** the user picks a module file and
   confirms, **Then** the module is loaded through the same path as the F9
   Load Module screen (same unsaved-changes prompt, same result), and the
   file's folder becomes the current module directory, so the next F9 or
   Ctrl-Shift-F9 opens there.
3. **Given** the dialog is open, **When** the user cancels, **Then** nothing
   changes: same screen, same song, same cursor.
4. **Given** a song is playing, **When** the user opens the dialog,
   **Then** playback continues until a new module is actually loaded.
5. **Given** the user presses Shift-F9 (without Ctrl), **Then** the message
   editor still opens as before.

---

### User Story 2 - Save a module with the OS "Save As" dialog (Priority: P2)

The musician wants to save the current song under a new name in a different
folder. They press Ctrl-Shift-F10, the operating system's "Save As" dialog
appears, they choose folder, name and (where offered) format, and the song is
saved exactly as the F10 Save Module screen would save it.

**Why this priority**: The natural partner of P1; together they cover the
module workflow. It depends on the same mechanism, so it is cheap once P1
exists.

**Independent Test**: Press Ctrl-Shift-F10, save to a new folder under a new
name, then reload the file and compare it with a save made through F10: the
files must be byte-identical.

**Acceptance Scenarios**:

1. **Given** any tracker screen, **When** the user presses Ctrl-Shift-F10,
   **Then** the native "Save As" dialog appears, starting in the configured
   module directory with the current song's file name filled in.
2. **Given** the user confirms a name, **Then** the song is written through
   the same save path as F10, in the format chosen: the dialog offers
   "Impulse Tracker (*.it)" and "Scream Tracker 3 (*.s3m)", the chosen type
   or typed extension decides, and a missing or unknown extension saves as
   IT with `.it` appended.
3. **Given** the chosen file already exists, **Then** the user is asked to
   confirm overwriting exactly once (either by the OS dialog or by the
   tracker, not both).
4. **Given** the user cancels, **Then** nothing is written and nothing
   changes.

---

### User Story 3 - Load a sample or instrument with the OS dialog (Priority: P3)

On the F3 sample list (or F4 instrument list), the musician presses Ctrl-O,
picks a sample (or instrument) file in the OS dialog, and it is loaded into
the currently selected slot, exactly as the Load Sample (Load Instrument)
screen would load it.

**Why this priority**: Useful, but users are already on F3/F4 when loading
into a slot, and the existing Load Sample screen (feature 015) works well;
the gain is smaller than for modules.

**Independent Test**: On F3, select slot 05, press Ctrl-O, pick a WAV file
from another folder; slot 05 holds the sample with the same name, length,
loop and rate as when loaded through the Load Sample screen. Repeat on F4
with an instrument file.

**Acceptance Scenarios**:

1. **Given** the F3 sample list, **When** the user presses Ctrl-O and picks
   a supported sample file, **Then** it is loaded into the current slot
   with the same result as the Load Sample screen, and the slot and screen
   stay selected.
2. **Given** the F4 instrument list, **When** the user presses Ctrl-O and
   picks a supported instrument file, **Then** it is loaded into the
   current instrument slot with the same result as the Load Instrument
   screen.
3. **Given** the user picks a file the tracker cannot read, **Then** the
   same error the original load screens show for that file appears, and the
   slot is unchanged.
4. **Given** the user cancels, **Then** nothing changes.

---

### User Story 4 - Pick the F12 directories with a folder picker (Priority: P3)

On the F12 settings screen, the musician moves the cursor to the Module,
Sample or Instrument path field, presses Ctrl-O, picks a folder in the OS
folder picker, and the folder's path appears in that field (issue #26).

**Why this priority**: The original request, but a small convenience; the
fields can still be typed.

**Independent Test**: On F12, focus the Sample path field, press Ctrl-O,
choose a folder; the field shows that folder, and the Load Sample screen
opens in it afterwards.

**Acceptance Scenarios**:

1. **Given** a focused F12 path field, **When** the user presses Ctrl-O,
   **Then** the native folder picker opens, starting in the folder the field
   currently names (or the working directory if it is empty or invalid).
2. **Given** the user picks a folder, **Then** that path is written into the
   focused field only, and is used wherever that directory is used, just as
   if it had been typed.
3. **Given** the focus is on an F12 control that is not a path field,
   **When** the user presses Ctrl-O, **Then** nothing happens.
4. **Given** the user cancels, **Then** the field is unchanged.

---

### User Story 5 - F1 help that matches the platform (Priority: P2)

A Mac user opens the F1 help in the pattern editor. Where Impulse Tracker
says "Caps Lock+Key  Preview note", the Mac build says "Right Option+Key",
the key that actually does it there (issue #20). The help also mentions the
new dialog keys on the screens where they apply.

**Why this priority**: Wrong help is a small but real usability bug on the
Mac (Caps Lock does not preview there), and it is cheap to fix.

**Independent Test**: On macOS, open F1 help in the pattern editor and find
the preview line; on Windows and Linux the same line reads exactly as in
Impulse Tracker 2.14.

**Acceptance Scenarios**:

1. **Given** the macOS build, **When** the user opens F1 help in the pattern
   editor, **Then** the preview line names Right Option instead of Caps Lock,
   and all other lines, columns and colours are unchanged.
2. **Given** the Windows or Linux build, **Then** the help is byte-for-byte
   the original Impulse Tracker 2.14 text, except for the added dialog-key
   lines.
3. **Given** any build with the dialogs available, **When** the user opens
   F1 help on a screen where a dialog key applies (global keys, F3, F4,
   F12), **Then** the help lists that key, set apart so it is clearly an
   addition to the original help.

### Edge Cases

- **No dialog program on Linux**: if neither supported dialog helper is
  installed, the keys do nothing visible except a short status message
  saying no file dialog is available; nothing else changes.
- **Terminal backend / headless**: the keys are not offered; pressing them
  does nothing.
- **Unrepresentable characters**: a chosen file whose path contains
  characters the tracker's 8-bit character set cannot show is still opened,
  saved or loaded under its real path; wherever the tracker displays that
  name (song file name, sample name from the file, status line), each such
  character shows as `?`. A folder chosen for an F12 path field that
  contains such characters is rejected with a status message, and the
  field is unchanged.
- **Overlong paths**: a chosen path longer than the field or internal path
  limit is rejected with a status message explaining why, and nothing
  changes (no truncated path is ever stored or used).
- **Unsaved changes**: opening a module through Ctrl-Shift-F9 asks the same
  "unsaved changes" question F9 loading asks, after a file is chosen
  (cancelling the dialog never asks).
- **Keys held when the dialog opens/closes**: after the dialog closes, no
  modifier (Shift, Ctrl, Alt, Right Option) or other key is treated as still
  held, and no keystroke typed into the dialog reaches the tracker.
- **Fullscreen**: the dialog appears in front of the tracker and receives
  keyboard focus also when the tracker is fullscreen; after closing, the
  tracker returns to the same window state.
- **Dialog opened while a dialog/prompt of the tracker is showing**: the
  keys are ignored while one of the tracker's own modal prompts is open.
- **Key repeat**: holding Ctrl-Shift-F9 or Ctrl-O opens one dialog, not
  several.
- **Wrong file type picked** (e.g. a sample in the module dialog): the same
  error the original load path gives for that file.

## Requirements *(mandatory)*

### Functional Requirements

**Keys and availability**

- **FR-001**: The tracker MUST open a native module "Open" dialog on
  Ctrl-Shift-F9 from every screen where global keys are active, without
  switching screens before a file is chosen.
- **FR-002**: The tracker MUST open a native module "Save As" dialog on
  Ctrl-Shift-F10 from every screen where global keys are active.
- **FR-003**: The tracker MUST open a native file "Open" dialog on Ctrl-O on
  the F3 sample list and on the F4 instrument list.
- **FR-004**: The tracker MUST open a native folder picker on Ctrl-O while
  an F12 Module, Sample or Instrument path field has focus, and MUST ignore
  Ctrl-O on other F12 controls.
- **FR-005**: Every key binding of Impulse Tracker 2.14 MUST keep its
  behaviour; in particular Shift-F9 MUST still open the message editor and
  F9/F10/Ctrl-F3/Ctrl-F4/Ctrl-F12 MUST be unchanged. Ctrl-Shift-F9 MUST be
  recognised before Shift-F9 on every backend.
- **FR-006**: The feature MUST be available in the Windows pixel backend and
  the SDL backend on macOS and Linux, and MUST be absent (keys inert) in the
  terminal backend and in headless/test runs.

**Results**

- **FR-007**: A module chosen through Ctrl-Shift-F9 MUST be loaded through
  the same load path as the F9 Load Module screen, with identical results
  and prompts.
- **FR-008**: A save through Ctrl-Shift-F10 MUST be written through the same
  save path as the F10 Save Module screen; the file MUST be byte-identical to
  an F10 save of the same song, name and format. The dialog MUST offer the
  types "Impulse Tracker (*.it)" and "Scream Tracker 3 (*.s3m)"; the chosen
  type or the typed extension (`.it` / `.s3m`, case-insensitive) MUST decide
  the format, and a missing or unknown extension MUST save as IT with `.it`
  appended.
- **FR-009**: A sample or instrument chosen through Ctrl-O MUST be loaded
  into the currently selected slot through the same path as the Load Sample /
  Load Instrument screens, with identical results.
- **FR-010**: A folder chosen through the F12 picker MUST replace the
  focused field's content and behave exactly as if typed.
- **FR-011**: Each dialog MUST start in the relevant configured directory
  (module, sample or instrument directory; for F12 the field's current
  folder), falling back to the working directory when that folder does not
  exist.
- **FR-011a**: After a successful open, save or load through a dialog, the
  folder of the chosen file MUST become the current module, sample or
  instrument directory respectively, updated exactly as the corresponding
  original screen (F9, F10, Load Sample, Load Instrument) updates it when
  the user browses there. Cancelled or failed operations MUST NOT change
  any directory.
- **FR-012**: Cancelling any dialog MUST leave song, slots, settings, screen
  and cursor unchanged.
- **FR-013**: Open, save and load MUST use the chosen path unchanged, even
  when it contains characters the tracker's 8-bit character set cannot
  show; the display of such a name MUST substitute `?` per unrepresentable
  character and MUST NOT feed back into the stored path. A folder for an
  F12 path field containing such characters MUST be rejected with a status
  message. Paths exceeding the tracker's path or field length MUST be
  rejected with a status message and MUST NOT be truncated.

**Robustness**

- **FR-014**: Audio playback MUST continue while a dialog is open.
- **FR-015**: After a dialog closes, the tracker MUST discard input that
  arrived while the dialog had focus and MUST treat all modifiers and keys
  as released.
- **FR-016**: Only one dialog MUST be open at a time; repeated or held keys
  MUST NOT open further dialogs.
- **FR-017**: When no native dialog can be shown (e.g. no helper available
  on Linux), the tracker MUST show a status message and change nothing.
- **FR-018**: The dialog keys MUST be ignored while one of the tracker's own
  modal prompts is open.

**Help and documentation**

- **FR-019**: On macOS, the pattern editor F1 help line for the note preview
  MUST name Right Option instead of Caps Lock, keeping the original line's
  position, width, columns and colours.
- **FR-020**: On all platforms where dialogs are available, the F1 help MUST
  list the dialog keys on the screens where they apply (global keys, F3, F4,
  F12), visibly marked as additions; all original help text MUST otherwise
  remain unchanged.
- **FR-021**: The README fidelity notes MUST document the new keys and the
  macOS help wording as deliberate deviations.

### Key Entities

- **Dialog request**: what is being chosen (module to open, module to save,
  sample, instrument, folder), the starting folder, and for saves the
  suggested file name and available formats.
- **Dialog result**: chosen path, or "cancelled", or "unavailable".
- **Configured directories**: the F12 Module, Sample and Instrument paths
  that dialogs start in and that the folder picker writes.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can load a module from any folder on the computer in
  three actions or fewer (key, pick, confirm) from any screen.
- **SC-002**: Files saved through Ctrl-Shift-F10 are byte-identical to files
  saved through F10 for the same song, name and format, in 100% of test
  cases.
- **SC-003**: Samples and instruments loaded through Ctrl-O are identical
  (name, length, loop, rate, data) to the same files loaded through the
  original load screens, in 100% of test cases.
- **SC-004**: No Impulse Tracker 2.14 key binding changes behaviour: the
  existing automated self-test passes unchanged, and Shift-F9, F9, F10,
  Ctrl-F3, Ctrl-F4 and Ctrl-F12 behave as before.
- **SC-005**: Offline renders stay bit-identical (the regression harness
  reports all modules identical).
- **SC-006**: After 20 consecutive open/cancel cycles, no key or modifier is
  stuck and playback has not been interrupted.
- **SC-007**: On macOS the help names Right Option for preview; on Windows
  and Linux the original help text is unchanged apart from the marked
  additions.

## Assumptions

- The native dialogs are those of the operating system (Windows file
  dialogs, macOS open/save panels, and on Linux an installed desktop dialog
  helper such as zenity or kdialog); no new GUI toolkit is bundled.
- The SDL 2 API stays; this feature does not require moving to SDL 3.
- The terminal backend does not get dialogs.
- On a platform whose dialog cannot show a type list (e.g. a minimal Linux
  helper), the typed extension alone decides the format, with the same IT
  fallback.
- Only one file is loaded per dialog (no multi-select).
- Out of scope, follow-up feature: Unicode display of file names (an
  embedded 8×8 Unicode bitmap font such as unscii-8, cells holding full
  characters) for all file screens (F9, F10, F3/F4 load screens, F12, and
  the dialog results), replacing the `?` substitution.
- Linux is tested in a desktop session; the AppImage does not bundle a
  dialog helper.
- Keys follow the user's verification of Impulse Tracker 2.14's key tables:
  Ctrl-Shift-F9, Ctrl-Shift-F10 and Ctrl-O are unbound there; Ctrl-Shift on
  F3/F4/F12 is avoided because Impulse Tracker treats it as Ctrl-F3/F4/F12.
- macOS users reach Ctrl-O with the Control key; Cmd-O is not used (macOS
  keeps Cmd shortcuts for its menu).
