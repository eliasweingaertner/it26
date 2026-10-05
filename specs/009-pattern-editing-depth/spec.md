# Feature Specification: Pattern Editing Depth

**Feature Branch**: `009-pattern-editing-depth`

**Created**: 2026-07-05

**Status**: Draft

**Input**: User description: "Pattern editing depth (roadmap #5): port the remaining
IT 2.17 pattern editor (F2) behaviour from the IT_PE.ASM key lists and PE_TRANS.INC
into it26. Scope: (1) block marking and block operations; (2) edit mask,
multichannel entry toggle, template mode; (3) row/field operations per the original
incl. undo; (4) navigation depth; (5) conveniences (pattern store/restore, play mark,
in-F2 mute/solo, hilight/division/centralise/trace/tracking toggles, pattern length,
default-/fast-volume toggles, view schemes); (6) key-layer prerequisites (Ctrl-arrow
codes, keyjazz-table fidelity per KeyBoardTable); (7) numeric entry on thumbbars.
Out of scope: MIDI input triggers; the already-ported < > ; ' instrument cycling.
Behaviour MUST match the original ASM handlers 1:1; pattern mutations stay serialised
via the engine lock and round-trip through the exact pack/unpack codec; determinism
gates stay green."

**Source of truth**: The original pattern-editor key table and handlers in
`impulse-tracker-jthlim/IT_PE.ASM` (the `PEFunction_*` family, key list at
380..860) with the block/transform semantics in `PE_TRANS.INC`, the keyjazz
`KeyBoardTable` in `IT_DISK.ASM` (430..436), and the thumbbar digit handling
in `IT_F.ASM`. Behaviour MUST match IT's own, quirks included.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Mark a block and operate on it (Priority: P1)

A user composing in F2 marks a region (Shift+arrows, or Alt-B/E for
begin/end, Alt-D for a step of rows, Alt-L for the track/pattern), sees it
highlighted, and applies the original operations: copy, paste, overwrite,
mix, wipe, swap, double/halve row spacing, volume scaling/amplify, wipe
commands/excess volumes, and semitone transpose up/down — exactly as IT
2.17 does, including its edge-case quirks.

**Why this priority**: Block operations are the single biggest everyday
composing workflow missing from the port; nothing else in this feature is
useful without marking.

**Independent Test**: Mark regions of a testdata module's pattern by every
marking method, apply each operation, and compare the resulting pattern
cells against the same operations performed in the DOS original (or its
documented semantics from the ASM); audio-render the modified module twice
to confirm the edits are deterministic and pack/unpack-clean.

**Acceptance Scenarios**:

1. **Given** a pattern with content, **When** the user marks with
   Shift+arrows or Alt-B/E/D/L, **Then** the marked region is highlighted
   with the original's attributes and bounds semantics (marks clamp to the
   pattern; Alt-L widens track → pattern on repeat).
2. **Given** a marked block, **When** the user copies and pastes it
   elsewhere (paste, overwrite, or mix variants), **Then** cell contents
   land per the original's field rules, and the clipboard survives moving
   to another pattern.
3. **Given** a marked block, **When** transpose/volume/wipe operations are
   applied, **Then** notes, volumes and commands change exactly per the
   `PE_TRANS.INC` semantics (clamping at note/volume bounds as IT does).
4. **Given** any block operation, **When** it runs during playback,
   **Then** the mutation is serialised against the audio thread and the
   player never reads a half-written pattern.
5. **Given** Alt-U, **When** pressed, **Then** the mark is removed.

---

### User Story 2 - Control what note entry writes (edit mask, multichannel, template) (Priority: P2)

A user tunes the edit behaviour: the `,` mask editor chooses which fields
(instrument, volume, effect) piano entry writes; the multichannel toggle
enters notes across channels; template mode stamps the copied block's
shape when entering notes — all per the original.

