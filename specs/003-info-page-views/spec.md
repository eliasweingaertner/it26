# Feature Specification: Info Page (F5) Follow-ups — Oscilloscope & View Methods

**Feature Branch**: `003-info-page-views`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "Info page (F5) follow-ups: the real oscilloscope, the other view methods (note dots, sample dots, 5/8/.../64-channel pattern views, Display_Variables), split view windows, and per-channel solo. The first-pass track view (Display_HostChannel) already landed. Roadmap item #3 in docs/HANDOFF.md §6."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Real per-channel oscilloscope (Priority: P1)

A user playing a module on the info page (F5) sees a live oscilloscope for each
active channel — the actual waveform being mixed — where the first-pass track
view currently draws a VU bar. This is IT's signature live visualisation.

**Why this priority**: The oscilloscope is the most recognisable and requested
element of IT's info page and the most visible gap versus the original. It
delivers immediate standalone value on top of the existing track view.

**Independent Test**: Play a module, open F5 in the track view, and confirm each
active channel shows a live oscilloscope trace that moves with the audio and goes
flat when the channel is silent — visually comparable to IT and the reference
screenshots.

**Acceptance Scenarios**:

1. **Given** a playing module, **When** the user views F5, **Then** each active
   host channel shows a live oscilloscope of its current output.
2. **Given** a channel goes silent, **When** viewed on F5, **Then** its
   oscilloscope flattens.
3. **Given** the audio thread is mixing, **When** the info page reads channel
   state, **Then** it does so safely under the engine lock without disturbing
   playback or determinism.

---

### User Story 2 - The other Display view methods (Priority: P2)

A user cycles through the info page's view methods as in IT: note dots, sample
dots, the multi-channel pattern views (5 / 8 / ... / 64 channel), and the
Variables view — choosing the density and information that suits the module.

**Why this priority**: Multiple view methods are core info-page functionality and
the next-most-valuable layer after the oscilloscope. Each view is independently
demonstrable. They depend only on the live channel data the track view already
reads.

**Independent Test**: While playing, switch among each view method and confirm
each renders its distinct layout (note dots / sample dots / N-channel pattern
grid / variables) driven by live channel state, matching IT's
`IT_DISPL.ASM` methods.

**Acceptance Scenarios**:

1. **Given** F5 is shown, **When** the user switches the view method, **Then**
   the page re-renders in the selected method (note dots, sample dots,
   5/8/16/.../64-channel pattern view, or Variables).
2. **Given** a multi-channel pattern view, **When** the module plays, **Then**
   the per-channel activity updates live at the correct density.
3. **Given** the Variables view, **When** displayed, **Then** the global play
   variables render per `Display_Variables`.

---

### User Story 3 - Split views and per-channel solo (Priority: P3)

A user splits the info page into multiple view windows (e.g., an oscilloscope
pane plus a pattern-density pane) and solos an individual channel to isolate it.

**Why this priority**: Split windows and solo are power-user refinements layered
on top of the views (P2) and oscilloscope (P1); the page is fully useful without
them, so they are the lowest priority of this feature.

**Independent Test**: Split the info page into two view windows with different
methods and confirm both update live; solo a channel and confirm only it is
audible and the display indicates the solo state.

**Acceptance Scenarios**:

1. **Given** F5, **When** the user splits the view, **Then** two view windows
   render independently with their own methods and both update live.
2. **Given** several active channels, **When** the user solos one, **Then** only
   that channel is audible (others effectively muted) and the display reflects
   the solo.
3. **Given** a soloed channel, **When** the user clears solo, **Then** normal
   mute states are restored.

---

### Edge Cases

- More active channels than fit on screen — scrolling (already present for the
  track view) extends to the new views; the N-channel views select the right
  density.
- Stopped playback — info page shows a static/empty state without errors (live
  views read no signal).
- Channels muted vs. silent — colours/indicators distinguish the two as IT does
  (per `GetChannelColour`).
- Solo interacting with pre-existing manual mutes — solo/unsolo restores the
  prior mute state correctly.
- Window/terminal too small for a chosen density — degrade gracefully.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The info page MUST render a live per-channel oscilloscope of actual
  channel output in the track view, where the first pass draws a VU bar.
- **FR-002**: The info page MUST provide the additional view methods from
  `IT_DISPL.ASM`: note dots, sample dots, the multi-channel pattern views
  (5/8/.../64), and the Variables view, switchable by the user.
- **FR-003**: Users MUST be able to split the info page into multiple view
  windows, each with its own view method, all updating live.
- **FR-004**: Users MUST be able to solo an individual channel and clear the
  solo, restoring prior mute state.
- **FR-005**: All live views MUST read channel/engine state under the existing
  engine lock and MUST NOT alter audio output or break determinism.
- **FR-006**: Channel colouring and muted/active indicators MUST follow the
  original (`GetChannelColour`) conventions, consistent with the existing track
  view.
- **FR-007**: Scrolling and the existing first-pass track view MUST continue to
  work; new views integrate with the same navigation.
- **FR-008**: The audio-determinism regression (`tests/test_pattern.c`) MUST
  remain `IDENTICAL` for all four testdata modules.

### Key Entities

- **View method**: A way of presenting live channel state (track/oscilloscope,
  note dots, sample dots, N-channel pattern, Variables), corresponding to the
  original `Display_*` routines.
- **View window**: A region of the info page rendering one view method; the page
  may host one or several (split).
- **Channel state**: Live host/slave channel data (sample/instrument, volume,
  pan, on/off, output samples) read under the engine lock.
- **Solo state**: Which channel, if any, is isolated, plus the saved mute state
  to restore on unsolo.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: During playback, every active channel shows a live oscilloscope
  that visibly tracks its audio and flattens when silent.
- **SC-002**: All listed view methods are selectable and render their distinct,
  correct layout driven by live data, matching IT/reference screenshots.
- **SC-003**: The info page can be split into at least two independently-updating
  view windows.
- **SC-004**: Soloing a channel makes only that channel audible and is reversible
  to the exact prior mute state.
- **SC-005**: Audio-determinism regression remains `IDENTICAL` for all four
  testdata modules, confirming the views are read-only on the engine.

## Assumptions

- Layout and behaviour come from `IT_DISPL.ASM` (`Display_HostChannel` already
  ported; `Display_NoteDots`/`SampleDots`/N-channel/`Display_Variables` to
  follow), not from eyeballing.
- The oscilloscope reads the same live slave-channel output the mixer produces;
  no new audio path is added.
- Solo is implemented via the engine's existing channel mute mechanism (no new
  engine feature), preserving determinism for offline renders.
- These visualisations are live/interactive and are exercised from a running
  session; `ITED_SHOT` static captures of live views may be empty unless
  playing, consistent with the current track view.
- Mouse interaction (where applicable) assumes a pixel backend; keyboard control
  works on all backends.
