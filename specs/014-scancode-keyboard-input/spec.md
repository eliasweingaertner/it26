# Feature Specification: Scancode-Based Keyboard Input with Layout-Aware Text Entry

**Feature Branch**: `014-scancode-keyboard-input`

**Created**: 2026-08-20

**Status**: Draft

**Input**: User description: "Scancode-based keyboard input with layout-aware text entry — the port dispatches note entry and command keys on translated characters while Impulse Tracker dispatches on physical scancodes, so on a German QWERTZ keyboard the note rows are cross-wired and umlauts cannot be typed."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Note entry lands on the same physical keys on every layout (Priority: P1)

A musician using a non-US keyboard opens the pattern editor and plays a chromatic
run across the tracker's two note rows. The notes that sound are determined by
*where the keys are on the keyboard*, not by which letters are printed on them.
The sweep across the bottom two rows and the sweep across the top two rows each
produce a continuous chromatic scale, exactly as they do in the original tracker
under DOSBox and exactly as they do for a US-keyboard user.

**Why this priority**: This is the reported defect and the reason the editor is
currently unusable for composing on a German keyboard. It is also the behaviour
the original hard-codes; every other story in this feature is a refinement on top
of it. Shipping only this story already restores parity for note entry, which is
the single most-used interaction in the program.

**Independent Test**: Play the lower-octave sweep and the upper-octave sweep on a
German QWERTZ keyboard and compare the resulting notes against the same physical
keys on a US keyboard and against real IT 2.14 under DOSBox. All three must agree.

**Acceptance Scenarios**:

1. **Given** a German QWERTZ keyboard and the pattern editor in edit mode, **When** the user presses the physical keys of the lower note row from left to right (the keys printed `y s x d c v g b h n j m`), **Then** the entered notes are the ascending chromatic sequence C, C#, D, D#, E, F, F#, G, G#, A, A#, B of the base octave — identical to what the same physical keys produce on a US keyboard.
2. **Given** a German QWERTZ keyboard, **When** the user presses the physical keys of the upper note row (the keys printed `q 2 w 3 e r 5 t 6 z 7 u i 9 o 0 p`), **Then** the entered notes are the ascending chromatic sequence starting one octave above the base octave, identical to the US-keyboard result.
3. **Given** any supported keyboard layout, **When** the user presses a key that carries a non-note binding in the pattern editor, **Then** the command bound to that *physical position* in the original fires.
4. **Given** the base octave is changed, **When** the note rows are played, **Then** the octave offset applies uniformly and note range clamping behaves as before.

---

### User Story 2 - National characters can be typed into names and the message (Priority: P2)

A German user names a sample "Röhrenbass", titles the song "Größenwahn", and
writes a message containing umlauts. The characters appear on screen in the
tracker's own font, are stored in the module file, and come back unchanged when
the module is reloaded.

**Why this priority**: Text entry is how the user labels their work, and the
inability to type their own language's characters is visible on every save. It is
lower priority than P1 only because it does not block composing.

**Independent Test**: Type each German special character into a sample name, a
song title, and the message editor, save the module, reload it, and confirm the
characters survive unchanged.

**Acceptance Scenarios**:

1. **Given** a German keyboard, **When** the user types `ä`, `ö`, `ü`, `ß`, `Ä`, `Ö`, `Ü` into a sample name field, **Then** each character is accepted and rendered with its correct glyph.
2. **Given** a name containing national characters, **When** the module is saved and reloaded, **Then** the name is byte-identical to what was typed.
3. **Given** a keyboard layout that can produce a character the tracker's character set cannot represent, **When** the user types that character, **Then** the keystroke is rejected without corrupting the field and without altering the cursor position.
4. **Given** the message editor, **When** national characters are typed, **Then** they render through the same character set used for the rest of the message and survive a save/reload round trip.

---

### User Story 3 - Alt and Ctrl shortcuts follow the printed keycap (Priority: P3)

A user who has learned the tracker's shortcuts from the manual presses Alt plus
the letter named in the documentation. The command that fires is the documented
one, regardless of where that letter sits on their physical keyboard.

**Why this priority**: Without it, fixing P1 would move the letter-named shortcuts
to surprising physical positions on non-US layouts. The original resolves this
explicitly, so matching it is required for parity — but it only matters once
positional note entry exists.

