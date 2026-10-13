# Feature Specification: Pattern Editor Mouse Selection, Context Menu and Scale-Aware Note Interpolation

**Feature Branch**: `017-pattern-editor-mouse-menu` (worked on master)

**Created**: 2026-10-13

**Status**: Implemented

**Input**: User description: "Mouse-select range in the pattern editor is not supported by IT 2.14 -- we add this as a mouse extension. Right click opens a context menu: Copy/Paste, Transpose, Interpolate. No 'Pattern' heading. Also add Clear, Interpolate Notes and Transpose N (with an additional input modal). Interpolate Notes must take the scale into account. We also need a hotkey for interpolating notes. Key detection must not be misled by note-triggered drums -- detect on the non-muted tracks."

## Context

Impulse Tracker 2.14 has no mouse handling in the pattern editor at all
(verified with the compare harness, round 3 of 2026-10-12): a click or a
drag there does nothing. Blocks are marked with Alt-B/Alt-E or Shift+cursor,
and block operations live on Alt keys that a newcomer has to know.

This feature adds a small, deliberate set of **extensions** next to the
original keys. Nothing that IT 2.14 does changes; every new action is
reachable only through input that IT 2.14 ignores (a mouse drag or right
click in the pattern area, Ctrl-K).

| Input | Where | Action |
|---|---|---|
| Left click | pattern area | cursor to the clicked cell (and anchor for a drag) |
| Left drag | pattern area | mark a block from the anchor to the pointer |
| Right click | F2 screen | block context menu at the pointer |
| Ctrl-K | pattern editor | Interpolate notes (scale-aware) on the marked block |

## Clarifications

### Session 2026-10-12

- Q: Is mouse range selection something IT 2.14 does? → A: No. It is added as an it26 mouse extension and documented as such.
- Q: Should the menu have a title? → A: No "Pattern" heading; the items start at the top of the box.
- Q: What should "Interpolate notes" produce between two notes? → A: A run in the chosen scale, not a chromatic or linear pitch slide. The top and bottom notes stay as written; the notes between are the scale notes from one to the other. A dialog chooses Scale, Root and Step; the key is pre-filled from detection.
- Q: Where do the notes go? → A: (revised 2026-10-13, replacing three "rhythm" modes) On the edit step, IT's own skip value (Alt-0..9): a note every Step rows from the block's top, plus the last row for the end note. The number of rows decides the number of notes; the scale notes between the endpoints are fitted onto them -- repeated when there are more rows than scale notes, skipped when there are fewer. The dialog's Step starts at the edit step and can be changed there.
- Q: Which hotkey? → A: Ctrl-K (unused in IT 2.14's pattern editor key table; Alt-K is IT's volume interpolation).

### Session 2026-10-13

- Q: Note-triggered drums would mislead a key detection over the whole song. How is that avoided? → A: Muted channels never count (so muting the drum tracks removes them). Within the unmuted channels the key comes from the block's own instrument (sample in sample mode) across all patterns in the order list. If that instrument has too few notes, every instrument except drum-like ones (80 % or more of its notes on one pitch class) counts. If no clear key results, the root is the block's top note. The dialog shows where the key came from.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Mark a block with the mouse (Priority: P1)

A user drags across the pattern with the left button held. The block
follows the pointer, and the cursor moves to where the drag ends. A plain
click moves the cursor without marking anything.

**Independent Test**: Drag from channel 1 row 0 to channel 4 row 16; Alt-C
copies exactly that block.

**Acceptance Scenarios**:

1. **Given** the pattern editor, **When** the user presses at a cell and releases at another, **Then** the block spans both cells (any drag direction), as Alt-B/Alt-E would mark it.
2. **Given** a marked block, **When** the user just clicks a cell, **Then** the cursor moves there and the block is left as it was.

---

### User Story 2 - Block operations from a right-click menu (Priority: P1)

A user right-clicks in the pattern and gets a menu at the pointer: Copy,
Paste, Clear, Transpose up, Transpose down, Transpose by..., Interpolate
notes, Interpolate volume, Interpolate effect. Items that have an IT key show
it ("(Alt-C)"), so the menu also teaches the keyboard.

**Independent Test**: Mark a block, right-click inside it, choose
"Transpose up"; the notes rise one semitone exactly as with Alt-Q.

