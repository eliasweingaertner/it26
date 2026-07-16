# Feature Specification: In-Depth Sample & Instrument Editors

**Feature Branch**: `005-sample-instrument-editors`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "In-depth sample & instrument editors (IT_I.ASM): envelopes, sample draw/loop/zoom ops. Roadmap item #6 in docs/HANDOFF.md §6."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Sample waveform view with loop & zoom (Priority: P1)

A user selects a sample (from F3) and opens the sample editor to see its waveform
drawn the way IT does, set/move the loop and sustain-loop points directly on the
waveform, and zoom/scroll into the data to work precisely. Today the editor lists
sample parameters but has no waveform editing surface.

**Why this priority**: Seeing and navigating the waveform with loop markers is the
foundation of sample editing; every destructive op (P2) needs this view first. It
is independently valuable as a non-destructive inspection/loop-setting tool.

**Independent Test**: Open a sample, confirm its waveform renders, zoom in and
scroll, drag the loop start/end (and sustain loop) on the waveform, audition the
sample, and confirm the loop is audibly applied through the real engine.

**Acceptance Scenarios**:

1. **Given** a non-empty sample, **When** the user opens the sample editor,
   **Then** the waveform renders with current loop / sustain-loop markers.
2. **Given** the waveform view, **When** the user zooms and scrolls, **Then** the
   display scales to the selected range without altering the sample.
3. **Given** loop markers, **When** the user moves loop / sustain-loop start and
   end, **Then** the sample's loop fields update and auditioning reflects them.

---

### User Story 2 - Destructive sample operations (Priority: P2)

A user performs IT's sample-processing operations — draw on the waveform,
cut/copy/paste a selection, and the standard transforms (e.g. amplify/normalize,
reverse, fade, convert) — to edit the actual sample data.

**Why this priority**: These are the real editing power of the sample editor, but
they depend on the view + selection model from P1. Each operation is
independently testable and can be added incrementally.

**Independent Test**: Select a range, apply each operation (draw, cut, reverse,
amplify, etc.), and confirm the waveform and the underlying sample data change as
expected and the result auditions correctly; confirm the engine handles the
edited sample (length/loop bounds stay valid).

**Acceptance Scenarios**:

1. **Given** the waveform editor, **When** the user draws on the waveform, **Then**
   the sample data under the cursor changes and redraws.
2. **Given** a selected range, **When** the user applies a transform (reverse,
   amplify, fade, etc.) or cut/copy/paste, **Then** the sample data changes
   accordingly and loop bounds are kept valid.
3. **Given** an edited sample, **When** auditioned via the piano keys, **Then**
   playback uses the edited data through the real engine.

---

### User Story 3 - Deep instrument envelope editing (Priority: P3)

A user does the in-depth instrument-side editing that goes beyond the F4 page's
basic tabs — full graphical envelope authoring (volume/panning/pitch) with all
node, loop, sustain and carry controls — as a cohesive editor.

**Why this priority**: This complements the F4 instrument-editor feature (#002)
with the deepest envelope work; it is lowest priority because #002 already
delivers operable tabs and basic node editing, and sample-side editing (P1/P2) is
the larger gap this feature exists to fill.

**Independent Test**: Author a volume/panning/pitch envelope end-to-end (add/move/
delete nodes, set loop & sustain, carry), audition, and confirm the engine plays
the authored envelopes.

**Acceptance Scenarios**:

1. **Given** an instrument, **When** the user authors each envelope graphically,
   **Then** the envelope data updates and renders.
2. **Given** authored envelopes, **When** the instrument is auditioned, **Then**
   the real engine reflects them.
3. **Given** overlap with the F4 tabs (#002), **When** edits are made in either
   place, **Then** they operate on the same instrument data (no divergence).

---

### Edge Cases

- Empty sample slot — editor shows an empty surface; drawing/recording into it
  initialises the sample correctly.
- 8-bit vs 16-bit and mono vs stereo samples — rendered and edited correctly at
  the right bit depth/channel count.
- Loop/sustain markers crossing or inverted — constrained to valid orderings.
- Operations that change sample length (cut/trim) — loop points and any
  dependent state stay consistent; engine bounds (interpolator padding) preserved.
- Very long samples — zoom/scroll and operations stay responsive.
- Undo of destructive ops — if provided, restores prior data exactly; if not
  provided in scope, that limitation is stated.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The sample editor MUST render a sample's waveform with current
  loop and sustain-loop markers, per IT's sample display.
- **FR-002**: Users MUST be able to zoom and scroll the waveform non-destructively
  and set/move loop and sustain-loop points directly on it.
- **FR-003**: Users MUST be able to perform destructive sample operations —
  draw, cut/copy/paste a selection, and standard transforms (e.g. amplify/
  normalize, reverse, fade, format/bit-depth convert) — on the real sample data.
- **FR-004**: Sample-length-changing operations MUST keep loop points and engine
  bounds (including interpolator padding) valid.
- **FR-005**: The instrument editor MUST support full graphical envelope authoring
  (volume/panning/pitch: nodes, loop, sustain, carry), operating on the same
  instrument data as the F4 page (#002), with no divergent copy.
- **FR-006**: All edits MUST act on the real engine sample/instrument structures
  so auditioning reflects them immediately.
- **FR-007**: 8/16-bit and mono/stereo samples MUST be displayed and edited
  correctly.
- **FR-008**: Editing MUST serialise against the audio thread (engine lock) when
  it mutates data the mixer may be reading.
- **FR-009**: The audio-determinism regression (`tests/test_pattern.c`) MUST
  remain `IDENTICAL` for all four testdata modules (unedited modules render
  unchanged).
- **FR-010**: Layout/behaviour MUST be sourced from IT_I.ASM (per constitution
  Principle II); any approximation MUST be flagged in the handoff docs.

### Key Entities

- **Sample**: Audio data with bit depth, channel count, length, loop and
  sustain-loop ranges, and the interpolator-padding the engine requires.
- **Selection**: A range within a sample targeted by destructive operations.
- **Instrument envelope**: Volume/panning/pitch node sets with loop/sustain/carry
  (shared with the F4 instrument-editor feature).
- **Sample operation**: A draw or transform (cut/copy/paste, reverse, amplify,
  fade, convert) applied to sample data.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: A user can view any sample's waveform, set its loops on-screen, and
  audition the loop audibly through the engine.
- **SC-002**: Each supported destructive operation produces the expected, audible
  change and leaves the sample in an engine-valid state (correct loops/bounds).
- **SC-003**: Instrument envelopes can be authored graphically end-to-end and
  audibly take effect on audition, sharing data with the F4 tabs.
- **SC-004**: Editing handles 8/16-bit and mono/stereo samples without corruption.
- **SC-005**: Audio-determinism regression remains `IDENTICAL` for all four
  testdata modules.

## Assumptions

- Layout and operation behaviour come from `IT_I.ASM` (and related IT sample/
  instrument code), not from eyeballing.
- This feature owns the deep sample-side editing; the F4 instrument-page layout
  and basic tabs are delivered by feature #002, and the two share the same
  underlying instrument/envelope data.
- Saving edited samples/instruments to disk depends on the Save module feature
  (#004); within this feature edits persist for the session.
- Undo/redo scope will be decided during planning; if not included it will be
  documented as a known limitation.
- Mouse-driven waveform drawing/selection assumes a pixel backend (Win32 today,
  SDL on POSIX via feature #001); keyboard-driven parameter edits work on all
  backends.
