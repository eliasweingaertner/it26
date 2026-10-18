# Contract: Keys, screens and messages

All keys are unused by IT 2.14 on the screen named (verified against IT's key
tables during implementation); IT's own bindings keep precedence where a
screen list already uses the key.

## F2 — Pattern editor

| Key | Action |
|---|---|
| Ctrl-O | render the editor's pattern, import (single-pattern, resume) |
| Shift-Ctrl-O | render the editor's pattern to the render folder (single-pattern, resume) |
| Shift-Alt-M | stems: one WAV per channel with a note 0..119 that is not muted (Alt-M stays block mix) |

## F11 — Order list

| Key | Action |
|---|---|
| Ctrl-O / Shift-Ctrl-O | render + import / render to folder — the playing pattern, else the pattern at the order cursor |
| Right on digit 2 (right-most) | as Ctrl-O; on digits 0–1 IT's cursor move |
| Shift-Right on digit 2 | as Shift-Ctrl-O; elsewhere no action |
| Shift-G | as Ctrl-O (plain G: IT's goto pattern) |
| Ctrl-G | as Shift-Ctrl-O |
| Ctrl-Shift-Right | export all samples |

## F5 — Info page

| Key | Action |
|---|---|
| Shift-Right, Ctrl-O | render the playing pattern (stopped: the editor's) to the render folder; channel selection unchanged |
| D, Ctrl-Shift-Right | export all samples |
| Right | IT's: next channel |

## F3 — Sample list

| Key | Action |
|---|---|
| Ctrl-Alt-Right | export the selected sample (header rate 44100); Ctrl-Right stays IT's next order |
| Ctrl-Shift-Right | export all samples |
| Alt-W | IT's: save sample as WAV (precedes the global Alt-W) |

## F10 — Save module

Format buttons: IT214, S3M, IT2xx, IT215, **WAV** (69,24–77,26),
**MWAV** (69,27–77,29). Links: IT215 Down → WAV; WAV Up/Down → IT215/MWAV;
MWAV Up → WAV, Down → file name; Left/Shift-Tab → drives; Right/Tab → file
name. Saving with WAV / MWAV selected renders the whole song under the typed
name; Ctrl-S and Alt-W do the same while WAV / MWAV is the save format.

## F12 — Song variables

"Quicksave" row under Instrument: input at (13,45), 64 characters; directory
box 12,41–78,46; "Save all Preferences" at 27,47–52,49. Enter on the row opens
a folder picker. Saved as `quicksavedir=`.

## Global

| Key | Action |
|---|---|
| Alt-W | save the module to the Quicksave folder under its name and format; empty folder → message + F10 |
| Shift-Alt-W | Quicksave folder = the current module folder; preferences saved |
| Esc / any render gesture during a render | stop it, finish the file |

## Status-line texts (verbatim from Esa unless noted)

- `Rendering pattern to WAV ...`
- `Pat NNN -> <file> -> sample NNN` (+ ` in inst MMM`)
- `Pattern truncated to 1MB; sample N`
- `Import failed: no free sample slot`
- `Saved pat NNN as <file> in <folder>`
- `Render halted -- finalizing to Quicksave ...`
- `Render aborted: Quicksave folder invalid (check F12)`
- `Dumped N samples as <HHMMSS>nn.WAV in <folder>` (it26: Esa's text says SMPnn.WAV, which his build no longer writes) · `No samples to dump`
- `Saving selected sample as C-4 44.1kHz WAV...` · `Saved C-4 44.1kHz WAV (sample N)` · `Selected sample is empty.`
- `Quick-save folder not configured.` · `Quick-save folder memorized.`
- it26: stems / whole song progress `Rendering channel NN ...` / `Rendering song ...`