**Acceptance Scenarios**:

1. **Given** a marked block, **When** the user right-clicks inside it, **Then** the menu opens at the pointer (moved inward at the screen edges) and the block stays marked.
2. **Given** a right click outside the block, **When** the menu opens, **Then** the cursor first moves to the clicked cell, so Paste lands there.
3. **Given** the menu, **When** the user picks Copy, Paste, Transpose up/down, Interpolate volume or Interpolate effect, **Then** the result equals Alt-C, Alt-P, Alt-Q, Alt-A, Alt-K, Alt-X respectively.
4. **Given** the menu, **When** the user picks Clear, **Then** every cell of the block is emptied, the clipboard is untouched, and one undo step "Undo block clear (Menu)" restores it.
5. **Given** the menu, **When** the user picks "Transpose by...", **Then** a small dialog asks for a signed number of semitones (-119..119); OK shifts every note in the block, clamped to C-0..B-9, as one undo step; Cancel, Esc or an empty/zero entry changes nothing.
6. **Given** the menu, **When** the user presses Esc or clicks outside it, **Then** it closes and nothing changes.

---

### User Story 3 - Interpolate notes in a scale (Priority: P1)

A user sets the edit step to 5 (Alt-5), writes C-5 on row 0 and G-5 on
row 20, marks rows 0..20 and presses Ctrl-K (or picks "Interpolate
notes"). A dialog shows Scale, Root and Step ("5 rows (5 notes)"),
pre-filled with the detected key and the edit step. With C Major the block
becomes C D E F G on every fifth row.

**Independent Test**: The scenario above gives C-5, D-5, E-5, F-5, G-5 on
rows 0, 5, 10, 15, 20 with the rows between empty.

**Acceptance Scenarios**:

1. **Given** a block whose first and last row hold a note in a channel, **When** the user confirms the dialog, **Then** that channel gets the run: the top note, the in-scale notes between, the bottom note (rising or falling).
2. **Given** Step s, **Then** notes sit on rows 0, s, 2s, ... of the block and on its last row; the dialog shows how many ("4 rows (6 notes)"). Step starts at the edit step (0 counts as 1, capped at the block height) and `<` `>` change it within 1..block height.
3. **Given** more note rows than scale notes, **Then** notes repeat, spread evenly (C D E E F G for 6 rows); **given** fewer, scale notes are skipped evenly.
4. **Given** Step 1, **Then** every row gets a note.
5. **Given** any step, **Then** the endpoints stay exactly as written, rows between that receive no note lose their note and instrument (volume and effect columns are kept), and new notes without an instrument take the top note's instrument.
6. **Given** a channel without a note on the block's first or last row, **Then** that channel is skipped; if no channel qualifies the status line says so and nothing changes.
7. **Given** the dialog, **When** the user cancels (Esc, Cancel), **Then** nothing changes. One undo step "Undo note interpolation (Menu)" reverts a confirmed run.
8. The scales offered are Chromatic, Major, Natural minor, Harmonic minor, Melodic minor, Dorian, Mixolydian, Pentatonic major, Pentatonic minor, Blues; Root is C..B. The last Scale is remembered for the session; Step follows the edit step each time the dialog opens.

---

### User Story 4 - Key detection that drums do not mislead (Priority: P2)

A song uses notes on a drum sample to pick drum sounds. When the user
interpolates a melody, the suggested key comes from the melody, not from the
drum notes.

**Independent Test**: A C-major melody on sample 01 and 24 F#'s on a drum
sample 02: the dialog suggests a key from sample 01 and shows "(smp 01)".
With the melody's channel muted it falls back to "(top note)".

**Acceptance Scenarios**:

1. **Given** muted channels (Alt-F9), **Then** their notes never count.
2. **Given** the block's top note's instrument (the last instrument above it in that channel, else the current instrument) has at least 8 notes over the order list's patterns (the current pattern taken from the editor, including unsaved edits), **Then** the key comes from that instrument alone and the Root shows "(ins NN)" or "(smp NN)".
3. **Given** too few notes for it, **Then** all instruments count except drum-like ones (4 or more notes, 80 % or more on one pitch class) and the Root shows "(no drums)".
4. **Given** no clear key either way (fewer than 8 notes or the best key correlation below 0.5), **Then** the root is the top note's pitch class, the scale stays as last chosen, and the Root shows "(top note)".
5. **Given** a detected key, **Then** a Major / Natural minor scale choice follows the detected mode; other scale choices are kept. Changing the root by hand removes the source note.

---

### User Story 5 - Discoverable in F1 help (Priority: P3)

F1 in the pattern editor lists the additions under "it26 additions (not in
Impulse Tracker).": Ctrl-K, right click, mouse drag, on every backend
(also where no native file dialogs exist).