**Why this priority**: These change the meaning of every subsequent
keystroke and are the second most-used depth feature, but plain note entry
(already ported) works without them.

**Independent Test**: Set each mask combination and enter notes, verifying
written/preserved fields; toggle multichannel and confirm entry advances
across the configured channels; copy a block, enable template mode, enter
a note and confirm the template stamps per the original.

**Acceptance Scenarios**:

1. **Given** a custom edit mask, **When** a note key is pressed, **Then**
   only the mask-enabled fields are written and the others keep their
   previous cell values, as the original's mask logic does.
2. **Given** multichannel mode, **When** notes are entered, **Then**
   channel advancement follows the original's multichannel rules.
3. **Given** a copied block and template mode on, **When** a note key is
   pressed, **Then** the block is stamped transposed to the entered note
   per the original template semantics; template-off restores plain entry.

---

### User Story 3 - Row/field editing parity and undo (Priority: P2)

A user gets the original's editing verbs: plain Ins/Del insert/delete a
row on the current track only, Alt-Ins/Del act across all channels,
Ctrl-Ins/Del roll the whole pattern down/up, Backspace steps back one row,
and Ctrl-Backspace opens the original's undo requester — a 10-slot typed
history of pattern snapshots (one per destructive operation) from which
the user picks the state to revert to; reverting offers a redo entry.
*(Corrected from the draft's "single-step undo" after decoding the ASM —
see research.md R0/R8.)*

**Why this priority**: Same tier as US2 — daily editing verbs; the port's
current simplified Ins/Del semantics actively differ from the original,
which is a fidelity defect.

**Independent Test**: Apply each verb on a known pattern and diff the
resulting cells against the original's documented behaviour; verify
Ctrl-Backspace restores the pre-edit pattern after each destructive
operation that the original snapshots.

**Acceptance Scenarios**:

1. **Given** the cursor mid-pattern, **When** Ins/Del is pressed, **Then**
   only the current track shifts, with the original's fill/discard rules;
   Alt variants shift all channels; Ctrl variants roll the pattern with
   wraparound as the original does.
2. **Given** an edit was just made, **When** Ctrl-Backspace is pressed,
   **Then** the pattern returns to its state before the change, matching
   the set of operations the original protects with its undo buffer.
3. **Given** Backspace, **When** pressed, **Then** the cursor moves up one
   row without editing.

---

### User Story 4 - Navigation depth and editor conveniences (Priority: P3)

A user moves like in the original — Alt/Ctrl-Home/End, Ctrl/Shift-PgUp/
PgDn, Alt-arrows for channel/view movement, Ctrl-Left/Right view scroll,
Shift +/- for ±4 patterns, Ctrl +/- to follow the order list — and uses
the F2-local conveniences: pattern store/restore scratch, play mark,
mute/solo-next keys, row-hilight/division toggles, centralise/trace/
tracking view toggles, pattern-length setting, default-volume and
fast-volume toggles, and the Ctrl-0..5 / Ctrl-Shift-1..4 view schemes.

**Why this priority**: Quality-of-life parity; each item is small and
independent, and none blocks the editing verbs above.

**Independent Test**: Exercise each key on a testdata module and verify
cursor/view/pattern state against the original's handler semantics;
screenshot-compare view-scheme layouts against the original screens.

**Acceptance Scenarios**:

1. **Given** any navigation key from the original F2 table, **When**
   pressed, **Then** cursor, channel, view window and pattern selection
   move exactly as the original handler moves them.
2. **Given** the convenience toggles, **When** used, **Then** the visible
   state (hilight rows, division lines, centralised cursor, trace/track
   view, pattern length, volume entry behaviour) matches the original's.
3. **Given** a stored pattern scratch, **When** restore is invoked,
   **Then** the pattern returns to the stored snapshot per the original.

---

### User Story 5 - Faithful key plumbing (Priority: P3, prerequisite for parts of US1/US4)