**Independent Test**: On a German keyboard, exercise every documented Alt-letter
and Ctrl-letter shortcut and confirm each fires the command the manual names.

**Acceptance Scenarios**:

1. **Given** a German QWERTZ keyboard, **When** the user presses Alt plus the key printed `Z`, **Then** the command documented as Alt-Z fires (not the one bound to the physical position that carries `Z` on a US keyboard).
2. **Given** a German QWERTZ keyboard, **When** the user presses Alt plus the key printed `Y`, **Then** the command documented as Alt-Y fires.
3. **Given** any layout, **When** a Ctrl-letter shortcut is pressed, **Then** it resolves by printed letter using the same rule as the Alt shortcuts.
4. **Given** a layout where a documented shortcut letter is only reachable with an extra modifier, **Then** the shortcut remains reachable and the behaviour is documented.

---

### User Story 4 - A layout can be supplied explicitly when the host cannot (Priority: P4)

A user whose environment does not report a usable keyboard layout — or who wants
to reproduce a specific original layout exactly — supplies a layout definition and
selects it in the configuration. The tracker then translates characters using that
definition instead of the host's.

**Why this priority**: The host layout covers the overwhelming majority of users
with no configuration at all. This is the escape hatch for the remainder and for
exact original-parity testing, so it is valuable but not on the critical path.

**Independent Test**: Select a supplied layout definition in the configuration,
restart, and confirm that character output follows the definition rather than the
host layout, while note entry positions are unchanged.

**Acceptance Scenarios**:

1. **Given** a valid layout definition file and a configuration entry selecting it, **When** the tracker starts, **Then** typed characters follow the file and a confirmation of the loaded layout is visible to the user.
2. **Given** a layout definition file selected in the configuration, **When** note keys are pressed, **Then** note entry positions are unaffected by the file.
3. **Given** a missing, unreadable, or malformed layout definition file, **When** the tracker starts, **Then** it reports the problem and continues with the host layout rather than failing to start.
4. **Given** no layout definition is configured, **When** the tracker starts, **Then** the host layout is used and no configuration is required.

---

### User Story 5 - The user can see what the tracker receives from a key (Priority: P5)

A user whose keyboard behaves unexpectedly opens a diagnostic view, presses keys,
and sees the position code and translated character the tracker received for each.

**Why this priority**: Purely a support and self-service diagnosis aid. The
original provides it and it is what makes writing a layout definition (P4)
practical, but nothing depends on it.

**Independent Test**: Open the diagnostic view, press several keys including ones
whose printed letter differs from the US position, and confirm the displayed
values match expectations.

**Acceptance Scenarios**:

1. **Given** the diagnostic view is open, **When** any key is pressed, **Then** its position code and the character it produced are displayed.
2. **Given** the diagnostic view is open, **When** modifier keys are held, **Then** the modifier state is displayed alongside the key.

---

### Edge Cases