### Edge Cases

- A right click anywhere on the F2 screen opens the menu; only a click on a pattern cell moves the cursor.
- The menu is clamped so it never leaves the 80x50 screen.
- A block of fewer than 3 rows: Interpolate notes does nothing (no row between).
- Two equal endpoint notes: that channel is skipped (there is nothing to interpolate).
- Interpolate notes over several channels uses one scale for all and one undo step.
- Transpose by... with a result beyond C-0/B-9 clamps per note instead of refusing.
- Terminal backend: right click arrives via SGR mouse reports (button 2 press).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: A left press in the pattern area MUST move the cursor to the cell under the pointer; holding and moving MUST mark the block between the press cell and the pointer cell.
- **FR-002**: A right click on the pattern editor screen MUST open the block context menu at the pointer; a right click outside the marked block MUST first move the cursor there.
- **FR-003**: The menu MUST contain, in order: Copy (Alt-C), Paste (Alt-P), Clear, Transpose up (Alt-Q), Transpose down (Alt-A), Transpose by..., Interpolate notes (Ctrl-K), Interpolate volume (Alt-K), Interpolate effect (Alt-X); no heading.
- **FR-004**: Menu items that mirror an IT key MUST call the same code as that key.
- **FR-005**: Clear, Transpose by... and Interpolate notes MUST each be one undo step, named in the undo list (Ctrl-Backspace).
- **FR-006**: Ctrl-K in the pattern editor MUST run Interpolate notes on the marked block.
- **FR-007**: Interpolate notes MUST keep the endpoints and fill only with notes of the chosen scale and root, on every Step rows plus the last row (US3).
- **FR-008**: Key detection MUST ignore muted channels and prefer the top note's instrument, with the drum-like and top-note fallbacks of US4, and MUST show its source in the dialog.
- **FR-009**: All backends (Win32, SDL, terminal, remote) MUST deliver a right click as `ITK_MOUSE_RIGHT`; the remote protocol's `mouse x y b` uses b bit 1 for a right click.
- **FR-010**: F1 in the pattern editor MUST list Ctrl-K, right click and mouse drag under the it26 additions heading.
- **FR-011**: Nothing IT 2.14 does with keys or the mouse may change; README lists the additions as fidelity notes.

### Key Entities

- **Block**: BlockTop/Bottom/Left/Right as set by Alt-B/Alt-E; the drag writes the same fields.
- **Scale**: name + 12-bit pitch-class mask relative to the root.
- **Key source**: instrument / sample number, "no drums" or "top note".

## Success Criteria *(mandatory)*

- **SC-001**: C-5 on row 0, G-5 on row 20, C Major, step 5 → C D E F G on rows 0/5/10/15/20 (verified, ctx_test5).
- **SC-002**: The same with step 4 → C D E E F G on rows 0/4/8/12/16/20; step 2 → C C D D E E E F F G G (verified, ctx_test5).
- **SC-003**: Melody on sample 01 + F# drums on sample 02 → source "(smp 01)"; melody channel muted → "(top note)" (verified, ctx_test4).
- **SC-004**: The editor selftest stays green (20/20); its DLG block checks the F1 help additions with and without dialogs.
- **SC-005**: Compare harness mouse scripts against IT 2.14 show no new differences outside the pattern area.

## Assumptions

- Krumhansl-Schmuckler profiles (major / minor) are good enough for a suggestion; the user can always override Scale and Root.
- Only Major and Natural minor are detected; modes and pentatonic scales are picked by hand.
- The 80 % single-pitch-class rule identifies note-triggered drums; a bass that plays mostly one note may be skipped in the fallback, which only matters when the block's own instrument has too few notes.