The key layer carries the modifier combinations the original F2 table
needs (Ctrl-arrows and any other missing combinations) on the Win32, SDL2
and terminal backends where technically possible, and the piano map is
reduced to the original `KeyBoardTable` (Z-row 12 + Q-row 17 notes) so
`,` `.` `;` `l` `/` are free for their original bindings. Thumbbars accept
typed digits for direct numeric entry, as the original widgets do.

**Why this priority**: Invisible plumbing, but several US1/US4 bindings
cannot exist without it; the keyjazz reduction is a fidelity fix that
makes the mask key (`,`) possible at all.

**Independent Test**: Key-event unit pass per backend (posted events on
Win32, scripted on SDL2), confirming each new combination arrives as a
distinct code; verify `,`/`.`/`;`/`l`/`/` no longer enter notes anywhere;
type digits on a thumbbar and confirm direct value entry with the
original's commit/cancel behaviour.

**Acceptance Scenarios**:

1. **Given** the pixel backends, **When** Ctrl-arrow (and the other added
   combinations) are pressed, **Then** the editor receives them as
   distinct keys and the US1/US4 bindings fire; terminal-backend gaps are
   documented (roadmap #7 owns terminal modifier work).
2. **Given** the reduced piano map, **When** `,` is pressed in F2,
   **Then** the mask editor opens instead of a note being entered, and no
   list/window anywhere still treats the five removed keys as notes.
3. **Given** a focused thumbbar, **When** digits are typed, **Then** the
   value is entered numerically per the original's widget behaviour.

---

### Edge Cases

- Marks that exceed the pattern after a pattern-length change or pattern
  switch — clamped/dropped exactly as the original does.
- Paste near the pattern edge — the original's clipping rules apply (no
  wrap, no overflow into other channels unless the original does so).
- Block operations on rows the player is currently reading — engine-lock
  serialisation; audio thread never sees partial rows.
- Undo scope: only the operations the original snapshots are undoable;
  undo after switching patterns behaves as the original (buffer is
  per-pattern/global exactly as the ASM has it).
- Transpose at note bounds (C-0 / B-9) and volume ops at 0/64 — clamp per
  `PE_TRANS.INC`, including any asymmetric quirks.
- Template stamping with a clipboard wider than the remaining channels —
  original clipping behaviour.
- View schemes on 64-channel patterns and at the last visible channel —
  original scrolling/limits.
- Keyjazz reduction must not break existing selftest note-entry scripts
  (they use Z/Q-row keys only — verify).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The pattern editor MUST support the original's block
  marking: Shift+arrow extension, Alt-B/E begin/end, Alt-D step-marking,
  Alt-L track/pattern widening, Alt-U unmark, with the original's
  highlight rendering and bounds rules.
- **FR-002**: All block operations from the original F2 key list MUST be
  ported with `PE_TRANS.INC` semantics: copy, paste, overwrite, mix, wipe
  block, swap, double/halve, block volume, volume amplify, wipe excess
  volumes, wipe commands, and semitone transpose up/down.
- **FR-003**: The edit mask (`,`), multichannel entry toggle and template
  mode MUST control note entry exactly as the original's handlers do.
- **FR-004**: Row/field verbs MUST match the original: track-local
  Ins/Del, all-channel Alt-Ins/Del, pattern roll Ctrl-Ins/Del, Backspace
  step-back, and Ctrl-Backspace single-step undo covering the same
  operation set the original snapshots.
- **FR-005**: The navigation keys and F2 conveniences listed in the scope
  (US4) MUST be ported with the original's handler semantics, including
  the view schemes and pattern store/restore.
