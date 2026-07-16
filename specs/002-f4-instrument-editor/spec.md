# Feature Specification: F4 Instrument Editor — Object-Exact Pane & Tabs

**Feature Branch**: `002-f4-instrument-editor`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "Object-exact F4 right pane + the Volume/Panning/Pitch tabs. The current F4 right-hand layout is eyeballed from reference screenshots and the Volume/Panning/Pitch tabs are stubs; the NNA/DCT/DCA buttons and the instrument list already work. Roadmap item #2 in docs/HANDOFF.md §6."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Authentic F4 instrument page layout (Priority: P1)

A user opens the instrument list (F4) and sees the full instrument page exactly
as Impulse Tracker draws it — every box, label, value field and widget on the
right-hand pane placed at the original coordinates with the original colors and
box styles, not approximated from screenshots. This removes the last known
"eyeballed" deviation in the editor (currently flagged in HANDOFF §2).

**Why this priority**: The right pane is the visible heart of the instrument
page; getting its layout authentic is the foundational deliverable and the one
that closes the standing fidelity gap. The interactive tabs (P2/P3) build on top
of a correctly laid-out pane.

**Independent Test**: Capture the F4 screen to a pixel-exact BMP (`ITED_SHOT`)
and compare against the reference IT 2.14 screenshot in `../screenshots/`; the
right pane's boxes, labels, value positions and colors match (near-pixel), with
no eyeballed offsets.

**Acceptance Scenarios**:

1. **Given** a loaded module, **When** the user presses F4, **Then** the
   instrument page renders with the right pane's geometry, box styles, labels and
   colors taken from the original ASM object data (not screenshot estimates).
2. **Given** the F4 page is shown, **When** compared side-by-side with the
   reference screenshot, **Then** the right pane matches to near-pixel accuracy.
3. **Given** the existing left-pane instrument list and NNA/DCT/DCA controls,
   **When** F4 is opened, **Then** they continue to work exactly as before.

---

### User Story 2 - Working Volume / Panning / Pitch envelope tabs (Priority: P2)

A user switches between the Volume, Panning and Pitch tabs on the instrument
page and sees each tab's full set of controls (envelope on/off, loop and sustain
loop points, carry, and the tab-specific options such as panning center / pitch
filter mode) laid out and operable as in IT, instead of the current stubs.

**Why this priority**: The tabs are the functional purpose of the instrument
page. They depend on the authentic pane (P1) existing. Delivered, they make the
instrument page genuinely editable rather than display-only.

**Independent Test**: On each of the three tabs, change every control with
keyboard and mouse and confirm the underlying instrument data updates and the
value is reflected on screen; reopen F4 and confirm the values persist for the
session.

**Acceptance Scenarios**:

1. **Given** the F4 page, **When** the user selects the Volume, Panning, or Pitch
   tab, **Then** that tab's controls render in the original layout.
2. **Given** a tab's controls, **When** the user toggles envelope on/off, sets
   loop / sustain-loop start and end, and toggles carry, **Then** the instrument
   data updates and is shown.
3. **Given** the Panning tab, **When** the user edits default pan / pan center,
   and on the Pitch tab the pitch-envelope-as-filter option, **Then** those
   tab-specific settings update correctly.

---

### User Story 3 - Interactive envelope graph editing (Priority: P3)

A user edits an instrument's envelope graphically on the active tab — adding,
moving and removing envelope nodes and setting loop/sustain markers directly on
the envelope display — the way IT's envelope editor works.

**Why this priority**: Graphical node editing is the richest part of the tabs and
the most involved; the tabs are still useful with numeric controls alone (P2), so
node editing is a separable enhancement. (Deeper envelope/waveform editing
overlaps roadmap #6 and is bounded out where it does.)

**Independent Test**: On the Volume tab, add a node, drag it, delete it, and set a
loop across nodes; confirm the envelope shape and loop markers update on screen
and in the instrument data.

**Acceptance Scenarios**:

1. **Given** an envelope display, **When** the user adds/moves/deletes a node,
   **Then** the envelope graph and the instrument's node data update accordingly.
2. **Given** an envelope with nodes, **When** the user sets loop / sustain-loop
   start and end nodes, **Then** the markers render and the data updates.
3. **Given** node edits, **When** the instrument is auditioned via the piano
   keys, **Then** playback reflects the edited envelope through the real engine.

---

### Edge Cases

- Empty / unused instrument slot selected on F4 — controls show sensible defaults
  and editing initialises the instrument correctly.
- Envelope with the maximum number of nodes — adding beyond the limit is
  prevented, matching IT's bounds.
- Loop/sustain markers set to invalid orderings (end before start) — constrained
  to valid values as IT does.
- Switching tabs mid-edit — in-progress edits are committed/handled without
  corrupting instrument data.
- Old-format / converted instruments — display and edit without crashing even if
  some fields were best-effort converted.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The F4 instrument-page right pane MUST render with geometry, box
  styles, labels, value positions and colors sourced from the original IT ASM
  object data, replacing the current screenshot-eyeballed layout.
- **FR-002**: The Volume, Panning and Pitch tabs MUST each render their full
  original control set and be selectable, replacing the current stub tabs.
- **FR-003**: Users MUST be able to edit every control on each tab via keyboard
  and mouse, with the same focus/activation model used elsewhere in the editor.
- **FR-004**: Edits MUST update the real instrument data structures used by the
  engine (no shadow/duplicate model), so auditioned playback reflects them.
- **FR-005**: The existing instrument list and NNA/DCT/DCA controls MUST continue
  to function unchanged.
- **FR-006**: Envelope editing MUST respect IT's bounds (node count, value
  ranges, loop/sustain ordering).
- **FR-007**: The change MUST be presentation/editor-only; the audio-determinism
  regression (`tests/test_pattern.c`) MUST remain `IDENTICAL` for all four
  testdata modules.
- **FR-008**: Layout/color data MUST NOT be hand-eyeballed; any unavoidable
  approximation MUST be explicitly flagged in the handoff docs (per constitution
  Principle II).

### Key Entities

- **Instrument**: An IT instrument with NNA/DCT/DCA settings, default
  volume/pan/pitch, and three envelopes (volume, panning, pitch), each with
  nodes and loop/sustain markers.
- **Envelope**: An ordered set of (tick, value) nodes with optional loop and
  sustain-loop ranges and a carry flag; volume/panning/pitch variants differ in
  value range and tab-specific options.
- **F4 page widgets**: The right-pane objects (labels, value fields, toggles,
  tab selector, envelope display) defined by the original object data.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The F4 right pane matches the reference IT screenshot to near-pixel
  accuracy with zero remaining eyeballed offsets (the HANDOFF §2 exception is
  closed).
- **SC-002**: All three tabs are fully operable — 100% of their controls can be
  changed and the change is reflected on screen and in instrument data.
- **SC-003**: An edited instrument auditions correctly through the real engine
  (the envelope/NNA settings audibly take effect).
- **SC-004**: Audio-determinism regression remains `IDENTICAL` for all four
  testdata modules.

## Assumptions

- Layout and colors come from the original IT_I.ASM / IT_OBJ1.ASM object and box
  data (the same sourcing approach used for the other screens), not screenshots.
- The instrument data structures already exist in the engine; this feature edits
  them in place and adds no new persisted format.
- Saving edited instruments to disk is out of scope here (covered by the Save
  module feature); edits persist for the session.
- The deepest sample/waveform editing is covered by the Sample/Instrument
  editors feature; this feature focuses on the instrument page and its envelope
  tabs.
- Mouse interaction assumes a pixel backend (Win32 today, SDL on POSIX once
  available); keyboard editing works on all backends.
