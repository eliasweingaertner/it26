# Feature Specification: Audio Rendering Extensions (after Esa Ruoho's IT 2.354)

**Feature Branch**: master (no feature branch)

**Created**: 2026-10-18

**Status**: Draft

**Input**: User description: "Build extended audio rendering capabilities exactly as Esa has done it with his extensions" — Esa Juhani Ruoho's fork of the Impulse Tracker source, [esaruoho/impulse-tracker](https://github.com/esaruoho/impulse-tracker) v2.354: pattern-to-WAV render with auto-import (Ctrl-O), render to a "Quicksave" folder, F11 / F5 render gestures, sample export, per-channel stems (Shift-Alt-M), whole-song WAV / MWAV from F10, the Quicksave folder with Alt-W module save, and a headless batch render.

## Context

it26 renders a whole song to WAV only through `itplay -w`. Esa's fork turns
rendering into an everyday tracker gesture: one key bounces the pattern you
are working on into a new sample, or into a folder a DAW on another machine
reads. This feature brings his rendering set to it26 as **extensions** —
on keys IT 2.14 does not use — reproducing his behaviour, names and messages.
His feature cards (`features/*.feature` in his repository) and his assembly
source are the reference; where his card and his code disagree, his code wins,
except for the one bug listed under Clarifications.