- **FR-006**: The key layer MUST deliver the modifier combinations the
  original table requires on the pixel backends; combinations impossible
  on the terminal backend are documented leftovers (roadmap #7). The
  keyjazz map MUST be reduced to the original `KeyBoardTable`.
- **FR-007**: Thumbbars MUST accept typed digits for direct numeric entry
  per the original widget behaviour.
- **FR-008**: Every pattern mutation MUST be serialised via the engine
  lock and MUST round-trip through the exact pack/unpack codec; the
  explicit cell-mask model stays authoritative.
- **FR-009**: The audio-determinism regression MUST remain `IDENTICAL`
  for all four testdata modules, and the selftest MUST gain coverage for
  the marking/block/undo/mask paths (headless, scripted keys).
- **FR-010**: Out of scope: MIDI input triggers (`MIDIInputToggle`,
  `PE_MIDINote/NoteOff/Aftertouch`) until MIDI-in exists; the `<` `>` `;`
  `'` instrument cycling (already ported); terminal-backend modifier
  delivery (roadmap #7).

### Key Entities

- **Mark (block selection)**: the selected cell region — start/end
  row+channel, with the original's growth/clamp rules; drawn with the
  original attributes.
- **Pattern clipboard**: the copied block — cell grid plus dimensions;
  persists across pattern switches as the original's does.
- **Edit mask**: the per-field write-enable set consulted by note entry.
- **Undo buffer**: the original's single-step pattern snapshot and the
  operation set that fills it.
- **Pattern scratch**: the store/restore snapshot (Alt-0 family),
  distinct from the undo buffer.
- **View scheme**: the channel-layout presets the Ctrl-0..5 /
  Ctrl-Shift-1..4 keys select.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Every key in the original F2 table (except the documented
  out-of-scope MIDI and terminal-modifier items) performs its original
  function in the port — verified key-by-key against the ASM handler
  list; zero unbound in-scope keys remain.
- **SC-002**: For a scripted battery of block operations on the testdata
  patterns, the resulting cell contents are 100% identical to the
  original's documented `PE_TRANS.INC` semantics, and re-rendered audio
  of the edited module is deterministic across two renders.
- **SC-003**: Ctrl-Backspace restores the exact pre-operation pattern for
  every operation the original protects — byte-identical after
  pack/unpack.
- **SC-004**: The audio-determinism regression stays `IDENTICAL` on all
  four testdata modules; the full selftest (including the new
  editing-depth block) passes headlessly on Win32 and Linux/SDL.
- **SC-005**: The five removed keyjazz keys no longer enter notes in any
  screen, and `,` opens the mask editor in F2 in 100% of presses.

## Assumptions

- The ASM is the specification: where this document says "as the
  original", the `PEFunction_*` handler and `PE_TRANS.INC` code paths
  (including quirks) define correct behaviour; the plan phase's
  research.md will record the decoded semantics before implementation,
  as specs 002..008 did.
- Undo is the original's 10-slot typed snapshot history with the
  Ctrl-Backspace picker requester (research.md R8) — deeper than the
  draft assumed, still 1:1 with the original, not a modern unlimited
  history.
- Where this spec says "PE_TRANS.INC semantics", the authoritative
  source is the `PEFunction_*` handler bodies in IT_PE.ASM —
  PE_TRANS.INC itself holds the format converters already ported in
  feature 007 (research.md R0). The `,` mask key toggles the mask bit
  for the cursor's field (no dialog).
- The clipboard, mask, template and scratch state are session-local (not
  persisted to `ited.cfg`) unless the original persists them in `IT.CFG`;
  the research phase records which, and the port follows.
- Ctrl-arrow (and similar) key codes are additions to the shared `ITK_*`
  enum behind the backend vtable; the terminal backend's inability to
  report some combinations stays a documented roadmap-#7 leftover, per
  the feature description.
- The `<` `>` `;` `'` instrument-cycle keys landed ahead of this spec
  (commit 96a607d) and are excluded; this spec's keyjazz reduction must
  not regress them.
- Mouse-based marking (click-drag over the grid, as IT's 8010h pixel
  events support) follows the same mark model; if the original's mouse
  marking has separate quirks, research records them and the port
  matches.