- What happens when a key produces a character outside the tracker's character set (for example Polish `ł` or a Cyrillic letter)? The keystroke must be rejected cleanly in text fields; it must not corrupt the field, move the cursor, or be silently substituted with a similar-looking character.
- What happens when a single key produces multiple characters (dead keys and composed sequences, e.g. `´` + `e` = `é`)? The composed result must reach the text field as one character, and the dead key alone must not enter anything.
- What happens on layouts where a note-row key requires AltGr to produce its printed character? Note entry must be unaffected, since it does not consult the character at all.
- What happens when the user switches keyboard layout while the tracker is running? Character translation must follow the new layout without a restart, or the limitation must be documented.
- What happens on the terminal backend, which receives characters only and cannot report physical key positions? Note entry must fall back to inferring position from the character using a configured layout, defaulting to the layout the original assumed; the resulting limitations must be stated plainly to the user.
- What happens when Caps Lock or Num Lock is engaged? Note entry must be unaffected by Caps Lock; text entry must follow the host layout's own casing rules.
- What happens to the keypad keys, which have distinct positions but overlap the main rows in the characters they produce? They must keep their existing distinct bindings.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Every key press delivered to the editor MUST carry three pieces of information: the physical key position (in the original's numbering), the character that position produces under the active layout, and the state of the modifier keys.
- **FR-002**: Note entry MUST be resolved from the physical key position alone, using the original's note table, and MUST NOT consult the translated character.
- **FR-003**: Every other binding that the original resolves by key position MUST likewise resolve by physical position.
- **FR-004**: Text entry into names, titles, filename requesters, and the message editor MUST use the translated character.
- **FR-005**: Characters representable in the tracker's own character set MUST be accepted in text fields and rendered with their correct glyph; characters not representable MUST be rejected without side effects.
- **FR-006**: Text typed into any field MUST survive a save-and-reload round trip unchanged.
- **FR-007**: Alt-letter and Ctrl-letter shortcuts MUST resolve by the letter printed on the key, matching the original's own remapping of those combinations on non-US layouts.
- **FR-008**: The character translation MUST come from the host keyboard layout by default, requiring no configuration from the user for any layout the host supports.
- **FR-009**: Users MUST be able to override the character translation with a supplied layout definition selected through the configuration file; the definition MUST use the original's documented layout format so that original layout files work unchanged.
- **FR-010**: A missing, unreadable, or malformed layout definition MUST be reported and MUST fall back to the host layout rather than preventing startup.
- **FR-011**: On backends that cannot report physical key positions, the system MUST infer position from the character using a configured layout, defaulting to the layout the original assumed, and MUST document which behaviours are unavailable there.
- **FR-012**: Extended keys already handled as distinct codes (function keys, cursor keys, editing keys, and the existing modifier combinations) MUST keep their current behaviour and identity.
- **FR-013**: A diagnostic view MUST let the user see the position code, translated character, and modifier state for any key pressed.
- **FR-014**: The self-test MUST verify that a simulated German layout and a simulated US layout produce identical notes from identical physical key positions while producing different characters from those same positions.
- **FR-015**: The self-test MUST verify that national characters entered into a name survive a save-and-reload round trip.
- **FR-016**: All existing correctness gates — the determinism regression across the four reference modules, the pattern round-trip idempotency check, and every existing self-test block — MUST continue to pass unchanged.

### Key Entities

- **Key event**: One key press as seen by the editor. Carries the physical position code, the character produced under the active layout (or none), and modifier state. Replaces today's single combined value.
- **Layout definition**: A mapping from physical key positions to the characters they produce under each modifier condition. Supplied by the host by default; optionally supplied as a file in the original's format.
- **Note position table**: The original's fixed mapping from physical key positions to semitone offsets. Independent of any layout definition.
- **Character set mapping**: The correspondence between characters the host layout can produce and the codes the tracker stores in module files and renders with its own font.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: On a German QWERTZ keyboard, all 29 note-row keys produce the same notes as the same physical keys on a US keyboard and as real IT 2.14 under DOSBox — 29 of 29 matching, with no manual configuration.
- **SC-002**: All seven German special letters (`ä ö ü ß Ä Ö Ü`) can be typed into song titles, sample names, instrument names, and the message editor, and 100% of them survive a save-and-reload round trip byte-identically.
- **SC-003**: 100% of documented Alt-letter and Ctrl-letter shortcuts fire the documented command on a German keyboard.
- **SC-004**: A user changing keyboard layout performs zero configuration steps for any layout the host system supports.
- **SC-005**: The determinism regression across the four reference modules and the pattern round-trip check produce results identical to the pre-change baseline.
- **SC-006**: Every pre-existing self-test block continues to pass, and the new keyboard block passes on both the Windows and Linux verification hosts.
- **SC-007**: A user can identify what the tracker received from any key within one minute, without external tools.

## Assumptions

- The physical key positions used are the original's own numbering, so the ported note table and every other position-indexed table transfer unchanged.
- The tracker's stored character set remains the original's, unchanged by this feature; only the path by which characters reach it changes. Characters outside it are rejected rather than transliterated, because the module file format has no way to represent them.
- The host keyboard layout is the default translation source on the graphical backends. An explicitly configured layout definition always takes precedence over the host layout when one is configured.
- The terminal backend cannot obtain physical key positions from a standard terminal, so position is inferred from the character there; the assumed default is the layout the original shipped as its default. This is a stated limitation of that backend, not a defect.
- Glyph rendering for national characters relies on the high-range character support already present in the editor's font handling; no new font work is assumed.
- Layout definitions supplied as files use the original's documented format, so the layout files shipped with the original are usable as-is.
- Verification of the German-layout behaviour is done both by the automated self-test (with simulated layouts, so it runs on any host) and by manual comparison against the original under DOSBox on the user's own German keyboard.
- MIDI input handling is out of scope; this feature concerns the computer keyboard only.