| Gesture | Where | Esa's behaviour |
|---|---|---|
| Ctrl-O | F2 pattern editor | render the editor's pattern, import it as a sample |
| Shift-Ctrl-O | F2 | render the editor's pattern to the Quicksave folder only |
| Ctrl-O / Shift-Ctrl-O | F11 order list | as F2, for the playing pattern (stopped: the one at the order cursor) |
| Right / Shift-Right on an order cell's right-most digit | F11 | render + import / Quicksave only (elsewhere: IT's cursor keys) |
| Shift-G / Ctrl-G | F11 | render + import / Quicksave only (plain G unchanged) |
| Ctrl-Shift-Right | F11, F3, F5 | export every sample as a WAV |
| Shift-Right, Ctrl-O | F5 info page | render the playing pattern (stopped: the editor's) to Quicksave only |
| D | F5 | export every sample as a WAV |
| Ctrl-Alt-Right | F3 sample list | export the selected sample as a WAV (Esa: Ctrl-Right — IT's next order there, kept) |
| Shift-Alt-M | F2 | render the pattern once per non-empty, unmuted channel (stems) |
| "WAV" / "MWAV" buttons | F10 Save Module | whole song to one WAV / one WAV per unmuted channel |
| Alt-W / Shift-Alt-W | everywhere (F3 keeps its own Alt-W) | save the module to the Quicksave folder / make the current folder the Quicksave folder |
| F12 "Quicksave" row | F12 | the Quicksave folder; Enter opens a folder picker |
| command line | `ited` | render every pattern (or one), or export all samples, then quit |

## Clarifications

### Session 2026-10-18

- Q: Ctrl-O renders on which screens? → A: Exactly Esa's: F2, F11 and F5. On F3, F4 and F12 Ctrl-O keeps opening the system file dialog (feature 016); his fork binds nothing there.
- Q: How much of the Quicksave feature? → A: The F12 Quicksave row (render destination, folder picker on Enter, saved with the preferences) plus Alt-W (save module there without asking) and Shift-Alt-W (memorize the current folder).
- Q: File names? → A: Exactly Esa's 8.3 names (LL<HHMMSS>.WAV, <PFX><NNNN>.WAV, HHMMSSnn.WAV, SONG01.WAV …).
- Q: Headless batch render? → A: Yes, as `ited` command-line options.
- Q: Which sample rate and bit depth do renders use? → A: Renders imported into a sample slot stay IT-compatible: 44.1 kHz, 16-bit (Esa's format). WAV files written for other programs (Quicksave renders, stems, F10 WAV/MWAV, batch) have no limits in mixing: 32-bit float without clipping, at the mixing rate set in Shift-F5. Sample export writes the samples' own data unchanged.
- Q: Esa imports stereo renders with IT's stereo bit set and interleaved data, which IT 2.17's mixer plays at double speed (IT has no stereo samples; its loader converts stereo WAVs to mono via the Left/Right/Both dialog). → A: it26 imports mono (left + right mixed), keeping songs IT-compatible as the user asked.
- Q: F3 Ctrl-Right is IT 2.14's global "next order"; Esa's F3 export shadows it. → A: Keep IT's next order; the single-sample export moves to Ctrl-Alt-Right.
- Q: Esa's F5 Ctrl-O dumps the samples (his handler treats a held Ctrl as Ctrl-Shift-Right), while his comment says it renders "for the muscle memory from F2/F11". → A: it26 renders, as the comment intends; the README notes the difference.

### Session 2026-10-18 (follow-up: file names)

- Q: Esa's 8.3 names (`LL163422.WAV`, `13424803.WAV`, `TWI0001.WAV`) say nothing about the content. What scheme instead? → A: Long, descriptive names and one folder per multi-file export (user, 2026-10-18), replacing the earlier "exactly Esa's names" decision:
  - pattern → file: `<Song> - pattern NNN - YYYY-MM-DD HH.MM.SS.wav`
  - sample export: folder `<Song> - samples YYYY-MM-DD HH.MM.SS` with `NN <sample name>.wav` (`NN.wav` when the sample has no name); one sample: `<Song> - NN <sample name>.wav`
  - Shift-Alt-M stems: folder `<Song> - pattern NNN stems YYYY-MM-DD HH.MM.SS` with `chNN.wav`
  - F10 WAV: the typed name as typed (`.wav` added without an extension); F10 MWAV: `<typed base> - chNN.wav`
  - batch: folder `<Song> - patterns YYYY-MM-DD HH.MM.SS` with `pattern NNN.wav`; `--dump-samples` as the sample export
  - `<Song>` = the song name, else the module's file name without extension, else `untitled`; IT's CP437 characters become their Unicode equivalents; `\ / : * ? " < > |` and control characters become `_`; trailing dots/spaces trimmed; each name at most 80 bytes; an existing name gets ` (2)`, ` (3)` … — nothing is overwritten.
  - Unchanged: an imported sample's file-name field inside the module stays 8.3 (`LL<HHMMSS>.WAV`; the IT field is 12 bytes) — no file is written for imports.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Turn the pattern into a sample (Priority: P1)

A musician has a groove in the pattern editor. Ctrl-O renders the pattern
once, from its first to its last row, and the result appears as a new
sample — looped, named "Pattern 005", the cursor already on it — ready to be
chopped, pitched or played on another channel. If the song was playing, it
stops for the moment of the render and carries on where it was.

**Why this priority**: The core of Esa's rendering work and the gesture all
others are variations of.

**Independent Test**: Load a module, F2 on pattern 5, press Ctrl-O: a new
sample "Pattern 005" exists in the first slot free in both the sample and
instrument lists, its length equals the pattern's playing time at 44.1 kHz,
it loops over its whole length, and the status line reads
"Pat 005 -> LL<HHMMSS>.WAV -> sample NNN".

**Acceptance Scenarios**:

1. **Given** the pattern editor, **When** Ctrl-O is pressed, **Then** the editor's pattern is rendered once (its row count; 64 when a pattern header says 0; at most 200) and imported into slot max(highest sample, highest instrument) + 1.
2. **Given** the import, **Then** the sample is rendered at 44.1 kHz and stored 16-bit **mono** (a stereo mix is mixed down, left + right halved — IT's own "both" choice for stereo WAVs; IT samples cannot be stereo), C5 speed 44100, default and global volume 64, forward loop from 0 to its end, named "Pattern NNN" (3-digit pattern number), its file name field holding the WAV's name.
3. **Given** instrument mode, **Then** an instrument hosts the sample (as "Create host instrument" does: the same-numbered instrument if blank, else the first blank one) and the status line adds " in inst MMM"; the F3 and F4 cursors move to the new sample / instrument.
4. **Given** a render longer than 1 MB of sample data, **Then** the sample keeps the first 1 MB and the status line reads "Pattern truncated to 1MB; sample N".
5. **Given** no slot free (99 used), **Then** nothing is imported and the status line reads "Import failed: no free sample slot".
6. **Given** a song playing in song mode (or a pattern looping), **When** Ctrl-O is pressed, **Then** playback stops for the render and resumes afterwards at the same order and row (pattern mode: the same pattern and row); if nothing was playing, nothing starts.
7. **Given** F11, **When** Ctrl-O, Shift-G, or Right on the right-most digit of an order cell is pressed, **Then** the playing pattern (stopped: the pattern at the order cursor) is rendered and imported the same way; Right elsewhere and plain G keep IT's behaviour.
8. **Given** an import render, **Then** no WAV file accumulates in the Quicksave folder.

---

### User Story 2 - Bounce a pattern to the Quicksave folder (Priority: P1)

A musician working next to a DAW sets a Quicksave folder in F12 — a shared
folder the DAW machine sees. Shift-Ctrl-O writes the pattern as a time-stamped
WAV there, without touching the sample list, ready to be dragged into the DAW.

**Why this priority**: Esa's main workflow (tracker next to a DAW) and the
destination every other render and export uses.

**Independent Test**: Set the Quicksave folder in F12, press Shift-Ctrl-O in
F2 at 16:34:22: LL163422.WAV appears in that folder, plays the pattern once,
and the sample list is unchanged.

**Acceptance Scenarios**:

1. **Given** F2, **When** Shift-Ctrl-O is pressed, **Then** the editor's pattern is written to the render folder as `LL<HHMMSS>.WAV` (24-hour local time) and the status line reads "Saved pat NNN as LL<HHMMSS>.WAV in <folder>".
2. **Given** F11, **When** Shift-Ctrl-O, Ctrl-G, or Shift-Right on an order cell's right-most digit is pressed, **Then** the playing pattern (stopped: the order cursor's) is written the same way.
3. **Given** F5 (info page), **When** Shift-Right or Ctrl-O is pressed, **Then** the playing pattern (stopped: the editor's pattern) is written the same way; the channel selection does not move; plain Right still moves it.
4. **Given** the render folder rule, **Then** files go to the Quicksave folder when one is set, else the sample folder (F12 Sample directory), else the current folder.
5. **Given** a Quicksave folder that does not exist, **Then** nothing is rendered, playback is untouched, and the status line names the problem.
6. **Given** two renders in the same second, **Then** the second overwrites the first (Esa's accepted limit).
7. **Given** F12, **Then** a "Quicksave" row under Instrument holds the folder (64 characters, saved with Save all Preferences), and Enter on it opens a folder picker.

---

### User Story 3 - Export the samples as WAVs (Priority: P2)

Moving a module's sounds to another machine, a musician exports every sample
with one key, or just the selected one.

**Independent Test**: A song with 12 samples, F5, press D: twelve WAVs named
`HHMMSSnn.WAV` (nn = slot) appear in the render folder; the status line reads
"Dumped 12 samples as <HHMMSS>nn.WAV in <folder>"; the song keeps playing.

**Acceptance Scenarios**:

1. **Given** F5, **When** D or Ctrl-Shift-Right is pressed, **Then** every non-empty sample slot 1..99 is written as a WAV in the render folder named time-of-export (HHMMSS) + 2-digit slot, e.g. 16342203.WAV.
2. **Given** F11 or F3, **When** Ctrl-Shift-Right is pressed, **Then** the same export runs.
3. **Given** F3, **When** Ctrl-Alt-Right is pressed on a sample (Esa's Ctrl-Right; it26 keeps IT's Ctrl-Right = next order), **Then** only that sample is written ("Saving selected sample as C-4 44.1kHz WAV..." then "Saved C-4 44.1kHz WAV (sample N)"); its header states 44100 Hz, as in Esa's build; an empty slot gives "Selected sample is empty.".
4. **Given** an export, **Then** 8-bit samples are written as unsigned 8-bit, 16-bit as 16-bit, stereo as stereo; the song's samples are not modified; playback continues.
5. **Given** no samples, **Then** the status line reads "No samples to dump" and no file is written.

---

### User Story 4 - Stems: per channel, or the whole song (Priority: P2)

To take a tune into a DAW, a musician renders stems: each channel of the
pattern on its own (Shift-Alt-M), or the whole song as one WAV or as one WAV
per channel (F10's "WAV" / "MWAV").

**Independent Test**: A pattern using channels 1, 2 and 5, channel 2 muted:
Shift-Alt-M writes two files (channels 1 and 5), each containing only that
channel; the mutes are as before afterwards.

**Acceptance Scenarios**:

1. **Given** F2, **When** Shift-Alt-M is pressed, **Then** the pattern is rendered once for every channel that has at least one note (C-0..B-9; note cut, note off and empty don't count) and is not muted, with only that channel audible, each to `<PFX><NNNN>.WAV`, nothing imported; the mute state afterwards is as before.
2. **Given** a sweep in progress, **When** Esc is pressed, **Then** the sweep stops after the channel being rendered and the mutes are restored.
3. **Given** F10 Save Module, **Then** two more format buttons "WAV" and "MWAV" sit below the others (same style).
4. **Given** "WAV" and a typed name `song.wav`, **When** saved, **Then** the whole song (order 0 to the end of the song) is written as SONG.WAV in the current folder.
5. **Given** "MWAV" and `song.wav`, **Then** the whole song is written once per unmuted channel as SONG01.WAV, SONG02.WAV … (base cut to 6 characters, 2-digit channel number; ".WAV" added when no extension is typed).
6. **Given** a render running (sweep or whole song), **When** any render gesture is pressed again, **Then** the render in progress stops as with Esc and its file is finished cleanly; nothing hangs.

---

### User Story 5 - Quicksave the module (Priority: P3)

Saving often is quicker when there is no question to answer: Alt-W writes the
module into the Quicksave folder under its own name.

**Independent Test**: Quicksave folder set, song loaded from elsewhere as
MYSONG.IT: Alt-W writes MYSONG.IT into the Quicksave folder.

**Acceptance Scenarios**:

1. **Given** a Quicksave folder, **When** Alt-W is pressed (any screen but F3, where Alt-W stays "save as raw/WAV sample"), **Then** the module is saved there under its current file name and format, as Ctrl-S would.
2. **Given** no Quicksave folder, **When** Alt-W is pressed, **Then** the status line reads "Quick-save folder not configured." and the F10 Save Module screen opens.
3. **Given** any screen, **When** Shift-Alt-W is pressed, **Then** the current module folder becomes the Quicksave folder, it is saved with the preferences, and the status line reads "Quick-save folder memorized.".

---

### User Story 6 - Render from the command line (Priority: P3)

To bounce a module without the interface, a user runs `ited` with a render
option; the files are written and the program exits.

**Independent Test**: `ited --render-patterns song.it` writes one WAV per
pattern that holds data into the Quicksave folder (counter names) and exits
with status 0, without opening a window.

**Acceptance Scenarios**:

1. **Given** a module and the render-patterns option, **Then** every pattern with data is written as `<PFX><NNNN>.WAV`; empty patterns are skipped; the program exits.
2. **Given** a pattern number as well, **Then** only that pattern is written.
3. **Given** the dump-samples option, **Then** every sample is written as in User Story 3 and the program exits.
4. **Given** a module that cannot be loaded, **Then** nothing is written and the exit status is not 0.

---

### User Story 7 - Discoverable (Priority: P3)

F1 lists the new keys under "it26 additions" on the screens where they work;
the README describes them as extensions after Esa's fork.

### Edge Cases

- A pattern number of 200 or more (no valid pattern under the cursor) is refused; nothing happens.
- A pattern whose header claims 0 rows renders 64 rows; more than 200 renders 200.
- A render while the song is stopped does not start playback afterwards.
- A pattern that ends in note-off / long releases is cut at its last row (one pass, no tail), as in Esa's build.
- Quicksave folder names longer than the field (64 characters) cannot be entered.
- The import never reuses a slot that is in use as a sample or as an instrument.
- Sample export of a slot with a sample name containing characters a file name can't hold is unaffected — names come from time and slot only.
- Render keys pressed in the help screen or a dialog do nothing.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Pattern renders MUST play the chosen pattern once, rows 0..last (64 if the header says 0, at most 200), through the ported engine, faster than real time, stereo when the song's stereo flag is set and mono otherwise. Renders for import MUST mix at 44100 Hz and be stored as 16-bit samples. Renders written as WAV files MUST mix at the Shift-F5 mixing rate and be written as 32-bit float without clipping (peaks above full scale preserved).
- **FR-002**: A song or pattern that was playing MUST be stopped for a single-pattern render and resumed afterwards in the same play mode and position; a sweep or whole-song render MUST NOT resume it; nothing MUST start when playback was stopped.
- **FR-003**: Import renders MUST create the sample described in User Story 1 (slot rule, name, file name, loop, volumes, speed, 1 MB cap, host instrument in instrument mode, cursors, status text) and MUST NOT leave the WAV in the Quicksave folder.
- **FR-004**: Quicksave-only renders MUST write `LL<HHMMSS>.WAV` into the render folder (Quicksave → sample folder → current folder) and report "Saved pat NNN as <file> in <folder>"; an invalid Quicksave folder MUST abort before anything changes.
- **FR-005**: Files written by renders and exports MUST carry the descriptive names and folders of Clarifications "follow-up: file names" (song, pattern / slot / channel, date and time), cleaned for Windows and macOS, never overwriting an existing file. F10's typed names MUST be used as typed. (Supersedes Esa's 8.3 names, 2026-10-18.)
- **FR-006**: The key bindings in the Context table MUST be available on exactly those screens; every key IT 2.14 binds there MUST keep its behaviour (F11 Right/Shift-Right away from the right-most digit, plain G, F5 plain Right, F3 Alt-W, Ctrl-O on F3/F4/F12).
- **FR-007**: Sample export MUST write each non-empty slot (or the selected one) as a WAV named HHMMSS + 2-digit slot, converting 8-bit data to unsigned, keeping 16-bit and stereo, without modifying the song, while playback continues.
- **FR-008**: Shift-Alt-M MUST render the current pattern per non-empty, unmuted channel, solo, without import, restore the mutes, and stop after the current channel on Esc.
- **FR-009**: F10 MUST offer "WAV" and "MWAV" format buttons rendering the whole song (one file / one per unmuted channel) under the typed name.
- **FR-010**: A render gesture during a render MUST stop the render in progress as Esc does and finish its file.
- **FR-011**: F12 MUST have a Quicksave directory row, saved and loaded with the preferences, with a folder picker on Enter; Alt-W and Shift-Alt-W MUST behave as in User Story 5.
- **FR-012**: `ited` MUST accept options to render every pattern with data (optionally one pattern) and to export all samples, writing to the Quicksave folder (from the preferences) and exiting without a window.
- **FR-013**: The playback engine MUST NOT change; renders MUST go through the same engine and mixer as live playback, so a render of a pattern equals what the engine plays.
- **FR-014**: F1 help MUST list the new keys under the it26 additions; the README MUST describe them as extensions credited to Esa Ruoho, including the F5 Ctrl-O difference.

### Key Entities

- **Render job**: what to play (pattern n / whole song), which channel is solo (or none), destination (import / folder file), file name, resume state.
- **Quicksave folder**: a path in the preferences; the render folder resolution falls back to the sample folder, then the current folder.
- **Render counter**: per-session number for `<PFX><NNNN>` names, starting at 1.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Ctrl-O on a 64-row pattern turns it into a playable, looped sample in under 2 seconds on a typical desktop, and playback resumes at the row it was on.
- **SC-002**: A rendered pattern WAV is sample-identical to rendering the same pattern twice in a row (deterministic), and its length equals the pattern's playing time at the render rate.
- **SC-007**: A WAV render of a pattern that overloads the mix keeps samples above full scale (no value clamped to ±1.0).
- **SC-003**: All file names produced in a test session match Esa's naming rules for each gesture (LL<HHMMSS>, <PFX><NNNN>, HHMMSSnn, SONGnn).
- **SC-004**: Every key IT 2.14 binds on F2, F3, F5, F10, F11 and F12 still does what it did (comparison harness unchanged for the existing scripts; selftest green).
- **SC-005**: Exporting all samples of a playing song produces correct WAVs (8-bit unsigned, 16-bit, stereo) without an audible interruption.
- **SC-006**: A command-line render of a module writes one WAV per pattern with data and exits with status 0 without a display.

## Assumptions

- "Exactly as Esa" means his user-visible behaviour, keys, names and texts; DOS mechanics he needed (driver swapping, EMS bounce buffers, a local C:\TEMP stage for imports, on-screen diagnostic markers, CTRLOLOG.TXT / PATLOG.TXT logs) are not reproduced.
- Esa's whole-song render runs in real time only because of his DOS driver model; it26 renders whole songs faster than real time as well. The result is the same file.
- Esa's MIDI-clock resume trigger is part of his MIDI sync feature, which is out of scope; the standalone resume after a single-pattern render is in scope.
- The F11 order-list tools (clone, extend, mute-wipe toggle, PATLOG.TXT) and the F5 Enter jump are separate features, not part of rendering.
- Imports follow Esa's WAV driver (44100 Hz, 16-bit, stereo by song flag). WAV files deliberately go beyond Esa's 16-bit: 32-bit float without clipping at the Shift-F5 mixing rate (user decision). Shift-F5's output format and SB16 filter/feedback options do not apply to renders.
- The whole-song end is IT's own end-of-song (order list end or the song looping back), as `itplay -w` uses.

## Later changes

- Feature 019 (stereo samples): imports from a stereo song are stereo samples, and the import cap is the editor's 4 MB sample size limit instead of 1 MB; sample exports write 2-channel WAVs for stereo samples. See `specs/019-stereo-samples/`.
