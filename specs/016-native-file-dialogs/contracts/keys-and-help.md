# Contract: Keys and Help Text

The user-facing interface of this feature: key bindings and the F1 help
lines. Behaviour not listed here is unchanged from Impulse Tracker 2.14.

## Key bindings

| Key | Active on | Action | Key code |
|---|---|---|---|
| Ctrl-Shift-F9 | every screen handled by the global key handler (not inside the tracker's own modal prompts, not on F1 help) | open-module dialog | `ITK_CTRL_SHIFT_F9` 0x298 |
| Ctrl-Shift-F10 | same | save-module-as dialog | `ITK_CTRL_SHIFT_F10` 0x299 |
| Ctrl-O | F3 sample list | open-sample dialog → current slot | char `0x0F` |
| Ctrl-O | F4 instrument list | open-instrument dialog → current slot | char `0x0F` |
| Ctrl-O | F12, focus on Module / Sample / Instrument path field | folder picker → that field | char `0x0F` |
| Ctrl-O | anywhere else | nothing | — |

Unchanged and to be regression-checked: Shift-F9 (message editor), F9,
F10, Ctrl-F3, Ctrl-F4, Ctrl-F12, Ctrl-S, Alt-F9, Alt-F10.

**Backend obligations**
- Recognise Ctrl+Shift+F9 / Ctrl+Shift+F10 before the Shift-F9 test.
- Deliver Ctrl-O as `0x0F` (already true for Win32, SDL, terminal).

**Availability**
- Win32 pixel backend: all keys.
- SDL backend (macOS, Linux): all keys; on Linux "unavailable" when
  neither `zenity` nor `kdialog` is installed.
- Terminal backend, headless: keys inert (status message only for
  Ctrl-Shift-F9/F10: "No file dialog available").

## Status messages

| Situation | Message |
|---|---|
| no dialog on this backend / no Linux helper | `No file dialog available` |
| folder with characters the screen cannot show (F12) | `Folder name has characters it26 cannot show` |
| path longer than the target allows | `Path too long` / `Path too long for this field` |
| Windows: name with no 8.3 form on this drive | `Can't save under this name on this drive` |
| load/save failures | the messages the original screens already use |

## F1 help

**macOS (SDL backend, `preview_key = "Right Option"`)**, pattern editor
context, line `HelpContext1_181`:

```
original: "Caps Lock+Key    Preview note"
macOS:    "Right Option+Key Preview note"
```
Same start column (5), "Preview" in the same column, same attribute.

**All platforms with dialogs**, appended at the end of the context, after
the original lines, introduced by a heading:

| Context | Added lines |
|---|---|
| global (all pages' global section) | `it26 additions:` · `Ctrl-Shift-F9   Open module (system dialog)` · `Ctrl-Shift-F10  Save module as (system dialog)` |
| 2 sample list | `it26 additions:` · `Ctrl-O          Load sample (system dialog)` |
| 7 instrument list | `it26 additions:` · `Ctrl-O          Load instrument (system dialog)` |
| 5 configuration | `it26 additions:` · `Ctrl-O          Choose folder (on a path field)` |

Columns follow the original help's key/description layout (key at column
5, description in the original description column of that context).

Terminal backend: no additions, original help only.
